/*
 * Headless test runner for the port (Linux, 32-bit).
 *
 * Runs the game for a number of frames with scripted input and writes
 * screenshots and the audio output, for automated checks without a Vita.
 *
 * Environment:
 *   PKR_FRAMES=n        frames to run (default 600)
 *   PKR_OUT=dir         output directory (default ./host_out)
 *   PKR_SHOT=a,b,c      frames to save as dir/frame_<n>.ppm
 *   PKR_SHOT_EVERY=k    additionally save every k-th frame
 *   PKR_INPUT=script    "frame:keys:len,..." keys as GBA bits (hex), held len frames
 *   PKR_AUTO_A=frame    press A every 30 frames from that frame on
 *   PKR_SAVE=file       flash save (default dir/pokemon.sav)
 */
#define _GNU_SOURCE
#include <execinfo.h>
#include <ucontext.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "port/port.h"

static const char *sOutDir = "host_out";
static int sMaxFrames = 600;
static int sShotEvery;
static char sShotList[4096];
static FILE *sWav;
static long sWavFrames;
static char sSavePath[1024];

struct InputEvent { int frame, len; unsigned keys; };
static struct InputEvent sInput[512];
static int sInputCount;

static int ShotWanted(int frame)
{
    char buf[32];
    if (sShotEvery && frame % sShotEvery == 0)
        return 1;
    snprintf(buf, sizeof(buf), ",%d,", frame);
    return strstr(sShotList, buf) != NULL;
}

void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight)
{
    int frame = (int)Port_FrameCounter();
    (void)zoomHeight;
    if (ShotWanted(frame)) {
        char path[1100];
        FILE *f;
        int x, y;
        snprintf(path, sizeof(path), "%s/frame_%05d.ppm", sOutDir, frame);
        f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width, height);
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++) {
                    uint32_t p = pixels[y * pitch + x];
                    unsigned char rgb[3] = {(unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16)};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
    }
}

static void FinishWav(void)
{
    if (sWav) {
        long bytes = sWavFrames * 4;
        unsigned v;
        fseek(sWav, 4, SEEK_SET);
        v = (unsigned)(36 + bytes);
        fwrite(&v, 4, 1, sWav);
        fseek(sWav, 40, SEEK_SET);
        v = (unsigned)bytes;
        fwrite(&v, 4, 1, sWav);
        fclose(sWav);
        sWav = NULL;
    }
}

extern int gFlatTrace __attribute__((weak)); /* debug build of m4a_gen.c */

uint16_t Platform_PollKeys(void)
{
    int frame = (int)Port_FrameCounter() + 1;
    uint16_t keys = 0;
    int i;

    if (frame >= sMaxFrames) {
        FinishWav();
        printf("host: done after %d frames\n", frame);
        exit(0);
    }
    if (&gFlatTrace && getenv("PKR_FLAT_TRACE_AT"))
        gFlatTrace = frame == atoi(getenv("PKR_FLAT_TRACE_AT"));
    for (i = 0; i < sInputCount; i++)
        if (frame >= sInput[i].frame && frame < sInput[i].frame + sInput[i].len)
            keys |= (uint16_t)sInput[i].keys;
    {   /* PKR_AUTO_A=frame: from that frame on A every 30 frames */
        static int autoA = -2;
        if (autoA == -2)
            autoA = getenv("PKR_AUTO_A") ? atoi(getenv("PKR_AUTO_A")) : -1;
        if (autoA >= 0 && frame >= autoA && (frame - autoA) % 30 < 3)
            keys |= 1;
    }
    return keys;
}

void Platform_SubmitAudio(const int16_t *samples, int frames)
{
    if (sWav) {
        fwrite(samples, 4, (size_t)frames, sWav);
        sWavFrames += frames;
    }
}

