// render.cpp — GL3 loader minimal + pipeline mesh/texture.
#include "render.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <algorithm>

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

// ── chargement de fonctions GL ────────────────────────────────────────────────
#define GLDECL(ret, name, args) typedef ret (APIENTRY* name##_t) args; static name##_t pgl##name;
GLDECL(void, GenVertexArrays, (GLsizei, GLuint*))
GLDECL(void, BindVertexArray, (GLuint))
GLDECL(void, GenBuffers, (GLsizei, GLuint*))
GLDECL(void, BindBuffer, (GLenum, GLuint))
GLDECL(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))
GLDECL(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))
GLDECL(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
GLDECL(void, EnableVertexAttribArray, (GLuint))
GLDECL(GLuint, CreateShader, (GLenum))
GLDECL(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLDECL(void, CompileShader, (GLuint))
GLDECL(void, GetShaderiv, (GLuint, GLenum, GLint*))
GLDECL(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLDECL(GLuint, CreateProgram, (void))
GLDECL(void, AttachShader, (GLuint, GLuint))
GLDECL(void, LinkProgram, (GLuint))
GLDECL(void, GetProgramiv, (GLuint, GLenum, GLint*))
GLDECL(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLDECL(void, UseProgram, (GLuint))
GLDECL(GLint, GetUniformLocation, (GLuint, const GLchar*))
GLDECL(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))
GLDECL(void, Uniform1i, (GLint, GLint))
GLDECL(void, Uniform1f, (GLint, GLfloat))
GLDECL(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLDECL(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLDECL(void, Uniform3fv, (GLint, GLsizei, const GLfloat*))
GLDECL(void, GenTextures, (GLsizei, GLuint*))
GLDECL(void, ActiveTexture, (GLenum))
GLDECL(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
GLDECL(void, DeleteVertexArrays, (GLsizei, const GLuint*))
GLDECL(void, DeleteBuffers, (GLsizei, const GLuint*))
GLDECL(void, DeleteProgram, (GLuint))
GLDECL(void, DeleteShader, (GLuint))
GLDECL(void, GenFramebuffers, (GLsizei, GLuint*))
GLDECL(void, DeleteFramebuffers, (GLsizei, const GLuint*))
GLDECL(void, BindFramebuffer, (GLenum, GLuint))
GLDECL(GLenum, CheckFramebufferStatus, (GLenum))
GLDECL(void, GenRenderbuffers, (GLsizei, GLuint*))
GLDECL(void, DeleteRenderbuffers, (GLsizei, const GLuint*))
GLDECL(void, BindRenderbuffer, (GLenum, GLuint))
GLDECL(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
GLDECL(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))
GLDECL(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GLDECL(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLDECL(void, DrawBuffers, (GLsizei, const GLenum*))
GLDECL(void, Uniform2f, (GLint, GLfloat, GLfloat))
GLDECL(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GLDECL(void, GenerateMipmap, (GLenum))
#undef GLDECL

static GLuint _prog = 0;
static GLint _uMVP = -1, _uModel = -1, _uTex = -1, _uUseTex = -1,
    _uTint = -1, _uAlpha = -1, _uUVdbg = -1, _uPolyA = -1,
    _uLDir = -1, _uLCol = -1, _uLN = -1, _uDiff = -1, _uAmb = -1,
    _uSpec = -1, _uEmi = -1, _uLit = -1, _uFId = -1, _uGlow = -1,
    _uWtr = -1, _uWTime = -1, _uWDay = -1;
static GLint _uUVAni = -1, _uUVTr = -1, _uUVRot = -1;
static GLint _uTintTo = -1, _uTintK = -1;
static DsLight _light;
static int _uvdbg = 0;
static float _cr = 0.53f, _cg = 0.67f, _cb = 0.85f;

static bool load_gl() {
    bool ok = true;
    #define LOAD(name) pgl##name = (name##_t)SDL_GL_GetProcAddress("gl" #name); \
        if (!pgl##name) { fprintf(stderr, "gl%s manquant\n", #name); ok = false; }
    LOAD(GenVertexArrays) LOAD(BindVertexArray) LOAD(GenBuffers) LOAD(BindBuffer)
    LOAD(BufferData) LOAD(BufferSubData) LOAD(VertexAttribPointer)
    LOAD(EnableVertexAttribArray) LOAD(CreateShader) LOAD(ShaderSource)
    LOAD(CompileShader) LOAD(GetShaderiv) LOAD(GetShaderInfoLog)
    LOAD(CreateProgram) LOAD(AttachShader) LOAD(LinkProgram)
    LOAD(GetProgramiv) LOAD(GetProgramInfoLog) LOAD(UseProgram)
    LOAD(GetUniformLocation) LOAD(UniformMatrix4fv) LOAD(Uniform1i)
    LOAD(Uniform1f) LOAD(Uniform3f) LOAD(Uniform4f) LOAD(Uniform3fv)
    LOAD(GenTextures) LOAD(ActiveTexture)
    LOAD(TexImage2D)
    LOAD(DeleteVertexArrays) LOAD(DeleteBuffers) LOAD(DeleteProgram)
    LOAD(DeleteShader)
    LOAD(GenFramebuffers) LOAD(DeleteFramebuffers) LOAD(BindFramebuffer)
    LOAD(CheckFramebufferStatus) LOAD(GenRenderbuffers)
    LOAD(DeleteRenderbuffers) LOAD(BindRenderbuffer) LOAD(RenderbufferStorage)
    LOAD(RenderbufferStorageMultisample) LOAD(FramebufferRenderbuffer)
    LOAD(FramebufferTexture2D) LOAD(DrawBuffers) LOAD(Uniform2f)
    LOAD(BlitFramebuffer) LOAD(GenerateMipmap)
    #undef LOAD
    return ok;
}

// ── shaders ───────────────────────────────────────────────────────────────────
static const char* VS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec4 aCol;
layout(location=3) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform vec3 uLDir[4];   // arealight DS : direction des lampes (fx16/4096)
uniform vec3 uLCol[4];   // couleurs 0..1
uniform int  uLN;        // lampes actives
uniform vec3 uDiff, uAmb, uSpec, uEmi;
uniform int  uLit;       // masque de lampes (0 = couche OBJ, pas de lumière)
out vec4 vCol;
out vec2 vUV;
out vec3 vWpos;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vWpos = aPos;
    vCol = aCol;
    if (uLit != 0) {
        // GX : emission + S col_l*(spec*shine + diff*max(0,-dir.n) + amb)
        //   shine = max(0, -(dir.n - n.z)/2)²   (demi-vecteur DS)
        vec3 n = normalize(aNrm);
        vec3 l = uEmi;
        for (int i = 0; i < uLN; i++) {
            if ((uLit & (1 << i)) == 0) continue;
            float df = max(0.0, -dot(uLDir[i], n));
            float hv = max(0.0, -(dot(uLDir[i], n) - n.z) * 0.5);
            l += uLCol[i] * (uSpec * hv * hv + uDiff * df + uAmb);
        }
        vCol = vec4(aCol.rgb * min(l, vec3(1.0)), aCol.a);
    }
    vUV = aUV;
})";

static const char* FS = R"(#version 330 core
in vec4 vCol;
in vec2 vUV;
in vec3 vWpos;
uniform sampler2D uTex;
uniform int uUseTex;
uniform vec4 uTint;
uniform vec3 uTintTo;   // assombrissement DS : mix vers cette teinte…
uniform float uTintK;   // …de ce facteur (level/16, ciel Monde Distordu)
uniform float uAlpha;
uniform float uPolyA;
uniform int uUVdbg;
uniform float uGlow;     // boost HDR (lampadaires/vitraux la nuit, mode cinéma)
uniform int uWaterMesh;  // surface d'eau : ondulation en coordonnées monde
uniform float uWTime, uWDay;
uniform int  uUVAni;     // anim matériau BTA0/SRT0 active
uniform vec4 uUVTr;      // transU,transV (texels), scaleU,scaleV
uniform vec2 uUVRot;     // cos,sin — pivot au centre de la texture
uniform vec4 uFieldId;   // 2e cible DS : r = id polygone (0-63), a = flag brouillard
layout(location=1) out vec4 oField;
out vec4 frag;
float wh(vec2 p) {       // hash discret pour les facettes de lumière
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
void main() {
    oField = uFieldId;
    if (uUVdbg == 1) { frag = vec4(fract(vUV), 0.0, 1.0); return; }
    if (uUVdbg == 2) { frag = texture(uTex, vUV); return; }
    if (uUVdbg == 3) { frag = uTint; return; }
    vec4 c = vCol * uTint;
    vec2 wuv = vUV;
    if (uUVAni != 0 && uUseTex != 0) {
        // SRT0 DS : trans + rot(pivot centre) * scale, en espace texel
        vec2 ts = vec2(textureSize(uTex, 0));
        vec2 t = wuv * ts - ts * 0.5;
        t = vec2(t.x*uUVRot.x - t.y*uUVRot.y,
                 t.x*uUVRot.y + t.y*uUVRot.x) * uUVTr.zw;
        wuv = (t + ts * 0.5 + uUVTr.xy) / ts;
    }
    if (uWaterMesh == 2) {
        // cascade (face verticale, Monde Distordu) : la texture défile
        // vers le bas en continu comme la matière animée DS
        wuv.y -= uWTime * 0.35;
    } else if (uWaterMesh == 3) {
        // fontaine : le scroll vient déjà du SRT0 — on ajoute juste une
        // déformation ondulante (l'eau tremble en tombant)
        wuv.x += sin(vWpos.y * 0.30 + uWTime * 5.0) * 0.020
               + sin(vWpos.y * 0.11 - uWTime * 2.2) * 0.012;
    } else if (uWaterMesh != 0) {
        // vagues ancrées au monde : la surface ondule sous la caméra,
        // elle ne glisse pas avec l'écran
        wuv += vec2(sin(vWpos.x * 0.14 + uWTime * 2.0),
                    sin(vWpos.z * 0.12 - uWTime * 1.5)) * 0.010
             + vec2(sin((vWpos.x + vWpos.z) * 0.06 + uWTime * 1.2),
                    sin((vWpos.x - vWpos.z) * 0.07 - uWTime * 1.8)) * 0.006;
    }
    vec4 tc = uUseTex != 0 ? texture(uTex, wuv) : vec4(1.0);
    c *= tc;
    if (uTintK > 0.0) c.rgb = mix(c.rgb, uTintTo, uTintK);
    if (c.a < uAlpha) discard;
    // glow teinté chaud (lampe à incandescence) + léger report du blanc
    vec3 rgb = c.rgb + tc.rgb * uGlow * vec3(1.0, 0.72, 0.42)
             + c.rgb * uGlow * 0.25;
    if (uWaterMesh == 2) {
        // cascade : stries lumineuses qui coulent, pas de facettes « mer »
        float st = sin(vWpos.y * 0.8 - uWTime * 3.0)
                 * sin((vWpos.x + vWpos.z) * 2.2);
        rgb += vec3(0.05, 0.10, 0.16) * (st * 0.5 + 0.5);
    } else if (uWaterMesh == 3) {
        // fontaine : fines stries qui coulent le long du jet + rares
        // micro-gouttelettes d'écume descendantes
        float st = sin(vWpos.y * 0.9 - uWTime * 5.5)
                 * sin((vWpos.x + vWpos.z) * 1.6 + uWTime * 0.7);
        rgb += vec3(0.04, 0.09, 0.14) * (st * 0.5 + 0.5) * 0.7;
        vec3 wc = floor(vWpos * 1.6);
        wc.y -= floor(uWTime * 9.0);
        float g = wh(wc.xy + wc.z * 17.0);
        rgb += step(0.993, g) * vec3(0.55, 0.72, 0.9)
               * (0.25 + 0.75 * uWDay) * 0.22;
        // léger coup d'aqua pour unifier l'eau
        rgb = mix(rgb, rgb * vec3(0.82, 0.97, 1.08) + vec3(0.0, 0.015, 0.03), 0.12);
    } else if (uWaterMesh != 0) {
        // vaguelettes qui balayent la surface + facettes scintillantes,
        // le tout fixe par rapport au monde
        float ca = sin(vWpos.x * 0.9 + uWTime * 1.2)
                 * sin(vWpos.z * 0.7 - uWTime * 0.9);
        rgb += vec3(0.10, 0.30, 0.45) * (ca * 0.5 + 0.5) * 0.10;
        // cellule 3D : les faces verticales (cascades) ont x,z constants —
        // sans y le hash vaut une seule valeur et la face entière flash blanc
        vec3 wc = floor(vWpos * 2.0);
        float g = wh(wc.xy + wc.z * 17.0 + floor(uWTime * 6.0) * 13.7);
        rgb += step(0.965, g) * vec3(1.0, 0.97, 0.85)
               * (0.15 + 0.85 * uWDay) * 0.85;
    }
    frag = vec4(rgb, c.a * uPolyA);
})";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = pglCreateShader(type);
    pglShaderSource(s, 1, &src, nullptr);
    pglCompileShader(s);
    GLint ok = 0;
    pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; pglGetShaderInfoLog(s, 1024, nullptr, log);
        fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

namespace rnd {

bool init() {
    if (!load_gl()) return false;
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    _prog = pglCreateProgram();
    pglAttachShader(_prog, vs); pglAttachShader(_prog, fs);
    pglLinkProgram(_prog);
    GLint ok = 0;
    pglGetProgramiv(_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; pglGetProgramInfoLog(_prog, 1024, nullptr, log);
        fprintf(stderr, "link: %s\n", log);
        return false;
    }
    pglDeleteShader(vs); pglDeleteShader(fs);
    _uMVP = pglGetUniformLocation(_prog, "uMVP");
    _uModel = pglGetUniformLocation(_prog, "uModel");
    _uTex = pglGetUniformLocation(_prog, "uTex");
    _uUseTex = pglGetUniformLocation(_prog, "uUseTex");
    _uTint = pglGetUniformLocation(_prog, "uTint");
    _uTintTo = pglGetUniformLocation(_prog, "uTintTo");
    _uTintK = pglGetUniformLocation(_prog, "uTintK");
    _uAlpha = pglGetUniformLocation(_prog, "uAlpha");
    _uUVdbg = pglGetUniformLocation(_prog, "uUVdbg");
    _uPolyA = pglGetUniformLocation(_prog, "uPolyA");
    _uLDir = pglGetUniformLocation(_prog, "uLDir[0]");
    _uLCol = pglGetUniformLocation(_prog, "uLCol[0]");
    _uLN = pglGetUniformLocation(_prog, "uLN");
    _uDiff = pglGetUniformLocation(_prog, "uDiff");
    _uAmb = pglGetUniformLocation(_prog, "uAmb");
    _uSpec = pglGetUniformLocation(_prog, "uSpec");
    _uEmi = pglGetUniformLocation(_prog, "uEmi");
    _uLit = pglGetUniformLocation(_prog, "uLit");
    _uFId = pglGetUniformLocation(_prog, "uFieldId");
    _uGlow = pglGetUniformLocation(_prog, "uGlow");
    _uWtr = pglGetUniformLocation(_prog, "uWaterMesh");
    _uWTime = pglGetUniformLocation(_prog, "uWTime");
    _uWDay = pglGetUniformLocation(_prog, "uWDay");
    _uUVAni = pglGetUniformLocation(_prog, "uUVAni");
    _uUVTr = pglGetUniformLocation(_prog, "uUVTr");
    _uUVRot = pglGetUniformLocation(_prog, "uUVRot");
    (void)_uModel;
    glEnable(GL_DEPTH_TEST);
    if (getenv("CULL")) { glEnable(GL_CULL_FACE); glCullFace(getenv("CULL")[0]=='F'?GL_FRONT:GL_BACK); }
    else glDisable(GL_CULL_FACE);  // winding DS ≠ GL
    return true;
}

void set_clear(float r, float g, float b) { _cr = r; _cg = g; _cb = b; }
void set_uvdbg(int v) { _uvdbg = v; }

// ── MSAA : FBO multisample -> blit vers le framebuffer écran ────────────────
static int _msaa_n = 0;                 // echantillons demandes (0/2/4/8)
static int _aniso = 0;                  // niveau anisotropie (0/2/4/8/16)
static GLuint _msaa_fbo = 0, _msaa_col = 0, _msaa_dep = 0;
static int _msaa_w = 0, _msaa_h = 0;
static bool _msaa_dirty = true;
static void comp_free();                // (défini plus bas)

static void msaa_free() {
    if (_msaa_col) pglDeleteRenderbuffers(1, &_msaa_col);
    if (_msaa_dep) pglDeleteRenderbuffers(1, &_msaa_dep);
    if (_msaa_fbo) pglDeleteFramebuffers(1, &_msaa_fbo);
    _msaa_fbo = _msaa_col = _msaa_dep = 0;
}

static void msaa_ensure() {
    if (!_msaa_dirty) return;
    msaa_free();
    _msaa_dirty = false;
    if (_msaa_n <= 0 || _msaa_w <= 0 || _msaa_h <= 0) return;
    pglGenFramebuffers(1, &_msaa_fbo);
    pglBindFramebuffer(GL_FRAMEBUFFER, _msaa_fbo);
    pglGenRenderbuffers(1, &_msaa_col);
    pglBindRenderbuffer(GL_RENDERBUFFER, _msaa_col);
    pglRenderbufferStorageMultisample(GL_RENDERBUFFER, _msaa_n,
        GL_RGBA8, _msaa_w, _msaa_h);
    pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_RENDERBUFFER, _msaa_col);
    pglGenRenderbuffers(1, &_msaa_dep);
    pglBindRenderbuffer(GL_RENDERBUFFER, _msaa_dep);
    pglRenderbufferStorageMultisample(GL_RENDERBUFFER, _msaa_n,
        GL_DEPTH_COMPONENT24, _msaa_w, _msaa_h);
    pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_RENDERBUFFER, _msaa_dep);
    if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[msaa] FBO incomplet, MSAA desactive\n");
        msaa_free();
        _msaa_n = 0;
    }
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void set_msaa(int n) { _msaa_n = 0; _msaa_dirty = true; (void)n; }
void resize(int w, int h) {
    if (w != _msaa_w || h != _msaa_h) { _msaa_w = w; _msaa_h = h; _msaa_dirty = true; comp_free(); }
}

void resolve() {
    if (!_msaa_fbo) return;
    pglBindFramebuffer(GL_READ_FRAMEBUFFER, _msaa_fbo);
    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    pglBlitFramebuffer(0, 0, _msaa_w, _msaa_h, 0, 0, _msaa_w, _msaa_h,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ── cible offscreen légère (minimap) : FBO RGBA8 + profondeur ──────────────
static GLuint _map_fbo = 0, _map_tex = 0, _map_dep = 0;
static int _map_sz = 0;
static GLint _map_vp[4] = {0, 0, 0, 0};

uint32_t map_begin(int size) {
    if (size <= 0) return 0;
    if (_map_sz != size) {
        if (_map_tex) glDeleteTextures(1, &_map_tex);
        if (_map_dep) pglDeleteRenderbuffers(1, &_map_dep);
        if (_map_fbo) pglDeleteFramebuffers(1, &_map_fbo);
        _map_fbo = _map_tex = _map_dep = 0;
        glGenTextures(1, &_map_tex);
        glBindTexture(GL_TEXTURE_2D, _map_tex);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pglGenRenderbuffers(1, &_map_dep);
        pglBindRenderbuffer(GL_RENDERBUFFER, _map_dep);
        pglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
            size, size);
        pglGenFramebuffers(1, &_map_fbo);
        pglBindFramebuffer(GL_FRAMEBUFFER, _map_fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, _map_tex, 0);
        pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_RENDERBUFFER, _map_dep);
        if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[map] FBO incomplet\n");
            pglBindFramebuffer(GL_FRAMEBUFFER, 0);
            return 0;
        }
        _map_sz = size;
    }
    glGetIntegerv(GL_VIEWPORT, _map_vp);
    pglBindFramebuffer(GL_FRAMEBUFFER, _map_fbo);
    glViewport(0, 0, _map_sz, _map_sz);
    glClearColor(_cr, _cg, _cb, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pglUseProgram(_prog);
    pglUniform1i(_uTex, 0);
    pglUniform1i(_uUVdbg, _uvdbg);
    pglUniform1f(_uPolyA, 1.0f);
    glDisable(GL_BLEND);
    return _map_tex;
}

void map_end() {
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(_map_vp[0], _map_vp[1], _map_vp[2], _map_vp[3]);
}

// ── composite DS : scène -> FBO (couleur + champ id/brouillard + profondeur)
//    puis passe plein-écran : edge marking (contour 1px DS, blend AA 17/32)
static int _comp = 0, _cindoor = 0;
static int _cine = 0;                       // rendu cinéma HD-2D
static PostFx _post;
static GLuint _cfbo = 0, _ctex = 0, _ftex = 0, _dtex = 0;
static GLuint _cprog = 0, _cvao = 0;
static int _cw = 0, _ch = 0;
static GLint _cuStep = -1, _cuEdge = -1, _cuCol = -1, _cuFld = -1, _cuDep = -1,
             _cuFill = -1;
static void post_free();

void set_composite(int on) { _comp = on; }
void set_edge_indoor(int in) { _cindoor = in; }
void set_cinema(int on) {
    if ((on != 0) == (_cine != 0)) return;
    _cine = on;
    if (on) _comp = 1;                      // le rendu cinéma a besoin du FBO
    _msaa_dirty = true; comp_free(); post_free();   // format couleur change
}
int cinema_on() { return _cine; }
void set_post(const PostFx* p) { _post = p ? *p : PostFx{}; }
static float _duv[2] = {-10.f, -10.f}, _dk = 0.f, _dr = 0.3f;
void set_dark(float ux, float uy, float k, float r) {
    _duv[0] = ux; _duv[1] = uy; _dk = k; _dr = r;
}

static const char* CVS = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                  gl_VertexID == 2 ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
})";

static const char* CFS = R"(#version 330 core
in vec2 vUv;
uniform sampler2D tColor, tField, tDepth;
uniform vec2 uStep;              // un pixel DS (fenêtre / 256x192)
uniform vec3 uEdge[8];           // contour par groupe d'id (area_data DS)
uniform float uFill;             // 1 = remplir le vide (pillarbox intérieur)
uniform vec2 uDarkPos;           // halo joueur en uv (grotte sombre DS)
uniform float uDarkK, uDarkR;
uniform vec2 uRes;
out vec4 frag;
int fid(vec4 f) { return int(f.r * 63.0 + 0.5); }
void main() {
    vec4 f0 = texture(tField, vUv);
    float d0 = texture(tDepth, vUv).r;
    frag = texture(tColor, vUv);
    if (uFill > 0.5 && d0 >= 1.0) {
        // vide hors-map : fondu vers le noir doux au bord de la pièce
        vec2 c = (vUv - 0.5) * 2.0;
        float r = max(abs(c.x), abs(c.y));
        frag.rgb *= clamp(1.6 - r, 0.0, 1.0);
        return;
    }
    int id = fid(f0);
    const vec2 O[4] = vec2[4](vec2(1,0), vec2(-1,0), vec2(0,1), vec2(0,-1));
    for (int i = 0; i < 4; i++) {
        vec2 n = vUv + O[i] * uStep;
        vec4 nf = texture(tField, n);
        float nd = texture(tDepth, n).r;
        if (fid(nf) == id || nd <= d0) continue;   // voisin autre id + plus loin
        vec3 e = uEdge[id / 8];
        if (nd >= 1.0) frag.rgb = e;               // contour sur le fond : plein
        // sinon : AA DS — 17/32 du contour sur le pixel voisin (loin)
        else frag.rgb = (e * 17.0 + texture(tColor, n).rgb * 15.0) / 32.0;
        break;
    }
    // grotte sombre DS (météo 16/29) : noir sauf un halo autour du joueur
    if (uDarkK > 0.001) {
        vec2 dd = (vUv - uDarkPos) * vec2(uRes.x / uRes.y, 1.0);
        float dk = smoothstep(uDarkR * 0.45, uDarkR, length(dd));
        frag.rgb *= 1.0 - dk * uDarkK * 0.92;
    }
})";

static void comp_free() {
    GLuint t[3] = {_ctex, _ftex, _dtex};
    for (int i = 0; i < 3; i++) if (t[i]) glDeleteTextures(1, &t[i]);
    if (_cfbo) pglDeleteFramebuffers(1, &_cfbo);
    _cfbo = _ctex = _ftex = _dtex = 0; _cw = _ch = 0;
}

static void comp_ensure() {
    if (_msaa_w <= 0 || _msaa_h <= 0) return;
    if (_cfbo && _cw == _msaa_w && _ch == _msaa_h) return;
    comp_free();
    _cw = _msaa_w; _ch = _msaa_h;
    pglGenFramebuffers(1, &_cfbo);
    pglBindFramebuffer(GL_FRAMEBUFFER, _cfbo);
    auto tex = [&](GLenum fmt, GLuint* o, GLint filt = GL_NEAREST) {
        pglGenTextures(1, o); pglActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *o);
        pglTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, _cw, _ch, 0,
            fmt == GL_DEPTH_COMPONENT24 ? GL_DEPTH_COMPONENT : GL_RGBA,
            fmt == GL_RGBA16F ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    // mode cinéma : couleur HDR (RGBA16F, LINEAR pour la chaîne de blur) ;
    // sinon RGBA8/NEAREST (edge marking veut des ids exacts, jamais lissés)
    tex(_cine ? GL_RGBA16F : GL_RGBA8, &_ctex, _cine ? GL_LINEAR : GL_NEAREST);
    tex(GL_RGBA8, &_ftex);
    tex(GL_DEPTH_COMPONENT24, &_dtex);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _ctex, 0);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, _ftex, 0);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _dtex, 0);
    GLenum db[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    pglDrawBuffers(2, db);
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!_cprog) {
        GLuint vs = compile(GL_VERTEX_SHADER, CVS), fs = compile(GL_FRAGMENT_SHADER, CFS);
        _cprog = pglCreateProgram();
        pglAttachShader(_cprog, vs); pglAttachShader(_cprog, fs);
        pglLinkProgram(_cprog);
        pglDeleteShader(vs); pglDeleteShader(fs);
        _cuStep = pglGetUniformLocation(_cprog, "uStep");
        _cuEdge = pglGetUniformLocation(_cprog, "uEdge");
        _cuCol = pglGetUniformLocation(_cprog, "tColor");
        _cuFld = pglGetUniformLocation(_cprog, "tField");
        _cuDep = pglGetUniformLocation(_cprog, "tDepth");
        _cuFill = pglGetUniformLocation(_cprog, "uFill");
        pglGenVertexArrays(1, &_cvao);
    }
}

void scene_begin() {
    if (!_comp) return;
    comp_ensure();
    if (!_cfbo) return;
    pglBindFramebuffer(GL_FRAMEBUFFER, _cfbo);
    glViewport(0, 0, _cw, _ch);
    glClearColor(_cr, _cg, _cb, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

int composite_on() { return _comp && _cfbo != 0; }

// ── rendu cinéma HD-2D : HDR -> réductions -> finale ───────────────────────
// _pt : 0 = moitié, 1/2 = quart ping-pong (flou DoF), 3 = quart (bloom serré),
// 4/5 = huitième ping-pong (bloom large). Tout en RGBA16F + LINEAR.
struct PT { GLuint fbo = 0, tex = 0; int w = 0, h = 0; };
static PT _pt[6];
static GLuint _pdown = 0, _pbright = 0, _pblur = 0, _psky = 0, _px = 0;

// lib partagée : luminance, hash, ciel procédural (utilisée par le pass ciel
// et par la passe finale — les uniforms communs y sont déclarés une fois)
static const char* GLIB = R"(
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
uniform float uDay, uDusk, uSunK, uTime, uAspect;
uniform vec2 uSunUV;
uniform vec3 uSunCol;
vec3 skycol(vec2 uv) {
    float t = clamp(uv.y * 1.18 - 0.06, 0.0, 1.0);
    vec3 day  = mix(vec3(0.74, 0.83, 0.98), vec3(0.30, 0.55, 0.95), t);
    vec3 dusk = mix(vec3(1.10, 0.52, 0.26), vec3(0.36, 0.24, 0.55), t);
    vec3 ngt  = mix(vec3(0.11, 0.15, 0.30), vec3(0.015, 0.03, 0.09), t);
    vec3 c = mix(ngt, day, clamp(uDay, 0.0, 1.0));
    c = mix(c, dusk, clamp(uDusk * (1.0 - t * 0.7), 0.0, 1.0));
    return c;
}
)";

static const char* DOWN_FS = R"(
in vec2 vUv; uniform sampler2D tSrc; out vec4 frag;
void main() { frag = vec4(texture(tSrc, vUv).rgb, 1.0); }
)";

static const char* BRIGHT_FS = R"(
in vec2 vUv; uniform sampler2D tSrc; uniform float uThr; out vec4 frag;
void main() {
    vec3 c = texture(tSrc, vUv).rgb;
    frag = vec4(c * smoothstep(uThr, uThr + 0.55,
                   dot(c, vec3(0.299, 0.587, 0.114))), 1.0);
}
)";

