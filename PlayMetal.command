#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p work/metal/shaders work/metal/captures
export FO2_LAUNCH_PROFILE="${FO2_LAUNCH_PROFILE:-play}"
export FO2_METAL_SESSION_DIR="${FO2_METAL_SESSION_DIR:-$PWD/work/metal/runs/$(date -u +%Y%m%dT%H%M%SZ)-$FO2_LAUNCH_PROFILE-$$}"
export MTL_DEBUG_LAYER="${MTL_DEBUG_LAYER:-0}"
export MTL_SHADER_VALIDATION="${MTL_SHADER_VALIDATION:-0}"
mkdir -p "$FO2_METAL_SESSION_DIR"
if [[ -n "${FO2_NET_TRACE:-}" && "${FO2_NET_TRACE:-0}" != 0 ]]; then
    export FO2_NET_TRACE_DIR="$FO2_METAL_SESSION_DIR/network-trace"
    mkdir -p "$FO2_NET_TRACE_DIR"
else
    unset FO2_NET_TRACE_DIR
fi
export NFSU2_SCREEN="${NFSU2_SCREEN:-1280x800}"
export FO2_RENDER="${FO2_RENDER:-1280x800}"
export FO2_SKIP_LOGO="${FO2_SKIP_LOGO:-1}"
export FO2_SKIP_INTRO="${FO2_SKIP_INTRO:-1}"
export NFSU2_LANG="${NFSU2_LANG:-Russian}"
export NFSU2_SHOW_FPS="${NFSU2_SHOW_FPS:-1}"
export FO2_SHADER_CAPTURE="${FO2_SHADER_CAPTURE:-$PWD/work/metal/shaders}"
# Use the same dimensions as the device mode advertised by the runtime.
python3 - <<'PYCFG'
import os, re, struct
from pathlib import Path
p=Path(os.environ.get('FO2_GAME_DIR','gamedata'))/'Savegame/device.cfg'
size=re.fullmatch(r'(\d+)x(\d+)',os.environ['FO2_RENDER'])
if p.exists() and size:
    w,h=map(int,size.groups());data=bytearray(p.read_bytes())
    if len(data)>=132 and 320<=w<=8192 and 240<=h<=8192:
        struct.pack_into('<III',data,120,w,h,32);p.write_bytes(data)
PYCFG
if [[ -f work/metal/play.log ]]; then cp work/metal/play.log work/metal/play.previous.log; fi
archive_failed_run() {
    local result=$?
    if (( result != 0 )) && [[ -f "$FO2_METAL_SESSION_DIR/play.log" ]]; then
        mkdir -p work/metal/failures
        local archived="work/metal/failures/play-$(date -u +%Y%m%dT%H%M%SZ)-$$.log"
        cp "$FO2_METAL_SESSION_DIR/play.log" "$archived"
        echo "Failed run archived: $archived"
    fi
    if [[ "${FO2_STARTUP_TRACE:-0}" == 1 && -f "$FO2_METAL_SESSION_DIR/play.log" ]]; then
        python3 scripts/summarize_startup.py "$FO2_METAL_SESSION_DIR" --output "$FO2_METAL_SESSION_DIR/startup-summary.json" || echo 'Startup summary unavailable; raw play.log retained.'
    fi
}
trap archive_failed_run EXIT
binary="${FO2_METAL_BINARY:-$PWD/work/metal/fo2-metal}"
printf 'profile=%s\nbinary=%s\napi_validation=%s\nshader_validation=%s\nparticle_capture=%s\ngame_dir=%s\nscenario=%s\ndebug_view=%s\n' \
    "$FO2_LAUNCH_PROFILE" "$binary" "$MTL_DEBUG_LAYER" "$MTL_SHADER_VALIDATION" \
    "${FO2_PARTICLE_CAPTURE:-off}" "${FO2_GAME_DIR:-$PWD/gamedata}" \
    "${FO2_TEST_SCENARIO:-manual}" "${FO2_DEBUG_VIEW:-off}" > "$FO2_METAL_SESSION_DIR/run-info.txt"
printf 'test_level=%s\n' "${FO2_TEST_LEVEL:-default}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
printf 'ssr_materials=%s\n' "${FO2_SSR_MATERIALS:-1}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
printf 'water_legacy_replacement=%s\n' "${FO2_WATER_REPLACE_REFLECTION:-0}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
printf 'water_reflection_gain=%s\n' "${FO2_WATER_REFLECTION_GAIN:-1}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
printf 'rt_geometry_capture=%s\n' "${FO2_RT_GEOMETRY_CAPTURE:-0}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
printf 'rt_live_diagnostic=%s\n' "${FO2_RT_LIVE_DIAGNOSTIC:-0}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
if [[ "${FO2_RT_LIVE_DIAGNOSTIC:-0}" == 1 ]]; then printf 'rt_live_mode=async-periodic-single-flight\nrt_geometry_prepare=deferred-mesh-cache-v1\n' >> "$FO2_METAL_SESSION_DIR/run-info.txt"; fi
printf 'network_trace=%s\n' "${FO2_NET_TRACE_DIR:-off}" >> "$FO2_METAL_SESSION_DIR/run-info.txt"
{
    echo "Metal profile: $FO2_LAUNCH_PROFILE"
    echo "Executable: $binary"
    echo "API validation: $MTL_DEBUG_LAYER; GPU validation: $MTL_SHADER_VALIDATION"
    echo "Session logs: $FO2_METAL_SESSION_DIR"
    "$binary" "${FO2_GAME_DIR:-$PWD/gamedata}" --windowed --cap60 "$@"
} 2>&1 | tee work/metal/play.log "$FO2_METAL_SESSION_DIR/play.log"
