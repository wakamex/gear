#!/usr/bin/env bash
# Makes the program's icons from gear-icon.svg: gear.ico, built into the Windows program.
set -euo pipefail
here=$(dirname "$0")
command -v magick >/dev/null || { echo "error: ImageMagick's magick is needed to make the icons" >&2; exit 1; }
magick -background none -density 384 "$here/gear-icon.svg" -resize 256x256 \
  -define icon:auto-resize=256,128,64,48,32,24,16 -strip "$here/gear.ico"
