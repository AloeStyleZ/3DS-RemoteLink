# .cia packaging

Builds `client.cia` to install on the HOME menu of a 3DS with CFW (Luma3DS).
For **development** keep using `.3dsx` + `3dslink` (see the client README); the
`.cia` is for a permanent install.

## 1) Tools (once)

Not shipped with devkitPro; grab them from Steveice10's GitHub releases and put
them on the PATH (e.g. in `$DEVKITPRO/tools/bin`):

- **makerom**   → `Project_CTR` repo
- **bannertool** → `bannertool` repo

Verify: `makerom -h` and `bannertool` should respond.

## 2) Assets (you provide these in this `client/cia/` folder)

| File | Format | Purpose |
|---|---|---|
| `icon.png`   | PNG **48×48**  | HOME menu icon |
| `banner.png` | PNG **256×128** | banner (the wide image when selected) |
| `banner.wav` | WAV **PCM 16-bit** (short) | banner sound; a silent one is fine |

No artwork yet? Placeholders work. With ImageMagick + ffmpeg:

```sh
magick -size 48x48   xc:#1e6fd0 icon.png
magick -size 256x128 xc:#1e6fd0 banner.png
ffmpeg -f lavfi -i anullsrc=r=22050:cl=mono -t 1 -c:a pcm_s16le banner.wav
```

## 3) Build

```sh
cd client
make                      # produces client.elf (and client.3dsx)
./cia/build_cia.sh        # produces client.cia
```

## 4) Install on the 3DS

1. Copy `client.cia` to the SD card.
2. Open **FBI** → SD → select `client.cia` → *Install and delete*.
3. "3DSStream" appears on the HOME menu. Launch it like any game.

(Luma3DS applies signature patches by default, so FBI installs the CIA without issues.)

## Why this RSF

[app.rsf](app.rsf) is based on the standard homebrew template. The app-specific
part is `ServiceAccessControl`, which **must** include `soc:U` (sockets),
`ndm:u` and `ac:u` (WiFi) and `y2r:u` (YUV→RGB conversion). If the CIA boots but
networking or video fails, that block is the first thing to check.
