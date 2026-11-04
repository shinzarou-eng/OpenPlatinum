// world_script.cpp — VM scripts, dialogue, events, sac/boutique.
#include "world.h"
#include "world_internal.h"
#include "msg.h"
#include "spr.h"
#include "sdat.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <unordered_set>

// ── scripts / interaction ─────────────────────────────────────────────────────
// Substitution des tags message : {103:n:*} / {108:n:*} = buffer StringTemplate
// n (rempli par les commandes Buffer*/SetVar*), autres tags effacés.
std::string World::_fmt_msg(const std::string& s,
        std::vector<std::pair<int,int>>* cols) {
    static const char* NAMES_FR[] = {"Héros", "René", "Aurore", "Pokémon"};
    static const char* NAMES_EN[] = {"Lucas", "Barry", "Dawn", "Pokémon"};
    const char** NAMES = g_opts.lang ? NAMES_EN : NAMES_FR;
    if (cols) cols->clear();
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '{') { o += s[i]; continue; }
        size_t e = s.find('}', i);
        if (e == std::string::npos) { o += s[i]; continue; }
        std::string tag = s.substr(i + 1, e - i - 1);
        if (tag.rfind("103:", 0) == 0 || tag.rfind("108:", 0) == 0) {
            int b = strtol(tag.c_str() + 4, nullptr, 16);
            if (b >= 0 && b < 16 && !_buf[b].empty()) o += _buf[b];
            else if (b >= 0 && b < 4) o += NAMES[b];
            else o += "?";
        } else if (tag.rfind("FF00:", 0) == 0 && cols) {
            // CHAR_CONTROL_SET_COLOR DS : mémorise où la couleur commence
            int n = strtol(tag.c_str() + 5, nullptr, 16);
            cols->push_back({(int)o.size(), n});
        }
        i = e;                                          // autre tag : effacé
    }
    return o;
}

// Banque de texte pl_msg[bank] décodée (cache)
const std::vector<std::string>& World::_bank_msgs(int b) {
    auto it = _msgbanks.find(b);
    if (it != _msgbanks.end()) return it->second;
    auto pm = _db->narc(Nds::P_PLMSG);
    _msgbanks[b] = (pm && b >= 0 && b < pm->count())
        ? msgbank_decode(pm->member(b)) : std::vector<std::string>{};
    return _msgbanks[b];
}

// Routeur scriptID -> (membre scr_seq, index fonction, banque texte).
// Table SCRIPT_RANGE_TABLE de pret/pokeplatinum script_manager.c : l'ID >= seuil
// charge le fichier dédié ET sa banque de messages, saute à (id - seuil).
struct ScrRange { int threshold, member, bank; };
static const ScrRange SCR_RANGES[] = {          // trié décroissant
    {10490,499,541},{10450,500,16},{10400,400,203},{10300,1051,552},
    {10200,407,379},{10150,1116,621},{10100,1115,622},{10000,409,381},
    {9950,411,383},{9900,397,213},{9800,212,217},{9700,422,429},
    {9600,412,213},{9500,501,547},{9400,426,432},{9300,406,374},
    {9200,423,430},{9100,0,11},{9000,213,221},{8970,425,7},
    {8950,498,539},{8900,424,431},{8800,497,538},{8000,408,380},
    {7000,404,369},{5000,1114,213},{3000,1114,213},{2800,413,397},
    {2500,1,17},{2000,211,213},
};

