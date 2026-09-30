/* ============================================================================
 * qjmnavig - QJM Map Navigator
 * 
 * COMPILATION:
 *   gcc -O3 -m32 -Wall -Wextra qjmnavig.c -o qjmnavig.exe -lcomdlg32 -lgdi32 -Wl,--large-address-aware
 *
 * FEATURES:
 *  - High-Speed QJMC Spatial Block Loader with LRU Caching
 *  - Snap-And-Fill Routing: Traces physical road geometries around curves.
 *  - Trackpad-friendly Double-Click pin dropping.
 *  - Pipe-routing search parsing with intelligent global coordinate extraction.
 *  - Automated distance calculation and clipboard integration.
 *  - PDF Export.
 *
 * THIS WORK IS NOT FIT FOR ANY FUNCTION OR PURPOSE, COMES WITH NO WARRANTY,
 * AND IS BEING RELEASED INTO THE PUBLIC DOMAIN.
 * ============================================================================ */

#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lzav.h"
#ifdef _WIN32
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64
#else
#define FSEEK64 fseeko
#define FTELL64 ftello
#endif
#define CLASS_UNKNOWN   0
#define CLASS_WATER     1
#define CLASS_LAND      2
#define CLASS_PARK      3
#define CLASS_COASTLINE 4
#define CLASS_HWY_MINOR 5
#define CLASS_HWY_MAIN  6
#define R_EARTH 6371.0 
#define GEOM_POINT 0
#define GEOM_LINE  1
#define GEOM_AREA  2
/* ==========================================================================
 * 1. SPATIAL DATA STRUCTURES & GLOBALS
 * ========================================================================== */
typedef struct { double lon, lat; } MapPoint;

typedef struct {
    int feature_class;
    char* name;
    char* postcode;
    MapPoint* points;
    uint32_t point_count;
    double min_lon, max_lon; 
    double min_lat, max_lat;
} MapFeature;

#pragma pack(push, 1)
typedef struct { 
    double min_lon, max_lon, min_lat, max_lat; 
    uint64_t offset; 
    uint32_t comp_size, uncomp_size; 
} ChunkMetaDisk;
#pragma pack(pop)
typedef struct { 
    double min_lon, max_lon, min_lat, max_lat; 
    uint64_t offset; 
    uint32_t comp_size, uncomp_size; 
    FILE* file;
} MapChunk;

typedef struct { char name[128]; double lon, lat; } MapPin;
typedef struct { char text[256]; double dist; } RouteStep;
typedef struct { char* data; int len; int cap; } Stream;

typedef struct {
    char* text;
    uint32_t chunk_idx;
    uint32_t feat_idx;
    double lon, lat;
} SearchItem;

// --- Map Data Globals ---
MapChunk* g_Chunks = NULL;
uint32_t g_ChunkCount = 0;
uint32_t g_ChunkCap = 0;

SearchItem* g_SearchItems = NULL;
uint32_t g_SearchItemCount = 0;
uint32_t g_SearchItemCap = 0;

int g_HighlightedChunk = -1;
int g_HighlightedFeat = -1;

#define LRU_SIZE 512
typedef struct {
    int chunk_idx;
    MapFeature* features;
    uint32_t feature_count;
    uint64_t last_accessed;
} LRUCacheEntry;

LRUCacheEntry g_LRU[LRU_SIZE];
uint64_t g_FrameCounter = 0;

// --- Application View State ---
double g_CenterLon = 0.0, g_CenterLat = 0.0;
double g_PanX = 0.0000, g_PanY = 0.0000, g_Zoom = 5.0000, g_Rotation = 0.000000;
bool g_AutoFit = true;
int g_WinX = 26, g_WinY = 26, g_WinW = 1024, g_WinH = 768;

float page_w_mm = 215.90f, page_h_mm = 279.40f;
float g_MargH = 4.00f, g_MargV = 4.00f;
int isLandscape = 0;
int g_ShowLand = 1, g_ShowWater = 1, g_ShowPark = 1, g_ShowCoast = 1, g_ShowHwyMin = 1, g_ShowHwyMain = 1, g_ShowLabels = 1;
int g_PdfExpLand = 1, g_PdfExpWater = 1, g_PdfExpPark = 1, g_PdfExpCoast = 1, g_PdfExpHwyMin = 1, g_PdfExpHwyMain = 1, g_PdfExpLabels = 1;
int g_ShowDebug = 0;
int g_RouteMode = 0;
bool g_SavePins = true;

char g_MapFilesStr[4096] = "";
bool g_IsDragging = false, g_MouseMoved = false;
POINT g_LastMousePos;

COLORREF c_Land = RGB(0xF2, 0xF0, 0xE9);
COLORREF c_Water = RGB(0xA3, 0xCC, 0xFF);
COLORREF c_Park = RGB(0xCA, 0xDF, 0xAA);
COLORREF c_Coast = RGB(0x64, 0x96, 0xB4);
COLORREF c_HwyMin = RGB(0xFF, 0xFF, 0xFF);
COLORREF c_HwyMain = RGB(0xFD, 0xB8, 0x13);
COLORREF c_Pin = RGB(0xDC, 0x32, 0x32);
COLORREF c_PinSel = RGB(0x32, 0x64, 0xFF);
COLORREF c_Route = RGB(0x8A, 0x2B, 0xE2);

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

double g_TotalRouteDist = 0.0;
bool g_UseMiles = false;

float g_LegendScrollY = 0.0f;
float g_LegendMaxScroll = 0.0f;

HWND hPdfDlg = NULL, hTxtOut, hBtnBrowse, hTxtTitle;
HWND hTxtWidth, hTxtHeight, hBtnToggle, hTxtMargH, hTxtMargV, hBtnCreatePdf;
HWND hChkLand, hChkWater, hChkPark, hChkCoast, hChkHwyMin, hChkHwyMain, hChkLabels;
HWND hTxtSearch, hBtnSearchOpts, hListSearch, hBtnRoute, hBtnCancel, hCboRouteType;

bool g_SearchRoads = true, g_SearchPostcodes = true;

long pdf_objects[4096];
int pdf_obj_cnt = 1;

