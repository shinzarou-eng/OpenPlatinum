// render.h — rendu GL3 : shader pos/nrm/col/uv, VBOs interleaved, textures RGBA.
// Chargeur de fonctions minimal via SDL_GL_GetProcAddress (pas de glad).
#pragma once
#include "common.h"
#include <unordered_map>

// matrice 4x4 colonne-major (compat GL)
struct Mat4 {
    float m[16];
    static Mat4 ident();
    static Mat4 perspective(float fov, float asp, float zn, float zf);
    static Mat4 ortho(float t, float asp, float zn, float zf);
    static Mat4 look_at(const Vec3& eye, const Vec3& tgt, const Vec3& up);
    Mat4 mul(const Mat4& o) const;
};

struct GpuMesh {
    uint32_t vao = 0, vbo = 0;
    int nverts = 0;
    uint32_t tex = 0;        // 0 = pas de texture
    RGBAf tint{1,1,1,1};
    float tint_k = 0.f;      // mix vers tint_to (DS CalculateTintedColor)
    float tint_to[3] = {0,0,0};
    bool alpha_cut = false;
    float blend = 1.f;       // <1 = translucide (POLYGON_ATTR alpha)
    int lit = 0xF;           // masque de lampes arealight DS (0 = non éclairé)
    uint8_t poly_id = 0;     // DS POLYGON_ATTR bits 24-29 (edge marking)
    bool fog = false;        // DS POLYGON_ATTR bit 15 (flag brouillard)
    bool water = false;      // surface d'eau (mer/lac) -> canal b de tField
    bool wfall = false;      // eau sur face verticale (cascade) -> scroll UV
    // water + normales à dominante verticale = cascade (le buffer uploadé
    // est en VF : pos3 nrm3 col4 uv2 — ny à l'offset 4)
    void mark_water(const std::vector<float>& buf) {
        if (!water || nverts <= 0) return;
        float s = 0;
        for (int i = 0; i < nverts; i++) s += fabsf(buf[(size_t)i * 12 + 4]);
        wfall = s / nverts < 0.6f;
    }
    bool glow = false;       // matériau lumineux (lampadaire/vitrail) -> HDR
    float ctr[3] = {0,0,0};  // centroïde local (position du halo lumineux)
    std::string dbg_name;    // debug : nom du matériau source
    // anim matériau DS (BTA0/SRT0) : transform UV en texels
    bool uvani = false;      // appliquer le transform ci-dessous
    float uvtr[4] = {0,0,1,1};  // transU,transV (texels), scaleU,scaleV
    float uvrot[2] = {1,0};     // cos,sin (pivot = centre texture)
};

// matériaux lumineux des villes : néons/enseignes (light*), vitraux
// (fenter*) et verres de lampadaires (c1_lamp*/lamp*)
inline bool mat_is_lamp(const std::string& n) {
    return n.rfind("light", 0) == 0 || n.rfind("fenter", 0) == 0
        || n.find("lamp") != std::string::npos
        || n.find("o02b") != std::string::npos;
}
// sources ponctuelles pour halos : lampadaires + néons (pas les vitraux —
// des halos sur des fenêtres de façade seraient faux). `o02b` = bandeau de
// verre des lampadaires urbains (le poteau `o02` reste du décor)
inline bool mat_is_light_src(const std::string& n) {
    return n.rfind("light", 0) == 0
        || n.find("lamp") != std::string::npos
        || n.find("o02b") != std::string::npos;
}

// surface d'eau animée : texture de la famille mer/lac (la plage hamabe,
// les rochers searock et les berges seaside restent du décor)
inline bool tex_is_water(const std::string& t) {
    if (t == "sea" || t == "lake" || t == "pond" || t == "umi")
        return true;
    return t.rfind("lakep", 0) == 0 || t.rfind("river", 0) == 0
        || t.rfind("mizu", 0) == 0 || t.rfind("water", 0) == 0
        || t.rfind("c1_fun", 0) == 0;               // fontaine (c1_fun1/2)
}

