#ifdef __PSP__

#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "config.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "types.h"
#include "util.h"
#include "variables.h"
#include "zelda_rtl.h"

enum {
  kPspScreenWidth = 480,
  kPspScreenHeight = 272,
  kPspFrameStride = 512,
  kPspFrameBytes = kPspFrameStride * kPspScreenHeight * 2,
  kPspTextureOffset = kPspFrameBytes * 2,
  kPspTextureCapacity = 512 * 256 * 2,
  kPspAtlas4Offset = kPspTextureOffset,
  kPspAtlas4Bytes = 256 * 512 / 2,
  kPspAtlas2Offset = kPspAtlas4Offset + kPspAtlas4Bytes,
  kPspAtlas2Bytes = 512 * 512 / 2,
  kPspDepthOffset = kPspAtlas2Offset + kPspAtlas2Bytes,
  kPspDepthBytes = kPspFrameStride * kPspScreenHeight * 2,
  kPspSubDepthOffset = kPspDepthOffset + kPspDepthBytes,
};

typedef struct PspSpriteVertex {
  float u, v;
  int16 x, y, z;
} PspSpriteVertex;

// Native main/subscreen composition can retain several copies of the tile
// geometry in one list. Leave enough headroom for windowed castle rooms and
// the dynamic spotlight without corrupting adjacent PSP memory.
static unsigned int g_gu_list[128 * 1024] __attribute__((aligned(16)));
static uint8 *g_pixels;
static uint8 *g_atlas4, *g_atlas2;
static int g_width, g_height, g_texture_width, g_texture_height, g_texture_stride;
static uint32 g_hash4[2048], g_hash2[4096];
static uint32 g_clut[256] __attribute__((aligned(64)));
static uint32 g_clut2[8] __attribute__((aligned(64)));

typedef struct PspTileVertex {
  float u, v;
  int16 x, y, z;
} PspTileVertex;

typedef struct PspColorVertex {
  uint32 color;
  int16 x, y, z;
} PspColorVertex;

static int g_out_x0, g_out_y0, g_out_x1, g_out_y1;
static int g_logical_width, g_logical_height, g_logical_left;
static bool g_dynamic_window;

extern void Die(const char *error);
static uint32 PspColor(Ppu *ppu, uint16 color, bool transparent);

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
  g_atlas4 = (uint8 *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000U) + kPspAtlas4Offset);
  g_atlas2 = (uint8 *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000U) + kPspAtlas2Offset);
  memset(g_hash4, 0xff, sizeof(g_hash4));
  memset(g_hash2, 0xff, sizeof(g_hash2));

  sceGuInit();
  sceGuStart(GU_DIRECT, g_gu_list);
  sceGuDrawBuffer(GU_PSM_5650, draw_buffer, kPspFrameStride);
  sceGuDispBuffer(kPspScreenWidth, kPspScreenHeight, display_buffer, kPspFrameStride);
  sceGuDepthBuffer((void *)kPspDepthOffset, kPspFrameStride);
  sceGuDepthRange(65535, 0);
  sceGuDepthFunc(GU_GEQUAL);
  sceGuClearDepth(0);
  sceGuOffset(2048 - kPspScreenWidth / 2, 2048 - kPspScreenHeight / 2);
  sceGuViewport(2048, 2048, kPspScreenWidth, kPspScreenHeight);
  sceGuScissor(0, 0, kPspScreenWidth, kPspScreenHeight);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuEnable(GU_DEPTH_TEST);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_CULL_FACE);
  sceGuDisable(GU_DITHER);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
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
  g_atlas4 = g_atlas2 = NULL;
}

static uint32 HashWords(const uint16 *src, int count) {
  uint32 h = 2166136261u;
  for (int i = 0; i < count; i++)
    h = (h ^ src[i]) * 16777619u;
  return h;
}

