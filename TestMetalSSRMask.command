#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
export FO2_LAUNCH_PROFILE=ssr-mask
export FO2_DEBUG_VIEW=ssr-mask
export FO2_MEMORY_CHECK=0 FO2_METAL_VALIDATE_GAME=0 FO2_PARTICLE_CAPTURE_ENABLED=0 FO2_HUD_CAPTURE=0
unset FO2_TEST_LEVEL FO2_METAL_SESSION_DIR FO2_PARTICLE_CAPTURE FO2_HUD_TRACE FO2_METAL_BINARY
echo 'Material mask: brighter means stronger reflectivity; unclassified surfaces stay black; verified water is cyan.'
exec ./TestMetal.command "$@"
