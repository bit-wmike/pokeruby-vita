/*
 * Battle stage (wide view): the battle in front of the place where it
 * happens, in the style of SNES role-playing games.
 *
 * At the first frame of a battle the stage is built once from the field
 * map that is still loaded:
 *  - the ground ahead of the player in perspective (mode-7 like), from the
 *    real metatiles (grass, sand, water, paths), solid blocks replaced by
 *    the most common ground around;
 *  - solid things (trees, rocks, signs, walls) as upright cards at their map
 *    position, far to near, a clearing left where the Pokemon stand;
 *  - a sky (outside) or darkness (caves, rooms), haze in the distance, a
 *    far tree line outside, soft shadows ("platforms") under the Pokemon;
 *  - softened a little.
 * Every battle frame: the GBA picture is drawn with BG3 (the GBA battle
 * background) hidden and the backdrop keyed; key pixels get the stage
 * (faded like the palettes) and the field weather drawn over it
 * (rain, sandstorm, ash, snow, fog, sun) - behind the Pokemon, always on.
 * The healthboxes go to the top corners, the text windows (BG0) to the
 * edges like in the field.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "main.h"
#include "fieldmap.h"
#include "palette.h"
#include "field_weather.h"
#include "constants/map_types.h"
#include "constants/weather.h"

#include "port/port.h"
#include "port/port_config.h"
#include "port/port_view.h"
#include "port/ppu/include/cpu/mode1.h"

#define W PORT_WIDE_W
#define H PORT_WIDE_H
#define HOR 96          /* horizon line */
#define CAM_H 30.0f     /* camera height (map pixels) */
#define FOC 220.0f      /* focal length */
/* The GBA battle picture (field: BG1/BG2 and the sprites) is drawn as a
 * 320x160 picture from GBA x -40 and shown 1.5x: GBA (gx, gy) is at
 * ((gx + 40) * 1.5, gy * 1.5 + 56) - the field over the whole width, GBA
 * line 112 (top of the message box) at the top of the text windows. */
#define FW 320
#define FH 160
#define FX0 40
#define FY 56
#define SX(gx) (((gx) + FX0) * 3 / 2)
#define SY(gy) ((gy) * 3 / 2 + FY)
#define KEY 0x00FF00FFu /* backdrop key (alpha 0) */

extern s16 gPortOamX[128], gPortOamY[128];
extern u16 gPortOamTag[128];
u16 PortMapGetBlockWide(int x, int y);
void LZ77UnCompWram(const void *src, void *dest);

static u32 sStage[H][W];
static int sState;      /* 0 none, 1 built (in battle) */
static int sSky;        /* 0 sky, 1 dark */
static int sWeather;
static u32 sFrame;

/* ---- tileset access ------------------------------------------------- */

static u8 sTiles[2][512 * 32];
static u32 sPal[16][16];
static u32 *sMeta[1024][2];   /* 16x16 ABGR, [0] whole block, [1] card */
static u16 sBase;             /* ground used under solid blocks */
static u32 sBaseImg[256];

static u32 Abgr(u16 c)
{
    u32 r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void LoadTileset(const struct Tileset *t, int which)
{
    if (!t)
        return;
    if (t->isCompressed)
        LZ77UnCompWram(t->tiles, sTiles[which]);
    else
        memcpy(sTiles[which], t->tiles, sizeof(sTiles[which]));
}

static void DrawTile(u32 *dst, u16 e, int opaque)
{
    int tid = e & 0x3FF, pal = e >> 12, x, y;
    const u8 *t = tid < 512 ? sTiles[0] + tid * 32 : sTiles[1] + (tid - 512) * 32;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int sx = (e & 0x400) ? 7 - x : x, sy = (e & 0x800) ? 7 - y : y;
            u8 b = t[sy * 4 + sx / 2];
            u8 ci = (sx & 1) ? b >> 4 : b & 15;
            if (ci)
                dst[y * 16 + x] = sPal[pal][ci];
            else if (opaque && !(dst[y * 16 + x] >> 24))
                dst[y * 16 + x] = sPal[0][0];
        }
}

