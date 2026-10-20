/*
 * Port option "Nur Hoenn": only Pokémon of the third generation.
 *
 *  - wild Pokémon: own encounter tables (src/data/port_gen3_wild.h);
 *  - trainers, Team Aqua/Magma, static encounters, in-game trades: a real
 *    Hoenn Pokémon replaces the Kanto/Johto one (same level, similar type
 *    and strength, its own level-up moves);
 *  - gym leaders, Elite Four, champion, Aqua/Magma leaders and Heiko keep
 *    their Pokémon exactly (stats, moves, level) so the balance stays, but
 *    look like a Hoenn Pokémon of the same kind: name, picture, cry and
 *    Pokédex entry ("reskin").
 */
#include <string.h>

#include "global.h"
#include "battle.h"
#include "data2.h"
#include "rom_8077ABC.h"
#include "pokemon.h"
#include "constants/species.h"
#include "constants/trainers.h"
#include "constants/opponents.h"

#include "port/port_config.h"

static int IsGen3(u16 s)
{
    return s >= SPECIES_TREECKO && s <= SPECIES_CHIMECHO;
}

/* Kanto/Johto species -> Hoenn species, by evolution stage */
static const u16 sReplace[][2] = {
    {SPECIES_ABRA, SPECIES_RALTS},           {SPECIES_KADABRA, SPECIES_KIRLIA},
    {SPECIES_ALAKAZAM, SPECIES_GARDEVOIR},   {SPECIES_MARILL, SPECIES_AZURILL},
    {SPECIES_AZUMARILL, SPECIES_SEALEO},     {SPECIES_ODDISH, SPECIES_SHROOMISH},
    {SPECIES_GLOOM, SPECIES_BRELOOM},        {SPECIES_VILEPLUME, SPECIES_BRELOOM},
    {SPECIES_BELLOSSOM, SPECIES_LUDICOLO},   {SPECIES_CHINCHOU, SPECIES_WAILMER},
    {SPECIES_LANTURN, SPECIES_WHISCASH},     {SPECIES_DODUO, SPECIES_TAILLOW},
    {SPECIES_DODRIO, SPECIES_SWELLOW},       {SPECIES_GEODUDE, SPECIES_NOSEPASS},
    {SPECIES_GRAVELER, SPECIES_CLAYDOL},     {SPECIES_GOLEM, SPECIES_ARMALDO},
    {SPECIES_GIRAFARIG, SPECIES_GRUMPIG},    {SPECIES_GOLDEEN, SPECIES_BARBOACH},
    {SPECIES_SEAKING, SPECIES_WHISCASH},     {SPECIES_PSYDUCK, SPECIES_WINGULL},
    {SPECIES_GOLDUCK, SPECIES_PELIPPER},     {SPECIES_MAGIKARP, SPECIES_FEEBAS},
    {SPECIES_GYARADOS, SPECIES_WAILORD},     {SPECIES_HORSEA, SPECIES_CORPHISH},
    {SPECIES_SEADRA, SPECIES_CRAWDAUNT},     {SPECIES_KOFFING, SPECIES_GULPIN},
    {SPECIES_WEEZING, SPECIES_SWALOT},       {SPECIES_GRIMER, SPECIES_GULPIN},
    {SPECIES_MUK, SPECIES_SWALOT},           {SPECIES_MACHOP, SPECIES_MAKUHITA},
    {SPECIES_MACHOKE, SPECIES_MEDICHAM},     {SPECIES_MACHAMP, SPECIES_HARIYAMA},
    {SPECIES_MAGNEMITE, SPECIES_ELECTRIKE},  {SPECIES_MAGNETON, SPECIES_MANECTRIC},
    {SPECIES_VOLTORB, SPECIES_MINUN},        {SPECIES_ELECTRODE, SPECIES_MANECTRIC},
    {SPECIES_PIKACHU, SPECIES_PLUSLE},       {SPECIES_SLUGMA, SPECIES_NUMEL},
    {SPECIES_MAGCARGO, SPECIES_CAMERUPT},    {SPECIES_VULPIX, SPECIES_TORKOAL},
    {SPECIES_NINETALES, SPECIES_TORKOAL},    {SPECIES_NATU, SPECIES_SPOINK},
    {SPECIES_XATU, SPECIES_CHIMECHO},        {SPECIES_RHYHORN, SPECIES_ARON},
    {SPECIES_RHYDON, SPECIES_AGGRON},        {SPECIES_SANDSHREW, SPECIES_TRAPINCH},
    {SPECIES_SANDSLASH, SPECIES_VIBRAVA},    {SPECIES_STARYU, SPECIES_LUVDISC},
    {SPECIES_STARMIE, SPECIES_GOREBYSS},     {SPECIES_TENTACOOL, SPECIES_CARVANHA},
    {SPECIES_TENTACRUEL, SPECIES_SHARPEDO},  {SPECIES_ZUBAT, SPECIES_SABLEYE},
    {SPECIES_GOLBAT, SPECIES_DUSKULL},       {SPECIES_CROBAT, SPECIES_DUSCLOPS},
    {SPECIES_JIGGLYPUFF, SPECIES_WHISMUR},   {SPECIES_WIGGLYTUFF, SPECIES_LOUDRED},
    {SPECIES_PINSIR, SPECIES_NINJASK},       {SPECIES_HERACROSS, SPECIES_BRELOOM},
    {SPECIES_SKARMORY, SPECIES_LAIRON},      {SPECIES_PHANPY, SPECIES_NUMEL},
    {SPECIES_WOBBUFFET, SPECIES_WYNAUT},     {SPECIES_CORSOLA, SPECIES_RELICANTH},
};

