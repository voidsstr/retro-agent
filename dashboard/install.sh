#!/usr/bin/env bash
# Install (or remove) the GDM login-screen fleet dashboard.
#
#   sudo bash dashboard/install.sh              # install + enable
#   sudo bash dashboard/install.sh --uninstall  # put everything back
#   sudo bash dashboard/install.sh --reload     # reinstall files, restart greeter
#
# Three moving parts:
#   1. the collector      -> /usr/local/lib/retro-dashboard/ + a system service
#   2. the extension      -> /usr/share/gnome-shell/extensions/<uuid>/
#   3. the greeter config -> /etc/gdm3/greeter.dconf-defaults, compiled by
#                            /usr/share/gdm/generate-config
#
# (3) is why this needs root and why it is scripted rather than documented:
# the greeter runs as a systemd DynamicUser whose home is a tmpfs, so the only
# durable place for its dconf is the system-wide defaults file.
#
# SAFETY: the greeter is how you log in. If the extension misbehaves, run
#   sudo bash dashboard/install.sh --uninstall
# over SSH and the login screen returns to stock at the next greeter restart.

set -euo pipefail

UUID="retro-fleet-dashboard@voidsstr"
EXT_DIR="/usr/share/gnome-shell/extensions/${UUID}"
LIB_DIR="/usr/local/lib/retro-dashboard"
UNIT="retro-dashboard-collector.service"
GREETER_DEFAULTS="/etc/gdm3/greeter.dconf-defaults"
MARK_BEGIN="# >>> retro-fleet-dashboard >>>"
MARK_END="# <<< retro-fleet-dashboard <<<"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { echo "error: $*" >&2; exit 1; }
note() { echo "  $*"; }

[ "$(id -u)" -eq 0 ] || die "run with sudo"

MODE="install"
case "${1:-}" in
    --uninstall) MODE="uninstall" ;;
    --reload)    MODE="reload" ;;
    "")          MODE="install" ;;
    *)           die "unknown argument: $1" ;;
esac

# ---------------------------------------------------------------- helpers

greeter_session() {
    loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}' | while read -r sid; do
        [ -n "$sid" ] || continue
        if loginctl show-session "$sid" -p Class 2>/dev/null | grep -q '=greeter$'; then
            echo "$sid"
            return
        fi
    done
}

restart_greeter() {
    # Never restart gdm.service: it also owns any Chrome Remote Desktop
    # session, so restarting it drops whoever is connected remotely.
    #
    # Nor is killing `gnome-shell --mode=gdm` enough. GdmLocalDisplayFactory
    # reaps the login screen once a user session is registered and will NOT
    # respawn the shell on its own — you get a dead greeter session with no
    # shell and a black monitor. (Learned the hard way; the org.gnome.Shell@gdm
    # and gnome-session-manager@gnome-login units both set RefuseManualStart,
    # so they cannot be restarted by hand either.)
    #
    # Terminating the whole greeter *session* is what works: GDM notices seat0
    # has no greeter and builds a fresh one, new dynamic user and all.
    local sid
    sid="$(greeter_session)"
    if [ -z "$sid" ]; then
        note "no greeter session; it will pick this up when one next starts"
        return
    fi

    note "restarting greeter session $sid"
    loginctl terminate-session "$sid" 2>/dev/null || true

    local i
    for i in $(seq 1 20); do
        sleep 0.5
        if pgrep -f 'gnome-shell --mode=gdm' >/dev/null 2>&1; then
            note "greeter back up"
            return
        fi
    done
    echo "  WARNING: greeter did not come back within 10s." >&2
    echo "           Check 'loginctl list-sessions' and 'systemctl status gdm'." >&2
}

remove_greeter_block() {
    [ -f "$GREETER_DEFAULTS" ] || return 0
    if grep -qF "$MARK_BEGIN" "$GREETER_DEFAULTS"; then
        sed -i "/${MARK_BEGIN}/,/${MARK_END}/d" "$GREETER_DEFAULTS"
        note "removed dconf block from $GREETER_DEFAULTS"
    fi
}

compile_greeter_dconf() {
    if [ -x /usr/share/gdm/generate-config ]; then
        /usr/share/gdm/generate-config
        note "recompiled greeter dconf"
    else
        die "/usr/share/gdm/generate-config missing — is this gdm3?"
    fi
}

# -------------------------------------------------------------- uninstall

if [ "$MODE" = "uninstall" ]; then
    echo "Removing the login-screen dashboard..."
    systemctl disable --now "$UNIT" 2>/dev/null || true
    rm -f "/etc/systemd/system/${UNIT}"
    systemctl daemon-reload
    rm -rf "$LIB_DIR" "$EXT_DIR"
    note "removed collector, unit and extension"
    remove_greeter_block
    compile_greeter_dconf
    restart_greeter
    echo "Done — the login screen is back to stock."
    exit 0
fi

# ---------------------------------------------------------------- install

echo "Installing the login-screen fleet dashboard..."

# --- sanity: the pieces we are about to wire together must exist ---
[ -f "$HERE/collector/dashboard_collector.py" ] || die "collector missing"
[ -f "$HERE/extension/metadata.json" ] || die "extension missing"
command -v gnome-shell >/dev/null || die "gnome-shell not installed"

