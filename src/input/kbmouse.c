#include <SDL_scancode.h>
#include <SDL_keycode.h>

#include "../defines.h"
#include "../engine.h"
#include "../camera.h"

extern int isOnTheGround;
extern float velocity;

static SDL_Scancode input[INPUT_MAX_KEYBINDS] = {0};
static int isMouseGrabbed = 0, isSprintActive = 0;
static int isPickCursorActive = 0, restoreGrabAfterPick = 0;
static int leftCtrlHeld = 0, rightCtrlHeld = 0;

static inline void SetMouseGrab(int enabled) {
	isMouseGrabbed = enabled;
	SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE);
	SDL_SetWindowGrab(Engine_GetWindow(), enabled ? SDL_TRUE : SDL_FALSE);
	SDL_ShowCursor(enabled ? SDL_DISABLE : SDL_ENABLE);
}

/* Hold Ctrl to show the arrow cursor and pick/move sprites without looking. */
static void SetPickCursor(int enabled) {
	if(enabled) {
		if(isPickCursorActive)
			return;
		restoreGrabAfterPick = isMouseGrabbed;
		isPickCursorActive = 1;
		isMouseGrabbed = 0;
		SDL_SetRelativeMouseMode(SDL_FALSE);
		SDL_SetWindowGrab(Engine_GetWindow(), SDL_FALSE);
		SDL_ShowCursor(SDL_ENABLE);
		return;
	}
	if(!isPickCursorActive)
		return;
	isPickCursorActive = 0;
	if(restoreGrabAfterPick)
		SetMouseGrab(1);
	else {
		SDL_ShowCursor(SDL_ENABLE);
		SDL_SetRelativeMouseMode(SDL_FALSE);
		SDL_SetWindowGrab(Engine_GetWindow(), SDL_FALSE);
	}
	restoreGrabAfterPick = 0;
}

static void UpdatePickCursorFromCtrl(void) {
	SetPickCursor(leftCtrlHeld || rightCtrlHeld);
}

static void ProcessKeyDown(SDL_Scancode code, Uint16 mod) {
	Camera *cam = NULL;
	Map *map = NULL;
	Engine_GetObjects(&cam, &map);
	switch(code) {
		case SDL_SCANCODE_W:
		case SDL_SCANCODE_S:
			input[0] = code;
			break;
		case SDL_SCANCODE_A:
		case SDL_SCANCODE_D:
			input[1] = code;
			break;
		case SDL_SCANCODE_R:
		case SDL_SCANCODE_F:
			input[3] = code;
			break;
		case SDL_SCANCODE_H:
		case SDL_SCANCODE_G:
			input[2] = code;
			break;
		case SDL_SCANCODE_SPACE:
			map->redraw = 1;
			if(mod & KMOD_CTRL) {
				Camera_ResetDistance(cam);
			} else if(isOnTheGround) {
				velocity = INPUT_JUMP_VELOCITY;
				isOnTheGround = 0;
			}
			break;
		case SDL_SCANCODE_LSHIFT:
			isSprintActive = 1;
			break;
		case SDL_SCANCODE_LCTRL:
			leftCtrlHeld = 1;
			UpdatePickCursorFromCtrl();
			break;
		case SDL_SCANCODE_RCTRL:
			rightCtrlHeld = 1;
			UpdatePickCursorFromCtrl();
			break;
		case SDL_SCANCODE_C:
			Camera_ResetPitch(cam);
			map->redraw = 1;
			break;
		case SDL_SCANCODE_J:
		case SDL_SCANCODE_K:
			Camera_AdjustZStep(cam, code == SDL_SCANCODE_J ? -1.0f : 1.0f);
			map->redraw = 1;
			break;
		case SDL_SCANCODE_O:
			if(mod & KMOD_CTRL) {
				map->optdist = 3000.0f;
				map->optimize ^= 1;
			} else
				map->ceilingEnabled ^= 1;
			map->redraw = 1;
			break;
		case SDL_SCANCODE_I:
			Engine_CycleIntegerScale();
			break;
		case SDL_SCANCODE_V:
			ToggleIsometricCamera();
			break;
		case SDL_SCANCODE_T:
			Engine_ToggleTerrainMode();
			break;
		case SDL_SCANCODE_F10:
			Engine_ToggleRenderer();
			break;
		case SDL_SCANCODE_RETURN:
			if((mod & KMOD_ALT) != 0)
				Engine_ToggleFullscreen();
			break;
		case SDL_SCANCODE_ESCAPE:
			if(isPickCursorActive) {
				leftCtrlHeld = rightCtrlHeld = 0;
				restoreGrabAfterPick = 0;
				SetPickCursor(0);
				break;
			}
			if(isMouseGrabbed) {
				SetMouseGrab(0);
				break;
			}

			Engine_Stop();
			break;
		default: break;
	}
}

