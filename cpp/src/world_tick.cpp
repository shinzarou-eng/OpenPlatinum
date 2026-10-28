// world_tick.cpp — update joueur, déclencheurs, rencontres, combat.
#include "world.h"
#include "world_internal.h"
#include "spr.h"
#include "sdat.h"
#include <cstdio>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <algorithm>

// ── update ────────────────────────────────────────────────────────────────────
void World::update(float dt, float ix, float iz) {
    if (_header < 0) return;
    static int inpdbg = -1;
    if (inpdbg < 0) inpdbg = getenv("INPDBG") != nullptr;
    if (inpdbg && (ix || iz))
        printf("[inp] ix=%.0f iz=%.0f frozen=%d busy=%d wt=%d wild=%.2f "
            "spot=%d lost=%d yesno=%d pstep=%d pdir=%d\n",
            ix, iz, (int)ui_frozen, (int)_vm.busy(), _wt.st, _wild_t,
            _spot.obj, (int)_lost_pc, (int)_yesno, _pstep, _pdir);
    if (ui_frozen) {                            // menu X : monde figé (DS)
        ix = iz = 0;
        dt = 0;
    }
    // gravité pour les sauts/chutes (steps y gérés par RecalculateObjectHeight)
    const float GRAV = 400.f;

    // tick des mouvements scriptés + reprise du VM en attente
    _tick_moves(dt);
    _tick_wander(dt);

    // snap paresseux des objets au sol : la cellule terrain peut arriver
    // après le chargement des events — retente tant que _ground_y échoue.
    // AVANT les early-returns (VM busy) : les PNJ doivent être posés même
    // pendant qu'un script tourne (sinon invisibles en intérieur).
    static bool snapdbg = getenv("SNAPDBG") != nullptr;
    static float opdbg_t = 0;
    if (getenv("OPDBG")) {
        opdbg_t += dt;
        if (opdbg_t > 0.25f) {
            opdbg_t = 0;
            for (size_t i = 0; i < _objpos.size(); i++)
                printf("[opos] i=%zu id=%d pos=(%.1f,%.1f,%.1f) gone=%d hide=%d\n",
                    i, _objects[i].id, _objpos[i].x, _objpos[i].y, _objpos[i].z,
                    (int)_objgone.count((int)i),
                    (int)_objhide.count((int)i));
        }
    }
    for (size_t i = 0; i < _objpos.size(); i++) {
        Vec3& p = _objpos[i];
        if (p.y < -1e8f) {
            // référence = event.y (fx32) comme RecalculateObjectHeight DS
            float g = _ground_y(p, _objects[i].y / 4096.f);
            if (snapdbg) printf("[snap] i=%zu pos=%.0f,%.0f g=%.0f\n",
                i, p.x, p.z, g);
            if (g > -1e8f) p.y = g;   // sentinelle conservée sinon -> réessaie
        }
    }

    if (_banner_t > 0) _banner_t -= dt;
    _tick_prop_anims(dt);                        // portes : avance les anims
    _flt_step(dt);                               // textures animées (eau, fleurs)
    if (_mbox && _dlg_chars < (float)(dlg_page_end() - _dlg_page))
        _dlg_chars += dt * (g_opts.text_speed == 0 ? 7.5f      // DS lent
            : g_opts.text_speed == 2 ? 45.f : 15.f);          // normal/rapide
    if (_fade.t >= 0) {                       // FadeScreen en cours
        _fade.t += dt;
        if (_fade.t >= _fade.dur) _fade.t = -1;
    }
    if (_vm.state() == ScriptVM::YESNO && !_yesno) _yesno = true;
    if (_vm.state() == ScriptVM::MOVE) {
        if (_moves_done()) {
            if (verbose) printf("[mv-done] reprise script pc=%d\n", _vm.pc());
            _vm.continue_after(ScriptVM::MOVE); _vm.step();
            if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
        } else if (verbose) {
            _mvdbg += dt;                       // dump jobs coincés toutes les 2s
            if (_mvdbg > 2.f) {
                _mvdbg = 0;
                for (auto& m : _moves)
                    if (!m.done)
                        printf("[mv-stuck] obj=%d i=%zu/%zu cmd=%02X a=%d "
                            "t=%.2f dist=%.0f dx=%d dz=%d\n",
                            m.obj, m.i, m.st.size(),
                            m.i < m.st.size() ? m.st[m.i].first : -1,
                            m.i < m.st.size() ? m.st[m.i].second : 0,
                            m.t, m.dist, m.dx, m.dz);
            }
        }
    }
    if (_vm.state() == ScriptVM::WAITTIME && --_vm.wait_t <= 0) {
        _vm.continue_after(ScriptVM::WAITTIME); _vm.step();
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
    }
    if (_vm.state() == ScriptVM::WAITFADE && _fade.t < 0) {
        _vm.continue_after(ScriptVM::WAITFADE); _vm.step();
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
    }
    if (_vm.state() == ScriptVM::WAITANIM &&
        (!_vm.cb_doorbusy || !_vm.cb_doorbusy(_vm.wait_anim))) {
        _vm.continue_after(ScriptVM::WAITANIM); _vm.step();
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
    }
    if (_vm.state() == ScriptVM::WAITSND &&
        (!_vm.cb_sndbusy || !_vm.cb_sndbusy(_vm.wait_snd))) {
        _vm.continue_after(ScriptVM::WAITSND); _vm.step();
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
    }
    // bascule BGM jour/nuit (~1 s de sondage)
    _mus_chk += dt;
    if (_mus_chk > 1.f) { _mus_chk = 0; _music_update(); }
    if (_wt.st >= 0) {                          // transition de map en cours :
        _tick_warp(dt);                         // joueur/scripts/pas gelés
        return;
    }
    // combat en cours : monde figé, la VM attend (état BATTLE) si c'est
    // un script qui l'a lancé ; la fuite/KO sauvage n'a pas de script.
    if (_btl.active) {
        static int btldbg = 0;
        if (verbose && (_btl_key || ++btldbg % 60 == 0))
            printf("[btl] st=%d anim=%.2f key=%d q=%d/%d hp=%d/%d\n",
                _btl.st, _btl.anim, _btl_key, _btl.qi, _btl.qn,
                _btl.you().hp, _btl.foe().hp);
        battle_update(_btl, _db, dt, _btl_key); _btl_key = 0;
        if (!_btl.active) _btl_end();
        return;
    }
    if (_vm.state() == ScriptVM::BATTLE) {    // StartTrainerBattle/First/Wild/Legendary
        if (_vm.battle_sp > 0) {
            _btl_start(false, 0, _vm.battle_sp, _vm.battle_lv);
            _vm.battle_sp = 0; return;
        }
        _btl_start(true, _vm.battle_tid, 0, 0); return;
    }
    // DS WaitButton : la croix ne fait que pivoter le joueur (TryFace,
    // movement.c) — le pas n'est pas consommé, le script continue d'attendre.
    if (_vm.state() == ScriptVM::WAITBTN && (ix || iz))
        _pdir = iz < 0 ? 1 : iz > 0 ? 0 : ix < 0 ? 2 : 3;
    if (_vm.busy()) return;                  // joueur verrouillé pendant script
    if (_wild_t > 0) {                       // intro rencontre sauvage en cours
        _wild_t -= dt;
        if (_wild_t <= 0) { _dialog.clear(); _mbox = false; }
        return;
    }
    if (_spot.obj >= 0) { _tick_spot(dt); return; }   // approche dresseur
    // ordre DS (field_control) : frame -> sight -> forcing -> coord -> push.
    if (_lost_pc) {                          // LostGoPc : fondu noir puis réveil
        _lost_pc = false;
        const SpawnLoc& s = SPAWN_TBL[_blackout_id - 1];
        for (auto& p : _vm.party)             // la DS soigne l'équipe au réveil
            p.hp = battle_make(_db, p.species, p.level).maxhp;
        // Location_InitBlackOut : header + tuile de la table, FACE_UP.
        // On réutilise la machine de transition : fondu sortant -> load ->
        // fondu entrant, avec coords imposées après le load.
        _entrance = {_header, -1,
            (int)floorf(_ppos.x / TILE), (int)floorf(_ppos.z / TILE), _pdir};
        _wt.dh = s.blackout_hdr; _wt.dw = -1;
        _wt.type = WT_PLAIN; _wt.adir = 1; _wt.sub = 0; _wt.door = -1;
        _wt.bo_tx = s.bx; _wt.bo_tz = s.bz;   // coords appliquées au WP_LOAD
        _fade = {0.f, 8.f / 60.f, 0, true};
        _wt.st = WP_DEP_FADE;
        return;
    }

    // table frame (init type1) : premier var==var gagne, vérif chaque frame
    {
        int id = _frame_script();
        if (id >= 0) {
            if (verbose) printf("[init] frame -> script %d\n", id);
            static int frhits = 0;
            if (getenv("FRDBG") && ++frhits < 20)
                printf("[frame-dbg] script %d re-déclenche (hit %d)\n", id, frhits);
            _start_script(id); _vm.step();
            return;
        }
    }
    if (_check_trainer_sight()) return;      // dresseur repère le joueur (sight)

    // ── mouvement joueur DS (player_move.c) : machine NONE→TURNING→MOVING ──
    // un pas = exactement 1 tuile ; tap = tourner d'abord, hold = enchaîner
    auto npc_at = [this](int tx, int tz) {            // tuile occupée par un PNJ
        for (size_t i = 0; i < _objects.size(); i++) {
            if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
            if ((int)floorf(_objpos[i].x / TILE) == tx &&
                (int)floorf(_objpos[i].z / TILE) == tz) return true;
        }
        return false;
    };
    int want = iz < 0 ? 1 : iz > 0 ? 0 : ix < 0 ? 2 : ix > 0 ? 3 : -1; // dir monde
    if (_pstep == 2) {                                  // pas en cours -> cible
        float ddx = _ptx - _ppos.x, ddz = _ptz - _ppos.z;
        float dl = sqrtf(ddx * ddx + ddz * ddz);
        if (getenv("PSTEPDBG")) printf("[pstep] pos=(%.1f,%.1f) pt=(%.1f,%.1f) dl=%.2f dt=%.4f\n",
            _ppos.x, _ppos.z, _ptx, _ptz, dl, dt);
        float step = _pstep_spd * dt;
        _panim += dt * (_pstep_spd > 80 ? 16.f : 10.f); // cycle marche/course
        if (_panim > 4.f) _panim -= 4.f;
        if (_pjump) {                              // rebord : arc sJumpHeights DS
            float tot = 2.f * TILE;                // JumpFar = 2 tuiles / 16 fr
            static const int JH[16] = {4,6,8,10,11,12,12,12,11,10,9,8,6,4,0,0};
            int ji = (int)((1.f - dl / tot) * 15.99f);
            if (ji < 0) ji = 0;
            float gy2 = _ground_y(_ppos, _ppos.y + 80.f);
            _ppos.y = (gy2 > -1e8f ? gy2 : _ppos.y) + JH[ji];
        }
        if (dl <= step) {
            _ppos.x = _ptx; _ppos.z = _ptz; _pstep = 0; _pjump = 0;
            // glace DS (field_behavior) : la glissière continue tant qu'on
            // est sur TB_ICE — la direction de marche est forcée
            _pslide = _tile_beh((int)floorf(_ppos.x / TILE),
                (int)floorf(_ppos.z / TILE)) == TB_ICE;
        }
        else { _ppos.x += ddx / dl * step; _ppos.z += ddz / dl * step; }
    } else {
        if (_pstep == 1 || _pstep == 3) {               // turn / bump en cours
            _pstep_t -= dt;
            if (_pstep == 3) { _panim += dt * 8.f; if (_panim > 4.f) _panim -= 4.f; }
            if (_pstep_t <= 0) _pstep = 0;
        }
        if (_pstep == 0) {
            if (_pslide) want = _pdir;              // glace : pas de choix de dir
            if (want < 0) { _panim = 0.f; }
            else if (want != _pdir) {                    // TURNING : face d'abord
                _pdir = want; _pstep = 1; _pstep_t = 2.f / 60.f;
            } else {                                     // MOVING : collision ?
                int ptx = (int)floorf(_ppos.x / TILE);
                int ptz = (int)floorf(_ppos.z / TILE);
                int ddx, ddz; dirv(_pdir, ddx, ddz);
                int ntx = ptx + ddx, ntz = ptz + ddz;
                bool nblock = !noclip && _blocked(
                    {ntx * TILE + TILE / 2, _ppos.y + 4, ntz * TILE + TILE / 2});
                if (getenv("PSTEPDBG"))
                    printf("[want] t=(%d,%d)->(%d,%d) beh=%d block=%d npc=%d\n",
                        ptx, ptz, ntx, ntz, _tile_beh(ntx, ntz), (int)nblock,
                        (int)npc_at(ntx, ntz));
                // rebord : behavior JUMP_<dir> sur la tuile cible -> saut 2 tuiles
                // DS player_move.c WillJump : E=56 O=57 N=58 S=59 (dir monde 0=S)
                static const int JMP[4] = {59, 58, 57, 56};
                int bh = _tile_beh(ntx, ntz);
                if (_surfable(bh)) nblock = !_psurf;   // eau : surf only
                if (bh == JMP[_pdir] && !_blocked({(ntx + ddx) * TILE + TILE / 2,
                        _ppos.y + 4, (ntz + ddz) * TILE + TILE / 2})) {
                    _ptx = (ntx + ddx) * TILE + TILE / 2;
                    _ptz = (ntz + ddz) * TILE + TILE / 2;
                    _pstep_spd = 120.f;              // 2 tuiles en 16 frames
                    _pstep = 2; _pjump = 1;          // saut par-dessus le rebord
                    if (verbose) printf("[jump] (%d,%d)->(%d,%d) beh=%d\n",
                        ptx, ptz, ntx + ddx, ntz + ddz, bh);
                } else if (nblock || (!noclip && npc_at(ntx, ntz))) { // mur/PNJ : bump + face
                    // Field_CheckMapTransition : pousser contre du terrain
                    // bloqué peut déclencher une transition (un PNJ ne suffit
                    // pas — DS CheckCollision ne teste que le terrain).
                    if (nblock) {
                        // a) la tuile visée : warp + comportement DOOR
                        //    -> transition porte, pas d'anim de bump
                        int wi = _warp_at(ntx, ntz);
                        if (wi >= 0 && _tile_beh(ntx, ntz) == TB_DOOR &&
                            _warp_depart(wi, WT_DOOR, _pdir)) return;
                        // b) la tuile courante : les comportements E/O/S et
                        //    escaliers exigent de pousser dans leur direction
                        int cb = _tile_beh(ptx, ptz);
                        bool dirok = true;
                        if (cb == TB_ENTRANCE_E || cb == TB_WARP_E ||
                            cb == TB_STAIRS_E) dirok = _pdir == 3;
                        else if (cb == TB_ENTRANCE_W || cb == TB_WARP_W ||
                            cb == TB_STAIRS_W) dirok = _pdir == 2;
                        else if (cb == TB_ENTRANCE_S || cb == TB_WARP_S)
                            dirok = _pdir == 0;
                        if (dirok && (wi = _warp_at(ptx, ptz)) >= 0) {
                            if (cb == TB_DOOR &&
                                _warp_depart(wi, WT_DOOR, _pdir)) return;
                            if ((cb == TB_STAIRS_E || cb == TB_STAIRS_W) &&
                                _warp_depart(wi, WT_STAIRS, _pdir)) return;
                            if ((cb == TB_ENTRANCE_E || cb == TB_WARP_E ||
                                 cb == TB_ENTRANCE_W || cb == TB_WARP_W ||
                                 cb == TB_ENTRANCE_S || cb == TB_WARP_S) &&
                                _warp_depart(wi, WT_PLAIN, _pdir)) return;
                        }
                    }
                    // DS Field_CheckSign : pousser contre un panneau mural
                    // (type 1) en face nord le lit automatiquement.
                    if (_pdir == 1) {
                        for (auto& b : _bgs)
                            if (b.type == 1 && (int)b.x == ntx && (int)b.z == ntz) {
                                _start_script(b.script); _vm.step();
                                if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
                                goto bump_done;
                            }
                    }
                    if (verbose) printf("[bump] t=(%d,%d) block=%d npc=%d\n",
                        ntx, ntz, (int)nblock, (int)npc_at(ntx, ntz));
                    bump_done:
                    _pslide = 0;                            // mur : fin de glissière
                    _pstep = 3; _pstep_t = 16.f / 60.f;  // WALK_ON_SPOT_SLOW : 16 fr DS
                } else {
                    _ptx = ntx * TILE + TILE / 2;
                    _ptz = ntz * TILE + TILE / 2;
                    _pstep_spd = (_can_run && run_held) ? 240.f : 120.f;  // DS : 2px/f marche, 4px/f course @60Hz
                    _pstep = 2;
                    if (_follower >= 0) {          // suiveur : mémorise la tuile
                        _ftrail.push_back({ptx, ptz});
                        if (_ftrail.size() > 16) _ftrail.pop_front();
                    }
                }
            }
        }
    }

    if (getenv("MOVDBG")) {                       // dérive : jobs/pas joueur
        _movdbg_t += dt;
        if (_movdbg_t > 0.5f) {
            _movdbg_t = 0;
            int nj = 0; for (auto& m : _moves) if (!m.done && m.obj == 0xFF) nj++;
            if (nj || _pstep) printf("[movdbg] pstep=%d pt=(%.0f,%.0f) jobs=%d\n",
                _pstep, _ptx, _ptz, nj);
            for (auto& m : _moves)
                if (!m.done && m.obj != 0xFF)
                    for (size_t i = 0; i < _objects.size(); i++)
                        if (_objects[i].id == m.obj)
                            printf("[mvpos] obj=%d cmd=%02X a=%d pos=(%.1f,%.1f) "
                                "tile=(%d,%d) dist=%.1f\n",
                                m.obj, m.i < m.st.size() ? m.st[m.i].first : -1,
                                m.i < m.st.size() ? m.st[m.i].second : 0,
                                _objpos[i].x, _objpos[i].z,
                                (int)floorf(_objpos[i].x / TILE),
                                (int)floorf(_objpos[i].z / TILE), m.dist);
        }
    }
    // gravité + sol (rayon depuis bien au-dessus : le spawn peut être bas)
    if (!_pjump) {                                 // saut rebord : Y piloté par l'arc
        float gy = _ground_y(_ppos, _ppos.y + 60.f);
        if (gy > -1e8f && _ppos.y <= gy + 2.f && _vel.y <= 0.f) {
            _ppos.y = gy; _vel.y = -1.f; _ground = true;
        } else {
            _vel.y -= GRAV * dt;
            _ground = false;
        }
        _ppos += _vel * dt;
        if (_ppos.y < -200.f) { _ppos.y = 60.f; _vel.y = 0; }   // secours
    }

    // suiveur : marche sur la file des tuiles du joueur (snake), téléport si loin
    if (_follower >= 0 && _follower != 0xFF) {
        int fi = _follower;                          // déjà un index objet
        if (_objgone.count(fi) || _objhide.count(fi)) fi = -1;
        if (fi >= 0) {
            Vec3& fp = _objpos[fi];
            if (_fstep) {
                float ddx = _ftx - fp.x, ddz = _ftz - fp.z;
                float dl = sqrtf(ddx * ddx + ddz * ddz), st = 96.f * dt;
                if (dl <= st) { fp.x = _ftx; fp.z = _ftz; _fstep = 0; }
                else { fp.x += ddx / dl * st; fp.z += ddz / dl * st; }
            } else if (!_ftrail.empty()) {
                auto t = _ftrail.front(); _ftrail.pop_front();
                int ftx = (int)floorf(fp.x / TILE), ftz = (int)floorf(fp.z / TILE);
                int ddx = t.first - ftx, ddz = t.second - ftz;
                if (abs(ddx) + abs(ddz) > 3)        // trop loin : téléport (warp)
                    fp = {t.first * TILE + TILE / 2, fp.y,
                          t.second * TILE + TILE / 2};
                else if (ddx || ddz) {              // pas de 1 tuile vers la file
                    if (abs(ddz) >= abs(ddx)) {
                        _objd[fi] = ddz > 0 ? 0 : 1;
                        _ftz = (ftz + (ddz > 0 ? 1 : -1)) * TILE + TILE / 2;
                        _ftx = fp.x;
                    } else {
                        _objd[fi] = ddx > 0 ? 3 : 2;
                        _ftx = (ftx + (ddx > 0 ? 1 : -1)) * TILE + TILE / 2;
                        _ftz = fp.z;
                    }
                    _fstep = 1;
                }
            }
        }
    }

    // streaming cellules + bascule de zone (overworld)
    _stream();

    // warps + déclencheurs : seulement quand le pas est terminé (posée sur la tuile,
    // comme sur DS) — évite les tirs en pleine transition et les re-déclenchements
    // warps + déclencheurs : à l'ENTRÉE sur une nouvelle tuile, pas terminé
    // (comportement DS : le warp/porte tire quand on pose le pas dessus —
    //  la tuile d'arrivée du spawn n'est pas un "déclenchement")
    int tx = (int)floorf(_ppos.x / TILE), tz = (int)floorf(_ppos.z / TILE);
    // DS : poser le pas sur la terre ferme en Surf = débarquement automatique
    if (_psurf && _pstep == 0 && !_surfable(_tile_beh(tx, tz))) {
        _psurf = false;
        if (verbose) printf("[surf] débarqué (%d,%d)\n", tx, tz);
    }
    if (getenv("WARPDBG"))
        for (auto& w : _warps)
            if (w.x == tx && w.z == tz)
                printf("[warpdbg] sur warp (%d,%d)->%d pstep=%d busy=%d last=(%d,%d)\n",
                    tx, tz, w.dest, _pstep, (int)_vm.busy(), _warp_cd_x, _warp_cd_z);
    if (_pstep == 0 && !_vm.busy() &&
        (tx != _warp_cd_x || tz != _warp_cd_z)) {
        _warp_cd_x = tx; _warp_cd_z = tz;      // nouvelle tuile -> événement
        // déclencheurs au sol (Coord) AVANT les warps : sur DS un trigger posé
        // sur la tuile de sortie (ex. maman à la porte) intercepte le pas
        for (size_t i = 0; i < _coords.size(); i++) {
            auto& c = _coords[i];
            bool in = tx >= c.x && tx < c.x + c.w && tz >= c.z && tz < c.z + c.l;
            if (in && getenv("COORDDBG"))
                printf("[coorddbg] tuile(%d,%d) in coord#%d scr=%d var=%x(%d)==%d\n",
                    tx, tz, (int)i, c.script, c.var,
                    c.var ? _vm.var(c.var) : -1, c.val);
            if (in && _coord_in != (int)i && c.script != 0xFFFF &&
                (!c.var || _vm.var(c.var) == c.val)) {
                _coord_in = (int)i;
                _start_script(c.script); _vm.step();
                return;
            }
            if (!in && _coord_in == (int)i) _coord_in = -1;
        }
        // Field_CheckTransition : le comportement de la tuile courante décide
        // si le warp s'active en marchant dessus (escalators, entrée/warp
        // nord, panneau warp) — les autres ne font rien au pas.
        int wi = _warp_at(tx, tz);
        if (wi >= 0) {
            int cb = _tile_beh(tx, tz), adir = -1, wtype = WT_PLAIN;
            if (cb == TB_ESCALATOR_FLIP) {          // O<->E imposés par l'escalator
                wtype = WT_ESCAL;
                if (_pdir == 2) adir = 3;
                else if (_pdir == 3) adir = 2;
            } else if (cb == TB_ESCALATOR) {        // conserve la direction
                wtype = WT_ESCAL;
                if (_pdir == 2 || _pdir == 3) adir = _pdir;
            } else if (cb == TB_ENTRANCE_N || cb == TB_WARP_N) {
                adir = 1;                           // arrivée face nord (DS)
            } else if (cb == TB_WARP_PANEL) {
                wtype = WT_PANEL;
                adir = _pdir;
            }
            if (adir >= 0 && _warp_depart(wi, wtype, adir)) return;
        }
        // rencontre sauvage (field_control.c : après les transitions de map)
        _try_wild(_tile_beh(tx, tz));
        if (_vm.busy()) return;
    }
}

