#include <SDL_render.h>
#include <SDL_log.h>
#include <stdlib.h>
#ifdef USE_AVX2
#include <immintrin.h>
#include <SDL_cpuinfo.h>
#endif
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
#ifdef USE_AVX2
	const size_t count = (size_t)(*pitch) * (size_t)(*height);
	if(SDL_HasAVX2()) {
		const __m256i color = _mm256_set1_epi32(0x9090E0FF);
		size_t i = 0;
		for(; i + 8 <= count; i += 8)
			_mm256_storeu_si256((__m256i *)(void *)(*pixels + i), color);
		for(; i < count; i++)
			(*pixels)[i] = 0x9090E0FF;
		return;
	}
#endif
	SDL_memset4(*pixels, 0x9090E0FF, (size_t)(*pitch) * (size_t)(*height));
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

static void DrawFromToFloorOnly(Map *map, Camera *cam, int *pixels, int pitch, int start, int end) {
	const float scale = cam->maxhorizon / 2.5f;
	const float sinang = SDL_sinf(cam->angle);
	const float cosang = SDL_cosf(cam->angle);
	const float camHeight = cam->height;
	const float camHorizon = cam->horizon;
	const float camDistance = cam->distance;
	const float camPosX = cam->position.x;
	const float camPosY = cam->position.y;
	const float zstep = cam->zstep;

	const int mapMask = map->width - 1;
	const int mapShift = map->shift;
	const int optimize = map->optimize;
	const float optdist = map->optdist;
	int *const hiddeny = map->hiddeny;
	const int *const color = map->color;
	const unsigned char *const altitude = map->altitude;

	float deltaz = 1.0f;
	for(float z = 1.0f; z < camDistance; z += deltaz) {
		const float invz = scale / z;
		const float dx = (2.0f * cosang * z) / (float)pitch;
		const float dy = (-2.0f * sinang * z) / (float)pitch;
		float px = ((-cosang - sinang) * z) + camPosX + (dx * (float)start);
		float py = ((sinang - cosang) * z) + camPosY + (dy * (float)start);
		int hasVisibleColumns = 0;

		for(int i = start; i < end; i++, px += dx, py += dy) {
			const int h = hiddeny[i];
			if(h <= 0)
				continue;
			hasVisibleColumns = 1;

			const int offset = ((((int)py & mapMask) << mapShift) + ((int)px & mapMask));
			const int floorTop = (int)((camHeight - (float)altitude[offset]) * invz + camHorizon);
			DrawVerticalLine(pixels, pitch, i, floorTop, h, color[offset]);
			if(floorTop < h)
				hiddeny[i] = floorTop;
		}
		if(!hasVisibleColumns)
			break;
		deltaz += zstep;
		if(optimize && z > optdist)
			deltaz += zstep * (z / 2.0f);
	}
}

