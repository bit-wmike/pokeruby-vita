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

static FILE *sLog;

uint64_t Platform_TimeUs(void)
{
    return sceKernelGetProcessTimeWide();
}

void Platform_Log(const char *fmt, ...)
{
    va_list ap;
    if (!sLog)
        return;
    va_start(ap, fmt);
    vfprintf(sLog, fmt, ap);
    va_end(ap);
    fflush(sLog);
}

/* -------------------------------------------------------------- video */

volatile int gVitaPhase;   /* hang watchdog: 1 game, 2 GPU wait, 3 upload/draw, 4 swap */
static vita2d_texture *sTex;
/* battle layers (Platform_PresentBattle) */
static vita2d_texture *sStageTex, *sFieldTex, *sUiTex, *sHudTex;
static PortBattleFrame sBattle;
static int sBattleOn;
static unsigned sStageSerial = ~0u;
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
    sFont = vita2d_load_default_pgf();
}

static int sCatchup; /* frames computed but not drawn (pacing) */

/* Frame timing for the log: work time = game logic + rendering of a frame. */
static SceUInt64 sLastPresentEnd;
static SceUInt64 sWorkSum, sWorkMax;
static int sWorkFrames, sSlowFrames;
static int sLastW = 240, sLastH = 160, sLastZoom = 160;

static void PerfFrame(SceUInt64 start)
{
    if (sLastPresentEnd) {
        SceUInt64 work = start - sLastPresentEnd;
        sWorkSum += work;
        if (work > sWorkMax)
            sWorkMax = work;
        if (work > 16000)
            sSlowFrames++;
        if (++sWorkFrames == 300) {
            Platform_Log("[perf] frame work avg %.2f ms (render %.2f ms), max %.2f ms, >16 ms: %d of 300, "
                         "view %dx%d, catchup %d, render threads %d\n",
                         sWorkSum / 300.0 / 1000.0, Port_ProfileAverageUs(PORT_PROF_RENDER) / 1000.0,
                         sWorkMax / 1000.0, sSlowFrames, sLastW, sLastH, sCatchup,
                         1 + virtuappu_mode1_worker_count());
            sCatchup = 0;
            sWorkSum = sWorkMax = 0;
            sWorkFrames = sSlowFrames = 0;
        }
    }
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
    while (FrameLate() < 0)
        sceDisplayWaitVblankStart();
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
    vita2d_wait_rendering_done();
    gVitaPhase = 3;
    dst = (uint8_t *)vita2d_texture_get_datap(sTex);
    stride = vita2d_texture_get_stride(sTex);
    for (y = 0; y < height; y++)
        memcpy(dst + y * stride, pixels + y * pitch, (size_t)width * 4);
    sBattleOn = 0;
    sLastW = width;
    sLastH = height;
    sLastZoom = zoomHeight;

    vita2d_pool_reset();
    DrawGame();
    vita2d_end_drawing();
    WaitDue();
    gVitaPhase = 4;
    vita2d_swap_buffers();
    gVitaPhase = 1;
    sLastPresentEnd = sceKernelGetProcessTimeWide();
}

