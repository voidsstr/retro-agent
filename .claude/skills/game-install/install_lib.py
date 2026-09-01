#!/usr/bin/env python3
"""install_lib.py — mechanical helpers for installing a share game onto a fleet box.

Used by the `game-install` skill. Everything here is the *deterministic* part of
an install (find the source, detect the format, stage it, run the silent path or
copy-in or unzip, verify by file-count parity). The GUI click-walk for installers
with no silent switch is LLM-in-the-loop and lives in the `gui-install` skill
(FastUI + CLICKSHOT); this module just gets you to the point of launching it and
verifies afterwards.

Reuses the wire client at retro-agent/client/retro_protocol.py. Import and drive
from an async context, or run as a CLI:

    python3 install_lib.py <ip> detect  "Z:\\Games\\GOG\\setup_tyrian_2000_3.01_(76355).exe"
    python3 install_lib.py <ip> install "Z:\\Games\\GOG\\setup_tyrian...exe" --dest "C:\\Games\\Tyrian2000"
    python3 install_lib.py <ip> remap        # repair a stale Z: mapping
    python3 install_lib.py <ip> writable     # find a writable install root (handles D:/UAC)
"""
import argparse
import asyncio
import json
import os
import re
import sys

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)
_RA = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _RA not in sys.path:
    sys.path.insert(0, _RA)

from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_SECRET", "retro-agent-secret")
SHARE_UNC = r"\\192.168.1.122\files"
SHARE_USER = os.environ.get("RETRO_SHARE_USER", "voidsstr")
SHARE_PASS = os.environ.get("RETRO_SHARE_PASS", "password")


async def cmd(c, x, t=90.0):
    st, d = await c.send_command(x, timeout=t)
    return d.decode("ascii", "replace")


async def env(c, name):
    out = (await cmd(c, "EXEC cmd /c echo %%%s%%" % name)).strip().splitlines()
    return out[-1].strip() if out else ""


# ---- share reachability -----------------------------------------------------

async def share_ok(c, drive="Z:"):
    """Is the share mapped and readable? Handles the 'Unavailable' stale state."""
    out = await cmd(c, r'EXEC cmd /c dir %s\Games\_games_index.json 2>&1' % drive)
    return "_games_index" in out


async def remap_share(c, drive="Z:"):
    """Repair a stale/absent Z: mapping (net use shows 'Unavailable')."""
    await cmd(c, "EXEC cmd /c net use %s /delete /y 2>&1" % drive)
    out = await cmd(c, r'EXEC cmd /c net use %s %s /user:%s %s /persistent:yes 2>&1'
                    % (drive, SHARE_UNC, SHARE_USER, SHARE_PASS))
    return await share_ok(c, drive)


# ---- writable-root discovery (dual-boot D:, Win7 UAC) -----------------------

async def writable_root(c):
    """Pick an install root that actually accepts writes on THIS box.

    Returns (drive, root, temp). Handles:
      - dual-boot XP where Windows/TEMP live on D:, not C:
      - Win7/UAC where C:\\ root is denied but C:\\Games / %TEMP% work
    Prefers a same-volume-as-TEMP root so the post-extract move is instant.
    """
    temp = await env(c, "TEMP")
    tdrive = (temp[:2] if len(temp) > 1 and temp[1] == ":" else "C:")
    for root in (r"%s\Games" % tdrive, r"C:\Games", r"D:\Games"):
        rd = root[:2]
        test = await cmd(c, r'EXEC cmd /c mkdir "%s" 2>nul & echo t>"%s\_wt.txt" 2>&1 '
                            r'&& (echo OK & del "%s\_wt.txt") || echo NO' % (root, root, root))
        if "OK" in test:
            return rd, root, temp
    return "C:", r"C:\Games", temp


# ---- format detection -------------------------------------------------------

