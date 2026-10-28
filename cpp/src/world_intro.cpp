// world_intro.cpp — assets d'intro DS : titre 3D (titledemo.narc = Giratina),
// écrans NSCR (logo, fonds, aides) et portraits RGCN (Sorbier, héros, rival).
#include "world.h"
#include "spr.h"
#include <map>

namespace {

// textures 2D décodées (paresse)
std::map<int, uint32_t> g_tex;
std::map<int, std::pair<int,int>> g_texsz;          // w,h px

// /resource/<lang>/... : langue de la ROM, repli sur les autres régions
std::string res_dir(Nds* db) {
    static const char* L[] = {"fra","eng","deu","ita","spa","jpn"};
    const std::string& code = db->game_code();
    char c = code.size() > 3 ? code[3] : 'F';
    const char* pref = c == 'F' ? "fra" : c == 'E' ? "eng" :
                       c == 'D' ? "deu" : c == 'I' ? "ita" :
                       c == 'S' ? "spa" : c == 'J' ? "jpn" : "fra";
    char buf[64];
    snprintf(buf, sizeof(buf), "/resource/%s/title/titledemo.narc", pref);
    if (db->exists(buf)) return pref;
    for (auto l : L) {
        snprintf(buf, sizeof(buf), "/resource/%s/title/titledemo.narc", l);
        if (db->exists(buf)) return l;
    }
    return "fra";
}

std::shared_ptr<Image> nscr_img(Narc& n, int si, int ci, int pi,
                                bool skip0 = false) {
    if (si >= n.count() || ci >= n.count() || pi >= n.count()) return nullptr;
    auto g = spr::parse_ncgr(n.member(ci));
    auto pals = spr::parse_nclr(n.member(pi));
    return spr::render_screen(n.member(si), g, pals, skip0);
}

std::shared_ptr<Image> portrait_img(Narc& n, int ci, int pi, int frame) {
    auto g = spr::parse_ncgr(n.member(ci));
    auto pals = spr::parse_nclr(n.member(pi));
    if (pals.empty()) return nullptr;
    // portraits intro : une frame = 8x16 tuiles (64x128)
    return spr::compose(g, frame * 128, 8, 16, pals[0]);
}

uint32_t put(int id, const std::shared_ptr<Image>& im) {
    if (!im || im->px.empty()) return 0;
    g_texsz[id] = {im->w, im->h};
    g_tex[id] = rnd::upload_tex(*im);
    return g_tex[id];
}

} // namespace

void World::intro_tex_size(int id, int* w, int* h) {
    auto it = g_texsz.find(id);
    if (it == g_texsz.end()) { *w = *h = 0; return; }
    *w = it->second.first; *h = it->second.second;
}

