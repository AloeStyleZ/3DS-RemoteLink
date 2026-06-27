#include "input.h"
#include "protocol.h"
#include <ws2tcpip.h>
#include <windows.h>   // SendInput (raton en modo escritorio)
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifdef HAVE_VIGEM
#include <ViGEm/Client.h>
#endif

InputServer::~InputServer() {
    stop();
}

bool InputServer::start(uint16_t port) {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) { fprintf(stderr, "[input] socket fallo\n"); return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[input] bind(%u) fallo (%d)\n", port, WSAGetLastError());
        closesocket(sock_); sock_ = INVALID_SOCKET;
        return false;
    }

    // Timeout de recepcion para poder salir limpio del hilo.
    DWORD to = 200;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));

#ifdef HAVE_VIGEM
    if (!vigemInit()) fprintf(stderr, "[input] ViGEm no disponible (driver instalado?)\n");
#endif

    running_ = true;
    thread_ = std::thread(&InputServer::runLoop, this);
    fprintf(stderr, "[input] escuchando en UDP %u\n", port);
    return true;
}

void InputServer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
#ifdef HAVE_VIGEM
    vigemShutdown();
#endif
}

void InputServer::runLoop() {
    char buf[128];
    while (running_) {
        sockaddr_in from{}; int fromLen = sizeof(from);
        int n = recvfrom(sock_, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR) continue; // timeout o error transitorio
        if (n < (int)sizeof(InputPacket)) continue;

        InputPacket p;
        memcpy(&p, buf, sizeof(p));
        if (p.magic != PROTO_MAGIC || p.type != PKT_INPUT) continue;

        // Descartar paquetes atrasados (UDP puede reordenar).
        if (haveSeq_ && !seq_is_newer(p.seq, lastSeq_)) continue;
        lastSeq_ = p.seq; haveSeq_ = true;

        applyInput(p);
    }
}

static inline short scaleAxis(int v) {
    // Circle pad 3DS (~ +-156) -> eje Xbox (+-32767).
    int s = (v * 32767) / 156;
    return (short)std::max(-32767, std::min(32767, s));
}

void InputServer::applyInput(const InputPacket& p) {
    const uint32_t b = p.buttons;
    const bool desktop = (p.mode & INPUT_MODE_DESKTOP) != 0;
    desktop_.store(desktop);
    audio_.store((p.mode & INPUT_FLAG_AUDIO) != 0);

    // --- Modo escritorio: raton via SendInput (funciona aunque no haya ViGEm) ---
    if (desktop) {
        if (p.right_x != 0 || p.right_y != 0) {
            INPUT mi; memset(&mi, 0, sizeof(mi));
            mi.type = INPUT_MOUSE;
            mi.mi.dx = p.right_x;
            mi.mi.dy = p.right_y;
            mi.mi.dwFlags = MOUSEEVENTF_MOVE;   // movimiento relativo (trackpad)
            SendInput(1, &mi, sizeof(INPUT));
        }
        const bool lclick = (p.vbuttons & VBTN_LCLICK) != 0;
        if (lclick && !prevLClick_) {            // flanco -> clic completo (down+up)
            INPUT cl[2]; memset(cl, 0, sizeof(cl));
            cl[0].type = INPUT_MOUSE; cl[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            cl[1].type = INPUT_MOUSE; cl[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, cl, sizeof(INPUT));
        }
        prevLClick_ = lclick;
    } else {
        prevLClick_ = false;
    }

#ifdef HAVE_VIGEM
    if (!pad_) return;
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);

    if (b & BTN_A)      r.wButtons |= XUSB_GAMEPAD_A;
    if (b & BTN_B)      r.wButtons |= XUSB_GAMEPAD_B;
    if (b & BTN_X)      r.wButtons |= XUSB_GAMEPAD_X;
    if (b & BTN_Y)      r.wButtons |= XUSB_GAMEPAD_Y;
    if (b & BTN_L)      r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b & BTN_R)      r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b & BTN_START)  r.wButtons |= XUSB_GAMEPAD_START;
    if (b & BTN_SELECT) r.wButtons |= XUSB_GAMEPAD_BACK;
    if (b & BTN_DUP)    r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (b & BTN_DDOWN)  r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b & BTN_DLEFT)  r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b & BTN_DRIGHT) r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

    r.sThumbLX = scaleAxis(p.circle_x);
    r.sThumbLY = scaleAxis(p.circle_y); // si va invertido, negar aqui

    if (!desktop) {  // stick derecho + R3 solo en modo juego (en escritorio = raton)
        r.sThumbRX = p.right_x;
        r.sThumbRY = p.right_y;
        if (p.vbuttons & VBTN_R3) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
    }

    vigem_target_x360_update((PVIGEM_CLIENT)client_, (PVIGEM_TARGET)pad_, r);
#else
    // Sin ViGEm: traza throttled para verificar el canal de input.
    static int n = 0;
    if ((n++ % 30) == 0) {
        fprintf(stderr, "[input] seq=%u btn=0x%06X mode=%u r=(%d,%d) vbtn=0x%02X\n",
                p.seq, p.buttons, p.mode, p.right_x, p.right_y, p.vbuttons);
    }
#endif
}

#ifdef HAVE_VIGEM
bool InputServer::vigemInit() {
    PVIGEM_CLIENT c = vigem_alloc();
    if (!c) return false;
    if (!VIGEM_SUCCESS(vigem_connect(c))) { vigem_free(c); return false; }
    PVIGEM_TARGET t = vigem_target_x360_alloc();
    if (!VIGEM_SUCCESS(vigem_target_add(c, t))) {
        vigem_target_free(t); vigem_disconnect(c); vigem_free(c);
        return false;
    }
    client_ = c; pad_ = t;
    fprintf(stderr, "[input] gamepad Xbox 360 virtual conectado\n");
    return true;
}

void InputServer::vigemShutdown() {
    if (pad_)    { vigem_target_remove((PVIGEM_CLIENT)client_, (PVIGEM_TARGET)pad_);
                   vigem_target_free((PVIGEM_TARGET)pad_); pad_ = nullptr; }
    if (client_) { vigem_disconnect((PVIGEM_CLIENT)client_);
                   vigem_free((PVIGEM_CLIENT)client_); client_ = nullptr; }
}
#endif
