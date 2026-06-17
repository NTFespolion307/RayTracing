cmake_minimum_required(VERSION 3.24)
project(VkGigaTracer LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Statically link the MSVC runtime so the exe needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Keep FetchContent downloads in a shared cache so reconfigures don't re-download.
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/_deps_cache" CACHE PATH "")
endif()

if(MSVC)
    add_compile_options(/MP /bigobj /utf-8 /EHsc /wd4244 /wd4267 /wd4305)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
                            UNICODE _UNICODE VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies (all pinned, built from source, statically linked)
# ---------------------------------------------------------------------------

# --- Vulkan-Headers (provides Vulkan::Headers target) ---
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.3.296
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(VulkanHeaders)

# --- volk (dynamic Vulkan loader; picks up Vulkan::Headers) ---
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.3.296.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(volk)

# --- Vulkan Memory Allocator (header only; we compile the impl ourselves) ---
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(vma)
if(NOT vma_POPULATED)
    FetchContent_Populate(vma)
endif()

# --- GLFW (static) ---
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui (no CMake of its own; we build the sources we need) ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# --- tinyobjloader (single header) ---
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG        v1.0.6
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(tinyobjloader)
if(NOT tinyobjloader_POPULATED)
    FetchContent_Populate(tinyobjloader)
endif()

# --- zlib (static) ---
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(zlib)
# zlib's CMake creates zlibstatic; expose include dirs (source + generated zconf.h).
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")

# --- libpng: we compile the sources ourselves to avoid its find_package(ZLIB) ---
FetchContent_Declare(libpng
    GIT_REPOSITORY https://github.com/pnggroup/libpng.git
    GIT_TAG        v1.6.44
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(libpng)
if(NOT libpng_POPULATED)
    FetchContent_Populate(libpng)
endif()
# Provide pnglibconf.h from the shipped prebuilt config.
configure_file(${libpng_SOURCE_DIR}/scripts/pnglibconf.h.prebuilt
               ${libpng_BINARY_DIR}/pnglibconf.h COPYONLY)
add_library(png_static STATIC
    ${libpng_SOURCE_DIR}/png.c
    ${libpng_SOURCE_DIR}/pngerror.c
    ${libpng_SOURCE_DIR}/pngget.c
    ${libpng_SOURCE_DIR}/pngmem.c
    ${libpng_SOURCE_DIR}/pngpread.c
    ${libpng_SOURCE_DIR}/pngread.c
    ${libpng_SOURCE_DIR}/pngrio.c
    ${libpng_SOURCE_DIR}/pngrtran.c
    ${libpng_SOURCE_DIR}/pngrutil.c
    ${libpng_SOURCE_DIR}/pngset.c
    ${libpng_SOURCE_DIR}/pngtrans.c
    ${libpng_SOURCE_DIR}/pngwio.c
    ${libpng_SOURCE_DIR}/pngwrite.c
    ${libpng_SOURCE_DIR}/pngwtran.c
    ${libpng_SOURCE_DIR}/pngwutil.c)
target_compile_definitions(png_static PRIVATE PNG_INTEL_SSE_OPT=0 PNG_ARM_NEON_OPT=0)
target_include_directories(png_static PUBLIC
    ${libpng_SOURCE_DIR} ${libpng_BINARY_DIR} ${ZLIB_INCLUDE_DIRS})
target_link_libraries(png_static PUBLIC zlibstatic)

# --- libjpeg-turbo (static, SIMD off so no NASM needed) ---
# libjpeg-turbo forbids add_subdirectory() integration, so we build it as an
# isolated ExternalProject and link the resulting static library.
FetchContent_Declare(libjpegturbo
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
    GIT_TAG        3.0.4
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(libjpegturbo)
if(NOT libjpegturbo_POPULATED)
    FetchContent_Populate(libjpegturbo)
endif()

include(ExternalProject)
set(JPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/jpegturbo-build")
ExternalProject_Add(jpegturbo_ext
    SOURCE_DIR       "${libjpegturbo_SOURCE_DIR}"
    BINARY_DIR       "${JPEG_BUILD_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND   ""
    CMAKE_ARGS
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_SIMD=OFF
        -DWITH_TURBOJPEG=OFF
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    BUILD_COMMAND    ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target jpeg-static
    INSTALL_COMMAND  ""
    BUILD_BYPRODUCTS "${JPEG_BUILD_DIR}/$<CONFIG>/jpeg-static.lib")

# Per-config imported locations (generator expressions are not expanded inside
# IMPORTED_LOCATION on multi-config generators, so set each config explicitly).
add_library(jpegturbo STATIC IMPORTED GLOBAL)
set_target_properties(jpegturbo PROPERTIES
    IMPORTED_CONFIGURATIONS "RELEASE;DEBUG;RELWITHDEBINFO;MINSIZEREL"
    IMPORTED_LOCATION_RELEASE        "${JPEG_BUILD_DIR}/Release/jpeg-static.lib"
    IMPORTED_LOCATION_DEBUG          "${JPEG_BUILD_DIR}/Debug/jpeg-static.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${JPEG_BUILD_DIR}/RelWithDebInfo/jpeg-static.lib"
    IMPORTED_LOCATION_MINSIZEREL     "${JPEG_BUILD_DIR}/MinSizeRel/jpeg-static.lib"
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RELWITHDEBINFO;RELEASE"
    MAP_IMPORTED_CONFIG_MINSIZEREL     "MINSIZEREL;RELEASE"
    INTERFACE_INCLUDE_DIRECTORIES "${libjpegturbo_SOURCE_DIR};${JPEG_BUILD_DIR}")

# --- Embeddable Python (build-time only; glslang's code generators need it) ---
# Fetched automatically so the build machine needs nothing but VS, CMake, net.
FetchContent_Declare(pythonembed
    URL https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip)
FetchContent_GetProperties(pythonembed)
if(NOT pythonembed_POPULATED)
    FetchContent_Populate(pythonembed)
endif()
set(Python3_EXECUTABLE "${pythonembed_SOURCE_DIR}/python.exe" CACHE FILEPATH "" FORCE)
set(Python_EXECUTABLE  "${pythonembed_SOURCE_DIR}/python.exe" CACHE FILEPATH "" FORCE)

# --- glslang (host shader compiler; no SPIRV-Tools optimizer to keep it lean) ---
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON  CACHE BOOL "" FORCE)
set(GLSLANG_TESTS          OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL         OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        14.3.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glslang)

# ---------------------------------------------------------------------------
# Dear ImGui static library (core + GLFW + Vulkan backends)
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw Vulkan::Headers)
# The Vulkan backend must not pull in static prototypes; it uses a loader we feed.
target_compile_definitions(imgui PUBLIC VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# ---------------------------------------------------------------------------
# Shader compilation: GLSL -> SPIR-V (two variants) -> embedded C headers
# ---------------------------------------------------------------------------
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GEN_DIR})

set(SHADER_SRC   "${CMAKE_SOURCE_DIR}/shaders/pathtrace.comp")
set(SHADER_COMMON "${CMAKE_SOURCE_DIR}/shaders/common.glsl")

# The standalone glslang front-end executable target.
set(GLSLANG_EXE glslang-standalone)

function(compile_and_embed variant_name target_env defines)
    set(spv "${GEN_DIR}/${variant_name}.spv")
    set(hdr "${GEN_DIR}/${variant_name}.h")
    add_custom_command(
        OUTPUT ${spv}
        COMMAND ${GLSLANG_EXE} -V --target-env ${target_env} ${defines}
                -S comp "${SHADER_SRC}" -o "${spv}"
        DEPENDS ${SHADER_SRC} ${SHADER_COMMON} ${GLSLANG_EXE}
        COMMENT "Compiling ${variant_name}.spv"
        VERBATIM)
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr}
                -DVARNAME=${variant_name}_spv
                -P "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
        DEPENDS ${spv} "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
        COMMENT "Embedding ${variant_name}.spv"
        VERBATIM)