static const char* BLUR_FS = R"(
in vec2 vUv; uniform sampler2D tSrc; uniform vec2 uDir; out vec4 frag;
void main() {
    vec3 c = texture(tSrc, vUv).rgb * 0.227027;
    c += (texture(tSrc, vUv + uDir * 1.3846154).rgb
        + texture(tSrc, vUv - uDir * 1.3846154).rgb) * 0.3162162;
    c += (texture(tSrc, vUv + uDir * 3.2307692).rgb
        + texture(tSrc, vUv - uDir * 3.2307692).rgb) * 0.0702703;
    frag = vec4(c, 1.0);
}
)";

// quad plein écran au fond (z = 0.9999) : ne passe le test de profondeur
// que là où la scène n'a rien dessiné -> le ciel devient de la « géométrie »
// pour les passes suivantes (il floute, bloom, contourne correctement)
static const char* SKY_VS = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                  gl_VertexID == 2 ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p.x, p.y, 0.9999, 1.0);
})";

static const char* SKY_FS_BODY = R"(
in vec2 vUv; out vec4 frag;
void main() { frag = vec4(skycol(vUv), 1.0); }
)";

// passe finale cinéma : brouillard -> DoF -> contours DS -> rayons -> bloom
// -> tonemap ACES -> grade chaud -> vignette -> grain + dither ordonné
static const char* XFS_BODY = R"(
in vec2 vUv;
uniform sampler2D tColor, tField, tDepth, tDof, tBloomA, tBloomB;
uniform vec2 uStep;
uniform vec3 uEdge[8];
uniform float uFill;
uniform vec2 uRes;
uniform float uNear, uFar, uFocus;
uniform int uOrtho;
uniform float uFogAmt;
uniform vec3 uFogCol;
uniform float uBloomK, uExposure, uEdgeK, uDofK, uRaysK, uGrain, uVign;
uniform float uWet;
uniform float uRoom;
uniform vec2 uDarkPos;
uniform float uDarkK, uDarkR;
out vec4 frag;
int fid(vec4 f) { return int(f.r * 63.0 + 0.5); }
float lin(float d) {
    return uOrtho != 0 ? uNear + d * (uFar - uNear)
        : uNear * uFar / max(1.0, uFar - d * (uFar - uNear));
}
void main() {
    vec4 f0 = texture(tField, vUv);
    float d0 = texture(tDepth, vUv).r;
    vec3 col = texture(tColor, vUv).rgb;
    // >= 0.9997 : ciel DW (0.9999) + nuages OBJ (0.9998) — le fog G3X DS
    // ne touche ni les sprites OBJ ni les BG 2D, et pas de contours non plus
    bool skypx = d0 >= 0.9997;
    if (skypx && uFill > 0.5) {                    // vide intérieur : fondu
        vec2 c2 = (vUv - 0.5) * 2.0;
        float e = max(abs(c2.x), abs(c2.y));
        vec3 voidc = uRoom > 1.5 ? vec3(0.006, 0.008, 0.018)
                                 : vec3(0.030, 0.040, 0.080);
        col = mix(col, voidc, smoothstep(0.55, 1.30, e));
    }
    float wmask = f0.b;                          // surface d'eau (mesh flagué)
    if (wmask > 0.01 && !skypx) {
        // l'ondulation/vagues/reflets vivent dans le shader scène
        // (coordonnées monde) — ici la couleur est déjà la bonne
        float lw = lum(col);
        // distance à la rive : terre ferme dans un rayon court OU large
        float shore = 0.0, rim = 0.0;
        for (int i = 0; i < 8; i++) {
            float an = float(i) * 0.785398;
            vec2 di = vec2(cos(an), sin(an));
            vec2 n1 = vUv + di * uStep * 4.0;
            vec2 n2 = vUv + di * uStep * 11.0;
            if (texture(tField, n1).b < 0.5
                    && texture(tDepth, n1).r < 0.999) { shore += 1.0; rim += 1.0; }
            else if (texture(tField, n2).b < 0.5
                    && texture(tDepth, n2).r < 0.999) shore += 0.4;
        }
        shore = clamp(shore * 0.125, 0.0, 1.0);
        rim = clamp(rim * 0.125, 0.0, 1.0);
        // corps : cyan clair au bord -> bleu profond au large (style PBA)
        vec3 shal = vec3(0.10, 0.55, 0.82);
        vec3 deep = vec3(0.02, 0.20, 0.56);
        vec3 body = mix(deep, shal, clamp(shore * 0.85 + lw * 0.40, 0.0, 1.0));
        col = mix(col, body * (0.55 + 1.15 * lw), 0.72);
        // reflet de ciel : loin (haut d'écran) = angle ras -> voile lumineux
        float graze = smoothstep(0.35, 0.95, vUv.y);
        col += skycol(vec2(vUv.x, 0.85)) * graze * 0.13;
        // mousse de rive : bande serrée + liseré blanc animé
        float fb = rim * (0.45
                     + 0.55 * sin(uTime * 2.4 + vUv.x * 80.0 + vUv.y * 55.0));
        col = mix(col, vec3(0.75, 0.88, 1.0), fb * 0.5);
        col = mix(col, vec3(0.95, 0.99, 1.0),
                  clamp(rim * shore * 2.0 - 0.25, 0.0, 1.0) * 0.30);
    }
    float fl = lin(d0);
    if (!skypx) {                                // brouillard de distance
        float fogf = clamp(1.0 - exp(-fl * uFogAmt * 0.0018), 0.0, 0.9);
        col = mix(col, uFogCol, fogf);
    }
    // fidèle DS : scène + brouillard + contours de polygones. La couche
    // « cinéma » (tonemap, DoF, bloom, vignette, grain, rayons, grades,
    // AO, dither) est retirée — aucun de ces effets n'existe sur DS.
    int id = fid(f0);
    const vec2 O[4] = vec2[4](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1));
    for (int i = 0; i < 4; i++) {
        vec2 n = vUv + O[i] * uStep;
        vec4 nf = texture(tField, n);
        float nd = texture(tDepth, n).r;
        if (fid(nf) == id || nd <= d0) continue;
        vec3 e = uEdge[id / 8];
        vec3 ec = nd >= 1.0 ? e
            : (e * 17.0 + texture(tColor, n).rgb * 15.0) / 32.0;
        col = mix(col, ec, uEdgeK);
        break;
    }
    // grotte sombre DS (météo 16/29) : tout s'éteint sauf un halo autour
    // du joueur — Flash (météo -> CLEAR) retire l'effet entièrement.
    if (uDarkK > 0.001) {
        vec2 dd = (vUv - uDarkPos) * vec2(uRes.x / uRes.y, 1.0);
        float dk = smoothstep(uDarkR * 0.45, uDarkR, length(dd));
        col *= 1.0 - dk * uDarkK * 0.92;
    }
    frag = vec4(col, 1.0);
}
)";