static void Ensure4bppTile(Ppu *ppu, int physical_tile) {
  physical_tile &= 2047;
  const uint16 *src = &ppu->vram[physical_tile * 16];
  uint32 hash = HashWords(src, 16);
  if (g_hash4[physical_tile] == hash)
    return;
  g_hash4[physical_tile] = hash;
  int tile_x = physical_tile & 31;
  int tile_y = physical_tile >> 5;
  uint8 *dst = g_atlas4 + tile_y * 8 * 128 + tile_x * 4;
  for (int y = 0; y < 8; y++, dst += 128) {
    uint32 planes = src[y] | src[y + 8] << 16;
    for (int x = 0; x < 8; x++) {
      int shift = 7 - x;
      int pixel = (planes >> shift & 1) | (planes >> (shift + 7) & 2) |
                  (planes >> (shift + 14) & 4) | (planes >> (shift + 21) & 8);
      if (x & 1)
        dst[x >> 1] = (dst[x >> 1] & 0x0f) | pixel << 4;
      else
        dst[x >> 1] = (dst[x >> 1] & 0xf0) | pixel;
    }
  }
}

static void Ensure2bppTile(Ppu *ppu, int physical_tile) {
  physical_tile &= 4095;
  const uint16 *src = &ppu->vram[physical_tile * 8];
  uint32 hash = HashWords(src, 8);
  if (g_hash2[physical_tile] == hash)
    return;
  g_hash2[physical_tile] = hash;
  int tile_x = physical_tile & 63;
  int tile_y = physical_tile >> 6;
  uint8 *dst = g_atlas2 + tile_y * 8 * 256 + tile_x * 4;
  for (int y = 0; y < 8; y++, dst += 256) {
    uint16 planes = src[y];
    for (int x = 0; x < 8; x++) {
      int shift = 7 - x;
      int pixel = (planes >> shift & 1) | (planes >> (shift + 7) & 2);
      if (x & 1)
        dst[x >> 1] = (dst[x >> 1] & 0x0f) | pixel << 4;
      else
        dst[x >> 1] = (dst[x >> 1] & 0xf0) | pixel;
    }
  }
}

static int MapX(int x) {
  return g_out_x0 + (x - g_logical_left) * (g_out_x1 - g_out_x0) / g_logical_width;
}

static int MapY(int y) {
  return g_out_y0 + y * (g_out_y1 - g_out_y0) / g_logical_height;
}

static int ClampToScreen(int value, int limit) {
  return value < 0 ? 0 : value > limit ? limit : value;
}

static void SetActiveScissor(Ppu *ppu) {
  // The software renderer leaves the unused portion of the configured wide
  // canvas black when a room or overworld area cannot expose that much space.
  int left = -ppu->extraLeftCur;
  int right = 256 + ppu->extraRightCur;
  int bottom = g_logical_height;
  if (bottom > 224)
    bottom = IntMin(bottom, 224 + ppu->extraBottomCur);
  sceGuScissor(ClampToScreen(MapX(left), kPspScreenWidth),
               ClampToScreen(MapY(0), kPspScreenHeight),
               ClampToScreen(MapX(right), kPspScreenWidth),
               ClampToScreen(MapY(bottom), kPspScreenHeight));
}

static int BgDepth(int layer, bool high) {
  // Mode 1 priority levels used by the software PPU. Zelda always enables
  // BG3 priority in mode 1 (BGMODE=$09), putting high BG3 above OBJ priority 3.
  static const uint8 depth[3][2] = {{8, 12}, {7, 11}, {1, 15}};
  return depth[layer][high] * 256;
}

static bool WindowState(Ppu *ppu, int layer, int x) {
  enum { kW1Inv = 1, kW1Enable = 2, kW2Inv = 4, kW2Enable = 8 };
  uint32 flags = ppu->windowsel >> (layer * 4);
  // An inverted window whose left edge is beyond its right edge is disabled
  // by the software PPU; treating it as an inverted empty span makes it cover
  // the entire screen and incorrectly removes castle layers.
  bool w1 = (flags & kW1Enable) && ppu->window1left <= ppu->window1right;
  bool w2 = (flags & kW2Enable) && ppu->window2left <= ppu->window2right;
  if (!w1 && !w2)
    return false;
  bool test1 = x >= ppu->window1left && x <= ppu->window1right;
  bool test2 = x >= ppu->window2left && x <= ppu->window2right;
  if (flags & kW1Inv) test1 = !test1;
  if (flags & kW2Inv) test2 = !test2;
  return w1 && w2 ? test1 || test2 : w1 ? test1 : test2;
}

