#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
export FO2_TEST_SCENARIO=manual
export FO2_GAME_DIR="$PWD/gamedata"
unset FO2_TEST_LEVEL FO2_SCENARIO_STATUS FO2_AUTOPRESS NFSU2_KEY_SCRIPT FO2_AUTORESET NFSU2_AUTODRIVE
auto_race=0
if [[ "${1:-}" == --auto-race ]]; then auto_race=1; shift; fi
mkdir -p work/metal/shaders work/metal/captures
export FO2_LAUNCH_PROFILE="${FO2_LAUNCH_PROFILE:-check}"
export FO2_METAL_SESSION_DIR="${FO2_METAL_SESSION_DIR:-$PWD/work/metal/runs/$(date -u +%Y%m%dT%H%M%SZ)-$FO2_LAUNCH_PROFILE-$$}"
mkdir -p "$FO2_METAL_SESSION_DIR"
if [[ -f work/metal/check.log ]]; then cp work/metal/check.log work/metal/check.previous.log; fi
exec > >(tee work/metal/check.log "$FO2_METAL_SESSION_DIR/check.log") 2>&1
trap 'result=$?; if (( result != 0 )); then echo "Metal check failed ($result). Logs: $FO2_METAL_SESSION_DIR"; if [[ -t 0 ]]; then read -r -p "Press Enter to close…" || true; fi; fi' EXIT
echo "Metal test profile: $FO2_LAUNCH_PROFILE"
echo "Session logs: $FO2_METAL_SESSION_DIR"
if (( auto_race )); then
    export FO2_GAME_DIR="$(python3 scripts/prepare_test_scenario.py --source "$PWD/gamedata" --session "$FO2_METAL_SESSION_DIR" --seed "$PWD/work/metal/scenarios/rt-auto-race/seed")"
    export FO2_TEST_SCENARIO=auto-race FO2_TEST_LEVEL=water-derby
    export FO2_SCENARIO_STATUS="$FO2_METAL_SESSION_DIR/scenario-status.json"
    echo "Automatic Derby Bowl 2 (level 56). Isolated saves: $FO2_GAME_DIR/Savegame"
else
    echo "Manual selection. Saves: $FO2_GAME_DIR/Savegame"
fi
export MTL_DEBUG_LAYER=1
export MTL_SHADER_VALIDATION=1
export MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
export MTL_SHADER_VALIDATION_REPORT_ALLOCATION_STACK_TRACE=1
export MTL_SHADER_VALIDATION_ABORT_ON_FAULT=1
export FO2_SHADER_CAPTURE="$PWD/work/metal/shaders"
echo 'Checking native Metal rendering and GPU readback…'
./work/metal/fo2-metal --metal-check
if [[ "${FO2_RT_PREFLIGHT:-0}" == 1 ]]; then
    echo 'Checking the independent Metal ray tracing fixture…'
    if ./work/metal/fo2-metal --metal-check-raytracing; then
        echo 'Metal RT intersection diagnostic passed.'
    else
        result=$?
        if (( result == 77 )); then
            echo 'Metal RT diagnostic unavailable on this device/session; continuing with SSR.'
        else
            exit "$result"
        fi
    fi
fi
if [[ "${FO2_RT_WATER:-0}" == 1 ]]; then
    echo 'Checking same-frame RT water and offscreen textured car material…'
    if ./work/metal/fo2-metal --metal-check-raytracing-water; then
        echo 'RT water fixture passed; experimental synchronous mode enabled.'
    else
        result=$?
        if (( result == 77 )); then
            export FO2_RT_WATER=0
            echo 'RT water unavailable; continuing with SSR.'
        else
            exit "$result"
        fi
    fi
fi
if [[ "${FO2_RT_SCENE_CHECK:-0}" == 1 ]]; then
    if [[ -z "${FO2_RT_SCENE_DIR:-}" ]]; then
        FO2_RT_SCENE_DIR="$(python3 - <<'PY'
