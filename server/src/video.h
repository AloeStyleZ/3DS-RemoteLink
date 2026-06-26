// Emisor de video: diff de tiles -> codificacion (RAW YUV420 o JPEG) ->
// fragmentacion en datagramas UDP segun protocol.h.
#pragma once
#include "convert.h"
#include <cstdint>
#include <vector>
#include <winsock2.h>

struct VideoConfig {
    int      width  = 400;
    int      height = 240;
    int      tileW  = 80;
    int      tileH  = 80;
    uint8_t  codec  = 0;   // CODEC_RAW_YUV420 / CODEC_JPEG_YCBCR
    uint8_t  keyframeInterval = 30;
    int      jpegQuality = 55;
};

class VideoSender {
public:
    ~VideoSender();
    // sock: UDP ya creado. dst: destino (IP:puerto del 3DS).
    bool init(SOCKET sock, const sockaddr_in& dst, const VideoConfig& cfg);

    // Procesa un frame YUV ya convertido: detecta tiles sucios y los envia.
    // forceKey fuerza un keyframe (todos los tiles).
    void sendFrame(const YuvFrame& cur, bool forceKey = false);

    int tilesX() const { return tilesX_; }
    int tilesY() const { return tilesY_; }

private:
    // Codifica el tile contenido en tileBuf_ (RAW de tamano rawBytes).
    // Devuelve puntero+tamano del payload a enviar (RAW o JPEG).
    const uint8_t* encodeTile(size_t rawBytes, size_t& outBytes);

    SOCKET      sock_ = INVALID_SOCKET;
    sockaddr_in dst_{};
    VideoConfig cfg_{};
    int         tilesX_ = 0, tilesY_ = 0;

    YuvFrame             prev_;
    std::vector<uint8_t> dirty_;
    std::vector<uint8_t> tileBuf_;   // RAW I420 del tile actual
    uint16_t             frameId_ = 0;
    int                  framesSinceKey_ = 0;

#ifdef HAVE_TURBOJPEG
    void*          tjh_ = nullptr;       // tjhandle
    unsigned char* jpegBuf_ = nullptr;   // buffer de salida JPEG
    unsigned long  jpegCap_ = 0;
#endif
};
