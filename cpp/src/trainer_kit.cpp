// trainer_kit.cpp — compositeur du kit de création de dresseur.
// Calques 256x256 (4x4 de 64px, lignes S,W,E,N, cols arrêt/pas/arrêt/pas)
// -> colonne 32x1024 au format skins/ (16 marche + 16 course).
#include "trainer_kit.h"
#include "../thirdparty/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../thirdparty/stb/stb_image_write.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace fs = std::filesystem;

static std::string normtok(std::string s) {
    std::string o;
    for (unsigned char c : s) {
        if (isspace(c)) { if (!o.empty() && o.back() != ' ') o += ' '; }
        else o += (char)tolower(c);
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    return o;
}

// préfixe commun de tous les noms -> couleur = reste du nom
static std::string common_prefix(const std::vector<std::string>& v) {
    if (v.empty()) return "";
    std::string p = v[0];
    for (size_t i = 1; i < v.size(); i++) {
        size_t j = 0;
        while (j < p.size() && j < v[i].size() && p[j] == v[i][j]) j++;
        p.resize(j);
    }
    return p;
}

// ordre d'affichage des catégories + dossier kit + optionnel ?
static const struct { const char* dir; const char* fr; const char* en;
                      bool opt; } CATS[] = {
    {"bases mf",        "PEAU",    "SKIN",   false},
    {"bottoms",         "BAS",     "BOTTOM", false},
    {"tops",            "HAUT",    "TOP",    false},
    {"hair",            "CHEVEUX", "HAIR",   true },
    {"hats",            "CHAPEAU", "HAT",    true },
    {"carrying stuff",  "SAC",     "BAG",    true },
};
static const int NCAT = (int)(sizeof CATS / sizeof CATS[0]);

static void scan_cat(TrainerKit::Cat& c, const std::string& dir) {
    std::error_code ec;
    std::vector<std::string> dirs, files;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string nm = e.path().filename().string();
        if (e.is_directory()) dirs.push_back(nm);
        else if (e.is_regular_file()) {
            auto ext = e.path().extension().string();
            for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
            if (ext == ".png")
                files.push_back(e.path().stem().string());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    for (auto& d : dirs) {                    // dossier = pièce + couleurs
        KitPiece p; p.name = d;
        std::vector<std::string> stems;
        for (auto& e : fs::directory_iterator(dir + "/" + d, ec))
            if (e.is_regular_file() &&
                e.path().extension() == ".png")
                stems.push_back(e.path().stem().string());
        std::sort(stems.begin(), stems.end());
        std::string pre = common_prefix(stems);
        for (auto& s : stems) {
            std::string col = s.size() > pre.size()
                ? s.substr(pre.size()) : s;
            p.colors.push_back(normtok(col).empty() ? s : normtok(col));
        }
        if (!p.colors.empty()) c.pieces.push_back(std::move(p));
    }
    for (auto& f : files) {                   // fichier seul = pièce 1 couleur
        KitPiece p; p.name = f;
        c.pieces.push_back(std::move(p));
    }
}

TrainerKit kit_scan(const std::string& root) {
    TrainerKit k;
    std::string wdir = root + "/walk";
    std::error_code ec;
    if (!fs::is_directory(wdir, ec)) return k;
    for (int i = 0; i < NCAT; i++) {
        TrainerKit::Cat c;
        c.dir = CATS[i].dir;
        c.label[0] = CATS[i].fr; c.label[1] = CATS[i].en;
        c.optional = CATS[i].opt;
        scan_cat(c, wdir + "/" + c.dir);
        k.cats.push_back(std::move(c));
    }
    k.ok = true;
    return k;
}

// résout le fichier PNG d'un calque (nullptr si "aucun"/introuvable)
static std::string kit_file(const std::string& root, const char* mode,
                            const TrainerKit::Cat& c, const KitSel& s) {
    if (s.piece < 0 || s.piece >= (int)c.pieces.size()) return "";
    const KitPiece& p = c.pieces[s.piece];
    std::string base = root + "/" + mode + "/" + c.dir;
    std::error_code ec;
    if (p.colors.empty()) {                   // fichier direct
        std::string f = base + "/" + p.name + ".png";
        if (fs::is_regular_file(f, ec)) return f;
        return "";
    }
    int ci = s.color < 0 ? 0 : s.color;
    if (ci >= (int)p.colors.size()) ci = 0;
    // le fichier = stem qui contient le jeton couleur
    std::string want = normtok(p.colors[ci]);
    for (auto& e : fs::directory_iterator(base + "/" + p.name, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".png") continue;
        if (normtok(e.path().stem().string()).find(want)
                != std::string::npos)
            return e.path().string();
    }
    return "";
}

static Image load_img(const std::string& path) {
    Image im;
    int w, h;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    if (!d) return im;
    im.create(w, h);
    memcpy(im.px.data(), d, (size_t)w * h * 4);
    stbi_image_free(d);
    return im;
}

// ── retouche HSV d'un calque (pixels opaques uniquement) ─────────────────
static void k_rgb2hsv(int R, int G, int B, float* h, float* s, float* v) {
    float r = R / 255.f, g = G / 255.f, b = B / 255.f;
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    *v = mx;
    float d = mx - mn;
    *s = mx < 1e-6f ? 0.f : d / mx;
    if (d < 1e-6f) { *h = 0.f; return; }
    if (mx == r)      *h = 60.f * fmodf((g - b) / d, 6.f);
    else if (mx == g) *h = 60.f * ((b - r) / d + 2.f);
    else              *h = 60.f * ((r - g) / d + 4.f);
    if (*h < 0.f) *h += 360.f;
}
static void k_hsv2rgb(float h, float s, float v, int* R, int* G, int* B) {
    float c = v * s, x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f)),
          m = v - c;
    float r = 0, g = 0, b = 0;
    int sec = (int)(h / 60.f) % 6;
    if (sec == 0)      { r = c; g = x; }
    else if (sec == 1) { r = x; g = c; }
    else if (sec == 2) { g = c; b = x; }
    else if (sec == 3) { g = x; b = c; }
    else if (sec == 4) { r = x; b = c; }
    else               { r = c; b = x; }
    *R = (int)((r + m) * 255.f + .5f);
    *G = (int)((g + m) * 255.f + .5f);
    *B = (int)((b + m) * 255.f + .5f);
}
static void kit_tint(Image* im, const KitTint& t) {
    if (!im || t.identity()) return;
    for (size_t i = 0; i + 3 < im->px.size(); i += 4) {
        if (im->px[i + 3] < 8) continue;
        float h, s, v;
        k_rgb2hsv(im->px[i], im->px[i + 1], im->px[i + 2], &h, &s, &v);
        h = fmodf(h + (float)t.hue + 720.f, 360.f);
        s = fminf(1.f, fmaxf(0.f, s * t.sat / 100.f));
        v = fminf(1.f, fmaxf(0.f, v * t.val / 100.f));
        int r, g, b; k_hsv2rgb(h, s, v, &r, &g, &b);
        im->px[i] = (uint8_t)r; im->px[i + 1] = (uint8_t)g;
        im->px[i + 2] = (uint8_t)b;
    }
}

