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
    bool alpha_cut = false;
    float blend = 1.f;       // <1 = translucide (POLYGON_ATTR alpha)
    int lit = 0xF;           // masque de lampes arealight DS (0 = non éclairé)
    uint8_t poly_id = 0;     // DS POLYGON_ATTR bits 24-29 (edge marking)
    bool fog = false;        // DS POLYGON_ATTR bit 15 (flag brouillard)
    std::string dbg_name;    // debug : nom du matériau source
};

// gabarit de lumière DS (arealight.narc) : 4 lampes dir+couleur,
// diffuse/ambiante/spéculaire/émission — tout ramené en 0..1
struct DsLight {
    float dir[4][3] = {}, col[4][3] = {};
    int n = 0;
    float dif[3] = {}, amb[3] = {}, spc[3] = {}, emi[3] = {};
};

namespace rnd {
    bool init();                          // charge les fonctions GL + shader
    void begin(const Mat4& mvp);          // clear + bind shader
    void draw(const GpuMesh& g, const Mat4& model);
    GpuMesh upload(const float* interleaved, int nverts, uint32_t tex);
    GpuMesh upload_dyn(const float* interleaved, int nverts, uint32_t tex);
    void update_dyn(GpuMesh& g, const float* interleaved, int nverts);
    uint32_t upload_tex(const Image& img);
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
    void set_aniso(int level);              // 0 = off, 2/4/8/16 = anisotropie
    void resize(int w, int h);              // recrée le FBO MSAA à la taille fenêtre
    void resolve();                         // blit MSAA -> framebuffer écran (avant ImGui)
    // layout vertex (12 floats / vertex) : pos3 nrm3 col4 uv2
    static constexpr int VF = 12;
}
