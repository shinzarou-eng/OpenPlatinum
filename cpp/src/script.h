// script.h — VM des scripts de terrain DPPt (fielddata/script/scr_seq.narc).
// Format fichier : table de u32 (offset relatif à fin d'entrée) + u16 0xFD13 ;
// commandes = u16 opcode + paramètres (b/h/w) ; sauts relatifs à fin de commande.
#pragma once
#include "common.h"
#include <functional>
#include <unordered_map>
#include <vector>

class ScriptVM {
public:
    struct OpDef { uint16_t op; const char* name; const char* widths; };
    enum St { DONE, RUN, MSG, WAITBTN, YESNO, MOVE, WAITTIME, WAITFADE, MENU,
              STARTER, BATTLE, SHOP, WAITANIM, WAITSND, PCBOX };

    bool load(const Bytes* file);            // membre scr_seq (réf stable)
    bool load_std(const Bytes* file);        // scripts_common (CallStandard)
    void start(int fn);                      // index de fonction du fichier
    St  state() const { return _st; }
    bool busy() const { return _st != DONE; }
    St  step();                              // exécute jusqu'à attente/fin
    void continue_after(St s) { if (_st == s) _st = RUN; }   // relance d'attente

    // callbacks connectés par World
    // (cb_msg déplacé près des callbacks fenêtre plus bas)
    std::function<void(int obj, size_t off)>        cb_move;     // ApplyMovement
    std::function<void(int hdr,int x,int z,int dir)> cb_warp;    // Warp
    std::function<void()>                           cb_face;     // FacePlayer
    std::function<void(int id, bool show)>          cb_people;   // AddPeople/RemovePeople
    std::function<void(const char* name,int op)>    cb_unimpl;   // debug
    // boîte message : id + instant (MessageInstant/texte d'un coup), ouvert/fermé
    std::function<void(int id,bool instant)>        cb_msg;
    std::function<void(bool open)>                  cb_box;      // OpenMessage/CloseMessage
    // buffers StringTemplate : (slot, kind, val)
    // kinds : 0=héros 1=rival 2=counterpart 10=item 11=item+article
    //         20=nombre 30=espèce 40=capacité 50=poche 60=pokétch
    //         70=UG goods 71=UG piège 72=UG trésor
    std::function<void(int slot,int kind,int val)>  cb_buf;
    std::function<void(int trans,int frames,int type,int color)> cb_fade; // FadeScreen
    std::function<int(int axis)>                    cb_pos;      // GetPlayerMapPos (0=x,1=z)
    std::function<int()>                            cb_pdir;     // GetPlayerDir (dir ROM)
    std::function<void(int obj,int x,int z,int dir,int y)> cb_setpos; // SetPosition (vivant)
    std::function<void(int obj,int x,int z)>        cb_evpos;    // SetObjectEventPos (template)
    std::function<void(int idx,int x,int z)>        cb_warpev;   // SetWarpEventPos (template)
    std::function<void(int idx,int x,int z)>        cb_bgev;     // SetBgEventPos (template)
    std::function<void(int obj,int kind,int val)>   cb_ow;       // SetObjectEvent*/SetMovementType
    std::function<void(int what)>                   cb_misc;     // 0=RunningShoes
    std::function<void(int id,bool lock)>           cb_lockobj;  // Lock/Release
    std::function<void(int cmd,int a,int b)>        cb_follow;   // follow system
    std::function<void()>                           cb_lostpc;   // LostGoPc
    std::function<void()>                           cb_heal;     // HealParty (soigne l'équipe)
    std::function<void(int)>                        cb_setbo;    // SetBlackOutWarpId -> respawn
    std::function<void(int species,bool on)>        cb_pic;      // PokemonPicture
    std::function<void(int tid,int msgType)>        cb_trainermsg; // PrintTrainerDialogue
    std::function<int(int tid)>                     cb_trdata;   // TRDATA_BATTLE_TYPE
    std::function<int(int ref)>                     cb_objmt;    // GetMovementType obj
    std::function<int(int set)>                     cb_targetflag; // flag 1364+trainerID cible
    // commandes de porte (ov5_021D431C.c) : LoadDoorAnimation mapX,mapZ,
    // tileX,tileZ,tag ; PlayOpen/Close tag ; WaitForAnimation -> WAITANIM
    std::function<void(int,int,int,int,int)>        cb_doorload;
    std::function<void(int,bool)>                   cb_dooranim;  // tag, open?
    std::function<int(int)>                         cb_doorbusy;  // tag -> anim ?
    std::function<void()>                           cb_surf;      // UseSurf -> monter sur l'eau
    std::function<int(int)>                         cb_findmove;  // FindPartySlotWithMove
    std::function<void(int)>                        cb_doorfree;  // UnloadAnimation

