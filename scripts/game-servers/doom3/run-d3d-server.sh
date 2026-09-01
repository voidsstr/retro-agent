#!/usr/bin/env bash
# DOOM 3 dedicated server on the dev host - Wine in a container.
# --net=host is required: Doom 3 answers on UDP 27666 and the fleet reaches it
# by direct address on the flat 192.168.1.0/24 subnet.
set -euo pipefail
GAME_DIR="${GAME_DIR:-$HOME/doom3-server}"
IMAGE="${IMAGE:-retro-wine:bookworm}"
NAME="${NAME:-d3dsrv}"
docker rm -f "$NAME" >/dev/null 2>&1 || true
exec docker run --rm --init --name "$NAME" --net=host \
    -v "$GAME_DIR:/game" \
    -e WINEDEBUG=-all -e WINEPREFIX=/tmp/wp \
    -e D3_MAP="${D3_MAP:-game/mp/d3dm1}" \
    "$IMAGE" \
    bash -c "exec xvfb-run -a /game/_run/entry.sh"
