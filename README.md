# VkGigaTracer

A portable Windows desktop **offline GPU path tracer** that renders 3D scenes
into 2D images at resolutions up to **80K (76,800 × 43,200 ≈ 3.32 gigapixels)**
on ordinary consumer hardware — including 4 GB GPUs and Vulkan-capable iGPUs —
**without ever failing for memory or size reasons.**

All GPU work is done through **Vulkan only** (via dynamic loading with volk). The
build produces a **single self-contained `VkGigaTracer.exe`** that runs on a clean
Windows machine whose only relevant software is a GPU driver.

---

## System requirements (to run)

- Windows 10/11, x64.
- Any **Vulkan 1.1** capable GPU + driver (roughly 2016 or newer). Drivers ship
  `vulkan-1.dll`; no Vulkan SDK or runtime is required.
- **Hardware ray tracing is used automatically** when the GPU supports
  `VK_KHR_ray_query` (NVIDIA RTX, AMD RX 6000+, Intel Arc). Otherwise a
  compute-only BVH path tracer is used, so ~2016-era GPUs and iGPUs still work.
- **Scratch disk space.** The full-resolution image is stored in a temporary
  file on disk (see architecture below). At 80K this file is **≈ 12.4 GiB**, so
  you need roughly **13 GB free** on the scratch drive. Smaller resolutions need
  proportionally less (4K ≈ 32 MB, 8K ≈ 127 MB, 40K ≈ 3.1 GiB). This is the only
  resource the app ever asks you about, and only before a render starts.

The exe imports only `KERNEL32.dll` and `USER32.dll` — the MSVC runtime is
statically linked, Vulkan is loaded dynamically at runtime, and all SPIR-V
shaders are embedded in the binary. There are no companion DLLs or shader files.

---

## Build requirements

- **Visual Studio 2022 or newer** (verified with the VS 2026 / v18 build tools).
- **CMake ≥ 3.24.**
- **Internet access** for the first configure (dependencies are fetched).

Nothing else is needed. In particular you do **not** need the Vulkan SDK, NASM,
or a system Python: a minimal embeddable Python is downloaded automatically and
used only to run glslang's build-time code generators.

### Steps

