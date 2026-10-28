// world_move.cpp — ApplyMovement, déambulation PNJ, vue dresseurs, suiveur.
#include "world.h"
#include "world_internal.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>

void World::_queue_move(int obj, size_t off) {
    if (!_scrnarc || _header < 0 || _vm_member < 0) return;
    const Bytes* f = &_scrnarc->member(_vm_member);
    if (!f || off >= f->size()) return;
    MoveJob mj; mj.obj = obj;
    size_t p = off;
    while (p + 3 < f->size()) {
        uint16_t c = u16(*f, p), a = u16(*f, p + 2);
        p += 4;
        if (c == 0xFE) break;
        mj.st.push_back({c, a});
    }
    if (verbose || getenv("MOVDBG")) {
        printf("[mv] obj=%d off=%zx cmds:", obj, off);
        for (auto& s : mj.st) printf(" %02X/%d", s.first, s.second);
        printf("\n");
    }
    _moves.push_back(std::move(mj));
}

bool World::_moves_done() const {
    for (auto& m : _moves) if (!m.done) return false;
    return true;
}


// Table des mouvements DS (unk_020655F4.c MovementAction_InitWalk) :
// dist = px/frame à 60 Hz ; tuiles/s = dist × 60/16. cmd&3 = dir ROM.
static float mv_speed(int c) {
    if (c >= 0x04 && c <= 0x07) return 3.75f * .5f;  // WalkSlower 0.5px/f → 1.875
    if (c >= 0x08 && c <= 0x0B) return 3.75f;        // WalkSlow 1px/f → 3.75
    if (c >= 0x0C && c <= 0x0F) return 3.75f * 2;    // WalkNormal 2px/f → 7.5
    if (c >= 0x10 && c <= 0x13) return 3.75f * 4;    // WalkFast 4px/f → 15
    if (c >= 0x14 && c <= 0x17) return 3.75f * 8;    // WalkFaster 8px/f → 30
    if (c >= 0x4C && c <= 0x4F) return 3.75f * 2.67f;// SlightlyFast 16px/6fr → 10
    if (c >= 0x50 && c <= 0x53) return 3.75f * 5.33f;// SlightlyFaster 16px/3fr → 20
    if (c >= 0x54 && c <= 0x57) return 3.75f * 16;   // WalkFastest 16px/f → 60
    if (c >= 0x58 && c <= 0x5B) return 3.75f * 4;    // Run 4px/f → 15
    if (c >= 0x60 && c <= 0x63) return 3.75f * 2.29f;// ESSFast ~16px/7fr → 8.6
    if (c >= 0x34 && c <= 0x3B) return 3.75f * 2;    // JumpNear/Far 2px/f → 7.5
    if (c >= 0x5C && c <= 0x5D) return 3.75f;        // JumpNearSlow 1px/f → 3.75
    if (c >= 0x5E && c <= 0x5F) return 3.75f * 4;    // JumpFarther 4px/f → 15
    return 0;
}
static int mv_tiles(int c) {                      // portée en tuiles (sauts)
    if (c >= 0x38 && c <= 0x3B) return 2;           // JumpFar
    if (c >= 0x5E && c <= 0x5F) return 3;           // JumpFarther
    return 1;
}
static bool mv_isjump(int c) {
    return (c >= 0x34 && c <= 0x3B) || (c >= 0x5C && c <= 0x5F);
}

