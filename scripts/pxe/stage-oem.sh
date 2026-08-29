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
    # "." means THIS MACHINE. The fleet convention is to put the hostname here,
    # but the image cannot know a hostname that setup generates at install time,
    # and an empty value makes Winlogon guess. A literal dot is the documented
    # way to say "the local account database" and is correct on every box
    # without templating anything.
    printf '"DefaultDomainName"="."\r\n'
    # Remove the countdown if setup wrote one. While AutoLogonCount exists
    # Windows decrements it and, at zero, deletes AutoAdminLogon and
    # DefaultPassword - so leaving it turns permanent auto-login into
    # exactly-one auto-login, and the agent never starts again.
    printf '"AutoLogonCount"=-\r\n\r\n'
    # XP SP3 installs with the firewall ON, which drops inbound 9898 - the
    # agent starts, logs happily, and is simply unreachable, which looks
    # exactly like it failing to start.
    #
    # We used to open just port 9898 and leave the firewall up. That was not
    # enough, and the reason only showed up once we started launching games:
    #
    #   1. Every networked game binds a socket, so on FIRST RUN the firewall
    #      throws a "Windows Security Alert" modal over the game. On a
    #      fullscreen title that dialog steals focus and can make the game bail
    #      during init - which reads as "the game is broken", not as a firewall
    #      prompt. It also sits on top of any screenshot we take.
    #   2. Even after dismissing it, the LAN multiplayer these boxes exist for
    #      is still blocked. Silencing the notification alone would hide the
    #      symptom while keeping the fault.
    #
    # So turn the firewall OFF on both profiles. These are isolated-LAN retro
    # boxes already running an unauthenticated agent on 9898; the firewall is
    # not what is protecting them, and it costs us the thing they are for.
    # DoNotAllowExceptions=0 and DisableNotifications=1 keep it quiet even if
    # something re-enables the service later.
    printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\StandardProfile]\r\n'
    printf '"EnableFirewall"=dword:00000000\r\n'
    printf '"DoNotAllowExceptions"=dword:00000000\r\n'
    printf '"DisableNotifications"=dword:00000001\r\n\r\n'
    printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\DomainProfile]\r\n'
    printf '"EnableFirewall"=dword:00000000\r\n'
    printf '"DoNotAllowExceptions"=dword:00000000\r\n'
    printf '"DisableNotifications"=dword:00000001\r\n\r\n'
    # Keep the explicit 9898 rule too. It is harmless with the firewall off and
    # it means the agent is still reachable if anyone turns the firewall back on.
    printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\StandardProfile\\GloballyOpenPorts\\List]\r\n'
    printf '"9898:TCP"="9898:TCP:*:Enabled:Retro Agent"\r\n\r\n'
    printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\DomainProfile\\GloballyOpenPorts\\List]\r\n'
    printf '"9898:TCP"="9898:TCP:*:Enabled:Retro Agent"\r\n'
} > "$OEM/retroagent.reg"
echo "   retroagent.reg: auto-login (no count) + Run key + firewall OFF (+9898 rule)"

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
    # ANSI, one byte per character - NOT UTF-16.
    #
    # This file is REGEDIT4, which is an ANSI format. A hex(2) value in it is
    # read byte-for-byte as ANSI. Encoding it UTF-16LE (the .reg v5 convention)
    # writes '%' followed by 00, so regedit reads one character, hits the NUL,
    # and stores a DevicePath of literally "%". PnP then has no driver search
    # path at all - which is how a Dell Dimension 3000 came up on the VGA
    # fallback at 640x480 in 16 colours with its Intel 865G driver sitting
    # unused in C:\D.
    #
    # It is easy to verify this the same wrong way it was written: decode the
    # hex as UTF-16 and it looks perfect. Decode it as ANSI, the way regedit
    # does, and the bug is obvious. tests/test_pxe_devicepath.py does the latter.
    fh.write('"DevicePath"=hex(2):' + ','.join('%02x' % ord(c) for c in value + '\0') + '\r\n')
