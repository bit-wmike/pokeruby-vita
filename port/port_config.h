/* Settings of the host / PS Vita port (settings.ini in the data folder). */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

enum { PORT_VIEW_GBA = 0, PORT_VIEW_FULL = 1 };
enum { PORT_STYLE_SMOOTH = 0, PORT_STYLE_SHARP = 1, PORT_STYLE_PIXEL = 2 };

struct PortConfig {
    int viewMode;     /* PORT_VIEW_* */
    int displayStyle; /* PORT_STYLE_* */
    int frameRate30;  /* render every 2nd frame, game speed unchanged */
};

extern struct PortConfig gPortConfig;

void Port_ConfigLoad(void);
void Port_ConfigSave(void);

/* Small files in the port's data folder (platform). */
long Platform_ReadFile(const char *name, void *data, unsigned long size);
int Platform_WriteFile(const char *name, const void *data, unsigned long size);

#endif /* PORT_CONFIG_H */
