// world_dw.cpp — Monde Distorsion (ov9_02249960.c)
//
// Port du système dynamique DS : étages 1F/B1F-B7F + salle Giratina +
// grotte Retour (headers 573-583), chacun décrit par un fichier map de
// tw_arc.narc (plateformes flottantes, points de saut, angles caméra,
// props fantômes). tw_arc_attr.narc = grilles d'attributs 32x32 des
// plateformes ; tw_arc_etc.narc = sprites de ciel (nuages).
//
// Coordonnées des templates = espace partagé : la mapinfo donne l'offset
// (offsetTileX, offsetAltitude, offsetTileZ) de chaque étage ; les tuiles
// locales = template - offset, l'altitude monde = (tileY - offset) * 8.
#include "world.h"
#include "sdat.h"   // DsAudio : seq_named/se/se_stop
#include "spr.h"    // parse_nclr/ncgr/ncer, render_screen/render_cell
#include <cmath>
#include <cstdio>

static const char* P_TWARC  = "/fielddata/tornworld/tw_arc.narc";
static const char* P_TWATTR = "/fielddata/tornworld/tw_arc_attr.narc";
static const char* P_TWETC  = "/data/tw_arc_etc.narc";
static Vec3 dw_kind_off(int kind);          // offset prop par kind (DS)
static Vec3 dw_kind_scale(int kind);        // échelle prop par kind (DS)

// headers Monde Distorsion
static bool dw_hdr(int h) { return h >= 573 && h <= 583 && h != 578; }

// ordre des étages (connexions DS linéaires) : prev/next de
// sDistWorldMapConnectionList ; -1 = invalide
static int dw_prev(int h) {
    switch (h) {
    case 574: return 573; case 575: return 574; case 576: return 575;
    case 577: return 576; case 579: return 577; case 580: return 579;
    case 581: return 580;
    }
    return -1;
}
static int dw_next(int h) {
    switch (h) {
    case 573: return 574; case 574: return 575; case 575: return 576;
    case 576: return 577; case 577: return 579; case 579: return 580;
    case 580: return 581;
    }
    return -1;
}

// ------------------------------------------------------------------
// coordonnées : tuiles locales -> tuiles partagées (espace template)
// ------------------------------------------------------------------
int World::_dw_shared_x() const {
    auto* i = _dw.info(_header);
    return (int)floorf(_ppos.x / TILE) + (i ? i->ox : 0);
}
int World::_dw_shared_z() const {
    auto* i = _dw.info(_header);
    return (int)floorf(_ppos.z / TILE) + (i ? i->oz : 0);
}
int World::_dw_shared_y() const {
    auto* i = _dw.info(_header);
    // tileY en tuiles : le y DS est logique (quantisé) — les plaques BDHC
    // ne sont pas toujours à 16.0 pile : arrondi, pas floorf (sinon une
    // surface à 15.9 lit la tuile du dessous et les triggers ratent).
    return (int)floorf(_ppos.y / TILE + 0.5f) + (i ? i->oa : 0);
}

// position monde depuis les coords partagées d'un template
// (DS : prop pos = COORD_CENTER_TO_FX32 -> tile*16+8, toutes axes)
Vec3 World::_dw_world(const DwSys::Info* i, int tx, int ty, int tz) const {
    // x/z = centre de tuile (MapObject pos DS) ; y = base : la hauteur
    // logique DS est tileY*16 px (MapObject.y = pos.y/8, /2 -> tuile).
    return {(tx - i->ox) * TILE + TILE / 2.f,
            (ty - i->oa) * TILE,
            (tz - i->oz) * TILE + TILE / 2.f};
}

// ------------------------------------------------------------------
// parsing : tw_arc member 0 (mapinfo) + fichiers map par étage
// ------------------------------------------------------------------
void World::_dw_load_file(DwFile& f, int hdr) {
    f = DwFile(); f.hdr = hdr;
    auto* i = _dw.info(hdr);
    if (!i || !_dw.tw_arc) return;
    int mi = i->fidx + 1;
    if (mi >= _dw.tw_arc->count()) return;
    const Bytes& d = _dw.tw_arc->member(mi);
    if (d.size() < 20) return;
    int32_t fpz = (int32_t)u32(d, 4), jpz = (int32_t)u32(d, 8),
            caz = (int32_t)u32(d, 12), ghz = (int32_t)u32(d, 16);
    size_t o = 20;
    if (fpz && o + 4 <= d.size()) {
        int cnt = (int32_t)u32(d, o); o += 4;
        for (int k = 0; k < cnt && o + 20 <= d.size(); k++, o += 20) {
            DwFP p;
            p.kind = (int16_t)u16(d, o); p.attr = u16(d, o + 2);
            p.b = {(int16_t)u16(d,o+4),(int16_t)u16(d,o+6),(int16_t)u16(d,o+8),
                   (int16_t)u16(d,o+10),(int16_t)u16(d,o+12),(int16_t)u16(d,o+14)};
            p.tcv = u16(d, o + 16); p.tch = u16(d, o + 18);
            f.fp.push_back(p);
        }
    }
    if (jpz && o + 4 <= d.size()) {
        int cnt = (int32_t)u32(d, o); o += 4;
        for (int k = 0; k < cnt && o + 40 <= d.size(); k++, o += 40) {
            DwJump j;
            j.h = u16(d, o); j.dir = (int16_t)u16(d, o + 2);
            j.b = {(int16_t)u16(d,o+8),(int16_t)u16(d,o+10),(int16_t)u16(d,o+12),
                   (int16_t)u16(d,o+14),(int16_t)u16(d,o+16),(int16_t)u16(d,o+18)};
            j.dx = (int16_t)u16(d, o+20); j.dy = (int16_t)u16(d, o+22);
            j.dz = (int16_t)u16(d, o+24); j.rot = (int16_t)u16(d, o+26);
            j.steps = (int16_t)u16(d, o+28);
            j.axis = u16(d,o+30); j.inv = u16(d,o+32);
            j.fdir = (int16_t)u16(d,o+34); j.kind = (int16_t)u16(d,o+36);
            j.fp = u16(d,o+38);
            f.jump.push_back(j);
            if (getenv("WARPDBG"))
                printf("[dw] jump h=%d dir=%d b=(%d..%d,%d..%d,%d..%d)"
                       " d=(%d,%d,%d) steps=%d kind=%d fp=%d\n",
                    j.h, j.dir, j.b.x, j.b.x + j.b.sx,
                    j.b.y, j.b.y + j.b.sy, j.b.z, j.b.z + j.b.sz,
                    j.dx, j.dy, j.dz, j.steps, j.kind, j.fp);
        }
    }
    if (caz && o + 4 <= d.size()) {
        int cnt = (int32_t)u32(d, o); o += 4;
        for (int k = 0; k < cnt && o + 24 <= d.size(); k++, o += 24) {
            DwCam c;
            c.b = {(int16_t)u16(d,o+0),(int16_t)u16(d,o+2),(int16_t)u16(d,o+4),
                   (int16_t)u16(d,o+6),(int16_t)u16(d,o+8),(int16_t)u16(d,o+10)};
            c.ax = u16(d,o+12); c.ay = u16(d,o+14); c.az = u16(d,o+16);
            c.dir = (int16_t)u16(d,o+18); c.steps = (int32_t)u32(d,o+20);
            f.cam.push_back(c);
        }
    }
    if (ghz && o + 12 <= d.size()) {
        int tc = (int32_t)u32(d,o), tr = (int32_t)u32(d,o+4);
        f.gh_def = u32(d,o+8); o += 12;
        for (int k = 0; k < tc && o + 12 <= d.size(); k++, o += 12) {
            DwGhP g;
            g.grp = u32(d,o); g.kind = u16(d,o+4);
            g.tx = (int16_t)u16(d,o+6); g.ty = (int16_t)u16(d,o+8);
            g.tz = (int16_t)u16(d,o+10); g.prop = -1;
            f.gh.push_back(g);
        }
        for (int k = 0; k < tr && o + 20 <= d.size(); k++, o += 20) {
            DwGhT t;
            t.grp = u32(d,o); t.dir = (int16_t)u16(d,o+4);
            t.show = (int16_t)u16(d,o+6);
            t.b = {(int16_t)u16(d,o+8),(int16_t)u16(d,o+10),(int16_t)u16(d,o+12),
                   (int16_t)u16(d,o+14),(int16_t)u16(d,o+16),(int16_t)u16(d,o+18)};
            f.gtr.push_back(t);
        }
    }
}

// ------------------------------------------------------------------
// props fantômes : groupes visibles -> prop fldeff (124+kind) à la
// position template — offset, échelle, bob et fondu par kind (ov9
// sPropInitialPosOffsetByKind / sPropScaleByKind / animFuncs)
// ------------------------------------------------------------------
// clips joints BCA / scrolls matériau BTA des membres fldeff
// (sPropAnimSetNARCIndexByKind : waterfall BTA 200, shadow BTA 198,
// vine BCA 191, rock BCA 192, portal BCA 193)
const bmd::JntAnim* World::_dw_bca_clip(int member) {
    auto it = _dw.dw_bca.find(member);
    if (it != _dw.dw_bca.end()) return it->second.ok ? &it->second : nullptr;
    bmd::JntAnim an;
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (n && member >= 0 && member < n->count())
        an = bmd::parse_bca(n->member(member));
    auto& r = _dw.dw_bca[member] = std::move(an);
    return r.ok ? &r : nullptr;
}

const std::vector<bmd::TexAnim>* World::_dw_bta_anims(int member) {
    auto it = _dw.dw_bta.find(member);
    if (it != _dw.dw_bta.end())
        return it->second.empty() ? nullptr : &it->second;
    std::vector<bmd::TexAnim> ta;
    auto n = _db->narc("/data/mmodel/fldeff.narc");
    if (n && member >= 0 && member < n->count()) {
        const Bytes& e = n->member(member);
        if (e.size() >= 4 && e[0] == 'B' && e[1] == 'T' && e[2] == 'A'
            && e[3] == '0')
            ta = bmd::parse_bta0(e);
    }
    auto& r = _dw.dw_bta[member] = std::move(ta);
    return r.empty() ? nullptr : &r;
}

// attache l'anim DS du kind au prop + les nœuds pour _prop_node_delta
void World::_dw_anim_attach(PropInst& pi, int kind) {
    if (!pi.mdl) return;
    switch (kind) {
    case 21: pi.tanms = _dw_bta_anims(200); break;    // cascade (scroll UV)
    case 20: pi.tanms = _dw_bta_anims(198); break;    // ombre Giratina
    case 22: case 23: case 24: {
        static const int MEMB[3] = {191, 192, 193};   // kind-22 : BCA
        if (auto* an = _dw_bca_clip(MEMB[kind - 22])) {
            pi.anm = an;
            // obstacle (vigne/rocher) : frame pilotée par l'état fantôme ;
            // portal/simple : boucle continue (Simple3D_UpdateAnim FX32_ONE)
            pi.anm_hold = kind != 24;
            pi.anm_loop = kind == 24;
        }
        break;
    }
    default: break;
    }
    if (!pi.anm && !pi.tanms) return;
    for (auto& mg : pi.mdl->meshes) {
        pi.mkey.push_back(bmd::group_key(mg.mat, mg.node));
        pi.minv.push_back(mg.xf.inverse());
    }
}

// spawn unitaire d'un prop fantôme (fldeff 124+kind, position template)
void World::_dw_spawn_one(DwFile& f, DwGhP& g) {
    // le rendu se fait dans l'espace de l'étage actif : coords partagées
    // -> locales de l'étage ACTIF (l'inactif est décalé par ses offsets)
    auto* i = _dw.info(_header);
    if (!i || f.hdr < 0) return;
    PropInst pi;
    int member = 124 + g.kind;
    pi.meshes = _fld_mesh(member);
    pi.mdl = _fld_mdl(member);
    if (!pi.mdl || pi.meshes.empty()) return;
    pi.pos = _dw_world(i, g.tx, g.ty, g.tz) + dw_kind_off(g.kind);
    pi.scale = dw_kind_scale(g.kind);
    pi.model = -3;
    pi.cell = -3;                           // prop DW : balayé à chaque init
    _dw_anim_attach(pi, g.kind);
    g.base = pi.pos;
    // obstacle déjà visible : DS fige le clip sur sa dernière frame
    if (g.vis && pi.anm && pi.anm_hold)
        g.aframe = (float)pi.anm->nframe;
    // bob : départ pseudo-aléatoire comme la DS (LCRNG % 8*FX32)
    if (g.prog == 0.f && g.delta == 0.f) {
        uint32_t h = g.grp * 2654435761u ^ (uint32_t)g.tx * 97u
                   ^ (uint32_t)g.tz * 13u ^ (uint32_t)g.ty * 7u;
        g.prog = (float)((h >> 3) % (8 * 4096));
        g.delta = ((int)g.prog & 1) ? -2048.f : 2048.f;
    }
    g.prop = (int)_props.size();
    _props.push_back(pi);
}

void World::_dw_spawn_ghosts(DwFile& f) {
    for (auto& g : f.gh) {
        g.prop = -1;
        if (g.grp > 23) continue;                       // GHOST_PROP_GROUP_MAX
        bool hid = (_dw.hidden_gh >> g.grp) & 1;
        g.vis = !hid;
        if (hid) { g.opac = 0.f; continue; }
        g.opac = 31.f;                                  // plein dès l'init DS
        _dw_spawn_one(f, g);
    }
}

// applique hidden_gh aux props spawnés + spawn à la demande des
// groupes révélés — la visibilité se fond dans _dw_ghost_tick (DS)
void World::_dw_ghost_apply(DwFile& f) {
    for (auto& g : f.gh) {
        if (g.grp > 23) continue;
        bool hid = (_dw.hidden_gh >> g.grp) & 1;
        g.vis = !hid;
        if (!hid && g.prop < 0) {
            g.opac = 0.f;
            _dw_spawn_one(f, g);
        }
    }
}

// animateur d'un prop fantôme (DistWorldPlatformProp/ObstacleProp ov9) :
// plateformes = bob 8 pas ping-pong + fondu ±1/frame ; obstacles
// (vigne-fleur 22, rocher 23) = clip joint ±2 frames + fondu ±2
void World::_dw_ghost_tick(DwFile& f, float s60) {
    static const float BOB[8] = {0,-1,-2,-4,-5,-5.5f,-5.75f,-6};
    for (auto& g : f.gh) {
        if (g.grp > 23 || g.prop < 0 || g.prop >= (int)_props.size())
            continue;
        auto& pi = _props[g.prop];
        bool obs = g.kind == 22 || g.kind == 23;
        if (obs) {
            if (pi.anm) {
                float nfr = (float)pi.anm->nframe;
                if (!g.vis) {
                    g.aframe = fmaxf(0.f, g.aframe - 2.f * s60);
                    if (g.opac >= g.aframe)
                        g.opac = fmaxf(0.f, g.opac - 2.f * s60);
                } else {
                    g.opac = fminf(31.f, g.opac + 2.f * s60);
                    g.aframe = fminf(nfr, g.aframe + 2.f * s60);
                }
                pi.anm_frame = g.aframe;
                pi.anm_cur.clear();          // force recompute au tick anim
            }
        } else {
            float y = BOB[((int)(g.prog / 4096.f)) & 7];
            // petite plateforme occupée : bob moitié (DistWorldPlatformProp)
            if (g.kind == 0 && f.hdr == _header
                && _dw_shared_x() == g.tx && _dw_shared_z() == g.tz
                && _dw_shared_y() == g.ty) y *= 0.5f;
            pi.pos.y = g.base.y + y;
            g.prog += g.delta * s60;
            if (g.prog < 0.f) { g.prog = 0.f; g.delta = 2048.f; }
            else if (g.prog >= 8.f * 4096.f) {
                g.prog = 8.f * 4096.f - 2048.f; g.delta = -2048.f;
            }
            if (!g.vis) {
                if (g.opac > 0.f) g.opac = fmaxf(0.f, g.opac - s60);
            } else if (g.opac < 31.f) g.opac = fminf(31.f, g.opac + s60);
        }
        float bl = g.opac / 31.f;
        for (auto& mg : pi.meshes) mg.blend = bl;
        pi.hidden = g.opac <= 0.f;
    }
}

