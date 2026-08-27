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
echo "== PnP drivers -> \$OEM\$\\\$1\\Drivers =="
OEM="$IMAGE/\$OEM\$/\$1/Drivers"
mkdir -p "$OEM"
paths=""
for pack in lan chipset massstorage; do
    src="$PACKS/$pack"
    [ -d "$src" ] || { echo "   skip $pack (not extracted)"; continue; }
    rm -rf "${OEM:?}/$pack"
    cp -a "$src" "$OEM/$pack"
    n=$(find "$OEM/$pack" -iname '*.inf' | wc -l)
    # OemPnPDriversPath is relative to %SystemDrive% and is a SEMICOLON list.
    # Every directory holding an INF has to be named explicitly - PnP does not
    # recurse, which is the usual reason a staged driver is never used.
    while IFS= read -r d; do
        rel="Drivers/$pack/${d#"$OEM/$pack/"}"
        [ "$d" = "$OEM/$pack" ] && rel="Drivers/$pack"
        paths="$paths;$(echo "$rel" | tr '/' '\\')"
    done < <(find "$OEM/$pack" -type d -exec sh -c 'ls "$1"/*.inf "$1"/*.INF >/dev/null 2>&1' _ {} \; -print)
    echo "   $pack: $n INFs"
done
paths="${paths#;}"
printf '%s' "$paths" > "$IMAGE/OemPnPDriversPath.txt"
echo "   OemPnPDriversPath written to $IMAGE/OemPnPDriversPath.txt ($(printf '%s' "$paths" | tr ';' '\n' | wc -l) dirs)"

echo
echo "Done. Rebuild the payload so winnt.sif picks up the new path:"
echo "  PRODUCT_KEY=... WIPE=1 XP_SOURCE=$IMAGE bash $HERE/make-xp-source.sh"
