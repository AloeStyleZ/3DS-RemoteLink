# 3DS client — 3DSStreaming

Client for the Old/New Nintendo 3DS. Receives YUV420 video over UDP, reassembles
it, converts it to RGB with the **Y2R hardware module** and draws it with
**citro2d**. Captures input with libctru and sends it to the PC over UDP, including
a virtual right stick + R3 button on the touch screen.

## Pipeline

```
TCP handshake :7999  ──► negotiate resolution/codec/ports        [net.c]
        │
  loop every frame (~60 Hz):
   1. hidScanInput → InputPacket → UDP :8001                     [main.c/net.c]
   2. net_drain_video: non-blocking recvfrom (UDP :8000)         [net.c]
        └─ video_on_packet: reassemble tile → JPEG/RAW decode    [decoder.c]
   3. video_update: Y2R (YUV→RGB) → texture (latest frame)       [decoder.c]
   4. citro2d: draw video (top) + virtual stick/R3 (bottom)      [main.c]
```

## Requirements

devkitPro with the 3DS packages:

```sh
dkp-pacman -S 3ds-dev 3ds-libjpeg-turbo
```

Make sure `DEVKITPRO` and `DEVKITARM` are set in the environment.

## Build

```sh
cd client
make          # -> client.3dsx
```

(Builds against `../shared/protocol.h`, shared with the server.)

## Configure and run

1. Edit [source/config.h](source/config.h):
   - `SERVER_IP` = your PC's IP on the LAN.
   - `STREAM_WIDTH` / `STREAM_HEIGHT` = stream resolution (multiple of 80: 400×240, 320×240, 320×160, 240×160).
2. Start the server on the PC (it waits on TCP 7999).
3. Copy `client.3dsx` to the SD card (`/3ds/`) and launch it from the Homebrew Launcher
   (or push it over WiFi with `3dslink -a <3DS_IP> client.3dsx`).
4. Exit: **START + SELECT**.

> Test on real hardware: Citra/Lime3DS does not implement LAN sockets to the host,
> so the network path won't connect there.

## Controls

| 3DS | Xbox 360 |
|---|---|
| A B X Y | A B X Y (by name) |
| L / R | LB / RB |
| D-pad | D-pad |
| Circle pad | Left stick |
| START / SELECT | Start / Back |
| Touch screen (right circle) | Right stick (sticky: clamps to the edge) |
| Touch screen (left button) | R3 (right-stick click) |

The touch screen is single-touch, so the right stick and R3 cannot be used at the
same time. Stick/R3 geometry lives in `shared/protocol.h` (`VSTICK_*`, `VR3_*`).

## Notes / TODO

- Receive on a core1 thread (less urgent; runs fine single-threaded for now).
- Map ZL/ZR to extra touch zones.
- Position-based face-button mapping if name-based feels swapped.
- Enter the server IP via on-screen keyboard (swkbd) instead of `config.h`.
