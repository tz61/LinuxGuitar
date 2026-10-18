#!/usr/bin/env bash
# Builds (if needed) and starts Guitar Studio. Arguments are passed to the program, e.g.: ./run.sh my_song.gp5
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
if [ ! -f "${ROOT}/third_party/imgui/imgui.h" ]; then "${ROOT}/scripts/fetch_deps.sh"; fi
cmake -S "${ROOT}" -B "${ROOT}/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${ROOT}/build" --target guitarstudio -j"$(nproc)" | grep -E "error|warning|Linking" || true
exec "${ROOT}/build/guitarstudio" "$@"
