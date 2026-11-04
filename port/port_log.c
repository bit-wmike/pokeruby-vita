/* Detailed log (0.3.18): every line with time since start and frame number,
 * events polled once per frame (screen/callback changes, map changes with
 * name and weather, battle start/end with opponents and outcome, button
 * changes) and per-pass render times for the performance lines.
 *
 * Vita: always on, written by the platform through a RAM buffer (no card
 * access on the game thread). Host: only with PKR_LOG=1, so the test
 * scripts' output stays as it was. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "global.h"
#include "main.h"
#include "battle.h"
#include "pokemon.h"
#include "sprite.h"
#include "event_object_movement.h"
#include "region_map.h"
#include "field_weather.h"
#include "port/port.h"

extern u8 gBattleOutcome;

uint32_t gPortPassUs[PORT_PASS_COUNT];   /* summed since the last Port_LogTakePasses */
uint32_t gPortPassN[PORT_PASS_COUNT];
uint32_t gPortPassLast[PORT_PASS_COUNT]; /* this frame */

static uint64_t sStartUs;
static int sEnabled = -1;

int Port_LogEnabled(void)
{
    if (sEnabled < 0) {
#ifdef __vita__
        sEnabled = 1;
#else
        sEnabled = getenv("PKR_LOG") != NULL;
#endif
    }
    return sEnabled;
}

int Port_LogPrefix(char *buf, int n)
{
    uint64_t t;
    if (!sStartUs)
        sStartUs = Platform_TimeUs();
    t = Platform_TimeUs() - sStartUs;
    return snprintf(buf, n, "[%5u:%02u.%03u f%u] ", (unsigned)(t / 60000000u), (unsigned)(t / 1000000u % 60),
                    (unsigned)(t / 1000u % 1000), Port_FrameCounter());
}

/* game text (German charmap) to ASCII/Latin, for map and species names */
static void GameToAscii(char *dst, int n, const u8 *src)
{
    int i = 0;
    for (; *src != 0xFF && i < n - 3; src++) {
        u8 c = *src;
        const char *s = NULL;
        char ch = 0;
        if (c >= 0xBB && c <= 0xD4) ch = (char)('A' + c - 0xBB);
        else if (c >= 0xD5 && c <= 0xEE) ch = (char)('a' + c - 0xD5);
        else if (c >= 0xA1 && c <= 0xAA) ch = (char)('0' + c - 0xA1);
        else if (c == 0x00) ch = ' ';
        else if (c == 0xAD) ch = '.';
        else if (c == 0xAE) ch = '-';
        else if (c == 0xF1) s = "Ae";
        else if (c == 0xF2) s = "Oe";
        else if (c == 0xF3) s = "Ue";
        else if (c == 0xF4) s = "ae";
        else if (c == 0xF5) s = "oe";
        else if (c == 0xF6) s = "ue";
        else if (c == 0x15) s = "ss";
        else if (c == 0x1B) ch = 'e';
        else ch = '?';
        if (s) {
            dst[i++] = s[0];
            dst[i++] = s[1];
        } else {
            dst[i++] = ch;
        }
    }
    dst[i] = 0;
}

void Port_LogCounts(int *npc, int *sprites)
{
    int i, n = 0, s = 0;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        n += gObjectEvents[i].active != 0;
    for (i = 0; i < MAX_SPRITES; i++)
        s += gSprites[i].inUse != 0;
    *npc = n;
    *sprites = s;
}

static void LogParty(const char *who, struct Pokemon *party, int count)
{
    char line[256], name[32];
    u8 raw[16];
    int i, o = 0;
    o += snprintf(line + o, sizeof(line) - o, "[battle] %s:", who);
    for (i = 0; i < count && i < PARTY_SIZE; i++) {
        u16 sp = (u16)GetMonData(&party[i], MON_DATA_SPECIES);
        if (sp == 0 || sp > 450)
            continue;
        GetSpeciesName(raw, sp);
        GameToAscii(name, sizeof(name), raw);
        o += snprintf(line + o, sizeof(line) - o, " %s(#%u) Lv%u KP %u/%u;", name, sp,
                      (unsigned)GetMonData(&party[i], MON_DATA_LEVEL), (unsigned)GetMonData(&party[i], MON_DATA_HP),
                      (unsigned)GetMonData(&party[i], MON_DATA_MAX_HP));
        if (o >= (int)sizeof(line) - 40)
            break;
    }
    Platform_Log("%s\n", line);
}

void Port_LogFrame(void)
{
    static MainCallback lastCb2, lastCb1;
    static int lastGroup = -100, lastNum = -100, lastWeather = -1, lastBattle, lastKeys = -1;
    static uint32_t battleStart;
    if (!Port_LogEnabled())
        return;

    if (gMain.callback2 != lastCb2 || gMain.callback1 != lastCb1) {
        Platform_Log("[scene] cb1 %p cb2 %p\n", (void *)gMain.callback1, (void *)gMain.callback2);
        lastCb2 = gMain.callback2;
        lastCb1 = gMain.callback1;
    }
    if (gSaveBlock1.location.mapGroup != lastGroup || gSaveBlock1.location.mapNum != lastNum) {
        char name[40];
        u8 raw[40];
        lastGroup = gSaveBlock1.location.mapGroup;
        lastNum = gSaveBlock1.location.mapNum;
        memset(raw, 0xFF, sizeof(raw));
        GetMapSectionName(raw, gMapHeader.regionMapSectionId, 20);
        raw[sizeof(raw) - 1] = 0xFF;
        GameToAscii(name, sizeof(name), raw);
        Platform_Log("[map] %d:%d \"%s\" pos %d,%d type %d\n", lastGroup, lastNum, name, gSaveBlock1.pos.x + 7,
                     gSaveBlock1.pos.y + 7, gMapHeader.mapType);
    }
    if (gWeatherPtr->currWeather != lastWeather) {
        lastWeather = gWeatherPtr->currWeather;
        Platform_Log("[weather] %d\n", lastWeather);
    }
    if (gMain.inBattle != lastBattle) {
        lastBattle = gMain.inBattle;
        if (lastBattle) {
            int n = 0;
            battleStart = Port_FrameCounter();
            Platform_Log("[battle] start type 0x%04x trainer %u%s\n", gBattleTypeFlags, gTrainerBattleOpponent,
                         (gBattleTypeFlags & 1) ? " (double)" : "");
            for (n = 0; n < PARTY_SIZE && GetMonData(&gEnemyParty[n], MON_DATA_SPECIES); n++)
                ;
            LogParty("foe", gEnemyParty, n);
            LogParty("own", gPlayerParty, gPlayerPartyCount);
        } else {
            Platform_Log("[battle] end outcome %u after %u frames\n", gBattleOutcome, Port_FrameCounter() - battleStart);
        }
    }
    if (gMain.heldKeysRaw != lastKeys) {
        static const char *const names[10] = {"A", "B", "SELECT", "START", "RECHTS", "LINKS", "OBEN", "UNTEN", "R", "L"};
        char k[64];
        int i, o = 0;
        lastKeys = gMain.heldKeysRaw;
        k[0] = 0;
        for (i = 0; i < 10; i++)
            if (lastKeys & (1 << i))
                o += snprintf(k + o, sizeof(k) - o, "%s ", names[i]);
        Platform_Log("[keys] %s\n", o ? k : "-");
    }
}
