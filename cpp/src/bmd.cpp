// bmd.cpp — NSBMD/BMD0 : NameList, BoneMatrix, render commands,
// évaluation des commandes GPU DS, TEX0/BTX0. Port fidèle de bmd.gd.
#include "bmd.h"
#include <cstdio>
#include <functional>
#include <unordered_map>

namespace bmd {

static float s10(uint32_t v) {
    v &= 0x3FF;
    return (float)(v >= 0x200 ? (int)v - 0x400 : (int)v);
}

RGBAf rgb555(uint32_t v) { return ::rgb555(v); }

// ── NameList ─────────────────────────────────────────────────────────────────
// -> [{off, pos, name}] ; off = valeur u32 de l'entrée (relatif à la liste
// englobante), pos = adresse de l'entrée (structs inline ex. TEX0).
std::vector<NL> namelist(const Bytes& d, size_t off) {
    std::vector<NL> out;
    if (off + 8 > d.size()) return out;
    int cnt = u8(d, off + 1);
    int sub = u16(d, off + 4);
    size_t p = off + 4 + sub + cnt * 4;
    if (p + 4 > d.size()) return out;
    int esz = u16(d, p);
    p += 4;
    for (int i = 0; i < cnt; i++) {
        NL e;
        e.off = (int)u32(d, p + i * esz);
        e.pos = p + i * esz;
        e.name = ascii(d, p + cnt * esz + i * 16, 16);
        out.push_back(e);
    }
    return out;
}

// pairings matière -> tex/pal : éléments {u16 io, u8 cnt, pad}, indices
// lus à base+io+j. Port de _namelist_idx.
static std::vector<std::pair<std::vector<int>, std::string>>
namelist_idx(const Bytes& d, size_t off, size_t base) {
    std::vector<std::pair<std::vector<int>, std::string>> out;
    if (off + 8 > d.size()) return out;
    int cnt = u8(d, off + 1);
    int sub = u16(d, off + 4);
    size_t p = off + 4 + sub + cnt * 4;
    if (p + 4 > d.size()) return out;
    int esz = u16(d, p);
    p += 4;
    for (int i = 0; i < cnt; i++) {
        size_t eoff = p + i * esz;
        if (eoff + 4 > d.size()) break;
        int io_ = u16(d, eoff);
        int c2 = u8(d, eoff + 2);
        std::vector<int> idxs;
        for (int j = 0; j < c2; j++)
            if (base + io_ + j < d.size())
                idxs.push_back(u8(d, base + io_ + j));
        out.push_back({idxs, ascii(d, p + cnt * esz + i * 16, 16)});
    }
    return out;
}

// ── BoneMatrix ────────────────────────────────────────────────────────────────
static void pivot(int form, float a, float b, float out[9]) {
    float c = sqrtf(std::max(0.f, 1.f - a * a - b * b));
    int idx = form / 3;
    for (int i = 0; i < 9; i++) out[i] = 0.f;
    float col[3] = {a, b, c};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            float v = 0.f;
            if (j == idx) v = col[i];
            else if (i == ((form % 3) + j) % 3 && j != idx)
                v = (i + j) % 2 == 0 ? 1.f : -1.f;
            out[j * 3 + i] = v;
        }
}

static M4 bone_matrix(const Bytes& d, size_t off) {
    int flags = u16(d, off);
    int t = flags & 1, rm = (flags >> 1) & 1, s = (flags >> 2) & 1;
    int rp = (flags >> 3) & 1;
    size_t p = off + 2;
    float m0 = fx16(s16(d, p)); p += 2;
    Vec3 trans{0, 0, 0};
    if (t == 0) {
        trans = {fx32(s32(d, p)), fx32(s32(d, p + 4)), fx32(s32(d, p + 8))};
        p += 12;
    }
    Vec3 rot[3] = {{1,0,0},{0,1,0},{0,0,1}};
    if (rp == 1) {
        float a = fx16(s16(d, p)), b = fx16(s16(d, p + 2)); p += 4;
        int form = (flags >> 4) & 0xF;
        float pv[9]; pivot(form, a, b, pv);
        float n1 = ((flags >> 8) & 1) ? -1.f : 1.f;
        float nc = ((flags >> 9) & 1) ? -1.f : 1.f;
        float nd = ((flags >> 10) & 1) ? -1.f : 1.f;
        rot[0] = Vec3(pv[0], pv[1], pv[2]) * n1;
        rot[1] = Vec3(pv[3], pv[4], pv[5]) * nc;
        rot[2] = Vec3(pv[6], pv[7], pv[8]) * nd;
    } else if (rm == 0) {
        float ms[8];
        for (int i = 0; i < 8; i++) ms[i] = fx16(s16(d, p + i * 2));
        p += 16;
        rot[0] = {m0, ms[0], ms[1]};
        rot[1] = {ms[2], ms[3], ms[4]};
        rot[2] = {ms[5], ms[6], ms[7]};
    }
    Vec3 sc{1, 1, 1};
    if (s == 0)
        sc = {fx32(s32(d, p)), fx32(s32(d, p + 4)), fx32(s32(d, p + 8))};
    return M4::trs(trans, rot, sc);
}