/* Team Aqua / Magma: their bats become the team's own Pokémon */
static u16 TeamReplace(u16 s, u8 trainerClass)
{
    int aqua = trainerClass == TRAINER_CLASS_TEAM_AQUA || trainerClass == TRAINER_CLASS_AQUA_ADMIN;
    int magma = trainerClass == TRAINER_CLASS_TEAM_MAGMA || trainerClass == TRAINER_CLASS_MAGMA_ADMIN;
    if (!aqua && !magma)
        return 0;
    switch (s) {
    case SPECIES_ZUBAT: return aqua ? SPECIES_CARVANHA : SPECIES_NUMEL;
    case SPECIES_GOLBAT: return aqua ? SPECIES_SHARPEDO : SPECIES_CAMERUPT;
    case SPECIES_CROBAT: return aqua ? SPECIES_SHARPEDO : SPECIES_CAMERUPT;
    case SPECIES_KOFFING: return aqua ? SPECIES_CARVANHA : SPECIES_NUMEL;
    case SPECIES_WEEZING: return aqua ? SPECIES_SHARPEDO : SPECIES_CAMERUPT;
    }
    return 0;
}

/* Walks back the evolution chain while the level is too low for the form. */
static u16 FitLevel(u16 species, u8 level)
{
    int guard;
    for (guard = 0; guard < 3; guard++) {
        u16 pre = 0, need = 0;
        int s, i;
        for (s = 1; s < NUM_SPECIES && !pre; s++)
            for (i = 0; i < 5; i++)
                if (gEvolutionTable[s][i].targetSpecies == species && gEvolutionTable[s][i].method) {
                    u16 m = gEvolutionTable[s][i].method;
                    pre = (u16)s;
                    need = (m == EVO_LEVEL || (m >= EVO_LEVEL_ATK_GT_DEF && m <= EVO_LEVEL_SHEDINJA))
                               ? gEvolutionTable[s][i].param : 30;
                    break;
                }
        if (!pre || level >= need)
            break;
        species = pre;
    }
    return species;
}

u16 PortHoennSpecies(u16 species, u8 level, u8 trainerClass)
{
    unsigned i;
    u16 r = 0;
    if (!gPortConfig.gen3Wild || species == SPECIES_NONE || species >= NUM_SPECIES || IsGen3(species))
        return species;
    r = TeamReplace(species, trainerClass);
    for (i = 0; !r && i < sizeof(sReplace) / sizeof(sReplace[0]); i++)
        if (sReplace[i][0] == species)
            r = sReplace[i][1];
    if (!r || !IsGen3(r))
        return species;
    return FitLevel(r, level);
}

/* ---- bosses: same Pokémon, Hoenn look ------------------------------- */

