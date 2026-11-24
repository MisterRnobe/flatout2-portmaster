#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
ios_destination="${1:-$HOME/IdeaProjects/flatout2-ios}"
ios_prepared="$PWD/work/ios-handoff/flatout2-ios"
if [[ -e "$ios_destination" ]]; then
    echo "Destination already exists; nothing changed: $ios_destination" >&2
    exit 1
fi
mkdir -p "$(dirname "$ios_destination")"
if [[ -d "$ios_prepared/.git" ]]; then
    # Independent APFS copy-on-write files, including private generated inputs.
    cp -cRp "$ios_prepared" "$ios_destination"
else
    git clone --branch ios-port https://github.com/MisterRnobe/flatout2-portmaster.git "$ios_destination"
    echo 'Source-only clone: provide private game data and generated inputs as described in IOS_HANDOFF.md.'
fi
echo "iOS project prepared: $ios_destination"
echo 'Read IOS_HANDOFF.md and start the other agent in this directory.'
