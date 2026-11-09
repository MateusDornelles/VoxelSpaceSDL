#ifndef VSTERRAINMESH_H
#define VSTERRAINMESH_H

#include "engine.h"

#define TERRAIN_MESH_STEP_DEFAULT 8

/* Build a top-face triangle mesh from the floor heightmap (step in map texels). */
int TerrainMesh_Build(Map *map, int step);
void TerrainMesh_Destroy(void);
int TerrainMesh_GetTriangleCount(void);

void TerrainMesh_DrawSoftware(Map *map, Camera *camera);

#ifdef USE_OPENGL_RENDER
void TerrainMesh_DrawOpenGL(Camera *camera, int width, int height);
#endif

#endif
