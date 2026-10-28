// world.h — monde : charge une map (header) depuis la ROM, cellules de matrice,
// props, collision par raycast + attrs terrain, warps, PNJ placeholders.
// Port de scripts/world.gd + player.gd.
#pragma once
#include "common.h"
#include "nds.h"
#include "land.h"
#include "bmd.h"
#include "render.h"
#include "script.h"
#include "battle.h"
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>

// options de jeu : DS (game_options.c) + graphiques natives du port.
// persistées dans options.cfg à côté de l'exe.
struct Opts {
    int text_speed = 1;      // 0 lent, 1 normal, 2 rapide
    int battle_scene = 0;    // 0 oui, 1 non
    int battle_style = 0;    // 0 choix, 1 defini
    int sound = 0;           // 0 stereo, 1 mono
    int buttons = 0;         // 0 normal, 1 start=x, 2 l=a
    int frame = 0;           // type de fenetre 0..19 (DS)
    // port PC
    int fullscreen = 0;      // SDL fullscreen
    int vsync = 1;           // swap interval GL
    int texfilter = 0;       // 0 = net (pixels DS), 1 = lisse
    int winscale = 0;        // 0:1280x720 1:1600x900 2:1920x1080
    int msaa = 0;            // 0:off 1:2x 2:4x 3:8x
    int aniso = 0;           // 0:off 1:2x 2:4x 3:8x 4:16x
    int lang = 0;            // 0:FR 1:EN (defaut = region de la ROM)
};
extern Opts g_opts;

// texte de l'UI native selon la langue choisie (les textes du jeu
// suivent la ROM inseree : FR -> francais, US -> anglais, ...)
inline const char* tr(const char* fr, const char* en) {
    return g_opts.lang ? en : fr;
}

class World {
public:
    static constexpr float TILE = 16.f;
    static constexpr float CELL = 512.f;
    static constexpr int LOAD_R = 2;      // rayon cellules (5×5, monde ouvert)
    static constexpr int UNLOAD_R = 3;    // hystérésis déchargement
    static constexpr int LOAD_BUDGET = 2; // cellules chargées par frame

    bool init(Nds* db);
    bool flat = false;                       // debug : couleur par surface
    bool verbose = false;                    // debug : logs détaillés
    void load_map(int header_id, int dest_warp);
    bool save(const char* path) const;
    bool load(const char* path);
    void update(float dt, float ix, float iz);   // ix,iz : input -1..1
    void draw(const Mat4& proj, int sw, int sh);
    Mat4 cam_proj(float asp) const;          // projection DS selon header->cam

    // debug / imgui
    int header() const { return _header; }
    const std::string& label() const { return _label; }
    const std::string& mapname() const { return _mapname; }
    Vec3 player_pos() const { return _ppos; }
    void set_pos(float x, float z);          // téléport + stream + sol
    void set_dir(int d) { _pdir = d; }       // dir monde : 0=S 1=N 2=O 3=E
    int cell_count() const { return (int)_cells.size(); }
    int warp_count() const { return (int)_warps.size(); }
    int obj_count() const { return (int)_objects.size(); }
    int surf_count() const { return _nsurf; }
    int vert_count() const { return _nverts; }
    bool on_ground() const { return _ground; }
    const std::vector<Nds::Zev::Warp>& warps() const { return _warps; }
    bool busy() const { return _busy; }

