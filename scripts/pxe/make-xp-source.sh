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
# The CD contents themselves are NOT copied here: they stay on the NAS. They are
# reached two different ways, and both matter - setupldr pulls txtsetup.sif over
# TFTP through the symlink this script creates, and text-mode setup then reads
# the rest over SMB1. See the README's SMB1 section for why that host has to be
# the NAS and not this one.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${XP_SOURCE:-/mnt/retro-share/Files/OS/XPSP3-PXE}"
TFTP_ROOT="${TFTP_ROOT:-/srv/retro-pxe/tftp}"
# What the target machine will be told to mount. These are consumed by XP's
# setup, not by anything on this host, so they stay in Windows/UNC form. Same
# two parameters the PowerShell sibling takes.
SHARE_UNC="${SHARE_UNC:-\\\\192.168.1.122\\files\\Files\\OS\\XPSP3-PXE}"
SOURCE_DEV="${SOURCE_DEV:-\\Device\\LanmanRedirector\\192.168.1.122\\files\\Files\\OS\\XPSP3-PXE}"

# Answer-file values. ComputerName "*" makes setup generate one from OrgName,
# which is what you want when imaging several boxes from one payload - a fixed
# name would collide the moment two are on the LAN together.
COMPUTERNAME="${COMPUTERNAME:-*}"
FULLNAME="${FULLNAME:-Retro Fleet}"
ORGNAME="${ORGNAME:-NSC Retro Fleet}"
WORKGROUP="${WORKGROUP:-WORKGROUP}"
TIMEZONE="${TIMEZONE:-35}"          # 35 = Eastern (US & Canada); the fleet is EDT
# PRODUCT_KEY decides how hands-off this is, and the script refuses to pretend.
# With a key it writes a FULLY unattended file; without one it falls back to
# ProvideDefault, which fills every page in and still shows them so the operator
# can type the key. A FullUnattended file with no ProductKey does not "mostly
# work" - setup stops dead on the missing entry, which on this media is the one
# thing we cannot answer for it (I386\SETUPP.INI reads Pid=76487000, the
# trailing 000 being the retail channel).
if [ -n "${PRODUCT_KEY:-}" ]; then
    UNATTEND_MODE="FullUnattended"
else
    UNATTEND_MODE="ProvideDefault"
    echo "NOTE: no PRODUCT_KEY set - building a semi-attended file; setup will" >&2
    echo "      show the pages with defaults filled and stop for the key." >&2
fi

# WIPE controls whether setup owns the whole disk. Default NO, because these
# machines are re-used and the destructive setting is not one to inherit by
# accident. WIPE=1 gives the hands-off re-image: every existing partition is
# deleted, one is created across the disk, formatted NTFS, and installed to.
if [ "${WIPE:-0}" = "1" ]; then
    AUTOPARTITION=1
    REPARTITION="Yes"
    FILESYSTEM="ConvertNTFS"
else
    AUTOPARTITION=0
    REPARTITION="No"
    FILESYSTEM="LeaveAlone"
fi

# $OEM$ and the PnP driver path are picked up from the image automatically, so
# a payload built from an injected image gets them without extra flags.
if [ -d "$SRC/\$OEM\$" ]; then
    OEMPREINSTALL="Yes"
else
    OEMPREINSTALL="No"
fi
OEMPNP="${OEMPNP:-}"
if [ -z "$OEMPNP" ] && [ -f "$SRC/OemPnPDriversPath.txt" ]; then
    OEMPNP="$(cat "$SRC/OemPnPDriversPath.txt")"
fi

command -v cabextract >/dev/null || {
    echo "cabextract not installed: sudo apt-get install cabextract" >&2; exit 1; }
[ -d "$SRC/I386" ] || {
    echo "XP source not found at $SRC/I386 (set XP_SOURCE)" >&2; exit 1; }

mkdir -p "$TFTP_ROOT"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ "${WIPE:-0}" = "1" ]; then
    echo "*** WIPE=1: the answer file will DELETE EVERY PARTITION on the target"
    echo "*** disk, create one across the whole drive, format NTFS and install."
    echo
