#ifndef SHPS_H
#define SHPS_H

static void WriteRawFeature(FILE* f_raw, WayNode* wnodes, uint32_t ref_count, bool is_area, const char* name, const char* pcode, uint8_t fc, SortEntry** entries, uint64_t* cnt, uint32_t* cap);

static WayNode* ShpClipRingAABB(WayNode* in_nodes, uint32_t in_cnt, double min_x, double min_y, double max_x, double max_y, uint32_t* out_cnt) {
    *out_cnt = 0;
    if (!in_nodes || in_cnt < 3) return NULL;

    double p_min_x = in_nodes[0].lon, p_max_x = in_nodes[0].lon;
    double p_min_y = in_nodes[0].lat, p_max_y = in_nodes[0].lat;
    for (uint32_t i = 1; i < in_cnt; i++) {
        if (in_nodes[i].lon < p_min_x) p_min_x = in_nodes[i].lon;
        if (in_nodes[i].lon > p_max_x) p_max_x = in_nodes[i].lon;
        if (in_nodes[i].lat < p_min_y) p_min_y = in_nodes[i].lat;
        if (in_nodes[i].lat > p_max_y) p_max_y = in_nodes[i].lat;
    }

    if (p_max_x < min_x || p_min_x > max_x || p_max_y < min_y || p_min_y > max_y) {
        return NULL;
    }

    if (p_min_x >= min_x && p_max_x <= max_x && p_min_y >= min_y && p_max_y <= max_y) {
        WayNode* copy = malloc(in_cnt * sizeof(WayNode));
        if (!copy) return NULL;
        memcpy(copy, in_nodes, in_cnt * sizeof(WayNode));
        *out_cnt = in_cnt;
        return copy;
    }

    uint32_t work_cnt = in_cnt;
    if (work_cnt > 3 && fabs(in_nodes[0].lon - in_nodes[work_cnt - 1].lon) < 1e-7 &&
        fabs(in_nodes[0].lat - in_nodes[work_cnt - 1].lat) < 1e-7) {
        work_cnt--;
    }

    uint32_t cap = work_cnt * 4 + 32;
    WayNode* buf_a = malloc(cap * sizeof(WayNode));
    WayNode* buf_b = malloc(cap * sizeof(WayNode));
    if (!buf_a || !buf_b) {
        free(buf_a); free(buf_b);
        return NULL;
    }

    const double bounds[4] = { min_x, max_x, min_y, max_y };
    const WayNode* src = in_nodes;
    uint32_t cnt = work_cnt;
    WayNode* dst = buf_a;

    for (int plane = 0; plane < 4; plane++) {
        if (cnt == 0) break;
        double bound = bounds[plane];
        uint32_t next_cnt = 0;
        WayNode prev = src[cnt - 1];
        bool prev_in = (plane == 0) ? (prev.lon >= bound) :
                       (plane == 1) ? (prev.lon <= bound) :
                       (plane == 2) ? (prev.lat >= bound) : (prev.lat <= bound);

        for (uint32_t i = 0; i < cnt; i++) {
            WayNode curr = src[i];
            bool curr_in = (plane == 0) ? (curr.lon >= bound) :
                           (plane == 1) ? (curr.lon <= bound) :
                           (plane == 2) ? (curr.lat >= bound) : (curr.lat <= bound);

            if (curr_in != prev_in) {
                WayNode inter;
                inter.id = 1;
                if (plane < 2) {
                    double dx = curr.lon - prev.lon;
                    double t = (fabs(dx) > 1e-12) ? ((bound - prev.lon) / dx) : 0.0;
                    inter.lon = bound;
                    inter.lat = prev.lat + t * (curr.lat - prev.lat);
                } else {
                    double dy = curr.lat - prev.lat;
                    double t = (fabs(dy) > 1e-12) ? ((bound - prev.lat) / dy) : 0.0;
                    inter.lon = prev.lon + t * (curr.lon - prev.lon);
                    inter.lat = bound;
                }
                if (next_cnt < cap) dst[next_cnt++] = inter;
            }
            if (curr_in && next_cnt < cap) {
                dst[next_cnt++] = curr;
            }
            prev = curr;
            prev_in = curr_in;
        }
        cnt = next_cnt;
        src = dst;
        dst = (dst == buf_a) ? buf_b : buf_a;
    }

    if (cnt < 3) {
        free(buf_a); free(buf_b);
        return NULL;
    }

    WayNode* final_nodes = malloc((cnt + 1) * sizeof(WayNode));
    if (!final_nodes) {
        free(buf_a); free(buf_b);
        return NULL;
    }

    uint32_t f_cnt = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (f_cnt > 0 && fabs(src[i].lon - final_nodes[f_cnt - 1].lon) < 1e-8 &&
            fabs(src[i].lat - final_nodes[f_cnt - 1].lat) < 1e-8) {
            continue;
        }
        final_nodes[f_cnt++] = src[i];
    }
    free(buf_a); free(buf_b);

    if (f_cnt < 3) {
        free(final_nodes);
        return NULL;
    }

    if (fabs(final_nodes[0].lon - final_nodes[f_cnt - 1].lon) > 1e-8 ||
        fabs(final_nodes[0].lat - final_nodes[f_cnt - 1].lat) > 1e-8) {
        final_nodes[f_cnt] = final_nodes[0];
        final_nodes[f_cnt].id = 1;
        f_cnt++;
    }

    *out_cnt = f_cnt;
    return final_nodes;
}