// ── évaluation mesh (commandes GPU DS) ────────────────────────────────────────
static std::vector<Prim> eval_mesh(const Bytes& d, const M4& base,
        std::unordered_map<int, M4>& stack) {
    std::vector<Prim> prims;
    int cur_prim = -1;
    std::vector<Vert> cur;
    Vec3 pos{0,0,0}, nrm{0,1,0};
    RGBAf col{1,1,1,1};
    Vec2 uv{0,0};
    M4 mtx = base;
    std::vector<M4> pstack;
    size_t i = 0, n = d.size();
    auto rd_m43 = [](const Bytes& d, size_t p) {
        M4 m;
        m.x = {fx32(s32(d, p)), fx32(s32(d, p + 16)), fx32(s32(d, p + 32))};
        m.y = {fx32(s32(d, p + 4)), fx32(s32(d, p + 20)), fx32(s32(d, p + 36))};
        m.z = {fx32(s32(d, p + 8)), fx32(s32(d, p + 24)), fx32(s32(d, p + 40))};
        m.w = {fx32(s32(d, p + 12)), fx32(s32(d, p + 28)), fx32(s32(d, p + 44))};
        return m;
    };
    auto vtx = [&]() {
        cur.push_back({mtx.xform(pos), mtx.xform_n(nrm), col, uv});
    };
    while (i + 4 <= n) {
        uint32_t ops = u32(d, i); i += 4;
        for (int k = 0; k < 4; k++) {
            int op = (ops >> (k * 8)) & 0xFF;
            if (op == 0) continue;
            bool stop = false, isv = false;
            switch (op) {
            case 0x10: i += 4; break;                      // MTX_MODE
            case 0x11: pstack.push_back(mtx); break;       // MTX_PUSH
            case 0x12: {                                   // MTX_POP : dépile N+1 matrices
                int cnt = (int)(u32(d, i) & 0x3F) + 1; i += 4;
                while (cnt-- > 0 && !pstack.empty()) {
                    mtx = pstack.back(); pstack.pop_back();
                }
            } break;
            case 0x13: {                                   // MTX_STORE slot
                stack[(int)(u32(d, i) & 0x1F)] = mtx; i += 4;
            } break;
            case 0x14: {                                   // MTX_RESTORE
                uint32_t slot = u32(d, i); i += 4;
                auto it = stack.find((int)slot);
                mtx = it != stack.end() ? it->second : base;
            } break;
            case 0x15: mtx = M4::ident(); break;
            case 0x16: mtx = rd_m43(d, i); i += 64; break;  // MTX_LOAD_4x4
            case 0x17: mtx = rd_m43(d, i); i += 48; break;  // MTX_LOAD_4x3
            case 0x18: mtx = mtx.mul(rd_m43(d, i)); i += 64; break; // MTX_MULT_4x4
            case 0x19: mtx = mtx.mul(rd_m43(d, i)); i += 48; break; // MTX_MULT_4x3
            case 0x1A: {                                   // MTX_MULT_3x3
                M4 mm;
                mm.x = {fx32(s32(d, i)), fx32(s32(d, i + 12)), fx32(s32(d, i + 24))};
                mm.y = {fx32(s32(d, i + 4)), fx32(s32(d, i + 16)), fx32(s32(d, i + 28))};
                mm.z = {fx32(s32(d, i + 8)), fx32(s32(d, i + 20)), fx32(s32(d, i + 32))};
                i += 36;
                mtx = mtx.mul(mm);
            } break;
            case 0x1B: {                                   // MTX_SCALE
                Vec3 sc{fx32(s32(d, i)), fx32(s32(d, i + 4)), fx32(s32(d, i + 8))};
                i += 12;
                M4 mm; mm.x = {sc.x, 0, 0}; mm.y = {0, sc.y, 0}; mm.z = {0, 0, sc.z};
                mtx = mtx.mul(mm);
            } break;
            case 0x1C: {                                   // MTX_TRANS
                M4 mm; mm.w = {fx32(s32(d, i)), fx32(s32(d, i + 4)), fx32(s32(d, i + 8))};
                i += 12;
                mtx = mtx.mul(mm);
            } break;
            case 0x20: col = rgb555f(u32(d, i) & 0x7FFF); i += 4; break;
            case 0x21: {                                   // NORMAL 3xs10
                uint32_t nv = u32(d, i); i += 4;
                nrm = {s10(nv) / 511.f, s10(nv >> 10) / 511.f, s10(nv >> 20) / 511.f};
            } break;
            case 0x22:
                uv = {s16(d, i) / 16.f, s16(d, i + 2) / 16.f}; i += 4; break;
            case 0x23:
                pos = Vec3((float)s16(d, i), (float)s16(d, i + 2),
                    (float)s16(d, i + 4)) * (1.f / 4096.f);
                i += 8; isv = true; break;
            case 0x24: {
                uint32_t pv = u32(d, i); i += 4;
                pos = Vec3(s10(pv), s10(pv >> 10), s10(pv >> 20)) * (1.f / 64.f);
                isv = true;
            } break;
            case 0x25:
                pos.x = s16(d, i) / 4096.f; pos.y = s16(d, i + 2) / 4096.f;
                i += 4; isv = true; break;
            case 0x26:
                pos.x = s16(d, i) / 4096.f; pos.z = s16(d, i + 2) / 4096.f;
                i += 4; isv = true; break;
            case 0x27:
                pos.y = s16(d, i) / 4096.f; pos.z = s16(d, i + 2) / 4096.f;
                i += 4; isv = true; break;
            case 0x28: {
                uint32_t dv = u32(d, i); i += 4;
                pos += Vec3(s10(dv), s10(dv >> 10), s10(dv >> 20)) * (1.f / 4096.f);
                isv = true;
            } break;
            case 0x29: case 0x2A: case 0x2B:
            case 0x30: case 0x31: case 0x32: case 0x33:
                i += 4; break;
            case 0x34: i += 128; break;
            case 0x40:
                cur_prim = u32(d, i) & 3; i += 4;
                cur.clear(); break;
            case 0x41:
                prims.push_back({cur_prim, cur}); cur.clear(); break;
            case 0x50: case 0x60: case 0x72: case 0x80:
                i += 4; break;
            case 0x70: i += 24; break;
            case 0x71: i += 8; break;
            default:
                stop = true; break;
            }
            if (isv) vtx();
            if (stop) { i = n; break; }
        }
    }
    return prims;
}

