// nds.cpp — implémentation NDS / NARC / headers / noms de lieux.
#include "nds.h"
#include "msg.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;

// ── NARC ──────────────────────────────────────────────────────────────────────
const Bytes& Narc::member(int i) const {
    static const Bytes empty;
    return (i >= 0 && i < (int)members.size()) ? members[i] : empty;
}

std::shared_ptr<Narc> Narc::parse(const Bytes& d) {
    // BTAF @0x10 : "BTAF" size count [off,size]×n ; puis BTNF, GMIF.
    if (d.size() < 0x20 || ascii(d, 0, 4) != "NARC")
        return nullptr;
    size_t btaf = 0x10;
    if (ascii(d, btaf, 4) != "BTAF") return nullptr;
    uint32_t n = u32(d, btaf + 8);
    size_t fat = btaf + 12;
    size_t btnf = fat + n * 8;
    if (ascii(d, btnf, 4) != "BTNF") return nullptr;
    size_t gmif = btnf + u32(d, btnf + 4);   // size BTNF inclut son header 8o
    if (ascii(d, gmif, 4) != "GMIF") return nullptr;
    size_t data = gmif + 8;
    auto out = std::make_shared<Narc>();
    for (uint32_t i = 0; i < n; i++) {
        // FAT = (offset, fin) relatif au bloc GMIF — pas (offset, taille).
        uint32_t o = u32(d, fat + i * 8), e = u32(d, fat + i * 8 + 4);
        out->members.push_back(slice(d, data + o, e >= o ? e - o : 0));
    }
    return out;
}

// ── montage ───────────────────────────────────────────────────────────────────
bool Nds::mount_dir(const std::string& dir) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        auto p = e.path().string();
        if (p.size() > 4) {
            auto ext = p.substr(p.size() - 4);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".nds") return mount(p);
        }
    }
    return false;
}

