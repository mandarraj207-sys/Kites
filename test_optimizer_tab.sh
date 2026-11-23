#!/usr/bin/env bash
# Build and test the Kites optimizer components, then launch the GUI for an
# optimizer-tab smoke test (Reordered Code + Analyzer).
#
# Usage:
#   ./test_optimizer_tab.sh [project-root] [--no-gui]
#
# If project-root is omitted, the script assumes the current directory is the
# Kites repository root.

set -Eeuo pipefail

PROJECT_ROOT="$(pwd)"
NO_GUI=0

for arg in "$@"; do
    case "$arg" in
        --no-gui) NO_GUI=1 ;;
        *)
            if [[ -d "$arg" ]]; then
                PROJECT_ROOT="$(cd "$arg" && pwd)"
            else
                echo "ERROR: project directory does not exist: $arg" >&2
                exit 2
            fi
            ;;
    esac
done

if [[ ! -f "$PROJECT_ROOT/CMakeLists.txt" ]]; then
    echo "ERROR: CMakeLists.txt not found in $PROJECT_ROOT" >&2
    echo "Run this script from the Kites repository root, or pass the root path." >&2
    exit 2
fi

BUILD_DIR="${KITES_OPTIMIZER_BUILD_DIR:-$PROJECT_ROOT/build-optimizer-tests}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
TEST_FILTER='*InstructionSchedulerTest*:*ForwardingSchedulerTest*:*DependencyGraphTest*:*StallModelTest*'
SMOKE_DIR="$BUILD_DIR/optimizer_smoke"
SMOKE_ASM="$SMOKE_DIR/optimizer_smoke.asm"

mkdir -p "$SMOKE_DIR"

cat > "$SMOKE_ASM" <<'ASM'
.text
.globl main
main:
    # Load-use dependency: optimizer should try to place independent work in the gap.
    lw   x5, 0(x1)
    add  x6, x5, x2

    # Independent instructions provide scheduling opportunities.
    add  x7, x8, x9
    sub  x10, x11, x12
    xor  x14, x15, x16

    # More dependencies for the Analyzer (RAW/WAR/WAW).
    add  x13, x6, x7
    add  x13, x13, x10

    # Memory operation / control-flow boundaries.
    sw   x13, 4(x1)
    beq  x13, x0, done
    add  x17, x18, x19

done:
    addi x20, x0, 1
ASM

echo "============================================================"
echo " Kites - Optimizer Tab Test"
echo "============================================================"
echo "Project : $PROJECT_ROOT"
echo "Build   : $BUILD_DIR"
echo "Jobs    : $JOBS"
echo

echo "[1/4] Configuring test build..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_TESTS=ON \
    -DDISABLE_GUI=OFF

echo
echo "[2/4] Building optimizer test executable and GUI..."
cmake --build "$BUILD_DIR" --target kites_test Kites -j "$JOBS"

TEST_BIN="$BUILD_DIR/test/kites_test"
if [[ ! -x "$TEST_BIN" ]]; then
    echo "ERROR: expected test executable not found: $TEST_BIN" >&2
    exit 3
fi

echo
echo "[3/4] Running optimizer-only unit tests..."
echo "Filter: $TEST_FILTER"
"$TEST_BIN" --gtest_color=yes --gtest_filter="$TEST_FILTER"

echo
echo "[4/4] GUI smoke test instructions"
echo "------------------------------------------------------------"
echo "A sample assembly program was created at:"
echo "  $SMOKE_ASM"
echo
echo "In the Kites GUI:"
echo "  1. Open the sample .asm file in the Editor tab and assemble it."
echo "  2. Open Optimizer -> Reordered Code."
echo "  3. Generate Suggestions with 'No forwarding (baseline)'."
echo "  4. Verify both Original and Optimized instruction listings are visible."
echo "  5. Check moved-row colours; hover an instruction to check its movement link."
echo "  6. Toggle Show stalls and verify stall rows/counts appear in both listings."
echo "  7. Select 'With forwarding (H_F)' and generate suggestions again."
echo "  8. Open Optimizer -> Analyzer."
echo "  9. Verify instruction count and RAW/WAR/WAW dependency counts are populated."
echo " 10. Run 'Benchmark Original vs Optimized' if the benchmark action is enabled."
echo

echo "IMPORTANT: the automated GoogleTests cover scheduling, dependency-graph, and stall-model logic."
echo "The Analyzer's displayed metric labels and the Qt listing/overlay interactions require the GUI smoke test above; this script does not pretend to automate those UI assertions."
echo

if [[ "$NO_GUI" -eq 1 ]]; then
    echo "GUI launch skipped (--no-gui)."
    exit 0
fi

APP_BIN="$BUILD_DIR/Kites"
if [[ ! -x "$APP_BIN" ]]; then
    echo "ERROR: GUI executable not found: $APP_BIN" >&2
    exit 3
fi

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "No DISPLAY/WAYLAND_DISPLAY detected; not launching the GUI."
    echo "Run the GUI manually in a graphical session: $APP_BIN"
    exit 0
fi

echo "Launching Kites GUI..."
exec "$APP_BIN"
