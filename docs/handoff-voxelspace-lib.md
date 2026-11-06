# Handoff: extrair `Map` como lib CPU → textura GPU

Documento de handoff para transformar o software renderer de `map.c` numa biblioteca independente que gera um framebuffer na CPU e o entrega para upload em **OpenGL** ou **Vulkan**.

Não inclui o backend atual de `opengl_renderer.c` (raymarch na GPU). O objetivo é o caminho **software**: CPU desenha a cena, a app sobe a imagem como textura.

---

## 1. Objetivo da lib

```
[diffuse + height] → Map_Open
[camera]           → Map_Draw → framebuffer ARGB8888 (+ depth opcional)
                   → host faz upload GL/VK → apresenta
```

A lib **não** deve depender de SDL Renderer, SDL_Texture, janela, input ou engine. Upload e apresentação ficam no host.

---

## 2. O que extrair (núcleo)

| Arquivo atual | Papel | Ação |
|---|---|---|
| `src/map.c` | Software voxel renderer | Extrair e adaptar |
| `src/map.h` | API / struct `Map` | Extrair e limpar |
| `src/camera.h` | Struct + helpers de câmera | Extrair (mínimo: struct + constantes usadas no draw) |
| `src/defines.h` | Constantes (`GRAPHICS_SKY_COLOR`, fog, LOD, zstep, etc.) | Extrair subset `voxel_constants.h` |
| `src/error.h` | Códigos de `Map_Open*` | Extrair |
| `Point` em `src/engine.h` | `typedef struct { float x, y; } Point` | Mover para header da lib (hoje acopla `map.h` ↔ `engine.h`) |

### Fora do escopo da v1

- `opengl_renderer.c` / shaders de terreno
- `polygon_renderer.*`, `sprite_renderer.*`
- `engine.c`, input, overlay, drag-and-drop, argparse
- Threads SDL (`USE_THREADED_RENDER`) — opcional na v2; v1 single-thread

### Dependências atuais a remover ou isolar

| Dependência | Onde | Como tratar |
|---|---|---|
| `SDL_LockTexture` / `SDL_UnlockTexture` / `SDL_QueryTexture` | `PrepareToDraw`, `Map_Draw`, `Map_SetScreen` | Substituir por framebuffer CPU explícito |
| `SDL_LoadBMP` / `IMG_Load` / `SDL_Surface` | `LoadLayer` | Callback de load **ou** API `Map_OpenFromPixels` |
| `SDL_calloc` / `SDL_free` / `SDL_memset` / `SDL_memset4` / `SDL_log10` | alocação / util | `malloc`/`free`/`memset` + `log2` local |
| `SDL_sinf` / `SDL_cosf` / `SDL_sqrtf` / `SDL_tanf` | `camera.h` | `math.h` (`sinf`, etc.) |
| `#include "engine.h"` | `map.c` | Remover; só precisa de `Point` |
| `exit(1)` em falha de alloc | `Map_SetScreen` | Retornar erro |
| AVX2 (`USE_AVX2`) | paths vetorizados | Keep como flag de build opcional; não exigir SDL_cpuinfo (usar CPUID próprio ou flag compile-time) |

---

## 3. Acoplamento crítico a quebrar

Hoje o target de desenho é um `void *screen` tratado como `SDL_Texture*`:

```c
// map.c — PrepareToDraw
SDL_LockTexture((SDL_Texture *)map->screen, NULL, (void **)pixels, pitch);
SDL_QueryTexture(..., NULL, height);
*pitch /= sizeof(int);
```

```c
// map.c — Map_SetScreen
SDL_QueryTexture(screen, ..., &width, &height);
map->hiddeny = ...; map->showny = ...; map->depth = ...;
```

Isso impede uso direto com Vulkan/OpenGL. A lib precisa de um **framebuffer opaco da lib**, não de textura de API gráfica.

---

## 4. API alvo sugerida

### Tipos

```c
typedef struct VsPoint { float x, y; } VsPoint;

typedef struct VsCamera {
    VsPoint position;
    float height;
    float distance;
    float maxhorizon;  /* tipicamente = (float)framebufferHeight */
    float horizon;
    float angle;
    float zstep;
} VsCamera;

typedef struct VsMap VsMap; /* opaco ou semi-opaco */

typedef struct VsFramebuffer {
    uint32_t *pixels;   /* ARGB8888, row-major */
    int width;
    int height;
    int pitch;          /* em pixels (não bytes) — igual ao uso interno atual */
    float *depth;       /* opcional; forward depth por pixel */
    int depth_owned;    /* se a lib alocou depth */
} VsFramebuffer;
```

### Funções