static const u16 *MetatileEntries(u16 id)
{
    const struct MapLayout *l = gMapHeader.mapLayout;
    if (id < NUM_METATILES_IN_PRIMARY)
        return l->primaryTileset->metatiles + id * 8;
    return l->secondaryTileset->metatiles + (id - NUM_METATILES_IN_PRIMARY) * 8;
}

/* kind 0: the whole block, opaque; 1: a card (the ground taken out) */
static const u32 *Meta(u16 id, int kind)
{
    static const int ofs[4] = {0, 8, 128, 136};
    u32 *img;
    int i, k;
    if (id >= 1024)
        id = 0;
    if (sMeta[id][kind])
        return sMeta[id][kind];
    img = calloc(256, 4);
    if (!img)
        return sBaseImg;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 4; i++)
            DrawTile(img + ofs[i], MetatileEntries(id)[k * 4 + i], kind == 0 && k == 0);
    if (kind == 1) /* the ground in the card is not part of the thing */
        for (i = 0; i < 256; i++)
            if (img[i] == sBaseImg[i])
                img[i] = 0;
    sMeta[id][kind] = img;
    return img;
}

static void FreeMeta(void)
{
    int i;
    for (i = 0; i < 1024; i++) {
        free(sMeta[i][0]); free(sMeta[i][1]);
        sMeta[i][0] = sMeta[i][1] = NULL;
    }
}

static int Solid(u16 block)
{
    return (block & MAPGRID_COLLISION_MASK) != 0;
}

/* ---- building the stage ---------------------------------------------- */

static u32 Mix(u32 a, u32 b, int t) /* t/256 of b */
{
    u32 ra = a & 255, ga = (a >> 8) & 255, ba = (a >> 16) & 255;
    u32 rb = b & 255, gb = (b >> 8) & 255, bb = (b >> 16) & 255;
    return 0xFF000000u | (((ba * (256 - t) + bb * t) >> 8) << 16) | (((ga * (256 - t) + gb * t) >> 8) << 8) |
           ((ra * (256 - t) + rb * t) >> 8);
}

static float sCamX, sCamY, sFx, sFy, sRx, sRy; /* camera, forward, right */

static u32 GroundAt(float wx, float wy)
{
    int ix = (int)wx, iy = (int)wy;
    int bx = ix >> 4, by = iy >> 4;
    u16 b = PortMapGetBlockWide(bx, by);
    const u32 *img;
    if (b == MAPGRID_UNDEFINED || Solid(b))
        img = Meta(sBase, 0);
    else
        img = Meta(b & MAPGRID_METATILE_ID_MASK, 0);
    return img[(iy & 15) * 16 + (ix & 15)];
}

static u32 SkyAt(int y)
{
    int t = y * 256 / HOR;
    if (sSky)
        return Mix(0xFF080808u, 0xFF283038u, t);
    return Mix(0xFFF09658u, 0xFFFFECD2u, t); /* ABGR: blue to pale */
}

static u32 HazeColor(void)
{
    return sSky ? 0xFF181818u : 0xFFFFECD2u;
}

static void DrawCard(const u32 *img, int w, int h, float sx, float sy, float s, int haze)
{
    int dw = (int)(w * s), dh = (int)(h * s), x, y;
    int x0 = (int)(sx - dw / 2.0f), y0 = (int)(sy - dh);
    if (dw < 1 || dh < 1 || dw > 2000)
        return;
    for (y = 0; y < dh; y++) {
        int yy = y0 + y, iy = y * h / dh;
        if (yy < 0 || yy >= H)
            continue;
        for (x = 0; x < dw; x++) {
            int xx = x0 + x;
            u32 c;
            if (xx < 0 || xx >= W)
                continue;
            c = img[iy * w + x * w / dw];
            if (c >> 24)
                sStage[yy][xx] = haze ? Mix(c, HazeColor(), haze) : c;
        }
    }
}