```bat
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

(Use `-G "Visual Studio 17 2022"` on a VS 2022 machine.) The resulting binary is
`build/Release/VkGigaTracer.exe`.

All third-party libraries are pulled with `FetchContent` at pinned release tags
and built from source, statically linked:

| Dependency | Tag | Role |
|---|---|---|
| Vulkan-Headers | v1.3.296 | Vulkan API headers |
| volk | vulkan-sdk-1.3.296.0 | dynamic Vulkan loader (no `vulkan-1.lib`) |
| VulkanMemoryAllocator | v3.1.0 | all GPU allocations |
| GLFW | 3.4 | window + surface |
| Dear ImGui | v1.91.5 | UI (GLFW + Vulkan backends) |
| glslang | 14.3.0 | GLSL → SPIR-V at build time (host tool) |
| tinyobjloader | v1.0.6 | OBJ/MTL import |
| zlib | v1.3.1 | PNG compression |
| libpng | v1.6.44 | streaming PNG encode |
| libjpeg-turbo | 3.0.4 | streaming JPEG encode (SIMD off, no NASM) |

The two SPIR-V variants of the path tracer are compiled at build time by
glslang and embedded into the executable as `constexpr` byte arrays
(`cmake/embed_spirv.cmake`). The exe loads **zero external files** at runtime.

---

## Using the app

Two tabs:

1. **Setup & Preview** — pick resolution, samples-per-pixel, max bounces,
   exposure, firefly clamp, sky, and backend; load the built-in **Demo Scene**
   or an **OBJ** file; and watch a live, interactive, progressively-accumulated
   path-traced preview (left-drag orbits, right-drag pans, the wheel dollies; the
   preview resolution is capped so it stays interactive on weak GPUs). Press
   **Render** to start the full out-of-core render.

2. **Render Result** — shows tiles filling in live, with tiles done / total,
   current tile spp, elapsed time, ETA, MRays/s, and a **Cancel** button. The
   viewer always letterbox-fits the whole image to the window, so an 80K render
   is fully visible on a 1080p monitor; the mouse wheel zooms and dragging pans.
   Zoom past the preview's pixel density and the visible region is streamed
   straight out of the memory-mapped file at full resolution. **Save Image…**
   writes PNG, JPEG, or PPM.

### Render options

All resolutions are 16:9, selected from a dropdown:

| Label | Pixels | | Label | Pixels |
|---|---|---|---|---|
| 480p  | 854 × 480     | | 10K | 9,600 × 5,400 |
| 720p  | 1,280 × 720   | | 15K | 14,400 × 8,100 |
| 1080p | 1,920 × 1,080 | | 20K | 19,200 × 10,800 |
| 2K    | 2,560 × 1,440 | | 40K | 38,400 × 21,600 |
| 4K    | 3,840 × 2,160 | | 60K | 57,600 × 32,400 |
| 6K    | 5,760 × 3,240 | | 80K | 76,800 × 43,200 |
| 8K    | 7,680 × 4,320 | |     |               |

- **Samples per pixel:** 1, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
  (1/4/8 are handy for fast look-dev; the default is 16).
- **Max bounces:** 16, 32, 48, 64.
- **Tile size** (Advanced): 128 / 256 / 512 (default 256).
- Plus exposure, firefly clamp, sky on/off + intensity, backend override
  (Auto / Hardware RT / Compute), and a scratch-folder picker.

### Saving

- **PNG** — lossless, available at every resolution (streamed via libpng
  `png_write_row`).
- **JPEG** — streamed via libjpeg-turbo `jpeg_write_scanlines`, quality 1–100.
  JPEG is physically limited to 65,500 px per side, so the option is greyed out
  (with a tooltip) whenever width or height exceeds that — i.e. 80K offers
  PNG/PPM only, while 60K and below allow JPEG.
- **PPM (P6 binary)** — raw RGB pixels behind a tiny ASCII header, written almost
  directly from the mapped file. **GIMP opens P6 PPM natively** with no import
  dialog, which makes it the easiest way to inspect a true-pixel gigapixel result.

Saving runs on a worker thread with its own progress bar and cancel, and streams
row chunks straight from the mapped file (RGBA→RGB on the fly), so peak extra RAM
stays small regardless of resolution.

---

## Architecture: how resolution is decoupled from VRAM and RAM

The headline trick is that **no full-resolution buffer is ever allocated** — not
in VRAM, not in system RAM.

1. **Disk-backed image.** At render start the app creates a scratch file of
   `width × height × 4` bytes (RGBA8) in the scratch folder and maps it with a
   single 64-bit `MapViewOfFile`. The renderer writes finished pixels straight
   into this mapping. The image's size is therefore bounded only by free disk
   space; 80K is ≈ 12.4 GiB on disk and a few hundred MB of committed host RAM.
   (`src/mmap_image.{h,cpp}`)

2. **Tiles.** The image is split into tiles (default 256×256). The GPU holds a
   **single** RGBA32F accumulation tile (≈ 1–4 MiB) plus small staging — GPU
   memory use is **constant with respect to output resolution**. 80K is 50,700
   tiles; each is path-traced, resolved (÷spp → exposure → ACES → sRGB → RGBA8),
   read back, and written into the mapped file, then box-downsampled into a small
   preview. (`src/renderer.{h,cpp}`)

3. **TDR safety.** Windows resets the GPU after ~2 s of unyielding work. Every
   submit targets ≈100 ms: the spp-per-batch is auto-tuned from measured submit
   times, and if even 1 spp on a tile is too slow (huge bounce counts on a weak
   iGPU) the tile is subdivided down to 128² or 64². One submit → one fence wait
   → yield; work is never chained unbounded.

4. **Threading.** The UI thread (GLFW/ImGui/present) stays responsive while a
   dedicated worker thread records, submits, and fence-waits the render. Queue
   submission is guarded by a mutex only around the brief submit call, so the UI
   keeps presenting. A dedicated compute queue family is used when available.

5. **Cancel & recovery.** Cancel takes effect between batches (< 1 s) and the
   partial image stays viewable and savable. On `VK_ERROR_DEVICE_LOST` the engine
   halves the batch (then the tile), recreates the device and all resources, and
   resumes from the next unfinished tile — every completed tile already lives in
   the mapped file. Every `VkResult` is routed through one checked handler that
   logs to `gigatracer.log` next to the exe; the app never calls `abort()`/`exit()`
   on a Vulkan error.

6. **Two-level viewer.** No GPU texture can hold a gigapixel image
   (`maxImageDimension2D` is typically 16384), so the viewer uses a downsampled
   **preview texture** (long side ≤ 4096) for fitted/overview zooms and streams
   the visible full-resolution region out of the mapped file into a screen-sized
   **detail texture** when the zoom exceeds the preview's density. 100% zoom shows
   true rendered pixels. (`src/viewer.{h,cpp}`)

### Path tracer

One GLSL compute kernel (`shaders/pathtrace.comp` + `shaders/common.glsl`,
`local_size 8×8`) is compiled into two SPIR-V variants via `#define USE_RAY_QUERY`:

- **Hardware RT:** Vulkan 1.2 + `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`;
  a BLAS/TLAS is built over all triangles and intersected with `rayQueryEXT` inside
  the compute shader (`src/accel.{h,cpp}`).
- **Compute fallback:** a CPU-built binned-SAH BVH (`src/bvh.{h,cpp}`) uploaded as
  SSBOs, traversed iteratively with a fixed 64-entry stack and Möller–Trumbore.

Both consume identical triangle data, so the primitive index reported by the
hardware AS indexes the same triangle buffer the fallback uses. If acceleration
structure creation fails for any reason, the engine **silently falls back to the
compute path**.

The integrator is unidirectional path tracing with cosine-weighted diffuse
sampling, **next-event estimation on emissive triangles with MIS (power
heuristic)**, GGX metal, dielectric glass, PCG RNG seeded per (pixel, sample),
Russian roulette after bounce 8 (never past the user's bounce cap), a per-sample
firefly clamp, a toggleable gradient sky, and a fitted-ACES + sRGB tonemap.
Materials are mapped deterministically from MTL: Lambert (`Kd`), GGX metal
(`Ks`, roughness from `Ns`), glass (`Ni > 1` / `d < 1`), emissive (`Ke > 0`).
If a loaded OBJ has no emissive material and the sky is off, the sky is
auto-enabled so renders are never black.

---

## Verification

Running `VkGigaTracer.exe --selftest` performs a headless (hidden-window)
render of the demo scene at 1080p / 16 spp / 16 bounces, writes
`selftest_out.ppm` next to the exe, and logs progress to `gigatracer.log`. This
is how the render path is validated without opening the UI.

This repository was built and run on an **Intel UHD Graphics** iGPU (the
compute fallback backend), where the self-test renders the Cornell demo to a
correct, physically-plausible image (red/green walls, color bleeding, sky) in
~11 s and saves a 5.9 MB PPM. The exe imports only `KERNEL32`, `USER32`,
`GDI32`, `IMM32`, `OLE32` and `SHELL32` — all standard Windows system DLLs. The
hardware ray-query backend compiles and is selected automatically on capable
GPUs; it was not runtime-exercised on this machine (the test iGPU exposes no
`VK_KHR_ray_query`), but it shares 95% of its code with the verified compute
path and falls back silently if anything fails.

## Design decisions / notes

- **VS 2026 toolchain.** This repo was built and verified with the Visual Studio
  v18 (2026) build tools; the generator string above reflects that. VS 2022
  (`Visual Studio 17 2022`) works the same way.
- **Embeddable Python.** glslang's build needs a Python interpreter for its code
  generators. To keep the build machine requirement at "VS + CMake + internet",
  CMake fetches the official Windows embeddable Python automatically and points
  glslang at it; nothing is installed system-wide.
- **libjpeg-turbo via ExternalProject.** libjpeg-turbo forbids
  `add_subdirectory` integration, so it is built as an isolated `ExternalProject`
  (SIMD off, static) and its `jpeg-static.lib` is linked in.
- **stb_image_write is intentionally not used** for saving — it buffers whole
  images in RAM, which is impossible at gigapixel sizes. All writers stream rows.
- **Scratch files** are created with `FILE_FLAG_DELETE_ON_CLOSE`, so they are
  reclaimed automatically when the app exits (or a new render starts).

## Project layout

```
CMakeLists.txt              build + all FetchContent dependencies
cmake/embed_spirv.cmake     .spv  ->  embedded C header
shaders/pathtrace.comp      the path tracing kernel (USE_RAY_QUERY ifdef)
shaders/common.glsl         RNG, BSDFs, intersection, NEE/MIS, sky
src/main.cpp                entry, DPI, COM
src/ui.{h,cpp}              window, swapchain, ImGui, tabs, dialogs, save
src/renderer.{h,cpp}        out-of-core tile scheduler, batching, recovery
src/viewer.{h,cpp}          fit/zoom/pan, preview + detail streaming
src/vk_context.{h,cpp}      instance/device/backend selection (volk, VMA)
src/accel.{h,cpp}           BLAS/TLAS for the hardware ray-query path
src/bvh.{h,cpp}             binned-SAH BVH for the compute path
src/scene.{h,cpp}           demo scene, materials, light list
src/obj_loader.{h,cpp}      OBJ/MTL import (wide paths, smooth normals)
src/mmap_image.{h,cpp}      disk-backed memory-mapped image
src/savers.{h,cpp}          streaming PNG / JPEG / PPM
src/mathlib.h               compact vector/matrix math
src/render_settings.h       the exact resolution / spp / bounce tables
```