// ids : 0 gamefreak, 1 logo Pokémon, 2 fond dégradé, 3 grosse balle,
// 4 Sorbier, 5 Lucas, 6 Dawn, 7 René, 8 aides boutons, 9 aides XY,
// 10 cadre tactile, 11 fond "écran tactile", 12 vortex titre,
// 13 fond nommage, 14-17 grilles clavier pages 0-3
uint32_t World::intro_tex(int id) {
    auto it = g_tex.find(id);
    if (it != g_tex.end()) return it->second;
    uint32_t t = 0;
    if (id >= 13 && id <= 17) {                      // namein.narc (clavier)
        std::string p = "/resource/" + res_dir(_db) + "/nameinput/namein.narc";
        auto n = _db->narc(p);
        if (n) {
            if (id == 13) t = put(id, nscr_img(*n, 4, 2, 0));     // fond champ
            else t = put(id, nscr_img(*n, 6 + (id - 14), 2, 0, true)); // grille
        }
    } else if (id <= 1 || id == 12) {                // titledemo.narc
        std::string p = "/resource/" + res_dir(_db) + "/title/titledemo.narc";
        auto n = _db->narc(p);
        if (n) {
            if (id == 0) t = put(id, nscr_img(*n, 3, 5, 4));     // GAME FREAK
            else if (id == 1) t = put(id, nscr_img(*n, 10, 12, 8, true)); // logo VERSION
            else if (id == 12) t = put(id, nscr_img(*n, 27, 26, 6)); // fond vortex (bottom_screen_border)
        }
    } else {                                         // intro.narc
        auto n = _db->narc("/demo/intro/intro.narc");
        if (n) switch (id) {
        case 2:  t = put(id, nscr_img(*n, 30, 24, 27)); break;  // dégradé bleu
        case 3:  t = put(id, nscr_img(*n, 31, 24, 27)); break;  // balle
        case 4:  t = put(id, portrait_img(*n, 19, 20, _intro_frame)); break;
        case 5:  t = put(id, portrait_img(*n, 9 + _intro_pframe, 13, 0));  break; // Lucas
        case 6:  t = put(id, portrait_img(*n, 14 + _intro_pframe, 18, 0)); break;// Dawn
        case 7:  t = put(id, portrait_img(*n, 21, 22, 0)); break;// René
        case 8:  t = put(id, nscr_img(*n, 5, 0, 1));  break;      // boutons
        case 9:  t = put(id, nscr_img(*n, 6, 0, 1));  break;      // X/Y
        case 10: t = put(id, nscr_img(*n, 8, 0, 1));  break;      // cadre
        case 11: t = put(id, nscr_img(*n, 7, 0, 1));  break;      // tactile
        }
    }
    if (!t) g_tex[id] = 0;
    return t;
}

// portraits : frame alternative pour Sorbier (deux poses dans le RGCN)
void World::intro_set_frame(int f) {
    if (f != _intro_frame) {
        _intro_frame = f;
        g_tex.erase(4);                               // re-décode Sorbier
    }
}

// choix du genre : 4 poses par perso (membres consécutifs 9-12 / 14-17)
void World::intro_set_pframe(int f) {
    f &= 3;
    if (f != _intro_pframe) {
        _intro_pframe = f;
        g_tex.erase(5); g_tex.erase(6);               // re-décode Lucas/Dawn
    }
}

// ── icône du champ de saisie (namein.narc) : NANR 48=garçon (cells 45-47),
// 49=fille (cells 49-51), 51=rival (cells 55-57) ; sprite à la pos DS (24,8) ──
uint32_t World::namein_icon_tex(int kind, int frame) {
    static uint32_t c[3][4] = {};
    static int cw[3], ch[3];
    if (kind < 0 || kind > 2) return 0;
    static const int FIRST[3] = {45, 49, 55};
    static const int SEQ[4] = {0, 1, 2, 1};      // NANR : boucle 0,1,2,1
    frame = SEQ[frame & 3];
    if (!c[kind][frame]) {
        std::string p = "/resource/" + res_dir(_db) + "/nameinput/namein.narc";
        auto n = _db->narc(p);
        if (n) {
            auto g = spr::parse_ncgr(n->member(10));
            auto pals = spr::parse_nclr(n->member(1));
            auto cells = spr::parse_ncer(n->member(12));
            int ci = FIRST[kind] + frame;
            if (ci < (int)cells.size()) {
                auto im = spr::render_cell(g, cells[ci], pals);
                if (im) {
                    cw[kind] = im->w; ch[kind] = im->h;
                    c[kind][frame] = rnd::upload_tex(*im);
                }
            }
        }
    }
    namein_icon_sz[0] = cw[kind]; namein_icon_sz[1] = ch[kind];
    return c[kind][frame];
}

