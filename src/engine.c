#include "engine.h"
#include "defines.h"
#include <SDL.h>
#ifdef USE_SDL_IMAGE
#include <SDL_image.h>
#endif
#ifdef USE_SDL_TTF
#include <SDL_ttf.h>
#endif
#include "camera.h"
#include "map.h"
#include "polygon_renderer.h"
#include "sprite_renderer.h"
#ifdef USE_OPENGL_RENDER
#include "opengl_renderer.h"
#endif

struct sContext {
	Uint64 lastTime, currTime;
	float deltaTime;
	Uint32 stopped;
	struct {
		Uint64 sampleStart;
		Uint32 sampleFrames;
		float value;
	} fps;
	struct {
		int enabled;
		Uint64 startCounter;
		Uint64 durationTicks;
		Uint64 frameCount;
		Point basePosition;
		float baseHeight;
		float baseHorizon;
	} benchmark;
	SDL_Window *wnd;
	SDL_Renderer *render;
	SDL_Texture *screen;
	RendererBackend backend;
	int integerScale, vsync;
	struct sListener {
		void(*func)(void *);
		struct sListener *next;
	} *listeners[LISTEN_TYPES_MAX];
	Camera camera;
	Map map;
} ctx = {
	.camera = {
		.zstep = CAMERA_ZSTEP_DEFAULT,
		.position = CAMERA_POSITION_DEFAULT,
		.height = CAMERA_HEIGHT_DEFAULT,
		.distance = CAMERA_DISTANCE_DEFAULT
	},
	.map = {
		.ceilingBase = 520.0f
	}
};

static void CompareSDLVersions(const char *libname, const SDL_version *cv, const SDL_version *lv) {
	if(SDL_VERSIONNUM(cv->major, cv->minor, cv->patch) != SDL_VERSIONNUM(lv->major, lv->minor, lv->patch)) {
		SDL_LogWarn(0, "%s library version mismatch. Expected: %d.%d.%d, loaded: %d.%d.%d",
			libname, cv->major, cv->minor, cv->patch, lv->major, lv->minor, lv->patch
		);
	} else SDL_Log("%s library version: %d.%d.%d", libname, cv->major, cv->minor, cv->patch);
}

enum {
	SEG_A = (1 << 0),
	SEG_B = (1 << 1),
	SEG_C = (1 << 2),
	SEG_D = (1 << 3),
	SEG_E = (1 << 4),
	SEG_F = (1 << 5),
	SEG_G = (1 << 6)
};

static Uint8 SevenSegMaskForChar(char c) {
	switch(c) {
		case '0': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
		case '1': return SEG_B | SEG_C;
		case '2': return SEG_A | SEG_B | SEG_G | SEG_E | SEG_D;
		case '3': return SEG_A | SEG_B | SEG_G | SEG_C | SEG_D;
		case '4': return SEG_F | SEG_G | SEG_B | SEG_C;
		case '5': return SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
		case '6': return SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D;
		case '7': return SEG_A | SEG_B | SEG_C;
		case '8': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
		case '9': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
		case 'F': return SEG_A | SEG_E | SEG_F | SEG_G;
		case 'P': return SEG_A | SEG_B | SEG_E | SEG_F | SEG_G;
		case 'S': return SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
		default: return 0;
	}
}

static void DrawSevenSegMask(SDL_Renderer *renderer, Uint8 mask, int x, int y, int scale) {
	const int t = max(scale, 1);
	const int h = t * 4;
	const int v = t * 5;

	if(mask & SEG_A) SDL_RenderFillRect(renderer, &(SDL_Rect){x + t, y, h, t});
	if(mask & SEG_B) SDL_RenderFillRect(renderer, &(SDL_Rect){x + t + h, y + t, t, v});
	if(mask & SEG_C) SDL_RenderFillRect(renderer, &(SDL_Rect){x + t + h, y + (2 * t) + v, t, v});
	if(mask & SEG_D) SDL_RenderFillRect(renderer, &(SDL_Rect){x + t, y + (2 * v) + (2 * t), h, t});
	if(mask & SEG_E) SDL_RenderFillRect(renderer, &(SDL_Rect){x, y + (2 * t) + v, t, v});
	if(mask & SEG_F) SDL_RenderFillRect(renderer, &(SDL_Rect){x, y + t, t, v});
	if(mask & SEG_G) SDL_RenderFillRect(renderer, &(SDL_Rect){x + t, y + t + v, h, t});
}