// alpha-over : src par-dessus dst (mêmes dimensions)
static void over(Image& dst, const Image& src) {
    int n = std::min(dst.w * dst.h, src.w * src.h);
    for (int i = 0; i < n; i++) {
        const uint8_t* s = &src.px[i * 4];
        if (!s[3]) continue;
        uint8_t* d = &dst.px[i * 4];
        float a = s[3] / 255.f, ia = 1.f - a;
        d[0] = (uint8_t)(s[0] * a + d[0] * ia);
        d[1] = (uint8_t)(s[1] * a + d[1] * ia);
        d[2] = (uint8_t)(s[2] * a + d[2] * ia);
        d[3] = (uint8_t)std::min(255, (int)(s[3] + d[3] * ia));
    }
}

// bbox serré des pixels opaques
static void tight(const Image& im, int& x0, int& y0, int& x1, int& y1) {
    x0 = im.w; y0 = im.h; x1 = y1 = -1;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++)
            if (im.px[((size_t)y * im.w + x) * 4 + 3] > 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
}

// redimensionne une cellule 64x64 -> 32x32 (ancré bas-centre du sprite)
// filtre de surface : chaque pixel dest couvre ~1/scale px source,
// couleurs prémultipliées par l'alpha -> bords lisses sans halo
static void cell_to_frame(const Image& sheet, int cx, int cy,
                          float scale, float ax, float ay, Image& out,
                          int fy) {
    float h = 0.5f / scale;                     // demi-côté de la zone source
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float sx = ax + (x - 16.f) / scale,   // centre src du pixel dest
                  sy = ay + (y - 31.f) / scale;   // bas de cellule = pieds
            float fx0 = sx - h, fy0 = sy - h,
                  fx1 = sx + h, fy1 = sy + h;
            int jx0 = (int)floorf(fx0), jy0 = (int)floorf(fy0),
                jx1 = (int)ceilf(fx1), jy1 = (int)ceilf(fy1);
            float acc[4] = {}, wsum = 0.f;
            for (int jy = jy0; jy < jy1; jy++)
                for (int jx = jx0; jx < jx1; jx++) {
                    float ox = std::min((float)jx + 1.f, fx1) -
                               std::max((float)jx, fx0),
                          oy = std::min((float)jy + 1.f, fy1) -
                               std::max((float)jy, fy0);
                    if (ox <= 0.f || oy <= 0.f) continue;
                    int qx = jx + cx, qy = jy + cy;
                    if (qx < cx || qx >= cx + 64 || qy < cy || qy >= cy + 64)
                        continue;
                    const uint8_t* q =
                        &sheet.px[((size_t)qy * sheet.w + qx) * 4];
                    float w = ox * oy * (q[3] / 255.f);
                    acc[0] += q[0] * w; acc[1] += q[1] * w;
                    acc[2] += q[2] * w; acc[3] += q[3] * ox * oy;
                    wsum += ox * oy;
                }
            if (acc[3] < 8.f) continue;
            uint8_t* d = &out.px[((size_t)(fy * 32 + y) * 32 + x) * 4];
            float aw = acc[3] / 255.f;
            d[0] = (uint8_t)std::min(255.f, acc[0] / std::max(1e-6f, aw));
            d[1] = (uint8_t)std::min(255.f, acc[1] / std::max(1e-6f, aw));
            d[2] = (uint8_t)std::min(255.f, acc[2] / std::max(1e-6f, aw));
            // alpha : couverture relative (la zone dest vaut wsum px src)
            d[3] = (uint8_t)std::min(255.f, acc[3] / wsum * 1.35f);
        }
}