static bool MathAllowed(Ppu *ppu, int x) {
  bool in_window = WindowState(ppu, 5, x);
  return ppu->preventMathMode == 0 ||
         (ppu->preventMathMode == 1 && in_window) ||
         (ppu->preventMathMode == 2 && !in_window);
}

static bool LayerVisibleAt(Ppu *ppu, int layer, bool sub, bool math_clip, int x) {
  // The scanline-varying HDMA window is applied once as a batched GE mask.
  // Applying its final register value as a static scissor would hide most of
  // the room.
  if (g_dynamic_window)
    return true;
  if ((ppu->screenWindowed[sub] & (1 << layer)) && WindowState(ppu, layer, x))
    return false;
  return !math_clip || MathAllowed(ppu, x);
}

static void DrawLayerGeometry(Ppu *ppu, int layer, bool sub, bool math_clip,
                              int count, const PspTileVertex *verts) {
  int left = -(layer != 2 ? ppu->extraLeftCur : 0);
  int right = 256 + (layer != 2 ? ppu->extraRightCur : 0);
  for (int x = left; x < right;) {
    bool visible = LayerVisibleAt(ppu, layer, sub, math_clip, x);
    int end = x + 1;
    while (end < right && LayerVisibleAt(ppu, layer, sub, math_clip, end) == visible)
      end++;
    if (visible) {
      sceGuScissor(ClampToScreen(MapX(x), kPspScreenWidth),
                   ClampToScreen(MapY(0), kPspScreenHeight),
                   ClampToScreen(MapX(end), kPspScreenWidth),
                   ClampToScreen(MapY(g_logical_height > 224 ?
                                      IntMin(g_logical_height, 224 + ppu->extraBottomCur) :
                                      g_logical_height), kPspScreenHeight));
      sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                     count, NULL, verts);
    }
    x = end;
  }
  SetActiveScissor(ppu);
}

static bool HasDynamicWindow(void) {
  if (g_zenv.dma == NULL)
    return false;
  for (int channel = 6; channel <= 7; channel++) {
    if ((HDMAEN_copy & (1 << channel)) && g_zenv.dma->channel[channel].bAdr == 0x26)
      return true;
  }
  return false;
}

static void DrawDynamicWindowDarkness(Ppu *ppu) {
  if (!g_dynamic_window || !ppu->subtractColor)
    return;
  int lines = IntMin(g_logical_height, 240);
  PspColorVertex *verts = sceGuGetMemory(lines * 4 * sizeof(*verts));
  PspColorVertex *v = verts;
  uint16 fixed = ppu->fixedColorR | ppu->fixedColorG << 5 | ppu->fixedColorB << 10;
  uint32 color = PspColor(ppu, fixed, false);
  int canvas_left = -ppu->extraLeftCur;
  int canvas_right = 256 + ppu->extraRightCur;
  for (int y = 0; y < lines; y++) {
    uint16 span = hdma_table_dynamic[y];
    int light_left = span & 0xff;
    int light_right = span >> 8;
    if (light_left > light_right) {
      v[0] = (PspColorVertex){color, MapX(canvas_left), MapY(y), 0};
      v[1] = (PspColorVertex){color, MapX(canvas_right), MapY(y + 1), 0};
      v += 2;
      continue;
    }
    if (light_left > canvas_left) {
      v[0] = (PspColorVertex){color, MapX(canvas_left), MapY(y), 0};
      v[1] = (PspColorVertex){color, MapX(light_left), MapY(y + 1), 0};
      v += 2;
    }
    if (light_right + 1 < canvas_right) {
      v[0] = (PspColorVertex){color, MapX(light_right + 1), MapY(y), 0};
      v[1] = (PspColorVertex){color, MapX(canvas_right), MapY(y + 1), 0};
      v += 2;
    }
  }
  if (v != verts) {
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX,
                   0x00ffffff, 0x00ffffff);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   v - verts, NULL, verts);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
  }
}

