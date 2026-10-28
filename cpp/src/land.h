// land.h — membre land_data.narc : attrs terrain 32×32, props, BMD0, BDHC.
// Port de scripts/land.gd.
#pragma once
#include "common.h"

struct LandProp {
    uint32_t model = 0;
    Vec3 pos, rot, scale{1, 1, 1};
};

// plaque BDHC (bdhc.c DS) : rectangle xz = bbox de 2 points, hauteur =
// équation de plan Ax+By+Cz+D=0 résolue en y = -(A*x + C*z + D) / B
struct BDHCPlate {
    float x0, z0, x1, z1;   // bbox locale à la cellule (px)
    float nx, ny, nz, c;    // normale fx32 + constante fx32
};

struct Land {
    uint16_t attrs[1024] = {};
    std::vector<LandProp> props;
    Bytes model;      // BMD0
    Bytes bdhc;
    std::vector<BDHCPlate> plates;

    static Land parse(const Bytes& d);
    int attr_at(int col, int row) const {
        if (col < 0 || col >= 32 || row < 0 || row >= 32) return 0;
        return attrs[row * 32 + col];
    }
};
