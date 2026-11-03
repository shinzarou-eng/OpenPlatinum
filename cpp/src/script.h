// script.h — VM des scripts de terrain DPPt (fielddata/script/scr_seq.narc).
// Format fichier : table de u32 (offset relatif à fin d'entrée) + u16 0xFD13 ;
// commandes = u16 opcode + paramètres (b/h/w) ; sauts relatifs à fin de commande.
#pragma once
#include "common.h"
#include <bitset>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

class ScriptVM {
public:
    struct OpDef { uint16_t op; const char* name; const char* widths; };
    enum St { DONE, RUN, MSG, WAITBTN, YESNO, MOVE, WAITTIME, WAITFADE, MENU,
              STARTER, BATTLE, SHOP, WAITANIM, WAITSND, PCBOX, NICK, PARTYPICK,
              MOVEPICK };

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
    std::function<void(int id, bool show)>          cb_people;   // AddObject/RemoveObject
    std::function<void(int id, bool hide)>          cb_hide;     // ShowObject/HideObject (vivant)
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
    std::function<void(int id,bool nocoll)>         cb_nocoll;   // 2B6 : STATUS_18
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
    std::function<void(int sp,int* t1,int* t2)>     cb_mtypes;    // types espèce
    std::function<int(int sp,uint32_t pid)>         cb_mgender;   // genre mon (0 m/1 f/2 aucun)
    std::function<int()>                            cb_pgender;   // genre joueur (0 g/1 f)
    std::function<int(int op)>                      cb_save;      // 0=TrySaveGame 1=CheckSaveType
    std::function<int(int slot)>                    cb_relearncnt;// capacités réapprenables
    std::function<void(int* map,int* sp)>           cb_swarm;     // GetSwarmMapAndSpecies
    std::function<int()>                            cb_honeysp;   // espèce arbre à miel
    std::function<void(int)>                        cb_doorfree;  // UnloadAnimation
    // approche dresseur (trainer_encounter.c) : slots 0/1 du ScriptManager.
    // (slot, op) -> int : op 0=lancer tâche 1=finie? 2=trainerID 3=approachType
    std::function<int(int,int)>                     cb_appr;
    // état terrain divers : 0=objPersistant(localID,flag) 1=SetPlayerState
    //   2=ChangePlayerState 3=FreeCamera(x,z) 4=RestoreCamera
    //   5=CamOverrideObj(x,z) 6=RemoveCamOverride 7=DestroyObstacle(localID)
    //   8=SeqVolume(seq,vol) 9=ShowStartMenu
    std::function<int(int,int,int)>                 cb_fld;
    std::function<int(int,int)>                     cb_item;      // 0 = poche objet
    // parcelles à baies : (sub,arg) -> int — cf. World::_berry_op
    std::function<int(int,int)>                     cb_berry;
    // features d'arène dynamiques : (sub,arg) -> int — cf. World::_gym_op
    std::function<int(int,int)>                     cb_gym;
    // grotte Revanche (Turnback) : réécrit les dests des warps
    //   (pillarsSeen, roomsVisited) — cf. World::_tb_op
    std::function<void(int,int)>                    cb_turnback;
    int menu_ox = 0, menu_oy = 0;                  // SetMenuX/YOriginSide

