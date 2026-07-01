// Servidor de streaming PC -> 3DS.
//  1) Handshake TCP (puerto 7999): negocia resolucion/codec/puertos.
//  2) Hilo de input: recibe gamepad del 3DS (UDP 8001) -> ViGEm.
//  3) Loop principal: captura DXGI -> I420 -> diff de tiles -> envio UDP (8000).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "protocol.h"
#include "capture.h"
#include "convert.h"
#include "video.h"
#include "input.h"
#include "audio.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

static std::atomic<bool> g_running{true};

static BOOL WINAPI ctrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

// Recibe exactamente n bytes de un socket TCP. false si se cierra/error.
static bool recvAll(SOCKET s, void* buf, int n) {
    char* p = (char*)buf;
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}
static bool sendAll(SOCKET s, const void* buf, int n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        int r = send(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

// Bloquea hasta que un 3DS conecta y completa el handshake.
// Rellena clientAddr (IP del 3DS) y el ServerHello acordado.
static bool doHandshake(int outputIndex, bool wantJpeg, int fps, int quality,
                        sockaddr_in& clientAddr, ServerHello& sh) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT_HANDSHAKE);
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, 1) == SOCKET_ERROR) {
        fprintf(stderr, "[hs] bind/listen %u fallo (%d)\n", PORT_HANDSHAKE, WSAGetLastError());
        closesocket(ls);
        return false;
    }
    fprintf(stderr, "[hs] esperando 3DS en TCP %u...\n", PORT_HANDSHAKE);

    int clen = sizeof(clientAddr);
    SOCKET cs = accept(ls, (sockaddr*)&clientAddr, &clen);
    closesocket(ls);
    if (cs == INVALID_SOCKET) return false;

    char ipstr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipstr, sizeof(ipstr));

    ClientHello ch{};
    if (!recvAll(cs, &ch, sizeof(ch)) || ch.magic != PROTO_MAGIC) {
        fprintf(stderr, "[hs] ClientHello invalido\n");
        closesocket(cs);
        return false;
    }
    fprintf(stderr, "[hs] 3DS %s pide %ux%u caps=0x%X video_port=%u input_port=%u\n",
            ipstr, ch.want_width, ch.want_height, ch.codec_caps,
            ch.udp_video_port, ch.udp_input_port);

    // Negociacion: resolucion del cliente si es valida, si no defaults.
    int w = ch.want_width, h = ch.want_height;
    if (w <= 0 || h <= 0 || w % TILE_DEFAULT_W || h % TILE_DEFAULT_H) {
        w = VIDEO_DEFAULT_W; h = VIDEO_DEFAULT_H;
    }
    uint8_t codec = CODEC_RAW_YUV420;
    if (wantJpeg && (ch.codec_caps & (1u << 1))) codec = CODEC_JPEG_YCBCR;

    memset(&sh, 0, sizeof(sh));
    sh.magic            = PROTO_MAGIC;
    sh.version          = PROTO_VERSION;
    sh.codec            = codec;
    sh.jpeg_quality     = (uint8_t)quality;
    sh.width            = (uint16_t)w;
    sh.height           = (uint16_t)h;
    sh.tile_w           = TILE_DEFAULT_W;
    sh.tile_h           = TILE_DEFAULT_H;
    sh.tiles_x          = (uint16_t)(w / TILE_DEFAULT_W);
    sh.tiles_y          = (uint16_t)(h / TILE_DEFAULT_H);
    sh.fps              = (uint8_t)fps;
    sh.keyframe_interval= (uint8_t)fps;   // un keyframe por segundo
    sh.udp_video_port   = ch.udp_video_port ? ch.udp_video_port : PORT_VIDEO;
    sh.udp_input_port   = PORT_INPUT;

    bool ok = sendAll(cs, &sh, sizeof(sh));
    closesocket(cs);
    if (!ok) { fprintf(stderr, "[hs] envio ServerHello fallo\n"); return false; }

    // Destino UDP del video = IP del 3DS : puerto que indico.
    clientAddr.sin_port = htons(sh.udp_video_port);
    fprintf(stderr, "[hs] acordado %ux%u codec=%u -> video a %s:%u\n",
            w, h, codec, ipstr, sh.udp_video_port);
    return true;
}

// Forma del puntero (1 = relleno). Punta (vertice principal) arriba-izquierda en (0,0).
// Cabeza triangular inclinada + cola hacia abajo-derecha, como un cursor clasico.
static const char* CURSOR_MASK[] = {
    "1..........",
    "11.........",
    "111........",
    "1111.......",
    "11111......",
    "111111.....",
    "1111111....",
    "11111111...",
    "111111111..",
    "1111111111.",
    ".....1111..",
    "......1111.",
    ".......1111",
    "........111",
    ".........11",
};

// Dibuja el cursor (flecha blanca con borde negro) en el frame YUV. DXGI no
// captura el cursor, asi que lo componemos donde esta el raton (GetCursorPos).
// El borde se genera solo: un pixel relleno es borde si algun vecino no lo esta.
static void drawCursorMarker(YuvFrame& f, int cx, int cy) {
    const int cs = f.w / 2;
    const int H  = (int)(sizeof(CURSOR_MASK) / sizeof(CURSOR_MASK[0]));
    for (int ry = 0; ry < H; ++ry) {
        const char* row = CURSOR_MASK[ry];
        const int W = (int)strlen(row);
        for (int rx = 0; rx < W; ++rx) {
            if (row[rx] != '1') continue;
            bool edge = false;
            if (rx == 0     || row[rx - 1] != '1') edge = true;
            if (rx == W - 1 || row[rx + 1] != '1') edge = true;
            if (ry == 0) edge = true;
            else { const char* up = CURSOR_MASK[ry - 1]; if (rx >= (int)strlen(up) || up[rx] != '1') edge = true; }
            if (ry == H - 1) edge = true;
            else { const char* dn = CURSOR_MASK[ry + 1]; if (rx >= (int)strlen(dn) || dn[rx] != '1') edge = true; }

            const int x = cx + rx, y = cy + ry;
            if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
            f.y[(size_t)y * f.w + x] = edge ? 16 : 235;   // borde negro / relleno blanco
            const size_t ci = (size_t)(y / 2) * cs + (x / 2);
            f.u[ci] = 128; f.v[ci] = 128;                  // gris neutro (blanco/negro)
        }
    }
}

