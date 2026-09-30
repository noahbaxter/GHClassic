#!/usr/bin/env bash
# Build each platform's icon from assets/icon.png (square, full bleed, which
# is what Windows and Linux show as is).
#
#   tools/make_icons.sh
#
# macOS draws an app's icon as given, so the shape is ours to make: Apple's
# macOS 11 grid, a 824x824 body with 185.4 corners centred on a 1024 canvas,
# over a black shadow at 50% opacity, 28 blur, 12 down.
# Needs ImageMagick 7.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/assets/icon.png"
OUT="$ROOT/assets/icon_macos.png"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

magick "$SRC" -resize 824x824! \
  \( -size 824x824 xc:black -fill white -draw "roundrectangle 0,0 823,823 185.4,185.4" \) \
  -alpha off -compose CopyOpacity -composite "$TMP/body.png"

# A 28 blur is a Gaussian of sigma 14.
magick -size 1024x1024 xc:none \
  -fill 'rgba(0,0,0,0.5)' -draw "roundrectangle 100,112 923,935 185.4,185.4" -blur 0x14 \
  "$TMP/body.png" -geometry +100+100 -compose Over -composite \
  -depth 8 "$OUT"
echo "wrote $OUT"

# The app bundle's icon, which Finder and other apps read (the Dock's comes
# from the window at runtime): every size iconutil wants, from the same art.
ICNS="$ROOT/assets/icon.icns"
mkdir "$TMP/icon.iconset"
for size in 16 32 128 256 512; do
  magick "$OUT" -resize "${size}x${size}" "$TMP/icon.iconset/icon_${size}x${size}.png"
  magick "$OUT" -resize "$((size * 2))x$((size * 2))" "$TMP/icon.iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$TMP/icon.iconset" -o "$ICNS"
echo "wrote $ICNS"
