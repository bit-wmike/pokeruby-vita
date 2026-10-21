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
    /* button mapping in the game (not the port menu): the GBA function of
     * each Vita button, PORT_KEY_* - order of PORT_BTN_* */
    int keymap[8];
};

enum { PORT_BTN_CROSS, PORT_BTN_CIRCLE, PORT_BTN_SQUARE, PORT_BTN_TRIANGLE, PORT_BTN_L, PORT_BTN_R,
       PORT_BTN_START, PORT_BTN_SELECT, PORT_BTN_COUNT };
enum { PORT_KEY_NONE, PORT_KEY_A, PORT_KEY_B, PORT_KEY_START, PORT_KEY_SELECT, PORT_KEY_L, PORT_KEY_R,
       PORT_KEY_COUNT };
extern const char *const gPortBtnNames[PORT_BTN_COUNT];   /* settings keys */

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