// ── parse modèle ──────────────────────────────────────────────────────────────
// boucle des render commands MDL0 (SBC) : matrices courantes + nœud courant.
// on_mesh(mat, node, mesh, cur, stack) à chaque 0x05 (dessin d'un mesh).
using MeshFn = std::function<void(int, int, int, const M4&,
    std::unordered_map<int, M4>&)>;
static void run_rc(const Bytes& d, size_t rc, const std::vector<M4>& bones,
        const std::vector<M4>& invb, float up_s, float down_s, const MeshFn& on_mesh) {
    M4 cur = M4::ident();
    std::unordered_map<int, M4> stack;
    std::unordered_map<int, int> snode;        // slot de pile -> nœud stocké
    int cur_mat = -1, cur_node = -1;
    auto restore = [&](int slot) {
        auto it = stack.find(slot);
        cur = it != stack.end() ? it->second : M4::ident();
        auto in = snode.find(slot);
        cur_node = in != snode.end() ? in->second : -1;
    };
    for (int guard = 0; guard < 10000; guard++) {
        if (rc >= d.size()) break;
        int op = u8(d, rc); rc += 1;
        switch (op) {
        case 0x00: case 0x40: case 0x80: continue;
        case 0x01: guard = 10000; break;
        case 0x02: case 0x0C: case 0x0D: rc += 2; break;
        case 0x03: restore(u8(d, rc)); rc += 1; break;
        case 0x04: case 0x24: case 0x44: cur_mat = u8(d, rc); rc += 1; break;
        case 0x05: {
            int mi = u8(d, rc); rc += 1;
            if (cur_mat >= 0) on_mesh(cur_mat, cur_node, mi, cur, stack);
        } break;
        case 0x06: case 0x26: case 0x46: case 0x66: {
            // 3 params (nœud, parent, unk) + load si 0x40 + store si 0x20
            int bidx = u8(d, rc); rc += 3;
            if (op & 0x40) { restore(u8(d, rc)); rc += 1; }
            if (bidx < (int)bones.size()) cur = cur.mul(bones[bidx]);
            cur_node = bidx;
            if (op & 0x20) { stack[u8(d, rc)] = cur; snode[u8(d, rc)] = bidx; rc += 1; }
        } break;
        case 0x07: case 0x08: rc += 1; break;
        case 0x47: rc += 2; break;
        case 0x09: {
            int si = u8(d, rc), nt = u8(d, rc + 1); rc += 2;
            M4 m; m.x = m.y = m.z = Vec3{0, 0, 0};
            for (int t = 0; t < nt; t++) {
                int stk = u8(d, rc), ib = u8(d, rc + 1);
                float wv = u8(d, rc + 2) / 256.f;
                rc += 3;
                M4 term;
                auto it = stack.find(stk);
                if (it != stack.end()) term = it->second;
                if (ib < (int)invb.size()) term = term.mul(invb[ib]);
                m = m.add(term.mul_s(wv));
            }
            stack[si] = m; snode[si] = -2;     // skin : pas animé par nœud
        } break;
        case 0x0B: cur = cur.scale_basis(up_s); break;
        case 0x2B: cur = cur.scale_basis(down_s); break;
        default: guard = 10000; break;
        }
    }
}