void World::_start_script(int id) {
    _dlg_dismiss = false;                           // nouveau script : boîte gérée
    _spk_obj = -1;                                  // nouveau locuteur à déterminer
    if (id == 10100) { _tv_open = true; return; }   // télé -> écran Motisma
    if (_header == 415 && id == 5)                  // scène TV de la chambre
        { _tv_open = true; _tv_scene = true; return; } // (intro, avant René)
    if (_header == 415 && id == 6)                  // "regarder la télé" (BG)
        { _tv_open = true; return; }
    bool tv_supp = _header == 415 && id == 3 && _tv_scene;
    if (tv_supp) _tv_suppress = true;               // fin d'émission : boîtes
    if (!_scrnarc) return;                          // avalées, flags exécutés
    // ScriptContext_LoadAndOffsetID : locaux 1-indexés, 0 -> scripts_unk_0402
    int member = _header >= 0 ? _db->header(_header)->scr : -1, fn = id - 1;
    _cur_bank = _header >= 0 ? _db->header(_header)->msg : -1;
    if (id == 0) { member = 402; fn = 0; _cur_bank = 355; }
    for (auto& r : SCR_RANGES)                  // premier seuil <= id (décroissant)
        if (id >= r.threshold) {
            member = r.member; fn = id - r.threshold; _cur_bank = r.bank; break;
        }
    if (member < 0 || member >= _scrnarc->count()) {
        printf("[scr] membre %d hors borne (id=%d)\n", member, id); return; }
    _vm_member = member;
    _vm.cur_script = id;                             // SCRIPT_MANAGER_SCRIPT_ID
    _vm.load(&_scrnarc->member(member));
    if (getenv("SCRDBG")) {                          // dump brut de la fonction
        const Bytes& s = _scrnarc->member(member);
        size_t p = 0; std::vector<size_t> fo;
        while (p + 4 <= s.size()) {
            if ((s[p] | (s[p + 1] << 8)) == 0xFD13) break;
            fo.push_back(p + 4 + (s[p] | (s[p+1]<<8) | (s[p+2]<<16) | ((unsigned)s[p+3]<<24)));
            p += 4;
        }
        printf("[scrdbg] id=%d member=%d fns=%zu\n", id, member, fo.size());
        if (fn >= 0 && fn < (int)fo.size()) {
            size_t o = fo[fn], e = (fn + 1 < (int)fo.size() && fo[fn+1] > o)
                ? fo[fn + 1] : s.size();
            printf("[scrdbg] fn%d @%zu..%zu :", fn, o, e);
            for (size_t i = o; i < e && i < o + 220 && i < s.size(); i++)
                printf(" %02x", s[i]);
            printf("\n");
        }
    }
    // objet caché (8000+) : remplit VAR_0x8000/01/02 = item/qté/flag
    // table gHiddenItems DS (indexée par scriptID-8000 = flag-730)
    if (id >= 8000 && id < 8900) {
        static const int16_t HI[][2] = {
            {41,1}, {308,1}, {311,1}, {3,1}, {38,1}, {86,1}, {74,1}, {51,1}, {72,1}, {3,1},
            {108,1}, {90,1}, {50,1}, {90,1}, {91,1}, {86,1}, {87,1}, {88,1}, {2,1}, {39,1},
            {87,1}, {50,1}, {2,1}, {77,1}, {25,1}, {27,1}, {109,1}, {52,1}, {88,1}, {89,1},
            {45,1}, {80,1}, {3,1}, {90,1}, {87,1}, {28,1}, {82,1}, {84,1}, {83,1}, {27,1},
            {51,1}, {49,1}, {87,1}, {48,1}, {301,1}, {107,1}, {46,1}, {91,1}, {89,1}, {87,1},
            {25,1}, {91,1}, {91,1}, {91,1}, {91,1}, {91,1}, {313,1}, {45,1}, {94,1}, {94,1},
            {49,1}, {74,1}, {34,1}, {92,1}, {90,1}, {72,1}, {91,1}, {75,1}, {73,1}, {50,1},
            {91,1}, {51,1}, {47,1}, {29,1}, {51,1}, {63,1}, {24,1}, {50,1}, {40,1}, {92,1},
            {326,1}, {72,1}, {91,1}, {77,1}, {91,1}, {90,1}, {23,1}, {88,1}, {81,1}, {73,1},
            {24,1}, {4,1}, {91,1}, {80,1}, {27,1}, {29,1}, {90,1}, {74,1}, {36,1}, {309,1},
            {41,1}, {50,1}, {23,1}, {90,1}, {36,1}, {75,1}, {50,1}, {24,1}, {75,1}, {23,1},
            {300,1}, {48,1}, {93,1}, {92,1}, {2,1}, {29,1}, {23,1}, {50,1}, {93,1}, {41,1},
            {306,1}, {2,1}, {45,1}, {93,1}, {50,1}, {88,1}, {93,1}, {89,1}, {2,1}, {50,1},
            {50,1}, {51,1}, {86,1}, {53,1}, {87,1}, {93,1}, {91,1}, {77,1}, {91,1}, {298,1},
            {90,1}, {47,1}, {49,1}, {91,1}, {23,1}, {83,1}, {92,1}, {2,1}, {84,1}, {2,1},
            {26,1}, {3,1}, {25,1}, {76,1}, {50,1}, {87,1}, {2,1}, {86,1}, {2,1}, {50,1},
            {86,1}, {90,1}, {75,1}, {46,1}, {88,1}, {88,1}, {29,1}, {94,3}, {327,1}, {52,1},
            {88,1}, {52,1}, {2,1}, {87,1}, {29,1}, {29,1}, {91,1}, {2,1}, {50,1}, {23,1},
            {87,1}, {28,1}, {2,1}, {47,1}, {93,1}, {89,1}, {93,1}, {94,3}, {3,1}, {25,1},
            {2,1}, {48,1}, {29,1}, {2,1}, {37,1}, {77,1}, {2,1}, {46,1}, {24,1}, {27,1},
            {92,1}, {111,1}, {2,1}, {85,1}, {2,1}, {50,1}, {77,1}, {53,1}, {26,1}, {73,1},
            {50,1}, {304,1}, {85,1}, {25,1}, {87,1}, {77,1}, {451,1}, {90,1}, {89,1}, {94,1},
            {94,1}, {94,1}, {94,1}, {25,1}, {28,1}, {27,1}, {51,1}, {29,1}, {81,1}, {28,1},
            {93,1}, {88,1}, {91,1}, {37,1}, {2,1}, {37,1}, {87,1}, {88,1}, {93,1}, {93,1},
            {93,1}, {82,1}, {35,1}, {35,1}, {34,1}, {28,1}, {221,1}, {92,1}, {92,1}, {92,1},
            {92,1}, {92,1}, {92,1}, {92,1}, {92,1}, {92,1}, {92,1},
        };
        int fidx = id - 8000, item = 0, qty = 1;
        if (fidx >= 0 && fidx < (int)(sizeof(HI) / sizeof(HI[0]))) {
            item = HI[fidx][0]; qty = HI[fidx][1];
        }
        _vm.set_var(0x8000, item);
        _vm.set_var(0x8001, qty);
        _vm.set_var(0x8002, 730 + fidx);             // HIDDEN_ITEM_FLAGS_START
    }
    _vm.start(fn);
    if (verbose) printf("[scr] start id=%d -> scr[%d] fn%d\n", id, member, fn);
}

// scripts d'init (scr_seq[hdr.init]) : entrées 5 octets, fin = type 0.
// type 1 = ON_FRAME_TABLE (u32 offset -> table {var,cvar,scriptID})
// types 2/3/4 = fixe (u16 scriptID + u16 pad)
int World::_init_script(int type) {
    if (!_initb) return -1;
    const Bytes& d = *_initb;
    for (size_t p = 0; p + 4 < d.size() && u8(d, p) != 0; p += 5) {
        if (u8(d, p) != type) continue;
        if (type == 1) return -1;            // table: voir _frame_script
        return u16(d, p + 1);                // scriptID fixe
    }
    return -1;
}

int World::_frame_script() {
    if (!_initb) return -1;
    const Bytes& d = *_initb;
    for (size_t p = 0; p + 4 < d.size() && u8(d, p) != 0; p += 5) {
        if (u8(d, p) != 1) continue;
        uint32_t off = u32(d, p + 1);
        size_t t = p + 5 + off;              // relatif à fin du champ offset
        if (t >= d.size()) return -1;
        while (t + 5 < d.size() && u8(d, t) != 0) {
            int var = u16(d, t), cvar = u16(d, t + 2), scr = u16(d, t + 4);
            if (!var) return -1;
            int a = var < 0x4000 ? var : _vm.var(var);
            int b = cvar < 0x4000 ? cvar : _vm.var(cvar);
            static bool fdbg = getenv("FRDBG") != nullptr;
            if (fdbg) printf("[frtbl] var=%04x(%d) cvar=%04x(%d) scr=%d\n",
                var, a, cvar, b, scr);
            if (a == b) return scr;
            t += 6;
        }
        return -1;
    }
    return -1;
}

