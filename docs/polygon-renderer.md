# Polygon renderer

`polygon_renderer.h` defines the geometry shared by the software and OpenGL
backends. Vertex `x` and `y` use the same coordinates as the heightmap, while
`z` is world height. Texture coordinates are stored in `u` and `v`.

Call `PolygonRenderer_AddTriangle` with a `PolygonTriangle`. Materials may use
`POLYGON_MATERIAL_FLAT` with a packed ARGB color, or
`POLYGON_MATERIAL_TEXTURED` with an ARGB pixel buffer. Texture storage belongs
to the caller and must remain valid while the triangle is in the scene.
Faces use counter-clockwise winding when viewed from outside. Back-facing
triangles are rejected before projection in both backends. Set a material's
`doubleSided` field to a nonzero value when both sides must remain visible.

`PolygonRenderer_Clear` removes all submitted geometry. After changing the
software scene, set `map->redraw` so the terrain texture and its depth buffer
are rebuilt before the new polygons are rasterized.

The software backend uses barycentric triangle rasterization, perspective-
correct UV interpolation and a camera-space depth buffer shared with the voxel
terrain. Projected triangles are distributed over 32x32-pixel tiles. Persistent
worker threads claim tiles atomically, and each tile owns its color and depth
region, avoiding locks and races at pixel level. The main thread also consumes
tiles while workers run. Builds without `USE_THREADED_RENDER` use the same
rasterizer through a single full-screen tile. When `USE_AVX2` is available,
each worker evaluates edge functions, barycentric coordinates, depth and UVs
for eight horizontal pixels at once. Flat materials use masked vector writes;
textured materials use AVX2 gather for their texels. The scalar path remains
the fallback for unsupported CPUs and incomplete eight-pixel spans. The OpenGL
backend writes terrain ray depth to the hardware depth buffer, then draws the
same triangles with depth testing.

The initial implementation supports opaque triangles. Near-plane polygon
clipping, alpha blending, model-file loading, lighting and batching by texture
are future extensions.
