/* ============================================================================
 * pbf2bmf - High-Speed Out-Of-Core Compiler with Feature Filtering & Zlib
 *
 * COMPILATION INSTRUCTIONS:
 *   gcc -O3 -Wall -Wextra pbf2bmf.c -o pbf2bmf.exe -lz -lcomdlg32 -lgdi32 -Wl,--large-address-aware
 *
 * FEATURES:
 *  - HIGH-SPEED PING-PONG I/O: Processes huge chunks without random disk seeks.
 *  - FEATURE FILTERING: Selectively extract only the layers you need.
 *  - BMFZ COMPRESSION: Natively streams the final output into a zlib compressed 
 *    BMFZ binary to reduce file size drastically.
 * ============================================================================ */
#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <math.h>

#define USE_ZLIB 1

#ifdef _WIN32
    #define FSEEK64 _fseeki64
    #define FTELL64 _ftelli64
#else
    #define FSEEK64 fseeko
    #define FTELL64 ftello
#endif

#define IO_BUF_SIZE (1024 * 1024 * 4) // 4MB Disk Buffer

#define CLASS_UNKNOWN   0
#define CLASS_WATER     1
#define CLASS_LAND      2
#define CLASS_PARK      3
#define CLASS_COASTLINE 4
#define CLASS_HWY_MINOR 5
#define CLASS_HWY_MAIN  6

#pragma pack(push, 1)
typedef struct { uint64_t id; int32_t lat; int32_t lon; } PbfNode; 
typedef struct { uint64_t id; double lat; double lon; } WayNode; 
#pragma pack(pop)

typedef struct { const char* str; size_t len; } PbfString;

typedef struct {
    char* name;
    char* pcode;
    uint32_t count;
    WayNode* nodes;
    bool merged;
} AccumWay;

// --- Global Config ---
bool g_CfgLand = true;
bool g_CfgCoast = true;
bool g_CfgHwyMain = true;
bool g_CfgHwyMin = true;
bool g_CfgWater = false;
bool g_CfgPark = false;
bool g_CfgPcode = true;
bool g_CfgZlib = true;

// --- GUI Globals ---
HWND hTxtIn, hTxtOut, hBtnIn, hBtnOut, hBtnRun;
HWND hChkLand, hChkCoast, hChkHwyMain, hChkHwyMin, hChkWater, hChkPark, hChkPcode, hChkZlib;

uint32_t SwapEndian32(uint32_t val) {
    return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) | ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000);
}

uint64_t ReadVarint(const uint8_t** ptr, const uint8_t* end) {
    uint64_t val = 0; int shift = 0;
    while (*ptr < end) {
        uint8_t b = **ptr; (*ptr)++;
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break; 
        shift += 7;
    }
    return val;
}

int64_t DecodeZigZag(uint64_t n) { return (n >> 1) ^ -(int64_t)(n & 1); }

void SkipProtobufField(const uint8_t** ptr, const uint8_t* end, uint8_t wire_type) {
    if(wire_type == 0) ReadVarint(ptr, end);
    else if(wire_type == 1) *ptr += 8;
    else if(wire_type == 2) *ptr += ReadVarint(ptr, end);
    else if(wire_type == 5) *ptr += 4;
    else *ptr = end;
}

int CompareNodes(const void* a, const void* b) {
    uint64_t idA = ((PbfNode*)a)->id, idB = ((PbfNode*)b)->id;
    return (idA > idB) - (idA < idB);
}

PbfNode* FindNode(PbfNode* cache, uint64_t count, uint64_t id) {
    int64_t left = 0, right = count - 1;
    while (left <= right) {
        int64_t mid = left + (right - left) / 2;
        if (cache[mid].id == id) return &cache[mid];
        if (cache[mid].id < id) left = mid + 1;
        else right = mid - 1;
    }
    return NULL;
}

