/* =============================================================================
 *  3DSStreaming - Protocolo de red compartido (PC <-> Old Nintendo 3DS)
 *
 *  Incluido tal cual por:
 *    - Servidor PC : C++ / MSVC (DXGI + libjpeg-turbo + ViGEm)
 *    - Cliente 3DS : C / devkitARM (libctru + citro3d + Y2R)
 *
 *  CONVENCIONES
 *  ------------
 *  - Endianness: LITTLE-ENDIAN en el cable. x86 y el ARM11 del 3DS son LE,
 *    asi que no hace falta byte-swapping en ningun lado.
 *  - Empaquetado: #pragma pack(1) para que MSVC y GCC produzcan layouts
 *    identicos. Ademas los campos estan ordenados para quedar "naturalmente"
 *    alineados aunque esten packed (no hay padding real). Si el buffer de
 *    recepcion esta alineado a >=4 bytes, todos los accesos halfword/word
 *    caen alineados -> seguro en ARM11 (ARMv6).
 *  - MTU: el payload UDP total se mantiene <= 1400 bytes para evitar
 *    fragmentacion IP sobre WiFi (MTU 1500 - 28 de cabeceras IP/UDP).
 * ============================================================================= */
#ifndef THREEDS_STREAMING_PROTOCOL_H
#define THREEDS_STREAMING_PROTOCOL_H

#include <stdint.h>

/* --- Tipos fijos por si algun toolchain no trae stdint completo ----------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t  s16;

/* --- Parametros globales del protocolo ------------------------------------ */
#define PROTO_MAGIC          0x33u   /* '3' : tag de sanidad en cada paquete   */
#define PROTO_VERSION        1u

#define PORT_HANDSHAKE       7999    /* TCP : negociacion inicial               */
#define PORT_VIDEO           8000    /* UDP : PC  -> 3DS (video)                */
#define PORT_INPUT           8001    /* UDP : 3DS -> PC  (input, max prioridad) */
#define PORT_AUDIO           8002    /* UDP : PC  -> 3DS (audio, opcional)      */

/* Payload util por datagrama (sin contar cabeceras IP/UDP).
 * 1400 (MTU seguro) - 16 (cabecera de video) = 1384, redondeo a 1380.        */
#define UDP_MAX_PAYLOAD      1380

/* Resolucion de referencia (negociable en el handshake). */
#define VIDEO_DEFAULT_W      400
#define VIDEO_DEFAULT_H      240

/* Rejilla de tiles por defecto: 80x80 -> 5x3 = 15 tiles.
 * 80 = 5*16  => alineado a MCU de JPEG 4:2:0 y a croma /2.                    */
#define TILE_DEFAULT_W       80
#define TILE_DEFAULT_H       80
#define TILE_MAX_COUNT       256     /* tope para dimensionar bitmaps          */

/* --- Stick virtual tactil (pantalla inferior 320x240) -> stick derecho -----
 * El cliente dibuja el stick aqui y el servidor mapea el toque dentro de este
 * circulo al stick derecho del gamepad. Ambos usan estas mismas constantes.   */
#define VSTICK_CX            250     /* centro X (lado derecho)                 */
#define VSTICK_CY            120     /* centro Y (vertical centrado)            */
#define VSTICK_R             65      /* radio del area activa / base            */

/* --- Boton tactil R3 (clic del stick derecho), lado izquierdo -------------- */
#define VR3_CX               55      /* centro X (lado izquierdo)               */
#define VR3_CY               120     /* centro Y                                */
#define VR3_R                32      /* radio del boton                         */

/* --- Identificadores de tipo de paquete (campo `type`) -------------------- */
enum {
    PKT_VIDEO  = 0x10,   /* fragmento de video  (PC -> 3DS)  */
    PKT_INPUT  = 0x20,   /* estado de input     (3DS -> PC)  */
    PKT_CTRL   = 0x30,   /* control fuera-de-banda por UDP (p.ej. pedir keyframe) */
    PKT_AUDIO  = 0x40    /* muestras de audio   (PC -> 3DS)  */
};

