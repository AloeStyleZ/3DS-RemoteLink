// Capa de red del cliente 3DS: soc, handshake TCP, recepcion UDP de video y
// envio UDP de input.
#pragma once
#include <3ds.h>
#include "protocol.h"

struct VideoDecoder; // fwd

// Inicializa el servicio soc (sockets). false si falla.
bool net_init(void);
void net_exit(void);

// Conecta por TCP al servidor, envia ClientHello y recibe ServerHello en `out`.
bool net_handshake(const char* server_ip, ServerHello* out);

// Crea el socket UDP de video (bind) y prepara el destino de input.
bool net_open_streams(const char* server_ip, const ServerHello* sh);

// Envia un paquete de input (fire-and-forget) al servidor.
void net_send_input(const InputPacket* p);

// Drena TODOS los datagramas de video disponibles (socket no bloqueante),
// pasandolos al decoder. Devuelve true si algun frame se completo (FRAME_END).
bool net_drain_video(struct VideoDecoder* dec);

// Drena los datagramas de audio disponibles y los reproduce (NDSP).
void net_drain_audio(void);
