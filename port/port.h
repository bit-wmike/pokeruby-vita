/*
 * Host / PS Vita port of Pokémon Ruby/Sapphire - shared port interface.
 *
 * The game code is compiled almost unchanged (a few #ifdef PORT spots); this
 * layer stands in for the GBA hardware: memory regions, BIOS calls, DMA,
 * interrupts, flash save chip, real-time clock, the PPU (software renderer in
 * port/ppu) and the sound hardware. A platform (platform/host,
 * platform/vita) supplies video, audio, input and files through the
 * Platform* functions below.
 */
#ifndef PORT_PORT_H
#define PORT_PORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char gHostEwram[0x40000];
extern unsigned char gHostIwram[0x8000];
extern unsigned char gHostIo[0x400];
extern unsigned char gHostPalram[0x400];
extern unsigned char gHostVram[0x1E000];
extern unsigned char gHostOam[0x400];
extern unsigned char gHostFlash[0x20000];

#define PORT_GBA_W 240
#define PORT_GBA_H 160

/* ---- core (port/host_core.c) ---- */
/* Runs the game forever (AgbMain, restarted on soft reset). */
void Port_RunGame(void);
uint32_t Port_FrameCounter(void);
/* Flash save changed (port/host_flash.c): written out by the core. */
void Port_FlashDirty(void);

/* DMA (port/host_dma.c) */
void HostDma_HBlank(void);
void HostDma_Reset(void);

/* ---- profiling ---- */
enum { PORT_PROF_RENDER, PORT_PROF_COUNT };
void Port_ProfileBegin(int what);
void Port_ProfileEnd(int what);
unsigned Port_ProfileAverageUs(int what);
uint64_t Platform_TimeUs(void);

/* ---- platform interface ---- */
/* Shows a finished frame (ABGR8888, `pitch` pixels per row), scaled so that
 * `zoomHeight` lines fill the screen height. */
void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight);
/* A battle frame (480x272) in layers, put together by the platform (the
 * Vita on its GPU): the stage, the moving weather, a fade over the stage,
 * the field (GBA picture, 1.5x, transparent where the stage shows), then
 * parts of the GBA sized UI picture (text windows) and of the healthbox
 * picture at their places. PortStage_ComposeCpu does the same in software. */
typedef struct PortBlit { int16_t sx, sy, w, h, dx, dy; } PortBlit;
typedef struct PortParticle { int16_t x0, y0, x1, y1; uint32_t color; } PortParticle; /* line, ABGR */
typedef struct PortBattleFrame {
    const uint32_t *stage;  unsigned stageSerial;          /* 480x272 */
    const PortParticle *parts; int nparts;
    uint32_t fadeColor; int fade;                           /* 0..255 over the stage */
    const uint32_t *field; int fieldPitch, fieldW, fieldH, fieldY;  /* alpha 0 = stage */
    const uint32_t *ui; int uiPitch;                        /* 240x160, alpha 0 = nothing */
    const PortBlit *blits; int nblits;
    const uint32_t *hud;                                    /* 240x160, like ui */
    const PortBlit *hudBlits; int nhud;
} PortBattleFrame;
void Platform_PresentBattle(const PortBattleFrame *f);
/* Behind GBA sized screens (bag, party, shop, ...): the last wide scene,
 * blurred and dimmed (480x272), or NULL for plain black. */
void Platform_SetBackdrop(const uint32_t *pixels);
void PortStage_ComposeCpu(const PortBattleFrame *f, uint32_t *fb, int pitch);
/* Returns pressed GBA keys (KEY_A ... KEY_L bits), polled once per frame. */
uint16_t Platform_PollKeys(void);
/* Receives interleaved stereo s16 samples at PORT_AUDIO_RATE. */
#define PORT_AUDIO_RATE 48000
void Platform_SubmitAudio(const int16_t *samples, int frames);
/* Persistent storage of the 128 KiB flash save. Return 0 if nothing saved. */
int Platform_LoadSave(void *data, size_t size);
void Platform_WriteSave(const void *data, size_t size);
void Platform_Log(const char *fmt, ...);
/* Frame pacing: 0 = skip drawing this frame (the game is running late). */
int Platform_PaceAllowsRender(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PORT_H */
