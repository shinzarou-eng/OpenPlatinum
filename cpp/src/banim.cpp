// banim.cpp — animations d'attaques DS : VM we.arc + particules SPL (.spa).
// Assets : battle_fx/we.bin (scripts 501+50) et battle_fx/spa.bin (485 .spa).
#include "banim.h"
#include "battle.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <filesystem>
namespace fs = std::filesystem;

// ── utils ────────────────────────────────────────────────────────────────────
#include "common.h"          // fx32/fx16
static void rgb555(int v, uint8_t c[3]) {
    c[0] = (v & 31) * 8; c[1] = ((v >> 5) & 31) * 8; c[2] = ((v >> 10) & 31) * 8;
}
static uint32_t rnd() { static uint32_t s = 0x9E3779B9; s = s * 1664525 + 1013904223; return s >> 8; }
static float frnd(float a, float b) { return a + (b - a) * (rnd() & 0xFFFF) / 65535.f; }
static std::string exe_dir() {
    const char* bp = SDL_GetBasePath();
    return bp ? bp : "";
}
static std::vector<uint8_t> load_file(const char* rel) {
    for (std::string p : {std::string(rel), exe_dir() + rel,
                          std::string("../") + rel, exe_dir() + "../" + rel}) {
        if (FILE* f = fopen(p.c_str(), "rb")) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> d(n); fread(d.data(), 1, n, f); fclose(f);
            return d;
        }
    }
    return {};
}

// ── assets ───────────────────────────────────────────────────────────────────
struct WeBlob { std::vector<uint8_t> d; int narc = 0; const uint32_t* offs = nullptr;
    const uint8_t* member(int i, int* n) const {
        if (i < 0 || i >= narc) return nullptr;
        uint32_t a = offs[i], e = (i + 1 < narc) ? offs[i + 1] : (uint32_t)d.size();
        *n = (int)(e - a); return d.data() + a;
    }
};
static WeBlob g_we;                       // scripts (0..500 = moves, 500+ = subs)
static std::vector<std::vector<uint8_t>> g_spa;   // 485 .spa bruts
static bool g_assets_tried = false;
static bool banim_load() {
    if (g_assets_tried) return !g_we.d.empty();
    g_assets_tried = true;
    auto wb = load_file("battle_fx/we.bin");
    auto sb = load_file("battle_fx/spa.bin");
    if (wb.size() >= 8 && sb.size() >= 4) {
        uint32_t na, ns; memcpy(&na, wb.data(), 4); memcpy(&ns, wb.data() + 4, 4);
        g_we.narc = na + ns;
        g_we.offs = (const uint32_t*)(wb.data() + 8);
        g_we.d = std::move(wb);
        const uint32_t* so = (const uint32_t*)(sb.data() + 4);
        uint32_t nsp; memcpy(&nsp, sb.data(), 4);
        for (uint32_t i = 0; i < nsp; i++) {
            uint32_t a = so[i], e = (i + 1 < nsp) ? so[i + 1] : (uint32_t)sb.size();
            if (a < e && e <= sb.size())
                g_spa.emplace_back(sb.begin() + a, sb.begin() + e);
            else g_spa.emplace_back();
        }
    }
    return !g_we.d.empty();
}
static const uint8_t* we_script(int mv, int* n) {
    // move id -> index membre (table waza_seq inutilisée : we.arc est indexé
    // par l'ID d'anim qui coïncide avec l'ordre des moves sur DS)
    return g_we.member(mv, n);
}

// ── .spa parsé ───────────────────────────────────────────────────────────────
struct SpaTex { int w = 0, h = 0; GLuint gl = 0; };
struct SpaRes {
    uint32_t flags = 0;
    int emitType = 0, drawType = 0;
    float basePos[3] = {0, 0, 0}, emitCount = 0, radius = 0, length = 0;
    float axis[3] = {0, 0, 4096}, velPos = 0, velAxis = 0, baseScale = 1;
    float aspect = 1; int startDelay = 0;
    uint8_t color[3] = {255, 255, 255};
    int emitLife = 0, ptclLife = 30;
    uint8_t rScale = 0, rLife = 0, rVel = 0;
    uint8_t interval = 0, alpha0 = 31, airRes = 0, texIdx = 0;
    int loop = 0;
    // anims (courbes DS : seuils en lifeRate 0..255)
    bool hasScale = false; float scS = 1, scM = 1, scE = 1; int scIn = 25, scOut = 255;
    bool hasColor = false; uint8_t cS[3] = {0}, cE[3] = {0};
    int cIn = 0, cPk = 128, cOut = 255; bool cInterp = true;
    bool hasAlpha = false; int aS = 31, aM = 31, aE = 31, aPk1 = 25, aPk2 = 75;
    bool hasTexA = false; uint8_t texFr[8] = {0}; int texN = 0, texStep = 4;
    bool scLoop = false, cLoop = false, aLoop = false, texLoop = false;
    bool randLoopAnim = false, hideParent = false;
    bool hasChild = false; uint16_t chFlags = 0, chLife = 0; float chEndScale = 1;
    float chVelMag = 0;
    uint8_t chCount = 0, chDelay = 0, chIv = 1, chVelR = 0, chScaleR = 0, chTex = 0;
    uint8_t chCol[3] = {0};
    uint32_t behav = 0;
    float grav[3] = {0}, randv[3] = {0}; int randIv = 0;
    float mag[4] = {0}, conv[4] = {0};
    int spinAng = 0;
};
struct Spa {
    std::vector<SpaRes> res;
    std::vector<SpaTex> tex;
    bool parsed = false;
};
static std::unordered_map<int, Spa> g_spas;

