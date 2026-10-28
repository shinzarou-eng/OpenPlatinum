// bmd.h — décodeur NSBMD/BMD0 : meshes GPU DS groupés par matériau,
// textures TEX0/BTX0. Port de scripts/bmd.gd.
#pragma once
#include "common.h"
#include <unordered_map>
#include <memory>

namespace bmd {

struct Vert {
    Vec3 p, n;
    RGBAf c;
    Vec2 uv;
};
struct Prim { int prim = 0; std::vector<Vert> v; };
struct MatDef {
    std::string name;
    uint32_t dif = 0, poly = 0, tex = 0;
    uint16_t tw = 1, th = 1;
    std::string tex_name, pal_name;
};
struct MeshGroup {
    int mat = -1;
    int node = -1;                   // nœud courant à l'émission (-1 inconnu, -2 skin)
    M4  xf;                          // matrice courante au repos (sommets déjà cuits)
    MatDef matdef;
    std::vector<Prim> prims;
};
struct TexEnt {
    int w = 0, h = 0, fmt = 0, c0t = 0;
    Bytes d;                         // copie du fichier (les offsets y réfèrent)
    int t1 = 0, c1 = 0, c2 = 0, pal_blk = 0;
    std::unordered_map<std::string, int> pals;   // nom -> offset<<3
    std::unordered_map<std::string, std::shared_ptr<Image>> imgs;
    std::shared_ptr<Image> img;
};
struct Model {
    std::vector<MeshGroup> meshes;
    std::vector<MatDef> mats;
    std::unordered_map<std::string, TexEnt> textures;
    // de quoi rejouer les commandes de rendu avec des nœuds animés (NSBCA)
    Bytes raw;
    size_t rc_off = 0;
    float up_s = 1.f, down_s = 1.f;
    std::vector<M4> bones, invb;     // locales de repos des nœuds, inv-bind skin
};

struct NL { int off = 0; size_t pos = 0; std::string name; };

Model parse_bmd(const Bytes& d);
std::vector<NL> namelist(const Bytes& d, size_t off);   // dictionnaire NNS
// clé d'un mesh group : (matériau, nœud)
inline int group_key(int mat, int node) { return mat * 1024 + node + 2; }
// matrices courantes à l'émission de chaque group, commandes rejouées avec
// `bones` à la place des locales de repos (clé group_key)
std::unordered_map<int, M4> replay_xf(const Model& m, const std::vector<M4>& bones);

// ── NSBCA (BCA0) : animation d'articulations (portes, hélices…) ──────────────
// Fichier = bloc 'JNT0' + dict ; on lit l'anim 0 (une par fichier dans bm_anime).
struct JntAnim {
    Bytes d;                            // fichier BCA0 complet
    size_t base = 0;                    // offset du NNSG3dResJntAnm dans d
    int nframe = 0;
    uint32_t flag = 0;                  // bit0 interpolation, bit1 end->start
    size_t rot3 = 0, rot5 = 0;          // offsets absolus des tables de rotations
    std::vector<int> nodes;             // dataIdx -> index de nœud du modèle
    std::vector<uint32_t> tofs;         // dataIdx -> offset du SRTTag (depuis base)
    bool ok = false;
};
JntAnim parse_bca(const Bytes& d);
// matrice locale animée du nœud `node` à la frame entière (clampée) ;
// `rest` = locale de repos du modèle pour les flags BASE_*.
M4 bca_node(const JntAnim& a, int node, int frame, const M4& rest);
std::unordered_map<std::string, TexEnt> parse_tex0(const Bytes& d, size_t off,
    std::vector<std::string>* order = nullptr);
std::shared_ptr<Image> tex_image(TexEnt& ent, const std::string& pal_name,
    bool idx0 = false);
std::shared_ptr<Image> decode_tex(const TexEnt& e, int pal_off, bool idx0 = false);
// texels d'une source, palette d'une autre (fldtanime : frames decodees avec
// le format et la palette de la texture de base, comme le fait la DS en VRAM)
std::shared_ptr<Image> decode_tex_raw(const Bytes& td, int t1, int c1, int c2,
    int fmt, int w, int h, int c0t, const Bytes& pd, int pal_blk, int pal_off,
    bool idx0 = false);

// helpers exposés (réutilisés par world pour les matériaux fallback)
RGBAf rgb555(uint32_t v);

} // namespace bmd
