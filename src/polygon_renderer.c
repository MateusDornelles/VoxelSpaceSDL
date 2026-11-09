#include "polygon_renderer.h"
#include "defines.h"
#include <SDL_render.h>
#include <SDL_stdinc.h>
#include <SDL_log.h>
#include <SDL_cpuinfo.h>
#ifdef USE_AVX2
#include <immintrin.h>
#endif
#include <float.h>
#include <stddef.h>
#ifdef USE_THREADED_RENDER
#include <SDL_atomic.h>
#include <SDL_thread.h>
#endif

#define POLYGON_TILE_SIZE 32

#ifdef USE_OPENGL_RENDER
#define GL_GLEXT_PROTOTYPES
#include <SDL_opengl.h>

typedef struct sGLPolygonVertex {
	float position[3], uv[2], color[4], textured;
} GLPolygonVertex;
#endif

typedef struct sProjectedVertex {
	float x, y, inverseDepth, uOverDepth, vOverDepth;
} ProjectedVertex;

typedef struct sProjectedTriangle {
	ProjectedVertex vertex[3];
	const PolygonMaterial *material;
	float area;
	int minX, maxX, minY, maxY;
} ProjectedTriangle;

typedef struct sPolygonCollider {
	float minX, minY, minZ;
	float maxX, maxY, maxZ;
} PolygonCollider;

static struct {
	PolygonTriangle *triangles;
	int triangleCount, triangleCapacity;
	PolygonCollider *colliders;
	int colliderCount, colliderCapacity;
	ProjectedTriangle *projected;
	int projectedCount, projectedCapacity;
	Uint32 checker[64];
	PolygonMaterial flatMaterial;
	PolygonMaterial texturedMaterial;
#ifdef USE_AVX2
	int useAVX2;
#endif
#ifdef USE_THREADED_RENDER
	SDL_Thread **workers;
	int *workerIndices;
	int workerCount;
	SDL_mutex *workerMutex;
	SDL_cond *workerCondition, *workerDone;
	SDL_atomic_t workerGeneration, nextTile, nextPrepare, workersPending, endWorkers;
	struct {
		ProjectedTriangle *tris;
		int count, capacity;
	} *prepareScratch;
	int prepareScratchCount;
	struct {
		int phase; /* 1 = prepare, 2 = raster */
		const PolygonTriangle *sourceTriangles;
		int sourceTriangleCount;
		Camera *camera;
		int width, height;
		Uint32 *pixels;
		float *depth;
		int pitch, tilesX, tileCount;
	} job;
#endif
#ifdef USE_OPENGL_RENDER
	GLuint program, vao, vbo, texture;
	const Uint32 *uploadedTexture;
	int uploadedTextureWidth, uploadedTextureHeight;
	GLPolygonVertex *batchVertices;
	int batchCapacity;
	GLuint staticVao, staticVbo, staticTex;
	int staticVertexCount;
	int staticSourceTriangleCount;
	const Uint32 *staticTexture;
	int staticTextureWidth, staticTextureHeight;
	const Uint32 *staticUploadedPixels;
	int staticUploadedWidth, staticUploadedHeight;
#endif
} polygons;

#ifdef USE_THREADED_RENDER
static void InitPolygonWorkers(Map *map);
static void DestroyPolygonWorkers(void);
#endif

static int ReserveTriangles(int required) {
	if(required <= polygons.triangleCapacity) return 1;
	int capacity = polygons.triangleCapacity ? polygons.triangleCapacity * 2 : 64;
	while(capacity < required) capacity *= 2;
	void *memory = SDL_realloc(polygons.triangles, (size_t)capacity * sizeof(*polygons.triangles));
	if(!memory) return 0;
	polygons.triangles = memory;
	polygons.triangleCapacity = capacity;
	return 1;
}

static int ReserveColliders(int required) {
	if(required <= polygons.colliderCapacity) return 1;
	int capacity = polygons.colliderCapacity ? polygons.colliderCapacity * 2 : 32;
	while(capacity < required) capacity *= 2;
	void *memory = SDL_realloc(polygons.colliders, (size_t)capacity * sizeof(*polygons.colliders));
	if(!memory) return 0;
	polygons.colliders = memory;
	polygons.colliderCapacity = capacity;
	return 1;
}

int PolygonRenderer_AddColliderBox(float x, float y, float z, float width, float depth, float height) {
	if(width <= 0.0f || depth <= 0.0f || height <= 0.0f) return 0;
	if(!ReserveColliders(polygons.colliderCount + 1)) return 0;
	PolygonCollider *collider = &polygons.colliders[polygons.colliderCount++];
	collider->minX = x;
	collider->minY = y;
	collider->minZ = z;
	collider->maxX = x + width;
	collider->maxY = y + depth;
	collider->maxZ = z + height;
	return 1;
}

float PolygonRenderer_GetSupportHeight(float x, float y) {
	float best = -FLT_MAX;
	for(int i = 0; i < polygons.colliderCount; i++) {
		const PolygonCollider *c = &polygons.colliders[i];
		if(x < c->minX || x > c->maxX || y < c->minY || y > c->maxY)
			continue;
		if(c->maxZ > best)
			best = c->maxZ;
	}
	return best;
}

void PolygonRenderer_ResolvePosition(float *x, float *y, float feetZ, float headZ, float radius) {
	if(!x || !y || radius <= 0.0f || headZ <= feetZ) return;
	const float skin = 0.05f;
	for(int pass = 0; pass < INPUT_COLLISION_ITERATIONS; pass++) {
		for(int i = 0; i < polygons.colliderCount; i++) {
			const PolygonCollider *c = &polygons.colliders[i];
			if(headZ <= c->minZ + skin || feetZ >= c->maxZ - skin)
				continue;

			const float minX = c->minX - radius;
			const float maxX = c->maxX + radius;
			const float minY = c->minY - radius;
			const float maxY = c->maxY + radius;
			if(*x <= minX || *x >= maxX || *y <= minY || *y >= maxY)
				continue;

			const float left = *x - minX;
			const float right = maxX - *x;
			const float bottom = *y - minY;
			const float top = maxY - *y;
			const float nearest = min(min(left, right), min(bottom, top));
			if(nearest == left) *x = minX;
			else if(nearest == right) *x = maxX;
			else if(nearest == bottom) *y = minY;
			else *y = maxY;
		}
	}
}

static void AddTriangleVertices(
	PolygonVertex a, PolygonVertex b, PolygonVertex c, const PolygonMaterial *material
) {
	if(!ReserveTriangles(polygons.triangleCount + 1)) return;
	PolygonTriangle *triangle = &polygons.triangles[polygons.triangleCount++];
	triangle->vertex[0] = a;
	triangle->vertex[1] = b;
	triangle->vertex[2] = c;
	triangle->material = material;
}

