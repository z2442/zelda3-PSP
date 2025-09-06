#ifdef __PSP__

#include <SDL2/SDL.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "types.h"
#include "util.h"
#include "config.h"

// Aligns x up to the next multiple of a
#define ALIGN_UP(x,a) (((x)+((a)-1)) & ~((a)-1))

// Returns the next power of two >= x
static inline int next_pot(int x) {
  int p = 1;
  while (p < x) p <<= 1;
  return p;
}

static SDL_Window *g_window;
static uint8 *g_screen_buffer;
static size_t g_screen_buffer_size;
static uint8 *g_upload_buffer; // unused after fix but retained for parity
static size_t g_upload_buffer_size;
static int g_draw_width, g_draw_height;
static int g_tex_max_w = 0, g_tex_max_h = 0;
static int g_last_w = -1, g_last_h = -1;
static int g_last_filter = -1; // GU_NEAREST or GU_LINEAR

// Double-buffered display lists to avoid CPU/GE contention
static unsigned int g_gu_list[2][262144] __attribute__((aligned(16)));
static int g_list_idx = 0;

// Texture backing buffer (row-padded to tex_w)
#define PSP_USE_SWIZZLE 0
static void *g_texbuf[2] = { NULL, NULL }; // double-buffered linear RGB565 textures (VRAM when possible)
static size_t g_texbuf_size = 0;
static int g_texbuf_idx = 0;
static void *g_swizzled = NULL;        // swizzled copy for faster sampling (optional)
static size_t g_swizzled_size = 0;
static int g_tex_w = 0, g_tex_h = 0;   // POT, also aligned to 16x8 tiles
static int g_frame_counter = 0;
// Control GPU sync frequency; 0 = never explicitly sync (prefer), N = sync every N frames
#define PSP_SYNC_EVERY_N 0

// BGRA8888 -> RGB565 pack (little-endian)
// PSP GU uses ABGR/BGR channel order. For 16-bit 565 textures (GU_PSM_5650),
// bits 15..11 expect Blue, 10..5 Green, 4..0 Red. Pack BGRA8888 -> BGR565.
static void pack_bgra8888_to_rgb565_row(uint16_t *dst, const uint8 *src, int w) {
  for (int x = 0; x < w; ++x) {
    const uint8 b = src[x * 4 + 0];
    const uint8 g = src[x * 4 + 1];
    const uint8 r = src[x * 4 + 2];
    const uint16_t rv = (uint16_t)((r >> 3) & 0x1F);
    const uint16_t gv = (uint16_t)((g >> 2) & 0x3F);
    const uint16_t bv = (uint16_t)((b >> 3) & 0x1F);
    // BGR565: B in bits 15..11, G in 10..5, R in 4..0
    dst[x] = (uint16_t)((bv << 11) | (gv << 5) | (rv));
  }
}

// Minimal VRAM allocator for textures. We place the texture after color+display+depth buffers.
// VRAM size is 2MB. Layout used above: draw(0) ~0x88000, disp 0x88000, depth 0x110000.
// Start allocation after ~0x154000 to be safe for 512x272 aligned buffers.
static unsigned int g_vram_alloc_off = 0x154000; // bytes from VRAM base
static void* vram_alloc_bytes(size_t size) {
  size = ALIGN_UP(size, 16);
  const unsigned int VRAM_SIZE = 0x200000; // 2 MB
  if (g_vram_alloc_off + size > VRAM_SIZE)
    return NULL;
  void *ptr = (void*)(0x04000000u + g_vram_alloc_off); // CPU-mapped VRAM
  g_vram_alloc_off += (unsigned int)size;
  return ptr;
}

static void ensure_texbuf(int tw, int th) {
  const size_t need = (size_t)tw * (size_t)th * 2; // RGB565
  if (need > g_texbuf_size) {
    g_texbuf_size = ALIGN_UP(need, 16);
    // Allocate texture storage in VRAM for faster sampling; fallback to system RAM if full
    for (int i = 0; i < 2; ++i) {
      g_texbuf[i] = vram_alloc_bytes(g_texbuf_size);
      if (!g_texbuf[i]) {
        g_texbuf[i] = memalign(16, g_texbuf_size);
      }
    }
  }
  if (PSP_USE_SWIZZLE) {
    if (need > g_swizzled_size) {
      if (g_swizzled) free(g_swizzled);
      g_swizzled_size = ALIGN_UP(need, 16);
      g_swizzled = memalign(16, g_swizzled_size);
    }
  }
  g_tex_w = tw; g_tex_h = th;
}

