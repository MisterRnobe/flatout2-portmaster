#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# Reuse the same RT preflight, logging and bounded menu navigator as the
# manual water test. The level override only runs against isolated game data.
exec ./TestMetalWater.command --rt-water --auto-race "$@"