// ── écran titre : scène 3D Giratina (BMD0/BCA0 de titledemo.narc) ────────────
void World::draw_title(float asp) {
    // type local : PropInst est privé à World -> accessible ici seulement
    struct TitleScene {
        bool ok = false;
        std::vector<PropInst> props;
        std::map<int, std::shared_ptr<bmd::Model>> mdls;
        std::map<int, bmd::JntAnim> anms;
        float t = 0.f;
    };
    static TitleScene g_ts;
    if (!g_ts.ok) {
        g_ts.ok = true;
        std::string p = "/resource/" + res_dir(_db) + "/title/titledemo.narc";
        auto n = _db->narc(p);
        if (n) {
            auto load = [&](int mdl, int anm, Vec3 pos, float ry, Vec3 sc) {
                auto model = std::make_shared<bmd::Model>(
                    bmd::parse_bmd(n->member(mdl)));
                PropInst pi;
                pi.pos = pos; pi.ry = ry; pi.scale = sc; pi.mdl = model.get();
                for (auto& mg : model->meshes) {
                    // ignore le plan "monde distordu" géant ( décor de cutscene )
                    Vec3 mnv{1e9f,1e9f,1e9f}, mxv{-1e9f,-1e9f,-1e9f};
                    for (auto& pr : mg.prims) for (auto& v : pr.v) {
                        mnv.x=fminf(mnv.x,v.p.x); mnv.y=fminf(mnv.y,v.p.y);
                        mnv.z=fminf(mnv.z,v.p.z); mxv.x=fmaxf(mxv.x,v.p.x);
                        mxv.y=fmaxf(mxv.y,v.p.y); mxv.z=fmaxf(mxv.z,v.p.z);
                    }
                    if (mxv.x - mnv.x > 800.f || mxv.z - mnv.z > 800.f)
                        continue;
                    std::vector<Vec3> ct; Vec3 o{0,0,0};
                    auto buf = _tris(mg, o, &ct);
                    bool alpha = false; int polyA = 31;
                    uint32_t t = _mat_tex(mg.matdef, model->textures,
                                          &alpha, &polyA);
                    if (verbose) {
                        printf("[title] mesh mat=%s tex=%s -> id=%u dif=%06X poly=%08X verts=%zu bbox %.0f..%.0f %.0f..%.0f %.0f..%.0f\n",
                            mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                            t, mg.matdef.dif & 0xFFFFFF, mg.matdef.poly,
                            buf.size() / rnd::VF,
                            mnv.x,mxv.x, mnv.y,mxv.y, mnv.z,mxv.z);
                    }
                    GpuMesh g = rnd::upload(buf.data(),
                        (int)buf.size() / rnd::VF, t);
                    g.alpha_cut = alpha;
                    g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                /*DS attr*/ g.lit = mg.matdef.poly & 0xF;
                    if (!t && (mg.matdef.dif & 0x8000))
                        g.tint = rgb555f(mg.matdef.dif & 0x7FFF);
                    pi.meshes.push_back(g);
                    pi.mkey.push_back(bmd::group_key(mg.mat, mg.node));
                    pi.minv.push_back(mg.xf.inverse());
                }
                if (anm >= 0) {
                    auto ait = g_ts.anms.find(anm);
                    if (ait == g_ts.anms.end())
                        ait = g_ts.anms.emplace(anm,
                            bmd::parse_bca(n->member(anm))).first;
                    pi.anm = &ait->second;
                }
                g_ts.mdls[(int)g_ts.props.size()] = model;
                g_ts.props.push_back(std::move(pi));
            };
            load(1, 2, {0,0,0}, 0, {1,1,1});        // Giratina animé
            load(16, 18, {0,0,0}, 0, {1,1,1});      // portail (cutscene)
            load(19, 21, {0,0,0}, 0, {1,1,1});      // visage (cutscene)
            if (verbose)
                for (size_t i = 0; i < g_ts.props.size(); i++) {
                    Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
                    for (auto& mg : g_ts.mdls[(int)i]->meshes)
                        for (auto& pr : mg.prims) for (auto& v : pr.v) {
                            mn.x=std::min(mn.x,v.p.x); mn.y=std::min(mn.y,v.p.y);
                            mn.z=std::min(mn.z,v.p.z); mx.x=std::max(mx.x,v.p.x);
                            mx.y=std::max(mx.y,v.p.y); mx.z=std::max(mx.z,v.p.z);
                        }
                    printf("[title] prop%d meshes=%d bbox %.0f..%.0f %.0f..%.0f %.0f..%.0f\n",
                        (int)i, (int)g_ts.props[i].meshes.size(),
                        mn.x,mx.x, mn.y,mx.y, mn.z,mx.z);
                }
        }
    }
    g_ts.t += 1.f / 60.f;
    if (verbose && !g_ts.props.empty() && g_ts.props[0].anm) {
        static bool lp = false;
        if (!lp) {
            lp = true;
            printf("[title] giratina anim nframe=%d nodes=%zu\n",
                g_ts.props[0].anm->nframe, g_ts.props[0].anm->nodes.size());
        }
    }
    // caméra : drift DS ×1.55 autour du centre animé de Giratina (-100,88,57)
    float f = fminf(1.f, g_ts.t / 1.f);
    Vec3 tgt{-100.f, 88.f, 57.f};
    Vec3 eye{tgt.x - 99.f * f, tgt.y + 143.f, tgt.z + 958.f - 180.f * f};
    Mat4 view = Mat4::look_at(eye, tgt, {0, 1, 0});
    Mat4 proj = Mat4::perspective(16.f * 0.01745329252f, asp, 8.f, 8192.f);
    Mat4 mvp = proj.mul(view);
    // lampes du titre DS : light0 = direction caméra->cible (decomp), light1
    // directionnelle blanche fixe, ambiante 0.30
    static DsLight TL;
    static bool tl_set = false;
    if (!tl_set) {
        tl_set = true;
        TL.n = 2;
        float lx = 0.f - (-64.f), ly = 100.f - 192.f, lz = -18.f - 484.f;
        float ln = sqrtf(lx*lx + ly*ly + lz*lz);
        TL.dir[0][0] = lx / ln; TL.dir[0][1] = ly / ln; TL.dir[0][2] = lz / ln;
        TL.dir[1][0] = -0.5534f; TL.dir[1][1] = 0.4768f; TL.dir[1][2] = -0.6828f;
        TL.col[0][0] = TL.col[0][1] = TL.col[0][2] = 1.f;
        TL.col[1][0] = TL.col[1][1] = TL.col[1][2] = 1.f;
        TL.dif[0] = TL.dif[1] = TL.dif[2] = 1.f;
        TL.amb[0] = TL.amb[1] = TL.amb[2] = 0.30f;
    }
    rnd::set_light(&TL);
    rnd::set_clear(0.f, 0.f, 0.f);          // fond noir DS derrière la scène
    // fond 2D du titre DS : image "bottom_screen_border" (vortex) derrière la 3D
    static GpuMesh g_bgq; static bool bgq_init = false;
    if (!bgq_init) {
        bgq_init = true;
        uint32_t btex = intro_tex(12);
        float v[6 * rnd::VF]; float hvv = 0.f;            // moitié hauteur
        auto putv = [&](int i, float x, float y, float z, float u, float vv) {
            float* p = v + i * rnd::VF;
            p[0]=x; p[1]=y; p[2]=z; p[3]=0; p[4]=0; p[5]=-1;
            p[6]=1; p[7]=1; p[8]=1; p[9]=1; p[10]=u; p[11]=vv;
            (void)hvv;
        };
        const float D = 3000.f, HH = 430.f, HW = 780.f;   // quad face caméra
        // base orthonormée caméra (dir ≈ eye->tgt ≈ (0.1,-0.15,-0.98))
        Vec3 d{0.1245f, -0.1789f, -0.9761f}, r{0.9922f, 0, -0.1245f},
             u{0.0223f, 0.9992f, -0.0028f};
        Vec3 e{-64.f, 192.f, 484.f};
        Vec3 c{e.x + d.x * D, e.y + d.y * D, e.z + d.z * D};
        auto cn = [&](float sx, float sy) {
            return Vec3{c.x + r.x * sx * HW + u.x * sy * HH,
                        c.y + r.y * sx * HW + u.y * sy * HH,
                        c.z + r.z * sx * HW + u.z * sy * HH};
        };
        Vec3 p0 = cn(-1,-1), p1 = cn(1,-1), p2 = cn(1,1), p3 = cn(-1,1);
        const float VT = 0.75f;                          // 192/256 visibles DS
        putv(0, p0.x,p0.y,p0.z, 0, VT); putv(1, p1.x,p1.y,p1.z, 1, VT);
        putv(2, p2.x,p2.y,p2.z, 1, 0);  putv(3, p0.x,p0.y,p0.z, 0, VT);
        putv(4, p2.x,p2.y,p2.z, 1, 0);  putv(5, p3.x,p3.y,p3.z, 0, 0);
        g_bgq = rnd::upload(v, 6, btex);
        g_bgq.lit = 0;                                   // pleine texture
        if (verbose) {
            int tw, th; intro_tex_size(12, &tw, &th);
            printf("[title] bg quad tex=%u img=%dx%d\n", btex, tw, th);
        }
    }
    rnd::draw(g_bgq, Mat4::ident());
    for (auto& p : g_ts.props) {
        if (!p.anm || !p.mdl) continue;
        p.anm_frame += (1.f / 60.f) * 30.f;
        int f = (int)p.anm_frame % std::max(1, p.anm->nframe);
        auto bones = p.mdl->bones;
        for (int ni : p.anm->nodes)
            if (ni >= 0 && ni < (int)bones.size())
                bones[ni] = bmd::bca_node(*p.anm, ni, f, p.mdl->bones[ni]);
        p.anm_cur = bmd::replay_xf(*p.mdl, bones);
    }
    auto drawp = [&](const PropInst& p, bool pass_alpha) {
        float cs = cosf(p.ry), sn = sinf(p.ry);
        Mat4 m = Mat4::ident();
        m.m[0]=cs*p.scale.x; m.m[8]=sn*p.scale.z; m.m[2]=-sn*p.scale.x;
        m.m[10]=cs*p.scale.z; m.m[5]=p.scale.y;
        m.m[12]=p.pos.x; m.m[13]=p.pos.y; m.m[14]=p.pos.z;
        Mat4 cm = mvp.mul(m);
        bool anim = !p.anm_cur.empty() && !getenv("SKIPANIM");
        for (size_t i = 0; i < p.meshes.size(); i++) {
            if (getenv("HIDEVORTEX") && i == 3) continue;
            bool a = p.meshes[i].blend < 0.999f;
            if (a == pass_alpha)
                rnd::draw(p.meshes[i],
                    anim ? cm.mul(_prop_node_delta(p,(int)i)) : cm);
        }
    };
    // écran titre stable : la DS ne dessine que Giratina (+ fond 2D vortex) ;
    // le portail et le visage ne servent que pendant la cutscene d'ouverture
    if (!g_ts.props.empty()) {
        if (verbose && !g_ts.props[0].anm_cur.empty()) {
            static bool vb = false;
            if (!vb && g_ts.t * 60 > 90) {
                vb = true;
                const PropInst& p = g_ts.props[0];
                Vec3 mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
                for (size_t i = 0; i < p.meshes.size(); i++) {
                    Mat4 d = _prop_node_delta(p, (int)i);
                    for (auto& pr : p.mdl->meshes[i].prims)
                        for (auto& v : pr.v) {
                            const float* m = d.m;
                            Vec3 w{m[0]*v.p.x+m[4]*v.p.y+m[8]*v.p.z+m[12],
                                   m[1]*v.p.x+m[5]*v.p.y+m[9]*v.p.z+m[13],
                                   m[2]*v.p.x+m[6]*v.p.y+m[10]*v.p.z+m[14]};
                            mn.x=fminf(mn.x,w.x); mn.y=fminf(mn.y,w.y);
                            mn.z=fminf(mn.z,w.z); mx.x=fmaxf(mx.x,w.x);
                            mx.y=fmaxf(mx.y,w.y); mx.z=fmaxf(mx.z,w.z);
                        }
                }
                printf("[title] ANIM bbox %.0f..%.0f %.0f..%.0f %.0f..%.0f  ctr %.0f,%.0f,%.0f\n",
                    mn.x,mx.x, mn.y,mx.y, mn.z,mx.z,
                    (mn.x+mx.x)/2,(mn.y+mx.y)/2,(mn.z+mx.z)/2);
            }
        }
        drawp(g_ts.props[0], false);
        drawp(g_ts.props[0], true);
    }
}

