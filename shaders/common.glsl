// common.glsl - RNG, BSDFs, intersection, sky, and NEE/MIS shared by both
// SPIR-V variants of the path tracer. All descriptor bindings and the push
// constant block live here so the helper functions can reach the scene data.
#ifndef COMMON_GLSL
#define COMMON_GLSL

const float PI      = 3.14159265358979323846;
const float INV_PI  = 0.31830988618379067154;
const float EPS     = 1e-4;
const float INF     = 1e30;

// Material types (must match MaterialType in scene.h).
const uint MAT_LAMBERT  = 0u;
const uint MAT_METAL    = 1u;
const uint MAT_GLASS    = 2u;
const uint MAT_EMISSIVE = 3u;

// ---- POD layouts (must match scene.h) ----
struct Vertex   { vec4 pos; vec4 normal; };
struct Triangle { uint v0; uint v1; uint v2; uint material; };
struct Material  { vec4 albedo; vec4 emission; vec4 params; }; // params: type,rough,ior
struct BVHNode   { vec4 lo; vec4 hi; };                        // lo.w=leftFirst hi.w=count
struct Light     { vec4 p0; vec4 p1; vec4 p2; vec4 emission; }; // emission.w=area

layout(std430, binding = 0) buffer Accum    { vec4 data[]; }      accum;
layout(std140, binding = 1) uniform Params {
    vec4 camOrigin;
    vec4 camU;
    vec4 camV;
    vec4 camW;
    vec4 sky0;   // x=skyIntensity y=skyEnabled z=exposure w=fireflyClampValue
    vec4 sky1;   // x=fireflyClampEnabled
    uint numLights;
    uint maxBounces;
    uint imageWidth;
    uint imageHeight;
} params;
layout(std430, binding = 2) readonly buffer Vertices  { Vertex   v[]; } verts;
layout(std430, binding = 3) readonly buffer Triangles { Triangle t[]; } tris;
layout(std430, binding = 4) readonly buffer Materials { Material m[]; } mats;
layout(std430, binding = 5) readonly buffer Lights    { Light    l[]; } lights;

#if USE_RAY_QUERY
layout(binding = 6) uniform accelerationStructureEXT topLevelAS;
#else
layout(std430, binding = 6) readonly buffer BVH { BVHNode n[]; } bvh;
#endif

layout(push_constant) uniform PushConstants {
    uint tileX, tileY, tileW, tileH;
    uint sampleBase, sampleCount, frameSeed, pad;
} pc;

// ---------------------------------------------------------------------------
// RNG (PCG)
// ---------------------------------------------------------------------------
uint g_rng;

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
void rngInit(uint a, uint b, uint c, uint d) {
    g_rng = pcgHash(a ^ pcgHash(b ^ pcgHash(c ^ pcgHash(d))));
}
float rnd() {
    g_rng = g_rng * 747796405u + 2891336453u;
    uint w = ((g_rng >> ((g_rng >> 28u) + 4u)) ^ g_rng) * 277803737u;
    w = (w >> 22u) ^ w;
    return float(w) * (1.0 / 4294967296.0);
}

// ---------------------------------------------------------------------------
// Sampling helpers
// ---------------------------------------------------------------------------
void onb(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float bb = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = vec3(bb, s + n.y * n.y * a, -n.y);
}
vec3 toWorld(vec3 local, vec3 n) {
    vec3 t, b; onb(n, t, b);
    return local.x * t + local.y * b + local.z * n;
}
vec3 cosineSampleHemisphere(vec3 n, out float pdf) {
    float r1 = rnd(), r2 = rnd();
    float r = sqrt(r1);
    float phi = 2.0 * PI * r2;
    vec3 local = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - r1)));
    pdf = local.z * INV_PI;
    return normalize(toWorld(local, n));
}
float powerHeuristic(float a, float b) {
    float a2 = a * a;
    return a2 / (a2 + b * b + 1e-12);
}

