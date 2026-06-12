// mathlib.h - compact vector/matrix math (no external dependency).
#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

struct vec2 { float x = 0, y = 0; };

struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3() = default;
    vec3(float s) : x(s), y(s), z(s) {}
    vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float& operator[](int i) { return (&x)[i]; }
    float  operator[](int i) const { return (&x)[i]; }
};

inline vec3 operator+(vec3 a, vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline vec3 operator-(vec3 a, vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline vec3 operator-(vec3 a) { return { -a.x, -a.y, -a.z }; }
inline vec3 operator*(vec3 a, vec3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline vec3 operator*(vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline vec3 operator*(float s, vec3 a) { return { a.x * s, a.y * s, a.z * s }; }
inline vec3 operator/(vec3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
inline vec3& operator+=(vec3& a, vec3 b) { a = a + b; return a; }
inline vec3& operator-=(vec3& a, vec3 b) { a = a - b; return a; }

inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3 cross(vec3 a, vec3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float length(vec3 a) { return std::sqrt(dot(a, a)); }
inline vec3 normalize(vec3 a) {
    float l = length(a);
    return l > 0.0f ? a / l : a;
}
inline vec3 vmin(vec3 a, vec3 b) { return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) }; }
inline vec3 vmax(vec3 a, vec3 b) { return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) }; }

struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec4() = default;
    vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    vec4(vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

// Column-major 4x4 matrix, m[col][row].
struct mat4 {
    float m[4][4] = {};
    static mat4 identity() {
        mat4 r;
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }
};

inline mat4 operator*(const mat4& a, const mat4& b) {
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k][row] * b.m[c][k];
            r.m[c][row] = s;
        }
    return r;
}

inline vec4 operator*(const mat4& a, const vec4& v) {
    vec4 r;
    r.x = a.m[0][0]*v.x + a.m[1][0]*v.y + a.m[2][0]*v.z + a.m[3][0]*v.w;
    r.y = a.m[0][1]*v.x + a.m[1][1]*v.y + a.m[2][1]*v.z + a.m[3][1]*v.w;
    r.z = a.m[0][2]*v.x + a.m[1][2]*v.y + a.m[2][2]*v.z + a.m[3][2]*v.w;
    r.w = a.m[0][3]*v.x + a.m[1][3]*v.y + a.m[2][3]*v.z + a.m[3][3]*v.w;
    return r;
}

inline mat4 translate(vec3 t) {
    mat4 r = mat4::identity();
    r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
    return r;
}

inline mat4 scale(vec3 s) {
    mat4 r = mat4::identity();
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
    return r;
}

inline mat4 rotateY(float a) {
    mat4 r = mat4::identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[2][0] = s;
    r.m[0][2] = -s; r.m[2][2] = c;
    return r;
}

inline mat4 rotateX(float a) {
    mat4 r = mat4::identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c; r.m[2][1] = -s;
    r.m[1][2] = s; r.m[2][2] = c;
    return r;
}

struct AABB {
    vec3 lo{ 1e30f, 1e30f, 1e30f };
    vec3 hi{ -1e30f, -1e30f, -1e30f };
    void expand(vec3 p) { lo = vmin(lo, p); hi = vmax(hi, p); }
    void expand(const AABB& b) { lo = vmin(lo, b.lo); hi = vmax(hi, b.hi); }
    vec3 center() const { return (lo + hi) * 0.5f; }
    vec3 extent() const { return hi - lo; }
    float surfaceArea() const {
        vec3 e = hi - lo;
        if (e.x < 0 || e.y < 0 || e.z < 0) return 0.0f;
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }
};

inline float radians(float deg) { return deg * 3.14159265358979323846f / 180.0f; }
inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