static void Shadow(int cx, int cy, int rx, int ry)
{
    int x, y;
    for (y = -ry; y <= ry; y++)
        for (x = -rx; x <= rx; x++) {
            float d = (float)(x * x) / (rx * rx) + (float)(y * y) / (ry * ry);
            int yy = cy + y, xx = cx + x;
            if (d >= 1.0f || yy < 0 || yy >= H || xx < 0 || xx >= W)
                continue;
            sStage[yy][xx] = Mix(sStage[yy][xx], 0xFF102010u, (int)(110 * (1.0f - d)));
        }
}

static void Soften(void)
{
    static u32 tmp[H][W];
    int x, y;
    memcpy(tmp, sStage, sizeof(tmp));
    for (y = 1; y < H - 1; y++)
        for (x = 1; x < W - 1; x++) {
            u32 r = 0, g = 0, b = 0;
            int dx, dy;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    u32 c = tmp[y + dy][x + dx];
                    int wgt = (dx == 0 && dy == 0) ? 4 : (dx == 0 || dy == 0) ? 2 : 1;
                    r += (c & 255) * wgt; g += ((c >> 8) & 255) * wgt; b += ((c >> 16) & 255) * wgt;
                }
            sStage[y][x] = 0xFF000000u | ((b >> 4) << 16) | ((g >> 4) << 8) | (r >> 4);
        }
}

struct Card { float d, sx, sy, s; u16 id[4]; u8 w, h; };

static int CmpCard(const void *a, const void *b)
{
    float da = ((const struct Card *)a)->d, db = ((const struct Card *)b)->d;
    return da < db ? 1 : da > db ? -1 : 0;
}

static void BakeWeather(void);