static void post_free() {
    for (auto& p : _pt) {
        if (p.tex) glDeleteTextures(1, &p.tex);
        if (p.fbo) pglDeleteFramebuffers(1, &p.fbo);
        p = PT{};
    }
}

// compile un FS de post : #version + [lib] + corps, link avec le quad VS
static GLuint post_prog(const char* body, bool lib) {
    std::string s = "#version 330 core\n";
    if (lib) s += GLIB;
    s += body;
    GLuint vs = compile(GL_VERTEX_SHADER, CVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, s.c_str());
    GLuint pr = pglCreateProgram();
    pglAttachShader(pr, vs); pglAttachShader(pr, fs);
    pglLinkProgram(pr);
    GLint ok = 0; pglGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char lg[1024]; pglGetProgramInfoLog(pr, 1024, nullptr, lg);
        fprintf(stderr, "link post: %s\n", lg);
    }
    pglDeleteShader(vs); pglDeleteShader(fs);
    pglUseProgram(pr);
    pglUniform1i(pglGetUniformLocation(pr, "tSrc"), 0);
    return pr;
}

static void post_ensure() {
    if (!_cine || _cw <= 0 || _ch <= 0) return;
    if (!_pdown) {
        _pdown   = post_prog(DOWN_FS, false);
        _pbright = post_prog(BRIGHT_FS, false);
        _pblur   = post_prog(BLUR_FS, false);
        {   // ciel : VS dédié (quad au fond) + FS depuis la lib
            std::string s = "#version 330 core\n" + std::string(GLIB) + SKY_FS_BODY;
            GLuint vs = compile(GL_VERTEX_SHADER, SKY_VS);
            GLuint fs = compile(GL_FRAGMENT_SHADER, s.c_str());
            _psky = pglCreateProgram();
            pglAttachShader(_psky, vs); pglAttachShader(_psky, fs);
            pglLinkProgram(_psky);
            pglDeleteShader(vs); pglDeleteShader(fs);
        }
        _px = post_prog(XFS_BODY, true);
        pglUseProgram(_px);
        pglUniform1i(pglGetUniformLocation(_px, "tColor"), 0);
        pglUniform1i(pglGetUniformLocation(_px, "tField"), 1);
        pglUniform1i(pglGetUniformLocation(_px, "tDepth"), 2);
        pglUniform1i(pglGetUniformLocation(_px, "tDof"), 3);
        pglUniform1i(pglGetUniformLocation(_px, "tBloomA"), 4);
        pglUniform1i(pglGetUniformLocation(_px, "tBloomB"), 5);
    }
    if (_pt[0].fbo && _pt[0].w == _cw / 2 && _pt[0].h == _ch / 2) return;
    post_free();
    const int W[6] = {_cw/2, _cw/4, _cw/4, _cw/4, _cw/8, _cw/8};
    const int H[6] = {_ch/2, _ch/4, _ch/4, _ch/4, _ch/8, _ch/8};
    for (int i = 0; i < 6; i++) {
        PT& p = _pt[i];
        p.w = W[i] > 0 ? W[i] : 1; p.h = H[i] > 0 ? H[i] : 1;
        pglGenTextures(1, &p.tex);
        pglActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, p.w, p.h, 0,
                      GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pglGenFramebuffers(1, &p.fbo);
        pglBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, p.tex, 0);
        if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[post] FBO %d incomplet\n", i);
    }
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// uniforms ciel communs au pass ciel et à la passe finale
static void sky_uni(GLuint pr, float aspect) {
    pglUniform1f(pglGetUniformLocation(pr, "uDay"), _post.day);
    pglUniform1f(pglGetUniformLocation(pr, "uDusk"), _post.dusk);
    pglUniform1f(pglGetUniformLocation(pr, "uSunK"), _post.sun_k);
    pglUniform1f(pglGetUniformLocation(pr, "uTime"), _post.time);
    pglUniform1f(pglGetUniformLocation(pr, "uAspect"), aspect);
    pglUniform2f(pglGetUniformLocation(pr, "uSunUV"),
                 _post.sun_uv[0], _post.sun_uv[1]);
    pglUniform3f(pglGetUniformLocation(pr, "uSunCol"),
                 _post.sun_col[0], _post.sun_col[1], _post.sun_col[2]);
}