Model parse_bmd(const Bytes& d) {
    Model out;
    if (d.size() < 0x20 || ascii(d, 0, 4) != "BMD0") return out;
    int nsub = u16(d, 0x0E);
    int mdl0 = -1, tex0 = -1;
    for (int i = 0; i < nsub; i++) {
        size_t o = u32(d, 0x10 + i * 4);
        std::string mg = ascii(d, o, 4);
        if (mg == "MDL0") mdl0 = (int)o;
        else if (mg == "TEX0") tex0 = (int)o;
    }
    if (mdl0 < 0) return out;
    if (tex0 >= 0) out.textures = parse_tex0(d, tex0);

    auto models = namelist(d, mdl0 + 8);
    if (models.empty()) return out;
    size_t mo = mdl0 + models[0].off;
    size_t rc_off = mo + u32(d, mo + 4);
    size_t mat_list = mo + u32(d, mo + 8);
    size_t mesh_list = mo + u32(d, mo + 12);
    size_t invb_off = mo + u32(d, mo + 16);
    int nbones = u8(d, mo + 23);
    float up_s = fx32(s32(d, mo + 28));
    float down_s = fx32(s32(d, mo + 32));

    std::vector<M4> bones;
    for (auto& b : namelist(d, mo + 64))
        bones.push_back(bone_matrix(d, mo + 64 + b.off));

    std::vector<M4> invb;
    size_t p = invb_off;
    for (int i = 0; i < nbones; i++) {
        if (p + 84 > d.size()) break;
        M4 m;
        m.x = {fx32(s32(d, p)), fx32(s32(d, p + 16)), fx32(s32(d, p + 32))};
        m.y = {fx32(s32(d, p + 4)), fx32(s32(d, p + 20)), fx32(s32(d, p + 36))};
        m.z = {fx32(s32(d, p + 8)), fx32(s32(d, p + 24)), fx32(s32(d, p + 40))};
        m.w = {fx32(s32(d, p + 12)), fx32(s32(d, p + 28)), fx32(s32(d, p + 44))};
        invb.push_back(m);
        p += 84;
    }

    int txp = u16(d, mat_list), plp = u16(d, mat_list + 2);
    auto mats = namelist(d, mat_list + 4);
    std::vector<MatDef> mat_objs;
    for (auto& mm : mats) {
        size_t mp = mat_list + mm.off;
        MatDef md;
        md.name = mm.name;
        md.dif = u32(d, mp + 4);
        md.poly = u32(d, mp + 12);
        md.tex = u32(d, mp + 20);
        md.tw = u16(d, mp + 32);
        md.th = u16(d, mp + 34);
        mat_objs.push_back(md);
    }
    if (txp > 0 && mat_list + txp + 8 < d.size())
        for (auto& pr : namelist_idx(d, mat_list + txp, mat_list))
            for (int mi : pr.first)
                if (mi < (int)mat_objs.size()) mat_objs[mi].tex_name = pr.second;
    if (plp > 0 && mat_list + plp + 8 < d.size())
        for (auto& pr : namelist_idx(d, mat_list + plp, mat_list))
            for (int mi : pr.first)
                if (mi < (int)mat_objs.size()) mat_objs[mi].pal_name = pr.second;

    auto mesh_objs = namelist(d, mesh_list);
    out.rc_off = rc_off; out.up_s = up_s; out.down_s = down_s;
    out.bones = bones; out.invb = invb;

    // render commands : groupés par (matériau, nœud) pour pouvoir animer un nœud
    struct G { int mat, node; M4 xf; std::vector<Prim> prims; };
    std::unordered_map<int, G> groups;
    run_rc(d, rc_off, bones, invb, up_s, down_s,
        [&](int mat, int node, int mi, const M4& cur,
            std::unordered_map<int, M4>& stack) {
            if (mi >= (int)mesh_objs.size()) return;
            size_t mo2 = mesh_list + mesh_objs[mi].off;
            size_t co = mo2 + u32(d, mo2 + 8);
            size_t cl = u32(d, mo2 + 12);
            auto pr = eval_mesh(slice(d, co, cl), cur, stack);
            auto& g = groups.try_emplace(group_key(mat, node),
                G{mat, node, cur, {}}).first->second;
            g.prims.insert(g.prims.end(), pr.begin(), pr.end());
        });

    for (auto& kv : groups) {
        MeshGroup mg;
        mg.mat = kv.second.mat;
        mg.node = kv.second.node;
        mg.xf = kv.second.xf;
        if (mg.mat < (int)mat_objs.size() && mg.mat >= 0)
            mg.matdef = mat_objs[mg.mat];
        mg.prims = std::move(kv.second.prims);
        out.meshes.push_back(std::move(mg));
    }
    out.mats = std::move(mat_objs);
    out.raw = d;
    return out;
}

std::unordered_map<int, M4> replay_xf(const Model& m, const std::vector<M4>& bones) {
    std::unordered_map<int, M4> out;
    if (m.raw.empty()) return out;
    run_rc(m.raw, m.rc_off, bones, m.invb, m.up_s, m.down_s,
        [&](int mat, int node, int, const M4& cur, std::unordered_map<int, M4>&) {
            out.try_emplace(group_key(mat, node), cur);
        });
    return out;
}

// ── TEX0 / BTX0 ───────────────────────────────────────────────────────────────
std::unordered_map<std::string, TexEnt> parse_tex0(const Bytes& d, size_t off,
        std::vector<std::string>* order) {
    std::unordered_map<std::string, TexEnt> out;
    std::string mg = ascii(d, off, 4);
    if (mg == "BTX0") {
        int nsub = u16(d, off + 0x0E);
        int found = -1;
        for (int i = 0; i < nsub; i++) {
            size_t o = off + u32(d, off + 0x10 + i * 4);
            if (ascii(d, o, 4) == "TEX0") { found = (int)o; break; }
        }
        if (found < 0) return out;
        off = found;
    } else if (mg != "TEX0") return out;

    size_t tex_list = off + u16(d, off + 0x0E);
    size_t tex_blk = off + u32(d, off + 0x14);
    size_t c1_blk = off + u32(d, off + 0x24);
    size_t c2_blk = off + u32(d, off + 0x28);
    size_t pal_blk = off + u32(d, off + 0x38);
    size_t pal_list = off + u32(d, off + 0x34);

    std::unordered_map<std::string, int> pals;
    for (auto& e : namelist(d, pal_list))
        pals[e.name] = u16(d, e.pos) << 3;

    for (auto& e : namelist(d, tex_list)) {
        uint32_t params = u32(d, e.pos);
        TexEnt ent;
        ent.w = 8 << ((params >> 20) & 7);
        ent.h = 8 << ((params >> 23) & 7);
        ent.fmt = (params >> 26) & 7;
        ent.c0t = (params >> 29) & 1;
        int toff = (params & 0xFFFF) << 3;
        ent.t1 = (int)(tex_blk + toff);
        ent.c1 = (int)(c1_blk + toff);
        ent.c2 = (int)(c2_blk + toff / 2);
        ent.pal_blk = (int)pal_blk;
        ent.d = d;
        ent.pals = pals;
        ent.img = decode_tex(ent, pals.empty() ? 0 : pals.begin()->second);
        if (ent.img) ent.imgs[""] = ent.img;
        out[e.name] = ent;
        if (order) order->push_back(e.name);
    }
    return out;
}

