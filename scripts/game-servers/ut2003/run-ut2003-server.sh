#!/usr/bin/env bash
# Unreal Tournament 2003 dedicated server on the dev host - Wine in a container.
# --net=host: UE2 answers on UDP 7757 (native query 7758) and the LAN
# responder on 10777, and the fleet reaches it by address / broadcast on the
# flat 192.168.1.0/24 subnet.
set -euo pipefail
GAME_DIR="${GAME_DIR:-$HOME/ut2003-server}"
IMAGE="${IMAGE:-retro-wine:bookworm}"
NAME="${NAME:-ut2003srv}"
docker rm -f "$NAME" >/dev/null 2>&1 || true
# --user: run as the tree's owner, not root, so the files UCC writes back into
# the bind-mounted tree (logs, the saved ini) stay rewritable by install.sh.
exec docker run --rm --init --name "$NAME" --net=host \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$GAME_DIR:/game" \
    -e WINEDEBUG=-all -e WINEPREFIX=/tmp/wp \
    -e UT3_MAP="${UT3_MAP:-DM-Antalus}" \
    -e UT3_PORT="${UT3_PORT:-7757}" \
    -e UT3_MINPLAYERS="${UT3_MINPLAYERS:-6}" \
    -e UT3_MAXPLAYERS="${UT3_MAXPLAYERS:-12}" \
    "$IMAGE" \
    /game/_run/entry.sh
