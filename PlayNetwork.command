#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
started=$(date +%s)
export FO2_NET_TRACE=1
./Play.command "$@"
status=$?
printf '\nReplaying the captured OpenSpy master-list request...\n'
python3 scripts/replay_openspy.py --since "$started"
printf '\nGame exit code: %s\n' "$status"
read -r -p "Press Enter to close..." _answer
exit "$status"
