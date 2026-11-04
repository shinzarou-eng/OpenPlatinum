// world_render.cpp — meshes GL, textures, billboards, caméra, draw.
#include "world.h"
#include "world_internal.h"
#include "msg.h"
#include "spr.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <map>
#include "gfx_map.inc"
#include "gfx_names.inc"
#include "gfx_3d.inc"
#include "gfx_shadow.inc"
#include "gfx_berry.inc"

static int gfx3d_member(int gfx) {
    for (int i = 0; i < GFX_3D_N; i++)
        if (GFX_3D[i].gfx == gfx) return GFX_3D[i].member;
    return -1;
}

// l'objet rend-il quelque chose ? faux = invisible à l'écran — s'il bloque
// aussi (npc_at), c'est un mur invisible. Couvre : marqueur 8192, modèle
// fldeff absent du narc, texture sprite manquante.
bool World::obj_invisible(int i) {
    int g = _resolve_gfx(_objects[i].gfx);
    if (g == 8192) return true;                      // marqueur DS (portail, puits)
    if (gfx3d_member(g) >= 0) {                      // objet 3D fldeff
        auto it = _obj3d.find(g);
        if (it != _obj3d.end()) return it->second.empty();
        int mi = gfx3d_member(g);
        auto n = _db->narc("/data/mmodel/fldeff.narc");
        return !n || mi < 0 || mi >= n->count();
    }
    if (g == 230) return _obj_gira().empty();        // giratina origin
    if (g >= SKIN_BASE) return false;                // skin png : tient
    return _mm_tex(g, 0) == 0;                       // sprite : texture ?
}

// meshes d'un membre fldeff quelconque (variantes : sol baie sec/humide)
std::vector<GpuMesh>& World::_fld_mesh(int mi) {
    auto it = _fldm.find(mi);
    if (it != _fldm.end()) return it->second;
    auto& out = _fldm[mi];
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (!n || mi < 0 || mi >= n->count()) return out;
    return _fld_upload(bmd::parse_bmd(n->member(mi)), out);
}

// construit (et cache) les meshes GL d'un objet 3D fldeff
std::vector<GpuMesh>& World::_obj_3d(int gfx) {
    auto it = _obj3d.find(gfx);
    if (it != _obj3d.end()) return it->second;
    auto& out = _obj3d[gfx];
    int mi = gfx3d_member(gfx);
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (mi < 0 || !n || mi >= n->count()) return out;
    auto model = bmd::parse_bmd(n->member(mi));
    return _fld_upload(model, out);
}

// GIRATINA_ORIGIN (gfx 230) : vrai billboard-model DS — mmodel.narc
// membre 440 = giratina_origin.nsbmd (field_sprites.order), sa texture =
// membre 393 (giratina_origin.nsbtx, feuille de frames giratinaorigin.N).
// Le modèle n'embarque pas de texset : on injecte le TEX0 du nsbtx et on
// alimente le nom de matériau avec la frame .1 (posture debout).
std::vector<GpuMesh>& World::_obj_gira() {
    if (_gira3d_ok) return _gira3d;
    _gira3d_ok = true;
    auto n = _db->narc(Nds::P_MMODEL);
    if (!n || n->count() <= 440) {
        if (getenv("WARPDBG"))
            printf("[gira] mmodel.narc absent ou trop petit (%d membres)\n",
                n ? n->count() : -1);
        return _gira3d;
    }
    auto model = bmd::parse_bmd(n->member(440));
    if (model.textures.empty())
        model.textures = bmd::parse_tex0(n->member(393), 0);
    if (getenv("WARPDBG")) {
        int v = 0; for (auto& mg : model.meshes) for (auto& pr : mg.prims) v += (int)pr.v.size();
        printf("[gira] meshes=%d verts=%d texset=%d\n",
            (int)model.meshes.size(), v, (int)model.textures.size());
        for (auto& mg : model.meshes)
            printf("[gira]   mat=%s tex=%s prims=%d\n", mg.matdef.name.c_str(),
                mg.matdef.tex_name.c_str(), (int)mg.prims.size());
        for (auto& kv : model.textures)
            printf("[gira]   texent %s\n", kv.first.c_str());
    }
    for (auto& mg : model.meshes) {
        if (mg.matdef.tex_name.empty()) continue;
        if (model.textures.count(mg.matdef.tex_name)) continue;
        for (auto& kv : model.textures)
            if (kv.first.compare(0, mg.matdef.tex_name.size(),
                    mg.matdef.tex_name) == 0
                && kv.first[mg.matdef.tex_name.size()] == '.') {
                model.textures[mg.matdef.tex_name] = kv.second;
                break;
            }
    }
    return _fld_upload(std::move(model), _gira3d);
}