```c
int  vs_map_create(VsMap **out);
void vs_map_destroy(VsMap *map);

/* Load: ou paths (host fornece decoder), ou pixels crus */
int  vs_map_open(VsMap *map, const char *diffuse, const char *height);
int  vs_map_open_dual(VsMap *map,
        const char *diffuse, const char *height,
        const char *ceiling_diffuse, const char *ceiling_height);
int  vs_map_open_rgba(VsMap *map,
        int size,                     /* power-of-two side */
        const uint32_t *color_argb,   /* size*size */
        const uint8_t *altitude);     /* size*size, R channel already extracted */

int  vs_map_resize(VsMap *map, int width, int height); /* realloc FB + depth + column bounds */
void vs_map_set_redraw(VsMap *map, int redraw);

const VsFramebuffer *vs_map_framebuffer(const VsMap *map);
float *vs_map_depth(VsMap *map, int *w, int *h);

uint8_t vs_map_get_height(const VsMap *map, float x, float y);

/* Draw: escreve no FB interno; ignora se !redraw (comportamento atual) */
int  vs_map_draw(VsMap *map, const VsCamera *cam);

void vs_map_set_optimize(VsMap *map, int enable, float optdist);
void vs_map_set_ceiling(VsMap *map, int enabled, float ceiling_base);
```

### Formato de pixel

- CPU: `uint32_t` **ARGB8888** (mesmo `SDL_PIXELFORMAT_ARGB8888` / `0xAARRGGBB` layout usado hoje).
- Sky/fog: `GRAPHICS_SKY_COLOR` (`0x9090E0FF` em `defines.h`).
- No upload:
  - **OpenGL**: tipicamente `GL_BGRA` + `GL_UNSIGNED_BYTE` (em little-endian equivale ao ARGB packed).
  - **Vulkan**: `VK_FORMAT_B8G8R8A8_UNORM` (ou swizzle / convert se preferir `R8G8B8A8`).

Confirmar endianness no host; o padrão deste projeto assume little-endian desktop.

### Restrições de mapa (manter)

Já validadas em `LoadLayer`:

1. Diffuse e height **quadrados** (`w == h`).
2. Lado do heightmap **potência de 2**.
3. Height não pode ser maior que diffuse; se menor, scale up do height.
4. Indexação wrap: `(y & (w-1)) << shift + (x & (w-1))`.

---

## 5. Plano de refatoração (passos)

### Passo A — Isolar headers

1. Criar `include/voxelspace/` com `map.h`, `camera.h`, `constants.h`, `error.h`, `point.h`.
2. Remover include cíclico `engine.h` ↔ `map.h`.
3. Tirar SDL de `camera.h` e `map.h` (sem `SDL_atomic`, sem `Uint8` → `uint8_t`).

### Passo B — FB CPU

1. Substituir `void *screen` por `VsFramebuffer` interno.
2. `vs_map_resize(w,h)` aloca:
   - `pixels[w*h]`
   - `hiddeny[w]`, `showny[w]`
   - `depth[w*h]`
3. `PrepareToDraw`: zerar depth + fill sky **no ponteiro interno** (sem LockTexture).
4. `Map_Draw`: remover UnlockTexture; ao fim `redraw = 0`.

### Passo C — Loader plugável

Opções (escolher uma na v1):

- **C1**: `vs_map_open_rgba` only — host carrega PNG/BMP e passa buffers.
- **C2**: manter load via stb_image / SDL_image atrás de `#ifdef VS_USE_STB_IMAGE`.

Recomendação: **C1 na API pública**; loader de arquivo como helper opcional fora do núcleo.

### Passo D — Remover engine / exit

1. Remover `#include "engine.h"`.
2. Trocar `exit(1)` por `return ERROR_MALLOC_FAIL`.
3. Logging: callback `void (*vs_log)(const char *)` ou `fprintf(stderr)` sob flag.

### Passo E — Build da lib

```
libvoxelspace.a / .so
  sources: map.c (+ camera helpers se .c)
  defines: USE_AVX2 opcional
  NO SDL required for core
```

Manter este repo compilando: adaptador fino `Map_SetScreen` → wrap SDL_Lock + chama lib, **ou** migrar o engine depois.

---

## 6. Integração GPU (contrato host)

A lib só devolve `pixels` + `width/height/pitch`. O host faz staging.

### OpenGL (por frame, se `redraw` consumido)

```c
vs_map_set_redraw(map, 1);          // quando câmera/mapa mudam
vs_map_draw(map, &cam);
const VsFramebuffer *fb = vs_map_framebuffer(map);

glBindTexture(GL_TEXTURE_2D, colorTex);
glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
    fb->width, fb->height,
    GL_BGRA, GL_UNSIGNED_BYTE, fb->pixels);
// desenhar fullscreen triangle/quad
```

Criação da textura (uma vez / on resize):

```c
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
             GL_BGRA, GL_UNSIGNED_BYTE, NULL);
glTexParameteri(..., GL_TEXTURE_MIN_FILTER, GL_NEAREST);
glTexParameteri(..., GL_TEXTURE_MAG_FILTER, GL_NEAREST);
```

