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

private:
    void runLoop();
    void applyInput(const InputPacket& p);

    SOCKET            sock_ = INVALID_SOCKET;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    uint16_t          lastSeq_ = 0;
    bool              haveSeq_ = false;

#ifdef HAVE_VIGEM
    void* client_ = nullptr;  // PVIGEM_CLIENT
    void* pad_    = nullptr;  // PVIGEM_TARGET
    bool  vigemInit();
    void  vigemShutdown();
#endif
};
