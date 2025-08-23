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
static GLuint g_stream_tex[2] = {0, 0};
static int g_stream_cur = 0;
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

  int dw=0, dh=0;
  SDL_GL_GetDrawableSize(g_window, &dw, &dh);
  if (dw <= 0 || dh <= 0) { dw = 512; dh = 512; }
  g_tex_max_w = g_has_npot_ext ? dw : next_pot(dw);
  g_tex_max_h = g_has_npot_ext ? dh : next_pot(dh);

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

  glGenTextures(2, g_stream_tex);
  for (int i=0;i<2;i++) {
    glBindTexture(GL_TEXTURE_2D, g_stream_tex[i]);
    const GLint init_filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, init_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, init_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_tex_max_w, g_tex_max_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  }
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
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
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
  if (g_stream_tex[0] || g_stream_tex[1]) {
    glDeleteTextures(2, g_stream_tex);
    g_stream_tex[0] = g_stream_tex[1] = 0;
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

  GLuint upload_tex = g_stream_tex[g_stream_cur ^ 1];
  glBindTexture(GL_TEXTURE_2D, upload_tex);

  const GLsizei w = (GLsizei)g_draw_width;
  const GLsizei h = (GLsizei)g_draw_height;
  const size_t px = (size_t)w * (size_t)h;
  const void *pixels;
  GLenum src_fmt;
  if (g_has_bgra_ext) {
    pixels = g_screen_buffer;
    src_fmt = GL_BGRA_EXT;
  } else {
    ensure_upload_buffer(px * 4);
    swizzle_bgra_to_rgba(g_upload_buffer, g_screen_buffer, px);
    pixels = g_upload_buffer;
    src_fmt = GL_RGBA;
  }

  const GLint filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
  if (filter != g_last_filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    g_last_filter = filter;
  }

  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, src_fmt, GL_UNSIGNED_BYTE, pixels);

  if (g_last_w != w || g_last_h != h) {
    const GLfloat umax = (GLfloat)w / (GLfloat)g_tex_max_w;
    const GLfloat vmax = (GLfloat)h / (GLfloat)g_tex_max_h;
    g_texcoords[0] = 0.f;  g_texcoords[1] = 0.f;
    g_texcoords[2] = 0.f;  g_texcoords[3] = vmax;
    g_texcoords[4] = umax; g_texcoords[5] = 0.f;
    g_texcoords[6] = umax; g_texcoords[7] = vmax;
    g_last_w = w; g_last_h = h;
  }

  g_stream_cur ^= 1;
  glBindTexture(GL_TEXTURE_2D, upload_tex);

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