    // équipe + starters (Sinnoh : 387 Tortipouss / 390 Ouisticram / 393 Tiplouf)
    struct PartyMon {
        int species = 0, level = 1, hp = 1;
        uint32_t ivs = 0xFFFFFFFFu;         // IVs DS packés 5b/stat (-1 = non tirés)
        uint32_t pid = 0, exp = 0;          // PID (nature) + points d'exp
        uint8_t evs[6] = {0};               // EVs par stat (255/stat, 510 total)
        int friendship = 70;                // amitié (évos bonheur)
        int moves[4] = {0}, pp[4] = {0}, nmoves = 0;  // moves persistants
        bool egg = false;                   // œuf (GiveEgg) : pas de combat
        int form = 0;                       // forme (SetPartyMonForm)
        int cond = 0;                       // état persistant (poison/brûlure/par/sommeil/gel)
        std::string nick;                   // surnom (ChoosePokeNick)
        std::bitset<80> ribbons;            // rubans DS (GiveRibbon/CheckRibbon)
        bool fateful = false;               // rencontre fatidique (events légend.)
        int otid = 0;                       // DO != joueur -> outsider (échange)
        int dc_gain = 0;                    // niveaux gagnés en pension
        int held = 0;                       // objet tenu (échanges, baies, etc.)
    };
    std::function<void(PartyMon&)>          cb_moninit;   // init DS GivePokemon
    std::vector<PartyMon> party;
    // stockage PC : 18 boîtes x 30 slots fixes (species==0 = slot vide)
    std::vector<PartyMon> pcbox = std::vector<PartyMon>(540);
    int pc_mode = 0;                           // BoxMode OpenPcFunction (0-4)
    int chosen_starter = 0, rival_starter = 0, cpart_starter = 0;
    static int starter_id(int i);                    // 0-2 -> espèce
    void starter_pick(int i);                        // choix -> reprend le script
    void force_starter() { _st = STARTER; }          // test écran starter
    void force_menu() { _st = MENU; }                // test menu multi
    void force_shop() {                              // test écran boutique
        if (shop_items.empty())
            shop_items = {4, 17, 18, 19, 20, 13, 14, 15, 16, 76,
                          77, 78, 12, 21, 22, 23, 24, 25, 26, 27};
        _st = SHOP;
    }
    int cur_script = 0;                            // SCRIPT_MANAGER_SCRIPT_ID
    int battle_tid = 0, battle_lost = 0;           // StartFirstBattle / CheckWonBattle
    int battle_result = 0;                         // bitmask DS : WIN=1 LOSE=2
                                                 // CAPTURED=4 FUIT_J=5 FUIT_E=6
    int battle_tid2 = 0;                           // StartTrainerBattle : 2e dresseur
    int battle_sp = 0, battle_lv = 0;              // StartWildBattle/StartLegendaryBattle
    int honey_tree = 0;                            // arbre à miel : 1 = enduit
    float honey_min = 0.f;                         // minutes restantes (DS : 360 -> combat)
    void clear_locals() {                          // DS ClearLocalFlags (changement de map)
        for (int v = 0x4000; v <= 0x401F; v++) _vars.erase(v);
        for (int f = 1; f <= 64; f++) _flags.erase(f);
    }
    int special_loc[5] = {0};                      // SetSpecialLocation map,warp,x,z,dir
    int battle_tutorial = 0;                       // StartCatchingTutorial
    // données sauvegardables DS (records / pokétch / anniversaire / accessoires)
    std::unordered_map<int,int> records;           // GameRecord + scores dresseur
    std::unordered_map<int,int> accessories;       // coffre à accessoires concours
    std::unordered_map<int,int> journal;           // événements du journal
    std::unordered_set<int>     dex_seen;          // espèces vues (dex)
    uint32_t poketch_apps = 0;                     // bitmask apps enregistrées
    uint32_t pk_steps = 0;                         // podomètre : pas du joueur
    bool     poketch_hidden = false;               // HidePoketch (cinéma/script)
    std::unordered_set<int>     dex_caught;        // espèces possédées (national)
    std::unordered_set<int>     dex_unown;         // lettres Zarbi vues (0-27)
    int birth_m = 0, birth_d = 0;                  // anniversaire joueur (0=non saisi)
    int player_state = 0;                          // état avatar (0=terrain normal)
    int hof_entries = 0;                           // victoires ligue (0x28F)
    int safari_caught = 0;                         // prises du safari en cours
    // échange PNJ (fld_trade.narc : Init/Get/Start/FinishNPCTrade)
    int trade_id = -1;
    // id -> sp,req,item,otid,nick (fld_trade.narc)
    std::function<void(int,int*,int*,int*,int*,std::string*)> cb_trade;
    std::function<void(PartyMon&)>          cb_trademake;  // construit l'offre
    std::function<void(int)>                cb_roam;       // ActSwarmPoke slot
    std::function<int()>                    cb_prevmap;    // GetPreviousMapId
    std::function<void(int,int,int)>        cb_exitloc;    // 0x2B5 exit location
    std::function<int(int,int)>             cb_dex;        // 0 = n° sinnoh
    std::function<int(int)>                 cb_eggsp;      // espèce bébé (pension)
    // pension de Solaceon (v16) : 2 hôtes + œuf
    PartyMon daycare[2];
    bool daycare_egg = false;
    int  daycare_steps = 0;                  // pas depuis dernière prise/œuf
    int  daycare_compat = -1;              // compatibilité (calculée au dépôt)
    // sélection d'équipe DS (échange 0x2A5 / pension 0x290)
    int pick_kind = 0;                           // 0 = échange, 1 = pension
    void pick_resolve(int slot) { _summary_slot = slot; _st = RUN; }

