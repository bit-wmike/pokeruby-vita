/*
 * Runtime support for game functions that exist only as assembly (NAKED)
 * and were translated to C by tools/port/naked2c.py.
 *
 * The translated code computes GBA addresses (e.g. REG_BASE built from
 * immediates, or 0x02000000 for gSharedMem); every memory access maps the
 * GBA regions onto the port's arrays. Host addresses never fall into
 * 0x02000000-0x07FFFFFF (32-bit Linux test build: 0x08048000 and up; Vita:
 * 0x81000000 and up).
 */
#ifndef PORT_ASM_RT_H
#define PORT_ASM_RT_H

#include <stdint.h>

extern unsigned char gHostEwram[], gHostIwram[], gHostIo[], gHostPalram[], gHostVram[], gHostOam[];

static inline unsigned char *AsmRt_Map(uint32_t a)
{
    switch (a >> 24) {
        case 0x02: return gHostEwram + (a & 0x3FFFF);
        case 0x03: return gHostIwram + (a & 0x7FFF);
        case 0x04: return gHostIo + (a & 0x3FF);
        case 0x05: return gHostPalram + (a & 0x3FF);
        case 0x06: {
            uint32_t o = a & 0x1FFFF;
            if (o >= 0x18000)
                o -= 0x8000;
            return gHostVram + o;
        }
        case 0x07: return gHostOam + (a & 0x3FF);
    }
    return (unsigned char *)(uintptr_t)a;
}

#define P(a) ((void *)AsmRt_Map(a))
#define LD8(a) (*(uint8_t *)AsmRt_Map(a))
#define LD16(a) (*(uint16_t *)AsmRt_Map(a))
#define LD32(a) (*(uint32_t *)AsmRt_Map(a))
#define ST8(a, x) (*(uint8_t *)AsmRt_Map(a) = (uint8_t)(x))
#define ST16(a, x) (*(uint16_t *)AsmRt_Map(a) = (uint16_t)(x))
#define ST32(a, x) (*(uint32_t *)AsmRt_Map(a) = (uint32_t)(x))

static inline uint32_t SHIFT_LSL(uint32_t x, uint32_t a, uint32_t *c)
{
    if (a == 0) return x;
    if (a < 32) { *c = (x >> (32 - a)) & 1; return x << a; }
    *c = (a == 32) ? (x & 1) : 0;
    return 0;
}

static inline uint32_t SHIFT_LSR(uint32_t x, uint32_t a, uint32_t *c)
{
    if (a == 0) return x;
    if (a < 32) { *c = (x >> (a - 1)) & 1; return x >> a; }
    *c = (a == 32) ? (x >> 31) : 0;
    return 0;
}

static inline uint32_t SHIFT_ASR(uint32_t x, uint32_t a, uint32_t *c)
{
    if (a == 0) return x;
    if (a < 32) { *c = ((uint32_t)((int32_t)x >> (a - 1))) & 1; return (uint32_t)((int32_t)x >> a); }
    *c = x >> 31;
    return (uint32_t)((int32_t)x >> 31);
}

static inline uint32_t SHIFT_ROR(uint32_t x, uint32_t a, uint32_t *c)
{
    if (a == 0) return x;
    a &= 31;
    if (a == 0) { *c = x >> 31; return x; }
    x = (x >> a) | (x << (32 - a));
    *c = x >> 31;
    return x;
}

typedef uint32_t (*AsmRt_Fn)(uint32_t, uint32_t, uint32_t, uint32_t);
#define ASMRT_CALL(f, a, b, c, d) (((AsmRt_Fn)(uintptr_t)(f))((a), (b), (c), (d)))

#endif /* PORT_ASM_RT_H */
