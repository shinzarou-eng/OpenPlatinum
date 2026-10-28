// world.cpp — chargement map : init, headers, cellules, streaming, save/load.
#include "world.h"
Opts g_opts;                                // options globales (menu Options)
#include "world_internal.h"
#include "msg.h"
#include "spr.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>

// ── init / chargement ─────────────────────────────────────────────────────────
bool World::init(Nds* db) {
    _db = db;
    _pbox = _box(5.f, 8.f, 5.f, {0.9f, 0.3f, 0.35f, 1});
    _npcbox = _box(4.f, 7.f, 4.f, {0.3f, 0.6f, 1.0f, 1});
    _npcbox2 = _box(4.f, 7.f, 4.f, {1.0f, 0.5f, 0.2f, 1});
    // quad billboard partagé (20×28, ancré aux pieds) pour les persos overworld
    float q[] = {
        -10,0,0,  0,0,1,  1,1,1,1,  0,1,
         10,0,0,  0,0,1,  1,1,1,1,  1,1,
        -10,28,0, 0,0,1,  1,1,1,1,  0,0,
         10,0,0,  0,0,1,  1,1,1,1,  1,1,
         10,28,0, 0,0,1,  1,1,1,1,  1,0,
        -10,28,0, 0,0,1,  1,1,1,1,  0,0,
    };
    _bb = rnd::upload(q, 6, 0);
    _bb.alpha_cut = true; _bb.lit = 1;          // DS : billboards = lampe 0 seule
    // bulle "!" DS (EmoteExclamationMark) : rond blanc + point d'exclamation
    {   Image im; im.create(12, 12);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 12; x++) {
                float dx = x - 5.5f, dy = y - 5.5f, r2 = dx*dx + dy*dy;
                if (r2 > 30.f) continue;                    // cercle ~5.5px
                bool edge = r2 > 20.f;
                im.set(x, y, edge ? 40 : 255, edge ? 40 : 255,
                       edge ? 60 : 255, 255);
            }
        for (int y = 2; y < 7; y++)                         // barre du "!"
            for (int x = 5; x < 7; x++) im.set(x, y, 230, 70, 40, 255);
        for (int y = 8; y < 10; y++)                        // point du "!"
            for (int x = 5; x < 7; x++) im.set(x, y, 230, 70, 40, 255);
        _emote_tex = rnd::upload_tex(im);
        float eq[] = {                                      // quad 10×10
            -5,0,0, 0,0,1, 1,1,1,1, 0,1,   5,0,0, 0,0,1, 1,1,1,1, 1,1,
            -5,10,0, 0,0,1, 1,1,1,1, 0,0,  5,0,0, 0,0,1, 1,1,1,1, 1,1,
             5,10,0, 0,0,1, 1,1,1,1, 1,0, -5,10,0, 0,0,1, 1,1,1,1, 0,0,
        };
        _emote_bb = rnd::upload(eq, 6, _emote_tex);
        _emote_bb.alpha_cut = true; _emote_bb.lit = 0;   // "!" : aucune lampe
        if (verbose) printf("[init] emote tex=%u vao=%u\n", _emote_tex, _emote_bb.vao);
    }
    {   // quad sprite sauvage 40×40 (pl_pokegra 80×80 frame, carré), ancré bas-centre
        float pq[] = {
            -20,0,0, 0,0,1, 1,1,1,1, 0,1,   20,0,0, 0,0,1, 1,1,1,1, 1,1,
            -20,40,0,0,0,1, 1,1,1,1, 0,0,   20,0,0, 0,0,1, 1,1,1,1, 1,1,
             20,40,0, 0,0,1, 1,1,1,1, 1,0, -20,40,0,0,0,1, 1,1,1,1, 0,0,
        };
        _pkbb = rnd::upload(pq, 6, 0);
        _pkbb.alpha_cut = true; _pkbb.lit = 1;
    }
    // scripts_init_new_game (scriptID 9600 -> scr_seq[412]) : pose tous les
    // FLAG_HIDE_*/vars de départ — le vrai point d'entrée d'une nouvelle partie
    _scrnarc = _db->narc(Nds::P_SCR);
    if (_scrnarc) {
        _start_script(9600);
        _vm.step();
        _dialog.clear(); _mbox = false; _yesno = false;
        printf("[init] new-game flags : rival=%d mom=%d guit=%x\n",
            _vm.flag(372), _vm.flag(497), _vm.var(0x4070));
    }
    return true;
}

void World::_unload_all() {
    for (auto& kv : _cells)
        for (auto& g : kv.second->meshes) rnd::free_mesh(g);
    for (auto& p : _props)
        for (auto& g : p.meshes) rnd::free_mesh(g);
    _cells.clear(); _props.clear();
    _nsurf = _nverts = 0;
}