std::shared_ptr<Image> tex_image(TexEnt& ent, const std::string& pal_name,
        bool idx0) {
    if (pal_name.empty() || !ent.pals.count(pal_name))
        return idx0 ? decode_tex(ent, 0, true) : ent.img;
    std::string key = idx0 ? pal_name + "~0" : pal_name;
    auto it = ent.imgs.find(key);
    if (it != ent.imgs.end()) return it->second;
    auto img = decode_tex(ent, ent.pals[pal_name], idx0);
    ent.imgs[key] = img;
    return img;
}

static uint16_t pal_u16(const Bytes& d, int blk, int i) {
    return (blk + i * 2 + 1 < (int)d.size()) ? u16(d, blk + i * 2) : 0;
}

std::shared_ptr<Image> decode_tex(const TexEnt& e, int pal_off, bool idx0) {
    return decode_tex_raw(e.d, e.t1, e.c1, e.c2, e.fmt, e.w, e.h, e.c0t,
        e.d, e.pal_blk, pal_off, idx0);
}

std::shared_ptr<Image> decode_tex_raw(const Bytes& d, int t1, int c1, int c2,
        int fmt, int w, int h, int c0t, const Bytes& pd, int pal_blk,
        int pal_off, bool idx0) {
    if (fmt == 0 || w <= 0 || h <= 0) return nullptr;   // TEXIMAGE_NONE : poly non texturé
    bool t0 = c0t || idx0;     // idx0 : decal mode -> index 0 transparent
    auto img = std::make_shared<Image>();
    img->create(w, h);
    int npx = w * h;
    auto put = [&](int i, RGBAf c) {
        img->set_i(i, (uint8_t)std::min(255.f, c.r), (uint8_t)std::min(255.f, c.g),
            (uint8_t)std::min(255.f, c.b), (uint8_t)(c.a * 255));
    };
    switch (fmt) {
    case 2:   // 2bpp
        for (int i = 0; i < npx; i++) {
            int b = u8(d, t1 + i / 4);
            int u = (b >> ((i % 4) * 2)) & 3;
            RGBAf c = rgb555(pal_u16(pd, pal_blk + pal_off, u));
            c.a = (u == 0 && t0) ? 0.f : 1.f;
            put(i, c);
        } break;
    case 3:   // 4bpp
        for (int i = 0; i < npx; i++) {
            int b = u8(d, t1 + i / 2);
            int u = (i % 2) ? (b >> 4) & 0xF : b & 0xF;
            RGBAf c = rgb555(pal_u16(pd, pal_blk + pal_off, u));
            c.a = (u == 0 && t0) ? 0.f : 1.f;
            put(i, c);
        } break;
    case 4:   // 8bpp
        for (int i = 0; i < npx; i++) {
            int u = u8(d, t1 + i);
            RGBAf c = rgb555(pal_u16(pd, pal_blk + pal_off, u));
            c.a = (u == 0 && t0) ? 0.f : 1.f;
            put(i, c);
        } break;
    case 5: { // compressé 4x4
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int bi = (w / 4) * (y / 4) + (x / 4);
                uint32_t blk = u32(d, t1 + bi * 4);
                int ext = u16(d, c2 + bi * 2);
                int texel = (blk >> (2 * (4 * (y % 4) + (x % 4)))) & 3;
                int mode = ext >> 14;
                int pa = (ext & 0x3FFF) << 1;
                int base = pal_blk + pal_off + pa * 2;
                RGBAf cols[4] = {
                    rgb555(pal_u16(pd, base, 0)), rgb555(pal_u16(pd, base, 1)),
                    rgb555(pal_u16(pd, base, 2)), rgb555(pal_u16(pd, base, 3))};
                RGBAf c{0,0,0,0};
                switch (mode) {
                case 0: c = texel < 3 ? cols[texel] : RGBAf{0,0,0,0}; break;
                case 1:
                    if (texel < 2) c = cols[texel];
                    else if (texel == 2)
                        c = {(cols[0].r + cols[1].r) * .5f, (cols[0].g + cols[1].g) * .5f,
                             (cols[0].b + cols[1].b) * .5f, 1.f};
                    else c = {0,0,0,0};
                    break;
                case 2: c = cols[texel]; break;
                case 3:
                    if (texel == 0) c = cols[0];
                    else if (texel == 1) c = cols[1];
                    else if (texel == 2)
                        c = {(cols[1].r * 3 + cols[0].r * 5) / 8.f,
                             (cols[1].g * 3 + cols[0].g * 5) / 8.f,
                             (cols[1].b * 3 + cols[0].b * 5) / 8.f, 1.f};
                    else
                        c = {(cols[0].r * 3 + cols[1].r * 5) / 8.f,
                             (cols[0].g * 3 + cols[1].g * 5) / 8.f,
                             (cols[0].b * 3 + cols[1].b * 5) / 8.f, 1.f};
                    break;
                }
                put(y * w + x, c);
            }
    } break;
    case 7:   // direct rgb555
        for (int i = 0; i < npx; i++) {
            uint16_t v = u16(d, t1 + i * 2);
            RGBAf c = rgb555(v);
            c.a = (v & 0x8000) ? 1.f : 0.f;
            put(i, c);
        } break;
    case 1:   // a3i5
        for (int i = 0; i < npx; i++) {
            int x2 = u8(d, t1 + i);
            RGBAf c = rgb555(pal_u16(pd, pal_blk + pal_off, x2 & 0x1F));
            c.a = ((x2 >> 5) & 7) / 7.f;
            put(i, c);
        } break;
    case 6:   // a5i3
        for (int i = 0; i < npx; i++) {
            int x2 = u8(d, t1 + i);
            RGBAf c = rgb555(pal_u16(pd, pal_blk + pal_off, x2 & 7));
            c.a = (x2 >> 3) / 31.f;
            put(i, c);
        } break;
    default:  // fmt 0/6 autres : magenta
        for (int i = 0; i < npx; i++) img->set_i(i, 150, 50, 150, 255);
    }
    return img;
}