    // scripts / dialogue
    void action();                          // touche A : parler / avancer
    bool script_busy() const { return _vm.busy(); }
    int  script_state() const { return (int)_vm.state(); }
    const std::string& dialog() const { return _dialog; }
    bool msgbox() const { return _mbox; }   // fenêtre de dialogue ouverte
    std::string dialog_shown() const;       // texte visible (défilement DS)
    // spans de couleur {FF00:n} de la ROM : (octet début dans _dialog, idx palette)
    const std::vector<std::pair<int,int>>& dialog_cols() const { return _dlg_cols; }
    size_t dlg_page_start() const { return _dlg_page; }
    bool yesno() const { return _yesno; }
    float banner_t() const { return _banner_t; }
    void choose_yesno(bool yes);            // réponse au menu Oui/Non
    // menu multi + fondu + course
    bool menu_open() const { return _vm.state() == ScriptVM::MENU; }
    const std::vector<std::pair<int,int>>& menu_items() const { return _vm.menu_items; }
    void menu_pick(int slot);               // choix menu -> reprend le script
    const std::string& menu_text(int id);   // texte d'une entrée (banque courante)
    float fade_alpha() const;               // alpha du fondu (0-1)
    uint32_t fade_color() const { return _fade.color; }
    bool run_held = false;                  // bouton B (course, main.cpp)
    bool can_run() const { return _can_run; }
    // sac / objets
    const std::unordered_map<int,int>& bag() const { return _vm.bag(); }
    const std::string& item_name(int id);   // nom FR (banque 392)
    // starters + équipe
    bool starter_open() const { return _vm.state() == ScriptVM::STARTER; }
    void starter_pick(int i) { _vm.starter_pick(i); _vm.step(); }
    void debug_mon() { debug_mon(387, 5); }                 // test : Tortipouss
    void debug_mon(int sp, int lv, int mv = 0) {            // test : espèce libre
        ScriptVM::PartyMon pm; pm.species = sp; pm.level = lv;
        pm.hp = mon_maxhp(sp, lv);
        if (mv) { pm.moves[0] = mv; pm.nmoves = 1; }
        _vm.party.push_back(pm); }
    const std::string& species_name(int id); // nom FR (banque 412)
    const std::vector<ScriptVM::PartyMon>& party() const { return _vm.party; }
    int pic_species() const { return _pic_species; }   // PokemonPicture actif
    // menu X / sac authentiques
    const std::string& msg(int bank, int idx);          // chaîne FR pl_msg[bank][idx]
    // identité joueur (intro DS : genre + noms choisis)
    int _gender = 0;                                 // 0:garçon 1:fille
    std::string _pname, _rname;                      // nom joueur / rival
    void set_identity(int g, const std::string& pn, const std::string& rn);
    const std::string& player_name() const {
        static const std::string d = g_opts.lang ? "Lucas" : "H\xC3\xA9ros";
        return _pname.empty() ? d : _pname;
    }
    const std::string& rival_name() const {
        static const std::string d = g_opts.lang ? "Barry" : "Ren\xC3\xA9";
        return _rname.empty() ? d : _rname;
    }
    const std::string& cpart_name() const {           // alter ego (sexe opposé)
        static const std::string df = g_opts.lang ? "Dawn" : "Aurore";
        static const std::string dm = g_opts.lang ? "Lucas" : "H\xC3\xA9ros";
        return _gender ? dm : df;                     // fille -> Lucas, garçon -> Aurore
    }
    // intro DS (world_intro.cpp) : titre 3D + écrans/portraits décodés
    void draw_title(float asp);                        // scène Giratina animée
    uint32_t intro_tex(int id);                        // texture 2D par id
    void intro_tex_size(int id, int* w, int* h);
    void intro_set_frame(int f);                       // pose portrait Sorbier
    void intro_set_pframe(int f);                      // pose Lucas/Dawn (0-3)
    int _intro_frame = 0, _intro_pframe = 0;
    uint32_t namein_icon_tex(int kind, int frame);     // icône champ : 0=♂ 1=♀ 2=rival
    int namein_icon_sz[2] = {};
    std::string msg_fmt(int bank, int idx) {           // message prêt à afficher
        auto& b = _bank_msgs(bank);
        return (idx >= 0 && idx < (int)b.size()) ? _fmt_msg(b[idx]) : std::string();
    }
    bool dex_obtained() const { return _vm.flag(146); }  // FLAG_HAS_POKEDEX
    bool shop_open() const { return _vm.state() == ScriptVM::SHOP; }
    const std::vector<int>& shop_items() const { return _vm.shop_items; }
    uint32_t money() const { return _vm.money; }
    int  badges() const { return _vm.badges; }       // bitmask des 8 badges
    int  gender() const { return _gender; }          // 0:garçon 1:fille
    int  item_price(int id) const;                     // pl_item_data u16@0
    void shop_buy(int idx);                            // achète shop_items[idx]
    void shop_close();                                 // rend la main au script
    bool has_starter() const { return !_vm.party.empty() || _vm.var(0x4032) != 0; }
    int item_pocket(int id);                             // poche sac (item_data.narc)
    // sac DS natif
    std::vector<std::pair<int,int>> bag_items(int pocket); // ordre d'obtention
    int  item_heal(int id);          // soin PV (pl_item_data u8@24)
    int  item_field_use(int id);     // 0=non, 1=soin équipe, 2=rappel
    bool item_tossable(int id);      // tout sauf objets rares (poche 7)
    bool item_registrable(int id);   // poche 7 -> touche Y
    int  mon_maxhp(int species, int lv, int iv = -1, int ev = 0);
    int  registered_item() const { return _reg_item; }
    void set_registered(int id) { _reg_item = id; }
    uint32_t item_icon_tex(int id, int frame);           // icône 32x32 (world_intro.cpp)
    uint32_t pocket_icon_tex(int pocket, bool sel);      // icône poche 16x16 (pl_bag_gra)
    uint32_t mon_icon_tex(int species, int frame);       // icône espèce 32x32 (pl_poke_icon)
    uint32_t battle_bg_tex(int bg, int tod);             // décor combat 512x256 (pl_batt_bg)
    uint32_t battle_plat_tex(int ter, int side);         // plateforme terrain (pl_batt_obj)
    void battle_plat_size(int ter, int side, int* w, int* h);
    std::vector<int> _bag_seq;                           // ordre d'obtention
    int _reg_item = 0;
    bool ui_frozen = false;                              // menu X : monde en pause (DS)

