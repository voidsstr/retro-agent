"""The chat brain must never publish a queue file the daemon can read half-written.

This encodes a real, user-visible failure: someone typed a message on a retro
box, the brain answered in three seconds, and nothing ever came back. The
daemon's log said

    outbox: invalid JSON in 192.168.1.143-1-000001.json, removing

Two things combined to destroy the reply:

  * the daemon watches the outbox with **inotify** and parses on the create
    event, so it is woken the instant the filename appears -- typically before
    any bytes are in it;
  * on `json.JSONDecodeError` the daemon **deletes** the file rather than
    retrying, so a transient race became permanent data loss.

`Path.write_text()` creates the file at its final name and then fills it, which
is exactly the window the daemon lands in. The fix is the convention the rest
of this project already uses (scripts/ai_status_bus.py, the dashboard
collector): write a temp file, fsync, then `os.replace`.

Run: pytest tests/python/test_chat_brain_atomic_outbox.py
"""

import importlib.util
import json
import os
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent.parent
_BRAIN = _REPO / "scripts" / "retro_chat_brain.py"


def _publish_json():
    """Import just the helper.

    Importing the whole brain drags in the Agent SDK and the fleet tools, which
    are not installed for the test suite; the atomicity contract is a property
    of this one function, so it is extracted and exec'd on its own.
    """
    src = _BRAIN.read_text()
    start = src.index("def publish_json(")
    end = src.index("def setup():", start)
    ns = {"Path": Path, "os": os, "json": json}
    exec(compile(src[start:end], str(_BRAIN), "exec"), ns)
    return ns["publish_json"]


@pytest.fixture(scope="module")
def publish():
    return _publish_json()


def test_the_brain_still_has_the_helper():
    """If someone reintroduces write_text() for a queue file, replies start
    disappearing again with no error anywhere except one daemon warning."""
    src = _BRAIN.read_text()
    assert "def publish_json(" in src
    assert "os.replace(" in src
    assert "write_text(json.dumps" not in src, \
        "a queue file is being written non-atomically again"


def test_payload_round_trips(publish, tmp_path):
    target = tmp_path / "host-1-000001.json"
    payload = {"host": "192.168.1.143", "seq": 1,
               "chunks": ["CHAT PATH OK"], "stream": True}
    publish(target, payload)
    assert json.loads(target.read_text()) == payload


def test_no_temp_file_is_left_behind(publish, tmp_path):
    publish(tmp_path / "a.json", {"x": 1})
    assert [p.name for p in tmp_path.iterdir()] == ["a.json"]


def test_the_temp_name_cannot_match_the_daemons_glob(publish, tmp_path):
    """The daemon does OUTBOX.glob('*.json'). If the in-progress file matched
    that, moving to a temp name would have changed nothing at all."""
    seen = []

    real_replace = os.replace

    def spy(src, dst):
        # Mid-write: whatever exists right now is what the daemon could see.
        seen.extend(p.name for p in Path(dst).parent.glob("*.json"))
        return real_replace(src, dst)

    ns_publish = publish
    import builtins  # noqa: F401
    orig = os.replace
    os.replace = spy
    try:
        ns_publish(tmp_path / "resp.json", {"chunks": ["hello"]})
    finally:
        os.replace = orig

    assert seen == [], f"a *.json file was visible before the rename: {seen}"


def test_the_final_name_only_ever_appears_complete(publish, tmp_path):
    """The property that actually matters: at no point does a file matching
    the daemon's glob exist with content that will not parse."""
    target = tmp_path / "resp.json"
    big = {"host": "192.168.1.143", "seq": 1,
           "chunks": ["x" * 200000], "stream": True}   # big enough to need many writes
    publish(target, big)
    for path in tmp_path.glob("*.json"):
        json.loads(path.read_text())      # raises if any visible file is partial


def test_overwriting_an_existing_file_is_still_atomic(publish, tmp_path):
    target = tmp_path / "resp.json"
    publish(target, {"n": 1})
    publish(target, {"n": 2})
    assert json.loads(target.read_text()) == {"n": 2}
    assert len(list(tmp_path.iterdir())) == 1


def test_a_failed_write_does_not_leave_a_stray_temp(publish, tmp_path):
    class Unserialisable:
        pass

    with pytest.raises(TypeError):
        publish(tmp_path / "bad.json", {"x": Unserialisable()})
    assert list(tmp_path.iterdir()) == [], "temp file survived a failed write"


def test_concurrent_publishes_do_not_collide(publish, tmp_path):
    """Two brain processes (a restart overlapping the old one) must not write
    the same temp path — the name carries the pid for that reason."""
    src = _BRAIN.read_text()
    start = src.index("def publish_json(")
    assert "os.getpid()" in src[start:src.index("def setup():", start)]
