// land.cpp
#include "land.h"

Land Land::parse(const Bytes& d) {
    Land L;
    if (d.size() < 0x810) return L;
    uint32_t attr_sz = u32(d, 0), props_sz = u32(d, 4);
    uint32_t model_sz = u32(d, 8), bdhc_sz = u32(d, 12);
    for (int i = 0; i < 1024; i++)
        L.attrs[i] = u16(d, 0x10 + i * 2);
    size_t base = 0x10 + attr_sz;
    for (uint32_t i = 0; i < props_sz / 48; i++) {
        size_t o = base + i * 48;
        LandProp p;
        p.model = u32(d, o);
        p.pos = {fx32(s32(d, o + 4)), fx32(s32(d, o + 8)), fx32(s32(d, o + 12))};
        p.rot = {(float)s32(d, o + 16), (float)s32(d, o + 20), (float)s32(d, o + 24)};
        p.scale = {fx32(s32(d, o + 28)), fx32(s32(d, o + 32)), fx32(s32(d, o + 36))};
        L.props.push_back(p);
    }
    base += props_sz;
    L.model = slice(d, base, model_sz);
    base += model_sz;
    if (bdhc_sz > 0) L.bdhc = slice(d, base, bdhc_sz);
    // BDHC (overlay005/bdhc.c) : magic "BDHC" + 6 compteurs u16, puis
    // points(fx32 x,z) / normals(VecFx32) / constants(fx32) /
    // plates(4×u16) / strips / accessList — strips/accès ignorés,
    // on teste les plaques directement.
    const Bytes& b = L.bdhc;
    if (b.size() >= 16 && b[0] == 'B' && b[1] == 'D' && b[2] == 'H'
        && b[3] == 'C') {
        int pc = u16(b, 4), nc = u16(b, 6), cc = u16(b, 8),
            plc = u16(b, 10);
        size_t o = 16;
        size_t need = o + pc * 8 + nc * 12 + cc * 4 + plc * 8;
        if (need <= b.size()) {
            std::vector<Vec2> pts(pc);
            for (int i = 0; i < pc; i++, o += 8)
                pts[i] = {fx32(s32(b, o)), fx32(s32(b, o + 4))};
            std::vector<Vec3> nrms(nc);
            for (int i = 0; i < nc; i++, o += 12)
                nrms[i] = {fx32(s32(b, o)), fx32(s32(b, o + 4)),
                    fx32(s32(b, o + 8))};
            std::vector<float> cst(cc);
            for (int i = 0; i < cc; i++, o += 4)
                cst[i] = fx32(s32(b, o));
            for (int i = 0; i < plc; i++, o += 8) {
                const Vec2& a = pts[u16(b, o)],& c2 = pts[u16(b, o + 2)];
                int ni = u16(b, o + 4), ci = u16(b, o + 6);
                BDHCPlate pl;
                pl.x0 = std::min(a.x, c2.x); pl.x1 = std::max(a.x, c2.x);
                pl.z0 = std::min(a.y, c2.y); pl.z1 = std::max(a.y, c2.y);
                const Vec3& n = nrms[ni];
                pl.nx = n.x; pl.ny = n.y; pl.nz = n.z;
                pl.c = cst[ci];
                L.plates.push_back(pl);
            }
        }
    }
    return L;
}