static void AddQuad(
	PolygonVertex a, PolygonVertex b, PolygonVertex c, PolygonVertex d,
	const PolygonMaterial *material
) {
	a.u = 0.0f; a.v = 0.0f; b.u = 1.0f; b.v = 0.0f;
	c.u = 1.0f; c.v = 1.0f; d.u = 0.0f; d.v = 1.0f;
	AddTriangleVertices(a, b, c, material);
	AddTriangleVertices(a, c, d, material);
}

static void AddBox(float x, float y, float z, float width, float depth, float height,
	const PolygonMaterial *material) {
	const float x1 = x + width, y1 = y + depth, z1 = z + height;
	const PolygonVertex p000 = {x, y, z, 0, 0}, p100 = {x1, y, z, 0, 0};
	const PolygonVertex p110 = {x1, y1, z, 0, 0}, p010 = {x, y1, z, 0, 0};
	const PolygonVertex p001 = {x, y, z1, 0, 0}, p101 = {x1, y, z1, 0, 0};
	const PolygonVertex p111 = {x1, y1, z1, 0, 0}, p011 = {x, y1, z1, 0, 0};
	AddQuad(p000, p100, p101, p001, material);
	AddQuad(p110, p010, p011, p111, material);
	AddQuad(p010, p000, p001, p011, material);
	AddQuad(p100, p110, p111, p101, material);
	AddQuad(p001, p101, p111, p011, material);
	AddQuad(p010, p110, p100, p000, material);
	PolygonRenderer_AddColliderBox(x, y, z, width, depth, height);
}

void PolygonRenderer_BuildingsPathFromHeightmap(const char *heightmap, char *out, size_t outSize) {
	if(!out || outSize == 0) return;
	out[0] = '\0';
	if(!heightmap || !heightmap[0]) return;
	SDL_strlcpy(out, heightmap, outSize);
	char *dot = SDL_strrchr(out, '.');
	char *slash = SDL_strrchr(out, '/');
	char *bslash = SDL_strrchr(out, '\\');
	char *sep = slash > bslash ? slash : bslash;
	if(dot && (!sep || dot > sep))
		*dot = '\0';
	SDL_strlcat(out, ".buildings", outSize);
}

int PolygonRenderer_LoadBuildings(Map *map, const char *path) {
	if(!map || !map->ready || !path || !path[0]) return 0;
	SDL_RWops *rw = SDL_RWFromFile(path, "r");
	if(!rw) {
		SDL_LogWarn(0, "Building list not found: %s", path);
		return 0;
	}

	int buildingCount = 0;
	const Sint64 size = SDL_RWsize(rw);
	if(size <= 0 || size > 1 << 20) {
		SDL_RWclose(rw);
		return 0;
	}
	char *text = SDL_malloc((size_t)size + 1);
	if(!text) {
		SDL_RWclose(rw);
		return 0;
	}
	if(SDL_RWread(rw, text, 1, (size_t)size) != (size_t)size) {
		SDL_free(text);
		SDL_RWclose(rw);
		return 0;
	}
	text[size] = '\0';
	SDL_RWclose(rw);

	char *cursor = text;
	while(*cursor) {
		char *lineStart = cursor;
		while(*cursor && *cursor != '\n' && *cursor != '\r') cursor++;
		const char end = *cursor;
		*cursor = '\0';
		if(end == '\r' && cursor[1] == '\n') cursor += 2;
		else if(end) cursor++;

		while(*lineStart == ' ' || *lineStart == '\t') lineStart++;
		if(!*lineStart || *lineStart == '#') continue;

		float x = 0, y = 0, width = 0, depth = 0, height = 0;
		int materialId = 0;
		if(SDL_sscanf(lineStart, "%f %f %f %f %f %d",
			&x, &y, &width, &depth, &height, &materialId) < 5)
			continue;
		if(width <= 0.0f || depth <= 0.0f || height <= 0.0f) continue;

		Point sample = {x + width * 0.5f, y + depth * 0.5f};
		const float base = (float)Map_GetHeight(map, &sample);
		const PolygonMaterial *material = materialId ?
			&polygons.texturedMaterial : &polygons.flatMaterial;
		AddBox(x, y, base, width, depth, height, material);
		buildingCount++;
	}

	SDL_free(text);
	SDL_Log("Loaded %d polygon buildings from %s", buildingCount, path);
	map->redraw = 1;
	return buildingCount;
}

void PolygonRenderer_Init(Map *map) {
	polygons.triangleCount = 0;
	polygons.colliderCount = 0;
#ifdef USE_AVX2
	polygons.useAVX2 = SDL_HasAVX2();
#endif
	for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++)
		polygons.checker[y * 8 + x] = ((x ^ y) & 1) ? 0xffd8b060u : 0xff704020u;
	polygons.flatMaterial = (PolygonMaterial){POLYGON_MATERIAL_FLAT, 0xff4080d0u, NULL, 0, 0, 0};
	polygons.texturedMaterial = (PolygonMaterial){POLYGON_MATERIAL_TEXTURED, 0xffffffffu,
		polygons.checker, 8, 8, 0};
#ifdef USE_THREADED_RENDER
	InitPolygonWorkers(map);
#else
	(void)map;
#endif
}

void PolygonRenderer_Destroy(void) {
#ifdef USE_THREADED_RENDER
	DestroyPolygonWorkers();
#endif
	SDL_free(polygons.triangles);
	SDL_free(polygons.colliders);
	SDL_free(polygons.projected);
	polygons.triangles = NULL;
	polygons.colliders = NULL;
	polygons.projected = NULL;
	polygons.triangleCount = 0;
	polygons.triangleCapacity = 0;
	polygons.colliderCount = 0;
	polygons.colliderCapacity = 0;
	polygons.projectedCount = polygons.projectedCapacity = 0;
}

void PolygonRenderer_Clear(void) {
	polygons.triangleCount = 0;
	polygons.colliderCount = 0;
}

int PolygonRenderer_AddTriangle(const PolygonTriangle *triangle) {
	if(!triangle || !triangle->material || !ReserveTriangles(polygons.triangleCount + 1))
		return 0;
	if(triangle->material->type == POLYGON_MATERIAL_TEXTURED &&
		(!triangle->material->texture || triangle->material->textureWidth <= 0 ||
		 triangle->material->textureHeight <= 0)) return 0;
	polygons.triangles[polygons.triangleCount++] = *triangle;
	return 1;
}

#define POLYGON_NEAR_DEPTH 1.0f

typedef struct sCameraSpaceVertex {
	float lateral, height, depth, u, v;
} CameraSpaceVertex;

