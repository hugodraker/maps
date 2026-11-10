/* ============================================================================
 * qjmhttpd.h - High-Performance Embedded HTTP & Vector Streaming Server
 * ============================================================================ */

#ifndef QJMHTTPD_H
#define QJMHTTPD_H

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "qjm.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

extern CRITICAL_SECTION g_FileCS;
extern MapChunk* g_Chunks;
extern uint32_t g_ChunkCount;
extern bool g_HasOverview;
extern SearchItem* g_SearchItems;
extern uint32_t g_SearchItemCount;
extern double g_CenterLon, g_CenterLat, g_Zoom;
extern float g_MargH, g_MargV;
extern FILE* g_WorldMapPtr;
extern double g_WorldMapMaxZoom, g_HighwayZoomMult;

static uint8_t* GetChunkData(int c_idx);
static void CalculateRouteForPins(const MapPin* pins, int pin_count, int route_mode, MapPoint** out_path, int* out_path_cnt, RouteStep** out_steps, int* out_step_cnt, double* out_total_dist, bool* out_use_miles);
static void generate_pdf_ex(const char* out_file, const char* title_text, float margH_pct, float margV_pct, double center_lon, double center_lat, double view_w, double view_h, double screen_zoom, const MapPoint* route_path, int route_path_cnt, const RouteStep* route_steps, int route_step_cnt, double total_route_dist, bool use_miles, const MapPin* pins, int pin_cnt);

static SOCKET g_HttpServerSocket = INVALID_SOCKET;
static HANDLE g_HttpServerThread = NULL;
static bool g_HttpServerRunning = false;