// Swizzle 16-bit texture (width multiple of 16, height multiple of 8)
static void swizzle_16bit(uint16_t *dst, const uint16_t *src, unsigned int width, unsigned int height) {
  unsigned int rowblocks = width / 16;
  unsigned int blockrowstride = width * 8; // 8 rows per block, 16 pixels per row
  for (unsigned int y = 0; y < height; y += 8) {
    const uint16_t *ysrc = src + y * width;
    for (unsigned int xb = 0; xb < rowblocks; ++xb) {
      const uint16_t *block = ysrc + xb * 16;
      for (unsigned int j = 0; j < 8; ++j) {
        memcpy(dst, block + j * width, 16 * sizeof(uint16_t));
        dst += 16;
      }
    }
  }
}

static inline void ensure_upload_buffer(size_t bytes) {
  if (bytes > g_upload_buffer_size) {
    g_upload_buffer_size = ALIGN_UP(bytes, 4096);
    g_upload_buffer = (uint8*)realloc(g_upload_buffer, g_upload_buffer_size);
  }
}

static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;

  // Initialize GU once
  sceGuInit();
  sceGuStart(GU_DIRECT, g_gu_list[g_list_idx]);
  // Set up buffers in VRAM (standard layout from samples)
  sceGuDrawBuffer(GU_PSM_8888, (void*)0, 512);
  sceGuDispBuffer(480, 272, (void*)0x88000, 512);
  sceGuDepthBuffer((void*)0x110000, 512);
  sceGuOffset(2048 - (480/2), 2048 - (272/2));
  sceGuViewport(2048, 2048, 480, 272);
  sceGuScissor(0, 0, 480, 272);
  // Minimal state; no scissor, no depth, no dithering
  sceGuDisable(GU_SCISSOR_TEST);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_DITHER);
  sceGuFinish();
  sceGuSync(0, 0);
  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);

  g_tex_max_w = 0;
  g_tex_max_h = 0;
  g_last_filter = -1;

  return true;
}

static void OpenGLRenderer_Destroy() {
  free(g_upload_buffer); g_upload_buffer = NULL; g_upload_buffer_size = 0;
  free(g_screen_buffer); g_screen_buffer = NULL; g_screen_buffer_size = 0;
  // No explicit VRAM free; allocator is bump-only and app exits after
  g_texbuf[0] = NULL;
  g_texbuf[1] = NULL;
  g_texbuf_size = 0;
  g_texbuf_idx = 0;
  sceGuTerm();
}

static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  const size_t needed = (size_t)width * (size_t)height * 4;
  if (needed > g_screen_buffer_size) {
    g_screen_buffer_size = ALIGN_UP(needed, 4096);
    free(g_screen_buffer);
    g_screen_buffer = (uint8*)malloc(g_screen_buffer_size);
  }
  g_draw_width = width;
  g_draw_height = height;
  *pixels = g_screen_buffer;
  *pitch = width * 4;
}

typedef struct {
  float u, v;
  float x, y, z;
} GuVertex;