endfunction()

compile_and_embed(pathtrace_compute  vulkan1.1 "-DUSE_RAY_QUERY=0")
compile_and_embed(pathtrace_rayquery vulkan1.2 "-DUSE_RAY_QUERY=1")

add_custom_target(shaders DEPENDS
    ${GEN_DIR}/pathtrace_compute.h
    ${GEN_DIR}/pathtrace_rayquery.h)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp
    src/vk_context.cpp
    src/vma_impl.cpp
    src/mmap_image.cpp
    src/scene.cpp
    src/obj_loader.cpp
    src/bvh.cpp
    src/accel.cpp
    src/renderer.cpp
    src/viewer.cpp
    src/savers.cpp
    src/theme.cpp
    src/ui.cpp)

add_executable(VkGigaTracer WIN32 ${APP_SOURCES})
add_dependencies(VkGigaTracer shaders jpegturbo_ext)

target_include_directories(VkGigaTracer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${vma_SOURCE_DIR}/include
    ${tinyobjloader_SOURCE_DIR}
    ${ZLIB_INCLUDE_DIRS})

target_link_libraries(VkGigaTracer PRIVATE
    volk
    Vulkan::Headers
    glfw
    imgui
    png_static
    zlibstatic
    jpegturbo
    Dwmapi
    Shcore
    Shlwapi
    Ole32
    Shell32)

set_target_properties(VkGigaTracer PROPERTIES OUTPUT_NAME "VkGigaTracer")
