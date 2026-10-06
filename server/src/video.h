// Emisor de video: diff de tiles -> codificacion hibrida (RLE para tiles planos
// tipo UI/menus/texto, JPEG para el resto) -> fragmentacion UDP segun protocol.h.
// El RLE decodifica casi gratis en el cliente (bucle trivial), asi que se prueba
// primero; solo cae a JPEG cuando el contenido es demasiado complejo para RLE.
#pragma once
#include "convert.h"
#include <cstdint>
#include <vector>
#include <winsock2.h>

// Diagnostico: si es >0, duerme este numero de milisegundos entre CADA
// datagrama UDP enviado (video.cpp). Sirve para probar la hipotesis de que una
// rafaga de keyframe (muchos paquetes de golpe, ETC1 pesa mas que JPEG) esta
// saturando el buffer de envio / la WiFi y perdiendo paquetes -- sin retry,
// un tile perdido en un patron ESTATICO se queda corrupto para siempre.
extern int g_udpPaceMs;

struct VideoConfig {
    int      width  = 400;
    int      height = 240;
    int      tileW  = 80;
    int      tileH  = 80;
    uint8_t  codec  = 0;   // CODEC_RAW_YUV420 / CODEC_JPEG_YCBCR / CODEC_ETC1
    uint8_t  keyframeInterval = 30;
    int      jpegQuality = 55;
    int      fps     = 30;
    int      maxKbps = 6000;  // techo de envio (del ClientHello o --maxkbps)
};

// Contadores acumulados desde la ultima lectura (para la linea de stats).
struct VideoStats {
    uint64_t bytes   = 0;   // bytes de payload enviados
    int      tiles   = 0;   // tiles enviados
    double   encodeMs = 0;  // tiempo total de codificacion ETC1
    int      backlog = 0;   // tiles pendientes (dirty sin enviar) ahora mismo
};

class VideoSender {
public:
    ~VideoSender();
    // sock: UDP ya creado. dst: destino (IP:puerto del 3DS).
    bool init(SOCKET sock, const sockaddr_in& dst, const VideoConfig& cfg);

    // Procesa un frame YUV ya convertido (RAW/JPEG): detecta tiles sucios y los
    // envia. forceKey fuerza un keyframe (todos los tiles).
    void sendFrame(const YuvFrame& cur, bool forceKey = false);

    // Igual que arriba, pero para el codec ETC1 (fuente RGB888, sin YUV), con
    // control de flujo: presupuesto de bytes/frame (maxKbps/fps) + refresco
    // rotativo en vez de keyframes en rafaga. forceKey re-marca todos los tiles.
    void sendFrameEtc1(const RgbFrame& cur, bool forceKey = false);

    // Ajustes en vivo (desde mensajes de control del cliente).
    void setQuality(int q)          { if (q < 10) q = 10; if (q > 95) q = 95; cfg_.jpegQuality = q; }
    void setKeyframeInterval(int n) { if (n < 1) n = 1;  cfg_.keyframeInterval = (uint8_t)n; }
    void setFps(int f);   // recalcula el presupuesto de bytes/frame (ETC1)

    int tilesX() const { return tilesX_; }
    int tilesY() const { return tilesY_; }

    // Devuelve los contadores acumulados desde la ultima llamada y los resetea.
    VideoStats statsFetch();

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

    // --- Estado especifico de ETC1 ---
    RgbFrame             prevRgb_;
    std::vector<uint8_t> dirtyRgb_;
    std::vector<uint8_t> tileBufRgb_;   // RGB888 del tile actual
    std::vector<uint8_t> etc1Buf_;      // salida ETC1 del tile actual (tamano fijo)

    // Control de flujo ETC1: en vez de un keyframe completo por segundo (rafaga
    // de ~28 datagramas que la WiFi del Old 3DS no absorbe), cada tile se marca
    // pendiente cuando cambia (o le toca refresco rotativo) y por frame se envia
    // como maximo el presupuesto de bytes derivado de maxKbps/fps. Lo que no
    // cabe queda pendiente para el frame siguiente (round-robin, sin inanicion).
    // pendingEtc1_: 0 = nada, 1 = puede ir como DELTA (solo bloques cambiados),
    // 2 = debe ir COMPLETO (primera vez / refresco rotativo / keyframe — los
    // completos son el mecanismo de reparacion ante perdida UDP: un delta asume
    // que el cliente tiene la base correcta, un completo restablece todo).
    std::vector<uint8_t> pendingEtc1_;
    std::vector<uint8_t> lastEtc1_;      // ultima codificacion ENVIADA por tile
    std::vector<uint8_t> tileEverSent_;  // 0/1: ya se envio completo alguna vez
    std::vector<uint8_t> sendScratch_;   // payloads del frame en curso (jobs)
    int                  rotRefresh_ = 0;  // proximo tile del refresco rotativo
    int                  rotAccum_   = 0;  // acumulador fraccional del refresco
    int                  sendCursor_ = 0;  // arranque round-robin del envio
    size_t               frameBudget_ = 0; // bytes de payload por frame

    VideoStats           stats_;
};
