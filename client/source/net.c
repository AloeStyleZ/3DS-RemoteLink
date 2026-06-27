#include "net.h"
#include "decoder.h"
#include "config.h"
#include "audio.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>

#define SOC_ALIGN    0x1000
#define SOC_BUFSIZE  0x100000   // 1 MB para el stack de red

static u32* s_socBuf   = NULL;
static int  s_videoSock = -1;
static int  s_inputSock = -1;
static int  s_audioSock = -1;
static struct sockaddr_in s_inputAddr;

bool net_init(void) {
    s_socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFSIZE);
    if (!s_socBuf) return false;
    if (socInit(s_socBuf, SOC_BUFSIZE) != 0) {
        free(s_socBuf); s_socBuf = NULL;
        return false;
    }
    return true;
}

void net_exit(void) {
    if (s_videoSock >= 0) { close(s_videoSock); s_videoSock = -1; }
    if (s_inputSock >= 0) { close(s_inputSock); s_inputSock = -1; }
    if (s_audioSock >= 0) { close(s_audioSock); s_audioSock = -1; }
    socExit();
    if (s_socBuf) { free(s_socBuf); s_socBuf = NULL; }
}

// Recibe exactamente n bytes de un socket TCP.
static bool recvAll(int fd, void* buf, int n) {
    u8* p = (u8*)buf;
    while (n > 0) {
        int r = recv(fd, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

bool net_handshake(const char* server_ip, ServerHello* out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT_HANDSHAKE);
    addr.sin_addr.s_addr = inet_addr(server_ip);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    ClientHello ch;
    memset(&ch, 0, sizeof(ch));
    ch.magic          = PROTO_MAGIC;
    ch.version        = PROTO_VERSION;
    ch.want_width     = STREAM_WIDTH;
    ch.want_height    = STREAM_HEIGHT;
    ch.codec_caps     = (1u << 0) | (1u << 1); // soporta RAW YUV420 y JPEG
    ch.udp_video_port = PORT_VIDEO;
    ch.udp_input_port = PORT_INPUT;
    ch.max_kbps       = 6000;

    if (send(fd, &ch, sizeof(ch), 0) != (int)sizeof(ch)) { close(fd); return false; }

    bool ok = recvAll(fd, out, sizeof(ServerHello));
    close(fd);
    return ok && out->magic == PROTO_MAGIC;
}

bool net_open_streams(const char* server_ip, const ServerHello* sh) {
    // --- video: UDP de recepcion, bind al puerto que acordamos ---
    s_videoSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_videoSock < 0) return false;

    struct sockaddr_in vaddr;
    memset(&vaddr, 0, sizeof(vaddr));
    vaddr.sin_family = AF_INET;
    vaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    vaddr.sin_port = htons(sh->udp_video_port ? sh->udp_video_port : PORT_VIDEO);
    if (bind(s_videoSock, (struct sockaddr*)&vaddr, sizeof(vaddr)) != 0) return false;

    int rcv = 256 * 1024; // amortiguar rafagas de keyframe
    setsockopt(s_videoSock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

    // no bloqueante: drenamos en cada frame hasta EWOULDBLOCK
    int fl = fcntl(s_videoSock, F_GETFL, 0);
    fcntl(s_videoSock, F_SETFL, fl | O_NONBLOCK);

    // --- input: UDP de envio hacia el servidor ---
    s_inputSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_inputSock < 0) return false;
    memset(&s_inputAddr, 0, sizeof(s_inputAddr));
    s_inputAddr.sin_family = AF_INET;
    s_inputAddr.sin_addr.s_addr = inet_addr(server_ip);
    s_inputAddr.sin_port = htons(sh->udp_input_port ? sh->udp_input_port : PORT_INPUT);

    // --- audio: UDP de recepcion (PC -> 3DS), no bloqueante ---
    s_audioSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_audioSock >= 0) {
        struct sockaddr_in aaddr;
        memset(&aaddr, 0, sizeof(aaddr));
        aaddr.sin_family = AF_INET;
        aaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        aaddr.sin_port = htons(PORT_AUDIO);
        if (bind(s_audioSock, (struct sockaddr*)&aaddr, sizeof(aaddr)) == 0) {
            int rcv = 128 * 1024;
            setsockopt(s_audioSock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
            int fl = fcntl(s_audioSock, F_GETFL, 0);
            fcntl(s_audioSock, F_SETFL, fl | O_NONBLOCK);
        } else {
            close(s_audioSock); s_audioSock = -1;
        }
    }

    return true;
}

void net_send_input(const InputPacket* p) {
    if (s_inputSock < 0) return;
    sendto(s_inputSock, p, sizeof(*p), 0,
           (struct sockaddr*)&s_inputAddr, sizeof(s_inputAddr));
}

bool net_drain_video(struct VideoDecoder* dec) {
    if (s_videoSock < 0) return false;
    static u8 buf[1600];
    bool frameDone = false;
    // Cota de paquetes por llamada: garantiza que el bucle SIEMPRE retorne
    // (aunque lleguen mas rapido de lo que se procesan) para que el render y el
    // envio de input no se queden bloqueados. Permite recuperar ~14 frames/iter.
    int budget = 1500;
    while (budget-- > 0) {
        int n = recvfrom(s_videoSock, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) break;             // EWOULDBLOCK / nada mas que leer
        if (video_on_packet(dec, buf, n)) frameDone = true;
    }
    return frameDone;
}

void net_drain_audio(void) {
    if (s_audioSock < 0) return;
    static u8 buf[2048];
    for (int i = 0; i < 64; ++i) {     // cota por iteracion
        int n = recvfrom(s_audioSock, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) break;
        if (n < (int)sizeof(AudioPacketHeader)) continue;
        AudioPacketHeader h;
        memcpy(&h, buf, sizeof(h));
        if (h.magic != PROTO_MAGIC || h.type != PKT_AUDIO) continue;
        int avail = (n - (int)sizeof(AudioPacketHeader)) / 2;
        int samples = h.samples < avail ? h.samples : avail;
        audio_feed((const s16*)(buf + sizeof(AudioPacketHeader)), samples, h.rate);
    }
}
