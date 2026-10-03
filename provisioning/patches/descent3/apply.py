#!/usr/bin/env python3
"""Descent 3 1.4 - make the in-flight view fill a 1080p (or any) display.

THE DEFECT (measured 2026-09-29 on .195 Win7 and .240 XP, see
.claude/evidence-1080p/_results/titles-verified.json "Descent3"):
the launcher's `-Width %FR_W% -Height %FR_H%` really does switch the display to
1920x1080, but the 3D view stays a 640x480 box in the middle of a black screen.

WHY: at SM_GAME Descent 3 sizes the game window from the PILOT file, not from
the display (DescentDevelopers/Descent3 game.cpp SetScreenMode):

    Current_pilot.get_hud_data(nullptr, nullptr, nullptr, &gw, &gh);
    if (force_res_change) { gw = Max_window_w; gh = Max_window_h; }
    InitGameScreen(gw, gh);          // clamps w,h to Max_window_w/h
    Current_pilot.set_hud_data(..., &Game_window_w, &Game_window_h);

and the staged `sdf.plt` (the only pilot in the tree) stored 640x480. Since
2026-10-02 the staged pilot is the USER'S (their WASD + F / mouse controls, set
up on .124 and staged fleet-wide at their request), and the game saved .124's
1280x960 into it - so at 1600x1200 on .123 the view was a 1280x960 box
(nonblack 160,120-1440,1080), and on a 1080p panel it would be one too.

THE FIX: raise the pilot's saved game window to 4096x4096. InitGameScreen clamps
it to the display, so ONE staged constant is right on every box - 1920x1080 on
the LCDs, the tube's own mode on a CRT. Verified twice on hardware. On .240
(2026-09-29, the April pilot, version 0x2A, `-pilot RVW -Width 1920 -Height
1080`): full screen at 1920x1080 (HW_VERIFIED_HISTORY). On .123 (2026-10-02,
the user's pilot, version 0x2B, EXACTLY the file this script builds, joined to
the dev host's server with `-pilot SDF -directip +connect` at 1600x1200): full
screen, against the unpatched pilot's centred 1280x960 box (HW_VERIFIED).

Both ways the game reaches the pilot keep the value: `-pilot SDF`
(menu.cpp MainMenu -> PltReadFile) and the PILOTS dialog (pilot.cpp PilotSelect
-> PltReadFile + VerifyPilotData, and pilot::verify() never touches the game
window). Only the -pilot path was exercised on hardware.

PILOT FILE LAYOUT (pilot_class.cpp pilot::read, version 0x2A/0x2B):
    int32   version                               0x2B (what 1.4 writes; 0x2A older)
    cstr    name                                  "sdf"
    cstr    ship_model                            "Pyro-GL"
    cstr    ship_logo, audio1, audio2             (read_custom_multiplayer_data)
    cstr    audio3, audio4                        (version >= 0x22)
    uint16  picture_id                            (end of read_custom_multiplayer_data)
    uint8   difficulty
    uint8   profanity filter                      (version >= 0x23)
    uint8   audiotaunts                           (version >= 0x28)
    uint8   hud_mode                              (read_hud_data)
    uint16  hud_stat
    uint16  hud_graphical_stat
    int32   game_window_w     <- patched
    int32   game_window_h     <- patched
    uint8   lrearview, rrearview                  (version >= 0x2B only)
The offsets therefore depend on the string lengths: this script PARSES the
header, and additionally asserts the parse lands on the offsets recorded for
the staged file (0x1F / 0x23), its version (0x2B) and its old values (1280/960).
0x2B only appends the two rearview bytes AFTER the window, so the window sits
where it does in 0x2A.

    apply.py --check                 read the staged pilot, assert, report
    apply.py --build [OUTDIR]        write patched copies + manifest.json
    apply.py --publish [OUTDIR] [--dry-run]
                                     FUTURE: back up the original, then put the
                                     patched file with scripts/fleet/sharewrite.py
                                     (one file at a time, stop on the first
                                     failure, verify each through /mnt,
                                     idempotent)

--check and --build work BEFORE and AFTER --publish. Once the share carries the
patched pilot, the original is reconstructed exactly by reverting the eight
bytes (its md5 is asserted), so the patch stays reproducible from the staged
file alone, backup or no backup.

There is deliberately NO --install-server: a dedicated server (the Host LAN
launcher, and the dev host's Wine descent3-server) never reads or writes a
pilot - PltReadFile builds an in-memory "SERVER" pilot and PltWriteFile returns
early when Dedicated_server (pilot.cpp) - and it renders nothing.

Nothing here writes game data into git: outputs go to
~/.retro-fleet/patch-out/descent3/ by default.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time

KEY = "descent3"
TITLE = "Descent3"
SHARE_ROOT = "/mnt/retro-share"               # read-only CIFS mount - reads/verification only
SHARE_LIB_REL = "Files/Games-Library"
LIBRARY = SHARE_ROOT + "/" + SHARE_LIB_REL
BACKUP_DIR = "originals-2026-10-02"
BACKUP_REL = SHARE_LIB_REL + "/_patches/" + TITLE + "/" + BACKUP_DIR
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/" + KEY)
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")

# InitGameScreen clamps to the display, so "as large as anything" is the right
# constant everywhere. 4096 is what was verified on hardware (rvw.plt, .240).
TARGET_W = 4096
TARGET_H = 4096

# Every pilot file the staged tree holds, found case-insensitively (*.plt):
# exactly one. A new pilot appearing in the tree is a finding, so --check fails
# on an unexpected .plt rather than silently leaving its own window size.
PILOTS = {
    # tree-relative path: expectations for the ORIGINAL staged file - since
    # 2026-10-02 the user's own pilot from .124 (their controls), kept in git as
    # voodoo-cleanroom/vcr-kmd/evidence/gametune_1001/descent3_controls/sdf.plt
    "sdf.plt": {
        "md5": "6b8115ec9cfd5db60edf5216ccac00fa",
        "size": 1772,
        "version": 0x2B,
        "name": "sdf",
        "w_off": 0x1F,
        "h_off": 0x23,
        "old_w": 1280,
        "old_h": 960,
        # the exact original bytes at 0x1F..0x26 (two little-endian int32s)
        "old_bytes": bytes.fromhex("00050000c0030000"),
        # what patch_pilot() makes of it; asserted on every build
        "patched_md5": "dd51fb8350f95511eb4cf006b19d72d1",
    },
}

# The staged pilot until 2026-10-02 (the library's April copy, 0x2A, 640x480),
# replaced at the user's request by their own; kept in git as
# .../descent3_controls/sdf.plt.library_20260416. Recorded so the history of
# what this patch was proven on stays readable.
PILOTS_HISTORY = {
    "sdf.plt@2026-04-16": {
        "md5": "b03f1b0f19e6d430f3d64b8c426f4da6", "size": 1770, "version": 0x2A,
        "old_w": 640, "old_h": 480, "patched_md5": "a42937534d704dce2374df0a0deea849",
    },
}

# The pilot proven on hardware: EXACTLY what this script builds from the
# user's pilot, run on .123 (CRT, 1600x1200) by joining the dev host's
# descent3-server with the staged Join launcher's own arguments - the join
# enters the level through SetScreenMode(SM_GAME), which reads the pilot's
# window. (The game's -timetest demo does NOT: it filled 1600x1200 with the
# unpatched pilot too, so it cannot show this defect.)
HW_VERIFIED = {
    "name": "sdf",
    "md5": "dd51fb8350f95511eb4cf006b19d72d1",
    "box": "192.168.1.123 (XP, Direct3D, Gateway VX1120 CRT)",
    "when": "2026-10-02 22:07-22:09 EDT",
    "evidence": "voodoo-cleanroom/vcr-kmd/evidence/gametune_1001/descent3_pilot_window/: "
                "join at 1600x1200, nonblack bbox 0,0-1600,1200 (unpatched user "
                "pilot: 160,120-1440,1080)",
}
# The first proof, on the April pilot (0x2A): the patched file with its name
# bytes changed to "rvw", run as `-pilot RVW` on .240.
HW_VERIFIED_HISTORY = [{
    "name": "rvw",
    "md5": "21a07b5456ca988d23a8d6c6b1a1ecc9",
    "box": "192.168.1.240 (XP, X800)",
    "when": "2026-09-29 01:12-01:13 EDT",
    "evidence": "240_d3_rvw_ingame_noaspect.png: nonblack bbox 0,0-1920,1080 "
                "(unpatched: 640,300-1280,780)",
}]

# Version gates from pilot_class.cpp
PFV_AUDIOTAUNT3N4 = 0x22
PFV_PROFANITY = 0x23
PFV_AUDIOTAUNTS = 0x28
PFV_REARVIEWINFO = 0x2B
PATCHABLE_VERSIONS = (0x2A, 0x2B)   # each verified on hardware (HW_VERIFIED*)


class PilotError(ValueError):
    pass


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def _cstr(data, off):
    end = data.find(b"\0", off)
    if end < 0:
        raise PilotError("unterminated string at 0x%X" % off)
    return data[off:end].decode("latin-1"), end + 1


def parse_pilot(data):
    """Walk a pilot file's header up to and including the game window size.

    Returns a dict with version, the strings, and the offsets of
    game_window_w / game_window_h. Raises PilotError on anything malformed.
    """
    if len(data) < 16:
        raise PilotError("file too short (%d B)" % len(data))
    (version,) = struct.unpack_from("<i", data, 0)
    if not (0x10 <= version <= 0x40):
        raise PilotError("implausible pilot version 0x%X" % version)
    off = 4
    name, off = _cstr(data, off)
    ship, off = _cstr(data, off)
    strings = []
    n = 5 if version >= PFV_AUDIOTAUNT3N4 else 3
    for _ in range(n):
        s, off = _cstr(data, off)
        strings.append(s)
    # picture_id, difficulty, [profanity], [audiotaunts], hud_mode, hud_stat,
    # hud_graphical_stat, game_window_w, game_window_h - all of it must be there,
    # so a truncated file is a PilotError rather than a struct/IndexError.
    need = (2 + 1 + (1 if version >= PFV_PROFANITY else 0)
            + (1 if version >= PFV_AUDIOTAUNTS else 0) + 1 + 4 + 8)
    if off + need > len(data):
        raise PilotError("file ends inside the header (%d B, need %d)" % (len(data), off + need))
    picture_id, = struct.unpack_from("<H", data, off); off += 2
    difficulty = data[off]; off += 1
    if version >= PFV_PROFANITY:
        off += 1
    if version >= PFV_AUDIOTAUNTS:
        off += 1
    hud_mode = data[off]; off += 1
    hud_stat, hud_gstat = struct.unpack_from("<HH", data, off); off += 4
    w_off = off
    h_off = off + 4
    if h_off + 4 > len(data):
        raise PilotError("file ends inside the HUD block")
    gw, gh = struct.unpack_from("<ii", data, w_off)
    return {
        "version": version, "name": name, "ship": ship,
        "custom_strings": strings, "picture_id": picture_id,
        "difficulty": difficulty, "hud_mode": hud_mode,
        "hud_stat": hud_stat, "hud_graphical_stat": hud_gstat,
        "w_off": w_off, "h_off": h_off, "game_window_w": gw, "game_window_h": gh,
    }


def _check_layout(p, expect):
    """The parsed header is the one `expect` describes: version, name, offsets."""
    if p["version"] not in PATCHABLE_VERSIONS or p["version"] != expect["version"]:
        raise PilotError("pilot version 0x%X, expected 0x%X" % (p["version"], expect["version"]))
    if p["name"].lower() != expect["name"].lower():
        raise PilotError("pilot name %r, expected %r" % (p["name"], expect["name"]))
    if (p["w_off"], p["h_off"]) != (expect["w_off"], expect["h_off"]):
        raise PilotError("parsed game window at 0x%X/0x%X, expected 0x%X/0x%X"
                         % (p["w_off"], p["h_off"], expect["w_off"], expect["h_off"]))


def patch_pilot(data, expect, target_w=TARGET_W, target_h=TARGET_H):
    """Return the patched bytes. Asserts everything `expect` records first.

    `expect` carries version, name, w_off, h_off, old_w, old_h, old_bytes and
    optionally patched_md5. Refuses (PilotError) on any mismatch - never
    patches a file it does not recognise. A file already carrying the target is
    handled by the callers (state_of / original_of), not re-patched here.
    """
    p = parse_pilot(data)
    _check_layout(p, expect)
    if (p["game_window_w"], p["game_window_h"]) != (expect["old_w"], expect["old_h"]):
        raise PilotError("game window is %dx%d, expected %dx%d"
                         % (p["game_window_w"], p["game_window_h"], expect["old_w"], expect["old_h"]))
    w_off = expect["w_off"]
    if data[w_off:w_off + 8] != expect["old_bytes"]:
        raise PilotError("bytes at 0x%X are %s, expected %s"
                         % (w_off, data[w_off:w_off + 8].hex(), expect["old_bytes"].hex()))
    out = bytearray(data)
    struct.pack_into("<ii", out, w_off, target_w, target_h)
    out = bytes(out)
    # post-condition: only those 8 bytes changed, and the file still parses
    diff = [i for i in range(len(data)) if data[i] != out[i]]
    if not diff or min(diff) < w_off or max(diff) >= w_off + 8 or len(out) != len(data):
        raise PilotError("patch touched bytes outside 0x%X..0x%X" % (w_off, w_off + 7))
    q = parse_pilot(out)
    if (q["game_window_w"], q["game_window_h"]) != (target_w, target_h):
        raise PilotError("patched file does not read back %dx%d" % (target_w, target_h))
    want = expect.get("patched_md5")
    if want and (target_w, target_h) == (TARGET_W, TARGET_H) and md5_bytes(out) != want:
        raise PilotError("patched md5 %s is not the recorded %s" % (md5_bytes(out), want))
    return out


def revert_pilot(data, expect):
    """Inverse of patch_pilot: the bytes a published (patched) pilot came from.

    Only the eight window bytes are put back; whether the result really is the
    original is for the caller to decide by md5 (see state_of)."""
    p = parse_pilot(data)
    _check_layout(p, expect)
    if (p["game_window_w"], p["game_window_h"]) != (TARGET_W, TARGET_H):
        raise PilotError("game window is %dx%d, not the patched %dx%d"
                         % (p["game_window_w"], p["game_window_h"], TARGET_W, TARGET_H))
    out = bytearray(data)
    w_off = expect["w_off"]
    out[w_off:w_off + 8] = expect["old_bytes"]
    return bytes(out)


def state_of(data, expect):
    """'original' | 'patched' | 'unknown: <why>' for a staged pilot's bytes.

    'patched' means EXACTLY this patch applied to EXACTLY the recorded original:
    the file reverts to the original md5. A pilot that merely carries 4096x4096
    (a different pilot, or one the game re-saved) is 'unknown'."""
    if md5_bytes(data) == expect["md5"] and len(data) == expect["size"]:
        return "original"
    try:
        orig = revert_pilot(data, expect)
    except PilotError as e:
        try:
            p = parse_pilot(data)
        except PilotError as e2:
            return "unknown: %s" % e2
        return "unknown: %s (version 0x%X, window %dx%d at 0x%X, md5 %s)" % (
            e, p["version"], p["game_window_w"], p["game_window_h"], p["w_off"], md5_bytes(data))
    if md5_bytes(orig) == expect["md5"] and len(orig) == expect["size"]:
        return "patched"
    return ("unknown: carries the %dx%d window but differs from the original elsewhere too "
            "(md5 %s; with the 8 bytes reverted %s, want %s)"
            % (TARGET_W, TARGET_H, md5_bytes(data), md5_bytes(orig), expect["md5"]))


def original_of(data, expect):
    """(original bytes, how) from a staged pilot in either known state.

    Raises PilotError for any other state - the build never guesses."""
    st = state_of(data, expect)
    if st == "original":
        return data, "the live staged file (still the original)"
    if st == "patched":
        return (revert_pilot(data, expect),
                "reconstructed from the live, already-patched staged file "
                "(8 bytes reverted, original md5 verified)")
    raise PilotError(st)


def find_pilots(tree):
    """Every *.plt under the staged tree, case-insensitively, tree-relative, '/'-separated."""
    found = []
    for root, _dirs, files in os.walk(tree):
        for f in files:
            if f.lower().endswith(".plt"):
                found.append(os.path.relpath(os.path.join(root, f), tree).replace(os.sep, "/"))
    return sorted(found)


def expected_for(rel):
    for k, v in PILOTS.items():
        if k.lower() == rel.lower():
            return k, v
    return None, None


def tree_dir(library):
    """The staged title directory, matched case-insensitively."""
    if not os.path.isdir(library):
        raise SystemExit("FAIL: library not mounted at %s - nothing was checked" % library)
    for d in sorted(os.listdir(library)):
        if d.lower() == TITLE.lower() and os.path.isdir(os.path.join(library, d)):
            return os.path.join(library, d)
    raise SystemExit("FAIL: %s not found (case-insensitive) in %s" % (TITLE, library))


def locate(tree, rel):
    """The on-share spelling of a pilot, matched case-insensitively (None if absent)."""
    for found in find_pilots(tree):
        if found.lower() == rel.lower():
            return found
    return None


def backup_path(library, rel):
    """Where --publish puts the original, as seen through the library mount."""
    return os.path.join(library, "_patches", TITLE, BACKUP_DIR, *rel.split("/"))


def cmd_check(library):
    tree = tree_dir(library)
    rc = 0
    pilots = find_pilots(tree)
    print("staged tree: %s" % tree)
    print("pilot files (*.plt, case-insensitive): %s" % (pilots or "NONE"))
    for rel in pilots:
        key, exp = expected_for(rel)
        if exp is None:
            print("FAIL %s: a pilot this patch does not know - it keeps its own game window"
                  " size; add it to PILOTS" % rel)
            rc = 1
            continue
        data = _read(os.path.join(tree, rel))
        st = state_of(data, exp)
        try:
            p = parse_pilot(data)
        except PilotError:
            p = None
        print("%s: %d B md5 %s -> %s" % (rel, len(data), md5_bytes(data), st))
        if p:
            print("  version 0x%X name %r ship %r hud_mode %d window %dx%d at 0x%X/0x%X"
                  % (p["version"], p["name"], p["ship"], p["hud_mode"],
                     p["game_window_w"], p["game_window_h"], p["w_off"], p["h_off"]))
        if st == "original":
            try:
                out = patch_pilot(data, exp)
            except PilotError as e:
                print("FAIL %s: %s" % (rel, e))
                rc = 1
                continue
            print("  would patch -> md5 %s (bytes 0x%X..0x%X: %s -> %s)"
                  % (md5_bytes(out), exp["w_off"], exp["w_off"] + 7,
                     exp["old_bytes"].hex(), out[exp["w_off"]:exp["w_off"] + 8].hex()))
        elif st == "patched":
            print("  already published: with bytes 0x%X..0x%X reverted it is the original"
                  " (md5 %s) exactly" % (exp["w_off"], exp["w_off"] + 7, exp["md5"]))
            b = backup_path(library, rel)
            if os.path.isfile(b):
                bm = md5_bytes(_read(b))
                if bm == exp["md5"]:
                    print("  backup present: %s (md5 %s)" % (b, bm))
                else:
                    print("FAIL %s: backup %s has md5 %s, not the original %s"
                          % (rel, b, bm, exp["md5"]))
                    rc = 1
            else:
                print("  WARN no backup at %s - the original is still reproducible exactly"
                      " (--build reverts the 8 bytes and checks the md5)" % b)
        else:
            print("FAIL %s: %s" % (rel, st))
            rc = 1
    for k in PILOTS:
        if not any(k.lower() == r.lower() for r in pilots):
            print("FAIL %s: expected in the staged tree and not found (case-insensitive)" % k)
            rc = 1
    print("check: %s" % ("OK" if rc == 0 else "FAILED"))
    return rc


def cmd_build(library, outdir):
    tree = tree_dir(library)
    manifest = {"key": KEY, "title": TITLE, "built": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "target_game_window": [TARGET_W, TARGET_H], "outputs": []}
    for rel, exp in PILOTS.items():
        actual = locate(tree, rel)
        if actual is None:
            raise SystemExit("FAIL %s: not in the staged tree (case-insensitive)" % rel)
        live = _read(os.path.join(tree, actual))
        try:
            orig, how = original_of(live, exp)
        except PilotError as e:
            raise SystemExit("FAIL %s: staged copy is %s - refusing to build" % (actual, e))
        out = patch_pilot(orig, exp)
        if md5_bytes(live) != exp["md5"] and out != live:
            raise SystemExit("FAIL %s: the published file is not what this patch makes" % actual)
        dst = os.path.join(outdir, TITLE, *actual.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(out)
        orig_copy = os.path.join(outdir, "originals", TITLE, *actual.split("/"))
        os.makedirs(os.path.dirname(orig_copy), exist_ok=True)
        with open(orig_copy, "wb") as f:
            f.write(orig)
        # verify what landed on disk, not what we meant to write
        if md5_bytes(_read(dst)) != md5_bytes(out) or md5_bytes(_read(orig_copy)) != exp["md5"]:
            raise SystemExit("FAIL: %s did not read back" % dst)
        manifest["outputs"].append({
            "rel": actual,
            "share_path": "%s/%s/%s" % (SHARE_LIB_REL, TITLE, actual),
            "local_path": dst,
            "md5": md5_bytes(out), "size": len(out),
            "original_md5": exp["md5"], "original_size": exp["size"],
            "original_local_copy": orig_copy,
            "backup_share_path": "%s/%s" % (BACKUP_REL, actual),
            "built_from": how,
            "change": "int32 game_window_w/h at 0x%X/0x%X: %d/%d -> %d/%d"
                      % (exp["w_off"], exp["h_off"], exp["old_w"], exp["old_h"], TARGET_W, TARGET_H),
        })
        print("built %s (%d B, md5 %s) from original md5 %s - %s"
              % (dst, len(out), md5_bytes(out), exp["md5"], how))
    mpath = os.path.join(outdir, "manifest.json")
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=2)
    print("manifest: %s" % mpath)
    return 0


def _share_md5(rel_share_path):
    """md5 of a share file read through the read-only /mnt mount (None if absent)."""
    p = os.path.join(SHARE_ROOT, *rel_share_path.split("/"))
    try:
        return md5_bytes(_read(p))
    except OSError:
        return None


def _put(local, dest, dry_run):
    if not os.path.isfile(SHAREWRITE):
        print("FAIL: %s not found - nothing can be published from this checkout" % SHAREWRITE,
              flush=True)
        return 2
    cmd = [sys.executable, SHAREWRITE, "put", local, dest] + (["--dry-run"] if dry_run else [])
    print("$ " + " ".join('"%s"' % c if " " in c else c for c in cmd), flush=True)
    return subprocess.run(cmd).returncode


def cmd_publish(outdir, dry_run):
    """FUTURE USE - not run in the build phase.

    Per output: prove the local files are the manifest's and this patch's,
    refuse a share file in any unknown state, back up the original (verified),
    then put the patched file (verified). One file at a time; the first failure
    stops everything. Re-running is a no-op."""
    mpath = os.path.join(outdir, "manifest.json")
    manifest = json.load(open(mpath))
    for o in manifest["outputs"]:
        rel = o.get("rel") or o["share_path"].split("%s/%s/" % (SHARE_LIB_REL, TITLE), 1)[-1]
        _key, exp = expected_for(rel)
        if exp is None:
            print("FAIL %s: not a pilot this patch knows - refusing" % rel)
            return 1
        # 0. the local files are what the manifest records, and what this patch makes
        try:
            local = _read(o["local_path"])
            orig = _read(o["original_local_copy"])
        except OSError as e:
            print("FAIL: %s - rebuild with --build first" % e)
            return 1
        if md5_bytes(orig) != o["original_md5"] or md5_bytes(orig) != exp["md5"]:
            print("FAIL %s: local original copy has md5 %s, not %s - rebuild"
                  % (rel, md5_bytes(orig), exp["md5"]))
            return 1
        if md5_bytes(local) != o["md5"]:
            print("FAIL %s: local patched file has md5 %s, the manifest says %s - rebuild"
                  % (rel, md5_bytes(local), o["md5"]))
            return 1
        try:
            if patch_pilot(orig, exp) != local:
                print("FAIL %s: the local patched file is not what this patch makes of the"
                      " original - rebuild" % rel)
                return 1
        except PilotError as e:
            print("FAIL %s: %s" % (rel, e))
            return 1
        live = _share_md5(o["share_path"])
        if live not in (o["md5"], o["original_md5"]):
            print("FAIL %s: share md5 is %s - neither the original %s nor the patched %s; refusing"
                  % (o["share_path"], live, o["original_md5"], o["md5"]))
            return 1
        # 1. the backup of the original, before the live file is touched
        bmd5 = _share_md5(o["backup_share_path"])
        if bmd5 == o["original_md5"]:
            print("backup already present: %s" % o["backup_share_path"])
        elif bmd5 is None:
            rc = _put(o["original_local_copy"], o["backup_share_path"], dry_run)
            if rc != 0:
                print("FAIL: backup put exited %d - stopping before touching the live file" % rc)
                return rc
            if not dry_run and _share_md5(o["backup_share_path"]) != o["original_md5"]:
                print("FAIL: %s does not read back as the original through %s - stopping"
                      % (o["backup_share_path"], SHARE_ROOT))
                return 3
        else:
            print("FAIL: %s exists with md5 %s, not the original - refusing to overwrite a backup"
                  % (o["backup_share_path"], bmd5))
            return 1
        # 2. the patched file
        if live == o["md5"]:
            print("skip %s: share already has the patched md5" % o["share_path"])
            continue
        rc = _put(o["local_path"], o["share_path"], dry_run)
        if rc != 0:
            print("FAIL: put exited %d - stopping" % rc)
            return rc
        if not dry_run:
            got = _share_md5(o["share_path"])
            if got != o["md5"]:
                print("FAIL: %s reads back as %s through %s, want %s"
                      % (o["share_path"], got, SHARE_ROOT, o["md5"]))
                return 3
            print("verified %s = %s through %s" % (o["share_path"], got, SHARE_ROOT))
    print("publish: %s" % ("dry run complete - nothing written" if dry_run else "OK"))
    print("NEXT (not done here): ONE bump of %s/_deploy_generation.txt - without it no"
          " provisioned box re-syncs the title. ('Play Descent 3.bat' passes -pilot SDF"
          " since 2026-10-02, stage-fleetres.py.)" % SHARE_LIB_REL)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--publish", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    ap.add_argument("--library", default=LIBRARY)
    ap.add_argument("--dry-run", action="store_true", help="with --publish: pass --dry-run to sharewrite")
    a = ap.parse_args(argv)
    if a.dry_run and not a.publish:
        ap.error("--dry-run only applies to --publish")
    if a.check:
        return cmd_check(a.library)
    if a.build:
        return cmd_build(a.library, a.build)
    return cmd_publish(a.publish, a.dry_run)


if __name__ == "__main__":
    sys.exit(main())