std::vector<GpuMesh>& World::_fld_upload(bmd::Model mdl, std::vector<GpuMesh>& out) {
    auto model = std::make_shared<bmd::Model>(std::move(mdl));
    for (auto& mg : model->meshes) {
        if (verbose) {
            Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            for (auto& pr : mg.prims) for (auto& v : pr.v) {
                mn.x = std::min(mn.x, v.p.x); mn.y = std::min(mn.y, v.p.y); mn.z = std::min(mn.z, v.p.z);
                mx.x = std::max(mx.x, v.p.x); mx.y = std::max(mx.y, v.p.y); mx.z = std::max(mx.z, v.p.z);
            }
            printf("[fldeff] mat=%s tex=%s verts=%d bbox %.1f..%.1f %.1f..%.1f %.1f..%.1f\n",
                mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                (int)mg.prims.size() ? (int)mg.prims[0].v.size() : 0,
                mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
            if (getenv("VERTDBG"))
                for (auto& pr : mg.prims)
                    for (auto& v : pr.v)
                        printf("  v (%.2f %.2f %.2f)\n", v.p.x, v.p.y, v.p.z);
        }
        std::vector<Vec3> ct; Vec3 o{0,0,0};
        auto buf = _tris(mg, o, &ct);
        bool alpha = false; int polyA = 31;
        // texset du modèle s'il en embarque un, sinon celui de la map —
        // les plateformes du Monde Distordu référencent criffp (texset map)
        auto& texs = model->textures.empty() ? _texset : model->textures;
        uint32_t t = _mat_tex(mg.matdef, texs, &alpha, &polyA);
        if (verbose && !t) {
            printf("[fldeff!tex] mat=%s want=%s have:",
                mg.matdef.name.c_str(), mg.matdef.tex_name.c_str());
            for (auto& kv : texs) printf(" %s", kv.first.c_str());
            printf("\n");
        }
        GpuMesh g = rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
        g.dbg_name = mg.matdef.name;          // mat pour les tracks PAT0
        g.alpha_cut = alpha;
        g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                /*DS attr*/ g.lit = mg.matdef.poly & 0xF;
                g.water = tex_is_water(mg.matdef.tex_name)
            || mg.matdef.name.rfind("c1_fun", 0) == 0;
                g.mark_water(buf);
                g.glow = mat_is_lamp(mg.matdef.name);
        if (!t && (mg.matdef.dif & 0x8000))
            g.tint = rgb555f(mg.matdef.dif & 0x7FFF);
        out.push_back(g);
    }
    return out;
}

// modèle fldeff gardé vivant : p.mdl sert à _pat_tex pour résoudre les
// textures des tracks PAT0 (fontaines Eterna : gym04_hunsui.*)
const bmd::Model* World::_fld_mdl(int mi) {
    auto it = _fldmdls.find(mi);
    if (it != _fldmdls.end()) return it->second.get();
    std::shared_ptr<bmd::Model> m;
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (n && mi >= 0 && mi < n->count())
        m = std::make_shared<bmd::Model>(bmd::parse_bmd(n->member(mi)));
    auto& r = _fldmdls[mi] = m;
    return r.get();
}

// PAT0 d'un membre fldeff (nsbtp) : les membres n'ont pas l'en-tête BTP0,
// juste un bloc PAT0 après un prologue de 12 o — on l'enrobe pour parse_btp0.
const std::vector<bmd::TexPat>* World::_fld_pat(int member) {
    auto it = _fldpats.find(member);
    if (it != _fldpats.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<bmd::TexPat> tp;
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (n && member >= 0 && member < n->count()) {
        const Bytes& e = n->member(member);
        size_t p = 0;
        while (p + 4 <= e.size() && memcmp(e.data() + p, "PAT0", 4)) p++;
        if (p + 4 <= e.size()) {
            Bytes d(0x14 + e.size() - p, 0);
            memcpy(d.data(), "BTP0", 4);
            d[0x0E] = 1;                                    // 1 bloc
            d[0x10] = 0x14;                                 // offset PAT0
            memcpy(d.data() + 0x14, e.data() + p, e.size() - p);
            tp = bmd::parse_btp0(d);
        }
    }
    auto& r = _fldpats[member] = std::move(tp);
    return r.empty() ? nullptr : &r;
}

// touffe d'herbe devant les pieds dans les hautes herbes (ov5_021F2EA4) :
// fldeff membre 83 = tall_grass.nsbmd, 89 = very_tall_grass.nsbmd ; quand le
// BMD n'embarque pas son texset, on pioche les nsbtx membres 0 et 8.
void World::_load_grass() {
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (!n) return;
    auto load = [&](int member, int texmem, std::vector<GpuMesh>& out) {
        if (member >= n->count()) return;
        auto model = std::make_shared<bmd::Model>(bmd::parse_bmd(n->member(member)));
        if (model->textures.empty() && texmem < n->count())
            model->textures = bmd::parse_tex0(n->member(texmem), 0);
        for (auto& mg : model->meshes) {
            std::vector<Vec3> ct; Vec3 o{0,0,0};
            auto buf = _tris(mg, o, &ct);
            if (verbose) {
                Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
                for (auto& pr : mg.prims) for (auto& v : pr.v) {
                    mn.x=std::min(mn.x,v.p.x); mn.y=std::min(mn.y,v.p.y); mn.z=std::min(mn.z,v.p.z);
                    mx.x=std::max(mx.x,v.p.x); mx.y=std::max(mx.y,v.p.y); mx.z=std::max(mx.z,v.p.z);
                }
                printf("[grass] mem=%d mat=%s tex=%s bbox %.1f..%.1f %.1f..%.1f %.1f..%.1f\n",
                    member, mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                    mn.x,mx.x, mn.y,mx.y, mn.z,mx.z);
            }
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
    };
    load(83, 0, _grass_meshes);     // hautes herbes (beh 0x02)
    load(89, 8, _vtall_meshes);     // très hautes herbes (beh 0x03)
}

// dessine la touffe à z_tuile+18 (DS : entre la caméra et les pieds du perso) —
// billboard caméra-facing comme le Billboard_Create de ov5_021F2D20.c ; un quad
// plat vertical serait écrasé sur le sol par la caméra haute au lieu de couvrir
// les pieds du perso.
void World::_grass_overlay(const Vec3& feet, int beh, const Vec3& cam,
    const Mat4& mvp) {
    const std::vector<GpuMesh>* ms =
        beh == 0x02 ? &_grass_meshes : beh == 0x03 ? &_vtall_meshes : nullptr;
    if (!ms || ms->empty()) return;
    float tx = floorf(feet.x / TILE), tz = floorf(feet.z / TILE);
    Vec3 at{(tx + 0.5f) * TILE, feet.y - 0.5f, tz * TILE + 18.f};
    Vec3 to{cam.x - at.x, cam.y - at.y, cam.z - at.z};
    float l = to.len();
    to = l > 1e-3f ? to * (1.f / l) : Vec3{0, 0, 1};
    Vec3 r{to.z, 0, -to.x};
    float rl = r.len();
    r = rl > 1e-3f ? r * (1.f / rl) : Vec3{1, 0, 0};
    Vec3 u = to.cross(r);
    Mat4 m;
    m.m[0] = r.x;  m.m[1] = r.y;  m.m[2]  = r.z;
    // brise légère : cisaillement du haut de la touffe, déphasé par tuile
    m.m[4] = u.x + sinf(_dbg_frames * 0.045f + (tx + tz) * 1.31f) * 0.11f
                + sinf(_dbg_frames * 0.19f + tx * 2.7f) * 0.02f;
    m.m[5] = u.y;  m.m[6]  = u.z;
    m.m[8] = to.x; m.m[9] = to.y; m.m[10] = to.z;
    m.m[12] = at.x; m.m[13] = at.y; m.m[14] = at.z;
    m.m[3] = m.m[7] = m.m[11] = 0.f; m.m[15] = 1.f;
    for (auto& g : *ms) rnd::draw(g, mvp.mul(m));
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
        _mat_tex(mg.matdef, model->textures, &alpha, &polyA);
        // la texture DS kage (16x16) est un disque à bord franc -> « rond »
        // dur à l'échelle HD. On lui substitue un dégradé radial doux :
        // même empreinte au sol, chute gaussienne au lieu du cercle net.
        static uint32_t stex = 0;
        if (!stex) {
            Image simg; simg.create(64, 64);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    float dx = (x - 31.5f) / 31.5f,
                          dy = (y - 31.5f) / 31.5f;
                    float r = sqrtf(dx * dx + dy * dy);
                    // deux zones : noyau d'occlusion serré sous les pieds +
                    // halo doux qui s'étale — vraie ombre de contact, pas
                    // un flou uniforme
                    float core = r >= 0.48f ? 0.f
                        : powf(1.f - r / 0.48f, 1.2f) * 0.72f;
                    float halo = r >= 1.f ? 0.f : powf(1.f - r * r, 2.1f) * 0.42f;
                    float a = fminf(1.f, core + halo);
                    simg.set(x, y, 8, 8, 16, (uint8_t)(a * 205));
                }
            stex = rnd::upload_tex(simg);
        }
        GpuMesh g = rnd::upload(buf.data(), (int)buf.size() / rnd::VF, stex);
        g.alpha_cut = false;
        g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                /*DS attr*/ g.lit = mg.matdef.poly & 0xF;
                g.water = tex_is_water(mg.matdef.tex_name)
            || mg.matdef.name.rfind("c1_fun", 0) == 0;
                g.mark_water(buf);
                g.glow = mat_is_lamp(mg.matdef.name);
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
    if (it == texs.end()) it = _ptexset.find(md.tex_name);
    if (it == _ptexset.end()) it = _texset.find(md.tex_name);
    if (it == _texset.end()) {
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
    if (getenv("DUMPTEX") && strstr(md.tex_name.c_str(), getenv("DUMPTEX"))) {
        char p[256]; snprintf(p, sizeof p, "../_shots/dump_%s.ppm", md.tex_name.c_str());
        FILE* fp = fopen(p, "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", img->w, img->h);
            for (size_t k = 0; k < (size_t)img->w * img->h; k++)
                fputc(img->px[k*4], fp), fputc(img->px[k*4+1], fp), fputc(img->px[k*4+2], fp);
            fclose(fp);
        }
        char pa[256]; snprintf(pa, sizeof pa, "../_shots/dump_%s_alpha.ppm", md.tex_name.c_str());
        FILE* fa = fopen(pa, "wb");
        if (fa) {
            fprintf(fa, "P6\n%d %d\n255\n", img->w, img->h);
            for (size_t k = 0; k < (size_t)img->w * img->h; k++) {
                uint8_t av = img->px[k*4+3];
                fputc(av, fa), fputc(av, fa), fputc(av, fa);
            }
            fclose(fa);
        }
    }
    auto tc = _texcache.find(img.get());
    if (tc != _texcache.end()) return tc->second.second;
    // nearest : les textures DS sont des atlas — le bilinéaire étale les
    // texels voisins dans les quads (fausse « lumière » sur mur/sol) et
    // floute le pixel art ; la réduction reste lissée par les mipmaps
    uint32_t t = rnd::upload_tex_nearest(*img);
    _texcache[img.get()] = {img, t};
    return t;
}

// BTP0/PAT0 : texture+palette par nom — texset du modèle s'il en embarque un,
// sinon celui de la zone (même règle que _mat_tex dans _inst_props).
uint32_t World::_pat_tex(const bmd::Model* mdl, const std::string& tn,
    const std::string& pn) {
    const auto& texs = mdl && !mdl->textures.empty() ? mdl->textures : _texset;
    auto it = texs.find(tn);
    if (it == texs.end()) {
        if (verbose) printf("    [pat] tex '%s' absente du texset\n", tn.c_str());
        return 0;
    }
    auto img = bmd::tex_image(const_cast<bmd::TexEnt&>(it->second), pn, false);
    if (!img) return 0;
    auto tc = _texcache.find(img.get());
    if (tc != _texcache.end()) return tc->second.second;
    uint32_t t = rnd::upload_tex_nearest(*img);
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
    a.tex = rnd::upload_tex_nearest(*a.frames[0]);
    a.cur = 0;
    if (getenv("DUMPTEX") && strstr(key.c_str(), getenv("DUMPTEX"))) {
        for (int i = 0; i < (int)a.frames.size(); i++) {
            Image& im = *a.frames[i];
            int amn = 255, amx = 0, a0 = 0;
            for (size_t k = 0; k < (size_t)im.w * im.h; k++) {
                int av = im.px[k*4+3];
                amn = std::min(amn, av); amx = std::max(amx, av);
                if (!av) a0++;
            }
            printf("[dumptex] %s f%d %dx%d alpha[%d..%d] transparents=%d/%d\n",
                key.c_str(), i, im.w, im.h, amn, amx, a0, im.w * im.h);
            char pa[256]; snprintf(pa, sizeof pa, "../_shots/dump_%s_a%d.ppm",
                key.substr(0, key.find('|')).c_str(), i);
            FILE* fa = fopen(pa, "wb");
            if (fa) {
                fprintf(fa, "P6\n%d %d\n255\n", im.w, im.h);
                for (size_t k = 0; k < (size_t)im.w * im.h; k++) {
                    uint8_t av = im.px[k*4+3];
                    fputc(av, fa), fputc(av, fa), fputc(av, fa);
                }
                fclose(fa);
            }
            char p[256]; snprintf(p, sizeof p, "../_shots/dump_%s_%d.ppm",
                key.substr(0, key.find('|')).c_str(), i);
            FILE* fp = fopen(p, "wb");
            if (fp) {
                Image& im = *a.frames[i];
                fprintf(fp, "P6\n%d %d\n255\n", im.w, im.h);
                for (size_t k = 0; k < (size_t)im.w * im.h; k++)
                    fputc(im.px[k*4], fp), fputc(im.px[k*4+1], fp), fputc(im.px[k*4+2], fp);
                fclose(fp);
            }
        }
    }
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

// recoloration du sprite joueur : teinte/saturation/luminosité appliquées
// aux couleurs de la palette du sprite (slots = parties : cheveux, haut,
// bas…). Tout coché -> les tons peau/désaturés sont préservés auto ;
// sélection partielle -> seules les couleurs cochées changent.
static void rgb2hsv(float r, float g, float b, float* hh, float* ss, float* vv) {
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    float h = 0;
    if (d > 1e-5f) {
        if (mx == r) h = 60.f * fmodf((g - b) / d, 6.f);
        else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
        else h = 60.f * ((r - g) / d + 4.f);
    }
    if (h < 0) h += 360.f;
    *hh = h; *ss = mx > 1e-5f ? d / mx : 0.f; *vv = mx;
}
static void hsv2rgb(float h, float s, float v, float* rr, float* gg, float* bb) {
    float c = v * s, x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f)),
          m = v - c, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    *rr = r + m; *gg = g + m; *bb = b + m;
}
// slots = couleurs opaques uniques du sprite, triées -> indices stables
// d'une frame à l'autre (même palette sur toute la feuille)
static int sprite_slots(const Image& im, uint32_t* cols, int maxc) {
    int n = 0;
    for (size_t i = 0; i + 3 < im.px.size() && n < maxc; i += 4) {
        if (im.px[i + 3] < 128) continue;
        uint32_t c = (uint32_t)im.px[i] | ((uint32_t)im.px[i + 1] << 8) |
                     ((uint32_t)im.px[i + 2] << 16);
        bool seen = false;
        for (int k = 0; k < n; k++) if (cols[k] == c) { seen = true; break; }
        if (!seen) cols[n++] = c;
    }
    std::sort(cols, cols + n);
    return n;
}
static void tint_apply(Image* im, const SkinTint& t) {
    uint32_t cols[32], map[32];
    int n = sprite_slots(*im, cols, 32);
    if (!n) return;
    uint32_t allm = n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
    bool all = (t.mask & allm) == allm;             // tout coché -> mode auto
    for (int i = 0; i < n; i++) {
        map[i] = cols[i];
        if (!(t.mask & (1u << i))) continue;
        float r = (cols[i] & 255) / 255.f, g = ((cols[i] >> 8) & 255) / 255.f,
              b = ((cols[i] >> 16) & 255) / 255.f;
        float h, s, v; rgb2hsv(r, g, b, &h, &s, &v);
        if (all && s < 0.18f) continue;             // peau/gris préservés
        h = fmodf(h + (float)t.hue + 720.f, 360.f);
        s = fminf(1.f, fmaxf(0.f, s * t.sat / 100.f));
        v = fminf(1.f, fmaxf(0.f, v * t.val / 100.f));
        hsv2rgb(h, s, v, &r, &g, &b);
        map[i] = (uint32_t)(r * 255.f + .5f) |
                 ((uint32_t)(g * 255.f + .5f) << 8) |
                 ((uint32_t)(b * 255.f + .5f) << 16);
    }
    for (size_t i = 0; i + 3 < im->px.size(); i += 4) {
        if (im->px[i + 3] < 128) continue;
        uint32_t c = (uint32_t)im->px[i] | ((uint32_t)im->px[i + 1] << 8) |
                     ((uint32_t)im->px[i + 2] << 16);
        for (int k = 0; k < n; k++)
            if (cols[k] == c) {
                uint32_t nc = map[k];
                if (nc != c) {
                    im->px[i] = (uint8_t)(nc & 255);
                    im->px[i + 1] = (uint8_t)((nc >> 8) & 255);
                    im->px[i + 2] = (uint8_t)((nc >> 16) & 255);
                }
                break;
            }
    }
}

// frame N de la BTX0 d'un membre mmodel (image partagée — copier avant retouche)
static std::shared_ptr<Image> mm_img(Nds* db, int member, int frame) {
    auto n = db->narc(Nds::P_MMODEL);
    if (!n || member < 0 || member >= n->count()) return nullptr;
    auto texs = bmd::parse_tex0(n->member(member), 0);
    for (auto& kv : texs) {
        auto dot = kv.first.rfind('.');
        if (dot != std::string::npos &&
            atoi(kv.first.c_str() + dot + 1) == frame + 1) {
            if (!kv.second.img)
                kv.second.img = bmd::tex_image(kv.second,
                    kv.second.pals.empty() ? "" : kv.second.pals.begin()->first);
            return kv.second.img;
        }
    }
    if (!texs.empty()) {
        auto& e = texs.begin()->second;
        if (!e.img)
            e.img = bmd::tex_image(e,
                e.pals.empty() ? "" : e.pals.begin()->first);
        return e.img;
    }
    return nullptr;
}
static int mm_member_of(const World* w, int gfx) {
    int member = (gfx >= 0 && gfx < GFX_TO_MEMBER_N) ? (int)GFX_TO_MEMBER[gfx] : gfx;
    if (gfx == 0 && w->gender() == 1) member = 91;    // joueuse : player_f.nsbtx
    return member < 0 ? gfx : member;
}

// sprite overworld : o.gfx (OBJ_EVENT_GFX) -> membre mmodel.narc via GFX_TO_MEMBER
// puis la frame N de la BTX0 (sheet 4 dir × marche). Fallback gfx direct si hors table.
uint32_t World::_mm_tex(int gfx, int frame) {
    static const SkinTint ID{};
    return _mm_tex(gfx, frame, ID);
}
uint32_t World::_mm_tex(int gfx, int frame, const SkinTint& t) {
    if (gfx >= SKIN_BASE) {                    // skin perso (PNG du dossier)
        ensure_skins();
        return _skinpng_tex(gfx - SKIN_BASE, frame, t);
    }
    if (gfx == 150 && rskin >= SKIN_BASE) {    // BARRY -> skin du rival choisie
        ensure_skins();
        int i = rskin - SKIN_BASE;
        uint32_t x = _skinpng_tex(i, frame, t);
        if (!x && frame >= 16) x = _skinpng_tex(i, frame - 16, t);
        if (!x && (frame & 3)) x = _skinpng_tex(i, frame & ~3, t);
        if (!x) x = _skinpng_tex(i, 0, t);
        if (x) return x;
    }
    return _mm_member_tex(mm_member_of(this, gfx), frame, t);
}

// skin joueur (gfx PNJ) : les feuilles PNJ n'ont pas toujours les 32 frames
// héros — repli : frame demandée -> marche (sans offset course) -> dir fixe -> 0
uint32_t World::_skin_tex(int pf) {
    SkinTint t = _tint();
    uint32_t x = _mm_tex(skin, pf, t);
    if (!x && pf >= 16) x = _mm_tex(skin, pf - 16, t);
    if (!x && (pf & 3)) x = _mm_tex(skin, pf & ~3, t);
    if (!x) x = _mm_tex(skin, 0, t);
    return x;
}

uint32_t World::sprite_preview(int gfx) {     // frame 4 : debout face sud
    return _mm_tex(gfx, 4);
}
bool World::sprite_dims(int gfx, int* w, int* h) {
    std::shared_ptr<Image> img;
    if (gfx >= SKIN_BASE) {                    // skin perso : feuille PNG
        ensure_skins();
        int i = gfx - SKIN_BASE;
        if (i >= 0 && i < (int)_skins.size() &&
            4 < (int)_skins[i].frames.size())
            img = _skins[i].frames[4];
    } else
        img = mm_img(_db, mm_member_of(this, gfx), 4);
    if (!img || !img->w) return false;
    *w = img->w; *h = img->h;
    return true;
}
uint32_t World::sprite_preview_t(int gfx, int hue, int sat, int val,
                                 uint32_t mask) {
    SkinTint t{hue, sat, val, mask};
    return _mm_tex(gfx, 4, t);
}
uint32_t World::sprite_frame_t(int gfx, int frame, int hue, int sat,
                               int val, uint32_t mask) {
    SkinTint t{hue, sat, val, mask};
    return _mm_tex(gfx, frame, t);
}
// feuille PNG d'une skin perso : frame -> image, replis comme _skin_tex
uint32_t World::_skinpng_tex(int i, int frame, const SkinTint& t) {
    int mk = i * 64 + (frame & 63);
    uint64_t tk = t.identity() ? 0
        : (uint64_t)t.mask | ((uint64_t)(t.hue & 0x1FF) << 32)
        | ((uint64_t)(t.sat & 0xFF) << 41) | ((uint64_t)(t.val & 0xFF) << 49);
    auto& inner = _skcache[mk];
    auto it = inner.find(tk);
    if (it != inner.end()) return it->second;
    uint32_t tex = 0;
    if (i >= 0 && i < (int)_skins.size() &&
        frame >= 0 && frame < (int)_skins[i].frames.size()) {
        auto& img = _skins[i].frames[frame];
        if (img) {
            if (t.identity()) tex = rnd::upload_tex_nearest(*img);
            else {
                Image cp = *img;               // la source reste intacte
                tint_apply(&cp, t);
                tex = rnd::upload_tex_nearest(cp);
            }
        }
    }
    inner[tk] = tex;
    return tex;
}

// sheet vélo dédiée d'une skin perso : skins/bike/<nom>.png, 24 frames 32×32
// dans l'ordre pl_cyclehero — utilisée telle quelle. 0 si absente (le caller
// retombe alors sur le sprite vélo DS officiel).
uint32_t World::_bike_skin_tex(int i, int pf, int bikeFrame) {
    SkinTint t = _tint();
    int mk = i * 2048 + (pf & 63) * 32 + (bikeFrame & 31);
    uint64_t tk = t.identity() ? 0
        : (uint64_t)t.mask | ((uint64_t)(t.hue & 0x1FF) << 32)
        | ((uint64_t)(t.sat & 0xFF) << 41) | ((uint64_t)(t.val & 0xFF) << 49);
    auto& inner = _bskcache[mk];
    auto it = inner.find(tk);
    if (it != inner.end()) return it->second;
    uint32_t tex = 0;
    ensure_skins();
    if (i >= 0 && i < (int)_skins.size() &&
        bikeFrame >= 0 && bikeFrame < (int)_skins[i].bike.size() &&
        _skins[i].bike[bikeFrame]) {
        Image cp = *_skins[i].bike[bikeFrame];
        if (!t.identity()) tint_apply(&cp, t);
        tex = rnd::upload_tex_nearest(cp);
    }
    inner[tk] = tex;
    return tex;
}

// couleurs (slots) du sprite choisi — pour la rangée de sélection couleur
std::vector<RGBAf> World::sprite_pal(int gfx) {
    if (gfx >= SKIN_BASE) {                    // skin perso : slots du PNG
        ensure_skins();
        int i = gfx - SKIN_BASE;
        std::vector<RGBAf> out;
        if (i >= 0 && i < (int)_skins.size()) {
            auto& fr = _skins[i].frames;
            std::shared_ptr<Image> img =
                (4 < (int)fr.size() && fr[4]) ? fr[4]
                : (!fr.empty() ? fr[0] : nullptr);
            if (img) {
                uint32_t cols[32];
                int n = sprite_slots(*img, cols, 32);
                out.reserve(n);
                for (int k = 0; k < n; k++)
                    out.push_back({(float)(cols[k] & 255),
                        (float)((cols[k] >> 8) & 255),
                        (float)((cols[k] >> 16) & 255), 255.f});
            }
        }
        return out;
    }
    int member = mm_member_of(this, gfx);
    auto it = _palcache.find(member);
    if (it != _palcache.end()) return it->second;
    std::vector<RGBAf> out;
    auto img = mm_img(_db, member, 4);
    if (img) {
        uint32_t cols[32];
        int n = sprite_slots(*img, cols, 32);
        out.reserve(n);
        for (int i = 0; i < n; i++)
            out.push_back({(float)(cols[i] & 255),
                (float)((cols[i] >> 8) & 255),
                (float)((cols[i] >> 16) & 255), 255.f});
    }
    _palcache[member] = out;
    return out;
}
const char* World::gfx_name(int gfx) {
    if (gfx >= SKIN_BASE) {
        ensure_skins();
        int i = gfx - SKIN_BASE;
        return (i >= 0 && i < (int)_skins.size())
            ? _skins[i].name.c_str() : "?";
    }
    return (gfx >= 0 && gfx < GFX_NAME_N) ? GFX_NAME[gfx] : "?";
}

// accès direct au membre mmodel.narc (sprites hors table : surf h=160 f=161)
uint32_t World::_mm_member_tex(int member, int frame) {
    static const SkinTint ID{};
    return _mm_member_tex(member, frame, ID);
}
uint32_t World::_mm_member_tex(int member, int frame, const SkinTint& t) {
    int mk = member * 64 + (frame & 63);
    uint64_t tk = t.identity() ? 0
        : (uint64_t)t.mask | ((uint64_t)(t.hue & 0x1FF) << 32)
        | ((uint64_t)(t.sat & 0xFF) << 41) | ((uint64_t)(t.val & 0xFF) << 49);
    auto& inner = _mmcache[mk];
    auto it = inner.find(tk);
    if (it != inner.end()) return it->second;
    uint32_t tex = 0;
    auto img = mm_img(_db, member, frame);
    if (img) {
        if (t.identity()) tex = rnd::upload_tex_nearest(*img);
        else {
            Image cp = *img;                       // la source reste intacte
            tint_apply(&cp, t);
            tex = rnd::upload_tex_nearest(cp);
        }
    }
    inner[tk] = tex;
    return tex;
}

// enfoncement visuel du sprite sur les tuiles profondes (neige/boue DS :
// le perso est dessiné plus bas selon le palier) — map_tile_behaviors.h
static float sink_y(int beh) {
    switch (beh) {
        case 161: return 3.f;   // SNOW_DEEP
        case 162: return 5.f;   // SNOW_DEEPER
        case 163: return 8.f;   // SNOW_DEEPEST
        case 164: return 2.f;   // MUD
        case 165: return 5.f;   // MUD_DEEP
        case 166: return 3.f;   // MUD_WITH_GRASS
        case 167: return 6.f;   // MUD_DEEP_WITH_GRASS
        case 168: return 1.5f;  // SNOW_SHALLOW
        default:  return 0.f;
    }
}
// vrai aussi pour l'herbe : pull réduit -> la touffe occlute les pieds
static float grass_pull(int beh) {
    return (beh == 0x02 || beh == 0x03) ? 2.f : 10.f;
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
    const Vec3& cam, const Mat4& mvp, float pull, float roll) {
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
    // Monde Distorsion : roll du sprite sur mur/plafond (rotAngles DS) —
    // rotation des axes droite/haut du billboard dans leur plan.
    if (roll != 0.f) {
        float c = cosf(roll), s = sinf(roll);
        Vec3 r2 = r * c + u * s;
        u = u * c - r * s;
        r = r2;
    }
    Mat4 m = Mat4::ident();
    m.m[0] = r.x;  m.m[1] = r.y;  m.m[2]  = r.z;
    m.m[4] = u.x;  m.m[5] = u.y;  m.m[6]  = u.z;
    m.m[8] = to.x; m.m[9] = to.y; m.m[10] = to.z;
    m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
    GpuMesh g = bb;
    g.tex = tex;
    rnd::draw(g, mvp.mul(m));
}

// ombre portée réelle : le sprite du perso est projeté sur le plan du sol
// (y = gy) le long de la direction du soleil — la silhouette suit la pose
// et l'anim de marche, et s'étire quand le soleil baisse.
void World::_sprite_shadow(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
    float gy, const Vec3& sd, const Vec3& cam, const Mat4& mvp) {
    Vec3 to{cam.x - pos.x, cam.y - pos.y, cam.z - pos.z};
    float l = to.len();
    to = l > 1e-3f ? to * (1.f / l) : Vec3{0, 0, 1};
    Vec3 r{to.z, 0, -to.x};
    float rl = r.len();
    r = rl > 1e-3f ? r * (1.f / rl) : Vec3{1, 0, 0};
    Vec3 u = to.cross(r);
    Mat4 m = Mat4::ident();
    m.m[0] = r.x;  m.m[1] = r.y;  m.m[2]  = r.z;
    m.m[4] = u.x;  m.m[5] = u.y;  m.m[6]  = u.z;
    m.m[8] = to.x; m.m[9] = to.y; m.m[10] = to.z;
    m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
    // cisaillement : v' = v - (v.y - gy) * sd/sd.y  -> plaque sur y = gy
    float kx = sd.x / sd.y, kz = sd.z / sd.y;
    float gyo = gy + 0.35f;
    Mat4 p;
    p.m[0] = 1; p.m[4] = -kx; p.m[8]  = 0; p.m[12] = kx * gyo;
    p.m[1] = 0; p.m[5] = 0;   p.m[9]  = 0; p.m[13] = gyo;
    p.m[2] = 0; p.m[6] = -kz; p.m[10] = 0; p.m[14] = kz * gyo;
    p.m[3] = 0; p.m[7] = 0;   p.m[11] = 0; p.m[15] = 1;
    GpuMesh g = bb;
    g.tex = tex;
    // ombre froide et légère : noir pur donnait une "personne noire" au sol
    g.tint = {0.04f, 0.05f, 0.10f, 0.30f};
    g.alpha_cut = false;                    // alpha du sprite = pochoir
    g.blend = 1.f;
    g.lit = 0;                              // non éclairé : reste sombre
    rnd::draw(g, mvp.mul(p).mul(m));
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
                            t = rnd::upload_tex_nearest(*img);
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

// sprites de formes alternatives : pl_otherpoke = mêmes NCGR chiffrés
// 160×80 que pl_pokegra ; palette passée en membre (membre pair NCLR).
uint32_t World::other_tex(int member, int pal_member, int frame) {
    int key = member * 2 + (frame & 1);
    auto it = _othertex.find(key);
    if (it != _othertex.end()) return it->second;
    uint32_t t = 0;
    auto op = _db->narc(Nds::P_OTHERPOKE);
    if (op && member >= 0 && member < op->count()) {
        Bytes raw = op->member(member);
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
                        auto pals = spr::parse_nclr(op->member(pal_member));
                        if (!pals.empty() && !pals[0].empty()) {
                            auto img = std::make_shared<Image>();
                            img->w = 80; img->h = 80;
                            img->px.resize(80 * 80 * 4, 0);
                            int fx = (frame & 1) * 40;
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
                            t = rnd::upload_tex_nearest(*img);
                        }
                    }
                    break;
                }
                if (sz < 8) break;
                p += sz;
            }
        }
    }
    _othertex[key] = t;
    return t;
}

