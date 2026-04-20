#!/usr/bin/env bash
# Install a public Quake 2 dedicated server (Yamagi engine) on this Linux box.
#
# - Installs yamagi-quake2 apt package (amd64 engine, no 32-bit libs required)
# - Pulls pak0/pak1/pak2 from an SMB share path (configurable)
# - Writes server.cfg with a stock DM rotation + community master uplinks
# - Systemd user unit quake2-server.service; UDP 27910 forwarded via UFW
#
# Idempotent — safe to re-run.
#
# Env vars:
#   INSTALL_ROOT           default: $HOME
#   SMB_HOST/SMB_USER/SMB_PASS/SMB_SHARE   (see common.sh)
#   Q2_PAKS_REMOTE_DIR     SMB subpath holding pak0/pak1/pak2.pak
#                          default: "Games/Windows 9x/Quake II v3.20/baseq2"

SCRIPT_NAME=install-quake2
. "$(dirname "$0")/common.sh"

: "${Q2_PAKS_REMOTE_DIR:=Games/Windows 9x/Quake II v3.20/baseq2}"

Q2_ROOT="$INSTALL_ROOT/q2-server"
Q2_BASE="$Q2_ROOT/baseq2"
YQ2_CFG_DIR="$HOME/.yq2/baseq2"

# 1. apt package
apt_ensure yamagi-quake2

# 2. Pull pak files
mkdir -p "$Q2_BASE" "$YQ2_CFG_DIR"
for pak in pak0.pak pak1.pak pak2.pak; do
    smb_fetch "${Q2_PAKS_REMOTE_DIR}/${pak}" "$Q2_BASE/$pak"
done

# 3. server.cfg — yamagi rejects the 'serverinfo' flag suffix that stock Q2
# accepted, so use plain `set` and let Yamagi propagate as userinfo/serverinfo
# based on the cvar's built-in flags.
HOSTNAME_FULL="${SERVER_HOSTNAME_PREFIX} (Q2)"
CFG="$YQ2_CFG_DIR/server.cfg"
log "writing $CFG"
cat > "$CFG" <<EOF
// Yamagi Quake 2 Dedicated Server — managed by install-quake2-server.sh
set hostname "$HOSTNAME_FULL"
set maxclients 12
set fraglimit 25
set timelimit 15
set deathmatch 1
set coop 0
set dmflags 272
set public 1
set rcon_password "retroadmin"

// Community master uplinks (Yamagi's default includes master.yamagi.org)
setmaster master.quakeservers.net master.q2servers.com

// Stock DM map rotation
set sv_maplist "q2dm1 q2dm2 q2dm3 q2dm4 q2dm5 q2dm6 q2dm7 q2dm8"
map q2dm1
EOF
# Also mirror to the datadir path so -exec picks it up either way
cp "$CFG" "$Q2_BASE/server.cfg"

# 4. UFW + linger + systemd
ufw_allow_udp 27910 27910 'Quake 2 dedicated server'
ensure_linger

install_user_unit quake2-server.service <<EOF
[Unit]
Description=Yamagi Quake 2 Dedicated Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$Q2_ROOT
ExecStart=/usr/lib/yamagi-quake2/quake2 -datadir $Q2_ROOT +set dedicated 1 +exec server.cfg
Restart=always
RestartSec=15
StandardOutput=append:$Q2_ROOT/server.log
StandardError=append:$Q2_ROOT/server.log

[Install]
WantedBy=default.target
EOF

log "Quake 2 server listening on UDP 27910"
log "hostname: $HOSTNAME_FULL"
log "logs: $Q2_ROOT/server.log"
