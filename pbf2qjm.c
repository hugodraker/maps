/* ============================================================================
 * pbf2qjm - High-Speed Out-Of-Core PBF to QJMC Compiler
 * 
 * COMPILATION:
 *   gcc -O3 -m32 -mwindows -Wall -Wextra pbf2qjm.c -o pbf2qjm.exe -lcomdlg32 -lgdi32 -lshell32
 * ============================================================================ */

#include "qjm.h"
double g_CenterLon = 0.0, g_CenterLat = 0.0, g_PanX = 0.0, g_PanY = 0.0, g_Zoom = 5.0, g_Rotation = 0.0;

#define IO_BUF_SIZE (1024 * 1024 * 4) 
typedef struct { char inFile[MAX_PATH]; char outFile[MAX_PATH]; } ThreadArgs;

typedef struct {
    int32_t qx, qy;
    uint32_t way_idx;
    uint8_t is_end;
} CoastEndPt;

static bool g_CfgLand = true, g_CfgCoast = true, g_CfgHwyMain = true, g_CfgHwyMin = true;
static bool g_CfgWater = false, g_CfgPark = false, g_CfgPcode = true, g_CfgSpecs = false;
static char g_ShapefileFile[MAX_PATH] = {0};static HWND g_hCompilerWnd = NULL;
static HWND hTxtIn, hTxtOut, hBtnIn, hBtnOut, hBtnRun, hBtnDelTmp, hLblStatus;
static HWND hChkPcode, hChkLZAV, hChkLand, hChkCoast, hChkHwyMain, hChkHwyMin, hChkWater, hChkPark, hChkSpecs;

static HWND hTxtSimpLand, hTxtSimpCoast, hTxtSimpHwyMain, hTxtSimpHwyMin, hTxtSimpWater, hTxtSimpPark, hTxtSimpLowPoly;
static HWND hChkNoRoads, hTxtSimpNoRoads;
static int g_SimpLand = 75, g_SimpCoast = 75, g_SimpHwyMain = 75, g_SimpHwyMin = 75, g_SimpWater = 75, g_SimpPark = 75, g_SimpLowPoly = 75;
static bool g_CfgNoRoads = true;
static int g_SimpNoRoads = 30;

static int CompareCoastEndPt(const void* a, const void* b) {
    const CoastEndPt* ea = (const CoastEndPt*)a;
    const CoastEndPt* eb = (const CoastEndPt*)b;
    if (ea->qx != eb->qx) return (ea->qx < eb->qx) ? -1 : 1;
    if (ea->qy != eb->qy) return (ea->qy < eb->qy) ? -1 : 1;
    return (ea->way_idx < eb->way_idx) ? -1 : (ea->way_idx > eb->way_idx);
}

static void SetStatus(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (hLblStatus) SetWindowTextA(hLblStatus, buf);
    if (g_hCompilerWnd) SetWindowTextA(g_hCompilerWnd, buf);
}

static void LoadConfig() {
    GetPrivateProfileStringA("MapData", "CoastlineShapefile", "", g_ShapefileFile, MAX_PATH, ".\\config.ini");
    if (strlen(g_ShapefileFile) > 0) g_CfgCoast = false; 
}

static void AutoFillOutput(const char* inPath) {
    SetWindowTextA(hTxtIn, inPath);
    char outPath[MAX_PATH]; strncpy(outPath, inPath, MAX_PATH); outPath[MAX_PATH-1] = '\0';
    char* outExt = strrchr(outPath, '.');
    if (outExt) strcpy(outExt, ".qjm"); else strcat(outPath, ".qjm");
    SetWindowTextA(hTxtOut, outPath);
}

static void DeleteTempFilesForOutput(const char* out_filename) {
    if (!out_filename || !*out_filename) return;
    const char* suffixes[] = {
        ".ways.A.tmp", ".ways.B.tmp", ".ways.resolved.tmp",
        ".nodes.tmp", ".raw.tmp", ".coasts.tmp"
    };
    int removed_cnt = 0;
    for (int i = 0; i < 6; i++) {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s%s", out_filename, suffixes[i]);
        if (remove(tmp_path) == 0) removed_cnt++;
    }
    remove("temp_truncate.tmp");
    SetStatus("PBF to QJMC Compiler - Deleted %d temp file(s)", removed_cnt);
}

static int64_t GetFileSize64(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    FSEEK64(f, 0, SEEK_END);
    int64_t sz = FTELL64(f);
    fclose(f);
    return sz;
}

static void WriteRawFeature(FILE* f_raw, WayNode* wnodes, uint32_t ref_count, bool is_area, const char* name, const char* pcode, uint8_t fc, SortEntry** entries, uint64_t* cnt, uint32_t* cap) {
    if (!f_raw || !wnodes || ref_count == 0) return;

    uint16_t nlen = name ? (uint16_t)strlen(name) : 0;
    uint16_t plen = pcode ? (uint16_t)strlen(pcode) : 0;
    uint8_t area_flag = is_area ? 1 : 0;

    uint32_t total_len = 1 + 1 + 2 + nlen + 2 + plen + 4 + (ref_count * sizeof(WayNode));
    uint64_t offset = FTELL64(f_raw);

    fwrite(&total_len, 4, 1, f_raw);
    fwrite(&fc, 1, 1, f_raw);
    fwrite(&area_flag, 1, 1, f_raw);
    fwrite(&nlen, 2, 1, f_raw);
    if (nlen > 0) fwrite(name, 1, nlen, f_raw);
    fwrite(&plen, 2, 1, f_raw);
    if (plen > 0) fwrite(pcode, 1, plen, f_raw);
    fwrite(&ref_count, 4, 1, f_raw);
    fwrite(wnodes, sizeof(WayNode), ref_count, f_raw);

    double min_lon = wnodes[0].lon, max_lon = wnodes[0].lon;
    double min_lat = wnodes[0].lat, max_lat = wnodes[0].lat;
    for (uint32_t i = 1; i < ref_count; i++) {
        if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon;
        if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
        if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat;
        if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
    }

    double cx = (min_lon + max_lon) / 2.0;
    double cy = (min_lat + max_lat) / 2.0;
    int32_t c_ix = (int32_t)((cx + 180.0) / 360.0 * 0x3FFFFFFF);
    int32_t c_iy = (int32_t)((cy + 90.0) / 180.0 * 0x3FFFFFFF);

    if (*cnt >= *cap) {
        *cap = (*cap == 0) ? 250000 : (*cap * 2);
        *entries = realloc(*entries, *cap * sizeof(SortEntry));
    }
    (*entries)[*cnt].morton = EncodeMorton(c_ix, c_iy);
    (*entries)[*cnt].offset = offset;
    (*cnt)++;
}

static uint32_t DecimateNodes(WayNode* in_nodes, uint32_t in_cnt, WayNode* out_nodes, int keep_percent) {
    if (in_cnt <= 2 || keep_percent >= 100) {
        for(uint32_t i = 0; i < in_cnt; i++) out_nodes[i] = in_nodes[i];
        return in_cnt;
    }
    
    uint32_t target_cnt = (uint32_t)(in_cnt * (keep_percent / 100.0));
    if (target_cnt < 2) target_cnt = 2;
    if (target_cnt > 65530) target_cnt = 65530; 

    if (target_cnt >= in_cnt && in_cnt <= 65530) {
        for(uint32_t i = 0; i < in_cnt; i++) out_nodes[i] = in_nodes[i];
        return in_cnt;
    }
    
    out_nodes[0] = in_nodes[0];
    uint32_t out_idx = 1;
    for (uint32_t i = 1; i < target_cnt - 1; i++) {
        uint32_t src_idx = (uint32_t)((i * (in_cnt - 1)) / (double)(target_cnt - 1));
        out_nodes[out_idx++] = in_nodes[src_idx];
    }
    out_nodes[out_idx++] = in_nodes[in_cnt - 1];
    
    return out_idx;
}