const Nds::Zev::Obj* World::_find_obj(int local) const {
    for (auto& o : _objects) if (o.id == local) return &o;
    return nullptr;
}
// respawn d'un objet depuis son event (MapObjectMan_AddMapObjectFromLocalID :
// position/direction/type de l'ObjectEvent, hauteur recalculée BDHC)
void World::obj_respawn(int i) {
    auto& o = _objects[i];
    _objgone.erase(i); _objhide.erase(i); _objlock.erase(i);
    _objpos[i] = {o.x * TILE + TILE / 2.f, 0.f, o.z * TILE + TILE / 2.f};
    float g0 = _ground_y(_objpos[i], o.y / 4096.f);
    _objpos[i].y = (g0 > -1e8f) ? g0 : -1e9f;
    static const int M[4] = {1, 0, 2, 3};          // dir ROM -> monde
    _objd[i] = M[o.dir & 3];
    _objmv[i] = o.move;
    // DS unk_020EDBAC[mv=48] = sub_02069BE0 : sub_02062D80(obj,0) à l'init —
    // téléporteur (Psy etc.) non-collidant tant que son cycle n'a pas warped in.
    if (o.move == 48) _objnocoll.insert(i); else _objnocoll.erase(i);
    _objw[i].home = _objpos[i];
    _objw[i].dx = _objw[i].dz = _objw[i].step = 0;
    _objw[i].phase = 0.f;
}
void World::obj_gone_set(int i, bool g) {          // RemoveObject/AddObject manuel
    if (g) { _objgone.insert(i);
        if (_objects[i].flag) _vm.set_flag(_objects[i].flag, true); }
    else _objgone.erase(i);
}
int World::msg_count(int b) { return (int)_bank_msgs(b).size(); }

// repositionne un objet vivant sur une tuile + snap BDHC (menu debug)
void World::obj_set_pos(int i, int tx, int tz) {
    _objpos[i] = {tx * TILE + TILE / 2.f, 0.f, tz * TILE + TILE / 2.f};
    float g = _ground_y(_objpos[i], _objects[i].y / 4096.f);
    _objpos[i].y = (g > -1e8f) ? g : -1e9f;
}
// slot de rencontre i (pl_enc_data : lv int8, espèce u32 à +4)
int World::enc_slot(int i, int* lv) const {
    if (!_enc || i < 0 || i >= 12) return -1;
    size_t so = 4 + (size_t)i * 8;
    if (so + 8 > _enc->size()) return -1;
    if (lv) *lv = (int)(int8_t)(*_enc)[so];
    return (int)u32(*_enc, so + 4);
}
// snapshot d'état complet (menu debug)
World::DbgState World::dbg_save() const {
    DbgState s;
    s.hdr = _header; s.pos = _ppos; s.dir = _pdir;
    s.vars = _vm.vars(); s.flags = _vm.flags();
    s.party = _vm.party; s.bag = _vm.bag();
    s.money = _vm.money; s.badges = _vm.badges;
    s.gone = _objgone; s.hide = _objhide; s.lock = _objlock;
    s.follower = _follower;
    return s;
}
void World::dbg_load(const DbgState& s) {
    if (s.hdr >= 0 && s.hdr != _header) load_map(s.hdr, -1);
    _ppos = s.pos; _pdir = s.dir;
    _ptx = _ppos.x; _ptz = _ppos.z; _pty = _ppos.y; _pface = -1;
    _pstep = 0; _vel = {0, 0, 0};
    _vm.import_vars(s.vars); _vm.import_flags(s.flags);
    _vm.party = s.party; _vm.money = s.money; _vm.badges = s.badges;
    for (auto& kv : s.bag) _vm.bag_add(kv.first, kv.second);
    _objgone = s.gone; _objhide = s.hide; _objlock = s.lock;
    _follower = s.follower;
}

int World::_obj_dir(int i) const { return _objd[i]; }

void World::_set_facing(int obj, int dir) {
    if (obj == 0xFF) { _pdir = dir & 3; return; }
    int i = _live_obj(obj);
    if (i >= 0) _objd[i] = dir & 3;
}

// file ApplyMovement : u16 cmd + u16 arg jusqu'à 0xFE — le fichier actif est
// celui routé par _start_script (membre _vm_member, pas forcément le fichier map)
void World::set_pos(float x, float z) {
    _ppos.x = x; _ppos.z = z;
    _vel = {0, 0, 0}; _pstep = 0;
    _wt.st = -1; _phidden = false;              // annule une transition en cours
    _warp_cd_x = (int)floorf(x / TILE);           // pas de faux déclenchement
    _warp_cd_z = (int)floorf(z / TILE);
    _stream();
    float gy = _ground_y(_ppos, 300.f);
    _ppos.y = (gy > -1e7f) ? gy : 40.f;
    _pftrail.clear(); _pfstep = 0;              // suiveur : se replace aussi
    if (_pokefol >= 0) _pokefol_spawn();
}

// début de la 3e ligne visible : même wrap glouton par mots que le rendu
// (boîte DS = 2 lignes, le texte au-delà défile comme une nouvelle page)
size_t World::_dlg_wrap_end(size_t mend) const {
    if (!dlg_measure) return std::string::npos;
    const float WRAPW = 27 * 8.f - 16.f;        // même largeur que main.cpp
    int lines = 1;
    size_t lstart = _dlg_page, j = _dlg_page;
    while (j < mend) {
        char c = _dialog[j];
        if (c == '\n') { j++; lstart = j;
            if (++lines > 2) return lstart; continue; }
        if (c == ' ') { j++; continue; }
        size_t we = j;
        while (we < mend && _dialog[we] != ' ' && _dialog[we] != '\n') we++;
        if (dlg_measure(_dialog, lstart, we) > WRAPW && j > lstart) {
            if (++lines > 2) return j;
            lstart = j;
        }
        j = we;
    }
    return std::string::npos;
}

// fin de la page de dialogue courante : POP — une page = tout le reste du
// message (les coupures DS à 2 lignes / \n\n ne sont plus appliquées ; la
// bulle grandit pour tout afficher d'un coup)
size_t World::dlg_page_end() const {
    return _dialog.size();
}

// nb de caractères visibles dans la page (UTF-8 : compte les octets de tête)
int World::dlg_page_chars() const {
    size_t pe = dlg_page_end();
    int n = 0;
    for (size_t i = _dlg_page; i < pe; i++)
        if ((_dialog[i] & 0xC0) != 0x80) n++;
    return n;
}