    // ── menu debug (imgui) ──
    struct MoveJob {                        // file d'un ApplyMovement
        int obj;                            // id local objet (0xFF=joueur)
        std::vector<std::pair<int,int>> st; // (cmd,arg)
        size_t i = 0; float t = 0; bool done = false;
        int dx = 0, dz = 0;                 // delta de la marche en cours
        int x0 = 0, z0 = 0;                 // tuile source de la marche en cours
        float spd = 0, dist = 0, y0 = 0;    // vitesse t/s, distance, y de départ
        int nt = 1, jump = 0;               // tuiles ciblées, saut (1/2)
        float hop = 0, hopt = 0;            // saut sur place : y animé pendant pause
        bool lockdir = false;
        bool spot = false;                  // WalkOnSpot en cours (cycle marche)
    };
    int pdir() const { return _pdir; }
    void tp_tile(int x, int z) { set_pos(x * TILE + TILE / 2, z * TILE + TILE / 2); }
    const std::vector<Nds::Zev::Obj>&   objects() const { return _objects; }
    const std::vector<Nds::Zev::Bg>&    bgs()     const { return _bgs; }
    const std::vector<Nds::Zev::Coord>& coords()  const { return _coords; }
    Vec3 obj_pos(int i)  const { return _objpos[i]; }
    int  obj_dir(int i)  const { return _objd[i]; }
    int  obj_move(int i) const { return _objmv[i]; }
    int  obj_gfx(int i)  const { return _resolve_gfx(_objects[i].gfx); }
    bool obj_gone(int i) const { return _objgone.count(i); }
    bool obj_hide(int i) const { return _objhide.count(i); }
    bool obj_lock(int i) const { return _objlock.count(i); }
    int  follower_idx()  const { return _follower; }
    void obj_respawn(int i);                   // AddObject manuel (event)
    void obj_gone_set(int i, bool g);
    void obj_hide_set(int i, bool h) { if (h) _objhide.insert(i); else _objhide.erase(i); }
    void obj_lock_set(int i, bool l) { if (l) _objlock.insert(i); else _objlock.erase(i); }
    ScriptVM& vm() { return _vm; }             // debug : vars/flags/state directs
    const std::vector<MoveJob>& moves() const { return _moves; }
    int  msg_count(int b);                     // taille banque de messages
    int  cur_bank() const { return _cur_bank; }
    void set_can_run(bool v) { _can_run = v; }
    void run_init() { _run_init(); }           // rejoue les scripts d'init
    void start_script_dbg(int id) { _start_script(id); }
    float ground_y_at(float x, float z, float from) const {
        return _ground_y({x, from, z}, from);
    }
    // ── menu debug : puissant ──
    float dbg_speed = 1.f;                 // multiplicateur de vitesse
    bool dbg_frame = false;                // pas-à-pas : simule 1 frame
    bool noclip = false;                   // traverse murs + PNJ
    int  tile_beh(int tx, int tz) const { return _tile_beh(tx, tz); }
    bool tile_blocked(int tx, int tz) const {
        return _blocked({tx * TILE + TILE / 2.f, _ppos.y + 4,
                         tz * TILE + TILE / 2.f});
    }
    Nds::Zev::Obj& obj_event(int i) { return _objects[i]; }   // fiche event
    void obj_set_pos(int i, int tx, int tz);
    void obj_set_dir(int i, int d)  { _objd[i] = d & 3; }
    void obj_set_move(int i, int m) { _objmv[i] = m; }
    void force_wild(int sp, int lv)   { _btl_start(false, 0, sp, lv); }
    void force_trainer(int tid)       { _btl_start(true, tid, 0, 0); }
    void force_starter()              { _vm.force_starter(); }   // test écran starter
    // scène starter DS (ev_pokeselect.narc) : mallette + balles en vrai 3D
    void draw_starter(float asp);
    bool starter_ball_scr(int i, float* sx, float* sy) const;  // pos écran balle
    int  starter_sel = 0;                   // balle sous le curseur
    int  starter_phase = 0;                 // 0 choix, 1 confirmation
    int  enc_rate() const { return _enc_rate; }
    int  enc_slot(int i, int* lv) const;    // espèce du slot (-1)
    bool surfing() const { return _psurf; }
    bool battle_active() const { return _wild_t > 0.f || _btl.active; }
    // snapshot complet (vars/flags/équipe/pos/états objets) -> rejouer une scène
    struct DbgState {
        int hdr = -1; Vec3 pos; int dir = 0;
        std::unordered_map<int,int>  vars;
        std::unordered_map<int,bool> flags;
        std::vector<ScriptVM::PartyMon> party;
        std::unordered_map<int,int>  bag;
        uint32_t money = 0; int badges = 0;
        std::unordered_set<int> gone, hide, lock;
        int follower = -1;
    };
    DbgState dbg_save() const;
    void dbg_load(const DbgState& s);
    // combat DS
    Battle& battle() { return _btl; }
    void battle_key(int k) { _btl_key = k; }
    void battle_throw(int item);            // lancer une Ball (menu SAC combat)
    uint32_t poke_tex(int sp, int face, int frame = 0) { // face 0=avant 1=dos
        return _poke_tex(sp * 6 + (face ? 1 : 3), frame); }
    int poke_feet(int sp, int face, int frame = 0) {     // ligne opaque basse
        return poke_feet_m(sp * 6 + (face ? 1 : 3), frame); }
    int poke_feet_m(int member, int frame);

private:
    struct Cell {
        Vec3 org;                       // centre monde de la cellule
        int cx = 0, cy = 0;             // coords matrice
        int hdr = -1, land_id = -1;
        int verts = 0;                  // stats : vertices des meshes
        const Land* land = nullptr;
        std::vector<GpuMesh> meshes;
        std::vector<Vec3> col_tris;     // triangles collision terrain (monde)
        std::vector<Vec3> prop_tris;    // triangles collision props (murs/toits)
    };
    struct PropInst {
        std::vector<GpuMesh> meshes;
        std::vector<int> mkey;          // group_key par mesh (-1 = pas animable)
        std::vector<M4> minv;           // inverse de la matrice de repos par mesh
        Vec3 pos; float ry = 0; Vec3 scale{1,1,1};
        Vec3 org;                       // origine cellule
        int64_t cell = -1;              // clé cellule source (déchargement)
        int model = -1;                 // index build_model.narc (id de prop)
        const bmd::Model* mdl = nullptr;
        // animation one-shot en cours (portes ; bm_anime.narc via bm_anime_list)
        const bmd::JntAnim* anm = nullptr;
        int anm_idx = -1;               // index dans anime_list (0..3)
        float anm_frame = 0.f;          // frame courante (fraction conservée)
        bool anm_hold = false;          // terminée : figer la dernière frame
        std::unordered_map<int, M4> anm_cur;   // group_key -> matrice animée
    };

