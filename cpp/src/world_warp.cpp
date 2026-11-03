// world_warp.cpp — transitions de map + animations de props (portes).
#include "world.h"
#include "sdat.h"
#include "world_internal.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>

// ── animations de props : portes (map_prop_animation.c + ov5_021D431C.c) ────
// bm_anime_list[model] = {u8 has, u8 flags, u8 bike, u8 _, s32 archives[4]} ;
// bm_anime[archive] = fichier BCA0/BTA0/BTP0 (on n'implémente que BCA0 —
// les portes classiques ; l'ascenseur en BTP0 retombe sur la transition fade).
static const int DOOR_MODELS[] = {          // ordre prop_models de la décomp
    66, 67, 68, 69, 246, 70, 427, 456, 260, 312, 313, 438, 444, 441, 442,
    298, 484, 128, 527, 75
};

const std::vector<int>* World::_anim_ids(int model) {
    auto it = _alist.find(model);
    if (it != _alist.end()) return &it->second;
    auto n = _db->narc(Nds::P_BMANILIST);
    std::vector<int> ids;
    if (n && model < (int)n->members.size()) {
        const Bytes& e = n->members[model];
        for (int i = 0; i < 4 && 4 + i * 4 + 4 <= (int)e.size(); i++) {
            int aid = s32(e, 4 + i * 4);
            if (aid < 0) break;
            ids.push_back(aid);
        }
    }
    return &_alist.emplace(model, std::move(ids)).first->second;
}

const bmd::JntAnim* World::_bca_anim(int aid) {
    auto it = _bca.find(aid);
    if (it != _bca.end()) return it->second.ok ? &it->second : nullptr;
    bmd::JntAnim an;
    auto n = _db->narc(Nds::P_BMANIME);
    if (n && aid >= 0 && aid < (int)n->members.size())
        an = bmd::parse_bca(n->members[aid]);
    auto& r = _bca[aid] = std::move(an);
    return r.ok ? &r : nullptr;
}

