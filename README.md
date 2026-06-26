# 3DSStreaming

Screen-streaming client **PC (Windows) → Old Nintendo 3DS** with gamepad input
sent back. Built to play PC games on the 3DS over local WiFi.

The Old 3DS has no hardware video decoder (the MVD chip is New-3DS only), so
instead of H.264 it uses **tile-based MJPEG with delta frames**, and the YUV→RGB
conversion runs on the **Y2R hardware module**, keeping the CPU free.

## Architecture

```
┌─ PC (server) ────────────────┐         ┌─ 3DS (client) ────────────────┐
│ DXGI desktop capture         │  UDP    │ receive tiles, reassemble     │
│ → BGRA→YUV420 + tile diff    │ 8000 →  │ → JPEG decode (turbojpeg)     │
│ → JPEG (libjpeg-turbo)       │         │ → Y2R (YUV→RGB) → citro2d     │
│ ViGEm ← input (X360 pad)     │ ← 8001  │ send buttons/sticks/touch     │
└──────────────────────────────┘  TCP    └───────────────────────────────┘
                                   7999 handshake
```

- **`shared/protocol.h`** — packet structures (video, input, handshake), shared by both sides.
- **`server/`** — C++ server (DXGI + libjpeg-turbo + ViGEm + Winsock). See [server/README.md](server/README.md).
- **`client/`** — C client for the 3DS (devkitPro: libctru + citro2d + Y2R + turbojpeg). See [client/README.md](client/README.md).

## Features

- Tile-based MJPEG video with delta frames (only changed regions are sent).
- Adjustable resolution and fps/quality (`--fps`, `--quality`; resolution in `client/source/config.h`).
- Hardware YUV→RGB conversion (Y2R), freeing the CPU to decode.
- Virtual Xbox 360 gamepad (ViGEm): buttons, D-pad, circle pad → left stick.
- **Virtual right stick** on the touch screen + an **R3** button.

## Dependencies

**PC (server):**
- CMake ≥ 3.20 and Visual Studio (MSVC).
- [vcpkg](https://github.com/microsoft/vcpkg) with `libjpeg-turbo` (`vcpkg install libjpeg-turbo:x64-windows`).
- [ViGEmBus](https://github.com/ViGEm/ViGEmBus/releases) driver installed (for the virtual gamepad).
- ViGEmClient is vendored under `server/third_party/` (MIT).

**3DS (client):**
- [devkitPro](https://devkitpro.org/) with `3ds-dev` and `3ds-libjpeg-turbo`
  (`dkp-pacman -S 3ds-dev 3ds-libjpeg-turbo`).
- A 3DS with CFW (Luma3DS) + the Homebrew Launcher.

## Build (quick)

```sh
# Server (PowerShell, with vcpkg)
cmake -S server -B server/build -DUSE_TURBOJPEG=ON -DUSE_VIGEM=ON ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build server/build --config Release

# Client (devkitPro MSYS2 terminal)
cd client && make    # -> client.3dsx
```

Run instructions and tuning options are in each folder's README.

## Status

Working: ~24-30 fps video (320×240) and full controls over local WiFi. Software
JPEG decode on the ARM11 (no NEON) is the fps bottleneck; quality is adjustable to
trade sharpness for frame rate.

## Credits

- [ViGEmClient / ViGEmBus](https://github.com/ViGEm) — virtual gamepad (MIT).
- [libjpeg-turbo](https://libjpeg-turbo.org/), [devkitPro](https://devkitpro.org/) (libctru, citro3d/citro2d).
