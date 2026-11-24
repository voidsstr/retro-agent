"""stage-title-subdir.py refuses everything except ADDING a new folder to an
existing title (it was written to stage The Specialists' ts\\ into
CounterStrike16, 2026-11-23, without letting a box sync a half-upload)."""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "stage_title_subdir", os.path.join(HERE, "..", "..", "scripts", "fleet", "stage-title-subdir.py"))
sts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sts)


def test_plan(tmp_path):
    lib = tmp_path
    (lib / "CounterStrike16" / "cstrike").mkdir(parents=True)
    assert sts.plan(str(lib), "CounterStrike16", "ts") is None
    # an existing folder, in any case, is refused - this only adds
    assert "already exists" in sts.plan(str(lib), "CounterStrike16", "CSTRIKE")
    # a title that does not exist is a NEW title - stage-new-title's job
    assert "not a library title" in sts.plan(str(lib), "NoSuchTitle", "ts")
    # the share's spelling must be used exactly (paths are built from it)
    assert "spelled" in sts.plan(str(lib), "counterstrike16", "ts")
    assert sts.plan(str(lib), "_incoming", "ts") is not None
    assert sts.plan(str(lib), "CounterStrike16", "../x") is not None
    assert sts.plan(str(lib), "CounterStrike16", "a/b") is not None
