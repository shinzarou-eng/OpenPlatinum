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
#include "bw_sprites.h"
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
    int theme = 4;           // 0:DS (cadres d'origine) 1:Sombre 2:Clair
                             // 3:Néo (maquette ultime) 4:POP SINNOH (affiche)
    int text_size = 1;       // 0:petit (DS) 1:normal 2:grand
    int ui_scale = 100;      // 50..100 % — échelle du plateau DS 256x192
    int rendu = 0;           // 0:DS (fidèle) 1:Cinéma (post-process HD-2D)
    int autorun = 0;         // course sans maintenir B (confort port)
    int music = 10;          // volume musique 0..10 (x10 %)
    int autosave = 1;        // sauvegarde auto save_auto.dat (fin combat/zone)
    int kmap[8] = {0};       // remap touches : scancodes (0 = défaut)
                           // 0 haut 1 bas 2 gauche 3 droite 4 A 5 B 6 X 7 course
};
extern Opts g_opts;

// texte de l'UI native selon la langue choisie (les textes du jeu
// suivent la ROM inseree : FR -> francais, US -> anglais, ...)
inline const char* tr(const char* fr, const char* en) {
    return g_opts.lang ? en : fr;
}

// retouche couleur d'un sprite : teinte (°) + sat/val (%) appliquées aux
// slots de palette cochés dans mask (bit i = couleur i). identity = neutre.
struct SkinTint {
    int hue = 0, sat = 100, val = 100;
    uint32_t mask = 0xFFFFFFFF;
    bool identity() const {
        return !mask || (hue == 0 && sat == 100 && val == 100);
    }
};

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
    struct SavePeek { std::string name; int header = -1; int party = 0;
        int gender = 0; int skin = -1; int skin_hue = 0;
        int skin_sat = 100, skin_val = 100;
        uint32_t skin_mask = 0xFFFFFFFF;
        int party_sp[6] = {0};              // espèces équipe (frise écran titre)
        uint32_t money = 0; int badges = 0; int dex = 0;
        uint32_t play_sec = 0; };           // v14
    bool save_peek(const char* path, SavePeek& out) const;   // résumé écran titre
    void update(float dt, float ix, float iz);   // ix,iz : input -1..1
    void draw(const Mat4& proj, int sw, int sh);
    uint32_t minimap_tex(int size, float half_world, Vec3 center); // vue du dessus -> texture ancrée
    Mat4 cam_proj(float asp) const;          // projection DS selon header->cam

    // debug / imgui
    int header() const { return _header; }
    const std::string& label() const { return _label; }
    const std::string& mapname() const { return _mapname; }
    const std::string& floor_txt() const { return _floor_txt; }
    Vec3 player_pos() const { return _ppos; }
    void set_pos(float x, float z);          // téléport + stream + sol
    void set_y(float y) { _ppos.y = y; }     // debug : force la hauteur
    void set_dir(int d) { _pdir = d; }       // dir monde : 0=S 1=N 2=O 3=E
    void set_surf(bool s) { _psurf = s; }        // debug : spawn en surf
    void set_special(int h) { _special.hdr = h; }  // debug : SpecialLocation
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
    float dlg_chars() const { return _dlg_chars; }   // chars révélés (feed TV)
    size_t dlg_page() const { return _dlg_page; }
    size_t dlg_pend() const { return dlg_page_end(); } // fin de page (feed TV)
    bool msgbox() const { return _mbox; }   // fenêtre de dialogue ouverte
    void dlg_debug(int bank, int id);       // test : force une boîte message
    std::string dialog_shown() const;       // texte visible (défilement DS)
    // spans de couleur {FF00:n} de la ROM : (octet début dans _dialog, idx palette)
    const std::vector<std::pair<int,int>>& dialog_cols() const { return _dlg_cols; }
    size_t dlg_page_start() const { return _dlg_page; }
    // mesure texte NFGR (injectée par main) : largeur px DS de s[b..e) —
    // sert à paginer automatiquement les pages qui dépassent 2 lignes
    std::function<float(const std::string&, size_t, size_t)> dlg_measure;
    bool yesno() const { return _yesno; }
    float banner_t() const { return _banner_t; }
    int bgm_seq(bool night) const { return night ? _mus_night : _mus_day; }
    int cam_type() const { return _cam_type; }
    // audio SDAT (injecté par main) : BGM de zone + ops de script
    void set_audio(class DsAudio* s) { _snd = s; }
    bool is_night() const;                       // heure DS : 20h-4h
    int  audio_op(int op, int a);                // exécute un op son du script
    int  audio_busy(int kind);                   // 1=SE 2=cri 3=fanfare
    void choose_yesno(bool yes);            // réponse au menu Oui/Non
    // menu multi + fondu + course
    bool menu_open() const { return _vm.state() == ScriptVM::MENU; }
    const std::vector<std::pair<int,int>>& menu_items() const { return _vm.menu_items; }
    void menu_pick(int slot);               // choix menu -> reprend le script
    const std::string& menu_text(int id);   // texte d'une entrée (banque courante)
    float fade_alpha() const;               // alpha du fondu (0-1)
    uint32_t fade_color() const { return _fade.color; }
    float dwarp_prog() const {              // vortex DoDWWarp : 0 -> 1
        return _dwarp_t < 0 ? -1.f : 1.f - _dwarp_t / 2.1f; }
    bool dwarp_active() const { return _dwarp_t >= 0; }
    bool run_held = false;                  // bouton B (course, main.cpp)
    bool can_run() const { return _can_run; }
    // sac / objets
    const std::unordered_map<int,int>& bag() const { return _vm.bag(); }
    const std::string& item_name(int id);   // nom FR (banque 392)
    // rencontres sauvages de la map courante (pl_enc_data, brut)
    const Bytes* enc_raw() const { return _enc; }
    // repousse : pas restants + drapeau « vient d'expirer » (consommé à la lecture)
    int  repel_steps() const { return _repel; }
    int  repel_last() const { return _repel_item; }
    bool repel_ask() { bool r = _repel_ask; _repel_ask = false; return r; }
    // starters + équipe
    bool starter_open() const { return _vm.state() == ScriptVM::STARTER; }
    void starter_pick(int i) { _vm.starter_pick(i); _vm.step(); }
    void debug_mon() { debug_mon(387, 5); }                 // test : Tortipouss
    void debug_mon(int sp, int lv, int mv = 0) {            // test : espèce libre
        ScriptVM::PartyMon pm; pm.species = sp; pm.level = lv;
        auto bm0 = battle_make(_db, sp, lv);             // IVs/PID/XP réels
        pm.ivs = bm0.ivs; pm.pid = bm0.pid; pm.exp = bm0.exp;
        pm.hp = bm0.maxhp;
        if (mv) { pm.moves[0] = mv; pm.nmoves = 1; }
        else default_moves(_db, sp, lv, pm.moves, &pm.nmoves);
        for (int k = 0; k < pm.nmoves; k++)
            pm.pp[k] = battle_move(_db, pm.moves[k]).pp;
        _vm.party.push_back(pm); }
    void debug_mon_moves(int sp, int lv, const int* mvs, int nm) { // test : moveset libre
        ScriptVM::PartyMon pm; pm.species = sp; pm.level = lv;
        auto bm0 = battle_make(_db, sp, lv);
        pm.ivs = bm0.ivs; pm.pid = bm0.pid; pm.exp = bm0.exp;
        pm.hp = bm0.maxhp;
        pm.nmoves = std::min(4, nm);
        for (int k = 0; k < pm.nmoves; k++) {
            pm.moves[k] = mvs[k];
            pm.pp[k] = battle_move(_db, mvs[k]).pp;
        }
        _vm.party.push_back(pm); }
    const std::string& species_name(int id); // nom FR (banque 412)
    const std::vector<ScriptVM::PartyMon>& party() const { return _vm.party; }
    // stockage PC (script attend la fermeture de l'appli, comme sur DS)
    bool pc_open() const { return _vm.state() == ScriptVM::PCBOX; }
    int  pc_mode() const { return _vm.pc_mode; }
    std::vector<ScriptVM::PartyMon>& pcbox() { return _vm.pcbox; }
    void pc_close() { _vm.continue_after(ScriptVM::PCBOX); _vm.step(); }
    // ChoosePokeNick : écran de nommage ouvert pour un membre d'équipe
    bool nick_open() const { return _vm.state() == ScriptVM::NICK; }
    int  nick_slot() const { return _vm.nick_slot; }
    void nick_pick(const std::string& nm, bool cancelled);
    // ŒUF / surnom / nom d'espèce (affichage)
    std::string mon_name(const ScriptVM::PartyMon& m);
    void pc_move_party_to_box(int i, int box); // équipe -> boîte (1er slot libre)
    void pc_move_box_to_party(int i);          // boîte -> équipe
    static const char* box_name(int i);        // « BOITE 1 » .. « BOITE 18 »
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
    void draw_dwarp(float asp);                        // vortex DoDWWarp (titledemo 16)
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
    bool poketch_on() const { return _vm.sys_poketch && !_vm.poketch_hidden; }
    bool shop_open() const { return _vm.state() == ScriptVM::SHOP; }
    const std::vector<int>& shop_items() const { return _vm.shop_items; }
    uint32_t money() const { return _vm.money; }
    int  badges() const { return _vm.badges; }       // bitmask des 8 badges
    int  gender() const { return _gender; }          // 0:garçon 1:fille
    int  item_price(int id) const;                     // pl_item_data u16@0
    void shop_buy(int idx);                            // achète shop_items[idx]
    void shop_close();                                 // rend la main au script
    void force_shop() { _vm.force_shop(); }            // test écran boutique
    bool has_starter() const { return !_vm.party.empty() || _vm.var(0x4032) != 0; }
    int item_pocket(int id);                             // poche sac (item_data.narc)
    // sac DS natif
    std::vector<std::pair<int,int>> bag_items(int pocket); // ordre d'obtention
    int  item_heal(int id);          // soin PV (pl_item_data u8@24)
    int  item_field_use(int id);     // 0=non, 1=soin équipe, 2=rappel
    bool item_tossable(int id);      // tout sauf objets rares (poche 7)
    bool item_registrable(int id);   // poche 7 -> touche Y
    int  mon_maxhp(int species, int lv, uint32_t ivs = 0xFFFFFFFFu, int ev = 0);
    int  registered_item() const { return _reg_item; }
    void set_registered(int id) { _reg_item = id; }
    uint32_t item_icon_tex(int id, int frame);           // icône 32x32 (world_intro.cpp)
    uint32_t pocket_icon_tex(int pocket, bool sel);      // icône poche 16x16 (pl_bag_gra)
    uint32_t mon_icon_tex(int species, int frame);       // icône espèce 32x32 (pl_poke_icon)
    uint32_t battle_bg_tex(int bg, int tod);             // décor combat 512x256 (pl_batt_bg)
    uint32_t battle_plat_tex(int ter, int side);         // plateforme terrain (pl_batt_obj)
    void battle_plat_size(int ter, int side, int* w, int* h);
    uint32_t trainer_back_tex(int idx);                  // dos dresseur (trbgra)
    uint32_t hero_back_tex(int g);                       // dos héros par défaut (PLAYER_M/F.png ou trbgra)
    uint32_t skin_front_tex(int i);                      // face d'une skin perso i (0 si absent)
    uint32_t hero_front_tex(int g);                      // face héros par défaut (front/PLAYER_M-F.png ou trfgra)
    uint32_t trainer_front_tex(int idx);                 // face dresseur (trfgra)
    uint32_t player_back_tex();                          // dos du joueur : sprite skin ou héros par genre
    uint32_t player_front_tex();                         // face du joueur : sprite skin ou héros par genre
    uint32_t skin_back_tex(int i);                       // dos d'une skin perso i (0 si absent)
    std::vector<int> _bag_seq;                           // ordre d'obtention
    int _reg_item = 0;
    bool ui_frozen = false;                              // menu X : monde en pause (DS)
    bool pop_xmenu() { bool w = _want_xmenu; _want_xmenu = false; return w; }
    void request_xmenu() { _want_xmenu = true; }   // debug : ouvre le menu X

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
    bool obj_invisible(int i);               // sprite/modèle non rendu (diag)
    int  obj_dir(int i)  const { return _objd[i]; }
    int  obj_move(int i) const { return _objmv[i]; }
    int  obj_gfx(int i)  const { return _resolve_gfx(_objects[i].gfx); }
    bool obj_gone(int i) const { return _objgone.count(i); }
    bool obj_hide(int i) const { return _objhide.count(i); }
    bool obj_lock(int i) const { return _objlock.count(i); }
    int  follower_idx()  const { return _follower; }
    // pokémon suiveur (mod HGSS) : 1er membre non-œuf de l'équipe
    int  pokefol_species() const { return _pokefol_sp; }
    int  pokefol_variant() const { return _pokefol_var; }
    int  pokefol_slot()    const { return _pokefol; }
    Vec3 pokefol_pos()     const { return _pfpos; }
    bool pokefol_shown()   const;          // actif + pas caché (surf/combat/…)
    void set_follower_dir(const std::string& d) { _foldir = d; }
    void set_save_path(const std::string& p) { _savepath = p; }
    // debug F1 : impose le suiveur (0 = retour au 1er non-œuf de l'équipe)
    void pokefol_force(int sp, int var = 0);
    int  pokefol_forced()  const { return _pokefol_lock; }
    bool pokefol_has_sprite(int sp, int var = 0); // followers/NNN[.png existe ?
    void pokefol_respawn() { if (_pokefol >= 0) _pokefol_spawn(); }
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
    bool cell_loaded_at(int tx, int tz) const {   // diag : cellule chargée
        return _cell_at(tx * TILE + TILE / 2.f,
                        tz * TILE + TILE / 2.f) != nullptr;
    }
    // ── menu debug : puissant ──
    float dbg_speed = 1.f;                 // multiplicateur de vitesse
    bool dbg_frame = false;                // pas-à-pas : simule 1 frame
    int  _dbg_frames = 0;                  // frame boucle main (diag)
    bool noclip = false;                   // traverse murs + PNJ
    int  dbg_night = -1;                   // heure simulée : -1 auto 0 jour 1 nuit
    int  dbg_hour = -1;                    // --time HH : heure forcée (test)
    int  dbg_weather = -1;                 // --weather N : type forcé (-1 auto)
    bool dbg_weather_dyn = false;          // --weather dyn : fronts + saisons
    int  dbg_season = -1;                  // --season N : saison forcée (-1 auto)
    int  weather() const { return _wthr.type; }      // type rendu (debug/UI)
    int  season() const { return _season(); }        // 0 hiver..3 automne
    float weather_intensity() const { return _wthr.intensity; }
    int  weather_at(float wx, float wz, int base = -1) const;  // type à pos.
    void set_bgm_override(int s) { _bgm_override = s; }   // musique forcée (-1 auto)
    int  bgm_override() const { return _bgm_override; }
    int  skin = -1;                        // apparence joueur : -1 héros/héroïne, sinon gfx PNJ
    int  rskin = -1;                       // apparence rival : -1 Barry ROM, sinon SKIN_BASE+i
    int  skin_hue = 0;                     // teinte 0..359° (0 = neutre)
    int  skin_sat = 100;                   // saturation % (100 = neutre)
    int  skin_val = 100;                   // luminosité % (100 = neutre)
    uint32_t skin_mask = 0xFFFFFFFF;       // slots de palette retouchés (bit i)
    int  talk_obj() const { return _talk_obj; }   // PNJ en cours de dialogue (-1)
    int  spk_obj() const { return _spk_obj; }     // dernier PNJ animé par le script courant
    int  dlg_spk() const { return _dlg_spk; }     // locuteur figé à l'affichage du message
    uint32_t sprite_preview(int gfx);      // frame face-sud d'un sprite overworld
    bool sprite_dims(int gfx, int* w, int* h);  // taille px de la frame sud
    // aperçu retouché (hue °, sat/val %, mask = slots de palette)
    uint32_t sprite_preview_t(int gfx, int hue, int sat, int val,
                              uint32_t mask);
    uint32_t sprite_frame_t(int gfx, int frame, int hue, int sat, int val,
                            uint32_t mask);   // frame quelconque retouchée
    std::vector<RGBAf> sprite_pal(int gfx);  // couleurs (slots) du sprite
    const char* gfx_name(int gfx);         // nom OBJ_EVENT_GFX
    // skins perso : PNG dans skins/ (colonne 32px ou feuille RMXP 4x4)
    static const int SKIN_BASE = 0x4000;      // gfx >= SKIN_BASE -> skins/[i]
    struct SkinSheet {
        std::string name;
        std::vector<std::shared_ptr<Image>> frames;   // jusqu'à 32 (trous ok)
        std::vector<std::shared_ptr<Image>> bike;     // skins/bike/<nom>.png : 24 (trous ok)
    };
    void scan_skins(const std::string& dir);  // charge/recharge les PNG
    const SkinSheet* skin_custom(int i) const;
    int  skin_custom_count() const { return (int)_skins.size(); }
    void ensure_skins();
    int  tile_beh(int tx, int tz) const { return _tile_beh(tx, tz); }
    int  tile_attr_dbg(int tx, int tz) const {  // attr 16b brut (diag audit)
        return _mattr_at(tx, tz);
    }
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
    void force_trainer2(int t1, int t2) { _btl_start(true, t1, 0, 0, t2); }
    void force_starter()              { _vm.force_starter(); }   // test écran starter
    // scène starter DS (ev_pokeselect.narc) : mallette + balles en vrai 3D
    void draw_starter(float asp);
    bool starter_ball_scr(int i, float* sx, float* sy) const;  // pos écran balle
    int  starter_sel = 0;                   // balle sous le curseur
    int  starter_phase = 0;                 // 0 choix, 1 confirmation
    int  enc_rate() const { return _enc_rate; }
    int  enc_slot(int i, int* lv) const;    // espèce du slot (-1)
    bool surfing() const { return _psurf; }
    bool biking() const { return _pstate == 1; }    // PLAYER_AVATAR_CYCLING
    int  pstate() const { return _pstate; }
    void set_bike(bool on);                         // monter/descendre (sys_bike+état)
    int  use_bicycle();    // objet 450 : 0 monté 1 descendu 2 interdit 3 démontage interdit
    void bike_gear();      // B : bascule 3e/4e vitesse (DS TryCyclingGearChange)
    bool battle_active() const { return _wild_t > 0.f || _btl.active; }
    // cut-in CS (PlayHmCutIn DS) : -1 inactif, sinon frame 60fps + espèce
    float cutin_t() const { return _cutin_slot < 0 ? -1.f : _cutin_f; }
    int   cutin_sp() const { return _cutin_sp; }
    int   cutin_slot() const { return _cutin_slot; }
    // ── CS de terrain (FieldMoveTasks DS : menu équipe) ──────────────
    // fm = index DS FIELD_MOVE_* (0 Coupe..14) ; seules les CS à script
    // (Coupe/Éclate-Roc/Force/Escalade/Surf/Cascade/Anti-Brume/Flash)
    // sont lançables pour l'instant.
    std::vector<int> party_field_moves(int slot) const; // CS connues (ordre att.)
    int  usable_field_moves() const;                    // bitmask utilisable ici
    int  field_move_error(int fm) const;                // 0/1 lieu/2 badge/3 partn/4 état
    int  field_move_script(int fm) const;               // script 10008+ ou -1
    bool field_move_runnable(int fm) const {            // script ou tâche dédiée
        return field_move_script(fm) >= 0 ||
               (fm >= 9 && fm <= 11);                   // Téléport/Tunnel/Dolce-Parfum
    }
    int  field_move_id(int fm) const;                   // id attaque du fm
    bool use_field_move(int slot, int fm);              // lance la CS (menu)
    const std::string& fm_error_text(int err);          // message DS (banque 453)
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
    uint32_t other_tex(int member, int pal_member, int frame = 0); // pl_otherpoke
    int poke_feet(int sp, int face, int frame = 0) {     // ligne opaque basse
        return poke_feet_m(sp * 6 + (face ? 1 : 3), frame); }
    void poke_bbox(int sp, int face, int* top, int* bot) { // bbox verticale
        poke_bbox_m(sp * 6 + (face ? 1 : 3), 0, top, bot); }
    int poke_feet_m(int member, int frame);
    void poke_bbox_m(int member, int frame, int* top, int* bot);

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
        Vec3 pos; float ry = 0, rx = 0, rz = 0; Vec3 scale{1,1,1};
        bool hidden = false;        // masqué (étages/plateformes Canalave)
        Vec3 org;                       // origine cellule
        int64_t cell = -1;              // clé cellule source (déchargement)
        int model = -1;                 // index build_model.narc (id de prop)
        const bmd::Model* mdl = nullptr;
        // animation one-shot en cours (portes ; bm_anime.narc via bm_anime_list)
        const bmd::JntAnim* anm = nullptr;
        int anm_idx = -1;               // index dans anime_list (0..3)
        float anm_frame = 0.f;          // frame courante (fraction conservée)
        bool anm_hold = false;          // terminée : figer la dernière frame
        bool anm_loop = false;          // anim d'ambiance : boucle infinie (DS)
        std::unordered_map<int, M4> anm_cur;   // group_key -> matrice animée
        // anim matériau BTA0 (scroll/transform UV : fontaines, roues à eau)
        const std::vector<bmd::TexAnim>* tanms = nullptr;
        float tanm_frame = 0.f;
        // anim texture BTP0 (swap texture+palette : écrans, pales d'éoliennes)
        const std::vector<bmd::TexPat>* pats = nullptr;
        float panm_frame = 0.f;
        uint8_t panm_state = 0;         // 0 boucle, 1 one-shot->figé, 2 figé
    };

    Nds* _db = nullptr;
    int _header = -1;
    std::string _label, _mapname;
    std::string _floor_txt;                              // ShowCurrentFloor (0x11D)
    int _floor_var = -1;                                 // var qui ferme la fenêtre à 0xFFFF
    float _banner_t = 0.f;
    int _mus_day = -1, _mus_night = -1;     // seq SDAT de zone (arm9 MapHeader)
    int _cam_type = -1;                     // type caméra (0..16, 4 = ortho int.)
    class DsAudio* _snd = nullptr;          // moteur son (main.cpp)
    int _bgm_override = -1;                 // SetBGM du script (-1 = défaut map)
    int _bgm_cur = -2;                      // dernière seq demandée au moteur
    bool _bgm_victory = false;              // jingle de victoire en cours
    float _mus_chk = 0;                     // sondage jour/nuit (sec)
    float _waitsnd_t = 0;                   // garde-fou WAITSND (SE bloqué)
    int   _waitsnd_f = 0;                   // idem en frames (headless dt~0)
    float _waitt_acc = 0;                   // sous-pas 60 Hz WAITTIME
    float _autodlg_t = 0;                   // auto-avance dialogues (lecture)
    int   _autodlg_f = 0;                   // idem en frames (headless dt~0)
    Nds::Matrix _mtx;
    std::vector<Nds::Zev::Warp> _warps;
    std::vector<Nds::Zev::Obj> _objects;
    std::unordered_map<int64_t, std::shared_ptr<Cell>> _cells;
    std::vector<PropInst> _props;
    std::unordered_map<int, std::shared_ptr<Land>> _lands;
    std::unordered_map<int, std::shared_ptr<bmd::Model>> _bmds;
    std::unordered_map<int, std::vector<GpuMesh>> _obj3d;   // gfx -> meshes fldeff (panneaux)
    std::vector<GpuMesh>& _obj_3d(int gfx);
    std::vector<GpuMesh> _gira3d;                 // billboard-model Giratina origin
    bool _gira3d_ok = false;
    std::vector<GpuMesh>& _obj_gira();
    std::vector<GpuMesh>& _fld_mesh(int member);       // membre fldeff quelconque
    std::vector<GpuMesh>& _fld_upload(bmd::Model mdl, std::vector<GpuMesh>& out);
    std::unordered_map<int, std::vector<GpuMesh>> _fldm; // cache membre -> meshes
    const bmd::Model* _fld_mdl(int member);            // modèle fldeff persistant (PAT0)
    const std::vector<bmd::TexPat>* _fld_pat(int member); // PAT0 fldeff (nsbtp)
    std::unordered_map<int, std::shared_ptr<bmd::Model>> _fldmdls;
    std::unordered_map<int, std::vector<bmd::TexPat>> _fldpats;
    // scène starter DS (choose_starter_app.c) : ev_pokeselect.narc en vrai 3D
    bool _scn_ok = false;
    float _scn_t = 0.f;                                    // temps scène
    float _scn_cam = 0.f;                                  // zoom confirmation 0->1
    std::vector<PropInst> _scn;                            // mallette, 3 balles, socle
    std::unordered_map<int, std::shared_ptr<bmd::Model>> _scn_mdl;
    std::unordered_map<int, bmd::JntAnim> _scn_anm;
    float _scn_scr[3][2]{};                                // NDC des 3 balles (curseur)
    std::vector<GpuMesh> _shadow_meshes;                    // shadow.nsbmd (fldeff 17)
    std::vector<GpuMesh> _grass_meshes, _vtall_meshes;      // tall_grass fldeff 83/89
    void _load_shadow();
    void _load_grass();
    void _grass_overlay(const Vec3& feet, int beh, const Vec3& cam, const Mat4& mvp);
    std::unordered_map<std::string, bmd::TexEnt> _texset;
    std::unordered_map<std::string, bmd::TexEnt> _ptexset; // areabm_texset (props)
    // build_model_matshp.dat : par modèle, paires (matériau, shape) dessinées
    // sans transform de nœud (NNS_G3dDraw1Mat1Shp sous la base du prop)
    std::unordered_map<int, std::vector<std::pair<int, int>>> _matshp;
    bool _matshp_loaded = false;
    const std::vector<std::pair<int, int>>* _matshp_pairs(int model);
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
    int _spk_obj = -1;                      // locuteur scripté : dernier PNJ
                                          // animé/verrouillé par le script courant
    int _dlg_spk = -1;                      // locuteur du message affiché
                                          // (figé par cb_msg au moment du Message)
    ScriptVM _vm;
    // ScriptContext DS : le script qui a déclenché un Warp/changement de map
    // reprend après les scripts d'init de la nouvelle map (FieldTask_InitCall
    // de la map-change task — le ctx conserve bytecode, pc, état et banque).
    ScriptVM::Ctx _vm_ctx;
    bool _vm_cont = false;                  // ctx sauvegardé en attente de reprise
    int  _vm_ctx_bank = -1, _vm_ctx_member = -1;
    void _vm_resume();                      // reprend le ctx sauvegardé
    // field-return DS (FieldTransition_StartMap) : après un combat gagné /
    // capturé / fui, les scripts OnLoad puis OnResume se rejouent avant la
    // reprise du script suspendu (REMOVE_OBJECT nettoie les statiques).
    bool _fldret = false;                   // file d'init active
    bool _fldret_susp = false;              // un script était suspendu (BATTLE)
    ScriptVM::Ctx _fldret_ctx;              // ctx du script suspendu
    std::vector<int> _fldret_q;             // types init restants (4,3)
    bool _fldret_next();                    // lance le prochain script d'init
    std::shared_ptr<Narc> _scrnarc;         // garde le fichier script vivant
    std::shared_ptr<Narc> _encnarc;         // pl_enc_data.narc (garde _enc vivant)
    const Bytes* _enc = nullptr;            // WildEncounters du header courant
    int _enc_rate = 0, _enc_steps = 0;      // taux terrestre + pas depuis combat
    // roamers DS (roaming_pokemon.c) : Mesprit/Cresselia/Darkrai/3 oiseaux
    struct Roamer {
        int sp = 0, lv = 0, hp = -1, status = 0, route = -1; // route = id header
        uint32_t ivs = 0xFFFFFFFFu, pid = 0;
    };
    Roamer _roamers[6];
    int _wild_roamer = -1;                  // slot roamer du combat en cours
    int _prev_map = 0;                      // header précédent (GetPreviousMapId)
    int _exit_map = 0, _exit_x = 0, _exit_z = 0; // 0x2B5 localisation de sortie
    int  _roamer_random_map(int exclude);   // route aléatoire != exclude
    void _roamer_step_maps();               // déplacement à chaque changement
    float _wild_t = 0;                      // intro rencontre sauvage en cours
    int _wild_sp = 0, _wild_lv = 0;         // espèce/niveau du sauvage actif
    uint32_t _wild_tex = 0;                 // texture du sprite sauvage
    std::unordered_map<int, uint32_t> _pktex; // cache textures pl_pokegra
    std::unordered_map<int, uint32_t> _othertex; // cache textures pl_otherpoke
    uint32_t _poke_tex(int member, int frame = 0); // sprite membre (sp*6+idx) + frame anim
    Battle _btl;                            // combat en cours
    int _btl_key = 0;                       // touche en attente pour la battle
    void _btl_start(bool trainer, int tid, int sp, int lv, int tid2 = 0);
    void _btl_end();                        // recopie résultat -> VM/partie
    std::unordered_map<int, std::vector<std::string>> _msgbanks; // banques décodées
    std::string _dialog;                    // texte affiché en cours
    float _dlg_chars = 0.f;                 // nb de chars dévoilés (page courante)
    size_t _dlg_page = 0;                   // début de la page courante (\n\n DS)
    std::vector<std::pair<int,int>> _dlg_cols;  // {FF00} -> (octet, palette)
    size_t dlg_page_end() const;            // fin de la page courante
    int    dlg_page_chars() const;          // nb de caractères UTF-8 dans la page
    size_t _dlg_wrap_end(size_t mend) const; // début de la 3e ligne (scroll)
    bool   _dlg_dismiss = false;            // script fini, boîte ouverte : A ferme
    void   _script_end_box();               // fin de script : boîte DS persistante
    std::string _buf[16];                   // buffers {STRVAR} StringTemplate
    struct Fade { float t = -1, dur = 0; uint32_t color = 0; bool out = true; } _fade;
    // DoDWWarp (0x320) : vortex de ~2,1 s avant le Warp scripté — le temps
    // restant pilote l'overlay spiral + retient _warp_req (DS : process
    // fils dw_warp, 85 fr d'anim + 20 fr de fondu @30 Hz)
    float _dwarp_t = -1.f;
    bool _dwarp_sfx = false;
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
    std::unordered_set<int> _objpass;       // traversables (puits DW remplis)
    std::unordered_set<int> _objnocoll;     // MAP_OBJ_STATUS_18 : sans collision
                                            // (opcode 0x2B6, mv=48 téléporteurs DS)
    std::unordered_set<int> _objlock;       // index gelés (Lock/Release par script)
    std::unordered_map<int,float> _objemote; // bulle "!" (index objet, 0xFF = joueur)
    std::unordered_map<int,float> _objdark;  // paletteDarkness 0-16 (arrivée Giratina DS)
    GpuMesh _emote_bb; uint32_t _emote_tex = 0;
    GpuMesh _emote2_bb; uint32_t _emote2_tex = 0;   // bulle « … » (joueur inactif)
    float _idle_t = 0.f;                    // temps sans bouger (s)
    float _idle_bub = 0.f;                  // durée restante bulle « … »
    GpuMesh _pkbb;                          // quad sprite sauvage 48×24
    // dresseur qui repère le joueur (trainer_encounter.c) : le moteur remplit
    // les slots SCRIPT_MANAGER_TRAINER_0/1 puis lance le template d'approche
    // (scr_seq[1114] fn928) ; les opcodes B6-B9 pilotent les tâches de marche.
    struct ApprSlot { int obj = -1, tid = 0, dir = 0, range = 0, script = 0; };
    ApprSlot _appr[2];
    int _appr_type = -1;                    // -1 aucun, 0 singles, 1 doubles, 2 vs2
    struct ApprTask { int obj = -1, phase = 0, dx = 0, dz = 0, steps = 0;
                      float t = 0.f, tgt = 0.f; bool done = true; };
    ApprTask _appr_task[2];               // start_approaching_trainer_task
    void _tick_appr(float dt);             // tâches d'approche en cours
    int  appr_start(int n);                // 0x00B6 : lance la marche du slot n
    int  appr_done(int n);                 // 0x00B7 : terminée ? (libère)
    int  appr_id(int n) const;             // 0x00B9 : trainerID du slot n
    int  appr_type() const { return _appr_type < 0 ? 0 : _appr_type; }
    bool _appr_active() const { return _appr_type >= 0; }
    int  _alive_party() const;             // Pokémon avec PV > 0
    // suiveur (FollowHero / StartFollow, ex. rival Route 201)
    int _follower = -1;                     // index de l'objet suiveur (-1 = aucun)
    bool _mbox = false;                     // fenêtre message ouverte (DS window)
    std::deque<std::pair<int,int>> _ftrail; // file des tuiles visitées par le joueur
    int _fstep = 0; float _ftx = 0, _ftz = 0; // pas de suiveur en cours
    float _fanim = 0.f;                       // phase marche du suiveur-PNJ
    // pokémon suiveur (mod HGSS) : entité dédiée (pas un objet de map,
    // pas de collision, jamais de script) qui serpente sur _pftrail.
    int  _pokefol = -1;                     // slot équipe suivi (-1 = aucun)
    int  _pokefol_lock = 0;                 // debug : espèce imposée (0 = auto)
    int  _pokefol_sp = 0;                   // espèce rendue (détecte le switch)
    int  _pokefol_var = 0;                  // variante : 0=mâle, 1=femelle, >=2 forme
    int  _pokefol_dbgvar = 0;               // variante imposée en mode forcé
    Vec3 _pfpos{0, 0, 0};                   // position monde
    int  _pfdir = 0;                        // dir monde 0=S 1=N 2=O 3=E
    // parcelles à baies (berry_patches.c DS) : 128 patchs persistants.
    // isGrowing n'est activé que pour les parcelles vues (UpdateGrowthStates)
    // ou plantées — la croissance tourne en minutes réelles comme la RTC DS.
    struct BerryPatch {
        uint8_t berryID = 0;                 // tag 1..64 (itemID-148), 0 = vide
        uint8_t stage = 0;                   // 0 vide,1 planté,2 germé,3 pousse,4 fleuri,5 fruit
        uint32_t stageMin = 0;               // minutes restantes dans le stade
        uint16_t moistMin = 0;               // report de drain (<60)
        uint8_t replant = 0, yield = 0;
        uint8_t moistRating = 0;             // 0..100 -> sol 77/76/75
        uint8_t yieldRating = 0;             // multiplicateur 0..5
        uint8_t mulch = 0;                   // 0..4 (GROWTH/DAMP/STABLE/GOOEY)
        uint8_t growing = 0;                 // isGrowing (vu ou planté)
    };
    BerryPatch _berry[128];
    struct BerryGrowth { uint8_t baseYield = 0, stageDur = 0, drain = 0; };
    BerryGrowth _bgrow[64];                  // nuts_data.narc (par tag-1)
    uint64_t _berry_minute = 0;              // horodatage epoch-minute du dernier tick
    bool _berry_ready = false;               // init table + données chargées
    void _berry_setup();                     // charge nuts_data + table d'init
    void _berry_elapse(uint32_t minutes);    // port de BerryPatches_ElapseMinutes
    void _berry_tick();                      // minutes réelles écoulées -> elapse
    int  _berry_op(int sub, int arg);        // opcodes script (target = _talk_obj)
    int  _berry_patch() const;               // patchID de l'objet parlé (-1)
    float _pfanim = 0.f;                    // phase marche (2 pas par direction)
    bool _pfjump = false;                   // petit saut de joie en cours
    float _pfjt = 0.f;                      // temps du saut
    int  _pfstep = 0; float _pftx = 0, _pftz = 0; // pas de 1 tuile en cours
    std::deque<std::pair<int,int>> _pftrail; // file des tuiles du joueur
    float _pfemote = 0.f;                   // durée restante de la bulle ♥
    float _pfcry_t = 0.f;                   // anti-spam du cri à l'interaction
    std::string _foldir;                    // dossier followers/ (set par main)
    std::string _savepath;                  // save.dat (set par main)
    struct FolSheet {                       // colonne PNG : 8 frames fw×fh
        std::shared_ptr<Image> img;         // source RGBA (gardée pour upload)
        int fw = 0, fh = 0;                 // taille d'une frame
        uint32_t tex[8] = {};               // textures GL par frame
        bool tried = false;                 // chargement tenté (évite rescan)
    };
    std::unordered_map<int, FolSheet> _folcache;      // espèce -> feuille
    std::unordered_map<uint32_t, GpuMesh> _folbb;     // (fw<<16|fh) -> quad
    uint32_t _heart_tex = 0;                          // bulle ♥ (réaction)
    GpuMesh _heart_bb;
    const FolSheet* _fol_sheet(int sp, int var = 0);  // charge followers/NNN[_f|_k].png
    int  _pokefol_variant() const;                    // variante du suiveur courant
    void _pokefol_tick(float dt);                     // marche sur la file
    void _pokefol_refresh();                          // choix du membre suiveur
    void _pokefol_spawn();                            // place derrière le joueur
    void _pokefol_talk();                             // interaction touche A
    std::unordered_map<int64_t,uint16_t> _tileover; // SetTilePassable (tx,tz)->attr
    // grille d'attributs précalculée de TOUTE la matrice (w*32 × h*32
    // tuiles, u16) : la collision terrain ne dépend plus du streaming —
    // une cellule non encore chargée garde son vrai attribut au lieu
    // d'être un mur fantôme (world_collision.cpp _blocked/_tile_beh).
    std::vector<uint16_t> _mattrs;
    int _mattr_at(int tx, int tz) const {          // -1 hors matrice
        if (tx < 0 || tz < 0 || _mtx.w <= 0 ||
            tx >= _mtx.w * 32 || tz >= _mtx.h * 32) return -1;
        return _mattrs.empty() ? -1
            : (int)_mattrs[tz * (_mtx.w * 32) + tx];
    }
    std::unordered_set<int> _objpersist;    // index SetObjectFlagIsPersistent
    int _pstate = 0, _pstate_pending = 0;   // état avatar (SetPlayerState)
    int _bgear = 0, _bspd = 0;              // vélo : vitesse DS (0=3e/1=4e) + palier 0-3
    bool _bbrake = false;                   // roue libre engagée (speed>=2, DS BIKE_BRAKE)
    bool _bon_road = false;                 // ForceBicycling : piste cyclable (AVATAR_MOVE_ON_CYCLING_ROAD)
    int _bslope = 0;                        // pente boue : 1=glissière sud, 2=descente forcée
    int _cam_mode = 0; float _cam_x = 0, _cam_z = 0;  // 0=joueur 1=libre 2=objet
    float _cam_ox = 0, _cam_oy = 0, _cam_oz = 0;      // 3=offset joueur (MoveCamera)
    std::unordered_map<int,int> _seqvol;    // volumes initiaux par séquence
    bool _want_xmenu = false;             // ShowStartMenu : main ouvre le menu X
    int _pic_species = 0;                   // PokemonPicture actif (espèce)
    bool _lost_pc = false;                  // LostGoPc : warp maison au prochain tick
    bool _tv_open = false;                  // script TV -> écran Télé-Motisma
    bool _tv_scene = false;                 // séquence TV intro en cours
    bool _tv_suppress = false;              // boîtes du script avalées (overlay)