// ------------------------------------------------------------------
// init / free — DynamicMapFeaturesInit DS (étage + fichier inactif)
// ------------------------------------------------------------------
void World::_dw_init(int hdr) {
    if (!dw_hdr(hdr)) return;
    int old_hdr = _dw.hdr;
    _dw.on = true; _dw.hdr = hdr;
    if (!_dw.tw_arc) {
        _dw.tw_arc  = _db->narc(P_TWARC);
        _dw.tw_attr = _db->narc(P_TWATTR);
        // mapinfo : member 0 = count + infos {hdr,fidx,ox,oa,oz}
        if (_dw.tw_arc && _dw.tw_arc->count() > 0) {
            const Bytes& d = _dw.tw_arc->member(0);
            int cnt = (int32_t)u32(d, 0);
            for (int k = 0; k < cnt && 4 + k * 12 + 12 <= d.size(); k++) {
                DwSys::Info in;
                in.hdr = u32(d, 4 + k * 12); in.fidx = u16(d, 8 + k * 12);
                in.ox = (int16_t)u16(d, 10 + k * 12);
                in.oa = (int16_t)u16(d, 12 + k * 12);
                in.oz = (int16_t)u16(d, 14 + k * 12);
                _dw.infos.push_back(in);
            }
        }
    }
    if (!_dw.info(hdr)) { _dw.on = false; _dw.hdr = -1; return; }
    if (getenv("WARPDBG"))
        for (auto& in : _dw.infos)
            printf("[dw] info hdr=%d fidx=%d off=(%d,%d,%d)\n",
                in.hdr, in.fidx, in.ox, in.oa, in.oz);
    // re-init (transition streaming) : tous les props gérés par le DW
    // (fantômes + étage inactif) sont balayés — marqueur cell=-3
    for (size_t i = 0; i < _props.size();)
        if (_props[i].cell == -3) {
            for (auto& g : _props[i].meshes) rnd::free_mesh(g);
            _props.erase(_props.begin() + i);
        } else i++;
    for (auto& c : _dw.ina_cells)
        for (auto& g : c->meshes) rnd::free_mesh(g);
    _dw.ina_cells.clear(); _dw.ina_mtx = Nds::Matrix();
    // InitPersistedData DS : !valid -> defaults de l'étage
    // platform flags : B1F_1=0..B4F_1=3, B5F_1=7 ; B7F -> les 8
    bool was_valid = _dw.valid;
    if (!_dw.valid) {
        _dw.hidden_gh = 0; _dw.cur_fp = 0;
        // flags plateformes DS : B4F_1|B5F_1 (bits 3,7) ; en B7F les 8
        // ascenseurs y sont stationnés (bits 0-4,7,9,10 = 0x69F)
        _dw.plat_flags = (hdr == 581) ? 0x69F : (1 << 3) | (1 << 7);
        // boulder puzzle DS : rochers B5F, ou tutos+puits si
        // FLAG_DISTORTION_WORLD_PUZZLE_FINISHED (2477) posé
        if (!_vm.flag(2477))
            _dw.puzz_flags = (1 << 0) | (1 << 1) | (1 << 2);
        else
            _dw.puzz_flags = (1 << 10) | (1 << 11) | (1 << 12)
                           | (1 << 6) | (1 << 7) | (1 << 8);
        _dw.cax = _dw.cay = _dw.caz = 0;
        _dw.valid = true;
    }
    // fichiers map : actif (étage courant) + inactif (toujours nextID DS)
    _dw_load_file(_dw.cur, hdr);
    _dw.ina_hdr = dw_next(hdr);
    _dw_load_file(_dw.ina, _dw.ina_hdr);
    // groupes fantômes : ~defaultVisiblePropGroups à chaque activation
    // d'étage (useDefaultVisibility=TRUE DS) ; la toute première entrée
    // restaure les groupes persistés si le buffer était valide
    if (old_hdr >= 0 || !was_valid)
        _dw.hidden_gh = ~_dw.cur.gh_def & 0xFFFFFF;
    _dw_spawn_ghosts(_dw.cur);
    _dw_load_inactive();
    // debug/test : DWPUZZ/DWPLAT=<hex|dec> forcent les flags au premier
    // passage (ex. DWPUZZ=0x8 rocher tombé, DWPLAT=0x188 asc. B4F visible)
    {   static bool seeded = false;
        if (!seeded) { seeded = true;
            if (const char* e = getenv("DWPUZZ"))
                _dw.puzz_flags = (uint32_t)strtoul(e, nullptr, 0);
            if (const char* e = getenv("DWPLAT"))
                _dw.plat_flags = (uint32_t)strtoul(e, nullptr, 0); } }
    _dw_plats_spawn();                     // plateformes cur + inactif
    _dw_objs_init();                       // objets auto (flagCond non-manuel)
    _dw_spawn_simps();                     // props simples (cascade, portails)
    _dw.roll_cur = 0.f; _dw.roll_delta = 0.f; _dw.roll_steps = 0;
    // caméra DS : base -10750 ; angles persistés restaurés
    for (int k = 0; k < 3; k++) {
        int p = (k == 0 ? _dw.cax : k == 1 ? _dw.cay : _dw.caz);
        _dw.tgt[k] = p * 0x100; _dw.cang[k] = _dw.tgt[k];
    }
    _dw.tr = false;
    _dw.fp_kind = -1; _dw.fp_attr.clear();
    if (getenv("WARPDBG"))
        printf("[dw] init hdr=%d fp=%d jump=%d cam=%d gh=%d trigs=%d"
               " ina=%d flags=%03x\n",
            hdr, (int)_dw.cur.fp.size(), (int)_dw.cur.jump.size(),
            (int)_dw.cur.cam.size(), (int)_dw.cur.gh.size(),
            (int)_dw.cur.gtr.size(), _dw.ina_hdr, _dw.plat_flags);
}

// étage inactif DS : toujours connections(curr)->nextID, chargé en entier
// (les maps DW sont petites) et rendu décalé du delta d'espace partagé —
// c'est ce qui fait apparaître l'étage suivant sous/au-dessus de l'actif
void World::_dw_load_inactive() {
    if (_dw.ina_hdr < 0) return;
    auto* ci = _dw.info(_dw.hdr), *ii = _dw.info(_dw.ina_hdr);
    const MapHeader* H = _db->header(_dw.ina_hdr);
    if (!ci || !ii || !H) return;
    _dw.ina_mtx = _db->matrix(H->mtx);
    if (_dw.ina_mtx.land.empty()) return;
    // delta espace partagé -> px (ex. B1F sous 1F : -512 en y)
    Vec3 off{(ii->ox - ci->ox) * TILE, (ii->oa - ci->oa) * TILE,
             (ii->oz - ci->oz) * TILE};
    bool has_hdr = !_dw.ina_mtx.headers.empty();
    for (int i = 0; i < (int)_dw.ina_mtx.land.size(); i++) {
        if (has_hdr && _dw.ina_mtx.headers[i] != _dw.ina_hdr) continue;
        int cx = i % _dw.ina_mtx.w, cy = i / _dw.ina_mtx.w;
        auto c = _mk_cell(_dw.ina_mtx, cx, cy, off);
        if (!c) continue;
        _dw.ina_cells.push_back(c);
        // props statiques de l'étage inactif (pas de collision : ils sont
        // hors de portée — ct_world=nullptr, marqueur -3 pour le sweep)
        if (c->land)
            for (auto& lp : c->land->props) {
                PropInst pi = _prop_make(lp, c->org, -3, nullptr);
                if (pi.mdl) _props.push_back(std::move(pi));
            }
    }
    // fantômes de l'étage inactif : ~default (jamais persisté, DS
    // InitInactiveGhostPropManager)
    uint32_t hid = ~_dw.ina.gh_def & 0xFFFFFF;
    for (auto& g : _dw.ina.gh) {
        g.prop = -1;
        bool gh_hid = g.grp <= 23 && ((hid >> g.grp) & 1);
        g.vis = !gh_hid;
        if (gh_hid) { g.opac = 0.f; continue; }
        g.opac = 31.f;
        _dw_spawn_one(_dw.ina, g);
    }
    if (getenv("WARPDBG"))
        printf("[dw] inactive=%d cells=%d off=(%.0f,%.0f,%.0f)\n",
            _dw.ina_hdr, (int)_dw.ina_cells.size(), off.x, off.y, off.z);
}

void World::_dw_free() {
    if (!_dw.on) return;
    // DS : le buffer persisté vit dans la sauvegarde — on ne garde que
    // les champs persistés, le runtime est libéré
    DwSys keep;
    keep.valid = _dw.valid; keep.hidden_gh = _dw.hidden_gh;
    keep.cur_fp = _dw.cur_fp;
    keep.cax = _dw.cax; keep.cay = _dw.cay; keep.caz = _dw.caz;
    keep.plat_flags = _dw.plat_flags; keep.puzz_flags = _dw.puzz_flags;
    keep.persist_id = _dw.persist_id;
    for (auto& c : _dw.ina_cells)
        for (auto& g : c->meshes) rnd::free_mesh(g);
    _dw = keep;
}

// ------------------------------------------------------------------
// pos joueur partagée helpers déjà définis plus haut
// ------------------------------------------------------------------

// attrs de la plateforme flottante courante (tw_arc_attr[attrID],
// grille 32x32 colonne-major, relative vert/horiz selon kind)
uint16_t World::_dw_fp_attr(int tx, int ty, int tz) const {
    if (_dw.cur_fp < 0 || _dw.cur_fp >= (int)_dw.cur.fp.size())
        return 0xFFFF;
    if (_dw.fp_attr.empty()) return 0xFFFF;
    const DwFP& p = _dw.cur.fp[_dw.cur_fp];
    if (!_dw_inb(tx, ty, tz, p.b)) return 0xFFFE;
    int v = 0, h = 0;
    switch (p.kind) {
    case 0: v = tx - p.b.x; h = tz - p.b.z; break;          // FLOOR
    case 1: v = p.b.sy - (ty - p.b.y); h = tz - p.b.z; break; // WEST_WALL
    case 2: v = ty - p.b.y; h = tz - p.b.z; break;          // EAST_WALL
    case 3: v = p.b.sx - (tx - p.b.x); h = tz - p.b.z; break; // CEILING
    default: return 0xFFFF;
    }
    if (v < 0 || h < 0 || v >= 32 || h >= 32) return 0xFFFE;
    return _dw.fp_attr[v + h * p.tcv];
}

// charge la grille d'attributs de la plateforme courante (DS
// LoadFloatingPlatformTerrainAttributes)
void World::_dw_fp_load() {
    _dw.fp_attr.clear(); _dw.fp_kind = -1;
    if (_dw.cur_fp < 0 || _dw.cur_fp >= (int)_dw.cur.fp.size()) return;
    const DwFP& p = _dw.cur.fp[_dw.cur_fp];
    if (!_dw.tw_attr || p.attr >= _dw.tw_attr->count()) return;
    const Bytes& d = _dw.tw_attr->member(p.attr);
    _dw.fp_attr.assign((const uint16_t*)d.data(),
                       (const uint16_t*)d.data() + d.size() / 2);
    _dw.fp_kind = p.kind;
}

// ------------------------------------------------------------------
// caméra — DoCameraTransition / CameraTransitionTask DS
// ------------------------------------------------------------------
void World::_dw_cam_apply(const DwCam& t) {
    _dw.cax = t.ax; _dw.cay = t.ay; _dw.caz = t.az;     // persisté
    _dw.tgt[0] = t.ax * 0x100; _dw.tgt[1] = t.ay * 0x100;
    _dw.tgt[2] = t.az * 0x100;
    if (_dw.cang[0] == _dw.tgt[0] && _dw.cang[1] == _dw.tgt[1]
        && _dw.cang[2] == _dw.tgt[2]) { _dw.tr = false; return; }
    _dw.tr = true; _dw.tr_left = t.steps > 0 ? t.steps : 1;
    for (int k = 0; k < 3; k++) {
        _dw.tr_val[k] = (float)_dw.cang[k];
        // delta arc court (CalculateCameraAngleDelta DS)
        int cur = _dw.cang[k] & 0xFFFF, tgt = _dw.tgt[k] & 0xFFFF;
        int d = (int16_t)tgt - (int16_t)cur;
        int ad = d < 0 ? -d : d;
        if (ad > 0x7FFF) {
            ad = (ad + 0x7FFF) % 0x7FFF;
            if (cur < tgt) ad = -ad;
            d = ad;
        }
        _dw.tr_step[k] = (float)d / (float)_dw.tr_left;
    }
}

bool World::_dw_cam(Vec3& eye, Vec3& tgt, Vec3& up) const {
    if (!_dw.on) return false;
    // angles u16 : 0x10000 = 360°. DS : pos = R(ang) * dist + target
    auto rad = [](int u16ang) { return (float)u16ang * 6.2831853f / 65536.f; };
    float ax = rad(_dw.base[0] + _dw.cang[0]);
    float ay = rad(_dw.base[1] + _dw.cang[1]);
    float az = rad(_dw.base[2] + _dw.cang[2]);
    float dist = 666.922f;
    // AdjustPositionAroundTarget DS : y = sin(-ax)*d, x = sin(ay)*d*cos(ax)
    Vec3 off{sinf(ay) * dist * cosf(ax),
             sinf(-ax) * dist,
             cosf(ay) * dist * cosf(ax)};
    Vec3 ctp = _ppos; ctp.y += 8.f;
    eye = ctp + off;
    tgt = ctp;
    up = {sinf(az) * cosf(ay), cosf(az), -sinf(az) * sinf(ay)};
    return true;
}

// ------------------------------------------------------------------
// objets dynamiques (sMapObjectEvents DS) — localID 128+ par étage.
// x,z = tuiles partagées ; tileY en tuiles. cond : FlagCondition DS
// (0=NONE 1=PUZZLE_FALSE 2=PUZZLE_TRUE 3=PROG_EQ 4=PROG_LEQ 5=PROG_GEQ
//  6=MANUAL_ADD_ONLY 7=GIRATINA_SHADOW 8=CYRUS_APPEARANCE)
// gfx : index object_events_gfx DS ; scr : entry script (7321 = orbe)
// ------------------------------------------------------------------
const World::DwObj* World::_dw_objs_tab(int& n) const {
    static const DwObj DW_OBJS[] = {
 // 1F (573)
 {573,128,138,0, 0,6,0, 0x37,0x28,289,0},      // Cynthia portail
 {573,129,138,5, 1,4,2, 0x27,0x34,289,0},      // Cynthia élévateur
 // B1F (574)
 {574,129,264,0,1,6,0, 0xC, 0x39,257,0},      // Mesprit
 // B2F (575) — Cynthia1/2 : rotation caméra 0x5A ignorée ici
 {575,128,138,2, 0,3,4, 0x1E,0x14,233,0},      // Cynthia (SAW_MESPRIT)
 {575,128,138,2, 1,3,5, 0x1E,0x14,232,0},      // Cynthia (TALKED)
 // B3F (576)
 {576,128,120,0, 1,6,0, 0x41,0x31,193,0},      // Cyrus
 // B4F (577) — CYRUS_APPEARANCE = var 0x405B
 {577,134,120,0,2,8,0, 0x59,0x40,161,0},      // Cyrus
 // B5F (579)
 {579,128, 84,10002,1,2,0, 0x4B,0x44,129,0},  // rocher Mesprit
 {579,129, 84,10002,1,2,1, 0x62,0x43,129,0},  // rocher Azelf
 {579,130, 84,10002,1,2,2, 0x56,0x38,129,0},  // rocher Uxie
 {579,131,151,0, 1,6,0, 0x56,0x3A,113,0},     // Uxie
 {579,132,153,0, 1,6,0, 0x60,0x43,117,0},     // Azelf
 {579,133,152,0, 1,6,0, 0x4D,0x44,121,0},     // Mesprit
 // B6F (580)
 {580,128, 84,10002,1,2,3, 0x4B,0x44,115,0},  // rochers dehors
 {580,129, 84,10002,1,2,4, 0x62,0x43,115,0},
 {580,130, 84,10002,1,2,5, 0x56,0x38,115,0},
 {580,131,267,0, 1,2,15,0x44,0x40,115,0},     // trio B6F
 {580,132,265,0, 1,2,13,0x52,0x3C,115,0},
 {580,133,266,0, 1,2,14,0x60,0x47,115,0},
 {580,134,138,2, 1,3,6, 0x55,0x50,115,0},     // Cynthia (TALKED_CYRUS)
 {580,134,138,2, 3,3,7, 0x54,0x54,115,0},     // Cynthia élévateur (PUZZLE)
 {580,135,8192,8,1,0,0, 0x44,0x43,115,0},     // textes puits invisibles
 {580,136,8192,8,1,0,0, 0x4B,0x43,115,0},
 {580,137,8192,8,1,0,0, 0x4F,0x43,115,0},
 {580,138,8192,8,1,0,0, 0x55,0x34,115,0},
 {580,139,8192,8,1,0,0, 0x55,0x38,115,0},
 {580,140,8192,8,1,0,0, 0x55,0x3C,115,0},
 {580,141,8192,8,1,0,0, 0x60,0x44,115,0},
 {580,142,8192,8,1,0,0, 0x64,0x44,115,0},
 {580,143,8192,8,1,0,0, 0x69,0x44,115,0},
 {580,144, 84,0, 1,2,6, 0x44,0x42,113,0},     // rochers dans puits
 {580,145, 84,0, 1,2,7, 0x60,0x45,113,0},
 {580,146, 84,0, 1,2,8, 0x54,0x3C,113,0},
 // B7F (581)
 {581,128,138,6, 1,3,8, 0x55,0x4A,65,0},      // Cynthia initiale
 {581,128,138,6, 3,3,9, 0x54,0x4A,65,0},      // Cynthia parlant
 {581,128,138,6, 1,5,10,0x59,0x45,65,0},      // Cynthia post-combat
 {581,129,120,5, 0,4,8, 0x55,0x46,65,0},      // Cyrus initial
 {581,129,120,5, 2,3,9, 0x55,0x4A,65,0},      // Cyrus parlant
 // salle Giratina (582)
 {582,128,230,5, 1,3,13,0xF, 0xD, 1,0},       // Giratina (ARRIVED)
 {582,129,138,0, 1,6,0, 0xF, 0x17, 1,0},      // Cynthia
 {582,130,120,0, 1,6,0, 0xF, 0x17, 1,0},      // Cyrus
 {582,131,8192,3,1,3,14,0xF, 0xD, 1,0},       // portail (BATTLED)
 {582,132,8192,6,1,3,14,0xF, 0xF, 1,0},       // Cynthia texte (15,1,15)
 // grotte Retour (583)
 {583,128,8192,2,1,0,0, 0x74,0x4A,65,0},      // portail
 {583,129, 87,7321,1,0,0, 0x59,0x37,65,0},    // Orbe Platiné
    };
    n = (int)(sizeof(DW_OBJS) / sizeof(DW_OBJS[0]));
    return DW_OBJS;
}

bool World::_dw_obj_cond(const DwObj& o) const {
    switch (o.cond) {
    case 0: return true;
    case 1: return !((_dw.puzz_flags >> o.cval) & 1);
    case 2: return (_dw.puzz_flags >> o.cval) & 1;
    case 3: return _vm.var(0x4055) == o.cval;   // VAR_DISTORTION_WORLD_PROGRESS
    case 4: return _vm.var(0x4055) <= o.cval;
    case 5: return _vm.var(0x4055) >= o.cval;
    case 6: return false;                     // MANUAL_ADD_ONLY
    case 8: return _vm.var(0x405B) == o.cval; // CYRUS_APPEARANCE (var==val)
    default: return false;
    }
}

