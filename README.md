# Voxel Space in C

## Description

Comanche's Voxel Space rendering algorithm written in C using SDL. You can try it [here](https://igvx.ru/vs_sdl/) (browser demo).

![Code_2022-02-02_14-48-09](https://user-images.githubusercontent.com/40758030/152148072-e869ee1a-53e6-4c21-9900-de16a2f9fc67.png)


## Bindings

### Keyboard + Mouse

* G - Toggle gravitation
* LMB (hold) - Free look
* LMB (double click) - Enable persist free look
* ESC - Close application
* W/S - Move camera forward/backward
* A/D - Rotate camera or strafe (if LMB pressed)
* E/Q - Adjust camera height (Jump if gravtitation enabled)
* R/F - Adjust camera pitch
* J/K - Adjust details level
* LShift - Increase camera movement speed
* C - Reset camera pitch
* Space - Increase render distance
* Shift + Space - Decrease render distance
* LCTRL + Space - Reset render distance to default value
* Alt + Enter - Toggle fullscreen
* F10 - Switch between software and OpenGL rendering

### Game Controller

* BACK - Toggle gravitation
* LS - Move camera
* RS - Rotate camera
* RSB - Reset camera pitch
* LT - Increase camera movement speed
* RT - Increase camera rotation speed
* LB - Decrease render distance
* RB - Increase render distance
* A - Jump (if gravitation enabled)
* B - Close application
* Y - Reset render distance to default value
* D-Pad Up/Down - Adjust camera height
* Touchpad - Free look

## Building

### Rendering backends

Desktop builds include both the SDL software renderer and the OpenGL 3.3 Core
renderer. The software backend is the default; select either backend when the
program starts:

```sh
./build/bin/vs -renderer software
./build/bin/vs -renderer opengl
```

The OpenGL backend lives in `src/opengl_renderer.c` and does not replace or
modify the software map renderer. Short forms `-r sw` and `-r gl` are also
accepted. Emscripten builds provide the software backend only, and the SDL_ttf
overlay is shown only by the software backend.

Press `F10` while the program is running to switch backends. The map, camera,
window size and fullscreen state are preserved.

### Arguments supported by build scripts

* ``dbg`` - Build with debug symbols;
* ``wall`` - Enable all compiler warnings;
* ``winf`` - Increase warning level;
* ``woff`` - Disable all compiler warnings;
* ``threaded`` - Build with multithreaded renderer;
* ``avx2`` - Build with AVX2 optimizations (x86_64 native builds only);
* ``sdlimage`` - Build with png/jpg/... images support;
* ``overlay`` - Build with onscreen information module;
* ``web`` - Build for browsers using [emscripten](https://emscripten.org/);
* ``run`` - Run compiled app if build successful.

### Build scripts usage

* On Windows you need to run the ``build.bat`` batch script in Visual Studio Developer Environment. Or... just open Visual Studio Code and press F5 button. If you need png image support, run ``build.bat sdlimage``.
* On Linux you need to run the ``build.sh`` bash script. If you need png image support, run ``build.sh sdlimage``;
* Almost all build agrguments can be combined.

## Notes

* You can change map by dropping two images (height map and diffuse map) on the Voxel Space window;
* The first letter in file name tells to the VoxelSpace how to use dropped file. ``D`` - use as a height map, and ``C`` - use as a diffuse map;
* Also you can change default map by passing commandline arguments to VoxelSpace: ``vs.exe -dm "path/to/diffuse.png"  -hm "path/to/height.png"`` or ``vs.exe /diffuse "path/to/image.png" /height "path/to/image.png"``;
* Benchmark mode is available via ``-benchmark [seconds]`` (alias: ``-bm``; default duration is 10 seconds);
* Integer window scaling is available via ``-scale 1|2|3`` (alias: ``-is``). For example, the default 640x360 render size opens at 1280x720 with ``-scale 2`` and 1920x1080 with ``-scale 3``;
* FPS camera controls: mouse to look, WASD to walk, Left Shift to sprint, Space to jump, and Escape to release the mouse;
* So far there are no VoxelSpace commandline arguments list, see ``src/argparse.c`` for more information.

## Thanks

Thanks to [s-macke](https://github.com/s-macke/VoxelSpace/) for the inspiration and excellent technology explanation :)

## License

The software part of the repository is under the MIT license. Please read the license file for more information. Please keep in mind, that the Voxel Space technology might be still [patented](https://patents.justia.com/assignee/novalogic-inc) in some countries. The color and height maps are reverse engineered from the game Comanche and are therefore excluded from the license.
