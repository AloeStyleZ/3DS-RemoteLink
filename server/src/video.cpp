#include "video.h"
#include "protocol.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <utility>
#include <vector>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#define REFINE_QUALITY 88   // calidad alta a la que se refinan los tiles estaticos
#define REFINE_BUDGET  2    // tiles refinados por frame (acota el ancho de banda)

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
    return true;
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