static void Build(void)
{
    const struct MapLayout *l = gMapHeader.mapLayout;
    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    static struct Card cards[1500];
    static u8 used[96][96];
    int n = 0, x, y, i, px = pl->currentCoords.x, py = pl->currentCoords.y;
    int counts[1024] = {0}, best = 0;

    sSky = gMapHeader.mapType == MAP_TYPE_UNDERGROUND || gMapHeader.mapType == MAP_TYPE_INDOOR ||
           gMapHeader.mapType == MAP_TYPE_SECRET_BASE;
    sWeather = GetCurrentWeather();
#ifndef __vita__
    if (getenv("PKR_STAGE_WEATHER"))
        sWeather = atoi(getenv("PKR_STAGE_WEATHER"));
#endif
    FreeMeta();
    LoadTileset(l->primaryTileset, 0);
    LoadTileset(l->secondaryTileset, 1);
    for (i = 0; i < 16; i++) {
        const u16 *p = (i < NUM_PALS_IN_PRIMARY ? l->primaryTileset : l->secondaryTileset)->palettes[i];
        int c;
        for (c = 0; c < 16; c++)
            sPal[i][c] = Abgr(p[c]);
    }
    /* the ground under solid things: the most common walkable block nearby */
    for (y = -10; y <= 10; y++)
        for (x = -10; x <= 10; x++) {
            u16 b = PortMapGetBlockWide(px + x, py + y);
            if (b != MAPGRID_UNDEFINED && !Solid(b))
                counts[b & MAPGRID_METATILE_ID_MASK]++;
        }
    for (i = 0; i < 1024; i++)
        if (counts[i] > counts[best])
            best = i;
    sBase = (u16)best;
    memset(sBaseImg, 0, sizeof(sBaseImg));
    memcpy(sBaseImg, Meta(sBase, 0), sizeof(sBaseImg));

    /* the camera behind the player, looking where the player looks */
    switch (pl->facingDirection) {
    case DIR_NORTH: sFx = 0; sFy = -1; break;
    case DIR_WEST: sFx = -1; sFy = 0; break;
    case DIR_EAST: sFx = 1; sFy = 0; break;
    default: sFx = 0; sFy = 1; break;
    }
    sRx = -sFy; sRy = sFx;
    sCamX = px * 16 + 8 - sFx * 24;
    sCamY = py * 16 + 8 - sFy * 24;

    /* sky and ground */
    for (y = 0; y < H; y++) {
        if (y <= HOR) {
            for (x = 0; x < W; x++)
                sStage[y][x] = SkyAt(y);
            continue;
        }
        {
            float d = CAM_H * FOC / (y - HOR);
            int hz = d < 80 ? 0 : d > 780 ? 150 : (int)((d - 80) * 150 / 700);
            for (x = 0; x < W; x++) {
                float lat = (x - W / 2) * d / FOC;
                u32 c = GroundAt(sCamX + sFx * d + sRx * lat, sCamY + sFy * d + sRy * lat);
                sStage[y][x] = hz ? Mix(c, HazeColor(), hz) : c;
            }
        }
    }

    /* a far tree line on the horizon (outside) */
    if (!sSky) {
        u16 tree[4] = {0, 0, 0, 0};
        int found = 0;
        for (y = -12; y <= 12 && !found; y++)
            for (x = -12; x <= 12 && !found; x++) {
                u16 a = PortMapGetBlockWide(px + x, py + y), b = PortMapGetBlockWide(px + x + 1, py + y);
                u16 c = PortMapGetBlockWide(px + x, py + y + 1), e = PortMapGetBlockWide(px + x + 1, py + y + 1);
                if (Solid(a) && Solid(b) && Solid(c) && Solid(e) && a != MAPGRID_UNDEFINED &&
                    (a & 0x3FF) != (b & 0x3FF)) {
                    tree[0] = a & 0x3FF; tree[1] = b & 0x3FF; tree[2] = c & 0x3FF; tree[3] = e & 0x3FF;
                    found = 1;
                }
            }
        if (found) {
            static u32 img[32 * 32];
            int k;
            for (k = 0; k < 4; k++) {
                const u32 *m = Meta(tree[k], 1);
                for (y = 0; y < 16; y++)
                    memcpy(img + ((k / 2) * 16 + y) * 32 + (k & 1) * 16, m + y * 16, 64);
            }
            for (k = 0; k < 2; k++) {
                float dist = k ? 450.0f : 700.0f, s = FOC / dist;
                int step = (int)(32 * s * 0.8f) + 1;
                for (x = -step + k * step / 2; x < W + step; x += step)
                    DrawCard(img, 32, 32, (float)x, HOR + CAM_H * s, s, k ? 90 : 130);
            }
        }
    }

    /* solid blocks ahead as upright cards (2x2 groups like trees) */
    memset(used, 0, sizeof(used));
    for (y = -40; y < 40; y++)
        for (x = -40; x < 40; x++) {
            int bx = px + x, by = py + y, w = 1, h = 1, k;
            u16 b = PortMapGetBlockWide(bx, by);
            float cx, cy, rx, ry, d, lat;
            if (used[y + 48][x + 48] || b == MAPGRID_UNDEFINED || !Solid(b))
                continue;
            if (x + 1 < 40 && !used[y + 48][x + 49] && Solid(PortMapGetBlockWide(bx + 1, by)))
                w = 2;
            if (y + 1 < 40 && Solid(PortMapGetBlockWide(bx, by + 1)) &&
                (w == 1 || Solid(PortMapGetBlockWide(bx + 1, by + 1))))
                h = 2;
            for (k = 0; k < w * h; k++)
                used[y + 48 + k / w][x + 48 + k % w] = 1;
            /* foot of the card: bottom middle, seen from the camera */
            cx = bx * 16 + w * 8; cy = by * 16 + h * 16;
            rx = cx - sCamX; ry = cy - sCamY;
            /* the card faces the camera: its foot is the edge nearest to it */
            d = rx * sFx + ry * sFy;
            lat = rx * sRx + ry * sRy;
            if (sFy < 0) d -= 0; else if (sFy > 0) d -= h * 16 - 2; else d -= 8;
            if (d < 40.0f)
                continue;
            /* the clearing where the Pokemon stand */
            if (d < 170.0f && lat > -110.0f && lat < 120.0f)
                continue;
            if (n >= (int)(sizeof(cards) / sizeof(cards[0])))
                continue;
            cards[n].d = d;
            cards[n].s = FOC / d;
            cards[n].sx = W / 2 + lat * cards[n].s;
            cards[n].sy = HOR + CAM_H * cards[n].s;
            cards[n].w = (u8)w; cards[n].h = (u8)h;
            for (k = 0; k < 4; k++)
                cards[n].id[k] = PortMapGetBlockWide(bx + (k & 1) * (w - 1), by + (k >> 1) * (h - 1)) & 0x3FF;
            if (w == 1 && h == 2) { cards[n].id[1] = cards[n].id[2]; }
            n++;
        }
    qsort(cards, n, sizeof(cards[0]), CmpCard);
    for (i = 0; i < n; i++) {
        static u32 img[32 * 32];
        int w = cards[i].w, h = cards[i].h, k, hz;
        if (cards[i].sx + w * 8 * cards[i].s < 0 || cards[i].sx - w * 8 * cards[i].s > W)
            continue;
        for (k = 0; k < w * h; k++) {
            const u32 *m = Meta(cards[i].id[w == 1 && h == 2 ? (k ? 2 : 0) : k], 1);
            for (y = 0; y < 16; y++)
                memcpy(img + ((k / w) * 16 + y) * (w * 16) + (k % w) * 16, m + y * 16, 64);
        }
        hz = cards[i].d < 80 ? 0 : cards[i].d > 780 ? 150 : (int)((cards[i].d - 80) * 150 / 700);
        DrawCard(img, w * 16, h * 16, cards[i].sx, cards[i].sy, cards[i].s, hz);
    }
    Soften();
    /* shadows where the Pokemon stand (GBA: foe around (176,72), player (72,112)) */
    Shadow(SX(176), SY(74), 50, 12);
    Shadow(SX(72), SY(110), 70, 14);
    BakeWeather();
    sState = 1;
}

