/* Cheats that change the game's data once (the Vita menu). */
#include "global.h"

void PortCheat_MaxMoney(void)
{
    gSaveBlock1.money = 999999;
}

/* ---- Rucksack (Mike, 0.3.12): the numbers of items one already has ------ */
#include "item.h"

static struct ItemSlot *Pocket(int p, int *n)
{
    switch (p) {
    case 0: *n = BAG_ITEMS_COUNT; return gSaveBlock1.bagPocket_Items;
    case 1: *n = BAG_POKEBALLS_COUNT; return gSaveBlock1.bagPocket_PokeBalls;
    case 2: *n = BAG_TMHM_COUNT; return gSaveBlock1.bagPocket_TMHM;
    default: *n = BAG_BERRIES_COUNT; return gSaveBlock1.bagPocket_Berries;
    }
}

/* the k-th item in the pocket (0 = none left) */
int PortBag_Get(int pocket, int k, unsigned short *item, unsigned short *qty)
{
    int n, i;
    struct ItemSlot *s = Pocket(pocket, &n);
    for (i = 0; i < n; i++)
        if (s[i].itemId && s[i].quantity && k-- == 0) {
            *item = s[i].itemId;
            *qty = s[i].quantity;
            return 1;
        }
    return 0;
}

void PortBag_Set(int pocket, int k, int qty)
{
    int n, i;
    struct ItemSlot *s = Pocket(pocket, &n);
    if (qty < 1) qty = 1;
    if (qty > 99) qty = 99;
    for (i = 0; i < n; i++)
        if (s[i].itemId && s[i].quantity && k-- == 0) {
            s[i].quantity = (u16)qty;
            return;
        }
}

/* the item's German name in plain letters for the Vita font */
void PortBag_Name(unsigned short item, char *out, int size)
{
    const u8 *s = ItemId_GetName(item);
    int o = 0;
    for (; *s != 0xFF && o < size - 3; s++) {
        u8 c = *s;
        const char *t = NULL;
        char ch = 0;
        if (c >= 0xBB && c <= 0xD4) ch = (char)('A' + c - 0xBB);
        else if (c >= 0xD5 && c <= 0xEE) ch = (char)('a' + c - 0xD5);
        else if (c >= 0xA1 && c <= 0xAA) ch = (char)('0' + c - 0xA1);
        else if (c == 0x00) ch = ' ';
        else if (c == 0xAD) ch = '.';
        else if (c == 0xAE) ch = '-';
        else if (c == 0xF1) t = "AE";
        else if (c == 0xF2) t = "OE";
        else if (c == 0xF3) t = "UE";
        else if (c == 0xF4) t = "ae";
        else if (c == 0xF5) t = "oe";
        else if (c == 0xF6) t = "ue";
        else if (c == 0x15) t = "ss";
        else if (c == 0x1B || c == 0x06) ch = 'e';
        else ch = '?';
        if (t) {
            while (*t && o < size - 1) out[o++] = *t++;
        } else {
            out[o++] = ch;
        }
    }
    out[o] = 0;
}
