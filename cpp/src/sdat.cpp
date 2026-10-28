// sdat.cpp — moteur son DS pour OpenPlatinum.
// SDAT -> SSEQ (séquenceur à trames ~192 Hz) -> SBNK (instruments) ->
// SWAR/SWAV (PCM8/16, IMA-ADPCM) + PSG/bruit -> mixage stéréo 44,1 kHz.
// Calqué sur le pilote son ARM7 : trame = 174592 cycles @ 33,513982 MHz ;
// tempo 240/tick, 48 ticks/noire ; priorités, enveloppes et LFO du hardware.
#include "sdat.h"
#include <cmath>
#include <cstring>

namespace {
const double ARM7_CLOCK      = 33513982.0;
const double SOUND_FRAME_SEC = 174592.0 / ARM7_CLOCK;   // ~5,2095 ms
const double CHANNEL_CLOCK   = ARM7_CLOCK / 2.0;
const int    ENV_MIN         = -92544;
const int    PSG_BASE_TIMER  = 8006;
const int    ALLOC_ORDER[16] = {4,5,6,7,2,0,3,1,8,9,10,11,14,12,15,13};
const int    ATTACK_TBL[19]  = {0,1,5,14,26,38,51,63,73,84,92,100,109,
                                116,123,127,132,137,143};
const int    IMA_IDX[8] = {-1,-1,-1,-1,2,4,6,8};
const int    IMA_STEP[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
    408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
    18500,20350,22385,24623,27086,29794,32767 };
const uint16_t PSG_MASK   = 0x3f00;                     // canaux 8-13
const uint16_t NOISE_MASK = 0xc000;                     // canaux 14-15
// trames son par tick de jeu (GAME_TICK 1119780 / SOUND_FRAME 174592)
const double FRAMES_PER_TICK_INV = 174592.0 / 1119780.0;

inline uint8_t  r8 (const uint8_t* p, size_t n, size_t o) { return o < n ? p[o] : 0; }
inline uint16_t r16(const uint8_t* p, size_t n, size_t o) {
    return o + 1 < n ? (uint16_t)(p[o] | (p[o+1] << 8)) : 0;
}
inline uint32_t r32(const uint8_t* p, size_t n, size_t o) {
    return o + 3 < n ? (uint32_t)(p[o] | (p[o+1] << 8) | (p[o+2] << 16)
        | ((uint32_t)p[o+3] << 24)) : 0;
}
inline bool nitro(const uint8_t* p, size_t n, const char* magic) {
    return n >= 8 && !memcmp(p, magic, 4) && r16(p, n, 4) == 0xFEFF;
}
} // namespace

// =====================================================================
//  SDAT
// =====================================================================
bool DsAudio::load(const Bytes& sdat) {
    _sd = &sdat;
    const uint8_t* p = sdat.data(); size_t n = sdat.size();
    if (!nitro(p, n, "SDAT")) return false;
    uint32_t symb = r32(p, n, 0x10), symbSz = r32(p, n, 0x14);
    uint32_t info = r32(p, n, 0x18), fatOfs = r32(p, n, 0x20);
    uint16_t blocks = r16(p, n, 0xE);
    if ((blocks != 3 && blocks != 4) || !info || !fatOfs) return false;
    if (memcmp(p + info, "INFO", 4) || memcmp(p + fatOfs, "FAT ", 4)) return false;

    auto list = [&](int kind, auto cb) {
        uint32_t lo = r32(p, n, info + 8 + kind * 4);
        if (!lo) return;
        uint32_t base = info + lo;
        uint32_t cnt = r32(p, n, base);
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t o = r32(p, n, base + 4 + i * 4);
            if (o) cb((int)i, (size_t)(info + o));
        }
    };
    list(0, [&](int i, size_t o) {
        if (i >= (int)_seqs.size()) _seqs.resize(i + 1);
        _seqs[i] = { r16(p,n,o), r16(p,n,o+4), r8(p,n,o+6),
                     r8(p,n,o+7), r8(p,n,o+8), r8(p,n,o+9) };
    });
    list(2, [&](int i, size_t o) {
        if (i >= (int)_banks.size()) _banks.resize(i + 1);
        BankRec b{}; b.file = r16(p,n,o);
        for (int k = 0; k < 4; k++) b.warc[k] = r16(p,n,o+4+k*2);
        _banks[i] = b;
    });
    list(3, [&](int i, size_t o) {
        if (i >= (int)_warcs.size()) _warcs.resize(i + 1);
        _warcs[i] = r16(p, n, o);
    });
    list(4, [&](int i, size_t o) {
        if (i >= (int)_players.size()) _players.resize(i + 1);
        _players[i] = { r8(p,n,o), r16(p,n,o+2) };
    });
    uint32_t fc = r32(p, n, fatOfs + 8);
    _fat.assign(fc, {0,0});
    for (uint32_t i = 0; i < fc; i++)
        _fat[i] = { r32(p,n,fatOfs+12+i*16), r32(p,n,fatOfs+16+i*16) };

    if (blocks == 4 && symb && symbSz && !memcmp(p + symb, "SYMB", 4)) {
        auto names = [&](int kind, std::unordered_map<std::string,int>& dst) {
            uint32_t lo = r32(p, n, symb + 8 + kind * 4);
            if (!lo) return;
            uint32_t base = symb + lo, cnt = r32(p, n, base);
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t o = r32(p, n, base + 4 + i * 4);
                if (o) dst[(const char*)(p + symb + o)] = (int)i;
            }
        };
        names(0, _seqNames);
        names(2, _bankNames);
    }
    for (auto& g : _globals) g = -1;
    _ok = true;
    return true;
}

