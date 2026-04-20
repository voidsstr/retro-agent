#!/usr/bin/env bash
# Shared helpers for the game server install scripts.
# Source from other scripts: . "$(dirname "$0")/common.sh"

set -euo pipefail

: "${SMB_HOST:=192.168.1.122}"
: "${SMB_USER:=admin}"
: "${SMB_PASS:=password}"
: "${SMB_SHARE:=files}"
: "${SYSTEMD_USER_DIR:=$HOME/.config/systemd/user}"
: "${INSTALL_ROOT:=$HOME}"
: "${SERVER_HOSTNAME_PREFIX:=NSC Retro Fleet Arena}"

log()  { printf '\033[36m[%s]\033[0m %s\n' "${SCRIPT_NAME:-install}" "$*" >&2; }
warn() { printf '\033[33m[%s] WARN:\033[0m %s\n' "${SCRIPT_NAME:-install}" "$*" >&2; }
die()  { printf '\033[31m[%s] ERROR:\033[0m %s\n' "${SCRIPT_NAME:-install}" "$*" >&2; exit 1; }

# Idempotent apt install — only install packages that are missing.
apt_ensure() {
    local missing=()
    for pkg in "$@"; do
        dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        log "apt packages already installed: $*"
        return 0
    fi
    log "installing: ${missing[*]}"
    sudo apt-get install -y "${missing[@]}"
}

# Download file from SMB share via curl smb://
# Usage: smb_fetch "remote/path/with spaces/file.ext" "/local/destination"
smb_fetch() {
    local remote="$1"
    local dest="$2"
    if [ -s "$dest" ]; then
        log "already have $(basename "$dest") ($(du -h "$dest" | cut -f1))"
        return 0
    fi
    mkdir -p "$(dirname "$dest")"
    # URL-encode spaces for curl's SMB handler
    local enc
    enc=$(printf '%s' "$remote" | sed 's/ /%20/g')
    log "fetching smb://${SMB_HOST}/${SMB_SHARE}/${enc}"
    curl -sS --max-time 900 -u "${SMB_USER}:${SMB_PASS}" \
         "smb://${SMB_HOST}/${SMB_SHARE}/${enc}" -o "$dest" \
      || die "SMB fetch failed: $remote"
    [ -s "$dest" ] || die "SMB fetch produced empty file: $dest"
}

# Download via HTTPS with retries
https_fetch() {
    local url="$1"
    local dest="$2"
    if [ -s "$dest" ]; then
        log "already have $(basename "$dest")"
        return 0
    fi
    mkdir -p "$(dirname "$dest")"
    log "downloading $url"
    curl -fsSL --max-time 900 --retry 3 "$url" -o "$dest" \
      || die "HTTPS fetch failed: $url"
}

# Install a systemd user unit and enable+start it.
# Args: unit-name content-heredoc-via-stdin
install_user_unit() {
    local name="$1"
    mkdir -p "$SYSTEMD_USER_DIR"
    cat > "$SYSTEMD_USER_DIR/$name"
    systemctl --user daemon-reload
    systemctl --user enable "$name" >/dev/null
    systemctl --user restart "$name"
    log "systemd user unit $name installed & started"
}

# Verify user-lingering is on so services persist past logout / survive reboot.
ensure_linger() {
    if ! loginctl show-user "$USER" 2>/dev/null | grep -q 'Linger=yes'; then
        log "enabling user lingering (requires sudo)"
        sudo loginctl enable-linger "$USER"
    else
        log "user lingering already enabled"
    fi
}

# Open a UDP port range in UFW if the service is active. Idempotent.
# Args: from-port to-port comment
#
# `ufw status` requires root to read the rule table. If we can't sudo
# non-interactively, we skip with a clear warning rather than silently
# claiming "ufw not active" based on a failed stat.
ufw_allow_udp() {
    local from="$1" to="$2" comment="$3"
    if ! command -v ufw >/dev/null 2>&1; then
        warn "ufw not installed, skipping firewall rule for $from:$to/udp"
        return 0
    fi
    if ! systemctl is-active --quiet ufw 2>/dev/null; then
        log "ufw inactive — not adding rule for $from:$to/udp"
        return 0
    fi
    local rule
    if [ "$from" = "$to" ]; then rule="$from/udp"; else rule="$from:$to/udp"; fi
    local status
    status=$(sudo -n ufw status 2>/dev/null) || {
        warn "can't read ufw rules non-interactively — rerun under sudo or add manually: sudo ufw allow $rule comment '$comment'"
        return 0
    }
    if printf '%s\n' "$status" | grep -qE "^${rule} +ALLOW"; then
        log "ufw rule already present for $rule"
        return 0
    fi
    log "adding ufw rule $rule ($comment)"
    sudo ufw allow "$rule" comment "$comment"
}
