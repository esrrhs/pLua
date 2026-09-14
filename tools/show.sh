#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INPUT="${1:-$SCRIPT_DIR/../test}"

if [ ! -d "$INPUT" ]; then
    echo "Error: Directory '$INPUT' does not exist."
    echo "Usage: $0 [path_to_profiles_dir]"
    exit 1
fi

shopt -s nullglob
PRO_FILES=("$INPUT"/*.pro)
shopt -u nullglob

if [ ${#PRO_FILES[@]} -eq 0 ]; then
    echo "No .pro profile files found in $INPUT."
    exit 0
fi

# Locate or build plua binary
PLUA_BIN=""
if [ -x "$SCRIPT_DIR/plua" ]; then
    PLUA_BIN="$SCRIPT_DIR/plua"
elif [ -x "$SCRIPT_DIR/../bin/plua" ]; then
    PLUA_BIN="$SCRIPT_DIR/../bin/plua"
elif command -v plua >/dev/null 2>&1; then
    PLUA_BIN="plua"
elif command -v go >/dev/null 2>&1; then
    echo "Compiling plua converter tool..."
    (cd "$SCRIPT_DIR" && go build -o plua plua.go)
    PLUA_BIN="$SCRIPT_DIR/plua"
else
    echo "Error: plua executable not found and 'go' is not installed to compile it."
    exit 1
fi

# Locate or download flamegraph.pl
FLAMEGRAPH_BIN=""
if command -v flamegraph.pl >/dev/null 2>&1; then
    FLAMEGRAPH_BIN="flamegraph.pl"
elif [ -x "$SCRIPT_DIR/flamegraph.pl" ]; then
    FLAMEGRAPH_BIN="$SCRIPT_DIR/flamegraph.pl"
else
    echo "flamegraph.pl not found. Attempting to download from upstream repository..."
    FLAMEGRAPH_URL="https://raw.githubusercontent.com/brendangregg/FlameGraph/master/flamegraph.pl"
    if command -v curl >/dev/null 2>&1; then
        curl -sSL "$FLAMEGRAPH_URL" -o "$SCRIPT_DIR/flamegraph.pl" && chmod +x "$SCRIPT_DIR/flamegraph.pl" || true
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$FLAMEGRAPH_URL" -O "$SCRIPT_DIR/flamegraph.pl" && chmod +x "$SCRIPT_DIR/flamegraph.pl" || true
    fi

    if [ -x "$SCRIPT_DIR/flamegraph.pl" ]; then
        FLAMEGRAPH_BIN="$SCRIPT_DIR/flamegraph.pl"
        echo "Successfully downloaded flamegraph.pl"
    else
        echo "Warning: could not download flamegraph.pl. Flamegraph SVG generation will be skipped."
    fi
fi

# Locate pprof
PPROF_BIN=""
if [ -x "$SCRIPT_DIR/pprof" ]; then
    PPROF_BIN="$SCRIPT_DIR/pprof"
elif command -v pprof >/dev/null 2>&1; then
    PPROF_BIN="pprof"
elif command -v google-pprof >/dev/null 2>&1; then
    PPROF_BIN="google-pprof"
else
    echo "Tip: pprof is not installed. Callgraph dot/png generation will be skipped."
    echo "     Install gperftools/google-perftools to enable callgraph visualization."
fi

# Check graphviz dot
HAS_DOT=0
if command -v dot >/dev/null 2>&1; then
    HAS_DOT=1
fi

for PRO_PATH in "${PRO_FILES[@]}"; do
    BASE_NAME="${PRO_PATH%.pro}"
    echo "Processing profile: $PRO_PATH"

    # 1. Convert .pro to flamegraph collapsed stacks
    "$PLUA_BIN" -i "$PRO_PATH" -flame "$BASE_NAME.fl" -pprof "$BASE_NAME.prof"

    # 2. Generate Flamegraph SVG
    if [ -n "$FLAMEGRAPH_BIN" ]; then
        if perl "$FLAMEGRAPH_BIN" "$BASE_NAME.fl" > "$BASE_NAME.svg" 2>/dev/null; then
            echo "  -> Generated flamegraph: $BASE_NAME.svg"
        else
            echo "  -> Warning: failed to generate $BASE_NAME.svg (check perl-open module)"
        fi
    fi

    # 3. Generate call graph (.dot and .png) if pprof is available
    if [ -n "$PPROF_BIN" ]; then
        if "$PPROF_BIN" --dot "$BASE_NAME.prof" > "$BASE_NAME.dot" 2>/dev/null; then
            echo "  -> Generated dot file: $BASE_NAME.dot"
            if [ $HAS_DOT -eq 1 ]; then
                dot -Tpng "$BASE_NAME.dot" -o "$BASE_NAME.png"
                echo "  -> Generated call graph image: $BASE_NAME.png"
            else
                echo "  -> Note: 'dot' command not found, skip PNG conversion (install graphviz)"
            fi
        else
            echo "  -> Warning: pprof failed to generate dot graph"
        fi
    fi
done

echo "Done processing all profiles!"
