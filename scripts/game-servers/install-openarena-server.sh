#!/usr/bin/env bash
# Install an OpenArena (Quake 3-compatible) dedicated server on this Linux box.
#
# - Installs openarena-server + openarena-data apt packages
# - Disables the system-wide openarena-server.service that conflicts on UDP 27960
# - Writes server.cfg with a stock DM rotation + multi-master uplink
# - Systemd user unit openarena-server.service
#
# Idempotent — safe to re-run.
#
# Env vars:
#   INSTALL_ROOT           default: $HOME
#   SERVER_HOSTNAME_PREFIX default: "NSC Retro Fleet Arena"

SCRIPT_NAME=install-openarena
. "$(dirname "$0")/common.sh"

OA_ROOT="$INSTALL_ROOT/q3-server"
OA_HOME="$OA_ROOT/.openarena/baseoa"

# 1. apt packages
apt_ensure openarena-server openarena-data

# 2. Disable conflicting system-wide instance (if it's there)
# The Debian package auto-enables a system unit that grabs UDP 27960 with the
# default config — our user unit will share the port but lose the query race.
if systemctl is-enabled openarena-server.service >/dev/null 2>&1 \
   || systemctl is-active openarena-server.service >/dev/null 2>&1; then
    log "disabling system-wide openarena-server.service (we run as user)"
    sudo systemctl disable --now openarena-server.service || true
else
    log "system-wide openarena-server.service already disabled"
fi

# 3. server.cfg
mkdir -p "$OA_HOME"
HOSTNAME_FULL="${SERVER_HOSTNAME_PREFIX} (OA)"
CFG="$OA_HOME/server.cfg"
log "writing $CFG"
cat > "$CFG" <<EOF
// OpenArena (Q3-compatible) Dedicated Server — managed by install-openarena-server.sh
seta sv_hostname "$HOSTNAME_FULL"
seta sv_maxclients 12
seta g_gametype 0            // 0=FFA DM, 3=TDM, 4=CTF
seta fraglimit 25
seta timelimit 15
seta g_allowVote 1
seta rconpassword "retroadmin"
seta sv_fps 40
seta sv_master1 "dpmaster.deathmask.net"
seta sv_master2 "master.ioquake3.org"
seta sv_master3 "master3.idsoftware.com"
seta dedicated 2             // 2 = internet public, heartbeat to masters
seta com_hunkMegs 128

// Map cycle through stock OA maps via vstr
set d1 "map oa_dm1 ; set nextmap vstr d2"
set d2 "map oa_dm2 ; set nextmap vstr d3"
set d3 "map oa_dm3 ; set nextmap vstr d4"
set d4 "map oa_dm4 ; set nextmap vstr d5"
set d5 "map oa_dm5 ; set nextmap vstr d6"
set d6 "map oa_dm6 ; set nextmap vstr d7"
set d7 "map oa_dm7 ; set nextmap vstr d1"
vstr d1
EOF

# 4. UFW + linger + systemd
ufw_allow_udp 27960 27960 'OpenArena/Q3 dedicated server'
ensure_linger

install_user_unit openarena-server.service <<EOF
[Unit]
Description=OpenArena (Q3-compatible) Dedicated Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$OA_ROOT
Environment=HOME=$OA_ROOT
ExecStart=/usr/games/openarena-server +set dedicated 2 +exec server.cfg
Restart=always
RestartSec=15
StandardOutput=append:$OA_ROOT/server.log
StandardError=append:$OA_ROOT/server.log

[Install]
WantedBy=default.target
EOF

log "OpenArena server listening on UDP 27960"
log "hostname: $HOSTNAME_FULL"
log "masters: dpmaster.deathmask.net, master.ioquake3.org, master3.idsoftware.com"
log "logs: $OA_ROOT/server.log"