INSTALLER_KINDS = {
    "gog_inno": "GOG/InnoSetup setup_*.exe — silent: /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /DIR=",
    "nsis": "Nullsoft (NSIS) — silent: /S  /D=<dir> (note: /D last, unquoted, no =)",
    "innosetup": "InnoSetup (non-GOG) — silent: /VERYSILENT /SUPPRESSMSGBOXES /DIR=",
    "wise": "Wise — usually NO working silent switch; extracts to %TEMP%\\tempinstall then EXITS. GUI-walk + move.",
    "installshield": "InstallShield — /s with a response .iss, else GUI-walk.",
    "clickteam": "Clickteam — GUI-walk (gui-install skill).",
    "iso": "Disc image — mount/extract, then install the setup.exe inside.",
    "zip": "ZIP archive — extract with retro_unzip.js (no unzip tool on old Windows).",
    "installed_dir": "Already-installed game folder — copy the tree in, no installer.",
    "unknown": "Unrecognized .exe — inspect strings / GUI-walk.",
}


def detect_by_name(path):
    """First-pass classification from the path alone (cheap)."""
    base = path.rstrip("\\/").split("\\")[-1]
    low = base.lower()
    if low.endswith(".iso") or low.endswith(".bin") or low.endswith(".cue"):
        return "iso"
    if low.endswith(".zip"):
        return "zip"
    if low.startswith("setup_") and low.endswith(".exe"):
        return "gog_inno"
    if low.endswith(".exe"):
        return None  # need content sniff
    return "installed_dir"  # a directory


async def detect(c, path):
    """Return (kind, note). Sniffs installer .exe content when the name is ambiguous."""
    k = detect_by_name(path)
    if k:
        return k, INSTALLER_KINDS[k]
    # a bare directory?
    isdir = "DIR" in await cmd(c, r'EXEC cmd /c if exist "%s\*" (echo DIR) else (echo FILE)' % path)
    if isdir:
        return "installed_dir", INSTALLER_KINDS["installed_dir"]
    # Sniff the .exe by testing each installer signature separately. `findstr /m`
    # prints the FILENAME (not the matched text) when a pattern is present, so a
    # non-empty result for a single-pattern search means that signature matched.
    # Order matters: Inno before Wise (some Inno stubs mention both).
    for needle, kind in [("Inno Setup", "innosetup"), ("Nullsoft", "nsis"),
                         ("Wise Installation", "wise"), ("InstallShield", "installshield"),
                         ("Clickteam", "clickteam")]:
        hit = await cmd(c, r'EXEC cmd /c findstr /m /c:"%s" "%s" >nul 2>&1 && echo YES'
                        % (needle, path))
        if "YES" in hit:
            # setup_*.exe Inno == a GOG installer (its /DIR handling is identical)
            if kind == "innosetup" and detect_by_name(path) == "gog_inno":
                kind = "gog_inno"
            return kind, INSTALLER_KINDS[kind]
    return "unknown", INSTALLER_KINDS["unknown"]


# ---- verification -----------------------------------------------------------

async def folder_bytes(c, path):
    txt = await cmd(c, 'EXECW 240 cmd /c dir "%s" /s 2>&1 | find "File(s)"' % path, 260.0)
    return sum(int(n.replace(",", "")) for n in re.findall(r"([\d,]+) bytes", txt))


async def count_files(c, path):
    txt = await cmd(c, r'EXECW 240 cmd /c dir "%s" /s /b /a-d 2>&1 | find /c /v ""' % path, 260.0)
    m = re.search(r"\d+", txt)
    return int(m.group()) if m else 0


async def poll_until_stable(c, path, min_bytes=1_000_000, every=15, tries=80):
    """Wait for a growing target to stop changing (2 stable reads)."""
    prev, stable = -1, 0
    for _ in range(tries):
        await asyncio.sleep(every)
        sz = await folder_bytes(c, path)
        if sz >= min_bytes and sz == prev:
            stable += 1
            if stable >= 2:
                return sz
        else:
            stable = 0
        prev = sz
    return prev


async def verify(c, dest, key_files):
    """True iff every key file exists under dest."""
    for f in key_files:
        r = await cmd(c, r'EXEC cmd /c if exist "%s\%s" (echo OK) else (echo NO)' % (dest, f))
        if "OK" not in r:
            return False, f
    return True, None


# ---- install paths ----------------------------------------------------------

