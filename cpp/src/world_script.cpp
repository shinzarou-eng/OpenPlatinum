// world_script.cpp — VM scripts, dialogue, events, sac/boutique.
#include "world.h"
#include "world_internal.h"
#include "msg.h"
#include "spr.h"
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
    if (!_scrnarc) return;
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
    // objet caché (8000+) : remplit VAR_0x8000/01/02 = item/qté/flag (gHiddenItems)
    if (id >= 8000 && id < 8900) {
        int fidx = id - 8000, item = 0, qty = 1;
        if (fidx == 201) { item = 111; qty = 1; }    // Twinleaf : Clé de Voûte
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
    _ppos = s.pos; _pdir = s.dir; _ptx = _ppos.x; _ptz = _ppos.z;
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
}

// fin de la page de dialogue courante : prochain marqueur page DS (\n\n,
// décodé depuis 0x25BC/BD) ou fin du message
size_t World::dlg_page_end() const {
    if (_dlg_page >= _dialog.size()) return _dialog.size();
    size_t p = _dialog.find("\n\n", _dlg_page);
    return p == std::string::npos ? _dialog.size() : p;
}

// texte dévoilé : _dlg_chars compte des caractères DANS la page courante
// (ne coupe qu'aux frontières UTF-8)
std::string World::dialog_shown() const {
    if (!_mbox || _dlg_page >= _dialog.size()) return "";
    size_t pe = dlg_page_end();
    size_t plen = pe - _dlg_page;
    if (_dlg_chars >= (float)plen)
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

// touche A / interaction
void World::action() {
    if (_yesno || _header < 0 || _wt.st >= 0) return;  // transition : pas d'input
    if (_vm.busy()) {
        auto s = _vm.state();
        // DS : appui pendant l'impression -> page entière d'abord
        if (s == ScriptVM::MSG || s == ScriptVM::WAITBTN) {
            size_t plen = dlg_page_end() - _dlg_page;
            if (_dlg_chars < (float)plen) { _dlg_chars = (float)plen; return; }
            // page terminée + il en reste : on avance de page, pas le script
            size_t pe = dlg_page_end();
            if (pe < _dialog.size()) {
                _dlg_page = pe + 2;                 // saute le marqueur \n\n
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
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
        if (_warp_req) { _warp_req = false; load_map(_pw.hdr, -1);
            _ppos = {_pw.x * TILE + TILE / 2.f, 60.f, _pw.z * TILE + TILE / 2.f};
            static const int M[4] = {1, 0, 2, 3};     // dir ROM -> monde
            _pdir = M[_pw.dir & 3];
            _warp_cd_x = _pw.x; _warp_cd_z = _pw.z; }
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
        printf("[act] p=(%d,%d) d=%d t=(%d,%d) objs=%zu\n",
            ptx, ptz, _pdir, tx, tz, _objects.size());
    if (!best) {                                   // panneaux / événements BG
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
            if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
            return;
        }
        // DS Field_TileBehaviorToScript : comportement de la tuile regardée
        // (PC/étagères/poubelle/rayons/télé/cascade/carte/parking vélo).
        int beh = _tile_beh(ftx, ftz), sid = 0xFFFF;
        // DS FieldMoves : face à une tuile surfable + badge Fen (bit 3) + un
        // Pokémon capable de Surf -> script 10004 (« Voulez-vous surfer ? »)
        if (!_psurf && _surfable(beh) && (_vm.badges & 8) && _party_can_surf())
            sid = 10004;
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
        case 0x86: if (_pdir == 1) sid = 10100; break;       // TV (face nord)
        case 0xDB: sid = 2030; break;                        // parking à vélos
        case 0x13: sid = 10006; break;                       // cascade
        }
        if (sid != 0xFFFF) {
            if (verbose) printf("[beh] beh=%02x scr=%d\n", beh, sid);
            _start_script(sid); _vm.step();
            if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
        }
        return;
    }
    // le PNJ se tourne vers le joueur (et le joueur vers lui)
    int fx = ptx - (int)floorf(_objpos[bi].x / TILE);
    int fz = ptz - (int)floorf(_objpos[bi].z / TILE);
    _objd[bi] = abs(fz) >= abs(fx) ? (fz > 0 ? 0 : 1) : (fx < 0 ? 2 : 3);
    _pdir = _objd[bi] ^ 1;
    _talk_obj = bi;
    if (verbose) printf("[obj] talk id=%d scr=%d\n", best->id, best->script);
    _start_script(best->script); _vm.step();
    if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
    if (_warp_req) { _warp_req = false; load_map(_pw.hdr, -1);
        _ppos = {_pw.x * TILE + TILE / 2.f, 60.f, _pw.z * TILE + TILE / 2.f};
        static const int M[4] = {1, 0, 2, 3};         // dir ROM -> monde
        _pdir = M[_pw.dir & 3];
        _warp_cd_x = _pw.x; _warp_cd_z = _pw.z; }
}

void World::choose_yesno(bool yes) {
    if (_vm.state() == ScriptVM::YESNO) {
        _vm.set_var(_vm.pending_var, yes ? 0 : 1);          // 0=oui 1=non
        _yesno = false;
        _vm.continue_after(ScriptVM::YESNO); _vm.step();
        if (!_vm.busy()) { _dialog.clear(); _mbox = false; }
    }
}

void World::menu_pick(int slot) {              // choix -> var dest -> reprend
    if (_vm.state() == ScriptVM::MENU) { _vm.menu_pick(slot); _vm.step(); }
}
const std::string& World::menu_text(int id) {  // texte d'entrée (banque courante)
    static std::string unk = "?";
    auto& b = _bank_msgs(_cur_bank);
    return (id >= 0 && id < (int)b.size()) ? b[id] : unk;
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
int World::mon_maxhp(int species, int lv, int iv, int ev) {  // PV max : pl_personal byte0
    if (iv < 0) iv = 16;
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
        auto bm = battle_make(_db, m.species, m.level, nullptr, m.iv);
        for (int k = 0; k < bm.nmoves; k++) if (bm.moves[k] == 57) return true;
        if (_mon_learns_hm(m, 3)) return true;        // sait/peut apprendre CS03
    }
    return false;
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
