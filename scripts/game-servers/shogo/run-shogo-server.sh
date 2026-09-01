#!/usr/bin/env bash
# Shogo dedicated server on the dev host - Wine in a container.
# --net=host: ShogoSrv answers on UDP 27888 and the fleet reaches it by direct
# address on the flat 192.168.1.0/24 subnet.
set -euo pipefail
GAME_DIR="${GAME_DIR:-$HOME/shogo-server}"
IMAGE="${IMAGE:-retro-wine:bookworm}"
NAME="${NAME:-shogosrv}"
docker rm -f "$NAME" >/dev/null 2>&1 || true
exec docker run --rm --init --name "$NAME" --net=host \
    -v "$GAME_DIR:/game" -v shogo-wineprefix:/wp \
    -e WINEDEBUG=-all -e WINEPREFIX=/wp \
    "$IMAGE" \
    bash -c "exec xvfb-run -a -s '-screen 0 1024x768x24' /game/_run/entry.sh"
