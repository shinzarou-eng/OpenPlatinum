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
    int iv = 0;                      // IV unique du Pokémon (persistant)
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
    int turns_out = 0;               // actions jouées (Fake Out)
    bool fainted() const { return hp <= 0; }
};

struct MoveInfo {                     // pl_waza_tbl : 16 octets
    int effect, cls, power, type, acc, pp, chance;
};

class Battle {
public:
    bool active = false;
    bool trainer = false;             // combat dresseur vs sauvage
    int tid = 0;                      // trainerID (dresseur)
    std::vector<BatMon> me, foes;     // équipes
    int mi = 0, fi = 0;               // actifs
    enum St { TRANS, INTRO, CHOICE, PICKMOVE, EXEC, BAG, THROW, WIN, LOSE, FLEE, DONE } st = TRANS;
    int sel = 0;                      // curseur (menu/moves)
    std::string msg;                  // texte en cours
    float msgt = 0.f;                 // timer texte
    float anim = 0.f;                 // timer animations
    uint32_t trans_tex = 0;           // capture du terrain pour le screen-slice
    int trans_w = 0, trans_h = 0;
    int queue[8][4];                  // file d'actions : {ordre, att, def, move}
    int qn = 0, qi = 0;
    int lastDmg = 0, lastEff = 100;   // dernier dégât/efficacité (x100)
    // ── animations de combat (rendu) ────────────────────────────────────
    int   atk_side = -1;              // attaquant en propulsion (-1 = aucun)
    float atk_t = 0.f;                // 0→1 : va-et-vient vers la cible
    int   hit_side = -1;              // défenseur qui flash/tremble
    float hit_t = 0.f;                // temps restant du flash
    float disp_hp[2] = {0.f, 0.f};    // PV affichés (jauge animée)
    float faint_t[2] = {0.f, 0.f};    // KO : 0→1 glissade + fondu
    bool foe_down = false, me_down = false;
    int waitA = 0;                    // attend appui A
    int tries = 0;                    // tentatives de fuite (formule DS)
    int won = 0;                      // résultat : 1=victoire -1=défaite/fuite
    int bg = 0;                       // BACKGROUND_* (décor pl_batt_bg)
    int terrain = 2;                  // TERRAIN_* (plateformes pl_batt_obj) : 2=herbe
    int tod = 0;                      // moment : 0 jour, 1 soir, 2 nuit
    uint8_t me_part = 0;              // masque des Pokémon joueur ayant participé (XP)
    // capture (THROW) : ball = id objet, shakes restants, caught = capturé
    int turns = 0;                    // tours écoulés (Chrono Ball)
    int ball = 0, shakes = 0, shake_max = 0, tphase = 0;
    bool caught = false;
    int screen[2] = {0, 0};           // Protection/Mur Lumière : tours restants

    const BatMon& you() const { return me[mi]; }
    const BatMon& foe() const { return foes[fi]; }
    BatMon& you() { return me[mi]; }
    BatMon& foe() { return foes[fi]; }
};

// construction d'un Pokémon : stats calculées (pl_personal), moves depuis
// learnset wotbl si non fournis (4 dernières attaques ≤ niveau)
BatMon battle_make(Nds* db, int species, int level, const uint16_t* moves = nullptr,
    int iv = -1);
// équipe dresseur (trdata/trpoke)
std::vector<BatMon> battle_trainer_party(Nds* db, int tid);
// info move (pl_waza_tbl, 16B)
MoveInfo battle_move(Nds* db, int move);
// nom FR d'un move (banque 647)
std::string battle_move_name(Nds* db, int move);
// un round : mvP = move joueur (0-3) ou -1 fuite. file d'actions remplie.
void battle_round(Battle& b, Nds* db, int mvP);
// dégâts Gen4 (battle_lib CalcMoveDamage simplifié : pas d'items/statuts)
int battle_damage(Nds* db, const BatMon& at, const BatMon& df,
    const MoveInfo& mv, int& eff_x100);
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
// recalcule les stats d'un Pokémon à son niveau (EVs + nature via pid%25)
void mon_recalc(Nds* db, BatMon& m);
// learnset wotbl : moves appris exactement au niveau lv
int mon_learnset_at(Nds* db, int sp, int lv, int out[8]);
// evo.narc : 7 × {u16 méthode, u16 param, u16 cible}. Trigger level-up :
// retourne la cible d'évolution ou 0 (méthodes niveau/bonheur/seuil/PID)
int mon_evolution_target(Nds* db, const BatMon& m, bool night);
// avance la machine à états ; key = code SDL d'une touche (0 = rien)
void battle_update(Battle& b, Nds* db, float dt, int key);