static void UrlDecode(char* dst, const char* src, size_t max_len) {
    size_t i = 0;
    while (*src && i + 1 < max_len) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static const char* kHtmlApp =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>QJM Map Web Navigator</title>"
"<style>"
"  html, body { margin:0; padding:0; width:100%; height:100%; overflow:hidden; font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif; background:#A3CCFF; }"
"  #map { width:100%; height:100%; display:block; cursor:default; }"
"  .panel { position:absolute; top:12px; left:12px; z-index:10; display:flex; gap:8px; align-items:center; }"
"  input, select, button { height:30px; border-radius:4px; border:1px solid #aaa; padding:0 8px; font-size:13px; outline:none; box-shadow:0 2px 4px rgba(0,0,0,0.15); }"
"  button { background:#fff; cursor:pointer; font-weight:600; }"
"  button:hover { background:#f0f0f0; }"
"  #search { width:200px; }"
"  #legend { position:absolute; top:55px; left:12px; width:260px; max-height:calc(100% - 75px); overflow-y:auto; background:rgba(250,248,245,0.95); border-radius:12px; border:1px solid #ccc; padding:12px; box-shadow:0 4px 12px rgba(0,0,0,0.15); z-index:10; display:none; font-size:12px; }"
"  #legend h3 { margin:0 0 8px 0; font-size:13px; color:#222; border-bottom:1px solid #ddd; padding-bottom:4px; }"
"  #legend ol { margin:0; padding-left:18px; line-height:1.5; color:#333; }"
"  #search-results { position:absolute; top:44px; left:12px; width:216px; max-height:220px; overflow-y:auto; background:#fff; border:1px solid #aaa; border-radius:4px; box-shadow:0 4px 8px rgba(0,0,0,0.2); z-index:20; display:none; }"
"  .search-item { padding:6px 10px; cursor:pointer; font-size:12px; border-bottom:1px solid #eee; }"
"  .search-item:hover { background:#e8f0fe; }"
"</style></head><body>"
"<div class='panel'>"
"  <input type='text' id='search' placeholder='Search street, postal code...' autocomplete='off'>"
"  <button id='btnCancel' style='display:none;'>Cancel Route</button>"
"  <select id='routeType' style='display:none;'><option value='0'>Fastest Route</option><option value='1'>Shortest Route</option></select>"
"  <button id='btnExtents'>Zoom Extents</button>"
"  <button id='btnPdf'>Export PDF</button>"
"</div>"
"<div id='search-results'></div>"
"<div id='legend'><h3 id='legHdr'>Route</h3><ol id='legSteps'></ol></div>"
"<canvas id='map'></canvas>"
"<script>"
"const canvas = document.getElementById('map'), ctx = canvas.getContext('2d');"
"let view = { lon: -64.8, lat: 46.1, zoom: 5000 };"
"let hasMaps = false;"
"let pins = [], routePath = [], routeSteps = [], totalDist = 0, useMiles = false;"
"let features = [];"
"let isDragging = false, dragMoved = false, lastX = 0, lastY = 0;"
"function resize() { canvas.width = window.innerWidth; canvas.height = window.innerHeight; render(); }"
"window.addEventListener('resize', resize);"
"function lonLatToScreen(lon, lat) { return [ (canvas.width / 2) + (lon - view.lon) * view.zoom, (canvas.height / 2) - (lat - view.lat) * view.zoom ]; }"
"function screenToLonLat(x, y) { return [ view.lon + (x - canvas.width / 2) / view.zoom, view.lat - (y - canvas.height / 2) / view.zoom ]; }"
"let isFetching = false, fetchQueued = false;"
"async function fetchVectorData() {"
"  if (isFetching) { fetchQueued = true; return; }"
"  isFetching = true; fetchQueued = false;"
"  const w = canvas.width, h = canvas.height;"
"  const [minLon, maxLat] = screenToLonLat(0, 0);"
"  const [maxLon, minLat] = screenToLonLat(w, h);"
"  const url = `/api/data?minLon=${minLon}&maxLon=${maxLon}&minLat=${minLat}&maxLat=${maxLat}&zoom=${view.zoom}`;"
"  try {"
"    const res = await fetch(url);"
"    const buf = await res.arrayBuffer();"
"    const dv = new DataView(buf);"
"    let off = 0;"
"    if (buf.byteLength >= 4) {"
"      const count = dv.getUint32(off, true); off += 4;"
"      const newFeats = [];"
"      for (let i = 0; i < count && off < buf.byteLength; i++) {"
"        const fc = dv.getUint8(off++);"
"        const isArea = dv.getUint8(off++);"
"        const ptCnt = dv.getUint16(off, true); off += 2;"
"        const nLen = dv.getUint16(off, true); off += 2;"
"        let name = '';"
"        if (nLen > 0) {"
"          const nameBytes = new Uint8Array(buf, off, nLen);"
"          name = new TextDecoder().decode(nameBytes); off += nLen;"
"        }"
"        const pts = new Float32Array(buf.slice(off, off + ptCnt * 8)); off += ptCnt * 8;"
"        newFeats.push({ fc, isArea, pts, name });"
"      }"
"      features = newFeats; render();"
"    }"
"  } catch(e) { console.error(e); }"
"  isFetching = false; if (fetchQueued) setTimeout(fetchVectorData, 80);"
"}"
"const COLORS = { 1: '#A3CCFF', 2: '#F2F0E9', 3: '#CADFAA', 4: '#6496B4', 5: '#FFFFFF', 6: '#FDB813', 7: '#E0E0E0', 8: '#D0D0D0' };"
"function render() {"
"  ctx.fillStyle = '#A3CCFF'; ctx.fillRect(0, 0, canvas.width, canvas.height);"
"  if (!hasMaps) return;"
"  const drawnNames = new Set();"
"  const drawnBoxes = [];"
"  for (let pass = 1; pass <= 7; pass++) {"
"    if (pass <= 5) {"
"      const groups = {};"
"      for (let f of features) {"
"        if (pass === 1 && (!f.isArea || (f.fc !== 2 && f.fc !== 4))) continue;"
"        if (pass === 2 && (!f.isArea || (f.fc !== 1 && f.fc !== 3))) continue;"
"        if (pass === 3 && (f.isArea || f.fc !== 4)) continue;"
"        if (pass === 4 && f.fc !== 5) continue;"
"        if (pass === 5 && f.fc !== 6) continue;"
"        let k = f.isArea ? 'A'+f.fc : 'L'+f.fc;"
"        if (!groups[k]) groups[k] = [];"
"        groups[k].push(f);"
"      }"
"      for (let k in groups) {"
"        ctx.beginPath();"
"        const cx = canvas.width/2 - view.lon*view.zoom, cy = canvas.height/2 + view.lat*view.zoom, vz = view.zoom;"
"        for (let f of groups[k]) {"
"          const pts = f.pts, ptCnt = pts.length / 2;"
"          for (let j = 0; j < ptCnt; j++) {"
"            let sx = cx + pts[j*2]*vz, sy = cy - pts[j*2+1]*vz;"
"            if (j === 0) ctx.moveTo(sx, sy); else ctx.lineTo(sx, sy);"
"          }"
"        }"
"        if (k.charAt(0) === 'A') {"
"          ctx.fillStyle = COLORS[parseInt(k.substring(1))] || '#fff'; ctx.fill();"
"        } else {"
"          let fc = parseInt(k.substring(1));"
"          ctx.strokeStyle = COLORS[fc] || '#fff';"
"          ctx.lineWidth = fc === 6 ? 4 : (fc === 5 ? 2.5 : 1.5);"
"          ctx.stroke();"
"        }"
"      }"
"    } else {"
"      for (let f of features) {"
"        if (drawnBoxes.length > 800) break;"
"        if (pass === 6 && ((f.fc !== 6 && f.fc !== 7 && f.fc !== 8) || !f.name)) continue;"
"        if (pass === 7 && (f.fc !== 5 || !f.name)) continue;"
"        if (drawnNames.has(f.name)) continue;"
"        const pts = f.pts, ptCnt = pts.length / 2;"
"        let maxLen = 0, bestI = 0;"
"        const cx_m = canvas.width/2 - view.lon*view.zoom, cy_m = canvas.height/2 + view.lat*view.zoom, vz = view.zoom;"
"        for (let j = 0; j < ptCnt - 1; j++) {"
"          let x1 = cx_m + pts[j*2]*vz, y1 = cy_m - pts[j*2+1]*vz;"
"          let x2 = cx_m + pts[(j+1)*2]*vz, y2 = cy_m - pts[(j+1)*2+1]*vz;"
"          let l = (x2-x1)*(x2-x1) + (y2-y1)*(y2-y1);"
"          if (l > maxLen) { maxLen = l; bestI = j; }"
"        }"
"        if (maxLen < (pass === 6 ? 120 : 30)) continue;"
"        let p1x = cx_m + pts[bestI*2]*vz, p1y = cy_m - pts[bestI*2+1]*vz;"
"        let p2x = cx_m + pts[(bestI+1)*2]*vz, p2y = cy_m - pts[(bestI+1)*2+1]*vz;"
"        let cx = (p1x+p2x)/2, cy = (p1y+p2y)/2;"
"        if (cx < -50 || cx > canvas.width+50 || cy < -50 || cy > canvas.height+50) continue;"
"        ctx.font = 'bold 12px Arial';"
"        let textW = ctx.measureText(f.name).width;"
"        let overlap = false;"
"        for(let i = 0; i < drawnBoxes.length; i++) {"
"          let box = drawnBoxes[i];"
"          if(Math.abs(cx - box.x) < (textW + box.w)/2 + 12 && Math.abs(cy - box.y) < 18) { overlap = true; break; }"
"        }"
"        if(overlap) continue;"
"        drawnBoxes.push({x:cx, y:cy, w:textW});"
"        drawnNames.add(f.name);"
"        let ang = Math.atan2(p2y - p1y, p2x - p1x);"
"        if (ang > Math.PI/2) ang -= Math.PI; else if (ang < -Math.PI/2) ang += Math.PI;"
"        ctx.save(); ctx.translate(cx, cy); ctx.rotate(ang);"
"        ctx.fillStyle = '#333'; ctx.textAlign = 'center';"
"        ctx.fillText(f.name, 0, -3); ctx.restore();"
"      }"
"    }"
"  }"
"  if (routePath.length > 1) {"
"    ctx.beginPath(); ctx.strokeStyle = '#E22B8A'; ctx.lineWidth = 6; ctx.lineCap = 'round'; ctx.lineJoin = 'round';"
"    for (let i = 0; i < routePath.length; i++) {"
"      const [sx, sy] = lonLatToScreen(routePath[i].lon, routePath[i].lat);"
"      if (i === 0) ctx.moveTo(sx, sy); else ctx.lineTo(sx, sy);"
"    }"
"    ctx.stroke();"
"  }"
"  pins.forEach((p, idx) => {"
"    const [sx, sy] = lonLatToScreen(p.lon, p.lat);"
"    ctx.fillStyle = '#DC3232'; ctx.strokeStyle = '#000'; ctx.lineWidth = 1;"
"    ctx.beginPath(); ctx.moveTo(sx, sy); ctx.lineTo(sx-8, sy-18); ctx.lineTo(sx, sy-26); ctx.lineTo(sx+8, sy-18); ctx.closePath();"
"    ctx.fill(); ctx.stroke();"
"    ctx.fillStyle = '#000'; ctx.font = '12px Arial'; ctx.textAlign = 'center';"
"    ctx.fillText(p.name || (idx + 1), sx, sy - 28);"
"  });"
"}"
"let fetchTimer = null;"
"function scheduleFetch() { clearTimeout(fetchTimer); fetchTimer = setTimeout(fetchVectorData, 80); }"
"canvas.addEventListener('mousedown', e => { if (e.button === 0) { isDragging = true; dragMoved = false; lastX = e.clientX; lastY = e.clientY; } });"
"window.addEventListener('mousemove', e => {"
"  if (!isDragging) return;"
"  const dx = e.clientX - lastX, dy = e.clientY - lastY;"
"  if (Math.abs(dx) > 2 || Math.abs(dy) > 2) dragMoved = true;"
"  view.lon -= dx / view.zoom; view.lat += dy / view.zoom;"
"  lastX = e.clientX; lastY = e.clientY;"
"  render(); scheduleFetch();"
"});"
"window.addEventListener('mouseup', () => { isDragging = false; });"
"canvas.addEventListener('wheel', e => {"
"  e.preventDefault();"
"  const factor = e.deltaY < 0 ? 1.25 : 0.8;"
"  const [mX, mY] = [e.clientX, e.clientY];"
"  const [lon, lat] = screenToLonLat(mX, mY);"
"  view.zoom *= factor;"
"  if(view.zoom > 1000000) view.zoom = 1000000;"
"  view.lon = lon - (mX - canvas.width / 2) / view.zoom;"
"  view.lat = lat + (mY - canvas.height / 2) / view.zoom;"
"  render(); scheduleFetch();"
"});"
"async function updateWebRoute() {"
"  if (pins.length < 2) {"
"    routePath = []; routeSteps = [];"
"    document.getElementById('btnCancel').style.display = 'none';"
"    document.getElementById('routeType').style.display = 'none';"
"    document.getElementById('legend').style.display = 'none';"
"    render(); return;"
"  }"
"  const pinStr = pins.map(p => `${p.lon},${p.lat}`).join('|');"
"  const mode = document.getElementById('routeType').value;"
"  const res = await fetch(`/api/route?pins=${encodeURIComponent(pinStr)}&mode=${mode}`);"
"  const data = await res.json();"
"  routePath = data.path || []; routeSteps = data.steps || []; totalDist = data.totalDist || 0; useMiles = data.useMiles;"
"  document.getElementById('btnCancel').style.display = 'inline-block';"
"  document.getElementById('routeType').style.display = 'inline-block';"
"  document.getElementById('legend').style.display = 'block';"
"  document.getElementById('legHdr').innerText = `Total Distance: ${totalDist.toFixed(1)} ${useMiles ? 'mi' : 'km'}`;"
"  document.getElementById('legSteps').innerHTML = routeSteps.map((s, i) => `<li>${s.text} (${s.dist.toFixed(1)} ${useMiles ? 'mi' : 'km'})</li>`).join('');"
"  render();"
"}"
"canvas.addEventListener('dblclick', async e => {"
"  const mx = e.clientX, my = e.clientY;"
"  for (let i = 0; i < pins.length; i++) {"
"    const [px, py] = lonLatToScreen(pins[i].lon, pins[i].lat);"
"    if (Math.abs(mx - px) <= 16 && (my - py) >= -38 && (my - py) <= 8) {"
"      const newName = prompt('Edit pin name:', pins[i].name);"
"      if (newName !== null) {"
"        pins[i].name = newName;"
"        render();"
"        await updateWebRoute();"
"      }"
"      return;"
"    }"
"  }"
"  let [lon, lat] = screenToLonLat(mx, my);"
"  let bestLon = lon, bestLat = lat;"
"  let snapDist = Infinity, snapRad = Math.max(0.04, 35.0 / view.zoom);"
"  for (let f of features) {"
"    if (f.fc !== 5 && f.fc !== 6) continue;"
"    const pts = f.pts;"
"    for (let j = 0; j < pts.length; j += 2) {"
"      let px = pts[j], py = pts[j+1];"
"      if (px < lon - snapRad || px > lon + snapRad || py < lat - snapRad || py > lat + snapRad) continue;"
"      let d = (px - lon)**2 + (py - lat)**2;"
"      if (d < snapDist) { snapDist = d; bestLon = px; bestLat = py; }"
"    }"
"  }"
"  pins.push({ lon: bestLon, lat: bestLat, name: (pins.length + 1).toString() });"
"  render();"
"  if (pins.length >= 2) await updateWebRoute();"
"});"
"canvas.addEventListener('contextmenu', async e => {"
"  e.preventDefault();"
"  const mx = e.clientX, my = e.clientY;"
"  for (let i = 0; i < pins.length; i++) {"
"    const [px, py] = lonLatToScreen(pins[i].lon, pins[i].lat);"
"    if (Math.abs(mx - px) <= 16 && (my - py) >= -38 && (my - py) <= 8) {"
"      pins.splice(i, 1);"
"      await updateWebRoute();"
"      return;"
"    }"
"  }"
"});"
"document.getElementById('routeType').addEventListener('change', updateWebRoute);"
"document.getElementById('btnCancel').addEventListener('click', () => {"
"  pins = []; routePath = []; routeSteps = [];"
"  document.getElementById('btnCancel').style.display = 'none';"
"  document.getElementById('routeType').style.display = 'none';"
"  document.getElementById('legend').style.display = 'none';"
"  render();"
"});"
"async function zoomToExtents() {"
"  try {"
"    const res = await fetch(`/api/state?w=${window.innerWidth}&h=${window.innerHeight}`);"
"    const st = await res.json();"
"    view.lon = st.lon; view.lat = st.lat; view.zoom = st.zoom; hasMaps = st.hasMaps;"
"    render(); scheduleFetch();"
"  } catch(e) {}"
"}"
"document.getElementById('btnExtents').addEventListener('click', zoomToExtents);"
"document.getElementById('btnPdf').addEventListener('click', () => {"
"  const pinStr = pins.map(p => `${p.lon},${p.lat}`).join('|');"
"  const mode = document.getElementById('routeType').value;"
"  window.location.href = `/api/pdf?title=Browser Map&lon=${view.lon}&lat=${view.lat}&zoom=${view.zoom}&w=${canvas.width}&h=${canvas.height}&mode=${mode}&pins=${encodeURIComponent(pinStr)}`;"
"});"
"const searchBox = document.getElementById('search');"
"const searchRes = document.getElementById('search-results');"
"searchBox.addEventListener('input', async () => {"
"  if (searchBox.value.length < 2) { searchRes.style.display = 'none'; return; }"
"  const res = await fetch(`/api/search?q=${encodeURIComponent(searchBox.value)}`);"
"  const list = await res.json();"
"  if (!list.length) { searchRes.style.display = 'none'; return; }"
"  searchRes.innerHTML = list.map(item => `<div class='search-item' data-lon='${item.lon}' data-lat='${item.lat}'>${item.name || ''} ${item.pcode || ''}</div>`).join('');"
"  searchRes.style.display = 'block';"
"});"
"searchRes.addEventListener('click', e => {"
"  const t = e.target.closest('.search-item'); if (!t) return;"
"  view.lon = parseFloat(t.dataset.lon); view.lat = parseFloat(t.dataset.lat); view.zoom = 25000;"
"  searchRes.style.display = 'none'; searchBox.value = '';"
"  render(); scheduleFetch();"
"});"
"resize(); zoomToExtents();"
"</script></body></html>";

static void SendHttpResponse(SOCKET s, const char* content_type, const void* data, size_t len, const char* extra_headers) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n%s\r\n",
        content_type, (unsigned int)len, extra_headers ? extra_headers : "");
    send(s, header, hlen, 0);
    send(s, (const char*)data, (int)len, 0);
}