static CameraSpaceVertex ToCameraSpace(const PolygonVertex *vertex, const Camera *camera) {
	const float sine = SDL_sinf(camera->angle), cosine = SDL_cosf(camera->angle);
	const float dx = vertex->x - camera->position.x;
	const float dy = vertex->y - camera->position.y;
	return (CameraSpaceVertex){
		.lateral = (cosine * dx) - (sine * dy),
		.height = camera->height - vertex->z,
		.depth = (-sine * dx) - (cosine * dy),
		.u = vertex->u,
		.v = vertex->v
	};
}

static CameraSpaceVertex LerpCameraSpace(CameraSpaceVertex a, CameraSpaceVertex b, float t) {
	return (CameraSpaceVertex){
		.lateral = a.lateral + (b.lateral - a.lateral) * t,
		.height = a.height + (b.height - a.height) * t,
		.depth = a.depth + (b.depth - a.depth) * t,
		.u = a.u + (b.u - a.u) * t,
		.v = a.v + (b.v - a.v) * t
	};
}

/* Clip polygon to depth >= near. Returns 0, 3 or 4 vertices. */
static int ClipPolygonNear(
	const CameraSpaceVertex *input, int inputCount,
	CameraSpaceVertex *output, float nearDepth
) {
	CameraSpaceVertex previous = input[inputCount - 1];
	int previousInside = previous.depth >= nearDepth;
	int outputCount = 0;
	for(int i = 0; i < inputCount; i++) {
		const CameraSpaceVertex current = input[i];
		const int currentInside = current.depth >= nearDepth;
		if(currentInside != previousInside) {
			const float denom = current.depth - previous.depth;
			const float t = SDL_fabsf(denom) > 1e-8f ?
				(nearDepth - previous.depth) / denom : 0.0f;
			output[outputCount++] = LerpCameraSpace(previous, current, t);
		}
		if(currentInside)
			output[outputCount++] = current;
		previous = current;
		previousInside = currentInside;
	}
	return outputCount;
}