static uint16 GetBgTile(Ppu *ppu, int layer, int tx, int ty) {
  BgLayer *bg = &ppu->bgLayer[layer];
  tx &= 63;
  ty &= 63;
  int addr = bg->tilemapAdr + (ty & 31) * 32 + (tx & 31);
  if ((tx & 32) && bg->tilemapWider)
    addr += 0x400;
  if ((ty & 32) && bg->tilemapHigher)
    addr += bg->tilemapWider ? 0x800 : 0x400;
  return ppu->vram[addr & 0x7fff];
}

static int CountBgTiles(Ppu *ppu, int layer, bool high, int palette) {
  BgLayer *bg = &ppu->bgLayer[layer];
  int left = layer == 2 ? 0 : g_logical_left;
  int right = layer == 2 ? 256 : g_logical_left + g_logical_width;
  int tx0 = (bg->hScroll + left) >> 3;
  int tx1 = (bg->hScroll + right + 7) >> 3;
  int ty0 = bg->vScroll >> 3;
  int ty1 = (bg->vScroll + g_logical_height + 7) >> 3;
  int count = 0;
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      uint16 tile = GetBgTile(ppu, layer, tx, ty);
      if (!!(tile & 0x2000) == high && ((tile >> 10) & 7) == palette)
        count++;
    }
  return count;
}

static void DrawBgPass(Ppu *ppu, int layer, bool high, int palette, bool sub, bool math_clip) {
  if (!(ppu->screenEnabled[sub] & (1 << layer)))
    return;
  int count = CountBgTiles(ppu, layer, high, palette);
  if (!count)
    return;

  bool is2 = layer == 2;
  if (is2) {
    for (int i = 0; i < 8; i++)
      g_clut2[i] = g_clut[palette * 4 + (i & 3)];
    g_clut2[0] &= 0x00ffffff;
    sceKernelDcacheWritebackRange(g_clut2, sizeof(g_clut2));
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(1, g_clut2);
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, 512, 512, 512, g_atlas2);
  } else {
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, palette);
    sceGuClutLoad(32, g_clut);
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, 256, 512, 256, g_atlas4);
  }

  PspTileVertex *verts = sceGuGetMemory(count * 2 * sizeof(*verts));
  PspTileVertex *v = verts;
  BgLayer *bg = &ppu->bgLayer[layer];
  int left = layer == 2 ? 0 : g_logical_left;
  int right = layer == 2 ? 256 : g_logical_left + g_logical_width;
  int tx0 = (bg->hScroll + left) >> 3;
  int tx1 = (bg->hScroll + right + 7) >> 3;
  int ty0 = bg->vScroll >> 3;
  int ty1 = (bg->vScroll + g_logical_height + 7) >> 3;
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      uint16 tile = GetBgTile(ppu, layer, tx, ty);
      if (!!(tile & 0x2000) != high || ((tile >> 10) & 7) != palette)
        continue;
      int tile_num = tile & 0x3ff;
      int physical = ((bg->tileAdr + tile_num * (is2 ? 8 : 16)) & 0x7fff) / (is2 ? 8 : 16);
      if (is2) Ensure2bppTile(ppu, physical); else Ensure4bppTile(ppu, physical);
      int atlas_cols = is2 ? 64 : 32;
      float u0 = (physical % atlas_cols) * 8;
      float v0 = (physical / atlas_cols) * 8;
      float u1 = u0 + 8, v1 = v0 + 8;
      if (tile & 0x4000) { float t = u0; u0 = u1; u1 = t; }
      if (tile & 0x8000) { float t = v0; v0 = v1; v1 = t; }
      int sx = tx * 8 - bg->hScroll;
      int sy = ty * 8 - bg->vScroll;
      int z = BgDepth(layer, high);
      v[0] = (PspTileVertex){u0, v0, MapX(sx), MapY(sy), z};
      v[1] = (PspTileVertex){u1, v1, MapX(sx + 8), MapY(sy + 8), z};
      v += 2;
    }
  }
  DrawLayerGeometry(ppu, layer, sub, math_clip, count * 2, verts);
}

