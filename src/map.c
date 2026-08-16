#include <SDL_render.h>
#include <SDL_log.h>
#ifdef USE_THREADED_RENDER
#include <SDL_timer.h>
#endif
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
	const size_t pixelCount = (size_t)sHeight->w * (size_t)sHeight->h;
	out->color = SDL_calloc(pixelCount, sizeof(*out->color));
	out->altitude = SDL_calloc(pixelCount, sizeof(*out->altitude));
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

static inline void PrepareToDraw(Map *map, int **pixels, int *pitch, int *height) {
	SDL_LockTexture((SDL_Texture *)map->screen, NULL, (void **)pixels, pitch);
	SDL_QueryTexture((SDL_Texture *)map->screen, NULL, NULL, NULL, height);
	*pitch /= sizeof(int);
	const size_t depthCount = (size_t)map->depthWidth * (size_t)map->depthHeight;
	for(size_t i = 0; i < depthCount; i++)
		map->depth[i] = CAMERA_DISTANCE_MAX + 1.0f;

	// Fill screen with a single color.
#ifdef USE_AVX2
	const size_t count = (size_t)(*pitch) * (size_t)(*height);
	if(map->useAVX2) {
		const __m256i color = _mm256_set1_epi32(GRAPHICS_SKY_COLOR);
		size_t i = 0;
		for(; i + 8 <= count; i += 8)
			_mm256_storeu_si256((__m256i *)(void *)(*pixels + i), color);
		for(; i < count; i++)
			(*pixels)[i] = GRAPHICS_SKY_COLOR;
		return;
	}
#endif
	SDL_memset4(*pixels, GRAPHICS_SKY_COLOR, (size_t)(*pitch) * (size_t)(*height));
}

static inline float SmoothStep(float value) {
	value = max(0.0f, min(value, 1.0f));
	return value * value * (3.0f - (2.0f * value));
}

static inline int DistanceFogAmount(float z, float distance) {
	const float start = distance * CAMERA_FOG_START_RATIO;
	const float end = distance * CAMERA_FOG_END_RATIO;
	if(z <= start) return 0;
	if(z >= end) return 256;
	return (int)(SmoothStep((z - start) / (end - start)) * 256.0f);
}

static inline int ApplyDistanceFog(int color, int amount) {
	if(amount <= 0) return color;
	if(amount >= 256) return GRAPHICS_SKY_COLOR;

	const unsigned int from = (unsigned int)color;
	const unsigned int to = GRAPHICS_SKY_COLOR;
	const unsigned int inverse = 256u - (unsigned int)amount;
	const unsigned int rb = (
		(((from & 0x00ff00ffu) * inverse) + ((to & 0x00ff00ffu) * (unsigned int)amount)) >> 8
	) & 0x00ff00ffu;
	const unsigned int ag = (
		(((((from >> 8) & 0x00ff00ffu) * inverse) +
		(((to >> 8) & 0x00ff00ffu) * (unsigned int)amount)) >> 8) & 0x00ff00ffu
	) << 8;
	return (int)(rb | ag);
}

static inline float SampleHeightBilinear(
	const unsigned char *altitude, float x, float y, int mask, int shift
) {
	int x0 = (int)x;
	int y0 = (int)y;
	float fx = x - (float)x0;
	float fy = y - (float)y0;
	if(fx < 0.0f) { x0--; fx += 1.0f; }
	if(fy < 0.0f) { y0--; fy += 1.0f; }

	const int x1 = x0 + 1;
	const int y1 = y0 + 1;
	const float h00 = (float)altitude[((y0 & mask) << shift) + (x0 & mask)];
	const float h10 = (float)altitude[((y0 & mask) << shift) + (x1 & mask)];
	const float h01 = (float)altitude[((y1 & mask) << shift) + (x0 & mask)];
	const float h11 = (float)altitude[((y1 & mask) << shift) + (x1 & mask)];
	const float top = h00 + (h10 - h00) * fx;
	const float bottom = h01 + (h11 - h01) * fx;
	return top + (bottom - top) * fy;
}