    Nds* _db = nullptr;
    int _header = -1;
    std::string _label, _mapname;
    float _banner_t = 0.f;
    Nds::Matrix _mtx;
    std::vector<Nds::Zev::Warp> _warps;
    std::vector<Nds::Zev::Obj> _objects;
    std::unordered_map<int64_t, std::shared_ptr<Cell>> _cells;
    std::vector<PropInst> _props;
    std::unordered_map<int, std::shared_ptr<Land>> _lands;
    std::unordered_map<int, std::shared_ptr<bmd::Model>> _bmds;
    std::unordered_map<int, std::vector<GpuMesh>> _obj3d;   // gfx -> meshes fldeff (panneaux)
    std::vector<GpuMesh>& _obj_3d(int gfx);
    // scène starter DS (choose_starter_app.c) : ev_pokeselect.narc en vrai 3D
    bool _scn_ok = false;
    float _scn_t = 0.f;                                    // temps scène
    float _scn_cam = 0.f;                                  // zoom confirmation 0->1
    std::vector<PropInst> _scn;                            // mallette, 3 balles, socle
    std::unordered_map<int, std::shared_ptr<bmd::Model>> _scn_mdl;
    std::unordered_map<int, bmd::JntAnim> _scn_anm;
    float _scn_scr[3][2]{};                                // NDC des 3 balles (curseur)
    std::vector<GpuMesh> _shadow_meshes;                    // shadow.nsbmd (fldeff 17)
    void _load_shadow();
    std::unordered_map<std::string, bmd::TexEnt> _texset;
    // cache texture GL : clé = adresse de l'Image ; le shared_ptr gardé vivant
    // évite qu'une adresse libérée soit réutilisée (collisions de textures !)
    std::unordered_map<const void*, std::pair<std::shared_ptr<Image>, uint32_t>> _texcache;
    // fldtanime.narc : textures de terrain animées (eau, fleurs). Le contenu
    // de la texture GL est réécrit à chaque changement de frame, comme la DS
    // qui écrase la VRAM — aucun changement au pipeline de dessin.
    struct FltAnim {
        uint32_t tex = 0;
        int cur = -1, total = 0;
        std::vector<int> ticks;                      // durée de chaque frame
        std::vector<std::shared_ptr<Image>> frames;
    };
    std::unordered_map<std::string, FltAnim> _flt;   // clé tex|pal -> anim
    // nom texture -> {membre narc, [(texIndex, ticks)]}
    std::unordered_map<std::string,
        std::pair<int, std::vector<std::pair<int,int>>>> _fltdef;
    bool _fltdef_ok = false;
    float _flt_t = 0.f;
    void _flt_load();
    uint32_t _flt_tex(const std::string& key, bmd::TexEnt& base, int pal_off);
    void _flt_step(float dt);
    float _cay[8] = {}; int _cai = 0;           // caméra : file hauteur Y (6t DS)
    std::unordered_set<int> _unimpl; int _unimpl_n = 0;   // opcodes manquants
    // arealight DS : gabarits du membre area.light (un par tranche horaire)
    struct Alt { int end = 0; DsLight L; };
    std::vector<Alt> _alt;
    void _alt_load(int member);
    int _flat_id = 0;
    int _nsurf = 0, _nverts = 0;
    bool _busy = false;