COLORREF ParseHexColor(const char* hex, COLORREF def) {
    if (!hex || strlen(hex) < 6) return def;
    int r, g, b;
    if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) return RGB(r, g, b); 
    return def;
}
void ColorToHex(COLORREF c, char* out) {
    sprintf(out, "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
}

double LonToScreen(double lon, int w) { return (lon - g_PanX) * g_Zoom + (w / 2.0); }
double LatToScreen(double lat, int h) { return (g_PanY - lat) * g_Zoom + (h / 2.0); }
double ScreenToLon(int x, int w) { return (x - (w / 2.0)) / g_Zoom + g_PanX; }
double ScreenToLat(int y, int h) { return g_PanY - (y - (h / 2.0)) / g_Zoom; }
double ToRad(double deg) { return deg * 3.141592653589793 / 180.0; }

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

uint64_t ReadVarint(uint8_t** ptr, uint8_t* end) {
    uint64_t result = 0; int shift = 0;
    while (*ptr < end) {
        uint8_t b = **ptr; (*ptr)++;
        result |= (uint64_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    return result;
}

int32_t DecodeMortonHalfX(uint64_t m) {
    m = m & 0x5555555555555555ULL;
    m = (m | (m >> 1)) & 0x3333333333333333ULL;
    m = (m | (m >> 2)) & 0x0F0F0F0F0F0F0F0FULL;
    m = (m | (m >> 4)) & 0x00FF00FF00FF00FFULL;
    m = (m | (m >> 8)) & 0x0000FFFF0000FFFFULL;
    m = (m | (m >> 16)) & 0x00000000FFFFFFFFULL;
    return (int32_t)m;
}

int32_t DecodeMortonHalfY(uint64_t m) {
    m = m >> 1;
    m = m & 0x5555555555555555ULL;
    m = (m | (m >> 1)) & 0x3333333333333333ULL;
    m = (m | (m >> 2)) & 0x0F0F0F0F0F0F0F0FULL;
    m = (m | (m >> 4)) & 0x00FF00FF00FF00FFULL;
    m = (m | (m >> 8)) & 0x0000FFFF0000FFFFULL;
    m = (m | (m >> 16)) & 0x00000000FFFFFFFFULL;
    return (int32_t)m;
}

int64_t DecodeZigZag(uint64_t n) { return (n >> 1) ^ -(int64_t)(n & 1); }

MapPoint IntToLatLon(int32_t ix, int32_t iy) {
    MapPoint p;
    p.lon = ((double)ix / 1073741823.0) * 360.0 - 180.0;
    p.lat = ((double)iy / 1073741823.0) * 180.0 - 90.0;
    return p;
}

MapFeature* GetChunkFeatures(int c_idx, uint32_t* out_count) {
    g_FrameCounter++;
    for (int i = 0; i < LRU_SIZE; i++) {
        if (g_LRU[i].chunk_idx == c_idx) {
            g_LRU[i].last_accessed = g_FrameCounter;
            *out_count = g_LRU[i].feature_count;
            return g_LRU[i].features;
        }
    }

    int evict_i = 0;
    uint64_t oldest = 0xFFFFFFFFFFFFFFFFULL;
    for (int i = 0; i < LRU_SIZE; i++) {
        if (g_LRU[i].chunk_idx == -1) { evict_i = i; break; }
        if (g_LRU[i].last_accessed < oldest) { oldest = g_LRU[i].last_accessed; evict_i = i; }
    }

    if (g_LRU[evict_i].chunk_idx != -1) {
        for (uint32_t i = 0; i < g_LRU[evict_i].feature_count; i++) {
            if (g_LRU[evict_i].features[i].name) free(g_LRU[evict_i].features[i].name);
            if (g_LRU[evict_i].features[i].postcode) free(g_LRU[evict_i].features[i].postcode);
            free(g_LRU[evict_i].features[i].points);
        }
        free(g_LRU[evict_i].features);
        g_LRU[evict_i].chunk_idx = -1;
    }

    MapChunk* mc = &g_Chunks[c_idx];
    FSEEK64(mc->file, mc->offset, SEEK_SET);
    uint8_t* comp_buf = malloc(mc->comp_size);
    fread(comp_buf, 1, mc->comp_size, mc->file);

    uint8_t* uncomp_buf = malloc(mc->uncomp_size);
    lzav_decompress(comp_buf, uncomp_buf, mc->comp_size, mc->uncomp_size);
    free(comp_buf);

    uint32_t f_cap = 1024;
    MapFeature* feats = malloc(f_cap * sizeof(MapFeature));
    uint32_t f_cnt = 0;

    uint8_t* ptr = uncomp_buf;
    uint8_t* end = uncomp_buf + mc->uncomp_size;
    
    while (ptr < end) {
        uint64_t feat_len = ReadVarint(&ptr, end);
        uint8_t* f_end = ptr + feat_len;
        
        if (f_cnt >= f_cap) { f_cap *= 2; feats = realloc(feats, f_cap * sizeof(MapFeature)); }
        
        uint8_t fc = ReadVarint(&ptr, f_end);
        ReadVarint(&ptr, f_end); 
        
        uint64_t nlen = ReadVarint(&ptr, f_end);
        char* name = NULL;
        if (nlen > 0) { name = malloc(nlen + 1); memcpy(name, ptr, nlen); name[nlen] = 0; ptr += nlen; }
        
        uint64_t plen = ReadVarint(&ptr, f_end);
        char* pcode = NULL;
        if (plen > 0) { pcode = malloc(plen + 1); memcpy(pcode, ptr, plen); pcode[plen] = 0; ptr += plen; }
        
        uint64_t ref_count = ReadVarint(&ptr, f_end);
        MapPoint* pts = malloc(ref_count * sizeof(MapPoint));
        
        uint64_t morton0 = ReadVarint(&ptr, f_end);
        int32_t ix = DecodeMortonHalfX(morton0);
        int32_t iy = DecodeMortonHalfY(morton0);
        pts[0] = IntToLatLon(ix, iy);
        
        double min_lon = pts[0].lon, max_lon = pts[0].lon, min_lat = pts[0].lat, max_lat = pts[0].lat;
        
        for (uint32_t k = 1; k < ref_count; k++) {
            ix += DecodeZigZag(ReadVarint(&ptr, f_end));
            iy += DecodeZigZag(ReadVarint(&ptr, f_end));
            pts[k] = IntToLatLon(ix, iy);
            if (pts[k].lon < min_lon) min_lon = pts[k].lon; if (pts[k].lon > max_lon) max_lon = pts[k].lon;
            if (pts[k].lat < min_lat) min_lat = pts[k].lat; if (pts[k].lat > max_lat) max_lat = pts[k].lat;
        }
        
        feats[f_cnt].feature_class = fc;
        feats[f_cnt].name = name;
        feats[f_cnt].postcode = pcode;
        feats[f_cnt].points = pts;
        feats[f_cnt].point_count = ref_count;
        feats[f_cnt].min_lon = min_lon; feats[f_cnt].max_lon = max_lon;
        feats[f_cnt].min_lat = min_lat; feats[f_cnt].max_lat = max_lat;
        f_cnt++;
        ptr = f_end;
    }
    free(uncomp_buf);

    g_LRU[evict_i].chunk_idx = c_idx;
    g_LRU[evict_i].features = feats;
    g_LRU[evict_i].feature_count = f_cnt;
    g_LRU[evict_i].last_accessed = g_FrameCounter;

    *out_count = f_cnt;
    return feats;
}

void DrawMap(HDC hdc, int w, int h) {
    HBRUSH bgOceanBrush = CreateSolidBrush(c_Water);
    FillRect(hdc, &(RECT){0, 0, w, h}, bgOceanBrush);
    DeleteObject(bgOceanBrush);

    if (g_ChunkCount == 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 50, 150));
        SetTextAlign(hdc, TA_CENTER | TA_BASELINE);
        TextOutA(hdc, w / 2, h / 2, "Drag a .qjm file here...", 24);
        return;
    }

    HPEN coastPen = CreatePen(PS_SOLID, 2, c_Coast);
    HPEN hwyMinorPen = CreatePen(PS_SOLID, 3, c_HwyMin);
    HPEN hwyMainPen = CreatePen(PS_SOLID, 5, c_HwyMain); 
    HBRUSH landBrush = CreateSolidBrush(c_Land);
    HBRUSH parkBrush = CreateSolidBrush(c_Park);
    HBRUSH waterBrush = CreateSolidBrush(c_Water);
    HPEN nullPen = GetStockObject(NULL_PEN);

    double l1, t1, l2, t2, l3, t3, l4, t4;
    ScreenToLatLon(-50, -50, w, h, &l1, &t1);
    ScreenToLatLon(w+50, -50, w, h, &l2, &t2);
    ScreenToLatLon(-50, h+50, w, h, &l3, &t3);
    ScreenToLatLon(w+50, h+50, w, h, &l4, &t4);
    double vp_min_lon = fmin(fmin(l1, l2), fmin(l3, l4));
    double vp_max_lon = fmax(fmax(l1, l2), fmax(l3, l4));
    double vp_min_lat = fmin(fmin(t1, t2), fmin(t3, t4));
    double vp_max_lat = fmax(fmax(t1, t2), fmax(t3, t4));

    static POINT* screen_pts = NULL;
    static uint32_t screen_pts_cap = 0;
    static RECT drawn_labels[4000]; 
    int label_cnt = 0;

    bool is_panning = (g_IsDragging && g_MouseMoved);

    for (int pass = 1; pass <= 7; pass++) {
        if (is_panning && pass > 1) break; 

        for (uint32_t c = 0; c < g_ChunkCount; c++) {
            MapChunk* mc = &g_Chunks[c];
            if (mc->max_lon < vp_min_lon || mc->min_lon > vp_max_lon || 
                mc->max_lat < vp_min_lat || mc->min_lat > vp_max_lat) continue;

            uint32_t f_cnt;
            MapFeature* feats = GetChunkFeatures(c, &f_cnt);

            for (uint32_t i = 0; i < f_cnt; i++) {
                MapFeature* f = &feats[i];
                if (f->point_count < 2) continue;

                if (f->max_lon < vp_min_lon || f->min_lon > vp_max_lon || 
                    f->max_lat < vp_min_lat || f->min_lat > vp_max_lat) continue;

                double feat_w = f->max_lon - f->min_lon;
                double feat_h = f->max_lat - f->min_lat;
                bool is_hl = (c == (uint32_t)g_HighlightedChunk && i == (uint32_t)g_HighlightedFeat);

                if (f->feature_class != CLASS_LAND && f->feature_class != CLASS_COASTLINE && !is_hl) {
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
                    if (is_hl) {
                    } else {
                        if (!g_ShowLabels) continue;
                        if (f->feature_class != CLASS_HWY_MAIN) continue; 
                        if (g_Zoom < 0.05) continue; 
                        if (!f->name) continue;
                    }
                } else if (pass == 7) {
                    if (is_hl) continue;
                    if (!g_ShowLabels) continue;
                    if (f->feature_class != CLASS_HWY_MINOR) continue; 
                    if (g_Zoom < 2500.0) continue; 
                    if (!f->name) continue;
                }

                if (f->point_count > screen_pts_cap) {
                    screen_pts_cap = f->point_count + 1024;
                    screen_pts = (POINT*)realloc(screen_pts, screen_pts_cap * sizeof(POINT));
                }

                uint32_t pt_idx = 0;
                int last_px = -999999, last_py = -999999;
                uint32_t step = is_panning ? 10 : 1; 
                
                for (uint32_t j = 0; j < f->point_count; j++) {
                    if (is_panning && (j % step != 0) && (j != f->point_count - 1)) continue;

                    double px, py;
                    LatLonToScreen(f->points[j].lon, f->points[j].lat, w, h, &px, &py);
                    int ipx = (int)px;
                    int ipy = (int)py;
                    
                    if (j == 0 || j == f->point_count - 1 || abs(ipx - last_px) >= 2 || abs(ipy - last_py) >= 2) {
                        screen_pts[pt_idx].x = ipx;
                        screen_pts[pt_idx].y = ipy;
                        pt_idx++;
                        last_px = ipx;
                        last_py = ipy;
                    }
                }
                
                if (pt_idx < 2) continue;
                
                if (pass == 6 || pass == 7) {
                    if (pass == 6 && is_hl) {
                        HPEN hlPen = CreatePen(PS_SOLID, 8, RGB(255, 50, 50));
                        SelectObject(hdc, GetStockObject(NULL_BRUSH)); 
                        SelectObject(hdc, hlPen);
                        Polyline(hdc, screen_pts, pt_idx);
                        DeleteObject(hlPen);
                        continue;
                    }

                    double max_len = 0; int best_i = 0;
                    for (uint32_t j = 0; j < pt_idx - 1; j++) {
                        double p_dx = screen_pts[j+1].x - screen_pts[j].x;
                        double p_dy = screen_pts[j+1].y - screen_pts[j].y;
                        double len = p_dx*p_dx + p_dy*p_dy;
                        if (len > max_len) { max_len = len; best_i = j; }
                    }

                    if (max_len < (pass == 6 ? 100 : 1000)) continue; 

                    int p1x = screen_pts[best_i].x, p1y = screen_pts[best_i].y;
                    int p2x = screen_pts[best_i+1].x, p2y = screen_pts[best_i+1].y;

                    double angle_rad = atan2(p1y - p2y, p2x - p1x); 
                    double angle_deg = angle_rad * 180.0 / 3.1415926535;

                    if (angle_deg < 0) angle_deg += 360.0;
                    if (angle_deg > 90.0 && angle_deg < 270.0) angle_deg -= 180.0; 

                    int cx = (p1x + p2x) / 2;
                    int cy = (p1y + p2y) / 2;

                    HFONT hFont = CreateFont(16, 0, (int)(angle_deg * 10), (int)(angle_deg * 10), FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

                    SIZE sz;
                    GetTextExtentPoint32(hdc, f->name, strlen(f->name), &sz);

                    int hw = sz.cx / 2, hh = sz.cy / 2;
                    RECT tr = { cx - hw - 10, cy - hh - 10, cx + hw + 10, cy + hh + 10 };

                    bool collision = false;
                    for (int k = 0; k < label_cnt; k++) {
                        RECT dummy;
                        if (IntersectRect(&dummy, &tr, &drawn_labels[k])) { collision = true; break; }
                    }

                    if (!collision) {
                        if (label_cnt < 4000) drawn_labels[label_cnt++] = tr;
                        SetTextColor(hdc, RGB(40, 40, 40));
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextAlign(hdc, TA_CENTER | TA_BASELINE);
                        TextOutA(hdc, cx, cy + (sz.cy/3), f->name, strlen(f->name));
                    }
                    
                    SelectObject(hdc, hOldFont);
                    DeleteObject(hFont);
                    continue;
                }

                if (pass == 1) { 
                    SelectObject(hdc, landBrush); SelectObject(hdc, nullPen);
                    Polygon(hdc, screen_pts, pt_idx);
                } else if (pass == 2) { 
                    SelectObject(hdc, (f->feature_class == CLASS_WATER) ? waterBrush : parkBrush);
                    SelectObject(hdc, nullPen);
                    Polygon(hdc, screen_pts, pt_idx);
                } else if (pass == 3) {
                    SelectObject(hdc, GetStockObject(NULL_BRUSH)); SelectObject(hdc, coastPen); 
                    Polyline(hdc, screen_pts, pt_idx);
                } else if (pass == 4) {
                    SelectObject(hdc, GetStockObject(NULL_BRUSH)); SelectObject(hdc, hwyMinorPen); 
                    Polyline(hdc, screen_pts, pt_idx);
                } else if (pass == 5) {
                    SelectObject(hdc, GetStockObject(NULL_BRUSH)); SelectObject(hdc, hwyMainPen); 
                    Polyline(hdc, screen_pts, pt_idx);
                }
            }
        }
    }

    if (g_RoutePathCount > 0 && !is_panning) {
        LOGBRUSH lbRoute = { BS_SOLID, c_Route, 0 };
        HPEN routePen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 6, &lbRoute, 0, NULL);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        SelectObject(hdc, routePen);
        
        if ((uint32_t)g_RoutePathCount > screen_pts_cap) {
            screen_pts_cap = g_RoutePathCount + 1024;
            screen_pts = (POINT*)realloc(screen_pts, screen_pts_cap * sizeof(POINT));
        }
        for (int i = 0; i < g_RoutePathCount; i++) {
            double px, py;
            LatLonToScreen(g_RoutePath[i].lon, g_RoutePath[i].lat, w, h, &px, &py);
            screen_pts[i].x = (LONG)px;
            screen_pts[i].y = (LONG)py;
        }
        Polyline(hdc, screen_pts, g_RoutePathCount);
        DeleteObject(routePen);
    }

    if (!is_panning) {
        HBRUSH brPin = CreateSolidBrush(c_Pin);
        HPEN pinPen = CreatePen(PS_SOLID, 1, RGB(100, 0, 0));
        HBRUSH brPinSel = CreateSolidBrush(c_PinSel);
        HPEN pinSelPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 100));

        for (int i = 0; i < g_PinCount; i++) {
            double px, py;
            LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, w, h, &px, &py);
            int x = (int)px, y = (int)py;
            
            bool selected = false;
            for(int k=0; k<g_SelectedPinCount; k++) if(g_SelectedPins[k] == i) selected = true;

            SelectObject(hdc, selected ? brPinSel : brPin);
            SelectObject(hdc, selected ? pinSelPen : pinPen);

            POINT pt[4] = {{x, y}, {x - 10, y - 20}, {x, y - 30}, {x + 10, y - 20}};
            Polygon(hdc, pt, 4);

            if (i != g_EditingPinIndex && strlen(g_Pins[i].name) > 0) {
                SetTextColor(hdc, RGB(20, 20, 20));
                SetBkMode(hdc, TRANSPARENT);
                SetTextAlign(hdc, TA_CENTER | TA_BOTTOM);
                TextOutA(hdc, x, y - 32, g_Pins[i].name, strlen(g_Pins[i].name));
            }
        }
        DeleteObject(brPin); DeleteObject(pinPen); DeleteObject(brPinSel); DeleteObject(pinSelPen);
    }

    DeleteObject(coastPen); DeleteObject(hwyMinorPen); DeleteObject(hwyMainPen);
    DeleteObject(landBrush); DeleteObject(parkBrush); DeleteObject(waterBrush);

    if (g_SelectedPinCount >= 2 && g_RouteStepCount > 0 && !is_panning) {
        int leg_w = 260;
        int max_leg_h = h - 100;
        if (max_leg_h < 100) max_leg_h = 100;
        int req_h = 40 + (g_RouteStepCount * 20); 
        int leg_h = (req_h < max_leg_h) ? req_h : max_leg_h;
        RECT legRect = { 10, 45, 10 + leg_w, 45 + leg_h };
        HBRUSH legBrush = CreateSolidBrush(RGB(250, 248, 245));
        HPEN legPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        SelectObject(hdc, legBrush); SelectObject(hdc, legPen);
        RoundRect(hdc, legRect.left, legRect.top, legRect.right, legRect.bottom, 15, 15);

        if (req_h > max_leg_h) {
            g_LegendMaxScroll = (float)(req_h - max_leg_h + 10);
            if (g_LegendScrollY < -g_LegendMaxScroll) g_LegendScrollY = -g_LegendMaxScroll;
        } else {
            g_LegendMaxScroll = 0; g_LegendScrollY = 0;
        }

        if (g_LegendMaxScroll > 0) {
            HBRUSH sbBg = CreateSolidBrush(RGB(230, 230, 230));
            HBRUSH sbFg = CreateSolidBrush(RGB(180, 180, 180));
            RECT sbTrack = { legRect.right - 12, legRect.top + 10, legRect.right - 4, legRect.bottom - 10 };
            FillRect(hdc, &sbTrack, sbBg);
            
            float visible_ratio = (float)leg_h / (float)req_h;
            int thumb_h = (int)((leg_h - 20) * visible_ratio);
            if (thumb_h < 20) thumb_h = 20;
            
            float scroll_ratio = -g_LegendScrollY / g_LegendMaxScroll;
            int thumb_y = sbTrack.top + (int)(scroll_ratio * (sbTrack.bottom - sbTrack.top - thumb_h));
            
            RECT sbThumb = { sbTrack.left, thumb_y, sbTrack.right, thumb_y + thumb_h };
            FillRect(hdc, &sbThumb, sbFg);
            DeleteObject(sbBg); DeleteObject(sbFg);
        }

        HRGN clipRgn = CreateRectRgn(legRect.left + 5, legRect.top + 5, legRect.right - (g_LegendMaxScroll > 0 ? 15 : 5), legRect.bottom - 5);
        SelectClipRgn(hdc, clipRgn);

        SetTextColor(hdc, RGB(30, 30, 30));
        SetTextAlign(hdc, TA_LEFT | TA_TOP);
        
        HFONT hBoldFont = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
        HFONT hOldF = (HFONT)SelectObject(hdc, hBoldFont);

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "Total Distance: %.1f %s", g_TotalRouteDist, g_UseMiles ? "mi" : "km");
        TextOutA(hdc, 20, 55 + (int)g_LegendScrollY, hdr, strlen(hdr));

        SelectObject(hdc, hOldF);
        DeleteObject(hBoldFont);

        float text_y = 80 + g_LegendScrollY;
        for (int step = 0; step < g_RouteStepCount; step++) {
            if (text_y > 40 && text_y < legRect.bottom) {
                char step_str[300];
                snprintf(step_str, sizeof(step_str), "%d. %s (%.1f %s)", step+1, g_RouteSteps[step].text, g_RouteSteps[step].dist, g_UseMiles ? "mi" : "km");
                TextOutA(hdc, 20, (int)text_y, step_str, strlen(step_str));
            }
            text_y += 20;
        }
        
        SelectClipRgn(hdc, NULL);
        DeleteObject(clipRgn); DeleteObject(legBrush); DeleteObject(legPen);
    }
}

