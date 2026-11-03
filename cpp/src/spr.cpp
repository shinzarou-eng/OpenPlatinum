// spr.cpp — décodeurs NCLR/NCGR/NCER (formats NNS G2D).
#include "spr.h"

namespace spr {

// ── LZ10 : membre compressé (0x10 + taille 24b) -> données brutes ──────────────
Bytes unlz(const Bytes& d) {
    if (d.size() < 5 || d[0] != 0x10) return d;
    uint32_t sz = d[1] | (d[2] << 8) | (d[3] << 16);
    if (!sz) { // en-tête étendu : taille 32b à +4
        if (d.size() < 8) return d;
        sz = u32(const_cast<Bytes&>(d), 4);
    }
    size_t start = (d[1] || d[2] || d[3]) ? 4 : 8;
    Bytes out;
    out.reserve(sz);
    size_t i = start;
    while (i < d.size() && out.size() < sz) {
        uint8_t f = d[i++];
        for (int b = 0; b < 8 && out.size() < sz; b++) {
            if (f & (0x80 >> b)) {
                if (i + 1 >= d.size()) break;
                int n2 = (d[i] >> 4) + 3;
                int off = (((d[i] & 15) << 8) | d[i + 1]) + 1;
                i += 2;
                if ((size_t)off > out.size()) return out;   // pas du LZ
                for (int k = 0; k < n2; k++)
                    out.push_back(out[out.size() - off]);
            } else {
                if (i >= d.size()) break;
                out.push_back(d[i++]);
            }
        }
    }
    return out;
}

// ── NCLR : 'RLCN' + section 'TTLP' -> palettes BGR555 ──────────────────────────
std::vector<std::vector<RGBAf>> parse_nclr(const Bytes& d0) {
    Bytes dd = unlz(d0);
    const Bytes& d = dd;
    std::vector<std::vector<RGBAf>> out;
    if (d.size() < 0x30) return out;
    size_t p = 0x10;
    // cherche la section TTLP/PLTT
    while (p + 8 <= d.size()) {
        uint32_t tag = u32(const_cast<Bytes&>(d), p);
        uint32_t sz = u32(const_cast<Bytes&>(d), p + 4);
        if ((tag == 0x54544C50 || tag == 0x504C5454) && sz >= 0x10) { // TTLP|PLTT
            const uint8_t* b = d.data() + p + 8;
            // en-tête 16B : u32 depth, u32 reserved, u32 dataLen, u32 palSz
            uint32_t dataLen = u32(const_cast<Bytes&>(d), p + 8 + 8);
            uint32_t palSz = u32(const_cast<Bytes&>(d), p + 8 + 12);
            const uint8_t* pd = b + 0x10;
            if (!palSz) palSz = 16;
            int npal = (int)(dataLen / (palSz * 2));
            if (npal <= 0 || npal > 64) npal = 1;
            for (int i = 0; i < npal; i++) {
                std::vector<RGBAf> pal;
                for (uint32_t c = 0; c < palSz; c++) {
                    size_t o = (size_t)i * palSz * 2 + c * 2;
                    if (pd + o + 1 > d.data() + d.size()) break;
                    pal.push_back(bmd::rgb555(pd[o] | (pd[o + 1] << 8)));
                }
                if (!pal.empty()) out.push_back(std::move(pal));
            }
            return out;
        }
        if (sz < 8) break;
        p += sz;
    }
    return out;
}

// ── NCGR : 'RGCN' + section 'RAHC' -> tuiles 8x8 ───────────────────────────────
Ncgr parse_ncgr(const Bytes& d0) {
    Bytes dd = unlz(d0);
    const Bytes& d = dd;
    Ncgr g;
    if (d.size() < 0x30) return g;
    size_t p = 0x10;
    while (p + 8 <= d.size()) {
        uint32_t tag = u32(const_cast<Bytes&>(d), p);
        uint32_t sz = u32(const_cast<Bytes&>(d), p + 4);
        if (tag == 0x43484152 || tag == 0x52414843) {   // RAHC/CHAR
            // +0: tilesW,H (ff ff si scan), +4: format(3=4bpp,4=8bpp),
            // +16: dataSize, +20: dataOffset (rel. au début du bloc data)
            uint32_t fmt = u32(const_cast<Bytes&>(d), p + 8 + 4);
            uint32_t dataSz = u32(const_cast<Bytes&>(d), p + 8 + 16);
            uint32_t dataOff = u32(const_cast<Bytes&>(d), p + 8 + 20);
            g.bpp = (fmt == 4) ? 8 : 4;
            size_t base = p + 8 + dataOff;
            int tsize = g.bpp == 8 ? 64 : 32;
            if (dataSz == 0 || base + dataSz > d.size())
                dataSz = (uint32_t)(d.size() - base);
            g.ntiles = (int)(dataSz / tsize);
            g.tiles.assign(d.begin() + base, d.begin() + base + dataSz);
            return g;
        }
        if (sz < 8) break;
        p += sz;
    }
    return g;
}

// ── NCER : 'RECN' + 'KBEC' -> cells (records 16B) ──────────────────────────────
std::vector<Cell> parse_ncer(const Bytes& d0) {
    Bytes dd = unlz(d0);
    const Bytes& d = dd;
    std::vector<Cell> out;
    if (d.size() < 0x20) return out;
    size_t p = 0x10;
    while (p + 8 <= d.size()) {
        uint32_t tag = u32(const_cast<Bytes&>(d), p);
        uint32_t sz = u32(const_cast<Bytes&>(d), p + 4);
        if (tag == 0x4345424B || tag == 0x4B424543) {   // CEBK/KBEC
            const uint8_t* b = d.data() + p + 8;
            int ncells = b[0] | (b[1] << 8);
            // en-tête KBEC = 0x18 octets. Deux variantes de records :
            // 16B (nOam,attr,oamOff,bounds i16x4) ou 8B (nOam,attr,oamOff,
            // bounds déduits des OAM). On détecte en validant oamOff.
            auto valid = [&](int rs) {
                if (b + 0x18 + (size_t)ncells * rs > d.data() + d.size())
                    return false;
                size_t oamreg = (d.data() + d.size()) - (b + 0x18 + ncells * rs);
                for (int i = 0; i < ncells; i++) {
                    const uint8_t* cr = b + 0x18 + (size_t)i * rs;
                    int no = cr[0] | (cr[1] << 8);
                    uint32_t oo = cr[4] | (cr[5] << 8) | (cr[6] << 16) |
                        ((uint32_t)cr[7] << 24);
                    if (no > 128 || oo >= oamreg) return false;
                }
                return true;
            };
            int btype = b[2] | (b[3] << 8);     // 1 = records étendus (bounds)
            int recsz = btype == 1 ? 16
                : (btype == 0 ? 8
                   : (valid(16) ? 16 : (valid(8) ? 8 : 16)));
            const uint8_t* cb = b + 0x18;
            const uint8_t* oamb = cb + ncells * recsz;
            static const int SZ[4][3] = {{8,8,8},{16,16,16},{32,32,32},{64,64,64}};
            for (int i = 0; i < ncells; i++) {
                const uint8_t* cr = cb + i * recsz;
                if (cr + recsz > d.data() + d.size()) break;
                Cell c;
                int noam = cr[0] | (cr[1] << 8);
                uint32_t oamo = cr[4] | (cr[5] << 8) | (cr[6] << 16) |
                    ((uint32_t)cr[7] << 24);
                if (recsz == 16) {
                    c.maxx = (int16_t)(cr[8] | (cr[9] << 8));
                    c.maxy = (int16_t)(cr[10] | (cr[11] << 8));
                    c.minx = (int16_t)(cr[12] | (cr[13] << 8));
                    c.miny = (int16_t)(cr[14] | (cr[15] << 8));
                } else {                       // 8B : bounds = union des OAM
                    c.minx = c.miny = 32767; c.maxx = c.maxy = -32768;
                }
                for (int j = 0; j < noam; j++) {
                    const uint8_t* r = oamb + oamo + j * 6;
                    if (r + 6 > d.data() + d.size()) break;
                    int a0 = r[0] | (r[1] << 8);
                    int a1 = r[2] | (r[3] << 8);
                    int a2 = r[4] | (r[5] << 8);
                    Oam o;
                    o.y = a0 & 0xFF; if (o.y > 127) o.y -= 256;
                    o.x = a1 & 0x1FF; if (o.x > 255) o.x -= 512;
                    int sz = (a1 >> 14) & 3, sh = (a0 >> 14) & 3;
                    o.fh = (a1 >> 12) & 1; o.fv = (a1 >> 13) & 1;
                    if (sh == 0) { o.w = o.h = 8 << sz; }
                    else if (sh == 1) { static const int W[4]={16,32,32,64};
                        static const int H[4]={8,8,16,32};
                        o.w = W[sz]; o.h = H[sz]; }
                    else { static const int W[4]={8,8,16,32};
                        static const int H[4]={16,32,32,64};
                        o.w = W[sz]; o.h = H[sz]; }
                    o.tile = a2 & 0x3FF; o.pal = (a2 >> 12) & 0xF;
                    c.oams.push_back(o);
                    if (recsz == 8) {           // bounds = union des OAM
                        if (o.x < c.minx) c.minx = o.x;
                        if (o.y < c.miny) c.miny = o.y;
                        if (o.x + o.w > c.maxx) c.maxx = o.x + o.w;
                        if (o.y + o.h > c.maxy) c.maxy = o.y + o.h;
                    }
                }
                if (recsz == 8 && c.maxx < c.minx)
                    c.minx = c.miny = c.maxx = c.maxy = 0;
                out.push_back(std::move(c));
            }
            return out;
        }
        if (sz < 8) break;
        p += sz;
    }
    return out;
}

const uint8_t* tile_px(const Ncgr& g, int t) {
    int tsize = g.bpp == 8 ? 64 : 32;
    if (t < 0 || t >= g.ntiles) return nullptr;
    return g.tiles.data() + (size_t)t * tsize;
}

static int tile_idx_px(const Ncgr& g, int t, int x, int y) {
    const uint8_t* px = tile_px(g, t);
    if (!px) return 0;
    if (g.bpp == 8) return px[y * 8 + x];
    int b = px[y * 4 + (x >> 1)];
    return (x & 1) ? (b >> 4) : (b & 0xF);
}

// fond DS "FillTilemapRect" : la même tuile répétée tx*ty fois.
std::shared_ptr<Image> compose_rep(const Ncgr& g, int tile, int tx, int ty,
    const std::vector<RGBAf>& pal, bool transparent0) {
    auto im = std::make_shared<Image>();
    im->w = tx * 8; im->h = ty * 8;
    im->px.resize((size_t)im->w * im->h * 4, 0);
    for (int yy = 0; yy < im->h; yy++)
        for (int xx = 0; xx < im->w; xx++) {
            int idx = tile_idx_px(g, tile, xx & 7, yy & 7);
            if (transparent0 && idx == 0) continue;
            if (idx >= (int)pal.size()) idx = 0;
            const RGBAf& c = pal[idx];
            size_t o = ((size_t)yy * im->w + xx) * 4;
            im->px[o + 0] = (uint8_t)c.r;
            im->px[o + 1] = (uint8_t)c.g;
            im->px[o + 2] = (uint8_t)c.b;
            im->px[o + 3] = (uint8_t)c.a;
        }
    return im;
}

std::shared_ptr<Image> compose(const Ncgr& g, int tile0, int tx, int ty,
    const std::vector<RGBAf>& pal, bool transparent0) {
    auto im = std::make_shared<Image>();
    im->w = tx * 8; im->h = ty * 8;
    im->px.resize((size_t)im->w * im->h * 4, 0);
    for (int yy = 0; yy < im->h; yy++)
        for (int xx = 0; xx < im->w; xx++) {
            int t = tile0 + (yy / 8) * tx + (xx / 8);
            int idx = tile_idx_px(g, t, xx & 7, yy & 7);
            if (transparent0 && idx == 0) continue;
            if (idx >= (int)pal.size()) idx = 0;
            const RGBAf& c = pal[idx];          // rgb555 : déjà en 0-255
            size_t o = ((size_t)yy * im->w + xx) * 4;
            im->px[o + 0] = (uint8_t)c.r;
            im->px[o + 1] = (uint8_t)c.g;
            im->px[o + 2] = (uint8_t)c.b;
            im->px[o + 3] = (uint8_t)c.a;
        }
    return im;
}

std::shared_ptr<Image> render_cell(const Ncgr& g, const Cell& c,
    const std::vector<std::vector<RGBAf>>& pals, int pal_ovr, bool tile64) {
    int w = c.maxx - c.minx + 1, h = c.maxy - c.miny + 1;
    if (w <= 0 || h <= 0 || c.oams.empty()) return nullptr;
    auto im = std::make_shared<Image>();
    im->w = w; im->h = h;
    im->px.resize((size_t)w * h * 4, 0);
    for (auto& o : c.oams) {
        int pal_i = (pal_ovr >= 0 ? pal_ovr : o.pal);
        if (pal_i >= (int)pals.size()) pal_i = 0;
        const auto& pal = pals.empty() ? std::vector<RGBAf>{} : pals[pal_i];
        int tw = o.w / 8;
        for (int yy = 0; yy < o.h; yy++)
            for (int xx = 0; xx < o.w; xx++) {
                int sx = o.fh ? (o.w - 1 - xx) : xx;
                int sy = o.fv ? (o.h - 1 - yy) : yy;
                int t = o.tile * (tile64 ? 2 : 1) + (sy / 8) * tw + (sx / 8);
                int idx = tile_idx_px(g, t, sx & 7, sy & 7);
                if (idx == 0) continue;
                int dx = o.x - c.minx + xx, dy = o.y - c.miny + yy;
                if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
                if (idx >= (int)pal.size()) idx = 0;
                const RGBAf& cc = pal.empty() ? RGBAf{255,255,255,255} : pal[idx];
                size_t oo = ((size_t)dy * w + dx) * 4;
                im->px[oo + 0] = (uint8_t)cc.r;
                im->px[oo + 1] = (uint8_t)cc.g;
                im->px[oo + 2] = (uint8_t)cc.b;
                im->px[oo + 3] = (uint8_t)cc.a;
            }
    }
    return im;
}

std::shared_ptr<Image> render_screen(const Bytes& scr0, const Ncgr& g,
    const std::vector<std::vector<RGBAf>>& pals, bool skip0) {
    Bytes sc = unlz(scr0);
    if (sc.size() < 0x24) return nullptr;
    int w = u16(sc, 0x18), h = u16(sc, 0x1A);          // px (256x192 / 512x256)
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return nullptr;
    int cw = w / 8, ncell = cw * (h / 8);
    int avail = (int)(sc.size() > 0x24 ? sc.size() - 0x24 : 0);
    if (avail <= 0) return nullptr;
    // couches affines : 1 octet d'indice de tuile par cellule (8bpp,
    // palette 256 couleurs à plat, pas de flip) — ex. trainer_card_front.
    if (avail == ncell && g.bpp == 8) {
        auto im = std::make_shared<Image>();
        im->w = w; im->h = h;
        im->px.resize((size_t)w * h * 4, 0);
        std::vector<RGBAf> flat;
        for (auto& p : pals) flat.insert(flat.end(), p.begin(), p.end());
        for (int c = 0; c < ncell; c++) {
            int cx = c % cw, cy = c / cw;
            int t = sc[0x24 + c];
            int ox = cx * 8, oy = cy * 8;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int idx = tile_idx_px(g, t, x, y);
                    if (skip0 && idx == 0) continue;
                    if (idx >= (int)flat.size()) idx = 0;
                    const RGBAf& cc = flat.empty()
                        ? RGBAf{0,0,0,0} : flat[idx];
                    size_t o = ((size_t)(oy + y) * w + ox + x) * 4;
                    im->px[o + 0] = (uint8_t)cc.r;
                    im->px[o + 1] = (uint8_t)cc.g;
                    im->px[o + 2] = (uint8_t)cc.b;
                    im->px[o + 3] = (uint8_t)cc.a;
                }
        }
        return im;
    }
    // certaines RCSN déclarent 256x256 mais n'embarquent que les lignes
    // utiles (ex. trainer_card_front = 256x128) : on borne aux entrées
    // réellement présentes.
    avail /= 2;
    if (ncell > avail) {
        h = (avail / cw) * 8;
        if (h <= 0) return nullptr;
        ncell = avail;
    }
    auto im = std::make_shared<Image>();
    im->w = w; im->h = h;
    im->px.resize((size_t)w * h * 4, 0);
    std::vector<RGBAf> flat;                        // 8bpp : palette 256 à plat
    if (g.bpp == 8)
        for (auto& p : pals) flat.insert(flat.end(), p.begin(), p.end());
    // données en blocs d'écran 32x32 seulement si l'écran dépasse 256 px ;
    // sinon les cellules sont rangées ligne par ligne (fenêtres <256 px)
    bool blocks = w > 256 || h > 256;
    for (int c = 0; c < ncell; c++) {
        int cx = c % cw, cy = c / cw;
        int si;
        if (blocks) {
            int blk = (cy / 32) * (cw / 32) + (cx / 32);
            si = blk * 1024 + (cy & 31) * 32 + (cx & 31);
        } else si = cy * cw + cx;
        if (si >= ncell) continue;
        uint16_t e = u16(sc, 0x24 + si * 2);
        int t = e & 0x3FF, pal = (e >> 12) & 0xF;
        bool fh = (e >> 10) & 1, fv = (e >> 11) & 1;
        if (pal >= (int)pals.size()) pal = 0;
        const auto& pp = (g.bpp == 8) ? flat
            : (pals.empty() ? std::vector<RGBAf>{} : pals[pal]);
        int ox = (c % cw) * 8, oy = (c / cw) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int sx = fh ? 7 - x : x, sy = fv ? 7 - y : y;
                int idx = tile_idx_px(g, t, sx, sy);
                if (skip0 && idx == 0) continue;
                if (idx >= (int)pp.size()) idx = 0;
                const RGBAf& cc = pp.empty() ? RGBAf{0,0,0,0} : pp[idx];
                size_t o = ((size_t)(oy + y) * w + ox + x) * 4;
                im->px[o + 0] = (uint8_t)cc.r;
                im->px[o + 1] = (uint8_t)cc.g;
                im->px[o + 2] = (uint8_t)cc.b;
                im->px[o + 3] = (uint8_t)cc.a;
            }
    }
    return im;
}

