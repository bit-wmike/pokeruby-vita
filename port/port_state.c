/*
 * Savestate (one slot, savestate.bin in the data folder).
 *
 * The whole game state lives in a few known places: the GBA memory arrays
 * (EWRAM, IWRAM, I/O, palettes, VRAM, OAM) and the game's own variables
 * (sections ewram_data, iwram_data, game_data, game_bss - the game is
 * compiled without -fdata-sections so they are whole). Saving copies them,
 * loading copies them back. That is done only between two frames of the
 * game's main loop (AgbMain waiting for VBlank), so the C stack is the same
 * at saving and loading. Pointers in the state are addresses of this very
 * program: a state only fits the version that wrote it. Some game structs
 * keep pointers to stack variables (a sprite's template copied on the
 * stack); they are moved along when the stack is elsewhere at loading.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "port/port.h"
#include "port/port_config.h"

extern unsigned char gHostOam[0x400];
extern unsigned char __start_game_bss[], __stop_game_bss[];
extern unsigned char __start_game_data[] __attribute__((weak)), __stop_game_data[] __attribute__((weak));
extern unsigned char __start_iwram_data[] __attribute__((weak)), __stop_iwram_data[] __attribute__((weak));
extern unsigned char __start_ewram_data[] __attribute__((weak)), __stop_ewram_data[] __attribute__((weak));
void *HostDma_State(unsigned *size);
extern unsigned char gUnknown_030007B8[0x770];   /* m4a work area (host_m4a.c) */
void PortStage_Reset(void);
void PortView_Reset(void);

int gPortInMainWait;   /* set by src/main.c around its VBlank wait */
static int sPending;   /* 1 save, 2 load */
static int sResult;    /* last result for the menu: 1 ok, -1 failed */

struct Block { void *p; unsigned size; };

static int Blocks(struct Block *b)
{
    int n = 0;
    unsigned dmaSize;
    void *dma = HostDma_State(&dmaSize);
    b[n].p = gHostEwram;   b[n++].size = sizeof(gHostEwram);
    b[n].p = gHostIwram;   b[n++].size = sizeof(gHostIwram);
    b[n].p = gHostIo;      b[n++].size = sizeof(gHostIo);
    b[n].p = gHostPalram;  b[n++].size = sizeof(gHostPalram);
    b[n].p = gHostVram;    b[n++].size = sizeof(gHostVram);
    b[n].p = gHostOam;     b[n++].size = sizeof(gHostOam);
    b[n].p = __start_game_bss; b[n++].size = (unsigned)(__stop_game_bss - __start_game_bss);
    if (__start_game_data) { b[n].p = __start_game_data; b[n++].size = (unsigned)(__stop_game_data - __start_game_data); }
    if (__start_iwram_data) { b[n].p = __start_iwram_data; b[n++].size = (unsigned)(__stop_iwram_data - __start_iwram_data); }
    if (__start_ewram_data) { b[n].p = __start_ewram_data; b[n++].size = (unsigned)(__stop_ewram_data - __start_ewram_data); }
    b[n].p = gUnknown_030007B8; b[n++].size = sizeof(gUnknown_030007B8);
    b[n].p = dma;          b[n++].size = dmaSize;
    return n;
}

struct Header {
    char magic[8];
    char version[24];
    unsigned blocks, total;
    unsigned sizes[16];
    unsigned long stack;   /* an address on the stack at saving */
};

static void FillHeader(struct Header *h, const struct Block *b, int n)
{
    int i;
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "PKRSTATE", 8);
    snprintf(h->version, sizeof(h->version), "%s", PKR_PORT_VERSION);
    h->blocks = (unsigned)n;
    for (i = 0; i < n; i++) {
        h->sizes[i] = b[i].size;
        h->total += b[i].size;
    }
}

static int DoSave(uintptr_t anchor)
{
    struct Block b[16];
    struct Header h;
    int n = Blocks(b), i;
    unsigned char *buf, *p;
    FillHeader(&h, b, n);
    h.stack = (unsigned long)anchor;
    buf = malloc(sizeof(h) + h.total);
    if (!buf)
        return 0;
    memcpy(buf, &h, sizeof(h));
    p = buf + sizeof(h);
    for (i = 0; i < n; i++) {
        memcpy(p, b[i].p, b[i].size);
        p += b[i].size;
    }
    i = Platform_WriteFile("savestate.bin", buf, (unsigned long)(sizeof(h) + h.total));
    free(buf);
    return i == 0;
}

static int DoLoad(uintptr_t anchor)
{
    struct Block b[16];
    struct Header h, want;
    int n = Blocks(b), i;
    unsigned char *buf, *p;
    long got;
    FillHeader(&want, b, n);
    buf = malloc(sizeof(h) + want.total);
    if (!buf)
        return 0;
    got = Platform_ReadFile("savestate.bin", buf, sizeof(h) + want.total);
    memcpy(&h, buf, sizeof(h));
    want.stack = h.stack;
    if (got != (long)(sizeof(h) + want.total) || memcmp(&h, &want, sizeof(h))) {
        free(buf);   /* missing, or written by another version */
        return 0;
    }
    p = buf + sizeof(h);
    for (i = 0; i < n; i++) {
        memcpy(b[i].p, p, b[i].size);
        p += b[i].size;
    }
    free(buf);
    {   /* pointers to the stack (the anchor: a local of Port_StateFrame,
         * always at the same depth of the game's main loop) */
        uintptr_t now = anchor, then = (uintptr_t)h.stack;
        intptr_t delta = (intptr_t)(now - then);
        if (delta) {
            for (i = 0; i < n; i++) {
                uint32_t *w = (uint32_t *)b[i].p;
                unsigned k;
                for (k = 0; k < b[i].size / 4; k++)
                    if (w[k] >= then - 0x40000u && w[k] < then + 0x100000u)
                        w[k] = (uint32_t)(w[k] + delta);
            }
        }
    }
    PortStage_Reset();
    PortView_Reset();
    return 1;
}

void Port_StateRequest(int load)
{
    sPending = load ? 2 : 1;
    sResult = 0;
}

int Port_StateResult(void)
{
    return sResult;
}

int Port_StateExists(void)
{
    struct Header h;
    return Platform_ReadFile("savestate.bin", &h, sizeof(h)) == (long)sizeof(h) && !memcmp(h.magic, "PKRSTATE", 8) &&
           !strcmp(h.version, PKR_PORT_VERSION);
}

/* after a frame (host_core): a pending save/load at the main loop's wait */
void Port_StateFrame(void)
{
    volatile int anchor = 0;
    if (!sPending || !gPortInMainWait)
        return;
    sResult = (sPending == 1 ? DoSave((uintptr_t)&anchor) : DoLoad((uintptr_t)&anchor)) ? 1 : -1;
    Platform_Log("[state] %s %s\n", sPending == 1 ? "save" : "load", sResult > 0 ? "ok" : "failed");
    sPending = 0;
}