static void HandleApiState(SOCKET s, const char* query) {
    double req_w = 1024.0, req_h = 768.0;
    const char* p;
    if ((p = strstr(query, "w="))) req_w = atof(p + 2);
    if ((p = strstr(query, "h="))) req_h = atof(p + 2);
    if (req_w < 100.0) req_w = 1024.0;
    if (req_h < 100.0) req_h = 768.0;

    double c_lon = 0.0, c_lat = 0.0, c_zoom = 5000.0;
    if (g_ChunkCount > 0) {
        double min_lon = 180.0, max_lon = -180.0, min_lat = 90.0, max_lat = -90.0;
        for (uint32_t i = 0; i < g_ChunkCount; i++) {
            if (g_Chunks[i].is_overview) continue;
            if (g_Chunks[i].max_lon - g_Chunks[i].min_lon > 90.0) continue;
            if (g_Chunks[i].min_lon < min_lon) min_lon = g_Chunks[i].min_lon;
            if (g_Chunks[i].max_lon > max_lon) max_lon = g_Chunks[i].max_lon;
            if (g_Chunks[i].min_lat < min_lat) min_lat = g_Chunks[i].min_lat;
            if (g_Chunks[i].max_lat > max_lat) max_lat = g_Chunks[i].max_lat;
        }
        if (min_lon == 180.0) {
            min_lon = g_Chunks[0].min_lon; max_lon = g_Chunks[0].max_lon;
            min_lat = g_Chunks[0].min_lat; max_lat = g_Chunks[0].max_lat;
        }
        c_lon = (min_lon + max_lon) / 2.0;
        c_lat = (min_lat + max_lat) / 2.0;
        if (max_lon - min_lon < 0.0001 || max_lat - min_lat < 0.0001) c_zoom = 25000.0;
        else {
            double zX = (req_w - 40.0) / (max_lon - min_lon);
            double zY = (req_h - 40.0) / (max_lat - min_lat);
            c_zoom = ((zX < zY) ? zX : zY) * 0.95;
        }
    }
    char json[256];
    int jlen = snprintf(json, sizeof(json), "{\"lon\":%.6f,\"lat\":%.6f,\"zoom\":%.2f,\"hasMaps\":%s}", c_lon, c_lat, c_zoom, (g_ChunkCount > 0) ? "true" : "false");
    SendHttpResponse(s, "application/json", json, jlen, NULL);
}

