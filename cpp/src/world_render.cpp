// world_render.cpp — meshes GL, textures, billboards, caméra, draw.
#include "world.h"
#include "world_internal.h"
#include "msg.h"
#include "spr.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <map>
#include "gfx_map.inc"
#include "gfx_3d.inc"
#include "gfx_shadow.inc"

static int gfx3d_member(int gfx) {
    for (int i = 0; i < GFX_3D_N; i++)
        if (GFX_3D[i].gfx == gfx) return GFX_3D[i].member;
    return -1;
}

// construit (et cache) les meshes GL d'un objet 3D fldeff
std::vector<GpuMesh>& World::_obj_3d(int gfx) {
    auto it = _obj3d.find(gfx);
    if (it != _obj3d.end()) return it->second;
    auto& out = _obj3d[gfx];
    int mi = gfx3d_member(gfx);
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (mi < 0 || !n || mi >= n->count()) return out;
    auto model = std::make_shared<bmd::Model>(bmd::parse_bmd(n->member(mi)));
    for (auto& mg : model->meshes) {
        if (verbose) {
            Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            for (auto& pr : mg.prims) for (auto& v : pr.v) {
                mn.x = std::min(mn.x, v.p.x); mn.y = std::min(mn.y, v.p.y); mn.z = std::min(mn.z, v.p.z);
                mx.x = std::max(mx.x, v.p.x); mx.y = std::max(mx.y, v.p.y); mx.z = std::max(mx.z, v.p.z);
            }
            printf("[fldeff] gfx=%d mem=%d mat=%s tex=%s verts=%d bbox %.1f..%.1f %.1f..%.1f %.1f..%.1f\n",
                gfx, mi, mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                (int)mg.prims.size() ? (int)mg.prims[0].v.size() : 0,
                mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
        }
        std::vector<Vec3> ct; Vec3 o{0,0,0};
        auto buf = _tris(mg, o, &ct);
        bool alpha = false; int polyA = 31;
        uint32_t t = _mat_tex(mg.matdef, model->textures, &alpha, &polyA);
        GpuMesh g = rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
        g.alpha_cut = alpha;
        g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                /*DS attr*/ g.lit = mg.matdef.poly & 0xF;
        if (!t && (mg.matdef.dif & 0x8000))
            g.tint = rgb555f(mg.matdef.dif & 0x7FFF);
        out.push_back(g);
    }
    return out;
}

// ombre des persos DS (ov5_021F134C.c) : fldeff.narc membre 17 = shadow.nsbmd
// (res/graphics/field_effects/field_effects.order ligne 18)
void World::_load_shadow() {
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (!n || n->count() <= 17) return;
    auto model = std::make_shared<bmd::Model>(bmd::parse_bmd(n->member(17)));
    for (auto& mg : model->meshes) {
        std::vector<Vec3> ct; Vec3 o{0,0,0};
        auto buf = _tris(mg, o, &ct);
        if (verbose) {
            Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            for (auto& pr : mg.prims) for (auto& v : pr.v) {
                mn.x=std::min(mn.x,v.p.x); mn.y=std::min(mn.y,v.p.y); mn.z=std::min(mn.z,v.p.z);
                mx.x=std::max(mx.x,v.p.x); mx.y=std::max(mx.y,v.p.y); mx.z=std::max(mx.z,v.p.z);
            }
            printf("[shadow] mat=%s bbox %.1f..%.1f %.1f..%.1f %.1f..%.1f\n",
                mg.matdef.name.c_str(), mn.x,mx.x, mn.y,mx.y, mn.z,mx.z);
        }
        bool alpha = false; int polyA = 31;
        uint32_t t = _mat_tex(mg.matdef, model->textures, &alpha, &polyA);
        GpuMesh g = rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
        g.alpha_cut = alpha;
        g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                /*DS attr*/ g.lit = mg.matdef.poly & 0xF;
        _shadow_meshes.push_back(g);
    }
}



// ── triangulation DS -> GL ────────────────────────────────────────────────────
// prim 0: tris  1: quads  2: tri-strip  3: quad-strip (v0,v1,v3,v2)
static void emit(std::vector<float>& out, std::vector<Vec3>* col,
    const bmd::Vert& v, float tw, float th) {
    out.insert(out.end(), {v.p.x, v.p.y, v.p.z, v.n.x, v.n.y, v.n.z,
        v.c.r, v.c.g, v.c.b, v.c.a, v.uv.x / tw, v.uv.y / th});
    if (col) col->push_back(v.p);
}
static void tri(std::vector<float>& out, std::vector<Vec3>* col,
    const std::vector<bmd::Vert>& v, int a, int b, int c, float tw, float th) {
    emit(out, col, v[a], tw, th); emit(out, col, v[b], tw, th); emit(out, col, v[c], tw, th);
}

std::vector<float> World::_tris(const bmd::MeshGroup& mg, Vec3& /*org*/,
    std::vector<Vec3>* coltris) {
    std::vector<float> out;
    float tw = mg.matdef.tw > 0 ? mg.matdef.tw : 1;
    float th = mg.matdef.th > 0 ? mg.matdef.th : 1;
    for (auto& pr : mg.prims) {
        auto& v = pr.v;
        int n = (int)v.size();
        switch (pr.prim) {
        case 0:
            for (int i = 0; i + 2 < n; i += 3)
                tri(out, coltris, v, i, i + 1, i + 2, tw, th);
            break;
        case 1:
            for (int i = 0; i + 3 < n; i += 4) {
                tri(out, coltris, v, i, i + 1, i + 2, tw, th);
                tri(out, coltris, v, i, i + 2, i + 3, tw, th);
            }
            break;
        case 2:
            for (int i = 0; i + 2 < n; i++) {
                if (i % 2 == 0) tri(out, coltris, v, i, i + 1, i + 2, tw, th);
                else            tri(out, coltris, v, i + 1, i, i + 2, tw, th);
            }
            break;
        case 3:
            for (int i = 0; i + 3 < n; i += 2) {
                tri(out, coltris, v, i, i + 1, i + 3, tw, th);
                tri(out, coltris, v, i, i + 3, i + 2, tw, th);
            }
            break;
        }
    }
    _nverts += (int)out.size() / rnd::VF;
    return out;
}

// ── box helper (joueur / PNJ) ─────────────────────────────────────────────────
GpuMesh World::_box(float hx, float hy, float hz, RGBAf c) {
    const float p[8][3] = {{-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz},
        {-hx,-hy,hz},{hx,-hy,hz},{hx,hy,hz},{-hx,hy,hz}};
    const int F[12][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
    const float N[6][3] = {{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}};
    const int FN[12] = {0,0,1,1,2,2,3,3,4,4,5,5};
    std::vector<float> buf;
    for (int f = 0; f < 12; f++)
        for (int k = 0; k < 3; k++) {
            auto& q = p[F[f][k]];
            buf.insert(buf.end(), {q[0], q[1], q[2],
                N[FN[f]][0], N[FN[f]][1], N[FN[f]][2],
                c.r, c.g, c.b, c.a, 0.f, 0.f});
        }
    return rnd::upload(buf.data(), 36, 0);
}

// ── textures / matériaux ──────────────────────────────────────────────────────
uint32_t World::_mat_tex(const bmd::MatDef& md,
    const std::unordered_map<std::string, bmd::TexEnt>& texs, bool* alpha,
    int* polyA) {
    *polyA = (md.poly >> 16) & 31;
    *alpha = *polyA < 31;
    if (md.tex_name.empty()) return 0;
    auto it = texs.find(md.tex_name);
    if (it == texs.end()) {
        if (verbose) printf("    [tex] '%s' (mat %s) absent du texset\n",
            md.tex_name.c_str(), md.name.c_str());
        return 0;
    }
    // transparence index-0 = flag c0t du TEXIMAGE_PARAM (hardware DS)
    bool decal = false;
    auto img = bmd::tex_image(const_cast<bmd::TexEnt&>(it->second),
        md.pal_name, decal);
    if (!img) return 0;
    // fldtanime : texture animée (eau, fleurs) -> id dédié, frames réécrites
    if (!_fltdef_ok) _flt_load();
    if (_fltdef.count(md.tex_name)) {
        int poff = 0;
        auto pi = it->second.pals.find(md.pal_name);
        if (pi != it->second.pals.end()) poff = pi->second;
        uint32_t at = _flt_tex(md.tex_name + "|" + md.pal_name,
            const_cast<bmd::TexEnt&>(it->second), poff);
        if (at) { *alpha = true; return at; }
    }
    *alpha = true;   // RGBA8 -> alpha scissor comme dans world.gd
    auto tc = _texcache.find(img.get());
    if (tc != _texcache.end()) return tc->second.second;
    uint32_t t = rnd::upload_tex(*img);
    _texcache[img.get()] = {img, t};
    return t;
}

// ── fldtanime : table membre 0 = u32 count + count×{name[16], {u8 texIdx,
// u8 dur}×18 (0xFF fin)} ; chaque entrée i a ses frames dans le membre i+1
// (BTX0), décodées avec le format/la palette de la texture de base (VRAM DS).
void World::_flt_load() {
    _fltdef_ok = true;
    auto n = _db->narc(Nds::P_FLDTANIME);
    if (!n || !n->count()) return;
    const Bytes& t0 = n->member(0);
    if (t0.size() < 4) return;
    int cnt = (int)u32(t0, 0);
    for (int i = 0; i < cnt; i++) {
        size_t o = 4 + (size_t)i * 52;
        if (o + 52 > t0.size()) break;
        char nm[17] = {};
        memcpy(nm, t0.data() + o, 16);
        std::vector<std::pair<int,int>> seq;
        for (int k = 0; k < 18; k++) {
            int ti = t0[o + 16 + k * 2];
            if (ti == 0xFF) break;
            seq.push_back({ti, t0[o + 17 + k * 2] + 1});   // dur+1 ticks
        }
        if (!seq.empty()) _fltdef[nm] = {i, std::move(seq)};
    }
    if (verbose) printf("[fldt] %d textures animées\n", (int)_fltdef.size());
}

uint32_t World::_flt_tex(const std::string& key, bmd::TexEnt& base,
    int pal_off) {
    auto& a = _flt[key];
    if (a.tex) return a.tex;
    auto fd = _fltdef.find(key.substr(0, key.find('|')));
    if (fd == _fltdef.end()) return 0;
    auto n = _db->narc(Nds::P_FLDTANIME);
    int mi = fd->second.first + 1;
    if (!n || mi >= n->count()) return 0;
    std::vector<std::string> order;
    auto ftex = bmd::parse_tex0(n->member(mi), 0, &order);
    for (auto& fr : fd->second.second) {
        if (fr.first >= (int)order.size()) continue;
        auto fe = ftex.find(order[fr.first]);
        if (fe == ftex.end()) continue;
        auto img = bmd::decode_tex_raw(fe->second.d, fe->second.t1,
            fe->second.c1, fe->second.c2, base.fmt, base.w, base.h,
            base.c0t, base.d, base.pal_blk, pal_off);
        a.frames.push_back(img);
        a.ticks.push_back(fr.second);
        a.total += fr.second;
    }
    if (a.frames.empty()) return 0;
    a.tex = rnd::upload_tex(*a.frames[0]);
    a.cur = 0;
    if (verbose) printf("[fldt] '%s' : %d frames / %d ticks\n",
        key.c_str(), (int)a.frames.size(), a.total);
    return a.tex;
}

void World::_flt_step(float dt) {
    _flt_t += dt;
    if (_flt.empty()) return;
    int tk = (int)(_flt_t * 30.f);                    // frames DS à 30 Hz
    for (auto& kv : _flt) {
        auto& a = kv.second;
        if (!a.tex || a.total <= 0) continue;
        int p = tk % a.total, acc = 0, idx = 0;
        for (int i = 0; i < (int)a.ticks.size(); i++) {
            if (p < acc + a.ticks[i]) { idx = i; break; }
            acc += a.ticks[i];
        }
        if (idx != a.cur) {
            a.cur = idx;
            rnd::tex_write(a.tex, *a.frames[idx]);
            if (verbose) printf("[fldt] %s -> frame %d\n", kv.first.c_str(), idx);
        }
    }
}

// sprite overworld : o.gfx (OBJ_EVENT_GFX) -> membre mmodel.narc via GFX_TO_MEMBER
// puis la frame N de la BTX0 (sheet 4 dir × marche). Fallback gfx direct si hors table.
uint32_t World::_mm_tex(int gfx, int frame) {
    int member = (gfx >= 0 && gfx < GFX_TO_MEMBER_N) ? (int)GFX_TO_MEMBER[gfx] : gfx;
    if (gfx == 0 && _gender == 1) member = 91;          // joueuse : player_f.nsbtx
    if (member < 0) member = gfx;                    // gfx sans entrée table : accès direct
    return _mm_member_tex(member, frame);
}

// accès direct au membre mmodel.narc (sprites hors table : surf h=160 f=161)
uint32_t World::_mm_member_tex(int member, int frame) {
    int key = member * 128 + frame;
    auto it = _mmcache.find(key);
    if (it != _mmcache.end()) return it->second;
    uint32_t t = 0;
    auto n = _db->narc(Nds::P_MMODEL);
    if (n && member >= 0 && member < n->count()) {
        auto texs = bmd::parse_tex0(n->member(member), 0);
        std::shared_ptr<Image> img;
        for (auto& kv : texs) {
            auto dot = kv.first.rfind('.');
            if (dot != std::string::npos &&
                atoi(kv.first.c_str() + dot + 1) == frame + 1) {
                img = kv.second.img;
                if (!img)
                    img = bmd::tex_image(kv.second,
                        kv.second.pals.empty() ? "" : kv.second.pals.begin()->first);
                break;
            }
        }
        if (!img && !texs.empty()) img = texs.begin()->second.img;
        if (img) t = rnd::upload_tex(*img);
    }
    _mmcache[key] = t;
    return t;
}

// billboard sphérique face caméra (yaw+pitch — comme les sprites overworld DS
// qui s'inclinent vers la caméra haute pour rester lisibles)
void World::_draw_billboard(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
    const Vec3& cam, const Mat4& mvp) {
    _draw_billboard(bb, tex, pos, cam, mvp, 0.f);
}

// tirage vers la caméra : les billboards perso ne doivent pas être "mangés"
// par le sol ou le rebord bas d'un prop (ov5_021EDEB4 fait pareil sur DS) —
// assez fort pour dégager tapis/escaliers, pas assez pour traverser un mur.
void World::_draw_billboard(const GpuMesh& bb, uint32_t tex, const Vec3& pos0,
    const Vec3& cam, const Mat4& mvp, float pull) {
    Vec3 pos = pos0;
    if (pull > 0.f) {
        Vec3 h{cam.x - pos.x, 0, cam.z - pos.z};   // vers caméra (horizontal)
        float hl = h.len();
        if (hl > 1e-3f) pos += h * (pull / hl);
    }
    Vec3 to{cam.x - pos.x, cam.y - pos.y, cam.z - pos.z};
    float l = to.len();
    to = l > 1e-3f ? to * (1.f / l) : Vec3{0, 0, 1};
    Vec3 r{to.z, 0, -to.x};            // droite = up-monde × face
    float rl = r.len();
    r = rl > 1e-3f ? r * (1.f / rl) : Vec3{1, 0, 0};
    Vec3 u = to.cross(r);              // haut = face × droite
    Mat4 m = Mat4::ident();
    m.m[0] = r.x;  m.m[1] = r.y;  m.m[2]  = r.z;
    m.m[4] = u.x;  m.m[5] = u.y;  m.m[6]  = u.z;
    m.m[8] = to.x; m.m[9] = to.y; m.m[10] = to.z;
    m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
    GpuMesh g = bb;
    g.tex = tex;
    rnd::draw(g, mvp.mul(m));
}

// sprite sauvage : pl_pokegra = 6 membres/espèce (FACE_FRONT=2 -> +2 fem/+3 mâle,
// palette +4 normale/+5 shiny) — pokemon.c:2929-2930.
// Les données NCGR sont XOR-chiffrées (PokemonSprite_DecryptPt) puis stockées
// en bitmap linéaire 160×80 (80 octets/ligne, 2 frames 80×80 côte à côte).
static void pokegra_decrypt(uint8_t* d, size_t n) {          // DecryptPt
    uint32_t seed = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint16_t v = (uint16_t)d[i] | ((uint16_t)d[i + 1] << 8);
        v ^= (uint16_t)seed;
        d[i] = (uint8_t)v; d[i + 1] = (uint8_t)(v >> 8);
        seed = seed * 1103515245u + 24691u;                   // LCRNG DS
    }
}

uint32_t World::_poke_tex(int member, int frame) {
    int key = member * 2 + (frame & 1);
    auto it = _pktex.find(key);
    if (it != _pktex.end()) return it->second;
    uint32_t t = 0;
    int species = member / 6;
    auto pg = _db->narc(Nds::P_POKEGRA);
    if (pg && species >= 0 && species * 6 + 5 < pg->count()) {
        Bytes raw = pg->member(member);                       // face choisie
        if (raw.size() >= 0x30) {
            size_t p = 0x10;
            while (p + 8 <= raw.size()) {                     // section RAHC
                uint32_t tag = u32(raw, p), sz = u32(raw, p + 4);
                if ((tag == 0x43484152 || tag == 0x52414843) && sz >= 0x20) {
                    uint32_t dataSz = u32(raw, p + 8 + 16);
                    uint32_t dataOff = u32(raw, p + 8 + 20);
                    size_t base = p + 8 + dataOff;
                    if (!dataSz || base + dataSz > raw.size())
                        dataSz = (uint32_t)(raw.size() - base);
                    if (base < raw.size() && dataSz >= 6400) {
                        std::vector<uint8_t> px(raw.begin() + base,
                            raw.begin() + base + 6400);
                        pokegra_decrypt(px.data(), px.size());
                        auto pals = spr::parse_nclr(pg->member(species * 6 + 4));
                        if (!pals.empty() && !pals[0].empty()) {
                            auto img = std::make_shared<Image>();
                            img->w = 80; img->h = 80;         // une frame 80x80
                            img->px.resize(80 * 80 * 4, 0);
                            int fx = (frame & 1) * 40;        // frame 1 = moitié droite
                            for (int y = 0; y < 80; y++)
                                for (int x = 0; x < 80; x++) {
                                    int b = px[y * 80 + fx + x / 2];
                                    int idx = (x & 1) ? (b >> 4) : (b & 15);
                                    if (!idx) continue;
                                    const RGBAf& c = pals[0][idx < (int)pals[0].size() ? idx : 0];
                                    size_t o = (y * 80 + x) * 4;
                                    img->px[o] = c.r; img->px[o + 1] = c.g;
                                    img->px[o + 2] = c.b; img->px[o + 3] = 255;
                                }
                            t = rnd::upload_tex(*img);
                        }
                    }
                    break;
                }
                if (sz < 8) break;
                p += sz;
            }
        }
    }
    _pktex[key] = t;
    return t;
}

// bas opaque du sprite 80x80 (marges transparentes variables par espèce) —
// sert à poser les pieds exactement sur la plateforme de terrain
int World::poke_feet_m(int member, int frame) {
    static std::map<int, int> c;
    int key = member * 2 + (frame & 1);
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    int bot = 80;
    int species = member / 6;
    auto pg = _db->narc(Nds::P_POKEGRA);
    if (pg && species >= 0 && species * 6 + 5 < pg->count()) {
        Bytes raw = pg->member(member);
        if (raw.size() >= 0x30) {
            size_t p = 0x10;
            while (p + 8 <= raw.size()) {
                uint32_t tag = u32(raw, p), sz = u32(raw, p + 4);
                if ((tag == 0x43484152 || tag == 0x52414843) && sz >= 0x20) {
                    uint32_t dataSz = u32(raw, p + 8 + 16);
                    uint32_t dataOff = u32(raw, p + 8 + 20);
                    size_t base = p + 8 + dataOff;
                    if (!dataSz || base + dataSz > raw.size())
                        dataSz = (uint32_t)(raw.size() - base);
                    if (base < raw.size() && dataSz >= 6400) {
                        std::vector<uint8_t> px(raw.begin() + base,
                            raw.begin() + base + 6400);
                        pokegra_decrypt(px.data(), px.size());
                        int fx = (frame & 1) * 40;
                        for (int y = 79; y >= 0; y--) {
                            bool any = false;
                            for (int x = 0; x < 80; x++) {
                                int b = px[y * 80 + fx + x / 2];
                                if (((x & 1) ? (b >> 4) : (b & 15))) { any = true; break; }
                            }
                            if (any) { bot = y + 1; break; }
                        }
                    }
                    break;
                }
                if (sz < 8) break;
                p += sz;
            }
        }
    }
    c[key] = bot;
    return bot;
}

// ── rendu ─────────────────────────────────────────────────────────────────────
// presets caméra DS : pokeplatinum src/overlay005/field_camera.c (sCameraTypes)
// distance/pitch autour de la cible ; fov_idx = demi-angle stocké (persp → ×2)
struct CamPreset { float dist, pitch, vfov, zn, zf; bool ortho; };
static const CamPreset CAMT[17] = {
    {666.922f, 59.0515f,  8.0914f,  150.f,  900.f, false},  // 0  default
    {666.922f, 68.3679f,  8.0914f,  150.f,  900.f, false},  // 1  Pastoria gym
    {515.456f, 54.6570f, 10.4590f,  150.f,  900.f, false},  // 2  zoomed in
    {666.922f, 59.0515f,  8.0914f,  150.f,  900.f, false},  // 3  Canalave gym
    {1563.54f, 50.0867f,  3.5211f,  150.f, 1735.f, true },  // 4  intérieur ORTHO
    {316.501f, 59.0460f, 16.8805f,   10.f, 1008.f, false},  // 5  Spear Pillar
    {866.554f, 73.1085f,  6.3336f,  115.f, 1221.f, false},  // 6  Mt Couronné ext S
    {666.922f, 59.0460f,  8.0914f,  153.f, 1031.f, false},  // 7  Mt Couronné ext N
    {662.922f, 70.4718f,  9.8492f,  150.f, 1034.f, false},  // 8  Mt Abrupt salle 2
    {357.604f, 40.5890f, 15.0293f,  150.f,  900.f, false},  // 9  arène Charbourg
    {1202.36f, 60.8038f,  4.5758f,  150.f, 1746.f, false},  // 10 arène Unionpolis
    {675.833f, 57.8156f,  8.0914f,  230.f, 1127.f, false},  // 11 zoom léger
    {574.578f, 63.2648f,  9.4977f,  150.f,  900.f, false},  // 12 grotte
    {515.456f, 47.7960f, 10.4590f,  150.f,  900.f, false},  // 13 grotte Île de Fer
    {169.462f, 78.3765f, 29.5367f,   10.f, 1008.f, false},  // 14 Hall Originel
    {653.929f, 54.6570f,  8.3496f,  150.f,  900.f, false},  // 15 Lac Savoir
    {330.922f, 59.0515f, 15.4742f,  150.f,  900.f, false},  // 16 inutilisé
};

static const CamPreset& cam_preset_for(const Nds* db, int header) {
    const MapHeader* H = db ? db->header(header) : nullptr;
    int c = H ? H->cam : 0;
    if (c < 0 || c >= (int)(sizeof(CAMT) / sizeof(CAMT[0]))) c = 0;
    return CAMT[c];
}

Mat4 World::cam_proj(float asp) const {
    const CamPreset& p = cam_preset_for(_db, _header);
    if (getenv("FOV"))                             // override debug
        return Mat4::perspective((float)atof(getenv("FOV")), asp, 30.f, 8000.f);
    if (p.ortho)                                   // intérieur : projection ortho DS
        return Mat4::ortho(tanf(p.vfov * 0.01745329252f) * p.dist, asp, p.zn, p.zf);
    return Mat4::perspective(p.vfov * 2.f * 0.01745329252f, asp, p.zn, p.zf);
}

// arealight.narc : ASCII CR-séparé — par gabarit : 1 ligne endTime (sec/2)
// puis 4 lignes lampe (valid,r,g,b,x,y,z — dir fx16), 4 lignes diff/amb/
// spec/emis, 1 ligne séparatrice ; le tout premier bloc n'a pas d'endTime
// (implicite 0) ; terminé par "EOF" (lighting_set_*.json de la decomp)
void World::_alt_load(int member) {
    _alt.clear();
    auto an = _db->narc(Nds::P_AREALIGHT);
    if (!an || member >= an->count()) return;
    const Bytes& d = an->member(member);
    std::string s((const char*)d.data(), d.size());
    std::vector<std::string> lines;
    for (size_t b = 0; b <= s.size();) {
        size_t p = s.find('\r', b);
        std::string l = s.substr(b, p == std::string::npos ? s.size() - b : p - b);
        if (!l.empty() && l[0] == '\n') l.erase(0, 1);
        while (!l.empty() && (l.back() == '\n' || l.back() == ' ')) l.pop_back();
        if (l.rfind("EOF", 0) == 0) break;
        lines.push_back(l);
        if (p == std::string::npos) break;
        b = p + 1;
    }
    auto is_end = [](const std::string& l) {        // ligne "endTime" = entier seul
        if (l.empty() || !isdigit((unsigned char)l[0])) return false;
        for (char c : l) if (c != ',' && !isdigit((unsigned char)c) && c != ' ') return false;
        return true;
    };
    auto ints = [](const std::string& l, std::vector<int>& v) {
        v.clear(); size_t b = 0;
        while (b <= l.size()) {
            size_t c = l.find(',', b);
            v.push_back(atoi(l.substr(b, c == std::string::npos ? c : c - b).c_str()));
            if (c == std::string::npos) break; b = c + 1;
        }
    };
    std::vector<int> v;
    size_t i = 0;
    int pend = 0;                                    // 1er bloc : endTime implicite 0
    while (i < lines.size()) {
        if (is_end(lines[i])) { ints(lines[i], v); pend = v.empty() ? 0 : v[0]; i++; }
        if (i + 8 >= lines.size()) break;
        Alt a; a.end = pend; pend = 0; a.L.n = 4;
        for (int k = 0; k < 4; k++) {                // lampes : index = bit du masque
            ints(lines[i + k], v);                   // (invalide -> couleur 0, pas de contribution)
            if (v.size() >= 7 && v[0] == 1)
                for (int c = 0; c < 3; c++) {
                    a.L.col[k][c] = v[1 + c] / 31.f;
                    a.L.dir[k][c] = std::max(-1.f, std::min(1.f, v[4 + c] / 4096.f));
                }
        }
        auto rgb3 = [&](int k, float* o) {
            ints(lines[i + 4 + k], v);
            for (int c = 0; c < 3 && c < (int)v.size(); c++) o[c] = v[c] / 31.f;
        };
        rgb3(0, a.L.dif); rgb3(1, a.L.amb); rgb3(2, a.L.spc); rgb3(3, a.L.emi);
        i += 9;                                      // +1 ligne séparatrice
        _alt.push_back(a);
    }
}

void World::draw(const Mat4& proj, int sw, int sh) {
    // gabarit du moment : premier dont endTime > heure/2 (comme le champ DS)
    if (!_alt.empty()) {
        time_t tt = time(nullptr); struct tm* lt = localtime(&tt);
        int s2 = getenv("HOUR")
            ? atoi(getenv("HOUR")) * 1800
            : (lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec) / 2;
        const DsLight* p = &_alt[0].L;
        for (auto& a : _alt)
            if (a.end > s2) { p = &a.L; break; }
        rnd::set_light(p);
    }
    // position caméra DS : cible = position joueur, offset = dist × (sin/cos pitch)
    const CamPreset& cp = cam_preset_for(_db, _header);
    bool indoor = _cells.size() <= 1;
    float pr = cp.pitch * 0.01745329252f;
    float ch = getenv("CAMH") ? (float)atof(getenv("CAMH")) : cp.dist * sinf(pr);
    float cb = getenv("CAMB") ? (float)atof(getenv("CAMB")) : cp.dist * cosf(pr);
    Vec3 coff{0, ch, cb};
    // caméra DS (field_camera.c) : Y suit le sol avec 6 ticks de retard
    _cay[_cai] = _ppos.y; _cai = (_cai + 1) % 7;
    float cy = _cay[_cai];
    Vec3 eye{_ppos.x, cy + 10.f, _ppos.z};
    // perspective + intérieur : léger retrait sur mur (DS ne compense pas l'ortho)
    float t = (!cp.ortho && indoor) ? _cam_hit(eye, coff) : 1.f;
    Vec3 cam = eye + coff * std::max(0.25f, t * 0.9f);
    Mat4 view = Mat4::look_at(cam, Vec3{_ppos.x, cy + 8, _ppos.z}, {0, 0, -1});
    Mat4 mvp = proj.mul(view);
    rnd::set_edge_indoor(indoor);             // palette de contours DS
    rnd::scene_begin();                       // composite : FBO coul+id+depth
    // passe 1 : opaque
    const char* only = getenv("ONLY");
    for (auto& kv : _cells) {
        Mat4 cell = Mat4::ident();
        cell.m[12] = kv.second->org.x;
        cell.m[13] = kv.second->org.y;
        cell.m[14] = kv.second->org.z;
        Mat4 cm = mvp.mul(cell);
        for (auto& g : kv.second->meshes)
            if (g.blend >= 0.999f &&
                (!only || g.dbg_name.find(only) != std::string::npos))
                rnd::draw(g, cm);
    }
    for (auto& p : _props) {
        float cs = cosf(p.ry), sn = sinf(p.ry);
        Mat4 cell = Mat4::ident();
        cell.m[0] = cs * p.scale.x; cell.m[8] = sn * p.scale.z;
        cell.m[2] = -sn * p.scale.x; cell.m[10] = cs * p.scale.z;
        cell.m[5] = p.scale.y;
        cell.m[12] = p.pos.x; cell.m[13] = p.pos.y; cell.m[14] = p.pos.z;
        Mat4 cm = mvp.mul(cell);
        bool anim = !p.anm_cur.empty();        // porte animée : delta par nœud
        for (size_t i = 0; i < p.meshes.size(); i++) {
            auto& g = p.meshes[i];
            if (g.blend >= 0.999f &&
                (!only || g.dbg_name.find(only) != std::string::npos))
                rnd::draw(g, anim ? cm.mul(_prop_node_delta(p, (int)i)) : cm);
        }
    }
    // ombres DS sous les persos (avant les billboards, ov5_021F134C.c)
    if (_shadow_meshes.empty()) _load_shadow();
    if (!_shadow_meshes.empty()) {
        auto sh = [&](const Vec3& gp) {              // pos + offsets DS (fx32)
            Mat4 m = Mat4::ident();
            // DS : y -4 (modèle à +3) ; ici le sol vient du mesh rendu, pas
            // du BDHC -> plan posé à +0.25 pour ne pas passer sous le terrain
            m.m[12] = gp.x - 0.5f; m.m[13] = gp.y - 2.75f; m.m[14] = gp.z + 1.f;
            for (auto& g : _shadow_meshes) rnd::draw(g, mvp.mul(m));
        };
        if (!_phidden) {                             // sol, pas l'arc du saut
            float gy = _ground_y(_ppos, _ppos.y + 20.f);
            sh({_ppos.x, gy > -1e8f ? gy : _ppos.y, _ppos.z});
        }
        for (size_t i = 0; i < _objects.size(); i++) {
            auto& o = _objects[i];
            int g = _resolve_gfx(o.gfx);             // VAR_0..F -> var 0x401F+n
            if (g < 0 || g >= (int)sizeof(GFX_SHADOW) || !GFX_SHADOW[g]) continue;
            if (gfx3d_member(g) >= 0) continue;      // 3D fldeff : pas d'ombre
            if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
            if (_objpos[i].y < -1e8f) continue;      // pas encore posé au sol
            sh(_objpos[i]);
        }
    }
    // PNJ : billboards mmodel[gfx], frame = dir*4 (position/direction runtime)
    static bool objdbg = getenv("OBJDBG") != nullptr;
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        int g = _resolve_gfx(o.gfx);                  // VAR_0..F -> var 0x401F+n
        if (objdbg) printf("[objdraw] i=%zu id=%d gfx=%d->%d gone=%d hide=%d pos=%.0f,%.0f,%.0f tex=%u\n",
            i, o.id, o.gfx, g, _objgone.count((int)i) ? 1 : 0,
            _objhide.count((int)i) ? 1 : 0,
            _objpos[i].x, _objpos[i].y, _objpos[i].z,
            _mm_tex(g, 0));
        if (_objgone.count((int)i)) continue;              // RemovePeople/flag-spawn
        if (_objhide.count((int)i)) continue;              // SetInvisible
        Vec3 p = _objpos[i];
        if (p.y > 30.f || p.y < -1e8f) {                    // ancre au sol
            p.y = _ground_y({p.x, 60, p.z}, 60.f);
            if (p.y < -1e8f) p.y = _objects[i].y / 4096.f;  // secours : event.y (fx32)
        }
        if (gfx3d_member(g) >= 0) {                  // objet 3D fldeff (panneau, boîte)
            auto& ms = _obj_3d(g);
            // rotation : dir monde -> angle (modèle orienté sud/+Z par défaut)
            static const float RY[4] = {0.f, 3.14159265f, 1.5707963f, -1.5707963f};
            float ry = RY[_objd[i] & 3];
            float cs = cosf(ry), sn = sinf(ry);
            Mat4 m = Mat4::ident();
            m.m[0] = cs; m.m[2] = -sn; m.m[8] = sn; m.m[10] = cs;
            m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
            for (auto& g : ms) rnd::draw(g, mvp.mul(m));
            continue;
        }
        bool mv = _objw[i].dx || _objw[i].dz;
        static const int W2R[4] = {1, 0, 2, 3};        // dir monde -> ROM (N,S,O,E)
        int fr = W2R[_objd[i] & 3] * 4 + (mv ? 1 + ((int)_objw[i].phase % 2) : 0);
        // ov5_021EDEB4 : sprite tiré vers la caméra (draw seul) — assez fort
        // pour passer devant les rebords bas (tapis, lèvre d'escalier)
        _draw_billboard(_bb, _mm_tex(g, fr),
            {p.x, p.y + 1.f, p.z}, cam, mvp, 10.f);
        if (_objemote.count((int)i))                  // bulle "!" au-dessus
            _draw_billboard(_emote_bb, _emote_tex,
                {p.x, p.y + 31.f, p.z}, cam, mvp, 10.f);
    }
    // joueur : PLAYER_M = gfx 0 -> player_m.nsbtx (32 frames : marche 0-15, course 16-31)
    if (!_phidden) {                           // caché : passage de porte DS
        static const int W2R[4] = {1, 0, 2, 3};
        int pf = W2R[_pdir & 3] * 4 + (_panim > 0.f ? 1 + ((int)_panim % 2) : 0);
        if (_can_run && run_held && !_psurf) pf += 16;     // section course
        uint32_t pt = _psurf
            ? _mm_member_tex(_gender == 1 ? 160 : 159, pf >> 2)  // swimhiro(ine) : 4 frames (dir)
            : _mm_tex(0, pf);
        if (objdbg) printf("[pdraw] ppos=%.0f,%.0f,%.0f dir=%d fr=%d tex=%u\n",
            _ppos.x, _ppos.y, _ppos.z, _pdir, pf, pt);
        _draw_billboard(_bb, pt,
            {_ppos.x, _ppos.y + 1.f, _ppos.z}, cam, mvp, 10.f);
        if (_objemote.count(0xFF))
            _draw_billboard(_emote_bb, _emote_tex,
                {_ppos.x, _ppos.y + 31.f, _ppos.z}, cam, mvp, 10.f);
    }
    // rencontre sauvage : le Pokémon surgit de l'herbe devant le joueur
    if (_wild_t > 0.f && _wild_tex) {
        static const int FX[4] = {0, 0, -1, 1}, FZ[4] = {1, -1, 0, 0};
        int d = _pdir & 3;
        float rise = fminf(1.f, (2.0f - _wild_t) * 2.5f);      // montée 0->1
        float bounce = sinf(_wild_t * 22.f) * 2.f * (1.f - rise);
        Vec3 wp{_ppos.x + FX[d] * 18.f,
                _ppos.y + 14.f * rise + bounce,
                _ppos.z + FZ[d] * 18.f};
        _draw_billboard(_pkbb, _wild_tex, wp, cam, mvp);
    }
    // passe 2 : translucides (ombres, lueurs) — triés loin->près (peintre),
    // ordre stable même quand les cellules streament (unordered_map)
    static std::vector<const Cell*> sorted;
    sorted.clear();
    for (auto& kv : _cells) sorted.push_back(kv.second.get());
    std::sort(sorted.begin(), sorted.end(), [&](const Cell* a, const Cell* b) {
        float da = (a->org - cam).len(), db = (b->org - cam).len();
        return da > db;
    });
    for (auto* c : sorted) {
        Mat4 cell = Mat4::ident();
        cell.m[12] = c->org.x;
        cell.m[13] = c->org.y;
        cell.m[14] = c->org.z;
        Mat4 cm = mvp.mul(cell);
        for (auto& g : c->meshes)
            if (g.blend < 0.999f) rnd::draw(g, cm);
    }
    static std::vector<const PropInst*> psorted;
    psorted.clear();
    for (auto& p : _props) psorted.push_back(&p);
    std::sort(psorted.begin(), psorted.end(),
        [&](const PropInst* a, const PropInst* b) {
            float da = (a->pos - cam).len(), db = (b->pos - cam).len();
            return da > db;
        });
    for (auto* pp : psorted) {
        auto& p = *pp;
        float cs = cosf(p.ry), sn = sinf(p.ry);
        Mat4 cell = Mat4::ident();
        cell.m[0] = cs * p.scale.x; cell.m[8] = sn * p.scale.z;
        cell.m[2] = -sn * p.scale.x; cell.m[10] = cs * p.scale.z;
        cell.m[5] = p.scale.y;
        cell.m[12] = p.pos.x; cell.m[13] = p.pos.y; cell.m[14] = p.pos.z;
        Mat4 cm = mvp.mul(cell);
        bool anim = !p.anm_cur.empty();
        for (size_t i = 0; i < p.meshes.size(); i++)
            if (p.meshes[i].blend < 0.999f)
                rnd::draw(p.meshes[i],
                          anim ? cm.mul(_prop_node_delta(p, (int)i)) : cm);
    }
    rnd::composite(sw, sh);                   // edge marking -> écran/MSAA
}