// ---------------------------------------------------------------------------
// GGX microfacet (metal)
// ---------------------------------------------------------------------------
float ggxD(float NoH, float a) {
    float a2 = a * a;
    float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * d * d + 1e-12);
}
float ggxG1(float NoX, float a) {
    float a2 = a * a;
    return 2.0 * NoX / (NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX) + 1e-12);
}
vec3 fresnelSchlick(vec3 f0, float cosT) {
    float m = clamp(1.0 - cosT, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}
vec3 sampleGGXHalf(vec3 n, float a) {
    float r1 = rnd(), r2 = rnd();
    float phi = 2.0 * PI * r1;
    float cosT = sqrt((1.0 - r2) / (1.0 + (a * a - 1.0) * r2));
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    vec3 local = vec3(sinT * cos(phi), sinT * sin(phi), cosT);
    return normalize(toWorld(local, n));
}

// Diffuse BSDF eval/pdf (for NEE/MIS).
vec3 lambertEval(vec3 albedo) { return albedo * INV_PI; }
float lambertPdf(vec3 n, vec3 wi) { return max(dot(n, wi), 0.0) * INV_PI; }

// Metal GGX eval/pdf for a given wo, wi (both pointing away from surface).
vec3 metalEval(vec3 n, vec3 wo, vec3 wi, vec3 f0, float a) {
    float NoWo = dot(n, wo), NoWi = dot(n, wi);
    if (NoWo <= 0.0 || NoWi <= 0.0) return vec3(0.0);
    vec3 h = normalize(wo + wi);
    float NoH = max(dot(n, h), 0.0);
    float D = ggxD(NoH, a);
    float G = ggxG1(NoWo, a) * ggxG1(NoWi, a);
    vec3  F = fresnelSchlick(f0, max(dot(wo, h), 0.0));
    return (D * G * F) / (4.0 * NoWo * NoWi + 1e-12);
}
float metalPdf(vec3 n, vec3 wo, vec3 wi, float a) {
    float NoWo = dot(n, wo), NoWi = dot(n, wi);
    if (NoWo <= 0.0 || NoWi <= 0.0) return 0.0;
    vec3 h = normalize(wo + wi);
    float NoH = max(dot(n, h), 0.0);
    float WoH = max(dot(wo, h), 0.0);
    return ggxD(NoH, a) * NoH / (4.0 * WoH + 1e-12);
}

// ---------------------------------------------------------------------------
// Sky
// ---------------------------------------------------------------------------
vec3 skyColor(vec3 dir) {
    if (params.sky0.y < 0.5) return vec3(0.0);
    float t = clamp(0.5 * (dir.y + 1.0), 0.0, 1.0);
    vec3 horizon = vec3(0.85, 0.90, 1.0);
    vec3 zenith  = vec3(0.20, 0.40, 0.85);
    vec3 ground  = vec3(0.35, 0.32, 0.30);
    vec3 c = dir.y >= 0.0 ? mix(horizon, zenith, t) : mix(horizon, ground, -dir.y);
    return c * params.sky0.x;
}

// ---------------------------------------------------------------------------
// Intersection
// ---------------------------------------------------------------------------
struct Hit {
    float t;
    uint  prim;
    float u, v;
    bool  valid;
};

bool intersectTriangle(vec3 ro, vec3 rd, vec3 p0, vec3 p1, vec3 p2,
                       out float t, out float u, out float v) {
    vec3 e1 = p1 - p0;
    vec3 e2 = p2 - p0;
    vec3 pv = cross(rd, e2);
    float det = dot(e1, pv);
    if (abs(det) < 1e-9) return false;
    float inv = 1.0 / det;
    vec3 tv = ro - p0;
    u = dot(tv, pv) * inv;
    if (u < 0.0 || u > 1.0) return false;
    vec3 qv = cross(tv, e1);
    v = dot(rd, qv) * inv;
    if (v < 0.0 || u + v > 1.0) return false;
    t = dot(e2, qv) * inv;
    return t > EPS;
}

#if USE_RAY_QUERY
Hit traceClosest(vec3 ro, vec3 rd, float tmax) {
    Hit h; h.valid = false; h.t = tmax;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, ro, EPS, rd, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) ==
        gl_RayQueryCommittedIntersectionTriangleEXT) {
        h.valid = true;
        h.t = rayQueryGetIntersectionTEXT(rq, true);
        h.prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
        vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
        h.u = bc.x; h.v = bc.y;
    }
    return h;
}
bool traceAny(vec3 ro, vec3 rd, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0xFF, ro, EPS, rd, tmax);
    rayQueryProceedEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT;
}
#else
bool slabTest(BVHNode node, vec3 ro, vec3 invD, float tmax) {
    vec3 t0 = (node.lo.xyz - ro) * invD;
    vec3 t1 = (node.hi.xyz - ro) * invD;
    vec3 tmin = min(t0, t1);
    vec3 tmaxv = max(t0, t1);
    float lo = max(max(tmin.x, tmin.y), tmin.z);
    float hi = min(min(tmaxv.x, tmaxv.y), tmaxv.z);
    return hi >= max(lo, 0.0) && lo <= tmax;
}
Hit traceClosest(vec3 ro, vec3 rd, float tmax) {
    Hit h; h.valid = false; h.t = tmax;
    vec3 invD = 1.0 / rd;
    uint stack[64];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        BVHNode node = bvh.n[stack[--sp]];
        if (!slabTest(node, ro, invD, h.t)) continue;
        uint count = floatBitsToUint(node.hi.w);
        if (count > 0u) {
            uint first = floatBitsToUint(node.lo.w);
            for (uint i = 0u; i < count; ++i) {
                Triangle tri = tris.t[first + i];
                vec3 p0 = verts.v[tri.v0].pos.xyz;
                vec3 p1 = verts.v[tri.v1].pos.xyz;
                vec3 p2 = verts.v[tri.v2].pos.xyz;
                float t, u, v;
                if (intersectTriangle(ro, rd, p0, p1, p2, t, u, v) && t < h.t) {
                    h.valid = true; h.t = t; h.prim = first + i; h.u = u; h.v = v;
                }
            }
        } else if (sp < 62) {
            uint left = floatBitsToUint(node.lo.w);
            stack[sp++] = left;
            stack[sp++] = left + 1u;
        }
    }
    return h;
}
bool traceAny(vec3 ro, vec3 rd, float tmax) {
    vec3 invD = 1.0 / rd;
    uint stack[64];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        BVHNode node = bvh.n[stack[--sp]];
        if (!slabTest(node, ro, invD, tmax)) continue;
        uint count = floatBitsToUint(node.hi.w);
        if (count > 0u) {
            uint first = floatBitsToUint(node.lo.w);
            for (uint i = 0u; i < count; ++i) {
                Triangle tri = tris.t[first + i];
                vec3 p0 = verts.v[tri.v0].pos.xyz;
                vec3 p1 = verts.v[tri.v1].pos.xyz;
                vec3 p2 = verts.v[tri.v2].pos.xyz;
                float t, u, v;
                if (intersectTriangle(ro, rd, p0, p1, p2, t, u, v) && t < tmax)
                    return true;
            }
        } else if (sp < 62) {
            uint left = floatBitsToUint(node.lo.w);
            stack[sp++] = left;
            stack[sp++] = left + 1u;
        }
    }
    return false;
}
#endif

