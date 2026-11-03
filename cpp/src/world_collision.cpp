// world_collision.cpp — raycast BDHC/props, attrs terrain, sol, caméra.
#include "world.h"
#include "world_internal.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>

// ── collision ─────────────────────────────────────────────────────────────────
const World::Cell* World::_cell_at(float wx, float wz) const {
    int cx = (int)floorf(wx / CELL), cy = (int)floorf(wz / CELL);
    auto it = _cells.find(ckey(cx, cy));
    return it == _cells.end() ? nullptr : it->second.get();
}

bool World::_blocked(const Vec3& p) const {
    int tx = (int)floorf(p.x / TILE), tz = (int)floorf(p.z / TILE);
    auto ov = _tileover.find(((int64_t)tx << 32) | (uint32_t)tz);
    if (ov != _tileover.end()) return (ov->second & 0x8000) != 0;
    int a = _mattr_at(tx, tz);
    if (a < 0) {
        // hors matrice = bord du monde. Une cellule non chargée n'est
        // plus un mur : la grille précalculée couvre toute la matrice.
        if (getenv("CDBG")) fprintf(stderr, "[blk] t=(%d,%d) : hors matrice\n", tx, tz);
        return true;
    }
    if (getenv("ATTRDBG"))
        fprintf(stderr, "[attr] t=(%d,%d) a=%04x\n", tx, tz, a);
    // DynamicMapFeatures Pastoria (gym_features.c) : le niveau d'eau pilote
    // la collision des tuiles du bassin — H/M/L_GROUND franchissables au
    // niveau correspondant, colonne d'eau bloquée tant que nappe > sol.
    if (_gym.kind == 1) {
        int beh = a & 0xFF;
        if (beh == 0x56) return _gym.plate_h != 0;    // H_GROUND : LOW
        if (beh == 0x57) return _gym.plate_h != 32;   // M_GROUND : MIDDLE
        if (beh == 0x58) return _gym.plate_h != 64;   // L_GROUND : HIGH
        if (beh == 0x59)                              // nappe au-dessus du sol
            return _gym.plate_h > _ground_y(p, p.y) + 0.5f;
    }
    // Sunyshore : les régions de l'état courant = murs d'engrenages
    // (DynamicMapFeatures_CheckCollision — avant la collision statique DS).
    if (_gym.kind == 2 && _gym.room >= 0) {
        const uint8_t* lst = SUN_LIST[_gym.room][_gym.rot];
        for (int i = 0; i < SUN_NLIST[_gym.room]; i++) {
            const SunReg& r = SUN_REG[_gym.room][lst[i]];
            if (tx >= r.x && tx < r.x + r.w && tz >= r.z && tz < r.z + r.h)
                return true;
        }
    }
    // Eterna : bitmap pendule 13x13 à (5,7) + rangée fontaines z=19 —
    // l'état horloge (SystemVars DS) ouvre des chemins au fil des victoires.
    if (_gym.kind == 3) {
        if (tz >= 7 && tz <= 19 && tx >= 5 && tx <= 17 &&
            ET_CLOCK[_eterna_clock][(tz - 7) * 13 + (tx - 5)]) return true;
        if (tz == 19 && tx >= 1 && tx <= 21 &&
            ET_FOUNT[_eterna_clock][tx - 1]) return true;
    }
    // Canalave : 4 étages -> 4 bitmaps 32x32 (sCanalaveGymCollisionMaps) ;
    // remplace entièrement la collision statique (DS retourne toujours TRUE).
    if (_gym.kind == 4) {
        int fl = (int)(p.y / TILE / 10);             // hauteur/16/10 = étage
        if (fl < 0) fl = 0; else if (fl > 3) fl = 3;
        if (tx >= 0 && tx < 32 && tz >= 0 && tz < 32)
            return (CANA_COL[fl][tz] >> tx) & 1;
        return true;
    }
    // Villa : CheckFurnitureCollision DS — meubles solides possédés,
    // rectangle de tuiles bloquées (s'ajoute à la collision statique).
    if (_gym.kind == 8) {
        for (const VillaFurn& f : VILLA_FURN)
            if (f.s && _vm.flag(2461 + f.t) &&
                tz >= f.tp && tz <= f.b && tx >= f.l && tx <= f.r)
                return true;
    }
    // Monde Distorsion : une plateforme mobile fait office de sol sur
    // sa tuile (FindMovingPlatformPropAnimatorAt DS) — elle peut être
    // au-dessus du vide (pas de plaque BDHC franchissable).
    if (_dw.on && _dw_plat_tile(tx, tz, p.y)) return false;
    // Monde Distorsion : plateforme flottante courante chargée —
    // CheckCollisionOnCurrentFloatingPlatform DS : la grille
    // tw_arc_attr gouverne la tuile cible ; hors-limites = bloqué
    // (on ne marche pas hors d'un mur/plafond)
    if (_dw.on && _dw.fp_kind >= 0) {
        auto* i = _dw.info(_header);
        int stx = tx + (i ? i->ox : 0), stz = tz + (i ? i->oz : 0);
        int sty = (int)floorf(p.y / TILE) + (i ? i->oa : 0);
        uint16_t pa = _dw_fp_attr(stx, sty, stz);
        if (pa == 0xFFFE) {                     // hors plateforme
            if (_dw.fp_kind > 0) return true;   // mur/plafond : bloqué
        } else if (pa != 0xFFFF) {
            return (pa & 0x8000) != 0;          // grille plateforme
        }
    }
    // Monde Distorsion : GetVerticalDirection DS — la hauteur effective
    // de la tuile (plateforme > plaque > base d'étage) à plus de ~20 px
    // du joueur bloque le pas (les gouffres ne se franchissent pas à
    // pied ; les sauts/pads restent libres).
    if (_dw.on && (a & 0x8000) == 0) {
        float eff = _ground_y(p, p.y);
        if (eff > -1e8f && fabsf(eff - (p.y - 4.f)) >= 20.f) return true;
    }
    return (a & 0x8000) != 0;
}