void World::_tick_moves(float dt) {
    for (auto& m : _moves) {
        if (m.done) continue;
        Vec3* pos = nullptr; int* od = nullptr; int oi = -1;
        if (m.obj == 0xFF) { pos = &_ppos; od = &_pdir; }
        else if (m.obj == 0xF2) {                    // LOCALID_FOLLOWER : suiveur
            if (_follower >= 0) { oi = _follower;
                pos = &_objpos[oi]; od = &_objd[oi]; } }
        else {
            oi = _live_obj(m.obj);                  // DS : 1er objet VIVANT —
            if (oi >= 0) { pos = &_objpos[oi]; od = &_objd[oi]; }
        }
        if (!pos) { m.done = true; continue; }
        // cycle de marche DS : alimente _objw (PNJ) / _panim (joueur) —
        // persiste entre les pas d'une même série, coupé sur les autres
        // commandes pour éviter la frame debout au milieu d'une marche.
        auto anim = [&](bool on, float rate) {
            if (oi >= 0) {
                _objw[oi].dx = on ? 1 : 0; _objw[oi].dz = 0;
                if (on) _objw[oi].phase += dt * rate;
            } else {
                if (on) { _panim += dt * rate;
                          if (_panim > 4.f) _panim -= 4.f; }
                else _panim = 0.f;
            }
        };
        if (m.t > 0) {                          // pause (Delay/WalkOnSpot/hop)
            m.t -= dt;
            if (m.spot) anim(true, 10.f);       // WalkOnSpot : cycle de marche
            else if (m.hop <= 0) anim(false, 0.f); // Delay : debout
            if (m.hop > 0) {                    // JumpOnSpot : arc vertical DS
                float k = 1.f - m.t / m.hopt;
                pos->y = m.y0 + sinf(k * 3.14159f) * TILE * 0.45f;
                if (m.t <= 0) { pos->y = m.y0; m.hop = 0; }
            }
            if (m.t > 0) continue;
            if (m.spot) { m.spot = false; anim(false, 0.f); }
        }
        // ── déplacement en cours ──
        if (m.dx || m.dz) {
            float step = m.spd * TILE * dt;
            m.dist += step;
            anim(true, m.spd >= 15.f ? 16.f : 10.f);
            if (m.dist >= TILE * m.nt - 0.01f) {      // arrive : snap tuile cible
                // cible = tuile source + dir*nt (pos a déjà franchi le centre —
                // snapper sur la tuile contenant pos + un delta double le pas)
                pos->x = (m.x0 + m.dx * m.nt) * TILE + TILE / 2.f;
                pos->z = (m.z0 + m.dz * m.nt) * TILE + TILE / 2.f;
                if (m.jump) pos->y = m.y0;
                m.dx = m.dz = 0; m.dist = 0; m.jump = 0;
                m.i++; continue;
            }
            pos->x += m.dx * step; pos->z += m.dz * step;
            if (m.jump) {                             // arc = sJumpHeights_High DS
                static const int JH[16] = {4,6,8,10,11,12,12,12,11,10,9,8,6,4,0,0};
                float k = m.dist / (TILE * m.nt);
                int ji = (int)(k * 15.99f); if (ji < 0) ji = 0;
                pos->y = m.y0 + JH[ji];
            } else {
                // la hauteur suit les plaques BDHC en continu (escaliers
                // inclinés : descente progressive comme sur DS)
                float g = _ground_y(*pos, pos->y + 40.f);
                if (g > -1e8f) pos->y = g;
            }
            continue;
        }
        anim(false, 0.f);                         // commande debout
        if (m.i >= m.st.size()) { m.done = true; continue; }
        int c = m.st[m.i].first, a = m.st[m.i].second;
        if (c <= 3) {                                  // Face N/S/O/E (dir ROM)
            if (od && !m.lockdir) *od = R2W[c & 3];
            m.i++; continue;
        }
        float spd = mv_speed(c);
        if (spd > 0) {                                 // Walk/Jump avec vitesse DS
            if (getenv("MOVDBG"))
                printf("[mvstep] obj=%d i=%zu cmd=%02X a=%d pos=(%.0f,%.0f)\n",
                    m.obj, m.i, c, a, pos->x, pos->z);
            int wd = R2W[c & 3];
            if (od && !m.lockdir) *od = wd;
            dirv(wd, m.dx, m.dz);
            m.x0 = (int)floorf(pos->x / TILE); m.z0 = (int)floorf(pos->z / TILE);
            m.spd = spd; m.nt = mv_tiles(c); m.dist = 0;
            m.jump = mv_isjump(c) ? 1 : 0; m.y0 = pos->y;
            if (!m.jump && a > 1)                      // marche : arg = nb de pas
                m.st.insert(m.st.begin() + m.i + 1, {c, a - 1});
            continue;
        }
        if (c >= 0x18 && c <= 0x33) {                  // WalkOnSpot / JumpOnSpot
            if (od && !m.lockdir) *od = R2W[c & 3];
            int f = (c <= 0x1B) ? 32 : (c <= 0x1F) ? 16 : (c <= 0x23) ? 8
                : (c <= 0x27) ? 4 : (c <= 0x2B) ? 2 : (c <= 0x2F) ? 16 : 8;
            m.t = f / 60.f;                              // durées = frames à 60 Hz
            if (c >= 0x2C) { m.hop = m.hopt = m.t; m.y0 = pos->y; } // saut sur place
            else m.spot = true;                          // marche sur place
            m.i++; continue;
        }
        if (c >= 0x3C && c <= 0x42) {                  // Delay1/2/4/8/15/16/32
            static const int DL[7] = {1, 2, 4, 8, 15, 16, 32};
            m.t = DL[c - 0x3C] / 60.f; m.i++; continue;
        }
        if (c == 0x44) {                               // TeleportDown/WarpIn
            pos->y += TILE * 6;                        // tombe du ciel -> haut
            if (oi >= 0) _objhide.erase(oi);
            m.i++; continue;
        }
        if (c == 0x45 || c == 0x46) {                  // SetInvisible/SetVisible
            if (m.obj == 0xFF) { /* joueur : pas de cache pour l'instant */ }
            else if (oi >= 0) {
                if (c == 0x45) _objhide.insert(oi);
                else _objhide.erase(oi);
            }
            m.i++; continue;
        }
        if (c == 0x47) { m.lockdir = true; m.i++; continue; }   // LockDir
        if (c == 0x48) { m.lockdir = false; m.i++; continue; }  // UnlockDir
        if (c == 0x4B || c == 0x67) {           // Emote "!" / "!!" DS
            _objemote[oi >= 0 ? oi : 0xFF] = (c == 0x67) ? 1.2f : 0.9f;
            if (verbose) printf("[emote] obj=%d i=%d\n", m.obj, oi);
            m.t = (c == 0x67) ? 1.2f : 0.9f; m.i++; continue;
        }
        if (c == 0x64) { if (od) *od = 0; m.t = 0.5f; m.i++; continue; } // NurseBow
        if (c == 0x66 || c == 0x68) { m.t = 0.45f; m.i++; continue; } // Give/Receive
        if (c == 0x65) { if (oi >= 0) _objhide.erase(oi);
            m.t = 0.3f; m.i++; continue; } // RevealTrainer
        m.i++;                                         // reste : no-op
    }
    for (auto it = _objemote.begin(); it != _objemote.end();) {
        it->second -= dt;
        if (it->second <= 0.f) it = _objemote.erase(it); else ++it;
    }
    // TeleportDown : les objets avec pos->y surélevée retombent doucement
    for (size_t i = 0; i < _objects.size(); i++) {
        Vec3& p = _objpos[i];
        if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
        float gy = _ground_y(p, p.y + 2.f);
        if (gy > -1e8f && p.y > gy + 1.f) p.y = std::max(gy, p.y - 90.f * dt);
    }
}

