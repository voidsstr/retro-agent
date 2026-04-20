#!/usr/bin/env bash
# Install a public Unreal Tournament 2004 dedicated server on this Linux box.
#
# - Base install: Epic dedicated server 3369.3 (bonus pack included, XP-compatible)
#   pulled from archive.org, extracted into $INSTALL_ROOT/ut2004-server
# - Community master uplinks via the MasterServerMirror mod:
#     utmaster.openspy.net / ut2004master.333networks.com / ut2004master.errorist.eu
# - 30-map stock DM rotation
# - systemd user unit ut2004-server.service, auto-starts at boot (with linger)
#
# Idempotent: safe to re-run. Existing installs are preserved; only missing
# pieces are (re)created.
#
# Env vars (override any):
#   INSTALL_ROOT            default: $HOME
#   SERVER_HOSTNAME_PREFIX  default: "NSC Retro Fleet Arena"
#   SERVER_ADMIN_PASSWORD   default: "retroadmin"
#   UT2004_ZIP_URL          default: archive.org 3369.3 zip
#   MASTERMIRROR_VERSION    default: v1.0.0

SCRIPT_NAME=install-ut2004
. "$(dirname "$0")/common.sh"

: "${SERVER_ADMIN_PASSWORD:=retroadmin}"
: "${UT2004_ZIP_URL:=https://archive.org/download/ut2004-server/dedicatedserver3369.3-bonuspack.zip}"
: "${MASTERMIRROR_VERSION:=v1.0.0}"

UT_ROOT="$INSTALL_ROOT/ut2004-server"
UT_SYS="$UT_ROOT/System"
ZIP_CACHE="$HOME/.cache/game-servers/ut2004.zip"

# 1. Fetch + extract base install (archive.org zip uses LZMA method 14 which
# Info-Zip's `unzip` can't handle, so we extract with Python).
if [ -x "$UT_SYS/ucc-bin-linux-amd64" ]; then
    log "ut2004 already extracted at $UT_ROOT"
else
    https_fetch "$UT2004_ZIP_URL" "$ZIP_CACHE"
    mkdir -p "$UT_ROOT"
    log "extracting UT2004 server (takes ~30s for 2.8 GB)"
    python3 <<PYEOF
