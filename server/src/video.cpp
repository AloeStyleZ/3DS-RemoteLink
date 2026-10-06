#include "video.h"
#include "protocol.h"
#include "etc1.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#define REFINE_QUALITY 88   // calidad alta a la que se refinan los tiles estaticos
#define REFINE_BUDGET  2    // tiles refinados por frame (acota el ancho de banda)

int g_udpPaceMs = 0;

VideoSender::~VideoSender() {
#ifdef HAVE_TURBOJPEG
    if (jpegBuf_) tjFree(jpegBuf_);
    if (tjh_)     tjDestroy((tjhandle)tjh_);
#endif
}

bool VideoSender::init(SOCKET sock, const sockaddr_in& dst, const VideoConfig& cfg) {
    sock_ = sock;
    dst_  = dst;
    cfg_  = cfg;
    tilesX_ = cfg.width  / cfg.tileW;
    tilesY_ = cfg.height / cfg.tileH;

    prev_.alloc(cfg.width, cfg.height);
    lastQ_.assign((size_t)tilesX_ * tilesY_, 0);
    refineCursor_ = 0;
    framesSinceKey_ = cfg.keyframeInterval; // fuerza keyframe en el primer frame

    // Subir el buffer de envio: un keyframe son ~100 datagramas en rafaga.
    int snd = 1 << 20; // 1 MB
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof(snd));

#ifdef HAVE_TURBOJPEG
    if (cfg_.codec == CODEC_JPEG_YCBCR) {
        tjh_ = tjInitCompress();
        jpegCap_ = tjBufSize(cfg_.tileW, cfg_.tileH, TJSAMP_420);
        jpegBuf_ = tjAlloc((int)jpegCap_);
        if (!tjh_ || !jpegBuf_) { fprintf(stderr, "[video] turbojpeg init fallo\n"); return false; }
    }
#else
    if (cfg_.codec == CODEC_JPEG_YCBCR) {
        fprintf(stderr, "[video] codec JPEG pedido pero build sin USE_TURBOJPEG; uso RAW\n");
        cfg_.codec = CODEC_RAW_YUV420;
    }
#endif

    if (cfg_.codec == CODEC_ETC1) {
        prevRgb_.alloc(cfg.width, cfg.height);
        const size_t maxEtc1Bytes = (size_t)(cfg.tileW / 4) * (cfg.tileH / 4) * ETC1_BLOCK_BYTES;
        etc1Buf_.resize(maxEtc1Bytes);

        // Primer frame: todos pendientes COMPLETOS (equivale al keyframe
        // inicial, pero el presupuesto por frame lo reparte sin rafaga).
        pendingEtc1_.assign((size_t)tilesX_ * tilesY_, 2);
        lastEtc1_.assign((size_t)tilesX_ * tilesY_ * maxEtc1Bytes, 0);
        tileEverSent_.assign((size_t)tilesX_ * tilesY_, 0);
        if (cfg_.fps < 1) cfg_.fps = 1;
        frameBudget_ = (size_t)cfg_.maxKbps * 1000 / 8 / cfg_.fps;
        if (frameBudget_ < maxEtc1Bytes) frameBudget_ = maxEtc1Bytes; // minimo 1 tile
    }
    return true;
}

void VideoSender::setFps(int f) {
    if (f < 1) f = 1;
    cfg_.fps = f;
    if (cfg_.codec == CODEC_ETC1) {
        const size_t maxEtc1Bytes = (size_t)(cfg_.tileW / 4) * (cfg_.tileH / 4) * ETC1_BLOCK_BYTES;
        frameBudget_ = (size_t)cfg_.maxKbps * 1000 / 8 / cfg_.fps;
        if (frameBudget_ < maxEtc1Bytes) frameBudget_ = maxEtc1Bytes; // minimo 1 tile
    }
}

VideoStats VideoSender::statsFetch() {
    VideoStats out = stats_;
    stats_ = VideoStats{};
    out.backlog = 0;
    for (uint8_t p : pendingEtc1_) out.backlog += p;
    return out;
}

