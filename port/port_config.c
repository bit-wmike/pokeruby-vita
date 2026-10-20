/* Settings of the host / PS Vita port, stored as settings.ini. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port_config.h"

struct PortConfig gPortConfig = {PORT_VIEW_GBA, PORT_STYLE_PIXEL, 0};

static void Set(const char *key, int v)
{
    if (!strcmp(key, "view_mode") && v >= 0 && v <= 1)
        gPortConfig.viewMode = v;
    else if (!strcmp(key, "display_style") && v >= 0 && v <= 2)
        gPortConfig.displayStyle = v;
    else if (!strcmp(key, "frame_rate_30"))
        gPortConfig.frameRate30 = v != 0;
}

void Port_ConfigLoad(void)
{
    char buf[1024];
    long n = Platform_ReadFile("settings.ini", buf, sizeof(buf) - 1);
    char *line;
    if (n <= 0)
        return;
    buf[n] = 0;
    for (line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char key[64];
        int v;
        if (sscanf(line, " %63[a-z_0-9] = %d", key, &v) == 2)
            Set(key, v);
    }
}

void Port_ConfigSave(void)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "# Pokemon Vita port settings\n"
                     "view_mode = %d\n"
                     "display_style = %d\n"
                     "frame_rate_30 = %d\n",
                     gPortConfig.viewMode, gPortConfig.displayStyle, gPortConfig.frameRate30);
    Platform_WriteFile("settings.ini", buf, (unsigned long)n);
}
