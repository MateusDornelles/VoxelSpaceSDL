#ifndef VSENGINE_H
#define VSENGINE_H
// Tell SDL not to redefine main
#define SDL_MAIN_HANDLED
typedef struct sPoint {
	float x, y;
} Point;

#include "map.h"
#include "camera.h"

typedef enum eRendererBackend {
	RENDERER_SOFTWARE,
	RENDERER_OPENGL
} RendererBackend;

typedef struct sEngineSettings {
	int vsync, width, height;
	int integerScale;
	int benchmarkSeconds;
	RendererBackend renderer;
	char *diffusemap, *heightmap;
	char *ceilingdiffusemap, *ceilingheightmap;
#ifdef USE_THREADED_RENDER
	int numthreads;
#endif
} EngineSettings;

typedef enum eListeners {
	LISTEN_ENGINE_START, // Called when engine starts
	LISTEN_ENGINE_UPDATE, // Called every tick
	LISTEN_ENGINE_DRAW, // Called before presenting to the renderer
	LISTEN_ENGINE_STOP, // Called when engine stops
	LISTEN_CONTROLLER_FAIL, // Called when controller init fails
	LISTEN_CONTROLLER_ADD, // Called when a controller is connected
	LISTEN_CONTROLLER_DEL, // Called when a controller is disconnected
	LISTEN_SDL_WINDOW, // Called when the SDL window is created
	LISTEN_SDL_EVENT, // Called when an SDL event is received
	LISTEN_TYPES_MAX
} Listeners;


int Engine_Start(EngineSettings *es);
int Engine_Update(void);
void Engine_Stop(void);
void Engine_End(void);

void Engine_ToggleFullscreen(void);
void Engine_ToggleRenderer(void);
void Engine_CycleIntegerScale(void);
void *Engine_GetWindow(void);
void Engine_GetObjects(Camera **cam, Map **map);
float Engine_GetDeltaTime(void);
void Engine_AddListener(Listeners type, void(*func)(void *));
void Engine_CallListeners(Listeners type, void *arg);
#endif