// sprites NB (images/) retirés : les sprites DS de pl_pokegra sont
// utilisés partout (combat, équipe, dex, résumé) — le set images/
// venait du repo Prism et n'est plus scanné.

// bas opaque du sprite 80x80 (marges transparentes variables par espèce) —
// sert à poser les pieds exactement sur la plateforme de terrain
void World::poke_bbox_m(int member, int frame, int* top, int* bot) {
    static std::map<int, std::pair<int, int>> c;
    int key = member * 2 + (frame & 1);
    auto it = c.find(key);
    if (it != c.end()) { *top = it->second.first; *bot = it->second.second; return; }
    int tp = 0, bt = 80;
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
                        auto opaque = [&](int y) {
                            for (int x = 0; x < 80; x++) {
                                int b = px[y * 80 + fx + x / 2];
                                if (((x & 1) ? (b >> 4) : (b & 15))) return true;
                            }
                            return false;
                        };
                        for (int y = 79; y >= 0; y--)
                            if (opaque(y)) { bt = y + 1; break; }
                        for (int y = 0; y < 80; y++)
                            if (opaque(y)) { tp = y; break; }
                    }
                    break;
                }
                if (sz < 8) break;
                p += sz;
            }
        }
    }
    c[key] = {tp, bt};
    *top = tp; *bot = bt;
}

int World::poke_feet_m(int member, int frame) {
    int t = 0, b = 80;
    poke_bbox_m(member, frame, &t, &b);
    return b;
}

