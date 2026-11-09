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

`PolygonRenderer_DrawTriangleList` (and the OpenGL counterpart) projects and
rasterizes an arbitrary triangle array through the same near-clip and tile
path used for buildings. The optional heightmap terrain path in
`terrain_mesh.c` builds a separate grid mesh (default step 8) textured with
the floor diffuse map (world-space UVs), and draws it with that API before
building triangles, so the two lists stay independent. Geometry is still
coarser than per-pixel raycast; ceiling polygons remain out of scope for v1.

Press **T** to toggle between voxel raycast terrain (`Map_Draw` / map
raymarch) and polygon terrain. In polygon mode the frame is cleared to sky /
far depth, then the terrain mesh, buildings, and sprites share the same depth
buffer. Rebuild happens on map load and drag-and-drop. Ceiling polygon
terrain is out of scope for v1.

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
backend writes terrain ray depth (or the polygon terrain mesh) to the hardware
depth buffer, then draws building triangles with depth testing.

Near-plane polygon clipping is implemented. Alpha blending, model-file
loading, lighting and batching by texture remain future extensions.