static inline float NextDepthStep(
	float current, float z, float distance, float zstep,
	int optimize, float optdist
) {
	current += zstep;
	const float lodStart = distance * CAMERA_LOD_START_RATIO;
	if(z > lodStart) {
		const float lod = SmoothStep((z - lodStart) / (distance - lodStart));
		current += zstep * (z / 2.0f) * CAMERA_LOD_STRENGTH * lod;
	}
	if(optimize && z > optdist)
		current += zstep * (z / 2.0f);
	return current;
}

static inline void DrawVerticalLine(
	Map *map, int *pixels, int pitch, int x, int top, int bottom, int color, float depth
) {
	// Clamp to screen bounds.
	if(top < 0) top = 0;
	if(top > bottom) return;

	int offset = (top * pitch) + x;
	int depthOffset = (top * map->depthWidth) + x;
	for(int i = top; i < bottom; i++) {
		pixels[offset] = color;
		map->depth[depthOffset] = depth;
		offset += pitch;
		depthOffset += map->depthWidth;
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
		const int fogAmount = DistanceFogAmount(z, camDistance);
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
			const float sampledHeight = SampleHeightBilinear(
				altitude, px, py, mapMask, mapShift);
			const int floorTop = (int)((camHeight - sampledHeight) * invz + camHorizon);
			DrawVerticalLine(
				map, pixels, pitch, i, floorTop, h,
				ApplyDistanceFog(color[offset], fogAmount), z
			);
			if(floorTop < h)
				hiddeny[i] = floorTop;
		}
		if(!hasVisibleColumns)
			break;
		deltaz = NextDepthStep(deltaz, z, camDistance, zstep, optimize, optdist);
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
		const int fogAmount = DistanceFogAmount(z, camDistance);
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

			int offsets[8], tops[8], hiddens[8], colors[8];
			float sampleXs[8], sampleYs[8], alts[8];
			_mm256_storeu_si256((__m256i *)(void *)offsets, offsetv);
			_mm256_storeu_si256((__m256i *)(void *)hiddens, hiddenv);
			_mm256_storeu_si256((__m256i *)(void *)colors, colorv);
			_mm256_storeu_ps(sampleXs, lanePx);
			_mm256_storeu_ps(sampleYs, lanePy);
			for(int lane = 0; lane < 8; lane++)
				alts[lane] = SampleHeightBilinear(
					altitude, sampleXs[lane], sampleYs[lane], mapMask, mapShift);

			const __m256 altv = _mm256_loadu_ps(alts);
			const __m256 floorTopf = _mm256_add_ps(
				_mm256_mul_ps(_mm256_sub_ps(camHeightv, altv), invzv),
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
				DrawVerticalLine(
					map, pixels, pitch, col, top, hidden,
					ApplyDistanceFog(colors[lane], fogAmount), z
				);
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
			const float sampledHeight = SampleHeightBilinear(
				altitude, lanePx, lanePy, mapMask, mapShift);
			const int floorTop = (int)((camHeight - sampledHeight) * invz + camHorizon);
			DrawVerticalLine(
				map, pixels, pitch, i, floorTop, h,
				ApplyDistanceFog(color[offset], fogAmount), z
			);
			if(floorTop < h)
				hiddeny[i] = floorTop;
		}

		if(!hasVisibleColumns)
			break;
		deltaz = NextDepthStep(deltaz, z, camDistance, zstep, optimize, optdist);
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
	const int sameLayout = floorMask == ceilMask && floorShift == ceilShift;
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
		const int fogAmount = DistanceFogAmount(z, camDistance);
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

			const int sampleX = (int)px;
			const int sampleY = (int)py;
			const int cOffset = (((sampleY & ceilMask) << ceilShift) + (sampleX & ceilMask));
			const float sampledCeilingHeight = SampleHeightBilinear(
				ceilingAltitude, px, py, ceilMask, ceilShift);
			const int cBottom = (int)((camHeight - (ceilingBase - sampledCeilingHeight)) * invz + camHorizon);
			DrawVerticalLine(
				map, pixels, pitch, i, s, min(cBottom, h),
				ApplyDistanceFog(ceilingColor[cOffset], fogAmount), z
			);
			if(cBottom > s)
				s = min(cBottom, h);
			showny[i] = s;
			if(s >= h)
				continue;

			const int fOffset = sameLayout ? cOffset :
				(((sampleY & floorMask) << floorShift) + (sampleX & floorMask));
			const float sampledFloorHeight = SampleHeightBilinear(
				floorAltitude, px, py, floorMask, floorShift);
			const int floorTop = (int)((camHeight - sampledFloorHeight) * invz + camHorizon);
			DrawVerticalLine(
				map, pixels, pitch, i, max(floorTop, s), h,
				ApplyDistanceFog(floorColor[fOffset], fogAmount), z
			);
			if(floorTop < h)
				hiddeny[i] = floorTop;
		}
		if(!hasVisibleColumns)
			break;
		deltaz = NextDepthStep(deltaz, z, camDistance, zstep, optimize, optdist);
	}
}

