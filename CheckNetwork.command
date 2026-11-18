#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
python3 scripts/check_network_macos.py --openspy
status=$?
printf '\nNetwork check exit code: %s\n' "$status"
read -r -p "Press Enter to close..." _answer
exit "$status"