int DsAudio::seq_named(const char* n) const {
    auto it = _seqNames.find(n); return it == _seqNames.end() ? -1 : it->second;
}

const uint8_t* DsAudio::_fat_file(int id, uint32_t* size) const {
    *size = 0;
    if (id < 0 || id >= (int)_fat.size() || !_sd) return nullptr;
    uint32_t o = _fat[id].first, s = _fat[id].second;
    if (o + s > _sd->size()) return nullptr;
    *size = s;
    return _sd->data() + o;
}

DsAudio::SseqData DsAudio::_sseq(int seq) {
    if (seq < 0 || seq >= (int)_seqs.size()) return {};
    uint32_t sz = 0;
    const uint8_t* f = _fat_file(_seqs[seq].file, &sz);
    if (!f || !nitro(f, sz, "SSEQ")) return {};
    size_t blk = r16(f, sz, 12);
    if (blk + 12 > sz || memcmp(f + blk, "DATA", 4)) return {};
    size_t end = blk + r32(f, sz, blk + 4);
    size_t st  = r32(f, sz, blk + 8);
    if (st > sz || end > sz || st > end) return {};
    return { f + st, end - st };
}

// ---- SBNK -> régions par programme --------------------------------------
const DsAudio::InstrBank& DsAudio::_bank(int idx) {
    static InstrBank empty;
    if (idx < 0 || idx >= (int)_banks.size()) return empty;
    auto it = _bankCache.find(idx);
    if (it != _bankCache.end()) return *it->second;
    auto ib = std::make_shared<InstrBank>();
    uint32_t sz = 0;
    const uint8_t* f = _fat_file(_banks[idx].file, &sz);
    if (f && nitro(f, sz, "SBNK")) {
        size_t blk = r16(f, sz, 12);
        if (blk + 0x2c <= sz && !memcmp(f + blk, "DATA", 4)) {
            uint32_t cnt = r32(f, sz, blk + 0x28);
            ib->progs.assign(cnt, {});
            // sous-record 12 octets : u16 type, u16 wave/duty, u16 warc,
            // u8 baseKey, a, d, s, r, pan
            auto sub = [&](size_t s, int lo, int hi) -> Region {
                Region r;
                r.type = (uint8_t)r16(f, sz, s);
                r.low = lo; r.high = hi;
                if (r.type == 1) { r.wave = r16(f,sz,s+2); r.warc = r16(f,sz,s+4); }
                else if (r.type == 2) r.wave = r16(f,sz,s+2);
                r.baseKey = r8(f,sz,s+6); r.a = r8(f,sz,s+7); r.d = r8(f,sz,s+8);
                r.s = r8(f,sz,s+9); r.r = r8(f,sz,s+10); r.pan = r8(f,sz,s+11);
                if (r.pan > 127) r.pan = 127;
                return r;
            };
            for (uint32_t i = 0; i < cnt; i++) {
                size_t e = blk + 0x2c + i * 4;
                int type = r8(f, sz, e);
                size_t o = r16(f, sz, e + 1);
                if (!type) continue;
                if (type == 16) {                       // drumset
                    int lo = r8(f,sz,o), hi = r8(f,sz,o+1);
                    for (int k = lo; k <= hi && k < 128; k++) {
                        size_t s = o + 2 + (k - lo) * 12;
                        Region r = sub(s, k, k);
                        if (r.type >= 1 && r.type <= 3) ib->progs[i].push_back(r);
                    }
                } else if (type == 17) {                // key split
                    int lo = 0; size_t s = o + 8;
                    for (int k = 0; k < 8; k++) {
                        int hi = r8(f, sz, o + k);
                        if (!hi) break;
                        Region r = sub(s, lo, hi);
                        if (r.type >= 1 && r.type <= 3) ib->progs[i].push_back(r);
                        lo = hi + 1; s += 12;
                    }
                } else if (type >= 1 && type <= 3) {
                    Region r;
                    r.type = (uint8_t)type;
                    if (type == 1) { r.wave = r16(f,sz,o); r.warc = r16(f,sz,o+2); }
                    else if (type == 2) r.wave = r16(f,sz,o);
                    r.baseKey = r8(f,sz,o+4); r.a = r8(f,sz,o+5); r.d = r8(f,sz,o+6);
                    r.s = r8(f,sz,o+7); r.r = r8(f,sz,o+8); r.pan = r8(f,sz,o+9);
                    if (r.pan > 127) r.pan = 127;
                    ib->progs[i].push_back(r);
                }
            }
        }
    }
    _bankCache[idx] = ib;
    return *ib;
}