static int SevenSegCharWidth(char c, int scale) {
	const int t = max(scale, 1);
	const int glyphWidth = (t * 4) + (2 * t);

	if(c == ' ')
		return glyphWidth / 2;
	return glyphWidth;
}

static int DrawSevenSegChar(SDL_Renderer *renderer, char c, int x, int y, int scale) {
	const int t = max(scale, 1);
	const int glyphWidth = SevenSegCharWidth(c, scale);

	if(c == '.') {
		const int v = t * 5;
		SDL_RenderFillRect(renderer, &(SDL_Rect){x + glyphWidth - t, y + (2 * v) + (2 * t), t, t});
		return glyphWidth;
	}
	if(c == ' ')
		return glyphWidth;

	DrawSevenSegMask(renderer, SevenSegMaskForChar(c), x, y, scale);
	return glyphWidth;
}

static void DrawFPSCounter(void) {
	int width, height;
	if(SDL_GetRendererOutputSize(ctx.render, &width, &height) != 0)
		return;
	(void)height;

	const int scale = 2;
	const int gap = scale * 2;
	const int margin = 10;
	const int padding = 6;
	const int glyphHeight = (2 * (scale * 5)) + (3 * scale);
	char text[24];
	SDL_snprintf(text, sizeof(text), "FPS %.0f", ctx.fps.value);

	int textWidth = 0;
	for(size_t i = 0; text[i]; i++)
		textWidth += SevenSegCharWidth(text[i], scale) + gap;
	if(textWidth > 0)
		textWidth -= gap;

	const SDL_Rect bg = {
		.x = max(width - textWidth - (padding * 2) - margin, margin),
		.y = margin,
		.w = textWidth + (padding * 2),
		.h = glyphHeight + (padding * 2)
	};

	SDL_BlendMode oldBlendMode;
	SDL_GetRenderDrawBlendMode(ctx.render, &oldBlendMode);
	SDL_SetRenderDrawBlendMode(ctx.render, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(ctx.render, 0, 0, 0, 140);
	SDL_RenderFillRect(ctx.render, &bg);

	SDL_SetRenderDrawColor(ctx.render, 255, 255, 90, 255);
	int x = bg.x + padding;
	for(size_t i = 0; text[i]; i++)
		x += DrawSevenSegChar(ctx.render, text[i], x, bg.y + padding, scale) + gap;
	SDL_SetRenderDrawBlendMode(ctx.render, oldBlendMode);
}

static inline void UpdateBenchmarkCamera(void) {
	if(!ctx.benchmark.enabled)
		return;

	const float t = (float)ctx.benchmark.frameCount * 0.02f;
	ctx.camera.angle = t * 0.65f;
	ctx.camera.position.x = ctx.benchmark.basePosition.x + SDL_sinf(t * 1.3f) * 160.0f;
	ctx.camera.position.y = ctx.benchmark.basePosition.y + SDL_cosf(t * 1.1f) * 160.0f;
	ctx.camera.height = ctx.benchmark.baseHeight + SDL_sinf(t * 0.5f) * 18.0f;
	ctx.camera.horizon = ctx.benchmark.baseHorizon + SDL_sinf(t * 0.8f) * (ctx.camera.maxhorizon * 0.08f);
	Camera_ClampPitch(&ctx.camera);
	ctx.map.redraw = 1;
}

static int SpawnScreen(void) {
	int wndWidth = 0, wndHeight = 0;
	int renderWidth = 0, renderHeight = 0;
	float horizonRatio = 0.5f;
	SDL_GetWindowSize(ctx.wnd, &wndWidth, &wndHeight);
#ifdef USE_OPENGL_RENDER
	if(ctx.backend == RENDERER_OPENGL) {
		int outputWidth = 0, outputHeight = 0;
		SDL_GL_GetDrawableSize(ctx.wnd, &outputWidth, &outputHeight);
		renderWidth = ctx.integerScale > 1 ? max(outputWidth / ctx.integerScale, 1) : outputWidth;
		renderHeight = ctx.integerScale > 1 ? max(outputHeight / ctx.integerScale, 1) : outputHeight;
		if(ctx.camera.maxhorizon > 0.0f)
			horizonRatio = ctx.camera.horizon / ctx.camera.maxhorizon;
		OpenGLRenderer_Resize(renderWidth, renderHeight,
			outputWidth, outputHeight, ctx.integerScale);
		ctx.camera.maxhorizon = (float)renderHeight;
		ctx.camera.horizon = horizonRatio * ctx.camera.maxhorizon;
		Camera_ClampPitch(&ctx.camera);
		ctx.map.redraw = 1;
		return 0;
	}
#endif
	if(ctx.integerScale > 1) {
		renderWidth = max(wndWidth / ctx.integerScale, 1);
		renderHeight = max(wndHeight / ctx.integerScale, 1);
	} else {
		renderWidth = wndWidth;
		renderHeight = wndHeight;
	}
	SDL_Log(
		"Render resolution: %dx%d, window: %dx%d, integer scale: %dx",
		renderWidth, renderHeight, wndWidth, wndHeight, ctx.integerScale
	);

	if(ctx.camera.maxhorizon > 0.0f)
		horizonRatio = ctx.camera.horizon / ctx.camera.maxhorizon;

	if(ctx.screen) SDL_DestroyTexture(ctx.screen);
	ctx.screen = SDL_CreateTexture(ctx.render,
		SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
		renderWidth, renderHeight
	);
	if(ctx.screen == NULL)
		return 1;

#if SDL_VERSION_ATLEAST(2, 0, 12)
	SDL_SetTextureScaleMode(ctx.screen, SDL_ScaleModeNearest);
#endif
	ctx.camera.maxhorizon = (float)renderHeight;
	ctx.camera.horizon = horizonRatio * ctx.camera.maxhorizon;
	Camera_ClampPitch(&ctx.camera);
	Map_SetScreen(&ctx.map, ctx.screen);
	return 0;
}

static void DestroyPresentationBackend(void) {
	if(ctx.backend == RENDERER_SOFTWARE) {
		Map_SetScreen(&ctx.map, NULL);
		if(ctx.screen) SDL_DestroyTexture(ctx.screen);
		if(ctx.render) SDL_DestroyRenderer(ctx.render);
		ctx.screen = NULL;
		ctx.render = NULL;
	}
#ifdef USE_OPENGL_RENDER
	else {
		OpenGLRenderer_Destroy();
	}
#endif
	if(ctx.wnd) SDL_DestroyWindow(ctx.wnd);
	ctx.wnd = NULL;
}

static int CreatePresentationBackend(int width, int height) {
	Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#ifdef USE_OPENGL_RENDER
	if(ctx.backend == RENDERER_OPENGL) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		windowFlags |= SDL_WINDOW_OPENGL;
	}
#endif
	ctx.wnd = SDL_CreateWindow(GRAPHICS_TITLE,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, height, windowFlags);
	if(!ctx.wnd) {
		SDL_LogCritical(0, "Failed to create SDL window: %s", SDL_GetError());
		return 2;
	}
	Engine_CallListeners(LISTEN_SDL_WINDOW, ctx.wnd);

#ifdef USE_OPENGL_RENDER
	if(ctx.backend == RENDERER_OPENGL) {
		SDL_SetWindowTitle(ctx.wnd, "VoxelSpace SDL (OpenGL 3.3)");
		if(OpenGLRenderer_Init(ctx.wnd, ctx.vsync) != 0)
			return 3;
	} else
#endif
	{
		Uint32 rendererFlags = SDL_RENDERER_TARGETTEXTURE;
		if(ctx.vsync) rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
		ctx.render = SDL_CreateRenderer(ctx.wnd, -1, rendererFlags);
		if(!ctx.render) {
			SDL_LogCritical(0, "Failed to create SDL renderer: %s", SDL_GetError());
			return 3;
		}
		SDL_RendererInfo info;
		if(SDL_GetRendererInfo(ctx.render, &info) == 0)
			SDL_Log("Using %s %s renderer",
				info.flags & SDL_RENDERER_ACCELERATED ? "hardware" : "software", info.name);
	}
	return SpawnScreen() ? 4 : 0;
}

