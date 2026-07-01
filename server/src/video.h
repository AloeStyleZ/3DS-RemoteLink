// Emisor de video: diff de tiles -> codificacion hibrida (RLE para tiles planos
// tipo UI/menus/texto, JPEG para el resto) -> fragmentacion UDP segun protocol.h.
// El RLE decodifica casi gratis en el cliente (bucle trivial), asi que se prueba
// primero; solo cae a JPEG cuando el contenido es demasiado complejo para RLE.
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

    // Ajustes en vivo (desde mensajes de control del cliente).
    void setQuality(int q)          { if (q < 10) q = 10; if (q > 95) q = 95; cfg_.jpegQuality = q; }
    void setKeyframeInterval(int n) { if (n < 1) n = 1;  cfg_.keyframeInterval = (uint8_t)n; }

    int tilesX() const { return tilesX_; }
    int tilesY() const { return tilesY_; }

private:
    // Codifica el tile contenido en tileBuf_ (RAW de tamano rawBytes) a la
    // calidad indicada. Devuelve puntero+tamano del payload a enviar (RAW o JPEG).
    const uint8_t* encodeTile(size_t rawBytes, int quality, size_t& outBytes);

    // Intenta RLE sobre tileBuf_[0..rawBytes). Devuelve bytes escritos en rleBuf_,
    // o 0 si no compensa (tiles con contenido complejo -> mejor JPEG). Barato:
    // aborta en cuanto el output supera el umbral, sin terminar de recorrer el tile.
    size_t tryRle(size_t rawBytes);

    SOCKET      sock_ = INVALID_SOCKET;
    sockaddr_in dst_{};
    VideoConfig cfg_{};
    int         tilesX_ = 0, tilesY_ = 0;

    YuvFrame             prev_;
    std::vector<uint8_t> dirty_;
    std::vector<uint8_t> tileBuf_;   // RAW I420 del tile actual
    std::vector<uint8_t> rleBuf_;    // salida RLE del tile actual (si compensa)
    std::vector<uint8_t> lastQ_;     // calidad con la que se envio cada tile (255 = RLE/lossless)
    int                  refineCursor_ = 0;
    uint16_t             frameId_ = 0;
    int                  framesSinceKey_ = 0;

#ifdef HAVE_TURBOJPEG
    void*          tjh_ = nullptr;       // tjhandle
    unsigned char* jpegBuf_ = nullptr;   // buffer de salida JPEG
    unsigned long  jpegCap_ = 0;
#endif
};