// ── NFGR : police bitmap Game Freak (pl_font.narc) ──────────────────────────
const uint8_t* Nfgr::glyph_px(int g) const {
    if (g < 0 || g >= nglyphs) return nullptr;
    return pixels.data() + (size_t)g * cellW * cellH;
}
int Nfgr::glyph_w(int g) const {
    if (g < 0 || g >= (int)widths.size()) return cellW;
    return widths[g];
}

Nfgr parse_nfgr(const Bytes& d0) {
    // ATTENTION : un NFGR brut commence par u32=headerSize (souvent 0x10),
    // indiscernable du marqueur LZ10 étendu — valider le format d'abord.
    Bytes dd = d0;
    auto looks_nfgr = [](const Bytes& b) {
        if (b.size() < 16) return false;
        uint32_t h = b[0] | (b[1]<<8) | (b[2]<<16) | ((uint32_t)b[3]<<24);
        uint32_t ng = b[8] | (b[9]<<8) | (b[10]<<16) | ((uint32_t)b[11]<<24);
        int gb = b[14] * b[15] * 16;
        return h >= 8 && h <= 0x40 && ng > 0 && ng <= 20000 && gb > 0 &&
               h + (uint64_t)ng * gb <= b.size();
    };
    if (!looks_nfgr(dd)) dd = unlz(dd);
    const Bytes& d = dd;
    Nfgr f;
    if (d.size() < 16) return f;
    uint32_t hdr = u32(d, 0), wto = u32(d, 4);
    f.nglyphs = (int)u32(d, 8);
    f.maxW = d[12]; f.maxH = d[13];
    int tw = d[14], th = d[15];
    f.cellW = tw * 8; f.cellH = th * 8;
    int gb = tw * th * 16;
    if (f.nglyphs <= 0 || hdr + (size_t)f.nglyphs * gb > d.size()) { f.nglyphs = 0; return f; }
    f.pixels.assign((size_t)f.nglyphs * f.cellW * f.cellH, 0);
    for (int g = 0; g < f.nglyphs; g++)
        for (int t = 0; t < tw * th; t++) {
            int tx = (t % tw) * 8, ty = (t / tw) * 8;
            for (int row = 0; row < 8; row++) {
                size_t o = hdr + (size_t)g * gb + t * 16 + row * 2;
                for (int x = 0; x < 8; x++) {
                    uint8_t byte = (x < 4) ? d[o + 1] : d[o];
                    f.pixels[(size_t)g * f.cellW * f.cellH +
                             (size_t)(ty + row) * f.cellW + tx + x] =
                        (byte >> (6 - 2 * (x & 3))) & 3;
                }
            }
        }
    if (wto + f.nglyphs <= d.size())
        f.widths.assign(d.begin() + wto, d.begin() + wto + f.nglyphs);
    else
        f.widths.assign(f.nglyphs, (uint8_t)f.cellW);
    return f;
}

