#include "picking.h"
#include "defines.h"
#include "engine.h"
#include "map.h"
#include "polygon_renderer.h"
#include "sprite_renderer.h"
#include <SDL_log.h>
#include <SDL_stdinc.h>

static struct {
	int selected;
	int moving;
	float targetX, targetY;
} picking = {
	.selected = -1,
	.moving = 0
};

void Picking_ClearSelection(void) {
	picking.selected = -1;
	picking.moving = 0;
}

int Picking_GetSelectedIndex(void) {
	return picking.selected;
}

static int WindowToRender(
	int windowX, int windowY, int *renderX, int *renderY,
	int *renderWidth, int *renderHeight
) {
	int windowWidth = 0, windowHeight = 0, integerScale = 1;
	Engine_GetPresentation(renderWidth, renderHeight, &windowWidth, &windowHeight, &integerScale);
	if(*renderWidth <= 0 || *renderHeight <= 0 || integerScale <= 0)
		return 0;

	const int scaledWidth = *renderWidth * integerScale;
	const int scaledHeight = *renderHeight * integerScale;
	const int offsetX = (windowWidth - scaledWidth) / 2;
	const int offsetY = (windowHeight - scaledHeight) / 2;
	if(windowX < offsetX || windowY < offsetY ||
		windowX >= offsetX + scaledWidth || windowY >= offsetY + scaledHeight)
		return 0;

	*renderX = (windowX - offsetX) / integerScale;
	*renderY = (windowY - offsetY) / integerScale;
	return 1;
}

static float SupportHeightAt(Map *map, float x, float y) {
	Point p = { x, y };
	const float terrain = map && map->ready ? (float)Map_GetHeight(map, &p) : 0.0f;
	const float polygon = PolygonRenderer_GetSupportHeight(x, y);
	return max(terrain, polygon);
}

/* Billboard screen bounds matching SpriteRenderer_DrawSoftware. */
static int SpriteScreenBounds(
	Camera *camera, int renderWidth, int renderHeight, int index,
	int *x0, int *y0, int *x1, int *y1, float *outDepth
) {
	float x = 0.0f, y = 0.0f, z = 0.0f, width = 0.0f, height = 0.0f;
	if(!SpriteRenderer_GetPose(index, &x, &y, &z, &width, &height))
		return 0;

	const float sine = SDL_sinf(camera->angle), cosine = SDL_cosf(camera->angle);
	const float dx = x - camera->position.x, dy = y - camera->position.y;
	const float depth = (-sine * dx) - (cosine * dy);
	const float lateral = (cosine * dx) - (sine * dy);
	if(depth <= 1.0f || depth >= camera->distance)
		return 0;

	const float scale = (float)renderHeight / CAMERA_PROJECTION_SCALE;
	const float cx = (lateral / depth + 1.0f) * (float)renderWidth * 0.5f;
	const float half = width / depth * (float)renderWidth * 0.5f;
	const float top = (camera->height - (z + height)) * scale / depth + camera->horizon;
	const float bottom = (camera->height - z) * scale / depth + camera->horizon;
	*x0 = max((int)SDL_floorf(cx - half), 0);
	*x1 = min((int)SDL_ceilf(cx + half), renderWidth);
	*y0 = max((int)SDL_floorf(top), 0);
	*y1 = min((int)SDL_ceilf(bottom), renderHeight);
	if(*x0 >= *x1 || *y0 >= *y1 || bottom <= top)
		return 0;
	if(outDepth)
		*outDepth = depth;
	return 1;
}

static int PickSpriteAt(Camera *camera, int renderWidth, int renderHeight, int rx, int ry) {
	int best = -1;
	float bestDepth = CAMERA_DISTANCE_MAX + 1.0f;
	const int count = SpriteRenderer_GetCount();
	for(int i = 0; i < count; i++) {
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		float depth = 0.0f;
		if(!SpriteScreenBounds(camera, renderWidth, renderHeight, i, &x0, &y0, &x1, &y1, &depth))
			continue;
		if(rx < x0 || rx >= x1 || ry < y0 || ry >= y1)
			continue;
		if(depth < bestDepth) {
			bestDepth = depth;
			best = i;
		}
	}
	return best;
}

static void RayPointAtDepth(
	Camera *camera, float sine, float cosine, float ndcX, float depth,
	float *outX, float *outY
) {
	const float lateral = ndcX * depth;
	*outX = camera->position.x + (cosine * lateral) - (sine * depth);
	*outY = camera->position.y + (-sine * lateral) - (cosine * depth);
}