void World::_try_wild(int tb) {
    // en surf : le taux eau est indépendant du taux herbe (maps eau pure : 0)
    if (_vm.busy() || _vm.party.empty() || !_enc ||
        (!_psurf && _enc_rate <= 0)) return;
    static const uint8_t ENC_BEH[] = {          // sTileBehaviorFlags & ENCOUNTER (DS)
        0x02,0x03,0x05,0x06,0x08,0x0B,0x10,0x11,0x12,0x15,0x22,
        0x24,0x25,0x2A,0x72,0x77,0x7B,0xA6,0xA7 };
    bool enc = false;                            // surf = 0x10..(sections eau)
    for (uint8_t b : ENC_BEH) if ((uint8_t)tb == b) { enc = true; break; }
    if (!enc) return;
    bool force = getenv("WILD1") != nullptr;     // debug : force la rencontre
    int grace = 8 - std::min(8, ((_enc_rate << 8) / 10) >> 8);   // rate 30 -> 5 pas
    if (_enc_steps++ < grace && !force) {        // pas post-combat : 5% seulement
        if (rand() % 100 >= 5) return;
    }
    int flat = 40 + (tb == 0x03 ? 30 : 0);       // très hautes herbes / vélo +30
    if (flat > 100) flat = 100;
    // rencontre eau (surf) : section WildEncounters.surfEncounters du fichier
    // = taux @204 puis 5 slots de 8o (nvMax, nvMin, pad, espèce) — poids 60/30/5/4/1
    if (_psurf) {
        if (_enc->size() < 248) return;
        int wrate = (int)u32(*_enc, 204);
        if (wrate <= 0 || (!force && (rand() % 100 >= flat ||
                rand() % 100 >= wrate))) return;
        static const int WCUM[5] = {60, 90, 95, 99, 100};
        int roll = rand() % 100, slot = 4;
        for (int s = 0; s < 5; s++) if (roll < WCUM[s]) { slot = s; break; }
        size_t so = 208 + slot * 8;
        int lmax = (int)(int8_t)(*_enc)[so], lmin = (int)(int8_t)(*_enc)[so + 1];
        if (lmax < lmin) std::swap(lmax, lmin);
        int lvl = lmin + (lmax > lmin ? rand() % (lmax - lmin + 1) : 0);
        int species = (int)u32(*_enc, so + 4);
        if (species <= 0) return;
        _enc_steps = 0;
        _vm.set_var(0x8000, species); _vm.set_var(0x8001, lvl);
        if (verbose) printf("[wild-surf] slot=%d esp=%d nv=%d\n", slot, species, lvl);
        _pstep = 0;
        _wild_sp = species; _wild_lv = lvl;
        _btl_start(false, 0, species, lvl);
        return;
    }
    if (!force && (rand() % 100 >= flat || rand() % 100 >= _enc_rate)) return;
    static const int CUM[12] = {20,40,50,60,70,80,85,90,94,98,99,100};
    int roll = rand() % 100, slot = 11;
    for (int s = 0; s < 12; s++) if (roll < CUM[s]) { slot = s; break; }
    size_t so = 4 + slot * 8;
    if (so + 8 > _enc->size()) return;
    int lvl = (int)(int8_t)(*_enc)[so + 1];         // DS : slots terrestres = nv min
    int species = (int)u32(*_enc, so + 4);
    if (species <= 0) return;
    _enc_steps = 0;
    _vm.set_var(0x8000, species); _vm.set_var(0x8001, lvl);
    if (verbose) printf("[wild] slot=%d esp=%d nv=%d\n", slot, species, lvl);
    _pstep = 0;                                   // pas figé : combat démarre
    _wild_sp = species; _wild_lv = lvl;
    _btl_start(false, 0, species, lvl);
}

