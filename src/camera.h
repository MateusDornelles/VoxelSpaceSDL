#ifndef VSCAMERA_H
#define VSCAMERA_H
#include <SDL_stdinc.h>
#include "defines.h"

typedef struct sCamera {
	Point position; // Current camera position
	float height; // Camera height above the map
	float distance; // Render distance
	float maxhorizon; // Maximum horizon level
	float horizon; // Horizon level
	float angle; // Camera angle
	float zstep; // Z-axis step
} Camera;

static inline void Camera_AdjustDistance(Camera *cam, float value) {
	cam->distance += value;
	if(cam->distance < CAMERA_DISTANCE_MIN)
		cam->distance = CAMERA_DISTANCE_MAX;
	else if (cam->distance > CAMERA_DISTANCE_MAX)
		cam->distance = CAMERA_DISTANCE_MIN;
}

static inline void Camera_AdjustZStep(Camera *cam, float dir) {
	cam->zstep += dir * CAMERA_ZSTEP_MOD;
	cam->zstep = min(max(cam->zstep, CAMERA_ZSTEP_MIN), CAMERA_ZSTEP_MAX);
}

static inline void Camera_ResetDistance(Camera *cam) {
	cam->distance = CAMERA_DISTANCE_DEFAULT;
}

static inline void Camera_StrafeHoriz(Camera *cam, float spd) {
	cam->position.x +=  spd * CAMERA_MOVE_STEP * SDL_cosf(cam->angle);
	cam->position.y +=  spd * CAMERA_MOVE_STEP * SDL_sinf(-cam->angle);
}

static inline void Camera_StrafeVert(Camera *cam, float spd) {
	cam->height += spd * CAMERA_MOVE_STEP;
}

static inline void Camera_ClampPitch(Camera *cam) {
	const float center = cam->maxhorizon * 0.5f;
	const float extent = (cam->maxhorizon / CAMERA_PROJECTION_SCALE) * SDL_tanf(CAMERA_MAX_PITCH);
	cam->horizon = max(center - extent, min(cam->horizon, center + extent));
}

static inline void Camera_Pitch(Camera *cam, float spd) {
	cam->horizon -= spd * cam->maxhorizon * CAMERA_HORIZON_STEP;
	Camera_ClampPitch(cam);
}

static inline void Camera_ResetPitch(Camera *cam) {
	cam->horizon = cam->maxhorizon / 2.0f;
}

static inline void Camera_Rotate(Camera *cam, float spd) {
	cam->angle -= spd * CAMERA_ANGLE_STEP;
}

static inline void Camera_MoveForward(Camera *cam, float spd, int modHeight) {
	if(modHeight) {
		cam->position.x += spd * CAMERA_MOVE_STEP * SDL_sinf(cam->angle);
		cam->position.y += spd * CAMERA_MOVE_STEP * SDL_cosf(cam->angle);
		return;
	}

    float low = cam->horizon / cam->maxhorizon * 2 - 1;
    float low_module = SDL_sqrtf(1 + low*low);
    cam->position.x += spd * CAMERA_MOVE_STEP * SDL_sinf(cam->angle) / low_module;
    cam->position.y += spd * CAMERA_MOVE_STEP * SDL_cosf(cam->angle) / low_module;
	cam->height -= spd * CAMERA_MOVE_STEP * low / low_module;
}
#endif
