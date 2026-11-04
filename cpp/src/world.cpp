// world.cpp — chargement map : init, headers, cellules, streaming, save/load.
#include "world.h"
Opts g_opts;                                // options globales (menu Options)
#include "world_internal.h"
#include "battle.h"
#include "msg.h"
#include "spr.h"
#include "sdat.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <functional>
#include <map>
namespace fs = std::filesystem;
#define STB_IMAGE_IMPLEMENTATION
#include "../thirdparty/stb/stb_image.h"

// un mesh `lamp*`/`o02b`/`light*` regroupe souvent tous les luminaires de la
// zone : clusterisé par connectivité — les 6 verts d'un quad partagent leurs
// positions, deux lampadaires distincts jamais (union-find eps ~1.5 unité)
static std::vector<Vec3> lamp_clusters(const float* buf, int nverts) {
    std::vector<Vec3> vs(nverts);
    std::vector<int> par(nverts);
    for (int i = 0; i < nverts; i++) {
        vs[i] = {buf[i * rnd::VF], buf[i * rnd::VF + 1], buf[i * rnd::VF + 2]};
        par[i] = i;
    }
    std::function<int(int)> root = [&](int i) {
        while (par[i] != i) { par[i] = par[par[i]]; i = par[i]; }
        return i;
    };
    // grille pour limiter le O(n²) : voisins dans les cases adjacentes
    std::unordered_map<int64_t, std::vector<int>> grid;
    auto gkey = [](const Vec3& v) {
        return (((int64_t)floorf(v.x / 8.f) + 65536) * 131072
              + ((int64_t)floorf(v.z / 8.f) + 65536)) * 256
              + ((int64_t)floorf(v.y / 8.f) + 128);
    };
    for (int i = 0; i < nverts; i++) grid[gkey(vs[i])].push_back(i);
    for (int i = 0; i < nverts; i++) {
        Vec3 v = vs[i];
        for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++) {
            Vec3 vv{v.x + dx * 8.f, v.y + dy * 8.f, v.z + dz * 8.f};
            auto it = grid.find(gkey(vv));
            if (it == grid.end()) continue;
            for (int j : it->second)
                if (j != i && (vs[j] - v).len() < 1.5f) {
                    int ri = root(i), rj = root(j);
                    if (ri != rj) par[rj] = ri;
                }
        }
    }
    std::unordered_map<int, std::pair<Vec3,int>> cl;
    for (int i = 0; i < nverts; i++) {
        auto& e = cl[root(i)];
        e.first = e.first + vs[i]; e.second++;
    }
    // centroïdes : un billboard croisé (deux quads en X) donne deux clusters
    // quasi confondus — dédup < 10 unités
    std::vector<Vec3> pts;
    for (auto& kv : cl) {
        Vec3 c = kv.second.first * (1.f / kv.second.second);
        bool dup = false;
        for (auto& p : pts)
            if ((p - c).len() < 10.f) { dup = true; break; }
        if (!dup) pts.push_back(c);
    }
    return pts;
}

