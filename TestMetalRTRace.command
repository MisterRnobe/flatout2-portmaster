#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# Reuse the same RT preflight, logging and bounded menu navigator as the
# manual water test. Direct Derby Bowl 2 startup only runs in isolated data.
export FO2_WATER_REFLECTION_GAIN="${FO2_WATER_REFLECTION_GAIN:-4}"
echo "Puddle reflection visibility gain: $FO2_WATER_REFLECTION_GAIN (1 = original strength)"
exec ./TestMetalWater.command --rt-water --auto-race "$@"
