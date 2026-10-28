// render.cpp — GL3 loader minimal + pipeline mesh/texture.
#include "render.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>
#include <cstring>

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
    _uSpec = -1, _uEmi = -1, _uLit = -1, _uFId = -1;
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
    LOAD(Uniform1f) LOAD(Uniform4f) LOAD(Uniform3fv) LOAD(GenTextures) LOAD(ActiveTexture)
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
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
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
uniform sampler2D uTex;
uniform int uUseTex;
uniform vec4 uTint;
uniform float uAlpha;
uniform float uPolyA;
uniform int uUVdbg;
uniform vec4 uFieldId;   // 2e cible DS : r = id polygone (0-63), a = flag brouillard
layout(location=1) out vec4 oField;
out vec4 frag;
void main() {
    oField = uFieldId;
    if (uUVdbg == 1) { frag = vec4(fract(vUV), 0.0, 1.0); return; }
    if (uUVdbg == 2) { frag = texture(uTex, vUV); return; }
    if (uUVdbg == 3) { frag = uTint; return; }
    vec4 c = vCol * uTint;
    if (uUseTex != 0) c *= texture(uTex, vUV);
    if (c.a < uAlpha) discard;
    frag = vec4(c.rgb, c.a * uPolyA);
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

void set_msaa(int n) { _msaa_n = n; _msaa_dirty = true; }
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

// ── composite DS : scène -> FBO (couleur + champ id/brouillard + profondeur)
//    puis passe plein-écran : edge marking (contour 1px DS, blend AA 17/32)
static int _comp = 0, _cindoor = 0;
static GLuint _cfbo = 0, _ctex = 0, _ftex = 0, _dtex = 0;
static GLuint _cprog = 0, _cvao = 0;
static int _cw = 0, _ch = 0;
static GLint _cuStep = -1, _cuEdge = -1, _cuCol = -1, _cuFld = -1, _cuDep = -1;

void set_composite(int on) { _comp = on; }
void set_edge_indoor(int in) { _cindoor = in; }

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
out vec4 frag;
int fid(vec4 f) { return int(f.r * 63.0 + 0.5); }
void main() {
    vec4 f0 = texture(tField, vUv);
    float d0 = texture(tDepth, vUv).r;
    frag = texture(tColor, vUv);
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
    auto tex = [&](GLenum fmt, GLuint* o) {
        pglGenTextures(1, o); pglActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *o);
        pglTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, _cw, _ch, 0,
            fmt == GL_DEPTH_COMPONENT24 ? GL_DEPTH_COMPONENT : GL_RGBA,
            GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    tex(GL_RGBA8, &_ctex); tex(GL_RGBA8, &_ftex); tex(GL_DEPTH_COMPONENT24, &_dtex);
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

// passe composite : FBO scène -> cible de sortie (MSAA ou écran)
void composite(int w, int h) {
    if (!_comp || !_cfbo) return;
    glViewport(0, 0, _msaa_w, _msaa_h);
    pglBindFramebuffer(GL_FRAMEBUFFER, _msaa_fbo ? _msaa_fbo : 0);
    glDisable(GL_DEPTH_TEST);
    pglUseProgram(_cprog);
    pglUniform2f(_cuStep, 1.f / 256.f, 1.f / 192.f); // pas d'un pixel DS
    // contours DS : extérieur gris foncé RGB(4,4,4) ; intérieur id 0 = noir
    const float dg = (4 * 8 + 1) / 255.f;           // rgb5(4) -> 0..1
    float ec[24];
    for (int i = 0; i < 8; i++) {
        bool blk = _cindoor && i == 0;
        ec[i*3] = ec[i*3+1] = ec[i*3+2] = blk ? 0.f : dg;
    }
    pglUniform3fv(_cuEdge, 8, ec);
    pglActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, _ctex);
    pglActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, _ftex);
    pglActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, _dtex);
    pglUniform1i(_cuCol, 0); pglUniform1i(_cuFld, 1); pglUniform1i(_cuDep, 2);
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

void draw(const GpuMesh& g, const Mat4& mvp) {
    if (!g.vao || g.nverts == 0) return;
    Mat4 ident = Mat4::ident();
    pglUniformMatrix4fv(_uMVP, 1, GL_FALSE, mvp.m);
    pglUniformMatrix4fv(_uModel, 1, GL_FALSE, ident.m);
    pglUniform4f(_uTint, g.tint.r, g.tint.g, g.tint.b, g.tint.a);
    pglUniform1i(_uUseTex, g.tex ? 1 : 0);
    pglUniform1f(_uAlpha, g.alpha_cut ? 0.4f : 0.0f);
    pglUniform1f(_uPolyA, g.blend);
    pglUniform1i(_uLit, g.lit);
    // 2e cible : id de polygone DS ; alpha 0 si translucide (la DS garde
    // l'id du dessous et AND le flag brouillard — approx : on l'écrit quand même)
    pglUniform4f(_uFId, g.poly_id / 63.f, 0.f, 0.f,
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

void set_tex_filter(int linear) {
    s_tex_lin = linear;
    for (auto t : s_tex) tex_apply(t);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void set_aniso(int level) {
    _aniso = level;
    for (auto t : s_tex) tex_apply(t);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void free_mesh(GpuMesh& g) {
    if (g.vao) pglDeleteVertexArrays(1, &g.vao);
    if (g.vbo) pglDeleteBuffers(1, &g.vbo);
    g.vao = g.vbo = 0; g.nverts = 0;
}

void free_tex(uint32_t t) { if (t) glDeleteTextures(1, &t); }

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