static void ProcessShapefile(const char* shp_path, FILE* f_raw, SortEntry** entries, uint64_t* cnt, uint32_t* cap, bool has_bbox, double b_minLon, double b_minLat, double b_maxLon, double b_maxLat) {
    FILE* f = fopen(shp_path, "rb"); if (!f) return;
    char dbf_path[MAX_PATH]; strncpy(dbf_path, shp_path, MAX_PATH - 1); dbf_path[MAX_PATH - 1] = '\0';
    char* ext = strrchr(dbf_path, '.'); if (ext) strcpy(ext, ".dbf");
    FILE* f_dbf = fopen(dbf_path, "rb");
    int name_offset = -1, name_len = 0, record_len = 0, num_records = 0, header_bytes = 0;

    if (f_dbf) {
        uint8_t head[32];
        if (fread(head, 1, 32, f_dbf) == 32) {
            num_records = head[4] | (head[5] << 8) | (head[6] << 16) | (head[7] << 24);
            header_bytes = head[8] | (head[9] << 8); record_len = head[10] | (head[11] << 8);
            int curr_offset = 1;
            while (ftell(f_dbf) < (long)(header_bytes - 1)) {
                uint8_t field[32]; if (fread(field, 1, 32, f_dbf) != 32 || field[0] == 0x0D) break;
                if (name_offset == -1 && field[11] == 'C') { name_offset = curr_offset; name_len = field[16]; }
                curr_offset += field[16];
            }
        }
    }

    FSEEK64(f, 0, SEEK_END); int64_t file_size = FTELL64(f); FSEEK64(f, 100, SEEK_SET);
    AccumWay* shp_coasts = NULL; uint32_t coast_cap = 0, coast_cnt = 0;

    while (FTELL64(f) < file_size) {
        uint32_t rec_num, rec_len;
        if (fread(&rec_num, 4, 1, f) != 1 || fread(&rec_len, 4, 1, f) != 1) break;
        rec_num = SwapEndian32(rec_num); rec_len = SwapEndian32(rec_len) * 2;
        int64_t next_rec = FTELL64(f) + rec_len; uint32_t shape_type;
        if (fread(&shape_type, 4, 1, f) != 1) break;

        if (shape_type == 3 || shape_type == 5) {
            double box[4]; if (fread(box, sizeof(double), 4, f) != 4) break;
            if (has_bbox && (box[2] < b_minLon || box[0] > b_maxLon || box[3] < b_minLat || box[1] > b_maxLat)) {
                FSEEK64(f, next_rec, SEEK_SET);
                continue;
            }

            uint32_t num_parts, num_points;
            if (fread(&num_parts, 4, 1, f) != 1 || fread(&num_points, 4, 1, f) != 1) break;

            uint32_t* parts = malloc(num_parts * 4); fread(parts, 4, num_parts, f);
            WayNode* all_points = malloc(num_points * sizeof(WayNode));
            for (uint32_t i = 0; i < num_points; i++) {
                double pt[2]; fread(pt, sizeof(double), 2, f);
                all_points[i].lon = pt[0]; all_points[i].lat = pt[1]; all_points[i].id = 0xFFFFFFFFFFFFFFFFULL;
            }

            char feat_name[256] = {0};
            if (f_dbf && name_offset != -1 && (int)rec_num <= num_records) {
                fseek(f_dbf, header_bytes + (rec_num - 1) * record_len + name_offset, SEEK_SET);
                int rlen = name_len < 255 ? name_len : 255; fread(feat_name, 1, rlen, f_dbf);
                for (int k = rlen - 1; k >= 0; k--) {
                    if (feat_name[k] == ' ' || feat_name[k] == '\r' || feat_name[k] == '\n') feat_name[k] = '\0'; else break;
                }
            }

            uint8_t fc = (shape_type == 5) ? CLASS_LAND : CLASS_COASTLINE;
            if (strlen(feat_name) > 0) {
                char ln[256]; for (int k = 0; feat_name[k] && k < 255; k++) ln[k] = tolower(feat_name[k]); ln[255] = '\0';
                if (strstr(ln, "coast") || strstr(ln, "shore") || strstr(ln, "beach") || strstr(ln, "island")) fc = CLASS_COASTLINE;
                else if (strstr(ln, "water") || strstr(ln, "lake") || strstr(ln, "river") || strstr(ln, "ocean") || strstr(ln, "sea")) fc = CLASS_WATER;
                else if (strstr(ln, "park") || strstr(ln, "forest") || strstr(ln, "reserve")) fc = CLASS_PARK;
                else if (strstr(ln, "road") || strstr(ln, "hwy") || strstr(ln, "highway") || strstr(ln, " st ") || strncmp(ln, "st ", 3) == 0 || strstr(ln, " street")) fc = CLASS_HWY_MAIN;
            }

            for (uint32_t i = 0; i < num_parts; i++) {
                uint32_t start = parts[i], end = (i == num_parts - 1) ? num_points : parts[i + 1], ref_count = end - start;
                if (ref_count >= 2) {
                    WayNode* part_pts = &all_points[start];
                    double dx = part_pts[0].lon - part_pts[ref_count - 1].lon;
                    double dy = part_pts[0].lat - part_pts[ref_count - 1].lat;
                    bool is_closed_ring = (ref_count >= 4) && (shape_type == 5 || (fabs(dx) < 1e-6 && fabs(dy) < 1e-6));

                    if (is_closed_ring) {
                        uint8_t out_fc = (fc == CLASS_COASTLINE) ? CLASS_LAND : fc;
                        if (has_bbox) {
                            uint32_t clipped_cnt = 0;
                            WayNode* clipped = ShpClipRingAABB(part_pts, ref_count, b_minLon, b_minLat, b_maxLon, b_maxLat, &clipped_cnt);
                            if (clipped && clipped_cnt >= 4) {
                                WriteRawFeature(f_raw, clipped, clipped_cnt, true, strlen(feat_name) ? feat_name : "Shapefile Entity", NULL, out_fc, entries, cnt, cap);
                            }
                            if (clipped) free(clipped);
                        } else {
                            part_pts[ref_count - 1] = part_pts[0];
                            WriteRawFeature(f_raw, part_pts, ref_count, true, strlen(feat_name) ? feat_name : "Shapefile Entity", NULL, out_fc, entries, cnt, cap);
                        }
                    } else if (fc == CLASS_COASTLINE || fc == CLASS_WATER || fc == CLASS_PARK || fc == CLASS_LAND) {
                        double p_min_x = part_pts[0].lon, p_max_x = part_pts[0].lon;
                        double p_min_y = part_pts[0].lat, p_max_y = part_pts[0].lat;
                        for (uint32_t j = 1; j < ref_count; j++) {
                            if (part_pts[j].lon < p_min_x) p_min_x = part_pts[j].lon;
                            if (part_pts[j].lon > p_max_x) p_max_x = part_pts[j].lon;
                            if (part_pts[j].lat < p_min_y) p_min_y = part_pts[j].lat;
                            if (part_pts[j].lat > p_max_y) p_max_y = part_pts[j].lat;
                        }
                        if (has_bbox && (p_max_x < b_minLon || p_min_x > b_maxLon || p_max_y < b_minLat || p_min_y > b_maxLat)) {
                            continue;
                        }
                        WayNode* wnodes = malloc(ref_count * sizeof(WayNode));
                        memcpy(wnodes, part_pts, ref_count * sizeof(WayNode));
                        if (coast_cnt >= coast_cap) { coast_cap = coast_cap == 0 ? 2000 : coast_cap * 2; shp_coasts = realloc(shp_coasts, coast_cap * sizeof(AccumWay)); }
                        shp_coasts[coast_cnt].name = strlen(feat_name) ? strdup(feat_name) : NULL; shp_coasts[coast_cnt].pcode = NULL;
                        shp_coasts[coast_cnt].count = ref_count; shp_coasts[coast_cnt].nodes = wnodes; shp_coasts[coast_cnt].merged = false; shp_coasts[coast_cnt].fc = fc; coast_cnt++;
                    } else {
                        WriteRawFeature(f_raw, part_pts, ref_count, false, strlen(feat_name) ? feat_name : "Shapefile Entity", NULL, fc, entries, cnt, cap);
                    }
                }
            }
            free(parts); free(all_points);
        }
        FSEEK64(f, next_rec, SEEK_SET);
    }

    if (coast_cnt > 0) {
        bool merged = true;
        while (merged) {
            merged = false;
            for (uint32_t i = 0; i < coast_cnt; i++) {
                if (shp_coasts[i].merged || shp_coasts[i].count < 2) continue;
                WayNode start_pt = shp_coasts[i].nodes[0], end_pt = shp_coasts[i].nodes[shp_coasts[i].count - 1];
                if (fabs(start_pt.lon - end_pt.lon) < 1e-6 && fabs(start_pt.lat - end_pt.lat) < 1e-6) continue;

                for (uint32_t j = i + 1; j < coast_cnt; j++) {
                    if (shp_coasts[j].merged || shp_coasts[j].count < 2) continue;
                    WayNode j_start = shp_coasts[j].nodes[0], j_end = shp_coasts[j].nodes[shp_coasts[j].count - 1];

                    bool match_end_start   = (fabs(end_pt.lon - j_start.lon) < 1e-6 && fabs(end_pt.lat - j_start.lat) < 1e-6);
                    bool match_end_end     = (fabs(end_pt.lon - j_end.lon) < 1e-6   && fabs(end_pt.lat - j_end.lat) < 1e-6);
                    bool match_start_end   = (fabs(start_pt.lon - j_end.lon) < 1e-6 && fabs(start_pt.lat - j_end.lat) < 1e-6);
                    bool match_start_start = (fabs(start_pt.lon - j_start.lon) < 1e-6 && fabs(start_pt.lat - j_start.lat) < 1e-6);

                    if (match_end_start || match_end_end || match_start_end || match_start_start) {
                        if (PolylinesIntersect(shp_coasts[i].nodes, shp_coasts[i].count, shp_coasts[j].nodes, shp_coasts[j].count)) continue;

                        if (match_start_end || match_start_start) {
                            for (uint32_t k = 0; k < shp_coasts[i].count / 2; k++) {
                                WayNode tmp = shp_coasts[i].nodes[k];
                                shp_coasts[i].nodes[k] = shp_coasts[i].nodes[shp_coasts[i].count - 1 - k];
                                shp_coasts[i].nodes[shp_coasts[i].count - 1 - k] = tmp;
                            }
                            start_pt = shp_coasts[i].nodes[0];
                            end_pt = shp_coasts[i].nodes[shp_coasts[i].count - 1];
                            if (match_start_end) match_end_end = true;
                            if (match_start_start) match_end_start = true;
                        }

                        uint32_t new_count = shp_coasts[i].count + shp_coasts[j].count - 1;
                        WayNode* safe = realloc(shp_coasts[i].nodes, new_count * sizeof(WayNode));
                        if (safe) {
                            shp_coasts[i].nodes = safe;
                            if (match_end_start) {
                                memcpy(shp_coasts[i].nodes + shp_coasts[i].count, shp_coasts[j].nodes + 1, (shp_coasts[j].count - 1) * sizeof(WayNode));
                            } else if (match_end_end) {
                                for (uint32_t k = 1; k < shp_coasts[j].count; k++) {
                                    shp_coasts[i].nodes[shp_coasts[i].count + k - 1] = shp_coasts[j].nodes[shp_coasts[j].count - 1 - k];
                                }
                            }
                            shp_coasts[i].count = new_count; shp_coasts[j].merged = true; merged = true;
                            end_pt = shp_coasts[i].nodes[shp_coasts[i].count - 1];
                        }
                    }
                }
            }
        }

        for (uint32_t i = 0; i < coast_cnt; i++) {
            if (shp_coasts[i].merged) { if (shp_coasts[i].name) free(shp_coasts[i].name); if (shp_coasts[i].pcode) free(shp_coasts[i].pcode); free(shp_coasts[i].nodes); continue; }
            WayNode* wnodes = shp_coasts[i].nodes; uint32_t ref_count = shp_coasts[i].count; uint8_t current_fc = shp_coasts[i].fc;
            double dx = wnodes[0].lon - wnodes[ref_count - 1].lon, dy = wnodes[0].lat - wnodes[ref_count - 1].lat, dist = sqrt(dx * dx + dy * dy);
            bool is_area = false;
            if (dist <= 0.000001) { wnodes[ref_count - 1] = wnodes[0]; is_area = true; }
            else if (dist < 0.1) {
                bool intersects = false;
                for (uint32_t k = 0; k < ref_count - 2; k++) {
                    if (SegmentsIntersect(wnodes[ref_count - 1].lon, wnodes[ref_count - 1].lat, wnodes[0].lon, wnodes[0].lat, wnodes[k].lon, wnodes[k].lat, wnodes[k + 1].lon, wnodes[k + 1].lat)) { intersects = true; break; }
                }
                if (!intersects) { WayNode* safe = realloc(wnodes, (ref_count + 1) * sizeof(WayNode)); if (safe) { wnodes = safe; wnodes[ref_count] = wnodes[0]; ref_count++; is_area = true; } }
            }
            if (is_area && current_fc == CLASS_COASTLINE) current_fc = CLASS_LAND;

            if (has_bbox && is_area) {
                uint32_t clipped_cnt = 0;
                WayNode* clipped = ShpClipRingAABB(wnodes, ref_count, b_minLon, b_minLat, b_maxLon, b_maxLat, &clipped_cnt);
                if (clipped && clipped_cnt >= 4) {
                    WriteRawFeature(f_raw, clipped, clipped_cnt, true, shp_coasts[i].name ? shp_coasts[i].name : "Shapefile Entity", NULL, current_fc, entries, cnt, cap);
                }
                if (clipped) free(clipped);
            } else {
                double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
                for (uint32_t j = 0; j < ref_count; j++) {
                    if (wnodes[j].lon < min_lon) min_lon = wnodes[j].lon; if (wnodes[j].lon > max_lon) max_lon = wnodes[j].lon;
                    if (wnodes[j].lat < min_lat) min_lat = wnodes[j].lat; if (wnodes[j].lat > max_lat) max_lat = wnodes[j].lat;
                }
                bool in_bbox = true; if (has_bbox && (max_lon < b_minLon || min_lon > b_maxLon || max_lat < b_minLat || min_lat > b_maxLat)) in_bbox = false;
                if (in_bbox) WriteRawFeature(f_raw, wnodes, ref_count, is_area, shp_coasts[i].name ? shp_coasts[i].name : "Shapefile Entity", NULL, current_fc, entries, cnt, cap);
            }
            if (shp_coasts[i].name) free(shp_coasts[i].name); if (shp_coasts[i].pcode) free(shp_coasts[i].pcode); free(wnodes);
        }
        free(shp_coasts);
    }
    fclose(f); if (f_dbf) fclose(f_dbf);
}

#endif // SHPS_H