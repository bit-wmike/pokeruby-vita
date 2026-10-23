#include <time.h>
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
#include "palette.h"
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

static u8 sLineIo[160][0x56];
int Port_EdgeFillScreen(void);   /* src/fldeff_flash.c */

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
    if (line < 160)   /* the registers of each line, for a second pass */
        memcpy(sLineIo[line], gHostIo, sizeof(sLineIo[0]));
}

/* a second pass of the same frame: the lines' registers again, without
 * running the HBlank DMA and interrupts twice */
static void ReplayLine(int line)
{
    if (line < 160)
        memcpy(gHostIo, sLineIo[line], sizeof(sLineIo[0]));
}


/* the intro scenery alone: no windows (they cut it), no blending, and when
 * it sinks its rest position (the port moves it itself) */
static void ReplayLineIntro(int line)
{
    extern int gPortIntroSink;
    ReplayLine(line);
    gHostIo[1] &= (u8)~0xE0;
    gHostIo[0x50] = gHostIo[0x51] = 0;
    if (gPortIntroSink)
        gHostIo[0x16] = gHostIo[0x17] = 0;
}

/* ---- the backdrop of GBA sized screens -------------------------------- */
static uint32_t sSnap[PORT_WIDE_H * PORT_WIDE_W], sBackdrop[PORT_WIDE_H * PORT_WIDE_W];
static int sSnapValid, sBackdropOn;
const uint32_t *PortStage_StageImage(void);
int PortIsTitleScreen(void);

/* A start menu screen (bag, party, Pokedex, ...) opens or closes: the
 * game fades the whole screen to black and back. In the wide view only
 * the middle (where the GBA sized screen comes) does; around it the scene
 * goes over into the blurred backdrop - no black flash over the whole
 * screen (Mike, 0.3.12). Set by src/start_menu.c, over when the scene is
 * back unfaded or the map changed (Fly, Teleport, ...). */
static int sMenuFade, sMenuFadeMap, sMenuFadeReady;
int gPortMapId(void)
{
    return gSaveBlock1.location.mapGroup * 256 + gSaveBlock1.location.mapNum;
}

void PortView_MenuFade(void)
{
    sMenuFade = 1;
    sMenuFadeReady = 0;
    sMenuFadeMap = gPortMapId();
}

static void MakeBackdrop(const uint32_t *src, int pitch);

static void MenuFadeFrame(void)
{
    const int W = PORT_WIDE_W, H = PORT_WIDE_H;
    int fading = gPaletteFade.active || gPaletteFade.y, x, y, t;
    if (!sMenuFade)
        return;
    if (gPortMapId() != sMenuFadeMap || !sSnapValid) {
        sMenuFade = 0;
        return;
    }
    if (!fading) {
        if (sMenuFadeReady)   /* back in the scene, unfaded */
            sMenuFade = 0;
        return;
    }
    if (!sMenuFadeReady) {
        MakeBackdrop(sSnap, W);
        sMenuFadeReady = 1;
    }
    t = gPaletteFade.y * 16;   /* 0..256: how far towards black */
    if (t > 256) t = 256;
    for (y = 0; y < H; y++) {
        uint32_t *row = sFrameBuffer + y * FRAME_MAX_W;
        const uint32_t *a = sSnap + y * W, *b = sBackdrop + y * W;
        int inY = y >= PORT_WIDE_OY && y < PORT_WIDE_OY + PORT_GBA_H;
        for (x = 0; x < W; x++) {
            uint32_t ca, cb;
            if (inY && x >= PORT_WIDE_OX && x < PORT_WIDE_OX + PORT_GBA_W)
                continue;
            ca = a[x]; cb = b[x];
            row[x] = 0xFF000000u |
                     ((((ca >> 16) & 255) * (256 - t) + ((cb >> 16) & 255) * t) >> 8) << 16 |
                     ((((ca >> 8) & 255) * (256 - t) + ((cb >> 8) & 255) * t) >> 8) << 8 |
                     (((ca & 255) * (256 - t) + (cb & 255) * t) >> 8);
        }
    }
}

