/* ============================================================================
 *
 * pbf2qjm - High-Speed Out-Of-Core PBF to QJMC Compiler (32-bit LargeAddressAware)
 *
 * COMPILATION:
 *
 * gcc -O3 -m32 -mwindows -Wl,--large-address-aware -Wall -Wextra pbf2qjm.c -o pbf2qjm.exe -lcomdlg32 -lgdi32 -lshell32
 *
 *  2026 HUGO
 *
 * THIS WORK IS NOT FIT FOR ANY FUNCTION OR PURPOSE, COMES WITH NO WARRANTY,
 * AND IS BEING RELEASED INTO THE PUBLIC DOMAIN.
 * ============================================================================ */

#include "qjm.h"
double g_CenterLon = 0.0, g_CenterLat = 0.0, g_PanX = 0.0, g_PanY = 0.0, g_Zoom = 5.0, g_Rotation = 0.0;

#define IO_BUF_SIZE (1024 * 1024 * 4)
#define MASK_RES 128
typedef struct { char inFile[MAX_PATH]; char outFile[MAX_PATH]; char refFile[MAX_PATH]; } ThreadArgs;

typedef struct {
    int32_t qx, qy;
    uint32_t way_idx;
    uint8_t is_end;
} CoastEndPt;

typedef struct {
    WayNode* nodes;
    uint32_t count;
} RefPoly;

// --- EXPLICIT STRUCT ADDITIONS FOR EXTERNAL SORTING ---
//typedef struct { uint64_t morton; uint64_t offset; } SortEntry;
typedef struct { SortEntry entry; int chunk_idx; } HeapNode;

static RefPoly* g_RefPolys = NULL;
static uint32_t g_RefPolyCount = 0;

static bool g_CfgLand = true, g_CfgCoast = true, g_CfgHwyMain = true, g_CfgHwyMin = true;
static bool g_CfgWater = false, g_CfgPark = false, g_CfgPcode = true, g_CfgSpecs = false;
static char g_ShapefileFile[MAX_PATH] = {0};
static HWND g_hCompilerWnd = NULL;
static HWND hTxtIn, hTxtRef, hTxtShp, hTxtOut, hBtnIn, hBtnRef, hBtnShp, hBtnOut, hBtnRun, hBtnDelTmp, hLblStatus;
static HWND hChkPcode, hChkLZAV, hChkLand, hChkCoast, hChkHwyMain, hChkHwyMin, hChkWater, hChkPark, hChkSpecs;

static HWND hTxtSimpLand, hTxtSimpCoast, hTxtSimpHwyMain, hTxtSimpHwyMin, hTxtSimpWater, hTxtSimpPark, hTxtSimpLowPoly;
static HWND hChkNoRoads, hTxtSimpNoRoads;
static int g_SimpLand = 100, g_SimpCoast = 100, g_SimpHwyMain = 100, g_SimpHwyMin = 100, g_SimpWater = 100, g_SimpPark = 100, g_SimpLowPoly = 75;
static bool g_CfgNoRoads = true;
static int g_SimpNoRoads = 100;

// Forward declaration to satisfy the compiler
static LRESULT CALLBACK CompilerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void WriteRawFeature(FILE* f_raw, WayNode* wnodes, uint32_t ref_count, bool is_area, const char* name, const char* pcode, uint8_t fc, FILE* f_sort, uint64_t* cnt);

static void ProjectToBBox(double lon, double lat, double min_x, double max_x, double min_y, double max_y, WayNode* out_node);
static bool IsPointInPolygon(double lon, double lat, WayNode* poly, uint32_t count) {
    bool inside = false;
    for (uint32_t i = 0, j = count - 1; i < count; j = i++) {
        if (((poly[i].lat > lat) != (poly[j].lat > lat)) &&
            (lon < (poly[j].lon - poly[i].lon) * (lat - poly[i].lat) / (poly[j].lat - poly[i].lat) + poly[i].lon)) {
            inside = !inside;
        }
    }
    return inside;
}

static bool IsPointInWaterPolys(double lon, double lat, AccumWay* water_polys, uint32_t water_poly_cnt) {
    for (uint32_t i = 0; i < water_poly_cnt; i++) {
        if (water_polys[i].count < 3) continue;
        if (IsPointInPolygon(lon, lat, water_polys[i].nodes, water_polys[i].count)) {
            return true;
        }
    }
    return false;
}

static bool IsPolygonMostlyWater(WayNode* poly, uint32_t count, AccumWay* water_polys, uint32_t water_poly_cnt) {
    if (count < 3 || water_poly_cnt == 0) return false;

    double min_x = 180.0, max_x = -180.0, min_y = 90.0, max_y = -90.0;
    for (uint32_t i = 0; i < count; i++) {
        if (poly[i].lon < min_x) min_x = poly[i].lon;
        if (poly[i].lon > max_x) max_x = poly[i].lon;
        if (poly[i].lat < min_y) min_y = poly[i].lat;
        if (poly[i].lat > max_y) max_y = poly[i].lat;
    }

    uint32_t inside_poly = 0, inside_water = 0;
    for (int gx = 1; gx <= 7; gx++) {
        for (int gy = 1; gy <= 7; gy++) {
            double tx = min_x + (max_x - min_x) * (gx / 8.0);
            double ty = min_y + (max_y - min_y) * (gy / 8.0);
            if (IsPointInPolygon(tx, ty, poly, count)) {
                inside_poly++;
                if (IsPointInWaterPolys(tx, ty, water_polys, water_poly_cnt)) {
                    inside_water++;
                }
            }
        }
    }

    if (inside_poly == 0) {
        double cx = (poly[0].lon + poly[count / 3].lon + poly[(2 * count) / 3].lon) / 3.0;
        double cy = (poly[0].lat + poly[count / 3].lat + poly[(2 * count) / 3].lat) / 3.0;
        return IsPointInWaterPolys(cx, cy, water_polys, water_poly_cnt);
    }

    return (inside_water * 2 >= inside_poly);
}

// --- Bowtie Intersection Detection & Geometry Helpers ---
static int BowtieOrientation(WayNode p, WayNode q, WayNode r) {
    double val = (q.lat - p.lat) * (r.lon - q.lon) - (q.lon - p.lon) * (r.lat - q.lat);
    if (fabs(val) < 1e-11) return 0; // Collinear
    return (val > 0) ? 1 : 2; // Clockwise or Counter-Clockwise
}

static bool BowtieOnSegment(WayNode p, WayNode q, WayNode r) {
    return (q.lon <= fmax(p.lon, r.lon) && q.lon >= fmin(p.lon, r.lon) &&
            q.lat <= fmax(p.lat, r.lat) && q.lat >= fmin(p.lat, r.lat));
}

static bool BowtieSegmentsIntersect(WayNode p1, WayNode q1, WayNode p2, WayNode q2) {
    if (fmax(p1.lon, q1.lon) < fmin(p2.lon, q2.lon) || fmax(p2.lon, q2.lon) < fmin(p1.lon, q1.lon) ||
        fmax(p1.lat, q1.lat) < fmin(p2.lat, q2.lat) || fmax(p2.lat, q2.lat) < fmin(p1.lat, q1.lat)) {
        return false;
    }

    if ((fabs(p1.lon - p2.lon) < 1e-7 && fabs(p1.lat - p2.lat) < 1e-7) ||
        (fabs(p1.lon - q2.lon) < 1e-7 && fabs(p1.lat - q2.lat) < 1e-7) ||
        (fabs(q1.lon - p2.lon) < 1e-7 && fabs(q1.lat - p2.lat) < 1e-7) ||
        (fabs(q1.lon - q2.lon) < 1e-7 && fabs(q1.lat - q2.lat) < 1e-7)) return false;

    int o1 = BowtieOrientation(p1, q1, p2);
    int o2 = BowtieOrientation(p1, q1, q2);
    int o3 = BowtieOrientation(p2, q2, p1);
    int o4 = BowtieOrientation(p2, q2, q1);

    if (o1 != o2 && o3 != o4) return true;
    if (o1 == 0 && BowtieOnSegment(p1, p2, q1)) return true;
    if (o2 == 0 && BowtieOnSegment(p1, q2, q1)) return true;
    if (o3 == 0 && BowtieOnSegment(p2, p1, q2)) return true;
    if (o4 == 0 && BowtieOnSegment(p2, q1, q2)) return true;
    return false;
}

static bool HasBowtie(WayNode* coast, uint32_t c_cnt, WayNode* route, uint32_t r_cnt) {
    if (c_cnt < 2 || r_cnt < 2) return false;
    WayNode cS = coast[0], cE = coast[c_cnt - 1];
    WayNode rS = route[0], rE = route[r_cnt - 1];

    for (uint32_t i = 0; i < r_cnt - 1; i++) {
        WayNode r1 = route[i], r2 = route[i + 1];
        for (uint32_t j = 0; j < c_cnt - 1; j++) {
            if (BowtieSegmentsIntersect(r1, r2, coast[j], coast[j + 1])) return true;
        }
    }

    for (uint32_t i = 0; i < r_cnt - 1; i++) {
        for (uint32_t j = i + 2; j < r_cnt - 1; j++) {
            if (BowtieSegmentsIntersect(route[i], route[i + 1], route[j], route[j + 1])) return true;
        }
    }

    if (fabs(cE.lon - rS.lon) > 1e-7 || fabs(cE.lat - rS.lat) > 1e-7) {
        for (uint32_t j = 0; j + 2 < c_cnt; j++) {
            if (BowtieSegmentsIntersect(cE, rS, coast[j], coast[j + 1])) return true;
        }
        for (uint32_t i = 1; i < r_cnt - 1; i++) {
            if (BowtieSegmentsIntersect(cE, rS, route[i], route[i + 1])) return true;
        }
    }

    if (fabs(rE.lon - cS.lon) > 1e-7 || fabs(rE.lat - cS.lat) > 1e-7) {
        for (uint32_t j = 1; j < c_cnt - 1; j++) {
            if (BowtieSegmentsIntersect(rE, cS, coast[j], coast[j + 1])) return true;
        }
        for (uint32_t i = 0; i + 2 < r_cnt; i++) {
            if (BowtieSegmentsIntersect(rE, cS, route[i], route[i + 1])) return true;
        }
    }

    if ((fabs(cE.lon - rS.lon) > 1e-7 || fabs(cE.lat - rS.lat) > 1e-7) &&
        (fabs(rE.lon - cS.lon) > 1e-7 || fabs(rE.lat - cS.lat) > 1e-7)) {
        if (BowtieSegmentsIntersect(cE, rS, rE, cS)) return true;
    }

    return false;
}

