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
extern unsigned char gHostVram[0x18000];
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
/* Returns pressed GBA keys (KEY_A ... KEY_L bits), polled once per frame. */
uint16_t Platform_PollKeys(void);
/* Receives interleaved stereo s16 samples at PORT_AUDIO_RATE. */
#define PORT_AUDIO_RATE 48000
void Platform_SubmitAudio(const int16_t *samples, int frames);
/* Persistent storage of the 128 KiB flash save. Return 0 if nothing saved. */
int Platform_LoadSave(void *data, size_t size);
void Platform_WriteSave(const void *data, size_t size);
void Platform_Log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PORT_H */