static void OpenGLRenderer_EndDraw() {
  // PSP display size
  const int drawable_width = 480;
  const int drawable_height = 272;

  int viewport_width = drawable_width, viewport_height = drawable_height;
  if (!g_config.ignore_aspect_ratio) {
    if (viewport_width * g_draw_height < viewport_height * g_draw_width)
      viewport_height = (viewport_width * g_draw_height) / g_draw_width;
    else
      viewport_width = (viewport_height * g_draw_width) / g_draw_height;
  }
  const int viewport_x = (drawable_width - viewport_width) >> 1;
  const int viewport_y = (drawable_height - viewport_height) >> 1;

  const int w = g_draw_width;
  const int h = g_draw_height;

  // POT texture dims; align to tile size (16x8) only if swizzling
  int tex_w = next_pot(w);
  int tex_h = next_pot(h);
  if (PSP_USE_SWIZZLE) {
    if (tex_w & 15) tex_w = (tex_w + 15) & ~15;
    if (tex_h & 7)  tex_h  = (tex_h  + 7) & ~7;
  }

  // Ensure texture buffer with row padding exists
  ensure_texbuf(tex_w, tex_h);

  // Convert and pad into texbuf with dst row stride = tex_w
  int next_tex = g_texbuf_idx ^ 1;
  uint16_t *dst = (uint16_t*)g_texbuf[next_tex];
  const uint8 *src = g_screen_buffer;
  for (int y = 0; y < h; ++y) {
    pack_bgra8888_to_rgb565_row(dst, src, w);
    // zero pad remaining texels on this row if any
    for (int x = w; x < tex_w; ++x) dst[x] = 0;
    dst += tex_w;
    src += w * 4;
  }
  // Clear remaining rows (if any) to avoid sampling garbage when filtering
  const int remaining_rows = tex_h - h;
  if (remaining_rows > 0) {
    memset(dst, 0, (size_t)remaining_rows * (size_t)tex_w * 2);
  }

  if (PSP_USE_SWIZZLE) {
    // Swizzle into optimal layout for PSP GE
    swizzle_16bit((uint16_t*)g_swizzled, (const uint16_t*)g_texbuf[next_tex], (unsigned int)tex_w, (unsigned int)tex_h);
    // Flush data cache so GU sees up-to-date texture memory
    sceKernelDcacheWritebackRange(g_swizzled, (size_t)tex_w * (size_t)tex_h * 2);
  } else {
    // Flush the linear texture buffer
    sceKernelDcacheWritebackRange(g_texbuf[next_tex], (size_t)tex_w * (size_t)tex_h * 2);
  }

  // Ensure texture cache sees updated texels
  sceGuTexFlush();

  // Record render commands using current list buffer
  sceGuStart(GU_DIRECT, g_gu_list[g_list_idx]);

  // Texture state
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_5650, 0, 0, PSP_USE_SWIZZLE ? 1 : 0);
  sceGuTexImage(0, tex_w, tex_h, tex_w, PSP_USE_SWIZZLE ? g_swizzled : g_texbuf[next_tex]);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  int wanted_filter = g_config.linear_filtering ? GU_LINEAR : GU_NEAREST;
  if (wanted_filter != g_last_filter) {
    sceGuTexFilter(wanted_filter, wanted_filter);
    g_last_filter = wanted_filter;
  }
  // Use pixel u,v with scale of 1/tex_size
  sceGuTexScale(1.0f / (float)tex_w, 1.0f / (float)tex_h);
  sceGuTexOffset(0.0f, 0.0f);

  // Build a textured quad
  GuVertex verts[4];
  // Triangle strip: (x0,y0)-(x1,y0)-(x0,y1)-(x1,y1)
  const float x0 = (float)viewport_x;
  const float y0 = (float)viewport_y;
  const float x1 = (float)(viewport_x + viewport_width);
  const float y1 = (float)(viewport_y + viewport_height);
  const float u1 = (float)w; // with TexScale = 1/tex_w
  const float v1 = (float)h;

  verts[0].u = 0.0f; verts[0].v = 0.0f; verts[0].x = x0; verts[0].y = y0; verts[0].z = 0.0f;
  verts[1].u = u1;   verts[1].v = 0.0f; verts[1].x = x1; verts[1].y = y0; verts[1].z = 0.0f;
  verts[2].u = 0.0f; verts[2].v = v1;   verts[2].x = x0; verts[2].y = y1; verts[2].z = 0.0f;
  verts[3].u = u1;   verts[3].v = v1;   verts[3].x = x1; verts[3].y = y1; verts[3].z = 0.0f;

  sceGuDisable(GU_DEPTH_TEST);
  sceGuColor(0xFFFFFFFF);
  sceGuDrawArray(GU_TRIANGLE_STRIP,
                 GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                 4, 0, verts);

  sceGuFinish();
#if PSP_SYNC_EVERY_N > 0
  if ((g_frame_counter++ % PSP_SYNC_EVERY_N) == 0) {
    sceGuSync(0, 0);
  }
#endif
  sceDisplayWaitVblankStartCB();
  sceGuSwapBuffers();

  // Swap indices
  g_texbuf_idx = next_tex;
  g_list_idx ^= 1;
}

static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
};

void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es) {
  (void)use_opengl_es;
  *funcs = kOpenGLRendererFuncs;
}

#else  // non-PSP (OpenGL ES path)

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "types.h"
#include "util.h"
#include "config.h"

// Platform OpenGL ES includes, without glext.h dependency
#include <GLES/gl.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Aligns x up to the next multiple of a
#define ALIGN_UP(x,a) (((x)+((a)-1)) & ~((a)-1))

// Returns the next power of two >= x
static inline int next_pot(int x) {
  int p = 1;
  while (p < x) p <<= 1;
  return p;
}

typedef struct {
  GLuint gl_texture;
  int width;
  int height;
} GlTextureWithSize;

