/* ============================================================================
 * qjmnavig - QJM Map Navigator & Editor
 * gcc -O3 -m32 -mwindows -Wall -Wextra qjmnavig.c -o qjmnavig.exe -lcomdlg32 -lgdi32 -lws2_32 -Wl,--large-address-aware
 *
 *  2026 HUGO
 *
 * THIS WORK IS NOT FIT FOR ANY FUNCTION OR PURPOSE, COMES WITH NO WARRANTY,
 * AND IS BEING RELEASED INTO THE PUBLIC DOMAIN.
 * ============================================================================ */
#define BUILD_NAVIGATOR
#include "qjm.h"
#include "qjmhttpd.h"
#ifndef CLASS_PLACE
#define CLASS_PLACE 7
#define CLASS_POI 8
#define LRU_SIZE 1024
#endif
CRITICAL_SECTION g_FileCS;

MapChunk* g_Chunks = NULL;
uint32_t g_ChunkCount = 0;
bool g_HasOverview = false;
SearchItem* g_SearchItems = NULL;
uint32_t g_SearchItemCount = 0, g_SearchItemCap = 0;
int g_HighlightedChunk = -1, g_HighlightedFeat = -1;
LRUCacheEntry g_LRU[LRU_SIZE];
uint64_t g_FrameCounter = 0;

// Edit mode state (-e argument)
bool g_EditMode = false;
bool g_MapModified = false;
int g_SelectedNode = -1;
bool g_DraggingNode = false;
uint8_t** g_DirtyBlobs = NULL;
uint32_t* g_DirtySizes = NULL;
char g_PrimaryMapFile[MAX_PATH] = {0};

// Top highway at country zoom
char g_TopHighwayName[128] = {0};

double g_WorldMapMaxZoom = 500.0;  
// World map visible when zoomed out (<= 500.0)
double g_HighwayZoomMult = 4.0;    
// Minor roads appear when zoom >= (500.0 * 4.0 = 2000.0)

int g_SidebarW = 250;
double g_CenterLon = 0.0, g_CenterLat = 0.0, g_PanX = 0.0, g_PanY = 0.0, g_Zoom = 5.0, g_Rotation = 0.0;
bool g_AutoFit = false, g_HasSavedView = false, g_IsDragging = false, g_MouseMoved = false, g_CanvasDirty = true, g_SavePins = true, g_IsLoading = false;
int g_WinX = 26, g_WinY = 26, g_WinW = 1024, g_WinH = 768, g_CanvasW = 0, g_CanvasH = 0;
double g_CanvasPanX = -999, g_CanvasPanY = -999, g_CanvasZoom = -999, g_CanvasRot = -999;

float page_w_mm = 215.90f, page_h_mm = 279.40f, g_MargH = 4.00f, g_MargV = 4.00f, g_LegendScrollY = 0.0f, g_LegendMaxScroll = 0.0f;
int isLandscape = 0, g_ShowLand = 1, g_ShowWater = 1, g_ShowPark = 1, g_ShowCoast = 1, g_ShowHwyMin = 1, g_ShowHwyMain = 1, g_ShowLabels = 1;
int g_PdfExpLand = 1, g_PdfExpWater = 1, g_PdfExpPark = 1, g_PdfExpCoast = 1, g_PdfExpHwyMin = 1, g_PdfExpHwyMain = 1, g_PdfExpLabels = 1;
int g_ShowDebug = 0, g_RouteMode = 0;

char g_MapFilesStr[4096] = "";
POINT g_LastMousePos;
HBITMAP g_hbmCanvas = NULL, g_hbmScreen = NULL;
HDC g_hdcCanvas = NULL, g_hdcScreen = NULL;
int g_ScreenW = 0, g_ScreenH = 0;

COLORREF c_Land = 0xE9F0F2, c_Water = 0xFFCCA3, c_Park = 0xAADFCA; 
COLORREF c_Coast = 0xB49664, c_HwyMin = 0xFFFFFF, c_HwyMain = 0x13B8FD;
COLORREF c_Pin = 0x3232DC, c_PinSel = 0xFF6432, c_Route = 0xE22B8A;

MapPin g_Pins[1000];
int g_PinCount = 0, g_EditingPinIndex = -1, g_SelectedPins[1000], g_SelectedPinCount = 0, g_RouteStepCount = 0, g_RoutePathCount = 0;
RouteStep* g_RouteSteps = NULL;
MapPoint* g_RoutePath = NULL;
double g_TotalRouteDist = 0.0;
bool g_UseMiles = false, g_SearchRoads = true, g_SearchPostcodes = true;

HWND g_hMainWnd = NULL, hPdfDlg = NULL, hTxtOut, hBtnBrowse, hTxtTitle, hTxtWidth, hTxtHeight, hBtnToggle, hTxtMargH, hTxtMargV, hBtnCreatePdf;
HWND hChkLand, hChkWater, hChkPark, hChkCoast, hChkHwyMin, hChkHwyMain, hChkLabels;
HWND hTxtSearch, hBtnSearchOpts, hListSearch, hBtnRoute, hBtnCancel, hCboRouteType, hBtnShowPdf, hEditPin = NULL, hLegend = NULL;
WNDPROC OldEditProc = NULL;

long pdf_objects[4096];
int pdf_obj_cnt = 1;

char g_WorldMapFile[MAX_PATH] = {0};
FILE* g_WorldMapPtr = NULL;

static int g_EditConstraint = 0; 
// 0 = None, 1 = Horizontal, 2 = Vertical
static int g_SelectedNode2 = -1;
static double g_DragStartLon = 0.0, g_DragStartLat = 0.0;
static int g_PendingInsertSeg = -1;
static int g_PendingSelectChunk = -1, g_PendingSelectFeat = -1;
static double g_PendingInsertLon = 0.0, g_PendingInsertLat = 0.0;

typedef struct {
    char path[MAX_PATH];
    FILE* file;
    bool is_world;
    bool is_qjm2;
} LoadedMapFile;

static LoadedMapFile g_LoadedFiles[64];

static int g_LoadedFileCount = 0;

static bool g_InteractiveCanvas = false;
static int g_AllocCanvasW = 0, g_AllocCanvasH = 0;

static WNDPROC OldSearchEditProc = NULL;

typedef struct { uint32_t id; float lon; float lat; } SNode;
typedef struct {
    char maps_str[4096];
} LoadThreadArgs;

typedef struct {
    POINT* pts;
    DWORD* counts;
    uint32_t pt_cnt, pt_cap;
    uint32_t poly_cnt, poly_cap;
} PolyBatch;

typedef struct {
    char txt[64];
    uint64_t hash;
    int p1x, p1y, p2x, p2y;
    uint8_t is_main;
} LabelCand;

static void CalculateRoute(void);
static void UpdateLegendWindow(HWND hwndMain);
static void FinishPinEdit(HWND hwndParent);
char g_PendingCliCommand[4096] = {0};

static uint8_t* EnsureChunkEditable(int c_idx);
static bool IsChunkActiveForAreas(uint32_t c_idx, double zoom);
static bool IsChunkActiveForRoads(uint32_t c_idx, double zoom);
static void AutoFitMap(HWND hwnd) {
    if (!g_AutoFit || g_ChunkCount == 0) return;
    double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;

    for (uint32_t i = 0; i < g_ChunkCount; i++) {
        if (g_Chunks[i].is_overview) continue;
        if (g_Chunks[i].file == g_WorldMapPtr) continue;
        if (g_Chunks[i].max_lon - g_Chunks[i].min_lon > 90.0) continue;
        if (g_Chunks[i].min_lon < min_lon) min_lon = g_Chunks[i].min_lon;
        if (g_Chunks[i].max_lon > max_lon) max_lon = g_Chunks[i].max_lon;
        if (g_Chunks[i].min_lat < min_lat) min_lat = g_Chunks[i].min_lat;
        if (g_Chunks[i].max_lat > max_lat) max_lat = g_Chunks[i].max_lat;
    }

    if (min_lon == 180.0) {
        for (uint32_t i = 0; i < g_ChunkCount; i++) {
            if (g_Chunks[i].min_lon < min_lon) min_lon = g_Chunks[i].min_lon;
            if (g_Chunks[i].max_lon > max_lon) max_lon = g_Chunks[i].max_lon;
            if (g_Chunks[i].min_lat < min_lat) min_lat = g_Chunks[i].min_lat;
            if (g_Chunks[i].max_lat > max_lat) max_lat = g_Chunks[i].max_lat;
        }
    }
    g_CenterLon = (min_lon + max_lon) / 2.0;
    g_CenterLat = (min_lat + max_lat) / 2.0;

    RECT rect; GetClientRect(hwnd, &rect);
    double width = (rect.right > 40) ? rect.right - 40 : rect.right;
    double height = (rect.bottom > 40) ? rect.bottom - 40 : rect.bottom;

    if (max_lon - min_lon < 0.0001 || max_lat - min_lat < 0.0001) g_Zoom = 25000.0;
    else {
        double zoomX = width / (max_lon - min_lon);
        double zoomY = height / (max_lat - min_lat);
        g_Zoom = (zoomX < zoomY) ? zoomX : zoomY;
        g_Zoom *= 0.95;
    }
    g_PanX = 0; g_PanY = 0; g_Rotation = 0;
    g_AutoFit = false;
    g_CanvasDirty = true;
}

static void CopyRouteDistanceToClipboard(const char* pin_input) {
    MapPin pins[1000];
    int pin_count = 0;
    
    char buf[8192];
    strncpy(buf, pin_input, sizeof(buf) - 1);
    char* start = strstr(buf, "pins=");
    if (start) start += 5; else start = buf;

    char* token = strtok(start, "|");
    while (token && pin_count < 1000) {
        char* pLong = strstr(token, ":long:");
        char* pLat = strstr(token, ":lat:");
        if (pLong && pLat) {
            *pLong = '\0';
            strncpy(pins[pin_count].name, token, 127);
            pins[pin_count].lon = atof(pLong + 6);
            pins[pin_count].lat = atof(pLat + 5);
            pin_count++;
        }
        token = strtok(NULL, "|");
    }

    if (pin_count >= 2) {
        MapPoint* path = NULL;
        int path_cnt = 0;
        RouteStep* steps = NULL;
        int step_cnt = 0;
        double total_dist = 0.0;
        bool use_miles = false;

        CalculateRouteForPins(pins, pin_count, g_RouteMode, &path, &path_cnt, &steps, &step_cnt, &total_dist, &use_miles);

        char out_str[4096];
        int offset = snprintf(out_str, sizeof(out_str), "Total Distance: %.1f %s\n", total_dist, use_miles ? "mi" : "km");
        for (int i = 0; i < step_cnt && offset < (int)sizeof(out_str) - 128; i++) {
            offset += snprintf(out_str + offset, sizeof(out_str) - offset, "%d. %s (%.1f %s)\n", i + 1, steps[i].text, steps[i].dist, use_miles ? "mi" : "km");
        }

        if (path) free(path);
        if (steps) free(steps);

        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, strlen(out_str) + 1);
            if (hGlob) {
                char* pMem = (char*)GlobalLock(hGlob);
                strcpy(pMem, out_str);
                GlobalUnlock(hGlob);
                SetClipboardData(CF_TEXT, hGlob);
            }
            CloseClipboard();
        }
    }
}
static inline uint64_t HashLabelStr(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static bool HasNonWorldChunks(void) {
    static uint32_t cached_cnt = 0xFFFFFFFF;
    static FILE* cached_world = (FILE*)1;
    static bool cached_res = false;
    if (g_ChunkCount == cached_cnt && g_WorldMapPtr == cached_world) {
        return cached_res;
    }
    cached_res = false;
    for (uint32_t i = 0; i < g_ChunkCount; i++) {
        if (g_Chunks[i].file != g_WorldMapPtr) {
            cached_res = true;
            break;
        }
    }
    cached_cnt = g_ChunkCount;
    cached_world = g_WorldMapPtr;
    return cached_res;
}

static bool HasDetailedChunks(void) {
    return HasNonWorldChunks();
}

static bool IsFeatureVisible(uint32_t c_idx, const PFeat* f, double zoom) {
    if (!f || f->pt_cnt < 2 || c_idx >= g_ChunkCount) return false;
    MapChunk* mc = &g_Chunks[c_idx];
    bool is_world_chunk = (mc->file == g_WorldMapPtr && g_WorldMapPtr != NULL);
    bool active_areas = IsChunkActiveForAreas(c_idx, zoom);
    bool active_roads = IsChunkActiveForRoads(c_idx, zoom);

    if (is_world_chunk && HasNonWorldChunks() && (f->fc == CLASS_HWY_MINOR || f->fc == CLASS_HWY_MAIN))
        return false;

    double main_hwy_min_zoom = g_WorldMapMaxZoom / 3.0;
    double hwy_zoom_threshold = (g_WorldMapMaxZoom * g_HighwayZoomMult) / 3.0;
    bool low_poly_mode = (zoom <= g_WorldMapMaxZoom);
    double min_feat_deg = (g_EditMode ? 0.5 : (low_poly_mode ? 0.75 : 1.5)) / zoom;

    if ((f->max_lon - f->min_lon) < min_feat_deg && (f->max_lat - f->min_lat) < min_feat_deg)
        return false;

    if (f->is_area) {
        if (f->pt_cnt < 3 || !active_areas) return false;
        if (f->fc == CLASS_LAND || f->fc == CLASS_COASTLINE) return g_ShowLand != 0;
        if (f->fc == CLASS_WATER) return g_ShowWater != 0;
        if (f->fc == CLASS_PARK) return g_ShowPark != 0;
        return false;
    } else {
        if (f->fc == CLASS_COASTLINE) {
            return active_areas && (g_ShowCoast != 0);
        } else if (f->fc == CLASS_HWY_MAIN) {
            return active_roads && (g_ShowHwyMain != 0) && (zoom >= main_hwy_min_zoom);
        } else if (f->fc == CLASS_HWY_MINOR) {
            return active_roads && (g_ShowHwyMin != 0) && (zoom >= hwy_zoom_threshold);
        }
        return false;
    }
}

static bool IsChunkActiveForRoads(uint32_t c_idx, double zoom) {
    if (c_idx >= g_ChunkCount) return false;
    MapChunk* mc = &g_Chunks[c_idx];
    bool is_world_chunk = (mc->file == g_WorldMapPtr && g_WorldMapPtr != NULL);
    bool has_non_world = HasNonWorldChunks();

    if (!has_non_world) return is_world_chunk;
    if (is_world_chunk) return false;

    double main_hwy_min_zoom = g_WorldMapMaxZoom / 3.0;
    if (zoom < main_hwy_min_zoom) return false;

    if (zoom <= g_WorldMapMaxZoom || (zoom < 500.0 && !g_EditMode)) {
        return g_HasOverview ? mc->is_overview : !mc->is_overview;
    }
    return !mc->is_overview;
}
static bool IsChunkActiveForAreas(uint32_t c_idx, double zoom) {
    if (c_idx >= g_ChunkCount) return false;
    MapChunk* mc = &g_Chunks[c_idx];
    bool is_world_chunk = (mc->file == g_WorldMapPtr && g_WorldMapPtr != NULL);
    bool has_non_world = HasNonWorldChunks();

    // If only a world map is loaded, its chunks are active at all zoom levels
    if (!has_non_world) return is_world_chunk;

    // When zoomed out (<= g_WorldMapMaxZoom), show worldMap if present, otherwise overview chunks
    if (zoom <= g_WorldMapMaxZoom) {
        if (g_WorldMapPtr != NULL && !g_EditMode) return is_world_chunk;
        if (is_world_chunk) return false;
        return g_HasOverview ? mc->is_overview : !mc->is_overview;
    }

    // Intermediate overview zoom (< 500.0) when not using worldMap
    if (zoom < 500.0 && g_HasOverview && !g_EditMode) {
        if (is_world_chunk) return false;
        return mc->is_overview;
    }

    // Detailed zoom (or Edit Mode above worldMap zoom)
    if (is_world_chunk) return false;
    return !mc->is_overview;
}

static void ProcessInputString(HWND hwnd, const char* input) {
    if (!input || !input[0]) return;
    
    bool do_route = true;
    const char* start = input;
    if (strncmp(start, "plot:", 5) == 0) {
        do_route = false;
        start += 5;
    } else if (strncmp(start, "route:", 6) == 0) {
        do_route = true;
        start += 6;
    } else if (strncmp(start, "pins=", 5) == 0) {
        start += 5;
    }
    
    char buf[8192];
    strncpy(buf, start, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    
    for (int i = (int)strlen(buf) - 1; i >= 0; i--) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ') buf[i] = '\0';
        else break;
    }

    int new_pins_start = g_PinCount;
    int added_pins = 0;

    char* token = strtok(buf, "|");
    while (token && g_PinCount < 1000) {
        while (*token == ' ') token++;
        
        char* pLong = strstr(token, ":long:");
        char* pLat = strstr(token, ":lat:");
        if (pLong && pLat) {
            *pLong = '\0';
            strncpy(g_Pins[g_PinCount].name, token, 127);
            g_Pins[g_PinCount].lon = atof(pLong + 6);
            g_Pins[g_PinCount].lat = atof(pLat + 5);
            *pLong = ':'; 
            g_PinCount++;
            added_pins++;
        } else {
            char pin_name[128] = "";
            char search_token[128] = "";
            char* colon = strchr(token, ':');
            
            if (colon) {
                int name_len = colon - token;
                if (name_len > 127) name_len = 127;
                strncpy(pin_name, token, name_len);
                pin_name[name_len] = '\0';
                colon++;
                while (*colon == ' ') colon++;
                strncpy(search_token, colon, 127);
            } else {
                strncpy(pin_name, token, 127);
                strncpy(search_token, token, 127);
            }

            double t_lat = 0, t_lon = 0;
            char* comma = strchr(search_token, ',');
            bool is_coord = false;
            if (comma) {
                char* endptr;
                t_lat = strtod(search_token, &endptr);
                if (endptr != search_token) {
                    while (*endptr == ' ') endptr++;
                    if (endptr == comma) {
                        char* endptr2;
                        t_lon = strtod(comma + 1, &endptr2);
                        if (endptr2 != comma + 1) {
                            is_coord = true;
                        }
                    }
                }
            }

            if (is_coord) {
                strncpy(g_Pins[g_PinCount].name, pin_name, 127);
                g_Pins[g_PinCount].lat = t_lat;
                g_Pins[g_PinCount].lon = t_lon;
                g_PinCount++;
                added_pins++;
            } else {
                char pc_token[128];
                int pc_idx = 0;
                for (int i = 0; search_token[i]; i++) {
                    if (search_token[i] != ' ') pc_token[pc_idx++] = search_token[i];
                }
                pc_token[pc_idx] = '\0';

                double found_lon = 0, found_lat = 0;
                bool found = false;
                EnterCriticalSection(&g_FileCS);
                for (uint32_t i = 0; i < g_SearchItemCount; i++) {
                    SearchItem* item = &g_SearchItems[i];
                    if (g_SearchPostcodes && item->pcode) {
                        char item_pc[128];
                        int idx = 0;
                        for (int j = 0; item->pcode[j]; j++) {
                            if (item->pcode[j] != ' ') item_pc[idx++] = item->pcode[j];
                        }
                        item_pc[idx] = '\0';
                        if (strcasecmp(item_pc, pc_token) == 0) {
                            found_lon = item->lon;
                            found_lat = item->lat;
                            found = true;
                            break;
                        }
                    }
                    if (!found && g_SearchRoads && item->name && strcasecmp(item->name, search_token) == 0) {
                        found_lon = item->lon;
                        found_lat = item->lat;
                        found = true;
                        break;
                    }
                }
                LeaveCriticalSection(&g_FileCS);

                if (found) {
                    strncpy(g_Pins[g_PinCount].name, pin_name, 127);
                    g_Pins[g_PinCount].lon = found_lon;
                    g_Pins[g_PinCount].lat = found_lat;
                    g_PinCount++;
                    added_pins++;
                }
            }
        }
        token = strtok(NULL, "|");
    }

    if (added_pins > 0) {
        g_SelectedPinCount = 0;
        if (do_route && added_pins >= 2) {
            for (int i = 0; i < added_pins; i++) {
                g_SelectedPins[g_SelectedPinCount++] = new_pins_start + i;
            }
            CalculateRoute();
            CopyRouteDistanceToClipboard(input);
        } else {
            if (g_RoutePath) { free(g_RoutePath); g_RoutePath = NULL; }
            if (g_RouteSteps) { free(g_RouteSteps); g_RouteSteps = NULL; }
            g_RouteStepCount = 0; g_RoutePathCount = 0; g_TotalRouteDist = 0.0;
            ShowWindow(hBtnCancel, SW_HIDE); ShowWindow(hCboRouteType, SW_HIDE);
            UpdateLegendWindow(hwnd);
            
            if (OpenClipboard(NULL)) {
                EmptyClipboard();
                char out_str[4096] = "Plotted Points:\n";
                int offset = strlen(out_str);
                for (int i = 0; i < added_pins && offset < 4000; i++) {
                    offset += snprintf(out_str + offset, sizeof(out_str) - offset, "%s: %.6f, %.6f\n", 
                        g_Pins[new_pins_start + i].name, g_Pins[new_pins_start + i].lat, g_Pins[new_pins_start + i].lon);
                }
                HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, strlen(out_str) + 1);
                if (hGlob) {
                    char* pMem = (char*)GlobalLock(hGlob);
                    strcpy(pMem, out_str);
                    GlobalUnlock(hGlob);
                    SetClipboardData(CF_TEXT, hGlob);
                }
                CloseClipboard();
            }
        }

        double min_lon = 180, max_lon = -180, min_lat = 90, max_lat = -90;
        for (int i = 0; i < added_pins; i++) {
            int idx = new_pins_start + i;
            if (g_Pins[idx].lon < min_lon) min_lon = g_Pins[idx].lon;
            if (g_Pins[idx].lon > max_lon) max_lon = g_Pins[idx].lon;
            if (g_Pins[idx].lat < min_lat) min_lat = g_Pins[idx].lat;
            if (g_Pins[idx].lat > max_lat) max_lat = g_Pins[idx].lat;
        }
        g_CenterLon = (min_lon + max_lon) / 2.0;
        g_CenterLat = (min_lat + max_lat) / 2.0;

        RECT rect; GetClientRect(hwnd, &rect);
        double width = (rect.right > 40) ? rect.right - 40 : rect.right;
        double height = (rect.bottom > 40) ? rect.bottom - 40 : rect.bottom;

        if (max_lon - min_lon < 0.0001 || max_lat - min_lat < 0.0001) g_Zoom = 25000.0;
        else {
            double zoomX = width / (max_lon - min_lon);
            double zoomY = height / (max_lat - min_lat);
            g_Zoom = (zoomX < zoomY) ? zoomX : zoomY;
            g_Zoom *= 0.85;
        }
        g_PanX = 0; g_PanY = 0; g_Rotation = 0;
        g_CanvasDirty = true;
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
    }
}