// ---- SWAR/SWAV -> échantillons s16 --------------------------------------
const DsAudio::Wave& DsAudio::_wave(int warcIdx, int w) {
    static Wave empty;
    auto it = _warcCache.find(warcIdx);
    std::shared_ptr<std::vector<Wave>> ar;
    if (it != _warcCache.end()) ar = it->second;
    else {
        ar = std::make_shared<std::vector<Wave>>();
        if (warcIdx >= 0 && warcIdx < (int)_warcs.size()) {
            uint32_t sz = 0;
            const uint8_t* f = _fat_file(_warcs[warcIdx], &sz);
            if (f && nitro(f, sz, "SWAR")) {
                size_t blk = r16(f, sz, 12);
                uint32_t cnt = r32(f, sz, blk + 0x28);
                for (uint32_t i = 0; i < cnt; i++) {
                    size_t o = r32(f, sz, blk + 0x2c + i * 4);
                    size_t e = i + 1 < cnt ? r32(f, sz, blk + 0x2c + (i+1)*4) : sz;
                    if (e > sz) e = sz;
                    Wave wv;
                    if (o + 12 <= e) {
                        int fmt = r8(f,sz,o);
                        wv.loop = r8(f,sz,o+1) != 0;
                        // +2 = fréquence nominale (Hz), +4 = timer canal DS,
                        // +6 = boucle en mots — le timer pilote le débit, pas rate
                        wv.timer = r16(f,sz,o+4);
                        int loopW = r16(f,sz,o+6);      // mots (4o) depuis début données
                        int body = (int)(loopW + r32(f,sz,o+8)) * 4;  // +8 = long. boucle
                        if (o + 12 + body > e) body = (int)(e - o - 12);
                        const uint8_t* b = f + o + 12;
                        if (fmt == 0) {                 // PCM8 -> s16
                            wv.s.resize(body);
                            for (int k = 0; k < body; k++)
                                wv.s[k] = (int16_t)((int8_t)b[k] << 8);
                            wv.loopStart = loopW * 4;
                        } else if (fmt == 1) {          // PCM16
                            wv.s.resize(body / 2);
                            for (size_t k = 0; k < wv.s.size(); k++)
                                wv.s[k] = (int16_t)(b[k*2] | (b[k*2+1] << 8));
                            wv.loopStart = loopW * 2;
                        } else if (fmt == 2 && body >= 4) { // IMA-ADPCM
                            int smp = (int16_t)(b[0] | (b[1] << 8));
                            int idx = std::min(88, (int)b[2]);
                            int ns = (body - 4) * 2;
                            wv.s.resize(ns);
                            for (int k = 0; k < ns; k++) {
                                uint8_t nb = k & 1 ? b[4 + k/2] >> 4 : b[4 + k/2] & 15;
                                int step = IMA_STEP[idx];
                                int diff = step >> 3;
                                if (nb & 1) diff += step >> 2;
                                if (nb & 2) diff += step >> 1;
                                if (nb & 4) diff += step;
                                smp = nb & 8 ? std::max(smp - diff, -0x7fff)
                                             : std::min(smp + diff, 0x7fff);
                                idx = std::min(88, std::max(0, idx + IMA_IDX[nb & 7]));
                                wv.s[k] = (int16_t)smp;
                            }
                            wv.loopStart = std::max(0, (loopW - 1) * 8);
                        }
                    }
                    ar->push_back(std::move(wv));
                }
            }
        }
        _warcCache[warcIdx] = ar;
    }
    if (w < 0 || w >= (int)ar->size()) return empty;
    return (*ar)[w];
}

// =====================================================================
//  Tables hardware
// =====================================================================
int DsAudio::_dbsq(int i) {
    if (i <= 0) return -32768;
    if (i >= 127) return 0;
    return (int)lround(std::max(-722.0, 400.0 * log10(i / 127.0)));
}
int DsAudio::_attackRate(int a) {
    if (a < 109) return 255 - a;
    return ATTACK_TBL[std::min(18, std::max(0, 127 - a))];
}
int DsAudio::_fallRate(int v) {
    if (v == 127) return 0xffff;
    if (v == 126) return 0x3c00;
    if (v < 50) return v * 2 + 1;
    return 0x1e00 / (126 - v);
}
int DsAudio::_sinIdx(int x) {
    static int SINE[33];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 33; i++)
            SINE[i] = (int)lround(127.0 * sin(i * 3.14159265358979323846 / 64.0));
        init = true;
    }
    x &= 127;
    if (x < 32) return SINE[x];
    if (x < 64) return SINE[64 - x];
    if (x < 96) return -SINE[x - 64];
    return -SINE[128 - x];
}
int DsAudio::_timerFor(int base, int pitch) {
    int oct = (int)floor(pitch / 768.0);
    int rem = pitch - oct * 768;
    double t = floor(base * pow(2.0, -rem / 768.0) * pow(2.0, -oct));
    return (int)std::min(65535.0, std::max(16.0, t));
}
double DsAudio::_channelAmp(int db) {                  // 1/10 dB -> linéaire
    if (db <= -723) return 0;
    if (db > 0) db = 0;
    int divider = db < -240 ? 16 : db < -120 ? 4 : db < -60 ? 2 : 1;
    return std::min(127.0, round(127.0 * pow(10.0, db / 200.0) * divider))
        / 127.0 / divider;
}

