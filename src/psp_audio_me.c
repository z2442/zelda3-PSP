#ifdef __PSP__

#include "psp_audio_me.h"

#include "audio.h"
#include "spc_player.h"

#include <me-core-mapper/me-core.h>
#include <pspkernel.h>
#include <string.h>

#define PSP_AUDIO_ME_CACHE_LINE 64
#define PSP_AUDIO_ME_MAX_SAMPLES 1024
#define PSP_AUDIO_ME_MAX_CHANNELS 2
#define PSP_AUDIO_ME_BOOT_TIMEOUT_US 250000
#define PSP_AUDIO_ME_JOB_TIMEOUT_US 250000
#define PSP_AUDIO_ME_POLL_US 50

enum {
  kPspAudioMeBooting,
  kPspAudioMeIdle,
  kPspAudioMeRun,
  kPspAudioMeStop,
  kPspAudioMeHalted,
  kPspAudioMeFault,
};

enum {
  kSharedState,
  kSharedProgress,
  kSharedPlayer,
  kSharedDsp,
  kSharedSamples,
  kSharedChannels,
  kSharedPortsIn,
  kSharedPortsOut,
  kSharedCount,
};

static volatile uint32_t g_me_shared_storage[kSharedCount]
    __attribute__((aligned(PSP_AUDIO_ME_CACHE_LINE), section(".uncached")));
#define ME_SHARED ((volatile uint32_t *)(UNCACHED_USER_MASK | (uintptr_t)g_me_shared_storage))

static int16_t g_me_output[PSP_AUDIO_ME_MAX_SAMPLES * PSP_AUDIO_ME_MAX_CHANNELS]
    __attribute__((aligned(PSP_AUDIO_ME_CACHE_LINE)));
static SpcPlayer *g_me_player;
static bool g_me_active;

static void AlignRange(const void *address, uint32_t size, void **start_out, uint32_t *size_out) {
  uintptr_t start = (uintptr_t)address & ~(uintptr_t)(PSP_AUDIO_ME_CACHE_LINE - 1);
  uintptr_t end = ((uintptr_t)address + size + PSP_AUDIO_ME_CACHE_LINE - 1) &
                  ~(uintptr_t)(PSP_AUDIO_ME_CACHE_LINE - 1);
  *start_out = (void *)start;
  *size_out = (uint32_t)(end - start);
}

static void CpuWritebackInvalidate(const void *address, uint32_t size) {
  void *start;
  uint32_t aligned_size;
  AlignRange(address, size, &start, &aligned_size);
  sceKernelDcacheWritebackInvalidateRange(start, aligned_size);
}

static void CpuInvalidate(const void *address, uint32_t size) {
  void *start;
  uint32_t aligned_size;
  AlignRange(address, size, &start, &aligned_size);
  sceKernelDcacheInvalidateRange(start, aligned_size);
}

static void MeInvalidate(const void *address, uint32_t size) {
  void *start;
  uint32_t aligned_size;
  AlignRange(address, size, &start, &aligned_size);
  meCoreDcacheInvalidateRange(start, aligned_size);
}

static void MeWriteback(const void *address, uint32_t size) {
  void *start;
  uint32_t aligned_size;
  AlignRange(address, size, &start, &aligned_size);
  meCoreDcacheWritebackRange(start, aligned_size);
}

