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
    _f = file; _stack.clear(); _st = DONE; _holds.clear();
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

// sauvegarde/reprise de contexte — voir Ctx dans script.h
ScriptVM::Ctx ScriptVM::save_ctx() const {
    Ctx c;
    c.fns = _fns; c.stdfns = _stdfns;
    c.pc = _pc; c.st = _st; c.stack = _stack;
    c.cur_script = cur_script; c.result = result;
    c.pending_var = pending_var; c.signpost_dest = signpost_dest;
    c.wait_t = wait_t; c.wait_anim = wait_anim; c.wait_snd = wait_snd;
    c.close_btn = close_btn;
    c.menu_dest = menu_dest; c.menu_cancel = menu_cancel;
    c.menu_bank = menu_bank; c.menu_items = menu_items;
    // ScriptContext DS : copie heap du bytecode (le buffer narc peut être
    // rechargé par le changement de map avant la reprise du script).
    std::unordered_map<const Bytes*, const Bytes*> remap;
    auto hold = [&](const Bytes* b) -> const Bytes* {
        if (!b) return nullptr;
        const Bytes*& r = remap[b];
        if (!r) { auto sp = std::make_shared<Bytes>(*b); c.holds.push_back(sp); r = sp.get(); }
        return r;
    };
    c.f = hold(_f); c.stdf = hold(_stdf);
    for (auto& fr : c.stack) fr.f = hold(fr.f);
    return c;
}
void ScriptVM::restore_ctx(Ctx&& c) {
    _holds = std::move(c.holds);
    _f = c.f; _fns = std::move(c.fns);
    _stdf = c.stdf; _stdfns = std::move(c.stdfns);
    _pc = c.pc; _st = c.st; _stack = std::move(c.stack);
    cur_script = c.cur_script; result = c.result;
    pending_var = c.pending_var; signpost_dest = c.signpost_dest;
    wait_t = c.wait_t; wait_anim = c.wait_anim; wait_snd = c.wait_snd;
    close_btn = c.close_btn;
    menu_dest = c.menu_dest; menu_cancel = c.menu_cancel;
    menu_bank = c.menu_bank; menu_items = std::move(c.menu_items);
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
    int guard = 0;
    while (_st == RUN) {
        if (++guard > 2000 || _pc + 1 >= _f->size()) { _st = DONE; break; }
        int pc0 = (int)_pc;
        uint16_t op = rh();
        const OpDef* od = opdef(op);
        trace[trace_n++ % 40] = {pc0, (int)op, od};
        static bool trace = getenv("SCRTRACE") != nullptr;
        if (trace) printf("  [op] %04X %s  @pc=%d f=%p\n", op,
            od ? od->name : "?", pc0, (void*)_f);
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
            wait_t = (int)_arg[0] * 2; _st = WAITTIME; break; // ticks DS @30Hz -> ~60Hz
        case 0x0015: _end(); break;                       // ExitStandard
        case 0x000B:                                      // CompareRegs (rare)
            result = _cmp(var((int)_arg[0]), var((int)_arg[1])); break;
        case 0x0011:                                      // CompareVarToValue (val littérale)
            result = _cmp(var((int)_arg[0]), (int)_arg[1]); break;
        case 0x0012:                                      // CompareVarToVar
            result = _cmp(var((int)_arg[0]), var((int)_arg[1])); break;
        case 0x0014: {                                    // CallStandard fn -> fichier commun
            int n = (int)_arg[0];
            if (n >= 2000) n -= 2000;                    // ids std 0x7D0+ -> index
            if (_stdf && n < (int)_stdfns.size()) {
                _stack.push_back({_f, _pc});
                _f = _stdf; _pc = _stdfns[n];
            }
            else if (cb_unimpl) cb_unimpl("CallStandard hors table", op);
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
        case 0x0021:                                      // CheckFlagFromVar fid,dest
            set_var((int)_arg[1], flag(_flex((int)_arg[0])) ? 1 : 0); break;
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
        case 0x002F:                                      // Message*/MessageNoSkip (GetVar)
            if (cb_msg) cb_msg(_flex((int)_arg[0]), false);
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
        case 0x0039: wait_t = 60; _st = WAITTIME; break;  // WaitForSignpostDone : minuté DS
        case 0x003A:                                      // DrawSignpostScrolling msg,dest
            if (cb_msg) cb_msg((int)_arg[0], false);
            signpost_dest = (int)_arg[1]; _st = WAITBTN; break;
        case 0x003B:                                      // GetSignpostInput dest
            signpost_dest = (int)_arg[0]; _st = WAITBTN; break;
        case 0x003E: pending_var = (int)_arg[0]; _st = YESNO; break;
        case 0x005E:                                      // ApplyMovement obj,off
            if (cb_move) cb_move(_flex((int)_arg[0]), _pc + (int32_t)_arg[1]);
            break;
        case 0x005F: _st = MOVE; break;                   // WaitMovement
        case 0x0040: case 0x0041: case 0x0044: case 0x0045: { // Init[Local|Global][Text]Menu
            menu_items.clear();
            menu_dest = (int)_arg[4];                       // var de sélection
            // arg3 = canExitWithB ; le DS écrit MENU_CANCEL(-2) si B autorisé
            menu_cancel = (int)_arg[3] ? 0xFFFE : -9999;
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
        case 0x003C: if (cb_fld) cb_fld(9, 0, 0); break;  // ShowStartMenu (menu X)
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
        case 0x00A1: break;                               // ReturnToField : DS =
            // FieldTransition_StartMap (restaure la vue terrain après un
            // sous-écran). Chez nous le retour est déjà implicite -> no-op.
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
        case 0x0063: if (cb_lockobj) cb_lockobj(_flex((int)_arg[0]), false); break; // Release
        case 0x0064: if (cb_people) cb_people(_flex((int)_arg[0]), true); break; // AddObject
        case 0x0065: if (cb_people) cb_people(_flex((int)_arg[0]), false); break;// RemoveObject
        case 0x01B1: if (cb_hide) cb_hide(_flex((int)_arg[0]), false); break;   // ShowObject
        case 0x01B2: if (cb_hide) cb_hide(_flex((int)_arg[0]), true); break;    // HideObject
        case 0x0066: if (cb_fld) cb_fld(3, _flex((int)_arg[0]),
            _flex((int)_arg[1])); break;                // AddFreeCamera x,z
        case 0x0067: if (cb_fld) cb_fld(4, 0, 0); break;  // RestoreCamera
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
        case 0x0080: set_var((int)_arg[1],              // GetItemPocket ->
            cb_item ? cb_item(0, _flex((int)_arg[0])) : 0); break; // field_pocket
        case 0x0083: case 0x0087: case 0x008F:            // SendGoodToPC/GiveTrap/GiveSphere
            set_var((int)_arg[2], 1); break;
        case 0x02A7:                                      // CheckPlate i,dest
            set_var((int)_arg[1], bag_count((int)_arg[0]) > 0 ? 1 : 0); break;
        case 0x0236: break;                             // Cmd_236 (nc)
        // ── buffers StringTemplate : (slot, kind, val) ──────────────────────
        case 0x00CD: case 0x00CE: case 0x00CF:            // BufferPlayer/Rival/Counterpart
            if (cb_buf) cb_buf((int)_arg[0], op - 0xCD, 0); break;
        case 0x00D0: {                                // BufferPartyMonSpecies
            int s = _flex((int)_arg[1]);                // slot équipe (via GetVar)
            int sp = (s >= 0 && s < (int)party.size()) ? party[s].species : 0;
            if (cb_buf) cb_buf((int)_arg[0], 30, sp); break; }
        case 0x00D6: {                                  // SetVarPokeNick slot
            int s = _flex((int)_arg[1]);                // -> surnom/ŒUF (kind 92)
            if (cb_buf) cb_buf((int)_arg[0], 92, s); break; }
        case 0x00D8:                                      // BufferTrainerClassName
            if (cb_buf) cb_buf((int)_arg[0], 100, _flex((int)_arg[1])); break;
        case 0x00D9: break;                               // SetVarWiFiSprite (nd)
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
            if (cb_evpos) cb_evpos(_flex((int)_arg[0]), _flex((int)_arg[1]),
                _flex((int)_arg[2]));
            break;                                       // -> event template
        case 0x0187:                                      // SetPosition o,x,y,z,dir
            if (cb_setpos) cb_setpos(_flex((int)_arg[0]), _flex((int)_arg[1]),
                _flex((int)_arg[3]), _flex((int)_arg[4]), _flex((int)_arg[2]));
            break;                                       // -> objet vivant
        case 0x0188: case 0x0189:                         // SetObjectEventMoveType/Dir
            if (cb_ow) cb_ow(_flex((int)_arg[0]), op - 0x188,
                _flex((int)_arg[1])); break;
        case 0x018B:                                      // SetBgEventPos idx,x,z
            if (cb_bgev) cb_bgev(_flex((int)_arg[0]), _flex((int)_arg[1]),
                _flex((int)_arg[2]));
            break;
        case 0x01B7: {                                    // GetRandom dest,borne
            int b = _flex((int)_arg[1]);                  // DS : arg0=var dest
            set_var((int)_arg[0], rand() % (b > 0 ? b : 1)); break; }
        case 0x01BD:                                      // GetPlayerDir dest
            if (cb_pdir) set_var((int)_arg[0], cb_pdir()); break;
        case 0x01CC: set_flag(2409, true); break;         // GiveJournal -> FLAG_JOURNAL_ACQUIRED
        case 0x022D:                                      // NatDex enable/check
            if ((int)_arg[0] == 1) set_flag(144, true);
            else set_var((int)_arg[1], flag(144) ? 1 : 0);
            break;
        case 0x0283: if (cb_fld) cb_fld(8, _flex((int)_arg[0]),
            _flex((int)_arg[1])); break;                // SetInitialVolumeForSequence
        case 0x01B6: {                                    // CheckTime -> période DS
            time_t t = time(nullptr); int h = localtime(&t)->tm_hour;
            int p = h >= 20 ? 3 : h >= 17 ? 2 : h >= 10 ? 1 : h >= 4 ? 0 : 4;
            set_var((int)_arg[0], p);
        } break;
        case 0x00BE:                                      // Warp hdr,?,x,z,dir (GetVar)
            if (cb_warp) cb_warp(_flex((int)_arg[0]), _flex((int)_arg[2]),
                _flex((int)_arg[3]), _flex((int)_arg[4]));
            // DS FieldTask_InitCall : la map-change task s'exécute avant la
            // reprise du contexte — le script continue après le chargement.
            wait_t = 1; _st = WAITTIME; break;
        case 0x014D: set_var((int)_arg[0], cb_pgender ? cb_pgender() : 0); break; // GetPlayerGender
        case 0x000A: break;                               // AdrsAdrsSet (unused)
        case 0x0062: if (cb_lockobj) cb_lockobj(_flex((int)_arg[0]), true); break;   // Lock
        case 0x006C: if (cb_fld) cb_fld(0, _flex((int)_arg[0]),
            (int)_arg[1]); break;                       // SetObjectFlagIsPersistent
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
        case 0x0133: {                                  // RegisterPoketchApp
            int a = _flex((int)_arg[0]);
            if (a >= 0 && a < 32) poketch_apps |= 1u << a;
            break; }
        case 0x0134: set_var((int)_arg[1],            // CheckPoketchAppRegistered
            (_flex((int)_arg[0]) >= 0 && _flex((int)_arg[0]) < 32)
                ? (int)((poketch_apps >> _flex((int)_arg[0])) & 1) : 0); break;
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
        case 0x00C9: if (cb_fld) cb_fld(2, _flex((int)_arg[0]), 0); break;  // ForceBicycling -> SetPlayerState
        case 0x011C: {                                  // GetFloorsAbove : table DS field_menu.c
            static const struct { int map, n; } F[] = {
                {421,1},{423,0},{436,0},{441,1},{444,1},{445,0},{470,1},
                {471,0},{435,4},{434,3},{433,2},{432,1},{431,0},{430,5},
                {407,3},{408,2},{409,1},{410,0}};
            int m = cb_mapid ? cb_mapid() : -1, n = 1;
            for (auto& e : F) if (e.map == m) { n = e.n; break; }
            set_var((int)_arg[0], n); break; }
        case 0x012E: set_var((int)_arg[1], 0); break;     // DressUpPhotoHasData
        case 0x00A7: break;                               // ShowDressUpPhoto
        case 0x0085: set_var((int)_arg[2], 1); break;     // CheckHasRoomForGoodsInPC
        case 0x009A: {                                    // FindPartySlotWithMove
            int s = cb_findmove ? cb_findmove(_flex((int)_arg[1])) : -1;
            set_var((int)_arg[0], s < 0 || s == 0xFF ? 6 : s); break; }
            // DS : slot trouvé ou MAX_PARTY_SIZE(6) ; les œufs sont ignorés
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
        case 0x019A: {                                    // CountPartyNonEggs dest
            int n19a = 0;
            for (auto& p : party) if (!p.egg) n19a++;
            set_var((int)_arg[0], n19a); break; }
        case 0x019C: {                                    // CountAliveMonsAndBoxMons dest
            int n19c = 0;                               // vivant = hp!=0 (-1 = plein)
            for (auto& p : party) if (p.species && !p.egg && p.hp != 0) n19c++;
            for (auto& p : pcbox) if (p.species && !p.egg && p.hp != 0) n19c++;
            set_var((int)_arg[0], n19c); break; }
        case 0x0196: _summary_slot = _flex((int)_arg[0]); break; // SetMonSummary
        case 0x0197: set_var((int)_arg[0], _summary_slot); break; // GetMonPartySlot
        case 0x0198: {                                    // GetPartyMonSpecies slot,dest
            int sl = var((int)_arg[0]);                 // slot contenu dans une var
            int sp = (sl >= 0 && sl < (int)party.size() && !party[sl].egg)
                ? party[sl].species : 0;                // œuf -> SPECIES_NONE
            set_var((int)_arg[1], sp); break; }
        case 0x019B: {                                    // CountAliveMonsExcept dest,slot
            int ex = _flex((int)_arg[1]), n = 0;
            for (int i = 0; i < (int)party.size(); i++)
                if (i != ex && party[i].hp > 0) n++;
            set_var((int)_arg[0], n); } break;
        case 0x01B9: {                                    // GetPartyMonFriendship
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0], (s >= 0 && s < (int)party.size())
                ? party[s].friendship : 0); break; }
        case 0x01BA: {                                    // IncreasePartyMonFriendship
            // DS : +150% soothe bell, +1 luxury ball, +1 lieu de capture ;
            // les objets portés ne sont pas suivis -> bonus non appliqués.
            int v = _flex((int)_arg[0]), s = _flex((int)_arg[1]);
            if (s >= 0 && s < (int)party.size() && v > 0) {
                int f = party[s].friendship + v;
                party[s].friendship = f > 255 ? 255 : f;  // MAX_FRIENDSHIP_VALUE
            } break; }
        case 0x01C0: {                                    // CheckPartyHasSpecies dest,sp
            int sp = _flex((int)_arg[1]), ok = 0;
            for (auto& m : party) if (m.species == sp) ok = 1;
            set_var((int)_arg[0], ok); } break;
        case 0x01CD: journal[_flex((int)_arg[0])]++; break; // CreateJournalEvent
        case 0x01DD: break;                               // CallBattleTowerFunction
        // fossiles DS (scrcmd_fossil.c) : item -> espèce
        case 0x01F1: {                                  // GetFossilCount dest
            static const int FOSS[7] = {103, 101, 102, 99, 100, 104, 105};
            int n = 0;
            for (int it : FOSS) n += bag_count(it);
            set_var((int)_arg[0], n); break; }
        case 0x01F4: {                                  // GetSpeciesFromFossil dest,item
            static const struct { int it, sp; } M[] = {
                {103, 142}, {101, 138}, {102, 140}, {99, 345},
                {100, 347}, {104, 410}, {105, 408}};
            int it = _flex((int)_arg[1]), sp = 0;
            for (auto& e : M) if (e.it == it) { sp = e.sp; break; }
            set_var((int)_arg[0], sp); break; }
        case 0x01F5: {                                  // FindFossilAtThreshold it,idx,th
            static const struct { int it, sp; } M[] = {
                {103, 142}, {101, 138}, {102, 140}, {99, 345},
                {100, 347}, {104, 410}, {105, 408}};
            int th = _flex((int)_arg[2]), cum = 0, oi = 0, oit = 0;
            for (int i = 0; i < 7; i++) {
                cum += bag_count(M[i].it);
                if (cum >= th) { oit = M[i].it; oi = i; break; }
            }
            set_var((int)_arg[0], oit); set_var((int)_arg[1], oi); break; }
        case 0x0201: set_var((int)_arg[0], cb_mapid ? cb_mapid() : 0); break; // GetCurrentMapID
        case 0x0202: break;                               // StartEndSafariGame
        case 0x0238: set_var((int)_arg[1], 0); break;     // CheckTVInterviewEligible
        case 0x0247: {                                    // GetFirstNonEggInParty
            int s = 0;
            for (int i = 0; i < (int)party.size(); i++)
                if (party[i].species > 0 && !party[i].egg) { s = i; break; }
            set_var((int)_arg[0], s); break; }
        case 0x0260: records[0x10000 + _flex((int)_arg[0])]++; break; // TrainerScore2
        case 0x0285: if (cb_turnback) cb_turnback(        // InitTurnbackCave
            _flex((int)_arg[0]), _flex((int)_arg[1])); break;
        case 0x02B5: if (cb_exitloc) cb_exitloc(_flex((int)_arg[0]),  // SetExitLocation
                             _flex((int)_arg[1]), _flex((int)_arg[2])); break;
        case 0x02BC: set_var((int)_arg[0], battle_result != 4); break; // CheckDidNotCapture
        case 0x02BD:                                      // StartLegendaryBattle sp,lv
            battle_sp = _flex((int)_arg[0]); battle_lv = _flex((int)_arg[1]);
            _st = BATTLE; break;
        case 0x0124:                                      // StartWildBattle sp,lv
            battle_sp = _flex((int)_arg[0]); battle_lv = _flex((int)_arg[1]);
            _st = BATTLE; break;
        case 0x0129:                                      // StartHoneyTreeBattle
            battle_sp = cb_honeysp ? cb_honeysp() : 0;
            if (battle_sp > 0) {                        // DS : niveau uniforme 5-15
                battle_lv = 5 + rand() % 11; _st = BATTLE; }
            honey_tree = 0; honey_min = 0.f; break;
        case 0x006B:                                      // MoveCamera x,y,z (offset px)
            if (cb_fld) cb_fld(10,
                ((_flex((int)_arg[0]) & 0xFFFF) << 16) | (_flex((int)_arg[2]) & 0xFFFF),
                _flex((int)_arg[1]));
            break;
        case 0x0093: set_var((int)_arg[0], 0); break;     // CountSealOccurence
        case 0x0094: break;                               // GiveOrTakeSeal
        case 0x0095: {                                    // GetPartyMonForm slot,dest
            int s = _flex((int)_arg[0]);
            set_var((int)_arg[1],
                s >= 0 && s < (int)party.size() ? party[s].form : 0); break; }
        case 0x02F1: {                                    // SetPartyMonForm slot,form
            int s = _flex((int)_arg[0]);
            if (s >= 0 && s < (int)party.size())
                party[s].form = _flex((int)_arg[1]); break; }
        case 0x0099: {                                    // CheckPartyMonHasMove dest,mv,slot
            int mv = _flex((int)_arg[1]), s = _flex((int)_arg[2]), ok = 0;
            if (s >= 0 && s < (int)party.size() && !party[s].egg)
                for (int k = 0; k < 4; k++)
                    if (mv && party[s].moves[k] == mv) ok = 1;
            set_var((int)_arg[0], ok); break; }
        case 0x00E3: {                                    // GetSwarmMapAndSpecies m,s
            int m = 0, s = 0;
            if (cb_swarm) cb_swarm(&m, &s);
            set_var((int)_arg[0], m); set_var((int)_arg[1], s); break; }
        case 0x0119: set_var((int)_arg[0], 0); break;     // CheckPartyPokerus
        case 0x011A: {                                    // GetPartyMonGender slot,dest
            int s = _flex((int)_arg[0]), g = 2;
            if (s >= 0 && s < (int)party.size() && !party[s].egg && cb_mgender)
                g = cb_mgender(party[s].species, party[s].pid);
            set_var((int)_arg[1], g); break; }
        case 0x011B: {                                      // SetSpecialLocation
            for (int i = 0; i < 5 && i < _narg; i++)
                special_loc[i] = _flex((int)_arg[i]);       // map,warp,x,z,dir
            break; }
        case 0x011D: break;                               // ShowCurrentFloor
        case 0x0121: {                                    // GetNationalDexCaughtCount
            // DS : bit « pris » du dex — dex_caught suivi (captures/dons/
            // évolutions/échanges). Repli : mons actuellement détenus.
            std::unordered_set<int> got = dex_caught;
            for (auto& p : party) if (p.species > 0) got.insert(p.species);
            for (auto& p : pcbox) if (p.species > 0) got.insert(p.species);
            set_var((int)_arg[0], (int)got.size()); break; }
        case 0x0127: honey_tree = 1; honey_min = 360.f; break; // SlatherHoneyTree (6h DS)
        case 0x0128:                                      // GetHoneyTreeStatus dest
            // DS : 0 = vierge, 1 = enduit (<6h), 2 = secoué (rencontre prête)
            set_var((int)_arg[0], !honey_tree ? 0 : (honey_min <= 0.f ? 2 : 1));
            break;
        case 0x012A: honey_tree = 0; honey_min = 0.f; break; // StopHoneyTreeShaking
        case 0x012C:                                      // CheckSaveType dest
            // enum DS : 0=OVERWRITE(sauvegarde existe), 1=NO_DATA_EXISTS
            set_var((int)_arg[0], cb_save && cb_save(1) ? 0 : 1); break;
        case 0x012D:                                      // TrySaveGame dest
            set_var((int)_arg[0], cb_save ? cb_save(0) : 0); break;
        case 0x018D: case 0x018E: break;                  // Show/HideSavingIcon
        case 0x0192: break;                               // OpenPartyMenuForUnionRoomBattle
        case 0x0199:                                      // CheckIsPartyMonOutsider slot,dest
            set_var((int)_arg[1], 0); break;              // OT = joueur
        case 0x01F6: {                                    // CountPartyMonsBelowLevelThreshold lv,dest
            int th = _flex((int)_arg[0]), n = 0;
            for (auto& p : party)
                if (p.species > 0 && !p.egg && p.level <= th) n++;
            set_var((int)_arg[1], n); break; }
        case 0x0213: {                                    // FindPartySlotWithNature nat,dest
            int nat = _flex((int)_arg[0]), slot = 0xFF;
            for (int i = 0; i < (int)party.size(); i++)
                if (party[i].species > 0 && !party[i].egg &&
                    (int)(party[i].pid % 25) == nat) { slot = i; break; }
            set_var((int)_arg[1], slot); break; }
        case 0x021F: {                                    // CheckHasLearnableReminderMoves dest,slot
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0],
                cb_relearncnt && s >= 0 && s < (int)party.size()
                    ? (cb_relearncnt(s) > 0) : 0); break; }
        case 0x0221: break;                               // OpenMoveReminderMenu (UI absente)
        case 0x0223: set_var((int)_arg[0], 0xFF); break;  // CheckLearnedReminderMove -> rien
        case 0x023B: break;                               // PlayPokecenterHealingAnimation
        case 0x0248: {                                    // GetPartyMonType d1,d2,slot
            int s = _flex((int)_arg[2]), t1 = 0, t2 = 0;
            if (s >= 0 && s < (int)party.size() && cb_mtypes)
                cb_mtypes(party[s].species, &t1, &t2);
            set_var((int)_arg[0], t1); set_var((int)_arg[1], t2); break; }
        case 0x02C1: case 0x02C2: break;                  // OpenSaveInfo/CloseSaveInfo
        case 0x02F2: if (cb_gym) cb_gym(21, 0); break;    // InitDistortionWorldFeatures
        case 0x0334: records[(int)_arg[0]] += _flex((int)_arg[1]); break; // AddToGameRecord
        case 0x0335: records[(int)_arg[0]] += (int)_arg[1]; break;        // AddToGameRecordBigValue
        case 0x0337: set_var((int)_arg[1],                // CheckHasSeenSpecies
            dex_seen.count(_flex((int)_arg[0])) ? 1 : 0); break;
        case 0x0096: {                                            // GivePokemon
            PartyMon m{_flex((int)_arg[0]), _flex((int)_arg[1]), -1};
            if (cb_moninit) cb_moninit(m);                  // init DS : PID/IVs/moves/PP/PV
            m.held = _flex((int)_arg[2]);                   // objet tenu
            dex_caught.insert(m.species);
            dex_seen.insert(m.species);
            party.push_back(m);
            if (trace) printf("  [party] +species %d lv%d (n=%zu)\n",
                m.species, m.level, party.size());
            set_var((int)_arg[3], 1); break; }                    // succès -> varptr
        case 0x0097: {                                            // GiveEgg sp,textId
            if (party.size() < 6) {                     // rien si équipe pleine
                PartyMon m;
                m.species = _flex((int)_arg[0]); m.level = 1;
                m.hp = -1; m.egg = true;
                party.push_back(m);
            }
            break; }
        case 0x00BB: {                                  // ChoosePokeNick slot,dest
            int s = _flex((int)_arg[0]);
            if (s >= 0 && s < (int)party.size()) {
                nick_slot = s; nick_dest = (int)_arg[1]; _st = NICK;
            } else set_var((int)_arg[1], 1);            // annulé -> 1
            break; }
        case 0x00B4: _st = STARTER; break;                  // StartChooseStarterScene
        case 0x00B5: set_var(0x4030, chosen_starter); break;      // SaveChosenStarter -> VAR_PLAYER_STARTER
        case 0x00DB: if (cb_buf) cb_buf((int)_arg[0], 30, chosen_starter); break;
        case 0x00DC: if (cb_buf) cb_buf((int)_arg[0], 30, rival_starter); break;
        case 0x00DD: if (cb_buf) cb_buf((int)_arg[0], 30, cpart_starter); break;
        case 0x00DE: set_var((int)_arg[0], var(0x4030)); break;   // GetPlayerStarterSpecies
        case 0x0125:                                    // StartFirstBattle tid=GetVar
            battle_tid = _flex((int)_arg[0]); _st = BATTLE; break;
        case 0x00E4: set_var((int)_arg[0],                // GetTrainerID
            cur_script - (cur_script < 5000 ? 2999 : 4999)); break;
        case 0x00E5:                                    // StartTrainerBattle t1,t2
            battle_tid = _flex((int)_arg[0]);
            battle_tid2 = _flex((int)_arg[1]); _st = BATTLE; break;
        case 0x00E6:                                    // PrintTrainerDialogue tid,type
            if (cb_trainermsg) cb_trainermsg(_flex((int)_arg[0]), _flex((int)_arg[1]));
            _st = MSG; break;
        case 0x00E7: {                                  // GetTrainerMessageTypes
            int tid = cur_script - (cur_script < 5000 ? 2999 : 4999);
            int dbl = cb_trdata ? cb_trdata(tid) : 0, b = cur_script >= 5000;
            set_var((int)_arg[0], dbl ? (b ? 7 : 3) : 0);
            set_var((int)_arg[1], dbl ? (b ? 9 : 5) : 2);
            set_var((int)_arg[2], dbl ? (b ? 10 : 6) : 0);
            break; }
        case 0x00E8: {                                  // GetTrainerRematchMessageTypes
            int tid = cur_script - (cur_script < 5000 ? 2999 : 4999);
            int dbl = cb_trdata ? cb_trdata(tid) : 0, b = cur_script >= 5000;
            // DS : TRMSG_REMATCH / TRMSG_REMATCH_FIRST/SECOND_*
            set_var((int)_arg[0], dbl ? (b ? 18 : 17) : 16);
            set_var((int)_arg[1], 0);
            set_var((int)_arg[2], dbl ? (b ? 10 : 6) : 0);
            break; }
        case 0x00E9: {                                  // CheckIsTrainerDoubleBattle
            int tid = cur_script - (cur_script < 5000 ? 2999 : 4999);
            set_var((int)_arg[0], cb_trdata ? (cb_trdata(tid) != 0) : 0); break; }
        case 0x00EA:                                    // PlayTrainerEncounterBGM
            if (cb_audio) cb_audio(0xEA, _flex((int)_arg[0])); break;
        case 0x00EC: set_var((int)_arg[0],          // CheckWonBattle
            battle_result != 2 && battle_result != 3); break;
        case 0x00ED: set_var((int)_arg[0],          // CheckLostBattle
            battle_result == 2 || battle_result == 3); break;
        case 0x00EE: {                                  // CheckHasTwoAliveMons
            int a = 0;
            for (auto& m : party) if (m.hp > 0) a++;
            set_var((int)_arg[0], a >= 2 ? 1 : 0); break; }
        case 0x00EF: battle_lost = 0; break;            // StartDummyTrainerBattle
        case 0x00F0: if (cb_targetflag) cb_targetflag(1); break;  // SetTargetTrainerDefeated
        case 0x00F1:                                    // GoToIfTargetTrainerDefeated
            if (cb_targetflag && cb_targetflag(0)) _rel((int32_t)_arg[0]);
            break;
        case 0x02AD:                                    // GetMovementType dest,objRef
            set_var((int)_arg[0], cb_objmt ? cb_objmt(_flex((int)_arg[1])) : 0); break;
        case 0x009B: set_var((int)_arg[1], 0); break;     // GetRematchTrainerID -> TRAINER_NONE
        case 0x020B: if (cb_gym) cb_gym(12, 0); break;    // SetMoveCodeForFacingDirection
        case 0x00EB: if (cb_lostpc) cb_lostpc(); _end(); break;   // BlackOutFromBattle
        case 0x014B: if (cb_lostpc) cb_lostpc(); _end(); break;   // DefeatGoPokecenter (idem)
        case 0x0208: if (cb_pic) cb_pic((int)_arg[0], true); break;  // PokemonPicture
        case 0x0209: if (cb_pic) cb_pic(0, false); break;          // HidePicture
        case 0x021C: if (cb_roam) cb_roam(_flex((int)_arg[0])); break;  // ActSwarmPoke -> roamer
        case 0x018A:                                      // SetWarpEventPos idx,x,z
            if (cb_warpev) cb_warpev(_flex((int)_arg[0]), _flex((int)_arg[1]),
                _flex((int)_arg[2])); break;             // -> event template
        // ---- queries équipe / dex / RTC / état joueur ----
        case 0x0177: set_var((int)_arg[0], (int)party.size()); break; // GetPartyCount
        case 0x0193: set_var((int)_arg[0],            // GetSelectedPartySlot
            (_summary_slot >= 0 && _summary_slot < 6) ? _summary_slot : 0xFF); break;
        case 0x0278: {                                    // GetPartyMonLevel dest,slot
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0],
                (s >= 0 && s < (int)party.size()) ? party[s].level : 0);
            break; }
        case 0x0262: {                                  // CheckPartyHasSpecies2 sp,dest
            int sp = _flex((int)_arg[0]), has = 0;
            for (auto& m : party) if (m.species == sp) has = 1;
            set_var((int)_arg[1], has); break; }
        case 0x011E: {                                  // GetLocalDexSeenCount dest
            int n = 0;
            for (int sp : dex_seen)
                if (cb_dex && cb_dex(0, sp) > 0) n++;
            set_var((int)_arg[0], n); break; }
        case 0x01E8: {                                  // CheckLocalDexCompleted
            int n = 0;
            for (int sp : dex_seen)
                if (cb_dex && cb_dex(0, sp) > 0) n++;
            set_var((int)_arg[0], n >= 210 ? 1 : 0); break; }
        case 0x01E9: {                                  // CheckNationalDexCompleted
            // DS : pris, hors 11 exclus (Mew..Arceus) -> objectif 482
            static const int EXC[] = {151, 249, 250, 251, 385, 386, 489,
                                      490, 491, 492, 493};
            int n = 0;
            for (int sp : dex_caught) {
                bool ex = false;
                for (int e : EXC) if (sp == e) { ex = true; break; }
                if (!ex && sp >= 1 && sp <= 493) n++;
            }
            set_var((int)_arg[0], n >= 482 ? 1 : 0); break; }
        case 0x0284: set_var((int)_arg[0],              // GetUnownFormsSeenCount
            (int)dex_unown.size()); break;
        case 0x02B7: set_var((int)_arg[0], 0); break;   // CheckPartyHasBadEgg
        case 0x026B: {                                  // CheckHasAllLegendaryTitansInParty
            bool ok = true;
            for (int sp : {377, 378, 379}) {
                bool h = false;
                for (auto& m : party) if (m.species == sp && !m.egg) h = true;
                if (!h) { ok = false; break; }
            }
            set_var((int)_arg[0], ok ? 1 : 0); break; }
        case 0x032B: {                                  // CheckPartyHasFatefulEncounterRegigigas
            int ok = 0;
            for (auto& m : party)
                if (!m.egg && m.species == 486 && m.fateful) { ok = 1; break; }
            set_var((int)_arg[0], ok); break; }
        case 0x031C: {                                  // FindPartySlotWithFatefulEncounterSpecies
            int sp = _flex((int)_arg[1]), slot = 0xFF;
            for (int i = 0; i < (int)party.size(); i++)
                if (!party[i].egg && party[i].species == sp &&
                    party[i].fateful) { slot = i; break; }
            set_var((int)_arg[0], slot); break; }
        case 0x0303: {                                  // GetPartyRotomCountAndFirst n,slot
            int n = 0, first = 0xFF;
            for (int i = 0; i < (int)party.size(); i++)
                if (!party[i].egg && party[i].species == 479 &&
                    party[i].form != 0) {
                    if (first == 0xFF) first = i;
                    n++;
                }
            set_var((int)_arg[0], n); set_var((int)_arg[1], first); break; }
        case 0x0302: for (int i = 0; i < 5; ++i) set_var((int)_arg[i], 0); break; // GetRotomFormsInSave
        case 0x0234: { time_t t = time(nullptr);        // GetDayOfWeek (RTC)
            set_var((int)_arg[0], localtime(&t)->tm_wday); break; }
        case 0x0268: { time_t t = time(nullptr);        // GetHour (RTC)
            set_var((int)_arg[0], localtime(&t)->tm_hour); break; }
        case 0x00CB: if (cb_fld) cb_fld(1, _flex((int)_arg[0]), 0); break; // SetPlayerState
        case 0x00CC: if (cb_fld) cb_fld(2, 0, 0); break;  // ChangePlayerState
        // ---- rubans / EV / concours / presse ----
        case 0x022F: {                                  // CountPartyRibbons dest
            int n = 0;
            for (int r = 0; r < 80; r++)
                for (auto& m : party)
                    if (!m.egg && m.ribbons.test(r)) { n++; break; }
            set_var((int)_arg[0], n); break; }
        case 0x0230: {                                  // GetPartyMonRibbon dest,slot,rib
            int s = _flex((int)_arg[1]), r = _flex((int)_arg[2]), ok = 0;
            if (s >= 0 && s < (int)party.size() && !party[s].egg &&
                r >= 0 && r < 80)
                ok = party[s].ribbons.test(r) ? 1 : 0;
            set_var((int)_arg[0], ok); break; }
        case 0x0231: {                                  // SetPartyMonRibbon slot,rib
            int s = _flex((int)_arg[0]), r = _flex((int)_arg[1]);
            if (s >= 0 && s < (int)party.size() && r >= 0 && r < 80)
                party[s].ribbons.set(r);
            break; }
        case 0x0233: {                                  // GetPartyMonEvTotal dest,slot
            int s = _flex((int)_arg[1]), n = 0;
            if (s >= 0 && s < (int)party.size())
                for (int k = 0; k < 6; k++) n += party[s].evs[k];
            set_var((int)_arg[0], n); break; }
        case 0x012F: set_var((int)_arg[1], 0); break;   // ContestPhotoHasData bank,dest
        case 0x01C1: set_var((int)_arg[0], 0); break;   // CalcSizeContestResult dest,slot
        case 0x01C2: break;                             // UpdateSizeContestRecord
        case 0x01C3: break;                             // BufferPartyPokemonSize
        case 0x0218: {                                    // GetRandomSeenSpecies
            int sp = 25;                                // DS : Pikachu si dex vide
            if (!dex_seen.empty()) {
                int r = rand() % (int)dex_seen.size(), i = 0;
                for (int s2 : dex_seen) if (i++ == r) { sp = s2; break; }
            }
            set_var((int)_arg[0], sp); break; }
        case 0x0219: break;                             // SetNewsPressDeadline
        case 0x0277: set_var((int)_arg[0], rand() % 46 + 5); break; // GetDailyRandomLevel
        case 0x01B8: set_var((int)_arg[0],             // GetRandom2 dest,max
            _flex((int)_arg[1]) ? rand() % _flex((int)_arg[1]) : 0); break;
        // ---- trades PNJ / tutors / menus party ----
        case 0x0191: break;                             // SelectMoveTutorPokemon
        case 0x0224: break;                             // OpenMoveTutorMenu
        case 0x0225: set_var((int)_arg[0], 0); break;   // CheckLearnedTutorMove
        case 0x0226: trade_id = _flex((int)_arg[0]); break;  // InitNpcTrade id
        case 0x0228: {                                  // GetNpcTradeRequestedSpecies
            int req = 0;
            if (cb_trade && trade_id >= 0) {
                int sp, it, ot; std::string nk;
                cb_trade(trade_id, &sp, &req, &it, &ot, &nk);
            }
            set_var((int)_arg[0], req); break; }
        case 0x0229: {                                  // StartNpcTrade slot
            int s = _flex((int)_arg[0]);
            if (s >= 0 && s < (int)party.size() &&
                cb_trade && trade_id >= 0) {
                int sp = 0, req = 0, it = 0, ot = 0; std::string nk;
                cb_trade(trade_id, &sp, &req, &it, &ot, &nk);
                if (party[s].species == req && !party[s].egg &&
                    (it == 0 || it == party[s].held)) {
                    PartyMon give{};                       // reçu
                    give.species = sp; give.level = party[s].level;
                    if (cb_moninit) cb_moninit(give);      // moves/IVs/PID/PV
                    give.nick = nk; give.otid = ot;        // PNJ = DO
                    give.held = it;
                    dex_caught.insert(give.species);
                    dex_seen.insert(give.species);
                    party[s] = give;
                }
            }
            break; }
        case 0x022A: trade_id = -1; break;              // FinishNpcTrade
        case 0x02A5: pick_kind = 0; _st = PARTYPICK; break;  // OpenPartyMenuForTrade
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
        case 0x0261:                                      // BufferAccessoryName
            if (cb_buf) cb_buf((int)_arg[0], 97, _flex((int)_arg[1])); break;
        case 0x0343:                                      // BufferAccessoryNameWithArticle
            if (cb_buf) cb_buf((int)_arg[0], 93, _flex((int)_arg[1])); break;
        case 0x0344:                                      // BufferTrainerClassNameWithArticle
            if (cb_buf) cb_buf((int)_arg[0], 94, _flex((int)_arg[1])); break;
        case 0x0345:                                      // BufferBallSealNamePlural
            if (cb_buf) cb_buf((int)_arg[0], 95, _flex((int)_arg[1])); break;
        case 0x0346:                                      // CapitalizeFirstLetter
            if (cb_buf) cb_buf((int)_arg[0], 96, 0); break;
        case 0x0272: set_var((int)_arg[0], 0); break;  // OpenShayminTabletNamingScreen (UI nom nd)
        case 0x02EF:                                      // BufferStatName
            if (cb_buf) cb_buf((int)_arg[0], 98, _flex((int)_arg[1])); break;
        case 0x033A: menu_ox = _flex((int)_arg[0]); break; // SetMenuXOriginSide
        case 0x033B: menu_oy = _flex((int)_arg[0]); break; // SetMenuYOriginSide
        // ---- RTC/events objets divers ----
        case 0x0055: break;                             // FadeInBGM
        case 0x00C5: break;                             // PlayHmCutIn
        case 0x0151: break;                             // FieldCommenterBattleRoom
        case 0x0270: break;                             // SetHiddenLocation
        case 0x0333: break;                             // SetPlayerVolume
        case 0x0269: break;                             // ShakeObject
        case 0x026D:                                      // MessageUnown (police Unown)
            if (cb_msg) cb_msg((int)_arg[0], false);
            _st = MSG; break;
        case 0x02C0: break;                             // MessageAutoScroll
        case 0x026E: set_var((int)_arg[0], 0); break;   // GetGBACartridgeVersion
        case 0x027D: set_var((int)_arg[0], 0); break;   // TryBufferAndUnlockRandomToughWord
        case 0x027F: set_var((int)_arg[0], 0); break;   // CheckAllToughWordsUnlocked
        case 0x0271: set_var((int)_arg[0], 1); break;   // ShayminTablet : NO_INPUT
        // ---- centre Pokémon / dépôt / jeu ----
        case 0x027E: set_var((int)_arg[0], 1); break;   // CheckIsDepartmentStoreRegular
        case 0x0276: set_var((int)_arg[0],            // CheckCanAddCoins
            coins + _flex((int)_arg[1]) <= 50000 ? 1 : 0); break;
        case 0x01B5: break;                             // StartRankingsMachine
        // ---- parcs / pal park / catching show ----
        case 0x0255: break;                             // MoveCatchingShowMonsToPcBoxes
        case 0x0256: set_var((int)_arg[1], 0); break;   // CalcCatchingShowPoints
        case 0x0257: break;                             // ShowAccessoryShop
        case 0x029B: break;                             // GetExchangeServiceCornerItemAndCost
        case 0x0210: set_var((int)_arg[1], cb_gym
            ? cb_gym(18, (int)_arg[0]) : 0); break;     // CheckGreatMarshTramLocation
        case 0x020F: if (cb_gym) cb_gym(17,             // MoveGreatMarshTram
            _flex((int)_arg[0]) | ((int)_arg[1] << 8)); break;
        // ---- mécaniques de gym / lifts ----
        case 0x016F: if (cb_gym) cb_gym(0, 0); break;   // InitPersistedMapFeaturesForPastoriaGym
        case 0x0170: if (cb_gym) cb_gym(1, 0); break;   // PressPastoriaGymButton
        case 0x0171: if (cb_gym) cb_gym(10, 0); break;  // InitPersistedMapFeaturesForHearthomeGym
        case 0x0172: if (cb_gym) cb_gym(11, 0); break;  // MoveHearthomeGymDpLift
        case 0x0173: if (cb_gym) cb_gym(9, 0); break;   // InitPersistedMapFeaturesForCanalaveGym
        case 0x0174: if (cb_gym) cb_gym(29, 0); break;  // InitPersistedMapFeaturesForVeilstoneGym
        case 0x0175: if (cb_gym) cb_gym(5, (int)_arg[0]); break; // InitPersistedMapFeaturesForSunyshoreGym
        case 0x0176: if (cb_gym) cb_gym(6, (int)_arg[0]); break; // PressSunyshoreGymButton
        case 0x02C9: if (cb_gym) cb_gym(7, 0); break;   // InitPersistedMapFeaturesForEternaGym
        case 0x02CA: if (cb_gym) cb_gym(8, 0); break;   // AdvanceEternaGymClock
        case 0x025B: if (cb_gym) cb_gym(13, 0); break;  // InitPersistedMapFeaturesForPlatformLift
        case 0x025C: if (cb_gym) cb_gym(14, 0); break;  // TriggerPlatformLift
        case 0x025D: set_var((int)_arg[0], cb_gym ? cb_gym(15, 0) : 0); break; // CheckPlatformLiftNotUsedWhenEnteredMap
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
        case 0x0319:                                      // StartGiratinaOriginBattle sp,lv
            battle_sp = _flex((int)_arg[0]); battle_lv = _flex((int)_arg[1]);
            _st = BATTLE; break;
        case 0x0314: set_var((int)_arg[0],          // GetBattleResult
            battle_result != 2 && battle_result != 3); break;
        // ---- monde distorsion / events légendaires ----
        case 0x0311: if (cb_gym) cb_gym(22, _flex((int)_arg[0])); break; // AddDistortionWorldMapObject
        case 0x0312: if (cb_gym) cb_gym(23, _flex((int)_arg[0])); break; // DeleteDistortionWorldMapObject
        case 0x031F: if (cb_gym) cb_gym(25, 0); break;    // ResetDistortionWorldCameraAngles
        case 0x0320: if (cb_gym) cb_gym(26, 0); break;    // DoDWWarp (vortex)
        case 0x0321: if (cb_gym) cb_gym(27, _flex((int)_arg[0])); break; // StartGiratinaShadowEvent
        case 0x0322: if (cb_gym) cb_gym(28, 0); break;    // FinishGiratinaShadowEvent
        case 0x023D: break;                             // PlayBoatCutscene
        // ---- place / divers ----
        case 0x00A8: set_var((int)_arg[1], 0); break;   // 0A8 hw,ptr
        case 0x01BF: set_var((int)_arg[0],              // CheckDaycareHasEgg
            daycare_egg ? 1 : 0); break;
        case 0x01B0: {                                  // GivePokeDCare partySlot
            int s = _flex((int)_arg[0]);
            if (s >= 0 && s < (int)party.size() && !party[s].egg) {
                int slot = daycare[0].species ? (daycare[1].species ? -1 : 1) : 0;
                if (slot >= 0) {
                    daycare[slot] = party[s];
                    daycare[slot].dc_gain = 0;
                    party.erase(party.begin() + s);
                    if (daycare[0].species && daycare[1].species) {
                        // compatibilité DS : score 20 + 20 mème espèce +
                        // 20 DO différent ; Métamorph passe avec tout
                        auto& a = daycare[0]; auto& b = daycare[1];
                        int sc = 20;
                        if (a.species == 132 || b.species == 132) sc = 70;
                        else {
                            int ga = cb_mgender ? cb_mgender(a.species, a.pid) : 2;
                            int gb = cb_mgender ? cb_mgender(b.species, b.pid) : 2;
                            if (ga + gb != 1) sc = 0;      // pas M+F
                            else {
                                if (a.species == b.species) sc += 20;
                                if (a.otid != b.otid) sc += 20;
                            }
                        }
                        daycare_compat = sc >= 70 ? 0 : sc >= 50 ? 1 :
                                         sc >= 20 ? 2 : 3;
                    }
                }
            }
            break; }
        case 0x01A4: {                                  // TakePokemonDCare dest,dcSlot
            int s = _flex((int)_arg[1]), ok = 0;
            if ((s == 0 || s == 1) && daycare[s].species &&
                party.size() < 6) {
                int price = 100 + 100 * daycare[s].dc_gain;
                if (money >= price) money -= price;
                party.push_back(daycare[s]);
                daycare[s] = PartyMon{};
                daycare_egg = false;                     // œuf perdu DS
                daycare_compat = -1;
                ok = 1;
            }
            set_var((int)_arg[0], ok); break; }
        case 0x01AA: {                                  // SetVarPokeAndMoneyDCare dest,dcSlot
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0], (s == 0 || s == 1) && daycare[s].species
                ? 100 + 100 * daycare[s].dc_gain : 0); break; }
        case 0x01AE: {                                  // SetVarPokeAndLevelDCare dest,dcSlot
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0], (s == 0 || s == 1) && daycare[s].species
                ? daycare[s].dc_gain : 0); break; }
        case 0x01BC: {                                  // SetVarDataDayCare n,l,g,dcSlot
            int s = _flex((int)_arg[3]);
            if (cb_buf && (s == 0 || s == 1) && daycare[s].species) {
                cb_buf(_flex((int)_arg[0]), 103, s);      // surnom
                cb_buf(_flex((int)_arg[1]), 20,
                       daycare[s].level + daycare[s].dc_gain);
                int g = cb_mgender ? cb_mgender(daycare[s].species,
                                                daycare[s].pid) : 2;
                cb_buf(_flex((int)_arg[2]), 105, g);      // symbole genre
            }
            break; }
        case 0x01BE: set_var((int)_arg[0],              // StorePokeDCareLove
            daycare_compat < 0 ? 3 : daycare_compat); break;
        case 0x01A8: daycare_steps = 0; break;          // ActEggDayCMan (reset compteur)
        case 0x02DF: set_var((int)_arg[0], 0); break;   // CalcAmitySquareBerryAndAccessoryManOptionId
        case 0x02E0: set_var((int)_arg[1], 0); break;   // CheckAmitySquareManGiftIsAccessory
        case 0x02E1: set_var((int)_arg[1], 0); break;   // GetAmitySquareBerryOrAccessoryIdFromMan
        case 0x0216: set_var((int)_arg[0], 0); break;   // GetAmitySquareStepCount
        case 0x020D:                                    // SetvarMtCoronet : DS
            // ov6_02243004(fs,param) — params 1/6 = sondes « l'effet caméra
            // distorsion est fini » que les scripts attendent en boucle
            // (scr237 pilier) ; sans l'effet on répond « fini » sinon
            // softlock. Params init/cleanup (0,2-5,7-9) -> 0.
            set_var((int)_arg[1],
                ((int)_arg[0] == 1 || (int)_arg[0] == 6) ? 1 : 0); break;
        case 0x018C: break;                             // 18C var,var
        case 0x02B6: if (cb_nocoll)                     // ScrCmd_2B6 : sub_02062D80
            cb_nocoll(_flex((int)_arg[0]),              // DS : byte=0 -> STATUS_18
                      _arg[1] == 0);                    // (fantôme), 1 -> solide
            break;
        case 0x02BB: break;                             // 2BB
        case 0x005D: break;                             // SetSubScene63
        case 0x01F9: break;                             // Dummy1F9
        case 0x029E: {                                  // StartDestroyObstaclesAnimation
            if (cb_fld) cb_fld(7, _flex((int)_arg[0]), 0);
            set_var((int)_arg[1], 1);                   // DS : yield jusqu'à fin anim
            wait_t = 1; _st = WAITTIME; break; }
        case 0x01CF: case 0x01D0: case 0x01D1: {        // DoStrength/Flash/DefogFunc
            // FIELD_MOVE_FUNC DS : SET=0 CLEAR=1 -> 0 arg, CHECK=2 -> dest ;
            // flags STRENGTH_ACTIVE 2402 / FLASH_ACTIVE 2426 / DEFOG_ACTIVE 2427
            static const int FMF[3] = {2402, 2426, 2427};
            int fl = FMF[op - 0x01CF];
            if (_arg[0] == 0) set_flag(fl, true);
            else if (_arg[0] == 1) set_flag(fl, false);
            else if (_arg[0] == 2) set_var((int)rh(), flag(fl) ? 1 : 0);
            break; }
        case 0x0235: {                                  // CallTVBroadcast (arité variable)
            static const char* W[6] = {"h","hhh","hhh","","hh","hhh"};
            int sub = (int)_arg[0];
            tv_req = true; _st = DONE;                  // Télé-Motisma (overlay)
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
        case 0x0263: {                                  // ChangeDeoxysForm form
            int f = _flex((int)_arg[0]);
            for (auto& m : party)
                if (!m.egg && m.species == 386) m.form = f & 3;
            break; }
        case 0x01E5: records[_flex((int)_arg[0])]++; break; // IncrementGameRecord
        case 0x00FF: break;                             // BufferContestRegistrationEntryNumber
        case 0x029F: break;                             // 29F
        case 0x02D8: shop_items.clear(); _st = SHOP; break; // PokeMartFrontier
        case 0x00BA: break;                             // Unused_0BA
        case 0x01BB: {                                    // DecreasePartyMonFriendship
            int v = _flex((int)_arg[0]), s = _flex((int)_arg[1]);
            if (s >= 0 && s < (int)party.size() &&
                party[s].friendship > v) party[s].friendship -= v;
            else if (s >= 0 && s < (int)party.size())
                party[s].friendship = 0;
            break; }
        case 0x032D: break; case 0x0331: break; case 0x032E: break;
        case 0x023A: {                                  // GetPartyMonFootprintType has,type,slot
            int s = _flex((int)_arg[2]), ok = 0, ty = 0;
            if (s >= 0 && s < (int)party.size() && !party[s].egg &&
                party[s].species > 0) { ok = 1; ty = (party[s].species - 1) & 15; }
            set_var((int)_arg[0], ok); set_var((int)_arg[1], ty); break; }
        case 0x0292: {                                  // CheckShouldShowGhost pct,dest
            int p = _flex((int)_arg[0]);
            if (p < 0) p = 0; if (p > 100) p = 100;      // DS : min(max(p,0),100)
            set_var((int)_arg[1], (int)(rand() % 100) < p ? 1 : 0); break; }
        case 0x0317: {                                  // GetPlayer3DPos x,y,z
            int dx = cb_gym ? cb_gym(24, 0) : -9999;   // DW : coords partagées
            if (dx != -9999) {
                set_var((int)_arg[0], dx);
                set_var((int)_arg[1], cb_gym(24, 1));
                set_var((int)_arg[2], cb_gym(24, 2));
            } else if (cb_pos) {
                set_var((int)_arg[0], cb_pos(0));
                set_var((int)_arg[1], cb_pos(2));       // y sol /8 -> unités DS
                set_var((int)_arg[2], cb_pos(1)); }
            break; }
        case 0x016E: {                                  // GetDaycareState (DS)
            int n = (daycare[0].species ? 1 : 0) +
                    (daycare[1].species ? 1 : 0);
            set_var((int)_arg[0],
                daycare_egg ? 1 : (n ? n + 1 : 0)); break; }
        case 0x00A2: break; case 0x0204: break;         // 0A2 / WarpLastElevator (nd)
        case 0x0135: break;                             // 135
        case 0x00AA: break;                             // OpenRegionMap (UI nd)
        case 0x0103: break;                             // BufferContestType
        case 0x014C:                                    // SetBlackOutWarpId warpId
            if (cb_setbo) cb_setbo(_flex((int)_arg[0])); break;
        case 0x02CD: break;                             // 2CD
        case 0x031B: set_var((int)_arg[0], safari_caught); break;  // GetCurrentSafariGameCaughtNum
        case 0x026C: set_var((int)_arg[0], 0); break;   // TryGetRandomMassageGirlAccessory
        case 0x027A: break;                             // UseVistaLighthouseBinoculars
        case 0x028F: set_var((int)_arg[0], hof_entries); break;  // GetLeagueVictories
        case 0x02BE: {                                  // GetTrainerCardLevel : nb étoiles DS
            int st = 0;
            if (badges == 8) st++;
            if (hof_entries > 0) st++;
            // dex national complet
            { int n = 0;
              for (int sp : dex_caught) if (sp >= 1 && sp <= 493) n++;
              if (n >= 482) st++; }
            set_var((int)_arg[0], st); break; }
        case 0x01F7: {                                    // SurvivePoison dest,slot
            // DS Pokemon_TrySurvivePoison : empoisonné + PV==1 -> le poison
            // s'estompe (survit à 1 PV). Bits cond : 0x08 psn + 0x80 toxique.
            int s = _flex((int)_arg[1]), ok = 0;
            if (s >= 0 && s < (int)party.size() &&
                (party[s].cond & 0x88) && party[s].hp == 1) {
                party[s].cond = 0; ok = 1;
            }
            set_var((int)_arg[0], ok); break; }
        case 0x018F: break;                             // StoreSaveResult
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
        case 0x01A9: {                                  // GiveEggFromDaycare
            if (daycare_egg && party.size() < 6) {
                int sp = 0;
                for (auto& d : daycare)                     // espèce de la mère
                    if (d.species && d.species != 132) {
                        int g = cb_mgender ? cb_mgender(d.species, d.pid) : 0;
                        if (g == 1 || !sp) sp = d.species;
                        if (g == 1) break;                  // femelle = mère
                    }
                if (!sp) sp = daycare[0].species ? daycare[0].species
                                               : daycare[1].species;
                if (cb_eggsp) sp = cb_eggsp(sp);            // forme bébé
                PartyMon e{}; e.species = sp; e.level = 1;
                e.hp = -1; e.egg = true;
                if (cb_moninit) cb_moninit(e);
                e.egg = true;                               // rester œuf
                party.push_back(e);
                daycare_egg = false; daycare_steps = 0;
            }
            break; }
        case 0x01AC: {                                  // HatchEgg : 1er œuf -> éclos
            for (auto& p : party) if (p.egg) {
                p.egg = false; p.hp = -1;               // -1 -> PV max au montage
                dex_caught.insert(p.species);
                break; }
            break; }
        case 0x01AF: {                          // BufferPartyMonNicknameReturnSpecies
            int s = _flex((int)_arg[1]);                // _arg[0] ignoré (tampon 0)
            if (cb_buf) cb_buf(0, 92, s);
            int sp = (s >= 0 && s < (int)party.size()) ? party[s].species : 0;
            set_var((int)_arg[2], sp); break; }
        case 0x0290: pick_kind = 1; _st = PARTYPICK; break;  // OpenPartyMenuForDaycare
        case 0x0291:                                    // GetDaycarePartyMenuResult
            set_var((int)_arg[0], _summary_slot);       // slot ou 0xFF
            set_var((int)_arg[1], 0);                   // showSummary = faux
            break;
        case 0x031E: break;                             // TryRevertPokemonForm
        case 0x00B3: break;                             // 0B3
        case 0x02F6: set_var((int)_arg[2], 0); break;   // 2F6 var,var,ptr
        case 0x0265: poketch_hidden = true; break;      // HidePoketch
        case 0x0266: poketch_hidden = false; break;     // ShowPoketch
        case 0x0116: break;                             // ShowLinkContestRecords
        case 0x00F6: break;                             // StartLinkBattle
        case 0x00A3: break; case 0x01F8: break;         // 0A3 / WaitForTransition
        case 0x0007: case 0x0008: case 0x0006: break;   // UNUSED_007/008/006
        case 0x026A: break;                             // FlickerObject
        case 0x0328: {                                  // SetPartyGiratinaForm
            int f = _flex((int)_arg[0]);
            for (auto& m : party)
                if (m.species == 487 && !m.egg) m.form = f;
            break; }
        case 0x025E: break;                             // InitLakeGuardianContainmentUnits
        case 0x0253: break;                             // SetClearInCatchingShowFlag
        case 0x02C5: case 0x02D5: case 0x02DC: break;   // DeleteActive*Streak
        case 0x00BF: break;                             // UseRockClimb (anim nd)
        case 0x00C0: if (cb_surf) cb_surf(); break;     // UseSurf -> monter sur l'eau
        // parcelles à baies : l'objet cible du script donne le patchID (d0)
        case 0x017D: set_var((int)_arg[0], cb_berry ? cb_berry(0, 0) : 0); break; // GetBerryGrowthStage
        case 0x017E: set_var((int)_arg[0], cb_berry ? cb_berry(1, 0) : 0); break; // GetBerryItemID
        case 0x017F: set_var((int)_arg[0], cb_berry ? cb_berry(2, 0) : 0); break; // GetBerryMulchType
        case 0x0180: set_var((int)_arg[0], cb_berry ? cb_berry(3, 0) : 0); break; // GetBerryMoisture
        case 0x0181: set_var((int)_arg[0], cb_berry ? cb_berry(4, 0) : 0); break; // GetBerryYield
        case 0x0182: if (cb_berry) cb_berry(5, _flex((int)_arg[0])); break;       // SetBerryMulch
        case 0x0183: if (cb_berry) cb_berry(6, _flex((int)_arg[0])); break;       // PlantBerry
        case 0x0184: if (cb_berry) cb_berry(7, (int)_arg[0]); break;              // SetBerryWateringState
        case 0x0185: if (cb_berry) cb_berry(8, 0); break;                         // HarvestBerry
        case 0x0323: set_var((int)_arg[0], 0); break;   // CheckNoWifiPlazaCooldown
        case 0x0308: if (cb_fld) cb_fld(5, _flex((int)_arg[0]),
            _flex((int)_arg[1])); break;                // AddCameraOverrideObject
        case 0x0309: if (cb_fld) cb_fld(6, 0, 0); break; // RemoveCameraOverrideObject
        case 0x0200: set_var((int)_arg[0],              // GetPreviousMapId
            cb_prevmap ? cb_prevmap() : 0); break;
        case 0x020E: if (cb_gym) cb_gym(16, 0); break;  // InitGreatMarshTram
        case 0x0211: if (cb_gym) cb_gym(19,             // SetPlayerHeightCalc
            (int)_arg[0]); break;
        case 0x020A: break;                             // StartVsSeeker
        case 0x0294: break;                             // ShowBattlePoints
        case 0x016D: if (cb_buf) {                       // BufferDaycareMonNicknames
                cb_buf(0, 103, 0); cb_buf(1, 103, 1); } break;
        case 0x0048: break;                             // ShowMenuMultiColumn
        case 0x0212: {                                    // GetPartyMonNature dest,slot
            int s = _flex((int)_arg[1]);
            set_var((int)_arg[0], (s >= 0 && s < (int)party.size())
                ? (int)(party[s].pid % 25) : 0); break; }
        case 0x017C: if (cb_buf) cb_buf(_flex((int)_arg[0]), 102,  // BufferNatureName arg,nature
                             _flex((int)_arg[1])); break;
        case 0x0107: set_var((int)_arg[0], 0); break;   // GetPlayerContestPlacement
        case 0x030C: break;                             // SaveTVSegmentHomeAndManorNoFurniture
        case 0x030D: break;                             // SaveTVSegmentHomeAndManor (segments TV)
        case 0x02F0: if (cb_gym) cb_gym(20, 0); break;  // InitPersistedMapFeaturesForVilla
        // -- tâches d'approche dresseur (trainer_encounter.c) : le moteur a
        // rempli les slots TRAINER_0/1 ; le script pilote la marche via B6-B9
        case 0x00B6: if (cb_appr) cb_appr(_flex((int)_arg[0]), 0); break;
        case 0x00B7: {                                  // CheckApproachTaskDone
            int n = _flex((int)_arg[0]);                // DS : return TRUE = yield frame
            set_var((int)_arg[1], cb_appr ? cb_appr(n, 1) : 1);
            wait_t = 1; _st = WAITTIME; break; }
        case 0x00B8: set_var((int)_arg[0],
            cb_appr ? cb_appr(0, 3) : 0); break;        // GetApproachingTrainerType
        case 0x00B9: set_var((int)_arg[1],
            cb_appr ? cb_appr(_flex((int)_arg[0]), 2) : 0); break;
        case 0x01D2: accessories[_flex((int)_arg[0])] +=
            _flex((int)_arg[1]); break;                 // AddAccessory
        case 0x01D5: accessories[0x8000 + _flex((int)_arg[0])] = 1; break; // AddContestBackdrop
        case 0x0282: {                                  // CheckIsTodayPlayerBirthday
            time_t tt = time(nullptr); struct tm lt{};
            localtime_s(&lt, &tt);
            set_var((int)_arg[0],
                (birth_m == lt.tm_mon + 1 && birth_d == lt.tm_mday) ? 1 : 0);
            break; }
        case 0x033D: if (cb_buf) cb_buf((int)_arg[0], 90,
            _flex((int)_arg[1])); break;                // BufferItemNamePlural
        case 0x0342: if (cb_buf) cb_buf((int)_arg[0], 91,
            starter_id(cpart_starter)); break;          // counterpart starter + article
        case 0x0123: {                                  // LoadPokedexRating
            int seen = (int)dex_seen.size();
            static const int LIM[] = {15,30,45,60,80,100,120,140,160,180,200};
            int mid = 0;
            while (mid < 11 && seen > LIM[mid]) mid++;
            set_var((int)_arg[1], mid);                 // PokedexRatings_Text_*
            break; }
        case 0x0126: battle_tutorial = 1; _st = BATTLE; break; // StartCatchingTutorial
        case 0x022B: break;                             // TurnOnPokedexLanguageDetection
        case 0x0059: set_var((int)_arg[0], 0); break;   // CheckRecordedChatotCryIsPlayable
        case 0x0120: set_var((int)_arg[0],
            (int)dex_seen.size()); break;              // GetNationalDexSeenCount
        case 0x01DB: break;                             // InitBattleTower
        case 0x02A2: {                                  // TrySetUnusedCollectedOrbFlag
            int it = _flex((int)_arg[0]);
            if (it == 135 || it == 136) set_flag(0xF000, true);  // orbe souterrain
            break;
        }
        case 0x0341:                                      // BufferSpeciesNameWithArticle
            if (cb_buf) cb_buf((int)_arg[0], 91, _flex((int)_arg[1])); break;
        case 0x00C3: break; case 0x00C4: break;         // Flash/Defog anims
        case 0x028C: break;                             // DrawPokemonPreviewFromPartySlot
        default:
            if (cb_unimpl) cb_unimpl(od->name, op);
            break;
        }
    }
    return _st;
}
