/* Settings of the host / PS Vita port (settings.ini in the data folder). */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

enum { PORT_VIEW_GBA = 0, PORT_VIEW_FULL = 1 };
/* display styles (settings value 2/3; the old 0/1 become pixel exact) */
enum { PORT_STYLE_PIXEL = 2, PORT_STYLE_HQ = 3 };

struct PortConfig {
    int viewMode;     /* PORT_VIEW_* */
    int displayStyle; /* PORT_STYLE_* */
    int frameRate30;  /* render every 2nd frame, game speed unchanged */
    int expMult;      /* cheat: EXP multiplier 1/2/4/8 */
    int gen3Wild;     /* "Nur Hoenn": only gen-3 wild Pokémon */
    int turbo;        /* turbo factor (right stick up held): 2..4 */
};

/* cheats of the session (not saved) */
struct PortCheats {
    int encounters;   /* 0 none, 1 normal, 2 often */
    int shiny;        /* wild Pokemon shiny 1 in 32 */
    int randomSpecies;/* any Pokemon instead of the area's (level of the area) */
    int alwaysFlee;
    int runIndoors;
};
extern struct PortCheats gPortCheats;

extern struct PortConfig gPortConfig;

void Port_ConfigLoad(void);
void Port_ConfigSave(void);

/* Small files in the port's data folder (platform). */
long Platform_ReadFile(const char *name, void *data, unsigned long size);
int Platform_WriteFile(const char *name, const void *data, unsigned long size);

#endif /* PORT_CONFIG_H */