// =====================================================================
//  Moteur
// =====================================================================
bool DsAudio::Player::playing() const {
    for (const auto& t : trk)
        if (t.active && (!t.ended || !t.chans.empty())) return true;
    return false;
}

void DsAudio::_start(int playerIdx, int seq, int bankOverride) {
    if (!_ok || seq < 0 || seq >= (int)_seqs.size()) return;
    const SeqRec& si = _seqs[seq];
    auto it = _pl.find(playerIdx);
    if (it != _pl.end()) { _stop(*it->second, 0); _pl.erase(it); }
    SseqData d = _sseq(seq);
    if (!d.p) return;
    auto p = std::make_unique<Player>();
    p->idx = playerIdx;
    p->seq = seq;
    int bi = bankOverride >= 0 ? bankOverride : si.bank;
    p->bank = &_bank(bi);
    if (bi >= 0 && bi < (int)_banks.size())
        for (int k = 0; k < 4; k++) p->warc[k] = _banks[bi].warc[k];
    p->chmask = playerIdx < (int)_players.size() && _players[playerIdx].chmask
        ? _players[playerIdx].chmask : 0xffff;
    p->prio = si.cprio;
    p->vol = si.vol;
    for (auto& v : p->vars) v = -1;
    Track& t = p->trk[0];
    t = Track{};
    for (auto& v : t.vars) v = -1;
    t.d = d; t.active = true;
    _pl[playerIdx] = std::move(p);
}

void DsAudio::_stop(Player& p, int ticks) {
    if (ticks > 0) {
        p.fadeFrom = (int)_fadeVol(p);
        p.fadeTo = 0; p.fadeTicks = ticks; p.fadeElapsed = 0;
        p.stopAfterFade = true;
        return;
    }
    for (auto& t : p.trk) _closeTrack(t, -1);
    p.seq = -1;
    for (auto& t : p.trk) { t.ended = true; t.active = false; t.d.p = nullptr; }
}

void DsAudio::_free(Chan& c) {
    c.active = false;
    c.ampl = c.left = c.right = 0;
    if (c.trk) {
        auto& v = c.trk->chans;
        v.erase(std::find(v.begin(), v.end(), &c));
        c.trk = nullptr;
    }
}

void DsAudio::_release(Chan& c, int rel) {
    if (rel >= 0) c.rel = _fallRate(rel);
    c.priority = 1;
    c.envState = 3;
}

void DsAudio::_releaseTrack(Track& t, int rel) {
    for (auto* c : t.chans) if (c->envState != 3) _release(*c, rel);
}

void DsAudio::_closeTrack(Track& t, int rel) {
    _releaseTrack(t, rel);
    for (auto* c : t.chans) c->trk = nullptr;
    t.chans.clear();
}

DsAudio::Chan* DsAudio::_alloc(uint16_t mask, int prio) {
    Chan* best = nullptr;
    for (int i : ALLOC_ORDER) {
        if (!(mask & (1 << i))) continue;
        Chan& c = _ch[i];
        int cp = c.active ? c.priority : 0;
        if (!best) { best = &c; continue; }
        int bp = best->active ? best->priority : 0;
        if (cp < bp || (cp == bp && c.ampl < best->ampl)) best = &c;
    }
    if (!best) return nullptr;
    if (best->active && prio < best->priority) return nullptr;
    if (best->active) _free(*best);
    return best;
}

uint8_t DsAudio::_rb(Track& t) { return t.pc < t.d.n ? t.d.p[t.pc++] : 0; }

int DsAudio::_varlen(Track& t) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = _rb(t);
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

int16_t& DsAudio::_var(Player& p, Track& t, int i) {
    if (i < 16) return t.vars[i];
    if (i < 32) return p.vars[i - 16];
    return _globals[(i - 32) & 15];
}

double DsAudio::_fadeVol(const Player& p) const {
    if (p.fadeTicks <= 0 || p.fadeElapsed >= p.fadeTicks) return p.fadeTo;
    return p.fadeFrom + (p.fadeTo - p.fadeFrom) * p.fadeElapsed / p.fadeTicks;
}

void DsAudio::_frame() {
    for (auto& kv : _pl) {
        Player& p = *kv.second;
        if (p.fadeElapsed < p.fadeTicks)
            p.fadeElapsed = std::min((double)p.fadeTicks,
                p.fadeElapsed + FRAMES_PER_TICK_INV);
        if (p.stopAfterFade && p.fadeElapsed >= p.fadeTicks) {
            p.stopAfterFade = false;
            _stop(p, 0);
            continue;
        }
        if (!p.paused) _seqFrame(p);
    }
    for (auto& kv : _pl) {
        Player& p = *kv.second;
        for (auto& t : p.trk)
            if (t.active) _updateTrackChans(p, t);
    }
    for (auto& c : _ch) if (c.active) _updateChan(c);
    // balaye les players finis (notes relâchées terminées)
    for (auto it = _pl.begin(); it != _pl.end();)
        if (it->second->seq < 0 && !it->second->playing())
            it = _pl.erase(it);
        else ++it;
}

