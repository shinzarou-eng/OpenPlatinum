// battle.cpp — combat Gen4 : stats pl_personal, moves wotbl/pl_waza_tbl,
// équipes trdata/trpoke, table des types + formule de dégâts DS.
#include "battle.h"
#include "banim.h"
#include "nds.h"
#include "msg.h"
#include "world.h"
#include <cstdlib>
#include <cstdio>

static const char* P_PERSONAL = "/poketool/personal/pl_personal.narc";
static const char* P_WOTBL    = "/poketool/personal/wotbl.narc";
static const char* P_WAZA     = "/poketool/waza/pl_waza_tbl.narc";
static const char* P_TRDATA   = "/poketool/trainer/trdata.narc";
static const char* P_TRPOKE   = "/poketool/trainer/trpoke.narc";

// ── stats d'espèce (SpeciesData, 44 octets) ────────────────────────────────────
// 0-5 stats, 6-7 types, 8 catchRate, 9 baseExp, 0x0A evYield(2b/stat),
// 0x12 amitié de base, 0x13 courbe XP, 0x16-17 talents, 0x1C-0x27 compat CT/CS
static bool personal(Nds* db, int sp, uint8_t st[6], int* t1, int* t2,
                     int* baseExp = nullptr, int* evY = nullptr,
                     int* grow = nullptr, int* frnd = nullptr) {
    auto n = db->narc(P_PERSONAL);
    if (!n || sp < 0 || sp >= n->count()) return false;
    const Bytes& d = n->member(sp);
    if (d.size() < 44) return false;
    for (int i = 0; i < 6; i++) st[i] = d[i];
    *t1 = d[6]; *t2 = d[7];
    if (baseExp) *baseExp = d[9];
    if (evY)     *evY = u16(d, 0x0A);
    if (grow)    *grow = d[0x13];
    if (frnd)    *frnd = d[0x12];
    return true;
}

// id du talent : pl_personal 0x16/0x17, choisi par le bit 0 du pid (DS)
int battle_ability(Nds* db, int sp, uint32_t pid) {
    auto n = db->narc(P_PERSONAL);
    if (!n || sp < 0 || sp >= n->count()) return 0;
    const Bytes& d = n->member(sp);
    if (d.size() < 0x18) return 0;
    int a1 = d[0x16], a2 = d[0x17];
    return a2 && (pid & 1) ? a2 : a1;
}

// natures : pid%25 -> {stat+ , stat-} (0=Atk,1=Def,2=Spe,3=SpA,4=SpD, -1 neutre)
static const int8_t NATURE[25][2] = {
    {-1,-1},{0,1},{0,2},{0,3},{0,4},
    {1,0},{-1,-1},{1,2},{1,3},{1,4},
    {2,0},{2,1},{-1,-1},{2,3},{2,4},
    {3,0},{3,1},{3,2},{-1,-1},{3,4},
    {4,0},{4,1},{4,2},{-1,-1},{4,3},
};

static int stat_hp(int base, int lv, int iv, int ev = 0) {
    return base == 1 ? 1 : (2 * base + iv + ev / 4) * lv / 100 + lv + 10;
}
static int stat_st(int base, int lv, int iv, int ev = 0, int nat = 0, int s = -1) {
    int v = (2 * base + iv + ev / 4) * lv / 100 + 5;
    if (NATURE[nat][0] == s) v = v * 110 / 100;
    if (NATURE[nat][1] == s) v = v * 90 / 100;
    return v & 0xFFFF;
}

// 4 derniers moves du learnset ≤ niveau (wotbl : u16 move:9|level:7, fin 0xFFFF)
void default_moves(Nds* db, int sp, int lv, int out[4], int* n) {
    *n = 0;
    auto n0 = db->narc(P_WOTBL);
    if (!n0 || sp < 0 || sp >= n0->count()) return;
    const Bytes& d = n0->member(sp);
    int learned[64], ln = 0;
    for (size_t i = 0; i + 1 < d.size() && ln < 64; i += 2) {
        uint16_t e = u16(d, i);
        if (e == 0xFFFF) break;
        int mv = e & 0x1FF, ml = e >> 9;
        if (mv && ml <= lv && ml > 0) learned[ln++] = mv;
    }
    int start = ln > 4 ? ln - 4 : 0;           // 4 plus récentes, ordre d'appris
    for (int i = start; i < ln && *n < 4; i++) {
        bool dup = false;
        for (int j = 0; j < *n; j++) if (out[j] == learned[i]) dup = true;
        if (!dup) out[(*n)++] = learned[i];
    }
    if (!*n) { out[0] = 165; *n = 1; }                   // Lutte secours
}

// 6 stats DS : pv + att/def/vit/att.spe/def.spe (nature via pid%25)
void battle_stats(Nds* db, int sp, int lv, uint32_t ivs, const uint8_t evs[6],
                  uint32_t pid, int out[6]) {
    uint8_t st[6]{}; int t1 = 0, t2 = 0;
    personal(db, sp, st, &t1, &t2);
    int nat = (int)(pid % 25);
    out[0] = stat_hp(st[0], lv, iv_at(ivs, 0), evs[0]);
    for (int s = 1; s < 6; s++)
        out[s] = stat_st(st[s], lv, iv_at(ivs, s), evs[s], nat, s - 1);
}

void battle_types(Nds* db, int sp, int* t1, int* t2) {
    uint8_t st[6]{}; *t1 = *t2 = -1;
    personal(db, sp, st, t1, t2);
}

MoveInfo battle_move(Nds* db, int mv) {
    MoveInfo r{};
    auto n = db->narc(P_WAZA);
    if (!n || mv < 0 || mv >= n->count()) return r;
    const Bytes& d = n->member(mv);
    if (d.size() < 12) return r;
    r.effect = u16(d, 0); r.cls = d[2]; r.power = d[3];
    r.type = d[4]; r.acc = d[5]; r.pp = d[6];
    r.chance = (int8_t)d[7];                          // % effet secondaire
    r.range = u16(d, 8);                              // RANGE_* (ciblage doubles)
    r.prio = (int8_t)d[10];                           // priorité DS (-6..+5)
    return r;
}

std::string battle_move_name(Nds* db, int mv) {
    static std::vector<std::string> names;
    if (names.empty()) {
        auto n = db->narc(Nds::P_PLMSG);
        if (n && n->count() > 647) names = msgbank_decode(n->member(647));
    }
    if (mv >= 0 && mv < (int)names.size()) return names[mv];
    return "Move " + std::to_string(mv);
}

BatMon battle_make(Nds* db, int sp, int lv, const uint16_t* moves, uint32_t ivs) {
    BatMon m; m.species = sp; m.level = lv;
    uint8_t st[6]{};
    int t1 = 0, t2 = 0, frnd = 70;
    personal(db, sp, st, &t1, &t2, nullptr, nullptr, nullptr, &frnd);
    m.t1 = t1; m.t2 = t2;
    if (ivs == IVS_UNSET) {                               // sauvage : 0-31/stat DS
        ivs = 0;
        for (int s = 0; s < 6; s++) ivs |= (uint32_t)(rand() % 32) << (s * 5);
    }
    m.ivs = ivs;
    m.pid = (uint32_t)rand() << 17 ^ (uint32_t)rand() << 2 ^ (uint32_t)(rand() & 3);
    m.exp = mon_exp_for(db, sp, lv);
    m.friendship = frnd;
    m.maxhp = stat_hp(st[0], lv, iv_at(ivs, 0)); m.hp = m.maxhp;
    int nat = (int)(m.pid % 25);
    m.atk = stat_st(st[1], lv, iv_at(ivs, 1), 0, nat, 0);
    m.def = stat_st(st[2], lv, iv_at(ivs, 2), 0, nat, 1);
    m.spe = stat_st(st[3], lv, iv_at(ivs, 3), 0, nat, 2);
    m.spa = stat_st(st[4], lv, iv_at(ivs, 4), 0, nat, 3);
    m.spd = stat_st(st[5], lv, iv_at(ivs, 5), 0, nat, 4);
    if (moves) {                                          // moves fournis (dresseur)
        for (int i = 0; i < 4 && moves[i]; i++) {
            m.moves[m.nmoves] = moves[i];
            m.pp[m.nmoves] = battle_move(db, moves[i]).pp;
            m.nmoves++;
        }
    }
    if (!m.nmoves) {                                      // sinon learnset
        default_moves(db, sp, lv, m.moves, &m.nmoves);
        for (int i = 0; i < m.nmoves; i++)
            m.pp[i] = battle_move(db, m.moves[i]).pp;
    }
    return m;
}

// équipe dresseur : trdata[tid] (20B : type,class,sprite,size,items4,ai,btltype)
// puis trpoke[tid] : N mons × 8B (base) / 16B (moves) / 10B (item) / 18B (les 2)
std::vector<BatMon> battle_trainer_party(Nds* db, int tid) {
    std::vector<BatMon> out;
    auto td = db->narc(P_TRDATA), tp = db->narc(P_TRPOKE);
    if (!td || !tp || tid < 0 || tid >= td->count()) return out;
    const Bytes& h = td->member(tid);
    const Bytes& d = tp->member(tid);
    if (h.size() < 20) return out;
    int dtype = h[0], np = h[3];
    int msize = dtype == 0 ? 8 : dtype == 1 ? 16 : dtype == 2 ? 10 : 18;
    for (int i = 0; i < np && i * msize + msize <= (int)d.size(); i++) {
        int ivsc = u16(d, i * msize + 0) * 31 / 255;        // ivScale DS -> 0-31
        int lv = u16(d, i * msize + 2);
        int sp = u16(d, i * msize + 4) & 0x3FF;
        if (!sp) continue;
        BatMon m;
        if (dtype == 1 || dtype == 3) {                   // moves présents
            int mo = dtype == 1 ? 6 : 8;
            uint16_t mv[4] = {};
            for (int k = 0; k < 4; k++) mv[k] = u16(d, i * msize + mo + k * 2);
            m = battle_make(db, sp, lv, mv, ivs_flat(ivsc));
        } else
            m = battle_make(db, sp, lv, nullptr, ivs_flat(ivsc));
        out.push_back(m);
    }
    return out;
}

// en-tête trdata[tid] : type@0, class@1, sprite@2 (trfgra), size@3
std::pair<int,int> battle_trainer_class_sprite(Nds* db, int tid) {
    auto td = db->narc(P_TRDATA);
    if (!td || tid < 0 || tid >= td->count()) return {-1, -1};
    const Bytes& h = td->member(tid);
    if (h.size() < 4) return {-1, -1};
    return {h[1], h[2]};
}