// ── icônes d'objets ──────────────────────────────────────────────────────────
// item -> membres de item_icon.narc : table générée (ordre res/items/item_icon.order)
#include "item_icons.inc"

uint32_t World::item_icon_tex(int id, int frame) {
    static std::map<uint64_t, uint32_t> c;
    uint64_t key = (uint64_t)(id & 0xFFFFF) * 4 + (frame & 1);
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    uint32_t t = 0;
    auto n = _db->narc(Nds::P_ITEMICON);
    if (n && id >= 0 && id < ITEM_ICON_N) {
        int gm = ITEM_ICON_NCGR[id], pm = ITEM_ICON_NCLR[id];
        if (gm < n->count() && pm < n->count()) {
            auto pals = spr::parse_nclr(n->member(pm));
            auto g    = spr::parse_ncgr(n->member(gm));
            if (!pals.empty() && g.tiles.size() >= 16 * 32) {
                int nf = (int)(g.tiles.size() / (16 * 32));   // 1 ou 2 frames
                int t0 = (nf > 1 ? (frame & 1) : 0) * 16;
                auto im = spr::compose(g, t0, 4, 4, pals[0]);
                if (im) t = rnd::upload_tex(*im);
            }
        }
    }
    c[key] = t;
    return t;
}

// ── icône d'espèce (pl_poke_icon.narc) : membre 7+sp, sous-palette par espèce
#include "poke_icon_pals.inc"
uint32_t World::mon_icon_tex(int species, int frame) {
    static std::map<int, uint32_t> c;
    int key = species * 2 + (frame & 1);
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    uint32_t t = 0;
    auto n = _db->narc("/poketool/icongra/pl_poke_icon.narc");
    if (n && n->count() > 7 + species) {
        auto pals = spr::parse_nclr(n->member(0));
        auto g = spr::parse_ncgr(n->member(7 + species));
        if (!pals.empty() && g.tiles.size() >= 16 * 32) {
            int pi = (species >= 0 && species < (int)(sizeof(POKE_ICON_PAL)))
                ? POKE_ICON_PAL[species] : 0;
            int nf = (int)(g.tiles.size() / (16 * 32));
            int t0 = (nf > 1 ? (frame & 1) : 0) * 16;
            auto im = spr::compose(g, t0, 4, 4, pals[pi < (int)pals.size() ? pi : 0]);
            if (im) t = rnd::upload_tex(*im);
        }
    }
    c[key] = t;
    return t;
}

