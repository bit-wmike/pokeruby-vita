/*
 * GBA BIOS functions used by the game (src/libs/libagbsyscall.s), in C.
 * VBlankIntrWait and SoftReset are in host_core.c.
 */
#include <math.h>
#include <string.h>

#include "port/port.h"
#include "port/asm_rt.h"
#include "global.h"
#include "gba/syscall.h"

/* Game RAM variables outside gHostIwram/gHostEwram (see Makefile.port: the
 * game's .bss is renamed to game_bss). */
extern unsigned char __start_game_bss[], __stop_game_bss[];
extern unsigned char __start_iwram_data[] __attribute__((weak)), __stop_iwram_data[] __attribute__((weak));
extern unsigned char __start_ewram_data[] __attribute__((weak)), __stop_ewram_data[] __attribute__((weak));

/* clears a whole section of globals (also the gaps a sanitizer puts
 * between them) */
static __attribute__((no_sanitize_address)) void ZeroSection(unsigned char *p, unsigned char *e)
{
    while (p < e)
        *(volatile unsigned char *)p++ = 0;
}

/* SWI 0x01 */
void RegisterRamReset(u32 flags)
{
    if (flags & RESET_EWRAM) {
        memset(gHostEwram, 0, sizeof(gHostEwram));
        if (__start_ewram_data)
            ZeroSection(__start_ewram_data, __stop_ewram_data);
    }
    if (flags & RESET_IWRAM) {
        /* the last 0x200 bytes (stacks, interrupt vector) are kept */
        memset(gHostIwram, 0, 0x7E00);
        if (__start_iwram_data)
            ZeroSection(__start_iwram_data, __stop_iwram_data);
        ZeroSection(__start_game_bss, __stop_game_bss);
    }
    if (flags & RESET_PALETTE)
        memset(gHostPalram, 0, sizeof(gHostPalram));
    if (flags & RESET_VRAM)
        memset(gHostVram, 0, sizeof(gHostVram));
    if (flags & RESET_OAM)
        memset(gHostOam, 0, sizeof(gHostOam));
    if (flags & RESET_SOUND_REGS)
        memset(gHostIo + 0x60, 0, 0x50);
    if (flags & RESET_REGS) {
        memset(gHostIo, 0, 0x60);
        memset(gHostIo + 0xB0, 0, 0x100 - 0xB0);
        *(u16 *)(gHostIo + 0x00) = 0x80; /* forced blank */
        *(u16 *)(gHostIo + 0x20) = 0x100; /* BG2PA */
        *(u16 *)(gHostIo + 0x26) = 0x100; /* BG2PD */
        *(u16 *)(gHostIo + 0x30) = 0x100; /* BG3PA */
        *(u16 *)(gHostIo + 0x36) = 0x100; /* BG3PD */
    }
}

/* GBA addresses that reach the BIOS calls straight from translated
 * assembly (e.g. the summary screen's CpuSet to 0x06005B40) */
static inline void *GbaPtr(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    if (a - 0x02000000u < 0x06000000u)
        return AsmRt_Map((uint32_t)a);
    return (void *)p;
}

/* SWI 0x0B */
void CpuSet(const void *src, void *dest, u32 control)
{
    src = GbaPtr(src);
    dest = GbaPtr(dest);
    u32 count = control & 0x1FFFFF;
    int fill = (control >> 24) & 1;
    if (count == 0)   /* nothing (the source may be NULL: a map without people) */
        return;
    if (control & (1 << 26)) {
        u32 *d = (u32 *)dest;
        const u32 *s = (const u32 *)src;
        u32 v = *s;
        while (count--)
            *d++ = fill ? v : *s++;
    } else {
        u16 *d = (u16 *)dest;
        const u16 *s = (const u16 *)src;
        u16 v = *s;
        while (count--)
            *d++ = fill ? v : *s++;
    }
}

/* SWI 0x0C: words in blocks of 8 */
void CpuFastSet(const void *src, void *dest, u32 control)
{
    src = GbaPtr(src);
    dest = GbaPtr(dest);
    u32 count = (control & 0x1FFFFF);
    u32 *d = (u32 *)dest;
    const u32 *s = (const u32 *)src;
    count = (count + 7) & ~7u;
    if (count == 0)
        return;
    if (control & (1 << 24)) {
        u32 v = *s;
        while (count--)
            *d++ = v;
    } else {
        while (count--)
            *d++ = *s++;
    }
}

static void Lz77(const void *src, void *dst)
{
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    u32 size = s[1] | (s[2] << 8) | (s[3] << 16);
    u32 out = 0;
    s += 4;
    while (out < size) {
        u8 flags = *s++;
        int i;
        for (i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                s += 2;
                while (len-- && out < size) {
                    d[out] = d[out - disp];
                    out++;
                }
            } else {
                d[out++] = *s++;
            }
        }
    }
}

