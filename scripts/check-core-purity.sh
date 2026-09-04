#!/usr/bin/env bash
# Verify adocao_core never pulls in GL/window/UI/audio-device/platform code.
# core/ is the pure-logic layer (level parsing, timeline, util) that may later
# be exported; banned includes are enforced here and in CI.
# Usage: scripts/check-core-purity.sh
set -euo pipefail

cd "$(dirname "$0")/.."

BANNED='#include[<"](glad|GLFW|imgui|miniaudio|tinyfiledialogs|windows\.h|winuser\.h|X11|unistd\.h|mach-o)'

bad="$(grep -rEn "$BANNED" core/ || true)"
if [ -n "$bad" ]; then
    echo "core purity check FAILED — banned includes in core/:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "core purity OK: no GL/window/UI/platform includes in core/"
