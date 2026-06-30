#!/usr/bin/env bash
# Install + enable the retro chat backend as systemd --user services so the
# daemon and brain auto-start on boot (no manual launching after a reboot).
#
#   bash scripts/install-chat-services.sh
#
# Uses systemctl --user (same convention as the game servers). loginctl
# enable-linger makes the services start at boot even before you log in.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
U="$HOME/.config/systemd/user"
mkdir -p "$U"

cp "$REPO/scripts/retro-chat-daemon.service" "$U/"
cp "$REPO/scripts/retro-chat-brain.service"  "$U/"

# Start at boot without an interactive login. May need sudo on some distros.
loginctl enable-linger "$USER" 2>/dev/null \
  || echo "NOTE: 'loginctl enable-linger $USER' needs sudo here — run it once so services start at boot without login."

systemctl --user daemon-reload
systemctl --user enable --now retro-chat-daemon
systemctl --user enable --now retro-chat-brain

echo "---"
systemctl --user --no-pager status retro-chat-daemon retro-chat-brain 2>/dev/null | grep -E 'Loaded:|Active:' || true
echo "---"
echo "Tail logs with:"
echo "  journalctl --user -u retro-chat-daemon -f"
echo "  journalctl --user -u retro-chat-brain -f"
