"""The share log mirror is WIRED to the change counters (agent 1.85.0).

tests/native/test_sharelog.c proves the decision in agent/shared/sharelog.h.
This proves the agent actually feeds it the right facts:

* log.c bumps the write counter where bytes reach the FILE (disk_out) and the
  rotation counter where agent.log is rolled (rotate_files) - not on the
  console echo, and not on the buffered append, which has not reached disk;
* sharelog_thread flushes, THEN reads the counters, THEN copies - so a line
  written during the copy makes the next pass copy again rather than being
  marked as mirrored;
* the only log lines it writes are on an outcome change. The old thread logged
  every copy, which is what made every next copy necessary.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"


def read(name):
    return (SRC / name).read_text(errors="replace")


def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
                  src, re.M | re.S)
    assert m, f"{name} not found"
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError(name)


def test_log_counts_writes_that_reach_the_file_and_rotations():
    log = code_only(read("log.c"))
    d = body(log, "disk_out")
    w = d.index("WriteFile(g_log_h")
    assert d.index("InterlockedIncrement((LONG *)&g_write_seq)") > w, \
        "count a write only once WriteFile succeeded"
    assert "g_write_seq" not in body(log, "raw_out"), \
        "a buffered append has not reached the file yet"
    r = body(log, "rotate_files")
    assert r.index("MoveFileA(path, bak)") < r.index("InterlockedIncrement((LONG *)&g_rotate_seq)")
    assert "log_write_seq(void)" in log and "log_rotation_seq(void)" in log


def test_the_thread_flushes_then_reads_the_counters_then_copies():
    b = code_only(body(read("main.c"), "sharelog_thread"))
    order = [b.index(x) for x in (
        "log_flush()", "log_write_seq()", "log_rotation_seq()",
        "sharelog_plan(", "CopyFileA(log_path(), dest", "sharelog_record(")]
    assert order == sorted(order), "flush -> counters -> plan -> copy -> record"
    assert "if (plan & SHARELOG_COPY_LOG)" in b, "the log copy must be conditional"
    assert "if (plan & SHARELOG_COPY_BAK)" in b, "the .1 copy must be conditional"


def test_the_thread_logs_only_on_an_outcome_change():
    b = code_only(body(read("main.c"), "sharelog_thread"))
    assert "mirrored to" not in b, "the per-copy log line is back"
    rec = b.index("if (sharelog_record(")
    for m in re.finditer(r"log_msg\(", b):
        assert m.start() > rec, "every mirror log line sits under the outcome test"
    assert "sharelog_next_ms(&st, SHARELOG_PERIOD_MS)" in b, "failures back off"
