# Battle data layout

All from the ROM, no tables hardcoded.

| what            | file                                        | record |
|-----------------|---------------------------------------------|--------|
| base stats      | poketool/personal/pl_personal.narc          | 44B    |
| moves           | poketool/waza/pl_waza_tbl.narc              | 16B    |
| learnsets       | poketool/personal/wotbl.narc                | var    |
| trainer params  | poketool/trainer/trdata.narc                | 20B    |
| trainer party   | poketool/trainer/trpoke.narc                | 8/16/10/18B per mon |
| item data       | itemdata/pl_itemdata.narc                   | 14B    |
| species names   | pl_msg bank 412, move names bank 647, items 392 | |
| sprites         | poketool/pokegra/pl_pokegra.narc (encrypted)| |

`pl_personal` SpeciesData: 6 base stats, 2 types, catch rate, exp curve,
ev yields, items, gender, egg groups/steps, abilities, sprite stuff.

`pl_waza_tbl` MoveTable: effect, class (0=phys / 1=spec / 2=status),
power, type, accuracy, pp, effect chance, target, priority, flags.
Note class 0 *is* physical — Tackle is class 0 and does damage.

`wotbl` learnset = `u16 move:9 | level:7` list until 0xFFFF; a mon's
moveset at level N = the last 4 entries with level <= N.

`trdata` header has `dataType` picking the trpoke member layout
(moves/no-items, items/no-moves, both). Party mons are raw
TrainerMon structs, species+level direct.

Damage (`BattleSystem_CalcMoveDamage`):

    (((2*level/5 + 2) * power * atk/def) / 50 + 2)
      * stab * typeEff * rand(85..100)/100

Type chart: `sTypeMatchupMultipliers` table (~70 triplets
atk-type/def-type/x1.0-as-10 ints, immunities and NVE stack).

Turn order = speed (with move priority), escape =
`speed*32 / (foe.speed/4) + 30*attempts` rolled vs 255.

Trainer battles are script-driven: `StartTrainerBattle` after the
approach/lockdown scene; `CheckWonBattle` branches the rest of the
script; `SetTrainerFlag` stops re-triggering.

Still missing: XP/level-up, capture, in-battle items/switching, AI,
doubles (the party layout flag is already parsed).