static void spa_parse(Spa& s, const std::vector<uint8_t>& d) {
    s.parsed = true;
    if (d.size() < 32) return;
    uint16_t rc, tc; memcpy(&rc, d.data() + 8, 2); memcpy(&tc, d.data() + 10, 2);
    size_t off = 32;
    auto rd = [&](void* p, size_t n) { if (off + n <= d.size()) memcpy(p, d.data() + off, n); off += n; };
    for (int i = 0; i < rc && off + 88 <= d.size(); i++) {
        SpaRes r; uint32_t u32; int32_t i32; int16_t i16; uint16_t u16;
        rd(&r.flags, 4);
        r.emitType = r.flags & 15; r.drawType = (r.flags >> 4) & 3;
        for (int k = 0; k < 3; k++) { rd(&i32, 4); r.basePos[k] = fx32(i32); }
        rd(&i32, 4); r.emitCount = fx32(i32);
        rd(&i32, 4); r.radius = fx32(i32);
        rd(&i32, 4); r.length = fx32(i32);
        for (int k = 0; k < 3; k++) { rd(&i16, 2); r.axis[k] = i16 / 4096.f; }
        rd(&u16, 2); rgb555(u16, r.color);
        rd(&i32, 4); r.velPos = fx32(i32);
        rd(&i32, 4); r.velAxis = fx32(i32);
        rd(&i32, 4); r.baseScale = fx32(i32);
        rd(&i16, 2); r.aspect = i16 / 4096.f; rd(&i16, 2); r.startDelay = i16;
        rd(&i16, 2); /*rotMin*/ rd(&i16, 2); /*rotMax*/
        rd(&i16, 2); /*initAngle*/ rd(&i16, 2); /*reserved*/
        rd(&u16, 2); r.emitLife = u16; rd(&u16, 2); r.ptclLife = u16;
        rd(&u32, 4); r.rScale = u32 & 255; r.rLife = (u32 >> 8) & 255; r.rVel = (u32 >> 16) & 255;
        rd(&u32, 4); r.interval = u32 & 255; r.alpha0 = (u32 >> 8) & 255;
        r.airRes = (u32 >> 16) & 255; r.texIdx = (u32 >> 24) & 255;
        rd(&u32, 4); r.loop = u32 & 255;
        rd(&u32, 4); rd(&u32, 4); rd(&u32, 4);   // poly + userData + pad
                                                  // 88 consommés
        // (repart proprement : les champs ci-dessus totalisent 88)
        if (r.flags & 0x100) {                    // scale anim 12B
            int16_t a, m, e; uint16_t c, f; uint16_t pk;
            rd(&a, 2); rd(&m, 2); rd(&e, 2); rd(&c, 2); rd(&f, 2); rd(&pk, 2);
            r.hasScale = true; r.scS = fx16(a); r.scM = fx16(m); r.scE = fx16(e);
            r.scIn = c & 255; r.scOut = (c >> 8) & 255; r.scLoop = (f & 1) != 0;
        }
        if (r.flags & 0x200) {                    // color anim 12B (curve u32!)
            uint16_t cs, ce, f, pk; uint32_t cv;
            rd(&cs, 2); rd(&ce, 2); rd(&cv, 4); rd(&f, 2); rd(&pk, 2);
            r.hasColor = true; rgb555(cs, r.cS); rgb555(ce, r.cE);
            r.cIn = cv & 255; r.cPk = (cv >> 8) & 255; r.cOut = (cv >> 16) & 255;
            r.cLoop = (f & 2) != 0; r.cInterp = (f & 4) != 0;
        }
        if (r.flags & 0x400) {                    // alpha anim 8B
            uint16_t a, f, cv, pk;
            rd(&a, 2); rd(&f, 2); rd(&cv, 2); rd(&pk, 2);
            r.hasAlpha = true; r.aS = a & 31; r.aM = (a >> 5) & 31; r.aE = (a >> 10) & 31;
            r.aPk1 = cv & 255; r.aPk2 = (cv >> 8) & 255; r.aLoop = (f & 0x100) != 0;
        }
        if (r.flags & 0x800) {                    // tex anim 12B
            uint8_t fr[8]; uint32_t pm;
            rd(fr, 8); rd(&pm, 4);
            r.hasTexA = true; r.texN = pm & 255; r.texStep = (pm >> 8) & 255;
            r.texLoop = (pm & 0x20000) != 0;
            for (int k = 0; k < 8 && k < r.texN; k++) r.texFr[k] = fr[k];
        }
        if (r.flags & 0x10000) {                  // child 20B
            uint16_t cf, rv, es, lt; uint8_t vr, sr; uint16_t col; uint32_t msc;
            rd(&cf, 2); rd(&rv, 2); rd(&es, 2); rd(&lt, 2); rd(&vr, 1); rd(&sr, 1);
            rd(&col, 2); rd(&msc, 4); off += 4;
            r.hasChild = true; r.chFlags = cf; r.chLife = lt; r.chEndScale = fx16(es);
            r.chVelMag = fx16(rv);
            r.chVelR = vr; r.chScaleR = sr; rgb555(col, r.chCol);
            r.chCount = msc & 255; r.chDelay = (msc >> 8) & 255;
            r.chIv = (msc >> 16) & 255; r.chTex = (msc >> 24) & 255;
        }
        r.randLoopAnim = (r.flags & 0x100000) != 0;
        r.hideParent = (r.flags & 0x400000) != 0;
        static const struct { uint32_t fl; int sz; } BH[] = {
            {0x1000000, 8}, {0x2000000, 8}, {0x4000000, 16},
            {0x8000000, 4}, {0x10000000, 8}, {0x20000000, 16}};
        for (auto& b : BH) if (r.flags & b.fl) {
            if (b.fl == 0x1000000) { int16_t x, y, z; uint16_t p;
                rd(&x, 2); rd(&y, 2); rd(&z, 2); rd(&p, 2);
                r.behav |= 1; r.grav[0] = fx16(x); r.grav[1] = fx16(y); r.grav[2] = fx16(z);
            } else if (b.fl == 0x2000000) { int16_t x, y, z; uint16_t iv;
                rd(&x, 2); rd(&y, 2); rd(&z, 2); rd(&iv, 2);
                r.behav |= 2; r.randv[0] = fx16(x); r.randv[1] = fx16(y);
                r.randv[2] = fx16(z); r.randIv = iv;
            } else if (b.fl == 0x4000000) { int32_t x, y, z; int16_t f2; uint16_t p;
                rd(&x, 4); rd(&y, 4); rd(&z, 4); rd(&f2, 2); rd(&p, 2);
                r.behav |= 4; r.mag[0] = fx32(x); r.mag[1] = fx32(y);
                r.mag[2] = fx32(z); r.mag[3] = fx16(f2);
            } else if (b.fl == 0x8000000) { uint16_t a, ax;
                rd(&a, 2); rd(&ax, 2); r.behav |= 8; r.spinAng = a;
            } else if (b.fl == 0x10000000) { int32_t y2; int16_t el; uint16_t ct;
                rd(&y2, 4); rd(&el, 2); rd(&ct, 2); r.behav |= 16;
            } else if (b.fl == 0x20000000) { int32_t x, y, z; int16_t f2; uint16_t p;
                rd(&x, 4); rd(&y, 4); rd(&z, 4); rd(&f2, 2); rd(&p, 2);
                r.behav |= 32; r.conv[0] = fx32(x); r.conv[1] = fx32(y);
                r.conv[2] = fx32(z); r.conv[3] = fx16(f2);
            }
        }
        s.res.push_back(r);
    }
    if (getenv("BANIMDBG"))
        for (int i = 0; i < (int)s.res.size(); i++) {
            const SpaRes& r = s.res[i];
            fprintf(stderr, "[spa] res%d fl=%08x emitLife=%d pLife=%d emitCnt=%.2f iv=%d alpha0=%d startDelay=%d chCnt=%d chLife=%d\n",
                i, r.flags, r.emitLife, r.ptclLife, r.emitCount, r.interval,
                r.alpha0, r.startDelay, r.chCount, r.chLife);
        }
    // textures : A5I3 (fmt6) / A3I5 (fmt1) -> RGBA + GL
    for (int i = 0; i < tc && off + 32 <= d.size(); i++) {
        uint32_t hdr[8]; memcpy(hdr, d.data() + off, 32);
        uint32_t param = hdr[1], tsz = hdr[2], pof = hdr[3], psz = hdr[4], rsz = hdr[7];
        int fmt = param & 15, w = 8 << ((param >> 4) & 15), h = 8 << ((param >> 8) & 15);
        SpaTex t; t.w = w; t.h = h;
        size_t to = off + 32;
        if ((fmt == 6 || fmt == 1) && to + tsz <= d.size() && pof + psz <= rsz) {
            std::vector<uint8_t> px(w * h * 4);
            size_t palo = off + pof;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                uint8_t v = d[to + (size_t)y * w + x];
                int idx, al;
                if (fmt == 6) { idx = v & 7; al = (v >> 3) & 31; }
                else { idx = v & 31; al = (v >> 5) & 7; }
                uint8_t c[3] = {0};
                if (palo + 2 * idx + 1 < d.size()) {
                    uint16_t pv; memcpy(&pv, d.data() + palo + 2 * idx, 2);
                    rgb555(pv, c);
                }
                uint8_t* q = &px[((size_t)y * w + x) * 4];
                q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
                q[3] = fmt == 6 ? al * 8 : al * 36;
            }
            glGenTextures(1, &t.gl);
            glBindTexture(GL_TEXTURE_2D, t.gl);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, px.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        s.tex.push_back(t);
        off += rsz ? rsz : 32;
    }
}
static Spa* spa_get(int id) {
    if (id < 0 || id >= (int)g_spa.size()) return nullptr;
    auto& s = g_spas[id];
    if (!s.parsed) spa_parse(s, g_spa[id]);
    return &s;
}