std::shared_ptr<Image> font_atlas(const Nfgr& f, RGBAf fg, RGBAf sh, int ss) {
    if (!f.nglyphs || !f.cellW || !f.cellH) return nullptr;
    if (ss < 1) ss = 1;
    int cols = 16, rows = (f.nglyphs + cols - 1) / cols;
    int w = cols * f.cellW * ss, h = rows * f.cellH * ss;
    auto im = std::make_shared<Image>(); im->w = w; im->h = h;
    im->px.assign((size_t)w * h * 4, 0);
    // couverture bilinéaire du masque binaire (v==want) -> bords lissés,
    // resserrés par une courbe de contraste pour rester nets (pas flous)
    auto cov = [&](const uint8_t* gp, int want, float sx, float sy) {
        int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
        float fx = sx - x0, fy = sy - y0, a = 0.f;
        for (int j = 0; j <= 1; j++) for (int i = 0; i <= 1; i++) {
            int x = x0 + i, y = y0 + j;
            float w_ = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
            if (x >= 0 && x < f.cellW && y >= 0 && y < f.cellH &&
                gp[y * f.cellW + x] == want)
                a += w_;
        }
        // transition sur ~1/3 de px source : antialiased mais net
        a = (a - 0.5f) * 2.6f + 0.5f;
        return a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
    };
    for (int g = 0; g < f.nglyphs; g++) {
        int ox = (g % cols) * f.cellW * ss, oy = (g / cols) * f.cellH * ss;
        const uint8_t* gp = f.glyph_px(g);
        for (int y = 0; y < f.cellH * ss; y++)
            for (int x = 0; x < f.cellW * ss; x++) {
                float sx = (x + 0.5f) / ss - 0.5f,
                      sy = (y + 0.5f) / ss - 0.5f;
                float af = cov(gp, 1, sx, sy), as = cov(gp, 2, sx, sy);
                if (af <= 0.f && as <= 0.f) continue;
                // ombre sous le texte : sh derrière, fg dessus
                float r = sh.r * as + fg.r * af * (1.f - as * 0.f);
                float gg = sh.g * as + fg.g * af;
                float b = sh.b * as + fg.b * af;
                float a = fminf(1.f, as + af) * 255.f;
                if (af > 0.f && as > 0.f) {          // chevauchement : fg l'emporte
                    r = fg.r * af + sh.r * as * (1.f - af);
                    gg = fg.g * af + sh.g * as * (1.f - af);
                    b = fg.b * af + sh.b * as * (1.f - af);
                }
                size_t o = ((size_t)(oy + y) * w + ox + x) * 4;
                im->px[o] = (uint8_t)fminf(255.f, r);
                im->px[o + 1] = (uint8_t)fminf(255.f, gg);
                im->px[o + 2] = (uint8_t)fminf(255.f, b);
                im->px[o + 3] = (uint8_t)a;
            }
    }
    return im;
}

} // namespace spr
