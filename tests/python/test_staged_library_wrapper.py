#!/usr/bin/env python3
"""A KILLED validator must not read as a BROKEN library.

WHY THIS EXISTS. On 2026-08-30 `tests/run_all.sh` reported

    == the staged library would deploy cleanly to a new box ==
      FAIL  the library would NOT deploy cleanly - see above

with **nothing above it**. The library was fine — three independent runs of
`validate-staged-library.py` either side of that one returned
`38 titles checked / DEPLOYABLE`. What actually happened is that the validator
walks every title's whole tree over CIFS, two dozen of them were running at
once from different fleet agents, and one was killed; `capture_output` lost its
buffers with it, so the wrapper printed its failure banner over an empty
report.

That is the repo's own rule broken by its own test harness: "not installed",
"could not be measured" and "it failed" are three different calls to action,
and only the last is a fault. A phantom bug report is worse than no report,
because it sends everyone chasing something that was never there.

Both directions are tested here, because a check that never fires is a lie.
"""
import importlib.util
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
WRAPPER = os.path.join(REPO, "tests", "test_staged_library.py")


def _load():
    spec = importlib.util.spec_from_file_location("sl_wrapper", WRAPPER)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["sl_wrapper"] = mod
    spec.loader.exec_module(mod)
    return mod


def _run(tmp_path, monkeypatch, capsys, body):
    """Point the wrapper at a fake validator that behaves however we like."""
    mod = _load()
    fake = tmp_path / "fake_validator.py"
    fake.write_text(body)
    lib = tmp_path / "lib"
    lib.mkdir()
    monkeypatch.setattr(mod, "VALIDATOR", str(fake))
    monkeypatch.setattr(mod, "LIB", str(lib))
    rc = mod.main()
    return rc, capsys.readouterr().out


def test_a_clean_validator_passes(tmp_path, monkeypatch, capsys):
    rc, out = _run(tmp_path, monkeypatch, capsys,
                   "print('DEPLOYABLE')\n")
    assert rc == 0
    assert "PASS" in out


def test_a_real_library_defect_still_says_the_library_is_broken(tmp_path,
                                                                monkeypatch,
                                                                capsys):
    """The original behaviour must survive: a validator that REPORTS problems
    and exits nonzero is a library failure and must say so."""
    rc, out = _run(tmp_path, monkeypatch, capsys,
                   "import sys\n"
                   "print('  [FAIL] Halo')\n"
                   "print('    fleetres: FLEETRES.EXE staged without .BAT')\n"
                   "sys.exit(1)\n")
    assert rc == 1
    assert "the library would NOT deploy cleanly" in out
    assert "[FAIL] Halo" in out, "the detail must still be shown"
    assert "was NOT checked" not in out, (
        "a real defect must not be excused as an unfinished measurement")


def test_a_killed_validator_is_reported_as_NOT_CHECKED(tmp_path, monkeypatch,
                                                       capsys):
    """The 2026-08-30 case: killed mid-walk, buffers lost, nonzero exit."""
    rc, out = _run(tmp_path, monkeypatch, capsys,
                   "import os, signal\n"
                   "os.kill(os.getpid(), signal.SIGKILL)\n")
    assert rc == 1, "it must still fail the suite - it just must not lie about why"
    assert "KILLED by signal 9" in out
    assert "was" in out and "NOT checked" in out
    assert "not a library failure" in out
    assert "the library would NOT deploy cleanly" not in out, (
        "a killed validator is being reported as a broken library again")


def test_a_silent_nonzero_exit_is_also_NOT_CHECKED(tmp_path, monkeypatch,
                                                   capsys):
    """Same shape without a signal: exits nonzero having printed nothing."""
    rc, out = _run(tmp_path, monkeypatch, capsys, "import sys; sys.exit(3)\n")
    assert rc == 1
    assert "without reporting anything" in out
    assert "not a library failure" in out
    assert "the library would NOT deploy cleanly" not in out


def test_an_unmounted_share_still_skips_loudly(tmp_path, monkeypatch, capsys):
    """The pre-existing guarantee: a dev host with no share must SKIP, and the
    skip must be loud, or the library rots unnoticed."""
    mod = _load()
    monkeypatch.setattr(mod, "LIB", str(tmp_path / "definitely-not-mounted"))
    rc = mod.main()
    out = capsys.readouterr().out
    assert rc == 0
    assert "SKIP" in out and "NOT checked" in out
