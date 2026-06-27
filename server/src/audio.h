// Captura el audio del sistema (WASAPI loopback) y lo envia por UDP al cliente
// en PCM16 mono. Solo envia cuando esta habilitado (lo pide el cliente).
#pragma once
#include <winsock2.h>
#include <thread>
#include <atomic>

class AudioSender {
public:
    ~AudioSender();
    bool start(const sockaddr_in& dst);   // dst = cliente:PORT_AUDIO
    void stop();
    void setEnabled(bool e) { enabled_.store(e); }

private:
    void run();   // hilo de captura WASAPI

    SOCKET            sock_ = INVALID_SOCKET;
    sockaddr_in       dst_{};
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{false};
};
