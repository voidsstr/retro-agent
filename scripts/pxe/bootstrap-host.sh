#!/usr/bin/env bash
# bootstrap-host.sh - stand up the retro PXE install server from nothing.
#
# Everything the server needs that is NOT in git lives under /srv/retro-pxe:
# the TFTP payload, the extracted DriverPacks, the generated NIC database and
# the runtime state. This script rebuilds all of it, so losing the host costs a
# rerun rather than a reconstruction.
#
# WHAT IT DOES NOT DO: it does not create the XP source image. That needs retail
# media, which is not ours to redistribute - see README.md, "Rebuilding the XP
# payload". Run make-xp-source.sh once media is in place.
#
# SECRETS come from Azure Key Vault (nsc-secrets-kv); nothing sensitive is in
# this repo. See SECRETS.md for the full list and what each is for.
set -euo pipefail

ROOT="${ROOT:-/srv/retro-pxe}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
VAULT="${VAULT:-nsc-secrets-kv}"
PACKS_ARCHIVE="${PACKS_ARCHIVE:-}"      # DriverPacks-XP-32.7z, if you have it

say() { printf '\n== %s ==\n' "$1"; }

say "packages"
# python3-hivex is needed by add-nic-services.py; cabextract expands the
# compressed setup files inside a retail I386.
sudo apt-get update -qq
sudo apt-get install -y -qq \
    python3 python3-hivex p7zip-full cabextract xorriso \
    qemu-system-x86 qemu-utils cifs-utils tcpdump
echo "   ok"

say "directories"
sudo mkdir -p "$ROOT"/{tftp,driverpacks}
sudo chown -R "$USER":"$USER" "$ROOT"
echo "   $ROOT"

say "driver packs"
# The packs are ~1.4 GB and are NOT redistributable through this repo. Point
# PACKS_ARCHIVE at DriverPacks-XP-32.7z (the fleet keeps a copy on the NAS at
# Files/Drivers/, and it is mirrored on archive.org as driver-packs-xp-32.7z).
if [ -n "$PACKS_ARCHIVE" ] && [ -f "$PACKS_ARCHIVE" ]; then
    cd "$ROOT/driverpacks"
    7z e -y "$PACKS_ARCHIVE" 'NT5/x86/DP_*.7z' >/dev/null
    for spec in "LAN-RIS:ris" "LAN:lan" "Chipset:chipset" "MassStorage:massstorage" \
                "Graphics_A:graphics_a" "Graphics_B:graphics_b" "Graphics_C:graphics_c" \
                "Sound_A:sound_a" "Sound_B:sound_b" "Monitor:monitor"; do
        name="${spec%%:*}"; dir="${spec##*:}"
        f=$(ls "DP_${name}_wnt5_x86-32_"*.7z 2>/dev/null | head -1) || true
        [ -n "${f:-}" ] || { echo "   missing pack: $name"; continue; }
        mkdir -p "$dir"; 7z x -y -o"$dir" "$f" >/dev/null
        echo "   $dir: $(find "$dir" -iname '*.inf' | wc -l) INFs"
    done
    cd - >/dev/null
else
    echo "   SKIPPED - set PACKS_ARCHIVE=/path/to/DriverPacks-XP-32.7z"
    echo "   Without them the image ships only the drivers retail XP has, which"
    echo "   is six Intel NICs and no third-party graphics or sound."
fi

say "systemd unit"
sudo cp "$HERE/retro-pxe.service" /etc/systemd/system/retro-pxe.service
sudo systemctl daemon-reload
sudo systemctl enable retro-pxe >/dev/null 2>&1 || true
echo "   installed (start it after the payload exists)"

say "secrets"
if command -v az >/dev/null 2>&1 && az account show >/dev/null 2>&1; then
    for s in fleet-winxp-pro-sp3-x14-80428-key; do
        if az keyvault secret show --vault-name "$VAULT" --name "$s" \
             --query value -o tsv >/dev/null 2>&1; then
            echo "   $s: present"
        else
            echo "   $s: MISSING from $VAULT - see SECRETS.md" >&2
        fi
    done
else
    echo "   az not logged in - run 'az login', then re-run, or see SECRETS.md"
fi

cat <<'NEXT'

== next ==
  1. Mount the fleet share (the XP media and the game library live there):
       sudo mount -t cifs //192.168.1.122/files /mnt/retro-share \
            -o vers=2.0,uid=$(id -u),gid=$(id -g),ro
     vers=2.0 matters - the LinkStation rejects 3.0 and 2.1 with mount error(22).
  2. Build the image (needs retail XP media):
       bash scripts/pxe/inject-drivers.sh          # widen hardware support
       bash scripts/pxe/stage-oem.sh               # agent, auto-login, drivers path
       python3 scripts/pxe/build-nicdb.py --i386 <image>/I386 --out /srv/retro-pxe/nicdb.json
  3. Generate the TFTP payload with the product key from Key Vault:
       PRODUCT_KEY=$(az keyvault secret show --vault-name nsc-secrets-kv \
            --name fleet-winxp-pro-sp3-x14-80428-key --query value -o tsv) \
       WIPE=1 XP_SOURCE=<image> bash scripts/pxe/make-xp-source.sh
  4. Start and verify:
       sudo systemctl start retro-pxe
       sudo python3 scripts/pxe/pxe_selftest.py
NEXT
