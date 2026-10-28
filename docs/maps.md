# Maps: headers, matrix, land_data, zone_event

## Map headers

Not a file — `sMapHeaders` is compiled into the arm9 (24-byte `MapHeader`,
see `include/map_header.h`). Field order in the ROM record:

    u8  areaDataArchiveID, preloadedMapObjectsArchiveID
    u16 mapMatrixID, scriptsArchiveID, initScriptsArchiveID, msgArchiveID
    u16 dayMusicID, nightMusicID
    u16 wildEncountersArchiveID, eventsArchiveID
    u8  mapLabelTextID, mapLabelWindowID
    u8  weather, cameraType
    u16 mapType:7 | battleBG:5 | bike:1 | run:1 | rope:1 | fly:1

`tools/gen_headers.py` rebuilds our 32-byte runtime records (music dropped,
bitfields split into u8s, names appended) straight from the decomp checkout,
so nothing copyrighted ships in the repo. Note: weather values above 31 are
compound `#define`s (`OVERWORLD_WEATHER_YEARLY_START + n`) — a naive parser
zeros them, which is how the previous table ended up with 133 maps at
weather 0.

## Matrix streaming

`map_matrix.narc` member `mtx` = an h×w grid of map-land pairs (`+ name`).
The overworld matrix is 30×30 maps of 32×32 tiles each; the engine keeps a
window of cells resident around the player and streams on grid crossings.
Zone transitions fire when the current cell's header changes.

## zone_event.narc

Per-map event block: object (NPC) records, warps, coordinate triggers,
background events (signs/PC/TV).

- Object coords are tile coords. World pos = `t*16 + 8` in x/z.
- `event.y` is a raw **fx32** already in world units (`0x10000` = 16.0).
  `map_object.c` assigns it directly to `pos.y`, then
  `RecalculateObjectHeight` overwrites y with the BDHC terrain height.
  It is an initial value/fallback — NOT a per-tile height to scale by
  (multiplying it throws objects kilometers up or down).
- Warps carry dest header + `dest_warp_id`; the door opens when you push
  into its wall, and arrival faces south with a step out.
- Coord events have a rect + `(var == value)` guard evaluated on tile
  entry only.

## land_data.narc / area_data

`land_data` = per-cell geometry: an NSBMD model + BDHC collision + tile
attributes + prop placements. `area_data` picks the texture set so several
cells share visuals. NSBMD parsing (mesh commands are packed display lists)
and TEX0 (4bpp/a5i3/...) are in `bmd.cpp`.

## Collision

BDHC plates + per-tile attribute behaviours. Ground height = vertical
raycast over the collision triangles; walls use the horizontal planes.
Ledge/jump behaviours and tall-grass flags come from tile attributes.
Interior maps have no matrix — one cell, orthographic camera preset from
the header (`CAMERA_TYPE_INTERIOR_ORTHOGRAPHIC`), and camera pulls back
when a wall blocks the view.
