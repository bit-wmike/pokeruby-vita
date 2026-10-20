/* Settings of the host / PS Vita port, stored as settings.ini. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port_config.h"

struct PortConfig gPortConfig = {PORT_VIEW_FULL, PORT_STYLE_PIXEL, 0, 0};

static void Set(const char *key, int v)
{
    if (!strcmp(key, "wide_view") && v >= 0 && v <= 1)
        gPortConfig.viewMode = v;
    else if (!strcmp(key, "display_style") && v >= 0 && v <= 2)
        gPortConfig.displayStyle = v;
    else if (!strcmp(key, "frame_rate_30"))
        gPortConfig.frameRate30 = v != 0;
    else if (!strcmp(key, "only_hoenn"))
        gPortConfig.gen3Wild = v != 0;
}

void Port_ConfigLoad(void)
{
    char buf[1024];
    long n = Platform_ReadFile("settings.ini", buf, sizeof(buf) - 1);
    char *line;
    const char *env = getenv("PKR_VIEW");
    if (n > 0) {
    buf[n] = 0;
    for (line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char key[64];
        int v;
        if (sscanf(line, " %63[a-z_0-9] = %d", key, &v) == 2)
            Set(key, v);
    }
    }
    if (getenv("PKR_HOENN")) /* host build */
        gPortConfig.gen3Wild = atoi(getenv("PKR_HOENN")) != 0;
    if (env) /* host build: PKR_VIEW=0/1 */
        gPortConfig.viewMode = atoi(env) ? PORT_VIEW_FULL : PORT_VIEW_GBA;
}

void Port_ConfigSave(void)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "# Pokemon Vita port settings\n"
                     "wide_view = %d\n"
                     "display_style = %d\n"
                     "frame_rate_30 = %d\n"
                     "only_hoenn = %d\n",
                     gPortConfig.viewMode, gPortConfig.displayStyle, gPortConfig.frameRate30, gPortConfig.gen3Wild);
    Platform_WriteFile("settings.ini", buf, (unsigned long)n);
}

int PortGen3Wild(void)
{
    return gPortConfig.gen3Wild;
}