int Engine_Start(EngineSettings *es) {
	ctx.backend = es->renderer;
#ifndef USE_OPENGL_RENDER
	if(ctx.backend == RENDERER_OPENGL) {
		SDL_LogCritical(0, "OpenGL renderer is not available in this build");
		return 1;
	}
#endif
	SDL_SetMainReady();
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	// Initialize SDL
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
		if(SDL_Init(SDL_INIT_VIDEO) != 0) {
			SDL_LogCritical(0, "SDL_Init failed: %s", SDL_GetError());
			return 1;
		}
	}

	SDL_version cver, lver;
	SDL_GetVersion(&lver);
	SDL_VERSION(&cver);
	CompareSDLVersions("SDL", &cver, &lver);

#ifdef USE_SDL_IMAGE
	SDL_IMAGE_VERSION(&cver);
	CompareSDLVersions("SDL Image", &cver, IMG_Linked_Version());
	if((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
		SDL_LogCritical(0, "Failed to initialize PNG support: %s.", IMG_GetError());
		return 1;
	}
#endif

#ifdef USE_SDL_TTF
	SDL_TTF_VERSION(&cver);
	CompareSDLVersions("SDL TTF", &cver, TTF_Linked_Version());
#endif

	ctx.integerScale = es->integerScale;
	ctx.vsync = es->vsync;
#ifdef USE_THREADED_RENDER
	/*
		Set the number of threads used for rendering.
		TODO: Expose this setting in a cleaner way.
	*/
	ctx.map.rctxcnt = es->numthreads;
#endif
	int backendResult = CreatePresentationBackend(
		es->width * ctx.integerScale, es->height * ctx.integerScale);
	if(backendResult != 0) return backendResult;

	if(es->diffusemap && es->heightmap)
		Map_OpenDual(
			&ctx.map,
			es->diffusemap, es->heightmap,
			es->ceilingdiffusemap ? es->ceilingdiffusemap : es->diffusemap,
			es->ceilingheightmap ? es->ceilingheightmap : es->heightmap
		);
	PolygonRenderer_Init(&ctx.map);
	if(es->spritePath)
		SpriteRenderer_AddAPNG(es->spritePath, es->spriteX, es->spriteY, es->spriteZ,
			es->spriteWidth, es->spriteHeight);
	#ifdef USE_APNG
	else if(ctx.map.ready) {
		Uint32 randomState = 0x51f15e77u;
		int spriteCount = 0;
		for(int i = 0; i < 200; i++) {
			randomState = randomState * 1664525u + 1013904223u;
			const float x = (float)(randomState % (Uint32)ctx.map.width);
			randomState = randomState * 1664525u + 1013904223u;
			const float y = (float)(randomState % (Uint32)ctx.map.height);
			Point position = {x, y};
			const float z = (float)Map_GetHeight(&ctx.map, &position);
			if(SpriteRenderer_AddAPNG("maps/supe.apng", x, y, z, 25.0f, 24.0f) >= 0)
				spriteCount++;
		}
		SDL_Log("Demo APNG objects: %d instances", spriteCount);
	}
	#endif

	ctx.fps.sampleStart = SDL_GetPerformanceCounter();
	ctx.fps.sampleFrames = 0;
	ctx.fps.value = 0.0f;
	if(es->benchmarkSeconds > 0) {
		ctx.benchmark.enabled = 1;
		ctx.benchmark.frameCount = 0;
		ctx.benchmark.startCounter = SDL_GetPerformanceCounter();
		ctx.benchmark.durationTicks = (Uint64)es->benchmarkSeconds * SDL_GetPerformanceFrequency();
		ctx.benchmark.basePosition = ctx.camera.position;
		ctx.benchmark.baseHeight = ctx.camera.height;
		ctx.benchmark.baseHorizon = ctx.camera.horizon;
		ctx.map.redraw = 1;
		SDL_Log("Benchmark enabled for %d second(s)", es->benchmarkSeconds);
	}

	Engine_CallListeners(LISTEN_ENGINE_START, NULL);
	return 0;
}

