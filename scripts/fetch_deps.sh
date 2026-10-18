#!/usr/bin/env bash
# Downloads third-party dependencies (Dear ImGui). Only needs to run once.
set -euo pipefail
cd "$(dirname "$0")/.."
IMGUI_TAG="v1.92.9b"
if [ ! -f third_party/imgui/imgui.h ]; then
    echo ">> Downloading Dear ImGui ${IMGUI_TAG} ..."
    rm -rf third_party/imgui
    git clone --depth 1 --branch "${IMGUI_TAG}" https://github.com/ocornut/imgui.git third_party/imgui
    rm -rf third_party/imgui/.git
else
    echo ">> Dear ImGui already present, skipping"
fi