// ── combat : décor/terrain selon la map (field_battle_data_transfer.c) ────────
// CalcTerrain : le comportement de la tuile sous le joueur prime, sinon la
// table par défaut du décor (sTerrainForBackground).
static int btl_terrain(int beh, int bg) {
    switch (beh) {
    case 0x20: return 8;                                  // glace → ICE
    case 0x02: case 0x03: return 2;                       // herbes → GRASS
    case 0x21: return 1;                                  // sable → SAND
    case 0xA1: case 0xA2: case 0xA3:
    case 0xA8: return 6;                                  // neige → SNOW
    case 0xA4: case 0xA5: case 0xA6:
    case 0xA7: return 10;                                 // boue → GREAT_MARSH
    case 0x08: return 5;                                  // sol grotte → CAVE
    }
    if ((beh >= 0x10 && beh <= 0x15) || beh == 0x19)      // surfable → WATER
        return 7;
    static const int T[] = {0,7,9,2,4,6,9,9,9,5,5,5,
                            12,13,14,15,16,17,18,19,20,21,22};
    return bg >= 0 && bg < 23 ? T[bg] : 0;
}
// GetBackgroundTimeOffset : palettes jour/soir/nuit pour les décors extérieurs.
static int btl_tod(int bg) {
    if (bg > 5) return 0;
    int h = [] { time_t t = time(nullptr); return localtime(&t)->tm_hour; }();
    if (h >= 17 && h < 20) return 1;                      // crépuscule
    if (h >= 20 || h < 4)  return 2;                      // nuit
    return 0;                                             // matin/jour
}

