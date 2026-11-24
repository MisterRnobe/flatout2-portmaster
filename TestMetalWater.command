#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
export FO2_NET_TRACE=1
export FO2_STARTUP_TRACE=1
export FO2_LAUNCH_PROFILE=water
export FO2_DEBUG_VIEW=ssr
export FO2_RT_PREFLIGHT=0 FO2_RT_GEOMETRY_CAPTURE=0
export FO2_RT_SCENE_CHECK=1
export FO2_RT_LIVE_DIAGNOSTIC=1
export FO2_RT_WATER=0
while (( $# )); do
    case "$1" in
        --rt-water) export FO2_RT_WATER=1 FO2_RT_LIVE_DIAGNOSTIC=0 FO2_RT_PREFLIGHT=1 FO2_LAUNCH_PROFILE=water-rt; shift ;;
        --mask) export FO2_LAUNCH_PROFILE=water-mask FO2_DEBUG_VIEW=ssr-mask; shift ;;
        --rt-check) export FO2_RT_PREFLIGHT=1; shift ;;
        --rt-capture) export FO2_RT_GEOMETRY_CAPTURE=1; shift ;;
        --no-rt-scene) export FO2_RT_SCENE_CHECK=0; shift ;;
        --no-rt-live) export FO2_RT_LIVE_DIAGNOSTIC=0; shift ;;
        --rt-scene)
            if (( $# < 2 )); then echo '--rt-scene requires a prepared scene directory' >&2; exit 2; fi
            export FO2_RT_SCENE_DIR="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
export FO2_TEST_SCENARIO=manual FO2_SSR_MATERIALS=1
export FO2_MEMORY_CHECK=0 FO2_METAL_VALIDATE_GAME=0 FO2_PARTICLE_CAPTURE_ENABLED=0 FO2_HUD_CAPTURE=0
unset FO2_TEST_LEVEL FO2_GAME_DIR FO2_SCENARIO_STATUS FO2_METAL_SESSION_DIR FO2_PARTICLE_CAPTURE FO2_HUD_TRACE FO2_METAL_BINARY
unset FO2_WATER_PHYSICS_SIGNATURES
# Prepare read-only physical-water identities once; optional diagnostics never
# block gameplay if the user's archive has a different layout.
if [[ "${FO2_WATER_PHYSICS_TRACE:-1}" == 1 && -f gamedata/data.bfs ]]; then
    catalog=work/metal/water-physics-research/catalog
    if [[ ! -s "$catalog/signatures.tsv" ]]; then
        if ! python3 scripts/research_water_physics.py --output "$catalog"; then
            echo 'Physical water catalog unavailable; continuing without semantic tracing.' >&2
        fi
    fi
    if [[ -s "$catalog/signatures.tsv" ]]; then
        export FO2_WATER_PHYSICS_SIGNATURES="$PWD/$catalog/signatures.tsv"
    fi
else
    unset FO2_WATER_PHYSICS_SIGNATURES
fi
exec ./TestMetal.command "$@"