// trdata[tid] @16 : battleType (BATTLE_TYPE_* de la ROM)
int battle_trainer_battle_type(Nds* db, int tid) {
    auto td = db->narc(P_TRDATA);
    if (!td || tid < 0 || tid >= td->count()) return 0;
    const Bytes& h = td->member(tid);
    return h.size() >= 20 ? (int)u32(h, 16) : 0;
}

// ── table des types (sTypeMatchupMultipliers, battle_lib.c:2399) ──────────────
// types : 0 Normal,1 Combat,2 Vol,3 Poison,4 Sol,5 Roche,6 Insecte,7 Spectre,
// 8 Acier,9 ?,10 Feu,11 Eau,12 Plante,13 Électrik,14 Psy,15 Glace,16 Dragon,17 Ténèbres
static const uint8_t TCHART[][3] = {                     // {atk, def, mulx2}
    {0,5,1},{0,8,1},                                     // Normal -> Roche/Acier nve
    {10,10,1},{10,11,1},{10,12,2},{10,15,2},{10,6,2},{10,5,1},{10,16,1},{10,8,2},
    {11,10,2},{11,11,1},{11,12,1},{11,4,2},{11,5,2},{11,16,1},
    {13,11,2},{13,13,1},{13,12,1},{13,4,0},{13,2,2},{13,16,1},
    {12,10,1},{12,11,2},{12,12,1},{12,3,1},{12,4,2},{12,2,1},{12,6,1},{12,5,2},{12,16,1},{12,8,1},
    {15,11,1},{15,12,2},{15,15,1},{15,4,2},{15,2,2},{15,16,2},{15,8,1},{15,10,1},
    {1,0,2},{1,15,2},{1,3,1},{1,2,1},{1,14,1},{1,6,1},{1,5,2},{1,17,2},{1,8,2},
    {3,12,2},{3,3,1},{3,4,1},{3,5,1},{3,7,1},{3,8,0},
    {4,10,2},{4,13,2},{4,12,1},{4,3,2},{4,2,0},{4,6,1},{4,5,2},{4,8,2},
    {2,13,1},{2,12,2},{2,1,2},{2,6,2},{2,5,1},{2,8,1},
    {14,1,2},{14,3,2},{14,14,1},{14,17,0},{14,8,1},
    {6,10,1},{6,12,2},{6,1,1},{6,3,1},{6,2,1},{6,14,2},{6,7,1},{6,17,2},{6,8,1},
    {5,10,2},{5,15,2},{5,1,1},{5,4,1},{5,2,2},{5,6,2},{5,8,1},
    {7,0,0},{7,14,2},{7,17,1},{7,8,1},{7,7,2},
    {16,16,2},{16,8,1},
    {17,1,1},{17,14,2},{17,7,2},{17,17,1},{17,8,1},
    {8,10,1},{8,11,1},{8,13,1},{8,15,2},{8,5,2},{8,8,1},
    {0,7,0},{1,7,0},                                     // Foresight-split (immunités)
};
static int type_mul(int atk_t, const BatMon& d) {        // x2 : 0=immun .5=nve 1=neutre 2=se
    int m = 4;                                           // x4 = neutre(×1 en quarts)
    for (auto& e : TCHART)
        if (e[0] == atk_t && (e[1] == d.t1 || e[1] == d.t2))
            m = e[2] == 0 ? 0 : e[2] == 1 ? m / 2 : m * 2; // nve=÷2, se=×2 (cumule)
    return m;                                            // quarts : 0,.5→2, 1→4, 2→8, 4→16
}
int battle_type_mul(int atk_t, const BatMon& d) {        // export pour l'UI
    return type_mul(atk_t, d);
}

// ── modificateurs de combat : stages de stats + statuts ──────────────────────
// stats de combat : stage -6..+6 -> ×(2+s)/2 ou ×2/(2-s). préc/esq : (3±s)/3.
static int stg_stat(int base, int s) {
    return s >= 0 ? base * (2 + s) / 2 : base * 2 / (2 - s);
}
static int stg_acc(int base, int s) {                    // précision/esquive
    return s >= 0 ? base * (3 + s) / 3 : base * 3 / (3 - s);
}
// stats effectives en combat (atk,def,spa,spd,spe -> stg[0..4])
static int eff_atk(const BatMon& m)  { int a = stg_stat(m.atk, m.stg[0]);
    if (m.cond & 0x10) a /= 2; return std::max(1, a); }  // brûlure : atk/2
static int eff_def(const BatMon& m)  { return std::max(1, stg_stat(m.def, m.stg[1])); }
static int eff_spa(const BatMon& m)  { return std::max(1, stg_stat(m.spa, m.stg[2])); }
static int eff_spd(const BatMon& m)  { return std::max(1, stg_stat(m.spd, m.stg[3])); }
static int eff_spe(const BatMon& m)  { int s = stg_stat(m.spe, m.stg[4]);
    if (m.cond & 0x40) s /= 4; return std::max(1, s); }  // para : spe/4

// dégâts Gen4 : (((2L/5+2) × P × A/D) / 50 + 2) × STAB × eff × rand(85-100)
int battle_damage(Nds* db, const BatMon& at, const BatMon& df,
    const MoveInfo& mv, int& eff_x100) {
    eff_x100 = 0;
    if (!mv.power || mv.cls == 2) return 0;              // statut : 0 dégât
    int a = mv.cls == 0 ? eff_atk(at) : eff_spa(at);     // DS : 0=phys 1=spé 2=statut
    int d = mv.cls == 0 ? eff_def(df) : eff_spd(df);
    if (!d) d = 1;
    int dmg = (((2 * at.level / 5 + 2) * mv.power * a / d) / 50) + 2;
    int tm = type_mul(mv.type, df);                      // en quarts (4=×1)
    if (!tm) return 0;
    bool stab = mv.type == at.t1 || mv.type == at.t2;
    dmg = dmg * (stab ? 15 : 10) / 10;
    dmg = dmg * tm / 4;
    dmg = dmg * (85 + rand() % 16) / 100;
    if (dmg < 1) dmg = 1;
    eff_x100 = tm * 25;                                  // 2→50 4→100 8→200 16→400
    return dmg;
}

// ── un round : file {battler, cible, action, arg} triée prio+vitesse ──────────
// slot vivant dans un camp (index équipe) ; -1 = aucun
static int alive_idx(Battle& b, int side, int slot) {
    int i = b.act_idx(side, slot);
    auto& v = side ? b.foes : b.me;
    return i >= 0 && i < (int)v.size() && !v[i].fainted() ? i : -1;
}
// premier battler vivant d'un camp (pour fuite/cible par défaut)
static int first_alive_bid(Battle& b, int side) {
    for (int s = 0; s < 2; s++)
        if (alive_idx(b, side, s) >= 0) return Battle::bid(side, s);
    return Battle::bid(side, 0);
}
// cible de l'IA selon la portée du move
static int ai_target(Battle& b, int side, int range) {
    int os = side ^ 1;
    switch (range) {
    case RANGE_USER: return Battle::bid(side, 0);
    case RANGE_ALLY: case RANGE_USER_OR_ALLY:
        return alive_idx(b, side, 1) >= 0 ? Battle::bid(side, 1)
                                          : Battle::bid(side, 0);
    case RANGE_USER_SIDE: case RANGE_FIELD: case RANGE_FOE_SIDE:
    case RANGE_ALL_FOES: case RANGE_ALL_ADJACENT: case RANGE_ALL:
        return -1;                                   // multi/terrain : pas de cible
    }
    int alive[2], an = 0;                            // adverse : slot au hasard
    for (int s = 0; s < 2; s++)
        if (alive_idx(b, os, s) >= 0) alive[an++] = Battle::bid(os, s);
    return an ? alive[rand() % an] : Battle::bid(os, 0);
}
// portée où le joueur choisit la cible en doubles (DS)
static bool tgt_pickable(int range) {
    return range == RANGE_SINGLE || range == RANGE_SINGLE_SPECIAL ||
           range == RANGE_RANDOM_FOE || range == RANGE_ALLY ||
           range == RANGE_USER_OR_ALLY;
}
// liste des battlers valides pour une portée donnée (camp du joueur `side`)
static int tgt_list(Battle& b, int side, int range, int out[4]) {
    int n = 0, os = side ^ 1;
    auto put = [&](int t) {
        BatMon* m = b.bmon(t);
        if (m && !m->fainted()) out[n++] = t;
    };
    int self = Battle::bid(side, b.pick);             // ALLY : pas soi-même
    switch (range) {
    case RANGE_ALLY: case RANGE_USER_OR_ALLY:
        for (int s = 0; s < 2; s++) {
            int t = Battle::bid(side, s);
            if (t == self && range == RANGE_ALLY) continue;
            put(t);
        }
        break;
    default:                                          // adverses
        for (int s = 0; s < 2; s++) put(Battle::bid(os, s));
        break;
    }
    return n;
}
// cible par défaut du curseur (premier slot adverse vivant)
static int def_tsel(Battle& b, int slot, int range) {
    if (range == RANGE_ALLY || range == RANGE_USER_OR_ALLY)
        return Battle::bid(0, 1 - slot);
    for (int s = 0; s < 2; s++)
        if (alive_idx(b, 1, s) >= 0) return Battle::bid(1, s);
    return Battle::bid(1, 0);
}
void battle_round(Battle& b, Nds* db) {
    b.qn = b.qi = 0; b.lastDmg = 0; b.lastEff = 100;
    b.turns++;                                     // tour (Chrono Ball)
    struct Ent { int bid, tgt, act, arg, pri, spe; };
    Ent ents[8]; int n = 0;
    auto push = [&](int side, int slot, int tgt, int act, int arg, int pri) {
        int i = b.act_idx(side, slot);
        if (i < 0) return;
        BatMon& m = (side ? b.foes : b.me)[i];
        ents[n] = {Battle::bid(side, slot), tgt, act, arg, pri, eff_spe(m)};
        n++;
    };
    // actions du joueur (choisies dans CHOICE/PICKMOVE/PICKTGT)
    for (int s = 0; s < 2; s++) {
        if (alive_idx(b, 0, s) < 0) continue;
        int mv = b.sel_mv[s], tgt = b.sel_tg[s];
        if (mv == -2) continue;                    // item utilisé (hors file)
        if (mv == -4) { push(0, s, -1, -4, tgt, 7); continue; }   // switch
        if (mv == -1) { push(0, s, -1, -1, 0, 6); continue; }     // fuite
        int pri = 0, act = mv;
        BatMon& m = b.me[b.act_idx(0, s)];
        if (mv >= 0 && mv < m.nmoves)
            pri = battle_move(db, m.moves[mv]).prio;
        else if (mv != -3) act = -3;               // inconnu -> Lutte
        if (act >= 0) {
            int r = battle_move(db, m.moves[mv]).range;
            if (tgt < 0) tgt = ai_target(b, 0, r); // cible par défaut/portée
        }
        push(0, s, tgt, act, 0, pri);
    }
    // actions ennemies : move au hasard avec PP, cible selon portée
    for (int s = 0; s < 2; s++) {
        int ai = alive_idx(b, 1, s);
        if (ai < 0) continue;
        BatMon& m = b.foes[ai];
        if (m.run_away && !b.trainer) {            // roamer DS : fuit
            push(1, s, -1, -5, 0, 0); continue;
        }
        int mF = -3, tries = 0;                    // -3 = Lutte si tout à 0
        do { mF = rand() % 4; tries++; }
        while (tries < 8 && (mF >= m.nmoves || m.pp[mF] <= 0 ||
               !m.moves[mF]));
        if (mF >= m.nmoves || m.pp[mF] <= 0 || !m.moves[mF]) mF = -3;
        int pri = 0, tgt = -1;
        if (mF >= 0) {
            MoveInfo mi2 = battle_move(db, m.moves[mF]);
            pri = mi2.prio;
            tgt = ai_target(b, 1, mi2.range);
        } else tgt = ai_target(b, 1, RANGE_RANDOM_FOE);
        push(1, s, tgt, mF, 0, pri);
    }
    // tri DS : priorité décroissante puis vitesse, égalité au hasard
    for (int i = 1; i < n; i++) {
        Ent e = ents[i]; int j = i - 1;
        while (j >= 0 && (ents[j].pri < e.pri ||
               (ents[j].pri == e.pri &&
                (ents[j].spe < e.spe ||
                 (ents[j].spe == e.spe && (rand() & 1))))))
            { ents[j + 1] = ents[j]; j--; }
        ents[j + 1] = e;
    }
    for (int i = 0; i < n; i++) {
        b.queue[b.qn][0] = ents[i].bid; b.queue[b.qn][1] = ents[i].tgt;
        b.queue[b.qn][2] = ents[i].act; b.queue[b.qn][3] = ents[i].arg;
        b.qn++;
    }
}