// AddMapObjectsForMap DS : spawn auto des objets non-manuels à l'init
// (aussi relancé par le field-return post-combat -> dédupe par localID)
void World::_dw_objs_init() {
    if (!_dw.on) return;
    int n; const DwObj* t = _dw_objs_tab(n);
    for (int k = 0; k < n; k++) {
        const DwObj& o = t[k];
        if (o.hdr == _header && o.cond != 6 && _dw_obj_cond(o)
            && !(o.hflag && _vm.flag(o.hflag)) && !_find_obj(o.id))
            _dw_obj_spawn(o);
    }
}

int World::_dw_obj_spawn(const DwObj& o) {
    auto* i = _dw.info(o.hdr);
    if (!i) return -1;
    Nds::Zev::Obj no{};
    no.id = o.id; no.gfx = o.gfx; no.move = 0; no.trainer = 0;
    no.flag = o.hflag; no.script = o.scr;
    no.dir = o.dir; no.d0 = no.d1 = no.d2 = 0; no.rx = no.rz = 0;
    no.x = (uint16_t)(o.x - i->ox); no.z = (uint16_t)(o.z - i->oz);
    // rochers tombés de B5F : la pos d'atterrissage enregistrée prime sur
    // le template (le rocher DS garde sa tuile en changeant de zone)
    if (o.hdr == 580 && o.id >= 128 && o.id <= 130
        && _dw.bld_pos[o.id - 128][0] >= 0)
        no.x = (uint16_t)(_dw.bld_pos[o.id - 128][0] - i->ox),
        no.z = (uint16_t)(_dw.bld_pos[o.id - 128][1] - i->oz);
    no.y = (uint32_t)(o.ty - i->oa) * TILE * 4096;   // fx32 local
    int idx = (int)_objects.size();
    _objects.push_back(no);
    _objpos.push_back({(float)no.x * TILE + TILE / 2.f,
                       (float)(o.ty - i->oa) * TILE,
                       (float)no.z * TILE + TILE / 2.f});
    static const int M[4] = {1, 0, 2, 3};            // dir ROM -> monde
    _objd.push_back(M[o.dir & 3]);
    _objmv.push_back(0);
    _objw.push_back({});
    _objw.back().home = _objpos.back();
    if (getenv("WARPDBG"))
        printf("[dw] obj id=%d gfx=%d scr=%d local=(%d,%d) y=%d\n",
            o.id, o.gfx, o.scr, no.x, no.z, o.ty - i->oa);
    return idx;
}

// DistWorld_AddMapObjectWithLocalID DS : isManual=TRUE -> cond 6
// toujours, sinon la flagCond est évaluée (variantes par progression)
void World::_dw_obj_add(int local_id) {
    if (!_dw.on) return;
    int n; const DwObj* t = _dw_objs_tab(n);
    for (int k = 0; k < n; k++) {
        const DwObj& o = t[k];
        if (o.hdr == _header && o.id == local_id
            && (o.cond == 6 || _dw_obj_cond(o)))
            _dw_obj_spawn(o);
    }
}

void World::_dw_obj_del(int local_id) {
    if (!_dw.on) return;
    for (size_t i = 0; i < _objects.size(); i++)
        if (_objects[i].id == local_id) {
            _objects.erase(_objects.begin() + i);
            _objpos.erase(_objpos.begin() + i);
            _objd.erase(_objd.begin() + i);
            _objmv.erase(_objmv.begin() + i);
            _objw.erase(_objw.begin() + i);
            // les sets d'index et le rocher suivi glissent d'un cran
            if (_bpush.obj > (int)i) _bpush.obj--;
            else if (_bpush.obj == (int)i) _bpush.obj = -1;
            auto shift = [i](std::unordered_set<int>& s) {
                std::unordered_set<int> n;
                for (int k : s)
                    if (k < (int)i) n.insert(k);
                    else if (k > (int)i) n.insert(k - 1);
                s = std::move(n);
            };
            shift(_objgone); shift(_objhide); shift(_objpass);
            return;
        }
}

// sBoulderFallLocations DS (table FALLS) : tuiles partagées ->
// type de chute. pit -2 = trou B5F->B6F, -1 = mauvais puits, 6/7/8 =
// puits corrects (flag DIST_WORLD_PUZZLE_FLAG_*_IN_B6F_PIT).
int World::_dw_fall_at(int stx, int stz, int* pit) const {
    struct FallLoc { uint16_t hdr; int8_t x, z, pit; };
    static const FallLoc FALLS[] = {
        {579, 0x4c, 0x44, -2}, {579, 0x61, 0x43, -2}, {579, 0x56, 0x39, -2},
        {580, 0x44, 0x43, 6},  {580, 0x4b, 0x43, -1}, {580, 0x4f, 0x43, -1},
        {580, 0x60, 0x44, 7},  {580, 0x64, 0x44, -1}, {580, 0x69, 0x44, -1},
        {580, 0x55, 0x3c, 8},  {580, 0x55, 0x38, -1}, {580, 0x55, 0x39, -1},
        {580, 0x55, 0x34, -1}, {580, 0x55, 0x35, -1},
    };
    for (auto& f : FALLS)
        if (f.hdr == _header && f.x == stx && f.z == stz) {
            if (f.pit == -2) return 1;           // trou -> descente B6F
            if (f.pit >= 0 && !((_dw.puzz_flags >> f.pit) & 1)) {
                *pit = f.pit; return 2;          // puits vide : juste
            }
            return 3;                            // mauvais/déjà rempli
        }
    return 0;
}

// BOULDERS par localId : bits puzzle (sur B5F / dehors B6F),
// objet puits, bit gardien-en-B6F, script gardien (1-indexé :
// ScriptEntry 4/5/6 -> 5/7/6).
struct DwBoulder { int b5f, outside, pit_obj, guard, scr; };
static DwBoulder dw_boulder(int id) {
    switch (id) {
    case 128: return {0, 3, 144, 15, 5};   // Mesprit
    case 129: return {1, 4, 145, 14, 7};   // Azelf
    case 130: return {2, 5, 146, 13, 6};   // Uxie
    }
    return {-1, -1, -1, -1, -1};
}

// ov5_021DFF1C + DistWorldFallingBoulder_* DS : glisse du rocher poussé
// (WALK_SLOW = 16 ticks/tuile), puis chute éventuelle — trou B5F vers
// B6F (-8px/frame), puits juste (tipOver, secousse, scène gardien) ou
// mauvais puits (disparition, retour B5F).
void World::_boulder_tick(float dt) {
    if (_bpush.obj < 0) return;
    int oi = _bpush.obj;
    if (oi >= (int)_objects.size() || _objgone.count(oi)) {
        _bpush = BoulderPush{}; return;
    }
    float ft = dt * 30.f;                      // ticks DS écoulés

    // ── glisse de la poussée ──
    if (_bpush.fall == 0) {
        _bpush.t += ft / 16.f;
        float t = _bpush.t > 1.f ? 1.f : _bpush.t;
        _objpos[oi] = _bpush.from + (_bpush.to - _bpush.from) * t;
        if (_bpush.t < 1.f) return;
        if (_dw.on) {                          // boulderFall DS
            auto* i = _dw.info(_header);
            int pit = -1;
            int fl = i ? _dw_fall_at(
                (int)floorf(_objpos[oi].x / TILE) + i->ox,
                (int)floorf(_objpos[oi].z / TILE) + i->oz, &pit) : 0;
            if (fl && dw_boulder(_objects[oi].id).b5f >= 0) {
                _bpush.fall = fl; _bpush.pit = pit;
                _bpush.bid = _objects[oi].id;   // localId avant swap puits
                _bpush.ph = 0; _bpush.tm = 0;
                _bpush.base_y = _objpos[oi].y;
                if (getenv("WARPDBG"))
                    printf("[dw] rocher id=%d chute=%d pit=%d\n",
                        _objects[oi].id, fl, pit);
                return;
            }
        }
        _bpush = BoulderPush{};                // poussée simple terminée
        return;
    }

    // ── chutes ──
    Vec3& p = _objpos[oi];
    int dx = _bpush.dir == 3 ? 1 : _bpush.dir == 2 ? -1 : 0;
    int dz = _bpush.dir == 0 ? 1 : _bpush.dir == 1 ? -1 : 0;
    DwBoulder b = dw_boulder(_bpush.bid);        // localId d'origine (stable)

    if (_bpush.fall == 1) {                    // toB6F : -8px/frame
        p.y -= 8.f * ft;
        auto* ci = _dw.info(_header);
        if (p.y / TILE + (ci ? ci->oa : 0) <= 115.f) {  // B6F_HEIGHT
            int sx = (int)floorf(p.x / TILE) + (ci ? ci->ox : 0);
            int sz = (int)floorf(p.z / TILE) + (ci ? ci->oz : 0);
            _dw.puzz_flags = (_dw.puzz_flags | (1u << b.outside))
                & ~(1u << b.b5f);
            _dw.bld_pos[_objects[oi].id - 128][0] = sx;
            _dw.bld_pos[_objects[oi].id - 128][1] = sz;
            _objgone.insert(oi);               // rejoint la zone B6F
            if (_snd) {
                int s = _snd->seq_named("SEQ_SE_DP_UG_008");
                if (s >= 0) _snd->se(s);
            }
            if (getenv("WARPDBG"))
                printf("[dw] rocher %d -> B6F (%d,%d) flags=%x\n",
                    _objects[oi].id, sx, sz, _dw.puzz_flags);
            _bpush = BoulderPush{};
        }
        return;
    }

    // tipOver DS : 8f à -2px y/+2px xz puis 4f à -4px y (bascule dans le
    // puits en glissant d'une tuile dans la direction poussée)
    if (_bpush.ph == 0) {
        if (_bpush.tm < 8.f) {
            p.y -= 2.f * ft;
            p.x += dx * 2.f * ft;
            p.z += dz * 2.f * ft;
        } else p.y -= 4.f * ft;
        _bpush.tm += ft;
        if (_bpush.tm >= 12.f) {
            p.x = _bpush.to.x + dx * TILE;     // 1 tuile dans le puits
            p.z = _bpush.to.z + dz * TILE;
            p.y = _bpush.base_y - 32.f;
            _bpush.ph = 1; _bpush.tm = 0;
            if (_bpush.fall == 2) {
                _dw.puzz_flags = (_dw.puzz_flags | (1u << _bpush.pit))
                    & ~(1u << b.outside);
                _objects[oi].id = b.pit_obj;   // localId -> objet puits
                _objpass.insert(oi);           // puits comblé : marchable DS
                if (_snd) {
                    int s = _snd->seq_named("SEQ_SE_DP_UG_008");
                    if (s >= 0) _snd->se(s);
                }
            }
        }
        return;
    }

    if (_bpush.fall == 2) {
        if (_bpush.ph == 1) {                  // secousse 32f : ±1px
            int st = (int)_bpush.tm;
            p.y = _bpush.base_y - 32.f
                + (st == 7 || st >= 32 ? 0.f : (st & 1) ? -1.f : 1.f);
            _bpush.tm += ft;
            if (st >= 32) {
                // le gardien quitte B6F : sa scène efface son objet
                _dw.puzz_flags &= ~(1u << b.guard);
                _start_script(b.scr); _vm.step();
                _bpush.ph = 2;
                if (getenv("WARPDBG"))
                    printf("[dw] puits %d rempli, script %d\n",
                        _bpush.pit, b.scr);
            }
            return;
        }
        if (!_vm.busy()) {                     // scène gardien terminée
            if ((_dw.puzz_flags & 0x1C0) == 0x1C0)      // puits 6|7|8
                _vm.set_flag(2477, true);       // PUZZLE_FINISHED
            _bpush = BoulderPush{};
        }
        return;
    }

    // fall == 3 : mauvais puits — le rocher coule 40 frames puis
    // réapparaît sur B5F (bit b5f reposé, outside effacé)
    p.y -= 8.f * ft;
    _bpush.tm += ft;
    if (_bpush.tm >= 40.f) {
        _dw.puzz_flags = (_dw.puzz_flags | (1u << b.b5f))
            & ~(1u << b.outside);
        _objgone.insert(oi);
        _bpush = BoulderPush{};
        if (getenv("WARPDBG"))
            printf("[dw] mauvais puits, rocher %d retour B5F flags=%x\n",
                b.pit_obj - 16, _dw.puzz_flags);
    }
}


