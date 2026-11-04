/*
 * PS Vita platform of the Pokémon Ruby/Sapphire port.
 *
 *  - Video: vita2d, three display styles (smooth, sharp, pixel-exact).
 *  - Audio: one BGM port at 48 kHz fed from a ring buffer by a thread.
 *  - Input: Vita buttons -> GBA keys; START+SELECT opens the port menu.
 *  - Files in ux0:data/<game>/: the flash save (128 KiB, the same format as
 *    emulator .sav files), settings.ini, log.txt (previous run: log_prev.txt).
 *
 * The game data is part of the build (made from the decomp's assets).
 */
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/lw_mutex.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "vita_hq.h"
#include <string.h>
#include <vita2d.h>

#include "port/port.h"
#include "port/port_config.h"
#include "cpu/mode1.h"
#include "gba/io_reg.h"

#ifdef RUBY
#define GAME_NAME "Pokemon Rubin"
#define DATA_DIR "ux0:data/pokemon_rubin"
#define SAVE_NAME "rubin.sav"
#else
#define GAME_NAME "Pokemon Saphir"
#define DATA_DIR "ux0:data/pokemon_saphir"
#define SAVE_NAME "saphir.sav"
#endif
#define SAVE_PATH DATA_DIR "/" SAVE_NAME
#define LOG_PATH DATA_DIR "/log.txt"

#define TEX_W 480
#define TEX_H 272

unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
int _newlib_heap_size_user = 128 * 1024 * 1024;

/* ---------------------------------------------------------------- log */

/* Every line gets time since start and frame number (port_log.c) and goes
 * into a RAM buffer; a low priority thread writes it to log.txt every half
 * second, so the game thread never waits for the memory card (before 0.3.18
 * each line was written and flushed at once). Lost on a crash: at most the
 * last half second. Above LOG_LIMIT bytes only warnings/hangs are kept. */
#define LOG_BUF (256 * 1024)
#define LOG_LIMIT (24 * 1024 * 1024)
static FILE *sLog;
static char sLogBuf[LOG_BUF];
static volatile unsigned sLogLen, sLogDropped, sLogWritten;
static SceKernelLwMutexWork sLogLock;
static int sLogLockOk;

uint64_t Platform_TimeUs(void)
{
    return sceKernelGetProcessTimeWide();
}

void Platform_Log(const char *fmt, ...)
{
    char line[512];
    int n;
    va_list ap;
    if (!sLog)
        return;
    n = Port_LogPrefix(line, sizeof(line));
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;
    if (sLogWritten > LOG_LIMIT && !strstr(line, "[hang]") && !strstr(line, "[log]"))
        return;
    if (sLogLockOk)
        sceKernelLockLwMutex(&sLogLock, 1, NULL);
    if (sLogLen + n <= LOG_BUF) {
        memcpy(sLogBuf + sLogLen, line, n);
        sLogLen += n;
    } else {
        sLogDropped++;
    }
    if (sLogLockOk)
        sceKernelUnlockLwMutex(&sLogLock, 1);
}

/* writes the buffer out (writer thread, hang watchdog, exit) */
void Platform_LogFlush(void)
{
    static char out[LOG_BUF];
    static SceKernelLwMutexWork flushLock;
    static int init;
    unsigned n, dropped;
    if (!sLog)
        return;
    if (!init) {
        init = sceKernelCreateLwMutex(&flushLock, "pkmn_logflush", 0, 0, NULL) >= 0 ? 1 : -1;
    }
    if (init > 0)
        sceKernelLockLwMutex(&flushLock, 1, NULL);
    if (sLogLockOk)
        sceKernelLockLwMutex(&sLogLock, 1, NULL);
    n = sLogLen;
    memcpy(out, sLogBuf, n);
    sLogLen = 0;
    dropped = sLogDropped;
    sLogDropped = 0;
    if (sLogLockOk)
        sceKernelUnlockLwMutex(&sLogLock, 1);
    if (n) {
        fwrite(out, 1, n, sLog);
        sLogWritten += n;
    }
    if (dropped)
        fprintf(sLog, "[log] %u lines lost (buffer full)\n", dropped);
    if (n || dropped)
        fflush(sLog);
    if (init > 0)
        sceKernelUnlockLwMutex(&flushLock, 1);
}

static int LogThread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        Platform_LogFlush();
    }
    return 0;
}

