#!/usr/bin/env bash
# Builds the app and adds a "Guitar Studio" launcher to the application menu (current user only, no sudo needed).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"${ROOT}/scripts/fetch_deps.sh"
cmake -S "${ROOT}" -B "${ROOT}/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${ROOT}/build" --target guitarstudio -j"$(nproc)"
mkdir -p "${HOME}/.local/share/applications" "${HOME}/.local/share/icons/hicolor/scalable/apps"
cp "${ROOT}/assets/guitarstudio.svg" "${HOME}/.local/share/icons/hicolor/scalable/apps/guitarstudio.svg"
cat > "${HOME}/.local/share/applications/guitarstudio.desktop" << DESKTOP
[Desktop Entry]
Type=Application
Name=Guitar Studio
Name[zh_CN]=Guitar Studio 吉他效果器
Comment=Low-latency guitar effects and practice
Comment[zh_CN]=低延迟吉他效果器 + 练琴 (谱子跟弹、音准节奏评分)
Exec=${ROOT}/build/guitarstudio %f
Icon=guitarstudio
Terminal=false
Categories=AudioVideo;Audio;Music;Education;
MimeType=application/x-guitar-pro;audio/midi;application/vnd.recordare.musicxml+xml;
DESKTOP
update-desktop-database "${HOME}/.local/share/applications" 2> /dev/null || true
echo ">> Installed: search for \"Guitar Studio\" in the application menu to start it"
