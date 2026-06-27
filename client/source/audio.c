#include "audio.h"
#include <string.h>
#include <malloc.h>

#define AUDIO_NUM_WBUF     8
#define AUDIO_WBUF_SAMPLES 1024   // muestras por wave buffer (cubre un datagrama)

static bool        s_ready = false;
static ndspWaveBuf s_wbuf[AUDIO_NUM_WBUF];
static s16*        s_data[AUDIO_NUM_WBUF];
static int         s_idx  = 0;
static u32         s_rate = 0;

bool audio_init(void) {
    if (R_FAILED(ndspInit())) return false;   // requiere dspfirm.cdc (CFW lo tiene)

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 48000.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f; mix[1] = 1.0f;             // mono -> ambos canales
    ndspChnSetMix(0, mix);

    for (int i = 0; i < AUDIO_NUM_WBUF; ++i) {
        s_data[i] = (s16*)linearAlloc(AUDIO_WBUF_SAMPLES * sizeof(s16));
        memset(&s_wbuf[i], 0, sizeof(s_wbuf[i]));   // status = NDSP_WBUF_FREE
        s_wbuf[i].data_vaddr = s_data[i];
    }
    s_rate  = 48000;
    s_ready = true;
    return true;
}

void audio_feed(const s16* pcm, int samples, u32 rate) {
    if (!s_ready || samples <= 0) return;
    if (rate && rate != s_rate) { ndspChnSetRate(0, (float)rate); s_rate = rate; }

    ndspWaveBuf* wb = &s_wbuf[s_idx];
    // Solo reusar un buffer libre o ya reproducido (si no, lo descartamos).
    if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) return;

    int n = samples;
    if (n > AUDIO_WBUF_SAMPLES) n = AUDIO_WBUF_SAMPLES;
    memcpy(s_data[s_idx], pcm, (size_t)n * sizeof(s16));
    DSP_FlushDataCache(s_data[s_idx], (size_t)n * sizeof(s16));

    memset(wb, 0, sizeof(*wb));
    wb->data_vaddr = s_data[s_idx];
    wb->nsamples   = n;
    ndspChnWaveBufAdd(0, wb);

    s_idx = (s_idx + 1) % AUDIO_NUM_WBUF;
}

void audio_exit(void) {
    if (!s_ready) return;
    ndspExit();
    for (int i = 0; i < AUDIO_NUM_WBUF; ++i)
        if (s_data[i]) { linearFree(s_data[i]); s_data[i] = NULL; }
    s_ready = false;
}