// DS : la boîte message appartient au système, pas au script — elle reste
// ouverte après la fin du script jusqu'à une fermeture explicite ou un
// appui A (le prochain A la ferme sans déclencher d'interaction).
// file field-return : prochain type init (4 OnLoad puis 3 OnResume) ; le
// script trouvé remplace le VM — son _script_end_box rappellera ce bloc.
bool World::_fldret_next() {
    while (!_fldret_q.empty()) {
        int t = _fldret_q.front(); _fldret_q.erase(_fldret_q.begin());
        int id = _init_script(t);
        if (id >= 0) { _start_script(id); _vm.step(); return true; }
    }
    return false;
}

void World::_script_end_box() {
    _appr_type = -1;                                    // rencontre dresseur finie
    _appr[0] = ApprSlot(); _appr[1] = ApprSlot();
    _appr_task[0] = ApprTask(); _appr_task[1] = ApprTask();
    if (_mbox && !_dialog.empty()) _dlg_dismiss = true;
    else { _dialog.clear(); _mbox = false; }
    if (_fldret) {
        while (_fldret_next())
            if (_vm.busy()) return;                     // init en cours -> sa
                                                        // fin relance ce bloc
        _fldret = false;
        // FieldMap_Init DS : les objets/props à condition de progression sont
        // réévalués au retour du combat (portail salle Giratina etc.)
        _dw_objs_init(); _dw_spawn_simps();
        if (_fldret_susp) {                             // reprise script combat
            _fldret_susp = false;
            _vm.restore_ctx(std::move(_fldret_ctx));
            _vm.continue_after(ScriptVM::BATTLE); _vm.step();
            if (!_vm.busy()) _script_end_box();
        }
    }
}

// texte dévoilé : _dlg_chars compte des caractères DANS la page courante
// (ne coupe qu'aux frontières UTF-8)
std::string World::dialog_shown() const {
    if (!_mbox || _dlg_page >= _dialog.size()) return "";
    size_t pe = dlg_page_end();
    size_t plen = pe - _dlg_page;
    if (_dlg_chars >= (float)dlg_page_chars())
        return _dialog.substr(_dlg_page, plen);
    int n = (int)_dlg_chars;
    size_t i = _dlg_page;
    while (i < pe && n > 0) {
        i += (_dialog[i] & 0x80) ? ((_dialog[i] & 0xE0) == 0xC0 ? 2
            : ((_dialog[i] & 0xF0) == 0xE0 ? 3 : 4)) : 1;
        n--;
    }
    return _dialog.substr(_dlg_page, i - _dlg_page);
}

// test : force une boîte message (DLGTEST) — réutilise le callback cb_msg
// (formatage + palette) ; pas de script derrière : A referme la boîte
void World::dlg_debug(int bank, int id) {
    _cur_bank = bank;
    if (_vm.cb_msg) _vm.cb_msg(id, true);
    _dlg_dismiss = true;
}

