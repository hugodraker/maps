QJM Navigation & Cartography Toolchain
A lightweight, standalone Win32 C mapping stack and vector rendering pipeline built entirely without heavyweight external GIS frameworks. Designed for extreme processing throughput, low memory footprint, and high-performance interactive desktop rendering.
The toolchain consists of two native applications:
`pbf2qjm`: An out-of-core OpenStreetMap (`.pbf`) and ESRI Shapefile (`.shp`) compiler that processes multi-gigabyte extracts into spatially clustered, LZAV-compressed multi-resolution slabs (`.qjm`).
`qjmnavig`: A hardware-efficient Win32 GDI interactive vector map viewer featuring turn-by-turn routing, PDF route/map generation, and an integrated lightweight HTTP daemon for web/mobile mirror navigation.
Both tools are public domain software released without warranties.
---
Architecture & Binary Formats
1. Spatial Slab Architecture (`.qjm` / `QJM2`)
Standard GIS formats suffer from massive pointer indirection and quadratic memory overhead on continent-scale datasets. The `.qjm` container resolves this by organizing vector elements into discrete Morton Z-curve sorted spatial slabs:
Spatial Morton Clustering: Coordinate centroids are converted to 30-bit integer coordinates and interleaved into 60-bit Morton codes (Z-order curve). Spatial proximity in 2D space translates directly to 1D linear locality on disk.
Zero-Allocation Slab Packing: Features, point streams, and string tables (names, postcodes) are packed into contiguous linear memory blocks.
LZAV Compression: Fast block compression provides high decompression throughput with bounded memory consumption.
Dual-Tier LOD Hierarchy (`QJM2`): High-detail vector geometry lives alongside an automatically decimated overview layer (`LOD overview`), enabling smooth continent-wide zoom-outs without viewport lag.
2. Out-of-Core External Merge Sort
To allow compiling continent-sized PBF files (such as 18GB+ North America extracts) within 32-bit address spaces (`-m32`), `pbf2qjm` uses a disk-backed K-way external merge sort:
Pass 4 streams raw spatial Morton keys and byte offsets to a temporary file (`.sort.tmp`), bypassing 32-bit RAM heap fragmentation limits.
Pass 5 sorts disk runs in 5,000,000-feature slices and runs an external Min-Heap tournament tree merge to produce a sequentially clustered database on disk without RAM exhaustion.
---
Tool Overview
`pbf2qjm` — High-Speed Out-of-Core Compiler
Streaming Node Resolution: 2-pass sequential ping-pong resolution matches OSM ways against node coordinates in streaming chunks without needing multi-gigabyte in-memory node caches.
Outward Coastline Assembly: Seed-and-grow polygon stitching automatically closes broken, segmented OSM coastal lines and handles estuary bridging.
Shapefile Fusion: Accepts secondary water and coastline boundary shapefiles (`.shp`) to stitch islands and complicated coastlines with polygon validation.
Layer Retention & Simplification: Configurable per-layer retention sliders for Land, Coastline, Main Roads, Minor Roads, Water, Parks, and Postcodes.
Crash-Resistant Resumption: Checkpoints intermediate temporary stages (`.raw.tmp`, `.coasts.tmp`, `.ways.resolved.tmp`) to allow resuming interrupted jobs without re-parsing raw PBF blobs.
`qjmnavig` — Map Viewer, Routing & PDF Engine
Native Win32 GDI Rendering: Sub-pixel anti-aliased polyline rasterization, polygon clipping, and real-time panning/zooming without DirectX or OpenGL dependencies.
Turn-by-Turn Routing Engine: Built-in graph routing with interactive waypoint pin drops, distance/time calculation, route legs, and automated legend positioning.
Vector PDF Generator: Generates high-resolution vector PDF roadmaps with thick polyline styling, legend boxes, and crisp scalable text directly through native PDF stream generation (no third-party PDF libraries).
Embedded HTTP Server (`qjmhttpd`): Optional lightweight daemon serving an interactive web-canvas client for browser-based map viewing and route synchronization.
---
Build Instructions
Compiles with MinGW GCC (32-bit with large-address-aware flag, or 64-bit) using minimal system library links.
Prerequisites
MinGW-w64 (`gcc`)
Windows SDK headers/libraries (`comdlg32`, `gdi32`, `shell32`, `ws2_32`)
Compiling `pbf2qjm`
Build with `--large-address-aware` to unlock full 3GB/4GB user space under 32-bit Windows:
```cmd
gcc -O3 -m32 -mwindows -Wl,--large-address-aware -Wall -Wextra pbf2qjm.c -o pbf2qjm.exe -lcomdlg32 -lgdi32 -lshell32
```
(For 64-bit builds, replace `-m32` with `-m64`).
Compiling `qjmnavig`
```cmd
gcc -O3 -m32 -mwindows -Wl,--large-address-aware -Wall -Wextra qjmnavig.c -o qjmnavig.exe -lcomdlg32 -lgdi32 -lws2_32 -lshell32
```
---
Usage
Compiling a Map (`pbf2qjm`)
GUI Mode
Launch `pbf2qjm.exe` without arguments:
Drag and drop your `.pbf` map extract onto the window.
(Optional) Select a secondary reference map (`.qjm`) or water polygon shapefile (`.shp`).
Configure layer checkmarks and feature node retention percentages.
Click Compile.
CLI / Automated Mode
```cmd
pbf2qjm.exe north-america-latest.osm.pbf north-america.qjm
```
Viewing and Routing (`qjmnavig`)
```cmd
qjmnavig.exe north-america.qjm
```
Pan/Zoom: Mouse drag to pan; mouse wheel to zoom.
Drop Pins: Left-click to place start, intermediate, and destination routing pins.
Routing: The shortest driving path calculates automatically across topological highway segments.
Export PDF: Click the PDF action button to generate an export-ready vector PDF document of the active viewport and route directions.
Web UI Mirror: Enable the HTTP server via `pbf2qjm.ini` / `qjmnavig.ini` to connect any mobile device or browser on the local subnet to port 80 (or configured port).
---
Configuration (`pbf2qjm.ini` / `qjmnavig.ini`)
Runtime configurations can be persisted via INI files placed in the application root directory:
```ini
[MapData]
CoastlineShapefile=C:\Maps\land-polygons-complete-4326\land_polygons.shp

[Server]
Port=80
EnableHttp=1
```
---
License
This work is released into the Public Domain unencumbered without warranties of any kind. You are free to modify, distribute, vendor, or use the source code for any purpose, commercial or personal.
