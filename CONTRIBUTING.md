# Contributing to OpenPlatinum

Thanks for taking a look. Bug reports, research notes and code are all welcome. This page explains
how the project works so your time isn't wasted.

## The one rule: match the DS

OpenPlatinum reimplements the game's behavior. It doesn't invent new behavior. Before changing how
something works, find the function in [pret/pokeplatinum](https://github.com/pret/pokeplatinum)
that does it on the DS, and follow that.

- Put the decomp function name in a comment next to your code, e.g.
  `// field_control.c Field_CheckMapTransition`. Reviewers check against it.
- If the DS does something odd, reproduce the odd thing. Fixes and enhancements belong behind an
  option, never in the default behavior.
- If you can't find the original logic, say so in the PR. A clearly marked approximation is fine,
  a silent one isn't.

## Legal: no game data, ever

- Never commit a ROM, extracted files, textures, models, sounds, text dumps or screenshots of
  ROM-only content.
- Tables derived from the decompilation (like `gfx_shadow.inc` or `headers.bin` via
  `tools/gen_headers.py`) are fine. They come from the decomp's source, not from the cartridge.
  Say in the file header where they came from.
- Don't copy code from other ports, emulators or recompilation projects unless their license
  allows it and you credit them. Reading them to understand a format is fine.

## Building and testing

See the README for the toolchain. Before opening a PR:

1. `cpp/build.bat` builds without new warnings.
2. You've tested the change in game. The harness makes this reproducible:
   ```bat
   OpenPlatinum.exe --map 411 --pos 116,887 --walk 60 n 30 --shot 200
   ```
   `--shot` writes a frame to `_shots/`. `--gif F1,F2` dumps a range of frames.
3. If you touched maps, events or warps, re-run the opening route: bedroom → 1F (Mom's scene)
   → Twinleaf → back inside. That route uses most of the field engine.
4. For script VM changes, run `python tools/audit_opcodes.py <rom>` and keep it at zero missing
   opcodes.

Useful debug variables: `SCRTRACE=1` (script trace), `WARPDBG=1`, `WARPAUDIT=1`, `OBJDBG=1`,
`MOVDBG=1`.

## Code style

- C++17, no exceptions. Follow the style of the surrounding code: compact, early returns, no
  deep nesting.
- Keep comments short and about *why*, or about which DS function the code mirrors. Existing
  comments are mostly in French. English is fine for new code.
- No new dependencies without discussing it in an issue first. SDL3, OpenGL and Dear ImGui are
  the whole stack.
- One topic per PR. A warp fix and a rendering change are two PRs.

## Reporting bugs

The most useful bug report contains:

- the map header ID and tile position (shown in the debug window, `F1`);
- what the DS does vs. what OpenPlatinum does. A clip or screenshot of each helps a lot;
- how to reproduce it, ideally as a harness command line;
- the console output with the relevant debug variable set.

## Research notes

Found something about a format or a DS routine that isn't documented? Open an issue or add a note
to `docs/`. The "Findings worth knowing" section of the README grows from these.