// comportement de tuile = octet bas de l'attribut (map_tile_behavior.h)
int World::_tile_beh(int tx, int tz) const {
    auto ov = _tileover.find(((int64_t)tx << 32) | (uint32_t)tz);
    int a = (ov != _tileover.end()) ? ov->second : _mattr_at(tx, tz);
    return a < 0 ? 0 : a & 0xFF;
}

// ray-triangle Möller–Trumbore, rayon (0,-1,0) depuis `p` vers le bas
static bool tri_ground(Vec3 a, Vec3 b, Vec3 c, const Vec3& o, float& t) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 d{0, -1, 0};
    Vec3 pv = d.cross(e2);
    float det = e1.dot(pv);
    if (fabsf(det) < 1e-7f) return false;
    float inv = 1.f / det;
    Vec3 tv = o - a;
    float u = tv.dot(pv) * inv;
    if (u < -1e-4f || u > 1.0001f) return false;
    Vec3 qv = tv.cross(e1);
    float v = d.dot(qv) * inv;
    if (v < -1e-4f || u + v > 1.0001f) return false;
    t = e2.dot(qv) * inv;
    return t >= 0;
}

// segment-triangle générique (Möller–Trumbore) -> t le long de dir
static bool seg_hit(Vec3 a, Vec3 b, Vec3 c, const Vec3& o, const Vec3& d,
    float& t) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 pv = d.cross(e2);
    float det = e1.dot(pv);
    if (fabsf(det) < 1e-7f) return false;
    float inv = 1.f / det;
    Vec3 tv = o - a;
    float u = tv.dot(pv) * inv;
    if (u < -1e-4f || u > 1.0001f) return false;
    Vec3 qv = tv.cross(e1);
    float v = d.dot(qv) * inv;
    if (v < -1e-4f || u + v > 1.0001f) return false;
    t = e2.dot(qv) * inv;
    return t > 0.01f;
}