from pathlib import Path
scenes = list(Path('work/metal/runs').glob('*/rt-view-scene/scene.json'))
print(max(scenes, key=lambda p: p.stat().st_mtime).parent.resolve() if scenes else '')
PY
)"
    fi
    if [[ -n "$FO2_RT_SCENE_DIR" ]]; then
        echo "Checking captured geometry with Metal RT: $FO2_RT_SCENE_DIR"
        if ./work/metal/fo2-metal --metal-check-raytracing-scene "$FO2_RT_SCENE_DIR"; then
            echo 'Captured RT scene passed; images/report are in this session. Gameplay still uses SSR.'
        else
            result=$?
            if (( result == 77 )); then
                echo 'Captured RT scene unavailable on this device/session; continuing with SSR.'
            else
                exit "$result"
            fi
        fi
    else
        echo 'No prepared RT snapshot found; captured-scene diagnostic skipped.'
    fi
fi
echo 'Compiling HLSL fixtures through the lifted D3DX…'
FO2_TEST_LEVEL= FO2_TEST_SCENARIO=manual ./work/metal/fo2-metal "$PWD/gamedata" --check-runtime
echo 'Compiling captured shaders on the Metal device…'
./work/metal/fo2-metal --metal-compile-shaders "$FO2_SHADER_CAPTURE"
# HUD diagnosis is deferred; keep it available as an explicit opt-in.
if [[ "${FO2_HUD_CAPTURE:-0}" == 1 ]]; then
    export FO2_HUD_TRACE="${FO2_HUD_TRACE:-$PWD/work/metal/hud-trace.log}"
    if [[ -f "$FO2_HUD_TRACE" ]]; then cp "$FO2_HUD_TRACE" work/metal/hud-trace.previous.log; fi
    echo 'HUD fade trace enabled: work/metal/hud-trace.log'
else
    unset FO2_HUD_TRACE
fi
# Select exactly the requested executable. ASan never silently falls back.
export FO2_METAL_BINARY="$PWD/work/metal/fo2-metal"
if [[ "${FO2_MEMORY_CHECK:-0}" == 1 ]]; then
    if [[ ! -x work/metal/fo2-metal-asan || ! work/metal/asan.ready -nt work/metal/fo2-metal-asan ]]; then
        echo 'ASan profile unavailable: build and validate the diagnostic executable first.'
        exit 1
    fi
    export FO2_METAL_BINARY="$PWD/work/metal/fo2-metal-asan"
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1}"
    export ASAN_SYMBOLIZER_PATH="${ASAN_SYMBOLIZER_PATH:-/opt/homebrew/opt/llvm/bin/llvm-symbolizer}"
    echo 'Host memory diagnostics enabled.'
fi
echo 'Checking resource loading and upload memory boundaries…'
"${FO2_METAL_BINARY:-./work/metal/fo2-metal}" --metal-check-memory
if [[ "${FO2_PARTICLE_CAPTURE_ENABLED:-0}" == 1 ]]; then
    export FO2_PARTICLE_CAPTURE="${FO2_PARTICLE_CAPTURE:-$FO2_METAL_SESSION_DIR/particles}"
    mkdir -p "$FO2_PARTICLE_CAPTURE"
else
    unset FO2_PARTICLE_CAPTURE
fi
export FO2_DEBUG_VIEW="${FO2_DEBUG_VIEW:-off}"
export FO2_DEBUG_STATUS="$FO2_METAL_SESSION_DIR/debug-view.json"
export FO2_SCREENSHOT_DIR="$FO2_METAL_SESSION_DIR/captures"

echo 'Checks passed. Starting FlatOut 2 with native Metal…'
if [[ "${FO2_METAL_VALIDATE_GAME:-0}" == 1 ]]; then
    ./PlayMetal.command "$@"
else
    echo 'Starting gameplay without validation instrumentation (preflight above used validation).'
    MTL_DEBUG_LAYER=0 MTL_SHADER_VALIDATION=0 MTL_SHADER_VALIDATION_ABORT_ON_FAULT=0 ./PlayMetal.command "$@"
fi