static void ProcessKeyUp(SDL_Scancode code) {
	switch(code) {
		case SDL_SCANCODE_W:
		case SDL_SCANCODE_S:
			if(input[0] == code) input[0] = 0;
			break;
		case SDL_SCANCODE_A:
		case SDL_SCANCODE_D:
			if(input[1] == code) input[1] = 0;
			break;
		case SDL_SCANCODE_R:
		case SDL_SCANCODE_F:
			if(input[3] == code) input[3] = 0;
			break;
		case SDL_SCANCODE_H:
		case SDL_SCANCODE_G:
			if(input[2] == code) input[2] = 0;
			break;
		case SDL_SCANCODE_LSHIFT:
			isSprintActive = 0;
			break;
		case SDL_SCANCODE_LCTRL:
			leftCtrlHeld = 0;
			UpdatePickCursorFromCtrl();
			break;
		case SDL_SCANCODE_RCTRL:
			rightCtrlHeld = 0;
			UpdatePickCursorFromCtrl();
			break;
		default: break;
	}
}

static int ProcessKeyboard(Camera *cam, float dm) {
	if(input[3] && !isIsometric)
		Camera_Pitch(cam, (input[3] == SDL_SCANCODE_R ? -dm : dm));
	const float speed = (dm / INPUT_WALK_SPEED_DIVISOR) *
		(isSprintActive ? INPUT_SPRINT_MULTIPLIER : 1.0f);
	if(input[1]) {
		const float direction = input[1] == SDL_SCANCODE_A ? -speed : speed;
		Camera_StrafeHoriz(cam, direction);
	}
	if(input[0]) {
		const float direction = input[0] == SDL_SCANCODE_W ? -speed : speed;
		Camera_MoveForward(cam, direction, 1);
	}
	if(input[2]) {
		const float direction = input[2] == SDL_SCANCODE_H ? speed : -speed;
		if(isIsometric) {
			isoHeightOffset = max(
				CAMERA_EYE_HEIGHT,
				min(isoHeightOffset + direction * CAMERA_MOVE_STEP, CAMERA_HEIGHT_MAX)
			);
		} else {
			Camera_StrafeVert(cam, direction);
			if(direction > 0.0f)
				isOnTheGround = 0;
		}
	}

	/*
		Redraw the world if at least one
		registered key is pressed.
	*/
	for(int i = 0; i < INPUT_MAX_KEYBINDS; i++)
		if(input[i]) return 1;

	return 0;
}

static void ProcessMouseMotion(Sint32 rx, Sint32 ry) {
	Map *map = NULL;
	Camera *cam = NULL;
	Engine_GetObjects(&cam, &map);
	if(isIsometric) {
		cam->angle -= (float)rx * INPUT_MOUSE_SENS;
		map->redraw = 1;
		return;
	}
	cam->angle -= (float)rx * INPUT_MOUSE_SENS;
	cam->horizon -= (float)ry * cam->maxhorizon * INPUT_MOUSE_SENS;
	Camera_ClampPitch(cam);
	map->redraw = 1;
}