/* --- Codecs del payload de video (campo `codec`) -------------------------- */
enum {
    CODEC_RAW_YUV420 = 0,  /* planos Y||U||V crudos del tile (CPU ~0 en 3DS)    */
    CODEC_JPEG_YCBCR = 1,  /* JPEG del tile, salida en planos YCbCr (sin RGB)   */
    CODEC_ETC1       = 2   /* bloques ETC1 (4x4 px, 8 bytes) - decode 100% GPU  */
};
#define ETC1_BLOCK_BYTES 8   /* bytes de un bloque ETC1 (4x4 px). Compartido    *
                              * entre el encoder (servidor) y el swizzle        *
                              * (cliente).                                      */

/* Bits de codec_caps en ClientHello (que codecs sabe decodificar el cliente). */
enum {
    CODEC_CAP_RAW  = 1u << 0,
    CODEC_CAP_JPEG = 1u << 1,
    CODEC_CAP_ETC1 = 1u << 2
};

/* =============================================================================
 *  CANAL DE VIDEO   (PC -> 3DS, UDP, puerto 8000)
 * -----------------------------------------------------------------------------
 *  Cada frame se divide en TILES (rejilla negociada). Solo se envian los tiles
 *  que cambiaron respecto al frame anterior (delta), salvo en KEYFRAMES donde
 *  van todos. Cada tile se trocea en 1..N fragmentos que caben en un datagrama.
 *
 *  Cada datagrama es AUTO-DESCRIPTIVO: aunque se pierdan otros, el cliente sabe
 *  exactamente a que frame / tile / offset pertenece. El cliente mantiene un
 *  framebuffer YUV persistente; al recibir un tile completo escribe sus planos
 *  en la posicion del tile. Al ver FLAG_FRAME_END, dispara Y2R + render+flip.
 * ============================================================================= */
#pragma pack(push, 1)

/* flags (bitfield de 8 bits) */
enum {
    VFLAG_FRAME_START = 1u << 0,  /* primer datagrama de un frame_id nuevo      */
    VFLAG_FRAME_END   = 1u << 1,  /* completa el frame -> el cliente presenta   */
    VFLAG_KEYFRAME    = 1u << 2,  /* este frame contiene TODOS los tiles        */
    VFLAG_TILE_LAST   = 1u << 3   /* ultimo fragmento de este tile (= frag_index==frag_count-1) */
};

typedef struct {
    u8  magic;        /* PROTO_MAGIC                                            */
    u8  type;         /* PKT_VIDEO                                              */
    u8  flags;        /* combinacion de VFLAG_*                                 */
    u8  codec;        /* CODEC_RAW_YUV420 | CODEC_JPEG_YCBCR                    */
    u16 frame_id;     /* contador monotono de frame (wrap a 65536)             */
    u16 tile_id;      /* indice del tile en la rejilla (0..tile_count-1)       */
    u16 frag_index;   /* fragmento dentro del tile (0..frag_count-1)           */
    u16 frag_count;   /* total de fragmentos de este tile                      */
    u16 payload_len;  /* bytes utiles de payload en ESTE datagrama             */
    u16 tile_bytes;   /* tamano total (codificado) del tile completo           */
    /* sigue: u8 payload[payload_len]                                          */
} VideoPktHeader;     /* sizeof == 16 */

/* Datagrama de video completo (cabecera + payload en el mismo buffer). */
typedef struct {
    VideoPktHeader hdr;
    u8             payload[UDP_MAX_PAYLOAD];
} VideoPacket;

