#!/usr/bin/env bash
# Install Unreal Tournament 99 dedicated server (OldUnreal 469e) on Linux.
#
# - Base game data (Maps/Textures/Music/Sounds/System) pulled from SMB share
# - OldUnreal 469e Linux amd64 patch overlaid on top (community-maintained
#   continuation of UT99, last stable release Nov 2025). Cross-compatible with
#   legacy Epic 436/451 clients AND with the WinXP 469e client patch, so the
#   retro fleet can play on it without modification.
# - Listens on UDP 7797 (game) + 7798 (query) — shifted from the default 7777
#   to coexist with the UT2004 dedicated server on the same host.
# - Master uplinks: OldUnreal 469e ships with 8 community masters in
#   UnrealTournament.ini (333networks, errorist.eu, oldunreal.com ×2, openspy,
#   qtracker, hypercoop, telefragged). No mod required.
#
# Idempotent — safe to re-run.
#
# Env vars:
#   INSTALL_ROOT            default: $HOME
#   SMB_HOST/USER/PASS/SHARE (see common.sh)
#   UT99_SHARE_DIR          SMB subpath to a stock UT99 install
#                           default: "Games/Windows 9x/Unreal Tournament (Installed)"
#   UT99_PATCH_URL          default: OldUnreal 469e Linux amd64 tarball
#   UT99_PORT               default: 7797 (game port; query = PORT+1)

SCRIPT_NAME=install-ut99
. "$(dirname "$0")/common.sh"

: "${UT99_SHARE_DIR:=Games/Windows 9x/Unreal Tournament (Installed)}"
: "${UT99_PATCH_URL:=https://github.com/OldUnreal/UnrealTournamentPatches/releases/download/v469e/OldUnreal-UTPatch469e-Linux-amd64.tar.bz2}"
: "${UT99_PORT:=7797}"

UT99_ROOT="$INSTALL_ROOT/ut99-server"
UT99_SYS="$UT99_ROOT/System64"
CFG_INI="$HOME/.utpg/System/UnrealTournament.ini"
PATCH_CACHE="$HOME/.cache/game-servers/ut99-linux.tar.bz2"

# 1. Pull base game data from SMB share (~830MB: Maps, Textures, Music, Sounds, System)
log "pulling UT99 base game data from SMB share (~830 MB)"
python3 - "$UT99_ROOT" "$SMB_HOST" "$SMB_SHARE" "$SMB_USER" "$SMB_PASS" "$UT99_SHARE_DIR" <<'PYEOF'
import os, subprocess, sys, urllib.parse
dest_root, host, share, user, password, share_dir = sys.argv[1:7]
try:
    import json
    inv_path = os.path.join(
        os.path.dirname(os.path.realpath(sys.argv[0])),
        '..', '..', '..', 'nsc-assistant', 'share-inventory.json'
    )
    # Fallback search paths
    for p in (inv_path, '/home/voidsstr/development/nsc-assistant/share-inventory.json'):
        if os.path.exists(p):
            inv_path = p
            break
    with open(inv_path) as f:
        inv = json.load(f)
    files = inv.get('files', [])
except Exception:
    files = []

keep_dirs = {'Maps', 'Music', 'Sounds', 'Textures', 'System', 'Help', 'Web', 'UnrealEd'}
base_prefix = share_dir + '/'

targets = []
for f in files:
    p = f.get('path', '')
    if not p.startswith(base_prefix):
        continue
    rest = p[len(base_prefix):]
    top = rest.split('/')[0]
    if top not in keep_dirs:
        continue
    targets.append((rest, f.get('size_bytes', 0)))

if not targets:
    print('NOTE: share-inventory.json not found or empty — you must populate '
          f'{dest_root} manually from your UT99 install', file=sys.stderr)
    sys.exit(1)

total = len(targets)
print(f'{total} files, {sum(s for _,s in targets)//1024//1024} MB', flush=True)
enc_share_dir = urllib.parse.quote(share_dir)
for i, (rest, size) in enumerate(targets, 1):
    local = os.path.join(dest_root, rest)
    if os.path.exists(local) and os.path.getsize(local) == size:
        continue
    os.makedirs(os.path.dirname(local), exist_ok=True)
    url = f'smb://{host}/{share}/{enc_share_dir}/{urllib.parse.quote(rest)}'
    subprocess.run(
        ['curl','-s','--max-time','300','-u', f'{user}:{password}',
         url, '-o', local],
        capture_output=True, check=False,
    )
    if i % 50 == 0 or i == total:
        print(f'  [{i}/{total}]', flush=True)
PYEOF