// ── icônes de poches : icône d'objet représentatif par poche (item_icon.narc)
uint32_t World::pocket_icon_tex(int pocket, bool sel) {
    // OBJETS / MEDICAMENTS / BALLS / CT&CS / BAIES / LETTRES / COMBAT / RARES
    static const int ITEM[8] = {78, 17, 4, 328, 149, 137, 57, 437};
    return item_icon_tex(ITEM[pocket & 7], sel ? 1 : 0);
}

// ── décor de combat : pl_batt_bg.narc ------------------------------------------
// formule ROM (battle_main.c) : tuiles = 3+bg (RGCN 8bpp), palette = 172+bg*3+tod
// (jour/soir/nuit), tilemap = membre 2 (RCSN 512x256, blocs d'écran 32x32).
// La DS remplit palette[0] avec sBackgroundColors[bg][tod] (couleur de fond).
static const RGBAf BG_FILL[][3] = {
    /* plain/water/city/mountain */ { {136,208,240,255}, {240,136,0,255},  {0,128,184,255} },
    /* forest                   */ { {88,168,40,255},  {88,168,40,255},  {88,168,40,255}  },
    /* snow                     */ { {248,208,248,255},{248,208,176,255},{192,176,176,255}},
    /* indoors/league/frontier  */ { {248,248,248,255},{248,248,248,255},{248,248,248,255}},
    /* cave                     */ { {40,40,56,255},   {40,40,56,255},   {40,40,56,255}   },
};
static RGBAf bg_fill(int bg, int tod) {
    int g = (bg == 3) ? 1 : (bg == 5) ? 2 : (bg == 6 || bg == 7 || bg == 8 ||
        bg == 12 || bg == 13 || bg == 14 || bg == 15 || bg == 16 || bg == 17 ||
        bg == 18 || bg == 19 || bg == 20 || bg == 21 || bg == 22) ? 3 :
        (bg >= 9 && bg <= 11) ? 4 : 0;
    if (tod < 0 || tod > 2) tod = 0;
    return BG_FILL[g][tod];
}
uint32_t World::battle_bg_tex(int bg, int tod) {
    static std::map<int, uint32_t> c;
    int key = bg * 4 + (tod & 3);
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    uint32_t t = 0;
    std::shared_ptr<Narc> n = _db->narc("/battle/graphic/pl_batt_bg.narc");
    if (!n || !n->count())
        n = _db->narc("/resource/" + res_dir(_db) + "/battle_graphic/pl_batt_bg.narc");
    if (n) {
        int ti = 3 + bg, pi = 172 + bg * 3 + tod, si = 2;
        if (ti < n->count() && pi < n->count() && si < n->count()) {
            auto g = spr::parse_ncgr(n->member(ti));
            auto pals = spr::parse_nclr(n->member(pi));
            RGBAf fill = bg_fill(bg, tod);
            for (auto& p : pals) if (!p.empty()) p[0] = fill;  // fond uni DS
            auto im = spr::render_screen(n->member(si), g, pals);
            if (im) {
                for (size_t k = 0; k < im->px.size(); k += 4)
                    if (!im->px[k + 3]) {
                        im->px[k] = fill.r; im->px[k + 1] = fill.g;
                        im->px[k + 2] = fill.b; im->px[k + 3] = 255;
                    }
                t = rnd::upload_tex(*im);
            }
        }
    }
    c[key] = t;
    return t;
}

