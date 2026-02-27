#ifndef VSCONSTANTS_H
#define VSCONSTANTS_H
#ifndef min
#define min(a, b) (((a)<(b))?(a):(b))
#define max(a, b) (((a)>(b))?(a):(b))
#endif

#ifndef NULL
#define NULL (void *)0
#endif

#define POINT_ADD(a, b) (a).x += (b).x; (a).y += (b).y;
#define POINT_MAKE(_x, _y) {.x=_x, .y=_y}

#ifdef USE_THREADED_RENDER
#define GRAPHICS_TITLE "VoxelSpace SDL (MT)"
#else
#define GRAPHICS_TITLE "VoxelSpace SDL"
#endif
#define GRAPHICS_WIDTH 960 // SDL window width
#define GRAPHICS_HEIGHT 540 // SDL window height

#define INPUT_MOUSE_SENS 0.0022f // Mouse sensitivity for camera control
#define INPUT_TOUCH_SENS 80.0f // Gamepad touchpad sensitivity
#define INPUT_MAX_PADS 8 // Maximum number of supported gamepads
#define INPUT_MAX_KEYBINDS 4 // Number of engine keybind slots
#define INPUT_GRAVITATION_MULT 9.81f // Gravity multiplier
#define INPUT_JUMP_VELOCITY 3.0f // Jump force in gravity mode

#define CAMERA_MOVE_STEP 4.0f // Camera movement speed
#define CAMERA_ANGLE_STEP 0.08f // Camera rotation step
#define CAMERA_HORIZON_STEP 0.0383f // Camera pitch adjustment step
#define CAMERA_HEIGHT_DEFAULT 178.0f // Default camera height
#define CAMERA_HEIGHT_MAX 30000.0f // Maximum camera height
#define CAMERA_HEIGHT_MOD 0.02f // Influence of horizon line on movement vector
#define CAMERA_DISTANCE_DEFAULT 3000.0f // Default render distance
#define CAMERA_DISTANCE_MIN 300.0f // Minimum render distance
#define CAMERA_DISTANCE_MAX 30000.0f // Maximum render distance
#define CAMERA_DISTANCE_STEP 150.0f // Render distance adjustment step
#define CAMERA_POSITION_DEFAULT POINT_MAKE(512.0f, 800.0f) // Default camera position
#define CAMERA_ZSTEP_DEFAULT 0.002f // Default Z-axis step
#define CAMERA_ZSTEP_MIN 0.001f // Minimum Z-axis step
#define CAMERA_ZSTEP_MAX 0.16f // Above this value, the image becomes a pixel mess
#define CAMERA_ZSTEP_MOD 0.001f // This define name sounds intimidating
#endif