/* ---- every battle frame ---------------------------------------------- */

void CB2_InitBattleInternal(void);
void BattleMainCB2(void);
extern int gPortBattleIntro;   /* src/battle/anim/battle_intro.c */
extern int gPortViewBattle;    /* port_view.c: battle window layout */

static u32 sStageW[H][W];      /* the stage with the weather tint */

static int StageWanted(void)
{
    return gPortConfig.viewMode == PORT_VIEW_FULL && gMain.inBattle && gMapHeader.mapLayout &&
           gMapHeader.mapType != MAP_TYPE_UNDERWATER;
}

int PortStage_BeginFrame(void)
{
    /* the battle itself (not its bag, party or summary screens) */
    if (!StageWanted() || (gMain.callback2 != BattleMainCB2 && gMain.callback2 != CB2_InitBattleInternal)) {
        if (!StageWanted())
            sState = 0;
        virtuappu_mode1_backdrop_key = 0;
        return 0;
    }
    if (!sState)
        Build();
    sFrame++;
    return 1;
}

static int IsHud(int i)
{
    return gPortOamTag[i] >= 55039 && gPortOamTag[i] <= 55060;
}

static void ClearKnobs(void)
{
    int b, i;
    for (b = 0; b < 4; b++) {
        virtuappu_mode1_bg_screen_override[b] = 0;
        virtuappu_mode1_bg_dx[b] = virtuappu_mode1_bg_dy[b] = 0;
        virtuappu_mode1_bg_region_count[b] = 0;
        virtuappu_mode1_bg_clip_on[b] = 1;     /* off unless opened below */
        virtuappu_mode1_bg_clip_x0[b] = virtuappu_mode1_bg_clip_x1[b] = 0;
        virtuappu_mode1_bg_clip_y0[b] = virtuappu_mode1_bg_clip_y1[b] = 0;
    }
    virtuappu_mode1_backdrop_key = KEY;
    virtuappu_mode1_win_dx = virtuappu_mode1_win_dy = 0;
    virtuappu_mode1_line_dy = 0;
    virtuappu_mode1_obj_port = 1;
    for (i = 0; i < 128; i++) {
        virtuappu_mode1_obj_base_x[i] = gPortOamX[i];
        virtuappu_mode1_obj_base_y[i] = gPortOamY[i];
        virtuappu_mode1_obj_dx[i] = 0;
        virtuappu_mode1_obj_dy[i] = 0;
    }
}

static u8 sUiObj[128];
static s16 sUiDx[128], sUiDy[128];
static virtuappu_mode1_region sReg[VIRTUAPPU_MODE1_MAX_REGIONS];
static int sRegCount;

