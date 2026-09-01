#!/usr/bin/env bash
# Serious Sam TFE dedicated server on the dev host - Wine in a container.
# --net=host: the engine answers on UDP 25600 and the fleet reaches it by
# direct address on the flat 192.168.1.0/24 subnet.
set -euo pipefail
GAME_DIR="${GAME_DIR:-$HOME/ssam-tfe-server}"
IMAGE="${IMAGE:-retro-wine:bookworm}"
NAME="${NAME:-sstfesrv}"
docker rm -f "$NAME" >/dev/null 2>&1 || true
exec docker run --rm --init --name "$NAME" --net=host \
    -v "$GAME_DIR:/game" \
    -e WINEDEBUG=-all -e WINEPREFIX=/tmp/wp \
    -e SS_CONFIG="${SS_CONFIG:-NSCFleet}" \
    "$IMAGE" \
    bash -c "exec xvfb-run -a /game/_run/entry.sh"