// ── capture : formule DS (battle_script.c CalcCatchShakes) ───────────────────
// rate = taux espèce * ballMod/10 * (3PVmax - 2PV) / 3PVmax ; >=255 -> capturé ;
// sinon 4 jets sous p = (0xFFFF<<4) / sqrt(sqrt(0xFF*0x10000/rate)).
int battle_catch(Nds* db, Battle& b, int item) {
    if (item == 1) return 4;                            // Master Ball
    BatMon* fp = b.bmon(first_alive_bid(b, 1));         // le sauvage vivant
    if (!fp) return 0;
    BatMon& tgt = *fp;
    int rate = 255;
    auto n = db->narc(P_PERSONAL);
    if (n && tgt.species >= 0 && tgt.species < n->count()) {
        const Bytes& d = n->member(tgt.species);
        if (d.size() >= 9) rate = d[8];
    }
    int bm = 10;                                        // Poké Ball et co.
    bool wtr = tgt.t1 == 10 || tgt.t2 == 10,            // type eau / insecte
         bug = tgt.t1 == 6  || tgt.t2 == 6;
    switch (item) {
    case 2: bm = 20; break;                             // Hyper Ball
    case 3: bm = 15; break;                             // Super Ball
    case 5: bm = 15; break;                             // Safari Ball
    case 6:  if (wtr || bug) bm = 30; break;            // Filet Ball
    case 7:  if (b.terrain == 7) bm = 35; break;        // Scuba Ball (eau)
    case 8:  bm = tgt.level < 40 ? std::max(10, 40 - tgt.level) : 10; break; // Faiblo
    case 10: bm = std::min(40, 10 + b.turns); break;    // Chrono Ball
    case 13: if (b.tod == 2 || b.terrain == 5) bm = 35; break; // Sombre (nuit/grotte)
    case 15: if (b.turns < 1) bm = 40; break;           // Rapide Ball
    }
    long cr = (long)rate * bm / 10;
    cr = cr * (3L * tgt.maxhp - 2L * tgt.hp) / (3L * tgt.maxhp);
    // DS : statut -> x2 sommeil/gel, x1.5 poison/brûlure/paralysie
    if (tgt.cond & 0x27) cr *= 2;
    else if (tgt.cond & 0xD8) cr = cr * 3 / 2;
    if (b.tutorial) return 4;                        // tuto : capture garantie
    if (cr >= 255) return 4;
    double a = (255.0 * 65536.0) / cr;                  // 0xFF<<16 / rate
    int p = (int)((65536.0 * 16.0) / sqrt(sqrt(a)));    // (0xFFFF<<4)/sqrt⁴
    int shakes = 0;
    for (; shakes < 4; shakes++)
        if ((rand() & 0xFFFF) >= p) break;
    return shakes;
}

std::string battle_species_name(Nds* db, int sp) {
    static std::vector<std::string> names;
    if (names.empty()) {
        auto n = db->narc(Nds::P_PLMSG);
        if (n && n->count() > 412) names = msgbank_decode(n->member(412));
    }
    if (sp >= 0 && sp < (int)names.size()) return names[sp];
    return "Pokémon " + std::to_string(sp);
}

// ── effets de moves (BATTLE_EFFECT_*, move_battle_effects.txt) ───────────────
static const char* stat_fr[] = {"Attaque","Défense","Attaque Spé.","Défense Spé.","Vitesse","Précision","Esquive","taux de critiques"};
static const char* stat_en[] = {"Attack","Defense","Sp. Atk","Sp. Def","Speed","accuracy","evasiveness","critical-hit ratio"};

// modifie un stage avec message DS ; clamp ±6
static void stg_change(Battle& b, Nds* db, BatMon& m, int st, int n, bool self) {
    std::string nm = battle_species_name(db, m.species);
    if (m.stg[st] >= 6 && n > 0)  { b.msg += tr("\nÇa ne montera pas plus haut !",
        "\nIt won't go any higher!"); return; }
    if (m.stg[st] <= -6 && n < 0) { b.msg += tr("\nÇa ne baissera pas plus bas !",
        "\nIt won't go any lower!"); return; }
    int8_t& s = m.stg[st]; s = (int8_t)std::clamp(s + n, -6, 6);
    b.msg += "\n" + nm + tr(" : ", "'s ") +
        tr(stat_fr[st], stat_en[st]) +
        (n > 1  ? tr(" augmente beaucoup !",  " sharply rose!")
         : n > 0 ? tr(" augmente !",          " rose!")
         : n < -1 ? tr(" baisse beaucoup !",  " sharply fell!")
                 : tr(" baisse !",            " fell!"));
}

// pose un statut primaire (immunités DS : psn→poison/acier, brn→feu, frz→glace)
static bool set_cond(Battle& b, Nds* db, BatMon& m, uint32_t c,
    const char* fr, const char* en, bool toxic = false) {
    bool immune = ((c & 0x88) && (m.t1 == 3 || m.t1 == 8 || m.t2 == 3 || m.t2 == 8))
               || ((c & 0x10) && (m.t1 == 10 || m.t2 == 10))
               || ((c & 0x20) && (m.t1 == 15 || m.t2 == 15));
    if (m.cond || immune) {                           // statut primaire exclusif
        b.msg += std::string("\n") + tr("Ça n'affecte pas ", "It doesn't affect ") +
            battle_species_name(db, m.species) + "...";
        return false;
    }
    m.cond |= c;
    if (toxic) m.toxic_n = 1;
    b.msg += "\n" + battle_species_name(db, m.species) + tr(fr, en);
    return true;
}

// helpers définis plus bas (ciblage/reset au switch)
static void reset_volatile(BatMon& m);
static int next_bench(Battle& b, int side, int slot = 0, int from = 0);