# 2. Overlay OldUnreal 469e Linux amd64 patch
https_fetch "$UT99_PATCH_URL" "$PATCH_CACHE"
log "overlaying 469e patch"
tar -xjf "$PATCH_CACHE" -C "$UT99_ROOT" --overwrite 2>&1 | tail -3 || true
chmod +x "$UT99_SYS/ucc-bin-amd64" "$UT99_SYS/ut-bin-amd64" 2>/dev/null || true

# 3. Initialize the OldUnreal preferences dir by running ucc once
if [ ! -f "$CFG_INI" ]; then
    log "first-run init to populate $CFG_INI"
    ( cd "$UT99_SYS" && timeout 6 ./ucc-bin-amd64 help >/dev/null 2>&1 ) || true
fi

# 4. Configure the server: hostname, port, admin password, drop missing skin packages
log "configuring $CFG_INI"
python3 - "$CFG_INI" "$UT99_PORT" "${SERVER_HOSTNAME_PREFIX} (UT99)" "${SERVER_ADMIN_PASSWORD:-retroadmin}" <<'PYEOF'
import re, sys
path, port, hostname, adminpw = sys.argv[1:]

with open(path) as f:
    content = f.read()

def set_kv(section, key, value, c):
    pat = re.compile(
        rf'(\[{re.escape(section)}\][^\[]*?)(^{re.escape(key)}\s*=[^\n]*\n)',
        re.MULTILINE | re.DOTALL
    )
    if pat.search(c):
        return pat.sub(rf'\g<1>{key}={value}\n', c, count=1)
    sec = re.compile(rf'(\[{re.escape(section)}\]\n)', re.MULTILINE)
    if sec.search(c):
        return sec.sub(rf'\1{key}={value}\n', c, count=1)
    return c + f'\n[{section}]\n{key}={value}\n'

content = set_kv('URL', 'Port', port, content)
content = set_kv('URL', 'Map', 'DM-Deck16][.unr', content)
content = set_kv('Engine.GameReplicationInfo', 'ServerName', hostname, content)
content = set_kv('Engine.GameReplicationInfo', 'ShortName', hostname.split()[0], content)
content = set_kv('Engine.GameReplicationInfo', 'AdminName', 'admin', content)
content = set_kv('Engine.GameReplicationInfo', 'AdminEmail', '', content)
content = set_kv('Engine.GameReplicationInfo', 'MOTDLine1', hostname, content)
content = set_kv('Engine.GameReplicationInfo', 'MOTDLine2', '469e server - stock maps', content)
content = set_kv('Engine.GameInfo', 'AdminPassword', adminpw, content)
content = set_kv('Botpack.DeathMatchPlus', 'TimeLimit', '15', content)
content = set_kv('Botpack.DeathMatchPlus', 'FragLimit', '25', content)
content = set_kv('Botpack.DeathMatchPlus', 'MaxPlayers', '12', content)
content = set_kv('Botpack.DeathMatchPlus', 'MinPlayers', '6', content)
content = set_kv('Botpack.DeathMatchPlus', 'bAutoNumBots', 'True', content)
content = set_kv('IpServer.UdpServerUplink', 'DoUplink', 'True', content)
content = set_kv('IpServer.UdpServerUplink', 'UpdateMinutes', '1', content)

# Drop ServerPackages that reference the Epic bonus-pack mesh skins which the
# stock install doesn't include (TCowMeshSkins, TNaliMeshSkins, TSkMSkins).
# Leaving them in aborts server startup with "Failed to load TCowMeshSkins".
missing = ('TCowMeshSkins', 'TNaliMeshSkins', 'TSkMSkins')
lines_out = []
for line in content.splitlines(keepends=True):
    if any(line.startswith(f'ServerPackages={m}') for m in missing):
        continue
    lines_out.append(line)
content = ''.join(lines_out)

with open(path, 'w') as f:
    f.write(content)
PYEOF

# 5. UFW + linger + systemd
ufw_allow_udp "$UT99_PORT" "$((UT99_PORT + 1))" 'UT99 dedicated server'
ensure_linger

install_user_unit ut99-server.service <<EOF
[Unit]
Description=Unreal Tournament 99 (OldUnreal 469e) Dedicated Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$UT99_SYS
ExecStart=$UT99_SYS/ucc-bin-amd64 server DM-Deck16][.unr?game=Botpack.DeathMatchPlus -port=$UT99_PORT
Restart=always
RestartSec=15
StandardOutput=append:$UT99_ROOT/server.log
StandardError=append:$UT99_ROOT/server.log

[Install]
WantedBy=default.target
EOF

log "UT99 server listening on UDP $UT99_PORT (game) + $((UT99_PORT + 1)) (query)"
log "hostname: ${SERVER_HOSTNAME_PREFIX} (UT99)"
log "master uplinks: 8 community masters (ships with 469e), no extra mod needed"
log "logs: $UT99_ROOT/server.log"
