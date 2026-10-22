/*
 * The GBA memory regions of the host / PS Vita port.
 *
 * RAM symbols that the GBA build places with its linker scripts
 * (gSharedMem, battle buffers, ...) are put at the same offsets inside these
 * arrays by the generated mem.s (tools/port/gen_mem.py).
 */
#include "port/port.h"

#define ALIGN64 __attribute__((aligned(64)))

/* gHostEwram and gHostIwram are defined in the generated mem.s. */
unsigned char gHostIo[0x400] ALIGN64;
unsigned char gHostPalram[0x400] ALIGN64;
unsigned char gHostVram[0x22000] ALIGN64;
unsigned char gHostOam[0x400] ALIGN64;
unsigned char gHostFlash[0x20000] ALIGN64;