static void DrawBgPriority(Ppu *ppu, int layer, bool high, bool sub, bool math_clip) {
  for (int palette = 0; palette < 8; palette++)
    DrawBgPass(ppu, layer, high, palette, sub, math_clip);
}

static int SpriteSize(Ppu *ppu, int big) {
  static const uint8 sizes[8][2] = {
    {8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}
  };
  return sizes[ppu->objSize & 7][big & 1];
}

static int CountSpriteTiles(Ppu *ppu, int priority, int palette) {
  int count = 0;
  for (int index = 0; index < 256; index += 2) {
    int y = ppu->oam[index] >> 8;
    if (y == 0xf0) continue;
    int hi = ppu->oam[0x100 + (index >> 4)] >> (index & 15);
    int attr = ppu->oam[index + 1];
    if (((attr >> 12) & 3) != priority || ((attr >> 9) & 7) != palette) continue;
    int size = SpriteSize(ppu, hi >> 1);
    count += (size / 8) * (size / 8);
  }
  return count;
}

static void DrawSpritePass(Ppu *ppu, int priority, int palette, bool sub, bool math_clip) {
  if (!(ppu->screenEnabled[sub] & 0x10)) return;
  int count = CountSpriteTiles(ppu, priority, palette);
  if (!count) return;
  sceGuClutMode(GU_PSM_8888, 0, 0x0f, 8 + palette);
  sceGuClutLoad(32, g_clut);
  sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
  sceGuTexImage(0, 256, 512, 256, g_atlas4);
  PspTileVertex *verts = sceGuGetMemory(count * 2 * sizeof(*verts)), *v = verts;
  for (int index = 254; index >= 0; index -= 2) {
    int sy0 = ppu->oam[index] >> 8;
    if (sy0 == 0xf0) continue;
    int hi = ppu->oam[0x100 + (index >> 4)] >> (index & 15);
    int attr = ppu->oam[index + 1];
    if (((attr >> 12) & 3) != priority || ((attr >> 9) & 7) != palette) continue;
    int size = SpriteSize(ppu, hi >> 1);
    int sx0 = (ppu->oam[index] & 0xff) + (hi & 1) * 256;
    // OAM's ninth X bit wraps beyond the complete widened SNES canvas, not
    // at the original 256-pixel edge. This is what allows game-side
    // widescreen OAM fixes to place sprites in the right extension.
    if (sx0 >= 256 + ppu->extraLeftRight) sx0 -= 512;
    for (int row = 0; row < size; row += 8) for (int col = 0; col < size; col += 8) {
      int source_col = attr & 0x4000 ? size - 8 - col : col;
      int source_row = attr & 0x8000 ? size - 8 - row : row;
      int tile = ((((attr & 0xff) >> 4) + (source_row >> 3)) << 4) |
                 (((attr & 0xf) + (source_col >> 3)) & 0xf);
      int base = (attr & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
      int physical = ((base + tile * 16) & 0x7fff) / 16;
      Ensure4bppTile(ppu, physical);
      float u0 = (physical & 31) * 8, v0 = (physical >> 5) * 8;
      float u1 = u0 + 8, v1 = v0 + 8;
      if (attr & 0x4000) { float t=u0;u0=u1;u1=t; }
      if (attr & 0x8000) { float t=v0;v0=v1;v1=t; }
      int z = (priority * 4 + 2) * 256 + 128 - (index >> 1);
      v[0] = (PspTileVertex){u0,v0,MapX(sx0+col),MapY(sy0+row),z};
      v[1] = (PspTileVertex){u1,v1,MapX(sx0+col+8),MapY(sy0+row+8),z};
      v += 2;
    }
  }
  DrawLayerGeometry(ppu, 4, sub, math_clip, count * 2, verts);
}

static void DrawSprites(Ppu *ppu, int priority, bool sub, bool math_clip) {
  for (int palette = 0; palette < 8; palette++)
    DrawSpritePass(ppu, priority, palette, sub, math_clip);
}

static void RestoreBackdrop(Ppu *ppu) {
  PspColorVertex *v = sceGuGetMemory(2 * sizeof(*v));
  int left = -ppu->extraLeftCur;
  int right = 256 + ppu->extraRightCur;
  uint32 color = PspColor(ppu, ppu->cgram[0], false);
  v[0] = (PspColorVertex){color, MapX(left), MapY(0), 0};
  v[1] = (PspColorVertex){color, MapX(right), MapY(g_logical_height), 0};
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                 2, NULL, v);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuEnable(GU_TEXTURE_2D);
}

