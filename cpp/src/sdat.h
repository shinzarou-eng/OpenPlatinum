// sdat.h — moteur son DS : SDAT (SSEQ/SBNK/SWAR/SWAV), séquenceur ~192 Hz,
// synthèse 16 canaux (PCM8/16, IMA-ADPCM, PSG, bruit) -> stéréo 44,1 kHz.
#pragma once
#include "common.h"
#include <memory>
#include <unordered_map>

class DsAudio {
public:
    bool load(const Bytes& sdat);          // parse l'archive (octets ROM vivants)
    bool ok() const { return _ok; }
    int  seq_named(const char* n) const;   // index séquence par nom SYMB (-1)

    // ---- API jeu ---------------------------------------------------------
    void mix(float* interleaved, int frames);       // remplit F32 stéréo 44100
    void music(int seq);                            // BGM de zone (maintien)
    void music_fade(int ticks);                     // fondu -> stop (ticks 30 Hz)
    void music_restore(int ticks);                  // rejoue _bgm_zone avec fondu
    int  music_seq() const { return _bgm_seq; }
    void se(int seq);                               // effet (player de la SEQ)
    void se_stop(int seq);
    bool se_busy(int seq) const;
    bool se_any_busy() const;                       // un player SE joue encore
    bool player_busy(int idx) const;                // le player SDAT idx joue
    void fanfare(int seq);                          // ME : met le BGM en pause
    bool fanfare_busy();                            // joue OU queue de 15 sondes
    void fanfare_check();                           // appelé par WaitFanfare
    void cry(int species, int form = 0);            // SEQ_PV + banque=espèce
    bool cry_busy() const;
    void pause_bgm(bool p);
    void stop_all();
    void set_gain(float g) { _gain = g; }
    float gain() const { return _gain; }

private:
    // ---------------- SDAT -------------------------------------------------
    struct SeqRec  { uint16_t file = 0, bank = 0; uint8_t vol = 0,
                     cprio = 0, pprio = 0, player = 0; };
    struct BankRec { uint16_t file = 0, warc[4] = {0xffff,0xffff,0xffff,0xffff}; };
    struct PlyRec  { uint8_t maxseq = 0; uint16_t chmask = 0; };
    struct Region {
        uint8_t type = 0;                    // 1 PCM 2 PSG 3 noise
        uint16_t wave = 0, warc = 0;         // warc = slot 0-3 de la banque
        uint8_t baseKey = 60, a = 0, d = 0, s = 0, r = 0, pan = 64;
        int low = 0, high = 127;
    };
    struct InstrBank { std::vector<std::vector<Region>> progs; };
    struct Wave {
        std::vector<int16_t> s;
        int loopStart = 0; bool loop = false;
        uint16_t timer = 0;
    };
    struct SseqData { const uint8_t* p = nullptr; size_t n = 0; };

    const Bytes* _sd = nullptr;
    std::vector<std::pair<uint32_t,uint32_t>> _fat;
    std::vector<SeqRec>  _seqs;
    std::vector<BankRec> _banks;
    std::vector<PlyRec>  _players;
    std::vector<uint16_t> _warcs;
    std::unordered_map<std::string,int> _seqNames, _bankNames;
    bool _ok = false;

    const uint8_t* _fat_file(int id, uint32_t* size) const;
    SseqData       _sseq(int seq);
    const InstrBank& _bank(int idx);            // décode SBNK (cache)
    const Wave&      _wave(int warcIdx, int w); // décode SWAR/SWAV (cache)
    std::unordered_map<int, std::shared_ptr<InstrBank>> _bankCache;
    std::unordered_map<int, std::shared_ptr<std::vector<Wave>>> _warcCache;

