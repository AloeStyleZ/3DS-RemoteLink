#include "video.h"
#include "protocol.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

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

const uint8_t* VideoSender::encodeTile(size_t rawBytes, size_t& outBytes) {
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
                                         cfg_.jpegQuality, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        if (rc == 0) { outBytes = jsize; return jpegBuf_; }
        // fallback a RAW si fallara
    }
#endif
    outBytes = rawBytes;
    return tileBuf_.data();
}

void VideoSender::sendFrame(const YuvFrame& cur, bool forceKey) {
    bool keyframe = forceKey;
    if (++framesSinceKey_ >= cfg_.keyframeInterval) keyframe = true;

    computeDirtyTiles(cur, prev_, cfg_.tileW, cfg_.tileH, tilesX_, tilesY_, keyframe, dirty_);

    // Indices de tiles sucios.
    std::vector<int> tiles;
    tiles.reserve(dirty_.size());
    for (int i = 0; i < tilesX_ * tilesY_; ++i)
        if (dirty_[i]) tiles.push_back(i);

    const uint16_t fid = frameId_++;
    if (tiles.empty()) return;           // nada cambio: no enviamos nada
    if (keyframe) framesSinceKey_ = 0;

    VideoPacket pkt;
    for (size_t ti = 0; ti < tiles.size(); ++ti) {
        const int idx = tiles[ti];
        const int tx = idx % tilesX_, ty = idx / tilesX_;

        const size_t rawBytes = extractTile(cur, tx, ty, cfg_.tileW, cfg_.tileH, tileBuf_);
        size_t payloadBytes = 0;
        const uint8_t* data = encodeTile(rawBytes, payloadBytes);

        const int fragCount = (int)((payloadBytes + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD);
        for (int f = 0; f < fragCount; ++f) {
            const size_t off = (size_t)f * UDP_MAX_PAYLOAD;
            const size_t len = std::min((size_t)UDP_MAX_PAYLOAD, payloadBytes - off);

            pkt.hdr.magic = PROTO_MAGIC;
            pkt.hdr.type  = PKT_VIDEO;
            pkt.hdr.flags = 0;
            if (keyframe)                                pkt.hdr.flags |= VFLAG_KEYFRAME;
            if (ti == 0 && f == 0)                       pkt.hdr.flags |= VFLAG_FRAME_START;
            if (ti + 1 == tiles.size() && f + 1 == fragCount) pkt.hdr.flags |= VFLAG_FRAME_END;
            if (f + 1 == fragCount)                      pkt.hdr.flags |= VFLAG_TILE_LAST;
            pkt.hdr.codec      = cfg_.codec;
            pkt.hdr.frame_id   = fid;
            pkt.hdr.tile_id    = (uint16_t)idx;
            pkt.hdr.frag_index = (uint16_t)f;
            pkt.hdr.frag_count = (uint16_t)fragCount;
            pkt.hdr.payload_len= (uint16_t)len;
            pkt.hdr.tile_bytes = (uint16_t)payloadBytes;
            memcpy(pkt.payload, data + off, len);

            const int total = (int)sizeof(VideoPktHeader) + (int)len;
            sendto(sock_, (const char*)&pkt, total, 0, (sockaddr*)&dst_, sizeof(dst_));
        }
    }

    prev_ = cur; // referencia para el diff del proximo frame
}