// DistortionWorld_DynamicMapFeatures DS : à chaque tuile atteinte —
// ascenseur -> event -> téléports, puis triggers fantômes, caméra,
// points de saut, cascades ; enfin plateforme flottante courante
// commandes d'events de tuile (sMapEvents DS) — tables plus bas
enum {
    EV_END = 0, EV_MOVE, EV_JUMP, EV_SCR, EV_PROG, EV_ADDOBJ, EV_DELOBJ,
    EV_PSET, EV_PCLR, EV_ANIMFLAG, EV_SHADOW, EV_TUTO, EV_ARRIVAL,
    EV_SHOWGP, EV_HIDEGP, EV_CASC
};
struct DwEvCmd { uint8_t k; int16_t a, b, c, d, e, f; };
// cascades : B4F descente / B5F remontée (simplifié : glisse verticale)
static const DwEvCmd EV_CASC_DN[] = {
    {EV_CASC, -1, -0x298, -0x15, -0x2A, 2, 24},
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_CASC_UP[] = {
    {EV_CASC, 1, 0x298, 0x14, 0x2A, 8, 24},
    {EV_END, 0, 0, 0, 0, 0, 0},
};

void World::_dw_moved(int dir) {
    if (!_dw.on) return;
    if (_dw_busy()) {
        if (getenv("WARPDBG"))
            printf("[dw] moved SKIP busy elev=%d ev=%p jump=%.0f\n",
                _dw.elev_st, (void*)_dw.ev_cmds, _dw.jump_t);
        return;
    }
    static const int W2R[4] = {1, 0, 2, 3};   // dir monde -> DS (N,S,W,E)
    int dsd = W2R[dir & 3];
    int tx = _dw_shared_x(), ty = _dw_shared_y(), tz = _dw_shared_z();
    if (getenv("WARPDBG"))
        printf("[dw] moved dir=%d shared=(%d,%d,%d)\n",
            dir, tx, ty, tz);
    // 1) ascenseur : tuile exacte d'une plateforme dir!=NONE. Si le
    // joueur est en chute vers la plateforme (pas fini avant le snap),
    // plat_pend arme un retry par _dw_tick pendant l'atterrissage.
    int pi = _dw.elev_skip ? -1 : _dw_plat_at(tx, ty, tz);
    if (pi >= 0) { _dw.plat_pend = 0; _dw_elev_begin(pi); return; }
    _dw.plat_pend = _dw.elev_skip ? 0 : 24;   // ~0.4 s de chute max
    // 2) event de tuile
    if (_dw_ev_at(tx, ty, tz)) return;
    // 3) téléports DS : B7F (face N, progress>=WON_CYRUS, 89/65/56-57)
    //    -> script 2 ; salle Giratina (face S, 15/1/25-26) -> script 4
    if (_header == 581 && dsd == 0 && _vm.var(0x4055) >= 10
        && tx == 89 && ty == 65 && (tz == 56 || tz == 57)) {
        _start_script(2); _vm.step(); return;
    }
    if (_header == 582 && dsd == 1
        && tx == 15 && ty == 1 && (tz == 25 || tz == 26)) {
        _start_script(4); _vm.step(); return;
    }
    // 4) triggers props fantômes (step vers la tuile)
    bool gh_chg = false;
    for (auto& t : _dw.cur.gtr)
        if (dsd == t.dir && _dw_inb(tx, ty, tz, t.b)) {
            if (t.show) _dw.hidden_gh &= ~(1u << t.grp);
            else _dw.hidden_gh |= (1u << t.grp);
            gh_chg = true;
        }
    if (gh_chg) _dw_ghost_apply(_dw.cur);
    // 5) caméra : template par bounds + direction
    for (auto& c : _dw.cur.cam)
        if (dsd == c.dir && _dw_inb(tx, ty, tz, c.b)) {
            _dw_cam_apply(c); break;
        }
    // 6) points de saut -> tâche de saut
    for (auto& j : _dw.cur.jump)
        if (dsd == j.dir && _dw_inb(tx, ty, tz, j.b)) {
            _dw_jump_start(j); return;
        }
    // 7) cascades : B4F (face E, x=104 y=170 z 76-79) descend vers B5F ;
    //    B5F (face E, x=104 y=128 z 76-79) remonte vers B4F
    if (_header == 577 && dsd == 3
        && tx == 104 && ty == 170 && tz >= 76 && tz <= 79) {
        _dw.ev_cmds = EV_CASC_DN; _dw.ev_ci = 0; _dw.ev_ph = 0;
        return;
    }
    if (_header == 579 && dsd == 3
        && tx == 104 && ty == 128 && tz >= 76 && tz <= 79) {
        _dw.ev_cmds = EV_CASC_UP; _dw.ev_ci = 0; _dw.ev_ph = 0;
        return;
    }
    // 8) plateforme courante : FindAndPrepareNewCurrentFloatingPlatform
    for (int i = 0; i < (int)_dw.cur.fp.size(); i++)
        if (i != _dw.cur_fp && _dw_inb(tx, ty, tz, _dw.cur.fp[i].b)) {
            _dw.cur_fp = i; _dw_fp_load();
            break;
        }
}

// DS HandlePlayerMoved : le point de saut se déclenche à la PRESSION sur
// la direction depuis la tuile trigger — pas après le pas (sinon le retour
// mur -> terre, dont le pas serait bloqué hors de la paroi, ne part pas).
bool World::_dw_jump_press(int wdir) {
    static const int W2R[4] = {1, 0, 2, 3};
    int dsd = W2R[wdir & 3];
    int tx = _dw_shared_x(), ty = _dw_shared_y(), tz = _dw_shared_z();
    for (auto& j : _dw.cur.jump)
        if (dsd == j.dir && _dw_inb(tx, ty, tz, j.b))
            return _dw_jump_start(j);
    return false;
}

// changement de position hors pas (warp/téléport/script) : mêmes tests
void World::_dw_pos_changed() {
    if (!_dw.on || _dw_busy()) return;
    _dw_moved(_pdir);
}

// saut sur plateforme : déplace le joueur de (dx,dy,dz) tuiles en
// steps ticks — DS JumpOnFloatingPlatform
bool World::_dw_jump_start(const DwJump& j) {
    if (_dw.jump_t > 0 || j.steps <= 0) return false;
    Vec3 from = _ppos;
    Vec3 to{from.x + j.dx * TILE, from.y + j.dy * TILE,
            from.z + j.dz * TILE};
    _dw.jump_from = from; _dw.jump_to = to;
    _dw.jump_steps = j.steps; _dw.jump_t = j.steps;
    _dw.jump_axis = j.axis; _dw.jump_fdir = j.fdir;
    _dw.jump_prog = 0.f;
    // RotateMapObject DS : le sprite tourne de j.rot degrés étalés sur
    // j.steps frames (cumulatif — pas un angle par surface)
    if (j.steps > 0) {
        _dw.roll_delta = (float)j.rot / (float)j.steps;
        _dw.roll_steps = j.steps;
    }
    // fp = plateforme cible DS ; 0xFFFF = atterrissage sur la terre ferme
    // (support null : plus de plateforme courante, plus de grille attrs).
    if (j.fp == 0xFFFF) {
        _dw.cur_fp = -1;
        _dw_fp_load();
    } else if (j.fp < _dw.cur.fp.size()) {
        _dw.cur_fp = j.fp;
        _dw_fp_load();
    }
    return true;
}

// tick système — transition caméra + saut plateforme + trajets/events
void World::_dw_tick(float dt) {
    if (!_dw.on) return;
    float s60 = dt * 60.f;                 // pas par frame DS (60 Hz)
    if (_dw.tr) {
        _dw.tr_left -= s60;
        if (_dw.tr_left <= 0) {
            _dw.tr = false;
            for (int k = 0; k < 3; k++) _dw.cang[k] = _dw.tgt[k];
        } else {
            for (int k = 0; k < 3; k++) {
                _dw.tr_val[k] += _dw.tr_step[k] * s60;
                _dw.cang[k] = (int)_dw.tr_val[k];
            }
        }
    }
    if (_dw.jump_t > 0) {
        _dw.jump_t -= s60;
        _dw.jump_prog = 1.f - (float)_dw.jump_t / (float)_dw.jump_steps;
        float t = _dw.jump_prog;
        _ppos.x = _dw.jump_from.x + (_dw.jump_to.x - _dw.jump_from.x) * t;
        _ppos.z = _dw.jump_from.z + (_dw.jump_to.z - _dw.jump_from.z) * t;
        _ppos.y = _dw.jump_from.y + (_dw.jump_to.y - _dw.jump_from.y) * t
                + sinf(t * 3.14159265f) * TILE * 0.5f;   // arc de saut
        if (_dw.jump_t <= 0) {
            _ppos = _dw.jump_to;
            if (_dw.jump_fdir >= 0 && _dw.jump_fdir <= 3) {
                _pdir = _dw.jump_fdir;
                _pface = _dw.jump_fdir;   // facing dessiné DS (JUMP_FACING)
            }
        }
    }
    // atterrissage différé sur un élévateur : le pas s'est fini en
    // chute libre vers la plateforme — dès que sa surface est atteinte
    // (tuile partagée + portée), le trajet démarre.
    if (_dw.plat_pend > 0 && !_dw_busy() && _pstep == 0) {
        _dw.plat_pend -= s60;
        int pi = _dw_plat_on(_ppos.y);
        // DS exige la tuile template exacte : on snappe seulement
        // l'atterrissage ; le trajet démarre quand le joueur rejoint
        // la tuile de déclenchement (marché ou pas).
        if (pi >= 0) {
            const DwSys::DwPlat& p = _dw.plats[pi];
            float s = _dw_plat_surf(p);
            if (_ppos.y <= s + 2.f) _ppos.y = s;
            if (p.dir != 2 && _dw_shared_x() == p.tx
                && _dw_shared_z() == p.tz) {
                _dw.plat_pend = 0;
                _ppos.y = s;                      // atterrissage
                _dw_elev_begin(pi);
            }
        }
    }
    // trajets élévateur + file d'events : logique exprimée en px/frame DS —
    // sous-pas 60 Hz pour rester fidèle quel que soit le fps hôte
    _dw.elev_acc += s60;
    while (_dw.elev_st > 0 && _dw.elev_acc >= 1.f) {
        _dw.elev_acc -= 1.f; _dw_elev_tick();
    }
    _dw.ev_acc += s60;
    while (_dw.ev_cmds && _dw.ev_acc >= 1.f) {
        _dw.ev_acc -= 1.f; _dw_ev_tick();
    }
    _dw_ghost_tick(_dw.cur, s60);          // props fantômes : bob + fondu
    _dw_ghost_tick(_dw.ina, s60);          // ceux de l'étage inactif aussi
    _dw_shadow_tick(s60);                  // ombre Giratina : glisse + sfx
    _dw_arrival_tick(s60);                 // arrivée : descente + ciel forcé
    // marche Cyrus B4F (MapObject_StartAnimation DS) : 1 px/frame,
    // est*(x-88) puis nord*4 ; fin -> DeleteMapObject + CYRUS_APPEARANCE=1
    if (_dw.cy_obj >= 0) {
        if (_dw.cy_obj < (int)_objpos.size()) {
            Vec3& cp = _objpos[_dw.cy_obj];
            float st = fminf(s60, _dw.cy_left);
            if (_dw.cy_leg == 0) cp.x += st; else cp.z -= st;
            _objd[_dw.cy_obj] = _dw.cy_leg == 0 ? 3 : 1;
            _dw.cy_left -= st;
            if (_dw.cy_left <= 0.f) {
                if (_dw.cy_leg == 0) {
                    _dw.cy_leg = 1; _dw.cy_left = 4 * TILE;
                } else {
                    _dw_obj_del(134);
                    _vm.set_var(0x405B, 1);
                    _dw.cy_obj = -1;
                    if (getenv("WARPDBG")) printf("[dw] cyrus fin\n");
                }
            }
        } else { _dw.cy_obj = -1; _vm.set_var(0x405B, 1); }
    }
    // plateformes salle Giratina : ShowGhostPropGroup/HideGhostPropGroup
    // DS avec leurs délais (36 + 48/groupe ; 16 + 48/groupe + attente 8)
    if (_dw.gp_st) {
        _dw.gp_delay -= s60;
        if (getenv("GPDDBG") && _dbg_frames % 30 == 0)
            printf("[gp] st=%d grp=%d dl=%.1f done=%d ev=%p ci=%d\n",
                _dw.gp_st, _dw.gp_grp, _dw.gp_delay, (int)_dw.gp_done,
                (void*)_dw.ev_cmds, _dw.ev_ci);
        if (_dw.gp_delay <= 0) {
            int s = _snd ? _snd->seq_named("SEQ_SE_PL_SYUWA3") : -1;
            if (_dw.gp_st == 1) {                       // show grp 1->3
                if (s >= 0) _snd->se_stop(s);
                _dw.hidden_gh &= ~(1u << _dw.gp_grp);
                _dw_ghost_apply(_dw.cur);
                if (++_dw.gp_grp > 3) {
                    _dw.gp_st = 0; _dw.gp_done = true;
                } else _dw.gp_delay = 48.f;
            } else if (_dw.gp_st == 2) {                // hide grp 3->1
                if (_dw.gp_grp >= 1) {
                    if (s >= 0) _snd->se_stop(s);
                    _dw.hidden_gh |= 1u << _dw.gp_grp;
                    _dw_ghost_apply(_dw.cur);
                    _dw.gp_grp--; _dw.gp_delay = 48.f;
                } else { _dw.gp_st = 3; _dw.gp_delay = 8.f; }
            } else { _dw.gp_st = 0; _dw.gp_done = true; }
        }
    }
    // props DS animés à FX32_ONE/frame (60 fps) : le tick générique des
    // props avance déjà à 30 fps — la moitié manquante est ajoutée ici
    for (auto& pi : _props) {
        if (pi.cell != -3) continue;
        if (pi.tanms) pi.tanm_frame += dt * 30.f;
        if (pi.anm && !pi.anm_hold) pi.anm_frame += dt * 30.f;
    }
    // pierres de la salle Giratina : apparition en fondu (step>>1/31)
    for (auto& p : _dw.plats) {
        if (p.stones < 0) {
            if (p.hdr == 580 && p.idx == 1 && _vm.flag(2423)) p.stones = 0;
            continue;
        }
        if (p.stones < 62.f) p.stones += s60;
        if (p.stones > 62.f) p.stones = 62.f;
        if (p.prop >= 0 && p.prop < (int)_props.size()) {
            float bl = fminf(31.f, floorf(p.stones * .5f)) / 31.f;
            for (auto& mg : _props[p.prop].meshes) mg.blend = bl;
        }
    }
    // roll sprite : RotateMapObject DS — avance roll_delta par frame puis
    // fige (la valeur de surface vient de l'accumulation des sauts)
    if (_dw.roll_steps > 0) {
        float adv = fminf(s60, _dw.roll_steps);
        _dw.roll_cur += _dw.roll_delta * adv;
        _dw.roll_steps -= adv;
        if (_dw.roll_steps <= 0) {
            _dw.roll_steps = 0;
            _dw.roll_cur = fmodf(fmodf(_dw.roll_cur, 360.f) + 360.f, 360.f);
        }
    }
    // ciel : kind salle Giratina (progress 10-12 tourne, 13 assombri) +
    // orbites des nuages (fx32 deg/frame DS) + assombrissement selon la
    // hauteur du joueur (SKY_BACKGROUND_MAX_DARKNESS ov9)
    {
        static const float GDEL[3][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {0.5f, 1.5f, 3.f, 5.f},           // salle Giratina (Cyrus battu)
            {-2.f, -4.5f, -7.f, -12.5f},      // salle sombre (Giratina venu)
        };
        int p = _vm.var(0x4055);
        _dw.sky_kind = (_dw.hdr == 582)
            ? (p >= 10 && p <= 12 ? 1 : (p >= 10 && p <= 13 ? 2 : 0)) : 0;
        for (auto& c : _dw.sky_cl) {
            float want = GDEL[_dw.sky_kind][c.grp & 3];
            if (want < c.delta) c.delta = fmaxf(want, c.delta - 0.125f * s60);
            else if (want > c.delta)
                c.delta = fminf(want, c.delta + 0.125f * s60);
            c.ang += (c.spd + c.delta) * s60;
            c.ang = fmodf(fmodf(c.ang, 360.f) + 360.f, 360.f);
        }
    }
    {
        // RecalculateSkyBackgroundDarkness DS : fixedDarkness (arrivée
        // Giratina) ou niveau entier par hauteur — (y_px - 65*16) /
        // trunc(224*16/12) px par niveau, borné [0,12]
        float sty = (float)_dw_shared_y();
        float nd;
        if (_dw.sky_fix >= 0) nd = (float)_dw.sky_fix;
        else {
            float lv = (sty * 16.f - 65.f * 16.f) / 298.f;
            nd = lv < 0.f ? 0.f : lv > 12.f ? 12.f : floorf(lv);
        }
        if (getenv("WARPDBG") && nd != _dw.sky_dark)
            printf("[dw] sky dark %.1f (ty=%.0f)\n", nd, sty);
        _dw.sky_dark = nd;
    }
}

// ------------------------------------------------------------------
// ciel (tw_arc_etc.narc — InitSkyBackground + InitSkyCloudAnimators ov9)
// fond BG2 256px tuilé teinté vers GX_RGB(4,4,8) selon la hauteur ;
// 9 nuages sprites en orbite autour de points de l'écran DS.
// ------------------------------------------------------------------
// sSkyCloudsTemplates DS : set, groupe, angle0, rot sprite, rayon orbite
// (px), vitesse (deg/frame), centre x/y, échelle.
struct DwSkyT { int set, grp; float ang, off, rad, spd, cx, cy, scl; };
static const DwSkyT DW_SKY[] = {
    {6, 3,   0, 135, 104, 1.25f, 128, 108, 2},
    {6, 3, 180, 135, 104, 1.25f, 128, 108, 2},
    {6, 3,  90, 135, 104, 1.25f, 128, 108, 2},
    {6, 3, 270, 135, 104, 1.25f, 128, 108, 2},
    {1, 2,   0,  90,  60, 1.00f, 128, 128, 1},
    {2, 2, 180,  90,  60, 1.00f, 128, 128, 1},
    {3, 1, 315,  90,  42, 0.75f, 128, 128, 1},
    {4, 1, 135,  90,  42, 0.75f, 128, 128, 1},
    {5, 0,   0,  90,   0, 0.50f, 128, 128, 1},
};

void World::_dw_sky_load() {
    _dw.sky_ok = true;                            // une seule tentative
    auto n = _db->narc(P_TWETC);
    if (!n || n->count() < 25) return;
    // quad écran : verts en px DS (0..256 x 0..192), uv en tuiles 256px
    auto quad = [](float x0, float y0, float x1, float y1,
                   float us, float z) {
        float u0 = x0 / us, u1 = x1 / us, v0 = y0 / us, v1 = y1 / us;
        float v[] = {
            x0,y0,z, 0,0,1, 1,1,1,1, u0,v0,   x1,y0,z, 0,0,1, 1,1,1,1, u1,v0,
            x0,y1,z, 0,0,1, 1,1,1,1, u0,v1,   x1,y0,z, 0,0,1, 1,1,1,1, u1,v0,
            x1,y1,z, 0,0,1, 1,1,1,1, u1,v1,   x0,y1,z, 0,0,1, 1,1,1,1, u0,v1,
        };
        return std::vector<float>(v, v + sizeof v / sizeof *v);
    };
    // fond BG2 : NCGR 0 + NCLR 1 + NSCR 2 -> image 256px tuilée
    auto g0 = spr::parse_ncgr(n->member(0));
    auto p0 = spr::parse_nclr(n->member(1));
    auto bg = spr::render_screen(n->member(2), g0, p0);
    if (getenv("SKYDUMP") && bg &&
        bg->px.size() >= (size_t)bg->w * bg->h * 4) {
        FILE* fp = fopen("_shots/sky_bg.ppm", "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", bg->w, bg->h);
            for (size_t k = 0; k < (size_t)bg->w * bg->h; k++)
                fputc(bg->px[k*4], fp), fputc(bg->px[k*4+1], fp),
                    fputc(bg->px[k*4+2], fp);
            fclose(fp);
        }
    }
    if (bg) {
        // rect plus large que l'écran : la tuile 256px répète via REPEAT
        auto v = quad(-256, -192, 512, 384, 256.f, 0.9999f);
        _dw.sky_bg_m = rnd::upload(v.data(), 6,
            rnd::upload_tex_nearest(*bg));
        _dw.sky_bg_m.lit = 0;
    }
    // 7 sets de nuages : NCER 3+3k + NCGR 4+3k, palette commune membre 24
    auto pals = spr::parse_nclr(n->member(24));
    for (int k = 0; k < 7; k++) {
        auto cells = spr::parse_ncer(n->member(3 + 3 * k));
        auto gk = spr::parse_ncgr(n->member(4 + 3 * k));

        auto im = cells.empty() ? std::shared_ptr<Image>{}
                              : spr::render_cell(gk, cells[0], pals);
        if (verbose)
            printf("[dw]   cloud%d cells=%d oams=%d x[%d..%d] y[%d..%d] tiles=%d pals=%d im=%d\n",
                k, (int)cells.size(),
                cells.empty() ? -1 : (int)cells[0].oams.size(),
                cells.empty() ? 0 : cells[0].minx, cells.empty() ? 0 : cells[0].maxx,
                cells.empty() ? 0 : cells[0].miny, cells.empty() ? 0 : cells[0].maxy,
                (int)gk.tiles.size(), (int)pals.size(), im ? 1 : 0);
        if (getenv("SKYDUMP") && im &&
            im->px.size() >= (size_t)im->w * im->h * 4) {
            char nm[64]; snprintf(nm, sizeof nm, "_shots/sky_cl%d.ppm", k);
            FILE* fp = fopen(nm, "wb");
            if (fp) {
                fprintf(fp, "P6\n%d %d\n255\n", im->w, im->h);
                for (size_t q = 0; q < (size_t)im->w * im->h; q++)
                    fputc(im->px[q*4], fp), fputc(im->px[q*4+1], fp),
                        fputc(im->px[q*4+2], fp);
                fclose(fp);
            }
        }
        if (!im) continue;
        // rect px du cell autour de l'origine du sprite (rot. au centre) —
        // l'image rendue couvre exactement ce rect : UV 0..1 (pas de tuile)
        {
            float x0 = cells[0].minx, y0 = cells[0].miny,
                  x1 = cells[0].maxx, y1 = cells[0].maxy;
            float v[] = {
                x0,y0,0.9998f, 0,0,1, 1,1,1,1, 0,0,
                x1,y0,0.9998f, 0,0,1, 1,1,1,1, 1,0,
                x0,y1,0.9998f, 0,0,1, 1,1,1,1, 0,1,
                x1,y0,0.9998f, 0,0,1, 1,1,1,1, 1,0,
                x1,y1,0.9998f, 0,0,1, 1,1,1,1, 1,1,
                x0,y1,0.9998f, 0,0,1, 1,1,1,1, 0,1,
            };
            uint32_t ct = rnd::upload_tex_nearest(*im);
            _dw.sky_cm[k] = rnd::upload(v, 6, ct);
            _dw.sky_cm[k].lit = 0;
            _dw.sky_cm[k].alpha_cut = true;
            _dw.sky_ct[k] = true;
            continue;
        }
    }
    for (auto& t : DW_SKY)
        _dw.sky_cl.push_back({t.set, t.grp, t.cx, t.cy, t.rad, t.spd,
                              t.off, t.scl, t.ang, 0.f});
    if (verbose)
        printf("[dw] sky bg=%d clouds=%d\n",
            _dw.sky_bg_m.vao != 0, (int)_dw.sky_cl.size());
}

