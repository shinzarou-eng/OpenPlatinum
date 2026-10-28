// script.cpp — VM des scripts de terrain DPPt.
// op_table.inc : {opcode, nom, largeurs} généré depuis macros JimB16 + DSPRE.
#include "script.h"
#include <cstdio>
#include <ctime>

static const ScriptVM::OpDef OPS[] = {
#include "op_table.inc"
};

const ScriptVM::OpDef* ScriptVM::opdef(uint16_t op) {
    // OPS est trié : recherche binaire
    int lo = 0, hi = (int)(sizeof(OPS) / sizeof(OPS[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (OPS[mid].op == op) return &OPS[mid];
        if (OPS[mid].op < op) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

static void parse_fns(const Bytes* file, std::vector<size_t>& out) {
    out.clear();
    if (!file || file->size() < 4) return;
    const Bytes& d = *file;
    size_t p = 0;
    while (p + 4 <= d.size()) {
        if (u16(d, p) == 0xFD13) break;
        out.push_back(p + 4 + u32(d, p));
        p += 4;
    }
}

bool ScriptVM::load(const Bytes* file) {
    _f = file; _stack.clear(); _st = DONE;
    parse_fns(file, _fns);
    return !_fns.empty();
}

bool ScriptVM::load_std(const Bytes* file) {
    _stdf = file;
    parse_fns(file, _stdfns);
    return !_stdfns.empty();
}

void ScriptVM::start(int fn) {
    if (fn < 0 || fn >= (int)_fns.size()) { _st = DONE; return; }
    _pc = _fns[fn];
    _stack.clear();
    _st = RUN;
}

int ScriptVM::var(int v) const {
    if (v < 0x4000) return v;                     // constante (TryGetVar)
    if (v >= 0x8000) {                            // work regs (0x800C = var DS normale)
        auto it = _wvars.find(v);
        return it != _wvars.end() ? it->second : 0;
    }
    auto it = _vars.find(v);
    return it != _vars.end() ? it->second : 0;
}
void ScriptVM::set_var(int v, int val) {
    if (v < 0x4000) return;                       // les constantes ne s'écrivent pas
    if (v >= 0x8000) {                            // work regs
        _wvars[v] = val; return;
    }
    _vars[v] = val;
}
bool ScriptVM::flag(int f) const {
    auto it = _flags.find(f);
    return it != _flags.end() && it->second;
}
void ScriptVM::set_flag(int f, bool on) { _flags[f] = on; }

int ScriptVM::_flex(int v) {   // ScriptContext_GetVar : >=0x4000 = variable (TryGetVar)
    return v >= 0x4000 ? var(v) : v;
}
bool ScriptVM::_cok(int op, int r) const {
    switch (op) {
    case 0: return r == 0;             // LESS
    case 1: return r == 1;             // EQUAL
    case 2: return r == 2;             // GREATER
    case 3: return r == 0 || r == 1;   // LESS/EQUAL
    case 4: return r == 2 || r == 1;   // GREATER/EQUAL
    case 5: return r != 1;             // DIFFERENT
    }
    return false;
}
void ScriptVM::_end() {
    if (!_stack.empty()) {
        Frame fr = _stack.back(); _stack.pop_back();
        _f = fr.f; _pc = fr.pc; return;
    }
    _st = DONE;
}

// starters Sinnoh : 0=Tortipouss, 1=Ouisticram, 2=Tiplouf ; le rival prend
// celui qui a l'avantage de type (i+1), Dawn/Lucas le restant (i+2)
int ScriptVM::starter_id(int i) {
    static const int S[3] = {387, 390, 393};
    return S[((i % 3) + 3) % 3];
}
void ScriptVM::starter_pick(int i) {
    i = ((i % 3) + 3) % 3;
    chosen_starter = starter_id(i);
    rival_starter  = starter_id(i + 1);
    cpart_starter  = starter_id(i + 2);
    if (!party.empty()) party.clear();              // nouveau choix -> reset
    _st = RUN;
}
void ScriptVM::_skip_params(const char* w) {
    for (; *w; w++) _pc += (*w == 'b') ? 1 : (*w == 'h') ? 2 : 4;
}

ScriptVM::St ScriptVM::step() {
    if (!_f || _st != RUN) return _st;
    const Bytes& d = *_f;
    int guard = 0;
    while (_st == RUN) {
        if (++guard > 2000 || _pc + 1 >= d.size()) { _st = DONE; break; }
        int pc0 = (int)_pc;
        uint16_t op = rh();
        const OpDef* od = opdef(op);
        trace[trace_n++ % 40] = {pc0, (int)op, od};
        static bool trace = getenv("SCRTRACE") != nullptr;
        if (trace) printf("  [op] %04X %s\n", op, od ? od->name : "?");
        // lit les params génériquement (b/h/w) dans _arg
        _narg = 0;
        if (od) for (const char* w = od->widths; *w && _narg < 8; w++)
            _arg[_narg++] = (*w == 'b') ? rb() : (*w == 'h') ? rh() : rw();
        else { _st = DONE; break; }

        switch (op) {
        case 0x0000: case 0x0001: break;                    // Nop
        case 0x0002: _end(); break;                       // End
        case 0x0003:                                      // WaitTime frames,dest
            set_var((int)_arg[1], 0);
            wait_t = (int)_arg[0]; _st = WAITTIME; break;
        case 0x0015: _end(); break;                       // ExitStandard
        case 0x000B:                                      // CompareRegs (rare)
            result = _cmp(var((int)_arg[0]), var((int)_arg[1])); break;
        case 0x0011:                                      // CompareVarToValue (val littérale)
            result = _cmp(var((int)_arg[0]), (int)_arg[1]); break;
        case 0x0012:                                      // CompareVarToVar
            result = _cmp(var((int)_arg[0]), var((int)_arg[1])); break;
        case 0x0014: {                                    // CallStandard fn -> fichier commun
            int n = (int)_arg[0];
            if (_stdf && n < (int)_stdfns.size()) {
                _stack.push_back({_f, _pc});
                _f = _stdf; _pc = _stdfns[n];
            }
        } break;
        case 0x0016: _rel((int32_t)_arg[0]); break;       // Jump
        case 0x001A:                                      // Call
            _stack.push_back({_f, _pc}); _rel((int32_t)_arg[0]); break;
        case 0x001B: _end(); break;                       // Return
        case 0x001C:                                      // JumpIf cmp,target
            if (_cok((int)_arg[0], result)) _rel((int32_t)_arg[1]); break;
        case 0x001D:                                      // CallIf cmp,target
            if (_cok((int)_arg[0], result)) {
                _stack.push_back({_f, _pc}); _rel((int32_t)_arg[1]); } break;
        case 0x001E: set_flag((int)_arg[0], true); break; // SetFlag
        case 0x001F: set_flag((int)_arg[0], false); break;// ClearFlag
        case 0x0020: result = flag((int)_arg[0]) ? 1 : 0; break; // CheckFlag
        case 0x0021: result = flag(_flex((int)_arg[0])) ? 1 : 0; break; // CheckFlagFromVar
        case 0x0022: set_flag(_flex((int)_arg[0]), true); break;        // SetFlagFromVar
        case 0x0023: set_flag(1364 + _flex((int)_arg[0]), true); break; // SetTrainerFlag
        case 0x0024: set_flag(1364 + _flex((int)_arg[0]), false); break;// ClearTrainerFlag
        case 0x0025: result = flag(1364 + _flex((int)_arg[0])) ? 1 : 0; break; // CheckTrainerFlag
        case 0x0026:                                      // AddValue var,val
            set_var((int)_arg[0], var((int)_arg[0]) + _flex((int)_arg[1])); break;
        case 0x0027:                                      // SubValue
            set_var((int)_arg[0], var((int)_arg[0]) - _flex((int)_arg[1])); break;
        case 0x0028: set_var((int)_arg[0], (int)_arg[1]); break;   // SetVar
        case 0x0029: set_var((int)_arg[0], var((int)_arg[1])); break;
        case 0x002A: set_var((int)_arg[0], _flex((int)_arg[1])); break;
        case 0x002B:                                      // MessageInstant : texte d'un coup, pas d'attente
            if (cb_msg) cb_msg((int)_arg[0], true); break;
        case 0x002C: case 0x002E:
        case 0x002F:                                      // Message*/MessageNoSkip
            if (cb_msg) cb_msg((int)_arg[0], false);
            _st = MSG; break;
        case 0x002D:                                      // MessageVar (id via GetVar)
            if (cb_msg) cb_msg(_flex((int)_arg[0]), false);
            _st = MSG; break;
        case 0x0030: case 0x0031: case 0x0032:            // WaitAB/WaitButton
            _st = WAITBTN; break;
        case 0x0034:                                      // CloseMsgOnKeyPress : touche -> ferme + continue
            _st = WAITBTN; close_btn = true; break;
        case 0x0033: if (cb_box) cb_box(true); break;     // OpenMessage
        case 0x0035: if (cb_box) cb_box(false); break;    // CloseMessage
        case 0x0037: case 0x0038: break;                  // DrawSignpostTextBox/SetSignpostCmd
        case 0x0036:                                      // DrawSignpostInstantMessage (bbhh)
            if (cb_msg) cb_msg((int)_arg[0], true);       //  affiché, pas d'attente
            break;
        case 0x0039: _st = WAITBTN; break;                // WaitForSignpostDone
        case 0x003A:                                      // DrawSignpostScrolling msg,dest
            if (cb_msg) cb_msg((int)_arg[0], false);
            signpost_dest = (int)_arg[1]; _st = WAITBTN; break;
        case 0x003B:                                      // GetSignpostInput dest
            signpost_dest = (int)_arg[0]; _st = WAITBTN; break;
        case 0x003E: pending_var = (int)_arg[0]; _st = YESNO; break;
        case 0x005E:                                      // ApplyMovement obj,off
            if (cb_move) cb_move((int)_arg[0], _pc + (int32_t)_arg[1]);
            break;
        case 0x005F: _st = MOVE; break;                   // WaitMovement
        case 0x0040: case 0x0041: case 0x0044: case 0x0045: { // Init[Local|Global][Text]Menu
            menu_items.clear();
            menu_dest = (int)_arg[4];                       // var de sélection
            menu_cancel = (int)_arg[3];                     // slot du bouton annuler
            // DS field_menu.c : InitGlobalTextMenu (0x40) passe loader=NULL ->
            // TEXT_BANK_MENU_ENTRIES (361) ; les variantes locales utilisent
            // la banque du script.
            menu_bank = (op == 0x0040 || op == 0x0044) ? 361 : -1;
        } break;
        case 0x0042:                                      // AddMenuEntryImm text,slot
            menu_items.push_back({(int)_arg[1], (int)_arg[0]}); break;
        case 0x0046:                                      // AddListMenuEntry msg,hi,slot
            menu_items.push_back({(int)_arg[2], (int)_arg[0]}); break;
        case 0x0043: case 0x0047:                         // ShowMenu/ShowListMenu
            _st = MENU; break;
        case 0x003C: break;                               // ShowStartMenu (menu X)
        case 0x0049:                                      // PlaySE seq
            if (cb_audio) cb_audio(0x49, (int)_arg[0]); break;
        case 0x004A:                                      // StopSE seq
            if (cb_audio) cb_audio(0x4A, (int)_arg[0]); break;
        case 0x004B: wait_snd = 1; _st = WAITSND; break;  // WaitSE
        case 0x0050:                                      // PlayMusic seq
            if (cb_audio) cb_audio(0x50, (int)_arg[0]); break;
        case 0x0051:                                      // StopMusic
            if (cb_audio) cb_audio(0x51, 0); break;
        case 0x0052:                                      // PlayDefaultBGM
            if (cb_audio) cb_audio(0x52, 0); break;
        case 0x0053: break;                               // (réservé)
        case 0x0054:                                      // FadeOutBGM frames
            if (cb_audio) cb_audio(0x54, (int)_arg[0]); break;
        case 0x0057:                                      // SetBGM seq (mémorisé)
            if (cb_audio) cb_audio(0x57, (int)_arg[0]); break;
        case 0x00A1: break;                               // ReturnToField (suite)
        case 0x00BC:                                      // FadeScreen t,f,type,col
            if (cb_fade) cb_fade((int)_arg[0], (int)_arg[1],
                (int)_arg[2], (int)_arg[3]);
            break;
        case 0x00BD: _st = WAITFADE; break;               // WaitFadeScreen
        case 0x0069:                                      // GetPlayerMapPos x,z
            if (cb_pos) { set_var((int)_arg[0], cb_pos(0));
                set_var((int)_arg[1], cb_pos(1)); } break;
        case 0x0060: if (cb_misc) cb_misc(1); break;      // LockAll : PNJ gelés
        case 0x0061: if (cb_misc) cb_misc(2); break;      // ReleaseAll
        case 0x01C5: break;                               // ShowMoveInfo (nd)
        case 0x0250: break;                               // InitSizeContestRecord (nd)
        case 0x027B: break;                               // InitDailyRandomLevel (nd)
        case 0x0063: if (cb_lockobj) cb_lockobj((int)_arg[0], false); break; // Release
        case 0x0064: case 0x01B1:                         // AddPeople[2] id
            if (cb_people) cb_people((int)_arg[0], true); break;
        case 0x0065: case 0x01B2:                         // RemovePeople[2] id
            if (cb_people) cb_people((int)_arg[0], false); break;
        case 0x0066: case 0x0067: break;                  // LockCam/ZoomCam
        case 0x0068: if (cb_face) cb_face(); break;       // FacePlayer
        // ── portes animées (SCRCMD_LOADDOORANIMATION…) — ov5_021D431C.c ────
        case 0x0168:                                      // LoadDoorAnimation
            if (cb_doorload) cb_doorload((int)_arg[0], (int)_arg[1],
                _flex((int)_arg[2]), _flex((int)_arg[3]), (int)_arg[4]);
            break;
        case 0x0169:                                      // WaitForAnimation tag
            wait_anim = (int)_arg[0]; _st = WAITANIM; break;
        case 0x016A:                                      // UnloadAnimation tag
            if (cb_doorfree) cb_doorfree((int)_arg[0]); break;
        case 0x016B:                                      // PlayDoorOpenAnim tag
            if (cb_dooranim) cb_dooranim((int)_arg[0], true); break;
        case 0x016C:                                      // PlayDoorCloseAnim tag
            if (cb_dooranim) cb_dooranim((int)_arg[0], false); break;
        case 0x007B:                                      // AddItem i,c,dest
            bag_add((int)_arg[0], (int)_arg[1]);
            set_var((int)_arg[2], 1); break;
        case 0x007C: {                                    // RemoveItem i,c,dest
            bool ok = bag_count((int)_arg[0]) >= (int)_arg[1];
            if (ok) bag_add((int)_arg[0], -(int)_arg[1]);
            set_var((int)_arg[2], ok ? 1 : 0);
        } break;
        case 0x007D:                                      // CanFitItem -> tjs OK
            set_var((int)_arg[2], 1); break;
        case 0x007E:                                      // CheckItem i,c,dest
            set_var((int)_arg[2], bag_count((int)_arg[0]) >= (int)_arg[1] ? 1 : 0);
            break;
        case 0x007F:                                      // IsItemTMHM i,dest
            set_var((int)_arg[1],
                ((int)_arg[0] >= 328 && (int)_arg[0] <= 427) ? 1 : 0); break;
        case 0x0080: set_var((int)_arg[1], 0); break;     // GetItemPocket (nd)
        case 0x0083: case 0x0087: case 0x008F:            // SendGoodToPC/GiveTrap/GiveSphere
            set_var((int)_arg[2], 1); break;
        case 0x02A7:                                      // CheckPlate i,dest
            set_var((int)_arg[1], bag_count((int)_arg[0]) > 0 ? 1 : 0); break;
        case 0x0236: break;                             // Cmd_236 (nc)
        // ── buffers StringTemplate : (slot, kind, val) ──────────────────────
        case 0x00CD: case 0x00CE: case 0x00CF:            // BufferPlayer/Rival/Counterpart
            if (cb_buf) cb_buf((int)_arg[0], op - 0xCD, 0); break;
        case 0x00D0: case 0x00D6: {                     // BufferPartyMonSpecies/Nickname
            int s = _flex((int)_arg[1]);                // slot équipe (via GetVar)
            int sp = (s >= 0 && s < (int)party.size()) ? party[s].species : 0;
            if (cb_buf) cb_buf((int)_arg[0], 30, sp); break; }
        case 0x00D8: case 0x00D9:                       // BufferTrainerClass*(nd)
            if (cb_buf) cb_buf((int)_arg[0], 20, _flex((int)_arg[1])); break;
        case 0x00DA:                                    // BufferSpeciesNameFromVar
            if (cb_buf) cb_buf((int)_arg[0], 30, _flex((int)_arg[1])); break;
        case 0x00D1:                                      // BufferItemName
            if (cb_buf) cb_buf((int)_arg[0], 10, _flex((int)_arg[1])); break;
        case 0x00D2:                                      // BufferPocketName
            if (cb_buf) cb_buf((int)_arg[0], 50, _flex((int)_arg[1])); break;
        case 0x00D3:                                      // BufferTMHMMoveName (item)
            if (cb_buf) cb_buf((int)_arg[0], 41, _flex((int)_arg[1])); break;
        case 0x00D4:                                      // BufferMoveName
            if (cb_buf) cb_buf((int)_arg[0], 40, _flex((int)_arg[1])); break;
        case 0x00D5:                                      // BufferNumber
            if (cb_buf) cb_buf((int)_arg[0], 20, _flex((int)_arg[1])); break;
        case 0x00D7:                                      // BufferPoketchAppName
            if (cb_buf) cb_buf((int)_arg[0], 60, _flex((int)_arg[1])); break;
        case 0x00DF: case 0x00E0: case 0x00E1:            // BufferUG goods/trap/treasure
            if (cb_buf) cb_buf((int)_arg[0], 70 + op - 0xDF, _flex((int)_arg[1])); break;
        case 0x00E2:                                      // BufferMapName
            if (cb_buf) cb_buf((int)_arg[0], 80, _flex((int)_arg[1])); break;
        case 0x033C:                                      // BufferItemNameWithArticle
            if (cb_buf) cb_buf((int)_arg[0], 11, _flex((int)_arg[1])); break;
        case 0x014E: if (cb_heal) cb_heal(); break;       // HealParty
        case 0x015A: if (cb_misc) cb_misc(0); break;      // GiveRunningShoes
        case 0x0186:                                      // SetObjectEventPos o,x,z
            if (cb_evpos) cb_evpos((int)_arg[0], (int)_arg[1], (int)_arg[2]);
            break;                                       // -> event template
        case 0x0187:                                      // SetPosition o,x,y,z,dir
            if (cb_setpos) cb_setpos((int)_arg[0], (int)_arg[1], (int)_arg[3],
                (int)_arg[4], (int)_arg[2]);
            break;                                       // -> objet vivant
        case 0x0188: case 0x0189:                         // SetObjectEventMoveType/Dir
            if (cb_ow) cb_ow((int)_arg[0], op - 0x188, (int)_arg[1]); break;
        case 0x018B:                                      // SetBgEventPos idx,x,z
            if (cb_bgev) cb_bgev((int)_arg[0], (int)_arg[1], (int)_arg[2]);
            break;
        case 0x01B7:                                      // GetRandom range,dest
            set_var((int)_arg[1], rand() % (int)(_arg[0] ? _arg[0] : 1)); break;
        case 0x01BD:                                      // GetPlayerDir dest
            if (cb_pdir) set_var((int)_arg[0], cb_pdir()); break;
        case 0x01CC: set_flag(2409, true); break;         // GiveJournal -> FLAG_JOURNAL_ACQUIRED
        case 0x022D:                                      // NatDex enable/check
            if ((int)_arg[0] == 1) set_flag(144, true);
            else set_var((int)_arg[1], flag(144) ? 1 : 0);
            break;
        case 0x0283: break;                               // SetInitialVolumeForSequence
        case 0x01B6: {                                    // CheckTime -> période DS
            time_t t = time(nullptr); int h = localtime(&t)->tm_hour;
            int p = h >= 20 ? 3 : h >= 17 ? 2 : h >= 10 ? 1 : h >= 4 ? 0 : 4;
            set_var((int)_arg[0], p);
        } break;
        case 0x00BE:                                      // Warp hdr,?,x,z,dir (GetVar)
            if (cb_warp) cb_warp(_flex((int)_arg[0]), _flex((int)_arg[2]),
                _flex((int)_arg[3]), _flex((int)_arg[4]));
            _end(); break;
        case 0x014D: set_var((int)_arg[0], 0); break;     // CheckGender (garçon)
        case 0x000A: break;                               // AdrsAdrsSet (unused)
        case 0x0062: if (cb_lockobj) cb_lockobj((int)_arg[0], true); break;   // Lock
        case 0x006C: break;                               // SetObjectFlagIsPersistent
        case 0x006D:                                      // SetMovementType obj,type
            if (cb_ow) cb_ow(_flex((int)_arg[0]), 2, _flex((int)_arg[1]));
            break;
        case 0x006E: break;                               // unused
        // -- argent / jetons (TrainerInfo DS) --
        case 0x006F: money += (uint32_t)_arg[0]; break;   // GiveMoney u32
        case 0x0070: money = money > (uint32_t)_arg[0] ? money - (uint32_t)_arg[0] : 0; break; // RemoveMoney
        case 0x0071: set_var((int)_arg[0], money >= (uint32_t)_arg[1] ? 1 : 0); break; // CheckMoney
        case 0x0072: case 0x0073: case 0x0074: break;     // ShowMoney/HideMoney/Update
        case 0x0075: case 0x0076: case 0x0077: break;     // ShowCoins/HideCoins/Update
        case 0x0078: set_var((int)_arg[0], coins); break; // GetCoinsAmount
        case 0x0079: coins += _flex((int)_arg[0]); break; // AddCoins
        case 0x007A: coins -= _flex((int)_arg[0]); if (coins < 0) coins = 0; break; // SubtractCoins
        // -- audio : cris + fanfares (SDAT) --
        case 0x004C:                                      // PlayCry espèce,forme?
            if (cb_audio) cb_audio(0x4C, (int)_arg[0]); break;
        case 0x004D: wait_snd = 2; _st = WAITSND; break;  // WaitCry
        case 0x004E:                                      // PlayFanfare seq
            if (cb_audio) cb_audio(0x4E, (int)_arg[0]); break;
        case 0x004F: wait_snd = 3; _st = WAITSND; break;  // WaitFanfare
        // -- poketch --
        case 0x0131: sys_poketch = true; break;           // EnablePoketch
        case 0x0132: set_var((int)_arg[0], sys_poketch ? 1 : 0); break; // CheckPoketchEnabled
        case 0x0133: break;                               // RegisterPoketchApp
        case 0x0134: set_var((int)_arg[1], 0); break;     // CheckPoketchAppRegistered
        // -- pokedex / sac / chaussures / badges --
        case 0x0157: set_var((int)_arg[0], flag(146) ? 1 : 0); break; // CheckPokedexAcquired
        case 0x0158: set_flag(146, true); break;          // GivePokedex
        case 0x0159: set_var((int)_arg[0], cb_runstate ? cb_runstate() : 0); break; // CheckRunningShoesAcquired
        case 0x015B: set_var((int)_arg[1], (badges >> (_flex((int)_arg[0]) & 7)) & 1); break; // CheckBadgeAcquired
        case 0x015C: badges |= 1 << (_flex((int)_arg[0]) & 7); break; // GiveBadge
        case 0x015D: { int n = 0; for (int i = 0; i < 8; i++) n += (badges >> i) & 1;
            set_var((int)_arg[0], n); } break;            // CountBadgesAcquired
        case 0x015E: set_var((int)_arg[0], flag(2406) ? 1 : 0); break; // CheckBagAcquired
        case 0x015F: set_flag(2406, true); break;         // GiveBag
        case 0x0160: set_var((int)_arg[0], flag(2407) ? 1 : 0); break; // CheckHasPartner
        case 0x0161: set_flag(2407, true);                // SetHasPartner
            if (cb_follow) { cb_follow(4, 0, 0); } break;
        case 0x0162: set_flag(2407, false);               // ClearHasPartner
            if (cb_follow) { cb_follow(5, 0, 0); } break;
        case 0x0163: set_var((int)_arg[0], flag(2411) ? 1 : 0); break; // CheckStepFlag
        case 0x0164: set_flag(2411, true); break;         // SetStepFlag
        case 0x0165: set_flag(2411, false); break;        // ClearStepFlag
        case 0x0166: set_var((int)_arg[0], flag(2410) ? 1 : 0); break; // CheckGameCompleted
        case 0x0167: set_flag(2410, true); break;         // SetGameCompleted
        // -- divers système --
        case 0x00C7: set_var((int)_arg[0], sys_bike ? 1 : 0); break; // CheckPlayerOnBike
        case 0x00C8: sys_bike = _flex((int)_arg[0]) != 0; break;      // SetPlayerBike
        case 0x00C9: break;                               // ForceBicycling
        case 0x011C: set_var((int)_arg[0], 0); break;     // GetFloorsAbove
        case 0x012E: set_var((int)_arg[1], 0); break;     // DressUpPhotoHasData
        case 0x00A7: break;                               // ShowDressUpPhoto
        case 0x0085: set_var((int)_arg[2], 1); break;     // CheckHasRoomForGoodsInPC
        case 0x009A: set_var((int)_arg[0],              // FindPartySlotWithMove
            cb_findmove ? cb_findmove(_flex((int)_arg[1])) : 0xFF); break;
        // ── Poké Mart : stocks authentiques (include/data/mart_items.h) ──
        case 0x0147: {                                  // PokeMartCommon -> badge tier
            static const struct { uint16_t item, req; } COMMON[] = {
                {4,1},{3,3},{2,4},{17,1},{26,2},{25,4},{24,5},{23,6},
                {28,3},{18,1},{22,1},{21,2},{19,2},{20,2},{27,4},
                {78,2},{79,2},{76,3},{77,4} };
            int nb = 0; for (int i = 0; i < 8; i++) nb += (badges >> i) & 1;
            int req = nb <= 0 ? 1 : nb <= 2 ? 2 : nb <= 4 ? 3 : nb <= 6 ? 4 : nb == 7 ? 5 : 6;
            shop_items.clear();
            for (auto& e : COMMON) if (e.req <= req) shop_items.push_back(e.item);
            _st = SHOP; break; }
        case 0x0148: {                                  // PokeMartSpecialties id
            static const uint16_t SPEC[][12] = {
                {146,14,0},{141,14,6,0},{140,14,6,0},{146,14,6,8,0},      // 0-3 Jub/Ore/Flo/Eterna
                {36,34,35,37,0},{143,14,6,8,0},{146,6,8,13,0},{146,8,13,15,0}, // 4-7 Herbe/Heart/Sola/Pastoria
                {17,26,25,24,28,18,22,19,20,21,27,0},                     // 8 Veil1F-R
                {4,3,2,78,63,79,76,77,137,138,139,145},                   // 9 Veil1F-L
                {59,57,58,55,56,60,61,62,0},{46,47,49,52,48,45,0},        // 10-11 Veil2F
                {410,344,381,347,360,343,397,0},{365,352,341,349,379,342,0}, // 12-13 Veil3F
                {146,13,15,10,0},{144,13,15,10,0},{146,15,10,9,0},        // 14-16 Celestic/Snow/Canalave
                {142,11,0},{14,6,8,13,15,10,9,11,0},{159,160,161,162,163,0} }; // 17-19 Suny/League/B1F
            int id = _flex((int)_arg[0]);
            shop_items.clear();
            if (id >= 0 && id < 20)
                for (int i = 0; i < 12 && SPEC[id][i]; i++) shop_items.push_back(SPEC[id][i]);
            _st = SHOP; break; }
        case 0x0149: case 0x014A: shop_items.clear(); _st = SHOP; break; // Decor/Seal (UG nd)
        case 0x0154: break;                               // LoadTrainerAppearances
        case 0x0155: set_var((int)_arg[1], 0); break;     // GetTrainerInfoTrainerClass
        case 0x0156: break;                               // SetTrainerInfoAppearance
        case 0x0190: wait_t = _flex((int)_arg[0]); _st = WAITTIME; break; // WaitABPressTime
        case 0x0198: {                                    // GetPartyMonSpecies slot,dest
            int sl = var((int)_arg[0]);
            set_var((int)_arg[1], (sl >= 0 && sl < (int)party.size()) ? party[sl].species : 0); }
            break;
        case 0x019B: {                                    // CountAliveMonsExcept dest,slot
            int ex = _flex((int)_arg[1]), n = 0;
            for (int i = 0; i < (int)party.size(); i++)
                if (i != ex && party[i].hp > 0) n++;
            set_var((int)_arg[0], n); } break;
        case 0x01B9: set_var((int)_arg[0], 70); break;    // GetPartyMonFriendship
        case 0x01BA: break;                               // IncreasePartyMonFriendship
        case 0x01C0: {                                    // CheckPartyHasSpecies dest,sp
            int sp = _flex((int)_arg[1]), ok = 0;
            for (auto& m : party) if (m.species == sp) ok = 1;
            set_var((int)_arg[0], ok); } break;
        case 0x01CD: break;                               // CreateJournalEvent
        case 0x01DD: break;                               // CallBattleTowerFunction
        case 0x01F1: set_var((int)_arg[0], 0); break;     // GetFossilCount
        case 0x01F4: set_var((int)_arg[0], 0); break;     // GetSpeciesFromFossil
        case 0x01F5: set_var((int)_arg[0], 0); break;     // FindFossilAtThreshold
        case 0x0201: set_var((int)_arg[0], cb_mapid ? cb_mapid() : 0); break; // GetCurrentMapID
        case 0x0202: break;                               // StartEndSafariGame
        case 0x0238: set_var((int)_arg[1], 0); break;     // CheckTVInterviewEligible
        case 0x0247: set_var((int)_arg[0], party.empty() ? 0 : 0); break; // GetFirstNonEggInParty
        case 0x0260: break;                               // IncrementTrainerScore2
        case 0x0285: break;                               // InitTurnBackCave
        case 0x02B5: break;                               // Cmd_2B5
        case 0x02BC: set_var((int)_arg[0], 0); break;     // CheckDidNotCapture
        case 0x02BD:                                      // StartLegendaryBattle sp,lv
            battle_sp = _flex((int)_arg[0]); battle_lv = _flex((int)_arg[1]);
            _st = BATTLE; break;
        case 0x0124:                                      // StartWildBattle sp,lv
            battle_sp = _flex((int)_arg[0]); battle_lv = _flex((int)_arg[1]);
            _st = BATTLE; break;
        case 0x0129:                                      // StartHoneyTreeBattle (nd)
            _st = BATTLE; break;
        case 0x02F2: break;                               // InitDistortionWorldFeatures
        case 0x0334: break;                               // AddToGameRecord
        case 0x0096: {                                            // GivePokemon
            PartyMon m{_flex((int)_arg[0]), _flex((int)_arg[1]), -1};
            party.push_back(m);                                   // (objet tenu _arg[2] ignoré)
            if (trace) printf("  [party] +species %d lv%d (n=%zu)\n",
                m.species, m.level, party.size());
            set_var((int)_arg[3], 1); break; }                    // succès -> varptr
        case 0x00B4: _st = STARTER; break;                  // StartChooseStarterScene
        case 0x00B5: set_var(0x4032, chosen_starter); break;      // SaveChosenStarter
        case 0x00DB: if (cb_buf) cb_buf((int)_arg[0], 30, chosen_starter); break;
        case 0x00DC: if (cb_buf) cb_buf((int)_arg[0], 30, rival_starter); break;
        case 0x00DD: if (cb_buf) cb_buf((int)_arg[0], 30, cpart_starter); break;
        case 0x00DE: set_var((int)_arg[0], var(0x4032)); break;   // GetPlayerStarterSpecies
        case 0x0125:                                    // StartFirstBattle tid=GetVar
            battle_tid = _flex((int)_arg[0]); _st = BATTLE; break;
        case 0x00E4: set_var((int)_arg[0],                // GetTrainerID
            cur_script - (cur_script < 5000 ? 2999 : 4999)); break;
        case 0x00E5:                                    // StartTrainerBattle
            battle_tid = _flex((int)_arg[0]); _st = BATTLE; break;
        case 0x00E6:                                    // PrintTrainerDialogue tid,type
            if (cb_trainermsg) cb_trainermsg(_flex((int)_arg[0]), _flex((int)_arg[1]));
            _st = MSG; break;
        case 0x00E7: case 0x00E8: {                     // Get(Rematch)TrainerMessageTypes
            int tid = cur_script - (cur_script < 5000 ? 2999 : 4999);
            int dbl = cb_trdata ? cb_trdata(tid) : 0, b = cur_script >= 5000;
            set_var((int)_arg[0], dbl ? (b ? 7 : 3) : 0);
            set_var((int)_arg[1], dbl ? (b ? 9 : 5) : 2);
            set_var((int)_arg[2], dbl ? (b ? 10 : 6) : 0);
            break; }
        case 0x00E9: {                                  // CheckIsTrainerDoubleBattle
            int tid = cur_script - (cur_script < 5000 ? 2999 : 4999);
            set_var((int)_arg[0], cb_trdata ? (cb_trdata(tid) != 0) : 0); break; }
        case 0x00EA:                                    // PlayTrainerEncounterBGM
            if (cb_audio) cb_audio(0xEA, (int)_arg[0]); break;
        case 0x00EC: set_var((int)_arg[0], !battle_lost); break;  // CheckWonBattle
        case 0x00ED: set_var((int)_arg[0], battle_lost); break;   // CheckLostBattle
        case 0x00EE: set_var((int)_arg[0], 1); break;     // CheckHasTwoAliveMons (nd)
        case 0x00EF: battle_lost = 0; break;            // StartDummyTrainerBattle
        case 0x00F0: if (cb_targetflag) cb_targetflag(1); break;  // SetTargetTrainerDefeated
        case 0x00F1:                                    // GoToIfTargetTrainerDefeated
            if (cb_targetflag && cb_targetflag(0)) _rel((int32_t)_arg[0]);
            break;
        case 0x02AD:                                    // GetMovementType dest,objRef
            set_var((int)_arg[0], cb_objmt ? cb_objmt(_flex((int)_arg[1])) : 0); break;
        case 0x009B: set_var((int)_arg[1], 0); break;     // GetRematchTrainerID -> TRAINER_NONE
        case 0x020B: break;                             // SetMoveCodeForFacingDirection (nd)
        case 0x00EB: if (cb_lostpc) cb_lostpc(); _end(); break;   // BlackOutFromBattle
        case 0x014B: if (cb_lostpc) cb_lostpc(); _end(); break;   // DefeatGoPokecenter (idem)
        case 0x0208: if (cb_pic) cb_pic((int)_arg[0], true); break;  // PokemonPicture
        case 0x0209: if (cb_pic) cb_pic(0, false); break;          // HidePicture
        case 0x021C: break;                               // ActSwarmPoke
        case 0x018A:                                      // SetWarpEventPos idx,x,z
            if (cb_warpev) cb_warpev((int)_arg[0], (int)_arg[1],
                (int)_arg[2]); break;                    // -> event template
        // ---- queries équipe / dex / RTC / état joueur ----
        case 0x0177: set_var((int)_arg[0], (int)party.size()); break; // GetPartyCount
        case 0x0193: set_var((int)_arg[0], 0xFFFF); break; // GetSelectedPartySlot
        case 0x0278: {                                    // GetPartyMonLevel dest,slot
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0],
                (s >= 0 && s < (int)party.size()) ? party[s].level : 0);
            break; }
        case 0x0262: {                                  // CheckPartyHasSpecies2 sp,dest
            int sp = _flex((int)_arg[0]), has = 0;
            for (auto& m : party) if (m.species == sp) has = 1;
            set_var((int)_arg[1], has); break; }
        case 0x011E: set_var((int)_arg[0], 0); break;   // GetLocalDexSeenCount
        case 0x01E8: set_var((int)_arg[0], 0); break;   // CheckLocalDexCompleted
        case 0x01E9: set_var((int)_arg[0], 0); break;   // CheckNationalDexCompleted
        case 0x0284: set_var((int)_arg[0], 0); break;   // GetUnownFormsSeenCount
        case 0x02B7: set_var((int)_arg[0], 0); break;   // CheckPartyHasBadEgg
        case 0x026B: set_var((int)_arg[0], 0); break;   // CheckHasAllLegendaryTitansInParty
        case 0x032B: set_var((int)_arg[0], 0); break;   // CheckPartyHasFatefulEncounterRegigigas
        case 0x031C: set_var((int)_arg[0], 0xFFFF); break; // FindPartySlotWithFatefulEncounterSpecies
        case 0x0303: set_var((int)_arg[0], 0); set_var((int)_arg[1], 0); break; // GetPartyRotomCountAndFirst
        case 0x0302: for (int i = 0; i < 5; ++i) set_var((int)_arg[i], 0); break; // GetRotomFormsInSave
        case 0x0234: { time_t t = time(nullptr);        // GetDayOfWeek (RTC)
            set_var((int)_arg[0], localtime(&t)->tm_wday); break; }
        case 0x0268: { time_t t = time(nullptr);        // GetHour (RTC)
            set_var((int)_arg[0], localtime(&t)->tm_hour); break; }
        case 0x00CB: break;                             // SetPlayerState (nd)
        case 0x00CC: break;                             // ChangePlayerState (nd)
        // ---- rubans / EV / concours / presse ----
        case 0x022F: set_var((int)_arg[0], 0); break;   // CountPartyRibbons
        case 0x0230: set_var((int)_arg[0], 0); break;   // GetPartyMonRibbon dest,slot,ribbon
        case 0x0231: break;                             // SetPartyMonRibbon
        case 0x0233: set_var((int)_arg[0], 0); break;   // GetPartyMonEvTotal
        case 0x012F: set_var((int)_arg[1], 0); break;   // ContestPhotoHasData bank,dest
        case 0x01C1: set_var((int)_arg[0], 0); break;   // CalcSizeContestResult dest,slot
        case 0x01C2: break;                             // UpdateSizeContestRecord
        case 0x01C3: break;                             // BufferPartyPokemonSize
        case 0x0218: set_var((int)_arg[0], 403); break; // GetRandomSeenSpecies
        case 0x0219: break;                             // SetNewsPressDeadline
        case 0x0277: set_var((int)_arg[0], rand() % 46 + 5); break; // GetDailyRandomLevel
        case 0x01B8: set_var((int)_arg[0],             // GetRandom2 dest,max
            _flex((int)_arg[1]) ? rand() % _flex((int)_arg[1]) : 0); break;
        // ---- trades PNJ / tutors / menus party ----
        case 0x0191: break;                             // SelectMoveTutorPokemon
        case 0x0224: break;                             // OpenMoveTutorMenu
        case 0x0225: set_var((int)_arg[0], 0); break;   // CheckLearnedTutorMove
        case 0x0226: break;                             // InitNpcTrade b
        case 0x0228: set_var((int)_arg[0], 0); break;   // GetNpcTradeRequestedSpecies
        case 0x0229: break;                             // StartNpcTrade
        case 0x022A: break;                             // FinishNpcTrade
        case 0x02A5: break;                             // OpenPartyMenuForTrade
        // ---- centres comm / underground / wifi ----
        case 0x013C: break;                             // DoUnionRoomGreeting
        case 0x0140: set_var((int)_arg[0], 0); break;   // 140
        case 0x0141: break;                             // 141
        case 0x0142: break;                             // 142
        case 0x0146: set_var((int)_arg[1], 0); break;   // 146
        case 0x014F: break;                             // Dummy14F
        case 0x0152: break;                             // SetCommPlayerDir
        case 0x019F: break;                             // UndergroundNpcMessage
        case 0x01A0: break;                             // CloseUndergroundNpcMessage
        case 0x02E2: break;                             // OpenScratchOffCardsApp
        case 0x0329: for (int i = 0; i < 4; ++i) set_var((int)_arg[i], 0); break; // GetBattleFrontierReporterPosition
        case 0x030E: set_var((int)_arg[0], 0); break;   // 30E
        case 0x0207: set_var((int)_arg[0], 0); break;   // GetCurNetID
        case 0x02AA: set_var((int)_arg[0], 0); break;   // CheckIsMysteryGiftPhrase
        case 0x0249: set_var((int)_arg[0], 0); break;   // GetWallpaperFromCustomMessageWords
        case 0x0244: for (int i = 1; i < 4; ++i) set_var((int)_arg[i], 0); break; // ChooseTwoCustomMessageWords
        case 0x028B: set_var((int)_arg[1], 0); break;   // CheckDistributionEvent
        // ---- menus génériques / buffers texte ----
        case 0x029D: menu_items.push_back({(int)_arg[1],
            _flex((int)_arg[0])}); break;               // AddMenuEntry
        case 0x0280: if (cb_buf) cb_buf((int)_arg[0], 13,
            _flex((int)_arg[1])); break;                // BufferVarPaddingDigits
        case 0x0261: break;                             // BufferAccessoryName (nd)
        case 0x0343: break;                             // BufferAccessoryNameWithArticle (nd)
        case 0x0344: break;                             // BufferTrainerClassNameWithArticle (nd)
        case 0x0346: break;                             // CapitalizeFirstLetter
        case 0x0272: break;                             // BufferTabletName
        case 0x02EF: break;                             // BufferStatName
        case 0x033A: break;                             // SetMenuXOriginSide
        case 0x033B: break;                             // SetMenuYOriginSide
        // ---- RTC/events objets divers ----
        case 0x0055: break;                             // FadeInBGM
        case 0x00C5: break;                             // PlayHmCutIn
        case 0x0151: break;                             // FieldCommenterBattleRoom
        case 0x0211: break;                             // SetPlayerHeightCalculationEnabled
        case 0x0270: break;                             // SetHiddenLocation
        case 0x0333: break;                             // SetPlayerVolume
        case 0x0269: break;                             // ShakeObject
        case 0x026D: break;                             // MessageUnown
        case 0x02C0: break;                             // MessageAutoScroll
        case 0x026E: set_var((int)_arg[0], 0); break;   // GetGBACartridgeVersion
        case 0x027D: set_var((int)_arg[0], 0); break;   // TryBufferAndUnlockRandomToughWord
        case 0x027F: set_var((int)_arg[0], 0); break;   // CheckAllToughWordsUnlocked
        case 0x0271: break;                             // OpenShayminTabletNamingScreen
        // ---- centre Pokémon / dépôt / jeu ----
        case 0x027E: set_var((int)_arg[0], 1); break;   // CheckIsDepartmentStoreRegular
        case 0x0276: set_var((int)_arg[0], 1); break;   // CheckCanAddCoins
        case 0x01B5: break;                             // StartRankingsMachine
        // ---- parcs / pal park / catching show ----
        case 0x0255: break;                             // MoveCatchingShowMonsToPcBoxes
        case 0x0256: set_var((int)_arg[1], 0); break;   // CalcCatchingShowPoints
        case 0x0257: break;                             // ShowAccessoryShop
        case 0x029B: break;                             // GetExchangeServiceCornerItemAndCost
        case 0x0210: set_var((int)_arg[1], 0); break;   // CheckGreatMarshTramLocation
        case 0x020F: set_var((int)_arg[0], 0); break;   // MoveGreatMarshTram
        // ---- mécaniques de gym / lifts ----
        case 0x016F: break;                             // InitPersistedMapFeaturesForPastoriaGym
        case 0x0170: break;                             // PressPastoriaGymButton
        case 0x0171: break;                             // InitPersistedMapFeaturesForHearthomeGym
        case 0x0172: break;                             // MoveHearthomeGymDpLift
        case 0x0175: break;                             // InitPersistedMapFeaturesForSunyshoreGym
        case 0x0176: break;                             // PressSunyshoreGymButton
        case 0x02C9: break;                             // InitPersistedMapFeaturesForEternaGym
        case 0x02CA: break;                             // AdvanceEternaGymClock
        case 0x025B: break;                             // InitPersistedMapFeaturesForPlatformLift
        case 0x025C: break;                             // TriggerPlatformLift
        case 0x025D: set_var((int)_arg[0], 0); break;   // CheckPlatformLiftNotUsedWhenEnteredMap
        // ---- battlefield / tower / castle / arcade ----
        case 0x01DA: break;                             // SetBattleTowerNull
        case 0x01DF: set_var((int)_arg[1], 0); break;   // 1DF hw,ptr
        case 0x01E0: set_var((int)_arg[1], 0); break;   // 1E0
        case 0x01E3: set_var((int)_arg[0], 0);
            set_var((int)_arg[2], 0); break;            // 1E3 ptr,hw,ptr,hw
        case 0x01E4: set_var((int)_arg[0], 0); break;   // 1E4 ptr,hw
        case 0x01E6: set_var((int)_arg[3], 0);
            set_var((int)_arg[4], 0); break;            // 1E6
        case 0x01E7: break;                             // 1E7
        case 0x01DE: break;                             // GetBattleTowerPartnerSpeciesAndMove (nd)
        case 0x02CC: set_var((int)_arg[2], 0); break;   // CallBattleHallLobbyFunction
        case 0x02D2: set_var((int)_arg[2], 0); break;   // CallBattleCastleLobbyFunction
        case 0x02D9: set_var((int)_arg[2], 0); break;   // CallBattleArcadeLobbyFunction
        case 0x02DE: set_var((int)_arg[5], 0); set_var((int)_arg[6], 0);
            set_var((int)_arg[7], 0); break;            // ShowBattleHallRecordMonSelectionMenu
        case 0x02D1: break;                             // DeleteActiveBattleHallStreak
        case 0x0313: break;                             // CheckHeapMemory
        case 0x0324: set_var((int)_arg[4], 0);
            set_var((int)_arg[5], 0); break;            // GetBattleHallRecordKeeperStats
        case 0x0325: set_var((int)_arg[0], 0); break;   // GetNumSpeciesWithBattleHallRecords
        // ---- battles spéciales ----
        case 0x02A0: battle_tid = _flex((int)_arg[0]);  // StartTagBattle
            _st = BATTLE; break;
        case 0x0318:                                      // StartFatefulEncounter sp,lv
            battle_sp = _flex((int)_arg[0]); battle_lv = _flex((int)_arg[1]);
            _st = BATTLE; break;
        case 0x0319:                                      // StartGiratinaOriginBattle
            battle_sp = 487; battle_lv = _flex((int)_arg[0]);
            _st = BATTLE; break;
        case 0x0314: set_var((int)_arg[0], !battle_lost); break; // GetBattleResult
        // ---- monde distorsion / events légendaires ----
        case 0x0311: break;                             // AddDistortionWorldMapObject
        case 0x0312: break;                             // DeleteDistortionWorldMapObject
        case 0x0321: break;                             // StartDistortionWorldGiratinaShadowEvent
        case 0x0322: break;                             // FinishDistortionWorldGiratinaShadowEvent
        case 0x023D: break;                             // PlayBoatCutscene
        // ---- place / divers ----
        case 0x00A8: set_var((int)_arg[1], 0); break;   // 0A8 hw,ptr
        case 0x01BF: set_var((int)_arg[0], 0); break;   // CheckDaycareHasEgg
        case 0x02DF: set_var((int)_arg[0], 0); break;   // CalcAmitySquareBerryAndAccessoryManOptionId
        case 0x02E0: set_var((int)_arg[1], 0); break;   // CheckAmitySquareManGiftIsAccessory
        case 0x02E1: set_var((int)_arg[1], 0); break;   // GetAmitySquareBerryOrAccessoryIdFromMan
        case 0x0216: set_var((int)_arg[0], 0); break;   // GetAmitySquareStepCount
        case 0x020D: set_var((int)_arg[1], 0); break;   // 20D b,ptr
        case 0x018C: break;                             // 18C var,var
        case 0x02B6: break;                             // 2B6 var,byte
        case 0x02BB: break;                             // 2BB
        case 0x005D: break;                             // SetSubScene63
        case 0x01F9: break;                             // Dummy1F9
        case 0x029E: break;                             // StartDestroyObstaclesAnimation
        case 0x01CF: case 0x01D0: case 0x01D1: {        // DoStrength/Flash/DefogFunc
            // sélecteur b puis: SET=0/CLEAR=1 -> 0 arg, CHECK=2 -> dest var
            if (_arg[0] == 2) { set_var((int)rh(), 1); }   // utilisable
            break; }
        case 0x0235: {                                  // CallTVBroadcast (arité variable)
            static const char* W[6] = {"h","hhh","hhh","","hh","hhh"};
            int sub = (int)_arg[0];
            const char* w = sub >= 0 && sub < 6 ? W[sub] : "";
            _narg = 1;
            for (; *w && _narg < 8; w++)
                _arg[_narg++] = rh();
            if (sub == 0) set_var((int)_arg[1], 0);        // CHECK_STATUS -> aucune
            else if (sub == 1) { set_var((int)_arg[2], 0); // framing: bank,msg vides
                set_var((int)_arg[3], 0); }
            else if (sub == 2) { set_var((int)_arg[2], 0);
                set_var((int)_arg[3], 0); }
            else if (sub == 4) { set_var((int)_arg[1], 0);
                set_var((int)_arg[2], 0); }
            else if (sub == 5) set_var((int)_arg[3], 0);
            break; }
        case 0x0237:                                    // CallTVInterview (var)
            if (_arg[0] == 0) { _arg[1]=rh(); _arg[2]=rh(); _arg[3]=rh();
                set_var((int)_arg[2], 0); set_var((int)_arg[3], 0); }
            break;
        case 0x021D: {                                  // DoGroupConnectionAction (var)
            static const char* W[6] = {"hh","hh","hh","hh","h","h"};
            int sub = (int)_arg[0];
            const char* w = sub >= 0 && sub < 6 ? W[sub] : "";
            _narg = 1;
            for (; *w && _narg < 8; w++)
                _arg[_narg++] = rh();
            if (sub == 0 || sub == 1) set_var((int)_arg[2], 0); // valid/override -> 0
            else if (sub == 4) set_var((int)_arg[1], 0);
            break; }
        case 0x023E: {                                  // MysteryGiftGive (var)
            static const char* W[8] = {"","","","h","h","h","","hh"};
            int sub = (int)_arg[0];
            const char* w = sub >= 0 && sub < 8 ? W[sub] : "";
            _narg = 1;
            for (; *w && _narg < 8; w++)
                _arg[_narg++] = rh();
            if (sub >= 3 && sub <= 5) set_var((int)_arg[1], 0);
            else if (sub == 7) { set_var((int)_arg[1], 0);
                set_var((int)_arg[2], 0); }
            break; }
        // ---- reste : stubs des ops rares (late-game / systèmes non portés) ----
        case 0x0263: break;                             // ChangeDeoxysForm
        case 0x01E5: break;                             // IncrementGameRecord
        case 0x00FF: break;                             // BufferContestRegistrationEntryNumber
        case 0x029F: break;                             // 29F
        case 0x02D8: shop_items.clear(); _st = SHOP; break; // PokeMartFrontier
        case 0x00BA: break;                             // Unused_0BA
        case 0x01BB: break;                             // DecreasePartyMonFriendship
        case 0x032D: break; case 0x0331: break; case 0x032E: break;
        case 0x023A: set_var((int)_arg[0], 0);
            set_var((int)_arg[1], 0); break;            // GetPartyMonFootprintType
        case 0x0292: set_var((int)_arg[1], 0); break;   // CheckShouldShowGhost
        case 0x0317: set_var((int)_arg[0], 0); set_var((int)_arg[1], 0);
            set_var((int)_arg[2], 0); break;            // GetPlayer3DPos (nd)
        case 0x016E: set_var((int)_arg[0], 0); break;   // GetDaycareState
        case 0x00A2: break; case 0x0204: break;         // 0A2 / WarpLastElevator (nd)
        case 0x0135: break;                             // 135
        case 0x00AA: break;                             // OpenRegionMap (UI nd)
        case 0x0173: break;                             // InitPersistedMapFeaturesForCanalaveGym
        case 0x0103: break;                             // BufferContestType
        case 0x014C:                                    // SetBlackOutWarpId warpId
            if (cb_setbo) cb_setbo(_flex((int)_arg[0])); break;
        case 0x02CD: break;                             // 2CD
        case 0x031B: set_var((int)_arg[0], 0); break;   // GetCurrentSafariGameCaughtNum
        case 0x0174: break;                             // InitPersistedMapFeaturesForVeilstoneGym
        case 0x026C: set_var((int)_arg[0], 0); break;   // TryGetRandomMassageGirlAccessory
        case 0x027A: break;                             // UseVistaLighthouseBinoculars
        case 0x028F: set_var((int)_arg[0], 0); break;   // GetLeagueVictories
        case 0x02BE: set_var((int)_arg[0], 0); break;   // GetTrainerCardLevel
        case 0x01F7: set_var((int)_arg[0], 1); break;   // SurvivePoison -> 1
        case 0x018F: break;                             // StoreSaveResult
        case 0x0128: set_var((int)_arg[0], 0); break;   // GetHoneyTreeStatus
        case 0x024B: break; case 0x024C: break;         // PC boot anims
        case 0x024D: break;                             // ClosePcAnimation
        case 0x00AB:                                    // OpenPcFunction (boxMode)
            pc_mode = (int)_arg[0]; _st = PCBOX; break;
        case 0x0251: {                                  // SetvarIdPokeBoxes type,dst
            int res = -1;
            for (int b = 0; b < 18 && res < 0; b++) {
                bool full = true;
                for (int s = b * 30; s < b * 30 + 30; s++)
                    if (!pcbox[s].species) { full = false; break; }
                if (!full) res = b;
            }
            set_var((int)_arg[1], res);
        } break;
        case 0x0252: {                                  // CheckBoxesNumber dst
            int n = 0;
            for (int b = 0; b < 18; b++)
                for (int s = b * 30; s < b * 30 + 30; s++)
                    if (pcbox[s].species) { n++; break; }
            set_var((int)_arg[0], n);
        } break;
        case 0x00A9: break;                             // OpenSealCapsuleEditor
        case 0x0205: break; case 0x020C: break;         // GeoNet / MessageFromTrainerType
        case 0x01AC: break;                             // HatchEgg (anim nd)
        case 0x00B3: break;                             // 0B3
        case 0x02F6: set_var((int)_arg[2], 0); break;   // 2F6 var,var,ptr
        case 0x0265: break; case 0x0266: break;         // HidePoketch / ShowPoketch
        case 0x0116: break;                             // ShowLinkContestRecords
        case 0x00F6: break;                             // StartLinkBattle
        case 0x00A3: break; case 0x01F8: break;         // 0A3 / WaitForTransition
        case 0x0007: case 0x0008: case 0x0006: break;   // UNUSED_007/008/006
        case 0x026A: break;                             // FlickerObject
        case 0x0328: break;                             // SetPartyGiratinaForm
        case 0x025E: break;                             // InitLakeGuardianContainmentUnits
        case 0x0253: break;                             // SetClearInCatchingShowFlag
        case 0x02C5: case 0x02D5: case 0x02DC: break;   // DeleteActive*Streak
        case 0x00BF: break;                             // UseRockClimb (anim nd)
        case 0x00C0: if (cb_surf) cb_surf(); break;     // UseSurf -> monter sur l'eau
        case 0x0181: set_var((int)_arg[0], 0); break;   // GetBerryYield
        case 0x017D: set_var((int)_arg[0], 0); break;   // GetBerryGrowthStage
        case 0x017E: set_var((int)_arg[0], 0); break;   // GetBerryItemId
        case 0x0183: case 0x0184: break;                // PlantBerry / SetBerryWateringState
        case 0x0323: set_var((int)_arg[0], 0); break;   // CheckNoWifiPlazaCooldown
        case 0x0308: break; case 0x0309: break;         // CameraOverrideObject add/remove
        case 0x0200: set_var((int)_arg[0], 0); break;   // GetPreviousMapId
        case 0x020E: break;                             // InitGreatMarshTram
        case 0x020A: break;                             // StartVsSeeker
        case 0x0294: break;                             // ShowBattlePoints
        case 0x016D: break;                             // BufferDaycareMonNicknames
        case 0x0048: break;                             // ShowMenuMultiColumn
        case 0x0212: set_var((int)_arg[0], 0); break;   // GetPartyMonNature
        case 0x017C: break;                             // BufferNatureName
        case 0x0107: set_var((int)_arg[0], 0); break;   // GetPlayerContestPlacement
        case 0x030C: break;                             // SaveTVSegmentHomeAndManorNoFurniture
        case 0x02F0: break;                             // InitPersistedMapFeaturesForVilla
        case 0x00B8: set_var((int)_arg[0], 0); break;   // GetApproachingTrainerType
        case 0x022B: break;                             // TurnOnPokedexLanguageDetection
        case 0x0059: set_var((int)_arg[0], 0); break;   // CheckRecordedChatotCryIsPlayable
        case 0x0120: set_var((int)_arg[0], 0); break;   // GetNationalDexSeenCount
        case 0x01DB: break;                             // InitBattleTower
        case 0x02A2: {                                  // TrySetUnusedCollectedOrbFlag
            int it = _flex((int)_arg[0]);
            if (it == 135 || it == 136) set_flag(0xF000, true);  // orbe souterrain
            break;
        }
        case 0x0341: break;                             // BufferSpeciesNameWithArticle (nd)
        case 0x00C3: break; case 0x00C4: break;         // Flash/Defog anims
        case 0x028C: break;                             // DrawPokemonPreviewFromPartySlot
        default:
            if (cb_unimpl) cb_unimpl(od->name, op);
            break;
        }
    }
    return _st;
}
