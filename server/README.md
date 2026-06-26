# Servidor PC — 3DSStreaming

Captura el escritorio con DXGI, lo convierte a YUV420, envía solo los tiles que
cambiaron por UDP al 3DS y recibe el input del 3DS para inyectarlo como gamepad
Xbox 360 virtual.

## Pipeline

```
DXGI Desktop Duplication (GPU, BGRA)
        │  CopyResource → staging → Map (CPU)
        ▼
bgraToI420Scaled()  BGRA → I420 + escalado a 400×240   [convert.cpp]
        ▼
computeDirtyTiles() diff por tile vs frame anterior     [convert.cpp]
        ▼
VideoSender::sendFrame()  encode (RAW/JPEG) + fragmentación UDP → :8000  [video.cpp]

InputServer (hilo aparte): UDP :8001 → filtro seq → ViGEm Xbox360   [input.cpp]
Handshake TCP :7999 negocia resolución/codec/puertos               [main.cpp]
```

## Compilar

Núcleo (solo Windows SDK, sin dependencias externas):

```sh
cmake -S . -B build
cmake --build build --config Release
# -> build/Release/server.exe
```

Con gamepad virtual (requiere driver ViGEmBus + vcpkg):

```sh
vcpkg install vigemclient libjpeg-turbo
cmake -S . -B build -DUSE_VIGEM=ON -DUSE_TURBOJPEG=ON ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Ejecutar

```sh
server.exe                 # codec RAW YUV420 (baseline, CPU ~0 en el 3DS)
server.exe --jpeg          # codec MJPEG (requiere USE_TURBOJPEG)
server.exe --output 1      # capturar el segundo monitor
```

Queda esperando en TCP 7999 a que el cliente 3DS conecte. Ctrl+C para salir.

## Estado

| Componente | Estado |
|---|---|
| Handshake TCP | ✅ |
| Captura DXGI + reinit ante ACCESS_LOST | ✅ |
| BGRA→I420 + escalado | ✅ (point-sampling; sustituible por libyuv) |
| Delta por tiles + keyframes | ✅ |
| Fragmentación UDP (protocol.h) | ✅ |
| Recepción de input + filtro de secuencia | ✅ |
| Inyección ViGEm | ✅ tras `-DUSE_VIGEM=ON` (si no, traza por consola) |
| Codec MJPEG | ✅ tras `-DUSE_TURBOJPEG=ON` |

## Pendiente / mejoras

- Sin ViGEm, el input solo se traza por consola (útil para validar el canal).
- El touchscreen se recibe pero aún no se mapea a ViGEm (¿stick derecho / ratón?).
- Escalado por point-sampling: para calidad/velocidad, migrar a libyuv.
- Manejo de `CtrlPacket` (CTRL_REQUEST_KEYFRAME) entrante aún no implementado.
