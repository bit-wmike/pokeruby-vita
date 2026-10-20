# Pokémon Saphir / Rubin (German) – native PS Vita port

A native port of the German Pokémon Sapphire / Ruby to the PS Vita, built
from the [pret/pokeruby](https://github.com/pret/pokeruby) decompilation (see
`docs/` and `INSTALL.md` for the decomp itself). No emulator: the game's C
code runs directly on the Vita.

## Status (0.1.0)

- German Sapphire runs on the host test build from the intro to the
  overworld, with music and sound; x86 and ARM builds produce bit-identical
  pictures and sound.
- PS Vita build: GBA view (240×160), display styles (smooth, sharp,
  pixel-exact), 30 fps option, START+SELECT menu, flash save in
  `ux0:data/pokemon_saphir/saphir.sav` (same format as emulator saves).
- Next: "Weitsicht" – the overworld at 480×272 on the whole Vita screen.

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