static float Edge(float ax, float ay, float bx, float by, float px, float py) {
	return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void ProjectCameraVertex(
	const CameraSpaceVertex *vertex, int width, int height, float horizon,
	ProjectedVertex *projected
) {
	const float scale = (float)height / CAMERA_PROJECTION_SCALE;
	const float depth = vertex->depth;
	projected->x = ((vertex->lateral / depth) + 1.0f) * (float)width * 0.5f;
	projected->y = (vertex->height * scale / depth) + horizon;
	projected->inverseDepth = 1.0f / depth;
	projected->uOverDepth = vertex->u / depth;
	projected->vOverDepth = vertex->v / depth;
}

static int EmitProjectedTriangle(
	const CameraSpaceVertex *a, const CameraSpaceVertex *b, const CameraSpaceVertex *c,
	const PolygonMaterial *material, const Camera *camera, int width, int height,
	ProjectedTriangle *out, int *outCount, int outCapacity
) {
	if(a->depth >= camera->distance && b->depth >= camera->distance && c->depth >= camera->distance)
		return 0;
	if(*outCount >= outCapacity)
		return 0;
	ProjectedTriangle *triangle = &out[*outCount];
	ProjectCameraVertex(a, width, height, camera->horizon, &triangle->vertex[0]);
	ProjectCameraVertex(b, width, height, camera->horizon, &triangle->vertex[1]);
	ProjectCameraVertex(c, width, height, camera->horizon, &triangle->vertex[2]);
	ProjectedVertex *p = triangle->vertex;
	triangle->area = Edge(p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
	if(SDL_fabsf(triangle->area) < 0.0001f) return 0;
	triangle->minX = max(0, (int)SDL_floorf(min(p[0].x, min(p[1].x, p[2].x))));
	triangle->maxX = min(width - 1, (int)SDL_ceilf(max(p[0].x, max(p[1].x, p[2].x))));
	triangle->minY = max(0, (int)SDL_floorf(min(p[0].y, min(p[1].y, p[2].y))));
	triangle->maxY = min(height - 1, (int)SDL_ceilf(max(p[0].y, max(p[1].y, p[2].y))));
	if(triangle->minX > triangle->maxX || triangle->minY > triangle->maxY) return 0;
	triangle->material = material;
	(*outCount)++;
	return 1;
}

static int TriangleFacesCamera(const PolygonTriangle *triangle, const Camera *camera) {
	if(triangle->material->doubleSided) return 1;
	const PolygonVertex *a=&triangle->vertex[0],*b=&triangle->vertex[1],*c=&triangle->vertex[2];
	const float abx=b->x-a->x,aby=b->y-a->y,abz=b->z-a->z;
	const float acx=c->x-a->x,acy=c->y-a->y,acz=c->z-a->z;
	const float nx=aby*acz-abz*acy,ny=abz*acx-abx*acz,nz=abx*acy-aby*acx;
	const float viewX=camera->position.x-a->x,viewY=camera->position.y-a->y;
	const float viewZ=camera->height-a->z;
	return nx*viewX+ny*viewY+nz*viewZ>0.0f;
}

static Uint32 SampleMaterial(const PolygonMaterial *material, float u, float v) {
	if(material->type == POLYGON_MATERIAL_FLAT || !material->texture)
		return material->color;
	u -= SDL_floorf(u); v -= SDL_floorf(v);
	const int x = min((int)(u * material->textureWidth), material->textureWidth - 1);
	const int y = min((int)(v * material->textureHeight), material->textureHeight - 1);
	return material->texture[y * material->textureWidth + x];
}

#define POLYGON_PREPARE_CHUNK 512
#define POLYGON_JOB_PREPARE 1
#define POLYGON_JOB_RASTER 2

static int EnsureProjectedCapacity(ProjectedTriangle **tris, int *capacity, int required) {
	if(required <= *capacity)
		return 1;
	int next = *capacity ? *capacity * 2 : 64;
	while(next < required)
		next *= 2;
	void *memory = SDL_realloc(*tris, (size_t)next * sizeof(**tris));
	if(!memory)
		return 0;
	*tris = memory;
	*capacity = next;
	return 1;
}

/* Near clipping can split one source triangle into two projected triangles. */
static void PrepareTriangleRange(
	Camera *camera, int width, int height,
	const PolygonTriangle *triangles, int begin, int end,
	ProjectedTriangle **outTris, int *outCount, int *outCapacity
) {
	if(begin >= end)
		return;
	if(!EnsureProjectedCapacity(outTris, outCapacity, *outCount + (end - begin) * 2))
		return;
	for(int index = begin; index < end; index++) {
		const PolygonTriangle *source = &triangles[index];
		if(!source->material || !TriangleFacesCamera(source, camera))
			continue;

		CameraSpaceVertex eye[3] = {
			ToCameraSpace(&source->vertex[0], camera),
			ToCameraSpace(&source->vertex[1], camera),
			ToCameraSpace(&source->vertex[2], camera)
		};
		if(eye[0].depth < POLYGON_NEAR_DEPTH &&
			eye[1].depth < POLYGON_NEAR_DEPTH &&
			eye[2].depth < POLYGON_NEAR_DEPTH)
			continue;

		CameraSpaceVertex clipped[4];
		const int clipCount = ClipPolygonNear(eye, 3, clipped, POLYGON_NEAR_DEPTH);
		if(clipCount < 3)
			continue;
		EmitProjectedTriangle(
			&clipped[0], &clipped[1], &clipped[2],
			source->material, camera, width, height,
			*outTris, outCount, *outCapacity
		);
		if(clipCount == 4)
			EmitProjectedTriangle(
				&clipped[0], &clipped[2], &clipped[3],
				source->material, camera, width, height,
				*outTris, outCount, *outCapacity
			);
	}
}

static inline void RasterizePixel(const ProjectedTriangle *triangle,
	Uint32 *pixels, float *depthBuffer, int pitch, int depthWidth, int x, int y) {
	const ProjectedVertex *p = triangle->vertex;
	const float px = (float)x + 0.5f, py = (float)y + 0.5f;
	const float w0 = Edge(p[1].x, p[1].y, p[2].x, p[2].y, px, py) / triangle->area;
	const float w1 = Edge(p[2].x, p[2].y, p[0].x, p[0].y, px, py) / triangle->area;
	const float w2 = 1.0f - w0 - w1;
	if(w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) return;
	const float inverseDepth = w0 * p[0].inverseDepth + w1 * p[1].inverseDepth + w2 * p[2].inverseDepth;
	const float depth = 1.0f / inverseDepth;
	const int pixelOffset = y * pitch + x, depthOffset = y * depthWidth + x;
	if(depth >= depthBuffer[depthOffset]) return;
	const float u = (w0 * p[0].uOverDepth + w1 * p[1].uOverDepth + w2 * p[2].uOverDepth) / inverseDepth;
	const float v = (w0 * p[0].vOverDepth + w1 * p[1].vOverDepth + w2 * p[2].vOverDepth) / inverseDepth;
	pixels[pixelOffset] = SampleMaterial(triangle->material, u, v);
	depthBuffer[depthOffset] = depth;
}

#ifdef USE_AVX2
static inline __m256 Edge8(const ProjectedVertex *a, const ProjectedVertex *b,
	__m256 px, __m256 py) {
	return _mm256_sub_ps(
		_mm256_mul_ps(_mm256_sub_ps(px, _mm256_set1_ps(a->x)), _mm256_set1_ps(b->y - a->y)),
		_mm256_mul_ps(_mm256_sub_ps(py, _mm256_set1_ps(a->y)), _mm256_set1_ps(b->x - a->x))
	);
}

static void RasterizeSpanAVX2(const ProjectedTriangle *triangle,
	Uint32 *pixels, float *depthBuffer, int pitch, int depthWidth,
	int y, int minX, int maxX) {
	const ProjectedVertex *p = triangle->vertex;
	const __m256 lane = _mm256_setr_ps(.5f,1.5f,2.5f,3.5f,4.5f,5.5f,6.5f,7.5f);
	const __m256 py = _mm256_set1_ps((float)y + .5f);
	const __m256 inverseArea = _mm256_set1_ps(1.0f / triangle->area);
	const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
	int x = minX;
	for(; x + 7 <= maxX; x += 8) {
		const __m256 px = _mm256_add_ps(_mm256_set1_ps((float)x), lane);
		const __m256 w0 = _mm256_mul_ps(Edge8(&p[1],&p[2],px,py),inverseArea);
		const __m256 w1 = _mm256_mul_ps(Edge8(&p[2],&p[0],px,py),inverseArea);
		const __m256 w2 = _mm256_sub_ps(one,_mm256_add_ps(w0,w1));
		__m256 mask = _mm256_and_ps(_mm256_cmp_ps(w0,zero,_CMP_GE_OQ),
			_mm256_and_ps(_mm256_cmp_ps(w1,zero,_CMP_GE_OQ),_mm256_cmp_ps(w2,zero,_CMP_GE_OQ)));
		if(!_mm256_movemask_ps(mask)) continue;
		const __m256 inverseDepth = _mm256_add_ps(
			_mm256_add_ps(_mm256_mul_ps(w0,_mm256_set1_ps(p[0].inverseDepth)),
				_mm256_mul_ps(w1,_mm256_set1_ps(p[1].inverseDepth))),
			_mm256_mul_ps(w2,_mm256_set1_ps(p[2].inverseDepth)));
		const __m256 depth = _mm256_div_ps(one,inverseDepth);
		float *depthAddress = depthBuffer + y * depthWidth + x;
		mask = _mm256_and_ps(mask,_mm256_cmp_ps(depth,_mm256_loadu_ps(depthAddress),_CMP_LT_OQ));
		if(!_mm256_movemask_ps(mask)) continue;
		__m256i colors;
		if(triangle->material->type == POLYGON_MATERIAL_TEXTURED && triangle->material->texture) {
			const __m256 uOverDepth = _mm256_add_ps(_mm256_add_ps(
				_mm256_mul_ps(w0,_mm256_set1_ps(p[0].uOverDepth)),_mm256_mul_ps(w1,_mm256_set1_ps(p[1].uOverDepth))),
				_mm256_mul_ps(w2,_mm256_set1_ps(p[2].uOverDepth)));
			const __m256 vOverDepth = _mm256_add_ps(_mm256_add_ps(
				_mm256_mul_ps(w0,_mm256_set1_ps(p[0].vOverDepth)),_mm256_mul_ps(w1,_mm256_set1_ps(p[1].vOverDepth))),
				_mm256_mul_ps(w2,_mm256_set1_ps(p[2].vOverDepth)));
			__m256 u=_mm256_div_ps(uOverDepth,inverseDepth),v=_mm256_div_ps(vOverDepth,inverseDepth);
			u=_mm256_sub_ps(u,_mm256_floor_ps(u));v=_mm256_sub_ps(v,_mm256_floor_ps(v));
			const __m256i tx=_mm256_cvttps_epi32(_mm256_mul_ps(u,_mm256_set1_ps((float)triangle->material->textureWidth)));
			const __m256i ty=_mm256_cvttps_epi32(_mm256_mul_ps(v,_mm256_set1_ps((float)triangle->material->textureHeight)));
			const __m256i indices=_mm256_add_epi32(tx,_mm256_mullo_epi32(ty,_mm256_set1_epi32(triangle->material->textureWidth)));
			colors=_mm256_i32gather_epi32((const int *)triangle->material->texture,indices,4);
		} else colors=_mm256_set1_epi32((int)triangle->material->color);
		const __m256i integerMask=_mm256_castps_si256(mask);
		_mm256_maskstore_epi32((int *)(pixels+y*pitch+x),integerMask,colors);
		_mm256_maskstore_ps(depthAddress,integerMask,depth);
	}
	for(;x<=maxX;x++) RasterizePixel(triangle,pixels,depthBuffer,pitch,depthWidth,x,y);
}
#endif

static void RasterizeTile(Uint32 *pixels, float *depthBuffer, int pitch, int depthWidth,
	int tileMinX, int tileMinY, int tileMaxX, int tileMaxY) {
	for(int index = 0; index < polygons.projectedCount; index++) {
		const ProjectedTriangle *triangle = &polygons.projected[index];
		const int minX = max(tileMinX, triangle->minX), maxX = min(tileMaxX, triangle->maxX);
		const int minY = max(tileMinY, triangle->minY), maxY = min(tileMaxY, triangle->maxY);
		if(minX > maxX || minY > maxY) continue;
		for(int y = minY; y <= maxY; y++) {
#ifdef USE_AVX2
			if(polygons.useAVX2) RasterizeSpanAVX2(triangle,pixels,depthBuffer,pitch,depthWidth,y,minX,maxX);
			else
#endif
				for(int x=minX;x<=maxX;x++) RasterizePixel(triangle,pixels,depthBuffer,pitch,depthWidth,x,y);
		}
	}
}

#ifdef USE_THREADED_RENDER
static void ConsumePrepareRanges(int scratchIndex) {
	while(1) {
		const int begin = SDL_AtomicAdd(&polygons.nextPrepare, POLYGON_PREPARE_CHUNK);
		if(begin >= polygons.job.sourceTriangleCount)
			return;
		const int end = min(begin + POLYGON_PREPARE_CHUNK, polygons.job.sourceTriangleCount);
		PrepareTriangleRange(
			polygons.job.camera, polygons.job.width, polygons.job.height,
			polygons.job.sourceTriangles, begin, end,
			&polygons.prepareScratch[scratchIndex].tris,
			&polygons.prepareScratch[scratchIndex].count,
			&polygons.prepareScratch[scratchIndex].capacity
		);
	}
}

static void ConsumePolygonTiles(void) {
	while(1) {
		const int tile = SDL_AtomicAdd(&polygons.nextTile, 1);
		if(tile >= polygons.job.tileCount) return;
		const int tileX = tile % polygons.job.tilesX, tileY = tile / polygons.job.tilesX;
		const int minX = tileX * POLYGON_TILE_SIZE, minY = tileY * POLYGON_TILE_SIZE;
		RasterizeTile(polygons.job.pixels, polygons.job.depth, polygons.job.pitch,
			polygons.job.width, minX, minY,
			min(minX + POLYGON_TILE_SIZE - 1, polygons.job.width - 1),
			min(minY + POLYGON_TILE_SIZE - 1, polygons.job.height - 1));
	}
}

static void RunPolygonJob(int scratchIndex) {
	SDL_MemoryBarrierAcquire();
	if(polygons.job.phase == POLYGON_JOB_PREPARE)
		ConsumePrepareRanges(scratchIndex);
	else
		ConsumePolygonTiles();
}

static int PolygonWorker(void *arg) {
	const int workerIndex = *(const int *)arg;
	int generation = SDL_AtomicGet(&polygons.workerGeneration);
	while(1) {
		SDL_LockMutex(polygons.workerMutex);
		while(!SDL_AtomicGet(&polygons.endWorkers) &&
			SDL_AtomicGet(&polygons.workerGeneration) == generation)
			SDL_CondWait(polygons.workerCondition, polygons.workerMutex);
		SDL_UnlockMutex(polygons.workerMutex);
		if(SDL_AtomicGet(&polygons.endWorkers)) break;
		generation = SDL_AtomicGet(&polygons.workerGeneration);
		RunPolygonJob(workerIndex);
		if(SDL_AtomicAdd(&polygons.workersPending, -1) - 1 == 0) {
			SDL_LockMutex(polygons.workerMutex);
			SDL_CondSignal(polygons.workerDone);
			SDL_UnlockMutex(polygons.workerMutex);
		}
	}
	return 0;
}

static int MergePrepareScratches(void) {
	int total = 0;
	for(int i = 0; i < polygons.prepareScratchCount; i++)
		total += polygons.prepareScratch[i].count;
	if(!EnsureProjectedCapacity(&polygons.projected, &polygons.projectedCapacity, total))
		return 0;
	int offset = 0;
	for(int i = 0; i < polygons.prepareScratchCount; i++) {
		const int count = polygons.prepareScratch[i].count;
		if(count > 0) {
			SDL_memcpy(
				polygons.projected + offset,
				polygons.prepareScratch[i].tris,
				(size_t)count * sizeof(*polygons.projected)
			);
			offset += count;
			polygons.prepareScratch[i].count = 0;
		}
	}
	polygons.projectedCount = total;
	return 1;
}

static int PrepareProjectedTrianglesParallel(
	Camera *camera, int width, int height,
	const PolygonTriangle *triangles, int triangleCount
) {
	for(int i = 0; i < polygons.prepareScratchCount; i++)
		polygons.prepareScratch[i].count = 0;

	polygons.job.phase = POLYGON_JOB_PREPARE;
	polygons.job.sourceTriangles = triangles;
	polygons.job.sourceTriangleCount = triangleCount;
	polygons.job.camera = camera;
	polygons.job.width = width;
	polygons.job.height = height;
	SDL_AtomicSet(&polygons.nextPrepare, 0);
	SDL_AtomicSet(&polygons.workersPending, polygons.workerCount);
	SDL_MemoryBarrierRelease();
	SDL_LockMutex(polygons.workerMutex);
	SDL_AtomicAdd(&polygons.workerGeneration, 1);
	SDL_CondBroadcast(polygons.workerCondition);
	SDL_UnlockMutex(polygons.workerMutex);

	/* Main thread uses the last scratch slot (workerCount). */
	RunPolygonJob(polygons.workerCount);

	SDL_LockMutex(polygons.workerMutex);
	while(SDL_AtomicGet(&polygons.workersPending) > 0)
		SDL_CondWait(polygons.workerDone, polygons.workerMutex);
	SDL_UnlockMutex(polygons.workerMutex);

	return MergePrepareScratches();
}

static void InitPolygonWorkers(Map *map) {
	const int availableWorkers = map->rctxcnt > 0 ? map->rctxcnt : max(SDL_GetCPUCount() - 1, 1);
	polygons.workerCount = min(availableWorkers, 8);
	polygons.workerMutex = SDL_CreateMutex();
	polygons.workerCondition = SDL_CreateCond();
	polygons.workerDone = SDL_CreateCond();
	if(!polygons.workerMutex || !polygons.workerCondition || !polygons.workerDone) {
		polygons.workerCount = 0;
		return;
	}
	polygons.workers = SDL_calloc((size_t)polygons.workerCount, sizeof(*polygons.workers));
	polygons.workerIndices = SDL_calloc((size_t)polygons.workerCount, sizeof(*polygons.workerIndices));
	/* One scratch per worker + one for the main thread. */
	polygons.prepareScratchCount = polygons.workerCount + 1;
	polygons.prepareScratch = SDL_calloc(
		(size_t)polygons.prepareScratchCount, sizeof(*polygons.prepareScratch));
	if(!polygons.workers || !polygons.workerIndices || !polygons.prepareScratch) {
		SDL_free(polygons.workers);
		SDL_free(polygons.workerIndices);
		SDL_free(polygons.prepareScratch);
		polygons.workers = NULL;
		polygons.workerIndices = NULL;
		polygons.prepareScratch = NULL;
		polygons.workerCount = 0;
		polygons.prepareScratchCount = 0;
		return;
	}
	SDL_AtomicSet(&polygons.endWorkers, 0);
	for(int i = 0; i < polygons.workerCount; i++) {
		polygons.workerIndices[i] = i;
		polygons.workers[i] = SDL_CreateThread(
			PolygonWorker, "polygon-render", &polygons.workerIndices[i]);
		if(!polygons.workers[i]) {
			polygons.workerCount = i;
			polygons.prepareScratchCount = polygons.workerCount + 1;
			break;
		}
	}
	SDL_Log("Polygon software renderer: %d worker thread(s), %dx%d tiles, parallel prepare",
		polygons.workerCount, POLYGON_TILE_SIZE, POLYGON_TILE_SIZE);
}

static void DestroyPolygonWorkers(void) {
	if(polygons.workerMutex) {
		SDL_LockMutex(polygons.workerMutex);SDL_AtomicSet(&polygons.endWorkers,1);
		SDL_AtomicAdd(&polygons.workerGeneration,1);SDL_CondBroadcast(polygons.workerCondition);SDL_UnlockMutex(polygons.workerMutex);
	}
	for(int i=0;i<polygons.workerCount;i++)if(polygons.workers[i])SDL_WaitThread(polygons.workers[i],NULL);
	SDL_free(polygons.workers);polygons.workers=NULL;
	SDL_free(polygons.workerIndices);polygons.workerIndices=NULL;
	for(int i = 0; i < polygons.prepareScratchCount; i++)
		SDL_free(polygons.prepareScratch[i].tris);
	SDL_free(polygons.prepareScratch);
	polygons.prepareScratch = NULL;
	polygons.prepareScratchCount = 0;
	polygons.workerCount=0;
	if(polygons.workerDone)SDL_DestroyCond(polygons.workerDone);if(polygons.workerCondition)SDL_DestroyCond(polygons.workerCondition);if(polygons.workerMutex)SDL_DestroyMutex(polygons.workerMutex);
	polygons.workerDone=polygons.workerCondition=NULL;polygons.workerMutex=NULL;
}
#endif

static int PrepareProjectedTriangles(
	Camera *camera, int width, int height,
	const PolygonTriangle *triangles, int triangleCount
) {
#ifdef USE_THREADED_RENDER
	if(polygons.workerCount > 0 && polygons.prepareScratchCount > 0)
		return PrepareProjectedTrianglesParallel(camera, width, height, triangles, triangleCount);
#endif
	polygons.projectedCount = 0;
	PrepareTriangleRange(
		camera, width, height, triangles, 0, triangleCount,
		&polygons.projected, &polygons.projectedCount, &polygons.projectedCapacity
	);
	return 1;
}

void PolygonRenderer_DrawTriangleList(
	Map *map, Camera *camera, const PolygonTriangle *triangles, int triangleCount
) {
	if(!map || !map->screen || !map->depth || !triangles || triangleCount <= 0) return;
	int width = 0, height = 0, pitchBytes = 0;
	Uint32 *pixels = NULL;
	SDL_QueryTexture((SDL_Texture *)map->screen, NULL, NULL, &width, &height);
	if(!PrepareProjectedTriangles(camera, width, height, triangles, triangleCount) ||
		polygons.projectedCount == 0)
		return;
	if(SDL_LockTexture((SDL_Texture *)map->screen, NULL, (void **)&pixels, &pitchBytes) != 0) return;
	const int pitch = pitchBytes / (int)sizeof(*pixels);
#ifdef USE_THREADED_RENDER
	if(polygons.workerCount > 0) {
		polygons.job.phase = POLYGON_JOB_RASTER;
		polygons.job.pixels=pixels;polygons.job.depth=map->depth;
		polygons.job.width=width;polygons.job.height=height;polygons.job.pitch=pitch;
		polygons.job.tilesX=(width+POLYGON_TILE_SIZE-1)/POLYGON_TILE_SIZE;
		polygons.job.tileCount=polygons.job.tilesX*((height+POLYGON_TILE_SIZE-1)/POLYGON_TILE_SIZE);
		SDL_AtomicSet(&polygons.nextTile,0);SDL_AtomicSet(&polygons.workersPending,polygons.workerCount);
		SDL_MemoryBarrierRelease();SDL_LockMutex(polygons.workerMutex);SDL_AtomicAdd(&polygons.workerGeneration,1);
		SDL_CondBroadcast(polygons.workerCondition);SDL_UnlockMutex(polygons.workerMutex);
		RunPolygonJob(0);
		SDL_LockMutex(polygons.workerMutex);while(SDL_AtomicGet(&polygons.workersPending)>0)SDL_CondWait(polygons.workerDone,polygons.workerMutex);SDL_UnlockMutex(polygons.workerMutex);
	} else
#endif
		RasterizeTile(pixels,map->depth,pitch,width,0,0,width-1,height-1);
	SDL_UnlockTexture((SDL_Texture *)map->screen);
}

void PolygonRenderer_DrawSoftware(Map *map, Camera *camera) {
	PolygonRenderer_DrawTriangleList(map, camera, polygons.triangles, polygons.triangleCount);
}

#ifdef USE_OPENGL_RENDER
static GLuint CompilePolygonShader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type); glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
	GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) { glDeleteShader(shader); return 0; }
	return shader;
}