// ── scène starter DS : ev_pokeselect.narc en vrai 3D (choose_starter_app.c) ──
// membres narc : 1 mallette (anim 0) ; 3,5,7 balles (anims 2,4,6) ; 9 socle ;
// 8 = balle sélectionnée. Caméra DS : dist 300, pitch -30°, fov 22, cible 0.
void World::draw_starter(float asp) {
    if (!_scn_ok) {
        auto n = _db->narc("/graphic/ev_pokeselect.narc");
        if (n) {
            auto load = [&](int mdl, int anm, Vec3 pos, float ry, Vec3 sc) {
                auto model = std::make_shared<bmd::Model>(
                    bmd::parse_bmd(n->member(mdl)));
                PropInst pi;
                pi.pos = pos; pi.ry = ry; pi.scale = sc; pi.mdl = model.get();
                for (auto& mg : model->meshes) {
                    std::vector<Vec3> ct; Vec3 o{0,0,0};
                    auto buf = _tris(mg, o, &ct);
                    bool alpha = false; int polyA = 31;
                    uint32_t t = _mat_tex(mg.matdef, model->textures, &alpha, &polyA);
                    GpuMesh g = rnd::upload(buf.data(), (int)buf.size()/rnd::VF, t);
                    g.alpha_cut = alpha;
                    g.blend = polyA < 31 ? std::max(polyA/31.f, 0.03f) : 1.f;
                    /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                    /*DS attr*/ g.lit = mg.matdef.poly & 0xF;
                    if (!t && (mg.matdef.dif & 0x8000))
                        g.tint = rgb555f(mg.matdef.dif & 0x7FFF);
                    pi.meshes.push_back(g);
                    pi.mkey.push_back(bmd::group_key(mg.mat, mg.node));
                    pi.minv.push_back(mg.xf.inverse());
                }
                if (anm >= 0) {
                    auto ait = _scn_anm.find(anm);
                    if (ait == _scn_anm.end())
                        ait = _scn_anm.emplace(anm,
                            bmd::parse_bca(n->member(anm))).first;
                    pi.anm = &ait->second;
                }
                _scn_mdl[(int)_scn.size()] = model;   // garder le modèle vivant
                _scn.push_back(std::move(pi));
            };
            load(8, -1, {0,0,0}, 0, {1,1,1});             // 0 mallette ouverte
            load(3, 2, {-44,-4,32}, 0, {1,1,1});          // 1 balle gauche
            load(5, 4, {0,-4,62},   0, {1,1,1});          // 2 balle centre
            load(7, 6, {38,-4,26},  0, {1,1,1});          // 3 balle droite
            load(9, -1, {0,-28,40}, 3.14159265f, {3.5f,1,3.5f}); // 4 socle
            if (verbose) {
                for (size_t i = 0; i < _scn.size(); i++) {
                    Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
                    auto& m = *_scn_mdl[(int)i];
                    for (auto& mg : m.meshes)
                        for (auto& pr : mg.prims) for (auto& v : pr.v) {
                            mn.x=std::min(mn.x,v.p.x); mn.y=std::min(mn.y,v.p.y);
                            mn.z=std::min(mn.z,v.p.z); mx.x=std::max(mx.x,v.p.x);
                            mx.y=std::max(mx.y,v.p.y); mx.z=std::max(mx.z,v.p.z);
                        }
                    printf("[starter] prop%d meshes=%d pos=%.0f,%.0f,%.0f bbox "
                        "%.1f..%.1f %.1f..%.1f %.1f..%.1f\n", (int)i,
                        (int)_scn[i].meshes.size(), _scn[i].pos.x, _scn[i].pos.y,
                        _scn[i].pos.z, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
                }
                printf("[starter] scn: %d props\n", (int)_scn.size());
            }
            if (verbose) printf("[starter] scn: %d props, meshes %d,%d,%d,%d,%d\n",
                (int)_scn.size(), (int)_scn[0].meshes.size(),
                (int)_scn[1].meshes.size(), (int)_scn[2].meshes.size(),
                (int)_scn[3].meshes.size(), (int)_scn[4].meshes.size());
        }
        _scn_ok = true;
    }
    if (_scn.empty()) return;

    // caméra DS : dist 300 pitch -30° ; à la confirmation zoom -> dist 200,
    // pitch -50°, cible z=36 (StartCameraMovement, ~6 frames). Lerp doux.
    _scn_cam += ((starter_phase ? 1.f : 0.f) - _scn_cam) * 0.2f;
    float pitch = (-30.f + -12.f * _scn_cam) * 0.01745329252f;
    float dist = 470.f - 100.f * _scn_cam;              // recul : valise entière
    float tz = 18.f + 24.f * _scn_cam;   // +z décale la scène vers le haut
    Vec3 tgt{0, 12.f, tz};               // cible remontée : balles visibles
    Vec3 eye{0, 12.f - sinf(pitch) * dist, tz + cosf(pitch) * dist};
    Mat4 view = Mat4::look_at(eye, tgt, {0, 1, 0});
    Mat4 proj = Mat4::perspective(22.f * 0.01745329252f, asp, 8.f, 4096.f);
    Mat4 mvp = proj.mul(view);

    // DS UpdateSelectedPokeballAnimation : seule la balle sous le curseur
    // boucle son anim (rebond) ; les autres sont figées à la frame 0.
    _scn_t += 1.f/60.f;
    for (size_t i = 0; i < _scn.size(); i++) {
        auto& p = _scn[i];
        if (!p.anm || !p.mdl) continue;
        bool sel = (i == 1 + (size_t)starter_sel);      // balles = props 1..3
        p.anm_frame += (1.f/60.f) * 30.f;               // 30 fps DS
        int f = sel ? (int)p.anm_frame % std::max(1, p.anm->nframe) : 0;
        if (!sel) p.anm_frame = 0;
        auto bones = p.mdl->bones;
        for (int ni : p.anm->nodes)
            if (ni >= 0 && ni < (int)bones.size())
                bones[ni] = bmd::bca_node(*p.anm, ni, f, p.mdl->bones[ni]);
        p.anm_cur = bmd::replay_xf(*p.mdl, bones);
    }

    // rendu : socle puis valise puis balles (opaque d'abord, alpha ensuite)
    auto drawp = [&](const PropInst& p, bool pass_alpha) {
        float cs = cosf(p.ry), sn = sinf(p.ry);
        Mat4 m = Mat4::ident();
        m.m[0]=cs*p.scale.x; m.m[8]=sn*p.scale.z; m.m[2]=-sn*p.scale.x;
        m.m[10]=cs*p.scale.z; m.m[5]=p.scale.y;
        m.m[12]=p.pos.x; m.m[13]=p.pos.y; m.m[14]=p.pos.z;
        Mat4 cm = mvp.mul(m);
        bool anim = !p.anm_cur.empty();
        for (size_t i = 0; i < p.meshes.size(); i++) {
            bool a = p.meshes[i].blend < 0.999f;
            if (a == pass_alpha)
                rnd::draw(p.meshes[i],
                    anim ? cm.mul(_prop_node_delta(p,(int)i)) : cm);
        }
    };
    for (auto& p : _scn) drawp(p, false);
    for (auto& p : _scn) drawp(p, true);

    // NDC des balles pour le curseur ImGui (positions DS + hauteur ~10)
    for (int i = 0; i < 3; i++) {
        Vec3 w = _scn[1 + i].pos; w.y += 10;
        float x = mvp.m[0]*w.x+mvp.m[4]*w.y+mvp.m[8]*w.z+mvp.m[12];
        float y = mvp.m[1]*w.x+mvp.m[5]*w.y+mvp.m[9]*w.z+mvp.m[13];
        float ww = mvp.m[3]*w.x+mvp.m[7]*w.y+mvp.m[11]*w.z+mvp.m[15];
        _scn_scr[i][0] = ww > 0 ? x/ww : 0;
        _scn_scr[i][1] = ww > 0 ? y/ww : 0;
    }
}

bool World::starter_ball_scr(int i, float* sx, float* sy) const {
    if (i < 0 || i > 2) return false;
    *sx = _scn_scr[i][0]; *sy = _scn_scr[i][1];
    return true;
}
