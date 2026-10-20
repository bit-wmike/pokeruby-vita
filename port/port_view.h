/* Wide view of the Pokémon port (port/port_view.c). */
#ifndef PORT_PORT_VIEW_H
#define PORT_PORT_VIEW_H

#define PORT_WIDE_W 480
#define PORT_WIDE_H 272
/* where the GBA screen sits in the wide frame */
#define PORT_WIDE_OX 120
#define PORT_WIDE_OY 56

/* How far beyond the GBA screen the game keeps objects alive (pixels;
 * read by src/event_object_movement.c, 0 in the GBA view). */
extern int gPortViewExtraX, gPortViewExtraY;

/* Right after the VBlank interrupt: what the next frame will show. */
void PortView_VBlank(void);
/* Before rendering: sets up the renderer; returns 1 for a wide frame
 * (480x272), 0 for a GBA frame (240x160). */
int PortView_BeginFrame(void);

#endif
