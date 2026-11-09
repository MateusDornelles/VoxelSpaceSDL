#include "terrain_mesh.h"
#include "defines.h"
#include "polygon_renderer.h"
#include <SDL_log.h>
#include <SDL_stdinc.h>

static struct {
	PolygonTriangle *triangles;
	PolygonMaterial material;
	int triangleCount;
	int step;
} terrain;

static inline int SampleOffset(const Map *map, int x, int y) {
	return ((y & (map->width - 1)) << map->shift) + (x & (map->height - 1));
}

void TerrainMesh_Destroy(void) {
#ifdef USE_OPENGL_RENDER
	PolygonRenderer_ClearStaticMeshOpenGL();
#endif
	SDL_free(terrain.triangles);
	terrain.triangles = NULL;
	terrain.triangleCount = 0;
	terrain.step = 0;
	terrain.material = (PolygonMaterial){0};
}

int TerrainMesh_Build(Map *map, int step) {
	TerrainMesh_Destroy();
	if(!map || !map->ready || !map->altitude || !map->color)
		return 0;
	if(step < 1)
		step = TERRAIN_MESH_STEP_DEFAULT;

	const int cellsX = (map->width - 1 + step - 1) / step;
	const int cellsY = (map->height - 1 + step - 1) / step;
	const int triangleCount = cellsX * cellsY * 2;
	if(triangleCount <= 0)
		return 0;

	terrain.triangles = SDL_calloc((size_t)triangleCount, sizeof(*terrain.triangles));
	if(!terrain.triangles) {
		TerrainMesh_Destroy();
		return 0;
	}

	/* Same diffuse sampling as raycast: per-pixel (texel) lookup via UVs. */
	terrain.material = (PolygonMaterial){
		.type = POLYGON_MATERIAL_TEXTURED,
		.color = 0xffffffffu,
		.texture = (const Uint32 *)map->color,
		.textureWidth = map->width,
		.textureHeight = map->height,
		.doubleSided = 0
	};

	const float invW = 1.0f / (float)map->width;
	const float invH = 1.0f / (float)map->height;

	int out = 0;
	for(int y = 0; y < map->height - 1; y += step) {
		const int y1 = min(y + step, map->height - 1);
		for(int x = 0; x < map->width - 1; x += step) {
			const int x1 = min(x + step, map->width - 1);
			const int o00 = SampleOffset(map, x, y);
			const int o10 = SampleOffset(map, x1, y);
			const int o01 = SampleOffset(map, x, y1);
			const int o11 = SampleOffset(map, x1, y1);

			const PolygonVertex v00 = {
				.x = (float)x, .y = (float)y, .z = (float)map->altitude[o00],
				.u = (float)x * invW, .v = (float)y * invH
			};
			const PolygonVertex v10 = {
				.x = (float)x1, .y = (float)y, .z = (float)map->altitude[o10],
				.u = (float)x1 * invW, .v = (float)y * invH
			};
			const PolygonVertex v01 = {
				.x = (float)x, .y = (float)y1, .z = (float)map->altitude[o01],
				.u = (float)x * invW, .v = (float)y1 * invH
			};
			const PolygonVertex v11 = {
				.x = (float)x1, .y = (float)y1, .z = (float)map->altitude[o11],
				.u = (float)x1 * invW, .v = (float)y1 * invH
			};

			terrain.triangles[out++] = (PolygonTriangle){
				.vertex = { v00, v10, v01 },
				.material = &terrain.material
			};
			terrain.triangles[out++] = (PolygonTriangle){
				.vertex = { v10, v11, v01 },
				.material = &terrain.material
			};
		}
	}

	terrain.triangleCount = out;
	terrain.step = step;
	SDL_Log("Terrain mesh: %d triangles (step %d, %dx%d map, textured)",
		terrain.triangleCount, step, map->width, map->height);
#ifdef USE_OPENGL_RENDER
	PolygonRenderer_UploadStaticMeshOpenGL(terrain.triangles, terrain.triangleCount);
#endif
	return 1;
}

int TerrainMesh_GetTriangleCount(void) {
	return terrain.triangleCount;
}

void TerrainMesh_DrawSoftware(Map *map, Camera *camera) {
	if(terrain.triangleCount <= 0)
		return;
	PolygonRenderer_DrawTriangleList(map, camera, terrain.triangles, terrain.triangleCount);
}

#ifdef USE_OPENGL_RENDER
void TerrainMesh_DrawOpenGL(Camera *camera, int width, int height) {
	if(terrain.triangleCount <= 0)
		return;
	PolygonRenderer_UploadStaticMeshOpenGL(terrain.triangles, terrain.triangleCount);
	if(PolygonRenderer_HasStaticMeshOpenGL())
		PolygonRenderer_DrawStaticMeshOpenGL(camera, width, height);
	else
		PolygonRenderer_DrawTriangleListOpenGL(
			camera, width, height, terrain.triangles, terrain.triangleCount);
}
#endif
