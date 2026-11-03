// world_tick.cpp — update joueur, déclencheurs, rencontres, combat.
#include "world.h"
#include "world_internal.h"
#include "spr.h"
#include "sdat.h"
#include "battle.h"
#include <cstdio>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <algorithm>

// ── pokémon suiveur : marche sur la file des tuiles du joueur ─────────────────
// identique au suiveur-PNJ (serpent 1 tuile/pas à 96 px/s, téléport si >3 tuiles
// de retard), mais sur une entité dédiée — pas de collision ni de script.
void World::_pokefol_tick(float dt) {
    static float dbg = 0.f;
    if (getenv("PFDBG") && (dbg -= dt) <= 0.f) {
        dbg = 0.5f;
        printf("[pf] fol=%d sp=%d shown=%d pf=%.1f,%.1f,%.1f pp=%.1f,%.1f,%.1f dir=%d step=%d anim=%.2f trail=%zu\n",
            _pokefol, _pokefol_sp, (int)pokefol_shown(),
            _pfpos.x, _pfpos.y, _pfpos.z, _ppos.x, _ppos.y, _ppos.z,
            _pfdir, _pfstep, _pfanim, _pftrail.size());
    }
    if (_pokefol < 0 || !pokefol_shown()) { _pftrail.clear(); return; }
    Vec3& fp = _pfpos;
    if (_pfstep) {
        float ddx = _pftx - fp.x, ddz = _pftz - fp.z;
        float dl = sqrtf(ddx * ddx + ddz * ddz);
        // DS : même durée que le pas du joueur ; file non vide -> rattrapage
        float spd = _pftrail.empty() ? 60.f : 120.f, st = spd * dt;
        _pfanim += dt * (spd / 8.f);             // ~2 alternances par tuile
        if (dl <= st) { fp.x = _pftx; fp.z = _pftz; _pfstep = 0; }
        else { fp.x += ddx / dl * st; fp.z += ddz / dl * st; }
    } else if (!_pftrail.empty()) {
        auto t = _pftrail.front(); _pftrail.pop_front();
        int ftx = (int)floorf(fp.x / TILE), ftz = (int)floorf(fp.z / TILE);
        int ddx = t.first - ftx, ddz = t.second - ftz;
        if (abs(ddx) + abs(ddz) > 3) {           // trop loin : rappel sous le joueur
            fp = {_ppos.x, fp.y, _ppos.z};       // (DS : caché, réapparaît)
            _pftrail.clear();
        } else if (ddx || ddz) {                 // pas de 1 tuile vers la file
            if (abs(ddz) >= abs(ddx)) {
                _pfdir = ddz > 0 ? 0 : 1;
                _pftz = (ftz + (ddz > 0 ? 1 : -1)) * TILE + TILE / 2;
                _pftx = fp.x;
            } else {
                _pfdir = ddx > 0 ? 3 : 2;
                _pftx = (ftx + (ddx > 0 ? 1 : -1)) * TILE + TILE / 2;
                _pftz = fp.z;
            }
            _pfstep = 1; _pfanim = 0.f;
        }
    }
    // ancre au sol (dénivelé, escaliers)
    float g = _ground_y({fp.x, fp.y, fp.z}, fp.y);
    if (g > -1e8f) fp.y = g;
}

// ── météo variable : état visé -> intensité continue ───────────────────────
// La tendance vient du header (byte wthr, résolu par date) ; --weather dyn
// ajoute un système complet : un champ de bruit 1D sur les coordonnées de
// matrice dérive dans le temps -> les fronts traversent les zones voisines,
// et la saison (date réelle) convertit pluie <-> neige. Plus de constante
// par map : la météo est un événement qui arrive, passe et repart.
void World::_weather_tick(float dt) {
    Weather& w = _wthr;
    w.t += dt;
    if (w.front_t <= 0.f) {                        // dérive calée sur l'horloge
        const char* f = getenv("WTHFRONT");        // test : position forcée
        w.front_t = f ? (float)atof(f) : (float)time(nullptr) / 600.f;
    }
    w.front_t += dt;
    int base = dbg_weather >= 0 ? dbg_weather : _wthr_base;
    if (dbg_weather_dyn) {
        w.phase_t -= dt;
        if (w.phase_t <= 0.f) {
            w.phase_t = 12.f;                      // le champ bouge lentement
            w.target = _weather_field();
        }
    } else w.target = base;
    static bool wdbg = getenv("WTHDBG") != nullptr;
    if (wdbg && w.target != _wthr_dbg_last) {
        printf("[wthr] cible %d -> %d (base %d, dyn %d, saison %d)\n",
            _wthr_dbg_last, w.target, base, (int)dbg_weather_dyn, _season());
        _wthr_dbg_last = w.target;
    }

    if (w.target == w.type) {
        if (w.intensity < 1.f)
            w.intensity = fminf(1.f, w.intensity + dt * 0.10f);   // ~10 s de montée
    } else {
        w.intensity -= dt * 0.22f;                               // sortie rapide
        if (w.intensity <= 0.f) { w.intensity = 0.f; w.type = w.target; }
    }
    if (w.type == 4 && w.intensity > 0.4f) {    // orage : éclairs aléatoires
        w.flash_t -= dt;
        if (w.flash_t <= 0.f) {
            w.flash = 0.85f;
            w.flash_t = 5.f + (rand() % 90) / 10.f;
        }
    }
    if (w.flash > 0.f) w.flash -= dt * 3.5f;
    // sol mouillé : monte sous la pluie, sèche en ~2 min après
    if (w.type >= 2 && w.type <= 4)
        w.wet = fminf(1.f, w.wet + dt * 0.12f);
    else
        w.wet = fmaxf(0.f, w.wet - dt * 0.008f);
    // neige accumulée : monte sous la neige (~40 s), fond en ~4 min
    if (w.type >= 5 && w.type <= 7)
        w.snow_depth = fminf(1.f, w.snow_depth + dt * 0.025f);
    else
        w.snow_depth = fmaxf(0.f, w.snow_depth - dt * 0.004f);
    // eau à portée (scan paresseux ~2 s) : pilote la boucle de vagues NAMI
    static float nami_chk = 0.f;
    nami_chk -= dt;
    if (nami_chk <= 0.f) {
        nami_chk = 2.f;
        int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
        w.nami = false;
        for (int dz = -8; dz <= 8 && !w.nami; dz++)
            for (int dx = -8; dx <= 8 && !w.nami; dx++)
                if (_surfable(_tile_beh(ptx + dx, ptz + dz))) w.nami = true;
    }

    // ambient : les boucles météo de la ROM (SEQ_SE_DP_T_*, KAZE, SUNA,
    // YUKI) — on/off piloté par le type comme sur DS (pas de fondu). La
    // brume reste silencieuse : l'absence de son EST l'ambiance.
    static const struct { int t; const char* n; } WSE[] = {
        { 2, "SEQ_SE_DP_T_AME" },   { 3, "SEQ_SE_DP_T_OOAME" },
        { 4, "SEQ_SE_DP_T_OOAME" }, { 5, "SEQ_SE_PL_YUKI" },
        { 6, "SEQ_SE_PL_YUKI" },    { 7, "SEQ_SE_DP_KAZE" },
        { 9, "SEQ_SE_DP_KAZE2" },   {10, "SEQ_SE_DP_SUNA" },
        {11, "SEQ_SE_DP_T_OOAME" },
    };
    if (_snd) {
        const char* nm = nullptr;
        for (auto& e : WSE) if (e.t == w.type) { nm = e.n; break; }
        int want = nm ? _snd->seq_named(nm) : -1;
        if (nm && want < 0 && getenv("WTHDBG"))
            printf("[wthr] seq absente : %s\n", nm);
        if (want != w.amb) {
            if (w.amb >= 0) _snd->se_stop(w.amb);
            w.amb = -1; w.amb_cut = -1;           // le type change : oubli
            if (want >= 0 && w.intensity > 0.3f) { _snd->se(want); w.amb = want; }
        }
        if (w.amb >= 0 && w.intensity <= 0.05f) {
            _snd->se_stop(w.amb); w.amb = -1;
        }
        // orage : coup de tonnerre retardé après l'éclair (délai = distance)
        if (w.flash >= 0.8f && w.kami_t < 0.f)
            w.kami_t = 0.7f + (rand() % 14) / 10.f;
        if (w.kami_t >= 0.f) {
            w.kami_t -= dt;
            if (w.kami_t < 0.f) {
                int k = _snd->seq_named(rand() & 1 ? "SEQ_SE_DP_T_KAMI"
                                                   : "SEQ_SE_DP_T_KAMI2");
                if (k >= 0) _snd->se(k);
            }
        }
        // vagues : boucle SEQ_SE_DP_NAMI près de l'eau, coupée quand un
        // ambient météo joue (même player possible -> pas de conflit)
        int ns = _snd->seq_named("SEQ_SE_DP_NAMI");
        if (w.nami && ns >= 0 && w.nami_seq < 0 && w.amb < 0) {
            _snd->seq_vol(ns, 60); _snd->se(ns); w.nami_seq = ns;
        }
        if (w.nami_seq >= 0 && (!w.nami || w.amb >= 0)) {
            _snd->se_stop(w.nami_seq); w.nami_seq = -1;
        }
        static int nami_last = -2;
        if (wdbg && w.nami_seq != nami_last) {
            printf("[wthr] nami %s\n", w.nami_seq >= 0 ? "on" : "off");
            nami_last = w.nami_seq;
        }
        // boucle coupée par un one-shot (pas de neige) sur le même player :
        // réédition quand le player se libère
        if (w.amb_cut >= 0) {
            int pl = _snd->seq_player(w.amb_cut);
            if (pl < 0 || !_snd->player_busy(pl)) {
                _snd->se(w.amb_cut); w.amb_cut = -1;
            }
        }
    }
}

