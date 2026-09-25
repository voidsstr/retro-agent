"""The chat daemon's deferred task queue ("run this when the box is next on").

Pins three failures:

  * every queued command got a 60 s timeout, so `EXECW 300 ...` always
    "failed" — and a command that timed out AFTER it was sent was re-run up
    to three times (it had usually run). Now the timeout is sized from the
    command, and a command that may have run is never re-run: the task goes
    to failed/ saying why.
  * a task interrupted by a network failure re-ran its already-completed
    commands on the next attempt. It now resumes at the command that never
    left.
  * the queue lived in tmpfs /tmp/retro-chat/tasks, so a host reboot erased
    work queued for a powered-off box. It lives under ~/.retro-fleet/ now; the
    old location is migrated once, and scripts/retro_enqueue.py writes (and
    lists) the new one.

Run: NSC_ASSISTANT_DIR=<nsc worktree> pytest tests/python/test_chat_daemon_tasks.py
"""

import asyncio
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from chat_fake_agent import DAEMON, FakeAgent, load_daemon, point_at

pytestmark = pytest.mark.skipif(not DAEMON.is_file(),
                                reason=f"chat daemon not checked out at {DAEMON}")

A = "192.168.1.221"
REPO = Path(__file__).resolve().parent.parent.parent


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 30))


def test_timeouts_are_sized_from_the_command(tmp_path):
    mod = load_daemon(tmp_path)
    assert mod.command_timeout("EXECW 300 setup.exe /S") == 330
    assert mod.command_timeout("EXECW 5000 x") == 930, "the agent clamps EXECW at 15 min"
    assert mod.command_timeout("EXEC dir C:\\") == 90
    assert mod.command_timeout("SYSINFO") == 60
    assert mod.command_timeout("EXECW 300 x", slow=True) > 330, "Win9x gets more"


def test_a_command_that_timed_out_after_sending_is_never_rerun(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent().start()
        agent.delay["EXECW"] = 1.0
        point_at(mod, {A: agent})
        mod.command_timeout = lambda cmd, slow=False: 0.3
        f = mod.enqueue_task(A, ["EXECW 600 install.exe /S", "SYSINFO"], "install")
        st = mod.HostState(A)
        await mod.drain_task_queue(st)
        await mod.drain_task_queue(st)           # a later pass must not re-run it
        execs = [c for c in agent.commands if c.startswith("EXECW")]
        assert len(execs) == 1, "a command that may have run was sent again"
        failed = f.parent / "failed" / f.name
        assert failed.exists() and not f.exists()
        task = json.loads(failed.read_text())
        assert "after it was sent" in task["reason"]
        assert "SYSINFO" not in agent.commands, "the rest of a failed task ran anyway"
        await st.close_all()
        await agent.stop()

    run(go())


def test_a_task_resumes_at_the_command_that_never_left(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent().start()
        point_at(mod, {A: agent})
        f = mod.enqueue_task(A, ["EXEC first", "EXEC second"], "two steps")
        st = mod.HostState(A)
        real = mod._send_cmd

        async def box_vanishes_before_step_two(state, cmd, timeout):
            if cmd == "EXEC second" and not getattr(box_vanishes_before_step_two, "done", False):
                box_vanishes_before_step_two.done = True
                raise mod.SendError("EXEC", False, ConnectionRefusedError())
            return await real(state, cmd, timeout)

        mod._send_cmd = box_vanishes_before_step_two
        await mod.drain_task_queue(st)
        pending = json.loads(f.read_text())
        assert pending["next"] == 1 and pending["attempts"] == 1
        await mod.drain_task_queue(st)
        done = f.parent / "done" / f.name
        assert done.exists()
        assert agent.commands.count("EXEC first") == 1, "a completed step ran twice"
        assert agent.commands.count("EXEC second") == 1
        await st.close_all()
        await agent.stop()

    run(go())


def test_the_queue_is_durable_and_the_old_tmp_queue_is_migrated(tmp_path):
    mod = load_daemon(tmp_path)
    assert "/tmp/" not in str(Path(mod.__file__).read_text().split("TASKS = Path(")[1][:120])
    old = mod.LEGACY_TASKS / A
    old.mkdir(parents=True)
    (old / "000001-1-legacy.json").write_text(json.dumps({"cmds": ["PING"]}))
    assert mod.migrate_legacy_tasks() == 1
    assert (mod.TASKS / A / "000001-1-legacy.json").exists()
    assert not list(old.glob("*.json"))
    assert mod.migrate_legacy_tasks() == 0


def test_retro_enqueue_writes_the_durable_queue_and_lists_the_legacy_one(tmp_path):
    env = dict(os.environ,
               RETRO_CHAT_TASKS=str(tmp_path / "durable"),
               RETRO_CHAT_ROOT=str(tmp_path / "tmpfs"))
    legacy = tmp_path / "tmpfs" / "tasks" / A
    legacy.mkdir(parents=True)
    (legacy / "000001-1-old.json").write_text(json.dumps({"cmds": ["PING"]}))
    script = REPO / "scripts" / "retro_enqueue.py"
    r = subprocess.run([sys.executable, str(script), A, "SYSINFO", "--label", "inv"],
                       env=env, capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    written = list((tmp_path / "durable" / A).glob("*.json"))
    assert len(written) == 1 and json.loads(written[0].read_text())["cmds"] == ["SYSINFO"]
    r = subprocess.run([sys.executable, str(script), "--list"],
                       env=env, capture_output=True, text=True, timeout=30)
    assert "1 pending" in r.stdout
    assert "old" in r.stdout and "tasks" in r.stdout


def test_retro_enqueue_default_is_not_tmpfs():
    src = (REPO / "scripts" / "retro_enqueue.py").read_text()
    default = src.split("TASKS = Path(os.environ.get(", 1)[1].split("LEGACY_TASKS", 1)[0]
    assert ".retro-fleet" in default and "/tmp" not in default