async def install_silent(c, src, dest, kind, log=print):
    """Run a silent installer. Returns (ok, message). GUI-only kinds return (False, why)."""
    tmp = r"%s\_gi_setup.exe" % (await writable_root(c))[1]
    log("staging %s -> %s" % (src, tmp))
    out = await cmd(c, r'EXECW 900 cmd /c copy /Y "%s" "%s"' % (src, tmp), 920.0)
    if "copied" not in out:
        return False, "copy failed: " + out.strip()[:120]
    switches = {
        "gog_inno": r'/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /DIR="%s"' % dest,
        "innosetup": r'/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="%s"' % dest,
        "nsis": r'/S /D=%s' % dest,   # /D must be last, unquoted
    }.get(kind)
    if not switches:
        return False, "kind '%s' has no reliable silent switch — use the GUI walk (gui-install)" % kind
    log("silent install (%s)..." % kind)
    await cmd(c, r'EXECW 1200 cmd /c "%s" %s' % (tmp, switches), 1220.0)
    await cmd(c, r'EXEC cmd /c del /q "%s" 2>nul' % tmp)
    return True, "silent install issued"


async def install_copy_in(c, src_dir, dest, log=print):
    """Copy an already-installed game folder from the share into place."""
    log("copy-in %s -> %s" % (src_dir, dest))
    await cmd(c, r'EXEC cmd /c mkdir "%s" 2>nul' % dest)
    # `< nul` IS LOAD-BEARING, NOT TIDINESS. xcopy asks whether the target is a
    # file or a directory and reads the answer from stdin; the agent's hidden
    # CreateProcess gives the child no stdin handle, so xcopy exits IMMEDIATELY,
    # copies nothing, prints nothing and returns 0. Measured on .143 2026-09-01:
    # `EXEC cmd /c xcopy /?` produced no output at all, and `xcopy /? < nul`
    # printed the full help - so it is not "a console" that is missing, it is a
    # readable stdin, and one redirect fixes it without `start /wait` (which
    # detaches and throws the exit code away).
    out = await cmd(c, r'EXECW 1200 cmd /c xcopy "%s" "%s" /E /I /Y /Q < nul 2>&1' % (src_dir, dest), 1220.0)
    got = await count_files(c, dest)
    want = await count_files(c, src_dir)
    # The parity check stays. A redirect explains the failure we have SEEN; it
    # does not license trusting xcopy's return value, which is 0 either way.
    if got < want:  # fall back to robocopy/copy tree
        log("xcopy short (%d/%d) — retrying with robocopy" % (got, want))
        await cmd(c, r'EXECW 1200 cmd /c robocopy "%s" "%s" /E /NFL /NDL /NJH /NJS 2>&1' % (src_dir, dest), 1220.0)
        got = await count_files(c, dest)
    return got >= want, "copied %d/%d files" % (got, want)


# ---- CLI --------------------------------------------------------------------

async def _main():
    p = argparse.ArgumentParser()
    p.add_argument("ip")
    p.add_argument("action", choices=["detect", "install", "remap", "writable", "verify"])
    p.add_argument("src", nargs="?")
    p.add_argument("--dest")
    p.add_argument("--key", action="append", default=[], help="key file(s) for verify")
    a = p.parse_args()

    c = RetroConnection(a.ip, 9898)
    await c.connect(SECRET, timeout=12.0)
    try:
        if not await share_ok(c):
            print("Z: stale — remapping..."); await remap_share(c)
        if a.action == "remap":
            print("share ok:", await share_ok(c))
        elif a.action == "writable":
            print("writable root:", await writable_root(c))
        elif a.action == "detect":
            k, note = await detect(c, a.src); print("%s\n  %s" % (k, note))
        elif a.action == "verify":
            ok, missing = await verify(c, a.dest or a.src, a.key)
            print("verified" if ok else "MISSING: " + str(missing))
        elif a.action == "install":
            k, note = await detect(c, a.src)
            print("kind:", k)
            dest = a.dest or (r"%s\%s" % ((await writable_root(c))[1],
                              re.sub(r'[<>:"/\\|?*]', "_", a.src.split("\\")[-1].rsplit(".", 1)[0])))
            if k == "installed_dir":
                ok, msg = await install_copy_in(c, a.src, dest)
            elif k in ("gog_inno", "innosetup", "nsis"):
                ok, msg = await install_silent(c, a.src, dest, k)
                if ok:
                    await poll_until_stable(c, dest)
            else:
                ok, msg = False, "kind '%s' needs the GUI walk — see gui-install skill" % k
            print(("OK: " if ok else "NEEDS-ATTENTION: ") + msg + " -> " + dest)
    finally:
        await c.close()


if __name__ == "__main__":
    asyncio.run(_main())