// ── rendu ─────────────────────────────────────────────────────────────────────
// presets caméra DS : pokeplatinum src/overlay005/field_camera.c (sCameraTypes)
// distance/pitch autour de la cible ; fov_idx = demi-angle stocké (persp → ×2)
struct CamPreset { float dist, pitch, vfov, zn, zf; bool ortho; };
static const CamPreset CAMT[17] = {
    {666.922f, 59.0515f,  8.0914f,  150.f,  900.f, false},  // 0  default
    {666.922f, 68.3679f,  8.0914f,  150.f,  900.f, false},  // 1  Pastoria gym
    {515.456f, 54.6570f, 10.4590f,  150.f,  900.f, false},  // 2  zoomed in
    {666.922f, 59.0515f,  8.0914f,  100.f, 1700.f, false},  // 3  Canalave gym (DS clip 100/1700)
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
    // mode cinéma : preset DS conservé, légèrement plus bas + recul +
    // fov un rien plus ouvert — « DS mais plus large », sans en faire trop.
    // les intérieurs ortho restent tels quels
    static CamPreset adj;
    adj = CAMT[c];
    if (rnd::cinema_on() && !adj.ortho) {
        adj.pitch = fmaxf(adj.pitch - 5.5f, 40.f);
        adj.dist  *= 1.14f;
        adj.vfov  *= 1.12f;
        adj.zf    = fmaxf(adj.zf, 1400.f);
    }
    return adj;
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
        if (getenv("ALTDBG"))
            printf("[alt] m=%d end=%d dif=(%.2f %.2f %.2f) amb=(%.2f %.2f %.2f)"
                   " emi=(%.2f %.2f %.2f) L0d=(%.2f %.2f %.2f) L0c=(%.2f %.2f %.2f)\n",
                member, a.end, a.L.dif[0], a.L.dif[1], a.L.dif[2],
                a.L.amb[0], a.L.amb[1], a.L.amb[2],
                a.L.emi[0], a.L.emi[1], a.L.emi[2],
                a.L.dir[0][0], a.L.dir[0][1], a.L.dir[0][2],
                a.L.col[0][0], a.L.col[0][1], a.L.col[0][2]);
        i += 9;                                      // +1 ligne séparatrice
        _alt.push_back(a);
    }
}

// regrade lampe par type météo — la palette DS reste, la lampe bouge.
// Partagé par les deux passes de cellules : un front se voit en traversant
// l'écran (cellule sous la pluie assombrie, celle d'à côté éclairée).
static void wgrade(int t, DsLight& L) {
    // NB : vCol = aCol*min(l,1) — en plein jour la lampe sature, donc la
    // teinte doit baisser emi+amb+dif+spc ensemble, pas seulement amb.
    float m = 1.f, warm = 0.f, cool = 0.f;
    switch (t) {
    case 1: m = 0.94f; break;                                       // couvert
    case 2: case 3: m = 0.84f; cool = 1.08f; break;                 // pluie
    case 4: m = 0.78f; cool = 1.08f; break;                         // orage
    case 5: case 6: case 7: m = 0.92f; cool = 1.06f; break;         // neige
    case 9: m = 0.76f; warm = 1.10f; break;                         // cendres
    case 10: m = 0.92f; warm = 1.06f; break;                        // sable chaud
    case 11: m = 0.88f; break;                                      // grêle
    case 14: case 15: m = 0.87f; break;                             // brume
    default: break;
    }
    for (int c = 0; c < 3; c++) {
        L.amb[c] *= m; L.dif[c] *= m; L.spc[c] *= m; L.emi[c] *= m;
    }
    if (cool) L.dif[2] = fminf(1.f, L.dif[2] * cool);
    if (warm) L.dif[0] = fminf(1.f, L.dif[0] * warm);
}

// matrice d'un prop : Ry·Rx de base ; quand rz est posé, Rz·(Ry·Rx) —
// ordre effectif de MTX_Rot33Angles(alpha,beta,gamma) sur DS
// (Concat33(a,b,out) = b·a) — vérifié géométriquement : le cône lampe
// (base large y=0, pointe y=64) doit orienter +Y à l'opposé du regard
// pour que la pointe retombe près du porteur, et seul Rz·Ry·Rx donne
// ce motif sur les 4 directions.
Mat4 World::_prop_mat(const PropInst& p) {
    Mat4 m = Mat4::ident();
    float cs = cosf(p.ry), sn = sinf(p.ry);
    float cr = cosf(p.rx), sr = sinf(p.rx);
    m.m[0] = cs * p.scale.x; m.m[8] = cr * sn * p.scale.z;
    m.m[2] = -sn * p.scale.x; m.m[10] = cr * cs * p.scale.z;
    m.m[4] = sr * sn * p.scale.y; m.m[6] = sr * cs * p.scale.y;
    m.m[5] = cr * p.scale.y; m.m[9] = -sr * p.scale.z;
    m.m[12] = p.pos.x; m.m[13] = p.pos.y; m.m[14] = p.pos.z;
    if (p.rz != 0.f) {                      // cône lampe : RotZ·(Ry·Rx)
        float cz = cosf(p.rz), sz = sinf(p.rz);
        for (int c = 0; c < 3; c++) {
            float x = m.m[c * 4], y = m.m[c * 4 + 1];
            m.m[c * 4]     = cz * x - sz * y;
            m.m[c * 4 + 1] = sz * x + cz * y;
        }
    }
    return m;
}

