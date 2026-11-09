#ifndef PBFS_H
#define PBFS_H
static inline uint64_t ReadVarint(const uint8_t** ptr, const uint8_t* end) {
    uint64_t val = 0; int shift = 0;
    while (*ptr < end) { uint8_t b = **ptr; (*ptr)++; val |= (uint64_t)(b & 0x7F) << shift; if (!(b & 0x80)) break; shift += 7; }
    return val;
}
static inline int64_t DecodeZigZag(uint64_t n) { return (n >> 1) ^ -(int64_t)(n & 1); }
static inline void SkipProtobufField(const uint8_t** ptr, const uint8_t* end, uint8_t wire_type) {
    if(wire_type == 0) ReadVarint(ptr, end); else if(wire_type == 1) *ptr += 8;
    else if(wire_type == 2) *ptr += ReadVarint(ptr, end); else if(wire_type == 5) *ptr += 4; else *ptr = end;
}
static int CompareNodes(const void* a, const void* b) { uint64_t idA = ((PbfNode*)a)->id, idB = ((PbfNode*)b)->id; return (idA > idB) - (idA < idB); }

static PbfNode* FindNode(PbfNode* cache, uint64_t count, uint64_t id) {
    int64_t left = 0, right = count - 1;
    while (left <= right) {
        int64_t mid = left + (right - left) / 2;
        if (cache[mid].id == id) return &cache[mid];
        if (cache[mid].id < id) left = mid + 1; else right = mid - 1;
    } return NULL;
}