// ── déambulation PNJ (move type DS : 2-5 marche, 12-15 regarde autour) ────────
void World::_tick_wander(float dt) {
    if (_lockall) return;                            // LockAll : tout gelé
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        if (!_objmv[i] || _objgone.count((int)i) || _objhide.count((int)i)
            || _objlock.count((int)i) || (int)i == _follower
            || (int)i == _spot.obj) continue;          // en approche (spot)
        bool scripted = false;
        for (auto& m : _moves)
            if (!m.done && m.obj != 0xFF) {
                int tgt = (m.obj == 0xF2) ? _follower : _live_obj(m.obj);
                if (tgt == (int)i) { scripted = true; break; }
            }
        if (scripted) continue;
        auto& w = _objw[i];
        Vec3& p = _objpos[i];
        if (w.dx || w.dz) {                       // marche PNJ : WalkNormal DS = 2px/f
            w.phase += dt * 10.f;                  // cycle frames 1-2
            float step = 120.f * dt;
            if (w.dx) {
                p.x += step * w.dx;
                if ((w.dx > 0 && p.x >= w.tgt) || (w.dx < 0 && p.x <= w.tgt)) {
                    p.x = w.tgt; w.dx = 0; }
            } else {
                p.z += step * w.dz;
                if ((w.dz > 0 && p.z >= w.tgt) || (w.dz < 0 && p.z <= w.tgt)) {
                    p.z = w.tgt; w.dz = 0; }
            }
            if (!w.dx && !w.dz) {                   // fin de pas : snap sol DS
                p.y = _ground_y(p, p.y + 40.f);
            } else {
                float g = _ground_y(p, p.y + 40.f);   // suit les pentes
                if (g > -1e8f) p.y = g;
            }
            continue;
        }
        w.idle -= dt;
        if (w.idle > 0) continue;
        w.idle = (0x10 << (rand() & 3)) / 60.f;    // Unk_020EEA88 : 16/32/48/64 fr
        int mv = _objmv[i];
        // types DS (generated/movement_types.txt) : dir monde 0=S 1=N 2=O 3=E
        if (mv >= 14 && mv <= 17) continue;                    // LOOK_N/S/O/E fixe
        if (mv == 2) { _objd[i] = rand() & 3; continue; }      // LOOK_AROUND
        if (mv >= 6 && mv <= 13) {                             // LOOK sous-ensemble
            static const int LK[8][3] = {{1,2,0},{1,3,0},{0,2,0},{0,3,0},
                {1,0,2},{1,0,3},{1,2,3},{0,2,3}};              // NO,NE,SO,SE,NSO,NSE,NOE,SOE
            _objd[i] = LK[mv - 6][rand() % (mv <= 9 ? 2 : 3)]; continue;
        }
        if (mv == 45 || mv == 46) {                            // LOOK_N/S ou O/E alternés
            _objd[i] = (mv == 45) ? (rand() & 1) : (2 + (rand() & 1)); continue;
        }
        if (mv == 18) {                                        // ROTATE_CCW : N→O→S→E
            static const int CCW[4] = {3, 2, 0, 1};
            _objd[i] = CCW[_objd[i] & 3]; continue;
        }
        if (mv == 19) {                                        // ROTATE_CW : N→E→S→O
            static const int CW[4] = {2, 3, 1, 0};
            _objd[i] = CW[_objd[i] & 3]; continue;
        }
        static const int DR[4][3] = {{0,1,0},{0,-1,1},{-1,0,2},{1,0,3}};
        if (mv == 20 || (mv >= 21 && mv <= 44)) {              // va-et-vient / cycles 4-dir
            // 24 permutations DS (movement_types.txt) : indices dir monde 0=S 1=N 2=O 3=E
            static const int CYC[24][4] = {
                {1,3,2,0},{3,2,0,1},{0,1,3,2},{2,0,1,3},        // NEWS,EWSN,SNEW,WSNE
                {2,3,0,1},{1,2,3,0},{0,1,2,3},{3,0,1,2},        // WESN,NWES,SNWE,ESNW
                {2,1,0,3},{1,0,3,2},{3,2,1,0},{0,3,2,1},        // WNSE,NSEW,EWNS,SEWN
                {3,1,0,2},{1,0,2,3},{2,3,1,0},{0,2,3,1},        // ENSW,NSWE,WENS,SWEN
                {1,2,0,3},{0,3,1,2},{2,0,3,1},{3,1,2,0},        // NWSE,SENW,WSEN,ENWS
                {1,3,0,2},{0,2,1,3},{2,1,3,0},{3,0,2,1}};       // NESW,SWNE,WNES,ESWN
            int d;
            if (mv == 20) {                                    // va-et-vient sur l'axe range
                if (!w.step) w.step = (o.rx >= o.rz) ? 4 : 1;  // E ou S d'abord
                d = (w.step - 1) & 3;
            } else {
                d = CYC[mv - 21][w.step++ & 3];
            }
            int dx = DR[d][0], dz = DR[d][1];
            Vec3 np{p.x + dx * TILE, p.y, p.z + dz * TILE};
            _objd[i] = DR[d][2];                               // tourne même si bloqué
            if (fabsf(np.x - w.home.x) > o.rx * TILE + 1 ||
                fabsf(np.z - w.home.z) > o.rz * TILE + 1 ||
                _blocked(np)) {
                if (mv == 20) w.step = ((d + 2) & 3) + 1;      // inverse au bord
                continue;
            }
            w.dx = dx; w.dz = dz; w.tgt = dx ? np.x : np.z;
            continue;
        }
        if (mv != 3 && mv != 4 && mv != 5) continue;           // 3-5 = déambule
        // WANDER : 1 tuile dans la boîte rx,rz ; 4=N/S seul, 5=O/E seul
        int ordre[4] = {0, 1, 2, 3};
        for (int t = 0; t < 4; t++) {
            int j = rand() % 4, tmp = ordre[t];
            ordre[t] = ordre[j]; ordre[j] = tmp;
        }
        for (int t = 0; t < 4; t++) {
            int d = ordre[t], dx = DR[d][0], dz = DR[d][1];
            if (mv == 4 && dx) continue;                       // N/S seulement
            if (mv == 5 && dz) continue;                       // O/E seulement
            Vec3 np{p.x + dx * TILE, p.y, p.z + dz * TILE};
            if (fabsf(np.x - w.home.x) > o.rx * TILE + 1) continue;
            if (fabsf(np.z - w.home.z) > o.rz * TILE + 1) continue;
            if (_blocked(np)) continue;
            if (fabsf(np.x - _ppos.x) < TILE && fabsf(np.z - _ppos.z) < TILE)
                continue;
            bool occ = false;
            for (size_t j = 0; j < _objects.size(); j++)
                if (j != i && fabsf(np.x - _objpos[j].x) < TILE / 2 &&
                    fabsf(np.z - _objpos[j].z) < TILE / 2) { occ = true; break; }
            if (occ) continue;
            _objd[i] = DR[d][2];
            w.dx = dx; w.dz = dz;
            w.tgt = dx ? np.x : np.z;
            break;
        }
    }
}