// effet d'un move après la touche (dmg = dégâts infligés, side = attaquant,
// tbid = battler id de la cible pour les effets positionnels comme Roar)
static void apply_effect(Battle& b, Nds* db, BatMon& at, BatMon& df,
    const MoveInfo& mv, int dmg, int side, int tbid, bool sec) {
    static const int ST_MAP[7] = {0, 1, 4, 2, 3, 5, 6};   // ordre table DS
    switch (mv.effect) {
    // ── statut primaire (moves pow=0 : toujours appliqué à la touche) ──
    case 1:   set_cond(b, db, df, 2 + rand() % 3,
        tr(" s'endort !", " fell asleep!"), ""); break;
    case 33:  set_cond(b, db, df, 0x80,
        tr(" est gravement empoisonné !", " was badly poisoned!"), "", true); break;
    case 66:  set_cond(b, db, df, 0x08,
        tr(" est empoisonné !", " was poisoned!"), ""); break;
    case 67:  set_cond(b, db, df, 0x40,
        tr(" est paralysé ! Il risque de ne pas pouvoir bouger !",
           " is paralyzed! It may be unable to move!"), ""); break;
    case 167: set_cond(b, db, df, 0x10,
        tr(" est brûlé !", " was burned!"), ""); break;
    case 49:  if (!df.cond && !df.conf) { df.conf = 2 + rand() % 4;
        b.msg += "\n" + battle_species_name(db, df.species) +
            tr(" devient confus !", " became confused!"); } break;
    case 120:                                   // Attraction : 50% immobilisé,
        if (!df.attract) { df.attract = true;   // jusqu'au switch/KO (DS)
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" tombe amoureux !", " fell in love!"); } break;
    case 199:                                   // Teeter Dance : confusion
        if (!df.conf) { df.conf = 2 + rand() % 4;
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" devient confus !", " became confused!"); } break;
    case 187: if (!df.cond) { df.drowsy = 2;                // Bâillement : dort au tour suivant
        b.msg += "\n" + battle_species_name(db, df.species) +
            tr(" bâille !", " grew drowsy!"); } break;
    case 84:  if (!df.seeded && df.t1 != 12 && df.t2 != 12) {   // Vampigraine
        df.seeded = true;
        b.msg += "\n" + battle_species_name(db, df.species) +
            tr(" est vampirisé !", " was seeded!"); } break;
    case 118:                                 // Swagger : atk +2 + confus
        stg_change(b, db, df, 0, 2, false);
        if (!df.conf) { df.conf = 2 + rand() % 4;
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" devient confus !", " became confused!"); } break;
    case 166: stg_change(b, db, df, 2, 1, false);               // Flatter
        if (!df.conf) { df.conf = 2 + rand() % 4;
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" devient confus !", " became confused!"); } break;
    // ── stats : soi ± ─────────────────────────────────────────────
    case 10: case 11: case 12: case 13: case 14: case 15: case 16:
        stg_change(b, db, at, ST_MAP[mv.effect - 10], 1, true); break;
    case 50: case 51: case 52: case 53: case 54: case 55: case 56:
        stg_change(b, db, at, ST_MAP[mv.effect - 50], 2, true); break;
    case 18: case 19: case 20: case 21: case 22: case 23: case 24:
        stg_change(b, db, df, ST_MAP[mv.effect - 18], -1, false); break;
    case 58: case 59: case 60: case 61: case 62:
        stg_change(b, db, df, ST_MAP[mv.effect - 58], -2, false); break;
    case 63: stg_change(b, db, df, 6, -2, false); break;   // EVA_DOWN_2
    case 64: stg_change(b, db, df, 5, -2, false); break;   // ACC_DOWN_2
    case 108: stg_change(b, db, at, 6, 2, true); break;    // Minimize
    case 156: stg_change(b, db, at, 1, 1, true); break;    // Defense Curl
    case 205: stg_change(b, db, at, 0, -1, true);          // Close Combat
              stg_change(b, db, at, 1, -1, true); break;   //  (atk+def -1 soi)
    case 182: stg_change(b, db, at, 1, -1, true);          // own def/spdef
              stg_change(b, db, at, 3, -1, true); break;
    case 204: stg_change(b, db, at, 2, -2, true); break;   // Draco/Overheat
    case 206: stg_change(b, db, at, 1, 1, true);           // def+speed +1
              stg_change(b, db, at, 4, 1, true); break;
    case 208: stg_change(b, db, at, 0, 1, true);           // Bulk Up
              stg_change(b, db, at, 1, 1, true); break;
    case 211: stg_change(b, db, at, 2, 1, true);           // Calm Mind
              stg_change(b, db, at, 3, 1, true); break;
    case 212: stg_change(b, db, at, 0, 1, true);           // Dragon Dance
              stg_change(b, db, at, 4, 1, true); break;
    case 226: stg_change(b, db, at, rand() % 7, 2, true); break; // Acupressure
    case 47:  at.stg[7] = std::min<int8_t>(at.stg[7] + 2, 6);    // Focus Energy
        b.msg += tr("\nLe taux de critiques augmente !",
                    "\nIt's getting pumped!"); break;
    case 25: for (int k = 0; k < 8; k++) { at.stg[k] = 0; df.stg[k] = 0; }
        b.msg += tr("\nToutes les modifs de stats ont été annulées !",
                    "\nAll stat changes were eliminated!"); break;
    case 143: for (int k = 0; k < 8; k++) at.stg[k] = df.stg[k];
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" copie les modifs de stats !", " copied the stat changes!"); break;
    case 250: for (int k = 0; k < 8; k++) std::swap(at.stg[k], df.stg[k]);
        b.msg += tr("\nLes modifs de stats sont échangées !",
                    "\nStat changes were swapped!"); break;
    // ── dégâts fixes / spéciaux ────────────────────────────────────
    case 38:                                              // OHKO
        if (df.level > at.level) b.msg += tr("\nÇa n'affecte pas ", "\nIt doesn't affect ") +
            battle_species_name(db, df.species) + "...";
        else { df.hp = 0;
            b.msg += tr("\nKO en un coup !", "\nIt's a one-hit KO!"); }
        break;
    case 40:  df.hp -= std::min(df.hp, df.maxhp / 2); break;   // Super Fang
    case 41:  df.hp -= std::min(df.hp, 40); break;             // Dragon Rage
    case 130: df.hp -= std::min(df.hp, 20); break;             // Sonicboom
    case 87:  df.hp -= std::min(df.hp, at.level); break;       // Night Shade
    case 88:  df.hp -= std::min(df.hp, rand() % at.level + 1); break; // Psywave-like
    case 126: { int d = std::max(1, at.level * (50 + rand() % 101) / 100);
        df.hp -= std::min(df.hp, d); break; }                  // Psywave
    case 91:  { int h = (at.hp + df.hp) / 2;                   // Pain Split
        at.hp = df.hp = std::min(std::max(h, 1), at.maxhp); } break;
    case 189: if (at.hp < df.hp) df.hp = at.hp; break;         // Endeavor
    case 45:  break;                                          // crash : géré au miss
    // ── soins ─────────────────────────────────────────────────────
    case 32: case 132: case 214: {                            // +moitié PV
        int h = std::max(1, at.maxhp / 2);
        at.hp = std::min(at.maxhp, at.hp + h);
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" récupère des PV !", " regained health!"); } break;
    case 37:                                                  // Rest
        at.hp = at.maxhp; at.cond = 3; at.conf = 0;           // Repos : dort 2 tours
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" s'endort et récupère !", " went to sleep and recovered!"); break;
    case 102: case 193: {                                     // soin du statut
        uint32_t oc = at.cond; at.cond = 0; at.conf = 0;
        if (oc) b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" est guéri de son statut !", " was cured of its status!");
        else b.msg += tr("\nMais ça a raté !", "\nBut it failed!"); } break;
    case 234: if (at.cond) {                                  // Psycho Shift
        df.cond = at.cond; at.cond = 0;
        b.msg += tr("\nLe statut est transféré !", "\nStatus was transferred!"); } break;
    // ── drain / recul ──────────────────────────────────────────────
    case 3:   if (dmg > 0 && df.hp >= 0) {                    // drain moitié
        int h = std::max(1, dmg / 2);
        at.hp = std::min(at.maxhp, at.hp + h);
        b.msg += "\n" + battle_species_name(db, df.species) +
            tr(" voit son énergie drainée !", " had its energy drained!"); } break;
    case 8:   if (df.cond & 7 && dmg > 0) {                   // Dream Eater
        int h = std::max(1, dmg / 2); at.hp = std::min(at.maxhp, at.hp + h); }
        else if (!(df.cond & 7))
            b.msg += tr("\nMais ça a raté !", "\nBut it failed!");
        break;
    case 48:  at.hp = std::max(1, at.hp - dmg / 4);           // recul 1/4
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" subit le contrecoup !", " is hit with recoil!"); break;
    case 198: at.hp = std::max(1, at.hp - dmg / 3);           // recul 1/3
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" subit le contrecoup !", " is hit with recoil!"); break;
    case 269: at.hp = std::max(1, at.hp - dmg / 2);           // recul moitié
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" subit le contrecoup !", " is hit with recoil!"); break;
    // ── divers combat ─────────────────────────────────────────────
    case 7:   at.hp = 0;                                      // Explosion
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" s'explose !", " exploded!"); break;
    case 80:  at.recharge = true; break;                      // Hyper Beam
    case 27:  at.conf = 2;                                    // rampage → confus
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" devient confus !", " became confused!"); break;
    case 28: {                                              // Roar : switch forcé
        int ds = Battle::bid_side(tbid), dslot = Battle::bid_slot(tbid);
        if (!b.trainer && ds == 1) {                        // sauvage adverse fuit
            b.st = Battle::FLEE; b.won = 1;
            b.msg += tr("\nLe combat prend fin !", "\nThe battle ended!");
            break;
        }
        int nx = next_bench(b, ds, dslot);                  // remplaçant du slot
        if (nx >= 0) {
            reset_volatile(df);
            if (ds) { if (dslot) b.fi2 = nx; else b.fi = nx; }
            else    { if (dslot) b.mi2 = nx; else b.mi = nx;
                      b.me_part |= 1 << nx; }
            b.msg += tr("\nLe Pokémon est forcé de changer !",
                        "\nThe Pokémon was switched out!");
        } else if (!b.trainer) { b.st = Battle::FLEE; b.won = 1; }
        else b.msg += tr("\nMais ça a raté !", "\nBut it failed!");
        break; }
    case 153:                                   // Teleport : échoue vs dresseur
        if (b.trainer) { b.msg += tr("\nMais ça a échoué !", "\nBut it failed!"); break; }
        b.st = Battle::FLEE; b.won = side == 0 ? 0 : 1;
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" s'enfuit !", " fled!"); break;
    case 101: if (df.hp <= 0) df.hp = 1; break;               // False Swipe
    case 111: at.prot = true;                                 // Protect
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" se protège !", " protected itself!"); break;
    case 116: at.endure = true;                               // Endure
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" se concentre !", " braced itself!"); break;
    case 181: at.ingrain = true;                              // Racines
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" plante ses racines !", " planted its roots!"); break;
    case 35:  b.screen[side] |= 0x50;                         // Mur Lumière 5t (spé)
        b.msg += tr("\nMur Lumière dressé !", "\nLight Screen raised!"); break;
    case 65:  b.screen[side] |= 0x05;                         // Protection 5t (phys)
        b.msg += tr("\nProtection dressée !", "\nReflect raised!"); break;
    case 186: b.screen[side ^ 1] = 0;                         // Brick Break
        b.msg += tr("\nLes barrières sont brisées !",
                    "\nThe barriers were shattered!"); break;
    case 109:                                                 // Malédiction
        if (at.t1 == 7 || at.t2 == 7) {                       // spectre
            at.hp /= 2; df.conf = 3;                          // simplifié
        } else { stg_change(b, db, at, 0, 1, true);
                 stg_change(b, db, at, 1, 1, true);
                 stg_change(b, db, at, 4, -1, true); } break;
    case 168: at.hp = 0;                                      // Memento
        stg_change(b, db, df, 0, -2, false);
        stg_change(b, db, df, 2, -2, false); break;
    case 220: case 270: {                                     // at KO + heal next
        at.hp = 0;
        for (auto& m : (side ? b.foes : b.me))
            if (!m.fainted()) { m.hp = m.maxhp; m.cond = 0; break; }
        b.msg += "\n" + battle_species_name(db, at.species) +
            tr(" se sacrifie !", " sacrificed itself!"); } break;
    // ── secondaires (chance%) ──────────────────────────────────────
    default:
        if (!sec) break;
        switch (mv.effect) {
        case 2:  set_cond(b, db, df, 0x08, tr(" est empoisonné !"," was poisoned!"), ""); break;
        case 202: set_cond(b, db, df, 0x80, tr(" est gravement empoisonné !"," was badly poisoned!"), "", true); break;
        case 4:  set_cond(b, db, df, 0x10, tr(" est brûlé !"," was burned!"), ""); break;
        case 5:  set_cond(b, db, df, 0x20, tr(" est gelé !"," was frozen!"), ""); break;
        case 6:  set_cond(b, db, df, 0x40, tr(" est paralysé !"," is paralyzed!"), ""); break;
        case 31: case 146: case 147: case 150: case 151: case 158:
        case 273: case 274: case 275: {                   // flinch si pas encore agi
            if (!df.fainted())
                for (int k = b.qi; k < b.qn; k++)
                    if (b.queue[k][0] == tbid) { df.flinch = true; break; }
            } break;
        case 76: case 267:
            if (!df.conf && !df.fainted()) { df.conf = 2 + rand() % 4;
                b.msg += "\n" + battle_species_name(db, df.species) +
                    tr(" devient confus !", " became confused!"); } break;
        case 36: {                                            // Tri Attack
            int r = rand() % 3;
            if (r == 0) set_cond(b, db, df, 0x40, tr(" est paralysé !"," is paralyzed!"), "");
            else if (r == 1) set_cond(b, db, df, 0x10, tr(" est brûlé !"," was burned!"), "");
            else set_cond(b, db, df, 0x20, tr(" est gelé !"," was frozen!"), ""); } break;
        case 68: case 69: case 70: case 71: case 72: case 73: case 74:
            stg_change(b, db, df, ST_MAP[mv.effect - 68], -1, false); break;
        case 218: stg_change(b, db, df, 4, -1, false); break;
        case 229: stg_change(b, db, df, 1, -1, false);
                  stg_change(b, db, df, 4, -1, false); break;
        case 138: stg_change(b, db, at, 1, 1, true); break;
        case 139: stg_change(b, db, at, 0, 1, true); break;
        case 276: stg_change(b, db, at, 2, 1, true); break;
        case 271: stg_change(b, db, df, 3, -2, false); break;
        case 140:                                          // Ancientpower
            for (int k = 0; k < 5; k++) stg_change(b, db, at, k, 1, true);
            break;
        case 77: case 29: break;                              // multi-hit ailleurs
        case 125: if (at.cond & 0x20) { at.cond &= ~0x20u;    // Scald : dégel
            b.msg += "\n" + battle_species_name(db, at.species) +
                tr(" a dégelé !", " thawed out!"); }
            set_cond(b, db, df, 0x10, tr(" est brûlé !"," was burned!"), ""); break;
        case 169: break;                                      // Hex : déjà doublé
        case 42: case 261: if (!df.bound) { df.bound = 4 + rand() % 2;
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" est piégé !", " was trapped!"); } break;
        case 253: at.hp = std::max(1, at.hp - dmg / 4);       // recul + brûlure
            set_cond(b, db, df, 0x10, tr(" est brûlé !"," was burned!"), ""); break;
        case 262: at.hp = std::max(1, at.hp - dmg / 4);       // recul + para
            set_cond(b, db, df, 0x40, tr(" est paralysé !"," is paralyzed!"), ""); break;
        default: break;
        }
    }
}

