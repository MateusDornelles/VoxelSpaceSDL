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
#define GRAPHICS_WIDTH 640 // SDL window width
#define GRAPHICS_HEIGHT 360 // SDL window height
#define GRAPHICS_SKY_COLOR 0x9090E0FF // Background and distance fog color

#define RENDER_HOT_WORKERS 1 // Workers allowed to spin between continuous frames
#define RENDER_WORKER_SPIN_US 1000 // Hot-wait budget before workers sleep
#define RENDER_COMPLETION_SPIN_US 25 // Avoid sleeping near the end of a frame
#define RENDER_SPIN_CHECK_INTERVAL 64 // Pause instructions between clock checks

#define INPUT_MOUSE_SENS 0.0022f // Mouse sensitivity for camera control
#define INPUT_TOUCH_SENS 80.0f // Gamepad touchpad sensitivity
#define INPUT_MAX_PADS 8 // Maximum number of supported gamepads
#define INPUT_MAX_KEYBINDS 4 // Number of engine keybind slots
#define INPUT_GRAVITY_ACCELERATION 120.0f // Downward acceleration in terrain units per second squared
#define INPUT_JUMP_VELOCITY 45.0f // Initial upward jump velocity
#define INPUT_WALK_SPEED_DIVISOR 2.5f // Converts the legacy movement step to walking speed
#define INPUT_SPRINT_MULTIPLIER 2.0f // Running speed relative to walking

#define CAMERA_MOVE_STEP 4.0f // Camera movement speed
#define CAMERA_ANGLE_STEP 0.08f // Camera rotation step
#define CAMERA_HORIZON_STEP 0.0383f // Camera pitch adjustment step
#define CAMERA_PROJECTION_SCALE 2.5f // Vertical projection divisor used by the renderer
#define CAMERA_MAX_PITCH 0.78539816339f // Maximum look angle: 45 degrees in radians
#define CAMERA_HEIGHT_DEFAULT 178.0f // Default camera height
#define CAMERA_EYE_HEIGHT 9.0f // Eye position above the terrain
#define CAMERA_HEIGHT_MAX 30000.0f // Maximum camera height
#define CAMERA_HEIGHT_MOD 0.02f // Influence of horizon line on movement vector
#define CAMERA_DISTANCE_DEFAULT 600.0f // Default render distance
#define CAMERA_DISTANCE_MIN 300.0f // Minimum render distance
#define CAMERA_DISTANCE_MAX 30000.0f // Maximum render distance
#define CAMERA_DISTANCE_STEP 75.0f // Render distance adjustment step
#define CAMERA_FOG_START_RATIO 0.5833f // Fog starts at roughly 350 units at default distance
#define CAMERA_FOG_END_RATIO 0.9667f // Fog reaches the sky color before the draw cutoff
#define CAMERA_LOD_START_RATIO 0.5f // Gradually reduce depth detail in the far half
#define CAMERA_LOD_STRENGTH 0.35f // Far-distance depth step multiplier
#define CAMERA_POSITION_DEFAULT POINT_MAKE(512.0f, 800.0f) // Default camera position
#define CAMERA_ZSTEP_DEFAULT 0.002f // Default Z-axis step
#define CAMERA_ZSTEP_MIN 0.001f // Minimum Z-axis step
#define CAMERA_ZSTEP_MAX 0.16f // Above this value, the image becomes a pixel mess
#define CAMERA_ZSTEP_MOD 0.001f // This define name sounds intimidating
#endif