static uint32_t CountRoadSamplesInside(WayNode* poly, uint32_t count, WayNode* samples, uint32_t sample_cnt) {
    if (count < 3 || sample_cnt == 0) return 0;
    double min_x = 180.0, max_x = -180.0, min_y = 90.0, max_y = -90.0;
    for (uint32_t i = 0; i < count; i++) {
        if (poly[i].lon < min_x) min_x = poly[i].lon;
        if (poly[i].lon > max_x) max_x = poly[i].lon;
        if (poly[i].lat < min_y) min_y = poly[i].lat;
        if (poly[i].lat > max_y) max_y = poly[i].lat;
    }
    uint32_t hits = 0;
    uint32_t step = (sample_cnt > 12000) ? (sample_cnt / 12000) : 1;
    for (uint32_t i = 0; i < sample_cnt; i += step) {
        double x = samples[i].lon, y = samples[i].lat;
        if (x < min_x || x > max_x || y < min_y || y > max_y) continue;
        if (IsPointInPolygon(x, y, poly, count)) hits++;
    }
    return hits;
}

static double BBoxPerimeterDist(WayNode p, double min_x, double max_x, double min_y, double max_y) {
    double W = max_x - min_x, H = max_y - min_y;
    if (fabs(p.lat - max_y) < 1e-5) return max_x - p.lon;
    if (fabs(p.lon - min_x) < 1e-5) return W + (max_y - p.lat);
    if (fabs(p.lat - min_y) < 1e-5) return W + H + (p.lon - min_x);
    if (fabs(p.lon - max_x) < 1e-5) return W + H + W + (p.lat - min_y);
    return 0.0;
}

static double CalculateSignedArea(WayNode* nodes, uint32_t count) {
    double area = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t j = (i + 1) % count;
        area += (nodes[j].lon - nodes[i].lon) * (nodes[j].lat + nodes[i].lat);
    }
    return area;
}

static void ReverseWayNodes(WayNode* nodes, uint32_t count) {
    for (uint32_t k = 0; k < count / 2; k++) {
        WayNode tmp = nodes[k];
        nodes[k] = nodes[count - 1 - k];
        nodes[count - 1 - k] = tmp;
    }
}

static bool StitchWithWaterBoundary(AccumWay* coast, AccumWay* water_polys, uint32_t water_poly_cnt, WayNode* road_samples, uint32_t road_sample_cnt) {
    if (coast->count < 2 || water_poly_cnt == 0) return false;

    WayNode start_pt = coast->nodes[0];
    WayNode end_pt = coast->nodes[coast->count - 1];
    double threshold = 0.25 * 0.25;

    int best_score = -1000000;
    WayNode* best_route_overall = NULL;
    uint32_t best_len_overall = 0;

    for (uint32_t i = 0; i < water_poly_cnt; i++) {
        uint32_t p_count = water_polys[i].count;
        if (p_count < 3) continue;

        if (p_count > 3) {
            WayNode w0 = water_polys[i].nodes[0];
            WayNode wN = water_polys[i].nodes[p_count - 1];
            if (fabs(w0.lon - wN.lon) < 1e-7 && fabs(w0.lat - wN.lat) < 1e-7) p_count--;
        }

        double cur_start_dist = 999999.0;
        double cur_end_dist = 999999.0;
        uint32_t cur_start_idx = 0;
        uint32_t cur_end_idx = 0;
        WayNode cur_start_proj = {0, 0.0, 0.0};
        WayNode cur_end_proj = {0, 0.0, 0.0};
        double cur_t_s = 0.0, cur_t_e = 0.0;

        for (uint32_t j = 0; j < p_count; j++) {
            uint32_t j_next = (j + 1) % p_count;
            WayNode A = water_polys[i].nodes[j];
            WayNode B = water_polys[i].nodes[j_next];

            double l2 = (B.lon - A.lon) * (B.lon - A.lon) + (B.lat - A.lat) * (B.lat - A.lat);
            double t_s = 0.0, t_e = 0.0;

            if (l2 > 1e-16) {
                t_s = ((start_pt.lon - A.lon) * (B.lon - A.lon) + (start_pt.lat - A.lat) * (B.lat - A.lat)) / l2;
                t_e = ((end_pt.lon - A.lon) * (B.lon - A.lon) + (end_pt.lat - A.lat) * (B.lat - A.lat)) / l2;
            }

            t_s = (t_s < 0.0) ? 0.0 : ((t_s > 1.0) ? 1.0 : t_s);
            t_e = (t_e < 0.0) ? 0.0 : ((t_e > 1.0) ? 1.0 : t_e);

            WayNode proj_s = {1, A.lat + t_s * (B.lat - A.lat), A.lon + t_s * (B.lon - A.lon)};
            WayNode proj_e = {1, A.lat + t_e * (B.lat - A.lat), A.lon + t_e * (B.lon - A.lon)};

            double sd = (proj_s.lon - start_pt.lon) * (proj_s.lon - start_pt.lon) + (proj_s.lat - start_pt.lat) * (proj_s.lat - start_pt.lat);
            if (sd < cur_start_dist) {
                cur_start_dist = sd; cur_start_idx = j; cur_start_proj = proj_s; cur_t_s = t_s;
            }

            double ed = (proj_e.lon - end_pt.lon) * (proj_e.lon - end_pt.lon) + (proj_e.lat - end_pt.lat) * (proj_e.lat - end_pt.lat);
            if (ed < cur_end_dist) {
                cur_end_dist = ed; cur_end_idx = j; cur_end_proj = proj_e; cur_t_e = t_e;
            }
        }

        if (cur_start_dist > threshold || cur_end_dist > threshold) continue;

        uint32_t seg_start = cur_end_idx;
        uint32_t seg_end = cur_start_idx;
        uint32_t fwd_steps, rev_steps;

        if (seg_start == seg_end) {
            if (cur_t_e <= cur_t_s) { fwd_steps = 0; rev_steps = p_count; }
            else { fwd_steps = p_count; rev_steps = 0; }
        } else {
            fwd_steps = (seg_end > seg_start) ? (seg_end - seg_start) : (seg_end + p_count - seg_start);
            rev_steps = (seg_start > seg_end) ? (seg_start - seg_end) : (seg_start + p_count - seg_end);
        }

        uint32_t fwd_len = fwd_steps + 2;
        uint32_t rev_len = rev_steps + 2;

        WayNode* fwd_route = malloc(fwd_len * sizeof(WayNode));
        fwd_route[0] = cur_end_proj;
        uint32_t idx = (seg_start + 1) % p_count;
        for (uint32_t k = 1; k <= fwd_steps; k++) {
            fwd_route[k] = water_polys[i].nodes[idx];
            fwd_route[k].id = 1;
            idx = (idx + 1) % p_count;
        }
        fwd_route[fwd_len - 1] = cur_start_proj;

        WayNode* rev_route = malloc(rev_len * sizeof(WayNode));
        rev_route[0] = cur_end_proj;
        idx = seg_start;
        for (uint32_t k = 1; k <= rev_steps; k++) {
            rev_route[k] = water_polys[i].nodes[idx];
            rev_route[k].id = 1;
            idx = (idx == 0) ? (p_count - 1) : (idx - 1);
        }
        rev_route[rev_len - 1] = cur_start_proj;

        WayNode* candidates[2] = { fwd_route, rev_route };
        uint32_t cand_lens[2] = { fwd_len, rev_len };

        for (int c = 0; c < 2; c++) {
            if (HasBowtie(coast->nodes, coast->count, candidates[c], cand_lens[c])) continue;

            uint32_t p_cnt = coast->count + cand_lens[c];
            WayNode* poly = malloc(p_cnt * sizeof(WayNode));
            memcpy(poly, coast->nodes, coast->count * sizeof(WayNode));
            memcpy(poly + coast->count, candidates[c], cand_lens[c] * sizeof(WayNode));

            uint32_t roads = CountRoadSamplesInside(poly, p_cnt, road_samples, road_sample_cnt);
            bool is_water = IsPolygonMostlyWater(poly, p_cnt, water_polys, water_poly_cnt);
            double area = CalculateSignedArea(poly, p_cnt);
            free(poly);

            // Reject any stitched polygon that encloses water or has no roads
            if (is_water || (road_sample_cnt > 0 && roads == 0)) continue;

            int score = (int)(roads * 10) + ((area < 0.0) ? 5 : 0) - (int)((cur_start_dist + cur_end_dist) * 100.0);
            if (score > best_score) {
                best_score = score;
                if (best_route_overall) free(best_route_overall);
                best_route_overall = malloc(cand_lens[c] * sizeof(WayNode));
                memcpy(best_route_overall, candidates[c], cand_lens[c] * sizeof(WayNode));
                best_len_overall = cand_lens[c];
            }
        }

        free(fwd_route);
        free(rev_route);
    }

    if (best_route_overall && best_len_overall > 0) {
        WayNode* safe = realloc(coast->nodes, (coast->count + best_len_overall + 1) * sizeof(WayNode));
        if (safe) {
            coast->nodes = safe;
            for (uint32_t k = 0; k < best_len_overall; k++) {
                coast->nodes[coast->count++] = best_route_overall[k];
            }
            WayNode close_pt = coast->nodes[0];
            close_pt.id = 1;
            coast->nodes[coast->count++] = close_pt;
            free(best_route_overall);
            return true;
        }
        free(best_route_overall);
    }
    return false;
}

static void ProjectToBBox(double lon, double lat, double min_x, double max_x, double min_y, double max_y, WayNode* out_node) {
    double dl = fabs(lon - min_x), dr = fabs(lon - max_x);
    double db = fabs(lat - min_y), dt = fabs(lat - max_y);
    double m = dl; int edge = 1; 
    if (db < m) { m = db; edge = 2; }
    if (dr < m) { m = dr; edge = 3; }
    if (dt < m) { m = dt; edge = 0; }
    
    out_node->id = 0;
    if (edge == 1) { out_node->lon = min_x; out_node->lat = lat; }
    else if (edge == 2) { out_node->lon = lon; out_node->lat = min_y; }
    else if (edge == 3) { out_node->lon = max_x; out_node->lat = lat; }
    else if (edge == 0) { out_node->lon = lon; out_node->lat = max_y; }
}

