#!/usr/bin/env python3
"""d3dprobe_paced.py - tools/d3dprobe_run.py, unchanged, except that d3dprobe
is given `--pace 5000`: at least 5 s between its display-mode switches (the
campaign rule for this run; vcr_pace.h's own floor is 3 s, and a short perf
run at a high refresh can finish inside 5 s). Same arguments as
d3dprobe_run.py. (d3dplan_1112/onbox, 2026-11-12.)"""
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(TOOLS))
import d3dprobe_run as dr  # noqa: E402

_orig = dr.lab_args


def lab_args(a, log):
    return _orig(a, log) + " --pace 5000"


dr.lab_args = lab_args

if __name__ == "__main__":
    dr.main()