__attribute__((noinline, aligned(4))) void meLibOnException(void) {
  ME_SHARED[kSharedState] = kPspAudioMeFault;
  meLibSync();
  meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnExternalInterrupt(void) {
  ME_SHARED[kSharedState] = kPspAudioMeFault;
  meLibSync();
  meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnProcess(void) {
  ME_SHARED[kSharedProgress] = 1;
  meLibSync();

  while (ME_SHARED[kSharedState] == kPspAudioMeBooting)
    meLibDelayPipeline();

  ME_SHARED[kSharedProgress] = 2;
  meLibSync();

  while (ME_SHARED[kSharedState] != kPspAudioMeStop) {
    if (ME_SHARED[kSharedState] == kPspAudioMeRun) {
      SpcPlayer *player = (SpcPlayer *)(uintptr_t)ME_SHARED[kSharedPlayer];
      Dsp *dsp = (Dsp *)(uintptr_t)ME_SHARED[kSharedDsp];
      uint8_t ports[4];
      uint32_t packed_ports = ME_SHARED[kSharedPortsIn];
      int samples = (int)ME_SHARED[kSharedSamples];
      int channels = (int)ME_SHARED[kSharedChannels];

      ports[0] = packed_ports;
      ports[1] = packed_ports >> 8;
      ports[2] = packed_ports >> 16;
      ports[3] = packed_ports >> 24;

      MeInvalidate(player, sizeof(*player));
      MeInvalidate(dsp, sizeof(*dsp));
      MeInvalidate(g_me_output, samples * channels * sizeof(g_me_output[0]));

      ZeldaRenderAudioPrepared(player, g_me_output, samples, channels, ports);

      MeWriteback(g_me_output, samples * channels * sizeof(g_me_output[0]));
      MeWriteback(dsp, sizeof(*dsp));
      MeWriteback(player, sizeof(*player));
      ME_SHARED[kSharedPortsOut] = player->port_to_snes[0] |
                                   player->port_to_snes[1] << 8 |
                                   player->port_to_snes[2] << 16 |
                                   player->port_to_snes[3] << 24;
      meLibSync();
      ME_SHARED[kSharedState] = kPspAudioMeIdle;
      meLibSync();
    } else {
      meLibDelayPipeline();
    }
  }

  ME_SHARED[kSharedState] = kPspAudioMeHalted;
  meLibSync();
  meLibHalt();
}

static bool WaitForValue(int index, uint32_t value, uint32_t timeout_us) {
  uint32_t start = sceKernelGetSystemTimeLow();
  while (ME_SHARED[index] != value) {
    if (ME_SHARED[kSharedState] == kPspAudioMeFault ||
        sceKernelGetSystemTimeLow() - start >= timeout_us)
      return false;
    sceKernelDelayThread(PSP_AUDIO_ME_POLL_US);
  }
  return true;
}

bool PspAudioMe_Init(SpcPlayer *player) {
  if (g_me_active)
    return true;

  memset((void *)g_me_shared_storage, 0, sizeof(g_me_shared_storage));
  ME_SHARED[kSharedState] = kPspAudioMeBooting;
  meLibSync();
  int result = meLibDefaultInit();
  if (result < 0)
    return false;

  if (!WaitForValue(kSharedProgress, 1, PSP_AUDIO_ME_BOOT_TIMEOUT_US))
    return false;

  g_me_player = player;
  CpuWritebackInvalidate(player, sizeof(*player));
  CpuWritebackInvalidate(player->dsp, sizeof(*player->dsp));
  ME_SHARED[kSharedPlayer] = (uint32_t)(uintptr_t)player;
  ME_SHARED[kSharedDsp] = (uint32_t)(uintptr_t)player->dsp;
  ME_SHARED[kSharedPortsOut] = player->port_to_snes[0] |
                               player->port_to_snes[1] << 8 |
                               player->port_to_snes[2] << 16 |
                               player->port_to_snes[3] << 24;
  meLibSync();
  ME_SHARED[kSharedState] = kPspAudioMeIdle;
  meLibSync();

  if (!WaitForValue(kSharedProgress, 2, PSP_AUDIO_ME_BOOT_TIMEOUT_US))
    return false;

  g_me_active = true;
  return true;
}

bool PspAudioMe_IsActive(void) {
  return g_me_active;
}

bool PspAudioMe_Render(int16_t *output, int samples, int channels, const uint8_t ports[4]) {
  if (!g_me_active || samples <= 0 || samples > PSP_AUDIO_ME_MAX_SAMPLES ||
      channels <= 0 || channels > PSP_AUDIO_ME_MAX_CHANNELS)
    return false;

  if (!WaitForValue(kSharedState, kPspAudioMeIdle, PSP_AUDIO_ME_JOB_TIMEOUT_US)) {
    memset(output, 0, samples * channels * sizeof(*output));
    return false;
  }

  ME_SHARED[kSharedSamples] = samples;
  ME_SHARED[kSharedChannels] = channels;
  ME_SHARED[kSharedPortsIn] = ports[0] | ports[1] << 8 | ports[2] << 16 | ports[3] << 24;
  meLibSync();
  ME_SHARED[kSharedState] = kPspAudioMeRun;
  meLibSync();

  if (!WaitForValue(kSharedState, kPspAudioMeIdle, PSP_AUDIO_ME_JOB_TIMEOUT_US)) {
    memset(output, 0, samples * channels * sizeof(*output));
    return false;
  }

  CpuInvalidate(g_me_output, samples * channels * sizeof(*output));
  memcpy(output, g_me_output, samples * channels * sizeof(*output));
  return true;
}

uint8_t PspAudioMe_ReadPort(int port) {
  return (ME_SHARED[kSharedPortsOut] >> ((port & 3) * 8)) & 0xff;
}

void PspAudioMe_LockState(void) {
  if (!g_me_active)
    return;
  while (ME_SHARED[kSharedState] == kPspAudioMeRun)
    sceKernelDelayThread(PSP_AUDIO_ME_POLL_US);
  CpuInvalidate(g_me_player, sizeof(*g_me_player));
  CpuInvalidate(g_me_player->dsp, sizeof(*g_me_player->dsp));
}

void PspAudioMe_UnlockState(void) {
  if (!g_me_active)
    return;
  ME_SHARED[kSharedPortsOut] = g_me_player->port_to_snes[0] |
                               g_me_player->port_to_snes[1] << 8 |
                               g_me_player->port_to_snes[2] << 16 |
                               g_me_player->port_to_snes[3] << 24;
  CpuWritebackInvalidate(g_me_player->dsp, sizeof(*g_me_player->dsp));
  CpuWritebackInvalidate(g_me_player, sizeof(*g_me_player));
  meLibSync();
}

void PspAudioMe_Shutdown(void) {
  if (!g_me_active)
    return;
  PspAudioMe_LockState();
  ME_SHARED[kSharedState] = kPspAudioMeStop;
  meLibSync();
  WaitForValue(kSharedState, kPspAudioMeHalted, PSP_AUDIO_ME_BOOT_TIMEOUT_US);
  g_me_active = false;
}

#endif
