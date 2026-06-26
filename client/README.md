# Cliente 3DS — 3DSStreaming

Cliente para Old/New Nintendo 3DS. Recibe vídeo YUV420 por UDP, lo reensambla,
lo convierte a RGB con el módulo hardware **Y2R** y lo dibuja con **citro2d**.
Captura el input con libctru y lo envía al PC por UDP.

## Pipeline

```
Handshake TCP :7999  ──► negocia resolución/codec/puertos        [net.c]
        │
  bucle cada frame (~60 Hz):
   1. hidScanInput → InputPacket → UDP :8001                     [main.c/net.c]
   2. net_drain_video: recvfrom no bloqueante (UDP :8000)        [net.c]
        └─ video_on_packet: reensambla tile → unpack en I420     [decoder.c]
           └─ al recibir FRAME_END: Y2R (YUV→RGB565) → textura   [decoder.c]
   3. C2D_DrawImageAt: dibuja la textura en el top screen        [decoder.c]
```

## Requisitos

devkitPro con los paquetes 3DS:

```sh
# instalar devkitPro (pacman): 3ds-dev incluye libctru, citro3d, citro2d
dkp-pacman -S 3ds-dev
```

Asegúrate de tener `DEVKITPRO` y `DEVKITARM` en el entorno.

## Compilar

```sh
cd client
make          # -> client.3dsx
```

(Compila contra `../shared/protocol.h`, compartido con el servidor.)

## Configurar y ejecutar

1. Edita [source/config.h](source/config.h): pon `SERVER_IP` = IP de tu PC en la LAN.
2. Arranca el servidor en el PC (queda esperando en TCP 7999).
3. Copia `client.3dsx` a la SD (`/3ds/`) y lánzalo desde el Homebrew Launcher,
   o ejecútalo en **Citra/Lime3DS** (emulador) para iterar rápido.
4. Salir: **START + SELECT**.

## Estado y puntos a verificar en hardware

Escrito sin poder compilar/probar aquí (sin toolchain). Lo más probable que haya
que ajustar al primer arranque:

| Punto | Dónde | Qué mirar |
|---|---|---|
| Layout Y2R→textura | `video_present()` en decoder.c | Si la imagen sale descuadrada/rayada, revisar `unit`/`gap` de `Y2RU_SetReceiving` |
| Orientación de la imagen | `subtex.top/bottom` en decoder.c | Si sale invertida, intercambiar `top`/`bottom` |
| Color | `Y2RU_SetStandardCoefficient` | `_SCALING` (rango estudio) vs sin scaling |
| Socket no bloqueante | `net_open_streams()` | depende de `fcntl(O_NONBLOCK)` en el lwip del 3DS |

## Pendiente / mejoras

- **Hilo receptor en core1** (`APT_SetAppCpuTimeLimit` + `threadCreate`): hoy el
  `recv` se drena en el hilo principal. Es la optimización clave del plan original.
- Decoder JPEG (codec 1): hoy solo RAW YUV420.
- Pedir keyframe (`CTRL_REQUEST_KEYFRAME`) al detectar tiles congelados.
- IP del servidor por teclado en pantalla (`swkbdInit`) en vez de `config.h`.