// réinitialise les états volatils d'un Pokémon qui sort du terrain (DS)
static void reset_volatile(BatMon& m) {
    for (int k = 0; k < 8; k++) m.stg[k] = 0;
    m.conf = 0; m.seeded = false; m.bound = 0;
    m.toxic_n = 0; m.ingrain = false; m.drowsy = 0;
    m.recharge = false; m.flinch = false; m.prot = false;
    m.endure = false; m.turns_out = 0; m.attract = false;
}
// premier index d'équipe non-KO et non déjà sur le terrain
static int next_bench(Battle& b, int side, int slot, int from) {
    auto& v = side ? b.foes : b.me;
    int lo = 0, hi = (int)v.size();
    // combat duo : la réserve du slot est celle de SON dresseur
    if (side && b.foe_split > 0) { if (slot) lo = b.foe_split; else hi = b.foe_split; }
    for (int i = std::max(lo, from); i < hi; i++) {
        if (v[i].fainted()) continue;
        if (i == b.act_idx(side, 0) || i == b.act_idx(side, 1)) continue;
        return i;
    }
    return -1;
}
// machine à états ; keys : 'E' confirmer, 'Q' retour, 'WASD' déplacer curseur
void battle_update(Battle& b, Nds* db, float dt, int key) {
    // timers d'animation combat (propulsion / flash de touche / jauges PV)
    if (b.atk_side >= 0) {
        b.atk_t += dt * 3.2f;                        // ~0.3s aller-retour
        if (b.atk_t >= 1.f) b.atk_side = -1;
    }
    banim_update(b, dt);                            // particules + script we.arc
    if (b.hit_t > 0.f) { b.hit_t -= dt; if (b.hit_t <= 0.f) b.hit_side = -1; }
    for (int s = 0; s < 4; s++) {                    // jauge PV glisse vers réel
        BatMon* m = b.bmon(s);
        if (!m) continue;
        int real = m->hp;
        float& d = b.disp_hp[s];
        if (d <= 0.f) d = (float)real;
        if (d > (float)real) { d -= dt * 60.f; if (d < real) d = (float)real; }
        else if (d < (float)real) { d += dt * 60.f; if (d > real) d = (float)real; }
        // KO : glissade descendante + fondu
        if (real <= 0 && b.faint_t[s] < 1.f) b.faint_t[s] += dt * 2.f;
        if (real > 0) b.faint_t[s] = 0.f;
    }
    if (!b.active) return;
    // avance au prochain slot à commander ; sinon lance l'exécution du tour
    auto next_pick = [&]() {
        if (b.dbl) {
            for (int s = b.pick + 1; s < 2; s++)
                if (alive_idx(b, 0, s) >= 0) { b.pick = s; b.sel = 0;
                    b.st = Battle::CHOICE; return; }
        }
        battle_round(b, db);                      // file complète puis EXEC
        b.st = Battle::EXEC; b.anim = 0.0f; b.qi = 0; b.eor = 0;
    };
    auto act = [&]() {                       // traiter une entrée de la file
        int bid = b.queue[b.qi][0], tbid = b.queue[b.qi][1];
        int mvI = b.queue[b.qi][2], arg = b.queue[b.qi][3];
        b.qi++;
        BatMon* atp = b.bmon(bid);
        if (!atp || atp->fainted()) return;
        BatMon& at = *atp;
        int side = Battle::bid_side(bid);
        if (mvI == -4) {                       // switch (arg = index équipe)
            auto& v = side ? b.foes : b.me;
            if (arg >= 0 && arg < (int)v.size() && !v[arg].fainted()) {
                reset_volatile(at);
                if (side) (bid & 2) ? b.fi2 = arg : b.fi = arg;
                else      (bid & 2) ? b.mi2 = arg : b.mi = arg;
                if (!side) b.me_part |= 1 << arg;
                b.msg = tr(side ? "L'ennemi rappelle " : "",
                           side ? "The foe called back " : "") +
                    battle_species_name(db, at.species) + " !\n" +
                    tr(side ? "Il envoie " : "En avant, ",
                       side ? "They send out " : "Go, ") +
                    battle_species_name(db, v[arg].species) + " !";
                b.anim = 1.2f;
            }
            return;
        }
        if (mvI == -5) {                       // roamer : fuite ennemie
            b.st = Battle::FLEE; b.won = 1;
            b.msg = battle_species_name(db, at.species) +
                tr(" s'enfuit !", " fled!"); b.anim = 0.8f;
            return;
        }
        if (mvI == -1) {                       // tentative de fuite (formule DS)
            // DS : A = (spe_fuyard*128 / spe_adv) %256 + 30*essais ; l'essai
            // n'incrémente qu'après un échec ; l'ennemi attaque ensuite.
            BatMon* fp = b.bmon(first_alive_bid(b, 1));
            int fspe = fp ? fp->spe : 1;
            int odds = ((at.spe * 128) / std::max(1, fspe)) & 0xFF;
            if (at.spe >= fspe ||
                rand() % 256 < odds + b.tries * 30) {
                b.st = Battle::FLEE; b.won = -1;
                b.msg = tr("Vous prenez la fuite !", "Got away safely!"); b.anim = 0;
            } else {
                b.tries++;
                b.msg = tr("Fuite impossible !", "Can't escape!"); b.anim = 1.0f;
            }
            return;
        }
        std::string nm = battle_species_name(db, at.species);
        // ── statuts pré-action (ordre DS : sommeil, gel, flinch, recharge,
        //    confusion, paralysie) ──────────────────────────────────────
        if (at.cond & 7) {                                // sommeil (bits0-2)
            at.cond--;
            if (at.cond & 7) {
                b.msg = nm + tr(" dort profondément.", " is fast asleep.");
                b.anim = 0.9f; return;
            }
            b.msg = nm + tr(" se réveille !", " woke up!");
        }
        if (at.cond & 0x20) {                             // gel : 20% dégel
            if (rand() % 5 == 0) { at.cond &= ~0x20u;
                b.msg = nm + tr(" a dégelé !", " thawed out!"); }
            else { b.msg = nm + tr(" est gelé !", " is frozen solid!");
                   b.anim = 0.9f; return; }
        }
        if (at.flinch) { at.flinch = false;
            b.msg = nm + tr(" a la frousse !", " flinched!");
            b.anim = 0.9f; return; }
        if (at.recharge) { at.recharge = false;
            b.msg = nm + tr(" doit recharger !", " must recharge!");
            b.anim = 0.9f; return; }
        if (at.conf) {                                    // confusion
            at.conf--;
            if (!at.conf)
                b.msg = nm + tr(" n'est plus confus !", " snapped out of it!");
            else if (rand() & 1) {                        // 50% : se blesse
                MoveInfo cf{0, 0, 40, -1, 0, 0, 0};       // 40 neutre vs soi
                int ef; int d = battle_damage(db, at, at, cf, ef);
                at.hp = std::max(0, at.hp - d);
                b.msg = nm + tr(" est confus !", " is confused!") +
                    tr("\nIl se blesse dans sa confusion !",
                       "\nIt hurt itself in its confusion!");
                if (at.fainted())
                    b.msg += "\n" + nm + tr(" est K.O. !", " fainted!");
                b.anim = 1.1f; return;
            }
        }
        if ((at.cond & 0x40) && rand() % 4 == 0) {        // para : 25%
            b.msg = nm + tr(" est paralysé ! Il ne peut plus bouger !",
                            " is paralyzed! It can't move!");
            b.anim = 0.9f; return;
        }
        if (at.attract && rand() & 1) {                   // Attraction : 50% DS
            b.msg = nm + tr(" est immobilisé par l'amour !",
                            " is immobilized by love!");
            b.anim = 0.9f; return;
        }
        at.turns_out++;
        int mid = mvI == -3 ? 165 : at.moves[mvI];   // -3 = Lutte (Struggle)
        MoveInfo mv = battle_move(db, mid);
        if (getenv("BTLDBG"))
            printf("   [act] bid=%d mvI=%d id=%d pow=%d cls=%d acc=%d eff=%d rng=%d\n",
                bid, mvI, mid, mv.power, mv.cls, mv.acc, mv.effect, mv.range);
        // ── cibles réelles selon la portée (MOVE_RANGE DS) ────────────────
        int tbids[4]; int nt = 0;
        auto add_t = [&](int t) {
            BatMon* m = b.bmon(t);
            if (m && !m->fainted()) tbids[nt++] = t;
        };
        int oside = side ^ 1, oslot = 1 - Battle::bid_slot(bid);
        switch (mv.range) {
        case RANGE_USER: add_t(bid); break;
        case RANGE_ALLY: case RANGE_USER_OR_ALLY:
            add_t(tbid >= 0 ? tbid : Battle::bid(side, oslot)); break;
        case RANGE_USER_SIDE: case RANGE_FIELD: case RANGE_FOE_SIDE:
            add_t(tbid >= 0 ? tbid : first_alive_bid(b, oside)); break;
        case RANGE_ALL_FOES:
            for (int s = 0; s < 2; s++) add_t(Battle::bid(oside, s)); break;
        case RANGE_ALL_ADJACENT:
            for (int s = 0; s < 2; s++) add_t(Battle::bid(oside, s));
            add_t(Battle::bid(side, oslot)); break;
        case RANGE_ALL:
            for (int s = 0; s < 2; s++) add_t(Battle::bid(oside, s));
            add_t(Battle::bid(side, oslot)); add_t(bid); break;
        case RANGE_RANDOM_FOE: tbid = ai_target(b, side, mv.range);
            [[fallthrough]];
        default: {                                   // 1 cible ; DS : si tombée,
            add_t(tbid);                             // repart sur l'autre slot
            if (!nt) for (int s = 0; s < 2; s++)     // adverse vivant
                if (Battle::bid(oside, s) != tbid) add_t(Battle::bid(oside, s));
            break; }
        }
        if (!nt) {                                   // plus aucune cible :
            b.msg = nm + tr(" rate son attaque !",   // le move échoue
                            "'s attack missed!");    //  (cibles déjà K.O.)
            b.anim = 1.0f; return;
        }
        if (!b.msg.empty()) b.msg += "\n";
        b.msg += nm + tr(" utilise ", " used ") +
            battle_move_name(db, mid) + " !";
        b.atk_side = bid; b.atk_t = 0.f;             // propulsion attaquant
        banim_start(b, mid, bid, tbid);             // anim DS we.arc du move
        if (mvI >= 0 && mvI < at.nmoves && at.pp[mvI] > 0) at.pp[mvI]--;
        bool sec = mv.chance > 0 && rand() % 100 < mv.chance;
        int anyfaint = 0;
        for (int ti = 0; ti < nt; ti++) {            // ── boucle par cible ──
        int tside = Battle::bid_side(tbids[ti]);
        BatMon& df = *b.bmon(tbids[ti]);
        // Protect adverse : le move est bloqué
        if (df.prot && mv.effect != 223 && mv.power >= 0) {
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" se protège !", " protected itself!");
            df.prot = false; continue;
        }
        // précision effective : acc ± stages att/def ; acc=0 -> jamais raté
        int accv = mv.acc;
        if (accv && (at.stg[5] || df.stg[6]))
            accv = accv * stg_acc(100, at.stg[5]) / stg_acc(100, df.stg[6]);
        if (accv && b.weather == BW_FOG)                  // brume : acc x0.6
            accv = accv * 6 / 10;
        if (b.weather == BW_RAIN && mid == 87)            // Tonnerre : jamais raté
            accv = 0;
        if (accv && rand() % 100 >= accv) {
            b.msg += "\n" + battle_species_name(db, df.species) +
                tr(" évite l'attaque !", " avoided the attack!");
            if (mv.effect == 45) {                        // crash : at -50% PV
                at.hp = std::max(0, at.hp - at.maxhp / 2);
                b.msg += "\n" + nm + tr(" s'écrase !", " crashed!");
            }
            continue;
        }
        int dmg = 0;
        // dégâts fixes/OHKO : l'effet gère tout, pas de formule
        static const int FIXE[] = {38, 40, 41, 87, 88, 126, 130, 189};
        bool noform = false;
        for (int e : FIXE) if (mv.effect == e) noform = true;
        if (mv.power > 0 && !noform) {
            MoveInfo mv2 = mv;
            int hits = 1;
            // puissance variable selon l'effet
            if (mv.effect == 169 && df.cond) mv2.power *= 2;      // Hex/Choc
            if (mv.effect == 221 && at.hp * 2 <= at.maxhp) mv2.power *= 2;
            if (mv.effect == 121) mv2.power = std::max(1, at.friendship * 10 / 25);
            if (mv.effect == 123) mv2.power = std::max(1, (255 - at.friendship) * 10 / 25);
            if (mv.effect == 29 || mv.effect == 77) {              // multi-hit DS
                int r = rand() % 8;                    // 37.5/37.5/12.5/12.5
                hits = r < 3 ? 2 : r < 6 ? 3 : r < 7 ? 4 : 5;
            } else if (mv.effect == 44) hits = 2;
            else if (mv.effect == 104) hits = 3;
            for (int h = 0; h < hits && !df.fainted(); h++) {
                int eff; int d = battle_damage(db, at, df, mv2, eff);
                if (d > 0 && nt > 1)                      // dispersion Gen4 :
                    d = std::max(1, d * 3 / 4);           // multi-cibles x0.75
                if (d > 0 && b.weather == BW_RAIN)        // pluie : Eau x1.5 Feu x0.5
                    d = mv2.type == 11 ? std::max(1, d * 3 / 2)
                        : mv2.type == 10 ? std::max(1, d / 2) : d;
                // coup critique DS : base 1/16, +1 stage (haut crit) = 1/8
                int cs = at.stg[7] + ((mv.effect == 43 || mv.effect == 200 ||
                                       mv.effect == 209) ? 1 : 0);
                static const int CR[5] = {16, 8, 4, 3, 2};
                bool crit = rand() % CR[std::min(4, cs)] == 0 && d > 0;
                if (crit) {
                    d *= 2;
                    if (h == 0) b.msg += tr("\nCoup critique !", "\nCritical hit!");
                }
                // DS : les écrans ne réduisent pas un coup critique
                if (!crit && b.screen[tside ^ 1] & (mv.cls == 0 ? 0x0F : 0xF0))
                    d = std::max(1, d / 2);                  // écran adverse
                if (df.endure && d >= df.hp) d = df.hp - 1;
                df.hp -= d; if (df.hp < 0) df.hp = 0;
                if (d > 0) { b.hit_side = tbids[ti]; b.hit_t = 0.4f; }
                b.lastDmg = d; b.lastEff = eff;
                if (getenv("BTLDBG"))
                    printf("   [dmg] %d -> dfhp=%d eff=%d\n", d, df.hp, eff);
                if (!eff) { b.msg = tr("Ça n'affecte pas ", "It doesn't affect ") +
                    battle_species_name(db, df.species) + "..."; break; }
                if (eff >= 200)
                    b.msg += tr("\nC'est super efficace !", "\nIt's super effective!");
                else if (eff < 100)
                    b.msg += tr("\nCe n'est pas très efficace...",
                                "\nIt's not very effective...");
                dmg += d;
            }
            if (hits > 1 && dmg > 0)
                b.msg += std::string("\n") + tr("Touché ", "Hit ") + std::to_string(hits) +
                    tr(" fois !", " time(s)!");
        } else if (mv.cls != 2 || mv.power) {
            // dégâts fixes des moves "pow=0" gérés dans apply_effect
        }
        // moves de statut : immunité de type (Cage-Éclair vs Sol, etc.)
        if (!mv.power && mv.cls == 2 && mv.effect != 0 &&
            mv.range != RANGE_USER && mv.range != RANGE_USER_SIDE &&
            type_mul(mv.type, df) == 0) {
            b.msg += tr("\nÇa n'affecte pas ", "\nIt doesn't affect ") +
                battle_species_name(db, df.species) + "...";
        } else
            apply_effect(b, db, at, df, mv, dmg, side, tbids[ti], sec);
        if (df.fainted()) {
            anyfaint = 1;
            b.msg += "\n" + battle_species_name(db, df.species) +
                     (tside ? tr(" est K.O. !", " fainted!")
                            : tr(" ennemi est K.O. !", " fainted!"));
        }
        // KO adverse -> XP + EVs aux participants (DS : au moment du KO)
        if (df.fainted() && tside) {
            int np = 0;
            for (int i = 0; i < (int)b.me.size() && i < 8; i++)
                if ((b.me_part >> i) & 1) np++;
            if (!np) np = 1;
            int each = std::max(1, battle_exp_gain(db, df, b.trainer, false) / np);
            int evY = 0; uint8_t st6[6]; int a2, b2;
            personal(db, df.species, st6, &a2, &b2, nullptr, &evY);
            for (int i = 0; i < (int)b.me.size() && i < 8; i++) {
                if (!((b.me_part >> i) & 1) || b.me[i].fainted()) continue;
                BatMon& mm = b.me[i];
                int gain = each;
                std::string nm2 = battle_species_name(db, mm.species);
                int lv0 = mm.level;
                // EVs du vaincu (caps 255/510)
                int tot = 0; for (int k = 0; k < 6; k++) tot += mm.evs[k];
                for (int k = 0; k < 6 && tot < 510 && mm.level < 100; k++) {
                    int y = (evY >> (k * 2)) & 3;
                    int add = std::min(y, std::min(255 - mm.evs[k], 510 - tot));
                    if (add > 0) { mm.evs[k] += add; tot += add; }
                }
                // XP (divisée entre participants)
                if (mm.level < 100) {
                    mm.exp = std::min(mm.exp + (uint32_t)gain,
                                      mon_exp_for(db, mm.species, 100));
                    b.msg += "\n" + nm2 + tr(" a gagné ", " gained ") +
                        std::to_string(gain) + tr(" points d'EXP. !", " EXP. Points!");
                }
                // montées de niveau (un cran à la fois)
                while (mm.level < 100 &&
                       mm.exp >= mon_exp_for(db, mm.species, mm.level + 1)) {
                    mm.level++;
                    mon_recalc(db, mm);
                    mm.friendship = std::min(255, mm.friendship +
                        (mm.friendship < 100 ? 5 : mm.friendship < 200 ? 3 : 2));
                    b.msg += "\n" + nm2 + tr(" passe au N. ", " grew to Lv. ") +
                        std::to_string(mm.level) + " !";
                    int nw[8]; int nn = mon_learnset_at(db, mm.species, mm.level, nw);
                    for (int j = 0; j < nn; j++) {
                        bool known = false;
                        for (int k = 0; k < mm.nmoves; k++)
                            if (mm.moves[k] == nw[j]) known = true;
                        if (known || mm.nmoves >= 4) continue;
                        mm.moves[mm.nmoves] = nw[j];
                        mm.pp[mm.nmoves] = battle_move(db, nw[j]).pp;
                        mm.nmoves++;
                        b.msg += "\n" + nm2 + tr(" apprend ", " learned ") +
                            battle_move_name(db, nw[j]) + " !";
                    }
                }
                (void)lv0;
            }
            b.anim += 0.6f;
        }
        }                                          // fin boucle cibles
        b.anim = anyfaint ? (b.anim > 1.2f ? b.anim : 1.6f) : 1.1f;
        if (getenv("BTLDBG") && !b.msg.empty())
            printf("   [msg] %s\n", b.msg.c_str());
    };
    switch (b.st) {
    case Battle::TRANS:                       // rencontre : flash 22t + slice 13t + noir 1t
        b.anim -= dt;
        if (b.anim <= 0 || key == 'E') { b.st = Battle::INTRO; b.anim = 1.2f; }
        break;
    case Battle::INTRO:
        b.anim -= dt;
        if (b.anim <= 0 || key == 'E') { b.st = Battle::CHOICE; b.sel = 0;
            b.pick = 0; b.msg.clear(); }
        break;
    case Battle::CHOICE: {                    // 0=Attaque 1=Sac / 2=Pokémon 3=Fuite
        // doubles : on commande d'abord le slot `pick` puis le suivant
        BatMon* cur = b.bmon(Battle::bid(0, b.pick));
        if (!cur || cur->fainted()) {           // slot mort/vide : suivant
            b.sel_mv[b.pick] = -2;              // hors file
            next_pick(); break;
        }
        // layout DS : ATTAQUE large en haut, SAC/POKéMON/FUITE ligne du bas
        if (key == 'W') b.sel = 0;
        else if (key == 'S' && b.sel == 0) b.sel = 2;   // haut -> milieu du bas
        else if (key == 'A' || key == 'D') {
            int dd = key == 'D' ? 1 : -1;
            if (b.sel == 0) b.sel = dd > 0 ? 3 : 1;     // coins du bas
            else { int s = b.sel + dd; b.sel = s < 1 ? 1 : (s > 3 ? 3 : s); }
        }
        if (key == 'Q' && b.dbl && b.pick == 1) { // annuler -> re-commander P1
            b.pick = 0; b.sel = 0; b.sel_mv[0] = b.sel_tg[0] = -1; break;
        }
        if (key != 'E') break;
        if (b.sel == 0) { b.st = Battle::PICKMOVE; b.sel = 0; }
        else if (b.sel == 3) {
            if (b.trainer) { b.msg = tr("Impossible de fuir un combat de Dresseur !",
                "No! There's no running from a Trainer battle!"); b.anim = 0.9f; b.st = Battle::EXEC; b.qn = b.qi = 0; }
            else {
                b.sel_mv[b.pick] = -1;              // fuite : action du slot
                b.sel_tg[b.pick] = -1;
                next_pick();
            }
        } else if (b.sel == 1) {
            if (b.dbl && !b.trainer && b.foes_out() > 1) {
                // DS : impossible de viser avec deux sauvages de front
                b.msg = tr("Il y a deux Pokémon !\nImpossible de viser !",
                    "There are two Pokémon!\nCan't aim!"); b.anim = 0.9f;
                b.st = Battle::EXEC; b.qn = b.qi = 0;
            } else { b.st = Battle::BAG; b.sel = 0; }  // liste des Balls (main.cpp)
        } else {                               // Pokémon : switch du slot pick
            int nx = next_bench(b, 0, b.pick);
            if (nx < 0) { b.msg = tr("Aucun autre Pokémon !", "No other Pokémon!"); b.anim = 0.9f;
                b.st = Battle::EXEC; b.qn = b.qi = 0; }
            else {
                b.sel_mv[b.pick] = -4;            // switch, arg = index équipe
                b.sel_tg[b.pick] = nx;
                next_pick();
            }
        }
        break; }
    case Battle::PICKMOVE: {                  // grille 2x2 comme sur DS
        BatMon* cur = b.bmon(Battle::bid(0, b.pick));
        bool nopp = true;                              // Lutte si tout PP à 0
        for (int k = 0; cur && k < cur->nmoves; k++) if (cur->pp[k] > 0) { nopp = false; break; }
        if (!cur || cur->nmoves == 0) nopp = true;
        if (nopp) { b.sel_mv[b.pick] = -3; b.sel_tg[b.pick] = -1;
                    next_pick(); break; }
        if (key == 'W' && b.sel >= 2) b.sel -= 2;
        else if (key == 'S' && b.sel < 2 && b.sel + 2 < cur->nmoves)
            b.sel += 2;
        else if (key == 'A' && (b.sel & 1)) b.sel -= 1;
        else if (key == 'D' && !(b.sel & 1) && b.sel + 1 < cur->nmoves)
            b.sel += 1;
        if (key == 'Q') { b.st = Battle::CHOICE; b.sel = 0; break; }
        if (key == 'E') {
            if (b.sel < cur->nmoves && cur->pp[b.sel] > 0) {
                b.sel_mv[b.pick] = b.sel;
                MoveInfo mi2 = battle_move(db, cur->moves[b.sel]);
                if (b.dbl && tgt_pickable(mi2.range)) {   // choisir la cible
                    b.tsel = def_tsel(b, b.pick, mi2.range);
                    b.st = Battle::PICKTGT; break;
                }
                b.sel_tg[b.pick] = ai_target(b, 0, mi2.range);
                next_pick();
            } else { b.msg = tr("Plus de PP !", "No PP left!"); b.anim = 0.5f; }
        }
        break; }
    case Battle::PICKTGT: {                   // choix de la cible (doubles)
        BatMon* cur2 = b.bmon(Battle::bid(0, b.pick));
        MoveInfo mi2 = battle_move(db, cur2->moves[b.sel_mv[b.pick]]);
        int t[4], tn = tgt_list(b, 0, mi2.range, t);  // cibles valides
        if (!tn) { b.st = Battle::PICKMOVE; break; }
        int ci = 0;
        for (int i = 0; i < tn; i++) if (t[i] == b.tsel) ci = i;
        if (key == 'A' || key == 'D' || key == 'W' || key == 'S') {
            int dd = (key == 'D' || key == 'S') ? 1 : tn - 1;
            b.tsel = t[(ci + dd) % tn];
        }
        if (key == 'Q') { b.st = Battle::PICKMOVE; break; }
        if (key == 'E') {
            b.sel_tg[b.pick] = b.tsel;
            next_pick();
        }
        break; }
    case Battle::BAG:                          // le picker Balls vit dans main
        if (key == 'Q') { b.st = Battle::CHOICE; b.sel = 0; }
        break;
    case Battle::THROW: {                      // lancer de Ball (ball, shakes)
        b.anim -= dt;
        if (b.tphase == 0) {                   // « Vous lancez une X Ball ! »
            if (b.anim <= 0 || key == 'E') { b.tphase = 1; b.anim = 0.5f; }
        } else if (b.tphase == 1) {            // secousses DS (une par 0.55s)
            if (b.anim <= 0) {
                if (b.shakes > 0) { b.shakes--; b.anim = 0.55f; }
                else {                          // fini : résultat
                    b.tphase = 2; b.anim = 0.3f;
                    if (b.caught) {
                        BatMon* w = b.bmon(first_alive_bid(b, 1));
                        b.msg = tr("Youpi ! ", "Gotcha! ") +
                            battle_species_name(db, w ? w->species : 0) +
                            tr(" a été capturé !", " was caught!");
                        if (b.caught_box >= 0)
                            b.msg += "\n\n" +
                                battle_species_name(db, w ? w->species : 0) +
                                tr(" a été envoyé à la\nBoîte ",
                                   " was sent to\nBox ") +
                                std::to_string(b.caught_box + 1) + " !";
                    }
                    else
                        b.msg = b.shake_max >= 3
                          ? tr("Zut ! Il était presque capturé !",
                            "Shoot! It was so close!")
                          : tr("Le Pokémon s'est échappé !",
                            "The Pokémon broke free!");
                }
            }
            if (key == 'E' && b.anim < 0.2f) { b.anim = 0; }   // pas de skip mi-secousse
        } else {                               // résultat : A pour continuer
            if (b.anim <= 0 && key == 'E') {
                if (b.caught) { b.st = Battle::WIN; b.won = 1; b.anim = 0.5f; }
                else {                          // raté : action consommée,
                    b.sel_mv[b.pick] = -2;          // le tour continue
                    next_pick();                    // (ennemis attaquent)
                    b.anim = 0.9f;
                }
            }
        }
        break; }
    case Battle::EXEC:
        if (b.anim > 0) { b.anim -= dt; break; }
        if (b.qi < b.qn) { act(); break; }
        if (b.st != Battle::EXEC) break;                  // Roar/Teleport a clos
        // ── résidus de fin de tour (DS : brûlure, poison, toxique,
        //    vampigraine, ligotage, racines) — avant les checks de KO ──
        if (b.eor) goto kocheck;                          // déjà passés (remplacement)
        b.eor = 1;
        for (int bd = 0; bd < 4; bd++) {
            BatMon* mp = b.bmon(bd);
            if (!mp || mp->fainted()) continue;
            BatMon& m = *mp;
            int s = Battle::bid_side(bd);
            BatMon* op = b.bmon(first_alive_bid(b, s ^ 1));
            std::string mnm = battle_species_name(db, m.species);
            if (m.cond & 0x80) {                          // poison grave n/16
                int d = std::max(1, m.maxhp * m.toxic_n / 16);
                m.toxic_n = std::min(15, m.toxic_n + 1);
                m.hp = std::max(0, m.hp - d);
                b.msg += "\n" + mnm + tr(" souffre du poison !", " is hurt by poison!");
            } else if (m.cond & 0x08) {                   // poison 1/8
                m.hp = std::max(0, m.hp - std::max(1, m.maxhp / 8));
                b.msg += "\n" + mnm + tr(" souffre du poison !", " is hurt by poison!");
            } else if (m.cond & 0x10) {                   // brûlure 1/8 (Gen4)
                m.hp = std::max(0, m.hp - std::max(1, m.maxhp / 8));
                b.msg += "\n" + mnm + tr(" souffre de sa brûlure !", " is hurt by its burn!");
            }
            if (m.seeded && op && !op->fainted() && m.hp > 0) { // Vampigraine 1/8
                int d = std::max(1, m.maxhp / 8);
                m.hp = std::max(0, m.hp - d);
                op->hp = std::min(op->maxhp, op->hp + d);
                b.msg += "\n" + mnm + tr(" est vampirisé !", "'s health is sapped!");
            }
            if (m.bound && m.hp > 0) {                    // ligotage 1/16
                m.bound--;
                m.hp = std::max(0, m.hp - std::max(1, m.maxhp / 16));
            }
            if (m.ingrain)                                // Racines +1/16
                m.hp = std::min(m.maxhp, m.hp + std::max(1, m.maxhp / 16));
            // résidus météo : grêle (sauf Glace=15), sable (sauf Sol4/Roche5/Acier8)
            if (b.weather == BW_HAIL && m.t1 != 15 && m.t2 != 15) {
                m.hp = std::max(0, m.hp - std::max(1, m.maxhp / 16));
                b.msg += "\n" + mnm + tr(" subit la grêle !",
                    " is buffeted by the hail!");
            }
            if (b.weather == BW_SAND &&
                m.t1 != 4 && m.t2 != 4 && m.t1 != 5 && m.t2 != 5 &&
                m.t1 != 8 && m.t2 != 8) {
                m.hp = std::max(0, m.hp - std::max(1, m.maxhp / 16));
                b.msg += "\n" + mnm + tr(" subit la tempête de sable !",
                    " is buffeted by the sandstorm!");
            }
            if (m.drowsy && !--m.drowsy && !m.cond && !m.fainted()) {
                m.cond = 2 + rand() % 3;                  // Bâillement → dort 1-3 t (DS)
                b.msg += "\n" + mnm + tr(" s'endort !", " fell asleep!");
            }
            // écrans : décrément (nibble bas = phys, haut = spé) — par camp
            if (Battle::bid_slot(bd) == 0) {
                int sc = b.screen[s];
                if (sc & 0x0F) sc = (sc & 0xF0) | ((sc & 0x0F) - 1);
                if (sc & 0xF0) sc = (sc & 0x0F) | ((sc & 0xF0) - 0x10);
                b.screen[s] = sc;
            }
        }
        // fin du round : résolution des K.O. par slot
        kocheck:
        for (int s = 0; s < 2; s++) {                    // slot adverse K.O. ?
            if (alive_idx(b, 1, s) < 0 &&
                b.act_idx(1, s) >= 0) {                  // slot occupé mais mort
                int nx = next_bench(b, 1, s);
                if (nx >= 0) {
                    if (s) b.fi2 = nx; else b.fi = nx;
                    if (getenv("BTLDBG"))
                        printf("   [swap] E%d <- foes[%d]\n", s + 1, nx);
                    if (!b.msg.empty()) b.msg += "\n";
                    b.msg += tr("L'ennemi envoie ", "The foe sent out ") +
                        battle_species_name(db, b.foes[nx].species) + "!";
                    b.anim = 1.0f; b.qn = b.qi = 0;
                }
            }
        }
        if (b.foes_out() == 0) {                          // plus rien en face
            b.st = Battle::WIN; b.won = 1;
            b.msg = b.trainer ? tr("Vous avez gagné le combat !", "You won the battle!")
                              : tr("Vous avez battu le Pokémon sauvage !", "You defeated the wild Pokémon!");
            b.anim = 0.5f; break;
        }
        for (int s = 0; s < 2; s++) {                    // slot joueur K.O. ?
            if (alive_idx(b, 0, s) < 0 && b.act_idx(0, s) >= 0) {
                int nx = next_bench(b, 0, s);
                if (nx >= 0) {
                    if (s) b.mi2 = nx; else b.mi = nx;
                    b.me_part |= 1 << nx;
                    if (getenv("BTLDBG"))
                        printf("   [swap] P%d <- me[%d]\n", s + 1, nx);
                    if (!b.msg.empty()) b.msg += "\n";
                    b.msg += tr("En avant, ", "Go, ") +
                        battle_species_name(db, b.me[nx].species) + "!";
                    b.anim = 1.0f; b.qn = b.qi = 0;
                }
            }
        }
        if (b.me_out() == 0) {                           // plus rien chez nous
            b.st = Battle::LOSE; b.won = -1;
            b.msg = tr("Tous vos Pokémon sont K.O. !", "All your Pokémon fainted!");
            b.anim = 0.5f; break;
        }
        if (b.anim > 0) break;                            // un remplacement joue
        b.st = Battle::CHOICE; b.sel = 0; b.pick = 0; b.msg.clear();
        b.sel_mv[0] = b.sel_mv[1] = -1;
        b.sel_tg[0] = b.sel_tg[1] = -1;
        break;
    case Battle::WIN: case Battle::LOSE: case Battle::FLEE:
        b.anim -= dt;
        if (b.anim <= 0 && (key == 'E' || key == ' ' || b.anim < -30))
            b.active = false;
        if (key == 'E') b.active = false;
        break;
    default: break;
    }
}

