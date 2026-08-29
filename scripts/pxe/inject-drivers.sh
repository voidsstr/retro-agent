#!/usr/bin/env bash
# inject-drivers.sh - widen an XP PXE image's hardware support.
#
# THE PROBLEM THIS SOLVES. Retail XP SP3 media knows about 151 PCI network
# adapters, and only SIX of them are Intel. A machine whose NIC is not in that
# list stops in text-mode setup with:
#
#   "The operating system image you selected does not have the required drivers"
#
# and there is no way round it at run time: text-mode setup needs the NIC
# driver in order to reach the install source, so it cannot download one. The
# driver has to be in the image beforehand. Observed on a Gateway 550 (440BX)
# and a second box, 2026-08-26.
#
# TWO TIERS, because the two halves of setup load drivers differently:
#
#   text-mode  needs the NIC driver FLAT in I386. This is what unblocks the
#              install. Sourced from DriverPacks LAN-RIS, which exists for
#              exactly this and carries 276 PCI IDs.
#   GUI-mode   picks up everything else by PnP from $OEM$\$1\Drivers\..., which
#              winnt.sif points at with OemPnPDriversPath. LAN (502 IDs),
#              chipset and mass-storage go here.
#
# COLLISIONS ARE REAL AND MUST NOT BE SILENT. Flattening LAN-RIS gives 13
# name clashes out of 90 files - several Realtek generations ship a
# same-named .sys with different content, and an INF names its .sys by that
# name. Blind flattening silently drops a vendor. The rules below prefer the
# XP build over 2000/2003/Vista, drop x64 outright (this is an x86 image), and
# for anything still ambiguous keep the variant covering the most device IDs
# and PRINT what was dropped.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${IMAGE:-/mnt/retro-share/Files/OS/XPSP3-FLEET}"
PACKS="${PACKS:-/srv/retro-pxe/driverpacks}"

[ -d "$IMAGE/I386" ] || { echo "no I386 under $IMAGE (set IMAGE)" >&2; exit 1; }
[ -d "$PACKS/ris" ] || { echo "LAN-RIS not extracted at $PACKS/ris" >&2; exit 1; }

ids() { grep -aohiE 'VEN_[0-9A-F]{4}&DEV_[0-9A-F]{4}' "$1" 2>/dev/null \
        | tr 'a-z' 'A-Z' | sort -u | wc -l; }

echo "image : $IMAGE"
echo "packs : $PACKS"
echo

# ---- tier 1: text-mode NIC drivers, flat into I386 -----------------------
echo "== text-mode NIC drivers -> I386 =="
# Resolve collisions PER DIRECTORY, not per file. An INF names its .sys by
# filename, so taking net*.inf from one vendor build and its .sys from another
# produces a driver that looks installed and cannot bind. Directories are
# ranked by how many device IDs their INFs cover, then copied best-first with
# first-wins - so every file a driver needs comes from one consistent build.
kept=0; skipped=0
MANIFEST="$(mktemp)"
trap 'rm -f "$MANIFEST"' EXIT
ranked=$(for d in $(find "$PACKS/ris/D/LR" -type d | sort); do
    case "$d" in
        *64|*[Vv][Ii][Ss][Tt][Aa]*|*Win2003*|*/2000) continue ;;
    esac
    # find -iname, NOT `ls *.inf *.INF`: ls returns non-zero when EITHER glob
    # fails to match, even though the other matched - so every directory whose
    # INFs are upper-case only was judged empty and skipped. That silently cost
    # the entire Intel set (I1/E1000325.INF) and left the image with 12 drivers.
    infs=$(find "$d" -maxdepth 1 -iname '*.inf' 2>/dev/null)
    [ -n "$infs" ] || continue
    n=$(cat $infs 2>/dev/null | grep -aohiE 'VEN_[0-9A-F]{4}&DEV_[0-9A-F]{4}' \
        | tr 'a-z' 'A-Z' | sort -u | wc -l)
    echo "$n|$d"
done | sort -t'|' -k1,1nr)