/* box blur (radius r) across and down, then dimmed */
static void MakeBackdrop(const uint32_t *src, int pitch)
{
    static uint32_t tmp[PORT_WIDE_H * PORT_WIDE_W];
    const int r = 6, W = PORT_WIDE_W, H = PORT_WIDE_H, n = 2 * r + 1;
    int x, y, pass;
    for (y = 0; y < H; y++)
        memcpy(tmp + y * W, src + y * pitch, W * 4);
    for (pass = 0; pass < 2; pass++) {
        for (y = 0; y < H; y++) {           /* across: tmp -> sBackdrop */
            const uint32_t *s = tmp + y * W;
            uint32_t *d = sBackdrop + y * W;
            unsigned rs = 0, gs = 0, bs = 0;
            for (x = -r; x <= r; x++) {
                uint32_t c = s[x < 0 ? 0 : x];
                rs += c & 255; gs += (c >> 8) & 255; bs += (c >> 16) & 255;
            }
            for (x = 0; x < W; x++) {
                uint32_t a = s[x + r + 1 >= W ? W - 1 : x + r + 1], b = s[x - r < 0 ? 0 : x - r];
                d[x] = (bs / n) << 16 | (gs / n) << 8 | (rs / n);
                rs += (a & 255) - (b & 255); gs += ((a >> 8) & 255) - ((b >> 8) & 255);
                bs += ((a >> 16) & 255) - ((b >> 16) & 255);
            }
        }
        for (x = 0; x < W; x++) {           /* down: sBackdrop -> tmp */
            unsigned rs = 0, gs = 0, bs = 0;
            for (y = -r; y <= r; y++) {
                uint32_t c = sBackdrop[(y < 0 ? 0 : y) * W + x];
                rs += c & 255; gs += (c >> 8) & 255; bs += (c >> 16) & 255;
            }
            for (y = 0; y < H; y++) {
                uint32_t a = sBackdrop[(y + r + 1 >= H ? H - 1 : y + r + 1) * W + x];
                uint32_t b = sBackdrop[(y - r < 0 ? 0 : y - r) * W + x];
                tmp[y * W + x] = (bs / n) << 16 | (gs / n) << 8 | (rs / n);
                rs += (a & 255) - (b & 255); gs += ((a >> 8) & 255) - ((b >> 8) & 255);
                bs += ((a >> 16) & 255) - ((b >> 16) & 255);
            }
        }
    }
    for (x = 0; x < W * H; x++) {           /* dimmed to 55 % */
        uint32_t c = tmp[x];
        sBackdrop[x] = 0xFF000000u | ((((c >> 16) & 255) * 140 >> 8) << 16) | ((((c >> 8) & 255) * 140 >> 8) << 8) |
                       ((c & 255) * 140 >> 8);
    }
}

#define FIELD_W 480   /* pitch (the renderer wants at least 480) */

