#ifndef PDFS_H
#define PDFS_H

extern CRITICAL_SECTION g_FileCS;
extern double g_Rotation;
extern int g_HighlightedChunk, g_HighlightedFeat;
extern FILE* g_WorldMapPtr;
extern double g_WorldMapMaxZoom, g_HighwayZoomMult;

#ifndef QJM_S_APP_DEFINED
#define QJM_S_APP_DEFINED
static void s_app(Stream* s, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args); va_end(args);
    if (n < 0) return;
    if (s->len + n >= s->cap) {
        s->cap = (s->cap == 0 ? 4096 : s->cap * 2) + n + 1024;
        s->data = (char*)realloc(s->data, s->cap);
    }
    va_start(args, fmt);
    vsnprintf(s->data + s->len, n + 1, fmt, args);
    va_end(args);
    s->len += n;
}
#endif

static void pdf_color(Stream* s, int is_stroke, int r, int g, int b) {
    s_app(s, "%.3f %.3f %.3f %s\n", r / 255.0f, g / 255.0f, b / 255.0f, is_stroke ? "RG" : "rg");
}

static uint8_t unicode_to_cp1252(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) return (uint8_t)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (uint8_t)cp;
    switch (cp) {
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015:
        case 0x2212: return '-';
        case 0x2018: case 0x2019: case 0x201A: return '\'';
        case 0x201C: case 0x201D: case 0x201E: return '"';
        case 0x2026: return '.';
        case 0x0152: return 0x8C; // OE
        case 0x0153: return 0x9C; // oe
        case 0x0160: return 0x8A; // Scaron
        case 0x0161: return 0x9A; // scaron
        case 0x0178: return 0x9F; // Ydieresis
        case 0x017D: return 0x8E; // Zcaron
        case 0x017E: return 0x9E; // zcaron
        case 0x20AC: return 0x80; // Euro
        default: return '?';
    }
}

static size_t format_pdf_string(const char* src, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    const uint8_t* p = (const uint8_t*)src;
    size_t out = 0, vis_len = 0;

    while (*p && out + 3 < dst_size) {
        uint8_t ch = 0;
        if (p[0] < 0x80) {
            ch = *p++;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            ch = unicode_to_cp1252(cp);
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
            ch = unicode_to_cp1252(cp);
            p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                          ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            ch = unicode_to_cp1252(cp);
            p += 4;
        } else {
            // Already single-byte CP1252 / WinAnsi
            ch = *p++;
            if (ch == 0x96 || ch == 0x97) ch = '-';
        }

        if (ch == '(' || ch == ')' || ch == '\\') {
            dst[out++] = '\\';
        }
        dst[out++] = (char)ch;
        vis_len++;
    }
    dst[out] = '\0';
    return vis_len;
}

static void pdf_center_text(Stream* s, const char* text, float x, float y, float w, int font_size, int is_bold) {
    char clean[512];
    size_t vis_len = format_pdf_string(text, clean, sizeof(clean));
    float text_w = (float)vis_len * font_size * 0.5f;
    float offset = x + (w - text_w) / 2.0f; if (offset < x) offset = x;
    s_app(s, "BT %s %d Tf 0 0 0 rg %.2f %.2f Td (%s) Tj ET\n", is_bold ? "/F2" : "/F1", font_size, offset, y, clean);
}

static void LatLonToPdf(double lon, double lat, double center_lon, double center_lat, double cx, double cy, double pdf_zoom, double* outX, double* outY) {
    double dx = (lon - center_lon) * pdf_zoom;
    double dy = (lat - center_lat) * pdf_zoom;
    if (fabs(g_Rotation) > 1e-6) {
        double c = cos(-g_Rotation), s = sin(-g_Rotation);
        double rx = dx * c - dy * s, ry = dx * s + dy * c;
        dx = rx; dy = ry;
    }
    *outX = cx + dx;
    *outY = cy + dy;
}

