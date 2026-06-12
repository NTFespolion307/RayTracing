#define TINYOBJLOADER_IMPLEMENTATION
#include "obj_loader.h"
#include "tiny_obj_loader.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstdint>

namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Splits a wide path into directory (with trailing slash) and filename.
std::wstring dirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    return path.substr(0, p + 1);
}

// MaterialReader that opens .mtl files via wide paths (non-ASCII safe).
class WideMaterialReader : public tinyobj::MaterialReader {
public:
    explicit WideMaterialReader(std::wstring baseDir) : baseDir_(std::move(baseDir)) {}
    bool operator()(const std::string& matId,
                    std::vector<tinyobj::material_t>* materials,
                    std::map<std::string, int>* matMap,
                    std::string* err) override {
        std::wstring full = baseDir_ + utf8ToWide(matId);
        std::ifstream stream(full.c_str(), std::ios::binary);
        if (!stream) {
            if (err) *err += "Could not open material file: " + matId + "\n";
            return true; // not fatal; OBJ still renders with defaults
        }
        std::stringstream ss;
        ss << stream.rdbuf();
        std::string content = ss.str();
        std::stringstream parse(content);
        std::string warning;
        tinyobj::LoadMtl(matMap, materials, &parse, &warning);
        return true;
    }
private:
    std::wstring baseDir_;
};

GPUMaterial mapMaterial(const tinyobj::material_t& m) {
    GPUMaterial out;
    vec3 kd{ m.diffuse[0], m.diffuse[1], m.diffuse[2] };
    vec3 ks{ m.specular[0], m.specular[1], m.specular[2] };
    vec3 ke{ m.emission[0], m.emission[1], m.emission[2] };

    float emLum = ke.x + ke.y + ke.z;
    float ksMax = std::max(ks.x, std::max(ks.y, ks.z));

    if (emLum > 0.0f) {
        out.albedo = vec4(0, 0, 0, 0);
        out.emission = vec4(ke, 0);
        out.params = vec4((float)MAT_EMISSIVE, 1.0f, 1.0f, 0);
    } else if ((m.ior > 1.05f && m.dissolve < 1.0f) || m.illum == 4 || m.illum == 6 || m.illum == 7) {
        out.albedo = vec4(kd.x > 0 ? kd : vec3(1.0f), 0);
        out.emission = vec4(0, 0, 0, 0);
        out.params = vec4((float)MAT_GLASS, 0.0f, m.ior > 1.0f ? m.ior : 1.5f, 0);
    } else if (m.illum == 3 || m.illum == 5 || ksMax > 0.25f) {
        // GGX metal: roughness derived from the Phong exponent Ns.
        float alpha = std::sqrt(2.0f / (m.shininess + 2.0f));
        alpha = clampf(alpha, 0.02f, 1.0f);
        out.albedo = vec4(ksMax > 0 ? ks : kd, 0);
        out.emission = vec4(0, 0, 0, 0);
        out.params = vec4((float)MAT_METAL, alpha, 1.0f, 0);
    } else {
        out.albedo = vec4(kd, 0);
        out.emission = vec4(0, 0, 0, 0);
        out.params = vec4((float)MAT_LAMBERT, 1.0f, 1.0f, 0);
    }
    return out;
}

} // namespace

