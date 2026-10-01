"""Every tracked path must be one a Windows checkout can create.

The fleet is Windows and some of this repo is read from Windows boxes and the
NAS; a path with a character Windows forbids (< > : " | ? * or a control
character), or a component ending in a dot or a space, makes `git checkout`
fail there outright. Twelve evidence logs named after a bench title's variant
(`quake3:allours_1024x768_32_cfg5.log`) were committed before this test
existed (fixed 2026-10-01, with scripts/benchmarks/v56k_bench.run_stem).
"""
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BAD_CHARS = set('<>:"|?*')
RESERVED = {"con", "prn", "aux", "nul"} | {f"com{i}" for i in range(1, 10)} | {f"lpt{i}" for i in range(1, 10)}


def _tracked():
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z"],
                             capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip("not a git checkout - nothing to check")
    return [p for p in out.decode("utf-8", "replace").split("\0") if p]


def _problems(path):
    why = []
    for part in path.split("/"):
        if any(c in BAD_CHARS or ord(c) < 32 for c in part):
            why.append(f"{part!r}: a character Windows forbids")
        if part.endswith((".", " ")):
            why.append(f"{part!r}: ends in a dot or a space")
        if part.split(".")[0].lower() in RESERVED:
            why.append(f"{part!r}: a reserved DOS device name")
    return why


def test_the_checker_catches_what_it_should():
    assert _problems("evidence/quake3:allours_1024x768_32_cfg5.log")
    assert _problems("a/b./c") and _problems("a/nul.txt") and _problems('a/x"y')
    assert not _problems("evidence/quake3-allours_1024x768_32_cfg5.log")


def test_every_tracked_path_is_windows_safe():
    bad = {p: _problems(p) for p in _tracked()}
    bad = {p: w for p, w in bad.items() if w}
    assert not bad, "paths a Windows checkout cannot create:\n" + "\n".join(
        f"  {p}: {'; '.join(w)}" for p, w in sorted(bad.items()))
