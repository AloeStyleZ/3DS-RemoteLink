#include "etc1.h"
#include <algorithm>
#include <cstring>

// Tabla de moduladores de intensidad, verificada contra el decoder oficial de
// Ericsson (compressParams en etcdec.cxx). Las columnas ya estan reordenadas
// [+chico, +grande, -chico, -grande] para que el indice crudo de 2 bits
// (0,1,2,3) indexe DIRECTAMENTE esta tabla sin necesitar la tabla 'unscramble'
// del decoder (el resultado es identico: raw=0->+chico, 1->+grande, 2->-chico,
// 3->-grande, que es exactamente como el decoder interpreta esos mismos bits).
static const int MOD_TABLE[8][4] = {
    {  2,   8,   -2,   -8 },
    {  5,  17,   -5,  -17 },
    {  9,  29,   -9,  -29 },
    { 13,  42,  -13,  -42 },
    { 18,  60,  -18,  -60 },
    { 24,  80,  -24,  -80 },
    { 33, 106,  -33, -106 },
    { 47, 183,  -47, -183 },
};

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int clamp255(int v) { return clampi(v, 0, 255); }

// Cuantiza un valor 0..255 a 5 bits (0..31), redondeando al mas cercano.
static inline int quant5(int v) { return (v * 31 + 127) / 255; }
// Expande 5 bits a 8 (replica los 3 bits altos en los bajos): estandar en ETC1.
static inline int expand5(int v5) { return (v5 << 3) | (v5 >> 2); }

// Posicion (0..15) de un pixel (x,y) dentro del stream de indices de un bloque,
// segun el orden de iteracion "for x: for y" que usa cada sub-bloque (verificado
// contra decompressBlockDiffFlipC en etcdec.cxx). Depende de flip porque cambia
// la forma de las dos mitades (2x4 lado a lado, o 4x2 arriba/abajo).
static inline int pixelShift(int x, int y, int flip) {
    if (!flip) return x * 4 + y;                 // mitades verticales (x<2 / x>=2)
    const int half = y / 2, y2 = y % 2;           // mitades horizontales (y<2 / y>=2)
    return half * 8 + x * 2 + y2;
}
// A que sub-bloque (0 o 1) pertenece el pixel (x,y).
static inline int subblockOf(int x, int y, int flip) {
    return flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
}

struct Candidate {
    int r1, g1, b1;      // base sub-bloque 0, 5 bits
    int dr, dg, db;      // delta sub-bloque 1 (3 bits signed, ya recortado a rango valido)
    int cw[2];           // tabla de intensidad por sub-bloque
    uint8_t code[16];    // codigo crudo (0-3) por pixel, indexado por pixelShift
    int _flip;
    long long error;
};

// Ajusta dr para que r1+dr quede dentro de 0..31 y dr dentro de -4..3.
static inline int fitDelta(int base5, int desired5) {
    int dr = desired5 - base5;
    const int lo = std::max(-4, -base5);
    const int hi = std::min(3, 31 - base5);
    return clampi(dr, lo, hi);
}

// Prueba las 8 tablas de intensidad para un sub-bloque (8 pixeles) con una base
// RGB de 8 bits ya fija; devuelve la mejor tabla, sus 8 codigos y el error total.
static long long bestTableForSubblock(const uint8_t* rgb, int stride,
                                      int tileOx, int tileOy, int flip, int sub,
                                      const int base8[3], int* outCw, uint8_t outCode[16]) {
    long long bestErr = -1;
    int bestCw = 0;
    uint8_t bestCodes[8];

    for (int cw = 0; cw < 8; ++cw) {
        long long err = 0;
        uint8_t codes[8];
        int pi = 0;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                if (subblockOf(x, y, flip) != sub) continue;
                const uint8_t* p = rgb + (size_t)(tileOy + y) * stride + (size_t)(tileOx + x) * 3;
                long long bestPixErr = -1;
                int bestCode = 0;
                for (int code = 0; code < 4; ++code) {
                    const int m = MOD_TABLE[cw][code];
                    const int dr = clamp255(base8[0] + m) - p[0];
                    const int dg = clamp255(base8[1] + m) - p[1];
                    const int db = clamp255(base8[2] + m) - p[2];
                    const long long e = (long long)dr * dr + (long long)dg * dg + (long long)db * db;
                    if (bestPixErr < 0 || e < bestPixErr) { bestPixErr = e; bestCode = code; }
                }
                codes[pi++] = (uint8_t)bestCode;
                err += bestPixErr;
            }
        }
        if (bestErr < 0 || err < bestErr) {
            bestErr = err; bestCw = cw;
            memcpy(bestCodes, codes, sizeof(bestCodes));
        }
    }

    *outCw = bestCw;
    // Vuelca los 8 codigos ganadores a sus posiciones reales (indexadas por pixelShift).
    int pi = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            if (subblockOf(x, y, flip) == sub)
                outCode[pixelShift(x, y, flip)] = bestCodes[pi++];
    return bestErr;
}