#ifdef USE_AVX2
static void DrawFromToFloorOnlyAVX2(Map *map, Camera *cam, int *pixels, int pitch, int start, int end) {
	const float scale = cam->maxhorizon / 2.5f;
	const float sinang = SDL_sinf(cam->angle);
	const float cosang = SDL_cosf(cam->angle);
	const float camHeight = cam->height;
	const float camHorizon = cam->horizon;
	const float camDistance = cam->distance;
	const float camPosX = cam->position.x;
	const float camPosY = cam->position.y;
	const float zstep = cam->zstep;

	const int mapMask = map->width - 1;
	const int mapShift = map->shift;
	const int optimize = map->optimize;
	const float optdist = map->optdist;
	int *const hiddeny = map->hiddeny;
	const int *const color = map->color;
	const unsigned char *const altitude = map->altitude;

	const __m256 lanes = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
	const __m256i zeroi = _mm256_setzero_si256();
	const __m256i mapMaskv = _mm256_set1_epi32(mapMask);
	const __m256i shiftv = _mm256_set1_epi32(mapShift);

	float deltaz = 1.0f;
	for(float z = 1.0f; z < camDistance; z += deltaz) {
		const float invz = scale / z;
		const float dx = (2.0f * cosang * z) / (float)pitch;
		const float dy = (-2.0f * sinang * z) / (float)pitch;
		float px = ((-cosang - sinang) * z) + camPosX + (dx * (float)start);
		float py = ((sinang - cosang) * z) + camPosY + (dy * (float)start);
		int hasVisibleColumns = 0;

		const __m256 dxv = _mm256_set1_ps(dx);
		const __m256 dyv = _mm256_set1_ps(dy);
		const __m256 invzv = _mm256_set1_ps(invz);
		const __m256 camHeightv = _mm256_set1_ps(camHeight);
		const __m256 camHorizonv = _mm256_set1_ps(camHorizon);

		int i = start;
		for(; i + 8 <= end; i += 8) {
			const __m256i hiddenv = _mm256_loadu_si256((const __m256i *)(const void *)(hiddeny + i));
			const __m256i visiblev = _mm256_cmpgt_epi32(hiddenv, zeroi);
			const int visibleMask = _mm256_movemask_ps(_mm256_castsi256_ps(visiblev));
			if(!visibleMask)
				continue;
			hasVisibleColumns = 1;

			const __m256 chunkBaseX = _mm256_set1_ps(px + dx * (float)(i - start));
			const __m256 chunkBaseY = _mm256_set1_ps(py + dy * (float)(i - start));
			const __m256 lanePx = _mm256_add_ps(chunkBaseX, _mm256_mul_ps(dxv, lanes));
			const __m256 lanePy = _mm256_add_ps(chunkBaseY, _mm256_mul_ps(dyv, lanes));
			const __m256i ix = _mm256_and_si256(_mm256_cvttps_epi32(lanePx), mapMaskv);
			const __m256i iy = _mm256_and_si256(_mm256_cvttps_epi32(lanePy), mapMaskv);
			const __m256i offsetv = _mm256_add_epi32(_mm256_sllv_epi32(iy, shiftv), ix);
			const __m256i colorv = _mm256_i32gather_epi32(color, offsetv, sizeof(int));

			int offsets[8], tops[8], hiddens[8], colors[8], alts[8];
			_mm256_storeu_si256((__m256i *)(void *)offsets, offsetv);
			_mm256_storeu_si256((__m256i *)(void *)hiddens, hiddenv);
			_mm256_storeu_si256((__m256i *)(void *)colors, colorv);
			for(int lane = 0; lane < 8; lane++)
				alts[lane] = (int)altitude[offsets[lane]];

			const __m256i altv = _mm256_setr_epi32(alts[0], alts[1], alts[2], alts[3], alts[4], alts[5], alts[6], alts[7]);
			const __m256 floorTopf = _mm256_add_ps(
				_mm256_mul_ps(_mm256_sub_ps(camHeightv, _mm256_cvtepi32_ps(altv)), invzv),
				camHorizonv
			);
			const __m256i floorTopv = _mm256_cvttps_epi32(floorTopf);
			_mm256_storeu_si256((__m256i *)(void *)tops, floorTopv);

			for(int lane = 0; lane < 8; lane++) {
				if((visibleMask & (1 << lane)) == 0)
					continue;
				const int col = i + lane;
				const int top = tops[lane];
				const int hidden = hiddens[lane];
				DrawVerticalLine(pixels, pitch, col, top, hidden, colors[lane]);
				if(top < hidden)
					hiddeny[col] = top;
			}
		}

		for(; i < end; i++) {
			const int h = hiddeny[i];
			if(h <= 0)
				continue;
			hasVisibleColumns = 1;

			const float lanePx = px + dx * (float)(i - start);
			const float lanePy = py + dy * (float)(i - start);
			const int offset = ((((int)lanePy & mapMask) << mapShift) + ((int)lanePx & mapMask));
			const int floorTop = (int)((camHeight - (float)altitude[offset]) * invz + camHorizon);
			DrawVerticalLine(pixels, pitch, i, floorTop, h, color[offset]);
			if(floorTop < h)
				hiddeny[i] = floorTop;
		}

		if(!hasVisibleColumns)
			break;
		deltaz += zstep;
		if(optimize && z > optdist)
			deltaz += zstep * (z / 2.0f);
	}
}
#endif

