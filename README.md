# 3DSStreaming

Cliente de streaming de pantalla **PC (Windows) → Old Nintendo 3DS** con control
de gamepad de vuelta. Pensado para jugar en PC desde la 3DS por WiFi local.

La Old 3DS no tiene decodificador de vídeo por hardware (el chip MVD es exclusivo
de la New 3DS), así que en vez de H.264 se usa **MJPEG por tiles con delta frames**
y la conversión YUV→RGB se hace en el **módulo hardware Y2R**, dejando la CPU libre.

## Arquitectura

```
┌─ PC (servidor) ──────────────┐         ┌─ 3DS (cliente) ───────────────┐
│ DXGI captura escritorio      │  UDP    │ recibe tiles, reensambla      │
│ → BGRA→YUV420 + diff tiles   │ 8000 →  │ → JPEG decode (turbojpeg)     │
│ → JPEG (libjpeg-turbo)       │         │ → Y2R (YUV→RGB) → citro2d     │
│ ViGEm ← input (gamepad X360) │ ← 8001  │ envia botones/sticks/tactil   │
└──────────────────────────────┘  TCP    └───────────────────────────────┘
                                   7999 handshake
```

- **`shared/protocol.h`** — estructuras de paquetes (vídeo, input, handshake), compartidas por ambos lados.
- **`server/`** — servidor C++ (DXGI + libjpeg-turbo + ViGEm + Winsock). Ver [server/README.md](server/README.md).
- **`client/`** — cliente C para 3DS (devkitPro: libctru + citro2d + Y2R + turbojpeg). Ver [client/README.md](client/README.md).

## Características

- Vídeo MJPEG por tiles con delta frames (solo se envían las zonas que cambian).
- Resolución y fps/calidad ajustables (`--fps`, `--quality`; resolución en `client/source/config.h`).
- Conversión de color YUV→RGB por hardware (Y2R), CPU libre para decodificar.
- Gamepad Xbox 360 virtual (ViGEm): botones, cruceta, circle pad → stick izquierdo.
- **Stick derecho virtual** en la pantalla táctil + botón **R3**.

## Dependencias

**PC (servidor):**
- CMake ≥ 3.20 y Visual Studio (MSVC).
- [vcpkg](https://github.com/microsoft/vcpkg) con `libjpeg-turbo` (`vcpkg install libjpeg-turbo:x64-windows`).
- Driver [ViGEmBus](https://github.com/ViGEm/ViGEmBus/releases) instalado (para el gamepad virtual).
- ViGEmClient va incluido en `server/third_party/` (MIT).

**3DS (cliente):**
- [devkitPro](https://devkitpro.org/) con `3ds-dev` y `3ds-libjpeg-turbo`
  (`dkp-pacman -S 3ds-dev 3ds-libjpeg-turbo`).
- 3DS con CFW (Luma3DS) + Homebrew Launcher.

## Compilar (resumen)

```sh
# Servidor (PowerShell, con vcpkg)
cmake -S server -B server/build -DUSE_TURBOJPEG=ON -DUSE_VIGEM=ON ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build server/build --config Release

# Cliente (terminal MSYS2 de devkitPro)
cd client && make    # -> client.3dsx
```

Detalles de ejecución y ajustes en los README de cada carpeta.

## Estado

Funcional: vídeo a ~24-30 fps (320×240) y controles completos sobre WiFi local.
El decode JPEG por software en el ARM11 (sin NEON) es el límite de fps; la calidad
es ajustable según el equilibrio fps/nitidez que prefieras.

## Créditos

- [ViGEmClient / ViGEmBus](https://github.com/ViGEm) — gamepad virtual (MIT).
- [libjpeg-turbo](https://libjpeg-turbo.org/), [devkitPro](https://devkitpro.org/) (libctru, citro3d/citro2d).
