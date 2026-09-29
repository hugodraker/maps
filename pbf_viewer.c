/*
 * Win32 GDI Map Viewer - Native PBF, Hardware Culling, Tag Parsing & PDF Export
 * Compile: gcc -Os -s -Wall -Wextra pbf_viewer.c -o pbf_viewer.exe -mwindows -lgdi32 -lz -lcomdlg32 -Wl,--large-address-aware
 *
 * THIS WORK IS NOT FIT FOR ANY FUNCTION OR PURPOSE, COMES WITH NO WARRANTY,
 * AND IS BEING RELEASED INTO THE PUBLIC DOMAIN.
 * ============================================================================ */

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <math.h>
#include <shellapi.h> 
#include <ctype.h>
#define WIRETYPE_VARINT 0
#define WIRETYPE_64BIT  1
#define WIRETYPE_LENGTH 2
#define WIRETYPE_32BIT  5
// Rendering Classes
#define CLASS_UNKNOWN   0
#define CLASS_WATER     1
#define CLASS_LAND      2
#define CLASS_PARK      3
#define CLASS_COASTLINE 4
#define CLASS_HWY_MINOR 5
#define CLASS_HWY_MAIN  6
/* ==========================================================================
 * 1. SPATIAL DATA STRUCTURES & GLOBALS
 * ========================================================================== */
/* ==========================================================================
 * 1. SPATIAL DATA STRUCTURES & GLOBALS
 * ========================================================================== */
typedef struct { 
    double lon; 
    double lat; 
} MapPoint;

typedef struct {
    int feature_class;
    char* name;
    char* postcode;
    MapPoint* points;
    uint64_t* refs;  // Temporary storage for Node IDs before resolution
    int point_count;
    double min_lon, max_lon; 
    double min_lat, max_lat;
} MapFeature;

typedef struct {
    uint64_t id;
    int32_t lat, lon; // Scaled to 1e-7 to save 33% memory footprint over int64_t
} PbfNode;

typedef struct {
    const char* str;
    size_t len;
} PbfString;

typedef struct {
    char name[128];
    double lon, lat;
} MapPin;

typedef struct {
    char text[256];
    double dist; 
} RouteStep;

// --- Map Data Globals ---
PbfNode* g_NodeCache = NULL;
uint64_t g_NodeCount = 0;
uint64_t g_NodeCapacity = 0;

uint64_t* g_NeededNodes = NULL; // Strict node ID filter
uint64_t g_NeededNodeCount = 0;
uint64_t g_NeededNodeCap = 0;

MapFeature* g_MapFeatures = NULL;
uint64_t g_FeatureCount = 0;
uint64_t g_FeatureCapacity = 0;

int g_HighlightedFeature = -1;
bool g_PendingMapRestart = false;

// --- Application View State ---
double g_CenterLon = 0.0, g_CenterLat = 0.0;
double g_PanX = 0.0, g_PanY = 0.0;
double g_Zoom = 5.0;
double g_Rotation = 0.0;
bool g_AutoFit = true;
char g_MapFilesStr[2048] = "";

int g_WinX = CW_USEDEFAULT, g_WinY = CW_USEDEFAULT;
int g_WinW = 1024, g_WinH = 768;
bool g_ShowDebug = false;

bool g_IsDragging = false;
bool g_MouseMoved = false;
POINT g_LastMousePos;

// --- Colors ---
COLORREF c_Land = RGB(242, 240, 233);
COLORREF c_Water = RGB(163, 204, 255);
COLORREF c_Park = RGB(202, 223, 170);
COLORREF c_Coast = RGB(100, 150, 180);
COLORREF c_HwyMin = RGB(255, 255, 255);
COLORREF c_HwyMain = RGB(253, 184, 19);
COLORREF c_Pin = RGB(220, 50, 50);
COLORREF c_PinSel = RGB(50, 100, 255);
COLORREF c_Route = RGB(138, 43, 226);

// --- Pins & Routing Globals ---
MapPin g_Pins[1000];
int g_PinCount = 0;
int g_EditingPinIndex = -1;
HWND hEditPin = NULL;
WNDPROC OldEditProc = NULL;

int g_SelectedPins[1000];
int g_SelectedPinCount = 0;

RouteStep* g_RouteSteps = NULL;
int g_RouteStepCount = 0;
MapPoint* g_RoutePath = NULL;
int g_RoutePathCount = 0;
char g_SavedRouteStr[8192] = "";

double g_TotalRouteDist = 0.0;
bool g_UseMiles = false;

float g_LegendScrollY = 0.0f;
float g_LegendMaxScroll = 0.0f;
HWND hBtnCancel = NULL, hCboRouteType = NULL;
int g_RouteMode = 0; // 0 = Fastest, 1 = Shortest

// --- GUI & Layer Toggles ---
HWND hPdfDlg = NULL;
HWND hTxtOut, hBtnBrowse, hTxtTitle;
HWND hTxtWidth, hTxtHeight, hBtnToggle, hTxtMargH, hTxtMargV, hBtnCreatePdf;
HWND hChkLand, hChkWater, hChkPark, hChkCoast, hChkHwyMin, hChkHwyMain, hChkLabels;
HWND hTxtSearch, hBtnSearchOpts, hListSearch;

int g_ShowLand = 1, g_ShowWater = 1, g_ShowPark = 1, g_ShowCoast = 1, g_ShowHwyMin = 1, g_ShowHwyMain = 1, g_ShowLabels = 1;
int g_PdfExpLand = 1, g_PdfExpWater = 1, g_PdfExpPark = 1, g_PdfExpCoast = 1, g_PdfExpHwyMin = 1, g_PdfExpHwyMain = 1, g_PdfExpLabels = 1;
bool g_SearchRoads = true, g_SearchPostcodes = true;

int isLandscape = 0;
float page_w_mm = 215.9f, page_h_mm = 279.4f;
float g_MargH = 4.0f, g_MargV = 4.0f;

long pdf_objects[4096];
int pdf_obj_cnt = 1;

typedef struct { char* data; int len; int cap; } Stream;

int CompareUint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

bool IsNodeNeeded(uint64_t id) {
    if (g_NeededNodeCount == 0) return false;
    int64_t left = 0, right = g_NeededNodeCount - 1;
    while (left <= right) {
        int64_t mid = left + (right - left) / 2;
        if (g_NeededNodes[mid] == id) return true;
        if (g_NeededNodes[mid] < id) left = mid + 1;
        else right = mid - 1;
    }
    return false;
}
/* ==========================================================================
 * 2. HELPERS & SEARCH ENGINE
 * ========================================================================== */
COLORREF ParseHexColor(const char* hex, COLORREF def) {
    if (!hex || strlen(hex) < 6) return def;
    int r, g, b;
    if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) return RGB(r, g, b);
    return def;
}

void ColorToHex(COLORREF c, char* out) {
    sprintf(out, "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
}

uint32_t SwapEndian32(uint32_t val) {
    return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) | ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000);
}

uint64_t ReadVarint(const uint8_t** ptr, const uint8_t* end) {
    uint64_t val = 0;
    int shift = 0;
    while (*ptr < end) {
        uint8_t b = **ptr;
        (*ptr)++;
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return val;
}

int64_t DecodeZigZag(uint64_t n) { 
    return (n >> 1) ^ -(int64_t)(n & 1); 
}

void SkipProtobufField(const uint8_t** ptr, const uint8_t* end, uint8_t wire_type) {
    switch (wire_type) {
        case WIRETYPE_VARINT: ReadVarint(ptr, end); break;
        case WIRETYPE_64BIT:  *ptr += 8; break;
        case WIRETYPE_LENGTH: *ptr += ReadVarint(ptr, end); break;
        case WIRETYPE_32BIT:  *ptr += 4; break;
        default: *ptr = end; break; 
    }
}

PbfNode* FindNode(uint64_t id) {
    int64_t left = 0, right = g_NodeCount - 1;
    while (left <= right) {
        int64_t mid = left + (right - left) / 2;
        if (g_NodeCache[mid].id == id) return &g_NodeCache[mid];
        if (g_NodeCache[mid].id < id) left = mid + 1;
        else right = mid - 1;
    }
    return NULL;
}

int CompareNodes(const void* a, const void* b) {
    uint64_t idA = ((PbfNode*)a)->id;
    uint64_t idB = ((PbfNode*)b)->id;
    if (idA < idB) return -1;
    if (idA > idB) return 1;
    return 0;
}

bool FindSubStringIC(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    int hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen == 0) return true;
    for (int i = 0; i <= hlen - nlen; i++) {
        int j = 0;
        while (j < nlen && tolower(haystack[i+j]) == tolower(needle[j])) j++;
        if (j == nlen) return true;
    }
    return false;
}

void PopulateSearchList(HWND hwnd) {
    char query[128];
    GetWindowTextA(hTxtSearch, query, 128);
    SendMessage(hListSearch, LB_RESETCONTENT, 0, 0);
    if (strlen(query) < 2) {
        ShowWindow(hListSearch, SW_HIDE);
        return;
    }

    int count = 0;
    for (uint64_t i = 0; i < g_FeatureCount && count < 50; i++) {
        MapFeature* f = &g_MapFeatures[i];
        bool match = false;
        
        if (g_SearchRoads && f->name && FindSubStringIC(f->name, query)) match = true;
        if (!match && g_SearchPostcodes && f->postcode && FindSubStringIC(f->postcode, query)) match = true;

        if (match) {
            char display[256];
            snprintf(display, 256, "%s %s", f->name ? f->name : "", f->postcode ? f->postcode : "");
            int idx = SendMessageA(hListSearch, LB_ADDSTRING, 0, (LPARAM)display);
            SendMessageA(hListSearch, LB_SETITEMDATA, idx, (LPARAM)i);
            count++;
        }
    }
    if (count > 0) {
        RECT r; GetWindowRect(hTxtSearch, &r);
        POINT pt = {r.left, r.bottom};
        ScreenToClient(hwnd, &pt);
        SetWindowPos(hListSearch, HWND_TOP, pt.x, pt.y, 250, 150, SWP_SHOWWINDOW);
    } else {
        ShowWindow(hListSearch, SW_HIDE);
    }
}

/* ==========================================================================
 * 3. ROUTING ENGINE
 * ========================================================================== */