    // équipe + starters (Sinnoh : 387 Tortipouss / 390 Ouisticram / 393 Tiplouf)
    struct PartyMon {
        int species = 0, level = 1, hp = 1, iv = -1;
        uint32_t pid = 0, exp = 0;          // PID (nature) + points d'exp
        uint8_t evs[6] = {0};               // EVs par stat (255/stat, 510 total)
        int friendship = 70;                // amitié (évos bonheur)
        int moves[4] = {0}, pp[4] = {0}, nmoves = 0;  // moves persistants
    };
    std::vector<PartyMon> party;
    // stockage PC : 18 boîtes x 30 slots fixes (species==0 = slot vide)
    std::vector<PartyMon> pcbox = std::vector<PartyMon>(540);
    int pc_mode = 0;                           // BoxMode OpenPcFunction (0-4)
    int chosen_starter = 0, rival_starter = 0, cpart_starter = 0;
    static int starter_id(int i);                    // 0-2 -> espèce
    void starter_pick(int i);                        // choix -> reprend le script
    void force_starter() { _st = STARTER; }          // test écran starter
    int cur_script = 0;                            // SCRIPT_MANAGER_SCRIPT_ID
    int battle_tid = 0, battle_lost = 0;           // StartFirstBattle / CheckWonBattle
    int battle_sp = 0, battle_lv = 0;              // StartWildBattle/StartLegendaryBattle

    // état système DS (sauvegarde à faire plus tard)
    int      badges = 0;                           // bitmask 8 badges
    uint32_t money = 3000;                         // argent de départ DS
    int      coins = 0;                            // jetons casino
    bool     sys_poketch = false, sys_bike = false;
    std::function<int()> cb_runstate;              // chaussures de sport acquises
    std::function<int()> cb_mapid;                 // map courante (GetCurrentMapID)

    // variables + flags (persistants entre maps)
    int  var(int v) const;
    void set_var(int v, int val);
    bool flag(int f) const;
    void set_flag(int f, bool on);
    int  result = 0;                          // registre de comparaison ctx->data[0]
    int  pending_var = 0;                     // var cible d'un YesNoBox
    int  signpost_dest = 0;                   // var cible GetSignpostInput (0 si actif)
    int  wait_t = 0;                          // compteur WaitTime (frames)
    bool close_btn = false;                   // CloseMsgOnKeyPress : touche -> ferme la boîte
    int  wait_anim = -1;                      // tag de l'anim de porte attendue
    int  wait_snd = 0;                        // WAITSND : 1=SE 2=cri 3=fanfare
    std::function<int(int op,int a)> cb_audio;  // ops son (0x49-0x57, 0xEA)
    std::function<int(int kind)>   cb_sndbusy;  // 1 si le canal attendu joue
    // menu multi (Multi/ShowMenu) : préparé par World, résultat -> menu_dest
    int  menu_dest = 0, menu_cancel = 0;
    int  menu_bank = -1;                         // banque texte (-1 = courante)
    std::vector<std::pair<int,int>> menu_items; // (slot -> id msg banque courante)
    void menu_pick(int slot) { set_var(menu_dest, slot); _st = RUN; }
    // boutique (PokeMart*) : liste d'objets en vente, l'UI tourne côté main
    std::vector<int> shop_items;
    // sauvegarde
    const std::unordered_map<int,int>&  vars()  const { return _vars; }
    const std::unordered_map<int,bool>& flags() const { return _flags; }
    // import/export (menu debug : snapshots d'état)
    void import_vars (const std::unordered_map<int,int>& v)  { _vars = v; }
    void import_flags(const std::unordered_map<int,bool>& f) { _flags = f; }
    void bag_clear() { _bag.clear(); }
    // trace circulaire des derniers opcodes exécutés (menu debug)
    struct TraceEnt { int pc = 0, op = -1; const OpDef* od = nullptr; };
    TraceEnt trace[40]; int trace_n = 0;
    int  stack_depth() const { return (int)_stack.size(); }
    void load_state(const std::unordered_map<int,int>& v,
                    const std::unordered_map<int,bool>& f) { _vars = v; _flags = f; }
    // sac (item -> qté)
    const std::unordered_map<int,int>& bag() const { return _bag; }
    int  bag_count(int i) const { auto it = _bag.find(i); return it == _bag.end() ? 0 : it->second; }
    void bag_add(int i, int n) { _bag[i] += n; if (_bag[i] <= 0) _bag.erase(i); }
    void bag_load(const std::unordered_map<int,int>& b) { _bag = b; }
    const std::string& last() const { return _last; }
    int pc() const { return (int)_pc; }

private:
    static const OpDef* opdef(uint16_t op);

    const Bytes* _f = nullptr;
    std::vector<size_t> _fns;
    const Bytes* _stdf = nullptr;
    std::vector<size_t> _stdfns;
    size_t _pc = 0;
    struct Frame { const Bytes* f; size_t pc; };          // pile cross-fichier
    std::vector<Frame> _stack;
    std::unordered_map<int,int> _vars;
    std::unordered_map<int,bool> _flags;
    std::unordered_map<int,int> _bag;
    std::unordered_map<int,int> _wvars;         // work regs 0x8000+ (non sauvés)
    St _st = DONE;
    std::string _last;
    size_t _arg[8]; int _narg = 0;

    uint8_t  rb() { return u8(*_f, _pc++); }
    uint16_t rh() { uint16_t v = u16(*_f, _pc); _pc += 2; return v; }
    uint32_t rw() { uint32_t v = u32(*_f, _pc); _pc += 4; return v; }
    void _rel(int32_t off) { _pc = _pc + off; }          // après params lus
    int  _cmp(int a, int b) { return a < b ? 0 : (a == b ? 1 : 2); }
    bool _cok(int op, int r) const;                      // op de comparaison
    int  _flex(int v);                                   // 0x8000+ = var
    void _skip_params(const char* w);
    void _end();
};
