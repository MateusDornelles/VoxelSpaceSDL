#ifndef VSPICKING_H
#define VSPICKING_H

/* Click-to-select / click-to-move for world sprites (APNG billboards). */

/* Map a window-space click. Returns 1 if the click selected a sprite or issued a move. */
int Picking_HandleClick(int windowX, int windowY);
void Picking_Update(float deltaMilliseconds);
void Picking_ClearSelection(void);
int Picking_GetSelectedIndex(void);

#endif
