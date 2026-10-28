// main.cpp — OpenPlatinum : SDL3 + OpenGL3 + Dear ImGui.
// Charge la ROM .nds directement, rend les maps, joueur + caméra + warps.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "nds.h"
#include "msg.h"
#include "world.h"
#include "render.h"
#include "spr.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
namespace fs = std::filesystem;

static std::string find_rom(int argc, char** argv, std::string& hdr_path) {
    // 1) --rom <path>  2) ./rom/*.nds  3) ../rom/*.nds (exe dans builddir)
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--rom")) return argv[i + 1];
    const char* dirs[] = {"rom", "../rom", "../../rom"};
    for (auto d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec)) {
            auto p = e.path().string();
            if (p.size() > 4 && p.substr(p.size() - 4) == ".nds") return p;
        }
    }
    return "";
}

static int arg_map(int argc, char** argv, int def) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--map")) return atoi(argv[i + 1]);
    return def;
}

static int arg_shot(int argc, char** argv) {   // --shot N : frame de capture
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--shot")) return atoi(argv[i + 1]);
    return -1;
}

// --gif F1,F2 : dump chaque frame de la plage en PPM -> _shots/gif_%05d.ppm
static int arg_gif(int argc, char** argv, int* f2) {
    *f2 = -1;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--gif")) {
            int a = -1, b = -1;
            sscanf(argv[i + 1], "%d,%d", &a, &b);
            *f2 = b;
            return a;
        }
    return -1;
}

static void save_ppm(const char* path, int w, int h, const uint8_t* rgba) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (int y = h - 1; y >= 0; y--) {            // GL lit bottom-up
        const uint8_t* s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = s[x * 4 + 0];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4 + 2];
        }
        fwrite(row.data(), 3, (size_t)w, f);
    }
    fclose(f);
    printf("[shot] %s\n", path);
}

// ── style HUD custom (mockup hud.html) : verre sombre + accents ─────────────
static const ImU32 C_PANEL_BG = IM_COL32(16, 21, 31, 242);
static const ImU32 C_PANEL_BD = IM_COL32(58, 74, 104, 255);
static const ImU32 C_TEXT     = IM_COL32(232, 236, 244, 255);
static const ImU32 C_ACCENT_B = IM_COL32(91, 141, 239, 255);   // bleu
static const ImU32 C_ACCENT_V = IM_COL32(160, 107, 255, 255);  // violet
static const ImU32 C_AMBER    = IM_COL32(255, 184, 77, 255);   // noms/curseur
static const ImU32 C_SEL_BG   = IM_COL32(91, 141, 239, 40);
static const ImU32 C_DIM      = IM_COL32(139, 152, 181, 255);
// couleur de bordure par "TYPE de fenêtre" DS (option FENETRE 1-20) :
// palette inspirée des cadres pl_winframe (bleu, vert, rouge, violet...)
static ImU32 frame_col() {
    static const ImU32 T[20] = {
        IM_COL32( 58, 74,104,255), IM_COL32( 58, 74,104,255),
        IM_COL32( 90,140,220,255), IM_COL32( 80,190,140,255),
        IM_COL32(230,110, 90,255), IM_COL32(170,110,230,255),
        IM_COL32(240,190, 80,255), IM_COL32( 90,200,220,255),
        IM_COL32(200, 90,150,255), IM_COL32(130,170,255,255),
        IM_COL32(110,110,120,255), IM_COL32(180,140, 90,255),
        IM_COL32( 90,150, 90,255), IM_COL32(220, 80, 80,255),
        IM_COL32(140, 90,200,255), IM_COL32(230,170, 60,255),
        IM_COL32( 70,180,200,255), IM_COL32(190, 90,120,255),
        IM_COL32(100,130,200,255), IM_COL32(150,150,160,255),
    };
    return T[g_opts.frame >= 0 && g_opts.frame < 20 ? g_opts.frame : 0];
}
static void panel_push() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, C_PANEL_BG);
    ImGui::PushStyleColor(ImGuiCol_Border, frame_col());
    ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
}
static void panel_pop() { ImGui::PopStyleVar(3); ImGui::PopStyleColor(3); }
// liseré bleu->violet en haut du panneau courant
static void accent_line() {
    ImVec2 p = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor({p.x, p.y},
        {p.x + ImGui::GetWindowWidth(), p.y + 2},
        C_ACCENT_B, C_ACCENT_V, C_ACCENT_V, C_ACCENT_B);
}
// simule une pression clavier (clics souris -> logique touches)
static void push_key(SDL_Keycode k) {
    SDL_Event kev{}; kev.type = SDL_EVENT_KEY_DOWN;
    kev.key.key = k;
    kev.key.scancode = SDL_GetScancodeFromKey(k, SDL_KMOD_NONE);
    kev.key.down = true; SDL_PushEvent(&kev);
}
static ImU32 hp_col(float r) {                       // vert/ambre/rouge + glow
    return r > 0.5f ? IM_COL32(62, 207, 88, 255)
        : r > 0.25f ? IM_COL32(255, 176, 58, 255)
        : IM_COL32(240, 68, 56, 255);
}
// ── palette « style B » : les vraies couleurs des écrans DS Platine ─────
static const ImU32 B_NAVY   = IM_COL32(30, 45, 90, 255);
static const ImU32 B_DIM    = IM_COL32(90, 110, 150, 255);
static const ImU32 B_ORANGE = IM_COL32(240, 160, 50, 255);
static const ImU32 B_GREEN  = IM_COL32(80, 180, 120, 255);
static const ImU32 B_TEAL   = IM_COL32(90, 160, 190, 255);
static const ImU32 B_WHITE  = IM_COL32(250, 252, 255, 255);
static const ImU32 B_SELBG  = IM_COL32(255, 214, 110, 255);
static const ImU32 B_PURP   = IM_COL32(110, 80, 190, 255);
static const ImU32 B_PURP2  = IM_COL32(80, 60, 150, 255);
static const ImU32 B_ZEBRA  = IM_COL32(225, 238, 252, 255);
static const ImU32 B_ACT    = IM_COL32(90, 140, 190, 255);
// fond plein écran : ciel dégradé + stries + bandeau violet bas
static void sb_bg(ImDrawList* dl, float w, float h, bool strip = true) {
    dl->AddRectFilledMultiColor({0, 0}, {w, h},
        IM_COL32(150, 195, 235, 255), IM_COL32(150, 195, 235, 255),
        IM_COL32(240, 245, 250, 255), IM_COL32(240, 245, 250, 255));
    for (float yy = 0; yy < h; yy += 6.f)
        dl->AddLine({0, yy}, {w, yy}, IM_COL32(160, 205, 240, 110));
    if (strip) {
        dl->AddRectFilled({0, h - 70}, {w, h}, B_PURP);
        dl->AddRectFilled({0, h - 70}, {w, h - 64}, B_PURP2);
    }
}
// voile clair qui estompe le jeu derrière un menu superposé

// ── briques UI DS réelles : police NFGR (pl_font) + cadre pl_winframe ────────
static GLuint ui_upload(const Image& im, bool linear = false) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im.w, im.h, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, im.px.data());
    GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
// encres police DS (pl_font m7) : [fg][ombre] par code couleur du jeu
static const ImU32 DS_FG[8]  = {IM_COL32(80,80,88,255), IM_COL32(232,32,16,255),
    IM_COL32(0,112,248,255), IM_COL32(0,0,0,255), IM_COL32(40,168,32,255),
    IM_COL32(248,248,248,255), IM_COL32(216,216,216,255), IM_COL32(248,248,248,255)};
static const ImU32 DS_SH[8]  = {IM_COL32(160,160,168,255), IM_COL32(248,168,184,255),
    IM_COL32(120,184,232,255), IM_COL32(120,120,120,255), IM_COL32(160,230,160,255),
    IM_COL32(248,184,192,255), IM_COL32(144,208,248,255), IM_COL32(0,0,0,255)};
static spr::Nfgr g_dfont;
static GLuint g_fatlas[8] = {};
static bool g_font_try = false;
static bool ds_font_ready(Nds& db) {
    if (!g_font_try) {
        g_font_try = true;
        auto fn = db.narc("/graphic/pl_font.narc");
        if (fn && fn->count() > 1)
            g_dfont = spr::parse_nfgr(fn->member(1));    // FONT_MESSAGE
    }
    return g_dfont.nglyphs > 0;
}
static GLuint ds_font_tex(int ci) {
    if (ci < 0 || ci > 7) ci = 0;
    if (!g_fatlas[ci]) {
        auto im = spr::font_atlas(g_dfont,
            RGBAf{(float)(DS_FG[ci] & 255), (float)((DS_FG[ci] >> 8) & 255),
                  (float)((DS_FG[ci] >> 16) & 255), 255},
            RGBAf{(float)(DS_SH[ci] & 255), (float)((DS_SH[ci] >> 8) & 255),
                  (float)((DS_SH[ci] >> 16) & 255), 255});
        if (im) g_fatlas[ci] = ui_upload(*im, true);   // police lissée
    }
    return g_fatlas[ci];
}
// dessine une chaîne glyphe par glyphe (avances natives du NFGR)
static float ds_text(ImDrawList* dl, const std::string& s, float x, float y,
                     int ci, float scale) {
    if (!g_dfont.nglyphs) { dl->AddText({x, y}, DS_FG[ci & 7], s.c_str()); return 0; }
    GLuint at = ds_font_tex(ci);
    float x0 = x;
    int awt = 16 * g_dfont.cellW,
        aht = ((g_dfont.nglyphs + 15) / 16) * g_dfont.cellH;
    for (size_t k = 0; k < s.size(); ) {
        int cp = msg_next_utf8(s, k);
        if (cp < 0) break;
        int gc = msg_char_code_of((char32_t)cp);
        int g = gc > 0 ? gc - 1 : -1;
        int w = g >= 0 ? g_dfont.glyph_w(g) : g_dfont.cellW;
        if (!w) w = g_dfont.cellW;
        if (g >= 0 && cp != ' ') {
            float pi = 0.4f;   // retrait UV : évite le saignement LINEAR
            float u0 = ((g % 16) * g_dfont.cellW + pi) / awt,
                  v0 = ((g / 16) * g_dfont.cellH + pi) / aht,
                  u1 = ((g % 16 + 1) * g_dfont.cellW - pi) / awt,
                  v1 = ((g / 16 + 1) * g_dfont.cellH - pi) / aht;
            dl->AddImage((ImTextureID)(intptr_t)at, ImVec2(x, y),
                ImVec2(x + g_dfont.cellW * scale, y + g_dfont.cellH * scale),
                ImVec2(u0, v0), ImVec2(u1, v1));
        }
        x += w * scale;
    }
    return x - x0;
}
// mesure d'une chaîne en pixels DS (échelle 1 = cellules natives)
static float ds_text_w(const std::string& s, float scale) {
    float w = 0;
    for (size_t k = 0; k < s.size(); ) {
        int cp = msg_next_utf8(s, k);
        if (cp < 0) break;
        int gc = msg_char_code_of((char32_t)cp);
        int g = gc > 0 ? gc - 1 : -1;
        int gw = g >= 0 ? g_dfont.glyph_w(g) : g_dfont.cellW;
        w += (gw ? gw : g_dfont.cellW) * scale;
    }
    return w;
}
// ── couche UI en pixels DS (256x192), upscalée à la fenêtre ─────────────────
// toutes les positions ci-dessous sont en coordonnées DS natives (tuiles 8px)
static float g_uds = 1.f, g_udx = 0.f;
static inline ImVec2 dp(float x, float y) {
    return ImVec2(g_udx + x * g_uds, y * g_uds);
}
// texte NFGR en coordonnées DS (taille native, espacement lettre 0)
static float dt_text(ImDrawList* dl, const std::string& s, float x, float y,
                     int ci) {
    ds_text(dl, s, g_udx + x * g_uds, y * g_uds, ci, g_uds);
    return 0;
}
// largeur en pixels DS (échelle native)
static float dt_text_w(const std::string& s) { return ds_text_w(s, 1.f); }
// pages du clavier de nommage DS : 5 lignes de 13 touches (espace = vide)
static const char* KB_ROWS[3][5] = {
    {   // page 0 : MAJUSCULES
        "ABCDEFGHIJ ,.",
        "KLMNOPQRST '-",
        "UVWXYZ    ♂♀",
        "             ",
        "0123456789   ",
    }, { // page 1 : minuscules
        "abcdefghij ,.",
        "klmnopqrst '-",
        "uvwxyz    ♂♀",
        "             ",
        "0123456789   ",
    }, { // page 2 : accents FR + symboles
        "ÀÂÄÆÇÉÈÊËÎÏ Ô",
        "ŒÙÛÜàâäæçéèêë",
        "îïôœùûüÿ !? ♂♀",
        "             ",
        "«»()'+-×÷=.,:;",
    },
};

// pl_winframe.narc : chaque membre est un NCGR/RLCN standard.
// [0-1]=cadres standard 3x3 (system/field), [2-21]=cadres message 6x3
// (18 tuiles, choix du joueur), [22]=curseur scroll (3 frames 16x16),
// [23]=wait_dial, [24]=pal standard, [25-44]=pals message.
static GLuint g_mtex[20] = {}, g_stdtex = 0, g_curtex = 0;
static ImU32 g_mfill[20] = {}, g_stdfill = 0;
static ImVec4 g_pal_cache[20] __attribute__((unused)) = {};
// tuile t d'une grille cols x rows -> sous-rect UV + dessin en coords DS
static void grid_tile(ImDrawList* dl, GLuint tex, int t, int cols, int rows,
                      float x, float y, float w = 8, float h = 8) {
    float u0 = (float)(t % cols) / cols, v0 = (float)(t / cols) / rows,
          u1 = u0 + 1.f / cols, v1 = v0 + 1.f / rows;
    dl->AddImage((ImTextureID)(intptr_t)tex, dp(x, y), dp(x + w, y + h),
        ImVec2(u0, v0), ImVec2(u1, v1));
}
static bool mbox_ready(Nds& db, int fr) {
    if (fr < 0 || fr > 19) fr = 0;
    if (g_mtex[fr]) return true;
    auto wn = db.narc("/graphic/pl_winframe.narc");
    if (!wn || wn->count() <= 25 + fr) return false;
    auto nc = spr::parse_ncgr(wn->member(2 + fr));
    auto wp = spr::parse_nclr(wn->member(25 + fr));
    if (nc.ntiles < 18 || wp.empty() || wp[0].size() <= 15) return false;
    auto im = spr::compose(nc, 0, 6, 3, wp[0], true);
    if (!im) return false;
    g_mtex[fr] = ui_upload(*im);
    g_mfill[fr] = IM_COL32((int)wp[0][15].r, (int)wp[0][15].g,
        (int)wp[0][15].b, 255);
    return true;
}
static bool std_ready(Nds& db) {
    if (g_stdtex) return true;
    auto wn = db.narc("/graphic/pl_winframe.narc");
    if (!wn || wn->count() <= 24) return false;
    auto nc = spr::parse_ncgr(wn->member(1));           // standard_field
    auto wp = spr::parse_nclr(wn->member(24));          // standard_system
    if (nc.ntiles < 9 || wp.empty() || wp[0].size() <= 15) return false;
    auto im = spr::compose(nc, 0, 3, 3, wp[0], true);
    if (!im) return false;
    g_stdtex = ui_upload(*im);
    g_stdfill = IM_COL32((int)wp[0][15].r, (int)wp[0][15].g,
        (int)wp[0][15].b, 255);
    return true;
}
// DrawMessageBoxFrame : cadre 18 tuiles AUTOUR du rect (x,y,w,h en tuiles DS)
static void dt_msgbox(ImDrawList* dl, Nds& db, int xt, int yt, int wt, int ht,
                      int frame = -1) {
    int fr = frame >= 0 ? frame : g_opts.frame;
    if (!mbox_ready(db, fr)) {
        dl->AddRectFilled(dp(xt * 8, yt * 8), dp((xt + wt) * 8, (yt + ht) * 8),
            B_WHITE);
        return;
    }
    // contenu : fond uni = palette du cadre, index 15
    dl->AddRectFilled(dp(xt * 8, yt * 8), dp((xt + wt) * 8, (yt + ht) * 8),
        g_mfill[fr]);
    float x = xt * 8.f, y = yt * 8.f;
    auto T = [&](int t, float cx, float cy) {
        grid_tile(dl, g_mtex[fr], t, 6, 3, x + cx * 8, y + cy * 8);
    };
    // rangée haute : 2 tuiles à gauche, bord répété, 3 à droite
    T(0, -2, -1); T(1, -1, -1);
    for (int i = 0; i < wt; i++) T(2, i, -1);
    T(3, wt, -1); T(4, wt + 1, -1); T(5, wt + 2, -1);
    // bords verticaux (tuile 8 = fond curseur, inutilisée ici)
    for (int i = 0; i < ht; i++) {
        T(6, -2, i); T(7, -1, i);
        T(9, wt, i); T(10, wt + 1, i); T(11, wt + 2, i);
    }
    // rangée basse
    T(12, -2, ht); T(13, -1, ht);
    for (int i = 0; i < wt; i++) T(14, i, ht);
    T(15, wt, ht); T(16, wt + 1, ht); T(17, wt + 2, ht);
}
// DrawStandardWindowFrame : cadre 9 tuiles autour du rect (menus, oui/non)
static void dt_stdframe(ImDrawList* dl, Nds& db, int xt, int yt, int wt,
                        int ht) {
    if (!std_ready(db)) {
        dl->AddRectFilled(dp(xt * 8, yt * 8), dp((xt + wt) * 8, (yt + ht) * 8),
            B_WHITE);
        return;
    }
    dl->AddRectFilled(dp(xt * 8, yt * 8), dp((xt + wt) * 8, (yt + ht) * 8),
        g_stdfill);
    float x = xt * 8.f, y = yt * 8.f;
    auto T = [&](int t, float cx, float cy) {
        grid_tile(dl, g_stdtex, t, 3, 3, x + cx * 8, y + cy * 8);
    };
    T(0, -1, -1); for (int i = 0; i < wt; i++) T(1, i, -1); T(2, wt, -1);
    for (int i = 0; i < ht; i++) { T(3, -1, i); T(5, wt, i); }
    T(6, -1, ht); for (int i = 0; i < wt; i++) T(7, i, ht); T(8, wt, ht);
}
// compat écrans pas encore convertis : cadre standard DS en espace écran
static void ds_window(ImDrawList* dl, Nds& db, int frame, float ts,
                      ImVec2 p0, ImVec2 p1) {
    (void)frame;
    if (!std_ready(db)) {
        dl->AddRectFilled(p0, p1, B_WHITE, 8.f);
        return;
    }
    dl->AddRectFilled(p0, p1, g_stdfill);
    auto T = [&](int t, ImVec2 a, ImVec2 b) {
        float u0 = (float)(t % 3) / 3, v0 = (float)(t / 3) / 3,
              u1 = u0 + 1.f / 3, v1 = v0 + 1.f / 3;
        dl->AddImage((ImTextureID)(intptr_t)g_stdtex, a, b,
            ImVec2(u0, v0), ImVec2(u1, v1));
    };
    float x0 = p0.x, y0 = p0.y, x1 = p1.x, y1 = p1.y;
    T(0, {x0 - ts, y0 - ts}, {x0, y0});
    T(1, {x0, y0 - ts}, {x1, y0});
    T(2, {x1, y0 - ts}, {x1 + ts, y0});
    T(3, {x0 - ts, y0}, {x0, y1});
    T(5, {x1, y0}, {x1 + ts, y1});
    T(6, {x0 - ts, y1}, {x0, y1 + ts});
    T(7, {x0, y1}, {x1, y1 + ts});
    T(8, {x1, y1}, {x1 + ts, y1 + ts});
}
// curseur scroll : scroll_cursor.NCGR = 12 tuiles = 3 frames 16x16 (2x2)
static bool cur_ready(Nds& db) {
    if (g_curtex) return true;
    auto wn = db.narc("/graphic/pl_winframe.narc");
    if (!wn || wn->count() <= 45) return false;
    auto nc = spr::parse_ncgr(wn->member(22));
    auto wp = spr::parse_nclr(wn->member(25 + (g_opts.frame & 19)));
    if (nc.ntiles < 12 || wp.empty()) return false;
    // composé en grille 2x6 : chaque frame 16x16 empilée
    auto im = spr::compose(nc, 0, 2, 6, wp[0], true);
    if (!im) return false;
    g_curtex = ui_upload(*im);
    return true;
}
static void dt_scrollcur(ImDrawList* dl, Nds& db, float x, float y) {
    if (!cur_ready(db)) return;
    int f = (int)(ImGui::GetTime() * 8.0) % 3;
    float v0 = f * (16.f / 48.f), v1 = v0 + 16.f / 48.f;
    dl->AddImage((ImTextureID)(intptr_t)g_curtex, dp(x, y), dp(x + 16, y + 16),
        ImVec2(0, v0), ImVec2(1, v1));
}

// ── menu debug (F1) : inspecteur complet monde/VM/objets/flags ───────────────
static const char* VM_ST[] = {"DONE","RUN","MSG","WAITBTN","YESNO","MOVE",
    "WAITTIME","WAITFADE","MENU","STARTER","BATTLE","SHOP","WAITANIM"};
