#!/usr/bin/env bash
# Install a Counter-Strike 1.6 (HLDS / GoldSrc, Steam app 90) dedicated server
# as a `systemctl --user` unit, matching the other game-server installers.
#
#   ./install-cs16-server.sh
#
# Tunables (env vars):
#   CS_PORT          default 27015   (single UDP port: game + query)
#   CS_MAXPLAYERS    default 16
#   CS_MAP           default de_dust2
#   RCON_PASS        default retroadmin
#   CS_DIR           default $HOME/hlds-cs16
#   STEAMCMD_DIR     default $HOME/steamcmd
#
# Notes:
#   - HLDS binaries are 32-bit, so this needs i386 libs (installed via sudo).
#   - sv_lan 1 is set so non-Steam retro clients on the LAN can join without
#     Steam authentication / a GSLT (Game Server Login Token). Flip to 0 only
#     if you obtain a GSLT and want public master-server listing.
#   - Steam app 90 downloads are notoriously flaky; the install step retries.
SCRIPT_NAME=cs16
. "$(dirname "$0")/common.sh"

CS_PORT="${CS_PORT:-27015}"
CS_MAXPLAYERS="${CS_MAXPLAYERS:-16}"
CS_MAP="${CS_MAP:-de_dust2}"
RCON_PASS="${RCON_PASS:-retroadmin}"
CS_DIR="${CS_DIR:-$INSTALL_ROOT/hlds-cs16}"
STEAMCMD_DIR="${STEAMCMD_DIR:-$INSTALL_ROOT/steamcmd}"
HOSTNAME_FULL="${SERVER_HOSTNAME_PREFIX} (CS 1.6)"

# 1. 32-bit runtime (HLDS + SteamCMD are i386)
if ! dpkg --print-foreign-architectures 2>/dev/null | grep -q i386; then
    log "adding i386 architecture (sudo)"
    sudo dpkg --add-architecture i386
    sudo apt-get update
fi
apt_ensure ca-certificates curl tar lib32gcc-s1 lib32stdc++6 libc6:i386

# 2. SteamCMD
if [ ! -x "$STEAMCMD_DIR/steamcmd.sh" ]; then
    log "installing SteamCMD -> $STEAMCMD_DIR"
    mkdir -p "$STEAMCMD_DIR"
    https_fetch "https://media.steampowered.com/installer/steamcmd_linux.tar.gz" \
                "$STEAMCMD_DIR/steamcmd_linux.tar.gz"
    tar -xzf "$STEAMCMD_DIR/steamcmd_linux.tar.gz" -C "$STEAMCMD_DIR"
fi

# 3. Install/Update HLDS + Counter-Strike (app 90, mod cstrike). Retry: the
#    GoldSrc HLDS depot frequently drops mid-download and needs another pass.
# force_install_dir MUST precede login, or SteamCMD warns "use force_install_dir
# before logon!" and installs to the default ~/Steam location instead.
# The Linux game lib is cstrike/dlls/cs.so (modern naming; mp.so is the old name).
install_hlds() {
    "$STEAMCMD_DIR/steamcmd.sh" \
        +force_install_dir "$CS_DIR" \
        +login anonymous \
        +app_set_config 90 mod cstrike \
        +app_update 90 validate +quit || true
}
for attempt in 1 2 3 4 5 6; do
    if [ -x "$CS_DIR/hlds_run" ] && [ -f "$CS_DIR/cstrike/dlls/cs.so" ]; then
        log "HLDS + cstrike present"
        break
    fi
    log "HLDS install/validate pass $attempt ..."
    install_hlds
done
[ -x "$CS_DIR/hlds_run" ]            || die "hlds_run missing after install"
[ -f "$CS_DIR/cstrike/dlls/cs.so" ]  || die "cstrike mod missing after install"

# 4. Server config
cat > "$CS_DIR/cstrike/server.cfg" <<CFG
hostname "$HOSTNAME_FULL"
rcon_password "$RCON_PASS"
sv_lan 1
sv_region 255
sv_contact ""
mp_autokick 0
mp_timelimit 30
mp_roundtime 5
mp_freezetime 3
mp_friendlyfire 0
mp_autoteambalance 1
mp_limitteams 2
sv_voiceenable 1
sv_alltalk 0
sv_cheats 0
log on
CFG
log "wrote $CS_DIR/cstrike/server.cfg"

# 5. systemd --user unit
install_user_unit cs16-server.service <<UNIT
[Unit]
Description=Counter-Strike 1.6 (HLDS) dedicated server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$CS_DIR
ExecStart=$CS_DIR/hlds_run -game cstrike -strictportbind -ip 0.0.0.0 -port $CS_PORT +sv_lan 1 +maxplayers $CS_MAXPLAYERS +map $CS_MAP +servercfgfile server.cfg
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
UNIT

# 6. Persistence + firewall
ensure_linger
ufw_allow_udp "$CS_PORT" "$CS_PORT" "CS 1.6 GoldSrc"

sleep 2
systemctl --user --no-pager status cs16-server.service | head -n 12 || true
log "Done. CS 1.6 server '$HOSTNAME_FULL' on UDP $CS_PORT"
log "Retro clients: connect to  $(ip -4 addr show 2>/dev/null | awk '/inet 192\.168\./{print $2}' | cut -d/ -f1 | head -1):$CS_PORT"