static void BindPolygonVertexAttributes(void) {
	const GLsizei stride = (GLsizei)sizeof(GLPolygonVertex);
	for(int i = 0; i < 4; i++)
		glEnableVertexAttribArray(i);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(GLPolygonVertex, position));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(GLPolygonVertex, uv));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(GLPolygonVertex, color));
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(GLPolygonVertex, textured));
}

static void PackGLVertex(GLPolygonVertex *out, const PolygonVertex *vertex, const PolygonMaterial *material) {
	out->position[0] = vertex->x;
	out->position[1] = vertex->y;
	out->position[2] = vertex->z;
	out->uv[0] = vertex->u;
	out->uv[1] = vertex->v;
	/* Map/building colors are packed as 0xAARRGGBB in CPU memory (same as software). */
	const Uint32 c = material->color;
	out->color[0] = (float)((c >> 16) & 255) / 255.0f;
	out->color[1] = (float)((c >> 8) & 255) / 255.0f;
	out->color[2] = (float)(c & 255) / 255.0f;
	/* Heightmap texels often store alpha as 0; force opaque for the GL color path. */
	out->color[3] = 1.0f;
	out->textured = material->type == POLYGON_MATERIAL_TEXTURED ? 1.0f : 0.0f;
}

static int EnsureBatchCapacity(int required) {
	if(required <= polygons.batchCapacity)
		return 1;
	int capacity = polygons.batchCapacity ? polygons.batchCapacity * 2 : 1024;
	while(capacity < required)
		capacity *= 2;
	void *memory = SDL_realloc(polygons.batchVertices, (size_t)capacity * sizeof(*polygons.batchVertices));
	if(!memory)
		return 0;
	polygons.batchVertices = memory;
	polygons.batchCapacity = capacity;
	return 1;
}