    // rappel/tuteur de capacités DS (0x221/0x224 -> menu, 0x223/0x225 -> résultat)
    int tutor_slot = -1;                         // membre d'équipe concerné
    int tutor_move = 0;                          // capacité choisie à apprendre
    int tutor_phase = 0;                         // 0 = choix capacité, 1 = choix oubli
    int tutor_result = 0xFF;                     // 0 appris / 0xFF gardé ou annulé
    std::vector<int> tutor_moves;                // liste affichée (ordre learnset)
    std::function<int(int slot, int* out)>      cb_relearnlist;  // moves réapprenables
    std::function<bool(int slot, int mv, int forget)> cb_teachmove;  // -1 = auto
    void tutor_resolve(int idx) {
        if (tutor_phase == 0) {
            if (idx < 0 || idx >= (int)tutor_moves.size()) {
                tutor_result = 0xFF; _st = RUN; return; }
            tutor_move = tutor_moves[idx];
            // place libre -> apprentissage direct, sinon phase « oublier »
            if (cb_teachmove && cb_teachmove(tutor_slot, tutor_move, -1)) {
                tutor_result = 0; _st = RUN; return; }
            tutor_phase = 1; return;
        }
        tutor_result = (idx >= 0 && cb_teachmove &&
            cb_teachmove(tutor_slot, tutor_move, idx)) ? 0 : 0xFF;
        _st = RUN;
    }

    // état système DS (sauvegarde à faire plus tard)
    int      badges = 0;                           // bitmask 8 badges
    uint32_t money = 3000;                         // argent de départ DS
    double   play_sec = 0;                         // temps de jeu cumulé (s)
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
    bool tv_req = false;                            // CallTVBroadcast -> Télé-Motisma
    int  menu_bank = -1;                         // banque texte (-1 = courante)
    std::vector<std::pair<int,int>> menu_items; // (slot -> id msg banque courante)
    void menu_pick(int slot) { set_var(menu_dest, slot); _st = RUN; }
    // boutique (PokeMart*) : liste d'objets en vente, l'UI tourne côté main
    std::vector<int> shop_items;
    // ChoosePokeNick : écran de nommage pour un membre d'équipe
    int  nick_slot = -1, nick_dest = 0;
    int  _summary_slot = 0;                       // SetMonSummary -> GetMonPartySlot
    void nick_pick(bool cancelled) {                  // résultat -> destVar
        set_var(nick_dest, cancelled ? 1 : 0);
        nick_slot = -1; _st = RUN;
    }
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
    // ScriptContext DS : chaque ctx charge sa copie du bytecode (heap), donc
    // un script qui Warp poursuit son exécution après le changement de map
    // (FieldTask_InitCall de la map-change task). Snapshot pour reprise.
    struct Frame { const Bytes* f; size_t pc; };          // pile cross-fichier
    struct Ctx {
        const Bytes* f = nullptr; std::vector<size_t> fns;
        const Bytes* stdf = nullptr; std::vector<size_t> stdfns;
        size_t pc = 0; St st = DONE;
        std::vector<Frame> stack;
        int cur_script = 0, result = 0, pending_var = 0, signpost_dest = 0;
        int wait_t = 0, wait_anim = -1, wait_snd = 0; bool close_btn = false;
        int menu_dest = 0, menu_cancel = 0, menu_bank = -1;
        std::vector<std::pair<int,int>> menu_items;
        // copies heap des fichiers bytecode (survit au reload narc)
        std::vector<std::shared_ptr<Bytes>> holds;
    };
    Ctx  save_ctx() const;
    void restore_ctx(Ctx&& c);
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
    std::vector<std::shared_ptr<Bytes>> _holds;   // fichiers ctx repris (heapptr)
    size_t _pc = 0;
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