// compose un mode (walk|run) -> 16 frames de la colonne
static void compose_mode(const TrainerKit& k, const std::string& root,
                         const char* mode, const KitSel sel[],
                         Image& out, int fbase, const KitTint tints[]) {
    Image sheet; sheet.create(256, 256);
    Image base;                                 // corps seul (repère d'échelle)
    for (size_t i = 0; i < k.cats.size(); i++) {
        std::string f = kit_file(root, mode, k.cats[i],
                                 i < NCAT ? sel[i] : KitSel{});
        if (f.empty()) continue;
        Image layer = load_img(f);
        if (!layer.w) continue;
        if (i == 0) base = layer;               // peau = référence taille
        if (layer.w != 256 || layer.h != 256) continue;
        if (tints) kit_tint(&layer, tints[i]);
        over(sheet, layer);
    }
    // échelle : corps ~44px dans la cellule 64 -> ~22px dans la cellule 32
    // (aligné sur les héros DS qui font ~23-25px chapeau compris)
    int x0 = 16, y0 = 10, x1 = 48, y1 = 56;
    if (base.w >= 64) {                        // bbox dans la cellule (0,0)
        Image c; c.create(64, 64);
        for (int y = 0; y < 64; y++)
            memcpy(&c.px[y * 256], &base.px[(size_t)y * base.w * 4], 256);
        tight(c, x0, y0, x1, y1);
        if (x1 < x0) { x0 = 16; y0 = 10; x1 = 48; y1 = 56; }
    }
    float bh = (float)(y1 - y0 + 1);
    // réduction pile 2:1 alignée sur la grille : chaque bloc 2x2 src
    // devient 1 pixel net (corps ~44px -> ~22px, gabarit héros DS)
    float scale = 0.5f;
    float ax = floorf((x0 + x1) * 0.5f + 0.5f),
          ay = floorf((float)y1 + 0.5f);
    (void)bh;
    // RMXP lignes S,W,E,N -> blocs decomp S=1,N=0,W=2,E=3
    static const int R2B[4] = {1, 2, 3, 0};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int fr = fbase + R2B[r] * 4 + c;
            cell_to_frame(sheet, c * 64, r * 64, scale, ax, ay, out, fr);
        }
}