// ── runtime ──────────────────────────────────────────────────────────────────
struct Particle {
    float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    int age = 0, life = 0; float scl = 1;
    int res = -1, spa = -1; int ti = 0;
    bool child = false;
    uint8_t lrOff = 0;                      // offset aléatoire des anims bouclées
    uint8_t qcol[3] = {0}; bool hasQCol = false; // couleur figée à la naissance (enfants)
};
struct Emitter {
    bool on = false; int spa = -1, res = -1, anchor = -1;
    float x = 0, y = 0;                     // coords écran DS absolues (256x192)
    int t = 0, sinceEmit = 0, emitted = 0;
    float xp[8] = {0};                      // SetExtraParams
    int mvt = -9999; float mx0 = 0, my0 = 0, mx1 = 0, my1 = 0; int mdur = 1;
    float mr = 20.f; bool parab = false;
};
struct FuncTask {                            // effet battler/fondu asynchrone
    int kind = 0, t = 0, dur = 1, tgt = -1;
    float a[8] = {0};
};
struct Banim {
    bool on = false; int move = -1, atk = 0, def = 1; bool dbl = false;
    // VM
    const uint8_t* pc = nullptr; const uint8_t* base = nullptr; const uint8_t* end = nullptr;
    int32_t vars[8] = {0};
    int delay = 0, waitTasks = false, waitEmit = false, ended = true;
    const uint8_t* calls[8]; int ncall = 0;
    struct { const uint8_t* pc; int n; } loops[8]; int nloop = 0;
    Spa* psys[8] = {nullptr};               // LoadPS slots
    Emitter emit[8];
    std::vector<Particle> parts;
    std::vector<FuncTask> tasks;
    // état visuel battlers consulté par le rendu
    float bdx[4] = {0}, bdy[4] = {0}, bsa[4] = {1, 1, 1, 1}, bsy[4] = {1, 1, 1, 1};
    float balp[4] = {1, 1, 1, 1}; bool bhid[4] = {0}; int bblink[4] = {0};
    uint8_t fadeC[3] = {0}; float fadeA = 0;   // FadeBg overlay
    uint8_t flashC[3] = {255, 255, 255}; float flashA[4] = {0};
};
static Banim G;

static int rdw(const uint8_t*& pc) {
    uint32_t v; memcpy(&v, pc, 4); pc += 4;
    return v & 0x80000000 ? (int32_t)v : (int32_t)v;
}
static float pos_param(const Emitter& e, int i) { return e.xp[i]; }

// ── émission d'une particule depuis une ressource ────────────────────────────
static void emit_one(Emitter& e, Spa& s, const SpaRes& r, bool child,
                     float px, float py, float pz) {
    if (G.parts.size() >= 500) return;
    Particle p; p.spa = e.spa; p.res = e.res; p.child = child;
    float rv = r.rVel ? frnd(1.f - r.rVel / 255.f, 1.f) : 1.f;
    // position initiale selon la forme d'émission
    float ex = 0, ey = 0, ez = 0;
    switch (r.emitType) {
    case 1: {                               // sphere surface
        float u = frnd(-1, 1), ph = frnd(0, 6.2832f);
        float rr = sqrtf(1 - u * u) * r.radius;
        ex = rr * cosf(ph); ey = u * r.radius; ez = rr * sinf(ph);
    } break;
    case 2: {                               // circle border (plan XY)
        float ph = frnd(0, 6.2832f);
        ex = r.radius * cosf(ph); ey = r.radius * sinf(ph);
    } break;
    case 3: {                               // circle uniform
        float ph = frnd(0, 6.2832f), rr = r.radius * sqrtf(frnd(0, 1));
        ex = rr * cosf(ph); ey = rr * sinf(ph);
    } break;
    case 4: {                               // sphere volume
        float u = frnd(-1, 1), ph = frnd(0, 6.2832f);
        float rr = powf(frnd(0, 1), 1 / 3.f) * r.radius;
        ex = rr * sqrtf(1 - u * u) * cosf(ph); ey = rr * u; ez = rr * sqrtf(1 - u * u) * sinf(ph);
    } break;
    case 5: {                               // circle (disque XZ)
        float ph = frnd(0, 6.2832f), rr = r.radius * sqrtf(frnd(0, 1));
        ex = rr * cosf(ph); ez = rr * sinf(ph);
    } break;
    case 6: case 7: {                       // cylindre
        float ph = frnd(0, 6.2832f), hh = frnd(0, r.length);
        float rr = (r.emitType == 6) ? r.radius : r.radius * sqrtf(frnd(0, 1));
        ex = rr * cosf(ph); ey = hh; ez = rr * sinf(ph);
    } break;
    default: break;                         // point
    }
    // vélocité : velPos le long du rayon + velAxis le long de l'axe
    float vl = sqrtf(ex * ex + ey * ey + ez * ez);
    float nx = vl > 0.01f ? ex / vl : 0.f, ny = vl > 0.01f ? ey / vl : 1.f, nz = vl > 0.01f ? ez / vl : 0.f;
    p.vx = (nx * r.velPos + r.axis[0] * r.velAxis) * rv;
    p.vy = (ny * r.velPos + r.axis[1] * r.velAxis) * rv;
    p.vz = (nz * r.velPos + r.axis[2] * r.velAxis) * rv;
    p.x = px + r.basePos[0] + ex; p.y = py + r.basePos[1] + ey; p.z = pz + r.basePos[2] + ez;
    p.life = r.ptclLife;
    if (r.rLife) p.life = (int)(p.life * frnd(1.f - r.rLife / 255.f, 1.f));
    if (p.life < 1) p.life = 1;
    p.scl = r.baseScale * (r.rScale ? frnd(1.f - r.rScale / 255.f, 1.f) : 1.f);
    if (child) p.scl *= r.chScaleR ? r.chScaleR / 255.f * 0.01f + 0.99f : 1.f;
    p.ti = r.texIdx;
    p.lrOff = r.randLoopAnim ? (uint8_t)(rand() & 255) : 0;
    G.parts.push_back(p);
}

