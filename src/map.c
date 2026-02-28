#include <SDL_render.h>
#include <SDL_log.h>
#include <stdlib.h>
#ifdef USE_SDL_IMAGE
#include <SDL_image.h>
#endif
#include "defines.h"
#include "engine.h"
#include "error.h"
#include "camera.h"
#include "map.h"

struct sMapLayerData {
	int width, height;
	int shift;
	int *color;
	unsigned char *altitude;
};

static SDL_Surface *ReencodeSurface(SDL_Surface *old) {
	if(old == NULL) return NULL;
	SDL_Surface *new = old;

	if(old->format->format != SDL_PIXELFORMAT_ARGB8888) {
		new = SDL_ConvertSurfaceFormat(old, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(old);
	}

	return new;
}

static SDL_Surface *ScaleSurface(SDL_Surface *old, int side) {
	SDL_Surface *new;
	if((new = SDL_CreateRGBSurfaceWithFormat(0, side, side, 32, SDL_PIXELFORMAT_ARGB8888)) == NULL)
		return NULL;

	SDL_Rect rct = {.w = side, .h = side};
	if(SDL_BlitScaled(old, NULL, new, &rct) != 0) {
		SDL_FreeSurface(new);
		SDL_FreeSurface(old);
		return NULL;
	}

	SDL_FreeSurface(old);
	return new;
}

static void FreeLayerData(struct sMapLayerData *layer) {
	if(layer->altitude) SDL_free(layer->altitude);
	if(layer->color) SDL_free(layer->color);
	SDL_memset(layer, 0, sizeof(*layer));
}

static int LoadLayer(const char *diffuse, const char *height, struct sMapLayerData *out) {
	SDL_Surface *sDiffuse = NULL, *sHeight = NULL;
	int ret = ERROR_OK;
	SDL_memset(out, 0, sizeof(*out));

#ifndef USE_SDL_IMAGE
	sDiffuse = ReencodeSurface(SDL_LoadBMP(diffuse));
	sHeight = ReencodeSurface(SDL_LoadBMP(height));
#else
	sDiffuse = ReencodeSurface(IMG_Load(diffuse));
	sHeight = ReencodeSurface(IMG_Load(height));
#endif

	if(sDiffuse == NULL || sHeight == NULL) {
		ret = ERROR_MAPLOAD_FILE;
		goto cleanup;
	}

	if((sDiffuse->w != sDiffuse->h) || (sHeight->w != sHeight->h)) {
		ret = ERROR_MAPLOAD_IMGSIZE;
		goto cleanup;
	}

	if((sHeight->w & (sHeight->w - 1)) != 0) {
		ret = ERROR_MAPLOAD_WIDTHINVALID;
		goto cleanup;
	}

	if(sHeight->w < sDiffuse->w) {
		sHeight = ScaleSurface(sHeight, sDiffuse->w);
		if(sHeight == NULL) {
			ret = ERROR_MAPLOAD_SCALE;
			goto cleanup;
		}
	} else if(sDiffuse->w < sHeight->w) {
		ret = ERROR_MAPLOAD_MAPSMISMATCH;
		goto cleanup;
	}

	out->width = sHeight->w;
	out->height = sHeight->h;
	out->shift = (int)(SDL_log10(sHeight->w) / SDL_log10(2));
	out->color = SDL_calloc(4, sHeight->w * sHeight->h);
	out->altitude = SDL_calloc(1, sHeight->w * sHeight->h);
	if(!out->color || !out->altitude) {
		ret = ERROR_MALLOC_FAIL;
		goto cleanup;
	}

	const unsigned char *datah = sHeight->pixels;
	unsigned int *datac = sDiffuse->pixels;
	for(int i = 0; i < sHeight->w * sHeight->h; i++) {
		out->color[i] = datac[i];
		out->altitude[i] = datah[i << 2];
	}

cleanup:
	if(sDiffuse) SDL_FreeSurface(sDiffuse);
	if(sHeight) SDL_FreeSurface(sHeight);
	if(ret != ERROR_OK)
		FreeLayerData(out);
	return ret;
}

int Map_OpenDual(Map *map, const char *diffuse, const char *height, const char *ceilingDiffuse, const char *ceilingHeight) {
	struct sMapLayerData floorLayer, ceilingLayer;
	int ret;

	if((ret = LoadLayer(diffuse, height, &floorLayer)) != ERROR_OK)
		return ret;
	if((ret = LoadLayer(ceilingDiffuse, ceilingHeight, &ceilingLayer)) != ERROR_OK) {
		FreeLayerData(&floorLayer);
		return ret;
	}

	Map_Close(map);
	map->width = floorLayer.width;
	map->height = floorLayer.height;
	map->shift = floorLayer.shift;
	map->color = floorLayer.color;
	map->altitude = floorLayer.altitude;
	map->ready = 1;

	map->ceilingWidth = ceilingLayer.width;
	map->ceilingHeight = ceilingLayer.height;
	map->ceilingShift = ceilingLayer.shift;
	map->ceilingColor = ceilingLayer.color;
	map->ceilingAltitude = ceilingLayer.altitude;
	map->ceilingReady = 1;
	map->ceilingEnabled = 1;
	map->redraw = 1;
	return ERROR_OK;
}

int Map_Open(Map *map, const char *diffuse, const char *height) {
	return Map_OpenDual(map, diffuse, height, diffuse, height);
}

static inline void PrepareToDraw(SDL_Texture *screen, int **pixels, int *pitch, int *height) {
	SDL_LockTexture(screen, NULL, (void **)pixels, pitch);
	SDL_QueryTexture(screen, NULL, NULL, NULL, height);
	*pitch /= sizeof(int);

	// Fill screen with a single color
	for(int i = 0; i < (*pitch) * (*height); i++)
		(*pixels)[i] = 0x9090E0FF;
}

static inline void DrawVerticalLine(int *pixels, int pitch, int x, int top, int bottom, int color) {
	// Clamp to screen bounds.
	if(top < 0) top = 0;
	if(top > bottom) return;

	int offset = ((top * pitch) + x);
	for(int i = top; i < bottom; i++) {
		pixels[offset] = color;
		offset += pitch;
	}
}

static inline int MapOffsetForPoint(float x, float y, int width, int height, int shift) {
	return (((int)y & (width - 1)) << shift) + ((int)x & (height - 1));
}

static void DrawFromTo(Map *map, Camera *cam, int *pixels, int pitch, int start, int end) {
	if(!map->ready) return;
	const int drawCeiling = map->ceilingReady && map->ceilingEnabled;
	const float scale = cam->maxhorizon / 2.5f;
	const float sinang = SDL_sinf(cam->angle);
	const float cosang = SDL_cosf(cam->angle);
	const float camHeight = cam->height;
	const float camHorizon = cam->horizon;
	const float ceilingBase = map->ceilingBase;
	float deltaz = 1.0f;

	for(float z = 1.0f; z < cam->distance; z += deltaz) {
		const float invz = scale / z;
		int hasVisibleColumns = 0;
		Point pLeft = {-cosang * z - sinang * z, sinang * z - cosang * z},
		pRight = {cosang * z - sinang * z, -sinang * z - cosang * z},
		pDelta = {
			(pRight.x - pLeft.x) / (float)pitch,
			(pRight.y - pLeft.y) / (float)pitch
		};
		pLeft.x += pDelta.x * start;
		pLeft.y += pDelta.y * start;
		POINT_ADD(pLeft, cam->position);
		for(int i = start; i < end; i++) {
			if(drawCeiling) {
				if(map->showny[i] >= map->hiddeny[i]) {
					POINT_ADD(pLeft, pDelta);
					continue;
				}
			} else if(map->hiddeny[i] <= 0) {
				POINT_ADD(pLeft, pDelta);
				continue;
			}
			hasVisibleColumns = 1;

			if(drawCeiling) {
				const int cOffset = MapOffsetForPoint(
					pLeft.x, pLeft.y,
					map->ceilingWidth, map->ceilingHeight, map->ceilingShift
				);
				const int cBottom = (int)(
					(camHeight - (ceilingBase - (float)map->ceilingAltitude[cOffset]))
					* invz + camHorizon
				);
				const int cTop = map->showny[i];
				const int cDrawBottom = min(cBottom, map->hiddeny[i]);
				DrawVerticalLine(pixels, pitch, i, cTop, cDrawBottom, map->ceilingColor[cOffset]);
				if(cBottom > map->showny[i])
					map->showny[i] = min(cBottom, map->hiddeny[i]);
			}

			const int offset = MapOffsetForPoint(pLeft.x, pLeft.y, map->width, map->height, map->shift);
			const int floorTop = (int)((camHeight - (float)map->altitude[offset]) * invz + camHorizon);
			const int drawFloorTop = drawCeiling ? max(floorTop, map->showny[i]) : floorTop;
			DrawVerticalLine(pixels, pitch, i, drawFloorTop, map->hiddeny[i], map->color[offset]);
			/*
				Slightly speed up rendering by hiding
				overlapping line segments.
			*/
			if(floorTop < map->hiddeny[i])
				map->hiddeny[i] = (int)floorTop;
			POINT_ADD(pLeft, pDelta);
		}
		if(!hasVisibleColumns)
			break;
		deltaz += cam->zstep;
		if(map->optimize && z > map->optdist)
			deltaz += cam->zstep * (z / 2.0f);
	}
}

#ifndef USE_THREADED_RENDER
void Map_Draw(Map *map, Camera *cam) {
	if(!map->redraw) return;
	int *pixels = NULL, pitch = 0, height = 0;
	PrepareToDraw((SDL_Texture *)map->screen, &pixels, &pitch, &height);
	for(int i = 0; i < pitch; i++) {
		map->hiddeny[i] = height;
		map->showny[i] = 0;
	}
	DrawFromTo(map, cam, pixels, pitch, 0, pitch);

	SDL_UnlockTexture((SDL_Texture *)map->screen);
	map->redraw = 0;
}
#else
#include <SDL_cpuinfo.h>

static int RenderThread(void *ptr) {
	struct sMapRenderCtx *ctx = (struct sMapRenderCtx *)ptr;
	struct SMapRenderGlobCtx *gctx = ctx->global;
	SDL_mutex *mtx = SDL_CreateMutex();

	while(1) {
		SDL_LockMutex(mtx);
		SDL_CondWait(gctx->unlockcond, mtx);
		if(gctx->endwork) break;
		DrawFromTo(
			gctx->self, gctx->cam,
			gctx->pixels, gctx->pitch,
			ctx->start, ctx->end
		);
		SDL_SemPost(ctx->semaphore);
		SDL_UnlockMutex(mtx);
	}

	SDL_DestroyMutex(mtx);
	SDL_SemPost(ctx->semaphore);
	return 0;
}

void Map_Draw(Map *map, Camera *cam) {
	if(!map->redraw) return;
	int *pixels = NULL, pitch = 0, height = 0, failed = 0;
	PrepareToDraw((SDL_Texture *)map->screen, &pixels, &pitch, &height);
	if(map->ready) {
		map->rgctx.cam = cam;
		map->rgctx.pixels = pixels;
		map->rgctx.pitch = pitch;
		for(int i = 0; i < pitch; i++) {
			map->hiddeny[i] = height;
			map->showny[i] = 0;
		}
		SDL_CondBroadcast(map->rgctx.unlockcond);
		for(int i = 0; i < map->rctxcnt && !failed; i++)
			failed = SDL_SemWaitTimeout(map->rctxs[i].semaphore, 600) != 0;
	}

	SDL_UnlockTexture((SDL_Texture *)map->screen);
	if(failed) {
		SDL_LogWarn(0, "Failed to draw map, retrying...");
		return;
	}
	map->redraw = 0;
}

static void DestroyThreads(Map *map) {
	if(map->rctxs) {
		map->rgctx.endwork = 1;
		SDL_CondBroadcast(map->rgctx.unlockcond);
		for(int i = 0; i < map->rctxcnt; i++) {
			SDL_sem *sem = map->rctxs[i].semaphore;
			SDL_SemWait(sem);
			SDL_DestroySemaphore(sem);
		}

		SDL_free(map->rctxs);
		map->rctxs = NULL;
	}
	if(map->rgctx.unlockcond) {
		SDL_DestroyCond(map->rgctx.unlockcond);
		map->rgctx.unlockcond = NULL;
	}
	if(map->hiddeny) {
		SDL_free(map->hiddeny);
		map->hiddeny = NULL;
	}
	if(map->showny) {
		SDL_free(map->showny);
		map->showny = NULL;
	}
}
#endif

void Map_SetScreen(Map *map, void *screen) {
#ifdef USE_THREADED_RENDER
	DestroyThreads(map);
#else
	if(map->hiddeny) {
		SDL_free(map->hiddeny);
		map->hiddeny = NULL;
	}
	if(map->showny) {
		SDL_free(map->showny);
		map->showny = NULL;
	}
#endif
	map->screen = screen;
	if(!screen) return;
	int width = 0;
	if(SDL_QueryTexture(screen, NULL, NULL, &width, NULL) == 0) {
		map->hiddeny = SDL_calloc(4, width);
		map->showny = SDL_calloc(4, width);
		map->redraw = 1;
		if(!map->hiddeny || !map->showny) {
			SDL_LogCritical(0, "Failed to allocate screen line buffers");
			exit(1);
		}
#ifdef USE_THREADED_RENDER
		map->rgctx.self = map;
		map->rgctx.endwork = 0;
		map->rgctx.unlockcond = SDL_CreateCond();
		// If this value was not set externally, pick a default.
		if(!map->rctxcnt) map->rctxcnt = max(SDL_GetCPUCount() - 1, 1);
		map->rctxs = SDL_calloc(map->rctxcnt, sizeof(struct sMapRenderCtx));

		int perthwidth = (width / map->rctxcnt) + 1;
		for(int i = 0; i < map->rctxcnt; i++) {
			struct sMapRenderCtx *ctx = &map->rctxs[i];
			ctx->semaphore = SDL_CreateSemaphore(0);
			ctx->global = &map->rgctx;
			ctx->start = i * perthwidth;
			ctx->end = min(ctx->start + perthwidth, width);
			ctx->self = SDL_CreateThread(RenderThread, NULL, ctx);
		}
#endif
	} else {
		SDL_LogCritical(0, "Failed to query screen texture");
		exit(1);
	}
}

void Map_Close(Map *map) {
	if(map->altitude) {
		SDL_free(map->altitude);
		map->altitude = NULL;
	}
	if(map->color) {
		SDL_free(map->color);
		map->color = NULL;
	}
	if(map->ceilingAltitude) {
		SDL_free(map->ceilingAltitude);
		map->ceilingAltitude = NULL;
	}
	if(map->ceilingColor) {
		SDL_free(map->ceilingColor);
		map->ceilingColor = NULL;
	}

	map->ready = 0;
	map->ceilingReady = 0;
	map->ceilingEnabled = 0;
	map->width = 0;
	map->height = 0;
	map->shift = 0;
	map->ceilingWidth = 0;
	map->ceilingHeight = 0;
	map->ceilingShift = 0;
}