// Surface data at a hit.
struct Surface {
    vec3 p;
    vec3 ns;   // shading normal (geometry, smoothed), oriented against the ray
    vec3 ng;   // geometric normal
    uint matId;
};
Surface getSurface(vec3 ro, vec3 rd, Hit h) {
    Triangle tri = tris.t[h.prim];
    vec3 p0 = verts.v[tri.v0].pos.xyz;
    vec3 p1 = verts.v[tri.v1].pos.xyz;
    vec3 p2 = verts.v[tri.v2].pos.xyz;
    vec3 n0 = verts.v[tri.v0].normal.xyz;
    vec3 n1 = verts.v[tri.v1].normal.xyz;
    vec3 n2 = verts.v[tri.v2].normal.xyz;
    float w0 = 1.0 - h.u - h.v;
    Surface s;
    s.p = ro + rd * h.t;
    s.ng = normalize(cross(p1 - p0, p2 - p0));
    vec3 ns = normalize(w0 * n0 + h.u * n1 + h.v * n2);
    if (dot(ns, ns) < 1e-8) ns = s.ng;
    s.ns = ns;
    s.matId = tri.material;
    return s;
}

// ---------------------------------------------------------------------------
// Next-event estimation against emissive triangles, with MIS.
// ---------------------------------------------------------------------------
vec3 sampleLightNEE(Surface s, vec3 wo, Material mat, uint type, float rough) {
    if (params.numLights == 0u) return vec3(0.0);
    uint li = min(uint(rnd() * float(params.numLights)), params.numLights - 1u);
    Light L = lights.l[li];

    float r1 = rnd(), r2 = rnd();
    float su = sqrt(r1);
    float b0 = 1.0 - su;
    float b1 = su * (1.0 - r2);
    float b2 = su * r2;
    vec3 q = L.p0.xyz * b0 + L.p1.xyz * b1 + L.p2.xyz * b2;

    vec3 toL = q - s.p;
    float dist2 = dot(toL, toL);
    float dist = sqrt(dist2);
    vec3 wi = toL / dist;

    vec3 nFace = dot(s.ns, wo) < 0.0 ? -s.ns : s.ns;
    float cosSurf = dot(nFace, wi);
    if (cosSurf <= 0.0) return vec3(0.0);

    vec3 ln = normalize(cross(L.p1.xyz - L.p0.xyz, L.p2.xyz - L.p0.xyz));
    float cosLight = abs(dot(ln, wi));
    if (cosLight <= 1e-4) return vec3(0.0);

    float area = L.emission.w;
    float pdfArea = 1.0 / (float(params.numLights) * max(area, 1e-8));
    float pdfW = pdfArea * dist2 / cosLight;
    if (pdfW <= 0.0) return vec3(0.0);

    // BSDF term.
    vec3 f; float bsdfPdf;
    if (type == MAT_METAL) {
        f = metalEval(nFace, wo, wi, mat.albedo.xyz, rough);
        bsdfPdf = metalPdf(nFace, wo, wi, rough);
    } else {
        f = lambertEval(mat.albedo.xyz);
        bsdfPdf = lambertPdf(nFace, wi);
    }
    if (dot(f, f) <= 0.0) return vec3(0.0);

    if (traceAny(s.p + nFace * EPS, wi, dist - 2.0 * EPS)) return vec3(0.0);

    float mis = powerHeuristic(pdfW, bsdfPdf);
    return f * cosSurf * L.emission.xyz * mis / pdfW;
}

// Dielectric (glass) helpers.
float fresnelDielectric(float cosI, float eta) {
    cosI = clamp(cosI, -1.0, 1.0);
    float sinT2 = eta * eta * (1.0 - cosI * cosI);
    if (sinT2 >= 1.0) return 1.0; // total internal reflection
    float cosT = sqrt(1.0 - sinT2);
    float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
    float rp = (cosI - eta * cosT) / (cosI + eta * cosT);
    return 0.5 * (rs * rs + rp * rp);
}

#endif // COMMON_GLSL