    // scripts / événements
    void _start_script(int id);             // routeur scriptID -> membre scr_seq
    int  _init_script(int type);            // script d'init fixe (2/3/4) -> scriptID|-1
    int  _frame_script();                   // table var==var -> scriptID|-1
    const Bytes* _initb = nullptr;          // scr_seq[hdr.init]
    std::string _fmt_msg(const std::string& s,   // tags message -> texte
        std::vector<std::pair<int,int>>* cols = nullptr); // + positions {FF00}
    const std::vector<std::string>& _bank_msgs(int b); // pl_msg[b] (cache)
    int _vm_member = -1;                    // membre scr_seq actif (mouvements)
    int _cur_bank = -1;                     // banque texte du script actif
    int _coord_in = -1;                     // déclencheur sol actif (-1 = aucun)
    int _talk_obj = -1;                     // PNJ en cours de dialogue
    ScriptVM _vm;
    std::shared_ptr<Narc> _scrnarc;         // garde le fichier script vivant
    std::shared_ptr<Narc> _encnarc;         // pl_enc_data.narc (garde _enc vivant)
    const Bytes* _enc = nullptr;            // WildEncounters du header courant
    int _enc_rate = 0, _enc_steps = 0;      // taux terrestre + pas depuis combat
    float _wild_t = 0;                      // intro rencontre sauvage en cours
    int _wild_sp = 0, _wild_lv = 0;         // espèce/niveau du sauvage actif
    uint32_t _wild_tex = 0;                 // texture du sprite sauvage
    std::unordered_map<int, uint32_t> _pktex; // cache textures pl_pokegra
    uint32_t _poke_tex(int member, int frame = 0); // sprite membre (sp*6+idx) + frame anim
    Battle _btl;                            // combat en cours
    int _btl_key = 0;                       // touche en attente pour la battle
    void _btl_start(bool trainer, int tid, int sp, int lv);
    void _btl_end();                        // recopie résultat -> VM/partie
    std::unordered_map<int, std::vector<std::string>> _msgbanks; // banques décodées
    std::string _dialog;                    // texte affiché en cours
    float _dlg_chars = 0.f;                 // nb de chars dévoilés (page courante)
    size_t _dlg_page = 0;                   // début de la page courante (\n\n DS)
    std::vector<std::pair<int,int>> _dlg_cols;  // {FF00} -> (octet, palette)
    size_t dlg_page_end() const;            // fin de la page courante
    std::string _buf[16];                   // buffers {STRVAR} StringTemplate
    struct Fade { float t = -1, dur = 0; uint32_t color = 0; bool out = true; } _fade;
    bool _can_run = false;                  // chaussures de sport obtenues
    int _lockall = 0;                       // LockAll : PNJ gelés pendant script
    bool _yesno = false;
    int _yesno_var = 0;
    std::vector<MoveJob> _moves;
    float _mvdbg = 0;                       // debug jobs coincés
    // états par INDEX d'objet (map_object.c : localID -> 1er objet vivant ;
    // les ids locaux ne sont pas uniques, ex. Route 201 : Rowan + panneau = 5)
    std::unordered_set<int> _objgone;       // index retirés (RemovePeople/flag)
    std::unordered_set<int> _objhide;       // index invisibles (SetInvisible 0x45)
    std::unordered_set<int> _objlock;       // index gelés (Lock/Release par script)
    std::unordered_map<int,float> _objemote; // bulle "!" (index objet, 0xFF = joueur)
    GpuMesh _emote_bb; uint32_t _emote_tex = 0;
    GpuMesh _pkbb;                          // quad sprite sauvage 48×24
    // dresseur qui repère le joueur (trainer_encounter.c) : "!" -> approche
    // jusqu'à adjacent -> joueur face -> script de combat (3000+/5000+)
    struct Spot { int obj = -1, phase = 0, dx = 0, dz = 0;
                  float t = 0.f; } _spot;
    // suiveur (FollowHero / StartFollow, ex. rival Route 201)
    int _follower = -1;                     // index de l'objet suiveur (-1 = aucun)
    bool _mbox = false;                     // fenêtre message ouverte (DS window)
    std::deque<std::pair<int,int>> _ftrail; // file des tuiles visitées par le joueur
    int _fstep = 0; float _ftx = 0, _ftz = 0; // pas de suiveur en cours
    std::unordered_map<int64_t,uint16_t> _tileover; // SetTilePassable (tx,tz)->attr
    int _pic_species = 0;                   // PokemonPicture actif (espèce)
    bool _lost_pc = false;                  // LostGoPc : warp maison au prochain tick
    bool _moves_done() const;
    void _queue_move(int obj, size_t off);
    void _tick_moves(float dt);
    void _set_facing(int obj, int dir);
    const Nds::Zev::Obj* _find_obj(int local) const;
    std::vector<Nds::Zev::Bg> _bgs;
    std::vector<Nds::Zev::Coord> _coords;
    int  _obj_dir(int i) const;             // dir actuelle d'un objet (mouv.)
    std::vector<Vec3> _objpos;              // position monde par objet
    std::vector<int> _objd;                 // direction courante par objet
    std::vector<int> _objmv;                // movementType vivant (copie event au spawn)
    struct Wander { float idle = 1.f + (rand() % 40) / 10.f;
        int dx = 0, dz = 0; float tgt = 0; Vec3 home;
        float phase = 0.f; int step = 0; };   // step : cycle/va-et-vient
    std::vector<Wander> _objw;              // déambulation PNJ (mv 2-5 / look 12-15)
    void _tick_wander(float dt);
    void _tick_spot(float dt);               // approche dresseur en cours
    bool _check_trainer_sight();             // un dresseur voit le joueur ?
    void _try_wild(int tb);                  // WildEncounters_TryWildEncounter
    struct PendingWarp { int hdr, x, z, dir; };
    bool _warp_req = false; PendingWarp _pw{};

