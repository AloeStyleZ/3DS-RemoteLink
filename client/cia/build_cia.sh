#!/usr/bin/env bash
# Genera client.cia a partir del build de `make`.
# Uso (desde cualquier sitio):  client/cia/build_cia.sh
# Requiere 'makerom' y 'bannertool' en el PATH (releases de Steveice10 en GitHub).
set -euo pipefail

# Situarse en client/ (la carpeta padre de este script).
cd "$(dirname "$0")/.."

APP=client
CIA_DIR=cia

TITLE="3DSStream"
DESC="PC to 3DS streaming client"
AUTHOR="3DSStreaming"

# --- comprobaciones ---
command -v makerom    >/dev/null 2>&1 || { echo "ERROR: 'makerom' no esta en el PATH."; exit 1; }
command -v bannertool >/dev/null 2>&1 || { echo "ERROR: 'bannertool' no esta en el PATH."; exit 1; }
[ -f "$APP.elf" ] || { echo "ERROR: falta $APP.elf -> ejecuta 'make' primero."; exit 1; }

missing=0
for f in icon.png banner.png banner.wav; do
  if [ ! -f "$CIA_DIR/$f" ]; then echo "ERROR: falta $CIA_DIR/$f"; missing=1; fi
done
[ "$missing" -eq 0 ] || { echo "Faltan assets. Ver $CIA_DIR/README.md"; exit 1; }

# --- 1) SMDH (icono + textos del menu HOME) ---
echo "[1/3] SMDH..."
bannertool makesmdh -s "$TITLE" -l "$DESC" -p "$AUTHOR" \
  -i "$CIA_DIR/icon.png" -o "$APP.smdh"

# --- 2) Banner (imagen 256x128 + audio) ---
echo "[2/3] Banner..."
bannertool makebanner -i "$CIA_DIR/banner.png" -a "$CIA_DIR/banner.wav" -o "$APP.bnr"

# --- 3) CIA ---
echo "[3/3] CIA..."
makerom -f cia -o "$APP.cia" -rsf "$CIA_DIR/app.rsf" -exefslogo \
  -elf "$APP.elf" -icon "$APP.smdh" -banner "$APP.bnr"

echo
echo "OK -> $(pwd)/$APP.cia"
echo "Instalalo con FBI (CFW Luma3DS con parches de firma activos, que vienen por defecto)."