SHELL_MAJOR="$(gnome-shell --version | grep -oE '[0-9]+' | head -1)"
DECLARED="$(grep -oE '"shell-version"[^]]*]' "$HERE/extension/metadata.json")"
if ! grep -q "\"$SHELL_MAJOR\"" <<<"$DECLARED"; then
    die "extension declares $DECLARED but this is GNOME Shell $SHELL_MAJOR — update metadata.json"
fi
note "GNOME Shell $SHELL_MAJOR"

# --- 1. collector ---
install -d -m 0755 "$LIB_DIR"
install -m 0755 "$HERE/collector/dashboard_collector.py" "$LIB_DIR/dashboard_collector.py"
note "collector -> $LIB_DIR"

# Record where the checkouts live. The collector is copied to /usr/local/lib,
# so it cannot find the retro client or omenfan by walking up from __file__ —
# without this every fleet row reads "down", which is indistinguishable from a
# fleet that is simply powered off.
REPO_ROOT="$(cd "$HERE/.." && pwd)"
[ -d "$REPO_ROOT/client" ] || die "cannot find the retro-agent client/ next to dashboard/"
install -d -m 0755 /etc/retro-dashboard
{
    echo "# Written by dashboard/install.sh — regenerated on every install."
    echo "RETRO_AGENT_REPO=$REPO_ROOT"
    for cand in "${OMENFAN_PATH:-}" "$HOME/development/omen-fan-control" \
                /home/voidsstr/development/omen-fan-control; do
        if [ -n "$cand" ] && [ -d "$cand/omenfan" ]; then
            echo "OMENFAN_PATH=$cand"
            break
        fi
    done
} >/etc/retro-dashboard/collector.env
chmod 0644 /etc/retro-dashboard/collector.env
note "repo paths -> /etc/retro-dashboard/collector.env"
grep -q OMENFAN_PATH /etc/retro-dashboard/collector.env \
    || echo "  WARNING: omen-fan-control not found; vitals panels will be empty" >&2

install -m 0644 "$HERE/systemd/$UNIT" "/etc/systemd/system/$UNIT"
systemctl daemon-reload
systemctl enable "$UNIT" >/dev/null
# restart, not just enable --now: on a reinstall the unit is already running
# and would otherwise keep executing the previous copy of the collector.
systemctl restart "$UNIT"
note "service $UNIT enabled and restarted"

# Give it a moment to publish, then confirm the greeter will find something.
for _ in $(seq 1 10); do
    [ -r /run/retro-dashboard/state.json ] && break
    sleep 0.5
done
if [ -r /run/retro-dashboard/state.json ]; then
    note "state file present ($(stat -c%s /run/retro-dashboard/state.json) bytes)"
else
    echo "  WARNING: no state file yet — check: journalctl -u $UNIT -n 40" >&2
fi

# --- 2. extension ---
install -d -m 0755 "$EXT_DIR"
install -m 0644 "$HERE/extension/metadata.json"   "$EXT_DIR/metadata.json"
install -m 0644 "$HERE/extension/extension.js"    "$EXT_DIR/extension.js"
install -m 0644 "$HERE/extension/render.js"       "$EXT_DIR/render.js"
install -m 0644 "$HERE/extension/stylesheet.css"  "$EXT_DIR/stylesheet.css"
note "extension -> $EXT_DIR"

# --- 3. greeter dconf ---
[ -f "$GREETER_DEFAULTS" ] || die "$GREETER_DEFAULTS missing — is this gdm3?"
cp -n "$GREETER_DEFAULTS" "${GREETER_DEFAULTS}.pre-retro-dashboard" 2>/dev/null || true
remove_greeter_block
cat >>"$GREETER_DEFAULTS" <<EOF

$MARK_BEGIN
# Draws the fleet dashboard on the login screen. Managed by
# retro-agent/dashboard/install.sh — edit there, not here.
[org/gnome/shell]
enabled-extensions=['${UUID}']

# Keep the monitor awake. Without these the greeter takes GNOME's defaults —
# blank after 5 minutes idle, put the display to sleep after 20 — so the
# dashboard renders perfectly into a screen that has been dark for hours. A
# status wall nobody can see is not a status wall.
#
# To go back to a screen that sleeps, delete these two stanzas (keep the
# [org/gnome/shell] one) and re-run /usr/share/gdm/generate-config.
[org/gnome/desktop/session]
idle-delay=uint32 0

[org/gnome/settings-daemon/plugins/power]
sleep-inactive-ac-timeout=0
sleep-inactive-ac-type='nothing'
sleep-inactive-battery-timeout=0
sleep-inactive-battery-type='nothing'
$MARK_END
EOF
note "enabled in $GREETER_DEFAULTS"
compile_greeter_dconf

# --- 4. reload the greeter ---
restart_greeter

cat <<EOF

Installed.

  service   systemctl status $UNIT
  state     /run/retro-dashboard/state.json
  extension $EXT_DIR
  greeter   journalctl -b /usr/bin/gnome-shell | tail

Switch to the login screen (Ctrl+Alt+F1, or log out) to see it.
Any key or mouse movement dismisses the wall and shows the password prompt;
it returns after 45 seconds idle.

To undo everything:  sudo bash dashboard/install.sh --uninstall
EOF
