// bw_sprites.h — sprites Pokémon style NB (images/ : front.gif animés,
// back.png statiques) préférés aux sprites pl_pokegra quand présents.
#pragma once
#include <cstdint>
#include <vector>
#include <string>

struct BWSprite {
    bool ok = false;
    int w = 0, h = 0;
    std::vector<uint32_t> frames;     // textures GL de chaque frame GIF
    std::vector<int> cum_ms;          // début de chaque frame (ms)
    int total_ms = 0;                 // durée boucle (0 = image fixe)
    int feet = 0;                     // ligne opaque la plus basse (px)
    int top = 0;                      // ligne opaque la plus haute (px)
    int cx = 0;                       // centre x opaque (px)
};

// indice de frame à l'instant t (ms) ; images fixes -> 0
int bw_frame_at(const BWSprite& s, uint32_t t_ms);

// sprite NB pour (espèce, face 0=avant/1=dos, shiny, genre -1/0/1) ;
// nullptr si le set ne couvre pas cette espèce — l'appelant retombe
// alors sur pl_pokegra.
const BWSprite* bw_load(int sp, int face, int shiny = 0, int gender = -1);