void DsAudio::_seqFrame(Player& p) {
    while (p.tempoCnt >= 240) {
        p.tempoCnt -= 240;
        bool playing = false;
        for (int i = 0; i < 16; i++) {
            Track& t = p.trk[i];
            if (!t.active) continue;
            if (_stepTrack(p, t)) {
                _closeTrack(t, -1);
                t.active = false;
            } else playing = true;
        }
        if (!playing) { _stop(p, 0); return; }
    }
    p.tempoCnt += p.tempo;
}

bool DsAudio::_stepTrack(Player& p, Track& t) {
    for (auto* c : t.chans) {
        if (c->length > 0 && --c->length == 0 && c->envState != 3)
            _release(*c, -1);
        if (!c->autoSweep && c->sweepCnt < c->sweepLen) c->sweepCnt++;
    }
    if (t.ended) return t.chans.empty();
    if (t.wait > 0) { t.wait--; if (t.wait > 0) return false; }
    int n = 0;
    while (t.wait == 0 && !t.ended) {
        if (++n > 10000) { t.ended = true; break; }
        _command(p, t);
    }
    return t.ended && t.chans.empty();
}

void DsAudio::_command(Player& p, Track& t) {
    int cmd = _rb(t);
    bool cond = true;
    int prefix = 0;                                    // 1 random 2 var
    for (;;) {
        if (cmd == 0xa2) cond = t.cmp;
        else if (cmd == 0xa0) prefix = 1;
        else if (cmd == 0xa1) prefix = 2;
        else break;
        cmd = _rb(t);
    }
    auto last = [&](auto rd) -> int {
        if (prefix == 1) {
            int mn = (int16_t)(_rb(t) | (_rb(t) << 8));
            int mx = (int16_t)(_rb(t) | (_rb(t) << 8));
            _rand = _rand * 1664525u + 1013904223u;
            return mn + (int)(((_rand >> 16) * (mx - mn + 1)) >> 16);
        }
        if (prefix == 2) { int i = _rb(t); return _var(p, t, i); }
        return rd();
    };
    auto u8r  = [&] { return _rb(t); };
    auto s8r  = [&] { return (int8_t)_rb(t); };
    auto u16r = [&] { return _rb(t) | (_rb(t) << 8); };
    auto s16r = [&] { return (int16_t)(_rb(t) | (_rb(t) << 8)); };
    auto u24r = [&] { return _rb(t) | (_rb(t) << 8) | (_rb(t) << 16); };
    auto vlr  = [&] { return _varlen(t); };

    if (cmd < 0x80) {                                  // note
        int vel = u8r();                               // vélocité brute
        int len = last(vlr);                           // durée (préfixe ok)
        if (!cond) return;
        int key = std::min(127, std::max(0, cmd + t.transpose));
        _noteOn(p, t, key, vel, len);
        t.portaKey = key;
        if (t.noteWait) t.wait = len;
        return;
    }
    switch (cmd) {
    case 0x80: { int v = last(vlr); if (cond) t.wait = v; return; }
    case 0x81: { int v = last(vlr); if (cond) t.prg = v; return; }
    case 0x93: {
        int n_ = u8r(); int at = u24r();
        if (cond && n_ > 0 && n_ < 16 && at < (int)t.d.n) {
            Track& nt = p.trk[n_];
            nt = Track{};
            for (auto& v : nt.vars) v = -1;
            nt.d = t.d; nt.pc = (size_t)at; nt.active = true;
        }
        return; }
    case 0x94: { int at = u24r(); if (cond) t.pc = (size_t)at; return; }
    case 0x95: { int at = u24r();
        if (cond && t.sp < 3) { t.stack[t.sp++] = { t.pc, -1 }; t.pc = (size_t)at; }
        return; }
    case 0xfd:
        if (!cond) return;
        if (t.sp > 0) t.pc = t.stack[--t.sp].pc;
        return;
    case 0xd4: {
        int cnt = last(u8r);
        if (cond && t.sp < 3) t.stack[t.sp++] = { t.pc, cnt };
        return; }
    case 0xfc:
        if (!cond || t.sp <= 0) return;
        { auto& top = t.stack[t.sp - 1];
          if (top.count == 0) t.pc = top.pc;
          else if (--top.count == 0) t.sp--;
          else t.pc = top.pc; }
        return;
    case 0xfe: { int v = last(u16r); if (cond) p.chmask = (uint16_t)v; return; }
    case 0xd6: last(u8r); return;                  // PrintVar (debug, sans effet)
    case 0xff: if (cond) t.ended = true; return;
    case 0xe0: { int v = last(u16r); if (cond) t.lfoDelay = v; return; }
    case 0xe1: { int v = last(u16r); if (cond) p.tempo = v; return; }
    case 0xe3: { int v = last(s16r); if (cond) t.sweepPitch = v; return; }
    }
    if (cmd >= 0xb0 && cmd <= 0xbd) {
        int i = u8r();                                   // index brut
        int val = last(s16r);
        if (!cond) return;
        int16_t& vs = _var(p, t, i); int v = vs;
        switch (cmd) {
        case 0xb0: vs = (int16_t)val; break;
        case 0xb1: vs = (int16_t)(v + val); break;
        case 0xb2: vs = (int16_t)(v - val); break;
        case 0xb3: vs = (int16_t)(v * val); break;
        case 0xb4: if (val) vs = (int16_t)(v / val); break;
        case 0xb5: vs = (int16_t)(val >= 0 ? v << val : v >> -val); break;
        case 0xb6: _rand = _rand * 1664525u + 1013904223u;
            vs = (int16_t)((((_rand >> 16) * (std::abs(val) + 1)) >> 16)
                * (val < 0 ? -1 : 1)); break;
        case 0xb8: t.cmp = v == val; break;
        case 0xb9: t.cmp = v >= val; break;
        case 0xba: t.cmp = v >  val; break;
        case 0xbb: t.cmp = v <= val; break;
        case 0xbc: t.cmp = v <  val; break;
        case 0xbd: t.cmp = v != val; break;
        }
        return;
    }
    if (cmd >= 0xc0 && cmd <= 0xd6) {
        int v = (cmd == 0xc3 || cmd == 0xc4) ? last(s8r) : last(u8r);
        if (!cond) return;
        switch (cmd) {
        case 0xc0: t.pan = v - 64; break;
        case 0xc1: t.vol = v; break;
        case 0xc2: p.vol = v; break;
        case 0xc3: t.transpose = v; break;
        case 0xc4: t.pitchBend = v; break;
        case 0xc5: t.bendRange = v; break;
        case 0xc6: t.prio = v; break;
        case 0xc7: t.noteWait = v != 0; break;
        case 0xc8: t.tie = v != 0; if (!t.tie) _releaseTrack(t, -1); break;
        case 0xc9: t.portaKey = v + t.transpose; t.portamento = true; break;
        case 0xca: t.lfoDepth = v; break;
        case 0xcb: t.lfoSpeed = v; break;
        case 0xcc: t.lfoType = v; break;
        case 0xcd: t.lfoRange = v; break;
        case 0xce: t.portamento = v != 0; break;
        case 0xcf: t.portaTime = v; break;
        case 0xd0: t.att = v; break;
        case 0xd1: t.dec = v; break;
        case 0xd2: t.sus = v; break;
        case 0xd3: t.rel = v; break;
        case 0xd5: t.expr = v; break;
        case 0xd6: break;
        }
        return;
    }
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("SDATDBG") ? 1 : 0;
    if (dbg) fprintf(stderr, "[sdat] op inconnu %02x seq=%d pc=%zx\n",
        cmd, p.seq, t.pc);
    t.ended = true;                                    // opcode inconnu
}