// ── combat : construction de l'équipe joueur depuis _vm.party ─────────────────
void World::_btl_start(bool trainer, int tid, int sp, int lv) {
    _btl = Battle();
    {   // SetBackgroundAndTerrain : décor du header (+eau si tuile surfable)
        const MapHeader* h = _db->header(_header);
        int bg = h ? h->bbg : 0;
        int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
        int beh = _tile_beh(ptx, ptz);
        if ((beh >= 0x10 && beh <= 0x15) || beh == 0x19) bg = 1;   // WATER
        _btl.bg = bg;
        _btl.terrain = btl_terrain(beh, bg);
        _btl.tod = btl_tod(bg);
    }
    _btl.trainer = trainer; _btl.tid = tid;
    _btl.mi = -1; _btl.me_part = 0;
    for (int i = 0; i < (int)_vm.party.size(); i++) {
        auto& pm = _vm.party[i];
        BatMon m = battle_make(_db, pm.species, pm.level, nullptr, pm.iv);
        pm.iv = m.iv;                                    // IV figé à la 1re utilisation
        // données persistées : PID/nature/XP/EVs/amitié/moves
        if (pm.pid) m.pid = pm.pid; else pm.pid = m.pid;
        if (!pm.exp) pm.exp = mon_exp_for(_db, pm.species, pm.level);  // 1re util.
        m.exp = pm.exp; m.friendship = pm.friendship;
        for (int k = 0; k < 6; k++) m.evs[k] = pm.evs[k];
        if (pm.nmoves) {                                 // moves appris conservés
            m.nmoves = pm.nmoves;
            for (int k = 0; k < 4; k++) {
                m.moves[k] = pm.moves[k];
                m.pp[k] = pm.pp[k] > 0 ? pm.pp[k]
                          : (pm.moves[k] ? battle_move(_db, pm.moves[k]).pp : 0);
            }
        }
        mon_recalc(_db, m);                              // stats niveau+EV+nature
        m.hp = pm.hp < 0 ? m.maxhp : std::min(pm.hp, m.maxhp);
        _btl.me.push_back(m);
        if (_btl.mi < 0 && m.hp > 0) { _btl.mi = i; _btl.me_part |= 1 << i; }
    }
    if (trainer) {
        _btl.foes = battle_trainer_party(_db, tid);
        auto cs = battle_trainer_class_sprite(_db, tid);
        _btl.foe_class = cs.first; _btl.foe_sprite = cs.second;
    } else _btl.foes.push_back(battle_make(_db, sp, lv));
    if (_btl.mi < 0 || _btl.foes.empty()) {       // pas d'équipe : victoire auto
        if (_vm.state() == ScriptVM::BATTLE) {
            _vm.battle_lost = 0;
            _vm.continue_after(ScriptVM::BATTLE); _vm.step();
        }
        return;
    }
    _btl.active = true;
    _btl.st = trainer ? Battle::INTRO : Battle::TRANS;
    _btl.anim = 1.2f;
    // musique de combat : sauvage BA_POKE, dresseur BA_TRAIN (SEQ SDAT)
    if (_snd) {
        int bseq = _snd->seq_named(trainer ? "SEQ_BA_TRAIN" : "SEQ_BA_POKE");
        if (bseq >= 0) { _snd->music(bseq); _bgm_cur = bseq; }
    }
    if (trainer)
        _btl.msg = tr("Le Dresseur veut se battre !\nIl envoie ",
                      "The Trainer wants to battle!\nThey send out ") +
            battle_species_name(_db, _btl.foes[0].species) + "!";
    else
        _btl.msg = tr("Un ", "A wild ") +
            battle_species_name(_db, _btl.foes[0].species) +
            tr(" sauvage apparaît !", " appeared!");
    _pstep = 0;
    if (verbose) printf("[battle] %s tid=%d foe=%s niv.%d vs %s niv.%d\n",
        trainer ? "trainer" : "wild", tid,
        battle_species_name(_db, _btl.foes[0].species).c_str(), _btl.foes[0].level,
        battle_species_name(_db, _btl.you().species).c_str(), _btl.you().level);
}

