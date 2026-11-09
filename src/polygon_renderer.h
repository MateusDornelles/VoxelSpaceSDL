#ifndef VSPOLYGONRENDERER_H
#define VSPOLYGONRENDERER_H

#include <stddef.h>
#include "engine.h"

typedef enum ePolygonMaterialType {
	POLYGON_MATERIAL_FLAT,
	POLYGON_MATERIAL_TEXTURED
} PolygonMaterialType;

typedef struct sPolygonVertex {
	float x, y, z;
	float u, v;
} PolygonVertex;

typedef struct sPolygonMaterial {
	PolygonMaterialType type;
	Uint32 color;
	const Uint32 *texture;
	int textureWidth, textureHeight;
	int doubleSided;
} PolygonMaterial;

typedef struct sPolygonTriangle {
	PolygonVertex vertex[3];
	const PolygonMaterial *material;
} PolygonTriangle;

void PolygonRenderer_Init(Map *map);
/* Load axis-aligned buildings from a text file (x y width depth height material). */
int PolygonRenderer_LoadBuildings(Map *map, const char *path);
/* Derive "maps/foo.buildings" from a heightmap path like "maps/foo.png". */
void PolygonRenderer_BuildingsPathFromHeightmap(const char *heightmap, char *out, size_t outSize);
void PolygonRenderer_Destroy(void);
void PolygonRenderer_Clear(void);
int PolygonRenderer_AddTriangle(const PolygonTriangle *triangle);
/* Axis-aligned solid used by movement collision (independent of draw triangles). */
int PolygonRenderer_AddColliderBox(float x, float y, float z, float width, float depth, float height);
/* Highest walkable polygon top under (x, y), or -FLT_MAX when none. */
float PolygonRenderer_GetSupportHeight(float x, float y);
/* Push (x, y) out of solid colliders that overlap the vertical capsule [feetZ, headZ]. */
void PolygonRenderer_ResolvePosition(float *x, float *y, float feetZ, float headZ, float radius);
void PolygonRenderer_DrawSoftware(Map *map, Camera *camera);
/* Project and rasterize an arbitrary triangle list into the map screen/depth buffers. */
void PolygonRenderer_DrawTriangleList(
	Map *map, Camera *camera, const PolygonTriangle *triangles, int triangleCount);

#ifdef USE_OPENGL_RENDER
int PolygonRenderer_InitOpenGL(void);
void PolygonRenderer_DrawOpenGL(Camera *camera, int width, int height);
void PolygonRenderer_DrawTriangleListOpenGL(
	Camera *camera, int width, int height, const PolygonTriangle *triangles, int triangleCount);
/* Upload a static GPU mesh (e.g. terrain). Replaces any previous static mesh. */
void PolygonRenderer_UploadStaticMeshOpenGL(const PolygonTriangle *triangles, int triangleCount);
void PolygonRenderer_ClearStaticMeshOpenGL(void);
int PolygonRenderer_HasStaticMeshOpenGL(void);
void PolygonRenderer_DrawStaticMeshOpenGL(Camera *camera, int width, int height);
void PolygonRenderer_DestroyOpenGL(void);
#endif

#endif
