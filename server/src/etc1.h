// Encoder ETC1 (modo diferencial). Cada bloque de 4x4 pixeles se codifica a 8
// bytes que la GPU del 3DS (PICA200) descomprime por hardware al muestrear la
// textura -> coste de CPU CERO en el cliente (a diferencia de JPEG/RLE).
//
// Bit layout verificado contra el decoder de referencia oficial de Ericsson
// (github.com/Ericsson/ETCPACK, source/etcdec.cxx) via WebFetch: posiciones de
// diffbit/flipbit/colores/tablas, tabla de moduladores de intensidad, y el
// mapeo (indice crudo 2 bits -> modulador) via el array 'unscramble'.
#pragma once
#include <cstdint>
#include <cstddef>
#include "protocol.h"   // ETC1_BLOCK_BYTES

// Codifica un bloque de tileW x tileH pixeles RGB888 (stride = tileW*3, sin
// padding) en bloques ETC1 de 4x4, escritos secuencialmente y en ORDEN DE
// LECTURA (bloque (0,0), (1,0), (2,0)... fila a fila) en `out`. tileW y tileH
// deben ser multiplos de 4. Devuelve el numero de bytes escritos
// ((tileW/4)*(tileH/4)*8).
size_t etc1EncodeTile(const uint8_t* rgb, int tileW, int tileH, uint8_t* out);

// Diagnostico: si es true, fuerza cw=0 y codigo=0 (mismo modulador) en TODOS
// los pixeles de TODOS los bloques, saltandose la busqueda normal. Sirve para
// aislar si un artefacto (p.ej. franjas verticales en tiles solidos) depende
// de que distintos pixeles usen distintos codigos, o pasa igual con codigo fijo.
extern bool g_etc1ForceFlatCode;