// passe composite : FBO scène -> cible de sortie (MSAA ou écran)
void composite(int w, int h) {
    if (!_comp || !_cfbo) return;
    if (_cine) {
        post_ensure();
        // 1. ciel procédural écrit dans la scène là où depth == 1
        if (_post.sky && _psky) {
            pglBindFramebuffer(GL_FRAMEBUFFER, _cfbo);
            glViewport(0, 0, _cw, _ch);
            pglUseProgram(_psky);
            sky_uni(_psky, _ch ? (float)_cw / (float)_ch : 1.f);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            pglBindVertexArray(_cvao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
        }
    }
    // 3. finale : DS (CFS) ou cinéma (_px) -> écran
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, _msaa_w, _msaa_h);
    glDisable(GL_DEPTH_TEST);
    GLuint pr = _cine ? _px : _cprog;
    pglUseProgram(pr);
    // contours DS : extérieur gris foncé RGB(4,4,4) ; intérieur id 0 = noir
    const float dg = (4 * 8 + 1) / 255.f;           // rgb5(4) -> 0..1
    float ec[24];
    for (int i = 0; i < 8; i++) {
        bool blk = _cindoor && i == 0;
        ec[i*3] = ec[i*3+1] = ec[i*3+2] = blk ? 0.f : dg;
    }
    pglUniform3fv(pglGetUniformLocation(pr, "uEdge"), 8, ec);
    pglUniform2f(pglGetUniformLocation(pr, "uStep"), 1.f/256.f, 1.f/192.f);
    pglUniform1f(pglGetUniformLocation(pr, "uFill"),
                 _cindoor && !getenv("NOFILL") ? 1.f : 0.f);
    // halo grotte sombre : commun aux deux passes (DS + cinéma)
    pglUniform2f(pglGetUniformLocation(pr, "uDarkPos"), _duv[0], _duv[1]);
    pglUniform1f(pglGetUniformLocation(pr, "uDarkK"), _dk);
    pglUniform1f(pglGetUniformLocation(pr, "uDarkR"), _dr);
    pglUniform2f(pglGetUniformLocation(pr, "uRes"),
                 (float)_msaa_w, (float)_msaa_h);
    pglActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, _ctex);
    pglActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, _ftex);
    pglActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, _dtex);
    if (_cine) {
        float asp = _ch ? (float)_cw / (float)_ch : 1.f;
        pglActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, _pt[1].tex);
        pglActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, _pt[3].tex);
        pglActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, _pt[4].tex);
        sky_uni(pr, asp);
        pglUniform1f(pglGetUniformLocation(pr, "uNear"), _post.zn);
        pglUniform1f(pglGetUniformLocation(pr, "uFar"), _post.zf);
        pglUniform1f(pglGetUniformLocation(pr, "uFocus"), _post.focus);
        pglUniform1i(pglGetUniformLocation(pr, "uOrtho"), _post.ortho);
        pglUniform1f(pglGetUniformLocation(pr, "uFogAmt"), _post.fog_amt);
        pglUniform3f(pglGetUniformLocation(pr, "uFogCol"),
                     _post.fog_col[0], _post.fog_col[1], _post.fog_col[2]);
        pglUniform1f(pglGetUniformLocation(pr, "uBloomK"), _post.bloom);
        pglUniform1f(pglGetUniformLocation(pr, "uExposure"), _post.exposure);
        pglUniform1f(pglGetUniformLocation(pr, "uEdgeK"), _post.edge);
        pglUniform1f(pglGetUniformLocation(pr, "uDofK"), _post.dof);
        pglUniform1f(pglGetUniformLocation(pr, "uRaysK"), _post.rays);
        pglUniform1f(pglGetUniformLocation(pr, "uGrain"), _post.grain);
        pglUniform1f(pglGetUniformLocation(pr, "uVign"), _post.vign);
        pglUniform1f(pglGetUniformLocation(pr, "uWet"), _post.wet);
        pglUniform1f(pglGetUniformLocation(pr, "uRoom"), _post.room);
        pglUniform2f(pglGetUniformLocation(pr, "uRes"),
                     (float)_msaa_w, (float)_msaa_h);
    }
    pglBindVertexArray(_cvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    pglActiveTexture(GL_TEXTURE0);
    pglUseProgram(_prog);
    glEnable(GL_DEPTH_TEST);
    (void)w; (void)h;
}

