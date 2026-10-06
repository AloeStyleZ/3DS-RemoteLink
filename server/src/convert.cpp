#include "convert.h"
#include <cstring>
#include <algorithm>

// BT.601 limited range, aritmetica entera (coeficientes *256).
static inline uint8_t rgb2y(int R, int G, int B) {
    return (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
}
static inline uint8_t rgb2u(int R, int G, int B) {
    return (uint8_t)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
}
static inline uint8_t rgb2v(int R, int G, int B) {
    return (uint8_t)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
}

void bgraToI420Scaled(const uint8_t* src, int sw, int sh, int sp, YuvFrame& d) {
    const int dw = d.w, dh = d.h;
    const int cw = dw / 2, ch = dh / 2;

    // LUTs de mapeo de columnas (evita una division por pixel).
    std::vector<int> xmapY(dw), xmapC(cw);
    for (int x = 0; x < dw; ++x) xmapY[x] = ((x * sw) / dw) * 4;
    for (int x = 0; x < cw; ++x) xmapC[x] = (((x * 2) * sw) / dw) * 4;

    // Plano Y (resolucion completa).
    for (int y = 0; y < dh; ++y) {
        const uint8_t* srow = src + (size_t)((y * sh) / dh) * sp;
        uint8_t* yrow = d.y.data() + (size_t)y * dw;
        for (int x = 0; x < dw; ++x) {
            const uint8_t* p = srow + xmapY[x]; // B,G,R,A
            yrow[x] = rgb2y(p[2], p[1], p[0]);
        }
    }

    // Planos U/V (submuestreo 2x2: se toma 1 muestra por bloque).
    for (int y = 0; y < ch; ++y) {
        const uint8_t* srow = src + (size_t)(((y * 2) * sh) / dh) * sp;
        uint8_t* urow = d.u.data() + (size_t)y * cw;
        uint8_t* vrow = d.v.data() + (size_t)y * cw;
        for (int x = 0; x < cw; ++x) {
            const uint8_t* p = srow + xmapC[x];
            urow[x] = rgb2u(p[2], p[1], p[0]);
            vrow[x] = rgb2v(p[2], p[1], p[0]);
        }
    }
}

// Compara una region rectangular entre dos planos (mismo stride). true si difiere.
static bool planeRegionDiffers(const uint8_t* a, const uint8_t* b, int stride,
                               int x0, int y0, int rw, int rh) {
    for (int y = 0; y < rh; ++y) {
        const uint8_t* ra = a + (size_t)(y0 + y) * stride + x0;
        const uint8_t* rb = b + (size_t)(y0 + y) * stride + x0;
        if (memcmp(ra, rb, rw) != 0) return true;
    }
    return false;
}

void computeDirtyTiles(const YuvFrame& cur, const YuvFrame& prev,
                       int tileW, int tileH, int tilesX, int tilesY,
                       bool keyframe, std::vector<uint8_t>& dirty) {
    dirty.assign((size_t)tilesX * tilesY, 0);
    if (keyframe || prev.w != cur.w || prev.h != cur.h) {
        std::fill(dirty.begin(), dirty.end(), (uint8_t)1);
        return;
    }
    const int cTileW = tileW / 2, cTileH = tileH / 2;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int yx = tx * tileW, yy = ty * tileH;
            bool changed =
                planeRegionDiffers(cur.y.data(), prev.y.data(), cur.yStride(),
                                   yx, yy, tileW, tileH);
            if (!changed) {
                const int cx = tx * cTileW, cy = ty * cTileH;
                changed = planeRegionDiffers(cur.u.data(), prev.u.data(),
                                             cur.cStride(), cx, cy, cTileW, cTileH) ||
                          planeRegionDiffers(cur.v.data(), prev.v.data(),
                                             cur.cStride(), cx, cy, cTileW, cTileH);
            }
            dirty[(size_t)ty * tilesX + tx] = changed ? 1 : 0;
        }
    }
}

size_t extractTile(const YuvFrame& f, int tx, int ty, int tileW, int tileH,
                   std::vector<uint8_t>& out) {
    const int cTileW = tileW / 2, cTileH = tileH / 2;
    const size_t need = (size_t)tileW * tileH + 2 * (size_t)cTileW * cTileH;
    out.resize(need);
    uint8_t* dst = out.data();

    // Y
    const int yx = tx * tileW, yy = ty * tileH;
    for (int r = 0; r < tileH; ++r) {
        memcpy(dst, f.y.data() + (size_t)(yy + r) * f.yStride() + yx, tileW);
        dst += tileW;
    }
    // U
    const int cx = tx * cTileW, cy = ty * cTileH;
    for (int r = 0; r < cTileH; ++r) {
        memcpy(dst, f.u.data() + (size_t)(cy + r) * f.cStride() + cx, cTileW);
        dst += cTileW;
    }
    // V
    for (int r = 0; r < cTileH; ++r) {
        memcpy(dst, f.v.data() + (size_t)(cy + r) * f.cStride() + cx, cTileW);
        dst += cTileW;
    }
    return need;
}

