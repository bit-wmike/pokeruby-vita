/* "HQ" display style (vita_hq.c) */
#ifndef VITA_HQ_H
#define VITA_HQ_H
#include <vita2d.h>
/* 1 when the HQ shaders are ready (tries once) */
int VitaHq_Available(void);
/* draws part (sx, sy, w, h) of t at (x, y), scaled, smoothed */
void VitaHq_Draw(vita2d_texture *t, float x, float y, float sx, float sy, float w, float h, float scale);
int VitaHq_Scale3xAvailable(void);
/* battle field: Scale3x, alpha blended (scale fixed at 3) */
void VitaHq_DrawScale3x(vita2d_texture *t, float x, float y, float w, float h);
#endif