void begin(const Mat4& /*mvp*/) {
    if (_msaa_n > 0) {
        msaa_ensure();
        if (_msaa_fbo) pglBindFramebuffer(GL_FRAMEBUFFER, _msaa_fbo);
    }
    glClearColor(_cr, _cg, _cb, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pglUseProgram(_prog);
    pglUniform1i(_uTex, 0);
    pglUniform1i(_uUVdbg, _uvdbg);
    pglUniform1f(_uPolyA, 1.0f);
    glDisable(GL_BLEND);
}

void set_light(const DsLight* L) { _light = L ? *L : DsLight{}; }

static float _glow_k = 0.f;              // boost HDR des matériaux `glow`
void set_glow(float k) { _glow_k = k; }
static float _wtr_t = 0.f, _wtr_d = 1.f;   // horloge/lumière de l'eau (cinéma)
void set_water(float t, float day) { _wtr_t = t; _wtr_d = day; }

void draw(const GpuMesh& g, const Mat4& mvp) {
    if (!g.vao || g.nverts == 0) return;
    Mat4 ident = Mat4::ident();
    pglUniformMatrix4fv(_uMVP, 1, GL_FALSE, mvp.m);
    pglUniformMatrix4fv(_uModel, 1, GL_FALSE, ident.m);
    pglUniform4f(_uTint, g.tint.r, g.tint.g, g.tint.b, g.tint.a);
    pglUniform3f(_uTintTo, g.tint_to[0], g.tint_to[1], g.tint_to[2]);
    pglUniform1f(_uTintK, g.tint_k);
    pglUniform1i(_uUseTex, g.tex ? 1 : 0);
    pglUniform1f(_uAlpha, g.alpha_cut ? 0.4f : 0.0f);
    pglUniform1f(_uPolyA, g.blend);
    pglUniform1i(_uLit, g.lit);
    pglUniform1f(_uGlow, g.glow ? _glow_k : 0.f);
    pglUniform1i(_uWtr, g.uvani && g.water ? 3
        : g.wfall ? 2 : (g.water && _cine) ? 1 : 0);
    pglUniform1f(_uWTime, _wtr_t);
    pglUniform1f(_uWDay, _wtr_d);
    pglUniform1i(_uUVAni, g.uvani ? 1 : 0);
    if (g.uvani) {
        pglUniform4f(_uUVTr, g.uvtr[0], g.uvtr[1], g.uvtr[2], g.uvtr[3]);
        pglUniform2f(_uUVRot, g.uvrot[0], g.uvrot[1]);
    }
    // 2e cible : id de polygone DS ; alpha 0 si translucide (la DS garde
    // l'id du dessous et AND le flag brouillard — approx : on l'écrit quand même)
    pglUniform4f(_uFId, g.poly_id / 63.f, 0.f, g.water ? 1.f : 0.f,
        (g.fog && g.blend > 0.999f) ? 1.f : 0.f);
    pglUniform3fv(_uLDir, 4, &_light.dir[0][0]);
    pglUniform3fv(_uLCol, 4, &_light.col[0][0]);
    pglUniform1i(_uLN, _light.n);
    pglUniform3fv(_uDiff, 1, _light.dif);
    pglUniform3fv(_uAmb, 1, _light.amb);
    pglUniform3fv(_uSpec, 1, _light.spc);
    pglUniform3fv(_uEmi, 1, _light.emi);
    if (g.blend < 0.999f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    if (g.tex) { pglActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g.tex); }
    pglBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, g.nverts);
}