static void RenderFrame(void)
{
    static int bound;
    PPUMemory ppu;
    u16 dispcnt = IO16(0x000);
    int wide;
#ifndef __vita__
    if (getenv("PKR_DUMP_OAM") && sFrameCounter == (uint32_t)atoi(getenv("PKR_DUMP_OAM"))) {
        int k;
        for (k = 0; k < 128; k++) {
            u16 a0 = gHostOam[k * 8] | (gHostOam[k * 8 + 1] << 8), a1 = gHostOam[k * 8 + 2] | (gHostOam[k * 8 + 3] << 8);
            u16 a2 = gHostOam[k * 8 + 4] | (gHostOam[k * 8 + 5] << 8);
            if ((a0 & 0x300) == 0x200 || (a0 & 255) == 160) continue;
            fprintf(stderr, "oam%3d a0=%04x a1=%04x a2=%04x y=%d x=%d tile=%d pal=%d\n", k, a0, a1, a2, a0 & 255, a1 & 511, a2 & 1023, a2 >> 12);
        }
    }
#endif
    wide = PortView_BeginFrame();
#ifndef __vita__
    {   /* host: PKR_HIDE=mask clears DISPCNT layer bits for the picture */
        static int hide = -1;
        if (hide < 0) hide = getenv("PKR_HIDE") ? (int)strtol(getenv("PKR_HIDE"), NULL, 16) : 0;
        if (hide) { u16 d = IO16(0x000) & ~hide; IO16(0x000) = d; }
    }
#endif
    int stage = wide ? 0 : PortStage_BeginFrame();
    int width, height, zoom;
    static int lastKind, wasFading;   /* 0 GBA sized, 1 wide, 2 battle */
    if (gPortConfig.viewMode != PORT_VIEW_FULL || PortIsTitleScreen()) {
        if (sBackdropOn)
            Platform_SetBackdrop(NULL);
        sBackdropOn = sSnapValid = 0;
    } else if (!wide && !stage) {
        /* a GBA sized screen after the wide scene: the scene behind it */
        if (lastKind == 1 && sSnapValid) {
            MakeBackdrop(sSnap, PORT_WIDE_W);
            Platform_SetBackdrop(sBackdrop);
            sBackdropOn = 1;
        } else if (lastKind == 2) {
            MakeBackdrop(PortStage_StageImage(), PORT_WIDE_W);
            Platform_SetBackdrop(sBackdrop);
            sBackdropOn = 1;
        }
    } else if (wide && !stage) {
        /* the scene as it was before the screen faded out */
        int fading = gPaletteFade.active || gPaletteFade.y;
        if (!fading && (sFrameCounter % 16 == 0 || !sSnapValid)) {
            int y;
            for (y = 0; y < PORT_WIDE_H; y++)
                memcpy(sSnap + y * PORT_WIDE_W, sFrameBuffer + y * FRAME_MAX_W, PORT_WIDE_W * 4);
            sSnapValid = 1;
        } else if (fading && !wasFading) {
            int y;
            for (y = 0; y < PORT_WIDE_H; y++)
                memcpy(sSnap + y * PORT_WIDE_W, sFrameBuffer + y * FRAME_MAX_W, PORT_WIDE_W * 4);
            sSnapValid = 1;
        }
        wasFading = fading;
    }
    lastKind = stage ? 2 : wide ? 1 : 0;
    if (stage)
        wide = 1;
    width = wide ? PORT_WIDE_W : PORT_GBA_W;
    height = wide ? PORT_WIDE_H : PORT_GBA_H;
    /* full screen view: the wide scenes at 2x, the GBA-sized ones (intro,
     * menus) zoomed to 3x */
    zoom = gPortConfig.viewMode != PORT_VIEW_FULL ? PORT_GBA_H : wide ? PORT_WIDE_H : 181;
    /* in the game (bag, party, Pokedex, ... over the paused scene): pixel
     * exact like the field and its windows, not enlarged (Mike, 0.3.8);
     * title and intro stay 3x */
    if (zoom == 181 && sBackdropOn)
        zoom = PORT_WIDE_H;

    if (!bound) {
        VirtuaPPUMode1GbaMemory mem = {
            gHostIo, gHostVram, (uint16_t *)gHostPalram, (uint16_t *)(gHostPalram + 0x200), (uint16_t *)gHostOam,
        };
        virtuappu_mode1_bind_gba_memory(&mem);
        virtuappu_mode1_set_old3ds_profile(true);
        bound = 1;
    }

    if (stage) {
        /* battle: the field (GBA picture, 320 wide) and the windows in
         * two passes, put together 1.5x over the stage */
        static uint32_t field[FIELD_W * 160], ui[FIELD_W * 160];
        int fw, fh;
#ifndef __vita__
        static double acc[3]; static int nacc; struct timespec ts0, ts1;
#define STAGE_T(k) do { clock_gettime(CLOCK_MONOTONIC, &ts1); acc[k] += (ts1.tv_sec - ts0.tv_sec) * 1e6 + (ts1.tv_nsec - ts0.tv_nsec) / 1e3; ts0 = ts1; if (k == 2 && ++nacc == 200) { if (getenv("PKR_STAGE_TIME")) fprintf(stderr, "[stage] field %.0f ui %.0f compose %.0f us\n", acc[0] / 200, acc[1] / 200, acc[2] / 200); acc[0] = acc[1] = acc[2] = 0; nacc = 0; } } while (0)
        clock_gettime(CLOCK_MONOTONIC, &ts0);
#else
#define STAGE_T(k) do {} while (0)
#endif
        PortStage_FieldPass(&fw, &fh);
        memset(&ppu, 0, sizeof(ppu));
        ppu.frame_width = (uint16_t)fw;
        ppu.frame_height = (uint16_t)fh;
        ppu.frame_pitch = FIELD_W;
        ppu.mode = ((dispcnt & 7) == 0) ? 1 : 2;
        virtuappu_mode1_set_output_buffer(field, FIELD_W);
        virtuappu_mode1_pre_line_callback = PreLine;
        sRendering = 1;
        Port_ProfileBegin(PORT_PROF_RENDER);
        virtuappu_mode1_render_frame(&ppu);
        sRendering = 0;
        {   /* the intro scenery alone, while it slides in */
            static uint32_t intro[FIELD_W * 160];
            int PortStage_IntroPass(void);
            void PortStage_SetIntroPixels(const uint32_t *pix);
            if (PortStage_IntroPass()) {
                static u8 io[0x56];
                memcpy(io, gHostIo, sizeof(io));
                virtuappu_mode1_set_output_buffer(intro, FIELD_W);
                virtuappu_mode1_pre_line_callback = ReplayLineIntro;
                virtuappu_mode1_render_frame(&ppu);
                memcpy(gHostIo, io, sizeof(io));
                PortStage_SetIntroPixels(intro);
            } else {
                PortStage_SetIntroPixels(NULL);
            }
        }
        {   /* a move's background over the whole screen */
            static uint32_t fill[FIELD_W * 184];
            int PortStage_BgFillPass(int *h);
            void PortStage_SetBgFillPixels(const uint32_t *pix, int pitch);
            int fh2;
            if (PortStage_BgFillPass(&fh2)) {
                static u8 io[0x56];
                memcpy(io, gHostIo, sizeof(io));
                ppu.frame_height = (uint16_t)fh2;
                virtuappu_mode1_set_output_buffer(fill, FIELD_W);
                virtuappu_mode1_pre_line_callback = ReplayLine;
                virtuappu_mode1_render_frame(&ppu);
                memcpy(gHostIo, io, sizeof(io));
                ppu.frame_height = (uint16_t)fh;
                PortStage_SetBgFillPixels(fill, FIELD_W);
            }
        }
        STAGE_T(0);
        PortStage_UiPass();
        memset(&ppu, 0, sizeof(ppu));
        ppu.frame_width = PORT_GBA_W;
        ppu.frame_height = PORT_GBA_H;
        ppu.frame_pitch = FIELD_W;
        ppu.mode = ((dispcnt & 7) == 0) ? 1 : 2;
        virtuappu_mode1_set_output_buffer(ui, FIELD_W);
        virtuappu_mode1_pre_line_callback = NULL;
        virtuappu_mode1_render_frame(&ppu);
        {   /* the healthboxes alone when a window covers them */
            static uint32_t hud[FIELD_W * 160];
            int PortStage_HudPass(void);
            void PortStage_SetHudPixels(const uint32_t *pix);
            if (PortStage_HudPass()) {
                virtuappu_mode1_set_output_buffer(hud, FIELD_W);
                virtuappu_mode1_render_frame(&ppu);
                PortStage_SetHudPixels(hud);
            } else {
                PortStage_SetHudPixels(NULL);
            }
        }
        STAGE_T(1);
        Port_ProfileEnd(PORT_PROF_RENDER);
        PortStage_Present(field, FIELD_W, ui, FIELD_W);
        STAGE_T(2);
        return;
    }

    memset(&ppu, 0, sizeof(ppu));
    ppu.frame_width = (uint16_t)width;
    ppu.frame_height = (uint16_t)height;
    ppu.frame_pitch = FRAME_MAX_W;
    ppu.mode = ((dispcnt & 7) == 0) ? 1 : 2;

    virtuappu_mode1_set_output_buffer(sFrameBuffer, FRAME_MAX_W);
    virtuappu_mode1_pre_line_callback = PreLine;
    sRendering = 1;
    Port_ProfileBegin(PORT_PROF_RENDER);
    virtuappu_mode1_render_frame(&ppu);
    Port_ProfileEnd(PORT_PROF_RENDER);
    sRendering = 0;

    if (wide)
        MenuFadeFrame();
    if (!wide && gPortConfig.viewMode == PORT_VIEW_FULL && Port_EdgeFillScreen()) {
        /* a full screen picture of the GBA (the cave entry's rings): its
         * edges carried out to the whole wide screen, 1:1 like the field
         * (Mike, 0.3.3) */
        static uint32_t gba[PORT_GBA_W * PORT_GBA_H];
        int x, y, ox = (PORT_WIDE_W - PORT_GBA_W) / 2, oy = (PORT_WIDE_H - PORT_GBA_H) / 2;
        for (y = 0; y < PORT_GBA_H; y++)
            memcpy(gba + y * PORT_GBA_W, sFrameBuffer + y * FRAME_MAX_W, PORT_GBA_W * 4);
        for (y = 0; y < PORT_WIDE_H; y++) {
            int gy = y - oy;
            const uint32_t *r;
            if (gy < 0) gy = 0;
            if (gy >= PORT_GBA_H) gy = PORT_GBA_H - 1;
            r = gba + gy * PORT_GBA_W;
            for (x = 0; x < PORT_WIDE_W; x++) {
                int gx = x - ox;
                if (gx < 0) gx = 0;
                if (gx >= PORT_GBA_W) gx = PORT_GBA_W - 1;
                sFrameBuffer[y * FRAME_MAX_W + x] = r[gx];
            }
        }
        if (sBackdropOn)
            Platform_SetBackdrop(NULL);
        sBackdropOn = 0;
        Platform_Present(sFrameBuffer, PORT_WIDE_W, PORT_WIDE_H, FRAME_MAX_W, PORT_WIDE_H);
        return;
    }
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

void Port_StateFrame(void);

void VBlankIntrWait(void)
{
    HostRunFrame();
    Port_StateFrame();
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
#ifndef __vita__
        if (getenv("PKR_PROF") && what == PORT_PROF_RENDER)
            fprintf(stderr, "[prof] render avg %u us\n", (unsigned)sProfAvg[what]);
#endif
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