// ── plateformes de terrain : pl_batt_obj.narc ----------------------------------
// sprites.order : [membre NCGR joueur, membre NCGR ennemi, membre NCLR du décor]
// (jour). La cellule NCER suit toujours le NCGR joueur (128) / ennemi (131).
struct PlatDef { int nclr, ncgr_p, ncgr_e; };
static const PlatDef PLAT_DEFS[] = {
    /* plain    */ { 7,  135, 136 },  // path
    /* sand     */ { 22, 145, 146 },
    /* grass    */ { 1,  127, 130 },
    /* puddle   */ { 31, 151, 152 },
    /* mountain */ { 13, 139, 140 },  // rocky
    /* cave     */ { 28, 149, 150 },
    /* snow     */ { 16, 141, 142 },
    /* water    */ { 4,  133, 134 },
    /* ice      */ { 10, 137, 138 },
    /* building */ { 19, 143, 144 },  // indoors
    /* marsh    */ { 25, 147, 148 },  // mud
    /* bridge   */ { 31, 151, 152 },  // puddles
    /* aaron    */ { 34, 153, 154 },
    /* bertha   */ { 37, 155, 156 },
    /* flint    */ { 40, 157, 158 },
    /* lucian   */ { 43, 159, 160 },
    /* cynthia  */ { 46, 161, 162 },
    /* distort. */ { 49, 163, 164 },
    /* tower    */ { 52, 165, 166 },
    /* factory  */ { 55, 167, 168 },
    /* arcade   */ { 58, 169, 170 },
    /* castle   */ { 61, 171, 172 },
    /* hall     */ { 64, 173, 174 },
    /* giratina */ { 67, 175, 176 },
};
static const int PLAT_N = (int)(sizeof(PLAT_DEFS) / sizeof(PLAT_DEFS[0]));
static std::map<int, std::pair<int,int>> g_platsz;