void World::draw(const Mat4& proj, int sw, int sh) {
    DsLight Ls;                                   // lampe du moment (alt)
    bool indoor = _cells.size() <= 1;
    const MapHeader* Hd = _db ? _db->header(_header) : nullptr;
    bool cave = Hd && (Hd->mtype == 3 || Hd->mtype == 6);  // grotte/sous-sol
    // gabarit du moment : premier dont endTime > heure/2 (comme le champ DS)
    if (!_alt.empty()) {
        time_t tt = time(nullptr); struct tm* lt = localtime(&tt);
        int s2 = dbg_hour >= 0 ? dbg_hour * 1800
            : getenv("HOUR") ? atoi(getenv("HOUR")) * 1800
            : (lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec) / 2;
        const DsLight* p = &_alt[0].L;
        for (auto& a : _alt)
            if (a.end > s2) { p = &a.L; break; }
        // saison : léger regrade (hiver plus froid et sombre, automne plus
        // chaud) — la palette DS reste, seule la lampe bouge
        Ls = *p;
        switch (_season()) {
        case 0: for (auto& c : Ls.amb) c *= 0.94f;
                Ls.dif[2] = fminf(1.f, Ls.dif[2] * 1.06f); break;   // hiver bleu
        case 3: Ls.dif[0] = fminf(1.f, Ls.dif[0] * 1.05f);
                Ls.dif[2] *= 0.92f; break;                           // automne or
        default: break;
        }
        // sol mouillé : assombrissement tant que la pluie n'a pas séché
        // (extérieur seulement : l'intérieur garde sa lumière propre)
        if (_wthr.wet > 0.01f && !_mtx.headers.empty()) {
            float m = 1.f - 0.10f * _wthr.wet;
            for (auto& c : Ls.amb) c *= m;
        }
        // neige accumulée : le sol renvoie la lumière (léger blanchiment
        // bleuté), fondu à la fonte — extérieur seulement
        if (_wthr.snow_depth > 0.01f && !_mtx.headers.empty()) {
            float b = 1.f + 0.09f * _wthr.snow_depth;
            for (auto& c : Ls.amb) c = fminf(1.f, c * b);
            Ls.dif[2] = fminf(1.f, Ls.dif[2] * (1.f + 0.05f * _wthr.snow_depth));
        }
        // éclair : la foudre illumine toute la scène un instant
        if (_wthr.flash > 0.f) {
            float b = 1.f + _wthr.flash * 1.7f;
            for (int c = 0; c < 3; c++) {
                Ls.amb[c] = fminf(1.f, Ls.amb[c] * b);
                Ls.dif[c] = fminf(1.f, Ls.dif[c] * b);
            }
        }
        // intérieur (hors grotte) : adoucit la lampe directionnelle — sur DS
        // les pièces sont éclairées à l'ambiante, sans fort gradient
        // latéral sur les murs en pignon
        if (indoor && !cave) {
            for (int c = 0; c < 3; c++) {
                float flat = 0.5f * (Ls.dif[c] + Ls.amb[c]);
                Ls.amb[c] = fminf(1.f, flat * 0.98f);
                Ls.dif[c] = flat * (getenv("DIFX")
                    ? (float)atof(getenv("DIFX")) : 0.58f);
            }
        }
        rnd::set_light(&Ls);
    }
    // position caméra DS : cible = position joueur (ou override caméra),
    // offset = dist × (sin/cos pitch)
    const CamPreset& cp = cam_preset_for(_db, _header);
    float pr = cp.pitch * 0.01745329252f;
    // caméra ~15% plus basse que le preset DS (léger recul pour garder le
    // cadrage) — angle plus proche du sol, plus immersif
    float ch = getenv("CAMH") ? (float)atof(getenv("CAMH"))
                             : cp.dist * sinf(pr) * 0.85f;
    float cb = getenv("CAMB") ? (float)atof(getenv("CAMB"))
                             : cp.dist * cosf(pr) * 1.03f;
    Vec3 coff{0, ch, cb};
    // caméra DS (field_camera.c) : Y suit le sol avec 6 ticks de retard
    Vec3 ctp = cam_pos();                        // override libre/objet sinon joueur
    _cay[_cai] = ctp.y; _cai = (_cai + 1) % 7;
    float cy = _cay[_cai];
    Vec3 cam, ltarget{ctp.x, cy + 8, ctp.z}, lup{0, 0, -1};
    if (_dw.on && _dw_cam(cam, ltarget, lup)) {
        // Monde Distorsion : caméra orbitale DS (angles u16 persistés,
        // transitions par templates de bounds+direction)
    } else {
        Vec3 eye{ctp.x, cy + 10.f, ctp.z};
        // perspective + intérieur : léger retrait sur mur (DS ne compense pas l'ortho)
        float t = (!cp.ortho && indoor) ? _cam_hit(eye, coff) : 1.f;
        cam = eye + coff * std::max(0.25f, t * 0.9f);
    }
    // orage : la caméra tremble avec l'éclair (wobble rapide, décroit avec lui)
    if (_wthr.flash > 0.05f) {
        cam.x += sinf(_wthr.t * 97.f) * _wthr.flash * 5.f;
        cam.y += cosf(_wthr.t * 113.f) * _wthr.flash * 3.5f;
    }
    Mat4 view = Mat4::look_at(cam, ltarget, lup);
    Mat4 mvp = proj.mul(view);
    // astre du moment : heure (même source que la lampe arealight) ->
    // jour/crépuscule + direction, partagé entre ombres allongées et post
    time_t htt = time(nullptr); struct tm* hlt = localtime(&htt);
    float hh = dbg_hour >= 0 ? (float)dbg_hour
        : getenv("HOUR") ? (float)atoi(getenv("HOUR"))
        : hlt->tm_hour + hlt->tm_min / 60.f + hlt->tm_sec / 3600.f;
    auto sunst = [](float a, float b, float x) {
        float t = (x - a) / (b - a);
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        return t * t * (3.f - 2.f * t);
    };
    float sun_day  = sunst(5.5f, 8.f, hh) - sunst(18.f, 21.f, hh);
    float sun_dusk = sunst(4.5f, 6.5f, hh) - sunst(9.f, 12.f, hh)
                   + sunst(16.5f, 18.5f, hh) - sunst(20.5f, 23.f, hh);
    float sa = (hh - 6.f) / 12.f * 3.14159265f;
    Vec3 sdir{cosf(sa), sinf(sa) * 0.85f + 0.05f, -0.42f};
    bool is_moon = sdir.y < 0.04f;
    if (is_moon) {
        float ma = (hh - 18.f) / 12.f * 3.14159265f;
        sdir = Vec3{cosf(ma) * -1.f, sinf(ma) * 0.6f + 0.2f, -0.42f};
    }
    sdir = sdir.normalized();
    // soleil bas -> ombres longues (0 en zénith, 1 au ras de l'horizon)
    float sunlow = fminf(1.f, fmaxf(0.f, 0.7f - sdir.y) / 0.5f);
    if (is_moon) sunlow *= 0.3f;
    rnd::set_edge_indoor(indoor);             // palette de contours DS
    // fenêtres/lampadaires allumés la nuit : les matériaux `light*`/`fenter*`
    // chauffent et s'illuminent quand le jour tombe (uGlow, teinte chaude)
    rnd::set_glow(indoor ? 0.45f : (1.f - sun_day) * 1.2f);
    rnd::set_water(_wthr.t, indoor ? 1.f : sun_day);
    rnd::scene_begin();                       // composite : FBO coul+id+depth
    // teinte météo par cellule : le type local (header de la cellule, ou
    // champ dyn échantillonné à son centre) regrade sa lampe — un front
    // se voit d'une cellule à l'autre avant d'atteindre le joueur
    auto cell_wthr = [&](const Cell* c) -> int {
        int wb = dbg_weather >= 0 ? dbg_weather
            : (c->hdr >= 0 && _db && _db->header(c->hdr)
                ? _db->header(c->hdr)->wthr : _wthr_base);
        int cb = _weather_base(wb);               // byte header -> type
        return dbg_weather_dyn
            ? weather_at(c->org.x, c->org.z, cb) : cb;
    };
    if (getenv("CDBG") && _dbg_frames == 90) {
        for (auto& kv : _cells)
            printf("[cdbg] cell %d,%d hdr=%d -> wthr %d\n",
                   kv.second->cx, kv.second->cy, kv.second->hdr,
                   cell_wthr(kv.second.get()));
    }
    // passe 1 : opaque
    const char* only = getenv("ONLY");
    for (auto& kv : _cells) {
        Mat4 cell = Mat4::ident();
        cell.m[12] = kv.second->org.x;
        cell.m[13] = kv.second->org.y;
        cell.m[14] = kv.second->org.z;
        Mat4 cm = mvp.mul(cell);
        if (!_alt.empty()) {
            DsLight Lc = Ls; wgrade(cell_wthr(kv.second.get()), Lc);
            rnd::set_light(&Lc);
        }
        for (auto& g : kv.second->meshes)
            if (g.blend >= 0.999f &&
                (!only || g.dbg_name.find(only) != std::string::npos))
                rnd::draw(g, cm);
    }
    // Monde Distorsion : étage inactif (nextID) rendu décalé sous l'actif
    for (auto& c : _dw.ina_cells) {
        Mat4 cell = Mat4::ident();
        cell.m[12] = c->org.x; cell.m[13] = c->org.y; cell.m[14] = c->org.z;
        Mat4 cm = mvp.mul(cell);
        if (!_alt.empty()) {
            DsLight Lc = Ls; wgrade(cell_wthr(c.get()), Lc);
            rnd::set_light(&Lc);
        }
        for (auto& g : c->meshes)
            if (g.blend >= 0.999f &&
                (!only || g.dbg_name.find(only) != std::string::npos))
                rnd::draw(g, cm);
    }
    if (!_alt.empty()) rnd::set_light(&Ls);       // props/persos : lampe globale
    for (auto& p : _props) {
        if (p.hidden || getenv("NOPROPS")) continue; // MapProp_SetHidden DS
        if (getenv("NODOOR") && (p.model == 67 || p.model == atoi(getenv("NODOOR")))) continue;
        if (getenv("ONLYMDL") && p.model != atoi(getenv("ONLYMDL"))) continue;
        static bool pl = getenv("PROPLIST") != nullptr;
        if (pl && p.pos.x > 1840 && p.pos.x < 1900 && p.pos.z > 14140) {
            pl = false;
            for (auto& q : _props)
                if (q.pos.x > 1840 && q.pos.x < 1900 && q.pos.z > 14140 && q.pos.z < 14200)
                    printf("[pnear] mdl=%d pos=%.0f,%.0f,%.0f meshes=%zu hid=%d\n",
                        q.model, q.pos.x, q.pos.y, q.pos.z, q.meshes.size(), (int)q.hidden);
        }
        Mat4 cell = _prop_mat(p);
        Mat4 cm = mvp.mul(cell);
        bool anim = !p.anm_cur.empty();        // porte animée : delta par nœud
        for (size_t i = 0; i < p.meshes.size(); i++) {
            auto& g = p.meshes[i];
            if (g.blend < 0.999f ||
                (only && g.dbg_name.find(only) == std::string::npos))
                continue;
            Mat4 mm = anim ? cm.mul(_prop_node_delta(p, (int)i)) : cm;
            rnd::draw(g, mm);
            if (!indoor && sdir.y > 0.12f) {
                // ombre portée : le mesh du prop aplati sur le sol le long
                // de -sdir (même cisaillement planaire que _sprite_shadow)
                float gy = _ground_y(p.pos, p.pos.y + 30.f);
                if (gy < -1e8f) gy = p.pos.y;
                float kx = sdir.x / sdir.y, kz = sdir.z / sdir.y,
                      gyo = gy + 0.3f;
                Mat4 pr;
                pr.m[0] = 1; pr.m[4] = -kx; pr.m[8]  = 0; pr.m[12] = kx * gyo;
                pr.m[1] = 0; pr.m[5] = 0;   pr.m[9]  = 0; pr.m[13] = gyo;
                pr.m[2] = 0; pr.m[6] = -kz; pr.m[10] = 0; pr.m[14] = kz * gyo;
                pr.m[3] = 0; pr.m[7] = 0;   pr.m[11] = 0; pr.m[15] = 1;
                GpuMesh s = g;
                s.tint = {0.04f, 0.05f, 0.10f, 0.26f};
                s.alpha_cut = false; s.lit = 0;
                rnd::draw(s, anim ? mvp.mul(pr).mul(cell)
                                      .mul(_prop_node_delta(p, (int)i))
                                  : mvp.mul(pr).mul(cell));
            }
        }
    }
    // ombres DS sous les persos (avant les billboards, ov5_021F134C.c)
    if (_shadow_meshes.empty()) _load_shadow();
    if (_grass_meshes.empty() && _vtall_meshes.empty()) _load_grass();
    if (!_shadow_meshes.empty()) {
        auto sh = [&](const Vec3& gp) {              // pos + offsets DS (fx32)
            Mat4 m = Mat4::ident();
            // DS : y -4 (modèle à +3) ; ici le sol vient du mesh rendu, pas
            // du BDHC -> plan posé à +0.25 pour ne pas passer sous le terrain
            m.m[12] = gp.x - 0.5f; m.m[13] = gp.y - 2.75f; m.m[14] = gp.z + 1.f;
            // quand la silhouette projetée est active, le blob ne sert plus
            // que de contact léger — alpha réduit pour éviter le double fond
            bool cast = !indoor && sdir.y > 0.12f;
            for (auto& g : _shadow_meshes) {
                GpuMesh s = g;
                if (cast) s.tint = {1.f, 1.f, 1.f, 0.55f};
                rnd::draw(s, mvp.mul(m));
            }
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
            // rotation : dir monde -> angle (modèle orienté sud/+Z par défaut)
            static const float RY[4] = {0.f, 3.14159265f, 1.5707963f, -1.5707963f};
            float ry = RY[_objd[i] & 3];
            float cs = cosf(ry), sn = sinf(ry);
            Mat4 m = Mat4::ident();
            m.m[0] = cs; m.m[2] = -sn; m.m[8] = sn; m.m[10] = cs;
            m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
            if (g == 100) {                          // parcelle à baies (état DS)
                _berry_setup();
                const BerryPatch& bp = _berry[o.d0 & 127];
                // sol : berry_patch_moist 75 / dry 76 / very_dry 77 (humidité)
                int soil = bp.moistRating == 0 ? 77
                    : bp.moistRating <= 50 ? 76 : 75;
                for (auto& gm : _fld_mesh(soil)) rnd::draw(gm, mvp.mul(m));
                // arbre : stade 2 germé (pousse 16x16), 3 pousse, 4 fleuri,
                // 5 fruit — fruit F -> pousse F-2, fleuri F-1 (triplets nsbtx)
                if (bp.stage >= 2 &&
                    o.d0 < (int)(sizeof(BERRY_FRUIT)/sizeof(int16_t))) {
                    int fr = ((int)(_flt_t * 1.5f)) & 1; // 2 frames .1/.2
                    if (bp.stage == 2) {                 // SPROUTED : germe
                        uint32_t st = _mm_member_tex(95, 0);
                        _draw_billboard(_bb16, st,
                            {p.x, p.y + 1.f, p.z}, cam, mvp, 6.f);
                    } else {
                        int fm = BERRY_FRUIT[o.d0];
                        int tm = bp.stage == 5 ? fm
                            : bp.stage == 4 ? fm - 1 : fm - 2;
                        _draw_billboard(_btree_bb, _mm_member_tex(tm, fr),
                            {p.x, p.y + 1.f, p.z}, cam, mvp, 8.f);
                    }
                }
                continue;
            }
            auto& ms = _obj_3d(g);
            for (auto& g : ms) rnd::draw(g, mvp.mul(m));
            continue;
        }
        bool mv = _objw[i].dx || _objw[i].dz ||
                  (i == _follower && _fstep);   // suiveur : file du joueur
        static const int W2R[4] = {1, 0, 2, 3};        // dir monde -> ROM (N,S,O,E)
        float ph = i == _follower ? _fanim : _objw[i].phase;
        int fr = W2R[_objd[i] & 3] * 4 + (mv ? 1 + ((int)ph % 2) : 0);
        // ov5_021EDEB4 : sprite tiré vers la caméra (draw seul) — assez fort
        // pour passer devant les rebords bas (tapis, lèvre d'escalier) ;
        // dans l'herbe (beh 2/3) le tirage tombe à ~2 pour que la touffe
        // occlute les pieds, comme la géométrie DS
        int ob = _tile_beh((int)floorf(p.x / TILE), (int)floorf(p.z / TILE));
        // gfx 8192 : objet DS « invisible » (marqueurs d'interaction — puits
        // B6F, portails Monde Distordu) : rien à dessiner, sinon quad blanc
        if (g == 8192) continue;
        // gfx 230 GIRATINA_ORIGIN : billboard-model DS (quad NSBMD orienté
        // face caméra) — taille réelle du modèle, pas le quad PNJ 20x28.
        if (g == 230 && !_obj_gira().empty()) {
            Vec3 to{cam.x - p.x, cam.y - p.y, cam.z - p.z};
            float tl = to.len();
            to = tl > 1e-3f ? to * (1.f / tl) : Vec3{0, 0, 1};
            Vec3 rr{to.z, 0, -to.x};
            float rl = rr.len();
            rr = rl > 1e-3f ? rr * (1.f / rl) : Vec3{1, 0, 0};
            Vec3 uu = to.cross(rr);
            Mat4 m = Mat4::ident();
            m.m[0] = rr.x; m.m[1] = rr.y; m.m[2]  = rr.z;
            m.m[4] = uu.x; m.m[5] = uu.y; m.m[6]  = uu.z;
            m.m[8] = to.x; m.m[9] = to.y; m.m[10] = to.z;
            m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
            auto dk = _objdark.find((int)i);
            float k = dk != _objdark.end() ? fminf(1.f, dk->second / 16.f) : 0.f;
            for (auto& gm : _obj_gira()) {
                GpuMesh gg = gm;
                if (k > 0.f) {
                    gg.tint_k = k;
                    gg.tint_to[0] = gg.tint_to[1] = gg.tint_to[2] = 0.f;
                }
                rnd::draw(gg, mvp.mul(m));
            }
            continue;
        }
        // sprites 16x16 DS (GENERIC_16x16) : balle objet 87, rocher Force 84,
        // rocher mousse 190, rocher glace 191 — moitie d'un perso, pas 20x28
        const GpuMesh& bbq = (g == 87 || g == 84 || g == 190 || g == 191)
            ? _bb16 : _bb;
        if (!indoor && sdir.y > 0.12f)
            _sprite_shadow(bbq, _mm_tex(g, fr),
                {p.x, p.y + 1.f - sink_y(ob), p.z}, p.y, sdir, cam, mvp);
        // paletteDarkness DS (arrivée Giratina) : mix vers noir
        auto dk = _objdark.find((int)i);
        if (dk != _objdark.end() && dk->second > 0.f) {
            GpuMesh db = bbq;
            db.tint_k = fminf(1.f, dk->second / 16.f);
            db.tint_to[0] = db.tint_to[1] = db.tint_to[2] = 0.f;
            _draw_billboard(db, _mm_tex(g, fr),
                {p.x, p.y + 1.f - sink_y(ob), p.z}, cam, mvp, grass_pull(ob));
        } else
            _draw_billboard(bbq, _mm_tex(g, fr),
                {p.x, p.y + 1.f - sink_y(ob), p.z}, cam, mvp, grass_pull(ob));
        if (ob == 0x02 || ob == 0x03)
            _grass_overlay({p.x, p.y - sink_y(ob), p.z}, ob, cam, mvp);
        if (_objemote.count((int)i))                  // bulle "!" au-dessus
            _draw_billboard(_emote_bb, _emote_tex,
                {p.x, p.y + 31.f, p.z}, cam, mvp, 10.f);
    }
    // joueur : PLAYER_M = gfx 0 -> player_m.nsbtx (32 frames : marche 0-15, course 16-31)
    if (!_phidden) {                           // caché : passage de porte DS
        static const int W2R[4] = {1, 0, 2, 3};
        // Monde Distorsion : facing dessiné = rotation de la direction
        // (mur O 90°/mur E 270°/plafond 180° via _pface + roll sprite)
        int fdir = (_dw.on && _dw.fp_kind > 0 && _pface >= 0)
            ? _pface : _pdir;
        static const float DWROLL[4] = {0.f, 90.f, 270.f, 180.f};
        // roll cumulatif DS : pendant un saut le sprite tourne en continu
        // (rotate j.rot/j.steps), au repos l'angle de la surface
        float prl = _dw.on
            ? ((_dw.roll_steps > 0 ? _dw.roll_cur
                : (_dw.fp_kind > 0 ? DWROLL[_dw.fp_kind & 3] : 0.f))
               * 3.14159265f / 180.f)
            : 0.f;
        int pf = W2R[fdir & 3] * 4 + (_panim > 0.f ? 1 + ((int)_panim % 2) : 0);
        if (_can_run && run_held && !_psurf) pf += 16;     // section course
        // vélo : pl_cyclehero M=92/F=93 — bike.bin mappe 4 textures par
        // direction (N 0-3, S 4-7, O 8-11, E 12-15) : base dir + pédalage 0-3
        // en mouvement ; au repos les anims "coast" DS {4,5,6,7} donnent les
        // textures 16,18 (N,S bouclées) et 20,21 (O,E fixes). Skin perso :
        // composite skin + vélo DS.
        static const int BIKE_IDLE[4] = {16, 18, 20, 21};
        int bf = _panim > 0.f ? W2R[fdir & 3] * 4 + (int)_panim % 4
                            : BIKE_IDLE[W2R[fdir & 3]];
        // skin perso à vélo : sheet dédiée skins/bike/<nom>.png si elle existe,
        // sinon sprite DS officiel (Lucas/Aurore — la ROM n'a que ceux-là).
        uint32_t bsk = 0;
        if (!_psurf && _pstate == 1 && skin >= 0)
            bsk = _bike_skin_tex(skin - SKIN_BASE,
                    _panim > 0.f ? pf | 16 : pf, bf);
        uint32_t pt = _psurf
            ? _mm_member_tex(_gender == 1 ? 160 : 159, pf >> 2, _tint())
            : _pstate == 1
                ? (bsk ? bsk
                       : _mm_member_tex(_gender == 1 ? 93 : 92, bf, _tint()))
                : (skin >= 0 ? _skin_tex(pf) : _mm_tex(0, pf, _tint()));
        int pb = _tile_beh((int)floorf(_ppos.x / TILE),
            (int)floorf(_ppos.z / TILE));
        if (objdbg) printf("[pdraw] ppos=%.0f,%.0f,%.0f dir=%d fr=%d tex=%u pb=%d\n",
            _ppos.x, _ppos.y, _ppos.z, _pdir, pf, pt, pb);
        // SPRITEDBG : trace la résolution du sprite joueur à chaque changement
        // (pf/bf/pt/état) — utile pour les soucis de frame/texture du rendu.
        static int sprdbg = getenv("SPRITEDBG") ? 1 : 0;
        static uint32_t last_pt = 0; static int last_pf = -1;
        if (sprdbg && (pt != last_pt || pf != last_pf)) {
            printf("[spritedbg] pf=%d bf=%d pt=%u pstate=%d skin=%d surf=%d panim=%.1f\n",
                   pf, bf, pt, _pstate, skin, _psurf, _panim);
            last_pt = pt; last_pf = pf;
        }
        if (!indoor && sdir.y > 0.12f) {
            float gy = _ground_y(_ppos, _ppos.y + 20.f);
            _sprite_shadow(_bb, pt,
                {_ppos.x, _ppos.y + 1.f - sink_y(pb), _ppos.z},
                gy > -1e8f ? gy : _ppos.y, sdir, cam, mvp);
        }
        _draw_billboard(_bb, pt,
            {_ppos.x, _ppos.y + 1.f - sink_y(pb), _ppos.z}, cam, mvp,
            grass_pull(pb), prl);
        if (pb == 0x02 || pb == 0x03)
            _grass_overlay({_ppos.x, _ppos.y - sink_y(pb), _ppos.z}, pb, cam, mvp);
        if (_objemote.count(0xFF))
            _draw_billboard(_emote_bb, _emote_tex,
                {_ppos.x, _ppos.y + 31.f, _ppos.z}, cam, mvp, 10.f);
        else if (_idle_bub > 0.f)                 // « … » joueur inactif
            _draw_billboard(_emote2_bb, _emote2_tex,
                {_ppos.x, _ppos.y + 31.f, _ppos.z}, cam, mvp, 10.f);
    }
    // Pokémon suiveur : billboard de sa colonne PNG (N,N,S,S,W,W,E,E)
    if (pokefol_shown()) {
        const FolSheet* fs = _fol_sheet(_pokefol_sp, _pokefol_var);
        if (fs && fs->tex[0]) {
            // dir monde -> base de la paire : S=2 N=0 O=4 E=6
            static const int FB[4] = {2, 0, 4, 6};
            int fr = FB[_pfdir & 3] +
                     (_pfstep ? ((int)_pfanim & 1) : 0);   // pas = 2e frame
            uint32_t key = ((uint32_t)fs->fw << 16) | (uint32_t)fs->fh;
            auto& q = _folbb[key];
            if (!q.vao) {                        // quad ancré pieds, ~0.75 unité/px
                float hw = fs->fw * 0.375f, hh = fs->fh * 0.75f;
                float v[] = {
                    -hw,0,0,  0,1,0, 1,1,1,1, 0,1,   hw,0,0,  0,1,0, 1,1,1,1, 1,1,
                    -hw,hh,0, 0,1,0, 1,1,1,1, 0,0,   hw,0,0,  0,1,0, 1,1,1,1, 1,1,
                     hw,hh,0, 0,1,0, 1,1,1,1, 1,0,  -hw,hh,0, 0,1,0, 1,1,1,1, 0,0,
                };
                q = rnd::upload(v, 6, 0);
                q.alpha_cut = true; q.lit = 1;
            }
            Vec3 fp = _pfpos;
            if (_pfjump) {                       // saut de joie (arc)
                float t = _pfjt / 0.4f;
                fp.y += sinf(t * 3.14159265f) * 8.f;
            }
            int fb2 = _tile_beh((int)floorf(fp.x / TILE),
                (int)floorf(fp.z / TILE));
            if (!indoor && sdir.y > 0.12f)
                _sprite_shadow(q, fs->tex[fr & 7],
                    {fp.x, fp.y + 1.f - sink_y(fb2), fp.z}, fp.y,
                    sdir, cam, mvp);
            _draw_billboard(q, fs->tex[fr & 7],
                {fp.x, fp.y + 1.f - sink_y(fb2), fp.z}, cam, mvp,
                grass_pull(fb2));
            if (fb2 == 0x02 || fb2 == 0x03)
                _grass_overlay({fp.x, fp.y - sink_y(fb2), fp.z}, fb2, cam, mvp);
            if (_pfemote > 0.f)
                _draw_billboard(_heart_bb, _heart_tex,
                    {fp.x, fp.y + fs->fh * 0.75f + 8.f, fp.z}, cam, mvp, 10.f);
        }
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
    if (_dw.on) _dw_sky_draw();           // ciel BG2 + nuages DS derrière
    for (auto& c : _dw.ina_cells) sorted.push_back(c.get());
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
        if (!_alt.empty()) {
            DsLight Lc = Ls; wgrade(cell_wthr(c), Lc);
            rnd::set_light(&Lc);
        }
        for (auto& g : c->meshes)
            if (g.blend < 0.999f) rnd::draw(g, cm);
    }
    if (!_alt.empty()) rnd::set_light(&Ls);
    static std::vector<const PropInst*> psorted;
    psorted.clear();
    for (auto& p : _props)
        if (!p.hidden) psorted.push_back(&p);
    std::sort(psorted.begin(), psorted.end(),
        [&](const PropInst* a, const PropInst* b) {
            float da = (a->pos - cam).len(), db = (b->pos - cam).len();
            return da > db;
        });
    for (auto* pp : psorted) {
        auto& p = *pp;
        Mat4 cm = mvp.mul(_prop_mat(p));
        bool anim = !p.anm_cur.empty();
        for (size_t i = 0; i < p.meshes.size(); i++)
            if (p.meshes[i].blend < 0.999f)
                rnd::draw(p.meshes[i],
                          anim ? cm.mul(_prop_node_delta(p, (int)i)) : cm);
    }
    _draw_weather(cam, ctp, mvp);             // météo : particules + voiles
    _draw_lamps(cam, ctp, mvp,                // halos des lampadaires
                indoor ? 0.35f : (1.f - sun_day));
    if (rnd::cinema_on()) {
        // uniforms cinéma : heure -> ciel/soleil, météo -> brouillard,
        // caméra -> focus DoF (heure/astre calculés plus haut, partagés
        // avec les ombres allongées)
        PostFx pf;
        pf.zn = cp.zn; pf.zf = cp.zf; pf.ortho = cp.ortho ? 1 : 0;
        pf.sky = indoor ? 0 : 1;
        pf.room = cave ? 2.f : indoor ? 1.f : 0.f;
        pf.focus = (cam - Vec3{ctp.x, cy + 8, ctp.z}).len();
        pf.day = sun_day; pf.dusk = sun_dusk;
        // intérieur : éclairage artificiel — pas de teinte nuit/crépuscule
        // (sinon les pièces se lavent en gris), ni d'ombre allongée
        if (indoor) { pf.day = 1.f; pf.dusk = 0.f; }
        if (is_moon) { pf.sun_col[0] = .72f; pf.sun_col[1] = .80f;
                       pf.sun_col[2] = 1.f; }
        Vec3 sp = ctp + sdir * (cp.zf * 0.9f);
        float cw2 = mvp.m[3] * sp.x + mvp.m[7] * sp.y
                  + mvp.m[11] * sp.z + mvp.m[15];
        if (cw2 > 0.f && sdir.y > 0.02f) {
            pf.sun_uv[0] = (mvp.m[0] * sp.x + mvp.m[4] * sp.y
                          + mvp.m[8] * sp.z + mvp.m[12]) / cw2 * .5f + .5f;
            pf.sun_uv[1] = (mvp.m[1] * sp.x + mvp.m[5] * sp.y
                          + mvp.m[9] * sp.z + mvp.m[13]) / cw2 * .5f + .5f;
            pf.sun_k = fminf(1.f, sdir.y * 2.4f);
            if (is_moon) pf.sun_k *= 0.5f;
        }
        // atmosphère : brouillard + teinte selon météo/moment de journée
        int wt = _wthr.type;
        pf.fog_amt = indoor ? 0.02f : 0.035f;
        switch (wt) {
        case 9:  pf.fog_amt = 0.38f; break;                       // cendres
        case 10: pf.fog_amt = 0.30f; break;                       // sable
        case 14: case 15: pf.fog_amt = 0.55f; break;              // brume
        case 2: case 3: case 4:
            pf.fog_amt = 0.16f + 0.12f * _wthr.intensity; break;  // pluie/orage
        case 5: case 6: case 7: case 8:
            pf.fog_amt = 0.22f + 0.10f * _wthr.intensity; break;  // neige
        default: break;
        }
        for (int c = 0; c < 3; c++)
            pf.fog_col[c] = .28f + pf.day * .5f;
        pf.fog_col[2] += .10f;                                    // légèrement bleuté
        // horizon de couchant : le voile lointain prend l'orange
        pf.fog_col[0] += pf.dusk * .30f;
        pf.fog_col[1] += pf.dusk * .05f;
        pf.fog_col[2] -= pf.dusk * .22f;
        if (wt == 10) { pf.fog_col[0] += .08f; pf.fog_col[2] -= .12f; }
        if (wt == 9)  for (int c = 0; c < 3; c++) pf.fog_col[c] *= .55f;
        if (wt >= 5 && wt <= 8) for (int c = 0; c < 3; c++) pf.fog_col[c] += .06f;
        // Canalave gym : FogManager DS = brouillard noir constant (le puits
        // s'assombrit en profondeur) — couleur noire, densité modérée.
        if (_gym.kind == 4) {
            pf.fog_amt = 0.14f;
            pf.fog_col[0] = pf.fog_col[1] = pf.fog_col[2] = 0.02f;
        }
        // Hearthome gym : obscurité DS = fog noir très dense (table densité
        // 109 ; 119 salle 2 ; 91 avec le badge RELIC) couleur (1,1,1)/31.
        if (_gym.kind == 5) {
            float dens = 109.f;
            if (_vm.badges & (1 << 4)) dens = 91.f;          // badge Relic
            else if (_header == 90) dens = 119.f;           // salle 2
            pf.fog_amt = 4.5f * dens / 109.f;
            pf.fog_col[0] = pf.fog_col[1] = pf.fog_col[2] = 0.03f;
        }
        // Monde Distordu : InitFog DS = brouillard noir (pente 6, offset
        // 0x7600, alpha 16, GX_FOGBLEND_COLOR_ALPHA) — le lointain perd
        // son alpha et laisse voir le ciel ; on le simule en fondant vers
        // la couleur du ciel, assombrie du même facteur que la palette DS.
        if (_dw.on) {
            float dk = _dw.sky_dark / 16.f;
            pf.fog_col[0] = (16.f / 255.f) * (1.f - dk) + (4.f / 31.f) * dk;
            pf.fog_col[1] = (32.f / 255.f) * (1.f - dk) + (4.f / 31.f) * dk;
            pf.fog_col[2] = (222.f / 255.f) * (1.f - dk) + (8.f / 31.f) * dk;
            pf.fog_amt = 1.1f;
            pf.sky = 0;    // le ciel DS de tw_arc_etc est déjà dessiné
        }
        pf.bloom = (indoor ? 0.5f : 0.35f) + (1.f - pf.day) * 0.35f;
        pf.exposure = 1.02f;
        pf.edge = 1.f;
        pf.dof = indoor ? 0.22f : 1.f;
        // rayons volumétriques : discrets en intérieur — pas de faisceau
        // marqué sur les murs (reste perceptible près des fenêtres)
        pf.rays = indoor ? 0.16f : 1.f;
        pf.vign = 0.32f;
        if (cave) {                       // souterrain : sombre, sec, sans
            pf.fog_amt = 0.008f;          // ciel — vignette plus marquée
            pf.bloom = 0.30f; pf.rays = 0.15f; pf.vign = 0.45f;
        }
        pf.grain = 0.3f;
        pf.time = _wthr.t;
        pf.wet = indoor ? 0.f : _wthr.wet;
        rnd::set_post(&pf);
    }
    // grotte sombre DS : météo 16 (DARK_FLASH) ou 29 -> tout noir sauf
    // un halo autour du joueur ; Flash efface la météo -> halo plein.
    // Hors bloc cinéma : l'effet s'applique aussi en mode DS.
    {   int wt = _wthr.type;
        if ((wt == 16 || wt == 29) && _wthr.intensity > 0.01f) {
            Vec3 pp{_ppos.x, _ppos.y + 10.f, _ppos.z};   // buste du perso
            float cw3 = mvp.m[3] * pp.x + mvp.m[7] * pp.y
                      + mvp.m[11] * pp.z + mvp.m[15];
            if (cw3 > 0.f)
                rnd::set_dark(
                    (mvp.m[0] * pp.x + mvp.m[4] * pp.y
                   + mvp.m[8] * pp.z + mvp.m[12]) / cw3 * .5f + .5f,
                    1.f - ((mvp.m[1] * pp.x + mvp.m[5] * pp.y
                   + mvp.m[9] * pp.z + mvp.m[13]) / cw3 * .5f + .5f),
                    0.9f * _wthr.intensity,
                    wt == 16 ? 0.17f : 0.24f);          // Revêche plus noir
            else rnd::set_dark(-10.f, -10.f, 0.f, 0.3f);
        } else rnd::set_dark(-10.f, -10.f, 0.f, 0.3f);
    }
    rnd::composite(sw, sh);                   // edge marking -> écran/MSAA
}

