#include "engine.h"
#include "argparse.h"
#include <SDL_log.h>

#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

static int TestArg(char *arg, char *test, char *alias) {
	return SDL_strcasecmp(arg, test) == 0 ||
	SDL_strcasecmp(arg, alias) == 0;
}

// TODO: Finish this eventually
static char *HelpLines[] = {
	"",
	NULL
};

static int ShowHelp(void) {
	for(int i = 0; HelpLines[i]; i++)
		SDL_Log("%s", HelpLines[i]);
	return 1;
}

int CommandArgs_Parse(int argc, char *argv[], EngineSettings *es) {
	char *pathend = SDL_strrchr(argv[0], '\\');
	if(!pathend) pathend = SDL_strrchr(argv[0], '/');
	if(pathend) {
		*pathend = '\0';
		if(chdir(argv[0]) == -1)
			SDL_LogWarn(0, "Failed to change directory");
	}

	int cursor = 1;
	while(cursor < argc) {
		char pre, *argstr = argv[cursor++];
		if((pre = *argstr++) != '/' && pre != '-') {
			return ShowHelp();
		}
		if(TestArg(argstr, "diffuse", "dm")) {
			es->diffusemap = argv[cursor++];
		} else if(TestArg(argstr, "height", "hm")) {
			es->heightmap = argv[cursor++];
		} else if(TestArg(argstr, "ceildiffuse", "cdm")) {
			es->ceilingdiffusemap = argv[cursor++];
		} else if(TestArg(argstr, "ceilheight", "chm")) {
			es->ceilingheightmap = argv[cursor++];
		} else if(TestArg(argstr, "novsync", "nv")) {
			es->vsync = 0;
		} else if(TestArg(argstr, "winwidth", "ww")) {
			es->width = SDL_atoi(argv[cursor++]);
			if(!es->width) return ShowHelp();
		} else if(TestArg(argstr, "winheight", "wh")) {
			es->height = SDL_atoi(argv[cursor++]);
			if(!es->height) return ShowHelp();
		} else if(TestArg(argstr, "scale", "is")) {
			if(cursor >= argc) return ShowHelp();
			es->integerScale = SDL_atoi(argv[cursor++]);
			if(es->integerScale < 1 || es->integerScale > 3)
				return ShowHelp();
		} else if(TestArg(argstr, "benchmark", "bm")) {
			if(cursor < argc && argv[cursor] && argv[cursor][0] != '-' && argv[cursor][0] != '/')
				es->benchmarkSeconds = SDL_atoi(argv[cursor++]);
			else
				es->benchmarkSeconds = 10;
			if(es->benchmarkSeconds <= 0) return ShowHelp();
		} else if(TestArg(argstr, "renderer", "r")) {
			if(cursor >= argc) return ShowHelp();
			const char *renderer = argv[cursor++];
			if(SDL_strcasecmp(renderer, "software") == 0 || SDL_strcasecmp(renderer, "sw") == 0)
				es->renderer = RENDERER_SOFTWARE;
			else if(SDL_strcasecmp(renderer, "opengl") == 0 || SDL_strcasecmp(renderer, "gl") == 0)
				es->renderer = RENDERER_OPENGL;
			else return ShowHelp();
		} else if(TestArg(argstr, "sprite", "sp")) {
			if(cursor + 5 >= argc) return ShowHelp();
			es->spritePath = argv[cursor++];
			es->spriteX = SDL_atof(argv[cursor++]);
			es->spriteY = SDL_atof(argv[cursor++]);
			es->spriteZ = SDL_atof(argv[cursor++]);
			es->spriteWidth = SDL_atof(argv[cursor++]);
			es->spriteHeight = SDL_atof(argv[cursor++]);
			if(es->spriteWidth <= 0.0f || es->spriteHeight <= 0.0f) return ShowHelp();
#ifdef USE_THREADED_RENDER
		} else if(TestArg(argstr, "numthreads", "nt")) {
			es->numthreads = SDL_atoi(argv[cursor++]);
#endif
		} else return ShowHelp();
	}

	return 0;
}