GpuMesh upload(const float* data, int nverts, uint32_t tex) {
    GpuMesh g;
    g.nverts = nverts;
    g.tex = tex;
    double cx = 0, cy = 0, cz = 0;                  // centroïde (halos lampes)
    for (int i = 0; i < nverts; i++) {
        cx += data[(size_t)i * VF];
        cy += data[(size_t)i * VF + 1];
        cz += data[(size_t)i * VF + 2];
    }
    if (nverts) {
        g.ctr[0] = (float)(cx / nverts);
        g.ctr[1] = (float)(cy / nverts);
        g.ctr[2] = (float)(cz / nverts);
    }
    pglGenVertexArrays(1, &g.vao);
    pglBindVertexArray(g.vao);
    pglGenBuffers(1, &g.vbo);
    pglBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    pglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * VF * 4, data, GL_STATIC_DRAW);
    int st = VF * 4;
    pglVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);
    pglVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)12);
    pglVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void*)24);
    pglVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)40);
    for (int i = 0; i < 4; i++) pglEnableVertexAttribArray(i);
    pglBindVertexArray(0);
    return g;
}

GpuMesh upload_dyn(const float* data, int nverts, uint32_t tex) {
    GpuMesh g = upload(data, nverts, tex);
    pglBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    pglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * VF * 4, data, GL_DYNAMIC_DRAW);
    return g;
}

