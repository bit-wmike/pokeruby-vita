/*
 * Frame loop of the host / PS Vita port: stands in for the GBA's display
 * timing and interrupts.
 *
 * The game runs its logic until it waits for VBlank (VBlankIntrWait). At
 * that point the port
 *   1. renders the 160 visible scanlines with the software PPU, running
 *      HBlank DMA and the HBlank / VCount interrupts between scanlines,
 *   2. shows the frame, raises the VBlank interrupt and reads the controls.
 * SoftReset restarts AgbMain.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/ppu/include/virtuappu.h"
#include "port/ppu/include/cpu/mode1.h"

#include "port/port_config.h"
#include "port/port_view.h"

#include "global.h"
#include "main.h"

typedef void (*IntrFunc)(void);
extern IntrFunc gIntrTable[];

void AgbMain(void);
void HostAudio_Frame(void);

#define FRAME_MAX_W 480
#define FRAME_MAX_H 272

static int sRendering;
static uint32_t sFrameCounter;
static uint32_t sFrameBuffer[FRAME_MAX_W * FRAME_MAX_H];
static jmp_buf sResetJump;
static int sFlashDirty, sFlashTimer;

#define IO16(off) (*(volatile u16 *)(gHostIo + (off)))

uint32_t Port_FrameCounter(void)
{
    return sFrameCounter;
}

void Port_FlashDirty(void)
{
    sFlashDirty = 1;
    sFlashTimer = 0;
}

/* ---- interrupts ------------------------------------------------------- */

void *HostVcountPtr(void)
{
    if (!sRendering) {
        /* Outside of rendering, every read lets the beam move on one line. */
        u16 vc = IO16(0x006);
        IO16(0x006) = (u16)((vc + 1) % 228);
    }
    return gHostIo + 0x006;
}

/* Index into gIntrTable for an IF bit, in the order of IntrMain (crt0.s). */
static int IrqIndex(u16 flag)
{
    switch (flag) {
        case INTR_FLAG_SERIAL: return 0;
        case INTR_FLAG_TIMER3: return 1;
        case INTR_FLAG_HBLANK: return 2;
        case INTR_FLAG_VBLANK: return 3;
        case INTR_FLAG_VCOUNT: return 4;
        default: return -1;
    }
}

static void RaiseIrq(u16 flag)
{
    int index;

    if (!(IO16(0x208) & 1) || !(IO16(0x200) & flag))
        return;
    index = IrqIndex(flag);
    if (index < 0 || gIntrTable[index] == NULL)
        return;
    gIntrTable[index]();
}

/* ---- display ---------------------------------------------------------- */

static void PreLine(int line)
{
    u16 dispstat = IO16(0x004);
    int prev = line == 0 ? 227 : line - 1;

    /* HBlank of the previous scanline */
    IO16(0x006) = (u16)prev;
    if (line != 0)
        HostDma_HBlank();
    if (dispstat & DISPSTAT_HBLANK_INTR)
        RaiseIrq(INTR_FLAG_HBLANK);

    /* Start of this scanline */
    IO16(0x006) = (u16)line;
    if ((dispstat & DISPSTAT_VCOUNT_INTR) && (dispstat >> 8) == line)
        RaiseIrq(INTR_FLAG_VCOUNT);
}