// plus proche obstacle entre `from` et `from+dir` -> t ∈ [0,1] (1 = libre)
float World::_cam_hit(const Vec3& from, const Vec3& dir) const {
    float best = 1.f;
    Vec3 end = from + dir;
    int x0 = (int)floorf(std::min(from.x, end.x) / CELL) - 1;
    int x1 = (int)floorf(std::max(from.x, end.x) / CELL) + 1;
    int y0 = (int)floorf(std::min(from.z, end.z) / CELL) - 1;
    int y1 = (int)floorf(std::max(from.z, end.z) / CELL) + 1;
    for (int cx = x0; cx <= x1; cx++)
        for (int cy = y0; cy <= y1; cy++) {
            auto it = _cells.find(ckey(cx, cy));
            if (it == _cells.end()) continue;
            for (size_t i = 0; i + 2 < it->second->col_tris.size(); i += 3) {
                float t;
                if (seg_hit(it->second->col_tris[i], it->second->col_tris[i + 1],
                        it->second->col_tris[i + 2], from, dir, t) && t < best)
                    best = t;
            }
            for (size_t i = 0; i + 2 < it->second->prop_tris.size(); i += 3) {
                float t;
                if (seg_hit(it->second->prop_tris[i], it->second->prop_tris[i + 1],
                        it->second->prop_tris[i + 2], from, dir, t) && t < best)
                    best = t;
            }
        }
    return best;
}

