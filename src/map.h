#ifndef VSMAP_H
#define VSMAP_H
#include "camera.h"
#ifdef USE_THREADED_RENDER
#include <SDL_thread.h>
#endif

typedef struct sMap {
	int ready; // Whether the floor map was loaded successfully
	int ceilingReady; // Whether the ceiling map was loaded successfully
	int ceilingEnabled; // Whether ceiling rendering is enabled
	int redraw; // Whether the map needs to be redrawn
	int width, height; // Floor map dimensions
	int shift; // Bit shift for floor map width
	int tileShift; // Bit shift for floor tile side
	int tileMask; // Floor tile side minus one
	int tilesShift; // Bit shift for tiles-per-row in floor map
	int tileAreaShift; // Bit shift for floor tile area
	int ceilingWidth, ceilingHeight; // Ceiling map dimensions
	int ceilingShift; // Bit shift for ceiling map width
	int ceilingTileShift; // Bit shift for ceiling tile side
	int ceilingTileMask; // Ceiling tile side minus one
	int ceilingTilesShift; // Bit shift for tiles-per-row in ceiling map
	int ceilingTileAreaShift; // Bit shift for ceiling tile area
	int optimize; // Enable draw-time optimizations
	float optdist; // Distance where quality reduction starts
	float ceilingBase; // Base ceiling height in world space
	int *hiddeny; // Lower draw bound per column
	int *showny; // Upper draw bound per column
	int *color; // Floor texture
	unsigned char *altitude; // Floor heightmap
	int *ceilingColor; // Ceiling texture
	unsigned char *ceilingAltitude; // Ceiling heightmap
	void *screen; // Render target texture

#ifdef USE_THREADED_RENDER
	struct SMapRenderGlobCtx {
		SDL_cond *unlockcond; // Condition used to wake render threads
		struct sMap *self; // Pointer to map data
		Camera *cam; // Pointer to camera data
		int endwork; // If 1, all awakened threads exit
		int *pixels; // Pointer to screen pixel buffer
		int pitch; // Pixel buffer row length
	} rgctx; // Shared render context
	int rctxcnt; // Number of render threads
	struct sMapRenderCtx {
		SDL_sem *semaphore; // Semaphore used to wait for thread completion
		struct SMapRenderGlobCtx *global; // Shared context used by all threads
		SDL_Thread *self; // Thread object pointer
		int start, end; // Start/end of the slice rendered by this thread
	} *rctxs;
#endif
} Map;

void Map_SetScreen(Map *map, void *screen);
int Map_Open(Map *map, const char *diffuse, const char *height);
int Map_OpenDual(Map *map, const char *diffuse, const char *height, const char *ceilingDiffuse, const char *ceilingHeight);
static inline Uint8 Map_GetHeight(Map *map, Point *p) {
	if(!map->ready) return 0;
	const unsigned int mapx = (unsigned int)((int)p->x & (map->width - 1));
	const unsigned int mapy = (unsigned int)((int)p->y & (map->height - 1));
	const unsigned int tilex = mapx >> map->tileShift;
	const unsigned int tiley = mapy >> map->tileShift;
	const unsigned int tileIndex = (tiley << map->tilesShift) + tilex;
	const unsigned int inTileOffset = ((mapy & map->tileMask) << map->tileShift) + (mapx & map->tileMask);
	const unsigned int offset = (tileIndex << map->tileAreaShift) + inTileOffset;
	return map->altitude[offset];
}
void Map_Draw(Map *map, Camera *cam);
void Map_Close(Map *map);
#endif