void World::_dw_sky_draw() {
    if (getenv("SKYOFF")) return;
    if (!_dw.sky_ok) _dw_sky_load();
    if (getenv("WARPDBG"))
        printf("[dw] sky draw on=%d vao=%u clouds=%d\n",
            _dw.on ? 1 : 0, _dw.sky_bg_m.vao, (int)_dw.sky_cl.size());
    if (!_dw.sky_bg_m.vao) return;
    // px DS -> clip ; le quad repose tout au fond (z ~ 0.9999)
    Mat4 p2n = Mat4::ident();
    p2n.m[0] = 2.f / 256.f; p2n.m[5] = -2.f / 192.f;
    p2n.m[12] = -1.f; p2n.m[13] = 1.f;
    float k = getenv("SKYDARK") ? atof(getenv("SKYDARK"))
                                : _dw.sky_dark / 16.f; // CalculateTintedColor
    // DS : mix(couleur, cible, level/16) — fond vers GX_RGB(4,4,8),
    // nuages vers GX_RGB(6,6,8)
    _dw.sky_bg_m.tint_k = k;
    _dw.sky_bg_m.tint_to[0] = 4 / 31.f;
    _dw.sky_bg_m.tint_to[1] = 4 / 31.f;
    _dw.sky_bg_m.tint_to[2] = 8 / 31.f;
    rnd::draw(_dw.sky_bg_m, p2n);
    for (auto& c : _dw.sky_cl) {
        if (c.set < 0 || c.set > 6 || !_dw.sky_ct[c.set]) continue;
        float a = c.ang * 0.0174533f;
        Mat4 tr = Mat4::ident();
        tr.m[12] = c.cx + cosf(a) * c.rad;
        tr.m[13] = c.cy + sinf(a) * c.rad;
        float b = (c.ang + c.off) * 0.0174533f;
        Mat4 rt = Mat4::ident();
        rt.m[0] = cosf(b); rt.m[1] = sinf(b);
        rt.m[4] = -sinf(b); rt.m[5] = cosf(b);
        Mat4 sc = Mat4::ident(); sc.m[0] = sc.m[5] = c.scl;
        _dw.sky_cm[c.set].tint_k = k;
        _dw.sky_cm[c.set].tint_to[0] = 6 / 31.f;
        _dw.sky_cm[c.set].tint_to[1] = 6 / 31.f;
        _dw.sky_cm[c.set].tint_to[2] = 8 / 31.f;
        rnd::draw(_dw.sky_cm[c.set], p2n.mul(tr).mul(rt).mul(sc));
    }
}

// ------------------------------------------------------------------
// plateformes mobiles + élévateurs (sMovingPlatformMapTemplates DS)
// dir : 0=UP 1=DOWN 2=NONE ; pflag = bit plat_flags (0xFF = invalide)
// ------------------------------------------------------------------
enum { DW_PF_B1F_1 = 0, DW_PF_B2F_1, DW_PF_B3F_1, DW_PF_B4F_1, DW_PF_B4F_2,
       DW_PF_B5F_3, DW_PF_B3F_2, DW_PF_B5F_1, DW_PF_B4F_3, DW_PF_B6F_1,
       DW_PF_B7F_1 };
struct DwPlatT { uint16_t hdr; uint8_t idx, dir, dest, kind, pflag, path;
                 int16_t tx, ty, tz; };
static const DwPlatT DW_PLATS[] = {
    // 1F (573)
    {573, 0, 1, 0, 2, 0xFF, 0x0, 0x28, 0x121, 0x36},
    // B1F (574)
    {574, 0, 0, 0, 2, DW_PF_B1F_1, 0x1, 0x28, 0x101, 0x36},
    {574, 1, 1, 0, 3, 0xFF,      0x2, 0x21, 0x101, 0x2D},
    // B2F (575)
    {575, 0, 0, 1, 3, DW_PF_B2F_1, 0x3, 0x21, 0xE1, 0x2D},
    {575, 1, 2, 0, 5, 0xFF,      0x0, 0x21, 0xE1, 0x26},
    {575, 2, 2, 0, 5, 0xFF,      0x0, 0x21, 0xE1, 0x17},
    {575, 3, 2, 0, 5, 0xFF,      0x0, 0x21, 0xE1, 0x8},
    {575, 4, 2, 0, 5, 0xFF,      0x0, 0x31, 0xE1, 0x8},
    {575, 5, 2, 0, 5, 0xFF,      0x0, 0x38, 0xE1, 0x8},
    {575, 6, 2, 0, 5, 0xFF,      0x0, 0x31, 0xE1, 0x1F},
    {575, 7, 1, 0, 4, 0xFF,      0x4, 0x41, 0xE1, 0x1F},
    {575, 8, 2, 0, 5, 0xFF,      0x0, 0x38, 0xE1, 0x34},
    {575, 9, 2, 1, 6, 0xFF,      0x0, 0x21, 0xE9, 0xF},
    {575,10, 2, 1, 6, 0xFF,      0x0, 0x31, 0xE9, 0xF},
    {575,11, 2, 1, 6, 0xFF,      0x0, 0x41, 0xE9, 0xF},
    {575,12, 2, 1, 6, 0xFF,      0x0, 0x41, 0xE9, 0x2D},
    {575,13, 2, 1, 6, 0xFF,      0x0, 0x41, 0xE9, 0x34},
    {575,14, 2, 1, 6, 0xFF,      0x0, 0x29, 0xE9, 0x34},
    {575,15, 2, 1, 6, 0xFF,      0x0, 0x31, 0xE9, 0x2D},
    {575,16, 2, 1, 6, 0xFF,      0x0, 0x1A, 0xE9, 0x26},
    {575,17, 2, 1, 6, 0xFF,      0x0, 0x31, 0xE9, 0x26},
    // B3F (576)
    {576, 0, 0, 7, 4, DW_PF_B3F_1, 0x5, 0x41, 0xC1, 0x1F},
    {576, 1, 1, 0, 9, DW_PF_B3F_2, 0x6, 0x4F, 0xC1, 0x3E},
    {576, 2, 1, 1, 9, 0xFF,      0x7, 0x5F, 0xC1, 0x46},
    {576, 3, 1, 2, 8, 0xFF,      0x8, 0x60, 0xC1, 0x2B},
    // B4F (577)
    {577, 0, 0, 1, 9, DW_PF_B4F_1, 0xA, 0x4F, 0xA1, 0x3E},
    {577, 1, 0, 2, 9, DW_PF_B4F_2, 0xB, 0x5F, 0xA1, 0x46},
    {577, 2, 1, 0,16, DW_PF_B4F_3, 0xC, 0x4E, 0xA1, 0x4D},
    // B5F (579)
    {579, 0, 0, 2,16, DW_PF_B5F_1, 0xD, 0x4E, 0x81, 0x4D},
    {579, 1, 1, 0,18, 0xFF,      0xE, 0x57, 0x81, 0x43},
    {579, 2, 0, 3, 8, DW_PF_B5F_3, 0xF, 0x60, 0x81, 0x2B},
    // B6F (580)
    {580, 0, 0, 1,18, DW_PF_B6F_1, 0x11,0x57, 0x73, 0x43},
    {580, 1, 1, 0,19, 0xFF,      0x12,0x55, 0x73, 0x56},
    // B7F (581)
    {581, 0, 0, 1,19, DW_PF_B7F_1, 0x13,0x55, 0x41, 0x56},
};
static const int DW_PLATS_N = (int)(sizeof(DW_PLATS) / sizeof(DW_PLATS[0]));

// chemins d'élévateur (sElevatorPlatformPaths DS) — offsets en tuiles,
// delta en px/frame (60 Hz) ; fset/fclr = bits plat_flags appliqués au
// changeMaps quand next == -1
struct DwPath { int8_t next; int16_t fx, fy, fz, cx, cy, cz;
                int8_t dx, dy, dz; int8_t fset, fclr; };
static const DwPath DW_PATHS[] = {
    { -1,  0,-0x20,0,  0,-0x10,0,  0,-4,0, DW_PF_B1F_1, -1 },        // 00
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, -1, DW_PF_B1F_1 },        // 01
    { -1,  0,-0x20,0,  0,-0x1C,0,  0,-4,0, DW_PF_B2F_1, -1 },        // 02
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, -1, DW_PF_B2F_1 },        // 03
    { -1,  0,-0x20,0,  0,-0x0E,0,  0,-4,0, DW_PF_B3F_1, -1 },        // 04
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, -1, DW_PF_B3F_1 },        // 05
    { -1,  0,-0x20,0,  0,-0x12,0,  0,-4,0, DW_PF_B4F_1, DW_PF_B3F_2 },// 06
    { -1,  0,-0x20,0,  0,-0x0E,0,  0,-4,0, DW_PF_B4F_2, -1 },        // 07
    {  9,  0,-0x20,0,  0,-0x12,0,  0,-4,0, DW_PF_B5F_3, -1 },        // 08
    { -1,  0,-0x20,0,  0,-0x0E,0,  0,-4,0, -1, -1 },                 // 09
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, DW_PF_B3F_2, DW_PF_B4F_1 },// 0A
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, -1, DW_PF_B4F_2 },        // 0B
    { -1,  0,-0x20,0,  0,-0x0E,0,  0,-4,0, DW_PF_B5F_1, DW_PF_B4F_3 },// 0C
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, DW_PF_B4F_3, DW_PF_B5F_1 },// 0D
    { -1,  0,-0x0E,0,  0,-0x0C,0,  0,-4,0, DW_PF_B6F_1, -1 },        // 0E
    {0x10, 0, 0x20,0,  0, 0x12,0,  0, 4,0, -1, DW_PF_B5F_3 },        // 0F
    { -1,  0, 0x20,0,  0, 0x0A,0,  0, 4,0, -1, -1 },                 // 10
    { -1,  0, 0x0E,0,  0, 0x01,0,  0, 4,0, -1, DW_PF_B6F_1 },        // 11
    { -1,  0,-0x32,0,  0,-0x11,0,  0,-4,0, DW_PF_B7F_1, -1 },        // 12
    { -1,  0, 0x32,0,  0, 0x12,0,  0, 4,0, -1, DW_PF_B7F_1 },        // 13
    { -1,  0, 0x20,0,  0, 0x12,0,  0, 4,0, -1, -1 },                 // 14
    { -1,  0,-0x20,0,  0,-0x0E,0,  0, 4,0, -1, -1 },                 // 15
};
static const int DW_PATHS_N = (int)(sizeof(DW_PATHS) / sizeof(DW_PATHS[0]));

// offset initial du prop par kind (sPropInitialPosOffsetByKind DS, px)
static Vec3 dw_kind_off(int kind) {
    switch (kind) {
    case 8: case 16: return {-8.f, -25.f, 2.f};
    case 18:        return {-8.f, -25.f, 10.f};
    case 20: case 21: case 22: case 23: return {0.f, -32.f, -6.f};
    case 24:        return {0.f, -14.f, 8.f};
    default:        return {0.f, -25.f, -6.f};
    }
}

// échelle du prop par kind (sPropScaleByKind DS) : 1.25 partout (vigne
// z×1.5) ; rocher/portail restent à 1 — leur clip joint porte l'échelle
static Vec3 dw_kind_scale(int kind) {
    switch (kind) {
    case 22:        return {1.25f, 1.25f, 1.5f};
    case 23: case 24: return {1.f, 1.f, 1.f};
    default:        return {1.25f, 1.25f, 1.25f};
    }
}

// props simples (sSimplePropsMapTemplates ov9) : cascade B5F, portails
// 1F/salle Giratina/grotte retour — spawnés si la cond flag tient
struct DwSimp { uint16_t hdr; int16_t kind, tx, ty, tz; uint16_t cond, cval; };
static const DwSimp DW_SIMPS[] = {
    {573, 24,  55, 289, 39, 0,  0},    // portail 1F
    {579, 21, 106, 153, 78, 0,  0},    // cascade B5F (scroll BTA)
    {582, 24,  15,   1, 12, 5, 14},    // portail salle (progress >= 14)
    {583, 24, 116,  65, 74, 0,  0},    // portail grotte retour
};

void World::_dw_spawn_simps() {
    auto* i = _dw.info(_header);
    if (!i) return;
    for (auto& s : DW_SIMPS) {
        if (s.hdr != _header || !_dw_ev_cond(s.cond, s.cval)) continue;
        PropInst pi;
        int member = 124 + s.kind;
        pi.meshes = _fld_mesh(member);
        pi.mdl = _fld_mdl(member);
        if (!pi.mdl || pi.meshes.empty()) continue;
        pi.pos = _dw_world(i, s.tx, s.ty, s.tz) + dw_kind_off(s.kind);
        // field-return : ne pas respawner un simp déjà présent à cette tuile
        bool dup = false;
        Vec3 w = _dw_world(i, s.tx, s.ty, s.tz);
        for (auto& p : _props)
            if (p.cell == -3 && fabsf(p.pos.x - w.x) < 1.f
                && fabsf(p.pos.z - w.z) < 1.f) { dup = true; break; }
        if (dup) continue;
        pi.scale = dw_kind_scale(s.kind);
        pi.model = -3; pi.cell = -3;
        _dw_anim_attach(pi, s.kind);
        _props.push_back(pi);
    }
}