// lifeRate DS : 0..255 ; boucle = phase sur loopFrames + offset aléatoire
static int lr_of(const Particle& p, const SpaRes& r, bool lp) {
    if (lp && r.loop > 0)
        return (p.age * 255 / r.loop + p.lrOff) & 255;
    int lr = p.life > 0 ? p.age * 255 / p.life : 0;
    return lr > 255 ? 255 : lr;
}
// couleur courante DS : base -> start -> base -> end sur 3 phases
static void eval_col(const Particle& p, const SpaRes& r, uint8_t out[3]) {
    if (p.child && p.hasQCol) { memcpy(out, p.qcol, 3); return; }
    out[0] = r.color[0]; out[1] = r.color[1]; out[2] = r.color[2];
    if (!r.hasColor) return;
    int lr = lr_of(p, r, r.cLoop);
    const uint8_t* a = r.cS; const uint8_t* b = r.color; float f = 1.f;
    if (lr < r.cIn) { a = r.cS; b = r.cS; }
    else if (lr < r.cPk) {
        f = r.cInterp && r.cPk > r.cIn ? (float)(lr - r.cIn) / (r.cPk - r.cIn) : 1.f;
    } else if (lr < r.cOut) {
        a = r.color; b = r.cE;
        f = r.cInterp && r.cOut > r.cPk ? (float)(lr - r.cPk) / (r.cOut - r.cPk) : 1.f;
    } else { a = r.cE; b = r.cE; }
    for (int i = 0; i < 3; i++) out[i] = (uint8_t)(a[i] + (b[i] - a[i]) * f);
}

// ── CallFunc : tâches d'effet battler ────────────────────────────────────────
// centre des sprites battler en px DS (mon_ctr du rendu, canvas 256x192)
static void ds_anchor(int b, float& x, float& y) {
    if (!G.dbl) { x = (b & 1) ? 192.f : 64.f; y = (b & 1) ? 48.f : 112.f; return; }
    static const float X[4] = {40, 216, 80, 176}, Y[4] = {112, 50, 120, 42};
    x = X[b & 3]; y = Y[b & 3];
}
// cible : masque BATTLE_ANIM_* -> battler id (partner = autre slot, bid^2)
static int tgt_of(int32_t m) {
    if (m & 2) return G.atk;                 // ATTACKER
    if (m & 8) return G.def;                 // DEFENDER
    if (m & 4) return G.atk ^ 2;             // ATTACKER_PARTNER
    if (m & 16) return G.def ^ 2;            // DEFENDER_PARTNER
    return G.def;
}
static void func_task(int fid, const int32_t* a, int n) {
    FuncTask t; t.tgt = G.def;
    switch (fid) {
    case 33:                                // FadeBg: bgType,delay,start,end,color
        if (n >= 5) { t.kind = 1; t.dur = a[1] > 0 ? a[1] : 1;
            t.a[0] = a[2]; t.a[1] = a[3];
            rgb555(a[4] & 0x7FFF, G.fadeC); }
        break;
    case 34:                                // FadeBattlerSprite: tgt,stepFrames,endDelay,color,alpha,hold
        if (n >= 4) { t.kind = 2; t.dur = a[1] > 0 ? a[1] : 1;
            t.tgt = tgt_of(a[0]);
            uint8_t c[3]; rgb555(a[3] & 0x7FFF, c);
            t.a[0] = c[0]; t.a[1] = c[1]; t.a[2] = c[2];
            t.a[3] = n > 4 ? a[4] : 255; t.a[4] = n > 5 ? a[5] : 0; }
        break;
    case 35:                                // ScalePokemonSprite: tgt,alpha,s0,s1,ref,cyc,frames,spr
        if (n >= 7) { t.kind = 4; t.tgt = tgt_of(a[0]);
            t.dur = a[6] > 0 ? a[6] : 1;
            t.a[0] = t.a[2] = a[2] / 100.f; t.a[1] = t.a[3] = a[3] / 100.f;
            t.a[4] = a[5]; }
        break;
    case 36: {                              // Shake: ex,ey,interval,amount,targets
        t.kind = 3;
        t.dur = (n > 2 && a[2] > 0 ? a[2] : 1) * (n > 3 && a[3] > 0 ? a[3] : 4);
        t.a[0] = n > 0 ? a[0] : 2; t.a[1] = n > 1 ? a[1] : 0;
        t.tgt = n > 4 ? tgt_of(a[4]) : G.def;
    } break;
    case 40: if (n >= 1) { int tb = tgt_of(a[0]);
            if (tb >= 0 && tb < 4) G.bhid[tb] = n > 1 && a[1] != 0; } return;
    case 42:                                // ScaleBattlerSprite: tgt,sx0,sx1,sy0,sy1,ref,cycles,frames
        if (n >= 8) { t.kind = 4; t.tgt = tgt_of(a[0]); t.dur = a[7] > 0 ? a[7] : 1;
            for (int i = 0; i < 4; i++) t.a[i] = a[1 + i] / 100.f;
            t.a[4] = a[6]; }
        break;
    case 50:                                // BlinkAttacker: count,interval
        if (n >= 2) { G.bblink[G.atk] = a[0] * a[1] + 1; } return;
    case 51: case 52:                       // MoveBattlerX: frames,offset,target
        if (n >= 3) { t.kind = 5; t.tgt = tgt_of(a[2]);
            t.dur = a[0] > 0 ? a[0] : 1; t.a[0] = a[1]; }
        break;
    case 57:                                // MoveBattler: frames,dx,dy,target
        if (n >= 4) { t.kind = 6; t.tgt = tgt_of(a[3]);
            t.dur = a[0] > 0 ? a[0] : 1;
            float s = (t.tgt & 1) ? -1.f : 1.f;   // dir transform (camp ennemi)
            t.a[0] = a[1] * s; t.a[1] = a[2] * s; }
        break;
    case 60:                                // RevolveBattler: target,revs,framesPerRev
        if (n >= 3) { t.kind = 10; t.tgt = tgt_of(a[0]);
            t.dur = a[2] > 0 ? a[1] * a[2] : 30; t.a[0] = a[1] > 0 ? a[1] : 1; }
        break;
    case 61:                                // MoveOffScreen: target,frames
        if (n >= 1) { t.kind = 7; t.tgt = tgt_of(a[0]);
            t.dur = n > 1 && a[1] > 0 ? a[1] : 20; t.a[0] = 1; } break;
    case 62:                                // MoveToDefault: target,frames
        if (n >= 1) { t.kind = 7; t.tgt = tgt_of(a[0]);
            t.dur = n > 1 && a[1] > 0 ? a[1] : 10; t.a[0] = 0; } break;
    case 63:                                // FadePokemonSprite: tgt,stepF,step,start,end,color
        if (n >= 5) { t.kind = 8; t.tgt = tgt_of(a[0]);
            int st = (int)labs(a[4] - a[3]) / (a[2] > 0 ? a[2] : 1);
            t.dur = a[1] * (st > 0 ? st : 1);
            t.a[0] = a[3] / 100.f; t.a[1] = a[4] / 100.f; }
        break;
    case 65: case 66: {                     // MoveEmitterA2B: id,dx,dy,delay,frames,radius,mode
        if (n >= 5) {
            int ei = a[0];
            if (ei >= 0 && ei < 8 && G.emit[ei].on) {
                Emitter& e = G.emit[ei];
                bool dta = n > 6 && a[6] == 1;   // mode 1 = def -> atk
                int sb = dta ? G.def : G.atk, eb = dta ? G.atk : G.def;
                float dir = (G.atk & 1) ? -1.f : 1.f;
                float sx, sy, ex2, ey2;
                ds_anchor(sb, sx, sy); ds_anchor(eb, ex2, ey2);
                e.mvt = n > 3 && a[3] > 0 ? -(int)a[3] : 0;   // délai de départ
                e.mdur = a[4] > 0 ? a[4] : 1;
                e.mx0 = sx; e.my0 = sy;
                e.mx1 = ex2 + (n > 1 ? a[1] * dir : 0);
                e.my1 = ey2 + (n > 2 ? a[2] * dir : 0);
                if (n > 5 && a[5] > 0) e.mr = (float)a[5];
                e.parab = fid == 66;
            }
        }
    } return;
    case 77:                                // MoveBattlerOnOrOffScreen: mode,target,frames
        if (n >= 2) { t.kind = 7; t.tgt = tgt_of(a[1]);
            t.dur = n > 2 && a[2] > 0 ? a[2] : 20; t.a[0] = a[0] ? 0.f : 1.f; }
        break;
    case 80: case 81: case 82: case 83: {   // StatChange: flash battler + flèche
        t.kind = 9; t.tgt = G.def; t.dur = 30; t.a[0] = fid == 81 ? -1 : 1;
    } break;
    default: return;                        // fonction non gérée : rien
    }
    if (t.dur <= 0) t.dur = 1;
    G.tasks.push_back(t);
}