void LZ77UnCompWram(const void *src, void *dest) { Lz77(GbaPtr(src), GbaPtr(dest)); }
void LZ77UnCompVram(const void *src, void *dest) { Lz77(GbaPtr(src), GbaPtr(dest)); }

static void Rl(const void *src, void *dst)
{
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    u32 size = s[1] | (s[2] << 8) | (s[3] << 16);
    u32 out = 0;
    s += 4;
    while (out < size) {
        u8 flag = *s++;
        if (flag & 0x80) {
            u32 len = (flag & 0x7F) + 3;
            u8 v = *s++;
            while (len-- && out < size)
                d[out++] = v;
        } else {
            u32 len = (flag & 0x7F) + 1;
            while (len-- && out < size)
                d[out++] = *s++;
        }
    }
}

void RLUnCompWram(const void *src, void *dest) { Rl(GbaPtr(src), GbaPtr(dest)); }
void RLUnCompVram(const void *src, void *dest) { Rl(GbaPtr(src), GbaPtr(dest)); }

/* SWI 0x08 */
u16 Sqrt(u32 number)
{
    u32 r = 0, b = 1u << 30;
    while (b > number)
        b >>= 2;
    while (b) {
        if (number >= r + b) {
            number -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (u16)r;
}

/* SWI 0x09 ArcTan (tan in 1.14 fixed point), the BIOS polynomial. */
static s32 ArcTan(s32 r0)
{
    s32 r1 = -((r0 * r0) >> 14);
    s32 r3 = ((0xA9 * r1) >> 14) + 0x390;
    r3 = ((r1 * r3) >> 14) + 0x91C;
    r3 = ((r1 * r3) >> 14) + 0xFB6;
    r3 = ((r1 * r3) >> 14) + 0x16AA;
    r3 = ((r1 * r3) >> 14) + 0x2081;
    r3 = ((r1 * r3) >> 14) + 0x3651;
    r3 = ((r1 * r3) >> 14) + 0xA2F9;
    return (r0 * r3) >> 16;
}

/* SWI 0x0A */
u16 ArcTan2(s16 x, s16 y)
{
    if (y == 0)
        return x >= 0 ? 0 : 0x8000;
    if (x == 0)
        return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y)
                return (u16)ArcTan(((s32)y << 14) / x);
        } else if (-x >= y) {
            return (u16)(ArcTan(((s32)y << 14) / x) + 0x8000);
        }
        return (u16)(0x4000 - ArcTan(((s32)x << 14) / y));
    } else {
        if (x <= 0) {
            if (-x > -y)
                return (u16)(ArcTan(((s32)y << 14) / x) + 0x8000);
        } else if (x >= -y) {
            return (u16)(ArcTan(((s32)y << 14) / x) + 0x10000);
        }
        return (u16)(0xC000 - ArcTan(((s32)x << 14) / y));
    }
}

/* The BIOS sine table: 256 entries, 1.14 fixed point. */
static s16 sSin[256];

static void InitSin(void)
{
    int i;
    if (sSin[64])
        return;
    for (i = 0; i < 256; i++)
        sSin[i] = (s16)lrint(sin(i * 2.0 * M_PI / 256.0) * 16384.0);
}

/* SWI 0x0E */
void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    InitSin();
    while (count-- > 0) {
        s32 s = sSin[(src->alpha >> 8) & 0xFF];
        s32 c = sSin[((src->alpha >> 8) + 64) & 0xFF];
        s32 pa = (src->sx * c) >> 14;
        s32 pb = -((src->sx * s) >> 14);
        s32 pc = (src->sy * s) >> 14;
        s32 pd = (src->sy * c) >> 14;
        dest->pa = (s16)pa;
        dest->pb = (s16)pb;
        dest->pc = (s16)pc;
        dest->pd = (s16)pd;
        dest->dx = src->texX - (pa * src->scrX + pb * src->scrY);
        dest->dy = src->texY - (pc * src->scrX + pd * src->scrY);
        src++;
        dest++;
    }
}

/* SWI 0x0F: writes pa, pb, pc, pd, `offset` bytes apart */
void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    u8 *d = (u8 *)dest;
    InitSin();
    while (count-- > 0) {
        s32 s = sSin[(src->rotation >> 8) & 0xFF];
        s32 c = sSin[((src->rotation >> 8) + 64) & 0xFF];
        *(s16 *)(d + 0 * offset) = (s16)((src->xScale * c) >> 14);
        *(s16 *)(d + 1 * offset) = (s16)(-((src->xScale * s) >> 14));
        *(s16 *)(d + 2 * offset) = (s16)((src->yScale * s) >> 14);
        *(s16 *)(d + 3 * offset) = (s16)((src->yScale * c) >> 14);
        d += 4 * offset;
        src++;
    }
}

int MultiBoot(struct MultiBootParam *mp)
{
    (void)mp;
    return 1; /* no link cable */
}