static bool PointInPolygonPts(const float* pts, uint32_t pt_cnt, double lon, double lat) {
    if (pt_cnt < 3) return false;
    bool inside = false;
    for (uint32_t i = 0, j = pt_cnt - 1; i < pt_cnt; j = i++) {
        double xi = pts[i * 2], yi = pts[i * 2 + 1];
        double xj = pts[j * 2], yj = pts[j * 2 + 1];
        if (((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / (yj - yi + 1e-15) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}
static void RecalcFeatureAndChunkBounds(int c_idx, int f_idx) {
    uint8_t* data = g_DirtyBlobs ? g_DirtyBlobs[c_idx] : NULL;
    if (!data) return;
    uint32_t f_cnt = *(uint32_t*)data;
    PFeat* feats = (PFeat*)(data + 12);
    float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
    PFeat* f = &feats[f_idx];
    float min_lon = 180.0f, max_lon = -180.0f, min_lat = 90.0f, max_lat = -90.0f;
    for (uint32_t j = 0; j < f->pt_cnt; j++) {
        float lon = points[f->pt_off + j * 2], lat = points[f->pt_off + j * 2 + 1];
        if (lon < min_lon) min_lon = lon; if (lon > max_lon) max_lon = lon;
        if (lat < min_lat) min_lat = lat; if (lat > max_lat) max_lat = lat;
    }
    f->min_lon = min_lon; f->max_lon = max_lon; f->min_lat = min_lat; f->max_lat = max_lat;
    if (min_lon < g_Chunks[c_idx].min_lon) g_Chunks[c_idx].min_lon = min_lon;
    if (max_lon > g_Chunks[c_idx].max_lon) g_Chunks[c_idx].max_lon = max_lon;
    if (min_lat < g_Chunks[c_idx].min_lat) g_Chunks[c_idx].min_lat = min_lat;
    if (max_lat > g_Chunks[c_idx].max_lat) g_Chunks[c_idx].max_lat = max_lat;
}

static void ClosePolylineAndAddNode(int c_idx, int f_idx, int node1, int node2) {
    uint8_t* data = EnsureChunkEditable(c_idx);
    if (!data) return;
    uint32_t f_cnt = *(uint32_t*)data;
    uint32_t p_cnt = *(uint32_t*)(data + 4);
    uint32_t s_len = *(uint32_t*)(data + 8);
    if (f_idx < 0 || (uint32_t)f_idx >= f_cnt) return;

    PFeat* feats = (PFeat*)(data + 12);
    PFeat* f = &feats[f_idx];
    if (node1 < 0 || (uint32_t)node1 >= f->pt_cnt || node2 < 0 || (uint32_t)node2 >= f->pt_cnt) return;

    if (node1 > node2) {
        int tmp = node1;
        node1 = node2;
        node2 = tmp;
    }

    float* old_pts = (float*)(data + 12 + f_cnt * sizeof(PFeat));
    float lon1 = old_pts[f->pt_off + node1 * 2], lat1 = old_pts[f->pt_off + node1 * 2 + 1];
    float lon2 = old_pts[f->pt_off + node2 * 2], lat2 = old_pts[f->pt_off + node2 * 2 + 1];
    float new_lon = (lon1 + lon2) * 0.5f;
    float new_lat = (lat1 + lat2) * 0.5f;

    uint32_t old_sz = g_DirtySizes[c_idx];
    uint32_t new_sz = old_sz + 2 * sizeof(float);
    uint8_t* new_buf = (uint8_t*)malloc(new_sz);
    if (!new_buf) return;

    uint32_t new_p_cnt = p_cnt + 2;
    memcpy(new_buf, &f_cnt, 4);
    memcpy(new_buf + 4, &new_p_cnt, 4);
    memcpy(new_buf + 8, &s_len, 4);

    PFeat* new_feats = (PFeat*)(new_buf + 12);
    memcpy(new_feats, feats, f_cnt * sizeof(PFeat));

    uint32_t ins_float_idx = f->pt_off + (node2 + 1) * 2;
    float* new_pts = (float*)(new_buf + 12 + f_cnt * sizeof(PFeat));
    memcpy(new_pts, old_pts, ins_float_idx * sizeof(float));
    new_pts[ins_float_idx] = new_lon;
    new_pts[ins_float_idx + 1] = new_lat;
    memcpy(new_pts + ins_float_idx + 2, old_pts + ins_float_idx, (p_cnt - ins_float_idx) * sizeof(float));

    char* old_strs = (char*)((uint8_t*)old_pts + p_cnt * sizeof(float));
    char* new_strs = (char*)((uint8_t*)new_pts + new_p_cnt * sizeof(float));
    memcpy(new_strs, old_strs, s_len);

    new_feats[f_idx].pt_cnt++;
    new_feats[f_idx].is_area = 1;
    for (uint32_t i = 0; i < f_cnt; i++) {
        if (i != (uint32_t)f_idx && new_feats[i].pt_off > f->pt_off) {
            new_feats[i].pt_off += 2;
        }
    }

    free(data);
    g_DirtyBlobs[c_idx] = new_buf;
    g_DirtySizes[c_idx] = new_sz;
    g_Chunks[c_idx].uncomp_size = new_sz;
    g_SelectedNode = node2 + 1;
    g_MapModified = true;
    RecalcFeatureAndChunkBounds(c_idx, f_idx);
}
static void DeleteFeature(int c_idx, int f_idx) {
    uint8_t* data = EnsureChunkEditable(c_idx);
    if (!data) return;
    uint32_t f_cnt = *(uint32_t*)data;
    uint32_t p_cnt = *(uint32_t*)(data + 4);
    uint32_t s_len = *(uint32_t*)(data + 8);
    if (f_idx < 0 || (uint32_t)f_idx >= f_cnt) return;

    PFeat* feats = (PFeat*)(data + 12);
    PFeat* f = &feats[f_idx];
    uint32_t pts_to_remove = f->pt_cnt;
    uint32_t rm_off = f->pt_off;
    uint32_t del_float_cnt = pts_to_remove * 2;

    uint32_t old_sz = g_DirtySizes[c_idx];
    uint32_t new_sz = old_sz - sizeof(PFeat) - (del_float_cnt * sizeof(float));
    uint8_t* new_buf = (uint8_t*)malloc(new_sz);
    if (!new_buf) return;

    uint32_t new_f_cnt = f_cnt - 1;
    uint32_t new_p_cnt = p_cnt - del_float_cnt;

    memcpy(new_buf, &new_f_cnt, 4);
    memcpy(new_buf + 4, &new_p_cnt, 4);
    memcpy(new_buf + 8, &s_len, 4);

    PFeat* new_feats = (PFeat*)(new_buf + 12);
    float* old_pts = (float*)(data + 12 + f_cnt * sizeof(PFeat));
    float* new_pts = (float*)(new_buf + 12 + new_f_cnt * sizeof(PFeat));
    char* old_strs = (char*)((uint8_t*)old_pts + p_cnt * sizeof(float));
    char* new_strs = (char*)((uint8_t*)new_pts + new_p_cnt * sizeof(float));

    if (f_idx > 0) memcpy(new_feats, feats, f_idx * sizeof(PFeat));
    if ((uint32_t)f_idx < f_cnt - 1) memcpy(new_feats + f_idx, feats + f_idx + 1, (f_cnt - 1 - f_idx) * sizeof(PFeat));

    for (uint32_t i = f_idx; i < new_f_cnt; i++) {
        if (new_feats[i].pt_off > rm_off) {
            new_feats[i].pt_off -= del_float_cnt;
        }
    }

    if (rm_off > 0) memcpy(new_pts, old_pts, rm_off * sizeof(float));
    if (rm_off + del_float_cnt < p_cnt) memcpy(new_pts + rm_off, old_pts + rm_off + del_float_cnt, (p_cnt - rm_off - del_float_cnt) * sizeof(float));

    memcpy(new_strs, old_strs, s_len);

    free(data);
    g_DirtyBlobs[c_idx] = new_buf;
    g_DirtySizes[c_idx] = new_sz;
    g_Chunks[c_idx].uncomp_size = new_sz;
    g_HighlightedFeat = -1;
    g_SelectedNode = -1;
    g_SelectedNode2 = -1;
    g_MapModified = true;
}

int cmp_snode(const void* a, const void* b) {
    float d = ((const SNode*)a)->lon - ((const SNode*)b)->lon;
    return (d > 0) - (d < 0);
}

static inline void NavScreenToLatLon(int x, int y, int w, int h, double* lon, double* lat) {
    double dx = x - (w / 2.0) - g_PanX;
    double dy = y - (h / 2.0) - g_PanY;
    if (fabs(g_Rotation) > 1e-6) {
        double c = cos(-g_Rotation), s = sin(-g_Rotation);
        double rx = dx * c - dy * s;
        double ry = dx * s + dy * c;
        dx = rx; dy = ry;
    }
    *lon = g_CenterLon + dx / g_Zoom;
    *lat = g_CenterLat - dy / g_Zoom;
}

static inline void NavLatLonToScreen(double lon, double lat, int w, int h, double pan_x, double pan_y, double zoom, double* px, double* py) {
    double dx = (lon - g_CenterLon) * zoom;
    double dy = -(lat - g_CenterLat) * zoom;
    if (fabs(g_Rotation) > 1e-6) {
        double c = cos(g_Rotation), s = sin(g_Rotation);
        double rx = dx * c - dy * s;
        double ry = dx * s + dy * c;
        dx = rx; dy = ry;
    }
    *px = (w / 2.0) + dx + pan_x;
    *py = (h / 2.0) + dy + pan_y;
}

static inline void NavLatLonToCanvasScreen(double lon, double lat, int w, int h, double* px, double* py) {
    NavLatLonToScreen(lon, lat, w, h, g_PanX, g_PanY, g_Zoom, px, py);
}
static LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_DOWN || wParam == VK_UP) {
            if (IsWindowVisible(hListSearch)) {
                int count = SendMessage(hListSearch, LB_GETCOUNT, 0, 0);
                if (count > 0) {
                    int sel = SendMessage(hListSearch, LB_GETCURSEL, 0, 0);
                    if (wParam == VK_DOWN) sel++;
                    else sel--;
                    if (sel < 0) sel = count - 1;
                    if (sel >= count) sel = 0;
                    SendMessage(hListSearch, LB_SETCURSEL, sel, 0);
                    return 0; 
                }
            }
        } else if (wParam == VK_RETURN) {
            if (IsWindowVisible(hListSearch)) {
                int sel = SendMessage(hListSearch, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    SendMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(1003, LBN_SELCHANGE), (LPARAM)hListSearch);
                    return 0; 
                }
            }
        } else if (wParam == VK_ESCAPE) {
            if (IsWindowVisible(hListSearch)) {
                ShowWindow(hListSearch, SW_HIDE);
                return 0;
            }
        }
    } else if (msg == WM_GETDLGCODE) {
        if (wParam == VK_RETURN) return DLGC_WANTALLKEYS;
    }
    return CallWindowProc(OldSearchEditProc, hwnd, msg, wParam, lParam);
}

static int SanitizeMapLabel(const char* raw, uint32_t max_rem, char* out, uint32_t out_cap) {
    if (!raw || max_rem == 0 || out_cap < 2) return 0;
    uint32_t limit = (max_rem < out_cap - 1) ? max_rem : (out_cap - 1);
    uint32_t len = 0;
    bool has_alnum = false;
    while (len < limit && raw[len] != '\0') {
        uint8_t c = (uint8_t)raw[len];
        if (c < 32 || c >= 127) {
            out[0] = '\0';
            return 0;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            has_alnum = true;
        }
        out[len++] = (char)c;
    }
    if (len == 0 || len >= limit || raw[len] != '\0' || !has_alnum) {
        out[0] = '\0';
        return 0;
    }
    out[len] = '\0';
    return (int)len;
}

static void ComputeScreenRectLatLonBounds(const RECT* r, int w, int h, double pad_deg,
                                          double* min_lon, double* max_lon, double* min_lat, double* max_lat) {
    int xs[4] = { r->left, r->right, r->left, r->right };
    int ys[4] = { r->top,  r->top,   r->bottom, r->bottom };
    *min_lon = 1e30; *max_lon = -1e30;
    *min_lat = 1e30; *max_lat = -1e30;
    for (int i = 0; i < 4; i++) {
        double lon, lat;
        NavScreenToLatLon(xs[i], ys[i], w, h, &lon, &lat);
        if (lon < *min_lon) *min_lon = lon;
        if (lon > *max_lon) *max_lon = lon;
        if (lat < *min_lat) *min_lat = lat;
        if (lat > *max_lat) *max_lat = lat;
    }
    *min_lon -= pad_deg; *max_lon += pad_deg;
    *min_lat -= pad_deg; *max_lat += pad_deg;
}

