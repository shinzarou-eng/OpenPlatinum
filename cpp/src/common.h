// common.h — types de base : Vec2/3, M4 (3x4), Bytes, decode LE, Image RGBA.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

using Bytes = std::vector<uint8_t>;
struct BytesView { const uint8_t* p = nullptr; size_t n = 0; };

// ---- lectures little-endian bornées -----------------------------------------
inline uint8_t  u8 (const Bytes& d, size_t o) { return o < d.size() ? d[o] : 0; }
inline uint16_t u16(const Bytes& d, size_t o) {
    return o + 1 < d.size() ? (uint16_t)(d[o] | (d[o+1] << 8)) : 0;
}
inline uint32_t u32(const Bytes& d, size_t o) {
    return o + 3 < d.size() ? (uint32_t)(d[o] | (d[o+1] << 8) | (d[o+2] << 16)
        | ((uint32_t)d[o+3] << 24)) : 0;
}
inline int16_t  s16(const Bytes& d, size_t o) { return (int16_t)u16(d, o); }
inline int32_t  s32(const Bytes& d, size_t o) { return (int32_t)u32(d, o); }
inline std::string ascii(const Bytes& d, size_t o, size_t l) {
    if (o >= d.size()) return "";
    l = std::min(l, d.size() - o);
    std::string s;
    for (size_t i = 0; i < l; i++) {
        char c = (char)d[o + i];
        if (c == 0) break;
        s += c;
    }
    return s;
}
inline Bytes slice(const Bytes& d, size_t o, size_t l) {
    if (o >= d.size()) return {};
    l = std::min(l, d.size() - o);
    return Bytes(d.begin() + o, d.begin() + o + l);
}

inline float fx32(int32_t v) { return (float)v / 4096.0f; }
inline float fx16(int16_t v) { return (float)v / 4096.0f; }

// ---- Vec ---------------------------------------------------------------------
struct Vec2 { float x = 0, y = 0; };
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
    Vec3& operator+=(const Vec3& o) { x+=o.x; y+=o.y; z+=o.z; return *this; }
    float len() const { return sqrtf(x*x + y*y + z*z); }
    Vec3 normalized() const { float l = len(); return l > 0 ? *this * (1.0f/l) : Vec3(0,0,0); }
    Vec3 cross(const Vec3& o) const {
        return {y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x};
    }
    float dot(const Vec3& o) const { return x*o.x + y*o.y + z*o.z; }
};

// ---- M4 : matrice 3x4 (colonnes x,y,z + origine w) ----------------------------
struct M4 {
    Vec3 x{1,0,0}, y{0,1,0}, z{0,0,1}, w{0,0,0};
    static M4 ident() { return M4(); }
    static M4 scale(float s) {
        M4 m; m.x = {s,0,0}; m.y = {0,s,0}; m.z = {0,0,s}; return m;
    }
    static M4 trs(const Vec3& t, const Vec3 r[3], const Vec3& s) {
        M4 m; m.x = r[0]*s.x; m.y = r[1]*s.y; m.z = r[2]*s.z; m.w = t; return m;
    }
    M4 mul(const M4& o) const {
        M4 m;
        m.x = x*o.x.x + y*o.x.y + z*o.x.z;
        m.y = x*o.y.x + y*o.y.y + z*o.y.z;
        m.z = x*o.z.x + y*o.z.y + z*o.z.z;
        m.w = x*o.w.x + y*o.w.y + z*o.w.z + w;
        return m;
    }
    M4 mul_s(float s) const {          // scale TOUT (poids skinning)
        M4 m; m.x = x*s; m.y = y*s; m.z = z*s; m.w = w*s; return m;
    }
    M4 scale_basis(float s) const {    // cur *= scale : pas la translation
        M4 m; m.x = x*s; m.y = y*s; m.z = z*s; return m;
    }
    M4 add(const M4& o) const {
        M4 m; m.x = x+o.x; m.y = y+o.y; m.z = z+o.z; m.w = w+o.w; return m;
    }
    Vec3 xform(const Vec3& v) const { return x*v.x + y*v.y + z*v.z + w; }
    Vec3 xform_n(const Vec3& v) const { return (x*v.x + y*v.y + z*v.z).normalized(); }
    M4 inverse() const {               // inverse affine (3x3 + translation)
        Vec3 r0 = y.cross(z), r1 = z.cross(x), r2 = x.cross(y);
        float det = x.dot(r0);
        if (fabsf(det) < 1e-12f) return M4();
        float id = 1.f / det;
        r0 = r0 * id; r1 = r1 * id; r2 = r2 * id;   // lignes de l'inverse
        M4 m;
        m.x = {r0.x, r1.x, r2.x}; m.y = {r0.y, r1.y, r2.y}; m.z = {r0.z, r1.z, r2.z};
        m.w = Vec3{-r0.dot(w), -r1.dot(w), -r2.dot(w)};
        return m;
    }
};

// ---- Image RGBA --------------------------------------------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA8
    void create(int w_, int h_) { w = w_; h = h_; px.assign(w_*h_*4, 0); }
    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        size_t i = ((size_t)y * w + x) * 4;
        px[i] = r; px[i+1] = g; px[i+2] = b; px[i+3] = a;
    }
    void set_i(size_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        size_t o = i * 4; px[o] = r; px[o+1] = g; px[o+2] = b; px[o+3] = a;
    }
};

struct RGBAf { float r=1, g=1, b=1, a=1; };
// 0-255 : pour écrire dans des pixels u8 de textures
inline RGBAf rgb555(uint32_t v) {
    return { (v & 31) * 255.f / 31.f, ((v >> 5) & 31) * 255.f / 31.f,
             ((v >> 10) & 31) * 255.f / 31.f, 255.0f };
}
// 0-1 : pour couleurs de sommets / teintes passées au shader
inline RGBAf rgb555f(uint32_t v) {
    return { (v & 31) / 31.f, ((v >> 5) & 31) / 31.f,
             ((v >> 10) & 31) / 31.f, 1.0f };
}
