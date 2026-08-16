#ifndef VSMAP_H
#define VSMAP_H
#include "camera.h"
#ifdef USE_THREADED_RENDER
#include <SDL_atomic.h>
#include <SDL_thread.h>
#endif

typedef struct sMap {
	int ready; // Whether the floor map was loaded successfully
	int ceilingReady; // Whether the ceiling map was loaded successfully
	int ceilingEnabled; // Whether ceiling rendering is enabled
	int redraw; // Whether the map needs to be redrawn
	int width, height; // Floor map dimensions
	int shift; // Bit shift for floor map width
	int ceilingWidth, ceilingHeight; // Ceiling map dimensions
	int ceilingShift; // Bit shift for ceiling map width
	int optimize; // Enable draw-time optimizations
	float optdist; // Distance where quality reduction starts
#ifdef USE_AVX2
	int useAVX2; // Cached CPU support check
#endif
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
		SDL_mutex *mutex; // Protects render generation and completion state
		SDL_cond *workcond; // Condition used to publish a new frame
		SDL_cond *donecond; // Condition used to report frame completion
		struct sMap *self; // Pointer to map data
		Camera *cam; // Pointer to camera data
		SDL_atomic_t endwork; // If 1, all render threads exit
		SDL_atomic_t generation; // Monotonically increasing frame identifier
		SDL_atomic_t workersPending; // Workers that have not finished this frame
		SDL_atomic_t sleepingWorkers; // Workers blocked on workcond
		SDL_atomic_t nextColumn; // Start of the next render chunk
		Uint64 workerSpinTicks; // Hot-wait budget between frames
		Uint64 completionSpinTicks; // Main-thread completion wait budget
		int chunkWidth; // Number of columns claimed by each render job
		int *pixels; // Pointer to screen pixel buffer
		int pitch; // Pixel buffer row length
	} rgctx; // Shared render context
	int rctxcnt; // Number of render threads
	struct sMapRenderCtx {
		struct SMapRenderGlobCtx *global; // Shared context used by all threads
		SDL_Thread *self; // Thread object pointer
		int index; // Stable worker index used by the hybrid wait policy
	} *rctxs;
#endif
} Map;

void Map_SetScreen(Map *map, void *screen);
int Map_Open(Map *map, const char *diffuse, const char *height);
int Map_OpenDual(Map *map, const char *diffuse, const char *height, const char *ceilingDiffuse, const char *ceilingHeight);
static inline Uint8 Map_GetHeight(Map *map, Point *p) {
	if(!map->ready) return 0;
	unsigned int offset = (((int)p->y & (map->width - 1)) << map->shift) + ((int)p->x & (map->height - 1));
	return map->altitude[offset];
}
void Map_Draw(Map *map, Camera *cam);
void Map_Close(Map *map);
#endif