int main(int argc, char** argv) {
    int  outputIndex = 0;
    bool wantJpeg = false;
    int  fps = 30;
    int  quality = 55;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--jpeg")) wantJpeg = true;
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) outputIndex = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = atoi(argv[++i]);
    }
    if (fps < 5)  fps = 5;
    if (fps > 60) fps = 60;
    if (quality < 10)  quality = 10;
    if (quality > 95)  quality = 95;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup fallo\n"); return 1; }
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Socket UDP de video (solo envio) y servicios persistentes.
    SOCKET videoSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (videoSock == INVALID_SOCKET) { fprintf(stderr, "socket video fallo\n"); WSACleanup(); return 1; }

    InputServer input;
    input.start(PORT_INPUT);

    DesktopCapture capture;
    if (!capture.init(outputIndex)) {
        fprintf(stderr, "[main] captura DXGI fallo\n");
        input.stop(); closesocket(videoSock); WSACleanup();
        return 1;
    }

    AudioSender audio;
    bool     audioStarted = false;
    YuvFrame cur;

    // Bucle de conexiones: tras un cliente, vuelve a aceptar (soporta reconexion).
    while (g_running) {
        sockaddr_in clientAddr{};
        ServerHello sh{};
        if (!doHandshake(outputIndex, wantJpeg, fps, quality, clientAddr, sh)) {
            if (!g_running) break;
            continue;                    // reintenta aceptar
        }

        VideoConfig cfg;
        cfg.width  = sh.width;  cfg.height = sh.height;
        cfg.tileW  = sh.tile_w; cfg.tileH  = sh.tile_h;
        cfg.codec  = sh.codec;  cfg.keyframeInterval = sh.keyframe_interval;
        cfg.jpegQuality = sh.jpeg_quality;
        cur.alloc(cfg.width, cfg.height);

        VideoSender sender;
        if (!sender.init(videoSock, clientAddr, cfg)) continue;

        if (!audioStarted) {
            sockaddr_in audioDst = clientAddr;
            audioDst.sin_port = htons(PORT_AUDIO);
            audio.start(audioDst);
            audioStarted = true;
        }

        input.markActive();              // el cliente acaba de conectar
        auto frameDur = std::chrono::milliseconds(1000 / sh.fps);
        int  curQuality = sh.jpeg_quality, curFps = sh.fps;
        uint64_t frames = 0;
        auto statT = std::chrono::steady_clock::now();
        fprintf(stderr, "[main] streaming a %d fps.\n", sh.fps);

        // Stream hasta que el cliente deje de mandar input (se fue) o Ctrl+C.
        // 8s de tolerancia: el teclado en pantalla (swkbd) bloquea el input del
        // cliente mientras se escribe; no debe contar como desconexion.
        while (g_running && input.clientActive(8000)) {
            auto t0 = std::chrono::steady_clock::now();
            audio.setEnabled(input.audioEnabled());

            int rq = input.qualityReq();
            if (rq && rq != curQuality) { sender.setQuality(rq); curQuality = rq;
                fprintf(stderr, "[main] calidad -> %d\n", rq); }
            int rf = input.fpsReq();
            if (rf && rf != curFps) { curFps = rf; frameDur = std::chrono::milliseconds(1000 / rf);
                sender.setKeyframeInterval(rf); fprintf(stderr, "[main] fps -> %d\n", rf); }

            CapturedFrame f;
            if (capture.capture(f, (uint32_t)frameDur.count())) {
                if (f.valid) {
                    bgraToI420Scaled(f.bgra, f.width, f.height, f.rowPitch, cur);
                    capture.endFrame();
                    if (input.isDesktop()) {
                        POINT pt;
                        if (GetCursorPos(&pt)) {
                            int dw = capture.desktopWidth(), dh = capture.desktopHeight();
                            if (dw > 0 && dh > 0) {
                                int sx = (int)((long long)pt.x * cfg.width  / dw);
                                int sy = (int)((long long)pt.y * cfg.height / dh);
                                if (sx < 0) sx = 0; else if (sx >= cfg.width)  sx = cfg.width  - 1;
                                if (sy < 0) sy = 0; else if (sy >= cfg.height) sy = cfg.height - 1;
                                drawCursorMarker(cur, sx, sy);
                            }
                        }
                    }
                    sender.sendFrame(cur, input.takeKeyframeReq());
                    ++frames;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            auto elapsed = std::chrono::steady_clock::now() - t0;
            if (elapsed < frameDur) std::this_thread::sleep_for(frameDur - elapsed);

            auto now = std::chrono::steady_clock::now();
            if (now - statT >= std::chrono::seconds(2)) {
                fprintf(stderr, "[main] %.1f fps enviados\n",
                        frames / std::chrono::duration<double>(now - statT).count());
                frames = 0; statT = now;
            }
        }
        fprintf(stderr, "[main] cliente inactivo -> esperando nuevo handshake...\n");
    }

    fprintf(stderr, "\n[main] cerrando...\n");
    audio.stop();
    input.stop();
    closesocket(videoSock);
    WSACleanup();
    return 0;
}