const uint8_t* VideoSender::encodeTile(size_t rawBytes, int quality, size_t& outBytes) {
#ifdef HAVE_TURBOJPEG
    if (cfg_.codec == CODEC_JPEG_YCBCR) {
        const int cw = cfg_.tileW / 2, ch = cfg_.tileH / 2;
        const unsigned char* planes[3] = {
            tileBuf_.data(),
            tileBuf_.data() + (size_t)cfg_.tileW * cfg_.tileH,
            tileBuf_.data() + (size_t)cfg_.tileW * cfg_.tileH + (size_t)cw * ch
        };
        int strides[3] = { cfg_.tileW, cw, cw };
        unsigned long jsize = jpegCap_;
        int rc = tjCompressFromYUVPlanes((tjhandle)tjh_, planes, cfg_.tileW, strides,
                                         cfg_.tileH, TJSAMP_420, &jpegBuf_, &jsize,
                                         quality, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        if (rc == 0) { outBytes = jsize; return jpegBuf_; }
        // fallback a RAW si fallara
    }
#else
    (void)quality;
#endif
    outBytes = rawBytes;
    return tileBuf_.data();
}

// RLE (count,value) sobre el blob crudo del tile (Y|U|V concatenado). Aborta en
// cuanto el resultado supera `cap` (1/4 del tamano crudo): mas alla de eso el
// contenido ya no es "plano" y JPEG comprime mejor, asi que no compensa seguir.
size_t VideoSender::tryRle(size_t rawBytes) {
    const size_t cap = rawBytes / 4;
    if (rleBuf_.size() < cap) rleBuf_.resize(cap);
    const uint8_t* src = tileBuf_.data();
    size_t oi = 0, i = 0;
    while (i < rawBytes) {
        const uint8_t v = src[i];
        size_t run = 1;
        while (i + run < rawBytes && src[i + run] == v && run < 255) ++run;
        if (oi + 2 > cap) return 0;   // no compensa: contenido demasiado complejo
        rleBuf_[oi++] = (uint8_t)run;
        rleBuf_[oi++] = v;
        i += run;
    }
    return oi;
}

void VideoSender::sendFrame(const YuvFrame& cur, bool forceKey) {
    const int nTiles = tilesX_ * tilesY_;
    bool keyframe = forceKey;
    if (++framesSinceKey_ >= cfg_.keyframeInterval) keyframe = true;

    // Tiles realmente cambiados (sin forzar keyframe, para distinguir movimiento).
    computeDirtyTiles(cur, prev_, cfg_.tileW, cfg_.tileH, tilesX_, tilesY_, false, dirty_);

    const int  motionQ = cfg_.jpegQuality;
    const bool jpeg    = (cfg_.codec == CODEC_JPEG_YCBCR);

    // Trabajos a enviar este frame: (tile, calidad).
    std::vector<std::pair<int,int>> jobs;

    // lastQ_[i] guarda la "calidad" con la que se envio el tile la ultima vez;
    // 255 es el centinela de "RLE/lossless" (ver bucle de codificacion mas abajo,
    // que es quien escribe lastQ_ segun el codec REALMENTE usado). Aqui solo se
    // LEE para decidir la calidad de fallback JPEG, capada a REFINE_QUALITY para
    // no colar un 255 invalido como "quality" de libjpeg si el RLE fallara.
    if (keyframe) {
        // Reenvia todos los tiles (recuperacion), conservando la calidad refinada
        // de los estaticos para no causar un "pulso" de borroso cada keyframe.
        for (int i = 0; i < nTiles; ++i) {
            const int prevQ = std::min((int)lastQ_[i], REFINE_QUALITY);
            int q = (dirty_[i] || !jpeg) ? motionQ
                  : (lastQ_[i] ? std::max(motionQ, prevQ) : motionQ);
            jobs.emplace_back(i, q);
        }
        framesSinceKey_ = 0;
    } else {
        for (int i = 0; i < nTiles; ++i)
            if (dirty_[i]) jobs.emplace_back(i, motionQ);

        // Refinamiento progresivo: unos pocos tiles estaticos a alta calidad (solo JPEG).
        // Los tiles ya marcados como RLE (lastQ_==255) quedan fuera solos, porque
        // 255 no es < REFINE_QUALITY: son lossless, no necesitan refinarse.
        if (jpeg) {
            int budget = REFINE_BUDGET;
            for (int n = 0; n < nTiles && budget > 0; ++n) {
                int i = refineCursor_;
                refineCursor_ = (refineCursor_ + 1) % nTiles;
                if (!dirty_[i] && lastQ_[i] && lastQ_[i] < REFINE_QUALITY) {
                    jobs.emplace_back(i, REFINE_QUALITY);
                    --budget;
                }
            }
        }
    }

    const uint16_t fid = frameId_++;
    if (jobs.empty()) return;            // nada que enviar

    VideoPacket pkt;
    for (size_t ti = 0; ti < jobs.size(); ++ti) {
        const int idx = jobs[ti].first;
        const int q   = jobs[ti].second;
        const int tx = idx % tilesX_, ty = idx / tilesX_;

        const size_t rawBytes = extractTile(cur, tx, ty, cfg_.tileW, cfg_.tileH, tileBuf_);

        // Hibrido: RLE primero (casi gratis de decodificar) si el tile es "plano"
        // (UI/menus/texto); si no compensa (contenido complejo/texturizado), JPEG.
        size_t         payloadBytes = 0;
        const uint8_t* data = nullptr;
        uint8_t        usedCodec = cfg_.codec;
        if (jpeg) {
            const size_t rleBytes = tryRle(rawBytes);
            if (rleBytes > 0) { data = rleBuf_.data(); payloadBytes = rleBytes; usedCodec = CODEC_RLE_YUV420; }
        }
        if (!data) { data = encodeTile(rawBytes, q, payloadBytes); usedCodec = cfg_.codec; }
        lastQ_[idx] = (usedCodec == CODEC_RLE_YUV420) ? (uint8_t)255 : (uint8_t)q;

        const int fragCount = (int)((payloadBytes + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD);
        for (int f = 0; f < fragCount; ++f) {
            const size_t off = (size_t)f * UDP_MAX_PAYLOAD;
            const size_t len = std::min((size_t)UDP_MAX_PAYLOAD, payloadBytes - off);

            pkt.hdr.magic = PROTO_MAGIC;
            pkt.hdr.type  = PKT_VIDEO;
            pkt.hdr.flags = 0;
            if (keyframe)                                     pkt.hdr.flags |= VFLAG_KEYFRAME;
            if (ti == 0 && f == 0)                            pkt.hdr.flags |= VFLAG_FRAME_START;
            if (ti + 1 == jobs.size() && f + 1 == fragCount)  pkt.hdr.flags |= VFLAG_FRAME_END;
            if (f + 1 == fragCount)                           pkt.hdr.flags |= VFLAG_TILE_LAST;
            pkt.hdr.codec      = usedCodec;
            pkt.hdr.frame_id   = fid;
            pkt.hdr.tile_id    = (uint16_t)idx;
            pkt.hdr.frag_index = (uint16_t)f;
            pkt.hdr.frag_count = (uint16_t)fragCount;
            pkt.hdr.payload_len= (uint16_t)len;
            pkt.hdr.tile_bytes = (uint16_t)payloadBytes;
            memcpy(pkt.payload, data + off, len);

            const int pktLen = (int)sizeof(VideoPktHeader) + (int)len;
            sendto(sock_, (const char*)&pkt, pktLen, 0, (sockaddr*)&dst_, sizeof(dst_));
        }
    }

    prev_ = cur; // referencia para el diff del proximo frame
}

void VideoSender::sendFrameEtc1(const RgbFrame& cur, bool forceKey) {
    const int    tileCount = tilesX_ * tilesY_;
    const size_t tileBytes = (size_t)(cfg_.tileW / 4) * (cfg_.tileH / 4) * ETC1_BLOCK_BYTES;
    const int    nBlocks   = (int)(tileBytes / ETC1_BLOCK_BYTES);
    const int    mapBytes  = (nBlocks + 7) / 8;

    // 1) Marca pendientes. Nivel 1 = puede ir como DELTA; nivel 2 = COMPLETO
    // (primera vez / refresco rotativo / keyframe: reparan perdidas UDP).
    computeDirtyTilesRgb(cur, prevRgb_, cfg_.tileW, cfg_.tileH, tilesX_, tilesY_, false, dirtyRgb_);
    for (int i = 0; i < tileCount; ++i)
        if (dirtyRgb_[i] && !pendingEtc1_[i]) pendingEtc1_[i] = 1;
    if (forceKey)
        for (int i = 0; i < tileCount; ++i) pendingEtc1_[i] = 2;
    // Acumulador fraccional: exactamente tileCount tiles marcados cada
    // keyframeInterval frames (todos los tiles reparados una vez por segundo),
    // sin redondear hacia arriba (ceil enviaba hasta 2.5x mas trafico de
    // reparacion del necesario).
    rotAccum_ += tileCount;
    while (rotAccum_ >= (int)cfg_.keyframeInterval) {
        rotAccum_ -= (int)cfg_.keyframeInterval;
        pendingEtc1_[rotRefresh_] = 2;
        rotRefresh_ = (rotRefresh_ + 1) % tileCount;
    }

    // 2) Seleccion round-robin + codificacion + construccion de payloads
    // (delta o completo), hasta agotar el presupuesto de bytes del frame.
    struct Job { int idx; size_t off, bytes; bool delta; };
    std::vector<Job> jobs;
    if (sendScratch_.size() < (size_t)tileCount * (tileBytes + (size_t)mapBytes))
        sendScratch_.resize((size_t)tileCount * (tileBytes + (size_t)mapBytes));

    const auto tEnc0 = std::chrono::steady_clock::now();
    size_t spent = 0, scratchOff = 0;
    int lastSent = -1;
    for (int n = 0; n < tileCount; ++n) {
        const int idx = (sendCursor_ + n) % tileCount;
        if (!pendingEtc1_[idx]) continue;
        if (!jobs.empty() && spent >= frameBudget_) break;  // el resto, al proximo frame

        const int tx = idx % tilesX_, ty = idx / tilesX_;
        extractTileRgb(cur, tx, ty, cfg_.tileW, cfg_.tileH, tileBufRgb_);
        etc1EncodeTile(tileBufRgb_.data(), cfg_.tileW, cfg_.tileH, etc1Buf_.data());

        uint8_t* last = lastEtc1_.data() + (size_t)idx * tileBytes;
        uint8_t* out  = sendScratch_.data() + scratchOff;
        bool   delta = false;
        size_t payloadBytes = 0;

        if (pendingEtc1_[idx] == 1 && tileEverSent_[idx]) {
            // Delta por bloque: bitmap de bloques cambiados + solo esos bloques.
            // Un cursor o un HUD tocan ~20 bloques -> ~210 bytes en vez de 3200.
            memset(out, 0, (size_t)mapBytes);
            uint8_t* dst = out + mapBytes;
            int changed = 0;
            for (int b = 0; b < nBlocks; ++b) {
                const uint8_t* nb = etc1Buf_.data() + (size_t)b * ETC1_BLOCK_BYTES;
                if (memcmp(nb, last + (size_t)b * ETC1_BLOCK_BYTES, ETC1_BLOCK_BYTES) == 0)
                    continue;
                out[b >> 3] |= (uint8_t)(1u << (b & 7));
                memcpy(dst, nb, ETC1_BLOCK_BYTES);
                dst += ETC1_BLOCK_BYTES;
                ++changed;
            }
            if (changed == 0) {           // el diff RGB disparo, pero la codificacion
                pendingEtc1_[idx] = 0;    // ETC1 quedo identica: nada que enviar
                continue;
            }
            payloadBytes = (size_t)mapBytes + (size_t)changed * ETC1_BLOCK_BYTES;
            if (payloadBytes < tileBytes) delta = true;   // si no compensa, va completo
        }
        if (!delta) {
            memcpy(out, etc1Buf_.data(), tileBytes);
            payloadBytes = tileBytes;
        }
        memcpy(last, etc1Buf_.data(), tileBytes);   // nueva base "enviada"
        tileEverSent_[idx] = 1;
        pendingEtc1_[idx]  = 0;

        jobs.push_back({ idx, scratchOff, payloadBytes, delta });
        scratchOff += payloadBytes;
        spent      += payloadBytes;
        lastSent    = idx;
    }
    stats_.encodeMs += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - tEnc0).count();

    // Referencia = lo ultimo VISTO. Un tile que quedo pendiente no se pierde:
    // pendingEtc1_ lo mantiene marcado hasta que de verdad se envie.
    prevRgb_ = cur;
    if (jobs.empty()) return;
    sendCursor_ = (lastSent + 1) % tileCount;

    // 3) Transmision (fragmentada en datagramas <= UDP_MAX_PAYLOAD).
    const uint16_t fid = frameId_++;
    VideoPacket pkt;
    for (size_t ji = 0; ji < jobs.size(); ++ji) {
        const Job& j = jobs[ji];
        const uint8_t* data = sendScratch_.data() + j.off;
        const int fragCount = (int)((j.bytes + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD);
        for (int f = 0; f < fragCount; ++f) {
            const size_t off = (size_t)f * UDP_MAX_PAYLOAD;
            const size_t len = std::min((size_t)UDP_MAX_PAYLOAD, j.bytes - off);

            pkt.hdr.magic = PROTO_MAGIC;
            pkt.hdr.type  = PKT_VIDEO;
            pkt.hdr.flags = 0;
            if (forceKey)                                     pkt.hdr.flags |= VFLAG_KEYFRAME;
            if (ji == 0 && f == 0)                            pkt.hdr.flags |= VFLAG_FRAME_START;
            if (ji + 1 == jobs.size() && f + 1 == fragCount)  pkt.hdr.flags |= VFLAG_FRAME_END;
            if (f + 1 == fragCount)                           pkt.hdr.flags |= VFLAG_TILE_LAST;
            if (j.delta)                                      pkt.hdr.flags |= VFLAG_TILE_DELTA;
            pkt.hdr.codec      = CODEC_ETC1;
            pkt.hdr.frame_id   = fid;
            pkt.hdr.tile_id    = (uint16_t)j.idx;
            pkt.hdr.frag_index = (uint16_t)f;
            pkt.hdr.frag_count = (uint16_t)fragCount;
            pkt.hdr.payload_len= (uint16_t)len;
            pkt.hdr.tile_bytes = (uint16_t)j.bytes;
            memcpy(pkt.payload, data + off, len);

            const int total = (int)sizeof(VideoPktHeader) + (int)len;
            sendto(sock_, (const char*)&pkt, total, 0, (sockaddr*)&dst_, sizeof(dst_));
            if (g_udpPaceMs > 0) Sleep((DWORD)g_udpPaceMs);
        }
        stats_.bytes += j.bytes;
        stats_.tiles += 1;
    }
}