    // transitions de map DS (field_control.c + unk_02056B30.c) : déclenchées
    // en posant le pas sur la tuile ou en poussant vers une tuile bloquée.
    int  _warp_at(int tx, int tz) const;        // index du warp sur la tuile, -1
    bool _warp_depart(int wi, int type, int adir);  // déclenche une transition
    void _tick_warp(float dt);                  // machine de transition active
    bool _warp_step(float dt);                  // marche auto -> _wt.wx/wz
    void _warp_fin();                           // fin de transition
    struct WarpEnt { int hdr = -1, warp = -1, x = 0, z = 0, dir = 0; };
    WarpEnt _entrance;                          // entrance FieldOverworldState
    struct { int hdr = -1, warp = -1; } _special;   // lieu spécial (dwarp 0x100)
    int _blackout_id = 1;                           // spawn_locations.c : index table (défaut = maison)
    struct WarpTask {
        int st = -1;                            // phase en cours (-1 = inactif)
        int type = 0;                           // 0 fondu, 1 porte, 3 escaliers
        int dh = -1, dw = -1, adir = 0;         // destination + dir d'arrivée
        int sub = 0;                            // sous-phase d'arrivée
        float wx = 0, wz = 0, wspd = 0;         // cible + vitesse marche auto
        float py = 0;                           // sol sous le joueur (panneau)
        float t = 0.f;                          // accumulateur temps (ticks DS)
        int door = -1;                          // prop de porte animée (-1)
        int bo_tx = -1, bo_tz = -1;             // blackout : tuile imposée au load
    } _wt;
    bool _phidden = false;                      // joueur caché (MapObject hidden)

