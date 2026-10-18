#ifndef QJM_H
#define QJM_H

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#ifdef MICROSOFT_WINDOWS_WINBASE_H_DEFINE_INTERLOCKED_CPLUSPLUS_OVERLOADS
#undef MICROSOFT_WINDOWS_WINBASE_H_DEFINE_INTERLOCKED_CPLUSPLUS_OVERLOADS
#endif

#include "miniz.c" 
#include "lzav.h"

#ifdef _WIN32
    #define FSEEK64 _fseeki64
    #define FTELL64 _ftelli64
#else
    #define FSEEK64 fseeko
    #define FTELL64 ftello
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CLASS_UNKNOWN   0
#define CLASS_WATER     1
#define CLASS_LAND      2
#define CLASS_PARK      3
#define CLASS_COASTLINE 4
#define CLASS_HWY_MINOR 5
#define CLASS_HWY_MAIN  6
#define CLASS_PLACE     7
#define CLASS_POI       8
#define R_EARTH 6371.0 
#define LRU_SIZE 128

typedef struct { double lon, lat; } MapPoint;

#pragma pack(push, 1)
typedef struct { uint64_t id; int32_t lat; int32_t lon; } PbfNode; 
typedef struct { uint64_t id; double lat; double lon; } WayNode; 

typedef struct {
    uint32_t name_off, pcode_off, pt_off, pt_cnt;
    float min_lon, max_lon, min_lat, max_lat;
    uint8_t fc, is_area;
    uint16_t pad;
} PFeat;

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
    bool is_overview;
    bool is_dirty;
    uint8_t* dirty_blob;
    int file_id;
} MapChunk;

typedef struct { char name[128]; double lon, lat; } MapPin;
typedef struct { char text[256]; double dist; } RouteStep;
typedef struct { char* data; int len; int cap; } Stream;

typedef struct {
    char* name;
    char* pcode;
    uint32_t chunk_idx;
    uint32_t feat_idx;
    double lon, lat;
} SearchItem;

typedef struct { const char* str; size_t len; } PbfString;
typedef struct { char* name; char* pcode; uint32_t count; WayNode* nodes; bool merged; uint8_t fc; } AccumWay;
typedef struct { uint64_t morton; uint64_t offset; } SortEntry;
typedef struct { int chunk_idx; uint8_t* blob; uint64_t last_accessed; } LRUCacheEntry;

extern double g_CenterLon, g_CenterLat, g_PanX, g_PanY, g_Zoom, g_Rotation;

static inline bool FindSubStringIC(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    for (; *haystack; haystack++) {
        if (toupper((unsigned char)*haystack) == toupper((unsigned char)*needle)) {
            const char *h = haystack + 1, *n = needle + 1;
            while (*h && *n && toupper((unsigned char)*h) == toupper((unsigned char)*n)) {
                h++; n++;
            }
            if (!*n) return true;
        }
    }
    return false;
}

static inline void LatLonToScreen(double lon, double lat, int w, int h, double pan_x, double pan_y, double zoom, double* px, double* py) {
    double dx = (lon - g_CenterLon) * zoom;
    double dy = -(lat - g_CenterLat) * zoom;
    if (fabs(g_Rotation) > 1e-7) {
        double c = cos(g_Rotation), s = sin(g_Rotation);
        double rx = dx * c - dy * s;
        double ry = dx * s + dy * c;
        dx = rx; dy = ry;
    }
    *px = (w / 2.0) + dx + pan_x;
    *py = (h / 2.0) + dy + pan_y;
}

static inline void ScreenToLatLon(int x, int y, int w, int h, double* lon, double* lat) {
    double dx = x - (w / 2.0) - g_PanX;
    double dy = y - (h / 2.0) - g_PanY;
    if (fabs(g_Rotation) > 1e-7) {
        double c = cos(-g_Rotation), s = sin(-g_Rotation);
        double rx = dx * c - dy * s;
        double ry = dx * s + dy * c;
        dx = rx; dy = ry;
    }
    *lon = g_CenterLon + dx / g_Zoom;
    *lat = g_CenterLat - dy / g_Zoom;
}

static inline float Haversine(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * (M_PI / 180.0);
    double dlon = (lon2 - lon1) * (M_PI / 180.0);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1 * (M_PI / 180.0)) * cos(lat2 * (M_PI / 180.0)) *
               sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return (float)(R_EARTH * c);
}