fi
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
# Generated inline from the parameters above, exactly as make-xp-source.ps1
# does - winnt.sif.template stays a REFERENCE copy of the result, not an input,
# so the two builders cannot drift apart through a template only one of them
# reads.
#
# WHY THIS IS A FULL ANSWER FILE AND NOT THREE LINES.
#
# floppyless=1 plus MsDosInitiated=1 tells setup this is an UNATTENDED install,
# and from that moment it stops prompting and starts DEMANDING. A file with
# only [Data] and [SetupData] gets you:
#
#   "the entry \"computername\" in the [userdata] section of the inf file is
#    corrupt or missing"
#
# and then, one reboot at a time, the same complaint for every other entry it
# wanted. The fix is not to add ComputerName; it is to answer the whole
# questionnaire. Every section below exists to stop one of those stops.
#
# UnattendMode = ProvideDefault is deliberate. This is RETAIL media
# (I386\SETUPP.INI reads Pid=76487000 - the trailing 000 is the retail
# channel), so setup wants a product key and we do not ship one. ProvideDefault
# fills every page in and still SHOWS them, so the operator types the key and
# clicks through instead of hitting an error. Set PRODUCT_KEY in the
# environment to make it fully unattended.
#
# Two settings are deliberately conservative because this runs against machines
# with data on them:
#   AutoPartition = 0   the operator picks the partition; setup does not choose
#   Repartition   = No  never wipe the disk unasked
#
# AdminPassword and AutoLogon follow the fleet convention in CLAUDE.md - the
# console account password is "password" on every box, and auto-login is what
# lets the retro agent come back by itself after a reboot.
{
    printf '[Data]\r\n'
    printf '    AutoPartition = %s\r\n' "$AUTOPARTITION"
    printf '    MsDosInitiated = "1"\r\n'
    printf '    UnattendedInstall = "Yes"\r\n'
    printf '    floppyless = "1"\r\n'
    printf '    OriSrc = "%s\\I386"\r\n' "$SHARE_UNC"
    printf '    OriTyp = "4"\r\n'
    printf '    LocalSourceOnCD = 1\r\n'
    printf '\r\n'
    printf '[SetupData]\r\n'
    printf '    OsLoadOptions = "/fastdetect"\r\n'
    printf '    SetupSourceDevice = "%s"\r\n' "$SOURCE_DEV"
    printf '\r\n'
    printf '[Unattended]\r\n'
    printf '    UnattendMode = %s\r\n' "$UNATTEND_MODE"
    # $OEM$ only exists for setup when OemPreinstall is Yes. Without it the
    # whole tree - drivers, the agent, C:\Games - is silently ignored and you
    # get a clean install with none of it, and no error saying why.
    printf '    OemPreinstall = %s\r\n' "$OEMPREINSTALL"
    printf '    OemSkipEula = Yes\r\n'
    printf '    TargetPath = \\WINDOWS\r\n'
    printf '    FileSystem = %s\r\n' "$FILESYSTEM"
    printf '    NtUpgrade = No\r\n'
    printf '    OverwriteOemFilesOnUpgrade = No\r\n'
    printf '    Repartition = %s\r\n' "$REPARTITION"
    printf '    DriverSigningPolicy = Ignore\r\n'
    # PnP does NOT recurse: every directory holding an INF must be listed.
    # inject-drivers.sh writes the list next to the image.
    if [ -n "${OEMPNP:-}" ]; then
        printf '    OemPnPDriversPath = "%s"\r\n' "$OEMPNP"
    fi
    printf '    WaitForReboot = No\r\n'
    printf '\r\n'
    printf '[GuiUnattended]\r\n'
    printf '    AdminPassword = "password"\r\n'
    printf '    EncryptedAdminPassword = No\r\n'
    printf '    AutoLogon = Yes\r\n'
    printf '    AutoLogonCount = 1\r\n'
    printf '    OEMSkipRegional = 1\r\n'
    printf '    OemSkipWelcome = 1\r\n'
    printf '    TimeZone = %s\r\n' "$TIMEZONE"
    printf '\r\n'
    printf '[UserData]\r\n'
    printf '    ComputerName = %s\r\n' "$COMPUTERNAME"
    printf '    FullName = "%s"\r\n' "$FULLNAME"
    printf '    OrgName = "%s"\r\n' "$ORGNAME"
    if [ -n "${PRODUCT_KEY:-}" ]; then
        printf '    ProductKey = "%s"\r\n' "$PRODUCT_KEY"
    fi
    printf '\r\n'
    printf '[RegionalSettings]\r\n'
    printf '    LanguageGroup = 1\r\n'
    printf '    Language = 00000409\r\n'
    printf '\r\n'
    printf '[Networking]\r\n'
    printf '    InstallDefaultComponents = Yes\r\n'
    printf '\r\n'
    printf '[Identification]\r\n'
    printf '    JoinWorkgroup = %s\r\n' "$WORKGROUP"
} > "$TFTP_ROOT/winnt.sif"

# --- the source tree, reachable over TFTP ---------------------------------
# setupldr does NOT switch to SMB when it loads txtsetup.sif. It keeps using
# TFTP, taking the path out of winnt.sif and dropping the UNC prefix - so a
# winnt.sif naming \\192.168.1.122\files\Files\OS\XPSP3-PXE makes it ask TFTP
# for \Files\OS\XPSP3-PXE\i386\txtsetup.sif, relative to the TFTP root.
#
# Without this the boot dies at "txtsetup.sif is corrupt or missing", which
# reads like a bad copy of the CD and is nothing of the sort. Observed on the
# Gateway 440BX, 2026-08-26.
#
# A symlink is enough: the resolver rejects ".." but does not resolve symlinks
# against the root, and the case-insensitive walk handles i386 -> I386 and
# txtsetup.sif -> TXTSETUP.SIF.
SHARE_ROOT="${SHARE_ROOT:-/mnt/retro-share}"
LINK_NAME="$(printf '%s' "$SHARE_UNC" | sed 's|.*\\files\\||; s|\\.*||')"
if [ -d "$SHARE_ROOT/$LINK_NAME" ]; then
    ln -sfn "$SHARE_ROOT/$LINK_NAME" "$TFTP_ROOT/$LINK_NAME"
    echo "linked $TFTP_ROOT/$LINK_NAME -> $SHARE_ROOT/$LINK_NAME"
else
    echo "WARNING: $SHARE_ROOT/$LINK_NAME not found - setupldr will not be able" >&2
    echo "         to TFTP txtsetup.sif and the boot will stop there." >&2
fi

echo
printf '%-16s %s\n' "file" "bytes"
for f in startrom.n12 ntldr ntdetect.com winnt.sif; do
    printf '%-16s %s\n' "$f" "$(stat -c%s "$TFTP_ROOT/$f")"
done
echo
echo "Payload ready.   mode=$UNATTEND_MODE  autopartition=$AUTOPARTITION  repartition=$REPARTITION"
echo "setupldr TFTPs txtsetup.sif through the link above; text-mode setup then"
echo "reads the rest of the CD from the NAS over SMB1."