// archive BTA0 (SRT0 : transform UV des matériaux) — fontaines, roues à eau
const std::vector<bmd::TexAnim>* World::_bta_anim(int aid) {
    auto it = _btas.find(aid);
    if (it != _btas.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<bmd::TexAnim> ta;
    auto n = _db->narc(Nds::P_BMANIME);
    if (n && aid >= 0 && aid < (int)n->members.size()) {
        const Bytes& e = n->members[aid];
        if (e.size() >= 4 && e[0] == 'B' && e[1] == 'T' && e[2] == 'A' && e[3] == '0')
            ta = bmd::parse_bta0(e);
    }
    auto& r = _btas[aid] = std::move(ta);
    return r.empty() ? nullptr : &r;
}

// archive BTP0 (PAT0 : swap texture+palette des matériaux) — éoliennes, écrans
const std::vector<bmd::TexPat>* World::_btp_anim(int aid) {
    auto it = _btps.find(aid);
    if (it != _btps.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<bmd::TexPat> tp;
    auto n = _db->narc(Nds::P_BMANIME);
    if (n && aid >= 0 && aid < (int)n->members.size()) {
        const Bytes& e = n->members[aid];
        if (e.size() >= 4 && e[0] == 'B' && e[1] == 'T' && e[2] == 'P' && e[3] == '0')
            tp = bmd::parse_btp0(e);
    }
    auto& r = _btps[aid] = std::move(tp);
    return r.empty() ? nullptr : &r;
}

int World::_prop_anim_count(int model) {
    auto ids = _anim_ids(model);
    return ids ? (int)ids->size() : 0;
}

// map_prop_animation.c : entry ni différée (flags==0) ni pente vélo -> l'anim
// est attachée au chargement et boucle à l'infini (éoliennes, fontaines…).
// flags=0x3 = portes/script, 0x2 = attachée plus tard (script), bike = pente.
int World::_anim_auto(int model) {
    auto n = _db->narc(Nds::P_BMANILIST);
    if (!n || model < 0 || model >= (int)n->members.size()) return -1;
    const Bytes& e = n->members[model];
    if (e.size() < 8 || e[1] != 0 || e[2] != 0) return -1;
    return s32(e, 4) >= 0 ? 0 : -1;
}

// porte dont l'origine tombe dans la hitbox tuiles [tx0..tx1]×[tz0..tz1]
// (TerrainCollisionHitbox_Init de la DS, appliqué à la position du prop)
int World::_find_door_prop(int tx0, int tz0, int tx1, int tz1) {
    float x0 = tx0 * TILE - 8.f, x1 = (tx1 + 1) * TILE + 8.f;
    float z0 = tz0 * TILE - 8.f, z1 = (tz1 + 1) * TILE + 8.f;
    for (size_t i = 0; i < _props.size(); i++) {
        auto& p = _props[i];
        if (p.model < 0) continue;
        bool isdoor = false;
        for (int dm : DOOR_MODELS) if (dm == p.model) { isdoor = true; break; }
        if (!isdoor) continue;
        if (p.pos.x >= x0 && p.pos.x < x1 && p.pos.z >= z0 && p.pos.z < z1)
            return (int)i;
    }
    return -1;
}

bool World::_prop_play(int pi, int idx) {
    if (pi < 0 || pi >= (int)_props.size()) return false;
    auto& p = _props[pi];
    auto ids = _anim_ids(p.model);
    if (!ids || idx < 0 || idx >= (int)ids->size()) return false;
    auto* an = _bca_anim((*ids)[idx]);
    if (!an) {
        // BTP0 (swap texture) : attaché au prop, joué en boucle dans le tick
        if (auto* tp = _btp_anim((*ids)[idx])) {
            p.pats = tp; p.panm_frame = 0.f;
            if (getenv("WARPDBG"))
                printf("[door] prop %d model %d anim[%d] arc=%d -> BTP0\n",
                    pi, p.model, idx, (*ids)[idx]);
            return true;
        }
    }
    if (getenv("WARPDBG"))
        printf("[door] prop %d model %d anim[%d] arc=%d -> %s nfr=%d nodes=%zu\n",
            pi, p.model, idx, (*ids)[idx], an ? "BCA0" : "non-BCA0",
            an ? an->nframe : 0, an ? an->nodes.size() : 0);
    if (!an) {                                   // non géré -> skip
        p.anm = nullptr; p.anm_idx = -1; p.anm_hold = true;
        p.anm_cur.clear();
        return false;
    }
    p.anm = an; p.anm_idx = idx; p.anm_frame = 0.f; p.anm_hold = false;
    // première frame tout de suite (sinon la porte reste fermée 1 frame)
    return true;
}

bool World::_prop_anim_done(const PropInst& p) {
    return !p.anm || p.anm_hold;
}

void World::_prop_anim_stop(PropInst& p) {
    p.anm = nullptr; p.anm_idx = -1; p.anm_hold = false;
    p.anm_cur.clear();
}

// valeur d'un canal SRT0 à la frame f (clés cyclées, sinon constante)
static float srt_eval(const bmd::TexAnim::Chan& c, int f) {
    if (c.key && !c.keys.empty()) return c.keys[f % (int)c.keys.size()];
    return c.cst;
}

// avance les anims (30 fps DS) et recalcule les matrices des nœuds animés
void World::_tick_prop_anims(float dt) {
    for (auto& p : _props) {
        if (p.tanms) {                      // SRT0 : transform UV par matériau
            int nfr = 0;
            for (auto& t : *p.tanms) nfr = std::max(nfr, t.nframe);
            if (nfr > 0) {
                p.tanm_frame += dt * 30.f;
                while (p.tanm_frame >= nfr) p.tanm_frame -= nfr;
                int f = (int)p.tanm_frame;
                for (auto& g : p.meshes) {
                    const bmd::TexAnim* ta = nullptr;
                    for (auto& t : *p.tanms)
                        if (t.mat == g.dbg_name) { ta = &t; break; }
                    if (!ta) continue;
                    float rt = srt_eval(ta->rot, f);
                    g.uvtr[0] = srt_eval(ta->tu, f);
                    g.uvtr[1] = srt_eval(ta->tv, f);
                    g.uvtr[2] = srt_eval(ta->su, f);
                    g.uvtr[3] = srt_eval(ta->sv, f);
                    if (g.uvtr[2] == 0.f) g.uvtr[2] = 1.f;
                    if (g.uvtr[3] == 0.f) g.uvtr[3] = 1.f;
                    g.uvrot[0] = cosf(rt); g.uvrot[1] = sinf(rt);
                    g.uvani = true;
                }
            }
        }
        if (p.pats) {                     // PAT0 : swap texture par matériau
            int nfr = 0;
            for (auto& tp : *p.pats) nfr = std::max(nfr, tp.nframe);
            if (nfr > 0) {
                // panm_state : 0 boucle, 1 one-shot->figé (boutons arène),
                // 2 figé sur la frame courante (état imposé par le script)
                if (p.panm_state != 2) {
                    p.panm_frame += dt * 30.f;
                    if (p.panm_state == 1 &&
                        (int)p.panm_frame >= nfr - 1) {
                        p.panm_frame = (float)(nfr - 1);
                        p.panm_state = 2;
                    } else {
                        while (p.panm_frame >= nfr) p.panm_frame -= nfr;
                    }
                }
                int f = (int)p.panm_frame;
                for (auto& tp : *p.pats)
                    for (auto& tr : tp.tracks) {
                        if (tr.keys.empty()) continue;
                        const bmd::TexPat::Key* k = &tr.keys.back();
                        for (auto& kk : tr.keys) {   // dernière clé <= f
                            if (kk.frame <= f) k = &kk; else break;
                        }
                        if (k->tex >= tp.texs.size() || k->pal >= tp.pals.size())
                            continue;
                        for (auto& g : p.meshes)
                            if (g.dbg_name == tr.mat) {
                                uint32_t t = _pat_tex(p.mdl,
                                    tp.texs[k->tex], tp.pals[k->pal]);
                                if (t && t != g.tex) {
                                    if (getenv("WARPDBG"))
                                        printf("[pat] prop mdl=%d mat=%s f=%d -> %s|%s\n",
                                            p.model, tr.mat.c_str(), f,
                                            tp.texs[k->tex].c_str(),
                                            tp.pals[k->pal].c_str());
                                    g.tex = t;
                                }
                            }
                    }
            }
        }
        if (!p.anm || !p.mdl) continue;
        bool dirty = false;
        if (!p.anm_hold) {
            p.anm_frame += dt * 30.f;            // 30 fps DS (fraction conservée)
            if (p.anm_loop) {
                while (p.anm_frame >= p.anm->nframe)
                    p.anm_frame -= p.anm->nframe;  // boucle infinie (loopCount=-1)
            } else if ((int)p.anm_frame >= p.anm->nframe - 1) {
                p.anm_frame = (float)(p.anm->nframe - 1);
                p.anm_hold = true;
            }
            dirty = true;
        }
        if (p.anm_cur.empty()) dirty = true;
        if (!dirty) continue;
        auto bones = p.mdl->bones;                // locales remplacées par l'anim
        for (int ni : p.anm->nodes)
            if (ni >= 0 && ni < (int)bones.size())
                bones[ni] = bmd::bca_node(*p.anm, ni, (int)p.anm_frame,
                                          p.mdl->bones[ni]);
        p.anm_cur = bmd::replay_xf(*p.mdl, bones);
    }
}

// matrice d'animation d'un mesh : anim_cur * repos⁻¹ (verts déjà cuits repos)
Mat4 World::_prop_node_delta(const PropInst& p, int mesh_i) {
    Mat4 m = Mat4::ident();
    if (!p.anm || mesh_i >= (int)p.mkey.size()) return m;
    auto it = p.anm_cur.find(p.mkey[mesh_i]);
    if (it == p.anm_cur.end() || mesh_i >= (int)p.minv.size()) return m;
    M4 d = it->second.mul(p.minv[mesh_i]);        // M4 3x4 -> Mat4 4x4 (col-major)
    m.m[0] = d.x.x; m.m[1] = d.x.y; m.m[2] = d.x.z;  m.m[3] = 0;
    m.m[4] = d.y.x; m.m[5] = d.y.y; m.m[6] = d.y.z;  m.m[7] = 0;
    m.m[8] = d.z.x; m.m[9] = d.z.y; m.m[10] = d.z.z; m.m[11] = 0;
    m.m[12] = d.w.x; m.m[13] = d.w.y; m.m[14] = d.w.z; m.m[15] = 1;
    return m;
}

// ── transitions de map DS ────────────────────────────────────────────────────
// field_control.c Field_CheckMapTransition (pousser vers une tuile bloquée)
// + Field_CheckTransition (poser le pas) + unk_02056B30.c (task départ/arrivée).
int World::_warp_at(int tx, int tz) const {
    for (size_t i = 0; i < _warps.size(); i++)
        if (_warps[i].x == tx && _warps[i].z == tz) return (int)i;
    return -1;
}

// Field_MapConnection : mémorise la sortie (entrance) et résout la
// destination — dwarp 0x100 -> lieu spécial sauvegardé (field_map_change.c).
bool World::_warp_depart(int wi, int type, int adir) {
    auto& w = _warps[wi];
    _entrance = {_header, wi, w.x, w.z, _pdir};
    if (w.dwarp == 0x100) {
        if (_special.hdr < 0) {
            static int nwarn = 0;               // le bump reteste chaque frame
            if (nwarn++ < 3)
                printf("[warp] dwarp 0x100 sans lieu spécial (hdr %d)\n", _header);
            return false;
        }
        _wt.dh = _special.hdr; _wt.dw = _special.warp;
        // SetSpecialLocation : position + direction d'arrivée explicites
        _wt.dx = _special.x; _wt.dz = _special.z; _wt.ddir = _special.dir;
    } else { _wt.dh = w.dest; _wt.dw = w.dwarp;
             _wt.dx = _wt.dz = _wt.ddir = -1; }
    _wt.type = type; _wt.adir = adir; _wt.sub = 0; _wt.door = -1; _wt.t = 0;
    _pstep = 0; _panim = 0.f;
    if (getenv("WARPDBG"))
        printf("[warp] depart w%d (%d,%d)->hdr%d w%d type=%d adir=%d\n",
            wi, w.x, w.z, _wt.dh, _wt.dw, type, adir);
    if (type == WT_PANEL) {
        // ov5 panneau warp : spin 20 ticks (rotation 90° tous les 2 ticks)
        // + montée h = n(2.2+0.5n) unités ; fondu sortant lancé au tick 20.
        if (_snd) _snd->se(1499);                // SEQ_SE_PL_WARP
        _wt.py = _ppos.y; _wt.sub = 0;
        _wt.st = WP_DEP_PANEL;
        return true;
    }
    if (type == WT_DOOR || type == WT_STAIRS || type == WT_ESCAL) {
        // départ : le joueur entre — marche auto 1 tuile, collision ignorée
        // (porte = vitesse normale, escaliers = lente)
        int tx = (int)floorf(_ppos.x / TILE), tz = (int)floorf(_ppos.z / TILE);
        int ddx, ddz; dirv(_pdir, ddx, ddz);
        _wt.wx = (tx + ddx) * TILE + TILE / 2;
        _wt.wz = (tz + ddz) * TILE + TILE / 2;
        _wt.wspd = type == WT_DOOR ? 60.f : 30.f;   // escalator = marche lente
        if (type == WT_ESCAL && _snd) _snd->se(1557);  // SEQ_SE_DP_ESUKA
        _wt.door = -1;
        if (type == WT_DOOR) {
            // ov5_021D433C : hitbox 3x1 sur la rangée de la tuile porte,
            // ouverture (anim 0) puis attente de fin avant la marche.
            int bx = tx + ddx, bz = tz + ddz;     // tuile porte poussée
            _wt.door = _find_door_prop(bx - 1, bz - 1, bx + 1, bz);
            if (getenv("WARPDBG"))
                printf("[door] hitbox (%d..%d,%d..%d) -> prop %d model %d\n",
                    bx - 1, bx + 1, bz - 1, bz, _wt.door,
                    _wt.door >= 0 ? _props[_wt.door].model : -1);
            if (_wt.door >= 0 && _prop_play(_wt.door, 0)) {
                if (_snd) _snd->se(1540);         // SEQ_SE_DP_DOOR (ouverture)
                _wt.st = WP_DEP_DOOR; _wt.sub = 0;
                return true;
            }
        }
        _wt.st = WP_DEP_WALK;
    } else {
        _fade = {0.f, 8.f / 30.f, 0, true};     // fondu sortant (~8 frames DS)
        _wt.st = WP_DEP_FADE;
    }
    return true;
}

// marche automatique du joueur vers (_wt.wx,_wt.wz) — vrai une fois arrivé
bool World::_warp_step(float dt) {
    float ddx = _wt.wx - _ppos.x, ddz = _wt.wz - _ppos.z;
    float dl = sqrtf(ddx * ddx + ddz * ddz), st = _wt.wspd * dt;
    static int wdbg = -1;
    if (wdbg < 0) wdbg = getenv("WSTEPDBG") != nullptr;
    if (wdbg) printf("[wstep] pos=(%.1f,%.1f) tgt=(%.1f,%.1f) dl=%.2f "
        "wspd=%.1f dt=%.4f st=%.3f\n", _ppos.x, _ppos.z, _wt.wx, _wt.wz,
        dl, _wt.wspd, dt, st);
    _panim += dt * (_wt.wspd > 40.f ? 8.f : 5.f);
    if (_panim > 4.f) _panim -= 4.f;
    if (dl <= st) { _ppos.x = _wt.wx; _ppos.z = _wt.wz; _panim = 0.f; return true; }
    _ppos.x += ddx / dl * st; _ppos.z += ddz / dl * st;
    return false;
}

void World::_warp_fin() {                       // fin de transition
    _wt.st = -1; _phidden = false; _panim = 0.f;
    if (getenv("WARPDBG"))
        printf("[warp] fin t=(%d,%d) dir=%d hdr=%d\n",
            (int)floorf(_ppos.x / TILE), (int)floorf(_ppos.z / TILE),
            _pdir, _header);
    // les marches scriptées ne passent pas par ProcessStep : la tuile
    // finale est marquée pour ne pas déclencher d'événement de pas.
    _warp_cd_x = (int)floorf(_ppos.x / TILE);
    _warp_cd_z = (int)floorf(_ppos.z / TILE);
}

void World::_tick_warp(float dt) {
    if (_wt.st != WP_LOAD && _wt.st != WP_DEP_PANEL &&
        _wt.st != WP_ARR_PANEL) {               // gravité gelée : posé au sol
        // DS CalculateObjectHeight : plaque la plus proche du y courant —
        // un biais vers le haut faisait sauter le joueur sur les plaques
        // empilées (ponts, hauts d'escalier) pendant les marches de warp.
        float gy = _ground_y(_ppos, _ppos.y);
        if (gy > -1e8f) { _ppos.y = gy; _vel.y = 0.f; }
    }
    switch (_wt.st) {
    case WP_DEP_PANEL: {                        // panneau warp : spin + montée
        _wt.t += dt;
        int n = std::min(20, (int)(_wt.t * 30.f));
        if (n % 2 == 0) _pdir = (_pdir + 1) & 3;
        _ppos.y = _wt.py + n * (2.2f + 0.5f * n);
        if (n >= 20) {
            _fade = {0.f, 6.f / 30.f, 0, true}; // fondu sortant 6 ticks DS
            _wt.st = WP_DEP_FADE;
        }
        break; }
    case WP_ARR_PANEL: {                        // descente : hauteur se défait
        _wt.t += dt;
        int n = std::max(0, 20 - (int)(_wt.t * 30.f));
        if (n % 2 == 0) _pdir = (_pdir + 1) & 3;
        _ppos.y = _wt.py + n * (2.2f + 0.5f * n);
        if (n == 2)
            _fade = {0.f, 6.f / 30.f, 0, false};    // fondu entrant au tick 2
        if (n <= 0) { _ppos.y = _wt.py; _pdir = 0; _wt.st = WP_ARR; }
        break; }
    case WP_DEP_DOOR:                           // entrée : anim porte DS
        if (_wt.door < 0) { _wt.st = WP_DEP_WALK; break; }
        if (_wt.sub == 0) {                     // ouverture terminée -> marcher
            if (_prop_anim_done(_props[_wt.door])) _wt.st = WP_DEP_WALK;
        } else {                                // fermeture terminée -> fondu
            if (_prop_anim_done(_props[_wt.door])) {
                _fade = {0.f, 8.f / 30.f, 0, true};
                _wt.st = WP_DEP_FADE;
            }
        }
        break;
    case WP_DEP_WALK:                           // entrée porte/escaliers
        if (_warp_step(dt)) {
            if (_wt.type == WT_DOOR) {
                _phidden = true;                // passe le chambranle
                if (_wt.door >= 0 && _prop_play(_wt.door, 1)) {
                    if (_snd) _snd->se(1542);     // SEQ_SE_DP_DOOR_CLOSE
                    _wt.st = WP_DEP_DOOR; _wt.sub = 1;    // refermer puis fondu
                    break;
                }
            }
            _fade = {0.f, 8.f / 30.f, 0, true};
            _wt.st = WP_DEP_FADE;
        }
        break;
    case WP_DEP_FADE:
        if (_fade.t >= 0) break;
        _wt.st = WP_LOAD;                       // marqueur : load_map interne
        load_map(_wt.dh, _wt.dw);
        if (_wt.bo_tx >= 0) {                   // blackout : tuile de la table
            set_pos(_wt.bo_tx * TILE + TILE / 2,
                    _wt.bo_tz * TILE + TILE / 2);
            _wt.bo_tx = _wt.bo_tz = -1;
        }
        _pdir = _wt.adir;                       // direction d'arrivée DS
        _phidden = false;
        // FieldMapChange_SetNewLocation : un warp d'arrivée pointant vers
        // 0x100 mémorise l'entrée comme nouveau lieu spécial.
        if (_wt.dw >= 0 && _wt.dw < (int)_warps.size() &&
            _warps[_wt.dw].dwarp == 0x100) {
            _special.hdr = _entrance.hdr; _special.warp = _entrance.warp;
            _special.x = _special.z = _special.dir = -1;
        }
        _fade = {0.f, 8.f / 30.f, 0, false};    // fondu entrant
        if (_wt.type == WT_PANEL) {
            // ov5 : le joueur redescend de h=244 en défaisant la spirale
            _wt.py = _ppos.y; _wt.t = 0.f;
            _fade.t = -1;                       // fondu lancé au tick 2
            _wt.st = WP_ARR_PANEL;
        } else if (_wt.type == WT_ESCAL) {
            // ov5_021D5150 : placé 1 tuile en arrière, deux pas lents vers
            // la sortie de l'escalator (arrivée ds = 2 x WALK_SLOW)
            int ddx, ddz; dirv(_pdir, ddx, ddz);
            _ppos.x -= ddx * TILE; _ppos.z -= ddz * TILE;
            _wt.wx = _ppos.x + ddx * 2 * TILE;
            _wt.wz = _ppos.z + ddz * 2 * TILE;
            _wt.wspd = 30.f;
            _wt.st = WP_ARR_WALK;
        } else if (_wt.type == WT_STAIRS) {
            // sub_02057368 : placé au sommet de l'escalier d'arrivée, face à
            // la descente, puis marche lente jusqu'à la tuile du warp.
            int tx = (int)floorf(_ppos.x / TILE), tz = (int)floorf(_ppos.z / TILE);
            int cb = _tile_beh(tx, tz);
            if (cb == TB_STAIRS_E) { _ppos.x += TILE; _pdir = 2; }
            else if (cb == TB_STAIRS_W) { _ppos.x -= TILE; _pdir = 3; }
            int ddx, ddz; dirv(_pdir, ddx, ddz);
            _wt.wx = _ppos.x + ddx * TILE; _wt.wz = _ppos.z + ddz * TILE;
            _wt.wspd = 30.f;
            _wt.st = WP_ARR_WALK;
        } else {
            // ov5_021D5150 : arrivée face sud = sortir de l'embrasure —
            // caché une frame puis 1 pas sud (sauf si la tuile est bloquée)
            int tx = (int)floorf(_ppos.x / TILE), tz = (int)floorf(_ppos.z / TILE);
            bool blk = _blocked({tx * TILE + TILE / 2, _ppos.y + 4,
                                 (tz + 1) * TILE + TILE / 2});
            _wt.sub = (_pdir == 0 && !blk) ? 1 : 0;
            _phidden = _wt.sub == 1;
            _wt.st = WP_ARR;
            // ov5_021D453C : tuile d'arrivée = porte -> séquence de sortie
            // DS : ouverture (anim 2 si 4 anims, 0 sinon), joueur révélé,
            // pas sud, fermeture (3 ou 1). Le fondu entrant tourne en //.
            // La décision se prend sur la tuile d'arrivée seule : sortir par
            // le tapis intérieur (TB_ENTRANCE_S, transition simple) atterrit
            // aussi sur la porte extérieure -> même séquence.
            if (_tile_beh(tx, tz) == TB_DOOR) {
                _wt.door = _find_door_prop(tx - 1, tz - 1, tx + 1, tz);
                int n = _wt.door >= 0 ?
                    _prop_anim_count(_props[_wt.door].model) : 0;
                if (_wt.door >= 0 && n >= 2 &&
                    _prop_play(_wt.door, n >= 4 ? 2 : 0)) {
                    if (_snd) _snd->se(1540);     // SEQ_SE_DP_DOOR (ouverture)
                    _pdir = 0;                      // sortie toujours face sud
                    _phidden = true;
                    _wt.st = WP_ARR_DOOR; _wt.sub = 0;
                } else _wt.door = -1;
            }
        }
        break;
    case WP_ARR:                                // arrivée générique
        if (_wt.sub == 1) {                     // caché sur la tuile -> on sort
            _phidden = false;
            _wt.wx = _ppos.x; _wt.wz = _ppos.z + TILE;
            _wt.wspd = 60.f;
            _wt.sub = 2;
        } else if (_wt.sub == 2) {
            if (_warp_step(dt)) _wt.sub = 3;
        } else if (_fade.t < 0) _warp_fin();
        break;
    case WP_ARR_WALK:                           // arrivée escaliers : descente
        _warp_step(dt);
        if (_fade.t < 0 && _ppos.x == _wt.wx && _ppos.z == _wt.wz) _warp_fin();
        break;
    case WP_ARR_DOOR:                           // sortie : séquence porte DS
        if (_wt.door < 0) { _wt.st = WP_ARR; _wt.sub = 1; break; }
        if (_wt.sub == 0) {                     // ouverture finie -> révéler+pas sud
            if (_prop_anim_done(_props[_wt.door])) {
                _phidden = false;
                _wt.wx = _ppos.x; _wt.wz = _ppos.z + TILE; _wt.wspd = 60.f;
                _wt.sub = 1;
            }
        } else if (_wt.sub == 1) {              // pas sud fini -> fermer
            if (_warp_step(dt)) {
                int n = _prop_anim_count(_props[_wt.door].model);
                if (!_prop_play(_wt.door, n >= 4 ? 3 : 1)) {
                    _wt.door = -1;
                    if (_fade.t < 0) _warp_fin(); else _wt.st = WP_ARR;
                    break;
                }
                if (_snd) _snd->se(1542);        // SEQ_SE_DP_DOOR_CLOSE
                _wt.sub = 2;
            }
        } else {                                // fermeture+fondu finis -> fin
            if (_prop_anim_done(_props[_wt.door]) && _fade.t < 0) _warp_fin();
        }
        break;
    default:
        _warp_fin();                            // phase inconnue : filet de sûreté
        break;
    }
}

// WildEncounters_TryWildEncounter (overlay006/wild_encounters.c) :
// tuile à rencontres -> grâce -> 40% plat (+30 hautes herbes) -> rate% ->
// slot pondéré [20,20,10,10,10,10,5,5,4,4,1,1] -> espèce/niveau de la table.