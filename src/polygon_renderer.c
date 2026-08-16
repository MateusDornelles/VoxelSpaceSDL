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

static struct {
	PolygonTriangle *triangles;
	int triangleCount, triangleCapacity;
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
	int workerCount;
	SDL_mutex *workerMutex;
	SDL_cond *workerCondition, *workerDone;
	SDL_atomic_t workerGeneration, nextTile, workersPending, endWorkers;
	struct {
		Uint32 *pixels;
		float *depth;
		int width, height, pitch, tilesX, tileCount;
	} job;
#endif
#ifdef USE_OPENGL_RENDER
	GLuint program, vao, vbo, texture;
	const Uint32 *uploadedTexture;
	int uploadedTextureWidth, uploadedTextureHeight;
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
}

void PolygonRenderer_Init(Map *map) {
	polygons.triangleCount = 0;
#ifdef USE_AVX2
	polygons.useAVX2 = SDL_HasAVX2();
#endif
	for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++)
		polygons.checker[y * 8 + x] = ((x ^ y) & 1) ? 0xffd8b060u : 0xff704020u;
	polygons.flatMaterial = (PolygonMaterial){POLYGON_MATERIAL_FLAT, 0xff4080d0u, NULL, 0, 0};
	polygons.texturedMaterial = (PolygonMaterial){POLYGON_MATERIAL_TEXTURED, 0xffffffffu,
		polygons.checker, 8, 8};
	for(int index = 0; index < 10; index++) {
		const int column = index % 2;
		const int row = index / 2;
		Point flat = {410.0f + (float)column * 125.0f, 650.0f - (float)row * 65.0f};
		Point textured = {flat.x + 45.0f, flat.y};
		const float flatGround = (float)Map_GetHeight(map, &flat);
		const float texturedGround = (float)Map_GetHeight(map, &textured);
		AddBox(flat.x, flat.y, flatGround, 36.0f, 36.0f, 45.0f, &polygons.flatMaterial);
		AddBox(textured.x, textured.y, texturedGround, 36.0f, 36.0f, 45.0f,
			&polygons.texturedMaterial);
	}
#ifdef USE_THREADED_RENDER
	InitPolygonWorkers(map);
#endif
}

void PolygonRenderer_Destroy(void) {
#ifdef USE_THREADED_RENDER
	DestroyPolygonWorkers();
#endif
	SDL_free(polygons.triangles);
	SDL_free(polygons.projected);
	polygons.triangles = NULL;
	polygons.projected = NULL;
	polygons.triangleCount = 0;
	polygons.triangleCapacity = 0;
	polygons.projectedCount = polygons.projectedCapacity = 0;
}