/*
	Walk the camera ray for pixel (rx, ry) and find where it meets the ground
	(terrain / polygon support), matching the software projection.
*/
static int UnprojectGround(
	Camera *camera, Map *map, int renderWidth, int renderHeight,
	int rx, int ry, float *outX, float *outY
) {
	const float sine = SDL_sinf(camera->angle), cosine = SDL_cosf(camera->angle);
	const float scale = (float)renderHeight / CAMERA_PROJECTION_SCALE;
	const float ndcX = ((float)rx + 0.5f) / (float)renderWidth * 2.0f - 1.0f;
	const float targetY = (float)ry + 0.5f;

	float prevDepth = 1.0f, prevSy = 0.0f, prevX = 0.0f, prevY = 0.0f;
	int hasPrev = 0;
	/* Keep the step small enough for click accuracy on the ground plane. */
	const float step = max(2.0f, min(8.0f, camera->distance / 256.0f));

	for(float depth = 1.0f; depth < camera->distance; depth += step) {
		float wx = 0.0f, wy = 0.0f;
		RayPointAtDepth(camera, sine, cosine, ndcX, depth, &wx, &wy);
		const float ground = SupportHeightAt(map, wx, wy);
		const float sy = (camera->height - ground) * scale / depth + camera->horizon;

		if(hasPrev && (prevSy - targetY) * (sy - targetY) <= 0.0f) {
			const float denom = sy - prevSy;
			const float t = SDL_fabsf(denom) > 1e-4f ? (targetY - prevSy) / denom : 0.0f;
			const float clamped = max(0.0f, min(t, 1.0f));
			*outX = prevX + (wx - prevX) * clamped;
			*outY = prevY + (wy - prevY) * clamped;
			(void)prevDepth;
			return 1;
		}

		prevDepth = depth;
		prevSy = sy;
		prevX = wx;
		prevY = wy;
		hasPrev = 1;
	}

	/* No screen-Y crossing: use the farthest sample on the ray. */
	if(hasPrev) {
		*outX = prevX;
		*outY = prevY;
		return 1;
	}

	RayPointAtDepth(
		camera, sine, cosine, ndcX,
		min(camera->distance * 0.75f, camera->distance - 1.0f),
		outX, outY
	);
	return 1;
}

int Picking_HandleClick(int windowX, int windowY) {
	Camera *camera = NULL;
	Map *map = NULL;
	Engine_GetObjects(&camera, &map);
	if(!camera || !map)
		return 0;

	int renderWidth = 0, renderHeight = 0, rx = 0, ry = 0;
	if(!WindowToRender(windowX, windowY, &rx, &ry, &renderWidth, &renderHeight))
		return 0;

	const int hit = PickSpriteAt(camera, renderWidth, renderHeight, rx, ry);
	if(hit >= 0) {
		picking.selected = hit;
		picking.moving = 0;
		SDL_Log("Picking: selected sprite %d", hit);
		map->redraw = 1;
		return 1;
	}

	if(picking.selected < 0)
		return 0;

	float targetX = 0.0f, targetY = 0.0f;
	if(!UnprojectGround(camera, map, renderWidth, renderHeight, rx, ry, &targetX, &targetY))
		return 0;

	picking.targetX = targetX;
	picking.targetY = targetY;
	picking.moving = 1;
	SDL_Log("Picking: move sprite %d to (%.1f, %.1f)", picking.selected, targetX, targetY);
	map->redraw = 1;
	return 1;
}

void Picking_Update(float deltaMilliseconds) {
	if(!picking.moving || picking.selected < 0)
		return;

	float x = 0.0f, y = 0.0f;
	if(!SpriteRenderer_GetPose(picking.selected, &x, &y, NULL, NULL, NULL)) {
		Picking_ClearSelection();
		return;
	}

	const float dx = picking.targetX - x;
	const float dy = picking.targetY - y;
	const float dist = SDL_sqrtf(dx * dx + dy * dy);
	if(dist <= PICKING_ARRIVE_EPSILON) {
		SpriteRenderer_SetPosition(picking.selected, picking.targetX, picking.targetY);
		picking.moving = 0;
		Map *map = NULL;
		Engine_GetObjects(NULL, &map);
		if(map) map->redraw = 1;
		return;
	}

	const float frameSeconds = deltaMilliseconds * 0.001f;
	const float step = min(PICKING_MOVE_SPEED * frameSeconds, dist);
	SpriteRenderer_SetPosition(
		picking.selected,
		x + dx / dist * step,
		y + dy / dist * step
	);
	Map *map = NULL;
	Engine_GetObjects(NULL, &map);
	if(map) map->redraw = 1;
}
