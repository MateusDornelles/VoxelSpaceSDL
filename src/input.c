#include <SDL_events.h>
#include <SDL_stdinc.h>
#include "defines.h"
#include "engine.h"
#include "camera.h"
#include "map.h"
#include "polygon_renderer.h"
#include "input.h"

static int isOnTheGround = 0;
static float velocity = 0.0f;
static int isIsometric = 0;
static float isoHeightOffset = CAMERA_ISO_HEIGHT;
static struct {
	float angle, horizon, height;
} isoSaved;

static void ToggleIsometricCamera(void);

#include "input/kbmouse.c"
#include "input/gamepad.c"

static void ApplyIsometricCamera(Camera *cam, Map *map) {
	const float terrainHeight = (float)Map_GetHeight(map, &cam->position);
	const float polygonHeight = PolygonRenderer_GetSupportHeight(
		cam->position.x, cam->position.y
	);
	const float support = max(terrainHeight, polygonHeight);
	const float center = cam->maxhorizon * 0.5f;
	const float extent =
		(cam->maxhorizon / CAMERA_PROJECTION_SCALE) * SDL_tanf(CAMERA_MAX_PITCH);

	/* Keep yaw free for mouse orbit; lock pitch and altitude. */
	cam->horizon = center - extent * CAMERA_ISO_LOOK_DOWN;
	cam->height = support + isoHeightOffset;
	Camera_ClampPitch(cam);
	velocity = 0.0f;
	isOnTheGround = 1;
}

static void ToggleIsometricCamera(void) {
	Map *map = NULL;
	Camera *cam = NULL;
	Engine_GetObjects(&cam, &map);
	if(!isIsometric) {
		isoSaved.angle = cam->angle;
		isoSaved.horizon = cam->horizon;
		isoSaved.height = cam->height;
		isIsometric = 1;
		isoHeightOffset = CAMERA_ISO_HEIGHT;
		cam->angle = CAMERA_ISO_ANGLE;
		ApplyIsometricCamera(cam, map);
		SDL_Log("Isometric camera: on");
	} else {
		cam->angle = isoSaved.angle;
		cam->horizon = isoSaved.horizon;
		cam->height = isoSaved.height;
		Camera_ClampPitch(cam);
		isIsometric = 0;
		SDL_Log("Isometric camera: off");
	}
	map->redraw = 1;
}

void Input_Start(void *ptr) {
	(void)ptr;
	Map *map = NULL;
	Camera *cam = NULL;
	Engine_GetObjects(&cam, &map);
	cam->height = (float)Map_GetHeight(map, &cam->position) + CAMERA_EYE_HEIGHT;
	Camera_ResetPitch(cam);
	isOnTheGround = 1;
	velocity = 0.0f;
	isIsometric = 0;
	SetMouseGrab(1);
	map->redraw = 1;
}

void Input_Update(void *ptr) {
	float delta = *(float *)ptr;
	Map *map = NULL;
	Camera *cam = NULL;
	Engine_GetObjects(&cam, &map);

	// Request a world redraw if any control key was pressed
	if(PollControllers(cam, delta * 0.03f) || ProcessKeyboard(cam, delta * 0.03f))
		map->redraw = 1;

	float feetZ = cam->height - CAMERA_EYE_HEIGHT;
	PolygonRenderer_ResolvePosition(
		&cam->position.x, &cam->position.y,
		feetZ, cam->height, INPUT_COLLISION_RADIUS
	);

	const float terrainHeight = (float)Map_GetHeight(map, &cam->position);
	const float polygonHeight = PolygonRenderer_GetSupportHeight(
		cam->position.x, cam->position.y
	);
	const float groundHeight =
		max(terrainHeight, polygonHeight) + CAMERA_EYE_HEIGHT;
	if(!isOnTheGround) {
		const float frameSeconds = delta * 0.001f;
		velocity -= INPUT_GRAVITY_ACCELERATION * frameSeconds;
		cam->height += velocity * frameSeconds;
		map->redraw = 1;
	}

	if(cam->height <= groundHeight) {
		cam->height = groundHeight;
		isOnTheGround = 1;
		velocity = 0.0f;
	} else if(isOnTheGround) {
		// Walking over a descending slope starts a short fall.
		isOnTheGround = 0;
	}

	// Pull the camera above terrain / polygon tops if it went below ground
	cam->height = max(groundHeight, min(cam->height, CAMERA_HEIGHT_MAX));
	feetZ = cam->height - CAMERA_EYE_HEIGHT;
	PolygonRenderer_ResolvePosition(
		&cam->position.x, &cam->position.y,
		feetZ, cam->height, INPUT_COLLISION_RADIUS
	);
	if(isIsometric) {
		ApplyIsometricCamera(cam, map);
		map->redraw = 1;
	}
	// Reset camera angle after a full turn
	if(SDL_fabsf(cam->angle) > M_PI * 2) cam->angle = 0;
}

void Input_Event(void *ptr) {
	SDL_Event *ev = (SDL_Event *)ptr;
	switch(ev->type) {
		case SDL_CONTROLLERDEVICEADDED:
			AddController(ev->cdevice.which);
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			RemoveController(ev->cdevice.which);
			break;
		case SDL_KEYDOWN:
			ProcessKeyDown(ev->key.keysym.scancode, ev->key.keysym.mod);
			break;
		case SDL_KEYUP:
			ProcessKeyUp(ev->key.keysym.scancode);
			break;
		case SDL_MOUSEBUTTONDOWN:
			if(ev->button.button == SDL_BUTTON_LEFT && !isMouseGrabbed)
				SetMouseGrab(1);
			break;
		case SDL_MOUSEMOTION:
			if(isMouseGrabbed)
				ProcessMouseMotion(ev->motion.xrel, ev->motion.yrel);
			break;
	}
}
