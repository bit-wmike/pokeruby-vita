/*
 * Host-only test helpers (not in the PS Vita build).
 *
 *   PKR_TEST_BATTLE=frame:species:level   at that frame (in the overworld)
 *       give the player a Mudkip if the party is empty and start a wild
 *       battle against species/level (numbers from constants/species.h).
 *   PKR_TEST_TRAINER=frame:trainer      a trainer battle (constants/opponents.h)
 *   PKR_TEST_WARP=frame:group:num:x:y   warp there (in the overworld)
 *   PKR_TEST_MOVES=first                in a battle: both sides use every move
 *       from `first` on (the first slot is replaced each turn), nobody
 *       faints - press A regularly (PKR_INPUT) to go through all animations.
 */
#ifndef __vita__
#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "pokemon.h"
#include "battle_setup.h"
#include "battle.h"
#include "palette.h"
#include "global.fieldmap.h"
#include "constants/species.h"

#include "port/port.h"

void CreateScriptedWildMon(u16 species, u8 level, u16 item);
int PortIsOverworld(void);

extern u16 gCurrentMove;
extern struct BattlePokemon gBattleMons[4];
void BattleMainCB2(void);

static void TestMoves(void)
{
    static int cur = -1, seen;
    (void)gCurrentMove;
    int b, k;
    if (cur < 0) {
        const char *s = getenv("PKR_TEST_MOVES");
        cur = s ? atoi(s) : 0;
        if (!cur)
            cur = 9999;
    }
    if (cur > 354 || gMain.callback2 != BattleMainCB2)
        return;
    /* moves that end the battle or faint the user */
    while (cur == 18 || cur == 46 || cur == 100 || cur == 120 || cur == 153 || cur == 195 || cur == 226 ||
           cur == 262 || cur == 194 || cur == 288)
        cur++;
    for (b = 0; b < 2; b++) {
        for (k = 0; k < 4; k++) {
            gBattleMons[b].moves[k] = (u16)cur;
            gBattleMons[b].pp[k] = 30;
        }
        gBattleMons[b].hp = gBattleMons[b].maxHP;
        gBattleMons[b].status1 = 0;
    }
    /* the next move each turn */
    if (seen != gBattleResults.battleTurnCounter) {
        if (seen)
            fprintf(stderr, "[test] move %d done at frame %u\n", cur, Port_FrameCounter()), cur++;
        seen = gBattleResults.battleTurnCounter;
    }
}

void Overworld_SetWarpDestination(s8 mapGroup, s8 mapNum, s8 warpId, s8 x, s8 y);
void sub_8080E88(void);
void ResetInitialPlayerAvatarState(void);

static void TestWarp(void)
{
    static int parsed, frame = -1, g, n, x, y;
    if (!parsed) {
        const char *s = getenv("PKR_TEST_WARP");
        parsed = 1;
        if (s)
            sscanf(s, "%d:%d:%d:%d:%d", &frame, &g, &n, &x, &y);
    }
    if (frame < 0 || (int)Port_FrameCounter() < frame || !PortIsOverworld())
        return;
    frame = -1;
    Overworld_SetWarpDestination(g, n, -1, x, y);
    sub_8080E88();
    ResetInitialPlayerAvatarState();
    fprintf(stderr, "[test] warp %d.%d (%d,%d) at frame %u\n", g, n, x, y, Port_FrameCounter());
}

u8 sub_809070C(u16 dexNum, u32 b, u32 c);

/* PKR_TEST_DEX=frame: the dex page of a caught Pokemon (in a battle) */
static void TestDex(void)
{
    static int frame = -2;
    if (frame == -2)
        frame = getenv("PKR_TEST_DEX") ? atoi(getenv("PKR_TEST_DEX")) : -1;
    if (frame < 0 || (int)Port_FrameCounter() != frame)
        return;
    BeginNormalPaletteFade(0xFFFFFFFF, 0, 0, 16, 0);
    sub_809070C(19, 0, 0);
    fprintf(stderr, "[test] dex page at frame %u\n", Port_FrameCounter());
}

void Port_StateRequest(int load);

void PortTest_Frame(void)
{
    {   /* PKR_STATE_SAVE=frame, PKR_STATE_LOAD=frame */
        const char *a = getenv("PKR_STATE_SAVE"), *b = getenv("PKR_STATE_LOAD");
        if (a && (int)Port_FrameCounter() == atoi(a))
            Port_StateRequest(0);
        if (b && (int)Port_FrameCounter() == atoi(b))
            Port_StateRequest(1);
    }
    TestDex();
    if (getenv("PKR_POS") && Port_FrameCounter() % 60 == 0 && PortIsOverworld())
        fprintf(stderr, "[pos] %u map %d.%d (%d,%d)\n", Port_FrameCounter(), gSaveBlock1.location.mapGroup,
                gSaveBlock1.location.mapNum, gSaveBlock1.pos.x, gSaveBlock1.pos.y);
    TestWarp();
    TestMoves();
    static int parsed, frame = -1, species, level, tframe = -1, trainer;
    if (!parsed) {
        const char *s = getenv("PKR_TEST_BATTLE");
        parsed = 1;
        if (s)
            sscanf(s, "%d:%d:%d", &frame, &species, &level);
        if ((s = getenv("PKR_TEST_TRAINER")) != NULL)
            sscanf(s, "%d:%d", &tframe, &trainer);
    }
    if (tframe >= 0 && (int)Port_FrameCounter() >= tframe && PortIsOverworld()) {
        tframe = -1;
        /* double battles need two Pokemon (the game checks that before) */
        if (CalculatePlayerPartyCount() < 2) {
            CreateMon(&gPlayerParty[1], SPECIES_ZIGZAGOON, 10, 32, 0, 0, 0, 0);
            CalculatePlayerPartyCount();
        }
        gTrainerBattleOpponent = (u16)trainer;
        BattleSetup_StartTrainerBattle();
        fprintf(stderr, "[test] trainer battle %d at frame %u\n", trainer, Port_FrameCounter());
        return;
    }
    if (frame < 0 || (int)Port_FrameCounter() < frame || !PortIsOverworld())
        return;
    frame = -1;
    if (CalculatePlayerPartyCount() == 0) {
        CreateMon(&gPlayerParty[0], SPECIES_MUDKIP, 7, 32, 0, 0, 0, 0);
        CalculatePlayerPartyCount();
    }
    CreateScriptedWildMon((u16)species, (u8)level, 0);
    BattleSetup_StartWildBattle();
    fprintf(stderr, "[test] wild battle at frame %u\n", Port_FrameCounter());
}
#else
void PortTest_Frame(void) {}
#endif
