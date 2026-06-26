# PC server — 3DSStreaming

Captures the desktop with DXGI, converts it to YUV420, sends only the tiles that
changed over UDP to the 3DS, and receives the 3DS input to inject it as a virtual
Xbox 360 gamepad.

## Pipeline

```
DXGI Desktop Duplication (GPU, BGRA)
        │  CopyResource → staging → Map (CPU)
        ▼
bgraToI420Scaled()  BGRA → I420 + scale to 400×240    [convert.cpp]
        ▼
computeDirtyTiles() per-tile diff vs previous frame    [convert.cpp]
        ▼
VideoSender::sendFrame()  encode (RAW/JPEG) + UDP fragmentation → :8000  [video.cpp]

InputServer (separate thread): UDP :8001 → seq filter → ViGEm Xbox360   [input.cpp]
TCP handshake :7999 negotiates resolution/codec/ports                  [main.cpp]
```

## Build

Core (Windows SDK only, no external dependencies):

```sh
cmake -S . -B build
cmake --build build --config Release
# -> build/Release/server.exe
```

With JPEG codec + virtual gamepad (requires vcpkg + the ViGEmBus driver):

```sh
vcpkg install libjpeg-turbo:x64-windows
cmake -S . -B build -DUSE_TURBOJPEG=ON -DUSE_VIGEM=ON ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

ViGEmClient is vendored under `third_party/ViGEmClient` (MIT); the runtime still
needs the [ViGEmBus](https://github.com/ViGEm/ViGEmBus/releases) driver installed.

## Run

```sh
server.exe                          # RAW YUV420 (baseline, ~0 CPU on the 3DS)
server.exe --jpeg                   # MJPEG codec (needs USE_TURBOJPEG)
server.exe --jpeg --fps 24 --quality 40   # tune frame rate / quality
server.exe --output 1               # capture the second monitor
```

Flags:
- `--jpeg` — use the MJPEG codec (much lower bandwidth; needed for motion).
- `--fps N` — target frame rate (5–60).
- `--quality N` — JPEG quality (10–95). Lower = smaller tiles + faster client decode.
- `--output N` — monitor index to capture.

It waits on TCP 7999 for the 3DS client to connect. Ctrl+C to exit.

## Status

| Component | State |
|---|---|
| TCP handshake | ✅ |
| DXGI capture + reinit on ACCESS_LOST | ✅ |
| BGRA→I420 + scaling | ✅ (point sampling; swappable for libyuv) |
| Tile delta + keyframes | ✅ |
| UDP fragmentation (protocol.h) | ✅ |
| Input receive + sequence filter | ✅ |
| ViGEm injection | ✅ with `-DUSE_VIGEM=ON` (otherwise logs to console) |
| MJPEG codec | ✅ with `-DUSE_TURBOJPEG=ON` |
| Touch → right stick + R3 | ✅ |

## Notes / TODO

- Without ViGEm, input is only logged to the console (useful to validate the channel).
- Point-sampling scaler: migrate to libyuv for quality/speed if needed.
- Incoming `CtrlPacket` (CTRL_REQUEST_KEYFRAME) not yet handled.