static void LogInit(void)
{
    SceUID th;
    SceDateTime now;
    sLogLockOk = sceKernelCreateLwMutex(&sLogLock, "pkmn_log", 0, 0, NULL) >= 0;
    sLog = fopen(LOG_PATH, "w");
    if (!sLog)
        return;
    Platform_Log(GAME_NAME " Vita %s (built " __DATE__ " " __TIME__ ")\n", PKR_PORT_VERSION);
    if (sceRtcGetCurrentClockLocalTime(&now) >= 0)
        Platform_Log("[sys] date %04d-%02d-%02d %02d:%02d:%02d\n", now.year, now.month, now.day, now.hour,
                     now.minute, now.second);
    th = sceKernelCreateThread("pkmn_log", LogThread, 0x10000100 + 40, 0x4000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* every 60 s and at start: battery, clocks, memory */
static void LogSystem(void)
{
    SceKernelFreeMemorySizeInfo mem;
    struct mallinfo mi = mallinfo();
    memset(&mem, 0, sizeof(mem));
    mem.size = sizeof(mem);
    sceKernelGetFreeMemorySize(&mem);
    Platform_Log("[sys] battery %d%%%s %d mV %d C, clock arm %d bus %d gpu %d xbar %d MHz, "
                 "heap used %u KB, free user %d KB cdram %d KB phycont %d KB, log %u KB\n",
                 scePowerGetBatteryLifePercent(), scePowerIsBatteryCharging() ? " (laedt)" : "",
                 scePowerGetBatteryVolt(), scePowerGetBatteryTemp() / 100,
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                 scePowerGetGpuXbarClockFrequency(), (unsigned)(mi.uordblks / 1024), mem.size_user / 1024,
                 mem.size_cdram / 1024, mem.size_phycont / 1024, sLogWritten / 1024);
}

/* -------------------------------------------------------------- video */

volatile int gVitaPhase;   /* hang watchdog: 1 game, 2 GPU wait, 3 upload/draw, 5 end drawing, 6 pacing, 4 swap */
static vita2d_texture *sTex;
/* battle layers (Platform_PresentBattle) */
static vita2d_texture *sStageTex, *sFieldTex, *sUiTex, *sHudTex;
static PortBattleFrame sBattle;
static int sBattleOn;
static unsigned sStageSerial = ~0u;
static vita2d_texture *sShadowTex;  /* soft ellipse under the battle figures */
static vita2d_texture *sBackTex;   /* behind GBA sized screens */
static int sBackOn;

void Platform_SetBackdrop(const uint32_t *pixels)
{
    if (!pixels) {
        sBackOn = 0;
        return;
    }
    vita2d_wait_rendering_done();
    {
        uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(sBackTex);
        unsigned stride = vita2d_texture_get_stride(sBackTex);
        int y;
        for (y = 0; y < 272; y++)
            memcpy(dst + y * stride, pixels + y * 480, 480 * 4);
    }
    sBackOn = 1;
}
static vita2d_pgf *sFont;


static void VideoInit(void)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_set_vblank_wait(0); /* paced by WaitDue() */
    sTex = vita2d_create_empty_texture_format(TEX_W, TEX_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sStageTex = vita2d_create_empty_texture_format(480, 272, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sFieldTex = vita2d_create_empty_texture_format(320, 160, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sUiTex = vita2d_create_empty_texture_format(240, 160, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sHudTex = vita2d_create_empty_texture_format(240, 160, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sBackTex = vita2d_create_empty_texture_format(480, 272, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    vita2d_texture_set_filters(sBackTex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_texture_set_filters(sStageTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    vita2d_texture_set_filters(sFieldTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    vita2d_texture_set_filters(sUiTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    vita2d_texture_set_filters(sHudTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    sShadowTex = vita2d_create_empty_texture_format(64, 16, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (sShadowTex) {   /* as PortStage_ComposeCpu: 0x102010 at up to 110/256 */
        uint8_t *d = (uint8_t *)vita2d_texture_get_datap(sShadowTex);
        unsigned st = vita2d_texture_get_stride(sShadowTex);
        int x, y;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 64; x++) {
                float fx = (x + 0.5f - 32) / 32, fy = (y + 0.5f - 8) / 8, q = fx * fx + fy * fy;
                uint32_t a = q >= 1.0f ? 0 : (uint32_t)(110 * (1.0f - q));
                *(uint32_t *)(d + y * st + x * 4) = (a << 24) | 0x102010u;
            }
        vita2d_texture_set_filters(sShadowTex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    sFont = vita2d_load_default_pgf();
    VitaHq_Available();   /* compile the shaders now, not at the first battle (a pause on black) */
}

/* Frame timing for the log: work time = game logic + rendering of a frame
 * (from the end of the last present to the start of this one). Once a
 * second a [perf] line with averages/maxima, the render passes and the
 * present steps; a frame over 16.7 ms also gets its own [slow] line. */
static SceUInt64 sLastPresentEnd;
static int sLastW = 240, sLastH = 160, sLastZoom = 160;
enum { PT_WAIT, PT_UPLOAD, PT_DRAW, PT_PACE, PT_COUNT };   /* present steps */
static uint32_t sPt[PT_COUNT], sPtSum[PT_COUNT], sPtMax[PT_COUNT];
static volatile uint32_t sAudioDry, sAudioStretch, sAudioSqueeze;
static int sCatchup; /* frames computed but not drawn (pacing) */

static void PerfFrame(SceUInt64 start)
{
    static SceUInt64 workSum, workMax;
    static int frames, slow, slowLogged, battleFrames, seconds;
    static uint32_t passSnap[PORT_PASS_COUNT], passNSnap[PORT_PASS_COUNT];
    int i;
    if (sLastPresentEnd) {
        SceUInt64 work = start - sLastPresentEnd;
        workSum += work;
        if (work > workMax)
            workMax = work;
        battleFrames += sBattleOn;
        if (work > 16700) {
            slow++;
            if (slowLogged++ < 5)
                Platform_Log("[slow] work %.2f ms %s | field %.2f intro %.2f fill %.2f ui %.2f hud %.2f present %.2f wide %.2f"
                             " | last present: wait %.2f upload %.2f draw %.2f pace %.2f\n",
                             work / 1000.0, sBattleOn ? "battle" : "field",
                             gPortPassLast[PORT_PASS_FIELD] / 1000.0, gPortPassLast[PORT_PASS_INTRO] / 1000.0,
                             gPortPassLast[PORT_PASS_FILL] / 1000.0, gPortPassLast[PORT_PASS_UI] / 1000.0,
                             gPortPassLast[PORT_PASS_HUD] / 1000.0, gPortPassLast[PORT_PASS_PRESENT] / 1000.0,
                             gPortPassLast[PORT_PASS_WIDE] / 1000.0, sPt[PT_WAIT] / 1000.0,
                             sPt[PT_UPLOAD] / 1000.0, sPt[PT_DRAW] / 1000.0, sPt[PT_PACE] / 1000.0);
        }
        memset(gPortPassLast, 0, sizeof(gPortPassLast));
        if (++frames == 60) {
            static const char *const pn[PORT_PASS_COUNT] = {"field", "intro", "fill", "ui", "hud", "present", "wide"};
            char passes[256];
            int o = 0, npc, spr;
            passes[0] = 0;
            for (i = 0; i < PORT_PASS_COUNT; i++) {
                uint32_t n = gPortPassN[i] - passNSnap[i], us = gPortPassUs[i] - passSnap[i];
                if (n)
                    o += snprintf(passes + o, sizeof(passes) - o, " %s %.2f/%u", pn[i], us / (double)n / 1000.0, n);
                passSnap[i] = gPortPassUs[i];
                passNSnap[i] = gPortPassN[i];
            }
            Port_LogCounts(&npc, &spr);
            Platform_Log("[perf] work avg %.2f max %.2f ms, >16.7 ms %d/60, skipped %d, %s %dx%d |%s"
                         " | wait %.2f/%.2f upload %.2f/%.2f draw %.2f/%.2f pace %.2f/%.2f (avg/max ms)"
                         " | npc %d sprites %d | audio dry %u stretch %u squeeze %u\n",
                         workSum / 60.0 / 1000.0, workMax / 1000.0, slow, sCatchup,
                         battleFrames ? "battle" : "field", sLastW, sLastH, passes,
                         sPtSum[PT_WAIT] / 60000.0, sPtMax[PT_WAIT] / 1000.0, sPtSum[PT_UPLOAD] / 60000.0,
                         sPtMax[PT_UPLOAD] / 1000.0, sPtSum[PT_DRAW] / 60000.0, sPtMax[PT_DRAW] / 1000.0,
                         sPtSum[PT_PACE] / 60000.0, sPtMax[PT_PACE] / 1000.0, npc, spr,
                         sAudioDry, sAudioStretch, sAudioSqueeze);
            sAudioDry = sAudioStretch = sAudioSqueeze = 0;
            memset(sPtSum, 0, sizeof(sPtSum));
            memset(sPtMax, 0, sizeof(sPtMax));
            sCatchup = 0;
            workSum = workMax = 0;
            frames = slow = slowLogged = battleFrames = 0;
            if (++seconds % 60 == 0)
                LogSystem();
        }
    }
}

/* time of one present step (Platform_Present*) */
static SceUInt64 sPtT0;
static void PtMark(int k)
{
    SceUInt64 t = sceKernelGetProcessTimeWide();
    if (k >= 0) {
        sPt[k] = (uint32_t)(t - sPtT0);
        sPtSum[k] += sPt[k];
        if (sPt[k] > sPtMax[k])
            sPtMax[k] = sPt[k];
    }
    sPtT0 = t;
}

/* Frame pacing: logic frame n is due at display vblank sAnchorVcount + n.
 * When the game runs late, frames are computed without drawing (at most two
 * in a row) so the game keeps its speed; after a long stall (loading, menu)
 * the schedule is re-anchored instead of catching up. */
static unsigned sAnchorVcount, sAnchorFrame;
static int sAnchored, sSkipRun;

static int FrameLate(void)
{
    unsigned due = sAnchorVcount + (Port_FrameCounter() - sAnchorFrame);
    return (int)(sceDisplayGetVcount() - due);
}

static void Reanchor(void)
{
    sAnchorVcount = sceDisplayGetVcount();
    sAnchorFrame = Port_FrameCounter();
    sAnchored = 1;
}

static int sTurbo;   /* right stick up: the game runs gPortConfig.turbo times as fast */

int Platform_PaceAllowsRender(void)
{
    int late;
    if (sTurbo) {
        /* no waiting, only every n-th frame drawn */
        sAnchored = 0;
        return Port_FrameCounter() % (unsigned)(gPortConfig.turbo > 1 ? gPortConfig.turbo : 2) == 0;
    }
    if (!sAnchored)
        Reanchor();
    late = FrameLate();
    if (late > 8) {
        Reanchor();
        late = 0;
    }
    if (late >= (gPortConfig.frameRate30 ? 3 : 2) && sSkipRun < 2) {
        sSkipRun++;
        sCatchup++;
        return 0;
    }
    sSkipRun = 0;
    return 1;
}

static void WaitDue(void)
{
    if (sTurbo) {
        Reanchor();
        return;
    }
    {   /* at most a few vblanks: far ahead of the schedule means it is broken
         * (0.2.9 hang: stuck in this phase) - start a new one instead */
        int n = 0;
        while (FrameLate() < 0 && n++ < 4)
            sceDisplayWaitVblankStart();
        if (FrameLate() < 0)
            Reanchor();
    }
}

/* for the hang watchdog */
void Platform_DescribePacing(void)
{
    Platform_Log("[hang] vcount %u, anchor vcount %u frame %u, late %d, turbo %d\n", sceDisplayGetVcount(),
                 sAnchorVcount, sAnchorFrame, FrameLate(), sTurbo);
}

int PortIsTitleScreen(void);

/* the battle layers at screen scale k (480x272 -> 960x544: k = 2, the field
 * 1.5x -> 3x: sharp pixels without any filtering) */
static void DrawBattle(float dx, float dy, float k)
{
    const PortBattleFrame *f = &sBattle;
    int i;
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_texture_scale(sStageTex, dx, dy, k, k);
    for (i = 0; i < f->nshadows && sShadowTex; i++) {
        const PortShadow *s = &f->shadows[i];
        vita2d_draw_texture_scale(sShadowTex, dx + (s->cx - s->rx) * k, dy + (s->cy - s->ry) * k,
                                  s->rx * 2 * k / 64.0f, s->ry * 2 * k / 16.0f);
    }
    for (i = 0; i < f->nparts; i++) {
        const PortParticle *p = &f->parts[i];
        if (p->x0 == p->x1 || p->y0 == p->y1)
            vita2d_draw_rectangle(dx + p->x0 * k, dy + p->y0 * k, (abs(p->x1 - p->x0) + 1) * k,
                                  (abs(p->y1 - p->y0) + 1) * k, p->color);
        else
            vita2d_draw_line(dx + p->x0 * k, dy + p->y0 * k, dx + p->x1 * k, dy + p->y1 * k, p->color);
    }
    if (f->fade > 0)
        vita2d_draw_rectangle(dx, dy, 480 * k, 272 * k, (f->fadeColor & 0x00FFFFFFu) | ((uint32_t)f->fade << 24));
    if (k == 2.0f && VitaHq_Scale3xAvailable())   /* field at 3x: Scale3x edges */
        VitaHq_DrawScale3x(sFieldTex, dx, dy + f->fieldY * k, (float)f->fieldW, (float)f->fieldH);
    else
        vita2d_draw_texture_part_scale(sFieldTex, dx, dy + f->fieldY * k, 0, 0, (float)f->fieldW, (float)f->fieldH,
                                       1.5f * k, 1.5f * k);
    for (i = 0; i < f->nblits; i++) {
        const PortBlit *b = &f->blits[i];
        vita2d_draw_texture_part_scale(sUiTex, dx + b->dx * k, dy + b->dy * k, b->sx, b->sy, b->w, b->h, k, k);
    }
    for (i = 0; i < f->nhud; i++) {
        const PortBlit *b = &f->hudBlits[i];
        vita2d_draw_texture_part_scale(sHudTex, dx + b->dx * k, dy + b->dy * k, b->sx, b->sy, b->w, b->h, k, k);
    }
}

static void Upload(vita2d_texture *t, const uint32_t *src, int w, int h, int pitch)
{
    uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    int y;
    for (y = 0; y < h; y++)
        memcpy(dst + y * stride, src + y * pitch, (size_t)w * 4);
}

/* only the rows the blits draw from (UI and HUD layers: the rest of the
 * texture is never drawn; performance, 0.3.17) */
static void UploadBlitRows(vita2d_texture *t, const uint32_t *src, int pitch, const PortBlit *b, int n)
{
    uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    uint8_t rows[160];
    int i, y;
    memset(rows, 0, sizeof(rows));
    for (i = 0; i < n; i++)
        for (y = b[i].sy < 0 ? 0 : b[i].sy; y < b[i].sy + b[i].h && y < 160; y++)
            rows[y] = 1;
    for (y = 0; y < 160; y++)
        if (rows[y])
            memcpy(dst + y * stride, src + y * pitch, 240 * 4);
}

static void DrawGame(void)
{
    int width = sLastW, height = sLastH;
    int k = 544 / sLastZoom;
    float scale, dx, dy;

    /* whole-number scale: 2x wide view, 3x GBA sized screens */
    if (k * width > 960)
        k = 960 / width;
    scale = (float)(k < 1 ? 1 : k);
    dx = (float)(int)((960.0f - width * scale) / 2.0f);
    dy = (float)(int)((544.0f - height * scale) / 2.0f);
    if (sBattleOn) {
        DrawBattle(dx, dy, scale);   /* battle layers: pixel exact */
        goto overlay;
    }
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (sBackOn && width < 480) {
        /* GBA sized screen (bag, party, shop ...) over the blurred scene */
        vita2d_draw_texture_scale(sBackTex, 0, 0, 2.0f, 2.0f);
        vita2d_draw_rectangle(dx - 4, dy - 4, width * scale + 8, height * scale + 8, RGBA8(0, 0, 0, 120));
    }
    if (gPortConfig.displayStyle == PORT_STYLE_HQ && VitaHq_Available()) {
        VitaHq_Draw(sTex, dx, dy, 0, 0, (float)width, (float)height, scale);
    } else {
        vita2d_texture_set_filters(sTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        vita2d_draw_texture_part_scale(sTex, dx, dy, 0, 0, (float)width, (float)height, scale, scale);
    }
overlay:
    /* the port version, small, on the title screen */
    if (PortIsTitleScreen())
        vita2d_pgf_draw_text(sFont, 960 - 8 - vita2d_pgf_text_width(sFont, 0.8f, "v" PKR_PORT_VERSION), 536,
                             RGBA8(255, 255, 255, 170), 0.8f, "v" PKR_PORT_VERSION);
}

void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight)
{
    uint8_t *dst;
    unsigned stride;
    int y;

    PerfFrame(sceKernelGetProcessTimeWide());
    gVitaPhase = 2;
    PtMark(-1);
    vita2d_wait_rendering_done();
    PtMark(PT_WAIT);
    gVitaPhase = 3;
    dst = (uint8_t *)vita2d_texture_get_datap(sTex);
    stride = vita2d_texture_get_stride(sTex);
    for (y = 0; y < height; y++)
        memcpy(dst + y * stride, pixels + y * pitch, (size_t)width * 4);
    sBattleOn = 0;
    sLastW = width;
    sLastH = height;
    sLastZoom = zoomHeight;

    PtMark(PT_UPLOAD);
    vita2d_pool_reset();
    DrawGame();
    gVitaPhase = 5;
    vita2d_end_drawing();
    PtMark(PT_DRAW);
    gVitaPhase = 6;
    WaitDue();
    PtMark(PT_PACE);
    gVitaPhase = 4;
    vita2d_swap_buffers();
    gVitaPhase = 1;
    sLastPresentEnd = sceKernelGetProcessTimeWide();
}

void Platform_PresentBattle(const PortBattleFrame *f)
{
    PerfFrame(sceKernelGetProcessTimeWide());
    gVitaPhase = 2;
    PtMark(-1);
    vita2d_wait_rendering_done();
    PtMark(PT_WAIT);
    gVitaPhase = 3;
    if (f->stageSerial != sStageSerial || !sBattleOn) {
        Upload(sStageTex, f->stage, 480, 272, 480);
        sStageSerial = f->stageSerial;
    }
    Upload(sFieldTex, f->field, f->fieldW, f->fieldH, f->fieldPitch);
    UploadBlitRows(sUiTex, f->ui, f->uiPitch, f->blits, f->nblits);
    UploadBlitRows(sHudTex, f->hud, 240, f->hudBlits, f->nhud);
    sBattle = *f;
    sBattleOn = 1;
    sLastW = 480;
    sLastH = 272;
    sLastZoom = 272;

    PtMark(PT_UPLOAD);
    vita2d_pool_reset();
    DrawGame();
    gVitaPhase = 5;
    vita2d_end_drawing();
    PtMark(PT_DRAW);
    gVitaPhase = 6;
    WaitDue();
    PtMark(PT_PACE);
    gVitaPhase = 4;
    vita2d_swap_buffers();
    gVitaPhase = 1;
    sLastPresentEnd = sceKernelGetProcessTimeWide();
}

/* ----------------------------------------------------- hang watchdog */

/* A frozen game (no frame for 5 s, the menu aside) writes what it was doing
 * to the log and its state to haenger_zustand.bin, which converts for the
 * PC build (tools/state/vita2host.py). */
int main(int argc, char **argv);
static volatile int sMenuOpen;
extern volatile int gVitaPhase;

static int WatchdogThread(SceSize args, void *argp)
{
    unsigned last = ~0u;
    int still = 0, done = 0;
    (void)args;
    (void)argp;
    for (;;) {
        unsigned f;
        sceKernelDelayThread(500 * 1000);
        f = Port_FrameCounter();
        if (sMenuOpen || f != last) {
            last = f;
            still = 0;
            continue;
        }
        if (++still < 10 || done)
            continue;
        done = 1;
        {
            void Port_DescribeHang(void);
            Platform_Log("[hang] no frame for 5 s at frame %u, phase %d, main=%p\n", f, gVitaPhase, (void *)&main);
            Port_DescribeHang();
            {
                void Platform_DescribePacing(void);
                Platform_DescribePacing();
            }
            Platform_Log("[hang] state %s\n", Port_StateDumpTo("haenger_zustand.bin") ? "written" : "failed");
            Platform_LogFlush();
        }
    }
    return 0;
}

static void WatchdogInit(void)
{
    SceUID th = sceKernelCreateThread("pkmn_watchdog", WatchdogThread, 0x10000100, 0x4000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* -------------------------------------------------------------- audio */

#define AUDIO_GRAIN 256
#define RING_FRAMES 8192 /* stereo frames, power of two */

static int16_t sRing[RING_FRAMES * 2];
static volatile uint32_t sRingWrite, sRingRead;
static int sAudioPort = -1;
static int16_t sHoldL, sHoldR;

void Platform_SubmitAudio(const int16_t *samples, int frames)
{
    int i;
    uint32_t w = sRingWrite;
    for (i = 0; i < frames; i++) {
        if (w - sRingRead >= RING_FRAMES - 1)
            break; /* full: drop the rest */
        sRing[(w & (RING_FRAMES - 1)) * 2] = samples[i * 2];
        sRing[(w & (RING_FRAMES - 1)) * 2 + 1] = samples[i * 2 + 1];
        w++;
    }
    sRingWrite = w;
}

/* The game hands over 800 samples per frame; frames come a little
 * unevenly (a late frame, a catch-up of two). The output keeps a cushion of
 * about 50 ms in the ring and holds its level by stretching or squeezing a
 * grain by one sample now and then (inaudible) - before 0.3.10 the ring ran
 * nearly empty and every late frame was a hitch in the music (Mike: every
 * 4-5 s). After running dry it waits for the cushion again. */
#define AUDIO_TARGET 2400
static int AudioThread(SceSize args, void *argp)
{
    static int16_t buf[AUDIO_GRAIN * 2];
    int filling = 1;
    (void)args;
    (void)argp;
    for (;;) {
        uint32_t r = sRingRead, level = sRingWrite - r;
        int i, take = AUDIO_GRAIN;
        if (filling && level >= AUDIO_TARGET)
            filling = 0;
        if (!filling && level < AUDIO_GRAIN) {
            filling = 1;   /* ran dry: wait for the cushion */
            sAudioDry++;
        }
        if (filling) {
            for (i = 0; i < AUDIO_GRAIN; i++) {
                buf[i * 2] = sHoldL;
                buf[i * 2 + 1] = sHoldR;
            }
            sceAudioOutOutput(sAudioPort, buf);
            continue;
        }
        if (level < AUDIO_TARGET - 600)
            take = AUDIO_GRAIN - 1, sAudioStretch++;   /* stretch: one sample twice */
        else if (level > AUDIO_TARGET + 600 && level > AUDIO_GRAIN + 1)
            take = AUDIO_GRAIN + 1, sAudioSqueeze++;   /* squeeze: one sample left out */
        for (i = 0; i < AUDIO_GRAIN; i++) {
            int si = take == AUDIO_GRAIN ? i : take < AUDIO_GRAIN ? (i < AUDIO_GRAIN / 2 ? i : i - 1)
                                                                  : (i < AUDIO_GRAIN / 2 ? i : i + 1);
            uint32_t k = r + (uint32_t)si;
            buf[i * 2] = sRing[(k & (RING_FRAMES - 1)) * 2];
            buf[i * 2 + 1] = sRing[(k & (RING_FRAMES - 1)) * 2 + 1];
        }
        sRingRead = r + (uint32_t)take;
        sHoldL = buf[AUDIO_GRAIN * 2 - 2];
        sHoldR = buf[AUDIO_GRAIN * 2 - 1];
        sceAudioOutOutput(sAudioPort, buf);
    }
    return 0;
}

static void AudioInit(void)
{
    SceUID th;
    sAudioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, PORT_AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (sAudioPort < 0) {
        Platform_Log("audio: open port failed 0x%08x\n", sAudioPort);
        return;
    }
    {
        int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
        sceAudioOutSetVolume(sAudioPort, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    }
    th = sceKernelCreateThread("pkmn_audio", AudioThread, 0x10000100, 0x10000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* ---------------------------------------------------- menu (START+SELECT) */

void Port_StateRequest(int load);
int Port_StateResult(void);
int Port_StateExists(void);
void PortCheat_MaxMoney(void);

enum {
    MI_H_DISPLAY, MI_STYLE, MI_FPS,
    MI_H_GAME, MI_HOENN, MI_DOUBLES, MI_TURBO,
    MI_H_CHEATS, MI_EXP, MI_BAG, MI_MONEY, MI_ENC, MI_SHINY, MI_RANDOM, MI_FLEE, MI_RUN,
    MI_H_STATE, MI_SAVE, MI_LOAD,
    MI_BACK,
    MI_H_KEYS, MI_K0, MI_K7 = MI_K0 + PORT_BTN_COUNT - 1,   /* right column */
    MI_COUNT
};

static int IsHeader(int item)
{
    return item == MI_H_DISPLAY || item == MI_H_GAME || item == MI_H_CHEATS || item == MI_H_STATE || item == MI_H_KEYS;
}

static char sMenuNote[96];
static int sConfirm = -1;   /* save/load state: asked once more (X yes, O no) */

static void MenuLine(int item, char *buf, size_t size)
{
    static const char *enc[] = {"Aus", "Normal", "Hoch"};
    const char *onoff[] = {"Aus", "An"};
    if (item >= MI_K0 && item <= MI_K7) {
        static const char *btn[PORT_BTN_COUNT] = {"Kreuz", "Kreis", "Viereck", "Dreieck", "L", "R", "START", "SELECT"};
        static const char *fn[PORT_KEY_COUNT] = {"-", "A", "B", "START", "SELECT", "L", "R"};
        snprintf(buf, size, "%s: %s", btn[item - MI_K0], fn[gPortConfig.keymap[item - MI_K0]]);
        return;
    }
    switch (item) {
        case MI_H_DISPLAY: snprintf(buf, size, "ANZEIGE"); break;
        case MI_STYLE: snprintf(buf, size, "Bilddarstellung: %s",
                                gPortConfig.displayStyle == PORT_STYLE_HQ ? "HQ (geglaettet)" : "Pixelgenau"); break;
        case MI_FPS: snprintf(buf, size, "Bildrate: %s", gPortConfig.frameRate30 ? "30 (Spieltempo bleibt)" : "60"); break;
        case MI_H_GAME: snprintf(buf, size, "SPIEL"); break;
        case MI_HOENN: snprintf(buf, size, "Pokemon: %s", gPortConfig.gen3Wild ? "Nur Hoenn (3. Gen.)" : "Original"); break;
        case MI_DOUBLES: snprintf(buf, size, "Wilde Doppelkaempfe: %s", gPortConfig.wildDoubles == 50 ? "50 %" :
                                         gPortConfig.wildDoubles == 30 ? "30 %" : "Normal"); break;
        case MI_TURBO: snprintf(buf, size, "Turbo (rechter Stick hoch halten): %dx", gPortConfig.turbo); break;
        case MI_H_CHEATS: snprintf(buf, size, "CHEATS (bis zum Beenden, EP bleibt gespeichert)"); break;
        case MI_EXP: snprintf(buf, size, "EP-Bonus: %dx", gPortConfig.expMult); break;
        case MI_MONEY: snprintf(buf, size, "Geld auf Maximum setzen"); break;
        case MI_BAG: snprintf(buf, size, "Rucksack: Anzahl der Items aendern ..."); break;
        case MI_ENC: snprintf(buf, size, "Begegnungsrate: %s", enc[gPortCheats.encounters]); break;
        case MI_SHINY: snprintf(buf, size, "Hohe Shiny-Chance (1:32): %s", onoff[gPortCheats.shiny]); break;
        case MI_RANDOM: snprintf(buf, size, "Zufalls-Pokemon: %s", onoff[gPortCheats.randomSpecies]); break;
        case MI_FLEE: snprintf(buf, size, "Immer fliehen: %s", onoff[gPortCheats.alwaysFlee]); break;
        case MI_RUN: snprintf(buf, size, "Rennen in Gebaeuden: %s", onoff[gPortCheats.runIndoors]); break;
        case MI_H_STATE: snprintf(buf, size, "SCHNELLSPEICHERN"); break;
        case MI_H_KEYS: snprintf(buf, size, "TASTEN (nur im Spiel)"); break;
        case MI_SAVE: snprintf(buf, size, sConfirm == MI_SAVE ? "Wirklich speichern? X: Ja  O: Nein" : "Zustand speichern"); break;
        case MI_LOAD: snprintf(buf, size, sConfirm == MI_LOAD ? "Wirklich laden? X: Ja  O: Nein" :
                                          Port_StateExists() ? "Zustand laden" : "Zustand laden (keiner da)"); break;
        default: snprintf(buf, size, "Zurueck zum Spiel"); break;
    }
}

static int Wrap(int v, int d, int n)
{
    return (v + d + n) % n;
}

/* The Rucksack editor (Mike, 0.3.12): only the numbers of the items one
 * has, 1..99; no new items, nothing removed. The game is paused (menu). */
int PortBag_Get(int pocket, int k, unsigned short *item, unsigned short *qty);
void PortBag_Set(int pocket, int k, int qty);
void PortBag_Name(unsigned short item, char *out, int size);

static void RunBag(void)
{
    static const char *pockets[4] = {"Items", "Baelle", "TM/VM", "Beeren"};
    int pocket = 0, sel = 0, top = 0;
    unsigned prev = ~0u;
    for (;;) {
        SceCtrlData pad;
        unsigned pressed, it, q;
        int n = 0, i, y, d = 0;
        unsigned short item, qty;
        char name[48], line[96];
        while (PortBag_Get(pocket, n, &item, &qty))
            n++;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~prev;
        prev = pad.buttons;
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            break;
        if (pressed & SCE_CTRL_TRIANGLE) {
            pocket = (pocket + 1) % 4;
            sel = top = 0;
            continue;
        }
        if (n > 0) {
            if (pressed & SCE_CTRL_UP) sel = (sel + n - 1) % n;
            if (pressed & SCE_CTRL_DOWN) sel = (sel + 1) % n;
            if (pressed & SCE_CTRL_LEFT) d = -1;
            if (pressed & SCE_CTRL_RIGHT) d = 1;
            if (pressed & SCE_CTRL_LTRIGGER) d = -10;
            if (pressed & SCE_CTRL_RTRIGGER) d = 10;
            if (sel >= n) sel = n - 1;
            if (d && PortBag_Get(pocket, sel, &item, &qty))
                PortBag_Set(pocket, sel, qty + d);
        }
        if (sel < top) top = sel;
        if (sel >= top + 20) top = sel - 19;

        vita2d_pool_reset();
        DrawGame();
        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 210));
        vita2d_pgf_draw_text(sFont, 60, 44, RGBA8(90, 160, 255, 255), 1.2f, "Rucksack");
        for (i = 0; i < 4; i++)
            vita2d_pgf_draw_text(sFont, 260 + i * 130, 44, i == pocket ? RGBA8(255, 230, 80, 255) : RGBA8(150, 150, 150, 255),
                                 0.9f, pockets[i]);
        if (n == 0)
            vita2d_pgf_draw_text(sFont, 90, 96, RGBA8(220, 220, 220, 255), 0.85f, "(nichts in dieser Tasche)");
        for (i = top, y = 96; i < n && i < top + 20; i++, y += 20) {
            if (!PortBag_Get(pocket, i, &item, &qty))
                break;
            it = item; q = qty;
            PortBag_Name((unsigned short)it, name, sizeof(name));
            snprintf(line, sizeof(line), "%s", name);
            vita2d_pgf_draw_text(sFont, 90, y, i == sel ? RGBA8(255, 230, 80, 255) : RGBA8(220, 220, 220, 255), 0.85f, line);
            snprintf(line, sizeof(line), "x %u", q);
            vita2d_pgf_draw_text(sFont, 420, y, i == sel ? RGBA8(255, 230, 80, 255) : RGBA8(220, 220, 220, 255), 0.85f, line);
            if (i == sel)
                vita2d_pgf_draw_text(sFont, 70, y, RGBA8(255, 230, 80, 255), 0.85f, ">");
        }
        vita2d_pgf_draw_text(sFont, 60, 530, RGBA8(150, 150, 150, 255), 0.8f,
                             "Hoch/Runter: Item   Links/Rechts: -1/+1   L/R: -10/+10   Dreieck: Tasche   O: zurueck");
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }
    Platform_Log("[ui] bag edited\n");
}

/* Changes an item (d -1/+1: left/right, 0: X); returns 1 when the menu
 * should close. */
static int MenuChange(int item, int d)
{
    if (item >= MI_K0 && item <= MI_K7) {
        int *k = &gPortConfig.keymap[item - MI_K0];
        *k = Wrap(*k, d ? d : 1, PORT_KEY_COUNT);
        return 0;
    }
    switch (item) {
        case MI_STYLE:
            if (gPortConfig.displayStyle == PORT_STYLE_HQ) {
                gPortConfig.displayStyle = PORT_STYLE_PIXEL;
            } else if (VitaHq_Available()) {
                gPortConfig.displayStyle = PORT_STYLE_HQ;
            } else {
                snprintf(sMenuNote, sizeof(sMenuNote), "HQ braucht ur0:data/libshacccg.suprx (siehe log.txt)");
            }
            break;
        case MI_FPS: gPortConfig.frameRate30 ^= 1; break;
        case MI_HOENN: gPortConfig.gen3Wild ^= 1; break;
        case MI_DOUBLES: {
            static const int v[3] = {0, 30, 50};
            int k = gPortConfig.wildDoubles == 50 ? 2 : gPortConfig.wildDoubles == 30 ? 1 : 0;
            gPortConfig.wildDoubles = v[Wrap(k, d ? d : 1, 3)];
            break;
        }
        case MI_TURBO: gPortConfig.turbo = 2 + Wrap(gPortConfig.turbo - 2, d ? d : 1, 3); break;
        case MI_EXP: {
            static const int steps[] = {1, 2, 4, 8};
            int k = 0;
            while (k < 3 && steps[k] != gPortConfig.expMult)
                k++;
            gPortConfig.expMult = steps[Wrap(k, d ? d : 1, 4)];
            break;
        }
        case MI_BAG:
            if (d == 0) {
                int PortIsOverworld(void);
                if (!PortIsOverworld())
                    snprintf(sMenuNote, sizeof(sMenuNote), "Rucksack nur in der Oberwelt");
                else
                    RunBag();
            }
            break;
        case MI_MONEY:
            if (d == 0) {
                PortCheat_MaxMoney();
                snprintf(sMenuNote, sizeof(sMenuNote), "Geld: 999999");
            }
            break;
        case MI_ENC: gPortCheats.encounters = Wrap(gPortCheats.encounters, d ? d : 1, 3); break;
        case MI_SHINY: gPortCheats.shiny ^= 1; break;
        case MI_RANDOM: gPortCheats.randomSpecies ^= 1; break;
        case MI_FLEE: gPortCheats.alwaysFlee ^= 1; break;
        case MI_RUN: gPortCheats.runIndoors ^= 1; break;
        case MI_SAVE:
            if (d == 0) {
                if (sConfirm != MI_SAVE) {   /* ask first (Mike pressed X for O once) */
                    sConfirm = MI_SAVE;
                    break;
                }
                sConfirm = -1;
                Port_StateRequest(0);
                return 1;
            }
            break;
        case MI_LOAD:
            if (d == 0) {
                if (!Port_StateExists()) {
                    snprintf(sMenuNote, sizeof(sMenuNote), "Kein Zustand dieser Version gespeichert");
                    break;
                }
                if (sConfirm != MI_LOAD) {
                    sConfirm = MI_LOAD;
                    break;
                }
                sConfirm = -1;
                Port_StateRequest(1);
                return 1;
            }
            break;
        case MI_BACK:
            if (d == 0)
                return 1;
            break;
    }
    return 0;
}

static void RunMenu(void)
{
    static int sel = MI_STYLE;
    unsigned prev = ~0u;
    char line[160];

    Platform_Log("[ui] menu open\n");
    sMenuOpen = 1;
    sMenuNote[0] = 0;
    sConfirm = -1;
    for (;;) {
        SceCtrlData pad;
        unsigned pressed;
        int i, close = 0, y;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~prev;
        prev = pad.buttons;
        if (pressed & SCE_CTRL_UP)
            do sel = Wrap(sel, -1, MI_COUNT); while (IsHeader(sel));
        if (pressed & SCE_CTRL_DOWN)
            do sel = Wrap(sel, 1, MI_COUNT); while (IsHeader(sel));
        if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS))
            sMenuNote[0] = 0;
        if (pressed & SCE_CTRL_LEFT) close = MenuChange(sel, -1);
        if (pressed & SCE_CTRL_RIGHT) close = MenuChange(sel, 1);
        if (pressed & SCE_CTRL_CROSS) close = MenuChange(sel, 0) || close;
        if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
            sConfirm = -1;
        if ((pressed & SCE_CTRL_CIRCLE) && sConfirm >= 0)
            sConfirm = -1;   /* "Nein": stay in the menu */
        else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            close = 1;

        vita2d_pool_reset();
        DrawGame();
        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 200));
        vita2d_pgf_draw_text(sFont, 60, 44, RGBA8(90, 160, 255, 255), 1.2f, GAME_NAME " Vita " PKR_PORT_VERSION);
        for (i = 0, y = 76; i < MI_COUNT; i++) {
            int cx = i >= MI_H_KEYS ? 560 : 0;   /* the buttons: right column */
            if (i == MI_H_KEYS)
                y = 76;
            if (IsHeader(i))
                y += 8;
            MenuLine(i, line, sizeof(line));
            if (IsHeader(i))
                vita2d_pgf_draw_text(sFont, cx + 60, y, RGBA8(90, 160, 255, 255), 0.8f, line);
            else
                vita2d_pgf_draw_text(sFont, cx + 90, y, i == sel ? RGBA8(255, 230, 80, 255) : RGBA8(220, 220, 220, 255),
                                     0.85f, line);
            if (i == sel)
                vita2d_pgf_draw_text(sFont, cx + 70, y, RGBA8(255, 230, 80, 255), 0.85f, ">");
            y += 20;
        }
        if (sMenuNote[0])
            vita2d_pgf_draw_text(sFont, 520, 44, RGBA8(255, 200, 120, 255), 0.8f, sMenuNote);
        vita2d_pgf_draw_text(sFont, 60, 530, RGBA8(150, 150, 150, 255), 0.8f,
                             "Steuerkreuz: waehlen/aendern   X: ausfuehren   O/START: zurueck");
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
        if (close)
            break;
    }
    Port_ConfigSave();
    sMenuOpen = 0;
    Platform_Log("[ui] menu closed: hoenn=%d doubles=%d view=%d style=%d fps30=%d turbo=%d cheats=%d%d%d%d%d\n",
                 gPortConfig.gen3Wild, gPortConfig.wildDoubles, gPortConfig.viewMode, gPortConfig.displayStyle, gPortConfig.frameRate30,
                 gPortConfig.turbo, gPortCheats.encounters, gPortCheats.shiny, gPortCheats.randomSpecies,
                 gPortCheats.alwaysFlee, gPortCheats.runIndoors);
    sAnchored = 0; /* the menu stalled the game: re-anchor the pacing */
}

/* -------------------------------------------------------------- input */

static int sWaitRelease; /* after the menu: ignore buttons until released */

uint16_t Platform_PollKeys(void)
{
    SceCtrlData pad;
    uint16_t keys = 0;

    sceCtrlPeekBufferPositive(0, &pad, 1);
    if ((pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT)) {
        RunMenu();
        sWaitRelease = 1;
        return 0;
    }
    if (sWaitRelease) {
        if (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))
            return 0;
        sWaitRelease = 0;
    }

    {   /* the buttons as set in the port menu (TASTEN) */
        static const unsigned btn[PORT_BTN_COUNT] = {SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
                                                     SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_START, SCE_CTRL_SELECT};
        static const uint16_t gba[PORT_KEY_COUNT] = {0, A_BUTTON, B_BUTTON, START_BUTTON, SELECT_BUTTON, L_BUTTON, R_BUTTON};
        int b;
        for (b = 0; b < PORT_BTN_COUNT; b++)
            if (pad.buttons & btn[b])
                keys |= gba[gPortConfig.keymap[b] % PORT_KEY_COUNT];
    }
    if ((pad.buttons & SCE_CTRL_UP) || pad.ly < 128 - 64) keys |= DPAD_UP;
    if ((pad.buttons & SCE_CTRL_DOWN) || pad.ly > 128 + 64) keys |= DPAD_DOWN;
    if ((pad.buttons & SCE_CTRL_LEFT) || pad.lx < 128 - 64) keys |= DPAD_LEFT;
    if ((pad.buttons & SCE_CTRL_RIGHT) || pad.lx > 128 + 64) keys |= DPAD_RIGHT;
    sTurbo = pad.ry < 128 - 80;   /* right stick up held */
    return keys;
}

/* -------------------------------------------------------------- files */

int Platform_LoadSave(void *data, size_t size)
{
    SceUID fd = sceIoOpen(SAVE_PATH, SCE_O_RDONLY, 0);
    int n;
    if (fd < 0)
        return 0;
    memset(data, 0xFF, size);
    n = sceIoRead(fd, data, size);
    sceIoClose(fd);
    Platform_Log("save: loaded %d bytes\n", n);
    return n > 0;
}

void Platform_WriteSave(const void *data, size_t size)
{
    SceUID fd = sceIoOpen(SAVE_PATH ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        Platform_Log("save: cannot write (0x%08x)\n", fd);
        return;
    }
    sceIoWrite(fd, data, size);
    sceIoClose(fd);
    sceIoRemove(SAVE_PATH);
    sceIoRename(SAVE_PATH ".tmp", SAVE_PATH);
    Platform_Log("save: written\n");
}

long Platform_ReadFile(const char *name, void *data, unsigned long size)
{
    char path[256];
    SceUID fd;
    int n;
    snprintf(path, sizeof(path), DATA_DIR "/%s", name);
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    n = sceIoRead(fd, data, size);
    sceIoClose(fd);
    return n < 0 ? -1 : n;
}

int Platform_WriteFile(const char *name, const void *data, unsigned long size)
{
    char path[256], tmp[260];
    SceUID fd;
    int n;
    snprintf(path, sizeof(path), DATA_DIR "/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return -1;
    n = sceIoWrite(fd, data, size);
    sceIoClose(fd);
    if (n != (int)size) {
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(path);
    return sceIoRename(tmp, path) < 0 ? -1 : 0;
}

/* --------------------------------------------------------------- main */

/* libgcc reaches pthreads only through *weak* references, which never pull
 * an object out of a static archive: on the Vita they all resolved to NULL,
 * so libgcc took the program for single-threaded and the emulated TLS behind
 * the renderer's __thread scanline buffers would be shared by all render
 * threads. Taking the address of every gthreads entry point forces them in
 * (same fix as in the Minish Cap port). */
void *const gPkmnGthreadsLink[] = {
    (void *)&pthread_attr_destroy,   (void *)&pthread_attr_init,         (void *)&pthread_attr_setdetachstate,
    (void *)&pthread_cancel,         (void *)&pthread_cond_broadcast,    (void *)&pthread_cond_destroy,
    (void *)&pthread_cond_init,      (void *)&pthread_cond_signal,       (void *)&pthread_cond_timedwait,
    (void *)&pthread_cond_wait,      (void *)&pthread_create,            (void *)&pthread_detach,
    (void *)&pthread_equal,          (void *)&pthread_exit,              (void *)&pthread_getschedparam,
    (void *)&pthread_getspecific,    (void *)&pthread_join,              (void *)&pthread_key_create,
    (void *)&pthread_key_delete,     (void *)&pthread_mutex_destroy,     (void *)&pthread_mutex_init,
    (void *)&pthread_mutex_lock,     (void *)&pthread_mutex_timedlock,   (void *)&pthread_mutex_trylock,
    (void *)&pthread_mutex_unlock,   (void *)&pthread_mutexattr_destroy, (void *)&pthread_mutexattr_init,
    (void *)&pthread_mutexattr_settype, (void *)&pthread_once,           (void *)&pthread_rwlock_rdlock,
    (void *)&pthread_rwlock_tryrdlock, (void *)&pthread_rwlock_trywrlock, (void *)&pthread_rwlock_unlock,
    (void *)&pthread_rwlock_wrlock,  (void *)&pthread_self,              (void *)&pthread_setschedparam,
    (void *)&pthread_setspecific,    (void *)&sched_get_priority_max,    (void *)&sched_get_priority_min,
    (void *)&sched_yield,
};

static void CheckGthreads(void)
{
    unsigned i, missing = 0;
    for (i = 0; i < sizeof(gPkmnGthreadsLink) / sizeof(gPkmnGthreadsLink[0]); i++)
        if (gPkmnGthreadsLink[i] == NULL)
            missing++;
    Platform_Log("[thread] gthreads entry points missing=%u\n", missing);
}

int main(int argc, char **argv)
{
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;

    (void)argc;
    (void)argv;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceIoMkdir(DATA_DIR, 0777);
    sceIoRemove(DATA_DIR "/log_prev.txt");
    sceIoRename(LOG_PATH, DATA_DIR "/log_prev.txt");
    LogInit();
    CheckGthreads();
    LogSystem();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    VideoInit();
    AudioInit();
    WatchdogInit();
    Port_RunGame();

    sceKernelExitProcess(0);
    return 0;
}