int PortHoennIsBoss(u16 trainerNum, u8 trainerClass)
{
    return trainerClass == TRAINER_CLASS_LEADER || trainerClass == TRAINER_CLASS_ELITE_FOUR ||
           trainerClass == TRAINER_CLASS_CHAMPION || trainerClass == TRAINER_CLASS_AQUA_LEADER ||
           trainerClass == TRAINER_CLASS_MAGMA_LEADER ||
           (trainerNum >= TRAINER_WALLY_1 && trainerNum <= TRAINER_WALLY_1 + 5);
}

/* look-alike of the same kind (type), for display only */
u16 PortHoennBossLook(u16 trainerNum, u16 species)
{
    if (!gPortConfig.gen3Wild || IsGen3(species))
        return SPECIES_NONE;
    switch (species) {
    case SPECIES_GEODUDE: return SPECIES_ARON;
    case SPECIES_MACHOP: return SPECIES_MEDITITE;
    case SPECIES_MAGNEMITE: return SPECIES_ELECTRIKE;
    case SPECIES_VOLTORB: return SPECIES_MINUN;
    case SPECIES_MAGNETON: return SPECIES_MANECTRIC;
    case SPECIES_SLUGMA: return SPECIES_NUMEL;
    case SPECIES_SKARMORY: return trainerNum == TRAINER_STEVEN ? SPECIES_METANG : SPECIES_TROPIUS;
    case SPECIES_SEAKING: return SPECIES_GOREBYSS;
    case SPECIES_GOLBAT: return SPECIES_DUSTOX;
    case SPECIES_CROBAT: return SPECIES_DUSTOX;
    }
    return SPECIES_NONE;
}

/* The looks of the enemy party, tied to the exact Pokémon (species and
 * personality), so a later wild battle never inherits them. */
static struct {
    u16 species, look;
    u32 personality;
} sLooks[PARTY_SIZE];

void PortHoennClearLooks(void)
{
    memset(sLooks, 0, sizeof(sLooks));
}

void PortHoennSetLook(int slot, struct Pokemon *mon, u16 look)
{
    if (slot < 0 || slot >= PARTY_SIZE)
        return;
    sLooks[slot].species = (u16)GetMonData(mon, MON_DATA_SPECIES);
    sLooks[slot].personality = GetMonData(mon, MON_DATA_PERSONALITY);
    sLooks[slot].look = look;
    SetMonData(mon, MON_DATA_NICKNAME, gSpeciesNames[look]);
}

/* what the player sees of this Pokémon */
u16 PortHoennLook(struct Pokemon *mon, u16 species)
{
    int slot;
    if (mon < &gEnemyParty[0] || mon >= &gEnemyParty[PARTY_SIZE])
        return species;
    slot = (int)(mon - &gEnemyParty[0]);
    if (sLooks[slot].look && sLooks[slot].species == species &&
        sLooks[slot].personality == GetMonData(mon, MON_DATA_PERSONALITY))
        return sLooks[slot].look;
    return species;
}

extern u16 gBattlerPartyIndexes[];

u16 PortHoennBattlerLook(u8 battler, u16 species)
{
    if (GetBattlerSide(battler) != 1)
        return species;
    return PortHoennLook(&gEnemyParty[gBattlerPartyIndexes[battler]], species);
}

#ifndef __vita__
/* Host build: PKR_HOENN_SELFTEST=1 prints the enemy parties of all trainers
 * (trainer, slot, species, look, level, moves) and exits. */
#include <stdio.h>
#include <stdlib.h>
#include "battle.h"
void PortHoennSelfTest(void)
{
    int t, i, j;
    gPortConfig.gen3Wild = 1;
    for (t = 1; t <= 693; t++) {
        u8 n;
        gBattleTypeFlags = BATTLE_TYPE_TRAINER;
        n = CreateNPCTrainerParty(gEnemyParty, (u16)t);
        for (i = 0; i < n && i < PARTY_SIZE; i++) {
            u16 sp = (u16)GetMonData(&gEnemyParty[i], MON_DATA_SPECIES);
            printf("T %d %d %u %u %u", t, i, sp, PortHoennLook(&gEnemyParty[i], sp),
                   (unsigned)GetMonData(&gEnemyParty[i], MON_DATA_LEVEL));
            for (j = 0; j < 4; j++)
                printf(" %u", (unsigned)GetMonData(&gEnemyParty[i], MON_DATA_MOVE1 + j));
            printf("\n");
        }
    }
    exit(0);
}
#endif
