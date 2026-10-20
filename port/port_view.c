/*
 * Wide view (480x272, the PS Vita screen at exactly 2x) for the overworld.
 *
 * The GBA shows 15x10 metatiles of the map; the game keeps a 32x32-tile ring
 * of them in VRAM (BG1-3) and scrolls over it. In the wide view the port
 * builds its own 64x64-tile maps of BG1-3 (VRAM beyond the GBA's 96 KiB) for
 * the whole wide frame:
 *  - the part the GBA would show is copied from the game's ring, so door
 *    animations and other directly drawn metatiles stay exact;
 *  - the rest comes from the map data, beyond the map's edge from the
 *    connected maps (src/fieldmap.c PortMetatileTilesWide).
 * BG0 (text boxes, menus) stays the GBA screen in the middle; sprites use the
 * game's full coordinates (src/sprite.c gPortOamX/Y) instead of the 9-bit
 * OAM ones; the object events are kept alive and visible in the wider area
 * (src/event_object_movement.c). Everything that is not the overworld is
 * shown as the GBA screen at the same zoom.
 */
#include "global.h"
#include "main.h"

#include "port/port.h"
#include "port/port_config.h"
#include "port/port_view.h"
#include "port/ppu/include/cpu/mode1.h"

int gPortViewExtraX, gPortViewExtraY;

extern s16 gPortOamX[128], gPortOamY[128];
void PortMetatileTilesWide(int x, int y, u16 *bg1, u16 *bg2, u16 *bg3);
void PortFieldCameraTileOffset(u8 *xTile, u8 *yTile);
int PortIsOverworld(void);

#define EXT_BASE 0x18000u /* BG1..3 wide maps, 0x2000 bytes each */

static struct {
    int valid;
    int wx, wy;          /* world pixel at the GBA screen's top-left */
    u8 xTile, yTile;
    s16 posX, posY;
} sSnap;

static inline u16 IoRead(u32 off)
{
    return (u16)(gHostIo[off] | (gHostIo[off + 1] << 8));
}

void PortView_VBlank(void)
{
    u16 dispcnt = IoRead(0x000);
    int b;

    gPortViewExtraX = gPortConfig.viewMode == PORT_VIEW_FULL ? PORT_WIDE_OX : 0;
    gPortViewExtraY = gPortConfig.viewMode == PORT_VIEW_FULL ? PORT_WIDE_OY : 0;
    sSnap.valid = 0;
    if (gPortConfig.viewMode != PORT_VIEW_FULL || !PortIsOverworld() || (dispcnt & 7) != 0)
        return;
    /* the field map: BG1-3 as 32x32 maps with the same scroll */
    for (b = 1; b <= 3; b++) {
        u16 cnt = IoRead(0x008 + b * 2);
        if (cnt & 0xC000)
            return;
        if (IoRead(0x010 + b * 4) != IoRead(0x014) || IoRead(0x012 + b * 4) != IoRead(0x016))
            return;
    }
    PortFieldCameraTileOffset(&sSnap.xTile, &sSnap.yTile);
    sSnap.posX = gSaveBlock1.pos.x;
    sSnap.posY = gSaveBlock1.pos.y;
    sSnap.wx = sSnap.posX * 16 + (s8)((IoRead(0x014) - sSnap.xTile * 8) & 0xFF);
    sSnap.wy = sSnap.posY * 16 + (s8)((IoRead(0x016) - sSnap.yTile * 8) & 0xFF);
    sSnap.valid = 1;
}

static inline u16 *WideEntry(int bg, int tx, int ty)
{
    u32 sb = (u32)((tx >> 5) & 1) + (u32)((ty >> 5) & 1) * 2;
    return (u16 *)(gHostVram + EXT_BASE + (u32)(bg - 1) * 0x2000u + sb * 0x800u) + ((ty & 31) * 32 + (tx & 31));
}