int Platform_LoadSave(void *data, size_t size)
{
    FILE *f = fopen(sSavePath, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(data, 1, size, f);
    fclose(f);
    return n == size;
}

void Platform_WriteSave(const void *data, size_t size)
{
    FILE *f = fopen(sSavePath, "wb");
    if (!f)
        return;
    fwrite(data, 1, size, f);
    fclose(f);
}

void Platform_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

uint64_t Platform_TimeUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void OnCrash(int sig)
{
    void *bt[64];
    int n = backtrace(bt, 64);
    fprintf(stderr, "host: signal %d at frame %u\n", sig, Port_FrameCounter());
    backtrace_symbols_fd(bt, n, 2);
    _exit(1);
}

/* Integer division by zero: x86 traps, the ARM targets (GBA BIOS/libgcc,
 * Vita libgcc) return 0. Emulate that for the test build: skip the div/idiv
 * instruction with quotient 0 and remainder = dividend. */
#ifdef __i386__
static int InsnLength(const unsigned char *p)
{
    int len = 0, mod, rm;
    while (p[len] == 0x66 || p[len] == 0x2E || p[len] == 0x3E || p[len] == 0x26 || p[len] == 0x36 ||
           p[len] == 0x64 || p[len] == 0x65)
        len++;
    if (p[len] != 0xF7 && p[len] != 0xF6)
        return 0;
    len++;
    mod = p[len] >> 6;
    rm = p[len] & 7;
    len++;
    if (mod != 3 && rm == 4) { /* SIB */
        if (mod == 0 && (p[len] & 7) == 5)
            len += 4;
        len++;
    }
    if (mod == 1)
        len += 1;
    else if (mod == 2 || (mod == 0 && rm == 5))
        len += 4;
    return len;
}

static void OnDivZero(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned char *ip = (unsigned char *)uc->uc_mcontext.gregs[REG_EIP];
    int len = InsnLength(ip);
    (void)sig;
    if (info->si_code != FPE_INTDIV || len == 0) {
        fprintf(stderr, "host: SIGFPE at frame %u\n", Port_FrameCounter());
        _exit(1);
    }
    uc->uc_mcontext.gregs[REG_EDX] = uc->uc_mcontext.gregs[REG_EAX];
    uc->uc_mcontext.gregs[REG_EAX] = 0;
    uc->uc_mcontext.gregs[REG_EIP] += len;
}
#endif

static void OpenWav(void)
{
    char path[1100];
    static const unsigned char hdr[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
        0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0,
    };
    snprintf(path, sizeof(path), "%s/audio.wav", sOutDir);
    sWav = fopen(path, "wb");
    if (sWav)
        fwrite(hdr, 1, sizeof(hdr), sWav);
}

int main(void)
{
    const char *s;

    signal(SIGSEGV, OnCrash);
    signal(SIGBUS, OnCrash);
#ifdef __i386__
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = OnDivZero;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGFPE, &sa, NULL);
    }
#endif
    signal(SIGABRT, OnCrash);
    if ((s = getenv("PKR_OUT")) != NULL)
        sOutDir = s;
    if ((s = getenv("PKR_FRAMES")) != NULL)
        sMaxFrames = atoi(s);
    if ((s = getenv("PKR_SHOT_EVERY")) != NULL)
        sShotEvery = atoi(s);
    if ((s = getenv("PKR_SHOT")) != NULL)
        snprintf(sShotList, sizeof(sShotList), ",%s,", s);
    if ((s = getenv("PKR_SAVE")) != NULL)
        snprintf(sSavePath, sizeof(sSavePath), "%s", s);
    else
        snprintf(sSavePath, sizeof(sSavePath), "%s/pokemon.sav", sOutDir);
    if ((s = getenv("PKR_INPUT")) != NULL) {
        const char *p = s;
        while (*p && sInputCount < 512) {
            struct InputEvent *e = &sInput[sInputCount];
            if (sscanf(p, "%d:%x:%d", &e->frame, &e->keys, &e->len) == 3)
                sInputCount++;
            p = strchr(p, ',');
            if (!p)
                break;
            p++;
        }
    }
    OpenWav();
    {
        extern int virtuappu_mode1_no_winopt;
        virtuappu_mode1_no_winopt = getenv("PKR_NO_WINOPT") != NULL;
    }
    if (getenv("PKR_PROF")) {
        extern unsigned Port_ProfileAverageUs(int);
        (void)Port_ProfileAverageUs;
    }
    if (getenv("PKR_HOENN_SELFTEST")) {
        void PortHoennSelfTest(void);
        PortHoennSelfTest();
    }
    Port_RunGame();
    return 0;
}

long Platform_ReadFile(const char *name, void *data, unsigned long size)
{
    char path[1100];
    FILE *f;
    long n;
    snprintf(path, sizeof(path), "%s/%s", sOutDir, name);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = (long)fread(data, 1, size, f);
    fclose(f);
    return n;
}

int Platform_WriteFile(const char *name, const void *data, unsigned long size)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", sOutDir, name);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}

int Platform_PaceAllowsRender(void)
{
    return 1;
}

#ifdef __arm__
/* Integer division by zero: the GBA and the Vita return 0; Linux' libgcc
 * would raise SIGFPE. */
int __aeabi_idiv0(int r)
{
    (void)r;
    return 0;
}

long long __aeabi_ldiv0(long long r)
{
    (void)r;
    return 0;
}
#endif
