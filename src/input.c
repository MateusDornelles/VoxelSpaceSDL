#include <SDL_events.h>
#include <SDL_stdinc.h>
#include "defines.h"
#include "engine.h"
#include "camera.h"
#include "map.h"
#include "input.h"

static int isOnTheGround = 0;
static float velocity = 0.0f;

#include "input/kbmouse.c"
#include "input/gamepad.c"

void Input_Start(void *ptr) {
	(void)ptr;
	Map *map = NULL;
	Camera *cam = NULL;
	Engine_GetObjects(&cam, &map);
	cam->height = (float)Map_GetHeight(map, &cam->position) + CAMERA_EYE_HEIGHT;
	Camera_ResetPitch(cam);
	isOnTheGround = 1;
	velocity = 0.0f;
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

	const float groundHeight =
		(float)Map_GetHeight(map, &cam->position) + CAMERA_EYE_HEIGHT;
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

	// Pull the camera above terrain if it went below ground
	cam->height = max(groundHeight, min(cam->height, CAMERA_HEIGHT_MAX));
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
