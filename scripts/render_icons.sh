#!/usr/bin/env bash
# Renders the PNG icon sizes from the SVG. Run it after editing packaging/icons/hicolor/scalable/apps/guitarstudio.svg
# (the PNGs are committed; the 128 px one is also built into the program as its window icon).
# Uses rsvg-convert (sudo apt install librsvg2-bin), ksvgtopng5 (KDE) or Inkscape.
set -euo pipefail
cd "$(dirname "$0")/../packaging/icons/hicolor"
SVG=scalable/apps/guitarstudio.svg
for size in 16 22 24 32 48 64 128 256; do
    out="${size}x${size}/apps/guitarstudio.png"
    mkdir -p "$(dirname "${out}")"
    if command -v rsvg-convert > /dev/null; then
        rsvg-convert -w "${size}" -h "${size}" -o "${out}" "${SVG}"
    elif command -v ksvgtopng5 > /dev/null; then
        QT_QPA_PLATFORM=offscreen ksvgtopng5 "${size}" "${size}" "${SVG}" "${out}"
    elif command -v inkscape > /dev/null; then
        inkscape -w "${size}" -h "${size}" -o "${out}" "${SVG}"
    else
        echo "Needs rsvg-convert (sudo apt install librsvg2-bin), ksvgtopng5 or inkscape" >&2
        exit 1
    fi
    echo "${out}"
done