static void BindPolygonProgram(Camera *camera, int width, int height) {
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	/* No GL_CULL_FACE: the custom clip projection can flip winding vs world-space
	   CCW, so hardware cull would drop the terrain static mesh. Buildings still
	   use CPU TriangleFacesCamera before batching. */
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glUseProgram(polygons.program);
	glUniform2f(glGetUniformLocation(polygons.program, "cameraPosition"),
		camera->position.x, camera->position.y);
	glUniform2f(glGetUniformLocation(polygons.program, "resolution"),
		(float)width, (float)height);
	glUniform1f(glGetUniformLocation(polygons.program, "cameraHeight"), camera->height);
	glUniform1f(glGetUniformLocation(polygons.program, "horizon"), camera->horizon);
	glUniform1f(glGetUniformLocation(polygons.program, "angle"), camera->angle);
	glUniform1f(glGetUniformLocation(polygons.program, "distance"), camera->distance);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, polygons.texture);
	glUniform1i(glGetUniformLocation(polygons.program, "polygonTexture"), 0);
}

static void UnbindPolygonProgram(void) {
	glBindVertexArray(0);
	glDisable(GL_DEPTH_TEST);
}

static void FlushPolygonBatch(int vertexCount) {
	if(vertexCount <= 0)
		return;
	glBufferData(GL_ARRAY_BUFFER,
		(GLsizeiptr)((size_t)vertexCount * sizeof(GLPolygonVertex)),
		polygons.batchVertices, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

int PolygonRenderer_InitOpenGL(void) {
	for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++)
		polygons.checker[y * 8 + x] = ((x ^ y) & 1) ? 0xffd8b060u : 0xff704020u;
	/* Match map.c / raymarch fog: smoothstep(distance*0.5833, distance*0.9667, z) → sky. */
	static const char *vs =
		"#version 330 core\n"
		"layout(location=0)in vec3 p;layout(location=1)in vec2 uv;"
		"layout(location=2)in vec4 color;layout(location=3)in float textured;"
		"uniform vec2 cameraPosition,resolution;"
		"uniform float cameraHeight,horizon,angle,distance;"
		"out vec2 fuv;out vec4 fc;flat out float ft;out float fz;"
		"void main(){"
		"float s=sin(angle),c=cos(angle);vec2 d=p.xy-cameraPosition;"
		"float z=-s*d.x-c*d.y,l=c*d.x-s*d.y;"
		"float ny=(1.-2.*horizon/resolution.y)*z-(2./2.5)*(cameraHeight-p.z);"
		"float n=1.,f=distance;"
		"gl_Position=vec4(l,ny,((f+n)/(f-n))*z-(2.*f*n/(f-n)),z);"
		"fuv=uv;fc=color;ft=textured;fz=z;}";
	static const char *fs =
		"#version 330 core\n"
		"uniform sampler2D polygonTexture;uniform float distance;"
		"in vec2 fuv;in vec4 fc;flat in float ft;in float fz;out vec4 outColor;"
		"vec3 fog(vec3 c,float z){"
		"float t=smoothstep(distance*.5833,distance*.9667,z);"
		"return mix(c,vec3(144.,224.,255.)/255.,t);}"
		"void main(){"
		"vec4 base=ft>.5?texture(polygonTexture,fuv)*fc:fc;"
		"outColor=vec4(fog(base.rgb,fz),base.a);}";
	GLuint vertex = CompilePolygonShader(GL_VERTEX_SHADER, vs), fragment = CompilePolygonShader(GL_FRAGMENT_SHADER, fs);
	if(!vertex || !fragment) return 1;
	polygons.program = glCreateProgram(); glAttachShader(polygons.program, vertex); glAttachShader(polygons.program, fragment); glLinkProgram(polygons.program);
	glDeleteShader(vertex); glDeleteShader(fragment);
	GLint linked = 0; glGetProgramiv(polygons.program, GL_LINK_STATUS, &linked); if(!linked) return 1;
	glGenVertexArrays(1, &polygons.vao); glGenBuffers(1, &polygons.vbo); glGenTextures(1, &polygons.texture);
	glBindVertexArray(polygons.vao); glBindBuffer(GL_ARRAY_BUFFER, polygons.vbo);
	BindPolygonVertexAttributes();
	glBindTexture(GL_TEXTURE_2D, polygons.texture); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,8,8,0,GL_BGRA,GL_UNSIGNED_BYTE,polygons.checker);
	polygons.uploadedTexture=polygons.checker;polygons.uploadedTextureWidth=8;polygons.uploadedTextureHeight=8;
	/* Fresh GL context: drop any stale static-mesh bookkeeping from a prior backend. */
	polygons.staticVao = polygons.staticVbo = polygons.staticTex = 0;
	polygons.staticVertexCount = 0;
	polygons.staticSourceTriangleCount = -1;
	polygons.staticTexture = NULL;
	polygons.staticTextureWidth = polygons.staticTextureHeight = 0;
	polygons.staticUploadedPixels = NULL;
	polygons.staticUploadedWidth = polygons.staticUploadedHeight = 0;
	return 0;
}

