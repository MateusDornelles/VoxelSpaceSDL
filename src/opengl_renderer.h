#ifndef VSOPENGLRENDERER_H
#define VSOPENGLRENDERER_H

#include <SDL_video.h>
#include "engine.h"

int OpenGLRenderer_Init(SDL_Window *window, int vsync);
void OpenGLRenderer_Resize(int width, int height, int outputWidth, int outputHeight, int integerScale);
void OpenGLRenderer_Draw(Map *map, Camera *cam);
void OpenGLRenderer_DrawFPS(float fps);
void OpenGLRenderer_Present(SDL_Window *window);
void OpenGLRenderer_Destroy(void);

#endif
