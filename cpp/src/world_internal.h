// world_internal.h — helpers partagés entre les unités world_*.cpp
// (extraits de l'ancien world.cpp monolithique ; internes au module World)
#pragma once
#include <cstdint>
#include <cmath>

static inline int64_t ckey(int x, int y) { return ((int64_t)x << 32) | (uint32_t)y; }

// comportements de tuile liés aux warps (map_tile_behavior.h DS)
enum {
    TB_STAIRS_E = 0x5E, TB_STAIRS_W = 0x5F,
    TB_ENTRANCE_E = 0x62, TB_ENTRANCE_W = 0x63,
    TB_ENTRANCE_N = 0x64, TB_ENTRANCE_S = 0x65,
    TB_WARP_PANEL = 0x67, TB_DOOR = 0x69,
    TB_ESCALATOR_FLIP = 0x6A, TB_ESCALATOR = 0x6B,
    TB_WARP_E = 0x6C, TB_WARP_W = 0x6D, TB_WARP_N = 0x6E, TB_WARP_S = 0x6F,
    TB_ICE = 0x20,                              // glace : glissière forcée
};
// phases de la task de transition ; types : fondu simple / porte / escaliers /
// escalator / panneau warp (spin DS de 20 ticks + montée/descente)
enum { WP_DEP_WALK, WP_DEP_FADE, WP_LOAD, WP_ARR, WP_ARR_WALK,
       WP_DEP_DOOR, WP_ARR_DOOR, WP_DEP_PANEL, WP_ARR_PANEL };
enum { WT_PLAIN, WT_DOOR, WT_STAIRS = 3, WT_ESCAL, WT_PANEL };

// dirs script : 0=S(+z) 1=N(-z) 2=O(-x) 3=E(+x)
static inline void dirv(int d, int& dx, int& dz) {
    dx = d == 2 ? -1 : d == 3 ? 1 : 0;
    dz = d == 0 ? 1 : d == 1 ? -1 : 0;
}
// ROM dir -> monde : 0=N->1, 1=S->0, 2=O->2, 3=E->3
static const int R2W[4] = {1, 0, 2, 3};

// Points de réapparition (spawn_locations.c DS : sSpawnLocations).
// blackout : tuile + FACE_UP ; fly : coords px monde + FACE_DOWN.
// L'entrée sur un header isWarpPos mémorise l'index (field_map_change.c :
// GetMapBlackOutWarpId -> SetBlackOutWarpId).
struct SpawnLoc { int blackout_hdr, bx, bz, fly_hdr, fx, fz, isWarpPos; };
static const SpawnLoc SPAWN_TBL[] = {
    {414, 0x8, 0x8,   411, 0x74, 0x376, 1},   // maison du joueur / Bonaugure
    {420, 0x8, 0x6,   418, 0xB1, 0x34B, 1},   // PC Féli-Ville
    {428, 0x8, 0x6,   426, 0xB0, 0x29B, 1},   // PC Floraville
    {435, 0x8, 0x6,   433, 0x236, 0x291, 1},  // PC Bonville
    {443, 0x8, 0x6,   442, 0x1D8, 0x21B, 1},  // PC Célestia
    {6,   0x8, 0x6,   3,   0xB4, 0x309, 1},   // PC Féli-Cité
    {36,  0x8, 0x6,   33,  0x3A, 0x2D3, 1},   // PC Joliberges
    {48,  0x8, 0x6,   45,  0x12F, 0x2F5, 1},  // PC Charbourg
    {69,  0x8, 0x6,   65,  0x131, 0x213, 1},  // PC Vestigion
    {101, 0x8, 0x6,   86,  0x1D1, 0x2BA, 1},  // PC Unionpolis
    {123, 0x8, 0x6,   120, 0x258, 0x330, 1},  // PC Verchamps
    {134, 0x8, 0x6,   132, 0x2CD, 0x264, 1},  // PC Voilaroc
    {151, 0x8, 0x6,   150, 0x35C, 0x311, 1},  // PC Rivamar
    {168, 0x8, 0x6,   165, 0x17B, 0xEA, 1},   // PC Frimapic
    {173, 0x8, 0x6,   172, 0x34A, 0x257, 1},  // PC Ligue (sud)
    {189, 0x8, 0x6,   188, 0x287, 0x1AE, 1},  // PC Aire de Combat
    {452, 0x8, 0x6,   450, 0x293, 0x153, 1},  // PC Aire de Survie
    {459, 0x8, 0x6,   457, 0x322, 0x1D9, 1},  // PC Aire de Détente
    {393, 0x8, 0x6,   392, 0x132, 0x38E, 0},  // Parc des Amis (hall) / R221
    {175, 0x4, 0x3,   172, 0x34F, 0x230, 1},  // PC Ligue (nord)
};
static const int SPAWN_TBL_N = sizeof(SPAWN_TBL) / sizeof(SPAWN_TBL[0]);

// index spawn (1..N) dont le point de blackout est ce header, 0 sinon
static inline int spawn_blackout_id(int hdr) {
    for (int i = 0; i < SPAWN_TBL_N; i++)
        if (SPAWN_TBL[i].blackout_hdr == hdr && SPAWN_TBL[i].isWarpPos)
            return i + 1;
    return 0;
}
