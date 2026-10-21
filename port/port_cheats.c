/* Cheats that change the game's data once (the Vita menu). */
#include "global.h"

void PortCheat_MaxMoney(void)
{
    gSaveBlock1.money = 999999;
}