static SDL_Window *g_window;
static uint8 *g_screen_buffer;
static size_t g_screen_buffer_size;
static uint8 *g_upload_buffer;
static size_t g_upload_buffer_size;
static int g_draw_width, g_draw_height;
static GlTextureWithSize g_texture;
static GLuint g_tex = 0;
static int g_tex_max_w = 0, g_tex_max_h = 0;
static bool g_has_bgra_ext = false;
static bool g_has_npot_ext = false;
static GLint g_last_filter = -1;
static GLfloat g_texcoords[8] = {
  0.f, 0.f,  0.f, 1.f,
  1.f, 0.f,  1.f, 1.f
};
static bool g_opengl_es;
static int g_last_w = -1, g_last_h = -1;
static bool g_use_rgb565 = false; // prefer 16-bit on GLES devices (PSP)

// --- extension check helper
static bool has_extension(const char *exts, const char *needle) {
  if (!exts || !needle) return false;
  const char *p = exts;
  size_t nlen = strlen(needle);
  while ((p = strstr(p, needle))) {
    const char c = p[nlen];
    if ((p == exts || p[-1] == ' ') && (c == '\0' || c == ' '))
      return true;
    p += nlen;
  }
  return false;
}

static void detect_extensions(void) {
  const char *exts = (const char*)glGetString(GL_EXTENSIONS);
  g_has_bgra_ext = has_extension(exts, "GL_EXT_texture_format_BGRA8888");
  g_has_npot_ext = has_extension(exts, "GL_OES_texture_npot") ||
                   has_extension(exts, "GL_ARB_texture_non_power_of_two") ||
                   has_extension(exts, "GL_IMG_texture_npot");
}

static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;
  SDL_GLContext context = SDL_GL_CreateContext(window);
  (void)context;

  SDL_GL_SetSwapInterval(1); // set to 0 for raw throughput

  detect_extensions();
  // Prefer RGB565 on GLES to reduce bandwidth/VRAM (important on PSP)
  g_use_rgb565 = g_opengl_es;

  // Defer allocating the texture until we know the draw size.
  g_tex_max_w = 0;
  g_tex_max_h = 0;

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

  glGenTextures(1, &g_tex);
  glBindTexture(GL_TEXTURE_2D, g_tex);
  const GLint init_filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, init_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, init_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  g_last_filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);

  glDisable(GL_BLEND);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FOG);
  glDisable(GL_LIGHTING);

  static const GLfloat kPositions[12] = {
    -1.f,  1.f, 0.f,
    -1.f, -1.f, 0.f,
     1.f,  1.f, 0.f,
     1.f, -1.f, 0.f
  };
  glVertexPointer(3, GL_FLOAT, 0, kPositions);
  glTexCoordPointer(2, GL_FLOAT, 0, g_texcoords);

  glEnable(GL_TEXTURE_2D);
  glPixelStorei(GL_UNPACK_ALIGNMENT, g_use_rgb565 ? 2 : 4);
  glDisable(GL_DITHER);

  g_texture.gl_texture = 0;
  g_texture.width = 0;
  g_texture.height = 0;

  return true;
}

static void OpenGLRenderer_Destroy() {
  if (g_texture.gl_texture) {
    glDeleteTextures(1, &g_texture.gl_texture);
    g_texture.gl_texture = 0;
  }
  if (g_tex) {
    glDeleteTextures(1, &g_tex);
    g_tex = 0;
  }
  free(g_upload_buffer); g_upload_buffer = NULL; g_upload_buffer_size = 0;
  free(g_screen_buffer); g_screen_buffer = NULL; g_screen_buffer_size = 0;
}

static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  const size_t needed = (size_t)width * (size_t)height * 4;
  if (needed > g_screen_buffer_size) {
    g_screen_buffer_size = ALIGN_UP(needed, 4096);
    free(g_screen_buffer);
    g_screen_buffer = (uint8*)malloc(g_screen_buffer_size);
  }
  g_draw_width = width;
  g_draw_height = height;
  *pixels = g_screen_buffer;
  *pitch = width * 4;
}

static inline void ensure_upload_buffer(size_t bytes) {
  if (bytes > g_upload_buffer_size) {
    g_upload_buffer_size = ALIGN_UP(bytes, 4096);
    g_upload_buffer = (uint8*)realloc(g_upload_buffer, g_upload_buffer_size);
  }
}

