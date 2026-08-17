#ifndef VSPOLYGONRENDERER_H
#define VSPOLYGONRENDERER_H

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
void PolygonRenderer_Destroy(void);
void PolygonRenderer_Clear(void);
int PolygonRenderer_AddTriangle(const PolygonTriangle *triangle);
void PolygonRenderer_DrawSoftware(Map *map, Camera *camera);

#ifdef USE_OPENGL_RENDER
int PolygonRenderer_InitOpenGL(void);
void PolygonRenderer_DrawOpenGL(Camera *camera, int width, int height);
void PolygonRenderer_DestroyOpenGL(void);
#endif

#endif