void PolygonRenderer_ClearStaticMeshOpenGL(void) {
	if(polygons.staticVbo)
		glDeleteBuffers(1, &polygons.staticVbo);
	if(polygons.staticVao)
		glDeleteVertexArrays(1, &polygons.staticVao);
	if(polygons.staticTex)
		glDeleteTextures(1, &polygons.staticTex);
	polygons.staticVbo = polygons.staticVao = polygons.staticTex = 0;
	polygons.staticVertexCount = 0;
	polygons.staticSourceTriangleCount = -1;
	polygons.staticTexture = NULL;
	polygons.staticTextureWidth = polygons.staticTextureHeight = 0;
	polygons.staticUploadedPixels = NULL;
	polygons.staticUploadedWidth = polygons.staticUploadedHeight = 0;
}

void PolygonRenderer_UploadStaticMeshOpenGL(
	const PolygonTriangle *triangles, int triangleCount
) {
	if(polygons.program && polygons.staticVao &&
		polygons.staticSourceTriangleCount == triangleCount)
		return;

	PolygonRenderer_ClearStaticMeshOpenGL();
	if(!polygons.program || !triangles || triangleCount <= 0)
		return;

	const int vertexCount = triangleCount * 3;
	GLPolygonVertex *vertices = SDL_malloc((size_t)vertexCount * sizeof(*vertices));
	if(!vertices)
		return;

	int out = 0;
	for(int i = 0; i < triangleCount; i++) {
		const PolygonTriangle *triangle = &triangles[i];
		if(!triangle->material)
			continue;
		if(!polygons.staticTexture &&
			triangle->material->type == POLYGON_MATERIAL_TEXTURED &&
			triangle->material->texture) {
			polygons.staticTexture = triangle->material->texture;
			polygons.staticTextureWidth = triangle->material->textureWidth;
			polygons.staticTextureHeight = triangle->material->textureHeight;
		}
		for(int j = 0; j < 3; j++)
			PackGLVertex(&vertices[out++], &triangle->vertex[j], triangle->material);
	}
	if(out <= 0) {
		SDL_free(vertices);
		return;
	}

	glGenVertexArrays(1, &polygons.staticVao);
	glGenBuffers(1, &polygons.staticVbo);
	glBindVertexArray(polygons.staticVao);
	glBindBuffer(GL_ARRAY_BUFFER, polygons.staticVbo);
	glBufferData(GL_ARRAY_BUFFER,
		(GLsizeiptr)((size_t)out * sizeof(*vertices)), vertices, GL_STATIC_DRAW);
	BindPolygonVertexAttributes();
	glBindVertexArray(0);
	polygons.staticVertexCount = out;
	polygons.staticSourceTriangleCount = triangleCount;
	SDL_free(vertices);
}

