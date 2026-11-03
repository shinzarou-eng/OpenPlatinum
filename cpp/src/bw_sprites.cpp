// bw_sprites.cpp — index + chargement des sprites NB (images/NNN-Nom-*)
#include "bw_sprites.h"
#include "render.h"
#include <cstring>
#include "../thirdparty/stb/stb_image.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <unordered_map>
#include <cstdio>

namespace fs = std::filesystem;

// index (dex,face,shiny,genre) -> chemin — supporte les deux nommages :
//   "NNN-Nom-front.gif" / "-back.png" / "-shinyfront.gif" / "-shinyback.png"
//   "N-front-n.gif" / "N-back-n.gif" / "N-front-s.gif" / "N-back-s.gif"
//   (+ suffixe genre "-f"/"-m" avant l'extension : "25-front-n-f.gif")
// genre : -1 aucun/neutre, 0 mâle, 1 femelle
static std::unordered_map<int64_t, std::string> g_idx;
static int g_state = -1;                        // -1 pas tenté, 0 absent, 1 ok

static int64_t bw_key(int sp, int face, int shiny, int gender) {
    return ((int64_t)sp << 4) | ((face & 1) << 3) | ((shiny & 1) << 2) |
           (gender + 1);
}

static void scan_dir(const std::string& dir) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory(ec)) { scan_dir(e.path().string()); continue; }
        std::string fn = e.path().filename().string();
        if (fn.size() < 6) continue;
        int sp = atoi(fn.c_str());                // "NNN-..." requis
        if (sp <= 0) continue;
        int face = -1, shiny = 0, gender = -1;
        std::string low = fn;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("-front") != std::string::npos) face = 0;
        else if (low.find("-back") != std::string::npos) face = 1;
        else continue;
        if (low.find("shiny") != std::string::npos) shiny = 1;
        else {                                    // suffixe pack HD : -s.gif
            size_t dot = low.rfind('.');
            std::string stem = dot ? low.substr(0, dot) : low;
            if (stem.size() >= 2 &&
                stem.compare(stem.size() - 2, 2, "-s") == 0) shiny = 1;
            else if (stem.size() >= 4 &&
                stem.compare(stem.size() - 4, 4, "-s-f") == 0)
                shiny = 1, gender = 1;
            else if (stem.size() >= 4 &&
                stem.compare(stem.size() - 4, 4, "-s-m") == 0)
                shiny = 1, gender = 0;
        }
        {                                          // suffixe genre -f/-m
            size_t dot = low.rfind('.');
            std::string stem = dot ? low.substr(0, dot) : low;
            if (stem.size() >= 2) {
                std::string t = stem.substr(stem.size() - 2);
                if (t == "-f") gender = 1;
                else if (t == "-m") gender = 0;
            }
        }
        g_idx[bw_key(sp, face, shiny, gender)] = e.path().string();
    }
}

static void bw_scan() {
    g_state = 0;
    const char* dirs[] = {"images", "../images", "../../images",
                          "../../../images"};
    std::string bases[8]; int nb = 0;
    if (const char* bp = SDL_GetBasePath())
        for (const char* d : dirs) bases[nb++] = std::string(bp) + d;
    for (const char* d : dirs) bases[nb++] = d;
    for (int i = 0; i < nb; i++) {
        std::error_code ec;
        if (!fs::is_directory(bases[i], ec)) continue;
        scan_dir(bases[i]);
        if (!g_idx.empty()) {
            g_state = 1;
            printf("[bw] %zu fichiers NB indexés (%s)\n", g_idx.size(),
                bases[i].c_str());
            return;
        }
    }
}

int bw_frame_at(const BWSprite& s, uint32_t t_ms) {
    if (s.frames.size() <= 1 || s.total_ms <= 0) return 0;
    uint32_t m = t_ms % (uint32_t)s.total_ms;
    for (size_t i = s.cum_ms.size(); i-- > 0;)
        if (m >= (uint32_t)s.cum_ms[i]) return (int)i;
    return 0;
}

// les GIFs NB n'ont pas d'alpha : fond blanc opaque. Détourage par
// flood-fill depuis les bords sur les pixels quasi-blancs — le blanc
// interne du sprite (yeux, ventre) reste intact.
static void key_white(uint8_t* px, int w, int h) {
    auto white = [&](int x, int y) {
        const uint8_t* p = px + (y * w + x) * 4;
        return p[3] >= 250 && p[0] >= 240 && p[1] >= 240 && p[2] >= 240;
    };
    std::vector<int> q;
    std::vector<uint8_t> seen(w * h, 0);
    auto push = [&](int x, int y) {
        int i = y * w + x;
        if (!seen[i]) { seen[i] = 1; q.push_back(i); }
    };
    for (int x = 0; x < w; x++) { push(x, 0); push(x, h - 1); }
    for (int y = 0; y < h; y++) { push(0, y); push(w - 1, y); }
    static const int D4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (size_t i = 0; i < q.size(); i++) {
        int c = q[i], x = c % w, y = c / w;
        px[c * 4 + 3] = 0;
        for (auto& d : D4) {
            int nx = x + d[0], ny = y + d[1];
            if (nx >= 0 && nx < w && ny >= 0 && ny < h &&
                !seen[ny * w + nx] && white(nx, ny)) push(nx, ny);
        }
    }
}

