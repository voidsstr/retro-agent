#!/usr/bin/env bash
# Deus Ex MP dedicated server on the dev host - Wine in a container.
# --net=host: UE1 answers on UDP 7790 (query 7791) and the fleet reaches it by
# direct address on the flat 192.168.1.0/24 subnet.
set -euo pipefail
GAME_DIR="${GAME_DIR:-$HOME/deusex-server}"
IMAGE="${IMAGE:-retro-wine:bookworm}"
NAME="${NAME:-dxsrv}"
docker rm -f "$NAME" >/dev/null 2>&1 || true
exec docker run --rm --init --name "$NAME" --net=host \
    -v "$GAME_DIR:/game" \
    -e WINEDEBUG=-all -e WINEPREFIX=/tmp/wp \
    -e DX_MAP="${DX_MAP:-DXMP_Cathedral.dx}" \
    "$IMAGE" \
    bash -c "exec xvfb-run -a /game/_run/entry.sh"