/* Where the text windows (BG0 regions) and the sprites of the windows and
 * the healthboxes go in the wide frame; they are drawn GBA sized (UI pass)
 * and copied there. */
static void PlanUi(void)
{
    int i;
    ClearKnobs();
    gPortViewBattle = 1;
    PortView_PlaceWindows();
    gPortViewBattle = 0;
    sRegCount = virtuappu_mode1_bg_region_count[0];
    memcpy(sReg, virtuappu_mode1_bg_regions[0], sizeof(sReg[0]) * sRegCount);
    for (i = 0; i < 128; i++) {
        sUiObj[i] = 0;
        if (IsHud(i)) {
            /* the foe's healthbox top left, ours bottom right over the
             * text windows */
            sUiObj[i] = 1;
            if (gPortOamX[i] < PORT_GBA_W / 2) {
                sUiDx[i] = 8 - 13;
                sUiDy[i] = 8 - 16;
            } else {
                sUiDx[i] = (s16)(W - 8 - 230);
                sUiDy[i] = (s16)(SY(112) - 6 - 110);
            }
        } else if (PortView_SpriteInWindow(i)) {
            sUiObj[i] = 1;
            sUiDx[i] = virtuappu_mode1_obj_dx[i];
            sUiDy[i] = virtuappu_mode1_obj_dy[i];
        }
    }
}

/* Pass 1: the field (BG1/BG2 and the sprites but the healthboxes and the
 * sprites of the text windows), GBA sized but 320 wide. */
void PortStage_FieldPass(int *w, int *h)
{
    int b, i;
    PlanUi();
    ClearKnobs();
    for (b = 1; b <= 2; b++) {
        virtuappu_mode1_bg_dx[b] = -FX0 & 511;
        virtuappu_mode1_bg_clip_y1[b] = FH;
        /* the GBA picture; the intro scenery (a repeating band) as wide
         * as the screen */
        virtuappu_mode1_bg_clip_x0[b] = gPortBattleIntro ? 0 : FX0;
        virtuappu_mode1_bg_clip_x1[b] = gPortBattleIntro ? FW : FX0 + PORT_GBA_W;
    }
    virtuappu_mode1_win_dx = FX0;
    for (i = 0; i < 128; i++) {
        virtuappu_mode1_obj_dx[i] = FX0;
        if (sUiObj[i])
            virtuappu_mode1_obj_dy[i] = 4000;
    }
    *w = FW;
    *h = FH;
}

/* Pass 2: BG0 and the window/healthbox sprites, GBA sized (240x160). */
void PortStage_UiPass(void)
{
    int i;
    ClearKnobs();
    virtuappu_mode1_bg_clip_on[0] = 0;
    for (i = 0; i < 128; i++)
        if (!sUiObj[i])
            virtuappu_mode1_obj_dy[i] = 4000;
}

/* copy a GBA sized rect of the UI picture to (x0+dx.., y0+dy..) */
static void Blit(u32 *fb, int pitch, const u32 *ui, int upitch, int x0, int y0, int x1, int y1, int dx, int dy)
{
    const u32 key = KEY & 0x00FFFFFFu;
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PORT_GBA_W) x1 = PORT_GBA_W;
    if (y1 > PORT_GBA_H) y1 = PORT_GBA_H;
    if (x0 + dx < 0) x0 = -dx;
    if (y0 + dy < 0) y0 = -dy;
    if (x1 + dx > W) x1 = W - dx;
    if (y1 + dy > H) y1 = H - dy;
    for (y = y0; y < y1; y++) {
        const u32 *s = ui + y * upitch;
        u32 *d = fb + (y + dy) * pitch + dx;
        for (x = x0; x < x1; x++)
            if ((s[x] & 0xFFFFFFu) != key)
                d[x] = s[x];
    }
}

