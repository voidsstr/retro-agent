#!/usr/bin/env bash
# stage-oem.sh - build the $OEM$ control files an unattended XP image needs.
#
# $OEM$ is how anything of ours gets onto the machine: the agent, the drivers,
# the auto-login. Two things about it are easy to get wrong and both fail
# SILENTLY, leaving a clean-looking install with nothing to say what happened.
#
#  1. dosnet.inf must list $oem$ under [OptionalSrcDirs]. Text-mode setup copies
#     only the optional source directories named there; retail media has no such
#     section at all, because retail media has no $OEM$. Without it the whole
#     tree is ignored - no cmdlines.txt, no registry merge, no agent, no drivers,
#     and OemPnPDriversPath pointing at a directory that was never copied.
#
#  2. Auto-login must be written WITHOUT an AutoLogonCount. Windows decrements
#     that value at every logon and deletes AutoAdminLogon and DefaultPassword
#     when it hits zero, so a count of 1 gives exactly one auto-login and then a
#     logon screen forever - which on this fleet means the agent never starts.
#
# Idempotent: safe to re-run against an image that has already been staged.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${IMAGE:-/mnt/retro-share/Files/OS/XPSP3-FLEET}"
AGENT_SRC="${AGENT_SRC:-$HERE/../../agent/retro_agent.exe}"
ADMIN_PASS="${ADMIN_PASS:-password}"

[ -d "$IMAGE/I386" ] || { echo "no I386 under $IMAGE (set IMAGE)" >&2; exit 1; }

OEM="$IMAGE/\$OEM\$"
mkdir -p "$OEM/\$1/RETRO_AGENT"

echo "image: $IMAGE"

# ---- 1. dosnet.inf must advertise $oem$ ----------------------------------
DOSNET=$(find "$IMAGE/I386" -maxdepth 1 -iname 'dosnet.inf' | head -1)
if [ -z "$DOSNET" ]; then
    echo "   WARNING: no dosnet.inf in $IMAGE/I386 - \$OEM\$ will not be copied" >&2
else
    if grep -qai '^\[OptionalSrcDirs\]' "$DOSNET"; then
        if grep -qai '^\$oem\$' "$DOSNET"; then
            echo "   dosnet.inf: [OptionalSrcDirs] already lists \$oem\$"
        else
            # Append into the existing section rather than making a second one.
            python3 - "$DOSNET" <<'PY'
import sys
p = sys.argv[1]
raw = open(p, 'rb').read().decode('latin-1')
lines = raw.split('\n')
for i, l in enumerate(lines):
    if l.strip().lower() == '[optionalsrcdirs]':
        eol = '\r' if l.endswith('\r') else ''
        lines.insert(i + 1, '$oem$' + eol)
        break
open(p, 'wb').write('\n'.join(lines).encode('latin-1'))
print('   dosnet.inf: added $oem$ to the existing [OptionalSrcDirs]')
PY
        fi
    else
        cp -n "$DOSNET" "$DOSNET.preoem" 2>/dev/null || true
        # CRLF throughout: a stray LF in a setup INF is "corrupt or missing"
        # and a dead boot, which this project has already hit once.
        printf '\r\n[OptionalSrcDirs]\r\n$oem$\r\n' >> "$DOSNET"
        echo "   dosnet.inf: appended [OptionalSrcDirs] with \$oem\$"
    fi
    lf=$(python3 -c "
d=open('$DOSNET','rb').read()
print(sum(1 for l in d.split(b'\n')[:-1] if not l.endswith(b'\r')))")
    echo "   dosnet.inf: $lf LF-only line(s) (must be 0)"
    [ "$lf" = "0" ] || { echo "   FATAL: dosnet.inf has bare LFs" >&2; exit 1; }
fi

# ---- 2. the agent ---------------------------------------------------------
if [ -f "$AGENT_SRC" ]; then
    cp -f "$AGENT_SRC" "$OEM/\$1/RETRO_AGENT/retro_agent.exe.tmp"
    mv -f "$OEM/\$1/RETRO_AGENT/retro_agent.exe.tmp" \
          "$OEM/\$1/RETRO_AGENT/retro_agent.exe"
    echo "   agent: $(stat -c%s "$OEM/\$1/RETRO_AGENT/retro_agent.exe") bytes from $AGENT_SRC"
else
    echo "   WARNING: no agent at $AGENT_SRC - the box will install without one" >&2
fi

# ---- 2b. the "this machine was just imaged" flag -------------------------
# The agent needs to know a box is FRESH so it can provision it - stage the game
# library, apply the desktop theme and screensaver, and so on. Inferring that
# from the ABSENCE of a done-marker is weaker than it looks: a marker can also be
# absent because someone deleted it, or because the file never got written on a
# box that has been running for months. A flag placed BY THE IMAGE is positive
# evidence, and it carries the build date so a machine can say which image it
# came from.
{
    printf 'imaged=%s\r\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'image=%s\r\n' "$(basename "$IMAGE")"
    printf 'agent=%s\r\n' "$(stat -c%s "$OEM/\$1/RETRO_AGENT/retro_agent.exe" 2>/dev/null || echo unknown)"
} > "$OEM/\$1/RETRO_AGENT/newimage.flag"
echo "   newimage.flag: marks the box as freshly imaged for the agent"

# ---- 3. registry: permanent auto-login + the agent's Run key -------------
# REGEDIT4, not REGEDIT5: cmdlines.txt runs under XP's regedit at T-12.
{
    printf 'REGEDIT4\r\n\r\n'
    printf '[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run]\r\n'
    printf '"RetroAgent"="C:\\\\RETRO_AGENT\\\\retro_agent.exe"\r\n\r\n'
    printf '[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon]\r\n'
    printf '"AutoAdminLogon"="1"\r\n'
    printf '"ForceAutoLogon"="1"\r\n'
    printf '"DefaultUserName"="Administrator"\r\n'
    printf '"DefaultPassword"="%s"\r\n' "$ADMIN_PASS"
    printf '"DefaultDomainName"=""\r\n'
    # Remove the countdown if setup wrote one. While AutoLogonCount exists
    # Windows decrements it and, at zero, deletes AutoAdminLogon and
    # DefaultPassword - so leaving it turns permanent auto-login into
    # exactly-one auto-login, and the agent never starts again.
    printf '"AutoLogonCount"=-\r\n\r\n'
    # XP SP3 installs with the firewall ON, which drops inbound 9898 - the
    # agent starts, logs happily, and is simply unreachable, which looks
    # exactly like it failing to start. Open the one port rather than turning
    # the firewall off: these boxes are old and unpatched, and the agent needs
    # nothing else inbound.
    printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\StandardProfile\\GloballyOpenPorts\\List]\r\n'
    printf '"9898:TCP"="9898:TCP:*:Enabled:Retro Agent"\r\n\r\n'
    printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\DomainProfile\\GloballyOpenPorts\\List]\r\n'
    printf '"9898:TCP"="9898:TCP:*:Enabled:Retro Agent"\r\n'
} > "$OEM/retroagent.reg"
echo "   retroagent.reg: auto-login (no count) + Run key + firewall port 9898"