print('   DevicePath: %d dirs, %d chars' % (len(abs_paths.split(';')), len(value)))
PYEOF
fi

# ---- 3c. PowerStrip ------------------------------------------------------
# PowerStrip ships PRE-INSTALLED rather than as an installer run during setup:
# its installer is a Gentee package with no silent switch (the stub does not even
# import GetCommandLineA, so it never reads argv) and cannot be automated. It was
# installed once in a build VM, captured, and staged into $OEM$.
#
# The whole registry block is generated by ONE python step. Building it from
# shell printf and piping fragments through python is how the first attempt
# wrote a literal "%s" into ImagePath and left a bare LF in a CRLF file - both
# silent, both fatal in their own way.
if [ -d "$OEM/\$Progs/PowerStrip" ]; then
    python3 - "$OEM/retroagent.reg" <<'PYPS'
import io, sys

IMAGE_PATH = r'\??\C:\WINDOWS\system32\DRIVERS\PSTRIP.SYS'
# REGEDIT4 is ANSI: a hex(2) value is read byte-for-byte, one byte per
# character. Encoding it UTF-16 gives a value that stops at the first NUL.
hexed = ','.join('%02x' % ord(c) for c in IMAGE_PATH + '\0')

KEY = ('0QTswSZFr2MXGiDhyGk1QfrBtnTvy/wX/228nsH5IaFk+ydoGmfxjv+QKg1+oRnlmvPvbO'
       'pS4hznIkAlkaHmNP1ZlXmFddmv4UQci8xoOeOr05EYDOziHjig2M0MR6xsngfpudYjQ2Qz'
       '6D7Gx9lwcCUYkIHuxIkqPtU4gYv/Zl0Q=')

lines = [
  '',
  '; PowerStrip 3.90 (EnTech Taiwan, final release 2012-03-25). Files are staged',
  '; in $OEM$; these three keys are what make a copied install a working one.',
  '',
  '; 1. The kernel service. Start=3 is DEMAND start - it loads when PowerStrip is',
  ';    opened, never at boot. PowerStrip reprograms display timings, so on a',
  ';    games fleet it should only ever run when somebody asks for it.',
  '[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\PStrip]',
  '"Type"=dword:00000001',
  '"Start"=dword:00000003',
  '"ErrorControl"=dword:00000001',
  '"Group"="Extended Base"',
  '"DisplayName"="PSTRIP"',
  '"ImagePath"=hex(2):' + hexed,
  '',
  '; 2. EnTech\'s own product key, published free of charge for the discontinued',
  ';    PowerStrip 3: https://www.entechtaiwan.com/util/ps.shtm ("Product key").',
  ';    This is the vendor\'s licence, not a bypass, and it is what clears the',
  ';    trial nag. Imported at T-12 so HKCU is the Default User hive and every',
  ';    profile created afterwards inherits it.',
  '[HKEY_CURRENT_USER\\Software\\EnTech\\PowerStrip]',
  '"Key"="' + KEY + '"',
  '',
  '; 3. No Run entry. The installer\'s "Auto-load with Windows" checkbox is the',
  ';    actual source of the prompt at login; nothing started, nothing to nag.',
  '[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run]',
  '"PowerStrip"=-',
  '',
  '[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run]',
  '"PowerStrip"=-',
  '',
]
with io.open(sys.argv[1], 'a', encoding='latin1', newline='') as fh:
    fh.write('\r\n'.join(lines))
print('   PowerStrip: service + vendor key + no-autostart written')
PYPS
fi

# ---- 4. cmdlines.txt ------------------------------------------------------
printf '[Commands]\r\n"regedit /s retroagent.reg"\r\n' > "$OEM/cmdlines.txt"
echo "   cmdlines.txt: merges retroagent.reg at T-12"

echo
echo "\$OEM\$ staged. Contents:"
find "$OEM" -maxdepth 2 -mindepth 1 -printf '   %P\n' | sort | head -20