// bascule de zone : recharge les événements/objets/scripts du header
// (DS : transition de zone quand le joueur entre dans une cellule matrice
//  d'un autre header — pas de rechargement géométrie, position conservée)
void World::_set_header(int header_id, bool init) {
    const MapHeader* H = _db->header(header_id);
    if (!H) return;
    _header = header_id;
    auto ev = _db->zevents(H->evt);
    _warps = ev.warps;
    if (verbose)
        for (auto& w : _warps)
            printf("    [warp] (%d,%d) -> hdr%d dwarp%d\n", w.x, w.z, w.dest, w.dwarp);
    _objects = ev.objects;
    _bgs = ev.bgs;
    _coords = ev.coords;
    if (verbose) {
        for (auto& c : _coords)
            printf("    [coord] (%d,%d %dx%d) scr=%d var=%x==%d\n",
                c.x, c.z, c.w, c.l, c.script, c.var, c.val);
        for (auto& b : _bgs)
            printf("    [bg] (%d,%d) scr=%d type=%d dir=%d\n",
                b.x, b.z, b.script, b.type, b.dir);
    }

    // état runtime des PNJ (position monde + direction)
    _objpos.resize(_objects.size());
    _objd.resize(_objects.size());
    _objmv.resize(_objects.size());
    _objw.resize(_objects.size());
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        _objpos[i] = {o.x * TILE + TILE / 2.f, 0.f, o.z * TILE + TILE / 2.f};
        // DS map_object.c:709 : pos.y = event.y (fx32) puis RecalculateObjectHeight
        // -> plaque BDHC la plus proche de event.y (référence = event.y,
        // pas +200 : un PNJ sous un pont reste au sol, pas sur le pont)
        // Échec -> -1e9 : la cellule n'est pas encore chargée, le snap
        // paresseux de update() reprendra la pose (objets statiques
        // compris : boîtes aux lettres, panneaux).
        float g0 = _ground_y(_objpos[i], o.y / 4096.f);
        _objpos[i].y = (g0 > -1e8f) ? g0 : -1e9f;
        static const int M[4] = {1, 0, 2, 3};       // dir ROM (0=N) -> monde (0=S)
        _objd[i] = M[o.dir & 3];
        _objmv[i] = o.move;                          // SwitchMovementType : vivant
        _objw[i].home = _objpos[i];
        if (verbose)
            printf("    [obj %d] id=%d gfx=%d mv=%d dir=%d pos=(%d,%d) r=%d,%d scr=%d flag=%d world=(%.0f,%.0f,%.0f)\n",
                (int)i, o.id, o.gfx, o.move, o.dir, o.x, o.z, o.rx, o.rz,
                o.script, o.flag, _objpos[i].x, _objpos[i].y, _objpos[i].z);
    }
    _moves.clear(); _objgone.clear(); _objhide.clear(); _objlock.clear();
    _follower = -1; _ftrail.clear(); _fstep = 0;
    _lockall = 0; _coord_in = -1; _talk_obj = -1;
    _pstep = 0; _ptx = _ppos.x; _ptz = _ppos.z;         // pas en cours -> annulé
    // visibilité DS : hiddenFlag SET au chargement -> objet non spawné
    // (map_object.c l.617 : les objets sans script spawnett toujours)
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        if (o.script != 0xFFFF && o.flag && _vm.flag(o.flag)) _objgone.insert((int)i);
    }
    for (auto& b : _buf) b.clear();
    _dialog.clear(); _mbox = false; _yesno = false; _warp_req = false;

    // fichier script + banque texte de la map
    _scrnarc = _db->narc(Nds::P_SCR);
    if (_scrnarc && H->scr < _scrnarc->count())
        _vm.load(&_scrnarc->member(H->scr));
    else
        _vm.load(nullptr);
    if (_scrnarc && _scrnarc->count() > 211)
        _vm.load_std(&_scrnarc->member(211));   // scripts_common (CallStandard)
    _initb = (_scrnarc && H->init < _scrnarc->count())
        ? &_scrnarc->member(H->init) : nullptr;
    _cur_bank = H->msg;
    // WildEncounters (pl_enc_data.narc, membre H->wild) : grass rate + slots
    _encnarc = _db->narc(Nds::P_ENC);
    _enc = (_encnarc && H->wild < _encnarc->count())
        ? &_encnarc->member(H->wild) : nullptr;
    _enc_rate = (_enc && _enc->size() >= 4) ? (int)u32(*_enc, 0) : 0;
    _enc_steps = 0;          // DS : encounterAttempts remis à 0 à chaque transition
    _vm.cb_msg = [this](int id, bool instant) {
        auto& bank = _bank_msgs(_cur_bank);
        _dialog = (id >= 0 && id < (int)bank.size())
            ? bank[id] : ("<msg " + std::to_string(id) + ">");
        _dialog = _fmt_msg(_dialog, &_dlg_cols);
        _dlg_page = 0;
        _dlg_chars = instant ? (float)_dialog.size() : 0.f;
        _mbox = true;                               // fenêtre ouverte (DS)
    };
    _vm.cb_box = [this](bool open) {                // OpenMessage/CloseMessage
        _mbox = open;
        if (!open) { _dialog.clear(); _dlg_chars = 0; }
    };
    // buffers StringTemplate : (slot, kind, val) — noms via banques pl_msg
    _vm.cb_buf = [this](int slot, int kind, int val) {
        if (slot < 0 || slot > 15) return;
        static const int BANKS[] = {0,0,0,0,0,0,0,0,0,0,
            392, 393, 0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,
            412,0,0,0,0,0,0,0,0,0,
            647,0,0,0,0,0,0,0,0,0,
            395,0,0,0,0,0,0,0,0,0,
            457,0,0,0,0,0,0,0,0,0,
            626, 630, 628 };
        if (kind == 41) {                            // TM/HM -> nom de capacité
            _buf[slot] = (val >= 328 && val <= 419)
                ? "CT" + std::to_string(val - 327)
                : (val >= 420 && val <= 427)
                    ? "CS" + std::to_string(val - 419) : "?";
            return;
        }
        switch (kind) {
        case 0: _buf[slot] = player_name(); break;          // nom joueur
        case 1: _buf[slot] = rival_name(); break;           // rival
        case 2: _buf[slot] = cpart_name(); break;           // counterpart
        case 20: _buf[slot] = std::to_string(val); break;   // nombre
        case 80: {                                   // nom de map (header -> label)
            auto h = _db->header(val);
            _buf[slot] = h ? _db->loc_name(h->label) : "?"; break;
        }
        default:
            if (kind >= 10 && kind <= 72 && BANKS[kind]) {
                auto& b = _bank_msgs(BANKS[kind]);
                _buf[slot] = (val >= 0 && val < (int)b.size())
                    ? _fmt_msg(b[val]) : "?";
            } else _buf[slot] = "?";
            break;
        }
    };
    _vm.cb_move = [this](int obj, size_t off) { _queue_move(obj, off); };
    _vm.cb_warp = [this](int hdr, int x, int z, int dir) {
        _pw = {hdr, x, z, dir}; _warp_req = true;
    };
    _vm.cb_face = [this]() {                        // FacePlayer : PNJ -> joueur
        if (_talk_obj < 0 || _talk_obj >= (int)_objects.size()) return;
        int fx = (int)floorf(_ppos.x / TILE) - (int)floorf(_objpos[_talk_obj].x / TILE);
        int fz = (int)floorf(_ppos.z / TILE) - (int)floorf(_objpos[_talk_obj].z / TILE);
        _objd[_talk_obj] = abs(fz) >= abs(fx) ? (fz > 0 ? 0 : 1) : (fx < 0 ? 2 : 3);
    };
    _vm.cb_people = [this](int id, bool show) {           // AddObject/RemoveObject
        if (!show) {                                    // DS : LocalMapObjByIndex ->
            int i = _live_obj(id);                      // 1er vivant = localID,
            if (i < 0) return;                          // SetFlagAndDeleteObject
            _objgone.insert(i);                         // + hiddenFlag SET
            if (_objects[i].flag) _vm.set_flag(_objects[i].flag, true);
            return;
        }
        // AddObject : sub_020631A4 -> 1er event SCRIPTABLE (script!=0xFFFF) de
        // ce localID, puis respawn depuis l'ObjectEvent
        for (size_t i = 0; i < _objects.size(); i++) {
            auto& o = _objects[i];
            if (o.id != id || o.script == 0xFFFF) continue;
            if (o.flag && _vm.flag(o.flag)) return;   // hiddenFlag posé -> rien
            obj_respawn((int)i);
            return;
        }
    };
    _vm.cb_unimpl = [this](const char* n, int op) {
        if (_unimpl.insert(op).second) {            // log une fois par opcode
            _unimpl_n++;
            if (verbose)
                printf("[scr] unimpl 0x%04X %s (%d distincts)\n",
                    op, n, _unimpl_n);
        }
    };
    _vm.cb_fade = [this](int trans, int frames, int type, int color) {
        _fade.color = color; _fade.t = 0;
        _fade.dur = (frames > 0 ? frames : 1) / 60.f;   // frames DS @60Hz
        _fade.out = (trans != 0);            // transition!=0 -> vers la couleur
        if (verbose) printf("[fade] trans=%d f=%d type=%d col=%d\n",
            trans, frames, type, color);
    };
    _vm.cb_pos = [this](int axis) {          // GetPlayerMapPos : tuiles monde
        return axis == 0 ? (int)floorf(_ppos.x / TILE)
                         : (int)floorf(_ppos.z / TILE);
    };
    _vm.cb_pdir = [this]() {                 // GetPlayerDir : dir ROM (0=N)
        static const int W2R[4] = {1, 0, 2, 3};
        return W2R[_pdir & 3];
    };
    _vm.cb_surf = [this]() {                 // UseSurf (field_moves fn4) :
        _psurf = true;                       // le joueur saute sur la tuile
        int ddx, ddz; dirv(_pdir, ddx, ddz); // d'eau en face (montée CS)
        _ptx = _ppos.x + ddx * TILE; _ptz = _ppos.z + ddz * TILE;
        _pstep = 2; _pstep_spd = 120.f; _pjump = 1;   // petit saut d'eau DS
        _warp_cd_x = _warp_cd_z = -9999;              // rencontre possible
    };
    _vm.cb_findmove = [this](int mv) {       // FindPartySlotWithMove :
        for (int i = 0; i < (int)_vm.party.size(); i++) { // slot du 1er Pokémon
            auto bm = battle_make(_db, _vm.party[i].species,
                _vm.party[i].level, nullptr, _vm.party[i].iv);
            for (int k = 0; k < bm.nmoves; k++)
                if (bm.moves[k] == mv) return i;          // connaît (learnset)
        }
        // sinon : le Pokémon peut apprendre la CS correspondante (compat CS DS)
        // CS01 Coupe=15 CS02 Vol=19 CS03 Surf=57 CS04 Force=70
        // CS05 Anti-Brume=432 CS06 Éclate-Roc=249 CS07 Cascade=127 CS08 Escalade=431
        static const int HMOVE[8] = {15, 19, 57, 70, 432, 249, 127, 431};
        for (int i = 0; i < (int)_vm.party.size(); i++)
            for (int h = 0; h < 8; h++)
                if (HMOVE[h] == mv && _mon_learns_hm(_vm.party[i], h + 1))
                    return i;
        return 0xFF;
    };
    _vm.cb_evpos = [this](int obj, int x, int z) {      // SetObjectEventPos :
        for (auto& o : _objects)                        // event template seul
            if (o.id == obj) { o.x = x; o.z = z; return; }
    };
    _vm.cb_warpev = [this](int idx, int x, int z) {     // SetWarpEventPos :
        if (idx >= 0 && idx < (int)_warps.size()) {     // event template seul
            _warps[idx].x = x; _warps[idx].z = z;
        }
    };
    _vm.cb_bgev = [this](int idx, int x, int z) {       // SetBgEventPos :
        if (idx >= 0 && idx < (int)_bgs.size()) {       // event template seul
            _bgs[idx].x = x; _bgs[idx].z = z;
        }
    };
    _vm.cb_setpos = [this](int obj, int x, int z, int dir, int y) {
        Vec3* p = nullptr; int* d = nullptr;
        if (obj == 0xFF) { p = &_ppos; d = &_pdir; }
        else { int i = _live_obj(obj);              // SetPosition : objet vivant
            if (i >= 0) { p = &_objpos[i]; d = &_objd[i]; } }
        if (!p) return;
        // SetPosDirFromCoords : pos = tuile*16+8, y en unités de 8 px,
        // puis RecalculateObjectHeight -> snap sur la hauteur BDHC
        p->x = x * TILE + TILE / 2; p->z = z * TILE + TILE / 2;
        if (y >= 0) p->y = y * 8.f;
        float g = _ground_y(*p, p->y);       // référence = hauteur posée (DS)
        if (g > -1e8f) p->y = g;
        static const int M[4] = {1, 0, 2, 3};   // dir ROM -> monde
        if (dir >= 0 && d) *d = M[dir & 3];
        if (obj == 0xFF) { _ptx = x; _ptz = z; }        // coords tuile joueur
    };
    _vm.cb_ow = [this](int obj, int kind, int val) {
        // DS : SetMovementType touche l'objet VIVANT (1er spawné = localID) ;
        // SetObjectEvent* n'écrivent QUE l'event template (1er localID, prochain
        // AddObject) — map_header_data.c n'exclut pas les events sans script.
        if (kind == 2) {
            int i = _live_obj(obj);
            if (i >= 0) _objmv[i] = val;
            return;
        }
        for (size_t i = 0; i < _objects.size(); i++) {
            if (_objects[i].id != obj) continue;
            if (kind == 0) _objects[i].move = val;       // SetObjectEventMovementType
            else _objects[i].dir = val;                  // SetObjectEventDir (dir ROM)
            return;
        }
    };
    _vm.cb_misc = [this](int what) {
        if (what == 0) { _can_run = true;                 // GiveRunningShoes
            if (verbose) printf("[evt] chaussures de sport !\n"); }
        else if (what == 1) _lockall++;                   // LockAll : gel PNJ
        else if (what == 2) _lockall = 0;                 // ReleaseAll
    };
    _vm.cb_lockobj = [this](int id, bool lock) {          // Lock/Release : vivant
        int i = _live_obj(id);
        if (i < 0) return;
        if (lock) _objlock.insert(i); else _objlock.erase(i);
    };
    _vm.cb_follow = [this](int cmd, int a, int b) {       // follow rival/cie
        (void)b;
        switch (cmd) {
        case 0: case 1: case 2:                           // ContinueFollow/FollowHero/SetFollowingOverworld
            _follower = _live_obj(a); _ftrail.clear(); _fstep = 0;
            if (verbose) printf("[follow] obj %d -> i%d\n", a, _follower);
            break;
        case 3: _vm.set_var(a, _follower >= 0 ? 1 : 0); break; // CheckFollow
        case 4: break;                                             // StartFollow
        case 5: _follower = -1; _ftrail.clear(); break;            // StopFollow
        }
    };
    _vm.cb_runstate = [this]() -> int { return _can_run ? 1 : 0; };
    _vm.cb_mapid = [this]() -> int { return _header; };
    // commandes de porte (DoorAnimation_* de la décomp) : le tag pointe vers
    // le prop trouvé par la hitbox script (-1,0,3,1 sur la tuile cible)
    _vm.cb_doorload = [this](int mx, int mz, int tx, int tz, int tag) {
        int wx = mx * 32 + tx, wz = mz * 32 + tz;    // tuiles monde (32/map)
        _doortags[tag] = _find_door_prop(wx - 1, wz - 1, wx + 1, wz);
        if (verbose) printf("[door] load tag=%d @(%d,%d) -> prop %d\n",
            tag, wx, wz, _doortags[tag]);
    };
    _vm.cb_dooranim = [this](int tag, bool open) {
        auto it = _doortags.find(tag);
        if (it == _doortags.end() || it->second < 0) return;
        _prop_play(it->second, open ? 0 : 1);   // DS : open=0, close=1 (2/4)
    };
    _vm.cb_doorbusy = [this](int tag) -> int {
        auto it = _doortags.find(tag);
        if (it == _doortags.end() || it->second < 0 ||
            it->second >= (int)_props.size()) return 0;
        return _prop_anim_done(_props[it->second]) ? 0 : 1;
    };
    _vm.cb_doorfree = [this](int tag) {
        auto it = _doortags.find(tag);
        if (it != _doortags.end()) {
            if (it->second >= 0 && it->second < (int)_props.size())
                _prop_anim_stop(_props[it->second]);
            _doortags.erase(it);
        }
    };
    _vm.cb_lostpc = [this]() { _lost_pc = true; };        // LostGoPc -> maison
    _vm.cb_heal = [this]() {                              // HealParty (infirmière)
        for (auto& p : _vm.party) {
            auto bm = battle_make(_db, p.species, p.level, nullptr, p.iv);
            p.iv = bm.iv; p.hp = bm.maxhp;
        } };
    _vm.cb_setbo = [this](int id) {                       // SetBlackOutWarpId
        if (id >= 1 && id <= SPAWN_TBL_N) _blackout_id = id; };
    _vm.cb_pic = [this](int sp, bool on) { _pic_species = on ? sp : 0; };
    // PrintTrainerDialogue : trtblofs -> trtbl -> pl_msg[617] (TEXT_BANK_NPC_TRAINER_MESSAGES)
    _vm.cb_trainermsg = [this](int tid, int mtype) {
        auto ofs = _db->narc(Nds::P_TRTBLOF), tbl = _db->narc(Nds::P_TRTBL);
        _dialog.clear(); _dlg_chars = 0;
        if (!ofs || !tbl || !ofs->count() || !tbl->count()) return;
        _mbox = true;                           // fenêtre message ouverte
        const Bytes& o = ofs->member(0), & t = tbl->member(0);
        if (tid * 2 + 1 >= (int)o.size()) return;
        int off = u16(o, tid * 2);
        auto& bank = _bank_msgs(617);
        while (off + 4 <= (int)t.size()) {
            int et = u16(t, off), etype = u16(t, off + 2);
            if (et == tid && etype == mtype) {
                int mi = off / 4;
                _dialog = _fmt_msg(mi < (int)bank.size() ? bank[mi] : "?",
                    &_dlg_cols);
                _dlg_page = 0; _dlg_chars = 0;
                break;
            }
            if (et != tid) break;
            off += 4;
        }
    };
    _vm.cb_trdata = [this](int tid) -> int {            // TRDATA_BATTLE_TYPE
        auto td = _db->narc(Nds::P_TRDATA);
        if (!td || tid < 0 || tid >= td->count()) return 0;
        const Bytes& m = td->member(tid);
        return m.size() >= 20 ? (int)u32(m, 16) : 0;    // TrainerHeader.battleType
    };
    _vm.cb_objmt = [this](int ref) -> int {             // GetMovementType : vivant
        if (ref >= 0 && ref < (int)_objmv.size()) return _objmv[ref];
        for (size_t i = 0; i < _objects.size(); i++)
            if (_objects[i].id == ref) return _objmv[i];
        return 0;
    };
    _vm.cb_targetflag = [this](int set) -> int {        // flag dresseur : trainerID cible
        if (_talk_obj < 0 || _talk_obj >= (int)_objects.size()) return 0;
        int s = _objects[_talk_obj].script;
        if (s < 3000) return 0;
        int tid = (s >= 5000) ? s - 4999 : s - 2999;   // Script_GetTrainerID DS
        int f = 1364 + tid;                            // TRAINER_DEFEATED_FLAGS_START
        if (set) { _vm.set_flag(f, true); return 1; }
        return _vm.flag(f) ? 1 : 0;
    };
    _label = _db->loc_name(H->label);
    if (getenv("WARPDBG")) fprintf(stderr, "[label] hdr=%d label=%d -> '%s'\n",
        header_id, H->label, _label.c_str());
    _mapname = _db->header_name(header_id);
    // DS map_name_popup : windowID==0 ou IsBuilding (INDOORS/POKECENTER) -> pas de popup
    _banner_t = (H->win == 0 || H->mtype == 4 || H->mtype == 5) ? 0.f : 1.5f;
    if (init) _run_init();
}