// gabarit de lumière DS (arealight.narc) : 4 lampes dir+couleur,
// diffuse/ambiante/spéculaire/émission — tout ramené en 0..1
struct DsLight {
    float dir[4][3] = {}, col[4][3] = {};
    int n = 0;
    float dif[3] = {}, amb[3] = {}, spc[3] = {}, emi[3] = {};
};

// paramètres du rendu « cinéma » (HD-2D) : poussés chaque frame par le monde
// avant rnd::composite — heure/météo/focus caméra -> uniforms du post-process
struct PostFx {
    float focus = 0.f;                  // distance caméra->cible (plan net)
    float zn = 150.f, zf = 900.f;       // plans de coupe caméra (linearize)
    int   ortho = 0;                    // projection ortho : profondeur linéaire
    float sun_uv[2] = {-10.f, -10.f};   // soleil/lune en uv écran (hors = off)
    float sun_col[3] = {1.f, .92f, .78f};
    float sun_k = 0.f;                  // intensité disque + rayons
    float day = 1.f;                    // 0 nuit .. 1 plein jour
    float dusk = 0.f;                   // part dorée aube/crépuscule
    float fog_amt = 0.f;                // densité atmosphère 0..1
    float fog_col[3] = {.75f, .8f, .88f};
    float bloom = .7f;
    float exposure = 1.f;
    float edge = 1.f;                   // force contours DS
    float dof = 1.f;                    // intensité flou de profondeur
    float rays = 1.f;                   // rayons volumétriques
    float grain = .5f, vign = .55f;
    float wet = 0.f;                    // sol détrempé (pluie) : reflets/sombre
    float room = 0.f;                   // 0 extérieur, 1 intérieur, 2 cave
    int   sky = 0;                      // 1 = ciel procédural (extérieur)
    float time = 0.f;                   // grain animé
};

namespace rnd {
    bool init();                          // charge les fonctions GL + shader
    void begin(const Mat4& mvp);          // clear + bind shader
    void draw(const GpuMesh& g, const Mat4& model);
    GpuMesh upload(const float* interleaved, int nverts, uint32_t tex);
    GpuMesh upload_dyn(const float* interleaved, int nverts, uint32_t tex);
    void update_dyn(GpuMesh& g, const float* interleaved, int nverts);
    uint32_t upload_tex(const Image& img);
    uint32_t upload_tex_nearest(const Image& img);   // pixel art : toujours NET
    void tex_write(uint32_t tex, const Image& img);   // écrase le contenu (VRAM DS)
    void free_mesh(GpuMesh& g);
    void free_tex(uint32_t t);
    void set_clear(float r, float g, float b);
    void set_light(const DsLight* L);         // nullptr = lumière neutre
    void set_uvdbg(int v);
    void set_tex_filter(int linear);        // 0 = pixels nets DS, 1 = lissé
    void set_msaa(int samples);             // 0 = off, 2/4/8 = MSAA (FBO -> blit)
    void set_composite(int on);             // 1 = composite DS (edge/fog post-pass)
    void set_edge_indoor(int in);           // palette de contours int/ext
    void composite(int w, int h);           // passe edge-marking -> écran
    void scene_begin();                     // bascule la scène dans le FBO composite
    int  composite_on();                    // composite actif ?
    void set_cinema(int on);                // 1 = rendu HD-2D (post-process)
    int  cinema_on();                       // mode cinéma actif ?
    void set_glow(float k);                 // boost HDR des matériaux `glow`
    void set_water(float t, float day);     // horloge+lumière eau (monde)
    void set_post(const PostFx* p);         // uniforms cinéma de la frame
    void set_dark(float ux, float uy, float k, float r); // halo grotte sombre
    void set_aniso(int level);              // 0 = off, 2/4/8/16 = anisotropie
    void resize(int w, int h);              // recrée le FBO MSAA à la taille fenêtre
    void resolve();                         // blit MSAA -> framebuffer écran (avant ImGui)
    uint32_t map_begin(int size);           // FBO offscreen carré (minimap) : bind+clear+shader, retourne sa texture (0 = échec)
    void     map_end();                     // revient au framebuffer/viewport précédent
    // layout vertex (12 floats / vertex) : pos3 nrm3 col4 uv2
    static constexpr int VF = 12;
}