static int feet_of(const uint8_t* px, int w, int h) {
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++)
            if (px[(y * w + x) * 4 + 3] > 16) return y + 1;
    }
    return h;
}

static bool fill_sprite(BWSprite& s, const uint8_t* rgba, int w, int h,
                        int n, const int* delays) {
    // canvas carré S×S (draws combat = quads fixes) : centré en x, collé en
    // bas pour que les pieds touchent le bas de la case
    int S = w > h ? w : h, ox = (S - w) / 2, oy = S - h;
    s.w = S; s.h = S;
    s.feet = oy + feet_of(rgba, w, h);
    int acc = 0;
    for (int i = 0; i < n; i++) {
        Image img;
        img.w = S; img.h = S;
        img.px.assign(S * S * 4, 0);
        const uint8_t* src = rgba + (size_t)i * w * h * 4;
        for (int y = 0; y < h; y++)
            memcpy(&img.px[((y + oy) * S + ox) * 4],
                   &src[y * w * 4], (size_t)w * 4);
        key_white(img.px.data(), S, S);
        if (i == 0) {                        // bbox réelle post-détourage
            int t = S, b = 0, l = S, r = 0;
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                    if (img.px[(y * S + x) * 4 + 3] > 16) {
                        if (y < t) t = y; if (y > b) b = y;
                        if (x < l) l = x; if (x > r) r = x;
                    }
            s.top = t; s.feet = b + 1; s.cx = (l + r) / 2;
        }
        uint32_t t = rnd::upload_tex_nearest(img);
        if (!t) return false;
        s.frames.push_back(t);
        s.cum_ms.push_back(acc);
        acc += delays && delays[i] > 0 ? delays[i] : 100;
    }
    s.total_ms = acc;
    return s.ok = true;
}

const BWSprite* bw_load(int sp, int face, int shiny, int gender) {
    if (g_state < 0) bw_scan();
    static std::unordered_map<int64_t, BWSprite> cache;
    int64_t key = bw_key(sp, face, shiny, gender);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.ok ? &it->second : nullptr;
    BWSprite s;
    if (g_state == 1) {
        std::string fp;
        for (int try_g = gender; !fp.size(); ) {
            int64_t k = bw_key(sp, face, shiny, try_g);
            auto gi = g_idx.find(k);
            if (gi != g_idx.end()) { fp = gi->second; break; }
            if (try_g == gender) { try_g = -1; continue; }
            if (try_g == -1) { try_g = 0; if (try_g == gender) break; continue; }
            break;
        }
        if (fp.empty() && shiny) {                  // shiny absent -> normal
            for (int tg : {gender, -1, 0}) {
                auto gi = g_idx.find(bw_key(sp, face, 0, tg));
                if (gi != g_idx.end()) { fp = gi->second; break; }
                if (tg == gender || tg == 0) break;
            }
        }
        if (fp.empty())                             // filet final : n'importe
            for (int tg : {-1, 0, 1})
                for (int ts : {0, 1}) {
                    auto gi = g_idx.find(bw_key(sp, face, ts, tg));
                    if (gi != g_idx.end()) { fp = gi->second; break; }
                }
        if (!fp.empty()) {
                FILE* f = fopen(fp.c_str(), "rb");
                if (f) {
                    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
                    std::vector<uint8_t> buf(sz);
                    if (fread(buf.data(), 1, sz, f) != (size_t)sz) buf.clear();
                    fclose(f);
                    if (!buf.empty()) {
                        if (fp.size() > 4 &&
                            fp.substr(fp.size() - 4) == ".gif") {
                            int x = 0, y = 0, z = 0, comp = 0; int* del = nullptr;
                            uint8_t* d = stbi_load_gif_from_memory(buf.data(),
                                (int)buf.size(), &del, &x, &y, &z, &comp, 4);
                            if (d && z > 0)
                                fill_sprite(s, d, x, y, z, del);
                            if (d) stbi_image_free(d);
                            if (del) free(del);
                        } else {
                            int x = 0, y = 0, comp = 0;
                            uint8_t* d = stbi_load_from_memory(buf.data(),
                                (int)buf.size(), &x, &y, &comp, 4);
                            if (d) {
                                fill_sprite(s, d, x, y, 1, nullptr);
                                stbi_image_free(d);
                            }
                        }
                    }
                }
        }
    }
    auto r = cache.emplace(key, std::move(s));
    return r.first->second.ok ? &r.first->second : nullptr;
}