// ── NSBCA (BCA0) : animations d'articulations ────────────────────────────────
// NNSG3dResJntAnm : anmHeader(4) numFrame u16 numNode u16 flag u32
// ofsRot3 u32 ofsRot5 u32 puis u16 ofsTag[numNode]. Chaque tag : u32 dont
// bits[31:24]=index de nœud, flags IDENTITY/BASE/CONST par composante SRT,
// suivi des descripteurs {info u32, ofs u32} (offsets relatifs au JntAnm).
// Steps de sampling : 1/2/4 frames par élément, tableaux fx16 ou fx32,
// rotations compressées pivot (rot3) ou 5-valeurs (rot5, ligne 2 = cross).
namespace {

enum {  // NNSG3dJntAnmSRTTag
    JA_IDENTITY = 0x1,
    JA_ID_T = 0x2,  JA_BASE_T = 0x4,
    JA_C_TX = 0x8,  JA_C_TY = 0x10, JA_C_TZ = 0x20,
    JA_ID_R = 0x40, JA_BASE_R = 0x80, JA_C_R = 0x100,
    JA_ID_S = 0x200, JA_BASE_S = 0x400,
    JA_C_SX = 0x800, JA_C_SY = 0x1000, JA_C_SZ = 0x2000,
};

// échantillonne un scalaire animé (translation) à une frame entière.
// desc -> {info u32, ofsDonnées u32} ; données relatives à `base`.
float anm_scalar(const Bytes& d, size_t base, size_t desc, int frame) {
    uint32_t info = u32(d, desc);
    size_t arr = base + u32(d, desc + 4);
    bool f16 = (info & 0x20000000) != 0;
    int step = (int)(info >> 30);
    int lasti = (int)((info >> 16) & 0x1FFF);
    auto at = [&](int i) {
        return f16 ? fx16(s16(d, arr + i * 2)) : fx32(s32(d, arr + i * 4));
    };
    if (step == 0) return at(frame);                    // step 1
    if (step == 1) {                                    // step 2
        if (frame & 1) {
            if (frame > lasti) return at((lasti >> 1) + 1);
            int i = frame >> 1;
            return (at(i) + at(i + 1)) * 0.5f;
        }
        return at(frame >> 1);
    }                                                   // step 4
    if (frame & 3) {
        if (frame > lasti) return at((lasti >> 2) + (frame & 3));
        if (frame & 1) {
            int i, isub;                                // poids 3:1
            if (frame & 2) { isub = frame >> 2; i = isub + 1; }
            else { i = frame >> 2; isub = i + 1; }
            return (at(i) * 3.f + at(isub)) * 0.25f;
        }
        int i = frame >> 2;
        return (at(i) + at(i + 1)) * 0.5f;
    }
    return at(frame >> 2);
}

// idem pour les paires (scale, invScale) : on ne retient que le scale
float anm_scale(const Bytes& d, size_t base, size_t desc, int frame) {
    uint32_t info = u32(d, desc);
    size_t arr = base + u32(d, desc + 4);
    bool f16 = (info & 0x20000000) != 0;
    int step = (int)(info >> 30);
    int lasti = (int)((info >> 16) & 0x1FFF);
    auto at = [&](int i) {                              // paires interlacées
        return f16 ? fx16(s16(d, arr + i * 4)) : fx32(s32(d, arr + i * 8));
    };
    if (step == 0) return at(frame);
    if (step == 1) {
        if (frame & 1) {
            if (frame > lasti) return at((lasti >> 1) + 1);
            int i = frame >> 1;
            return (at(i) + at(i + 1)) * 0.5f;
        }
        return at(frame >> 1);
    }
    if (frame & 3) {
        if (frame > lasti) return at((lasti >> 2) + (frame & 3));
        if (frame & 1) {
            int i, isub;
            if (frame & 2) { isub = frame >> 2; i = isub + 1; }
            else { i = frame >> 2; isub = i + 1; }
            return (at(i) * 3.f + at(isub)) * 0.25f;
        }
        int i = frame >> 2;
        return (at(i) + at(i + 1)) * 0.5f;
    }
    return at(frame >> 2);
}

const int PIVOT_UTIL[9][4] = {
    {4, 5, 7, 8}, {3, 5, 6, 8}, {3, 4, 6, 7},
    {1, 2, 7, 8}, {0, 2, 6, 8}, {0, 1, 6, 7},
    {1, 2, 4, 5}, {0, 2, 3, 5}, {0, 1, 3, 4}
};

// décode une entrée de rotation indexée ; m = matrice 3x3 row-major DS.
// retourne vrai si la ligne 2 doit être recomposée par produit vectoriel.
bool rot_by_idx(const JntAnim& a, uint16_t ridx, float m[9]) {
    if (ridx & 0x8000) {                                // pivot : rot3
        size_t p = a.rot3 + (ridx & 0x7FFF) * 6;
        uint16_t inf = u16(a.d, p);
        float A = fx16(s16(a.d, p + 2)), B = fx16(s16(a.d, p + 4));
        int pv = inf & 0xF;
        for (int i = 0; i < 9; i++) m[i] = 0.f;
        m[pv] = (inf & 0x10) ? -1.f : 1.f;
        m[PIVOT_UTIL[pv][0]] = A;
        m[PIVOT_UTIL[pv][1]] = B;
        m[PIVOT_UTIL[pv][2]] = (inf & 0x20) ? -B : B;
        m[PIVOT_UTIL[pv][3]] = (inf & 0x40) ? -A : A;
        return false;
    }
    // rot5 : 5 fx16 -> 6 éléments ; _12 réparti sur les 3 bits de poids faible
    size_t p = a.rot5 + (ridx & 0x7FFF) * 10;
    int acc = u16(a.d, p + 8) & 7;                      // data[4]
    m[4] = fx16((int16_t)(s16(a.d, p + 8) >> 3));       // _11
    for (int i = 0; i < 4; i++) {
        acc = (acc << 3) | (u16(a.d, p + i * 2) & 7);
        static const int dst[4] = {0, 1, 2, 3};         // _00,_01,_02,_10
        m[dst[i]] = fx16((int16_t)(s16(a.d, p + i * 2) >> 3));
    }
    m[5] = fx16((int16_t)((acc << 19) >> 19));          // _12 : 13 bits signés
    return true;
}

Vec3 rot_cross(const float m[9]) {
    Vec3 r0{m[0], m[1], m[2]}, r1{m[3], m[4], m[5]};
    return r0.cross(r1);
}

// rotation complète (avec ligne 2) depuis un index de table
void rot_full(const JntAnim& a, uint16_t ridx, float m[9]) {
    if (rot_by_idx(a, ridx, m)) {
        Vec3 c = rot_cross(m);
        m[6] = c.x; m[7] = c.y; m[8] = c.z;
    }
}

// interpolation matricielle : lignes 0/1 normalisées, ligne 2 = cross si une
// des deux entrées est en rot5 (comme G3D_NORMALIZE_ROT_MTX de la DS)
void rot_lerp(const float A[9], const float B[9], float wa, float wb,
              bool cross, float m[9]) {
    Vec3 n;
    for (int i = 0; i < 6; i++) m[i] = A[i] * wa + B[i] * wb;
    n = Vec3{m[0], m[1], m[2]}.normalized();
    m[0] = n.x; m[1] = n.y; m[2] = n.z;
    n = Vec3{m[3], m[4], m[5]}.normalized();
    m[3] = n.x; m[4] = n.y; m[5] = n.z;
    if (cross) {
        Vec3 c = rot_cross(m);
        m[6] = c.x; m[7] = c.y; m[8] = c.z;
    } else {
        for (int i = 6; i < 9; i++) m[i] = A[i] * wa + B[i] * wb;
        n = Vec3{m[6], m[7], m[8]}.normalized();
        m[6] = n.x; m[7] = n.y; m[8] = n.z;
    }
}

void rot_sample(const JntAnim& a, size_t desc, int frame, float m[9]) {
    uint32_t info = u32(a.d, desc);
    size_t arr = a.base + u32(a.d, desc + 4);
    int step = (int)(info >> 30);
    int lasti = (int)((info >> 16) & 0x1FFF);
    auto idxat = [&](int i) { return u16(a.d, arr + i * 2); };
    auto two = [&](int i0, int i1, float wa, float wb) {
        float m0[9], m1[9];
        bool cross = rot_by_idx(a, idxat(i0), m0);
        cross |= rot_by_idx(a, idxat(i1), m1);
        rot_lerp(m0, m1, wa, wb, cross, m);
    };
    if (step == 0) { rot_full(a, idxat(frame), m); return; }
    if (step == 1) {
        if (frame & 1) {
            if (frame > lasti) rot_full(a, idxat((lasti >> 1) + 1), m);
            else two(frame >> 1, (frame >> 1) + 1, 0.5f, 0.5f);
        } else rot_full(a, idxat(frame >> 1), m);
        return;
    }
    if (frame & 3) {
        if (frame > lasti) rot_full(a, idxat((lasti >> 2) + (frame & 3)), m);
        else if (frame & 1) {
            if (frame & 2) two((frame >> 2) + 1, frame >> 2, 0.75f, 0.25f);
            else two(frame >> 2, (frame >> 2) + 1, 0.75f, 0.25f);
        } else two(frame >> 2, (frame >> 2) + 1, 0.5f, 0.5f);
        return;
    }
    rot_full(a, idxat(frame >> 2), m);
}

} // namespace