static void draw_debug_menu(World& world, Nds& db) {
    ImGui::Begin("Debug OpenPlatinum", nullptr, ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        ImGui::TextDisabled("%s (%s) · %.0f fps", db.game_title().c_str(),
            db.game_code().c_str(), ImGui::GetIO().Framerate);
        ImGui::EndMenuBar();
    }
    if (ImGui::BeginTabBar("##dbg_tabs")) {
        // DBGTAB=n : force la sélection d'un onglet (tests headless)
        static int dbg_tab = getenv("DBGTAB") ? atoi(getenv("DBGTAB")) : -1;
        auto tabfl = [](int i, int sel) {
            return i == sel ? ImGuiTabItemFlags_SetSelected
                            : ImGuiTabItemFlags_None;
        };
        // ── Carte / position ──────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Carte","Map"), nullptr, tabfl(0, dbg_tab))) {
            ImGui::Text("%s — %s (hdr %d)", world.label().c_str(),
                world.mapname().c_str(), world.header());
            Vec3 p = world.player_pos();
            ImGui::Text("%s : %.1f %.1f %.1f  %s %d,%d  dir=%d  %s", tr("Joueur","Player"),
                p.x, p.y, p.z, tr("tuile","tile"), (int)(p.x/16), (int)(p.z/16), world.pdir(),
                world.on_ground() ? tr("sol","ground") : "air");
            static int tpx = 0, tpz = 0;
            if (ImGui::Button(tr("copier pos","copy pos"))) { tpx = (int)(p.x/16); tpz = (int)(p.z/16); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70); ImGui::InputInt("x", &tpx);
            ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputInt("z", &tpz);
            ImGui::SameLine();
            if (ImGui::Button(tr("TP tuile","TP tile"))) world.tp_tile(tpx, tpz);
            // tuile sous le joueur + tuile en face
            {
                int ptx = (int)(p.x / 16), ptz = (int)(p.z / 16);
                static const int DX[4] = {0, 0, -1, 1}, DZ[4] = {1, -1, 0, 0};
                int fx = ptx + DX[world.pdir()], fz = ptz + DZ[world.pdir()];
                ImGui::Text("%s %d,%d beh=0x%02X %s | %s %d,%d beh=0x%02X %s", tr("ici","here"),
                    ptx, ptz, world.tile_beh(ptx, ptz),
                    world.tile_blocked(ptx, ptz) ? tr("BLOQ","BLOCKED") : tr("libre","free"),
                    tr("face","facing"), fx, fz, world.tile_beh(fx, fz),
                    world.tile_blocked(fx, fz) ? tr("BLOQ","BLOCKED") : tr("libre","free"));
            }
            // ── cadence & triche ─────────────────────────────────────────
            ImGui::SliderFloat(tr("vitesse","speed"), &world.dbg_speed, 0.1f, 8.f, "x%.2f");
            if (ImGui::Button("x0.5")) world.dbg_speed = .5f;
            ImGui::SameLine(); if (ImGui::Button("x1")) world.dbg_speed = 1.f;
            ImGui::SameLine(); if (ImGui::Button("x2")) world.dbg_speed = 2.f;
            ImGui::SameLine(); if (ImGui::Button("x4")) world.dbg_speed = 4.f;
            ImGui::SameLine(); if (ImGui::Button("x8")) world.dbg_speed = 8.f;
            ImGui::Checkbox(tr("monde figé (X)","freeze world (X)"), &world.ui_frozen);
            ImGui::SameLine();
            if (ImGui::Button("+1 frame")) world.dbg_frame = true;
            ImGui::SameLine();
            ImGui::Checkbox("noclip", &world.noclip);
            // ── snapshots d'état (rejouer une scène) ──────────────────────
            static World::DbgState snaps[3];
            ImGui::TextDisabled("saves:");
            ImGui::SameLine();
            for (int s = 0; s < 3; s++) {
                ImGui::PushID(900 + s);
                if (ImGui::SmallButton("S")) snaps[s] = world.dbg_save();
                ImGui::SameLine();
                if (ImGui::SmallButton("L") && snaps[s].hdr >= 0)
                    world.dbg_load(snaps[s]);
                ImGui::SameLine();
                ImGui::TextDisabled("%d%s ", s + 1,
                    snaps[s].hdr >= 0 ? "*" : " ");
                ImGui::PopID(); ImGui::SameLine();
            }
            ImGui::NewLine();
            static int hdr = 411;
            ImGui::SetNextItemWidth(90); ImGui::InputInt("hdr", &hdr);
            ImGui::SameLine();
            if (ImGui::Button(tr("Charger","Load"))) world.load_map(hdr, -1);
            ImGui::SameLine();
            if (ImGui::Button(tr("Rejouer init","Replay init"))) world.run_init();
            ImGui::SameLine(); ImGui::TextDisabled("%s", db.header_name(hdr));
            // navigation par nom de map
            if (ImGui::BeginCombo("##maplist", tr("Choisir une map...","Pick a map..."))) {
                for (int i = 0; i < db.header_count(); i++) {
                    const char* n = db.header_name(i);
                    if (!n || !*n) continue;
                    char l[160]; snprintf(l, sizeof l, "%d %s", i, n);
                    if (ImGui::Selectable(l)) { hdr = i; world.load_map(i, -1); }
                }
                ImGui::EndCombo();
            }
            // ── points d'intérêt : téléport direct vers les lieux clés ──
            static const char* POI[] = {
                "TWINLEAF_TOWN", "SANDGEM_TOWN", "SANDGEM_TOWN_POKEMON_RESEARCH_LAB",
                "JUBILIFE_CITY", "OREBURGH_CITY", "FLOAROMA_TOWN",
                "ETERNA_CITY", "HEARTHOME_CITY", "SOLACEON_TOWN",
                "VEILSTONE_CITY", "PASTORIA_CITY", "CELESTIC_TOWN",
                "CANALAVE_CITY", "SNOWPOINT_CITY", "SUNYSHORE_CITY",
                "POKEMON_LEAGUE", "FIGHT_AREA", "SURVIVAL_AREA", "RESORT_AREA",
                "VERITY_LAKEFRONT", "VALOR_LAKEFRONT", "ACUITY_LAKEFRONT",
                "LAKE_VERITY", "LAKE_VALOR", "LAKE_ACUITY",
                "MT_CORONET_OUTSIDE_SOUTH", "SPEAR_PILLAR",
                "STARK_MOUNTAIN_OUTSIDE", "SPRING_PATH", "FLOWER_PARADISE",
            };
            if (ImGui::CollapsingHeader(tr("Points d'intérêt","Points of interest"),
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                for (auto key : POI) {
                    bool found = false;
                    for (int i = 0; i < db.header_count(); i++) {
                        const char* n = db.header_name(i);
                        if (!n || strncmp(n, "MAP_HEADER_", 11)) continue;
                        if (strcmp(n + 11, key)) continue;
                        const MapHeader* h = db.header(i);
                        char l[200];
                        snprintf(l, sizeof l, "%-22s %s", n + 11,
                            h ? db.loc_name(h->label).c_str() : "");
                        if (ImGui::Selectable(l)) { hdr = i; world.load_map(i, -1); }
                        found = true; break;
                    }
                    if (!found)
                        ImGui::TextDisabled("  %s %s", key, tr("(absent)","(missing)"));
                }
            }
            if (ImGui::CollapsingHeader("Routes")) {
                ImGui::BeginChild("rt", ImVec2(0, 200), true);
                for (int i = 0; i < db.header_count(); i++) {
                    const char* n = db.header_name(i);
                    if (!n || !strstr(n, "_ROUTE_")) continue;
                    char l[160]; snprintf(l, sizeof l, "%d %s", i, n + 11);
                    if (ImGui::Selectable(l)) { hdr = i; world.load_map(i, -1); }
                }
                ImGui::EndChild();
            }
            if (ImGui::CollapsingHeader("Warps", ImGuiTreeNodeFlags_DefaultOpen)) {
                int i = 0;
                for (auto& w : world.warps()) {
                    char l[96]; snprintf(l, sizeof l,
                        "#%d (%d,%d) -> hdr%d warp%d", i, w.x, w.z, w.dest, w.dwarp);
                    if (ImGui::Selectable(l)) world.load_map(w.dest, w.dwarp);
                    i++;
                }
            }
            if (ImGui::CollapsingHeader("Coords trigger")) {
                int i = 0;
                for (auto& c : world.coords()) {
                    ImGui::Text("#%d (%d,%d %dx%d) scr%d var%x==%d", i++,
                        c.x, c.z, c.w, c.l, c.script, c.var, c.val);
                }
            }
            if (ImGui::CollapsingHeader("BG events")) {
                int i = 0;
                for (auto& b : world.bgs())
                    ImGui::Text("#%d (%d,%d) scr%d type%d dir%d", i++,
                        (int)b.x, (int)b.z, b.script, b.type, b.dir);
            }
            ImGui::EndTabItem();
        }
        // ── Objets ────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Objets","Objects"), nullptr, tabfl(1, dbg_tab))) {
            static int sel = getenv("DBGOBJ") ? atoi(getenv("DBGOBJ")) : -1;
            if (ImGui::BeginTable("objs", 10,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 220))) {
                ImGui::TableSetupColumn("i");   ImGui::TableSetupColumn("id");
                ImGui::TableSetupColumn("gfx"); ImGui::TableSetupColumn(tr("tuile","tile"));
                ImGui::TableSetupColumn("dir"); ImGui::TableSetupColumn("mv");
                ImGui::TableSetupColumn("scr"); ImGui::TableSetupColumn("flag");
                ImGui::TableSetupColumn(tr("état","state")); ImGui::TableSetupColumn("act");
                ImGui::TableHeadersRow();
                int n = (int)world.objects().size();
                for (int i = 0; i < n; i++) {
                    auto& o = world.objects()[i];
                    Vec3 p = world.obj_pos(i);
                    ImGui::TableNextRow(); ImGui::PushID(i);
                    ImGui::TableNextColumn();
                    char slbl[24]; snprintf(slbl, sizeof slbl, "%d%s", i,
                        world.follower_idx() == i ? "*" : "");
                    if (ImGui::Selectable(slbl, sel == i,
                            ImGuiSelectableFlags_SpanAllColumns))
                        sel = i;
                    ImGui::TableNextColumn(); ImGui::Text("%d", o.id);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d->%d", o.gfx, world.obj_gfx(i));
                    ImGui::TableNextColumn();
                    ImGui::Text("%d,%d y%.0f", (int)(p.x/16), (int)(p.z/16), p.y);
                    ImGui::TableNextColumn(); ImGui::Text("%d", world.obj_dir(i));
                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%d", o.move, world.obj_move(i));
                    ImGui::TableNextColumn();
                    ImGui::Text(o.script==0xFFFF ? "none" : "%d", o.script);
                    ImGui::TableNextColumn(); ImGui::Text("%d", o.flag);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s%s%s",
                        world.obj_gone(i)?"gone ":"", world.obj_hide(i)?"hide ":"",
                        world.obj_lock(i)?"lock":"");
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("tp")) world.tp_tile((int)(p.x/16), (int)(p.z/16));
                    ImGui::SameLine();
                    if (world.obj_gone(i)) {
                        if (ImGui::SmallButton("spawn")) world.obj_respawn(i);
                    } else {
                        if (ImGui::SmallButton("rem")) world.obj_gone_set(i, true);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(world.obj_hide(i)?"vis":"inv"))
                        world.obj_hide_set(i, !world.obj_hide(i));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(world.obj_lock(i)?"rel":"lck"))
                        world.obj_lock_set(i, !world.obj_lock(i));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("%s", tr("* = suiveur · dir/mv = vivant · gfx -> résolu VAR",
                "* = follower · dir/mv = alive · gfx -> resolved VAR"));
            // ── éditeur de l'objet sélectionné ───────────────────────────
            int n = (int)world.objects().size();
            if (sel >= 0 && sel < n) {
                auto& ev = world.obj_event(sel);
                Vec3 op = world.obj_pos(sel);
                ImGui::Separator();
                ImGui::Text("%s #%d : id=%d  gfx=%d->%d  scr=%d  flag=%d  "
                    "trainer=%d  rx,rz=%d,%d  y_ev=%u", tr("Objet","Object"), sel, ev.id, ev.gfx,
                    world.obj_gfx(sel), ev.script, ev.flag, ev.trainer,
                    ev.rx, ev.rz, ev.y);
                int ox = (int)(op.x / 16), oz = (int)(op.z / 16);
                int od2 = world.obj_dir(sel), om = world.obj_move(sel);
                ImGui::SetNextItemWidth(70);
                if (ImGui::InputInt("ox", &ox)) world.obj_set_pos(sel, ox, oz);
                ImGui::SameLine(); ImGui::SetNextItemWidth(70);
                if (ImGui::InputInt("oz", &oz)) world.obj_set_pos(sel, ox, oz);
                ImGui::SameLine(); ImGui::SetNextItemWidth(60);
                if (ImGui::InputInt("dir", &od2)) world.obj_set_dir(sel, od2);
                ImGui::SameLine(); ImGui::SetNextItemWidth(60);
                if (ImGui::InputInt("mv", &om)) world.obj_set_move(sel, om);
                int eg = ev.gfx, es = ev.script, ef = ev.flag;
                ImGui::SetNextItemWidth(70);
                if (ImGui::InputInt("gfx ev", &eg)) ev.gfx = (uint16_t)eg;
                ImGui::SameLine(); ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("scr ev", &es)) ev.script = (uint16_t)es;
                ImGui::SameLine(); ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt(tr("flag ev","evt flag"), &ef)) ev.flag = (uint16_t)ef;
                if (ImGui::Button(tr("Exécuter script","Run script")) && es != 0xFFFF)
                    { world.start_script_dbg(es); world.vm().step(); }
                ImGui::SameLine();
                if (ImGui::Button(tr("tp joueur->ici","tp player->here")))
                    world.tp_tile((int)(op.x / 16), (int)(op.z / 16));
                ImGui::SameLine();
                if (ImGui::Button(tr("obj->joueur","obj->player")))
                    world.obj_set_pos(sel, (int)(world.player_pos().x / 16),
                                      (int)(world.player_pos().z / 16));
                ImGui::SameLine();
                if (ImGui::Button("respawn event")) world.obj_respawn(sel);
            }
            ImGui::EndTabItem();
        }
        // ── Script VM ─────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Script", nullptr, tabfl(2, dbg_tab))) {
            auto& vm = world.vm();
            int st = world.script_state();
            ImGui::Text("%s: %s  script: %d  pc: %d  bank: %d", tr("état","state"),
                st >= 0 && st < 13 ? VM_ST[st] : "?", vm.cur_script,
                vm.pc(), world.cur_bank());
            ImGui::Text("result: %d  wait_t: %d  pending_var: 0x%X  signpost: %d",
                vm.result, vm.wait_t, vm.pending_var, vm.signpost_dest);
            ImGui::Checkbox("ui_frozen", &world.ui_frozen);
            static int scr = 0;
            ImGui::SetNextItemWidth(80); ImGui::InputInt("script id", &scr);
            ImGui::SameLine();
            if (ImGui::Button(tr("Lancer","Run"))) { world.start_script_dbg(scr); world.vm().step(); }
            ImGui::SameLine();
            if (ImGui::Button("Step")) world.vm().step();
            ImGui::Text("%s : %d", tr("pile Call/CallStandard","Call/CallStandard stack"), vm.stack_depth());
            if (ImGui::CollapsingHeader("MoveJobs", ImGuiTreeNodeFlags_DefaultOpen)) {
                int i = 0;
                for (auto& m : world.moves()) {
                    ImGui::Text("#%d obj=%d cmd %zu/%zu %s",
                        i++, m.obj, m.i, m.st.size(), m.done ? "done" : "");
                }
            }
            if (ImGui::CollapsingHeader(tr("Trace opcodes","Opcode trace"),
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                int n = std::min(vm.trace_n, 40);
                ImGui::BeginChild("tr", ImVec2(0, 160), true);
                for (int k = 0; k < n; k++) {
                    auto& e = vm.trace[(vm.trace_n - n + k + 4000) % 40];
                    ImGui::Text("%04X  %02X %s", e.pc, e.op & 0xFF,
                        e.od ? e.od->name : "?");
                }
                if (n) ImGui::SetScrollHereY(1.f);
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        // ── Flags & Vars ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Flags/Vars", nullptr, tabfl(3, dbg_tab))) {
            auto& vm = world.vm();
            static int fid = 0;
            ImGui::SetNextItemWidth(90); ImGui::InputInt("flag id", &fid);
            ImGui::SameLine();
            bool fv = vm.flag(fid);
            if (ImGui::Checkbox("set", &fv)) vm.set_flag(fid, fv);
            static int vid = 0x4000, vval = 0;
            ImGui::SetNextItemWidth(90); ImGui::InputInt("var id", &vid, 1, 16);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90); ImGui::InputInt("val", &vval);
            ImGui::SameLine();
            if (ImGui::Button(tr("écrire","write"))) vm.set_var(vid, vval);
            ImGui::SameLine(); ImGui::TextDisabled("= %d", vm.var(vid));
            static char fflt[32] = "";
            ImGui::SetNextItemWidth(150);
            ImGui::InputTextWithHint(tr("filtre","filter"), "id (ex: 40)", fflt, sizeof fflt);
            int fv2 = ImGui::GetFrameHeightWithSpacing() * 14;
            if (ImGui::BeginTable("vars", 4, ImGuiTableFlags_Borders |
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                    ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, fv2))) {
                ImGui::TableSetupColumn("var"); ImGui::TableSetupColumn("val");
                ImGui::TableSetupColumn("var"); ImGui::TableSetupColumn("val");
                ImGui::TableHeadersRow();
                int col = 0;
                for (auto& kv : vm.vars()) {
                    char k[24]; snprintf(k, sizeof k, "%x", kv.first);
                    if (*fflt && !strstr(k, fflt)) continue;
                    if (col == 0) ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%X", kv.first);
                    ImGui::TableNextColumn();
                    ImGui::PushID(kv.first);
                    int v = kv.second;
                    ImGui::SetNextItemWidth(80);
                    if (ImGui::InputInt("##v", &v)) vm.set_var(kv.first, v);
                    ImGui::PopID();
                    col = (col + 1) & 1;
                }
                ImGui::EndTable();
            }
            ImGui::SameLine();
            if (ImGui::BeginTable("flags", 4, ImGuiTableFlags_Borders |
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                    ImVec2(0, fv2))) {
                ImGui::TableSetupColumn("flag"); ImGui::TableSetupColumn("on");
                ImGui::TableSetupColumn("flag"); ImGui::TableSetupColumn("on");
                ImGui::TableHeadersRow();
                int col = 0;
                for (auto& kv : vm.flags()) {
                    char k[24]; snprintf(k, sizeof k, "%d", kv.first);
                    if (*fflt && !strstr(k, fflt)) continue;
                    if (col == 0) ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", kv.first);
                    ImGui::TableNextColumn();
                    ImGui::PushID(kv.first);
                    bool b = kv.second;
                    if (ImGui::Checkbox("##f", &b)) vm.set_flag(kv.first, b);
                    ImGui::PopID();
                    col = (col + 1) & 1;
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        // ── Joueur ────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Joueur","Player"), nullptr, tabfl(4, dbg_tab))) {
            bool cr = world.can_run();
            if (ImGui::Checkbox("chaussures de sport", &cr)) world.set_can_run(cr);
            int mny = (int)world.money();
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputInt(tr("argent","money"), &mny)) world.vm().money = mny;
            ImGui::Text("badges: %02X  starter: %d  dex: %s",
                world.vm().badges, world.vm().chosen_starter,
                world.dex_obtained() ? "oui" : "non");
            static int bset = 0;
            ImGui::SetNextItemWidth(80); ImGui::InputInt("badge bit", &bset);
            ImGui::SameLine();
            if (ImGui::Button("on")) world.vm().badges |= 1 << bset;
            ImGui::SameLine();
            if (ImGui::Button("off")) world.vm().badges &= ~(1 << bset);
            ImGui::Separator();
            ImGui::Text("%s (%d):", tr("Équipe","Party"), (int)world.party().size());
            int i = 0;
            for (auto& pm : world.party())
                ImGui::Text("  #%d %s nv%d pv%d", ++i,
                    world.species_name(pm.species).c_str(), pm.level, pm.hp);
            static int sp = 1, lv = 5;
            ImGui::SetNextItemWidth(80); ImGui::InputInt(tr("espèce","species"), &sp);
            ImGui::SameLine(); ImGui::TextDisabled("%s", world.species_name(sp).c_str());
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputInt(tr("nv","lv"), &lv);
            ImGui::SameLine();
            if (ImGui::Button(tr("donner","give")))
                world.debug_mon(sp, lv);
            ImGui::SameLine();
            if (ImGui::Button("Tortipouss")) world.debug_mon();
            ImGui::SameLine();
            if (ImGui::Button(tr("Vider équipe","Clear party"))) world.vm().party.clear();
            // ── sac ──────────────────────────────────────────────────────
            ImGui::Separator();
            ImGui::Text("%s (%d):", tr("Sac","Bag"), (int)world.bag().size());
            static int iid = 4, ict = 5;
            ImGui::SetNextItemWidth(80); ImGui::InputInt("item id", &iid);
            ImGui::SameLine(); ImGui::TextDisabled("%s", world.item_name(iid).c_str());
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputInt(tr("qté","qty"), &ict);
            ImGui::SameLine();
            if (ImGui::Button(tr("ajouter","add"))) world.vm().bag_add(iid, ict);
            ImGui::SameLine();
            if (ImGui::Button(tr("vider sac","clear bag"))) world.vm().bag_clear();
            ImGui::SameLine();
            if (ImGui::Button("12 Poké Balls")) world.vm().bag_add(4, 12);
            ImGui::BeginChild("bagv", ImVec2(0, 110), true);
            for (auto& kv : world.bag())
                ImGui::Text("%d x%d  %s", kv.first, kv.second,
                    world.item_name(kv.first).c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ── Combat / rencontres ───────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Combat","Battle"), nullptr, tabfl(5, dbg_tab))) {
            ImGui::Text("%s: %s   %s: %d", tr("combat","battle"),
                world.battle_active() ? tr("EN COURS","ACTIVE") : tr("aucun","none"),
                tr("taux rencontre carte","map encounter rate"), world.enc_rate());
            static int fsp = 396, flv = 8, ftid = 1;
            ImGui::SetNextItemWidth(80); ImGui::InputInt(tr("espèce","species"), &fsp);
            ImGui::SameLine(); ImGui::TextDisabled("%s",
                world.species_name(fsp).c_str());
            ImGui::SameLine(); ImGui::SetNextItemWidth(60);
            ImGui::InputInt(tr("nv","lv"), &flv);
            ImGui::SameLine();
            if (ImGui::Button(tr("Combat sauvage","Wild battle"))) world.force_wild(fsp, flv);
            ImGui::SetNextItemWidth(90); ImGui::InputInt(tr("dresseur id","trainer id"), &ftid);
            ImGui::SameLine();
            if (ImGui::Button(tr("Combat dresseur","Trainer battle"))) world.force_trainer(ftid);
            if (ImGui::BeginTable("enc", 8, ImGuiTableFlags_Borders |
                    ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("slot"); ImGui::TableSetupColumn(tr("espèce","species"));
                ImGui::TableSetupColumn(tr("nom","name"));  ImGui::TableSetupColumn(tr("nv","lv"));
                ImGui::TableSetupColumn("slot"); ImGui::TableSetupColumn(tr("espèce","species"));
                ImGui::TableSetupColumn(tr("nom","name"));  ImGui::TableSetupColumn(tr("nv","lv"));
                ImGui::TableHeadersRow();
                for (int i = 0; i < 6; i++) {
                    ImGui::TableNextRow();
                    for (int half = 0; half < 2; half++) {
                        int s = i + half * 6, lv = 0;
                        int sp = world.enc_slot(s, &lv);
                        ImGui::TableNextColumn(); ImGui::Text("%d", s);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", sp > 0 ? sp : 0);
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", sp > 0 ?
                            world.species_name(sp).c_str() : "-");
                        ImGui::TableNextColumn(); ImGui::Text("%d", lv);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("%s", tr("poids DS: 20/20/10/10/10/10/5/5/4/4/1/1",
                "DS weights: 20/20/10/10/10/10/5/5/4/4/1/1"));
            ImGui::EndTabItem();
        }
        // ── Messages ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Messages", nullptr, tabfl(6, dbg_tab))) {
            static int bank = 0;
            ImGui::SetNextItemWidth(80); ImGui::InputInt(tr("banque","bank"), &bank);
            ImGui::SameLine();
            ImGui::TextDisabled("%s = %d · %d %s", tr("courante","current"),
                world.cur_bank(), world.msg_count(bank),
                tr("entrées","entries"));
            int n = world.msg_count(bank);
            ImGui::BeginChild("msgs", ImVec2(0, 320), true);
            for (int i = 0; i < n && i < 2000; i++) {
                ImGui::PushID(i);
                ImGui::Text("%d: %s", i, world.msg(bank, i).c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ── Rendu ─────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Rendu","Render"), nullptr, tabfl(7, dbg_tab))) {
            ImGui::Checkbox("flat", &world.flat);
            ImGui::Checkbox("verbose", &world.verbose);
            ImGui::Text("cells %d · surf %d · verts %d · objs %d · warps %d",
                world.cell_count(), world.surf_count(), world.vert_count(),
                world.obj_count(), world.warp_count());
            ImGui::Text("fade: %.2f col %06X  banner %.1f",
                world.fade_alpha(), world.fade_color(), world.banner_t());
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("env vars actives :","active env vars:"));
            for (const char* v : {"OPDBG","OBJDBG","MOVDBG","INPDBG","SNAPDBG",
                                  "WARPDBG","PLDBG","FRDBG"})
                if (getenv(v)) ImGui::BulletText("%s", v);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// options.cfg : sauvegarde/chargement + application des options graphiques
static void opt_save() {
    FILE* f = fopen("../options.cfg", "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d\n",
        g_opts.text_speed, g_opts.battle_scene, g_opts.battle_style,
        g_opts.sound, g_opts.buttons, g_opts.frame,
        g_opts.fullscreen, g_opts.vsync, g_opts.texfilter, g_opts.winscale,
        g_opts.msaa, g_opts.aniso, g_opts.lang);
    fclose(f);
}
static bool opt_load(SDL_Window* win) {
    FILE* f = fopen("../options.cfg", "r");
    if (!f) return false;
    fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d",
        &g_opts.text_speed, &g_opts.battle_scene, &g_opts.battle_style,
        &g_opts.sound, &g_opts.buttons, &g_opts.frame,
        &g_opts.fullscreen, &g_opts.vsync, &g_opts.texfilter,
        &g_opts.winscale, &g_opts.msaa, &g_opts.aniso, &g_opts.lang);
    fclose(f);
    g_opts.text_speed = g_opts.text_speed < 0 ? 0 :
        g_opts.text_speed > 2 ? 2 : g_opts.text_speed;
    g_opts.winscale = g_opts.winscale < 0 ? 0 :
        g_opts.winscale > 2 ? 2 : g_opts.winscale;
    g_opts.frame &= 31;
    g_opts.msaa = g_opts.msaa < 0 ? 0 : g_opts.msaa > 3 ? 3 : g_opts.msaa;
    g_opts.aniso = g_opts.aniso < 0 ? 0 : g_opts.aniso > 4 ? 4 : g_opts.aniso;
    g_opts.lang &= 1;
    // applique les options graphiques au démarrage
    static const int RW[] = {1280, 1600, 1920};
    static const int RH[] = {720, 900, 1080};
    if (g_opts.winscale)
        SDL_SetWindowSize(win, RW[g_opts.winscale], RH[g_opts.winscale]);
    if (g_opts.fullscreen) SDL_SetWindowFullscreen(win, true);
    SDL_GL_SetSwapInterval(g_opts.vsync);
    rnd::set_tex_filter(g_opts.texfilter);
    rnd::set_aniso(g_opts.aniso);
    rnd::set_msaa(g_opts.msaa ? (1 << g_opts.msaa) : 0);   // 1->2x 2->4x 3->8x
    rnd::set_composite(!getenv("NOEDGE"));                 // contours DS
    return true;
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // audio : scaffold (pas encore de SDAT)
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 44100};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow("OpenPlatinum",
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "GL ctx: %s\n", SDL_GetError()); return 1; }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    if (!rnd::init()) { fprintf(stderr, "GL init KO\n"); return 1; }
    bool cfg_ok = opt_load(win);                // options.cfg si présent

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // fonte : couvre latin étendu + ponctuation “”«» + symboles ♂♀★♪●■◆←↑↓→
    {
        ImGuiIO& io = ImGui::GetIO();
        static const ImWchar ranges[] = {
            0x20, 0x17F,          // latin + latin-1 + étendu A (accents FR)
            0x2010, 0x205F,       // ponctuation générale (“”«»…)
            0x2190, 0x21FF,       // flèches
            0x25A0, 0x25FF,       // formes (●■◆▲)
            0x2600, 0x26FF,       // symboles (★♪♂♀)
            0
        };
        const char* fonts[] = {"C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/tahoma.ttf"};
        ImFont* f = nullptr;
        for (auto p : fonts) {
            f = io.Fonts->AddFontFromFileTTF(p, 19.f, nullptr, ranges);
            if (f) break;
        }
        if (!f) io.Fonts->AddFontDefault();
    }
    ImGui_ImplSDL3_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ROM
    std::string dummy;
    std::string rom = find_rom(argc, argv, dummy);
    Nds db;
    if (rom.empty() || !db.mount(rom)) {
        fprintf(stderr, "%s", tr("ROM introuvable — placez un .nds dans rom/\n",
                                 "ROM not found — put a .nds in rom/\n"));
        // continue quand même (fenêtre + imgui de debug)
    } else {
        // headers.bin à côté de l'exe ou dans data/
        const char* hd[] = {"data/headers.bin", "headers.bin",
            "../data/headers.bin", "../../data/headers.bin"};
        for (auto p : hd)
            if (fs::exists(p)) { db.load_headers(p); break; }
        // langue UI : par défaut celle de la ROM (CPUF->FR, sinon EN)
        if (!cfg_ok && db.game_code().size() >= 4)
            g_opts.lang = db.game_code()[3] == 'F' ? 0 : 1;
    }

    World world;
    world.init(&db);
    bool dbg_win = false;                       // F1 / --dbg
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flat")) world.flat = true;
        if (!strcmp(argv[i], "--verbose")) world.verbose = true;
        if (!strcmp(argv[i], "--dbg")) dbg_win = true;
        if (!strcmp(argv[i], "--uvdbg")) rnd::set_uvdbg(i + 1 < argc ? atoi(argv[i + 1]) : 1);
    }
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--dumpnarc") && i + 2 < argc) {
            auto n = db.narc(argv[i + 1]);
            if (n) {
                int m = atoi(argv[i + 2]);
                const Bytes& b = n->member(m);
                char fn[256]; snprintf(fn, 256, "../_narc_%d.bin", m);
                FILE* f = fopen(fn, "wb");
                if (f) { fwrite(b.data(), 1, b.size(), f); fclose(f); }
                printf("[dumpnarc] %s -> %s (%zu B)\n", argv[i + 1], fn, b.size());
            }
        }
        if (!strcmp(argv[i], "--dumpmsg")) {          // debug : banque pl_msg
            auto pm = db.narc(Nds::P_PLMSG);
            int b = atoi(argv[i + 1]);
            if (pm && b >= 0 && b < pm->count()) {
                auto v = msgbank_decode(pm->member(b));
                for (size_t k = 0; k < v.size(); k++)
                    printf("[%d] %s\n", (int)k, v[k].c_str());
            }
            return 0;
        }
    }
    // ── intro DS : GAME FREAK -> titre Giratina -> menu -> Prof. Sorbier ────
    enum ISt { I_OFF, I_GF, I_TITLE, I_MENU, I_SEQ, I_HELP, I_BALL, I_GENDER,
               I_NAME, I_FADE };
    enum IAf { A_HELP, A_BALL, A_GENDER, A_RNAME, A_DONE };
    int ist = I_OFF;
    std::vector<int> imsgq;                 // file de messages (banque 389)
    int iafter = A_HELP;
    float it = 0, ichars = 0;               // chrono état + machine à écrire
    std::string idlg;                       // page courante affichée
    std::vector<std::string> ipages;        // pages du message (DS : \n\n)
    int ipage = 0;
    int icur = -1;                          // id du message affiché
    int ihelp = 0, igender = 0, iyn_sel = 0;
    int iyn = 0;                            // 1=genre 2=joueur 3=rival
    int iname_who = 0;                      // 0=joueur 1=rival
    char iname[32] = "";
    int ikb_page = 0, ikb_cx = 0, ikb_cy = 0; // clavier nommage : page/curseur
    int kb_preset = -1;                      // sélection nom prédéfini rival
    // aides du clavier de nommage (partagées touches/clic)
    auto u8enc = [](int cp, char* u8) -> int {
        if (cp < 0x80) { u8[0] = (char)cp; return 1; }
        if (cp < 0x800) {
            u8[0] = (char)(0xC0 | (cp >> 6));
            u8[1] = (char)(0x80 | (cp & 63));
            return 2;
        }
        u8[0] = (char)(0xE0 | (cp >> 12));
        u8[1] = (char)(0x80 | ((cp >> 6) & 63));
        u8[2] = (char)(0x80 | (cp & 63));
        return 3;
    };
    std::function<int()> nm_nlen;
    std::function<void()> nm_del, nm_confirm;
    std::function<void(int,int)> nm_press;
    bool iball_bad = false;                 // mauvais bouton sur la balle
    float post_fade = 0;                    // fondu entrant après la chambre
    ImVec4 ball_rc = {0,0,0,0};             // rect écran de la balle (I_BALL)
    auto iset_msg = [&](int id) {           // message -> pages (séparées \n\n)
        icur = id;
        std::string s = world.msg_fmt(389, id);
        ipages.clear(); ipage = 0;
        size_t p = 0;
        while (true) {
            size_t q = s.find("\n\n", p);
            ipages.push_back(s.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) break;
            p = q + 2;
        }
        while (ipages.size() > 1 && ipages.back().empty()) ipages.pop_back();
        idlg = ipages[0]; ichars = 0;
    };
    auto inext_msg = [&]() { iset_msg(imsgq.front()); imsgq.erase(imsgq.begin()); };
    auto ipush = [&](std::initializer_list<int> ids, int after) {
        imsgq.assign(ids.begin(), ids.end()); iafter = after;
        inext_msg(); ist = I_SEQ;
    };
    auto intro_go = [&]() {                 // fin de file -> étape suivante
        switch (iafter) {
        case A_HELP:   ist = I_HELP; ihelp = 0; break;
        case A_BALL:   ist = I_BALL; world.intro_set_frame(1); break;
        case A_GENDER: ist = I_GENDER; igender = 0;
                       world.intro_set_frame(0);
                       iset_msg(21); break;
        case A_RNAME:  ist = I_NAME; iname_who = 1;
                       strcpy(iname, world.rival_name().c_str());
                       iset_msg(28); break;
        case A_DONE:   ist = I_FADE; it = 0; break;
        }
    };
    auto imenu = [&]() {                    // menu après START
        ist = I_MENU; ihelp = fs::exists("../save.dat") ? 0 : 1;
    };
    auto iadvance = [&]() {                 // E : complète / page suiv. / suite
        if (ichars < (float)idlg.size()) { ichars = (float)idlg.size(); return; }
        if (ipage + 1 < (int)ipages.size()) {
            ipage++; idlg = ipages[ipage]; ichars = 0; return;
        }
        if (imsgq.empty()) intro_go(); else inext_msg();
    };
    auto intro_e = [&]() {                  // E générique (tests --talk)
        if (ichars < (float)idlg.size()) { ichars = (float)idlg.size(); return; }
        switch (ist) {
        case I_GF:    ist = I_TITLE; it = 0; break;
        case I_TITLE: imenu(); break;
        case I_BALL:  ipush({19, 20}, A_GENDER); break;
        case I_SEQ:   iadvance(); break;
        default: break;
        }
    };
    // intro DS par défaut ; sautée par les flags de test ou --nointro
    bool want_intro = db.file_count() > 0;
    for (int i = 1; i < argc && want_intro; i++) {
        static const char* SKIP[] = {"--map", "--pos", "--ui", "--walk",
            "--tp", "--wild", "--starter", "--xmenu", "--talk", "--rec",
            "--bench", "--debugmon", "--norun", "--nointro", "--flat",
            "--grid", "--fly", "--cam", "--shot", "--gif", nullptr};
        for (int j = 0; SKIP[j]; j++)
            if (!strcmp(argv[i], SKIP[j])) want_intro = false;
    }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--intro") || !strcmp(argv[i], "--intro-at"))
            want_intro = db.file_count() > 0;
    if (want_intro) {
        int iat = 0;                                // --intro-at N : saute à l'étape
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--intro-at")) iat = atoi(argv[i + 1]);
        if (iat <= 0) ipush({0, 1}, A_HELP);        // direct : création perso
        else switch (iat) {
            case 1: ist = I_GF; break;
            case 2: ist = I_TITLE; break;
            case 3: ist = I_MENU; ihelp = 0; break;
            case 4: ist = I_HELP; ihelp = 0; iset_msg(1); break;
            case 5: ist = I_BALL; iset_msg(17); break;
            case 6: ist = I_GENDER; iset_msg(21); break;
            case 7: ist = I_NAME; iname_who = 0; iset_msg(24); break;
            case 8: ist = I_NAME; iname_who = 1; iset_msg(28); break;
            case 9: ipush({30}, A_DONE); break;
            default: ipush({0, 1}, A_HELP); break;
        }
    }
    if (db.file_count() > 0 && ist == I_OFF) {
        int start = arg_map(argc, argv, 415);           // 415 = chambre du joueur (vrai départ DS)
        world.load_map(start, -1);
        if (start == 415) {                             // sPlayerStartLocation (location.c)
            world.set_pos(4 * 16 + 8, 6 * 16 + 8);      // x=4 z=6, FACE_UP
            world.set_dir(1);
        }
    }
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--pos")) {
                double px = 0, pz = 0;
                if (strchr(argv[i + 1], ',')) {       // --pos x,z
                    sscanf(argv[i + 1], "%lf,%lf", &px, &pz);
                } else if (i + 2 < argc) {            // --pos x z
                    px = atof(argv[i + 1]); pz = atof(argv[i + 2]); i++;
                }
                world.set_pos((float)px * 16 + 8, (float)pz * 16 + 8);
            }

    bool running = true;
    int shot_at = arg_shot(argc, argv), frames = 0;
    int gif_end = -1;
    int gif_at = arg_gif(argc, argv, &gif_end);
    bool recording = false;
    int rec_n = 0;
    int talk_at = -1, walk_n = 0, walk_d = 0, walk_at = 20, xmenu_at = -1;
    struct Walk { int n, d, at; };
    std::vector<Walk> walks;
    int ui_arg = -1, ui_arg_state = 1;
    struct TpEv { int f, x, z; };           // téléport debug (--tp F,X,Z)
    std::vector<TpEv> tps;
    struct WildEv { int f, sp, lv; };       // combat forcé (--wild F,sp,lv)
    std::vector<WildEv> wilds;
    std::vector<std::pair<int, SDL_Keycode>> keyev;   // --keys F:c,...
    int starter_sel = 0;                    // curseur écran starter
    int starter_st = 0;                     // 0 = choix balle, 1 = confirmation
    bool starter_yn = false;                // curseur oui/non confirm
    int starter_at = -1;                    // --starter F : ouverture test
    // ── menu X authentique DS ─────────────────────────────────────────────
    enum UISt { UI_NONE, UI_XMENU, UI_BAG, UI_PARTY, UI_SAVE, UI_OPTIONS, UI_CARD };
    int ui_st = UI_NONE, menu_sel = 0, save_st = 0, bag_pocket = 0;
    // sac DS : sous-états (curseur / menu actions / cible équipe / jeter / oui-non)
    int bag_sel = 0, bag_scroll = 0, bag_act = -1, bag_asel = 0;
    int bag_party = -1, bag_psel = 0, bag_toss = -1, bag_tossn = 1;
    int bag_yn = -1, bag_ysel = 0;
    std::vector<int> bag_acts;                       // entrées du menu actions
    float bag_note_t = 0; std::string bag_note;
    int yn_sel = 0;                             // curseur choix oui/non
    float save_t = 0;
    std::vector<int> menu_opts;
    auto xmenu_opts = [&world]() {           // options visibles (ordre DS)
        std::vector<int> v;
        if (world.dex_obtained()) v.push_back(0);
        if (world.has_starter()) v.push_back(1);
        v.insert(v.end(), {2, 3, 4, 5, 6});  // Sac, nom, Sauver, Options, Quitter
        return v;
    };
    auto ui_enter = [&](int opt) -> int {
        switch (opt) {
            case 0: return UI_XMENU;         // Pokédex : pas d'appli encore
            case 1: return UI_PARTY;
            case 2: bag_pocket = 0; menu_sel = 0; return UI_BAG;
            case 3: return UI_CARD;
            case 4: save_st = 0; menu_sel = 0; return UI_SAVE;
            case 5: menu_sel = 0; return UI_OPTIONS;
            case 6: return UI_NONE;          // Quitter
        }
        return UI_NONE;
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--talk"))
            talk_at = (i + 1 < argc && isdigit(argv[i + 1][0])) ? atoi(argv[i + 1]) : 6;
        if (!strcmp(argv[i], "--debugmon"))            // test : starter en équipe
            world.debug_mon();
        if (!strcmp(argv[i], "--badge") && i + 1 < argc)   // test : badge N
            world.vm().badges |= 1 << (atoi(argv[i + 1]) & 7);
        if (!strcmp(argv[i], "--addmon") && i + 1 < argc) { // test : espèce,nv[,mv]
            int sp, lv, mv = 0;
            if (sscanf(argv[i + 1], "%d,%d,%d", &sp, &lv, &mv) >= 2)
                world.debug_mon(sp, lv, mv);
        }
        if (!strcmp(argv[i], "--debugbag")) {          // test : sac rempli
            static const int FILL[][2] = {
                {4,12},{17,5},{19,3},{25,1},{27,2},{77,2},
                {328,1},{337,1},{428,3},{445,1},{1,0}
            };
            for (auto& f : FILL) if (f[1]) world.vm().bag_add(f[0], f[1]);
        }
        if (!strcmp(argv[i], "--xmenu") && i + 1 < argc)
            xmenu_at = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--ui") && i + 1 < argc) {
            const char* u = argv[i + 1];
            ui_arg = 60;
            ui_arg_state = !strcmp(u, "party") ? UI_PARTY
                : !strcmp(u, "save") ? UI_SAVE : !strcmp(u, "options") ? UI_OPTIONS
                : !strcmp(u, "card") ? UI_CARD
                : !strncmp(u, "bag", 3) ? UI_BAG : UI_XMENU;
            if (!strncmp(u, "bag", 3) && isdigit(u[3]))   // --ui bagN : poche N
                bag_pocket = u[3] - '0';
        }
        if (!strcmp(argv[i], "--walk") && i + 2 < argc) {
            walk_n = atoi(argv[i + 1]);
            walk_d = argv[i + 2][0] == 'n' ? 1 : argv[i + 2][0] == 'e' ? 3 :
                     argv[i + 2][0] == 'o' ? 2 : 0;   // défaut sud
            if (i + 3 < argc && isdigit(argv[i + 3][0]))
                walk_at = atoi(argv[i + 3]);          // départ différé
            walks.push_back({walk_n, walk_d, walk_at});  // répétable
        }
        if (!strcmp(argv[i], "--tp") && i + 1 < argc) {   // --tp F,X,Z : téléport
            int f, tx, tz;
            if (sscanf(argv[i + 1], "%d,%d,%d", &f, &tx, &tz) == 3)
                tps.push_back({f, tx, tz});
        }
        if (!strcmp(argv[i], "--wild") && i + 1 < argc) {   // --wild F,sp,lv
            int f, sp, lv;
            if (sscanf(argv[i + 1], "%d,%d,%d", &f, &sp, &lv) == 3)
                wilds.push_back({f, sp, lv});
        }
        if (!strcmp(argv[i], "--starter") && i + 1 < argc)  // écran starter test
            starter_at = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--keys") && i + 1 < argc) {  // "F:c,F:c,..." : touche c à la frame F
            for (char* tok = strtok(argv[i + 1], ","); tok; tok = strtok(nullptr, ",")) {
                int f = atoi(tok);
                const char* k = strchr(tok, ':');
                if (!k++) continue;
                SDL_Keycode kc = SDL_GetKeyFromName(k);
                if (kc == SDLK_UNKNOWN && strlen(k) == 1) kc = (SDL_Keycode)*k;
                if (kc != SDLK_UNKNOWN) keyev.push_back({f, kc});
            }
        }
    }
    // clavier de nommage : taille/effacement/validation/pression de touche
    nm_nlen = [&] {
        size_t i = 0; int n = 0;
        while (i < strlen(iname)) msg_next_utf8(iname, i), n++;
        return n;
    };
    nm_del = [&] {
        size_t i = 0, last = 0;
        while (i < strlen(iname)) { last = i; msg_next_utf8(iname, i); }
        iname[last] = 0;
    };
    nm_confirm = [&] {
        if (!iname[0]) return;
        if (!iname_who) {
            world.set_identity(igender, iname, "");
            iset_msg(25); iyn = 2;
        } else {
            world.set_identity(igender, world._pname, iname);
            iset_msg(29); iyn = 3;
        }
        iyn_sel = 0;
    };
    nm_press = [&](int cx, int cy) {
        if (cy < 5) {                             // lettre
            const char* row = KB_ROWS[ikb_page][cy];
            size_t kk = 0; int ci = 0, cp = 0;
            while (kk < strlen(row)) {
                int q = msg_next_utf8(row, kk);
                if (ci++ == cx) { cp = q; break; }
            }
            if (cp <= 0 || cp == ' ' || nm_nlen() >= 7) return;
            char u8[8]; u8[u8enc(cp, u8)] = 0;
            strncat(iname, u8, sizeof(iname) - strlen(iname) - 1);
        } else {                                  // ligne d'accueil
            if (cx < 6) ikb_page = cx / 2;
            else if (cx < 8) { if (iname_who) kb_preset = 0; }
            else if (cx < 11) nm_del();
            else nm_confirm();
        }
    };
    Uint64 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                ev.button.button == SDL_BUTTON_LEFT && ist == I_BALL) {
                // tactile : clic sur la balle
                float mx = ev.button.x, my = ev.button.y;
                if (ball_rc.z > ball_rc.x) {
                    float rw = ball_rc.z - ball_rc.x;
                    float rh = ball_rc.w - ball_rc.y;
                    float cx = (ball_rc.x + ball_rc.z) / 2,
                          cy = ball_rc.y + rh * 0.44f;
                    float dx = mx - cx, dy = my - cy;
                    float br = rw * 0.085f;
                    if (dx*dx + dy*dy < br*br) {           // bouton central
                        if (iball_bad) { iball_bad = false; iset_msg(17); }
                        else ipush({19, 20}, A_GENDER);
                    } else if (mx > ball_rc.x && mx < ball_rc.z &&
                               my > ball_rc.y && my < ball_rc.w) {
                        iball_bad = true; iset_msg(18);    // à côté
                    }
                }
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                ev.button.button == SDL_BUTTON_LEFT && ist == I_NAME &&
                !iyn) {
                // tactile : clic sur le clavier du nommage (coords DS)
                float dxx = (ev.button.x - g_udx) / g_uds,
                      dyy = ev.button.y / g_uds;
                if (kb_preset >= 0) {           // liste noms rival
                    if (dxx >= 160 && dxx < 248 && dyy >= 16 && dyy < 72) {
                        kb_preset = std::min(3,
                            std::max(0, (int)((dyy - 22) / 11)));
                        strncpy(iname,
                            world.msg(389, 37 + kb_preset).c_str(),
                            sizeof(iname) - 1);
                        kb_preset = -1;
                    } else kb_preset = -1;
                } else if (dyy >= 52 && dyy < 132 && dxx >= 16 && dxx < 224) {
                    ikb_cx = (int)((dxx - 16) / 16);
                    ikb_cy = (int)((dyy - 52) / 16);
                    nm_press(ikb_cx, ikb_cy);
                } else if (dyy >= 132 && dyy < 148 && dxx >= 16 && dxx < 224) {
                    nm_press((int)((dxx - 16) / 16), 5);
                }
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_ESCAPE) running = false;
                if (ev.key.key == SDLK_F8) {
                    recording = !recording;
                    printf("[rec] %s\n", recording ? "start" : "stop");
                }
                if (world.battle().active) {           // combat : route les touches
                    int k = ev.key.key;
                    int c = (k == SDLK_E || k == SDLK_RETURN ||
                             k == SDLK_SPACE) ? 'E' :
                        (k == SDLK_Q || k == SDLK_BACKSPACE) ? 'Q' :
                        (k == SDLK_W || k == SDLK_UP) ? 'W' :
                        (k == SDLK_S || k == SDLK_DOWN) ? 'S' :
                        (k == SDLK_A || k == SDLK_LEFT) ? 'A' :
                        (k == SDLK_D || k == SDLK_RIGHT) ? 'D' : 0;
                    if (world.battle().st == Battle::BAG) {   // picker Balls
                        auto& bt2 = world.battle();
                        int nb = (int)world.bag_items(2).size();
                        if (c == 'W' && nb)
                            bt2.sel = (bt2.sel + nb - 1) % nb;
                        else if (c == 'S' && nb)
                            bt2.sel = (bt2.sel + 1) % nb;
                        else if (c == 'E' && nb && bt2.sel < nb)
                            world.battle_throw(
                                world.bag_items(2)[bt2.sel].first);
                        else if (c == 'Q' || (c == 'E' && !nb))
                            world.battle_key('Q');            // retour choix
                        continue;
                    }
                    if (c) world.battle_key(c);
                    continue;
                }
                if (ist != I_OFF) {                      // intro : ses touches
                    int k = ev.key.key;
                    bool kE = k == SDLK_E || k == SDLK_RETURN || k == SDLK_SPACE;
                    bool kQ = k == SDLK_Q || k == SDLK_BACKSPACE;
                    bool kU = k == SDLK_W || k == SDLK_UP;
                    bool kD = k == SDLK_S || k == SDLK_DOWN;
                    bool kL = k == SDLK_A || k == SDLK_LEFT;
                    bool kR = k == SDLK_D || k == SDLK_RIGHT;
                    bool typing = ist == I_NAME;
                    bool done_typing = !typing ||
                        !ImGui::GetIO().WantTextInput;
                    if (ist == I_GF) { if (kE) { ist = I_TITLE; it = 0; } }
                    else if (ist == I_TITLE) { if (kE) imenu(); }
                    else if (ist == I_MENU) {
                        int n = fs::exists("../save.dat") ? 2 : 1;
                        if (kU || kD) ihelp = (ihelp + 1) % (n + 1);
                        if (ihelp == n) ihelp = 0;      // 2 entrées max
                        if (kE) {
                            if (fs::exists("../save.dat") && ihelp == 0) {
                                printf("[load] %s\n",
                                    world.load("../save.dat") ? "ok" : "echec");
                                ist = I_OFF;
                            } else {
                                strcpy(iname, "");
                                ipush({0, 1}, A_HELP);
                            }
                        }
                    }
                    else if (ist == I_HELP) {
                        if (kU) ihelp = (ihelp + 2) % 3;
                        if (kD) ihelp = (ihelp + 1) % 3;
                        if (kE) {
                            if (ihelp == 0) ipush({2,3,4,5,9}, A_HELP);
                            else if (ihelp == 1)
                                ipush({10,11,12,13,14,15,9}, A_HELP);
                            else ipush({16,17}, A_BALL);
                        }
                    }
                    else if (ist == I_BALL) {
                        if (kE) {
                            if (iball_bad) {
                                iball_bad = false;
                                iset_msg(17);
                            } else ipush({19, 20}, A_GENDER);
                        }
                        else if (kQ) {
                            iball_bad = true;
                            iset_msg(18);
                        }
                    }
                    else if (ist == I_GENDER) {
                        if (iyn) {
                            if (kU || kD) iyn_sel ^= 1;
                            if (kE || kQ) {
                                if (kQ || iyn_sel == 1) {   // NON : rechoix
                                    iyn = 0; iset_msg(21);
                                    ichars = 0;
                                } else {                     // OUI : nommer
                                    iyn = 0; ist = I_NAME; iname_who = 0;
                                    strcpy(iname, "");
                                    iset_msg(24);
                                    ichars = 0;
                                }
                            }
                        } else {
                            if (kL || kR) igender ^= 1;
                            if (kE) {                       // confirme le genre
                                iset_msg(igender ? 23 : 22); iyn = 1; iyn_sel = 0;
                            }
                        }
                    }
                    else if (ist == I_NAME) {
                        if (iyn) {
                            if (kU || kD) iyn_sel ^= 1;
                            if (kE || kQ) {
                                if (kQ || iyn_sel == 1) {   // NON : ressaisie
                                    iyn = 0;
                                    iset_msg(iname_who ? 28 : 24);
                                } else if (iname_who == 0) {
                                    iyn = 0; ipush({27}, A_RNAME);
                                } else {
                                    iyn = 0; ipush({30}, A_DONE);
                                }
                            }
                        } else {
                            // clavier de nommage DS : flèches/Entrée/Retour
                            // pilotent ; les lettres tapent directement
                            bool aU = k == SDLK_UP, aD = k == SDLK_DOWN;
                            bool aL = k == SDLK_LEFT, aR = k == SDLK_RIGHT;
                            bool aE = k == SDLK_RETURN,
                                 aX = k == SDLK_BACKSPACE;
                            if (kb_preset >= 0) {     // liste noms rival
                                if (aU || aD)
                                    kb_preset = (kb_preset + (aD ? 1 : 3)) & 3;
                                if (aE) {
                                    strncpy(iname,
                                        world.msg(389, 37 + kb_preset).c_str(),
                                        sizeof(iname) - 1);
                                    kb_preset = -1;
                                }
                                if (aX) kb_preset = -1;
                            } else {
                                if (aU) ikb_cy = (ikb_cy + 5) % 6;
                                if (aD) ikb_cy = (ikb_cy + 1) % 6;
                                if (aL && iname_who && ikb_cx == 0)
                                    kb_preset = 0;
                                else if (aL) ikb_cx = (ikb_cx + 12) % 13;
                                if (aR) ikb_cx = (ikb_cx + 1) % 13;
                                if (aX) nm_del();
                                if (aE) nm_press(ikb_cx, ikb_cy);
                                else if (!aX && !aU && !aD && !aL && !aR &&
                                         k >= ' ' && k <= '~' &&
                                         nm_nlen() < 7) {
                                    char u8[2] = {(char)k, 0};
                                    if (k >= 'a' && k <= 'z' && ikb_page == 0)
                                        u8[0] = (char)toupper(k);
                                    strncat(iname, u8,
                                        sizeof(iname) - strlen(iname) - 1);
                                }
                            }
                        }
                    }
                    else if (ist == I_SEQ) {
                        if (kE && done_typing) iadvance();
                        if (kQ && !imsgq.empty()) {          // saut rapide
                            imsgq.clear(); intro_go();
                        }
                    }
                    continue;
                }
                if (ev.key.key == SDLK_E || ev.key.key == SDLK_RETURN ||
                    ev.key.key == SDLK_SPACE) {
                    if (world.yesno()) {
                        world.choose_yesno(yn_sel == 0);
                        yn_sel = 0;
                    } else if (world.starter_open()) {
                        // DS : balle choisie -> confirmation "prendre ?" -> oui/non
                        if (starter_st == 0) { starter_st = 1; starter_yn = false; }
                        else if (starter_yn) {
                            world.starter_pick(starter_sel); starter_st = 0;
                        } else starter_st = 0;
                    } else if (!ImGui::GetIO().WantCaptureKeyboard)
                        world.action();
                }
                if (world.starter_open() && starter_st == 0) {  // curseur <- ->
                    if (ev.key.key == SDLK_A || ev.key.key == SDLK_LEFT)
                        starter_sel = (starter_sel + 2) % 3;
                    if (ev.key.key == SDLK_D || ev.key.key == SDLK_RIGHT)
                        starter_sel = (starter_sel + 1) % 3;
                }
                if (world.starter_open() && starter_st == 1 &&
                    (ev.key.key == SDLK_A || ev.key.key == SDLK_D ||
                     ev.key.key == SDLK_LEFT || ev.key.key == SDLK_RIGHT))
                    starter_yn = !starter_yn;
                if (world.starter_open() && starter_st == 1 &&
                    (ev.key.key == SDLK_Q || ev.key.key == SDLK_BACKSPACE ||
                     ev.key.key == SDLK_ESCAPE))
                    starter_st = 0;                             // retour choix
                if (world.yesno() &&
                    (ev.key.key == SDLK_W || ev.key.key == SDLK_S ||
                     ev.key.key == SDLK_UP || ev.key.key == SDLK_DOWN))
                    yn_sel ^= 1;
                if (ev.key.key == SDLK_Q && world.yesno()) {
                    world.choose_yesno(false);
                    yn_sel = 0;
                }
                // ── sac DS : navigation (liste / actions / équipe / jeter) ────
                if (ui_st == UI_BAG) {
                    auto items = world.bag_items(bag_pocket);
                    int nb = (int)items.size(), key = ev.key.key;
                    bool up = key == SDLK_UP || key == SDLK_W,
                         dn = key == SDLK_DOWN || key == SDLK_S,
                         lf = key == SDLK_LEFT || key == SDLK_A,
                         rt = key == SDLK_RIGHT || key == SDLK_D,
                         ok = key == SDLK_E || key == SDLK_RETURN,
                         bk = key == SDLK_Q || key == SDLK_BACKSPACE;
                    auto note = [&](const std::string& s) {
                        bag_note = s; bag_note_t = 2.5f;
                    };
                    if (bag_yn >= 0) {              // confirmation OUI/NON
                        if (up || dn || lf || rt) bag_ysel ^= 1;
                        else if (ok) {
                            if (bag_ysel == 0) {    // OUI -> jette
                                world.vm().bag_add(bag_yn, -bag_tossn);
                                char tn[160];
                                snprintf(tn, sizeof(tn),
                                    tr("%s x%d a été jeté",
                                       "%s x%d was thrown away"),
                                    world.item_name(bag_yn).c_str(), bag_tossn);
                                note(tn);
                            }
                            bag_yn = -1; bag_toss = -1; bag_act = -1;
                        } else if (bk) bag_yn = -1, bag_toss = -1;
                    } else if (bag_toss >= 0) {     // quantité à jeter
                        int mx = 1;
                        for (auto& kv : world.bag())
                            if (kv.first == bag_toss) mx = kv.second;
                        if (up) bag_tossn = std::min(bag_tossn + 1, mx);
                        if (dn) bag_tossn = std::max(bag_tossn - 1, 1);
                        if (lf) bag_tossn = std::max(bag_tossn - 10, 1);
                        if (rt) bag_tossn = std::min(bag_tossn + 10, mx);
                        if (ok) { bag_yn = bag_toss; bag_ysel = 1; }
                        if (bk) bag_toss = -1;
                    } else if (bag_party >= 0) {    // cible équipe (soin/rappel)
                        int np = (int)world.party().size();
                        if (np && up) bag_psel = (bag_psel + np - 1) % np;
                        if (np && dn) bag_psel = (bag_psel + 1) % np;
                        if (ok && np && bag_psel < np) {
                            auto& pm = world.vm().party[bag_psel];
                            int id = bag_party, mx = world.mon_maxhp(
                                pm.species, pm.level, pm.iv, pm.evs[0]);
                            bool rev = world.item_field_use(id) == 2;
                            bool done = false;
                            if (rev) {
                                if (pm.hp > 0) note(tr("Cela n'aurait aucun effet.",
                                    "It wouldn't have any effect."));
                                else {
                                    int h = world.item_heal(id);
                                    pm.hp = (h == 0xFF) ? mx : mx / 2;
                                    done = true;
                                }
                            } else if (pm.hp <= 0)
                                note(tr("Cela n'aurait aucun effet.",
                                        "It wouldn't have any effect."));
                            else if (pm.hp >= mx)
                                note(tr("Ses PV sont déjà au max !",
                                        "Its HP is already full!"));
                            else {
                                int h = world.item_heal(id);
                                pm.hp = std::min(mx, pm.hp + (h >= 0xFE ? mx : h));
                                done = true;
                            }
                            if (done) {
                                world.vm().bag_add(id, -1);
                                char tn[160];
                                snprintf(tn, sizeof(tn),
                                    tr("%s récupère des PV !",
                                       "%s regained HP!"),
                                    world.species_name(pm.species).c_str());
                                note(tn);
                                bag_party = -1; bag_act = -1;
                            }
                        }
                        if (bk) bag_party = -1;
                    } else if (bag_act >= 0) {      // menu contextuel actions
                        int na = (int)bag_acts.size();
                        if (up) bag_asel = (bag_asel + na - 1) % na;
                        if (dn) bag_asel = (bag_asel + 1) % na;
                        if (ok && bag_asel < na) {
                            int id = bag_act, a = bag_acts[bag_asel];
                            if (a == 0) {               // UTILISER
                                if (world.item_field_use(id) == 3) {
                                    // Cendresacrée : ranime tout le groupe
                                    int n = 0;
                                    for (auto& m : world.vm().party)
                                        if (m.hp <= 0) {
                                            m.hp = world.mon_maxhp(
                                                m.species, m.level, m.iv, m.evs[0]);
                                            n++;
                                        }
                                    if (n) {
                                        world.vm().bag_add(id, -1);
                                        note(tr("Le groupe est ranimé !",
                                                "The party was revived!"));
                                        bag_act = -1;
                                    } else note(tr("Cela n'aurait aucun effet.",
                                        "It wouldn't have any effect."));
                                } else if (world.party().empty())
                                    note(tr("Pas de Pokémon dans l'équipe.",
                                            "No Pokémon in the party."));
                                else { bag_party = id; bag_psel = 0; }
                            } else if (a == 1) {        // ENREGISTRER / retirer
                                bool r = world.registered_item() == id;
                                world.set_registered(r ? 0 : id);
                                char tn[160];
                                snprintf(tn, sizeof(tn),
                                    r ? tr("%s désenregistré", "%s unregistered")
                                      : tr("%s enregistré sur Y",
                                           "%s registered to Y"),
                                    world.item_name(id).c_str());
                                note(tn); bag_act = -1;
                            } else if (a == 2) {        // JETER
                                int mx = 1;
                                for (auto& kv : world.bag())
                                    if (kv.first == id) mx = kv.second;
                                if (mx > 1) { bag_toss = id; bag_tossn = 1; }
                                else { bag_yn = id; bag_ysel = 1; bag_tossn = 1;
                                       bag_toss = id; }
                            } else bag_act = -1;        // ANNULER
                        }
                        if (bk) bag_act = -1;
                    } else {                            // liste des objets
                        if (up && nb) bag_sel = (bag_sel + nb - 1) % nb;
                        if (dn && nb) bag_sel = (bag_sel + 1) % nb;
                        if (lf) bag_pocket = (bag_pocket + 7) % 8,
                                bag_sel = 0, bag_scroll = 0;
                        if (rt) bag_pocket = (bag_pocket + 1) % 8,
                                bag_sel = 0, bag_scroll = 0;
                        if (ok && nb && bag_sel < nb) { // ouvre le menu actions
                            int id = items[bag_sel].first;
                            bag_acts.clear();
                            if (world.item_field_use(id)) bag_acts.push_back(0);
                            if (world.item_registrable(id)) bag_acts.push_back(1);
                            if (world.item_tossable(id)) bag_acts.push_back(2);
                            bag_acts.push_back(3);      // ANNULER toujours
                            bag_act = id; bag_asel = 0;
                        }
                        if (bk || key == SDLK_X) ui_st = UI_XMENU;
                    }
                    world.ui_frozen = true;
                } else if (ui_st != UI_NONE) {
                    // nb de lignes navigables : options = 11, oui/non sauvegarde
                    // = 2, sinon taille du menu X
                    int n = ui_st == UI_OPTIONS ? 14
                        : (ui_st == UI_SAVE && save_st == 0) ? 2
                        : (int)menu_opts.size();
                    int key = ev.key.key;
                    if (key == SDLK_UP || key == SDLK_W)
                        menu_sel = n ? (menu_sel + n - 1) % n : 0;
                    if (key == SDLK_DOWN || key == SDLK_S)
                        menu_sel = n ? (menu_sel + 1) % n : 0;
                    if (key == SDLK_E || key == SDLK_RETURN ||
                        (ui_st != UI_OPTIONS &&
                         (key == SDLK_D || key == SDLK_RIGHT))) {
                        if (ui_st == UI_XMENU && menu_sel < n)
                            ui_st = ui_enter(menu_opts[menu_sel]);
                        else if (ui_st == UI_SAVE && save_st == 0) {
                            save_st = (menu_sel == 0) ? 1 : 4;    // Oui/Non
                            save_t = 0;
                        } else if (ui_st == UI_OPTIONS && menu_sel == 13) {
                            opt_save();                            // CONFIRMER
                            ui_st = UI_XMENU, menu_sel = 5;
                        }
                        else if (ui_st == UI_SAVE && save_st == 3)
                            ui_st = UI_NONE;
                    }
                    // options : <- -> change la valeur de la ligne
                    if (ui_st == UI_OPTIONS && menu_sel < 13 &&
                        (key == SDLK_LEFT || key == SDLK_RIGHT ||
                         key == SDLK_A || key == SDLK_D)) {
                        int dir = (key == SDLK_LEFT || key == SDLK_A) ? -1 : 1;
                        switch (menu_sel) {
                        case 0:
                            g_opts.text_speed = (g_opts.text_speed + dir + 3) % 3; break;
                        case 1:
                            g_opts.battle_scene ^= 1; break;
                        case 2:
                            g_opts.battle_style ^= 1; break;
                        case 3:
                            g_opts.sound ^= 1; break;
                        case 4:
                            g_opts.buttons = (g_opts.buttons + dir + 3) % 3; break;
                        case 5:
                            g_opts.frame = (g_opts.frame + dir + 20) % 20; break;
                        case 6:
                            g_opts.lang ^= 1; break;
                        case 7:
                            g_opts.fullscreen ^= 1;
                            SDL_SetWindowFullscreen(win, g_opts.fullscreen);
                            break;
                        case 8: {
                            g_opts.winscale = (g_opts.winscale + dir + 3) % 3;
                            static const int RW[] = {1280, 1600, 1920};
                            static const int RH[] = {720, 900, 1080};
                            SDL_SetWindowSize(win, RW[g_opts.winscale],
                                              RH[g_opts.winscale]);
                            break;
                        }
                        case 9:
                            g_opts.texfilter ^= 1;
                            rnd::set_tex_filter(g_opts.texfilter);
                            break;
                        case 10:
                            g_opts.msaa = (g_opts.msaa + dir + 4) % 4;
                            rnd::set_msaa(g_opts.msaa ? (1 << g_opts.msaa) : 0);
                            break;
                        case 11:
                            g_opts.aniso = (g_opts.aniso + dir + 5) % 5;
                            rnd::set_aniso(g_opts.aniso ? (1 << g_opts.aniso) : 0);
                            break;
                        case 12:
                            g_opts.vsync ^= 1;
                            SDL_GL_SetSwapInterval(g_opts.vsync);
                            break;
                        }
                    }
                    if (key == SDLK_LEFT || key == SDLK_A) {
                        if (ui_st == UI_BAG)
                            bag_pocket = (bag_pocket + 7) % 8, menu_sel = 0;
                        else if (ui_st == UI_SAVE && save_st == 0)
                            menu_sel ^= 1;
                    }
                    if (key == SDLK_RIGHT && ui_st == UI_BAG)
                        bag_pocket = (bag_pocket + 1) % 8, menu_sel = 0;
                    if (key == SDLK_Q || key == SDLK_BACKSPACE ||
                        (key == SDLK_X && ui_st != UI_SAVE)) {
                        if (ui_st == UI_XMENU || ui_st == UI_SAVE) ui_st = UI_NONE;
                        else ui_st = UI_XMENU;
                    }
                    world.ui_frozen = ui_st != UI_NONE;
                } else if (ev.key.key == SDLK_X && !world.script_busy()) {
                    menu_opts = xmenu_opts();
                    menu_sel = 0; ui_st = UI_XMENU;
                    world.ui_frozen = true;
                }
                if (ev.key.key == SDLK_F5)
                    printf("[save] %s\n", world.save("../save.dat") ? "ok" : "echec");
                if (ev.key.key == SDLK_F9)
                    printf("[load] %s\n", world.load("../save.dat") ? "ok" : "echec");
            }
        }
        Uint64 now = SDL_GetTicks();
        float dt = (now - last) / 1000.f;
        if (dt > 0.05f) dt = 0.05f;
        last = now;

        const bool* keys = SDL_GetKeyboardState(nullptr);
        float ix = 0, iz = 0;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    iz -= 1;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  iz += 1;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  ix -= 1;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) ix += 1;
        // --walk N d [F] : simule une marche de N frames dans la direction d
        for (auto& w : walks)
            if (frames >= w.at && frames < w.at + w.n) {
                if (w.d == 0) iz += 1;
                else if (w.d == 1) iz -= 1;
                else if (w.d == 2) ix -= 1;
                else ix += 1;
            }
        // bouton B DS = course (après les chaussures de sport)
        world.run_held = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        if (getenv("INPDBG") && frames % 30 == 0 && (ix || iz || !walks.empty()))
            printf("[main-inp] ix=%.0f iz=%.0f wck=%d upd=%d\n", ix, iz,
                (int)ImGui::GetIO().WantCaptureKeyboard,
                (int)!ImGui::GetIO().WantCaptureKeyboard);
        if (ist == I_OFF && !ImGui::GetIO().WantCaptureKeyboard)
            world.update(dt * world.dbg_speed, ix, iz);
        // pas-à-pas : une frame simulée même si le monde est figé (menu debug)
        if (world.dbg_frame) {
            bool fz = world.ui_frozen; world.ui_frozen = false;
            world.update(1.f / 60.f, 0, 0); world.ui_frozen = fz;
            world.dbg_frame = false;
        }
        // --talk N : appuie E à la frame 25 puis toutes les 20 frames
        if ((walk_n || talk_at > 0) && frames % 40 == 0)
            printf("[pos] f%d p=(%.0f,%.0f) t=(%d,%d)\n", frames,
                world.player_pos().x, world.player_pos().z,
                (int)floorf(world.player_pos().x / 16), (int)floorf(world.player_pos().z / 16));
        if (talk_at > 0 && frames >= 25 && frames % 20 == 5) {
            if (world.battle().active) world.battle_key('E'); // avance le combat
            else if (world.yesno()) world.choose_yesno(true);     // répond Oui
            else if (world.starter_open()) {            // même logique que E
                if (starter_st == 0) { starter_st = 1; starter_yn = false; }
                else if (starter_yn) { world.starter_pick(starter_sel); starter_st = 0; }
                else starter_st = 0;
            }
            else if (ist != I_OFF) intro_e();           // avance l'intro
            else world.action();
            printf("[talk] f%d state=%d dlg=%.100s\n", frames,
                world.script_state(), world.dialog().c_str());
            talk_at--;
        }
        // --tp F,X,Z : téléport debug
        for (auto& t : tps)
            if (t.f == frames)
                world.set_pos(t.x * 16.f + 8, t.z * 16.f + 8);
        // --xmenu F : ouvre le menu X à la frame F (test)
        if (xmenu_at >= 0 && frames == xmenu_at) {
            menu_opts = xmenu_opts();
            menu_sel = 0; ui_st = UI_XMENU;
            world.ui_frozen = true;
        }
        // --ui <nom> : ouvre directement un écran (test)
        if (ui_arg >= 0 && frames == ui_arg) {
            menu_opts = xmenu_opts(); menu_sel = 0;
            ui_st = ui_arg_state;
            world.ui_frozen = ui_st != UI_NONE;
        }
        // --keys : touche synthétique à la frame F (test)
        for (auto it2 = keyev.begin(); it2 != keyev.end();)
            if (it2->first == frames) {
                SDL_Event kev{};
                kev.type = SDL_EVENT_KEY_DOWN;
                kev.key.key = it2->second;
                kev.key.scancode = SDL_GetScancodeFromKey(it2->second,
                    SDL_KMOD_NONE);
                kev.key.down = true;
                SDL_PushEvent(&kev);
                it2 = keyev.erase(it2);
            } else ++it2;
        // --wild F,sp,lv : force un combat sauvage à la frame F (test)
        for (auto& wld : wilds)
            if (frames == wld.f) world.force_wild(wld.sp, wld.lv);
        if (starter_at >= 0 && frames == starter_at)
            world.force_starter();          // test écran starter DS

        int sw, sh;
        SDL_GetWindowSize(win, &sw, &sh);
        float asp = sh > 0 ? (float)sw / sh : 1.f;
        Mat4 proj = world.cam_proj(asp);    // preset caméra DS du header courant

        glViewport(0, 0, sw, sh);
        rnd::resize(sw, sh);                // recrée le FBO MSAA si besoin
        rnd::begin(proj);
        if (ist == I_TITLE || ist == I_MENU)
            world.draw_title(asp);          // scène 3D Giratina (titledemo)
        else if (ist == I_OFF) {
            rnd::set_clear(0.53f, 0.67f, 0.85f);   // ciel du monde
            if (world.starter_open())
            if (world.starter_open())
                world.draw_starter(asp);    // vraie scène 3D ev_pokeselect
            else world.draw(proj, sw, sh);
        }

        // ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        static bool f1_prev = false;
        {
            bool f1 = keys[SDL_SCANCODE_F1];
            if (f1 && !f1_prev) dbg_win = !dbg_win;
            f1_prev = f1;
        }
        if (dbg_win) draw_debug_menu(world, db);
        // ── intro DS ─────────────────────────────────────────────────────
        if (ist != I_OFF) {
            it += dt;
            ichars += dt * (g_opts.text_speed == 0 ? 25.f :
                            g_opts.text_speed == 2 ? 90.f : 45.f);
            if (ist == I_GF && it > 2.4f) { ist = I_TITLE; it = 0; }
            if (ist == I_FADE && it > 1.1f) {        // fin fondu -> chambre
                world.load_map(415, -1);
                world.set_pos(4 * 16 + 8, 6 * 16 + 8);
                world.set_dir(1);
                ist = I_OFF; post_fade = 1.f;
            }
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            ImVec2 io = ImGui::GetIO().DisplaySize;
            // fond noir de base (pas sur le titre/menu : la 3D Giratina dessine)
            if (ist != I_TITLE && ist != I_MENU)
                bg->AddRectFilled({0,0}, io, IM_COL32(0, 0, 0, 255));
            auto img_c = [&](int id, float cx, float cy, float sc) {
                uint32_t t = world.intro_tex(id);
                if (!t) return;
                int tw, th; world.intro_tex_size(id, &tw, &th);
                float w = tw * sc, h = th * sc;
                bg->AddImage((ImTextureID)(uintptr_t)t,
                    {cx - w/2, cy - h/2}, {cx + w/2, cy + h/2});
            };
            auto img_b = [&](int id, float cx, float by, float sc) {
                uint32_t t = world.intro_tex(id);
                if (!t) return;
                int tw, th; world.intro_tex_size(id, &tw, &th);
                float w = tw * sc, h = th * sc;
                bg->AddImage((ImTextureID)(uintptr_t)t,
                    {cx - w/2, by - h}, {cx + w/2, by});
            };
            ImVec4 last_rc = {0,0,0,0};               // rect réel de l'écran bas
            auto scr_img = [&](int id, const ImVec4& rc, bool fit) {
                uint32_t t = world.intro_tex(id);
                if (!t) return;
                int tw, th; world.intro_tex_size(id, &tw, &th);
                if (!tw) return;
                if (!fit) {                          // étire dans la zone
                    bg->AddImage((ImTextureID)(uintptr_t)t,
                        {rc.x, rc.y}, {rc.z, rc.w});
                    last_rc = rc;
                } else {                             // aspect natif centré
                    float sc = fminf((rc.z - rc.x) / tw, (rc.w - rc.y) / th);
                    float w = tw * sc, h = th * sc;
                    float cx = (rc.x + rc.z) / 2, cy = (rc.y + rc.w) / 2;
                    bg->AddImage((ImTextureID)(uintptr_t)t,
                        {cx - w/2, cy - h/2}, {cx + w/2, cy + h/2});
                    last_rc = {cx - w/2, cy - h/2, cx + w/2, cy + h/2};
                }
            };
            // split natif : zone haute (prof/dialogue) + zone basse (tactile)
            bool sorb = ist == I_SEQ || ist == I_HELP || ist == I_BALL ||
                        ist == I_GENDER || ist == I_NAME;
            int slide = -1;                          // contenu zone basse
            if (ist == I_BALL) slide = 3;
            else if (ist == I_SEQ && iafter == A_HELP)
                slide = icur == 2 || icur == 3 ? 8 :
                        icur == 4 ? 9 : icur == 5 ? 10 : -1;
            float top_h = slide >= 0 ? io.y * 0.60f : io.y;
            ImVec4 scrT = {0, 0, io.x, top_h};
            ImVec4 scrB = {0, top_h, io.x, io.y};
            // écran du bas : balle / aides boutons, aspect 4:3 centré
            if (slide >= 0) {
                scr_img(slide, scrB, true);
                bg->AddRectFilled({0, top_h - 2}, {io.x, top_h + 2},
                    IM_COL32(255, 184, 77, 200));      // séparateur
            }
            ball_rc = ist == I_BALL ? last_rc : ImVec4{0,0,0,0};
            // fond dégradé bleu sur la zone haute
            if (sorb) scr_img(2, scrT, false);
            if (ist == I_GF)
                scr_img(0, scrT, false);    // plein écran comme sur DS
            else if (ist == I_TITLE) {
                int lw, lh; world.intro_tex_size(1, &lw, &lh);
                float ls = fminf(io.x * 0.62f / lw, io.y * 0.42f / lh);
                img_c(1, io.x/2, io.y * 0.26f, ls);
                const std::string& ps = world.msg_fmt(609, 0);   // APPUYEZ SUR START
                if ((int)(ImGui::GetTime() * 1.6) & 1) {
                    g_uds = io.y / 192.f;
                    g_udx = (io.x - 256.f * g_uds) * 0.5f;
                    ds_font_ready(db);
                    float pw = dt_text_w(ps);
                    dt_text(bg, ps, (256.f - pw) * 0.5f, 160.f, 5);
                }
            }
            else if (ist == I_MENU) {
                img_c(1, io.x/2, io.y * 0.24f, fminf(io.x * 0.4f, 500.f) / 256.f);
                static const char* MF[] = {"CONTINUER", "NOUVELLE PARTIE"};
                static const char* ME[] = {"CONTINUE", "NEW GAME"};
                bool has_save = fs::exists("../save.dat");
                int ni = has_save ? 2 : 1;
                g_uds = io.y / 192.f;
                g_udx = (io.x - 256.f * g_uds) * 0.5f;
                ds_font_ready(db);
                // liste DS : fenêtre standard à droite + flèche curseur
                dt_stdframe(bg, db, 17, 9, 14, ni * 2 + 2);
                for (int i = 0; i < ni; i++) {
                    float iy = 80 + i * 16.f;
                    if (i == ihelp) {
                        ImVec2 c0 = dp(136, iy + 2), c1 = dp(136, iy + 14),
                               c2 = dp(142, iy + 8);
                        bg->AddTriangleFilled(c0, c1, c2,
                            IM_COL32(232, 40, 40, 255));
                    }
                    dt_text(bg, g_opts.lang ? ME[i] : MF[i], 144, iy, 0);
                }
            }
            else if (ist == I_GENDER) {
                world.intro_set_pframe(
                    (int)(ImGui::GetTime() * 7.f) & 3); // vague en boucle
                float sc = io.y * 0.0039f;
                float by = io.y * 0.66f;               // pieds au-dessus boîte
                img_b(5, io.x * 0.30f, by, sc);        // Lucas
                img_b(6, io.x * 0.70f, by, sc);        // Dawn
                // cadre de sélection (sprite + label)
                float bx = (igender ? 0.70f : 0.30f) * io.x;
                bg->AddRect({bx - 33.f * sc, by - 128.f * sc - 8.f},
                            {bx + 33.f * sc, by + 26.f},
                            IM_COL32(255, 184, 77, 255), 8.f, 0, 3.f);
                ImVec2 sz = ImGui::CalcTextSize(
                    igender ? tr("FILLE","GIRL") : tr("GARÇON","BOY"));
                bg->AddText({bx - sz.x/2, by + 5.f},
                    IM_COL32(255, 184, 77, 255),
                    igender ? tr("FILLE","GIRL") : tr("GARÇON","BOY"));
            }
            else if (ist == I_NAME && !iyn) {
                // vrai écran de nommage DS : fond namein + grille clavier 13x6
                g_uds = io.y / 192.f;
                g_udx = (io.x - 256.f * g_uds) * 0.5f;
                ds_font_ready(db);
                uint32_t nbt = world.intro_tex(13);
                if (nbt)
                    bg->AddImage((ImTextureID)(uintptr_t)nbt,
                        dp(0, 0), dp(256, 192));
                // nom saisi dans le champ blanc + curseur clignotant
                {
                    std::string nm = iname;
                    float nw2 = dt_text_w(nm);
                    dt_text(bg, nm, (256.f - nw2) * 0.5f, 14.f, 0);
                    if ((int)(ImGui::GetTime() * 2.f) & 1)
                        dt_text(bg, "_", (256.f - nw2) * 0.5f + nw2 + 1.f,
                            14.f, 0);
                }
                // grille clavier de la page courante (membres 6-9 de namein)
                uint32_t kt = world.intro_tex(14 + ikb_page);
                if (kt)
                    bg->AddImage((ImTextureID)(uintptr_t)kt,
                        dp(0, 44), dp(256, 156));
                // lettres centrées dans les cellules 16x16
                for (int r = 0; r < 5; r++) {
                    const char* row = KB_ROWS[ikb_page][r];
                    for (int c = 0; c < 13; c++) {
                        int cp = 0; int ci = 0;
                        for (size_t kk = 0; kk < strlen(row); ) {
                            int q = msg_next_utf8(
                                std::string(row), kk);
                            if (ci++ == c) { cp = q; break; }
                        }
                        if (cp <= 0 || cp == ' ') continue;
                        char u8[8]; int n8 = 0;
                        if (cp < 0x80) u8[n8++] = (char)cp;
                        else if (cp < 0x800) {
                            u8[n8++] = (char)(0xC0 | (cp >> 6));
                            u8[n8++] = (char)(0x80 | (cp & 63));
                        } else {
                            u8[n8++] = (char)(0xE0 | (cp >> 12));
                            u8[n8++] = (char)(0x80 | ((cp >> 6) & 63));
                            u8[n8++] = (char)(0x80 | (cp & 63));
                        }
                        u8[n8] = 0;
                        float cw = dt_text_w(u8);
                        int ccol = (ikb_cy == r && ikb_cx == c) ? 3 : 0;
                        dt_text(bg, u8,
                            16.f + c * 16.f + (16.f - cw) * 0.5f,
                            52.f + r * 16.f, ccol);
                    }
                }
                // boutons d'accueil : MAJ min AUTRE | EFF OK (+ NOMS rival)
                {
                    static const char* BT[] = {"MAJ", "min", "AUT", "EFF", "OK"};
                    static const float BX[] = {16, 48, 80, 144, 192};
                    static const float BW[] = {32, 32, 32, 48, 32};
                    for (int b = 0; b < 5; b++) {
                        bool sel = ikb_cy == 5 &&
                            ikb_cx >= (b == 3 ? 8 : b == 4 ? 11 : b * 2) &&
                            ikb_cx < (b == 3 ? 11 : b == 4 ? 13 : b * 2 + 2);
                        bool cur = b < 3 && ikb_page == b;
                        if (sel || cur) {
                            ImU32 fc = sel ? IM_COL32(255,184,77,210)
                                           : IM_COL32(120,120,120,210);
                            bg->AddRectFilled(dp(BX[b], 132), dp(BX[b] + BW[b], 148),
                                fc, 6.f);
                        }
                        float tw2 = dt_text_w(BT[b]);
                        dt_text(bg, BT[b],
                            BX[b] + (BW[b] - tw2) * 0.5f, 134.f,
                            sel ? 0 : (cur ? 3 : 0));
                    }
                    if (iname_who) {              // cellules 6-7 : liste prénoms
                        bool sel = ikb_cy == 5 && ikb_cx >= 6 && ikb_cx < 8;
                        if (sel)
                            bg->AddRectFilled(dp(112, 132), dp(144, 148),
                                IM_COL32(255, 184, 77, 210), 6.f);
                        float tw2 = dt_text_w("NOMS");
                        dt_text(bg, "NOMS", 112.f + (32.f - tw2) * 0.5f,
                            134.f, 0);
                    }
                }
                // curseur clavier orange sur la cellule courante
                if (ikb_cy < 5)
                    bg->AddRect(dp(16 + ikb_cx * 16, 52 + ikb_cy * 16),
                        dp(16 + ikb_cx * 16 + 16, 52 + ikb_cy * 16 + 16),
                        IM_COL32(255, 184, 77, 255), 4.f, 0, 2.5f);
                else {                            // bande bouton du curseur
                    float x = 192, w = 32;
                    if (ikb_cx < 6)      { x = 16.f + (ikb_cx / 2) * 32.f; }
                    else if (ikb_cx < 8) { x = 112.f; }
                    else if (ikb_cx < 11){ x = 144.f; w = 48.f; }
                    bg->AddRect(dp(x, 132), dp(x + w, 148),
                        IM_COL32(255, 184, 77, 255), 4.f, 0, 2.5f);
                }
                // noms prédéfinis rival : fenêtre DS par-dessus le clavier
                if (iname_who && kb_preset >= 0) {
                    dt_stdframe(bg, db, 20, 2, 11, 7);
                    for (int i = 0; i < 4; i++) {
                        std::string pn = world.msg(389, 37 + i);
                        dt_text(bg, pn, 168.f, 22.f + i * 11.f,
                            kb_preset == i ? 3 : 0);
                        if (kb_preset == i) {
                            ImVec2 a = dp(166, 24 + i * 11),
                                   b = dp(166, 34 + i * 11),
                                   c2 = dp(162, 29 + i * 11);
                            bg->AddTriangleFilled(a, b, c2,
                                IM_COL32(232, 40, 40, 255));
                        }
                    }
                }
            }
            // portrait Sorbier : monte depuis le bas + bouge en parlant
            if (ist == I_SEQ || ist == I_HELP || ist == I_BALL) {
                float rise = fminf(1.f, it / 0.45f);
                float off = (1.f - rise) * (1.f - rise) * top_h * 0.5f;
                bool speaking = !idlg.empty() && ichars < (float)idlg.size();
                float t_gui = (float)ImGui::GetTime();
                float amp = speaking ? 6.f : 2.f;
                float spd = speaking ? 7.5f : 1.6f;
                float bob = fabsf(sinf(t_gui * spd)) * -amp;
                img_b(4, io.x * 0.5f, top_h - top_h * 0.02f + off + bob,
                      top_h * 0.0058f);
            }
            // boîte de message DS : cadre 18 tuiles, fond blanc, NFGR.
            // Le canvas 256x192 couvre l'écran du haut (top_h).
            bool show_box = !idlg.empty() && ist != I_GF && ist != I_TITLE &&
                            ist != I_FADE;
            if (show_box) {
                g_uds = top_h / 192.f;
                g_udx = (io.x - 256.f * g_uds) * 0.5f;
                ds_font_ready(db);
                dt_msgbox(bg, db, 2, 19, 27, 4);
                // texte visible : n premiers caractères UTF-8
                size_t vi = 0; int vc = 0;
                const std::string& s = idlg;
                while (vi < s.size() && vc < (int)ichars) {
                    vc++;
                    uint8_t b = (uint8_t)s[vi];
                    vi += (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 1;
                }
                std::string vis = s.substr(0, vi);
                // wrap glouton par mots à 216 px DS, origine (16,152)
                float lh = g_dfont.nglyphs ? (float)g_dfont.cellH : 16.f;
                float adv0 = dt_text_w(" ");
                if (adv0 < 2.f) adv0 = 8.f;
                float lx = 0.f; int li = 0;
                for (size_t k = 0; k < vis.size(); ) {
                    if (vis[k] == '\n') { li++; lx = 0; k++; continue; }
                    if (vis[k] == ' ') { lx += adv0; k++; continue; }
                    size_t we = k;
                    while (we < vis.size() && vis[we] != ' ' &&
                           vis[we] != '\n') we++;
                    std::string w = vis.substr(k, we - k);
                    float ww = dt_text_w(w);
                    if (lx > 0 && lx + ww > 216.f) { li++; lx = 0; }
                    dt_text(bg, w, 16 + lx, 152 + li * lh, 0);
                    lx += ww + adv0;
                    k = we;
                }
                if (ichars >= (float)s.size() && !iyn)
                    dt_scrollcur(bg, db, 232 - 16 - 4, 184 - 16);
                // oui/non DS : fenêtre standard (25,13,6,4) + flèche 8x16
                if (iyn) {
                    dt_stdframe(bg, db, 25, 13, 6, 4);
                    const char* yn[2] = {world.msg(389, 34).c_str(),
                                         world.msg(389, 35).c_str()};
                    for (int i = 0; i < 2; i++) {
                        float iy = 104 + i * 16.f;
                        if (i == iyn_sel) {
                            ImVec2 c0 = dp(200, iy + 2), c1 = dp(200, iy + 14),
                                   c2 = dp(206, iy + 8);
                            bg->AddTriangleFilled(c0, c1, c2,
                                IM_COL32(232, 40, 40, 255));
                        }
                        dt_text(bg, yn[i], 208, iy, 0);
                    }
                }
                // menu aide 3 choix : fenêtre standard + flèche curseur
                if (ist == I_HELP) {
                    dt_stdframe(bg, db, 17, 7, 14, 10);
                    for (int i = 0; i < 3; i++) {
                        float iy = 60 + i * 20.f;
                        if (i == ihelp) {
                            ImVec2 c0 = dp(136, iy + 2), c1 = dp(136, iy + 14),
                                   c2 = dp(142, iy + 8);
                            bg->AddTriangleFilled(c0, c1, c2,
                                IM_COL32(232, 40, 40, 255));
                        }
                        dt_text(bg, world.msg(389, 31 + i), 144, iy, 0);
                    }
                }
            }
            // fondu sortant vers la chambre
            if (ist == I_FADE) {
                bg->AddRectFilled({0,0}, io,
                    IM_COL32(0, 0, 0, (int)(255 * fminf(1.f, it / 0.9f))));
            }
        }
        if (post_fade > 0.f) {                  // entrée dans la chambre
            post_fade = fmaxf(0.f, post_fade - dt * 1.6f);
            ImGui::GetBackgroundDrawList()->AddRectFilled({0,0},
                ImGui::GetIO().DisplaySize,
                IM_COL32(0, 0, 0, (int)(255 * post_fade)));
        }
        // ── bandeau de zone (3 s à l'entrée de map) ────────────────────────
        if (ist == I_OFF && ui_st == UI_NONE && world.banner_t() > 0 &&
            !world.label().empty() && !world.starter_open()) {
            int sw2, sh2;
            SDL_GetWindowSize(win, &sw2, &sh2);
            float fade = world.banner_t() < 1.f ? world.banner_t() : 1.f;
            ImVec2 sz = ImGui::CalcTextSize(world.label().c_str());
            ImGui::SetNextWindowPos(ImVec2((sw2 - sz.x - 48) / 2, 30));
            ImGui::SetNextWindowSize(ImVec2(sz.x + 48, sz.y + 22));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
            panel_push();
            ImGui::Begin("##zone", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoInputs);
            accent_line();
            ImGui::SetCursorPosX(24);
            ImGui::Text("%s", world.label().c_str());
            ImGui::End();
            panel_pop();
            ImGui::PopStyleVar();
        }
        // upload Image -> texture GL filtrée NEAREST (sûr en drawlist ImGui)
        auto& spr_upload = ui_upload;
        // ── boîte de dialogue (scripts) — masquée pendant un combat ──────
        // DS : la fenêtre n'existe que quand la messagerie l'a ouverte
        // (Message*/OpenMessage) jusqu'à CloseMessage — plus de texte
        // fantôme persistant quand le script se termine.
        if (ist == I_OFF && !world.battle().active &&
            ui_st == UI_NONE && (world.msgbox() || world.yesno())) {
            int sw2, sh2;
            SDL_GetWindowSize(win, &sw2, &sh2);
            // couche DS : tout dessiné en pixels 256x192 puis mis à l'échelle
            g_uds = sh2 / 192.f;
            g_udx = (sw2 - 256.f * g_uds) * 0.5f;
            const bool dsf = ds_font_ready(db);
            ImDrawList* dld = ImGui::GetForegroundDrawList();
            // fenêtre message : rect tuiles (2,19,27,4) + cadre pl_winframe
            dt_msgbox(dld, db, 2, 19, 27, 4);
            std::string dtx = world.dialog_shown();
            // palette police DS (pl_font.narc m7) : fg n*2+1, ombre n*2+2
            static const ImU32 FC[][2] = {
                {IM_COL32(80,80,88,255),   IM_COL32(160,160,168,255)}, // 0 normal
                {IM_COL32(232,32,16,255),  IM_COL32(248,168,184,255)}, // 1 rouge
                {IM_COL32(0,112,248,255),  IM_COL32(120,184,232,255)}, // 2 bleu
                {IM_COL32(0,0,0,255),      IM_COL32(120,120,120,255)}, // 3 noir
                {IM_COL32(40,168,32,255),  IM_COL32(160,230,160,255)}, // 4 vert
                {IM_COL32(248,248,248,255),IM_COL32(248,184,192,255)}, // 5 blanc
                {IM_COL32(216,216,216,255),IM_COL32(144,208,248,255)}, // 6 gris
                {IM_COL32(248,248,248,255),IM_COL32(0,0,0,255)},       // 7 blanc
            };
            auto& cols = world.dialog_cols();
            size_t base = world.dlg_page_start();
            auto colIdx = [&](size_t pos) {          // couleur active à l'octet pos
                int n = 0;
                for (auto& s : cols)
                    if (s.first <= (int)(base + pos)) n = s.second;
                    else break;
                return (n >= 0 && n < 8) ? n : 0;     // FF/autres -> normal
            };
            auto gf_glyph = [&](size_t& k, int& cp) -> int {  // utf8 -> glyphe (avance k)
                int c = msg_next_utf8(dtx, k); cp = c;
                if (c < 0) return -1;
                int gc = msg_char_code_of((char32_t)c);
                return gc > 0 ? gc - 1 : -1;
            };
            auto adv = [&](int g) -> float {                  // avance px DS
                if (!dsf || g < 0) return 8.f;
                int w = g_dfont.glyph_w(g);
                return w ? w : g_dfont.cellW;
            };
            auto linew = [&](size_t b, size_t e) {            // largeur ligne
                float w = 0;
                for (size_t k = b; k < e; ) { int cp; int g = gf_glyph(k, cp);
                    w += (cp == ' ') ? adv(0) : adv(g); }
                return w;
            };
            // wrap glouton par mots à la largeur de la fenêtre DS (27 tuiles)
            float wrapw = 27 * 8.f;
            float lh = dsf ? (float)g_dfont.cellH : 16.f;
            struct Ln { size_t b, e; };
            std::vector<Ln> lines;
            size_t lstart = 0, j = 0;
            while (j < dtx.size()) {
                if (dtx[j] == '\n') {
                    lines.push_back({lstart, j}); j++; lstart = j; continue;
                }
                if (dtx[j] == ' ') { j++; continue; }
                size_t we = j;
                while (we < dtx.size() && dtx[we] != ' ' && dtx[we] != '\n') we++;
                if (linew(lstart, we) > wrapw && j > lstart) {
                    lines.push_back({lstart, j}); lstart = j;
                }
                j = we;
            }
            lines.push_back({lstart, dtx.size()});
            // le texte démarre à l'origine de la fenêtre (tuiles 2,19)
            for (size_t li = 0; li < lines.size(); li++) {
                float x = 16.f, y = 152.f + li * lh;
                for (size_t k = lines[li].b; k < lines[li].e; ) {
                    size_t k0 = k;
                    int cp; int g = gf_glyph(k, cp);      // avance k
                    if (cp == ' ') { x += adv(0); continue; }
                    int ci = colIdx(k0);
                    GLuint at = dsf ? ds_font_tex(ci) : 0;
                    if (at && g >= 0) {
                        int awt = 16 * g_dfont.cellW,
                            aht = ((g_dfont.nglyphs + 15) / 16) * g_dfont.cellH;
                        float u0 = (float)((g % 16) * g_dfont.cellW) / awt,
                              v0 = (float)((g / 16) * g_dfont.cellH) / aht,
                              u1 = u0 + (float)g_dfont.cellW / awt,
                              v1 = v0 + (float)g_dfont.cellH / aht;
                        ImVec2 a = dp(x, y);
                        dld->AddImage((ImTextureID)(intptr_t)at,
                            a, ImVec2(a.x + g_dfont.cellW * g_uds,
                                      a.y + g_dfont.cellH * g_uds),
                            ImVec2(u0, v0), ImVec2(u1, v1));
                    } else if (!dsf) {                       // repli ImGui
                        ImVec2 a = dp(x, y);
                        dld->AddText(ImVec2(a.x + 1, a.y + 1), FC[ci][1],
                            dtx.c_str() + k0, dtx.c_str() + k);
                        dld->AddText(a, FC[ci][0],
                            dtx.c_str() + k0, dtx.c_str() + k);
                    }
                    x += adv(g);
                }
            }
            if (!world.yesno() && world.msgbox()) {
                // curseur scroll DS (scroll_cursor.NCGR animé, bas-droite)
                dt_scrollcur(dld, db, 232 - 16 - 4, 184 - 16);
            }
            // oui/non DS : fenêtre standard tuiles (25,13,6,4) ;
            // texte à xOffset=8, curseur 8x16 à x=0, lignes à pitch 16
            if (world.yesno()) {
                dt_stdframe(dld, db, 25, 13, 6, 4);
                const char* yn[2] = {tr("Oui","Yes"), tr("Non","No")};
                ImVec2 mp = ImGui::GetIO().MousePos;
                bool clk = ImGui::IsMouseClicked(0);
                for (int i = 0; i < 2; i++) {
                    float iy = 104 + i * 16.f;
                    bool hov = mp.x >= dp(200, iy).x && mp.x <= dp(248, iy).x &&
                               mp.y >= dp(200, iy).y &&
                               mp.y <= dp(200, iy + 16).y;
                    if (clk && hov) {
                        if (i == yn_sel) push_key(SDLK_E);
                        else yn_sel = i;
                    }
                    if (i == yn_sel) {                  // flèche 8x16
                        ImVec2 c0 = dp(200, iy + 2), c1 = dp(200, iy + 14),
                               c2 = dp(206, iy + 8);
                        dld->AddTriangleFilled(c0, c1, c2, IM_COL32(232,40,40,255));
                    }
                    dt_text(dld, yn[i], 208, iy, 0);
                }
            }
        }
        // ── combat : fond ciel/herbe + panneaux verre sombre (mockup) ────
        if (world.battle().active) {
            auto& bt = world.battle();
            ImVec2 io = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            if (bt.st == Battle::TRANS) {
                // transition de rencontre DS (36 ticks) : 2 flashs, puis
                // l'écran tranché en bandes qui partent, puis 1 tick noir.
                float el = (1.2f - bt.anim) * 30.f;         // ticks écoulés
                int foe_lv = bt.foe().level;
                int lead_lv = bt.you().level;
                bool higher = foe_lv > lead_lv;             // flash blanc
                if (el < 22.f) {
                    static const int PULSE[11] =
                        {0,0,0,5,10,16,16,16,11,6,0};
                    int v = PULSE[(int)el % 11];
                    dl->AddRectFilled({0, 0}, io, higher
                        ? IM_COL32(255, 255, 255, v * 16)
                        : IM_COL32(0, 0, 0, v * 16));
                } else if (el < 35.f) {
                    if (!bt.trans_tex) {                    // capture du champ
                        GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
                        Image img; img.create(vp[2], vp[3]);
                        glReadPixels(0, 0, vp[2], vp[3], GL_RGBA,
                            GL_UNSIGNED_BYTE, img.px.data());
                        for (int y = 0; y < img.h / 2; y++)   // bas -> haut
                            for (int x = 0; x < img.w * 4; x++)
                                std::swap(img.px[y * img.w * 4 + x],
                                    img.px[(img.h - 1 - y) * img.w * 4 + x]);
                        bt.trans_tex = rnd::upload_tex(img);
                        bt.trans_w = img.w; bt.trans_h = img.h;
                    }
                    // offset quadratique : 7f vers ~-2.5px, 6f vers 255
                    float f = el - 22.f, rest = higher ? -3.f : -2.f;
                    auto quad = [](float s, float from, float to,
                                   float v, int n) {
                        float a = 2.f * (to - from - v * n) / (n * n);
                        return from + v * s + a * s * s * 0.5f;
                    };
                    float dx = f <= 7.f ? quad(f, 0, rest, -12.f, 7)
                        : quad(std::min(f - 7.f, 6.f), rest, 255.f, 30.f, 6);
                    dx *= io.x / 256.f;
                    float band = (higher ? 2.f : 5.f) * io.y / 192.f;
                    dl->AddRectFilled({0, 0}, io, IM_COL32(0, 0, 0, 255));
                    for (int i = 0; i * band < io.y; i++) {
                        float y0 = i * band, y1 = std::min((i + 1) * band, io.y);
                        float off = (i & 1) ? dx : -dx;
                        dl->AddImage((ImTextureID)(intptr_t)bt.trans_tex,
                            {off, y0}, {io.x + off, y1},
                            {0, y0 / io.y}, {1, y1 / io.y});
                    }
                } else
                    dl->AddRectFilled({0, 0}, io, IM_COL32(0, 0, 0, 255));
            } else {
            if (bt.trans_tex) {
                GLuint tt = bt.trans_tex; glDeleteTextures(1, &tt);
                bt.trans_tex = 0;
            }
            // décor ROM : panorama 512x256 (pl_batt_bg), vue centrée 256x192
            float bgh = io.y * 0.72f;                 // hauteur de la scène
            float s2 = bgh / 192.f;                   // px DS -> px écran
            uint32_t bgt = world.battle_bg_tex(bt.bg, bt.tod);
            if (bgt) {
                dl->AddImage((ImTextureID)(intptr_t)bgt, {0, 0},
                    {io.x, bgh}, {0.25f, 0.f}, {0.75f, 0.75f});
            } else {  // repli : dégradé si la ROM n'a pas l'archive
                dl->AddRectFilledMultiColor({0, 0}, {io.x, bgh},
                    IM_COL32(185, 220, 242, 255), IM_COL32(185, 220, 242, 255),
                    IM_COL32(143, 188, 94, 255), IM_COL32(143, 188, 94, 255));
            }
            dl->AddRectFilled({0, bgh}, io, B_NAVY);
            // plateformes de terrain (pl_batt_obj) : ennemi haut-droite,
            // joueur bas-gauche — les sprites des Pokémon se posent dessus
            int pw = 0, ph = 0;
            world.battle_plat_size(bt.terrain, 1, &pw, &ph);
            uint32_t tpe = world.battle_plat_tex(bt.terrain, 1);
            uint32_t tpp = world.battle_plat_tex(bt.terrain, 0);
            float epw = pw ? pw * s2 : 129 * s2, eph = ph ? ph * s2 : 65 * s2;
            float ppw = 257 * s2, pph = 33 * s2;
            world.battle_plat_size(bt.terrain, 0, &pw, &ph);
            if (pw) { ppw = pw * s2; pph = ph * s2; }
            ImVec2 epc = {io.x * 0.66f, bgh * 0.44f};    // centre plat. ennemie
            ImVec2 ppc = {io.x * 0.30f, bgh * 0.93f};    // centre plat. joueur
            // intro DS : sprites+plateformes glissent depuis les bords (±272px
            // sur 0.8s), puis les boîtes PV glissent (160px à 48px/tick)
            float slide = bt.st == Battle::INTRO
                ? std::min(1.f, (1.2f - bt.anim) / 0.8f) : 1.f;
            float hbox_sl = bt.st == Battle::INTRO
                ? std::max(0.f, 1.f - (1.2f - bt.anim - 0.8f) / 0.15f) : 1.f;
            float esl = (1.f - slide) * 272.f * s2;    // ennemi depuis la droite
            float psl = (1.f - slide) * 272.f * s2;    // joueur depuis la gauche
            float roff = 0.f;
            if (tpp) dl->AddImage((ImTextureID)(intptr_t)tpp,
                {ppc.x - ppw * 0.5f - psl, ppc.y - pph * 0.5f},
                {ppc.x + ppw * 0.5f - psl, ppc.y + pph * 0.5f});
            if (tpe) dl->AddImage((ImTextureID)(intptr_t)tpe,
                {epc.x - epw * 0.5f + esl, epc.y - eph * 0.5f + roff},
                {epc.x + epw * 0.5f + esl, epc.y + eph * 0.5f + roff});
            // sprites ROM : ennemi face avant, joueur dos (anim 2 frames)
            int afr = (int)(ImGui::GetTime() * 2.5) & 1;
            uint32_t tf = world.poke_tex(bt.foe().species, 0, afr);
            uint32_t tb = world.poke_tex(bt.you().species, 1, afr);
            float fs = 82.f * s2, ps = 90.f * s2;
            // pieds = ligne opaque basse du sprite posée sur la plateforme
            float ffoot = epc.y - eph * 0.12f + roff;
            float pfoot = ppc.y - pph * 0.42f;
            int fb = world.poke_feet(bt.foe().species, 0, afr);
            int pb = world.poke_feet(bt.you().species, 1, afr);
            // anims combat : propulsion attaquant (va-et-vient vers la cible),
            // clignotement + secousse sur le touché, glissade au KO
            float lung = bt.atk_t < 1.f ? sinf(bt.atk_t * 3.1416f) : 0.f;
            ImVec2 foff = {esl, 0}, poff = {-psl, 0};
            if (bt.atk_side == 1) {                  // ennemi fond vers le bas-gauche
                foff.x = -lung * 46.f * s2; foff.y = lung * 30.f * s2;
            } else if (bt.atk_side == 0) {           // joueur fond vers haut-droit
                poff.x = lung * 46.f * s2; poff.y = -lung * 30.f * s2;
            }
            // hit DS (12 ticks @30Hz) : invisible t%4∈{2,3}, ±2px sinon
            auto hit_fx = [&](int side, bool& blink, float& dx) {
                blink = false;
                if (bt.hit_side != side || bt.hit_t <= 0.f) return;
                int t = (int)((0.45f - bt.hit_t) * 30.f) % 12;
                if (t % 4 >= 2) blink = true;
                else dx += (t % 2 ? 2.f : -2.f) * s2;
            };
            bool fblink, pblink;
            hit_fx(1, fblink, foff.x);
            hit_fx(0, pblink, poff.x);
            // lancer de Ball : l'ennemi disparaît dedans (secousses), la Ball
            // vole depuis le pad joueur puis se balance sur le pad ennemi
            bool foe_out = bt.caught ||
                (bt.st == Battle::THROW && bt.tphase == 1);
            // KO DS : le sprite s'enfonce dans le sol (crop bas) en 10 ticks
            float fdrop = bt.faint_t[1];
            if (tf && !foe_out && fdrop < 1.f && !fblink) {
                float sink = fdrop * 80.f * s2;
                float ybot = ffoot + (80 - fb) * fs / 80.f + foff.y;
                dl->PushClipRect({0, 0}, {io.x, ybot}, true);
                dl->AddImage((ImTextureID)(intptr_t)tf,
                    {epc.x - fs * 0.5f + foff.x, ffoot - fb * fs / 80.f + foff.y + sink},
                    {epc.x + fs * 0.5f + foff.x, ybot + sink});
                dl->PopClipRect();
            }
            if (bt.st == Battle::THROW || bt.caught) {
                GLuint btex = world.item_icon_tex(bt.ball, 0);
                float bs = 22.f * s2;
                if (bt.tphase == 0) {           // vol en arc joueur -> ennemi
                    float t = std::min(1.f, 1.f - bt.anim / 1.0f);
                    float bx = ppc.x + (epc.x - ppc.x) * t;
                    float by = pfoot - ps + (ffoot - pfoot + ps * 0.4f) * t
                        - sinf(t * 3.1416f) * 40.f * s2;
                    if (btex) dl->AddImage((ImTextureID)(intptr_t)btex,
                        {bx - bs / 2, by - bs / 2}, {bx + bs / 2, by + bs / 2});
                } else if (btex) {              // au sol : wobble en secousse
                    float wx = (bt.tphase == 1 && bt.anim > 0.25f)
                        ? sinf((float)ImGui::GetTime() * 28.f) * 5.f * s2 : 0.f;
                    dl->AddImage((ImTextureID)(intptr_t)btex,
                        {epc.x - bs / 2 + wx, ffoot - bs * 0.8f},
                        {epc.x + bs / 2 + wx, ffoot + bs * 0.2f});
                }
            }
            float pdrop = bt.faint_t[0];
            if (tb && pdrop < 1.f && !pblink) {
                float sink = pdrop * 80.f * s2;
                float ybot = pfoot + (80 - pb) * ps / 80.f + poff.y;
                dl->PushClipRect({0, 0}, {io.x, ybot}, true);
                dl->AddImage((ImTextureID)(intptr_t)tb,
                    {ppc.x - ps * 0.5f + poff.x, pfoot - pb * ps / 80.f + poff.y + sink},
                    {ppc.x + ps * 0.5f + poff.x, ybot + sink});
                dl->PopClipRect();
            }
            // ── boîtes PV : fenêtre DS pl_winframe + NFGR ──
            auto hpbox = [&](const BatMon& m, float x, float y, bool mine) {
                float uy2 = io.y / 720.f;
                ds_font_ready(db);
                float w = mine ? 300.f : 272.f, h = mine ? 104.f : 72.f;
                const MapHeader* MHP = db.header(world.header());
                ds_window(dl, db, MHP ? (int)MHP->win : 0, 8.f * uy2 * 1.4f,
                    ImVec2(x, y), ImVec2(x + w, y + h));
                float hsc = uy2 * 1.5f;
                std::string nm = battle_species_name(&db, m.species);
                ds_text(dl, nm, x + 16, y + 10, 0, hsc);
                std::string lv = tr("N. ", "Lv. ") + std::to_string(m.level);
                ds_text(dl, lv, x + w - 16 - ds_text_w(lv, hsc), y + 10,
                    6, hsc);
                float r = m.maxhp ? bt.disp_hp[mine ? 0 : 1] / m.maxhp : 0.f;
                if (r > 1.f) r = 1.f;
                float hby = y + h - (mine ? 52 : 30) - uy2 * 4.f;
                ds_text(dl, tr("PV", "HP"), x + 16, hby, 5, hsc);
                float bxx = x + 16 + ds_text_w("PV", hsc) + 14,
                      bw2 = w - (bxx - x) - 18;
                dl->AddRectFilled(ImVec2(bxx, hby + 2),
                    ImVec2(bxx + bw2, hby + 14 * uy2),
                    IM_COL32(60, 70, 90, 255), 5.f);
                if (r > 0) dl->AddRectFilled(ImVec2(bxx, hby + 2),
                    ImVec2(bxx + bw2 * r, hby + 14 * uy2), hp_col(r), 5.f);
                dl->AddRect(ImVec2(bxx, hby + 2),
                    ImVec2(bxx + bw2, hby + 14 * uy2), B_NAVY, 5.f, 0, 1.5f);
                if (mine) {
                    std::string hn = std::to_string(m.hp) + " / " +
                        std::to_string(m.maxhp);
                    ds_text(dl, hn, x + w - 16 - ds_text_w(hn, hsc),
                        hby + 16 * uy2 + 4, 0, hsc);
                }
            };
            float hb = (1.f - hbox_sl) * 160.f * s2;  // boîtes PV glissent
            hpbox(bt.foe(), 24 - hb, io.y * 0.06f, false);
            hpbox(bt.you(), io.x - 324 + hb, io.y * 0.40f, true);

            // ── zone tactile DS : texte à gauche + 4 gros boutons à droite ──
            float mx = 18.f;
            float uyb = io.y / 720.f;
            float strip = 10.f;                    // liseré violet bas
            float btnw = 226.f * (io.x / 1280.f), btnh = 58.f * uyb, bgap = 10.f;
            bool menu_st = bt.st == Battle::CHOICE ||
                           bt.st == Battle::PICKMOVE;
            // menus tactiles : zone élargie (le sous-écran DS = 192px)
            float cmdh = menu_st ? io.y * 0.40f
                                 : 2 * btnh + bgap + 16.f;
            float my2 = io.y - strip - cmdh - 10.f;
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool clk = ImGui::IsMouseClicked(0);
            bool choice = bt.st == Battle::CHOICE ||
                          bt.st == Battle::PICKMOVE;
            // sous-écran DS : pl_batt_bg — coquille (m49) + tuiles boutons
            // (m42 action / m47 attaques), palette m242, tuiles m28.
            static GLuint bt_shell = 0, bt_act = 0, bt_mov = 0;
            static int btsh_h = 192;
            static bool bts_try = false;
            if (!bts_try) {
                bts_try = true;
                auto bn = db.narc("/battle/graphic/pl_batt_bg.narc");
                if (bn && bn->count() > 242) {
                    auto bp = spr::parse_nclr(bn->member(242));
                    auto btt = spr::parse_ncgr(bn->member(28));
                    if (auto im = spr::render_screen(bn->member(49), btt, bp)) {
                        bt_shell = ui_upload(*im); btsh_h = im->h;
                    }
                    if (auto im = spr::render_screen(bn->member(42), btt, bp, true))
                        bt_act = ui_upload(*im);
                    if (auto im = spr::render_screen(bn->member(47), btt, bp, true))
                        bt_mov = ui_upload(*im);
                }
            }
            float uvmax = btsh_h > 0 ? 192.f / btsh_h : 1.f;
            // zone menu DS : bande du bas pleine largeur (carte 256x192 ->
            // rect fenêtre). scx/scy mappent les coordonnées DS -> pixels.
            auto scx = [&](float sx) {
                return mx + sx * (io.x - 2 * mx) / 256.f; };
            auto scy = [&](float sy) {
                return my2 + sy * cmdh / 192.f; };
            if (choice && bt_shell) {
                dl->AddImage((ImTextureID)(intptr_t)bt_shell,
                    {mx, my2}, {io.x - mx, my2 + cmdh},
                    {0, 0}, {1, uvmax});
                dl->AddImage((ImTextureID)(intptr_t)
                        (bt.st == Battle::CHOICE ? bt_act : bt_mov),
                    {mx, my2}, {io.x - mx, my2 + cmdh},
                    {0, 0}, {1, uvmax});
            }
            // boîte message DS (pleine largeur hors menus ; le sous-écran
            // affiche l'invite en haut de la coquille)
            std::string qt = bt.msg;
            if (qt.empty() && bt.st == Battle::CHOICE) {
                qt = tr("Que doit faire ", "What will ") +
                     battle_species_name(&db, bt.you().species) +
                     tr(" ?", " do?");
            }
            ds_font_ready(db);
            float tw = io.x - 2 * mx;
            if (!choice || !bt_shell) {
                const MapHeader* MHB = db.header(world.header());
                int bf = MHB ? (int)MHB->win : 0;
                ds_window(dl, db, bf, 8.f * uyb * 2.6f, ImVec2(mx, my2),
                    ImVec2(mx + tw, my2 + cmdh));
            }
            if (bt.st == Battle::CHOICE) {
                float bfsc = uyb * 2.6f;
                ds_text(dl, qt, scx(10), scy(4), bt_shell ? 3 : 0, bfsc);
            } else if (!choice) {
                float bfsc = uyb * 2.6f;
                ds_text(dl, qt, mx + 30, my2 + cmdh * 0.5f - 14.f, 0, bfsc);
            }
            // ▼ centré en bas si message seul
            if (!choice && bt.st != Battle::PICKMOVE) {
                float bob = sinf((float)ImGui::GetTime() * 6.f) * 2.5f;
                ImVec2 asz = ImGui::CalcTextSize("▼");
                dl->AddText({mx + (tw - asz.x) / 2,
                    my2 + cmdh - asz.y - 8 + bob},
                    B_ORANGE, "▼");
            }
            if (bt.st == Battle::CHOICE) {
                // boutons du sous-écran DS (membre 42) : ATTAQUE large en
                // haut, SAC / POKéMON / FUITE en ligne du bas.
                static const float BR[4][4] = {          // src 256x192
                    {16, 34, 240, 128},                  // ATTAQUE (rouge)
                    {0, 144, 84, 192},                   // SAC     (orange)
                    {84, 144, 176, 192},                 // POKéMON (bleu)
                    {172, 144, 256, 192}                 // FUITE   (vert)
                };
                const char* q[4] = {tr("ATTAQUE", "FIGHT"), tr("SAC", "BAG"),
                                    tr("POKéMON", "POKéMON"),
                                    tr("FUITE", "RUN")};
                if (bt_act) {
                    for (int i = 0; i < 4; i++) {
                        float x0 = scx(BR[i][0]), y0 = scy(BR[i][1]);
                        float x1 = scx(BR[i][2]), y1 = scy(BR[i][3]);
                        bool sel = i == bt.sel;
                        bool hov = mp.x >= x0 && mp.x <= x1 &&
                                   mp.y >= y0 && mp.y <= y1;
                        if (hov) bt.sel = i;
                        if (hov && clk) push_key(SDLK_E);
                        if (sel)
                            dl->AddRect({x0, y0}, {x1, y1}, B_WHITE,
                                10.f, 0, 3.f);
                        float bfsc2 = uyb * 1.7f;
                        float qw = ds_text_w(q[i], bfsc2);
                        ds_text(dl, q[i], x0 + (x1 - x0 - qw) / 2,
                            y0 + (y1 - y0) / 2 - g_dfont.cellH * bfsc2 / 2,
                            5, bfsc2);
                    }
                } else {
                    float bx0 = io.x - mx - (2 * btnw + bgap);
                    for (int i = 0; i < 4; i++) {
                        float bx = bx0 + (i % 2) * (btnw + bgap);
                        float by = my2 + 8 + (i / 2) * (btnh + bgap);
                        bool sel = i == bt.sel;
                        bool hov = mp.x >= bx && mp.x <= bx + btnw &&
                                   mp.y >= by && mp.y <= by + btnh;
                        if (hov) bt.sel = i;
                        if (hov && clk) push_key(SDLK_E);
                        dl->AddRectFilled({bx, by}, {bx + btnw, by + btnh},
                            IM_COL32(200, 80, 80, 255), 12.f);
                        dl->AddRect({bx, by}, {bx + btnw, by + btnh},
                            B_WHITE, 12.f, 0, sel ? 3.5f : 2.f);
                        float bfsc2 = uyb * 1.9f;
                        float qw = ds_text_w(q[i], bfsc2);
                        ds_text(dl, q[i], bx + (btnw - qw) / 2,
                            by + btnh / 2 - g_dfont.cellH * bfsc2 / 2,
                            5, bfsc2);
                    }
                }
            } else if (bt.st == Battle::PICKMOVE) {
                // attaques : boutons acier du sous-écran DS (nom + PP)
                // boutons acier du sous-écran DS (membre 47) : 2x2
                static const float MB[4][4] = {        // src 256x192
                    {5, 27, 121, 80}, {134, 27, 250, 80},
                    {5, 83, 121, 136}, {134, 83, 250, 136}
                };
                for (int i = 0; i < 4; i++) {
                    float bx = scx(MB[i][0]), by = scy(MB[i][1]);
                    float gx = scx(MB[i][2]) - bx, gh = scy(MB[i][3]) - by;
                    bool have = i < bt.you().nmoves;
                    bool sel = i == bt.sel;
                    bool hov = have && mp.x >= bx && mp.x <= bx + gx &&
                               mp.y >= by && mp.y <= by + gh;
                    if (hov) bt.sel = i;
                    if (hov && clk) push_key(SDLK_E);
                    if (sel)
                        dl->AddRect({bx, by}, {bx + gx, by + gh},
                            B_ORANGE, 8.f, 0, 2.5f);
                    if (!have) {
                        ds_text(dl, "-", bx + 14, by + 6, 6, uyb * 1.7f);
                        continue;
                    }
                    MoveInfo mi = battle_move(&db, bt.you().moves[i]);
                    std::string s = battle_move_name(&db, bt.you().moves[i]);
                    float mfsc = uyb * 1.6f;
                    float mw = ds_text_w(s, mfsc);
                    ds_text(dl, s, bx + (gx - mw) / 2,
                        by + gh * 0.28f, 3, mfsc);
                    // PP courant/max centré en bas du bouton
                    float psc = uyb * 1.25f;
                    char pp[32]; snprintf(pp, sizeof pp,
                        tr("PP %d/%d", "PP %d/%d"), bt.you().pp[i], mi.pp);
                    float pw = ds_text_w(pp, psc);
                    ds_text(dl, pp, bx + (gx - pw) / 2,
                        by + gh * 0.62f, 3, psc);
                }
            } else if (bt.st == Battle::BAG) {
                // picker Balls DS : liste compacte à droite (icône+nom+qté)
                auto balls = world.bag_items(2);        // poche Balls DS
                int nb = (int)balls.size();
                float pw2 = 2 * btnw + bgap, ph2 = cmdh;
                float bx0 = io.x - mx - pw2;
                {
                    const MapHeader* MH6 = db.header(world.header());
                    int cf7 = MH6 ? (int)MH6->win : 0;
                    ds_window(dl, db, cf7, 8.f * uyb * 2.6f,
                        ImVec2(bx0, my2), ImVec2(bx0 + pw2, my2 + ph2));
                }
                float bfsc3 = uyb * 2.2f;
                if (!nb) {
                    ds_text(dl, tr("Aucune Ball dans le sac !",
                        "No Balls in the Bag!"), bx0 + 22, my2 + 40, 0, bfsc3);
                }
                float rh = std::min(46.f, (ph2 - 34.f) / std::max(1, nb));
                for (int i = 0; i < nb; i++) {
                    float ry = my2 + 30 + i * rh;
                    bool sel = i == bt.sel;
                    bool hov = mp.x >= bx0 + 8 && mp.x <= bx0 + pw2 - 8 &&
                               mp.y >= ry && mp.y <= ry + rh - 4;
                    if (hov) bt.sel = i;
                    if (hov && clk) world.battle_throw(balls[i].first);
                    if (sel || hov)
                        dl->AddRectFilled({bx0 + 8, ry}, {bx0 + pw2 - 8, ry + rh - 4},
                            IM_COL32(200, 216, 240, 255), 4.f);
                    GLuint ic = world.item_icon_tex(balls[i].first, sel ? 1 : 0);
                    float is = rh - 12;
                    if (ic) dl->AddImage((ImTextureID)(intptr_t)ic,
                        {bx0 + 16, ry + 3}, {bx0 + 16 + is, ry + 3 + is});
                    std::string nm = world.item_name(balls[i].first);
                    ds_text(dl, nm, bx0 + 20 + is, ry + (rh - 4 - 16) / 2,
                        0, bfsc3);
                    char qt2[16]; snprintf(qt2, sizeof qt2, "x%d",
                        balls[i].second);
                    ds_text(dl, qt2, bx0 + pw2 - 26 - ds_text_w(qt2, bfsc3),
                        ry + (rh - 4 - 16) / 2, 6, bfsc3);
                }
            } else if (clk && mp.y >= my2 && mp.y <= my2 + cmdh) {
                push_key(SDLK_E);      // clic sur le message = avancer
            }
            // fade-in DS : 8 ticks depuis le noir pendant le slide-in
            if (bt.st == Battle::INTRO && bt.anim > 0.933f) {
                int fa = (int)((bt.anim - 0.933f) / 0.267f * 255.f);
                dl->AddRectFilled({0, 0}, io,
                    IM_COL32(0, 0, 0, std::min(255, fa)));
            }
            // liseré violet DS en bas
            dl->AddRectFilled({0, io.y - strip}, {io.x, io.y}, B_PURP);
            dl->AddRectFilled({0, io.y - strip}, {io.x, io.y - strip + 3},
                B_PURP2);
            }
        }
        // ── menu multi (Multi/ShowMenu) : liste DS + curseur triangle ─────
        static int msel = 0; static bool mwas = false;
        if (!world.menu_open()) { mwas = false; msel = 0; }
        if (world.menu_open()) {
            if (!mwas) { msel = 0; mwas = true; }
            auto& items = world.menu_items();
            int nm = (int)items.size();
            if (msel >= nm) msel = 0;
            int sw2, sh2;
            SDL_GetWindowSize(win, &sw2, &sh2);
            float uy3 = sh2 / 720.f;
            ds_font_ready(db);
            float msc = uy3 * 2.6f, mlh = g_dfont.cellH * msc;
            // largeur = plus long libellé + curseur
            float mw2 = 0;
            for (auto& it : items)
                mw2 = std::max(mw2, ds_text_w(world.menu_text(it.second), msc));
            mw2 += 64 * uy3;
            float rowh = mlh + 14 * uy3, mh2 = nm * rowh + 20 * uy3;
            float mxx = sw2 - mw2 - 26 * uy3, myy = 30 * uy3;
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(sw2, sh2));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##menu", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            {
                const MapHeader* MHM = db.header(world.header());
                ds_window(dl, db, MHM ? (int)MHM->win : 0, 8.f * uy3 * 1.6f,
                    ImVec2(mxx, myy), ImVec2(mxx + mw2, myy + mh2));
            }
            ImVec2 mp2 = ImGui::GetIO().MousePos;
            for (int i = 0; i < nm; i++) {
                float ry = myy + 10 * uy3 + i * rowh;
                if (mp2.x >= mxx && mp2.x <= mxx + mw2 &&
                    mp2.y >= ry && mp2.y <= ry + rowh) {
                    msel = i;
                    if (ImGui::IsMouseClicked(0))
                        world.menu_pick(items[i].first);
                }
                if (i == msel)                      // triangle curseur DS
                    dl->AddTriangleFilled(
                        ImVec2(mxx + 18 * uy3, ry + rowh / 2 - 7 * uy3),
                        ImVec2(mxx + 18 * uy3, ry + rowh / 2 + 7 * uy3),
                        ImVec2(mxx + 30 * uy3, ry + rowh / 2),
                        B_ORANGE);
                ds_text(dl, world.menu_text(items[i].second),
                    mxx + 40 * uy3, ry + (rowh - mlh) / 2, 0, msc);
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            if (ImGui::IsKeyPressed(ImGuiKey_S) ||
                ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                msel = (msel + 1) % std::max(1, nm);
            if (ImGui::IsKeyPressed(ImGuiKey_W) ||
                ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                msel = (msel + nm - 1) % std::max(1, nm);
            if (ImGui::IsKeyPressed(ImGuiKey_E) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter))
                world.menu_pick(items[msel].first);
        }
        // ── boutique DS : fenêtre + liste NFGR + prix + argent ────────────
        if (world.shop_open()) {
            // ── boutique DS : fond tilemap.NSCR de shop_gra + fenêtres ──
            // liste (12,2,19,14) ; argent (1,1,9,4) ; description (5,18,27,6)
            static int shop_sel = 0;
            int sw3, sh3; SDL_GetWindowSize(win, &sw3, &sh3);
            g_uds = sh3 / 192.f;
            g_udx = (sw3 - 256.f * g_uds) * 0.5f;
            static GLuint shopbg = 0, shopcur = 0;
            static bool shop_try = false;
            if (!shop_try) {
                shop_try = true;
                auto sn = db.narc("/graphic/shop_gra.narc");
                if (sn && sn->count() >= 10) {
                    auto st = spr::parse_ncgr(sn->member(0));
                    auto sp = spr::parse_nclr(sn->member(1));
                    if (auto im = spr::render_screen(sn->member(2), st, sp))
                        shopbg = ui_upload(*im);
                    auto cu = spr::parse_ncgr(sn->member(7));
                    auto cuc = spr::parse_ncer(sn->member(8));
                    auto cup = spr::parse_nclr(sn->member(10));
                    if (cu.ntiles && !cuc.empty() && !cup.empty())
                        if (auto im = spr::render_cell(cu, cuc[0], cup))
                            shopcur = ui_upload(*im);
                }
                ds_font_ready(db);
            }
            auto& items = world.shop_items();
            int nsh = (int)items.size();
            if (shop_sel >= nsh) shop_sel = 0;
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            if (shopbg)
                bg->AddImage((ImTextureID)(intptr_t)shopbg,
                    dp(0, 0), dp(256, 192));
            else sb_bg(bg, (float)sw3, (float)sh3);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            // argent : fenêtre (1,1,9,4) "ARGENT" + somme
            dt_stdframe(dl, db, 1, 1, 9, 4);
            char mon[32]; snprintf(mon, sizeof mon, "%u", world.money());
            dt_text(dl, tr("Argent", "Money"), 12, 10, 0);
            dt_text(dl, mon, 76 - dt_text_w(mon), 24, 0);
            // liste : 7 lignes pitch 16 dans (12,2,19,14)
            int vmax = std::min(nsh, 7);
            int sc0 = std::max(0, std::min(shop_sel - vmax / 2,
                std::max(0, nsh - vmax)));
            for (int vi = 0; vi < vmax; vi++) {
                int i = sc0 + vi;
                float ry = 18 + vi * 16;
                if (i == shop_sel) {
                    if (shopcur)
                        dl->AddImage((ImTextureID)(intptr_t)shopcur,
                            dp(96, ry), dp(112, ry + 16));
                    else {
                        ImVec2 c0 = dp(97, ry + 3), c1 = dp(97, ry + 13),
                               c2 = dp(103, ry + 8);
                        dl->AddTriangleFilled(c0, c1, c2, B_ORANGE);
                    }
                }
                dt_text(dl, world.item_name(items[i]), 112, ry, 0);
                char pr[24]; snprintf(pr, sizeof pr, "%d",
                    world.item_price(items[i]));
                dt_text(dl, pr, 244 - dt_text_w(pr), ry, 0);
            }
            // ascenseur
            if (nsh > 7) {
                float th = std::max(10.f, 104.f * 7 / nsh);
                float tp = 104.f * sc0 / nsh;
                dl->AddRectFilled(dp(246, 20 + tp), dp(249, 20 + tp + th),
                    IM_COL32(90, 110, 140, 255), 2.f);
            }
            // description de l'objet sous le curseur (5,18,27,6), texte blanc
            if (nsh) {
                std::string dsc = world.msg_fmt(391, items[shop_sel]);
                std::string ln; size_t k2 = 0; float dly = 148;
                while (k2 < dsc.size()) {
                    size_t sp = dsc.find(' ', k2);
                    std::string w = dsc.substr(k2,
                        sp == std::string::npos ? dsc.size() - k2 : sp - k2);
                    if (!ln.empty() && dt_text_w(ln + " " + w) > 200) {
                        dt_text(dl, ln, 44, dly, 5);
                        dly += 14;
                        if (dly > 176) break;
                        ln = w;
                    } else ln = ln.empty() ? w : ln + " " + w;
                    if (sp == std::string::npos) break;
                    k2 = sp + 1;
                }
                if (!ln.empty() && dly <= 176)
                    dt_text(dl, ln, 44, dly, 5);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_S) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                shop_sel = (shop_sel + 1) % std::max(1, nsh);
            if (ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                shop_sel = (shop_sel + nsh - 1) % std::max(1, nsh);
            if (ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_Enter))
                if (!items.empty()) world.shop_buy(shop_sel);
            if (ImGui::IsKeyPressed(ImGuiKey_X) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                world.shop_close();
            ImVec2 mp2 = ImGui::GetIO().MousePos;
            if (ImGui::IsMouseClicked(0) && mp2.x >= dp(96, 16).x &&
                mp2.x <= dp(248, 128).x && mp2.y >= dp(96, 16).y &&
                mp2.y <= dp(96 + 152, 16 + 112).y) {
                int r = (int)((mp2.y - dp(0, 18).y) / (16.f * g_uds));
                int i = sc0 + r;
                if (i >= 0 && i < nsh) {
                    if (i == shop_sel) world.shop_buy(shop_sel);
                    else shop_sel = i;
                }
            }
        }
        // ── menu X DS authentique : boîte à droite, icônes + texte FR ─────
        static bool spr_init = false;
        static GLuint icon_col[10] = {}, icon_gry[10] = {};
        if (!spr_init) {
            spr_init = true;
            auto n = db.narc("/graphic/menu_gra.narc");
            if (n) {
                auto pals = spr::parse_nclr(n->member(5));
                auto ic = spr::parse_ncgr(n->member(6));
                auto cells = spr::parse_ncer(n->member(4));
                // NCLR menu : pal0 = gris (non-sél.), pal1 = couleurs (sél.)
                int gpal = pals.size() > 0 ? 0 : -1;
                int cpal = pals.size() > 1 ? 1 : gpal;
                for (size_t i = 0; i < cells.size() && i < 10; i++) {
                    if (cells[i].oams.empty()) continue;
                    auto im = spr::render_cell(ic, cells[i], pals, cpal);
                    if (im) icon_col[i] = spr_upload(*im, false);
                    auto ig = spr::render_cell(ic, cells[i], pals, gpal);
                    if (ig) icon_gry[i] = spr_upload(*ig, false);
                }
            }
        }
        if (ui_st != UI_NONE) {
            int sw2, sh2;
            SDL_GetWindowSize(win, &sw2, &sh2);
            if (ui_st == UI_XMENU) {
                // menu pause Platine : panneau marine arrondi à droite,
                // icônes grandes, sélection = contour orange arrondi.
                float sw = (float)sw2, sh = (float)sh2;
                float uy = sh / 720.f;
                float mw = 330 * uy, rh = 72 * uy;
                float mx = sw - mw - 26 * uy, my = 26 * uy;
                float mh = rh * menu_opts.size() + 20 * uy;
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                ds_font_ready(db);
                float fsc = uy * 2.5f;
                ImVec2 mp = ImGui::GetIO().MousePos;
                bool clk = ImGui::IsMouseClicked(0);
                dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                    IM_COL32(13, 20, 34, 244), 14 * uy);
                dl->AddRect(ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                    IM_COL32(58, 74, 102, 255), 14 * uy, 0, 2.f);
                for (size_t i = 0; i < menu_opts.size(); i++) {
                    int opt = menu_opts[i];
                    bool sel = (int)i == menu_sel;
                    float ry = my + 10 * uy + i * rh;
                    bool hov = mp.x >= mx + 10 && mp.x <= mx + mw - 10 &&
                               mp.y >= ry && mp.y <= ry + rh - 8;
                    if (clk && hov) {
                        if (sel) push_key(SDLK_E);
                        else menu_sel = (int)i;
                    }
                    float iw = 46 * uy;
                    float icy = ry + (rh - iw) / 2;
                    if (sel) {
                        dl->AddRect(ImVec2(mx + 10 * uy, ry + 4 * uy),
                            ImVec2(mx + mw - 10 * uy, ry + rh - 8 * uy),
                            B_ORANGE, 10 * uy, 0, 2.5f);
                    }
                    GLuint t = sel ? icon_col[opt]
                        : (icon_gry[opt] ? icon_gry[opt] : icon_col[opt]);
                    if (t) {
                        dl->AddImage((ImTextureID)(intptr_t)t,
                            ImVec2(mx + 24 * uy, icy),
                            ImVec2(mx + 24 * uy + iw, icy + iw));
                    }
                    const char* txt = (opt == 3) ? world.player_name().c_str()
                        : world.msg(367, opt).c_str();
                    float tx = mx + 24 * uy + iw + 18 * uy,
                          ty = ry + (rh - g_dfont.cellH * fsc) / 2;
                    ds_text(dl, txt, tx, ty, 7, fsc);
                }
            } else if (ui_st == UI_BAG) {
                // ── sac DS : écran réel pl_bag_gra en coords 256x192 ──
                // fenêtres décomp : liste objets (14,0,17,18) ; poches (0,11,12,2) ;
                // nom poche (0,13) ; description (0,18,32,6) ; actions (23,*,8,*)
                auto items = world.bag_items(bag_pocket);
                int nb = (int)items.size();
                if (bag_sel >= nb) bag_sel = std::max(0, nb - 1);
                float sw = (float)sw2, sh = (float)sh2;
                int iconf = (int)(SDL_GetTicks() / 250) & 1;
                g_uds = sh / 192.f;
                g_udx = (sw - 256.f * g_uds) * 0.5f;
                static GLuint bagbg = 0, bagbrd = 0, baghl = 0, bagpic = 0,
                              bagspr = 0;
                static spr::Ncgr bag_tiles;
                static std::vector<std::vector<RGBAf>> bag_pals;
                static bool bag_try = false;
                if (!bag_try) {
                    bag_try = true;
                    auto bn = db.narc("/resource/fra/bag/pl_bag_gra.narc");
                    if (bn && bn->count() >= 39) {
                        bag_tiles = spr::parse_ncgr(bn->member(11));
                        bag_pals = spr::parse_nclr(bn->member(12));
                        if (auto im = spr::render_screen(bn->member(14),
                                bag_tiles, bag_pals, true)) {
                            bagbg = ui_upload(*im);
                            if (getenv("DUMPBAG")) {
                                FILE* f=fopen("../_shots/bagbg.ppm","wb");
                                fprintf(f,"P6\n%d %d\n255\n",im->w,im->h);
                                for(size_t i=0;i<im->px.size();i+=4)
                                    fwrite(&im->px[i],1,3,f);
                                fclose(f);
                            }
                        }
                        if (auto im2 = spr::render_screen(bn->member(13),
                                bag_tiles, bag_pals, true))
                            bagbrd = ui_upload(*im2);
                        // bande poches : pocket_selector_icons + sa palette
                        auto pic = spr::parse_ncgr(bn->member(21));
                        auto pip = spr::parse_nclr(bn->member(22));
                        if (pic.ntiles && !pip.empty())
                            if (auto im = spr::compose(pic, 0,
                                    pic.ntiles > 32 ? 32 : pic.ntiles, 2,
                                    pip[0]))
                                bagpic = ui_upload(*im);
                        // surlignage ligne : item_highlight (cell + tiles)
                        auto hl = spr::parse_ncgr(bn->member(28));
                        auto hlc = spr::parse_ncer(bn->member(27));
                        if (hl.ntiles && !hlc.empty())
                            if (auto im = spr::render_cell(hl, hlc[0],
                                    bag_pals))
                                baghl = ui_upload(*im);
                        // sprite poche (sac qui rebondit à gauche)
                        auto sp = spr::parse_ncgr(bn->member(2));
                        auto spc = spr::parse_ncer(bn->member(1));
                        auto spp = spr::parse_nclr(bn->member(3));
                        if (sp.ntiles && !spc.empty() && !spp.empty())
                            if (auto im = spr::render_cell(sp, spc[0], spp))
                                bagspr = ui_upload(*im);
                        (void)bagspr;   // affiché plus tard (anim rebond)
                    }
                    ds_font_ready(db);
                }
                auto bg = ImGui::GetBackgroundDrawList();
                // ordre DS : l'écran de bordure (13) est le calque du dessous,
                // l'écran principal (14) par-dessus avec index 0 transparent.
                // les écrans font 256x256 : seuls les 192 px du haut sont visibles.
                if (bagbrd)
                    bg->AddImage((ImTextureID)(intptr_t)bagbrd,
                        dp(0, 0), dp(256, 192),
                        ImVec2(0, 0), ImVec2(1, 192 / 256.f));
                else sb_bg(bg, sw, sh);
                if (bagbg)
                    bg->AddImage((ImTextureID)(intptr_t)bagbg,
                        dp(0, 0), dp(256, 192),
                        ImVec2(0, 0), ImVec2(1, 192 / 256.f));
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                ImVec2 mp = ImGui::GetIO().MousePos;
                bool clk = ImGui::IsMouseClicked(0);
                bool rclk = ImGui::IsMouseClicked(1);
                int mwh = (int)ImGui::GetIO().MouseWheel;
                // ── colonne gauche : icône de l'objet sélectionné (rebond
                // au-dessus de l'ombre à ~52,80) + rangée des 8 poches ──
                if (nb && bag_sel < nb) {
                    GLuint ic = world.item_icon_tex(items[bag_sel].first,
                        iconf);
                    float bounce = fabsf(sinf(
                        (float)ImGui::GetTime() * 4.f)) * 4.f;
                    if (ic)
                        dl->AddImage((ImTextureID)(intptr_t)ic,
                            dp(36, 46 - bounce), dp(68, 78 - bounce));
                }
                // poches : 8 icônes 10x10 à y=91, x = 7 + i*11 (décomp)
                for (int p = 0; p < 8; p++) {
                    float ix = 7 + p * 11;
                    if (bagpic) {
                        float u0 = p * 32 / 256.f, u1 = u0 + 10 / 256.f;
                        dl->AddImage((ImTextureID)(intptr_t)bagpic,
                            dp(ix, 91), dp(ix + 10, 101),
                            ImVec2(u0, 0), ImVec2(u1, 0.625f));
                    }
                    ImVec2 a = dp(ix - 1, 88), b = dp(ix + 11, 103);
                    if (clk && mp.x >= a.x && mp.x <= b.x &&
                        mp.y >= a.y && mp.y <= b.y) {
                        bag_pocket = p; bag_sel = 0; bag_scroll = 0;
                    }
                }
                // nom de la poche (fenêtre 0,13 : centré sous les icônes)
                {
                    std::string pn = world.msg(395, bag_pocket);
                    float pw = dt_text_w(pn);
                    dt_text(dl, pn, 48 - pw / 2, 106, 5);
                }
                // ── liste objets : fenêtre (14,0,17,18), 9 lignes pitch 16 ──
                const float LX = 112, LY = 0, LW = 136;
                int vis = 9;
                if (bag_sel < bag_scroll) bag_scroll = bag_sel;
                if (bag_sel >= bag_scroll + vis)
                    bag_scroll = bag_sel - vis + 1;
                bag_scroll = std::max(0, std::min(bag_scroll, nb - vis));
                if (!nb) dt_text(dl, tr("(vide)", "(empty)"), 116, 2, 0);
                ImVec2 la = dp(LX, LY), lb = dp(LX + LW, 140);
                if (mwh && mp.x >= la.x && mp.x <= lb.x &&
                    mp.y >= la.y && mp.y <= lb.y)
                    bag_sel = std::max(0, std::min(nb - 1, bag_sel - mwh));
                for (int vi = 0; vi < vis && bag_scroll + vi < nb; vi++) {
                    int i = bag_scroll + vi, id = items[i].first;
                    float ry = LY + 1 + vi * 16;
                    bool sel = i == bag_sel;
                    ImVec2 ra = dp(LX + 2, ry - 1), rb = dp(LX + LW - 4,
                        ry + 15);
                    bool hov = mp.x >= ra.x && mp.x <= rb.x &&
                               mp.y >= ra.y && mp.y <= rb.y;
                    if ((clk || rclk) && hov) {
                        if (sel || rclk) push_key(SDLK_E);
                        else bag_sel = i;
                    }
                    if (sel) {
                        if (baghl)
                            dl->AddImage((ImTextureID)(intptr_t)baghl,
                                dp(LX - 1, ry - 1), dp(LX + LW - 2, ry + 15));
                        else
                            dl->AddRect(ra, rb, B_ORANGE, 3.f, 0, 2.f);
                    }
                    dt_text(dl, world.item_name(id), LX + 4, ry + 1,
                        sel ? 1 : 0);
                    if (bag_pocket != 7) {
                        char q[16]; snprintf(q, sizeof(q), "x%d",
                            items[i].second);
                        dt_text(dl, q, LX + LW - 8 - dt_text_w(q), ry + 1, 0);
                    }
                    if (world.registered_item() == id)
                        dt_text(dl, "Y", LX + LW - 10 - dt_text_w("x0") -
                            10, ry + 1, 4);
                }
                // ascenseur dans le bord droit de la liste
                if (nb > vis) {
                    float th = std::max(12.f, 136.f * vis / nb);
                    float tp = 136.f * bag_scroll / nb;
                    dl->AddRectFilled(dp(LX + LW - 2, LY + 4 + tp),
                        dp(LX + LW + 1, LY + 4 + tp + th),
                        IM_COL32(60, 90, 120, 255), 2.f);
                }
                // ── description : fenêtre (0,18,32,6) — icône + texte ──
                if (nb && bag_sel < nb) {
                    int id = items[bag_sel].first;
                    GLuint ic = world.item_icon_tex(id, iconf);
                    if (ic)
                        dl->AddImage((ImTextureID)(intptr_t)ic,
                            dp(6, 149), dp(36, 179));
                    std::string dsc = world.msg_fmt(391, id);
                    float dwmax = 204.f;
                    float dly = 147;
                    std::string ln; size_t k2 = 0;
                    while (k2 < dsc.size()) {
                        size_t sp = dsc.find(' ', k2);
                        std::string w = dsc.substr(k2,
                            sp == std::string::npos ? dsc.size() - k2
                                                    : sp - k2);
                        if (!ln.empty() &&
                            dt_text_w(ln + " " + w) > dwmax) {
                            dt_text(dl, ln, 40, dly, 5);
                            dly += 14;
                            if (dly > 178) break;
                            ln = w;
                        } else ln = ln.empty() ? w : ln + " " + w;
                        if (sp == std::string::npos) break;
                        k2 = sp + 1;
                    }
                    if (!ln.empty() && dly <= 178)
                        dt_text(dl, ln, 40, dly, 5);
                }
                // ── actions : fenêtre DS (23, 23-2n, 8, 2n) ──
                if (bag_act >= 0 && bag_party < 0 && bag_toss < 0
                    && bag_yn < 0) {
                    static const char* FR[] = {"UTILISER", "ENREGISTRER",
                                               "JETER", "ANNULER"};
                    static const char* EN[] = {"USE", "REGISTER",
                                               "TOSS", "CANCEL"};
                    const char** LBL = g_opts.lang ? EN : FR;
                    int nact = (int)bag_acts.size();
                    int ay0 = 23 - 2 * nact;
                    dt_stdframe(dl, db, 23, ay0, 8, 2 * nact);
                    for (int i = 0; i < nact; i++) {
                        float ey = ay0 * 8 + i * 16;
                        int a = bag_acts[i];
                        const char* t = LBL[a];
                        if (a == 1 && world.registered_item() == bag_act)
                            t = tr("DESENREGISTRER", "UNREGISTER");
                        ImVec2 ra = dp(23 * 8, ey), rb = dp(31 * 8, ey + 16);
                        if (clk && mp.x >= ra.x && mp.x <= rb.x &&
                            mp.y >= ra.y && mp.y <= rb.y) {
                            bag_asel = i; push_key(SDLK_E);
                        }
                        if (i == bag_asel) {
                            ImVec2 c0 = dp(23 * 8 + 1, ey + 2),
                                   c1 = dp(23 * 8 + 1, ey + 14),
                                   c2 = dp(23 * 8 + 7, ey + 8);
                            dl->AddTriangleFilled(c0, c1, c2, B_ORANGE);
                        }
                        dt_text(dl, t, 23 * 8 + 9, ey + 1, 0);
                    }
                }
                // ── toast : boîte message DS (6,19,14,4) ──
                if (bag_note_t > 0) {
                    bag_note_t -= dt;
                    dt_msgbox(dl, db, 6, 19, 14, 4);
                    dt_text(dl, bag_note, 6 * 8 + 4, 19 * 8 + 6, 0);
                }
                // ── sélecteur équipe : fenêtre DS centrée ──
                if (bag_party >= 0) {
                    auto& pty = world.party();
                    int np = (int)pty.size();
                    int ph = 24 + np * 18, pw2 = 176;
                    int px2 = 14, py2 = (144 - ph) / 2;
                    dt_stdframe(dl, db, px2 / 8, py2 / 8, pw2 / 8, ph / 8);
                    dt_text(dl,
                        tr("SUR QUI L'UTILISER ?", "USE ON WHICH POKéMON?"),
                        px2 + 8, py2 + 4, 0);
                    for (int i = 0; i < np; i++) {
                        auto& pm = pty[i];
                        float ry = py2 + 20 + i * 18;
                        ImVec2 ra = dp(px2 + 4, ry), rb = dp(px2 + pw2 - 4,
                            ry + 17);
                        bool hov2 = mp.x >= ra.x && mp.x <= rb.x &&
                                    mp.y >= ra.y && mp.y <= rb.y;
                        if (clk && hov2) {
                            if (i == bag_psel) push_key(SDLK_E);
                            else bag_psel = i;
                        }
                        if (i == bag_psel)
                            dl->AddRectFilled(ra, rb, B_SELBG, 3.f);
                        GLuint ic = world.mon_icon_tex(pm.species,
                            i == bag_psel ? iconf : 0);
                        if (ic)
                            dl->AddImage((ImTextureID)(intptr_t)ic,
                                dp(px2 + 6, ry - 4), dp(px2 + 30, ry + 20));
                        dt_text(dl, world.species_name(pm.species),
                            px2 + 34, ry + 4, 0);
                        char lv[16]; snprintf(lv, sizeof(lv), "N. %d",
                            pm.level);
                        dt_text(dl, lv,
                            px2 + pw2 - 10 - dt_text_w(lv), ry + 4, 6);
                        int mx2 = world.mon_maxhp(pm.species, pm.level,
                            pm.iv, pm.evs[0]);
                        float frac = mx2 ? (float)pm.hp / mx2 : 0;
                        frac = std::max(0.f, std::min(1.f, frac));
                        ImU32 hc = frac > 0.5f ? IM_COL32(90, 200, 90, 255)
                            : frac > 0.2f ? IM_COL32(250, 190, 50, 255)
                            : IM_COL32(230, 70, 60, 255);
                        dl->AddRectFilled(dp(px2 + 96, ry + 12),
                            dp(px2 + 148, ry + 16), IM_COL32(0, 0, 0, 255));
                        if (frac > 0)
                            dl->AddRectFilled(dp(px2 + 96, ry + 12),
                                dp(px2 + 96 + 52 * frac, ry + 16), hc);
                        if (pm.hp <= 0)
                            dt_text(dl, "KO", px2 + 150, ry + 4, 1);
                    }
                    if (!np)
                        dt_text(dl, tr("(equipe vide)", "(empty party)"),
                            px2 + 8, py2 + 24, 6);
                }
                // ── quantité à jeter : fenêtre DS (24,19,7,4) ──
                if (bag_toss >= 0 && bag_yn < 0) {
                    dt_stdframe(dl, db, 24, 19, 7, 4);
                    dt_text(dl, tr("JETER", "TOSS"), 24 * 8 + 4, 19 * 8 + 2, 0);
                    char q[16]; snprintf(q, sizeof(q), "x %d", bag_tossn);
                    dt_text(dl, q, 24 * 8 + 4, 19 * 8 + 18, 1);
                    int mxi = 1;
                    for (auto& kv : items)
                        if (kv.first == bag_toss) mxi = kv.second;
                    if (mwh) bag_tossn = std::max(1,
                        std::min(mxi, bag_tossn - mwh));
                    ImVec2 ra = dp(24 * 8, 19 * 8), rb = dp(31 * 8, 23 * 8);
                    if (clk && mp.x >= ra.x && mp.x <= rb.x &&
                        mp.y >= ra.y && mp.y <= rb.y) push_key(SDLK_E);
                }
                // ── confirmation OUI/NON : message + fenêtre (25,13,6,4) ──
                if (bag_yn >= 0) {
                    dt_msgbox(dl, db, 2, 19, 27, 4);
                    char q[200]; snprintf(q, sizeof(q),
                        tr("Jeter %s x %d ?", "Toss %s x %d?"),
                        world.item_name(bag_yn).c_str(), bag_tossn);
                    dt_text(dl, q, 16 + 4, 152 + 6, 0);
                    dt_stdframe(dl, db, 25, 13, 6, 4);
                    const char* LBL[] = {tr("OUI", "YES"), tr("NON", "NO")};
                    for (int i = 0; i < 2; i++) {
                        float ey = 13 * 8 + i * 16;
                        ImVec2 ra = dp(25 * 8, ey), rb = dp(31 * 8, ey + 16);
                        if (clk && mp.x >= ra.x && mp.x <= rb.x &&
                            mp.y >= ra.y && mp.y <= rb.y) {
                            bag_ysel = i; push_key(SDLK_E);
                        }
                        if (i == bag_ysel) {
                            ImVec2 c0 = dp(25 * 8 + 1, ey + 2),
                                   c1 = dp(25 * 8 + 1, ey + 14),
                                   c2 = dp(25 * 8 + 7, ey + 8);
                            dl->AddTriangleFilled(c0, c1, c2, B_ORANGE);
                        }
                        dt_text(dl, LBL[i], 25 * 8 + 9, ey + 1, 0);
                    }
                }
                // clic sur la boîte POCHE = fermer le sac
                if (clk && mp.x >= dp(0, 128).x && mp.x <= dp(96, 144).x &&
                    mp.y >= dp(0, 128).y && mp.y <= dp(96, 144).y)
                    push_key(SDLK_Q);
            } else if (ui_st == UI_PARTY) {
                // ── équipe : vrai écran pl_plist_gra (menu.NSCR) ──
                // icônes espèces aux positions DS + texte NFGR aux fenêtres DS
                static GLuint plistbg = 0;
                static bool plist_try = false;
                if (!plist_try) {
                    plist_try = true;
                    auto pn = db.narc("/resource/fra/pokelist/pl_plist_gra.narc");
                    if (pn && pn->count() > 17) {
                        auto t = spr::parse_ncgr(pn->member(15));
                        auto pals = spr::parse_nclr(pn->member(16));
                        if (auto im = spr::render_screen(pn->member(17), t, pals))
                            plistbg = ui_upload(*im);
                    }
                    ds_font_ready(db);
                }
                float sw = (float)sw2, sh = (float)sh2;
                g_uds = sh / 192.f;
                g_udx = (sw - 256.f * g_uds) * 0.5f;
                ImDrawList* bg = ImGui::GetBackgroundDrawList();
                if (plistbg)
                    bg->AddImage((ImTextureID)(intptr_t)plistbg,
                        dp(0, 0), dp(256, 192),
                        ImVec2(0, 0), ImVec2(1, 192 / 256.f));
                else sb_bg(bg, sw, sh);
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                auto& pty = world.party();
                int np = (int)pty.size();
                int iconf = (int)(SDL_GetTicks() / 250) & 1;
                // positions DS en px (décomp : MemberPanelTemplate +
                // sMemberWindowTemplates — fenêtres en tuiles *8)
                static const int ICX[6] = {30,158,30,158,30,158};
                static const int ICY[6] = {16,24,64,72,112,120};
                static const int PXX[6] = {0,128,0,128,0,128};    // panelX*8
                static const int PYY[6] = {0,8,48,56,96,104};     // panelY*8
                static const int NTX[6] = {48,176,48,176,48,176}; // nom (6/22,1/2)
                static const int NTY[6] = {8,16,56,64,104,112};
                static const int LVX[6] = {0,128,0,128,0,128};    // niveau (0/16,4/5)
                static const int LVY[6] = {32,40,80,88,128,136};
                static const int HPX[6] = {56,184,56,184,56,184}; // pv (7/23,*)
                static const int BRX[6] = {64,192,64,192,64,192}; // barre (8/24,*)
                static const int BRY[6] = {24,32,72,80,120,128};
                ImVec2 mp = ImGui::GetIO().MousePos;
                bool clk = ImGui::IsMouseClicked(0);
                for (int i = 0; i < np && i < 6; i++) {
                    auto& m = pty[i];
                    bool sel = (int)i == menu_sel;
                    float px0 = (float)PXX[i], py0 = (float)PYY[i];
                    bool hov = mp.x >= dp(px0, py0).x &&
                               mp.x <= dp(px0 + 128, py0 + 48).x &&
                               mp.y >= dp(px0, py0).y &&
                               mp.y <= dp(px0, py0 + 48).y;
                    if (clk && hov) {
                        if (sel) push_key(SDLK_E);
                        else menu_sel = i;
                    }
                    if (sel) {
                        dl->AddRectFilled(dp(px0, py0), dp(px0 + 128, py0 + 48),
                            IM_COL32(255, 255, 255, 30));
                        ImVec2 c0 = dp(px0 + 4, py0 + 18),
                               c1 = dp(px0 + 4, py0 + 30),
                               c2 = dp(px0 + 10, py0 + 24);
                        dl->AddTriangleFilled(c0, c1, c2,
                            IM_COL32(232, 40, 40, 255));
                    }
                    GLuint ic = world.mon_icon_tex(m.species, iconf);
                    if (ic) {
                        ImVec2 a = dp(ICX[i] - 16, ICY[i] - 16),
                               b = dp(ICX[i] + 16, ICY[i] + 16);
                        dl->AddImage((ImTextureID)(intptr_t)ic, a, b);
                    }
                    int ci = 5;    // texte blanc (panneaux sombres)
                    dt_text(dl, world.species_name(m.species),
                        NTX[i], NTY[i], ci);
                    // niveau : fenêtre (0/16,4/5), texte à +5,+2
                    char lv[16];
                    snprintf(lv, sizeof(lv), tr("N. %d", "Lv. %d"), m.level);
                    dt_text(dl, lv, LVX[i] + 5, LVY[i] + 2, ci);
                    // pv : cur à +4, "/" à +28, max à +36 (fenêtre 7/23)
                    int mx = world.mon_maxhp(m.species, m.level, m.iv, m.evs[0]);
                    float frac = mx ? (float)m.hp / mx : 0.f;
                    frac = std::max(0.f, std::min(1.f, frac));
                    char s1[12], s2[12];
                    snprintf(s1, sizeof(s1), "%d", m.hp);
                    snprintf(s2, sizeof(s2), "%d", mx);
                    dt_text(dl, s1, HPX[i] + 4, LVY[i] + 2, ci);
                    dt_text(dl, "/", HPX[i] + 28, LVY[i] + 2, ci);
                    dt_text(dl, s2, HPX[i] + 36, LVY[i] + 2, ci);
                    // barre PV : fenêtre (8/24,3/4), remplissage y+2..y+6, 48 px
                    ImVec2 ba = dp(BRX[i], BRY[i] + 2),
                           bb = dp(BRX[i] + 48, BRY[i] + 6);
                    dl->AddRectFilled(ba, bb, IM_COL32(8, 16, 24, 255));
                    if (frac > 0)
                        dl->AddRectFilled(ba,
                            ImVec2(ba.x + (bb.x - ba.x) * frac, bb.y),
                            hp_col(frac));
                    if (m.hp <= 0)
                        dt_text(dl, "KO", BRX[i] + 44, BRY[i], 1);
                }
                // bouton ANNULER DS (fenêtre 26,21) + message long (2,19,27,4)
                {
                    ImVec2 ra = dp(208, 168), rb = dp(248, 184);
                    bool hov = mp.x >= ra.x && mp.x <= rb.x &&
                               mp.y >= ra.y && mp.y <= rb.y;
                    if (clk && hov) push_key(SDLK_ESCAPE);
                    dt_text(dl, tr("ANNULER", "CANCEL"), 210, 172,
                        hov ? 1 : 5);
                }
                dt_msgbox(dl, db, 2, 19, 27, 4);
                if (!np)
                    dt_text(dl, tr("(equipe vide)", "(empty party)"), 20, 158, 0);
                else
                    dt_text(dl, tr("Choisir un Pokémon.", "Choose a POKÉMON."),
                        20, 158, 0);
            } else if (ui_st == UI_SAVE) {
                // ── sauvegarde DS : boîte message en bas + oui/non ──
                float sw = (float)sw2, sh = (float)sh2;
                g_uds = sh / 192.f;
                g_udx = (sw - 256.f * g_uds) * 0.5f;
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                ds_font_ready(db);
                // boîte message DS (2,19,27,4)
                dt_msgbox(dl, db, 2, 19, 27, 4);
                ImVec2 mp = ImGui::GetIO().MousePos;
                bool clk = ImGui::IsMouseClicked(0);
                if (save_st == 0) {
                    dt_text(dl, world.msg(213, 13), 20, 158, 0);
                    // oui/non DS : (25,13,6,4), texte +8, lignes pitch 16
                    const char* yn[2] = {tr("Oui","Yes"), tr("Non","No")};
                    dt_stdframe(dl, db, 25, 13, 6, 4);
                    for (int i = 0; i < 2; i++) {
                        float iy = 104 + i * 16;
                        ImVec2 ra = dp(200, iy), rb = dp(248, iy + 16);
                        if (clk && mp.x >= ra.x && mp.x <= rb.x &&
                            mp.y >= ra.y && mp.y <= rb.y) {
                            if (i == menu_sel) push_key(SDLK_E);
                            else menu_sel = i;
                        }
                        if (i == menu_sel) {
                            ImVec2 c0 = dp(201, iy + 2), c1 = dp(201, iy + 14),
                                   c2 = dp(207, iy + 8);
                            dl->AddTriangleFilled(c0, c1, c2, B_ORANGE);
                        }
                        dt_text(dl, yn[i], 208, iy + 1,
                            i == menu_sel ? 1 : 0);
                    }
                } else if (save_st == 1 || save_st == 2) {
                    save_t += ImGui::GetIO().DeltaTime;
                    dt_text(dl, world.msg(213, 15), 20, 158, 0);
                    if (save_st == 1) {
                        world.save("../save.dat");
                        save_st = 2;
                    }
                    if (save_t > 1.2f) save_st = 3;
                } else if (save_st == 3) {
                    char b[128];
                    snprintf(b, 128, world.msg(213, 16).c_str(),
                        world.player_name().c_str());
                    std::string s = b;
                    auto p = s.find("{103:0:0}");
                    if (p != std::string::npos)
                        s.replace(p, 9, world.player_name());
                    dt_text(dl, s, 20, 158, 0);
                    dt_scrollcur(dl, db, 236, 170);
                } else {
                    ui_st = UI_NONE;
                }
            } else if (ui_st == UI_OPTIONS) {
                // menu options : entrées DS (bank 220) + graphiques du port.
                // <- -> change la valeur, E sur CONFIRMER applique & sauve.
                auto m220 = [&](int i) { return world.msg(220, i); };
                auto yn = [&](int v) { return m220(v ? 13 : 14); };  // OUI/NON
                static const char* RES[] = {"1280x720","1600x900","1920x1080"};
                static const char* BUT[] = {"NORMAL","START = X","L = A"};
                static const char* MSAAV[] = {"NON","x2","x4","x8"};
                static const char* ANISOV[] = {"NON","x2","x4","x8","x16"};
                static const char* LANGV[] = {"FR","EN"};
                struct Row { std::string lab, val; int desc; };
                Row rows[] = {
                    {m220(3),  m220(10 + g_opts.text_speed),        43},
                    {m220(4),  yn(!g_opts.battle_scene),            44},
                    {m220(5),  m220(15 + g_opts.battle_style),      45},
                    {m220(6),  m220(17 + g_opts.sound),             46},
                    {m220(7),  BUT[g_opts.buttons],                 47},
                    {m220(8),  m220(22 + g_opts.frame),             48},
                    {tr("LANGUE", "LANGUAGE"), LANGV[g_opts.lang],  -9},
                    {tr("PLEIN ÉCRAN", "FULLSCREEN"), yn(g_opts.fullscreen), -1},
                    {tr("RÉSOLUTION", "RESOLUTION"), RES[g_opts.winscale], -2},
                    {tr("FILTRE TEXTURES", "TEXTURE FILTER"),
                        tr(g_opts.texfilter ? "LISSE" : "NET",
                           g_opts.texfilter ? "SMOOTH" : "SHARP"),  -3},
                    {tr("ANTICRÉNELAGE", "ANTIALIASING"),
                        g_opts.msaa ? MSAAV[g_opts.msaa] : tr("NON","OFF"), -6},
                    {tr("ANISOTROPIE", "ANISOTROPY"),
                        g_opts.aniso ? ANISOV[g_opts.aniso] : tr("NON","OFF"), -7},
                    {"VSYNC",        yn(g_opts.vsync),              -4},
                    {m220(9), "", -5},                              // CONFIRMER
                };
                const int NROW = (int)(sizeof(rows) / sizeof(rows[0]));
                float sw = (float)sw2, sh = (float)sh2;
                g_uds = sh / 192.f;
                g_udx = (sw - 256.f * g_uds) * 0.5f;
                // fond réel : config_gra.narc — la décomp remplit MAIN_2/SUB_0
                // de la tuile 1 (fond à motifs), et fait défiler la couche
                // MAIN_0 (tilemap 256x32 = cadre rouge) sur la ligne
                // sélectionnée : scroll = -(curseur*16 + 24).
                static GLuint opt_bg = 0, opt_band = 0;
                static bool opt_try = false;
                if (!opt_try) {
                    opt_try = true;
                    auto cn = db.narc("/graphic/config_gra.narc");
                    if (cn && cn->count() >= 3) {
                        auto op = spr::parse_nclr(cn->member(0));
                        auto ot = spr::parse_ncgr(cn->member(1));
                        if (!op.empty()) {
                            if (auto im = spr::compose_rep(ot, 1, 32, 24,
                                    op[0]))
                                opt_bg = ui_upload(*im);
                            if (auto im = spr::render_screen(cn->member(2),
                                    ot, op, true))
                                opt_band = ui_upload(*im);
                        }
                    }
                    ds_font_ready(db);
                }
                auto bg = ImGui::GetBackgroundDrawList();
                if (opt_bg)
                    bg->AddImage((ImTextureID)(intptr_t)opt_bg,
                        dp(0, 0), dp(256, 192));
                else sb_bg(bg, sw, sh);
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                ImVec2 mp = ImGui::GetIO().MousePos;
                bool clk = ImGui::IsMouseClicked(0);
                // titre "OPTIONS" : fenêtre (1,0,12,2), texte à +2,+2 = (10,2)
                dt_text(dl, m220(0), 10, 2, 5);
                // fenêtre entrées : cadre standard (1,3,30,14)
                dt_stdframe(dl, db, 1, 3, 30, 14);
                // 7 lignes visibles max — défile si plus (options du port)
                int rvis = 7;
                int roff = std::max(0,
                    std::min(menu_sel - rvis + 1, NROW - rvis));
                // curseur DS = la bande rouge de config_gra sur la ligne
                if (opt_band) {
                    float by = 24 + (menu_sel - roff) * 16;
                    dl->AddImage((ImTextureID)(intptr_t)opt_band,
                        dp(0, by), dp(256, by + 32),
                        ImVec2(0, 0), ImVec2(1, 1));
                }
                for (int vi = 0; vi < rvis && roff + vi < NROW; vi++) {
                    int i = roff + vi;
                    float ry = 24 + 2 + vi * 16;   // texte DS : +2 dans la ligne
                    bool sel = i == menu_sel;
                    ImVec2 ra = dp(8, ry - 2), rb = dp(248, ry + 14);
                    bool hov = mp.x >= ra.x && mp.x <= rb.x &&
                               mp.y >= ra.y && mp.y <= rb.y;
                    if (clk && hov) {
                        if (sel) push_key(SDLK_RIGHT);
                        else menu_sel = i;
                    }
                    // étiquette blanche, valeur à x=108 (DS CHOICES_X=100+8)
                    dt_text(dl, rows[i].lab, 12, ry, 5);
                    if (!rows[i].val.empty())
                        dt_text(dl, rows[i].val, 108, ry, sel ? 1 : 6);
                }
                // description : boîte message DS (2,19,27,4)
                dt_msgbox(dl, db, 2, 19, 27, 4);
                int d = rows[menu_sel].desc;
                std::string ds = d >= 0 ? m220(d)
                    : d == -1 ? tr("Affiche le jeu en plein écran.",
                                   "Displays the game fullscreen.")
                    : d == -2 ? tr("Change la taille de la fenêtre.",
                                   "Changes the window size.")
                    : d == -3 ? tr("Lisse ou pixelise les textures du monde.",
                                   "Smooths or pixelates world textures.")
                    : d == -4 ? tr("Synchronise l'affichage sur l'écran.",
                                   "Syncs rendering to the display.")
                    : d == -6 ? tr("Adoucit les contours des modèles 3D.",
                                   "Smooths the edges of 3D models.")
                    : d == -7 ? tr("Affine les textures vues de biais.",
                                   "Sharpens textures viewed at an angle.")
                    : d == -9 ? tr("Langue de l'interface du port.",
                                   "Language of the port interface.")
                    :           tr("Applique et sauvegarde ces paramètres.",
                                   "Applies and saves these settings.");
                std::string cl;
                for (size_t i = 0; i < ds.size(); i++) {
                    if (ds[i] == '{') {
                        auto e = ds.find('}', i);
                        if (e != std::string::npos) { i = e; continue; }
                    }
                    cl += ds[i];
                }
                // wrap aux mots sur la largeur de la boîte
                {
                    std::string ln; size_t k2 = 0; float dly = 158;
                    while (k2 < cl.size()) {
                        size_t sp = cl.find(' ', k2);
                        std::string w = cl.substr(k2,
                            sp == std::string::npos ? cl.size() - k2
                                                    : sp - k2);
                        if (!ln.empty() &&
                            dt_text_w(ln + " " + w) > 206) {
                            dt_text(dl, ln, 20, dly, 0);
                            dly += 14;
                            if (dly > 178) break;
                            ln = w;
                        } else ln = ln.empty() ? w : ln + " " + w;
                        if (sp == std::string::npos) break;
                        k2 = sp + 1;
                    }
                    if (!ln.empty() && dly <= 178)
                        dt_text(dl, ln, 20, dly, 0);
                }
            } else if (ui_st == UI_CARD) {
                // ── vraie carte dresseur : trainer_case.narc, couche SUB ──
                // recto NSCR 35 (affine 8bpp) + tuiles 27 ; héros NSCR 40/41
                // (texte u16) + tuiles 31. Palette GXS recomposée comme la
                // décomp : slot0 = case_platinum, slots1-15 = carte.
                float sw = (float)sw2, sh = (float)sh2;
                g_uds = sh / 192.f;
                g_udx = (sw - 256.f * g_uds) * 0.5f;
                static GLuint cardbg = 0, cardtr = 0;
                static bool card_try = false;
                if (!card_try) {
                    card_try = true;
                    auto tn = db.narc(
                        "/resource/fra/trainer_case/trainer_case.narc");
                    if (tn && tn->count() > 44) {
                        auto cardPal = spr::parse_nclr(tn->member(0));
                        auto casePal = spr::parse_nclr(tn->member(10));
                        std::vector<std::vector<RGBAf>> pals(16);
                        pals[0] = !casePal.empty() ? casePal[0]
                                    : std::vector<RGBAf>(16);
                        for (int i = 1; i < 16 && i < (int)cardPal.size(); i++)
                            pals[i] = cardPal[i];
                        auto ct = spr::parse_ncgr(tn->member(27));
                        if (auto im = spr::render_screen(tn->member(35),
                                ct, pals))
                            cardbg = ui_upload(*im);
                        auto tt = spr::parse_ncgr(tn->member(31));
                        int tscr = world.gender() ? 41 : 40;
                        if (auto im = spr::render_screen(tn->member(tscr),
                                tt, pals, true))
                            cardtr = ui_upload(*im);
                    }
                    ds_font_ready(db);
                }
                auto bg = ImGui::GetBackgroundDrawList();
                if (cardbg)
                    bg->AddImage((ImTextureID)(intptr_t)cardbg,
                        dp(0, 0), dp(256, 192),
                        ImVec2(0, 0), ImVec2(1, 0.75f));
                else sb_bg(bg, sw, sh);
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                if (cardtr)
                    dl->AddImage((ImTextureID)(intptr_t)cardtr,
                        dp(0, 0), dp(256, 192),
                        ImVec2(0, 0), ImVec2(1, 0.75f));
                // champs : étiquettes banque carte (616 FRA / 617 US) à x=16,
                // valeurs alignées droite (fenêtres partielles -> x152 ;
                // pleines -> x240)
                int cbank = db.game_code().size() >= 4 &&
                            db.game_code()[3] == 'F' ? 616 : 617;
                auto m617 = [&](int i) { return world.msg(cbank, i); };
                auto field = [&](int i, float ty, const std::string& val,
                                 int fullw) {
                    dt_text(dl, m617(i), 16, ty, 0);
                    float w = dt_text_w(val);
                    dt_text(dl, val, (fullw ? 240 : 152) - w, ty, 0);
                };
                unsigned idh = 2166136261u;
                for (char c : std::string(world.player_name()))
                    idh = (idh ^ (unsigned char)c) * 16777619u;
                char idb[32]; snprintf(idb, sizeof(idb), "%05u",
                    idh % 100000u);
                field(0, 32, idb, 0);                     // NºID
                field(1, 48, world.player_name(), 0);     // NOM
                char mb[32]; snprintf(mb, sizeof(mb), "%d $", world.money());
                field(2, 72, mb, 0);                      // ARGENT
                if (world.dex_obtained()) {
                    int seen = 0;
                    for (int b = 0; b < 8; b++)
                        if (world.badges() & (1 << b)) seen++;
                    char db2[16]; snprintf(db2, sizeof(db2), "%d", seen);
                    field(3, 96, db2, 0);                 // POKéDEX
                }
                char sc[16]; snprintf(sc, sizeof(sc), "%d",
                    (int)(world.money() / 10));
                field(4, 120, sc, 0);                     // SCORE
                int secs = (int)(ImGui::GetTime());
                char tb[32]; snprintf(tb, sizeof(tb), "%d : %02d",
                    secs / 3600, (secs / 60) % 60);
                field(5, 144, tb, 1);                     // DURÉE JEU
                field(6, 160, tr("----", "----"), 1);     // DÉBUT AVENTURE
            }
        }
        if (!world.starter_open() && starter_st) starter_st = 0;  // reset scène
        world.starter_sel = starter_sel;    // balle sous le curseur -> anim DS
        world.starter_phase = starter_st;   // phase choix/confirm -> zoom cam
        // ── choix du starter : vraie scène 3D ev_pokeselect rendue en GL ────
        // (choose_starter_app.c) — ImGui ne fait que le curseur main DS au-
        // dessus de la balle pointée + fenêtre d'aperçu + message banque 360.
        if (world.starter_open()) {
            ImVec2 io2 = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            float cx = io2.x * 0.5f;
            // curseur main DS au-dessus de la balle choisie (NDC -> écran)
            {
                float nx, ny;
                if (world.starter_ball_scr(starter_sel, &nx, &ny)) {
                    float bx = (nx * 0.5f + 0.5f) * io2.x;
                    float by = (-ny * 0.5f + 0.5f) * io2.y;
                    float bob = sinf((float)ImGui::GetTime() * 6.f) * 4.f;
                    if (starter_st == 0)              // gant jaune pointant
                        dl->AddTriangleFilled({bx - 12, by - 52 + bob},
                            {bx + 12, by - 52 + bob},
                            {bx, by - 28 + bob},
                            IM_COL32(255, 214, 90, 255));
                }
            }
            // sprite du Pokémon AU-DESSUS de la balle pointée (phase choix)
            // ou prise (phase confirmation) — comme sur DS
            {
                int sp = ScriptVM::starter_id(starter_sel);
                float nx, ny;
                float bx = cx, by = io2.y * 0.42f;
                if (world.starter_ball_scr(starter_sel, &nx, &ny)) {
                    bx = (nx * 0.5f + 0.5f) * io2.x;
                    by = (-ny * 0.5f + 0.5f) * io2.y;
                }
                int afr = (int)(ImGui::GetTime() * 2.5) & 1;   // ping-pong DS
                uint32_t pt0 = world.poke_tex(sp, 0, afr);
                if (pt0) {
                    float bob = sinf((float)ImGui::GetTime() * 4.f) * 3.f;
                    float ss = io2.y * (starter_st ? 0.34f : 0.26f);
                    dl->AddImage((ImTextureID)(intptr_t)pt0,
                        {bx - ss / 2, by - ss - 10.f + bob},
                        {bx + ss / 2, by - 10.f + bob});
                }
            }

            // message DS banque 360 : [7] choix, [1+i] confirmation balle
            {
                int mi = starter_st == 0 ? 7 : 1 + starter_sel;
                std::string m = world.msg(360, mi);
                std::string clean;                      // retire {FF00:x}
                for (size_t i = 0; i < m.size(); i++) {
                    if (m[i] == '{') {
                        auto e = m.find('}', i);
                        if (e != std::string::npos) { i = e; continue; }
                    }
                    clean += m[i];
                }
                float mw = io2.x * 0.62f, mh = 92.f;
                float mx = cx - mw / 2, my2 = io2.y - mh - 26.f;
                ds_font_ready(db);
                {
                    const MapHeader* MHS = db.header(world.header());
                    ds_window(dl, db, MHS ? (int)MHS->win : 0,
                        8.f * (io2.y / 720.f) * 2.6f,
                        ImVec2(mx, my2), ImVec2(mx + mw, my2 + mh));
                }
                float ssc = (io2.y / 720.f) * 2.6f;
                ds_text(dl, clean, mx + 24, my2 + (mh - g_dfont.cellH * ssc) / 2,
                    0, ssc);
                if (starter_st == 1) {                // oui/non façon DS
                    float bw = 110.f, bh = 34.f, gap = 12.f;
                    float ox = mx + mw - (bw * 2 + gap) - 18, oy = my2 + mh - bh - 12;
                    const char* opts[2] = {tr("Oui","Yes"), tr("Non","No")};
                    for (int i = 0; i < 2; i++) {
                        bool s = starter_yn == (i == 0);
                        ImU32 c = s ? IM_COL32(66, 96, 190, 255)
                                    : IM_COL32(28, 34, 48, 255);
                        dl->AddRectFilled({ox + i * (bw + gap), oy},
                            {ox + i * (bw + gap) + bw, oy + bh}, c, 8.f);
                        dl->AddRect({ox + i * (bw + gap), oy},
                            {ox + i * (bw + gap) + bw, oy + bh},
                            s ? C_ACCENT_B : C_PANEL_BD, 8.f, 0, 1.5f);
                        float tw2 = ds_text_w(opts[i], ssc * 0.7f);
                        ds_text(dl, opts[i],
                            ox + i * (bw + gap) + (bw - tw2) / 2,
                            oy + 9, 5, ssc * 0.7f);
                    }
                } else {
                    // indicateur "touche" discret façon flèche DS
                    float bx2 = mx + mw - 26, by2 = my2 + mh - 24;
                    float bob = sinf((float)ImGui::GetTime() * 5.f) * 3.f;
                    dl->AddTriangleFilled({bx2 - 7, by2 + bob},
                        {bx2 + 7, by2 + bob}, {bx2, by2 + 9 + bob},
                        C_ACCENT_B);
                }
            }
        }
        // ── PokemonPicture (0x0208) : espèce montrée en encart ─────────────
        if (int ps = world.pic_species()) {
            int sw2, sh2;
            SDL_GetWindowSize(win, &sw2, &sh2);
            ImGui::SetNextWindowPos(ImVec2(sw2 - 180.f, sh2 - 260.f));
            ImGui::SetNextWindowSize(ImVec2(160.f, 0.f));
            panel_push();
            ImGui::Begin("##pokepic", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            accent_line();
            ImGui::Text("%s", world.species_name(ps).c_str());
            ImGui::End();
            panel_pop();
        }
        // ── fondu écran (FadeScreen) ───────────────────────────────────────
        {
            float fa = world.fade_alpha();
            if (fa > 0.01f) {
                int sw2, sh2;
                SDL_GetWindowSize(win, &sw2, &sh2);
                uint32_t col = world.fade_color();
                ImGui::GetBackgroundDrawList()->AddRectFilled(
                    ImVec2(0, 0), ImVec2((float)sw2, (float)sh2),
                    IM_COL32((col >> 0) & 0xFF, (col >> 8) & 0xFF,
                        (col >> 16) & 0xFF, (int)(fa * 255)));
            }
        }
        ImGui::Render();
        rnd::resolve();                     // blit MSAA -> écran (avant ImGui)
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // captures AVANT le swap : le contenu du back buffer est indéfini
        // après SDL_GL_SwapWindow -> lectures striées.
        frames++;
        auto dump_frame = [&](const char* path) {
            int dw = 0, dh = 0;
            SDL_GetWindowSizeInPixels(win, &dw, &dh);   // drawable != fenêtre si HiDPI
            if (dw <= 0 || dh <= 0) { dw = sw; dh = sh; }
            std::vector<uint8_t> rgba((size_t)dw * dh * 4); // RGBA : toujours aligné 4
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            save_ppm(path, dw, dh, rgba.data());
        };
        if (recording && frames % 3 == 0) {      // F8 : dump 1 frame/3
            fs::create_directories("../_shots/rec");
            char p[256];
            snprintf(p, 256, "../_shots/rec/rec_%05d.ppm", rec_n++);
            dump_frame(p);
        }
        if (gif_at > 0 && frames >= gif_at && frames <= gif_end) {
            fs::create_directories("../_shots");
            char p[256];
            snprintf(p, 256, "../_shots/gif_%05d.ppm", frames);
            dump_frame(p);
            if (frames >= gif_end) running = false;
        }
        if (shot_at > 0 && frames >= shot_at) {
            fs::create_directories("../_shots");
            char p[256];
            snprintf(p, 256, "../_shots/world_%d.ppm", world.header());
            dump_frame(p);
            running = false;
        }
        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