### Vulkan (por frame)

1. Imagem `VK_FORMAT_B8G8R8A8_UNORM`, usage `TRANSFER_DST | SAMPLED`.
2. Buffer staging host-visible (`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`).
3. `memcpy` de `fb->pixels` (cuidado se `pitch != width`: copiar linha a linha).
4. `vkCmdCopyBufferToImage` + barriers `UNDEFINED/SHADER_READ_ONLY → TRANSFER_DST → SHADER_READ_ONLY`.
5. Sample no fragment shader (fullscreen).

**Depth (opcional):** se o host precisar de oclusão de sprites/meshes, subir `fb->depth` como `R32_SFLOAT` texture, ou manter depth só na CPU para software overlays. Formato atual: `float` forward camera-space depth; céu inicia em `CAMERA_DISTANCE_MAX + 1`.

### Pitch

Internamente `pitch` é em **inteiros (pixels)**. Se `pitch == width` (caso normal deste projeto após QueryTexture), upload contigous. Se no futuro houver padding, upload row-by-row:

```c
for (int y = 0; y < h; y++)
    memcpy(dst + y * w, fb->pixels + y * fb->pitch, w * 4);
```

---

## 7. Semântica de `redraw`

Comportamento atual:

- `Map_Draw` retorna imediatamente se `!map->redraw`.
- Após draw bem-sucedido: `redraw = 0`.
- Host deve setar `redraw = 1` quando câmera, mapa, resolução ou overlays dependentes mudarem.

Na lib, documentar isso; opcionalmente `vs_map_draw_force` que ignora o flag.

---

## 8. Constantes que o draw usa (não esquecer)

De `defines.h`, necessárias no núcleo:

- `GRAPHICS_SKY_COLOR`
- `CAMERA_DISTANCE_MAX`, `CAMERA_FOG_*`, `CAMERA_LOD_*`
- `CAMERA_PROJECTION_SCALE` (também em pitch helpers)
- `RENDER_*` só se portar threads

`GRAPHICS_WIDTH/HEIGHT` são defaults do app, não da lib — o host passa o tamanho via `vs_map_resize`.

---

## 9. Checklist de aceitação

- [ ] Compila sem SDL (núcleo).
- [ ] `open` + `resize(640,360)` + `draw` produz imagem reconhecível do mapa C1W/D1 (ou equivalente).
- [ ] Upload GL: textura fullscreen correta, sem swap R/B.
- [ ] Upload VK: mesmo visual que GL.
- [ ] Resize de framebuffer realloca depth/hiddeny/showny sem leak.
- [ ] `get_height` bate com amostragem wrap do software path.
- [ ] Ceiling path (`open_dual` + `ceilingEnabled`) opcional mas preservado se possível.
- [ ] AVX2 continua opt-in e não quebra build genérico.

---

## 10. Ordem de trabalho sugerida

1. Copiar `map.c`/`map.h`/`camera.h`/`defines`/`error` + `Point` para um tree `lib/`.
2. Compilar isolado até falhar só em símbolos SDL — listar e substituir.
3. Introduzir `VsFramebuffer`; remover Lock/Unlock/QueryTexture.
4. Expor `vs_map_draw` + getter de pixels.
5. Demo mínima: GLFW+OpenGL quad (ou SDL só na demo).
6. Demo Vulkan (ou layer de upload compartilhado).
7. Só então reintegrar no VoxelSpaceSDL substituindo o path software, se desejado.

---

## 11. Referências no repo atual

| Peça | Local |
|---|---|
| Load + validação | `map.c` → `LoadLayer`, `Map_OpenDual` |
| Fill + draw | `map.c` → `PrepareToDraw`, `DrawFromTo*`, `Map_Draw` |
| Target SDL | `map.c` → `Map_SetScreen`; `engine.c` cria `SDL_TEXTUREACCESS_STREAMING` |
| Depth API | `Map_GetDepthBuffer` |
| Loop software | `engine.c` → `Map_Draw` → `SDL_RenderCopy` |
| Contraste GPU-native | `opengl_renderer.c` → `SyncTextures` (não usar como modelo desta lib) |

---

## 12. Decisão de produto (registrar na implementação)

| Pergunta | Recomendação |
|---|---|
| Lib opaca vs struct pública? | Semi-opaca: FB público read-only; resto interno |
| Loader de arquivo no núcleo? | Não; `open_rgba` + helper opcional |
| Threads na v1? | Não |
| Depth no upload GPU? | Opcional; cor primeiro |
| Nome do pacote | `voxelspace` / `libvoxelspace` |

Quando essas decisões estiverem fechadas, a implementação é mecânica: desacoplar o framebuffer e publicar o ponteiro de pixels para o host fazer `glTexSubImage2D` / `vkCmdCopyBufferToImage`.
