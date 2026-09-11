#ifndef ZELDA3_PSP_AUDIO_ME_H_
#define ZELDA3_PSP_AUDIO_ME_H_

#ifdef __PSP__

#include <stdbool.h>
#include <stdint.h>

struct SpcPlayer;

bool PspAudioMe_Init(struct SpcPlayer *player);
void PspAudioMe_Shutdown(void);
bool PspAudioMe_IsActive(void);
bool PspAudioMe_Render(int16_t *output, int samples, int channels, const uint8_t ports[4]);
uint8_t PspAudioMe_ReadPort(int port);

// Called while g_audio_mutex is held. These make CPU-side save/load/reset
// operations coherent with the ME-owned SPC and DSP state.
void PspAudioMe_LockState(void);
void PspAudioMe_UnlockState(void);

#endif

#endif
