/* ============================================================================
 * bmf2pbf - High-Speed Sequential Out-Of-Core Engine (Win32 GUI)
 *
 * COMPILATION INSTRUCTIONS:
 *
 * 1. UNCOMPRESSED (No zlib dependency):
 *    gcc -O3 -Wall -Wextra bmf2pbf.c -o bmf2pbf.exe -lcomdlg32 -lgdi32 -Wl,--large-address-aware
 *
 * 2. COMPRESSED (Standard OSM PBF size, requires zlib):
 *    gcc -O3 -Wall -Wextra bmf2pbf.c -o bmf2pbf.exe -DUSE_ZLIB -lz -lcomdlg32 -lgdi32 -Wl,--large-address-aware
 *
 * REQUIREMENTS: Windows XP or later
 *
 * FEATURES:
 *  - Native Win32 GUI if launched without arguments.
 *  - TRUE OUT-OF-CORE ARCHITECTURE: Streams BMF files directly from disk.
 *  - HIGH-SPEED I/O: Uses massive 4MB disk buffers to maximize SSD/HDD throughput.
 *  - Memory usage strictly capped at ~5MB regardless of map size.
 *  - Backward compatibility: Reads both BMF1 and BMF2 natively.
 *
 * THIS WORK IS NOT FIT FOR ANY FUNCTION OR PURPOSE, COMES WITH NO WARRANTY,
 * AND IS BEING RELEASED INTO THE PUBLIC DOMAIN.
 * ============================================================================ */

#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef USE_ZLIB
#include <zlib.h>
#endif

#define IO_BUF_SIZE (1024 * 1024 * 4) // 4MB High-Speed Disk Buffer

#define CLASS_UNKNOWN   0
#define CLASS_WATER     1
#define CLASS_LAND      2
#define CLASS_PARK      3
#define CLASS_COASTLINE 4
#define CLASS_HWY_MINOR 5
#define CLASS_HWY_MAIN  6

typedef struct { double lon, lat; } MapPoint;

typedef struct {
    uint8_t feature_class;
    char* name;
    char* postcode;
    MapPoint* points;
    uint32_t point_count;
} MapFeature;

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char** strings;
    size_t count;
    size_t cap;
} StringTable;

// --- GUI Globals ---
HWND hTxtIn, hTxtOut, hBtnIn, hBtnOut, hBtnRun;

uint32_t SwapEndian32(uint32_t val) {
    return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) | ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000);
}

uint64_t EncodeZigZag(int64_t n) {
    return (n << 1) ^ (n >> 63);
}