void Engine_AddListener(Listeners type, void(*func)(void *)) {
	struct sListener *newlistener = SDL_calloc(1, sizeof(struct sListener));
	if(ctx.listeners[type]) newlistener->next = ctx.listeners[type];
	ctx.listeners[type] = newlistener;
	newlistener->func = func;
}

void Engine_CallListeners(Listeners type, void *arg) {
	struct sListener *listener = ctx.listeners[type];

	while(listener) {
		listener->func(arg);
		listener = listener->next;
	}
}

int Engine_Update(void) {
	if(ctx.stopped) return 0;
	// Handle queued SDL events
	SDL_Event ev;
	while(SDL_PollEvent(&ev)) {
		switch(ev.type) {
			case SDL_QUIT:
				return 0;
			case SDL_WINDOWEVENT:
				if(ev.window.event == SDL_WINDOWEVENT_RESIZED && SpawnScreen()) {
					SDL_LogCritical(0, "Failed to create SDL texture: %s", SDL_GetError());
					exit(1);
				}

			default:
				Engine_CallListeners(LISTEN_SDL_EVENT, &ev);
				break;
		}
	}

	// Run all listeners waiting for UPDATE
	Engine_CallListeners(LISTEN_ENGINE_UPDATE, &ctx.deltaTime);
	SpriteRenderer_Update(ctx.deltaTime);
	if(ctx.backend == RENDERER_SOFTWARE && SpriteRenderer_HasObjects())
		ctx.map.redraw = 1;
	UpdateBenchmarkCamera();

	// Redraw the world
#ifdef USE_OPENGL_RENDER
	if(ctx.backend == RENDERER_OPENGL) {
		OpenGLRenderer_Draw(&ctx.map, &ctx.camera);
		OpenGLRenderer_DrawFPS(ctx.fps.value);
		OpenGLRenderer_Present(ctx.wnd);
	} else
#endif
	{
	Map_Draw(&ctx.map, &ctx.camera);
	PolygonRenderer_DrawSoftware(&ctx.map, &ctx.camera);
	SpriteRenderer_DrawSoftware(&ctx.map, &ctx.camera);

	// Present our texture in the SDL window
	SDL_RenderClear(ctx.render);
	if(ctx.integerScale > 1) {
		int wndWidth = 0, wndHeight = 0, renderWidth = 0, renderHeight = 0;
		SDL_GetRendererOutputSize(ctx.render, &wndWidth, &wndHeight);
		SDL_QueryTexture(ctx.screen, NULL, NULL, &renderWidth, &renderHeight);
		SDL_Rect dst = {
			.x = (wndWidth - (renderWidth * ctx.integerScale)) / 2,
			.y = (wndHeight - (renderHeight * ctx.integerScale)) / 2,
			.w = renderWidth * ctx.integerScale,
			.h = renderHeight * ctx.integerScale
		};
		SDL_RenderCopy(ctx.render, ctx.screen, NULL, &dst);
	} else
		SDL_RenderCopy(ctx.render, ctx.screen, NULL, NULL);
	Engine_CallListeners(LISTEN_ENGINE_DRAW, ctx.render);
	DrawFPSCounter();
	SDL_RenderPresent(ctx.render);
	}

	ctx.fps.sampleFrames++;
	if(ctx.fps.sampleStart > 0) {
		const Uint64 now = SDL_GetPerformanceCounter();
		const double elapsed = (double)(now - ctx.fps.sampleStart) / SDL_GetPerformanceFrequency();
		if(elapsed >= 0.25) {
			ctx.fps.value = (float)(ctx.fps.sampleFrames / elapsed);
			ctx.fps.sampleFrames = 0;
			ctx.fps.sampleStart = now;
		}
	}
	if(ctx.benchmark.enabled) {
		ctx.benchmark.frameCount++;
		const Uint64 now = SDL_GetPerformanceCounter();
		const Uint64 elapsedTicks = now - ctx.benchmark.startCounter;
			if(elapsedTicks >= ctx.benchmark.durationTicks) {
				const double elapsed = (double)elapsedTicks / SDL_GetPerformanceFrequency();
				const double fps = (double)ctx.benchmark.frameCount / elapsed;
				const double frameMs = fps > 0.0 ? 1000.0 / fps : 0.0;
				SDL_Log(
					"Benchmark result: %.2fs, %llu frames, %.2f FPS, %.3f ms/frame",
					elapsed,
					(unsigned long long)ctx.benchmark.frameCount,
					fps,
					frameMs
				);
				return 0;
			}
		}

	// Compute time spent on the full tick
	ctx.lastTime = ctx.currTime;
	ctx.currTime = SDL_GetPerformanceCounter();
	if(ctx.lastTime > 0) {
		ctx.deltaTime = (float)((ctx.currTime - ctx.lastTime) * 1000) / SDL_GetPerformanceFrequency();
		ctx.deltaTime = max(0.1f, min(ctx.deltaTime, 1000.0f));
	}
	return 1;
}

