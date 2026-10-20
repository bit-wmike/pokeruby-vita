# Pokémon Saphir / Rubin (German) – native PS Vita port

A native port of the German Pokémon Sapphire / Ruby to the PS Vita, built
from the [pret/pokeruby](https://github.com/pret/pokeruby) decompilation (its README:
`README-decomp.md`, setup: `INSTALL.md`). No emulator: the game's C
code runs directly on the Vita.

## Status (0.2.0)

- German Sapphire runs on the host test build from the intro to the
  overworld, with music and sound; x86 and ARM builds produce bit-identical
  pictures and sound.
- PS Vita build: GBA view (240×160), display styles (smooth, sharp,
  pixel-exact), 30 fps option, START+SELECT menu, flash save in
  `ux0:data/pokemon_saphir/saphir.sav` (same format as emulator saves).
- "Weitsicht" (default): the overworld at 480×272 – the whole Vita screen at
  exactly 2× – with the map, the connected maps and the people around you
  really there (port/port_view.c). Text boxes and menus stay in the
  middle; battles, menus and other scenes are the GBA screen at the same
  zoom. Switch in the START+SELECT menu ("Ansicht").
- "Nur Hoenn" (menu, off by default): only third-generation Pokémon. Wild
  encounters use own tables (tools/port/gen3_wild.py, list in
  docs/nur_hoenn_wild.md); trainers, Team Aqua/Magma, static encounters
  and in-game trades get real Hoenn Pokémon; gym leaders, Elite Four,
  champion, Aqua/Magma leaders and Heiko keep their Pokémon (stats, moves,
  level) but look like a Hoenn Pokémon of the same kind (port/port_hoenn.c).
  Saves stay normal.
- Next: battle/menu presentation, weather at the edges, quick-save,
  map overlay; Ruby.

## Building

```
# German setup and the decomp's own build (generates graphics, maps, songs)
make clean && sh de_before.sh        # already applied on this branch
make sapphire_de CPP=cpp             # needs agbcc (pret/agbcc) in tools/
# headless Linux test build / ARM test build (qemu-arm) / PS Vita package
make -f Makefile.port TARGET=host
make -f Makefile.port TARGET=armhost
make -f Makefile.port TARGET=vita vpk     # needs $VITASDK
```
`GAME=ruby` (with `make ruby_de`) builds Ruby.

Personal LiveArea artwork made from the game's own graphics:
`python3 tools/port/make_livearea_local.py sapphire <title screenshot.ppm>` writes
`platform/vita/sce_sys_local_sapphire/` (git-ignored, used by `vpk`).

## How the port works

- `port/`: the GBA hardware in C – memory regions, DMA, BIOS calls,
  interrupts, flash save chip, real-time clock (system clock), sound output
  with PSG emulation, software PPU (`port/ppu`, from the Minish Cap /
  Zero Mission ports, renders on 3 cores).
- `tools/port/naked2c.py`: preprocesses the game's C for the port – the
  GBA's APCS struct layout (`aligned(4)`), assembly-only functions translated
  to C from the GBA build's objects.
- `tools/port/asm2c_flat.py`: the m4a sound engine's hand-written ARM/Thumb
  assembly translated into one C function with exact flags.
- `tools/port/as_host.py`, `gen_mem.py`: data assembly for x86, RAM symbols at
  their GBA offsets.
