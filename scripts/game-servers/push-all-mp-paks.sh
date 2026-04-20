#!/usr/bin/env bash
# Push multiplayer download-packs for all four games to a retro agent.
# Each of the four underlying push scripts is independently usable; this is
# just a convenience wrapper that iterates through the full set so a freshly
# provisioned machine gets every bundle in one call.
#
# Usage:
#   push-all-mp-paks.sh 192.168.1.143 [192.168.1.133 ...]

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <agent-ip> [<agent-ip> ...]" >&2
    exit 1
fi

HERE="$(dirname "$(readlink -f "$0")")"

for target in "$@"; do
    echo
    echo "================ $target ================"
    for script in \
        push-q3-mp-paks.py \
        push-ut99-mp-paks.py \
        push-ut2004-mp-paks.py \
        push-q2-mp-paks.py \
    ; do
        echo "---- $script ----"
        python3 "$HERE/$script" "$target" || echo "  $script exited non-zero on $target"
    done
done