// ── VM ───────────────────────────────────────────────────────────────────────
// ancre d'émetteur selon callbackID (battle_anim.h EMITTER_CB_*)
static int cb_anchor(int cb) {
    switch (cb) {
    case 1: return 1;                        // ENEMY_1
    case 2: return 0;                        // PLAYER_1
    case 3: case 21: return G.atk;           // ATTACKER
    case 4: return G.def;                    // DEFENDER
    case 19: return G.atk & 1;               // côté attaquant (slot 0 du camp)
    case 20: return G.def & 1;               // côté défenseur
    case 5: case 6: case 7: return G.atk;    // axe atk->def : origine attaquant
    default: return G.def;                   // converge/magnet/générique : défenseur
    }
}
static void vm_create_emitter(int slot, int psIdx, int resId, int cb) {
    if (getenv("BANIMDBG"))
        fprintf(stderr, "[banim] create_emitter slot=%d ps=%d res=%d cb=%d psys=%d resN=%d\n",
            slot, psIdx, resId, cb, (psIdx >= 0 && psIdx < 8 && G.psys[psIdx]) ? 1 : 0,
            (psIdx >= 0 && psIdx < 8 && G.psys[psIdx]) ? (int)G.psys[psIdx]->res.size() : -1);
    if (slot < 0 || slot >= 8 || psIdx < 0 || psIdx >= 8 || !G.psys[psIdx]) return;
    Spa& s = *G.psys[psIdx];
    if (resId < 0 || resId >= (int)s.res.size()) return;
    Emitter& e = G.emit[slot]; e = Emitter{};
    e.on = true; e.spa = psIdx; e.res = resId;
    e.anchor = cb_anchor(cb);
    if (e.anchor < 0) e.anchor = G.def;
    ds_anchor(e.anchor, e.x, e.y);          // position initiale = battler ancre
}
static void vm_step() {
    int guard = 0;
    while (!G.ended && !G.delay && !G.waitTasks && !G.waitEmit && ++guard < 4096) {
        if (G.pc + 4 > G.end) { G.ended = true; break; }
        const uint8_t* instr = G.pc;
        int op = rdw(G.pc);
        auto rel = [&](int w) {              // cible = mot contenant l'offset
            const uint8_t* wpos = G.pc + 4 * w;
            int32_t r; memcpy(&r, wpos, 4);
            return wpos + 4 * r;
        };
        switch (op) {
        case 0: G.delay = rdw(G.pc); break;
        case 1: G.waitTasks = true; break;
        case 2: { int n = rdw(G.pc);
            if (G.nloop < 8) { G.loops[G.nloop].pc = G.pc; G.loops[G.nloop].n = n; G.nloop++; }
        } break;
        case 3: if (G.nloop > 0) {
                if (--G.loops[G.nloop - 1].n > 0) G.pc = G.loops[G.nloop - 1].pc;
                else G.nloop--;
            } break;
        case 4: G.ended = true; break;
        case 10: if (G.ncall < 8) { G.calls[G.ncall++] = G.pc + 4; G.pc = rel(0); }
                 else G.pc += 4; break;
        case 11: if (G.ncall > 0) G.pc = G.calls[--G.ncall]; else G.ended = true; break;
        case 12: { int v = rdw(G.pc), x = rdw(G.pc); if (v >= 0 && v < 8) G.vars[v] = x; } break;
        case 13: {                            // JmpChanceOdd : 2 rels (paire/impaire)
            const uint8_t* w0 = G.pc; int32_t r0; memcpy(&r0, w0, 4);
            const uint8_t* w1 = G.pc + 4; int32_t r1; memcpy(&r1, w1, 4);
            G.pc = (rnd() & 1) ? w1 + 4 * r1 : w0 + 4 * r0;
        } break;
        case 14: {                            // JmpChance pct,rel
            int p = rdw(G.pc);
            const uint8_t* w = G.pc; int32_t r; memcpy(&r, w, 4); G.pc += 4;
            if ((int)(rnd() % 100) < p) G.pc = w + 4 * r;
        } break;
        case 15: { const uint8_t* w = G.pc; int32_t r; memcpy(&r, w, 4);
            G.pc = w + 4 * r; } break;
        case 30: { int a = rdw(G.pc), b = rdw(G.pc);
            const uint8_t* w = G.pc; int32_t r; memcpy(&r, w, 4); G.pc += 4;
            int av = (a >= 0 && a < 8) ? G.vars[a] : a;
            if (av == b) G.pc = w + 4 * r; } break;
        case 33: rdw(G.pc); break;            // JmpUnk01 : ignore
        case 45: {                            // CallFunc fid,argc,args
            int fid = rdw(G.pc), n = rdw(G.pc); if (n > 16) n = 16;
            int32_t a[16]; for (int i = 0; i < n; i++) a[i] = rdw(G.pc);
            func_task(fid, a, n);
        } break;
        case 46: { int ps = rdw(G.pc), rs = rdw(G.pc), cb = rdw(G.pc);
            vm_create_emitter(0, ps, rs, cb); } break;
        case 47: { int ps = rdw(G.pc), ei = rdw(G.pc), rs = rdw(G.pc), cb = rdw(G.pc);
            vm_create_emitter(ei, ps, rs, cb); } break;
        case 48: {                            // EmitterForMove ps,res[6],callback
            int ps = rdw(G.pc); int rt[6]; for (int i = 0; i < 6; i++) rt[i] = rdw(G.pc);
            int cb = rdw(G.pc);
            // battle_anim_system.c : indexTable[typeAtk][typeDef] - 1
            static const int IT[6][6] = {
                {-1, 0,-1,-1,-1,-1}, { 3,-1,-1,-1,-1,-1},
                {-1,-1,-1, 2,-1, 0}, {-1,-1, 5, 3,-1,-1},
                {-1,-1,-1, 0,-1, 1}, {-1,-1,-1, 3,-1, 4}};
            // types DS : solo 0/1 ; double slot1 = 2/3, slot2 = 4/5
            int ta = G.atk, td = G.def;
            int typeA = G.dbl ? 2 + ta : (ta & 1), typeD = G.dbl ? 2 + td : (td & 1);
            int rs = (typeA >= 0 && typeA < 6 && typeD >= 0 && typeD < 6)
                ? IT[typeA][typeD] : -1;
            vm_create_emitter(0, ps, rs >= 0 ? rt[rs] : rt[0], cb);
        } break;
        case 49: {                            // EmitterFF ps,res[4],callback
            int ps = rdw(G.pc); int rt[4]; for (int i = 0; i < 4; i++) rt[i] = rdw(G.pc);
            int cb = rdw(G.pc);
            // GetParticleResIdxForFriendlyFire : indexTable directe
            static const int FT[6][6] = {
                {-1,-1,-1,-1,-1,-1}, {-1,-1,-1,-1,-1,-1},
                {-1,-1,-1,-1, 0,-1}, {-1,-1,-1,-1,-1, 1},
                {-1,-1, 1,-1,-1,-1}, {-1,-1,-1, 0,-1,-1}};
            int typeA = G.dbl ? 2 + G.atk : (G.atk & 1);
            int typeD = G.dbl ? 2 + G.def : (G.def & 1);
            int rs = (typeA >= 0 && typeA < 6 && typeD >= 0 && typeD < 6)
                ? FT[typeA][typeD] : -1;
            vm_create_emitter(0, ps, rs >= 0 ? rt[rs] : rt[0], cb);
        } break;
        case 50: G.waitEmit = true; break;
        case 51: { int sl = rdw(G.pc), id = rdw(G.pc);
            if (sl >= 0 && sl < 8) G.psys[sl] = spa_get(id); } break;
        case 52: rdw(G.pc); rdw(G.pc); rdw(G.pc); break;
        case 53: { int sl = rdw(G.pc); if (sl >= 0 && sl < 8) {
                G.psys[sl] = nullptr;
                for (auto& e : G.emit) if (e.spa == sl) e.on = false;
            } } break;
        case 55: {                            // SetExtraParams n,args -> émetteur 0
            int n = rdw(G.pc); if (n > 8) n = 8;
            for (int i = 0; i < n; i++) {
                int32_t v = rdw(G.pc);
                if (G.emit[0].on) G.emit[0].xp[i] = v;
            }
        } break;
        case 56: case 57: case 58: case 59: case 60: {
            static const int NA[] = {0, 1, 4, 0, 1};
            for (int i = 0; i < NA[op - 56]; i++) rdw(G.pc);
        } break;
        case 61: rdw(G.pc); break;            // CancelTrack : 1 arg
        case 62: case 63: { rdw(G.pc); rdw(G.pc); } break;
        case 64: {                            // JmpBattlerSide v,r1,r2
            rdw(G.pc);
            const uint8_t* w1 = G.pc; int32_t r1; memcpy(&r1, w1, 4);
            const uint8_t* w2 = G.pc + 4; int32_t r2; memcpy(&r2, w2, 4);
            G.pc += 8;
            G.pc = (G.atk & 1) ? w2 + 4 * r2 : w1 + 4 * r1;
        } break;
        case 65: rdw(G.pc); rdw(G.pc); rdw(G.pc); break;
        case 66: rdw(G.pc); break;
        case 67: memset(G.vars, 0, sizeof G.vars); break;
        case 68: case 69: rdw(G.pc); break;
        case 70: for (int i = 0; i < 5; i++) rdw(G.pc); break;  // JmpWeather : none
        case 71: rdw(G.pc); break;            // JmpContest : jamais
        case 72: rdw(G.pc); break;            // JmpFF : jamais
        case 73: for (int i = 0; i < 8; i++) rdw(G.pc); break;
        case 74: case 76: case 77: rdw(G.pc); rdw(G.pc); break;
        case 75: rdw(G.pc); rdw(G.pc); rdw(G.pc); break;
        case 78: { for (int i = 0; i < 8; i++) rdw(G.pc);
            int n = rdw(G.pc); for (int i = 0; i < n; i++) rdw(G.pc); } break;
        case 79: for (int i = 0; i < 8; i++) rdw(G.pc); break;
        case 80: rdw(G.pc); break;
        case 81: rdw(G.pc); rdw(G.pc); break;
        case 82: rdw(G.pc); rdw(G.pc); rdw(G.pc); break;
        case 83: rdw(G.pc); break;
        case 84: G.waitTasks = true; break;   // WaitLRX ~ wait tâches
        case 5: case 8: case 9: case 16: case 17: case 18: case 21:
        case 22: case 23: case 24: case 25: case 26:
        case 29: case 31: case 32: case 34: case 35: case 36: case 44: {
            static const std::unordered_map<int,int> na = {
                {5,1},{8,2},{9,0},{16,2},{17,2},{18,2},{21,1},
                {22,2},{23,1},{24,5},{25,4},{26,3},
                {29,0},{31,2},{32,1},{34,3},{35,5},{36,5},{44,1}};
            auto it = na.find(op); int c = it == na.end() ? 0 : it->second;
            for (int i = 0; i < c; i++) rdw(G.pc);
        } break;
        case 6: case 7: case 19: case 20: case 27: case 28:
        case 37: case 38: case 39: case 40: case 41: case 42: case 43:
        case 54: break;                       // Nop / waits bg : aucun arg
        default:                              // inconnu : resynchronise mot à mot
            G.pc = instr + 4; break;
        }
        (void)instr;
    }
}