static void HandleApiData(SOCKET s, const char* query) {
    double min_lon = -180, max_lon = 180, min_lat = -90, max_lat = 90, zoom = 5000;
    const char* p;
    if ((p = strstr(query, "minLon="))) min_lon = atof(p + 7);
    if ((p = strstr(query, "maxLon="))) max_lon = atof(p + 7);
    if ((p = strstr(query, "minLat="))) min_lat = atof(p + 7);
    if ((p = strstr(query, "maxLat="))) max_lat = atof(p + 7);
    if ((p = strstr(query, "zoom="))) zoom = atof(p + 5);

    bool show_world = (zoom <= g_WorldMapMaxZoom && g_WorldMapPtr != NULL);
    bool use_overview = (!show_world && zoom < 500.0 && g_HasOverview);

    double main_hwy_min_zoom = g_WorldMapMaxZoom / 3.0;
    double hwy_zoom_threshold = (g_WorldMapMaxZoom * g_HighwayZoomMult) / 3.0;
    
    size_t cap = 256 * 1024, len = 4;
    uint8_t* out = (uint8_t*)malloc(cap);
    uint32_t total_written = 0;

    EnterCriticalSection(&g_FileCS);
    for (uint32_t c = 0; c < g_ChunkCount; c++) {
        MapChunk* mc = &g_Chunks[c];
        bool is_world_chunk = (mc->file == g_WorldMapPtr && g_WorldMapPtr != NULL);

        if (show_world) {
            if (!is_world_chunk) continue;
        } else {
            if (is_world_chunk) continue;
            if (use_overview && !mc->is_overview) continue;
            if (!use_overview && mc->is_overview) continue;
        }
        if (mc->max_lon < min_lon || mc->min_lon > max_lon || mc->max_lat < min_lat || mc->min_lat > max_lat) continue;

        uint8_t* data = GetChunkData(c); if (!data) continue;
        uint32_t f_cnt = *(uint32_t*)data;
        PFeat* feats = (PFeat*)(data + 12);
        float* points = (float*)(data + 12 + f_cnt * sizeof(PFeat));
        char* strings = (char*)((uint8_t*)points + (*(uint32_t*)(data + 4)) * sizeof(float));

        for (uint32_t i = 0; i < f_cnt; i++) {
            PFeat* f = &feats[i];
            if (f->pt_cnt < 2) continue;

            if (!f->is_area) {
                if (f->fc == CLASS_HWY_MINOR) {
                    if (use_overview || zoom < hwy_zoom_threshold) continue;
                } else if (f->fc == CLASS_HWY_MAIN) {
                    if (zoom < main_hwy_min_zoom) continue;
                } else if (f->fc == CLASS_PLACE || f->fc == CLASS_POI) {
                    if (use_overview || zoom < 500.0) continue;
                }
            }

            if (f->max_lon < min_lon || f->min_lon > max_lon || f->max_lat < min_lat || f->min_lat > max_lat) continue;

            double feat_w = f->max_lon - f->min_lon, feat_h = f->max_lat - f->min_lat;
            if (f->is_area && zoom < 10000.0 && (feat_w * zoom < 6.0) && (feat_h * zoom < 6.0)) continue;
            if (!use_overview && !show_world) {
                if (zoom < 50.0 && feat_w < 0.005 && feat_h < 0.005) continue; 
                if (zoom < 10.0 && feat_w < 0.02 && feat_h < 0.02) continue; 
                if (zoom < 2.0 && feat_w < 0.1 && feat_h < 0.1) continue; 
            }

            float min_step = (float)(1.5 / zoom);
            if (f->is_area) min_step = (float)(0.75 / zoom);
            if (show_world) min_step = (float)(2.5 / zoom);
            else if (use_overview) min_step = (float)(1.5 / zoom);

            float* tmp_pts = (float*)malloc(f->pt_cnt * 2 * sizeof(float));
            uint32_t out_pt_cnt = 0;
            float last_x = -999.0f, last_y = -999.0f;
            float* src_pts = points + f->pt_off;
            
            for (uint32_t j = 0; j < f->pt_cnt; j++) {
                float x = src_pts[j*2], y = src_pts[j*2+1];
                if (j == 0 || j == f->pt_cnt - 1 || 
                    fabsf(x - last_x) >= min_step || fabsf(y - last_y) >= min_step) {
                    tmp_pts[out_pt_cnt*2] = x;
                    tmp_pts[out_pt_cnt*2+1] = y;
                    last_x = x;
                    last_y = y;
                    out_pt_cnt++;
                }
            }

            if (f->is_area ? (out_pt_cnt < 3) : (out_pt_cnt < 2)) {
                free(tmp_pts);
                continue;
            }

            const char* name = f->name_off ? (strings + f->name_off) : "";
            uint16_t nlen = (uint16_t)strlen(name);
            
            if (f->fc == CLASS_HWY_MINOR && zoom < 10000.0) nlen = 0;
            if (f->fc == CLASS_HWY_MAIN && zoom < 1500.0) nlen = 0;

            size_t req = 1 + 1 + 2 + 2 + nlen + (out_pt_cnt * 2 * sizeof(float));

            if (len + req >= cap) {
                cap = (cap * 2) + req + (1024 * 1024);
                out = (uint8_t*)realloc(out, cap);
            }

            out[len++] = f->fc;
            out[len++] = f->is_area;
            uint16_t cast_cnt = (uint16_t)(out_pt_cnt > 65535 ? 65535 : out_pt_cnt);
            memcpy(out + len, &cast_cnt, 2); len += 2;
            memcpy(out + len, &nlen, 2); len += 2;
            if (nlen > 0) { memcpy(out + len, name, nlen); len += nlen; }
            memcpy(out + len, tmp_pts, cast_cnt * 2 * sizeof(float));
            len += cast_cnt * 2 * sizeof(float);
            total_written++;
            
            free(tmp_pts);
        }
    }
    LeaveCriticalSection(&g_FileCS);
    
    memcpy(out, &total_written, 4);
    SendHttpResponse(s, "application/octet-stream", out, len, NULL);
    free(out);
}

