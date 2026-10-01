#!/usr/bin/env bash
# Install / refresh the Carmageddon 2 LAN host into ~/carmageddon2-server.
#
#   bash scripts/game-servers/carmageddon2/install.sh
#
# Copies ONLY what the host needs: the staged tree (read-only share) into
# game/, with IPXWrapper's DLLs put in place the way the fleet's NT launcher
# does, and the runtime files into _run/. Idempotent.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
DEST="${DEST:-$HOME/carmageddon2-server}"
LIB="${LIB:-/mnt/retro-share/Files/Games-Library/Carmageddon2}"
NAME="${NETNAME:-FLEET HOST}"

[ -f "$LIB/Carma2_SW.exe" ] || { echo "FATAL: $LIB/Carma2_SW.exe not found (is the share mounted?)" >&2; exit 1; }
mkdir -p "$DEST/game" "$DEST/_run"

# The game. Manual.pdf and the IPXWrapper source zip are not needed to host.
rsync -a --exclude 'Manual.pdf' --exclude 'ipxwrapper/' "$LIB/" "$DEST/game/"
# IPXWrapper ships under inert .ipxw names (Win9x cannot load it); the host is
# NT-like (Wine), so activate it exactly as "Play Carmageddon 2.bat" does.
for f in wsock32 mswsock dpwsockx ipxwrapper; do
    cp -f "$DEST/game/$f.dll.ipxw" "$DEST/game/$f.dll"
done

# The host's name in every joiner's list. OPTIONS.TXT keeps it on the line
# after "NetName 0" (and the driver name after "PlayerName 0").
python3 - "$DEST/game/data/OPTIONS.TXT" "$NAME" <<'PY'
import sys
path, name = sys.argv[1], sys.argv[2]
raw = open(path, 'rb').read()
nl = b'\r\n' if b'\r\n' in raw else b'\n'
lines = raw.split(nl)
for key in (b'NetName', b'PlayerName'):
    for i, l in enumerate(lines[:-1]):
        if l.split(b' ')[0] == key:
            lines[i + 1] = name.encode()
open(path, 'wb').write(nl.join(lines))
PY

# The bind shim, built for the container's glibc (bookworm, 2.36).
gcc -O2 -shared -fPIC -o "$DEST/_run/bindiface.so" "$HERE/bindiface.c"
if objdump -T "$DEST/_run/bindiface.so" | grep -o 'GLIBC_2\.[0-9]*' | sort -uV | tail -1 \
        | awk -F. '{exit !($2 > 36)}'; then
    echo "FATAL: bindiface.so needs a glibc newer than the container's 2.36" >&2; exit 1
fi

for f in entry.sh run-carmageddon2-server.sh host.py frame_refs.json asoundrc; do
    install -m 0755 "$HERE/$f" "$DEST/_run/$f"
done
install -m 0644 "$REPO/scripts/game-servers/units/carmageddon2-server.service" \
    "$HOME/.config/systemd/user/carmageddon2-server.service"
systemctl --user daemon-reload
echo "installed into $DEST; start with: systemctl --user enable --now carmageddon2-server"
