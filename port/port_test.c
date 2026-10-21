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
void CB2_PartyMenuMain(void);
void sub_809D844(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "constants/moves.h"
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
int Port_StateResult(void);

/* ---- PKR_TEST_PLAN=file: a list of steps, one per line -----------------
 *   warp G N X Y      warp there (waits until the overworld is back)
 *   face D            press a direction (1 down 2 up 3 left 4 right) once
 *   mash F            press A every 8 frames for F frames, choosing "yes"
 *   back F            press B every 8 frames for F frames
 *   walk F            walk around at random for F frames
 *   keys HEX F        hold keys for F frames
 *   note TEXT         printed as "[plan] TEXT"
 * Before the first step a savestate is taken; when the game is not back in
 * the overworld 2000 frames after a step, it is loaded ("[plan] stuck") and
 * the plan goes on. Every step is printed before it runs, so a crash names
 * its step. */
u16 gPortTestKeys;   /* ORed into the keys by the host platform */

static FILE *sPlan;
static char sStep[128], sNote[128];
static int sStepLeft, sStepKind, sStepArg, sStepKeys, sWaitWarp, sPlanState, sStuck, sSeed = 1;
static int sWarp[4];

static int PlanRand(void)
{
    sSeed = sSeed * 1103515245 + 12345;
    return (sSeed >> 16) & 0x7FFF;
}

static int sParty, sReparty;

static void ApplyParty(void)
{
    /* a strong team, so test battles end soon */
    static const u16 team[6] = {SPECIES_RAYQUAZA, SPECIES_GROUDON, SPECIES_KYOGRE, SPECIES_METAGROSS,
                                SPECIES_SALAMENCE, SPECIES_LATIOS};
    int k;
    for (k = 0; k < 6; k++)
    {
        u16 mv = MOVE_DRAGON_CLAW;   /* hits everything in gen 3 */
        u8 pp = 40;
        CreateMon(&gPlayerParty[k], team[k], 100, 32, 0, 0, 0, 0);
        SetMonData(&gPlayerParty[k], MON_DATA_MOVE1, &mv);
        SetMonData(&gPlayerParty[k], MON_DATA_PP1, &pp);
    }
    CalculatePlayerPartyCount();
    gSaveBlock2.optionsBattleStyle = 1;   /* "set": no switch question */
}

static void TestPlan(void)
{
    static int init;
    if (!init) {
        const char *f = getenv("PKR_TEST_PLAN");
        init = 1;
        if (f)
            sPlan = fopen(f, "r");
    }
    gPortTestKeys = 0;
    if (!sPlan || (int)Port_FrameCounter() < 1600)
        return;
    if (sPlanState == 0) {             /* the savestate to fall back to */
        if (!PortIsOverworld())
            return;
        Port_StateRequest(0);
        sPlanState = 1;
        return;
    }
    if (sReparty && --sReparty == 0)   /* a few frames on: the load is done */
        ApplyParty();
    if (sWaitWarp) {
        if (PortIsOverworld() && --sWaitWarp == 0)
            return;
        static int wasBattle;
        if (wasBattle != gMain.inBattle) {   /* battle and field timed apart */
            wasBattle = gMain.inBattle;
            sStuck = 0;
        }
        if (!sStuck)
            sStuck = (int)Port_FrameCounter();
        if ((int)Port_FrameCounter() - sStuck > (gMain.inBattle ? 12000 : 2500)) {
            fprintf(stderr, "[plan] stuck after: %s | %s | cb2=%p cb1=%p battle=%d\n", sNote, sStep,
                    (void *)gMain.callback2, (void *)gMain.callback1, gMain.inBattle);
            Port_StateRequest(1);
            sReparty = sParty ? 10 : 0;   /* the savestate is older than the team */
            sStuck = 0;
            sWaitWarp = 0;
        }
        /* in a battle A (fight on), elsewhere B (leave menus, end texts) */
        gPortTestKeys = (Port_FrameCounter() & 8) ? (gMain.inBattle ? 1 : 2) : 0;
        /* party menu / summary (switch offered): cancel with B */
        if (gMain.callback2 == CB2_PartyMenuMain || gMain.callback2 == sub_809D844)
            gPortTestKeys = (Port_FrameCounter() & 8) ? 2 : 0;
        return;
    }
    if (sStepLeft > 0) {
        sStepLeft--;
        switch (sStepKind) {
        case 'm': gPortTestKeys = (sStepLeft & 7) == 0 ? 1 : 0; break;
        case 'b': gPortTestKeys = (sStepLeft & 7) == 0 ? 2 : 0; break;
        case 'k': gPortTestKeys = (u16)sStepKeys; break;
        case 'w':
            if ((sStepLeft & 15) == 0)
                sStepArg = 0x10 << (PlanRand() & 3);
            gPortTestKeys = (u16)sStepArg;
            break;
        }
        if (sStepLeft == 0 && sStepKind != 'k')
            sWaitWarp = 1;   /* back in the overworld before the next step */
        return;
    }
    if (!fgets(sStep, sizeof(sStep), sPlan)) {
        fprintf(stderr, "[plan] done at frame %u\n", Port_FrameCounter());
        fclose(sPlan);
        exit(0);
    }
    sStep[strcspn(sStep, "\r\n")] = 0;
    sStuck = 0;
    if (!strncmp(sStep, "warp", 4) && sscanf(sStep + 4, "%d %d %d %d", &sWarp[0], &sWarp[1], &sWarp[2], &sWarp[3]) == 4) {
        if (!PortIsOverworld()) {
            sWaitWarp = 1;
            return;
        }
        Overworld_SetWarpDestination(sWarp[0], sWarp[1], -1, sWarp[2], sWarp[3]);
        sub_8080E88();
        ResetInitialPlayerAvatarState();
        sWaitWarp = 30;
    } else if (!strncmp(sStep, "face", 4)) {
        static const u16 dir[5] = {0, 0x80, 0x40, 0x20, 0x10};
        sStepKind = 'k';
        int d = atoi(sStep + 5);
        sStepKeys = dir[d >= 1 && d <= 4 ? d : 1];
        sStepLeft = 2;
    } else if (!strncmp(sStep, "mash", 4)) {
        sStepKind = 'm';
        sStepLeft = atoi(sStep + 5);
    } else if (!strncmp(sStep, "back", 4)) {
        sStepKind = 'b';
        sStepLeft = atoi(sStep + 5);
    } else if (!strncmp(sStep, "walk", 4)) {
        sStepKind = 'w';
        sStepLeft = atoi(sStep + 5);
    } else if (!strncmp(sStep, "keys", 4)) {
        unsigned k;
        int f;
        if (sscanf(sStep + 5, "%x %d", &k, &f) == 2) {
            sStepKind = 'k';
            sStepKeys = (int)k;
            sStepLeft = f;
        }
    } else if (!strncmp(sStep, "party", 5)) {
        sParty = 1;
        ApplyParty();
    } else if (!strncmp(sStep, "note", 4)) {
        fprintf(stderr, "[plan] %s\n", sStep + 5);
        if (sParty)   /* fresh team (HP, PP) for every target */
            ApplyParty();
        snprintf(sNote, sizeof(sNote), "%s", sStep + 5);
    }
}

/* PKR_TEST_SPECIES=first:last: a wild battle against each species, with
 * the same species (level 60) on our side - front and back sprites, cries,
 * the dex page; press A regularly (PKR_AUTO_A) */
static void TestSpecies(void)
{
    static int init, cur = -1, last, wait, started;
    if (!init) {
        const char *e = getenv("PKR_TEST_SPECIES");
        init = 1;
        if (e)
            sscanf(e, "%d:%d", &cur, &last);
    }
    if (cur < 0 || cur > last || (int)Port_FrameCounter() < 1600)
        return;
    if (!PortIsOverworld()) {
        wait = 0;
        started = 0;   /* the battle is on */
        return;
    }
    if (started && ++started < 600)   /* the transition is still to begin */
        return;
    if (started)
        fprintf(stderr, "[test] species %d: no battle\n", cur - 1);
    if (++wait < 30)
        return;
    wait = 0;
    started = 1;
    if (cur == 252) cur = 277;   /* unused numbers between Celebi and Treecko */
    if (cur > last)
        return;
    {
        u16 mv = cur == SPECIES_SHEDINJA ? MOVE_SHADOW_BALL : MOVE_DRAGON_CLAW;   /* first move must end the battle (Wonder Guard) */
        CreateMon(&gPlayerParty[0], (u16)cur, 60, 32, 0, 0, 0, 0);
        SetMonData(&gPlayerParty[0], MON_DATA_MOVE1, &mv);
    }
    CalculatePlayerPartyCount();
    CreateScriptedWildMon((u16)cur, 5, 0);
    BattleSetup_StartWildBattle();
    fprintf(stderr, "[test] species %d at frame %u\n", cur, Port_FrameCounter());
    cur++;
}

void PortTest_Frame(void)
{
    TestSpecies();
    TestPlan();
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
