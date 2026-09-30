#!/usr/bin/env bash
# ADOCAO 构建脚本（macOS / Linux）
#   build.sh                 首次构建走交互式完整流程；
#                            若已有 build/ 缓存且不带参数 → 自动增量构建（不提问、不重新 configure）
#   build.sh -U -Portable    带参数：跳过交互，按指定选项重新 configure 后构建
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── Colour helpers ──────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; DIM='\033[2m';  NC='\033[0m'

# ── Compiler detection ──────────────────────────────────────────
detect_compiler() {
    if command -v g++ &>/dev/null && command -v gcc &>/dev/null; then
        CXX="$(command -v g++)"; CC="$(command -v gcc)"
        CXX_KIND="G++ (GCC)"
    elif command -v clang++ &>/dev/null && command -v clang &>/dev/null; then
        CXX="$(command -v clang++)"; CC="$(command -v clang)"
        CXX_KIND="Clang++"
    else
        CXX_KIND=""
    fi
}

# ── Prompt helper ───────────────────────────────────────────────
prompt_bool() {
    local label="$1" default="${2:-N}"
    local yn="y/N"
    [[ "$default" == "Y" ]] && yn="Y/n"
    read -r -p "  $label ($yn) " reply
    [[ -z "$reply" ]] && reply="$default"
    [[ "$reply" =~ ^[Yy] ]]
}

# ── Argument parsing ────────────────────────────────────────────
HAS_ARGS=false
PORTABLE=OFF; ZOOM_LEVEL="Ultra"

for arg in "$@"; do
    HAS_ARGS=true
    case "${arg}" in
        -Portable|-P)   PORTABLE=ON ;;
        -Normal|-N)         ZOOM_LEVEL="Normal" ;;
        -Extra|-T)          ZOOM_LEVEL="Extra" ;;
        -Super|-S)          ZOOM_LEVEL="Super" ;;
        -Ultra|-U)          ZOOM_LEVEL="Ultra" ;;
        -Hyper|-H)          ZOOM_LEVEL="Hyper" ;;
        -Extreme|-X)        ZOOM_LEVEL="Extreme" ;;
        -Unimaginable|-I)   ZOOM_LEVEL="Unimaginable" ;;
        *) echo "Unknown: $arg"; exit 1 ;;
    esac
done

# ── Incremental mode：已有缓存且无参数 → 跳过提问/configure，直接增量编译 ──
INCREMENTAL=false
if [ "$HAS_ARGS" = false ] && [ -f build/CMakeCache.txt ]; then
    INCREMENTAL=true
fi

VERSION="$(sed -nE 's/project\(ADOCAO VERSION ([0-9.]+).*/\1/p' CMakeLists.txt | head -1)"
[ -n "$VERSION" ] || VERSION="?"

# ── Banner ──────────────────────────────────────────────────────
echo ""
echo -e "${CYAN}╔══════════════════════════════════════════╗"
echo -e "${CYAN}║      ADOCAO v${VERSION} — Build Script      ║"
echo -e "${CYAN}╚══════════════════════════════════════════╝"
echo ""

mkdir -p build
cd build

if [ "$INCREMENTAL" = false ]; then
    # ── 完整流程：编译器检测 / 交互提问 / configure ──
    detect_compiler
    if [[ -z "$CXX_KIND" ]]; then
        echo -e "${RED}ERROR: No compiler found (g++/gcc or clang++/clang).${NC}"
        exit 1
    fi
    echo -ne "Compiler detected: "; echo -e "${GREEN}$CXX_KIND${NC}"
    echo -e "${DIM}  CC  = $CC${NC}"
    echo -e "${DIM}  CXX = $CXX${NC}"

    if [ "$HAS_ARGS" = false ]; then
        echo ""
        echo -e "${YELLOW}Build options (press Enter for default):${NC}"
        prompt_bool "  Static-linked portable build?"  "N" && PORTABLE=ON
        echo "  Zoom level: Normal(10) Extra(5) Super(2.5) Ultra(1) Hyper(0.5) Extreme(0.25) Unimaginable(0.1)"
        read -r -p "  Level [Ultra]: " zoom_in
        case "${zoom_in}" in
            Normal|N)           ZOOM_LEVEL="Normal" ;;
            Extra|T)            ZOOM_LEVEL="Extra" ;;
            Super|S)            ZOOM_LEVEL="Super" ;;
            Ultra|U|"")         ZOOM_LEVEL="Ultra" ;;
            Hyper|H)            ZOOM_LEVEL="Hyper" ;;
            Extreme|X)          ZOOM_LEVEL="Extreme" ;;
            Unimaginable|I)     ZOOM_LEVEL="Unimaginable" ;;
        esac
    fi

    bool_str() {
        [[ "$1" == "ON" ]] && echo -e "${GREEN}ON${NC}" || echo -e "${DIM}OFF${NC}"
    }

    echo ""
    echo -e "${YELLOW}Configuration:${NC}"
    echo -ne "  Portable:     "; bool_str "$PORTABLE"
    echo -e  "  Zoom Level:   ${GREEN}${ZOOM_LEVEL}${NC}"
    echo ""

    echo -e "${CYAN}Configuring...${NC}"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DADOCAO_PORTABLE="$PORTABLE" \
        -DADOCAO_ZOOM_LEVEL="$ZOOM_LEVEL"
    echo -e "${DIM}  OK${NC}"
else
    echo -e "${DIM}增量构建：复用现有 build/ 配置（改选项请带参数，如 -U / -Portable）${NC}"
    echo ""
fi

# ── CMake build（公共，增量安全）───────────────────────────────
echo -e "${CYAN}Building...${NC}"
if ! cmake --build . --parallel "$(nproc 2>/dev/null || echo 4)" 2>&1 | while IFS= read -r line; do
    if [[ "$line" =~ \[[[:space:]]*([0-9]+)%\] ]]; then
        pct="${BASH_REMATCH[1]}"
        if (( pct % 10 == 0 )); then
            echo -e "${GREEN}$line${NC}"
        fi
    elif [[ "$line" =~ [Ee]rror|fatal ]]; then
        echo -e "${RED}$line${NC}"
    elif [[ "$line" =~ warning ]]; then
        echo -e "${YELLOW}$line${NC}"
    fi
done; then
    echo ""
    echo -e "${RED}Build failed.${NC}" >&2
    exit 1
fi

# ── Done ─────────────────────────────────────────────────────────
# Only one of ADOCAO/ADOCAO.exe exists on a given platform; `ls` exits
# non-zero when the other is missing, and pipefail would poison this
# assignment (set -e aborts before the banner).
OUT="$(ls ADOCAO ADOCAO.exe 2>/dev/null | head -1 || true)"
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════╗"
echo -e "${GREEN}║            Build successful!             ║"
echo -e "${GREEN}╚══════════════════════════════════════════╝"
echo ""
echo -ne "  "; echo -e "${GREEN}$(realpath "${OUT:-ADOCAO}" 2>/dev/null || echo "$PWD/ADOCAO")${NC}"
echo ""