// =============================================================================
// Pipeline RGB888 (codec ETC1): sin conversion de color, solo reordenar B,G,R,A
// -> R,G,B y escalar. Evita la perdida de croma del YUV420, relevante porque
// ETC1 no tiene margen de calidad ajustable (tasa fija).
// =============================================================================

void bgraToRgbScaled(const uint8_t* src, int sw, int sh, int sp, RgbFrame& d) {
    const int dw = d.w, dh = d.h;

    // Promedio de AREA (box filter): cada pixel destino = media de TODO su
    // bloque fuente (~6x4 pixeles al bajar 1920x1080 -> 320x240). El vecino-
    // mas-cercano anterior tomaba 1 muestra y descartaba ~96% de la imagen:
    // texto roto, bordes dentados y "shimmering" al moverse. El coste extra es
    // solo del lado PC (unos ms); para la 3DS es identico. Ademas la imagen
    // suavizada dispara menos tiles sucios y ETC1 la comprime con menos error.
    //
    // Bordes de cada bloque precalculados (enteros, sin division por pixel).
    std::vector<int> xEdge(dw + 1), yEdge(dh + 1);
    for (int x = 0; x <= dw; ++x) xEdge[x] = (int)((int64_t)x * sw / dw);
    for (int y = 0; y <= dh; ++y) yEdge[y] = (int)((int64_t)y * sh / dh);

    for (int y = 0; y < dh; ++y) {
        const int sy0 = yEdge[y];
        const int sy1 = (yEdge[y + 1] > sy0) ? yEdge[y + 1] : sy0 + 1;
        uint8_t* drow = d.rgb.data() + (size_t)y * d.stride();

        for (int x = 0; x < dw; ++x) {
            const int sx0 = xEdge[x];
            const int sx1 = (xEdge[x + 1] > sx0) ? xEdge[x + 1] : sx0 + 1;

            unsigned sumR = 0, sumG = 0, sumB = 0;
            for (int yy = sy0; yy < sy1; ++yy) {
                const uint8_t* p = src + (size_t)yy * sp + (size_t)sx0 * 4; // B,G,R,A
                for (int xx = sx0; xx < sx1; ++xx, p += 4) {
                    sumB += p[0]; sumG += p[1]; sumR += p[2];
                }
            }
            const unsigned n = (unsigned)((sx1 - sx0) * (sy1 - sy0));
            uint8_t* q = drow + x * 3;
            q[0] = (uint8_t)(sumR / n);
            q[1] = (uint8_t)(sumG / n);
            q[2] = (uint8_t)(sumB / n);
        }
    }
}

void computeDirtyTilesRgb(const RgbFrame& cur, const RgbFrame& prev,
                          int tileW, int tileH, int tilesX, int tilesY,
                          bool keyframe, std::vector<uint8_t>& dirty) {
    dirty.assign((size_t)tilesX * tilesY, 0);
    if (keyframe || prev.w != cur.w || prev.h != cur.h) {
        std::fill(dirty.begin(), dirty.end(), (uint8_t)1);
        return;
    }
    const int stride = cur.stride();
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int x0 = tx * tileW * 3, y0 = ty * tileH;
            bool changed = false;
            for (int r = 0; r < tileH && !changed; ++r) {
                const uint8_t* ra = cur.rgb.data()  + (size_t)(y0 + r) * stride + x0;
                const uint8_t* rb = prev.rgb.data() + (size_t)(y0 + r) * stride + x0;
                changed = memcmp(ra, rb, (size_t)tileW * 3) != 0;
            }
            dirty[(size_t)ty * tilesX + tx] = changed ? 1 : 0;
        }
    }
}

size_t extractTileRgb(const RgbFrame& f, int tx, int ty, int tileW, int tileH,
                      std::vector<uint8_t>& out) {
    const size_t need = (size_t)tileW * tileH * 3;
    out.resize(need);
    const int x0 = tx * tileW * 3, y0 = ty * tileH;
    const int stride = f.stride();
    uint8_t* dst = out.data();
    for (int r = 0; r < tileH; ++r) {
        memcpy(dst, f.rgb.data() + (size_t)(y0 + r) * stride + x0, (size_t)tileW * 3);
        dst += tileW * 3;
    }
    return need;
}