static void RestoreLayersExcludedFromMath(Ppu *ppu) {
  // Reuse the untouched main-screen depth buffer and accept only fragments
  // whose exact layer won the original priority test. This keeps an excluded
  // low-priority layer from painting over a higher layer that did use math.
  sceGuDepthBuffer((void *)kPspDepthOffset, kPspFrameStride);
  sceGuDepthFunc(GU_EQUAL);
  sceGuDepthMask(GU_TRUE);

  if (!(ppu->mathEnabled & 0x20))
    RestoreBackdrop(ppu);
  for (int layer = 0; layer < 3; layer++) {
    if (!(ppu->mathEnabled & (1 << layer))) {
      DrawBgPriority(ppu, layer, false, false, false);
      DrawBgPriority(ppu, layer, true, false, false);
    }
  }

  // OBJ palettes 0-3 are tagged as layer 6 by the software PPU and never use
  // color math. Palettes 4-7 follow the normal OBJ enable bit (layer 4).
  for (int priority = 0; priority < 4; priority++) {
    for (int palette = 0; palette < 4; palette++)
      DrawSpritePass(ppu, priority, palette, false, false);
    if (!(ppu->mathEnabled & 0x10)) {
      for (int palette = 4; palette < 8; palette++)
        DrawSpritePass(ppu, priority, palette, false, false);
    }
  }

  sceGuDepthMask(GU_FALSE);
  sceGuDepthFunc(GU_GEQUAL);
}

static void PspRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  const int texture_width = NextPowerOfTwo(width);
  const int texture_height = NextPowerOfTwo(height);
  const int texture_stride = (width + 15) & ~15;
  const size_t needed = (size_t)texture_stride * texture_height * 2;

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
  *pitch = texture_stride * 2;
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
  sceGuDisable(GU_DEPTH_TEST);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
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
  // Zelda advances once per presented frame. Always pace the PSP build on
  // the panel's 60 Hz vblank so gameplay cannot run uncapped.
  sceDisplayWaitVblankStart();
  sceGuSwapBuffers();
}

static uint32 PspColor(Ppu *ppu, uint16 color, bool transparent) {
  int brightness = ppu->brightness;
  int r = (((color & 31) << 3) | ((color & 31) >> 2)) * brightness / 15;
  int g5 = (color >> 5) & 31;
  int b5 = (color >> 10) & 31;
  int g = ((g5 << 3) | (g5 >> 2)) * brightness / 15;
  int b = ((b5 << 3) | (b5 >> 2)) * brightness / 15;
  return (transparent ? 0 : 0xff000000) | b << 16 | g << 8 | r;
}