import os, zipfile
src = "$ZIP_CACHE"
dest = "$UT_ROOT"
prefix = "dedicatedserver3369.3-bonuspack/"
with zipfile.ZipFile(src) as z:
    count = 0
    for info in z.infolist():
        if not info.filename.startswith(prefix): continue
        rel = info.filename[len(prefix):]
        if not rel: continue
        out = os.path.join(dest, rel)
        if info.is_dir():
            os.makedirs(out, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with z.open(info) as sf, open(out, 'wb') as df:
            while True:
                buf = sf.read(1 << 20)
                if not buf: break
                df.write(buf)
        count += 1
    print(f"extracted {count} files")
PYEOF
    chmod +x "$UT_SYS/ucc-bin-linux-amd64" "$UT_SYS/ucc-bin" 2>/dev/null || true
fi

# 2. Install MasterServerMirror mod (for multi-master uplink — Epic's official
# master has been dead since 2023).
if [ -f "$UT_SYS/MasterServerMirror.u" ]; then
    log "MasterServerMirror mod already installed"
else
    base="https://github.com/0xC0ncord/MasterServerMirror/releases/download/$MASTERMIRROR_VERSION"
    https_fetch "$base/MasterServerMirror.u"   "$UT_SYS/MasterServerMirror.u"
    https_fetch "$base/MasterServerMirror.ini" "$UT_SYS/MasterServerMirror.ini"
fi

# 3. Patch UT2004.ini — idempotent: only rewrites sections we manage.
# The stock UT2004.ini ships with 0x1B (ESC) characters embedded in some
# maplist section headers (DefaultDM, DefaultTDM, 1on1Deathmatch,
# 1on1TeamDeathmatch). Those must be stripped before any section-based edits.
HOSTNAME_FULL="${SERVER_HOSTNAME_PREFIX}"
MAPS=(
  DM-Rankin DM-Deck17 DM-Antalus DM-Asbestos
  DM-Gael DM-Curse4 DM-Goliath DM-Gestalt
  DM-Rrajigar DM-Corrugation DM-Metallurgy DM-IronDeity
  DM-HyperBlast2 DM-Morpheus3 DM-Icetomb DM-Plunge
  DM-Inferno DM-Leviathan DM-TokaraForest DM-Flux2
  DM-Junkyard DM-DesertIsle DM-Injector DM-Insidious
  DM-TrainingDay DM-Sulphur
  DM-BP2-Calandras DM-BP2-GoopGod
  DM-1on1-Albatross DM-1on1-Serpentine
)
log "writing UT2004.ini server config"
python3 - "$UT_SYS/UT2004.ini" "$HOSTNAME_FULL" "$SERVER_ADMIN_PASSWORD" "${MAPS[@]}" <<'PYEOF'
import re, sys
path, host, adminpw, *maps = sys.argv[1:]

with open(path, 'rb') as f:
    data = f.read()
# Strip the stray 0x1B bytes Epic left in some section headers
data = data.replace(b'\x1b', b'')
with open(path, 'wb') as f:
    f.write(data)

with open(path, 'r', encoding='latin-1') as f:
    lines = f.readlines()

def set_kv(section, key, value):
    in_sec = False
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s == section:
            in_sec = True
            sec_start = i + 1
            continue
        if in_sec and s.startswith('[') and s.endswith(']'):
            sec_end = i
            break
    else:
        sec_end = len(lines)
    if not in_sec:
        return
    for i in range(sec_start, sec_end):
        if re.match(rf'^{re.escape(key)}\s*=', lines[i]):
            lines[i] = f'{key}={value}\n'
            return
    lines.insert(sec_end, f'{key}={value}\n')

def rewrite_section(header, body):
    out, i = [], 0
    while i < len(lines):
        if lines[i].strip() == header:
            out.append(lines[i]); i += 1
            while i < len(lines) and not lines[i].lstrip().startswith('['):
                i += 1
            for bl in body:
                out.append(bl + '\n')
            if i < len(lines) and out[-1] != '\n':
                out.append('\n')
            continue
        out.append(lines[i]); i += 1
    return out

set_kv('[Engine.GameReplicationInfo]', 'ServerName', host)
set_kv('[Engine.GameReplicationInfo]', 'ShortName', host.split()[0])
set_kv('[Engine.GameReplicationInfo]', 'AdminName', 'admin')
set_kv('[Engine.GameReplicationInfo]', 'AdminEmail', '')
set_kv('[Engine.GameReplicationInfo]', 'MOTDLine1', host)
set_kv('[Engine.GameReplicationInfo]', 'MOTDLine2', 'DM server - stock maps')
set_kv('[Engine.AccessControl]', 'AdminPassword', adminpw)
set_kv('[Engine.AccessControl]', 'GamePassword', '')
set_kv('[Engine.GameInfo]', 'MaxPlayers', '12')
set_kv('[Engine.GameInfo]', 'MaxSpectators', '4')
set_kv('[XGame.xDeathMatch]', 'MinPlayers', '6')
set_kv('[XGame.xDeathMatch]', 'GoalScore', '25')
set_kv('[XGame.xDeathMatch]', 'TimeLimit', '15')
set_kv('[XGame.xDeathMatch]', 'bAutoNumBots', 'True')
set_kv('[XGame.xDeathMatch]', 'BotRatio', '1.0')
set_kv('[IpDrv.MasterServerUplink]', 'ServerBehindNAT', 'True')

# Wire up MasterServerMirror as a ServerActor (dedupes itself)
joined = ''.join(lines)
if 'MasterServerMirror.MasterServerMirror' not in joined:
    lines = re.sub(
        r'(\[Engine\.GameEngine\]\n)',
        r'\1ServerActors=MasterServerMirror.MasterServerMirror\n',
        joined, count=1
    ).splitlines(keepends=True)

# Point the client-side MasterServerLink at community masters too
old = ('MasterServerList=(Address="ut2004master1.epicgames.com",Port=28902)\n'
       'MasterServerList=(Address="ut2004master2.epicgames.com",Port=28902)')
new = ('MasterServerList=(Address="ut2004master.333networks.com",Port=28902)\n'
       'MasterServerList=(Address="utmaster.openspy.net",Port=28902)')
lines = [l for l in lines]
text = ''.join(lines)
if old in text:
    text = text.replace(old, new)
    lines = text.splitlines(keepends=True)

# Rewrite the DM map rotation (DefaultDM + runtime maplist)
dm_header_body = [
    'DefaultTitle=Default DM',
    'DefaultGameType=XGame.xDeathMatch',
    'DefaultActive=0',
] + [f'DefaultMaps={m}' for m in maps]
lines = rewrite_section('[DefaultDM MaplistRecord]', dm_header_body)
lines = rewrite_section('[XInterface.MapListDeathMatch]',
                        ['MapNum=0'] + [f'Maps={m}' for m in maps])

with open(path, 'w', encoding='latin-1') as f:
    f.writelines(lines)
PYEOF

# 4. UFW + linger + systemd unit
ufw_allow_udp 7777 7787 'UT2004 dedicated server'
ensure_linger

install_user_unit ut2004-server.service <<EOF
[Unit]
Description=Unreal Tournament 2004 Dedicated Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$UT_SYS
ExecStart=$UT_SYS/ucc-bin-linux-amd64 server DM-Rankin?game=XGame.xDeathMatch -nohomedir
Restart=always
RestartSec=15
StandardOutput=append:$UT_ROOT/server.log
StandardError=append:$UT_ROOT/server.log
KillSignal=SIGTERM
TimeoutStopSec=20

[Install]
WantedBy=default.target
EOF

log "UT2004 server listening on UDP 7777 (game), 7778 (browser), 7787 (gamespy)"
log "hostname: $HOSTNAME_FULL  admin password: $SERVER_ADMIN_PASSWORD"
log "logs: $UT_ROOT/server.log"