static void ProcessPrimitiveBlock(const uint8_t* data, uint64_t size, int extract_mode, FILE* outFile) {
    const uint8_t* end = data + size;
    int64_t lat_offset = 0, lon_offset = 0, granularity = 100;
    PbfString* str_table = NULL; uint64_t str_count = 0, str_cap = 0;
    const uint8_t* ptr = data;

    while (ptr < end) {
        uint64_t tag_wire = ReadVarint(&ptr, end);
        if ((tag_wire >> 3) == 1 && (tag_wire & 7) == 2) {
            uint64_t st_len = ReadVarint(&ptr, end); const uint8_t* st_end = ptr + st_len;
            while (ptr < st_end) {
                uint64_t s_tag_wire = ReadVarint(&ptr, st_end);
                if ((s_tag_wire >> 3) == 1) {
                    uint64_t s_len = ReadVarint(&ptr, st_end);
                    if (str_count >= str_cap) { str_cap = str_cap == 0 ? 1024 : str_cap * 2; str_table = (PbfString*)realloc(str_table, str_cap * sizeof(PbfString)); }
                    str_table[str_count].str = (const char*)ptr; str_table[str_count].len = (size_t)s_len;
                    str_count++; ptr += s_len;
                } else SkipProtobufField(&ptr, st_end, s_tag_wire & 7);
            }
        }
        else if ((tag_wire >> 3) == 17) granularity = ReadVarint(&ptr, end);
        else if ((tag_wire >> 3) == 19) lat_offset = ReadVarint(&ptr, end);
        else if ((tag_wire >> 3) == 20) lon_offset = ReadVarint(&ptr, end);
        else SkipProtobufField(&ptr, end, tag_wire & 7);
    }

    // Allocate reusable way node buffer once per block instead of 120MB per way
    uint32_t temp_refs_cap = 65536;
    WayNode* temp_refs = (extract_mode == 1) ? (WayNode*)malloc(temp_refs_cap * sizeof(WayNode)) : NULL;
    bool allow_places_pois = (g_CfgHwyMain || g_CfgHwyMin || g_CfgWater || g_CfgPark);

    ptr = data;
    while (ptr < end) {
        uint64_t tag_wire = ReadVarint(&ptr, end);
        if ((tag_wire >> 3) == 2 && (tag_wire & 7) == 2) {
            uint64_t group_len = ReadVarint(&ptr, end); const uint8_t* group_end = ptr + group_len;
            while (ptr < group_end) {
                uint64_t g_tag_wire = ReadVarint(&ptr, group_end); uint64_t g_tag = g_tag_wire >> 3;

                if (extract_mode == 1 && (g_tag == 1 || g_tag == 2)) { SkipProtobufField(&ptr, group_end, g_tag_wire & 7); continue; }
                if (extract_mode == 2 && g_tag == 3) { SkipProtobufField(&ptr, group_end, g_tag_wire & 7); continue; }

                if (g_tag == 1 && extract_mode == 2) {
                    uint64_t msg_len = ReadVarint(&ptr, group_end); const uint8_t* msg_end = ptr + msg_len;
                    uint64_t id = 0; int64_t lat = 0, lon = 0;
                    while (ptr < msg_end) {
                        uint64_t n_tag_wire = ReadVarint(&ptr, msg_end);
                        if ((n_tag_wire >> 3) == 1) id = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else if ((n_tag_wire >> 3) == 8) lat = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else if ((n_tag_wire >> 3) == 9) lon = DecodeZigZag(ReadVarint(&ptr, msg_end));
                        else SkipProtobufField(&ptr, msg_end, n_tag_wire & 7);
                    }
                    PbfNode node; node.id = id; node.lat = (int32_t)((lat_offset + (granularity * lat)) / 100); node.lon = (int32_t)((lon_offset + (granularity * lon)) / 100);
                    fwrite(&node, sizeof(PbfNode), 1, outFile);
                }
                else if (g_tag == 2 && extract_mode == 2) {
                    uint64_t dense_len = ReadVarint(&ptr, group_end); const uint8_t* dense_end = ptr + dense_len;
                    const uint8_t *id_ptr = NULL, *lat_ptr = NULL, *lon_ptr = NULL;

                    while (ptr < dense_end) {
                        uint64_t d_tag_wire = ReadVarint(&ptr, dense_end);
                        if ((d_tag_wire >> 3) == 1) { id_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else if ((d_tag_wire >> 3) == 8) { lat_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else if ((d_tag_wire >> 3) == 9) { lon_ptr = ptr; SkipProtobufField(&ptr, dense_end, d_tag_wire & 7); }
                        else SkipProtobufField(&ptr, dense_end, d_tag_wire & 7);
                    }
                    if (id_ptr && lat_ptr && lon_ptr) {
                        uint64_t id_len = ReadVarint(&id_ptr, dense_end); ReadVarint(&lat_ptr, dense_end); ReadVarint(&lon_ptr, dense_end);
                        const uint8_t* id_end = id_ptr + id_len; int64_t last_id = 0, last_lat = 0, last_lon = 0;
                        while (id_ptr < id_end) {
                            last_id += DecodeZigZag(ReadVarint(&id_ptr, dense_end)); last_lat += DecodeZigZag(ReadVarint(&lat_ptr, dense_end)); last_lon += DecodeZigZag(ReadVarint(&lon_ptr, dense_end));
                            PbfNode node; node.id = last_id; node.lat = (int32_t)((lat_offset + (granularity * last_lat)) / 100); node.lon = (int32_t)((lon_offset + (granularity * last_lon)) / 100);
                            fwrite(&node, sizeof(PbfNode), 1, outFile);
                        }
                    }
                }
                else if (g_tag == 3 && extract_mode == 1) {
                    uint64_t way_len = ReadVarint(&ptr, group_end); const uint8_t* way_end = ptr + way_len;
                    const uint8_t *refs_ptr = NULL, *keys_ptr = NULL, *vals_ptr = NULL; uint64_t keys_len = 0, vals_len = 0;
                    while (ptr < way_end) {
                        uint64_t w_tag_wire = ReadVarint(&ptr, way_end);
                        if ((w_tag_wire >> 3) == 2) { keys_len = ReadVarint(&ptr, way_end); keys_ptr = ptr; ptr += keys_len; }
                        else if ((w_tag_wire >> 3) == 3) { vals_len = ReadVarint(&ptr, way_end); vals_ptr = ptr; ptr += vals_len; }
                        else if ((w_tag_wire >> 3) == 8) { refs_ptr = ptr; SkipProtobufField(&ptr, way_end, w_tag_wire & 7); }
                        else SkipProtobufField(&ptr, way_end, w_tag_wire & 7);
                    }

                    if (refs_ptr && temp_refs) {
                        int feature_class = CLASS_UNKNOWN; const char* feat_name = NULL; size_t feat_name_len = 0;
                        const char* feat_postcode = NULL; size_t feat_postcode_len = 0; bool is_coastline = false;

                        if (keys_ptr && vals_ptr && str_table) {
                            const uint8_t *k_p = keys_ptr, *v_p = vals_ptr;
                            while (k_p < keys_ptr + keys_len && v_p < vals_ptr + vals_len) {
                                uint64_t k_idx = ReadVarint(&k_p, keys_ptr + keys_len); uint64_t v_idx = ReadVarint(&v_p, vals_ptr + vals_len);
                                if (k_idx < str_count && v_idx < str_count) {
                                    PbfString* k = &str_table[k_idx]; PbfString* v = &str_table[v_idx];
                                    if (k->len == 4 && strncmp(k->str, "name", 4) == 0) { feat_name = v->str; feat_name_len = v->len; }
                                    else if ((k->len == 13 && strncmp(k->str, "addr:postcode", 13) == 0) || (k->len == 11 && strncmp(k->str, "postal_code", 11) == 0)) { feat_postcode = v->str; feat_postcode_len = v->len; }
                                    else if (k->len == 7 && strncmp(k->str, "highway", 7) == 0) {
                                        feature_class = CLASS_HWY_MINOR;
                                        if (v->len == 8 && strncmp(v->str, "motorway", 8) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 5 && strncmp(v->str, "trunk", 5) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 7 && strncmp(v->str, "primary", 7) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 13 && strncmp(v->str, "motorway_link", 13) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 10 && strncmp(v->str, "trunk_link", 10) == 0) feature_class = CLASS_HWY_MAIN;
                                        else if (v->len == 12 && strncmp(v->str, "primary_link", 12) == 0) feature_class = CLASS_HWY_MAIN;
                                    } else if (k->len == 7 && strncmp(k->str, "natural", 7) == 0) {
                                        if (v->len == 5 && strncmp(v->str, "water", 5) == 0) feature_class = CLASS_WATER;
                                        else if (v->len == 9 && strncmp(v->str, "coastline", 9) == 0) { feature_class = CLASS_COASTLINE; is_coastline = true; }
                                        else if (v->len == 4 && strncmp(v->str, "land", 4) == 0) feature_class = CLASS_LAND;
                                        else if (v->len == 4 && strncmp(v->str, "wood", 4) == 0) feature_class = CLASS_PARK;
                                    } else if (k->len == 8 && strncmp(k->str, "waterway", 8) == 0) feature_class = CLASS_WATER;
                                    else if (k->len == 7 && strncmp(k->str, "landuse", 7) == 0) {
                                        if (v->len == 6 && strncmp(v->str, "forest", 6) == 0) feature_class = CLASS_PARK;
                                        else if (v->len == 5 && strncmp(v->str, "grass", 5) == 0) feature_class = CLASS_PARK;
                                        else if (g_CfgSpecs) feature_class = CLASS_LAND;
                                    } else if (k->len == 7 && strncmp(k->str, "leisure", 7) == 0) feature_class = CLASS_PARK;
                                    else if (k->len == 5 && strncmp(k->str, "place", 5) == 0) feature_class = CLASS_PLACE;
                                    else if (k->len == 8 && strncmp(k->str, "boundary", 8) == 0 && v->len == 14 && strncmp(v->str, "administrative", 14) == 0) feature_class = CLASS_PLACE;
                                    else if (k->len == 8 && strncmp(k->str, "historic", 8) == 0 && v->len == 8 && strncmp(v->str, "monument", 8) == 0) feature_class = CLASS_POI;
                                    else if (k->len == 7 && strncmp(k->str, "tourism", 7) == 0 && v->len == 7 && strncmp(v->str, "artwork", 7) == 0) feature_class = CLASS_POI;
                                    else if (k->len == 8 && strncmp(k->str, "man_made", 8) == 0 && v->len == 6 && strncmp(v->str, "statue", 6) == 0) feature_class = CLASS_POI;
                                }
                            }
                        }
                        if (is_coastline) feature_class = CLASS_COASTLINE;
                        if (feature_class == CLASS_WATER && !g_CfgWater) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_LAND && !g_CfgLand) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_PARK && !g_CfgPark) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_COASTLINE && !g_CfgCoast) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_HWY_MAIN && !g_CfgHwyMain) feature_class = CLASS_UNKNOWN;
                        if (feature_class == CLASS_HWY_MINOR && !g_CfgHwyMin) feature_class = CLASS_UNKNOWN;
                        if ((feature_class == CLASS_PLACE || feature_class == CLASS_POI) && !allow_places_pois) feature_class = CLASS_UNKNOWN;
                        if (!g_CfgPcode) { feat_postcode = NULL; feat_postcode_len = 0; }

                        if (feature_class != CLASS_UNKNOWN) {
                            uint64_t refs_len = ReadVarint(&refs_ptr, way_end); const uint8_t* refs_end = refs_ptr + refs_len;
                            uint8_t fc = (uint8_t)feature_class; fwrite(&fc, 1, 1, outFile);
                            uint16_t nlen = feat_name ? (uint16_t)feat_name_len : 0; fwrite(&nlen, 2, 1, outFile);
                            if (nlen > 0) fwrite(feat_name, 1, nlen, outFile);
                            uint16_t plen = feat_postcode ? (uint16_t)feat_postcode_len : 0; fwrite(&plen, 2, 1, outFile);
                            if (plen > 0) fwrite(feat_postcode, 1, plen, outFile);

                            uint32_t ref_count = 0;
                            int64_t last_ref = 0;
                            while (refs_ptr < refs_end && ref_count < 5000000) {
                                if (ref_count >= temp_refs_cap) {
                                    uint32_t new_cap = temp_refs_cap * 2;
                                    if (new_cap > 5000000) new_cap = 5000000;
                                    WayNode* safe = (WayNode*)realloc(temp_refs, new_cap * sizeof(WayNode));
                                    if (!safe) break;
                                    temp_refs = safe;
                                    temp_refs_cap = new_cap;
                                }
                                last_ref += DecodeZigZag(ReadVarint(&refs_ptr, refs_end));
                                temp_refs[ref_count].id = (uint64_t)last_ref;
                                temp_refs[ref_count].lat = 0;
                                temp_refs[ref_count].lon = 0;
                                ref_count++;
                            }
                            fwrite(&ref_count, 4, 1, outFile);
                            if (ref_count > 0) fwrite(temp_refs, sizeof(WayNode), ref_count, outFile);
                        }
                    }
                } else SkipProtobufField(&ptr, group_end, g_tag_wire & 7);
            }
        } else SkipProtobufField(&ptr, end, tag_wire & 7);
    }
    if (temp_refs) free(temp_refs);
    if (str_table) free(str_table);
}
#endif // PBFS_H