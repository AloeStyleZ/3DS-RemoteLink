# Empaquetado .cia

Genera `client.cia` para instalar en el menú HOME de una 3DS con CFW (Luma3DS).
Para **desarrollo** sigue prefiriendo `.3dsx` + `3dslink` (ver el README del cliente);
el `.cia` es para tenerlo instalado "de verdad".

## 1) Herramientas (una vez)

No vienen con devkitPro; bájalas de los releases de GitHub de Steveice10 y ponlas
en el PATH (p.ej. en `$DEVKITPRO/tools/bin`):

- **makerom**   → repo `Project_CTR`
- **bannertool** → repo `bannertool`

Comprueba: `makerom -h` y `bannertool` deben responder.

## 2) Assets (los pones tú en esta carpeta `client/cia/`)

| Archivo | Formato | Para qué |
|---|---|---|
| `icon.png`   | PNG **48×48**  | icono del menú HOME |
| `banner.png` | PNG **256×128** | banner (la imagen ancha al seleccionar) |
| `banner.wav` | WAV **PCM 16-bit** (corto) | sonido del banner; vale uno en silencio |

¿No tienes arte aún? Sirven placeholders. Con ImageMagick + ffmpeg:

```sh
magick -size 48x48  xc:#1e6fd0 icon.png
magick -size 256x128 xc:#1e6fd0 banner.png
ffmpeg -f lavfi -i anullsrc=r=22050:cl=mono -t 1 -c:a pcm_s16le banner.wav
```

## 3) Construir

```sh
cd client
make                      # produce client.elf (y client.3dsx)
./cia/build_cia.sh        # produce client.cia
```

## 4) Instalar en la 3DS

1. Copia `client.cia` a la SD.
2. Abre **FBI** → SD → selecciona `client.cia` → *Install and delete*.
3. Aparece "3DSStream" en el menú HOME. Lánzalo como cualquier juego.

(Luma3DS aplica los parches de firma por defecto, así que FBI instala el CIA sin más.)

## Por qué este RSF

[app.rsf](app.rsf) parte de la plantilla estándar de homebrew. Lo específico de
esta app es `ServiceAccessControl`, que **debe** incluir `soc:U` (sockets),
`ndm:u` y `ac:u` (WiFi) y `y2r:u` (conversión YUV→RGB). Si el CIA arranca pero
falla la red o el vídeo, ese bloque es lo primero a revisar.


cd /d/Proyectos/3DSStreaming/client

agregemos por ultimo que al presionar select + start salga un menu con 2 opciones, Configura y Salir, con configuracion agregaremos opciones y con salir saldra de la aplicacion 