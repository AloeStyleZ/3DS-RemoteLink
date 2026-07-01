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

// Cierra los sockets de stream (video/input/audio) para poder reconectar.
void net_close_streams(void);

// Envia un paquete de input (fire-and-forget) al servidor.
void net_send_input(const InputPacket* p);

// Envia un mensaje de control al servidor (p.ej. cambiar calidad/fps en vivo).
void net_send_ctrl(u8 code, u32 arg);

// Envia texto (UTF-8) para que el PC lo teclee.
void net_send_text(const char* s, int len);

// Drena TODOS los datagramas de video disponibles (socket no bloqueante),
// pasandolos al decoder. Devuelve true si se recibio ALGUN paquete (actividad,
// para detectar caida de conexion).
bool net_drain_video(struct VideoDecoder* dec);

// Drena los datagramas de audio disponibles y los reproduce (NDSP).
void net_drain_audio(void);

// Hilo receptor experimental en core1: recibe + decodifica + Y2R en paralelo
// al hilo principal. Devuelve true si arranco (entonces el main NO debe drenar
// el video el mismo). Si devuelve false, usar net_drain_video()+video_update().
bool net_start_receiver(struct VideoDecoder* dec);
void net_stop_receiver(void);
