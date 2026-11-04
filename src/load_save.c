#include "global.h"
#include "gba/flash_internal.h"
#include "load_save.h"
#include "main.h"
#include "pokemon.h"
#include "overworld.h"
#ifdef PORT
#include <string.h>
#include "event_object_movement.h"
#endif

extern u8 gPlayerPartyCount;

bool32 gFlashMemoryPresent;

struct LoadedSaveData
{
    struct ItemSlot items[20];
    struct ItemSlot keyItems[20];
    struct ItemSlot pokeBalls[16];
    struct ItemSlot TMsHMs[64];
    struct ItemSlot berries[46];
    struct MailStruct mail[16];
};

EWRAM_DATA struct SaveBlock2 gSaveBlock2 = {0};
EWRAM_DATA struct SaveBlock1 gSaveBlock1 = {0};

static EWRAM_DATA struct LoadedSaveData gLoadedSaveData = {0};

void CheckForFlashMemory(void)
{
    if (!IdentifyFlash())
    {
        gFlashMemoryPresent = TRUE;
        InitFlashTimer();
    }
    else
        gFlashMemoryPresent = FALSE;
}

bool32 GetSecretBase2Field_9(void)
{
    return gSaveBlock2.specialSaveWarp;
}

void ClearSecretBase2Field_9(void)
{
    gSaveBlock2.specialSaveWarp = 0;
}

void SetSecretBase2Field_9(void)
{
    gSaveBlock2.specialSaveWarp = 1;
}

void SetSecretBase2Field_9_AndHideBG(void) // note: no other function sets specialSaveWarp to values other than 0 or 1, hence clear and set distinctions.
{
    gpu_sync_bg_hide(0); // the function doesn't use the parameter passed to it, but this is necessary to match.
    gSaveBlock2.specialSaveWarp = 1;
}

void ClearSecretBase2Field_9_2(void) // duplicate function
{
    gSaveBlock2.specialSaveWarp = 0;
}

void SavePlayerParty(void)
{
    int i;

    gSaveBlock1.playerPartyCount = gPlayerPartyCount;

    for (i = 0; i < PARTY_SIZE; i++)
        gSaveBlock1.playerParty[i] = gPlayerParty[i];
}

void LoadPlayerParty(void)
{
    int i;

    gPlayerPartyCount = gSaveBlock1.playerPartyCount;

    for (i = 0; i < PARTY_SIZE; i++)
        gPlayerParty[i] = gSaveBlock1.playerParty[i];
}

#ifdef PORT
// The port keeps up to OBJECT_EVENTS_COUNT (32) object events at runtime, the
// save file only OBJECT_EVENTS_SAVE_COUNT (16, GBA layout). If all active ones
// sit in the first 16 slots the copy is the original one; otherwise the player
// and the objects nearest to him are kept (in slot order), the others spawn
// again from the map templates after loading (PortObjectEvents_AfterLoad).
static bool8 sPortObjectsLoaded;

static void SaveObjectEvents(void)
{
    int i, j, n = 0;
    u8 keep[OBJECT_EVENTS_COUNT];
    int dist[OBJECT_EVENTS_COUNT];
    s16 px = 0, py = 0;

    for (i = OBJECT_EVENTS_SAVE_COUNT; i < OBJECT_EVENTS_COUNT; i++)
        if (gObjectEvents[i].active)
            break;
    if (i == OBJECT_EVENTS_COUNT)
    {
        for (i = 0; i < OBJECT_EVENTS_SAVE_COUNT; i++)
            gSaveBlock1.objectEvents[i] = gObjectEvents[i];
        return;
    }
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        if (gObjectEvents[i].active && gObjectEvents[i].isPlayer)
            px = gObjectEvents[i].currentCoords.x, py = gObjectEvents[i].currentCoords.y;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        struct ObjectEvent *o = &gObjectEvents[i];
        int dx, dy;
        keep[i] = 0;
        if (!o->active)
        {
            dist[i] = 0x7FFFFFFF;
            continue;
        }
        dx = o->currentCoords.x - px;
        dy = o->currentCoords.y - py;
        dist[i] = o->isPlayer ? -1 : (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }
    // pick the OBJECT_EVENTS_SAVE_COUNT smallest distances (player first, ties by slot)
    for (n = 0; n < OBJECT_EVENTS_SAVE_COUNT; n++)
    {
        int best = -1;
        for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
            if (!keep[i] && dist[i] != 0x7FFFFFFF && (best < 0 || dist[i] < dist[best]))
                best = i;
        if (best < 0)
            break;
        keep[best] = 1;
    }
    for (i = 0, j = 0; i < OBJECT_EVENTS_COUNT; i++)
        if (keep[i])
            gSaveBlock1.objectEvents[j++] = gObjectEvents[i];
    for (; j < OBJECT_EVENTS_SAVE_COUNT; j++)
        memset(&gSaveBlock1.objectEvents[j], 0, sizeof(gSaveBlock1.objectEvents[j]));
}

