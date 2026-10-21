/* Wide view of the Pokémon port (port/port_view.c). */
#ifndef PORT_PORT_VIEW_H
#define PORT_PORT_VIEW_H

#include <stdint.h>

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
/* BG0 text windows to the edges of the wide frame (also for the battle) */
void PortView_PlaceWindows(void);
int PortView_SpriteInWindow(int oamIndex);
int PortView_InWindow(int x, int y);

/* Battle stage (port/port_stage.c): 1 when this frame is a wide battle */
int PortStage_BeginFrame(void);
void PortStage_FieldPass(int *w, int *h);
void PortStage_UiPass(void);
void PortStage_Present(const uint32_t *field, int fpitch, const uint32_t *ui, int upitch);

#endif