// ── API ──────────────────────────────────────────────────────────────────────
void banim_start(Battle& b, int move, int attacker, int defender) {
    if (!banim_load()) { if (getenv("BANIMDBG")) fprintf(stderr, "[banim] assets manquants\n"); return; }
    int n = 0; const uint8_t* s = we_script(move, &n);
    if (!s || n < 8) { if (getenv("BANIMDBG")) fprintf(stderr, "[banim] mv=%d pas de script\n", move); return; }
    G = Banim{};
    G.on = true; G.move = move; G.atk = attacker; G.dbl = b.dbl;
    // tbid peut être -1 (portée résolue en multi-cible) : ancre = 1er battler
    // du camp adverse (slot 0 en double aussi, défaut raisonnable)
    G.def = (defender >= 0 && defender < 4) ? defender : ((attacker & 1) ? 0 : 1);
    G.pc = G.base = s; G.end = s + n; G.ended = false;
    G.parts.reserve(256);
    vm_step();
}
void banim_stop() { G.on = false; G.ended = true; G.parts.clear(); }
bool banim_busy() { return G.on; }

void banim_update(Battle& b, float dt) {
    if (!G.on) return;
    // timing DS : le script compte en frames 60Hz
    static float acc = 0;
    acc += dt;
    int frames = (int)(acc * 60.f);
    if (frames < 1) return;
    acc -= frames / 60.f;
    for (int f = 0; f < frames; f++) {
        // tâches asynchrones (shake, move, fade…)
        for (auto& t : G.tasks) {
            if (t.tgt >= 0 && t.tgt < 4) {
                float k = (float)t.t / t.dur;
                switch (t.kind) {
                case 3: {                    // shake
                    float amp = t.a[0] * (1 - k);
                    G.bdx[t.tgt] = (t.t % 4 < 2 ? amp : -amp);
                    G.bdy[t.tgt] = (t.t % 4 < 2 ? -t.a[1] : t.a[1]) * (1 - k);
                } break;
                case 4: {                    // scale battler
                    float osc = t.a[4] > 0 ? fabsf(sinf(k * 3.1416f * t.a[4])) : 1.f;
                    G.bsa[t.tgt] = t.a[0] + (t.a[1] - t.a[0]) * k * osc + (1 - osc);
                    G.bsy[t.tgt] = t.a[2] + (t.a[3] - t.a[2]) * k * osc + (1 - osc);
                } break;
                case 5: case 6: {            // move battler (va-et-vient)
                    float e = sinf(k * 3.1416f);
                    G.bdx[t.tgt] = t.a[0] * e; G.bdy[t.tgt] = t.a[1] * e;
                } break;
                case 7: {                    // off/retour : sens selon le camp
                    float s = (t.tgt & 1) ? 1.f : -1.f;
                    G.bdx[t.tgt] = t.a[0] > 0 ? k * 300 * s : (1 - k) * 300 * s;
                } break;
                case 8: G.balp[t.tgt] = t.a[0] + (t.a[1] - t.a[0]) * k; break;
                case 9: G.bdy[t.tgt] = t.a[0] * sinf(k * 6.28f) * 4.f; break;
                case 10: {                   // revolve : orbite autour du centre
                    float th = k * 6.2832f * t.a[0];
                    G.bdx[t.tgt] = 18.f * sinf(th);
                    G.bdy[t.tgt] = -18.f * (1.f - cosf(th));
                } break;
                }
            }
            if (t.kind == 1)                 // FadeBg
                G.fadeA = t.a[0] + (t.a[1] - t.a[0]) * ((float)t.t / t.dur);
            if (t.kind == 2 && t.tgt >= 0 && t.tgt < 4) {
                float k = (float)t.t / t.dur;
                G.flashC[0] = (uint8_t)t.a[0]; G.flashC[1] = (uint8_t)t.a[1]; G.flashC[2] = (uint8_t)t.a[2];
                G.flashA[t.tgt] = k < 0.5f ? k * 2 : 2 - k * 2;
            }
            t.t++;
        }
        G.tasks.erase(std::remove_if(G.tasks.begin(), G.tasks.end(),
            [](const FuncTask& t) { return t.t >= t.dur; }), G.tasks.end());
        for (int i = 0; i < 4; i++)
            if (G.bblink[i] > 0) G.bblink[i]--;
        // émetteurs
        for (auto& e : G.emit) {
            if (!e.on || e.spa < 0 || e.spa >= 8 || !G.psys[e.spa]) continue;
            Spa& s = *G.psys[e.spa];
            if (e.res < 0 || e.res >= (int)s.res.size()) continue;
            const SpaRes& r = s.res[e.res];
            if (e.mvt > -9999) {             // déplacement d'émetteur (mvt<0 = délai)
                if (e.mvt >= 0) {
                    float k = fminf(1.f, (float)e.mvt / e.mdur);
                    e.x = e.mx0 + (e.mx1 - e.mx0) * k;
                    e.y = e.my0 + (e.my1 - e.my0) * k;
                    if (e.parab) e.y -= 4 * k * (1 - k) * e.mr;
                }
                if (++e.mvt > e.mdur) e.mvt = -9999;
            }
            e.t++;
            if (e.t <= r.startDelay) continue;
            int emT = e.t - r.startDelay;     // 1..emitLife = âge 0..emitLife-1
            if (r.emitLife == 0 || emT <= r.emitLife) {
                int iv = r.interval > 0 ? r.interval : 1;
                int cnt = (int)(r.emitCount + 0.5f); if (cnt < 1) cnt = 1;
                if ((emT - 1) % iv == 0)
                    for (int n = 0; n < cnt && (int)G.parts.size() < 500; n++)
                        emit_one(e, s, r, false, e.x, e.y, 0);
            }
            // DS : fin quand durée d'émission passée + plus de particules ;
            // emitLife=0 = émet jusqu'à UnloadPS ou fin du script
            bool expired = r.emitLife != 0 ? emT > r.emitLife : G.ended;
            if (expired) {
                bool alive = false;
                for (auto& p : G.parts) if (p.spa == e.spa && p.res == e.res) { alive = true; break; }
                if (!alive) e.on = false;
            }
        }
        // particules
        for (auto& p : G.parts) {
            if (p.res < 0) continue;
            Spa& s = *G.psys[p.spa];
            const SpaRes& r = s.res[p.res];
            p.age++;
            p.vy += r.grav[1]; p.vx += r.grav[0]; p.vz += r.grav[2];
            if (r.behav & 2 && p.age % (r.randIv ? r.randIv : 1) == 0) {
                p.vx += frnd(-r.randv[0], r.randv[0]);
                p.vy += frnd(-r.randv[1], r.randv[1]);
                p.vz += frnd(-r.randv[2], r.randv[2]);
            }
            if (r.airRes) { float d = 1.f - r.airRes / 2550.f;
                p.vx *= d; p.vy *= d; p.vz *= d; }
            if (r.behav & 32) {              // convergence
                p.vx += (r.conv[0] - p.x) * r.conv[3] * 0.01f;
                p.vy += (r.conv[1] - p.y) * r.conv[3] * 0.01f;
            }
            p.x += p.vx; p.y += p.vy; p.z += p.vz;
            // enfants : émission continue pendant la vie du parent (DS EmitChildren)
            if (r.hasChild && !p.child) {
                int delay = p.life * r.chDelay / 256;
                int diff = p.age - delay;
                int iv = r.chIv > 0 ? r.chIv : 1;
                if (diff >= 0 && diff % iv == 0) {
                    int cnt = r.chCount ? r.chCount : 1;
                    for (int c = 0; c < cnt && (int)G.parts.size() < 500; c++) {
                        Particle q; q.spa = p.spa; q.res = p.res; q.child = true;
                        q.x = p.x; q.y = p.y; q.z = p.z;
                        float vr = r.chVelR / 256.f;
                        q.vx = p.vx * vr + frnd(-r.chVelMag, r.chVelMag);
                        q.vy = p.vy * vr + frnd(-r.chVelMag, r.chVelMag);
                        q.vz = p.vz * vr + frnd(-r.chVelMag, r.chVelMag);
                        q.life = r.chLife ? r.chLife : 1;
                        q.scl = p.scl * (r.chScaleR + 1) / 64.f;
                        q.ti = r.chTex;
                        if (r.chFlags & 0x20) memcpy(q.qcol, r.chCol, 3);
                        else eval_col(p, r, q.qcol);
                        q.hasQCol = true;
                        G.parts.push_back(q);
                    }
                }
            }
            if (p.age >= p.life) p.res = -1;
        }
        G.parts.erase(std::remove_if(G.parts.begin(), G.parts.end(),
            [](const Particle& p) { return p.res < 0; }), G.parts.end());
        // VM
        if (G.delay > 0 && --G.delay == 0) {}
        if (G.waitTasks && G.tasks.empty()) G.waitTasks = false;
        if (G.waitEmit) {
            bool any = false;
            for (auto& e : G.emit) if (e.on) { any = true; break; }
            if (!any && G.parts.empty()) G.waitEmit = false;
        }
        if (!G.ended) vm_step();
    }
    if (getenv("BANIMDBG")) {
        fprintf(stderr, "[banim] mv=%d on=%d ended=%d parts=%d emit=%d tasks=%d",
            G.move, G.on, G.ended, (int)G.parts.size(),
            [&]{int n=0; for(auto&e:G.emit) n+=e.on; return n;}(),
            (int)G.tasks.size());
        for (auto& t : G.tasks) fprintf(stderr, " k%d=%d/%d", t.kind, t.t, t.dur);
        for (auto& e : G.emit) if (e.on) fprintf(stderr, " e(res%d,t%d)", e.res, e.t);
        int byres[8] = {0}; int nchild = 0;
        for (auto& p : G.parts) { if (p.res >= 0 && p.res < 8) byres[p.res]++; if (p.child) nchild++; }
        fprintf(stderr, " res:["); for (int i = 0; i < 8; i++) if (byres[i]) fprintf(stderr, "%d:%d ", i, byres[i]);
        fprintf(stderr, "] ch=%d\n", nchild);
    }
    // fin naturelle : script fini + plus rien à l'écran
    if (G.ended && G.tasks.empty() && G.parts.empty()) {
        bool any = false;
        for (auto& e : G.emit) if (e.on) { any = true; break; }
        if (!any) G.on = false;
        for (int i = 0; i < 4; i++) { G.bdx[i] = G.bdy[i] = 0;
            G.bsa[i] = G.bsy[i] = 1; G.balp[i] = 1; G.bhid[i] = false; }
    }
}

