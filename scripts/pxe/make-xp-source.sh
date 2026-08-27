#!/usr/bin/env bash
# make-xp-source.sh - build the PXE TFTP payload for a Windows XP network
# install, on the LINUX fleet host. The PowerShell sibling
# (make-xp-source.ps1) does the same job on the Windows host.
#
# It needs FOUR files in the TFTP root, and only one of them keeps its name:
#
#   startrom.n12   <- I386\STARTROM.N1_   the PXE boot ROM image. The ".n12"
#                     variant is the one that does NOT wait for a keypress;
#                     STARTROM.COM does, and on a headless bring-up that looks
#                     exactly like a hang.
#   ntldr          <- I386\SETUPLDR.EX_   THE RENAME IS THE POINT. startrom
#                     asks TFTP for the literal name "ntldr", and what it wants
#                     there is setup's loader, not the installed system's
#                     ntldr. Copy SETUPLDR under its own name and the boot dies
#                     with no useful message.
#   ntdetect.com   <- I386\NTDETECT.COM   copied as-is.
#   winnt.sif      generated - tells setup where the install source is.
#
# The compressed ones are CABs (MSCF magic), not the SZDD that "expand" implies,
# so cabextract is the tool - msexpand fails on them.
#
# The CD contents themselves are NOT copied here: they stay on the NAS and
# setup pulls them over SMB1. See the README's SMB1 section for why that host
# has to be the NAS and not this one.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${XP_SOURCE:-/mnt/retro-share/Files/OS/XPSP3-PXE}"
TFTP_ROOT="${TFTP_ROOT:-/srv/retro-pxe/tftp}"
# What the target machine will be told to mount. These are consumed by XP's
# setup, not by anything on this host, so they stay in Windows/UNC form. Same
# two parameters the PowerShell sibling takes.
SHARE_UNC="${SHARE_UNC:-\\\\192.168.1.122\\files\\Files\\OS\\XPSP3-PXE}"
SOURCE_DEV="${SOURCE_DEV:-\\Device\\LanmanRedirector\\192.168.1.122\\files\\Files\\OS\\XPSP3-PXE}"

command -v cabextract >/dev/null || {
    echo "cabextract not installed: sudo apt-get install cabextract" >&2; exit 1; }
[ -d "$SRC/I386" ] || {
    echo "XP source not found at $SRC/I386 (set XP_SOURCE)" >&2; exit 1; }

mkdir -p "$TFTP_ROOT"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "source:    $SRC"
echo "tftp root: $TFTP_ROOT"

# --- startrom.n12 --------------------------------------------------------
cabextract -q -d "$work" "$SRC/I386/STARTROM.N1_"
found=$(find "$work" -maxdepth 1 -type f | head -1)
[ -n "$found" ] || { echo "STARTROM.N1_ produced nothing" >&2; exit 1; }
install -m 0644 "$found" "$TFTP_ROOT/startrom.n12"
rm -f "$work"/*

# --- ntldr (this is SETUPLDR, renamed - see the header) ------------------
cabextract -q -d "$work" "$SRC/I386/SETUPLDR.EX_"
found=$(find "$work" -maxdepth 1 -type f | head -1)
[ -n "$found" ] || { echo "SETUPLDR.EX_ produced nothing" >&2; exit 1; }
install -m 0644 "$found" "$TFTP_ROOT/ntldr"
rm -f "$work"/*

# --- ntdetect.com --------------------------------------------------------
install -m 0644 "$SRC/I386/NTDETECT.COM" "$TFTP_ROOT/ntdetect.com"

# --- winnt.sif -----------------------------------------------------------
# Generated inline from the two parameters above, exactly as make-xp-source.ps1
# does - winnt.sif.template stays a REFERENCE copy of the result, not an input,
# so the two builders cannot drift apart through a template only one of them
# reads.
#
# CRLF throughout: this is read by XP's text-mode setup, which is not forgiving
# about line endings.
{
    printf '[Data]\r\n'
    printf '    floppyless = "1"\r\n'
    printf '    msdosinitiated = "1"\r\n'
    printf '    OriSrc = "%s\\I386"\r\n' "$SHARE_UNC"
    printf '    OriTyp = "4"\r\n'
    printf '    LocalSourceOnCD = 1\r\n'
    printf '\r\n'
    printf '[SetupData]\r\n'
    printf '    OsLoadOptions = "/fastdetect"\r\n'
    printf '    SetupSourceDevice = "%s"\r\n' "$SOURCE_DEV"
} > "$TFTP_ROOT/winnt.sif"

echo
printf '%-16s %s\n' "file" "bytes"
for f in startrom.n12 ntldr ntdetect.com winnt.sif; do
    printf '%-16s %s\n' "$f" "$(stat -c%s "$TFTP_ROOT/$f")"
done
echo
echo "Payload ready. The CD itself stays on the NAS; setup fetches it over SMB1."