// bulletin météo (Pokétch) : type du champ à une position monde arbitraire.
// Permet de lire la météo des zones voisines avant d'y arriver.
int World::weather_at(float wx, float wz, int base) const {
    if (base < 0) base = dbg_weather >= 0 ? dbg_weather : _wthr_base;
    if (!dbg_weather_dyn || !_weather_variable(base) ||
        _mtx.headers.empty()) return base;
    float v = _wnoise(wx / CELL * 0.31f + wz / CELL * 0.27f +
                      _wthr.front_t * 0.02f);
    return _weather_season(_weather_band(v, base));
}

// champ météo : bruit de valeur 1D sur les coords de matrice, dérivant avec
// front_t — le même front sert les cellules voisines (la pluie vue au nord
// arrive réellement vers toi). Déterministe : aucune donnée par particule.
float World::_wfh(int i) {                       // hachage entier -> 0..1
    unsigned x = (unsigned)(i * 2654435761u + 974711u);
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return (x & 0xFFFFFF) / 16777216.f;
}
float World::_wnoise(float x) {                  // value noise lissé
    int i = (int)floorf(x);
    float f = x - i, u = f * f * (3.f - 2.f * f);
    return _wfh(i) + (_wfh(i + 1) - _wfh(i)) * u;
}

// climat soumis aux fronts : tempéré + pluie + neige varient ; sable, cendres,
// brouillards et intérieurs gardent leur type (le header gagne).
bool World::_weather_variable(int t) {
    return t >= 0 && t <= 7;
}

int World::_weather_field() const {
    int base = dbg_weather >= 0 ? dbg_weather : _wthr_base;
    // intérieur = matrice sans headers par cellule (critère DS, cf. _stream)
    if (!_weather_variable(base) || _mtx.headers.empty()) return base;
    float cx = _ppos.x / CELL, cz = _ppos.z / CELL;
    float v = _wnoise(cx * 0.31f + cz * 0.27f + _wthr.front_t * 0.02f);
    return _weather_season(_weather_band(v, base));
}

// champ 0..1 -> type, par famille de climat (base = météo header résolue).
int World::_weather_band(float v, int base) const {
    bool snowfam = base >= 5 && base <= 7;
    bool rainfam = base >= 2 && base <= 4;
    if (snowfam)                                    // nord : neige dominante
        return v < 0.30f ? 0 : v < 0.45f ? 5 : v < 0.75f ? 6 : 7;
    if (rainfam)                                    // climat pluie
        return v < 0.25f ? 0 : v < 0.35f ? 1 : v < 0.80f ? 2 : v < 0.95f ? 3 : 4;
    return v < 0.45f ? 0 : v < 0.65f ? 1 : v < 0.85f ? 2 : v < 0.95f ? 3 : 4;
}

// distance de v aux bornes de sa bande (0..1 dans la bande, 0 au seuil) —
// sert le fondu des particules aux limites de front (rendu spatial).
float World::_band_margin(float v, int base) const {
    float lo = 0.f, hi = 1.f;
    const float* T; int nt;
    static const float TS[] = {0.30f, 0.45f, 0.75f};        // neige
    static const float TR[] = {0.25f, 0.35f, 0.80f, 0.95f}; // pluie
    static const float TC[] = {0.45f, 0.65f, 0.85f, 0.95f}; // tempéré
    if (base >= 5 && base <= 7)      { T = TS; nt = 3; }
    else if (base >= 2 && base <= 4) { T = TR; nt = 4; }
    else                             { T = TC; nt = 4; }
    for (int i = 0; i < nt; i++) {
        if (v < T[i]) { hi = T[i]; break; }
        lo = T[i];
    }
    return fminf(v - lo, hi - v);
}

// conversion saisonnière du type choisi par le champ : l'hiver transforme
// toute précipitation liquide en neige ; l'été assainit les nuages.
int World::_weather_season(int t) const {
    if (_season() == 0) {                          // hiver
        if (t == 2) return 5;
        if (t == 3) return 6;
        if (t == 4) return 7;
        if (t == 1) return 14;                     // nuageux -> brume hivernale
    }
    if (_season() == 2 && t == 1) return 0;        // été : ciel nettoyé
    return t;
}

// saison astronomique (hémisphère nord) : déc-fév hiver, etc.
int World::_season() const {
    if (dbg_season >= 0) return dbg_season;        // --season N (test)
    time_t tt = time(nullptr); struct tm lt{};
    localtime_s(&lt, &tt);
    return ((lt.tm_mon + 1) / 3) % 4;              // 0 hiver 1 print 2 été 3 aut
}