void CalculateRoute() {
    if (g_RoutePath) free(g_RoutePath);
    g_RoutePath = NULL;
    g_RouteStepCount = 0;
    g_RoutePathCount = 0;
    g_TotalRouteDist = 0.0;
    g_LegendScrollY = 0.0f;
    
    if (g_SelectedPinCount < 2) {
        ShowWindow(hBtnCancel, SW_HIDE);
        ShowWindow(hCboRouteType, SW_HIDE);
        return;
    }

    char sys_measure[2] = "0";
    GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_IMEASURE, sys_measure, 2);
    g_UseMiles = (sys_measure[0] == '1'); 

    for (int p = 0; p < g_SelectedPinCount - 1; p++) {
        MapPin* pA = &g_Pins[g_SelectedPins[p]];
        MapPin* pB = &g_Pins[g_SelectedPins[p+1]];

        double dx = pB->lon - pA->lon;
        double dy = pB->lat - pA->lat;
        double dist_km = sqrt(dx*dx + dy*dy) * 111.0; 
        double dist_disp = g_UseMiles ? (dist_km * 0.621371) : dist_km;
        
        g_TotalRouteDist += dist_disp;

        g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
        g_RoutePath[g_RoutePathCount].lon = pA->lon;
        g_RoutePath[g_RoutePathCount].lat = pA->lat;
        g_RoutePathCount++;

        int steps = (int)(dist_km * 8.0) + 5; 
        char last_road[128] = "";
        double accum_dist = 0;
        
        MapFeature* last_f = NULL;
        int last_idx = -1;

        for (int s = 0; s <= steps; s++) {
            double slon = pA->lon + dx * (s / (double)steps);
            double slat = pA->lat + dy * (s / (double)steps);

            double min_d = 999999;
            char best_road[128] = "Unnamed Road";
            MapFeature* best_f = NULL;
            int best_idx = -1;

            for (uint64_t i = 0; i < g_FeatureCount; i++) {
                MapFeature* f = &g_MapFeatures[i];
                if (f->feature_class != CLASS_HWY_MAIN && f->feature_class != CLASS_HWY_MINOR) continue;
                if (slon < f->min_lon - 0.03 || slon > f->max_lon + 0.03) continue;
                if (slat < f->min_lat - 0.03 || slat > f->max_lat + 0.03) continue;

                for (int j = 0; j < f->point_count; j++) {
                    double ddx = f->points[j].lon - slon;
                    double ddy = f->points[j].lat - slat;
                    double d = ddx*ddx + ddy*ddy;
                    
                    if (g_RouteMode == 0 && f->feature_class == CLASS_HWY_MAIN) d *= 0.1; 

                    if (d < min_d) {
                        min_d = d;
                        best_f = f;
                        best_idx = j;
                        if (f->name && strlen(f->name) > 0) strcpy(best_road, f->name);
                        else strcpy(best_road, (f->feature_class == CLASS_HWY_MAIN) ? "Main Highway" : "Local Road");
                    }
                }
            }

            if (best_f) {
                if (best_f == last_f && best_idx != last_idx && last_idx != -1) {
                    int step_dir = (best_idx > last_idx) ? 1 : -1;
                    for (int k = last_idx + step_dir; k != best_idx; k += step_dir) {
                        g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
                        g_RoutePath[g_RoutePathCount++] = best_f->points[k];
                    }
                }
                
                g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
                g_RoutePath[g_RoutePathCount++] = best_f->points[best_idx];

                last_f = best_f;
                last_idx = best_idx;
            }

            double step_len = dist_disp / steps;
            if (strcmp(best_road, last_road) != 0) {
                if (strlen(last_road) > 0) {
                    g_RouteSteps = realloc(g_RouteSteps, (g_RouteStepCount + 1) * sizeof(RouteStep));
                    snprintf(g_RouteSteps[g_RouteStepCount].text, 256, "Take %.128s", last_road);
                    g_RouteSteps[g_RouteStepCount].dist = accum_dist;
                    g_RouteStepCount++;
                }
                strcpy(last_road, best_road);
                accum_dist = step_len;
            } else {
                accum_dist += step_len;
            }
        }
        if (accum_dist > 0) {
            g_RouteSteps = realloc(g_RouteSteps, (g_RouteStepCount + 1) * sizeof(RouteStep));
            snprintf(g_RouteSteps[g_RouteStepCount].text, 256, "Take %.128s", last_road);
            g_RouteSteps[g_RouteStepCount].dist = accum_dist;
            g_RouteStepCount++;
        }

        g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
        g_RoutePath[g_RoutePathCount].lon = pB->lon;
        g_RoutePath[g_RoutePathCount].lat = pB->lat;
        g_RoutePathCount++;
    }

    if (g_RoutePathCount > 0) {
        ShowWindow(hBtnCancel, SW_SHOW);
        ShowWindow(hCboRouteType, SW_SHOW);
    }
}

/* ==========================================================================
 * 4. OPENSTREETMAP PBF TAG PARSER & EXTRACTOR
 * ========================================================================== */