void Engine_ToggleRenderer(void) {
#ifdef USE_OPENGL_RENDER
	const RendererBackend previous = ctx.backend;
	const RendererBackend next = previous == RENDERER_SOFTWARE ?
		RENDERER_OPENGL : RENDERER_SOFTWARE;
	int width = GRAPHICS_WIDTH, height = GRAPHICS_HEIGHT;
	int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
	const int wasFullscreen = (SDL_GetWindowFlags(ctx.wnd) & SDL_WINDOW_FULLSCREEN) != 0;
	SDL_GetWindowSize(ctx.wnd, &width, &height);
	SDL_GetWindowPosition(ctx.wnd, &x, &y);

	DestroyPresentationBackend();
	ctx.backend = next;
	int result = CreatePresentationBackend(width, height);
	if(result != 0) {
		SDL_LogError(0, "Failed to switch renderer; restoring previous backend");
		DestroyPresentationBackend();
		ctx.backend = previous;
		result = CreatePresentationBackend(width, height);
		if(result != 0) {
			SDL_LogCritical(0, "Failed to restore renderer backend");
			ctx.stopped = 1;
			return;
		}
	}

	SDL_SetWindowPosition(ctx.wnd, x, y);
	if(wasFullscreen) {
		SDL_SetWindowFullscreen(ctx.wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
		SpawnScreen();
	}
	ctx.map.redraw = 1;
	SDL_Log("Renderer switched to %s", ctx.backend == RENDERER_OPENGL ? "OpenGL 3.3" : "software");
#else
	SDL_LogWarn(0, "OpenGL renderer is not available in this build");
#endif
}

void Engine_ToggleFullscreen(void) {
	Uint32 flags = SDL_GetWindowFlags(ctx.wnd);
	flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
	SDL_SetWindowFullscreen(ctx.wnd, flags);
	if(SpawnScreen()) {
		SDL_LogCritical(0, "Failed to create SDL texture: %s", SDL_GetError());
		exit(1);
	}
}

void Engine_CycleIntegerScale(void) {
	int renderWidth = GRAPHICS_WIDTH, renderHeight = GRAPHICS_HEIGHT;
	if(ctx.screen)
		SDL_QueryTexture(ctx.screen, NULL, NULL, &renderWidth, &renderHeight);

	ctx.integerScale = (ctx.integerScale % 3) + 1;
	SDL_Log("Integer scaling: %dx", ctx.integerScale);
	if(!(SDL_GetWindowFlags(ctx.wnd) & SDL_WINDOW_FULLSCREEN_DESKTOP))
		SDL_SetWindowSize(
			ctx.wnd,
			renderWidth * ctx.integerScale,
			renderHeight * ctx.integerScale
		);
	if(SpawnScreen()) {
		SDL_LogCritical(0, "Failed to create SDL texture: %s", SDL_GetError());
		exit(1);
	}
}

void *Engine_GetWindow(void) {
	return ctx.wnd;
}

void Engine_GetObjects(Camera **cam, Map **map) {
	if(cam) *cam = &ctx.camera;
	if(map) *map = &ctx.map;
}

float Engine_GetDeltaTime(void) {
	return ctx.deltaTime;
}

void Engine_Stop(void) {
	ctx.stopped = 1;
}

void Engine_End(void) {
	DestroyPresentationBackend();
	PolygonRenderer_Destroy();
	SpriteRenderer_Destroy();
	Map_Close(&ctx.map);

#ifdef USE_SDL_IMAGE
	IMG_Quit();
#endif

	SDL_Quit();
}