    // animations de props (bm_anime_list -> bm_anime) : portes one-shot.
    // map_prop_animation.c / ov5_021D431C.c de la décomp.
    std::unordered_map<int, std::vector<int>> _alist;      // model -> archive ids
    std::unordered_map<int, bmd::JntAnim> _bca;            // archive -> anim BCA0
    std::unordered_map<int, int> _doortags;                // tag script -> _props
    const std::vector<int>* _anim_ids(int model);          // cache bm_anime_list
    const bmd::JntAnim* _bca_anim(int aid);                // cache bm_anime (BCA0)
    int  _prop_anim_count(int model);                      // nb d'anims du prop
    int  _find_door_prop(int tx0, int tz0, int tx1, int tz1); // porte en hitbox tuiles
    bool _prop_play(int pi, int idx);                      // lance l'anim idx
    bool _prop_anim_done(const PropInst& p);               // one-shot terminée ?
    void _prop_anim_stop(PropInst& p);
    void _tick_prop_anims(float dt);
    Mat4 _prop_node_delta(const PropInst& p, int mesh_i);  // anim * repos⁻¹

    // joueur — mouvement DS : pas de 1 tuile (player_move.c / unk_020655F4.c)
    // WalkNormal 2px/tick(30Hz)=8 ticks, Run 4px/tick=4 ticks, Turn 2 ticks
    Vec3 _ppos{0, 40, 0}, _vel{0, 0, 0};
    bool _ground = false;
    int _warp_cd_x = -9999, _warp_cd_z = -9999;   // tuile warp du spawn
    int _pdir = 0;                              // direction 0-3 (frame mmodel)
    bool _psurf = false;                        // avatar surf (PLAYER_AVATAR_SURFING)
    float _panim = 0.f;                         // cycle marche
    int _pstep = 0;                             // 0=NONE 1=TURNING 2=MOVING 3=BUMP
    float _pstep_t = 0;                         // durée restante (turn/bump)
    int   _pjump = 0;                           // saut de rebord en cours
    int   _pslide = 0;                          // glissière glace en cours (dir forcée)
    float _ptx = 0, _ptz = 0;                   // cible du pas en cours
    float _movdbg_t = 0;                        // debug dérive joueur
    float _pstep_spd = 0;                       // vitesse du pas (px/s)
    GpuMesh _pbox, _npcbox, _npcbox2, _bb;
    std::unordered_map<int, uint32_t> _mmcache; // gfx*32+frame -> tex GL