/* =============================================================================
 *  CANAL DE INPUT   (3DS -> PC, UDP, puerto 8001)
 * -----------------------------------------------------------------------------
 *  Fire-and-forget, sin ACK, maxima prioridad. Se envia cada frame del cliente
 *  (idealmente 60 Hz) -> <16ms extremo a extremo. El PC aplica solo el `seq`
 *  mas reciente y descarta los atrasados (manejo de wrap con resta unsigned).
 *
 *  Mapeo desde libctru:
 *    buttons   = hidKeysHeld()          (digital + dpad)
 *    circle_x  = hidCircleRead().dx      (~ -156..156)  -> stick izquierdo
 *    circle_y  = hidCircleRead().dy
 *    right_x/y = stick derecho, calculado en el cliente a partir del stick
 *                virtual tactil (ya en rango -32767..32767, listo para sThumbR*).
 *    vbuttons  = botones tactiles virtuales (VBTN_*), p.ej. R3.
 * ============================================================================= */
typedef struct {
    u8  magic;        /* PROTO_MAGIC                          off 0            */
    u8  type;         /* PKT_INPUT                            off 1            */
    u16 seq;          /* secuencia monotona (wrap a 65536)    off 2            */
    u32 buttons;      /* hidKeysHeld() bitfield               off 4 (alineado) */
    s16 circle_x;     /* eje X circle pad                     off 8            */
    s16 circle_y;     /* eje Y circle pad                     off 10           */
    s16 right_x;      /* juego: stick derecho; escritorio: delta raton X off 12 */
    s16 right_y;      /* juego: stick derecho; escritorio: delta raton Y off 14 */
    u8  vbuttons;     /* botones tactiles virtuales (VBTN_*)  off 16           */
    u8  mode;         /* INPUT_MODE_* (juego / escritorio)    off 17           */
} InputPacket;        /* sizeof == 18 */

/* Campo `mode`: ahora es un bitfield de flags.
 *   bit0 = tactil como trackpad de raton (si no, stick derecho + R3)
 *   bit1 = audio habilitado (el cliente pide que el servidor envie audio)        */
enum {
    INPUT_MODE_GAME    = 0,        /* (valor) tactil = stick derecho + R3 */
    INPUT_MODE_DESKTOP = 1u << 0,  /* (bit0)  tactil = trackpad de raton   */
    INPUT_FLAG_AUDIO   = 1u << 1   /* (bit1)  audio habilitado             */
};

/* Botones tactiles virtuales (campo vbuttons). */
enum {
    VBTN_R3     = 1u << 0,   /* (juego)      clic del stick derecho             */
    VBTN_LCLICK = 1u << 1    /* (escritorio) clic izquierdo del raton           */
};

/* Subset de mascaras de hidKeysHeld() relevantes (espejo de libctru/hid.h).
 * Se duplican aqui para que el SERVIDOR PC las conozca sin depender de libctru. */
enum {
    BTN_A      = 1u << 0,
    BTN_B      = 1u << 1,
    BTN_SELECT = 1u << 2,
    BTN_START  = 1u << 3,
    BTN_DRIGHT = 1u << 4,
    BTN_DLEFT  = 1u << 5,
    BTN_DUP    = 1u << 6,
    BTN_DDOWN  = 1u << 7,
    BTN_R      = 1u << 8,
    BTN_L      = 1u << 9,
    BTN_X      = 1u << 10,
    BTN_Y      = 1u << 11,
    BTN_TOUCH  = 1u << 20   /* KEY_TOUCH : pantalla tactil presionada          */
};

/* =============================================================================
 *  CANAL DE CONTROL  (UDP fuera-de-banda, opcional)
 * -----------------------------------------------------------------------------
 *  El 3DS puede pedir un keyframe tras detectar perdida prolongada de un tile,
 *  sin abrir nada nuevo: se manda al puerto de video con type=PKT_CTRL.
 * ============================================================================= */
enum {
    CTRL_REQUEST_KEYFRAME = 1,
    CTRL_PING             = 2,
    CTRL_BYE              = 3
};