static void generate_pdf_ex(
    const char* out_file, const char* title_text, float margH_pct, float margV_pct,
    double center_lon, double center_lat, double view_w, double view_h, double screen_zoom,
    const MapPoint* route_path, int route_path_cnt,
    const RouteStep* route_steps, int route_step_cnt,
    double total_route_dist, bool use_miles,
    const MapPin* pins, int pin_cnt)
{
    EnterCriticalSection(&g_FileCS);
    FILE* f = fopen(out_file, "wb");
    if (!f) { LeaveCriticalSection(&g_FileCS); return; }

    if (view_w < 100.0) view_w = 1024.0;
    if (view_h < 100.0) view_h = 768.0;
    if (screen_zoom < 0.001) screen_zoom = 0.001;

    float raw_w_pt = page_w_mm * 72.0f / 25.4f, raw_h_pt = page_h_mm * 72.0f / 25.4f;
    float short_pt = fminf(raw_w_pt, raw_h_pt), long_pt = fmaxf(raw_w_pt, raw_h_pt);
    float pt_w = (view_w >= view_h) ? long_pt : short_pt;
    float pt_h = (view_w >= view_h) ? short_pt : long_pt;

    float m_x = pt_w * (margH_pct / 100.0f), m_y = pt_h * (margV_pct / 100.0f);
    float map_w = pt_w - (2.0f * m_x), map_h = pt_h - (2.0f * m_y) - 36.0f;
    if (map_w < 50.0f) map_w = 50.0f;
    if (map_h < 50.0f) map_h = 50.0f;
    float map_cx = m_x + (map_w / 2.0f), map_cy = m_y + (map_h / 2.0f);

    double pdf_scale = fmin((double)map_w / view_w, (double)map_h / view_h);
    double pdf_zoom = screen_zoom * pdf_scale;
    double pad_deg = 8.0 / screen_zoom;
    double vp_min_lon, vp_max_lon, vp_min_lat, vp_max_lat;
    if (fabs(g_Rotation) < 1e-6) {
        double half_lon = (map_w * 0.5) / pdf_zoom + pad_deg;
        double half_lat = (map_h * 0.5) / pdf_zoom + pad_deg;
        vp_min_lon = center_lon - half_lon; vp_max_lon = center_lon + half_lon;
        vp_min_lat = center_lat - half_lat; vp_max_lat = center_lat + half_lat;
    } else {
        double rad = hypot(map_w, map_h) / (2.0 * pdf_zoom) + pad_deg;
        vp_min_lon = center_lon - rad; vp_max_lon = center_lon + rad;
        vp_min_lat = center_lat - rad; vp_max_lat = center_lat + rad;
    }

    fprintf(f, "%%PDF-1.1\n");
    int info_obj = 1, catalog_obj = 2, pages_obj = 3, font1_obj = 4, font2_obj = 5; pdf_obj_cnt = 6;
    pdf_objects[info_obj] = ftell(f); fprintf(f, "%d 0 obj\n<< /Title (Map) /Creator (QJMnavig) >>\nendobj\n", info_obj);
    pdf_objects[catalog_obj] = ftell(f); fprintf(f, "%d 0 obj\n<< /Type /Catalog /Pages %d 0 R >>\nendobj\n", catalog_obj, pages_obj);
    pdf_objects[font1_obj] = ftell(f); fprintf(f, "%d 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>\nendobj\n", font1_obj);
    pdf_objects[font2_obj] = ftell(f); fprintf(f, "%d 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>\nendobj\n", font2_obj);

    Stream s = {0}; int page_obj = pdf_obj_cnt++, stream_obj = pdf_obj_cnt++;
    pdf_objects[page_obj] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> >> >>\nendobj\n",
        page_obj, pages_obj, pt_w, pt_h, stream_obj, font1_obj, font2_obj);

    pdf_center_text(&s, title_text, m_x, pt_h - m_y - 22.0f, map_w, 20, 1);
    s_app(&s, "q %.2f %.2f %.2f %.2f re W n\n", m_x, m_y, map_w, map_h);
    pdf_color(&s, 0, GetRValue(c_Water), GetGValue(c_Water), GetBValue(c_Water));
    s_app(&s, "%.2f %.2f %.2f %.2f re f\n1 J 1 j\n", m_x, m_y, map_w, map_h);

    double main_hwy_min_zoom = g_WorldMapMaxZoom / 3.0;
    double hwy_zoom_threshold = (g_WorldMapMaxZoom * g_HighwayZoomMult) / 3.0;
    bool show_world = (screen_zoom <= g_WorldMapMaxZoom && g_WorldMapPtr != NULL);
    bool use_overview = (!show_world && screen_zoom < 500.0 && g_HasOverview);
    double min_feat_deg = (show_world ? 0.75 : 1.5) / screen_zoom;

    typedef struct { float l, t, r, b; } PdfBox;
    PdfBox* drawn_boxes = (PdfBox*)malloc(4000 * sizeof(PdfBox));
    const char** drawn_names = (const char**)malloc(4000 * sizeof(const char*));
    int box_cnt = 0, name_cnt = 0;

    double *seg_x = NULL, *seg_y = NULL; uint32_t seg_cap = 0;
    float w_coast = fmaxf(0.6f, (float)(1.5 * pdf_scale));
    float w_minor = fmaxf(0.8f, (float)(2.5 * pdf_scale));
    float w_main  = fmaxf(1.4f, (float)(4.0 * pdf_scale));

    for (int pass = 1; pass <= 7; pass++) {
        for (uint32_t c = 0; c < g_ChunkCount; c++) {
            MapChunk* mc = &g_Chunks[c];
            bool is_world_chunk = (mc->file == g_WorldMapPtr && g_WorldMapPtr != NULL);

            if (pass <= 3) {
                if (show_world) {
                    if (!is_world_chunk) continue;
                } else {
                    if (is_world_chunk) continue;
                    if (use_overview && !mc->is_overview) continue;
                    if (!use_overview && mc->is_overview) continue;
                }
            } else {
                if (screen_zoom < main_hwy_min_zoom) {
                    if (!is_world_chunk) continue;
                } else if (show_world) {
                    if (is_world_chunk) continue;
                    if (g_HasOverview && !mc->is_overview) continue;
                    if (!g_HasOverview && mc->is_overview) continue;
                } else {
                    if (is_world_chunk) continue;
                    if (use_overview && !mc->is_overview) continue;
                    if (!use_overview && mc->is_overview) continue;
                }
            }

            if (mc->max_lon < vp_min_lon || mc->min_lon > vp_max_lon || mc->max_lat < vp_min_lat || mc->min_lat > vp_max_lat) continue;

            uint8_t* data = GetChunkData(c); if (!data) continue;
            uint32_t f_cnt = *(uint32_t*)data; PFeat* feats = (PFeat*)(data + 12);
            float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
            char* strings = (char*)((uint8_t*)points + (*(uint32_t*)(data + 4)) * sizeof(float));

            for (uint32_t i = 0; i < f_cnt; i++) {
                PFeat* feat = &feats[i]; if (feat->pt_cnt < 2) continue;
                if (is_world_chunk && (feat->fc == CLASS_HWY_MINOR || feat->fc == CLASS_HWY_MAIN)) continue;
                if (feat->max_lon < vp_min_lon || feat->min_lon > vp_max_lon || feat->max_lat < vp_min_lat || feat->min_lat > vp_max_lat) continue;

                double feat_w = feat->max_lon - feat->min_lon, feat_h = feat->max_lat - feat->min_lat;
                if (feat_w < min_feat_deg && feat_h < min_feat_deg) continue;

                if (pass == 1) {
                    if (!g_PdfExpLand || !feat->is_area || (feat->fc != CLASS_LAND && feat->fc != CLASS_COASTLINE)) continue;
                    pdf_color(&s, 0, GetRValue(c_Land), GetGValue(c_Land), GetBValue(c_Land));
                } else if (pass == 2) {
                    if (!feat->is_area) continue;
                    if (feat->fc == CLASS_WATER && g_PdfExpWater) pdf_color(&s, 0, GetRValue(c_Water), GetGValue(c_Water), GetBValue(c_Water));
                    else if (feat->fc == CLASS_PARK && g_PdfExpPark) pdf_color(&s, 0, GetRValue(c_Park), GetGValue(c_Park), GetBValue(c_Park));
                    else continue;
                } else if (pass == 3) {
                    if (!g_PdfExpCoast || feat->is_area || feat->fc != CLASS_COASTLINE) continue;
                    pdf_color(&s, 1, GetRValue(c_Coast), GetGValue(c_Coast), GetBValue(c_Coast)); s_app(&s, "%.2f w\n", w_coast);
                } else if (pass == 4) {
                    if (!g_PdfExpHwyMin || feat->fc != CLASS_HWY_MINOR || screen_zoom < hwy_zoom_threshold || use_overview) continue;
                    pdf_color(&s, 1, GetRValue(c_HwyMin), GetGValue(c_HwyMin), GetBValue(c_HwyMin)); s_app(&s, "%.2f w\n", w_minor);
                } else if (pass == 5) {
                    if (!g_PdfExpHwyMain || feat->fc != CLASS_HWY_MAIN || screen_zoom < main_hwy_min_zoom) continue;
                    pdf_color(&s, 1, GetRValue(c_HwyMain), GetGValue(c_HwyMain), GetBValue(c_HwyMain)); s_app(&s, "%.2f w\n", w_main);
                } else if (pass == 6) {
                    if (!g_PdfExpLabels || (feat->fc != CLASS_HWY_MAIN && feat->fc != CLASS_PLACE && feat->fc != CLASS_POI) || screen_zoom < main_hwy_min_zoom || !feat->name_off) continue;
                } else if (pass == 7) {
                    if (!g_PdfExpLabels || feat->fc != CLASS_HWY_MINOR || screen_zoom < 5000.0 || !feat->name_off || use_overview) continue;
                }

                if (feat->pt_cnt > seg_cap) {
                    seg_cap = feat->pt_cnt + 1024;
                    seg_x = (double*)realloc(seg_x, seg_cap * sizeof(double));
                    seg_y = (double*)realloc(seg_y, seg_cap * sizeof(double));
                }

                double last_px = -99999, last_py = -99999; uint32_t pts_written = 0;
                double min_step_sq = fmax(0.25, (1.0 * pdf_scale) * (1.0 * pdf_scale));
                for (uint32_t j = 0; j < feat->pt_cnt; j++) {
                    double px, py;
                    LatLonToPdf(points[feat->pt_off + j * 2], points[feat->pt_off + j * 2 + 1], center_lon, center_lat, map_cx, map_cy, pdf_zoom, &px, &py);
                    if (px < -32000) px = -32000; else if (px > 32000) px = 32000;
                    if (py < -32000) py = -32000; else if (py > 32000) py = 32000;
                    if (j == 0 || j == feat->pt_cnt - 1 || ((px - last_px) * (px - last_px) + (py - last_py) * (py - last_py) >= min_step_sq)) {
                        seg_x[pts_written] = px; seg_y[pts_written] = py;
                        if (pass <= 5) s_app(&s, "%.2f %.2f %s\n", px, py, pts_written == 0 ? "m" : "l");
                        last_px = px; last_py = py; pts_written++;
                    }
                }
                if (feat->is_area ? (pts_written < 3) : (pts_written < 2)) continue;

                if (pass == 6 || pass == 7) {
                    char* txt = strings + feat->name_off;
                    bool already_drawn = false;
                    for (int k = 0; k < name_cnt; k++) { if (strcmp(drawn_names[k], txt) == 0) { already_drawn = true; break; } }
                    if (already_drawn) continue;

                    double max_len = 0; uint32_t best_i = 0;
                    for (uint32_t j = 0; j < pts_written - 1; j++) {
                        double dx = seg_x[j + 1] - seg_x[j], dy = seg_y[j + 1] - seg_y[j], len = dx * dx + dy * dy;
                        if (len > max_len) { max_len = len; best_i = j; }
                    }
                    if (max_len < (pass == 6 ? 120.0 : 30.0) * pdf_scale * pdf_scale) continue;

                    double p1x = seg_x[best_i], p1y = seg_y[best_i], p2x = seg_x[best_i + 1], p2y = seg_y[best_i + 1];
                    double cx = (p1x + p2x) / 2.0, cy = (p1y + p2y) / 2.0;
                    if (cx < m_x + 20 || cx > m_x + map_w - 20 || cy < m_y + 15 || cy > m_y + map_h - 15) continue;

                    char clean_name[256];
                    size_t vis_len = format_pdf_string(txt, clean_name, sizeof(clean_name));
                    float fs = fminf(10.0f, fmaxf(6.5f, (float)(10.0 * pdf_scale)));
                    float text_w = (float)vis_len * fs * 0.52f;
                    float hw = text_w / 2.0f, hh = fs / 2.0f, pad = (pass == 6 ? 16.0f : 6.0f) * (float)pdf_scale;
                    PdfBox bx = { (float)cx - hw - pad, (float)cy - hh - pad, (float)cx + hw + pad, (float)cy + hh + pad };

                    bool collision = false;
                    for (int k = 0; k < box_cnt; k++) {
                        if (!(bx.r < drawn_boxes[k].l || bx.l > drawn_boxes[k].r || bx.b < drawn_boxes[k].t || bx.t > drawn_boxes[k].b)) { collision = true; break; }
                    }
                    if (collision) continue;
                    if (box_cnt < 4000) drawn_boxes[box_cnt++] = bx;
                    if (name_cnt < 4000) drawn_names[name_cnt++] = txt;

                    double angle_rad = atan2(p2y - p1y, p2x - p1x);
                    if (angle_rad < -1.570796) angle_rad += 3.14159265;
                    if (angle_rad > 1.570796) angle_rad -= 3.14159265;
                    double a = cos(angle_rad), b = sin(angle_rad), c_mat = -sin(angle_rad), d_mat = cos(angle_rad);
                    s_app(&s, "BT %.4f %.4f %.4f %.4f %.2f %.2f Tm 0.16 0.16 0.16 rg /F2 %.1f Tf (%s) Tj ET\n",
                        a, b, c_mat, d_mat, cx - (text_w / 2.0) * a + 2.0 * c_mat, cy - (text_w / 2.0) * b + 2.0 * d_mat, fs, clean_name);
                } else {
                    s_app(&s, "%s\n", (pass == 1 || pass == 2) ? "f" : "S");
                }
            }
        }
    }
    if (seg_x) free(seg_x);
    if (seg_y) free(seg_y);
    free(drawn_boxes); free(drawn_names);

    if (route_path_cnt > 1 && route_path) {
        pdf_color(&s, 1, GetRValue(c_Route), GetGValue(c_Route), GetBValue(c_Route));
        s_app(&s, "%.2f w\n", fmaxf(2.2f, (float)(5.5 * pdf_scale)));
        for (int j = 0; j < route_path_cnt; j++) {
            double px, py; LatLonToPdf(route_path[j].lon, route_path[j].lat, center_lon, center_lat, map_cx, map_cy, pdf_zoom, &px, &py);
            s_app(&s, "%.2f %.2f %s\n", px, py, j == 0 ? "m" : "l");
        }
        s_app(&s, "S\n");
    }

    if (pin_cnt > 0 && pins) {
        float ps = fminf(1.2f, fmaxf(0.65f, (float)pdf_scale));
        for (int i = 0; i < pin_cnt; i++) {
            double px, py; LatLonToPdf(pins[i].lon, pins[i].lat, center_lon, center_lat, map_cx, map_cy, pdf_zoom, &px, &py);
            if (px < m_x || px > m_x + map_w || py < m_y || py > m_y + map_h) continue;
            pdf_color(&s, 0, GetRValue(c_Pin), GetGValue(c_Pin), GetBValue(c_Pin));
            pdf_color(&s, 1, 0, 0, 0);
            s_app(&s, "0.8 w %.2f %.2f m %.2f %.2f l %.2f %.2f l %.2f %.2f l h B\n",
                px, py, px - 8.0 * ps, py + 18.0 * ps, px, py + 26.0 * ps, px + 8.0 * ps, py + 18.0 * ps);
            if (pins[i].name[0]) {
                char clean_pin[256];
                size_t vis_len = format_pdf_string(pins[i].name, clean_pin, sizeof(clean_pin));
                float pfs = 9.0f * ps, pw = (float)vis_len * pfs * 0.5f;
                s_app(&s, "BT /F2 %.1f Tf 0 0 0 rg %.2f %.2f Td (%s) Tj ET\n", pfs, px - pw / 2.0, py + 28.0 * ps, clean_pin);
            }
        }
    }

    if (route_step_cnt > 0 && route_steps) {
        float leg_w = 195.0f, max_leg_h = map_h - 20.0f, req_h = 30.0f + route_step_cnt * 12.0f;
        float leg_h = (req_h < max_leg_h) ? req_h : max_leg_h;
        float leg_x = m_x + 10.0f, leg_y = m_y + map_h - leg_h - 10.0f;
        pdf_color(&s, 0, 250, 248, 245); s_app(&s, "%.2f %.2f %.2f %.2f re f\n", leg_x, leg_y, leg_w, leg_h);
        pdf_color(&s, 1, 200, 200, 200); s_app(&s, "1.0 w %.2f %.2f %.2f %.2f re S\n", leg_x, leg_y, leg_w, leg_h);
        pdf_color(&s, 0, 30, 30, 30); float text_y = m_y + map_h - 25.0f;
        char hdr_str[128], clean_hdr[256];
        snprintf(hdr_str, sizeof(hdr_str), "Total Distance: %.1f %s", total_route_dist, use_miles ? "mi" : "km");
        format_pdf_string(hdr_str, clean_hdr, sizeof(clean_hdr));
        s_app(&s, "BT /F2 9 Tf %.2f %.2f Td (%s) Tj ET\n", leg_x + 8.0f, text_y, clean_hdr); text_y -= 15.0f;
        for (int step = 0; step < route_step_cnt && text_y >= leg_y + 8.0f; step++, text_y -= 12.0f) {
            char step_str[300], clean_step[512];
            snprintf(step_str, sizeof(step_str), "%d. %s (%.1f %s)", step + 1, route_steps[step].text, route_steps[step].dist, use_miles ? "mi" : "km");
            format_pdf_string(step_str, clean_step, sizeof(clean_step));
            s_app(&s, "BT /F1 8 Tf %.2f %.2f Td (%s) Tj ET\n", leg_x + 8.0f, text_y, clean_step);
        }
    }

    s_app(&s, "Q\n");
    pdf_color(&s, 1, 160, 160, 160);
    s_app(&s, "0.75 w %.2f %.2f %.2f %.2f re S\n", m_x, m_y, map_w, map_h);

    pdf_objects[stream_obj] = ftell(f); fprintf(f, "%d 0 obj\n<< /Length %d >>\nstream\n%s\nendstream\nendobj\n", stream_obj, s.len, s.data);
    pdf_objects[pages_obj] = ftell(f); fprintf(f, "%d 0 obj\n<< /Type /Pages /Count 1 /Kids [ %d 0 R ] >>\nendobj\n", pages_obj, page_obj);
    long xref_pos = ftell(f); fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", pdf_obj_cnt);
    for (int i = 1; i < pdf_obj_cnt; i++) fprintf(f, "%010ld 00000 n \n", pdf_objects[i]);
    fprintf(f, "trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%ld\n%%%%EOF\n", pdf_obj_cnt, catalog_obj, info_obj, xref_pos);

    if (s.data) free(s.data);
    fclose(f);
    LeaveCriticalSection(&g_FileCS);
}

static void generate_pdf(const char* out_file, const char* title_text, float margH_pct, float margV_pct, double view_w, double view_h) {
    double eff_lon, eff_lat;
    ScreenToLatLon((int)(view_w / 2.0), (int)(view_h / 2.0), (int)view_w, (int)view_h, &eff_lon, &eff_lat);
    generate_pdf_ex(out_file, title_text, margH_pct, margV_pct, eff_lon, eff_lat, view_w, view_h, g_Zoom,
        g_RoutePath, g_RoutePathCount, g_RouteSteps, g_RouteStepCount, g_TotalRouteDist, g_UseMiles, g_Pins, g_PinCount);
}

#endif // PDFS_H