// scripts d'init : ON_LOAD(4) puis ON_TRANSITION(2) puis ON_RESUME(3)
void World::_run_init() {
    for (int t : {4, 2, 3}) {
        int id = _init_script(t);
        if (id >= 0) {
            if (verbose) printf("[init] type%d -> script %d\n", t, id);
            _start_script(id); _vm.step();
        }
    }
}

void World::load_map(int header_id, int dest_warp) {
    if (_busy) return;
    if (_wt.st != WP_LOAD) {               // appel direct (debug/script) :
        _wt.st = -1;                       // annule une transition en cours
        _phidden = false;
    }
    _busy = true;
    const MapHeader* H = _db->header(header_id);
    if (!H) { _busy = false; return; }
    _mtx = _db->matrix(H->mtx);
    _set_header(header_id, false);          // events + scripts + PNJ
    // field_map_change.c : un header de soin mémorise le point de blackout
    if (int bid = spawn_blackout_id(header_id)) _blackout_id = bid;

    _unload_all();
    _texset.clear();
    _texcache.clear();
    _lands.clear(); _bmds.clear();

    // cellules du header (overworld) ou toutes (intérieur sans section headers)
    bool has_hdr = !_mtx.headers.empty();
    std::unordered_map<int64_t, bool> keep;
    for (int i = 0; i < (int)_mtx.land.size(); i++) {
        bool want = has_hdr ? _mtx.headers[i] == header_id : true;
        if (!want) continue;
        int cx = i % _mtx.w, cy = i / _mtx.w;
        for (int dx = -LOAD_R; dx <= LOAD_R; dx++)
            for (int dz = -LOAD_R; dz <= LOAD_R; dz++) {
                int nx = cx + dx, ny = cy + dz;
                if (nx >= 0 && ny >= 0 && nx < _mtx.w && ny < _mtx.h)
                    keep[ckey(nx, ny)] = true;
            }
    }

    auto ar = _db->area(H->area);
    auto tsn = _db->narc(Nds::P_TEXSET);
    if (tsn) {
        const Bytes& td = tsn->member(ar.tex);
        _texset = bmd::parse_tex0(td, 0);
    }
    if (!getenv("NOALT")) _alt_load(ar.light);

    for (auto& kv : keep) {
        int cx = (int)(kv.first >> 32), cy = (int)(uint32_t)kv.first;
        _load_cell(cx, cy);
    }

    // props -> instances monde
    for (auto& kv : _cells) _inst_props(kv.second.get());

    _finish_load(header_id, dest_warp);
}

