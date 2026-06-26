// Decoder de video: reensambla tiles RAW YUV420 en un framebuffer persistente,
// convierte a RGB con el modulo HW Y2R y lo expone como C2D_Image (textura POT).
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include "protocol.h"

#define MAX_FRAGS_PER_TILE 32  // tope de fragmentos por tile (RAW 80x80 ~ 7)

// Estado de reensamblado de un tile para el frame en curso.
typedef struct {
    u16 frameId;     // frame que se esta reensamblando (0xFFFF = ninguno)
    u16 fragCount;   // fragmentos esperados
    u16 received;    // fragmentos recibidos
    u16 tileBytes;   // bytes totales del tile
    u32 fragMask;    // bitmask de fragmentos ya recibidos
    u8* buf;         // buffer de reensamblado (maxTileBytes)
} TileReasm;

typedef struct VideoDecoder {
    int width, height;
    int tileW, tileH, tilesX, tilesY, tileCount;
    int maxTileBytes;

    // Planos I420 persistentes en memoria lineal (delta frames aplicados aqui).
    u8 *y, *u, *v;
    size_t ySize, cSize;

    TileReasm* reasm;

    // Salida: textura POT RGB565 envuelta como C2D_Image.
    C3D_Tex           tex;
    Tex3DS_SubTexture subtex;
    C2D_Image         img;

    Handle y2rEvent;
    bool   ready;          // hay al menos un frame valido en la textura
    bool   pendingPresent; // llego un FRAME_END; falta hacer Y2R+present
    void*  tjDec;          // tjhandle de decompresion JPEG (turbojpeg)
} VideoDecoder;

bool video_init(VideoDecoder* d, int w, int h, int tileW, int tileH);
void video_exit(VideoDecoder* d);

// Procesa un datagrama de video (cabecera+payload). Devuelve true si el paquete
// llevaba VFLAG_FRAME_END (frame presentado / textura actualizada).
bool video_on_packet(VideoDecoder* d, const u8* pkt, int len);

// Si hay un frame completo pendiente, lo convierte con Y2R y actualiza la
// textura. Llamar UNA vez por iteracion del bucle, tras drenar el socket.
void video_update(VideoDecoder* d);

// Dibuja la textura actual (llamar dentro de C2D_SceneBegin).
void video_draw(VideoDecoder* d, float x, float y);
