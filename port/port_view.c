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
#include <stdio.h>
#include <stdlib.h>
#include "global.h"
#include "main.h"
#include "fieldmap.h"
#include "constants/map_types.h"

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
    int fx, fy;          /* world pixel at the wide frame's top-left */
    u8 xTile, yTile;
    s16 posX, posY;
} sSnap;

static inline u16 IoRead(u32 off)
{
    return (u16)(gHostIo[off] | (gHostIo[off + 1] << 8));
}

/* Where the wide frame looks at. Outdoors it follows the GBA camera (the
 * player in the middle). Rooms (houses, centres, gyms, secret bases) stay put: a
 * room smaller than the screen is shown whole in the middle, a bigger one
 * follows the player but never shows more than the room. Caves are shown
 * like the open world. */
static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void FrameOrigin(void)
{
    const struct MapLayout *l = gMapHeader.mapLayout;
    /* caves are shown like the open world; houses, centres, gyms stay put */
    int room = gMapHeader.mapType == MAP_TYPE_INDOOR || gMapHeader.mapType == MAP_TYPE_SECRET_BASE;
    sSnap.fx = sSnap.wx - PORT_WIDE_OX;
    sSnap.fy = sSnap.wy - PORT_WIDE_OY;
    if (room && l) {
        int x0 = MAP_OFFSET * 16, x1 = (MAP_OFFSET + l->width) * 16;
        int y0 = MAP_OFFSET * 16, y1 = (MAP_OFFSET + l->height) * 16;
        sSnap.fx = x1 - x0 <= PORT_WIDE_W ? (x0 + x1) / 2 - PORT_WIDE_W / 2 : Clamp(sSnap.fx, x0, x1 - PORT_WIDE_W);
        sSnap.fy = y1 - y0 <= PORT_WIDE_H ? (y0 + y1) / 2 - PORT_WIDE_H / 2 : Clamp(sSnap.fy, y0, y1 - PORT_WIDE_H);
    }
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
    FrameOrigin();
    sSnap.valid = 1;
}

static inline u16 *WideEntry(int bg, int tx, int ty)
{
    u32 sb = (u32)((tx >> 5) & 1) + (u32)((ty >> 5) & 1) * 2;
    return (u16 *)(gHostVram + EXT_BASE + (u32)(bg - 1) * 0x2000u + sb * 0x800u) + ((ty & 31) * 32 + (tx & 31));
}

static void FillWorld(void)
{
    const int left = sSnap.fx, top = sSnap.fy;
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
        virtuappu_mode1_bg_region_count[b] = 0;
    }
    virtuappu_mode1_win_dx = virtuappu_mode1_win_dy = 0;
    virtuappu_mode1_line_dy = 0;
    virtuappu_mode1_obj_port = 0;
    for (i = 0; i < 128; i++)
        virtuappu_mode1_obj_dx[i] = virtuappu_mode1_obj_dy[i] = 0;
}

/* ---- text windows (BG0) to the edges of the wide frame ----------------
 * The game draws its windows (message box, menus, yes/no, map name) into
 * BG0 of the GBA screen. They are found as rectangles of non-empty tiles
 * and drawn at the matching edge of the wide frame: left/middle/right and
 * top/middle/bottom like on the GBA; boxes as wide as the GBA screen are
 * stretched to the full width (the right part keeps its place, a column
 * near the right edge is repeated). */
static void AddRegion(int x0, int x1, int y0, int y1, int dx, int dy)
{
    int n = virtuappu_mode1_bg_region_count[0];
    if (n >= VIRTUAPPU_MODE1_MAX_REGIONS || x0 >= x1 || y0 >= y1)
        return;
    virtuappu_mode1_bg_regions[0][n].x0 = x0;
    virtuappu_mode1_bg_regions[0][n].x1 = x1;
    virtuappu_mode1_bg_regions[0][n].y0 = y0;
    virtuappu_mode1_bg_regions[0][n].y1 = y1;
    virtuappu_mode1_bg_regions[0][n].dx = dx;
    virtuappu_mode1_bg_regions[0][n].dy = dy;
    virtuappu_mode1_bg_region_count[0] = n + 1;
}

static struct {
    int x0, x1, y0, y1, dx, dy, split, rdx;
    int moves, mc0, mr0, mdx[4], mdy[4];   /* battle move list in one row */
} sWin[16];
int gPortViewBattle;   /* set by the battle stage: battle layout */
static int sWinCount;