// lancer de Ball depuis le menu SAC du combat : consomme l'objet, résout la
// capture, et passe en phase THROW (anim secousses). Le Pokémon capturé rejoint
// l'équipe à la fin du combat (_btl_end, via _btl.caught).
void World::battle_throw(int item) {
    if (!_btl.active || _btl.st != Battle::BAG || _vm.bag_count(item) <= 0) return;
    _vm.bag_add(item, -1);
    _btl.ball = item;
    _btl.st = Battle::THROW; _btl.tphase = 0; _btl.anim = 1.0f;
    _btl.msg = tr("Vous lancez une ", "You threw a ") + item_name(item) + " !";
    if (_btl.trainer) {                         // le dresseur bloque la Ball
        _btl.shakes = _btl.shake_max = 0; _btl.caught = false;
        _btl.msg = tr("Le Dresseur a dévié la Ball !",
                      "The Trainer blocked the Ball!");
        _btl.tphase = 2; _btl.anim = 0.4f;
        return;
    }
    int s = battle_catch(_db, _btl, item);
    _btl.shake_max = s;
    _btl.shakes = s < 4 ? s : 3;                // capture : 3 secousses + clic
    _btl.caught = s == 4;
    _btl.caught_box = -1;
    if (_btl.caught && _vm.party.size() >= 6)   // équipe pleine -> boîte PC
        for (int s = 0; s < 540; s++)
            if (!_vm.pcbox[s].species) { _btl.caught_box = s / 30; break; }
}