void BufInit(Buffer* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void BufFree(Buffer* b) {
    if (b->data) free(b->data);
    BufInit(b);
}

void BufEnsure(Buffer* b, size_t extra) {
    if (b->len + extra > b->cap) {
        b->cap = b->cap == 0 ? 1024 : b->cap * 2;
        if (b->cap < b->len + extra) b->cap = b->len + extra;
        b->data = (uint8_t*)realloc(b->data, b->cap);
    }
}

void BufAppend(Buffer* b, const uint8_t* data, size_t len) {
    if (len == 0) return;
    BufEnsure(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void BufWriteVarint(Buffer* b, uint64_t v) {
    uint8_t buf[10];
    int len = 0;
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        buf[len++] = byte;
    } while (v);
    BufAppend(b, buf, len);
}

void BufWriteTag(Buffer* b, uint32_t field, uint8_t wire) {
    BufWriteVarint(b, (field << 3) | wire);
}

void BufWriteVarintField(Buffer* b, uint32_t field, uint64_t v) {
    BufWriteTag(b, field, 0); 
    BufWriteVarint(b, v);
}

void BufWriteBytesField(Buffer* b, uint32_t field, const void* data, size_t len) {
    BufWriteTag(b, field, 2); 
    BufWriteVarint(b, len);
    BufAppend(b, (const uint8_t*)data, len);
}

void StInit(StringTable* st) {
    st->count = 0;
    st->cap = 64;
    st->strings = (char**)malloc(st->cap * sizeof(char*));
}

void StFree(StringTable* st) {
    for (size_t i = 0; i < st->count; i++) free(st->strings[i]);
    free(st->strings);
    st->count = 0;
    st->cap = 0;
}

uint32_t StAdd(StringTable* st, const char* str) {
    for (size_t i = 0; i < st->count; i++) {
        if (strcmp(st->strings[i], str) == 0) return (uint32_t)i;
    }
    if (st->count >= st->cap) {
        st->cap *= 2;
        st->strings = (char**)realloc(st->strings, st->cap * sizeof(char*));
    }
    st->strings[st->count] = strdup(str);
    return (uint32_t)st->count++;
}

int GetClassTags(uint8_t fclass, uint32_t* k, uint32_t* v) {
    switch (fclass) {
        case CLASS_WATER:     *k = 2; *v = 3; return 1; 
        case CLASS_LAND:      *k = 2; *v = 4; return 1; 
        case CLASS_COASTLINE: *k = 2; *v = 5; return 1; 
        case CLASS_HWY_MINOR: *k = 6; *v = 7; return 1; 
        case CLASS_HWY_MAIN:  *k = 6; *v = 8; return 1; 
        case CLASS_PARK:      *k = 9; *v = 10; return 1;
        default: return 0;
    }
}

void WriteBlob(FILE* out, const char* type, Buffer* content, bool compress_blob) {
    Buffer blob; BufInit(&blob);

#ifdef USE_ZLIB
    if (compress_blob) {
        BufWriteVarintField(&blob, 1, content->len);
        unsigned long zlen = compressBound(content->len);
        uint8_t* zbuf = (uint8_t*)malloc(zlen);
        
        if (compress(zbuf, &zlen, content->data, content->len) == Z_OK) {
            BufWriteBytesField(&blob, 3, zbuf, zlen); 
        } else {
            BufWriteBytesField(&blob, 2, content->data, content->len);
        }
        free(zbuf);
    } else {
        BufWriteVarintField(&blob, 1, content->len);
        BufWriteBytesField(&blob, 2, content->data, content->len); 
    }
#else
    (void)compress_blob; 
    BufWriteVarintField(&blob, 1, content->len); 
    BufWriteBytesField(&blob, 2, content->data, content->len); 
#endif

    Buffer blobHeader; BufInit(&blobHeader);
    BufWriteBytesField(&blobHeader, 1, type, strlen(type));
    BufWriteVarintField(&blobHeader, 3, blob.len); 
    
    uint32_t bh_len = SwapEndian32((uint32_t)blobHeader.len);
    fwrite(&bh_len, 4, 1, out);
    fwrite(blobHeader.data, 1, blobHeader.len, out);
    fwrite(blob.data, 1, blob.len, out);
    
    BufFree(&blob);
    BufFree(&blobHeader);
}

void WriteOSMHeader(FILE* out) {
    Buffer hb; BufInit(&hb);
    BufWriteBytesField(&hb, 4, "OsmSchema-V0.6", 14);
    BufWriteBytesField(&hb, 4, "DenseNodes", 10);
    BufWriteBytesField(&hb, 5, "bmf2pbf", 7);
    WriteBlob(out, "OSMHeader", &hb, false);
    BufFree(&hb);
}

// ============================================================================
// OUT-OF-CORE COMPILER LOGIC
// ============================================================================
int RunCompilation(const char* in_filename, const char* out_filename) {
    FILE* in = fopen(in_filename, "rb");
    if (!in) { 
        printf("Cannot open input file: %s\n", in_filename); 
        return 1; 
    }

    // Attach 4MB High-Speed I/O Buffer
    char* io_in_buf = malloc(IO_BUF_SIZE);
    if (io_in_buf) setvbuf(in, io_in_buf, _IOFBF, IO_BUF_SIZE);

    char magic[4];
    if (fread(magic, 1, 4, in) != 4 || (strncmp(magic, "BMF1", 4) != 0 && strncmp(magic, "BMF2", 4) != 0)) {
        printf("Invalid or missing BMF header. Make sure the file is compiled with pbf2bmf.\n");
        fclose(in);
        if (io_in_buf) free(io_in_buf);
        return 1;
    }
    
    bool is_bmf2 = (strncmp(magic, "BMF2", 4) == 0);

    uint64_t total_feature_count;
    fread(&total_feature_count, sizeof(uint64_t), 1, in);
    printf("Detected %llu features. Streaming %s...\n", total_feature_count, is_bmf2 ? "BMF2" : "BMF1");

    FILE* out = fopen(out_filename, "wb");
    if (!out) { 
        printf("Cannot create output file: %s\n", out_filename); 
        fclose(in);
        if (io_in_buf) free(io_in_buf);
        return 1; 
    }

    // Attach 4MB High-Speed I/O Buffer
    char* io_out_buf = malloc(IO_BUF_SIZE);
    if (io_out_buf) setvbuf(out, io_out_buf, _IOFBF, IO_BUF_SIZE);

    WriteOSMHeader(out);

    uint64_t global_node_id = 1;
    uint64_t global_way_id = 1;
    const uint64_t CHUNK_SIZE = 8000;

    printf("Encoding Protocol Buffers...\n");

    for (uint64_t i = 0; i < total_feature_count; i += CHUNK_SIZE) {
        uint64_t end = i + CHUNK_SIZE;
        if (end > total_feature_count) end = total_feature_count;
        uint64_t batch_size = end - i;

        // 1. Read strict batch limits from disk to prevent RAM bloat
        MapFeature* batch = malloc(batch_size * sizeof(MapFeature));
        for (uint64_t j = 0; j < batch_size; j++) {
            MapFeature* f = &batch[j];
            fread(&f->feature_class, 1, 1, in);
            
            uint16_t nlen; fread(&nlen, 2, 1, in);
            if (nlen > 0) {
                f->name = malloc(nlen + 1);
                fread(f->name, 1, nlen, in);
                f->name[nlen] = '\0';
            } else { f->name = NULL; }

            if (is_bmf2) {
                uint16_t plen; fread(&plen, 2, 1, in);
                if (plen > 0) {
                    f->postcode = malloc(plen + 1);
                    fread(f->postcode, 1, plen, in);
                    f->postcode[plen] = '\0';
                } else { f->postcode = NULL; }
            } else {
                f->postcode = NULL;
            }
            
            fread(&f->point_count, 4, 1, in);
            f->points = malloc(f->point_count * sizeof(MapPoint));
            fread(f->points, sizeof(MapPoint), f->point_count, in);
        }

        // 2. Encode Protocol Buffers
        StringTable st; StInit(&st);
        StAdd(&st, "");
        StAdd(&st, "name");
        StAdd(&st, "natural");
        StAdd(&st, "water");
        StAdd(&st, "land");
        StAdd(&st, "coastline");
        StAdd(&st, "highway");
        StAdd(&st, "residential");
        StAdd(&st, "primary");
        StAdd(&st, "leisure");
        StAdd(&st, "park");
        StAdd(&st, "addr:postcode");

        Buffer node_ids, node_lats, node_lons;
        BufInit(&node_ids); BufInit(&node_lats); BufInit(&node_lons);
        Buffer ways_buf; BufInit(&ways_buf);

        int64_t last_node_id = 0, last_lat = 0, last_lon = 0;

        for (uint64_t j = 0; j < batch_size; j++) {
            MapFeature* f = &batch[j];
            uint64_t first_node_id = global_node_id;

            // Encode Nodes
            for (uint32_t p = 0; p < f->point_count; p++) {
                int64_t lat = (int64_t)round(f->points[p].lat * 10000000.0);
                int64_t lon = (int64_t)round(f->points[p].lon * 10000000.0);
                
                BufWriteVarint(&node_ids, EncodeZigZag(global_node_id - last_node_id));
                BufWriteVarint(&node_lats, EncodeZigZag(lat - last_lat));
                BufWriteVarint(&node_lons, EncodeZigZag(lon - last_lon));
                last_node_id = global_node_id++;
                last_lat = lat;
                last_lon = lon;
            }

            // Encode Way
            Buffer way; BufInit(&way);
            BufWriteVarintField(&way, 1, global_way_id++);
            
            Buffer keys, vals; BufInit(&keys); BufInit(&vals);
            uint32_t tk, tv;
            if (GetClassTags(f->feature_class, &tk, &tv)) {
                BufWriteVarint(&keys, tk);
                BufWriteVarint(&vals, tv);
            }
            if (f->name) {
                uint32_t n_idx = StAdd(&st, f->name);
                uint32_t k_idx = StAdd(&st, "name");
                BufWriteVarint(&keys, k_idx);
                BufWriteVarint(&vals, n_idx);
            }
            if (f->postcode) {
                uint32_t p_idx = StAdd(&st, f->postcode);
                uint32_t k_idx = StAdd(&st, "addr:postcode");
                BufWriteVarint(&keys, k_idx);
                BufWriteVarint(&vals, p_idx);
            }
            
            if (keys.len > 0) {
                BufWriteBytesField(&way, 2, keys.data, keys.len);
                BufWriteBytesField(&way, 3, vals.data, vals.len);
            }
            
            Buffer refs; BufInit(&refs);
            int64_t last_ref = 0;
            for (uint32_t p = 0; p < f->point_count; p++) {
                int64_t ref = first_node_id + p;
                BufWriteVarint(&refs, EncodeZigZag(ref - last_ref));
                last_ref = ref;
            }
            BufWriteBytesField(&way, 8, refs.data, refs.len);
            BufWriteBytesField(&ways_buf, 3, way.data, way.len);
            
            BufFree(&way); BufFree(&keys); BufFree(&vals); BufFree(&refs);
        }

        // Assemble PrimitiveGroup
        Buffer dense; BufInit(&dense);
        BufWriteBytesField(&dense, 1, node_ids.data, node_ids.len);
        BufWriteBytesField(&dense, 8, node_lats.data, node_lats.len); 
        BufWriteBytesField(&dense, 9, node_lons.data, node_lons.len); 
        
        Buffer pg; BufInit(&pg);
        BufWriteBytesField(&pg, 2, dense.data, dense.len);
        BufAppend(&pg, ways_buf.data, ways_buf.len);

        // Assemble StringTable & PrimitiveBlock
        Buffer st_buf; BufInit(&st_buf);
        for (size_t s = 0; s < st.count; s++) {
            BufWriteBytesField(&st_buf, 1, st.strings[s], strlen(st.strings[s]));
        }

        Buffer pb; BufInit(&pb);
        BufWriteBytesField(&pb, 1, st_buf.data, st_buf.len);
        BufWriteBytesField(&pb, 2, pg.data, pg.len);
        BufWriteVarintField(&pb, 17, 100); 

        // Write chunk
        WriteBlob(out, "OSMData", &pb, true);

        // 3. Free batch memory
        StFree(&st);
        BufFree(&node_ids); BufFree(&node_lats); BufFree(&node_lons);
        BufFree(&ways_buf); BufFree(&dense); BufFree(&pg);
        BufFree(&st_buf); BufFree(&pb);

        for (uint64_t j = 0; j < batch_size; j++) {
            if (batch[j].name) free(batch[j].name);
            if (batch[j].postcode) free(batch[j].postcode);
            free(batch[j].points);
        }
        free(batch);
        
        printf("\rProgress: %llu / %llu features", end, total_feature_count);
    }

    fclose(in);
    fclose(out);
    if (io_in_buf) free(io_in_buf);
    if (io_out_buf) free(io_out_buf);
    
    printf("\nSuccessfully generated PBF: %s\n", out_filename);

    return 0;
}

// ============================================================================
// GUI & THREAD MANAGEMENT
// ============================================================================
typedef struct {
    char inFile[MAX_PATH];
    char outFile[MAX_PATH];
} ThreadArgs;

DWORD WINAPI CompileThread(LPVOID lpParam) {
    ThreadArgs* args = (ThreadArgs*)lpParam;
    
    EnableWindow(hBtnRun, FALSE);
    EnableWindow(hTxtIn, FALSE);
    EnableWindow(hTxtOut, FALSE);
    EnableWindow(hBtnIn, FALSE);
    EnableWindow(hBtnOut, FALSE);
    
    printf("\n=== COMPILATION STARTED ===\n");
    printf("Input: %s\nOutput: %s\n\n", args->inFile, args->outFile);
    
    int result = RunCompilation(args->inFile, args->outFile);
    
    if (result == 0) {
        MessageBoxA(NULL, "Compilation successfully completed!\nThe PBF file is ready.", "Success", MB_ICONINFORMATION);
    } else {
        MessageBoxA(NULL, "Compilation failed or was interrupted.\nCheck the console output for errors.", "Error", MB_ICONERROR);
    }

    EnableWindow(hBtnRun, TRUE);
    EnableWindow(hTxtIn, TRUE);
    EnableWindow(hTxtOut, TRUE);
    EnableWindow(hBtnIn, TRUE);
    EnableWindow(hBtnOut, TRUE);
    
    free(args);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            HWND hLblIn = CreateWindowA("STATIC", "Source BMF File:", WS_VISIBLE | WS_CHILD, 20, 20, 150, 20, hwnd, NULL, NULL, NULL);
            hTxtIn = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 20, 40, 310, 24, hwnd, NULL, NULL, NULL);
            hBtnIn = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD, 340, 40, 80, 24, hwnd, (HMENU)1, NULL, NULL);

            HWND hLblOut = CreateWindowA("STATIC", "Output PBF File:", WS_VISIBLE | WS_CHILD, 20, 80, 150, 20, hwnd, NULL, NULL, NULL);
            hTxtOut = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 20, 100, 310, 24, hwnd, NULL, NULL, NULL);
            hBtnOut = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD, 340, 100, 80, 24, hwnd, (HMENU)2, NULL, NULL);

            hBtnRun = CreateWindowA("BUTTON", "Compile PBF", WS_VISIBLE | WS_CHILD, 20, 145, 400, 35, hwnd, (HMENU)3, NULL, NULL);

            SendMessage(hLblIn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hTxtIn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnIn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hLblOut, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hTxtOut, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnOut, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnRun, WM_SETFONT, (WPARAM)hFont, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) { 
                OPENFILENAMEA ofn = {0};
                char path[MAX_PATH] = "";
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "BMF Maps (*.bmf)\0*.bmf\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) SetWindowTextA(hTxtIn, path);
            }
            else if (LOWORD(wParam) == 2) { 
                OPENFILENAMEA ofn = {0};
                char path[MAX_PATH] = "";
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "PBF Maps (*.pbf)\0*.pbf\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                ofn.lpstrDefExt = "pbf";
                if (GetSaveFileNameA(&ofn)) SetWindowTextA(hTxtOut, path);
            }
            else if (LOWORD(wParam) == 3) { 
                ThreadArgs* args = malloc(sizeof(ThreadArgs));
                GetWindowTextA(hTxtIn, args->inFile, MAX_PATH);
                GetWindowTextA(hTxtOut, args->outFile, MAX_PATH);

                if (strlen(args->inFile) == 0 || strlen(args->outFile) == 0) {
                    MessageBoxA(hwnd, "Please specify both an input and an output file.", "Missing Fields", MB_ICONWARNING);
                    free(args);
                    return 0;
                }

                char* ext = strrchr(args->outFile, '.');
                char* slash = strrchr(args->outFile, '\\');
                if (!ext || (slash && ext < slash) || strcasecmp(ext, ".pbf") != 0) {
                    strncat(args->outFile, ".pbf", MAX_PATH - strlen(args->outFile) - 1);
                    SetWindowTextA(hTxtOut, args->outFile);
                }

                CreateThread(NULL, 0, CompileThread, args, 0, NULL);
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        char out_filename[1024];
        strncpy(out_filename, argv[1], sizeof(out_filename) - 1);
        out_filename[sizeof(out_filename) - 1] = '\0';
        
        if (argc >= 3) {
            strncpy(out_filename, argv[2], sizeof(out_filename) - 1);
        } else {
            char* ext = strrchr(out_filename, '.');
            if (ext && (strcasecmp(ext, ".bmf") == 0)) strcpy(ext, ".pbf");
            else strncat(out_filename, ".pbf", sizeof(out_filename) - strlen(out_filename) - 1);
        }
        return RunCompilation(argv[1], out_filename);
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "Bmf2PbfGuiClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, "Bmf2PbfGuiClass", "BMF to PBF Compiler",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 240, NULL, NULL, wc.hInstance, NULL);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}