#!/usr/bin/env python3
# gen_headers.py - rebuild cpp/data/headers.bin from a pokeplatinum checkout
#
#   python tools/gen_headers.py <path/to/pokeplatinum> [out.bin]
#
# The DS keeps its map header table baked into the arm9 (sMapHeaders in
# include/data/map_headers.h). Rather than shipping the extracted table,
# we parse the decomp source and emit our own 32-byte records:
#
#   u16 area, objs, mtx, scr, init, msg, wild, evt, label, win, wthr
#   u8  cam, mtype, bbg, bike, run, rope, fly
#   (+3 pad)
#
# The day/night music IDs are dropped (no audio yet). Names at the tail
# come from generated/map_headers.txt, used by the debug window.

import json, re, struct, sys, os


def load_table(root):
    """Parse include/data/map_headers.h -> [(enum_name, {field: raw_str})]"""
    src = open(os.path.join(root, 'include/data/map_headers.h'),
               encoding='utf-8').read()
    recs = []
    for m in re.finditer(r'\[(MAP_HEADER_\w+)\]\s*=\s*\{([^}]*)\}', src):
        name, body = m.group(1), m.group(2)
        fields = dict(re.findall(r'\.(\w+)\s*=\s*([^,}]+)', body))
        recs.append((name, fields))
    return recs


def order_syms(path):
    """.order file -> {symbol: index}"""
    out = {}
    for i, line in enumerate(open(path, encoding='utf-8')):
        line = line.strip()
        if line:
            out[line] = i
    return out


def txt_syms(path):
    return order_syms(path)


def enum_syms(src, prefix):
    """Named enum members (auto-increment) - only inside enum { } blocks."""
    out = {}
    for blk in re.finditer(r'enum\s+\w*\s*\{([^}]*)\}', src):
        n = 0
        for m in re.finditer(r'(\w+)\s*(?:=\s*(0x[0-9A-Fa-f]+|\d+))?\s*,', blk.group(1)):
            name, val = m.group(1), m.group(2)
            n = int(val, 0) if val else n
            if name.startswith(prefix):
                out[name] = n
            n += 1
    return out


def build_symbols(root):
    p = lambda *a: os.path.join(root, *a)
    sym = {}
    sym.update(order_syms(p('res/field/area_data/area_data.order')))
    sym.update(order_syms(p('res/field/matrices/map_matrices.order')))
    sym.update(order_syms(p('res/field/scripts/scripts.order')))
    sym.update(order_syms(p('res/field/encounters/encounters.order')))
    sym.update(order_syms(p('res/field/encounters/encdata_ex.order')))
    sym.update(order_syms(p('res/field/events/zone_event.order')))
    sym.update(txt_syms(p('generated/text_banks.txt')))
    sym.update(txt_syms(p('generated/battle_backgrounds.txt')))

    # SEQ_*_sseq -> seqInfo index in pl_sound_data.json
    snd = json.load(open(p('res/sound/pl_sound_data.json'), encoding='utf-8'))
    for i, e in enumerate(snd['seqInfo']):
        if e.get('name'):
            sym[e['name'] + '_sseq'] = i

    # LocationNames_Text_* -> message index in location_names.json bank
    loc = json.load(open(p('res/text/location_names.json'), encoding='utf-8'))
    for i, e in enumerate(loc['messages']):
        sym[e['id']] = i

    # small enums live in headers/constants
    hdr_src = open(p('include/data/map_headers.h'), encoding='utf-8').read()
    sym.update(enum_syms(hdr_src, 'MAP_LABEL_WINDOW_'))
    sym.update(enum_syms(hdr_src, 'MAP_TYPE_'))
    sym.update(enum_syms(open(p('include/constants/camera_types.h'),
                              encoding='utf-8').read(), 'CAMERA_TYPE_'))
    # weather : #define simples + references entre defines
    #   (ex: OVERWORLD_WEATHER_SNOWPOINT_CITY = YEARLY_START + 4)
    for line in open(p('include/constants/overworld_weather.h'),
                     encoding='utf-8'):
        m = re.match(r'#define\s+(OVERWORLD_WEATHER_\w+)\s+(.+)', line)
        if not m:
            continue
        name, expr = m.group(1), m.group(2).strip()
        for ref in re.findall(r'[A-Z_]\w*', expr):
            expr = expr.replace(ref, str(sym.get(ref, 0)))
        try:
            sym[name] = int(eval(expr, {}, {}))
        except Exception:
            pass

    sym['ENCOUNTERS_NONE'] = 0xFFFF
    sym['TRUE'], sym['FALSE'] = 1, 0
    return sym


def resolve(sym, raw, field, name):
    raw = raw.strip()
    if raw.startswith('0x') or raw.isdigit():
        return int(raw, 0)
    if raw not in sym:
        raise KeyError('%s: %s inconnu dans %s' % (raw, field, name))
    return sym[raw]


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else '_ref_pokeplatinum'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'cpp/data/headers.bin'
    sym = build_symbols(root)
    recs = dict(load_table(root))

    # generated/map_headers.txt liste aussi les aliases de fin
    # (MAP_HEADER_COUNT, MAP_HEADER_INVALID = ..., MAP_HEADER_DYNAMIC = ...)
    # -> record tout a zero, nom recopie tel quel
    names = [l.strip() for l in open(
        os.path.join(root, 'generated/map_headers.txt'),
        encoding='utf-8') if l.strip()]

    buf = bytearray()
    buf += struct.pack('<II', len(names), 8 + 32 * len(names))
    for name in names:
        f = recs.get(name)
        if f is None:
            buf += bytes(32)
            continue
        r = lambda k: resolve(sym, f.get(k, '0'), k, name)
        area   = r('areaDataArchiveID')
        objs   = r('preloadedMapObjectsArchiveID')
        mtx    = r('mapMatrixID')
        scr    = r('scriptsArchiveID')
        init   = r('initScriptsArchiveID')
        msg    = r('msgArchiveID')
        wild   = r('wildEncountersArchiveID')
        evt    = r('eventsArchiveID')
        label  = r('mapLabelTextID')
        win    = r('mapLabelWindowID')
        wthr   = r('weather')
        cam    = r('cameraType')
        mtype  = r('mapType')
        bbg    = r('battleBG')
        bike   = r('isBikeAllowed')
        run    = r('isRunningAllowed')
        rope   = r('isEscapeRopeAllowed')
        fly    = r('isFlyAllowed')
        buf += struct.pack('<11H7B3x', area, objs, mtx, scr, init, msg,
                           wild, evt, label, win, wthr,
                           cam, mtype, bbg, bike, run, rope, fly)
    buf += b'\0'.join(n.encode() for n in names) + b'\0'

    open(out_path, 'wb').write(bytes(buf))
    print('%s: %d records, %d octets' % (out_path, len(names), len(buf)))


if __name__ == '__main__':
    main()