void PolygonRenderer_Clear(void) {
	polygons.triangleCount = 0;
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

static int ProjectVertex(const PolygonVertex *vertex, const Camera *camera,
	int width, int height, ProjectedVertex *projected) {
	const float sine = SDL_sinf(camera->angle), cosine = SDL_cosf(camera->angle);
	const float dx = vertex->x - camera->position.x;
	const float dy = vertex->y - camera->position.y;
	const float depth = (-sine * dx) - (cosine * dy);
	if(depth <= 1.0f || depth >= camera->distance) return 0;
	const float lateral = (cosine * dx) - (sine * dy);
	const float scale = (float)height / CAMERA_PROJECTION_SCALE;
	projected->x = ((lateral / depth) + 1.0f) * (float)width * 0.5f;
	projected->y = ((camera->height - vertex->z) * scale / depth) + camera->horizon;
	projected->inverseDepth = 1.0f / depth;
	projected->uOverDepth = vertex->u / depth;
	projected->vOverDepth = vertex->v / depth;
	return 1;
}

static float Edge(float ax, float ay, float bx, float by, float px, float py) {
	return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static Uint32 SampleMaterial(const PolygonMaterial *material, float u, float v) {
	if(material->type == POLYGON_MATERIAL_FLAT || !material->texture)
		return material->color;
	u -= SDL_floorf(u); v -= SDL_floorf(v);
	const int x = min((int)(u * material->textureWidth), material->textureWidth - 1);
	const int y = min((int)(v * material->textureHeight), material->textureHeight - 1);
	return material->texture[y * material->textureWidth + x];
}

		
static int PrepareProjectedTriangles(Camera *camera, int width, int height) {
	if(polygons.projectedCapacity < polygons.triangleCount) {
		void *memory = SDL_realloc(polygons.projected,
			(size_t)polygons.triangleCount * sizeof(*polygons.projected));
		if(!memory) return 0;
		polygons.projected = memory;
		polygons.projectedCapacity = polygons.triangleCount;
	}
	polygons.projectedCount = 0;
	for(int index = 0; index < polygons.triangleCount; index++) {
		const PolygonTriangle *source = &polygons.triangles[index];
		ProjectedTriangle *triangle = &polygons.projected[polygons.projectedCount];
		if(!ProjectVertex(&source->vertex[0], camera, width, height, &triangle->vertex[0]) ||
		   !ProjectVertex(&source->vertex[1], camera, width, height, &triangle->vertex[1]) ||
		   !ProjectVertex(&source->vertex[2], camera, width, height, &triangle->vertex[2])) continue;
		ProjectedVertex *p = triangle->vertex;
		triangle->area = Edge(p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
		if(SDL_fabsf(triangle->area) < 0.0001f) continue;
		triangle->minX = max(0, (int)SDL_floorf(min(p[0].x, min(p[1].x, p[2].x))));
		triangle->maxX = min(width - 1, (int)SDL_ceilf(max(p[0].x, max(p[1].x, p[2].x))));
		triangle->minY = max(0, (int)SDL_floorf(min(p[0].y, min(p[1].y, p[2].y))));
		triangle->maxY = min(height - 1, (int)SDL_ceilf(max(p[0].y, max(p[1].y, p[2].y))));
		if(triangle->minX > triangle->maxX || triangle->minY > triangle->maxY) continue;
		triangle->material = source->material;
		polygons.projectedCount++;
	}
	return 1;
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

static int PolygonWorker(void *unused) {
	(void)unused;
	int generation = SDL_AtomicGet(&polygons.workerGeneration);
	while(1) {
		SDL_LockMutex(polygons.workerMutex);
		while(!SDL_AtomicGet(&polygons.endWorkers) &&
			SDL_AtomicGet(&polygons.workerGeneration) == generation)
			SDL_CondWait(polygons.workerCondition, polygons.workerMutex);
		SDL_UnlockMutex(polygons.workerMutex);
		if(SDL_AtomicGet(&polygons.endWorkers)) break;
		generation = SDL_AtomicGet(&polygons.workerGeneration);
		SDL_MemoryBarrierAcquire();
		ConsumePolygonTiles();
		if(SDL_AtomicAdd(&polygons.workersPending, -1) - 1 == 0) {
			SDL_LockMutex(polygons.workerMutex);
			SDL_CondSignal(polygons.workerDone);
			SDL_UnlockMutex(polygons.workerMutex);
		}
	}
	return 0;
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
	if(!polygons.workers) { polygons.workerCount = 0; return; }
	SDL_AtomicSet(&polygons.endWorkers, 0);
	for(int i = 0; i < polygons.workerCount; i++) {
		polygons.workers[i] = SDL_CreateThread(PolygonWorker, "polygon-render", NULL);
		if(!polygons.workers[i]) { polygons.workerCount = i; break; }
	}
	SDL_Log("Polygon software renderer: %d worker thread(s), %dx%d tiles",
		polygons.workerCount, POLYGON_TILE_SIZE, POLYGON_TILE_SIZE);
}

static void DestroyPolygonWorkers(void) {
	if(polygons.workerMutex) {
		SDL_LockMutex(polygons.workerMutex);SDL_AtomicSet(&polygons.endWorkers,1);
		SDL_AtomicAdd(&polygons.workerGeneration,1);SDL_CondBroadcast(polygons.workerCondition);SDL_UnlockMutex(polygons.workerMutex);
	}
	for(int i=0;i<polygons.workerCount;i++)if(polygons.workers[i])SDL_WaitThread(polygons.workers[i],NULL);
	SDL_free(polygons.workers);polygons.workers=NULL;polygons.workerCount=0;
	if(polygons.workerDone)SDL_DestroyCond(polygons.workerDone);if(polygons.workerCondition)SDL_DestroyCond(polygons.workerCondition);if(polygons.workerMutex)SDL_DestroyMutex(polygons.workerMutex);
	polygons.workerDone=polygons.workerCondition=NULL;polygons.workerMutex=NULL;
}
#endif

void PolygonRenderer_DrawSoftware(Map *map, Camera *camera) {
	if(!map->screen || !map->depth) return;
	int width = 0, height = 0, pitchBytes = 0;
	Uint32 *pixels = NULL;
	SDL_QueryTexture((SDL_Texture *)map->screen, NULL, NULL, &width, &height);
	if(!PrepareProjectedTriangles(camera, width, height) || polygons.projectedCount == 0) return;
	if(SDL_LockTexture((SDL_Texture *)map->screen, NULL, (void **)&pixels, &pitchBytes) != 0) return;
	const int pitch = pitchBytes / (int)sizeof(*pixels);
#ifdef USE_THREADED_RENDER
	if(polygons.workerCount > 0) {
		polygons.job.pixels=pixels;polygons.job.depth=map->depth;
		polygons.job.width=width;polygons.job.height=height;polygons.job.pitch=pitch;
		polygons.job.tilesX=(width+POLYGON_TILE_SIZE-1)/POLYGON_TILE_SIZE;
		polygons.job.tileCount=polygons.job.tilesX*((height+POLYGON_TILE_SIZE-1)/POLYGON_TILE_SIZE);
		SDL_AtomicSet(&polygons.nextTile,0);SDL_AtomicSet(&polygons.workersPending,polygons.workerCount);
		SDL_MemoryBarrierRelease();SDL_LockMutex(polygons.workerMutex);SDL_AtomicAdd(&polygons.workerGeneration,1);
		SDL_CondBroadcast(polygons.workerCondition);SDL_UnlockMutex(polygons.workerMutex);
		ConsumePolygonTiles();
		SDL_LockMutex(polygons.workerMutex);while(SDL_AtomicGet(&polygons.workersPending)>0)SDL_CondWait(polygons.workerDone,polygons.workerMutex);SDL_UnlockMutex(polygons.workerMutex);
	} else
#endif
		RasterizeTile(pixels,map->depth,pitch,width,0,0,width-1,height-1);
	SDL_UnlockTexture((SDL_Texture *)map->screen);
}

#ifdef USE_OPENGL_RENDER
typedef struct sGLPolygonVertex { float position[3], uv[2], color[4], textured; } GLPolygonVertex;

static GLuint CompilePolygonShader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type); glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
	GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) { glDeleteShader(shader); return 0; }
	return shader;
}

int PolygonRenderer_InitOpenGL(void) {
	for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++)
		polygons.checker[y * 8 + x] = ((x ^ y) & 1) ? 0xffd8b060u : 0xff704020u;
	static const char *vs = "#version 330 core\nlayout(location=0)in vec3 p;layout(location=1)in vec2 uv;layout(location=2)in vec4 color;layout(location=3)in float textured;uniform vec2 cameraPosition,resolution;uniform float cameraHeight,horizon,angle,distance;out vec2 fuv;out vec4 fc;flat out float ft;void main(){float s=sin(angle),c=cos(angle);vec2 d=p.xy-cameraPosition;float z=-s*d.x-c*d.y,l=c*d.x-s*d.y;float ny=(1.-2.*horizon/resolution.y)*z-(2./2.5)*(cameraHeight-p.z);float n=1.,f=distance;gl_Position=vec4(l,ny,((f+n)/(f-n))*z-(2.*f*n/(f-n)),z);fuv=uv;fc=color;ft=textured;}";
	static const char *fs = "#version 330 core\nuniform sampler2D polygonTexture;in vec2 fuv;in vec4 fc;flat in float ft;out vec4 outColor;void main(){outColor=ft>.5?texture(polygonTexture,fuv)*fc:fc;}";
	GLuint vertex = CompilePolygonShader(GL_VERTEX_SHADER, vs), fragment = CompilePolygonShader(GL_FRAGMENT_SHADER, fs);
	if(!vertex || !fragment) return 1;
	polygons.program = glCreateProgram(); glAttachShader(polygons.program, vertex); glAttachShader(polygons.program, fragment); glLinkProgram(polygons.program);
	glDeleteShader(vertex); glDeleteShader(fragment);
	GLint linked = 0; glGetProgramiv(polygons.program, GL_LINK_STATUS, &linked); if(!linked) return 1;
	glGenVertexArrays(1, &polygons.vao); glGenBuffers(1, &polygons.vbo); glGenTextures(1, &polygons.texture);
	glBindVertexArray(polygons.vao); glBindBuffer(GL_ARRAY_BUFFER, polygons.vbo);
	const GLsizei stride = sizeof(GLPolygonVertex);
	for(int i = 0; i < 4; i++) glEnableVertexAttribArray(i);
	glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,stride,(void *)offsetof(GLPolygonVertex,position));
	glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,stride,(void *)offsetof(GLPolygonVertex,uv));
	glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,stride,(void *)offsetof(GLPolygonVertex,color));
	glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,stride,(void *)offsetof(GLPolygonVertex,textured));
	glBindTexture(GL_TEXTURE_2D, polygons.texture); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,8,8,0,GL_BGRA,GL_UNSIGNED_BYTE,polygons.checker);
	polygons.uploadedTexture=polygons.checker;polygons.uploadedTextureWidth=8;polygons.uploadedTextureHeight=8;
	return 0;
}

