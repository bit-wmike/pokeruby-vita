/* Settings of the host / PS Vita port, stored as settings.ini. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port_config.h"

struct PortConfig gPortConfig = {PORT_VIEW_FULL, PORT_STYLE_PIXEL, 0, 1, 0, 3,
    /* X: A, O: B, square: B (run), triangle: SELECT (registered item) */
    {PORT_KEY_A, PORT_KEY_B, PORT_KEY_B, PORT_KEY_SELECT, PORT_KEY_L, PORT_KEY_R, PORT_KEY_START, PORT_KEY_SELECT}};
const char *const gPortBtnNames[PORT_BTN_COUNT] = {
    "key_cross", "key_circle", "key_square", "key_triangle", "key_l", "key_r", "key_start", "key_select"};
struct PortCheats gPortCheats = {1, 0, 0, 0, 0};

static void Set(const char *key, int v)
{
    if (!strcmp(key, "wide_view") && v >= 0 && v <= 1)
        gPortConfig.viewMode = v;
    else if (!strcmp(key, "display_style") && v >= 0 && v <= 3)
        gPortConfig.displayStyle = v == PORT_STYLE_HQ ? PORT_STYLE_HQ : PORT_STYLE_PIXEL;
    else if (!strcmp(key, "turbo") && v >= 2 && v <= 4)
        gPortConfig.turbo = v;
    else if (!strcmp(key, "frame_rate_30"))
        gPortConfig.frameRate30 = v != 0;
    else if (!strcmp(key, "exp_multiplier") && (v == 1 || v == 2 || v == 4 || v == 8))
        gPortConfig.expMult = v;
    else if (!strcmp(key, "only_hoenn"))
        gPortConfig.gen3Wild = v != 0;
    else {
        int i;
        for (i = 0; i < PORT_BTN_COUNT; i++)
            if (!strcmp(key, gPortBtnNames[i]) && v >= 0 && v < PORT_KEY_COUNT)
                gPortConfig.keymap[i] = v;
    }
}

void Port_ConfigLoad(void)
{
    char buf[1024];
    long n = Platform_ReadFile("settings.ini", buf, sizeof(buf) - 1);
    char *line;
    const char *env = getenv("PKR_VIEW");
    if (n > 0) {
    buf[n] = 0;
    for (line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char key[64];
        int v;
        if (sscanf(line, " %63[a-z_0-9] = %d", key, &v) == 2)
            Set(key, v);
    }
    }
    if (getenv("PKR_CHEATS")) /* host build: encounters,shiny,random,flee,run */
        sscanf(getenv("PKR_CHEATS"), "%d,%d,%d,%d,%d", &gPortCheats.encounters, &gPortCheats.shiny,
               &gPortCheats.randomSpecies, &gPortCheats.alwaysFlee, &gPortCheats.runIndoors);
    if (getenv("PKR_EXP")) /* host build */
        gPortConfig.expMult = atoi(getenv("PKR_EXP"));
    if (getenv("PKR_HOENN")) /* host build */
        gPortConfig.gen3Wild = atoi(getenv("PKR_HOENN")) != 0;
    if (env) /* host build: PKR_VIEW=0/1 */
        gPortConfig.viewMode = atoi(env) ? PORT_VIEW_FULL : PORT_VIEW_GBA;
}

void Port_ConfigSave(void)
{
    char buf[512];
    int i, n = snprintf(buf, sizeof(buf),
                     "# Pokemon Vita port settings\n"
                     "wide_view = %d\n"
                     "display_style = %d\n"
                     "frame_rate_30 = %d\n"
                     "only_hoenn = %d\n"
                     "exp_multiplier = %d\n"
                     "turbo = %d\n",
                     gPortConfig.viewMode, gPortConfig.displayStyle, gPortConfig.frameRate30, gPortConfig.gen3Wild,
                     gPortConfig.expMult, gPortConfig.turbo);
    /* buttons: 0 none, 1 A, 2 B, 3 START, 4 SELECT, 5 L, 6 R */
    for (i = 0; i < PORT_BTN_COUNT; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s = %d\n", gPortBtnNames[i], gPortConfig.keymap[i]);
    Platform_WriteFile("settings.ini", buf, (unsigned long)n);
}

int PortGen3Wild(void)
{
    return gPortConfig.gen3Wild;
}

int PortExpMultiplier(void)
{
    return gPortConfig.expMult >= 1 ? gPortConfig.expMult : 1;
}

/* ---- cheats, asked by the game (src/wild_encounter.c, battle_main.c,
 * bike.c) ---- */
int PortCheatEncounters(void)
{
    return gPortCheats.encounters;
}

int PortCheatShiny(void)
{
    return gPortCheats.shiny;
}

int PortCheatFlee(void)
{
    return gPortCheats.alwaysFlee;
}

int PortCheatRunIndoors(void)
{
    return gPortCheats.runIndoors;
}

/* any Pokemon (internal numbers: 1-251 Kanto/Johto, 277-411 Hoenn), with
 * "Nur Hoenn" only Hoenn ones */
unsigned short PortCheatSpecies(unsigned short species, unsigned rnd)
{
    if (!gPortCheats.randomSpecies)
        return species;
    if (gPortConfig.gen3Wild)
        return (unsigned short)(277 + rnd % 135);
    rnd %= 251 + 135;
    return (unsigned short)(rnd < 251 ? 1 + rnd : 277 + (rnd - 251));
}