// touche A / interaction
void World::action() {
    if (_yesno || _header < 0 || _wt.st >= 0) return;  // transition : pas d'input
    if (_phm || _cutin_slot >= 0) return;             // tâche/cut-in CS en cours
    if (_dlg_dismiss && !_vm.busy()) {              // boîte laissée ouverte : A ferme
        _dlg_dismiss = false; _mbox = false;
        _dialog.clear(); _dlg_chars = 0.f; _dlg_page = 0;
        return;
    }
    if (_vm.busy()) {
        auto s = _vm.state();
        // DS : appui pendant l'impression -> page entière d'abord
        if (s == ScriptVM::MSG || s == ScriptVM::WAITBTN) {
            float plen = (float)dlg_page_chars();
            if (_dlg_chars < plen) { _dlg_chars = plen; return; }
            // page terminée + il en reste : on avance de page, pas le script
            size_t pe = dlg_page_end();
            if (pe < _dialog.size()) {
                size_t np = pe;                     // saute marqueur \n\n
                while (np < _dialog.size() &&
                       (_dialog[np] == '\n' || _dialog[np] == ' ')) np++;
                _dlg_page = np;
                _dlg_chars = 0.f;
                return;
            }
        }
        if (s == ScriptVM::MSG) { _vm.continue_after(ScriptVM::MSG); _vm.step(); }
        else if (s == ScriptVM::WAITBTN) {
            if (_vm.signpost_dest) {                       // signpost : A/B -> destVar=0
                _vm.set_var(_vm.signpost_dest, 0); _vm.signpost_dest = 0; }
            _vm.continue_after(ScriptVM::WAITBTN);
            if (_vm.close_btn) {                        // CloseMsgOnKeyPress DS
                _vm.close_btn = false;
                _mbox = false; _dialog.clear(); _dlg_chars = 0;
            }
            _vm.step();
        }
        if (!_vm.busy()) _script_end_box();
        if (_warp_req) { _warp_req = false; load_map(_pw.hdr, -1);
            _ppos = {_pw.x * TILE + TILE / 2.f, 60.f, _pw.z * TILE + TILE / 2.f};
            static const int M[4] = {1, 0, 2, 3};     // dir ROM -> monde
            _pdir = M[_pw.dir & 3];
            _warp_cd_x = _pw.x; _warp_cd_z = _pw.z;
            if (_pokefol >= 0) _pokefol_spawn(); }
        return;
    }
    // DS sub_0203C9D4 : objet sur la tuile regardée ; si cette tuile est un
    // comptoir (TILE_BEHAVIOR_TABLE=0x80), on teste la tuile juste derrière.
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    int dx, dz; dirv(_pdir, dx, dz);
    int tx = ptx + dx, tz = ptz + dz;
    int ftx = tx, ftz = tz;                          // tuile regardée (BG)
    if (_tile_beh(tx, tz) == 0x80) { tx += dx; tz += dz; }   // comptoir
    const Nds::Zev::Obj* best = nullptr; int bi = -1;
    for (size_t i = 0; i < _objects.size(); i++) {
        auto& o = _objects[i];
        if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
        if (o.script == 0xFFFF) continue;
        int ox = (int)floorf(_objpos[i].x / TILE);
        int oz = (int)floorf(_objpos[i].z / TILE);
        if (ox == tx && oz == tz) { best = &o; bi = (int)i; break; }
    }
    if (verbose)
        printf("[act] f=%d p=(%d,%d) d=%d t=(%d,%d) objs=%zu\n",
            _dbg_frames, ptx, ptz, _pdir, tx, tz, _objects.size());
    if (!best) {
        _talk_obj = -1;                      // pas un PNJ : pas de locuteur
        // Pokémon suiveur : A face à lui -> réaction (avant les événements BG)
        if (pokefol_shown()) {
            int ftx2 = (int)floorf(_pfpos.x / TILE);
            int ftz2 = (int)floorf(_pfpos.z / TILE);
            if (ftx2 == tx && ftz2 == tz) { _pokefol_talk(); return; }
        }
        // panneaux / événements BG
        // DS : FieldEvent_GetInteractedBgEventScript — uniquement la tuile
        // regardée (tx,tz). playerFacingDir : 0=N,1=E,2=W,3=S,4=ALL,5=W/E,6=N/S.
        static const int W2R[4] = {3, 0, 2, 1};    // S,N,W,E -> enum ROM
        for (auto& b : _bgs) {
            if ((int)b.x != ftx || (int)b.z != ftz) continue;
            // objet caché déjà ramassé ? (flag = script-8000 + 730)
            if (b.type == 2 && b.script >= 8000 && b.script < 8900 &&
                _vm.flag(b.script - 8000 + 730)) continue;
            if (b.type != 2) {                     // HIDDEN_ITEM ignore le sens
                int rd = W2R[_pdir & 3];
                bool ok = (b.dir == 4) || (b.dir == rd) ||
                          (b.dir == 5 && (rd == 2 || rd == 3)) ||
                          (b.dir == 6 && (rd == 0 || rd == 1));
                if (!ok) continue;
            }
            if (verbose) printf("[bg] fire (%d,%d) scr=%d t=(%d,%d)\n",
                (int)b.x, (int)b.z, b.script, tx, tz);
            _start_script(b.script); _vm.step();
            if (!_vm.busy()) _script_end_box();
            return;
        }
        // Villa : FieldSystem_TrySetVillaFurnitureScript DS — meuble solide
        // possédé sur la tuile regardée -> script 0x15+type (TV : 10100,
        // face nord uniquement).
        if (_gym.kind == 8) {
            for (const VillaFurn& f : VILLA_FURN) {
                if (!f.s || !_vm.flag(2461 + f.t)) continue;
                if (tx < f.l || tx > f.r || tz < f.tp || tz > f.b) continue;
                int sid = f.t == 5 ? (_pdir == 1 ? 10100 : -1)
                                 : 0x15 + f.t;
                if (sid < 0) continue;
                if (sid == 10100) { _tv_open = true; return; } // écran Motisma
                _start_script(sid); _vm.step();
                if (!_vm.busy()) _script_end_box();
                return;
            }
        }
        // DS Field_TileBehaviorToScript : comportement de la tuile regardée
        // (PC/étagères/poubelle/rayons/télé/cascade/carte/parking vélo).
        int beh = _tile_beh(ftx, ftz), sid = 0xFFFF;
        if (verbose || getenv("BEHDBG"))
            printf("[beh] faced=(%d,%d) beh=%02x\n", ftx, ftz, beh);
        // DS FieldMoves : face à une tuile surfable + badge Fen (bit 3) + un
        // Pokémon capable de Surf -> script 10004 (« Voulez-vous surfer ? »)
        if (!_psurf && _surfable(beh) && (_vm.badges & 8) && _party_can_surf())
            sid = 10004;
        // DS PlayerAvatar_CanUseRockClimb : paroi N/S (0x4B) face nord/sud,
        // paroi E/W (0x4C) face est/ouest -> script 10003 (field_moves fn3).
        else if ((beh == 0x4B && _pdir <= 1) ||
                 (beh == 0x4C && _pdir >= 2)) sid = 10003;
        else switch (beh) {
        case 0x83: if (_pdir == 1) sid = 2018; break;        // PC (face nord)
        case 0xE0: sid = 2500; break;                        // petite étagère 1
        case 0xEA: sid = 2501; break;                        // petite étagère 2
        case 0xE1: sid = 2502; break;                        // étagère 1
        case 0xE2: sid = 2503; break;                        // étagère 2
        case 0xE4: sid = 2504; break;                        // poubelle
        case 0xE5: sid = 2505; break;                        // rayon magasin 1
        case 0xEB: sid = 2506; break;                        // rayon magasin 2
        case 0xEC: sid = 2507; break;                        // rayon magasin 3
        case 0x85: sid = 2508; break;                        // carte de la ville
        case 0x86: if (_pdir == 1) { _tv_open = true; return; } break; // Télé-Motisma
        case 0xDB: sid = 2030; break;                        // parking à vélos
        case 0x13: sid = 10006; break;                       // cascade
        }
        if (sid != 0xFFFF) {
            if (verbose) printf("[beh] beh=%02x scr=%d\n", beh, sid);
            _start_script(sid); _vm.step();
            if (!_vm.busy()) _script_end_box();
        }
        return;
    }
    // le PNJ se tourne vers le joueur (et le joueur vers lui)
    int fx = ptx - (int)floorf(_objpos[bi].x / TILE);
    int fz = ptz - (int)floorf(_objpos[bi].z / TILE);
    _objd[bi] = abs(fz) >= abs(fx) ? (fz > 0 ? 0 : 1) : (fx < 0 ? 2 : 3);
    _pdir = _objd[bi] ^ 1;
    _talk_obj = bi;
    _vm.set_var(0x800D, best->id);              // VAR_LAST_TALKED (RemoveObject)
    if (verbose) printf("[obj] talk id=%d scr=%d\n", best->id, best->script);
    _start_script(best->script); _vm.step();
    if (!_vm.busy()) _script_end_box();
    if (_warp_req) { _warp_req = false; load_map(_pw.hdr, -1);
        _ppos = {_pw.x * TILE + TILE / 2.f, 60.f, _pw.z * TILE + TILE / 2.f};
        static const int M[4] = {1, 0, 2, 3};         // dir ROM -> monde
        _pdir = M[_pw.dir & 3];
        _warp_cd_x = _pw.x; _warp_cd_z = _pw.z;
        if (_pokefol >= 0) _pokefol_spawn(); }
}

void World::choose_yesno(bool yes) {
    if (_vm.state() == ScriptVM::YESNO) {
        _vm.set_var(_vm.pending_var, yes ? 0 : 1);          // 0=oui 1=non
        _yesno = false;
        _vm.continue_after(ScriptVM::YESNO); _vm.step();
        if (!_vm.busy()) _script_end_box();
    }
}

