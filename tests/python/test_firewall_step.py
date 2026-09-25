"""The firewall step runs off the startup path and reports what really happened.

tests/native/test_fwplan.c proves the decision (agent/shared/fwplan.h). This
proves main.c uses it the way the fix intends:

* ensure_firewall_exception() is no longer called on the startup path before
  listen() - it used to hold the agent off the network for up to 10 s of netsh
  on every boot - but from a background helper spawned after the listener;
* netsh runs only as fw_plan() says (after a registry lookup);
* the log carries netsh's EXIT CODE and the post-condition (is the exception
  present afterwards), never a bare "added" for a process that merely started.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "agent" / "src" / "main.c"


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


def test_the_firewall_step_is_off_the_startup_path():
    run = code_only(body(MAIN.read_text(errors="replace"), "agent_run"))
    assert "ensure_firewall_exception()" not in run, (
        "agent_run calls the firewall step directly again - it ran two netsh "
        "processes before listen() on every boot")
    spawn = run.index("spawn_helper(firewall_thread")
    assert run.index("listen(listen_sock, 4)") < spawn, \
        "the firewall helper must start after the listener is up"
    ft = code_only(body(MAIN.read_text(errors="replace"), "firewall_thread"))
    assert ft.index("thread_background()") < ft.index("ensure_firewall_exception()")


def test_netsh_runs_only_as_the_plan_says():
    b = code_only(body(MAIN.read_text(errors="replace"), "ensure_firewall_exception"))
    assert b.index("firewall_already_allows(") < b.index("fw_plan(")
    assert "if (plan & FWP_NETSH_FIREWALL)" in b
    assert "if (plan & FWP_NETSH_ADV)" in b
    assert b.index("if (!plan)") < b.index("run_netsh(")


def test_the_log_reports_the_exit_code_and_the_post_condition():
    src = code_only(MAIN.read_text(errors="replace"))
    rn = body(src, "run_netsh")
    assert "GetExitCodeProcess" in rn and "WAIT_TIMEOUT" in rn
    assert "Firewall exception added" not in src, \
        "the unconditional 'added' line is back"
    b = body(src, "ensure_firewall_exception")
    last = b.rindex("firewall_already_allows(")
    assert last > b.rindex("run_netsh("), "re-check the registry AFTER netsh"