static void HandleApiRoute(SOCKET s, const char* query) {
    const char* p_pins = strstr(query, "pins=");
    if (!p_pins) { SendHttpResponse(s, "application/json", "{}", 2, NULL); return; }

    int route_mode = 0;
    const char* p_mode = strstr(query, "mode=");
    if (p_mode) route_mode = atoi(p_mode + 5);

    char raw_pbuf[2048] = {0}, pbuf[2048] = {0};
    strncpy(raw_pbuf, p_pins + 5, sizeof(raw_pbuf) - 1);
    char* end = strchr(raw_pbuf, '&'); if (end) *end = '\0';
    UrlDecode(pbuf, raw_pbuf, sizeof(pbuf));

    MapPin web_pins[1000];
    int web_pin_cnt = 0;

    char* token = strtok(pbuf, "|");
    while (token && web_pin_cnt < 1000) {
        double lon = 0, lat = 0;
        if (sscanf(token, "%lf,%lf", &lon, &lat) == 2) {
            web_pins[web_pin_cnt].lon = lon;
            web_pins[web_pin_cnt].lat = lat;
            snprintf(web_pins[web_pin_cnt].name, 128, "%d", web_pin_cnt + 1);
            web_pin_cnt++;
        }
        token = strtok(NULL, "|");
    }

    MapPoint* route_path = NULL; int route_path_cnt = 0;
    RouteStep* route_steps = NULL; int route_step_cnt = 0;
    double total_dist = 0.0; bool use_miles = false;

    CalculateRouteForPins(web_pins, web_pin_cnt, route_mode, &route_path, &route_path_cnt, &route_steps, &route_step_cnt, &total_dist, &use_miles);

    size_t j_cap = 1024 * 1024;
    char* json = (char*)malloc(j_cap);
    int jlen = snprintf(json, j_cap, "{\"totalDist\":%.2f,\"useMiles\":%s,\"steps\":[",
        total_dist, use_miles ? "true" : "false");

    for (int i = 0; i < route_step_cnt; i++) {
        if (jlen + 512 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
        char escaped[256] = {0}; int k = 0;
        for (int m = 0; route_steps[i].text[m] && k < 250; m++) {
            if (route_steps[i].text[m] == '"') escaped[k++] = '\\';
            escaped[k++] = route_steps[i].text[m];
        }
        jlen += snprintf(json + jlen, j_cap - jlen, "%s{\"text\":\"%s\",\"dist\":%.2f}",
            (i > 0 ? "," : ""), escaped, route_steps[i].dist);
    }
    if (jlen + 128 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
    jlen += snprintf(json + jlen, j_cap - jlen, "],\"path\":[");

    for (int i = 0; i < route_path_cnt; i++) {
        if (jlen + 128 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
        jlen += snprintf(json + jlen, j_cap - jlen, "%s{\"lon\":%.6f,\"lat\":%.6f}",
            (i > 0 ? "," : ""), route_path[i].lon, route_path[i].lat);
    }
    if (jlen + 128 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
    jlen += snprintf(json + jlen, j_cap - jlen, "]}");

    SendHttpResponse(s, "application/json", json, jlen, NULL);
    free(json);
    if (route_path) free(route_path);
    if (route_steps) free(route_steps);
}

static void HandleApiSearch(SOCKET s, const char* query) {
    const char* q = strstr(query, "q=");
    char raw_str[128] = {0}, search_str[128] = {0};
    if (q) {
        strncpy(raw_str, q + 2, sizeof(raw_str) - 1);
        char* end = strchr(raw_str, '&'); if (end) *end = '\0';
        UrlDecode(search_str, raw_str, sizeof(search_str));
    }

    char* json = (char*)malloc(128 * 1024);
    int jlen = snprintf(json, 128 * 1024, "[");
    int count = 0;

    for (uint32_t i = 0; i < g_SearchItemCount && count < 25; i++) {
        SearchItem* it = &g_SearchItems[i];
        bool match = (it->name && FindSubStringIC(it->name, search_str)) ||
                     (it->pcode && FindSubStringIC(it->pcode, search_str));
        if (match) {
            jlen += snprintf(json + jlen, 128 * 1024 - jlen, "%s{\"name\":\"%s\",\"pcode\":\"%s\",\"lon\":%.6f,\"lat\":%.6f}",
                count > 0 ? "," : "", it->name ? it->name : "", it->pcode ? it->pcode : "", it->lon, it->lat);
            count++;
        }
    }
    jlen += snprintf(json + jlen, 128 * 1024 - jlen, "]");
    SendHttpResponse(s, "application/json", json, jlen, NULL);
    free(json);
}

static void HandleApiPdf(SOCKET s, const char* query) {
    double lon = g_CenterLon, lat = g_CenterLat, zoom = g_Zoom;
    double view_w = 1280.0, view_h = 800.0;
    int route_mode = 0;
    char title[128] = "Web Map Export";
    const char* p;

    if ((p = strstr(query, "lon="))) lon = atof(p + 4);
    if ((p = strstr(query, "lat="))) lat = atof(p + 4);
    if ((p = strstr(query, "zoom="))) zoom = atof(p + 5);
    if ((p = strstr(query, "w="))) view_w = atof(p + 2);
    if ((p = strstr(query, "h="))) view_h = atof(p + 2);
    if ((p = strstr(query, "mode="))) route_mode = atoi(p + 5);
    if ((p = strstr(query, "title="))) {
        char raw_t[128] = {0};
        strncpy(raw_t, p + 6, sizeof(raw_t) - 1);
        char* end = strchr(raw_t, '&'); if (end) *end = '\0';
        UrlDecode(title, raw_t, sizeof(title));
    }

    MapPoint* route_path = NULL; int route_path_cnt = 0;
    RouteStep* route_steps = NULL; int route_step_cnt = 0;
    double total_dist = 0.0; bool use_miles = false;

    MapPin web_pins[1000];
    int web_pin_cnt = 0;

    const char* p_pins = strstr(query, "pins=");
    if (p_pins) {
        char raw_pbuf[2048] = {0}, pbuf[2048] = {0};
        strncpy(raw_pbuf, p_pins + 5, sizeof(raw_pbuf) - 1);
        char* end = strchr(raw_pbuf, '&'); if (end) *end = '\0';
        UrlDecode(pbuf, raw_pbuf, sizeof(pbuf));

        char* token = strtok(pbuf, "|");
        while (token && web_pin_cnt < 1000) {
            double plon = 0, plat = 0;
            if (sscanf(token, "%lf,%lf", &plon, &plat) == 2) {
                web_pins[web_pin_cnt].lon = plon;
                web_pins[web_pin_cnt].lat = plat;
                snprintf(web_pins[web_pin_cnt].name, 128, "%d", web_pin_cnt + 1);
                web_pin_cnt++;
            }
            token = strtok(NULL, "|");
        }
        if (web_pin_cnt >= 2) {
            CalculateRouteForPins(web_pins, web_pin_cnt, route_mode, &route_path, &route_path_cnt, &route_steps, &route_step_cnt, &total_dist, &use_miles);
        }
    }

    char temp_pdf[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_pdf);
    char fname[64];
    snprintf(fname, sizeof(fname), "qjm_web_%lu_%u.pdf", (unsigned long)GetCurrentThreadId(), (unsigned int)GetTickCount());
    strcat(temp_pdf, fname);

    generate_pdf_ex(
        temp_pdf, title, g_MargH, g_MargV,
        lon, lat, view_w, view_h, zoom,
        route_path, route_path_cnt,
        route_steps, route_step_cnt,
        total_dist, use_miles,
        web_pins, web_pin_cnt
    );

    if (route_path) free(route_path);
    if (route_steps) free(route_steps);

    FILE* fp = fopen(temp_pdf, "rb");
    if (!fp) { SendHttpResponse(s, "text/plain", "PDF generation failed", 21, NULL); return; }

    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc(sz);
    fread(buf, 1, sz, fp); fclose(fp);
    remove(temp_pdf);

    SendHttpResponse(s, "application/pdf", buf, sz, "Content-Disposition: attachment; filename=\"map.pdf\"\r\n");
    free(buf);
}

static void HandleApiCmd(SOCKET s, const char* query) {
    const char* q = strstr(query, "q=");
    if (!q) { SendHttpResponse(s, "application/json", "{}", 2, NULL); return; }

    char raw_str[4096] = {0}, cmd_str[4096] = {0};
    strncpy(raw_str, q + 2, sizeof(raw_str) - 1);
    char* end = strchr(raw_str, '&'); if (end) *end = '\0';
    UrlDecode(cmd_str, raw_str, sizeof(cmd_str));

    bool do_route = true;
    const char* start = cmd_str;
    if (strncmp(start, "plot:", 5) == 0) { do_route = false; start += 5; }
    else if (strncmp(start, "route:", 6) == 0) { do_route = true; start += 6; }
    else if (strncmp(start, "pins=", 5) == 0) { start += 5; }

    char buf[8192];
    strncpy(buf, start, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    MapPin web_pins[1000];
    int web_pin_cnt = 0;

    char* token = strtok(buf, "|");
    while (token && web_pin_cnt < 1000) {
        char* pLong = strstr(token, ":long:");
        char* pLat = strstr(token, ":lat:");
        if (pLong && pLat) {
            *pLong = '\0';
            strncpy(web_pins[web_pin_cnt].name, token, 127);
            web_pins[web_pin_cnt].lon = atof(pLong + 6);
            web_pins[web_pin_cnt].lat = atof(pLat + 5);
            *pLong = ':';
            web_pin_cnt++;
        } else {
            double found_lon = 0, found_lat = 0;
            bool found = false;
            EnterCriticalSection(&g_FileCS);
            for (uint32_t i = 0; i < g_SearchItemCount; i++) {
                SearchItem* item = &g_SearchItems[i];
                if ((item->pcode && strcasecmp(item->pcode, token) == 0) ||
                    (item->name && strcasecmp(item->name, token) == 0)) {
                    found_lon = item->lon;
                    found_lat = item->lat;
                    found = true;
                    break;
                }
            }
            LeaveCriticalSection(&g_FileCS);
            if (found) {
                strncpy(web_pins[web_pin_cnt].name, token, 127);
                web_pins[web_pin_cnt].lon = found_lon;
                web_pins[web_pin_cnt].lat = found_lat;
                web_pin_cnt++;
            }
        }
        token = strtok(NULL, "|");
    }

    size_t j_cap = 1024 * 1024;
    char* json = (char*)malloc(j_cap);
    int jlen = snprintf(json, j_cap, "{");

    jlen += snprintf(json + jlen, j_cap - jlen, "\"pins\":[");
    for (int i = 0; i < web_pin_cnt; i++) {
        char escaped[256] = {0}; int k = 0;
        for (int m = 0; web_pins[i].name[m] && k < 250; m++) {
            if (web_pins[i].name[m] == '"') escaped[k++] = '\\';
            escaped[k++] = web_pins[i].name[m];
        }
        jlen += snprintf(json + jlen, j_cap - jlen, "%s{\"name\":\"%s\",\"lon\":%.6f,\"lat\":%.6f}",
            (i > 0 ? "," : ""), escaped, web_pins[i].lon, web_pins[i].lat);
    }
    jlen += snprintf(json + jlen, j_cap - jlen, "]");

    if (do_route && web_pin_cnt >= 2) {
        MapPoint* route_path = NULL; int route_path_cnt = 0;
        RouteStep* route_steps = NULL; int route_step_cnt = 0;
        double total_dist = 0.0; bool use_miles = false;

        CalculateRouteForPins(web_pins, web_pin_cnt, 0, &route_path, &route_path_cnt, &route_steps, &route_step_cnt, &total_dist, &use_miles);

        jlen += snprintf(json + jlen, j_cap - jlen, ",\"totalDist\":%.2f,\"useMiles\":%s,\"steps\":[", total_dist, use_miles ? "true" : "false");
        for (int i = 0; i < route_step_cnt; i++) {
            if (jlen + 512 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
            char escaped[256] = {0}; int k = 0;
            for (int m = 0; route_steps[i].text[m] && k < 250; m++) {
                if (route_steps[i].text[m] == '"') escaped[k++] = '\\';
                escaped[k++] = route_steps[i].text[m];
            }
            jlen += snprintf(json + jlen, j_cap - jlen, "%s{\"text\":\"%s\",\"dist\":%.2f}",
                (i > 0 ? "," : ""), escaped, route_steps[i].dist);
        }
        
        if (jlen + 128 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
        jlen += snprintf(json + jlen, j_cap - jlen, "],\"path\":[");
        for (int i = 0; i < route_path_cnt; i++) {
            if (jlen + 128 >= (int)j_cap) { j_cap *= 2; json = (char*)realloc(json, j_cap); }
            jlen += snprintf(json + jlen, j_cap - jlen, "%s{\"lon\":%.6f,\"lat\":%.6f}",
                (i > 0 ? "," : ""), route_path[i].lon, route_path[i].lat);
        }
        jlen += snprintf(json + jlen, j_cap - jlen, "]");

        if (route_path) free(route_path);
        if (route_steps) free(route_steps);
    }
    
    jlen += snprintf(json + jlen, j_cap - jlen, "}");
    SendHttpResponse(s, "application/json", json, jlen, NULL);
    free(json);
}

static DWORD WINAPI ClientWorkerThread(LPVOID lpParam) {
    SOCKET client = (SOCKET)(uintptr_t)lpParam;
    char req[4096] = {0};
    int rlen = recv(client, req, sizeof(req) - 1, 0);
    if (rlen > 0) {
        req[rlen] = '\0';
        char method[16] = {0}, uri[2048] = {0};
        sscanf(req, "%15s %2047s", method, uri);

        char* query = strchr(uri, '?');
        if (query) { *query = '\0'; query++; } else query = "";

        if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
            SendHttpResponse(client, "text/html", kHtmlApp, strlen(kHtmlApp), NULL);
        } else if (strcmp(uri, "/api/data") == 0) {
            HandleApiData(client, query);
        } else if (strcmp(uri, "/api/route") == 0) {
            HandleApiRoute(client, query);
        } else if (strcmp(uri, "/api/search") == 0) {
            HandleApiSearch(client, query);
        } else if (strcmp(uri, "/api/state") == 0) {
            HandleApiState(client, query);
        } else if (strcmp(uri, "/api/pdf") == 0) {
            HandleApiPdf(client, query);
        } else if (strcmp(uri, "/api/cmd") == 0) {
            HandleApiCmd(client, query);
        } else {
            const char* not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send(client, not_found, (int)strlen(not_found), 0);
        }
    }
    closesocket(client);
    return 0;
}

static DWORD WINAPI HttpServerThread(LPVOID lpParam) {
    (void)lpParam;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    int port = 80;
    FILE* ini = fopen("qjmnavig.ini", "r");
    if (ini) {
        char line[256];
        while (fgets(line, sizeof(line), ini)) {
            if (strncmp(line, "port=", 5) == 0) port = atoi(line + 5);
            else if (strncmp(line, "httpPort=", 9) == 0) port = atoi(line + 9);
        }
        fclose(ini);
    }
    if (port <= 0 || port > 65535) port = 80;

    g_HttpServerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_HttpServerSocket == INVALID_SOCKET) { WSACleanup(); return 1; }

    BOOL opt = TRUE;
    setsockopt(g_HttpServerSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(g_HttpServerSocket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        addr.sin_port = htons(8080);
        if (bind(g_HttpServerSocket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(g_HttpServerSocket);
            WSACleanup();
            return 1;
        }
        port = 8080;
    }

    if (listen(g_HttpServerSocket, 16) == SOCKET_ERROR) {
        closesocket(g_HttpServerSocket);
        WSACleanup();
        return 1;
    }

    printf("[+] Embedded QJM HTTP Server online at http://localhost:%d/\n", port);
    g_HttpServerRunning = true;

    while (g_HttpServerRunning) {
        SOCKET client = accept(g_HttpServerSocket, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        CreateThread(NULL, 0, ClientWorkerThread, (LPVOID)(uintptr_t)client, 0, NULL);
    }

    closesocket(g_HttpServerSocket);
    WSACleanup();
    return 0;
}

static inline void StartHttpServer(void) {
    if (!g_HttpServerRunning) {
        g_HttpServerThread = CreateThread(NULL, 0, HttpServerThread, NULL, 0, NULL);
    }
}

static inline void StopHttpServer(void) {
    g_HttpServerRunning = false;
    if (g_HttpServerSocket != INVALID_SOCKET) {
        closesocket(g_HttpServerSocket);
        g_HttpServerSocket = INVALID_SOCKET;
    }
    if (g_HttpServerThread) {
        WaitForSingleObject(g_HttpServerThread, 500);
        CloseHandle(g_HttpServerThread);
        g_HttpServerThread = NULL;
    }
}

#endif // QJMHTTPD_H