void World::menu_pick(int slot) {              // choix -> var dest -> reprend
    if (_vm.state() == ScriptVM::MENU) { _vm.menu_pick(slot); _vm.step(); }
}
const std::string& World::menu_text(int id) {  // texte d'entrée
    static std::string unk = "?";
    // menu global (InitGlobalTextMenu) : TEXT_BANK_MENU_ENTRIES fixe (361) ;
    // sinon banque du script courant.
    auto& b = _bank_msgs(_vm.menu_bank >= 0 ? _vm.menu_bank : _cur_bank);
    if (id < 0 || id >= (int)b.size()) return unk;
    static std::string fmt;                     // {tag:arg} expansés (Buffer*)
    fmt = _fmt_msg(b[id]);
    return fmt;
}
const std::string& World::item_name(int id) {  // nom d'objet FR (pl_msg 392)
    static std::string unk = "???";
    auto& b = _bank_msgs(392);
    return (id >= 0 && id < (int)b.size()) ? b[id] : unk;
}
const std::string& World::species_name(int id) { // nom d'espèce FR (pl_msg 412)
    static std::string unk = "???";
    auto& b = _bank_msgs(412);
    return (id >= 0 && id < (int)b.size()) ? b[id] : unk;
}
// stockage PC : déplacements équipe <-> boîtes (18 x 30, slots plats)
void World::pc_move_party_to_box(int i, int box) {
    if (i < 0 || i >= (int)_vm.party.size() || _vm.party.size() < 2)
        return;                               // jamais le dernier Pokémon
    int slot = -1;                            // boîte vue d'abord, sinon ordre
    for (int s = box * 30; s < box * 30 + 30 && slot < 0; s++)
        if (!_vm.pcbox[s].species) slot = s;
    for (int s = 0; s < 540 && slot < 0; s++)
        if (!_vm.pcbox[s].species) slot = s;
    if (slot < 0) return;
    _vm.pcbox[slot] = _vm.party[i];
    _vm.party.erase(_vm.party.begin() + i);
}
void World::pc_move_box_to_party(int i) {
    if (i < 0 || i >= 540 || !_vm.pcbox[i].species ||
        _vm.party.size() >= 6) return;
    _vm.party.push_back(_vm.pcbox[i]);
    _vm.pcbox[i] = ScriptVM::PartyMon();
}
const char* World::box_name(int i) {
    static char n[16];
    snprintf(n, sizeof(n), "BOITE %d", i + 1);
    return n;
}
const std::string& World::msg(int bank, int idx) {     // chaîne FR quelconque
    static std::string unk = "";
    auto& b = _bank_msgs(bank);
    return (idx >= 0 && idx < (int)b.size()) ? b[idx] : unk;
}
int World::item_pocket(int id) {                       // poche sac réelle
    static std::unordered_map<int,int> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    int pk = 0;
    auto n = _db->narc("/itemtool/itemdata/pl_item_data.narc");
    if (n && id >= 0 && id < n->count()) {
        const Bytes& d = n->member(id);
        if (d.size() >= 10) pk = (u16(d, 8) >> 7) & 0xF;
    }
    cache[id] = pk;
    return pk;
}
int World::item_price(int id) const {          // prix DS : pl_item_data u16@0
    auto n = _db->narc("/itemtool/itemdata/pl_item_data.narc");
    if (!n || id < 0 || id >= n->count()) return 0;
    const Bytes& d = n->member(id);
    return d.size() >= 2 ? (int)u16(d, 0) : 0;
}
// ── sac DS : métadonnées objets ──────────────────────────────────────────────
std::vector<std::pair<int,int>> World::bag_items(int pocket) {
    std::vector<std::pair<int,int>> out;
    std::unordered_set<int> seen;
    for (int id : _bag_seq) {                    // ordre d'obtention conservé
        auto it = _vm.bag().find(id);
        if (it != _vm.bag().end() && item_pocket(id) == pocket) {
            out.push_back(*it); seen.insert(id);
        }
    }
    for (auto& kv : _vm.bag())                   // objets ajoutés depuis
        if (!seen.count(kv.first) && item_pocket(kv.first) == pocket) {
            out.push_back(kv); _bag_seq.push_back(kv.first);
        }
    return out;
}
int World::item_heal(int id) {                   // soin PV : pl_item_data u8@27
    auto n = _db->narc("/itemtool/itemdata/pl_item_data.narc");
    if (!n || id < 0 || id >= n->count()) return 0;
    const Bytes& d = n->member(id);
    return d.size() >= 28 ? d[27] : 0;           // 0xFE=rappel moitié, FF=plein
}
static int item_u8(const Bytes& d, int off) {
    return (int)d.size() > off ? d[off] : 0;
}
int World::item_field_use(int id) {              // utilisable sur le terrain
    if (id == 450) return 5;                     // Bicyclette : montage/démontage
    if (id == 76 || id == 77 || id == 79) {      // Repousse DS : 200/250/100 pas
        _repel = id == 77 ? 250 : id == 76 ? 200 : 100;
        _repel_item = id;
        return 4;
    }
    if (item_pocket(id) != 1) return 0;          // médicaments uniquement
    auto n = _db->narc("/itemtool/itemdata/pl_item_data.narc");
    if (!n || id < 0 || id >= n->count()) return 0;
    const Bytes& d = n->member(id);
    if (item_u8(d, 15) & 2) return 3;            // Cendresacrée : rappel groupe
    if (item_u8(d, 15) & 1) return 2;            // Rappel : ranime un KO
    if (item_u8(d, 27) || item_u8(d, 14)) return 1;   // soin PV / statut
    return 0;
}
bool World::item_tossable(int id) {
    return item_pocket(id) != 7;                 // objets rares : jamais jetés
}
bool World::item_registrable(int id) {
    return item_pocket(id) == 7;                 // objets rares -> touche Y (DS)
}
int World::mon_maxhp(int species, int lv, uint32_t ivs, int ev) {  // PV max : pl_personal byte0
    int iv = ivs == 0xFFFFFFFFu ? 16 : iv_at(ivs, 0);      // non tirés : IV moyen
    auto n = _db->narc("/poketool/personal/pl_personal.narc");
    if (n && species > 0 && species < n->count()) {
        const Bytes& d = n->member(species);
        if (d.size() >= 6)
            return d[0] == 1 ? 1 : (2 * d[0] + iv + ev / 4) * lv / 100 + lv + 10;
    }
    return lv * 2 + 10;
}