static inline void DrawFromTo(Map *map, Camera *cam, int *pixels, int pitch, int start, int end) {
	if(!map->ready) return;
	if(map->ceilingReady && map->ceilingEnabled) {
		DrawFromToFloorAndCeiling(map, cam, pixels, pitch, start, end);
	} else {
#ifdef USE_AVX2
		if(map->useAVX2)
			DrawFromToFloorOnlyAVX2(map, cam, pixels, pitch, start, end);
		else
#endif
			DrawFromToFloorOnly(map, cam, pixels, pitch, start, end);
	}
}

static inline void ResetColumnBounds(Map *map, int pitch, int height) {
#ifdef USE_AVX2
	if(map->useAVX2) {
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
	PrepareToDraw(map, &pixels, &pitch, &height);
	ResetColumnBounds(map, pitch, height);
	DrawFromTo(map, cam, pixels, pitch, 0, pitch);

	SDL_UnlockTexture((SDL_Texture *)map->screen);
	map->redraw = 0;
}
#else

static void RenderChunks(struct SMapRenderGlobCtx *gctx) {
	while(1) {
		const int start = SDL_AtomicAdd(&gctx->nextColumn, gctx->chunkWidth);
		if(start >= gctx->pitch) break;
		DrawFromTo(
			gctx->self, gctx->cam,
			gctx->pixels, gctx->pitch,
			start, min(start + gctx->chunkWidth, gctx->pitch)
		);
	}
}

static int SpinForRenderWork(struct SMapRenderGlobCtx *gctx, int generation) {
	const Uint64 deadline = SDL_GetPerformanceCounter() + gctx->workerSpinTicks;
	do {
		for(int i = 0; i < RENDER_SPIN_CHECK_INTERVAL; i++)
			SDL_CPUPauseInstruction();
		if(SDL_AtomicGet(&gctx->endwork) ||
			SDL_AtomicGet(&gctx->generation) != generation)
			return 1;
	} while(SDL_GetPerformanceCounter() < deadline);
	return 0;
}

static void WaitForRenderWork(struct SMapRenderGlobCtx *gctx, int generation, int workerIndex) {
	if(workerIndex < RENDER_HOT_WORKERS && SpinForRenderWork(gctx, generation))
		return;

	SDL_LockMutex(gctx->mutex);
	if(!SDL_AtomicGet(&gctx->endwork) &&
		SDL_AtomicGet(&gctx->generation) == generation) {
		SDL_AtomicAdd(&gctx->sleepingWorkers, 1);
		while(!SDL_AtomicGet(&gctx->endwork) &&
			SDL_AtomicGet(&gctx->generation) == generation)
			SDL_CondWait(gctx->workcond, gctx->mutex);
		SDL_AtomicAdd(&gctx->sleepingWorkers, -1);
	}
	SDL_UnlockMutex(gctx->mutex);
}

static void SpinForWorkerCompletion(struct SMapRenderGlobCtx *gctx) {
	const Uint64 deadline = SDL_GetPerformanceCounter() + gctx->completionSpinTicks;
	do {
		if(SDL_AtomicGet(&gctx->workersPending) == 0)
			return;
		for(int i = 0; i < RENDER_SPIN_CHECK_INTERVAL; i++)
			SDL_CPUPauseInstruction();
	} while(SDL_GetPerformanceCounter() < deadline);
}

static int RenderThread(void *ptr) {
	struct sMapRenderCtx *ctx = (struct sMapRenderCtx *)ptr;
	struct SMapRenderGlobCtx *gctx = ctx->global;
	int generation = SDL_AtomicGet(&gctx->generation);

	while(1) {
		WaitForRenderWork(gctx, generation, ctx->index);
		if(SDL_AtomicGet(&gctx->endwork))
			break;
		generation = SDL_AtomicGet(&gctx->generation);
		SDL_MemoryBarrierAcquire();

		RenderChunks(gctx);

		const int remaining = SDL_AtomicAdd(&gctx->workersPending, -1) - 1;
		if(remaining == 0) {
			SDL_LockMutex(gctx->mutex);
			SDL_CondSignal(gctx->donecond);
			SDL_UnlockMutex(gctx->mutex);
		}
	}

	return 0;
}

void Map_Draw(Map *map, Camera *cam) {
	if(!map->redraw) return;
	int *pixels = NULL, pitch = 0, height = 0;
	PrepareToDraw(map, &pixels, &pitch, &height);
	if(map->ready) {
		ResetColumnBounds(map, pitch, height);
		if(map->rctxcnt > 0) {
			SDL_LockMutex(map->rgctx.mutex);
			map->rgctx.cam = cam;
			map->rgctx.pixels = pixels;
			map->rgctx.pitch = pitch;
			SDL_AtomicSet(&map->rgctx.workersPending, map->rctxcnt);
			SDL_AtomicSet(&map->rgctx.nextColumn, 0);
			SDL_MemoryBarrierRelease();
			SDL_AtomicAdd(&map->rgctx.generation, 1);
			if(SDL_AtomicGet(&map->rgctx.sleepingWorkers) > 0)
				SDL_CondBroadcast(map->rgctx.workcond);
			SDL_UnlockMutex(map->rgctx.mutex);

			// The main thread also consumes chunks instead of waiting idle.
			RenderChunks(&map->rgctx);
			SpinForWorkerCompletion(&map->rgctx);

			SDL_LockMutex(map->rgctx.mutex);
			while(SDL_AtomicGet(&map->rgctx.workersPending) > 0)
				SDL_CondWait(map->rgctx.donecond, map->rgctx.mutex);
			SDL_UnlockMutex(map->rgctx.mutex);
		} else {
			DrawFromTo(map, cam, pixels, pitch, 0, pitch);
		}
	}

	SDL_UnlockTexture((SDL_Texture *)map->screen);
	map->redraw = 0;
}

static void DestroyThreads(Map *map) {
	if(map->rctxs) {
		SDL_LockMutex(map->rgctx.mutex);
		SDL_AtomicSet(&map->rgctx.endwork, 1);
		SDL_AtomicAdd(&map->rgctx.generation, 1);
		SDL_CondBroadcast(map->rgctx.workcond);
		SDL_UnlockMutex(map->rgctx.mutex);

		for(int i = 0; i < map->rctxcnt; i++) {
			SDL_WaitThread(map->rctxs[i].self, NULL);
			map->rctxs[i].self = NULL;
		}

		SDL_free(map->rctxs);
		map->rctxs = NULL;
	}
	if(map->rgctx.donecond) {
		SDL_DestroyCond(map->rgctx.donecond);
		map->rgctx.donecond = NULL;
	}
	if(map->rgctx.workcond) {
		SDL_DestroyCond(map->rgctx.workcond);
		map->rgctx.workcond = NULL;
	}
	if(map->rgctx.mutex) {
		SDL_DestroyMutex(map->rgctx.mutex);
		map->rgctx.mutex = NULL;
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
	if(map->depth) {
		SDL_free(map->depth);
		map->depth = NULL;
	}
	map->depthWidth = 0;
	map->depthHeight = 0;
	map->screen = screen;
	if(!screen) return;
	int width = 0, height = 0;
	if(SDL_QueryTexture(screen, NULL, NULL, &width, &height) == 0) {
#ifdef USE_AVX2
		map->useAVX2 = SDL_HasAVX2();
#endif
		map->hiddeny = SDL_calloc(4, width);
		map->showny = SDL_calloc(4, width);
		map->depth = SDL_malloc((size_t)width * (size_t)height * sizeof(*map->depth));
		map->depthWidth = width;
		map->depthHeight = height;
		map->redraw = 1;
		if(!map->hiddeny || !map->showny || !map->depth) {
			SDL_LogCritical(0, "Failed to allocate screen line buffers");
			exit(1);
		}
#ifdef USE_THREADED_RENDER
		map->rgctx.self = map;
		SDL_AtomicSet(&map->rgctx.endwork, 0);
		SDL_AtomicSet(&map->rgctx.generation, 0);
		SDL_AtomicSet(&map->rgctx.workersPending, 0);
		SDL_AtomicSet(&map->rgctx.sleepingWorkers, 0);
		const Uint64 performanceFrequency = SDL_GetPerformanceFrequency();
		map->rgctx.workerSpinTicks =
			(performanceFrequency * RENDER_WORKER_SPIN_US) / 1000000u;
		map->rgctx.completionSpinTicks =
			(performanceFrequency * RENDER_COMPLETION_SPIN_US) / 1000000u;
		// Keep enough jobs to balance uneven columns without repeating too much setup.
		map->rgctx.chunkWidth = width <= 1024 ? 24 : 32;
		map->rgctx.mutex = SDL_CreateMutex();
		map->rgctx.workcond = SDL_CreateCond();
		map->rgctx.donecond = SDL_CreateCond();
		// If this value was not set externally, pick a default.
		if(map->rctxcnt <= 0) map->rctxcnt = max(SDL_GetCPUCount() - 1, 1);
		map->rctxcnt = min(map->rctxcnt, max(width / map->rgctx.chunkWidth, 1));

		if(map->rgctx.mutex && map->rgctx.workcond && map->rgctx.donecond)
			map->rctxs = SDL_calloc(map->rctxcnt, sizeof(struct sMapRenderCtx));
		if(!map->rctxs) {
			SDL_LogWarn(0, "Failed to initialize render threads; using the main thread");
			map->rctxcnt = 0;
		} else {
			const int requestedThreads = map->rctxcnt;
			map->rctxcnt = 0;
			for(int i = 0; i < requestedThreads; i++) {
				struct sMapRenderCtx *ctx = &map->rctxs[i];
				ctx->global = &map->rgctx;
				ctx->index = i;
				ctx->self = SDL_CreateThread(RenderThread, "voxel-render", ctx);
				if(!ctx->self) {
					SDL_LogWarn(0, "Failed to create render thread: %s", SDL_GetError());
					break;
				}
				map->rctxcnt++;
			}
		}
#endif
	} else {
		SDL_LogCritical(0, "Failed to query screen texture");
		exit(1);
	}
}

float *Map_GetDepthBuffer(Map *map, int *width, int *height) {
	if(width) *width = map->depthWidth;
	if(height) *height = map->depthHeight;
	return map->depth;
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
