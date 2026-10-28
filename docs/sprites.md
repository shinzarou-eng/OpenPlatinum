# Overworld + Pokemon sprites

## field sprites (`data/mmodel/mmodel.narc`)

The narc is a flat list of NSBTX texture cells (one model per row).
`res/graphics/field_sprites/field_sprites.order` is the authoritative
member order — line index = narc member index. `OBJ_EVENT_GFX_*` ids from
`object_events_gfx.txt` index that table through
`gObjectEventGfxTexturesTable` in `overlay005/object_event_gfx_data.c`.

    gfx 0   (player)      -> member 90  player_m.nsbtx
    gfx 140 (mom)         -> member 119 mom.nsbtx   ("mama")
    gfx ...               -> gfx_map.inc (generated table)

Each cell is a row of 64x64 frames: facing direction x walk step.
Objects render as camera-facing billboards tilted back to match the DS
perspective; interior camera is orthographic so they stay flat.

## 3D field objects (`fldeff.narc`)

Signposts, mailboxes, berries etc. are real NSBMD models, not sprites.
`gfx_3d.inc` maps `OBJ_EVENT_GFX` -> fldeff member.

## Pokemon sprites (`poketool/pokegra/pl_pokegra.narc`)

Six members per species, dex order:

    sp*6+0..1  back frames (F/M)
    sp*6+2..3  front frames (F/M)
    sp*6+4..5  palettes normal / shiny

Encryption is `PokemonSprite_DecryptPt`: LCRNG `seed = seed*1103515245 + 24691`
(XorShift family), seeded with the first u16 of the CHAR block, XORed word
by word over the pixel data. Pixels are stored as a **linear 160x80 bitmap**
(two 80x80 animation frames side by side), not tiled like NCGR.

Small sprites (party icons, footprints) live in other narcs and are not
encrypted.

## pl_poketex for battles

Battle backgrounds/boxes come from the same pipeline; the wild-encounter
"sprite pops out of the grass" effect is the front sprite billboarded at
the encounter tile with a short bounce.

## Text (`pl_msg.narc`)

724 banks; bank index is the message archive from the map header / battle
tables. Decryption: table key `(seed*i*0x2FD & 0xFFFF) * 0x10001`, text key
`0x91BD3*i` stepped by `0x493D`, Gen4 charmap (A=0x145), tags
`{xxxx:args}` for names/numbers/buffers. French banks used throughout.
