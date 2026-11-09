#include <SDL_events.h>
#include <SDL_stdinc.h>
#include <SDL_log.h>
#include "dragndrop.h"
#include "../engine.h"
#define VSERROR_STRINGS
#include "../error.h"
#include "../map.h"
#include "../polygon_renderer.h"
#include "../terrain_mesh.h"

char *droppedFile = NULL, droppedFileType = 0;

void DND_Window(void *ptr) {
	if(ptr) SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
}

void DND_Event(void *ptr) {
	SDL_Event *ev = (SDL_Event *)ptr;
	char *tmpsymptr = NULL, typesym = 0;
	const char *diffuse = NULL, *height = NULL;
	Camera *cam = NULL;
	Map *map = NULL;
	switch(ev->type) {
		case SDL_DROPFILE:
			tmpsymptr = SDL_strrchr(ev->drop.file, '\\');
			if(!tmpsymptr) tmpsymptr = SDL_strrchr(ev->drop.file, '/');
			typesym = tmpsymptr ? *++tmpsymptr : *ev->drop.file;
			if(typesym == 'C' || typesym == 'c' || typesym == 'D' || typesym == 'd') {
				if(droppedFile) {
					if(droppedFileType == typesym) {
						SDL_free(droppedFile);
						droppedFile = NULL;
						return;
					}
				} else {
					droppedFile = ev->drop.file;
					droppedFileType = typesym;
					return;
				}

				Engine_GetObjects(&cam, &map);
				Map_Close(map);
				if(typesym == 'C' || typesym == 'c')
					diffuse = ev->drop.file, height = droppedFile;
				else
					diffuse = droppedFile, height = ev->drop.file;

				Errors ret;
				if((ret = Map_Open(map, diffuse, height)) != ERROR_OK)
					SDL_LogError(0, "Failed to load map: %s", Errors_Strings[ret]);
				else {
					char buildingsPath[512];
					PolygonRenderer_Clear();
					PolygonRenderer_BuildingsPathFromHeightmap(
						height, buildingsPath, sizeof(buildingsPath));
					PolygonRenderer_LoadBuildings(map, buildingsPath);
					TerrainMesh_Build(map, TERRAIN_MESH_STEP_DEFAULT);
					cam->height = (float)Map_GetHeight(map, &cam->position) + CAMERA_EYE_HEIGHT;
					map->redraw = 1;
				}

				SDL_free(ev->drop.file);
				SDL_free(droppedFile);
				droppedFile = NULL;
			}
			break;
	}
}