# ---- 3b. the driver search path, as a registry value ---------------------
# OemPnPDriversPath in winnt.sif is the documented way to do this, and for a
# handful of directories it is the right one. It stops being right at our scale:
# 492 directories is a 3470-character line in an INF that setupldr parses with
# fixed buffers, which took winnt.sif from 1.6 KB to 4.6 KB. A truncated line
# loses drivers silently; a line that overruns takes the whole answer file with
# it, and setup then runs INTERACTIVELY - which is precisely what a fleet
# machine did on 2026-08-27 after the driver set grew.
#
# DevicePath is where OemPnPDriversPath ultimately lands anyway, so write it
# directly at T-12 instead. A registry string has none of the INF parser's
# limits, and winnt.sif goes back to being small and boring.
if [ -f "$IMAGE/OemPnPDriversPath.txt" ]; then
    # Write DevicePath as a PLAIN REG_EXPAND_SZ string, not hex.
    #
    # The first version encoded it as hex(2) through a shell/python pipeline and
    # produced a value of literally "%" - a single character. PnP therefore had
    # no driver path at all, the Found New Hardware wizard could not find a NIC
    # driver that was sitting right there on the disk, and one of two identical
    # machines came up with no networking. The failure was invisible from the
    # build side: the .reg looked plausible and regedit merged it without
    # complaint.
    #
    # In .reg syntax every backslash is doubled, so the path separators need
    # escaping. %SystemRoot%\inf must come first or Windows loses its own
    # driver store.
    python3 - "$IMAGE/OemPnPDriversPath.txt" "$OEM/retroagent.reg" <<'PYEOF'
import io, sys
paths = io.open(sys.argv[1], encoding='latin1').read().strip()
# Relative "D\L001" -> absolute "C:\D\L001"; PnP does not resolve relatives.
abs_paths = ';'.join('C:\\' + p for p in paths.split(';') if p)
value = '%SystemRoot%\\inf;' + abs_paths
escaped = value.replace('\\', '\\\\')
with io.open(sys.argv[2], 'a', encoding='latin1', newline='') as fh:
    fh.write('\r\n')
    fh.write('[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion]\r\n')
    fh.write('"DevicePath"=hex(2):' + ','.join('%02x,00' % ord(c) for c in value + '\0') + '\r\n')
print('   DevicePath: %d dirs, %d chars' % (len(abs_paths.split(';')), len(value)))
PYEOF
fi

# ---- 4. cmdlines.txt ------------------------------------------------------
printf '[Commands]\r\n"regedit /s retroagent.reg"\r\n' > "$OEM/cmdlines.txt"
echo "   cmdlines.txt: merges retroagent.reg at T-12"

echo
echo "\$OEM\$ staged. Contents:"
find "$OEM" -maxdepth 2 -mindepth 1 -printf '   %P\n' | sort | head -20