// Codifica un unico bloque 4x4 (esquina superior-izq en tileOx,tileOy dentro de
// `rgb`, con `stride` bytes por fila) y escribe los 8 bytes en `out`.
static void encodeBlock(const uint8_t* rgb, int stride, int tileOx, int tileOy, uint8_t* out) {
    Candidate best{};
    best.error = -1;

    for (int flip = 0; flip < 2; ++flip) {
        // Promedio RGB de cada sub-bloque (siempre 8 pixeles, sea 2x4 o 4x2).
        long sum[2][3] = {{0,0,0},{0,0,0}};
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const int sub = subblockOf(x, y, flip);
                const uint8_t* p = rgb + (size_t)(tileOy + y) * stride + (size_t)(tileOx + x) * 3;
                sum[sub][0] += p[0]; sum[sub][1] += p[1]; sum[sub][2] += p[2];
            }
        }
        const int avg0[3] = { (int)(sum[0][0] / 8), (int)(sum[0][1] / 8), (int)(sum[0][2] / 8) };
        const int avg1[3] = { (int)(sum[1][0] / 8), (int)(sum[1][1] / 8), (int)(sum[1][2] / 8) };

        const int r1 = quant5(avg0[0]), g1 = quant5(avg0[1]), b1 = quant5(avg0[2]);
        const int dr = fitDelta(r1, quant5(avg1[0]));
        const int dg = fitDelta(g1, quant5(avg1[1]));
        const int db = fitDelta(b1, quant5(avg1[2]));

        const int base0_8[3] = { expand5(r1), expand5(g1), expand5(b1) };
        const int base1_8[3] = { expand5(r1 + dr), expand5(g1 + dg), expand5(b1 + db) };

        uint8_t codes[16];
        int cw0 = 0, cw1 = 0;
        long long err = 0;
        err += bestTableForSubblock(rgb, stride, tileOx, tileOy, flip, 0,
                                    base0_8, &cw0, codes);
        err += bestTableForSubblock(rgb, stride, tileOx, tileOy, flip, 1,
                                    base1_8, &cw1, codes);

        if (best.error < 0 || err < best.error) {
            best.error = err;
            best.r1 = r1; best.g1 = g1; best.b1 = b1;
            best.dr = dr; best.dg = dg; best.db = db;
            best.cw[0] = cw0; best.cw[1] = cw1;
            memcpy(best.code, codes, sizeof(codes));
            best._flip = flip;
        }
    }

    // --- Empaquetado (verificado contra etcdec.cxx / GETBITSHIGH) ---------
    // Palabra alta (bits 63-32): R1'(5)dR(3) G1'(5)dG(3) B1'(5)dB(3) cw1(3)cw2(3) diff(1) flip(1)
    uint32_t high = 0;
    high |= (uint32_t)(best.r1 & 0x1F) << 27;
    high |= (uint32_t)(best.dr & 0x7)  << 24;
    high |= (uint32_t)(best.g1 & 0x1F) << 19;
    high |= (uint32_t)(best.dg & 0x7)  << 16;
    high |= (uint32_t)(best.b1 & 0x1F) << 11;
    high |= (uint32_t)(best.db & 0x7)  << 8;
    high |= (uint32_t)(best.cw[0] & 0x7) << 5;
    high |= (uint32_t)(best.cw[1] & 0x7) << 2;
    high |= 1u << 1;                              // diffbit = 1 (modo diferencial)
    high |= (uint32_t)(best._flip & 1);           // flipbit

    // Palabra baja (bits 31-0): plano MSB (31-16) + plano LSB (15-0), un bit por pixel.
    uint32_t low = 0;
    for (int n = 0; n < 16; ++n) {
        const int code = best.code[n];
        const int msb = (code >> 1) & 1, lsb = code & 1;
        low |= (uint32_t)msb << (16 + n);
        low |= (uint32_t)lsb << n;
    }

    out[0] = (uint8_t)(high >> 24); out[1] = (uint8_t)(high >> 16);
    out[2] = (uint8_t)(high >> 8);  out[3] = (uint8_t)(high);
    out[4] = (uint8_t)(low >> 24);  out[5] = (uint8_t)(low >> 16);
    out[6] = (uint8_t)(low >> 8);   out[7] = (uint8_t)(low);
}

size_t etc1EncodeTile(const uint8_t* rgb, int tileW, int tileH, uint8_t* out) {
    const int stride = tileW * 3;
    size_t n = 0;
    for (int by = 0; by < tileH; by += 4) {
        for (int bx = 0; bx < tileW; bx += 4) {
            encodeBlock(rgb, stride, bx, by, out + n);
            n += ETC1_BLOCK_BYTES;
        }
    }
    return n;
}