// ── CS / déplacements de terrain (DS map_tile_behavior.c) ────────────────────
// TILE_BEHAVIOR_FLAG_SURFABLE : rivière/mer/cascade + variantes (tiles sur les-
// quelles on peut monter en Surf). Le bit "encounter" distingue l'eau peuplée.
bool World::_surfable(int beh) {
    switch (beh) {
    case 0x10: case 0x11: case 0x12:                    // rivière (+variantes)
    case 0x13: case 0x14:                             // cascade
    case 0x15:                                        // mer
    case 0x19: case 0x22: case 0x2A:                    // autres eaux surfables
    case 0x50: case 0x51: case 0x52: case 0x53:         // eau monde distorsion
        return true;
    }
    return false;
}

// compatibilité CS : pl_personal bytes 28-39 = bitfield CT01-92 + CS01-08
// (bit 92+n = CS n). DS PartyMon compat, pas « connaît » (moves non stockés).
bool World::_mon_learns_hm(const ScriptVM::PartyMon& m, int hm) const {
    if (hm < 1 || hm > 8) return false;
    auto n = _db->narc("/poketool/personal/pl_personal.narc");
    if (!n || m.species <= 0 || m.species >= n->count()) return false;
    const Bytes& d = n->member(m.species);
    if (d.size() < 41) return false;
    int bit = 91 + hm;                                // CS03 (Surf) -> bit 94
    return (d[28 + bit / 8] >> (bit & 7)) & 1;        // champ CT/CS @0x1C
}

// PlayerHasUsableFieldMove : un membre de l'équipe peut faire Surf (CS03).
bool World::_party_can_surf() const {
    for (auto& m : _vm.party) {
        if (m.hp <= 0) continue;
        auto bm = battle_make(_db, m.species, m.level, nullptr, m.ivs);
        for (int k = 0; k < bm.nmoves; k++) if (bm.moves[k] == 57) return true;
        if (_mon_learns_hm(m, 3)) return true;        // sait/peut apprendre CS03
    }
    return false;
}

// ── vélo (player_move.c / item_use_functions.c DS) ───────────────────────────
void World::set_bike(bool on) {
    _vm.sys_bike = on;
    _pstate = on ? 1 : (_pstate == 1 ? 0 : _pstate);
    _bspd = 0; _bbrake = false; _bslope = 0;
    if (on && _snd) {                            // SEQ_BICYCLE DS (FieldBGM_SetOverride)
        int s = _snd->seq_named("SEQ_BICYCLE");
        if (s < 0) s = _snd->seq_named("SEQ_PL_BICYCLE");
        set_bgm_override(s);
    } else set_bgm_override(-1);
}

int World::use_bicycle() {                       // CanUseBicycle DS
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    int beh = _tile_beh(ptx, ptz);
    if (_pstate == 1) {                          // démontage
        if (_bon_road || (beh >= 0x76 && beh <= 0x7D)) return 3;
        set_bike(false);
        return 1;
    }
    const MapHeader* H = _db->header(_header);   // MapHeader_IsBikeAllowed
    if (!H || !H->bike || _psurf || _pstate == 2 ||
        beh == 0x03 || beh == 0xA4 || beh == 0xA5 || beh == 0xA6)
        return 2;                                // haute herbe/boue/surf/map
    set_bike(true);
    return 0;
}

void World::bike_gear() {                        // B : 3e <-> 4e (sauf rampe)
    if (_pstate != 1) return;
    int beh = _tile_beh((int)floorf(_ppos.x / TILE),
        (int)floorf(_ppos.z / TILE));
    if (beh == 0xD7 || beh == 0xD8) return;
    _bgear ^= 1;
    if (_snd) {
        int s = _snd->seq_named(_bgear ? "SEQ_SE_DP_GEAR" : "SEQ_SE_DP_GEAR2");
        if (s >= 0) _snd->se(s);
    }
}

// ── CS de terrain (FieldMoveTasks / menu équipe DS) ───────────────────
// Ordre DS FIELD_MOVE_* : Coupe Vol Surf Force Anti-Brume Éclate-Roc
// Cascade Escalade Flash Téléportation Tunnel Dolce-Parfum Babillard
// Lait Meumeu Coquelet.
static const int FM_MOVE_IDS[15] = {
    15, 19, 57, 70, 432, 249, 127, 431, 148, 100, 91, 230, 448, 208, 135 };