/* entry (tx, ty) of a text BG map of 32/64 x 32/64 tiles */
static inline u16 MapEntry(const u16 *map, int wide64, int tall64, int tx, int ty)
{
    int sb;
    tx &= wide64 ? 63 : 31;
    ty &= tall64 ? 63 : 31;
    sb = (tx >> 5) + (ty >> 5) * (wide64 ? 2 : 1);
    return map[sb * 1024 + (ty & 31) * 32 + (tx & 31)];
}

/* one window: tile rect [c0,c1]x[r0,r1] of the visible BG0 tiles */
/* rx: right edge of a stretched box (0: like on the GBA), hdxIn: fixed
 * horizontal offset (-1: by position) */
static void PlaceWindow(const u16 *map, int wide64, int tall64, int hofs, int vofs, int c0, int c1, int r0, int r1,
                        int rxIn, int hdxIn)
{
    int gx0 = c0 * 8 - (hofs & 7), gx1 = (c1 + 1) * 8 - (hofs & 7);
    int gy0 = r0 * 8 - (vofs & 7), gy1 = (r1 + 1) * 8 - (vofs & 7);
    int hdx, vdy, split = 9999, rdx = 0;
    const int ex = PORT_WIDE_W - PORT_GBA_W, ey = PORT_WIDE_H - PORT_GBA_H;

    if (gx0 < 0) gx0 = 0;
    if (gx1 > PORT_GBA_W) gx1 = PORT_GBA_W;
    if (gy0 < 0) gy0 = 0;
    if (gy1 > PORT_GBA_H) gy1 = PORT_GBA_H;
    if (gx0 >= gx1 || gy0 >= gy1)
        return;
    vdy = gy0 <= 8 ? 0 : gy1 >= 100 ? ey : ey / 2;
    hdx = (gx0 + gx1) / 2 < 80 ? 0 : (gx0 + gx1) / 2 > 160 ? ex : ex / 2;
    if (hdxIn >= 0)
        hdx = hdxIn;

    if (gx1 - gx0 >= 26 * 8 || rxIn > 0) {
        /* A box across the GBA screen goes across the wide frame.
         * Battle boxes use one map entry for their empty cells: they are
         * stretched at the end of the longest run of equal columns (after
         * the text, between text box and command box). Field boxes have a
         * tile of their own per cell (the text is drawn into the tiles), so
         * the column after the left border - never text - is repeated
         * between the box and its right border. */
        int x, cc, k, run = 0, bestRun = 0, fillCol = -1, fx, rx = rxIn > 0 ? rxIn : PORT_WIDE_W - (PORT_GBA_W - gx1);
        hdx = 0;
        for (cc = c0 + 1; cc <= c1; cc++) {
            int same = 1;
            for (k = r0; k <= r1 && same; k++)
                same = MapEntry(map, wide64, tall64, (hofs >> 3) + cc, (vofs >> 3) + k) ==
                       MapEntry(map, wide64, tall64, (hofs >> 3) + cc - 1, (vofs >> 3) + k);
            run = same ? run + 1 : 0;
            if (same && run >= bestRun) {
                bestRun = run;
                fillCol = cc;
            }
        }
        if (bestRun >= 3) {
            int rest;
            fx = fillCol * 8 - (hofs & 7);
            rest = gx1 - (fx + 8);
            AddRegion(gx0, fx + 8, gy0 + vdy, gy1 + vdy, 0, vdy);
            for (x = fx + 8; x < rx - rest; x += 8)
                AddRegion(x, x + 8 < rx - rest ? x + 8 : rx - rest, gy0 + vdy, gy1 + vdy, x - fx, vdy);
            AddRegion(rx - rest, rx, gy0 + vdy, gy1 + vdy, rx - gx1, vdy);
            split = fx + 8;
        } else {
            fx = gx0 + 8;   /* the column after the left border */
            AddRegion(gx0, gx1 - 8, gy0 + vdy, gy1 + vdy, 0, vdy);
            for (x = gx1 - 8; x < rx - 8; x += 8)
                AddRegion(x, x + 8 < rx - 8 ? x + 8 : rx - 8, gy0 + vdy, gy1 + vdy, x - fx, vdy);
            AddRegion(rx - 8, rx, gy0 + vdy, gy1 + vdy, rx - gx1, vdy);
            split = gx1 - 8;
        }
        rdx = rx - gx1;
    } else {
        AddRegion(gx0 + hdx, gx1 + hdx, gy0 + vdy, gy1 + vdy, hdx, vdy);
    }
    if (sWinCount < 16) {
        sWin[sWinCount].x0 = gx0; sWin[sWinCount].x1 = gx1;
        sWin[sWinCount].y0 = gy0; sWin[sWinCount].y1 = gy1;
        sWin[sWinCount].dx = hdx; sWin[sWinCount].dy = vdy;
        sWin[sWinCount].split = split;
        sWin[sWinCount].rdx = rdx;
        sWin[sWinCount].moves = 0;
        sWinCount++;
    }
}

