#ifndef VSSPRITERENDERER_H
#define VSSPRITERENDERER_H
#include "engine.h"
int SpriteRenderer_AddAPNG(const char *path,float x,float y,float z,float width,float height);
void SpriteRenderer_Update(float deltaMilliseconds);
int SpriteRenderer_HasObjects(void);
void SpriteRenderer_DrawSoftware(Map *map,Camera *camera);
void SpriteRenderer_Destroy(void);
#ifdef USE_OPENGL_RENDER
int SpriteRenderer_InitOpenGL(void);
void SpriteRenderer_DrawOpenGL(Camera *camera,int width,int height);
void SpriteRenderer_DestroyOpenGL(void);
#endif
#endif