static inline void ColorToHex(COLORREF color, char* hexStr) {
    sprintf(hexStr, "#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
}

static inline COLORREF ParseHexColor(const char* hexStr, COLORREF defaultColor) {
    if (!hexStr) return defaultColor;
    while (*hexStr == ' ' || *hexStr == '#') hexStr++;
    if (strlen(hexStr) < 6) return defaultColor;
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hexStr, "%02x%02x%02x", &r, &g, &b) == 3) {
        return RGB(r, g, b);
    }
    return defaultColor;
}

static inline bool SegmentsIntersect(double p0_x, double p0_y, double p1_x, double p1_y, double p2_x, double p2_y, double p3_x, double p3_y) {
    double s1_x = p1_x - p0_x, s1_y = p1_y - p0_y;
    double s2_x = p3_x - p2_x, s2_y = p3_y - p2_y;
    double den = (-s2_x * s1_y + s1_x * s2_y);
    if (fabs(den) < 1e-9) return false; 
    double s = (-s1_y * (p0_x - p2_x) + s1_x * (p0_y - p2_y)) / den;
    double t = ( s2_x * (p0_y - p2_y) - s2_y * (p0_x - p2_x)) / den;
    return (s > 1e-5 && s < (1.0 - 1e-5) && t > 1e-5 && t < (1.0 - 1e-5));
}

static inline bool PolylinesIntersect(WayNode* a, uint32_t a_cnt, WayNode* b, uint32_t b_cnt) {
    double a_min_x = a[0].lon, a_max_x = a[0].lon, a_min_y = a[0].lat, a_max_y = a[0].lat;
    for(uint32_t i=1; i<a_cnt; i++){
        if(a[i].lon < a_min_x) a_min_x = a[i].lon; if(a[i].lon > a_max_x) a_max_x = a[i].lon;
        if(a[i].lat < a_min_y) a_min_y = a[i].lat; if(a[i].lat > a_max_y) a_max_y = a[i].lat;
    }
    double b_min_x = b[0].lon, b_max_x = b[0].lon, b_min_y = b[0].lat, b_max_y = b[0].lat;
    for(uint32_t i=1; i<b_cnt; i++){
        if(b[i].lon < b_min_x) b_min_x = b[i].lon; if(b[i].lon > b_max_x) b_max_x = b[i].lon;
        if(b[i].lat < b_min_y) b_min_y = b[i].lat; if(b[i].lat > b_max_y) b_max_y = b[i].lat;
    }
    if(a_max_x < b_min_x || a_min_x > b_max_x || a_max_y < b_min_y || a_min_y > b_max_y) return false;

    for(uint32_t i=0; i<a_cnt-1; i++){
        for(uint32_t j=0; j<b_cnt-1; j++){
            if(SegmentsIntersect(a[i].lon, a[i].lat, a[i+1].lon, a[i+1].lat, b[j].lon, b[j].lat, b[j+1].lon, b[j+1].lat)) return true;
        }
    }
    return false;
}

static inline uint32_t SwapEndian32(uint32_t val) { return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) | ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000); }
static inline uint64_t EncodeMorton(int32_t x, int32_t y) {
    uint64_t xx = (uint32_t)x, yy = (uint32_t)y;
    xx = (xx | (xx << 16)) & 0x0000FFFF0000FFFFULL; xx = (xx | (xx <<  8)) & 0x00FF00FF00FF00FFULL;
    xx = (xx | (xx <<  4)) & 0x0F0F0F0F0F0F0F0FULL; xx = (xx | (xx <<  2)) & 0x3333333333333333ULL; xx = (xx | (xx <<  1)) & 0x5555555555555555ULL;
    yy = (yy | (yy << 16)) & 0x0000FFFF0000FFFFULL; yy = (yy | (yy <<  8)) & 0x00FF00FF00FF00FFULL;
    yy = (yy | (yy <<  4)) & 0x0F0F0F0F0F0F0F0FULL; yy = (yy | (yy <<  2)) & 0x3333333333333333ULL; yy = (yy | (yy <<  1)) & 0x5555555555555555ULL;
    return xx | (yy << 1);
}
static inline int CompareSortEntry(const void* a, const void* b) { uint64_t ma = ((SortEntry*)a)->morton, mb = ((SortEntry*)b)->morton; if (ma < mb) return -1; if (ma > mb) return 1; return 0; }

#endif // QJM_H