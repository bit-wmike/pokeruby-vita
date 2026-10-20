/*
 * Glue for the m4a sound engine of the host / PS Vita port. The assembly
 * part (src/libs/m4a_1.s) is translated to C at build time
 * (tools/port/asm2c_flat.py -> m4a_gen.c); this file holds what the
 * assembly file defines besides code, and the stand-ins for the link
 * features the port does not have.
 */
#include <stdint.h>

unsigned char gUnknown_030007B8[0x770] __attribute__((aligned(4)));

#define STUB(name) void name(void) {}

/* BIOS SoundGetJumpList (swi 0x2A): unused, MPlayJumpTableCopy copies the
 * game's own template. */
STUB(MusicPlayerJumpTableCopy)

/* GameCube link (Pokémon Colosseum / Box): no link cable on the port. */
STUB(GameCubeMultiBoot_Main) STUB(GameCubeMultiBoot_Quit) STUB(GameCubeMultiBoot_Init)
STUB(GameCubeMultiBoot_HandleSerialInterrupt) STUB(GameCubeMultiBoot_ExecuteProgram)

/* InitIntrHandlers copies this to IWRAM; the port dispatches interrupts
 * itself (host_core.c). */
STUB(IntrMain)