void ProcessPrimitiveBlock(const uint8_t* data, uint64_t size, int extract_mode, FILE* outFile) {
    const uint8_t* end = data + size;
    int64_t lat_offset = 0, lon_offset = 0, granularity = 100;
    PbfString* str_table = NULL; uint64_t str_count = 0, str_cap = 0;
    const uint8_t* ptr = data;

    while (ptr < end) {
        uint64_t tag_wire = ReadVarint(&ptr, end);
        if ((tag_wire >> 3) == 1 && (tag_wire & 7) == 2) { 
            uint64_t st_len = ReadVarint(&ptr, end);
            const uint8_t* st_end = ptr + st_len;
            while (ptr < st_end) {
                uint64_t s_tag_wire = ReadVarint(&ptr, st_end);
                if ((s_tag_wire >> 3) == 1) {
                    uint64_t s_len = ReadVarint(&ptr, st_end);
                    if (str_count >= str_cap) {
                        str_cap = str_cap == 0 ? 1024 : str_cap * 2;
                        str_table = (PbfString*)realloc(str_table, str_cap * sizeof(PbfString));
                    }
                    str_table[str_count].str = (const char*)ptr;
                    str_table[str_count].len = (size_t)s_len;
                    str_count++; ptr += s_len;
                } else SkipProtobufField(&ptr, st_end, s_tag_wire & 7);
            }
        }
        else if ((tag_wire >> 3) == 17) granularity = ReadVarint(&ptr, end);
        else if ((tag_wire >> 3) == 19) lat_offset = ReadVarint(&ptr, end);
        else if ((tag_wire >> 3) == 20) lon_offset = ReadVarint(&ptr, end);
        else SkipProtobufField(&ptr, end, tag_wire & 7);
    }

    ptr = data;
    while (ptr < end) {
        uint64_t tag_wire = ReadVarint(&ptr, end);
        if ((tag_wire >> 3) == 2 && (tag_wire & 7) == 2) { 
            uint64_t group_len = ReadVarint(&ptr, end);
            const uint8_t* group_end = ptr + group_len;
            
            while (ptr < group_end) {
                uint64_t g_tag_wire = ReadVarint(&ptr, group_end);
                uint64_t g_tag = g_tag_wire >> 3;

                if (extract_mode == 1 && (g_tag == 1 || g_tag == 2)) {
                    SkipProtobufField(&ptr, group_end, g_tag_wire & 7); 
                    continue;
                }
                if (extract_mode == 2 && g_tag == 3) {
                    SkipProtobufField(&ptr, group_end, g_tag_wire & 7); 
                    continue;
                }

                if (g_tag == 1 && extract_mode == 2) { 
                    uint64_t msg_len = ReadVarint(&ptr, group_end);
                    const uint8_t* msg_end = ptr + msg_len;
                    uint64_t id = 0; int64_t lat = 0, lon = 0;
                    while (ptr < msg_end) {
                        uint64_t n_tag_wire = ReadVarint(&ptr, msg_end);
                        if ((n_tag_wire >> 3) == 1) id = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else if ((n_tag_wire >> 3) == 8) lat = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else if ((n_tag_wire >> 3) == 9) lon = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else SkipProtobufField(&ptr, msg_end, n_tag_wire & 7);
                    }
                    PbfNode node;
                    node.id = id;
                    node.lat = (int32_t)((lat_offset + (granularity * lat)) / 100);
                    node.lon = (int32_t)((lon_offset + (granularity * lon)) / 100);
                    fwrite(&node, sizeof(PbfNode), 1, outFile);
                }
                else if (g_tag == 2 && extract_mode == 2) { 
                    uint64_t dense_len = ReadVarint(&ptr, group_end);
                    const uint8_t* dense_end = ptr + dense_len;
                    const uint8_t *id_ptr = NULL, *lat_ptr = NULL, *lon_ptr = NULL;

                    while (ptr < dense_end) {
                        uint64_t d_tag_wire = ReadVarint(&ptr, dense_end);
                        if ((d_tag_wire >> 3) == 1) { id_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else if ((d_tag_wire >> 3) == 8) { lat_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else if ((d_tag_wire >> 3) == 9) { lon_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else SkipProtobufField(&ptr, dense_end, d_tag_wire & 7);
                    }
                    if (id_ptr && lat_ptr && lon_ptr) {
                        uint64_t id_len = ReadVarint(&id_ptr, dense_end);
                        ReadVarint(&lat_ptr, dense_end); ReadVarint(&lon_ptr, dense_end);
                        const uint8_t* id_end = id_ptr + id_len;
                        int64_t last_id = 0, last_lat = 0, last_lon = 0;

                        while (id_ptr < id_end) {
                            last_id += DecodeZigZag(ReadVarint(&id_ptr, dense_end));
                            last_lat += DecodeZigZag(ReadVarint(&lat_ptr, dense_end));
                            last_lon += DecodeZigZag(ReadVarint(&lon_ptr, dense_end));
                            
                            PbfNode node;
                            node.id = last_id;
                            node.lat = (int32_t)((lat_offset + (granularity * last_lat)) / 100);
                            node.lon = (int32_t)((lon_offset + (granularity * last_lon)) / 100);
                            fwrite(&node, sizeof(PbfNode), 1, outFile);
                        }
                    }
                } 
                else if (g_tag == 3 && extract_mode == 1) { 
                    uint64_t way_len = ReadVarint(&ptr, group_end);
                    const uint8_t* way_end = ptr + way_len;
                    const uint8_t *refs_ptr = NULL, *keys_ptr = NULL, *vals_ptr = NULL;
                    uint64_t keys_len = 0, vals_len = 0;

                    while (ptr < way_end) {
                        uint64_t w_tag_wire = ReadVarint(&ptr, way_end);
                        if ((w_tag_wire >> 3) == 2) { keys_len = ReadVarint(&ptr, way_end); keys_ptr = ptr; ptr += keys_len; }
                        else if ((w_tag_wire >> 3) == 3) { vals_len = ReadVarint(&ptr, way_end); vals_ptr = ptr; ptr += vals_len; }
                        else if ((w_tag_wire >> 3) == 8) { refs_ptr = ptr; SkipProtobufField(&ptr, way_end, w_tag_wire & 7); }
                        else SkipProtobufField(&ptr, way_end, w_tag_wire & 7);
                    }

                    if (refs_ptr) {
                        int feature_class = CLASS_UNKNOWN;
                        const char* feat_name = NULL; size_t feat_name_len = 0;
                        const char* feat_postcode = NULL; size_t feat_postcode_len = 0;

                        if (keys_ptr && vals_ptr && str_table) {
                            const uint8_t *k_p = keys_ptr, *v_p = vals_ptr;
                            while (k_p < keys_ptr + keys_len && v_p < vals_ptr + vals_len) {
                                uint64_t k_idx = ReadVarint(&k_p, keys_ptr + keys_len);
                                uint64_t v_idx = ReadVarint(&v_p, vals_ptr + vals_len);
                                if (k_idx < str_count && v_idx < str_count) {
                                    PbfString* k = &str_table[k_idx]; PbfString* v = &str_table[v_idx];
                                    
                                    if (k->len == 4 && strncmp(k->str, "name", 4) == 0) { feat_name = v->str; feat_name_len = v->len; }
                                    else if ((k->len == 13 && strncmp(k->str, "addr:postcode", 13) == 0) || 
                                             (k->len == 11 && strncmp(k->str, "postal_code", 11) == 0)) {
                                        feat_postcode = v->str; feat_postcode_len = v->len;
                                    }
                                    else if (k->len == 7 && strncmp(k->str, "highway", 7) == 0) {
                                        feature_class = CLASS_HWY_MINOR;
                                        if (v->len == 8 && strncmp(v->str, "motorway", 8) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 5 && strncmp(v->str, "trunk", 5) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 7 && strncmp(v->str, "primary", 7) == 0) feature_class = CLASS_HWY_MAIN;
                                    }
                                    else if (k->len == 7 && strncmp(k->str, "natural", 7) == 0) {
                                        if (v->len == 5 && strncmp(v->str, "water", 5) == 0) feature_class = CLASS_WATER;
                                        else if (v->len == 9 && strncmp(v->str, "coastline", 9) == 0) feature_class = CLASS_COASTLINE;
                                        else if (v->len == 4 && strncmp(v->str, "land", 4) == 0) feature_class = CLASS_LAND;
                                        else if (v->len == 4 && strncmp(v->str, "wood", 4) == 0) feature_class = CLASS_PARK;
                                    }
                                    else if (k->len == 8 && strncmp(k->str, "waterway", 8) == 0) feature_class = CLASS_WATER;
                                    else if (k->len == 7 && strncmp(k->str, "landuse", 7) == 0) {
                                        if (v->len == 6 && strncmp(v->str, "forest", 6) == 0) feature_class = CLASS_PARK;
                                        else if (v->len == 5 && strncmp(v->str, "grass", 5) == 0) feature_class = CLASS_PARK;
                                        else feature_class = CLASS_LAND;
                                    }
                                    else if (k->len == 7 && strncmp(k->str, "leisure", 7) == 0) feature_class = CLASS_PARK;
                                }
                            }
                        }

                        // Apply GUI filters to features
                        if (feature_class == CLASS_WATER && !g_CfgWater) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_LAND && !g_CfgLand) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_PARK && !g_CfgPark) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_COASTLINE && !g_CfgCoast) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_HWY_MAIN && !g_CfgHwyMain) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_HWY_MINOR && !g_CfgHwyMin) feature_class = CLASS_UNKNOWN;
                        if (!g_CfgPcode) { feat_postcode = NULL; feat_postcode_len = 0; }

                        if (feature_class != CLASS_UNKNOWN) {
                            uint64_t refs_len = ReadVarint(&refs_ptr, way_end);
                            const uint8_t* refs_end = refs_ptr + refs_len;

                            uint8_t fc = (uint8_t)feature_class;
                            fwrite(&fc, 1, 1, outFile);

                            uint16_t nlen = feat_name ? feat_name_len : 0;
                            fwrite(&nlen, 2, 1, outFile);
                            if (nlen > 0) fwrite(feat_name, 1, nlen, outFile);

                            uint16_t plen = feat_postcode ? feat_postcode_len : 0;
                            fwrite(&plen, 2, 1, outFile);
                            if (plen > 0) fwrite(feat_postcode, 1, plen, outFile);

                            uint32_t ref_count = 0;
                            WayNode* temp_refs = malloc(16000 * sizeof(WayNode));
                            if (temp_refs) {
                                int64_t last_ref = 0;
                                while (refs_ptr < refs_end && ref_count < 16000) {
                                    last_ref += DecodeZigZag(ReadVarint(&refs_ptr, refs_end));
                                    temp_refs[ref_count].id = (uint64_t)last_ref;
                                    temp_refs[ref_count].lat = 0.0;
                                    temp_refs[ref_count].lon = 0.0;
                                    ref_count++;
                                }
                                
                                fwrite(&ref_count, 4, 1, outFile);
                                fwrite(temp_refs, sizeof(WayNode), ref_count, outFile);
                                free(temp_refs);
                            }
                        }
                    }
                }
                else SkipProtobufField(&ptr, group_end, g_tag_wire & 7);
            }
        } else SkipProtobufField(&ptr, end, tag_wire & 7);
    }
    if (str_table) free(str_table);
}

