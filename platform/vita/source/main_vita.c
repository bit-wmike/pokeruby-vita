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

static vita2d_texture *sTex;
static vita2d_texture *sSharp;
static vita2d_pgf *sFont;

#define SHARP_K 3

static void VideoInit(void)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_set_vblank_wait(0); /* paced by WaitDue() */
    sTex = vita2d_create_empty_texture_format(TEX_W, TEX_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sSharp = vita2d_create_empty_texture_rendertarget(TEX_W * SHARP_K, TEX_H * SHARP_K, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
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

int Platform_PaceAllowsRender(void)
{
    int late;
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
    while (FrameLate() < 0)
        sceDisplayWaitVblankStart();
}

static void DrawGame(void)
{
    int width = sLastW, height = sLastH;
    float scale = 544.0f / sLastZoom;
    float dx, dy;

    if (gPortConfig.displayStyle == PORT_STYLE_PIXEL) {
        int k = 544 / sLastZoom;
        if (k * width > 960)
            k = 960 / width;
        scale = (float)(k < 1 ? 1 : k);
    } else if (scale * width > 960.0f) {
        scale = 960.0f / width;
    }
    dx = (float)(int)((960.0f - width * scale) / 2.0f);
    dy = (float)(int)((544.0f - height * scale) / 2.0f);

    if (gPortConfig.displayStyle == PORT_STYLE_SHARP) {
        /* nearest-neighbour to 3x, then a smooth scale to the screen */
        vita2d_start_drawing_advanced(sSharp, SCE_GXM_SCENE_FRAGMENT_SET_DEPENDENCY);
        vita2d_texture_set_filters(sTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        vita2d_draw_texture_part_scale(sTex, 0, 0, 0, 0, (float)width, (float)height, (float)SHARP_K, (float)SHARP_K);
        vita2d_end_drawing();
        vita2d_start_drawing_advanced(NULL, SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY);
        vita2d_clear_screen();
        vita2d_texture_set_filters(sSharp, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        vita2d_draw_texture_part_scale(sSharp, dx, dy, 0, 0, (float)(width * SHARP_K), (float)(height * SHARP_K),
                                       scale / SHARP_K, scale / SHARP_K);
    } else {
        SceGxmTextureFilter f = gPortConfig.displayStyle == PORT_STYLE_SMOOTH ? SCE_GXM_TEXTURE_FILTER_LINEAR
                                                                               : SCE_GXM_TEXTURE_FILTER_POINT;
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_texture_set_filters(sTex, f, f);
        vita2d_draw_texture_part_scale(sTex, dx, dy, 0, 0, (float)width, (float)height, scale, scale);
    }
}

void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight)
{
    uint8_t *dst;
    unsigned stride;
    int y;

    PerfFrame(sceKernelGetProcessTimeWide());
    vita2d_wait_rendering_done();
    dst = (uint8_t *)vita2d_texture_get_datap(sTex);
    stride = vita2d_texture_get_stride(sTex);
    for (y = 0; y < height; y++)
        memcpy(dst + y * stride, pixels + y * pitch, (size_t)width * 4);
    sLastW = width;
    sLastH = height;
    sLastZoom = zoomHeight;

    vita2d_pool_reset();
    DrawGame();
    vita2d_end_drawing();
    WaitDue();
    vita2d_swap_buffers();
    sLastPresentEnd = sceKernelGetProcessTimeWide();
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

/* MI_VIEW (Weitsicht) follows in 0.2.0 */
enum { MI_STYLE, MI_FPS, MI_BACK, MI_COUNT, MI_VIEW };

static void MenuLine(int item, char *buf, size_t size)
{
    static const char *views[] = {"GBA (240x160)", "Weitsicht (480x272, ganzer Bildschirm)"};
    static const char *styles[] = {"Weich", "Scharf", "Pixelgenau"};
    switch (item) {
        case MI_VIEW: snprintf(buf, size, "Ansicht: %s", views[gPortConfig.viewMode]); break;
        case MI_STYLE: snprintf(buf, size, "Bilddarstellung: %s", styles[gPortConfig.displayStyle]); break;
        case MI_FPS: snprintf(buf, size, "Bildrate: %s", gPortConfig.frameRate30 ? "30 (Spieltempo bleibt)" : "60"); break;
        default: snprintf(buf, size, "Zurueck zum Spiel"); break;
    }
}

static int Wrap(int v, int d, int n)
{
    return (v + d + n) % n;
}

/* Changes an item; returns 1 when the menu should close. */
static int MenuChange(int item, int d)
{
    switch (item) {
        case MI_VIEW: gPortConfig.viewMode = Wrap(gPortConfig.viewMode, d ? d : 1, 2); break;
        case MI_STYLE: gPortConfig.displayStyle = Wrap(gPortConfig.displayStyle, d ? d : 1, 3); break;
        case MI_FPS: gPortConfig.frameRate30 ^= 1; break;
        case MI_BACK:
            if (d == 0)
                return 1;
            break;
    }
    return 0;
}

static void RunMenu(void)
{
    static int sel;
    unsigned prev = ~0u;
    char line[160];

    Platform_Log("[ui] menu open\n");
    for (;;) {
        SceCtrlData pad;
        unsigned pressed;
        int i, close = 0;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~prev;
        prev = pad.buttons;
        if (pressed & SCE_CTRL_UP) sel = Wrap(sel, -1, MI_COUNT);
        if (pressed & SCE_CTRL_DOWN) sel = Wrap(sel, 1, MI_COUNT);
        if (pressed & SCE_CTRL_LEFT) close = MenuChange(sel, -1);
        if (pressed & SCE_CTRL_RIGHT) close = MenuChange(sel, 1);
        if (pressed & SCE_CTRL_CROSS) close = MenuChange(sel, 0) || close;
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            close = 1;

        vita2d_pool_reset();
        DrawGame();
        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 190));
        vita2d_pgf_draw_text(sFont, 60, 50, RGBA8(90, 160, 255, 255), 1.3f, GAME_NAME " Vita " PKR_PORT_VERSION);
        for (i = 0; i < MI_COUNT; i++) {
            MenuLine(i, line, sizeof(line));
            vita2d_pgf_draw_text(sFont, 80, 100 + i * 30, i == sel ? RGBA8(255, 230, 80, 255) : RGBA8(220, 220, 220, 255),
                                 1.0f, line);
        }
        vita2d_pgf_draw_text(sFont, 60, 100 + sel * 30, RGBA8(255, 230, 80, 255), 1.0f, ">");
        vita2d_pgf_draw_text(sFont, 60, 525, RGBA8(150, 150, 150, 255), 0.85f,
                             "Steuerkreuz: waehlen/aendern   X: ausfuehren   O/START: zurueck");
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
        if (close)
            break;
    }
    Port_ConfigSave();
    Platform_Log("[ui] menu closed: view=%d style=%d fps30=%d\n", gPortConfig.viewMode, gPortConfig.displayStyle,
                 gPortConfig.frameRate30);
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

    if (pad.buttons & SCE_CTRL_CROSS) keys |= A_BUTTON;
    if (pad.buttons & SCE_CTRL_CIRCLE) keys |= B_BUTTON;
    if (pad.buttons & SCE_CTRL_SQUARE) keys |= B_BUTTON;    /* run */
    if (pad.buttons & SCE_CTRL_TRIANGLE) keys |= SELECT_BUTTON; /* registered item */
    if (pad.buttons & SCE_CTRL_LTRIGGER) keys |= L_BUTTON;
    if (pad.buttons & SCE_CTRL_RTRIGGER) keys |= R_BUTTON;
    if (pad.buttons & SCE_CTRL_START) keys |= START_BUTTON;
    if (pad.buttons & SCE_CTRL_SELECT) keys |= SELECT_BUTTON;
    if ((pad.buttons & SCE_CTRL_UP) || pad.ly < 128 - 64) keys |= DPAD_UP;
    if ((pad.buttons & SCE_CTRL_DOWN) || pad.ly > 128 + 64) keys |= DPAD_DOWN;
    if ((pad.buttons & SCE_CTRL_LEFT) || pad.lx < 128 - 64) keys |= DPAD_LEFT;
    if ((pad.buttons & SCE_CTRL_RIGHT) || pad.lx > 128 + 64) keys |= DPAD_RIGHT;
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
    Port_RunGame();

    sceKernelExitProcess(0);
    return 0;
}