// Fast BGRA -> RGBA swizzle
static void swizzle_bgra_to_rgba(uint8 *dst, const uint8 *src, size_t px_count) {
  const uint32_t *s32 = (const uint32_t*)src;
  uint32_t *d32 = (uint32_t*)dst;
  for (size_t i = 0; i < px_count; ++i) {
    uint32_t v = s32[i];
    d32[i] = (v >> 16) | (v & 0xFF00FF00) | (v << 16);
  }
}

// Pack BGRA8888 -> RGB565 (little-endian)
static void pack_bgra8888_to_rgb565(uint16_t *dst, const uint8 *src, size_t px_count) {
  for (size_t i = 0; i < px_count; ++i) {
    uint8 b = src[i * 4 + 0];
    uint8 g = src[i * 4 + 1];
    uint8 r = src[i * 4 + 2];
    uint16_t rv = (uint16_t)((r >> 3) & 0x1F);
    uint16_t gv = (uint16_t)((g >> 2) & 0x3F);
    uint16_t bv = (uint16_t)((b >> 3) & 0x1F);
    dst[i] = (uint16_t)((rv << 11) | (gv << 5) | (bv));
  }
}

static void OpenGLRenderer_EndDraw() {
  int drawable_width = 0, drawable_height = 0;
  SDL_GL_GetDrawableSize(g_window, &drawable_width, &drawable_height);

  int viewport_width = drawable_width, viewport_height = drawable_height;
  if (!g_config.ignore_aspect_ratio) {
    if (viewport_width * g_draw_height < viewport_height * g_draw_width)
      viewport_height = (viewport_width * g_draw_height) / g_draw_width;
    else
      viewport_width = (viewport_height * g_draw_width) / g_draw_height;
  }
  const int viewport_x = (drawable_width - viewport_width) >> 1;
  const int viewport_y = (drawable_height - viewport_height) >> 1;

  glViewport(viewport_x, viewport_y, viewport_width, viewport_height);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glBindTexture(GL_TEXTURE_2D, g_tex);

  const GLsizei w = (GLsizei)g_draw_width;
  const GLsizei h = (GLsizei)g_draw_height;
  const size_t px = (size_t)w * (size_t)h;
  const void *pixels;
  GLenum src_fmt;
  GLenum src_type;
  if (g_use_rgb565) {
    ensure_upload_buffer(px * 2);
    pack_bgra8888_to_rgb565((uint16_t*)g_upload_buffer, g_screen_buffer, px);
    pixels = g_upload_buffer;
    src_fmt = GL_RGB;
    src_type = GL_UNSIGNED_SHORT_5_6_5;
  } else if (g_has_bgra_ext) {
    pixels = g_screen_buffer;
    src_fmt = GL_BGRA_EXT;
    src_type = GL_UNSIGNED_BYTE;
  } else {
    ensure_upload_buffer(px * 4);
    swizzle_bgra_to_rgba(g_upload_buffer, g_screen_buffer, px);
    pixels = g_upload_buffer;
    src_fmt = GL_RGBA;
    src_type = GL_UNSIGNED_BYTE;
  }

  const GLint filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
  if (filter != g_last_filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    g_last_filter = filter;
  }

  // (Re)allocate texture storage if needed (first frame or size change)
  int desired_w = g_has_npot_ext ? w : next_pot(w);
  int desired_h = g_has_npot_ext ? h : next_pot(h);
  if (desired_w != g_tex_max_w || desired_h != g_tex_max_h) {
    GLenum internal_format = g_use_rgb565 ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
                 desired_w, desired_h, 0, internal_format,
                 g_use_rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE,
                 NULL);
    g_tex_max_w = desired_w;
    g_tex_max_h = desired_h;
    // Force texcoord update
    g_last_w = -1; g_last_h = -1;
  }

  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, src_fmt, src_type, pixels);

  if (g_last_w != w || g_last_h != h) {
    const GLfloat umax = (GLfloat)w / (GLfloat)g_tex_max_w;
    const GLfloat vmax = (GLfloat)h / (GLfloat)g_tex_max_h;
    g_texcoords[0] = 0.f;  g_texcoords[1] = 0.f;
    g_texcoords[2] = 0.f;  g_texcoords[3] = vmax;
    g_texcoords[4] = umax; g_texcoords[5] = 0.f;
    g_texcoords[6] = umax; g_texcoords[7] = vmax;
    g_last_w = w; g_last_h = h;
  }

  glBindTexture(GL_TEXTURE_2D, g_tex);

  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  SDL_GL_SwapWindow(g_window);
}

static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
};

void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es) {
  g_opengl_es = use_opengl_es;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  *funcs = kOpenGLRendererFuncs;
}

#endif // __PSP__