static void LoadObjectEvents(void)
{
    int i;

    for (i = 0; i < OBJECT_EVENTS_SAVE_COUNT; i++)
        gObjectEvents[i] = gSaveBlock1.objectEvents[i];
    for (; i < OBJECT_EVENTS_COUNT; i++)
        memset(&gObjectEvents[i], 0, sizeof(gObjectEvents[i]));
}

// Called once the loaded objects have their sprites again (continue game):
// spawn the people of the current map the save file had no room for.
void PortObjectEvents_AfterLoad(void)
{
    if (sPortObjectsLoaded)
    {
        sPortObjectsLoaded = FALSE;
        TrySpawnObjectEvents(0, 0);
    }
}

// CB2_ContinueSavedGame: the objects came from the save file.
void PortObjectEvents_RequestAfterLoad(void)
{
    sPortObjectsLoaded = TRUE;
}
#else
static void SaveObjectEvents(void)
{
    int i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        gSaveBlock1.objectEvents[i] = gObjectEvents[i];
}

static void LoadObjectEvents(void)
{
    int i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        gObjectEvents[i] = gSaveBlock1.objectEvents[i];
}
#endif

void SaveSerializedGame(void)
{
    SavePlayerParty();
    SaveObjectEvents();
}

void LoadSerializedGame(void)
{
    LoadPlayerParty();
    LoadObjectEvents();
}

void LoadPlayerBag(void)
{
    int i;

    // load player items.
    for (i = 0; i < BAG_ITEMS_COUNT; i++)
        gLoadedSaveData.items[i] = gSaveBlock1.bagPocket_Items[i];

    // load player key items.
    for (i = 0; i < BAG_KEYITEMS_COUNT; i++)
        gLoadedSaveData.keyItems[i] = gSaveBlock1.bagPocket_KeyItems[i];

    // load player pokeballs.
    for (i = 0; i < BAG_POKEBALLS_COUNT; i++)
        gLoadedSaveData.pokeBalls[i] = gSaveBlock1.bagPocket_PokeBalls[i];

    // load player TMs and HMs.
    for (i = 0; i < BAG_TMHM_COUNT; i++)
        gLoadedSaveData.TMsHMs[i] = gSaveBlock1.bagPocket_TMHM[i];

    // load player berries.
    for (i = 0; i < BAG_BERRIES_COUNT; i++)
        gLoadedSaveData.berries[i] = gSaveBlock1.bagPocket_Berries[i];

    // load mail.
    for (i = 0; i < MAIL_COUNT; i++)
        gLoadedSaveData.mail[i] = gSaveBlock1.mail[i];
}

void SavePlayerBag(void)
{
    int i;

    // save player items.
    for (i = 0; i < BAG_ITEMS_COUNT; i++)
        gSaveBlock1.bagPocket_Items[i] = gLoadedSaveData.items[i];

    // save player key items.
    for (i = 0; i < BAG_KEYITEMS_COUNT; i++)
        gSaveBlock1.bagPocket_KeyItems[i] = gLoadedSaveData.keyItems[i];

    // save player pokeballs.
    for (i = 0; i < BAG_POKEBALLS_COUNT; i++)
        gSaveBlock1.bagPocket_PokeBalls[i] = gLoadedSaveData.pokeBalls[i];

    // save player TMs and HMs.
    for (i = 0; i < BAG_TMHM_COUNT; i++)
        gSaveBlock1.bagPocket_TMHM[i] = gLoadedSaveData.TMsHMs[i];

    // save player berries.
    for (i = 0; i < BAG_BERRIES_COUNT; i++)
        gSaveBlock1.bagPocket_Berries[i] = gLoadedSaveData.berries[i];

    // save mail.
    for (i = 0; i < MAIL_COUNT; i++)
        gSaveBlock1.mail[i] = gLoadedSaveData.mail[i];
}