void World::_btl_end() {
    bool lost = _btl.st == Battle::LOSE;
    // musique de victoire DS : WINPOKE/WINTRAIN, puis le thème de zone reprend
    if (_snd && !lost) {
        int w = _snd->seq_named(_btl.trainer ? "SEQ_WINTRAIN" : "SEQ_WINPOKE");
        if (w >= 0) { _snd->music(w); _bgm_victory = true; }
    }
    if (_btl.caught) {                            // capturé -> équipe ou PC
        ScriptVM::PartyMon pm;
        pm.species = _btl.foe().species; pm.level = _btl.foe().level;
        pm.hp = _btl.foe().hp; pm.iv = _btl.foe().iv;
        pm.pid = _btl.foe().pid; pm.exp = _btl.foe().exp;
        pm.friendship = _btl.foe().friendship;
        for (int k = 0; k < 6; k++) pm.evs[k] = _btl.foe().evs[k];
        pm.nmoves = _btl.foe().nmoves;
        for (int k = 0; k < 4; k++) { pm.moves[k] = _btl.foe().moves[k]; pm.pp[k] = _btl.foe().pp[k]; }
        if (_vm.party.size() < 6) _vm.party.push_back(pm);
        else for (int s = 0; s < 540; s++)
            if (!_vm.pcbox[s].species) { _vm.pcbox[s] = pm; break; }
    }
    // retour combat -> équipe : PV + progression (XP, niveau, moves, EVs)
    bool night = [] { time_t t = time(nullptr); int h = localtime(&t)->tm_hour;
                      return h >= 20 || h < 4; }();
    std::string evo_msg;
    for (int i = 0; i < (int)_vm.party.size() && i < (int)_btl.me.size(); i++) {
        auto& pm = _vm.party[i]; auto& bm = _btl.me[i];
        bool lvlup = !lost && bm.level > pm.level;
        pm.hp = lost ? bm.maxhp : bm.hp;
        pm.level = bm.level; pm.exp = bm.exp; pm.pid = bm.pid;
        pm.friendship = bm.friendship;
        for (int k = 0; k < 6; k++) pm.evs[k] = bm.evs[k];
        pm.nmoves = bm.nmoves;
        for (int k = 0; k < 4; k++) { pm.moves[k] = bm.moves[k]; pm.pp[k] = bm.pp[k]; }
        // évolution au level-up (premier match evo.narc, ordre ROM)
        if (lvlup) {
            BatMon tmp = bm;
            int tgt = mon_evolution_target(_db, tmp, night);
            if (tgt) {
                std::string a = battle_species_name(_db, pm.species),
                            b = battle_species_name(_db, tgt);
                pm.species = tgt;                        // stats recalculées
                BatMon nm = battle_make(_db, tgt, pm.level, nullptr, pm.iv);
                // conserve les données persistées
                for (int k = 0; k < 4; k++) { nm.moves[k] = pm.moves[k]; nm.pp[k] = pm.pp[k]; }
                nm.nmoves = pm.nmoves;
                mon_recalc(_db, nm);
                pm.hp = std::max(1, pm.hp + nm.maxhp - bm.maxhp);  // gain de max
                evo_msg += "Hein ? " + a + tr(" évolue !", " is evolving!") +
                           "\n\n" + a + tr(" a évolué en ", " evolved into ") +
                           b + " !\n\n";
                if (verbose) printf("[evo] %s -> %s\n", a.c_str(), b.c_str());
            }
        }
    }
    if (!evo_msg.empty()) {
        _dialog = evo_msg; _dlg_page = 0; _dlg_chars = 0; _mbox = true;
    }
    if (lost)                                     // DS : soigné au Centre
        for (auto& p : _vm.party) p.hp = -1;      // -1 -> _btl_start soigne
    _vm.battle_lost = lost ? 1 : 0;
    if (_vm.state() == ScriptVM::BATTLE) {        // reprendre le script dresseur
        _vm.continue_after(ScriptVM::BATTLE); _vm.step();
        // le script dresseur déclenche lui-même BlackOutFromBattle si perdu
    } else if (lost)
        _lost_pc = true;                          // sauvage : blackout direct DS
    if (verbose) printf("[battle] fin won=%d\n", _btl.won);
}

// (pokegra_decrypt a déménagé dans world_render.cpp, utilisé par _poke_tex)