static void RenderFrame(void)
{
    static int bound;
    PPUMemory ppu;
    u16 dispcnt = IO16(0x000);
    int wide = PortView_BeginFrame();
    int width = wide ? PORT_WIDE_W : PORT_GBA_W, height = wide ? PORT_WIDE_H : PORT_GBA_H;
    int zoom = gPortConfig.viewMode == PORT_VIEW_FULL ? PORT_WIDE_H : PORT_GBA_H;

    if (!bound) {
        VirtuaPPUMode1GbaMemory mem = {
            gHostIo, gHostVram, (uint16_t *)gHostPalram, (uint16_t *)(gHostPalram + 0x200), (uint16_t *)gHostOam,
        };
        virtuappu_mode1_bind_gba_memory(&mem);
        virtuappu_mode1_set_old3ds_profile(true);
        virtuappu_mode1_set_output_buffer(sFrameBuffer, FRAME_MAX_W);
        bound = 1;
    }

    memset(&ppu, 0, sizeof(ppu));
    ppu.frame_width = (uint16_t)width;
    ppu.frame_height = (uint16_t)height;
    ppu.frame_pitch = FRAME_MAX_W;
    ppu.mode = ((dispcnt & 7) == 0) ? 1 : 2;

    virtuappu_mode1_pre_line_callback = PreLine;
    sRendering = 1;
    Port_ProfileBegin(PORT_PROF_RENDER);
    virtuappu_mode1_render_frame(&ppu);
    Port_ProfileEnd(PORT_PROF_RENDER);
    sRendering = 0;

    Platform_Present(sFrameBuffer, width, height, FRAME_MAX_W, zoom);
}

/* A frame that is not shown: the scanline timing (HBlank DMA and
 * interrupts) still happens, as the game depends on it. */
static void SkipFrame(void)
{
    int line;
    sRendering = 1;
    for (line = 0; line < 160; line++)
        PreLine(line);
    sRendering = 0;
}

static void CheckFlash(void)
{
    if (sFlashDirty && ++sFlashTimer >= 30) {
        sFlashDirty = 0;
        Platform_WriteSave(gHostFlash, sizeof(gHostFlash));
    }
}

static void HostRunFrame(void)
{
    u16 keys;

    if ((gPortConfig.frameRate30 && (sFrameCounter & 1)) || !Platform_PaceAllowsRender())
        SkipFrame();
    else
        RenderFrame();

    /* VBlank */
    IO16(0x006) = 160;
    IO16(0x004) |= DISPSTAT_VBLANK;
    keys = Platform_PollKeys();
    IO16(0x130) = (u16)(~keys & 0x3FF);
    RaiseIrq(INTR_FLAG_VBLANK);
    PortView_VBlank();
    IO16(0x004) &= ~DISPSTAT_VBLANK;

    HostAudio_Frame();
    CheckFlash();
    {
        void PortTest_Frame(void);
        PortTest_Frame();
    }
    sFrameCounter++;
}

/* ---- BIOS calls that touch the flow of the program -------------------- */

void VBlankIntrWait(void)
{
    HostRunFrame();
}

void SoftReset(u32 resetFlags)
{
    (void)resetFlags;
    longjmp(sResetJump, 1);
}

/* ---- profiling ---------------------------------------------------------- */

static uint64_t sProfStart[PORT_PROF_COUNT], sProfSum[PORT_PROF_COUNT], sProfAvg[PORT_PROF_COUNT];
static unsigned sProfCount[PORT_PROF_COUNT];

void Port_ProfileBegin(int what)
{
    sProfStart[what] = Platform_TimeUs();
}

void Port_ProfileEnd(int what)
{
    sProfSum[what] += Platform_TimeUs() - sProfStart[what];
    if (++sProfCount[what] == 300) {
        sProfAvg[what] = sProfSum[what] / 300;
        sProfSum[what] = 0;
        sProfCount[what] = 0;
    }
}

unsigned Port_ProfileAverageUs(int what)
{
    return (unsigned)sProfAvg[what];
}

/* ---- entry ------------------------------------------------------------ */

void Port_RunGame(void)
{
    Port_ConfigLoad();
    if (!Platform_LoadSave(gHostFlash, sizeof(gHostFlash)))
        memset(gHostFlash, 0xFF, sizeof(gHostFlash));
    setjmp(sResetJump);
    HostDma_Reset();
    memset(gHostIo, 0, sizeof(gHostIo));
    IO16(0x130) = 0x3FF;
    AgbMain();
}

void PortFatalHalt(const char *why)
{
    Platform_Log("port: fatal: %s (frame %u)\n", why, sFrameCounter);
    for (;;)
        VBlankIntrWait();
}
