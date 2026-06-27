#define WIN32_LEAN_AND_MEAN
#include "audio.h"
#include "protocol.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>     // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT

#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdint>

AudioSender::~AudioSender() { stop(); }

bool AudioSender::start(const sockaddr_in& dst) {
    dst_  = dst;
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;
    running_ = true;
    thread_  = std::thread(&AudioSender::run, this);
    return true;
}

void AudioSender::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
}

static inline int16_t f32_to_s16(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

void AudioSender::run() {
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return;

    IMMDeviceEnumerator* en  = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        ac  = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX*        wf  = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) break;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ac))) break;
        if (FAILED(ac->GetMixFormat(&wf))) break;
        // Buffer de 200ms. AUDCLNT_STREAMFLAGS_LOOPBACK => captura lo que suena.
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  2000000, 0, wf, NULL))) break;
        if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) break;
        ok = true;
    } while (0);

    if (ok) {
        const int srcCh   = wf->nChannels;
        const int srcRate = (int)wf->nSamplesPerSec;
        const int bps     = wf->wBitsPerSample / 8;
        bool isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* we = (WAVEFORMATEXTENSIBLE*)wf;
            if (IsEqualGUID(we->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) isFloat = true;
        }

        ac->Start();
        std::vector<int16_t> mono;
        mono.reserve(AUDIO_MAX_SAMPLES * 2);
        uint16_t seq = 0;

        while (running_.load()) {
            UINT32 avail = 0;
            if (FAILED(cap->GetNextPacketSize(&avail))) break;
            if (avail == 0) { Sleep(5); continue; }

            while (avail != 0) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, NULL, NULL))) { avail = 0; break; }

                if (enabled_.load()) {
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    for (UINT32 i = 0; i < frames; ++i) {
                        int16_t s = 0;
                        if (!silent) {
                            if (isFloat && bps == 4) {
                                const float* fp = (const float*)(data + (size_t)i * srcCh * 4);
                                float acc = 0; for (int c = 0; c < srcCh; ++c) acc += fp[c];
                                s = f32_to_s16(acc / srcCh);
                            } else if (bps == 2) {
                                const int16_t* ip = (const int16_t*)(data + (size_t)i * srcCh * 2);
                                int acc = 0; for (int c = 0; c < srcCh; ++c) acc += ip[c];
                                s = (int16_t)(acc / srcCh);
                            }
                        }
                        mono.push_back(s);
                        if ((int)mono.size() >= AUDIO_MAX_SAMPLES) {
                            uint8_t buf[sizeof(AudioPacketHeader) + AUDIO_MAX_SAMPLES * 2];
                            AudioPacketHeader* h = (AudioPacketHeader*)buf;
                            h->magic = PROTO_MAGIC; h->type = PKT_AUDIO; h->seq = seq++;
                            h->samples = (uint16_t)mono.size(); h->rate = (uint16_t)srcRate;
                            memcpy(buf + sizeof(AudioPacketHeader), mono.data(), mono.size() * 2);
                            sendto(sock_, (const char*)buf,
                                   (int)(sizeof(AudioPacketHeader) + mono.size() * 2),
                                   0, (sockaddr*)&dst_, sizeof(dst_));
                            mono.clear();
                        }
                    }
                }
                cap->ReleaseBuffer(frames);
                if (FAILED(cap->GetNextPacketSize(&avail))) { avail = 0; break; }
            }
        }
        ac->Stop();
    } else {
        fprintf(stderr, "[audio] WASAPI loopback no disponible\n");
    }

    if (wf)  CoTaskMemFree(wf);
    if (cap) cap->Release();
    if (ac)  ac->Release();
    if (dev) dev->Release();
    if (en)  en->Release();
    CoUninitialize();
}