uint32_t World::battle_plat_tex(int ter, int side) {
    static std::map<int, uint32_t> c;
    int key = ter * 2 + (side & 1);
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    uint32_t t = 0;
    if (ter < 0 || ter >= PLAT_N) ter = 2;  // grass
    const PlatDef& d = PLAT_DEFS[ter];
    auto n = _db->narc("/resource/" + res_dir(_db) + "/battle_graphic/pl_batt_obj.narc");
    if (!n || !n->count())
        n = _db->narc("/battle/graphic/pl_batt_obj.narc");
    int gi = side ? d.ncgr_e : d.ncgr_p;
    int ci = side ? 131 : 128;             // enemy_cell / player_cell
    if (n && gi < n->count() && ci < n->count() && d.nclr < n->count()) {
        auto g = spr::parse_ncgr(n->member(gi));
        auto pals = spr::parse_nclr(n->member(d.nclr));
        auto cells = spr::parse_ncer(n->member(ci));
        if (!pals.empty() && !cells.empty()) {
            auto im = spr::render_cell(g, cells[0], pals, 0, true);
            if (im) {
                g_platsz[key] = {im->w, im->h};
                t = rnd::upload_tex(*im);
            }
        }
    }
    c[key] = t;
    return t;
}

void World::battle_plat_size(int ter, int side, int* w, int* h) {
    int key = ter * 2 + (side & 1);
    auto it = g_platsz.find(key);
    if (it == g_platsz.end()) { *w = *h = 0; return; }
    *w = it->second.first; *h = it->second.second;
}