// empreinte marchable de la plateforme = bbox xz des tris CPU du prop
// (espace modèle) — la surface du prop fait office de sol sur toute
// l'aire qu'il recouvre, pas seulement la tuile du template.
void World::_dw_plat_fp(DwSys::DwPlat& p, const PropInst& pi) {
    if (!pi.mdl) return;
    Vec3 mn{1e9f, 1e9f, 1e9f}, mx{-1e9f, -1e9f, -1e9f};
    for (auto& mg : pi.mdl->meshes) {
        std::vector<Vec3> ct; Vec3 o{0,0,0};
        _tris(mg, o, &ct);
        for (auto& v : ct) {
            v.x *= pi.scale.x; v.y *= pi.scale.y; v.z *= pi.scale.z;
            if (v.x < mn.x) mn.x = v.x;
            if (v.x > mx.x) mx.x = v.x;
            if (v.y < mn.y) mn.y = v.y;
            if (v.y > mx.y) mx.y = v.y;
            if (v.z < mn.z) mn.z = v.z;
            if (v.z > mx.z) mx.z = v.z;
        }
    }
    if (mn.x <= mx.x) { p.bmn = mn; p.bmx = mx; }
    if (getenv("WARPDBG"))
        printf("[dw] plat %d.%d fp x=%.0f..%.0f y=%.0f..%.0f z=%.0f..%.0f\n",
            p.hdr, p.idx, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
}

// spawn des plateformes de l'étage actif + inactif
// (InitMovingPlatformPropsForMap cur+next DS) — flag persisté requis
void World::_dw_plats_spawn() {
    _dw.plats.clear();
    auto* ci = _dw.info(_header);
    if (!ci) return;
    int floors[2] = {_dw.hdr, _dw.ina_hdr};
    for (int fi = 0; fi < 2; fi++) {
        int fh = floors[fi];
        if (fh < 0) continue;
        for (int k = 0; k < DW_PLATS_N; k++) {
            const DwPlatT& t = DW_PLATS[k];
            if (t.hdr != fh) continue;
            if (t.pflag != 0xFF && !((_dw.plat_flags >> t.pflag) & 1))
                continue;
            DwSys::DwPlat p;
            p.hdr = t.hdr; p.idx = t.idx; p.dir = t.dir; p.dest = t.dest;
            p.kind = t.kind; p.pflag = t.pflag; p.path = t.path;
            p.tx = t.tx; p.ty = t.ty; p.tz = t.tz;
            p.pos = _dw_world(ci, t.tx, t.ty, t.tz) + dw_kind_off(t.kind);
            PropInst pi;
            int member = 124 + t.kind;
            pi.meshes = _fld_mesh(member);
            pi.mdl = _fld_mdl(member);
            if (pi.mdl && !pi.meshes.empty()) {
                pi.pos = p.pos; pi.model = -3; pi.cell = -3;
                pi.scale = dw_kind_scale(t.kind);
                _dw_anim_attach(pi, t.kind);
                // ascenseur salle Giratina : apparition en fondu quand
                // FLAG_DISTORTION_WORLD_STEPPING_STONES (2423) se pose
                // (DS : opacité +1 un frame sur deux -> blend = step>>1)
                if (t.hdr == 580 && t.idx == 1) {
                    p.stones = _vm.flag(2423) ? 0 : -1;
                    for (auto& mg : pi.meshes) mg.blend = 0.f;
                }
                _dw_plat_fp(p, pi);
                p.prop = (int)_props.size();
                _props.push_back(pi);
            }
            _dw.plats.push_back(p);
        }
    }
    if (getenv("WARPDBG"))
        printf("[dw] plats=%d flags=%03x\n",
               (int)_dw.plats.size(), _dw.plat_flags);
}

// InitSpecificMovingPlatformPropForMap DS — spawn forcé d'une plateforme
void World::_dw_plat_spawn_one(int hdr, int idx) {
    auto* ci = _dw.info(_header);
    for (int k = 0; k < DW_PLATS_N; k++) {
        const DwPlatT& t = DW_PLATS[k];
        if (t.hdr != hdr || t.idx != idx) continue;
        for (auto& p : _dw.plats)
            if (p.hdr == hdr && p.idx == idx) return;   // déjà là
        DwSys::DwPlat p;
        p.hdr = t.hdr; p.idx = t.idx; p.dir = t.dir; p.dest = t.dest;
        p.kind = t.kind; p.pflag = t.pflag; p.path = t.path;
        p.tx = t.tx; p.ty = t.ty; p.tz = t.tz;
        p.pos = _dw_world(ci, t.tx, t.ty, t.tz) + dw_kind_off(t.kind);
        PropInst pi;
        int member = 124 + t.kind;
        pi.meshes = _fld_mesh(member);
        pi.mdl = _fld_mdl(member);
        if (pi.mdl && !pi.meshes.empty()) {
            pi.pos = p.pos; pi.model = -3; pi.cell = -3;
            pi.scale = dw_kind_scale(t.kind);
            _dw_anim_attach(pi, t.kind);
            _dw_plat_fp(p, pi);
            p.prop = (int)_props.size();
            _props.push_back(pi);
        }
        _dw.plats.push_back(p);
        return;
    }
}

// hauteur du plancher de plateforme : base logique de la tuile template
// (le DS compare les tuiles partagées — pos.y inclut l'offset de kind
// qui enfonce le modèle sous son niveau logique).
float World::_dw_plat_surf(const DwSys::DwPlat& p) const {
    return p.pos.y - dw_kind_off(p.kind).y;
}

// FindMovingPlatformPropAnimatorAt DS — tuile partagée + dir lift.
// DS compare playerY = MapObject.y/2 (hauteur px en tuile) : on accepte
// la tuile exacte OU la plateforme sous le joueur à moins d'une tuile
// (la chute d'une tuile sur la plateforme n'est pas forcément finie à
// la fin du pas — plat_pend reteste pendant l'atterrissage).
int World::_dw_plat_at(int tx, int ty, int tz) const {
    for (int i = 0; i < (int)_dw.plats.size(); i++) {
        const DwSys::DwPlat& p = _dw.plats[i];
        if (p.hdr != _header || p.dir == 2 || p.tx != tx || p.tz != tz)
            continue;
        float s = _dw_plat_surf(p);
        if (p.ty == ty || (s <= _ppos.y + 2.f && _ppos.y - s <= TILE))
            return i;
    }
    return -1;
}

// point (px monde) dans l'empreinte d'une plateforme (suit le prop
// pendant les trajets — bmn/bmx sont espace modèle).
bool World::_dw_fp_has(const DwSys::DwPlat& p, float wx, float wz) const {
    return wx >= p.pos.x + p.bmn.x - 4.f && wx <= p.pos.x + p.bmx.x + 4.f
        && wz >= p.pos.z + p.bmn.z - 4.f && wz <= p.pos.z + p.bmx.z + 4.f;
}

// plateforme sous le joueur (toutes dir confondues) : empreinte +
// hauteur — utilisée pour savoir si l'on se tient dessus.
int World::_dw_plat_on(float y) const {
    for (int i = 0; i < (int)_dw.plats.size(); i++) {
        const DwSys::DwPlat& p = _dw.plats[i];
        if (p.hdr != _header || !_dw_fp_has(p, _ppos.x, _ppos.z)) continue;
        float s = _dw_plat_surf(p);
        if (s <= y + 2.f && y - s <= TILE) return i;
    }
    return -1;
}

// sol offert par une plateforme sur la tuile locale (x,z) : sa surface
// si elle couvre la tuile et qu'elle est à portée de marche/chute du
// point requêté, sinon rien — une plateforme garée au-dessus ou loin
// en dessous n'aspire pas le joueur.
float World::_dw_plat_ground(int tx, int tz, float from) const {
    if (!_dw.on) return -1e9f;
    float wx = tx * TILE + TILE / 2.f, wz = tz * TILE + TILE / 2.f;
    for (const auto& p : _dw.plats) {
        if (p.hdr != _header || !_dw_fp_has(p, wx, wz)) continue;
        float s = _dw_plat_surf(p);
        if (fabsf(s - from) <= TILE) return s;
    }
    return -1e9f;
}

// tuile plateforme franchissable : la plateforme fait office de sol —
// la collision BDHC en dessous est ignorée quand sa surface est à
// portée du pas (même portée que _dw_plat_ground).
bool World::_dw_plat_tile(int tx, int tz, float y) const {
    if (!_dw.on) return false;
    float wx = tx * TILE + TILE / 2.f, wz = tz * TILE + TILE / 2.f;
    for (const auto& p : _dw.plats) {
        if (p.hdr != _header || !_dw_fp_has(p, wx, wz)) continue;
        float s = _dw_plat_surf(p);
        if (fabsf(s - y) <= TILE) return true;
    }
    return false;
}

// ------------------------------------------------------------------
// trajet élévateur (DistWorldElevatorPlatform DS) :
// vibre -> moitié 1 -> changeMaps (étage) -> moitié 2 -> fin/chaînage
// ------------------------------------------------------------------
void World::_dw_elev_begin(int pi) {
    if (_dw.elev_st > 0) return;
    _dw.elev_p = pi;
    _dw.elev_vd = false;                      // vibration jouée 1x/trajet
    _dw.elev_pass = -1;
    // DS : le joueur s'aligne sur la surface logique de la plateforme
    // (sa tuile partagée pilote le delta du chemin).
    _ppos.y = _dw_plat_surf(_dw.plats[pi]);
    _dw_elev_leg();
    _phcalc = false;                          // hauteur figée pendant le trajet
    if (getenv("WARPDBG"))
        printf("[dw] elev begin plat=%d path=%d dir=%d\n",
            pi, _dw.elev_path, _dw.elev_dir);
}

// DistWorldElevatorPlatform_BeginMovement DS — par leg de la chaîne
void World::_dw_elev_leg() {
    DwSys::DwPlat& p = _dw.plats[_dw.elev_p];
    _dw.elev_dir = p.dir;
    _dw.elev_path = p.path;
    const DwPath& ph = DW_PATHS[_dw.elev_path];
    _dw.elev_dest = _dw.elev_dir == 1 ? dw_next(_header) : dw_prev(_header);
    _dw.elev_pass = -1;
    if (_dw.elev_dir == 1) {
        // passager Cynthia : 1F après le saut, B6F après le puzzle
        int lid = -1;
        if (_header == 573 && _vm.var(0x4055) == 2) lid = 129;
        else if (_header == 580 && _vm.var(0x4055) == 7) lid = 128;
        if (lid >= 0)
            for (int i = 0; i < (int)_objects.size(); i++)
                if (_objects[i].id == lid) { _dw.elev_pass = i; break; }
    } else {
        // effets de chemin UP appliqués au début du leg (DS)
        switch (_dw.elev_path) {
        case 0xD:
            _dw.plat_flags |= 1 << DW_PF_B4F_1;
            _dw.plat_flags &= ~(1 << DW_PF_B4F_2);
            break;
        case 0xA:
            _dw.plat_flags &= ~(1 << DW_PF_B5F_3);
            break;
        case 0xB:
            _dw.plat_flags |= 1 << DW_PF_B3F_2;
            _dw.plat_flags &= ~(1 << DW_PF_B4F_1);
            _dw.plat_flags &= ~(1 << DW_PF_B5F_3);
            break;
        }
    }
    _dw.elev_off = {0, 0, 0};
    // tuile finale en espace PARTAGÉ (DS : GetPlayerPos + delta chemin) —
    // le repère local change au changeMaps, le partagé est invariant.
    _dw.elev_ftx = _dw_shared_x() + ph.fx;
    _dw.elev_fty = _dw_shared_y() + ph.fy;
    _dw.elev_ftz = _dw_shared_z() + ph.fz;
    _dw.elev_fin = {ph.fx * TILE, ph.fy * TILE, ph.fz * TILE};
    _dw.elev_chg = {ph.cx * TILE, ph.cy * TILE, ph.cz * TILE};
    _dw.elev_dlt = {(float)ph.dx, (float)ph.dy, (float)ph.dz};
    _dw.elev_next = ph.next;
    // flags persistés : changeMaps du leg FINAL seulement (DS)
    _dw.elev_set = _dw.elev_next < 0 ? ph.fset : -1;
    _dw.elev_clr = _dw.elev_next < 0 ? ph.fclr : -1;
    if (!_dw.elev_vd) {
        _dw.elev_st = 1;
        _dw.elev_vib = 6.f;                   // ELEVATOR_VIBRATION_Y_DELTA
        _dw.elev_vib_n = 0;
        _dw.elev_py0 = _ppos.y;
        _dw.elev_ly0 = _dw.plats[_dw.elev_p].pos.y;
        _dw.elev_vd = true;
        if (_snd) {
            int s = _snd->seq_named("SEQ_SE_PL_FW089");
            if (s >= 0) _snd->se(s);
        }
    } else {
        _dw.elev_st = 2;
    }
}

void World::_dw_elev_tick() {
    if (!_dw.on || _dw.elev_st <= 0) return;
    if (_dw.elev_p < 0 || _dw.elev_p >= (int)_dw.plats.size()) {
        _dw.elev_st = 0; _phcalc = true; return;
    }
    DwSys::DwPlat& p = _dw.plats[_dw.elev_p];
    if (_dw.elev_st == 1) {                   // VIBRATE DS
        p.pos.y = _dw.elev_ly0 + _dw.elev_vib;
        _ppos.y = _dw.elev_py0 + _dw.elev_vib;
        if (p.prop >= 0 && p.prop < (int)_props.size())
            _props[p.prop].pos = p.pos;
        _dw.elev_vib = -_dw.elev_vib;
        if (_dw.elev_vib >= 0) {
            if (_dw.elev_vib >= 4.f) _dw.elev_vib -= 2.f;
            else if (_dw.elev_vib > 1.f) _dw.elev_vib -= 1.f;
            else if (++_dw.elev_vib_n >= 8) _dw.elev_vib -= 1.f;
            if (_dw.elev_vib <= 0.f) {
                p.pos.y = _dw.elev_ly0; _ppos.y = _dw.elev_py0;
                if (p.prop >= 0 && p.prop < (int)_props.size())
                    _props[p.prop].pos = p.pos;
                _dw.elev_st = 2;
            }
        }
        return;
    }
    // MOVE : chaque axe avance de delta (px/frame) jusqu'à l'offset visé ;
    // joueur + plateforme partagent le même déplacement borné
    auto adv = [](float& off, float tgt, float dl, float* pp, float* lp) {
        if (off == tgt) return;
        float n = off + dl;
        if ((dl > 0 && n > tgt) || (dl < 0 && n < tgt)) n = tgt;
        float m = n - off;
        *pp += m;
        if (lp) *lp += m;
        off = n;
    };
    adv(_dw.elev_off.x, _dw.elev_fin.x, _dw.elev_dlt.x, &_ppos.x, &p.pos.x);
    adv(_dw.elev_off.y, _dw.elev_fin.y, _dw.elev_dlt.y, &_ppos.y, &p.pos.y);
    adv(_dw.elev_off.z, _dw.elev_fin.z, _dw.elev_dlt.z, &_ppos.z, &p.pos.z);
    if (p.prop >= 0 && p.prop < (int)_props.size())
        _props[p.prop].pos = p.pos;
    if (_dw.elev_pass >= 0 && _dw.elev_pass < (int)_objects.size())
        _objpos[_dw.elev_pass].y = _ppos.y;
    if (_dw.elev_st == 2
        && _dw.elev_off.y == _dw.elev_chg.y) {   // changeMaps atteint
        _dw.elev_st = 3;
        _dw_floor_change(_dw.elev_dir);
        return;
    }
    if (_dw.elev_st == 3
        && _dw.elev_off.y == _dw.elev_fin.y) {   // leg terminé
        if (_dw.elev_next < 0) {
            _dw.elev_st = 0;
            // tuile finale partagée -> locale de l'étage d'arrivée
            auto* i = _dw.info(_header);
            _ppos = {(_dw.elev_ftx - (i ? i->ox : 0)) * TILE + TILE / 2.f,
                     (_dw.elev_fty - (i ? i->oa : 0)) * TILE,
                     (_dw.elev_ftz - (i ? i->oz : 0)) * TILE + TILE / 2.f};
            _phcalc = true;
            _dw.elev_pass = -1;
            // fin de trajet : cas particuliers DS
            if (_dw.elev_dir == 1 && _dw.elev_path == 9) {
                _dw.plat_flags |= 1 << DW_PF_B5F_1;
                _dw_plat_spawn_one(579, 0);
            }
            // fin descente B4F (pathIndex 1) : Cyrus marche est*(x-88)
            // puis nord*4, puis DeleteMapObject + CYRUS_APPEARANCE=1
            if (_dw.elev_dest == 577 && _dw.elev_dir == 1
                && _dw.plats[_dw.elev_p].idx == 1
                && _vm.var(0x405B) == 0) {
                auto* inf = _dw.info(577);
                for (int k = 0; k < (int)_objects.size(); k++)
                    if (_objects[k].id == 134 && inf) {
                        int sxp = (int)(_objpos[k].x / TILE);
                        _dw.cy_obj = k; _dw.cy_leg = 0;
                        _dw.cy_east = sxp + inf->ox - 88;
                        _dw.cy_left = _dw.cy_east * TILE;
                        if (_dw.cy_left <= 0) _dw.cy_leg = 1,
                            _dw.cy_left = 4 * TILE;
                        break;
                    }
                if (_dw.cy_obj < 0) _vm.set_var(0x405B, 1);
            }
            // rafraîchit la plateforme flottante sous les pieds
            _dw.cur_fp = -1; _dw.fp_kind = -1; _dw.fp_attr.clear();
            // l'atterrissage n'est pas un pas : l'ascenseur du quai
            // d'arrivée (même tuile partagée) ne rembarque pas le joueur
            _dw.elev_skip = 1;
            _dw_moved(_pdir >= 0 ? _pdir : 0);
            _dw.elev_skip = 0;
            if (getenv("WARPDBG")) {
                printf("[dw] elev done ->(%d,%d,%d)\n",
                    _dw.elev_ftx, _dw.elev_fty, _dw.elev_ftz);
                for (int i = 0; i < (int)_dw.plats.size(); i++) {
                    auto& q = _dw.plats[i];
                    if (q.hdr != _header) continue;
                    printf("[dw]   plat%d pos=(%.0f,%.0f,%.0f) prop=%d hid=%d\n",
                        i, q.pos.x, q.pos.y, q.pos.z, q.prop,
                        q.prop >= 0 && q.prop < (int)_props.size()
                            ? _props[q.prop].hidden : -1);
                }
            }
        } else {
            // leg chaîné : le template garde sa tuile ; le chemin
            // bascule sur nextIndex (DS EndMovement -> state INIT)
            _dw.plats[_dw.elev_p].path = _dw.elev_next;
            _dw_elev_leg();
        }
    }
}

// LoadFloor DS : l'étage devient actif (next si descente, prev sinon) ;
// la position du joueur est continue — repérée dans le nouvel espace
// local : L_new = L_old + (off_old - off_new) * TILE
void World::_dw_floor_change(int dir) {
    int nh = dir == 1 ? dw_next(_header) : dw_prev(_header);
    auto* ci = _dw.info(_header);
    auto* ni = _dw.info(nh);
    if (nh < 0 || !ci || !ni) return;
    Vec3 adj{(ci->ox - ni->ox) * TILE, (ci->oa - ni->oa) * TILE,
             (ci->oz - ni->oz) * TILE};
    // DS : flags persistés + bascule template seulement au leg final
    if (_dw.elev_set >= 0) _dw.plat_flags |= 1 << _dw.elev_set;
    if (_dw.elev_clr >= 0) _dw.plat_flags &= ~(1 << _dw.elev_clr);
    int dest_idx = -1, chain = 0;
    if (_dw.elev_next < 0)
        dest_idx = _dw.plats[_dw.elev_p].dest;
    else {
        chain = 1;
        // cab chaîné : conserve sa géométrie, continue dans le nouveau
        // plan (template non basculé, cf. ChangeMaps DS)
    }
    Vec3 ppos_new = _ppos + adj;
    int ep = _dw.elev_p;
    Vec3 cont = ep >= 0 ? _dw.plats[ep].pos + adj : Vec3{0, 0, 0};
    int ep_kind = ep >= 0 ? _dw.plats[ep].kind : 0;   // avant reload
    int ep_prop_m = ep >= 0 ? _dw.plats[ep].prop : -1;
    Vec3 ep_bmn = ep >= 0 ? _dw.plats[ep].bmn : Vec3{0, 0, 0};
    Vec3 ep_bmx = ep >= 0 ? _dw.plats[ep].bmx : Vec3{0, 0, 0};
    _ppos = ppos_new; _vel = {0, 0, 0};
    // garde la hauteur figée pendant que le trajet continue
    bool hc = _phcalc; _phcalc = true;
    load_map(nh, -1);
    _phcalc = hc;
    _ppos = ppos_new; _vel = {0, 0, 0};
    _phidden = false;
    // rattache la cabine : plateforme destination (leg final) ou prop
    // synthétique poursuivant la descente (leg chaîné)
    _dw.elev_p = -1;
    if (!chain) {
        for (int i = 0; i < (int)_dw.plats.size(); i++)
            if (_dw.plats[i].hdr == nh && _dw.plats[i].idx == dest_idx) {
                _dw.plats[i].pos = cont;
                if (_dw.plats[i].prop >= 0
                    && _dw.plats[i].prop < (int)_props.size())
                    _props[_dw.plats[i].prop].pos = cont;
                _dw.elev_p = i;
                break;
            }
    }
    if (_dw.elev_p < 0) {
        DwSys::DwPlat p{};
        p.hdr = (uint16_t)nh; p.idx = 0xFF; p.dir = 2; p.dest = 0;
        p.kind = (uint8_t)ep_kind;
        p.pflag = 0xFF; p.path = 0;
        p.pos = cont;
        p.bmn = ep_bmn; p.bmx = ep_bmx;      // empreinte conservée
        // réutilise le prop de la cabine s'il a survécu au reload,
        // sinon en respawne un
        if (ep_prop_m >= 0 && ep_prop_m < (int)_props.size()
            && _props[ep_prop_m].cell == -3) {
            _props[ep_prop_m].pos = cont;
            _props[ep_prop_m].hidden = false;
            p.prop = ep_prop_m;
        } else {
            PropInst pi;
            int member = 124 + p.kind;
            pi.meshes = _fld_mesh(member);
            pi.mdl = _fld_mdl(member);
            if (pi.mdl && !pi.meshes.empty()) {
                pi.pos = cont; pi.model = -3; pi.cell = -3;
                p.prop = (int)_props.size();
                _props.push_back(pi);
            }
        }
        _dw.plats.push_back(p);
        _dw.elev_p = (int)_dw.plats.size() - 1;
    }
    if (getenv("WARPDBG"))
        printf("[dw] floor ->%d pos=(%.0f,%.0f,%.0f) elev_p=%d\n",
            nh, _ppos.x, _ppos.y, _ppos.z, _dw.elev_p);
}

// ------------------------------------------------------------------
// events de tuile (sMapEvents DS) — liste de commandes par étage
// ------------------------------------------------------------------
struct DwEv { uint16_t hdr; int16_t x, y, z; uint8_t fc; uint16_t fv;
              const DwEvCmd* c; };
// cond : 0 NONE 1 PUZZLE_FALSE 2 PUZZLE_TRUE 3 PROG_EQ 4 PROG_LEQ
//        5 PROG_GEQ 6 MANUAL 7 SHADOW 8 CYRUS

// 1F : texte Cynthia sur l'élévateur
static const DwEvCmd EV_1F_0[] = {
    {EV_SCR, 4, 0, 0, 0, 0, 0},
    {EV_PROG, 2, 0, 0, 0, 0, 0},             // JUMPED_ON_1F_ELEVATOR
    {EV_END, 0, 0, 0, 0, 0, 0},
};
// B1F : Mesprit disparaît + Cynthia B2F
static const DwEvCmd EV_B1F_0[] = {
    {EV_SCR, 3, 0, 0, 0, 0, 0},
    {EV_PROG, 4, 0, 0, 0, 0, 0},             // SAW_B1F_MESPRIT
    {EV_ADDOBJ, 575, 128, 0, 0, 0, 0},       // Cynthia B2F
    {EV_END, 0, 0, 0, 0, 0, 0},
};
// B2F : navettes plateformes + saut (move -delta, saut dir, retour)
#define EV_SHUT(plat, mp1, ox, oy, oz, jdir) \
    {EV_MOVE, plat, mp1, ox, oy, oz, 0}, \
    {EV_JUMP, jdir, 0, 0, 0, 0, 0}, \
    {EV_MOVE, plat, !mp1, -(ox), -(oy), -(oz), 0}, \
    {EV_END, 0, 0, 0, 0, 0, 0}
static const DwEvCmd EV_B2F_00[] = { EV_SHUT(1, 1, 0, 0, -8, 0) };
static const DwEvCmd EV_B2F_01[] = { EV_SHUT(1, 0, 0, 0, -8, 1) };
static const DwEvCmd EV_B2F_02[] = { EV_SHUT(2, 1, 0, 0, -8, 0) };
static const DwEvCmd EV_B2F_03[] = { EV_SHUT(2, 0, 0, 0, -8, 1) };
static const DwEvCmd EV_B2F_04[] = { EV_SHUT(3, 1, 9, 0, 0, 3) };
static const DwEvCmd EV_B2F_05[] = { EV_SHUT(3, 0, 9, 0, 0, 2) };
static const DwEvCmd EV_B2F_06[] = { EV_SHUT(5, 1, 0, 0, 0x25, 1) };
static const DwEvCmd EV_B2F_07[] = { EV_SHUT(5, 0, 0, 0, 0x25, 0) };
static const DwEvCmd EV_B2F_08[] = { EV_SHUT(6, 1, 9, 0, 0, 3) };
static const DwEvCmd EV_B2F_09[] = { EV_SHUT(6, 0, 9, 0, 0, 2) };
static const DwEvCmd EV_B2F_10[] = { EV_SHUT(9, 1, 9, 0, 0, 3) };
static const DwEvCmd EV_B2F_11[] = { EV_SHUT(9, 0, 9, 0, 0, 2) };
static const DwEvCmd EV_B2F_12[] = { EV_SHUT(10, 1, 9, 0, 0, 3) };
static const DwEvCmd EV_B2F_13[] = { EV_SHUT(10, 0, 9, 0, 0, 2) };
static const DwEvCmd EV_B2F_14[] = { EV_SHUT(11, 1, 0, 0, 0x17, 1) };
static const DwEvCmd EV_B2F_15[] = { EV_SHUT(11, 0, 0, 0, 0x17, 0) };
static const DwEvCmd EV_B2F_16[] = { EV_SHUT(12, 1, -9, 0, 0, 2) };
static const DwEvCmd EV_B2F_17[] = { EV_SHUT(12, 0, -9, 0, 0, 3) };
static const DwEvCmd EV_B2F_18[] = { EV_SHUT(13, 1, -0x11, 0, 0, 2) };
static const DwEvCmd EV_B2F_19[] = { EV_SHUT(13, 0, -0x11, 0, 0, 3) };
static const DwEvCmd EV_B2F_20[] = { EV_SHUT(17, 1, 0, -8, 0, 0) };
static const DwEvCmd EV_B2F_21[] = { EV_SHUT(17, 0, 0, -8, 0, 1) };
static const DwEvCmd EV_B2F_22[] = { EV_SHUT(16, 1, 0, -8, 0, 3) };
static const DwEvCmd EV_B2F_23[] = { EV_SHUT(16, 0, 0, -8, 0, 2) };
// B3F : Cyrus apparaît (2 variantes de progression)
static const DwEvCmd EV_B3F_0[] = {
    {EV_SCR, 2, 0, 0, 0, 0, 0},
    {EV_PROG, 6, 0, 0, 0, 0, 0},             // TALKED_TO_B3F_CYRUS
    {EV_END, 0, 0, 0, 0, 0, 0},
};
// B4F : ombre Giratina (flag anim -> vu une fois)
static const DwEvCmd EV_B4F_S1[] = {
    {EV_SHADOW, 0, 0, 0, 0, 0, 0},
    {EV_ANIMFLAG, 0, 0, 0, 0, 0, 0},
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_B4F_S2[] = {
    {EV_SHADOW, 1, 0, 0, 0, 0, 0},
    {EV_ANIMFLAG, 1, 0, 0, 0, 0, 0},
    {EV_END, 0, 0, 0, 0, 0, 0},
};
// B5F : tutos rochers des 3 gardiens
static const DwEvCmd EV_B5F_T0[] = {
    {EV_TUTO, 0, 0, 0, 0, 0, 0}, {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_B5F_T1[] = {
    {EV_TUTO, 1, 0, 0, 0, 0, 0}, {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_B5F_T2[] = {
    {EV_TUTO, 2, 0, 0, 0, 0, 0}, {EV_END, 0, 0, 0, 0, 0, 0},
};
// B7F : scène Cynthia + Cyrus
static const DwEvCmd EV_B7F_0[] = {
    {EV_SCR, 4, 0, 0, 0, 0, 0},
    {EV_PROG, 9, 0, 0, 0, 0, 0},             // LISTENED_TO_CYNTHIA_CYRUS
    {EV_END, 0, 0, 0, 0, 0, 0},
};
// salle Giratina : plateformes + ombres + arrivée
static const DwEvCmd EV_GR_0[] = {
    {EV_SHOWGP, 0, 0, 0, 0, 0, 0},
    {EV_PSET, 16, 0, 0, 0, 0, 0},            // PLATFORMS_VISIBLE
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_GR_1[] = {
    {EV_HIDEGP, 0, 0, 0, 0, 0, 0},
    {EV_PCLR, 16, 0, 0, 0, 0, 0},
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_GR_2[] = {
    {EV_SCR, 7, 0, 0, 0, 0, 0},
    {EV_SHADOW, 2, 0, 0, 0, 0, 0},           // tpl salle : 1re ombre
    {EV_PROG, 11, 0, 0, 0, 0, 0},            // FIRST_SHADOW
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_GR_3[] = {
    {EV_SHADOW, 3, 0, 0, 0, 0, 0},           // tpl salle : 2nde ombre
    {EV_PROG, 12, 0, 0, 0, 0, 0},            // SECOND_SHADOW
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEvCmd EV_GR_4[] = {
    {EV_PROG, 13, 0, 0, 0, 0, 0},            // GIRATINA_ARRIVED
    {EV_ARRIVAL, 0, 0, 0, 0, 0, 0},
    {EV_SCR, 8, 0, 0, 0, 0, 0},
    {EV_END, 0, 0, 0, 0, 0, 0},
};
static const DwEv DW_EVS[] = {
    {573, 0x28, 0x121, 0x34, 3, 1, EV_1F_0},
    {574, 0xF, 0x101, 0x3A, 3, 3, EV_B1F_0},
    // B2F navettes (cond NONE)
    {575, 0x21, 0xE1, 0x24, 0, 0, EV_B2F_00},
    {575, 0x21, 0xE1, 0x19, 0, 0, EV_B2F_01},
    {575, 0x21, 0xE1, 0x15, 0, 0, EV_B2F_02},
    {575, 0x21, 0xE1, 0xA, 0, 0, EV_B2F_03},
    {575, 0x23, 0xE1, 0x8, 0, 0, EV_B2F_04},
    {575, 0x2F, 0xE1, 0x8, 0, 0, EV_B2F_05},
    {575, 0x38, 0xE1, 0xA, 0, 0, EV_B2F_06},
    {575, 0x38, 0xE1, 0x32, 0, 0, EV_B2F_07},
    {575, 0x33, 0xE1, 0x1F, 0, 0, EV_B2F_08},
    {575, 0x3F, 0xE1, 0x1F, 0, 0, EV_B2F_09},
    {575, 0x23, 0xE9, 0xF, 0, 0, EV_B2F_10},
    {575, 0x2F, 0xE9, 0xF, 0, 0, EV_B2F_11},
    {575, 0x33, 0xE9, 0xF, 0, 0, EV_B2F_12},
    {575, 0x3F, 0xE9, 0xF, 0, 0, EV_B2F_13},
    {575, 0x41, 0xE9, 0x11, 0, 0, EV_B2F_14},
    {575, 0x41, 0xE9, 0x2B, 0, 0, EV_B2F_15},
    {575, 0x3F, 0xE9, 0x2D, 0, 0, EV_B2F_16},
    {575, 0x33, 0xE9, 0x2D, 0, 0, EV_B2F_17},
    {575, 0x3F, 0xE9, 0x34, 0, 0, EV_B2F_18},
    {575, 0x2B, 0xE9, 0x34, 0, 0, EV_B2F_19},
    {575, 0x31, 0xE9, 0x24, 0, 0, EV_B2F_20},
    {575, 0x31, 0xE1, 0x21, 0, 0, EV_B2F_21},
    {575, 0x1C, 0xE9, 0x26, 0, 0, EV_B2F_22},
    {575, 0x1F, 0xE1, 0x26, 0, 0, EV_B2F_23},
    // B3F Cyrus
    {576, 0x41, 0xC1, 0x29, 3, 4, EV_B3F_0},
    {576, 0x41, 0xC1, 0x29, 3, 5, EV_B3F_0},
    // B4F ombres (cond SHADOW : tirée si flag anim non posé)
    {577, 0x41, 0xA1, 0x39, 7, 0, EV_B4F_S1},
    {577, 0x62, 0xA1, 0x38, 7, 1, EV_B4F_S2},
    {577, 0x62, 0xA1, 0x39, 7, 1, EV_B4F_S2},
    // B5F tutos (cond PUZZLE_FALSE : tant que tuto pas vu)
    {579, 0x56, 0x81, 0x35, 1, 10, EV_B5F_T0},
    {579, 0x57, 0x81, 0x35, 1, 10, EV_B5F_T0},
    {579, 0x64, 0x81, 0x43, 1, 11, EV_B5F_T1},
    {579, 0x64, 0x81, 0x44, 1, 11, EV_B5F_T1},
    {579, 0x50, 0x81, 0x43, 1, 12, EV_B5F_T2},
    {579, 0x50, 0x81, 0x44, 1, 12, EV_B5F_T2},
    // B7F scène
    {581, 0x54, 0x41, 0x4C, 3, 8, EV_B7F_0},
    {581, 0x55, 0x41, 0x4C, 3, 8, EV_B7F_0},
    {581, 0x56, 0x41, 0x4C, 3, 8, EV_B7F_0},
    // salle Giratina
    {582, 0xF, 1, 0x17, 1, 16, EV_GR_0},
    {582, 0xF, 1, 0x18, 2, 16, EV_GR_1},
    {582, 0xF, 1, 0x18, 3, 10, EV_GR_2},
    {582, 0xF, 1, 0x11, 3, 11, EV_GR_3},
    {582, 0xF, 1, 0xE, 3, 12, EV_GR_4},
};
static const int DW_EVS_N = (int)(sizeof(DW_EVS) / sizeof(DW_EVS[0]));

bool World::_dw_ev_cond(int fc, uint16_t fv) const {
    switch (fc) {
    case 0: return true;
    case 1: return !((_dw.puzz_flags >> fv) & 1);
    case 2: return (_dw.puzz_flags >> fv) & 1;
    case 3: return _vm.var(0x4055) == fv;
    case 4: return _vm.var(0x4055) <= fv;
    case 5: return _vm.var(0x4055) >= fv;
    case 7: return !_vm.flag(2478 + fv);   // GIRATINA_SHADOW_1+anim
    case 8: return _vm.var(0x405B) == fv;
    default: return false;
    }
}

// HandleEventAt DS : la première event qui matche la tuile partagée
// est jouée ; l'event file ses commandes (tâche)
bool World::_dw_ev_at(int tx, int ty, int tz) {
    if (_dw.ev_cmds) return true;             // déjà occupé
    for (int i = 0; i < DW_EVS_N; i++) {
        const DwEv& e = DW_EVS[i];
        if (e.hdr == _header && e.x == tx && e.y == ty && e.z == tz
            && _dw_ev_cond(e.fc, e.fv)) {
            _dw.ev_cmds = e.c; _dw.ev_ci = 0; _dw.ev_ph = 0;
            if (getenv("WARPDBG"))
                printf("[dw] ev i=%d hdr=%d @(%d,%d,%d) k0=%d\n",
                    i, e.hdr, tx, ty, tz, e.c ? e.c[0].k : -1);
            return true;
        }
    }
    return false;
}

// MOVE_PLATFORM : vibre puis glisse vers l'offset (porte le joueur)
void World::_dw_ev_plat_move(int plat, bool mp, int x, int y, int z,
                             int dx, int dy, int dz) {
    _dw.ev_p = -1;
    for (int i = 0; i < (int)_dw.plats.size(); i++)
        if (_dw.plats[i].hdr == _header && _dw.plats[i].idx == plat) {
            _dw.ev_p = i; break;
        }
    if (_dw.ev_p < 0) { _dw.ev_ph = 0; return; }
    _dw.ev_mp = mp;
    _dw.ev_off = {0, 0, 0};
    _dw.ev_fin = {x * TILE, y * TILE, z * TILE};
    _dw.ev_dlt = {dx ? (float)dx : (x ? 4.f * (x > 0 ? 1 : -1) : 0.f),
                  dy ? (float)dy : (y ? 4.f * (y > 0 ? 1 : -1) : 0.f),
                  dz ? (float)dz : (z ? 4.f * (z > 0 ? 1 : -1) : 0.f)};
    _dw.ev_ph = 1;
    _dw.ev_vib = 3.f; _dw.ev_vib_n = 0;       // MOVING_PLATFORM_VIB_DELTA
    _dw.ev_py0 = _ppos.y;
    _dw.ev_ly0 = _dw.plats[_dw.ev_p].pos.y;
    _phcalc = false;
    if (_snd) {
        int s = _snd->seq_named("SEQ_SE_PL_FW089B");
        if (s >= 0) _snd->se(s);
    }
}

// JUMP_DISTORTION_WORLD_* : saut de 2 tuiles en 24 frames (arc haut)
void World::_dw_ev_jump(int dir) {
    static const int DX[4] = {0, 0, -1, 1};   // N S W E (dir DS)
    static const int DZ[4] = {-1, 1, 0, 0};
    int d = dir & 3;
    _dw.jump_from = _ppos;
    _dw.jump_to = {_ppos.x + DX[d] * 2 * TILE,
                   _ppos.y, _ppos.z + DZ[d] * 2 * TILE};
    _dw.jump_steps = 24; _dw.jump_t = 24;
    _dw.jump_axis = 1; _dw.jump_fdir = d;
    _dw.jump_prog = 0.f;
    _phcalc = false;
}

// file d'event : exécute la commande courante puis avance
void World::_dw_ev_tick() {
    if (!_dw.ev_cmds) return;
    const DwEvCmd* cmds = (const DwEvCmd*)_dw.ev_cmds;
    const DwEvCmd& c = cmds[_dw.ev_ci];
    switch (c.k) {
    case EV_END:
        _dw.ev_cmds = nullptr; _dw.ev_ph = 0;
        _phcalc = true;
        return;
    case EV_MOVE:
        if (_dw.ev_ph == 0) {
            _dw_ev_plat_move(c.a, c.b != 0, c.c, c.d, c.e,
                             0, 0, 0);
            if (_dw.ev_ph == 0) { _dw.ev_ci++; return; }
        }
        if (_dw.ev_ph == 1) {                 // vibration plateforme
            DwSys::DwPlat& p = _dw.plats[_dw.ev_p];
            p.pos.y = _dw.ev_ly0 + _dw.ev_vib;
            if (p.prop >= 0 && p.prop < (int)_props.size())
                _props[p.prop].pos = p.pos;
            if (_dw.ev_mp) _ppos.y = _dw.ev_py0 + _dw.ev_vib;
            _dw.ev_vib = -_dw.ev_vib;
            if (_dw.ev_vib >= 0) {
                if (++_dw.ev_vib_n >= 4) _dw.ev_vib = 0;
                else _dw.ev_vib -= 1.f;
                if (_dw.ev_vib <= 0) {
                    p.pos.y = _dw.ev_ly0;
                    if (p.prop >= 0 && p.prop < (int)_props.size())
                        _props[p.prop].pos = p.pos;
                    _dw.ev_ph = 2;
                }
            }
        } else {                              // glisse
            DwSys::DwPlat& p = _dw.plats[_dw.ev_p];
            bool done = true;
            auto adv = [](float& off, float tgt, float dl, float* pa,
                          float* pb) {
                if (off == tgt) return;
                float n = off + dl;
                if ((dl > 0 && n > tgt) || (dl < 0 && n < tgt)) n = tgt;
                *pa += n - off;
                if (pb) *pb += n - off;
                off = n;
            };
            if (_dw.ev_off.x != _dw.ev_fin.x) {
                adv(_dw.ev_off.x, _dw.ev_fin.x, _dw.ev_dlt.x,
                    &p.pos.x, _dw.ev_mp ? &_ppos.x : nullptr);
                done = _dw.ev_off.x == _dw.ev_fin.x && done;
            }
            if (_dw.ev_off.y != _dw.ev_fin.y) {
                adv(_dw.ev_off.y, _dw.ev_fin.y, _dw.ev_dlt.y,
                    &p.pos.y, _dw.ev_mp ? &_ppos.y : nullptr);
                done = _dw.ev_off.y == _dw.ev_fin.y && done;
            }
            if (_dw.ev_off.z != _dw.ev_fin.z) {
                adv(_dw.ev_off.z, _dw.ev_fin.z, _dw.ev_dlt.z,
                    &p.pos.z, _dw.ev_mp ? &_ppos.z : nullptr);
                done = _dw.ev_off.z == _dw.ev_fin.z && done;
            }
            if (p.prop >= 0 && p.prop < (int)_props.size())
                _props[p.prop].pos = p.pos;
            if (done) {
                // fin du déplacement : tuiles plateforme suivies
                p.tx += (int16_t)(_dw.ev_fin.x / TILE);
                p.ty += (int16_t)(_dw.ev_fin.y / TILE);
                p.tz += (int16_t)(_dw.ev_fin.z / TILE);
                _dw.ev_ph = 0; _dw.ev_ci++;
                _phcalc = true;
                if (_dw.ev_mp) {              // resynchronise la fp
                    _dw.cur_fp = -1; _dw.fp_kind = -1;
                    _dw.fp_attr.clear();
                    _dw_moved(_pdir >= 0 ? _pdir : 0);
                }
            }
        }
        return;
    case EV_JUMP:
        if (_dw.ev_ph == 0) {                 // lance le saut joueur
            _dw_ev_jump(c.a);
            _dw.ev_ph = 3;
        } else if (_dw.jump_t <= 0) {         // saut terminé -> cmd suivante
            _dw.ev_ph = 0; _dw.ev_ci++;
            _phcalc = true;
        }
        return;
    case EV_SCR:
        if (!_dw.ev_wait_scr) {
            _dw.ev_wait_scr = true;
            _start_script(c.a); _vm.step();
        } else if (!_vm.busy()) {
            _dw.ev_wait_scr = false;
            _dw.ev_ci++;
        }
        return;
    case EV_PROG:
        _vm.set_var(0x4055, c.a);
        _dw.ev_ci++;
        return;
    case EV_ADDOBJ:
        _dw_obj_add(c.b);
        _dw.ev_ci++;
        return;
    case EV_DELOBJ:
        _dw_obj_del(c.b);
        _dw.ev_ci++;
        return;
    case EV_PSET:
        _dw.puzz_flags |= 1u << c.a;
        _dw.ev_ci++;
        return;
    case EV_PCLR:
        _dw.puzz_flags &= ~(1u << c.a);
        _dw.ev_ci++;
        return;
    case EV_ANIMFLAG:
        _vm.set_flag(2478 + c.a, true);       // GIRATINA_SHADOW_1+anim
        _dw.ev_ci++;
        return;
    case EV_SHADOW:                           // show-giratina-shadow DS
        if (_dw.sh_state == 0) {              // spawn ; l'event attend la fin
            _dw_shadow_start(c.a);
            if (_dw.sh_state == 0) _dw.ev_ci++;   // prop introuvable : passe
            return;
        }
        if (_dw.sh_state == 2) { _dw.sh_state = 0; _dw.ev_ci++; }
        return;
    case EV_ARRIVAL:                          // play-giratina-arrival DS
        if (_dw.arr_ph == 0) {
            int before = (int)_objects.size();
            _dw_obj_add(128);                 // AddMapObjectWithLocalID
            if ((int)_objects.size() > before) {
                _dw.arr_obj = before;
                _objpos[_dw.arr_obj].y += 160.f;  // INITIAL_Y_OFFSET 10<<4
                _dw.arr_lift = 160.f;
                _dw.arr_pal = 16.f;               // SPRITE_PALETTE_MAX_TINT
                _objdark[_dw.arr_obj] = 16.f;
            }
            _dw.sky_kind = 2;                 // SKY_GIRATINA_ROOM_DARK
            _dw.sky_fix = 0; _dw.arr_dark = 0.f;
            _dw.arr_ph = 1;
            if (_snd) {
                int s = _snd->seq_named("SEQ_SE_PL_GIRA");
                if (s >= 0) _snd->se(s);
            }
            return;
        }
        if (_dw.arr_ph == 4) { _dw.arr_ph = 0; _dw.ev_ci++; }
        return;
    case EV_TUTO:                             // tuto rocher (guardian)
        // descendToB6F DS : tuto vu (10-12) + gardien en attente sur B6F
        // (13-15) ; l'objet gardien B6F spawn via sa cond flag à l'arrivée
        _dw.puzz_flags |= (1u << (10 + c.a)) | (1u << (13 + c.a));
        if (getenv("WARPDBG"))
            printf("[dw] tuto %d vu\n", c.a);
        _dw.ev_ci++;
        return;
    case EV_SHOWGP:                           // show-giratina-room-platforms
        if (_dw.gp_done) { _dw.gp_done = false; _dw.ev_ci++; return; }
        if (_dw.gp_st == 0) {                 // caméra (sautée) + délai 36
            _dw.gp_st = 1; _dw.gp_grp = 1;
            _dw.gp_delay = 36.f; _dw.gp_done = false;
        }
        return;
    case EV_HIDEGP:                           // hide-giratina-room-platforms
        if (_dw.gp_done) { _dw.gp_done = false; _dw.ev_ci++; return; }
        if (_dw.gp_st == 0) {
            _dw.gp_st = 2; _dw.gp_grp = 3;
            _dw.gp_delay = 16.f; _dw.gp_done = false;
        }
        return;
    case EV_CASC:
        _dw_ev_casc(c.a, c.b, c.c, c.d, c.e, c.f);
        return;
    default:
        _dw.ev_ci++;
        return;
    }
}

// CASCADE_UP/DOWN DS (simplifié) : courte phase de rotation à demi-
// vitesse puis glisse verticale ; l'étage change à l'offset mapLoad,
// le joueur est repositionné à finishingPosFix à la fin
void World::_dw_ev_casc(int dir, int fin_px, int load_t, int fix_t,
                        int dl, int rot) {
    if (_dw.ev_ph == 0) {
        _dw.ev_ph = 5;
        _dw.ev_off = {0, 0, 0};
        _dw.ev_fin = {0.f, (float)fin_px, 0.f};
        _dw.ev_dlt = {0.f, (float)(dir > 0 ? dl : -dl), 0.f};
        _dw.ev_c[0] = load_t * 16;              // mapLoadPosOffset (px)
        _dw.ev_c[1] = fix_t;                    // finishingPosFix (tuiles)
        _dw.ev_c[2] = rot;                      // frames de rotation
        _dw.ev_c[3] = 0;                        // map chargé
        _dw.ev_py0 = _ppos.y;
        _phcalc = false;
        _pdir = 2;                              // FACE_LEFT DS
        if (_snd) {
            int s = _snd->seq_named("SEQ_SE_PL_FW463");
            if (s >= 0) _snd->se(s);
        }
        return;
    }
    if (_dw.ev_ph == 5) {                       // rotation (demi-vitesse)
        _ppos.y += _dw.ev_dlt.y * 0.5f;
        _dw.ev_off.y += _dw.ev_dlt.y * 0.5f;
        if (--_dw.ev_c[2] <= 0) _dw.ev_ph = 6;
        return;
    }
    // glisse : avance de delta (signe selon dir) jusqu'à fin
    float n = _dw.ev_off.y + _dw.ev_dlt.y;
    float tgt = _dw.ev_fin.y;
    if ((_dw.ev_dlt.y > 0 && n > tgt) || (_dw.ev_dlt.y < 0 && n < tgt))
        n = tgt;
    _ppos.y += n - _dw.ev_off.y;
    _dw.ev_off.y = n;
    if (!_dw.ev_c[3]
        && ((_dw.ev_dlt.y < 0 && n <= _dw.ev_c[0])
            || (_dw.ev_dlt.y > 0 && n >= _dw.ev_c[0]))) {
        _dw.ev_c[3] = 1;
        // changeMaps cascade : descente -> next, remontée -> prev ;
        // sans cabine ni flags de chemin d'élévateur
        int e = _dw.elev_p, es = _dw.elev_set,
            ec = _dw.elev_clr, en = _dw.elev_next;
        _dw.elev_p = -1; _dw.elev_set = _dw.elev_clr = -1;
        _dw.elev_next = -1;
        _dw_floor_change(_dw.ev_dlt.y < 0 ? 1 : 0);
        _dw.elev_set = es; _dw.elev_clr = ec; _dw.elev_next = en;
        _dw.elev_p = -1;                      // pas de cabine (e inutilisé)
        (void)e;
    }
    if (_dw.ev_off.y == tgt) {
        // finishingPosFix : tuiles relatives à la tuile courante
        _ppos.y = (floorf(_ppos.y / TILE) + _dw.ev_c[1]) * TILE
                + TILE / 2.f;
        _pdir = 2;
        _dw.ev_ph = 0; _dw.ev_ci++;
        _phcalc = true;
        _dw.cur_fp = -1; _dw.fp_kind = -1; _dw.fp_attr.clear();
        if (_dw.ev_dlt.y < 0) {                 // fin de descente DS :
            _dw.plat_flags |= 1 << DW_PF_B5F_1; // ascenseur B5F_1 spawné
            _dw_plat_spawn_one(579, 0);
        }
        if (_snd) {
            int s = _snd->seq_named("SEQ_SE_PL_FW463");
            if (s >= 0) _snd->se_stop(s);
        }
    }
}

// ------------------------------------------------------------------
// ombre Giratina (DistWorldGiratinaShadowProp DS) : prop fldeff 144 +
// anim BTA 198 ; pos = tuile partagée*16 (+8 px sur x quand l'ombre
// est couchée, rotAnglesIndex 4) ; glisse posDelta px/frame ; sfx au
// premier tick ; l'event attend animFinished puis le prop disparaît.
// ------------------------------------------------------------------
struct DwShadowT { int16_t x, y, z; uint8_t rot, snd; float scl;
                   int8_t sx, sy, sz; int16_t steps; };
// sMapEventCmdParams*_Giratina* + sGiratinaShadowExternal DS :
// snd 0=NONE 1=CRY 2=FLEE ; rot -> angles {x,y,z} degs
static const DwShadowT DW_SHADOWS[] = {
    {63,169,9,   1, 1, 1.f,   0, 0,48, 64},   // 0 B4F ombre 1
    {42,137,32,  3, 1, 1.f,  48, 0, 0, 72},   // 1 B4F ombre 2
    {-9,-4,22,   3, 0, .25f, 16, 0, 0, 48},   // 2 salle : 1re ombre
    {15,-34,8,   4, 2, .5f,   0,32, 2, 32},   // 3 salle : 2nde ombre
    {6,297,42,   3, 1, 1.f,  48, 0, 0, 64},   // 4 1F (event script 0)
};
// sGiratinaShadowPropRotAngles DS (deg, sens horaire vu de +axe)
static const float DW_SH_ROT[5][3] = {
    {0,0,0}, {0,180,0}, {0,90,0}, {0,270,0}, {90,0,0},
};

void World::_dw_shadow_start(int tpl) {
    if (_dw.sh_state != 0 || tpl < 0 || tpl > 4) return;
    const DwShadowT& t = DW_SHADOWS[tpl];
    auto* i = _dw.info(_header);
    if (!i) return;
    PropInst pi;
    pi.meshes = _fld_mesh(124 + 20);
    pi.mdl = _fld_mdl(124 + 20);
    if (!pi.mdl || pi.meshes.empty()) return;
    // pos DS : tuile partagée*16 (PAS de centre +8 ni d'offset kind)
    pi.pos = {(t.x - i->ox) * TILE + (t.rot == 4 ? 8.f : 0.f),
              (t.y - i->oa) * TILE,
              (t.z - i->oz) * TILE};
    const float D = 3.14159265f / 180.f;
    pi.rx = DW_SH_ROT[t.rot][0] * D;
    pi.ry = DW_SH_ROT[t.rot][1] * D;
    pi.rz = DW_SH_ROT[t.rot][2] * D;
    pi.scale = {t.scl, t.scl, t.scl};
    pi.model = -3; pi.cell = -3;
    _dw_anim_attach(pi, 20);              // BTA 198 (matériau ondulant)
    _dw.sh_prop = (int)_props.size();
    _props.push_back(pi);
    _dw.sh_step = {(float)t.sx, (float)t.sy, (float)t.sz};
    _dw.sh_left = (float)t.steps; _dw.sh_sfx = t.snd;
    _dw.sh_state = 1;
    if (getenv("WARPDBG"))
        printf("[dw] shadow tpl %d @(%d,%d,%d) rot %d steps %d\n",
            tpl, t.x, t.y, t.z, t.rot, t.steps);
}

void World::_dw_shadow_stop() {
    if (_dw.sh_prop >= 0 && _dw.sh_prop < (int)_props.size())
        _props[_dw.sh_prop].hidden = true;
    _dw.sh_prop = -1; _dw.sh_state = 0; _dw.sh_left = 0;
}

void World::_dw_shadow_tick(float s60) {
    if (_dw.sh_state != 1) return;
    if (_dw.sh_sfx >= 0) {                // son au premier tick DS
        if (_snd) {
            int s = -1;
            if (_dw.sh_sfx == 1) _snd->cry(487, 0);       // cri Giratina
            else if (_dw.sh_sfx == 2)
                s = _snd->seq_named("SEQ_SE_DP_FW019");   // fuite
            if (s >= 0) _snd->se(s);
        }
        _dw.sh_sfx = -1;
    }
    if (_dw.sh_prop >= 0 && _dw.sh_prop < (int)_props.size()) {
        PropInst& p = _props[_dw.sh_prop];
        p.pos.x += _dw.sh_step.x * s60;
        p.pos.y += _dw.sh_step.y * s60;
        p.pos.z += _dw.sh_step.z * s60;
    }
    _dw.sh_left -= s60;
    if (_dw.sh_left <= 0) {
        _dw_shadow_stop(); _dw.sh_state = 2;
        if (getenv("WARPDBG")) printf("[dw] shadow fin\n");
    }
}

// play-giratina-arrival DS : Giratina ajoutée 160px au-dessus, descend
// (1px/frame puis .5 sous 16px) pendant que le ciel forcé monte à 12
// (8/90 level par frame) ; puis sa palette 16 -> 0 (16/90) ; 30 frames.
void World::_dw_arrival_tick(float s60) {
    if (_dw.arr_ph == 1) {
        _dw.arr_dark = fminf(12.f * 4096.f,
            _dw.arr_dark + (4096.f * 8.f / 90.f) * s60);
        _dw.sky_fix = (int)(_dw.arr_dark / 4096.f);
        float dl = (_dw.arr_lift >= 16.f ? 1.f : .5f) * s60;
        if (dl > _dw.arr_lift) dl = _dw.arr_lift;   // DS entier : pas de dépassement
        _dw.arr_lift -= dl;
        if (_dw.arr_obj >= 0 && _dw.arr_obj < (int)_objpos.size())
            _objpos[_dw.arr_obj].y -= dl;
        if (_dw.arr_lift <= 0.f && _dw.arr_dark >= 12.f * 4096.f) {
            _dw.arr_lift = 0.f;
            if (_dw.arr_obj >= 0 && _dw.arr_obj < (int)_objpos.size())
                _objpos[_dw.arr_obj].y
                    = floorf(_objpos[_dw.arr_obj].y / TILE) * TILE;
            _dw.arr_ph = 2;
        }
    } else if (_dw.arr_ph == 2) {
        _dw.arr_pal = fmaxf(0.f, _dw.arr_pal - (16.f / 90.f) * s60);
        if (_dw.arr_obj >= 0) _objdark[_dw.arr_obj] = _dw.arr_pal;
        if (_dw.arr_pal <= 0.f) _dw.arr_ph = 3;
    } else if (_dw.arr_ph == 3) {
        _dw.arr_wait += s60;
        if (_dw.arr_wait >= 30) {
            _dw.arr_wait = 0; _dw.arr_ph = 4;
            if (_dw.arr_obj >= 0) _objdark.erase(_dw.arr_obj);
            _dw.arr_obj = -1;
        }
    }
}