bool kit_compose(const TrainerKit& k, const std::string& root,
                 const KitSel sel[], Image& out, const KitTint tints[]) {
    if (!k.ok || sel[0].piece < 0) return false;
    out.create(32, 32 * 32);
    compose_mode(k, root, "walk", sel, out, 0, tints);
    compose_mode(k, root, "run", sel, out, 16, tints);
    return true;
}

bool kit_save(const TrainerKit& k, const std::string& root,
              const KitSel sel[], const std::string& path,
              const KitTint tints[]) {
    Image out;
    if (!kit_compose(k, root, sel, out, tints)) return false;
    return stbi_write_png(path.c_str(), out.w, out.h, 4,
                          out.px.data(), 0) != 0;
}

// ── dos de combat : back m|f/{base,clothing,hair,hat} -------------------------
// feuilles 875x196 = 5 poses de 175px (pose 0 = debout, la seule utilisée).
// Correspondances avec les choix du créateur :
//   peau  "m|f base <ton>"        -> base/base <ton>.png
//   haut  "xxx top m|f|fm"        -> clothing/<xxx>/<xxx> <couleur>.png
//   chev. "boy|girl N"            -> hair/hair N/N <couleur>.png
//   chap. "<nom>"                 -> hat/<nom>/<nom> <couleur>.png
// (bas et sac n'ont pas de calque de dos : le sprite n'est qu'un buste)

// jeton couleur choisi pour une sélection ("" si rien)
static std::string sel_color(const TrainerKit::Cat& c, const KitSel& s) {
    if (s.piece < 0 || s.piece >= (int)c.pieces.size()) return "";
    const KitPiece& p = c.pieces[s.piece];
    if (p.colors.empty() || s.color < 0 ||
        s.color >= (int)p.colors.size())
        return "";
    return normtok(p.colors[s.color]);
}

// fichier PNG du dossier dont le stem contient le jeton (sinon premier trié)
static std::string pick_color_file(const std::string& dir,
                                   const std::string& tok) {
    std::error_code ec;
    std::vector<std::string> paths;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".png")
            paths.push_back(e.path().string());
    std::sort(paths.begin(), paths.end());
    for (auto& p : paths)
        if (!tok.empty() &&
            normtok(fs::path(p).stem().string()).find(tok)
                != std::string::npos)
            return p;
    return paths.empty() ? "" : paths.front();
}