int World::field_move_id(int fm) const {
    return (fm >= 0 && fm < 15) ? FM_MOVE_IDS[fm] : 0;
}
int World::_facing_obj() const {                  // DS sub_0203C9D4
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    int dx, dz; dirv(_pdir, dx, dz);
    int tx = ptx + dx, tz = ptz + dz;
    if (_tile_beh(tx, tz) == 0x80) { tx += dx; tz += dz; }   // comptoir
    for (size_t i = 0; i < _objects.size(); i++) {
        if (_objgone.count((int)i) || _objhide.count((int)i)) continue;
        int ox = (int)floorf(_objpos[i].x / TILE);
        int oz = (int)floorf(_objpos[i].z / TILE);
        if (ox == tx && oz == tz) return (int)i;
    }
    return -1;
}
std::vector<int> World::party_field_moves(int slot) const {
    std::vector<int> r;                              // DS GetContextMenuEntries
    if (slot < 0 || slot >= (int)_vm.party.size()) return r;
    auto& m = _vm.party[slot];
    if (m.egg) return r;                             // œuf : pas de CS
    for (int i = 0; i < m.nmoves && i < 4 && m.moves[i]; i++)
        for (int f = 0; f < 15; f++)
            if (m.moves[i] == FM_MOVE_IDS[f]) { r.push_back(f); break; }
    return r;
}
int World::usable_field_moves() const {            // DS FieldMoves_SetUsableMoves
    int u = 0, o = _facing_obj();
    if (o >= 0) switch (_resolve_gfx(_objects[o].gfx)) {
    case 84: u |= 1 << 3; break;                    // rocher Force
    case 85: u |= 1 << 5; break;                    // rocher Éclate-Roc
    case 86: u |= 1 << 0; break;                    // arbuste Coupe
    }
    int ptx = (int)floorf(_ppos.x / TILE), ptz = (int)floorf(_ppos.z / TILE);
    int dx, dz; dirv(_pdir, dx, dz);
    int nbeh = _tile_beh(ptx + dx, ptz + dz);       // tuile regardée
    if (_surfable(nbeh)) u |= 1 << 2;               // CanUseSurf (ponts omis)
    if ((nbeh == 0x4B && _pdir <= 1) || (nbeh == 0x4C && _pdir >= 2))
        u |= 1 << 7;                                // CanUseRockClimb
    if (nbeh == 0x13) u |= 1 << 6;                  // cascade
    if (_wthr_base == 14) u |= 1 << 4;              // brume -> Anti-Brume
    if (_wthr_base == 16) u |= 1 << 8;              // grotte sombre -> Flash
    return u;
}
int World::field_move_error(int fm) const {        // DS FieldMoves_Check*
    const MapHeader* H = _header >= 0 ? _db->header(_header) : nullptr;
    int u = usable_field_moves();
    bool partner = _vm.flag(2408);                  // FLAG_HAS_PARTNER
    bool safpp = _vm.flag(2414) || _vm.flag(2432);  // SAFARI | PAL_PARK
    bool surfing = _pstate == 2 || _psurf;          // PLAYER_AVATAR_SURFING
    switch (fm) {
    case 0:                                         // Coupe : badge Forêt
        if (!(_vm.badges & 2)) return 2;
        return (u & 1) ? 0 : 1;
    case 1:                                         // Vol : badge Roc
        if (!(_vm.badges & 4)) return 2;
        if (!H || !H->fly) return 1;
        if (partner) return 3;
        return safpp ? 1 : 0;
    case 2:                                         // Surf : badge Marais
        if (!(_vm.badges & 8)) return 2;
        if (surfing) return 4;
        if (!(u & 4)) return 1;
        return partner ? 3 : 0;
    case 3:                                         // Force : badge Mine
        if (!(_vm.badges & 32)) return 2;
        return (u & 8) ? 0 : 1;
    case 4:                                         // Anti-Brume : badge Fantôme
        if (!(_vm.badges & 16)) return 2;
        return (u & 16) ? 0 : 1;
    case 5:                                         // Éclate-Roc : badge Charbon
        if (!(_vm.badges & 1)) return 2;
        if (surfing) return 1;
        return (u & 32) ? 0 : 1;
    case 6:                                         // Cascade : badge Glaçon
        if (!(_vm.badges & 128)) return 2;
        return (u & 64) ? 0 : 1;
    case 7:                                         // Escalade : badge Frimas
        if (!(_vm.badges & 64)) return 2;
        if (!(u & 128)) return 1;
        return partner ? 3 : 0;
    case 8:                                         // Flash : grotte sombre
        return (u & 256) ? 0 : 1;
    case 9:                                         // Téléportation
        if (!H || !H->fly || H->mtype == 1) return 1; // flyAllowed && !ville
        if (partner) return 3;
        return safpp ? 1 : 0;
    case 10:                                        // Tunnel : grotte + corde
        if (!H || H->mtype != 3 || !H->rope) return 1;
        return partner ? 3 : 0;
    case 11: return _vm.flag(2432) ? 1 : 0;         // Dolce-Parfum : hors Pal Park
    case 12: return _dw.on ? 1 : 0;                 // Babillard : hors Distorsion
    case 13: case 14: return 0;                     // Lait Meumeu / Coquelet
    }
    return 1;
}
int World::field_move_script(int fm) const {       // tâches scriptées DS
    switch (fm) {
    case 0: return 10008;                           // Coupe (menu)
    case 5: return 10009;                           // Éclate-Roc
    case 3: return 10010;                           // Force
    case 7: return 10011;                           // Escalade
    case 2: return 10012;                           // Surf
    case 6: return 10013;                           // Cascade
    case 4: return 10014;                           // Anti-Brume
    case 8: return 10015;                           // Flash
    }
    return -1;      // Vol/Téléportation/Tunnel/Dolce-Parfum/Babillard : tâches dédiées
}
bool World::use_field_move(int slot, int fm) {     // FieldMoves_*Task DS
    int sid = field_move_script(fm);
    if (sid < 0) {                                  // tâche dédiée DS : cut-in
        int after = fm == 9 ? 1 : fm == 10 ? 2 : fm == 11 ? 3 : 0;
        if (!after) return false;                   // puis Téléport/Tunnel/DP
        _cutin_slot = slot; _cutin_f = 0.f; _cutin_cry = false;
        _cutin_sp = (slot >= 0 && slot < (int)_vm.party.size())
            ? _vm.party[slot].species : 0;
        _cutin_after = after;
        return true;
    }
    _vm.set_var(0x8000, slot);                      // SCRIPT_DATA_PARAMETER_0
    int o = _facing_obj();                          // RemoveObject VAR_LAST_TALKED
    _vm.set_var(0x800D, o >= 0 ? _objects[o].id : 0);
    _start_script(sid); _vm.step();
    if (!_vm.busy()) _script_end_box();
    return true;
}
const std::string& World::fm_error_text(int err) { // banque party_menu (453) DS
    static std::string unk = "?";
    static const int IDX[5] = {104, 104, 76, 196, 102};
    auto& b = _bank_msgs(453);
    int i = (err >= 0 && err <= 4) ? IDX[err] : 104;
    return (i >= 0 && i < (int)b.size()) ? b[i] : unk;
}
void World::shop_buy(int idx) {                // achat -> argent + sac
    if (idx < 0 || idx >= (int)_vm.shop_items.size()) return;
    int id = _vm.shop_items[idx], p = item_price(id);
    if (_vm.money < (uint32_t)p) return;
    _vm.money -= p; _vm.bag_add(id, 1);
    if (verbose) printf("[shop] achat %s (-%d, reste %u)\n",
        item_name(id).c_str(), p, _vm.money);
}
void World::shop_close() {                     // quitte la boutique -> script reprend
    _vm.continue_after(ScriptVM::SHOP); _vm.step();
}
float World::fade_alpha() const {              // alpha du fondu écran
    if (_fade.t < 0 || _fade.dur <= 0) return 0;
    float k = _fade.t / _fade.dur;
    return _fade.out ? k : 1.f - k;
}