double Haversine(double lat1, double lon1, double lat2, double lon2) {
    double dLat = ToRad(lat2 - lat1);
    double dLon = ToRad(lon2 - lon1);
    double a = sin(dLat/2)*sin(dLat/2) + cos(ToRad(lat1))*cos(ToRad(lat2))*sin(dLon/2)*sin(dLon/2);
    return 2 * R_EARTH * atan2(sqrt(a), sqrt(1-a));
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

void AutoFitMap(HWND hwnd) {
    if (g_ChunkCount == 0) return;
    double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
    
    for (uint32_t i = 0; i < g_ChunkCount; i++) {
        if (g_Chunks[i].min_lon < min_lon) min_lon = g_Chunks[i].min_lon;
        if (g_Chunks[i].max_lon > max_lon) max_lon = g_Chunks[i].max_lon;
        if (g_Chunks[i].min_lat < min_lat) min_lat = g_Chunks[i].min_lat;
        if (g_Chunks[i].max_lat > max_lat) max_lat = g_Chunks[i].max_lat;
    }
    
    g_CenterLon = (min_lon + max_lon) / 2.0;
    g_CenterLat = (min_lat + max_lat) / 2.0;

    if (!g_AutoFit) return;

    RECT rect; GetClientRect(hwnd, &rect);
    double width = (rect.right > 40) ? rect.right - 40 : rect.right;
    double height = (rect.bottom > 40) ? rect.bottom - 40 : rect.bottom;
    
    if (max_lon - min_lon < 0.0001 || max_lat - min_lat < 0.0001) {
        g_Zoom = 25000.0;
    } else {
        double zoomX = width / (max_lon - min_lon);
        double zoomY = height / (max_lat - min_lat);
        g_Zoom = (zoomX < zoomY) ? zoomX : zoomY;
    }
    g_PanX = 0; g_PanY = 0; g_Rotation = 0;
}

void LoadQJMData(const char* filepath) {
    char path[512] = {0};
    if (filepath[0] == '"') strncpy(path, filepath + 1, strlen(filepath) - 2);
    else strncpy(path, filepath, sizeof(path) - 1);

    FILE* file = fopen(path, "rb");
    if (!file) return;

    char magic[4];
    if (fread(magic, 1, 4, file) != 4 || strncmp(magic, "QJMC", 4) != 0) { 
        fclose(file); return; 
    }

    FSEEK64(file, -8, SEEK_END);
    uint64_t dir_offset;
    fread(&dir_offset, 8, 1, file);
    
    FSEEK64(file, dir_offset, SEEK_SET);
    uint32_t chunk_idx;
    fread(&chunk_idx, 4, 1, file);

    ChunkMetaDisk* metas = malloc(chunk_idx * sizeof(ChunkMetaDisk));
    fread(metas, sizeof(ChunkMetaDisk), chunk_idx, file);

    uint32_t start_c = g_ChunkCount;
    g_ChunkCount += chunk_idx;
    g_Chunks = realloc(g_Chunks, g_ChunkCount * sizeof(MapChunk));

    for (uint32_t i = 0; i < chunk_idx; i++) {
        g_Chunks[start_c + i].min_lon = metas[i].min_lon;
        g_Chunks[start_c + i].max_lon = metas[i].max_lon;
        g_Chunks[start_c + i].min_lat = metas[i].min_lat;
        g_Chunks[start_c + i].max_lat = metas[i].max_lat;
        g_Chunks[start_c + i].offset = metas[i].offset;
        g_Chunks[start_c + i].comp_size = metas[i].comp_size;
        g_Chunks[start_c + i].uncomp_size = metas[i].uncomp_size;
        g_Chunks[start_c + i].file = file;
    }
    free(metas);

    for (uint32_t i = 0; i < chunk_idx; i++) {
        uint32_t f_cnt;
        MapFeature* feats = GetChunkFeatures(start_c + i, &f_cnt);
        for (uint32_t j = 0; j < f_cnt; j++) {
            if (feats[j].name || feats[j].postcode) {
                if (g_SearchItemCount >= g_SearchItemCap) {
                    g_SearchItemCap = g_SearchItemCap == 0 ? 10000 : g_SearchItemCap * 2;
                    g_SearchItems = realloc(g_SearchItems, g_SearchItemCap * sizeof(SearchItem));
                }
                char buf[512] = {0};
                if (feats[j].name && feats[j].postcode) snprintf(buf, 512, "%s %s", feats[j].name, feats[j].postcode);
                else if (feats[j].name) snprintf(buf, 512, "%s", feats[j].name);
                else snprintf(buf, 512, "%s", feats[j].postcode);

                g_SearchItems[g_SearchItemCount].text = malloc(strlen(buf) + 1);
                strcpy(g_SearchItems[g_SearchItemCount].text, buf);
                g_SearchItems[g_SearchItemCount].chunk_idx = start_c + i;
                g_SearchItems[g_SearchItemCount].feat_idx = j;
                g_SearchItems[g_SearchItemCount].lon = (feats[j].min_lon + feats[j].max_lon)/2.0;
                g_SearchItems[g_SearchItemCount].lat = (feats[j].min_lat + feats[j].max_lat)/2.0;
                g_SearchItemCount++;
            }
        }
    }
}

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

        double dist_km = Haversine(pA->lat, pA->lon, pB->lat, pB->lon);
        g_TotalRouteDist += g_UseMiles ? (dist_km * 0.621371) : dist_km;

        g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
        g_RoutePath[g_RoutePathCount].lon = pA->lon;
        g_RoutePath[g_RoutePathCount].lat = pA->lat;
        g_RoutePathCount++;

        int steps = (int)(dist_km * 50.0) + 20; 
        double dx = pB->lon - pA->lon;
        double dy = pB->lat - pA->lat;
        
        int last_chunk = -1;
        int last_feat = -1;
        int last_pt = -1;
        char last_road[128] = "";
        double accum_dist = 0;

        for (int s = 0; s <= steps; s++) {
            double slon = pA->lon + dx * (s / (double)steps);
            double slat = pA->lat + dy * (s / (double)steps);

            double min_d = 999999;
            char best_road[128] = "Unnamed Road";
            int best_chunk = -1;
            int best_feat = -1;
            int best_pt = -1;

            for (uint32_t c = 0; c < g_ChunkCount; c++) {
                MapChunk* mc = &g_Chunks[c];
                if (slon < mc->min_lon - 0.02 || slon > mc->max_lon + 0.02) continue;
                if (slat < mc->min_lat - 0.02 || slat > mc->max_lat + 0.02) continue;

                uint32_t f_cnt;
                MapFeature* feats = GetChunkFeatures(c, &f_cnt);
                
                for (uint32_t i = 0; i < f_cnt; i++) {
                    MapFeature* f = &feats[i];
                    if (f->feature_class != CLASS_HWY_MAIN && f->feature_class != CLASS_HWY_MINOR) continue;
                    if (slon < f->min_lon - 0.02 || slon > f->max_lon + 0.02) continue;
                    if (slat < f->min_lat - 0.02 || slat > f->max_lat + 0.02) continue;

                    for (uint32_t j = 0; j < f->point_count; j++) {
                        double ddx = f->points[j].lon - slon;
                        double ddy = f->points[j].lat - slat;
                        double d = ddx*ddx + ddy*ddy;
                        
                        if (g_RouteMode == 0 && f->feature_class == CLASS_HWY_MAIN) d *= 0.005;
                        
                        if (last_chunk != -1 && c == (uint32_t)last_chunk && i == (uint32_t)last_feat) {
                            d *= 0.0001; 
                        } else if (f->name && strlen(last_road) > 0 && strcmp(f->name, last_road) == 0) {
                            d *= 0.001; 
                        }

                        if (d < min_d) {
                            min_d = d; best_chunk = c; best_feat = i; best_pt = j;
                            if (f->name && strlen(f->name) > 0) strcpy(best_road, f->name);
                            else strcpy(best_road, (f->feature_class == CLASS_HWY_MAIN) ? "Main Highway" : "Local Road");
                        }
                    }
                }
            }

            if (best_chunk != -1) {
                uint32_t dummy;
                MapFeature* b_feats = GetChunkFeatures(best_chunk, &dummy);
                MapFeature* b_f = &b_feats[best_feat];

                if (best_chunk == last_chunk && best_feat == last_feat && best_pt != last_pt && last_pt != -1) {
                    int step_dir = (best_pt > last_pt) ? 1 : -1;
                    for (int k = last_pt + step_dir; k != best_pt; k += step_dir) {
                        g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
                        g_RoutePath[g_RoutePathCount++] = b_f->points[k];
                    }
                }
                
                if (best_chunk != last_chunk || best_feat != last_feat || best_pt != last_pt) {
                    g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
                    g_RoutePath[g_RoutePathCount++] = b_f->points[best_pt];
                }
                last_chunk = best_chunk; last_feat = best_feat; last_pt = best_pt;
            }

            double step_len = dist_km / steps;
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

void HandlePipeRoute(HWND hwnd, const char* query) {
    g_PinCount = 0;
    g_SelectedPinCount = 0;

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", query);

    char* token = strtok(buf, "|");
    while (token && g_PinCount < 1000) {
        while (*token == ' ') token++;
        char* end = token + strlen(token) - 1;
        while (end > token && *end == ' ') { *end = '\0'; end--; }

        double lat = 0, lon = 0;
        bool is_coord = false;

        if (strchr(token, ',') || strchr(token, '.') || strchr(token, '-')) {
            char* p = token;
            double vals[2] = {0};
            int val_idx = 0;
            char dirs[2] = {0};
            
            while (*p && val_idx < 2) {
                if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
                    vals[val_idx] = atof(p);
                    while (*p && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) p++;
                    
                    while (*p && !((*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) {
                        char c = toupper(*p);
                        if (c == 'N' || c == 'S' || c == 'E' || c == 'W') {
                            dirs[val_idx] = c;
                        }
                        p++;
                    }
                    val_idx++;
                } else {
                    p++;
                }
            }
            
            if (val_idx == 2) {
                is_coord = true;
                for (int i=0; i<2; i++) {
                    if (dirs[i] == 'N') lat = vals[i];
                    else if (dirs[i] == 'S') lat = -vals[i];
                    else if (dirs[i] == 'E') lon = vals[i];
                    else if (dirs[i] == 'W') lon = -vals[i];
                }
                
                if (dirs[0] == 0 && dirs[1] == 0) {
                    lat = vals[0]; lon = vals[1]; 
                } else if (lat == 0 && lon != 0) { 
                     lat = (dirs[0] == 'W' || dirs[0] == 'E') ? vals[1] : vals[0];
                } else if (lon == 0 && lat != 0) {
                     lon = (dirs[0] == 'N' || dirs[0] == 'S') ? vals[1] : vals[0];
                }
            }
        }

        if (is_coord) {
            sprintf(g_Pins[g_PinCount].name, "Point %d", g_PinCount+1);
            g_Pins[g_PinCount].lat = lat;
            g_Pins[g_PinCount].lon = lon;
            g_SelectedPins[g_SelectedPinCount++] = g_PinCount;
            g_PinCount++;
        } else {
            char clean_tok[256] = {0};
            int j = 0;
            for (int i=0; token[i] && j<255; i++) {
                if (token[i] != ' ') clean_tok[j++] = toupper(token[i]);
            }
            
            for (uint32_t i = 0; i < g_SearchItemCount; i++) {
                char clean_pc[256] = {0};
                int k = 0;
                for (int m=0; g_SearchItems[i].text[m] && k<255; m++) {
                    if (g_SearchItems[i].text[m] != ' ') clean_pc[k++] = toupper(g_SearchItems[i].text[m]);
                }
                if (strcmp(clean_tok, clean_pc) == 0 || strstr(clean_pc, clean_tok)) {
                    double clat = g_SearchItems[i].lat;
                    double clon = g_SearchItems[i].lon;
                    strncpy(g_Pins[g_PinCount].name, token, 127);
                    g_Pins[g_PinCount].lat = clat;
                    g_Pins[g_PinCount].lon = clon;
                    g_SelectedPins[g_SelectedPinCount++] = g_PinCount;
                    g_PinCount++;
                    
                    if (g_SelectedPinCount == 1) {
                        g_PanX = 0; g_PanY = 0; 
                        g_CenterLon = clon; g_CenterLat = clat; 
                        g_Zoom = 25000;
                    }
                    break;
                }
            }
        }
        token = strtok(NULL, "|");
    }

    if (g_SelectedPinCount >= 2) {
        CalculateRoute(); 
        
        char dist_str[128];
        char sys_measure[2] = "0";
        GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_IMEASURE, sys_measure, 2);
        g_UseMiles = (sys_measure[0] == '1'); 

        if (g_UseMiles) {
            double mi = g_TotalRouteDist * 0.621371;
            sprintf(dist_str, "%.1f mi", mi);
        } else {
            if (g_TotalRouteDist < 1.0) sprintf(dist_str, "%.0f m", g_TotalRouteDist * 1000.0);
            else sprintf(dist_str, "%.1f km", g_TotalRouteDist);
        }

        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(dist_str) + 1);
            if (hMem) {
                memcpy(GlobalLock(hMem), dist_str, strlen(dist_str) + 1);
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
            CloseClipboard();
        }
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

void PopulateSearchList(HWND hwnd) {
    char query[128];
    GetWindowTextA(hTxtSearch, query, 128);
    SendMessage(hListSearch, LB_RESETCONTENT, 0, 0);
    if (strlen(query) < 2) { ShowWindow(hListSearch, SW_HIDE); return; }

    int count = 0;
    for (uint32_t i = 0; i < g_SearchItemCount && count < 50; i++) {
        SearchItem* item = &g_SearchItems[i];
        if (FindSubStringIC(item->text, query)) {
            int idx = SendMessageA(hListSearch, LB_ADDSTRING, 0, (LPARAM)item->text);
            SendMessageA(hListSearch, LB_SETITEMDATA, idx, (LPARAM)i);
            count++;
        }
    }
    if (count > 0) {
        RECT r; GetWindowRect(hTxtSearch, &r);
        POINT pt = {r.left, r.bottom}; ScreenToClient(hwnd, &pt);
        SetWindowPos(hListSearch, HWND_TOP, pt.x, pt.y, 250, 150, SWP_SHOWWINDOW);
    } else {
        ShowWindow(hListSearch, SW_HIDE);
    }
}

/* ==========================================================================
 * 4. PDF GENERATION ENGINE
 * ========================================================================== */
void s_app(Stream* s, const char* fmt, ...) {
    char buf[1024]; va_list args; va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    if (s->len + n >= s->cap) {
        s->cap = (s->cap == 0 ? 4096 : s->cap * 2) + n + 1024;
        s->data = (char*)realloc(s->data, s->cap);
    }
    memcpy(s->data + s->len, buf, n);
    s->len += n; s->data[s->len] = 0;
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
    fprintf(f, "%d 0 obj\n<< /Title (Map) /Creator (QJMnavig) >>\nendobj\n", info_obj);

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

    for (int pass = 1; pass <= 6; pass++) {
        for (uint32_t c = 0; c < g_ChunkCount; c++) {
            MapChunk* mc = &g_Chunks[c];
            double cx1, cy1, cx2, cy2, cx3, cy3, cx4, cy4;
            LatLonToPdf(mc->min_lon, mc->min_lat, map_cx, map_cy, pdf_zoom, &cx1, &cy1);
            LatLonToPdf(mc->min_lon, mc->max_lat, map_cx, map_cy, pdf_zoom, &cx2, &cy2);
            LatLonToPdf(mc->max_lon, mc->min_lat, map_cx, map_cy, pdf_zoom, &cx3, &cy3);
            LatLonToPdf(mc->max_lon, mc->max_lat, map_cx, map_cy, pdf_zoom, &cx4, &cy4);
            
            double min_x = fmin(fmin(cx1, cx2), fmin(cx3, cx4));
            double max_x = fmax(fmax(cx1, cx2), fmax(cx3, cx4));
            double min_y = fmin(fmin(cy1, cy2), fmin(cy3, cy4));
            double max_y = fmax(fmax(cy1, cy2), fmax(cy3, cy4));

            if (max_x < m_x || min_x > m_x + map_w || max_y < m_y || min_y > m_y + map_h) continue;

            uint32_t f_cnt;
            MapFeature* feats = GetChunkFeatures(c, &f_cnt);

            for (uint32_t i = 0; i < f_cnt; i++) {
                MapFeature* feat = &feats[i];
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
                    if (feat->feature_class != CLASS_HWY_MINOR) continue;
                    if (g_Zoom < 500.0) continue;
                    if (!feat->name) continue;
                }
                else if (pass == 6) {
                    if (!g_PdfExpLabels) continue; 
                    if (feat->feature_class != CLASS_HWY_MAIN) continue;
                    if (g_Zoom < 0.05) continue;
                    if (!feat->name) continue;
                }

                double f_cx1, f_cy1, f_cx2, f_cy2, f_cx3, f_cy3, f_cx4, f_cy4;
                LatLonToPdf(feat->min_lon, feat->min_lat, map_cx, map_cy, pdf_zoom, &f_cx1, &f_cy1);
                LatLonToPdf(feat->min_lon, feat->max_lat, map_cx, map_cy, pdf_zoom, &f_cx2, &f_cy2);
                LatLonToPdf(feat->max_lon, feat->min_lat, map_cx, map_cy, pdf_zoom, &f_cx3, &f_cy3);
                LatLonToPdf(feat->max_lon, feat->max_lat, map_cx, map_cy, pdf_zoom, &f_cx4, &f_cy4);
                
                double f_min_x = fmin(fmin(f_cx1, f_cx2), fmin(f_cx3, f_cx4));
                double f_max_x = fmax(fmax(f_cx1, f_cx2), fmax(f_cx3, f_cx4));
                double f_min_y = fmin(fmin(f_cy1, f_cy2), fmin(f_cy3, f_cy4));
                double f_max_y = fmax(fmax(f_cy1, f_cy2), fmax(f_cy3, f_cy4));

                if (f_max_x < m_x || f_min_x > m_x + map_w || f_max_y < m_y || f_min_y > m_y + map_h) continue;

                double last_px = -99999, last_py = -99999;
                int pts_written = 0;
                double p1x = 0, p1y = 0, p2x = 0, p2y = 0;

                for (uint32_t j = 0; j < feat->point_count; j++) {
                    double px, py;
                    LatLonToPdf(feat->points[j].lon, feat->points[j].lat, map_cx, map_cy, pdf_zoom, &px, &py);
                    
                    if (j == 0 || j == feat->point_count - 1 || ((px - last_px)*(px - last_px) + (py - last_py)*(py - last_py) > 1.0)) {
                        if (pass == 5 || pass == 6) {
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

                if ((pass == 5 || pass == 6) && pts_written >= 2) {
                    double p_dx = p2x - p1x;
                    double p_dy = p2y - p1y;
                    if (p_dx*p_dx + p_dy*p_dy < (pass == 6 ? 400 : 4000)) continue; 

                    double angle_rad = atan2(p_dy, p_dx);
                    if (angle_rad < -1.5707) angle_rad += 3.14159;
                    if (angle_rad > 1.5707) angle_rad -= 3.14159;

                    double a = cos(angle_rad), b = sin(angle_rad), c = -sin(angle_rad), d = cos(angle_rad);
                    double txt_cx = (p1x + p2x) / 2.0;
                    double txt_cy = (p1y + p2y) / 2.0 + 3.0;

                    char clean_name[128];
                    snprintf(clean_name, sizeof(clean_name), "%s", feat->name);
                    sanitize_pdf_string(clean_name);

                    s_app(&s, "BT %.4f %.4f %.4f %.4f %.2f %.2f Tm 0.2 0.2 0.2 rg /F2 8 Tf (%s) Tj ET\n", 
                        a, b, c, d, txt_cx, txt_cy, clean_name);
                } 
                else if (pass == 1) s_app(&s, "f\n");
                else if (pass != 5 && pass != 6) s_app(&s, "S\n");
            }
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
                OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = "";
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "PDF Files (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
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
                snprintf(g_Pins[g_EditingPinIndex].name, sizeof(g_Pins[g_EditingPinIndex].name), "%s", buf);
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            DragAcceptFiles(hwnd, TRUE);
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            
            RegisterHotKey(hwnd, 1, MOD_CONTROL, 'P');

            hTxtSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", 
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 180, 25, hwnd, (HMENU)1004, NULL, NULL);
            
            hListSearch = CreateWindowExA(WS_EX_TOPMOST, "LISTBOX", "", 
                WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, hwnd, (HMENU)1003, NULL, NULL);
            
            hBtnCancel = CreateWindowA("BUTTON", "Cancel Route", 
                WS_CHILD | WS_VISIBLE, 200, 10, 90, 25, hwnd, (HMENU)4, NULL, NULL);
            hCboRouteType = CreateWindowA("COMBOBOX", "", 
                CBS_DROPDOWNLIST | WS_VISIBLE | WS_CHILD, 295, 12, 110, 100, hwnd, (HMENU)5, NULL, NULL);
            
            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Fastest Route");
            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Shortest Route");
            SendMessage(hCboRouteType, CB_SETCURSEL, g_RouteMode, 0);

            ShowWindow(hBtnCancel, SW_HIDE);
            ShowWindow(hCboRouteType, SW_HIDE);
            
            SendMessage(hTxtSearch, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCboRouteType, WM_SETFONT, (WPARAM)hFont, TRUE);
            return 0;
        }
        case WM_HOTKEY: {
            if (wParam == 1) { 
                if (hPdfDlg) {
                    ShowWindow(hPdfDlg, SW_SHOW);
                    SetForegroundWindow(hPdfDlg);
                }
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                PopulateSearchList(hwnd);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1004 && HIWORD(wParam) == EN_CHANGE) {
                SetTimer(hwnd, 1, 500, NULL);
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
            } else if (LOWORD(wParam) == 1003 && HIWORD(wParam) == LBN_SELCHANGE) {
                int sel = SendMessage(hListSearch, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    uint64_t idx = SendMessage(hListSearch, LB_GETITEMDATA, sel, 0);
                    SearchItem* item = &g_SearchItems[idx];
                    g_HighlightedChunk = item->chunk_idx;
                    g_HighlightedFeat = item->feat_idx;
                    g_CenterLon = item->lon;
                    g_CenterLat = item->lat;
                    g_Zoom = 25000.0;
                    g_PanX = 0;
                    g_PanY = 0;
                    ShowWindow(hListSearch, SW_HIDE);
                    SetFocus(hwnd);
                    
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            char path[MAX_PATH];
            DragQueryFileA(hDrop, 0, path, MAX_PATH);
            DragFinish(hDrop);
            
            if (strlen(g_MapFilesStr) + strlen(path) + 2 < sizeof(g_MapFilesStr)) {
                if (strlen(g_MapFilesStr) > 0) strcat(g_MapFilesStr, "|");
                strcat(g_MapFilesStr, path);
            }

            char* ext = strrchr(path, '.');
            if (ext && (strcasecmp(ext, ".qjm") == 0 || strcasecmp(ext, ".qjmc") == 0)) {
                LoadQJMData(path);
            }
            
            AutoFitMap(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_SIZE: {
            AutoFitMap(hwnd); 
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == 'P' && (GetKeyState(VK_CONTROL) & 0x8000)) { 
                if (hPdfDlg) {
                    ShowWindow(hPdfDlg, SW_SHOW);
                    SetForegroundWindow(hPdfDlg);
                }
                return 0;
            }
            if (wParam == VK_SPACE) { 
                g_AutoFit = true; 
                AutoFitMap(hwnd); 
                InvalidateRect(hwnd, NULL, FALSE); 
            } else if (wParam == VK_ESCAPE) {
                g_SelectedPinCount = 0;
                g_RouteStepCount = 0;
                g_RoutePathCount = 0;
                ShowWindow(hBtnCancel, SW_HIDE);
                ShowWindow(hCboRouteType, SW_HIDE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_RBUTTONDOWN: {
            if (hEditPin) { SetFocus(hwnd); return 0; } 
            
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            RECT rc; GetClientRect(hwnd, &rc);
            
            bool pin_deleted = false;
            for (int i = 0; i < g_PinCount; i++) {
                double px, py;
                LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rc.right, rc.bottom, &px, &py);
                if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) {
                    int sel_idx = -1;
                    for (int k = 0; k < g_SelectedPinCount; k++) {
                        if (g_SelectedPins[k] == i) sel_idx = k;
                    }
                    if (sel_idx >= 0) {
                        for (int k = sel_idx; k < g_SelectedPinCount - 1; k++) g_SelectedPins[k] = g_SelectedPins[k+1];
                        g_SelectedPinCount--;
                    }
                    for (int k = 0; k < g_SelectedPinCount; k++) {
                        if (g_SelectedPins[k] > i) g_SelectedPins[k]--;
                    }
                    for (int k = i; k < g_PinCount - 1; k++) g_Pins[k] = g_Pins[k+1];
                    g_PinCount--;
                    
                    if (g_SelectedPinCount >= 2) CalculateRoute();
                    else {
                        g_RouteStepCount = 0; g_RoutePathCount = 0;
                        ShowWindow(hBtnCancel, SW_HIDE); ShowWindow(hCboRouteType, SW_HIDE);
                    }
                    pin_deleted = true;
                    break;
                }
            }

            if (!pin_deleted) {
                double c_lon, c_lat;
                ScreenToLatLon(mx, my, rc.right, rc.bottom, &c_lon, &c_lat);
                
                double best_d = 999999;
                int best_chunk = -1;
                int best_feat = -1;
                
                for (uint32_t c = 0; c < g_ChunkCount; c++) {
                    MapChunk* mc = &g_Chunks[c];
                    if (c_lon < mc->min_lon - 0.05 || c_lon > mc->max_lon + 0.05) continue;
                    if (c_lat < mc->min_lat - 0.05 || c_lat > mc->max_lat + 0.05) continue;
                    
                    uint32_t f_cnt;
                    MapFeature* feats = GetChunkFeatures(c, &f_cnt);
                    for (uint32_t i = 0; i < f_cnt; i++) {
                        MapFeature* f = &feats[i];
                        if (c_lon < f->min_lon - 0.05 || c_lon > f->max_lon + 0.05) continue;
                        if (c_lat < f->min_lat - 0.05 || c_lat > f->max_lat + 0.05) continue;

                        for (uint32_t j = 0; j < f->point_count; j++) {
                            double ddx = f->points[j].lon - c_lon;
                            double ddy = f->points[j].lat - c_lat;
                            double d = ddx*ddx + ddy*ddy;
                            if (d < best_d) { best_d = d; best_chunk = c; best_feat = i; }
                        }
                    }
                }
                
                if (best_chunk != -1) {
                    uint32_t dummy;
                    MapFeature* feats = GetChunkFeatures(best_chunk, &dummy);
                    MapFeature* best_f = &feats[best_feat];

                    double feat_w = best_f->max_lon - best_f->min_lon;
                    double feat_h = best_f->max_lat - best_f->min_lat;
                    if (feat_w < 0.0001) feat_w = 0.0001;
                    if (feat_h < 0.0001) feat_h = 0.0001;
                    
                    g_CenterLon = (best_f->min_lon + best_f->max_lon) / 2.0;
                    g_CenterLat = (best_f->min_lat + best_f->max_lat) / 2.0;
                    g_PanX = 0; g_PanY = 0;
                    
                    double zoomX = rc.right / (0.10 * feat_w);
                    double zoomY = rc.bottom / (0.10 * feat_h);
                    g_Zoom = (zoomX < zoomY) ? zoomX : zoomY;
                }

                g_SelectedPinCount = 0;
                g_RouteStepCount = 0;
                g_RoutePathCount = 0;
                ShowWindow(hBtnCancel, SW_HIDE);
                ShowWindow(hCboRouteType, SW_HIDE);
            }
            
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int mx = (short)LOWORD(lParam); 
            if (g_SelectedPinCount >= 2 && mx < 300) {
                float scroll_delta = ((int16_t)HIWORD(wParam) > 0) ? 30.0f : -30.0f;
                g_LegendScrollY += scroll_delta;
                if (g_LegendScrollY > 0) g_LegendScrollY = 0;
                if (g_LegendMaxScroll > 0 && g_LegendScrollY < -g_LegendMaxScroll) g_LegendScrollY = -g_LegendMaxScroll;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int my = (short)HIWORD(lParam);
            ScreenToClient(hwnd, &(POINT){mx, my});
            
            RECT rect; GetClientRect(hwnd, &rect);
            double zoom_factor = (delta > 0) ? 1.2 : 0.8333333;
            double rel_x = mx - (rect.right / 2.0);
            double rel_y = my - (rect.bottom / 2.0);
            g_PanX = rel_x - (rel_x - g_PanX) * zoom_factor;
            g_PanY = rel_y - (rel_y - g_PanY) * zoom_factor;
            
            g_Zoom *= zoom_factor;
            if (g_Zoom < 0.001) g_Zoom = 0.001; 

            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd); 
            if (hEditPin) SetFocus(hwnd); 
            g_IsDragging = true;
            g_MouseMoved = false;
            g_LastMousePos.x = (short)LOWORD(lParam); 
            g_LastMousePos.y = (short)HIWORD(lParam);
            g_HighlightedChunk = -1;
            g_HighlightedFeat = -1;
            ShowWindow(hListSearch, SW_HIDE);
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (g_IsDragging) {
                int mx = (short)LOWORD(lParam);
                int my = (short)HIWORD(lParam);
                if (abs(mx - g_LastMousePos.x) > 3 || abs(my - g_LastMousePos.y) > 3) {
                    g_MouseMoved = true;
                }
                if (g_MouseMoved) {
                    int dx = mx - g_LastMousePos.x;
                    int dy = my - g_LastMousePos.y;
                    
                    if (wParam & MK_SHIFT) {
                        g_Rotation += dx * 0.01;
                    } else {
                        g_PanX += dx;
                        g_PanY += dy;
                    }
                    
                    g_LastMousePos.x = mx;
                    g_LastMousePos.y = my;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_IsDragging) {
                g_IsDragging = false;
                ReleaseCapture();
                
                if (!g_MouseMoved && hEditPin == NULL) {
                    int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
                    RECT rect; GetClientRect(hwnd, &rect);
                    for (int i = 0; i < g_PinCount; i++) {
                        double px, py;
                        LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rect.right, rect.bottom, &px, &py);
                        if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) { 
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
                }

                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_LBUTTONDBLCLK: {
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            RECT rc; GetClientRect(hwnd, &rc);
            
            bool hit_pin = false;
            for (int i = 0; i < g_PinCount; i++) {
                double px, py;
                LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rc.right, rc.bottom, &px, &py);
                if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) {
                    g_EditingPinIndex = i;
                    hEditPin = CreateWindowExA(0, "EDIT", g_Pins[i].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 
                        (int)px - 50, (int)py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                    OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
                    SetFocus(hEditPin);
                    SendMessage(hEditPin, EM_SETSEL, 0, -1);
                    InvalidateRect(hwnd, NULL, FALSE);
                    hit_pin = true;
                    break;
                }
            }

            if (!hit_pin && g_PinCount < 1000) {
                if (my < 45) return 0; 
                if (g_SelectedPinCount >= 2 && mx < 280) return 0;

                g_EditingPinIndex = g_PinCount;
                double t_lon, t_lat;
                ScreenToLatLon(mx, my, rc.right, rc.bottom, &t_lon, &t_lat);

                double snap_dist = 999999;
                for (uint32_t c = 0; c < g_ChunkCount; c++) {
                    MapChunk* mc = &g_Chunks[c];
                    if (t_lon < mc->min_lon - 0.02 || t_lon > mc->max_lon + 0.02) continue;
                    if (t_lat < mc->min_lat - 0.02 || t_lat > mc->max_lat + 0.02) continue;

                    uint32_t f_cnt;
                    MapFeature* feats = GetChunkFeatures(c, &f_cnt);

                    for (uint32_t i = 0; i < f_cnt; i++) {
                        MapFeature* f = &feats[i];
                        if (f->feature_class != CLASS_HWY_MAIN && f->feature_class != CLASS_HWY_MINOR) continue;
                        if (f->feature_class == CLASS_HWY_MINOR && g_Zoom < 2500.0) continue; 
                        
                        if (t_lon < f->min_lon - 0.02 || t_lon > f->max_lon + 0.02) continue;
                        if (t_lat < f->min_lat - 0.02 || t_lat > f->max_lat + 0.02) continue;

                        for (uint32_t j = 0; j < f->point_count; j++) {
                            double ddx = f->points[j].lon - t_lon;
                            double ddy = f->points[j].lat - t_lat;
                            double d = ddx*ddx + ddy*ddy;
                            if (d < snap_dist) {
                                snap_dist = d;
                                g_Pins[g_PinCount].lon = f->points[j].lon;
                                g_Pins[g_PinCount].lat = f->points[j].lat;
                            }
                        }
                    }
                }

                if (snap_dist == 999999) {
                    g_Pins[g_PinCount].lon = t_lon;
                    g_Pins[g_PinCount].lat = t_lat;
                }

                snprintf(g_Pins[g_PinCount].name, 128, "%d", g_PinCount + 1); 
                g_SelectedPins[g_SelectedPinCount++] = g_PinCount;
                g_PinCount++;
                if (g_SelectedPinCount >= 2) CalculateRoute();

                double e_px, e_py;
                LatLonToScreen(g_Pins[g_PinCount-1].lon, g_Pins[g_PinCount-1].lat, rc.right, rc.bottom, &e_px, &e_py);

                hEditPin = CreateWindowExA(0, "EDIT", g_Pins[g_PinCount-1].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 
                    (int)e_px - 50, (int)e_py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
                SetFocus(hEditPin);
                SendMessage(hEditPin, EM_SETSEL, 0, -1);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rc; GetClientRect(hwnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            SelectObject(memDC, memBM);
            
            SetGraphicsMode(memDC, GM_ADVANCED); 
            SetPolyFillMode(memDC, WINDING);

            bool is_panning = (g_IsDragging && g_MouseMoved);
            int o_Water = g_ShowWater, o_Park = g_ShowPark, o_Coast = g_ShowCoast;
            int o_HwyMin = g_ShowHwyMin, o_HwyMain = g_ShowHwyMain, o_Labels = g_ShowLabels;

            if (is_panning) {
                g_ShowWater = 0; g_ShowPark = 0; g_ShowCoast = 0;
                g_ShowHwyMin = 0; g_ShowHwyMain = 0; g_ShowLabels = 0;
            }

            DrawMap(memDC, rc.right, rc.bottom);
            
            if (is_panning) {
                g_ShowWater = o_Water; g_ShowPark = o_Park; g_ShowCoast = o_Coast;
                g_ShowHwyMin = o_HwyMin; g_ShowHwyMain = o_HwyMain; g_ShowLabels = o_Labels;
            }
            
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            DeleteObject(memBM); DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            UnregisterHotKey(hwnd, 1);
            if (g_SavePins) {
                FILE* ini = fopen("qjmnavig.ini", "w");
                if (ini) {
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
                    fprintf(ini, "searchRoads=%d\n", g_SearchRoads);
                    fprintf(ini, "searchPostcodes=%d\n", g_SearchPostcodes);

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
                    
                    if (g_PinCount > 0) {
                        fprintf(ini, "pins=");
                        for (int i=0; i<g_PinCount; i++) {
                            fprintf(ini, "%s:long:%.7f:lat:%.7f|", g_Pins[i].name, g_Pins[i].lon, g_Pins[i].lat);
                        }
                        fprintf(ini, "\n");
                    } else {
                        fprintf(ini, "pins=\n");
                    }

                    char routeStr[8192] = ""; 
                    for (int i = 0; i < g_SelectedPinCount; i++) {
                        if (i > 0) strcat(routeStr, "|");
                        if (strlen(routeStr) + strlen(g_Pins[g_SelectedPins[i]].name) < sizeof(routeStr)) {
                            strcat(routeStr, g_Pins[g_SelectedPins[i]].name);
                        }
                    }
                    fprintf(ini, "route=%s\n", routeStr);
                    fprintf(ini, "maps=%s\n", g_MapFilesStr);

                    fclose(ini);
                }
            }
            
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    for (int i = 0; i < LRU_SIZE; i++) g_LRU[i].chunk_idx = -1;
    
    char initMaps[4096] = {0};

    FILE* ini = fopen("qjmnavig.ini", "r");
    if (ini) {
        char line[8192];
        while (fgets(line, sizeof(line), ini)) {
            if (strncmp(line, "maps=", 5) == 0) {
                line[strcspn(line, "\r\n")] = 0; 
                strcpy(initMaps, line + 5);
                strcpy(g_MapFilesStr, line + 5);
            }
            else if (strncmp(line, "panX=", 5) == 0) g_PanX = atof(line + 5);
            else if (strncmp(line, "panY=", 5) == 0) g_PanY = atof(line + 5);
            else if (strncmp(line, "zoom=", 5) == 0) {
                g_Zoom = atof(line + 5);
                g_AutoFit = false;
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
            else if (strncmp(line, "searchRoads=", 12) == 0) g_SearchRoads = atoi(line + 12);
            else if (strncmp(line, "searchPostcodes=", 16) == 0) g_SearchPostcodes = atoi(line + 16);
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
        }
        fclose(ini);
    }
    
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        bool has_cmd = false;
        for(size_t i=0; i<strlen(lpCmdLine); i++) {
            if(lpCmdLine[i] > ' ') { has_cmd = true; break; }
        }
        if (has_cmd) {
            char cleanCmd[4096] = {0};
            strncpy(cleanCmd, lpCmdLine, 4095);
            if (cleanCmd[0] == '"') {
                size_t len = strlen(cleanCmd);
                if (cleanCmd[len-1] == '"') cleanCmd[len-1] = '\0';
                memmove(cleanCmd, cleanCmd + 1, strlen(cleanCmd));
            }
            strcpy(initMaps, cleanCmd);
            strcpy(g_MapFilesStr, cleanCmd);
        }
    }

    if (strlen(initMaps) > 0) {
        char* token = strtok(initMaps, "|");
        while (token) {
            char* ext = strrchr(token, '.');
            if (ext && (strcasecmp(ext, ".qjm") == 0 || strcasecmp(ext, ".qjmc") == 0)) {
                LoadQJMData(token);
            }
            token = strtok(NULL, "|");
        }
    }

    WNDCLASSA wcd = {0};
    wcd.lpfnWndProc = PdfWndProc; wcd.hInstance = hInstance;
    wcd.lpszClassName = "PdfDialogClass"; wcd.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wcd.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wcd);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc; 
    wc.hInstance = hInstance;
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = "QjmnavigClass"; 
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, "QjmnavigClass", "QJM Map Navigator",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, g_WinX, g_WinY, g_WinW, g_WinH, NULL, NULL, hInstance, NULL);

    hPdfDlg = CreateWindowExA(0, "PdfDialogClass", "Export PDF Options", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, g_WinX + 50, g_WinY + 50, 280, 260, hwnd, NULL, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}