// ── minimap : rendu ortho top-down de la map dans le FBO dédié ────────────
// Nord en haut, joueur au centre ; géométrie seule (pas de billboards/météo).
uint32_t World::minimap_tex(int size, float half_world, Vec3 center) {
    uint32_t t = rnd::map_begin(size);
    if (!t) return 0;
    Vec3 c = center;
    Mat4 proj = Mat4::ortho(half_world, 1.f, 1.f, 1600.f);
    Mat4 view = Mat4::look_at({c.x, c.y + 700.f, c.z},
                              {c.x, c.y, c.z}, {0.f, 0.f, -1.f});
    Mat4 mvp = proj.mul(view);
    // lampe arealight de la map (1er gabarit) — sinon couleurs matière
    if (!_alt.empty()) rnd::set_light(&_alt[0].L);
    auto cellmat = [](const Cell* c2) {
        Mat4 cell = Mat4::ident();
        cell.m[12] = c2->org.x; cell.m[13] = c2->org.y; cell.m[14] = c2->org.z;
        return cell;
    };
    for (auto& kv : _cells) {
        Mat4 cm = mvp.mul(cellmat(kv.second.get()));
        for (auto& g : kv.second->meshes)
            if (g.blend >= 0.999f) rnd::draw(g, cm);
    }
    for (auto& c2 : _dw.ina_cells) {
        Mat4 cm = mvp.mul(cellmat(c2.get()));
        for (auto& g : c2->meshes)
            if (g.blend >= 0.999f) rnd::draw(g, cm);
    }
    for (auto& p : _props) {
        if (p.hidden) continue;
        Mat4 cm = mvp.mul(_prop_mat(p));
        for (auto& g : p.meshes)
            if (g.blend >= 0.999f) rnd::draw(g, cm);
    }
    // translucides après (eau, feuillages)
    for (auto& kv : _cells) {
        Mat4 cm = mvp.mul(cellmat(kv.second.get()));
        for (auto& g : kv.second->meshes)
            if (g.blend < 0.999f) rnd::draw(g, cm);
    }
    for (auto& p : _props) {
        if (p.hidden) continue;
        Mat4 cm = mvp.mul(_prop_mat(p));
        for (auto& g : p.meshes)
            if (g.blend < 0.999f) rnd::draw(g, cm);
    }
    rnd::map_end();
    return t;
}

