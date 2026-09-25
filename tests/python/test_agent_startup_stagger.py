"""The agent's startup helpers do not all wake at the same moment (1.85.0).

Until 1.85.0 retrowall (RETROWALL_DELAY_SEC), GAMESYNC (GS_FIRST_DELAY_MS) and
GAMEINDEX (GI_FIRST_DELAY_MS) all started work 20 s after the agent - the theme
broadcasts, the library/desktop work and a full disk walk landing together on a
box that was still finishing its own logon. On a Pentium 166 that is the
"the agent taxes the CPU at startup" report. They are now staggered 20 / 40 /
120 s, and none of them shares a second with the other timed helpers.
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "agent" / "src"


def seconds(path, name, unit):
    m = re.search(r"#define\s+" + name + r"\s+(\d+)", (SRC / path).read_text(errors="replace"))
    assert m, f"{name} not found in {path}"
    v = int(m.group(1))
    return v if unit == "s" else v / 1000.0


def hwpub_first_retry():
    m = re.search(r"schedule\[HWPUB_MAX_ATTEMPTS\]\s*=\s*\{\s*(\d+)",
                  (SRC.parent / "shared" / "hwpub.h").read_text())
    assert m
    return int(m.group(1))


def test_the_three_heavy_helpers_are_staggered_in_order():
    wall = seconds("retrowall.c", "RETROWALL_DELAY_SEC", "s")
    sync = seconds("gamesync.c", "GS_FIRST_DELAY_MS", "ms")
    index = seconds("gameindex.c", "GI_FIRST_DELAY_MS", "ms")
    # the old values, for the record: all three were 20 s
    assert (wall, sync, index) == (20, 40, 120)
    assert wall < sync < index
    # even without a cached index the walk waits for the other two
    assert seconds("gameindex.c", "GI_FIRST_DELAY_NOCACHE_MS", "ms") > sync


def test_no_two_timed_helpers_start_in_the_same_second():
    starts = {
        "autoupdate": seconds("autoupdate.c", "UPDATE_DELAY_SEC", "s"),
        "retrowall": seconds("retrowall.c", "RETROWALL_DELAY_SEC", "s"),
        "gamesync": seconds("gamesync.c", "GS_FIRST_DELAY_MS", "ms"),
        "dosstage": seconds("dosstage.c", "DOSSTAGE_DELAY_SEC", "s"),
        "hwpublish": hwpub_first_retry(),
        "gameindex": seconds("gameindex.c", "GI_FIRST_DELAY_MS", "ms"),
        "gameindex (no cache)": seconds("gameindex.c", "GI_FIRST_DELAY_NOCACHE_MS", "ms"),
        "sharelog": seconds("main.c", "SHARELOG_FIRST_MS", "ms"),
    }
    by_time = {}
    for name, t in starts.items():
        by_time.setdefault(t, []).append(name)
    clashes = {t: n for t, n in by_time.items() if len(n) > 1}
    assert not clashes, f"helpers waking together: {clashes}"