static void ObjBox(int i, int *x, int *y, int *w, int *h)
{
    extern u8 gHostOam[];
    static const u8 sz[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    u16 a0 = gHostOam[i * 8] | (gHostOam[i * 8 + 1] << 8), a1 = gHostOam[i * 8 + 2] | (gHostOam[i * 8 + 3] << 8);
    int shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
    if ((a0 & 0x300) == 0x200 || shape == 3) {
        *w = *h = 0;
        return;
    }
    *w = sz[shape][size][0];
    *h = sz[shape][size][1];
    if ((a0 & 0x300) == 0x300) {
        *w *= 2;
        *h *= 2;
    }
    *y = a0 & 255;
    if (*y >= 160) *y -= 256;
    *x = a1 & 511;
    if (*x >= 240) *x -= 512;
}

/* how far the screen is faded (the game fades the palettes) */
static int FadeAmount(int *white)
{
    u16 c = gPaletteFade.blendColor;
    if (!(gPaletteFade.multipurpose1 & 1))   /* backdrop palette not faded */
        return 0;
    *white = (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31) > 45;
    return gPaletteFade.y * 16;
}

static u32 Rnd(u32 *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 8;
}

/* the weather's tint, once per battle */
static void BakeWeather(void)
{
    u32 tint = 0;
    int t = 0, x, y;
    switch (sWeather) {
    case WEATHER_RAIN_LIGHT: case WEATHER_RAIN_MED: tint = 0xFF706860u; t = 70; break;
    case WEATHER_RAIN_HEAVY: tint = 0xFF706860u; t = 110; break;
    case WEATHER_SANDSTORM: tint = 0xFF60A8D8u; t = 70; break;
    case WEATHER_ASH: tint = 0xFF808890u; t = 60; break;
    case WEATHER_FOG_1: case WEATHER_FOG_2: case WEATHER_FOG_3: tint = 0xFFE8E8E8u; t = 120; break;
    case WEATHER_DROUGHT: tint = 0xFF80E0FFu; t = 60; break;
    case WEATHER_SUNNY: tint = 0xFF80E0FFu; t = 25; break;
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            sStageW[y][x] = t ? Mix(sStage[y][x], tint, t) : sStage[y][x];
}

/* moving weather (rain, sand, ash, snow) on the stage pixels (mask) */
static void Particles(u32 *fb, int pitch, const u8 *bg)
{
    u32 seed = 1;
    int i, n, x, y;
    switch (sWeather) {
    case WEATHER_RAIN_LIGHT:
    case WEATHER_RAIN_MED:
    case WEATHER_RAIN_HEAVY:
        n = sWeather == WEATHER_RAIN_HEAVY ? 260 : sWeather == WEATHER_RAIN_MED ? 160 : 90;
        for (i = 0; i < n; i++) {
            int x0 = (int)(Rnd(&seed) % (W + 80)), sp = 9 + (int)(Rnd(&seed) % 5);
            int y0 = (int)((Rnd(&seed) + sFrame * sp) % (H + 40)) - 20;
            int k;
            x0 = (x0 - (int)(sFrame * sp / 3)) % (W + 80);
            if (x0 < 0) x0 += W + 80;
            for (k = 0; k < 10; k++) {
                x = x0 - 40 - k / 3; y = y0 + k;
                if (x >= 0 && x < W && y >= 0 && y < H && bg[y * W + x])
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], 0xFFF8E0C8u, 140);
            }
        }
        break;
    case WEATHER_SANDSTORM:
        for (i = 0; i < 420; i++) {
            int sp = 4 + (int)(Rnd(&seed) % 6);
            x = (int)((Rnd(&seed) + sFrame * sp) % W);
            y = (int)((Rnd(&seed) + (sFrame * sp) / 5) % H);
            if (bg[y * W + x])
                fb[y * pitch + x] = 0xFF4890D0u;
            if (x + 1 < W && bg[y * W + x + 1])
                fb[y * pitch + x + 1] = 0xFF70B0E0u;
        }
        break;
    case WEATHER_ASH:
    case WEATHER_SNOW:
        for (i = 0; i < 220; i++) {
            int sp = 1 + (int)(Rnd(&seed) % 2), ph = (int)(Rnd(&seed) % 64);
            u32 col = sWeather == WEATHER_ASH ? 0xFF707070u : 0xFFF8F8F8u;
            x = (int)((Rnd(&seed) + (sFrame + ph) / 3) % W);
            y = (int)((Rnd(&seed) + sFrame * sp / 2) % H);
            x = (x + ((((sFrame + ph) >> 4) & 1) ? 1 : 0)) % W;
            if (bg[y * W + x])
                fb[y * pitch + x] = col;
            if (x + 1 < W && y + 1 < H && bg[(y + 1) * W + x + 1])
                fb[(y + 1) * pitch + x + 1] = col;
        }
        break;
    }
}