// ── init / chargement ─────────────────────────────────────────────────────────
bool World::init(Nds* db) {
    _db = db;
    _pbox = _box(5.f, 8.f, 5.f, {0.9f, 0.3f, 0.35f, 1});
    _npcbox = _box(4.f, 7.f, 4.f, {0.3f, 0.6f, 1.0f, 1});
    _npcbox2 = _box(4.f, 7.f, 4.f, {1.0f, 0.5f, 0.2f, 1});
    // quad billboard partagé (20×28, ancré aux pieds) pour les persos overworld
    // normale verticale : le billboard est éclairé comme le sol sous lui
    // (aNrm sert en espace local dans le VS) — une normale horizontale ne
    // captait que l'ambiante et rendait les persos foncés
    float q[] = {
        -10,0,0,  0,1,0,  1,1,1,1,  0,1,
         10,0,0,  0,1,0,  1,1,1,1,  1,1,
        -10,28,0, 0,1,0,  1,1,1,1,  0,0,
         10,0,0,  0,1,0,  1,1,1,1,  1,1,
         10,28,0, 0,1,0,  1,1,1,1,  1,0,
        -10,28,0, 0,1,0,  1,1,1,1,  0,0,
    };
    _bb = rnd::upload(q, 6, 0);
    _bb.alpha_cut = true; _bb.lit = 1;          // DS : billboards = lampe 0 seule
    // quad 10x10 (moitie du perso) : sprites GENERIC_16x16 de la DS
    // (OBJ_EVENT_GFX POKEBALL 87, STRENGTH_BOULDER 84, MOSS/ICE_ROCK 190/191)
    float q16[] = {
        -5,0,0,  0,1,0,  1,1,1,1,  0,1,
         5,0,0,  0,1,0,  1,1,1,1,  1,1,
        -5,10,0, 0,1,0,  1,1,1,1,  0,0,
         5,0,0,  0,1,0,  1,1,1,1,  1,1,
         5,10,0, 0,1,0,  1,1,1,1,  1,0,
        -5,10,0, 0,1,0,  1,1,1,1,  0,0,
    };
    _bb16 = rnd::upload(q16, 6, 0);
    _bb16.alpha_cut = true; _bb16.lit = 1;
    // quad arbre à baies DS (berry_tree.nsbmd : 16x32 ancré aux pieds)
    float qb[] = {
        -8,0,0,  0,1,0,  1,1,1,1,  0,1,
         8,0,0,  0,1,0,  1,1,1,1,  1,1,
        -8,32,0, 0,1,0,  1,1,1,1,  0,0,
         8,0,0,  0,1,0,  1,1,1,1,  1,1,
         8,32,0, 0,1,0,  1,1,1,1,  1,0,
        -8,32,0, 0,1,0,  1,1,1,1,  0,0,
    };
    _btree_bb = rnd::upload(qb, 6, 0);
    _btree_bb.alpha_cut = true; _btree_bb.lit = 1;
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
        _emote_tex = rnd::upload_tex_nearest(im);
        float eq[] = {                                      // quad 10×10
            -5,0,0, 0,0,1, 1,1,1,1, 0,1,   5,0,0, 0,0,1, 1,1,1,1, 1,1,
            -5,10,0, 0,0,1, 1,1,1,1, 0,0,  5,0,0, 0,0,1, 1,1,1,1, 1,1,
             5,10,0, 0,0,1, 1,1,1,1, 1,0, -5,10,0, 0,0,1, 1,1,1,1, 0,0,
        };
        _emote_bb = rnd::upload(eq, 6, _emote_tex);
        _emote_bb.alpha_cut = true; _emote_bb.lit = 0;   // "!" : aucune lampe
        if (verbose) printf("[init] emote tex=%u vao=%u\n", _emote_tex, _emote_bb.vao);
    }
    // bulle cœur pour le Pokémon suiveur (réaction à l'interaction)
    {   Image im; im.create(14, 12);
        static const char* PX[] = {              // cœur 13x9 dans la texture
            ".XX.XX.",
            "XXXXXXX",
            "XXXXXXX",
            ".XXXXX.",
            "..XXX..",
            "...X...",
        };
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 14; x++) {
                int px = x - 3, py = y - 2;
                bool on = py >= 0 && py < 6 && px >= 0 && px < 7 &&
                          PX[py][px] == 'X';
                if (on) im.set(x, y, 235, 70, 110, 255);
            }
        _heart_tex = rnd::upload_tex_nearest(im);
        float hq[] = {                             // quad 12x10 ancré centre-bas
            -6,0,0,  0,0,1, 1,1,1,1, 0,1,   6,0,0,  0,0,1, 1,1,1,1, 1,1,
            -6,10,0, 0,0,1, 1,1,1,1, 0,0,   6,0,0, 0,0,1, 1,1,1,1, 1,0,
             6,10,0, 0,0,1, 1,1,1,1, 1,0,  -6,10,0,0,0,1, 1,1,1,1, 0,0,
        };
        _heart_bb = rnd::upload(hq, 6, _heart_tex);
        _heart_bb.alpha_cut = true; _heart_bb.lit = 0;
    }
    // bulle « … » du joueur inactif (idle bubble) : rond blanc + 3 points
    {   Image im; im.create(14, 12);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 14; x++) {
                float dx = x - 6.5f, dy = y - 5.5f;
                if (dx*dx + dy*dy > 32.f) continue;
                bool edge = dx*dx + dy*dy > 24.f;
                im.set(x, y, edge ? 40 : 255, edge ? 40 : 255,
                       edge ? 60 : 255, 255);
            }
        for (int d = 0; d < 3; d++)                    // « … » encre
            for (int y = 5; y < 7; y++)
                for (int x = 3 + d * 4; x < 5 + d * 4; x++)
                    im.set(x, y, 40, 40, 60, 255);
        _emote2_tex = rnd::upload_tex_nearest(im);
        float dq[] = {                                   // quad 10×10 (comme "!")
            -5,0,0, 0,0,1, 1,1,1,1, 0,1,   5,0,0, 0,0,1, 1,1,1,1, 1,1,
            -5,10,0, 0,0,1, 1,1,1,1, 0,0,  5,0,0, 0,0,1, 1,1,1,1, 1,1,
             5,10,0, 0,0,1, 1,1,1,1, 1,0, -5,10,0, 0,0,1, 1,1,1,1, 0,0,
        };
        _emote2_bb = rnd::upload(dq, 6, _emote2_tex);
        _emote2_bb.alpha_cut = true; _emote2_bb.lit = 0;
    }
    {   // quad sprite sauvage 40×40 (pl_pokegra 80×80 frame, carré), ancré bas-centre
        float pq[] = {
            -20,0,0, 0,1,0, 1,1,1,1, 0,1,   20,0,0, 0,1,0, 1,1,1,1, 1,1,
            -20,40,0,0,1,0, 1,1,1,1, 0,0,   20,0,0, 0,1,0, 1,1,1,1, 1,1,
             20,40,0, 0,1,0, 1,1,1,1, 1,0, -20,40,0,0,1,0, 1,1,1,1, 0,0,
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
        _dialog.clear(); _mbox = false; _yesno = false; _dlg_dismiss = false;
        _dlg_spk = -1;
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
    _lamps.clear();                          // halos lampadaires par map
    _nsurf = _nverts = 0;
    _gym = GymDyn();                       // features d'arène : reset par map
}

// bascule de zone : recharge les événements/objets/scripts du header
// (DS : transition de zone quand le joueur entre dans une cellule matrice
//  d'un autre header — pas de rechargement géométrie, position conservée)
void World::_set_header(int header_id, bool init) {
    const MapHeader* H = _db->header(header_id);
    if (!H) return;
    if (_header != header_id) {
        _vm.clear_locals();                         // DS ClearLocalFlags
        _prev_map = _header;                        // GetPreviousMapId
        _roamer_step_maps();                        // roamers bougent (DS)
    }
    _header = header_id;
    _floor_txt.clear(); _floor_var = -1;             // fenêtre étage : par map
    _gym_vl = false; _gbanim.bag = -1;     // sacs Veilstone : ré-init par 0x174
    for (auto& b : _gbags) b.obj = -1;
    for (auto& s : _gstacks) s.obj = -1;
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
        for (int i = 0; i < (int)_objects.size(); i++)
            printf("    [obj%d] gfx=%d scr=%d flag=%d x=%d z=%d\n",
                i, _objects[i].gfx, _objects[i].script,
                _objects[i].flag, _objects[i].x, _objects[i].z);
    }

    // état runtime des PNJ (position monde + direction)
    _objpos.resize(_objects.size());
    _objd.resize(_objects.size());
    _objmv.resize(_objects.size());
    _objw.resize(_objects.size());
    _objnocoll.clear();                        // avant la boucle : mv=48 réinsérés
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
        if (o.move == 48) _objnocoll.insert(i);      // téléporteur DS : fantôme
                                                     // à l'init (sub_02062D80 0)
        if (verbose)
            printf("    [obj %d] id=%d gfx=%d mv=%d dir=%d pos=(%d,%d) r=%d,%d scr=%d flag=%d world=(%.0f,%.0f,%.0f)\n",
                (int)i, o.id, o.gfx, o.move, o.dir, o.x, o.z, o.rx, o.rz,
                o.script, o.flag, _objpos[i].x, _objpos[i].y, _objpos[i].z);
    }
    // transitions de zone (init) : les jobs du joueur survivent — le trajet
    // tram du Grand Marais est une FieldTask DS qui traverse les cellules
    // de la matrice ; la sortie ApplyMovement attend la fin du ride.
    for (auto it = _moves.begin(); it != _moves.end();)
        it = (init && it->obj == 0xFF) ? ++it : _moves.erase(it);
    _objgone.clear(); _objhide.clear(); _objlock.clear();
    _objpersist.clear(); _cam_mode = 0;
    _appr_type = -1; _appr[0] = ApprSlot(); _appr[1] = ApprSlot();
    _appr_task[0] = ApprTask(); _appr_task[1] = ApprTask();
    _follower = -1; _ftrail.clear(); _fstep = 0;
    _pftrail.clear(); _pfstep = 0;                 // suiveur : file repart à 0
    _idle_t = 0.f; _idle_bub = 0.f;
    _lockall = 0; _coord_in = -1; _talk_obj = -1; _spk_obj = -1; _dlg_spk = -1;
    _pstep = 0; _ptx = _ppos.x; _ptz = _ppos.z;         // pas en cours -> annulé
    _psurf = 0; _pslide = 0; _pjump = 0;                // DS : warp -> à pied
    _phm = 0; _phm_t = 0;                               // tâche CS annulée
    // visibilité DS : hiddenFlag SET au chargement -> objet non spawné
    // (map_object.c l.617 : les objets sans script spawnett toujours)
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        if (o.script != 0xFFFF && o.flag && _vm.flag(o.flag)) _objgone.insert((int)i);
    }
    // BerryPatches_UpdateGrowthStates : les parcelles de la map passent en
    // isGrowing (la croissance RTC ne les fait évoluer qu'après les avoir vues)
    _berry_setup();
    for (auto& o : _objects)
        if (o.gfx == 100 && o.d0 >= 0 && o.d0 < 128 && _berry[o.d0].berryID)
            _berry[o.d0].growing = 1;
    for (auto& b : _buf) b.clear();
    _dialog.clear(); _mbox = false; _yesno = false; _warp_req = false;
    _dlg_dismiss = false; _dlg_spk = -1;

    // ScriptContext DS : un script actif (ex. villa livraison) survit au
    // Warp/changement de map — sauvegardé, repris après les scripts d'init.
    if (_vm.busy() && !_vm_cont) {
        _vm_ctx = _vm.save_ctx(); _vm_cont = true;
        _vm_ctx_bank = _cur_bank; _vm_ctx_member = _vm_member;
        if (verbose) printf("[vm] ctx sauvegardé pc=%d st=%d (warp)\n",
            (int)_vm_ctx.pc, (int)_vm_ctx.st);
    }

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
    if (getenv("ENCDUMP") && _enc && _enc->size() >= 100) {
        printf("[encdump] hdr=%d rate=%d\n", _header, _enc_rate);
        for (int s = 0; s < 12; s++) {
            size_t eo = 4 + s * 8;
            int sp = (int)u32(*_enc, eo + 4),
                lv = (int)(int8_t)(*_enc)[eo], t1 = -1, t2 = -1;
            battle_types(_db, sp, &t1, &t2);
            printf("[encdump] slot %2d : esp %3d lv %2d types %d/%d\n",
                   s, sp, lv, t1, t2);
        }
    }
    // météo : le byte header est la tendance ; les maps « yearly » (32-36)
    // dépendent de la date (tirage déterministe jour+zone)
    {   // diamond dust : drapeau visuel, décidé ici (base reste pure)
        time_t tt = time(nullptr); struct tm lt{};
        localtime_s(&lt, &tt);
        _wthr.dust = (lt.tm_mon == 0 && lt.tm_mday == 12 && H->wthr >= 34);
    }
    _wthr_base = _weather_base(H->wthr);
    // DS field_map_change : Flash/Défog mémorisés (flags système 2426/2427,
    // posés par DoFlashFunc/DoDefogFunc) effacent la météo de la nouvelle
    // map si c'est une grotte sombre (16) ou de la brume (14). Hors grotte
    // (mapType != CAVE) les deux flags sont effacés à l'arrivée.
    if (H->mtype != 3) {
        _vm.set_flag(2433, false);
        _vm.set_flag(2434, false);
    }
    // DS field_map_change_flags : FLAG_ON_CYCLING_ROAD (2459) et le flag
    // avatar ForceBicycling sont effacés à chaque changement de map —
    // les tuiles frontière de la piste cyclable les réarment au passage.
    _vm.set_flag(2459, false);
    _bon_road = false;
    if ((H->wthr == 14 && _vm.flag(2434)) || (H->wthr == 16 && _vm.flag(2433)))
        _wthr_base = 0;
    _vm.cb_msg = [this](int id, bool instant) {
        if (_tv_suppress) {                          // scène TV : texte routé
            auto& bank = _bank_msgs(_cur_bank);      //  vers l'écran du poste
            _dialog = (id >= 0 && id < (int)bank.size())
                ? bank[id] : ("<msg " + std::to_string(id) + ">");
            _dialog = _fmt_msg(_dialog, &_dlg_cols);
            _dlg_page = 0;
            _dlg_chars = (float)_dialog.size();      // feed TV : texte entier
            _mbox = false;                           // pas de boîte DS
            return;
        }
        auto& bank = _bank_msgs(_cur_bank);
        _dialog = (id >= 0 && id < (int)bank.size())
            ? bank[id] : ("<msg " + std::to_string(id) + ">");
        _dialog = _fmt_msg(_dialog, &_dlg_cols);
        _dlg_page = 0;
        _dlg_chars = instant ? (float)_dialog.size() : 0.f;
        _dlg_spk = _spk_obj;                        // locuteur figé pour le rendu
        if (getenv("SPKDBG")) printf("[dlg] spk=%d talk=%d\n", _dlg_spk, _talk_obj);
        _mbox = true;                               // fenêtre ouverte (DS)
        _dlg_dismiss = false;                       // le script reprend la main
    };
    _vm.cb_box = [this](bool open) {                // OpenMessage/CloseMessage
        _mbox = open; _dlg_dismiss = false;
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
        case 90: {                                   // BufferItemNamePlural : +s
            std::string n = item_name(val);          // sauf fin s/x/z (déjà pluriel)
            char c = n.empty() ? 0 : n.back();
            if (c != 's' && c != 'x' && c != 'z') n += 's';
            _buf[slot] = n; break;
        }
        case 92:                                   // surnom/ŒUF du membre (slot)
            _buf[slot] = (val >= 0 && val < (int)_vm.party.size())
                ? mon_name(_vm.party[val]) : "?"; break;
        case 91: {                                   // espèce + article FR
            std::string n = species_name(val);       // « l' » voyelle, « le » sinon
            char c = n.empty() ? 0 : (char)tolower((unsigned char)n[0]);
            bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o'
                      || c == 'u' || c == 'y' || c == 'h';
            _buf[slot] = (vowel ? "l'" : "le ") + n;
            break;
        }
        case 93: case 94: case 95: case 97: case 98: case 100: {
            int bank = kind == 93 ? 387 : kind == 94 ? 620 : kind == 95 ? 13
                   : kind == 97 ? 386 : kind == 98 ? 551 : 619;
            auto& b = _bank_msgs(bank);
            _buf[slot] = (val >= 0 && val < (int)b.size())
                ? _fmt_msg(b[val]) : "?";
            break;
        }
        case 96: {                                   // CapitalizeFirstLetter
            auto& s = _buf[slot];
            for (auto& c : s)
                if (isalpha((unsigned char)c)) { c = (char)toupper((unsigned char)c); break; }
            break;
        }
        case 102: {                                  // BufferNatureName : banque 457
            auto& b = _bank_msgs(457);
            _buf[slot] = (val >= 0 && val < (int)b.size())
                ? _fmt_msg(b[val]) : "?";
            break;
        }
        case 103:                                    // surnom pension (slot 0/1)
            _buf[slot] = (val == 0 || val == 1) &&
                _vm.daycare[val].species
                ? mon_name(_vm.daycare[val]) : "-"; break;
        case 105:                                    // symbole genre mon
            _buf[slot] = val == 0 ? "♂" : val == 1 ? "♀" : "-"; break;
        case 106: {                                  // BufferFloorNumber : 361
            auto& b = _bank_msgs(361);               // 0->B1F(121), n->1F+n-1(115+n)
            int e = val <= 0 ? 121 : 115 + val;
            _buf[slot] = (e >= 0 && e < (int)b.size())
                ? _fmt_msg(b[e]) : "?"; break;
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
    _vm.cb_move = [this](int obj, size_t off) {
        _queue_move(obj, off);
        // PNJ animé par le script courant : candidat locuteur pour la
        // boîte de dialogue (les cutscenes ne passent pas par _talk_obj)
        if (obj != 0xFF && obj != 0xF2) {
            int i = _live_obj(obj);
            if (i >= 0) _spk_obj = i;
            if (getenv("SPKDBG"))
                printf("[spk] move localid=%d -> i=%d gfx=%d(%s)\n",
                    obj, _spk_obj, i >= 0 ? obj_gfx(i) : -1,
                    i >= 0 ? gfx_name(obj_gfx(i)) : "?");
        }
    };
    _vm.cb_warp = [this](int hdr, int x, int z, int dir) {
        _pw = {hdr, x, z, dir}; _warp_req = true;
    };
    _vm.cb_turnback = [this](int pillars, int rooms) {
        // ov5 InitTurnbackCave : réécrit le dest des 4 warps de la salle
        // (sauf celui d'entrée) selon piliers vus / salles visitées.
        int dest;
        if (pillars >= 3) dest = 270;              // GIRATINA_ROOM
        else if (rooms >= 30) dest = 268;          // ENTRANCE
        else if (rand() % 100 < 25) dest = 269;    // PILLAR_ROOM (hub)
        else {
            static const int ROOMS[] = {
                271, 272, 273, 518, 519, 520,      // pilier 1
                521, 522, 523, 524, 525, 526,      // pilier 2
                527, 528, 529, 530, 531, 532 };    // pilier 3
            dest = ROOMS[pillars * 6 + rand() % 6];
        }
        int x = (int)floorf(_ppos.x / TILE), z = (int)floorf(_ppos.z / TILE);
        int entry = (x == 11) ? (z == 1 ? 0 : z == 20 ? 2 : 5)
                            : (x == 20 ? 1 : 3);
        for (int i = 0; i < 4 && i < (int)_warps.size(); i++)
            if (i != entry) _warps[i].dest = dest;
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
    _vm.cb_hide = [this](int id, bool hide) {             // ShowObject/HideObject :
        int i = _live_obj(id);                          // toggle sur l'objet VIVANT
        if (i < 0) return;                              // (pas de spawn/delete)
        if (hide) _objhide.insert(i); else _objhide.erase(i);
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
        _fade.dur = (frames > 0 ? frames : 1) / 30.f;   // frames DS @30Hz
        _fade.out = (trans != 0);            // transition!=0 -> vers la couleur
        if (verbose) printf("[fade] trans=%d f=%d type=%d col=%d\n",
            trans, frames, type, color);
    };
    _vm.cb_pos = [this](int axis) {          // GetPlayerMapPos : tuiles monde
        if (axis == 2) return (int)floorf(_ppos.y / 8.f); // 3DPos.y (unités 8px)
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
        _pstep = 2; _pstep_spd = 60.f; _pjump = 1;    // saut d'eau 8 ticks @30Hz
        _warp_cd_x = _warp_cd_z = -9999;              // rencontre possible
    };
    _vm.cb_climb = [this]() {                // UseRockClimb (field_moves fn3) :
        int ddx, ddz; dirv(_pdir, ddx, ddz); // SubTask_HopOn : saut JUMP_NEAR
        _ptx = _ppos.x + ddx * TILE; _ptz = _ppos.z + ddz * TILE;
        {   Vec3 tp{_ptx, _ppos.y, _ptz};
            float gy = _ground_y(tp, _ppos.y + TILE);   // rampe de la paroi
            _pty = gy > -1e8f ? gy : _ppos.y; }
        _pstep = 2; _pstep_spd = 120.f; _pjump = 1;   // FAST : 8 ticks @30Hz
        _phm = 1;
        if (_snd) { int s = _snd->seq_named("SEQ_SE_DP_UG_023_sseq");
            if (s >= 0) _snd->se(s); }
    };
    _vm.cb_wfall = [this]() {                // UseWaterfall (field_moves fn6) :
        _wfall_start(_pdir == 1);            // DS : DIR_NORTH -> montée,
    };                                       // sinon descente (toujours sud)
    _vm.cb_wclear = [this]() {               // Flash/Défog (0xC3/0xC4) : la
        _wthr_base = 0;                      // météo affichée passe à CLEAR ;
    };                                       // tick -> fondu de sortie
    _vm.cb_hmcut = [this](int slot) {        // PlayHmCutIn : lance le cut-in
        _cutin_slot = slot;                  // (bande blanche + sprite CS qui
        _cutin_f = 0.f; _cutin_cry = false;  // traverse), la VM attend HMCUT ;
        _cutin_sp = (slot >= 0 && slot < (int)_vm.party.size())
            ? _vm.party[slot].species : 0;   // espèce du lanceur
    };
    _vm.cb_findmove = [this](int mv) {       // FindPartySlotWithMove :
        for (int i = 0; i < (int)_vm.party.size(); i++) { // slot du 1er Pokémon
            if (_vm.party[i].egg) continue;               // DS : œufs ignorés
            auto bm = battle_make(_db, _vm.party[i].species,
                _vm.party[i].level, nullptr, _vm.party[i].ivs);
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
        if (lock) { _objlock.insert(i); _spk_obj = i; }
        else _objlock.erase(i);
    };
    _vm.cb_nocoll = [this](int id, bool nocoll) {         // 2B6 : STATUS_18 DS
        int i = _live_obj(id);
        if (i < 0) return;
        if (nocoll) _objnocoll.insert(i); else _objnocoll.erase(i);
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
    _vm.cb_mtypes = [this](int sp, int* t1, int* t2) {
        battle_types(_db, sp, t1, t2);                       // GetPartyMonType
    };
    _vm.cb_moninit = [this](ScriptVM::PartyMon& p) {        // init DS GivePokemon :
        auto bm = battle_make(_db, p.species, p.level, nullptr, // PID/IVs/XP/moves/PP/PV
            p.ivs == 0xFFFFFFFFu ? IVS_UNSET : p.ivs);          // tout ce qui manque
        p.ivs = bm.ivs;
        if (!p.pid) p.pid = bm.pid;
        if (!p.exp) p.exp = bm.exp;
        p.friendship = bm.friendship;
        if (p.hp <= 0) p.hp = bm.maxhp;
        if (!p.nmoves) {
            p.nmoves = bm.nmoves;
            for (int k = 0; k < p.nmoves; k++) {
                p.moves[k] = bm.moves[k]; p.pp[k] = bm.pp[k]; }
        }
    };
    _vm.cb_mgender = [this](int sp, uint32_t pid) -> int {  // genre vs ratio byte 0x10
        static const char* PP = "/poketool/personal/pl_personal.narc";
        auto n = _db->narc(PP);
        if (!n || sp <= 0 || sp >= n->count()) return 2;
        const Bytes& d = n->member(sp);
        int r = d.size() > 0x10 ? d[0x10] : 0;
        if (r == 0xFF) return 2;                            // asexué
        if (r == 0xFE) return 1;
        if (r == 0) return 0;
        return (int)(pid % 256) < r ? 1 : 0;                // pid%256 < ratio -> femelle
    };
    _vm.cb_pgender = [this]() { return _gender; };          // genre joueur (DS)
    _vm.cb_berry = [this](int sub, int arg) -> int {        // parcelles à baies
        return _berry_op(sub, arg);
    };
    _vm.cb_gym = [this](int sub, int arg) -> int {          // features d'arène
        return _gym_op(sub, arg);
    };
    _vm.cb_save = [this](int op) -> int {
        if (op == 1)                                        // CheckSaveType
            return _savepath.empty() ? 0 : (fs::exists(_savepath) ? 1 : 0);
        return (!_savepath.empty() && save(_savepath.c_str())) ? 1 : 0; // TrySaveGame
    };
    _vm.cb_relearncnt = [this](int slot) -> int {           // capacités réapprenables
        const auto& pm = _vm.party[slot];
        bool known[560] = {0}, learn[560] = {0};
        for (int k = 0; k < 4; k++) if (pm.moves[k] > 0) known[pm.moves[k]] = true;
        int tmp[8];
        for (int lv = 1; lv <= pm.level; lv++) {
            int nm = mon_learnset_at(_db, pm.species, lv, tmp);
            for (int k = 0; k < nm; k++) if (tmp[k] > 0) learn[tmp[k]] = true;
        }
        int n = 0;
        for (int m = 1; m < 560; m++) if (learn[m] && !known[m]) n++;
        return n;
    };
    _vm.cb_relearnlist = [this](int slot, int* out) -> int {  // ordre learnset DS
        const auto& pm = _vm.party[slot];
        bool known[560] = {0}, seen[560] = {0};
        for (int k = 0; k < 4; k++) if (pm.moves[k] > 0) known[pm.moves[k]] = true;
        int tmp[8], n = 0;
        for (int lv = 1; lv <= pm.level && n < 22; lv++) {
            int nm = mon_learnset_at(_db, pm.species, lv, tmp);
            for (int k = 0; k < nm; k++)
                if (tmp[k] > 0 && tmp[k] < 560 && !known[tmp[k]] &&
                    !seen[tmp[k]]) { seen[tmp[k]] = true; out[n++] = tmp[k]; }
        }
        return n;
    };
    _vm.cb_teachmove = [this](int slot, int mv, int forget) -> bool {
        if (slot < 0 || slot >= (int)_vm.party.size() ||
            mv <= 0 || mv >= 560) return false;
        auto& pm = _vm.party[slot];
        int i = forget;
        if (i < 0) {                                        // place libre auto
            if (pm.nmoves >= 4) return false;
            i = pm.nmoves;
        }
        if (i < 0 || i > 3) return false;
        pm.moves[i] = mv;
        pm.pp[i] = battle_move(_db, mv).pp;
        if (i == pm.nmoves) pm.nmoves++;
        return true;
    };
    _vm.cb_swarm = [this](int* map, int* sp) {              // essaim du jour
        _swarm_map_species(map, sp);
    };
    _vm.cb_honeysp = []() -> int {                          // arbre à miel
        // table DS : Wurmple, Silcoon, Cascoon, Burmy, Cherubi, Combee, Aipom,
        // Munchlax rare (espèces Sinnoh réelles)
        static const int TBL[][2] = {
            {265, 30}, {266, 10}, {268, 10}, {412, 15}, {420, 10},
            {415, 10}, {190, 14}, {446, 1}};
        int r = rand() % 100, acc = 0;
        for (auto& e : TBL) { acc += e[1]; if (r < acc) return e[0]; }
        return 265;
    };
    _vm.cb_runstate = [this]() -> int { return _can_run ? 1 : 0; };
    _vm.cb_mapid = [this]() -> int { return _header; };
    _vm.cb_specmap = [this]() -> int { return _special.hdr; };
    _vm.cb_prevmap = [this]() -> int { return _prev_map; };
    _vm.cb_exitloc = [this](int m, int x, int z) {          // SetExitLocation
        _exit_map = m; _exit_x = x; _exit_z = z;
    };
    // échanges PNJ : fld_trade.narc, enregistrements 80o (NPCTradeMon decomp)
    _vm.cb_trade = [this](int id, int* sp, int* req, int* it,
                          int* ot, std::string* nk) {
        auto n = _db->narc("/fielddata/pokemon_trade/fld_trade.narc");
        if (!n || id < 0 || id >= (int)n->count()) return;
        const Bytes& t = n->member(id);
        if (t.size() < 80) return;
        auto rd = [&](int i) { return (int)u32(t, i * 4); };
        if (sp)  *sp = rd(0);
        if (req) *req = rd(19);
        if (it)  *it = rd(15);
        if (ot)  *ot = rd(8);
        if (nk) {                                    // TEXT_BANK_NPC_TRADE_NAMES
            auto& b = _bank_msgs(370);
            *nk = (id < (int)b.size()) ? _fmt_msg(b[id]) : "";
        }
    };
    _vm.cb_trademake = [this](ScriptVM::PartyMon& m) {   // IVs/PID du record
        if (_vm.trade_id < 0 || !_vm.cb_trade) return;
        int sp = 0, req = 0, it = 0, ot = 0; std::string nk;
        _vm.cb_trade(_vm.trade_id, &sp, &req, &it, &ot, &nk);
        auto n = _db->narc("/fielddata/pokemon_trade/fld_trade.narc");
        if (n && _vm.trade_id < (int)n->count()) {
            const Bytes& t = n->member(_vm.trade_id);
            if (t.size() >= 80) {
                // IVs 5b/stat (hp,atk,def,vit,spa,spd) + personnalité fixe
                m.ivs = (u32(t, 4) & 31) | ((u32(t, 8) & 31) << 5) |
                        ((u32(t, 12) & 31) << 10) | ((u32(t, 16) & 31) << 15) |
                        ((u32(t, 20) & 31) << 20) | ((u32(t, 24) & 31) << 25);
                m.pid = u32(t, 56);
            }
        }
        int lv = m.level;
        if (_vm.cb_moninit) _vm.cb_moninit(m);
        m.level = lv;                                // niveau du mon donné
        m.nick = nk; m.otid = ot; m.held = it;
    };
    // roamers (roaming_pokemon.c) : slot -> espèce/niveau DS
    _vm.cb_roam = [this](int slot) {
        static const int SP[6] = {481, 488, 491, 146, 145, 144};
        static const int LV[6] = {50, 50, 40, 60, 60, 60};
        if (slot < 0 || slot > 5 || _roamers[slot].sp) return;
        _roamers[slot].sp = SP[slot]; _roamers[slot].lv = LV[slot];
        _roamers[slot].hp = -1;                       // PV recalculés au combat
        _roamers[slot].route = _roamer_random_map(-1);
        _vm.dex_seen.insert(SP[slot]);
    };
    // pokédex : op 0 -> n° Sinnoh de l'espèce nationale (pl_pokezukan)
    _vm.cb_dex = [this](int op, int sp) -> int {
        if (op != 0 || sp < 1 || sp > 493) return 0;
        static std::vector<uint16_t> sin;
        if (sin.empty()) {
            auto zn = _db->narc("/poketool/pl_pokezukan.narc");
            if (zn && zn->count()) {
                const Bytes& z = zn->member(0);
                for (int i = 0; i + 2 <= (int)z.size(); i += 2)
                    sin.push_back(z[i] | (z[i + 1] << 8));
            }
        }
        return sp <= (int)sin.size() ? sin[sp - 1] : 0;
    };
    _vm.cb_eggsp = [this](int sp) -> int {              // espèce bébé (pension)
        // remonter la chaîne d'évolution (evo.narc : 7 entrées de 6o)
        auto n = _db->narc("/poketool/personal/evo.narc");
        if (!n) return sp;
        int cur = sp;
        for (int depth = 0; depth < 6; depth++) {
            int pre = 0;
            for (int i = 1; i < (int)n->count() && !pre; i++) {
                if (i == cur) continue;
                const Bytes& d = n->member(i);
                for (int e = 0; e < 7 && e * 6 + 6 <= (int)d.size(); e++) {
                    if (!u16(d, e * 6)) break;
                    if (u16(d, e * 6 + 4) == cur) { pre = i; break; }
                }
            }
            if (!pre) break;
            cur = pre;
        }
        return cur;
    };
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
            auto bm = battle_make(_db, p.species, p.level, nullptr, p.ivs);
            p.ivs = bm.ivs; p.hp = bm.maxhp;
        } };
    _vm.cb_setbo = [this](int id) {                       // SetBlackOutWarpId
        if (id >= 1 && id <= SPAWN_TBL_N) _blackout_id = id; };
    _vm.cb_pic = [this](int sp, bool on) { _pic_species = on ? sp : 0; };
    // PrintTrainerDialogue : trtblofs -> trtbl -> pl_msg[617] (TEXT_BANK_NPC_TRAINER_MESSAGES)
    _vm.cb_trainermsg = [this](int tid, int mtype) {
        auto ofs = _db->narc(Nds::P_TRTBLOF), tbl = _db->narc(Nds::P_TRTBL);
        _dialog.clear(); _dlg_chars = 0;
        if (!ofs || !tbl || !ofs->count() || !tbl->count()) return;
        _dlg_spk = _spk_obj;                      // dresseur vu par le script
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
    _vm.cb_audio   = [this](int op,int a) { return audio_op(op,a); };
    _vm.cb_sndbusy = [this](int k) { return audio_busy(k); };
    _vm.cb_appr = [this](int n, int op) -> int {          // B6-B9 approche
        switch (op) {
        case 0: return appr_start(n);
        case 1: return appr_done(n);
        case 2: return appr_id(n);
        default: return appr_type();
        }
    };
    _vm.cb_item = [this](int op, int id) -> int {
        if (op == 0) return item_pocket(id);              // ITEM_PARAM_FIELD_POCKET
        return 0;
    };
    _vm.cb_fld = [this](int op, int a, int b) -> int {    // divers état terrain
        switch (op) {
        case 0: {                                       // SetObjectFlagIsPersistent
            for (size_t i = 0; i < _objects.size(); i++)
                if (_objects[i].id == a) {
                    if (b) _objpersist.insert((int)i);
                    else _objpersist.erase((int)i);
                }
            return 1; }
        case 1: _pstate_pending = a; return 1;          // SetPlayerState
        case 2: _pstate = _pstate_pending; return 1;    // ChangePlayerState
        case 14: _bon_road = a != 0; return 1;          // ForceBicycling (avatar flag DS)
        case 15: _vm.set_var(a, _pstate); return 1;     // GetPlayerState -> var
        case 16: _pstate = a ? 1 : (_pstate == 1 ? 0 : _pstate);  // SetPlayerBike
            _bspd = 0; _bbrake = false; _bslope = 0; return 1;
        case 17: {                                    // SetCyclingBGM (SEQ_BICYCLE)
            int s = _snd ? _snd->seq_named("SEQ_BICYCLE") : -1;
            if (s < 0 && _snd) s = _snd->seq_named("SEQ_PL_BICYCLE");
            set_bgm_override(s); return 1; }
        case 3: _cam_mode = 1; _cam_x = (float)a; _cam_z = (float)b; return 1;
        case 4: _cam_mode = 0; return 1;                // RestoreCamera
        case 5: _cam_mode = 2; _cam_x = (float)a; _cam_z = (float)b; return 1;
        case 6: _cam_mode = 0; return 1;                // RemoveCameraOverrideObj
        case 7:                                         // DestroyObstacle (anim)
            // DS : arg0 = TYPE d'obstacle (0 arbre/1 rocher/2 bloc), pas un
            // localID — la suppression réelle vient du RemoveObject suivant.
            return 1;
        case 8: _seqvol[a] = b; if (_snd) _snd->seq_vol(a, b); return 1;
        case 9: _want_xmenu = true; return 1;           // ShowStartMenu
        case 10:                                        // MoveCamera : offset joueur
            _cam_mode = 3;
            _cam_ox = (float)(short)((a >> 16) & 0xFFFF);
            _cam_oz = (float)(short)(a & 0xFFFF);
            _cam_oy = (float)b; return 1;
        case 11:                                        // SetSpecialLocation
            _special = {_vm.special_loc[0], _vm.special_loc[1],
                        _vm.special_loc[2], _vm.special_loc[3],
                        _vm.special_loc[4]};
            return 1;
        case 12:                                        // PlayElevatorAnimation
            if (_snd) {
                int s = _snd->seq_named("SEQ_SE_DP_ELEBETA");
                if (s >= 0) _snd->se(s);
            }
            return 1;
        case 13: {                                      // ShowCurrentFloor : fenêtre "Étage XF"
            auto& bk = _bank_msgs(361);                 // DS : 15=libellé, 16={STRVAR}=_buf[0]
            std::string lb = (15 < (int)bk.size()) ? _fmt_msg(bk[15]) : "";
            _floor_txt = lb + "\n" + _buf[0];
            _floor_var = a;                             // fermeture quand la var vaut 0xFFFF
            return 1; }
        }
        return 0;
    };
    _label = _db->loc_name(H->label);
    if (getenv("WARPDBG")) fprintf(stderr, "[label] hdr=%d label=%d -> '%s'\n",
        header_id, H->label, _label.c_str());
    _mapname = _db->header_name(header_id);
    // BGM de zone : table MapHeader 24B de l'arm9 (jour/nuit)
    _mus_day = _db->map_music(header_id, false);
    _mus_night = _db->map_music(header_id, true);
    if (_mus_night < 0) _mus_night = _mus_day;
    _cam_type = _db->map_camera(header_id);
    _bgm_override = -1;                       // nouvelle zone : reset SetBGM
    _music_update();
    // DS map_name_popup : windowID==0 ou IsBuilding (INDOORS/POKECENTER) -> pas de popup
    _banner_t = (H->win == 0 || H->mtype == 4 || H->mtype == 5) ? 0.f : 1.5f;
    // Monde Distorsion (transition streaming init) : DynamicMapFeaturesInit
    // avant les scripts OnTransition ; load_map le fait dans _finish_load
    // (après _unload_all, sinon les props fantômes sont libérés)
    if (init) {
        if (header_id >= 573 && header_id <= 583 && header_id != 578)
            _dw_init(header_id);
        else if (_dw.on) _dw_free();
        _run_init();
    }
}

// header wthr -> type concret. Les maps « yearly » (32-36 : Rte 212 sud,
// 213, 216, Lac Savoir, Frimapic) varient selon la date réelle — hachage
// (jour_année, header) déterministe = tirage climatique par jour (approxi-
// mation : les vraies tables date->météo sont dans field_weather.c).
int World::_weather_base(int w) const {
    if (w == 26 || w == 27) return 15;      // brouillard sombre (SetDarkFog)
    if (w < 32 || w > 36) return w;
    time_t tt = time(nullptr); struct tm lt{};
    localtime_s(&lt, &tt);
    // diamond dust : paillettes au lieu de neige les 12 janv. dans le nord
    if (lt.tm_mon == 0 && lt.tm_mday == 12 && w >= 34) return 5;
    unsigned h = (unsigned)(lt.tm_yday * 131 + _header);
    h = (h * 2654435761u) >> 24;
    int r = (int)(h % 100);
    switch (w) {
    case 32: return r < 60 ? 2 : r < 80 ? 3 : 1;    // Rte 212 : pluie dominante
    case 33: return r < 40 ? 2 : r < 70 ? 1 : 0;    // Rte 213 : variable
    case 34: return r < 50 ? 5 : r < 80 ? 6 : 0;    // Rte 216 : neige dominante
    default: return r < 55 ? 5 : 0;                  // Lac Savoir / Frimapic
    }
}

// essaim du jour : sSwarmMapIdTable DS -> indices de headers (ordre de la
// table ROM), puis le slot essaim des rencontres de la map choisie
// routes errantes DS (RoamingPokemonRoutes) : R201-R222 + éoliennes + Fuego
static const int ROAM_ROUTES[] = {
    342, 343, 344, 345, 346, 347, 349, 350, 353, 354, 356, 362, 363,
    365, 366, 367, 371, 373, 380, 382, 383, 385, 388, 391, 467, 392,
    395, 200, 204};
static bool _roam_valid(int h) {
    for (int r : ROAM_ROUTES) if (r == h) return true;
    return false;
}
int World::_roamer_random_map(int exclude) {
    int idx = -1;
    for (int t = 0; t < 40; t++) {
        int i = rand() % (int)(sizeof(ROAM_ROUTES) / sizeof(int));
        if (ROAM_ROUTES[i] != exclude) { idx = i; break; }
    }
    return idx < 0 ? ROAM_ROUTES[0] : ROAM_ROUTES[idx];
}
void World::_roamer_step_maps() {
    // DS : à chaque changement de map, 1/16 de téléport aléatoire, sinon
    // route voisine (approx : autre route, jamais celle qu'on quitte)
    for (auto& r : _roamers) {
        if (!r.sp) continue;
        r.route = _roamer_random_map(_prev_map);
    }
}

void World::_swarm_map_species(int* map, int* sp) const {
    static const int SWARM_MAPS[22] = {342,343,344,350,353,354,356,380,382,
        385,388,392,395,399,400,469,403,406,407,471,200,203};
    time_t tt = time(nullptr); struct tm lt{}; localtime_s(&lt, &tt);
    int hdr = SWARM_MAPS[lt.tm_yday % 22];
    *map = hdr; *sp = 0;
    const MapHeader* mh = _db->header(hdr);
    auto en = _db->narc(Nds::P_ENC);
    if (!mh || !en || mh->wild >= en->count()) return;
    const Bytes& d = en->member(mh->wild);
    if (d.size() >= 0x6C) *sp = (int)u32(d, 0x64);      // swarmEncounters[0]
}

// ---- audio : horloge DS (nuit 20h-4h), routage des ops de script ----------
bool World::is_night() const {
    if (dbg_night >= 0) return dbg_night != 0;   // override debug (menu F1)
    if (dbg_hour >= 0) return dbg_hour >= 20 || dbg_hour < 4;  // --time
    time_t tt = time(nullptr); struct tm lt{};
    localtime_s(&lt, &tt);
    return lt.tm_hour >= 20 || lt.tm_hour < 4;
}

void World::_music_update() {
    if (!_snd || _btl.active) return;
    if (_bgm_victory) {                         // jingle victoire -> fin -> zone
        if (_snd->player_busy(7)) return;
        _bgm_victory = false; _bgm_cur = -2;
    }
    int want = _bgm_override >= 0 ? _bgm_override : bgm_seq(is_night());
    if (want != _bgm_cur) { _bgm_cur = want; if (want >= 0) _snd->music(want); }
}

int World::audio_op(int op, int a) {
    if (!_snd) return 0;
    switch (op) {
    case 0x49: _snd->se(a); return 1;                       // PlaySE
    case 0x4A: _snd->se_stop(a); return 1;                  // StopSE
    case 0x4C: _snd->cry(a & 0x7FF, a >> 11); return 1;     // PlayCry
    case 0x4E: _snd->fanfare(a); return 1;                  // PlayFanfare
    case 0x50: _bgm_override = a; _snd->music(a); _bgm_cur = a; return 1;
    case 0x51: _snd->music_fade(0); _bgm_override = -1; _bgm_cur = -2; return 1;
    case 0x52: _bgm_override = -1; _bgm_cur = -2;
        _music_update(); return 1;                          // PlayDefaultBGM
    case 0x54: _snd->music_fade(a > 0 ? a : 30); return 1;  // FadeOutBGM
    case 0x57: _bgm_override = a; return 1;                 // SetBGM (mémorisé)
    case 0xEA: {                                          // PlayTrainerEncounterBGM
        // DS ScrCmd_PlayTrainerEncounterBGM : l'opérande est le trainerID ;
        // Sound_SwapBGM(FieldBGM_GetEyesMeetForTrainer) — table classe->SEQ_EYE
        int tcls = 0;
        if (auto td = _db->narc(Nds::P_TRDATA)) {
            if (a >= 0 && a < td->count() && td->member(a).size() >= 20)
                tcls = td->member(a)[1];                  // TrainerHeader.trainerType
        }
        static const struct { uint8_t cls; uint16_t seq; } EYE[] = {
            {2,1100},{3,1110},{4,1100},{5,1110},{6,1100},{7,1104},{8,1101},
            {9,1105},{10,1102},{11,1107},{12,1111},{13,1111},{14,1102},
            {15,1112},{16,1100},{17,1104},{18,1105},{19,1111},{20,1112},
            {21,1112},{22,1101},{23,1112},{24,1109},{25,1109},{26,1112},
            {27,1109},{28,1101},{29,1109},{30,1111},{31,1109},{32,1106},
            {33,1106},{34,1106},{35,1106},{36,1104},{37,1108},{38,1102},
            {39,1109},{40,1109},{41,1108},{42,1111},{43,1104},{44,1101},
            {45,1101},{46,1107},{47,1111},{48,1105},{49,1102},{50,1102},
            {51,1106},{52,1102},{53,1109},{54,1109},{55,1111},{56,1111},
            {57,1102},{58,1112},{59,1112},{60,1100},{61,1110},{65,1113},
            {66,1113},{67,1113},{68,1113},{69,1114},{70,1105},{71,1105},
            {72,1103},{73,1103},{80,1104},{81,1112},{82,1112},{83,1112},
            {84,1112},{85,1110},{87,1103},{88,1103},{89,1103},{98,1112},
        };
        int seq = 1101;                                   // défaut DS : SEQ_EYE_KID
        for (auto& e : EYE) if (e.cls == tcls) { seq = e.seq; break; }
        _bgm_override = seq; _snd->music(seq); _bgm_cur = seq;
        return 1; }
    }
    return 0;
}

int World::audio_busy(int kind) {
    if (!_snd) return 0;
    switch (kind) {
    case 1: return _snd->se_any_busy() ? 1 : 0;             // un SE quelconque
    case 2: return _snd->cry_busy() ? 1 : 0;
    case 3: _snd->fanfare_check(); return _snd->fanfare_busy() ? 1 : 0;
    }
    return 0;
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
        _phidden = false; _phcalc = true;  // restauration DS hors du tram
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

    // grille d'attributs de toute la matrice : la collision terrain ne
    // dépend plus des cellules chargées (mur fantôme en bord de flux).
    // Remplit aussi le cache _lands que _load_cell réutilisera.
    _mattrs.assign((size_t)_mtx.w * 32 * _mtx.h * 32, 0);
    {
        auto ln = _db->narc(Nds::P_LAND);
        for (int i = 0; i < (int)_mtx.land.size(); i++) {
            int lid = _mtx.land[i];
            if (lid == 0xFFFF) continue;           // trou de matrice : libre
            auto lit = _lands.find(lid);
            if (lit == _lands.end()) {
                std::shared_ptr<Land> L;
                if (ln) L = std::make_shared<Land>(
                    Land::parse(ln->member(lid)));
                lit = _lands.emplace(lid, L).first;
            }
            if (!lit->second) continue;
            int cx = i % _mtx.w, cy = i / _mtx.w;
            for (int r = 0; r < 32; r++)
                memcpy(&_mattrs[(cy * 32 + r) * (_mtx.w * 32) + cx * 32],
                       &lit->second->attrs[r * 32], 32 * sizeof(uint16_t));
        }
    }

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
    // texset des props DS (areabm_texset, meme index que le prop set) :
    // sert de fallback pour les materiaux de terrain non lies a mapTexture
    // (sur DS ils restent lies au tex prop encore en VRAM — ex. gym_obj1/2
    // et h_kage de l'arene d'Eternia) et pour les props sans TEX0 embarque.
    _ptexset.clear();
    if (auto abt = _db->narc(Nds::P_ABMTEX)) {
        const Bytes& pd = abt->member(ar.props);
        _ptexset = bmd::parse_tex0(pd, 0);
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
// instanciation d'un prop (corps par-prop de _inst_props, réutilisé par
// _spawn_prop pour les props dynamiques type nappe d'arène). ct_world reçoit
// les triangles de collision transformés (nullptr = ignorés — ex. nappe).
// build_model_matshp.dat : paires (matériau, shape) par modèle — dessinées
// sans transform de nœud (NNS_G3dDraw1Mat1Shp sous la base du prop) : la porte
// intégrée des maisons reste ainsi à l'intérieur du bâtiment et libère
// l'embrasure pour le prop porte animé.
const std::vector<std::pair<int, int>>* World::_matshp_pairs(int model) {
    if (!_matshp_loaded) {
        _matshp_loaded = true;
        const Bytes& d = _db->open_path(
            "/fielddata/build_model/build_model_matshp.dat");
        if (d.size() >= 4) {
            int nloc = u16(d, 0), nids = u16(d, 2);
            size_t ids_off = 4 + (size_t)nloc * 4;
            for (int m = 0; m < nloc; m++) {
                int cnt = u16(d, 4 + m * 4), idx = u16(d, 6 + m * 4);
                auto& v = _matshp[m];
                for (int i = 0; i < cnt && idx + i < nids; i++) {
                    size_t o = ids_off + (idx + i) * 4;
                    if (o + 4 > d.size()) break;
                    v.push_back({u16(d, o), u16(d, o + 2)});
                }
            }
        }
    }
    auto it = _matshp.find(model);
    return it != _matshp.end() && !it->second.empty() ? &it->second : nullptr;
}

World::PropInst World::_prop_make(const LandProp& lp, Vec3 org, int64_t cell,
    std::vector<Vec3>* ct_world) {
    PropInst pi;
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
    if (!model) {
        static std::unordered_set<int> warned;
        if (warned.insert((int)lp.model).second)
            fprintf(stderr, "[prop] model %d introuvable -> mur invisible ?\n",
                (int)lp.model);
        return pi;                              // pi.mdl == nullptr : échec
    }
    pi.cell = cell;                             // déchargement ciblé (-1 = non)
    pi.pos = org + lp.pos;
    pi.model = (int)lp.model;
    pi.mdl = model.get();
    uint16_t ry = (uint16_t)((int)lp.rot.y & 0xFFFF);
    pi.ry = (float)ry / 65536.f * 6.28318530718f;
    pi.scale = lp.scale.len() > 0 ? lp.scale : Vec3{1,1,1};
    pi.org = pi.pos;
    // anim d'ambiance DS (éoliennes, fontaines) : entry non différée
    // -> attachée au chargement, boucle infinie dans _tick_prop_anims
    int ai = getenv("NOANIM") ? -1 : _anim_auto(pi.model);
    if (ai >= 0) {
        auto ids = _anim_ids(pi.model);
        if (ids && ai < (int)ids->size()) {
            if (auto* an = _bca_anim((*ids)[ai])) {
                pi.anm = an; pi.anm_idx = ai; pi.anm_loop = true;
            } else if (auto* tn = _bta_anim((*ids)[ai])) {
                pi.tanms = tn;              // SRT0 : anim UV (fontaines…)
            } else if (auto* tp = _btp_anim((*ids)[ai])) {
                pi.pats = tp;               // PAT0 : swap texture (éoliennes…)
            }
        }
    }
    auto& texs = model->textures.empty() ? _texset : model->textures;
    if (verbose) printf("    [prop %d] mats=%d pos=%.0f,%.0f,%.0f\n", (int)lp.model,
        (int)model->meshes.size(), pi.pos.x, pi.pos.y, pi.pos.z);
    // liste de dessin : matshp = paires (matériau, shape) dessinées SANS
    // transform de nœud (NNS_G3dDraw1Mat1Shp sous la base du prop — la porte
    // intégrée reste alors dans la maison et libère l'embrasure) ; sinon
    // chaque groupe avec son propre matériau.
    struct Draw { const bmd::MeshGroup* g; const bmd::MatDef* md; int unx; };
    std::vector<Draw> draws; std::vector<M4> unxs;
    if (const auto* pairs = _matshp_pairs(pi.model)) {
        unxs.reserve(pairs->size());
        for (auto& [mi, sj] : *pairs) {
            const bmd::MeshGroup* g = nullptr;
            for (auto& mg : model->meshes)
                if (mg.shape == sj) { g = &mg; break; }
            if (!g || mi < 0 || mi >= (int)model->mats.size()) continue;
            // Draw1Mat1Shp = sommets bruts du shape sous la base du prop :
            // on retire le nœud (bones[node]) mais PAS le scale_basis du
            // flux de rendu — sinon le modèle rapetisserait d'un facteur up_s.
            unxs.push_back(g->node >= 0 && g->node < (int)model->bones.size()
                ? model->bones[g->node].inverse() : M4::ident());
            draws.push_back({g, &model->mats[mi], (int)unxs.size() - 1});
        }
    }
    if (draws.empty())
        for (auto& mg : model->meshes)
            draws.push_back({&mg, &mg.matdef, -1});
    for (auto& ds : draws) {
        bmd::MeshGroup mg = *ds.g;
        mg.matdef = *ds.md;
        if (ds.unx >= 0)                        // sommets -> repère nœud (DS)
            for (auto& pr : mg.prims)
                for (auto& v : pr.v) {
                    v.p = unxs[ds.unx].xform(v.p);
                    v.n = unxs[ds.unx].xform_n(v.n);
                }
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
        bool hidden = hide && *hide
            && mg.matdef.name.find(hide) != std::string::npos;
        bool alpha = false; int polyA = 31;
        uint32_t t = hidden ? 0
            : _mat_tex(mg.matdef, texs, &alpha, &polyA);
        GpuMesh g = hidden ? GpuMesh{}
            : rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
        g.dbg_name = mg.matdef.name;
        g.alpha_cut = alpha;
        g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
        // décalcomanies lumineuses (*lamp*, points light*) : translucides —
        // voir la même règle dans le chemin cellules
        if (mg.matdef.name.find("lamp") != std::string::npos
            || mg.matdef.name.rfind("light", 0) == 0) {
            g.blend = 0.98f; g.alpha_cut = false;
        }
        /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
        /*DS attr*/ g.lit = mg.matdef.poly & 0xF;          // masque lampes (bits 0-3)
        g.water = tex_is_water(mg.matdef.tex_name)
            || mg.matdef.name.rfind("c1_fun", 0) == 0;
        g.mark_water(buf);
        // eau de fontaine : très légère translucidité (la vasque se devine)
        if (mg.matdef.name.rfind("c1_fun", 0) == 0 && g.blend > 0.96f)
            g.blend = 0.96f;
        g.glow = mat_is_lamp(mg.matdef.name);
        if (mat_is_light_src(mg.matdef.name)) {
            // prop : buf local -> monde via la matrice de placement
            Mat4 pm = _prop_mat(pi);
            for (auto& v : lamp_clusters(buf.data(),
                    (int)buf.size() / rnd::VF))
                _lamps.push_back({
                    pm.m[0]*v.x + pm.m[4]*v.y + pm.m[8]*v.z + pm.m[12],
                    pm.m[1]*v.x + pm.m[5]*v.y + pm.m[9]*v.z + pm.m[13],
                    pm.m[2]*v.x + pm.m[6]*v.y + pm.m[10]*v.z + pm.m[14]});
        }
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
        // nœud du group pour les anims (portes) : delta = anim * repos⁻¹ —
        // draws matshp : sommets déjà node-local, pas de nœud animable (-1)
        pi.mkey.push_back(ds.unx >= 0 ? -1 : bmd::group_key(mg.mat, mg.node));
        pi.minv.push_back(ds.unx >= 0 ? M4::ident() : mg.xf.inverse());
        // collision props : transformés par pos/rot/scale — dans
        // prop_tris (bloque le déplacement) PAS col_tris : la DS
        // n'utilise que les plaques BDHC pour la hauteur d'objet
        if (ct_world) {
            float cs = cosf(pi.ry), sn = sinf(pi.ry);
            for (size_t i = 0; i < ct.size(); i++) {
                Vec3 v{ct[i].x * pi.scale.x, ct[i].y * pi.scale.y,
                    ct[i].z * pi.scale.z};
                Vec3 r{v.x * cs + v.z * sn, v.y, -v.x * sn + v.z * cs};
                ct_world->push_back(r + pi.pos);
            }
        }
    }
    if (pi.meshes.empty()) {
        static std::unordered_set<int> warned;
        if (warned.insert(0x1000 + (int)lp.model).second)
            fprintf(stderr, "[prop] model %d : 0 mesh -> invisible\n",
                (int)lp.model);
    }
    return pi;
}

void World::_inst_props(Cell* cp) {
    Cell& c = *cp;
    if (!c.land) return;
    for (auto& lp : c.land->props) {
        std::vector<Vec3> ct;
        PropInst pi = _prop_make(lp, c.org, ckey(c.cx, c.cy), &ct);
        if (!pi.mdl) continue;
        c.prop_tris.insert(c.prop_tris.end(), ct.begin(), ct.end());
        _props.push_back(std::move(pi));
    }
}

// prop créé à l'exécution (nappe d'eau d'arène) : pas de triangles de
// collision (le MapProp DS n'est pas un collider terrain) et cell=-1 —
// il vit jusqu'au prochain _unload_all comme les autres.
int World::_spawn_prop(int model, const Vec3& pos) {
    LandProp lp;
    lp.model = (uint32_t)model;
    lp.pos = {0, 0, 0};
    lp.rot = {0, 0, 0};
    lp.scale = {1, 1, 1};
    PropInst pi = _prop_make(lp, pos, -1, nullptr);
    if (!pi.mdl) return -1;
    _props.push_back(std::move(pi));
    return (int)_props.size() - 1;
}

// ── features d'arène dynamiques (overlay008/gym_features.c DS) ───────────
// Pastoria : 3 boutons (props 239 bleu / 240 vert / 241 orange) règlent la
// nappe d'eau (prop 242) sur LOW/MIDDLE/HIGH = 0/32/64. L'opcode Init
// réinitialise la feature persistée à GREEN à chaque entrée (PersistedMap-
// Features_InitWithID vide le buffer DS) -> l'eau revient toujours MIDDLE.
namespace {
    // modèle prop -> (bouton DS, niveau cible)
    // DS : bleu->HIGH(64), vert->MIDDLE(32), orange->LOW(0)
    constexpr int GYM_BTN_MODEL[3] = {241, 240, 239};   // enum DS -> modèle
    constexpr int GYM_BTN_TARGET[3] = {0, 32, 64};      // orange/vert/bleu
}

// état visuel des boutons : pressé = fin de son anim PAT0 (enfoncé),
// relâché = frame 0. DS MapPropAnimation GoToLastFrame + paused à l'init ;
// à l'appui le bouton rejoue son enfoncement (one-shot) avant de figer.
void World::_gym_buttons(int pressed, bool animate) {
    for (auto& p : _props) {
        int btn = p.model == 239 ? 2 : p.model == 240 ? 1
            : p.model == 241 ? 0 : -1;
        if (btn < 0 || !p.pats) continue;
        int nfr = 0;
        for (auto& tp : *p.pats) nfr = std::max(nfr, tp.nframe);
        if (nfr <= 0) continue;
        if (btn == pressed) {
            p.panm_frame = animate ? 0.f : (float)(nfr - 1);
            p.panm_state = animate ? 1 : 2;
        } else {
            p.panm_frame = 0.f;
            p.panm_state = 2;               // relâché : figé sur frame haute
        }
    }
}

// Eterna : rotation des aiguilles (gym_features.c UpdateClockHandPositions) —
// heure : table[hh] (+91*min si fraction animée) +180°, minute : table[mm/5]
// si quart d'heure sinon mm*1093, -90° ; prop DS = rotation négée.
void World::_eterna_hands() {
    constexpr float IDX2RAD = 6.28318530718f / 65536.f;
    int m = (int)_gym.et_mm, h = (int)_gym.et_hh;
    if (_gym.et_hands[1] >= 0 && _gym.et_hands[1] < (int)_props.size()) {
        uint32_t rot = (m % 15 == 0) ? ET_HROT[m / 5] : (uint32_t)m * 1093;
        rot = (rot + 0x10000 - 0x4000) & 0xFFFF;
        _props[_gym.et_hands[1]].ry = (0x10000 - rot) * IDX2RAD;
    }
    if (_gym.et_hands[0] >= 0 && _gym.et_hands[0] < (int)_props.size()) {
        uint32_t rot = ET_HROT[h % 12];
        if (_gym.et_roth) rot += 91u * (uint32_t)m;
        rot = (rot + 0x8000) & 0xFFFF;
        _props[_gym.et_hands[0]].ry = (0x10000 - rot) * IDX2RAD;
    }
    if (getenv("WARPDBG"))
        printf("[et] h=%d m=%d ry_h=%.2f ry_m=%.2f\n", h, m,
            _gym.et_hands[0] >= 0 ? _props[_gym.et_hands[0]].ry : -1.f,
            _gym.et_hands[1] >= 0 ? _props[_gym.et_hands[1]].ry : -1.f);
}

// jet de fontaine Eterna : fldeff membre 111 (eterna_gym_fountain.nsbmd) avec
// l'anim PAT0 166 en boucle (gym04_hunsui_a) — comme Simple3D_CreateRenderObj
// WithAnim de la DS ; à l'arrêt -> anim 167 (hunsui_b) puis disparition.
int World::_fount_spawn(int tx) {
    Vec3 p{tx * TILE + 8.f, 0.f, 19 * TILE + 8.f};
    p.y = _ground_y({p.x, 40.f, p.z}, 80.f);
    if (p.y < -1e8f) p.y = 0.f;
    PropInst pi;
    pi.meshes = _fld_mesh(111);
    pi.mdl = _fld_mdl(111);
    pi.pats = _fld_pat(166);
    pi.panm_state = 0;                        // boucle (Simple3D_UpdateAnim loop)
    pi.pos = p; pi.model = -3;
    _props.push_back(pi);
    return (int)_props.size() - 1;
}

int World::_gym_op(int sub, int arg) {
    switch (sub) {
    case 0: {   // InitPersistedMapFeaturesForPastoriaGym (0x16F)
        _gym = GymDyn();
        _gym.kind = 1;
        // DS : pressedButton = GREEN à l'init -> nappe à MIDDLE
        _gym.plate_h = _gym.target = 32;
        _gym.water_prop = _spawn_prop(242,
            {256.f, (float)_gym.plate_h, 256.f});
        _gym_buttons(1, false);                 // vert enfoncé (GoToLastFrame)
        if (getenv("WARPDBG"))
            printf("[gym] pastoria init water=%d prop=%d\n",
                _gym.plate_h, _gym.water_prop);
        return 0;
    }
    case 1: {   // PressPastoriaGymButton (0x170) : hitbox 1x1 sur la tuile
        int ptx = (int)floorf(_ppos.x / TILE);
        int ptz = (int)floorf(_ppos.z / TILE);
        for (size_t i = 0; i < _props.size(); i++) {
            auto& p = _props[i];
            int btn = p.model == 239 ? 2 : p.model == 240 ? 1
                : p.model == 241 ? 0 : -1;
            if (btn < 0) continue;
            if ((int)floorf(p.pos.x / TILE) != ptx ||
                (int)floorf(p.pos.z / TILE) != ptz) continue;
            int target = GYM_BTN_TARGET[btn];
            if (target == _gym.plate_h) return 0;   // déjà à ce niveau
            _gym_buttons(btn, true);
            _gym.target = target;
            _gym.dir = target > _gym.plate_h ? 1 : -1;
            _gym.wait = 0.5f;                       // DS : fin anims boutons
            if (getenv("WARPDBG"))
                printf("[gym] bouton %s -> eau %d->%d\n",
                    btn == 2 ? "bleu" : btn == 1 ? "vert" : "orange",
                    _gym.plate_h, target);
            return 1;
        }
        return 0;
    }
    case 5: {   // InitPersistedMapFeaturesForSunyshoreGym (0x175), arg = roomID
        _gym = GymDyn();
        _gym.kind = 2;
        _gym.room = arg & 3;
        if (_gym.room > 2) _gym.room = 0;
        // DS : état par défaut de la salle ; entrée à entranceZ -> rot 0.
        // InitWithID vide le buffer : reset à chaque entrée de map.
        _gym.rot = SUN_DEFROT[_gym.room];
        if ((int)floorf(_ppos.z / TILE) == SUN_ENTZ[_gym.room]) _gym.rot = 0;
        // engrenages : pos tuile->monde (+8 centre), +8 y si vertical ;
        // angle = 90*initRot ± 90*state (sens selon turnsClockwise)
        _gym.num_gears = SUN_NGEARS[_gym.room];
        for (int i = 0; i < _gym.num_gears; i++) {
            const SunGear& g = SUN_GEARS[_gym.room][i];
            float ang = (float)(90 * g.init
                + (g.cw ? -1 : 1) * 90 * _gym.rot);
            int idx = _spawn_prop(g.model,
                {g.x * TILE + 8.f,
                 g.y * TILE + (g.vert ? 8.f : 0.f),
                 g.z * TILE + 8.f});
            if (idx >= 0) {
                float rad = ang * 3.14159265359f / 180.f;
                if (g.vert) _props[idx].rx = rad;
                else        _props[idx].ry = rad;
            }
            _gym.gears[i] = idx;
        }
        if (getenv("WARPDBG"))
            printf("[gym] sunyshore init room=%d rot=%d gears=%d\n",
                _gym.room, _gym.rot, _gym.num_gears);
        return 0;
    }
    case 6: {   // PressSunyshoreGymButton (0x176) : 0 normal, 1 reverse, 2 double
        if (_gym.kind != 2 || _gym.room < 0) return 0;
        if (arg == 1) {           // SUNYSHORE_GYM_BUTTON_REVERSE
            _gym.rot = (_gym.rot + 3) % 4;
            _gym.gear_goal = 90.f; _gym.gear_sign = -1;
        } else if (arg == 2) {    // SUNYSHORE_GYM_BUTTON_DOUBLE
            _gym.rot = (_gym.rot + 2) % 4;
            _gym.gear_goal = 180.f; _gym.gear_sign = 1;
        } else {                  // NORMAL
            _gym.rot = (_gym.rot + 1) % 4;
            _gym.gear_goal = 90.f; _gym.gear_sign = 1;
        }
        _gym.gear_prog = 0.f;
        // DS : l'état (donc la collision) bascule immédiatement,
        // l'animation des engrenages suit (5.625°/frame).
        if (getenv("WARPDBG"))
            printf("[gym] sunyshore bouton=%d -> rot=%d\n", arg, _gym.rot);
        return 1;
    }
    case 7: {   // InitPersistedMapFeaturesForEternaGym (0x2C9)
        _gym = GymDyn();
        _gym.kind = 3;
        // DS : l'état vient des SystemVars (persisté en save, pas remis
        // à zéro à l'entrée contrairement à Pastoria/Sunyshore).
        if (_eterna_clock > 4) _eterna_clock = 4;
        if (getenv("ETERNASTATE"))                  // debug : état forcé
            _eterna_clock = std::min(4, std::max(0,
                atoi(getenv("ETERNASTATE"))));
        // aiguilles : heure = modèle 537, minute = 536 ; pivot au centre
        // de la pendule (11,13), y=12/13 (superposition DS).
        _gym.et_hands[0] = _spawn_prop(537, {184.f, 12.f, 216.f});
        _gym.et_hands[1] = _spawn_prop(536, {184.f, 13.f, 216.f});
        const int* t = ET_TIME[_eterna_clock];
        _gym.et_hh = (float)t[0]; _gym.et_mm = (float)t[1];
        _eterna_hands();
        // fontaines : ov5_021F4098 (2=les deux côtés, 0=gauche seule ;
        // DS n'appelle rien pour état>=3 — elles restent arrêtées)
        int sides = _eterna_clock < 3 ? ET_FOUNT_INIT[_eterna_clock] : 255;
        if (sides != 255) {
            if (sides != 1) {           // gauche x1-4 (param != 1)
                for (int i = 0; i < 4; i++) _gym.et_fount[i] = _fount_spawn(1 + i);
            }
            if (sides != 0) {           // droite x18-21 (param != 0)
                for (int i = 0; i < 4; i++) _gym.et_fount[4 + i] = _fount_spawn(18 + i);
            }
        }
        if (getenv("WARPDBG"))
            printf("[gym] eterna init state=%d h=%d m=%d\n",
                _eterna_clock, t[0], t[1]);
        return 0;
    }
    case 8: {   // AdvanceEternaGymClock (0x2CA) : état++ + cinématique
        if (_gym.kind != 3) return 0;
        if (_eterna_clock >= 4) return 0;         // DEFEATED_GYM_LEADER
        _eterna_clock++;
        const int* t = ET_TIME[_eterna_clock];
        _gym.et_dh = (float)t[0]; _gym.et_dm = (float)t[1];
        // caméra : joueur -> centre pendule, axe le plus court d'abord
        _gym.et_cx = floorf(_ppos.x / TILE); _gym.et_cz = floorf(_ppos.z / TILE);
        _gym.et_tx = 11.f; _gym.et_tz = 13.f;
        _gym.et_xfirst = fabsf(_gym.et_tx - _gym.et_cx) < fabsf(_gym.et_tz - _gym.et_cz);
        _gym.et_t = 0.f; _gym.et_phase = 1; _gym.et_next = 2;
        _cam_mode = 1; _cam_x = _gym.et_cx; _cam_z = _gym.et_cz;
        if (getenv("WARPDBG"))
            printf("[gym] eterna avance -> state=%d (%d:%02d)\n",
                _eterna_clock, t[0], t[1]);
        return 1;
    }
    case 9: {   // InitPersistedMapFeaturesForCanalaveGym (0x173)
        // DS : InitWithID vide le buffer puis InitForCanalaveGym recharge
        // les startInPositionB -> reset complet à chaque entrée.
        _gym = GymDyn();
        _gym.kind = 4;
        _gym.cn_inB = CANA_START_B;
        // étages 2/3/4 : grands modèles centrés (256, 160/320/480, 256)
        for (int f = 0; f < 3; f++)
            _gym.cn_flr[f] = _spawn_prop(300 + f,   // membres 300/301/302
                {256.f, 160.f * (f + 1), 256.f});
        // 24 plateformes : pos tuile -> monde +8 (CANALAVE_XZ_OFFSET),
        // hauteur = y tuiles * 16 (DIST_BETWEEN_FLOORS=10 par étage)
        for (int i = 0; i < 24; i++) {
            const CanalavePlat& P = CANA_PLAT[i];
            bool inB = (CANA_START_B >> i) & 1;
            _gym.cn_pos[i][0] = inB ? P.bx : P.ax;
            _gym.cn_pos[i][1] = inB ? P.by : P.ay;
            _gym.cn_pos[i][2] = inB ? P.bz : P.az;
            _gym.cn_fl[i] = inB ? P.fb : P.fa;
            _gym.cn_plat[i] = _spawn_prop(P.model,
                {_gym.cn_pos[i][0] * TILE + TILE / 2.f,
                 _gym.cn_pos[i][1] * TILE,
                 _gym.cn_pos[i][2] * TILE + TILE / 2.f});
        }
        _cana_vis(_ppos.y);         // DS : UpdateVisibleProps(playerPos.y)
        if (getenv("WARPDBG"))
            printf("[gym] canalave init plat=%d flr=%d/%d/%d\n",
                24, _gym.cn_flr[0], _gym.cn_flr[1], _gym.cn_flr[2]);
        return 0;
    }
    case 10: {  // InitPersistedMapFeaturesForHearthomeGym (0x171)
        // DS : InitWithID vide le buffer -> l'énigme de portes est tirée
        // au hasard à chaque entrée (correctDoorID, clueX, clueZ).
        _gym = GymDyn();
        _gym.kind = 5;
        int ri = _header == 89 ? 0 : _header == 90 ? 1 : -1;   // salles 1/2
        if (ri >= 0) {
            const HearthomeRoom& R = HH_ROOM[ri];
            _gym.hh_door = R.firstDoor + (int)(rand() % R.numDoors);
            // indice : case valide (bitmap DS : 0 = posable) non occupée
            // par un objet (sub_0206326C vérifie la tuile)
            for (int t = 0; t < 64; t++) {
                _gym.hh_cluex = R.ox + rand() % R.sx;
                _gym.hh_cluez = R.oz + rand() % R.sz;
                if (R.clues[(_gym.hh_cluez - R.oz) * R.sx +
                            (_gym.hh_cluex - R.ox)] == 1) continue;
                bool occ = false;
                for (size_t oi = 0; oi < _objects.size(); oi++)
                    if ((int)floorf(_objpos[oi].x / TILE) == _gym.hh_cluex &&
                        (int)floorf(_objpos[oi].z / TILE) == _gym.hh_cluez)
                        { occ = true; break; }
                if (!occ) break;
            }
            // portes incorrectes : renvoi à l'entrée de l'arène
            // (MapHeaderData_SetWarpEventDestHeaderID/DestWarpID -> hdr 88 w1)
            for (int d = 0; d < R.numDoors; d++) {
                const int8_t* dr = HH_DOOR[ri][d];
                if (dr[0] == _gym.hh_door) continue;
                for (auto& w : _warps)
                    if (w.x == dr[1] && w.z == dr[2])
                        { w.dest = 88; w.dwarp = 1; }
            }
            // indice au sol : hint_<symbole> (fldeff 116+doorID), le DS
            // pose au centre de la tuile (-3 y, -4 z depuis RecalcPosHeight)
            Vec3 cp{_gym.hh_cluex * TILE + TILE / 2.f, 0.f,
                    _gym.hh_cluez * TILE + TILE / 2.f};
            cp.y = _ground_y(cp, 80.f);
            if (cp.y < -1e8f) cp.y = 0.f;
            PropInst cl;
            cl.meshes = _fld_mesh(116 + _gym.hh_door);
            cl.mdl = _fld_mdl(116 + _gym.hh_door);
            cl.pos = {cp.x, cp.y - 3.f, cp.z - 4.f};
            cl.model = -3;
            _props.push_back(cl);
            _gym.hh_clue = (int)_props.size() - 1;
        }
        // cônes de lampe (ov5_021F4840) : joueur portée fixe 2, dresseurs
        // data[0] (TRAINER_TYPE_NORMAL uniquement ; 0 = pas de cône)
        auto spawn_cone = [&](int oi, int range) {
            if (_gym.hh_nlt >= 9 || range <= 0) return;
            if (range > 5) range = 5;
            PropInst pi;
            pi.meshes = _fld_mesh(115);              // flashlight_cone.nsbmd
            pi.mdl = _fld_mdl(115);
            // DS : NNS_G3dMdlSetMdlFogEnableFlagAll(model, 0) — le cône
            // perce le brouillard noir (seul le cône est exempt, pas les indices)
            for (auto& g : pi.meshes) g.fog = false;
            pi.pats = _fld_pat(199);                 // .nsbta : balayage
            pi.panm_state = 0;                       // boucle (UpdateAnim DS)
            pi.pos = oi < 0 ? _ppos : _objpos[oi];
            pi.model = -3;
            int s = _gym.hh_nlt++;
            _gym.hh_lt[s] = (int)_props.size();
            _props.push_back(pi);
            _gym.hh_lt_obj[s] = oi;
            _gym.hh_lt_st[s] = range;
        };
        spawn_cone(-1, 2);
        for (size_t oi = 0; oi < _objects.size(); oi++)
            if (_objects[oi].trainer == 1 && _objects[oi].d0 != 0)
                spawn_cone((int)oi, _objects[oi].d0);
        // ascenseur DP : dans DP le prop iron_island_platform_lift (258)
        // était placé dans les données des salles ascenseur ; dans Platine
        // ces maps restantes partagent land232 (sans le prop) — on le
        // respawn donc sur la tuile du CoordEvent MoveLift (9,13), en
        // calant son empreinte 3x2 sur les tuiles libres autour (9-10,13-14)
        if (_header == 93 || _header == 95) {
            int i = _spawn_prop(258, {9.5f * TILE, 0.f, 13.5f * TILE});
            if (i >= 0) {
                _gym.hh_lift = i;
                _gym.hh_lift_y = _props[i].pos.y;
                _gym.hh_lift_tx0 = 9; _gym.hh_lift_tz0 = 13;
                _gym.hh_lift_tx1 = 10; _gym.hh_lift_tz1 = 14;
            }
        } else {
            // cartes où le prop est présent dans le land (Iron Island…) :
            // on le repère au cas où l'arène serait réutilisée
            for (size_t p = 0; p < _props.size(); p++)
                if (_props[p].model == 258) {
                    _gym.hh_lift = (int)p;
                    _gym.hh_lift_y = _props[p].pos.y;
                    _gym.hh_lift_tx0 = (int)floorf(_props[p].pos.x / TILE);
                    _gym.hh_lift_tz0 = (int)floorf(_props[p].pos.z / TILE);
                    _gym.hh_lift_tx1 = _gym.hh_lift_tx0;
                    _gym.hh_lift_tz1 = _gym.hh_lift_tz0;
                    break;
                }
        }
        if (getenv("WARPDBG"))
            printf("[gym] hearthome init hdr=%d door=%d clue=(%d,%d) "
                "cones=%d lift=%d\n", _header, _gym.hh_door, _gym.hh_cluex,
                _gym.hh_cluez, _gym.hh_nlt, _gym.hh_lift);
        return 0;
    }
    case 11: {  // MoveHearthomeGymDpLift (0x172) : ascenseur 0 <-> 160
        if (_gym.kind != 5 || _gym.hh_lift < 0 || _gym.hh_lift_move) return 0;
        bool up = _ppos.y < 80.f;      // DS : y == HEIGHT_DOWN -> RaiseLift
        _gym.hh_lift_move = up ? 1 : -1;
        _gym.hh_lift_dest = up ? 160.f : 0.f;
        if (_snd) {
            int s = _snd->seq_named("SEQ_SE_DP_ELEBETA");
            if (s >= 0) _snd->se(s);
        }
        if (getenv("WARPDBG"))
            printf("[gym] hearthome lift %s\n", up ? "monte" : "descend");
        return 1;
    }
    case 12: {  // SetMoveCodeForFacingDirection (0x020B) — dresseur après combat
        int i = _talk_obj;
        if (i < 0 || i >= (int)_objects.size()) return 0;
        if (_gym.kind == 5) {
            // HearthomeGym_SetTrainerPostBattleMovement DS : position
            // courante = nouveau centre, déambulation boîte 2x2.
            _objw[i].home = _objpos[i];
            _objects[i].rx = _objects[i].rz = 2;
            _objmv[i] = 3;                          // WANDER_AROUND
            return 1;
        }
        // VsSeeker_SetMoveCodeForFacingDirection : regarde dans sa
        // direction actuelle (LOOK_N/S/O/E = 14/15/16/17 DS)
        int d = _objd[i] & 3;                       // monde 0=S 1=N 2=O 3=E
        _objmv[i] = d < 2 ? 15 - d : d + 14;
        return 1;
    }
    case 13: {  // PersistedMapFeatures_InitForPlatformLift (0x25B)
        // platform_lift.c : 9 lifts — Iron Island (modèle 258, étage lu
        // depuis le warp d'entrée z) et ascenseurs de la Ligue (modèle 502,
        // preventGoingDown = sens unique). Le prop de la salle B2F gauche
        // (hdr 293) n'est pas dans son land : le DS le charge dynamiquement
        // au centre de la plaque (18,44)+3x2 -> monde (312,y,712).
        struct LiftCfg { int hdr, warpz, tx, tz, model, kind;
                         float y0, y1; bool nodown; };
        static const LiftCfg CFG[] = {
            {291, 26, 10, 23, 258, 0, 16, 144, false},  // B1F droite
            {293, 48, 18, 44, 258, 0, 16, 144, false},  // B2F gauche (spawn)
            {294, 15, 8,  11, 258, 0, 16, 144, false},  // B3F
            {176, 15, 3,  11, 502, 1, 0,  160, true},   // -> Aaron
            {178, 15, 3,  11, 502, 1, 0,  80,  true},   // -> Bertha
            {180, 15, 3,  11, 502, 1, 0,  80,  true},   // -> Flint
            {182, 15, 3,  11, 502, 1, 0,  80,  true},   // -> Lucian
            {184, 23, 3,  19, 502, 1, 0,  80,  true},   // -> Champion
            {185, 18, 7,  8,  502, 1, 0,  240, true},   // salle Champion
        };
        const LiftCfg* C = nullptr;
        for (auto& c : CFG) if (c.hdr == _header) { C = &c; break; }
        if (!C) return 0;
        _gym = GymDyn();
        _gym.kind = 6;
        int ptz = (int)floorf(_ppos.z / TILE);
        _gym.pl_floor = ptz == C->warpz ? 0 : 1;      // DS : location->z
        _gym.pl_y0 = C->y0; _gym.pl_y1 = C->y1;
        _gym.pl_y = _gym.pl_floor ? C->y1 : C->y0;
        _gym.pl_tx = C->tx; _gym.pl_tz = C->tz;
        _gym.pl_kind = C->kind;
        _gym.pl_nodown = C->nodown;
        // Ligue : arrivée par le haut -> lift déjà utilisé (check 0x25D)
        _gym.pl_notused = !(_gym.pl_kind && _gym.pl_floor == 1);
        if (_header == 293) {                          // spawn dynamique DS
            int i = _spawn_prop(C->model,
                {19.5f * TILE, _gym.pl_y, 44.5f * TILE});
            if (i >= 0) _gym.pl_lift = i;
        } else {
            for (size_t p = 0; p < _props.size(); p++)
                if (_props[p].model == C->model) {
                    _gym.pl_lift = (int)p;
                    _props[p].pos.y = _gym.pl_y;       // SetPosition DS
                    break;
                }
        }
        if (getenv("WARPDBG"))
            printf("[gym] lift init hdr=%d floor=%d y=%.0f prop=%d\n",
                _header, _gym.pl_floor, _gym.pl_y, _gym.pl_lift);
        return 0;
    }
    case 14: {  // TriggerPlatformLift (0x25C)
        if (_gym.kind != 6 || _gym.pl_lift < 0 || _gym.pl_move) return 0;
        if (_gym.pl_nodown && _gym.pl_floor == 1) return 0;
        bool up = _gym.pl_floor == 0;
        _gym.pl_move = up ? 1 : -1;
        _gym.pl_dest = up ? _gym.pl_y1 : _gym.pl_y0;
        _gym.pl_floor ^= 1;
        if (_snd) {
            int s = _snd->seq_named(_gym.pl_kind
                ? "SEQ_SE_DP_ELEBETA4" : "SEQ_SE_DP_ELEBETA");
            if (s >= 0) _snd->se(s);
        }
        if (getenv("WARPDBG"))
            printf("[gym] lift %s y=%.0f->%.0f\n",
                up ? "monte" : "descend", _gym.pl_y, _gym.pl_dest);
        return 1;
    }
    case 15:    // CheckPlatformLiftNotUsedWhenEnteredMap (0x25D)
        return _gym.kind == 6 && _gym.pl_notused ? 1 : 0;
    case 16: {  // InitGreatMarshTram (0x20E) — InitForGreatMarsh DS
        // DS : location remise à AREA_5_6 sauf si la feature active était
        // déjà le tram (transition de zone Marais->Marais : _gym survit,
        // contrairement à load_map qui passe par _unload_all). Le trajet en
        // cours (task DS indépendante) survit aussi à la transition.
        bool was = _gym.kind == 7;
        GymDyn old = _gym;
        _gym = GymDyn();
        _gym.kind = 7;
        int loc = 2, tram = -1;
        if (was) {
            _gym.gm_loc = old.gm_loc;
            _gym.gm_task = old.gm_task;
            _gym.gm_south = old.gm_south;
            _gym.gm_destz = old.gm_destz;
            _gym.gm_slowz = old.gm_slowz;
            _gym.gm_lvl = old.gm_lvl;
            _gym.gm_fr = old.gm_fr;
            loc = old.gm_loc; tram = old.gm_tram;
        }
        _gym.gm_loc = loc;
        _gym.gm_tram = tram;
        if (tram < 0 || tram >= (int)_props.size() ||
            _props[tram].model != 475) {
            // positions DS : x=1056 (tuile 66), y=16, z par arrêt
            static const float Z[3] = {16.f * 40, 16.f * 82, 16.f * 108};
            int i = _spawn_prop(475, {1056.f, 16.f, Z[loc]});
            _gym.gm_tram = i;
        }
        if (getenv("WARPDBG"))
            printf("[gym] tram init hdr=%d loc=%d prop=%d\n",
                _header, _gym.gm_loc, _gym.gm_tram);
        return 0;
    }
    case 17: {  // MoveGreatMarshTram (0x20F) arg = loc | type<<8 (3=CALL 4=RIDE)
        if (_gym.kind != 7 || _gym.gm_tram < 0) return 0;
        int dest = arg & 0xFF, type = arg >> 8;
        // DS MoveToLocation : (destPos, slowDownZ, isMovingSouth) calculés à
        // l'appel selon la location courante, tramState->location mis à jour
        // immédiatement — même quand la tâche est mise en file (InitCall).
        float dz = 0; int slow = 0; bool south;
        if (_gym.gm_loc == 0) {
            south = true;
            if (dest == 1) { dz = 16.f * 82; slow = 74; }
            else           { dz = 16.f * 108; slow = 100; }
        } else if (_gym.gm_loc == 1) {
            if (dest == 0) { south = false; dz = 16.f * 40; slow = 46; }
            else           { south = true;  dz = 16.f * 108; slow = 100; }
        } else {
            south = false;
            if (dest == 0) { dz = 16.f * 40; slow = 46; }
            else           { dz = 16.f * 82; slow = 88; }
        }
        _gym.gm_loc = dest;             // DS : mis à jour immédiatement
        if (_gym.gm_task) {                     // DS InitCall : en file
            _gym.gm_pend = type; _gym.gm_pdz = dz;
            _gym.gm_pslow = slow; _gym.gm_psouth = south;
            return 1;
        }
        _gm_start(dz, slow, south, type);
        return 1;
    }
    case 18:    // CheckGreatMarshTramLocation (0x210)
        // DS : GREAT_MARSH_TRAM_AT_LOCATION=5 / NOT_AT_LOCATION=6
        return _gym.kind == 7 && _gym.gm_loc == arg ? 5 : 6;
    case 19:    // SetPlayerHeightCalculationEnabled (0x211)
        _phcalc = arg != 0; return 0;
    case 20: {  // InitPersistedMapFeaturesForVilla (0x2F0) : spawn des
        // meubles possédés (flags 2461+type = FLAG_VILLA_FURNITURE_*),
        // prop 559+type à la position DS du slot.
        _gym = GymDyn(); _gym.kind = 8;
        int n = 0;
        for (const VillaFurn& f : VILLA_FURN)
            if (_vm.flag(2461 + f.t)) {
                _spawn_prop(559 + f.t, {f.x, 0.f, f.z});
                n++;
            }
        if (getenv("WARPDBG"))
            printf("[gym] villa init furn=%d\n", n);
        return 1;
    }
    // ── Monde Distorsion (ov9_02249960) ──────────────────────────────
    case 21:    // InitPersistedMapFeaturesForDistortionWorld (0x2F2)
        // DS : id=9 + memset(buffer,0) — inconditionnel à chaque OnTransition
        // de 1F ; les defaults sont re-posed par _dw_init au prochain étage
        _dw.persist_id = 9;
        _dw.hidden_gh = 0; _dw.cur_fp = 0;
        _dw.cax = _dw.cay = _dw.caz = 0;
        _dw.plat_flags = 0; _dw.puzz_flags = 0;
        _dw.valid = false;
        return 1;
    case 22: _dw_obj_add(arg); return 1;    // AddDistortionWorldMapObject
    case 23: _dw_obj_del(arg); return 1;    // DeleteDistortionWorldMapObject
    case 24:    // GetPlayer3DPos : coords MapObject DS (x,z tuiles, y
        // coordY = demi-tuiles) — espace partagé DW ; -9999 hors DW
        if (!_dw.on) return -9999;
        if (arg == 0) return _dw_shared_x();
        if (arg == 1) return _dw_shared_y() * 2;
        return _dw_shared_z();
    case 25:    // ResetDistortionWorldPersistedCameraAngles (0x31F)
        _dw.cax = _dw.cay = _dw.caz = 0;
        _dw.tgt[0] = _dw.tgt[1] = _dw.tgt[2] = 0;
        _dw.cang[0] = _dw.cang[1] = _dw.cang[2] = 0;
        _dw.tr = false;
        return 1;
    case 26:                                // DoDWWarp (0x320) : vortex DS
        _dwarp_t = 2.1f; _dwarp_sfx = false;
        return 1;
    case 27:                                // StartGiratinaShadowEvent (0x321)
        _dw_shadow_start(4);                // sGiratinaShadowExternal[0] DS
        return 1;
    case 28: _dw_shadow_stop(); return 1;   // FinishGiratinaShadowEvent (0x322)
    case 29: _gvl_init(); return 0;         // InitPersistedMapFeaturesForVeilstoneGym (0x174)
    }
    return 0;
}

// ===== gym Veilstone : sacs de frappe + pneus (overlay008/gym_features.c) =====
// sVeilstoneTileBehaviors DS 32x32 : 1 mur, 0 libre, 4 pause, 2 bloque
// N/S, 3 bloque O/E, 5 bloque O. Les objets DS sont créés en pos.z-2 —
// les coords ci-dessous sont déjà les positions logiques de tuile.
static const char* GVL_TB[32] = {
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111000400000000001111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11101111100004004001111111111111",
    "11101111111111111111111011111111",
    "11135000111111111111011011111111",
    "11130111111111111111011011111111",
    "11100000111111111111011011111111",
    "11110111111111111111011011111111",
    "11110111111111111111301011111111",
    "11100000011111111111301011111111",
    "11100111011111111111101011111111",
    "11100111011111111111101011111111",
    "11100000011111111111141011111111",
    "11110111111111111111101011111111",
    "11110111111111111111101011111111",
    "11111111111111111111101011111111",
    "11111111111111111111101011111111",
    "11111111111111111111111011111111",
    "11111111111111111100000111011111111",
    "11111111111111111111111011111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
    "11111111111111111111111111111111",
};
// sVeilstonePunchingBags / sVeilstoneTireStacks : pos.z-2 (tuile logique)
static const int8_t GVL_BAGS[9][2] = {
    {3,10},{4,22},{8,7},{8,20},{15,26},{16,10},{20,17},{21,24},{23,11}};
static const int8_t GVL_STKS[11][2] = {
    {3,21},{8,10},{8,12},{8,14},{12,7},{12,10},
    {19,10},{20,11},{20,26},{22,7},{23,28}};

int World::_gvl_flags(int x, int z, int dir) const {
    // VeilstoneGym_GetTileFlags : comportement de la tuile courante puis
    // mur/pile sur la tuile suivante. dirs monde : 0=S 1=N 2=O 3=E.
    int b = (x < 0 || z < 0 || x >= 32 || z >= 32) ? -1 : GVL_TB[z][x] - '0';
    if (b == 4) return 4;                             // PAUSE (ligne 4=avant-dernière?)
    if ((b == 2 && dir <= 1) || (b == 3 && dir >= 2) || (b == 5 && dir == 2))
        return 1;
    int dx, dz; dirv(dir, dx, dz);
    int nx = x + dx, nz = z + dz;
    if (nx >= 0 && nz >= 0 && nx < 32 && nz < 32 && GVL_TB[nz][nx] == '1')
        return 1;
    for (int i = 0; i < 11; i++)
        if (_gstacks[i].exists && _gstacks[i].x == nx && _gstacks[i].z == nz)
            return 2;                               // stack touché
    return 0;
}

int World::_gvl_bag_obj(int oi) const {
    if (!_gym_vl) return -1;
    for (int i = 0; i < 9; i++)
        if (_gbags[i].exists && _gbags[i].obj == oi) return i;
    return -1;
}

void World::_gvl_init() {
    if (_header != 133) return;                     // VEILSTONE_CITY_GYM
    _gym_vl = true; _gbanim.bag = -1;
    for (auto& b : _gbags) if (b.obj >= 0) _objgone.insert(b.obj);
    for (auto& s : _gstacks) if (s.obj >= 0) _objgone.insert(s.obj);
    auto spawn = [this](int tx, int tz, int gfx, int id) -> int {
        Nds::Zev::Obj o = {};
        o.id = (uint16_t)id; o.gfx = (uint16_t)gfx;
        o.x = (uint16_t)tx; o.z = (uint16_t)tz; o.y = 0;
        _objects.push_back(o);
        Vec3 p{tx * TILE + TILE / 2.f, 0.f, tz * TILE + TILE / 2.f};
        float g0 = _ground_y(p, 40.f);
        p.y = g0 > -1e8f ? g0 : 0.f;
        _objpos.push_back(p); _objd.push_back(0); _objmv.push_back(0);
        _objw.push_back({});
        _objw.back().home = p;
        return (int)_objects.size() - 1;
    };
    for (int i = 0; i < 9; i++)
        _gbags[i] = {GVL_BAGS[i][0], GVL_BAGS[i][1], true,
                     spawn(GVL_BAGS[i][0], GVL_BAGS[i][1], 950, 9600 + i)};
    for (int i = 0; i < 11; i++)
        _gstacks[i] = {GVL_STKS[i][0], GVL_STKS[i][1], true,
                       spawn(GVL_STKS[i][0], GVL_STKS[i][1], 951, 9620 + i)};
}

bool World::_gvl_kick(int oi, int dir) {
    int bi = _gvl_bag_obj(oi);
    if (bi < 0) return false;
    if (_gbanim.bag >= 0) return true;              // un sac roule déjà
    // VeilstoneGym_CalculateDistanceBagWillTravel : avance tuile par tuile
    // tant que flags==0 ; PAUSE au départ n'empêche pas le mouvement.
    int x = _gbags[bi].x, z = _gbags[bi].z, stack = -1;
    int f = _gvl_flags(x, z, dir);
    if (f != 0 && f != 4) return true;              // poussée absorbée
    int dx, dz; dirv(dir, dx, dz);
    for (;;) {
        x += dx; z += dz;
        f = _gvl_flags(x, z, dir);
        if (f != 0) break;
    }
    if (f == 2) {                                   // pile de pneus en x+1
        for (int i = 0; i < 11; i++)
            if (_gstacks[i].exists && _gstacks[i].x == x + dx
                && _gstacks[i].z == z + dz) { stack = i; break; }
    }
    _gbanim.bag = bi; _gbanim.dir = dir; _gbanim.stack = stack;
    int oj = _gbags[bi].obj;
    _gbanim.sx = _objpos[oj].x; _gbanim.sz = _objpos[oj].z;
    _gbanim.tx = x * TILE + TILE / 2.f; _gbanim.tz = z * TILE + TILE / 2.f;
    _gbanim.t = 0;
    _gbags[bi].x = x; _gbags[bi].z = z;
    _objects[oj].x = (uint16_t)x; _objects[oj].z = (uint16_t)z;
    if (_snd) {
        int s = _snd->seq_named("SEQ_SE_DP_TELE2");
        if (s >= 0) _snd->se(s);
    }
    return true;
}

void World::_gvl_tick(float dt) {
    if (_gbanim.bag < 0) return;
    int oj = _gbags[_gbanim.bag].obj;
    float tiles = fabsf(_gbanim.tx - _gbanim.sx) + fabsf(_gbanim.tz - _gbanim.sz);
    float dur = tiles > 0 ? tiles / TILE * 0.14f : 0.01f;   // ~7 tuiles/s
    _gbanim.t += dt;
    float k = _gbanim.t >= dur ? 1.f : _gbanim.t / dur;
    _objpos[oj].x = _gbanim.sx + (_gbanim.tx - _gbanim.sx) * k;
    _objpos[oj].z = _gbanim.sz + (_gbanim.tz - _gbanim.sz) * k;
    if (k < 1.f) return;
    if (_gbanim.stack >= 0) {           // renversement : la pile disparaît
        _gstacks[_gbanim.stack].exists = false;
        _objgone.insert(_gstacks[_gbanim.stack].obj);
        if (_snd) {
            int s = _snd->seq_named("SEQ_SE_DP_WALL_HIT2");
            if (s >= 0) _snd->se(s);
        }
    }
    _gbanim.bag = -1;
}

// great_marsh_tram.c MoveTram : lance un déplacement (CALL autonome ou
// RIDE qui porte le joueur). Les tâches se chaînent via gm_pend (InitCall).
void World::_gm_start(float dz, int slow, bool south, int type) {
    _gym.gm_south = south; _gym.gm_destz = dz; _gym.gm_slowz = slow;
    _gym.gm_lvl = 0; _gym.gm_fr = 0;
    _gym.gm_task = type == 3 ? 1 : 2;
    _gm_sfx = -1; _gm_sfx_t = 2.5f;               // plafond de l'attente SFX
    if (type != 3 && _snd) {                      // RIDE : TRAIN04 avant départ
        _gm_sfx = _snd->seq_named("SEQ_SE_DP_TRAIN04");
        if (_gm_sfx >= 0) _snd->se(_gm_sfx);
        else _gym.gm_task = 3;
    }
    if (getenv("WARPDBG"))
        printf("[gym] tram %s z=%.0f slow=%d\n",
            type == 3 ? "call" : "ride", dz, slow);
}

// HearthomeGym_CheckIfEnteredIncorrectDoor DS : pousser NORD dans une
// porte qui n'est pas la porte correcte retourne le joueur (transition
// forcée SUD — combinée au warp réécrit vers l'entrée à l'init).
int World::_hh_door_dir(int tx, int tz) {
    if (_gym.kind != 5 || _gym.hh_door == 8 || _pdir != 1) return _pdir;
    int ri = _header == 89 ? 0 : _header == 90 ? 1 : -1;
    if (ri < 0) return _pdir;
    for (int d = 0; d < HH_ROOM[ri].numDoors; d++) {
        const int8_t* dr = HH_DOOR[ri][d];
        if (dr[1] == tx && dr[2] == tz && dr[0] == _gym.hh_door)
            return _pdir;                       // porte correcte : passe
    }
    return 0;                                   // DIR_SOUTH : retourné
}

// CanalaveGym_UpdateVisibleProps : les étages supérieurs (et leurs
// plateformes) ne sont révélés qu'à la montée d'une plateforme.
void World::_cana_vis(float py) {
    for (int f = 0; f < 3; f++) {
        bool vis = py >= TILE + f * 160.f;      // 16 / 176 / 336
        if (_gym.cn_flr[f] >= 0) _props[_gym.cn_flr[f]].hidden = !vis;
        for (int i = 0; i < 24; i++)
            if (_gym.cn_fl[i] == f + 1 && _gym.cn_plat[i] >= 0)
                _props[_gym.cn_plat[i]].hidden = !vis;
    }
}

// déplacement de la nappe : 1 unité/tick@30Hz (DS FX32_ONE par update) ;
// la hauteur de plaque ne bascule qu'une fois la cible atteinte.
// Sunyshore : 5.625°/frame @60fps (FieldTask) jusqu'à 90/180°.
void World::_gym_tick(float dt) {
    if (_gym.kind == 1) {
        if (_gym.wait > 0.f) { _gym.wait -= dt; return; }
        if (_gym.water_prop < 0 || _gym.water_prop >= (int)_props.size()) return;
        if (_gym.plate_h == _gym.target) return;
        auto& p = _props[_gym.water_prop];
        p.pos.y += _gym.dir * 30.f * dt;
        float done = _gym.dir > 0 ? (float)_gym.target - p.pos.y
            : p.pos.y - (float)_gym.target;
        if (done <= 0.f) {
            p.pos.y = (float)_gym.target;
            _gym.plate_h = _gym.target;   // SetHeight DS : collision bascule
            if (getenv("WARPDBG"))
                printf("[gym] nappe stabilisée a %d\n", _gym.plate_h);
        }
        return;
    }
    if (_gym.kind == 2) {
        if (_gym.gear_prog >= _gym.gear_goal) return;
        float step = 5.625f * 60.f * dt;
        if (_gym.gear_prog + step > _gym.gear_goal)
            step = _gym.gear_goal - _gym.gear_prog;
        _gym.gear_prog += step;
        for (int i = 0; i < _gym.num_gears && i < 13; i++) {
            int idx = _gym.gears[i];
            if (idx < 0 || idx >= (int)_props.size()) continue;
            const SunGear& g = SUN_GEARS[_gym.room][i];
            // DS : step négatif pour REVERSE, re-négatif pour turnsClockwise
            float d = step * (float)_gym.gear_sign * (g.cw ? -1.f : 1.f)
                * 3.14159265359f / 180.f;
            if (g.vert) _props[idx].rx += d;
            else        _props[idx].ry += d;
        }
        return;
    }
    if (_gym.kind == 4) {
        // Canalave : trajet de plateforme — vertical porte le joueur,
        // horizontal suit le joueur (FieldTask_*UpDown/EastWest/NorthSouth)
        int i = _gym.cn_move;
        if (i < 0 || _gym.cn_plat[i] < 0) return;
        PropInst& P = _props[_gym.cn_plat[i]];
        const CanalavePlat& T = CANA_PLAT[i];
        bool done = false;
        if (_gym.cn_dir <= 1) {                    // UP/DOWN : portage
            P.pos.y += (_gym.cn_dir == 0 ? 480.f : -480.f) * dt;
            done = _gym.cn_dir == 0 ? P.pos.y >= _gym.cn_dest
                                    : P.pos.y <= _gym.cn_dest;
            if (done) { P.pos.y = _gym.cn_dest;
                        if (_gym.cn_dir == 0) _gym.cn_fl[i] = T.fb; }
                        // DOWN : cn_fl = floorA dès le départ (trigger)
            _ppos.y = P.pos.y;                     // Player_SetYPos DS
            _cana_vis(P.pos.y);
        } else if (_gym.cn_dir == 4 || _gym.cn_dir == 5) {   // E/O
            P.pos.x = _ppos.x;                     // platformPos.x = player.x
            done = _gym.cn_dir == 4 ? _ppos.x >= _gym.cn_dest
                                    : _ppos.x <= _gym.cn_dest;
            if (done) P.pos.x = _gym.cn_dest;
        } else {                                   // N/S
            P.pos.z = _ppos.z;
            done = _gym.cn_dir == 3 ? _ppos.z >= _gym.cn_dest
                                    : _ppos.z <= _gym.cn_dest;
            if (done) P.pos.z = _gym.cn_dest;
        }
        if (done) {
            _gym.cn_move = -1;
            if (_snd) {                          // thud d'arrivée (DS)
                int s = _snd->seq_named("SEQ_SE_DP_KI_GASYAN");
                if (s >= 0) _snd->se(s);
            }
            if (getenv("WARPDBG")) {
                int hid = 0;
                for (int f = 0; f < 3; f++)
                    if (_gym.cn_flr[f] >= 0 && _props[_gym.cn_flr[f]].hidden)
                        hid |= 1 << f;
                int phid = 0;
                for (int k = 0; k < 24; k++)
                    if (_gym.cn_plat[k] >= 0 && _props[_gym.cn_plat[k]].hidden)
                        phid++;
                printf("[gym] canalave plat %d arrivee (%.0f,%.0f,%.0f) "
                    "flr_hidden=%d plat_hidden=%d\n",
                    i, P.pos.x, P.pos.y, P.pos.z, hid, phid);
            }
        }
        return;
    }
    if (_gym.kind == 5) {
        // Hearthome : (a) ascenseur DP — FX32_ONE/frame@60 + portage joueur
        // (HearthomeGymDP_RaiseLift/LowerLift) ; (b) cônes de lampe suivant
        // les objets avec lerp table contre les murs (ov5_021F4908/4B6C).
        if (_gym.hh_lift_move && _gym.hh_lift >= 0) {
            PropInst& L = _props[_gym.hh_lift];
            _gym.hh_lift_y += _gym.hh_lift_move * 60.f * dt;
            bool done = _gym.hh_lift_move > 0
                ? _gym.hh_lift_y >= _gym.hh_lift_dest
                : _gym.hh_lift_y <= _gym.hh_lift_dest;
            if (done) {
                _gym.hh_lift_y = _gym.hh_lift_dest;
                _gym.hh_lift_move = 0;
                if (_snd) {                 // DS : KI_GASYAN à l'arrivée
                    int s = _snd->seq_named("SEQ_SE_DP_KI_GASYAN");
                    if (s >= 0) _snd->se(s);
                }
                if (getenv("WARPDBG"))
                    printf("[gym] hearthome lift y=%.0f\n", _gym.hh_lift_y);
            }
            L.pos.y = _gym.hh_lift_y;
            _ppos.y = _gym.hh_lift_y;               // Player_SetYPos DS
        }
        // cônes : direction + portée par objet ; si la course est libre sur
        // toute la portée -> preset [range], sinon lerp ov5_021F4B6C entre
        // les presets voisins selon la distance exacte au mur.
        static const int DDX[4] = {0, 0, -1, 1};    // dir DS : N,S,O,E
        static const int DDZ[4] = {-1, 1, 0, 0};
        for (int s = 0; s < _gym.hh_nlt; s++) {
            int pidx = _gym.hh_lt[s];
            if (pidx < 0 || pidx >= (int)_props.size()) continue;
            PropInst& C = _props[pidx];
            int oi = _gym.hh_lt_obj[s];
            if (oi >= 0 && (oi >= (int)_objects.size() ||
                _objgone.count(oi))) { C.hidden = true; continue; }
            C.hidden = false;
            Vec3 bp = oi < 0 ? _ppos : _objpos[oi];
            int dir = oi < 0 ? _pdir : (_objd[oi] & 3);
            int dd = HH_DIR[dir];
            int range = _gym.hh_lt_st[s];
            if (range < 1) { C.hidden = true; continue; }
            // ov5_021F4AB4 : avance case par case jusqu'à mur/objet/portée
            int bx = (int)floorf(bp.x / TILE), bz = (int)floorf(bp.z / TILE);
            int free_n = 0;
            float ex = bp.x + DDX[dd] * range * TILE;      // bout libre
            float ez = bp.z + DDZ[dd] * range * TILE;
            for (int k = 1; k <= range; k++) {
                int tx = bx + DDX[dd] * k, tz = bz + DDZ[dd] * k;
                bool occ = false;                        // objet bloquant
                for (size_t j = 0; j < _objects.size(); j++) {
                    if ((int)j == oi || _objgone.count((int)j)) continue;
                    if ((int)floorf(_objpos[j].x / TILE) == tx &&
                        (int)floorf(_objpos[j].z / TILE) == tz) {
                        ex = _objpos[j].x; ez = _objpos[j].z;
                        occ = true; break;
                    }
                }
                if (occ) break;
                if (_blocked({tx * TILE + TILE / 2.f, bp.y + 4.f,
                              tz * TILE + TILE / 2.f})) {
                    ex = tx * TILE + TILE / 2.f;         // tuile du mur
                    ez = tz * TILE + TILE / 2.f;
                    break;
                }
                free_n = k;
            }
            float off[3], scl[3];
            if (free_n >= range) {          // course libre : preset direct
                for (int c = 0; c < 3; c++) {
                    off[c] = HH_CONE_POS[dd][range][c];
                    scl[c] = HH_CONE_SCL[dd][range][c];
                }
            } else {                        // lerp DS vers le mur
                float fwd = dd == 0 ? bp.z - ez : dd == 1 ? ez - bp.z
                        : dd == 2 ? bp.x - ex : ex - bp.x;
                int v1 = (int)(fabsf(fwd) / TILE);
                if (v1 > range) v1 = range;
                if (--v1 < 0) v1 = 0;
                int v2 = v1;
                float rem = fmodf(fwd, TILE);
                if (rem < 0) v2--; else if (rem > 0) v2++;
                if (v2 < 0) v2 = 0; else if (v2 > range) v2 = range;
                float v3 = fabsf(rem / (TILE / 10.f));    // dixièmes DS
                for (int c = 0; c < 3; c++) {
                    off[c] = HH_CONE_POS[dd][v1][c]
                        + (HH_CONE_POS[dd][v2][c] - HH_CONE_POS[dd][v1][c])
                        / 10.f * v3;
                    scl[c] = HH_CONE_SCL[dd][v1][c]
                        + (HH_CONE_SCL[dd][v2][c] - HH_CONE_SCL[dd][v1][c])
                        / 10.f * v3;
                }
            }
            C.pos = {bp.x + off[0], bp.y + off[1] + 16.f, bp.z + off[2]};
            C.scale = {scl[0], scl[1], scl[2]};
            // rotation DS (Simple3DRotationAngles alpha,beta,gamma =
            // Rot33Angles : Rx puis Ry puis Rz -> notre M = Rz·Ry·Rx)
            C.rx = HH_CONE_ROT[dd][0] * 3.14159265359f / 180.f;
            C.ry = HH_CONE_ROT[dd][1] * 3.14159265359f / 180.f;
            C.rz = HH_CONE_ROT[dd][2] * 3.14159265359f / 180.f;
            if (getenv("HHDBG") && s == 0)
                printf("[hh] s%d dir=%d dd=%d free=%d bp=(%.0f,%.0f,%.0f) pos=(%.0f,%.0f,%.0f) "
                       "scl=(%.2f,%.2f,%.2f) rot=(%.0f,%.0f,%.0f) hid=%d meshes=%d blend=%.2f\n",
                       s, dir, dd, free_n, bp.x, bp.y, bp.z,
                       C.pos.x, C.pos.y, C.pos.z,
                       C.scale.x, C.scale.y, C.scale.z,
                       C.rx * 57.3f, C.ry * 57.3f, C.rz * 57.3f, C.hidden,
                       (int)C.meshes.size(),
                       C.meshes.empty() ? -1.f : C.meshes[0].blend);
        }
        return;
    }
    if (_gym.kind == 6) {
        // platform_lift.c FieldTask GoUp/GoDown : iron ±FX32_ONE/frame,
        // ligue +2 en montée (descente toujours -1) ; le joueur est porté
        // (Player_SetYPos) puis la plaque 0 prend la hauteur finale.
        if (_gym.pl_move && _gym.pl_lift >= 0) {
            float spd = _gym.pl_move > 0
                ? (_gym.pl_kind ? 120.f : 60.f) : 60.f;
            _gym.pl_y += _gym.pl_move * spd * dt;
            bool done = _gym.pl_move > 0
                ? _gym.pl_y >= _gym.pl_dest : _gym.pl_y <= _gym.pl_dest;
            if (done) {
                _gym.pl_y = _gym.pl_dest;
                _gym.pl_move = 0;
                if (_snd) {                     // thud DS à l'arrivée
                    int s = _snd->seq_named("SEQ_SE_DP_KI_GASYAN");
                    if (s >= 0) _snd->se(s);
                }
                if (getenv("WARPDBG"))
                    printf("[gym] lift y=%.0f\n", _gym.pl_y);
            }
            _props[_gym.pl_lift].pos.y = _gym.pl_y;
            _ppos.y = _gym.pl_y;                // Player_SetYPos DS
        }
        return;
    }
    if (_gym.kind == 7) {
        // great_marsh_tram.c MoveTram : CALL = déplacement autonome avec
        // décélération par paliers ; RIDE = le joueur marche forcé (vitesses
        // sRidingTram*Movements) et le tram suit player.z + 8.
        if (_gym.gm_tram < 0 || _gym.gm_tram >= (int)_props.size()) return;
        PropInst& T = _props[_gym.gm_tram];
        static const float CSPD[7] = {240, 120, 120, 60, 60, 30, 15}; // u/30t
        static const float CFR[7]  = {2, 4, 4, 8, 8, 16, 32};         // ticks
        if (_gym.gm_task == 1) {                       // CALL
            float z = T.pos.z
                + (_gym.gm_south ? 1.f : -1.f) * CSPD[_gym.gm_lvl] * dt;
            int tt = (int)((z - 8.f) / TILE);          // tuile tram DS
            if (_gym.gm_lvl < 6 &&
                (_gym.gm_south ? tt >= _gym.gm_slowz
                               : tt <= _gym.gm_slowz)) {
                _gym.gm_fr += dt * 30.f;
                if (_gym.gm_fr >= CFR[_gym.gm_lvl]) {
                    if (_gym.gm_lvl == 0 && _snd) {    // DS : TRAIN03 au 1er palier
                        int s = _snd->seq_named("SEQ_SE_DP_TRAIN03");
                        if (s >= 0) _snd->se(s);
                    }
                    _gym.gm_lvl++; _gym.gm_fr = 0;
                }
            }
            if ((_gym.gm_south && z >= _gym.gm_destz) ||
                (!_gym.gm_south && z <= _gym.gm_destz)) {
                z = _gym.gm_destz; _gym.gm_task = 0;
                if (getenv("WARPDBG"))
                    printf("[gym] tram arrive z=%.0f loc=%d\n", z, _gym.gm_loc);
            }
            T.pos.z = z;
        } else if (_gym.gm_task == 2) {                // RIDE : attente TRAIN04
            _gm_sfx_t -= dt;
            if (getenv("WARPDBG"))
                printf("[gm] wait sfx=%d busy=%d t=%.2f\n", _gm_sfx,
                    _snd ? (int)_snd->se_busy(_gm_sfx) : -1, _gm_sfx_t);
            if (_gm_sfx < 0 || !_snd || !_snd->se_busy(_gm_sfx) ||
                _gm_sfx_t <= 0.f)
                _gym.gm_task = 3;
        } else if (_gym.gm_task == 3) {                // RIDE
            // DS : SetAnimationCode walk[speedLevel] sur le joueur — il
            // marche le long du rail et le tram suit player.z + 8. Le VM
            // attend WaitMovement pendant le trajet -> déplacement direct.
            static const float RSPD[7] = {30, 60, 120, 120, 240, 240, 480};
            _ppos.z += (_gym.gm_south ? 1.f : -1.f) * RSPD[_gym.gm_lvl] * dt;
            _pdir = _gym.gm_south ? 0 : 1;
            _pstep = 0;                              // pas de step machine ici
            _panim += dt * 8.f;
            if (_panim > 4.f) _panim -= 4.f;
            // arrivée : le tram a gagné la gare (T.z == destz <=> pz == dest-8)
            float target = _gym.gm_destz - 8.f;
            if ((_gym.gm_south && _ppos.z >= target) ||
                (!_gym.gm_south && _ppos.z <= target))
                _ppos.z = target;
            T.pos.z = _ppos.z + 8.f;
            int pz = (int)floorf(_ppos.z / TILE);      // PlayerAvatar_GetZPos
            bool far = _gym.gm_south ? pz < _gym.gm_slowz
                                     : pz > _gym.gm_slowz;
            if (_gym.gm_lvl < 6 && far) {
                if (_gym.gm_lvl == 0 && _snd) {        // DS : TRAIN02 accélération
                    int s = _snd->seq_named("SEQ_SE_DP_TRAIN02");
                    if (s >= 0) _snd->se(s);
                }
                _gym.gm_lvl++;
            } else if (!far && _gym.gm_lvl > 0) {
                if (_gym.gm_lvl == 6 && _snd) {        // DS : TRAIN03 au 1er ralentissement
                    int s = _snd->seq_named("SEQ_SE_DP_TRAIN03");
                    if (s >= 0) _snd->se(s);
                }
                _gym.gm_lvl--;
            }
            if (T.pos.z == _gym.gm_destz) {
                _gym.gm_task = 0;
                if (getenv("WARPDBG"))
                    printf("[gym] tram ride arrive z=%.0f loc=%d\n",
                        T.pos.z, _gym.gm_loc);
            }
        }
        if (_gym.gm_task == 0 && _gym.gm_pend >= 0) {   // DS : tâche en file
            int t = _gym.gm_pend; _gym.gm_pend = -1;
            _gm_start(_gym.gm_pdz, _gym.gm_pslow, _gym.gm_psouth, t);
        }
        return;
    }
    if (_gym.kind != 3 || _gym.et_phase == 0) return;
    // Eterna : FieldTask_EternaGym_AdvanceClockState DS —
    // travelling caméra (60px/s, axe court d'abord) -> délai -> remontée
    // du temps (2 min/frame@60) -> fontaines -> retour caméra.
    auto camstep = [&]() -> bool {
        if (_gym.et_cx == _gym.et_tx && _gym.et_cz == _gym.et_tz) return true;
        float sp = 60.f * dt / TILE;                  // WALK_NORMAL DS (tuiles)
        // DS : axe le plus court d'abord, puis l'autre
        bool mx = _gym.et_xfirst ? (_gym.et_cx != _gym.et_tx)
            : (_gym.et_cz == _gym.et_tz);
        float& c = mx ? _gym.et_cx : _gym.et_cz;
        float d = (mx ? _gym.et_tx : _gym.et_tz) - c;
        c += fabsf(d) <= sp ? d : copysignf(sp, d);
        _cam_x = _gym.et_cx; _cam_z = _gym.et_cz;
        return _gym.et_cx == _gym.et_tx && _gym.et_cz == _gym.et_tz;
    };
    switch (_gym.et_phase) {
    case 1:                                         // cam joueur -> pendule
        if (camstep()) { _gym.et_phase = _gym.et_next; _gym.et_t = 0; }
        break;
    case 2:                                         // 8 frames @60, tic SE
        _gym.et_t += dt;
        if (_gym.et_t >= 8.f / 60.f) { _gym.et_phase = 3; _gym.et_t = 0; }
        break;
    case 3: {                                       // remontée du temps
        bool leader = _eterna_clock == 4;
        float oldm = _gym.et_mm;
        if (leader && _gym.et_hh == _gym.et_dh && _gym.et_mm > _gym.et_dm)
            _gym.et_hh = 11.f;                      // DS : force 11h
        // IncrementClockTime : +2 min/frame @60, report sur l'heure
        _gym.et_mm += 120.f * dt;
        while (_gym.et_mm >= 60.f) { _gym.et_mm -= 60.f; _gym.et_hh += 1.f; }
        while (_gym.et_mm < 0.f)   { _gym.et_mm += 60.f; _gym.et_hh += 1.f; }
        _gym.et_hh = fmodf(_gym.et_hh, 12.f);
        // clamp quand la cible est dépassée dans la bonne heure
        if (_gym.et_hh == _gym.et_dh) {
            float nm = _gym.et_mm, dm = _gym.et_dm;
            if (oldm > nm) {
                nm += 60.f; dm += 60.f;
                if (oldm < dm && nm > dm) _gym.et_mm = _gym.et_dm;
            } else if (oldm < dm && nm > dm) _gym.et_mm = _gym.et_dm;
        }
        if (leader) _gym.et_hh = _gym.et_dh;        // heure figée côté DS
        _gym.et_roth = _gym.et_hh != _gym.et_dh ||
            (_gym.et_mm > _gym.et_dm && !leader);
        _eterna_hands();
        if (_gym.et_hh == _gym.et_dh && _gym.et_mm == _gym.et_dm) {
            _gym.et_phase = 4; _gym.et_t = 0;
        }
        break;
    }
    case 4:                                         // 8 frames @60
        _gym.et_t += dt;
        if (_gym.et_t < 8.f / 60.f) break;
        _gym.et_t = 0;
        if (_eterna_clock == 2 || _eterna_clock == 3) {
            // DS : cam vers (11,19) puis la fontaine du bon côté
            _gym.et_tx = 11.f; _gym.et_tz = 19.f;
            _gym.et_xfirst = 0;                     // Z d'abord (sud)
            _gym.et_phase = 7; _gym.et_next = 8;
        } else {                                    // retour joueur
            _gym.et_tx = floorf(_ppos.x / TILE);
            _gym.et_tz = floorf(_ppos.z / TILE);
            _gym.et_xfirst ^= 1;                    // trajet inversé DS
            _gym.et_phase = 5; _gym.et_next = 6;
        }
        break;
    case 5:                                         // cam retour joueur
        if (camstep()) { _gym.et_phase = _gym.et_next; _gym.et_t = 0; }
        break;
    case 6:                                         // fin : caméra joueur
        _cam_mode = 0; _gym.et_phase = 0;
        break;
    case 7:                                         // cam pendule -> (11,19)
        if (camstep()) {
            _gym.et_tx = _eterna_clock == 3 ? 2.f : 20.f;
            _gym.et_xfirst = 1;                     // X d'abord (traverse)
            _gym.et_phase = 8; _gym.et_t = 0;
        }
        break;
    case 8:                                         // cam -> fontaine
        if (camstep()) { _gym.et_phase = 9; _gym.et_t = 0; }
        break;
    case 9:                                         // 4 frames puis arrêt
        _gym.et_t += dt;
        if (_gym.et_t >= 4.f / 60.f) {
            _gym.et_t = 0; _gym.et_phase = 10;
            // ov5_021F416C : 2nd dresseur = droite(1), 3e = gauche(0) ;
            // l'anim courante est libérée et remplacée par l'arrêt
            // (eterna_gym_fountain_stopping = fldeff 167, one-shot)
            _gym.et_drain = _eterna_clock == 2 ? 1 : 0;
            for (int i = 0; i < 4; i++) {
                int idx = _gym.et_fount[_gym.et_drain * 4 + i];
                if (idx >= 0 && idx < (int)_props.size()) {
                    _props[idx].pats = _fld_pat(167);
                    _props[idx].panm_state = 1;
                    _props[idx].panm_frame = 0.f;
                }
            }
        }
        break;
    case 10: {                                      // arrêt puis 60 frames
        _gym.et_t += dt;
        // DS : quand l'anim d'arrêt finit, FinishAnimManager détruit
        // l'objet rendu -> le jet disparaît (meshes vidés)
        for (int i = 0; i < 4; i++) {
            int idx = _gym.et_fount[_gym.et_drain * 4 + i];
            if (idx >= 0 && idx < (int)_props.size() &&
                _props[idx].panm_state == 2 && !_props[idx].meshes.empty()) {
                _props[idx].meshes.clear();
                if (getenv("WARPDBG"))
                    printf("[et] fontaine %d eteinte\n", idx);
            }
        }
        if (_gym.et_t >= 60.f / 60.f + 0.5f) {
            _gym.et_phase = 11; _gym.et_t = 0;
            // message DS : "Le niveau d'eau de la fontaine a baissé !"
            // (EternaGym_Text_FountainWaterLevelDropped = msg 12 de la banque)
            auto& bank = _bank_msgs(_cur_bank);
            _dialog = bank.size() > 12 ? _fmt_msg(bank[12], &_dlg_cols)
                : "Le niveau d'eau de la fontaine a baisse !";
            _dlg_page = 0; _dlg_chars = (float)_dialog.size();
            _dlg_spk = -1;
            _mbox = true; _dlg_dismiss = true;      // A/B referme (action())
        }
        break;
    }
    case 11:                                        // DS : attend l'appui A/B
        if (_mbox) break;
        _gym.et_tx = floorf(_ppos.x / TILE);
        _gym.et_tz = floorf(_ppos.z / TILE);
        _gym.et_xfirst = fabsf(_gym.et_tx - _gym.et_cx) <
            fabsf(_gym.et_tz - _gym.et_cz);         // MoveFromLastPosition DS
        _gym.et_phase = 5; _gym.et_next = 6;
        break;
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
    // lieu spécial (SetSpecialLocation) : position + direction explicites
    // qui priment sur les coords du warp d'arrivée.
    if (_wt.dx >= 0) {
        _ppos.x = _wt.dx * TILE + TILE / 2;
        _ppos.z = _wt.dz * TILE + TILE / 2;
        if (_wt.ddir >= 0) _pdir = _wt.ddir;
        _wt.dx = _wt.dz = _wt.ddir = -1;
    }
    // DS : le joueur est posé à y=0 puis RecalculateObjectHeight colle la
    // plaque la plus proche de 0 — le spawn à 40 + snap biaisé faisait
    // atterrir sur les plaques hautes empilées (haut d'escalier, pont).
    {   float gy0 = _ground_y({_ppos.x, 0.f, _ppos.z}, 0.f);
        if (gy0 > -1e8f) _ppos.y = gy0; }
    if (_pokefol >= 0) _pokefol_spawn();      // suiveur : derrière le joueur
    // ne pas re-déclencher le warp sur lequel on vient de spawner
    _warp_cd_x = (int)floorf(spawn.x / TILE);
    _warp_cd_z = (int)floorf(spawn.z / TILE);
    printf("[world] hdr=%d %s cells=%d surf=%d verts=%d warps=%d objs=%d\n",
        header_id, _mapname.c_str(), (int)_cells.size(), _nsurf, _nverts,
        (int)_warps.size(), (int)_objects.size());
    if (getenv("MTXDBG")) {
        printf("[mtx] %dx%d land=%zu hdrs : ", _mtx.w, _mtx.h,
            _mtx.land.size());
        for (size_t i = 0; i < _mtx.land.size(); i++)
            printf("%d:%d ", (int)i, has_hdr ? _mtx.headers[i] : -1);
        printf("\n");
    }
    if (getenv("BEHGRID")) {                    // grille comportements autour du spawn
        int px = (int)floorf(spawn.x / TILE), pz = (int)floorf(spawn.z / TILE);
        for (int gz = pz - 12; gz < pz + 28; gz++) {
            printf("[grid] z=%3d ", gz);
            for (int gx = px - 12; gx < px + 52; gx++) {
                int b = _tile_beh(gx, gz) & 0xFF;
                printf("%c", b == 0x02 || b == 0x03 ? '#' :
                       b == 0x6e ? 'w' : b ? '+' : '.');
            }
            printf("\n");
        }
    }
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
    // Monde Distorsion : DynamicMapFeaturesInit post-chargement, avant
    // les scripts d'init (DS : _set_header(init) le fait pour le streaming)
    if (header_id >= 573 && header_id <= 583 && header_id != 578)
        _dw_init(header_id);
    else if (_dw.on) _dw_free();
    _run_init();
    // reprise immédiate si aucun script d'init ne bloque (sinon : update())
    if (_vm_cont && !_vm.busy()) _vm_resume();
}

// FieldTask_InitCall DS : le script qui a demandé le Warp reprend après les
// scripts d'init de la nouvelle map, avec son bytecode/pc/état/banque d'origine.
void World::_vm_resume() {
    _vm_cont = false;
    _vm.restore_ctx(std::move(_vm_ctx));
    _cur_bank = _vm_ctx_bank; _vm_member = _vm_ctx_member;
    if (verbose) printf("[vm] reprise ctx pc=%d st=%d\n", _vm.pc(), _vm.state());
    _vm.step();
    if (!_vm.busy()) _script_end_box();
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
    // props de cette cellule : libère meshes et MARQUE l'instance plutôt que
    // de l'effacer — les arènes dynamiques (aiguilles, engrenages, fontaines)
    // référencent _props par index, un erase() les décalerait.
    for (auto& p : _props)
        if (p.cell == k) {
            for (auto& g : p.meshes) {
                _nverts -= g.nverts;
                rnd::free_mesh(g);
            }
            p.meshes.clear();
            p.cell = -2;                        // cadavre : plus de cellule
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
    auto c = _mk_cell(_mtx, cx, cy, {0.f, 0.f, 0.f});
    if (c) _cells[ckey(cx, cy)] = c;
}

// corps commun de _load_cell, paramétré par la matrice et un décalage
// monde — le Monde Distorsion le réutilise pour l'étage inactif (off =
// delta d'espace partagé). Ne touche ni _cells ni _props.
std::shared_ptr<World::Cell> World::_mk_cell(const Nds::Matrix& mtx,
        int cx, int cy, Vec3 off) {
    int i = cy * mtx.w + cx;
    if (i < 0 || i >= (int)mtx.land.size()) return nullptr;
    int land_id = mtx.land[i];
    int alt = i < (int)mtx.alt.size() ? (int8_t)mtx.alt[i] : 0;

    auto c = std::make_shared<Cell>();
    // Godot : node.position = cellule centre — les verts BMD sont en ±256
    c->org = {cx * CELL + CELL / 2 + off.x, alt * 8.f + off.y,
              cy * CELL + CELL / 2 + off.z};
    c->cx = cx; c->cy = cy;
    c->land_id = land_id;
    c->hdr = i < (int)mtx.headers.size() ? mtx.headers[i] : -1;

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
                printf("    [cell %d,%d land%d] mat=%s tex=%s poly=%08x dif=%08x verts=%d x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]\n",
                    cx, cy, land_id, mg.matdef.name.c_str(), mg.matdef.tex_name.c_str(),
                    mg.matdef.poly, mg.matdef.dif,
                    (int)buf.size() / rnd::VF, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
                if (!mg.prims.empty() && !mg.prims[0].v.empty()) {
                    auto& vv = mg.prims[0].v[0];
                    printf("      v0 col=(%.2f %.2f %.2f %.2f) nrm=(%.2f %.2f %.2f)\n",
                        vv.c.r, vv.c.g, vv.c.b, vv.c.a, vv.n.x, vv.n.y, vv.n.z);
                }
            }
            if (buf.empty()) continue;
            const char* hide = getenv("HIDE");
            bool hidden = hide && *hide
                && mg.matdef.name.find(hide) != std::string::npos;
            bool alpha = false; int polyA = 31;
            uint32_t t = hidden ? 0
                : _mat_tex(mg.matdef, _texset, &alpha, &polyA);
            GpuMesh g = hidden ? GpuMesh{}
                : rnd::upload(buf.data(), (int)buf.size() / rnd::VF, t);
            g.dbg_name = mg.matdef.name;
            g.alpha_cut = alpha;
            g.blend = polyA < 31 ? std::max(polyA / 31.f, 0.03f) : 1.f;
            // décalcomanies lumineuses au sol (*lamp*, points light*) : la DS
            // les rend en translucide — leur alpha est un dégradé. Opaques +
            // alpha-cut, le centre plein forme un carré lumineux ; en blend,
            // le dégradé donne une lueur douce.
            if (mg.matdef.name.find("lamp") != std::string::npos
                || mg.matdef.name.rfind("light", 0) == 0) {
                g.blend = 0.98f; g.alpha_cut = false;
            }
                /*DS attr*/ g.poly_id = (mg.matdef.poly >> 24) & 63; g.fog = ((mg.matdef.poly >> 15) & 1) != 0;
                /*DS attr*/ g.lit = mg.matdef.poly & 0xF;          // masque lampes (bits 0-3)
                g.water = tex_is_water(mg.matdef.tex_name)
            || mg.matdef.name.rfind("c1_fun", 0) == 0;
                g.mark_water(buf);
                g.glow = mat_is_lamp(mg.matdef.name);
            if (mat_is_light_src(mg.matdef.name)) {
                // buf local -> monde : org de la cellule + centroïde de
                // chaque cluster (un mesh = plusieurs lampadaires)
                for (auto& v : lamp_clusters(buf.data(),
                        (int)buf.size() / rnd::VF))
                    _lamps.push_back(v + c->org);
            }
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
                printf("  [bdhc] cell %d,%d org=%.0f,%.0f,%.0f bdhc_sz=%zu plates=%d\n",
                    cx, cy, c->org.x, c->org.y, c->org.z,
                    c->land->bdhc.size(), (int)c->land->plates.size());
                for (auto& pl : c->land->plates)
                    printf("    plate wx[%d..%d] wz[%d..%d] wy=%d"
                           " n=(%.2f,%.2f,%.2f)\n",
                        (int)floorf((pl.x0 + c->org.x) / 16),
                        (int)floorf((pl.x1 + c->org.x - 1) / 16),
                        (int)floorf((pl.z0 + c->org.z) / 16),
                        (int)floorf((pl.z1 + c->org.z - 1) / 16),
                        (int)(-(pl.nx * (pl.x0 + pl.x1) / 2 + pl.nz * (pl.z0 + pl.z1) / 2 + pl.c) / pl.ny + c->org.y),
                        pl.nx, pl.ny, pl.nz);
                for (int tz = 10; tz < 32; tz++) {
                    printf("    attr z=%02d ", tz);
                    for (int tx = 13; tx < 20; tx++)
                        printf("%04x ", c->land->attrs[tz * 32 + tx]);
                    printf("\n");
                }
            }
        }
    }
    return c;
}

// ── sauvegarde : map + position + vars + flags ────────────────────────────────
void World::nick_pick(const std::string& nm, bool cancelled) {
    int s = _vm.nick_slot;
    if (!cancelled && s >= 0 && s < (int)_vm.party.size()) {
        auto& pm = _vm.party[s];
        // nom vide ou = espèce -> pas de surnom (comme la décomp)
        pm.nick = (nm.empty() || nm == species_name(pm.species))
            ? "" : nm;
    }
    _vm.nick_pick(cancelled);
    _vm.step();
}

std::string World::mon_name(const ScriptVM::PartyMon& m) {
    if (m.egg) return tr("\u0152UF", "EGG");
    return m.nick.empty() ? species_name(m.species) : m.nick;
}

// ── skins perso : PNG du dossier skins/ ───────────────────────────────────────
// Formats reconnus :
//   - colonne 32px : frames 32x32 empilées (layout decomp : 16 marche + 16 course)
//   - feuille RMXP 4x4 (128x128) : lignes bas/gauche/droite/haut, cols pas/arrêt
void World::scan_skins(const std::string& dir) {
    _skindir = dir;
    _skins_scanned = true;
    _skins.clear();
    for (auto& kv : _skcache)
        for (auto& tk : kv.second) rnd::free_tex(tk.second);
    _skcache.clear();
    if (dir.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".png") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        int w, h;
        unsigned char* d = stbi_load(f.c_str(), &w, &h, nullptr, 4);
        if (!d) continue;
        SkinSheet ss;
        ss.name = fs::path(f).stem().string();
        ss.frames.assign(32, nullptr);
        auto mkimg = [&](const unsigned char* src, int stride) {
            auto im = std::make_shared<Image>();
            im->create(32, 32);
            for (int y = 0; y < 32; y++)
                memcpy(&im->px[y * 128], src + y * stride, 128);
            return im;
        };
        if (w == 32 && h >= 32 && h % 32 == 0) {          // colonne native
            int nf = std::min(32, h / 32);
            for (int f2 = 0; f2 < nf; f2++)
                ss.frames[f2] = mkimg(d + (size_t)f2 * 32 * 32 * 4, 128);
        } else if (w == 128 && h == 128) {                // RMXP 4x4
            // lignes RMXP bas/gauche/droite/haut -> blocs decomp
            // (block = W2R[dir]*4 : S->1, N->0, O->2, E->3)
            static const int R2B[4] = {1, 2, 3, 0};       // bas,haut,gch,drt
            // cols RMXP arrêt/pasG/arrêt/pasD -> offets 0/1/2 du bloc
            static const int C2F[4] = {0, 1, 0, 2};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    ss.frames[R2B[r] * 4 + C2F[c]] = mkimg(
                        d + ((size_t)r * 32 * w + c * 32) * 4, (size_t)w * 4);
        }
        stbi_image_free(d);
        // sheet vélo dédiée : skins/bike/<nom>.png — 24 frames 32×32 dans
        // l'ordre pl_cyclehero (N 0-3, S 4-7, O 8-11, E 12-15 pédalage ;
        // 16-21 coast/idle ; 20-23 montage). Colonne 32×768 ou grille.
        std::string bp = dir + "/bike/" + ss.name + ".png";
        if (unsigned char* bd = stbi_load(bp.c_str(), &w, &h, nullptr, 4)) {
            if (w % 32 == 0 && h % 32 == 0 && (w / 32) * (h / 32) >= 24) {
                int cols = w / 32;
                ss.bike.assign(24, nullptr);
                for (int f2 = 0; f2 < 24; f2++) {
                    int cx = (f2 % cols) * 32, cy = (f2 / cols) * 32;
                    auto im = std::make_shared<Image>();
                    im->create(32, 32);
                    for (int y = 0; y < 32; y++)
                        memcpy(&im->px[y * 128],
                            bd + ((size_t)(cy + y) * w + cx) * 4, 128);
                    ss.bike[f2] = im;
                }
            }
            stbi_image_free(bd);
        }
        if (ss.frames[0] || ss.frames[4])
            _skins.push_back(std::move(ss));
    }
}

void World::ensure_skins() {
    if (!_skins_scanned) {
        _skins_scanned = true;
        if (!_skindir.empty()) {
            _skins_scanned = false;
            scan_skins(_skindir);
        }
    }
}

// dos du joueur en combat : skins/back/<nom>.png quand la skin perso en a un
// (Gold/Lyra HGSS), sinon le dos du héros/héroïne par genre (trbgra Platine)
static uint32_t _side_tex(const std::string& dir, const std::string& nm) {
    static std::map<std::string, uint32_t> cache;
    std::string key = dir + "/" + nm;
    auto it = cache.find(key);
    if (it == cache.end()) {
        uint32_t& t = cache[key];
        int w, h;
        std::string p = dir + "/" + nm + ".png";
        if (unsigned char* d = stbi_load(p.c_str(), &w, &h, nullptr, 4)) {
            Image im;
            im.create(w, h);
            memcpy(im.px.data(), d, (size_t)w * h * 4);
            t = rnd::upload_tex_nearest(im);
            stbi_image_free(d);
        }
        it = cache.find(key);
    }
    return it->second;
}

uint32_t World::skin_back_tex(int i) {
    ensure_skins();
    if (i < 0 || i >= (int)_skins.size()) return 0;
    return _side_tex(_skindir + "/back", _skins[i].name);
}

// face avant d'une skin perso : skins/front/<nom>.png
uint32_t World::skin_front_tex(int i) {
    ensure_skins();
    if (i < 0 || i >= (int)_skins.size()) return 0;
    return _side_tex(_skindir + "/front", _skins[i].name);
}

// dos du héros par défaut : skins/back/PLAYER_M.png / PLAYER_F.png
// remplace le dos trbgra quand le fichier existe (Lucas custom HD par ex.)
uint32_t World::hero_back_tex(int g) {
    ensure_skins();
    uint32_t t = _side_tex(_skindir + "/back", g ? "PLAYER_F" : "PLAYER_M");
    return t ? t : trainer_back_tex(g);
}

// face du héros par défaut : skins/front/PLAYER_M.png / PLAYER_F.png
uint32_t World::hero_front_tex(int g) {
    ensure_skins();
    uint32_t t = _side_tex(_skindir + "/front", g ? "PLAYER_F" : "PLAYER_M");
    return t ? t : trainer_front_tex(g);
}

uint32_t World::player_back_tex() {
    if (skin >= SKIN_BASE) {
        uint32_t t = skin_back_tex(skin - SKIN_BASE);
        if (t) return t;
    }
    return hero_back_tex(_gender);
}

uint32_t World::player_front_tex() {
    if (skin >= SKIN_BASE) {
        uint32_t t = skin_front_tex(skin - SKIN_BASE);
        if (t) return t;
    }
    return hero_front_tex(_gender);
}

const World::SkinSheet* World::skin_custom(int i) const {
    return (i >= 0 && i < (int)_skins.size()) ? &_skins[i] : nullptr;
}

// ── pokémon suiveur (mod façon HGSS) ─────────────────────────────────────────
// followers/NNN.png = colonne de 8 frames fw×fh (N,N,S,S,W,W,E,E), transparent.
// var : 0 = normal, 1 = femelle (NNN_f.png), >=2 = forme (NNN_var.png) ;
// fallback sur la version normale quand le fichier variante n'existe pas.
const World::FolSheet* World::_fol_sheet(int sp, int var) {
    int key = sp * 256 + var;
    auto it = _folcache.find(key);
    if (it == _folcache.end()) it = _folcache.emplace(key, FolSheet{}).first;
    FolSheet& s = it->second;
    if (s.tried) return s.img ? &s : nullptr;
    s.tried = true;
    if (_foldir.empty()) return nullptr;
    char nm[24];
    if (var == 1)      snprintf(nm, sizeof nm, "%03d_f.png", sp);
    else if (var >= 2) snprintf(nm, sizeof nm, "%03d_%d.png", sp, var);
    else               snprintf(nm, sizeof nm, "%03d.png", sp);
    std::string path = _foldir + "/" + nm;
    int w, h;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    if (!d && var) {                        // variante absente -> version normale
        snprintf(nm, sizeof nm, "%03d.png", sp);
        path = _foldir + "/" + nm;
        d = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    }
    if (!d || h < 8 || h % 8) { if (d) stbi_image_free(d); return nullptr; }
    s.fw = w; s.fh = h / 8;
    auto im = std::make_shared<Image>();
    im->create(w, h);
    memcpy(im->px.data(), d, (size_t)w * h * 4);
    stbi_image_free(d);
    s.img = im;
    for (int f = 0; f < 8; f++) {
        Image fr; fr.create(s.fw, s.fh);
        memcpy(fr.px.data(), im->px.data() + (size_t)f * s.fh * w * 4,
               (size_t)s.fw * s.fh * 4);
        s.tex[f] = rnd::upload_tex_nearest(fr);
    }
    if (verbose) printf("[fol] %s -> %dx%d x8\n", nm, s.fw, s.fh);
    return &s;
}

// variante du suiveur courant : 1 = femelle (PID < seuil de la table gen4,
// seulement pour les espèces à sprite dimorphe), sinon 0. En mode forcé on
// rend la variante de debug.
int World::_pokefol_variant() const {
    if (_pokefol_lock) return _pokefol_dbgvar;
    if (_pokefol < 0 || _pokefol >= (int)_vm.party.size()) return 0;
    const auto& m = _vm.party[_pokefol];
    if (!m.pid) return 0;
    static const int F[][2] = {{3,31},{25,127},{154,31},{202,127},{208,127},
                               {214,127},{415,31},{443,127},{444,127},{445,127},
                               {449,127},{450,127}};
    for (auto& e : F)
        if (e[0] == m.species)
            return (int)(m.pid & 0xFF) < e[1] ? 1 : 0;
    return 0;
}

bool World::pokefol_shown() const {
    return _pokefol >= 0 && _pokefol_sp > 0 &&
           !_psurf && !_pstate && !_phidden &&   // surf/vélo/caché : pas de suiveur
           !_btl.active && _wild_t <= 0.f;      // combat/rencontre : rangé
}

// choisit le 1er Pokémon non-œuf de l'équipe (règle HGSS), respawn si changé
void World::_pokefol_refresh() {
    if (_pokefol_lock) return;               // debug : espèce imposée -> garde
    int want = -1;
    for (int i = 0; i < (int)_vm.party.size(); i++)
        if (!_vm.party[i].egg && _vm.party[i].species > 0) { want = i; break; }
    if (want < 0) { _pokefol = -1; _pokefol_sp = 0; _pokefol_var = 0;
                    _pftrail.clear(); _pfstep = 0; return; }
    int vsp = _vm.party[want].species;
    if (want != _pokefol || vsp != _pokefol_sp) {
        _pokefol = want;
        _pokefol_sp = vsp;
        _pokefol_var = _pokefol_variant();
        _pftrail.clear(); _pfstep = 0;
        _pokefol_spawn();
    }
}

// apparaît sur la tuile derrière le joueur
void World::_pokefol_spawn() {
    int dx, dz; dirv(_pdir, dx, dz);
    _pfpos = {_ppos.x - dx * TILE, 0.f, _ppos.z - dz * TILE};
    float g = _ground_y({_pfpos.x, _ppos.y, _pfpos.z}, _ppos.y);
    _pfpos.y = g > -1e8f ? g : _ppos.y;
    _pfdir = _pdir; _pfanim = 0.f;
    _pfjump = false; _pfemote = 0.f;
}

// interaction touche A face au suiveur : il se tourne vers le joueur,
// cœur au-dessus, cri + petite réaction dans la boîte de dialogue.
void World::_pokefol_talk() {
    int ftx = (int)floorf(_pfpos.x / TILE), ftz = (int)floorf(_pfpos.z / TILE);
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    int dx = ptx - ftx, dz = ptz - ftz;
    _pfdir = abs(dz) >= abs(dx) ? (dz > 0 ? 0 : 1) : (dx > 0 ? 3 : 2);
    _pfemote = 1.4f;
    _pfjump = true; _pfjt = 0.f;
    if (_snd && _pfcry_t <= 0.f) { _snd->cry(_pokefol_sp); _pfcry_t = 2.5f; }
    static const char* F[][2] = {
        {"%s a l'air heureux de vous suivre !", "%s looks happy to follow you!"},
        {"%s vous regarde avec affection !", "%s gazes at you fondly!"},
        {"%s saute de joie !", "%s jumps for joy!"},
        {"%s semble prêt à continuer !", "%s seems ready to go on!"},
        {"%s vous fait un petit cri !", "%s cries happily at you!"},
        {"%s se frotte contre vous !", "%s rubs against you!"},
    };
    int r = rand() % 6;
    char buf[160];
    snprintf(buf, sizeof buf, tr(F[r][0], F[r][1]),
             species_name(_pokefol_sp).c_str());
    _dialog = buf;
    _dlg_cols.clear(); _dlg_page = 0; _dlg_chars = 0.f;
    _dlg_spk = -1;                                 // suiveur : pas de PNJ
    _mbox = true; _dlg_dismiss = true;             // A referme (action())
}

// debug F1 : impose n'importe quelle espèce comme suiveur
// (sp > 0 = espèce forcée, 0 = retour à l'équipe, < 0 = suiveur caché)
void World::pokefol_force(int sp, int var) {
    if (sp < 0) {                           // caché : verrouillé sans espèce
        _pokefol_lock = 1; _pokefol = -1; _pokefol_sp = 0; _pokefol_var = 0;
        _pftrail.clear(); _pfstep = 0; return;
    }
    _pokefol_lock = sp > 0 ? 1 : 0;
    if (sp <= 0) { _pokefol = -1; _pokefol_sp = 0; _pokefol_var = 0;
                   _pokefol_refresh(); return; }
    _pokefol = 0;                           // slot factice (équipe non liée)
    _pokefol_sp = sp;
    _pokefol_dbgvar = var;
    _pokefol_var = var;
    _pftrail.clear(); _pfstep = 0;
    _pokefol_spawn();
}

bool World::pokefol_has_sprite(int sp, int var) {
    return sp > 0 && _fol_sheet(sp, var) && _fol_sheet(sp, var)->tex[0];
}

// ── parcelles à baies (port de berry_patches.c / berry_patch_manager.c DS) ──
// 128 patchs persistants ; croissance en minutes réelles (RTC DS). La table
// d'init (berry_init.inc) pré-plante les 118 parcelles au stade FRUIT.
// isGrowing : le DS ne fait pousser que les parcelles vues ou plantées —
// on marque celles de la map courante au chargement.

#include "berry_init.inc"

void World::_berry_setup() {
    if (_berry_ready) return;
    _berry_ready = true;
    // données de croissance : /itemtool/itemdata/nuts_data.narc, 64 membres
    // de 12 o — baseYield @3, stageDuration (heures) @4, drainRate @5
    if (auto n = _db->narc("/itemtool/itemdata/nuts_data.narc"))
        for (int i = 0; i < 64 && i < n->count(); i++) {
            const Bytes& d = n->member(i);
            if (d.size() >= 6) {
                _bgrow[i].baseYield = d[3];
                _bgrow[i].stageDur  = d[4];
                _bgrow[i].drain     = d[5];
            }
        }
    for (int i = 0; i < 128; i++) {          // sBerryInitTable DS
        auto& p = _berry[i];
        p.berryID = BERRY_INIT[i][0];
        if (!p.berryID) continue;
        p.stage = 5;                         // BERRY_GROWTH_STAGE_FRUIT
        int dur = p.berryID <= 64 ? _bgrow[p.berryID - 1].stageDur * 60 : 0;
        p.stageMin = (uint32_t)dur * 4;      // * BASE_HARVEST_STAGES
        p.yield = BERRY_INIT[i][1];
        p.moistRating = 100;
        p.yieldRating = 3;
        // isGrowing reste 0 tant que la parcelle n'a pas été vue
    }
    _berry_minute = (uint64_t)(time(nullptr) / 60);
}

// minutes réelles -> elapse (appelé chaque tick + au chargement de save)
void World::_berry_tick() {
    _berry_setup();
    uint64_t now = (uint64_t)(time(nullptr) / 60);
    if (now <= _berry_minute) return;
    uint32_t mins = (uint32_t)std::min<uint64_t>(now - _berry_minute, 60 * 24 * 400);
    _berry_minute = now;
    _berry_elapse(mins);
}

void World::_berry_elapse(uint32_t minutes) {
    // CalcMinutesRemainingInStage : stageDur heures -> minutes, ajusté mulch
    auto stage_min = [&](int tag, int mulch) -> uint32_t {
        int m = (tag >= 1 && tag <= 64 ? _bgrow[tag - 1].stageDur : 0) * 60;
        if (mulch == 1) m = m * 3 / 4;                       // GROWTH : -25%
        else if (mulch == 2) m += m / 2;                     // DAMP : +50%
        return (uint32_t)std::max(m, 1);
    };
    auto drain_rate = [&](int tag, int mulch) -> int {
        int r = tag >= 1 && tag <= 64 ? _bgrow[tag - 1].drain : 0;
        if (mulch == 2) return r / 2;                        // DAMP : drain/2
        if (mulch == 1) return r + r / 2;                    // GROWTH : drain*1.5
        return r;
    };
    auto harvest_stages = [](int mulch) { return mulch == 3 ? 6 : 4; };
    auto max_replant  = [](int mulch) { return mulch == 4 ? 15 : 10; };
    auto drain = [&](BerryPatch& p, int mins) {              // DrainPatchMoisture
        if (p.stage == 5) return;                            // FRUIT : pas de drain
        mins += p.moistMin;
        int hours = mins / 60; p.moistMin = (uint16_t)(mins % 60);
        if (!hours) return;
        int dr = drain_rate(p.berryID, p.mulch);
        if (p.moistRating >= dr * hours) { p.moistRating -= dr * hours; return; }
        if (p.moistRating > 0) {
            hours -= (p.moistRating + dr - 1) / dr;
            p.moistRating = 0;
        }
        p.yieldRating = hours < p.yieldRating ? p.yieldRating - hours : 0;
    };
    auto advance = [&](BerryPatch& p) {                      // AdvancePatchGrowth
        switch (p.stage) {
        case 1: case 2: case 3: p.stage++; break;
        case 4:
            p.yield = (uint8_t)(_bgrow[p.berryID - 1].baseYield * p.yieldRating);
            if (p.yield < 2) p.yield = 2;
            p.stage = 5; break;
        case 5:
            p.yield = 0; p.stage = 2;                        // repousse : SPROUTED
            p.yieldRating = 5;
            if (++p.replant >= max_replant(p.mulch)) p = BerryPatch();
            break;
        }
    };
    for (auto& p : _berry) {
        if (!p.berryID || !p.stage || !p.growing) continue;
        uint32_t maxLife = stage_min(p.berryID, p.mulch) *
            (1 + (3 + harvest_stages(p.mulch)) * max_replant(p.mulch));
        if (minutes >= maxLife) { p = BerryPatch(); continue; }
        int total = (int)minutes;
        while (p.stage && total) {
            if ((int)p.stageMin > total) {
                drain(p, total); p.stageMin -= total; break;
            }
            drain(p, (int)p.stageMin);
            total -= (int)p.stageMin;
            advance(p);
            p.stageMin = stage_min(p.berryID, p.mulch);
            if (p.stage == 5) p.stageMin *= harvest_stages(p.mulch);
        }
    }
}

// patchID de l'objet cible du script (SCRIPT_MANAGER_TARGET_OBJECT -> d0)
int World::_berry_patch() const {
    if (_talk_obj < 0 || _talk_obj >= (int)_objects.size()) return -1;
    if (_objects[_talk_obj].gfx != 100) return -1;
    int id = _objects[_talk_obj].d0;
    return id >= 0 && id < 128 ? id : -1;
}

// 0 stade 1 itemID 2 mulch->item 3 humidité 4 yield 5 setMulch 6 planter
// 7 arrosage (0 début/1 fin) 8 récolte. DS : itemID = tag+148, mulch = type+212.
int World::_berry_op(int sub, int arg) {
    _berry_setup();
    int pi = _berry_patch();
    if (pi < 0) return 0;
    BerryPatch& p = _berry[pi];
    switch (sub) {
    case 0: return p.stage;
    case 1: return p.berryID ? p.berryID + 148 : 0;
    case 2: return p.mulch ? p.mulch + 212 : 0;
    case 3: return p.moistRating == 0 ? 0 : p.moistRating <= 50 ? 1 : 2; // VERY_DRY/DRY/MOIST
    case 4: return p.yield;
    case 5: p.mulch = (uint8_t)(arg >= 213 && arg <= 216 ? arg - 212 : 0); return 0;
    case 6: {                                                // PlantBerry
        int tag = arg - 148;
        if (tag < 1 || tag > 64) return 0;
        p.berryID = (uint8_t)tag; p.stage = 1;               // PLANTED
        p.stageMin = 0;                                      // recalculé ci-dessous
        p.stageMin = (uint32_t)(_bgrow[tag - 1].stageDur * 60);
        if (p.mulch == 1) p.stageMin = p.stageMin * 3 / 4;
        else if (p.mulch == 2) p.stageMin += p.stageMin / 2;
        p.moistMin = 0; p.replant = 0; p.yield = 0;
        p.moistRating = 100; p.yieldRating = 5; p.growing = 1;
        return 1;
    }
    case 7:                                                  // arrosoir (fin = sol mouillé)
        if (arg == 1) { p.moistRating = 100; p.moistMin = 0; }
        return 0;
    case 8: {                                                // HarvestBerry -> sac
        int tag = p.berryID, q = p.yield;
        p = BerryPatch();
        if (tag && q > 0) _vm.bag_add(tag + 148, q);
        return q;
    }
    }
    return 0;
}

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
    // format v6 : IV legacy (octet, IV PV) + PV (2o) par membre d'équipe
    uint8_t nv6 = (uint8_t)_vm.party.size(); fwrite(&nv6, 1, 1, f);
    for (auto& m : _vm.party) {
        uint8_t iv = m.ivs == 0xFFFFFFFFu ? 0 : (uint8_t)iv_at(m.ivs, 0);
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
    // format v8 : boîtes PC (espèce, niveau, PV, IV + progression v7 par membre)
    uint16_t nb8 = (uint16_t)_vm.pcbox.size(); fwrite(&nb8, 2, 1, f);
    for (auto& m : _vm.pcbox) {
        int16_t s = (int16_t)m.species, l = (int16_t)m.level;
        fwrite(&s, 2, 1, f); fwrite(&l, 2, 1, f);
        int16_t hp = (int16_t)m.hp;
        uint8_t iv = m.ivs == 0xFFFFFFFFu ? 0 : (uint8_t)iv_at(m.ivs, 0);
        fwrite(&hp, 2, 1, f); fwrite(&iv, 1, 1, f);
        fwrite(&m.pid, 4, 1, f); fwrite(&m.exp, 4, 1, f);
        fwrite(m.evs, 1, 6, f);
        int16_t fr = (int16_t)m.friendship; fwrite(&fr, 2, 1, f);
        uint8_t nm = (uint8_t)m.nmoves; fwrite(&nm, 1, 1, f);
        for (int k = 0; k < 4; k++) {
            int16_t mv = (int16_t)m.moves[k], pp = (int16_t)m.pp[k];
            fwrite(&mv, 2, 1, f); fwrite(&pp, 2, 1, f);
        }
    }
    // format v9 : apparence du joueur (gfx PNJ choisi, -1 = héros/héroïne)
    int16_t sk = (int16_t)skin; fwrite(&sk, 2, 1, f);
    uint8_t hu = (uint8_t)((skin_hue / 45) & 7); fwrite(&hu, 1, 1, f);  // compat
    // v10 : retouche couleur complète (mask 32b + sat + val + teinte °)
    uint32_t mk = skin_mask; fwrite(&mk, 4, 1, f);
    uint8_t sa = (uint8_t)skin_sat, va = (uint8_t)skin_val;
    fwrite(&sa, 1, 1, f); fwrite(&va, 1, 1, f);
    int16_t hd = (int16_t)skin_hue; fwrite(&hd, 2, 1, f);
    // v11 : œuf + surnom par membre (équipe puis boîtes)
    auto wnick = [&](const ScriptVM::PartyMon& m) {
        uint8_t eg = m.egg ? 1 : 0; fwrite(&eg, 1, 1, f);
        uint8_t nl = (uint8_t)std::min<size_t>(m.nick.size(), 16);
        fwrite(&nl, 1, 1, f);
        if (nl) fwrite(m.nick.data(), 1, nl, f);
    };
    uint8_t nv11 = (uint8_t)_vm.party.size(); fwrite(&nv11, 1, 1, f);
    for (auto& m : _vm.party) wnick(m);
    uint16_t nb11 = (uint16_t)_vm.pcbox.size(); fwrite(&nb11, 2, 1, f);
    for (auto& m : _vm.pcbox) wnick(m);
    // format v12 : parcelles à baies (128 x 12o) + horodatage minute
    fwrite(_berry, sizeof(BerryPatch), 128, f);
    uint64_t bm = _berry_minute; fwrite(&bm, 8, 1, f);
    // v13 : état de la pendule d'Eterna (SystemVars DS)
    uint8_t ec = (uint8_t)_eterna_clock; fwrite(&ec, 1, 1, f);
    // v14 : argent + badges + temps + pokédex vu (écran titre, persistance)
    uint32_t mo = _vm.money; fwrite(&mo, 4, 1, f);
    uint8_t bd = (uint8_t)_vm.badges; fwrite(&bd, 1, 1, f);
    uint32_t pt = (uint32_t)_vm.play_sec; fwrite(&pt, 4, 1, f);
    uint16_t nd = (uint16_t)_vm.dex_seen.size(); fwrite(&nd, 2, 1, f);
    for (int sp : _vm.dex_seen) { int16_t s = (int16_t)sp; fwrite(&s, 2, 1, f); }
    // v15 : IVs DS packés (u32, 5 bits/stat) — équipe puis boîtes PC
    uint8_t nv15 = (uint8_t)_vm.party.size(); fwrite(&nv15, 1, 1, f);
    for (auto& m : _vm.party) fwrite(&m.ivs, 4, 1, f);
    uint16_t nb15 = (uint16_t)_vm.pcbox.size(); fwrite(&nb15, 2, 1, f);
    for (auto& m : _vm.pcbox) fwrite(&m.ivs, 4, 1, f);
    // v16 : dex pris + zarbi + extras mon (rubans/fatidique/DO/objet/pension)
    uint16_t nc = (uint16_t)_vm.dex_caught.size(); fwrite(&nc, 2, 1, f);
    for (int sp : _vm.dex_caught) { int16_t s = (int16_t)sp; fwrite(&s, 2, 1, f); }
    uint8_t nu = (uint8_t)_vm.dex_unown.size(); fwrite(&nu, 1, 1, f);
    for (int u : _vm.dex_unown) { uint8_t b = (uint8_t)u; fwrite(&b, 1, 1, f); }
    auto wext = [&](const ScriptVM::PartyMon& m) {
        uint64_t rb = m.ribbons.to_ullong(); fwrite(&rb, 8, 1, f);
        uint8_t ff = m.fateful ? 1 : 0; fwrite(&ff, 1, 1, f);
        int32_t ot = m.otid; fwrite(&ot, 4, 1, f);
        int16_t dg = (int16_t)m.dc_gain, hd = (int16_t)m.held;
        fwrite(&dg, 2, 1, f); fwrite(&hd, 2, 1, f);
    };
    uint8_t nv16p = (uint8_t)_vm.party.size(); fwrite(&nv16p, 1, 1, f);
    for (auto& m : _vm.party) wext(m);
    uint16_t nv16b = (uint16_t)_vm.pcbox.size(); fwrite(&nv16b, 2, 1, f);
    for (auto& m : _vm.pcbox) wext(m);
    // pension : 2 hôtes compacts + œuf/pas/compat
    auto wmon = [&](const ScriptVM::PartyMon& m) {
        int16_t s = (int16_t)m.species, l = (int16_t)m.level,
                hp = (int16_t)m.hp;
        fwrite(&s, 2, 1, f); fwrite(&l, 2, 1, f); fwrite(&hp, 2, 1, f);
        fwrite(&m.ivs, 4, 1, f); fwrite(&m.pid, 4, 1, f);
        fwrite(&m.exp, 4, 1, f); fwrite(m.evs, 1, 6, f);
        int16_t fr = (int16_t)m.friendship; fwrite(&fr, 2, 1, f);
        uint8_t nm = (uint8_t)m.nmoves; fwrite(&nm, 1, 1, f);
        for (int k = 0; k < 4; k++) {
            int16_t mv = (int16_t)m.moves[k], pp = (int16_t)m.pp[k];
            fwrite(&mv, 2, 1, f); fwrite(&pp, 2, 1, f);
        }
        wnick(m);
        int16_t dg = (int16_t)m.dc_gain; fwrite(&dg, 2, 1, f);
        int32_t ot = m.otid; fwrite(&ot, 4, 1, f);
    };
    wmon(_vm.daycare[0]); wmon(_vm.daycare[1]);
    uint8_t de = _vm.daycare_egg ? 1 : 0; fwrite(&de, 1, 1, f);
    int16_t ds = (int16_t)_vm.daycare_steps; fwrite(&ds, 2, 1, f);
    int8_t dc = (int8_t)_vm.daycare_compat; fwrite(&dc, 1, 1, f);
    // roamers + maps + compteurs
    for (auto& r : _roamers) {
        int16_t sp = (int16_t)r.sp, rt = (int16_t)r.route,
                hp = (int16_t)r.hp;
        fwrite(&sp, 2, 1, f); fwrite(&rt, 2, 1, f); fwrite(&hp, 2, 1, f);
        uint8_t lv = (uint8_t)r.lv, st2 = (uint8_t)r.status;
        fwrite(&lv, 1, 1, f); fwrite(&st2, 1, 1, f);
        fwrite(&r.ivs, 4, 1, f); fwrite(&r.pid, 4, 1, f);
    }
    int16_t pm = (int16_t)_prev_map, em = (int16_t)_exit_map,
            ex = (int16_t)_exit_x, ez = (int16_t)_exit_z;
    fwrite(&pm, 2, 1, f); fwrite(&em, 2, 1, f);
    fwrite(&ex, 2, 1, f); fwrite(&ez, 2, 1, f);
    int16_t hf = (int16_t)_vm.hof_entries, sc = (int16_t)_vm.safari_caught;
    fwrite(&hf, 2, 1, f); fwrite(&sc, 2, 1, f);
    int16_t rk = (int16_t)rskin; fwrite(&rk, 2, 1, f);   // v17 : skin rival
    fclose(f);
    return true;
}

bool World::save_peek(const char* path, SavePeek& out) const {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    auto rd = [&](void* p, size_t n) { return fread(p, 1, n, f) == n; };
    auto skip = [&](long n) { return fseek(f, n, SEEK_CUR) == 0; };
    uint32_t magic = 0;
    if (!rd(&magic, 4) || magic != 0x504E4156) { fclose(f); return false; }
    int32_t hdr = 0;
    if (!rd(&hdr, 4) || !skip(16)) { fclose(f); return false; }
    out.header = hdr;
    uint32_t nv = 0, nf = 0, nb = 0;
    if (!rd(&nv, 4) || !skip((long)nv * 8) ||
        !rd(&nf, 4) || !skip((long)nf * 5) ||
        !rd(&nb, 4) || !skip((long)nb * 8) ||
        !skip(1)) { fclose(f); return false; }   // run
    uint8_t np = 0;
    if (!rd(&np, 1)) { fclose(f); return false; }
    for (int i = 0; i < np; i++) {               // espèce(2o) + niveau(2o)
        int16_t s = 0, l = 0;
        if (!rd(&s, 2) || !rd(&l, 2)) { fclose(f); return false; }
        if (i < 6) out.party_sp[i] = s;
    }
    if (!skip(8)) { fclose(f); return false; }   // st(6) + blackout(2)
    out.party = np;
    uint8_t ge = 0;
    if (!rd(&ge, 1)) { fclose(f); return false; }   // genre
    out.gender = ge & 1;
    uint8_t pl = 0;
    if (!rd(&pl, 1)) { fclose(f); return false; }
    char nm[64] = {0};
    if (pl >= sizeof(nm) || (pl && !rd(nm, pl))) { fclose(f); return false; }
    out.name = nm;
    // suite tolérante : nom rival (v3), objet+ordre sac (v5), IV/PV (v6),
    // surf (v6b), progression (v7), boîtes PC (v8) puis apparence (v9/v10)
    uint8_t rl = 0;
    if (!rd(&rl, 1)) { fclose(f); return true; }
    if (!skip(rl)) { fclose(f); return true; }
    if (!skip(2)) { fclose(f); return true; }                 // reg_item
    uint16_t nsq = 0;
    if (!rd(&nsq, 2) || !skip((long)nsq * 2)) { fclose(f); return true; }
    uint8_t n6 = 0;
    if (!rd(&n6, 1) || !skip((long)n6 * 3)) { fclose(f); return true; }
    if (!skip(1)) { fclose(f); return true; }                 // surf
    uint8_t n7 = 0;
    if (!rd(&n7, 1) || !skip((long)n7 * 33)) { fclose(f); return true; }
    uint16_t n8 = 0;
    if (!rd(&n8, 2) || !skip((long)n8 * 40)) { fclose(f); return true; }
    int16_t sk = -1; uint8_t hu = 0;
    if (!rd(&sk, 2)) { fclose(f); return true; }
    out.skin = sk;
    if (!rd(&hu, 1)) { fclose(f); return true; }
    out.skin_hue = (hu & 7) * 45;                             // ancien format
    uint32_t mk = 0; uint8_t sa = 0, va = 0; int16_t hd = 0;
    if (rd(&mk, 4) && rd(&sa, 1) && rd(&va, 1) && rd(&hd, 2)) {
        out.skin_mask = mk; out.skin_sat = sa;
        out.skin_val = va; out.skin_hue = hd;
    }
    // v11 (surnoms) -> v12 (baies) -> v13 (pendule) -> v14 (stats titre)
    auto skipnick = [&](int cnt) {
        for (int i = 0; i < cnt; i++) {
            uint8_t nl = 0;
            if (!skip(1) || !rd(&nl, 1) || !skip(nl)) return false;
        }
        return true;
    };
    uint8_t nv11 = 0; uint16_t nb11 = 0;
    if (!rd(&nv11, 1) || !skipnick(nv11) ||
        !rd(&nb11, 2) || !skipnick(nb11) ||
        !skip(128 * 12 + 8) || !skip(1))
        { fclose(f); return true; }
    uint32_t mo = 0, pt = 0; uint8_t bd = 0; uint16_t nd = 0;
    if (!rd(&mo, 4) || !rd(&bd, 1) || !rd(&pt, 4) || !rd(&nd, 2))
        { fclose(f); return true; }
    out.money = mo; out.badges = bd; out.dex = nd;
    out.play_sec = pt;
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
                party[i].ivs = ivs_flat(iv); party[i].hp = hp;  // IV legacy -> flat
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
    {                                                     // v8 : boîtes PC
        uint16_t nb8 = 0;
        if (fread(&nb8, 2, 1, f) == 1)
            for (int i = 0; i < nb8 && i < 540; i++) {
                ScriptVM::PartyMon m;
                int16_t s = 0, l = 1, hp = 0, fr = 70;
                uint8_t iv = 0, nm = 0;
                if (fread(&s, 2, 1, f) != 1 || fread(&l, 2, 1, f) != 1 ||
                    fread(&hp, 2, 1, f) != 1 || fread(&iv, 1, 1, f) != 1 ||
                    fread(&m.pid, 4, 1, f) != 1 ||
                    fread(&m.exp, 4, 1, f) != 1 ||
                    fread(m.evs, 1, 6, f) != 1 ||
                    fread(&fr, 2, 1, f) != 1 ||
                    fread(&nm, 1, 1, f) != 1) break;
                m.species = s; m.level = l; m.hp = hp;
                m.ivs = ivs_flat(iv); m.friendship = fr; m.nmoves = nm;
                for (int k = 0; k < 4; k++) {
                    int16_t mv = 0, pp = 0;
                    if (fread(&mv, 2, 1, f) != 1 || fread(&pp, 2, 1, f) != 1) break;
                    m.moves[k] = mv; m.pp[k] = pp;
                }
                _vm.pcbox[i] = m;
            }
    }
    {                                                     // v9 : skin joueur
        int16_t sk = -1;
        if (fread(&sk, 2, 1, f) == 1) skin = sk;
        uint8_t hu = 0;
        if (fread(&hu, 1, 1, f) == 1) {
            skin_hue = (hu & 7) * 45;                 // ancien index -> degrés
            skin_sat = 100; skin_val = 100; skin_mask = 0xFFFFFFFF;
        }
        uint32_t mk; uint8_t st, vl; int16_t hd;      // v10 : retouche complète
        if (fread(&mk, 4, 1, f) == 1 && fread(&st, 1, 1, f) == 1 &&
            fread(&vl, 1, 1, f) == 1 && fread(&hd, 2, 1, f) == 1) {
            skin_mask = mk; skin_sat = st; skin_val = vl; skin_hue = hd;
        }
    }
    {                                                     // v11 : œufs + surnoms
        uint8_t nv = 0;
        if (fread(&nv, 1, 1, f) == 1)
            for (int i = 0; i < nv && i < (int)party.size(); i++) {
                uint8_t eg = 0, nl = 0; char nb[17] = {};
                if (fread(&eg, 1, 1, f) != 1 || fread(&nl, 1, 1, f) != 1 ||
                    (nl && fread(nb, 1, nl, f) != nl)) break;
                party[i].egg = eg != 0; party[i].nick = nb;
            }
        uint16_t nbb = 0;
        if (fread(&nbb, 2, 1, f) == 1)
            for (int i = 0; i < nbb && i < 540; i++) {
                uint8_t eg = 0, nl = 0; char nb[17] = {};
                if (fread(&eg, 1, 1, f) != 1 || fread(&nl, 1, 1, f) != 1 ||
                    (nl && fread(nb, 1, nl, f) != nl)) break;
                _vm.pcbox[i].egg = eg != 0; _vm.pcbox[i].nick = nb;
            }
    }
    {                                                     // v12 : parcelles à baies
        if (fread(_berry, sizeof(BerryPatch), 128, f) == 128) {
            _berry_ready = true;                          // état sauvegardé -> pas d'init
            if (auto n = _db->narc("/itemtool/itemdata/nuts_data.narc"))
                for (int i = 0; i < 64 && i < n->count(); i++) {
                    const Bytes& d = n->member(i);
                    if (d.size() >= 6) { _bgrow[i].baseYield = d[3];
                        _bgrow[i].stageDur = d[4]; _bgrow[i].drain = d[5]; }
                }
            uint64_t bm = 0;
            if (fread(&bm, 8, 1, f) == 1 && bm > 0) {
                // croissance hors-ligne (RTC DS : le temps continue en coulisse)
                _berry_minute = bm;
                uint64_t now = (uint64_t)(time(nullptr) / 60);
                if (now > _berry_minute) {
                    uint32_t mins = (uint32_t)std::min<uint64_t>(
                        now - _berry_minute, 60 * 24 * 400);
                    _berry_minute = now;
                    _berry_elapse(mins);
                }
            }
        }
    }
    {                                                     // v13 : pendule Eterna
        uint8_t ec = 0;
        if (fread(&ec, 1, 1, f) == 1) _eterna_clock = ec & 7;
    }
    {                                                     // v14 : argent/badges/temps/dex
        uint32_t mo = 0, pt = 0; uint8_t bd = 0; uint16_t nd = 0;
        if (fread(&mo, 4, 1, f) == 1 && fread(&bd, 1, 1, f) == 1 &&
            fread(&pt, 4, 1, f) == 1 && fread(&nd, 2, 1, f) == 1) {
            _vm.money = mo; _vm.badges = bd; _vm.play_sec = pt;
            _vm.dex_seen.clear();
            for (int i = 0; i < nd; i++) {
                int16_t s = 0;
                if (fread(&s, 2, 1, f) != 1) break;
                _vm.dex_seen.insert(s);
            }
        }
    }
    {                                                     // v15 : IVs DS packés
        uint8_t nv15 = 0;
        if (fread(&nv15, 1, 1, f) == 1) {
            for (int i = 0; i < nv15 && i < (int)party.size(); i++) {
                uint32_t ivs = 0;
                if (fread(&ivs, 4, 1, f) != 1) break;
                party[i].ivs = ivs;
            }
            uint16_t nb15 = 0;
            if (fread(&nb15, 2, 1, f) == 1)
                for (int i = 0; i < nb15 && i < 540; i++) {
                    uint32_t ivs = 0;
                    if (fread(&ivs, 4, 1, f) != 1) break;
                    _vm.pcbox[i].ivs = ivs;
                }
        }
    }
    {                                                     // v16 : dex pris + extras
        uint16_t nc = 0;
        if (fread(&nc, 2, 1, f) == 1) {
            _vm.dex_caught.clear();
            for (int i = 0; i < nc; i++) {
                int16_t s = 0;
                if (fread(&s, 2, 1, f) != 1) break;
                _vm.dex_caught.insert(s);
            }
            uint8_t nu = 0;
            if (fread(&nu, 1, 1, f) == 1) {
                _vm.dex_unown.clear();
                for (int i = 0; i < nu; i++) {
                    uint8_t b = 0;
                    if (fread(&b, 1, 1, f) != 1) break;
                    _vm.dex_unown.insert(b);
                }
            }
            auto rext = [&](ScriptVM::PartyMon& m) -> bool {
                uint64_t rb = 0; uint8_t ff = 0; int32_t ot = 0;
                int16_t dg = 0, hd = 0;
                if (fread(&rb, 8, 1, f) != 1) return false;
                fread(&ff, 1, 1, f); fread(&ot, 4, 1, f);
                fread(&dg, 2, 1, f); fread(&hd, 2, 1, f);
                m.ribbons = std::bitset<80>(rb);
                m.fateful = ff != 0; m.otid = ot;
                m.dc_gain = dg; m.held = hd;
                return true;
            };
            uint8_t nv16p = 0;
            if (fread(&nv16p, 1, 1, f) == 1)
                for (int i = 0; i < nv16p && i < (int)party.size(); i++)
                    if (!rext(party[i])) break;
            uint16_t nv16b = 0;
            if (fread(&nv16b, 2, 1, f) == 1)
                for (int i = 0; i < nv16b && i < 540; i++)
                    if (!rext(_vm.pcbox[i])) break;
            // pension : hôtes compacts (même ordre que wmon)
            auto rmon = [&](ScriptVM::PartyMon& m) -> bool {
                int16_t s = 0, l = 0, hp = 0;
                if (fread(&s, 2, 1, f) != 1) return false;
                fread(&l, 2, 1, f); fread(&hp, 2, 1, f);
                m.species = s; m.level = l; m.hp = hp;
                fread(&m.ivs, 4, 1, f); fread(&m.pid, 4, 1, f);
                fread(&m.exp, 4, 1, f); fread(m.evs, 1, 6, f);
                int16_t fr = 0; fread(&fr, 2, 1, f); m.friendship = fr;
                uint8_t nm = 0; fread(&nm, 1, 1, f); m.nmoves = nm;
                for (int k = 0; k < 4; k++) {
                    int16_t mv = 0, pp = 0;
                    fread(&mv, 2, 1, f); fread(&pp, 2, 1, f);
                    m.moves[k] = mv; m.pp[k] = pp;
                }
                uint8_t eg = 0, nl = 0; char nk[17] = {0};
                fread(&eg, 1, 1, f); fread(&nl, 1, 1, f);
                if (nl) fread(nk, 1, nl > 16 ? 16 : nl, f);
                m.egg = eg != 0; m.nick = nk;
                int16_t dg = 0; fread(&dg, 2, 1, f); m.dc_gain = dg;
                fread(&m.otid, 4, 1, f);
                return true;
            };
            rmon(_vm.daycare[0]); rmon(_vm.daycare[1]);
            uint8_t de = 0; int16_t ds = 0; int8_t dc = -1;
            fread(&de, 1, 1, f); fread(&ds, 2, 1, f); fread(&dc, 1, 1, f);
            _vm.daycare_egg = de != 0; _vm.daycare_steps = ds;
            _vm.daycare_compat = dc;
            for (auto& r : _roamers) {
                int16_t sp = 0, rt = -1, hp = -1; uint8_t lv = 0, st2 = 0;
                if (fread(&sp, 2, 1, f) != 1) break;
                fread(&rt, 2, 1, f); fread(&hp, 2, 1, f);
                fread(&lv, 1, 1, f); fread(&st2, 1, 1, f);
                fread(&r.ivs, 4, 1, f); fread(&r.pid, 4, 1, f);
                r.sp = sp; r.route = rt; r.hp = hp;
                r.lv = lv; r.status = st2;
            }
            int16_t pm = 0, em = 0, ex = 0, ez = 0;
            fread(&pm, 2, 1, f); fread(&em, 2, 1, f);
            fread(&ex, 2, 1, f); fread(&ez, 2, 1, f);
            _prev_map = pm; _exit_map = em; _exit_x = ex; _exit_z = ez;
            int16_t hf = 0, sc = 0;
            fread(&hf, 2, 1, f); fread(&sc, 2, 1, f);
            _vm.hof_entries = hf; _vm.safari_caught = sc;
            int16_t rk = -1;
            if (fread(&rk, 2, 1, f) == 1) rskin = rk;   // v17 : skin rival
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
    for (auto& p : _vm.party)                    // répare les mons sans moves
        if (p.species && !p.egg && !p.nmoves && _vm.cb_moninit)  // (GivePokemon
            _vm.cb_moninit(p);                                   //  avant le fix)
    for (auto& p : _vm.pcbox)
        if (p.species && !p.egg && !p.nmoves && _vm.cb_moninit)
            _vm.cb_moninit(p);
    _pokefol_refresh();                          // suiveur = 1er non-œuf chargé
    _vm.chosen_starter = st[0]; _vm.rival_starter = st[1];
    _vm.cpart_starter = st[2];
    return true;
}
