"""DRIVERS STATUS / DRIVERS PLAN (agent 1.88.0) - report-only.

Step one of "keep every driver on the box correct, except 3dfx" (user
directive 2026-09-27): before anything installs, every device gets ONE state
from agent/shared/drvplan.h (tested natively by tests/native/test_drvplan.c),
with the 3dfx rule (agent/shared/drvsafe.h) judged first. Pinned here: the
routing, that both OS paths use the same state function and the 3dfx rule,
and that nothing in either path writes anything.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def src(name):
    return re.sub(r"/\*.*?\*/", "", (ROOT / "agent" / "src" / name).read_text(), flags=re.S)


def body(code, name):
    start = code.index(name)
    depth, i = 0, code.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[start:i + 1]
        i += 1


def test_drivers_status_and_plan_route_by_os_and_keep_the_class_filter():
    v = src("video.c")
    start = v.index("void handle_drivers(")
    route = v[start:v.index("SetupDiGetClassDevsA", start)]   # (the body holds a '{' char literal)
    assert '_strnicmp(args, "STATUS", 6) == 0' in route and '_strnicmp(args, "PLAN", 4) == 0' in route
    assert "drv9x_status(sock, plan)" in route and "gs_drivers_status(sock, plan)" in route
    assert "GetVersion() & 0x80000000UL" in route
    handlers = (ROOT / "agent" / "src" / "handlers.c").read_text()
    assert re.search(r'\{\s*"DRIVERS",\s*1,\s*NULL,\s*handle_drivers,\s*0\s*\}', handlers), "still read-only (reconfigures_host 0)"
    assert "$(SRCDIR)/drv9x.c" in (ROOT / "agent" / "Makefile").read_text()


def test_both_paths_judge_3dfx_first_and_use_one_state_function():
    nt = body(src("gamesync.c"), "void gs_drivers_status(")
    assert nt.index("r->excl = gs_device_3dfx(") < nt.index("drvplan_state(")
    w9 = body(src("drv9x.c"), "void drv9x_status(")
    assert "drvsafe_ids_3dfx(hwids, compat)" in w9 and "drvsafe_driver_is_3dfx(" in w9
    assert w9.index("r->excl =") < w9.index("drvplan_state(")
    assert '"Config Manager\\\\Enum"' in w9 and "HKEY_DYN_DATA" in w9


def test_the_report_writes_nothing():
    for code in (body(src("gamesync.c"), "void gs_drivers_status("), body(src("drv9x.c"), "void drv9x_status(")):
        for w in ("RegSetValue", "RegDelete", "RegCreateKey", "UpdateDriverForPlugAndPlayDevices", "gs_force_install",
                  "CM_Reenumerate", "CreateFileA", "DeleteFile"):
            assert w not in code, w