ObjLoadResult loadObj(const std::wstring& widePath) {
    ObjLoadResult result;

    std::ifstream objStream(widePath.c_str(), std::ios::binary);
    if (!objStream) {
        result.message = "Could not open the OBJ file.";
        return result;
    }

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> mats;
    std::string err;

    WideMaterialReader matReader(dirOf(widePath));
    bool ok = tinyobj::LoadObj(&attrib, &shapes, &mats, &err,
                               &objStream, &matReader, /*triangulate*/ true);
    if (!ok) {
        result.message = "Failed to parse OBJ: " + err;
        return result;
    }

    Scene& scene = result.scene;
    {
        std::wstring fname = widePath.substr(dirOf(widePath).size());
        scene.name = wideToUtf8(fname);
    }

    // Map materials (+ a trailing default for faces with material_id == -1).
    for (auto& m : mats)
        scene.materials.push_back(mapMaterial(m));
    uint32_t defaultMat = (uint32_t)scene.materials.size();
    {
        GPUMaterial def;
        def.albedo = vec4(0.7f, 0.7f, 0.7f, 0);
        def.emission = vec4(0, 0, 0, 0);
        def.params = vec4((float)MAT_LAMBERT, 1.0f, 1.0f, 0);
        scene.materials.push_back(def);
    }

    const size_t numPos = attrib.vertices.size() / 3;
    const bool hasNormals = !attrib.normals.empty();

    // Smoothed per-position normals as a fallback when the OBJ has none.
    std::vector<vec3> smoothN(numPos, vec3(0, 0, 0));
    if (!hasNormals) {
        for (auto& shape : shapes) {
            const auto& idx = shape.mesh.indices;
            for (size_t f = 0; f + 2 < idx.size(); f += 3) {
                int i0 = idx[f + 0].vertex_index;
                int i1 = idx[f + 1].vertex_index;
                int i2 = idx[f + 2].vertex_index;
                if (i0 < 0 || i1 < 0 || i2 < 0) continue;
                vec3 p0{ attrib.vertices[3*i0], attrib.vertices[3*i0+1], attrib.vertices[3*i0+2] };
                vec3 p1{ attrib.vertices[3*i1], attrib.vertices[3*i1+1], attrib.vertices[3*i1+2] };
                vec3 p2{ attrib.vertices[3*i2], attrib.vertices[3*i2+1], attrib.vertices[3*i2+2] };
                vec3 n = cross(p1 - p0, p2 - p0);
                smoothN[i0] += n; smoothN[i1] += n; smoothN[i2] += n;
            }
        }
        for (auto& n : smoothN) n = normalize(n);
    }

    // Emit deduplicated vertices and triangles.
    std::unordered_map<uint64_t, uint32_t> cache;
    cache.reserve(numPos * 2);
    auto emitVertex = [&](const tinyobj::index_t& id) -> uint32_t {
        int vi = id.vertex_index;
        int ni = id.normal_index;
        uint64_t key = ((uint64_t)(uint32_t)(vi + 1) << 32) |
                       (uint32_t)(ni >= 0 ? ni : 0x7FFFFFFF);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        GPUVertex v;
        v.pos = vec4(attrib.vertices[3*vi], attrib.vertices[3*vi+1], attrib.vertices[3*vi+2], 0);
        vec3 n;
        if (ni >= 0) n = vec3(attrib.normals[3*ni], attrib.normals[3*ni+1], attrib.normals[3*ni+2]);
        else         n = (vi < (int)smoothN.size()) ? smoothN[vi] : vec3(0, 1, 0);
        v.normal = vec4(normalize(n), 0);
        uint32_t index = (uint32_t)scene.vertices.size();
        scene.vertices.push_back(v);
        cache.emplace(key, index);
        return index;
    };

    for (auto& shape : shapes) {
        const auto& idx = shape.mesh.indices;
        const auto& matIds = shape.mesh.material_ids;
        for (size_t f = 0; f + 2 < idx.size(); f += 3) {
            uint32_t a = emitVertex(idx[f + 0]);
            uint32_t b = emitVertex(idx[f + 1]);
            uint32_t c = emitVertex(idx[f + 2]);
            int faceMat = (f / 3) < matIds.size() ? matIds[f / 3] : -1;
            uint32_t mat = (faceMat >= 0 && faceMat < (int)mats.size())
                               ? (uint32_t)faceMat : defaultMat;
            scene.triangles.push_back({ a, b, c, mat });
        }
    }

    if (scene.triangles.empty()) {
        result.message = "The OBJ contained no triangles.";
        return result;
    }

    scene.camera.fovYDegrees = 40.0f;
    scene.finalize();
    scene.frameCamera();

    result.ok = true;
    result.message = err;
    return result;
}