static bool CloseSingleCoastlineNoBowtie(AccumWay* coast, AccumWay* water_polys, uint32_t water_poly_cnt, WayNode* road_samples, uint32_t road_sample_cnt, double min_x, double max_x, double min_y, double max_y) {
    if (coast->count < 2) return false;

    WayNode start_pt = coast->nodes[0];
    WayNode end_pt = coast->nodes[coast->count - 1];
    double gap2 = (start_pt.lon - end_pt.lon) * (start_pt.lon - end_pt.lon) + (start_pt.lat - end_pt.lat) * (start_pt.lat - end_pt.lat);

    if (gap2 <= 1e-10) {
        coast->nodes[coast->count - 1] = start_pt;
        return true;
    }

    if (StitchWithWaterBoundary(coast, water_polys, water_poly_cnt, road_samples, road_sample_cnt)) {
        return true;
    }

    if (coast->count < 64 || gap2 < 0.15 * 0.15) {
        uint32_t roads = CountRoadSamplesInside(coast->nodes, coast->count, road_samples, road_sample_cnt);
        bool is_water = IsPolygonMostlyWater(coast->nodes, coast->count, water_polys, water_poly_cnt);
        if (is_water || (road_sample_cnt > 0 && roads == 0)) {
            coast->merged = true;
            return false;
        }
        WayNode* safe = realloc(coast->nodes, (coast->count + 1) * sizeof(WayNode));
        if (safe) {
            coast->nodes = safe;
            WayNode close_pt = start_pt;
            close_pt.id = 1;
            coast->nodes[coast->count++] = close_pt;
            return true;
        }
        return false;
    }

    WayNode pS, pE;
    ProjectToBBox(start_pt.lon, start_pt.lat, min_x, max_x, min_y, max_y, &pS);
    ProjectToBBox(end_pt.lon, end_pt.lat, min_x, max_x, min_y, max_y, &pE);

    double W = max_x - min_x, H = max_y - min_y, perimeter = 2.0 * W + 2.0 * H;
    double posS = BBoxPerimeterDist(pS, min_x, max_x, min_y, max_y);
    double posE = BBoxPerimeterDist(pE, min_x, max_x, min_y, max_y);

    WayNode corners[4] = { {0, max_y, max_x}, {0, max_y, min_x}, {0, min_y, min_x}, {0, min_y, max_x} };
    double corners_p[4] = { 0.0, W, W + H, 2.0 * W + H };

    WayNode f_rt[12]; uint32_t f_cnt = 0;
    f_rt[f_cnt++] = pE;
    double end_pos_fwd = (posS <= posE) ? posS + perimeter : posS;
    for (int c = 0; c < 8; c++) {
        double cp = corners_p[c % 4] + (c >= 4 ? perimeter : 0.0);
        if (cp > posE && cp < end_pos_fwd) f_rt[f_cnt++] = corners[c % 4];
    }
    f_rt[f_cnt++] = pS;

    WayNode* safe = realloc(coast->nodes, (coast->count + f_cnt + 1) * sizeof(WayNode));
    if (safe) {
        coast->nodes = safe;
        for (uint32_t k = 0; k < f_cnt; k++) {
            WayNode cn = f_rt[k];
            cn.id = 1;
            coast->nodes[coast->count++] = cn;
        }
        WayNode close_pt = coast->nodes[0];
        close_pt.id = 1;
        coast->nodes[coast->count++] = close_pt;
        return true;
    }
    return false;
}
static void AssembleAndCloseCoastlinesOutward(AccumWay* coasts, uint32_t coast_cnt, AccumWay* water_polys, uint32_t water_poly_cnt, WayNode* road_samples, uint32_t road_sample_cnt, double min_x, double max_x, double min_y, double max_y) {
    for (uint32_t seed_step = 0; seed_step < coast_cnt; seed_step++) {
        uint32_t i = (seed_step % 2 == 0)
            ? (coast_cnt / 2 + seed_step / 2) % coast_cnt
            : (coast_cnt / 2 + coast_cnt - (seed_step + 1) / 2) % coast_cnt;

        if (coasts[i].merged || coasts[i].count < 2) continue;

        bool grew = true;
        while (grew) {
            grew = false;
            WayNode head_pt = coasts[i].nodes[0];
            WayNode tail_pt = coasts[i].nodes[coasts[i].count - 1];

            if ((head_pt.lon - tail_pt.lon) * (head_pt.lon - tail_pt.lon) +
                (head_pt.lat - tail_pt.lat) * (head_pt.lat - tail_pt.lat) < 1e-10) {
                coasts[i].nodes[coasts[i].count - 1] = head_pt;
                break;
            }

            double best_tail_d = 1e-6;
            uint32_t best_tail_j = (uint32_t)-1;
            for (uint32_t j = 0; j < coast_cnt; j++) {
                if (i == j || coasts[j].merged || coasts[j].count < 2) continue;
                WayNode jS = coasts[j].nodes[0];
                double d = (jS.lon - tail_pt.lon) * (jS.lon - tail_pt.lon) + (jS.lat - tail_pt.lat) * (jS.lat - tail_pt.lat);
                if (d < best_tail_d) {
                    best_tail_d = d;
                    best_tail_j = j;
                }
            }

            if (best_tail_j != (uint32_t)-1) {
                uint32_t j = best_tail_j;
                uint32_t skip = (best_tail_d < 1e-10) ? 1 : 0;
                uint32_t add_cnt = coasts[j].count - skip;
                WayNode* safe = realloc(coasts[i].nodes, (coasts[i].count + add_cnt) * sizeof(WayNode));
                if (safe) {
                    coasts[i].nodes = safe;
                    memcpy(coasts[i].nodes + coasts[i].count, coasts[j].nodes + skip, add_cnt * sizeof(WayNode));
                    coasts[i].count += add_cnt;
                    coasts[j].merged = true;
                    if (coasts[j].name) { free(coasts[j].name); coasts[j].name = NULL; }
                    if (coasts[j].pcode) { free(coasts[j].pcode); coasts[j].pcode = NULL; }
                    free(coasts[j].nodes); coasts[j].nodes = NULL;
                    grew = true;
                }
            }

            head_pt = coasts[i].nodes[0];
            tail_pt = coasts[i].nodes[coasts[i].count - 1];
            if ((head_pt.lon - tail_pt.lon) * (head_pt.lon - tail_pt.lon) +
                (head_pt.lat - tail_pt.lat) * (head_pt.lat - tail_pt.lat) < 1e-10) {
                coasts[i].nodes[coasts[i].count - 1] = head_pt;
                break;
            }

            double best_head_d = 1e-6;
            uint32_t best_head_j = (uint32_t)-1;
            for (uint32_t j = 0; j < coast_cnt; j++) {
                if (i == j || coasts[j].merged || coasts[j].count < 2) continue;
                WayNode jE = coasts[j].nodes[coasts[j].count - 1];
                double d = (jE.lon - head_pt.lon) * (jE.lon - head_pt.lon) + (jE.lat - head_pt.lat) * (jE.lat - head_pt.lat);
                if (d < best_head_d) {
                    best_head_d = d;
                    best_head_j = j;
                }
            }

            if (best_head_j != (uint32_t)-1) {
                uint32_t j = best_head_j;
                uint32_t skip = (best_head_d < 1e-10) ? 1 : 0;
                uint32_t prepend_cnt = coasts[j].count - skip;
                uint32_t new_cnt = prepend_cnt + coasts[i].count;
                WayNode* safe = malloc(new_cnt * sizeof(WayNode));
                if (safe) {
                    memcpy(safe, coasts[j].nodes, prepend_cnt * sizeof(WayNode));
                    memcpy(safe + prepend_cnt, coasts[i].nodes, coasts[i].count * sizeof(WayNode));
                    free(coasts[i].nodes);
                    coasts[i].nodes = safe;
                    coasts[i].count = new_cnt;
                    coasts[j].merged = true;
                    if (coasts[j].name) { free(coasts[j].name); coasts[j].name = NULL; }
                    if (coasts[j].pcode) { free(coasts[j].pcode); coasts[j].pcode = NULL; }
                    free(coasts[j].nodes); coasts[j].nodes = NULL;
                    grew = true;
                }
            }
        }
    }

    bool bridged = true;
    while (bridged) {
        bridged = false;
        for (uint32_t i = 0; i < coast_cnt; i++) {
            if (coasts[i].merged || coasts[i].count < 2) continue;
            WayNode iS = coasts[i].nodes[0], iE = coasts[i].nodes[coasts[i].count - 1];
            if ((iS.lon - iE.lon) * (iS.lon - iE.lon) + (iS.lat - iE.lat) * (iS.lat - iE.lat) < 1e-8) continue;

            uint32_t best_j = (uint32_t)-1;
            double best_d = 0.15 * 0.15;

            for (uint32_t j = 0; j < coast_cnt; j++) {
                if (i == j || coasts[j].merged || coasts[j].count < 2) continue;
                WayNode jS = coasts[j].nodes[0], jE = coasts[j].nodes[coasts[j].count - 1];
                if ((jS.lon - jE.lon) * (jS.lon - jE.lon) + (jS.lat - jE.lat) * (jS.lat - jE.lat) < 1e-8) continue;

                double d = (iE.lon - jS.lon) * (iE.lon - jS.lon) + (iE.lat - jS.lat) * (iE.lat - jS.lat);
                if (d < best_d) {
                    WayNode link[2] = { iE, jS };
                    bool crosses = false;
                    for (uint32_t k = 0; k + 2 < coasts[i].count; k++) {
                        if (BowtieSegmentsIntersect(link[0], link[1], coasts[i].nodes[k], coasts[i].nodes[k + 1])) { crosses = true; break; }
                    }
                    if (!crosses) {
                        for (uint32_t k = 1; k + 1 < coasts[j].count; k++) {
                            if (BowtieSegmentsIntersect(link[0], link[1], coasts[j].nodes[k], coasts[j].nodes[k + 1])) { crosses = true; break; }
                        }
                    }
                    if (!crosses) {
                        best_d = d;
                        best_j = j;
                    }
                }
            }

            if (best_j != (uint32_t)-1) {
                uint32_t j = best_j;
                uint32_t new_count = coasts[i].count + coasts[j].count;
                WayNode* safe = realloc(coasts[i].nodes, new_count * sizeof(WayNode));
                if (safe) {
                    coasts[i].nodes = safe;
                    memcpy(coasts[i].nodes + coasts[i].count, coasts[j].nodes, coasts[j].count * sizeof(WayNode));
                    coasts[i].count = new_count;
                    coasts[j].merged = true;
                    if (coasts[j].name) { free(coasts[j].name); coasts[j].name = NULL; }
                    if (coasts[j].pcode) { free(coasts[j].pcode); coasts[j].pcode = NULL; }
                    free(coasts[j].nodes); coasts[j].nodes = NULL;
                    bridged = true;
                }
            }
        }
    }

    uint32_t major_idx[256];
    uint32_t major_cnt = 0;
    double W = max_x - min_x, H = max_y - min_y, perimeter = 2.0 * W + 2.0 * H;
    WayNode corners[4] = { {1, max_y, max_x}, {1, max_y, min_x}, {1, min_y, min_x}, {1, min_y, max_x} };
    double corners_p[4] = { 0.0, W, W + H, 2.0 * W + H };

    for (uint32_t i = 0; i < coast_cnt && major_cnt < 256; i++) {
        if (coasts[i].merged || coasts[i].count < 64) continue;
        WayNode s = coasts[i].nodes[0], e = coasts[i].nodes[coasts[i].count - 1];
        double gap2 = (s.lon - e.lon) * (s.lon - e.lon) + (s.lat - e.lat) * (s.lat - e.lat);
        if (gap2 >= 0.15 * 0.15) {
            major_idx[major_cnt++] = i;
        }
    }

    if (major_cnt >= 2) {
        for (uint32_t a = 0; a < major_cnt; a++) {
            for (uint32_t b = a + 1; b < major_cnt; b++) {
                WayNode pSa, pSb;
                ProjectToBBox(coasts[major_idx[a]].nodes[0].lon, coasts[major_idx[a]].nodes[0].lat, min_x, max_x, min_y, max_y, &pSa);
                ProjectToBBox(coasts[major_idx[b]].nodes[0].lon, coasts[major_idx[b]].nodes[0].lat, min_x, max_x, min_y, max_y, &pSb);
                double posA = BBoxPerimeterDist(pSa, min_x, max_x, min_y, max_y);
                double posB = BBoxPerimeterDist(pSb, min_x, max_x, min_y, max_y);
                if (posA > posB) {
                    uint32_t tmp = major_idx[a];
                    major_idx[a] = major_idx[b];
                    major_idx[b] = tmp;
                }
            }
        }

        uint32_t total_alloc = 0;
        for (uint32_t m = 0; m < major_cnt; m++) total_alloc += coasts[major_idx[m]].count + 16;
        WayNode* merged_nodes = malloc(total_alloc * sizeof(WayNode));
        uint32_t merged_cnt = 0;

        for (uint32_t m = 0; m < major_cnt; m++) {
            AccumWay* cur = &coasts[major_idx[m]];
            AccumWay* nxt = &coasts[major_idx[(m + 1) % major_cnt]];

            memcpy(merged_nodes + merged_cnt, cur->nodes, cur->count * sizeof(WayNode));
            merged_cnt += cur->count;

            WayNode pE, pS_next;
            ProjectToBBox(cur->nodes[cur->count - 1].lon, cur->nodes[cur->count - 1].lat, min_x, max_x, min_y, max_y, &pE);
            ProjectToBBox(nxt->nodes[0].lon, nxt->nodes[0].lat, min_x, max_x, min_y, max_y, &pS_next);
            pE.id = 1;
            pS_next.id = 1;

            double posE = BBoxPerimeterDist(pE, min_x, max_x, min_y, max_y);
            double posS_next = BBoxPerimeterDist(pS_next, min_x, max_x, min_y, max_y);

            merged_nodes[merged_cnt++] = pE;
            double end_pos_fwd = (posS_next <= posE) ? posS_next + perimeter : posS_next;
            for (int c = 0; c < 8; c++) {
                double cp = corners_p[c % 4] + (c >= 4 ? perimeter : 0.0);
                if (cp > posE && cp < end_pos_fwd) {
                    merged_nodes[merged_cnt++] = corners[c % 4];
                }
            }
            merged_nodes[merged_cnt++] = pS_next;

            if (m > 0) {
                cur->merged = true;
                if (cur->name) { free(cur->name); cur->name = NULL; }
                if (cur->pcode) { free(cur->pcode); cur->pcode = NULL; }
                free(cur->nodes); cur->nodes = NULL;
            }
        }

        WayNode first_pt = merged_nodes[0];
        first_pt.id = 1;
        merged_nodes[merged_cnt++] = first_pt;

        free(coasts[major_idx[0]].nodes);
        coasts[major_idx[0]].nodes = merged_nodes;
        coasts[major_idx[0]].count = merged_cnt;
    }

    for (uint32_t i = 0; i < coast_cnt; i++) {
        if (coasts[i].merged || coasts[i].count < 2) continue;
        CloseSingleCoastlineNoBowtie(&coasts[i], water_polys, water_poly_cnt, road_samples, road_sample_cnt, min_x, max_x, min_y, max_y);
    }
}

