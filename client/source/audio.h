// Reproduccion de audio en la 3DS (NDSP). Recibe PCM16 mono del servidor.
#pragma once
#include <3ds.h>

bool audio_init(void);
// Encola un bloque de muestras PCM16 mono para reproducir (rate en Hz).
void audio_feed(const s16* pcm, int samples, u32 rate);
void audio_exit(void);