// Zlib Stream Compressor for Final BMF
bool StreamCompressBMFZ(const char* in_path, const char* out_path) {
    FILE* fin = fopen(in_path, "rb");
    if (!fin) return false;

    FILE* fout = fopen(out_path, "wb");
    if (!fout) { fclose(fin); return false; }

    FSEEK64(fin, 0, SEEK_END);
    uint64_t uncomp_size = FTELL64(fin);
    FSEEK64(fin, 0, SEEK_SET);

    fwrite("BMFZ", 1, 4, fout);
    fwrite(&uncomp_size, 8, 1, fout);

    z_stream strm = {0};
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK) {
        fclose(fin); fclose(fout); return false;
    }

    uint8_t in[262144];
    uint8_t out[262144];
    int flush;

    do {
        strm.avail_in = fread(in, 1, sizeof(in), fin);
        if (ferror(fin)) { deflateEnd(&strm); fclose(fin); fclose(fout); return false; }
        flush = feof(fin) ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;

        do {
            strm.avail_out = sizeof(out);
            strm.next_out = out;
            deflate(&strm, flush);
            size_t have = sizeof(out) - strm.avail_out;
            if (fwrite(out, 1, have, fout) != have || ferror(fout)) {
                deflateEnd(&strm); fclose(fin); fclose(fout); return false;
            }
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);

    deflateEnd(&strm);
    fclose(fin);
    fclose(fout);
    return true;
}