JntAnim parse_bca(const Bytes& d) {
    JntAnim a;
    if (d.size() < 0x20 || ascii(d, 0, 4) != "BCA0") return a;
    int nblk = u16(d, 0x0E);
    for (int i = 0; i < nblk; i++) {
        size_t o = u32(d, 0x10 + i * 4);
        if (o + 16 > d.size() || ascii(d, o, 4) != "JNT0") continue;
        // dict NNS à o+8 : {rev u8, n u8, size u16, dummy u16, ofsEntry u16}
        size_t dd = o + 8;
        int nent = u8(d, dd + 1);
        size_t ent = dd + u16(d, dd + 6);
        if (!nent || ent + 4 > d.size()) break;
        size_t jb = o + u32(d, ent + 4);                // offset rel. bloc JNT0
        if (jb + 0x14 > d.size() || u8(d, jb) != 'J') break;
        a.base = jb; a.d = d;
        a.nframe = u16(d, jb + 4);
        int nnode = u16(d, jb + 6);
        a.flag = u32(d, jb + 8);
        a.rot3 = jb + u32(d, jb + 0x0C);
        a.rot5 = jb + u32(d, jb + 0x10);
        for (int n = 0; n < nnode; n++) {
            uint32_t to = u16(d, jb + 0x14 + n * 2);
            a.tofs.push_back(to);
            a.nodes.push_back((int)(u32(d, jb + to) >> 24));
        }
        a.ok = a.nframe > 0;
        return a;
    }
    return a;
}