int PolygonRenderer_HasStaticMeshOpenGL(void) {
	return polygons.program && polygons.staticVao && polygons.staticVertexCount > 0;
}

void PolygonRenderer_DrawStaticMeshOpenGL(Camera *camera, int width, int height) {
	if(!PolygonRenderer_HasStaticMeshOpenGL())
		return;
	BindPolygonProgram(camera, width, height);
	/* Own texture object so building draws cannot force a 2048² re-upload every frame. */
	if(polygons.staticTexture) {
		if(!polygons.staticTex) {
			glGenTextures(1, &polygons.staticTex);
			glBindTexture(GL_TEXTURE_2D, polygons.staticTex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		} else
			glBindTexture(GL_TEXTURE_2D, polygons.staticTex);
		if(polygons.staticTexture != polygons.staticUploadedPixels ||
			polygons.staticTextureWidth != polygons.staticUploadedWidth ||
			polygons.staticTextureHeight != polygons.staticUploadedHeight) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
				polygons.staticTextureWidth, polygons.staticTextureHeight,
				0, GL_BGRA, GL_UNSIGNED_BYTE, polygons.staticTexture);
			polygons.staticUploadedPixels = polygons.staticTexture;
			polygons.staticUploadedWidth = polygons.staticTextureWidth;
			polygons.staticUploadedHeight = polygons.staticTextureHeight;
		}
	}
	glBindVertexArray(polygons.staticVao);
	glDrawArrays(GL_TRIANGLES, 0, polygons.staticVertexCount);
	UnbindPolygonProgram();
}

void PolygonRenderer_DrawTriangleListOpenGL(
	Camera *camera, int width, int height, const PolygonTriangle *triangles, int triangleCount
) {
	if(!triangles || triangleCount <= 0 || !polygons.program) return;
	BindPolygonProgram(camera, width, height);
	glBindVertexArray(polygons.vao);
	glBindBuffer(GL_ARRAY_BUFFER, polygons.vbo);

	int batchCount = 0;
	int batchTextured = -1;
	const Uint32 *batchTexture = NULL;
	int batchTextureWidth = 0, batchTextureHeight = 0;

	for(int i = 0; i < triangleCount; i++) {
		const PolygonTriangle *triangle = &triangles[i];
		if(!triangle->material)
			continue;
		if(!triangle->material->doubleSided && !TriangleFacesCamera(triangle, camera))
			continue;

		const int textured = triangle->material->type == POLYGON_MATERIAL_TEXTURED;
		const int textureChanged = textured && (
			triangle->material->texture != batchTexture ||
			triangle->material->textureWidth != batchTextureWidth ||
			triangle->material->textureHeight != batchTextureHeight);

		if(batchCount > 0 && (textured != batchTextured || textureChanged)) {
			FlushPolygonBatch(batchCount);
			batchCount = 0;
		}

		if(textured && (textureChanged || batchTexture == NULL)) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
				triangle->material->textureWidth, triangle->material->textureHeight,
				0, GL_BGRA, GL_UNSIGNED_BYTE, triangle->material->texture);
			polygons.uploadedTexture = triangle->material->texture;
			polygons.uploadedTextureWidth = triangle->material->textureWidth;
			polygons.uploadedTextureHeight = triangle->material->textureHeight;
			batchTexture = triangle->material->texture;
			batchTextureWidth = triangle->material->textureWidth;
			batchTextureHeight = triangle->material->textureHeight;
		}

		if(!EnsureBatchCapacity(batchCount + 3))
			break;
		for(int j = 0; j < 3; j++)
			PackGLVertex(&polygons.batchVertices[batchCount + j],
				&triangle->vertex[j], triangle->material);
		batchCount += 3;
		batchTextured = textured;
	}

	FlushPolygonBatch(batchCount);
	UnbindPolygonProgram();
}

void PolygonRenderer_DrawOpenGL(Camera *camera, int width, int height) {
	PolygonRenderer_DrawTriangleListOpenGL(camera, width, height, polygons.triangles, polygons.triangleCount);
}

void PolygonRenderer_DestroyOpenGL(void) {
	PolygonRenderer_ClearStaticMeshOpenGL();
	SDL_free(polygons.batchVertices);
	polygons.batchVertices = NULL;
	polygons.batchCapacity = 0;
	if(polygons.texture)glDeleteTextures(1,&polygons.texture);if(polygons.vbo)glDeleteBuffers(1,&polygons.vbo);if(polygons.vao)glDeleteVertexArrays(1,&polygons.vao);if(polygons.program)glDeleteProgram(polygons.program);
	polygons.texture=polygons.vbo=polygons.vao=polygons.program=0;
	polygons.uploadedTexture=NULL;polygons.uploadedTextureWidth=polygons.uploadedTextureHeight=0;
}
#endif
