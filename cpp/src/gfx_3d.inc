// gfx id (OBJ_EVENT_GFX) -> membre fldeff.narc (modeles 3D)
static const struct { int16_t gfx, member; } GFX_3D[] = {
    {91, 69},
    {92, 70},
    {93, 71},
    {94, 72},
    {95, 73},
    {96, 74},
    {100, 75},   // BERRY_SOIL : monticule berry_patch_moist.nsbmd (humide DS)
    {183, 79},
    {209, 110},
    {262, 149},
    {950, 113},  // VEILSTONE_GYM : sac de frappe (veilstone_gym_sandbag)
    {951, 112},  // VEILSTONE_GYM : pneus (tire_stack_toppled, 4 meshes)
};
static const int GFX_3D_N = (int)(sizeof(GFX_3D) / sizeof(GFX_3D[0]));