M4 bca_node(const JntAnim& a, int node, int frame, const M4& rest) {
    int di = -1;
    for (size_t i = 0; i < a.nodes.size(); i++)
        if (a.nodes[i] == node) { di = (int)i; break; }
    if (di < 0) return rest;
    uint32_t tag = u32(a.d, a.base + a.tofs[di]);
    if (tag & JA_IDENTITY) return M4::ident();
    if (frame < 0) frame = 0;
    if (frame >= a.nframe) frame = a.nframe - 1;
    size_t p = a.base + a.tofs[di] + 4;

    Vec3 t{0, 0, 0};                                    // translation
    if (tag & JA_BASE_T) t = rest.w;
    else if (!(tag & JA_ID_T)) {
        if (tag & JA_C_TX) { t.x = fx32(s32(a.d, p)); p += 4; }
        else { t.x = anm_scalar(a.d, a.base, p, frame); p += 8; }
        if (tag & JA_C_TY) { t.y = fx32(s32(a.d, p)); p += 4; }
        else { t.y = anm_scalar(a.d, a.base, p, frame); p += 8; }
        if (tag & JA_C_TZ) { t.z = fx32(s32(a.d, p)); p += 4; }
        else { t.z = anm_scalar(a.d, a.base, p, frame); p += 8; }
    }

    float r[9] = {1,0,0, 0,1,0, 0,0,1};                 // rotation (row-major)
    if (tag & JA_BASE_R) {                              // repos : base orthonormée
        Vec3 sx{rest.x.len(), rest.y.len(), rest.z.len()};
        Vec3 c0 = sx.x ? rest.x * (1.f / sx.x) : Vec3{1,0,0};
        Vec3 c1 = sx.y ? rest.y * (1.f / sx.y) : Vec3{0,1,0};
        Vec3 c2 = sx.z ? rest.z * (1.f / sx.z) : Vec3{0,0,1};
        r[0]=c0.x; r[3]=c0.y; r[6]=c0.z;
        r[1]=c1.x; r[4]=c1.y; r[7]=c1.z;
        r[2]=c2.x; r[5]=c2.y; r[8]=c2.z;
    } else if (!(tag & JA_ID_R)) {
        if (tag & JA_C_R) { rot_full(a, u16(a.d, p), r); p += 4; }
        else { rot_sample(a, p, frame, r); p += 8; }
    }

    Vec3 s{1, 1, 1};                                    // scale
    if (tag & JA_BASE_S) s = {rest.x.len(), rest.y.len(), rest.z.len()};
    else if (!(tag & JA_ID_S)) {
        if (tag & JA_C_SX) { s.x = fx32(s32(a.d, p)); p += 8; }
        else { s.x = anm_scale(a.d, a.base, p, frame); p += 8; }
        if (tag & JA_C_SY) { s.y = fx32(s32(a.d, p)); p += 8; }
        else { s.y = anm_scale(a.d, a.base, p, frame); p += 8; }
        if (tag & JA_C_SZ) { s.z = fx32(s32(a.d, p)); p += 8; }
        else { s.z = anm_scale(a.d, a.base, p, frame); p += 8; }
    }

    Vec3 cols[3] = {                                    // row-major -> colonnes M4
        {r[0], r[3], r[6]}, {r[1], r[4], r[7]}, {r[2], r[5], r[8]}
    };
    return M4::trs(t, cols, s);
}

} // namespace bmd
