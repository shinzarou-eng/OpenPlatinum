# scr_seq.narc and the field script VM

`/fielddata/script/scr_seq.narc` holds every field script. Each member is a
table of u32 relative offsets (terminated by `0xFD13`) pointing at function
bodies. A body is a flat stream of `u16 opcode + params`. `ScrCmd_End`
(0x0002) and `ScrCmd_Return` (0x0024) terminate it — anything after belongs
to the next function, so a parser that ignores `Return` desyncs and reads
parameter bytes as opcodes (phantom ops like 0x800C/0x4020 are exactly that).

## Opcode IDs are the command table index

Opcode `N` is `gFieldScriptCommands[N]` — the table in
`src/field/scr_overworld.c`. Do NOT use Diamond/Pearl documentation: the
Platinum table was rebuilt and the param widths differ all over the place.
Floating "scrcmd width" tables on the internet are DP-era. Every width in
`cpp/src/op_table.inc` was re-derived by reading the `ScrCmd_*` bodies
(`Script_ReadByte/Halfword/Word` calls in order). Examples that are wrong
in the DP-era tables:

    opcode  handler                       real width
    0x00A1  ScrCmd_WaitTime               hh  (not hhh)
    0x006C  ScrCmd_SetObjectEventPos      bhh
    0x0120  ScrCmd_OpenYesNoMenu          h
    0x0279  ScrCmd_CountPartyNonEggMons   h

Being off by one byte doesn't crash — it desyncs every following command.

## Variable-arity commands

Eight commands take a sub-function selector and then a *different* number of
args per case — the callee reads them itself:

    CallTVBroadcast, CallTVInterview, DoGroupConnectionAction,
    MysteryGiftGive, DoStrengthFunc, DoFlashFunc, DoDefogFunc, ...

The VM has to read the selector then the right arg count for that case, or
the stream desyncs. A fixed-width table cannot represent these.

## Scripts types

- Type 1 (init scripts): table of `(var, expected_value, func)` — run once
  per frame while `var==value`. Used for entry cutscenes.
- Type 2/3/4: fixed script slots.
- Objects/warps/coord/bg events live in `zone_event` (see `maps.md`),
  objects carry a script index into the map's script archive.

## Runtime notes

- `LockAll` freezes the player + objects, `ReleaseAll` resumes. Movement is
  gated on `vm.busy()` — input during a scene must be eaten, not queued.
- `ApplyMovement` + `WaitMovement`: movement lists are u16 command streams
  (walk, jump, teleport, disappear) shared between player and NPCs.
- `StartTrainerBattle` suspends the VM (a dedicated state) and resumes at
  the next command after the battle ends; `CheckWonBattle` then branches.
- Buffers `{103:n:*}` in message text resolve through buffer vars written
  by commands like `BufferPartyMonNickname` / `BufferItemName`.

`tools/audit_opcodes.py` walks all 2698 script functions against the width
table and reports ops not implemented by the VM.
