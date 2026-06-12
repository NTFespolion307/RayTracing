// scene.h - CPU scene representation and the GPU-facing POD layouts that are
// uploaded as SSBOs/UBO. The GPU structs must match the std430/std140 layout
// used in shaders/pathtrace.comp.
#pragma once
#include "mathlib.h"
#include <vector>
#include <cstdint>
#include <string>

// Material categories (kept in sync with common.glsl MAT_* constants).
enum MaterialType : uint32_t {
    MAT_LAMBERT  = 0,
    MAT_METAL    = 1,
    MAT_GLASS    = 2,
    MAT_EMISSIVE = 3,
};

// ---- GPU POD layouts (std430) ----
struct GPUVertex {          // 32 bytes
    vec4 pos;               // xyz position, w unused
    vec4 normal;            // xyz normal, w unused
};

struct GPUTriangle {        // 16 bytes
    uint32_t v0, v1, v2;    // vertex indices
    uint32_t material;      // material index
};

struct GPUMaterial {        // 48 bytes
    vec4 albedo;            // rgb base color
    vec4 emission;          // rgb emission (radiance)
    vec4 params;            // x=type, y=roughness, z=ior, w=unused
};

struct GPUBVHNode {         // 32 bytes
    vec4 lo;               // xyz aabb min, w = leftFirst (uint bits)
    vec4 hi;               // xyz aabb max, w = triCount (uint bits, 0 => interior)
};

struct GPULight {           // 64 bytes - an emissive triangle for NEE
    vec4 p0;               // xyz position, w unused
    vec4 p1;
    vec4 p2;
    vec4 emission;         // rgb radiance, w = area
};

// std140 uniform block shared with the shader.
struct GPUParams {
    vec4 camOrigin;        // xyz
    vec4 camU;             // right * tan(fov/2) * aspect
    vec4 camV;             // up    * tan(fov/2)
    vec4 camW;             // forward
    vec4 sky0;             // x=skyIntensity, y=skyEnabled, z=exposure, w=fireflyClampValue
    vec4 sky1;             // x=fireflyClampEnabled, yzw unused
    uint32_t numLights;
    uint32_t maxBounces;
    uint32_t imageWidth;
    uint32_t imageHeight;
};

// Per-tile/per-batch push constants.
struct GPUPushConstants {
    uint32_t tileX, tileY;     // tile origin in image pixels
    uint32_t tileW, tileH;     // tile size in pixels
    uint32_t sampleBase;       // first sample index of this batch
    uint32_t sampleCount;      // samples to add this batch
    uint32_t frameSeed;        // extra entropy
    uint32_t pad;
};

struct Camera {
    vec3  position{ 0, 1, 4 };
    vec3  target{ 0, 1, 0 };
    vec3  up{ 0, 1, 0 };
    float fovYDegrees = 40.0f;

    // Orbit parameters (used by the interactive preview).
    float yaw = 0.0f, pitch = 0.0f, distance = 4.0f;
    vec3  pivot{ 0, 1, 0 };

    void updateFromOrbit() {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        float cy = std::cos(yaw), sy = std::sin(yaw);
        vec3 dir{ cp * sy, sp, cp * cy };
        position = pivot + dir * distance;
        target = pivot;
    }
};

class Scene {
public:
    std::vector<GPUVertex>   vertices;
    std::vector<GPUTriangle> triangles;
    std::vector<GPUMaterial> materials;
    std::vector<GPULight>    lights;
    AABB    bounds;
    Camera  camera;
    std::string name = "Demo Scene";

    void clear() {
        vertices.clear();
        triangles.clear();
        materials.clear();
        lights.clear();
        bounds = AABB{};
    }

    // Recompute bounds, the emissive-triangle light list, and frame the camera.
    void finalize();
    void frameCamera();

    size_t triangleCount() const { return triangles.size(); }
};

// Build the procedural Cornell-style demo scene described in the spec.
Scene buildDemoScene();