while IFS='|' read -r n d; do
    [ -n "$d" ] || continue
    for f in "$d"/*; do
        [ -f "$f" ] || continue
        case "$f" in *.inf|*.INF|*.sys|*.SYS) ;; *) continue ;; esac
        case "$(basename "$f")" in *64.sys|*64.SYS|*64.inf|*64.INF) continue ;; esac
        target="$IMAGE/I386/$(basename "$f")"
        if [ -e "$target" ]; then
            # Already placed - possibly by an earlier run of this script, whose
            # registration may not have happened. Record it anyway so the
            # registrar is idempotent rather than a one-shot.
            if cmp -s "$f" "$target"; then
                basename "$f" >> "$MANIFEST"
            else
                skipped=$((skipped+1))
            fi
            continue
        fi
        cp -f "$f" "$target"
        basename "$f" >> "$MANIFEST"
        kept=$((kept+1))
    done
done <<< "$ranked"
echo "   $kept driver files placed in I386, $skipped superseded by a higher-ranked build"

# Placing the files is only half of it. Text-mode setup does NOT enumerate
# I386 - it only knows what txtsetup.sif lists, so an unregistered driver is
# invisible no matter that it is physically present. That is exactly how the
# Gateway 550 kept reporting "does not have the required drivers" against an
# image containing its driver: 52 of the 54 injected files were unlisted.
# NICs are matched through these INF listings and not through
# [HardwareIdsDatabase], which in retail XP SP3 holds 224 entries of which not
# one is a network adapter.
if [ "$kept" -gt 0 ] || [ -s "$MANIFEST" ]; then
    python3 "$HERE/register-txtsetup.py" "$IMAGE/I386/txtsetup.sif" \
            --from-file "$MANIFEST" | sed 's/^/   /'
fi

# ---- tier 2: PnP drivers under $OEM$ -------------------------------------
echo
echo "== PnP drivers -> \$OEM\$\\\$1\\D =="
# The root is "D", not "Drivers", and names are three characters. That is not
# cosmetic. OemPnPDriversPath is ONE semicolon-separated registry value that XP
# truncates around 4096 characters - silently, so every driver past the cut is
# simply never found. "Drivers\L001;" costs 13 characters per entry and 385
# directories would need 5005; "D\L001;" costs 7 and needs 2695. The short root
# is what makes shipping graphics and sound possible at all.
OEM="$IMAGE/\$OEM\$/\$1/D"
mkdir -p "$OEM"
paths=""

# One letter per pack, so a directory name says where it came from.
#   L lan   C chipset   M massstorage
#   G/H/I graphics A/B/C     S/T sound A/B     N monitor
for spec in "lan:L" "chipset:C" "massstorage:M" \
            "graphics_a:G" "graphics_b:H" "graphics_c:I" \
            "sound_a:S" "sound_b:T" "monitor:N"; do
    pack="${spec%%:*}"; initial="${spec##*:}"
    src="$PACKS/$pack"
    [ -d "$src" ] || { echo "   skip $pack (not extracted)"; continue; }
    n=$(find "$src" -iname '*.inf' | wc -l)
    idx=0
    # Copy whole directories, never individual files: an INF names its .sys by
    # filename, so mixing two vendor builds yields a driver that looks installed
    # and cannot bind.
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        idx=$((idx+1))
        short=$(printf '%s%03d' "$initial" "$idx")
        rm -rf "${OEM:?}/$short"
        mkdir -p "$OEM/$short"
        find "$d" -maxdepth 1 -type f -exec cp -f {} "$OEM/$short/" \;
        paths="$paths;D\\$short"
    done < <(find "$src" -type d | sort | while IFS= read -r cand; do
                 [ -n "$(find "$cand" -maxdepth 1 -iname '*.inf' 2>/dev/null)" ] \
                     && echo "$cand"
             done)
    echo "   $pack: $n INFs in $idx directories -> ${initial}001..$(printf '%s%03d' "$initial" "$idx")"
done

# 3dfx (Amigamerlin) is staged by hand rather than from a pack, and was
# previously left OUT of the path list - staged but unreachable, which is the
# same silent failure as not shipping it at all.
if [ -d "$IMAGE/\$OEM\$/\$1/Drivers/3dfx" ]; then
    rm -rf "${OEM:?}/3DFX"
    cp -a "$IMAGE/\$OEM\$/\$1/Drivers/3dfx" "$OEM/3DFX"
    rm -rf "$IMAGE/\$OEM\$/\$1/Drivers"
fi
if [ -d "$OEM/3DFX" ]; then
    # The Amigamerlin tree has SPACES in its directory names ("Amigamerlin 3.1
    # R1"). OemPnPDriversPath is a semicolon-separated list that XP parses
    # loosely, and a path with spaces in it is asking for trouble - so these get
    # flattened to short names like every other pack rather than being listed
    # in place.
    idx=0
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        idx=$((idx+1))
        short=$(printf 'V%03d' "$idx")
        rm -rf "${OEM:?}/$short"
        mkdir -p "$OEM/$short"
        find "$d" -maxdepth 1 -type f -exec cp -f {} "$OEM/$short/" \;
        paths="$paths;D\\$short"
    done < <(find "$OEM/3DFX" -type d | sort | while IFS= read -r cand; do
                 [ -n "$(find "$cand" -maxdepth 1 -iname '*.inf' 2>/dev/null)" ] \
                     && echo "$cand"
             done)
    rm -rf "${OEM:?}/3DFX"
    echo "   3dfx: $idx directory(ies) -> V001..$(printf 'V%03d' "$idx")"
fi

paths="${paths#;}"
printf '%s' "$paths" > "$IMAGE/OemPnPDriversPath.txt"

# A SECOND, SHORT list holding only the drivers setup needs DURING GUI setup.
#
# DevicePath is written by cmdlines.txt at T-12, but XP installs network devices
# as part of GUI setup - partly BEFORE that point. So the full list arrives too
# late for the NIC on some machines and in time on others, which is exactly what
# happened: of two identical-looking installs, one came up with working
# networking and the other with an unconfigured network adapter.
#
# LAN and chipset alone are ~102 directories and about 713 characters, which
# fits winnt.sif comfortably (the full 492 needed 3470 and broke the answer
# file). Everything else - graphics, sound, mass storage, monitor - is not
# needed to get the machine on the network.
#
# IT DOES NOT "WAIT FOR DevicePath" - IT IS NEVER USED AT ALL. That is what this
# comment used to say and it was wrong. cmdlines.txt runs at T-12, which is
# AFTER GUI setup has installed the devices, so a driver reachable only through
# DevicePath is copied to C:\D, indexed, and never consulted. Measured on the
# freshly imaged .124 (2026-08-29): its setupapi.log contains exactly SIX
# "Found ... in C:\D\" lines for the whole install, and every one names an L
# (LAN) or a C (chipset) directory. Not one G, H, I, M, N, S or T. Its GeForce2
# GTS came up on Microsoft's in-box nv4 at 800x600 in 16-bit colour with
# ForceWare 71.89 sitting unused on its own disk.
#
# DO NOT "FIX" THIS BY LENGTHENING THE EARLY LIST. Two reasons:
#   - It would not work. XP penalises an untrusted driver node by +0x8000
#     ("#I087 Driver node not trusted, rank changed from 0x2000 to 0xa000"), so
#     an unsigned INF loses to any trusted in-box match however new it is;
#     DriverSigningPolicy=Ignore suppresses the dialog, not the rank.
#   - It is actively dangerous. LAN+chipset+graphics is ~171 directories, about
#     1197 characters, against make-xp-source.sh's OEMPNP_MAX of 1200 - and when
#     the list exceeds that, it is dropped ENTIRELY, taking LAN and chipset with
#     it. Growing this list to gain graphics can therefore cost the machine its
#     network.
#
# The mechanism that DOES work is a forced install after setup: see
# scripts/pxe/driver-prefs.txt -> $OEM$\$1\D\PREFER.TXT and
# agent/src/gamesync.c:gs_apply_driver_prefs().
early=""
for d in $(ls "$OEM" 2>/dev/null | grep -E '^[LC][0-9]{3}$' | sort); do
    early="$early;D\\$d"
done
early="${early#;}"
printf '%s' "$early" > "$IMAGE/OemPnPDriversPathEarly.txt"
echo "   early path (LAN+chipset for winnt.sif): $(printf '%s' "$early" | tr ';' '\n' | wc -l) dirs, ${#early} chars"
ndirs=$(printf '%s' "$paths" | tr ';' '\n' | wc -l)
nchars=${#paths}
echo "   OemPnPDriversPath: $ndirs dirs, $nchars chars -> $IMAGE/OemPnPDriversPath.txt"
if [ "$nchars" -gt 4000 ]; then
    echo "   WARNING: $nchars chars exceeds the ~4096 XP handles reliably;" >&2
    echo "            trailing directories will be silently ignored." >&2
fi

echo
echo "Done. Rebuild the payload so winnt.sif picks up the new path:"
echo "  PRODUCT_KEY=... WIPE=1 XP_SOURCE=$IMAGE bash $HERE/make-xp-source.sh"