// instancie les props (bâtiments etc.) d'une cellule — appelé au chargement
// et quand _stream() ajoute une cellule en cours de jeu
void World::_inst_props(Cell* cp) {
    Cell& c = *cp;
    {
        if (!c.land) return;
        for (auto& lp : c.land->props) {
            auto it = _bmds.find(0x100000 + (int)lp.model);
            if (it == _bmds.end()) {
                auto bn = _db->narc(Nds::P_BMODEL);
                std::shared_ptr<bmd::Model> m;
                if (bn) {
                    auto md = std::make_shared<bmd::Model>(
                        bmd::parse_bmd(bn->member((int)lp.model)));
                    m = md;
                }
                it = _bmds.emplace(0x100000 + (int)lp.model, m).first;
            }
            auto& model = it->second;
            if (!model) continue;
            PropInst pi;
            pi.cell = ckey(c.cx, c.cy);         // pour déchargement ciblé
            pi.pos = c.org + lp.pos;
            pi.model = (int)lp.model;
            pi.mdl = model.get();
            uint16_t ry = (uint16_t)((int)lp.rot.y & 0xFFFF);
            pi.ry = (float)ry / 65536.f * 6.28318530718f;
            pi.scale = lp.scale.len() > 0 ? lp.scale : Vec3{1,1,1};
            pi.org = pi.pos;
            auto& texs = model->textures.empty() ? _texset : model->textures;
            if (verbose) printf("    [prop %d] mats=%d pos=%.0f,%.0f,%.0f\n", (int)lp.model,
                (int)model->meshes.size(), pi.pos.x, pi.pos.y, pi.pos.z);
            for (auto& mg : model->meshes) {
                std::vector<Vec3> ct;
                Vec3 o{0,0,0};
                auto buf = _tris(mg, o, &ct);
                if (verbose && !ct.empty()) {
                    Vec3 mn = ct[0], mx = ct[0];
                    for (auto& v : ct) {
                        mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
                        mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
                    }
                    printf("      [pmesh] mat=%s tex=%s pal=%s poly=%08x verts=%d x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]\n",
                        mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                        mg.matdef.pal_name.c_str(), mg.matdef.poly,
                        (int)buf.size() / rnd::VF,
                        mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
                }
                const char* hide = getenv("HIDE");
                bool hidden = mg.matdef.name.find("imped") != std::string::npos
                    || (hide && *hide && mg.matdef.name.find(hide) != std::string::npos);
                bool alpha = false; int polyA = 31;
                uint32_t t = hidden ? 0
                    : _mat_tex(mg.matdef, texs, &alpha, &polyA);
                GpuMesh g = hidden ? GpuMesh{}
                    : rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
                g.dbg_name = mg.matdef.name;
                g.alpha_cut = alpha;
                g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                if (!t && (mg.matdef.dif & 0x8000))
                    g.tint = rgb555f(mg.matdef.dif & 0x7FFF);
                if (flat) {
                    int id = _flat_id++;
                    float r = ((id * 73) % 255) / 255.f,
                        gg = ((id * 151) % 255) / 255.f,
                        bb = ((id * 199) % 255) / 255.f;
                    g.tint = {r * .85f + .15f, gg * .85f + .15f,
                        bb * .85f + .15f, 1};
                    g.tex = 0; g.alpha_cut = false; g.blend = 1.f;
                    printf("      [flat] mesh%d prop%d cell(%d,%d) mat=%s -> %.2f %.2f %.2f\n",
                        id, (int)_props.size(), -1, -1,
                        mg.matdef.name.c_str(), r, gg, bb);
                }
                pi.meshes.push_back(g);
                // nœud du group pour les anims (portes) : delta = anim * repos⁻¹
                pi.mkey.push_back(bmd::group_key(mg.mat, mg.node));
                pi.minv.push_back(mg.xf.inverse());
                // collision props : transformés par pos/rot/scale — dans
                // prop_tris (bloque le déplacement) PAS col_tris : la DS
                // n'utilise que les plaques BDHC pour la hauteur d'objet
                float cs = cosf(pi.ry), sn = sinf(pi.ry);
                for (size_t i = 0; i < ct.size(); i++) {
                    Vec3 v{ct[i].x * pi.scale.x, ct[i].y * pi.scale.y,
                        ct[i].z * pi.scale.z};
                    Vec3 r{v.x * cs + v.z * sn, v.y, -v.x * sn + v.z * cs};
                    c.prop_tris.push_back(r + pi.pos);
                }
            }
            _props.push_back(std::move(pi));
        }
    }
}

// fin de load_map : spawn joueur + bannière + logs + scripts d'init
void World::_finish_load(int header_id, int dest_warp) {
    bool has_hdr = !_mtx.headers.empty();

    // spawn
    Vec3 spawn{0, 40, 0};
    bool found = false;
    if (dest_warp >= 0 && dest_warp < (int)_warps.size()) {
        auto& w2 = _warps[dest_warp];
        spawn = {w2.x * TILE + TILE / 2, 40.f, w2.z * TILE + TILE / 2};
        found = true;
    } else if (!has_hdr && !_warps.empty()) {
        // intérieur : porte de sortie = position valide
        auto& w2 = _warps[0];
        spawn = {w2.x * TILE + TILE / 2, 40.f, w2.z * TILE + TILE / 2};
        found = true;
    } else if (has_hdr && !_warps.empty()) {
        // extérieur sans destination : porte de la map = sol toujours valide
        auto& w2 = _warps[0];
        spawn = {w2.x * TILE + TILE / 2, 40.f, w2.z * TILE + TILE / 2};
        found = true;
    } else if (has_hdr && !_objects.empty()) {
        // repli : spawn près d'un PNJ, position validée — sol sous les pieds
        // et rayon caméra {0,95,40} dégagé (pas dans un bâtiment)
        Vec3 coff{0, 95.f, 95.f * 0.42f};
        float best = -1.f;
        for (auto& o : _objects)
            for (int dz : {1, -1, 0}) {
                Vec3 s{o.x * TILE + TILE / 2, 40.f,
                    (o.z + dz) * TILE + TILE / 2};
                float gy = _ground_y(s, 80.f);
                if (gy < -100.f || gy > 35.f) continue;   // vide ou sur un toit
                float t = _cam_hit(s + Vec3{0, 10.f, 0}, coff);
                if (t > best) { best = t; spawn = s; found = true; }
                if (t > 0.98f) break;
            }
        if (!found) {   // repli : 1er PNJ brut
            spawn = {_objects[0].x * TILE + TILE / 2, 40.f,
                _objects[0].z * TILE + TILE / 2};
            found = true;
        }
    } else {
        for (int i = 0; i < (int)_mtx.land.size() && !found; i++) {
            bool want = has_hdr ? _mtx.headers[i] == header_id : true;
            if (!want) continue;
            int cx = i % _mtx.w, cy = i / _mtx.w;
            spawn = {cx * CELL + CELL / 2, 40.f, cy * CELL + CELL / 2};
            found = true;
        }
    }
    _ppos = spawn; _vel = {0, 0, 0};
    // ne pas re-déclencher le warp sur lequel on vient de spawner
    _warp_cd_x = (int)floorf(spawn.x / TILE);
    _warp_cd_z = (int)floorf(spawn.z / TILE);
    printf("[world] hdr=%d %s cells=%d surf=%d verts=%d warps=%d objs=%d\n",
        header_id, _mapname.c_str(), (int)_cells.size(), _nsurf, _nverts,
        (int)_warps.size(), (int)_objects.size());
    if (getenv("WARPAUDIT")) {                  // audit : warps + comportements
        for (size_t i = 0; i < _warps.size(); i++) {
            auto& w = _warps[i];
            int cx = w.x / 32, cy = w.z / 32;   // tuile -> cellule 32x32
            if (!_cells.count(ckey(cx, cy))) _load_cell(cx, cy);
            printf("[audit] warp%zu (%d,%d)->hdr%d w%d beh=%02x"
                " N=%02x S=%02x O=%02x E=%02x blk=%d\n",
                i, w.x, w.z, w.dest, w.dwarp, _tile_beh(w.x, w.z),
                _tile_beh(w.x, w.z - 1), _tile_beh(w.x, w.z + 1),
                _tile_beh(w.x - 1, w.z), _tile_beh(w.x + 1, w.z),
                (int)_blocked({w.x * TILE + TILE / 2, _ppos.y + 4,
                    w.z * TILE + TILE / 2}));
            // attr complet des 4 voisins (bit15 = bloqué, bas = comportement)
            auto A = [this](int ax, int az) {
                auto ov = _tileover.find(((int64_t)ax << 32) | (uint32_t)az);
                const Cell* c = _cell_at(ax * TILE + TILE / 2.f,
                                         az * TILE + TILE / 2.f);
                return (ov != _tileover.end()) ? ov->second
                    : (c && c->land)
                        ? c->land->attr_at(ax - c->cx * 32, az - c->cy * 32)
                        : 0;
            };
            printf("[audit]   attr=%04x N=%04x S=%04x O=%04x E=%04x\n",
                A(w.x, w.z), A(w.x, w.z - 1), A(w.x, w.z + 1),
                A(w.x - 1, w.z), A(w.x + 1, w.z));
        }
    }
    if (verbose) for (auto& kv : _cells) {
        auto& c = kv.second;
        if (c->col_tris.empty()) { printf("  cell %d,%d vide\n", c->cx, c->cy); continue; }
        Vec3 mn = c->col_tris[0], mx = c->col_tris[0];
        for (auto& v : c->col_tris) {
            mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
            mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
        }
        printf("  cell %d,%d org=%.0f,%.0f,%.0f tris=%d y[%.0f..%.0f] x[%.0f..%.0f] z[%.0f..%.0f]\n",
            c->cx, c->cy, c->org.x, c->org.y, c->org.z, (int)c->col_tris.size() / 3,
            mn.y, mx.y, mn.x, mx.x, mn.z, mx.z);
    }
    _busy = false;
    _run_init();
}

// streaming overworld : charge les cellules manquantes autour du joueur
// (budget/frame, triées par distance), décharge les cellules hors rayon
// (hystérésis) et bascule le header quand on entre dans une autre zone.
// Monde ouvert : pas de temps de chargement, la géométrie suit le joueur.
void World::_unload_cell(int64_t k) {
    auto it = _cells.find(k);
    if (it == _cells.end()) return;
    for (auto& g : it->second->meshes) rnd::free_mesh(g);
    _nsurf -= (int)it->second->meshes.size();
    _nverts -= it->second->verts;
    _cells.erase(it);
    // props de cette cellule : libère meshes + retire les instances
    for (size_t i = _props.size(); i-- > 0;)
        if (_props[i].cell == k) {
            for (auto& g : _props[i].meshes) {
                _nverts -= g.nverts;
                rnd::free_mesh(g);
            }
            _props.erase(_props.begin() + i);
        }
}

void World::_stream() {
    if (_busy || _mtx.headers.empty()) return;   // intérieur : matrice sans hdr
    int pcx = (int)floorf(_ppos.x / CELL), pcy = (int)floorf(_ppos.z / CELL);
    // charge les manquantes : les plus proches d'abord, budget limité
    std::vector<std::pair<int, int64_t>> todo;
    for (int dz = -LOAD_R; dz <= LOAD_R; dz++)
        for (int dx = -LOAD_R; dx <= LOAD_R; dx++) {
            int nx = pcx + dx, ny = pcy + dz;
            if (nx < 0 || ny < 0 || nx >= _mtx.w || ny >= _mtx.h) continue;
            int64_t k = ckey(nx, ny);
            if (!_cells.count(k))
                todo.push_back({dx * dx + dz * dz, k});
        }
    std::sort(todo.begin(), todo.end());
    int budget = LOAD_BUDGET;
    for (auto& kv : todo) {
        if (budget-- <= 0) break;
        int nx = (int)(kv.second >> 32), ny = (int)(uint32_t)kv.second;
        _load_cell(nx, ny);
        auto it = _cells.find(kv.second);
        if (it != _cells.end()) _inst_props(it->second.get());
    }
    // décharge les cellules hors rayon (+1 d'hystérésis)
    for (auto it = _cells.begin(); it != _cells.end();) {
        int dx = std::abs(it->second->cx - pcx),
            dy = std::abs(it->second->cy - pcy);
        if (std::max(dx, dy) > UNLOAD_R) _unload_cell((it++)->first);
        else ++it;
    }
    static float celldbg = 0;
    if (getenv("CELLDBG") && (celldbg -= 1.f / 60) <= 0) {
        celldbg = 2.f;
        printf("[cells] %zu chargées, %zu props, cell joueur (%d,%d)\n",
            _cells.size(), _props.size(), pcx, pcy);
    }
    int i = pcy * _mtx.w + pcx;
    if (i < 0 || i >= (int)_mtx.headers.size()) return;
    int hdr = _mtx.headers[i];
    if (hdr <= 0 || hdr == _header) return;      // 0 = cellule vide : zone gardée
    auto NH = _db->header(hdr), CH = _db->header(_header);
    if (!NH) return;
    if (CH && NH->mtx != CH->mtx) {              // autre matrice : rechargement complet
        if (verbose) printf("[stream] hdr %d -> %d (matrice %d)\n", _header, hdr, NH->mtx);
        load_map(hdr, -1);
        return;
    }
    printf("[world] zone %d -> %d %s\n", _header, hdr, _db->header_name(hdr));
    _set_header(hdr, true);                      // events + scripts d'init de la zone
}

void World::_load_cell(int cx, int cy) {
    int i = cy * _mtx.w + cx;
    if (i < 0 || i >= (int)_mtx.land.size()) return;
    int land_id = _mtx.land[i];
    int alt = i < (int)_mtx.alt.size() ? (int8_t)_mtx.alt[i] : 0;

    auto c = std::make_shared<Cell>();
    // Godot : node.position = cellule centre — les verts BMD sont en ±256
    c->org = {cx * CELL + CELL / 2, -alt * 8.f, cy * CELL + CELL / 2};
    c->cx = cx; c->cy = cy;
    c->land_id = land_id;
    c->hdr = i < (int)_mtx.headers.size() ? _mtx.headers[i] : -1;

    auto lit = _lands.find(land_id);
    if (lit == _lands.end()) {
        auto ln = _db->narc(Nds::P_LAND);
        std::shared_ptr<Land> L;
        if (ln) L = std::make_shared<Land>(Land::parse(ln->member(land_id)));
        lit = _lands.emplace(land_id, L).first;
    }
    c->land = lit->second.get();

    auto bit = _bmds.find(land_id);
    if (bit == _bmds.end()) {
        std::shared_ptr<bmd::Model> m;
        if (c->land && !c->land->model.empty())
            m = std::make_shared<bmd::Model>(bmd::parse_bmd(c->land->model));
        bit = _bmds.emplace(land_id, m).first;
    }
    if (bit->second) {
        for (auto& mg : bit->second->meshes) {
            std::vector<Vec3> ct;
            auto buf = _tris(mg, c->org, &ct);
            if (verbose && !ct.empty()) {
                Vec3 mn = ct[0], mx = ct[0];
                for (auto& v : ct) {
                    mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
                    mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
                }
                printf("    [cell %d,%d land%d] mat=%s tex=%s poly=%08x verts=%d x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]\n",
                    cx, cy, land_id, mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                    mg.matdef.poly,
                    (int)buf.size() / rnd::VF, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
            }
            if (buf.empty()) continue;
            const char* hide = getenv("HIDE");
            bool hidden = mg.matdef.name.find("imped") != std::string::npos
                || (hide && *hide && mg.matdef.name.find(hide) != std::string::npos);
            bool alpha = false; int polyA = 31;
            uint32_t t = hidden ? 0
                : _mat_tex(mg.matdef, _texset, &alpha, &polyA);
            GpuMesh g = hidden ? GpuMesh{}
                : rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
            g.dbg_name = mg.matdef.name;
            g.alpha_cut = alpha;
            g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
            if (!t && (mg.matdef.dif & 0x8000))          // fallback dif (shade…)
                g.tint = rgb555f(mg.matdef.dif & 0x7FFF);
            if (flat) {
                int id = _flat_id++;
                float r = ((id * 73) % 255) / 255.f, gg = ((id * 151) % 255) / 255.f,
                    bb = ((id * 199) % 255) / 255.f;
                g.tint = {r * .85f + .15f, gg * .85f + .15f, bb * .85f + .15f, 1};
                g.tex = 0; g.alpha_cut = false; g.blend = 1.f;
                printf("      [flat] mesh%d cell(%d,%d) mat=%s -> %.2f %.2f %.2f\n",
                    id, cx, cy, mg.matdef.name.c_str(), r, gg, bb);
            }
            c->verts += (int)buf.size() / rnd::VF;
            c->meshes.push_back(g);
            _nsurf++;
            for (auto& v : ct) c->col_tris.push_back(v + c->org);
            static bool bdbg = getenv("BDHCDBG") != nullptr;
            static std::unordered_map<int64_t, bool> bseen;
            if (bdbg && !bseen.count(ckey(cx, cy))) {
                bseen[ckey(cx, cy)] = true;
                printf("  [bdhc] cell %d,%d bdhc_sz=%zu plates=%d\n",
                    cx, cy, c->land->bdhc.size(), (int)c->land->plates.size());
            }
        }
    }
    _cells[ckey(cx, cy)] = c;
}

// ── sauvegarde : map + position + vars + flags ────────────────────────────────
bool World::save(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t magic = 0x504E4156;                    // 'PNAV'
    fwrite(&magic, 4, 1, f);
    int32_t hdr = _header; fwrite(&hdr, 4, 1, f);
    fwrite(&_ppos.x, 4, 3, f);
    int32_t dir = _pdir; fwrite(&dir, 4, 1, f);
    uint32_t nv = (uint32_t)_vm.vars().size();
    fwrite(&nv, 4, 1, f);
    for (auto& kv : _vm.vars()) {
        int32_t k = kv.first, v = kv.second;
        fwrite(&k, 4, 1, f); fwrite(&v, 4, 1, f);
    }
    uint32_t nf = (uint32_t)_vm.flags().size();
    fwrite(&nf, 4, 1, f);
    for (auto& kv : _vm.flags()) {
        int32_t k = kv.first; uint8_t b = kv.second ? 1 : 0;
        fwrite(&k, 4, 1, f); fwrite(&b, 1, 1, f);
    }
    uint32_t nb = (uint32_t)_vm.bag().size();
    fwrite(&nb, 4, 1, f);
    for (auto& kv : _vm.bag()) {
        int32_t k = kv.first, v = kv.second;
        fwrite(&k, 4, 1, f); fwrite(&v, 4, 1, f);
    }
    uint8_t run = _can_run ? 1 : 0; fwrite(&run, 1, 1, f);   // chaussures de sport
    // équipe + starters (format v2, tolérant à l'absence en fin de fichier)
    uint8_t np = (uint8_t)_vm.party.size(); fwrite(&np, 1, 1, f);
    for (auto& m : _vm.party) {
        int16_t s = (int16_t)m.species, l = (int16_t)m.level;
        fwrite(&s, 2, 1, f); fwrite(&l, 2, 1, f);
    }
    int16_t st[3] = {(int16_t)_vm.chosen_starter,
        (int16_t)_vm.rival_starter, (int16_t)_vm.cpart_starter};
    fwrite(st, 2, 3, f);
    int16_t bo = (int16_t)_blackout_id; fwrite(&bo, 2, 1, f);  // point de respawn
    // format v3 : identité du joueur (genre + noms choisis à l'intro)
    uint8_t g = (uint8_t)_gender; fwrite(&g, 1, 1, f);
    uint8_t pl = (uint8_t)_pname.size(); fwrite(&pl, 1, 1, f);
    if (pl) fwrite(_pname.data(), 1, pl, f);
    uint8_t rl = (uint8_t)_rname.size(); fwrite(&rl, 1, 1, f);
    if (rl) fwrite(_rname.data(), 1, rl, f);
    // format v5 : objet enregistré (touche Y) + ordre d'obtention du sac
    int16_t ri = (int16_t)_reg_item; fwrite(&ri, 2, 1, f);
    uint16_t ns = (uint16_t)_bag_seq.size(); fwrite(&ns, 2, 1, f);
    for (int id : _bag_seq) { int16_t s = (int16_t)id; fwrite(&s, 2, 1, f); }
    // format v6 : IV (octet) + PV (2o) par membre d'équipe
    uint8_t nv6 = (uint8_t)_vm.party.size(); fwrite(&nv6, 1, 1, f);
    for (auto& m : _vm.party) {
        uint8_t iv = (uint8_t)std::max(0, m.iv);
        int16_t hp = (int16_t)m.hp;
        fwrite(&iv, 1, 1, f); fwrite(&hp, 2, 1, f);
    }
    uint8_t sf = _psurf ? 1 : 0; fwrite(&sf, 1, 1, f);   // avatar surf (v6b)
    // format v7 : progression par membre (PID, XP, EVs, amitié, moves)
    uint8_t nv7 = (uint8_t)_vm.party.size(); fwrite(&nv7, 1, 1, f);
    for (auto& m : _vm.party) {
        fwrite(&m.pid, 4, 1, f); fwrite(&m.exp, 4, 1, f);
        fwrite(m.evs, 1, 6, f);
        int16_t fr = (int16_t)m.friendship; fwrite(&fr, 2, 1, f);
        uint8_t nm = (uint8_t)m.nmoves; fwrite(&nm, 1, 1, f);
        for (int k = 0; k < 4; k++) {
            int16_t mv = (int16_t)m.moves[k], pp = (int16_t)m.pp[k];
            fwrite(&mv, 2, 1, f); fwrite(&pp, 2, 1, f);
        }
    }
    fclose(f);
    return true;
}

void World::set_identity(int g, const std::string& pn, const std::string& rn) {
    _gender = g; _pname = pn; _rname = rn;
    _buf[0] = pn; _buf[1] = rn;
    _buf[2] = cpart_name();
}

bool World::load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0; fread(&magic, 4, 1, f);
    if (magic != 0x504E4156) { fclose(f); return false; }
    int32_t hdr = 0; fread(&hdr, 4, 1, f);
    Vec3 p{0, 40, 0}; fread(&p.x, 4, 3, f);
    int32_t dir = 0; fread(&dir, 4, 1, f);
    uint32_t nv = 0; fread(&nv, 4, 1, f);
    std::unordered_map<int, int> vs;
    for (uint32_t i = 0; i < nv && i < 65536; i++) {
        int32_t k, v; fread(&k, 4, 1, f); fread(&v, 4, 1, f); vs[k] = v;
    }
    uint32_t nf = 0; fread(&nf, 4, 1, f);
    std::unordered_map<int, bool> fl;
    for (uint32_t i = 0; i < nf && i < 65536; i++) {
        int32_t k; uint8_t b = 0;
        fread(&k, 4, 1, f); fread(&b, 1, 1, f); fl[k] = b != 0;
    }
    std::unordered_map<int, int> bag;
    uint32_t nb = 0;
    if (fread(&nb, 4, 1, f) == 1)
        for (uint32_t i = 0; i < nb && i < 65536; i++) {
            int32_t k, v; fread(&k, 4, 1, f); fread(&v, 4, 1, f); bag[k] = v;
        }
    uint8_t run = 0;
    fread(&run, 1, 1, f);                              // chaussures de sport
    uint8_t np = 0;
    std::vector<ScriptVM::PartyMon> party;
    if (fread(&np, 1, 1, f) == 1)                      // équipe (format v2)
        for (int i = 0; i < np && i < 6; i++) {
            int16_t s = 0, l = 1;
            fread(&s, 2, 1, f); fread(&l, 2, 1, f);
            party.push_back({(int)s, (int)l, 10});
        }
    int16_t st[3] = {0, 0, 0}; fread(st, 2, 3, f);     // starters choisis
    int16_t bo = 1; fread(&bo, 2, 1, f);               // point de respawn (v3)
    {                                                     // identité (v4, optionnel)
        uint8_t g = 0, pl = 0, rl = 0;
        char pn[32] = {}, rn[32] = {};
        if (fread(&g, 1, 1, f) == 1 && fread(&pl, 1, 1, f) == 1) {
            if (pl && pl < sizeof(pn)) fread(pn, 1, pl, f);
            if (fread(&rl, 1, 1, f) == 1 && rl && rl < sizeof(rn)) fread(rn, 1, rl, f);
            _gender = g & 1; _pname = pn; _rname = rn;
            _buf[0] = _pname; _buf[1] = _rname; _buf[2] = cpart_name();
        }
    }
    {                                                     // v5 : objet enregistré + ordre sac
        int16_t ri = 0;
        if (fread(&ri, 2, 1, f) == 1) _reg_item = ri;
        uint16_t ns = 0;
        if (fread(&ns, 2, 1, f) == 1)
            for (int i = 0; i < ns && i < 4096; i++) {
                int16_t id = 0;
                if (fread(&id, 2, 1, f) != 1) break;
                _bag_seq.push_back((int)id);
            }
    }
    {                                                     // v6 : IV + PV par membre
        uint8_t nv6 = 0;
        if (fread(&nv6, 1, 1, f) == 1)
            for (int i = 0; i < nv6 && i < (int)party.size(); i++) {
                uint8_t iv = 0; int16_t hp = 0;
                if (fread(&iv, 1, 1, f) != 1 ||
                    fread(&hp, 2, 1, f) != 1) break;
                party[i].iv = iv; party[i].hp = hp;
            }
        uint8_t sf = 0;                                // avatar surf (v6b)
        if (fread(&sf, 1, 1, f) == 1) _psurf = sf != 0;
    }
    {                                                     // v7 : progression
        uint8_t nv7 = 0;
        if (fread(&nv7, 1, 1, f) == 1)
            for (int i = 0; i < nv7 && i < (int)party.size(); i++) {
                auto& m = party[i];
                int16_t fr = 70; uint8_t nm = 0;
                if (fread(&m.pid, 4, 1, f) != 1 ||
                    fread(&m.exp, 4, 1, f) != 1 ||
                    fread(m.evs, 1, 6, f) != 1 ||
                    fread(&fr, 2, 1, f) != 1 ||
                    fread(&nm, 1, 1, f) != 1) break;
                m.friendship = fr; m.nmoves = nm;
                for (int k = 0; k < 4; k++) {
                    int16_t mv = 0, pp = 0;
                    if (fread(&mv, 2, 1, f) != 1 || fread(&pp, 2, 1, f) != 1) break;
                    m.moves[k] = mv; m.pp[k] = pp;
                }
            }
    }
    fclose(f);
    if (bo < 1 || bo > SPAWN_TBL_N) bo = 1;
    _blackout_id = bo;
    load_map(hdr, -1);                                 // mémorise le soin si besoin
    _ppos = p; _pdir = dir & 3;
    _vm.load_state(vs, fl);
    _vm.bag_load(bag);
    _can_run = run != 0;
    _vm.party = party;
    _vm.chosen_starter = st[0]; _vm.rival_starter = st[1];
    _vm.cpart_starter = st[2];
    return true;
}