void Platform_PresentBattle(const PortBattleFrame *f)
{
    PerfFrame(sceKernelGetProcessTimeWide());
    gVitaPhase = 2;
    vita2d_wait_rendering_done();
    gVitaPhase = 3;
    if (f->stageSerial != sStageSerial || !sBattleOn) {
        Upload(sStageTex, f->stage, 480, 272, 480);
        sStageSerial = f->stageSerial;
    }
    Upload(sFieldTex, f->field, f->fieldW, f->fieldH, f->fieldPitch);
    Upload(sUiTex, f->ui, 240, 160, f->uiPitch);
    Upload(sHudTex, f->hud, 240, 160, 240);
    sBattle = *f;
    sBattleOn = 1;
    sLastW = 480;
    sLastH = 272;
    sLastZoom = 272;

    vita2d_pool_reset();
    DrawGame();
    vita2d_end_drawing();
    WaitDue();
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
            Platform_Log("[hang] state %s\n", Port_StateDumpTo("haenger_zustand.bin") ? "written" : "failed");
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

static int AudioThread(SceSize args, void *argp)
{
    static int16_t buf[AUDIO_GRAIN * 2];
    (void)args;
    (void)argp;
    for (;;) {
        uint32_t r = sRingRead;
        int i;
        for (i = 0; i < AUDIO_GRAIN; i++) {
            if (r == sRingWrite) {
                /* underrun: hold the last sample (no click) */
                buf[i * 2] = i ? buf[i * 2 - 2] : sHoldL;
                buf[i * 2 + 1] = i ? buf[i * 2 - 1] : sHoldR;
            } else {
                buf[i * 2] = sRing[(r & (RING_FRAMES - 1)) * 2];
                buf[i * 2 + 1] = sRing[(r & (RING_FRAMES - 1)) * 2 + 1];
                r++;
            }
        }
        sRingRead = r;
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
    MI_H_DISPLAY, MI_VIEW, MI_STYLE, MI_FPS,
    MI_H_GAME, MI_HOENN, MI_TURBO,
    MI_H_CHEATS, MI_EXP, MI_MONEY, MI_ENC, MI_SHINY, MI_RANDOM, MI_FLEE, MI_RUN,
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

static void MenuLine(int item, char *buf, size_t size)
{
    static const char *views[] = {"GBA (240x160)", "Weitsicht (480x272)"};
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
        case MI_VIEW: snprintf(buf, size, "Ansicht: %s", views[gPortConfig.viewMode]); break;
        case MI_STYLE: snprintf(buf, size, "Bilddarstellung: %s",
                                gPortConfig.displayStyle == PORT_STYLE_HQ ? "HQ (geglaettet)" : "Pixelgenau"); break;
        case MI_FPS: snprintf(buf, size, "Bildrate: %s", gPortConfig.frameRate30 ? "30 (Spieltempo bleibt)" : "60"); break;
        case MI_H_GAME: snprintf(buf, size, "SPIEL"); break;
        case MI_HOENN: snprintf(buf, size, "Pokemon: %s", gPortConfig.gen3Wild ? "Nur Hoenn (3. Gen.)" : "Original"); break;
        case MI_TURBO: snprintf(buf, size, "Turbo (rechter Stick hoch halten): %dx", gPortConfig.turbo); break;
        case MI_H_CHEATS: snprintf(buf, size, "CHEATS (bis zum Beenden, EP bleibt gespeichert)"); break;
        case MI_EXP: snprintf(buf, size, "EP-Bonus: %dx", gPortConfig.expMult); break;
        case MI_MONEY: snprintf(buf, size, "Geld auf Maximum setzen"); break;
        case MI_ENC: snprintf(buf, size, "Begegnungsrate: %s", enc[gPortCheats.encounters]); break;
        case MI_SHINY: snprintf(buf, size, "Hohe Shiny-Chance (1:32): %s", onoff[gPortCheats.shiny]); break;
        case MI_RANDOM: snprintf(buf, size, "Zufalls-Pokemon: %s", onoff[gPortCheats.randomSpecies]); break;
        case MI_FLEE: snprintf(buf, size, "Immer fliehen: %s", onoff[gPortCheats.alwaysFlee]); break;
        case MI_RUN: snprintf(buf, size, "Rennen in Gebaeuden: %s", onoff[gPortCheats.runIndoors]); break;
        case MI_H_STATE: snprintf(buf, size, "SCHNELLSPEICHERN"); break;
        case MI_H_KEYS: snprintf(buf, size, "TASTEN (nur im Spiel)"); break;
        case MI_SAVE: snprintf(buf, size, "Zustand speichern"); break;
        case MI_LOAD: snprintf(buf, size, "Zustand laden%s", Port_StateExists() ? "" : " (keiner da)"); break;
        default: snprintf(buf, size, "Zurueck zum Spiel"); break;
    }
}

static int Wrap(int v, int d, int n)
{
    return (v + d + n) % n;
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
        case MI_VIEW: gPortConfig.viewMode = Wrap(gPortConfig.viewMode, d ? d : 1, 2); break;
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
        case MI_TURBO: gPortConfig.turbo = 2 + Wrap(gPortConfig.turbo - 2, d ? d : 1, 3); break;
        case MI_EXP: {
            static const int steps[] = {1, 2, 4, 8};
            int k = 0;
            while (k < 3 && steps[k] != gPortConfig.expMult)
                k++;
            gPortConfig.expMult = steps[Wrap(k, d ? d : 1, 4)];
            break;
        }
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
    static int sel = MI_VIEW;
    unsigned prev = ~0u;
    char line[160];

    Platform_Log("[ui] menu open\n");
    sMenuOpen = 1;
    sMenuNote[0] = 0;
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
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
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
            y += 21;
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
    Platform_Log("[ui] menu closed: hoenn=%d view=%d style=%d fps30=%d turbo=%d cheats=%d%d%d%d%d\n",
                 gPortConfig.gen3Wild, gPortConfig.viewMode, gPortConfig.displayStyle, gPortConfig.frameRate30,
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
    sLog = fopen(LOG_PATH, "w");
    Platform_Log(GAME_NAME " Vita %s\n", PKR_PORT_VERSION);
    CheckGthreads();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    VideoInit();
    AudioInit();
    WatchdogInit();
    Port_RunGame();

    sceKernelExitProcess(0);
    return 0;
}