    void _load_cell(int cx, int cy);
    void _unload_cell(int64_t k);               // libère meshes + props d'une cellule
    void _inst_props(Cell* c);                  // instancie les props d'une cellule
    void _finish_load(int header_id, int dest_warp); // spawn + logs + init
    void _set_header(int header_id, bool init); // events+scripts+PNJ d'un header
    void _run_init();                           // scripts d'init (4,2,3)
    void _stream();                             // cellules + header autour du joueur
    void _unload_all();
    std::vector<float> _tris(const bmd::MeshGroup& mg, Vec3& colout,
        std::vector<Vec3>* coltris);
    uint32_t _mat_tex(const bmd::MatDef& md,
        const std::unordered_map<std::string, bmd::TexEnt>& texs,
        bool* alpha, int* polyA);
    uint32_t _mm_tex(int gfx, int frame);       // sprite overworld
    uint32_t _mm_member_tex(int member, int frame); // membre mmodel direct (surf)
    static bool _surfable(int beh);             // TILE_BEHAVIOR_FLAG_SURFABLE
    bool _mon_learns_hm(const ScriptVM::PartyMon& m, int hm) const; // compat CS
    bool _party_can_surf() const;               // un Pokémon sait/peut Surf
    void _draw_billboard(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
        const Vec3& cam, const Mat4& mvp);
    void _draw_billboard(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
        const Vec3& cam, const Mat4& mvp, float pull);
    bool _blocked(const Vec3& p) const;
    int  _tile_beh(int tx, int tz) const;    // byte bas de l'attr (behavior DS)
    float _ground_y(const Vec3& p, float from) const;
    // localID -> index du 1er objet VIVANT (spawné) ; -1 si aucun (DS
    // MapObjMan_LocalMapObjByIndex). script==0xFFFF inclus pour le vivant.
    int  _live_obj(int id) const;
    // gfx event -> gfx réel : VAR_0..F (101-116) lus dans vars 0x401F+n
    // (MapObject_GetFieldSystemGraphicsID -> FieldSystem_GetGraphicsID)
    int  _resolve_gfx(int gfx) const;
    float _cam_hit(const Vec3& from, const Vec3& dir) const;
    const Cell* _cell_at(float wx, float wz) const;
    static GpuMesh _box(float hx, float hy, float hz, RGBAf c);
};