void PspRenderer_DrawPpuFrame(Ppu *ppu, int width, int height) {
  g_logical_width = width;
  g_logical_height = height;
  g_logical_left = -(width - 256) / 2;
  g_dynamic_window = HasDynamicWindow();
  g_out_x0 = 0;
  g_out_y0 = 0;
  g_out_x1 = kPspScreenWidth;
  g_out_y1 = kPspScreenHeight;
  if (!g_config.ignore_aspect_ratio) {
    if (kPspScreenWidth * height < kPspScreenHeight * width) {
      int output_height = kPspScreenWidth * height / width;
      g_out_y0 = (kPspScreenHeight - output_height) / 2;
      g_out_y1 = g_out_y0 + output_height;
    } else {
      int output_width = kPspScreenHeight * width / height;
      g_out_x0 = (kPspScreenWidth - output_width) / 2;
      g_out_x1 = g_out_x0 + output_width;
    }
  }

  for (int i = 0; i < 256; i++)
    g_clut[i] = PspColor(ppu, ppu->cgram[i], (i & 15) == 0);
  sceKernelDcacheWritebackRange(g_clut, sizeof(g_clut));

  sceGuStart(GU_DIRECT, g_gu_list);
  sceGuScissor(0, 0, kPspScreenWidth, kPspScreenHeight);
  sceGuDepthMask(GU_FALSE);
  sceGuEnable(GU_DEPTH_TEST);
  // Clear the complete output to black first, matching the software
  // renderer's padding and letterbox behavior.
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
  SetActiveScissor(ppu);
  if (!ppu->forcedBlank) {
    // Only the part of the wide canvas exposed by the current area receives
    // the SNES backdrop color.
    sceGuClearColor(PspColor(ppu, ppu->cgram[0], false));
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_NOTEQUAL, 0, 0xff);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuDisable(GU_BLEND);

    if (ppu->mode == 1) {
      // SNES priority order, back to front.
      DrawBgPriority(ppu, 2, false, false, false);
      DrawSprites(ppu, 0, false, false);
      DrawSprites(ppu, 1, false, false);
      DrawBgPriority(ppu, 1, false, false, false);
      DrawBgPriority(ppu, 0, false, false, false);
      DrawSprites(ppu, 2, false, false);
      DrawBgPriority(ppu, 1, true, false, false);
      DrawBgPriority(ppu, 0, true, false, false);
      DrawSprites(ppu, 3, false, false);
      DrawBgPriority(ppu, 2, true, false, false);

      // Zelda's rain is BG1 on the subscreen with additive half color.
      // Let the GE perform that composition rather than touching each pixel
      // on Allegrex.
      if (ppu->addSubscreen && ppu->screenEnabled[1] && ppu->mathEnabled &&
          ppu->preventMathMode != 3) {
        // Draw front-to-back into a freshly cleared depth buffer. This blends
        // only the selected top subscreen pixel instead of accumulating every
        // transparent background layer that happens to cover it.
        sceGuDepthBuffer((void *)kPspSubDepthOffset, kPspFrameStride);
        sceGuClear(GU_DEPTH_BUFFER_BIT);
        sceGuEnable(GU_BLEND);
        uint32 factor = ppu->halfColor ? 0x007f7f7f : 0x00ffffff;
        sceGuBlendFunc(ppu->subtractColor ? GU_REVERSE_SUBTRACT : GU_ADD,
                       GU_FIX, GU_FIX, factor, factor);
        DrawBgPriority(ppu, 2, true, true, true);
        DrawSprites(ppu, 3, true, true);
        DrawBgPriority(ppu, 0, true, true, true);
        DrawBgPriority(ppu, 1, true, true, true);
        DrawSprites(ppu, 2, true, true);
        DrawBgPriority(ppu, 0, false, true, true);
        DrawBgPriority(ppu, 1, false, true, true);
        DrawSprites(ppu, 1, true, true);
        DrawSprites(ppu, 0, true, true);
        DrawBgPriority(ppu, 2, false, true, true);
        sceGuDisable(GU_BLEND);
        RestoreLayersExcludedFromMath(ppu);
      }
      DrawDynamicWindowDarkness(ppu);
    } else {
      // Mode 7 sprites remain GE-native while the affine tile-plane path is
      // built from the same atlas machinery.
      DrawSprites(ppu, 0, false, false);
      DrawSprites(ppu, 1, false, false);
      DrawSprites(ppu, 2, false, false);
      DrawSprites(ppu, 3, false, false);
    }
  }
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  // Native PPU rendering is synchronized to one game update per display
  // refresh, regardless of the desktop-oriented DisableFrameDelay setting.
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