struct Rect { int c0, c1, r0, r1; };

#define CELL(c, r) MapEntry(map, wide64, tall64, (hofs >> 3) + (c), (vofs >> 3) + (r))

/* Battle: boxes side by side are one run of tiles; a box starts where a
 * column repeats the first column (same frame tiles: move list | PP box)
 * or where the palette changes in every row (message box | command box). */
static int SplitBoxes(const u16 *map, int wide64, int tall64, int hofs, int vofs, struct Rect *rect, int n)
{
    int i, s, r;
    for (i = 0; i < n; i++) {
        struct Rect *q = &rect[i];
        for (s = q->c0 + 2; s < q->c1; s++) {
            int same = 1, pal = 1;
            for (r = q->r0; r <= q->r1; r++) {
                u16 a = CELL(s, r), b = CELL(s - 1, r);
                if (a != CELL(q->c0, r))
                    same = 0;
                if (!(a & 0x3FF) || !(b & 0x3FF) || (a >> 12) == (b >> 12))
                    pal = 0;
            }
            if ((same && q->r1 > q->r0) || pal) {
                if (n < 64) {
                    rect[n] = *q;
                    rect[n].c0 = s;
                    q->c1 = s - 1;
                    n++;
                }
                break;
            }
        }
    }
    return n;
}

/* The four moves of the move list (2x2 on the GBA) in one row. */
static int MoveRow(const u16 *map, int wide64, int tall64, int hofs, int vofs, const struct Rect *q, int rx)
{
    int gx0 = q->c0 * 8 - (hofs & 7), gy0 = q->r0 * 8 - (vofs & 7), gy1 = (q->r1 + 1) * 8 - (vofs & 7);
    const int vdy = PORT_WIDE_H - PORT_GBA_H;
    int blank = -1, c, r, m, slot, y;
    if (q->c1 - q->c0 != 21 || q->r1 - q->r0 != 5 || gx0 != 0)
        return 0;
    for (c = q->c1 - 1; c > q->c0 && blank < 0; c--) {   /* an empty column */
        int ok = 1;
        for (r = q->r0 + 2; r < q->r1 && ok; r++)
            ok = CELL(c, r) == CELL(c, q->r0 + 1);
        if (ok)
            blank = c;
    }
    if (blank < 0 || sWinCount >= 16)
        return 0;
    AddRegion(0, 8, gy0 + vdy, gy1 + vdy, 0, vdy);
    for (c = 8; c < rx - 8; c += 8)
        AddRegion(c, c + 8 < rx - 8 ? c + 8 : rx - 8, gy0 + vdy, gy1 + vdy, c - blank * 8, vdy);
    AddRegion(rx - 8, rx, gy0 + vdy, gy1 + vdy, rx - 8 - q->c1 * 8, vdy);
    slot = (rx - 16) / 4;
    y = gy0 + vdy + 16;
    for (m = 0; m < 4; m++) {
        int sx = (q->c0 + 1 + (m & 1) * 10) * 8, sy = gy0 + 8 + (m >> 1) * 16;
        int dx = 8 + m * slot + (slot - 80) / 2;
        AddRegion(dx, dx + 80, y, y + 16, dx - sx, y - sy);
        sWin[sWinCount].mdx[m] = dx - sx;
        sWin[sWinCount].mdy[m] = y - sy;
    }
    sWin[sWinCount].x0 = gx0; sWin[sWinCount].x1 = gx0 + (q->c1 - q->c0 + 1) * 8;
    sWin[sWinCount].y0 = gy0; sWin[sWinCount].y1 = gy1;
    sWin[sWinCount].dx = 0; sWin[sWinCount].dy = vdy;
    sWin[sWinCount].split = 9999; sWin[sWinCount].rdx = 0;
    sWin[sWinCount].moves = 1;
    sWin[sWinCount].mc0 = gx0; sWin[sWinCount].mr0 = gy0;
    sWinCount++;
    return 1;
}

/* Battle: two boxes side by side at the bottom - the right one to the
 * right edge, the left one stretched up to it (the move list: one row). */
