#!/usr/bin/env bash
# Carmageddon 2 LAN host on the dev host - Wine in a container.
#
# --net=host: IPXWrapper speaks UDP 54792 and finds players by BROADCAST on
# the fleet subnet, so the container must sit on the host's own NICs.
set -euo pipefail
GAME_DIR="${GAME_DIR:-$HOME/carmageddon2-server}"
IMAGE="${IMAGE:-retro-wine:bookworm}"
NAME="${NAME:-c2srv}"
HOST_IP="${HOST_IP:-192.168.1.132}"

# The NIC that owns HOST_IP (enp129s0 today). Refuse rather than guess: on
# the wrong NIC the game is invisible to the fleet while looking fine here.
IFACE=$(ip -o -4 addr show | awk -v ip="$HOST_IP" '$4 ~ "^"ip"/" {print $2; exit}')
if [ -z "$IFACE" ]; then
    echo "[run] FATAL: no interface carries $HOST_IP - not starting" >&2
    exit 1
fi
MAC=$(tr 'a-f' 'A-F' < "/sys/class/net/$IFACE/address")

# IPXWrapper's interface table, HKCU\Software\IPXWrapper (layout from its own
# source, src/config.c): the wired NIC is primary and enabled; the "wildcard"
# interface (all NICs at once - Wi-Fi, docker bridges, tailscale, whose
# all-zero MAC also matches the wildcard key) and every other NIC disabled.
ipxw_reg() {
    printf 'REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\Software\\IPXWrapper]\r\n'
    printf '"primary"=hex:%s\r\n"port"=dword:0000d608\r\n\r\n' "$(echo "$MAC" | tr 'A-F:' 'a-f,')"
    printf '[HKEY_CURRENT_USER\\Software\\IPXWrapper\\00:00:00:00:00:00]\r\n"enabled"=dword:00000000\r\n\r\n'
    printf '[HKEY_CURRENT_USER\\Software\\IPXWrapper\\%s]\r\n"enabled"=dword:00000001\r\n\r\n' "$MAC"
    for d in /sys/class/net/*; do
        n=$(basename "$d"); [ "$n" = "$IFACE" ] && continue
        m=$(tr 'a-f' 'A-F' < "$d/address" 2>/dev/null) || continue
        case "$m" in ""|00:00:00:00:00:00) continue ;; esac
        printf '[HKEY_CURRENT_USER\\Software\\IPXWrapper\\%s]\r\n"enabled"=dword:00000000\r\n\r\n' "$m"
    done
}
if [ "${1:-}" = "--print-reg" ]; then ipxw_reg; exit 0; fi    # for the tests
ipxw_reg > "$GAME_DIR/_run/ipxwrapper.reg"
echo "[run] IPXWrapper pinned to $IFACE ($MAC, $HOST_IP)"

docker rm -f "$NAME" >/dev/null 2>&1 || true
exec docker run --rm --init --name "$NAME" --net=host \
    -v "$GAME_DIR:/game" -v carma2-wineprefix:/wp \
    -v "$GAME_DIR/_run/asoundrc:/root/.asoundrc:ro" \
    -e WINEDEBUG=-all -e WINEPREFIX=/wp \
    -e C2_BIND_IFACE="$IFACE" -e C2_BIND_PORT=54792 \
    "$IMAGE" \
    bash -c "exec xvfb-run -a -s '-screen 0 1024x768x24' /game/_run/entry.sh"
