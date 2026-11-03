// battle.h — moteur de combat DS authentique (Gen4).
// Données : pl_personal (stats), wotbl (learnset), pl_waza_tbl (moves),
// trdata/trpoke (dresseurs), pl_pokegra (sprites via World::_poke_tex).
#pragma once
#include "common.h"
#include <string>
#include <vector>

class Nds;

struct BatMon {
    int species = 0, form = 0, level = 1;
    int hp = 1, maxhp = 1;
    int atk = 1, def = 1, spa = 1, spd = 1, spe = 1;
    int t1 = 0, t2 = 0;              // types DS (0=Normal…17=Dark)
    int moves[4] = {0, 0, 0, 0};
    int pp[4] = {0, 0, 0, 0};
    int nmoves = 0;
    uint32_t ivs = 0;                // IVs DS (MON_DATA_IVS) : 5 bits/stat
    uint32_t pid = 0, exp = 0;       // PID (nature/shiny) + points d'expérience
    uint8_t evs[6] = {0};            // EVs par stat (cap 255/510)
    int friendship = 70;             // amitié (évos bonheur)
    // ── état de combat (volatil, non persisté) ──────────────────────
    int8_t stg[8] = {0};             // modifs : atk,def,spa,spd,spe,pré,esq,crit
    uint32_t cond = 0;               // DS COND : bits0-2 sommeil, 0x08 psn,
                                     // 0x10 brûlure, 0x20 gel, 0x40 para, 0x80 tox
    uint8_t conf = 0;                // tours de confusion (2-5)
    bool flinch = false;             // flinch ce tour
    bool recharge = false;           // Hyper Beam : tour suivant perdu
    bool seeded = false;             // Vampigraine posée
    int toxic_n = 0;                 // compteur poison grave (n/16 max PV)
    uint8_t bound = 0;               // tours de ligotage (Ligotage/Siphon)
    bool prot = false, endure = false;   // Protection / Ténacité (ce tour)
    bool ingrain = false;            // Racines : régénère 1/16 par tour
    uint8_t drowsy = 0;              // Bâillement : dort dans N tours
    bool attract = false;            // Attraction : 50% immobilisé par amour
    int turns_out = 0;               // actions jouées (Fake Out)
    bool run_away = false;           // roamer : fuit dès que possible (DS)
    bool fainted() const { return hp <= 0; }
};

struct MoveInfo {                     // pl_waza_tbl : 16 octets
    int effect, cls, power, type, acc, pp, chance;
    int range = 0, prio = 0;          // u16 @8 (RANGE_*), s8 @10
};

// portées de move (move_ranges.txt de la ROM) — utile pour le ciblage
// en combat double
enum MoveRange {
    RANGE_SINGLE = 0,                 // 1 cible au choix (adversaire par défaut)
    RANGE_SINGLE_SPECIAL = 1,         // 1 cible forcée (Contre, etc.)
    RANGE_RANDOM_FOE = 2,             // 1 ennemi au hasard
    RANGE_ALL_FOES = 3,               // les 2 adversaires (Surf, Eboulement…)
    RANGE_ALL_ADJACENT = 4,           // tous sauf soi (Séisme, Explosion…)
    RANGE_USER = 5,                   // soi-même
    RANGE_USER_SIDE = 6,              // le camp du lanceur (Mur Lumière)
    RANGE_FIELD = 7,                  // tout le terrain (Danse Pluie…)
    RANGE_FOE_SIDE = 8,               // le camp adverse (Piège de Roc…)
    RANGE_ALLY = 9,                   // l'allié seulement (Pinceau…)
    RANGE_USER_OR_ALLY = 10,          // soi ou allié au choix (Aromathérapie…)
    RANGE_ME_FIRST = 11,              // Me First : 1 adversaire
    RANGE_ALL = 16,                   // tout le monde (Distorsion…)
};

// météo de combat (Gen4) : reportée de l'overworld au lancement du combat
enum BW { BW_NONE, BW_RAIN, BW_SAND, BW_HAIL, BW_FOG };