// ── météo : particules déterministes autour de la caméra ────────────────────
// pas d'état par particule : position = f(seed, _wthr.t) ; un VBO dynamique
// rempli chaque frame (quads cylindriques -> tris, lit=0 = couleur exacte).
static float whash(int i, int k) {            // 0..1 déterministe
    unsigned x = (unsigned)(i * 2654435761u + k * 40503u);
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return (x & 0xFFFFFF) / 16777216.f;
}

// paramètres visuels par type météo (0 = rien à dessiner)
struct WSpec {
    int n = 0;
    float fall = 0.f, size = 2.f, stretch = 1.f, drift = 0.f, wob = 0.f;
    RGBAf col{1, 1, 1, 1};
    bool seasonal = false;              // feuilles/pétales : teinte variée
};
static WSpec wspec(int t) {
    WSpec s;
    switch (t) {
    case 2:  s.n = 220; s.fall = 330.f; s.size = 1.5f; s.stretch = 7.f;
             s.drift = -16.f; s.col = {0.60f, 0.68f, 0.90f, 0.55f}; break;
    case 3:  s.n = 300; s.fall = 360.f; s.size = 1.6f; s.stretch = 8.f;
             s.drift = -30.f; s.col = {0.58f, 0.66f, 0.88f, 0.65f}; break;
    case 4:  s.n = 300; s.fall = 360.f; s.size = 1.6f; s.stretch = 8.f;
             s.drift = -30.f; s.col = {0.58f, 0.66f, 0.88f, 0.65f}; break;
    case 5:  s.n = 180; s.fall = 24.f;  s.size = 2.4f; s.drift = 9.f;
             s.wob = 7.f;     s.col = {1.f, 1.f, 1.f, 0.9f}; break;
    case 6:  s.n = 260; s.fall = 34.f;  s.size = 2.3f; s.drift = 16.f;
             s.wob = 5.f;     s.col = {0.95f, 0.97f, 1.f, 0.9f}; break;
    case 7:  s.n = 320; s.fall = 60.f;  s.size = 2.0f; s.drift = 60.f;
             s.wob = 3.f;     s.col = {0.92f, 0.94f, 1.f, 0.85f}; break;
    case 9:  s.n = 150; s.fall = 15.f;  s.size = 1.9f; s.drift = 6.f;
             s.wob = 6.f;     s.col = {0.48f, 0.47f, 0.52f, 0.8f}; break;
    case 10: s.n = 240; s.fall = 7.f;   s.size = 1.7f; s.stretch = 9.f;
             s.drift = -150.f; s.col = {0.84f, 0.73f, 0.52f, 0.45f}; break;
    case 11: s.n = 200; s.fall = 280.f; s.size = 2.3f; s.stretch = 1.6f;
             s.drift = -20.f; s.col = {0.92f, 0.94f, 0.98f, 0.85f}; break;
    case 14: s.n = 8;   s.size = 150.f; s.drift = 7.f;
             s.col = {0.74f, 0.77f, 0.80f, 0.10f}; break;   // brume : voiles
    case 15: s.n = 12;  s.size = 170.f; s.drift = 5.f;
             s.col = {0.55f, 0.58f, 0.64f, 0.16f}; break;   // brume épaisse
    case 20: s.n = 55;  s.fall = 8.f;   s.size = 7.5f; s.drift = 7.f;
             s.wob = 10.f; s.seasonal = true;
             s.col = {0.88f, 0.55f, 0.24f, 0.85f}; break;   // feuilles (automne)
    case 21: s.n = 45;  s.fall = 6.5f;  s.size = 5.5f; s.drift = 5.5f;
             s.wob = 11.f; s.seasonal = true;
             s.col = {0.97f, 0.78f, 0.82f, 0.80f}; break;   // pétales (printemps)
    default: break;
    }
    return s;
}

// ── sprites saisonniers : accessoires de /graphic/imageclip.narc ─────────────
// Ces NCGR sont « mapping mode 1 » : bitmap plat tw*8 x th*8 px (4bpp),
// PAS des tuiles 8x8 — parse_ncgr/compose ne s'y prêtent pas, d'où ce
// décodeur direct. Palette = accessory.NCLR (membre 0).
namespace spr { Bytes unlz(const Bytes& d); }   // défini dans spr.cpp

static std::shared_ptr<Image> ncgr_flat(const Bytes& d0,
    const std::vector<RGBAf>& pal) {
    Bytes d = spr::unlz(d0);
    if (d.size() < 0x30) return nullptr;
    for (size_t p = 0x10; p + 8 <= d.size();) {
        uint32_t tag = u32(d, p), sz = u32(d, p + 4);
        if (tag == 0x43484152 || tag == 0x52414843) {     // RAHC/CHAR
            int w = (d[p + 8] | (d[p + 9] << 8)) * 8,
                h = (d[p + 10] | (d[p + 11] << 8)) * 8;
            uint32_t fmt = u32(d, p + 12), doff = u32(d, p + 28);
            size_t base = p + 8 + doff;
            auto img = std::make_shared<Image>();
            img->create(w, h);
            if (fmt != 3 || !w || !h) return img;         // 4bpp seulement
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w / 2; x++) {
                    size_t o = base + (size_t)y * (w / 2) + x;
                    if (o >= d.size()) return img;
                    uint8_t b = d[o];
                    for (int k = 0; k < 2; k++) {
                        int idx = k ? (b >> 4) : (b & 15);
                        if (!idx || idx >= (int)pal.size()) continue;
                        const RGBAf& c = pal[idx];   // rgb555 : échelle 0-255
                        img->set(x * 2 + k, y, (uint8_t)fminf(255.f, c.r),
                            (uint8_t)fminf(255.f, c.g),
                            (uint8_t)fminf(255.f, c.b), 255);
                    }
                }
            return img;
        }
        if (sz < 8) break;
        p += sz;
    }
    return nullptr;
}

// atlas 128x32 : [0] grande feuille 32x32, [1] petite feuille 32x32,
// [2] feuille fine 16x16, [3] pétale (fine + rampe rose), [4] fleur 16x16
uint32_t World::_seas_atlas() {
    if (_seas_tex) return _seas_tex;
    // fashion case : chemin localisé selon la langue de la ROM
    static const char* PATHS[] = {
        "/graphic/imageclip.narc",
        "/resource/eng/dress_up_graphic/imageclip.narc",
        "/resource/fra/dress_up_graphic/imageclip.narc",
        "/resource/jpn/dress_up_graphic/imageclip.narc",
        "/resource/ger/dress_up_graphic/imageclip.narc",
        "/resource/ita/dress_up_graphic/imageclip.narc",
        "/resource/spa/dress_up_graphic/imageclip.narc",
        "/resource/kor/dress_up_graphic/imageclip.narc",
    };
    std::shared_ptr<Narc> nc;
    for (const char* p : PATHS) {
        nc = _db->narc(p);
        if (nc && nc->count() >= 60) break;
        nc.reset();
    }
    if (!nc) return 0;
    auto pals = spr::parse_nclr(nc->member(0));
    if (pals.size() < 3) return 0;
    if (getenv("SEASDBG")) {
        fprintf(stderr, "[seas] pals=%d p0=", (int)pals.size());
        for (int i = 0; i < 8 && i < (int)pals[0].size(); i++)
            fprintf(stderr, "(%.2f,%.2f,%.2f)", pals[0][i].r,
                pals[0][i].g, pals[0][i].b);
        fprintf(stderr, "\n");
    }
    // rampe pétale : dérivée de la palette accessoire (tons ambrés -> roses)
    // (rgb555 : les RGBAf sont en échelle 0-255)
    std::vector<RGBAf> petal = pals[0];
    if (petal.size() >= 8) {
        petal[1] = {255, 230, 240, 255}; petal[2] = {250, 195, 220, 255};
        petal[3] = {238, 140, 180, 255}; petal[4] = {220, 110, 160, 255};
        petal[5] = {190,  80, 140, 255}; petal[6] = {160,  60, 120, 255};
        petal[7] = {255, 170, 190, 255};
    }
    struct Slot { int mem, pal; };
    const Slot slots[5] = {{28, 0}, {27, 0}, {29, 0}, {29, -1}, {52, 2}};
    Image at; at.create(128, 32);
    int x = 0;
    for (int i = 0; i < 5; i++) {
        const std::vector<RGBAf>& pal = slots[i].pal < 0 ? petal
                                      : pals[slots[i].pal];
        auto im = ncgr_flat(nc->member(slots[i].mem), pal);
        if (!im || !im->w) { _seas_tex = 0; return 0; }
        for (int yy = 0; yy < im->h; yy++)
            for (int xx = 0; xx < im->w; xx++) {
                size_t o = ((size_t)yy * im->w + xx) * 4;
                if (!im->px[o + 3]) continue;
                at.set(x + xx, yy, im->px[o], im->px[o + 1],
                    im->px[o + 2], im->px[o + 3]);
            }
        _seas_uv[i][0] = x / 128.f;         _seas_uv[i][2] = (x + im->w) / 128.f;
        _seas_uv[i][1] = 0.f;               _seas_uv[i][3] = im->h / 32.f;
        x += im->w;
    }
    _seas_tex = rnd::upload_tex_nearest(at);
    return _seas_tex;
}