static void FillWorld(void)
{
    const int left = sSnap.wx - PORT_WIDE_OX, top = sSnap.wy - PORT_WIDE_OY;
    const int mx0 = left >> 4, mx1 = (left + PORT_WIDE_W - 1) >> 4;
    const int my0 = top >> 4, my1 = (top + PORT_WIDE_H - 1) >> 4;
    const u16 *game[4];
    int mx, my, b;

    for (b = 1; b <= 3; b++)
        game[b] = (const u16 *)(gHostVram + ((IoRead(0x008 + b * 2) >> 8) & 0x1F) * 0x800u);

    for (my = my0; my <= my1; my++) {
        for (mx = mx0; mx <= mx1; mx++) {
            u16 t[4][4];
            const int tx = mx * 2, ty = my * 2;
            const int dx = mx - sSnap.posX, dy = my - sSnap.posY;
            /* shown by the GBA too: take the game's own tilemap */
            const int onGba = mx * 16 + 16 > sSnap.wx && mx * 16 < sSnap.wx + PORT_GBA_W &&
                              my * 16 + 16 > sSnap.wy && my * 16 < sSnap.wy + PORT_GBA_H &&
                              dx >= 0 && dx < 16 && dy >= 0 && dy < 16;
            if (onGba) {
                const int gx = (sSnap.xTile + dx * 2) & 31, gy = (sSnap.yTile + dy * 2) & 31;
                for (b = 1; b <= 3; b++) {
                    t[b][0] = game[b][gy * 32 + gx];
                    t[b][1] = game[b][gy * 32 + ((gx + 1) & 31)];
                    t[b][2] = game[b][((gy + 1) & 31) * 32 + gx];
                    t[b][3] = game[b][((gy + 1) & 31) * 32 + ((gx + 1) & 31)];
                }
            } else {
                PortMetatileTilesWide(mx, my, t[1], t[2], t[3]);
            }
            for (b = 1; b <= 3; b++) {
                *WideEntry(b, tx, ty) = t[b][0];
                *WideEntry(b, tx + 1, ty) = t[b][1];
                *WideEntry(b, tx, ty + 1) = t[b][2];
                *WideEntry(b, tx + 1, ty + 1) = t[b][3];
            }
        }
    }
    for (b = 1; b <= 3; b++) {
        virtuappu_mode1_bg_screen_override[b] = EXT_BASE + (u32)(b - 1) * 0x2000u;
        virtuappu_mode1_bg_dx[b] = (left - IoRead(0x010 + b * 4)) & 511;
        virtuappu_mode1_bg_dy[b] = (top - IoRead(0x012 + b * 4)) & 511;
    }
}

static void Reset(void)
{
    int b, i;
    for (b = 0; b < 4; b++) {
        virtuappu_mode1_bg_screen_override[b] = 0;
        virtuappu_mode1_bg_clip_on[b] = 0;
        virtuappu_mode1_bg_dx[b] = 0;
        virtuappu_mode1_bg_dy[b] = 0;
    }
    virtuappu_mode1_win_dx = virtuappu_mode1_win_dy = 0;
    virtuappu_mode1_line_dy = 0;
    virtuappu_mode1_obj_port = 0;
    for (i = 0; i < 128; i++)
        virtuappu_mode1_obj_dx[i] = virtuappu_mode1_obj_dy[i] = 0;
}

int PortView_BeginFrame(void)
{
    int i;

    Reset();
    if (!sSnap.valid)
        return 0;

    FillWorld();
    /* BG0: the GBA screen in the middle */
    virtuappu_mode1_bg_clip_on[0] = 1;
    virtuappu_mode1_bg_clip_x0[0] = PORT_WIDE_OX;
    virtuappu_mode1_bg_clip_x1[0] = PORT_WIDE_OX + PORT_GBA_W;
    virtuappu_mode1_bg_clip_y0[0] = PORT_WIDE_OY;
    virtuappu_mode1_bg_clip_y1[0] = PORT_WIDE_OY + PORT_GBA_H;
    virtuappu_mode1_bg_dx[0] = -PORT_WIDE_OX & 511;
    virtuappu_mode1_bg_dy[0] = -PORT_WIDE_OY & 511;

    virtuappu_mode1_win_dx = PORT_WIDE_OX;
    virtuappu_mode1_win_dy = PORT_WIDE_OY;
    virtuappu_mode1_line_dy = PORT_WIDE_OY;

    virtuappu_mode1_obj_port = 1;
    for (i = 0; i < 128; i++) {
        virtuappu_mode1_obj_base_x[i] = gPortOamX[i];
        virtuappu_mode1_obj_base_y[i] = gPortOamY[i];
        virtuappu_mode1_obj_dx[i] = PORT_WIDE_OX;
        virtuappu_mode1_obj_dy[i] = PORT_WIDE_OY;
    }
    return 1;
}
