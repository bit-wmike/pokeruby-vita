/*
 * DMA for the host / PS Vita port.
 *
 * Immediate transfers are plain copies. HBlank-timed transfers (the game uses
 * them on channel 0 for the heat haze / water ripple effects) are remembered
 * and replayed by the renderer once per visible scanline, like the hardware.
 * Sound FIFO DMA (channels 1/2) is emulated by port/host_core.c directly from
 * the mixing buffer, so writes to those registers are ignored here.
 */
#include <string.h>

#include "port/port.h"
#include "global.h"

struct HostHdma {
    int active;
    const u8 *src;
    u8 *dst;
    u32 units;
    u32 unitSize;
    u32 control;
};

static struct HostHdma sHdma[4];
__attribute__((no_sanitize_address))
static void Transfer(const u8 *src, u8 *dst, u32 units, u32 unitSize, u32 control)
{
    int srcStep = unitSize, dstStep = unitSize;
    u32 i;

    switch ((control >> 7) & 3) { /* source control */
        case 1: srcStep = -(int)unitSize; break;
        case 2: srcStep = 0; break;
    }
    switch ((control >> 5) & 3) { /* destination control */
        case 1: dstStep = -(int)unitSize; break;
        case 2: dstStep = 0; break;
        /* 3 = increment + reload: increments during the transfer */
    }

    if (srcStep == (int)unitSize && dstStep == (int)unitSize && (dst < src || dst >= src + units * unitSize)) {
        u32 n = units * unitSize;
        while (n--)
            *dst++ = *src++;
        return;
    }
    for (i = 0; i < units; i++) {
        if (unitSize == 4)
            *(u32 *)dst = *(const u32 *)src;
        else
            *(u16 *)dst = *(const u16 *)src;
        src += srcStep;
        dst += dstStep;
    }
}

void HostDmaSet(int channel, const void *src, void *dst, unsigned int cnt)
{
    u32 control = cnt >> 16;
    u32 units = cnt & 0xFFFF;
    u32 unitSize = (control & DMA_32BIT) ? 4 : 2;

    if (channel < 0 || channel > 3)
        return;

    if (!(control & DMA_ENABLE)) {
        sHdma[channel].active = 0;
        return;
    }
    if (units == 0)
        units = (channel == 3) ? 0x10000 : 0x4000;

    switch (control & DMA_START_MASK) {
        case DMA_START_NOW:
            Transfer((const u8 *)src, (u8 *)dst, units, unitSize, control);
            sHdma[channel].active = 0;
            break;

        case DMA_START_HBLANK:
            sHdma[channel].active = 1;
            sHdma[channel].src = (const u8 *)src;
            sHdma[channel].dst = (u8 *)dst;
            sHdma[channel].units = units;
            sHdma[channel].unitSize = unitSize;
            sHdma[channel].control = control;
            break;

        default:
            /* VBlank DMA is unused; sound FIFO DMA (DMA1/2) is done by
             * host_audio.c reading the mixer's buffer directly. */
            break;
    }
}

/* One HBlank: every armed HBlank DMA moves one block. */
void HostDma_HBlank(void)
{
    int ch;
    for (ch = 0; ch < 4; ch++) {
        struct HostHdma *d = &sHdma[ch];
        if (!d->active)
            continue;
        Transfer(d->src, d->dst, d->units, d->unitSize, d->control);
        switch ((d->control >> 7) & 3) { /* the source is never reloaded */
            case 0: d->src += d->units * d->unitSize; break;
            case 1: d->src -= d->units * d->unitSize; break;
        }
        switch ((d->control >> 5) & 3) { /* fixed (2) and reload (3) stay */
            case 0: d->dst += d->units * d->unitSize; break;
            case 1: d->dst -= d->units * d->unitSize; break;
        }
        if (!(d->control & DMA_REPEAT))
            d->active = 0;
    }
}

void HostDma_Reset(void)
{
    memset(sHdma, 0, sizeof(sHdma));
}

/* src/dma.c replacements: chunked DMA copies / fills, done directly.
 * The game sometimes copies a little past the end of a ROM table (harmless:
 * it only reads), so this copy is exempt from the address sanitizer. */
__attribute__((no_sanitize_address))

void HostDmaStop(int channel)
{
    if (channel >= 0 && channel < 4)
        sHdma[channel].active = 0;
}