/* average of two pixels; a key pixel takes the other one */
static inline u32 Avg(u32 a, u32 b)
{
    if (!(a >> 24))
        return b;
    if (!(b >> 24))
        return a;
    return 0xFF000000u | (((a & 0xFEFEFEu) >> 1) + ((b & 0xFEFEFEu) >> 1));
}

/* source pixels of an output pixel at 1.5x: a, between a and b, b */
static void Taps(int o, int *s0, int *s1)
{
    int k = (o / 3) * 2;
    switch (o % 3) {
    case 0: *s0 = *s1 = k; break;
    case 1: *s0 = k; *s1 = k + 1; break;
    default: *s0 = *s1 = k + 1; break;
    }
}

/* the frame: text windows and healthboxes (fb, keyed elsewhere) over the
 * field (1.5x) over the stage */
void PortStage_Compose(u32 *fb, int pitch, const u32 *field, int fpitch, const u32 *ui, int upitch)
{
    static u8 bg[H * W];
    static u32 row[FH][W];
    static short tx0[W], tx1[W];
    static int init;
    int x, y, white = 0, fade = FadeAmount(&white);
    u32 tint = white ? 0xFFFFFFFFu : 0xFF000000u;
    const u32 key = KEY & 0x00FFFFFFu;

    if (!init) {
        int a, b;
        for (x = 0; x < W; x++) {
            Taps(x, &a, &b);
            tx0[x] = (short)a;
            tx1[x] = (short)b;
        }
        init = 1;
    }
    /* field rows scaled across (key pixels get alpha 0) */
    for (y = 0; y < FH && SY(y) < H; y++) {
        const u32 *s = field + y * fpitch;
        u32 *d = row[y];
        for (x = 0; x < W; x++) {
            u32 a = s[tx0[x]], b = s[tx1[x]];
            a = (a & 0xFFFFFFu) == key ? 0 : a | 0xFF000000u;
            if (tx1[x] != tx0[x]) {
                b = (b & 0xFFFFFFu) == key ? 0 : b | 0xFF000000u;
                a = Avg(a, b);
            }
            d[x] = a;
        }
    }
    for (y = 0; y < H; y++) {
        u32 *p = fb + y * pitch;
        const u32 *st = sStageW[y];
        u8 *m = bg + y * W;
        const u32 *r0 = NULL, *r1 = NULL;
        if (y >= FY) {
            int a, b;
            Taps(y - FY, &a, &b);
            if (b < FH) {
                r0 = row[a];
                r1 = row[b];
            }
        }
        for (x = 0; x < W; x++) {
            u32 c = 0;
            if (r0)
                c = r0 == r1 ? r0[x] : Avg(r0[x], r1[x]);
            if (c >> 24) {
                p[x] = c;
                m[x] = 0;
            } else {
                p[x] = st[x];
                m[x] = 1;
            }
        }
    }
    Particles(fb, pitch, bg);
    if (fade > 0) {
        if (fade > 256)
            fade = 256;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (bg[y * W + x])
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], tint, fade);
    }
    /* the text windows and the sprites on them, the healthboxes */
    {
        int i;
        for (i = 0; i < sRegCount; i++) {
            const virtuappu_mode1_region *g = &sReg[i];
            Blit(fb, pitch, ui, upitch, g->x0 - g->dx, g->y0 - g->dy, g->x1 - g->dx, g->y1 - g->dy, g->dx, g->dy);
        }
        for (i = 0; i < 128; i++) {
            int ox, oy, ow, oh;
            if (!sUiObj[i])
                continue;
            ObjBox(i, &ox, &oy, &ow, &oh);
            if (ow)
                Blit(fb, pitch, ui, upitch, ox, oy, ox + ow, oy + oh, sUiDx[i], sUiDy[i]);
        }
    }
}
