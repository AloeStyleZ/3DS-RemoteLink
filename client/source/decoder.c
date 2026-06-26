#include "decoder.h"
#include <string.h>
#include <malloc.h>
#include <turbojpeg.h>

static u16 next_pow2(u16 v) {
    u16 p = 8;
    while (p < v) p <<= 1;
    return p;
}

bool video_init(VideoDecoder* d, int w, int h, int tileW, int tileH) {
    memset(d, 0, sizeof(*d));
    d->width = w;  d->height = h;
    d->tileW = tileW; d->tileH = tileH;
    d->tilesX = w / tileW; d->tilesY = h / tileH;
    d->tileCount = d->tilesX * d->tilesY;
    d->ySize = (size_t)w * h;
    d->cSize = (size_t)(w / 2) * (h / 2);
    d->maxTileBytes = tileW * tileH + 2 * (tileW / 2) * (tileH / 2);

    // Planos I420 en memoria lineal (requerido por el DMA del Y2R).
    d->y = (u8*)linearAlloc(d->ySize);
    d->u = (u8*)linearAlloc(d->cSize);
    d->v = (u8*)linearAlloc(d->cSize);
    if (!d->y || !d->u || !d->v) return false;
    memset(d->y, 16,  d->ySize);   // negro en YUV: Y=16, U=V=128
    memset(d->u, 128, d->cSize);
    memset(d->v, 128, d->cSize);

    d->reasm = (TileReasm*)calloc(d->tileCount, sizeof(TileReasm));
    if (!d->reasm) return false;
    for (int i = 0; i < d->tileCount; ++i) {
        d->reasm[i].frameId = 0xFFFF;
        d->reasm[i].buf = (u8*)malloc(d->maxTileBytes);
        if (!d->reasm[i].buf) return false;
    }

    // Textura POT (p.ej. 512x256) que contiene la imagen en su esquina sup-izq.
    u16 tw = next_pow2((u16)w), th = next_pow2((u16)h);
    if (!C3D_TexInit(&d->tex, tw, th, GPU_RGB565)) return false;
    C3D_TexSetFilter(&d->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&d->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(d->tex.data, 0, d->tex.size);

    d->subtex.width  = (u16)w;
    d->subtex.height = (u16)h;
    d->subtex.left   = 0.0f;
    d->subtex.top    = 1.0f;
    d->subtex.right  = (float)w / tw;
    d->subtex.bottom = 1.0f - (float)h / th;
    d->img.tex    = &d->tex;
    d->img.subtex = &d->subtex;

    // --- Y2R: YUV420 planar (8b) -> RGB565, salida tiled (BLOCK_8_BY_8) ---
    if (R_FAILED(y2rInit())) return false;
    Y2RU_SetInputFormat(INPUT_YUV420_INDIV_8);
    Y2RU_SetOutputFormat(OUTPUT_RGB_16_565);
    Y2RU_SetRotation(ROTATION_NONE);
    Y2RU_SetBlockAlignment(BLOCK_8_BY_8);
    Y2RU_SetTransferEndInterrupt(true);
    Y2RU_SetInputLineWidth((u16)w);
    Y2RU_SetInputLines((u16)h);
    // El servidor genera Y en rango estudio (16-235, formula BT.601 con +16), asi
    // que usamos la variante _SCALING que expande 16-235 -> 0-255. Si los colores
    // salen lavados/saturados, prueba COEFFICIENT_ITU_R_BT_601 (sin scaling).
    Y2RU_SetStandardCoefficient(COEFFICIENT_ITU_R_BT_601_SCALING);
    Y2RU_SetAlpha(0xFF);
    Y2RU_GetTransferEndEvent(&d->y2rEvent);

    // Decoder JPEG (para codec CODEC_JPEG_YCBCR).
    d->tjDec = tjInitDecompress();
    if (!d->tjDec) return false;
    return true;
}

void video_exit(VideoDecoder* d) {
    if (d->tjDec) tjDestroy((tjhandle)d->tjDec);
    y2rExit();
    C3D_TexDelete(&d->tex);
    if (d->reasm) {
        for (int i = 0; i < d->tileCount; ++i) free(d->reasm[i].buf);
        free(d->reasm);
    }
    if (d->y) linearFree(d->y);
    if (d->u) linearFree(d->u);
    if (d->v) linearFree(d->v);
}

// Desempaqueta un tile RAW (Y|U|V contiguo) en los planos persistentes.
static void unpack_tile_raw(VideoDecoder* d, int tx, int ty, const u8* blob) {
    const int tW = d->tileW, tH = d->tileH, cW = tW / 2, cH = tH / 2;
    const int cStride = d->width / 2;
    const u8* src = blob;

    const int yx = tx * tW, yy = ty * tH;
    for (int r = 0; r < tH; ++r) {
        memcpy(d->y + (size_t)(yy + r) * d->width + yx, src, tW);
        src += tW;
    }
    const int cx = tx * cW, cy = ty * cH;
    for (int r = 0; r < cH; ++r) {
        memcpy(d->u + (size_t)(cy + r) * cStride + cx, src, cW);
        src += cW;
    }
    for (int r = 0; r < cH; ++r) {
        memcpy(d->v + (size_t)(cy + r) * cStride + cx, src, cW);
        src += cW;
    }
}

// Decodifica un tile JPEG (4:2:0) DIRECTAMENTE en los planos persistentes, en la
// posicion del tile. Reusa la API simetrica a la del servidor (tjCompressFromYUVPlanes).
static void decode_tile_jpeg(VideoDecoder* d, int tx, int ty,
                             const u8* jpeg, unsigned long jpegSize) {
    const int cStride = d->width / 2;
    const int cw = d->tileW / 2, ch = d->tileH / 2;
    unsigned char* planes[3] = {
        d->y + (size_t)(ty * d->tileH) * d->width + (size_t)(tx * d->tileW),
        d->u + (size_t)(ty * ch) * cStride + (size_t)(tx * cw),
        d->v + (size_t)(ty * ch) * cStride + (size_t)(tx * cw)
    };
    int strides[3] = { d->width, cStride, cStride };
    tjDecompressToYUVPlanes((tjhandle)d->tjDec, jpeg, jpegSize, planes,
                            d->tileW, strides, d->tileH, TJFLAG_FASTDCT);
}

// Convierte los planos I420 -> textura RGB565 mediante el HW Y2R.
static void video_present(VideoDecoder* d) {
    // El Y2R lee por DMA: hay que volcar la cache CPU a RAM antes.
    GSPGPU_FlushDataCache(d->y, d->ySize);
    GSPGPU_FlushDataCache(d->u, d->cSize);
    GSPGPU_FlushDataCache(d->v, d->cSize);

    Y2RU_SetSendingY(d->y, d->ySize, (s16)d->width,       0);
    Y2RU_SetSendingU(d->u, d->cSize, (s16)(d->width / 2), 0);
    Y2RU_SetSendingV(d->v, d->cSize, (s16)(d->width / 2), 0);

    // Recepcion directa en la textura tiled. La imagen es d->width de ancho pero
    // la textura es POT (tex.width). Tras cada fila-de-tiles (8 lineas) saltamos
    // el padding hasta la siguiente fila-de-tiles del POT.
    //   unit = bytes de 1 fila-de-tiles de la imagen   = width * 8 * 2
    //   gap  = padding hasta la fila-de-tiles del POT   = (texW - width) * 8 * 2
    // (Si la imagen sale descuadrada, estos dos valores son lo primero a revisar.)
    const int unit = d->width * 8 * 2;
    const int gap  = (d->tex.width - d->width) * 8 * 2;
    Y2RU_SetReceiving(d->tex.data, (u32)(d->width * d->height * 2), (s16)unit, (s16)gap);

    Y2RU_StartConversion();
    svcWaitSynchronization(d->y2rEvent, 1000000000LL); // timeout 1s
    svcClearEvent(d->y2rEvent);

    // El Y2R escribio la textura por DMA; invalidamos la cache CPU para que
    // lineas obsoletas no la pisen cuando la GPU la muestree.
    GSPGPU_InvalidateDataCache(d->tex.data, d->tex.size);
    d->ready = true;
}

bool video_on_packet(VideoDecoder* d, const u8* pkt, int len) {
    if (len < (int)sizeof(VideoPktHeader)) return false;

    VideoPktHeader h;
    memcpy(&h, pkt, sizeof(h)); // copia para evitar accesos desalineados
    if (h.magic != PROTO_MAGIC || h.type != PKT_VIDEO) return false;
    if (h.tile_id >= d->tileCount)        return false;
    if (h.codec != CODEC_RAW_YUV420 && h.codec != CODEC_JPEG_YCBCR) return false;
    if (h.frag_index >= MAX_FRAGS_PER_TILE) return false;

    const u8* payload = pkt + sizeof(VideoPktHeader);
    const int payAvail = len - (int)sizeof(VideoPktHeader);
    if (payAvail < (int)h.payload_len) return false;

    TileReasm* r = &d->reasm[h.tile_id];
    if (r->frameId != h.frame_id) {
        // Nuevo frame para este tile: reinicia su reensamblado.
        r->frameId   = h.frame_id;
        r->fragCount = h.frag_count;
        r->received  = 0;
        r->fragMask  = 0;
        r->tileBytes = h.tile_bytes;
    }

    if (!(r->fragMask & (1u << h.frag_index))) {
        const size_t off = (size_t)h.frag_index * UDP_MAX_PAYLOAD;
        if (off + h.payload_len <= (size_t)d->maxTileBytes) {
            memcpy(r->buf + off, payload, h.payload_len);
            r->fragMask |= (1u << h.frag_index);
            r->received++;

            if (r->received == r->fragCount &&
                r->tileBytes <= (u16)d->maxTileBytes) {
                const int tx = h.tile_id % d->tilesX;
                const int ty = h.tile_id / d->tilesX;
                if (h.codec == CODEC_JPEG_YCBCR)
                    decode_tile_jpeg(d, tx, ty, r->buf, r->tileBytes);
                else
                    unpack_tile_raw(d, tx, ty, r->buf);
                // Hay datos nuevos -> presentar. NO esperamos al FRAME_END (que
                // se pierde a menudo en las rafagas de movimiento y causa los
                // congelados). Mostramos lo ultimo recibido cada iteracion.
                d->pendingPresent = true;
            }
        }
    }

    if (h.flags & VFLAG_FRAME_END) {
        // No hacemos Y2R aqui: solo marcamos. El bucle principal hara UN solo
        // present por iteracion sobre el frame mas reciente (salta intermedios).
        d->pendingPresent = true;
        return true;
    }
    return false;
}

void video_update(VideoDecoder* d) {
    if (d->pendingPresent) {
        video_present(d);
        d->pendingPresent = false;
    }
}

void video_draw(VideoDecoder* d, float x, float y) {
    if (!d->ready) return;
    // Escala para llenar la pantalla superior (400x240) sea cual sea la
    // resolucion del stream (la imagen nativa mide d->width x d->height).
    const float sx = 400.0f / (float)d->width;
    const float sy = 240.0f / (float)d->height;
    C2D_DrawImageAt(d->img, x, y, 0.0f, NULL, sx, sy);
}