public:
    bool tv_ui = false;                     // poste affiché (E -> avance le texte)
    bool tv_open() const { return _tv_open || _vm.tv_req; }
    void tv_close() { _tv_open = false; _vm.tv_req = false; }
    bool tv_feed() const { return _tv_suppress; }   // scène en cours (texte sur l'écran)
    void tv_advance();                      // E pendant la scène : avance le feed
private:
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
    bool _check_trainer_sight();             // un dresseur voit le joueur ?
    bool _obj_at_tile(int tx, int tz, float y, int exclude) const;
    int  _facing_obj() const;               // sub_0203C9D4 DS (comptoir compris)
    void _try_wild(int tb);                  // WildEncounters_TryWildEncounter
    void _swarm_map_species(int* map, int* sp) const;  // essaim du jour
    bool _wild_force = false;                // tuto capture : force la rencontre
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
    // lieu spécial (SetSpecialLocation / dwarp 0x100) — DS Location :
    // hdr,warp + position/direction d'arrivée explicites (x/z/dir = -1
    // quand mémorisé automatiquement depuis l'entrée).
    struct { int hdr = -1, warp = -1, x = -1, z = -1, dir = -1; } _special;
    int _blackout_id = 1;                           // spawn_locations.c : index table (défaut = maison)
    struct WarpTask {
        int st = -1;                            // phase en cours (-1 = inactif)
        int type = 0;                           // 0 fondu, 1 porte, 3 escaliers
        int dh = -1, dw = -1, adir = 0;         // destination + dir d'arrivée
        int dx = -1, dz = -1, ddir = -1;        // arrivée explicite (lieu spécial)
        int sub = 0;                            // sous-phase d'arrivée
        float wx = 0, wz = 0, wspd = 0;         // cible + vitesse marche auto
        float py = 0;                           // sol sous le joueur (panneau)
        float t = 0.f;                          // accumulateur temps (ticks DS)
        int door = -1;                          // prop de porte animée (-1)
        int bo_tx = -1, bo_tz = -1;             // blackout : tuile imposée au load
    } _wt;
    bool _phidden = false;                      // joueur caché (MapObject hidden)
    bool _phcalc = true;                        // hauteur recalculée (0x211)
    int  _gm_sfx = -1;                          // SE tram en cours (TRAIN04)
    float _gm_sfx_t = 0.f;                      // plafond attente SE tram (s)

    // animations de props (bm_anime_list -> bm_anime) : portes one-shot.
    // map_prop_animation.c / ov5_021D431C.c de la décomp.
    std::unordered_map<int, std::vector<int>> _alist;      // model -> archive ids
    std::unordered_map<int, bmd::JntAnim> _bca;            // archive -> anim BCA0
    std::unordered_map<int, std::vector<bmd::TexAnim>> _btas; // archive -> SRT0
    std::unordered_map<int, std::vector<bmd::TexPat>> _btps;  // archive -> PAT0
    std::unordered_map<int, int> _doortags;                // tag script -> _props
    const std::vector<int>* _anim_ids(int model);          // cache bm_anime_list
    int  _anim_auto(int model);        // idx d'anim bouclante (flags==0) ou -1
    const bmd::JntAnim* _bca_anim(int aid);                // cache bm_anime (BCA0)
    const std::vector<bmd::TexAnim>* _bta_anim(int aid);   // cache bm_anime (BTA0)
    const std::vector<bmd::TexPat>* _btp_anim(int aid);    // cache bm_anime (BTP0)
    // résout texName+palName -> texture GL (texset du modèle ou de la zone)
    uint32_t _pat_tex(const bmd::Model* mdl, const std::string& tn,
        const std::string& pn);
    int  _prop_anim_count(int model);                      // nb d'anims du prop
    int  _find_door_prop(int tx0, int tz0, int tx1, int tz1); // porte en hitbox tuiles
    bool _prop_play(int pi, int idx);                      // lance l'anim idx
    bool _prop_anim_done(const PropInst& p);               // one-shot terminée ?
    void _prop_anim_stop(PropInst& p);
    void _tick_prop_anims(float dt);
    Mat4 _prop_node_delta(const PropInst& p, int mesh_i);  // anim * repos⁻¹
    static Mat4 _prop_mat(const PropInst& p);              // Ry·Rx ou Rx·Ry·Rz (NITRO)

    // features d'arène dynamiques (overlay008/gym_features.c DS) :
    // plateau de hauteur dynamique couvrant le bassin — les tuiles
    // 0x56/57/58 ne sont franchissables qu'au niveau d'eau correspondant,
    // 0x59 = colonne d'eau (bloquée tant que la nappe dépasse le sol).
    // Sunyshore : engrenages = props rotatifs + régions-murs par état.
    struct SunGear { int16_t model; int8_t x, y, z, init; bool cw, vert; };
    struct SunReg { int8_t x, z, w, h; };
    static constexpr SunGear SUN_GEARS[3][13] = {
        // salle 1 : 3 engrenages L
        { {459,3,0,8,1,false,false}, {460,8,0,8,2,true,false},
          {459,13,0,8,0,false,false} },
        // salle 2 : 6 (2 verticaux)
        { {459,6,0,8,1,false,false}, {458,11,0,8,3,true,false},
          {463,15,3,8,1,true,true}, {463,2,3,13,1,false,true},
          {458,6,0,13,2,true,false}, {461,11,0,13,2,false,false} },
        // salle 3 : 13 (4 verticaux)
        { {461,6,6,8,1,false,false}, {460,11,6,8,0,true,false},
          {462,16,6,8,0,false,false}, {463,2,3,13,0,true,true},
          {460,6,6,13,1,true,false}, {461,11,6,13,3,false,false},
          {462,16,6,13,3,true,false}, {463,20,3,13,1,false,true},
          {464,2,3,18,0,false,true}, {459,6,0,18,1,true,false},
          {462,11,0,18,3,false,false}, {461,16,0,18,0,true,false},
          {464,20,3,18,1,true,true} },
    };
    static constexpr int SUN_NGEARS[3] = {3, 6, 13};
    static constexpr int SUN_DEFROT[3] = {2, 1, 0};   // état par défaut par salle
    static constexpr int SUN_ENTZ[3]   = {14, 21, 25}; // z d'entrée -> rot 0
    static constexpr SunReg SUN_REG[3][40] = {
        { {1,8,2,1},{3,6,1,2},{4,8,2,1},{3,9,1,2},{6,8,2,1},{8,6,1,2},
          {9,8,2,1},{8,9,1,2},{11,8,2,1},{13,6,1,2},{14,8,2,1},{13,9,1,2} },
        { {4,8,2,1},{6,6,1,2},{7,8,2,1},{6,9,1,2},{9,8,2,1},{11,6,1,2},
          {12,8,2,1},{11,9,1,2},{15,6,1,5},{2,11,1,5},{4,13,2,1},
          {6,11,1,2},{7,13,2,1},{6,14,1,2},{9,13,2,1},{11,11,1,2},
          {12,13,2,1},{11,14,1,2} },
        { {4,8,2,1},{6,6,1,2},{7,8,2,1},{6,9,1,2},{9,8,2,1},{11,6,1,2},
          {12,8,2,1},{11,9,1,2},{14,8,2,1},{16,6,1,2},{17,8,2,1},
          {16,9,1,2},{2,11,1,5},{4,13,2,1},{6,11,1,2},{7,13,2,1},
          {6,14,1,2},{9,13,2,1},{11,11,1,2},{12,13,2,1},{11,14,1,2},
          {14,13,2,1},{16,11,1,2},{17,13,2,1},{16,14,1,2},{20,11,1,5},
          {2,16,1,5},{4,18,2,1},{6,16,1,2},{7,18,2,1},{6,19,1,2},
          {9,18,2,1},{11,16,1,2},{12,18,2,1},{11,19,1,2},{14,18,2,1},
          {16,16,1,2},{17,18,2,1},{16,19,1,2},{20,16,1,5} },
    };
    static constexpr uint8_t SUN_LIST[3][4][14] = {
        { {1,2,4,5,10,11}, {0,1,5,6,9,10},
          {0,3,6,7,8,9}, {2,3,4,7,8,11} },
        { {1,2,4,5,7,8,9,10,11,12,15}, {0,1,4,5,6,11,12,13,14,0,0},
          {0,3,5,6,7,8,9,10,12,13,17}, {2,3,4,6,7,10,11,13,16,2,2} },
        { {2,6,7,11,14,15,17,21,25,28,29,31,38,39},
          {1,4,7,10,12,15,16,20,22,26,29,30,34,35},
          {0,4,5,9,13,16,19,23,25,27,30,33,36,39},
          {3,5,6,8,12,13,14,18,24,26,27,28,32,37} },
    };
    static constexpr int SUN_NLIST[3] = {6, 11, 14};

    // Eterna : pendule florale (gym_features.c DS). Horloge 13x13 aux
    // tuiles (5,7)-(17,19), centre (11,13) ; l'état (0..4) persiste via
    // SystemVars DS -> _eterna_clock sauvegardé. 5 états = vaincu les
    // dresseurs 1/2/3 puis le champion ; 5 heures cibles par état.
    static constexpr int ET_TIME[5][2] = {   // {heure, minute} par état
        {7,25}, {6,15}, {9,15}, {0,45}, {0,30} };
    static constexpr uint16_t ET_HROT[12] = {   // rotation aiguille par heure
        0x0000, 0x1559, 0x2AB2, 0x4000, 0x5564, 0x6ABD,
        0x8000, 0x956F, 0xAAC8, 0xC000, 0xD56A, 0xEAC3 };
    // tuiles-saut par-dessus l'aiguille : {x, z, axeNS} (-1 = pas de saut)
    static constexpr int8_t ET_JUMP[5][3] = {
        {-1,-1,0}, {11,18,1}, {6,13,0}, {11,8,1}, {11,8,1} };
    // bitmap pendule : 1 = mur (cadre), 0 = traversable (face/aiguilles)
    static constexpr uint8_t ET_CLOCK[5][169] = {
        { 1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          0,1,1,0,0,0,0,0,0,0,1,1,0 },
        { 1,1,1,1,1,1,1,1,1,1,1,1,0, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,0,
          1,1,1,1,1,1,1,1,1,1,1,1,0, 1,1,1,1,1,1,0,1,1,1,1,1,0,
          1,1,1,1,1,0,0,0,0,0,0,0,0, 1,1,1,1,1,1,0,1,1,1,1,1,0,
          1,1,1,1,1,1,0,1,1,1,1,1,0, 1,1,1,1,1,1,0,1,1,1,1,1,0,
          1,1,1,1,1,1,0,1,1,1,1,1,1, 1,1,1,1,1,1,0,1,1,1,1,1,1,
          0,1,1,0,0,0,0,0,0,0,1,1,0 },
        { 0,1,1,1,1,1,1,1,1,1,1,1,0, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 0,1,1,1,1,1,1,1,1,1,1,1,0,
          0,1,1,1,1,1,1,1,1,1,1,1,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,0,0,0,0,0,0,0,0,0,0,0,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,1,1,1,1,1,1,1,1,1,1,1,0, 0,1,1,1,1,1,1,1,1,1,1,1,0,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          0,1,1,0,0,0,0,0,0,0,1,1,0 },
        { 0,1,1,0,0,0,0,0,0,0,1,1,0, 1,1,1,1,1,1,0,1,1,1,1,1,1,
          1,1,1,1,1,1,0,1,1,1,1,1,1, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,1,1,1,1,1,0,1,1,1,1,1,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,0,0,0,0,0,0,0,1,1,1,1,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,1,1,1,1,1,1,1,1,1,1,1,0, 0,1,1,1,1,1,1,1,1,1,1,1,0,
          1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,
          0,1,1,0,0,0,0,0,0,0,1,1,0 },
        { 0,1,1,0,0,0,0,0,0,0,1,1,0, 1,1,1,1,1,1,0,1,1,1,1,1,1,
          1,1,1,1,1,1,0,1,1,1,1,1,1, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,1,1,1,1,1,0,1,1,1,1,1,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,1,1,1,1,0,0,0,1,1,1,1,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          0,1,1,1,1,1,0,1,1,1,1,1,0, 0,1,1,1,1,1,0,1,1,1,1,1,0,
          1,1,1,1,1,1,0,1,1,1,1,1,1, 1,1,1,1,1,1,0,1,1,1,1,1,1,
          0,1,1,0,0,0,0,0,0,0,1,1,0 } };
    // rangée fontaines z=19, tuiles x 1..21 : 1 = jet bloquant
    static constexpr uint8_t ET_FOUNT[5][21] = {
        {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
        {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
        {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} };
    // ov5_021F4098 : côtés fontaines affichés à l'init (2=les deux)
    static constexpr uint8_t ET_FOUNT_INIT[5] = {2, 2, 0, 255, 255};

    // ── Canalave : 24 plateformes mobiles (sCanalavePlatformPaths) ───────
    // modèles : 295 z_dir, 296 x_dir, 297 y_dir, 298 red (lift 0->3)
    // étages visuels : props 301/302/303 à y=160/320/480
    struct CanalavePlat {
        uint16_t model;
        uint8_t ax, ay, az, bx, by, bz;   // positions tuiles A/B (y=haut/16)
        uint8_t fa, fb;                   // étages A/B
    };
    // modèles = membres NARC (ligne .order − 1) : z=294 x=295 y=296 rouge=297
    static constexpr CanalavePlat CANA_PLAT[24] = {
        {297, 16,0, 9, 16,30, 9, 0,3}, {296, 11,0,13, 11,10,13, 0,1},
        {296, 15,0,13, 15,10,13, 0,1},  {296, 19,0,13, 19,10,13, 0,1},
        {296, 24,0,13, 24,10,13, 0,1},  {296, 21,0,22, 21,10,22, 0,1},
        {296, 25,0, 9, 25,10, 9, 0,1},  {296, 25,0,22, 25,10,22, 0,1},
        {296, 29,0,22, 29,10,22, 0,1},  {296,  5,10,26, 5,20,26, 1,2},
        {295, 11,10,22, 18,10,22, 1,1}, {296, 29,10, 9, 29,20, 9, 1,2},
        {295, 10,20, 4, 14,20, 4, 2,2}, {295, 19,20, 4, 22,20, 4, 2,2},
        {295,  7,20,12, 22,20,12, 2,2}, {295,  9,20,26, 21,20,26, 2,2},
        {294,  2,20,19,  2,20,22, 2,2}, {294, 26,20,16, 26,20,22, 2,2},
        {294, 29,20,16, 29,20,22, 2,2}, {297,  7,0, 9,  7,30, 9, 0,3},
        {295, 19,30, 4, 26,30, 4, 3,3}, {295,  5,30,26, 26,30,26, 3,3},
        {294, 29,30, 7, 29,30,23, 3,3}, {294,  2,30,12,  2,30,23, 3,3} };
    // sCanalaveGymPlatformsStartInPositionB (reset à chaque entrée)
    static constexpr uint32_t CANA_START_B =
        (1u<<6)|(1u<<7)|(1u<<8)|(1u<<10)|(1u<<11)|(1u<<16)|(1u<<18)|(1u<<19)|(1u<<21)|(1u<<23);
    // sCanalaveGymCollisionMaps : 4 étages, bitmask 32 tuiles x par ligne z
    static constexpr uint32_t CANA_COL[4][32] = {
        {
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
          0xF800023Fu, 0xF800023Fu, 0xF800023Fu, 0xFFFFFE3Fu,
          0xF802003Fu, 0xF802003Fu, 0xF802003Fu, 0xF802003Fu,
          0xF800003Fu, 0xF800003Fu, 0xF80FFE3Fu, 0xF808003Fu,
          0xF8F8003Fu, 0x8888003Fu, 0x8888003Fu, 0x8888003Fu,
          0x8F88003Fu, 0x800C403Fu, 0x8008003Fu, 0x8008003Fu,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        },
        {
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x8040023Fu,
          0x8040023Fu, 0x8040023Fu, 0x8F4FE23Fu, 0x8848223Fu,
          0x884BA3FFu, 0x884BA3FFu, 0x884BA3FFu, 0xFFFFE23Fu,
          0x804223FFu, 0x804223FFu, 0x804223FFu, 0x804FFFFFu,
          0x9FCFFFFFu, 0x880FFFFFu, 0x880FFFFFu, 0x880FFFFFu,
          0x88FFFC01u, 0x888FFC01u, 0x8883F0F9u, 0x888FFC81u,
          0xFFFFFC81u, 0xFFFFFC81u, 0xFFFFFC81u, 0xFFFFFC81u,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        },
        {
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x80FCFE09u,
          0x80303809u, 0x80FCFE09u, 0x80F03E09u, 0xFBF03FC9u,
          0x8AF3BFC9u, 0x8AF3BFC9u, 0x8AF3BFC9u, 0x8AFFFFCFu,
          0x8A3FFF01u, 0x8AFFFFC1u, 0x8AFFFFC1u, 0xDBFFFFC1u,
          0xDBFFFFC1u, 0xFFFFFFC1u, 0xFFFFFFFBu, 0xFFFFFFFBu,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xDBFFFFFBu, 0xDBFFFFFBu,
          0x817FFF01u, 0x817FFF01u, 0x811FFC01u, 0x817FFF01u,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        },
        {
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x8FFEFFFFu,
          0x83F07FFFu, 0x8FFC7FFFu, 0xDFF83FFFu, 0xDFF83FFFu,
          0xFFF83E01u, 0xFFF83E01u, 0xFFF83E01u, 0xFFFFFFFBu,
          0xFFFFFFFBu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xDFFFFFFBu,
          0xDFFFFFFBu, 0x8FFFFFF1u, 0x83FFFFC1u, 0x8FFFFFF1u,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        }
    };

    // ── Hearthome : gym obscur (gym_features.c, kind=5) ──────────────────
    // salles d'entraînement : énigme de portes aléatoire + indice
    struct HearthomeRoom {
        uint8_t hdr;           // 89 salle 1 (15x8), 90 salle 2 (27x20)
        uint8_t firstDoor;     // premier doorID (0/3)
        uint8_t numDoors;      // portes de sortie (3/5)
        uint8_t ox, oz;        // offset de la grille d'indices
        uint8_t sx, sz;        // taille de la grille
        const uint8_t* clues;  // bitmap positions valides (0 = OK)
    };
    static constexpr uint8_t HH_CLUE1[15 * 8] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1,
        0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0,
        0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0,
        0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0,
        0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    };
    static constexpr uint8_t HH_CLUE2[27 * 20] = {
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
        0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0,
        0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0,
        0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0,
        0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0,
        0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0,
        1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0,
        1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1,
        0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    };
    static constexpr HearthomeRoom HH_ROOM[2] = {
        {89, 0, 3, 1, 3, 15, 8,  HH_CLUE1},
        {90, 3, 5, 1, 3, 27, 20, HH_CLUE2},
    };
    // tuiles des portes par salle (sTrainerRoom1Doors / 2Doors)
    static constexpr int8_t HH_DOOR[2][5][3] = {
        {{0, 4, 2}, {1, 8, 2}, {2, 12, 2}, {-1, 0, 0}, {-1, 0, 0}},
        {{3, 4, 2}, {4, 9, 2}, {5, 14, 2}, {6, 19, 2}, {7, 24, 2}},
    };
    // cône de lampe (fldeff 115) : offset de position par portée 0-5 et
    // direction DS (N,S,O,E) — Unk_ov5_022008E0 ; scale = Unk_ov5_022007C0
    static constexpr float HH_CONE_POS[4][6][3] = {
        {{0, -15, -15}, {0, -15, -31}, {0, -15, -47},
         {0, -15, -63}, {0, -15, -79}, {0, -15, -95}},
        {{0, 0, 13}, {0, 0, 27}, {0, 0, 44},
         {0, 0, 60}, {0, 0, 78}, {0, 0, 93}},
        {{-13, 0, 6}, {-29, 0, 9}, {-45, 0, 10},
         {-61, 0, 10}, {-78, 0, 10}, {-85, 0, 10}},
        {{13, 0, 6}, {29, 0, 9}, {45, 0, 10},
         {61, 0, 10}, {78, 0, 10}, {85, 0, 10}},
    };
    static constexpr float HH_CONE_SCL[4][6][3] = {
        {{.75f, .0625f, 1}, {1, 1.25f, 1}, {1, 2, 1},
         {1, 3, 1}, {1, 4, 1}, {1, 5, 1}},
        {{.75f, .0625f, 1}, {1, 1.25f, 1}, {1, 2, 1},
         {1, 3, 1}, {1, 4, 1}, {1, 5, 1}},
        {{1, .5f, 1}, {1, 1.5f, 1}, {1, 2.5f, 1},
         {1, 3.5f, 1}, {1, 4.5f, 1}, {1, 5, 1}},
        {{1, .5f, 1}, {1, 1.5f, 1}, {1, 2.5f, 1},
         {1, 3.5f, 1}, {1, 4.5f, 1}, {1, 5, 1}},
    };
    // Unk_ov5_0220077C : angles x/y/z en degrés (MTX_Rot33Angles, Rz·Ry·Rx)
    static constexpr float HH_CONE_ROT[4][3] = {
        {270, 180, 0}, {90, 180, 180}, {0, 270, 270}, {0, 90, 90}};
    // direction native (0=S 1=N 2=O 3=E) -> direction DS (0=N 1=S 2=O 3=E)
    static constexpr int8_t HH_DIR[4] = {1, 0, 2, 3};

    // villa_furniture.c sVillaFurnitures : 23 slots {type, solid,
    // bounds l/t/r/b (tuiles), pos x/z (units)} ; modèles 559+type,
    // flags owned FLAG_VILLA_FURNITURE_START+type (2461), scripts 0x15+type.
    struct VillaFurn { int8_t t, s, l, tp, r, b; float x, z; };
    static constexpr VillaFurn VILLA_FURN[23] = {
        { 0, 1, 11, 5, 13, 8, 200.f, 112.f},   // TABLE
        { 1, 1, 14, 5, 15, 8, 240.f, 112.f},   // BIG_SOFA
        { 2, 1, 11, 9, 13,10, 216.f, 168.f},   // SMALL_SOFA
        { 3, 1, 18, 6, 20, 9, 296.f, 128.f},   // BED
        { 4, 1, 17, 6, 17, 7, 328.f, 120.f},   // NIGHT_TABLE
        { 5, 1, 11, 3, 12, 3, 192.f,  56.f},   // TV -> script 10100 face nord
        { 6, 1, 13, 3, 15, 3, 232.f,  56.f},   // AUDIO_SYSTEM
        { 7, 1, 16, 3, 17, 3, 272.f,  56.f},   // BOOKSHELF
        { 8, 1, 18, 3, 19, 3, 304.f,  56.f},   // RACK
        { 9, 1,  1, 3,  1, 3,  24.f,  56.f},   // HOUSEPLANT x4
        { 9, 1, 20, 3, 20, 3, 328.f,  56.f},
        { 9, 1,  1,11,  1,11,  24.f, 184.f},
        { 9, 1, 20,11, 20,11, 328.f, 184.f},
        {10, 1,  1, 7,  2, 9,  32.f, 136.f},   // PC_DESK
        {11, 0,  0, 0,  0, 0, 296.f,  56.f},   // MUSIC_BOX (déco)
        {12, 1,  2, 3,  2, 3,  40.f,  56.f},   // POKEMON_BUST
        {13, 1,  6, 3,  6, 3, 104.f,  56.f},   // POKEMON_BUST_SILVER
        {14, 1,  1, 4,  2, 6,  40.f,  88.f},   // PIANO
        {15, 1,  4, 9,  7,10,  96.f, 160.f},   // GUEST_SET
        {16, 0,  0, 0,  0, 0, 184.f,  24.f},   // WALL_CLOCK (déco)
        {17, 0,  0, 0,  0, 0, 136.f,  24.f},   // MASTERPIECE (déco)
        {18, 0,  0, 0,  0, 0, 104.f, 168.f},   // TEA_SET (déco)
        {19, 0,  0, 0,  0, 0, 120.f, 104.f},   // CHANDELIER (déco)
    };

    struct GymDyn {
        int kind = 0;          // 0 inactif, 1 Pastoria, 2 Sunyshore, 3 Eterna
        int plate_h = 0;       // hauteur dynamique courante (0/32/64)
        int target = 0;        // hauteur cible du déplacement en cours
        int water_prop = -1;   // index _props de la nappe (modèle 242)
        float wait = 0.f;      // délai avant déplacement (anim boutons, sec)
        int dir = 0;           // sens du mouvement (-1/+1)
        // Sunyshore : engrenages rotatifs (gym_features.c DS)
        int room = -1;         // salle 0/1/2
        int rot = 0;           // état de rotation 0..3 (collision immédiate)
        int gears[13];         // index _props des engrenages (-1 = libre)
        int num_gears = 0;
        float gear_prog = 0.f; // progression rotation (degrés, 0..goal)
        float gear_goal = 0.f; // 90 ou 180
        int gear_sign = 1;     // +1 normal/double, -1 reverse
        // Eterna : pendule florale (3 = actif ; état dans _eterna_clock)
        int et_hands[2] = {-1,-1};  // props aiguilles (0=heure 537, 1=min 536)
        int et_fount[8];            // jets fontaines (0-3 gauche, 4-7 droite)
        float et_hh = 0, et_mm = 0; // temps logique courant (heure/minute)
        float et_dh = 0, et_dm = 0; // destination cinématique
        bool et_roth = false;       // rotateHourHand (fraction min -> heure)
        int et_phase = 0;           // phase cinématique (0 = idle)
        int et_next = 0;            // phase à enchaîner après travelling
        float et_t = 0;             // timer interne de phase
        float et_cx = 0, et_cz = 0; // caméra (tuiles), et_tx/tz = cible
        float et_tx = 0, et_tz = 0;
        int et_xfirst = 0;          // axe le plus court d'abord (DS)
        int et_drain = -1;          // côté en vidange (0 gauche / 1 droite)
        // Canalave : plateformes mobiles (gym_features.c, kind=4)
        int  cn_plat[24];           // indices _props des plateformes
        int  cn_flr[3];             // indices _props floor2/3/4
        uint8_t cn_pos[24][3];      // tuile logique courante {x, haut, z}
        uint8_t cn_fl[24];          // platformCurrentFloors DS
        uint32_t cn_inB = 0;        // bitmask : plateforme en position B
        int  cn_move = -1;          // plateforme en trajet (-1)
        int  cn_dir = 0;            // 0 up 1 down 2 n 3 s 4 e 5 o (DS)
        float cn_dest = 0.f;        // destination monde sur l'axe
        // Hearthome : gym obscur (gym_features.c, kind=5). Le buffer DS est
        // remis à zéro par InitWithID à chaque entrée -> idem ici via le reset
        // de _gym au chargement : énigme régénérée à chaque visite.
        int  hh_door = 8;           // correctDoorID (8 = EXIT : pas d'énigme)
        int  hh_cluex = -1, hh_cluez = -1;  // tuile de l'indice
        int  hh_clue = -1;          // prop indice (fldeff 116+doorID)
        int  hh_lt[9];              // props cônes (0=joueur, 1-8 dresseurs)
        int  hh_lt_obj[9];          // -1 = joueur, sinon index _objects
        int  hh_lt_st[9];           // portée cible (data[0] ; joueur = 2)
        float hh_lt_p[9][3];        // offset pos courant (lerp DS)
        float hh_lt_s[9][3];        // scale courant
        int  hh_nlt = 0;
        int  hh_lift = -1;          // prop ascenseur (modèle 258)
        float hh_lift_y = 0.f;      // hauteur courante -> _ground_y
        // empreinte tuile du lift (x0..x1, z0..z1) — plaque dynamique 0
        int  hh_lift_tx0 = -1, hh_lift_tz0 = 0, hh_lift_tx1 = 0, hh_lift_tz1 = 0;
        int  hh_lift_move = 0;      // +1 monte / -1 descend / 0 arrêt
        float hh_lift_dest = 0.f;
        // platform_lift.c : ascenseurs Iron Island / Ligue Pokémon (kind=6).
        // floorID recalculé à chaque init depuis le warp d'entrée (location->z
        // DS) — pas besoin de persistance : revenir par le haut replace le
        // lift en haut, par le bas en bas.
        int  pl_lift = -1;          // prop lift (258 iron / 502 ligue)
        int  pl_floor = 0;          // floorID DS (0=bas, 1=haut)
        float pl_y = 0.f;           // hauteur courante (plaque dynamique 0)
        float pl_y0 = 0.f, pl_y1 = 0.f; // floorHeights {bas, haut}
        int  pl_tx = 0, pl_tz = 0;  // coin de la plaque (3x2 tuiles)
        int  pl_move = 0;           // +1 monte / -1 descend / 0 arrêt
        float pl_dest = 0.f;
        int  pl_kind = 0;           // 0 iron (+1 FX32/f), 1 ligue (+2)
        bool pl_nodown = false;     // preventGoingDown (Ligue : sens unique)
        bool pl_notused = false;    // notUsedWhenEnteredMap -> opcode 0x25D
        // great_marsh_tram.c : tram Safari (kind=7). _gym survit aux
        // transitions de zone (pas de _unload_all) -> location persiste
        // entre les maps du Marais ; entrée fraîche : loc = AREA_5_6.
        int  gm_tram = -1;          // prop great_marsh_train (modèle 475)
        int  gm_loc = 2;            // GREAT_MARSH_TRAM_LOCATION_* (0..2)
        int  gm_task = 0;           // 0=idle 1=CALL 2=RIDE attente SFX 3=RIDE
        int  gm_pend = -1;          // déplacement en file (type CALL/RIDE, DS InitCall)
        float gm_pdz = 0.f;         //   valeurs calculées à l'appel (destPos DS)
        int  gm_pslow = 0;          //   slowDownZ en file
        bool gm_psouth = false;     //   isMovingSouth en file
        bool gm_south = false;      // sens du déplacement (DS isMovingSouth)
        float gm_destz = 0.f;       // z cible du tram (destPos DS)
        int  gm_slowz = 0;          // tuile de ralentissement (slowDownZ)
        int  gm_lvl = 0;            // speedLevel DS
        float gm_fr = 0.f;          // ticks écoulés au niveau (CALL, ~30 Hz)
        GymDyn() { for (auto& g : gears) g = -1;
                   for (auto& f : et_fount) f = -1;
                   for (auto& p : cn_plat) p = -1;
                   for (auto& f : cn_flr) f = -1;
                   for (auto& l : hh_lt) l = -1;
                   for (auto& o : hh_lt_obj) o = -2; }
    };
    GymDyn _gym;

    // gym Veilstone (overlay008/gym_features.c) : 9 sacs de frappe et
    // 11 piles de pneus ; les sacs glissent au contact selon la table
    // de comportements 32x32 et renversent les pneus sur leur passage.
    struct GymThing { int x = 0, z = 0; bool exists = true; int obj = -1; };
    GymThing _gbags[9], _gstacks[11];
    bool _gym_vl = false;
    struct { int bag = -1, dir = 0, stack = -1;
             float tx = 0, tz = 0, sx = 0, sz = 0, t = 0; } _gbanim;
    int  _gvl_flags(int x, int z, int dir) const;
    int  _gvl_bag_obj(int oi) const;          // index objet -> index sac
    void _gvl_init();
    bool _gvl_kick(int oi, int dir);
    void _gvl_tick(float dt);

    // ov9_02249960.c : Monde Distorsion (Giratina). Coordonnées des
    // templates = espace partagé ; l'offset de l'étage (mapinfo tw_arc
    // member 0) les ramène en tuiles locales : world = (t-off)*16 + 8.
    struct DwBounds { int16_t x, y, z, sx, sy, sz; };
    static bool _dw_inb(int tx, int ty, int tz, const DwBounds& b) {
        return ty >= b.y && ty <= b.y + b.sy && tz >= b.z && tz <= b.z + b.sz
            && tx >= b.x && tx <= b.x + b.sx;
    }
    struct DwFP   { int16_t kind; uint16_t attr; DwBounds b; uint16_t tcv, tch; };
    struct DwJump { uint16_t h; int16_t dir; DwBounds b;
                    int16_t dx, dy, dz, rot, steps; uint16_t axis, inv;
                    int16_t fdir, kind; uint16_t fp; };
    struct DwCam  { DwBounds b; uint16_t ax, ay, az; int16_t dir; int32_t steps; };
    struct DwGhP  { uint32_t grp; uint16_t kind; int16_t tx, ty, tz; int prop;
                    // animateur DS (platform/obstacle prop) : opacité 0-31,
                    // bob fx32 ping-pong, frame du clip joint (obstacles)
                    float opac = 0.f, prog = 0.f, delta = 0.f, aframe = 0.f;
                    bool vis = false; Vec3 base{0,0,0}; };
    struct DwGhT  { uint32_t grp; int16_t dir, show; DwBounds b; };
    struct DwObj  { uint16_t hdr, id, gfx, scr; uint8_t dir, cond;
                    uint16_t cval; int16_t x, z, ty; uint16_t hflag; };
    struct DwFile {
        int hdr = -1;
        std::vector<DwFP> fp;
        std::vector<DwJump> jump;
        std::vector<DwCam> cam;
        uint32_t gh_def = 0;
        std::vector<DwGhP> gh;
        std::vector<DwGhT> gtr;
    };
    struct DwSys {
        bool on = false;            // header 573-583 actif
        int hdr = -1;
        bool valid = false;         // persistedData DS (0x2F2 = reset)
        uint32_t hidden_gh = 0;     // groupes fantômes cachés (24 bits)
        int cur_fp = 0;             // currentFloatingPlatformIndex persisté
        uint16_t cax = 0, cay = 0, caz = 0;   // angles caméra persistés
        uint16_t plat_flags = 0;    // movingPlatformFlags DS
        uint32_t puzz_flags = 0;    // boulderPuzzleFlags DS
        std::shared_ptr<Narc> tw_arc, tw_attr;
        struct Info { uint32_t hdr; uint16_t fidx; int16_t ox, oa, oz; };
        std::vector<Info> infos;
        DwFile cur, ina;            // fichier map actif + étage inactif
        int ina_hdr = -1;
        // caméra (cameraMan DS) — unités u16 : 0x10000 = 360°
        int base[3] = {-10750, 0, 0};
        int cang[3] = {0, 0, 0};    // currentAngle (cur = fichier DwFile)
        int tgt[3] = {0, 0, 0};     // targetAngle
        bool tr = false; float tr_left = 0.f;
        float tr_val[3] = {0,0,0}, tr_step[3] = {0,0,0};
        // plateforme flottante courante (floatingPlatformMan DS)
        int fp_kind = -1;           // kind de fp[cur_fp] (-1 = invalide)
        std::vector<uint16_t> fp_attr;  // grille 32x32 tw_arc_attr
        int persist_id = -1;        // PersistedMapFeatures.id DS (9 = DW)
        // déplacement en cours (jump point -> tâche de saut)
        float jump_t = 0.f;         // ticks restants (60 Hz)
        Vec3 jump_from{0,0,0}, jump_to{0,0,0};
        int jump_steps = 0, jump_axis = 0, jump_fdir = 0;
        float jump_prog = 0.f;
        // étage inactif (toujours nextID DS) : sa matrice + ses cellules
        // rendues décalées du delta d'espace partagé (~-512px sous l'actif)
        Nds::Matrix ina_mtx;
        std::vector<std::shared_ptr<Cell>> ina_cells;
        // plateformes mobiles / élévateurs (sMovingPlatformMapTemplates DS)
        // tx/ty/tz = tuiles partagées ; dir 0=UP 1=DOWN 2=NONE ;
        // pflag = bit plat_flags (0xFF = invalide) ; path = chemin courant
        struct DwPlat { uint16_t hdr; uint8_t idx, dir, dest, kind, pflag,
                        path; int16_t tx, ty, tz; int prop = -1;
                        float stones = -1.f;    // fondu pierres Giratina (0-62)
                        Vec3 pos{0,0,0};
                        // empreinte marchable du prop (bbox xz espace
                        // modèle, px) — sa surface fait office de sol sur
                        // toute la zone attr'd qu'il recouvre
                        Vec3 bmn{0,0,0}, bmx{0,0,0}; };
        std::vector<DwPlat> plats;
        float plat_pend = 0.f;          // retry atterrissage sur élévateur
        int elev_skip = 0;              // atterrissage post-trajet : pas d'embarquement
        // trajet élévateur (DistWorldElevatorPlatform DS)
        int elev_st = 0;            // 0 off 1 vibre 2 avt changeMaps 3 après
        int elev_p = -1;            // index plats[]
        int elev_dir = 0;
        int elev_path = 0, elev_next = -1;
        int elev_set = -1, elev_clr = -1;   // flags appliqués au changeMaps
        int elev_dest = -1;                 // header destination
        Vec3 elev_off{0,0,0}, elev_fin{0,0,0}, elev_chg{0,0,0};
        Vec3 elev_dlt{0,0,0};               // px/frame (60Hz)
        float elev_vib = 0.f; int elev_vib_n = 0;
        bool elev_vd = false;               // vibration déjà jouée (legs chaînés)
        float elev_py0 = 0.f, elev_ly0 = 0.f;
        int elev_ftx = 0, elev_fty = 0, elev_ftz = 0;  // finalPlayerTile local
        int elev_pass = -1;                 // index _objects passager
        float elev_acc = 0.f;               // sous-pas 60 Hz du trajet
        // file d'events de tuile (sMapEvents DS)
        const void* ev_cmds = nullptr;      // DwEvCmd* courants
        int ev_ci = 0;
        // sous-état de la cmd en cours : MOVE_PLATFORM (ph 1=vibre 2=move)
        // ou JUMP joueur (ph=3, jump_t pilote)
        int ev_ph = 0;
        int ev_p = -1;              // index plats[] déplacée
        bool ev_mp = false;         // movePlayer
        Vec3 ev_off{0,0,0}, ev_fin{0,0,0}, ev_dlt{0,0,0};
        float ev_vib = 0.f; int ev_vib_n = 0;
        float ev_py0 = 0.f, ev_ly0 = 0.f;
        bool ev_wait_scr = false;   // attente fin de script (START_SCRIPT)
        float ev_acc = 0.f;         // sous-pas 60 Hz de la file
        int ev_c[4] = {0, 0, 0, 0}; // params cascade (mapLoad/finFix/rot)
        // pos d'atterrissage B6F des rochers tombés de B5F (tuiles
        // partagées x/z par id-128 ; -1 = template) — DistWorld conserve
        // la position du rocher quand il change de zone.
        int bld_pos[3][2] = {{-1,-1},{-1,-1},{-1,-1}};
        // ciel DS (tw_arc_etc.narc) : fond BG2 tuilé 256px + 9 nuages en
        // orbite (InitSkyBackground / InitSkyCloudAnimators ov9)
        bool sky_ok = false;
        float sky_dark = 0.f;               // niveau 0-12 (hauteur joueur)
        int sky_kind = 0;                   // 0 normal 1 gira-room 2 dark
        struct SkyC { int set, grp; float cx, cy, rad, spd, off, scl, ang,
                      delta; };
        std::vector<SkyC> sky_cl;
        GpuMesh sky_bg_m, sky_cm[7];
        bool sky_ct[7] = {};
        // roll sprite cumulatif DS (RotateMapObject : rotate(rot,steps) au
        // saut, avance deg/steps par frame) — pas un snap par surface
        float roll_cur = 0.f, roll_delta = 0.f, roll_steps = 0;
        // ombre Giratina (show-giratina-shadow DS) : prop fldeff 144 +
        // BTA 198 qui glisse de step*left frames à 60Hz ; sfx joué au
        // premier tick ; la commande d'event attend la fin (state 0->2)
        int sh_state = 0;               // 0 inactif 1 en cours 2 terminé
        int sh_prop = -1, sh_sfx = -1;
        float sh_left = 0;              // frames restantes (s60 fractionnaire)
        Vec3 sh_step{0,0,0};
        // arrivée Giratina (play-giratina-arrival DS) : l'objet descend
        // de 160px pendant que le ciel s'assombrit à 12, puis sa palette
        // s'éclaircit de 16 à 0 ; ph 1=descend 2=palette 3=attente
        int arr_obj = -1, arr_ph = 0;
        float arr_wait = 0.f, arr_lift = 0.f, arr_dark = 0.f,
              arr_pal = 0.f;
        // marche Cyrus B4F (ELEVATOR_PLATFORM_STATE_CYRUS_B4F DS) : à
        // l'atterrissage de l'ascenseur il marche est*(x-88) tuiles puis
        // nord*4 à 1px/frame, disparaît et CYRUS_APPEARANCE passe à 1
        int cy_obj = -1, cy_leg = 0, cy_east = 0;
        float cy_left = 0.f;
        // plateformes salle Giratina = groupes fantômes 1-3 (ov9) :
        // SHOW délai 36 puis grp 1->3 toutes les 48 fr (+ stop SYUWA3) ;
        // HIDE délai 16, grp 3->1 /48 fr puis attente 8. st 0/1/2/3.
        int gp_st = 0, gp_grp = 0; float gp_delay = 0.f;
        bool gp_done = false;
        // SetSkyBackgroundDarknessLevel DS : >= 0 remplace le calcul
        // d'assombrissement par hauteur (arrivée Giratina)
        int sky_fix = -1;
        // caches d'anims props (membres fldeff) : BCA joints / BTA matériau
        std::unordered_map<int, bmd::JntAnim> dw_bca;
        std::unordered_map<int, std::vector<bmd::TexAnim>> dw_bta;
        const Info* info(int h) const {
            for (auto& i : infos) if ((int)i.hdr == h) return &i;
            return nullptr;
        }
    };
    DwSys _dw;
    void _dw_init(int hdr);              // DynamicMapFeaturesInit DS
    void _dw_free();                     // DynamicMapFeaturesFree DS
    void _dw_load_file(DwFile& f, int hdr);   // parse tw_arc member
    void _dw_spawn_ghosts(DwFile& f);    // props fantômes (fldeff 124+kind)
    void _dw_ghost_apply(DwFile& f);     // hidden_gh -> visibilité props
    void _dw_ghost_tick(DwFile& f, float s60); // anim bob/fondu/clip ov9
    void _dw_spawn_simps();              // sSimplePropsMapTemplates ov9
    void _dw_anim_attach(PropInst& pi, int kind); // BCA/BTA par kind
    const bmd::JntAnim* _dw_bca_clip(int kind);
    const std::vector<bmd::TexAnim>* _dw_bta_anims(int kind);
    void _dw_objs_init();                // AddMapObjectsForMap DS
    int  _dw_obj_spawn(const DwObj& o);  // MapObjectMan_AddMapObject
    void _dw_obj_add(int local_id);      // AddDistortionWorldMapObject
    void _dw_obj_del(int local_id);      // DeleteDistortionWorldMapObject
    void _dw_moved(int dir);             // HandlePlayerMoved DS
    void _dw_pos_changed();              // HandlePlayerPositionChanged DS
    void _dw_tick(float dt);             // transitions caméra + plateformes
    bool _dw_cam(Vec3& eye, Vec3& tgt, Vec3& up) const; // override caméra
    void _dw_cam_apply(const DwCam& t);  // DoCameraTransition DS
    int  _dw_shared_x() const;           // pos joueur en tuiles partagées
    int  _dw_shared_y() const;
    int  _dw_shared_z() const;
    uint16_t _dw_fp_attr(int tx, int ty, int tz) const; // attrs plateforme
    void _dw_fp_load();                  // grille tw_arc_attr -> fp_attr
    bool _dw_jump_start(const DwJump& j);// CreateJumpOnFloatingPlatformTask
    bool _dw_jump_press(int wdir);          // trigger saut à la pression DS
    Vec3 _dw_world(const DwSys::Info* i, int tx, int ty, int tz) const;
    void _dw_spawn_one(DwFile& f, DwGhP& g); // prop fantôme unitaire
    const DwObj* _dw_objs_tab(int& n) const; // table sMapObjectEvents DS
    bool _dw_obj_cond(const DwObj& o) const; // FlagCondition DS
    void _dw_sky_load();                 // tw_arc_etc -> textures + quads
    void _dw_sky_draw();                 // fond BG2 + nuages derrière la map
    void _dw_shadow_start(int tpl);      // LoadGiratinaShadowPropAnimation DS
    void _dw_shadow_stop();              // FinishGiratinaShadowPropRenderer DS
    void _dw_shadow_tick(float s60);     // glisse + sfx par frame
    void _dw_arrival_tick(float s60);    // play-giratina-arrival DS
    void _dw_load_inactive();                // étage next (cells+props+gh)
    void _dw_plats_spawn();                  // InitMovingPlatformPropsForMap DS
    void _dw_plat_spawn_one(int hdr, int idx); // InitSpecific..ForMap DS
    int  _dw_plat_at(int tx, int ty, int tz) const; // Find..At DS (actif)
    void _dw_plat_fp(DwSys::DwPlat& p, const PropInst& pi); // empreinte prop
    bool _dw_fp_has(const DwSys::DwPlat& p, float wx, float wz) const;
    float _dw_plat_surf(const DwSys::DwPlat& p) const; // hauteur du plancher
    int  _dw_plat_on(float y) const;     // plat sous le joueur (x,z+portée)
    float _dw_plat_ground(int tx, int tz, float from) const; // sol plat.
    bool _dw_plat_tile(int tx, int tz, float y) const;  // tuile franchissable
    void _dw_elev_begin(int pi);             // CreateElevatorPlatformHandlerTask
    void _dw_elev_tick();                    // machine d'état du trajet
    void _dw_elev_leg();                     // BeginMovement DS (par leg)
    void _dw_floor_change(int dir);          // LoadFloor NEXT/PREVIOUS
    bool _dw_ev_at(int tx, int ty, int tz);  // HandleEventAt DS
    bool _dw_ev_cond(int fc, uint16_t fv) const; // FlagCondition d'event
    void _dw_ev_tick();                      // file de commandes d'event
    void _dw_ev_plat_move(int plat, bool mp, int x, int y, int z,
                          int dx, int dy, int dz); // MOVE_PLATFORM
    void _dw_ev_jump(int dir);               // JUMP_DISTORTION_WORLD_*
    void _dw_ev_casc(int dir, int fin_px, int load_t, int fix_t,
                     int dl, int rot);       // CASCADE_UP/DOWN
    bool _dw_busy() const {                  // trajet/event en cours
        return _dw.elev_st > 0 || _dw.ev_cmds != nullptr || _dw.jump_t > 0;
    }

    // Poussée Force (ov5_021DFF1C DS) + chutes de rochers du Monde
    // Distorsion (DistWorldFallingBoulder_* DS).
    struct BoulderPush {
        int obj = -1;               // index _objects du rocher (-1 = off)
        int dir = 0;                // dir monde de la poussée
        float t = 0.f;              // progression glisse (16 ticks DS)
        Vec3 from{0,0,0}, to{0,0,0};
        int fall = 0;   // 0 pas de chute, 1 trou B5F->B6F, 2 puits juste,
                        // 3 mauvais puits (déjà rempli ou non numéroté)
        int bid = 0;    // localId d'origine du rocher (avant swap -> puits)
        int ph = 0;     // 0 tipOver, 1 secousse/chute libre, 2 attente script
        int pit = -1;               // bit puits 6/7/8 pour fall==2
        float tm = 0.f;             // temps de phase en ticks @30Hz
        float base_y = 0.f;         // y du rebord (référence tipOver/secousse)
    };
    BoulderPush _bpush;
    void _boulder_tick(float dt);              // glisse + chutes (tické toujours)
    int  _dw_fall_at(int stx, int stz, int* pit) const; // 0 non 1 trou 2 puits 3 mauvais

    int _eterna_clock = 0;          // état pendule (persisté save v13)
    void _eterna_hands();           // rotations aiguilles selon et_hh/mm
    int  _fount_spawn(int tx);      // jet de fontaine z=19 (fldeff 111)
    void _cana_vis(float py);       // visibilité étages/plateformes Canalave
    int  _gym_op(int sub, int arg);      // opcodes script 0x16F/0x170…
    int  _hh_door_dir(int tx, int tz);   // porte incorrecte -> sud (DS)
    void _gm_start(float dz, int slow, bool south, int type); // MoveTram DS
    void _gym_buttons(int pressed, bool animate);  // état visuel boutons
    void _gym_tick(float dt);            // déplacement de la nappe
    int  _spawn_prop(int model, const Vec3& pos);  // prop runtime (nappe)
    PropInst _prop_make(const LandProp& lp, Vec3 org, int64_t cell,
        std::vector<Vec3>* ct_world);     // corps par-prop de _inst_props

    // joueur — mouvement DS : pas de 1 tuile (player_move.c / unk_020655F4.c)
    // WalkNormal 2px/tick(30Hz)=8 ticks, Run 4px/tick=4 ticks, Turn 2 ticks
    Vec3 _ppos{0, 40, 0}, _vel{0, 0, 0};
    Vec3 cam_pos() const {                     // cible caméra : override ou joueur
        if (_cam_mode == 3)
            return {_ppos.x + _cam_ox, _ppos.y + _cam_oy, _ppos.z + _cam_oz};
        if (_cam_mode) return {_cam_x * TILE + TILE / 2.f, _ppos.y,
                               _cam_z * TILE + TILE / 2.f};
        return _ppos;
    }
    bool _ground = false;
    int _warp_cd_x = -9999, _warp_cd_z = -9999;   // tuile warp du spawn
    int _pdir = 0;                              // direction 0-3 (frame mmodel)
    int _pface = -1;                            // facing dessiné Distorsion (-1 = _pdir)
    bool _psurf = false;                        // avatar surf (PLAYER_AVATAR_SURFING)
    int _repel = 0;                             // pas restants (Repousse DS)
    int _repel_item = 0;                        // dernier repousse utilisé
    bool _repel_ask = false;                    // repousse vient d'expirer
    float _panim = 0.f;                         // cycle marche
    int _pstep = 0;                             // 0=NONE 1=TURNING 2=MOVING 3=BUMP
    float _pstep_t = 0;                         // durée restante (turn/bump)
    int   _pjump = 0;                           // saut de rebord en cours
    int   _pslide = 0;                          // glissière glace en cours (dir forcée)
    float _ptx = 0, _ptz = 0;                   // cible du pas en cours
    float _pty = 0;                             // cible y du pas (murs Distorsion)
    float _movdbg_t = 0;                        // debug dérive joueur
    float _pstep_spd = 0;                       // vitesse du pas (px/s)
    // tâches CS terrain (FieldTask_InitCall DS : la VM finit, la tâche
    // pilote _ppos/_pstep dans le tick ; input gelé tant que _phm != 0).
    // escalade : 1=saut sur la paroi 2=marche auto 3=saut de sortie ;
    // cascade : 4/5 = montée (y puis y+z), 6/7 = descente (z puis y+z).
    int   _phm = 0;
    float _phm_t = 0;                           // frames DS écoulées (cascade)
    Vec3  _phm_d{}, _phm_to{};                  // delta/frame + cible (cascade)
    void _climb_next();                         // escalade : enchaîne le mouvement
    void _wfall_start(bool up);                 // cascade : init montée/descente
    // cut-in CS (HMCutIn_StartTask DS) : slot équipe, espèce résolue, temps
    // en frames (60fps), cri joué. -1 = inactif. Dessiné par main.cpp.
    int   _cutin_slot = -1, _cutin_sp = 0;
    float _cutin_f = 0.f;
    bool  _cutin_cry = false;
    // tâche dédiée lancée après le cut-in (CS sans script DS) :
    // 1 = Téléportation, 2 = Tunnel, 3 = Dolce-Parfum
    int   _cutin_after = 0;
    void _fm_warp(bool teleport);               // FieldWarp Dig/Teleport DS
    void _sweet_scent();                        // rencontre sauvage forcée
    GpuMesh _pbox, _npcbox, _npcbox2, _bb, _bb16, _btree_bb;
    // membre*64+frame -> (tint_key -> tex GL) : textures overworld mises
    // en cache par variante de recoloration
    std::unordered_map<int, std::unordered_map<uint64_t, uint32_t>> _mmcache;
    std::string _skindir;                      // dossier skins/ (scan_skins)
    std::vector<SkinSheet> _skins;             // feuilles PNG chargées
    bool _skins_scanned = false;
    std::unordered_map<int, std::unordered_map<uint64_t, uint32_t>> _skcache;
    std::unordered_map<int, std::unordered_map<uint64_t, uint32_t>> _bskcache; // composites skin+vélo
    uint32_t _skinpng_tex(int i, int frame, const SkinTint& t);
    std::unordered_map<int, std::vector<RGBAf>> _palcache;   // slots palette

    void _load_cell(int cx, int cy);
    std::shared_ptr<Cell> _mk_cell(const Nds::Matrix& mtx, int cx, int cy,
                                   Vec3 off);   // corps commun (off monde)
    void _unload_cell(int64_t k);               // libère meshes + props d'une cellule
    void _inst_props(Cell* c);                  // instancie les props d'une cellule
    void _finish_load(int header_id, int dest_warp); // spawn + logs + init
    void _set_header(int header_id, bool init); // events+scripts+PNJ d'un header
    void _music_update();                     // applique BGM voulue au moteur
    void _run_init();                           // scripts d'init (4,2,3)
    void _stream();                             // cellules + header autour du joueur
    void _unload_all();
    std::vector<float> _tris(const bmd::MeshGroup& mg, Vec3& colout,
        std::vector<Vec3>* coltris);
    uint32_t _mat_tex(const bmd::MatDef& md,
        const std::unordered_map<std::string, bmd::TexEnt>& texs,
        bool* alpha, int* polyA);
    uint32_t _mm_tex(int gfx, int frame);                       // sprite overworld
    uint32_t _mm_tex(int gfx, int frame, const SkinTint& t);    // idem retouché
    uint32_t _mm_member_tex(int member, int frame);             // mmodel direct
    uint32_t _mm_member_tex(int member, int frame, const SkinTint& t);
    SkinTint _tint() const {
        return {skin_hue, skin_sat, skin_val, skin_mask};
    }
    uint32_t _skin_tex(int pf);                  // skin PNJ : frames de repli
    uint32_t _bike_skin_tex(int i, int pf, int bikeFrame); // skin + vélo DS dessous
    static bool _surfable(int beh);             // TILE_BEHAVIOR_FLAG_SURFABLE
    bool _mon_learns_hm(const ScriptVM::PartyMon& m, int hm) const; // compat CS
    bool _party_can_surf() const;               // un Pokémon sait/peut Surf
    void _draw_billboard(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
        const Vec3& cam, const Mat4& mvp);
    void _draw_billboard(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
        const Vec3& cam, const Mat4& mvp, float pull, float roll = 0.f);
    void _sprite_shadow(const GpuMesh& bb, uint32_t tex, const Vec3& pos,
        float gy, const Vec3& sd, const Vec3& cam, const Mat4& mvp);
    bool _blocked(const Vec3& p) const;
    int  _tile_beh(int tx, int tz) const;    // byte bas de l'attr (behavior DS)
    float _ground_y(const Vec3& p, float from) const;
    // météo dynamique : la tendance vient du byte wthr du header (résolu par
    // date pour les maps « yearly ») ; un état visé + une intensité continue
    // pilotent un mesh de particules reconstruit chaque frame.
    struct Weather {
        int  type = 0, target = 0;          // type rendu / type visé
        float intensity = 0.f;              // montée/descente douce 0..1
        float phase_t = 12.f;               // réévaluation du champ (dyn)
        float t = 0.f;                      // horloge particules
        float front_t = 0.f;                // dérive du front (init horloge)
        float flash = 0.f, flash_t = 6.f;   // éclairs (orage)
        float kami_t = -1.f;                // tonnerre retardé (s, -1 rien)
        int   amb = -1;                     // séquence ambient en cours (-1)
        int   nami_seq = -1;                // boucle vagues (SE_DP_NAMI, -1 off)
        int   amb_cut = -1;                 // boucle coupée par un pas (à rééditer)
        bool  nami = false;                 // eau surfable à portée du joueur
        float wet = 0.f;                    // sol mouillé post-pluie 0..1
        float snow_depth = 0.f;             // neige accumulée au sol 0..1
        bool  dust = false;                 // diamond dust (12 janv., nord)
    } _wthr;
    int _wthr_base = 0;                     // météo header résolue
    int _wthr_dbg_last = -1;                // debug : dernière cible loguée
    GpuMesh _wthr_mesh, _wthr_flash;
    GpuMesh _seas_mesh;                       // particules saisonnières texturées
    uint32_t _seas_tex = 0;                   // atlas feuilles/pétales (imageclip)
    GpuMesh _lamp_mesh;                       // halos des lampadaires (dyn)
    uint32_t _lamp_tex = 0;                   // sprite radial chaud généré
    std::vector<Vec3> _lamps;                 // sources light* vues cette frame
    void _draw_lamps(const Vec3& cam, const Vec3& tgt, const Mat4& mvp,
                     float nk);
    float _seas_uv[5][4] = {};                // u0 v0 u1 v1 par variante
    uint32_t _seas_atlas();                   // construit l'atlas (lazy)
    void _weather_tick(float dt);
    int  _weather_base(int w) const;        // header wthr -> type concret
    int  _weather_field() const;            // champ bruit -> type (fronts)
    int  _weather_band(float v, int base) const;  // champ -> type par climat
    float _band_margin(float v, int base) const;  // distance au bord de bande
    int  _weather_season(int t) const;      // conversion saisonnière
    int  _season() const;                   // 0 hiver..3 automne (date réelle)
    static bool _weather_variable(int t);   // climat soumis aux fronts ?
    static float _wfh(int i);               // hachage entier -> 0..1
    static float _wnoise(float x);          // value noise lissé
    void _draw_weather(const Vec3& cam, const Vec3& tgt, const Mat4& mvp);
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