static void DrawFromToFloorAndCeiling(Map *map, Camera *cam, int *pixels, int pitch, int start, int end) {
	const float scale = cam->maxhorizon / 2.5f;
	const float sinang = SDL_sinf(cam->angle);
	const float cosang = SDL_cosf(cam->angle);
	const float camHeight = cam->height;
	const float camHorizon = cam->horizon;
	const float camDistance = cam->distance;
	const float camPosX = cam->position.x;
	const float camPosY = cam->position.y;
	const float zstep = cam->zstep;
	const float ceilingBase = map->ceilingBase;

	const int floorMask = map->width - 1;
	const int floorShift = map->shift;
	const int ceilMask = map->ceilingWidth - 1;
	const int ceilShift = map->ceilingShift;
	const int optimize = map->optimize;
	const float optdist = map->optdist;
	int *const hiddeny = map->hiddeny;
	int *const showny = map->showny;
	const int *const floorColor = map->color;
	const unsigned char *const floorAltitude = map->altitude;
	const int *const ceilingColor = map->ceilingColor;
	const unsigned char *const ceilingAltitude = map->ceilingAltitude;

	float deltaz = 1.0f;
	for(float z = 1.0f; z < camDistance; z += deltaz) {
		const float invz = scale / z;
		const float dx = (2.0f * cosang * z) / (float)pitch;
		const float dy = (-2.0f * sinang * z) / (float)pitch;
		float px = ((-cosang - sinang) * z) + camPosX + (dx * (float)start);
		float py = ((sinang - cosang) * z) + camPosY + (dy * (float)start);
		int hasVisibleColumns = 0;

		for(int i = start; i < end; i++, px += dx, py += dy) {
			const int h = hiddeny[i];
			int s = showny[i];
			if(s >= h)
				continue;
			hasVisibleColumns = 1;

			const int cOffset = ((((int)py & ceilMask) << ceilShift) + ((int)px & ceilMask));
			const int cBottom = (int)((camHeight - (ceilingBase - (float)ceilingAltitude[cOffset])) * invz + camHorizon);
			DrawVerticalLine(pixels, pitch, i, s, min(cBottom, h), ceilingColor[cOffset]);
			if(cBottom > s)
				s = min(cBottom, h);
			showny[i] = s;

			const int fOffset = ((((int)py & floorMask) << floorShift) + ((int)px & floorMask));
			const int floorTop = (int)((camHeight - (float)floorAltitude[fOffset]) * invz + camHorizon);
			DrawVerticalLine(pixels, pitch, i, max(floorTop, s), h, floorColor[fOffset]);
			if(floorTop < h)
				hiddeny[i] = floorTop;
		}
		if(!hasVisibleColumns)
			break;
		deltaz += zstep;
		if(optimize && z > optdist)
			deltaz += zstep * (z / 2.0f);
	}
}

static inline void DrawFromTo(Map *map, Camera *cam, int *pixels, int pitch, int start, int end) {
	if(!map->ready) return;
	if(map->ceilingReady && map->ceilingEnabled)
		DrawFromToFloorAndCeiling(map, cam, pixels, pitch, start, end);
	else
#ifdef USE_AVX2
	if(SDL_HasAVX2())
		DrawFromToFloorOnlyAVX2(map, cam, pixels, pitch, start, end);
	else
#endif
		DrawFromToFloorOnly(map, cam, pixels, pitch, start, end);
}

static inline void ResetColumnBounds(Map *map, int pitch, int height) {
#ifdef USE_AVX2
	if(SDL_HasAVX2()) {
		const __m256i hiddenv = _mm256_set1_epi32(height);
		const __m256i shownv = _mm256_setzero_si256();
		int i = 0;
		for(; i + 8 <= pitch; i += 8) {
			_mm256_storeu_si256((__m256i *)(void *)(map->hiddeny + i), hiddenv);
			_mm256_storeu_si256((__m256i *)(void *)(map->showny + i), shownv);
		}
		for(; i < pitch; i++) {
			map->hiddeny[i] = height;
			map->showny[i] = 0;
		}
		return;
	}
#endif
	for(int i = 0; i < pitch; i++) {
		map->hiddeny[i] = height;
		map->showny[i] = 0;
	}
}

#ifndef USE_THREADED_RENDER
void Map_Draw(Map *map, Camera *cam) {
	if(!map->redraw) return;
	int *pixels = NULL, pitch = 0, height = 0;
	PrepareToDraw((SDL_Texture *)map->screen, &pixels, &pitch, &height);
	ResetColumnBounds(map, pitch, height);
	DrawFromTo(map, cam, pixels, pitch, 0, pitch);

	SDL_UnlockTexture((SDL_Texture *)map->screen);
	map->redraw = 0;
}
#else

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
		ResetColumnBounds(map, pitch, height);
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