// ============================================================================
// CORE COMPILER LOGIC
// ============================================================================
int RunCompilation(const char* in_filename, const char* out_filename, bool has_bbox, double b_minLon, double b_minLat, double b_maxLon, double b_maxLat) {
    char ways_tmp_a[1024]; snprintf(ways_tmp_a, sizeof(ways_tmp_a), "%s.ways.A.tmp", out_filename);
    char ways_tmp_b[1024]; snprintf(ways_tmp_b, sizeof(ways_tmp_b), "%s.ways.B.tmp", out_filename);
    char nodes_tmp_name[1024]; snprintf(nodes_tmp_name, sizeof(nodes_tmp_name), "%s.nodes.tmp", out_filename);
    char final_tmp_name[1024]; snprintf(final_tmp_name, sizeof(final_tmp_name), "%s.final.tmp", out_filename);

    char* current_ways = ways_tmp_a;
    char* next_ways = ways_tmp_b;

    for (int extract_mode = 1; extract_mode <= 2; extract_mode++) {
        FILE* tmpOut = NULL;
        char* io_buf = malloc(IO_BUF_SIZE);
        
        if (extract_mode == 1) {
            printf("Pass 1: Streaming Filtered Ways to Disk...\n");
            tmpOut = fopen(current_ways, "wb");
        } else {
            printf("Pass 2: Streaming All Nodes to Disk...\n");
            tmpOut = fopen(nodes_tmp_name, "wb");
        }
        
        if (tmpOut && io_buf) setvbuf(tmpOut, io_buf, _IOFBF, IO_BUF_SIZE);

        FILE* file = fopen(in_filename, "rb");
        if (!file) { 
            printf("Cannot open input file: %s\n", in_filename); 
            if (tmpOut) fclose(tmpOut);
            if (io_buf) free(io_buf);
            return 1; 
        }

        char* read_buf = malloc(IO_BUF_SIZE);
        if (read_buf) setvbuf(file, read_buf, _IOFBF, IO_BUF_SIZE);
        
        while (!feof(file)) {
            uint32_t header_len_be = 0;
            if (fread(&header_len_be, 1, 4, file) != 4) break;
            uint32_t header_len = SwapEndian32(header_len_be);
            if (header_len > 64 * 1024) break; 
            uint8_t* header_buf = (uint8_t*)malloc(header_len);
            if (fread(header_buf, 1, header_len, file) != header_len) break;

            const uint8_t* ptr = header_buf; const uint8_t* end = header_buf + header_len;
            char blob_type[32] = {0}; uint64_t datasize = 0;
            while (ptr < end) {
                uint64_t tag_wire = ReadVarint(&ptr, end);
                if ((tag_wire >> 3) == 1) {
                    uint64_t str_len = ReadVarint(&ptr, end);
                    size_t cp_len = str_len < 31 ? (size_t)str_len : 31;
                    memcpy(blob_type, ptr, cp_len);
                    blob_type[cp_len] = '\0';
                    ptr += str_len;
                } 
                else if ((tag_wire >> 3) == 3) datasize = ReadVarint(&ptr, end);
                else SkipProtobufField(&ptr, end, tag_wire & 7);
            }
            free(header_buf);
            
            if (datasize > 64 * 1024 * 1024) break;
            uint8_t* blob_buf = (uint8_t*)malloc(datasize);
            if (fread(blob_buf, 1, datasize, file) != datasize) break;

            if (strcmp(blob_type, "OSMData") == 0) {
                ptr = blob_buf; end = blob_buf + datasize;
                uint64_t raw_size = 0, zlib_size = 0; 
                uint8_t* zlib_data = NULL;
                uint8_t* raw_data = NULL;
                uint64_t raw_len = 0;

                while (ptr < end) {
                    uint64_t tag_wire = ReadVarint(&ptr, end);
                    if ((tag_wire >> 3) == 1) { 
                        raw_len = ReadVarint(&ptr, end);
                        raw_data = (uint8_t*)ptr;
                        ptr += raw_len;
                    }
                    else if ((tag_wire >> 3) == 2) { 
                        raw_size = ReadVarint(&ptr, end);
                    }
                    else if ((tag_wire >> 3) == 3) { 
                        zlib_size = ReadVarint(&ptr, end); 
                        zlib_data = (uint8_t*)ptr; 
                        ptr += zlib_size; 
                    } 
                    else SkipProtobufField(&ptr, end, tag_wire & 7);
                }

                if (zlib_data && raw_size > 0) {
                    uint8_t* uncompressed = (uint8_t*)malloc(raw_size);
                    unsigned long dest_len = raw_size;
                    if (uncompress(uncompressed, &dest_len, zlib_data, zlib_size) == Z_OK)
                        ProcessPrimitiveBlock(uncompressed, dest_len, extract_mode, tmpOut);
                    free(uncompressed);
                } else if (raw_data && raw_len > 0) {
                    ProcessPrimitiveBlock(raw_data, raw_len, extract_mode, tmpOut);
                }
            }
            free(blob_buf);
        }
        fclose(file);
        if (tmpOut) fclose(tmpOut);
        if (io_buf) free(io_buf);
        if (read_buf) free(read_buf);
    }

    printf("Pass 3: Sequential Ping-Pong Resolution...\n");
    FILE* fnodes = fopen(nodes_tmp_name, "rb");
    if (!fnodes) return 1;
    
    char* io_node_buf = malloc(IO_BUF_SIZE);
    if (io_node_buf) setvbuf(fnodes, io_node_buf, _IOFBF, IO_BUF_SIZE);

    const uint64_t CHUNK_NODES = 15000000; 
    PbfNode* chunk = malloc(CHUNK_NODES * sizeof(PbfNode));
    if (!chunk) {
        printf("Fatal OOM: Could not allocate memory chunk.\n");
        fclose(fnodes);
        return 1;
    }

    uint64_t nodes_read;
    int chunk_idx = 1;

    while ((nodes_read = fread(chunk, sizeof(PbfNode), CHUNK_NODES, fnodes)) > 0) {
        printf("  -> Resolving Node Chunk %d...\n", chunk_idx++);
        qsort(chunk, nodes_read, sizeof(PbfNode), CompareNodes);

        FILE* fways_in = fopen(current_ways, "rb");
        FILE* fways_out = fopen(next_ways, "wb");
        
        char* in_buf = malloc(IO_BUF_SIZE);
        char* out_buf = malloc(IO_BUF_SIZE);
        if (fways_in && in_buf) setvbuf(fways_in, in_buf, _IOFBF, IO_BUF_SIZE);
        if (fways_out && out_buf) setvbuf(fways_out, out_buf, _IOFBF, IO_BUF_SIZE);

        if(!fways_in || !fways_out) break;

        while (1) {
            uint8_t fc;
            if (fread(&fc, 1, 1, fways_in) != 1) break;

            uint16_t nlen;
            if (fread(&nlen, 2, 1, fways_in) != 1) break;
            char* name = NULL;
            if (nlen > 0) {
                name = malloc(nlen + 1);
                if (name) { fread(name, 1, nlen, fways_in); name[nlen] = '\0'; }
                else FSEEK64(fways_in, nlen, SEEK_CUR);
            }

            uint16_t plen;
            if (fread(&plen, 2, 1, fways_in) != 1) break;
            char* pcode = NULL;
            if (plen > 0) {
                pcode = malloc(plen + 1);
                if (pcode) { fread(pcode, 1, plen, fways_in); pcode[plen] = '\0'; }
                else FSEEK64(fways_in, plen, SEEK_CUR);
            }

            uint32_t ref_count;
            if (fread(&ref_count, 4, 1, fways_in) != 1) break;

            if (ref_count == 0 || ref_count > 16000) {
                if (name) free(name);
                if (pcode) free(pcode);
                break;
            }

            WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
            if (!wnodes) {
                if (name) free(name);
                if (pcode) free(pcode);
                break;
            }

            if (fread(wnodes, sizeof(WayNode), ref_count, fways_in) != ref_count) {
                if (name) free(name);
                if (pcode) free(pcode);
                free(wnodes);
                break;
            }

            for (uint32_t i = 0; i < ref_count; i++) {
                if (wnodes[i].id != 0xFFFFFFFFFFFFFFFFULL) {
                    PbfNode* found = FindNode(chunk, nodes_read, wnodes[i].id);
                    if (found) {
                        wnodes[i].lat = found->lat * 1e-7;
                        wnodes[i].lon = found->lon * 1e-7;
                        wnodes[i].id = 0xFFFFFFFFFFFFFFFFULL; 
                    }
                }
            }

            fwrite(&fc, 1, 1, fways_out);
            fwrite(&nlen, 2, 1, fways_out);
            if (nlen > 0 && name) fwrite(name, 1, nlen, fways_out);
            fwrite(&plen, 2, 1, fways_out);
            if (plen > 0 && pcode) fwrite(pcode, 1, plen, fways_out);
            fwrite(&ref_count, 4, 1, fways_out);
            fwrite(wnodes, sizeof(WayNode), ref_count, fways_out);

            if (name) free(name);
            if (pcode) free(pcode);
            free(wnodes);
        }

        fclose(fways_in);
        fclose(fways_out);
        if (in_buf) free(in_buf);
        if (out_buf) free(out_buf);

        remove(current_ways);
        char* tmp = current_ways;
        current_ways = next_ways;
        next_ways = tmp;
    }
    free(chunk);
    fclose(fnodes);
    if (io_node_buf) free(io_node_buf);

    printf("Pass 4: Writing Raw Binary Block...\n");
    FILE* fways = fopen(current_ways, "rb");
    FILE* out = fopen(final_tmp_name, "wb");
    
    char* io_final_in = malloc(IO_BUF_SIZE);
    char* io_final_out = malloc(IO_BUF_SIZE);
    if (fways && io_final_in) setvbuf(fways, io_final_in, _IOFBF, IO_BUF_SIZE);
    if (out && io_final_out) setvbuf(out, io_final_out, _IOFBF, IO_BUF_SIZE);

    if (!fways || !out) return 1;

    fwrite("BMF2", 1, 4, out);
    uint64_t written_feats = 0;
    long count_pos = ftell(out);
    fwrite(&written_feats, sizeof(uint64_t), 1, out);

    uint32_t coast_cap = 0, coast_cnt = 0;
    AccumWay* coasts = NULL;

    while (1) {
        uint8_t fc;
        if (fread(&fc, 1, 1, fways) != 1) break;

        uint16_t nlen; 
        if (fread(&nlen, 2, 1, fways) != 1) break;
        char* name = NULL;
        if (nlen > 0) { 
            name = malloc(nlen + 1); 
            if (name) { fread(name, 1, nlen, fways); name[nlen] = '\0'; }
            else FSEEK64(fways, nlen, SEEK_CUR);
        }

        uint16_t plen; 
        if (fread(&plen, 2, 1, fways) != 1) break;
        char* pcode = NULL;
        if (plen > 0) { 
            pcode = malloc(plen + 1); 
            if (pcode) { fread(pcode, 1, plen, fways); pcode[plen] = '\0'; }
            else FSEEK64(fways, plen, SEEK_CUR);
        }

        uint32_t ref_count; 
        if (fread(&ref_count, 4, 1, fways) != 1) break;
        
        if (ref_count == 0 || ref_count > 16000) {
            if (name) free(name);
            if (pcode) free(pcode);
            break;
        }

        WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
        if (!wnodes) {
            if (name) free(name);
            if (pcode) free(pcode);
            break;
        }
        
        if (fread(wnodes, sizeof(WayNode), ref_count, fways) != ref_count) {
            if (name) free(name);
            if (pcode) free(pcode);
            free(wnodes);
            break;
        }

        bool valid = true;
        for (uint32_t i = 0; i < ref_count; i++) {
            if (wnodes[i].id != 0xFFFFFFFFFFFFFFFFULL) {
                valid = false;
                break;
            }
        }

        if (valid && ref_count >= 2) {
            if (fc == CLASS_COASTLINE) {
                if (coast_cnt >= coast_cap) {
                    coast_cap = coast_cap == 0 ? 2000 : coast_cap * 2;
                    coasts = realloc(coasts, coast_cap * sizeof(AccumWay));
                }
                coasts[coast_cnt].name = name;
                coasts[coast_cnt].pcode = pcode;
                coasts[coast_cnt].count = ref_count;
                coasts[coast_cnt].nodes = wnodes;
                coasts[coast_cnt].merged = false;
                coast_cnt++;
            } else {
                double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                for (uint32_t i = 0; i < ref_count; i++) {
                    if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon;
                    if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                    if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat;
                    if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                }

                bool in_bbox = true;
                if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) {
                    in_bbox = false;
                }

                if (in_bbox) {
                    if (fc == CLASS_LAND || fc == CLASS_WATER || fc == CLASS_PARK) {
                        if (fabs(wnodes[0].lon - wnodes[ref_count-1].lon) > 1e-6 || 
                            fabs(wnodes[0].lat - wnodes[ref_count-1].lat) > 1e-6) {
                            WayNode* safe = realloc(wnodes, (ref_count + 1) * sizeof(WayNode));
                            if (safe) {
                                wnodes = safe;
                                wnodes[ref_count] = wnodes[0];
                                ref_count++;
                            }
                        }
                    }

                    if (wnodes) {
                        fwrite(&fc, 1, 1, out);
                        fwrite(&nlen, 2, 1, out);
                        if (nlen > 0 && name) fwrite(name, 1, nlen, out);
                        fwrite(&plen, 2, 1, out);
                        if (plen > 0 && pcode) fwrite(pcode, 1, plen, out);
                        
                        fwrite(&ref_count, 4, 1, out);
                        for(uint32_t i = 0; i < ref_count; i++) {
                            fwrite(&wnodes[i].lon, sizeof(double), 1, out);
                            fwrite(&wnodes[i].lat, sizeof(double), 1, out);
                        }
                        written_feats++;
                    }
                }
                if (name) free(name);
                if (pcode) free(pcode);
                if (wnodes) free(wnodes);
            }
        } else {
            if (name) free(name);
            if (pcode) free(pcode);
            free(wnodes);
        }
    }
    
    if (coast_cnt > 0) {
        printf("  -> Assembling %u Coastlines...\n", coast_cnt);
        bool merged = true;
        while (merged) {
            merged = false;
            for (uint32_t i = 0; i < coast_cnt; i++) {
                if (coasts[i].merged || coasts[i].count < 2) continue;
                
                WayNode start_pt = coasts[i].nodes[0];
                WayNode end_pt = coasts[i].nodes[coasts[i].count - 1];

                if (fabs(start_pt.lon - end_pt.lon) < 1e-6 && fabs(start_pt.lat - end_pt.lat) < 1e-6) continue; 

                for (uint32_t j = i + 1; j < coast_cnt; j++) {
                    if (coasts[j].merged || coasts[j].count < 2) continue;

                    WayNode j_start = coasts[j].nodes[0];
                    WayNode j_end = coasts[j].nodes[coasts[j].count - 1];

                    if (fabs(end_pt.lon - j_start.lon) < 1e-6 && fabs(end_pt.lat - j_start.lat) < 1e-6) {
                        uint32_t new_count = coasts[i].count + coasts[j].count - 1;
                        WayNode* safe = realloc(coasts[i].nodes, new_count * sizeof(WayNode));
                        if(safe) {
                            coasts[i].nodes = safe;
                            memcpy(coasts[i].nodes + coasts[i].count, coasts[j].nodes + 1, (coasts[j].count - 1) * sizeof(WayNode));
                            coasts[i].count = new_count;
                            coasts[j].merged = true;
                            merged = true;
                        }
                        break;
                    }
                    else if (fabs(start_pt.lon - j_end.lon) < 1e-6 && fabs(start_pt.lat - j_end.lat) < 1e-6) {
                        uint32_t new_count = coasts[j].count + coasts[i].count - 1;
                        WayNode* safe = realloc(coasts[j].nodes, new_count * sizeof(WayNode));
                        if(safe) {
                            coasts[j].nodes = safe;
                            memcpy(coasts[j].nodes + coasts[j].count, coasts[i].nodes + 1, (coasts[i].count - 1) * sizeof(WayNode));
                            coasts[j].count = new_count;
                            coasts[i].merged = true;
                            merged = true;
                        }
                        break;
                    }
                    else if (fabs(end_pt.lon - j_end.lon) < 1e-6 && fabs(end_pt.lat - j_end.lat) < 1e-6) {
                        uint32_t new_count = coasts[i].count + coasts[j].count - 1;
                        WayNode* safe = realloc(coasts[i].nodes, new_count * sizeof(WayNode));
                        if(safe) {
                            coasts[i].nodes = safe;
                            for (uint32_t k = 1; k < coasts[j].count; k++) {
                                coasts[i].nodes[coasts[i].count + k - 1] = coasts[j].nodes[coasts[j].count - 1 - k];
                            }
                            coasts[i].count = new_count;
                            coasts[j].merged = true;
                            merged = true;
                        }
                        break;
                    }
                    else if (fabs(start_pt.lon - j_start.lon) < 1e-6 && fabs(start_pt.lat - j_start.lat) < 1e-6) {
                        uint32_t new_count = coasts[j].count + coasts[i].count - 1;
                        WayNode* safe = realloc(coasts[j].nodes, new_count * sizeof(WayNode));
                        if(safe) {
                            coasts[j].nodes = safe;
                            for (uint32_t k = 1; k < coasts[i].count; k++) {
                                coasts[j].nodes[coasts[j].count + k - 1] = coasts[i].nodes[coasts[i].count - 1 - k];
                            }
                            coasts[j].count = new_count;
                            coasts[i].merged = true;
                            merged = true;
                        }
                        break;
                    }
                }
                if (merged) break; 
            }
        }

        for (uint32_t i = 0; i < coast_cnt; i++) {
            if (coasts[i].merged) {
                if (coasts[i].name) free(coasts[i].name);
                if (coasts[i].pcode) free(coasts[i].pcode);
                free(coasts[i].nodes);
                continue;
            }

            WayNode* wnodes = coasts[i].nodes;
            uint32_t ref_count = coasts[i].count;

            if (fabs(wnodes[0].lon - wnodes[ref_count-1].lon) > 1e-6 || 
                fabs(wnodes[0].lat - wnodes[ref_count-1].lat) > 1e-6) {
                WayNode* safe = realloc(wnodes, (ref_count + 1) * sizeof(WayNode));
                if (safe) {
                    wnodes = safe;
                    wnodes[ref_count] = wnodes[0];
                    ref_count++;
                }
            }

            double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
            for (uint32_t j = 0; j < ref_count; j++) {
                if (wnodes[j].lon < min_lon) min_lon = wnodes[j].lon;
                if (wnodes[j].lon > max_lon) max_lon = wnodes[j].lon;
                if (wnodes[j].lat < min_lat) min_lat = wnodes[j].lat;
                if (wnodes[j].lat > max_lat) max_lat = wnodes[j].lat;
            }

            bool in_bbox = true;
            if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) {
                in_bbox = false;
            }

            if (in_bbox) {
                uint8_t fc = CLASS_LAND; 
                uint16_t nlen = coasts[i].name ? strlen(coasts[i].name) : 0;
                uint16_t plen = coasts[i].pcode ? strlen(coasts[i].pcode) : 0;
                
                fwrite(&fc, 1, 1, out);
                fwrite(&nlen, 2, 1, out);
                if (nlen > 0) fwrite(coasts[i].name, 1, nlen, out);
                fwrite(&plen, 2, 1, out);
                if (plen > 0) fwrite(coasts[i].pcode, 1, plen, out);
                
                fwrite(&ref_count, 4, 1, out);
                for(uint32_t k = 0; k < ref_count; k++) {
                    fwrite(&wnodes[k].lon, sizeof(double), 1, out);
                    fwrite(&wnodes[k].lat, sizeof(double), 1, out);
                }
                written_feats++;
            }
            
            if (coasts[i].name) free(coasts[i].name);
            if (coasts[i].pcode) free(coasts[i].pcode);
            if (wnodes) free(wnodes);
        }
        free(coasts);
    }

    fseek(out, count_pos, SEEK_SET);
    fwrite(&written_feats, sizeof(uint64_t), 1, out);
    
    fclose(fways);
    fclose(out);
    if (io_final_in) free(io_final_in);
    if (io_final_out) free(io_final_out);

    remove(ways_tmp_a);
    remove(ways_tmp_b);
    remove(nodes_tmp_name);

    if (g_CfgZlib) {
        printf("Pass 5: Compacting and writing Zlib compressed BMFZ...\n");
        StreamCompressBMFZ(final_tmp_name, out_filename);
        remove(final_tmp_name);
        printf("Done! Wrote %llu features to highly compressed %s\n", written_feats, out_filename);
    } else {
        remove(out_filename);
        rename(final_tmp_name, out_filename);
        printf("Done! Wrote %llu features to uncompressed %s\n", written_feats, out_filename);
    }

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
    
    printf("\n=== COMPILATION STARTED ===\n");
    printf("Input: %s\nOutput: %s\n\n", args->inFile, args->outFile);
    
    int result = RunCompilation(args->inFile, args->outFile, false, 0, 0, 0, 0);
    
    if (result == 0) MessageBoxA(NULL, "Compilation completed!", "Success", MB_ICONINFORMATION);
    else MessageBoxA(NULL, "Compilation failed.", "Error", MB_ICONERROR);

    EnableWindow(hBtnRun, TRUE);
    free(args);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            CreateWindowA("STATIC", "Source PBF File:", WS_VISIBLE|WS_CHILD, 20, 20, 150, 20, hwnd, NULL, NULL, NULL);
            hTxtIn = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 40, 310, 24, hwnd, NULL, NULL, NULL);
            hBtnIn = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 340, 40, 80, 24, hwnd, (HMENU)1, NULL, NULL);

            CreateWindowA("STATIC", "Output File (.bmf / .bmfz):", WS_VISIBLE|WS_CHILD, 20, 80, 150, 20, hwnd, NULL, NULL, NULL);
            hTxtOut = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 100, 310, 24, hwnd, NULL, NULL, NULL);
            hBtnOut = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 340, 100, 80, 24, hwnd, (HMENU)2, NULL, NULL);

            CreateWindowA("STATIC", "Feature Layers to Extract:", WS_VISIBLE|WS_CHILD, 20, 140, 200, 20, hwnd, NULL, NULL, NULL);
            
            hChkLand = CreateWindowA("BUTTON", "Landmass", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 165, 100, 20, hwnd, NULL, NULL, NULL);
            hChkCoast = CreateWindowA("BUTTON", "Coastlines", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 190, 100, 20, hwnd, NULL, NULL, NULL);
            hChkHwyMain = CreateWindowA("BUTTON", "Main Roads", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 130, 165, 100, 20, hwnd, NULL, NULL, NULL);
            hChkHwyMin = CreateWindowA("BUTTON", "Minor Roads", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 130, 190, 100, 20, hwnd, NULL, NULL, NULL);
            hChkPcode = CreateWindowA("BUTTON", "Postcodes", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 240, 165, 100, 20, hwnd, NULL, NULL, NULL);
            
            hChkWater = CreateWindowA("BUTTON", "Water Bodies", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 240, 190, 100, 20, hwnd, NULL, NULL, NULL);
            hChkPark = CreateWindowA("BUTTON", "Parks/Forests", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 350, 165, 100, 20, hwnd, NULL, NULL, NULL);
            
            hChkZlib = CreateWindowA("BUTTON", "ZLib Compression (BMFZ)", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 350, 190, 200, 20, hwnd, NULL, NULL, NULL);

            SendMessage(hChkLand, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkCoast, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkHwyMain, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkHwyMin, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkPcode, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkZlib, BM_SETCHECK, BST_CHECKED, 0); // ZLIB on by default

            hBtnRun = CreateWindowA("BUTTON", "Compile BMF", WS_VISIBLE|WS_CHILD, 20, 230, 400, 35, hwnd, (HMENU)3, NULL, NULL);

            EnumChildWindows(hwnd, (WNDENUMPROC)SendMessageA, WM_SETFONT);
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) { 
                OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = "";
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "PBF Maps\0*.pbf\0All\0*.*\0";
                ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) SetWindowTextA(hTxtIn, path);
            }
            else if (LOWORD(wParam) == 2) { 
                OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = "";
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "BMFZ Compressed\0*.bmfz\0BMF Uncompressed\0*.bmf\0";
                ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT;
                if (GetSaveFileNameA(&ofn)) SetWindowTextA(hTxtOut, path);
            }
            else if (LOWORD(wParam) == 3) { 
                ThreadArgs* args = malloc(sizeof(ThreadArgs));
                GetWindowTextA(hTxtIn, args->inFile, MAX_PATH);
                GetWindowTextA(hTxtOut, args->outFile, MAX_PATH);

                if (strlen(args->inFile) == 0 || strlen(args->outFile) == 0) {
                    MessageBoxA(hwnd, "Please specify input and output files.", "Warning", MB_ICONWARNING);
                    free(args); return 0;
                }

                g_CfgLand = SendMessage(hChkLand, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgCoast = SendMessage(hChkCoast, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgHwyMain = SendMessage(hChkHwyMain, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgHwyMin = SendMessage(hChkHwyMin, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgWater = SendMessage(hChkWater, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgPark = SendMessage(hChkPark, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgPcode = SendMessage(hChkPcode, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgZlib = SendMessage(hChkZlib, BM_GETCHECK, 0, 0) == BST_CHECKED;

                CreateThread(NULL, 0, CompileThread, args, 0, NULL);
            }
            break;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        char out_filename[1024];
        strncpy(out_filename, argv[1], sizeof(out_filename) - 1);
        out_filename[sizeof(out_filename) - 1] = '\0';
        char* ext = strrchr(out_filename, '.');
        if (ext && (strcasecmp(ext, ".pbf") == 0)) strcpy(ext, g_CfgZlib ? ".bmfz" : ".bmf");
        else strncat(out_filename, g_CfgZlib ? ".bmfz" : ".bmf", sizeof(out_filename) - strlen(out_filename) - 1);
        
        return RunCompilation(argv[1], out_filename, false, 0, 0, 0, 0);
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "Pbf2BmfGuiClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, "Pbf2BmfGuiClass", "PBF to BMF Compiler",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 320, NULL, NULL, wc.hInstance, NULL);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}