class Battle {
public:
    bool active = false;
    bool trainer = false;             // combat dresseur vs sauvage
    int tutorial = 0;                 // tuto capture (script 0x0126)
    int tid = 0;                      // trainerID (dresseur)
    std::vector<BatMon> me, foes;     // équipes
    int mi = 0, fi = 0;               // actifs (slot 1 ; DS : gauche=1, droite=2)
    int mi2 = -1, fi2 = -1;           // actifs slot 2 (-1 = absent)
    bool dbl = false;                 // combat double 2v2
    enum St { TRANS, INTRO, CHOICE, PICKMOVE, PICKTGT, EXEC, BAG,
              THROW, WIN, LOSE, FLEE, DONE } st = TRANS;
    int sel = 0;                      // curseur (menu/moves)
    int pick = 0;                     // slot joueur en cours de commande (0/1)
    int tsel = 0;                     // curseur de cible (PICKTGT)
    int sel_mv[2] = {-1, -1};         // move choisi par slot (-1 fuite, -2 item, -3 Lutte, -4 switch)
    int sel_tg[2] = {-1, -1};         // cible choisie par slot (0..3 battler id)
    std::string msg;                  // texte en cours
    float msgt = 0.f;                 // timer texte
    float anim = 0.f;                 // timer animations
    uint32_t trans_tex = 0;           // capture du terrain pour le screen-slice
    int trans_w = 0, trans_h = 0;
    int queue[8][4];                  // file : {battler 0-3, cible 0-3, action, arg}
    int qn = 0, qi = 0;
    int eor = 0;                      // EXEC : résidus de fin de tour déjà passés
    int lastDmg = 0, lastEff = 100;   // dernier dégât/efficacité (x100)
    // ── animations de combat (rendu) ────────────────────────────────────
    int   atk_side = -1;              // battler en propulsion (0-3, -1 aucun)
    float atk_t = 0.f;                // 0→1 : va-et-vient vers la cible
    int   hit_side = -1;              // battler qui flash/tremble (0-3)
    float hit_t = 0.f;                // temps restant du flash
    float disp_hp[4] = {0.f, 0.f};    // PV affichés par battler (jauge animée)
    float faint_t[4] = {0.f, 0.f};    // KO : 0→1 glissade + fondu, par battler
    bool foe_down = false, me_down = false;
    int waitA = 0;                    // attend appui A
    int tries = 0;                    // tentatives de fuite (formule DS)
    int won = 0;                      // résultat : 1=victoire -1=défaite/fuite
    int bg = 0;                       // BACKGROUND_* (décor pl_batt_bg)
    int weather = 0;                  // BW_* : météo overworld au départ
    int terrain = 2;                  // TERRAIN_* (plateformes pl_batt_obj) : 2=herbe
    int tod = 0;                      // moment : 0 jour, 1 soir, 2 nuit
    int foe_class = 0, foe_sprite = -1; // trdata : classe + sprite du dresseur adverse
    int tid2 = 0;                     // 2e dresseur (combat 2vs2)
    int foe_sprite2 = -1;             // sprite du 2e dresseur
    int foe_split = 0;                // duo : index du 1er Pokémon du 2e dresseur
                                      // (réserve du slot E2 ; 0 = équipe commune)
    uint8_t me_part = 0;              // masque des Pokémon joueur ayant participé (XP)
    // capture (THROW) : ball = id objet, shakes restants, caught = capturé
    int turns = 0;                    // tours écoulés (Chrono Ball)
    int ball = 0, shakes = 0, shake_max = 0, tphase = 0;
    bool caught = false;
    int caught_box = -1;                        // >=0 : envoyé en boîte PC n
    int screen[2] = {0, 0};           // Protection/Mur Lumière : tours restants

    const BatMon& you() const { return me[mi]; }
    const BatMon& foe() const { return foes[fi]; }
    BatMon& you() { return me[mi]; }
    BatMon& foe() { return foes[fi]; }
    // ── combats doubles : battler ids DS (0=P1, 1=E1, 2=P2, 3=E2) ──────
    static int bid(int side, int slot) { return slot * 2 + side; }
    static int bid_side(int b) { return b & 1; }
    static int bid_slot(int b) { return b >> 1; }
    int act_idx(int side, int slot) const {          // index équipe du slot
        return slot ? (side ? fi2 : mi2) : (side ? fi : mi); }
    int act_bid(int side, int slot) const {          // battler id ou -1
        return act_idx(side, slot) >= 0 ? bid(side, slot) : -1; }
    BatMon* bmon(int b) {                            // Pokémon du battler
        int i = act_idx(bid_side(b), bid_slot(b));
        auto& v = bid_side(b) ? foes : me;
        return i >= 0 && i < (int)v.size() ? &v[i] : nullptr; }
    int foes_out() const {                           // ennemis vivants sortis
        int n = 0;
        for (int s = 0; s < 2; s++) {
            int i = act_idx(1, s);
            if (i >= 0 && i < (int)foes.size() && !foes[i].fainted()) n++;
        }
        return n;
    }
    int me_out() const {                             // joueurs vivants sortis
        int n = 0;
        for (int s = 0; s < 2; s++) {
            int i = act_idx(0, s);
            if (i >= 0 && i < (int)me.size() && !me[i].fainted()) n++;
        }
        return n;
    }
};