static int BattleWindow(const u16 *map, int wide64, int tall64, int hofs, int vofs, const struct Rect *rect, int n, int i)
{
    const struct Rect *q = &rect[i];
    int k;
    if ((q->r1 + 1) * 8 - (vofs & 7) < 150)
        return 0;
    for (k = 0; k < n; k++) {
        const struct Rect *o = &rect[k];
        if (k == i || o->r0 != q->r0 || o->r1 != q->r1)
            continue;
        if (o->c0 == q->c1 + 1) {          /* q is the left box */
            int rx = o->c0 * 8 - (hofs & 7) + PORT_WIDE_W - PORT_GBA_W;
            if (!MoveRow(map, wide64, tall64, hofs, vofs, q, rx))
                PlaceWindow(map, wide64, tall64, hofs, vofs, q->c0, q->c1, q->r0, q->r1, rx, 0);
            return 1;
        }
        if (o->c1 + 1 == q->c0) {          /* q is the right box */
            PlaceWindow(map, wide64, tall64, hofs, vofs, q->c0, q->c1, q->r0, q->r1, 0, PORT_WIDE_W - PORT_GBA_W);
            return 1;
        }
    }
    return 0;
}

void PortView_PlaceWindows(void)
{
    const u16 cnt = IoRead(0x008);
    const u16 *map = (const u16 *)(gHostVram + ((cnt >> 8) & 0x1F) * 0x800u);
    const int wide64 = (cnt >> 14) & 1, tall64 = (cnt >> 15) & 1;
    const int hofs = IoRead(0x010) & 0x1FF, vofs = IoRead(0x012) & 0x1FF;
    /* only the tiles the GBA shows (a 31st column / 21st row only when
     * the map is scrolled by part of a tile) */
    const int cols = 30 + ((hofs & 7) != 0), rows = 20 + ((vofs & 7) != 0);
    static u8 mask[21][31], seen[21][31];
    struct Rect rect[64];
    int nrect = 0, r, c, i, j, merged;

    virtuappu_mode1_bg_dx[0] = 0;
    virtuappu_mode1_bg_dy[0] = 0;
    virtuappu_mode1_bg_region_count[0] = 0;
    sWinCount = 0;

#ifndef __vita__
    if (getenv("PKR_DUMP_BG0") && Port_FrameCounter() == (u32)atoi(getenv("PKR_DUMP_BG0"))) {
        fprintf(stderr, "io dispcnt=%04x bg0cnt=%04x hofs=%d vofs=%d win0h=%04x win0v=%04x winin=%04x winout=%04x bldcnt=%04x win1h=%04x win1v=%04x\n", IoRead(0), IoRead(8), IoRead(0x10), IoRead(0x12), IoRead(0x40), IoRead(0x44), IoRead(0x48), IoRead(0x4A), IoRead(0x50), IoRead(0x42), IoRead(0x46));
        for (r = 0; r < 20; r++) {
            for (c = 0; c < 30; c++)
                fprintf(stderr, "%04x ", MapEntry(map, wide64, tall64, (hofs >> 3) + c, (vofs >> 3) + r));
            fprintf(stderr, "\n");
        }
    }
#endif
    /* empty cells use tile 0 (any palette) */
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            u16 e = MapEntry(map, wide64, tall64, (hofs >> 3) + c, (vofs >> 3) + r);
            mask[r][c] = (e & 0x3FF) != 0;
            seen[r][c] = 0;
        }
    /* windows are rectangles: runs of non-empty cells per row (gaps of one
     * empty cell bridged), rows with the same run stacked into one window */
    for (r = 0; r < rows; r++) {
        c = 0;
        while (c < cols) {
            int c0, c1, k, found = -1;
            while (c < cols && !mask[r][c])
                c++;
            if (c >= cols)
                break;
            c0 = c;
            while (c < cols && (mask[r][c] || (c + 1 < cols && mask[r][c + 1])))
                c++;
            c1 = c - 1;
            for (k = 0; k < nrect; k++)
                if (rect[k].c0 == c0 && rect[k].c1 == c1 && rect[k].r1 >= r - 2)
                    found = k;
            if (found >= 0)
                rect[found].r1 = r;
            else if (nrect < 64) {
                rect[nrect].c0 = c0; rect[nrect].c1 = c1; rect[nrect].r0 = r; rect[nrect].r1 = r;
                nrect++;
            }
        }
    }
    (void)seen; (void)merged; (void)j;
    if (gPortViewBattle)
        nrect = SplitBoxes(map, wide64, tall64, hofs, vofs, rect, nrect);
    for (i = 0; i < nrect; i++) {
#ifndef __vita__
        if (getenv("PKR_DUMP_BG0") && Port_FrameCounter() == (u32)atoi(getenv("PKR_DUMP_BG0")))
            fprintf(stderr, "rect c%d-%d r%d-%d hofs=%d vofs=%d\n", rect[i].c0, rect[i].c1, rect[i].r0, rect[i].r1, hofs, vofs);
#endif
        if (!gPortViewBattle || !BattleWindow(map, wide64, tall64, hofs, vofs, rect, nrect, i))
            PlaceWindow(map, wide64, tall64, hofs, vofs, rect[i].c0, rect[i].c1, rect[i].r0, rect[i].r1, 0, -1);
    }
}