void PolygonRenderer_DrawOpenGL(Camera *camera, int width, int height) {
	glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glUseProgram(polygons.program);
	glUniform2f(glGetUniformLocation(polygons.program,"cameraPosition"),camera->position.x,camera->position.y);glUniform2f(glGetUniformLocation(polygons.program,"resolution"),(float)width,(float)height);
	glUniform1f(glGetUniformLocation(polygons.program,"cameraHeight"),camera->height);glUniform1f(glGetUniformLocation(polygons.program,"horizon"),camera->horizon);glUniform1f(glGetUniformLocation(polygons.program,"angle"),camera->angle);glUniform1f(glGetUniformLocation(polygons.program,"distance"),camera->distance);
	glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,polygons.texture);glUniform1i(glGetUniformLocation(polygons.program,"polygonTexture"),0);
	glBindVertexArray(polygons.vao);glBindBuffer(GL_ARRAY_BUFFER,polygons.vbo);
	for(int i=0;i<polygons.triangleCount;i++) {
		const PolygonTriangle *t=&polygons.triangles[i];GLPolygonVertex vertices[3];
		if(t->material->type==POLYGON_MATERIAL_TEXTURED&&
			(t->material->texture!=polygons.uploadedTexture||t->material->textureWidth!=polygons.uploadedTextureWidth||t->material->textureHeight!=polygons.uploadedTextureHeight)){
			glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,t->material->textureWidth,t->material->textureHeight,0,GL_BGRA,GL_UNSIGNED_BYTE,t->material->texture);
			polygons.uploadedTexture=t->material->texture;polygons.uploadedTextureWidth=t->material->textureWidth;polygons.uploadedTextureHeight=t->material->textureHeight;
		}
		for(int j=0;j<3;j++){const PolygonVertex *v=&t->vertex[j];GLPolygonVertex *out=&vertices[j];out->position[0]=v->x;out->position[1]=v->y;out->position[2]=v->z;out->uv[0]=v->u;out->uv[1]=v->v;const Uint32 c=t->material->color;out->color[0]=(float)((c>>16)&255)/255.;out->color[1]=(float)((c>>8)&255)/255.;out->color[2]=(float)(c&255)/255.;out->color[3]=(float)((c>>24)&255)/255.;out->textured=t->material->type==POLYGON_MATERIAL_TEXTURED?1.f:0.f;}
		glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,3);
	}
	glBindVertexArray(0);glDisable(GL_DEPTH_TEST);
}

void PolygonRenderer_DestroyOpenGL(void) {
	if(polygons.texture)glDeleteTextures(1,&polygons.texture);if(polygons.vbo)glDeleteBuffers(1,&polygons.vbo);if(polygons.vao)glDeleteVertexArrays(1,&polygons.vao);if(polygons.program)glDeleteProgram(polygons.program);
	polygons.texture=polygons.vbo=polygons.vao=polygons.program=0;
	polygons.uploadedTexture=NULL;polygons.uploadedTextureWidth=polygons.uploadedTextureHeight=0;
}
#endif