// ── rendu ────────────────────────────────────────────────────────────────────
void banim_draw(ImDrawList* dl, const float feet[4][2], float uds) {
    if (!G.on || !dl) return;
    if (G.fadeA > 0.f) {                     // FadeBg overlay plein écran
        ImVec2 mx = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled({0, 0}, mx, IM_COL32(G.fadeC[0], G.fadeC[1],
            G.fadeC[2], (int)(fminf(1.f, G.fadeA) * 255)));
    }
    if (G.parts.empty()) return;
    // origine du canvas DS : les particules vivent en coords écran DS absolues
    // (256x192) ; feet[di] correspond à ds_anchor(di) -> on en déduit l'offset
    int di = (G.def >= 0 && G.def < 4) ? G.def : ((G.atk & 1) ? 0 : 1);
    float dax, day; ds_anchor(di, dax, day);
    float ox = feet[di][0] - dax * uds;
    float oy = feet[di][1] - 20.f * uds - day * uds;
    if (getenv("BANIMDBG")) {
        float mx = ImGui::GetIO().DisplaySize.x, my = ImGui::GetIO().DisplaySize.y;
        int inside = 0, draw = 0;
        for (const Particle& p : G.parts) {
            float px = ox + p.x * uds, py = oy + p.y * uds;
            if (px > -40 && px < mx + 40 && py > -40 && py < my + 40) inside++;
            const SpaRes& r = G.psys[p.spa]->res[p.res];
            int al = r.alpha0;
            if (al > 0) draw++;
        }
        fprintf(stderr, "[banim-draw] def=%d atk=%d ox=%.0f oy=%.0f uds=%.2f parts=%d inside=%d alpha>0=%d\n",
            G.def, G.atk, ox, oy, uds, (int)G.parts.size(), inside, draw);
    }
    for (const Particle& p : G.parts) {
        if (p.spa < 0 || p.spa >= 8 || !G.psys[p.spa]) continue;
        if (p.res < 0 || p.res >= (int)G.psys[p.spa]->res.size()) continue;
        Spa& s = *G.psys[p.spa];
        const SpaRes& r = s.res[p.res];
        if (r.hideParent && !p.child) continue;
        float sc = p.scl;
        if (r.hasScale) {                    // scale anim DS : S->[in]->M->[out]->E
            int lr = lr_of(p, r, r.scLoop);
            if (lr < r.scIn) sc *= r.scS + (r.scM - r.scS) * lr / (r.scIn ? r.scIn : 1);
            else if (lr < r.scOut) sc *= r.scM;
            else sc *= r.scE + (r.scE - r.scM) * (lr - 255) / (255 - (r.scOut < 255 ? r.scOut : 254));
        }
        int lrB = p.life > 0 ? p.age * 255 / p.life : 0; if (lrB > 255) lrB = 255;
        if (p.child && (r.chFlags & 2))      // child scale : 1.0 -> endScale
            sc *= r.chEndScale + (r.chEndScale - 1.f) * (lrB - 255) / 255.f;
        uint8_t cc[3]; eval_col(p, r, cc);
        uint8_t cr = cc[0], cg = cc[1], cb = cc[2];
        int al = r.alpha0;
        if (r.hasAlpha) {                    // alpha anim DS : S->[in]->M->[out]->E
            int lr = lr_of(p, r, r.aLoop);
            if (lr < r.aPk1) al = r.aS + (r.aM - r.aS) * lr / (r.aPk1 ? r.aPk1 : 1);
            else if (lr < r.aPk2) al = r.aM;
            else al = r.aE + (r.aE - r.aM) * (lr - 255) / (255 - (r.aPk2 < 255 ? r.aPk2 : 254));
        }
        if (p.child && (r.chFlags & 4))      // child alpha : fondu sortant
            al = (255 - lrB) * 31 / 255;
        int ti = p.ti;
        if (r.hasTexA && r.texN > 0) {       // DS : seuils step*(i+1) en lifeRate
            int lr = lr_of(p, r, r.texLoop);
            for (int i = 0; i < r.texN; i++)
                if (lr < r.texStep * (i + 1)) { ti = r.texFr[i]; break; }
        }
        GLuint gl = (ti >= 0 && ti < (int)s.tex.size()) ? s.tex[ti].gl : 0;
        float px = ox + p.x * uds;
        float py = oy + (p.y - p.z * 0.5f) * uds;   // y+ = bas, z+ = hauteur
        float hw = (gl ? s.tex[ti].w : 16) * sc * uds * 0.5f;
        float hh = (gl ? s.tex[ti].h : 16) * sc * uds * 0.5f * r.aspect;
        ImU32 col = IM_COL32(cr, cg, cb, (int)(fminf(31.f, (float)al) * 8.2f));
        if (getenv("BANIMMARK")) { dl->AddCircleFilled({px, py}, 5, IM_COL32(255, 0, 255, 200)); continue; }
        if (gl) dl->AddImage((ImTextureID)(intptr_t)gl,
            {px - hw, py - hh}, {px + hw, py + hh}, {0, 0}, {1, 1}, col);
        else dl->AddCircleFilled({px, py}, hw > 1 ? hw : 1, col);
    }
}

// accesseurs pour le rendu battler (offsets/scale/alpha/hide/flash par bid)
extern "C" {
float banim_bdx(int i) { return G.on && i >= 0 && i < 4 ? G.bdx[i] : 0; }
float banim_bdy(int i) { return G.on && i >= 0 && i < 4 ? G.bdy[i] : 0; }
float banim_bsx(int i) { return G.on && i >= 0 && i < 4 ? G.bsa[i] : 1; }
float banim_bsy(int i) { return G.on && i >= 0 && i < 4 ? G.bsy[i] : 1; }
float banim_balp(int i) { return G.on && i >= 0 && i < 4 ? G.balp[i] : 1; }
int banim_bhid(int i) { return G.on && i >= 0 && i < 4 ? G.bhid[i] : 0; }
int banim_bblink(int i) { return G.on && i >= 0 && i < 4 ? G.bblink[i] : 0; }
float banim_flash(int i, uint8_t c[3]) {
    if (!G.on || i < 0 || i >= 4 || G.flashA[i] <= 0) return 0;
    c[0] = G.flashC[0]; c[1] = G.flashC[1]; c[2] = G.flashC[2];
    return G.flashA[i];
}
}