// ── progression : XP, stats EV/nature, montées de niveau, évolution ───────────
// Sémantique reprise du Gen4 (pokeplatinum pokemon.c / scrcmd_exp.c) — mêmes
// formules entières et ordre des boosts que la DS.

static const uint32_t* growtbl(Nds* db, int grow) {
    static uint32_t T[8 * 101]; static bool loaded = false;
    if (!loaded) {
        loaded = true;
        auto n = db->narc("/poketool/personal/pl_growtbl.narc");
        if (n) for (int t = 0; t < 8 && t < n->count(); t++) {
            const Bytes& d = n->member(t);
            for (int i = 0; i < 101 && i * 4 + 4 <= (int)d.size(); i++)
                T[t * 101 + i] = u32(d, i * 4);
        }
    }
    if (grow < 0 || grow > 7) grow = 0;
    return &T[grow * 101];
}

uint32_t mon_exp_for(Nds* db, int sp, int lv) {
    int g = 0; uint8_t st[6]; int a, b;
    personal(db, sp, st, &a, &b, nullptr, nullptr, &g);
    if (lv < 1) lv = 1;
    if (lv > 100) lv = 100;
    return growtbl(db, g)[lv - 1];
}

void mon_recalc(Nds* db, BatMon& m) {
    uint8_t st[6]; int t1, t2;
    if (!personal(db, m.species, st, &t1, &t2)) return;
    int nat = (int)(m.pid % 25), old = m.maxhp;
    // DS : 6 IVs indépendants (MON_DATA_IVS, 5 bits/stat)
    m.maxhp = stat_hp(st[0], m.level, iv_at(m.ivs, 0), m.evs[0]);
    m.atk = stat_st(st[1], m.level, iv_at(m.ivs, 1), m.evs[1], nat, 0);
    m.def = stat_st(st[2], m.level, iv_at(m.ivs, 2), m.evs[2], nat, 1);
    m.spe = stat_st(st[3], m.level, iv_at(m.ivs, 3), m.evs[3], nat, 2);
    m.spa = stat_st(st[4], m.level, iv_at(m.ivs, 4), m.evs[4], nat, 3);
    m.spd = stat_st(st[5], m.level, iv_at(m.ivs, 5), m.evs[5], nat, 4);
    m.hp += m.maxhp - old;                             // le gain de PV reste
    if (m.hp > m.maxhp) m.hp = m.maxhp;
    if (m.hp < 0) m.hp = 0;
}