// ── update ────────────────────────────────────────────────────────────────────
void World::update(float dt, float ix, float iz) {
    if (_header < 0) return;
    if (dt > 0) _vm.play_sec += dt;             // temps de jeu (sauvegarde v14)
    static int inpdbg = -1;
    if (inpdbg < 0) inpdbg = getenv("INPDBG") != nullptr;
    if (inpdbg && (ix || iz))
        printf("[inp] ix=%.0f iz=%.0f frozen=%d busy=%d wt=%d wild=%.2f "
            "spot=%d lost=%d yesno=%d pstep=%d pdir=%d beh=%02x\n",
            ix, iz, (int)ui_frozen, (int)_vm.busy(), _wt.st, _wild_t,
            _appr[0].obj, (int)_lost_pc, (int)_yesno, _pstep, _pdir,
            _tile_beh((int)floorf(_ppos.x / TILE), (int)floorf(_ppos.z / TILE)));
    if (ui_frozen) {                            // menu X : monde figé (DS)
        ix = iz = 0;
        dt = 0;
    }
    if (_gym.kind == 3 && _gym.et_phase)        // cinématique pendule DS :
        ix = iz = 0;                            // FieldTask suspend le joueur
    if (getenv("BEHDBG")) {                     // dump behaviors (rayon = val)
        static bool done = false;
        if (!done && _dbg_frames > 60) {
            done = true;
            int rad = atoi(getenv("BEHDBG")); if (rad <= 0) rad = 10;
            int ptx = (int)floorf(_ppos.x / TILE),
                ptz = (int)floorf(_ppos.z / TILE);
            printf("[beh] joueur t=(%d,%d)\n", ptx, ptz);
            for (int dz = -rad; dz <= rad; dz++) {
                printf("[beh] z=%4d :", ptz + dz);
                for (int dx = -rad; dx <= rad; dx++)
                    printf(" %02x", _tile_beh(ptx + dx, ptz + dz) & 0xff);
                printf("\n");
            }
        }
    }
    // gravité pour les sauts/chutes (steps y gérés par RecalculateObjectHeight)
    const float GRAV = 400.f;

    // tick des mouvements scriptés + reprise du VM en attente
    _tick_moves(dt);
    _tick_wander(dt);
    // test rendu : DWVORTEX=1 rejoue la cinématique au premier tick
    static bool vortex_dbg = getenv("DWVORTEX") != nullptr;
    if (vortex_dbg) { vortex_dbg = false; _dwarp_t = 2.1f; _dwarp_sfx = false; }
    // vortex DoDWWarp : ~2,1 s avant que le Warp du script ne charge la map
    // (SEQ_SE_PL_SYUWA2 DS joué à la frame 15 ≈ 0,25 s)
    if (_dwarp_t >= 0) {
        _dwarp_t -= dt;
        if (!_dwarp_sfx && _dwarp_t < 1.85f) {
            _dwarp_sfx = true;
            if (_snd) {
                int s = _snd->seq_named("SEQ_SE_PL_SYUWA2");
                if (s >= 0) _snd->se(s);
            }
        }
        if (_dwarp_t < 0) _dwarp_t = -1;
    }
    // Warp scripté (frame script : ex. livraison villa) — consommé ici ; le
    // ctx est sauvegardé dans _set_header puis repris après les init scripts.
    if (_warp_req && !_busy && _dwarp_t < 0) {
        _warp_req = false; load_map(_pw.hdr, -1);
        _ppos = {_pw.x * TILE + TILE / 2.f, 60.f, _pw.z * TILE + TILE / 2.f};
        static const int M[4] = {1, 0, 2, 3};         // dir ROM -> monde
        _pdir = M[_pw.dir & 3];
        _warp_cd_x = _pw.x; _warp_cd_z = _pw.z;
        if (_pokefol >= 0) _pokefol_spawn();
    }
    // ScriptContext DS : un script qui a Warp reprend dès que les scripts
    // d'init de la nouvelle map ont libéré le VM (voir _set_header).
    if (_vm_cont && !_vm.busy() && !_busy) _vm_resume();

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
    if (verbose && _vm.busy() && _dbg_frames % 60 == 0)
        printf("[vm] st=%d pc=%d dlg=%zu box=%d chs=%.0f/%d\n", _vm.state(),
            _vm.pc(), _dialog.size(), _mbox, _dlg_chars, dlg_page_chars());
    _tick_prop_anims(dt);                        // portes : avance les anims
    {   // debug : ETERNADV=N déclenche AdvanceEternaGymClock à la frame N
        static int etadv = -1;
        if (etadv < 0) { const char* e = getenv("ETERNADV");
            etadv = e ? atoi(e) : 0; }
        if (etadv > 0 && _dbg_frames >= etadv) { etadv = 0; _gym_op(8, 0); }
    }
    _gym_tick(dt);                               // nappe d'eau arène (montée)
    _gvl_tick(dt);                               // sacs de frappe gym Veilstone
    _dw_tick(dt);                                // Monde Distorsion (caméra/sauts)
    _boulder_tick(dt);                           // poussée Force + chutes rochers
    _flt_step(dt);                               // textures animées (eau, fleurs)
    _berry_tick();                               // croissance baies (minutes réelles)
    _weather_tick(dt);                           // météo : fronts + particules
    if ((_mbox || _tv_suppress) && _dlg_chars < (float)dlg_page_chars())
        _dlg_chars += dt * (g_opts.text_speed == 0 ? 7.5f      // DS lent
            : g_opts.text_speed == 2 ? 60.f : 15.f);          // normal/rapide
    else if (_vm.state() == ScriptVM::MSG &&
             dlg_page_end() >= _dialog.size() &&
             !(_tv_suppress && tv_ui)) {
        // DS ScrCmd_Message : WaitForFinishedPrinting — le script reprend dès
        // que l'impression de la dernière page est finie (pas d'appui requis).
        // Scène TV + poste ouvert : chaque ligne attend E (tv_advance).
        _vm.continue_after(ScriptVM::MSG); _vm.step();
        if (!_vm.busy()) _script_end_box();
    }
    if (_fade.t >= 0) {                       // FadeScreen en cours
        _fade.t += dt;
        if (_fade.t >= _fade.dur) _fade.t = -1;
    }
    if (_vm.state() == ScriptVM::YESNO && !_yesno) _yesno = true;
    if (_vm.state() == ScriptVM::MOVE) {
        if (_moves_done()) {
            if (verbose) printf("[mv-done] reprise script pc=%d\n", _vm.pc());
            _vm.continue_after(ScriptVM::MOVE); _vm.step();
            if (!_vm.busy()) _script_end_box();
        } else if (verbose) {
            _mvdbg += dt;                       // dump jobs coincés toutes les 2s
            if (_mvdbg > 2.f) {
                _mvdbg = 0;
                for (auto& m : _moves)
                    if (!m.done)
                        printf("[mv-stuck] obj=%d i=%zu/%zu cmd=%02X a=%d "
                            "t=%.2f dist=%.2f dx=%d dz=%d spd=%.2f nt=%d "
                            "dt=%.4f x0=%d z0=%d\n",
                            m.obj, m.i, m.st.size(),
                            m.i < m.st.size() ? m.st[m.i].first : -1,
                            m.i < m.st.size() ? m.st[m.i].second : 0,
                            m.t, m.dist, m.dx, m.dz, m.spd, m.nt, dt,
                            m.x0, m.z0);
            }
        }
    }
    _tick_appr(dt);                              // tâches d'approche (B6/B7)
    if (_vm.honey_min > 0.f) _vm.honey_min -= dt / 60.f;   // miel : 360 min DS
    if (_vm.state() == ScriptVM::MSG && _tv_suppress && !tv_ui) {
        // poste fermé en pleine scène : pages déroulées en silence
        if (_dlg_chars < (float)dlg_page_chars())
            _dlg_chars = (float)dlg_page_chars();
        else {
            size_t pe = dlg_page_end();
            if (pe < _dialog.size()) { _dlg_page = pe; _dlg_chars = 0.f; }
            else { _vm.continue_after(ScriptVM::MSG); _vm.step();
                   if (!_vm.busy()) _script_end_box(); }
        }
    }
    if (_vm.state() == ScriptVM::WAITBTN && _tv_suppress && !tv_ui) {
        // scène TV avalée mais poste fermé : le script s'auto-avance
        _vm.continue_after(ScriptVM::WAITBTN); _vm.step();
        if (!_vm.busy()) _script_end_box();
    }
    {   // auto-avance des dialogues : texte entièrement révélé + temps de
        // lecture -> A simulé (cutscenes, PNJ, feed TV). Exclus : choix
        // oui/non, panneaux-signets (attente d'une vraie entrée).
        auto st9 = _vm.state();
        if ((st9 == ScriptVM::MSG || st9 == ScriptVM::WAITBTN) &&
            !_yesno && !_vm.signpost_dest && !_dialog.empty() &&
            _dlg_chars >= (float)dlg_page_chars()) {
            float need = 1.1f + (float)dlg_page_chars() * 0.045f;
            if (need > 6.f) need = 6.f;
            if (const char* ad = getenv("AUTODLG"))
                need = fmaxf(0.05f, (float)atof(ad));   // test : délai forcé
            _autodlg_t += dt; _autodlg_f++;         // sec + frames (dt~0 headless)
            if (getenv("ADLGDBG"))
                printf("[adlg] t=%.2f/%.2f f=%d st=%d supp=%d tvi=%d ch=%.0f/%d\n",
                    _autodlg_t, need, _autodlg_f, (int)st9,
                    (int)_tv_suppress, (int)tv_ui, _dlg_chars,
                    dlg_page_chars());
            if (_autodlg_t >= need || _autodlg_f >= (int)(need * 60.f)) {
                _autodlg_t = 0.f; _autodlg_f = 0;
                if (_tv_suppress && tv_ui) tv_advance();
                else action();
            }
        } else { _autodlg_t = 0.f; _autodlg_f = 0; }
    }
    if (_vm.state() == ScriptVM::WAITTIME) {
        // ticks DS ~60 Hz : sous-pas pour tenir la durée quel que soit le fps
        _waitt_acc += dt * 60.f;
        while (_waitt_acc >= 1.f && _vm.wait_t > 0) {
            _waitt_acc -= 1.f; _vm.wait_t--;
        }
        if (_vm.wait_t <= 0) {
            _waitt_acc = 0;
            _vm.continue_after(ScriptVM::WAITTIME); _vm.step();
            if (!_vm.busy()) _script_end_box();
        }
    }
    if (_vm.state() == ScriptVM::WAITFADE && _fade.t < 0) {
        _vm.continue_after(ScriptVM::WAITFADE); _vm.step();
        if (!_vm.busy()) _script_end_box();
    }
    if (_vm.state() == ScriptVM::WAITANIM &&
        (!_vm.cb_doorbusy || !_vm.cb_doorbusy(_vm.wait_anim))) {
        _vm.continue_after(ScriptVM::WAITANIM); _vm.step();
        if (!_vm.busy()) _script_end_box();
    }
    if (_tv_suppress && !_vm.busy())             // scène TV avalée terminée :
        _tv_suppress = _tv_scene = false;        //  les boîtes redeviennent actives
    if (_vm.state() == ScriptVM::WAITSND) {
        // garde-fou : un SE qui ne termine pas (canal bloqué, boucle) ne
        // doit pas figer le script — DS relâche quand le son finit
        _waitsnd_t += dt; _waitsnd_f++;
        bool done = !_vm.cb_sndbusy || !_vm.cb_sndbusy(_vm.wait_snd) ||
                    _waitsnd_t > 4.f || _waitsnd_f > 240;   // 4 s à ~60 fps
        if (getenv("SNDDBG"))
            printf("[snddbg] t=%.2f f=%d busy=%d kind=%d st=%d pc=%d\n",
                _waitsnd_t, _waitsnd_f,
                _vm.cb_sndbusy ? _vm.cb_sndbusy(_vm.wait_snd) : -1,
                _vm.wait_snd, (int)_vm.state(), _vm.pc());
        if (done) {
            if (_waitsnd_t > 4.f && verbose)
                printf("[waitsnd] timeout kind=%d -> reprise forcée\n",
                    _vm.wait_snd);
            _waitsnd_t = 0.f; _waitsnd_f = 0;
            _vm.continue_after(ScriptVM::WAITSND); _vm.step();
            if (!_vm.busy()) _script_end_box();
        }
    } else { _waitsnd_t = 0.f; _waitsnd_f = 0; }
    // bascule BGM jour/nuit (~1 s de sondage)
    _mus_chk += dt;
    if (_mus_chk > 1.f) { _mus_chk = 0; _music_update(); }
    _pokefol_refresh();                         // suiveur = 1er non-œuf d'équipe
    if (_pfemote > 0.f) _pfemote -= dt;         // bulle ♥ interaction
    if (_pfcry_t > 0.f) _pfcry_t -= dt;         // anti-spam cri
    if (_idle_bub > 0.f) _idle_bub -= dt;       // bulle « … » joueur inactif
    {   bool idle_ok = _pstep == 0 && !_pjump && _vel.y >= -1.5f &&
                       !_mbox && !_vm.busy() && !ui_frozen;
        if (idle_ok && _idle_bub <= 0.f) {
            _idle_t += dt;
            if (_idle_t > 9.f) { _idle_bub = 2.6f; _idle_t = -5.f; }
        } else _idle_t = 0.f;
    }
    if (_pfjump) {                              // saut de joie (~0.4 s)
        _pfjt += dt;
        if (_pfjt > 0.4f) _pfjump = false;
    }
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
        if (_vm.battle_tutorial) {                  // tuto capture : rencontre
            _vm.battle_tutorial = 0;                // sauvage scriptée
            _wild_force = true;
            _try_wild(0x02); _enc_steps = 999;
            if (_btl.active) _btl.tutorial = 1;
            else {                                  // pas de table : reprendre
                _vm.battle_lost = 0;
                _vm.continue_after(ScriptVM::BATTLE); _vm.step();
            }
            return;
        }
        if (_vm.battle_sp > 0) {
            _btl_start(false, 0, _vm.battle_sp, _vm.battle_lv);
            _vm.battle_sp = 0; return;
        }
        _btl_start(true, _vm.battle_tid, 0, 0, _vm.battle_tid2);
        _vm.battle_tid2 = 0;
        return;
    }
    // DS WaitButton : la croix ne fait que pivoter le joueur (TryFace,
    // movement.c) — le pas n'est pas consommé, le script continue d'attendre.
    if (_vm.state() == ScriptVM::WAITBTN && (ix || iz))
        _pdir = iz < 0 ? 1 : iz > 0 ? 0 : ix < 0 ? 2 : 3;
    // DS : un pas déjà engagé se termine (le sprite rejoint sa tuile logique)
    // même pendant un script — sinon le dresseur approchait vers la tuile
    // cible jamais atteinte et finissait en diagonal du joueur.
    if (_vm.busy() && _pstep != 2) return;       // joueur verrouillé pendant script
    if (_wild_t > 0) {                       // intro rencontre sauvage en cours
        _wild_t -= dt;
        if (_wild_t <= 0) { _dialog.clear(); _mbox = false; }
        return;
    }
    // (les tâches d'approche dresseur tournent plus haut, pendant l'attente VM)
    // ordre DS (field_control) : frame -> sight -> forcing -> coord -> push.
    if (_lost_pc && !_vm.busy()) {            // LostGoPc : fondu noir puis réveil
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
    if (!_vm.busy()) {
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
    if (!getenv("NOTRAINER") && !_vm.busy() && _check_trainer_sight()) return; // dresseur sight

    // ── mouvement joueur DS (player_move.c) : machine NONE→TURNING→MOVING ──
    // un pas = exactement 1 tuile ; tap = tourner d'abord, hold = enchaîner
    auto npc_at = [this](int tx, int tz) -> int {     // index PNJ occupant, -1 sinon
        for (size_t i = 0; i < _objects.size(); i++) {
            if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
            if (_objpass.count((int)i)) continue;      // puits rempli : traversable
            if (_objnocoll.count((int)i)) continue;    // STATUS_18 DS : fantôme
            if ((int)i == _follower) continue;         // FOLLOW_PLAYER : traversable
            if ((int)floorf(_objpos[i].x / TILE) == tx &&
                (int)floorf(_objpos[i].z / TILE) == tz) {
                // DS sub_02063F00 : MapObject_Y en 1/8 tuile, |Δy| < 2 -> bloque
                // = 1 tuile (16 px), pas 2. Pas encore posé : hauteur d'event
                // comme RecalculateObjectHeight DS.
                float oy = _objpos[i].y > -1e8f ? _objpos[i].y
                                               : _objects[i].y / 4096.f;
                if (fabsf(oy - _ppos.y) >= (float)TILE) {
                    if (getenv("NPCDBG"))
                        printf("[npcskip] obj=%d gfx=%d y=%.0f vs p=%.0f\n",
                            (int)i, _objects[i].gfx, oy, _ppos.y);
                    continue;
                }
                return (int)i;
            }
        }
        return -1;
    };
    int want = iz < 0 ? 1 : iz > 0 ? 0 : ix < 0 ? 2 : ix > 0 ? 3 : -1; // dir monde
    // Monde Distorsion : STEPS DS (support.ts OpenSinnoh) — pas d'une tuile
    // en espace partagé par surface [kind-1][dir monde 0=S 1=N 2=O 3=E].
    static const int8_t DWS[3][4][3] = {
        {{0,-1,0},{0,1,0},{0,0,1},{0,0,-1}},    // mur ouest : haut=+y, gch=+z
        {{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}},    // mur est   : haut=+y, gch=-z
        {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}},    // plafond   : haut=+z, gch=-x
    };
    // facing dessiné (uprightStep DS) : rotation 90° anti-h / horaire / 180°
    static const int8_t DWF[3][4] = {
        {3,2,0,1}, {2,3,1,0}, {1,0,3,2},
    };
    if (_pstep == 2) {                                  // pas en cours -> cible
        // DW sol (kinds mur/plafond = 1-3) : la chute keep-height
        // continue pendant le pas — _pty figée à la hauteur de départ
        // bloquerait le pas (ddy résiduel). La cible suit le sol réel.
        if (_dw.on && _dw.fp_kind < 1 && !_pjump) {
            float gy = _ground_y(_ppos, _ppos.y);
            if (gy > -1e8f) _pty = gy;
        }
        float ddx = _ptx - _ppos.x, ddz = _ptz - _ppos.z;
        float ddy = _pjump ? 0.f : _pty - _ppos.y;    // mur : pas vertical
        // la fin du pas se juge à l'horizontale : au sol, la hauteur
        // est reprise par le snap de plaques — un dénivelé entre tuiles
        // voisines figeait dl (~1.4) et le pas ne finissait jamais.
        // Murs/plafonds Distorsion : y est l'axe de marche -> compte.
        bool dvert = _dw.on && _dw.fp_kind > 0;
        float dl = dvert ? sqrtf(ddx * ddx + ddy * ddy + ddz * ddz)
                         : sqrtf(ddx * ddx + ddz * ddz);
        if (getenv("PSTEPDBG")) printf("[pstep] pos=(%.1f,%.1f) pt=(%.1f,%.1f) dl=%.2f dt=%.4f\n",
            _ppos.x, _ppos.z, _ptx, _ptz, dl, dt);
        float step = _pstep_spd * dt;
        _panim += dt * (_pstep_spd > 60 ? 16.f : 8.f);  // cycle anim @30Hz
        if (_panim > 4.f) _panim -= 4.f;
        if (_pjump) {                              // rebord : arc sJumpHeights DS
            float tot = _pjump * TILE;             // JumpNear=1, JumpFar=2 tuiles
            static const int JH[16] = {4,6,8,10,11,12,12,12,11,10,9,8,6,4,0,0};
            int ji = (int)((1.f - dl / tot) * 15.99f);
            if (ji < 0) ji = 0;
            // base = sol logique (DS : pos.y fixe, arc = offset sprite) —
            // sondée à _pty (hauteur cible) pour ne pas réinjecter l'arc
            Vec3 prb = _ppos; prb.y = _pty;
            float gy2 = _ground_y(prb, _pty);
            _ppos.y = (gy2 > -1e8f ? gy2 : _pty) + JH[ji];
        }
        if (dl <= step) {
            _ppos.x = _ptx; _ppos.z = _ptz;
            // atterrissage mur/plafond Distorsion : imposer _pty. Au sol,
            // y reste celui suivi par le snap (réinjecter l'ancienne
            // hauteur faisait rebondir le perso en fin de pas sur pente).
            if (!_pjump && dvert) _ppos.y = _pty;
            _pstep = 0; _pjump = 0;
            // Monde Distorsion : HandlePlayerMoved DS (triggers fantômes,
            // angles caméra, points de saut sur plateformes)
            if (_dw.on) _dw_moved(_pdir);
            // crissement dans la neige (SEQ_SE_DP_YUKIASHI) à chaque pas.
            // Le pas partage le player 3 avec les boucles ambient -> la
            // boucle coupée est mémorisée puis rééditée dans _weather_tick.
            if (_snd && _wthr.type >= 5 && _wthr.type <= 7 &&
                _wthr.intensity > 0.3f && !_psurf) {
                int y = _snd->seq_named("SEQ_SE_DP_YUKIASHI");
                if (y >= 0) {
                    _wthr.amb_cut = _wthr.amb >= 0 ? _wthr.amb
                                                  : _wthr.nami_seq;
                    _snd->se(y);
                }
            }
            // glace DS (field_behavior) : la glissière continue tant qu'on
            // est sur TB_ICE — la direction de marche est forcée
            _pslide = _tile_beh((int)floorf(_ppos.x / TILE),
                (int)floorf(_ppos.z / TILE)) == TB_ICE;
            if (_repel > 0 && --_repel == 0)
                _repel_ask = true;              // Repousse expiré : proposer
            // pension DS : +1 XP/pas -> +1 niv./256 pas ; œuf jeté tous
            // les 256 pas selon la compatibilité (70/50/20 %)
            if (_vm.daycare[0].species || _vm.daycare[1].species) {
                if (++_vm.daycare_steps >= 256) {
                    _vm.daycare_steps = 0;
                    for (auto& d : _vm.daycare)
                        if (d.species && d.level + d.dc_gain < 100)
                            d.dc_gain++;
                    if (!_vm.daycare_egg && _vm.daycare_compat >= 0 &&
                        _vm.daycare_compat < 3) {
                        static const int CH[3] = {70, 50, 20};
                        if (rand() % 100 < CH[_vm.daycare_compat])
                            _vm.daycare_egg = true;
                    }
                }
            }
        }
        else { _ppos.x += ddx / dl * step;
               if (dvert) _ppos.y += ddy / dl * step;   // sol : snap gère
               _ppos.z += ddz / dl * step; }
    } else {
        if (_pstep == 1 || _pstep == 3) {               // turn / bump en cours
            _pstep_t -= dt;
            if (_pstep == 3) { _panim += dt * 4.f; if (_panim > 4.f) _panim -= 4.f; }
            if (_pstep_t <= 0) _pstep = 0;
        }
        if (getenv("PSTEPDBG"))
            printf("[mvdbg] pstep=%d pt=%.2f want=%d pdir=%d dt=%.4f\n",
                _pstep, _pstep_t, want, _pdir, dt);
        if (_pstep == 0) {
            if (_pslide) want = _pdir;              // glace : pas de choix de dir
            // Canalave : trajet forcé — horizontal = marche dans le sens de
            // la plateforme (WALK_FASTER DS), vertical = immobile (porté)
            if (_gym.kind == 4 && _gym.cn_move >= 0) {
                static const int DIRW[6] = {-1, -1, 1, 0, 3, 2};
                want = DIRW[_gym.cn_dir];
            }
            // Hearthome : le trajet d'ascenseur DS bloque la marche — le
            // joueur est porté par _gym_tick (SetYPos) jusqu'à l'arrivée.
            if (_gym.kind == 5 && _gym.hh_lift_move) want = -1;
            // Monde Distorsion : trajet ascenseur / event plateforme /
            // saut / cascade — le joueur est porté par _dw_tick
            if (_dw.on && _dw_busy()) want = -1;
            if (_bpush.obj >= 0) want = -1;      // poussée/chute rocher en cours
            // platform_lift (Iron Island / Ligue) : idem pendant le trajet.
            if (_gym.kind == 6 && _gym.pl_move) want = -1;
            // Grand Marais : trajet tram = marche forcée N/S (DS RideTram —
            // le tram suit _ppos.z + 8 dans _gym_tick).
            if (_gym.kind == 7 && _gym.gm_task >= 3)
                want = _gym.gm_south ? 0 : 1;
            // Monde Distorsion : saut sur plateforme en cours — bloqué
            if (_dw.on && _dw.jump_t > 0) want = -1;
            // vortex DoDWWarp : terrain gelé pendant la cinématique DS
            if (_dwarp_t >= 0) want = -1;
            if (want < 0) { _panim = 0.f; }
            // Distorsion : saut plateforme déclenché par la PRESSION sur la
            // tuile trigger (avant le pas — le retour mur -> terre sinon ne
            // part jamais, le pas hors de la paroi étant bloqué).
            else if (_dw.on && _dw_jump_press(want)) { }
            else if (want != _pdir) {                    // TURNING : face d'abord
                _pdir = want; _pstep = 1; _pstep_t = 2.f / 30.f; // 2 ticks @30Hz DS
                _pface = (_dw.on && _dw.fp_kind > 0)
                    ? DWF[_dw.fp_kind - 1][want & 3] : -1;
            } else {                                     // MOVING : collision ?
                int ptx = (int)floorf(_ppos.x / TILE);
                int ptz = (int)floorf(_ppos.z / TILE);
                int ddx, ddz; dirv(_pdir, ddx, ddz);
                int sdy = 0;
                float cy = _ppos.y + 4.f;           // hauteur du test collision
                // Monde Distorsion : mur/plafond courant — le pas suit la
                // table STEPS DS (murs : ±y / ±z croisés, plafond : ∓z / ±x)
                // et le facing dessiné est la rotation de la direction.
                if (_dw.on && _dw.fp_kind > 0) {
                    const int8_t* s = DWS[_dw.fp_kind - 1][_pdir & 3];
                    ddx = s[0]; sdy = s[1]; ddz = s[2];
                    _pface = DWF[_dw.fp_kind - 1][_pdir & 3];
                    cy = (floorf(_ppos.y / TILE + 0.5f) + sdy) * TILE + 4.f;
                }
                int ntx = ptx + ddx, ntz = ptz + ddz;
                bool nblock = !noclip && _blocked(
                    {ntx * TILE + TILE / 2, cy, ntz * TILE + TILE / 2});
                // Canalave : le trajet plateforme est un mouvement forcé DS
                // (WALK_FASTER_* = déplacement scripté, sans test de collision
                // sur la lane bloquée du bitmap).
                if (_gym.kind == 4 && _gym.cn_move >= 0) nblock = false;
                // Grand Marais : le trajet tram survole le rail (0x8000).
                if (_gym.kind == 7 && _gym.gm_task >= 3) nblock = false;
                // DS WillPlayerCollide/GetVerticalDirection : dénivelé
                // >= 20 px entre la position courante et le sol de la
                // tuile visée = mur (falaise) — l'attribut seul ne
                // suffit pas (passe aussi le dénivelé négatif : on ne
                // descend pas d'une falaise à pied, il faut un rebord).
                // Gyms/DW : collision dynamique dédiée — la règle
                // statique ne s'y applique pas ; ni quand _phcalc gèle y.
                if (!nblock && !noclip && !_psurf && !_dw.on &&
                    _gym.kind == 0 && _phcalc) {
                    float gy2 = _ground_y({ntx * TILE + TILE / 2.f, cy,
                                           ntz * TILE + TILE / 2.f}, _ppos.y);
                    if (gy2 > -1e8f && fabsf(gy2 - _ppos.y) >= 20.f)
                        nblock = true;
                }
                if (getenv("PSTEPDBG"))
                    printf("[want] t=(%d,%d)->(%d,%d) beh=%d block=%d npc=%d\n",
                        ptx, ptz, ntx, ntz, _tile_beh(ntx, ntz), (int)nblock,
                        (int)npc_at(ntx, ntz));
                // rebord : behavior JUMP_<dir> sur la tuile cible -> saut 2 tuiles
                // DS player_move.c WillJump : E=56 O=57 N=58 S=59 (dir monde 0=S)
                static const int JMP[4] = {59, 58, 57, 56};
                // JUMP_<dir>_TWICE (0x5A-0x5D) : saut 2 tuiles du Monde
                // Distorsion (MOVEMENT_ACTION_JUMP_DISTORTION_WORLD DS) —
                // l'atterrissage est la tuile de saut opposée, bloquée par
                // design : pas de test de collision à l'arrivée.
                static const int JM2[4] = {0x5B, 0x5A, 0x5C, 0x5D};
                int bh = _tile_beh(ntx, ntz);
                // sur mur/plafond, le comportement vient de la grille
                // tw_arc_attr de la plateforme courante (octet bas)
                if (_dw.on && _dw.fp_kind > 0) {
                    auto* di = _dw.info(_header);
                    uint16_t pa = _dw_fp_attr(ntx + (di ? di->ox : 0),
                        (int)floorf(cy / TILE) + (di ? di->oa : 0),
                        ntz + (di ? di->oz : 0));
                    bh = (pa != 0xFFFF && pa != 0xFFFE) ? (pa & 0xFF) : 0;
                }
                // JUMP_<dir>_TWICE : sol/terre seulement DS (pas les murs)
                bool jtwice = bh == JM2[_pdir] &&
                    !(_dw.on && _dw.fp_kind > 0);
                if (_surfable(bh)) nblock = !_psurf;   // eau : surf only
                // Force (ov5_021DFF1C DS) : FLAG_STRENGTH_ACTIVE posé et un
                // rocher (gfx 84) sur la tuile visée -> poussée de 1 tuile en
                // 16 ticks si la tuile au-delà est libre ; dans le Monde
                // Distorsion une tuile de chute l'autorise même bloquée
                // (boulderWillFall). Le joueur reste sur place 8 ticks.
                bool bpush = false;
                if (!noclip && _vm.flag(2402)) {
                    int bidx = -1;
                    for (size_t i = 0; i < _objects.size(); i++) {
                        if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
                        if (_objpass.count((int)i)) continue;   // puits rempli
                        if (_objpos[i].y < -1e8f) continue;     // pas posé
                        if (_objects[i].gfx != 84) continue;
                        if ((int)floorf(_objpos[i].x / TILE) == ntx &&
                            (int)floorf(_objpos[i].z / TILE) == ntz)
                            { bidx = (int)i; break; }
                    }
                    if (bidx >= 0) {
                        int bx = ntx + ddx, bz = ntz + ddz;
                        bool beyond = _blocked({bx * TILE + TILE / 2, cy,
                                                bz * TILE + TILE / 2})
                            || npc_at(bx, bz) >= 0;
                        auto* di = _dw.info(_header);
                        int fp = -1;
                        int fall = (_dw.on && di) ? _dw_fall_at(
                            bx + di->ox, bz + di->oz, &fp) : 0;
                        if (!beyond || fall) {
                            _bpush = BoulderPush{};
                            _bpush.obj = bidx; _bpush.dir = _pdir;
                            _bpush.from = _objpos[bidx];
                            _bpush.to = _bpush.from
                                + Vec3((float)(ddx * TILE), 0.f,
                                       (float)(ddz * TILE));
                            _pslide = 0;
                            _pstep = 3; _pstep_t = 8.f / 30.f;   // ON_SPOT 8t DS
                            bpush = true;
                            if (verbose)
                                printf("[push] rocher idx=%d tile=(%d,%d) fall=%d\n",
                                    bidx, bx, bz, fall);
                        }
                    }
                }
                // Eterna : saut par-dessus l'aiguille des heures
                // (EternaGym_IsHourHandJumpTile : tuile + axe selon l'état)
                bool ejump = false;
                if (_gym.kind == 3) {
                    const int8_t* jt = ET_JUMP[_eterna_clock];
                    bool ns = (_pdir == 0 || _pdir == 1);
                    ejump = ntx == jt[0] && ntz == jt[1] && ns == (jt[2] != 0);
                }
                if (bpush) { /* poussée lancée : _boulder_tick pilote */ }
                else if ((bh == JMP[_pdir] || jtwice || ejump) &&
                    (jtwice || !_blocked({(ntx + ddx) * TILE + TILE / 2,
                        _ppos.y + 4, (ntz + ddz) * TILE + TILE / 2}))) {
                    // JUMP_*_TWICE du Monde Distorsion : 3 tuiles en 24
                    // ticks (MOVEMENT_ACTION_JUMP_DISTORTION_WORLD DS :
                    // distance 2px/frame -> 48px, StepDir tous les 16px)
                    bool dwj = _dw.on && _dw.fp_kind > 0;
                    int jlen = jtwice ? 3 : 2;
                    _ptx = (ptx + jlen * ddx) * TILE + TILE / 2;
                    _ptz = (ptz + jlen * ddz) * TILE + TILE / 2;
                    _pty = dwj ? (floorf(_ppos.y / TILE + 0.5f) + jlen * sdy) * TILE
                               : _ppos.y;
                    _pstep_spd = 60.f;               // JumpFar : 2 tuiles/16 ticks @30Hz
                    _pstep = 2;
                    _pjump = jtwice ? 3 : (dwj ? 0 : 2);  // arc = portée saut
                    if (verbose) printf("[jump] (%d,%d)->(%d,%d) beh=%d\n",
                        ptx, ptz, ntx + ddx, ntz + ddz, bh);
                } else if (nblock || (!noclip && npc_at(ntx, ntz) >= 0)) { // mur/PNJ : bump + face
                    // gym Veilstone : pousser un sac de frappe le fait
                    // glisser (VeilstoneGym_TryKickPunchingBag DS).
                    { int boi = npc_at(ntx, ntz);
                      if (_gvl_bag_obj(boi) >= 0) _gvl_kick(boi, want); }
                    // Field_CheckMapTransition : pousser contre du terrain
                    // bloqué peut déclencher une transition (un PNJ ne suffit
                    // pas — DS CheckCollision ne teste que le terrain).
                    if (nblock) {
                        // a) la tuile visée : warp + comportement DOOR
                        //    -> transition porte, pas d'anim de bump
                        int wi = _warp_at(ntx, ntz);
                        if (wi >= 0 && _tile_beh(ntx, ntz) == TB_DOOR &&
                            _warp_depart(wi, WT_DOOR,
                                _hh_door_dir(ntx, ntz))) return;
                        // b) la tuile courante : les comportements E/O/S et
                        //    escaliers exigent de pousser dans leur direction
                        int cb = _tile_beh(ptx, ptz);
                        bool dirok = true;
                        if (cb == TB_ENTRANCE_E || cb == TB_WARP_E ||
                            cb == TB_STAIRS_E) dirok = _pdir == 3;
                        else if (cb == TB_ENTRANCE_W || cb == TB_WARP_W ||
                            cb == TB_STAIRS_W) dirok = _pdir == 2;
                        else if (cb == TB_ENTRANCE_S || cb == TB_WARP_S ||
                            cb == TB_STAIRS_S) dirok = _pdir == 0;
                        else if (cb == TB_ENTRANCE_N || cb == TB_WARP_N ||
                            cb == TB_STAIRS_N) dirok = _pdir == 1;
                        if (dirok && (wi = _warp_at(ptx, ptz)) >= 0) {
                            if (cb == TB_DOOR &&
                                _warp_depart(wi, WT_DOOR,
                                    _hh_door_dir(ptx, ptz))) return;
                            if ((cb == TB_STAIRS_E || cb == TB_STAIRS_W ||
                                 cb == TB_STAIRS_N || cb == TB_STAIRS_S) &&
                                _warp_depart(wi, WT_STAIRS, _pdir)) return;
                            if ((cb == TB_ENTRANCE_E || cb == TB_WARP_E ||
                                 cb == TB_ENTRANCE_W || cb == TB_WARP_W ||
                                 cb == TB_ENTRANCE_N || cb == TB_WARP_N ||
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
                                if (!_vm.busy()) _script_end_box();
                                goto bump_done;
                            }
                    }
                    if (verbose) printf("[bump] t=(%d,%d) block=%d npc=%d\n",
                        ntx, ntz, (int)nblock, (int)npc_at(ntx, ntz));
                    // journal bump -> bump_dbg.txt : cause exacte du mur
                    // (attr/PNJ) pour traquer les murs invisibles en jeu.
                    {   int nb = npc_at(ntx, ntz);
                        if (FILE* bf = fopen("bump_dbg.txt", "a")) {
                            const Cell* cc = _cell_at(ntx * TILE + TILE / 2.f,
                                ntz * TILE + TILE / 2.f);
                            fprintf(bf,
                                "[bump] hdr=%d p=(%d,%d,y=%.0f) t=(%d,%d) "
                                "attr=%04x cell=%d terr=%d npc=%d",
                                _header, ptx, ptz, _ppos.y, ntx, ntz,
                                tile_attr_dbg(ntx, ntz), cc ? 1 : 0,
                                (int)nblock, nb);
                            if (nb >= 0)
                                fprintf(bf,
                                    " id=%d gfx=%d mv=%d oy=%.0f evy=%.0f",
                                    _objects[nb].id, _objects[nb].gfx,
                                    _objects[nb].move, _objpos[nb].y,
                                    _objects[nb].y / 4096.f);
                            fprintf(bf, "\n"); fclose(bf);
                        } }
                    bump_done:
                    _pslide = 0;                            // mur : fin de glissière
                    _pstep = 3; _pstep_t = 16.f / 30.f;  // WALK_ON_SPOT_SLOW : 16 ticks @30Hz
                } else {
                    _ptx = ntx * TILE + TILE / 2;
                    _ptz = ntz * TILE + TILE / 2;
                    _pty = (_dw.on && _dw.fp_kind > 0) ? cy - 4.f : _ppos.y;
                    // DS @30Hz : marche 2px/tick=8 ticks/tuile, course 4px=4 ticks,
                    // glace fixe 4 ticks, surf fixe 4 ticks (B sans effet)
                    _pstep_spd = (_pslide || _psurf) ? 120.f
                        : (_can_run && run_held) ? 120.f : 60.f;
                    // Grand Marais : paliers de vitesse du trajet tram DS
                    // (WALK_SLOWER..WALK_FASTER -> 15/30/60/120/240 px/s).
                    if (_gym.kind == 7 && _gym.gm_task >= 3) {
                        static const float RSPD[7] =
                            {15.f, 30.f, 60.f, 60.f, 120.f, 120.f, 240.f};
                        _pstep_spd = RSPD[_gym.gm_lvl];
                    }
                    // tuiles profondes DS (neige/boue) : vitesse réduite et
                    // course impossible tant qu'on est dedans
                    int cb2 = _tile_beh(ptx, ptz);
                    if (cb2 == 161 || cb2 == 162 || cb2 == 164 || cb2 == 166)
                        _pstep_spd = std::min(_pstep_spd, 40.f);   // deep/mud
                    else if (cb2 == 163 || cb2 == 165 || cb2 == 167)
                        _pstep_spd = std::min(_pstep_spd, 30.f);   // deepest/deep
                    _pstep = 2;
                    if (_follower >= 0) {          // suiveur : mémorise la tuile
                        _ftrail.push_back({ptx, ptz});
                        if (_ftrail.size() > 16) _ftrail.pop_front();
                    }
                    if (_pokefol >= 0) {           // suiveur Pokémon : idem
                        _pftrail.push_back({ptx, ptz});
                        if (_pftrail.size() > 16) _pftrail.pop_front();
                    }
                }
            }
        }
    }

    if (_vm.busy()) return;                      // fin du pas absorbé : stop

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
    // Canalave vertical : SetHeightCalculationEnabled(FALSE) DS — le y est
    // piloté par la plateforme (_gym_tick), pas par les plaques.
    bool cn_ride = _gym.kind == 4 && _gym.cn_move >= 0 && _gym.cn_dir <= 1;
    // Hearthome : ascenseur DP en trajet — y piloté par _gym_tick (idem DS).
    bool hh_ride = _gym.kind == 5 && _gym.hh_lift_move != 0;
    // platform_lift : SetHeightCalculationEnabled(FALSE) DS pendant le trajet.
    bool pl_ride = _gym.kind == 6 && _gym.pl_move != 0;
    // SetPlayerHeightCalculationEnabled(FALSE) DS : y figé (embarquement
    // tram, trajets portés) — ni snap de sol ni gravité.
    // Monde Distorsion : mur/plafond = support — ni gravité ni snap de sol
    // (le y est un axe de marche, le joueur reste collé à la paroi) ;
    // idem pendant le saut plateforme (jump_t pilote déjà _ppos.y).
    if (_phcalc && !_pjump && !cn_ride && !hh_ride && !pl_ride &&
        !(_dw.on && (_dw.fp_kind > 0 || _dw.jump_t > 0))) {
        // DS : plaque la plus proche du y courant (pas de biais +60 :
        // sous un pont/terrasse le joueur sautait sur la plaque haute).
        float gy = _ground_y(_ppos, _ppos.y);
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
                float dl = sqrtf(ddx * ddx + ddz * ddz);
                float spd = _ftrail.empty() ? 60.f : 120.f, st = spd * dt;
                _fanim += dt * (spd / 8.f);      // cycle marche (~2/tuile)
                if (dl <= st) { fp.x = _ftx; fp.z = _ftz; _fstep = 0; }
                else { fp.x += ddx / dl * st; fp.z += ddz / dl * st; }
            } else if (!_ftrail.empty()) {
                auto t = _ftrail.front(); _ftrail.pop_front();
                int ftx = (int)floorf(fp.x / TILE), ftz = (int)floorf(fp.z / TILE);
                int ddx = t.first - ftx, ddz = t.second - ftz;
                if (abs(ddx) + abs(ddz) > 3)        // trop loin : rappel
                    { fp = {_ppos.x, fp.y, _ppos.z}; _ftrail.clear(); }
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
                    _fstep = 1; _fanim = 0.f;
                }
            }
        }
    }
    _pokefol_tick(dt);                       // Pokémon suiveur : même serpent

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
        // CanalaveGym_CheckIfPlayerOnPlatform : Field_ProcessStep DS, avant
        // coord/warp — marcher sur la tuile d'une plateforme la déclenche.
        if (_gym.kind == 4 && _gym.cn_move < 0) {
            // DS : pos.y / TILE_SIZE — arrondi car la gravité laisse y ~0.02
            // sous la plaque (snap + intégration dans le même frame).
            int pty = (int)roundf(_ppos.y / TILE);
            for (int i = 0; i < 24; i++) {
                if (getenv("WARPDBG") && _gym.cn_pos[i][0] == tx &&
                    _gym.cn_pos[i][2] == tz)
                    printf("[gymdbg] plat %d pos={%d,%d,%d} vs (%d,%d,%d)\n",
                        i, _gym.cn_pos[i][0], _gym.cn_pos[i][1],
                        _gym.cn_pos[i][2], tx, pty, tz);
                if (_gym.cn_pos[i][1] == pty &&
                    _gym.cn_pos[i][0] == tx && _gym.cn_pos[i][2] == tz) {
                    bool inB = (_gym.cn_inB >> i) & 1;
                    _gym.cn_inB ^= (1u << i);         // TogglePlatformState
                    const CanalavePlat& T = CANA_PLAT[i];
                    _gym.cn_move = i;
                    if (T.model == 295) {             // x_dir : inB -> A (ouest)
                        _gym.cn_dir = inB ? 5 : 4;
                        _gym.cn_dest = (inB ? T.ax : T.bx) * TILE + TILE / 2.f;
                        _gym.cn_pos[i][0] = inB ? T.ax : T.bx;
                    } else if (T.model == 294) {      // z_dir : inB -> A (nord)
                        _gym.cn_dir = inB ? 2 : 3;
                        _gym.cn_dest = (inB ? T.az : T.bz) * TILE + TILE / 2.f;
                        _gym.cn_pos[i][2] = inB ? T.az : T.bz;
                    } else {                          // y_dir/red : inB -> A (bas)
                        _gym.cn_dir = inB ? 1 : 0;
                        _gym.cn_dest = (inB ? T.ay : T.by) * TILE;
                        _gym.cn_pos[i][1] = inB ? T.ay : T.by;
                        if (inB) _gym.cn_fl[i] = T.fa;  // DOWN : floorA immédiat
                    }
                    static const int DIRW[6] = {-1, -1, 1, 0, 3, 2};
                    int wd = DIRW[_gym.cn_dir];
                    if (wd >= 0) _pdir = wd;           // marche forcée (E/W/N/S)
                    if (_snd) {                        // ascenseur DS
                        int s = _snd->seq_named("SEQ_SE_DP_ELEBETA");
                        if (s >= 0) _snd->se(s);
                    }
                    if (getenv("WARPDBG"))
                        printf("[gym] canalave plat %d dir=%d dest=%.0f\n",
                            i, _gym.cn_dir, _gym.cn_dest);
                    return;
                }
            }
        }
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
            if (getenv("WARPDBG"))
                printf("[warpdbg] check warp (%d,%d) cb=%02x pdir=%d\n",
                       tx, tz, cb, _pdir);
            if (cb == TB_ESCALATOR_FLIP) {          // O<->E imposés par l'escalator
                wtype = WT_ESCAL;
                if (_pdir == 2) adir = 3;
                else if (_pdir == 3) adir = 2;
            } else if (cb == TB_ESCALATOR) {        // conserve la direction
                wtype = WT_ESCAL;
                if (_pdir == 2 || _pdir == 3) adir = _pdir;
            } else if (cb == TB_ENTRANCE_N || cb == TB_WARP_N) {
                if (_pdir == 1) adir = 1;           // entrée/warp N : dir N exigée
            } else if (cb == TB_ENTRANCE_S || cb == TB_WARP_S) {
                if (_pdir == 0) adir = 0;
            } else if (cb == TB_ENTRANCE_E || cb == TB_WARP_E) {
                if (_pdir == 3) adir = 3;
            } else if (cb == TB_ENTRANCE_W || cb == TB_WARP_W) {
                if (_pdir == 2) adir = 2;
            } else if (cb == TB_STAIRS_N || cb == TB_STAIRS_S ||
                       cb == TB_STAIRS_E || cb == TB_STAIRS_W) {
                // escaliers DS : on marche DESSUS dans leur direction
                static const int SD[4] = {0x61, 0x60, 0x5F, 0x5E}; // S,N,O,E
                if (cb == SD[_pdir]) { adir = _pdir; wtype = WT_STAIRS; }
            } else if (cb == TB_WARP_PANEL) {
                wtype = WT_PANEL;
                adir = _pdir;
            } else if (cb != TB_ENTRANCE_E && cb != TB_WARP_E &&
                       cb != TB_ENTRANCE_W && cb != TB_WARP_W &&
                       cb != TB_ENTRANCE_S && cb != TB_WARP_S &&
                       cb != TB_ENTRANCE_N && cb != TB_WARP_N &&
                       cb != TB_STAIRS_E && cb != TB_STAIRS_W &&
                       cb != TB_STAIRS_N && cb != TB_STAIRS_S &&
                       cb != TB_DOOR) {
                // warp ordinaire : toute tuile sans comportement directionnel
                // tire au pas (DS Field_CheckTransition — ex. sorties de lac
                // posées sur herbe haute, tuiles 0x0002 sans attribut warp)
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
        (!_psurf && _enc_rate <= 0) ||
        (getenv("NOENC") && !_wild_force)) return;   // debug : pas de rencontres
    static const uint8_t ENC_BEH[] = {          // sTileBehaviorFlags & ENCOUNTER (DS)
        0x02,0x03,0x05,0x06,0x08,0x0B,0x10,0x11,0x12,0x15,0x22,
        0x24,0x25,0x2A,0x72,0x77,0x7B,0xA6,0xA7 };
    bool enc = false;                            // surf = 0x10..(sections eau)
    for (uint8_t b : ENC_BEH) if ((uint8_t)tb == b) { enc = true; break; }
    if (!enc) return;
    bool force = getenv("WILD1") != nullptr || _wild_force; // debug : force
    _wild_force = false;
    // DS : la période de grâce utilise le taux DE LA TUILE (surf = taux eau)
    int crate = _enc_rate;
    if (_psurf)
        crate = (_enc && _enc->size() >= 208) ? (int)u32(*_enc, 204) : 0;
    if (crate <= 0) return;
    int grace = 8 - std::min(8, ((crate << 8) / 10) >> 8);      // rate 30 -> 5 pas
    if (_enc_steps++ < grace && !force) {        // pas post-combat : 5% seulement
        if (rand() % 100 >= 5) return;
    }
    int flat = 40 + (tb == 0x03 ? 30 : 0);       // très hautes herbes / vélo +30
    if (flat > 100) flat = 100;
    // rencontre eau (surf) : section WildEncounters.surfEncounters du fichier
    // = taux @204 puis 5 slots de 8o (nvMax, nvMin, pad, espèce) — poids 60/30/5/4/1
    if (_psurf) {
        if (_enc->size() < 248) return;
        if (!force && (rand() % 100 >= flat || rand() % 100 >= crate)) return;
        static const int WCUM[5] = {60, 90, 95, 99, 100};
        int roll = rand() % 100, slot = 4;
        for (int s = 0; s < 5; s++) if (roll < WCUM[s]) { slot = s; break; }
        size_t so = 208 + slot * 8;
        int lmax = (int)(int8_t)(*_enc)[so], lmin = (int)(int8_t)(*_enc)[so + 1];
        if (lmax < lmin) std::swap(lmax, lmin);
        int lvl = lmin + (lmax > lmin ? rand() % (lmax - lmin + 1) : 0);
        int species = (int)u32(*_enc, so + 4);
        if (species <= 0) return;
        if (_repel > 0 && _vm.party[0].species > 0 &&   // Repousse DS
            lvl < _vm.party[0].level) { _enc_steps = 0; return; }
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
    int lvl = (int)(int8_t)(*_enc)[so];              // DS GrassEncounter.level (+0 ; +1 = padding)
    int species = (int)u32(*_enc, so + 4);
    // remplacements DS (wild_encounters.c) : essaim -> slots 0,1 si map du jour
    // (actif après le Pokédex, flag 146) ; jour/nuit -> slots 2,3
    if (slot <= 1 && _enc->size() >= 0x6C && _vm.flag(146)) {
        int smap = 0, ssp = 0; _swarm_map_species(&smap, &ssp);
        if (smap == _header)
            species = (int)u32(*_enc, 0x64 + slot * 4);   // swarmEncounters[0/1]
    } else if (slot >= 2 && slot <= 3 && _enc->size() >= 0x78) {
        int h = dbg_hour >= 0 ? dbg_hour :
            [] { time_t t = time(nullptr); struct tm lt{};
                 localtime_s(&lt, &t); return lt.tm_hour; }();
        if (h >= 10 && h < 20)
            species = (int)u32(*_enc, 0x6C + (slot - 2) * 4);   // dayEncounters
        else if (h >= 20 || h < 4)
            species = (int)u32(*_enc, 0x74 + (slot - 2) * 4);   // nightEncounters
    }
    if (species <= 0) return;
    // DS TryEncounterRoamer : un roamer actif sur cette map remplace
    // la rencontre 1 fois sur 2 (Repousse s'applique à son niveau aussi)
    _wild_roamer = -1;
    {
        int cand[6], nc = 0;
        for (int i = 0; i < 6; i++)
            if (_roamers[i].sp && _roamers[i].hp != 0 &&
                _roamers[i].route == _header)
                cand[nc++] = i;
        if (nc && rand() % 2 == 0) {
            int i = cand[rand() % nc];
            species = _roamers[i].sp; lvl = _roamers[i].lv;
            _wild_roamer = i;
        }
    }
    if (_repel > 0 && _vm.party[0].species > 0 &&       // Repousse DS :
        lvl < _vm.party[0].level) {                     // niveau < meneur -> annulé
        _enc_steps = 0; return; }
    _enc_steps = 0;
    _vm.set_var(0x8000, species); _vm.set_var(0x8001, lvl);
    if (verbose) printf("[wild] slot=%d esp=%d nv=%d%s\n", slot, species, lvl,
        _wild_roamer >= 0 ? " ROAMER" : "");
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
void World::_btl_start(bool trainer, int tid, int sp, int lv, int tid2) {
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
    _btl.trainer = trainer; _btl.tid = tid; _btl.tid2 = tid2;
    // double : flag trdata BATTLE_TYPE_DOUBLES ou 2e dresseur (StartTrainerBattle t1,t2)
    _btl.dbl = trainer && (tid2 > 0 ||
        (battle_trainer_battle_type(_db, tid) & 2));
    // météo overworld -> météo de combat (Gen4 : pluie/sable/grêle/brume)
    switch (_wthr.type) {
    case 2: case 3: case 4: _btl.weather = BW_RAIN; break;
    case 5: case 6: case 7: case 11: _btl.weather = BW_HAIL; break;
    case 10: _btl.weather = BW_SAND; break;
    case 14: case 15: _btl.weather = BW_FOG; break;
    }
    _btl.mi = -1; _btl.mi2 = -1; _btl.me_part = 0;
    for (int i = 0; i < (int)_vm.party.size(); i++) {
        auto& pm = _vm.party[i];
        BatMon m = battle_make(_db, pm.species, pm.level, nullptr, pm.ivs);
        pm.ivs = m.ivs;                                  // IVs tirés à la 1re utilisation
        // données persistées : PID/nature/XP/EVs/amitié/moves
        if (pm.pid) m.pid = pm.pid; else pm.pid = m.pid;
        if (!pm.exp) pm.exp = mon_exp_for(_db, pm.species, pm.level);  // 1re util.
        m.exp = pm.exp; m.friendship = pm.friendship;
        m.cond = (uint32_t)pm.cond;                     // statut persisté DS
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
        m.hp = pm.egg ? 0                               // œuf : jamais au combat
               : (pm.hp < 0 ? m.maxhp : std::min(pm.hp, m.maxhp));
        _btl.me.push_back(m);
        if (_btl.mi < 0 && m.hp > 0) { _btl.mi = i; _btl.me_part |= 1 << i; }
        else if (_btl.dbl && _btl.mi2 < 0 && m.hp > 0) {
            _btl.mi2 = i; _btl.me_part |= 1 << i; }
    }
    if (trainer) {
        _btl.foes = battle_trainer_party(_db, tid);
        auto cs = battle_trainer_class_sprite(_db, tid);
        _btl.foe_class = cs.first; _btl.foe_sprite = cs.second;
        if (tid2 > 0) {                             // 2e dresseur (combat duo)
            auto p2 = battle_trainer_party(_db, tid2);
            int np1 = (int)_btl.foes.size();
            for (auto& m2 : p2) _btl.foes.push_back(m2);
            auto cs2 = battle_trainer_class_sprite(_db, tid2);
            _btl.foe_sprite2 = cs2.second;
            _btl.foe_split = np1;                   // réserve du slot E2 : [np1..)
            if (_btl.foes.size() > 1) _btl.fi2 = np1 < (int)_btl.foes.size()
                ? np1 : 1;
        } else if (_btl.dbl && _btl.foes.size() > 1) _btl.fi2 = 1;
    } else {
        _btl.foes.push_back(battle_make(_db, sp, lv));
        if (_wild_roamer >= 0 && _wild_roamer < 6) {      // roamer DS :
            auto& r = _roamers[_wild_roamer];             // état persistant
            auto& f = _btl.foes.back();
            if (r.pid) f.pid = r.pid;
            if (r.ivs != 0xFFFFFFFFu) f.ivs = r.ivs;
            if (r.hp >= 0) f.hp = std::min(r.hp, f.maxhp);
            f.cond = r.status;
            f.run_away = true;                            // fuit au 1er tour
        }
    }
    for (auto& f : _btl.foes) if (f.species > 0)
        _vm.dex_seen.insert(f.species);             // Pokedex « vu »
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
    if (trainer) {
        if (_btl.dbl) {
            int i2 = _btl.fi2 >= 0 ? _btl.fi2 : 1;
            if (i2 >= (int)_btl.foes.size()) i2 = (int)_btl.foes.size() - 1;
            _btl.msg = (tid2 > 0
                ? tr("Les Dresseurs veulent se battre !\nIls envoient ",
                     "The Trainers want to battle!\nThey send out ")
                : tr("Le Dresseur veut se battre !\nIl envoie ",
                     "The Trainer wants to battle!\nThey send out ")) +
                battle_species_name(_db, _btl.foes[0].species) +
                tr(" et ", " and ") +
                battle_species_name(_db, _btl.foes[i2].species) + " !";
        }
        else
            _btl.msg = tr("Le Dresseur veut se battre !\nIl envoie ",
                          "The Trainer wants to battle!\nThey send out ") +
                battle_species_name(_db, _btl.foes[0].species) + "!";
    } else                                          // DS : apparition puis lancer
        _btl.msg = tr("Un ", "A wild ") +
            battle_species_name(_db, _btl.foes[0].species) +
            tr(" sauvage apparaît !", " appeared!") +
            tr("\nEn avant, ", "\nGo, ") +
            battle_species_name(_db, _btl.me[_btl.mi].species) + "!";
    _pstep = 0;
    if (verbose) printf("[battle] %s tid=%d/%d dbl=%d foes=%d vs me=%d wthr=%d\n",
        trainer ? "trainer" : "wild", tid, tid2, _btl.dbl,
        _btl.foes_out(), _btl.me_out(), _btl.weather);
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
    if (verbose)
        printf("[btl] fin tid=%d won=%d caught=%d foes=%zu me=%zu\n",
            _btl.tid, _btl.won, _btl.caught, _btl.foes.size(), _btl.me.size());
    // musique de victoire DS : WINPOKE/WINTRAIN, puis le thème de zone reprend
    if (_snd && !lost) {
        int w = _snd->seq_named(_btl.trainer ? "SEQ_WINTRAIN" : "SEQ_WINPOKE");
        if (w >= 0) { _snd->music(w); _bgm_victory = true; }
    }
    if (_btl.caught) {                            // capturé -> équipe ou PC
        // cible de la Ball : premier sauvage vivant (id cible en doubles)
        BatMon* cm = nullptr;
        for (int s = 0; s < 2 && !cm; s++) {
            int i = _btl.act_idx(1, s);
            if (i >= 0 && i < (int)_btl.foes.size() && !_btl.foes[i].fainted())
                cm = &_btl.foes[i];
        }
        if (!cm) cm = &_btl.foe();
        ScriptVM::PartyMon pm;
        pm.species = cm->species; pm.level = cm->level;
        pm.hp = cm->hp; pm.ivs = cm->ivs;
        pm.pid = cm->pid; pm.exp = cm->exp;
        pm.friendship = cm->friendship;
        for (int k = 0; k < 6; k++) pm.evs[k] = cm->evs[k];
        pm.nmoves = cm->nmoves;
        for (int k = 0; k < 4; k++) { pm.moves[k] = cm->moves[k]; pm.pp[k] = cm->pp[k]; }
        if (_vm.party.size() < 6) _vm.party.push_back(pm);
        else for (int s = 0; s < 540; s++)
            if (!_vm.pcbox[s].species) { _vm.pcbox[s] = pm; break; }
        _vm.dex_caught.insert(pm.species);           // dex « pris » DS
        if (pm.species == 201) _vm.dex_unown.insert(pm.form);  // Zarbi lettre
    }
    // roamer : persistance DS — PV/statut conservés s'il s'est enfui,
    // désactivé s'il est capturé ; KO reste KO (plus de rencontres)
    if (_wild_roamer >= 0 && _wild_roamer < 6) {
        auto& r = _roamers[_wild_roamer];
        if (_btl.caught) r = Roamer{};
        else {
            const BatMon* fm = nullptr;
            for (auto& f : _btl.foes)
                if (f.species == r.sp) { fm = &f; break; }
            if (fm) {
                r.hp = std::max(0, fm->hp);
                r.status = (int)fm->cond;
                r.ivs = fm->ivs; r.pid = fm->pid;
            }
        }
        _wild_roamer = -1;
    }
    // retour combat -> équipe : PV + progression (XP, niveau, moves, EVs)
    bool night = [] { time_t t = time(nullptr); int h = localtime(&t)->tm_hour;
                      return h >= 20 || h < 4; }();
    std::string evo_msg;
    for (int i = 0; i < (int)_vm.party.size() && i < (int)_btl.me.size(); i++) {
        auto& pm = _vm.party[i]; auto& bm = _btl.me[i];
        bool lvlup = !lost && bm.level > pm.level;
        pm.hp = lost ? bm.maxhp : bm.hp;
        pm.cond = lost ? 0 : (int)bm.cond;          // statut persistant DS
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
                BatMon nm = battle_make(_db, tgt, pm.level, nullptr, pm.ivs);
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
        _dlg_spk = -1;                          // message système : pas de locuteur
    }
    // DS CalcPrizeMoney : niv dernier * 4 * mult classe (sTrainerClassPrizeMul)
    if (!lost && _btl.trainer && _btl.won == 1) {
        static const uint8_t MUL[105] = {
            0,0,4,4,4,4,4,8,4,8,4,8,8,8,6,12,12,12,4,8,16,16,2,16,15,
            15,8,20,2,8,8,30,40,40,50,50,14,16,10,15,15,12,4,4,1,1,8,
            5,12,8,8,30,6,15,15,8,8,6,6,10,5,5,30,25,30,30,30,30,30,
            50,14,10,20,10,30,30,30,30,30,30,8,8,18,8,10,18,45,20,20,
            10,30,30,30,30,30,25,25,0,10,0,0,0,0,0,0 };
        int lastlv = 1;
        for (auto& f : _btl.foes) if (f.species) lastlv = f.level;
        int mul = _btl.foe_class >= 0 && _btl.foe_class < 105
            ? MUL[_btl.foe_class] : 4;
        uint32_t pay = (uint32_t)lastlv * 4 * mul;
        _vm.money += pay;
        if (verbose) printf("[battle] prime %u$\n", pay);
    }
    if (lost) {                                   // DS : soigné au Centre + amende
        for (auto& p : _vm.party) p.hp = -1;      // -1 -> _btl_start soigne
        // perte d'argent DS : maxLv*4 * {2,4,6,9,12,16,20,25,30}[badges]
        static const int PEN[9] = {2,4,6,9,12,16,20,25,30};
        int nb = 0; for (int i = 0; i < 8; i++) nb += (_vm.badges >> i) & 1;
        int maxlv = 1;
        for (auto& p : _vm.party) if (p.species) maxlv = std::max(maxlv, p.level);
        uint32_t pen = (uint32_t)maxlv * 4 * PEN[nb];
        _vm.money = _vm.money > pen ? _vm.money - pen : 0;
    }
    _vm.battle_lost = lost ? 1 : 0;
    // DS SCRIPT_MANAGER_BATTLE_RESULT : WIN=1 LOSE=2 CAPTURED=4
    // PLAYER_FLED=5 ENEMY_FLED=6 DRAW=3 (fuite joueur = won<=0)
    _vm.battle_result =
        _btl.caught                ? 4 :
        _btl.st == Battle::WIN     ? 1 :
        _btl.st == Battle::LOSE    ? 2 :
        _btl.st == Battle::FLEE    ? (_btl.won == 1 ? 6 : 5) : 0;
    // DS FieldTransition_StartMap : victoire/capture/fuite -> le champ se
    // reconstruit : OnLoad puis OnResume se rejouent avant la reprise du
    // script suspendu (FLAG_MAP_LOCAL_REMOVE_OBJECT nettoie les statiques).
    if (!lost && _vm.battle_result != 3 && !_fldret) {
        _fldret_susp = _vm.state() == ScriptVM::BATTLE;
        if (_fldret_susp) _fldret_ctx = _vm.save_ctx();
        _fldret = true; _fldret_q = {4, 3};
        if (!_fldret_next() || !_vm.busy()) _script_end_box();
    } else if (_vm.state() == ScriptVM::BATTLE) {   // reprendre le script dresseur
        _vm.continue_after(ScriptVM::BATTLE); _vm.step();
        // le script dresseur déclenche lui-même BlackOutFromBattle si perdu
    } else if (lost)
        _lost_pc = true;                          // sauvage : blackout direct DS
    if (verbose) printf("[battle] fin won=%d\n", _btl.won);
}

// (pokegra_decrypt a déménagé dans world_render.cpp, utilisé par _poke_tex)
void World::tv_advance() {                          // E pendant la scène TV
    if (!_tv_suppress) return;
    if (_vm.state() == ScriptVM::MSG) {             // pagination comme la boîte
        if (_dlg_chars < (float)dlg_page_chars())
            _dlg_chars = (float)dlg_page_chars();   // révèle la page
        else {
            size_t pe = dlg_page_end();
            if (pe < _dialog.size()) { _dlg_page = pe; _dlg_chars = 0.f; }
            else { _vm.continue_after(ScriptVM::MSG); _vm.step();
                   if (!_vm.busy()) _script_end_box(); }
        }
    } else if (_vm.state() == ScriptVM::WAITBTN) {  // attente d'appui -> suite
        _vm.continue_after(ScriptVM::WAITBTN); _vm.step();
        if (!_vm.busy()) _script_end_box();
    }
}