void DsAudio::_noteOn(Player& p, Track& t, int key, int vel, int len) {
    Chan* c = nullptr;
    if (t.tie)
        for (auto* x : t.chans) if (x->envState != 3) { c = x; break; }
    if (c) {
        c->key = key; c->vel = vel;
    } else {
        if (t.prg >= (int)p.bank->progs.size()) return;
        const Region* reg = nullptr;
        for (auto& r : p.bank->progs[t.prg])
            if (key >= r.low && key <= r.high) { reg = &r; break; }
        if (!reg || reg->type < 1 || reg->type > 3) return;
        uint16_t mask = p.chmask;
        if (reg->type == 2) mask &= PSG_MASK;
        else if (reg->type == 3) mask &= NOISE_MASK;
        int prio = std::min(255, t.prio + p.prio);
        c = _alloc(mask, prio);
        if (!c) return;
        c->active = true;
        c->type = reg->type;
        c->w = nullptr;
        if (reg->type == 1 && reg->warc < 4 && p.warc[reg->warc] >= 0)
            c->w = &_wave(p.warc[reg->warc], reg->wave);
        if (reg->type == 1 && !c->w) { _free(*c); return; }
        c->duty = reg->type == 2 ? reg->wave : 0;
        c->priority = prio;
        c->key = key; c->baseKey = reg->baseKey; c->vel = vel;
        c->att = _attackRate(t.att != 255 ? t.att : reg->a);
        c->dec = _fallRate(t.dec != 255 ? t.dec : reg->d);
        c->sus = _dbsq(t.sus != 255 ? t.sus : reg->s) << 7;
        c->rel = _fallRate(t.rel != 255 ? t.rel : reg->r);
        c->envState = 0; c->envLevel = ENV_MIN;
        c->instPan = reg->pan - 64;
        c->lfoType = t.lfoType; c->lfoSpeed = t.lfoSpeed;
        c->lfoDepth = t.lfoDepth; c->lfoRange = t.lfoRange;
        c->lfoDelay = t.lfoDelay;
        c->lfoCnt = 0; c->lfoDly = 0;
        c->pos = 0; c->lfsr = 0x7fff; c->noiseOut = 1;
        c->length = t.tie ? -1 : len;
        c->fresh = true;
        c->trk = &t;
        t.chans.push_back(c);
    }
    int sweep = t.sweepPitch;
    if (t.portamento) sweep += (t.portaKey - key) << 6;
    c->sweepPitch = sweep;
    c->sweepCnt = 0;
    if (t.portaTime == 0) { c->sweepLen = len; c->autoSweep = false; }
    else {
        c->sweepLen = (t.portaTime * t.portaTime * std::abs(sweep)) >> 11;
        c->autoSweep = true;
    }
}