// IVs DS (MON_DATA_IVS) : u32 packé, 5 bits par stat
// ordre stats : PV, ATQ, DÉF, VIT, A.SP, D.SP (indices battle_stats)
constexpr uint32_t IVS_UNSET = 0xFFFFFFFFu;
static inline int iv_at(uint32_t ivs, int s) { return (int)((ivs >> (s * 5)) & 31); }
static inline uint32_t ivs_flat(int v) {      // même IV sur les 6 stats (dresseurs)
    uint32_t r = 0; for (int s = 0; s < 6; s++) r |= (uint32_t)(v & 31) << (s * 5);
    return r;
}

// construction d'un Pokémon : stats calculées (pl_personal), moves depuis
// learnset wotbl si non fournis (4 dernières attaques ≤ niveau)
BatMon battle_make(Nds* db, int species, int level, const uint16_t* moves = nullptr,
    uint32_t ivs = IVS_UNSET);
// équipe dresseur (trdata/trpoke)
std::vector<BatMon> battle_trainer_party(Nds* db, int tid);
// en-tête trdata[tid] : retourne {classe, sprite} (-1,-1 si hors bornes)
std::pair<int,int> battle_trainer_class_sprite(Nds* db, int tid);
// trdata[tid] @16 (u32) : battleType (bit 1 = DOUBLES, bit 3 = 2vs2)
int battle_trainer_battle_type(Nds* db, int tid);
// info move (pl_waza_tbl, 16B)
MoveInfo battle_move(Nds* db, int move);
void default_moves(Nds* db, int sp, int lv, int out[4], int* n);
void battle_stats(Nds* db, int sp, int lv, uint32_t ivs, const uint8_t evs[6],
                  uint32_t pid, int out[6]);   // 6 stats DS (hp/atk/def/spe/spa/spd)
void battle_types(Nds* db, int sp, int* t1, int* t2);   // types pl_personal
int  battle_ability(Nds* db, int sp, uint32_t pid);     // talent (pid&1)
// nom FR d'un move (banque 647)
std::string battle_move_name(Nds* db, int move);
// un round : lit sel_mv/sel_tg (joueur) + choix IA (ennemi), remplit la file.
// sel_mv : index de move, -1 fuite, -2 item (hors file), -3 Lutte, -4 switch.
void battle_round(Battle& b, Nds* db);
// dégâts Gen4 (battle_lib CalcMoveDamage simplifié : pas d'items/statuts)
int battle_damage(Nds* db, const BatMon& at, const BatMon& df,
    const MoveInfo& mv, int& eff_x100);
// multiplicateur de type en quarts (4=×1, 8=×2, 0=immun) — pour l'UI
int battle_type_mul(int atk_t, const BatMon& df);
// nom FR d'une espèce (banque 412)
std::string battle_species_name(Nds* db, int sp);
// lancer de Ball : formule capture Gen4 (battle_script.c) — retourne le
// nombre de secousses passées, 4 = capturé
int battle_catch(Nds* db, Battle& b, int item);
// XP Gen4 : floor(baseExp * niv.ennemi / 7), boosts floorés dans l'ordre
// (dresseur ×1.5 puis étranger ×1.5) — battle_script.c
int battle_exp_gain(Nds* db, const BatMon& foe, bool trainer, bool outsider);
// seuils d'exp par niveau : pl_growtbl[rate][lv-1]
uint32_t mon_exp_for(Nds* db, int species, int level);
int      mon_gender(Nds* db, const BatMon& m);         // 0=mâle 1=femelle -1=aucun
// recalcule les stats d'un Pokémon à son niveau (EVs + nature via pid%25)
void mon_recalc(Nds* db, BatMon& m);
// learnset wotbl : moves appris exactement au niveau lv
int mon_learnset_at(Nds* db, int sp, int lv, int out[8]);
// evo.narc : 7 × {u16 méthode, u16 param, u16 cible}. Trigger level-up :
// retourne la cible d'évolution ou 0 (méthodes niveau/bonheur/seuil/PID)
int mon_evolution_target(Nds* db, const BatMon& m, bool night);
// avance la machine à états ; key = code SDL d'une touche (0 = rien)
void battle_update(Battle& b, Nds* db, float dt, int key);