// ── sprites dresseur de combat : trbgra (dos) / trfgra (face) -----------------
// groupes de 5 membres : [NCGR][NCLR][RECN][RNAN][NCGR 2e feuille]. La cellule 0
// du RECN donne la pose debout (les cellules suivantes = lancer de Ball).
static uint32_t tr_tex(Nds* db, const char* path, int idx) {
    static std::map<std::string, std::map<int, uint32_t>> cache;
    auto& c = cache[path];
    auto it = c.find(idx);
    if (it != c.end()) return it->second;
    uint32_t t = 0;
    auto n = db->narc(path);
    if (n && idx * 5 + 2 < n->count()) {
        auto g = spr::parse_ncgr(n->member(idx * 5));
        auto pals = spr::parse_nclr(n->member(idx * 5 + 1));
        auto cells = spr::parse_ncer(n->member(idx * 5 + 2));
        if (!pals.empty() && !cells.empty()) {
            if (auto im = spr::render_cell(g, cells[0], pals, 0, true))
                t = rnd::upload_tex(*im);
        }
    }
    c[idx] = t;
    return t;
}

uint32_t World::trainer_back_tex(int idx) {
    return tr_tex(_db, "/poketool/trgra/trbgra.narc", idx);
}
uint32_t World::trainer_front_tex(int idx) {
    return tr_tex(_db, "/poketool/trgra/trfgra.narc", idx);
}