// nom de pièce -> dossier clothing/hat back (retire " top m|f", " fm", ...)
static std::string back_piece(std::string n) {
    n = normtok(n);
    static const char* SUF[] = {" top m", " top f", " top", " fm",
                                " f", " m"};
    for (auto s : SUF)
        if (n.size() > strlen(s) &&
            n.compare(n.size() - strlen(s), strlen(s), s) == 0)
            { n.resize(n.size() - strlen(s)); break; }
    return n;
}

bool kit_compose_back(const TrainerKit& k, const std::string& root,
                      const KitSel sel[], int gender, Image& out,
                      const KitTint tints[]) {
    std::error_code ec;
    std::string bd = root + (gender ? "/back f" : "/back m");
    if (!fs::is_directory(bd, ec) || k.cats.size() < 5) return false;
    Image sheet; sheet.create(875, 196);
    auto lay = [&](const std::string& f, int cat) {
        Image im = load_img(f);
        if (!im.w) return;
        if (tints && cat >= 0) kit_tint(&im, tints[cat]);
        over(sheet, im);
    };
    // peau : dernier jeton du nom de pièce = ton ("m base light" -> "light")
    if (sel[0].piece >= 0 && sel[0].piece < (int)k.cats[0].pieces.size()) {
        std::string n = normtok(k.cats[0].pieces[sel[0].piece].name);
        size_t sp = n.rfind(' ');
        std::string f = bd + "/base/base" +
            (sp == std::string::npos ? "" : " " + n.substr(sp + 1)) + ".png";
        lay(f, 0);
    }
    // haut : clothing/<pièce mappée>/
    if (sel[2].piece >= 0 && sel[2].piece < (int)k.cats[2].pieces.size()) {
        std::string d = bd + "/clothing/" +
            back_piece(k.cats[2].pieces[sel[2].piece].name);
        if (fs::is_directory(d, ec))
            lay(pick_color_file(d, sel_color(k.cats[2], sel[2])), 2);
    }
    // cheveux : boy|girl N -> hair/hair N/
    if (sel[3].piece >= 0 && sel[3].piece < (int)k.cats[3].pieces.size()) {
        std::string n = normtok(k.cats[3].pieces[sel[3].piece].name);
        size_t sp = n.rfind(' ');
        if (sp != std::string::npos) {
            std::string d = bd + "/hair/hair" + n.substr(sp);
            if (fs::is_directory(d, ec))
                lay(pick_color_file(d, sel_color(k.cats[3], sel[3])), 3);
        }
    }
    // chapeau : hat/<nom>/
    if (sel[4].piece >= 0 && sel[4].piece < (int)k.cats[4].pieces.size()) {
        std::string d = bd + "/hat/" +
            normtok(k.cats[4].pieces[sel[4].piece].name);
        if (fs::is_directory(d, ec))
            lay(pick_color_file(d, sel_color(k.cats[4], sel[4])), 4);
    }
    // pose 0 (175x196) -> 80x80 : bbox serrée ancrée bas-centre
    Image pose; pose.create(175, 196);
    for (int y = 0; y < 196; y++)
        memcpy(&pose.px[(size_t)y * 175 * 4],
               &sheet.px[(size_t)y * 875 * 4], 175 * 4);
    int x0, y0, x1, y1;
    tight(pose, x0, y0, x1, y1);
    if (x1 < x0) return false;
    out.create(80, 80);
    float sc = std::min(76.f / (y1 - y0 + 1), 76.f / (x1 - x0 + 1));
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 80; x++) {
            int sx = (int)((x0 + x1) * .5f + (x - 40.f) / sc + .5f);
            int sy = (int)(y1 + (y - 79.f) / sc + .5f);
            if (sx < 0 || sx >= 175 || sy < 0 || sy >= 196) continue;
            memcpy(&out.px[((size_t)y * 80 + x) * 4],
                   &pose.px[((size_t)sy * 175 + sx) * 4], 4);
        }
    return true;
}