    // ---------------- moteur ----------------------------------------------
    struct Track;
    struct Chan {
        bool active = false;
        int type = 0;                          // 1 PCM 2 PSG 3 noise
        const Wave* w = nullptr;
        double pos = 0;
        int duty = 0;
        uint32_t lfsr = 0x7fff; int noiseOut = 1;
        int key = 60, baseKey = 60, vel = 127;
        int envState = 0, envLevel = -92544;   // (1/10 dB) << 7
        int att = 0, dec = 0, sus = 0, rel = 0;
        int instPan = 0, priority = 0;
        int length = -1;                       // ticks de note restants
        int userVol = 0, userPitch = 0, userPan = 0;
        int sweepPitch = 0, sweepLen = 0, sweepCnt = 0; bool autoSweep = true;
        int lfoType = 0, lfoSpeed = 16, lfoDepth = 0, lfoRange = 1, lfoDelay = 0;
        int lfoCnt = 0, lfoDly = 0;
        Track* trk = nullptr;
        double ampl = 0, left = 0, right = 0, rate = 0;
        bool fresh = false;
    };
    struct Track {
        SseqData d; size_t pc = 0;
        bool active = false;                   // piste ouverte
        bool ended = false;
        int wait = 0;
        struct Fr { size_t pc; int count; };
        Fr stack[3]; int sp = 0;
        int prg = 0, vol = 127, expr = 127, pan = 0;
        int pitchBend = 0, bendRange = 2, prio = 64, transpose = 0;
        int att = 255, dec = 255, sus = 255, rel = 255;
        int lfoType = 0, lfoSpeed = 16, lfoDepth = 0, lfoRange = 1, lfoDelay = 0;
        bool portamento = false; int portaKey = 60, portaTime = 0;
        int sweepPitch = 0;
        bool tie = false, noteWait = true, cmp = true;
        int16_t vars[16];                        // variables locales de piste
        std::vector<Chan*> chans;
    };
    struct Player {
        int idx = -1;                          // index PLAYER du SDAT
        int seq = -1;
        const InstrBank* bank = nullptr;
        int warc[4] = {-1,-1,-1,-1};           // WAVEARC par slot banque
        uint16_t chmask = 0xffff;
        int prio = 0;
        Track trk[16];
        int tempo = 120, tempoCnt = 240;
        int vol = 127, initVol = 127;
        int16_t vars[16];
        bool paused = false;
        int fadeFrom = 127, fadeTo = 127;
        double fadeElapsed = 0; int fadeTicks = 0;
        bool stopAfterFade = false;
        bool playing() const;
    };

    std::unordered_map<int, std::unique_ptr<Player>> _pl;
    Chan _ch[16];
    int16_t _globals[16];
    uint32_t _rand = 0x12345678;
    double _frameClock = 0;                    // accumulateur -> trame ~5,2 ms
    float _gain = 1.0f;
    int _bgm_seq = -1, _bgm_zone = -1;
    int _bgm_player_idx = 7;                   // par défaut (INFO le précisera)
    int _cry_player_idx = 0;
    int _fanfare_tail = 0;                     // sondes restantes après le ME
    std::unordered_map<int,int> _playervol;    // volume par player INFO

    int  _bgm_player() const { return _bgm_player_idx; }
    int  _cry_player() const { return _cry_player_idx; }
    bool _bgm_paused() const {
        auto it = _pl.find(_bgm_player_idx);
        return it != _pl.end() && it->second->paused;
    }

    void   _start(int playerIdx, int seq, int bankOverride = -1);
    void   _stop(Player& p, int ticks);        // ticks>0 = fondu
    Player* _byPlayer(int idx) {
        auto it = _pl.find(idx); return it == _pl.end() ? nullptr : it->second.get();
    }
    void   _frame();                           // une trame son (~5,2095 ms)
    void   _seqFrame(Player& p);
    bool   _stepTrack(Player& p, Track& t);    // true = piste close
    void   _command(Player& p, Track& t);
    void   _noteOn(Player& p, Track& t, int key, int vel, int len);
    Chan*  _alloc(uint16_t mask, int prio);
    void   _free(Chan& c);
    void   _release(Chan& c, int rel);
    void   _releaseTrack(Track& t, int rel);
    void   _closeTrack(Track& t, int rel);
    void   _updateTrackChans(Player& p, Track& t);
    void   _updateChan(Chan& c);
    double _fadeVol(const Player& p) const;
    uint8_t _rb(Track& t);
    int    _varlen(Track& t);
    int16_t& _var(Player& p, Track& t, int i);   // 0-15 piste, 16-31 player, 32+ global
    double _mixSample(Chan& c, double step);
    static bool _pcmStep(Chan& c, double step);

    static int  _dbsq(int i);                  // decibelSquare (1/10 dB)
    static int  _attackRate(int a);
    static int  _fallRate(int v);
    static int  _sinIdx(int x);
    static int  _timerFor(int base, int pitch);
    static double _channelAmp(int db);         // 1/10 dB -> amplitude linéaire
};
