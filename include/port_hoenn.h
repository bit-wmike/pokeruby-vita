// Host / PS Vita port option "Nur Hoenn" (port/port_hoenn.c).
#ifndef GUARD_PORT_HOENN_H
#define GUARD_PORT_HOENN_H
#ifdef PORT
struct Pokemon;
u16 PortHoennSpecies(u16 species, u8 level, u8 trainerClass);
int PortHoennIsBoss(u16 trainerNum, u8 trainerClass);
u16 PortHoennBossLook(u16 trainerNum, u16 species);
void PortHoennClearLooks(void);
void PortHoennSetLook(int slot, struct Pokemon *mon, u16 look);
u16 PortHoennLook(struct Pokemon *mon, u16 species);
u16 PortHoennBattlerLook(u8 battler, u16 species);
#define PORT_LOOK(mon, species) PortHoennLook(mon, species)
#define PORT_BATTLER_LOOK(battler, species) PortHoennBattlerLook(battler, species)
#else
#define PORT_LOOK(mon, species) (species)
#define PORT_BATTLER_LOOK(battler, species) (species)
#endif
#endif