static void RenderCanvasRegion(HWND hwnd, const RECT* r1, const RECT* r2, bool draw_labels) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    bool is_strip = (r1 != NULL || r2 != NULL);

    if (!is_strip && (fabs(g_PanX) > 0.01 || fabs(g_PanY) > 0.01)) {
        double new_lon, new_lat;
        NavScreenToLatLon(w / 2, h / 2, w, h, &new_lon, &new_lat);
        g_CenterLon = new_lon;
        g_CenterLat = new_lat;
        g_PanX = 0.0;
        g_PanY = 0.0;
    }

    if (!g_hdcCanvas) {
        HDC hdc = GetDC(hwnd);
        g_hdcCanvas = CreateCompatibleDC(hdc);
        ReleaseDC(hwnd, hdc);
    }
    if (g_AllocCanvasW != w || g_AllocCanvasH != h) {
        if (g_hbmCanvas) DeleteObject(g_hbmCanvas);
        HDC hdc = GetDC(hwnd);
        g_hbmCanvas = CreateCompatibleBitmap(hdc, w, h);
        ReleaseDC(hwnd, hdc);
        SelectObject(g_hdcCanvas, g_hbmCanvas);
        g_AllocCanvasW = w;
        g_AllocCanvasH = h;
    }
    g_CanvasW = w;
    g_CanvasH = h;

    g_CanvasPanX = g_PanX;
    g_CanvasPanY = g_PanY;
    g_CanvasZoom = g_Zoom;
    g_CanvasRot  = g_Rotation;
    if (!is_strip) g_CanvasDirty = false;

    if (!is_strip && g_HighlightedChunk >= 0 && (uint32_t)g_HighlightedChunk < g_ChunkCount && g_HighlightedFeat >= 0) {
        bool still_vis = false;
        uint8_t* h_data = GetChunkData(g_HighlightedChunk);
        if (h_data) {
            uint32_t f_cnt = *(uint32_t*)h_data;
            if ((uint32_t)g_HighlightedFeat < f_cnt) {
                PFeat* hf = &((PFeat*)(h_data + 12))[g_HighlightedFeat];
                still_vis = IsFeatureVisible((uint32_t)g_HighlightedChunk, hf, g_Zoom);
            }
        }
        if (!still_vis) {
            g_HighlightedChunk = -1;
            g_HighlightedFeat = -1;
            g_SelectedNode = -1;
            g_SelectedNode2 = -1;
        }
    }

    HRGN hClipRgn = NULL;
    double pad_deg = 8.0 / g_Zoom;
    double b1_min_lon = 0, b1_max_lon = 0, b1_min_lat = 0, b1_max_lat = 0;
    double b2_min_lon = 0, b2_max_lon = 0, b2_min_lat = 0, b2_max_lat = 0;
    bool has_b1 = false, has_b2 = false;

    HBRUSH bgBrush = CreateSolidBrush(c_Water);
    if (is_strip) {
        if (r1) {
            hClipRgn = CreateRectRgnIndirect(r1);
            ComputeScreenRectLatLonBounds(r1, w, h, pad_deg, &b1_min_lon, &b1_max_lon, &b1_min_lat, &b1_max_lat);
            has_b1 = true;
        }
        if (r2) {
            HRGN rgn2 = CreateRectRgnIndirect(r2);
            if (hClipRgn) {
                CombineRgn(hClipRgn, hClipRgn, rgn2, RGN_OR);
                DeleteObject(rgn2);
            } else {
                hClipRgn = rgn2;
            }
            ComputeScreenRectLatLonBounds(r2, w, h, pad_deg, &b2_min_lon, &b2_max_lon, &b2_min_lat, &b2_max_lat);
            has_b2 = true;
        }
        SelectClipRgn(g_hdcCanvas, hClipRgn);
        FillRgn(g_hdcCanvas, hClipRgn, bgBrush);
    } else {
        RECT full_rc = { 0, 0, w, h };
        FillRect(g_hdcCanvas, &full_rc, bgBrush);
        ComputeScreenRectLatLonBounds(&full_rc, w, h, pad_deg, &b1_min_lon, &b1_max_lon, &b1_min_lat, &b1_max_lat);
        has_b1 = true;
    }
    DeleteObject(bgBrush);

    bool has_rot = (fabs(g_Rotation) > 1e-6);
    double half_cw = w * 0.5 + g_PanX;
    double half_ch = h * 0.5 + g_PanY;
    double off_x = half_cw - g_CenterLon * g_Zoom;
    double off_y = half_ch + g_CenterLat * g_Zoom;
    double neg_zoom = -g_Zoom;
    double cos_r = has_rot ? cos(g_Rotation) : 1.0;
    double sin_r = has_rot ? sin(g_Rotation) : 0.0;

    static POINT* screen_pts = NULL;
    static uint32_t screen_pts_cap = 0;
    static PolyBatch batches[3] = {0}; // 0: Coast, 1: Minor Hwy, 2: Main Hwy
    static LabelCand* label_cands = NULL;
    static uint32_t label_cand_cap = 0;
    uint32_t label_cand_cnt = 0;

    for (int b = 0; b < 3; b++) {
        batches[b].pt_cnt = 0;
        batches[b].poly_cnt = 0;
    }

    double main_hwy_min_zoom = g_WorldMapMaxZoom / 3.0;
    double hwy_zoom_threshold = (g_WorldMapMaxZoom * g_HighwayZoomMult) / 3.0;
    bool low_poly_mode = (g_Zoom <= g_WorldMapMaxZoom);
    double min_feat_deg = (g_EditMode ? 0.5 : (low_poly_mode ? 0.75 : 1.5)) / g_Zoom;
    bool has_non_world = HasNonWorldChunks();
    bool want_labels = (draw_labels && !is_strip && g_ShowLabels && g_Zoom >= main_hwy_min_zoom);

    HBRUSH hBrWater = CreateSolidBrush(c_Water);
    HBRUSH hBrLand  = CreateSolidBrush(c_Land);
    HBRUSH hBrPark  = CreateSolidBrush(c_Park);
    HPEN   hPenMain = CreatePen(PS_SOLID, 2, c_HwyMain);
    HPEN   hPenMin  = CreatePen(PS_SOLID, 1, c_HwyMin);
    HPEN   hPenCoast = CreatePen(PS_SOLID, 1, c_Coast);
    HPEN   hNullPen = (HPEN)GetStockObject(NULL_PEN);
    HBRUSH hNullBr  = (HBRUSH)GetStockObject(NULL_BRUSH);

    SelectObject(g_hdcCanvas, hNullPen);
    HBRUSH currentBrush = NULL;

    EnterCriticalSection(&g_FileCS);

    for (uint32_t c = 0; c < g_ChunkCount; c++) {
        MapChunk* mc = &g_Chunks[c];
        bool c_in_b1 = has_b1 && !(mc->max_lon < b1_min_lon || mc->min_lon > b1_max_lon || mc->max_lat < b1_min_lat || mc->min_lat > b1_max_lat);
        bool c_in_b2 = has_b2 && !(mc->max_lon < b2_min_lon || mc->min_lon > b2_max_lon || mc->max_lat < b2_min_lat || mc->min_lat > b2_max_lat);
        if (!c_in_b1 && !c_in_b2) continue;

        bool active_areas = IsChunkActiveForAreas(c, g_Zoom);
        bool active_roads = IsChunkActiveForRoads(c, g_Zoom);
        if (!active_areas && !active_roads) continue;

        bool is_world_chunk = (mc->file == g_WorldMapPtr && g_WorldMapPtr != NULL);

        uint8_t* data = GetChunkData(c);
        if (!data) continue;
        uint32_t uncomp_sz = mc->uncomp_size;
        uint32_t f_cnt = *(uint32_t*)data;
        uint32_t p_cnt = *(uint32_t*)(data + 4);
        uint32_t s_len = *(uint32_t*)(data + 8);
        uint32_t geom_end = 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float);
        if (geom_end >= uncomp_sz) s_len = 0;
        else if (geom_end + s_len > uncomp_sz) s_len = uncomp_sz - geom_end;

        PFeat* feats = (PFeat*)(data + 12);
        float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
        char* strings = (char*)((uint8_t*)points + p_cnt * sizeof(float));

        for (uint32_t i = 0; i < f_cnt; i++) {
            PFeat* f = &feats[i];
            if (f->pt_cnt < 2 || f->pt_off + f->pt_cnt * 2 > p_cnt) continue;
            if ((f->max_lon - f->min_lon) < min_feat_deg && (f->max_lat - f->min_lat) < min_feat_deg) continue;

            bool f_in_b1 = c_in_b1 && !(f->max_lon < b1_min_lon || f->min_lon > b1_max_lon || f->max_lat < b1_min_lat || f->min_lat > b1_max_lat);
            bool f_in_b2 = c_in_b2 && !(f->max_lon < b2_min_lon || f->min_lon > b2_max_lon || f->max_lat < b2_min_lat || f->min_lat > b2_max_lat);
            if (!f_in_b1 && !f_in_b2) continue;

            if (is_world_chunk && has_non_world && (f->fc == CLASS_HWY_MINOR || f->fc == CLASS_HWY_MAIN)) continue;

            int batch_id = -1;
            bool draw_area = false;
            bool check_label = false;

            if (f->is_area) {
                if (!active_areas || f->pt_cnt < 3) continue;
                if (!g_EditMode && g_Zoom < 10000.0 && ((f->max_lon - f->min_lon) * g_Zoom < 3.0) && ((f->max_lat - f->min_lat) * g_Zoom < 3.0)) continue;
                if ((f->fc == CLASS_LAND || f->fc == CLASS_COASTLINE) && g_ShowLand) draw_area = true;
                else if (f->fc == CLASS_WATER && g_ShowWater) draw_area = true;
                else if (f->fc == CLASS_PARK && g_ShowPark) draw_area = true;
                if (!draw_area) continue;
            } else {
                if (f->fc == CLASS_COASTLINE) {
                    if (active_areas && g_ShowCoast) batch_id = 0;
                } else if (f->fc == CLASS_HWY_MINOR) {
                    if (active_roads && g_ShowHwyMin && g_Zoom >= hwy_zoom_threshold) batch_id = 1;
                    if (want_labels && active_roads && g_Zoom >= 5000.0 && f->name_off > 0 && f->name_off < s_len) check_label = true;
                } else if (f->fc == CLASS_HWY_MAIN) {
                    if (active_roads && g_ShowHwyMain && g_Zoom >= main_hwy_min_zoom) batch_id = 2;
                    if (want_labels && active_roads && f->name_off > 0 && f->name_off < s_len) check_label = true;
                } else if (f->fc == CLASS_PLACE || f->fc == CLASS_POI) {
                    if (want_labels && active_roads && f->name_off > 0 && f->name_off < s_len) check_label = true;
                }
                if (batch_id == -1 && !check_label) continue;
            }

            POINT* dst_pts = NULL;
            if (batch_id >= 0) {
                PolyBatch* pb = &batches[batch_id];
                if (pb->pt_cnt + f->pt_cnt > pb->pt_cap) {
                    uint32_t new_cap = (pb->pt_cap == 0) ? 65536 : pb->pt_cap * 2;
                    while (new_cap < pb->pt_cnt + f->pt_cnt) new_cap *= 2;
                    POINT* tmp = (POINT*)realloc(pb->pts, new_cap * sizeof(POINT));
                    if (!tmp) continue;
                    pb->pts = tmp;
                    pb->pt_cap = new_cap;
                }
                if (pb->poly_cnt + 1 > pb->poly_cap) {
                    uint32_t new_cap = (pb->poly_cap == 0) ? 16384 : pb->poly_cap * 2;
                    DWORD* tmp = (DWORD*)realloc(pb->counts, new_cap * sizeof(DWORD));
                    if (!tmp) continue;
                    pb->counts = tmp;
                    pb->poly_cap = new_cap;
                }
                dst_pts = pb->pts + pb->pt_cnt;
            } else {
                if (f->pt_cnt > screen_pts_cap) {
                    screen_pts_cap = f->pt_cnt + 1024;
                    POINT* tmp = (POINT*)realloc(screen_pts, screen_pts_cap * sizeof(POINT));
                    if (!tmp) { screen_pts_cap = 0; screen_pts = NULL; continue; }
                    screen_pts = tmp;
                }
                dst_pts = screen_pts;
            }

            uint32_t pt_idx = 0;
            int last_px = -999999, last_py = -999999;
            int min_px_step = (low_poly_mode || f->is_area) ? 1 : 2;
            const float* f_pts = points + f->pt_off;

            if (!has_rot) {
                for (uint32_t j = 0; j < f->pt_cnt; j++) {
                    double dsx = f_pts[j * 2] * g_Zoom + off_x;
                    double dsy = f_pts[j * 2 + 1] * neg_zoom + off_y;
                    if (dsx < -32000.0) dsx = -32000.0; else if (dsx > 32000.0) dsx = 32000.0;
                    if (dsy < -32000.0) dsy = -32000.0; else if (dsy > 32000.0) dsy = 32000.0;
                    int sx = (int)dsx, sy = (int)dsy;
                    if (j == 0 || j == f->pt_cnt - 1 || abs(sx - last_px) >= min_px_step || abs(sy - last_py) >= min_px_step) {
                        dst_pts[pt_idx].x = sx; dst_pts[pt_idx].y = sy;
                        pt_idx++; last_px = sx; last_py = sy;
                    }
                }
            } else {
                for (uint32_t j = 0; j < f->pt_cnt; j++) {
                    double dx = (f_pts[j * 2] - g_CenterLon) * g_Zoom;
                    double dy = (g_CenterLat - f_pts[j * 2 + 1]) * g_Zoom;
                    double dsx = half_cw + dx * cos_r - dy * sin_r;
                    double dsy = half_ch + dx * sin_r + dy * cos_r;
                    if (dsx < -32000.0) dsx = -32000.0; else if (dsx > 32000.0) dsx = 32000.0;
                    if (dsy < -32000.0) dsy = -32000.0; else if (dsy > 32000.0) dsy = 32000.0;
                    int sx = (int)dsx, sy = (int)dsy;
                    if (j == 0 || j == f->pt_cnt - 1 || abs(sx - last_px) >= min_px_step || abs(sy - last_py) >= min_px_step) {
                        dst_pts[pt_idx].x = sx; dst_pts[pt_idx].y = sy;
                        pt_idx++; last_px = sx; last_py = sy;
                    }
                }
            }

            if (draw_area) {
                if (pt_idx >= 3) {
                    HBRUSH targetBrush = (f->fc == CLASS_WATER) ? hBrWater : ((f->fc == CLASS_PARK) ? hBrPark : hBrLand);
                    if (currentBrush != targetBrush) {
                        SelectObject(g_hdcCanvas, targetBrush);
                        currentBrush = targetBrush;
                    }
                    Polygon(g_hdcCanvas, dst_pts, pt_idx);
                }
            } else {
                if (batch_id >= 0 && pt_idx >= 2) {
                    PolyBatch* pb = &batches[batch_id];
                    pb->counts[pb->poly_cnt++] = pt_idx;
                    pb->pt_cnt += pt_idx;
                }
                if (check_label && pt_idx >= 2 && f->name_off < s_len) {
                    char clean_txt[64];
                    if (SanitizeMapLabel(strings + f->name_off, s_len - f->name_off, clean_txt, sizeof(clean_txt)) > 0) {
                        double max_len = 0.0;
                        uint32_t best_i = 0;
                        for (uint32_t j = 0; j < pt_idx - 1; j++) {
                            double p_dx = dst_pts[j+1].x - dst_pts[j].x;
                            double p_dy = dst_pts[j+1].y - dst_pts[j].y;
                            double len = p_dx * p_dx + p_dy * p_dy;
                            if (len > max_len) { max_len = len; best_i = j; }
                        }
                        if (max_len >= 30.0) {
                            int cx = (dst_pts[best_i].x + dst_pts[best_i+1].x) / 2;
                            int cy = (dst_pts[best_i].y + dst_pts[best_i+1].y) / 2;
                            if (cx >= 0 && cx <= w && cy >= 0 && cy <= h) {
                                if (label_cand_cnt >= label_cand_cap) {
                                    label_cand_cap = (label_cand_cap == 0) ? 2048 : label_cand_cap * 2;
                                    LabelCand* tmp = (LabelCand*)realloc(label_cands, label_cand_cap * sizeof(LabelCand));
                                    if (tmp) label_cands = tmp;
                                }
                                if (label_cands && label_cand_cnt < label_cand_cap) {
                                    LabelCand* lc = &label_cands[label_cand_cnt++];
                                    strcpy(lc->txt, clean_txt);
                                    lc->hash = HashLabelStr(lc->txt);
                                    lc->p1x = dst_pts[best_i].x;
                                    lc->p1y = dst_pts[best_i].y;
                                    lc->p2x = dst_pts[best_i+1].x;
                                    lc->p2y = dst_pts[best_i+1].y;
                                    lc->is_main = (f->fc == CLASS_HWY_MAIN) ? 1 : 0;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    SelectObject(g_hdcCanvas, hNullBr);
    if (batches[0].poly_cnt > 0) {
        SelectObject(g_hdcCanvas, hPenCoast);
        PolyPolyline(g_hdcCanvas, batches[0].pts, batches[0].counts, batches[0].poly_cnt);
    }
    if (batches[1].poly_cnt > 0) {
        SelectObject(g_hdcCanvas, hPenMin);
        PolyPolyline(g_hdcCanvas, batches[1].pts, batches[1].counts, batches[1].poly_cnt);
    }
    if (batches[2].poly_cnt > 0) {
        SelectObject(g_hdcCanvas, hPenMain);
        PolyPolyline(g_hdcCanvas, batches[2].pts, batches[2].counts, batches[2].poly_cnt);
    }

    if (want_labels && label_cand_cnt > 0) {
        static HFONT s_MeasureFont = NULL;
        static HFONT s_RotFonts[37] = {0};
        if (!s_MeasureFont) {
            s_MeasureFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
        }

        static RECT drawn_labels[4000];
        static uint64_t drawn_hashes[4000];
        int label_cnt = 0;

        HFONT hOldFont = (HFONT)SelectObject(g_hdcCanvas, s_MeasureFont);
        SetTextColor(g_hdcCanvas, RGB(40, 40, 40));
        SetBkMode(g_hdcCanvas, TRANSPARENT);
        SetTextAlign(g_hdcCanvas, TA_CENTER | TA_BASELINE);

        for (int prio = 1; prio >= 0; prio--) {
            for (uint32_t i = 0; i < label_cand_cnt && label_cnt < 4000; i++) {
                LabelCand* lc = &label_cands[i];
                if ((int)lc->is_main != prio) continue;

                bool already_drawn = false;
                for (int k = 0; k < label_cnt; k++) {
                    if (drawn_hashes[k] == lc->hash) { already_drawn = true; break; }
                }
                if (already_drawn) continue;

                int len = (int)strlen(lc->txt);
                SIZE sz;
                GetTextExtentPoint32A(g_hdcCanvas, lc->txt, len, &sz);
                int cx = (lc->p1x + lc->p2x) / 2;
                int cy = (lc->p1y + lc->p2y) / 2;
                int hw = sz.cx / 2, hh = sz.cy / 2;
                int pad = 10;
                RECT tr = { cx - hw - pad, cy - hh - pad, cx + hw + pad, cy + hh + pad };

                bool collision = false;
                for (int k = 0; k < label_cnt; k++) {
                    RECT dummy;
                    if (IntersectRect(&dummy, &tr, &drawn_labels[k])) { collision = true; break; }
                }
                if (collision) continue;

                double angle_deg = atan2(lc->p1y - lc->p2y, lc->p2x - lc->p1x) * 180.0 / 3.1415926535;
                while (angle_deg > 90.0) angle_deg -= 180.0;
                while (angle_deg < -90.0) angle_deg += 180.0;

                int bucket = (int)((angle_deg + 92.5) / 5.0);
                if (bucket < 0) bucket = 0;
                else if (bucket > 36) bucket = 36;

                if (!s_RotFonts[bucket]) {
                    int esc = (bucket * 5 - 90) * 10;
                    if (esc < 0) esc += 3600;
                    s_RotFonts[bucket] = CreateFontA(16, 0, esc, esc, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                }

                SelectObject(g_hdcCanvas, s_RotFonts[bucket]);
                TextOutA(g_hdcCanvas, cx, cy + (sz.cy / 3), lc->txt, len);
                SelectObject(g_hdcCanvas, s_MeasureFont);

                drawn_labels[label_cnt] = tr;
                drawn_hashes[label_cnt] = lc->hash;
                label_cnt++;
            }
        }
        SelectObject(g_hdcCanvas, hOldFont);
    }

    LeaveCriticalSection(&g_FileCS);

    if (hClipRgn) {
        SelectClipRgn(g_hdcCanvas, NULL);
        DeleteObject(hClipRgn);
    }

    SelectObject(g_hdcCanvas, hNullPen);
    SelectObject(g_hdcCanvas, hNullBr);
    DeleteObject(hBrWater);
    DeleteObject(hBrLand);
    DeleteObject(hBrPark);
    DeleteObject(hPenMain);
    DeleteObject(hPenMin);
    DeleteObject(hPenCoast);
}

static uint8_t* GetChunkData(int c_idx) {
    if (c_idx < 0 || (uint32_t)c_idx >= g_ChunkCount) return NULL;
    EnterCriticalSection(&g_FileCS);
    if (g_DirtyBlobs && g_DirtyBlobs[c_idx]) {
        uint8_t* ret = g_DirtyBlobs[c_idx];
        LeaveCriticalSection(&g_FileCS);
        return ret;
    }

    static int16_t* s_ChunkToLRU = NULL;
    static uint32_t s_ChunkToLRUCap = 0;
    if (g_ChunkCount > s_ChunkToLRUCap) {
        uint32_t new_cap = g_ChunkCount + 4096;
        int16_t* tmp = (int16_t*)realloc(s_ChunkToLRU, new_cap * sizeof(int16_t));
        if (tmp) {
            for (uint32_t i = s_ChunkToLRUCap; i < new_cap; i++) tmp[i] = -1;
            s_ChunkToLRU = tmp;
            s_ChunkToLRUCap = new_cap;
            for (int i = 0; i < LRU_SIZE; i++) {
                int cid = g_LRU[i].chunk_idx;
                if (cid >= 0 && (uint32_t)cid < s_ChunkToLRUCap) s_ChunkToLRU[cid] = (int16_t)i;
            }
        }
    }

    g_FrameCounter++;
    if (s_ChunkToLRU) {
        int slot = s_ChunkToLRU[c_idx];
        if (slot >= 0 && slot < LRU_SIZE && g_LRU[slot].chunk_idx == c_idx) {
            g_LRU[slot].last_accessed = g_FrameCounter;
            uint8_t* ret = g_LRU[slot].blob;
            LeaveCriticalSection(&g_FileCS);
            return ret;
        }
    } else {
        for (int i = 0; i < LRU_SIZE; i++) {
            if (g_LRU[i].chunk_idx == c_idx) {
                g_LRU[i].last_accessed = g_FrameCounter;
                uint8_t* ret = g_LRU[i].blob;
                LeaveCriticalSection(&g_FileCS);
                return ret;
            }
        }
    }

    int evict_i = 0;
    uint64_t oldest = 0xFFFFFFFFFFFFFFFFULL;
    for (int i = 0; i < LRU_SIZE; i++) {
        if (g_LRU[i].chunk_idx == -1) { evict_i = i; break; }
        if (g_LRU[i].last_accessed < oldest) { oldest = g_LRU[i].last_accessed; evict_i = i; }
    }
    if (g_LRU[evict_i].chunk_idx != -1) {
        if (s_ChunkToLRU && (uint32_t)g_LRU[evict_i].chunk_idx < s_ChunkToLRUCap) {
            s_ChunkToLRU[g_LRU[evict_i].chunk_idx] = -1;
        }
        free(g_LRU[evict_i].blob);
        g_LRU[evict_i].chunk_idx = -1;
    }

    MapChunk* mc = &g_Chunks[c_idx];
    FSEEK64(mc->file, mc->offset, SEEK_SET);

    uint8_t* comp_buf = malloc(mc->comp_size);
    fread(comp_buf, 1, mc->comp_size, mc->file);
    uint8_t* uncomp_buf = malloc(mc->uncomp_size);

    lzav_decompress(comp_buf, uncomp_buf, mc->comp_size, mc->uncomp_size);
    free(comp_buf);

    g_LRU[evict_i].chunk_idx = c_idx;
    g_LRU[evict_i].blob = uncomp_buf;
    g_LRU[evict_i].last_accessed = g_FrameCounter;
    if (s_ChunkToLRU) s_ChunkToLRU[c_idx] = (int16_t)evict_i;

    LeaveCriticalSection(&g_FileCS);
    return uncomp_buf;
}
static uint8_t* EnsureChunkEditable(int c_idx) {
    if (c_idx < 0 || (uint32_t)c_idx >= g_ChunkCount) return NULL;
    if (g_DirtyBlobs[c_idx]) return g_DirtyBlobs[c_idx];
    uint8_t* src = GetChunkData(c_idx);
    if (!src) return NULL;
    uint32_t sz = g_Chunks[c_idx].uncomp_size;
    uint8_t* copy = (uint8_t*)malloc(sz);
    if (!copy) return NULL;
    memcpy(copy, src, sz);
    g_DirtyBlobs[c_idx] = copy;
    g_DirtySizes[c_idx] = sz;
    g_MapModified = true;
    return copy;
}

static void InsertNodeInFeature(int c_idx, int f_idx, int after_node) {
    uint8_t* data = EnsureChunkEditable(c_idx);
    if (!data) return;
    uint32_t f_cnt = *(uint32_t*)data;
    uint32_t p_cnt = *(uint32_t*)(data + 4);
    uint32_t s_len = *(uint32_t*)(data + 8);
    if (f_idx < 0 || (uint32_t)f_idx >= f_cnt) return;

    PFeat* feats = (PFeat*)(data + 12);
    PFeat* f = &feats[f_idx];
    if (after_node < 0 || (uint32_t)after_node >= f->pt_cnt) after_node = f->pt_cnt - 1;

    float* old_pts = (float*)(data + 12 + f_cnt * sizeof(PFeat));
    float new_lon, new_lat;
    if (g_PendingInsertSeg != -1) {
        new_lon = (float)g_PendingInsertLon;
        new_lat = (float)g_PendingInsertLat;
    } else if (!f->is_area && (uint32_t)after_node == f->pt_cnt - 1 && f->pt_cnt >= 2) {
        float lon1 = old_pts[f->pt_off + (after_node - 1) * 2], lat1 = old_pts[f->pt_off + (after_node - 1) * 2 + 1];
        float lon2 = old_pts[f->pt_off + after_node * 2],       lat2 = old_pts[f->pt_off + after_node * 2 + 1];
        new_lon = lon2 + (lon2 - lon1) * 0.25f;
        new_lat = lat2 + (lat2 - lat1) * 0.25f;
    } else {
        uint32_t next_node = ((uint32_t)after_node + 1 < f->pt_cnt) ? (uint32_t)after_node + 1 : 0;
        float lon1 = old_pts[f->pt_off + after_node * 2], lat1 = old_pts[f->pt_off + after_node * 2 + 1];
        float lon2 = old_pts[f->pt_off + next_node * 2],  lat2 = old_pts[f->pt_off + next_node * 2 + 1];
        new_lon = (lon1 + lon2) * 0.5f;
        new_lat = (lat1 + lat2) * 0.5f;
    }

    uint32_t old_sz = g_DirtySizes[c_idx];
    uint32_t new_sz = old_sz + 2 * sizeof(float);
    uint8_t* new_buf = (uint8_t*)malloc(new_sz);
    if (!new_buf) return;

    uint32_t new_p_cnt = p_cnt + 2;
    memcpy(new_buf, &f_cnt, 4);
    memcpy(new_buf + 4, &new_p_cnt, 4);
    memcpy(new_buf + 8, &s_len, 4);

    PFeat* new_feats = (PFeat*)(new_buf + 12);
    memcpy(new_feats, feats, f_cnt * sizeof(PFeat));

    uint32_t ins_float_idx = f->pt_off + (after_node + 1) * 2;
    float* new_pts = (float*)(new_buf + 12 + f_cnt * sizeof(PFeat));
    memcpy(new_pts, old_pts, ins_float_idx * sizeof(float));
    new_pts[ins_float_idx] = new_lon;
    new_pts[ins_float_idx + 1] = new_lat;
    memcpy(new_pts + ins_float_idx + 2, old_pts + ins_float_idx, (p_cnt - ins_float_idx) * sizeof(float));

    char* old_strs = (char*)((uint8_t*)old_pts + p_cnt * sizeof(float));
    char* new_strs = (char*)((uint8_t*)new_pts + new_p_cnt * sizeof(float));
    memcpy(new_strs, old_strs, s_len);

    new_feats[f_idx].pt_cnt++;
    for (uint32_t i = 0; i < f_cnt; i++) {
        if (i != (uint32_t)f_idx && new_feats[i].pt_off > f->pt_off) {
            new_feats[i].pt_off += 2;
        }
    }

    free(data);
    g_DirtyBlobs[c_idx] = new_buf;
    g_DirtySizes[c_idx] = new_sz;
    g_Chunks[c_idx].uncomp_size = new_sz;
    g_SelectedNode = after_node + 1;
    g_MapModified = true;
    RecalcFeatureAndChunkBounds(c_idx, f_idx);
}
// IMPORTANT: Replace the existing DeleteNodeInFeature AND SaveEditedQJM functions with these corrected versions.
// 1. DeleteNodeInFeature had a severe structural bug that corrupted the chunk memory layout when saving.
// 2. SaveEditedQJM now explicitly unlocks thread mutexes, clears the LRU cache, and forcibly unloads all handles (including world maps) before writing.

static void DeleteNodeInFeature(int c_idx, int f_idx, int del_node) {
    uint8_t* data = EnsureChunkEditable(c_idx);
    if (!data) return;
    uint32_t f_cnt = *(uint32_t*)data;
    uint32_t p_cnt = *(uint32_t*)(data + 4);
    uint32_t s_len = *(uint32_t*)(data + 8);
    if (f_idx < 0 || (uint32_t)f_idx >= f_cnt) return;
    PFeat* feats = (PFeat*)(data + 12);
    PFeat* f = &feats[f_idx];
    if (f->pt_cnt <= 2 || del_node < 0 || (uint32_t)del_node >= f->pt_cnt) return;

    float* pts = (float*)(data + 12 + f_cnt * sizeof(PFeat));
    uint32_t del_float_idx = f->pt_off + del_node * 2;
    
    // CORRECTED: Move ALL subsequent points left by 2 floats (not just within the feature)
    memmove(pts + del_float_idx, pts + del_float_idx + 2, (p_cnt - del_float_idx - 2) * sizeof(float));
    
    // CORRECTED: Move the trailing string block left by 8 bytes (2 floats) to prevent corruption
    char* strs = (char*)((uint8_t*)pts + p_cnt * sizeof(float));
    memmove(strs - 8, strs, s_len);

    f->pt_cnt--;
    
    // CORRECTED: Update total float count in chunk header
    *(uint32_t*)(data + 4) = p_cnt - 2;

    // CORRECTED: Update point offsets for all subsequent features
    for (uint32_t i = 0; i < f_cnt; i++) {
        if (i != (uint32_t)f_idx && feats[i].pt_off > f->pt_off) {
            feats[i].pt_off -= 2;
        }
    }

    g_DirtySizes[c_idx] -= 8;
    g_Chunks[c_idx].uncomp_size -= 8;
    
    if (g_SelectedNode >= (int)f->pt_cnt) g_SelectedNode = (int)f->pt_cnt - 1;
    g_MapModified = true;
    RecalcFeatureAndChunkBounds(c_idx, f_idx);
}

static void ScrollAndUpdateCanvas(HWND hwnd, int dx, int dy) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    if (!g_hdcScreen) {
        HDC hdc = GetDC(hwnd);
        g_hdcScreen = CreateCompatibleDC(hdc);
        ReleaseDC(hwnd, hdc);
    }
    if (g_ScreenW != w || g_ScreenH != h) {
        if (g_hbmScreen) DeleteObject(g_hbmScreen);
        HDC hdc = GetDC(hwnd);
        g_hbmScreen = CreateCompatibleBitmap(hdc, w, h);
        ReleaseDC(hwnd, hdc);
        SelectObject(g_hdcScreen, g_hbmScreen);
        g_ScreenW = w; g_ScreenH = h;
    }

    BitBlt(g_hdcScreen, 0, 0, w, h, g_hdcCanvas, 0, 0, SRCCOPY);
    BitBlt(g_hdcCanvas, dx, dy, w, h, g_hdcScreen, 0, 0, SRCCOPY);

    RECT r1 = {0}, r2 = {0};
    bool has_r1 = false, has_r2 = false;
    int ov = 2;

    if (dx > 0) {
        r1.left = 0; r1.top = 0;
        r1.right = (dx + ov < w) ? (dx + ov) : w; r1.bottom = h;
        has_r1 = true;
    } else if (dx < 0) {
        r1.left = (w + dx - ov > 0) ? (w + dx - ov) : 0; r1.top = 0;
        r1.right = w; r1.bottom = h;
        has_r1 = true;
    }

    if (dy > 0) {
        r2.left = 0; r2.top = 0;
        r2.right = w; r2.bottom = (dy + ov < h) ? (dy + ov) : h;
        has_r2 = true;
    } else if (dy < 0) {
        r2.left = 0; r2.top = (h + dy - ov > 0) ? (h + dy - ov) : 0;
        r2.right = w; r2.bottom = h;
        has_r2 = true;
    }

    RenderCanvasRegion(hwnd, has_r1 ? &r1 : NULL, has_r2 ? &r2 : NULL, false);
}
static void SaveEditedQJM(void) {
    if (!g_EditMode || !g_MapModified || g_ChunkCount == 0 || g_LoadedFileCount == 0) return;

    EnterCriticalSection(&g_FileCS);

    for (int f_id = 0; f_id < g_LoadedFileCount; f_id++) {
        bool file_dirty = false;
        for (uint32_t i = 0; i < g_ChunkCount; i++) {
            if (g_Chunks[i].file_id == f_id && g_DirtyBlobs && g_DirtyBlobs[i]) {
                file_dirty = true;
                break;
            }
        }
        if (!file_dirty) continue;

        const char* target_path = g_LoadedFiles[f_id].path;
        FILE* orig_file = g_LoadedFiles[f_id].file;
        if (!target_path[0] || !orig_file) continue;

        char tmp_path[MAX_PATH + 32];
        snprintf(tmp_path, sizeof(tmp_path), "%.259s.save.tmp", target_path);
        FILE* f_out = fopen(tmp_path, "wb");
        if (!f_out) continue;

        fwrite("QJMC", 1, 4, f_out);

        uint32_t high_cnt = 0, over_cnt = 0;
        for (uint32_t i = 0; i < g_ChunkCount; i++) {
            if (g_Chunks[i].file_id != f_id) continue;
            if (g_Chunks[i].is_overview) over_cnt++; else high_cnt++;
        }

        ChunkMetaDisk* h_meta = high_cnt ? malloc(high_cnt * sizeof(ChunkMetaDisk)) : NULL;
        ChunkMetaDisk* o_meta = over_cnt ? malloc(over_cnt * sizeof(ChunkMetaDisk)) : NULL;
        uint32_t h_idx = 0, o_idx = 0;

        for (int pass = 0; pass < 2; pass++) {
            bool want_over = (pass == 1);
            for (uint32_t i = 0; i < g_ChunkCount; i++) {
                MapChunk* mc = &g_Chunks[i];
                if (mc->file_id != f_id || mc->is_overview != want_over) continue;

                uint64_t new_off = FTELL64(f_out);
                uint32_t comp_sz = 0, uncomp_sz = mc->uncomp_size;

                if (g_DirtyBlobs && g_DirtyBlobs[i]) {
                    uncomp_sz = g_DirtySizes[i];
                    int bound = lzav_compress_bound((int)uncomp_sz);
                    uint8_t* cbuf = malloc(bound);
                    comp_sz = (uint32_t)lzav_compress_default(g_DirtyBlobs[i], cbuf, (int)uncomp_sz, bound);
                    fwrite(cbuf, 1, comp_sz, f_out);
                    free(cbuf);
                } else {
                    comp_sz = mc->comp_size;
                    uint8_t* cbuf = malloc(comp_sz);
                    FSEEK64(orig_file, mc->offset, SEEK_SET);
                    fread(cbuf, 1, comp_sz, orig_file);
                    fwrite(cbuf, 1, comp_sz, f_out);
                    free(cbuf);
                }

                mc->offset = new_off;
                mc->comp_size = comp_sz;
                mc->uncomp_size = uncomp_sz;

                ChunkMetaDisk* dst = want_over ? &o_meta[o_idx++] : &h_meta[h_idx++];
                dst->min_lon = mc->min_lon; dst->max_lon = mc->max_lon;
                dst->min_lat = mc->min_lat; dst->max_lat = mc->max_lat;
                dst->offset = new_off; dst->comp_size = comp_sz; dst->uncomp_size = uncomp_sz;
            }
        }

        uint64_t high_dir_off = FTELL64(f_out);
        if (h_meta && high_cnt > 0) fwrite(h_meta, sizeof(ChunkMetaDisk), high_cnt, f_out);
        uint64_t over_dir_off = FTELL64(f_out);
        if (o_meta && over_cnt > 0) fwrite(o_meta, sizeof(ChunkMetaDisk), over_cnt, f_out);

        fwrite(&high_dir_off, 8, 1, f_out); fwrite(&high_cnt, 4, 1, f_out);
        fwrite(&over_dir_off, 8, 1, f_out); fwrite(&over_cnt, 4, 1, f_out);
        fwrite("QJM2", 1, 4, f_out);

        fclose(f_out);
        if (h_meta) free(h_meta);
        if (o_meta) free(o_meta);

        fclose(orig_file);

        int rename_ok = 0;
        for (int i = 0; i < 10; i++) {
            remove(target_path);
            if (rename(tmp_path, target_path) == 0) {
                rename_ok = 1;
                break;
            }
            Sleep(20);
        }
        if (!rename_ok) {
            MoveFileExA(tmp_path, target_path, MOVEFILE_REPLACE_EXISTING);
        }

        FILE* reopened = fopen(target_path, "rb");
        g_LoadedFiles[f_id].file = reopened;
        if (g_LoadedFiles[f_id].is_world) {
            g_WorldMapPtr = reopened;
        }

        for (uint32_t i = 0; i < g_ChunkCount; i++) {
            if (g_Chunks[i].file_id == f_id) {
                g_Chunks[i].file = reopened;
                if (g_DirtyBlobs && g_DirtyBlobs[i]) {
                    free(g_DirtyBlobs[i]);
                    g_DirtyBlobs[i] = NULL;
                    g_DirtySizes[i] = 0;
                }
            }
        }
    }

    g_MapModified = false;

    for (int i = 0; i < LRU_SIZE; i++) {
        if (g_LRU[i].chunk_idx != -1) {
            free(g_LRU[i].blob);
            g_LRU[i].chunk_idx = -1;
        }
    }

    LeaveCriticalSection(&g_FileCS);
}
//char g_WorldMapFile[MAX_PATH] = {0};
//FILE* g_WorldMapPtr = NULL;

bool IsTopConnectedHighway(uint32_t fc, uint32_t pt_cnt, const char* name) {
    (void)fc;
    (void)pt_cnt;
    if (!name || !name[0] || !g_TopHighwayName[0]) return false;
    return (strcmp(name, g_TopHighwayName) == 0);
}

#include "pdfs.h"
static void SyncPdfDialogOrientation(void) {
    if (!g_hMainWnd || !hPdfDlg) return;
    RECT rc; GetClientRect(g_hMainWnd, &rc);
    float short_mm = fminf(page_w_mm, page_h_mm);
    float long_mm  = fmaxf(page_w_mm, page_h_mm);
    if (rc.right >= rc.bottom) {
        page_w_mm = long_mm; page_h_mm = short_mm; isLandscape = 1;
    } else {
        page_w_mm = short_mm; page_h_mm = long_mm; isLandscape = 0;
    }
    char buf[32];
    sprintf(buf, "%.2f", page_w_mm); SetWindowTextA(hTxtWidth, buf);
    sprintf(buf, "%.2f", page_h_mm); SetWindowTextA(hTxtHeight, buf);
    SetWindowTextA(hBtnToggle, isLandscape ? "Land" : "Port");
}

static void CreatePDFAction(HWND hwnd) {
    char out_path[MAX_PATH]; GetWindowTextA(hTxtOut, out_path, MAX_PATH);
    char title_str[128], margH_str[16], margV_str[16];
    GetWindowTextA(hTxtTitle, title_str, 128); GetWindowTextA(hTxtMargH, margH_str, 16); GetWindowTextA(hTxtMargV, margV_str, 16);
    
    g_PdfExpLand = (SendMessage(hChkLand, BM_GETCHECK, 0, 0) == BST_CHECKED); g_PdfExpWater = (SendMessage(hChkWater, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpPark = (SendMessage(hChkPark, BM_GETCHECK, 0, 0) == BST_CHECKED); g_PdfExpCoast = (SendMessage(hChkCoast, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpHwyMin = (SendMessage(hChkHwyMin, BM_GETCHECK, 0, 0) == BST_CHECKED); g_PdfExpHwyMain = (SendMessage(hChkHwyMain, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_PdfExpLabels = (SendMessage(hChkLabels, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    RECT rc = {0, 0, 1024, 768};
    if (g_hMainWnd) GetClientRect(g_hMainWnd, &rc);
    generate_pdf(out_path, title_str, atof(margH_str), atof(margV_str), (double)rc.right, (double)rc.bottom);
    ShowWindow(hwnd, SW_HIDE);
}

static void DrawMap(HDC hdc, int w, int h, double pan_x, double pan_y) {
    HBRUSH bgOceanBrush = CreateSolidBrush(c_Water); FillRect(hdc, &(RECT){0, 0, w, h}, bgOceanBrush); DeleteObject(bgOceanBrush);

    if (g_ChunkCount == 0) {
        if (!g_IsLoading) {
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(0, 50, 150)); SetTextAlign(hdc, TA_CENTER | TA_BASELINE);
            TextOutA(hdc, w / 2, h / 2, "Drag a .qjm file here...", 24);
        }
        return;
    }

    HPEN coastPen = CreatePen(PS_SOLID, 2, c_Coast), hwyMinorPen = CreatePen(PS_SOLID, 3, c_HwyMin), hwyMainPen = CreatePen(PS_SOLID, 5, c_HwyMain); 
    HBRUSH landBrush = CreateSolidBrush(c_Land), parkBrush = CreateSolidBrush(c_Park), waterBrush = CreateSolidBrush(c_Water); HPEN nullPen = GetStockObject(NULL_PEN);

    double diag = sqrt((double)w * w + (double)h * h) / (2.0 * g_Zoom) + 0.15;
    double eff_lon = g_CenterLon - pan_x / g_Zoom;
    double eff_lat = g_CenterLat + pan_y / g_Zoom;
    double vp_min_lon = eff_lon - diag, vp_max_lon = eff_lon + diag;
    double vp_min_lat = eff_lat - diag, vp_max_lat = eff_lat + diag;

    static POINT* screen_pts = NULL; static uint32_t screen_pts_cap = 0; 
    static RECT drawn_labels[4000]; static const char* drawn_names[4000]; int label_cnt = 0, name_cnt = 0;
    bool use_overview = (!g_EditMode && g_Zoom < 1500.0 && g_HasOverview);
    bool country_zoom = (!g_EditMode && g_Zoom < 80.0 && g_TopHighwayName[0] != '\0');

    for (int pass = 1; pass <= 7; pass++) {
        if (country_zoom && (pass == 2 || pass == 3 || pass == 4 || pass == 7)) continue;

        for (uint32_t c = 0; c < g_ChunkCount; c++) {
            MapChunk* mc = &g_Chunks[c];
            if (mc->file == g_WorldMapPtr && g_Zoom > 250.0) continue; 
            if (use_overview && !mc->is_overview) continue;
            if (!use_overview && mc->is_overview) continue;
            if (mc->max_lon < vp_min_lon || mc->min_lon > vp_max_lon || mc->max_lat < vp_min_lat || mc->min_lat > vp_max_lat) continue;

            uint8_t* data = GetChunkData(c); if(!data) continue;
            uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12);
            float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat)); char* strings = (char*)((uint8_t*)points + (*(uint32_t*)(data+4)) * sizeof(float));

            for (uint32_t i = 0; i < f_cnt; i++) {
                PFeat* f = &feats[i]; if (f->pt_cnt < 2) continue;
                if (f->max_lon < vp_min_lon || f->min_lon > vp_max_lon || f->max_lat < vp_min_lat || f->min_lat > vp_max_lat) continue;

                double feat_w = f->max_lon - f->min_lon, feat_h = f->max_lat - f->min_lat;
                if (!g_EditMode && f->is_area && g_Zoom < 10000.0 && (feat_w * g_Zoom < 6.0) && (feat_h * g_Zoom < 6.0)) continue;
                
                if (!use_overview && !g_EditMode) {
                    if (g_Zoom < 50.0 && feat_w < 0.005 && feat_h < 0.005) continue; 
                    if (g_Zoom < 10.0 && feat_w < 0.02 && feat_h < 0.02) continue; 
                    if (g_Zoom < 2.0 && feat_w < 0.1 && feat_h < 0.1) continue; 
                }

                if (pass == 1) { 
                    if (!g_ShowLand || !f->is_area || (f->fc != CLASS_LAND && f->fc != CLASS_COASTLINE)) continue;
                } else if (pass == 2) { 
                    if (!f->is_area || (f->fc == CLASS_WATER && !g_ShowWater) || (f->fc == CLASS_PARK && !g_ShowPark) || (f->fc != CLASS_WATER && f->fc != CLASS_PARK)) continue;
                } else if (pass == 3) { 
                    if (!g_ShowCoast || f->is_area || f->fc != CLASS_COASTLINE) continue;
                } else if (pass == 4) { 
                    if (!g_ShowHwyMin || f->fc != CLASS_HWY_MINOR || g_Zoom < 10000.0 || use_overview) continue;
                } else if (pass == 5) { 
                    if (!g_ShowHwyMain || f->fc != CLASS_HWY_MAIN) continue;
                    if (country_zoom && (!f->name_off || strcmp(strings + f->name_off, g_TopHighwayName) != 0)) continue;
                } else if (pass == 6) { 
                    if (!g_ShowLabels || f->name_off == 0) continue;
                    if (country_zoom) {
                        if (f->fc != CLASS_HWY_MAIN || strcmp(strings + f->name_off, g_TopHighwayName) != 0) continue;
                    } else {
                        if ((f->fc != CLASS_HWY_MAIN && f->fc != CLASS_PLACE && f->fc != CLASS_POI) || g_Zoom < 500.0 || use_overview) continue;
                    }
                } else if (pass == 7) { 
                    if (!g_ShowLabels || f->fc != CLASS_HWY_MINOR || g_Zoom < 15000.0 || f->name_off == 0 || use_overview) continue;
                }

                if (f->pt_cnt > screen_pts_cap) { 
                    screen_pts_cap = f->pt_cnt + 1024; 
                    POINT* tmp = (POINT*)realloc(screen_pts, screen_pts_cap * sizeof(POINT)); 
                    if (!tmp) { screen_pts_cap = 0; screen_pts = NULL; continue; }
                    screen_pts = tmp;
                }
                
                uint32_t pt_idx = 0; int last_px = -999999, last_py = -999999;
                
                for (uint32_t j = 0; j < f->pt_cnt; j++) {
                    double px, py; NavLatLonToScreen(points[f->pt_off + j*2], points[f->pt_off + j*2 + 1], w, h, pan_x, pan_y, g_Zoom, &px, &py);
                    if (px < -32000) px = -32000; else if (px > 32000) px = 32000;
                    if (py < -32000) py = -32000; else if (py > 32000) py = 32000;
                    if (j == 0 || j == f->pt_cnt - 1 || abs((int)px - last_px) >= 2 || abs((int)py - last_py) >= 2) {
                        screen_pts[pt_idx].x = (int)px; screen_pts[pt_idx].y = (int)py; pt_idx++; last_px = (int)px; last_py = (int)py;
                    }
                }
                if (pt_idx < 2) continue;
                
                if (pass == 6 || pass == 7) {
                    char* txt = strings + f->name_off;
                    bool already_drawn = false;
                    for (int k = 0; k < name_cnt; k++) { if (strcmp(drawn_names[k], txt) == 0) { already_drawn = true; break; } }
                    if (already_drawn) continue;

                    double max_len = 0; int best_i = 0;
                    for (uint32_t j = 0; j < pt_idx - 1; j++) {
                        double p_dx = screen_pts[j+1].x - screen_pts[j].x, p_dy = screen_pts[j+1].y - screen_pts[j].y, len = p_dx*p_dx + p_dy*p_dy;
                        if (len > max_len) { max_len = len; best_i = j; }
                    }
                    if (max_len < (pass == 6 ? 120 : 30)) continue; 
                    int p1x = screen_pts[best_i].x, p1y = screen_pts[best_i].y, p2x = screen_pts[best_i+1].x, p2y = screen_pts[best_i+1].y;
                    double angle_rad = atan2(p1y - p2y, p2x - p1x), angle_deg = angle_rad * 180.0 / 3.1415926535;
                    
                    if (angle_deg < 0) angle_deg += 360.0; if (angle_deg > 90.0 && angle_deg < 270.0) angle_deg -= 180.0;
                    int cx = (p1x + p2x) / 2, cy = (p1y + p2y) / 2;

                    HFONT hFont = CreateFont(16, 0, (int)(angle_deg * 10), (int)(angle_deg * 10), FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont); SIZE sz;
                    GetTextExtentPoint32(hdc, txt, strlen(txt), &sz); int hw = sz.cx / 2, hh = sz.cy / 2;
                    int pad = (pass == 6) ? 30 : 10;
                    RECT tr = { cx - hw - pad, cy - hh - pad, cx + hw + pad, cy + hh + pad };
                    
                    bool collision = false;
                    for (int k = 0; k < label_cnt; k++) { 
                        RECT dummy; if (IntersectRect(&dummy, &tr, &drawn_labels[k])) { collision = true; break; } 
                        if (drawn_names[k] && strcmp(drawn_names[k], txt) == 0) {
                            int dx = cx - ((drawn_labels[k].left + drawn_labels[k].right)/2);
                            int dy = cy - ((drawn_labels[k].top + drawn_labels[k].bottom)/2);
                            if (dx*dx + dy*dy < 400000) { collision = true; break; }
                        }
                    }
                    
                    if (!collision && label_cnt < 4000 && name_cnt < 4000) {
                        drawn_labels[label_cnt++] = tr;
                        drawn_names[name_cnt++] = txt;
                        SetTextColor(hdc, RGB(40, 40, 40)); SetBkMode(hdc, TRANSPARENT); SetTextAlign(hdc, TA_CENTER | TA_BASELINE);
                        TextOutA(hdc, cx, cy + (sz.cy/3), txt, strlen(txt));
                    }
                    SelectObject(hdc, hOldFont); DeleteObject(hFont); continue;
                }
                
                if (pass == 1) { SelectObject(hdc, landBrush); SelectObject(hdc, nullPen); Polygon(hdc, screen_pts, pt_idx); } 
                else if (pass == 2) { SelectObject(hdc, (f->fc == CLASS_WATER) ? waterBrush : parkBrush); SelectObject(hdc, nullPen); Polygon(hdc, screen_pts, pt_idx); } 
                else if (pass == 3) { SelectObject(hdc, GetStockObject(NULL_BRUSH)); SelectObject(hdc, coastPen); Polyline(hdc, screen_pts, pt_idx); } 
                else if (pass == 4) { SelectObject(hdc, GetStockObject(NULL_BRUSH)); SelectObject(hdc, hwyMinorPen); Polyline(hdc, screen_pts, pt_idx); } 
                else if (pass == 5) { SelectObject(hdc, GetStockObject(NULL_BRUSH)); SelectObject(hdc, hwyMainPen); Polyline(hdc, screen_pts, pt_idx); }
            }
        }
    }
    DeleteObject(coastPen); DeleteObject(hwyMinorPen); DeleteObject(hwyMainPen); DeleteObject(landBrush); DeleteObject(parkBrush); DeleteObject(waterBrush);
}

void DrawOverlay(HDC hdc, int w, int h) {
    if (g_HighlightedChunk != -1) {
        uint8_t* data = GetChunkData(g_HighlightedChunk);
        if (data) {
            PFeat* feats = (PFeat*)(data + 12);
            float* points = (float*)(data + 12 + (*(uint32_t*)data) * sizeof(PFeat));
            PFeat* f = &feats[g_HighlightedFeat];
            
            HPEN hlPen = CreatePen(PS_SOLID, 8, RGB(255, 50, 50));
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            SelectObject(hdc, hlPen);
            
            POINT* pts = malloc(f->pt_cnt * sizeof(POINT));
            for (uint32_t j = 0; j < f->pt_cnt; j++) {
                double px, py;
                LatLonToScreen(points[f->pt_off + j*2], points[f->pt_off + j*2 + 1], w, h, g_PanX, g_PanY, g_Zoom, &px, &py);
                pts[j].x = (LONG)px;
                pts[j].y = (LONG)py;
            }
            
            Polyline(hdc, pts, f->pt_cnt);
            free(pts);
            DeleteObject(hlPen);
        }
    }

    if (g_RoutePathCount > 0) {
        LOGBRUSH lbRoute = { BS_SOLID, c_Route, 0 };
        HPEN routePen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 6, &lbRoute, 0, NULL);
        
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        SelectObject(hdc, routePen);
        
        POINT* screen_pts = malloc(g_RoutePathCount * sizeof(POINT));
        for (int i = 0; i < g_RoutePathCount; i++) {
            double px, py;
            LatLonToScreen(g_RoutePath[i].lon, g_RoutePath[i].lat, w, h, g_PanX, g_PanY, g_Zoom, &px, &py);
            screen_pts[i].x = (LONG)px;
            screen_pts[i].y = (LONG)py;
        }
        
        Polyline(hdc, screen_pts, g_RoutePathCount);
        
        if (g_RouteStepCount > 0) {
            SetTextColor(hdc, RGB(40, 10, 80));
            SetBkMode(hdc, TRANSPARENT);
            SetTextAlign(hdc, TA_CENTER | TA_BOTTOM);
            HFONT hBoldFont = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
            HFONT hOldF = (HFONT)SelectObject(hdc, hBoldFont);

            int step_idx = 0;
            double accum_path_dist = 0;
            double next_target = g_RouteSteps[0].dist / 2.0;
            
            for (int i = 0; i < g_RoutePathCount - 1 && step_idx < g_RouteStepCount; i++) {
                double km = Haversine(g_RoutePath[i].lat, g_RoutePath[i].lon, g_RoutePath[i+1].lat, g_RoutePath[i+1].lon);
                double d_val = g_UseMiles ? (km * 0.621371) : km;
                accum_path_dist += d_val;
                
                if (accum_path_dist >= next_target) {
                    TextOutA(hdc, screen_pts[i].x, screen_pts[i].y - 8, g_RouteSteps[step_idx].text, strlen(g_RouteSteps[step_idx].text));
                    next_target += g_RouteSteps[step_idx].dist / 2.0; 
                    step_idx++;
                    if (step_idx < g_RouteStepCount) {
                        next_target += g_RouteSteps[step_idx].dist / 2.0;
                    }
                }
            }
            SelectObject(hdc, hOldF);
            DeleteObject(hBoldFont);
        }

        free(screen_pts);
        DeleteObject(routePen);
    }

    HBRUSH brPin = CreateSolidBrush(c_Pin);
    HBRUSH brPinSel = CreateSolidBrush(c_PinSel);
    HPEN pinPen = CreatePen(PS_SOLID, 1, RGB(100, 0, 0));
    HPEN pinSelPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 100));

    for (int i = 0; i < g_PinCount; i++) {
        double px, py;
        LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, w, h, g_PanX, g_PanY, g_Zoom, &px, &py);
        int x = (int)px, y = (int)py;
        
        bool selected = false;
        for(int k=0; k<g_SelectedPinCount; k++) {
            if(g_SelectedPins[k] == i) {
                selected = true;
            }
        }

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
    
    DeleteObject(brPin);
    DeleteObject(pinPen);
    DeleteObject(brPinSel);
    DeleteObject(pinSelPen);

    if (g_SelectedPinCount >= 2 && g_RouteStepCount > 0) {
        int leg_w = 260;
        int max_leg_h = h - 100;
        if (max_leg_h < 100) {
            max_leg_h = 100;
        }
        
        int req_h = 40 + (g_RouteStepCount * 20);
        int leg_h = (req_h < max_leg_h) ? req_h : max_leg_h;
        
        RECT legRect = { 10, 45, 10 + leg_w, 45 + leg_h };
        HBRUSH legBrush = CreateSolidBrush(RGB(250, 248, 245));
        HPEN legPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        
        SelectObject(hdc, legBrush);
        SelectObject(hdc, legPen);
        RoundRect(hdc, legRect.left, legRect.top, legRect.right, legRect.bottom, 15, 15);

        if (req_h > max_leg_h) {
            g_LegendMaxScroll = (float)(req_h - max_leg_h + 10);
            if (g_LegendScrollY < -g_LegendMaxScroll) {
                g_LegendScrollY = -g_LegendMaxScroll;
            }
        } else {
            g_LegendMaxScroll = 0;
            g_LegendScrollY = 0;
        }

        if (g_LegendMaxScroll > 0) {
            HBRUSH sbBg = CreateSolidBrush(RGB(230, 230, 230));
            HBRUSH sbFg = CreateSolidBrush(RGB(180, 180, 180));
            RECT sbTrack = { legRect.right - 12, legRect.top + 10, legRect.right - 4, legRect.bottom - 10 };
            FillRect(hdc, &sbTrack, sbBg);
            
            float visible_ratio = (float)leg_h / (float)req_h;
            int thumb_h = (int)((leg_h - 20) * visible_ratio);
            if (thumb_h < 20) {
                thumb_h = 20;
            }
            
            float scroll_ratio = -g_LegendScrollY / g_LegendMaxScroll;
            int thumb_y = sbTrack.top + (int)(scroll_ratio * (sbTrack.bottom - sbTrack.top - thumb_h));
            
            RECT sbThumb = { sbTrack.left, thumb_y, sbTrack.right, thumb_y + thumb_h };
            FillRect(hdc, &sbThumb, sbFg);
            
            DeleteObject(sbBg);
            DeleteObject(sbFg);
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
        DeleteObject(clipRgn);
        DeleteObject(legBrush);
        DeleteObject(legPen);
    }
}

static void UpdateLegendWindow(HWND hwndMain) {
    if (!hwndMain) return;

    if (g_RoutePathCount > 0) {
        SetWindowTextA(hwndMain, g_EditMode ? "QJM Map Navigator [EDIT MODE] - Routed" : "QJM Map Navigator - Routed");
    } else {
        SetWindowTextA(hwndMain, g_EditMode ? "QJM Map Navigator [EDIT MODE]" : "QJM Map Navigator");
    }

    if (!hLegend) return;

    if (g_SelectedPinCount >= 2 && g_RouteStepCount > 0) {
        RECT rc; GetClientRect(hwndMain, &rc);
        int leg_w = 260;
        int max_leg_h = rc.bottom - 95;
        if (max_leg_h < 100) max_leg_h = 100;

        int req_h = 40 + (g_RouteStepCount * 20);
        int leg_h = (req_h < max_leg_h) ? req_h : max_leg_h;

        if (req_h > max_leg_h) {
            g_LegendMaxScroll = (float)(req_h - max_leg_h + 10);
            if (g_LegendScrollY < -g_LegendMaxScroll) g_LegendScrollY = -g_LegendMaxScroll;
        } else {
            g_LegendMaxScroll = 0.0f;
            g_LegendScrollY = 0.0f;
        }

        SetWindowPos(hLegend, NULL, 10, 80, leg_w, leg_h, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER);
        InvalidateRect(hLegend, NULL, FALSE);
    } else {
        ShowWindow(hLegend, SW_HIDE);
    }
}

static LRESULT CALLBACK LegendWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (hEditPin) FinishPinEdit(GetParent(hwnd));
            SetFocus(GetParent(hwnd));
            return 0;
        case WM_MOUSEWHEEL: {
            float scroll_delta = ((int16_t)HIWORD(wParam) > 0) ? 30.0f : -30.0f;
            g_LegendScrollY += scroll_delta;
            if (g_LegendScrollY > 0) g_LegendScrollY = 0;
            if (g_LegendMaxScroll > 0 && g_LegendScrollY < -g_LegendMaxScroll) g_LegendScrollY = -g_LegendMaxScroll;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc); int w = rc.right, h = rc.bottom;
            HDC memDC = CreateCompatibleDC(hdc); HBITMAP memBM = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

            HBRUSH legBrush = CreateSolidBrush(RGB(250, 248, 245)); HPEN legPen = CreatePen(PS_SOLID, 1, RGB(190, 190, 190));
            SelectObject(memDC, legBrush); SelectObject(memDC, legPen); Rectangle(memDC, 0, 0, w, h);

            int req_h = 40 + (g_RouteStepCount * 20);
            if (g_LegendMaxScroll > 0) {
                HBRUSH sbBg = CreateSolidBrush(RGB(230, 230, 230)), sbFg = CreateSolidBrush(RGB(180, 180, 180));
                RECT sbTrack = { w - 12, 8, w - 4, h - 8 }; FillRect(memDC, &sbTrack, sbBg);
                int thumb_h = (int)((h - 16) * ((float)h / (float)req_h)); if (thumb_h < 20) thumb_h = 20;
                int thumb_y = sbTrack.top + (int)((-g_LegendScrollY / g_LegendMaxScroll) * (sbTrack.bottom - sbTrack.top - thumb_h));
                RECT sbThumb = { sbTrack.left, thumb_y, sbTrack.right, thumb_y + thumb_h }; FillRect(memDC, &sbThumb, sbFg);
                DeleteObject(sbBg); DeleteObject(sbFg);
            }

            HRGN clipRgn = CreateRectRgn(4, 4, w - (g_LegendMaxScroll > 0 ? 15 : 4), h - 4);
            SelectClipRgn(memDC, clipRgn);
            SetTextColor(memDC, RGB(30, 30, 30)); SetTextAlign(memDC, TA_LEFT | TA_TOP); SetBkMode(memDC, TRANSPARENT);

            HFONT hBoldFont = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
            HFONT hRegFont  = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
            HFONT hOldF = (HFONT)SelectObject(memDC, hBoldFont);

            char hdr[128]; snprintf(hdr, sizeof(hdr), "Total Distance: %.1f %s", g_TotalRouteDist, g_UseMiles ? "mi" : "km");
            TextOutA(memDC, 10, 10 + (int)g_LegendScrollY, hdr, strlen(hdr));

            SelectObject(memDC, hRegFont);
            float text_y = 34.0f + g_LegendScrollY;
            for (int step = 0; step < g_RouteStepCount; step++) {
                if (text_y > -20.0f && text_y < h) {
                    char step_str[300]; snprintf(step_str, sizeof(step_str), "%d. %s (%.1f %s)", step + 1, g_RouteSteps[step].text, g_RouteSteps[step].dist, g_UseMiles ? "mi" : "km");
                    TextOutA(memDC, 10, (int)text_y, step_str, strlen(step_str));
                }
                text_y += 20.0f;
            }

            SelectObject(memDC, hOldF); DeleteObject(hBoldFont); DeleteObject(hRegFont);
            SelectClipRgn(memDC, NULL); DeleteObject(clipRgn); DeleteObject(legBrush); DeleteObject(legPen);
            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM); DeleteObject(memBM); DeleteDC(memDC);
            EndPaint(hwnd, &ps); return 0;
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void ComputeTopHighway(void) {
    typedef struct { char name[128]; uint32_t connections; } HwyStat;
    HwyStat stats[512]; int stat_cnt = 0;
    int best_idx = -1; uint32_t best_conn = 0;

    EnterCriticalSection(&g_FileCS);
    for (uint32_t c = 0; c < g_ChunkCount; c++) {
        if (!g_Chunks[c].is_overview && g_HasOverview) continue;
        uint8_t* data = GetChunkData(c); if (!data) continue;
        uint32_t f_cnt = *(uint32_t*)data, p_cnt = *(uint32_t*)(data + 4);
        PFeat* feats = (PFeat*)(data + 12);
        char* strings = (char*)((uint8_t*)feats + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float));
        for (uint32_t j = 0; j < f_cnt; j++) {
            if (feats[j].fc == CLASS_HWY_MAIN && feats[j].name_off) {
                const char* nm = strings + feats[j].name_off;
                if (!nm[0]) continue;
                int idx = -1;
                for (int k = 0; k < stat_cnt; k++) {
                    if (strcmp(stats[k].name, nm) == 0) { idx = k; break; }
                }
                if (idx == -1 && stat_cnt < 512) {
                    idx = stat_cnt++;
                    snprintf(stats[idx].name, sizeof(stats[idx].name), "%s", nm);
                    stats[idx].connections = 0;
                }
                if (idx != -1) {
                    stats[idx].connections += feats[j].pt_cnt;
                    if (stats[idx].connections > best_conn) {
                        best_conn = stats[idx].connections;
                        best_idx = idx;
                    }
                }
            }
        }
    }
    if (best_idx != -1) {
        snprintf(g_TopHighwayName, sizeof(g_TopHighwayName), "%s", stats[best_idx].name);
    }
    LeaveCriticalSection(&g_FileCS);
}

static void LoadQJMData(const char* filepath) {
    char path[MAX_PATH] = {0};
    if (filepath[0] == '"') {
        size_t len = strlen(filepath);
        if (len >= 2) {
            size_t copy_len = len - 2;
            if (copy_len >= sizeof(path)) copy_len = sizeof(path) - 1;
            memcpy(path, filepath + 1, copy_len);
            path[copy_len] = '\0';
        }
    } else {
        snprintf(path, sizeof(path), "%s", filepath);
    }
    if (!path[0]) return;

    EnterCriticalSection(&g_FileCS);
    for (int i = 0; i < g_LoadedFileCount; i++) {
        if (strcasecmp(g_LoadedFiles[i].path, path) == 0) {
            LeaveCriticalSection(&g_FileCS);
            return;
        }
    }
    LeaveCriticalSection(&g_FileCS);

    FILE* file = fopen(path, "rb"); if (!file) return;

    bool is_world = (g_WorldMapFile[0] && strcasecmp(path, g_WorldMapFile) == 0);
    if (is_world) g_WorldMapPtr = file;

    if (!g_PrimaryMapFile[0]) {
        snprintf(g_PrimaryMapFile, MAX_PATH, "%s", path);
    }

    char magic[4]; if (fread(magic, 1, 4, file) != 4) { fclose(file); return; }
    FSEEK64(file, -28, SEEK_END);
    uint64_t high_dir, over_dir; uint32_t high_cnt, over_cnt; char tail_magic[5] = {0};
    fread(&high_dir, 8, 1, file); fread(&high_cnt, 4, 1, file); fread(&over_dir, 8, 1, file); fread(&over_cnt, 4, 1, file); fread(tail_magic, 1, 4, file);

    ChunkMetaDisk* h_meta = NULL;
    ChunkMetaDisk* o_meta = NULL;
    ChunkMetaDisk* metas = NULL;
    bool is_qjm2 = (strncmp(tail_magic, "QJM2", 4) == 0);

    if (is_qjm2) {
        if (high_cnt > 0) {
            h_meta = malloc(high_cnt * sizeof(ChunkMetaDisk));
            FSEEK64(file, high_dir, SEEK_SET);
            fread(h_meta, sizeof(ChunkMetaDisk), high_cnt, file);
        }
        if (over_cnt > 0) {
            o_meta = malloc(over_cnt * sizeof(ChunkMetaDisk));
            FSEEK64(file, over_dir, SEEK_SET);
            fread(o_meta, sizeof(ChunkMetaDisk), over_cnt, file);
        }
    } else {
        FSEEK64(file, -8, SEEK_END); uint64_t dir_offset; fread(&dir_offset, 8, 1, file);
        FSEEK64(file, dir_offset, SEEK_SET); uint32_t chunk_idx; fread(&chunk_idx, 4, 1, file);
        high_cnt = chunk_idx;
        metas = malloc(chunk_idx * sizeof(ChunkMetaDisk));
        fread(metas, sizeof(ChunkMetaDisk), chunk_idx, file);
    }

    EnterCriticalSection(&g_FileCS);
    int file_id = g_LoadedFileCount < 64 ? g_LoadedFileCount++ : 63;
    snprintf(g_LoadedFiles[file_id].path, MAX_PATH, "%s", path);
    g_LoadedFiles[file_id].file = file;
    g_LoadedFiles[file_id].is_world = is_world;
    g_LoadedFiles[file_id].is_qjm2 = is_qjm2;

    uint32_t start_c = g_ChunkCount;

    if (is_qjm2) {
        if (is_world) {
            uint32_t use_cnt = (high_cnt > 0) ? high_cnt : over_cnt;
            ChunkMetaDisk* use_meta = (high_cnt > 0) ? h_meta : o_meta;
            g_ChunkCount += use_cnt;
            g_Chunks = realloc(g_Chunks, g_ChunkCount * sizeof(MapChunk));
            for (uint32_t i = 0; i < use_cnt; i++) {
                g_Chunks[start_c + i].min_lon = use_meta[i].min_lon; g_Chunks[start_c + i].max_lon = use_meta[i].max_lon;
                g_Chunks[start_c + i].min_lat = use_meta[i].min_lat; g_Chunks[start_c + i].max_lat = use_meta[i].max_lat;
                g_Chunks[start_c + i].offset = use_meta[i].offset; g_Chunks[start_c + i].comp_size = use_meta[i].comp_size; g_Chunks[start_c + i].uncomp_size = use_meta[i].uncomp_size;
                g_Chunks[start_c + i].file = file; g_Chunks[start_c + i].is_overview = false;
                g_Chunks[start_c + i].file_id = file_id;
            }
        } else {
            if (over_cnt > 0) g_HasOverview = true;
            g_ChunkCount += (high_cnt + over_cnt);
            g_Chunks = realloc(g_Chunks, g_ChunkCount * sizeof(MapChunk));

            for (uint32_t i = 0; i < high_cnt; i++) {
                g_Chunks[start_c + i].min_lon = h_meta[i].min_lon; g_Chunks[start_c + i].max_lon = h_meta[i].max_lon;
                g_Chunks[start_c + i].min_lat = h_meta[i].min_lat; g_Chunks[start_c + i].max_lat = h_meta[i].max_lat;
                g_Chunks[start_c + i].offset = h_meta[i].offset; g_Chunks[start_c + i].comp_size = h_meta[i].comp_size; g_Chunks[start_c + i].uncomp_size = h_meta[i].uncomp_size;
                g_Chunks[start_c + i].file = file; g_Chunks[start_c + i].is_overview = false;
                g_Chunks[start_c + i].file_id = file_id;
            }
            for (uint32_t i = 0; i < over_cnt; i++) {
                g_Chunks[start_c + high_cnt + i].min_lon = o_meta[i].min_lon; g_Chunks[start_c + high_cnt + i].max_lon = o_meta[i].max_lon;
                g_Chunks[start_c + high_cnt + i].min_lat = o_meta[i].min_lat; g_Chunks[start_c + high_cnt + i].max_lat = o_meta[i].max_lat;
                g_Chunks[start_c + high_cnt + i].offset = o_meta[i].offset; g_Chunks[start_c + high_cnt + i].comp_size = o_meta[i].comp_size; g_Chunks[start_c + high_cnt + i].uncomp_size = o_meta[i].uncomp_size;
                g_Chunks[start_c + high_cnt + i].file = file; g_Chunks[start_c + high_cnt + i].is_overview = true;
                g_Chunks[start_c + high_cnt + i].file_id = file_id;
            }
        }
        if (h_meta) free(h_meta);
        if (o_meta) free(o_meta);
    } else {
        g_ChunkCount += high_cnt;
        g_Chunks = realloc(g_Chunks, g_ChunkCount * sizeof(MapChunk));
        for (uint32_t i = 0; i < high_cnt; i++) {
            g_Chunks[start_c + i].min_lon = metas[i].min_lon; g_Chunks[start_c + i].max_lon = metas[i].max_lon;
            g_Chunks[start_c + i].min_lat = metas[i].min_lat; g_Chunks[start_c + i].max_lat = metas[i].max_lat;
            g_Chunks[start_c + i].offset = metas[i].offset; g_Chunks[start_c + i].comp_size = metas[i].comp_size; g_Chunks[start_c + i].uncomp_size = metas[i].uncomp_size;
            g_Chunks[start_c + i].file = file; g_Chunks[start_c + i].is_overview = false;
            g_Chunks[start_c + i].file_id = file_id;
        }
        if (metas) free(metas);
    }

    g_DirtyBlobs = (uint8_t**)realloc(g_DirtyBlobs, g_ChunkCount * sizeof(uint8_t*));
    g_DirtySizes = (uint32_t*)realloc(g_DirtySizes, g_ChunkCount * sizeof(uint32_t));
    for (uint32_t i = start_c; i < g_ChunkCount; i++) {
        g_DirtyBlobs[i] = NULL;
        g_DirtySizes[i] = 0;
    }

    // Keep g_FileCS locked while reading chunk buffers so UI thread cannot evict active LRU slots mid-read
    for (uint32_t i = start_c; i < g_ChunkCount; i++) {
        if (g_Chunks[i].is_overview && !is_world) continue;
        uint8_t* data = GetChunkData(i); if (!data) continue;
        uint32_t f_cnt = *(uint32_t*)data, p_cnt = *(uint32_t*)(data+4);
        PFeat* feats = (PFeat*)(data + 12); char* strings = (char*)((uint8_t*)feats + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float));

        for (uint32_t j = 0; j < f_cnt; j++) {
            if (feats[j].name_off || feats[j].pcode_off) {
                if (g_SearchItemCount >= g_SearchItemCap) { g_SearchItemCap = g_SearchItemCap == 0 ? 10000 : g_SearchItemCap * 2; g_SearchItems = realloc(g_SearchItems, g_SearchItemCap * sizeof(SearchItem)); }
                g_SearchItems[g_SearchItemCount].name = feats[j].name_off ? strcpy(malloc(strlen(strings + feats[j].name_off) + 1), strings + feats[j].name_off) : NULL;
                g_SearchItems[g_SearchItemCount].pcode = feats[j].pcode_off ? strcpy(malloc(strlen(strings + feats[j].pcode_off) + 1), strings + feats[j].pcode_off) : NULL;
                g_SearchItems[g_SearchItemCount].chunk_idx = i; g_SearchItems[g_SearchItemCount].feat_idx = j;
                g_SearchItems[g_SearchItemCount].lon = (feats[j].min_lon + feats[j].max_lon)/2.0; g_SearchItems[g_SearchItemCount].lat = (feats[j].min_lat + feats[j].max_lat)/2.0; g_SearchItemCount++;
            }
        }
    }
    LeaveCriticalSection(&g_FileCS);

    ComputeTopHighway();
}
static void CalculateRouteForPins(
    const MapPin* pins, int pin_count, int route_mode,
    MapPoint** out_path, int* out_path_cnt,
    RouteStep** out_steps, int* out_step_cnt,
    double* out_total_dist, bool* out_use_miles)
{
    *out_path = NULL; *out_path_cnt = 0;
    *out_steps = NULL; *out_step_cnt = 0;
    *out_total_dist = 0.0;
    char sys_measure[2] = "0"; GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_IMEASURE, sys_measure, 2);
    *out_use_miles = (sys_measure[0] == '1');
    if (pin_count < 2) return;

    EnterCriticalSection(&g_FileCS);

    typedef struct { uint32_t c, f, p; float lon, lat; int parent; float g, f_score; uint8_t fc; bool closed; } RNode;
    typedef struct { uint32_t id; float f; } HeapNode;

    for (int p_idx = 0; p_idx < pin_count - 1; p_idx++) {
        const MapPin* pA = &pins[p_idx]; const MapPin* pB = &pins[p_idx+1];
        
        uint32_t rn_cap = 50000;
        RNode* rnodes = malloc(rn_cap * sizeof(RNode));
        if (!rnodes) continue;
        uint32_t rn_cnt = 0;
        bool mem_fail = false;

        double leg_km = Haversine(pA->lat, pA->lon, pB->lat, pB->lon);
        double pad_deg = (leg_km > 500.0) ? 1.5 : 0.25;
        double bb_min_lon = fmin(pA->lon, pB->lon) - pad_deg;
        double bb_max_lon = fmax(pA->lon, pB->lon) + pad_deg;
        double bb_min_lat = fmin(pA->lat, pB->lat) - pad_deg;
        double bb_max_lat = fmax(pA->lat, pB->lat) + pad_deg;
        bool long_route = (leg_km > 350.0);

        for (uint32_t c = 0; c < g_ChunkCount; c++) {
            MapChunk* mc = &g_Chunks[c]; if (mc->is_overview) continue;
            if (mc->max_lon < bb_min_lon || mc->min_lon > bb_max_lon || mc->max_lat < bb_min_lat || mc->min_lat > bb_max_lat) continue;
            uint8_t* data = GetChunkData(c); if (!data) continue;
            uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12);
            float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
            
            for (uint32_t f = 0; f < f_cnt; f++) {
                PFeat* feat = &feats[f];
                if (feat->fc != CLASS_HWY_MAIN && feat->fc != CLASS_HWY_MINOR) continue;
                if (long_route && feat->fc == CLASS_HWY_MINOR) {
                    bool nearA = (pA->lon >= feat->min_lon - 0.2 && pA->lon <= feat->max_lon + 0.2 &&
                                  pA->lat >= feat->min_lat - 0.2 && pA->lat <= feat->max_lat + 0.2);
                    bool nearB = (pB->lon >= feat->min_lon - 0.2 && pB->lon <= feat->max_lon + 0.2 &&
                                  pB->lat >= feat->min_lat - 0.2 && pB->lat <= feat->max_lat + 0.2);
                    if (!nearA && !nearB) continue;
                }
                if (feat->max_lon < bb_min_lon || feat->min_lon > bb_max_lon || feat->max_lat < bb_min_lat || feat->min_lat > bb_max_lat) continue;
                
                for (uint32_t p = 0; p < feat->pt_cnt; p++) {
                    if (rn_cnt >= rn_cap) { 
                        rn_cap *= 2; 
                        RNode* temp = realloc(rnodes, rn_cap * sizeof(RNode)); 
                        if (!temp) { mem_fail = true; break; }
                        rnodes = temp;
                    }
                    rnodes[rn_cnt].c = c; rnodes[rn_cnt].f = f; rnodes[rn_cnt].p = p;
                    rnodes[rn_cnt].lon = points[feat->pt_off + p*2];
                    rnodes[rn_cnt].lat = points[feat->pt_off + p*2 + 1];
                    rnodes[rn_cnt].parent = -1; rnodes[rn_cnt].g = 1e30f; rnodes[rn_cnt].f_score = 1e30f;
                    rnodes[rn_cnt].fc = feat->fc; rnodes[rn_cnt].closed = false;
                    rn_cnt++;
                }
                if (mem_fail) break;
            }
            if (mem_fail) break;
        }

        if (rn_cnt == 0 || mem_fail) { free(rnodes); continue; }

        SNode* snodes = malloc(rn_cnt * sizeof(SNode));
        if (!snodes) { free(rnodes); continue; }
        for(uint32_t i=0; i<rn_cnt; i++) { snodes[i].id = i; snodes[i].lon = rnodes[i].lon; snodes[i].lat = rnodes[i].lat; }
        qsort(snodes, rn_cnt, sizeof(SNode), cmp_snode);

        int start_id = -1, end_id = -1;
        float min_d_start = 1e30f, min_d_end = 1e30f;
        int start_main_id = -1, end_main_id = -1;
        float min_d_start_main = 1e30f, min_d_end_main = 1e30f;

        for(uint32_t i=0; i<rn_cnt; i++) {
            float ds = (rnodes[i].lon - pA->lon)*(rnodes[i].lon - pA->lon) + (rnodes[i].lat - pA->lat)*(rnodes[i].lat - pA->lat);
            if(ds < min_d_start) { min_d_start = ds; start_id = i; }
            float de = (rnodes[i].lon - pB->lon)*(rnodes[i].lon - pB->lon) + (rnodes[i].lat - pB->lat)*(rnodes[i].lat - pB->lat);
            if(de < min_d_end) { min_d_end = de; end_id = i; }

            if (rnodes[i].fc == CLASS_HWY_MAIN) {
                if (ds < min_d_start_main) { min_d_start_main = ds; start_main_id = i; }
                if (de < min_d_end_main) { min_d_end_main = de; end_main_id = i; }
            }
        }

        // Fix: The previous squared degree threshold (0.01f) was pulling perfectly placed local pins
        // onto distant main roads up to ~11km away due to an aggressive fallback heuristic.
        // We now use real Haversine distance and only aggressively snap to a main highway on 
        // long cross-country routes, or if the main highway is extremely close (< 50m) locally.
        if (start_main_id != -1) {
            float dist_start_main = Haversine(pA->lat, pA->lon, rnodes[start_main_id].lat, rnodes[start_main_id].lon);
            if ((long_route && dist_start_main < 5.0f) || (!long_route && dist_start_main < 0.05f)) {
                start_id = start_main_id;
            }
        }
        if (end_main_id != -1) {
            float dist_end_main = Haversine(pB->lat, pB->lon, rnodes[end_main_id].lat, rnodes[end_main_id].lon);
            if ((long_route && dist_end_main < 5.0f) || (!long_route && dist_end_main < 0.05f)) {
                end_id = end_main_id;
            }
        }

        if (start_id == -1 || end_id == -1) { free(snodes); free(rnodes); continue; }

        float heur_weight = (route_mode == 0) ? 0.22f : 1.0f;
        rnodes[start_id].g = 0;
        rnodes[start_id].f_score = Haversine(rnodes[start_id].lat, rnodes[start_id].lon, pB->lat, pB->lon) * heur_weight;

        uint32_t heap_cap = rn_cnt > 10000000 ? 10000000 : (rn_cnt * 2 < 1000 ? 1000 : rn_cnt * 2);
        HeapNode* heap = malloc(heap_cap * sizeof(HeapNode));
        if (!heap) { free(snodes); free(rnodes); continue; }
        uint32_t heap_size = 0;
        
        heap[heap_size].id = start_id; heap[heap_size].f = rnodes[start_id].f_score; heap_size++;

        while (heap_size > 0 && !mem_fail) {
            uint32_t u = heap[0].id;
            HeapNode temp = heap[--heap_size];
            uint32_t idx = 0;
            while (idx * 2 + 1 < heap_size) {
                uint32_t child = idx * 2 + 1;
                if (child + 1 < heap_size && heap[child + 1].f < heap[child].f) child++;
                if (heap[child].f >= temp.f) break;
                heap[idx] = heap[child];
                idx = child;
            }
            heap[idx] = temp;

            if ((int)u == end_id) break;
            if (rnodes[u].closed) continue;
            rnodes[u].closed = true;

            uint32_t neighbors[512];
            float costs[512];
            uint32_t n_cnt = 0;
            float road_mult = (route_mode == 0) ? ((rnodes[u].fc == CLASS_HWY_MAIN) ? 0.22f : 1.80f) : 1.0f;

            if (rnodes[u].p > 0 && u > 0 && rnodes[u-1].c == rnodes[u].c && rnodes[u-1].f == rnodes[u].f) {
                float d = Haversine(rnodes[u].lat, rnodes[u].lon, rnodes[u-1].lat, rnodes[u-1].lon);
                if (d < 35.0f) {
                    neighbors[n_cnt] = u - 1;
                    costs[n_cnt] = d * road_mult;
                    n_cnt++;
                }
            }
            if (u + 1 < rn_cnt && rnodes[u+1].c == rnodes[u].c && rnodes[u+1].f == rnodes[u].f) {
                float d = Haversine(rnodes[u].lat, rnodes[u].lon, rnodes[u+1].lat, rnodes[u+1].lon);
                if (d < 35.0f) {
                    neighbors[n_cnt] = u + 1;
                    costs[n_cnt] = d * road_mult;
                    n_cnt++;
                }
            }

            const float eps = 1e-3f;
            float target_lon = rnodes[u].lon - eps;
            int lo = 0, hi = (int)rn_cnt;
            while (lo < hi) {
                int mid = lo + (hi - lo) / 2;
                if (snodes[mid].lon < target_lon) lo = mid + 1;
                else hi = mid;
            }
            for (int i = lo; i < (int)rn_cnt; i++) {
                if (snodes[i].lon > rnodes[u].lon + eps) break;
                if (snodes[i].id != u && fabsf(snodes[i].lat - rnodes[u].lat) <= eps) {
                    if (n_cnt < 512) {
                        float jd = Haversine(rnodes[u].lat, rnodes[u].lon, snodes[i].lat, snodes[i].lon);
                        neighbors[n_cnt] = snodes[i].id;
                        costs[n_cnt] = jd * road_mult;
                        n_cnt++;
                    }
                }
            }

            for (uint32_t i = 0; i < n_cnt; i++) {
                uint32_t v = neighbors[i];
                if (rnodes[v].closed) continue;
                float tentative_g = rnodes[u].g + costs[i];
                if (tentative_g < rnodes[v].g) {
                    rnodes[v].parent = u;
                    rnodes[v].g = tentative_g;
                    rnodes[v].f_score = tentative_g + Haversine(rnodes[v].lat, rnodes[v].lon, rnodes[end_id].lat, rnodes[end_id].lon) * heur_weight;
                    
                    if (heap_size >= heap_cap) {
                        heap_cap *= 2;
                        HeapNode* t_heap = realloc(heap, heap_cap * sizeof(HeapNode));
                        if (!t_heap) { mem_fail = true; break; }
                        heap = t_heap;
                    }
                    
                    uint32_t i_push = heap_size++;
                    while (i_push > 0) {
                        uint32_t parent = (i_push - 1) / 2;
                        if (heap[parent].f <= rnodes[v].f_score) break;
                        heap[i_push] = heap[parent];
                        i_push = parent;
                    }
                    heap[i_push].id = v; heap[i_push].f = rnodes[v].f_score;
                }
            }
        }

        if (!mem_fail && (rnodes[end_id].parent != -1 || start_id == end_id)) {
            int path_cap = 1000;
            int* path_ids = malloc(path_cap * sizeof(int));
            if (path_ids) {
                int path_len = 0;
                int curr = end_id;
                while(curr != -1) {
                    if (path_len >= path_cap) { 
                        path_cap *= 2; 
                        int* t_p = realloc(path_ids, path_cap * sizeof(int)); 
                        if (!t_p) { mem_fail = true; break; }
                        path_ids = t_p;
                    }
                    path_ids[path_len++] = curr;
                    curr = rnodes[curr].parent;
                }
                
                if (!mem_fail) {
                    char last_road[128] = "";
                    double accum_dist = 0;
                    
                    for(int i = path_len - 1; i >= 0; i--) {
                        uint32_t u = path_ids[i];
                        MapPoint* t_path = realloc(*out_path, (*out_path_cnt + 1) * sizeof(MapPoint));
                        if (!t_path) break;
                        *out_path = t_path;
                        (*out_path)[*out_path_cnt].lon = rnodes[u].lon;
                        (*out_path)[*out_path_cnt].lat = rnodes[u].lat;
                        (*out_path_cnt)++;

                        if (i < path_len - 1) { 
                            double d = Haversine(rnodes[u].lat, rnodes[u].lon, rnodes[path_ids[i+1]].lat, rnodes[path_ids[i+1]].lon);
                            accum_dist += d; *out_total_dist += (*out_use_miles ? (d * 0.621371) : d);
                            
                            char curr_road[128] = "Unnamed Road";
                            uint8_t* data = GetChunkData(rnodes[u].c);
                            if (data) {
                                PFeat* feats = (PFeat*)(data + 12);
                                if (feats[rnodes[u].f].name_off) {
                                    uint32_t f_cnt = *(uint32_t*)data; uint32_t p_cnt = *(uint32_t*)(data+4);
                                    char* strings = (char*)((uint8_t*)feats + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float));
                                    strncpy(curr_road, strings + feats[rnodes[u].f].name_off, 127); curr_road[127] = '\0';
                                } else { strcpy(curr_road, feats[rnodes[u].f].fc == CLASS_HWY_MAIN ? "Main Highway" : "Local Road"); }
                            }
                            
                            if (strcmp(curr_road, last_road) != 0 && strlen(last_road) > 0) {
                                RouteStep* t_step = realloc(*out_steps, (*out_step_cnt + 1) * sizeof(RouteStep));
                                if (t_step) {
                                    *out_steps = t_step;
                                    snprintf((*out_steps)[*out_step_cnt].text, 256, "Take %.128s", last_road);
                                    (*out_steps)[*out_step_cnt].dist = accum_dist * (*out_use_miles ? 0.621371 : 1.0);
                                    (*out_step_cnt)++; accum_dist = 0;
                                }
                            } strcpy(last_road, curr_road);
                        }
                    }
                    if (accum_dist > 0 && strlen(last_road) > 0) {
                        RouteStep* t_step = realloc(*out_steps, (*out_step_cnt + 1) * sizeof(RouteStep));
                        if (t_step) {
                            *out_steps = t_step;
                            snprintf((*out_steps)[*out_step_cnt].text, 256, "Take %.128s", last_road);
                            (*out_steps)[*out_step_cnt].dist = accum_dist * (*out_use_miles ? 0.621371 : 1.0);
                            (*out_step_cnt)++;
                        }
                    } 
                }
                free(path_ids);
            }
        }
        
        free(heap); free(snodes); free(rnodes);
    }
    LeaveCriticalSection(&g_FileCS);
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
        double last_x = 0; 
        double last_y = 0;
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
                if (slon < mc->min_lon - 0.02 || slon > mc->max_lon + 0.02 || slat < mc->min_lat - 0.02 || slat > mc->max_lat + 0.02) {
                    continue;
                }

                uint8_t* data = GetChunkData(c);
                if (!data) continue;
                
                uint32_t f_cnt = *(uint32_t*)data;
                PFeat* feats = (PFeat*)(data + 12);
                float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                char* strings = (char*)((uint8_t*)points + (*(uint32_t*)(data+4)) * sizeof(float));
                
                for (uint32_t i = 0; i < f_cnt; i++) {
                    PFeat* f = &feats[i];
                    if (f->fc != CLASS_HWY_MAIN && f->fc != CLASS_HWY_MINOR) continue;
                    if (slon < f->min_lon - 0.02 || slon > f->max_lon + 0.02 || slat < f->min_lat - 0.02 || slat > f->max_lat + 0.02) continue;

                    for (uint32_t j = 0; j < f->pt_cnt; j++) {
                        double px = points[f->pt_off + j*2];
                        double py = points[f->pt_off + j*2 + 1];
                        double ddx = px - slon;
                        double ddy = py - slat;
                        double d = ddx*ddx + ddy*ddy;
                        
                        if (g_RouteMode == 0 && f->fc == CLASS_HWY_MAIN) d *= 0.005;
                        
                        if (last_chunk != -1) {
                            if (c == (uint32_t)last_chunk && i == (uint32_t)last_feat) {
                                d *= 0.0001; 
                            } else {
                                double tdx = px - last_x;
                                double tdy = py - last_y;
                                if (tdx*tdx + tdy*tdy < 0.00000001) {
                                    d *= 0.0001; 
                                } else if (f->name_off && strlen(last_road) > 0 && strcmp(strings + f->name_off, last_road) == 0) {
                                    d *= 0.001; 
                                }
                            }
                        } else if (f->name_off && strlen(last_road) > 0 && strcmp(strings + f->name_off, last_road) == 0) {
                            d *= 0.001; 
                        }

                        if (d < min_d) {
                            min_d = d;
                            best_chunk = c;
                            best_feat = i;
                            best_pt = j;
                            if (f->name_off) {
                                strcpy(best_road, strings + f->name_off);
                            } else {
                                strcpy(best_road, (f->fc == CLASS_HWY_MAIN) ? "Main Highway" : "Local Road");
                            }
                        }
                    }
                }
            }

            if (best_chunk != -1) {
                uint8_t* b_data = GetChunkData(best_chunk);
                PFeat* b_feats = (PFeat*)(b_data + 12);
                float* b_points = (float*)(b_data + 12 + (*(uint32_t*)b_data) * sizeof(PFeat));
                PFeat* b_f = &b_feats[best_feat];

                if (best_chunk == last_chunk && best_feat == last_feat && best_pt != last_pt && last_pt != -1) {
                    int step_dir = (best_pt > last_pt) ? 1 : -1;
                    for (int k = last_pt + step_dir; k != best_pt; k += step_dir) {
                        g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
                        g_RoutePath[g_RoutePathCount].lon = b_points[b_f->pt_off + k*2];
                        g_RoutePath[g_RoutePathCount].lat = b_points[b_f->pt_off + k*2 + 1];
                        g_RoutePathCount++;
                    }
                }
                
                if (best_chunk != last_chunk || best_feat != last_feat || best_pt != last_pt) {
                    g_RoutePath = realloc(g_RoutePath, (g_RoutePathCount + 1) * sizeof(MapPoint));
                    g_RoutePath[g_RoutePathCount].lon = b_points[b_f->pt_off + best_pt*2];
                    g_RoutePath[g_RoutePathCount].lat = b_points[b_f->pt_off + best_pt*2 + 1];
                    g_RoutePathCount++;
                }
                last_chunk = best_chunk;
                last_feat = best_feat;
                last_pt = best_pt;
                last_x = b_points[b_f->pt_off + best_pt*2];
                last_y = b_points[b_f->pt_off + best_pt*2 + 1];
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

static DWORD WINAPI BackgroundLoadMapsThread(LPVOID lpParam) {
    LoadThreadArgs* args = (LoadThreadArgs*)lpParam;
    if (args->maps_str[0]) {
        char* token = strtok(args->maps_str, "|");
        while (token) {
            char* ext = strrchr(token, '.');
            if (ext && (strcasecmp(ext, ".qjm") == 0 || strcasecmp(ext, ".qjmc") == 0)) {
                LoadQJMData(token);
            }
            token = strtok(NULL, "|");
        }
    }
    free(args);
    if (g_hMainWnd) {
        g_IsLoading = false;
        g_CanvasDirty = true;
        InvalidateRect(g_hMainWnd, NULL, FALSE);
        
        // Execute pending CLI arguments once maps are successfully loaded
        if (g_PendingCliCommand[0] != '\0') {
            PostMessage(g_hMainWnd, WM_APP + 1, 0, 0);
        }
    }
    return 0;
}

static void FinishPinEdit(HWND hwndParent) {
    if (!hEditPin) return;
    HWND hEdit = hEditPin;
    hEditPin = NULL;
    int editIdx = g_EditingPinIndex;
    g_EditingPinIndex = -1;

    char buf[128] = {0};
    GetWindowTextA(hEdit, buf, sizeof(buf));
    DestroyWindow(hEdit);

    if (editIdx >= 0 && editIdx < g_PinCount) {
        if (strlen(buf) == 0) {
            int sel_idx = -1;
            for (int k = 0; k < g_SelectedPinCount; k++) {
                if (g_SelectedPins[k] == editIdx) { sel_idx = k; break; }
            }
            if (sel_idx >= 0) {
                for (int k = sel_idx; k < g_SelectedPinCount - 1; k++) g_SelectedPins[k] = g_SelectedPins[k + 1];
                g_SelectedPinCount--;
            }
            for (int k = 0; k < g_SelectedPinCount; k++) {
                if (g_SelectedPins[k] > editIdx) g_SelectedPins[k]--;
            }
            for (int i = editIdx; i < g_PinCount - 1; i++) g_Pins[i] = g_Pins[i + 1];
            g_PinCount--;
            CalculateRoute();
        } else {
            snprintf(g_Pins[editIdx].name, sizeof(g_Pins[editIdx].name), "%s", buf);
        }
    }
    if (hwndParent) {
        UpdateLegendWindow(hwndParent);
        SetFocus(hwndParent);
        InvalidateRect(hwndParent, NULL, FALSE);
        UpdateWindow(hwndParent);
    }
}

static void PopulateSearchList(HWND hwnd) {
    char query[128]; GetWindowTextA(hTxtSearch, query, 128); SendMessage(hListSearch, LB_RESETCONTENT, 0, 0);
    if (strlen(query) < 2) { ShowWindow(hListSearch, SW_HIDE); return; }
    
    char query_pc[128];
    int qp_idx = 0;
    for (int i = 0; query[i]; i++) {
        if (query[i] != ' ') query_pc[qp_idx++] = query[i];
    }
    query_pc[qp_idx] = '\0';

    int count = 0;
    for (uint32_t i = 0; i < g_SearchItemCount && count < 50; i++) {
        SearchItem* item = &g_SearchItems[i]; bool match = false;
        if (g_SearchRoads && item->name && FindSubStringIC(item->name, query)) match = true;
        if (!match && g_SearchPostcodes && item->pcode) {
            char item_pc[128];
            int idx = 0;
            for (int j = 0; item->pcode[j]; j++) {
                if (item->pcode[j] != ' ') item_pc[idx++] = item->pcode[j];
            }
            item_pc[idx] = '\0';
            if (FindSubStringIC(item_pc, query_pc)) match = true;
        }
        if (match) {
            char display[512]; snprintf(display, 512, "%s %s", item->name ? item->name : "", item->pcode ? item->pcode : "");
            int idx = SendMessageA(hListSearch, LB_ADDSTRING, 0, (LPARAM)display); SendMessageA(hListSearch, LB_SETITEMDATA, idx, (LPARAM)i); count++;
        }
    }
    if (count > 0) {
        RECT r; GetWindowRect(hTxtSearch, &r); POINT pt = {r.left, r.bottom}; ScreenToClient(hwnd, &pt);
        SetWindowPos(hListSearch, HWND_TOP, pt.x, pt.y, 230, 150, SWP_SHOWWINDOW);
    } else ShowWindow(hListSearch, SW_HIDE);
}

static void UpdateCanvas(HWND hwnd) {
    RenderCanvasRegion(hwnd, NULL, NULL, !g_InteractiveCanvas);
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
        FinishPinEdit(GetParent(hwnd));
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        HWND parent = GetParent(hwnd);
        FinishPinEdit(parent);
        SendMessage(parent, WM_MOUSEWHEEL, wParam, lParam);
        return 0;
    }
    if (msg == WM_RBUTTONDOWN) {
        FinishPinEdit(GetParent(hwnd));
        return 0;
    }
    if (msg == WM_KILLFOCUS) {
        if (hEditPin) FinishPinEdit(GetParent(hwnd));
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
            
            hTxtSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 180, 25, hwnd, (HMENU)1004, NULL, NULL);
            hBtnSearchOpts = CreateWindowA("BUTTON", "..", WS_CHILD | WS_VISIBLE, 195, 10, 25, 25, hwnd, (HMENU)1005, NULL, NULL);
            
            hListSearch = CreateWindowExA(WS_EX_TOPMOST, "LISTBOX", "", WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, hwnd, (HMENU)1003, NULL, NULL);
            hBtnCancel = CreateWindowA("BUTTON", "Cancel Route", WS_CHILD | WS_VISIBLE, 230, 10, 90, 25, hwnd, (HMENU)4, NULL, NULL);
            hCboRouteType = CreateWindowA("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VISIBLE | WS_CHILD, 325, 12, 110, 100, hwnd, (HMENU)5, NULL, NULL);
            
            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Fastest Route");
            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Shortest Route");
            SendMessage(hCboRouteType, CB_SETCURSEL, g_RouteMode, 0);
            
            ShowWindow(hBtnCancel, SW_HIDE);
            ShowWindow(hCboRouteType, SW_HIDE);
            
            SendMessage(hTxtSearch, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnSearchOpts, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCboRouteType, WM_SETFONT, (WPARAM)hFont, TRUE);
            return 0;
        }
        case WM_HOTKEY: {
            if (wParam == 1 && hPdfDlg) {
                ShowWindow(hPdfDlg, SW_SHOW);
                SetForegroundWindow(hPdfDlg);
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
            } else if (LOWORD(wParam) == 1005) {
                HMENU hSearchMenu = CreatePopupMenu();
                AppendMenuA(hSearchMenu, MF_STRING | (g_SearchRoads ? MF_CHECKED : MF_UNCHECKED), 1001, "Road Names");
                AppendMenuA(hSearchMenu, MF_STRING | (g_SearchPostcodes ? MF_CHECKED : MF_UNCHECKED), 1002, "Postal Codes");
                RECT r; 
                GetWindowRect(hBtnSearchOpts, &r);
                TrackPopupMenu(hSearchMenu, TPM_RIGHTBUTTON, r.left, r.bottom, 0, hwnd, NULL);
                DestroyMenu(hSearchMenu);
            } else if (LOWORD(wParam) == 1001) {
                g_SearchRoads = !g_SearchRoads;
                PopulateSearchList(hwnd);
            } else if (LOWORD(wParam) == 1002) {
                g_SearchPostcodes = !g_SearchPostcodes;
                PopulateSearchList(hwnd);
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
                    
                    if (item->pcode && g_PinCount < 1000) {
                        snprintf(g_Pins[g_PinCount].name, sizeof(g_Pins[g_PinCount].name), "%s", item->pcode);
                        g_Pins[g_PinCount].lon = item->lon;
                        g_Pins[g_PinCount].lat = item->lat;
                        g_PinCount++;
                    }
                    
                    ShowWindow(hListSearch, SW_HIDE);
                    SetFocus(hwnd);
                    g_CanvasDirty = true;
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
                if (strlen(g_MapFilesStr) > 0) {
                    strcat(g_MapFilesStr, "|");
                }
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
            if (hEditPin) {
                SetFocus(hwnd);
                return 0;
            }
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            bool pin_deleted = false;
            for (int i = 0; i < g_PinCount; i++) {
                double px, py;
                LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rc.right, rc.bottom, g_PanX, g_PanY, g_Zoom, &px, &py);
                if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) {
                    int sel_idx = -1;
                    for (int k = 0; k < g_SelectedPinCount; k++) {
                        if (g_SelectedPins[k] == i) {
                            sel_idx = k;
                        }
                    }
                    if (sel_idx >= 0) {
                        for (int k = sel_idx; k < g_SelectedPinCount - 1; k++) {
                            g_SelectedPins[k] = g_SelectedPins[k+1];
                        }
                        g_SelectedPinCount--;
                    }
                    for (int k = 0; k < g_SelectedPinCount; k++) {
                        if (g_SelectedPins[k] > i) {
                            g_SelectedPins[k]--;
                        }
                    }
                    for (int k = i; k < g_PinCount - 1; k++) {
                        g_Pins[k] = g_Pins[k+1];
                    }
                    g_PinCount--;
                    pin_deleted = true;
                    if (g_SelectedPinCount >= 2) {
                        CalculateRoute();
                    } else {
                        g_RouteStepCount = 0;
                        g_RoutePathCount = 0;
                        ShowWindow(hBtnCancel, SW_HIDE);
                        ShowWindow(hCboRouteType, SW_HIDE);
                    }
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
                    if (c_lon < mc->min_lon - 0.05 || c_lon > mc->max_lon + 0.05 || c_lat < mc->min_lat - 0.05 || c_lat > mc->max_lat + 0.05) continue;
                    uint8_t* data = GetChunkData(c);
                    if (!data) continue;
                    
                    uint32_t f_cnt = *(uint32_t*)data;
                    PFeat* feats = (PFeat*)(data + 12);
                    float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                    
                    for (uint32_t i = 0; i < f_cnt; i++) {
                        PFeat* f = &feats[i];
                        if (c_lon < f->min_lon - 0.05 || c_lon > f->max_lon + 0.05 || c_lat < f->min_lat - 0.05 || c_lat > f->max_lat + 0.05) continue;
                        for (uint32_t j = 0; j < f->pt_cnt; j++) {
                            double ddx = points[f->pt_off + j*2] - c_lon;
                            double ddy = points[f->pt_off + j*2 + 1] - c_lat;
                            double d = ddx*ddx + ddy*ddy;
                            if (d < best_d) {
                                best_d = d;
                                best_chunk = c;
                                best_feat = i;
                            }
                        }
                    }
                }
                if (best_chunk != -1) {
                    uint8_t* data = GetChunkData(best_chunk);
                    PFeat* best_f = &((PFeat*)(data + 12))[best_feat];
                    double feat_w = best_f->max_lon - best_f->min_lon;
                    double feat_h = best_f->max_lat - best_f->min_lat;
                    if (feat_w < 0.0001) feat_w = 0.0001;
                    if (feat_h < 0.0001) feat_h = 0.0001;
                    
                    g_CenterLon = (best_f->min_lon + best_f->max_lon) / 2.0;
                    g_CenterLat = (best_f->min_lat + best_f->max_lat) / 2.0;
                    g_PanX = 0;
                    g_PanY = 0;
                    
                    double zoomX = rc.right / (1.2 * feat_w);
                    double zoomY = rc.bottom / (1.2 * feat_h);
                    g_Zoom = (zoomX < zoomY) ? zoomX : zoomY;
                    
                    g_HighlightedChunk = best_chunk;
                    g_HighlightedFeat = best_feat;
                    g_CanvasDirty = true;
                } else {
                    g_HighlightedChunk = -1;
                    g_HighlightedFeat = -1;
                    g_CanvasDirty = true;
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
                if (g_LegendMaxScroll > 0 && g_LegendScrollY < -g_LegendMaxScroll) {
                    g_LegendScrollY = -g_LegendMaxScroll;
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int my = (short)HIWORD(lParam);
            ScreenToClient(hwnd, &(POINT){mx, my});
            RECT rect;
            GetClientRect(hwnd, &rect);
            double zoom_factor = (delta > 0) ? 1.2 : 0.8333333;
            double rel_x = mx - (rect.right / 2.0);
            double rel_y = my - (rect.bottom / 2.0);
            g_PanX = rel_x - (rel_x - g_PanX) * zoom_factor;
            g_PanY = rel_y - (rel_y - g_PanY) * zoom_factor;
            g_Zoom *= zoom_factor;
            if (g_Zoom < 0.001) g_Zoom = 0.001; 
            g_CanvasDirty = true;
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
            g_CanvasDirty = true;
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
                    int mx = (short)LOWORD(lParam);
                    int my = (short)HIWORD(lParam);
                    RECT rect;
                    GetClientRect(hwnd, &rect);
                    for (int i = 0; i < g_PinCount; i++) {
                        double px, py;
                        LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rect.right, rect.bottom, g_PanX, g_PanY, g_Zoom, &px, &py);
                        if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) { 
                            bool already_sel = false;
                            for(int k=0; k<g_SelectedPinCount; k++) {
                                if(g_SelectedPins[k] == i) {
                                    already_sel = true;
                                }
                            }
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
            RECT rc;
            GetClientRect(hwnd, &rc);
            bool hit_pin = false;
            for (int i = 0; i < g_PinCount; i++) {
                double px, py;
                LatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rc.right, rc.bottom, g_PanX, g_PanY, g_Zoom, &px, &py);
                if (abs(mx - (int)px) < 15 && (my - (int)py) > -35 && (my - (int)py) < 5) {
                    g_EditingPinIndex = i;
                    hEditPin = CreateWindowExA(0, "EDIT", g_Pins[i].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, (int)px - 50, (int)py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                    OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
                    SetFocus(hEditPin);
                    SendMessage(hEditPin, EM_SETSEL, 0, -1);
                    InvalidateRect(hwnd, NULL, FALSE);
                    hit_pin = true;
                    break;
                }
            }
            if (!hit_pin && g_PinCount < 1000) {
                if (my < 45 || (g_SelectedPinCount >= 2 && mx < 280)) {
                    return 0;
                }
                g_EditingPinIndex = g_PinCount;
                double t_lon, t_lat;
                ScreenToLatLon(mx, my, rc.right, rc.bottom, &t_lon, &t_lat);
                double snap_dist = 999999;
                for (uint32_t c = 0; c < g_ChunkCount; c++) {
                    MapChunk* mc = &g_Chunks[c];
                    if (t_lon < mc->min_lon - 0.02 || t_lon > mc->max_lon + 0.02 || t_lat < mc->min_lat - 0.02 || t_lat > mc->max_lat + 0.02) continue;
                    uint8_t* data = GetChunkData(c);
                    if (!data) continue;
                    uint32_t f_cnt = *(uint32_t*)data;
                    PFeat* feats = (PFeat*)(data + 12);
                    float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                    for (uint32_t i = 0; i < f_cnt; i++) {
                        PFeat* f = &feats[i];
                        if (f->fc != CLASS_HWY_MAIN && f->fc != CLASS_HWY_MINOR) continue;
                        if (f->fc == CLASS_HWY_MINOR && g_Zoom < 2500.0) continue; 
                        if (t_lon < f->min_lon - 0.02 || t_lon > f->max_lon + 0.02 || t_lat < f->min_lat - 0.02 || t_lat > f->max_lat + 0.02) continue;
                        for (uint32_t j = 0; j < f->pt_cnt; j++) {
                            double ddx = points[f->pt_off + j*2] - t_lon;
                            double ddy = points[f->pt_off + j*2 + 1] - t_lat;
                            double d = ddx*ddx + ddy*ddy;
                            if (d < snap_dist) {
                                snap_dist = d;
                                g_Pins[g_PinCount].lon = points[f->pt_off + j*2];
                                g_Pins[g_PinCount].lat = points[f->pt_off + j*2 + 1];
                            }
                        }
                    }
                }
                if (snap_dist == 999999) {
                    g_Pins[g_PinCount].lon = t_lon;
                    g_Pins[g_PinCount].lat = t_lat;
                }
                snprintf(g_Pins[g_PinCount].name, 128, "%d", g_PinCount + 1);
                g_SelectedPins[g_SelectedPinCount++] = g_PinCount++;
                if (g_SelectedPinCount >= 2) CalculateRoute();
                double e_px, e_py;
                LatLonToScreen(g_Pins[g_PinCount-1].lon, g_Pins[g_PinCount-1].lat, rc.right, rc.bottom, g_PanX, g_PanY, g_Zoom, &e_px, &e_py);
                hEditPin = CreateWindowExA(0, "EDIT", g_Pins[g_PinCount-1].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, (int)e_px - 50, (int)e_py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
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
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right;
            int h = rc.bottom;
            if (g_CanvasW != w * 3 || g_CanvasH != h * 3 || g_Zoom != g_CanvasZoom || g_CanvasDirty) {
                UpdateCanvas(hwnd);
            }
            int src_x = w - (int)(g_PanX - g_CanvasPanX);
            int src_y = h - (int)(g_PanY - g_CanvasPanY);
            if (src_x < w/2 || src_y < h/2 || src_x + w > g_CanvasW - w/2 || src_y + h > g_CanvasH - h/2) {
                UpdateCanvas(hwnd);
                src_x = w;
                src_y = h;
            }
            BitBlt(hdc, 0, 0, w, h, g_hdcCanvas, src_x, src_y, SRCCOPY);
            DrawOverlay(hdc, w, h);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            UnregisterHotKey(hwnd, 1);
            if (g_SavePins) {
                FILE* ini = fopen("qjmnavig.ini", "w");
                if (ini) {
                    fprintf(ini, "panX=%.4f\npanY=%.4f\nzoom=%.4f\nrotation=%.6f\n", g_PanX, g_PanY, g_Zoom, g_Rotation);
                    RECT wrect;
                    GetWindowRect(hwnd, &wrect);
                    fprintf(ini, "winX=%d\nwinY=%d\nwinW=%d\nwinH=%d\n", (int)wrect.left, (int)wrect.top, (int)(wrect.right - wrect.left), (int)(wrect.bottom - wrect.top));
                    fprintf(ini, "pageW=%.2f\npageH=%.2f\nmargH=%.2f\nmargV=%.2f\nisLandscape=%d\n", page_w_mm, page_h_mm, g_MargH, g_MargV, isLandscape);
                    fprintf(ini, "showLand=%d\nshowWater=%d\nshowPark=%d\nshowCoast=%d\nshowHwyMin=%d\nshowHwyMain=%d\nshowLabels=%d\n", g_ShowLand, g_ShowWater, g_ShowPark, g_ShowCoast, g_ShowHwyMin, g_ShowHwyMain, g_ShowLabels);
                    fprintf(ini, "pdfLand=%d\npdfWater=%d\npdfPark=%d\npdfCoast=%d\npdfHwyMin=%d\npdfHwyMain=%d\npdfLabels=%d\n", g_PdfExpLand, g_PdfExpWater, g_PdfExpPark, g_PdfExpCoast, g_PdfExpHwyMin, g_PdfExpHwyMain, g_PdfExpLabels);
                    fprintf(ini, "showDebug=%d\nrouteMode=%d\nsearchRoads=%d\nsearchPostcodes=%d\n", g_ShowDebug, g_RouteMode, g_SearchRoads, g_SearchPostcodes);
                    
                    char hex[16];
                    ColorToHex(c_Land, hex);
                    fprintf(ini, "colLand=%s\n", hex);
                    ColorToHex(c_Water, hex);
                    fprintf(ini, "colWater=%s\n", hex);
                    ColorToHex(c_Park, hex);
                    fprintf(ini, "colPark=%s\n", hex);
                    ColorToHex(c_Coast, hex);
                    fprintf(ini, "colCoast=%s\n", hex);
                    ColorToHex(c_HwyMin, hex);
                    fprintf(ini, "colHwyMin=%s\n", hex);
                    ColorToHex(c_HwyMain, hex);
                    fprintf(ini, "colHwyMain=%s\n", hex);
                    ColorToHex(c_Pin, hex);
                    fprintf(ini, "colPin=%s\n", hex);
                    ColorToHex(c_PinSel, hex);
                    fprintf(ini, "colPinSel=%s\n", hex);
                    ColorToHex(c_Route, hex);
                    fprintf(ini, "colRoute=%s\n", hex);
                    
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
                        if (i > 0) {
                            strcat(routeStr, "|");
                        }
                        if (strlen(routeStr) + strlen(g_Pins[g_SelectedPins[i]].name) < sizeof(routeStr)) {
                            strcat(routeStr, g_Pins[g_SelectedPins[i]].name);
                        }
                    }
                    fprintf(ini, "route=%s\nmaps=%s\n", routeStr, g_MapFilesStr);
                    fclose(ini);
                }
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
static void ZoomToFeatureWithSurroundingArea(HWND hwnd, int c_idx, int f_idx) {
    if (c_idx < 0 || (uint32_t)c_idx >= g_ChunkCount || f_idx < 0) return;
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    uint8_t* data = GetChunkData(c_idx);
    if (!data) return;
    uint32_t f_cnt = *(uint32_t*)data;
    if ((uint32_t)f_idx >= f_cnt) return;

    PFeat* best_f = &((PFeat*)(data + 12))[f_idx];
    double feat_w = best_f->max_lon - best_f->min_lon;
    double feat_h = best_f->max_lat - best_f->min_lat;

    double view_lon_span = fmax(feat_w * 3.5, 0.015);
    double view_lat_span = fmax(feat_h * 3.5, 0.015);

    g_CenterLon = (best_f->min_lon + best_f->max_lon) * 0.5;
    g_CenterLat = (best_f->min_lat + best_f->max_lat) * 0.5;
    g_PanX = 0.0;
    g_PanY = 0.0;

    double zoomX = rc.right / view_lon_span;
    double zoomY = rc.bottom / view_lat_span;
    double target_zoom = (zoomX < zoomY) ? zoomX : zoomY;
    if (target_zoom > 18000.0) target_zoom = 18000.0;
    if (target_zoom < 50.0) target_zoom = 50.0;
    if (target_zoom > g_Zoom) {
        g_Zoom = target_zoom;
    }
    g_CanvasDirty = true;
}

static LRESULT CALLBACK PdfWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
            char buf[32]; sprintf(buf, "%.2f", page_w_mm); SetWindowTextA(hTxtWidth, buf);
            sprintf(buf, "%.2f", page_h_mm); SetWindowTextA(hTxtHeight, buf);
            sprintf(buf, "%.1f", g_MargH); SetWindowTextA(hTxtMargH, buf);
            sprintf(buf, "%.1f", g_MargV); SetWindowTextA(hTxtMargV, buf);
            SetWindowTextA(hBtnToggle, isLandscape ? "Land" : "Port");
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) { 
                OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = ""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = "PDF Files (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameA(&ofn)) SetWindowTextA(hTxtOut, path);
            } else if (LOWORD(wParam) == 2) { 
                isLandscape = !isLandscape; SetWindowTextA(hBtnToggle, isLandscape ? "Land" : "Port"); 
                char w_str[32], h_str[32]; GetWindowTextA(hTxtWidth, w_str, 32); GetWindowTextA(hTxtHeight, h_str, 32);
                SetWindowTextA(hTxtWidth, h_str); SetWindowTextA(hTxtHeight, w_str); page_w_mm = atof(h_str); page_h_mm = atof(w_str);
            } else if (LOWORD(wParam) == 3) { 
                char w_str[32], h_str[32]; GetWindowTextA(hTxtWidth, w_str, 32); GetWindowTextA(hTxtHeight, h_str, 32);
                page_w_mm = atof(w_str); page_h_mm = atof(h_str); CreatePDFAction(hwnd);
            } break;
        }
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    } return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK NavigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP + 1: {
            if (g_PendingCliCommand[0] != '\0') {
                ProcessInputString(hwnd, g_PendingCliCommand);
                g_PendingCliCommand[0] = '\0';
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CREATE: {
            g_hMainWnd = hwnd;
            StartHttpServer();
            DragAcceptFiles(hwnd, TRUE); HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT); RegisterHotKey(hwnd, 1, MOD_CONTROL, 'P');
            hTxtSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 190, 25, hwnd, (HMENU)1004, NULL, NULL);
            
            OldSearchEditProc = (WNDPROC)SetWindowLongPtr(hTxtSearch, GWLP_WNDPROC, (LONG_PTR)SearchEditSubclassProc);
            
            hBtnSearchOpts = CreateWindowExA(WS_EX_CLIENTEDGE, "BUTTON", "..", WS_CHILD | WS_VISIBLE, 205, 10, 35, 25, hwnd, (HMENU)1005, NULL, NULL);
            hListSearch = CreateWindowExA(WS_EX_TOPMOST, "LISTBOX", "", WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, hwnd, (HMENU)1003, NULL, NULL);
            hBtnCancel = CreateWindowExA(WS_EX_CLIENTEDGE, "BUTTON", "Cancel Route", WS_CHILD | WS_VISIBLE, 10, 45, 100, 25, hwnd, (HMENU)4, NULL, NULL);
            hCboRouteType = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VISIBLE | WS_CHILD, 115, 45, 125, 100, hwnd, (HMENU)5, NULL, NULL);
            hLegend = CreateWindowExA(0, "QjmLegendClass", "", WS_CHILD | WS_CLIPCHILDREN, 10, 80, 260, 200, hwnd, (HMENU)1007, GetModuleHandle(NULL), NULL);

            SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Fastest Route"); SendMessage(hCboRouteType, CB_ADDSTRING, 0, (LPARAM)"Shortest Route"); SendMessage(hCboRouteType, CB_SETCURSEL, g_RouteMode, 0);
            ShowWindow(hBtnCancel, SW_HIDE); ShowWindow(hCboRouteType, SW_HIDE); ShowWindow(hLegend, SW_HIDE);
            if (g_EditMode) { ShowWindow(hTxtSearch, SW_HIDE); ShowWindow(hBtnSearchOpts, SW_HIDE); }
            
            SendMessage(hTxtSearch, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hBtnSearchOpts, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hCboRouteType, WM_SETFONT, (WPARAM)hFont, TRUE); return 0;
        }
        case WM_HOTKEY: {
            if (wParam == 1 && hPdfDlg) {
                SyncPdfDialogOrientation();
                ShowWindow(hPdfDlg, SW_SHOW); SetForegroundWindow(hPdfDlg);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                PopulateSearchList(hwnd);
            } else if (wParam == 2) {
                KillTimer(hwnd, 2);
                if (g_InteractiveCanvas) {
                    g_InteractiveCanvas = false;
                    g_CanvasDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1004 && HIWORD(wParam) == EN_CHANGE) SetTimer(hwnd, 1, 500, NULL);
            else if (LOWORD(wParam) == 1005) {
                if (hEditPin) FinishPinEdit(hwnd);
                HMENU hSearchMenu = CreatePopupMenu();
                AppendMenuA(hSearchMenu, MF_STRING | (g_SearchRoads ? MF_CHECKED : MF_UNCHECKED), 1001, "Road Names");
                AppendMenuA(hSearchMenu, MF_STRING | (g_SearchPostcodes ? MF_CHECKED : MF_UNCHECKED), 1002, "Postal Codes");
                AppendMenuA(hSearchMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hSearchMenu, MF_STRING, 1006, "Export PDF...");
                RECT r; GetWindowRect(hBtnSearchOpts, &r); TrackPopupMenu(hSearchMenu, TPM_RIGHTBUTTON, r.left, r.bottom, 0, hwnd, NULL); DestroyMenu(hSearchMenu);
            } else if (LOWORD(wParam) == 1001) { g_SearchRoads = !g_SearchRoads; PopulateSearchList(hwnd); }
            else if (LOWORD(wParam) == 1002) { g_SearchPostcodes = !g_SearchPostcodes; PopulateSearchList(hwnd); }
            else if (LOWORD(wParam) == 1006) {
                if (hPdfDlg) {
                    SyncPdfDialogOrientation();
                    ShowWindow(hPdfDlg, SW_SHOW); SetForegroundWindow(hPdfDlg);
                }
            }
            else if (LOWORD(wParam) == 4) {
                if (hEditPin) FinishPinEdit(hwnd);
                g_SelectedPinCount = 0; g_RouteStepCount = 0; g_RoutePathCount = 0;
                ShowWindow(hBtnCancel, SW_HIDE); ShowWindow(hCboRouteType, SW_HIDE);
                UpdateLegendWindow(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            } 
            else if (LOWORD(wParam) == 5 && HIWORD(wParam) == CBN_SELCHANGE) {
                if (hEditPin) FinishPinEdit(hwnd);
                g_RouteMode = SendMessage(hCboRouteType, CB_GETCURSEL, 0, 0);
                CalculateRoute();
                InvalidateRect(hwnd, NULL, FALSE);
            } 
            else if (LOWORD(wParam) == 1003 && HIWORD(wParam) == LBN_SELCHANGE) {
                if (hEditPin) FinishPinEdit(hwnd);
                int sel = SendMessage(hListSearch, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    uint64_t idx = SendMessage(hListSearch, LB_GETITEMDATA, sel, 0); SearchItem* item = &g_SearchItems[idx];
                    g_HighlightedChunk = item->chunk_idx; g_HighlightedFeat = item->feat_idx;
                    ShowWindow(hListSearch, SW_HIDE); SetFocus(hwnd);
                    
                    if (item->pcode && g_PinCount < 1000) {
                        snprintf(g_Pins[g_PinCount].name, sizeof(g_Pins[g_PinCount].name), "%s", item->pcode);
                        g_Pins[g_PinCount].lon = item->lon;
                        g_Pins[g_PinCount].lat = item->lat;
                        g_PinCount++;
                    }

                    EnterCriticalSection(&g_FileCS);
                    ZoomToFeatureWithSurroundingArea(hwnd, item->chunk_idx, item->feat_idx);
                    LeaveCriticalSection(&g_FileCS);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } break;
        }
        case WM_DROPFILES: {
            if (hEditPin) FinishPinEdit(hwnd);
            HDROP hDrop = (HDROP)wParam; char path[MAX_PATH]; DragQueryFileA(hDrop, 0, path, MAX_PATH); DragFinish(hDrop);
            if (strlen(g_MapFilesStr) + strlen(path) + 2 < sizeof(g_MapFilesStr)) { if (strlen(g_MapFilesStr) > 0) strcat(g_MapFilesStr, "|"); strcat(g_MapFilesStr, path); }
            char* ext = strrchr(path, '.'); if (ext && (strcasecmp(ext, ".qjm") == 0 || strcasecmp(ext, ".qjmc") == 0)) LoadQJMData(path);
            g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE); return 0;
        }
        case WM_SIZE: {
            if (hEditPin) FinishPinEdit(hwnd);
            UpdateLegendWindow(hwnd);
            g_CanvasDirty = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (hEditPin) FinishPinEdit(hwnd);
            
            if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (OpenClipboard(hwnd)) {
                    HANDLE hData = GetClipboardData(CF_TEXT);
                    if (hData) {
                        char* text = (char*)GlobalLock(hData);
                        if (text) {
                            ProcessInputString(hwnd, text);
                            GlobalUnlock(hData);
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }

            if (wParam == 'P' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (hPdfDlg) {
                    SyncPdfDialogOrientation();
                    ShowWindow(hPdfDlg, SW_SHOW); SetForegroundWindow(hPdfDlg);
                }
                return 0;
            }
            if (g_EditMode) {
                if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    SaveEditedQJM();
                    return 0;
                }
                if (g_HighlightedChunk != -1 && g_HighlightedFeat != -1) {
                    if (wParam == 'O') {
                        if (g_SelectedNode != -1) {
                            uint8_t* data = EnsureChunkEditable(g_HighlightedChunk);
                            if (data) {
                                PFeat* feats = (PFeat*)(data + 12);
                                PFeat* f = &feats[g_HighlightedFeat];
                                f->is_area = 0; 
                                g_MapModified = true;
                                g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE);
                            }
                        }
                        return 0;
                    } else if (wParam == 'D' || wParam == VK_DELETE || wParam == VK_BACK) {
                        if (g_SelectedNode != -1) {
                            DeleteNodeInFeature(g_HighlightedChunk, g_HighlightedFeat, g_SelectedNode);
                        } else {
                            DeleteFeature(g_HighlightedChunk, g_HighlightedFeat);
                        }
                        g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE); return 0;
                    } else if (wParam == 'A' || wParam == VK_INSERT) {
                        RECT rc; GetClientRect(hwnd, &rc);
                        POINT pt;
                        GetCursorPos(&pt);
                        ScreenToClient(hwnd, &pt);
                        int cx = pt.x;
                        int cy = pt.y;
                        double center_lon, center_lat;
                        NavScreenToLatLon(cx, cy, rc.right, rc.bottom, &center_lon, &center_lat);

                        uint8_t* data = EnsureChunkEditable(g_HighlightedChunk);
                        if (data) {
                            uint32_t f_cnt = *(uint32_t*)data;
                            PFeat* feats = (PFeat*)(data + 12);
                            float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                            PFeat* f = &feats[g_HighlightedFeat];

                            if (f->pt_cnt > 0) {
                                double min_d2 = 1e30;
                                int best_seg = 0;
                                double best_lon = center_lon, best_lat = center_lat;
                                
                                double pick_tol = 50.0 / g_Zoom;

                                for (uint32_t j = 0; j < f->pt_cnt; j++) {
                                    uint32_t next_j = (j + 1 < f->pt_cnt) ? (j + 1) : (f->is_area && f->pt_cnt >= 3 ? 0 : j);
                                    if (next_j == j) continue;

                                    float lon1 = points[f->pt_off + j*2], lat1 = points[f->pt_off + j*2 + 1];
                                    float lon2 = points[f->pt_off + next_j*2], lat2 = points[f->pt_off + next_j*2 + 1];

                                    float min_lon = lon1 < lon2 ? lon1 : lon2;
                                    float max_lon = lon1 > lon2 ? lon1 : lon2;
                                    float min_lat = lat1 < lat2 ? lat1 : lat2;
                                    float max_lat = lat1 > lat2 ? lat1 : lat2;

                                    if (center_lon < min_lon - pick_tol || center_lon > max_lon + pick_tol ||
                                        center_lat < min_lat - pick_tol || center_lat > max_lat + pick_tol) {
                                        continue;
                                    }

                                    double px1, py1, px2, py2;
                                    NavLatLonToCanvasScreen(lon1, lat1, rc.right, rc.bottom, &px1, &py1);
                                    NavLatLonToCanvasScreen(lon2, lat2, rc.right, rc.bottom, &px2, &py2);

                                    double vx = px2 - px1, vy = py2 - py1;
                                    double l2 = vx * vx + vy * vy;
                                    double t = 0.5;
                                    if (l2 > 1.0) {
                                        t = ((cx - px1) * vx + (cy - py1) * vy) / l2;
                                        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                                    }
                                    double projx = px1 + t * vx, projy = py1 + t * vy;
                                    double d2 = (cx - projx) * (cx - projx) + (cy - projy) * (cy - projy);

                                    if (d2 < min_d2) {
                                        min_d2 = d2;
                                        best_seg = (int)j;
                                        best_lon = lon1 + t * (lon2 - lon1);
                                        best_lat = lat1 + t * (lat2 - lat1);
                                    }
                                }

                                g_PendingInsertSeg = best_seg;
                                g_PendingInsertLon = best_lon;
                                g_PendingInsertLat = best_lat;
                                InsertNodeInFeature(g_HighlightedChunk, g_HighlightedFeat, best_seg);
                                g_PendingInsertSeg = -1;

                                g_SelectedNode = best_seg + 1;
                                g_SelectedNode2 = -1;
                            }
                            g_CanvasDirty = true;
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                        return 0;
                    } else if (wParam == 'C') {
                        if ((GetKeyState(VK_SHIFT) & 0x8000) && g_SelectedNode != -1 && g_SelectedNode2 != -1) {
                            ClosePolylineAndAddNode(g_HighlightedChunk, g_HighlightedFeat, g_SelectedNode, g_SelectedNode2);
                            g_CanvasDirty = true;
                            InvalidateRect(hwnd, NULL, FALSE);
                        } else {
                            uint8_t* data = EnsureChunkEditable(g_HighlightedChunk);
                            if (data) {
                                PFeat* feats = (PFeat*)(data + 12);
                                PFeat* f = &feats[g_HighlightedFeat];
                                f->fc = (f->fc == CLASS_WATER) ? CLASS_LAND : CLASS_WATER; f->is_area = 1;
                                g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE);
                            }
                        }
                        return 0;
                    } else if (wParam == 'K') {
                        uint8_t* data = EnsureChunkEditable(g_HighlightedChunk);
                        if (data) {
                            PFeat* feats = (PFeat*)(data + 12);
                            PFeat* f = &feats[g_HighlightedFeat];
                            if (f->fc == CLASS_COASTLINE) {
                                f->fc = CLASS_LAND;
                                f->is_area = 1;
                            } else {
                                f->fc = CLASS_COASTLINE;
                                f->is_area = 0;
                            }
                            g_MapModified = true;
                            g_CanvasDirty = true;
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                        return 0;
                    } else if (wParam == 'X') {
                        g_EditConstraint = (g_EditConstraint + 1) % 3;
                        InvalidateRect(hwnd, NULL, FALSE);
                        return 0;
                    } else if (wParam == 'L' || wParam == 'W') {
                        uint8_t* data = EnsureChunkEditable(g_HighlightedChunk);
                        if (data) {
                            PFeat* feats = (PFeat*)(data + 12);
                            PFeat* f = &feats[g_HighlightedFeat];
                            if (wParam == 'L') { f->fc = CLASS_LAND; f->is_area = 1; }
                            else { f->fc = CLASS_WATER; f->is_area = 1; }
                            g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE);
                        }
                        return 0;
                    }
                }
            }
            if (wParam == VK_SPACE) { g_AutoFit = true; AutoFitMap(hwnd); InvalidateRect(hwnd, NULL, FALSE); } 
            else if (wParam == VK_ESCAPE) {
                g_SelectedPinCount = 0; g_RouteStepCount = 0; g_RoutePathCount = 0;
                g_HighlightedChunk = -1; g_HighlightedFeat = -1; g_SelectedNode = -1; g_SelectedNode2 = -1;
                ShowWindow(hBtnCancel, SW_HIDE); ShowWindow(hCboRouteType, SW_HIDE);
                UpdateLegendWindow(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (hEditPin) FinishPinEdit(hwnd);
            int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam); RECT rc; GetClientRect(hwnd, &rc);
            bool pin_deleted = false;
            for (int i = 0; i < g_PinCount; i++) {
                double px, py; NavLatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rc.right, rc.bottom, g_PanX, g_PanY, g_Zoom, &px, &py);
                if (abs(mx - (int)px) <= 16 && (my - (int)py) >= -38 && (my - (int)py) <= 8) {
                    int sel_idx = -1; for (int k = 0; k < g_SelectedPinCount; k++) { if (g_SelectedPins[k] == i) sel_idx = k; }
                    if (sel_idx >= 0) { for (int k = sel_idx; k < g_SelectedPinCount - 1; k++) g_SelectedPins[k] = g_SelectedPins[k+1]; g_SelectedPinCount--; }
                    for (int k = 0; k < g_SelectedPinCount; k++) { if (g_SelectedPins[k] > i) g_SelectedPins[k]--; }
                    for (int k = i; k < g_PinCount - 1; k++) g_Pins[k] = g_Pins[k+1];
                    g_PinCount--; pin_deleted = true;
                    CalculateRoute();
                    break;
                }
            }
            if (!pin_deleted) {
                double c_lon, c_lat; NavScreenToLatLon(mx, my, rc.right, rc.bottom, &c_lon, &c_lat);
                double pick_tol = fmax(0.05, 16.0 / g_Zoom);
                double best_d = 999999; int best_chunk = -1, best_feat = -1;
                EnterCriticalSection(&g_FileCS);
                for (uint32_t c = 0; c < g_ChunkCount; c++) {
                    bool active_areas = IsChunkActiveForAreas(c, g_Zoom);
                    bool active_roads = IsChunkActiveForRoads(c, g_Zoom);
                    if (!active_areas && !active_roads) continue;
                    MapChunk* mc = &g_Chunks[c];
                    if (c_lon < mc->min_lon - pick_tol || c_lon > mc->max_lon + pick_tol || c_lat < mc->min_lat - pick_tol || c_lat > mc->max_lat + pick_tol) continue;
                    uint8_t* data = GetChunkData(c); if (!data) continue;
                    uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12); float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                    for (uint32_t i = 0; i < f_cnt; i++) {
                        PFeat* f = &feats[i];
                        if (!IsFeatureVisible(c, f, g_Zoom)) continue;
                        if (!g_EditMode && f->fc != CLASS_HWY_MAIN && f->fc != CLASS_HWY_MINOR) continue;
                        if (c_lon < f->min_lon - pick_tol || c_lon > f->max_lon + pick_tol || c_lat < f->min_lat - pick_tol || c_lat > f->max_lat + pick_tol) continue;
                        for (uint32_t j = 0; j < f->pt_cnt; j++) {
                            double ddx = points[f->pt_off + j*2] - c_lon, ddy = points[f->pt_off + j*2 + 1] - c_lat, d = ddx*ddx + ddy*ddy;
                            if (d < best_d) { best_d = d; best_chunk = c; best_feat = i; }
                        }
                    }
                }
                if (best_chunk != -1) {
                    g_HighlightedChunk = best_chunk;
                    g_HighlightedFeat = best_feat;
                    g_CanvasDirty = true;
                } else {
                    g_HighlightedChunk = -1;
                    g_HighlightedFeat = -1;
                    g_CanvasDirty = true;
                }
                LeaveCriticalSection(&g_FileCS);
                g_SelectedPinCount = 0; g_RouteStepCount = 0; g_RoutePathCount = 0;
                ShowWindow(hBtnCancel, SW_HIDE); ShowWindow(hCboRouteType, SW_HIDE);
            }
            UpdateLegendWindow(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (hEditPin) FinishPinEdit(hwnd);
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            ScreenToClient(hwnd, &pt);

            if (hLegend && IsWindowVisible(hLegend)) {
                RECT legRc; GetWindowRect(hLegend, &legRc);
                POINT spt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
                if (PtInRect(&legRc, spt)) {
                    SendMessage(hLegend, WM_MOUSEWHEEL, wParam, lParam);
                    return 0;
                }
            }

            RECT rect; GetClientRect(hwnd, &rect);
            if (rect.right <= 0 || rect.bottom <= 0) return 0;

            double mouse_lon, mouse_lat;
            NavScreenToLatLon(pt.x, pt.y, rect.right, rect.bottom, &mouse_lon, &mouse_lat);

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            double steps = (double)delta / 120.0;
            if (steps > 5.0) steps = 5.0;
            if (steps < -5.0) steps = -5.0;
            double zoom_factor = pow(1.2, steps);

            g_Zoom *= zoom_factor;
            if (g_Zoom < 2.0) g_Zoom = 2.0;
            if (g_Zoom > 250000.0) g_Zoom = 250000.0;

            double dx = pt.x - (rect.right / 2.0);
            double dy = pt.y - (rect.bottom / 2.0);
            if (fabs(g_Rotation) > 1e-6) {
                double c = cos(-g_Rotation), s = sin(-g_Rotation);
                double rx = dx * c - dy * s;
                double ry = dx * s + dy * c;
                dx = rx; dy = ry;
            }
            g_CenterLon = mouse_lon - dx / g_Zoom;
            g_CenterLat = mouse_lat + dy / g_Zoom;
            g_PanX = 0.0;
            g_PanY = 0.0;

            g_InteractiveCanvas = true;
            SetTimer(hwnd, 2, 140, NULL);
            g_CanvasDirty = true;
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (hEditPin) FinishPinEdit(hwnd);
            SetFocus(hwnd);
            int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
            g_LastMousePos.x = mx; g_LastMousePos.y = my;
            g_MouseMoved = false;
            g_PendingInsertSeg = -1;
            ShowWindow(hListSearch, SW_HIDE);
            SetCapture(hwnd);

            if (g_EditMode) {
                RECT rc; GetClientRect(hwnd, &rc);
                double c_lon, c_lat;
                NavScreenToLatLon(mx, my, rc.right, rc.bottom, &c_lon, &c_lat);

                EnterCriticalSection(&g_FileCS);

                bool handled = false;

                if (g_HighlightedChunk != -1 && g_HighlightedFeat != -1 && 
                    (uint32_t)g_HighlightedChunk < g_ChunkCount) {
                    uint8_t* data = GetChunkData(g_HighlightedChunk);
                    if (data) {
                        uint32_t f_cnt = *(uint32_t*)data;
                        if ((uint32_t)g_HighlightedFeat < f_cnt) {
                            PFeat* feats = (PFeat*)(data + 12);
                            float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                            PFeat* f = &feats[g_HighlightedFeat];
                            
                            if (IsFeatureVisible((uint32_t)g_HighlightedChunk, f, g_Zoom)) {
                                double pick_tol = 50.0 / g_Zoom;

                                for (uint32_t j = 0; j < f->pt_cnt; j++) {
                                    float n_lon = points[f->pt_off + j*2];
                                    float n_lat = points[f->pt_off + j*2 + 1];
                                    
                                    if (c_lon < n_lon - pick_tol || c_lon > n_lon + pick_tol ||
                                        c_lat < n_lat - pick_tol || c_lat > n_lat + pick_tol) continue;

                                    double px, py;
                                    NavLatLonToCanvasScreen(n_lon, n_lat, rc.right, rc.bottom, &px, &py);
                                    if (abs(mx - (int)px) <= 8 && abs(my - (int)py) <= 8) {
                                        if (GetKeyState(VK_SHIFT) & 0x8000) {
                                            g_SelectedNode2 = (int)j;
                                        } else {
                                            g_SelectedNode = (int)j;
                                            g_SelectedNode2 = -1;
                                            g_DragStartLon = n_lon;
                                            g_DragStartLat = n_lat;
                                        }
                                        g_DraggingNode = true;
                                        handled = true;
                                        break;
                                    }
                                }

                                if (!handled && f->pt_cnt > 0) {
                                    double best_d2 = 225.0;
                                    int best_seg = -1;
                                    double best_lon = c_lon, best_lat = c_lat;

                                    for (uint32_t j = 0; j < f->pt_cnt; j++) {
                                        uint32_t next_j = (j + 1 < f->pt_cnt) ? (j + 1) : (f->is_area && f->pt_cnt >= 3 ? 0 : j);
                                        if (next_j == j) continue;

                                        float lon1 = points[f->pt_off + j*2], lat1 = points[f->pt_off + j*2 + 1];
                                        float lon2 = points[f->pt_off + next_j*2], lat2 = points[f->pt_off + next_j*2 + 1];

                                        float min_lon = lon1 < lon2 ? lon1 : lon2;
                                        float max_lon = lon1 > lon2 ? lon1 : lon2;
                                        float min_lat = lat1 < lat2 ? lat1 : lat2;
                                        float max_lat = lat1 > lat2 ? lat1 : lat2;
                                        
                                        if (c_lon < min_lon - pick_tol || c_lon > max_lon + pick_tol ||
                                            c_lat < min_lat - pick_tol || c_lat > max_lat + pick_tol) continue;

                                        double px1, py1, px2, py2;
                                        NavLatLonToCanvasScreen(lon1, lat1, rc.right, rc.bottom, &px1, &py1);
                                        NavLatLonToCanvasScreen(lon2, lat2, rc.right, rc.bottom, &px2, &py2);

                                        double vx = px2 - px1, vy = py2 - py1;
                                        double l2 = vx * vx + vy * vy;
                                        if (l2 > 1.0) {
                                            double t = ((mx - px1) * vx + (my - py1) * vy) / l2;
                                            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                                            double projx = px1 + t * vx, projy = py1 + t * vy;
                                            double d2 = (mx - projx) * (mx - projx) + (my - projy) * (my - projy);
                                            if (d2 < best_d2) {
                                                best_d2 = d2;
                                                best_seg = (int)j;
                                                best_lon = lon1 + t * (lon2 - lon1);
                                                best_lat = lat1 + t * (lat2 - lat1);
                                            }
                                        }
                                    }

                                    if (best_seg != -1) {
                                        g_PendingInsertSeg = best_seg;
                                        g_PendingInsertLon = best_lon;
                                        g_PendingInsertLat = best_lat;
                                        InsertNodeInFeature(g_HighlightedChunk, g_HighlightedFeat, best_seg);
                                        g_DraggingNode = true;
                                        handled = true;
                                    }
                                }
                            }
                        }
                    }
                }

                if (!handled) {
                    double pick_tol = 16.0 / g_Zoom;
                    double best_scr_d = 144.0;
                    int best_c = -1, best_f = -1;

                    for (uint32_t c = 0; c < g_ChunkCount; c++) {
                        bool active_areas = IsChunkActiveForAreas(c, g_Zoom);
                        bool active_roads = IsChunkActiveForRoads(c, g_Zoom);
                        if (!active_areas && !active_roads) continue;
                        MapChunk* mc = &g_Chunks[c];
                        if (c_lon < mc->min_lon - pick_tol || c_lon > mc->max_lon + pick_tol || c_lat < mc->min_lat - pick_tol || c_lat > mc->max_lat + pick_tol) continue;
                        
                        uint8_t* d = GetChunkData(c); if (!d) continue;
                        uint32_t f_cnt = *(uint32_t*)d; PFeat* feats = (PFeat*)(d + 12); float* pts = (float*)(d + 12 + f_cnt * sizeof(PFeat));
                        for (uint32_t i = 0; i < f_cnt; i++) {
                            PFeat* f = &feats[i];
                            if (!IsFeatureVisible(c, f, g_Zoom)) continue;
                            if (c_lon < f->min_lon - pick_tol || c_lon > f->max_lon + pick_tol || c_lat < f->min_lat - pick_tol || c_lat > f->max_lat + pick_tol) continue;
                            for (uint32_t j = 0; j < f->pt_cnt; j++) {
                                uint32_t next_j = (j + 1 < f->pt_cnt) ? (j + 1) : (f->is_area && f->pt_cnt >= 3 ? 0 : j);
                                double px1, py1;
                                NavLatLonToCanvasScreen(pts[f->pt_off + j*2], pts[f->pt_off + j*2 + 1], rc.right, rc.bottom, &px1, &py1);
                                double dv = (px1 - mx)*(px1 - mx) + (py1 - my)*(py1 - my);
                                if (dv < best_scr_d) { best_scr_d = dv; best_c = (int)c; best_f = (int)i; }
                                if (next_j != j) {
                                    double px2, py2;
                                    NavLatLonToCanvasScreen(pts[f->pt_off + next_j*2], pts[f->pt_off + next_j*2 + 1], rc.right, rc.bottom, &px2, &py2);
                                    double vx = px2 - px1, vy = py2 - py1, l2 = vx*vx + vy*vy;
                                    if (l2 > 1.0) {
                                        double t = ((mx - px1)*vx + (my - py1)*vy) / l2;
                                        if (t > 0.0 && t < 1.0) {
                                            double ds = (mx - (px1 + t*vx))*(mx - (px1 + t*vx)) + (my - (py1 + t*vy))*(my - (py1 + t*vy));
                                            if (ds < best_scr_d) { best_scr_d = ds; best_c = (int)c; best_f = (int)i; }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (best_c != -1) {
                        g_HighlightedChunk = best_c;
                        g_HighlightedFeat = best_f;
                        g_SelectedNode = -1;
                        g_SelectedNode2 = -1;
                        handled = true;
                    }
                }

                LeaveCriticalSection(&g_FileCS);

                if (handled) {
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            g_IsDragging = true;
            return 0;
        }
        case WM_MOUSEMOVE: {
            int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
            if (g_DraggingNode && g_EditMode && g_HighlightedChunk != -1 && g_HighlightedFeat != -1) {
                if (g_SelectedNode != -1) {
                    RECT rc; GetClientRect(hwnd, &rc);
                    double n_lon, n_lat;
                    NavScreenToLatLon(mx, my, rc.right, rc.bottom, &n_lon, &n_lat);
                    uint8_t* data = EnsureChunkEditable(g_HighlightedChunk);
                    if (data) {
                        uint32_t f_cnt = *(uint32_t*)data;
                        PFeat* feats = (PFeat*)(data + 12);
                        float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                        PFeat* f = &feats[g_HighlightedFeat];
                        if ((uint32_t)g_SelectedNode < f->pt_cnt) {
                            double final_lon = n_lon;
                            double final_lat = n_lat;
                            if (g_EditConstraint == 1) {
                                final_lat = g_DragStartLat;
                            } else if (g_EditConstraint == 2) {
                                final_lon = g_DragStartLon;
                            }
                            points[f->pt_off + g_SelectedNode * 2] = (float)final_lon;
                            points[f->pt_off + g_SelectedNode * 2 + 1] = (float)final_lat;

                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                }
                return 0;
            }
            if (g_IsDragging) {
                if (hEditPin) FinishPinEdit(hwnd);
                if (abs(mx - g_LastMousePos.x) > 3 || abs(my - g_LastMousePos.y) > 3) g_MouseMoved = true;
                if (g_MouseMoved) {
                    int dx = mx - g_LastMousePos.x, dy = my - g_LastMousePos.y;
                    if (dx == 0 && dy == 0) return 0;
                    if (wParam & MK_SHIFT) {
                        g_Rotation += dx * 0.008;
                        g_InteractiveCanvas = true;
                        SetTimer(hwnd, 2, 120, NULL);
                        g_CanvasDirty = true;
                    } else {
                        g_PanX += dx;
                        g_PanY += dy;
                        g_InteractiveCanvas = true;
                        SetTimer(hwnd, 2, 120, NULL);
                    }
                    g_LastMousePos.x = mx;
                    g_LastMousePos.y = my;
                    InvalidateRect(hwnd, NULL, FALSE);
                    static DWORD s_LastPaint = 0;
                    DWORD now = GetTickCount();
                    if (now - s_LastPaint >= 14) {
                        s_LastPaint = now;
                        UpdateWindow(hwnd);
                    }
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_DraggingNode) {
                g_PendingInsertSeg = -1;
                g_DraggingNode = false;
                ReleaseCapture();

                if (g_HighlightedChunk != -1 && g_HighlightedFeat != -1) {
                    RecalcFeatureAndChunkBounds(g_HighlightedChunk, g_HighlightedFeat);
                }

                g_CanvasDirty = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_IsDragging) {
                g_IsDragging = false; ReleaseCapture();
                if (!g_MouseMoved && hEditPin == NULL) {
                    int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam); RECT rect; GetClientRect(hwnd, &rect);
                    bool hit_pin = false;
                    for (int i = 0; i < g_PinCount; i++) {
                        double px, py; NavLatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rect.right, rect.bottom, g_PanX, g_PanY, g_Zoom, &px, &py);
                        if (abs(mx - (int)px) <= 16 && (my - (int)py) >= -38 && (my - (int)py) <= 8) { 
                            bool already_sel = false; for(int k=0; k<g_SelectedPinCount; k++) { if(g_SelectedPins[k] == i) already_sel = true; }
                            if (!already_sel && g_SelectedPinCount < 1000) { g_SelectedPins[g_SelectedPinCount++] = i; CalculateRoute(); }
                            hit_pin = true; break; 
                        }
                    }
                    if (!hit_pin) {
                        double c_lon, c_lat; NavScreenToLatLon(mx, my, rect.right, rect.bottom, &c_lon, &c_lat);
                        double pick_tol = 20.0 / g_Zoom;
                        double best_scr_d = 196.0; 
                        int best_c = -1, best_f = -1, best_n = -1;

                        EnterCriticalSection(&g_FileCS);
                        for (uint32_t c = 0; c < g_ChunkCount; c++) {
                            bool active_areas = IsChunkActiveForAreas(c, g_Zoom);
                            bool active_roads = IsChunkActiveForRoads(c, g_Zoom);
                            if (!active_areas && !active_roads) continue;

                            MapChunk* mc = &g_Chunks[c];
                            if (c_lon < mc->min_lon - pick_tol || c_lon > mc->max_lon + pick_tol || c_lat < mc->min_lat - pick_tol || c_lat > mc->max_lat + pick_tol) continue;
                            uint8_t* data = GetChunkData(c); if (!data) continue;
                            uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12); float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                            for (uint32_t i = 0; i < f_cnt; i++) {
                                PFeat* f = &feats[i];
                                if (!IsFeatureVisible(c, f, g_Zoom)) continue;
                                if (!g_EditMode && f->fc != CLASS_HWY_MAIN && f->fc != CLASS_HWY_MINOR) continue;
                                if (c_lon < f->min_lon - pick_tol || c_lon > f->max_lon + pick_tol || c_lat < f->min_lat - pick_tol || c_lat > f->max_lat + pick_tol) continue;

                                for (uint32_t j = 0; j < f->pt_cnt; j++) {
                                    double px, py;
                                    NavLatLonToCanvasScreen(points[f->pt_off + j*2], points[f->pt_off + j*2 + 1], rect.right, rect.bottom, &px, &py);
                                    double d = (px - mx)*(px - mx) + (py - my)*(py - my);
                                    if (d < best_scr_d) {
                                        best_scr_d = d; best_c = (int)c; best_f = (int)i; best_n = (int)j;
                                    }
                                    uint32_t next_j = (j + 1 < f->pt_cnt) ? (j + 1) : (f->is_area && f->pt_cnt >= 3 ? 0 : j);
                                    if (next_j != j) {
                                        double px2, py2;
                                        NavLatLonToCanvasScreen(points[f->pt_off + next_j*2], points[f->pt_off + next_j*2 + 1], rect.right, rect.bottom, &px2, &py2);
                                        double vx = px2 - px, vy = py2 - py, l2 = vx*vx + vy*vy;
                                        if (l2 > 1.0) {
                                            double t = ((mx - px)*vx + (my - py)*vy) / l2;
                                            if (t > 0.0 && t < 1.0) {
                                                double projx = px + t * vx, projy = py + t * vy;
                                                double sd = (projx - mx)*(projx - mx) + (projy - my)*(projy - my);
                                                if (sd < best_scr_d) {
                                                    best_scr_d = sd; best_c = (int)c; best_f = (int)i;
                                                    best_n = (t < 0.5) ? (int)j : (int)next_j;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (best_c == -1 && g_EditMode) {
                            double best_area = 1e30;
                            for (uint32_t c = 0; c < g_ChunkCount; c++) {
                                if (!IsChunkActiveForAreas(c, g_Zoom)) continue;
                                MapChunk* mc = &g_Chunks[c];
                                if (c_lon < mc->min_lon || c_lon > mc->max_lon || c_lat < mc->min_lat || c_lat > mc->max_lat) continue;
                                uint8_t* data = GetChunkData(c); if (!data) continue;
                                uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12); float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                                for (uint32_t i = 0; i < f_cnt; i++) {
                                    PFeat* f = &feats[i];
                                    if (!f->is_area || !IsFeatureVisible(c, f, g_Zoom)) continue;
                                    if (c_lon < f->min_lon || c_lon > f->max_lon || c_lat < f->min_lat || c_lat > f->max_lat) continue;

                                    double area = (double)(f->max_lon - f->min_lon) * (double)(f->max_lat - f->min_lat);
                                    if (area < best_area && PointInPolygonPts(points + f->pt_off, f->pt_cnt, c_lon, c_lat)) {
                                        best_area = area;
                                        best_c = (int)c;
                                        best_f = (int)i;
                                        double min_vd = 1e30;
                                        best_n = 0;
                                        for (uint32_t j = 0; j < f->pt_cnt; j++) {
                                            double dx = points[f->pt_off + j*2] - c_lon;
                                            double dy = points[f->pt_off + j*2 + 1] - c_lat;
                                            if (dx*dx + dy*dy < min_vd) { min_vd = dx*dx + dy*dy; best_n = (int)j; }
                                        }
                                    }
                                }
                            }
                        }

                        g_HighlightedChunk = best_c;
                        g_HighlightedFeat = best_f;
                        if (GetKeyState(VK_SHIFT) & 0x8000) {
                            g_SelectedNode2 = best_n;
                        } else {
                            g_SelectedNode = best_n;
                            g_SelectedNode2 = -1;
                        }
                        LeaveCriticalSection(&g_FileCS);
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (hEditPin) FinishPinEdit(hwnd);
            if (g_EditMode && g_HighlightedChunk != -1 && g_HighlightedFeat != -1) {
                int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
                RECT rc; GetClientRect(hwnd, &rc);

                EnterCriticalSection(&g_FileCS);
                uint8_t* data = GetChunkData(g_HighlightedChunk);
                if (data) {
                    uint32_t f_cnt = *(uint32_t*)data;
                    if ((uint32_t)g_HighlightedFeat < f_cnt) {
                        PFeat* feats = (PFeat*)(data + 12);
                        float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                        PFeat* f = &feats[g_HighlightedFeat];

                        double best_d2 = 256.0;
                        int best_node = -1;

                        for (uint32_t j = 0; j < f->pt_cnt; j++) {
                            double px, py;
                            NavLatLonToCanvasScreen(points[f->pt_off + j*2], points[f->pt_off + j*2 + 1], rc.right, rc.bottom, &px, &py);
                            double d2 = (px - mx) * (px - mx) + (py - my) * (py - my);
                            if (d2 < best_d2) {
                                best_d2 = d2;
                                best_node = (int)j;
                            }
                        }

                        if (best_node != -1) {
                            g_SelectedNode = best_node;
                            InsertNodeInFeature(g_HighlightedChunk, g_HighlightedFeat, g_SelectedNode);
                        }
                    }
                }
                LeaveCriticalSection(&g_FileCS);

                g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam); RECT rc; GetClientRect(hwnd, &rc); bool hit_pin = false;
            for (int i = 0; i < g_PinCount; i++) {
                double px, py; NavLatLonToScreen(g_Pins[i].lon, g_Pins[i].lat, rc.right, rc.bottom, g_PanX, g_PanY, g_Zoom, &px, &py);
                if (abs(mx - (int)px) <= 16 && (my - (int)py) >= -38 && (my - (int)py) <= 8) {
                    g_EditingPinIndex = i; hEditPin = CreateWindowExA(0, "EDIT", g_Pins[i].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, (int)px - 50, (int)py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                    OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc); SetFocus(hEditPin); SendMessage(hEditPin, EM_SETSEL, 0, -1); InvalidateRect(hwnd, NULL, FALSE); hit_pin = true; break;
                }
            }
            if (!hit_pin && g_PinCount < 1000) {
                if (my < 45 || (hLegend && IsWindowVisible(hLegend) && mx < 280)) return 0;
                g_EditingPinIndex = g_PinCount;
                double t_lon, t_lat; NavScreenToLatLon(mx, my, rc.right, rc.bottom, &t_lon, &t_lat);
                double snap_dist = 999999, snap_rad = fmax(0.04, 35.0 / g_Zoom);
                EnterCriticalSection(&g_FileCS);
                for (uint32_t c = 0; c < g_ChunkCount; c++) {
                    MapChunk* mc = &g_Chunks[c]; if (mc->is_overview) continue;
                    if (t_lon < mc->min_lon - snap_rad || t_lon > mc->max_lon + snap_rad || t_lat < mc->min_lat - snap_rad || t_lat > mc->max_lat + snap_rad) continue;
                    uint8_t* data = GetChunkData(c); if (!data) continue;
                    uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12); float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
                    for (uint32_t i = 0; i < f_cnt; i++) {
                        PFeat* f = &feats[i];
                        if (!IsFeatureVisible(c, f, g_Zoom)) continue;
                        if (f->fc != CLASS_HWY_MAIN && f->fc != CLASS_HWY_MINOR) continue;
                        if (t_lon < f->min_lon - snap_rad || t_lon > f->max_lon + snap_rad || t_lat < f->min_lat - snap_rad || t_lat > f->max_lat + snap_rad) continue;
                        for (uint32_t j = 0; j < f->pt_cnt; j++) {
                            double ddx = points[f->pt_off + j*2] - t_lon, ddy = points[f->pt_off + j*2 + 1] - t_lat, d = ddx*ddx + ddy*ddy;
                            if (d < snap_dist) { snap_dist = d; g_Pins[g_PinCount].lon = points[f->pt_off + j*2]; g_Pins[g_PinCount].lat = points[f->pt_off + j*2 + 1]; }
                        }
                    }
                }
                LeaveCriticalSection(&g_FileCS);
                if (snap_dist == 999999) { g_Pins[g_PinCount].lon = t_lon; g_Pins[g_PinCount].lat = t_lat; }
                snprintf(g_Pins[g_PinCount].name, 128, "%d", g_PinCount + 1); g_SelectedPins[g_SelectedPinCount++] = g_PinCount++;
                if (g_SelectedPinCount >= 2) CalculateRoute();
                double e_px, e_py; NavLatLonToScreen(g_Pins[g_PinCount-1].lon, g_Pins[g_PinCount-1].lat, rc.right, rc.bottom, g_PanX, g_PanY, g_Zoom, &e_px, &e_py);
                hEditPin = CreateWindowExA(0, "EDIT", g_Pins[g_PinCount-1].name, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, (int)e_px - 50, (int)e_py - 55, 100, 20, hwnd, (HMENU)99, NULL, NULL);
                OldEditProc = (WNDPROC)SetWindowLongPtr(hEditPin, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc); SetFocus(hEditPin); SendMessage(hEditPin, EM_SETSEL, 0, -1); InvalidateRect(hwnd, NULL, FALSE);
            } return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc); int w = rc.right, h = rc.bottom;
            if (w > 0 && h > 0) {
                if (g_IsLoading && g_ChunkCount == 0) {
                    HBRUSH bgOceanBrush = CreateSolidBrush(c_Water);
                    FillRect(hdc, &rc, bgOceanBrush);
                    DeleteObject(bgOceanBrush);
                } else {
                    if (!g_hdcScreen) g_hdcScreen = CreateCompatibleDC(hdc);
                    if (g_ScreenW != w || g_ScreenH != h) {
                        if (g_hbmScreen) DeleteObject(g_hbmScreen);
                        g_hbmScreen = CreateCompatibleBitmap(hdc, w, h);
                        SelectObject(g_hdcScreen, g_hbmScreen);
                        g_ScreenW = w; g_ScreenH = h;
                    }

                    int dx = (int)(g_PanX - g_CanvasPanX);
                    int dy = (int)(g_PanY - g_CanvasPanY);

                    if (g_CanvasW != w || g_CanvasH != h || g_Zoom != g_CanvasZoom || g_Rotation != g_CanvasRot || g_CanvasDirty ||
                        abs(dx) >= w - 4 || abs(dy) >= h - 4) {
                        UpdateCanvas(hwnd);
                    } else if (dx != 0 || dy != 0) {
                        ScrollAndUpdateCanvas(hwnd, dx, dy);
                    }

                    BitBlt(g_hdcScreen, 0, 0, w, h, g_hdcCanvas, 0, 0, SRCCOPY);
                    EnterCriticalSection(&g_FileCS);
                    DrawOverlay(g_hdcScreen, w, h);
                    LeaveCriticalSection(&g_FileCS);

                    if (g_ChunkCount == 0 && !g_IsLoading) {
                        SetBkMode(g_hdcScreen, TRANSPARENT);
                        SetTextColor(g_hdcScreen, RGB(0, 50, 150));
                        SetTextAlign(g_hdcScreen, TA_CENTER | TA_BASELINE);

                        HFONT hFont = CreateFont(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                        HFONT hOldFont = (HFONT)SelectObject(g_hdcScreen, hFont);
                        const char* prompt = "Drag QJM map here to load.";
                        TextOutA(g_hdcScreen, w / 2, h / 2, prompt, strlen(prompt));
                        SelectObject(g_hdcScreen, hOldFont);
                        DeleteObject(hFont);
                    }

                    BitBlt(hdc, 0, 0, w, h, g_hdcScreen, 0, 0, SRCCOPY);
                }
            }
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_COPYDATA: {
            COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;
            if (cds && cds->lpData) {
                ProcessInputString(hwnd, (const char*)cds->lpData);
            }
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        case WM_DESTROY:
            StopHttpServer(); UnregisterHotKey(hwnd, 1);
            if (g_EditMode && g_MapModified) {
                SaveEditedQJM();
            }
            if (g_SavePins) {
                RECT crc; GetClientRect(hwnd, &crc);
                if (crc.right > 0 && crc.bottom > 0 && (fabs(g_PanX) > 0.01 || fabs(g_PanY) > 0.01)) {
                    double new_lon, new_lat;
                    NavScreenToLatLon(crc.right / 2, crc.bottom / 2, crc.right, crc.bottom, &new_lon, &new_lat);
                    g_CenterLon = new_lon;
                    g_CenterLat = new_lat;
                    g_PanX = 0.0;
                    g_PanY = 0.0;
                }
                FILE* ini = fopen("qjmnavig.ini", "w");
                if (ini) {
                    fprintf(ini, "centerLon=%.8f\ncenterLat=%.8f\npanX=%.4f\npanY=%.4f\nzoom=%.4f\nrotation=%.6f\n", g_CenterLon, g_CenterLat, g_PanX, g_PanY, g_Zoom, g_Rotation);
                    RECT wrect; GetWindowRect(hwnd, &wrect);
                    fprintf(ini, "winX=%d\nwinY=%d\nwinW=%d\nwinH=%d\n", (int)wrect.left, (int)wrect.top, (int)(wrect.right - wrect.left), (int)(wrect.bottom - wrect.top));
                    fprintf(ini, "pageW=%.2f\npageH=%.2f\nmargH=%.2f\nmargV=%.2f\nisLandscape=%d\n", page_w_mm, page_h_mm, g_MargH, g_MargV, isLandscape);
                    fprintf(ini, "showLand=%d\nshowWater=%d\nshowPark=%d\nshowCoast=%d\nshowHwyMin=%d\nshowHwyMain=%d\nshowLabels=%d\n", g_ShowLand, g_ShowWater, g_ShowPark, g_ShowCoast, g_ShowHwyMin, g_ShowHwyMain, g_ShowLabels);
                    fprintf(ini, "pdfLand=%d\npdfWater=%d\npdfPark=%d\npdfCoast=%d\npdfHwyMin=%d\npdfHwyMain=%d\npdfLabels=%d\n", g_PdfExpLand, g_PdfExpWater, g_PdfExpPark, g_PdfExpCoast, g_PdfExpHwyMin, g_PdfExpHwyMain, g_PdfExpLabels);
                    fprintf(ini, "showDebug=%d\nrouteMode=%d\nsearchRoads=%d\nsearchPostcodes=%d\nworldMapZoomMax=%.2f\nhighwayZoomMult=%.2f\n", g_ShowDebug, g_RouteMode, g_SearchRoads, g_SearchPostcodes, g_WorldMapMaxZoom, g_HighwayZoomMult);
                    
                    char hex[16]; ColorToHex(c_Land, hex); fprintf(ini, "colLand=%s\n", hex); ColorToHex(c_Water, hex); fprintf(ini, "colWater=%s\n", hex);
                    ColorToHex(c_Park, hex); fprintf(ini, "colPark=%s\n", hex); ColorToHex(c_Coast, hex); fprintf(ini, "colCoast=%s\n", hex);
                    ColorToHex(c_HwyMin, hex); fprintf(ini, "colHwyMin=%s\n", hex); ColorToHex(c_HwyMain, hex); fprintf(ini, "colHwyMain=%s\n", hex);
                    ColorToHex(c_Pin, hex); fprintf(ini, "colPin=%s\n", hex); ColorToHex(c_PinSel, hex); fprintf(ini, "colPinSel=%s\n", hex); ColorToHex(c_Route, hex); fprintf(ini, "colRoute=%s\n", hex);
                    
                    if (g_PinCount > 0) {
                        fprintf(ini, "pins="); for (int i=0; i<g_PinCount; i++) { fprintf(ini, "%s:long:%.7f:lat:%.7f|", g_Pins[i].name, g_Pins[i].lon, g_Pins[i].lat); } fprintf(ini, "\n");
                    } else fprintf(ini, "pins=\n");
                    
                    char routeStr[8192] = "";
                    for (int i = 0; i < g_SelectedPinCount; i++) {
                        if (i > 0) strcat(routeStr, "|");
                        if (strlen(routeStr) + strlen(g_Pins[g_SelectedPins[i]].name) < sizeof(routeStr)) strcat(routeStr, g_Pins[g_SelectedPins[i]].name);
                    }
                    fprintf(ini, "route=%s\nmaps=%s\n", routeStr, g_MapFilesStr);
                    fprintf(ini, "worldMap=%s\n", g_WorldMapFile);
                    fclose(ini);
                }
            } 
            if (g_hbmScreen) DeleteObject(g_hbmScreen);
            if (g_hdcScreen) DeleteDC(g_hdcScreen);
            DeleteCriticalSection(&g_FileCS);
            PostQuitMessage(0); return 0;
    } 
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "QjmNavigMutex_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (lpCmdLine && strlen(lpCmdLine) > 0) {
            char* p = lpCmdLine;
            while (*p == ' ') p++;
            if (strncmp(p, "-e", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
                p += 2; while (*p == ' ') p++;
            }
            
            char cleanCmd[4096] = {0};
            strncpy(cleanCmd, p, 4095);
            if (cleanCmd[0] == '"') {
                size_t len = strlen(cleanCmd);
                if (cleanCmd[len-1] == '"') cleanCmd[len-1] = '\0';
                memmove(cleanCmd, cleanCmd + 1, strlen(cleanCmd));
            }

            HWND hwndExisting = FindWindowA("QjmnavigClass", NULL);
            if (hwndExisting) {
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (DWORD)(strlen(cleanCmd) + 1);
                cds.lpData = (void*)cleanCmd;
                SendMessageA(hwndExisting, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
            } else {
                CopyRouteDistanceToClipboard(cleanCmd);
            }
        }
        return 0;
    }

    InitializeCriticalSection(&g_FileCS);
    for (int i = 0; i < LRU_SIZE; i++) g_LRU[i].chunk_idx = -1;

    char initMaps[4096] = {0}; FILE* ini = fopen("qjmnavig.ini", "r");
    if (ini) {
        char line[8192];
        while (fgets(line, sizeof(line), ini)) {
            if (strncmp(line, "maps=", 5) == 0) { line[strcspn(line, "\r\n")] = 0; strcpy(initMaps, line + 5); strcpy(g_MapFilesStr, line + 5); } 
            else if (strncmp(line, "worldMap=", 9) == 0) { line[strcspn(line, "\r\n")] = 0; strcpy(g_WorldMapFile, line + 9); }
            else if (strncmp(line, "centerLon=", 10) == 0) { g_CenterLon = atof(line + 10); g_HasSavedView = true; }
            else if (strncmp(line, "centerLat=", 10) == 0) { g_CenterLat = atof(line + 10); g_HasSavedView = true; }
            else if (strncmp(line, "panX=", 5) == 0) g_PanX = atof(line + 5);
            else if (strncmp(line, "panY=", 5) == 0) g_PanY = atof(line + 5); 
            else if (strncmp(line, "zoom=", 5) == 0) { g_Zoom = atof(line + 5); g_AutoFit = false; g_HasSavedView = true; }
            else if (strncmp(line, "rotation=", 9) == 0) g_Rotation = atof(line + 9); 
            else if (strncmp(line, "winX=", 5) == 0) g_WinX = atoi(line + 5); else if (strncmp(line, "winY=", 5) == 0) g_WinY = atoi(line + 5); 
            else if (strncmp(line, "winW=", 5) == 0) g_WinW = atoi(line + 5); else if (strncmp(line, "winH=", 5) == 0) g_WinH = atoi(line + 5); 
            else if (strncmp(line, "pageW=", 6) == 0) page_w_mm = atof(line + 6); else if (strncmp(line, "pageH=", 6) == 0) page_h_mm = atof(line + 6); 
            else if (strncmp(line, "isLandscape=", 12) == 0) isLandscape = atoi(line + 12); else if (strncmp(line, "margH=", 6) == 0) g_MargH = atof(line + 6); 
            else if (strncmp(line, "margV=", 6) == 0) g_MargV = atof(line + 6); else if (strncmp(line, "showLand=", 9) == 0) g_ShowLand = atoi(line + 9); 
            else if (strncmp(line, "showWater=", 10) == 0) g_ShowWater = atoi(line + 10); else if (strncmp(line, "showPark=", 9) == 0) g_ShowPark = atoi(line + 9); 
            else if (strncmp(line, "showCoast=", 10) == 0) g_ShowCoast = atoi(line + 10); else if (strncmp(line, "showHwyMin=", 11) == 0) g_ShowHwyMin = atoi(line + 11); 
            else if (strncmp(line, "showHwyMain=", 12) == 0) g_ShowHwyMain = atoi(line + 12); else if (strncmp(line, "showLabels=", 11) == 0) g_ShowLabels = atoi(line + 11); 
            else if (strncmp(line, "pdfLand=", 8) == 0) g_PdfExpLand = atoi(line + 8); else if (strncmp(line, "pdfWater=", 9) == 0) g_PdfExpWater = atoi(line + 9); 
            else if (strncmp(line, "pdfPark=", 8) == 0) g_PdfExpPark = atoi(line + 8); else if (strncmp(line, "pdfCoast=", 9) == 0) g_PdfExpCoast = atoi(line + 9); 
            else if (strncmp(line, "pdfHwyMin=", 10) == 0) g_PdfExpHwyMin = atoi(line + 10); else if (strncmp(line, "pdfHwyMain=", 11) == 0) g_PdfExpHwyMain = atoi(line + 11); 
            else if (strncmp(line, "pdfLabels=", 10) == 0) g_PdfExpLabels = atoi(line + 10); else if (strncmp(line, "showDebug=", 10) == 0) g_ShowDebug = atoi(line + 10); 
            else if (strncmp(line, "routeMode=", 10) == 0) g_RouteMode = atoi(line + 10); else if (strncmp(line, "searchRoads=", 12) == 0) g_SearchRoads = atoi(line + 12); 
            else if (strncmp(line, "searchPostcodes=", 16) == 0) g_SearchPostcodes = atoi(line + 16); 
            else if (sscanf(line, "worldMapZoomMax=%lf", &g_WorldMapMaxZoom) == 1) {}
            else if (sscanf(line, "highwayZoomMult=%lf", &g_HighwayZoomMult) == 1) {}
        } fclose(ini);
    }
    
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        char* p = lpCmdLine;
        while (*p == ' ') p++;
        if (strncmp(p, "-e", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
            g_EditMode = true; p += 2; while (*p == ' ') p++;
        }
        
        char cleanCmd[4096] = {0}; 
        strncpy(cleanCmd, p, 4095);
        if (cleanCmd[0] == '"') {
            size_t len = strlen(cleanCmd); 
            if (cleanCmd[len-1] == '"') cleanCmd[len-1] = '\0'; 
            memmove(cleanCmd, cleanCmd + 1, strlen(cleanCmd));
        }

        if (strstr(cleanCmd, "long:") != NULL || strncmp(cleanCmd, "plot:", 5) == 0 || strncmp(cleanCmd, "route:", 6) == 0 || (strchr(cleanCmd, '|') != NULL && !strstr(cleanCmd, ".qjm"))) {
            strncpy(g_PendingCliCommand, cleanCmd, sizeof(g_PendingCliCommand) - 1);
        } else if (cleanCmd[0]) {
            strcpy(initMaps, cleanCmd); 
            strcpy(g_MapFilesStr, cleanCmd);
        }
    }

    WNDCLASSA wc = {0}; wc.lpfnWndProc = NavigWndProc; wc.hInstance = hInstance; wc.style = CS_DBLCLKS; wc.lpszClassName = "QjmnavigClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc);

    WNDCLASSA wcl = {0}; wcl.lpfnWndProc = LegendWndProc; wcl.hInstance = hInstance; wcl.lpszClassName = "QjmLegendClass";
    wcl.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wcl);

    WNDCLASSA wcd = {0}; wcd.lpfnWndProc = PdfWndProc; wcd.hInstance = hInstance; wcd.lpszClassName = "PdfDialogClass";
    wcd.hbrBackground = (HBRUSH)(COLOR_WINDOW); wcd.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wcd);

    g_IsLoading = (strlen(initMaps) > 0 || g_WorldMapFile[0] != '\0');
    HWND hwnd = CreateWindowExA(0, "QjmnavigClass", g_EditMode ? "QJM Map Navigator [EDIT MODE]" : "QJM Map Navigator", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, g_WinX, g_WinY, g_WinW, g_WinH, NULL, NULL, hInstance, NULL);
    hPdfDlg = CreateWindowExA(0, "PdfDialogClass", "Export PDF Options", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, g_WinX + 50, g_WinY + 50, 280, 260, hwnd, NULL, hInstance, NULL);

    if (g_WorldMapFile[0]) { LoadQJMData(g_WorldMapFile); }
    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);

    if (g_WorldMapFile[0]) {
        g_IsLoading = false;
        g_CanvasDirty = true; InvalidateRect(hwnd, NULL, FALSE); UpdateWindow(hwnd);
    }

    if (strlen(initMaps) > 0) {
        LoadThreadArgs* args = malloc(sizeof(LoadThreadArgs));
        if (args) {
            strncpy(args->maps_str, initMaps, sizeof(args->maps_str) - 1);
            args->maps_str[sizeof(args->maps_str) - 1] = '\0';
            CreateThread(NULL, 0, BackgroundLoadMapsThread, args, 0, NULL);
        }
    } else if (g_PendingCliCommand[0] != '\0') {
        PostMessage(hwnd, WM_APP + 1, 0, 0);
    }

    MSG msg = {0}; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    ReleaseMutex(hMutex); CloseHandle(hMutex);
    return 0;
}
