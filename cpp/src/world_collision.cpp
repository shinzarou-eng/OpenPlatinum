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
    const Cell* c = _cell_at(p.x, p.z);
    if (!c || !c->land) return true;      // hors cellule chargée = mur
    int lc = tx - c->cx * 32;
    int lr = tz - c->cy * 32;
    if (getenv("ATTRDBG"))
        fprintf(stderr, "[attr] t=(%d,%d) lc,lr=(%d,%d) a=%04x\n",
            tx, tz, lc, lr, c->land->attr_at(lc, lr));
    return (c->land->attr_at(lc, lr) & 0x8000) != 0;
}

// comportement de tuile = octet bas de l'attribut (map_tile_behavior.h)
int World::_tile_beh(int tx, int tz) const {
    auto ov = _tileover.find(((int64_t)tx << 32) | (uint32_t)tz);
    const Cell* c = _cell_at(tx * TILE + TILE / 2.f, tz * TILE + TILE / 2.f);
    int a = (ov != _tileover.end()) ? ov->second
        : (c && c->land) ? c->land->attr_at(tx - c->cx * 32, tz - c->cy * 32) : 0;
    return a & 0xFF;
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
                float y = -(pl.nx * lx + pl.nz * lz + pl.c) / pl.ny;
                float d = fabsf(y - from);
                if (getenv("PLDBG"))
                    printf("[pl] p=%.0f,%.0f loc=%.0f,%.0f cell=%d,%d bbox %.0f..%.0f,%.0f..%.0f -> y=%.1f (from=%.0f)\n",
                        p.x, p.z, lx, lz, c->cx, c->cy,
                        pl.x0, pl.x1, pl.z0, pl.z1, y, from);
                if (d < bestd) { bestd = d; best = y; }
            }
        }
    if (best > -1e8f) return best;
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