// vue des dresseurs (trainer_encounter.c) : data[0]=portée, trainerType 1=
// direction regardée, 2=toutes directions, 4-8=rotations (vue = facing).
// Ligne de vue dégagée (pas de tuile bloquée entre) + dresseur non vaincu
// (flag 1364+trainerID, TRAINER_DEFEATED_FLAGS_START).
bool World::_check_trainer_sight() {
    if (_spot.obj >= 0) return false;
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
        int tt = o.trainer;
        if (tt == 0 || tt == 3 || tt == 9) continue;      // pas un dresseur vue
        int range = o.d0;
        if (!range || o.script < 3000) continue;
        int tid = (o.script >= 5000) ? o.script - 4999 : o.script - 2999;
        if (_vm.flag(1364 + tid)) continue;               // déjà vaincu
        int ox = (int)floorf(_objpos[i].x / TILE);
        int oz = (int)floorf(_objpos[i].z / TILE);
        int dirs[4] = {_objd[i] & 3, 0, 0, 0}, nd = 1;
        if (tt == 2) { dirs[0]=0; dirs[1]=1; dirs[2]=2; dirs[3]=3; nd = 4; }
        for (int k = 0; k < nd; k++) {
            int ddx, ddz; dirv(dirs[k], ddx, ddz);
            int dx = ptx - ox, dz = ptz - oz;
            if ((ddx && dz) || (ddz && dx)) continue;     // hors de la ligne
            int dist = ddx ? dx * ddx : dz * ddz;
            if (dist < 1 || dist > range) continue;
            bool clear = true;                          // IsPathInterrupted
            for (int s = 1; s < dist; s++)
                if (_blocked({(ox + ddx * s) * TILE + TILE / 2, _ppos.y + 4,
                        (oz + ddz * s) * TILE + TILE / 2})) { clear = false; break; }
            if (!clear) continue;
            _spot.obj = (int)i; _spot.phase = 1; _spot.t = 0.75f;
            _spot.dx = ddx; _spot.dz = ddz;
            _objd[i] = dirs[k];                          // face au joueur
            _objemote[(int)i] = 0.8f;                    // bulle "!"
            if (verbose) printf("[spot] obj %d dist=%d dir=%d\n",
                o.id, dist, dirs[k]);
            return true;
        }
    }
    return false;
}