#include "pbfs.h"
#include "shps.h"
static int RunCompilation(const char* in_filename, const char* out_filename, bool has_bbox, double b_minLon, double b_minLat, double b_maxLon, double b_maxLat) {
    char ways_tmp_a[1024]; snprintf(ways_tmp_a, sizeof(ways_tmp_a), "%s.ways.A.tmp", out_filename);
    char ways_tmp_b[1024]; snprintf(ways_tmp_b, sizeof(ways_tmp_b), "%s.ways.B.tmp", out_filename);
    char ways_resolved_name[1024]; snprintf(ways_resolved_name, sizeof(ways_resolved_name), "%s.ways.resolved.tmp", out_filename);
    char nodes_tmp_name[1024]; snprintf(nodes_tmp_name, sizeof(nodes_tmp_name), "%s.nodes.tmp", out_filename);
    char raw_tmp_name[1024]; snprintf(raw_tmp_name, sizeof(raw_tmp_name), "%s.raw.tmp", out_filename);
    char coasts_tmp_name[1024]; snprintf(coasts_tmp_name, sizeof(coasts_tmp_name), "%s.coasts.tmp", out_filename);

    uint32_t feature_capacity = 250000; SortEntry* sort_entries = malloc(feature_capacity * sizeof(SortEntry));
    uint64_t written_feats = 0; FILE* f_raw = NULL; bool resumed = false; bool is_main_input_shp = false;
    const char* ext_in = strrchr(in_filename, '.'); if (ext_in && strcasecmp(ext_in, ".shp") == 0) is_main_input_shp = true;

    uint32_t EOF_MARKER = 0xDEADBEEF;

    // 1. Check if .raw.tmp is fully completed
    f_raw = fopen(raw_tmp_name, "rb");
    if (f_raw) {
        FSEEK64(f_raw, 0, SEEK_END); int64_t raw_sz = FTELL64(f_raw);
        if (raw_sz >= 4) {
            FSEEK64(f_raw, -4, SEEK_END);
            uint32_t marker = 0; fread(&marker, 4, 1, f_raw);
            if (marker == EOF_MARKER) {
                SetStatus("Resuming from completed %s (Pass 5)...", raw_tmp_name);
                FSEEK64(f_raw, 0, SEEK_SET);
                uint32_t r_cap = 65536; WayNode* wnodes = malloc(r_cap * sizeof(WayNode));
                while (FTELL64(f_raw) < raw_sz - 4) {
                    uint64_t offset = FTELL64(f_raw); uint32_t total_len;
                    if (fread(&total_len, 4, 1, f_raw) != 1) break;
                    
                    uint8_t fc, area_flag; uint16_t nlen, plen;
                    fread(&fc, 1, 1, f_raw); fread(&area_flag, 1, 1, f_raw); fread(&nlen, 2, 1, f_raw);
                    if (nlen > 0) FSEEK64(f_raw, nlen, SEEK_CUR); fread(&plen, 2, 1, f_raw);
                    if (plen > 0) FSEEK64(f_raw, plen, SEEK_CUR);
                    
                    uint32_t ref_count; fread(&ref_count, 4, 1, f_raw);
                    if (ref_count > r_cap) { r_cap = ref_count + 16384; wnodes = realloc(wnodes, r_cap * sizeof(WayNode)); }
                    fread(wnodes, sizeof(WayNode), ref_count, f_raw);
                    double min_lon = wnodes[0].lon, max_lon = wnodes[0].lon, min_lat = wnodes[0].lat, max_lat = wnodes[0].lat;
                    for(uint32_t i=1; i<ref_count; i++) {
                        if(wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if(wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                        if(wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if(wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                    }
                    
                    double cx = (min_lon + max_lon) / 2.0, cy = (min_lat + max_lat) / 2.0;
                    int32_t c_ix = (int32_t)((cx + 180.0) / 360.0 * 0x3FFFFFFF), c_iy = (int32_t)((cy + 90.0) / 180.0 * 0x3FFFFFFF);
                    if (written_feats >= feature_capacity) { feature_capacity = feature_capacity < 4000000 ? feature_capacity * 2 : feature_capacity + 2000000; sort_entries = realloc(sort_entries, feature_capacity * sizeof(SortEntry)); }
                    sort_entries[written_feats].morton = EncodeMorton(c_ix, c_iy); sort_entries[written_feats].offset = offset; written_feats++;
                }
                free(wnodes);
                resumed = true;
            }
        }
        if (!resumed) { fclose(f_raw); f_raw = NULL; }
    }

    // 2. Check if .coasts.tmp exists for resuming polygon assembly
    bool resume_coasts = false;
    uint32_t coast_cap = 0, coast_cnt = 0; AccumWay* coasts = NULL;

    if (!resumed && !is_main_input_shp) {
        FILE* f_cst_in = fopen(coasts_tmp_name, "rb");
        if (f_cst_in) {
            int64_t safe_raw_sz = 0;
            // Read 4-byte coast_cnt to stay compatible with existing .coasts.tmp files
            if (fread(&safe_raw_sz, 8, 1, f_cst_in) == 1 && fread(&coast_cnt, 4, 1, f_cst_in) == 1) {
                SetStatus("Resuming Polygon Assembly from %s (%u ways)...", coasts_tmp_name, coast_cnt);
                
                f_raw = fopen(raw_tmp_name, "rb");
                if (f_raw) {
                    uint32_t r_cap = 65536; WayNode* wnodes = malloc(r_cap * sizeof(WayNode));
                    while (FTELL64(f_raw) < safe_raw_sz) {
                        uint64_t offset = FTELL64(f_raw); uint32_t total_len;
                        if (fread(&total_len, 4, 1, f_raw) != 1) break;
                        uint8_t fc, area_flag; uint16_t nlen, plen;
                        fread(&fc, 1, 1, f_raw); fread(&area_flag, 1, 1, f_raw); fread(&nlen, 2, 1, f_raw);
                        if (nlen > 0) FSEEK64(f_raw, nlen, SEEK_CUR); fread(&plen, 2, 1, f_raw);
                        if (plen > 0) FSEEK64(f_raw, plen, SEEK_CUR);
                        uint32_t ref_count; fread(&ref_count, 4, 1, f_raw);
                        if (ref_count > r_cap) { r_cap = ref_count + 16384; wnodes = realloc(wnodes, r_cap * sizeof(WayNode)); }
                        fread(wnodes, sizeof(WayNode), ref_count, f_raw);
                        double min_lon = wnodes[0].lon, max_lon = wnodes[0].lon, min_lat = wnodes[0].lat, max_lat = wnodes[0].lat;
                        for(uint32_t i=1; i<ref_count; i++) {
                            if(wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if(wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                            if(wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if(wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                        }
                        double cx = (min_lon + max_lon) / 2.0, cy = (min_lat + max_lat) / 2.0;
                        int32_t c_ix = (int32_t)((cx + 180.0) / 360.0 * 0x3FFFFFFF), c_iy = (int32_t)((cy + 90.0) / 180.0 * 0x3FFFFFFF);
                        if (written_feats >= feature_capacity) { feature_capacity = feature_capacity < 4000000 ? feature_capacity * 2 : feature_capacity + 2000000; sort_entries = realloc(sort_entries, feature_capacity * sizeof(SortEntry)); }
                        sort_entries[written_feats].morton = EncodeMorton(c_ix, c_iy); sort_entries[written_feats].offset = offset; written_feats++;
                    }
                    free(wnodes);
                    fclose(f_raw);
                }

                FILE* f_old = fopen(raw_tmp_name, "rb"); FILE* f_new = fopen("temp_truncate.tmp", "wb");
                if (f_old && f_new) {
                    char* buf = malloc(65536); int64_t remain = safe_raw_sz;
                    while (remain > 0) {
                        size_t to_read = remain < 65536 ? (size_t)remain : 65536;
                        size_t read_bytes = fread(buf, 1, to_read, f_old);
                        if (read_bytes == 0) break;
                        fwrite(buf, 1, read_bytes, f_new); remain -= read_bytes;
                    }
                    free(buf); fclose(f_old); fclose(f_new);
                    remove(raw_tmp_name); rename("temp_truncate.tmp", raw_tmp_name);
                }
                f_raw = fopen(raw_tmp_name, "ab");

                // Stream closed rings directly to disk so we don't blow out the 32-bit RAM limit on 6.9M polygons!
                uint32_t open_cnt = 0;
                coast_cap = 65536;
                coasts = malloc(coast_cap * sizeof(AccumWay));
                uint32_t r_cap = 65536; WayNode* tmp_nodes = malloc(r_cap * sizeof(WayNode));

                for (uint32_t i = 0; i < coast_cnt; i++) {
                    uint8_t nlen, plen, fc;
                    char name_buf[256] = {0}, pcode_buf[256] = {0};
                    if (fread(&nlen, 1, 1, f_cst_in) != 1) break;
                    if (nlen) { fread(name_buf, 1, nlen, f_cst_in); name_buf[nlen] = 0; }
                    if (fread(&plen, 1, 1, f_cst_in) != 1) break;
                    if (plen) { fread(pcode_buf, 1, plen, f_cst_in); pcode_buf[plen] = 0; }
                    uint32_t cnt = 0;
                    if (fread(&cnt, 4, 1, f_cst_in) != 1) break;
                    if (cnt > r_cap) { r_cap = cnt + 16384; tmp_nodes = realloc(tmp_nodes, r_cap * sizeof(WayNode)); }
                    if (fread(tmp_nodes, sizeof(WayNode), cnt, f_cst_in) != cnt) break;
                    if (fread(&fc, 1, 1, f_cst_in) != 1) break;

                    if (cnt < 2) continue;

                    double dx = tmp_nodes[0].lon - tmp_nodes[cnt-1].lon;
                    double dy = tmp_nodes[0].lat - tmp_nodes[cnt-1].lat;
                    bool is_closed = (fabs(dx) < 1e-6 && fabs(dy) < 1e-6);

                    // Only open coastline segments need to be held in RAM for Pass 4b merging
                    if (is_closed || fc != CLASS_COASTLINE) {
                        if (!is_closed && cnt + 1 <= r_cap) {
                            tmp_nodes[cnt] = tmp_nodes[0];
                            cnt++;
                        } else if (is_closed) {
                            tmp_nodes[cnt-1] = tmp_nodes[0];
                        }
                        uint8_t out_fc = (fc == CLASS_COASTLINE) ? CLASS_LAND : fc;
                        double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                        for (uint32_t j = 0; j < cnt; j++) {
                            if (tmp_nodes[j].lon < min_lon) min_lon = tmp_nodes[j].lon; if (tmp_nodes[j].lon > max_lon) max_lon = tmp_nodes[j].lon;
                            if (tmp_nodes[j].lat < min_lat) min_lat = tmp_nodes[j].lat; if (tmp_nodes[j].lat > max_lat) max_lat = tmp_nodes[j].lat;
                        }
                        bool in_bbox = true;
                        if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                        if (!g_CfgSpecs && (max_lon - min_lon < 0.005) && (max_lat - min_lat < 0.005)) in_bbox = false;
                        if (in_bbox) {
                            WriteRawFeature(f_raw, tmp_nodes, cnt, true, nlen ? name_buf : NULL, plen ? pcode_buf : NULL, out_fc, &sort_entries, &written_feats, &feature_capacity);
                        }
                    } else {
                        if (open_cnt >= coast_cap) {
                            coast_cap = coast_cap < 500000 ? coast_cap * 2 : coast_cap + 250000;
                            coasts = realloc(coasts, coast_cap * sizeof(AccumWay));
                        }
                        coasts[open_cnt].name = nlen ? strcpy(malloc(nlen + 1), name_buf) : NULL;
                        coasts[open_cnt].pcode = plen ? strcpy(malloc(plen + 1), pcode_buf) : NULL;
                        coasts[open_cnt].count = cnt;
                        coasts[open_cnt].nodes = malloc(cnt * sizeof(WayNode));
                        for (uint32_t k = 0; k < cnt; k++) {
                            coasts[open_cnt].nodes[k].id = 0;
                            coasts[open_cnt].nodes[k].lat = tmp_nodes[k].lat;
                            coasts[open_cnt].nodes[k].lon = tmp_nodes[k].lon;
                        }
                        coasts[open_cnt].fc = fc;
                        coasts[open_cnt].merged = false;
                        open_cnt++;
                    }
                }
                free(tmp_nodes);
                fclose(f_cst_in);
                coast_cnt = open_cnt;
                resume_coasts = true;
            } else {
                fclose(f_cst_in);
            }
        }
    }

    if (!resumed) {
        if (is_main_input_shp) {
            SetStatus("Pass 1-4: Extracting ESRI Shapefile geometries...");
            f_raw = fopen(raw_tmp_name, "wb"); if (!f_raw) return 1;
            ProcessShapefile(in_filename, f_raw, &sort_entries, &written_feats, &feature_capacity, has_bbox, b_minLon, b_minLat, b_maxLon, b_maxLat);
            fwrite(&EOF_MARKER, 4, 1, f_raw);
            fclose(f_raw); f_raw = fopen(raw_tmp_name, "rb");
        } else {
            if (!resume_coasts) {
                char* current_ways = ways_tmp_a;
                char* next_ways = ways_tmp_b;

                bool has_resolved_ways = (GetFileSize64(ways_resolved_name) > 0);
                if (has_resolved_ways) {
                    SetStatus("Found %s! Resuming at Pass 4...", ways_resolved_name);
                    current_ways = ways_resolved_name;
                } else {
                    bool has_ways_a = (GetFileSize64(ways_tmp_a) > 0);
                    bool has_ways_b = (GetFileSize64(ways_tmp_b) > 0);
                    bool has_nodes  = (GetFileSize64(nodes_tmp_name) > 0);

                    if (!has_ways_a && has_ways_b) {
                        current_ways = ways_tmp_b;
                        next_ways = ways_tmp_a;
                        has_ways_a = true;
                    }

                    for (int extract_mode = 1; extract_mode <= 2; extract_mode++) {
                        if (extract_mode == 1 && has_ways_a) {
                            SetStatus("Pass 1: Reusing existing ways temp file...");
                            continue;
                        }
                        if (extract_mode == 2 && has_nodes) {
                            SetStatus("Pass 2: Reusing existing nodes temp file...");
                            continue;
                        }

                        FILE* tmpOut = NULL; char* io_buf = malloc(IO_BUF_SIZE);
                        if (extract_mode == 1) { SetStatus("Pass 1: Streaming Filtered Ways to Disk..."); tmpOut = fopen(current_ways, "wb"); }
                        else { SetStatus("Pass 2: Streaming All Nodes to Disk..."); tmpOut = fopen(nodes_tmp_name, "wb"); }
                        if (tmpOut && io_buf) setvbuf(tmpOut, io_buf, _IOFBF, IO_BUF_SIZE);

                        FILE* file = fopen(in_filename, "rb");
                        if (!file) { if (tmpOut) fclose(tmpOut); if (io_buf) free(io_buf); return 1; }

                        char* read_buf = malloc(IO_BUF_SIZE);
                        if (read_buf) setvbuf(file, read_buf, _IOFBF, IO_BUF_SIZE);
                        
                        while (!feof(file)) {
                            uint32_t header_len_be = 0; if (fread(&header_len_be, 1, 4, file) != 4) break;
                            uint32_t header_len = SwapEndian32(header_len_be); if (header_len > 64 * 1024) break; 
                            uint8_t* header_buf = (uint8_t*)malloc(header_len); if (fread(header_buf, 1, header_len, file) != header_len) { free(header_buf); break; }

                            const uint8_t* ptr = header_buf; const uint8_t* end = header_buf + header_len;
                            char blob_type[32] = {0}; uint64_t datasize = 0;
                            while (ptr < end) {
                                uint64_t tag_wire = ReadVarint(&ptr, end);
                                if ((tag_wire >> 3) == 1) {
                                    uint64_t str_len = ReadVarint(&ptr, end); size_t cp_len = str_len < 31 ? (size_t)str_len : 31;
                                    memcpy(blob_type, ptr, cp_len); blob_type[cp_len] = '\0'; ptr += str_len;
                                } else if ((tag_wire >> 3) == 3) datasize = ReadVarint(&ptr, end); else SkipProtobufField(&ptr, end, tag_wire & 7);
                            }
                            free(header_buf);
                            
                            if (datasize > 64 * 1024 * 1024) break;
                            uint8_t* blob_buf = (uint8_t*)malloc(datasize); if (fread(blob_buf, 1, datasize, file) != datasize) { free(blob_buf); break; }

                            if (strcmp(blob_type, "OSMData") == 0) {
                                ptr = blob_buf; end = blob_buf + datasize;
                                uint64_t raw_size = 0, zlib_size = 0, raw_len = 0; 
                                uint8_t *zlib_data = NULL, *raw_data = NULL;

                                while (ptr < end) {
                                    uint64_t tag_wire = ReadVarint(&ptr, end);
                                    if ((tag_wire >> 3) == 1) { raw_len = ReadVarint(&ptr, end); raw_data = (uint8_t*)ptr; ptr += raw_len; }
                                    else if ((tag_wire >> 3) == 2) raw_size = ReadVarint(&ptr, end);
                                    else if ((tag_wire >> 3) == 3) { zlib_size = ReadVarint(&ptr, end); zlib_data = (uint8_t*)ptr; ptr += zlib_size; } 
                                    else SkipProtobufField(&ptr, end, tag_wire & 7);
                                }

                                if (zlib_data && raw_size > 0) {
                                    uint8_t* uncompressed = (uint8_t*)malloc(raw_size); unsigned long dest_len = raw_size;
                                    if (mz_uncompress(uncompressed, &dest_len, zlib_data, zlib_size) == MZ_OK) ProcessPrimitiveBlock(uncompressed, dest_len, extract_mode, tmpOut);
                                    free(uncompressed);
                                } else if (raw_data && raw_len > 0) ProcessPrimitiveBlock(raw_data, raw_len, extract_mode, tmpOut);
                            }
                            free(blob_buf);
                        }
                        fclose(file); if (tmpOut) fclose(tmpOut); if (io_buf) free(io_buf); if (read_buf) free(read_buf);
                    }

                    SetStatus("Pass 3: Sequential Ping-Pong Resolution...");
                    FILE* fnodes = fopen(nodes_tmp_name, "rb"); if (!fnodes) return 1;
                    char* io_node_buf = malloc(IO_BUF_SIZE); if (io_node_buf) setvbuf(fnodes, io_node_buf, _IOFBF, IO_BUF_SIZE);

                    const uint64_t CHUNK_NODES = 15000000; 
                    PbfNode* chunk = malloc(CHUNK_NODES * sizeof(PbfNode));
                    if (!chunk) { fclose(fnodes); if (io_node_buf) free(io_node_buf); return 1; }

                    uint64_t nodes_read; int node_chunk_idx = 1;

                    while ((nodes_read = fread(chunk, sizeof(PbfNode), CHUNK_NODES, fnodes)) > 0) {
                        SetStatus("Pass 3: Resolving Node Chunk %d...", node_chunk_idx++);
                        qsort(chunk, nodes_read, sizeof(PbfNode), CompareNodes);

                        FILE* fways_in = fopen(current_ways, "rb"); FILE* fways_out = fopen(next_ways, "wb");
                        char* in_buf = malloc(IO_BUF_SIZE); char* out_buf = malloc(IO_BUF_SIZE);
                        if (fways_in && in_buf) setvbuf(fways_in, in_buf, _IOFBF, IO_BUF_SIZE);
                        if (fways_out && out_buf) setvbuf(fways_out, out_buf, _IOFBF, IO_BUF_SIZE);
                        if(!fways_in || !fways_out) { if (in_buf) free(in_buf); if (out_buf) free(out_buf); break; }

                        while (1) {
                            uint8_t fc; if (fread(&fc, 1, 1, fways_in) != 1) break;
                            uint16_t nlen; if (fread(&nlen, 2, 1, fways_in) != 1) break;
                            char* name = NULL; if (nlen > 0) { name = malloc(nlen + 1); if (name) { fread(name, 1, nlen, fways_in); name[nlen] = '\0'; } else FSEEK64(fways_in, nlen, SEEK_CUR); }
                            uint16_t plen; if (fread(&plen, 2, 1, fways_in) != 1) break;
                            char* pcode = NULL; if (plen > 0) { pcode = malloc(plen + 1); if (pcode) { fread(pcode, 1, plen, fways_in); pcode[plen] = '\0'; } else FSEEK64(fways_in, plen, SEEK_CUR); }

                            uint32_t ref_count; if (fread(&ref_count, 4, 1, fways_in) != 1) break;
                            if (ref_count == 0 || ref_count > 16000) { if (name) free(name); if (pcode) free(pcode); break; }

                            WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
                            if (!wnodes) { if (name) free(name); if (pcode) free(pcode); break; }
                            if (fread(wnodes, sizeof(WayNode), ref_count, fways_in) != ref_count) { if (name) free(name); if (pcode) free(pcode); free(wnodes); break; }

                            for (uint32_t i = 0; i < ref_count; i++) {
                                if (wnodes[i].id != 0xFFFFFFFFFFFFFFFFULL) {
                                    PbfNode* found = FindNode(chunk, nodes_read, wnodes[i].id);
                                    if (found) { wnodes[i].lat = found->lat * 1e-7; wnodes[i].lon = found->lon * 1e-7; wnodes[i].id = 0xFFFFFFFFFFFFFFFFULL; }
                                }
                            }

                            fwrite(&fc, 1, 1, fways_out); fwrite(&nlen, 2, 1, fways_out);
                            if (nlen > 0 && name) fwrite(name, 1, nlen, fways_out);
                            fwrite(&plen, 2, 1, fways_out);
                            if (plen > 0 && pcode) fwrite(pcode, 1, plen, fways_out);
                            fwrite(&ref_count, 4, 1, fways_out); fwrite(wnodes, sizeof(WayNode), ref_count, fways_out);

                            if (name) free(name); if (pcode) free(pcode); free(wnodes);
                        }
                        fclose(fways_in); fclose(fways_out);
                        if (in_buf) free(in_buf); if (out_buf) free(out_buf);
                        char* tmp = current_ways; current_ways = next_ways; next_ways = tmp;
                    }
                    free(chunk); fclose(fnodes); if (io_node_buf) free(io_node_buf);

                    remove(ways_resolved_name);
                    if (rename(current_ways, ways_resolved_name) == 0) {
                        current_ways = ways_resolved_name;
                    }
                }

                SetStatus("Pass 4: Extracting raw features to temporary file...");
                FILE* fways = fopen(current_ways, "rb"); f_raw = fopen(raw_tmp_name, "wb");
                char* io_final_in = malloc(IO_BUF_SIZE);
                if (fways && io_final_in) setvbuf(fways, io_final_in, _IOFBF, IO_BUF_SIZE);
                if (!fways || !f_raw) { if (io_final_in) free(io_final_in); return 1; }

                while (1) {
                    uint8_t fc; if (fread(&fc, 1, 1, fways) != 1) break;
                    uint16_t nlen; if (fread(&nlen, 2, 1, fways) != 1) break;
                    char* name = NULL; if (nlen > 0) { name = malloc(nlen + 1); if (name) { fread(name, 1, nlen, fways); name[nlen] = '\0'; } else FSEEK64(fways, nlen, SEEK_CUR); }
                    uint16_t plen; if (fread(&plen, 2, 1, fways) != 1) break;
                    char* pcode = NULL; if (plen > 0) { pcode = malloc(plen + 1); if (pcode) { fread(pcode, 1, plen, fways); pcode[plen] = '\0'; } else FSEEK64(fways, plen, SEEK_CUR); }
                    uint32_t ref_count; if (fread(&ref_count, 4, 1, fways) != 1) break;
                    if (ref_count == 0 || ref_count > 16000) { if (name) free(name); if (pcode) free(pcode); break; }

                    WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
                    if (!wnodes) { if (name) free(name); if (pcode) free(pcode); break; }
                    if (fread(wnodes, sizeof(WayNode), ref_count, fways) != ref_count) { if (name) free(name); if (pcode) free(pcode); free(wnodes); break; }

                    bool valid = true;
                    for (uint32_t i = 0; i < ref_count; i++) { if (wnodes[i].id != 0xFFFFFFFFFFFFFFFFULL) { valid = false; break; } }

                    if (valid && ref_count >= 2) {
                        if (fc == CLASS_COASTLINE || fc == CLASS_WATER || fc == CLASS_PARK || fc == CLASS_LAND) {
                            double dx = wnodes[0].lon - wnodes[ref_count-1].lon;
                            double dy = wnodes[0].lat - wnodes[ref_count-1].lat;
                            bool is_closed = (fabs(dx) < 1e-6 && fabs(dy) < 1e-6);

                            if (is_closed || fc != CLASS_COASTLINE) {
                                if (!is_closed) {
                                    WayNode* safe = realloc(wnodes, (ref_count + 1) * sizeof(WayNode));
                                    if (safe) { wnodes = safe; wnodes[ref_count] = wnodes[0]; ref_count++; }
                                } else {
                                    wnodes[ref_count-1] = wnodes[0];
                                }
                                uint8_t out_fc = (fc == CLASS_COASTLINE) ? CLASS_LAND : fc;
                                double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                                for (uint32_t i = 0; i < ref_count; i++) {
                                    if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                                    if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                                }
                                bool in_bbox = true;
                                if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                                if (!g_CfgSpecs && (max_lon - min_lon < 0.005) && (max_lat - min_lat < 0.005)) in_bbox = false;
                                if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, true, name, pcode, out_fc, &sort_entries, &written_feats, &feature_capacity);
                                if (name) free(name); if (pcode) free(pcode); free(wnodes);
                            } else {
                                if (coast_cnt >= coast_cap) {
                                    coast_cap = coast_cap == 0 ? 65536 : (coast_cap < 500000 ? coast_cap * 2 : coast_cap + 250000);
                                    coasts = realloc(coasts, coast_cap * sizeof(AccumWay));
                                }
                                coasts[coast_cnt].name = name; coasts[coast_cnt].pcode = pcode; coasts[coast_cnt].count = ref_count; coasts[coast_cnt].nodes = wnodes; coasts[coast_cnt].merged = false; coasts[coast_cnt].fc = fc; coast_cnt++;
                            }
                        } else {
                            double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                            for (uint32_t i = 0; i < ref_count; i++) {
                                if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                                if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                            }
                            bool in_bbox = true; if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                            if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, false, name, pcode, fc, &sort_entries, &written_feats, &feature_capacity);
                            if (name) free(name); if (pcode) free(pcode); if (wnodes) free(wnodes);
                        }
                    } else { if (name) free(name); if (pcode) free(pcode); free(wnodes); }
                }

                if (coast_cnt > 0) {
                    int64_t raw_noncoast_sz = FTELL64(f_raw);
                    FILE* f_cst_out = fopen(coasts_tmp_name, "wb");
                    if (f_cst_out) {
                        fwrite(&raw_noncoast_sz, 8, 1, f_cst_out); fwrite(&coast_cnt, 4, 1, f_cst_out);
                        for (uint32_t i = 0; i < coast_cnt; i++) {
                            uint8_t nlen = coasts[i].name ? (uint8_t)strlen(coasts[i].name) : 0;
                            uint8_t plen = coasts[i].pcode ? (uint8_t)strlen(coasts[i].pcode) : 0;
                            fwrite(&nlen, 1, 1, f_cst_out); if (nlen) fwrite(coasts[i].name, 1, nlen, f_cst_out);
                            fwrite(&plen, 1, 1, f_cst_out); if (plen) fwrite(coasts[i].pcode, 1, plen, f_cst_out);
                            fwrite(&coasts[i].count, 4, 1, f_cst_out); fwrite(coasts[i].nodes, sizeof(WayNode), coasts[i].count, f_cst_out); fwrite(&coasts[i].fc, 1, 1, f_cst_out);
                        }
                        fclose(f_cst_out);
                    }
                }
                fclose(fways); if (io_final_in) free(io_final_in);
            }

            if (coast_cnt > 0) {
                SetStatus("Pass 4b: Indexing & Assembling %u Open Coastline Ways...", coast_cnt);
                CoastEndPt* eps = malloc(coast_cnt * 2 * sizeof(CoastEndPt));
                bool merged = true;
                int pass_iter = 1;

                while (merged) {
                    merged = false;
                    uint32_t ep_cnt = 0;
                    for (uint32_t i = 0; i < coast_cnt; i++) {
                        if (coasts[i].merged || coasts[i].count < 2) continue;
                        WayNode s = coasts[i].nodes[0], e = coasts[i].nodes[coasts[i].count - 1];
                        if (fabs(s.lon - e.lon) < 1e-6 && fabs(s.lat - e.lat) < 1e-6) continue;
                        eps[ep_cnt].qx = (int32_t)floor(s.lon * 1e6 + 0.5);
                        eps[ep_cnt].qy = (int32_t)floor(s.lat * 1e6 + 0.5);
                        eps[ep_cnt].way_idx = i;
                        eps[ep_cnt].is_end = 0;
                        ep_cnt++;

                        eps[ep_cnt].qx = (int32_t)floor(e.lon * 1e6 + 0.5);
                        eps[ep_cnt].qy = (int32_t)floor(e.lat * 1e6 + 0.5);
                        eps[ep_cnt].way_idx = i;
                        eps[ep_cnt].is_end = 1;
                        ep_cnt++;
                    }
                    if (ep_cnt < 2) break;

                    SetStatus("Pass 4b: Polygon Assembly Pass %d (%u open endpoints)...", pass_iter++, ep_cnt);
                    qsort(eps, ep_cnt, sizeof(CoastEndPt), CompareCoastEndPt);

                    for (uint32_t k = 0; k < ep_cnt - 1; k++) {
                        if (abs(eps[k].qx - eps[k+1].qx) <= 1 && abs(eps[k].qy - eps[k+1].qy) <= 1) {
                            uint32_t i = eps[k].way_idx;
                            uint32_t j = eps[k+1].way_idx;
                            if (i == j || coasts[i].merged || coasts[j].merged) continue;

                            WayNode start_pt = coasts[i].nodes[0], end_pt = coasts[i].nodes[coasts[i].count - 1];
                            WayNode j_start = coasts[j].nodes[0], j_end = coasts[j].nodes[coasts[j].count - 1];

                            bool match_end_start = (fabs(end_pt.lon - j_start.lon) < 1e-6 && fabs(end_pt.lat - j_start.lat) < 1e-6);
                            bool match_end_end   = (fabs(end_pt.lon - j_end.lon) < 1e-6   && fabs(end_pt.lat - j_end.lat) < 1e-6);
                            bool match_start_end = (fabs(start_pt.lon - j_end.lon) < 1e-6 && fabs(start_pt.lat - j_end.lat) < 1e-6);
                            bool match_start_start = (fabs(start_pt.lon - j_start.lon) < 1e-6 && fabs(start_pt.lat - j_start.lat) < 1e-6);

                            if (match_end_start || match_end_end || match_start_end || match_start_start) {
                                if (match_start_end || match_start_start) {
                                    for (uint32_t m = 0; m < coasts[i].count / 2; m++) {
                                        WayNode tmp = coasts[i].nodes[m];
                                        coasts[i].nodes[m] = coasts[i].nodes[coasts[i].count - 1 - m];
                                        coasts[i].nodes[coasts[i].count - 1 - m] = tmp;
                                    }
                                    if (match_start_end) match_end_end = true;
                                    if (match_start_start) match_end_start = true;
                                }

                                uint32_t new_count = coasts[i].count + coasts[j].count - 1;
                                WayNode* safe = realloc(coasts[i].nodes, new_count * sizeof(WayNode));
                                if (safe) {
                                    coasts[i].nodes = safe;
                                    if (match_end_start) {
                                        memcpy(coasts[i].nodes + coasts[i].count, coasts[j].nodes + 1, (coasts[j].count - 1) * sizeof(WayNode));
                                    } else if (match_end_end) {
                                        for (uint32_t m = 1; m < coasts[j].count; m++) {
                                            coasts[i].nodes[coasts[i].count + m - 1] = coasts[j].nodes[coasts[j].count - 1 - m];
                                        }
                                    }
                                    coasts[i].count = new_count;
                                    coasts[j].merged = true;
                                    free(coasts[j].nodes); coasts[j].nodes = NULL;
                                    if (coasts[j].name) { free(coasts[j].name); coasts[j].name = NULL; }
                                    if (coasts[j].pcode) { free(coasts[j].pcode); coasts[j].pcode = NULL; }
                                    merged = true;
                                    k++;
                                }
                            }
                        }
                    }
                }
                free(eps);

                for (uint32_t i = 0; i < coast_cnt; i++) {
                    if (coasts[i].merged) { if (coasts[i].name) free(coasts[i].name); if (coasts[i].pcode) free(coasts[i].pcode); if (coasts[i].nodes) free(coasts[i].nodes); continue; }
                    WayNode* wnodes = coasts[i].nodes; uint32_t ref_count = coasts[i].count; uint8_t current_fc = coasts[i].fc;

                    double dx = wnodes[0].lon - wnodes[ref_count-1].lon;
                    double dy = wnodes[0].lat - wnodes[ref_count-1].lat;
                    double dist = sqrt(dx*dx + dy*dy);
                    bool is_area = true;

                    if (dist <= 0.000001) { 
                        wnodes[ref_count-1] = wnodes[0]; 
                    } else {
                        WayNode* safe = realloc(wnodes, (ref_count + 1) * sizeof(WayNode)); 
                        if (safe) { 
                            wnodes = safe; 
                            wnodes[ref_count] = wnodes[0]; 
                            ref_count++; 
                        } 
                    }

                    if (current_fc == CLASS_COASTLINE) {
                        current_fc = CLASS_LAND;
                    }

                    double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                    for (uint32_t j = 0; j < ref_count; j++) {
                        if (wnodes[j].lon < min_lon) min_lon = wnodes[j].lon; if (wnodes[j].lon > max_lon) max_lon = wnodes[j].lon;
                        if (wnodes[j].lat < min_lat) min_lat = wnodes[j].lat; if (wnodes[j].lat > max_lat) max_lat = wnodes[j].lat;
                    }
                    bool in_bbox = true; 
                    if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                    if (!g_CfgSpecs && (max_lon - min_lon < 0.005) && (max_lat - min_lat < 0.005)) in_bbox = false;
                    if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, is_area, coasts[i].name, coasts[i].pcode, current_fc, &sort_entries, &written_feats, &feature_capacity);
                    if (coasts[i].name) free(coasts[i].name); if (coasts[i].pcode) free(coasts[i].pcode); free(wnodes);
                } free(coasts);
            }
            
            if (strlen(g_ShapefileFile) > 0) {
                SetStatus("Injecting Shapefile Coastlines from %s...", g_ShapefileFile);
                ProcessShapefile(g_ShapefileFile, f_raw, &sort_entries, &written_feats, &feature_capacity, has_bbox, b_minLon, b_minLat, b_maxLon, b_maxLat);
            }

            fwrite(&EOF_MARKER, 4, 1, f_raw);
            fclose(f_raw); f_raw = fopen(raw_tmp_name, "rb");
        }
    }

    SetStatus("Pass 5: Spatial Clustering (Morton Z-Curve Sorting)...");
    qsort(sort_entries, written_feats, sizeof(SortEntry), CompareSortEntry);

    SetStatus("Pass 6: High-Res QJMC Block Compression...");
    FILE* f_qjmc = fopen(out_filename, "wb"); if (!f_qjmc) return 1;
    fwrite("QJMC", 1, 4, f_qjmc);

    uint32_t flush_threshold = 256 * 1024; 
    uint32_t buf_cap = flush_threshold + (1024 * 1024 * 2); 
    uint8_t* uncomp_buf = malloc(buf_cap); uint8_t* comp_buf = malloc(lzav_compress_bound(buf_cap));

    uint32_t meta_cap = 10000; ChunkMetaDisk* meta = malloc(meta_cap * sizeof(ChunkMetaDisk)); uint32_t chunk_idx = 0;
    uint32_t feats_cap = 30000; PFeat* feats_buf = malloc(feats_cap * sizeof(PFeat));
    uint32_t pts_cap = 600000; float* pts_buf = malloc(pts_cap * sizeof(float));
    uint32_t str_cap = 600000; char* str_buf = malloc(str_cap);
    
    uint32_t f_cnt = 0, p_cnt = 0, s_len = 1; str_buf[0] = 0;
    double c_min_lon = 180, c_max_lon = -180, c_min_lat = 90, c_max_lat = -90;
    uint32_t raw_cap = 1024 * 1024; uint8_t* raw_buf = malloc(raw_cap);

    for (uint64_t i = 0; i < written_feats; i++) {
        FSEEK64(f_raw, sort_entries[i].offset, SEEK_SET); uint32_t total_len; fread(&total_len, 4, 1, f_raw);
        if (total_len > raw_cap) { raw_cap = total_len + 65536; raw_buf = realloc(raw_buf, raw_cap); } fread(raw_buf, 1, total_len, f_raw);
        
        uint8_t* rptr = raw_buf; uint8_t fc = *rptr++; uint8_t is_area = *rptr++;
        uint16_t nlen = *(uint16_t*)rptr; rptr += 2; char* name = nlen ? (char*)rptr : NULL; rptr += nlen;
        uint16_t plen = *(uint16_t*)rptr; rptr += 2; char* pcode = plen ? (char*)rptr : NULL; rptr += plen;
        uint32_t orig_ref_count = *(uint32_t*)rptr; rptr += 4; WayNode* orig_wnodes = (WayNode*)rptr;

        int keep_pct = 100;
        if (fc == CLASS_LAND) keep_pct = g_SimpLand;
        else if (fc == CLASS_COASTLINE) keep_pct = g_SimpCoast;
        else if (fc == CLASS_HWY_MAIN) keep_pct = g_SimpHwyMain;
        else if (fc == CLASS_HWY_MINOR) keep_pct = g_SimpHwyMin;
        else if (fc == CLASS_WATER) keep_pct = g_SimpWater;
        else if (fc == CLASS_PARK) keep_pct = g_SimpPark;

        WayNode* wnodes = malloc(orig_ref_count * sizeof(WayNode));
        uint32_t ref_count = DecimateNodes(orig_wnodes, orig_ref_count, wnodes, keep_pct);

        if (g_CfgNoRoads && (fc == CLASS_LAND || fc == CLASS_COASTLINE)) {
            WayNode* tmp = malloc(ref_count * sizeof(WayNode));
            ref_count = DecimateNodes(wnodes, ref_count, tmp, g_SimpNoRoads);
            free(wnodes);
            wnodes = tmp;
        }

        uint32_t required_uncomp = 12 + (f_cnt + 1) * sizeof(PFeat) + (p_cnt + ref_count * 2) * sizeof(float) + s_len + nlen + plen + 2;
        
        if (f_cnt > 0 && required_uncomp > flush_threshold) {
            uint32_t total_uncomp = 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float) + s_len;
            memcpy(uncomp_buf, &f_cnt, 4); memcpy(uncomp_buf + 4, &p_cnt, 4); memcpy(uncomp_buf + 8, &s_len, 4);
            memcpy(uncomp_buf + 12, feats_buf, f_cnt * sizeof(PFeat));
            memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat), pts_buf, p_cnt * sizeof(float));
            memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float), str_buf, s_len);

            int comp_size = lzav_compress_default(uncomp_buf, comp_buf, total_uncomp, lzav_compress_bound(buf_cap));
            if (chunk_idx >= meta_cap) { meta_cap *= 2; meta = realloc(meta, meta_cap * sizeof(ChunkMetaDisk)); }
            meta[chunk_idx].min_lon = c_min_lon; meta[chunk_idx].max_lon = c_max_lon; meta[chunk_idx].min_lat = c_min_lat; meta[chunk_idx].max_lat = c_max_lat;
            meta[chunk_idx].offset = FTELL64(f_qjmc); meta[chunk_idx].comp_size = comp_size; meta[chunk_idx].uncomp_size = total_uncomp;
            
            fwrite(comp_buf, 1, comp_size, f_qjmc); chunk_idx++;
            f_cnt = 0; p_cnt = 0; s_len = 1; str_buf[0] = 0; c_min_lon = 180; c_max_lon = -180; c_min_lat = 90; c_max_lat = -90;
            required_uncomp = 12 + (f_cnt + 1) * sizeof(PFeat) + (p_cnt + ref_count * 2) * sizeof(float) + s_len + nlen + plen + 2;
        }

        if (required_uncomp > buf_cap) { buf_cap = required_uncomp + (1024 * 1024 * 2); uncomp_buf = realloc(uncomp_buf, buf_cap); comp_buf = realloc(comp_buf, lzav_compress_bound(buf_cap)); }
        if (f_cnt + 1 > feats_cap) { feats_cap = f_cnt + 10000; feats_buf = realloc(feats_buf, feats_cap * sizeof(PFeat)); }
        if (p_cnt + ref_count * 2 > pts_cap) { pts_cap = p_cnt + (ref_count * 2) + 100000; pts_buf = realloc(pts_buf, pts_cap * sizeof(float)); }
        if (s_len + nlen + plen + 2 > str_cap) { str_cap = s_len + nlen + plen + 100000; str_buf = realloc(str_buf, str_cap); }

        uint32_t n_off = 0, p_off = 0;
        if (nlen) { n_off = s_len; memcpy(str_buf + s_len, name, nlen); str_buf[s_len + nlen] = 0; s_len += nlen + 1; }
        if (plen) { p_off = s_len; memcpy(str_buf + s_len, pcode, plen); str_buf[s_len + plen] = 0; s_len += plen + 1; }

        feats_buf[f_cnt].name_off = n_off; feats_buf[f_cnt].pcode_off = p_off; feats_buf[f_cnt].pt_off = p_cnt; feats_buf[f_cnt].pt_cnt = ref_count;
        feats_buf[f_cnt].fc = fc; feats_buf[f_cnt].is_area = is_area; feats_buf[f_cnt].pad = 0;

        float m_lon = 180, m_lat = 90, x_lon = -180, x_lat = -90;
        for (uint32_t k = 0; k < ref_count; k++) {
            float lon = (float)wnodes[k].lon, lat = (float)wnodes[k].lat;
            pts_buf[p_cnt++] = lon; pts_buf[p_cnt++] = lat;
            if (lon < m_lon) m_lon = lon; if (lon > x_lon) x_lon = lon;
            if (lat < m_lat) m_lat = lat; if (lat > x_lat) x_lat = lat;
            if (lon < c_min_lon) c_min_lon = lon; if (lon > c_max_lon) c_max_lon = lon;
            if (lat < c_min_lat) c_min_lat = lat; if (lat > c_max_lat) c_max_lat = lat;
        } 
        feats_buf[f_cnt].min_lon = m_lon; feats_buf[f_cnt].max_lon = x_lon;
        feats_buf[f_cnt].min_lat = m_lat; feats_buf[f_cnt].max_lat = x_lat;
        f_cnt++;
        
        free(wnodes);
    }
    
    if (f_cnt > 0) {
        uint32_t total_uncomp = 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float) + s_len;
        memcpy(uncomp_buf, &f_cnt, 4); memcpy(uncomp_buf + 4, &p_cnt, 4); memcpy(uncomp_buf + 8, &s_len, 4);
        memcpy(uncomp_buf + 12, feats_buf, f_cnt * sizeof(PFeat)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat), pts_buf, p_cnt * sizeof(float)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float), str_buf, s_len);
        int comp_size = lzav_compress_default(uncomp_buf, comp_buf, total_uncomp, lzav_compress_bound(buf_cap));
        if (chunk_idx >= meta_cap) { meta_cap *= 2; meta = realloc(meta, meta_cap * sizeof(ChunkMetaDisk)); }
        meta[chunk_idx].min_lon = c_min_lon; meta[chunk_idx].max_lon = c_max_lon; meta[chunk_idx].min_lat = c_min_lat; meta[chunk_idx].max_lat = c_max_lat;
        meta[chunk_idx].offset = FTELL64(f_qjmc); meta[chunk_idx].comp_size = comp_size; meta[chunk_idx].uncomp_size = total_uncomp;
        fwrite(comp_buf, 1, comp_size, f_qjmc); chunk_idx++;
    }

    uint64_t high_dir_off = FTELL64(f_qjmc);
    fwrite(meta, sizeof(ChunkMetaDisk), chunk_idx, f_qjmc);
    uint32_t high_cnt = chunk_idx;
    
    SetStatus("Pass 7: Generating Decimated LOD Overview (QJM2)...");
    uint32_t over_cap = 10000; ChunkMetaDisk* over_meta = malloc(over_cap * sizeof(ChunkMetaDisk)); uint32_t over_cnt = 0;
    
    f_cnt = 0; p_cnt = 0; s_len = 1; str_buf[0] = 0;
    c_min_lon = 180; c_max_lon = -180; c_min_lat = 90; c_max_lat = -90;

    for (uint64_t i = 0; i < written_feats; i++) {
        FSEEK64(f_raw, sort_entries[i].offset, SEEK_SET); uint32_t total_len; fread(&total_len, 4, 1, f_raw);
        if (total_len > raw_cap) { raw_cap = total_len + 65536; raw_buf = realloc(raw_buf, raw_cap); } fread(raw_buf, 1, total_len, f_raw);
        
        uint8_t* rptr = raw_buf; uint8_t fc = *rptr++; uint8_t is_area = *rptr++;
        uint16_t nlen = *(uint16_t*)rptr; rptr += 2; char* name = nlen ? (char*)rptr : NULL; rptr += nlen;
        uint16_t plen = *(uint16_t*)rptr; rptr += 2; char* pcode = plen ? (char*)rptr : NULL; rptr += plen;
        uint32_t orig_ref_count = *(uint32_t*)rptr; rptr += 4; WayNode* orig_wnodes = (WayNode*)rptr;

        if (fc == CLASS_HWY_MINOR || fc == CLASS_UNKNOWN) continue;

        int keep_pct = 100;
        if (fc == CLASS_LAND) keep_pct = g_SimpLand;
        else if (fc == CLASS_COASTLINE) keep_pct = g_SimpCoast;
        else if (fc == CLASS_HWY_MAIN) keep_pct = g_SimpHwyMain;
        else if (fc == CLASS_HWY_MINOR) keep_pct = g_SimpHwyMin;
        else if (fc == CLASS_WATER) keep_pct = g_SimpWater;
        else if (fc == CLASS_PARK) keep_pct = g_SimpPark;

        WayNode* wnodes = malloc(orig_ref_count * sizeof(WayNode));
        uint32_t ref_count = DecimateNodes(orig_wnodes, orig_ref_count, wnodes, keep_pct);
        
        double f_min_lon = wnodes[0].lon, f_max_lon = wnodes[0].lon, f_min_lat = wnodes[0].lat, f_max_lat = wnodes[0].lat;
        for(uint32_t j=1; j<ref_count; j++) {
            if(wnodes[j].lon < f_min_lon) f_min_lon = wnodes[j].lon; if(wnodes[j].lon > f_max_lon) f_max_lon = wnodes[j].lon;
            if(wnodes[j].lat < f_min_lat) f_min_lat = wnodes[j].lat; if(wnodes[j].lat > f_max_lat) f_max_lat = wnodes[j].lat;
        }
        if (is_area && (f_max_lon - f_min_lon < 0.02) && (f_max_lat - f_min_lat < 0.02)) {
            free(wnodes);
            continue;
        }

        WayNode* dec_nodes = malloc(ref_count * sizeof(WayNode)); uint32_t dec_cnt = 0; dec_nodes[dec_cnt++] = wnodes[0];
        for (uint32_t k = 1; k < ref_count - 1; k++) {
            double dx = wnodes[k].lon - dec_nodes[dec_cnt-1].lon, dy = wnodes[k].lat - dec_nodes[dec_cnt-1].lat;
            if (dx*dx + dy*dy > 0.0001) dec_nodes[dec_cnt++] = wnodes[k]; 
        }
        if (ref_count > 1) {
            double dx = wnodes[ref_count-1].lon - dec_nodes[dec_cnt-1].lon, dy = wnodes[ref_count-1].lat - dec_nodes[dec_cnt-1].lat;
            if (dx*dx + dy*dy > 0.0000001) dec_nodes[dec_cnt++] = wnodes[ref_count-1];
        }

        WayNode* final_dec_nodes = malloc(dec_cnt * sizeof(WayNode));
        dec_cnt = DecimateNodes(dec_nodes, dec_cnt, final_dec_nodes, g_SimpLowPoly);
        free(dec_nodes);
        dec_nodes = final_dec_nodes;

        if (g_CfgNoRoads && (fc == CLASS_LAND || fc == CLASS_COASTLINE)) {
            WayNode* tmp = malloc(dec_cnt * sizeof(WayNode));
            dec_cnt = DecimateNodes(dec_nodes, dec_cnt, tmp, g_SimpNoRoads);
            free(dec_nodes);
            dec_nodes = tmp;
        }

        uint32_t required_uncomp = 12 + (f_cnt + 1) * sizeof(PFeat) + (p_cnt + dec_cnt * 2) * sizeof(float) + s_len + nlen + plen + 2;
        if (f_cnt > 0 && required_uncomp > flush_threshold) {
            uint32_t total_uncomp = 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float) + s_len;
            memcpy(uncomp_buf, &f_cnt, 4); memcpy(uncomp_buf + 4, &p_cnt, 4); memcpy(uncomp_buf + 8, &s_len, 4);
            memcpy(uncomp_buf + 12, feats_buf, f_cnt * sizeof(PFeat)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat), pts_buf, p_cnt * sizeof(float)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float), str_buf, s_len);

            int comp_size = lzav_compress_default(uncomp_buf, comp_buf, total_uncomp, lzav_compress_bound(buf_cap));
            if (over_cnt >= over_cap) { over_cap *= 2; over_meta = realloc(over_meta, over_cap * sizeof(ChunkMetaDisk)); }
            over_meta[over_cnt].min_lon = c_min_lon; over_meta[over_cnt].max_lon = c_max_lon; over_meta[over_cnt].min_lat = c_min_lat; over_meta[over_cnt].max_lat = c_max_lat;
            over_meta[over_cnt].offset = FTELL64(f_qjmc); over_meta[over_cnt].comp_size = comp_size; over_meta[over_cnt].uncomp_size = total_uncomp;
            
            fwrite(comp_buf, 1, comp_size, f_qjmc); over_cnt++;
            f_cnt = 0; p_cnt = 0; s_len = 1; str_buf[0] = 0; c_min_lon = 180; c_max_lon = -180; c_min_lat = 90; c_max_lat = -90;
            required_uncomp = 12 + (f_cnt + 1) * sizeof(PFeat) + (p_cnt + dec_cnt * 2) * sizeof(float) + s_len + nlen + plen + 2;
        }

        if (required_uncomp > buf_cap) { buf_cap = required_uncomp + (1024 * 1024 * 2); uncomp_buf = realloc(uncomp_buf, buf_cap); comp_buf = realloc(comp_buf, lzav_compress_bound(buf_cap)); }
        if (f_cnt + 1 > feats_cap) { feats_cap = f_cnt + 10000; feats_buf = realloc(feats_buf, feats_cap * sizeof(PFeat)); }
        if (p_cnt + dec_cnt * 2 > pts_cap) { pts_cap = p_cnt + (dec_cnt * 2) + 100000; pts_buf = realloc(pts_buf, pts_cap * sizeof(float)); }
        if (s_len + nlen + plen + 2 > str_cap) { str_cap = s_len + nlen + plen + 100000; str_buf = realloc(str_buf, str_cap); }

        uint32_t n_off = 0, p_off = 0;
        if (nlen) { n_off = s_len; memcpy(str_buf + s_len, name, nlen); str_buf[s_len + nlen] = 0; s_len += nlen + 1; }
        if (plen) { p_off = s_len; memcpy(str_buf + s_len, pcode, plen); str_buf[s_len + plen] = 0; s_len += plen + 1; }

        feats_buf[f_cnt].name_off = n_off; feats_buf[f_cnt].pcode_off = p_off; feats_buf[f_cnt].pt_off = p_cnt; feats_buf[f_cnt].pt_cnt = dec_cnt;
        feats_buf[f_cnt].fc = fc; feats_buf[f_cnt].is_area = is_area; feats_buf[f_cnt].pad = 0;

        float m_lon = 180, m_lat = 90, x_lon = -180, x_lat = -90;
        for (uint32_t k = 0; k < dec_cnt; k++) {
            float lon = (float)dec_nodes[k].lon, lat = (float)dec_nodes[k].lat;
            pts_buf[p_cnt++] = lon; pts_buf[p_cnt++] = lat;
            if (lon < m_lon) m_lon = lon; if (lon > x_lon) x_lon = lon;
            if (lat < m_lat) m_lat = lat; if (lat > x_lat) x_lat = lat;
            if (lon < c_min_lon) c_min_lon = lon; if (lon > c_max_lon) c_max_lon = lon;
            if (lat < c_min_lat) c_min_lat = lat; if (lat > c_max_lat) c_max_lat = lat;
        } 
        feats_buf[f_cnt].min_lon = m_lon; feats_buf[f_cnt].max_lon = x_lon;
        feats_buf[f_cnt].min_lat = m_lat; feats_buf[f_cnt].max_lat = x_lat;
        f_cnt++; 
        free(dec_nodes);
        free(wnodes);
    }
    
    if (f_cnt > 0) {
        uint32_t total_uncomp = 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float) + s_len;
        memcpy(uncomp_buf, &f_cnt, 4); memcpy(uncomp_buf + 4, &p_cnt, 4); memcpy(uncomp_buf + 8, &s_len, 4);
        memcpy(uncomp_buf + 12, feats_buf, f_cnt * sizeof(PFeat)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat), pts_buf, p_cnt * sizeof(float)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float), str_buf, s_len);
        int comp_size = lzav_compress_default(uncomp_buf, comp_buf, total_uncomp, lzav_compress_bound(buf_cap));
        if (over_cnt >= over_cap) { over_cap *= 2; over_meta = realloc(over_meta, over_cap * sizeof(ChunkMetaDisk)); }
        over_meta[over_cnt].min_lon = c_min_lon; over_meta[over_cnt].max_lon = c_max_lon; over_meta[over_cnt].min_lat = c_min_lat; over_meta[over_cnt].max_lat = c_max_lat;
        over_meta[over_cnt].offset = FTELL64(f_qjmc); over_meta[over_cnt].comp_size = comp_size; over_meta[over_cnt].uncomp_size = total_uncomp;
        fwrite(comp_buf, 1, comp_size, f_qjmc); over_cnt++;
    }

    uint64_t over_dir_off = FTELL64(f_qjmc);
    fwrite(over_meta, sizeof(ChunkMetaDisk), over_cnt, f_qjmc);
    
    fwrite(&high_dir_off, 8, 1, f_qjmc); fwrite(&high_cnt, 4, 1, f_qjmc);
    fwrite(&over_dir_off, 8, 1, f_qjmc); fwrite(&over_cnt, 4, 1, f_qjmc);
    fwrite("QJM2", 1, 4, f_qjmc);
    
    fclose(f_qjmc);
    free(uncomp_buf); free(comp_buf); free(raw_buf); free(meta); free(sort_entries); free(over_meta);
    free(feats_buf); free(pts_buf); free(str_buf); fclose(f_raw);

    SetStatus("Done! Wrote %llu features to %s", (unsigned long long)written_feats, out_filename);
    return 0;
}
static DWORD WINAPI CompileThread(LPVOID lpParam) {
    ThreadArgs* args = (ThreadArgs*)lpParam;
    EnableWindow(hBtnRun, FALSE);
    EnableWindow(hBtnDelTmp, FALSE);
    SetStatus("Compilation started...");
    int result = RunCompilation(args->inFile, args->outFile, false, 0, 0, 0, 0);
    if (result != 0) SetStatus("Compilation failed.");
    EnableWindow(hBtnRun, TRUE);
    EnableWindow(hBtnDelTmp, TRUE);
    free(args);
    return 0;
}

static BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam) { SendMessageA(hwnd, WM_SETFONT, lParam, TRUE); return TRUE; }

static LRESULT CALLBACK CompilerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hCompilerWnd = hwnd;
            LoadConfig(); 
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            CreateWindowA("STATIC", "Source Map (.pbf, .shp):", WS_VISIBLE|WS_CHILD, 20, 20, 180, 20, hwnd, NULL, NULL, NULL);
            hTxtIn = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 40, 420, 24, hwnd, NULL, NULL, NULL);
            hBtnIn = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 450, 40, 90, 24, hwnd, (HMENU)1, NULL, NULL);
            CreateWindowA("STATIC", "Output File (.qjm):", WS_VISIBLE|WS_CHILD, 20, 80, 200, 20, hwnd, NULL, NULL, NULL);
            hTxtOut = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 100, 420, 24, hwnd, NULL, NULL, NULL);
            hBtnOut = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 450, 100, 90, 24, hwnd, (HMENU)2, NULL, NULL);
            
            CreateWindowA("STATIC", "Feature Layers & Retention % (Nodes < 65530):", WS_VISIBLE|WS_CHILD, 20, 140, 350, 20, hwnd, NULL, NULL, NULL);
            
            hChkLand = CreateWindowA("BUTTON", "Landmass", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 165, 80, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpLand = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 100, 165, 25, 20, hwnd, NULL, NULL, NULL);

            hChkHwyMain = CreateWindowA("BUTTON", "Main Roads", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 140, 165, 90, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpHwyMain = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 230, 165, 25, 20, hwnd, NULL, NULL, NULL);

            hChkPark = CreateWindowA("BUTTON", "Parks/Forests", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 270, 165, 105, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpPark = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 375, 165, 25, 20, hwnd, NULL, NULL, NULL);
            
            hChkCoast = CreateWindowA("BUTTON", "Coastlines", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 195, 80, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpCoast = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 100, 195, 25, 20, hwnd, NULL, NULL, NULL);

            hChkHwyMin = CreateWindowA("BUTTON", "Minor Roads", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 140, 195, 90, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpHwyMin = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 230, 195, 25, 20, hwnd, NULL, NULL, NULL);

            hChkWater = CreateWindowA("BUTTON", "Water Bodies", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 270, 195, 100, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpWater = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 375, 195, 25, 20, hwnd, NULL, NULL, NULL);

            hChkPcode = CreateWindowA("BUTTON", "Postcodes", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 225, 100, 20, hwnd, NULL, NULL, NULL);
            hChkSpecs = CreateWindowA("BUTTON", "Specs", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 140, 225, 80, 20, hwnd, NULL, NULL, NULL);

            CreateWindowA("STATIC", "LOD Ext. Simplification (Keep %):", WS_VISIBLE|WS_CHILD, 240, 225, 210, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpLowPoly = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 460, 225, 30, 20, hwnd, NULL, NULL, NULL);

            hChkNoRoads = CreateWindowA("BUTTON", "Land/Coast (No Roads)", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 255, 180, 20, hwnd, NULL, NULL, NULL);
            hTxtSimpNoRoads = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "30", WS_VISIBLE|WS_CHILD|ES_NUMBER, 205, 255, 30, 20, hwnd, NULL, NULL, NULL);
            
            SendMessage(hChkLand, BM_SETCHECK, BST_CHECKED, 0); 
            SendMessage(hChkCoast, BM_SETCHECK, g_CfgCoast ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkHwyMain, BM_SETCHECK, BST_CHECKED, 0); SendMessage(hChkHwyMin, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkPcode, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(hChkSpecs, BM_SETCHECK, BST_UNCHECKED, 0); 
            SendMessage(hChkNoRoads, BM_SETCHECK, BST_CHECKED, 0);
            
            hBtnRun = CreateWindowA("BUTTON", "Compile", WS_VISIBLE|WS_CHILD, 20, 290, 340, 35, hwnd, (HMENU)3, NULL, NULL);
            hBtnDelTmp = CreateWindowA("BUTTON", "Delete Temp Files", WS_VISIBLE|WS_CHILD, 370, 290, 170, 35, hwnd, (HMENU)4, NULL, NULL);
            hLblStatus = CreateWindowA("STATIC", "Ready.", WS_VISIBLE|WS_CHILD, 20, 335, 520, 20, hwnd, NULL, NULL, NULL);

            EnumChildWindows(hwnd, SetFontProc, (LPARAM)hFont);
            DragAcceptFiles(hwnd, TRUE); return 0;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam; char filePath[MAX_PATH]; DragQueryFileA(hDrop, 0, filePath, MAX_PATH); DragFinish(hDrop);
            char* ext = strrchr(filePath, '.');
            if (ext && (strcasecmp(ext, ".shp") == 0 || strcasecmp(ext, ".pbf") == 0)) AutoFillOutput(filePath);
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) { 
                OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = ""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "Map Files (*.pbf;*.shp)\0*.pbf;*.shp\0PBF Maps\0*.pbf\0Shapefiles\0*.shp\0All\0*.*\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) AutoFillOutput(path);
            } else if (LOWORD(wParam) == 2) { 
                OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = ""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "QJM Chunked File\0*.qjm\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT;
                if (GetSaveFileNameA(&ofn)) SetWindowTextA(hTxtOut, path);
            } else if (LOWORD(wParam) == 3) { 
                ThreadArgs* args = malloc(sizeof(ThreadArgs));
                GetWindowTextA(hTxtIn, args->inFile, MAX_PATH); GetWindowTextA(hTxtOut, args->outFile, MAX_PATH);
                if (strlen(args->inFile) == 0 || strlen(args->outFile) == 0) { SetStatus("Please specify input and output files."); free(args); return 0; }
                
                g_CfgLand = SendMessage(hChkLand, BM_GETCHECK, 0, 0) == BST_CHECKED; g_CfgCoast = SendMessage(hChkCoast, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgHwyMain = SendMessage(hChkHwyMain, BM_GETCHECK, 0, 0) == BST_CHECKED; g_CfgHwyMin = SendMessage(hChkHwyMin, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgWater = SendMessage(hChkWater, BM_GETCHECK, 0, 0) == BST_CHECKED; g_CfgPark = SendMessage(hChkPark, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgPcode = SendMessage(hChkPcode, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgSpecs = SendMessage(hChkSpecs, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_CfgNoRoads = SendMessage(hChkNoRoads, BM_GETCHECK, 0, 0) == BST_CHECKED;

                char buf[16];
                GetWindowTextA(hTxtSimpLand, buf, 16); g_SimpLand = atoi(buf);
                GetWindowTextA(hTxtSimpCoast, buf, 16); g_SimpCoast = atoi(buf);
                GetWindowTextA(hTxtSimpHwyMain, buf, 16); g_SimpHwyMain = atoi(buf);
                GetWindowTextA(hTxtSimpHwyMin, buf, 16); g_SimpHwyMin = atoi(buf);
                GetWindowTextA(hTxtSimpWater, buf, 16); g_SimpWater = atoi(buf);
                GetWindowTextA(hTxtSimpPark, buf, 16); g_SimpPark = atoi(buf);
                GetWindowTextA(hTxtSimpLowPoly, buf, 16); g_SimpLowPoly = atoi(buf);
                GetWindowTextA(hTxtSimpNoRoads, buf, 16); g_SimpNoRoads = atoi(buf);

                CreateThread(NULL, 0, CompileThread, args, 0, NULL);
            } else if (LOWORD(wParam) == 4) {
                char outFile[MAX_PATH] = {0};
                GetWindowTextA(hTxtOut, outFile, MAX_PATH);
                if (strlen(outFile) == 0) {
                    SetStatus("Specify an Output File (.qjm) first to delete its temp files.");
                } else {
                    DeleteTempFilesForOutput(outFile);
                }
            } break;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    } return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int main(int argc, char** argv) {
    FreeConsole();
    LoadConfig(); 
    if (argc >= 2) {
        char out_filename[1024]; strncpy(out_filename, argv[1], sizeof(out_filename) - 1); out_filename[sizeof(out_filename) - 1] = '\0';
        char* ext = strrchr(out_filename, '.');
        if (ext && (strcasecmp(ext, ".pbf") == 0 || strcasecmp(ext, ".shp") == 0)) strcpy(ext, ".qjm"); 
        else strncat(out_filename, ".qjm", sizeof(out_filename) - strlen(out_filename) - 1);
        return RunCompilation(argv[1], out_filename, false, 0, 0, 0, 0);
    }
    WNDCLASSA wc = {0}; wc.lpfnWndProc = CompilerWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "Pbf2QJMGuiClass"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "Pbf2QJMGuiClass", "PBF to QJMC Compiler", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 410, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    MSG msg = {0}; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); } return 0;
}