// Conversion BGRA -> I420 (YUV420 planar) con escalado, y deteccion de tiles
// cambiados (delta frames). Todo en CPU; en el PC es trivial. Para produccion
// se puede sustituir bgraToI420Scaled() por libyuv (ARGBScale + ARGBToI420).
#pragma once
#include <cstdint>
#include <vector>

// Frame YUV420 planar (I420): plano Y a resolucion completa, U y V a 1/4.
struct YuvFrame {
    int w = 0, h = 0;
    std::vector<uint8_t> y; // w*h
    std::vector<uint8_t> u; // (w/2)*(h/2)
    std::vector<uint8_t> v; // (w/2)*(h/2)

    void alloc(int width, int height) {
        w = width; h = height;
        y.assign((size_t)w * h, 16);
        u.assign((size_t)(w / 2) * (h / 2), 128);
        v.assign((size_t)(w / 2) * (h / 2), 128);
    }
    int yStride() const { return w; }
    int cStride() const { return w / 2; }
};

// Escala BGRA (srcW x srcH, srcPitch bytes/fila) -> dst.w x dst.h en I420.
// dst debe estar pre-asignado con alloc().
void bgraToI420Scaled(const uint8_t* bgra, int srcW, int srcH, int srcPitch, YuvFrame& dst);

// Marca en `dirty` (tamano tilesX*tilesY, 1=cambiado) los tiles que difieren
// entre cur y prev. Si keyframe==true, marca todos.
void computeDirtyTiles(const YuvFrame& cur, const YuvFrame& prev,
                       int tileW, int tileH, int tilesX, int tilesY,
                       bool keyframe, std::vector<uint8_t>& dirty);

// Extrae el tile (tx,ty) de f en formato I420 contiguo (Y | U | V) en `out`.
// Devuelve el numero de bytes escritos.
size_t extractTile(const YuvFrame& f, int tx, int ty, int tileW, int tileH,
                   std::vector<uint8_t>& out);
