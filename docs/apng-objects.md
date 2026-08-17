# APNG objects

`sprite_renderer.h` provides animated, camera-facing billboard objects for
characters, vegetation and other map details. APNG frames are decoded and
composited by FFmpeg when the object is loaded, then advanced using each
frame's duration. The image's alpha channel is preserved.

Use `SpriteRenderer_AddAPNG(path, x, y, z, width, height)` to add an object.
Coordinates use the same world space as the heightmap and polygons. `z` is the
bottom of the object; `width` and `height` are its world dimensions. The return
value is a non-negative object ID, or `-1` on failure. Objects currently face
the camera automatically and loop forever.

For a quick test, the same fields can be passed at startup:

```sh
./build/bin/vs -renderer software -sprite maps/character.apng 512 700 170 24 40
./build/bin/vs -renderer opengl  -sprite maps/character.apng 512 700 170 24 40
```

Both renderers depth-test sprites against the voxel terrain and polygon scene.
The software backend blends APNG pixels directly into the internal framebuffer;
OpenGL uploads the active frame and draws a depth-tested billboard before the
integer-scaling pass.