void update_dyn(GpuMesh& g, const float* data, int nverts) {
    pglBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    if (nverts > g.nverts) {
        pglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * VF * 4, data, GL_DYNAMIC_DRAW);
        g.nverts = nverts;
    } else {
        pglBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nverts * VF * 4, data);
        g.nverts = nverts;
    }
}

// registre des textures monde (le filtre NET/LISSE s'applique à toutes)
static std::vector<GLuint> s_tex;
static int s_tex_lin = 0;
// textures toujours NET (sprites 2D pixel art : portraits intro, etc.)
// — ignorées par l'option « LISSE » qui ne vise que le monde 3D
static std::set<GLuint> s_tex_near;
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF

static float aniso_max() {
    float v = 1.f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &v);
    return v;
}

// applique les params filtre courants a une texture liee
static void tex_apply(GLuint t) {
    glBindTexture(GL_TEXTURE_2D, t);
    GLint mag = s_tex_lin ? GL_LINEAR : GL_NEAREST;
    GLint min;
    if (_aniso > 0)
        min = s_tex_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
    else
        min = s_tex_lin ? GL_LINEAR : GL_NEAREST;
    if (_aniso > 0) {
        pglGenerateMipmap(GL_TEXTURE_2D);
        float a = (float)_aniso, m = aniso_max();
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
            a > m ? m : a);
    } else {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
}

