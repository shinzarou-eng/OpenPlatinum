// banim.h — animations d'attaques DS authentiques (we.arc + waza_particle).
// Interprète les scripts BTLANIMCMD de la ROM et simule les systèmes de
// particules SPL (.spa) dans la scène de combat ImGui.
#pragma once
#include <cstdint>
struct ImDrawList;
class Battle;

// lance le script d'anim du move (id pl_waza_tbl) depuis attacker vers defender
// (battler ids DS : 0=P1 1=E1 2=P2 3=E2). Sans effet si assets absents.
void banim_start(Battle& b, int move, int attacker, int defender);
// avance la VM + les particules (dt en secondes)
void banim_update(Battle& b, float dt);
// particules chargées / script en cours
bool banim_busy();
void banim_stop();
// dessine les particules. feet[bid] = position des pieds du battler en pixels
// écran, uds = échelle (px écran par px DS, ~io.x/256)
void banim_draw(ImDrawList* dl, const float feet[4][2], float uds);
// accesseurs effets battler (bid 0-3) — utilisés par le rendu de la scène
extern "C" {
float banim_bdx(int i);
float banim_bdy(int i);
float banim_bsx(int i);
float banim_bsy(int i);
float banim_balp(int i);
int   banim_bhid(int i);
int   banim_bblink(int i);
float banim_flash(int i, uint8_t c[3]);
}
