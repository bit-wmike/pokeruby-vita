/*
 * Host-only test helpers (not in the PS Vita build).
 *
 *   PKR_TEST_BATTLE=frame:species:level   at that frame (in the overworld)
 *       give the player a Mudkip if the party is empty and start a wild
 *       battle against species/level (numbers from constants/species.h).
 */
#ifndef __vita__
#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "pokemon.h"
#include "battle_setup.h"
#include "constants/species.h"

#include "port/port.h"

void CreateScriptedWildMon(u16 species, u8 level, u16 item);
int PortIsOverworld(void);

void PortTest_Frame(void)
{
    static int parsed, frame = -1, species, level;
    if (!parsed) {
        const char *s = getenv("PKR_TEST_BATTLE");
        parsed = 1;
        if (s)
            sscanf(s, "%d:%d:%d", &frame, &species, &level);
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
