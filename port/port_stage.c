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
#define OX 120          /* GBA battle picture in the frame */
#define OY 88
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
    Shadow(OX + 176, OY + 74, 34, 8);
    Shadow(OX + 72, OY + 112, 48, 10);
    sState = 1;
}

/* ---- every battle frame ---------------------------------------------- */

void CB2_InitBattleInternal(void);
void BattleMainCB2(void);

static int StageWanted(void)
{
    return gPortConfig.viewMode == PORT_VIEW_FULL && gMain.inBattle && gMapHeader.mapLayout &&
           gMapHeader.mapType != MAP_TYPE_UNDERWATER;
}

int PortStage_BeginFrame(void)
{
    int i, b;
    if (!StageWanted()) {
        sState = 0;
        return 0;
    }
    /* the battle itself (not its bag, party or summary screens) */
    if (gMain.callback2 != BattleMainCB2 && gMain.callback2 != CB2_InitBattleInternal)
        return 0;
    if (!sState)
        Build();
    sFrame++;

    for (b = 1; b <= 2; b++) {   /* BG1/2 (animations): the GBA picture */
        virtuappu_mode1_bg_dx[b] = -OX & 511;
        virtuappu_mode1_bg_dy[b] = -OY & 511;
        virtuappu_mode1_bg_clip_on[b] = 1;
        virtuappu_mode1_bg_clip_x0[b] = OX; virtuappu_mode1_bg_clip_x1[b] = OX + PORT_GBA_W;
        virtuappu_mode1_bg_clip_y0[b] = OY; virtuappu_mode1_bg_clip_y1[b] = OY + PORT_GBA_H;
    }
    virtuappu_mode1_bg_clip_on[3] = 1;   /* the GBA battle background: off */
    virtuappu_mode1_bg_clip_y0[3] = virtuappu_mode1_bg_clip_y1[3] = 0;
    PortView_PlaceWindows();             /* text boxes: bottom, full width */
    virtuappu_mode1_backdrop_key = KEY;
    virtuappu_mode1_win_dx = OX;
    virtuappu_mode1_win_dy = OY;
    virtuappu_mode1_line_dy = OY;
    virtuappu_mode1_obj_port = 1;
    for (i = 0; i < 128; i++) {
        u16 tag = gPortOamTag[i];
        virtuappu_mode1_obj_base_x[i] = gPortOamX[i];
        virtuappu_mode1_obj_base_y[i] = gPortOamY[i];
        virtuappu_mode1_obj_dx[i] = OX;
        virtuappu_mode1_obj_dy[i] = OY;
        if (tag >= 55039 && tag <= 55060) {
            /* healthboxes to the top corners: the foe's left, ours right */
            if (gPortOamX[i] < PORT_GBA_W / 2) {
                virtuappu_mode1_obj_dx[i] = 8 - 13;
                virtuappu_mode1_obj_dy[i] = 8 - 16;
            } else {
                virtuappu_mode1_obj_dx[i] = (s16)(W - 8 - 230);
                virtuappu_mode1_obj_dy[i] = (s16)(6 - 74);
            }
        } else {
            PortView_SpriteInWindow(i);
        }
    }
    return 1;
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

static void Weather(u32 *fb, int pitch, const u8 *bg)
{
    u32 seed = 1;
    int i, n, x, y;
    switch (sWeather) {
    case WEATHER_RAIN_LIGHT:
    case WEATHER_RAIN_MED:
    case WEATHER_RAIN_HEAVY:
        for (y = 0; y < H; y++)   /* a grey sky */
            for (x = 0; x < W; x++)
                if (bg[y * W + x])
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], 0xFF706860u, sWeather == WEATHER_RAIN_HEAVY ? 110 : 70);
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
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (bg[y * W + x])
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], 0xFF60A8D8u, 70);
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
        if (sWeather == WEATHER_ASH)
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++)
                    if (bg[y * W + x])
                        fb[y * pitch + x] = Mix(fb[y * pitch + x], 0xFF808890u, 60);
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
    case WEATHER_FOG_1:
    case WEATHER_FOG_2:
    case WEATHER_FOG_3:
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (bg[y * W + x]) {
                    int t = 90 + (int)(40 * (1 + ((x + y + (int)sFrame) & 63) / 64.0f)) / 2;
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], 0xFFE8E8E8u, t);
                }
        break;
    case WEATHER_DROUGHT:
    case WEATHER_SUNNY:
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (bg[y * W + x])
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], 0xFF80E0FFu, sWeather == WEATHER_DROUGHT ? 60 : 25);
        break;
    }
}

void PortStage_EndFrame(u32 *fb, int pitch)
{
    static u8 bg[H * W];
    int x, y, white = 0, fade = FadeAmount(&white);
    u32 tint = white ? 0xFFFFFFFFu : 0xFF000000u;

    virtuappu_mode1_backdrop_key = 0;
#ifndef __vita__
    if (getenv("PKR_STAGE_DEBUG") && sFrame % 100 == 0) {
        extern u8 gHostOam[];
        int k;
        fprintf(stderr, "[stage] fade=%d white=%d\n", fade, white);
        for (k = 0; k < 40; k++) {
            u16 a0 = gHostOam[k * 8] | (gHostOam[k * 8 + 1] << 8), a1 = gHostOam[k * 8 + 2] | (gHostOam[k * 8 + 3] << 8);
            if ((a0 & 0x300) == 0x200) continue;
            fprintf(stderr, "  oam%d y=%d x=%d base=%d,%d tag=%u\n", k, a0 & 255, a1 & 511, gPortOamX[k], gPortOamY[k], gPortOamTag[k]);
        }
    }
#endif
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            u32 *p = &fb[y * pitch + x];
            if ((*p & 0x00FFFFFFu) == (KEY & 0x00FFFFFFu)) {
                *p = sStage[y][x];
                bg[y * W + x] = 1;
            } else {
                bg[y * W + x] = 0;
            }
        }
    Weather(fb, pitch, bg);
    if (fade > 0)
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (bg[y * W + x])
                    fb[y * pitch + x] = Mix(fb[y * pitch + x], tint, fade > 256 ? 256 : fade);
}