// expGain : floor(baseExp × niv / 7), puis boosts floorés dans l'ordre
// (dresseur ×1.5, étranger ×1.5). Œuf Chance non géré (pas d'objet tenu).
int battle_exp_gain(Nds* db, const BatMon& foe, bool trainer, bool outsider) {
    int be = 0; uint8_t st[6]; int a, b;
    personal(db, foe.species, st, &a, &b, &be);
    int e = be * foe.level / 7;
    if (trainer) e = e * 3 / 2;
    if (outsider) e = e * 3 / 2;
    return e < 1 ? 1 : e;
}

// learnset wotbl : entries u16 (move:9 | niveau:7), 0xFFFF = fin. Retourne les
// moves appris exactement au niveau lv.
int mon_learnset_at(Nds* db, int sp, int lv, int out[8]) {
    int n = 0;
    auto n0 = db->narc(P_WOTBL);
    if (!n0 || sp < 0 || sp >= n0->count()) return 0;
    const Bytes& d = n0->member(sp);
    for (size_t i = 0; i + 1 < d.size() && n < 8; i += 2) {
        uint16_t e = u16(d, i);
        if (e == 0xFFFF) break;
        if ((e >> 9) == lv) { int mv = e & 0x1FF; if (mv) out[n++] = mv; }
    }
    return n;
}

