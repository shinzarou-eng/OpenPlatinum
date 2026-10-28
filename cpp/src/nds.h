// nds.h — monte la ROM .nds : header -> FAT/FNT -> accès par chemin,
// membres NARC en cache. Port de autoload/NdsDb.gd.
#pragma once
#include "common.h"
#include <memory>

struct Narc {
    std::vector<Bytes> members;
    static std::shared_ptr<Narc> parse(const Bytes& d);
    const Bytes& member(int i) const;
    int count() const { return (int)members.size(); }
};

struct MapHeader {
    uint16_t area, objs, mtx, scr, init, msg, wild, evt, label, win, wthr;
    uint8_t  cam, mtype, bbg, bike, run, rope, fly;
};

class Nds {
public:
    bool mount(const std::string& rom_path);
    bool mount_dir(const std::string& dir);   // cherche un .nds dans dir

    // fichiers
    int   id_of(const std::string& path) const;
    const Bytes& open_path(const std::string& path);   // contenu (ref stable)
    bool  exists(const std::string& path) const { return id_of(path) >= 0; }
    std::shared_ptr<Narc> narc(const std::string& path);
    const std::string& path_of(int id) const;

    // cartes
    struct Matrix {
        int w = 0, h = 0;
        std::string name;
        std::vector<int> headers, land;
        Bytes alt;
    };
    Matrix matrix(int idx);

    struct Area { int props = 0, tex = 0, unk = 0, light = 0; };
    Area area(int idx);

    struct Zev {
        struct Bg    { uint16_t script, type; uint32_t x, z, y; uint16_t dir; };
        struct Obj   { uint16_t id, gfx, move, trainer, flag, script, dir,
                              d0, d1, d2, rx, rz, x, z; uint32_t y; };
        struct Warp  { uint16_t x, z, dest, dwarp; };
        struct Coord { uint16_t script, x, z, w, l, y, val, var; };
        std::vector<Bg> bgs; std::vector<Obj> objects;
        std::vector<Warp> warps; std::vector<Coord> coords;
    };
    Zev zevents(int idx);

    void             load_headers(const std::string& bin_path);
    const MapHeader* header(int id) const;
    const char*      header_name(int id) const;
    int              header_count() const { return (int)_hdrs.size(); }
    std::string      loc_name(int label) const;   // bank 433 pl_msg
    std::string      mapname(int idx) const;      // maptable/mapname.bin

    const std::string& game_title() const { return _title; }
    const std::string& game_code() const { return _code; }
    int file_count() const { return (int)_fat.size(); }

    static constexpr const char* P_MATRIX = "/fielddata/mapmatrix/map_matrix.narc";
    static constexpr const char* P_LAND   = "/fielddata/land_data/land_data.narc";
    static constexpr const char* P_AREA   = "/fielddata/areadata/area_data.narc";
    static constexpr const char* P_ZEV    = "/fielddata/eventdata/zone_event.narc";
    static constexpr const char* P_TEXSET = "/fielddata/areadata/area_map_tex/map_tex_set.narc";
    static constexpr const char* P_BMODEL = "/fielddata/build_model/build_model.narc";
    static constexpr const char* P_MAPNAME= "/fielddata/maptable/mapname.bin";
    static constexpr const char* P_MSG    = "/msgdata/pl_msg.narc";
    static constexpr const char* P_MMODEL = "/data/mmodel/mmodel.narc";
    static constexpr const char* P_SCR   = "/fielddata/script/scr_seq.narc";
    static constexpr const char* P_PLMSG = "/msgdata/pl_msg.narc";
    static constexpr const char* P_TRDATA= "/poketool/trainer/trdata.narc";
    static constexpr const char* P_TRTBL = "/poketool/trmsg/trtbl.narc";
    static constexpr const char* P_TRTBLOF="/poketool/trmsg/trtblofs.narc";
    static constexpr const char* P_ENC   = "/fielddata/encountdata/pl_enc_data.narc";
    static constexpr const char* P_POKEGRA="/poketool/pokegra/pl_pokegra.narc";
    static constexpr const char* P_ITEMICON="/itemtool/itemdata/item_icon.narc";
    static constexpr const char* P_BMANIME="/arc/bm_anime.narc";
    static constexpr const char* P_BMANILIST="/arc/bm_anime_list.narc";
    static constexpr const char* P_FLDTANIME="/data/fldtanime.narc";
    static constexpr const char* P_AREALIGHT="/data/arealight.narc";

private:
    Bytes _rom;                       // ROM entière en mémoire
    std::vector<std::pair<uint32_t,uint32_t>> _fat;
    Bytes _fnt;
    std::unordered_map<int, std::string> _paths;
    std::unordered_map<std::string, int> _byname;
    std::unordered_map<std::string, Bytes> _files;
    std::unordered_map<std::string, std::shared_ptr<Narc>> _narcs;
    std::string _title, _code;
    // headers.bin : {u32 n, u32 names_ofs, entrées 32B, noms ASCIZ}
    Bytes _hdrbin;
    std::vector<const MapHeader*> _hdrs;
    std::vector<const char*> _hdrnames;
    bool _hdr_ok = false;

    void _walk_dir(int dir_idx, const std::string& path, int& fid_counter);
    void _load_headers(const std::string& bin_path);
};
