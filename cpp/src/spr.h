// spr.h — sprites 2D DS : NCLR (palettes BGR555), NCGR (tuiles 4bpp/8bpp),
// NCER (cells OAM) -> Image RGBA. Réutilisé pour les icônes de menu.
#pragma once
#include "common.h"
#include "bmd.h"
#include <memory>
#include <vector>

namespace spr {

struct Ncgr {
    int bpp = 4, ntiles = 0;
    std::vector<uint8_t> tiles;             // 32B/tuile (4bpp) ou 64B (8bpp)
};

struct Oam {
    int x = 0, y = 0, tile = 0, pal = 0;
    int w = 8, h = 8;
    bool fh = false, fv = false;
};

struct Cell {
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    std::vector<Oam> oams;
};

std::vector<std::vector<RGBAf>> parse_nclr(const Bytes& d);
Ncgr parse_ncgr(const Bytes& d);
std::vector<Cell> parse_ncer(const Bytes& d);

// pixels d'une tuile 8x8 -> indices palette (64 valeurs, ligne-major)
const uint8_t* tile_px(const Ncgr& g, int t);

// compose une cell NCER en Image RGBA (fond transparent)
// tile64 : indices OAM en unités de 64 octets (sprites combat -mappingtype 64)
std::shared_ptr<Image> render_cell(const Ncgr& g, const Cell& c,
    const std::vector<std::vector<RGBAf>>& pals, int pal_ovr = -1,
    bool tile64 = false);

// fond DS "FillTilemapRect" : la même tuile répétée tx*ty fois
std::shared_ptr<Image> compose_rep(const Ncgr& g, int tile, int tx, int ty,
    const std::vector<RGBAf>& pal, bool transparent0 = false);
// compose wxh px de tuiles consécutives (grille tx x ty) depuis tile0
std::shared_ptr<Image> compose(const Ncgr& g, int tile0, int tx, int ty,
    const std::vector<RGBAf>& pal, bool transparent0 = true);

// écran NSCR/RCSN : grille w/8 x h/8 d'entrées u16
// (tile:10 fh:1 fv:1 pal:4) -> Image RGBA, w x h lu dans l'en-tête
// skip0 : rend transparents les pixels d'index 0 (fond des logos)
std::shared_ptr<Image> render_screen(const Bytes& scr, const Ncgr& g,
    const std::vector<std::vector<RGBAf>>& pals, bool skip0 = false);

// ── police Game Freak "NFGR" (pl_font.narc) ─────────────────────────────────
// u32 taille_tete, u32 ofs_table_largeurs, u32 nb_glyphes,
// u8 maxW, u8 maxH, u8 tuilesW, u8 tuilesH ; glyphes = tuiles 8x8 2bpp
// (16 B/tuile, octet pair = px 4-7, impair = px 0-3, fort = gauche).
// px : 0 transparent, 1 avant-plan, 2 ombre, 3 fond.
struct Nfgr {
    int nglyphs = 0, maxW = 0, maxH = 0, cellW = 0, cellH = 0;
    std::vector<uint8_t> widths;            // largeur d'avance par glyphe
    std::vector<uint8_t> pixels;            // glyphe-major : g*cellW*cellH + y*cellW + x
    const uint8_t* glyph_px(int g) const;
    int glyph_w(int g) const;
};

Nfgr parse_nfgr(const Bytes& d);

// atlas 16 glyphes/ligne, encres fg/ombre personnalisables
std::shared_ptr<Image> font_atlas(const Nfgr& f, RGBAf fg, RGBAf sh, int ss = 1);

} // namespace spr