uint32_t upload_tex(const Image& img) {
    GLuint t = 0;
    pglGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    // UV DS natifs : les texels sont stockés ligne par ligne depuis le haut
    // et les UVs du BMD référencent directement -> upload tel quel.
    pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, img.px.data());
    tex_apply(t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    s_tex.push_back(t);
    return t;
}

void tex_write(uint32_t tex, const Image& img) {
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.w, img.h, GL_RGBA,
        GL_UNSIGNED_BYTE, img.px.data());
}

// sprites 2D pixel art : MAG toujours NEAREST (net en gros plan, « LISSE »
// ne concerne que le monde 3D) ; MIN linéaire+mipmap = doux quand minifié
static void near_apply(GLuint t) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (_aniso > 0) {
        pglGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
}

uint32_t upload_tex_nearest(const Image& img) {
    uint32_t t = upload_tex(img);
    s_tex_near.insert(t);
    near_apply(t);
    return t;
}



void set_tex_filter(int linear) {
    s_tex_lin = linear;
    for (auto t : s_tex)
        if (!s_tex_near.count(t)) tex_apply(t);
    for (auto t : s_tex_near) near_apply(t);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void set_aniso(int level) {
    _aniso = 0; (void)level;             // aniso/mipmaps retirés (rendu flou)
    for (auto t : s_tex) {
        if (s_tex_near.count(t)) near_apply(t);
        else tex_apply(t);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void free_mesh(GpuMesh& g) {
    if (g.vao) pglDeleteVertexArrays(1, &g.vao);
    if (g.vbo) pglDeleteBuffers(1, &g.vbo);
    g.vao = g.vbo = 0; g.nverts = 0;
}

void free_tex(uint32_t t) {
    if (!t) return;
    glDeleteTextures(1, &t);
    s_tex_near.erase(t);
    s_tex.erase(std::remove(s_tex.begin(), s_tex.end(), t), s_tex.end());
}

} // namespace rnd

// ── Mat4 ─────────────────────────────────────────────────────────────────────
Mat4 Mat4::ident() {
    Mat4 m; memset(m.m, 0, sizeof m.m);
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.f;
    return m;
}

Mat4 Mat4::perspective(float fov, float asp, float zn, float zf) {
    Mat4 m; memset(m.m, 0, sizeof m.m);
    float f = 1.f / tanf(fov * 0.5f);
    m.m[0] = f / asp;
    m.m[5] = f;
    m.m[10] = (zf + zn) / (zn - zf);
    m.m[11] = -1.f;
    m.m[14] = 2.f * zf * zn / (zn - zf);
    return m;
}

Mat4 Mat4::ortho(float t, float asp, float zn, float zf) {
    // ortho DS : top = tan(fov_idx) * distance ; right = top * asp (camera.c)
    Mat4 m; memset(m.m, 0, sizeof m.m);
    float r = t * asp;
    m.m[0] = 1.f / r;  m.m[5] = 1.f / t;
    m.m[10] = -2.f / (zf - zn); m.m[14] = -(zf + zn) / (zf - zn);
    m.m[15] = 1.f;
    return m;
}

Mat4 Mat4::look_at(const Vec3& eye, const Vec3& tgt, const Vec3& up) {
    Vec3 f = (tgt - eye).normalized();
    Vec3 s = f.cross(up).normalized();
    Vec3 u = s.cross(f);
    Mat4 m = ident();
    m.m[0] = s.x; m.m[4] = s.y; m.m[8]  = s.z;
    m.m[1] = u.x; m.m[5] = u.y; m.m[9]  = u.z;
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
    m.m[12] = -s.dot(eye); m.m[13] = -u.dot(eye); m.m[14] = f.dot(eye);
    return m;
}

Mat4 Mat4::mul(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += m[k * 4 + rw] * o.m[c * 4 + k];
            r.m[c * 4 + rw] = v;
        }
    return r;
}