// genre depuis PID vs taux de genre (byte 0x10 ; 0=mâle, 254=femelle, 255=asexué)
int mon_gender(Nds* db, const BatMon& m) {            // 0=mâle 1=femelle -1=aucun
    auto n = db->narc(P_PERSONAL);
    if (!n || m.species >= n->count()) return -1;
    const Bytes& d = n->member(m.species);
    int r = d.size() > 0x10 ? d[0x10] : 0;
    if (r == 0xFF) return -1;
    if (r == 0xFE) return 1;
    if (r == 0) return 0;
    return (int)(m.pid % 256) < r ? 1 : 0;             // DS : pid%256 < ratio -> femelle
}

// evo.narc : 7 × {u16 méthode, u16 param, u16 cible} (EVO_* ds evolution).
// Trigger montée de niveau (post-combat) : méthodes niveau/bonheur/seuil/PID/
// genre. Pierre Stase non gérée (pas d'objet tenu).
int mon_evolution_target(Nds* db, const BatMon& m, bool night) {
    auto n = db->narc("/poketool/personal/evo.narc");
    if (!n || m.species <= 0 || m.species >= n->count()) return 0;
    const Bytes& d = n->member(m.species);
    if (d.size() < 42) return 0;
    int gen = mon_gender(db, m);
    bool happy = m.friendship >= 220;                  // EVOLVE_FRIENDSHIP
    for (int i = 0; i < 7; i++) {
        int meth = u16(d, i * 6), param = u16(d, i * 6 + 2), tgt = u16(d, i * 6 + 4);
        if (!meth) break;
        bool lvl = m.level >= param, ok = false;
        switch (meth) {
        case 1:  ok = happy; break;                    // bonheur
        case 2:  ok = happy && !night; break;          // bonheur jour
        case 3:  ok = happy && night; break;           // bonheur nuit
        case 4:  ok = lvl; break;                      // niveau
        case 8:  ok = lvl && m.atk > m.def; break;     // Atk>Def (niveau)
        case 9:  ok = lvl && m.atk == m.def; break;
        case 10: ok = lvl && m.atk < m.def; break;
        case 11: ok = lvl && (m.pid >> 16) % 10 < 5; break;   // PID bas
        case 12: ok = lvl && (m.pid >> 16) % 10 >= 5; break;  // PID haut
        case 20: for (int k = 0; k < m.nmoves; k++)    // connaît le move param
                     if (m.moves[k] == param) ok = true;
                 break;
        case 22: ok = lvl && gen == 0; break;          // mâle
        case 23: ok = lvl && gen == 1; break;          // femelle
        default: break;                                // pierre/échange/lieu/beauté…
        }
        if (ok && tgt) return tgt;                     // 1er match dans l'ordre
    }
    return 0;
}
