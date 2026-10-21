/*
 * Host-only test helpers (not in the PS Vita build).
 *
 *   PKR_TEST_BATTLE=frame:species:level   at that frame (in the overworld)
 *       give the player a Mudkip if the party is empty and start a wild
 *       battle against species/level (numbers from constants/species.h).
 *   PKR_TEST_TRAINER=frame:trainer      a trainer battle (constants/opponents.h)
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

void PortTest_Frame(void)
{
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