static void LoadConfig() {
    GetPrivateProfileStringA("MapData", "CoastlineShapefile", "", g_ShapefileFile, MAX_PATH, ".\\pbf2qjm.ini");
}

static void CloseCoastlineBox(WayNode** nodes_ptr, uint32_t* count_ptr, double min_x, double max_x, double min_y, double max_y) {
    WayNode* nodes = *nodes_ptr;
    uint32_t cnt = *count_ptr;
    if (cnt < 2) return;
    
    WayNode pS, pE;
    ProjectToBBox(nodes[0].lon, nodes[0].lat, min_x, max_x, min_y, max_y, &pS);
    ProjectToBBox(nodes[cnt-1].lon, nodes[cnt-1].lat, min_x, max_x, min_y, max_y, &pE);
    
    double W = max_x - min_x, H = max_y - min_y;
    double posS = BBoxPerimeterDist(pS, min_x, max_x, min_y, max_y);
    double posE = BBoxPerimeterDist(pE, min_x, max_x, min_y, max_y);
    
    double corners_p[4] = { 0.0, W, W + H, 2 * W + H };
    WayNode corners[4] = { {0, max_y, max_x}, {0, max_y, min_x}, {0, min_y, min_x}, {0, min_y, max_x} };
    WayNode added[10]; uint32_t added_cnt = 0;
    
    added[added_cnt++] = pE;
    if (posS <= posE) posS += (2 * W + 2 * H);
    
    for (int i = 0; i < 8; i++) {
        int c_idx = i % 4;
        double cp = corners_p[c_idx] + (i >= 4 ? (2 * W + 2 * H) : 0.0);
        if (cp > posE && cp < posS) added[added_cnt++] = corners[c_idx];
    }
    added[added_cnt++] = pS;
    
    WayNode* safe = realloc(nodes, (cnt + added_cnt + 1) * sizeof(WayNode));
    if (safe) {
        for (uint32_t i = 0; i < added_cnt; i++) safe[cnt + i] = added[i];
        safe[cnt + added_cnt] = safe[0];
        *nodes_ptr = safe; *count_ptr = cnt + added_cnt + 1;
    }
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
        ".nodes.tmp", ".raw.tmp", ".coasts.tmp",
        ".sort.tmp", ".sorted.tmp" // Added sort temp files
    };
    int removed_cnt = 0;
    for (int i = 0; i < 8; i++) {
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

static void LoadReferenceMap(const char* qjm_path) {
    if (!qjm_path || strlen(qjm_path) == 0) return;
    FILE* f = fopen(qjm_path, "rb");
    if (!f) return;

    FSEEK64(f, -24, SEEK_END);
    uint64_t high_dir_off, over_dir_off;
    uint32_t high_cnt, over_cnt;
    char magic[4];
    
    if (fread(&high_dir_off, 8, 1, f) != 1 || fread(&high_cnt, 4, 1, f) != 1 ||
        fread(&over_dir_off, 8, 1, f) != 1 || fread(&over_cnt, 4, 1, f) != 1 ||
        fread(magic, 1, 4, f) != 1 || strncmp(magic, "QJM2", 4) != 0) {
        fclose(f); return;
    }

    uint64_t dir_off = over_cnt > 0 ? over_dir_off : high_dir_off;
    uint32_t chunk_cnt = over_cnt > 0 ? over_cnt : high_cnt;

    ChunkMetaDisk* meta = malloc(chunk_cnt * sizeof(ChunkMetaDisk));
    FSEEK64(f, dir_off, SEEK_SET);
    fread(meta, sizeof(ChunkMetaDisk), chunk_cnt, f);

    uint32_t ref_cap = 1000;
    g_RefPolys = malloc(ref_cap * sizeof(RefPoly));
    g_RefPolyCount = 0;

    for (uint32_t i = 0; i < chunk_cnt; i++) {
        FSEEK64(f, meta[i].offset, SEEK_SET);
        uint8_t* comp = malloc(meta[i].comp_size);
        uint8_t* uncomp = malloc(meta[i].uncomp_size);
        
        fread(comp, 1, meta[i].comp_size, f);
        if (lzav_decompress(comp, uncomp, (int)meta[i].comp_size, (int)meta[i].uncomp_size) > 0) {
            uint32_t f_cnt = *(uint32_t*)uncomp;
            PFeat* feats = (PFeat*)(uncomp + 12);
            float* pts = (float*)(uncomp + 12 + f_cnt * sizeof(PFeat));

            for (uint32_t j = 0; j < f_cnt; j++) {
                if ((feats[j].fc == CLASS_LAND || feats[j].fc == CLASS_COASTLINE) && feats[j].pt_cnt > 2) {
                    if (g_RefPolyCount >= ref_cap) {
                        ref_cap *= 2;
                        g_RefPolys = realloc(g_RefPolys, ref_cap * sizeof(RefPoly));
                    }
                    RefPoly* rp = &g_RefPolys[g_RefPolyCount++];
                    rp->count = feats[j].pt_cnt;
                    rp->nodes = malloc(rp->count * sizeof(WayNode));
                    float* feat_pts = pts + feats[j].pt_off;
                    for (uint32_t k = 0; k < rp->count; k++) {
                        rp->nodes[k].lon = feat_pts[k * 2];
                        rp->nodes[k].lat = feat_pts[k * 2 + 1];
                    }
                }
            }
        }
        free(comp); free(uncomp);
    }
    free(meta); fclose(f);
    SetStatus("Loaded %u reference polygons for stitching.", g_RefPolyCount);
}

// --- NEW EXTERNAL MERGE SORT FUNCTIONS FOR PASS 5 ---
void Heapify(HeapNode* heap, int size, int i) {
    int smallest = i;
    int left = 2 * i + 1, right = 2 * i + 2;
    if (left < size && heap[left].entry.morton < heap[smallest].entry.morton) smallest = left;
    if (right < size && heap[right].entry.morton < heap[smallest].entry.morton) smallest = right;
    if (smallest != i) {
        HeapNode temp = heap[i]; heap[i] = heap[smallest]; heap[smallest] = temp;
        Heapify(heap, size, smallest);
    }
}

bool ExternalMergeSort(const char* sort_tmp_name, const char* sorted_tmp_name) {
    FILE* f_in = fopen(sort_tmp_name, "rb"); if (!f_in) return false;
    uint32_t chunk_size = 5000000; 
    SortEntry* chunk = malloc(chunk_size * sizeof(SortEntry)); if (!chunk) return false;
    int num_chunks = 0; uint64_t read_feats; char chunk_name[1024];

    SetStatus("Pass 5: Splitting into sorted chunks on disk...");
    while ((read_feats = fread(chunk, sizeof(SortEntry), chunk_size, f_in)) > 0) {
        qsort(chunk, read_feats, sizeof(SortEntry), CompareSortEntry);
        snprintf(chunk_name, sizeof(chunk_name), "%s.chunk%d", sort_tmp_name, num_chunks);
        FILE* f_out = fopen(chunk_name, "wb"); fwrite(chunk, sizeof(SortEntry), read_feats, f_out); fclose(f_out);
        num_chunks++;
    }
    free(chunk); fclose(f_in);

    SetStatus("Pass 5: K-Way Merge %d chunks into final list...", num_chunks);
    FILE** chunk_files = malloc(num_chunks * sizeof(FILE*));
    HeapNode* heap = malloc(num_chunks * sizeof(HeapNode));
    int heap_size = 0;

    for (int i = 0; i < num_chunks; i++) {
        snprintf(chunk_name, sizeof(chunk_name), "%s.chunk%d", sort_tmp_name, i);
        chunk_files[i] = fopen(chunk_name, "rb");
        if (fread(&heap[heap_size].entry, sizeof(SortEntry), 1, chunk_files[i]) == 1) {
            heap[heap_size].chunk_idx = i; heap_size++;
        }
    }
    for (int i = heap_size / 2 - 1; i >= 0; i--) Heapify(heap, heap_size, i);

    FILE* f_sorted = fopen(sorted_tmp_name, "wb");
    char* out_buf = malloc(IO_BUF_SIZE); if (f_sorted && out_buf) setvbuf(f_sorted, out_buf, _IOFBF, IO_BUF_SIZE);

    while (heap_size > 0) {
        fwrite(&heap[0].entry, sizeof(SortEntry), 1, f_sorted);
        int c_idx = heap[0].chunk_idx;
        if (fread(&heap[0].entry, sizeof(SortEntry), 1, chunk_files[c_idx]) == 1) Heapify(heap, heap_size, 0);
        else { heap[0] = heap[heap_size - 1]; heap_size--; Heapify(heap, heap_size, 0); }
    }
    fclose(f_sorted); if (out_buf) free(out_buf);

    for (int i = 0; i < num_chunks; i++) {
        fclose(chunk_files[i]); snprintf(chunk_name, sizeof(chunk_name), "%s.chunk%d", sort_tmp_name, i); remove(chunk_name);
    }
    free(chunk_files); free(heap);
    return true;
}

// --- UPDATED WriteRawFeature DIRECTLY TO DISK ---
static void WriteRawFeature(FILE* f_raw, WayNode* wnodes, uint32_t ref_count, bool is_area, const char* name, const char* pcode, uint8_t fc, FILE* f_sort, uint64_t* cnt) {
    if (!f_raw || !f_sort || !wnodes || ref_count == 0) return;

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

    SortEntry entry;
    entry.morton = EncodeMorton(c_ix, c_iy);
    entry.offset = offset;
    fwrite(&entry, sizeof(SortEntry), 1, f_sort);
    (*cnt)++;
}

static uint32_t DecimateNodes(WayNode* in_nodes, uint32_t in_cnt, WayNode* out_nodes, int keep_percent) {
    if (in_cnt <= 10 || keep_percent >= 100) {
        for(uint32_t i = 0; i < in_cnt; i++) out_nodes[i] = in_nodes[i];
        return in_cnt;
    }

    uint32_t target_cnt = (uint32_t)(in_cnt * (keep_percent / 100.0));
    if (target_cnt < 4) target_cnt = 4; 

    if (target_cnt >= in_cnt) {
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
// --- UPDATED ProcessShapefile USING FILE* f_sort ---
static void ProcessShapefile(const char* shp_path, FILE* f_raw, FILE* f_sort, uint64_t* cnt, bool has_bbox, double b_minLon, double b_minLat, double b_maxLon, double b_maxLat) {
    FILE* f = fopen(shp_path, "rb");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 100, SEEK_SET);

    while (ftell(f) < file_size) {
        uint32_t rec_num, rec_len;
        if (fread(&rec_num, 4, 1, f) != 1) break;
        if (fread(&rec_len, 4, 1, f) != 1) break;
        rec_len = SwapEndian32(rec_len) * 2; 
        
        long next_rec = ftell(f) + rec_len;
        uint32_t shape_type;
        if (fread(&shape_type, 4, 1, f) != 1) break; 
        
        if (shape_type == 3 || shape_type == 5) { 
            double box[4];
            if (fread(box, sizeof(double), 4, f) != 4) break; 
            
            uint32_t num_parts, num_points;
            if (fread(&num_parts, 4, 1, f) != 1) break;
            if (fread(&num_points, 4, 1, f) != 1) break;
            
            uint32_t* parts = malloc(num_parts * 4);
            fread(parts, 4, num_parts, f);
            
            WayNode* all_points = malloc(num_points * sizeof(WayNode));
            for (uint32_t i = 0; i < num_points; i++) {
                double pt[2];
                fread(pt, sizeof(double), 2, f);
                all_points[i].lon = pt[0];
                all_points[i].lat = pt[1];
                all_points[i].id = 0xFFFFFFFFFFFFFFFFULL;
            }
            
            for (uint32_t i = 0; i < num_parts; i++) {
                uint32_t start = parts[i];
                uint32_t end = (i == num_parts - 1) ? num_points : parts[i + 1];
                uint32_t ref_count = end - start;
                if (ref_count >= 2) {
                    WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
                    for (uint32_t j = 0; j < ref_count; j++) wnodes[j] = all_points[start + j];
                    
                    bool in_bbox = true;
                    if (has_bbox && (box[2] < b_minLon || box[0] > b_maxLon || box[3] < b_minLat || box[1] > b_maxLat)) in_bbox = false;
                    
                    if (in_bbox) {
                        WriteRawFeature(f_raw, wnodes, ref_count, (shape_type == 5), "Coastline", NULL, CLASS_COASTLINE, f_sort, cnt);
                    }
                    free(wnodes);
                }
            }
            free(parts);
            free(all_points);
        }
        fseek(f, next_rec, SEEK_SET); 
    }
    fclose(f);
}

static int RunCompilation(const char* in_filename, const char* out_filename, const char* ref_filename, bool has_bbox, double b_minLon, double b_minLat, double b_maxLon, double b_maxLat) {
    (void)ref_filename;

    char ways_tmp_a[1024]; snprintf(ways_tmp_a, sizeof(ways_tmp_a), "%s.ways.A.tmp", out_filename);
    char ways_tmp_b[1024]; snprintf(ways_tmp_b, sizeof(ways_tmp_b), "%s.ways.B.tmp", out_filename);
    char ways_resolved_name[1024]; snprintf(ways_resolved_name, sizeof(ways_resolved_name), "%s.ways.resolved.tmp", out_filename);
    char nodes_tmp_name[1024]; snprintf(nodes_tmp_name, sizeof(nodes_tmp_name), "%s.nodes.tmp", out_filename);
    char raw_tmp_name[1024]; snprintf(raw_tmp_name, sizeof(raw_tmp_name), "%s.raw.tmp", out_filename);
    char coasts_tmp_name[1024]; snprintf(coasts_tmp_name, sizeof(coasts_tmp_name), "%s.coasts.tmp", out_filename);
    char sort_tmp_name[1024]; snprintf(sort_tmp_name, sizeof(sort_tmp_name), "%s.sort.tmp", out_filename);
    char sorted_tmp_name[1024]; snprintf(sorted_tmp_name, sizeof(sorted_tmp_name), "%s.sorted.tmp", out_filename);

    uint64_t written_feats = 0; FILE* f_raw = NULL; bool resumed = false; bool is_main_input_shp = false;
    const char* ext_in = strrchr(in_filename, '.'); if (ext_in && strcasecmp(ext_in, ".shp") == 0) is_main_input_shp = true;
    uint32_t EOF_MARKER = 0xDEADBEEF;

    f_raw = fopen(raw_tmp_name, "rb");
    if (f_raw) {
        FSEEK64(f_raw, 0, SEEK_END); int64_t raw_sz = FTELL64(f_raw);
        if (raw_sz >= 4) {
            FSEEK64(f_raw, -4, SEEK_END);
            uint32_t marker = 0; fread(&marker, 4, 1, f_raw);
            if (marker == EOF_MARKER) {
                SetStatus("Resuming from completed %s (Pass 5)...", raw_tmp_name);
                FSEEK64(f_raw, 0, SEEK_SET);
                FILE* f_sort = fopen(sort_tmp_name, "wb");
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
                    SortEntry entry; entry.morton = EncodeMorton(c_ix, c_iy); entry.offset = offset;
                    fwrite(&entry, sizeof(SortEntry), 1, f_sort); written_feats++;
                }
                free(wnodes); fclose(f_sort); resumed = true;
            }
        }
        if (!resumed) { fclose(f_raw); f_raw = NULL; }
    }

    bool resume_coasts = false;
    uint32_t coast_cap = 0, coast_cnt = 0; AccumWay* coasts = NULL;
    uint32_t road_sample_cap = 500000; uint32_t road_sample_cnt = 0; WayNode* road_samples = malloc(road_sample_cap * sizeof(WayNode));

    if (!resumed && !is_main_input_shp) {
        FILE* f_cst_in = fopen(coasts_tmp_name, "rb");
        if (f_cst_in) {
            int64_t safe_raw_sz = 0;
            if (fread(&safe_raw_sz, 8, 1, f_cst_in) == 1 && fread(&coast_cnt, 4, 1, f_cst_in) == 1) {
                SetStatus("Resuming Polygon Assembly from %s (%u ways)...", coasts_tmp_name, coast_cnt);
                f_raw = fopen(raw_tmp_name, "rb");
                if (f_raw) {
                    FILE* f_sort = fopen(sort_tmp_name, "wb");
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

                        if (fc == CLASS_HWY_MAIN || fc == CLASS_HWY_MINOR) {
                            for (uint32_t s_idx = 0; s_idx < ref_count; s_idx += 50) {
                                if (road_sample_cnt >= road_sample_cap) {
                                    road_sample_cap *= 2; road_samples = realloc(road_samples, road_sample_cap * sizeof(WayNode));
                                } road_samples[road_sample_cnt++] = wnodes[s_idx];
                            }
                        }

                        double min_lon = wnodes[0].lon, max_lon = wnodes[0].lon, min_lat = wnodes[0].lat, max_lat = wnodes[0].lat;
                        for(uint32_t i=1; i<ref_count; i++) {
                            if(wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if(wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                            if(wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if(wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                        }
                        double cx = (min_lon + max_lon) / 2.0, cy = (min_lat + max_lat) / 2.0;
                        int32_t c_ix = (int32_t)((cx + 180.0) / 360.0 * 0x3FFFFFFF), c_iy = (int32_t)((cy + 90.0) / 180.0 * 0x3FFFFFFF);
                        SortEntry entry; entry.morton = EncodeMorton(c_ix, c_iy); entry.offset = offset;
                        fwrite(&entry, sizeof(SortEntry), 1, f_sort); written_feats++;
                    }
                    free(wnodes); fclose(f_sort); fclose(f_raw);
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
                    free(buf); fclose(f_old); fclose(f_new); remove(raw_tmp_name); rename("temp_truncate.tmp", raw_tmp_name);
                }
                f_raw = fopen(raw_tmp_name, "ab");
                FILE* f_sort = fopen(sort_tmp_name, "ab");

                uint32_t open_cnt = 0; coast_cap = 65536; coasts = malloc(coast_cap * sizeof(AccumWay));
                uint32_t r_cap = 65536; WayNode* tmp_nodes = malloc(r_cap * sizeof(WayNode));

                for (uint32_t i = 0; i < coast_cnt; i++) {
                    uint8_t nlen, plen, fc; char name_buf[256] = {0}, pcode_buf[256] = {0};
                    if (fread(&nlen, 1, 1, f_cst_in) != 1) break;
                    if (nlen) { fread(name_buf, 1, nlen, f_cst_in); name_buf[nlen] = 0; }
                    if (fread(&plen, 1, 1, f_cst_in) != 1) break;
                    if (plen) { fread(pcode_buf, 1, plen, f_cst_in); pcode_buf[plen] = 0; }
                    uint32_t cnt = 0; if (fread(&cnt, 4, 1, f_cst_in) != 1) break;
                    if (cnt > r_cap) { r_cap = cnt + 16384; tmp_nodes = realloc(tmp_nodes, r_cap * sizeof(WayNode)); }
                    if (fread(tmp_nodes, sizeof(WayNode), cnt, f_cst_in) != cnt) break;
                    if (fread(&fc, 1, 1, f_cst_in) != 1) break;

                    if (cnt < 2) continue;
                    double dx = tmp_nodes[0].lon - tmp_nodes[cnt-1].lon, dy = tmp_nodes[0].lat - tmp_nodes[cnt-1].lat;
                    bool is_closed = (fabs(dx) < 1e-6 && fabs(dy) < 1e-6);

                    if (is_closed) {
                        tmp_nodes[cnt-1] = tmp_nodes[0];
                        double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                        for (uint32_t j = 0; j < cnt; j++) {
                            if (tmp_nodes[j].lon < min_lon) min_lon = tmp_nodes[j].lon; if (tmp_nodes[j].lon > max_lon) max_lon = tmp_nodes[j].lon;
                            if (tmp_nodes[j].lat < min_lat) min_lat = tmp_nodes[j].lat; if (tmp_nodes[j].lat > max_lat) max_lat = tmp_nodes[j].lat;
                        }
                        bool in_bbox = true; if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                        if (!g_CfgSpecs && (max_lon - min_lon < 0.005) && (max_lat - min_lat < 0.005)) in_bbox = false;
                        if (in_bbox) WriteRawFeature(f_raw, tmp_nodes, cnt, true, nlen ? name_buf : NULL, plen ? pcode_buf : NULL, fc, f_sort, &written_feats);
                    } else {
                        if (open_cnt >= coast_cap) { coast_cap *= 2; coasts = realloc(coasts, coast_cap * sizeof(AccumWay)); }
                        coasts[open_cnt].name = nlen ? strcpy(malloc(nlen + 1), name_buf) : NULL;
                        coasts[open_cnt].pcode = plen ? strcpy(malloc(plen + 1), pcode_buf) : NULL;
                        coasts[open_cnt].count = cnt; coasts[open_cnt].nodes = malloc(cnt * sizeof(WayNode));
                        for (uint32_t k = 0; k < cnt; k++) { coasts[open_cnt].nodes[k].id = 0; coasts[open_cnt].nodes[k].lat = tmp_nodes[k].lat; coasts[open_cnt].nodes[k].lon = tmp_nodes[k].lon; }
                        coasts[open_cnt].fc = fc; coasts[open_cnt].merged = false; open_cnt++;
                    }
                }
                free(tmp_nodes); fclose(f_cst_in); fclose(f_sort);
                coast_cnt = open_cnt; resume_coasts = true;
            } else fclose(f_cst_in);
        }

        if (is_main_input_shp) {
            SetStatus("Pass 1-4: Extracting ESRI Shapefile geometries...");
            f_raw = fopen(raw_tmp_name, "wb"); FILE* f_sort = fopen(sort_tmp_name, "wb");
            if (!f_raw || !f_sort) { free(road_samples); return 1; }
            ProcessShapefile(in_filename, f_raw, f_sort, &written_feats, has_bbox, b_minLon, b_minLat, b_maxLon, b_maxLat);
            fwrite(&EOF_MARKER, 4, 1, f_raw); fclose(f_raw); fclose(f_sort); f_raw = fopen(raw_tmp_name, "rb");
        } else {
            if (!resume_coasts) {
                char* current_ways = ways_tmp_a; char* next_ways = ways_tmp_b;
                bool has_resolved_ways = (GetFileSize64(ways_resolved_name) > 0);
                if (has_resolved_ways) { SetStatus("Found %s! Resuming at Pass 4...", ways_resolved_name); current_ways = ways_resolved_name; }
                else {
                    bool has_ways_a = (GetFileSize64(ways_tmp_a) > 0), has_ways_b = (GetFileSize64(ways_tmp_b) > 0), has_nodes = (GetFileSize64(nodes_tmp_name) > 0);
                    if (!has_ways_a && has_ways_b) { current_ways = ways_tmp_b; next_ways = ways_tmp_a; has_ways_a = true; }

                    for (int extract_mode = 1; extract_mode <= 2; extract_mode++) {
                        if (extract_mode == 1 && has_ways_a) { SetStatus("Pass 1: Reusing existing ways temp file..."); continue; }
                        if (extract_mode == 2 && has_nodes) { SetStatus("Pass 2: Reusing existing nodes temp file..."); continue; }

                        FILE* tmpOut = NULL; char* io_buf = malloc(IO_BUF_SIZE);
                        if (extract_mode == 1) { SetStatus("Pass 1: Streaming Filtered Ways to Disk..."); tmpOut = fopen(current_ways, "wb"); }
                        else { SetStatus("Pass 2: Streaming All Nodes to Disk..."); tmpOut = fopen(nodes_tmp_name, "wb"); }
                        if (tmpOut && io_buf) setvbuf(tmpOut, io_buf, _IOFBF, IO_BUF_SIZE);

                        FILE* file = fopen(in_filename, "rb"); if (!file) { if (tmpOut) fclose(tmpOut); if (io_buf) free(io_buf); free(road_samples); return 1; }
                        char* read_buf = malloc(IO_BUF_SIZE); if (read_buf) setvbuf(file, read_buf, _IOFBF, IO_BUF_SIZE);
                        
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
                                ptr = blob_buf; end = blob_buf + datasize; uint64_t raw_size = 0, zlib_size = 0, raw_len = 0; uint8_t *zlib_data = NULL, *raw_data = NULL;
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
                    FILE* fnodes = fopen(nodes_tmp_name, "rb"); if (!fnodes) { free(road_samples); return 1; }
                    char* io_node_buf = malloc(IO_BUF_SIZE); if (io_node_buf) setvbuf(fnodes, io_node_buf, _IOFBF, IO_BUF_SIZE);
                    const uint64_t CHUNK_NODES = 15000000; PbfNode* chunk = malloc(CHUNK_NODES * sizeof(PbfNode));
                    if (!chunk) { fclose(fnodes); if (io_node_buf) free(io_node_buf); free(road_samples); return 1; }

                    uint64_t nodes_read; int node_chunk_idx = 1;
                    while ((nodes_read = fread(chunk, sizeof(PbfNode), CHUNK_NODES, fnodes)) > 0) {
                        SetStatus("Pass 3: Resolving Node Chunk %d...", node_chunk_idx++);
                        qsort(chunk, nodes_read, sizeof(PbfNode), CompareNodes);
                        FILE* fways_in = fopen(current_ways, "rb"); FILE* fways_out = fopen(next_ways, "wb");
                        char* in_buf = malloc(IO_BUF_SIZE); char* out_buf = malloc(IO_BUF_SIZE);
                        if (fways_in && in_buf) setvbuf(fways_in, in_buf, _IOFBF, IO_BUF_SIZE); if (fways_out && out_buf) setvbuf(fways_out, out_buf, _IOFBF, IO_BUF_SIZE);
                        if(!fways_in || !fways_out) { if (in_buf) free(in_buf); if (out_buf) free(out_buf); break; }

                        while (1) {
                            uint8_t fc; if (fread(&fc, 1, 1, fways_in) != 1) break;
                            uint16_t nlen; if (fread(&nlen, 2, 1, fways_in) != 1) break;
                            char* name = NULL; if (nlen > 0) { name = malloc(nlen + 1); if (name) { fread(name, 1, nlen, fways_in); name[nlen] = '\0'; } else FSEEK64(fways_in, nlen, SEEK_CUR); }
                            uint16_t plen; if (fread(&plen, 2, 1, fways_in) != 1) break;
                            char* pcode = NULL; if (plen > 0) { pcode = malloc(plen + 1); if (pcode) { fread(pcode, 1, plen, fways_in); pcode[plen] = '\0'; } else FSEEK64(fways_in, plen, SEEK_CUR); }
                            uint32_t ref_count; if (fread(&ref_count, 4, 1, fways_in) != 1) break;
                            if (ref_count == 0 || ref_count > 5000000) { if (name) free(name); if (pcode) free(pcode); break; }

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
                        fclose(fways_in); fclose(fways_out); if (in_buf) free(in_buf); if (out_buf) free(out_buf);
                        char* tmp = current_ways; current_ways = next_ways; next_ways = tmp;
                    }
                    free(chunk); fclose(fnodes); if (io_node_buf) free(io_node_buf);
                    remove(ways_resolved_name); if (rename(current_ways, ways_resolved_name) == 0) current_ways = ways_resolved_name;
                }

                SetStatus("Pass 4: Extracting raw features to temporary file...");
                FILE* fways = fopen(current_ways, "rb"); f_raw = fopen(raw_tmp_name, "wb"); FILE* f_sort = fopen(sort_tmp_name, "wb");
                char* io_final_in = malloc(IO_BUF_SIZE); if (fways && io_final_in) setvbuf(fways, io_final_in, _IOFBF, IO_BUF_SIZE);
                if (!fways || !f_raw || !f_sort) { if (io_final_in) free(io_final_in); free(road_samples); return 1; }

                while (1) {
                    uint8_t fc; if (fread(&fc, 1, 1, fways) != 1) break;
                    uint16_t nlen; if (fread(&nlen, 2, 1, fways) != 1) break;
                    char* name = NULL; if (nlen > 0) { name = malloc(nlen + 1); if (name) { fread(name, 1, nlen, fways); name[nlen] = '\0'; } else FSEEK64(fways, nlen, SEEK_CUR); }
                    uint16_t plen; if (fread(&plen, 2, 1, fways) != 1) break;
                    char* pcode = NULL; if (plen > 0) { pcode = malloc(plen + 1); if (pcode) { fread(pcode, 1, plen, fways); pcode[plen] = '\0'; } else FSEEK64(fways, plen, SEEK_CUR); }
                    uint32_t ref_count; if (fread(&ref_count, 4, 1, fways) != 1) break;
                    if (ref_count == 0 || ref_count > 5000000) { if (name) free(name); if (pcode) free(pcode); break; }

                    WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
                    if (!wnodes) { if (name) free(name); if (pcode) free(pcode); break; }
                    if (fread(wnodes, sizeof(WayNode), ref_count, fways) != ref_count) { if (name) free(name); if (pcode) free(pcode); free(wnodes); break; }

                    bool valid = true;
                    for (uint32_t i = 0; i < ref_count; i++) { if (wnodes[i].id != 0xFFFFFFFFFFFFFFFFULL) { valid = false; break; } }

                    if (valid && ref_count >= 2) {
                        if (fc == CLASS_HWY_MAIN || fc == CLASS_HWY_MINOR) {
                            for (uint32_t s_idx = 0; s_idx < ref_count; s_idx += 50) {
                                if (road_sample_cnt >= road_sample_cap) {
                                    uint32_t new_cap = road_sample_cap * 2; WayNode* safe = realloc(road_samples, new_cap * sizeof(WayNode));
                                    if (!safe) break; road_samples = safe; road_sample_cap = new_cap;
                                } road_samples[road_sample_cnt++] = wnodes[s_idx];
                            }
                        }

                        if (fc == CLASS_COASTLINE) {
                            double dx = wnodes[0].lon - wnodes[ref_count-1].lon, dy = wnodes[0].lat - wnodes[ref_count-1].lat;
                            bool is_closed = (ref_count >= 4) && (fabs(dx) < 1e-6 && fabs(dy) < 1e-6);

                            if (is_closed) {
                                wnodes[ref_count-1] = wnodes[0];
                                double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                                for (uint32_t i = 0; i < ref_count; i++) {
                                    if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                                    if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                                }
                                bool in_bbox = true; if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                                if (!g_CfgSpecs && (max_lon - min_lon < 0.005) && (max_lat - min_lat < 0.005)) in_bbox = false;
                                if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, true, name, pcode, CLASS_LAND, f_sort, &written_feats);
                                if (name) free(name); if (pcode) free(pcode); free(wnodes);
                            } else {
                                if (coast_cnt >= coast_cap) { coast_cap = coast_cap == 0 ? 65536 : coast_cap * 2; coasts = realloc(coasts, coast_cap * sizeof(AccumWay)); }
                                coasts[coast_cnt].name = name; coasts[coast_cnt].pcode = pcode; coasts[coast_cnt].count = ref_count; coasts[coast_cnt].nodes = wnodes; coasts[coast_cnt].merged = false; coasts[coast_cnt].fc = fc; coast_cnt++;
                            }
                        } else if (fc == CLASS_WATER || fc == CLASS_PARK || fc == CLASS_LAND) {
                            double dx = wnodes[0].lon - wnodes[ref_count-1].lon, dy = wnodes[0].lat - wnodes[ref_count-1].lat;
                            bool is_closed = (ref_count >= 4) && (fabs(dx) < 1e-6 && fabs(dy) < 1e-6);
                            if (is_closed) wnodes[ref_count-1] = wnodes[0];
                            double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                            for (uint32_t i = 0; i < ref_count; i++) {
                                if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                                if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                            }
                            bool in_bbox = true; if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                            if (is_closed && !g_CfgSpecs && (max_lon - min_lon < 0.005) && (max_lat - min_lat < 0.005)) in_bbox = false;
                            if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, is_closed, name, pcode, fc, f_sort, &written_feats);
                            if (name) free(name); if (pcode) free(pcode); free(wnodes);
                        } else {
                            double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                            for (uint32_t i = 0; i < ref_count; i++) {
                                if (wnodes[i].lon < min_lon) min_lon = wnodes[i].lon; if (wnodes[i].lon > max_lon) max_lon = wnodes[i].lon;
                                if (wnodes[i].lat < min_lat) min_lat = wnodes[i].lat; if (wnodes[i].lat > max_lat) max_lat = wnodes[i].lat;
                            }
                            bool in_bbox = true; if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                            if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, false, name, pcode, fc, f_sort, &written_feats);
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
                            uint8_t nlen = coasts[i].name ? (uint8_t)strlen(coasts[i].name) : 0, plen = coasts[i].pcode ? (uint8_t)strlen(coasts[i].pcode) : 0;
                            fwrite(&nlen, 1, 1, f_cst_out); if (nlen) fwrite(coasts[i].name, 1, nlen, f_cst_out);
                            fwrite(&plen, 1, 1, f_cst_out); if (plen) fwrite(coasts[i].pcode, 1, plen, f_cst_out);
                            fwrite(&coasts[i].count, 4, 1, f_cst_out); fwrite(coasts[i].nodes, sizeof(WayNode), coasts[i].count, f_cst_out); fwrite(&coasts[i].fc, 1, 1, f_cst_out);
                        }
                        fclose(f_cst_out);
                    }
                }
                fclose(fways); if (io_final_in) free(io_final_in); fclose(f_sort);
            }

            if (coast_cnt > 0) {
                SetStatus("Pass 4b: Outward Seed-and-Grow Coastline Assembly (%u ways)...", coast_cnt);
                double ext_min_lon = 180.0, ext_max_lon = -180.0, ext_min_lat = 90.0, ext_max_lat = -90.0;
                for (uint32_t i = 0; i < coast_cnt; i++) {
                    if (coasts[i].merged) continue;
                    for (uint32_t j = 0; j < coasts[i].count; j++) {
                        if (coasts[i].nodes[j].lon < ext_min_lon) ext_min_lon = coasts[i].nodes[j].lon;
                        if (coasts[i].nodes[j].lon > ext_max_lon) ext_max_lon = coasts[i].nodes[j].lon;
                        if (coasts[i].nodes[j].lat < ext_min_lat) ext_min_lat = coasts[i].nodes[j].lat;
                        if (coasts[i].nodes[j].lat > ext_max_lat) ext_max_lat = coasts[i].nodes[j].lat;
                    }
                }
                for (uint32_t i = 0; i < road_sample_cnt; i++) {
                    if (road_samples[i].lon < ext_min_lon) ext_min_lon = road_samples[i].lon;
                    if (road_samples[i].lon > ext_max_lon) ext_max_lon = road_samples[i].lon;
                    if (road_samples[i].lat < ext_min_lat) ext_min_lat = road_samples[i].lat;
                    if (road_samples[i].lat > ext_max_lat) ext_max_lat = road_samples[i].lat;
                }
                ext_min_lon -= 0.002; ext_max_lon += 0.002; ext_min_lat -= 0.002; ext_max_lat += 0.002;

                AssembleAndCloseCoastlinesOutward(coasts, coast_cnt, NULL, 0, road_samples, road_sample_cnt, ext_min_lon, ext_max_lon, ext_min_lat, ext_max_lat);

                FILE* f_sort = fopen(sort_tmp_name, "ab");
                for (uint32_t i = 0; i < coast_cnt; i++) {
                    if (coasts[i].merged || coasts[i].count < 3) {
                        if (coasts[i].name) free(coasts[i].name); if (coasts[i].pcode) free(coasts[i].pcode); if (coasts[i].nodes) free(coasts[i].nodes);
                        continue;
                    }
                    uint8_t out_fc = (coasts[i].fc == CLASS_COASTLINE) ? CLASS_LAND : coasts[i].fc;
                    WriteRawFeature(f_raw, coasts[i].nodes, coasts[i].count, true, coasts[i].name, coasts[i].pcode, out_fc, f_sort, &written_feats);
                    if (coasts[i].name) free(coasts[i].name); if (coasts[i].pcode) free(coasts[i].pcode); free(coasts[i].nodes);
                }
                if (f_sort) fclose(f_sort);
                free(coasts);
            }

            if (strlen(g_ShapefileFile) > 0) {
                SetStatus("  -> Injecting Shapefile Coastlines from %s...", g_ShapefileFile);
                FILE* f_sort = fopen(sort_tmp_name, "ab");
                ProcessShapefile(g_ShapefileFile, f_raw, f_sort, &written_feats, has_bbox, b_minLon, b_minLat, b_maxLon, b_maxLat);
                if (f_sort) fclose(f_sort);
            }

            fwrite(&EOF_MARKER, 4, 1, f_raw); fclose(f_raw); f_raw = fopen(raw_tmp_name, "rb");
        }
    }
    free(road_samples);

    SetStatus("Pass 5: External Spatial Clustering (Morton Z-Curve Sorting)...");
    if (!ExternalMergeSort(sort_tmp_name, sorted_tmp_name)) {
        SetStatus("Fatal Error: External sorting failed.");
        return 1;
    }

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

    FILE* f_sorted = fopen(sorted_tmp_name, "rb");
    if (!f_sorted) return 1;

    for (uint64_t i = 0; i < written_feats; i++) {
        SortEntry entry; fread(&entry, sizeof(SortEntry), 1, f_sorted);
        FSEEK64(f_raw, entry.offset, SEEK_SET); uint32_t total_len; fread(&total_len, 4, 1, f_raw);
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
            free(wnodes); wnodes = tmp;
        }

        uint32_t required_uncomp = 12 + (f_cnt + 1) * sizeof(PFeat) + (p_cnt + ref_count * 2) * sizeof(float) + s_len + nlen + plen + 2;
        
        if (f_cnt > 0 && required_uncomp > flush_threshold) {
            uint32_t total_uncomp = 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float) + s_len;
            memcpy(uncomp_buf, &f_cnt, 4); memcpy(uncomp_buf + 4, &p_cnt, 4); memcpy(uncomp_buf + 8, &s_len, 4);
            memcpy(uncomp_buf + 12, feats_buf, f_cnt * sizeof(PFeat)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat), pts_buf, p_cnt * sizeof(float)); memcpy(uncomp_buf + 12 + f_cnt * sizeof(PFeat) + p_cnt * sizeof(float), str_buf, s_len);
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
        f_cnt++; free(wnodes);
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
    FSEEK64(f_sorted, 0, SEEK_SET);

    for (uint64_t i = 0; i < written_feats; i++) {
        SortEntry entry; fread(&entry, sizeof(SortEntry), 1, f_sorted);
        FSEEK64(f_raw, entry.offset, SEEK_SET); uint32_t total_len; fread(&total_len, 4, 1, f_raw);
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
        if (is_area && (f_max_lon - f_min_lon < 0.02) && (f_max_lat - f_min_lat < 0.02)) { free(wnodes); continue; }

        double dist_threshold = 0.0001 * (100.0 / (g_SimpLowPoly > 0 ? g_SimpLowPoly : 1));
        if (g_CfgNoRoads && (fc == CLASS_LAND || fc == CLASS_COASTLINE)) dist_threshold = 0.0001 * (100.0 / (g_SimpNoRoads > 0 ? g_SimpNoRoads : 1));

        WayNode* dec_nodes = malloc(ref_count * sizeof(WayNode)); uint32_t dec_cnt = 0; dec_nodes[dec_cnt++] = wnodes[0];
        for (uint32_t k = 1; k < ref_count - 1; k++) {
            double dx = wnodes[k].lon - dec_nodes[dec_cnt-1].lon, dy = wnodes[k].lat - dec_nodes[dec_cnt-1].lat;
            if (dx*dx + dy*dy > dist_threshold) dec_nodes[dec_cnt++] = wnodes[k]; 
        }
        if (ref_count > 1) {
            double dx = wnodes[ref_count-1].lon - dec_nodes[dec_cnt-1].lon, dy = wnodes[ref_count-1].lat - dec_nodes[dec_cnt-1].lat;
            if (dx*dx + dy*dy > (dist_threshold / 10.0)) dec_nodes[dec_cnt++] = wnodes[ref_count-1];
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
        f_cnt++; free(dec_nodes); free(wnodes);
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

    fclose(f_sorted); fclose(f_qjmc);
    free(uncomp_buf); free(comp_buf); free(raw_buf); free(meta); free(over_meta);
    free(feats_buf); free(pts_buf); free(str_buf); fclose(f_raw);
    remove(raw_tmp_name); remove(sort_tmp_name); remove(sorted_tmp_name);

    SetStatus("Done! Wrote %llu features to %s", (unsigned long long)written_feats, out_filename);
    return 0;
}

static DWORD WINAPI CompileThread(LPVOID lpParam) {
    ThreadArgs* args = (ThreadArgs*)lpParam;
    EnableWindow(hBtnRun, FALSE);
    EnableWindow(hBtnDelTmp, FALSE);
    SetStatus("Compilation started...");
    int result = RunCompilation(args->inFile, args->outFile, args->refFile, false, 0, 0, 0, 0);

    if (g_RefPolys) {
        for (uint32_t i = 0; i < g_RefPolyCount; i++) free(g_RefPolys[i].nodes);
        free(g_RefPolys);
        g_RefPolys = NULL;
        g_RefPolyCount = 0;
    }

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

        CreateWindowA("STATIC", "Low Poly Reference Map (.qjm) [Optional]:", WS_VISIBLE|WS_CHILD, 20, 80, 300, 20, hwnd, NULL, NULL, NULL);
        hTxtRef = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 100, 420, 24, hwnd, NULL, NULL, NULL);
        hBtnRef = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 450, 100, 90, 24, hwnd, (HMENU)5, NULL, NULL);

        CreateWindowA("STATIC", "Water Polygons Shapefile (.shp) [Optional]:", WS_VISIBLE|WS_CHILD, 20, 140, 320, 20, hwnd, NULL, NULL, NULL);
        hTxtShp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_ShapefileFile, WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 160, 420, 24, hwnd, NULL, NULL, NULL);
        hBtnShp = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 450, 160, 90, 24, hwnd, (HMENU)6, NULL, NULL);

        CreateWindowA("STATIC", "Output File (.qjm):", WS_VISIBLE|WS_CHILD, 20, 200, 200, 20, hwnd, NULL, NULL, NULL);
        hTxtOut = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL, 20, 220, 420, 24, hwnd, NULL, NULL, NULL);
        hBtnOut = CreateWindowA("BUTTON", "Browse...", WS_VISIBLE|WS_CHILD, 450, 220, 90, 24, hwnd, (HMENU)2, NULL, NULL);
        
        CreateWindowA("STATIC", "Feature Layers & Retention % (Nodes):", WS_VISIBLE|WS_CHILD, 20, 260, 350, 20, hwnd, NULL, NULL, NULL);
        
        hChkLand = CreateWindowA("BUTTON", "Landmass", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 285, 80, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpLand = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 100, 285, 25, 20, hwnd, NULL, NULL, NULL);

        hChkHwyMain = CreateWindowA("BUTTON", "Main Roads", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 140, 285, 90, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpHwyMain = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 230, 285, 25, 20, hwnd, NULL, NULL, NULL);

        hChkPark = CreateWindowA("BUTTON", "Parks/Forests", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 270, 285, 105, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpPark = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 375, 285, 25, 20, hwnd, NULL, NULL, NULL);
        
        hChkCoast = CreateWindowA("BUTTON", "Coastlines", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 315, 80, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpCoast = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 100, 315, 25, 20, hwnd, NULL, NULL, NULL);

        hChkHwyMin = CreateWindowA("BUTTON", "Minor Roads", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 140, 315, 90, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpHwyMin = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 230, 315, 25, 20, hwnd, NULL, NULL, NULL);

        hChkWater = CreateWindowA("BUTTON", "Water Bodies", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 270, 315, 100, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpWater = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 375, 315, 25, 20, hwnd, NULL, NULL, NULL);

        hChkPcode = CreateWindowA("BUTTON", "Postcodes", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 345, 100, 20, hwnd, NULL, NULL, NULL);
        hChkSpecs = CreateWindowA("BUTTON", "Specs", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 140, 345, 80, 20, hwnd, NULL, NULL, NULL);

        CreateWindowA("STATIC", "LOD Ext. Simplification (Keep %):", WS_VISIBLE|WS_CHILD, 240, 345, 210, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpLowPoly = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "75", WS_VISIBLE|WS_CHILD|ES_NUMBER, 460, 345, 30, 20, hwnd, NULL, NULL, NULL);

        hChkNoRoads = CreateWindowA("BUTTON", "Land/Coast (No Roads)", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, 20, 375, 180, 20, hwnd, NULL, NULL, NULL);
        hTxtSimpNoRoads = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "100", WS_VISIBLE|WS_CHILD|ES_NUMBER, 205, 375, 30, 20, hwnd, NULL, NULL, NULL);
        
        SendMessage(hChkLand, BM_SETCHECK, BST_CHECKED, 0); 
        SendMessage(hChkCoast, BM_SETCHECK, BST_CHECKED, 0);
        SendMessage(hChkHwyMain, BM_SETCHECK, BST_CHECKED, 0); SendMessage(hChkHwyMin, BM_SETCHECK, BST_CHECKED, 0);
        SendMessage(hChkPcode, BM_SETCHECK, BST_CHECKED, 0);
        SendMessage(hChkSpecs, BM_SETCHECK, BST_UNCHECKED, 0); 
        SendMessage(hChkNoRoads, BM_SETCHECK, BST_CHECKED, 0);
        
        hBtnRun = CreateWindowA("BUTTON", "Compile", WS_VISIBLE|WS_CHILD, 20, 410, 340, 35, hwnd, (HMENU)3, NULL, NULL);
        hBtnDelTmp = CreateWindowA("BUTTON", "Delete Temp Files", WS_VISIBLE|WS_CHILD, 370, 410, 170, 35, hwnd, (HMENU)4, NULL, NULL);
        hLblStatus = CreateWindowA("STATIC", "Ready.", WS_VISIBLE|WS_CHILD, 20, 455, 520, 20, hwnd, NULL, NULL, NULL);

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
        } else if (LOWORD(wParam) == 5) { 
            OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = ""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "QJM Reference File (*.qjm)\0*.qjm\0All\0*.*\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) SetWindowTextA(hTxtRef, path);
        } else if (LOWORD(wParam) == 6) { 
            OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = ""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "Shapefiles (*.shp)\0*.shp\0All\0*.*\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) SetWindowTextA(hTxtShp, path);
        } else if (LOWORD(wParam) == 2) { 
            OPENFILENAMEA ofn = {0}; char path[MAX_PATH] = ""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "QJM Chunked File\0*.qjm\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT;
            if (GetSaveFileNameA(&ofn)) SetWindowTextA(hTxtOut, path);
        } else if (LOWORD(wParam) == 3) { 
            ThreadArgs* args = malloc(sizeof(ThreadArgs));
            GetWindowTextA(hTxtIn, args->inFile, MAX_PATH); 
            GetWindowTextA(hTxtOut, args->outFile, MAX_PATH);
            GetWindowTextA(hTxtRef, args->refFile, MAX_PATH);
            
            GetWindowTextA(hTxtShp, g_ShapefileFile, MAX_PATH);
            WritePrivateProfileStringA("MapData", "CoastlineShapefile", g_ShapefileFile, ".\\pbf2qjm.ini");
            
            if (strlen(args->inFile) == 0 || strlen(args->outFile) == 0) { SetStatus("Please specify input and output files."); free(args); return 0; }
            
            g_CfgLand = SendMessage(hChkLand, BM_GETCHECK, 0, 0) == BST_CHECKED; 
            g_CfgCoast = SendMessage(hChkCoast, BM_GETCHECK, 0, 0) == BST_CHECKED;

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
} 
return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    
    LoadConfig(); 
    
    if (__argc >= 2) {
        char out_filename[1024]; 
        strncpy(out_filename, __argv[1], sizeof(out_filename) - 1); 
        out_filename[sizeof(out_filename) - 1] = '\0';
        
        char* ext = strrchr(out_filename, '.');
        if (ext && (strcasecmp(ext, ".pbf") == 0 || strcasecmp(ext, ".shp") == 0)) {
            strcpy(ext, ".qjm"); 
        } else {
            strncat(out_filename, ".qjm", sizeof(out_filename) - strlen(out_filename) - 1);
        }
        
        char* ref_arg = (__argc >= 3) ? __argv[2] : "";
        return RunCompilation(__argv[1], out_filename, ref_arg, false, 0, 0, 0, 0);
    }
    
    WNDCLASSA wc = {0}; 
    wc.lpfnWndProc = CompilerWndProc; 
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "Pbf2QJMGuiClass"; 
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); 
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    
    HWND hwnd = CreateWindowExA(0, "Pbf2QJMGuiClass", "PBF to QJMC Compiler", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 530, NULL, NULL, wc.hInstance, NULL);
        
    ShowWindow(hwnd, SW_SHOW); 
    UpdateWindow(hwnd);
    
    MSG msg = {0}; 
    while (GetMessage(&msg, NULL, 0, 0)) { 
        TranslateMessage(&msg); 
        DispatchMessage(&msg); 
    } 
    return 0;
}