// Halos custom désactivés : quel que soit l'angle (billboard face caméra,
// disque horizontal, flaque au sol), les quads se font trancher par la
// géométrie — murs, bordures, pyramide du lanternon — et la partie restante
// forme un carré lumineux. La lumière des lampadaires repose entièrement sur
// les matériaux DS : uGlow sur la vitre `o02b`/enseignes la nuit, décalcomanies
// au sol `*lamp*` rendues translucides. Zéro géométrie ajoutée, zéro artefact.
void World::_draw_lamps(const Vec3& cam, const Vec3& tgt, const Mat4& mvp,
    float nk) {
    if (getenv("LAMPDBG") && _dbg_frames == 120) {
        printf("[lamps] n=%d cam=%.1f,%.1f,%.1f tgt=%.1f,%.1f,%.1f\n",
            (int)_lamps.size(), cam.x, cam.y, cam.z, tgt.x, tgt.y, tgt.z);
        for (auto& p : _lamps)
            printf("  lamp %.0f,%.0f,%.0f\n", p.x, p.y, p.z);
    }
    (void)cam; (void)tgt; (void)mvp; (void)nk;
}

void World::_draw_weather(const Vec3& cam, const Vec3& tgt, const Mat4& mvp) {
    const Weather& w = _wthr;
    int base = dbg_weather >= 0 ? dbg_weather : _wthr_base;
    // fronts spatiaux (dyn) : chaque particule échantillonne le champ à SON
    // ancre monde -> le bord du front est visible à l'écran (pluie au nord,
    // clair au sud), avec fondu de densité près du seuil de bande. Hors dyn,
    // climats verrouillés ou intérieur : type global uniforme comme avant.
    bool spatial = dbg_weather_dyn && _weather_variable(base) &&
                   !_mtx.headers.empty();
    // particules saisonnières par ciel calme (types 0-1, extérieur) :
    // feuilles en automne, pétales au printemps — feature du mode dyn
    // seulement, et pas sur les climats verrouillés (désert, cendres,
    // brouillards natifs : base >7 -> rien de saisonnier n'a lieu d'être)
    int seas_t = 0;
    if (dbg_weather_dyn && w.type <= 1 && !_mtx.headers.empty() &&
        _weather_variable(base))
        seas_t = _season() == 3 ? 20 : _season() == 1 ? 21 : 0;
    // en spatial l'arrivée du front EST le fondu de bande — pas de garde
    // d'intensité globale (sinon la pluie au bord disparaît quand le joueur
    // est dans la zone claire)
    if (!spatial && w.intensity <= 0.01f && w.flash <= 0.f && !seas_t) return;
    WSpec g = seas_t ? wspec(seas_t) : wspec(w.type);
    const int MAXP = 340;                         // budget particules (dyn)
    // billboards cylindriques : « droite » écran = up-monde × vue horizontale
    Vec3 fw{tgt.x - cam.x, 0, tgt.z - cam.z};
    float fl = fw.len();
    fw = fl > 1e-3f ? fw * (1.f / fl) : Vec3{0, 0, -1};
    Vec3 rt{-fw.z, 0, fw.x};
    const float R = 165.f, H = 150.f;
    std::vector<float> buf;
    std::vector<float> buf_s;                       // saisonnières (texturées)
    uint32_t stex = _seas_atlas();
    int cnt = spatial ? MAXP : (seas_t ? g.n : (int)(g.n * w.intensity));
    if (cnt) buf.reserve((size_t)cnt * 6 * rnd::VF);
    for (int i = 0; i < cnt; i++) {
        float px = tgt.x + (whash(i, 1) * 2.f - 1.f) * R,
              pz = tgt.z + (whash(i, 2) * 2.f - 1.f) * R;
        float y0 = whash(i, 3), sp = 0.8f + 0.4f * whash(i, 4), py;
        WSpec s = g;
        int stype = seas_t;                         // type effectif (spatial)
        float fade = 1.f;
        bool pveil = w.type == 14 || w.type == 15;
        if (spatial) {                              // type local par ancre
            float v = _wnoise(px / CELL * 0.31f + pz / CELL * 0.27f +
                              w.front_t * 0.02f);
            int lt = _weather_season(_weather_band(v, base));
            s = wspec(lt);
            stype = lt;
            if (!s.n && lt <= 1) {                  // zones claires : saison
                // ~1/6 du budget -> densité comparable au chemin non-spatial
                int sp2 = whash(i, 16) < 0.17f
                    ? (_season() == 3 ? 20 : _season() == 1 ? 21 : 0) : 0;
                s = wspec(sp2);
                stype = sp2;
            }
            if (!s.n) continue;                     // clair/nuageux : rien
            pveil = lt == 14 || lt == 15;
            if (pveil && i > 24) continue;          // brume : patches limités
            fade = fminf(1.f, _band_margin(v, base) * 34.f);  // bord de front
            if (fade <= 0.01f) continue;
        }
        if (s.seasonal) {                           // ancre MONDE fixe : la
            const float CW = 48.f;                  // feuille reste à sa place
            int gx = (int)floorf(tgt.x / CW),       // pendant que le joueur
                gz = (int)floorf(tgt.z / CW);       // traverse la grille
            int sx = gx - 3 + (int)(whash(i, 1) * 7.f);
            int sz = gz - 3 + (int)(whash(i, 2) * 7.f);
            px = sx * CW + whash(i, 17) * CW;
            pz = sz * CW + whash(i, 18) * CW;
        }
        if (pveil) {                                // brume : voiles lents
            px += sinf(w.t * 0.05f + whash(i, 5) * 6.28f) * 60.f;
            pz += cosf(w.t * 0.04f + whash(i, 6) * 6.28f) * 60.f;
            py = tgt.y + 14.f + whash(i, 7) * 46.f;
        } else if (s.drift == -150.f) {             // sable : dérive horiz.
            px += fmodf(y0 * 2.f * R - w.t * s.drift * sp, 2.f * R) - R;
            py = tgt.y - 20.f + whash(i, 7) * 110.f;
        } else {                                    // chute + retour en haut
            py = tgt.y - 25.f + H - fmodf(y0 * H + w.t * s.fall * sp, H);
            px += w.t * s.drift * 0.06f * (whash(i, 5) - 0.2f); // vent léger
            if (s.wob) {
                px += sinf(w.t * 0.8f * sp + whash(i, 5) * 6.28f) * s.wob;
                pz += cosf(w.t * 0.6f * sp + whash(i, 6) * 6.28f) * s.wob * 0.6f;
            }
        }
        // diamond dust : paillettes à alpha pulsé (sinon couleur fixe)
        float a = s.col.a * fade;
        if (w.dust && whash(i, 9) > 0.55f)
            a *= 0.5f + 0.5f * sinf(w.t * 5.f + whash(i, 10) * 6.28f);
        // saisonnières : vrais sprites DS (imageclip.narc) — quad texturé
        // qui tournoie + bascule (la feuille passe de chant), teinte par
        // vertex color sur le sprite ambré -> dégradé orange/rouille
        float cr = s.col.r, cg = s.col.g, cb = s.col.b;
        if (s.seasonal) {
            if (stex) {
                // variante : automne = 3 feuilles, printemps = pétale/fleur
                float hv = whash(i, 13);
                int uv_i = stype == 20
                    ? (hv < 0.45f ? 0 : hv < 0.8f ? 1 : 2)
                    : (hv < 0.82f ? 3 : 4);
                const float* uv = _seas_uv[uv_i];
                // teinte : feuilles ambrées -> orange/rouille ; pétales/fleurs
                // restent claires (léger voile rosé)
                if (stype == 20) {
                    float h2 = whash(i, 11);
                    cr = 0.90f + 0.25f * h2;
                    cg = 0.62f + 0.30f * (1.f - h2);
                    cb = 0.45f + 0.20f * whash(i, 12);
                } else {
                    float h2 = whash(i, 11);
                    cr = 0.95f + 0.10f * h2;
                    cg = 0.85f + 0.15f * h2;
                    cb = 0.90f + 0.10f * (1.f - h2);
                }
                // rotation lente dans le plan écran + bascule de chant
                float ph = whash(i, 14) * 6.2832f;
                float ang = w.t * (0.30f + whash(i, 15) * 0.55f) * sp + ph;
                float ca = cosf(ang), sa = sinf(ang);
                float flip = 0.45f + 0.55f * fabsf(cosf(w.t * 0.8f * sp + ph));
                float hw = s.size * 0.5f * flip, hh = s.size * 0.5f;
                auto vtx2 = [&](float rx, float uy, float u, float v) {
                    float X = rx * ca - uy * sa, Y = rx * sa + uy * ca;
                    buf_s.insert(buf_s.end(), {
                        px + rt.x * X, py + Y, pz + rt.z * X,
                        0, 1, 0, cr, cg, cb, a, u, v});
                };
                vtx2(-hw, -hh, uv[0], uv[1]); vtx2(hw, -hh, uv[2], uv[1]);
                vtx2(hw, hh, uv[2], uv[3]);
                vtx2(-hw, -hh, uv[0], uv[1]); vtx2(hw, hh, uv[2], uv[3]);
                vtx2(-hw, hh, uv[0], uv[3]);
                continue;
            }
            float h = whash(i, 11);               // repli couleur sans texture
            cr *= 0.75f + 0.50f * h;
            cg *= 0.80f + 0.35f * (1.f - h);
            cb *= 0.80f + 0.30f * whash(i, 12);
        }
        float hw = s.size * 0.5f, hh = s.size * 0.5f * s.stretch;
        auto vtx = [&](float rx, float uy) {
            buf.insert(buf.end(), {
                px + rt.x * rx, py + uy, pz + rt.z * rx,
                0, 1, 0, cr, cg, cb, a, 0, 0});
        };
        vtx(-hw, -hh); vtx(hw, -hh); vtx(hw, hh);
        vtx(-hw, -hh); vtx(hw, hh);  vtx(-hw, hh);
    }
    if (!buf.empty()) {
        if (!_wthr_mesh.vao) {
            _wthr_mesh = rnd::upload_dyn(buf.data(),
                (int)buf.size() / rnd::VF, 0);
            _wthr_mesh.lit = 0;                     // couleur exacte
            _wthr_mesh.blend = 0.98f;               // <1 -> passe blend GL
        } else {
            rnd::update_dyn(_wthr_mesh, buf.data(),
                (int)buf.size() / rnd::VF);
        }
        rnd::draw(_wthr_mesh, mvp);
    }
    if (!buf_s.empty() && stex) {
        if (!_seas_mesh.vao) {
            _seas_mesh = rnd::upload_dyn(buf_s.data(),
                (int)buf_s.size() / rnd::VF, stex);
            _seas_mesh.lit = 0;
            _seas_mesh.blend = 0.98f;
        } else {
            _seas_mesh.tex = stex;
            rnd::update_dyn(_seas_mesh, buf_s.data(),
                (int)buf_s.size() / rnd::VF);
        }
        rnd::draw(_seas_mesh, mvp);
    }
    // orage : voile blanc géant face caméra, alpha = flash décroissant
    if (w.flash > 0.f) {
        if (!_wthr_flash.vao) {
            const float s = 900.f, h2 = 700.f;
            float v[] = {
                -s,-h2,0, 0,1,0, 1,1,1,1, 0,0,  s,-h2,0, 0,1,0, 1,1,1,1, 0,0,
                -s, h2,0, 0,1,0, 1,1,1,1, 0,0, -s,-h2,0, 0,1,0, 1,1,1,1, 0,0,
                 s, h2,0, 0,1,0, 1,1,1,1, 0,0,  s, h2,0, 0,1,0, 1,1,1,1, 0,0,
            };
            _wthr_flash = rnd::upload(v, 6, 0);
            _wthr_flash.lit = 0;
            _wthr_flash.blend = 0.55f;
        }
        _wthr_flash.tint = {1.f, 1.f, 1.f, w.flash};
        Vec3 f3{tgt.x - cam.x, tgt.y - cam.y, tgt.z - cam.z};
        float l3 = f3.len();
        f3 = l3 > 1e-3f ? f3 * (1.f / l3) : Vec3{0, 0, -1};
        Vec3 at{cam.x + f3.x * 300.f, cam.y + f3.y * 300.f,
                cam.z + f3.z * 300.f};
        _draw_billboard(_wthr_flash, 0, at, cam, mvp);
    }
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
        Mat4 cm = mvp.mul(_prop_mat(p));
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