void World::_tick_spot(float dt) {
    if (_spot.obj < 0) return;
    auto& o = _objects[_spot.obj];
    Vec3& p = _objpos[_spot.obj];
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    int ox = (int)floorf(p.x / TILE), oz = (int)floorf(p.z / TILE);
    int rem = abs(ptx - ox) + abs(ptz - oz) - 1;         // tuiles avant contact
    switch (_spot.phase) {
    case 1:                                            // pause bulle "!"
        _spot.t -= dt; if (_spot.t <= 0) _spot.phase = 2;
        break;
    case 2:                                            // approche (WalkNormal)
        if (rem <= 0) { _spot.phase = 3; _spot.t = 8.f / 60.f; break; }
        {
            float step = 120.f * dt;
            float txv = (ox + _spot.dx) * TILE + TILE / 2.f;
            float tzv = (oz + _spot.dz) * TILE + TILE / 2.f;
            if (_spot.dx) {
                p.x += _spot.dx * step;
                if ((_spot.dx > 0 && p.x >= txv) || (_spot.dx < 0 && p.x <= txv))
                    p.x = txv;
            } else {
                p.z += _spot.dz * step;
                if ((_spot.dz > 0 && p.z >= tzv) || (_spot.dz < 0 && p.z <= tzv))
                    p.z = tzv;
            }
        }
        break;
    case 3:                                            // délai 8 frames DS
        _spot.t -= dt;
        if (_spot.t <= 0) {                            // joueur face au dresseur
            int fx = ox - ptx, fz = oz - ptz;
            _pdir = abs(fz) >= abs(fx) ? (fz > 0 ? 0 : 1) : (fx < 0 ? 2 : 3);
            _spot.phase = 4;
        }
        break;
    case 4:                                            // script de combat
        _talk_obj = _spot.obj;                         // TARGET_OBJECT = dresseur
        _start_script(o.script); _vm.step();
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
        _spot.obj = -1;
        break;
    }
}

// téléport debug (--pos) : stream les cellules + colle au sol