bool Nds::mount(const std::string& rom_path) {
    FILE* f = fopen(rom_path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    _rom.resize(sz);
    size_t rd = fread(_rom.data(), 1, sz, f);
    fclose(f);
    if (rd != (size_t)sz) return false;

    _title = ascii(_rom, 0, 12);
    while (!_title.empty() && (uint8_t)_title.back() < 0x20) _title.pop_back();
    _code = ascii(_rom, 0x0C, 4);

    uint32_t fnt_off = u32(_rom, 0x40), fnt_sz = u32(_rom, 0x44);
    uint32_t fat_off = u32(_rom, 0x48), fat_sz = u32(_rom, 0x4C);
    for (uint32_t i = 0; i < fat_sz / 8; i++)
        _fat.push_back({u32(_rom, fat_off + i * 8), u32(_rom, fat_off + i * 8 + 4)});
    _fnt = slice(_rom, fnt_off, fnt_sz);
    int fid = u16(_fnt, 4);
    _walk_dir(0, "/", fid);
    printf("[nds] %s (%s) - %d fichiers, %d FAT\n",
        _title.c_str(), _code.c_str(), (int)_paths.size(), (int)_fat.size());
    return true;
}

void Nds::_walk_dir(int dir_idx, const std::string& path, int& fid) {
    size_t e = (size_t)dir_idx * 8;
    if (e + 8 > _fnt.size()) return;
    uint32_t sub = u32(_fnt, e);
    uint16_t f0 = u16(_fnt, e + 4);
    size_t pos = sub;
    int id = f0;
    while (pos < _fnt.size() && _fnt[pos] != 0) {
        uint8_t ln = _fnt[pos];
        int l = ln & 0x7F;
        std::string nm = ascii(_fnt, pos + 1, l);
        pos += 1 + l;
        if (ln & 0x80) {
            int did = u16(_fnt, pos) & 0xFFF;
            pos += 2;
            int sub_fid = u16(_fnt, (size_t)did * 8 + 4);
            _walk_dir(did, path + nm + "/", sub_fid);
        } else {
            _paths[id] = path + nm;
            _byname[path + nm] = id;
            id++;
        }
    }
    fid = id;
}

// ── accès fichiers ────────────────────────────────────────────────────────────
int Nds::id_of(const std::string& path) const {
    auto it = _byname.find(path);
    return it == _byname.end() ? -1 : it->second;
}

const Bytes& Nds::open_path(const std::string& path) {
    auto it = _files.find(path);
    if (it != _files.end()) return it->second;
    Bytes& out = _files[path];
    int id = id_of(path);
    if (id >= 0 && id < (int)_fat.size()) {
        auto r = _fat[id];
        out = slice(_rom, r.first, r.second - r.first);
    }
    return out;
}

const std::string& Nds::path_of(int id) const {
    static const std::string unk = "?";
    auto it = _paths.find(id);
    return it == _paths.end() ? unk : it->second;
}

std::shared_ptr<Narc> Nds::narc(const std::string& path) {
    auto it = _narcs.find(path);
    if (it != _narcs.end()) return it->second;
    auto n = Narc::parse(open_path(path));
    _narcs[path] = n;
    return n;
}

// ── headers.bin ───────────────────────────────────────────────────────────────
void Nds::load_headers(const std::string& bin_path) {
    FILE* f = fopen(bin_path.c_str(), "rb");
    if (!f) { fprintf(stderr, "[nds] headers.bin introuvable: %s\n", bin_path.c_str()); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    _hdrbin.resize(sz);
    if (fread(_hdrbin.data(), 1, sz, f) != (size_t)sz) { fclose(f); return; }
    fclose(f);
    uint32_t n = u32(_hdrbin, 0), noff = u32(_hdrbin, 4);
    _hdrs.resize(n); _hdrnames.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        _hdrs[i] = (const MapHeader*)(_hdrbin.data() + 8 + (size_t)i * 32);
        _hdrnames[i] = (const char*)(_hdrbin.data() + noff);
        noff += strlen((const char*)(_hdrbin.data() + noff)) + 1;
    }
    _hdr_ok = true;
}

const MapHeader* Nds::header(int id) const {
    if (!_hdr_ok || id < 0 || id >= (int)_hdrs.size()) return nullptr;
    return _hdrs[id];
}

const char* Nds::header_name(int id) const {
    if (!_hdr_ok || id < 0 || id >= (int)_hdrnames.size()) return "?";
    return _hdrnames[id];
}

// ── cartes ────────────────────────────────────────────────────────────────────
Nds::Matrix Nds::matrix(int idx) {
    Matrix out;
    auto n = narc(P_MATRIX);
    if (!n) return out;
    const Bytes& d = n->member(idx);
    if (d.size() < 6) return out;
    int h = u8(d, 0), w = u8(d, 1);
    bool has_h = u8(d, 2) != 0, has_a = u8(d, 3) != 0;
    int nl = u8(d, 4);
    size_t o = 5 + nl;
    int cnt = w * h;
    out.w = w; out.h = h;
    out.name = ascii(d, 5, nl);
    if (has_h) {
        out.headers.resize(cnt);
        for (int i = 0; i < cnt; i++) out.headers[i] = u16(d, o + i * 2);
        o += cnt * 2;
    }
    if (has_a) {
        out.alt = slice(d, o, cnt);
        o += cnt;
    }
    out.land.resize(cnt);
    for (int i = 0; i < cnt; i++) out.land[i] = u16(d, o + i * 2);
    return out;
}

Nds::Area Nds::area(int idx) {
    Area out;
    auto n = narc(P_AREA);
    if (!n) return out;
    const Bytes& d = n->member(idx);
    if (d.size() < 8) return out;
    out.props = u16(d, 0); out.tex = u16(d, 2);
    out.unk = u16(d, 4); out.light = u16(d, 6);
    return out;
}

Nds::Zev Nds::zevents(int idx) {
    Zev out;
    auto n = narc(P_ZEV);
    if (!n) return out;
    const Bytes& d = n->member(idx);
    if (d.size() < 4) return out;
    size_t p = 0;
    uint32_t nb = u32(d, p); p += 4;
    for (uint32_t i = 0; i < nb; i++) {   // bg 20B
        Zev::Bg b{u16(d,p), u16(d,p+2), u32(d,p+4), u32(d,p+8), u32(d,p+12), u16(d,p+16)};
        out.bgs.push_back(b); p += 20;
    }
    nb = u32(d, p); p += 4;
    for (uint32_t i = 0; i < nb; i++) {   // obj 32B
        Zev::Obj b{u16(d,p), u16(d,p+2), u16(d,p+4), u16(d,p+6), u16(d,p+8),
            u16(d,p+10), u16(d,p+12), u16(d,p+14), u16(d,p+16), u16(d,p+18),
            u16(d,p+20), u16(d,p+22), u16(d,p+24), u16(d,p+26), u32(d,p+28)};
        out.objects.push_back(b); p += 32;
    }
    nb = u32(d, p); p += 4;
    for (uint32_t i = 0; i < nb; i++) {   // warp 12B
        out.warps.push_back({u16(d,p), u16(d,p+2), u16(d,p+4), u16(d,p+6)});
        p += 12;
    }
    if (p + 4 <= d.size()) {
        nb = u32(d, p); p += 4;
        for (uint32_t i = 0; i < nb; i++) {   // coord 16B
            out.coords.push_back({u16(d,p), u16(d,p+2), u16(d,p+4), u16(d,p+6),
                u16(d,p+8), u16(d,p+10), u16(d,p+12), u16(d,p+14)});
            p += 16;
        }
    }
    return out;
}

std::string Nds::loc_name(int label) const {
    static std::vector<std::string> names;
    if (names.empty()) {
        auto n = const_cast<Nds*>(this)->narc(P_MSG);
        if (n) names = msgbank_decode(n->member(433));
    }
    if (label < 0 || label >= (int)names.size()) return "?";
    return names[label];
}

std::string Nds::mapname(int idx) const {
    const Bytes& mb = const_cast<Nds*>(this)->open_path(P_MAPNAME);
    if (idx < 0 || (size_t)idx * 16 + 16 > mb.size()) return "?";
    return ascii(mb, (size_t)idx * 16, 16);
}

// ── table MapHeader réelle (arm9, 24 B/enregistrement) ───────────────────────
// Cherche une suite d'au moins 590 records valides dans l'arm9. Critères :
// area<75, mtx<289, scr/init<1124, msg<724, wild<183|0xFFFF, evt<534, cam<17.
static bool hdr24_ok(const Bytes& a, size_t o) {
    if (u8(a, o) >= 75) return false;
    if (u16(a, o + 2) >= 289) return false;
    if (u16(a, o + 4) >= 1124 || u16(a, o + 6) >= 1124) return false;
    if (u16(a, o + 8) >= 724) return false;
    uint16_t w = u16(a, o + 14);
    if (w >= 183 && w != 0xFFFF) return false;
    if (u16(a, o + 16) >= 534) return false;
    if (u8(a, o + 21) >= 17) return false;
    return true;
}

static void hdr24_find(const Bytes& rom, size_t& off, int& n) {
    if (n != 0) return;                                 // -1 échec, >0 localisée
    n = -1;
    uint32_t a9off = u32(rom, 0x20), a9sz = u32(rom, 0x2C);
    size_t end = std::min(rom.size(), (size_t)a9off + a9sz);
    for (size_t o = a9off; o + 24 * 590 <= end; o += 4) {
        if (!hdr24_ok(rom, o)) continue;
        int cnt = 0;
        while (o + (size_t)cnt * 24 + 24 <= end && hdr24_ok(rom, o + cnt * 24))
            cnt++;
        if (cnt >= 590) { off = o; n = cnt; return; }
        o += 4;                                         // faux positif, continue
    }
}

int Nds::map_music(int hdr_id, bool night) const {
    hdr24_find(_rom, _hdr24off, _hdr24n);
    if (_hdr24n <= 0 || hdr_id < 0 || hdr_id >= _hdr24n) return -1;
    return u16(_rom, _hdr24off + (size_t)hdr_id * 24 + (night ? 12 : 10));
}

int Nds::map_camera(int hdr_id) const {
    hdr24_find(_rom, _hdr24off, _hdr24n);
    if (_hdr24n <= 0 || hdr_id < 0 || hdr_id >= _hdr24n) return -1;
    return u8(_rom, _hdr24off + (size_t)hdr_id * 24 + 21);
}