void DsAudio::_updateTrackChans(Player& p, Track& t) {
    int pv = _playervol.count(p.idx) ? _playervol[p.idx] : 127;
    int vol = _dbsq(t.vol) + _dbsq(t.expr) + _dbsq(p.vol) + _dbsq(p.initVol)
        + _dbsq(pv) + _dbsq((int)lround(_fadeVol(p)));
    int pitch = (t.pitchBend * t.bendRange) >> 1;
    int pan = t.pan;
    for (auto* c : t.chans) {
        c->userVol = vol;
        c->userPitch = pitch;
        c->userPan = pan;
    }
}

void DsAudio::_updateChan(Chan& c) {
    c.fresh = false;
    switch (c.envState) {
    case 0:                                            // attaque
        c.envLevel = -((-c.envLevel * c.att) >> 8);
        if (c.envLevel == 0) c.envState = 1;
        break;
    case 1:                                            // decay -> sustain
        c.envLevel -= c.dec;
        if (c.envLevel <= c.sus) { c.envLevel = c.sus; c.envState = 2; }
        break;
    case 3:                                            // release
        c.envLevel -= c.rel;
        if (c.envLevel <= ENV_MIN) { _free(c); return; }
        break;
    }
    int lfo = 0;
    if (c.lfoDepth) {
        if (c.lfoDly < c.lfoDelay) c.lfoDly++;
        else {
            lfo = _sinIdx(c.lfoCnt >> 8) * c.lfoDepth * c.lfoRange;
            c.lfoCnt = (c.lfoCnt + (c.lfoSpeed << 6)) & 0x7fff;
        }
    }
    if (c.autoSweep && c.sweepCnt < c.sweepLen) c.sweepCnt++;
    int sweep = c.sweepLen > 0
        ? (int)((double)c.sweepPitch * (c.sweepLen - c.sweepCnt) / c.sweepLen) : 0;
    int vol = c.userVol + _dbsq(c.vel) + (c.envLevel >> 7);
    int pitch = ((c.key - c.baseKey) << 6) + c.userPitch + sweep;
    int pan = c.userPan + c.instPan;
    if (c.lfoType == 0) pitch += (lfo * 64) >> 14;
    else if (c.lfoType == 1) vol += (lfo * 60) >> 14;
    else if (c.lfoType == 2) pan += (lfo * 64) >> 14;
    int hwPan = std::min(127, std::max(0, pan + 64));
    c.ampl = _channelAmp(vol);
    c.left = c.ampl * (127 - hwPan) / 127.0;
    c.right = c.ampl * hwPan / 127.0;
    int base = c.type == 1 && c.w ? c.w->timer : PSG_BASE_TIMER;
    c.rate = CHANNEL_CLOCK / _timerFor(base, pitch);
}

// ---- mixage --------------------------------------------------------------
double DsAudio::_mixSample(Chan& c, double step) {
    if (c.type == 2) {                                 // PSG carré
        double s = ((int)c.pos % 8) <= c.duty ? 1.0 : -1.0;
        c.pos = fmod(c.pos + step, 8.0);
        return s;
    }
    // bruit LFSR 15 bits : bit0 sort -> XOR 0x6000 (taps bits 13-14)
    double s = c.noiseOut;
    c.pos += step;
    while (c.pos >= 1.0) {
        c.pos -= 1.0;
        int fb = c.lfsr & 1;
        c.lfsr >>= 1;
        if (fb) c.lfsr ^= 0x6000;
        c.noiseOut = fb ? -1 : 1;
    }
    return s;
}

// avance un canal PCM d'un échantillon (hold, boucle ou fin)
bool DsAudio::_pcmStep(Chan& c, double step) {
    c.pos += step;
    const auto& w = *c.w;
    if (c.pos < (double)w.s.size()) return true;
    if (!w.loop || w.s.size() <= (size_t)w.loopStart) return false;
    int rest = (int)w.s.size() - w.loopStart;
    c.pos = w.loopStart + fmod(c.pos - w.loopStart, (double)rest);
    return true;
}

void DsAudio::mix(float* out, int frames) {
    if (!_ok) { memset(out, 0, frames * 8); return; }
    static int dbg = -1, dbgFrames = 0;
    if (dbg < 0) dbg = getenv("SDATDBG") ? 1 : 0;
    if (dbg && ++dbgFrames % 120 == 0) {
        int act = 0; for (auto& c : _ch) if (c.active) act++;
        fprintf(stderr, "[sdat] players=%d chans=%d\n", (int)_pl.size(), act);
    }
    const double frameSamples = SOUND_FRAME_SEC * 44100.0;
    for (int i = 0; i < frames; i++) {
        if (_frameClock <= 0) { _frame(); _frameClock += frameSamples; }
        _frameClock--;
        double l = 0, r = 0;
        for (auto& c : _ch) {
            if (!c.active || c.ampl == 0) continue;
            double s;
            if (c.type == 1) {
                if (!c.w || c.w->s.empty()) { s = 0; }
                else {
                    int n = (int)c.w->s.size();
                    int ip = (int)c.pos;
                    if (ip >= n) ip = n - 1;
                    int in = ip + 1 < n ? ip + 1 : ip;
                    double fr = c.pos - ip;
                    s = (c.w->s[ip] + (c.w->s[in] - c.w->s[ip]) * fr) / 32768.0;
                    if (!_pcmStep(c, c.rate / 44100.0)) _free(c);
                }
            } else {
                s = _mixSample(c, c.rate / 44100.0);
            }
            l += s * c.left;
            r += s * c.right;
        }
        out[i*2]   = (float)std::min(1.0, std::max(-1.0, l)) * _gain;
        out[i*2+1] = (float)std::min(1.0, std::max(-1.0, r)) * _gain;
    }
}