void ProcessPrimitiveBlock(const uint8_t* data, uint64_t size, int pass) {
    const uint8_t* end = data + size;
    int64_t lat_offset = 0, lon_offset = 0, granularity = 100;

    PbfString* str_table = NULL;
    uint64_t str_count = 0, str_cap = 0;

    const uint8_t* ptr = data;
    while (ptr < end) {
        uint64_t tag_wire = ReadVarint(&ptr, end);
        uint64_t tag = tag_wire >> 3;
        uint8_t wire = tag_wire & 7;

        if (tag == 1 && wire == WIRETYPE_LENGTH) { 
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
                    str_count++;
                    ptr += s_len;
                } else SkipProtobufField(&ptr, st_end, s_tag_wire & 7);
            }
        }
        else if (tag == 17) granularity = ReadVarint(&ptr, end);
        else if (tag == 19) lat_offset = ReadVarint(&ptr, end);
        else if (tag == 20) lon_offset = ReadVarint(&ptr, end);
        else SkipProtobufField(&ptr, end, wire);
    }

    ptr = data;
    while (ptr < end) {
        uint64_t tag_wire = ReadVarint(&ptr, end);
        uint64_t tag = tag_wire >> 3;
        uint8_t wire = tag_wire & 7;

        if (tag == 2 && wire == WIRETYPE_LENGTH) { 
            uint64_t group_len = ReadVarint(&ptr, end);
            const uint8_t* group_end = ptr + group_len;
            
            while (ptr < group_end) {
                uint64_t g_tag_wire = ReadVarint(&ptr, group_end);
                uint64_t g_tag = g_tag_wire >> 3;
                uint8_t g_wire = g_tag_wire & 7;

                if (g_tag == 1 && pass == 2) { 
                    uint64_t msg_len = ReadVarint(&ptr, group_end);
                    const uint8_t* msg_end = ptr + msg_len;
                    uint64_t id = 0;
                    int64_t lat = 0, lon = 0;
                    
                    while (ptr < msg_end) {
                        uint64_t n_tag_wire = ReadVarint(&ptr, msg_end);
                        uint64_t n_tag = n_tag_wire >> 3;
                        if (n_tag == 1) id = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else if (n_tag == 8) lat = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else if (n_tag == 9) lon = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else SkipProtobufField(&ptr, msg_end, n_tag_wire & 7);
                    }
                    if (IsNodeNeeded(id) && g_NodeCount < g_NeededNodeCount) {
                        g_NodeCache[g_NodeCount].id = id;
                        g_NodeCache[g_NodeCount].lat = (int32_t)((lat_offset + (granularity * lat)) / 100);
                        g_NodeCache[g_NodeCount].lon = (int32_t)((lon_offset + (granularity * lon)) / 100);
                        g_NodeCount++;
                    }
                }
                else if (g_tag == 2 && pass == 2) { 
                    uint64_t dense_len = ReadVarint(&ptr, group_end);
                    const uint8_t* dense_end = ptr + dense_len;
                    const uint8_t *id_ptr = NULL, *lat_ptr = NULL, *lon_ptr = NULL;

                    while (ptr < dense_end) {
                        uint64_t d_tag_wire = ReadVarint(&ptr, dense_end);
                        uint64_t d_tag = d_tag_wire >> 3;
                        if (d_tag == 1) { id_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else if (d_tag == 8) { lat_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else if (d_tag == 9) { lon_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else SkipProtobufField(&ptr, dense_end, d_tag_wire & 7);
                    }

                    if (id_ptr && lat_ptr && lon_ptr) {
                        uint64_t id_len = ReadVarint(&id_ptr, dense_end);
                        ReadVarint(&lat_ptr, dense_end); 
                        ReadVarint(&lon_ptr, dense_end);
                        
                        const uint8_t* id_end = id_ptr + id_len;
                        int64_t last_id = 0, last_lat = 0, last_lon = 0;

                        while (id_ptr < id_end) {
                            last_id += DecodeZigZag(ReadVarint(&id_ptr, dense_end));
                            last_lat += DecodeZigZag(ReadVarint(&lat_ptr, dense_end));
                            last_lon += DecodeZigZag(ReadVarint(&lon_ptr, dense_end));

                            if (IsNodeNeeded((uint64_t)last_id) && g_NodeCount < g_NeededNodeCount) {
                                g_NodeCache[g_NodeCount].id = last_id;
                                g_NodeCache[g_NodeCount].lat = (int32_t)((lat_offset + (granularity * last_lat)) / 100);
                                g_NodeCache[g_NodeCount].lon = (int32_t)((lon_offset + (granularity * last_lon)) / 100);
                                g_NodeCount++;
                            }
                        }
                    }
                } 
                else if (g_tag == 3 && pass == 1) { 
                    uint64_t way_len = ReadVarint(&ptr, group_end);
                    const uint8_t* way_end = ptr + way_len;
                    
                    const uint8_t *refs_ptr = NULL, *keys_ptr = NULL, *vals_ptr = NULL;
                    uint64_t keys_len = 0, vals_len = 0;

                    while (ptr < way_end) {
                        uint64_t w_tag_wire = ReadVarint(&ptr, way_end);
                        uint64_t w_tag = w_tag_wire >> 3;
                        if (w_tag == 2) { keys_len = ReadVarint(&ptr, way_end); keys_ptr = ptr; ptr += keys_len; }
                        else if (w_tag == 3) { vals_len = ReadVarint(&ptr, way_end); vals_ptr = ptr; ptr += vals_len; }
                        else if (w_tag == 8) { refs_ptr = ptr; SkipProtobufField(&ptr, way_end, w_tag_wire & 7); }
                        else SkipProtobufField(&ptr, way_end, w_tag_wire & 7);
                    }

                    if (refs_ptr) {
                        int feature_class = CLASS_UNKNOWN;
                        const char* feat_name = NULL;
                        const char* feat_postcode = NULL;
                        size_t feat_name_len = 0, feat_postcode_len = 0;

                        if (keys_ptr && vals_ptr && str_table) {
                            const uint8_t *k_p = keys_ptr, *v_p = vals_ptr;
                            while (k_p < keys_ptr + keys_len && v_p < vals_ptr + vals_len) {
                                uint64_t k_idx = ReadVarint(&k_p, keys_ptr + keys_len);
                                uint64_t v_idx = ReadVarint(&v_p, vals_ptr + vals_len);
                                
                                if (k_idx < str_count && v_idx < str_count) {
                                    PbfString* k = &str_table[k_idx];
                                    PbfString* v = &str_table[v_idx];
                                    
                                    if (k->len == 4 && strncmp(k->str, "name", 4) == 0) {
                                        feat_name = v->str;
                                        feat_name_len = v->len;
                                    }
                                    else if ((k->len == 13 && strncmp(k->str, "addr:postcode", 13) == 0) || 
                                             (k->len == 11 && strncmp(k->str, "postal_code", 11) == 0)) {
                                        feat_postcode = v->str;
                                        feat_postcode_len = v->len;
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
                                    else if (k->len == 7 && strncmp(k->str, "leisure", 7) == 0) {
                                        feature_class = CLASS_PARK;
                                    }
                                }
                            }
                        }

                        if (feature_class != CLASS_UNKNOWN) {
                            uint64_t refs_len = ReadVarint(&refs_ptr, way_end);
                            const uint8_t* refs_end = refs_ptr + refs_len;
                            
                            if (g_FeatureCount >= g_FeatureCapacity) {
                                g_FeatureCapacity = g_FeatureCapacity ? g_FeatureCapacity * 2 : 10000;
                                g_MapFeatures = (MapFeature*)realloc(g_MapFeatures, g_FeatureCapacity * sizeof(MapFeature));
                            }

                            MapFeature* f = &g_MapFeatures[g_FeatureCount++];
                            f->feature_class = feature_class;
                            f->points = NULL; 
                            f->refs = (uint64_t*)malloc((refs_len + 1) * sizeof(uint64_t));
                            f->point_count = 0;
                            f->min_lon = 180.0; f->max_lon = -180.0;
                            f->min_lat = 90.0;  f->max_lat = -90.0;
                            f->name = NULL;
                            f->postcode = NULL;

                            if (feat_name) {
                                f->name = (char*)malloc(feat_name_len + 1);
                                strncpy(f->name, feat_name, feat_name_len);
                                f->name[feat_name_len] = '\0';
                            }
                            if (feat_postcode) {
                                f->postcode = (char*)malloc(feat_postcode_len + 1);
                                strncpy(f->postcode, feat_postcode, feat_postcode_len);
                                f->postcode[feat_postcode_len] = '\0';
                            }
                            
                            int64_t last_ref = 0;
                            while (refs_ptr < refs_end) {
                                last_ref += DecodeZigZag(ReadVarint(&refs_ptr, refs_end));
                                f->refs[f->point_count++] = (uint64_t)last_ref;
                            }

                            if (f->point_count < 2) {
                                if (f->points) { free(f->points); f->points = NULL; }
                                if (f->refs) { free(f->refs); f->refs = NULL; }
                                if (f->name) { free(f->name); f->name = NULL; }
                                if (f->postcode) { free(f->postcode); f->postcode = NULL; }
                                g_FeatureCount--;
                            }
                        }
                    }
                }
                else SkipProtobufField(&ptr, group_end, g_wire);
            }
        } 
        else SkipProtobufField(&ptr, end, wire);
    }
    if (str_table) free(str_table);
}

void StitchCoastlines() {
    int coast_cnt = 0;
    for (uint64_t i = 0; i < g_FeatureCount; i++) {
        if (g_MapFeatures[i].feature_class == CLASS_COASTLINE) coast_cnt++;
    }
    if (coast_cnt > 2000) return; // Prevent infinite loop CPU lockup on massive 6GB files

    bool merged = true;
    while (merged) {
        merged = false;
        for (uint64_t i = 0; i < g_FeatureCount; i++) {
            if (g_MapFeatures[i].feature_class != CLASS_COASTLINE) continue;
            if (g_MapFeatures[i].point_count < 2) continue;
            
            MapPoint start_pt = g_MapFeatures[i].points[0];
            MapPoint end_pt = g_MapFeatures[i].points[g_MapFeatures[i].point_count - 1];

            if (fabs(start_pt.lon - end_pt.lon) < 1e-5 && fabs(start_pt.lat - end_pt.lat) < 1e-5) {
                g_MapFeatures[i].points[g_MapFeatures[i].point_count - 1] = start_pt;
                g_MapFeatures[i].feature_class = CLASS_LAND; 
                continue;
            }

            for (uint64_t j = i + 1; j < g_FeatureCount; j++) {
                if (g_MapFeatures[j].feature_class != CLASS_COASTLINE) continue;
                if (g_MapFeatures[j].point_count < 2) continue;

                MapPoint j_start = g_MapFeatures[j].points[0];
                MapPoint j_end = g_MapFeatures[j].points[g_MapFeatures[j].point_count - 1];

                if (fabs(end_pt.lon - j_start.lon) < 1e-5 && fabs(end_pt.lat - j_start.lat) < 1e-5) {
                    int new_count = g_MapFeatures[i].point_count + g_MapFeatures[j].point_count - 1;
                    g_MapFeatures[i].points = realloc(g_MapFeatures[i].points, new_count * sizeof(MapPoint));
                    memcpy(g_MapFeatures[i].points + g_MapFeatures[i].point_count, 
                           g_MapFeatures[j].points + 1, (g_MapFeatures[j].point_count - 1) * sizeof(MapPoint));
                    g_MapFeatures[i].point_count = new_count;
                    g_MapFeatures[j].point_count = 0;
                    merged = true; break;
                } else if (fabs(start_pt.lon - j_end.lon) < 1e-5 && fabs(start_pt.lat - j_end.lat) < 1e-5) {
                    int new_count = g_MapFeatures[j].point_count + g_MapFeatures[i].point_count - 1;
                    g_MapFeatures[j].points = realloc(g_MapFeatures[j].points, new_count * sizeof(MapPoint));
                    memcpy(g_MapFeatures[j].points + g_MapFeatures[j].point_count, 
                           g_MapFeatures[i].points + 1, (g_MapFeatures[i].point_count - 1) * sizeof(MapPoint));
                    g_MapFeatures[j].point_count = new_count;
                    g_MapFeatures[i].point_count = 0;
                    merged = true; break;
                }
            }
            if (merged) break; // Break out of i loop to cleanly scan the modified array
        }
    }
}

void LoadPbfData(const char* filepath) {
    char path[1024] = {0};
    size_t len = strlen(filepath);
    if (len >= 2 && filepath[0] == '"' && filepath[len - 1] == '"') {
        strncpy(path, filepath + 1, len - 2);
    } else {
        strncpy(path, filepath, sizeof(path) - 1);
    }

    uint64_t start_feature_idx = g_FeatureCount; 

    for (int pass = 1; pass <= 2; pass++) {
        FILE* file = fopen(path, "rb");
        if (!file) {
            if (pass == 1) { 
                char errMsg[2048];
                snprintf(errMsg, sizeof(errMsg), "Could not open file:\n%s\nEnsure the path is correct.", path);
                MessageBox(NULL, errMsg, "File Error", MB_ICONERROR);
            }
            return;
        }

        if (pass == 2) {
            uint64_t total_refs = 0;
            for (uint64_t i = start_feature_idx; i < g_FeatureCount; i++) {
                total_refs += g_MapFeatures[i].point_count;
            }
            if (total_refs > 0) {
                g_NeededNodes = (uint64_t*)malloc(total_refs * sizeof(uint64_t));
                if (!g_NeededNodes) {
                    MessageBox(NULL, "OOM: Map too large for 32-bit contiguous memory allocation.", "Memory Error", MB_ICONERROR);
                    fclose(file);
                    return;
                }
                g_NeededNodeCount = 0;
                for (uint64_t i = start_feature_idx; i < g_FeatureCount; i++) {
                    for (int j = 0; j < g_MapFeatures[i].point_count; j++) {
                        g_NeededNodes[g_NeededNodeCount++] = g_MapFeatures[i].refs[j];
                    }
                }
                
                qsort(g_NeededNodes, g_NeededNodeCount, sizeof(uint64_t), CompareUint64);
                uint64_t unique = 1;
                for (uint64_t i = 1; i < g_NeededNodeCount; i++) {
                    if (g_NeededNodes[i] != g_NeededNodes[i-1]) {
                        g_NeededNodes[unique++] = g_NeededNodes[i];
                    }
                }
                g_NeededNodeCount = unique;
                g_NodeCache = (PbfNode*)malloc(g_NeededNodeCount * sizeof(PbfNode));
                if (!g_NodeCache) {
                    MessageBox(NULL, "OOM: Cannot allocate Node Cache.", "Memory Error", MB_ICONERROR);
                    free(g_NeededNodes); g_NeededNodes = NULL; g_NeededNodeCount = 0;
                    fclose(file);
                    return;
                }
                g_NodeCount = 0;
            }
        }

        while (!feof(file)) {
            uint32_t header_len_be = 0;
            if (fread(&header_len_be, 1, 4, file) != 4) break;
            uint32_t header_len = SwapEndian32(header_len_be);

            if (header_len > 64 * 1024) break; 

            uint8_t* header_buf = (uint8_t*)malloc(header_len);
            if (!header_buf || fread(header_buf, 1, header_len, file) != header_len) break;

            const uint8_t* ptr = header_buf;
            const uint8_t* end = header_buf + header_len;
            char blob_type[32] = {0};
            uint64_t datasize = 0;

            while (ptr < end) {
                uint64_t tag_wire = ReadVarint(&ptr, end);
                uint64_t tag = tag_wire >> 3;
                if (tag == 1) {
                    uint64_t str_len = ReadVarint(&ptr, end);
                    strncpy(blob_type, (const char*)ptr, str_len < 31 ? str_len : 31);
                    ptr += str_len;
                } 
                else if (tag == 3) datasize = ReadVarint(&ptr, end);
                else SkipProtobufField(&ptr, end, tag_wire & 7);
            }
            free(header_buf);
            
            if (datasize > 64 * 1024 * 1024) break;

            uint8_t* blob_buf = (uint8_t*)malloc(datasize);
            if (!blob_buf || fread(blob_buf, 1, datasize, file) != datasize) break;

            if (strcmp(blob_type, "OSMData") == 0) {
                ptr = blob_buf;
                end = blob_buf + datasize;
                uint64_t raw_size = 0, zlib_size = 0;
                uint8_t* zlib_data = NULL;

                while (ptr < end) {
                    uint64_t tag_wire = ReadVarint(&ptr, end);
                    uint64_t tag = tag_wire >> 3;
                    if (tag == 2) raw_size = ReadVarint(&ptr, end);
                    else if (tag == 3) {
                        zlib_size = ReadVarint(&ptr, end);
                        zlib_data = (uint8_t*)ptr;
                        ptr += zlib_size;
                    } 
                    else SkipProtobufField(&ptr, end, tag_wire & 7);
                }

                if (zlib_data && raw_size > 0) {
                    uint8_t* uncompressed = (uint8_t*)malloc(raw_size);
                    if (uncompressed) {
                        unsigned long dest_len = raw_size;
                        if (uncompress(uncompressed, &dest_len, zlib_data, zlib_size) == Z_OK) {
                            ProcessPrimitiveBlock(uncompressed, dest_len, pass);
                        }
                        free(uncompressed);
                    }
                }
            }
            free(blob_buf);
        }
        fclose(file);
    }

    qsort(g_NodeCache, g_NodeCount, sizeof(PbfNode), CompareNodes);

    uint64_t valid_feats = start_feature_idx;
    for (uint64_t i = start_feature_idx; i < g_FeatureCount; i++) {
        MapFeature* f = &g_MapFeatures[i];
        
        if (f->point_count > 0 && f->refs) {
            f->points = (MapPoint*)malloc((f->point_count + 1) * sizeof(MapPoint));
            int valid_pts = 0;
            for (int j = 0; j < f->point_count; j++) {
                PbfNode* node = FindNode(f->refs[j]);
                if (node) {
                    f->points[valid_pts].lat = node->lat * 1e-7;
                    f->points[valid_pts].lon = node->lon * 1e-7;
                    
                    if (f->points[valid_pts].lon < f->min_lon) f->min_lon = f->points[valid_pts].lon;
                    if (f->points[valid_pts].lon > f->max_lon) f->max_lon = f->points[valid_pts].lon;
                    if (f->points[valid_pts].lat < f->min_lat) f->min_lat = f->points[valid_pts].lat;
                    if (f->points[valid_pts].lat > f->max_lat) f->max_lat = f->points[valid_pts].lat;
                    valid_pts++;
                }
            }
            free(f->refs);
            f->refs = NULL;
            f->point_count = valid_pts;
        }

        if (f->point_count < 2) {
            if (f->points) { free(f->points); f->points = NULL; }
            if (f->name) { free(f->name); f->name = NULL; }
            if (f->postcode) { free(f->postcode); f->postcode = NULL; }
        } else {
            if (f->feature_class == CLASS_LAND || f->feature_class == CLASS_WATER || f->feature_class == CLASS_PARK) {
                if (fabs(f->points[0].lon - f->points[f->point_count-1].lon) > 1e-6 ||
                    fabs(f->points[0].lat - f->points[f->point_count-1].lat) > 1e-6) {
                    f->points[f->point_count] = f->points[0];
                    f->point_count++;
                }
            }
            f->points = (MapPoint*)realloc(f->points, f->point_count * sizeof(MapPoint));
            g_MapFeatures[valid_feats++] = *f; 
        }
    }
    g_FeatureCount = valid_feats;

    StitchCoastlines();

    if (g_NodeCache) { free(g_NodeCache); g_NodeCache = NULL; g_NodeCount = 0; g_NodeCapacity = 0; }
    if (g_NeededNodes) { free(g_NeededNodes); g_NeededNodes = NULL; g_NeededNodeCount = 0; g_NeededNodeCap = 0; }
}

/* ==========================================================================
 * 5. PDF GENERATION ENGINE
 * ========================================================================== */
void s_app(Stream* s, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (s->len + n >= s->cap) {
        s->cap = (s->cap == 0 ? 4096 : s->cap * 2) + n + 1024;
        s->data = realloc(s->data, s->cap);
    }
    memcpy(s->data + s->len, buf, n);
    s->len += n;
    s->data[s->len] = 0;
}

void pdf_color(Stream* s, int is_stroke, int r, int g, int b) {
    s_app(s, "%.3f %.3f %.3f %s\n", r/255.0f, g/255.0f, b/255.0f, is_stroke ? "RG" : "rg");
}

void pdf_center_text(Stream* s, const char* text, float x, float y, float w, int font_size, int is_bold) {
    float text_w = strlen(text) * font_size * 0.5f; 
    float offset = x + (w - text_w) / 2.0f;
    if (offset < x) offset = x; 
    s_app(s, "BT %s %d Tf 0 0 0 rg %.2f %.2f Td (%s) Tj ET\n", is_bold ? "/F2" : "/F1", font_size, offset, y, text);
}

void LatLonToPdf(double lon, double lat, double cx, double cy, double zoom, double* outX, double* outY) {
    double dx = (lon - g_CenterLon) * zoom;
    double dy = (lat - g_CenterLat) * zoom;

    double cos_r = cos(g_Rotation);
    double sin_r = sin(g_Rotation);
    double rx = dx * cos_r - dy * sin_r;
    double ry = dx * sin_r + dy * cos_r;

    double pan_pdf_x = g_PanX * (72.0 / 96.0);
    double pan_pdf_y = g_PanY * (72.0 / 96.0);

    *outX = cx + rx + pan_pdf_x;
    *outY = cy + ry - pan_pdf_y; 
}

void sanitize_pdf_string(char* str) {
    while(*str) {
        if(*str == '(' || *str == ')' || *str == '\\') *str = '_';
        str++;
    }
}

void generate_pdf(const char* out_file, const char* title_text, float margH_pct, float margV_pct) {
    FILE* f = fopen(out_file, "wb");
    if (!f) return;
    
    float pt_w = page_w_mm * 72.0f / 25.4f;
    float pt_h = page_h_mm * 72.0f / 25.4f;

    float m_x = pt_w * (margH_pct / 100.0f);
    float m_y = pt_h * (margV_pct / 100.0f);
    float map_w = pt_w - (2 * m_x);
    float map_h = pt_h - (2 * m_y) - 40.0f; 
    float map_cx = m_x + (map_w / 2.0f);
    float map_cy = m_y + (map_h / 2.0f);
    
    double pdf_zoom = g_Zoom * (72.0 / 96.0); 

    fprintf(f, "%%PDF-1.1\n");
    int info_obj = 1, catalog_obj = 2, pages_obj = 3, font1_obj = 4, font2_obj = 5;
    pdf_obj_cnt = 6;

    pdf_objects[info_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Title (Map) /Creator (PBF-PDF) >>\nendobj\n", info_obj);

    pdf_objects[catalog_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Catalog /Pages %d 0 R >>\nendobj\n", catalog_obj, pages_obj);

    pdf_objects[font1_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n", font1_obj);

    pdf_objects[font2_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>\nendobj\n", font2_obj);

    Stream s = {0};
    int page_obj = pdf_obj_cnt++;
    int stream_obj = pdf_obj_cnt++;
    
    pdf_objects[page_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> >> >>\nendobj\n", 
        page_obj, pages_obj, pt_w, pt_h, stream_obj, font1_obj, font2_obj);

    pdf_center_text(&s, title_text, m_x, pt_h - m_y - 20, map_w, 24, 1);

    s_app(&s, "q %.2f %.2f %.2f %.2f re W n\n", m_x, m_y, map_w, map_h);
    pdf_color(&s, 0, GetRValue(c_Water), GetGValue(c_Water), GetBValue(c_Water)); 
    s_app(&s, "%.2f %.2f %.2f %.2f re f\n", m_x, m_y, map_w, map_h);
    s_app(&s, "1 J 1 j\n"); 

    for (int pass = 1; pass <= 5; pass++) {
        for (uint64_t i = 0; i < g_FeatureCount; i++) {
            MapFeature* feat = &g_MapFeatures[i];
            if (feat->point_count < 2) continue;
            
            bool is_closed = (fabs(feat->points[0].lon - feat->points[feat->point_count-1].lon) < 1e-5 && 
                              fabs(feat->points[0].lat - feat->points[feat->point_count-1].lat) < 1e-5);

            if (pass == 1) { 
                if (feat->feature_class == CLASS_LAND && !g_PdfExpLand) continue;
                if (feat->feature_class == CLASS_PARK && !g_PdfExpPark) continue;
                if (feat->feature_class == CLASS_WATER && !g_PdfExpWater) continue;
                if (feat->feature_class == CLASS_COASTLINE && is_closed && !g_PdfExpLand) continue;
                
                if (is_closed) {
                    if (feat->feature_class == CLASS_LAND || feat->feature_class == CLASS_COASTLINE) pdf_color(&s, 0, GetRValue(c_Land), GetGValue(c_Land), GetBValue(c_Land));
                    else if (feat->feature_class == CLASS_PARK) pdf_color(&s, 0, GetRValue(c_Park), GetGValue(c_Park), GetBValue(c_Park));
                    else if (feat->feature_class == CLASS_WATER) pdf_color(&s, 0, GetRValue(c_Water), GetGValue(c_Water), GetBValue(c_Water));
                    else continue; 
                } else continue;
            } 
            else if (pass == 2) { 
                if (!g_PdfExpCoast || is_closed || feat->feature_class != CLASS_COASTLINE) continue;
                pdf_color(&s, 1, GetRValue(c_Coast), GetGValue(c_Coast), GetBValue(c_Coast)); s_app(&s, "1.00 w\n");
            } 
            else if (pass == 3) {
                if (!g_PdfExpHwyMin || feat->feature_class != CLASS_HWY_MINOR) continue;
                if (g_Zoom < 2500.0) continue;
                pdf_color(&s, 1, GetRValue(c_HwyMin), GetGValue(c_HwyMin), GetBValue(c_HwyMin)); s_app(&s, "2.00 w\n");
            } 
            else if (pass == 4) {
                if (!g_PdfExpHwyMain || feat->feature_class != CLASS_HWY_MAIN) continue;
                pdf_color(&s, 1, GetRValue(c_HwyMain), GetGValue(c_HwyMain), GetBValue(c_HwyMain)); s_app(&s, "4.00 w\n");
            } 
            else if (pass == 5) {
                if (!g_PdfExpLabels) continue; 
                if (feat->feature_class == CLASS_HWY_MAIN && g_Zoom < 200.0) continue;
                if (feat->feature_class == CLASS_HWY_MINOR && g_Zoom < 2500.0) continue;
                if (!feat->name || (feat->feature_class != CLASS_HWY_MAIN && feat->feature_class != CLASS_HWY_MINOR)) continue;
            }

            double cx1, cy1, cx2, cy2, cx3, cy3, cx4, cy4;
            LatLonToPdf(feat->min_lon, feat->min_lat, map_cx, map_cy, pdf_zoom, &cx1, &cy1);
            LatLonToPdf(feat->min_lon, feat->max_lat, map_cx, map_cy, pdf_zoom, &cx2, &cy2);
            LatLonToPdf(feat->max_lon, feat->min_lat, map_cx, map_cy, pdf_zoom, &cx3, &cy3);
            LatLonToPdf(feat->max_lon, feat->max_lat, map_cx, map_cy, pdf_zoom, &cx4, &cy4);
            
            double min_x = fmin(fmin(cx1, cx2), fmin(cx3, cx4));
            double max_x = fmax(fmax(cx1, cx2), fmax(cx3, cx4));
            double min_y = fmin(fmin(cy1, cy2), fmin(cy3, cy4));
            double max_y = fmax(fmax(cy1, cy2), fmax(cy3, cy4));

            if (max_x < m_x || min_x > m_x + map_w || max_y < m_y || min_y > m_y + map_h) continue;

            double last_px = -99999, last_py = -99999;
            int pts_written = 0;
            double p1x = 0, p1y = 0, p2x = 0, p2y = 0;

            for (int j = 0; j < feat->point_count; j++) {
                double px, py;
                LatLonToPdf(feat->points[j].lon, feat->points[j].lat, map_cx, map_cy, pdf_zoom, &px, &py);
                
                if (j == 0 || j == feat->point_count - 1 || ((px - last_px)*(px - last_px) + (py - last_py)*(py - last_py) > 1.0)) {
                    if (pass == 5) {
                        if (pts_written == 0) { p1x = px; p1y = py; }
                        else { p2x = px; p2y = py; }
                    } else {
                        if (pts_written == 0) s_app(&s, "%.2f %.2f m\n", px, py);
                        else s_app(&s, "%.2f %.2f l\n", px, py);
                    }
                    last_px = px; last_py = py;
                    pts_written++;
                }
            }

            if (pass == 5 && pts_written >= 2) {
                double dx = p2x - p1x;
                double dy = p2y - p1y;
                if (dx*dx + dy*dy < 4000) continue; 

                double angle_rad = atan2(dy, dx);
                if (angle_rad < -1.5707) angle_rad += 3.14159;
                if (angle_rad > 1.5707) angle_rad -= 3.14159;

                double a = cos(angle_rad), b = sin(angle_rad), c = -sin(angle_rad), d = cos(angle_rad);
                double txt_cx = (p1x + p2x) / 2.0;
                double txt_cy = (p1y + p2y) / 2.0 + 3.0;

                char clean_name[128];
                strncpy(clean_name, feat->name, 127);
                clean_name[127] = '\0';
                sanitize_pdf_string(clean_name);

                s_app(&s, "BT %.4f %.4f %.4f %.4f %.2f %.2f Tm 0.2 0.2 0.2 rg /F2 8 Tf (%s) Tj ET\n", 
                    a, b, c, d, txt_cx, txt_cy, clean_name);
            } 
            else if (pass == 1) s_app(&s, "f\n");
            else if (pass != 5) s_app(&s, "S\n");
        }
    }
    
    if (g_RoutePathCount > 0) {
        pdf_color(&s, 1, GetRValue(c_Route), GetGValue(c_Route), GetBValue(c_Route));
        s_app(&s, "6.00 w\n");
        double px, py;
        LatLonToPdf(g_RoutePath[0].lon, g_RoutePath[0].lat, map_cx, map_cy, pdf_zoom, &px, &py);
        s_app(&s, "%.2f %.2f m\n", px, py);
        for (int j = 1; j < g_RoutePathCount; j++) {
            LatLonToPdf(g_RoutePath[j].lon, g_RoutePath[j].lat, map_cx, map_cy, pdf_zoom, &px, &py);
            s_app(&s, "%.2f %.2f l\n", px, py);
        }
        s_app(&s, "S\n");
    }

    if (g_SelectedPinCount >= 2 && g_RouteStepCount > 0) {
        float leg_w = 200.0f;
        float leg_h = map_h - 20.0f;
        
        pdf_color(&s, 0, 250, 248, 245);
        s_app(&s, "%.2f %.2f %.2f %.2f re f\n", m_x + 10, m_y + map_h - leg_h - 10, leg_w, leg_h);
        pdf_color(&s, 1, 200, 200, 200);
        s_app(&s, "1.0 w\n");
        s_app(&s, "%.2f %.2f %.2f %.2f re S\n", m_x + 10, m_y + map_h - leg_h - 10, leg_w, leg_h);

        pdf_color(&s, 0, 30, 30, 30);
        float text_y = m_y + map_h - 25;
        
        char hdr_str[128];
        snprintf(hdr_str, sizeof(hdr_str), "Total: %.1f %s", g_TotalRouteDist, g_UseMiles ? "mi" : "km");
        s_app(&s, "BT /F2 9 Tf %.2f %.2f Td (%s) Tj ET\n", m_x + 15, text_y, hdr_str);
        text_y -= 15;

        for (int step = 0; step < g_RouteStepCount; step++) {
            if (text_y < m_y + 15) break; 
            char step_str[300];
            snprintf(step_str, sizeof(step_str), "%d. %s (%.1f %s)", step+1, g_RouteSteps[step].text, g_RouteSteps[step].dist, g_UseMiles ? "mi" : "km");
            sanitize_pdf_string(step_str);
            s_app(&s, "BT /F1 8 Tf %.2f %.2f Td (%s) Tj ET\n", m_x + 15, text_y, step_str);
            text_y -= 12;
        }
    }

    s_app(&s, "Q\n"); 

    pdf_objects[stream_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Length %d >>\nstream\n%s\nendstream\nendobj\n", stream_obj, s.len, s.data);

    pdf_objects[pages_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Pages /Count 1 /Kids [ %d 0 R ] >>\nendobj\n", pages_obj, page_obj);

    long xref_pos = ftell(f);
    fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", pdf_obj_cnt);
    for(int i = 1; i < pdf_obj_cnt; i++) fprintf(f, "%010ld 00000 n \n", pdf_objects[i]);
    
    fprintf(f, "trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%ld\n%%%%EOF\n", pdf_obj_cnt, catalog_obj, info_obj, xref_pos);
    
    if(s.data) free(s.data);
    fclose(f);
}

void CreatePDFAction(HWND hwnd) {
    char out_path[MAX_PATH];
    GetWindowTextA(hTxtOut, out_path, MAX_PATH);

    char title_str[128];
    GetWindowTextA(hTxtTitle, title_str, 128);

    char margH_str[16], margV_str[16];
    GetWindowTextA(hTxtMargH, margH_str, 16);
    GetWindowTextA(hTxtMargV, margV_str, 16);

    g_PdfExpLand = (SendMessage(hChkLand, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpWater = (SendMessage(hChkWater, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpPark = (SendMessage(hChkPark, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpCoast = (SendMessage(hChkCoast, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpHwyMin = (SendMessage(hChkHwyMin, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpHwyMain = (SendMessage(hChkHwyMain, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpLabels = (SendMessage(hChkLabels, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    generate_pdf(out_path, title_str, atof(margH_str), atof(margV_str));
    ShowWindow(hwnd, SW_HIDE);
}

LRESULT CALLBACK PdfWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            CreateWindowA("STATIC", "Map Title:", WS_VISIBLE | WS_CHILD, 10, 15, 60, 20, hwnd, NULL, NULL, NULL);
            hTxtTitle = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "My Region", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 75, 15, 190, 22, hwnd, NULL, NULL, NULL);

            CreateWindowA("STATIC", "Out PDF:", WS_VISIBLE | WS_CHILD, 10, 45, 60, 20, hwnd, NULL, NULL, NULL);
            hTxtOut = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "map.pdf", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 75, 45, 120, 22, hwnd, NULL, NULL, NULL);
            hBtnBrowse = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD, 200, 45, 65, 22, hwnd, (HMENU)1, NULL, NULL);

            CreateWindowA("STATIC", "Size(mm):", WS_VISIBLE | WS_CHILD, 10, 75, 60, 20, hwnd, NULL, NULL, NULL);
            hTxtWidth = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "215.9", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 75, 75, 45, 22, hwnd, NULL, NULL, NULL);
            CreateWindowA("STATIC", "x", WS_VISIBLE | WS_CHILD, 125, 75, 10, 20, hwnd, NULL, NULL, NULL);
            hTxtHeight = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "279.4", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 140, 75, 45, 22, hwnd, NULL, NULL, NULL);
            
            hBtnToggle = CreateWindowA("BUTTON", "Land", WS_VISIBLE | WS_CHILD, 195, 75, 45, 22, hwnd, (HMENU)2, NULL, NULL);

            CreateWindowA("STATIC", "Marg(%):", WS_VISIBLE | WS_CHILD, 10, 105, 50, 20, hwnd, NULL, NULL, NULL);
            hTxtMargH = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "4", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 75, 105, 30, 22, hwnd, NULL, NULL, NULL);
            CreateWindowA("STATIC", "H", WS_VISIBLE | WS_CHILD, 110, 105, 15, 20, hwnd, NULL, NULL, NULL);
            hTxtMargV = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "4", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 130, 105, 30, 22, hwnd, NULL, NULL, NULL);
            CreateWindowA("STATIC", "V", WS_VISIBLE | WS_CHILD, 165, 105, 15, 20, hwnd, NULL, NULL, NULL);

            hChkLand = CreateWindowA("BUTTON", "Land", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 10, 135, 55, 20, hwnd, NULL, NULL, NULL);
            hChkWater = CreateWindowA("BUTTON", "Water", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 70, 135, 60, 20, hwnd, NULL, NULL, NULL);
            hChkPark = CreateWindowA("BUTTON", "Parks", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 135, 135, 55, 20, hwnd, NULL, NULL, NULL);
            hChkCoast = CreateWindowA("BUTTON", "Coast", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 195, 135, 60, 20, hwnd, NULL, NULL, NULL);
            hChkHwyMin = CreateWindowA("BUTTON", "Minor Rds", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 10, 160, 85, 20, hwnd, NULL, NULL, NULL);
            hChkHwyMain = CreateWindowA("BUTTON", "Main Rds", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 100, 160, 85, 20, hwnd, NULL, NULL, NULL);
            hChkLabels = CreateWindowA("BUTTON", "Labels", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 190, 160, 65, 20, hwnd, NULL, NULL, NULL);

            SendMessage(hChkLand, BM_SETCHECK, g_PdfExpLand ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkWater, BM_SETCHECK, g_PdfExpWater ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkPark, BM_SETCHECK, g_PdfExpPark ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkCoast, BM_SETCHECK, g_PdfExpCoast ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkHwyMin, BM_SETCHECK, g_PdfExpHwyMin ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkHwyMain, BM_SETCHECK, g_PdfExpHwyMain ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(hChkLabels, BM_SETCHECK, g_PdfExpLabels ? BST_CHECKED : BST_UNCHECKED, 0);

            hBtnCreatePdf = CreateWindowA("BUTTON", "Export PDF", WS_VISIBLE | WS_CHILD, 85, 190, 110, 30, hwnd, (HMENU)3, NULL, NULL);

            char buf[32];
            sprintf(buf, "%.2f", page_w_mm); SetWindowTextA(hTxtWidth, buf);
            sprintf(buf, "%.2f", page_h_mm); SetWindowTextA(hTxtHeight, buf);
            sprintf(buf, "%.1f", g_MargH); SetWindowTextA(hTxtMargH, buf);
            sprintf(buf, "%.1f", g_MargV); SetWindowTextA(hTxtMargV, buf);
            SetWindowTextA(hBtnToggle, isLandscape ? "Land" : "Port");

            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) { 
                OPENFILENAMEA ofn = {0};
                char path[MAX_PATH] = "";
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "PDF Files (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameA(&ofn)) SetWindowTextA(hTxtOut, path);
            } else if (LOWORD(wParam) == 2) { 
                isLandscape = !isLandscape;
                SetWindowTextA(hBtnToggle, isLandscape ? "Land" : "Port"); 
                char w_str[32], h_str[32];
                GetWindowTextA(hTxtWidth, w_str, 32); GetWindowTextA(hTxtHeight, h_str, 32);
                SetWindowTextA(hTxtWidth, h_str); SetWindowTextA(hTxtHeight, w_str);
                page_w_mm = atof(h_str); page_h_mm = atof(w_str);
            } else if (LOWORD(wParam) == 3) { 
                char w_str[32], h_str[32];
                GetWindowTextA(hTxtWidth, w_str, 32); GetWindowTextA(hTxtHeight, h_str, 32);
                page_w_mm = atof(w_str); page_h_mm = atof(h_str);
                CreatePDFAction(hwnd);
            }
            break;
        }
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ==========================================================================
 * 6. WIN32 GDI & MAIN WINDOW
 * ========================================================================== */
LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) { SetFocus(GetParent(hwnd)); return 0; }
    if (msg == WM_RBUTTONDOWN) { SetFocus(GetParent(hwnd)); return 0; }
    
    if (msg == WM_KILLFOCUS) {
        char buf[128];
        GetWindowTextA(hwnd, buf, 128);
        
        if (strlen(buf) == 0) {
            if (g_EditingPinIndex >= 0 && g_EditingPinIndex < g_PinCount) {
                int sel_idx = -1;
                for(int k=0; k<g_SelectedPinCount; k++) {
                    if (g_SelectedPins[k] == g_EditingPinIndex) sel_idx = k;
                }
                if (sel_idx >= 0) {
                    for(int k=sel_idx; k<g_SelectedPinCount-1; k++) g_SelectedPins[k] = g_SelectedPins[k+1];
                    g_SelectedPinCount--;
                }
                for(int k=0; k<g_SelectedPinCount; k++) {
                    if (g_SelectedPins[k] > g_EditingPinIndex) g_SelectedPins[k]--;
                }
                
                for(int i = g_EditingPinIndex; i < g_PinCount - 1; i++) g_Pins[i] = g_Pins[i+1];
                g_PinCount--;
                
                if (g_SelectedPinCount >= 2) {
                    CalculateRoute();
                } else {
                    g_RouteStepCount = 0; g_RoutePathCount = 0;
                    ShowWindow(hBtnCancel, SW_HIDE); 
                    ShowWindow(hCboRouteType, SW_HIDE);
                }
            }
        } else {
            if (g_EditingPinIndex >= 0) {
                strncpy(g_Pins[g_EditingPinIndex].name, buf, 127);
                g_Pins[g_EditingPinIndex].name[127] = '\0';
            }
        }
        
        g_EditingPinIndex = -1;
        DestroyWindow(hwnd);
        hEditPin = NULL;
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
        return 0;
    }
    return CallWindowProc(OldEditProc, hwnd, msg, wParam, lParam);
}

void AutoFitMap(HWND hwnd) {
    if (g_FeatureCount == 0) return;

    double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;

    for (uint64_t i = 0; i < g_FeatureCount; i++) {
        if (g_MapFeatures[i].min_lon < min_lon) min_lon = g_MapFeatures[i].min_lon;
        if (g_MapFeatures[i].max_lon > max_lon) max_lon = g_MapFeatures[i].max_lon;
        if (g_MapFeatures[i].min_lat < min_lat) min_lat = g_MapFeatures[i].min_lat;
        if (g_MapFeatures[i].max_lat > max_lat) max_lat = g_MapFeatures[i].max_lat;
    }

    g_CenterLon = (min_lon + max_lon) / 2.0;
    g_CenterLat = (min_lat + max_lat) / 2.0;

    if (!g_AutoFit) return;

    RECT rect; GetClientRect(hwnd, &rect);
    double width = (rect.right > 40) ? rect.right - 40 : rect.right;
    double height = (rect.bottom > 40) ? rect.bottom - 40 : rect.bottom;
    double zoomX = width / (max_lon - min_lon);
    double zoomY = height / (max_lat - min_lat);
    
    g_Zoom = (zoomX < zoomY) ? zoomX : zoomY;
    g_PanX = 0; g_PanY = 0; g_Rotation = 0;
}

void LatLonToScreen(double lon, double lat, int width, int height, double* outX, double* outY) {
    double dx = (lon - g_CenterLon) * g_Zoom;
    double dy = (lat - g_CenterLat) * g_Zoom;

    double cos_r = cos(g_Rotation);
    double sin_r = sin(g_Rotation);
    double rx = dx * cos_r - dy * sin_r;
    double ry = dx * sin_r + dy * cos_r;

    *outX = (width / 2.0) + rx + g_PanX;
    *outY = (height / 2.0) - ry + g_PanY; 
}

void ScreenToLatLon(int x, int y, int width, int height, double* outLon, double* outLat) {
    double rx = x - (width / 2.0) - g_PanX;
    double ry = (height / 2.0) - y + g_PanY;

    double cos_r = cos(-g_Rotation);
    double sin_r = sin(-g_Rotation);
    double dx = rx * cos_r - ry * sin_r;
    double dy = rx * sin_r + ry * cos_r;

    *outLon = g_CenterLon + dx / g_Zoom;
    *outLat = g_CenterLat + dy / g_Zoom;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hTxtSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 10, 12, 150, 22, hwnd, (HMENU)1004, NULL, NULL);
            hBtnSearchOpts = CreateWindowA("BUTTON", "..", WS_VISIBLE | WS_CHILD, 165, 12, 20, 22, hwnd, (HMENU)1005, NULL, NULL);
            hListSearch = CreateWindowExA(WS_EX_TOPMOST, "LISTBOX", "", WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, hwnd, (HMENU)1003, NULL, NULL);

            hBtnCancel = CreateWindowA("BUTTON", "Cancel Route", WS_VISIBLE | WS_CHILD, 195, 10, 100, 25, hwnd, (HMENU)4, NULL, NULL);
            hCboRouteType = CreateWindowA("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VISIBLE | WS_CHILD, 305, 12, 120, 100, hwnd, (HMENU)5, NULL, NULL);
            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Fastest Route");
            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Shortest Route");
            SendMessage(hCboRouteType, CB_SETCURSEL, g_RouteMode, 0);

            ShowWindow(hBtnCancel, SW_HIDE);
            ShowWindow(hCboRouteType, SW_HIDE);

            DragAcceptFiles(hwnd, TRUE);
            AutoFitMap(hwnd); 
            return 0;
        }
        case WM_TIMER: {
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                PopulateSearchList(hwnd);
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            char filepath[MAX_PATH];
            if (DragQueryFileA(hDrop, 0, filepath, MAX_PATH)) {
                if (strlen(g_MapFilesStr) + strlen(filepath) + 3 < sizeof(g_MapFilesStr)) {
                    if (strlen(g_MapFilesStr) > 0) strcat(g_MapFilesStr, ", ");
                    strcat(g_MapFilesStr, filepath);
                }
                g_PendingMapRestart = true;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            DragFinish(hDrop);
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1004 && HIWORD(wParam) == EN_CHANGE) {
                SetTimer(hwnd, 1, 500, NULL);
            } else if (LOWORD(wParam) == 1005) {
                HMENU hSearchMenu = CreatePopupMenu();
                AppendMenuA(hSearchMenu, MF_STRING | (g_SearchRoads ? MF_CHECKED : MF_UNCHECKED), 1001, "Road Names");
                AppendMenuA(hSearchMenu, MF_STRING | (g_SearchPostcodes ? MF_CHECKED : MF_UNCHECKED), 1002, "Postal Codes");
                RECT r; GetWindowRect(hBtnSearchOpts, &r);
                TrackPopupMenu(hSearchMenu, TPM_RIGHTBUTTON, r.left, r.bottom, 0, hwnd, NULL);
                DestroyMenu(hSearchMenu);
            } else if (LOWORD(wParam) == 1001) {
                g_SearchRoads = !g_SearchRoads;
                PopulateSearchList(hwnd);
            } else if (LOWORD(wParam) == 1002) {
                g_SearchPostcodes = !g_SearchPostcodes;
                PopulateSearchList(hwnd);
            } else if (LOWORD(wParam) == 1003 && HIWORD(wParam) == LBN_SELCHANGE) {
                int sel = SendMessage(hListSearch, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    uint64_t i = SendMessage(hListSearch, LB_GETITEMDATA, sel, 0);
                    g_HighlightedFeature = i;
                    MapFeature* f = &g_MapFeatures[i];
                    g_CenterLon = (f->min_lon + f->max_lon) / 2.0;
                    g_CenterLat = (f->min_lat + f->max_lat) / 2.0;
                    g_Zoom = 25000.0;
                    g_PanX = 0; g_PanY = 0;
                    ShowWindow(hListSearch, SW_HIDE);
                    SetFocus(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else if (LOWORD(wParam) == 4) { 
                g_SelectedPinCount = 0;
                g_RouteStepCount = 0;
                g_RoutePathCount = 0;
                ShowWindow(hBtnCancel, SW_HIDE);
                ShowWindow(hCboRouteType, SW_HIDE);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (LOWORD(wParam) == 5 && HIWORD(wParam) == CBN_SELCHANGE) { 
                g_RouteMode = SendMessage(hCboRouteType, CB_GETCURSEL, 0, 0);
                CalculateRoute();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case WM_SIZE: AutoFitMap(hwnd); InvalidateRect(hwnd, NULL, FALSE); return 0;
        case WM_KEYDOWN:
            if (wParam == VK_SPACE) { 
                g_AutoFit = true; 
                AutoFitMap(hwnd); 
                InvalidateRect(hwnd, NULL, FALSE); 
            } else if (wParam == 'P' && GetAsyncKeyState(VK_CONTROL)) {
                ShowWindow(hPdfDlg, SW_SHOW);
            } else if (wParam == VK_ESCAPE) {
                g_SelectedPinCount = 0;
                g_RouteStepCount = 0;
                g_RoutePathCount = 0;
                ShowWindow(hBtnCancel, SW_HIDE);
                ShowWindow(hCboRouteType, SW_HIDE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_RBUTTONDOWN:
            if (hEditPin) { SetFocus(hwnd); return 0; } 
            g_SelectedPinCount = 0;
            g_RouteStepCount = 0;
            g_RoutePathCount = 0;
            ShowWindow(hBtnCancel, SW_HIDE);
            ShowWindow(hCboRouteType, SW_HIDE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (hEditPin) SetFocus(hwnd); 
            g_IsDragging = true; 
            g_MouseMoved = false;
            g_LastMousePos.x = LOWORD(lParam); 
            g_LastMousePos.y = HIWORD(lParam);
            g_HighlightedFeature = -1;
            ShowWindow(hListSearch, SW_HIDE);
            SetCapture(hwnd); 
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_LBUTTONDBLCLK: {
            int mx = LOWORD(lParam), my = HIWORD(lParam);
            RECT rect; GetClientRect(hwnd, &rect);
            for (int i = 0; i < g_PinCount; i++) {
                double px, py;
                LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rect.right, rect.bottom, &px, &py);
                if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) {
                    g_EditingPinIndex = i;
                    hEditPin = CreateWindowExA(0, "EDIT", g_Pins[i].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 
                        (int)px - 50, (int)py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                    OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
                    SetFocus(hEditPin);
                    SendMessage(hEditPin, EM_SETSEL, 0, -1);
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            g_IsDragging = false; 
            ReleaseCapture(); 
            
            if (!g_MouseMoved && hEditPin == NULL) {
                int mx = LOWORD(lParam), my = HIWORD(lParam);
                bool hit_pin = false;
                RECT rect; GetClientRect(hwnd, &rect);
                
                for (int i = 0; i < g_PinCount; i++) {
                    double px, py;
                    LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rect.right, rect.bottom, &px, &py);
                    if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) { 
                        hit_pin = true; 
                        
                        bool already_sel = false;
                        for(int k=0; k<g_SelectedPinCount; k++) if(g_SelectedPins[k] == i) already_sel = true;
                        if (!already_sel && g_SelectedPinCount < 1000) {
                            g_SelectedPins[g_SelectedPinCount++] = i;
                            CalculateRoute();
                        }
                        InvalidateRect(hwnd, NULL, FALSE);
                        break; 
                    }
                }

                if (!hit_pin && g_PinCount < 1000) {
                    if (my < 45) return 0; 
                    if (g_SelectedPinCount >= 2 && mx < 280) return 0;

                    double lon, lat;
                    ScreenToLatLon(mx, my, rect.right, rect.bottom, &lon, &lat);
                    
                    g_EditingPinIndex = g_PinCount;
                    g_Pins[g_PinCount].lon = lon;
                    g_Pins[g_PinCount].lat = lat;
                    snprintf(g_Pins[g_PinCount].name, 128, "%d", g_PinCount + 1); 
                    g_PinCount++;

                    hEditPin = CreateWindowExA(0, "EDIT", g_Pins[g_PinCount-1].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 
                        mx - 50, my - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                    OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
                    SetFocus(hEditPin);
                    SendMessage(hEditPin, EM_SETSEL, 0, -1);
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_IsDragging) {
                if (abs(LOWORD(lParam) - g_LastMousePos.x) > 3 || abs(HIWORD(lParam) - g_LastMousePos.y) > 3) {
                    g_MouseMoved = true;
                }
                if (g_MouseMoved) {
                    if (wParam & MK_SHIFT) {
                        double delta = (LOWORD(lParam) - g_LastMousePos.x) * 0.01;
                        g_Rotation += delta;
                    } else {
                        g_PanX += LOWORD(lParam) - g_LastMousePos.x; 
                        g_PanY += HIWORD(lParam) - g_LastMousePos.y;
                    }
                    g_LastMousePos.x = LOWORD(lParam); g_LastMousePos.y = HIWORD(lParam);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int mx = LOWORD(lParam); 
            if (g_SelectedPinCount >= 2 && mx < 300) {
                float scroll_delta = ((int16_t)HIWORD(wParam) > 0) ? 30.0f : -30.0f;
                g_LegendScrollY += scroll_delta;
                if (g_LegendScrollY > 0) g_LegendScrollY = 0;
                if (g_LegendMaxScroll > 0 && g_LegendScrollY < -g_LegendMaxScroll) g_LegendScrollY = -g_LegendMaxScroll;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            double zoom_factor = ((int16_t)HIWORD(wParam) > 0) ? 1.2 : 0.8333333;
            RECT rect; GetClientRect(hwnd, &rect); POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            double rel_x = pt.x - (rect.right / 2.0), rel_y = pt.y - (rect.bottom / 2.0);
            g_PanX = rel_x - (rel_x - g_PanX) * zoom_factor; g_PanY = rel_y - (rel_y - g_PanY) * zoom_factor;
            g_Zoom *= zoom_factor;
            if (g_Zoom < 0.001) g_Zoom = 0.001; 
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rect; GetClientRect(hwnd, &rect);
            int width = rect.right, height = rect.bottom;
            
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            SetGraphicsMode(memDC, GM_ADVANCED); 
            SetPolyFillMode(memDC, WINDING);

            HBRUSH bgOceanBrush = CreateSolidBrush(c_Water); 
            FillRect(memDC, &rect, bgOceanBrush);
            DeleteObject(bgOceanBrush);

            if (g_FeatureCount == 0) {
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(0, 50, 150));
                SetTextAlign(memDC, TA_CENTER | TA_BASELINE);
                TextOut(memDC, width / 2, height / 2, "Drag a PBF map file here to open.", 33);

                if (g_PendingMapRestart) {
                    SetTextColor(memDC, RGB(220, 0, 0));
                    TextOut(memDC, width / 2, height / 2 + 30, "Map added to ini. The map will be loaded on next start.", 55);
                }

                BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); DeleteDC(memDC);
                EndPaint(hwnd, &ps); return 0;
            }

            double l1, t1, l2, t2, l3, t3, l4, t4;
            ScreenToLatLon(-50, -50, width, height, &l1, &t1);
            ScreenToLatLon(width+50, -50, width, height, &l2, &t2);
            ScreenToLatLon(-50, height+50, width, height, &l3, &t3);
            ScreenToLatLon(width+50, height+50, width, height, &l4, &t4);
            double vp_min_lon = fmin(fmin(l1, l2), fmin(l3, l4));
            double vp_max_lon = fmax(fmax(l1, l2), fmax(l3, l4));
            double vp_min_lat = fmin(fmin(t1, t2), fmin(t3, t4));
            double vp_max_lat = fmax(fmax(t1, t2), fmax(t3, t4));

            HBRUSH landBrush = CreateSolidBrush(c_Land);   
            HBRUSH parkBrush = CreateSolidBrush(c_Park);   
            HBRUSH waterBrush = CreateSolidBrush(c_Water);  
            HPEN nullPen = GetStockObject(NULL_PEN);
            
            LOGBRUSH lbCoast = { BS_SOLID, c_Coast, 0 };
            HPEN coastPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 2, &lbCoast, 0, NULL);
            
            LOGBRUSH lbMinor = { BS_SOLID, c_HwyMin, 0 };
            HPEN hwyMinorPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 4, &lbMinor, 0, NULL);
            
            LOGBRUSH lbMain = { BS_SOLID, c_HwyMain, 0 };      
            HPEN hwyMainPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 8, &lbMain, 0, NULL);

            static POINT* screen_pts = NULL;
            static int screen_pts_cap = 0;
            static RECT drawn_labels[4000]; 
            int label_cnt = 0;

            for (int pass = 1; pass <= 6; pass++) {
                for (uint64_t i = 0; i < g_FeatureCount; i++) {
                    MapFeature* f = &g_MapFeatures[i];
                    if (f->point_count < 2) continue;

                    if (f->max_lon < vp_min_lon || f->min_lon > vp_max_lon || 
                        f->max_lat < vp_min_lat || f->min_lat > vp_max_lat) continue;

                    double feat_w = f->max_lon - f->min_lon;
                    double feat_h = f->max_lat - f->min_lat;
                    if (f->feature_class != CLASS_LAND && f->feature_class != CLASS_COASTLINE && i != (uint64_t)g_HighlightedFeature) {
                        if (g_Zoom < 50.0 && feat_w < 0.005 && feat_h < 0.005) continue; 
                        if (g_Zoom < 10.0 && feat_w < 0.02 && feat_h < 0.02) continue; 
                        if (g_Zoom < 2.0 && feat_w < 0.1 && feat_h < 0.1) continue; 
                    }

                    bool is_closed = (fabs(f->points[0].lon - f->points[f->point_count-1].lon) < 1e-5 && 
                                      fabs(f->points[0].lat - f->points[f->point_count-1].lat) < 1e-5);
                    
                    if (pass == 1) { 
                        if (!g_ShowLand) continue;
                        if (!is_closed || (f->feature_class != CLASS_LAND && f->feature_class != CLASS_COASTLINE)) continue;
                    } else if (pass == 2) {
                        if (!is_closed) continue;
                        if (f->feature_class == CLASS_WATER && !g_ShowWater) continue;
                        if (f->feature_class == CLASS_PARK && !g_ShowPark) continue;
                        if (f->feature_class != CLASS_WATER && f->feature_class != CLASS_PARK) continue;
                    } else if (pass == 3) {
                        if (!g_ShowCoast || is_closed || f->feature_class != CLASS_COASTLINE) continue;
                    } else if (pass == 4) {
                        if (!g_ShowHwyMin || f->feature_class != CLASS_HWY_MINOR) continue;
                        if (g_Zoom < 2500.0) continue; 
                    } else if (pass == 5) {
                        if (!g_ShowHwyMain || f->feature_class != CLASS_HWY_MAIN) continue;
                    } else if (pass == 6) {
                        if (i == (uint64_t)g_HighlightedFeature) {
                            // Highlight pass proceeds
                        } else {
                            if (!g_ShowLabels) continue;
                            if (f->feature_class == CLASS_HWY_MAIN && g_Zoom < 200.0) continue;
                            if (f->feature_class == CLASS_HWY_MINOR && g_Zoom < 2500.0) continue;
                            if (!f->name || (f->feature_class != CLASS_HWY_MAIN && f->feature_class != CLASS_HWY_MINOR)) continue;
                        }
                    }

                    if (f->point_count > screen_pts_cap) {
                        screen_pts_cap = f->point_count + 1024;
                        screen_pts = (POINT*)realloc(screen_pts, screen_pts_cap * sizeof(POINT));
                    }

                    int step = 1;
                    if (f->point_count > 4000) {
                        step = f->point_count / 4000;
                        if (step < 1) step = 1;
                    }

                    int pt_idx = 0;
                    for (int j = 0; j < f->point_count; j += step) {
                        double px, py;
                        LatLonToScreen(f->points[j].lon, f->points[j].lat, width, height, &px, &py);
                        screen_pts[pt_idx].x = (int)px;
                        screen_pts[pt_idx].y = (int)py;
                        pt_idx++;
                    }
                    
                    if (is_closed && (f->point_count - 1) % step != 0) {
                        double px, py;
                        LatLonToScreen(f->points[f->point_count-1].lon, f->points[f->point_count-1].lat, width, height, &px, &py);
                        screen_pts[pt_idx].x = (int)px;
                        screen_pts[pt_idx].y = (int)py;
                        pt_idx++;
                    }
                    
                    if (pass == 6) {
                        if (i == (uint64_t)g_HighlightedFeature) {
                            HPEN hlPen = CreatePen(PS_SOLID, 8, RGB(255, 50, 50));
                            SelectObject(memDC, GetStockObject(NULL_BRUSH)); 
                            SelectObject(memDC, hlPen);
                            Polyline(memDC, screen_pts, pt_idx);
                            DeleteObject(hlPen);
                            continue;
                        }

                        double max_len = 0; int best_i = 0;
                        for (int j = 0; j < pt_idx - 1; j++) {
                            double dx = screen_pts[j+1].x - screen_pts[j].x;
                            double dy = screen_pts[j+1].y - screen_pts[j].y;
                            double len = dx*dx + dy*dy;
                            if (len > max_len) { max_len = len; best_i = j; }
                        }

                        if (max_len < 12000) continue; 

                        int p1x = screen_pts[best_i].x, p1y = screen_pts[best_i].y;
                        int p2x = screen_pts[best_i+1].x, p2y = screen_pts[best_i+1].y;

                        double angle_rad = atan2(p1y - p2y, p2x - p1x); 
                        double angle_deg = angle_rad * 180.0 / 3.1415926535;

                        if (angle_deg < 0) angle_deg += 360.0;
                        if (angle_deg > 90.0 && angle_deg < 270.0) angle_deg -= 180.0; 

                        int cx = (p1x + p2x) / 2;
                        int cy = (p1y + p2y) / 2;

                        HFONT hFont = CreateFont(16, 0, (int)(angle_deg * 10), (int)(angle_deg * 10), FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                        HFONT hOldFont = (HFONT)SelectObject(memDC, hFont);

                        SIZE sz;
                        GetTextExtentPoint32(memDC, f->name, strlen(f->name), &sz);

                        int hw = sz.cx / 2, hh = sz.cy / 2;
                        RECT tr = { cx - hw - 10, cy - hh - 10, cx + hw + 10, cy + hh + 10 };

                        bool collision = false;
                        for (int k = 0; k < label_cnt; k++) {
                            RECT dummy;
                            if (IntersectRect(&dummy, &tr, &drawn_labels[k])) { collision = true; break; }
                        }

                        if (!collision) {
                            if (label_cnt < 4000) drawn_labels[label_cnt++] = tr;
                            SetTextColor(memDC, RGB(40, 40, 40));
                            SetBkMode(memDC, TRANSPARENT);
                            SetTextAlign(memDC, TA_CENTER | TA_BASELINE);
                            TextOut(memDC, cx, cy + (sz.cy/3), f->name, strlen(f->name));
                        }
                        
                        SelectObject(memDC, hOldFont);
                        DeleteObject(hFont);
                        continue;
                    }

                    if (pass == 1) { 
                        if (g_Zoom < 10.0 && f->point_count > 5000) continue; 
                        SelectObject(memDC, landBrush); SelectObject(memDC, nullPen);
                        Polygon(memDC, screen_pts, pt_idx);
                    } else if (pass == 2) { 
                        SelectObject(memDC, (f->feature_class == CLASS_WATER) ? waterBrush : parkBrush);
                        SelectObject(memDC, nullPen);
                        Polygon(memDC, screen_pts, pt_idx);
                    } else if (pass == 3) {
                        SelectObject(memDC, GetStockObject(NULL_BRUSH)); SelectObject(memDC, coastPen); 
                        Polyline(memDC, screen_pts, pt_idx);
                    } else if (pass == 4) {
                        SelectObject(memDC, GetStockObject(NULL_BRUSH)); SelectObject(memDC, hwyMinorPen); 
                        Polyline(memDC, screen_pts, pt_idx);
                    } else if (pass == 5) {
                        SelectObject(memDC, GetStockObject(NULL_BRUSH)); SelectObject(memDC, hwyMainPen); 
                        Polyline(memDC, screen_pts, pt_idx);
                    }
                }
            }

            if (g_RoutePathCount > 0) {
                LOGBRUSH lbRoute = { BS_SOLID, c_Route, 0 }; 
                HPEN routePen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 6, &lbRoute, 0, NULL);
                SelectObject(memDC, GetStockObject(NULL_BRUSH)); 
                SelectObject(memDC, routePen);

                if (g_RoutePathCount > screen_pts_cap) {
                    screen_pts_cap = g_RoutePathCount + 1024;
                    screen_pts = (POINT*)realloc(screen_pts, screen_pts_cap * sizeof(POINT));
                }
                for (int j = 0; j < g_RoutePathCount; j++) {
                    double px, py;
                    LatLonToScreen(g_RoutePath[j].lon, g_RoutePath[j].lat, width, height, &px, &py);
                    screen_pts[j].x = (int)px;
                    screen_pts[j].y = (int)py;
                }
                
                Polyline(memDC, screen_pts, g_RoutePathCount);
                DeleteObject(routePen);
            }

            HBRUSH pinBrush = CreateSolidBrush(c_Pin);
            HPEN pinPen = CreatePen(PS_SOLID, 1, RGB(100, 0, 0));
            HBRUSH pinSelBrush = CreateSolidBrush(c_PinSel);
            HPEN pinSelPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 100));

            for (int i = 0; i < g_PinCount; i++) {
                double px, py;
                LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, width, height, &px, &py);
                int sx = (int)px, sy = (int)py;

                bool selected = false;
                for(int k=0; k<g_SelectedPinCount; k++) if(g_SelectedPins[k] == i) selected = true;

                SelectObject(memDC, selected ? pinSelBrush : pinBrush);
                SelectObject(memDC, selected ? pinSelPen : pinPen);

                POINT pt[4];
                pt[0].x = sx;      pt[0].y = sy;
                pt[1].x = sx - 10; pt[1].y = sy - 20;
                pt[2].x = sx;      pt[2].y = sy - 30;
                pt[3].x = sx + 10; pt[3].y = sy - 20;
                Polygon(memDC, pt, 4);
                
                if (i != g_EditingPinIndex && strlen(g_Pins[i].name) > 0) {
                    SetTextColor(memDC, RGB(20, 20, 20));
                    SetBkMode(memDC, TRANSPARENT);
                    SetTextAlign(memDC, TA_CENTER | TA_BOTTOM);
                    TextOut(memDC, sx, sy - 32, g_Pins[i].name, strlen(g_Pins[i].name));
                }
            }
            DeleteObject(pinBrush); DeleteObject(pinPen);
            DeleteObject(pinSelBrush); DeleteObject(pinSelPen);

            if (g_SelectedPinCount >= 2 && g_RouteStepCount > 0) {
                int leg_w = 260;
                int max_leg_h = height - 100;
                if (max_leg_h < 100) max_leg_h = 100;

                int req_h = 40 + (g_RouteStepCount * 20); 
                int leg_h = (req_h < max_leg_h) ? req_h : max_leg_h;

                RECT legRect = { 10, 45, 10 + leg_w, 45 + leg_h };
                
                HBRUSH legBrush = CreateSolidBrush(RGB(250, 248, 245));
                HPEN legPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
                SelectObject(memDC, legBrush);
                SelectObject(memDC, legPen);
                RoundRect(memDC, legRect.left, legRect.top, legRect.right, legRect.bottom, 15, 15);

                if (req_h > max_leg_h) {
                    g_LegendMaxScroll = (float)(req_h - max_leg_h + 10);
                    if (g_LegendScrollY < -g_LegendMaxScroll) g_LegendScrollY = -g_LegendMaxScroll;
                } else {
                    g_LegendMaxScroll = 0;
                    g_LegendScrollY = 0;
                }

                if (g_LegendMaxScroll > 0) {
                    HBRUSH sbBg = CreateSolidBrush(RGB(230, 230, 230));
                    HBRUSH sbFg = CreateSolidBrush(RGB(180, 180, 180));
                    RECT sbTrack = { legRect.right - 12, legRect.top + 10, legRect.right - 4, legRect.bottom - 10 };
                    FillRect(memDC, &sbTrack, sbBg);
                    
                    float visible_ratio = (float)leg_h / (float)req_h;
                    int thumb_h = (int)((leg_h - 20) * visible_ratio);
                    if (thumb_h < 20) thumb_h = 20;
                    
                    float scroll_ratio = -g_LegendScrollY / g_LegendMaxScroll;
                    int thumb_y = sbTrack.top + (int)(scroll_ratio * (sbTrack.bottom - sbTrack.top - thumb_h));
                    
                    RECT sbThumb = { sbTrack.left, thumb_y, sbTrack.right, thumb_y + thumb_h };
                    FillRect(memDC, &sbThumb, sbFg);
                    
                    DeleteObject(sbBg);
                    DeleteObject(sbFg);
                }
                
                HRGN clipRgn = CreateRectRgn(legRect.left + 5, legRect.top + 5, legRect.right - (g_LegendMaxScroll > 0 ? 15 : 5), legRect.bottom - 5);
                SelectClipRgn(memDC, clipRgn);

                SetTextColor(memDC, RGB(30, 30, 30));
                SetTextAlign(memDC, TA_LEFT | TA_TOP);
                
                HFONT hBoldFont = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                HFONT hOldF = (HFONT)SelectObject(memDC, hBoldFont);

                char hdr[128];
                snprintf(hdr, sizeof(hdr), "Total Distance: %.1f %s", g_TotalRouteDist, g_UseMiles ? "mi" : "km");
                TextOut(memDC, 20, 55 + (int)g_LegendScrollY, hdr, strlen(hdr));

                SelectObject(memDC, hOldF);
                DeleteObject(hBoldFont);

                float text_y = 80 + g_LegendScrollY;
                for (int s = 0; s < g_RouteStepCount; s++) {
                    char step_str[300];
                    snprintf(step_str, sizeof(step_str), "%d. %s (%.1f %s)", s+1, g_RouteSteps[s].text, g_RouteSteps[s].dist, g_UseMiles ? "mi" : "km");
                    
                    if (text_y > 40 && text_y < legRect.bottom) {
                        TextOut(memDC, 20, (int)text_y, step_str, strlen(step_str));
                    }
                    text_y += 20;
                }
                
                SelectClipRgn(memDC, NULL);
                DeleteObject(clipRgn);
                DeleteObject(legBrush);
                DeleteObject(legPen);
            }

            if (g_ShowDebug) {
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(0, 0, 0));
                SetTextAlign(memDC, TA_LEFT | TA_TOP);
                char overlay[256];
                sprintf(overlay, "Bare-Metal PBF Vector Engine\nStatistics: %llu Features Extracted", g_FeatureCount);
                RECT textRect = { 460, 10, 800, 100 }; 
                DrawText(memDC, overlay, -1, &textRect, DT_LEFT | DT_TOP);
            }

            if (g_PendingMapRestart) {
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(220, 0, 0));
                SetTextAlign(memDC, TA_CENTER | TA_TOP);
                TextOut(memDC, width / 2, 80, "Map added to ini. The map will be loaded on next start.", 55);
            }

            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            
            DeleteObject(landBrush); DeleteObject(parkBrush); DeleteObject(waterBrush);
            DeleteObject(coastPen); DeleteObject(hwyMinorPen); DeleteObject(hwyMainPen);
            SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); DeleteDC(memDC);
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_DESTROY: {
            FILE* ini = fopen("pbf_viewer.ini", "w");
            if (ini) {
                fprintf(ini, "maps=%s\n", g_MapFilesStr);
                fprintf(ini, "panX=%.4f\n", g_PanX);
                fprintf(ini, "panY=%.4f\n", g_PanY);
                fprintf(ini, "zoom=%.4f\n", g_Zoom);
                fprintf(ini, "rotation=%.6f\n", g_Rotation);
                
                RECT wrect; GetWindowRect(hwnd, &wrect);
                fprintf(ini, "winX=%d\n", (int)wrect.left);
                fprintf(ini, "winY=%d\n", (int)wrect.top);
                fprintf(ini, "winW=%d\n", (int)(wrect.right - wrect.left));
                fprintf(ini, "winH=%d\n", (int)(wrect.bottom - wrect.top));

                fprintf(ini, "pageW=%.2f\n", page_w_mm);
                fprintf(ini, "pageH=%.2f\n", page_h_mm);
                fprintf(ini, "margH=%.2f\n", g_MargH);
                fprintf(ini, "margV=%.2f\n", g_MargV);
                fprintf(ini, "isLandscape=%d\n", isLandscape);

                fprintf(ini, "showLand=%d\n", g_ShowLand);
                fprintf(ini, "showWater=%d\n", g_ShowWater);
                fprintf(ini, "showPark=%d\n", g_ShowPark);
                fprintf(ini, "showCoast=%d\n", g_ShowCoast);
                fprintf(ini, "showHwyMin=%d\n", g_ShowHwyMin);
                fprintf(ini, "showHwyMain=%d\n", g_ShowHwyMain);
                fprintf(ini, "showLabels=%d\n", g_ShowLabels);

                fprintf(ini, "pdfLand=%d\n", g_PdfExpLand);
                fprintf(ini, "pdfWater=%d\n", g_PdfExpWater);
                fprintf(ini, "pdfPark=%d\n", g_PdfExpPark);
                fprintf(ini, "pdfCoast=%d\n", g_PdfExpCoast);
                fprintf(ini, "pdfHwyMin=%d\n", g_PdfExpHwyMin);
                fprintf(ini, "pdfHwyMain=%d\n", g_PdfExpHwyMain);
                fprintf(ini, "pdfLabels=%d\n", g_PdfExpLabels);
                
                fprintf(ini, "showDebug=%d\n", g_ShowDebug);
                fprintf(ini, "routeMode=%d\n", g_RouteMode);

                char hex[16];
                ColorToHex(c_Land, hex); fprintf(ini, "colLand=%s\n", hex);
                ColorToHex(c_Water, hex); fprintf(ini, "colWater=%s\n", hex);
                ColorToHex(c_Park, hex); fprintf(ini, "colPark=%s\n", hex);
                ColorToHex(c_Coast, hex); fprintf(ini, "colCoast=%s\n", hex);
                ColorToHex(c_HwyMin, hex); fprintf(ini, "colHwyMin=%s\n", hex);
                ColorToHex(c_HwyMain, hex); fprintf(ini, "colHwyMain=%s\n", hex);
                ColorToHex(c_Pin, hex); fprintf(ini, "colPin=%s\n", hex);
                ColorToHex(c_PinSel, hex); fprintf(ini, "colPinSel=%s\n", hex);
                ColorToHex(c_Route, hex); fprintf(ini, "colRoute=%s\n", hex);

                char pinsStr[8192] = ""; 
                for (int i = 0; i < g_PinCount; i++) {
                    char temp[256];
                    snprintf(temp, sizeof(temp), "%.127s:long:%.7f:lat:%.7f|", g_Pins[i].name, g_Pins[i].lon, g_Pins[i].lat);
                    if (strlen(pinsStr) + strlen(temp) < sizeof(pinsStr)) {
                        strcat(pinsStr, temp);
                    }
                }
                if (strlen(pinsStr) > 0) pinsStr[strlen(pinsStr)-1] = '\0';
                fprintf(ini, "pins=%s\n", pinsStr);

                char routeStr[8192] = ""; 
                for (int i = 0; i < g_SelectedPinCount; i++) {
                    if (i > 0) strcat(routeStr, "|");
                    if (strlen(routeStr) + strlen(g_Pins[g_SelectedPins[i]].name) < sizeof(routeStr)) {
                        strcat(routeStr, g_Pins[g_SelectedPins[i]].name);
                    }
                }
                fprintf(ini, "route=%s\n", routeStr);

                fclose(ini);
            }
            PostQuitMessage(0); 
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
/* ==========================================================================
 * 7. PROGRAM ENTRY
 * ========================================================================== */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    
    FILE* ini = fopen("pbf_viewer.ini", "r");
    if (ini) {
        char line[8192];
        while (fgets(line, sizeof(line), ini)) {
            if (strncmp(line, "maps=", 5) == 0) {
                line[strcspn(line, "\r\n")] = 0; 
                strcpy(g_MapFilesStr, line + 5);
            }
            else if (strncmp(line, "panX=", 5) == 0) g_PanX = atof(line + 5);
            else if (strncmp(line, "panY=", 5) == 0) g_PanY = atof(line + 5);
            else if (strncmp(line, "zoom=", 5) == 0) {
                g_Zoom = atof(line + 5);
                if (g_Zoom > 0) g_AutoFit = false; 
            }
            else if (strncmp(line, "rotation=", 9) == 0) g_Rotation = atof(line + 9);
            else if (strncmp(line, "winX=", 5) == 0) g_WinX = atoi(line + 5);
            else if (strncmp(line, "winY=", 5) == 0) g_WinY = atoi(line + 5);
            else if (strncmp(line, "winW=", 5) == 0) g_WinW = atoi(line + 5);
            else if (strncmp(line, "winH=", 5) == 0) g_WinH = atoi(line + 5);
            else if (strncmp(line, "pageW=", 6) == 0) page_w_mm = atof(line + 6);
            else if (strncmp(line, "pageH=", 6) == 0) page_h_mm = atof(line + 6);
            else if (strncmp(line, "isLandscape=", 12) == 0) isLandscape = atoi(line + 12);
            else if (strncmp(line, "margH=", 6) == 0) g_MargH = atof(line + 6);
            else if (strncmp(line, "margV=", 6) == 0) g_MargV = atof(line + 6);
            
            else if (strncmp(line, "showLand=", 9) == 0) g_ShowLand = atoi(line + 9);
            else if (strncmp(line, "showWater=", 10) == 0) g_ShowWater = atoi(line + 10);
            else if (strncmp(line, "showPark=", 9) == 0) g_ShowPark = atoi(line + 9);
            else if (strncmp(line, "showCoast=", 10) == 0) g_ShowCoast = atoi(line + 10);
            else if (strncmp(line, "showHwyMin=", 11) == 0) g_ShowHwyMin = atoi(line + 11);
            else if (strncmp(line, "showHwyMain=", 12) == 0) g_ShowHwyMain = atoi(line + 12);
            else if (strncmp(line, "showLabels=", 11) == 0) g_ShowLabels = atoi(line + 11);

            else if (strncmp(line, "pdfLand=", 8) == 0) g_PdfExpLand = atoi(line + 8);
            else if (strncmp(line, "pdfWater=", 9) == 0) g_PdfExpWater = atoi(line + 9);
            else if (strncmp(line, "pdfPark=", 8) == 0) g_PdfExpPark = atoi(line + 8);
            else if (strncmp(line, "pdfCoast=", 9) == 0) g_PdfExpCoast = atoi(line + 9);
            else if (strncmp(line, "pdfHwyMin=", 10) == 0) g_PdfExpHwyMin = atoi(line + 10);
            else if (strncmp(line, "pdfHwyMain=", 11) == 0) g_PdfExpHwyMain = atoi(line + 11);
            else if (strncmp(line, "pdfLabels=", 10) == 0) g_PdfExpLabels = atoi(line + 10);
            else if (strncmp(line, "showDebug=", 10) == 0) g_ShowDebug = atoi(line + 10);
            else if (strncmp(line, "routeMode=", 10) == 0) g_RouteMode = atoi(line + 10);
            else if (strncmp(line, "colLand=", 8) == 0) c_Land = ParseHexColor(line + 8, c_Land);
            else if (strncmp(line, "colWater=", 9) == 0) c_Water = ParseHexColor(line + 9, c_Water);
            else if (strncmp(line, "colPark=", 8) == 0) c_Park = ParseHexColor(line + 8, c_Park);
            else if (strncmp(line, "colCoast=", 9) == 0) c_Coast = ParseHexColor(line + 9, c_Coast);
            else if (strncmp(line, "colHwyMin=", 10) == 0) c_HwyMin = ParseHexColor(line + 10, c_HwyMin);
            else if (strncmp(line, "colHwyMain=", 11) == 0) c_HwyMain = ParseHexColor(line + 11, c_HwyMain);
            else if (strncmp(line, "colPin=", 7) == 0) c_Pin = ParseHexColor(line + 7, c_Pin);
            else if (strncmp(line, "colPinSel=", 10) == 0) c_PinSel = ParseHexColor(line + 10, c_PinSel);
            else if (strncmp(line, "colRoute=", 9) == 0) c_Route = ParseHexColor(line + 9, c_Route);
            else if (strncmp(line, "pins=", 5) == 0) {
                line[strcspn(line, "\r\n")] = 0;
                char* token = strtok(line + 5, "|");
                while (token && g_PinCount < 1000) {
                    char* pLong = strstr(token, ":long:");
                    char* pLat = strstr(token, ":lat:");
                    if (pLong && pLat) {
                        *pLong = '\0';
                        strncpy(g_Pins[g_PinCount].name, token, 127);
                        g_Pins[g_PinCount].lon = atof(pLong + 6);
                        g_Pins[g_PinCount].lat = atof(pLat + 5);
                        g_PinCount++;
                    }
                    token = strtok(NULL, "|");
                }
            }
            else if (strncmp(line, "route=", 6) == 0) {
                line[strcspn(line, "\r\n")] = 0;
                strcpy(g_SavedRouteStr, line + 6);
            }
        }
        fclose(ini);

        if (strlen(g_SavedRouteStr) > 0) {
            char* token = strtok(g_SavedRouteStr, "|");
            while (token && g_SelectedPinCount < 1000) {
                for (int i = 0; i < g_PinCount; i++) {
                    if (strcmp(g_Pins[i].name, token) == 0) {
                        g_SelectedPins[g_SelectedPinCount++] = i;
                        break;
                    }
                }
                token = strtok(NULL, "|");
            }
        }

        if (strlen(g_MapFilesStr) > 0) {
            char tempMaps[2048];
            strcpy(tempMaps, g_MapFilesStr);
            
            char* token = strtok(tempMaps, ", ");
            while (token) {
                LoadPbfData(token);
                token = strtok(NULL, ", ");
            }
            if (g_SelectedPinCount >= 2) CalculateRoute();
        }
    } else if (strlen(lpCmdLine) > 0) {
        strcpy(g_MapFilesStr, lpCmdLine);
        LoadPbfData(lpCmdLine);
    } 

    WNDCLASSA wcd = {0};
    wcd.lpfnWndProc = PdfWndProc; wcd.hInstance = hInstance;
    wcd.lpszClassName = "PdfDialogClass"; wcd.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wcd.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wcd);

    hPdfDlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "PdfDialogClass", "Export PDF Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 
        CW_USEDEFAULT, CW_USEDEFAULT, 290, 270, NULL, NULL, hInstance, NULL);

    const char* CLASS_NAME = "PbfViewerClass";
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.style = CS_DBLCLKS; 
    wc.lpszClassName = CLASS_NAME; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, "Bare-Metal Win32 PBF Viewer",
        WS_OVERLAPPEDWINDOW, g_WinX, g_WinY, g_WinW, g_WinH, NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);
    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}