/* An OAM entry inside a text window moves with the window; returns 1. */
int PortView_SpriteInWindow(int i)
{
    int w;
    extern u16 gPortOamTag[128];
    if (gPortOamTag[i] == 0xFFFF || gPortOamTag[i] == 0)  /* people, objects */
        return 0;
    for (w = 0; w < sWinCount; w++) {
        int cx = gPortOamX[i] + 4, cy = gPortOamY[i] + 4;
        if (cx >= sWin[w].x0 && cx < sWin[w].x1 && cy >= sWin[w].y0 && cy < sWin[w].y1) {
            if (sWin[w].moves) {   /* the cursor of a move */
                int m = (cx >= sWin[w].mc0 + 88 ? 1 : 0) + (cy >= sWin[w].mr0 + 24 ? 2 : 0);
                virtuappu_mode1_obj_dx[i] = (s16)sWin[w].mdx[m];
                virtuappu_mode1_obj_dy[i] = (s16)sWin[w].mdy[m];
                return 1;
            }
            virtuappu_mode1_obj_dx[i] = (s16)(cx >= sWin[w].split ? sWin[w].rdx : sWin[w].dx);
            virtuappu_mode1_obj_dy[i] = (s16)sWin[w].dy;
            return 1;
        }
    }
    return 0;
}

int PortView_BeginFrame(void)
{
    int i, ox, oy;

    Reset();
#ifndef __vita__
    {   /* host: PKR_KEY_BG3=1 hides BG3 and keys the backdrop (mock-ups) */
        static int key = -1;
        if (key < 0)
            key = getenv("PKR_KEY_BG3") != NULL;
        virtuappu_mode1_backdrop_key = 0;
        if (key && !sSnap.valid) {
            virtuappu_mode1_backdrop_key = 0xFFFF00FFu;
            virtuappu_mode1_bg_clip_on[3] = 1;
        }
    }
#endif
#ifndef __vita__
    if (!sSnap.valid && getenv("PKR_WIDE_ALL")) {  /* experiment: everything wide */
        for (i = 0; i < 4; i++) {
            virtuappu_mode1_bg_dx[i] = -PORT_WIDE_OX & 511;
            virtuappu_mode1_bg_dy[i] = -PORT_WIDE_OY & 511;
        }
        virtuappu_mode1_win_dx = PORT_WIDE_OX; virtuappu_mode1_win_dy = PORT_WIDE_OY;
        virtuappu_mode1_line_dy = PORT_WIDE_OY;
        virtuappu_mode1_obj_port = 1;
        for (i = 0; i < 128; i++) {
            virtuappu_mode1_obj_base_x[i] = gPortOamX[i]; virtuappu_mode1_obj_base_y[i] = gPortOamY[i];
            virtuappu_mode1_obj_dx[i] = PORT_WIDE_OX; virtuappu_mode1_obj_dy[i] = PORT_WIDE_OY;
        }
        return 1;
    }
#endif
    if (!sSnap.valid)
        return 0;

    FillWorld();
    ox = sSnap.wx - sSnap.fx;   /* where the GBA screen is in the frame */
    oy = sSnap.wy - sSnap.fy;
    PortView_PlaceWindows();

    virtuappu_mode1_win_dx = ox;
    virtuappu_mode1_win_dy = oy;
    virtuappu_mode1_line_dy = oy;

    virtuappu_mode1_obj_port = 1;
    for (i = 0; i < 128; i++) {
        virtuappu_mode1_obj_base_x[i] = gPortOamX[i];
        virtuappu_mode1_obj_base_y[i] = gPortOamY[i];
        virtuappu_mode1_obj_dx[i] = (s16)ox;
        virtuappu_mode1_obj_dy[i] = (s16)oy;
        PortView_SpriteInWindow(i);  /* e.g. the menu cursor */
    }
    return 1;
}