typedef struct {
    u8  magic;        /* PROTO_MAGIC      */
    u8  type;         /* PKT_CTRL         */
    u8  ctrl_code;    /* CTRL_*           */
    u8  _pad;
    u32 arg;          /* p.ej. frame_id de referencia / timestamp ping        */
} CtrlPacket;         /* sizeof == 8 */

/* =============================================================================
 *  CANAL DE AUDIO  (PC -> 3DS, UDP, puerto 8002) — PCM16 mono, opcional
 * -----------------------------------------------------------------------------
 *  El servidor captura el audio del sistema (WASAPI loopback), lo baja a PCM16
 *  mono y lo trocea en datagramas. Solo lo envia si el cliente lo pide
 *  (INPUT_FLAG_AUDIO en el campo mode del input). El cliente lo reproduce con NDSP.
 * ============================================================================= */
#define AUDIO_MAX_SAMPLES 480     /* muestras PCM16 mono por datagrama (~10ms@48k) */

typedef struct {
    u8  magic;        /* PROTO_MAGIC                                           */
    u8  type;         /* PKT_AUDIO                                             */
    u16 seq;          /* secuencia                                             */
    u16 samples;      /* nº de muestras PCM16 mono en el payload               */
    u16 rate;         /* frecuencia de muestreo (Hz), p.ej. 48000              */
    /* sigue: s16 pcm[samples]                                                 */
} AudioPacketHeader;  /* sizeof == 8 */

/* =============================================================================
 *  HANDSHAKE   (TCP, puerto 7999)
 * -----------------------------------------------------------------------------
 *  1) 3DS conecta y envia ClientHello con lo que pide / soporta.
 *  2) PC responde ServerHello con los parametros ACORDADOS (manda el server).
 *  3) Se cierra/mantiene el TCP; el video/input ya van por UDP con esos params.
 * ============================================================================= */
typedef struct {
    u8  magic;            /* PROTO_MAGIC                                       */
    u8  version;          /* PROTO_VERSION                                     */
    u16 want_width;       /* resolucion deseada (p.ej. 400)                    */
    u16 want_height;      /* (p.ej. 240)                                       */
    u16 codec_caps;       /* bitmask CODEC_CAP_* (que codecs decodifica el cliente) */
    u16 udp_video_port;   /* puerto donde el 3DS escuchara video (8000)        */
    u16 udp_input_port;   /* puerto desde el que el 3DS enviara input (8001)   */
    u32 max_kbps;         /* techo de ancho de banda que el cliente acepta     */
} ClientHello;            /* sizeof == 16 */

typedef struct {
    u8  magic;            /* PROTO_MAGIC                                       */
    u8  version;          /* PROTO_VERSION                                     */
    u8  codec;            /* CODEC_* elegido por el servidor                   */
    u8  jpeg_quality;     /* 1..100 (solo si codec == JPEG)                    */
    u16 width;            /* resolucion ACORDADA                               */
    u16 height;
    u16 tile_w;           /* dimensiones de tile acordadas (def 80x80)         */
    u16 tile_h;
    u16 tiles_x;          /* width  / tile_w                                   */
    u16 tiles_y;          /* height / tile_h                                   */
    u8  fps;              /* objetivo (30)                                     */
    u8  keyframe_interval;/* cada cuantos frames se fuerza un keyframe (p.ej.30)*/
    u16 udp_video_port;   /* puerto del 3DS donde el PC enviara video          */
    u16 udp_input_port;   /* puerto del PC donde escucha input                 */
} ServerHello;           /* sizeof == 20 */

#pragma pack(pop)

/* =============================================================================
 *  Helpers de secuencia (manejo de wrap-around)
 * ============================================================================= */

/* Devuelve true si `a` es mas reciente que `b` en aritmetica circular de 16b.
 * Uso: en el PC, aceptar input solo si seq_is_newer(pkt.seq, last_seq).        */
static inline int seq_is_newer(u16 a, u16 b) {
    return (u16)(a - b) < 0x8000u;
}

#endif /* THREEDS_STREAMING_PROTOCOL_H */
