#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# Reuse the same RT preflight, logging and bounded menu navigator as the
# manual water test. Direct Derby Bowl 2 startup only runs in isolated data.
exec ./TestMetalWater.command --rt-water --auto-race "$@"