float World::_ground_y(const Vec3& p, float from) const {
    // plaques BDHC de la cellule (CalculateObjectHeight DS) : la plaque
    // contenant (x,z) donne y par son équation de plan ; plusieurs
    // plaques empilées -> la plus proche de la hauteur courante.
    int cx = (int)floorf(p.x / CELL), cy = (int)floorf(p.z / CELL);
    float best = -1e9f, bestd = 1e18f;
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) {
            auto it = _cells.find(ckey(cx + dx, cy + dz));
            if (it == _cells.end() || !it->second->land) continue;
            const Cell* c = it->second.get();
            float lx = p.x - c->org.x, lz = p.z - c->org.z;
            for (auto& pl : c->land->plates) {
                if (lx < pl.x0 || lx > pl.x1 || lz < pl.z0 || lz > pl.z1)
                    continue;
                if (fabsf(pl.ny) < 1e-5f) continue;
                // plaque locale -> monde : ajouter l'altitude de la cellule
                float y = -(pl.nx * lx + pl.nz * lz + pl.c) / pl.ny + c->org.y;
                float d = fabsf(y - from);
                if (getenv("PLDBG"))
                    printf("[pl] p=%.0f,%.0f loc=%.0f,%.0f cell=%d,%d bbox %.0f..%.0f,%.0f..%.0f -> y=%.1f (from=%.0f)\n",
                        p.x, p.z, lx, lz, c->cx, c->cy,
                        pl.x0, pl.x1, pl.z0, pl.z1, y, from);
                if (d < bestd) { bestd = d; best = y; }
            }
        }
    // Hearthome : plaque dynamique 0 = ascenseur DP — les tuiles du lift ont
    // pour hauteur son y courant (DynamicTerrainHeightManager DS), mais
    // seulement quand le point requêté est déjà à peu près à cette hauteur :
    // sinon le joueur au sol marche sous un lift resté en haut (le DS ne
    // pose jamais SetPlate pour ce lift — la plaque reste invalide).
    if (_gym.kind == 5 && _gym.hh_lift_tx0 >= 0) {
        int tx = (int)floorf(p.x / TILE), tz = (int)floorf(p.z / TILE);
        if (tx >= _gym.hh_lift_tx0 && tx <= _gym.hh_lift_tx1 &&
            tz >= _gym.hh_lift_tz0 && tz <= _gym.hh_lift_tz1 &&
            fabsf(p.y - _gym.hh_lift_y) < TILE)
            return _gym.hh_lift_y;
    }
    // platform_lift.c : plaque dynamique 0 = lift Iron Island / Ligue
    // (SetPlate 3x2 à l'init, SetHeight au fil du trajet) — même garde de
    // proximité qu'Hearthome : hors de la hauteur courante, le BDHC reprend
    // la main (le joueur au sol passe sous un lift resté en haut).
    if (_gym.kind == 6 && _gym.pl_lift >= 0) {
        int tx = (int)floorf(p.x / TILE), tz = (int)floorf(p.z / TILE);
        if (tx >= _gym.pl_tx && tx <= _gym.pl_tx + 2 &&
            tz >= _gym.pl_tz && tz <= _gym.pl_tz + 1 &&
            fabsf(p.y - _gym.pl_y) < TILE)
            return _gym.pl_y;
    }
    // Monde Distorsion : la plateforme mobile est le sol de sa tuile
    // (au-dessus du vide ou en mouvement, sa surface courante prime).
    // la proximité se mesure sur p.y (hauteur de l'objet), pas `from`
    // (référence de recherche biaisée +60).
    if (_dw.on) {
        float pg = _dw_plat_ground((int)floorf(p.x / TILE),
                                   (int)floorf(p.z / TILE), p.y);
        if (pg > -1e8f) return pg;
    }
    if (best > -1e8f) {
        // plaque dynamique (arène Pastoria) : sur une tuile 0x59 la hauteur
        // dynamique l'emporte quand elle dépasse le terrain (nappe d'eau)
        if (_gym.kind == 1 && _gym.plate_h > best &&
            _tile_beh((int)floorf(p.x / TILE),
                      (int)floorf(p.z / TILE)) == 0x59)
            return (float)_gym.plate_h;
        return best;
    }
    // Monde Distorsion : tuile franchissable sans plaque = sol invisible
    // (DS CALCULATED_HEIGHT_SOURCE_NONE -> pos.y inchangé ; les ponts de la
    // salle Giratina reposent là-dessus, marqueurs 0x5A/0x5B non bloquants).
    if (_dw.on) {
        const Cell* bc = _cell_at(p.x, p.z);
        if (bc && bc->land) {
            int a2 = bc->land->attr_at((int)floorf(p.x / TILE) - bc->cx * 32,
                                       (int)floorf(p.z / TILE) - bc->cy * 32);
            int b2 = a2 & 0xFF;
            if ((a2 & 0x8000) == 0 || (b2 >= 0x5A && b2 <= 0x5D))
                return p.y;
        }
    }
    // secours : raycast triangles terrain (cartes sans BDHC)
    Vec3 o{p.x, from, p.z};
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) {
            auto it = _cells.find(ckey(cx + dx, cy + dz));
            if (it == _cells.end()) continue;
            for (size_t i = 0; i + 2 < it->second->col_tris.size(); i += 3) {
                float t;
                if (tri_ground(it->second->col_tris[i], it->second->col_tris[i + 1],
                        it->second->col_tris[i + 2], o, t)) {
                    float y = o.y - t;
                    if (y > best) best = y;
                }
            }
        }
    return best;
}

// localID -> index du 1er objet VIVANT (DS MapObjMan_LocalMapObjByIndex :
// parcourt les MapObject spawnés dans l'ordre ; les ids ne sont pas uniques,
// ex. Route 201 : Sorbier ET un panneau partagent localID 5 — viser le 1er).
int World::_live_obj(int id) const {
    for (size_t i = 0; i < _objects.size(); i++)
        if (_objects[i].id == id && !_objgone.count((int)i)) return (int)i;
    return -1;
}

// gfx d'objet event -> gfx réel. DS MapObject_GetFieldSystemGraphicsID :
// OBJ_EVENT_GFX_VAR_0..F (101-116) -> FieldSystem_GetGraphicsID ->
// var[0x4020 + n] (VAR_OBJ_GFX_ID_0..F, remplis par le script OnTransition —
// 0x4020 = VARS_START + 32 map-locales, vars_flags.txt l.4143).
int World::_resolve_gfx(int gfx) const {
    if (gfx >= 101 && gfx <= 116) return _vm.var(0x4020 + gfx - 101);
    return gfx;
}
