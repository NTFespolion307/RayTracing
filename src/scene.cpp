#include "scene.h"
#include <unordered_map>
#include <array>
#include <cstdint>

// ---------------------------------------------------------------------------
// Geometry builders
// ---------------------------------------------------------------------------
namespace {

void addTriangle(Scene& s, vec3 a, vec3 b, vec3 c, vec3 na, vec3 nb, vec3 nc,
                 uint32_t mat) {
    uint32_t base = (uint32_t)s.vertices.size();
    s.vertices.push_back({ vec4(a, 0), vec4(na, 0) });
    s.vertices.push_back({ vec4(b, 0), vec4(nb, 0) });
    s.vertices.push_back({ vec4(c, 0), vec4(nc, 0) });
    s.triangles.push_back({ base, base + 1, base + 2, mat });
}

void addFlatTriangle(Scene& s, vec3 a, vec3 b, vec3 c, uint32_t mat) {
    vec3 n = normalize(cross(b - a, c - a));
    addTriangle(s, a, b, c, n, n, n, mat);
}

// Quad with corners in CCW order (as seen from the front face).
void addQuad(Scene& s, vec3 p0, vec3 p1, vec3 p2, vec3 p3, uint32_t mat) {
    addFlatTriangle(s, p0, p1, p2, mat);
    addFlatTriangle(s, p0, p2, p3, mat);
}

// Axis-aligned box (optionally rotated about Y around its center).
void addBox(Scene& s, vec3 center, vec3 halfSize, float rotY, uint32_t mat) {
    mat4 m = translate(center) * rotateY(rotY);
    auto P = [&](float x, float y, float z) -> vec3 {
        vec4 v = m * vec4(x, y, z, 1.0f);
        return { v.x, v.y, v.z };
    };
    vec3 h = halfSize;
    vec3 c000 = P(-h.x, -h.y, -h.z), c100 = P(h.x, -h.y, -h.z);
    vec3 c110 = P(h.x, h.y, -h.z),   c010 = P(-h.x, h.y, -h.z);
    vec3 c001 = P(-h.x, -h.y, h.z),  c101 = P(h.x, -h.y, h.z);
    vec3 c111 = P(h.x, h.y, h.z),    c011 = P(-h.x, h.y, h.z);
    addQuad(s, c001, c101, c111, c011, mat); // +z
    addQuad(s, c100, c000, c010, c110, mat); // -z
    addQuad(s, c101, c100, c110, c111, mat); // +x
    addQuad(s, c000, c001, c011, c010, mat); // -x
    addQuad(s, c011, c111, c110, c010, mat); // +y
    addQuad(s, c000, c100, c101, c001, mat); // -y
}

// Icosphere: subdivided icosahedron projected onto a sphere (smooth normals).
void addIcosphere(Scene& s, vec3 center, float radius, int subdiv, uint32_t mat) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<vec3> verts = {
        normalize({-1,  t,  0}), normalize({ 1,  t,  0}),
        normalize({-1, -t,  0}), normalize({ 1, -t,  0}),
        normalize({ 0, -1,  t}), normalize({ 0,  1,  t}),
        normalize({ 0, -1, -t}), normalize({ 0,  1, -t}),
        normalize({ t,  0, -1}), normalize({ t,  0,  1}),
        normalize({-t,  0, -1}), normalize({-t,  0,  1}),
    };
    std::vector<std::array<int, 3>> faces = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
    };

    std::unordered_map<uint64_t, int> midpointCache;
    auto midpoint = [&](int a, int b) -> int {
        uint64_t key = (uint64_t)std::min(a, b) << 32 | (uint32_t)std::max(a, b);
        auto it = midpointCache.find(key);
        if (it != midpointCache.end()) return it->second;
        vec3 m = normalize((verts[a] + verts[b]) * 0.5f);
        int idx = (int)verts.size();
        verts.push_back(m);
        midpointCache[key] = idx;
        return idx;
    };

    for (int i = 0; i < subdiv; ++i) {
        std::vector<std::array<int, 3>> next;
        next.reserve(faces.size() * 4);
        for (auto& f : faces) {
            int a = midpoint(f[0], f[1]);
            int b = midpoint(f[1], f[2]);
            int c = midpoint(f[2], f[0]);
            next.push_back({ f[0], a, c });
            next.push_back({ f[1], b, a });
            next.push_back({ f[2], c, b });
            next.push_back({ a, b, c });
        }
        faces.swap(next);
    }

    for (auto& f : faces) {
        vec3 n0 = verts[f[0]], n1 = verts[f[1]], n2 = verts[f[2]];
        addTriangle(s, center + n0 * radius, center + n1 * radius, center + n2 * radius,
                    n0, n1, n2, mat);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Scene finalization
// ---------------------------------------------------------------------------
void Scene::finalize() {
    bounds = AABB{};
    for (auto& v : vertices)
        bounds.expand({ v.pos.x, v.pos.y, v.pos.z });

    // Build the emissive-triangle light list for next-event estimation.
    lights.clear();
    for (auto& tri : triangles) {
        if (tri.material >= materials.size()) continue;
        const GPUMaterial& m = materials[tri.material];
        if ((uint32_t)m.params.x != MAT_EMISSIVE) continue;
        vec3 p0{ vertices[tri.v0].pos.x, vertices[tri.v0].pos.y, vertices[tri.v0].pos.z };
        vec3 p1{ vertices[tri.v1].pos.x, vertices[tri.v1].pos.y, vertices[tri.v1].pos.z };
        vec3 p2{ vertices[tri.v2].pos.x, vertices[tri.v2].pos.y, vertices[tri.v2].pos.z };
        float area = 0.5f * length(cross(p1 - p0, p2 - p0));
        GPULight L;
        L.p0 = vec4(p0, 0);
        L.p1 = vec4(p1, 0);
        L.p2 = vec4(p2, 0);
        L.emission = vec4(m.emission.x, m.emission.y, m.emission.z, area);
        lights.push_back(L);
    }
}

void Scene::frameCamera() {
    vec3 c = bounds.center();
    vec3 e = bounds.extent();
    float radius = 0.5f * length(e);
    if (radius <= 0) radius = 1.0f;
    camera.pivot = c;
    camera.distance = radius / std::sin(radians(camera.fovYDegrees * 0.5f)) * 1.1f;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    camera.updateFromOrbit();
}

// ---------------------------------------------------------------------------
// Demo scene: Cornell-style room with metal + glass spheres and a rotated box.
// ---------------------------------------------------------------------------
Scene buildDemoScene() {
    Scene s;
    s.name = "Demo Scene";

    auto addMaterial = [&](vec3 albedo, vec3 emission, MaterialType type,
                           float roughness, float ior) -> uint32_t {
        GPUMaterial m;
        m.albedo = vec4(albedo, 0);
        m.emission = vec4(emission, 0);
        m.params = vec4((float)type, roughness, ior, 0);
        s.materials.push_back(m);
        return (uint32_t)s.materials.size() - 1;
    };

    uint32_t white = addMaterial({ 0.73f, 0.73f, 0.73f }, { 0, 0, 0 }, MAT_LAMBERT, 1.0f, 1.0f);
    uint32_t red   = addMaterial({ 0.65f, 0.05f, 0.05f }, { 0, 0, 0 }, MAT_LAMBERT, 1.0f, 1.0f);
    uint32_t green = addMaterial({ 0.05f, 0.45f, 0.10f }, { 0, 0, 0 }, MAT_LAMBERT, 1.0f, 1.0f);
    uint32_t light = addMaterial({ 0, 0, 0 }, { 18.0f, 16.0f, 13.0f }, MAT_EMISSIVE, 1.0f, 1.0f);
    uint32_t metal = addMaterial({ 0.95f, 0.95f, 0.97f }, { 0, 0, 0 }, MAT_METAL, 0.04f, 1.0f);
    uint32_t glass = addMaterial({ 1.0f, 1.0f, 1.0f }, { 0, 0, 0 }, MAT_GLASS, 0.0f, 1.5f);

    // Room: x,z in [-1,1], y in [0,2], opening faces +z (toward camera).
    const float lo = -1.0f, hi = 1.0f, top = 2.0f, bot = 0.0f;
    // Floor (+y normal)
    addQuad(s, { lo, bot, lo }, { hi, bot, lo }, { hi, bot, hi }, { lo, bot, hi }, white);
    // Ceiling (-y normal)
    addQuad(s, { lo, top, hi }, { hi, top, hi }, { hi, top, lo }, { lo, top, lo }, white);
    // Back wall (z = lo, +z normal)
    addQuad(s, { lo, bot, lo }, { lo, top, lo }, { hi, top, lo }, { hi, bot, lo }, white);
    // Left wall (x = lo, +x normal) red
    addQuad(s, { lo, bot, hi }, { lo, top, hi }, { lo, top, lo }, { lo, bot, lo }, red);
    // Right wall (x = hi, -x normal) green
    addQuad(s, { hi, bot, lo }, { hi, top, lo }, { hi, top, hi }, { hi, bot, hi }, green);

    // Emissive ceiling quad slightly below the ceiling.
    const float lx = 0.35f, lz = 0.35f, ly = top - 0.001f;
    addQuad(s, { -lx, ly, lz }, { lx, ly, lz }, { lx, ly, -lz }, { -lx, ly, -lz }, light);

    // Objects.
    addIcosphere(s, { -0.42f, 0.45f, -0.30f }, 0.45f, 4, metal);
    addIcosphere(s, {  0.45f, 0.40f,  0.35f }, 0.40f, 4, glass);
    addBox(s, { 0.0f, 0.30f, -0.55f }, { 0.28f, 0.30f, 0.28f }, radians(20.0f), white);

    s.camera.fovYDegrees = 40.0f;
    s.finalize();
    s.camera.pivot = { 0.0f, 1.0f, 0.0f };
    s.camera.distance = 4.2f;
    s.camera.yaw = 0.0f;
    s.camera.pitch = 0.0f;
    s.camera.updateFromOrbit();
    return s;
}
