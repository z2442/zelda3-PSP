#ifdef __PSP__

#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "config.h"
#include "types.h"
#include "util.h"

enum {
  kPspScreenWidth = 480,
  kPspScreenHeight = 272,
  kPspFrameStride = 512,
  kPspFrameBytes = kPspFrameStride * kPspScreenHeight * 2,
  kPspTextureOffset = kPspFrameBytes * 2,
  kPspTextureCapacity = 512 * 256 * 4,
};

typedef struct PspSpriteVertex {
  float u, v;
  int16 x, y, z;
} PspSpriteVertex;

static unsigned int g_gu_list[16 * 1024] __attribute__((aligned(16)));
static uint8 *g_pixels;
static int g_width, g_height, g_texture_width, g_texture_height, g_texture_stride;

extern void Die(const char *error);

static int NextPowerOfTwo(int value) {
  int result = 1;
  while (result < value)
    result <<= 1;
  return result;
}

static bool PspRenderer_Init(SDL_Window *window) {
  (void)window;

  // Two RGB565 display buffers consume only 544 KiB of the PSP's 2 MiB VRAM.
  // Put the dynamic texture directly after them. Sampling a texture from main
  // RAM heavily stalls the GE on real hardware.
  void *draw_buffer = (void *)0;
  void *display_buffer = (void *)kPspFrameBytes;
  g_pixels = (uint8 *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000U) + kPspTextureOffset);

  sceGuInit();
  sceGuStart(GU_DIRECT, g_gu_list);
  sceGuDrawBuffer(GU_PSM_5650, draw_buffer, kPspFrameStride);
  sceGuDispBuffer(kPspScreenWidth, kPspScreenHeight, display_buffer, kPspFrameStride);
  sceGuOffset(2048 - kPspScreenWidth / 2, 2048 - kPspScreenHeight / 2);
  sceGuViewport(2048, 2048, kPspScreenWidth, kPspScreenHeight);
  sceGuScissor(0, 0, kPspScreenWidth, kPspScreenHeight);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_CULL_FACE);
  sceGuDisable(GU_DITHER);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);
  return true;
}

static void PspRenderer_Destroy(void) {
  sceGuDisplay(GU_FALSE);
  sceGuTerm();
  g_pixels = NULL;
}

static void PspRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  const int texture_width = NextPowerOfTwo(width);
  const int texture_height = NextPowerOfTwo(height);
  const int texture_stride = (width + 15) & ~15;
  const size_t needed = (size_t)texture_stride * texture_height * 4;

  // The PSP GE supports textures up to 512x512. Enhanced 4x Mode 7 is
  // disabled by main.c on PSP so the normal framebuffer always fits.
  if (texture_width > 512 || texture_height > 512)
    Die("PSP framebuffer exceeds the GE texture limit");

  if (needed > kPspTextureCapacity)
    Die("PSP framebuffer exceeds reserved VRAM");

  g_width = width;
  g_height = height;
  g_texture_width = texture_width;
  g_texture_height = texture_height;
  g_texture_stride = texture_stride;
  *pixels = g_pixels;
  *pitch = texture_stride * 4;
}

static void PspRenderer_EndDraw(void) {
  if (g_pixels == NULL)
    return;

  int x0 = 0, y0 = 0, x1 = kPspScreenWidth, y1 = kPspScreenHeight;
  if (!g_config.ignore_aspect_ratio) {
    if (kPspScreenWidth * g_height < kPspScreenHeight * g_width) {
      const int output_height = kPspScreenWidth * g_height / g_width;
      y0 = (kPspScreenHeight - output_height) / 2;
      y1 = y0 + output_height;
    } else {
      const int output_width = kPspScreenHeight * g_width / g_height;
      x0 = (kPspScreenWidth - output_width) / 2;
      x1 = x0 + output_width;
    }
  }

  sceGuStart(GU_DIRECT, g_gu_list);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
  sceGuTexImage(0, g_texture_width, g_texture_height, g_texture_stride, g_pixels);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(g_config.linear_filtering ? GU_LINEAR : GU_NEAREST,
                 g_config.linear_filtering ? GU_LINEAR : GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);

  PspSpriteVertex *vertices = sceGuGetMemory(2 * sizeof(*vertices));
  vertices[0].u = 0.0f;
  vertices[0].v = 0.0f;
  vertices[0].x = x0;
  vertices[0].y = y0;
  vertices[0].z = 0;
  vertices[1].u = (float)g_width;
  vertices[1].v = (float)g_height;
  vertices[1].x = x1;
  vertices[1].y = y1;
  vertices[1].z = 0;
  sceGuDrawArray(GU_SPRITES,
                 GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                 2, NULL, vertices);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  // Waiting here unconditionally quantizes a slightly late frame from ~55 Hz
  // all the way down to 30 Hz. Match DisableFrameDelay and allow immediate
  // swaps when benchmarking or when the frame misses the current vblank.
  if (!g_config.disable_frame_delay)
    sceDisplayWaitVblankStart();
  sceGuSwapBuffers();
}

static const struct RendererFuncs kPspRendererFuncs = {
  &PspRenderer_Init,
  &PspRenderer_Destroy,
  &PspRenderer_BeginDraw,
  &PspRenderer_EndDraw,
};

void PspRenderer_Create(struct RendererFuncs *funcs) {
  *funcs = kPspRendererFuncs;
}

#endif