// =====================================================================
//  API jeu
// =====================================================================
void DsAudio::music(int seq) {
    if (seq < 0 || seq >= (int)_seqs.size() || seq == _bgm_seq) return;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("SDATDBG") ? 1 : 0;
    if (dbg) fprintf(stderr, "[sdat] BGM seq=%d file=%d bank=%d ply=%d\n",
        seq, _seqs[seq].file, _seqs[seq].bank, _seqs[seq].player);
    _bgm_seq = seq; _bgm_zone = seq;
    Player* p = _byPlayer(_bgm_player());
    if (p) _stop(*p, 0);
    _bgm_player_idx = _seqs[seq].player;
    _start(_bgm_player_idx, seq);
}

void DsAudio::music_fade(int ticks) {                  // fondu sortant -> stop
    Player* p = _byPlayer(_bgm_player());
    if (p) _stop(*p, ticks);
}

void DsAudio::music_restore(int ticks) {
    if (_bgm_zone < 0) return;
    _start(_bgm_player_idx, _bgm_zone);
    Player* p = _byPlayer(_bgm_player());
    if (p) { p->fadeFrom = 0; p->fadeTo = 127; p->fadeTicks = ticks;
             p->fadeElapsed = 0; }
    _bgm_seq = _bgm_zone;
}

void DsAudio::se(int seq) {
    if (seq < 0 || seq >= (int)_seqs.size()) return;
    _start(_seqs[seq].player, seq);
}

void DsAudio::se_stop(int seq) {
    if (seq < 0 || seq >= (int)_seqs.size()) return;
    Player* p = _byPlayer(_seqs[seq].player);
    if (p && p->seq == seq) _stop(*p, 0);
}

bool DsAudio::se_busy(int seq) const {
    if (seq < 0 || seq >= (int)_seqs.size()) return false;
    auto it = _pl.find(_seqs[seq].player);
    return it != _pl.end() && it->second->seq == seq;
}

bool DsAudio::player_busy(int idx) const {
    auto it = _pl.find(idx);
    return it != _pl.end() && it->second->playing();
}

bool DsAudio::se_any_busy() const {                 // WaitSE : joueurs effets
    for (auto& kv : _pl) {
        int i = kv.second->idx;
        if (i != _bgm_player_idx && i != _cry_player_idx && i != 2 &&
            kv.second->playing()) return true;      // 2 = PLAYER_ME (fanfare)
    }
    return false;
}

void DsAudio::fanfare(int seq) {
    pause_bgm(true);                                   // le ME suspend le BGM
    _fanfare_tail = 0;
    se(seq);                                           // player ME (INFO)
}

bool DsAudio::fanfare_busy() {
    // le ME joue, puis 15 sondes de queue avant de rendre la main
    for (auto& kv : _pl)
        if (kv.second->idx >= 0 && kv.second->playing()) {
            // un joueur non-BGM encore actif ?
            if (kv.second->idx != _bgm_player()) return true;
        }
    return _fanfare_tail > 0;
}

void DsAudio::fanfare_check() {                        // sondé par WaitFanfare
    for (auto& kv : _pl)
        if (kv.second->idx != _bgm_player() && kv.second->playing()) return;
    if (_fanfare_tail > 0) {
        if (--_fanfare_tail == 0) pause_bgm(false);    // fin -> BGM reprend
    } else if (_bgm_paused()) {
        _fanfare_tail = 15;                            // queue : 15 sondes
    }
}

void DsAudio::cry(int species, int form) {
    int seq = seq_named("SEQ_PV001");
    if (seq < 0) seq = 1;                              // repli : index 1
    int bank = species == 492 && form == 1 ? 494 : species;
    if (bank < 0 || bank >= (int)_banks.size()) bank = 1;
    _cry_player_idx = _seqs[seq].player;
    _start(_cry_player_idx, seq, bank);
}

bool DsAudio::cry_busy() const {
    auto it = _pl.find(_cry_player());
    return it != _pl.end() && it->second->playing();
}

void DsAudio::pause_bgm(bool pz) {
    Player* p = _byPlayer(_bgm_player());
    if (p && p->paused != pz) {
        p->paused = pz;
        if (pz) for (auto& t : p->trk) _releaseTrack(t, 127);
    }
}

void DsAudio::stop_all() {
    for (auto& kv : _pl) _stop(*kv.second, 0);
    _pl.clear();
    _bgm_seq = _bgm_zone = -1;
    _fanfare_tail = 0;
}

