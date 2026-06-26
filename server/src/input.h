// Servidor de input: recibe InputPacket por UDP (puerto 8001) en su propio hilo,
// filtra por secuencia y lo inyecta como gamepad Xbox 360 virtual via ViGEm.
#pragma once
#include "protocol.h"
#include <winsock2.h>
#include <thread>
#include <atomic>
#include <cstdint>

class InputServer {
public:
    ~InputServer();
    // Crea y bindea el socket UDP en `port`. clientIp opcional para filtrar origen.
    bool start(uint16_t port);
    void stop();

    // true si el cliente esta en modo escritorio (raton). Lo lee el loop de video
    // para componer el marcador del cursor.
    bool isDesktop() const { return desktop_.load(); }

private:
    void runLoop();
    void applyInput(const InputPacket& p);

    SOCKET            sock_ = INVALID_SOCKET;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> desktop_{false};
    uint16_t          lastSeq_ = 0;
    bool              haveSeq_ = false;
    bool              prevLClick_ = false;

#ifdef HAVE_VIGEM
    void* client_ = nullptr;  // PVIGEM_CLIENT
    void* pad_    = nullptr;  // PVIGEM_TARGET
    bool  vigemInit();
    void  vigemShutdown();
#endif
};
