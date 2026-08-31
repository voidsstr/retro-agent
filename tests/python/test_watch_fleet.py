"""The fleet watcher must report EDGES, and must not mistake a socket for life.

Written while the operator was power-cycling machines by hand and asked to have
newly-arrived boxes picked up automatically. Three properties matter, and each
one is a mistake this project has already paid for:

1. **A successful TCP connect is not liveness.** When .243's agent died, port
   9897 kept accepting sockets while answering nothing, and 139 stayed open
   because the OS itself was healthy. A watcher that treats "connected" as "up"
   would have reported that box as fine. It must require a real PONG, and it
   must report `ACCEPTS-BUT-DEAD` as its own state -- "the agent crashed" and
   "the machine is off" call for different actions from a person.

2. **Only transitions are events.** This fleet is deliberately powered off most
   of the time ("an empty sweep is NOT an outage"), so a steady state of DOWN
   is the normal case. Emitting the level rather than the edge would make the
   common case the noisy one and train everyone to ignore it.

3. **Slow is not absent.** .171 and the Pentium 1 both answer slowly enough
   that short timeouts have dropped them from sweeps entirely. A false DOWN
   here would be reported as a power-cycle that never happened.

Pure source inspection -- no network, no fleet.
"""
import ast
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "watch-fleet.py")


def _src():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_liveness_requires_a_protocol_reply_not_a_connect():
    src = _src()
    assert "PING" in src and "PONG" in src, (
        "the watcher no longer speaks the protocol. A TCP connect succeeds "
        "against a DEAD agent -- 9897 accepted sockets from .243 for hours "
        "while answering nothing.")
    assert "ACCEPTS-BUT-DEAD" in src, (
        "the watcher no longer distinguishes a crashed agent from a powered-off "
        "machine; those need different responses from a human")


def test_it_emits_edges_not_levels():
    """A steady DOWN must be silent -- most of this fleet is off most of the time."""
    src = _src()
    assert "state.get(ip)" in src or "was == now" in src, (
        "the watcher must compare against previous state and emit only on "
        "change; reporting the level would make the normal case the noisy one")
    assert "first" in src, (
        "the opening sweep must be a baseline, not a burst of UP events for "
        "every box that was already running")


def test_the_timeout_is_generous_enough_for_the_slow_boxes():
    """A false DOWN reads as a power-cycle that never happened."""
    tree = ast.parse(_src())
    timeout = None
    for node in ast.walk(tree):
        if (isinstance(node, ast.Assign) and node.targets
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id == "TIMEOUT"):
            timeout = ast.literal_eval(node.value)
    assert timeout is not None, "TIMEOUT is gone"
    assert timeout >= 8.0, (
        "TIMEOUT is %.1fs. .171 and the Pentium 1 answer slowly; CLAUDE.md "
        "records that 1.5-2s timeouts dropped them from sweeps entirely, and "
        "a false DOWN here would be reported as a power-cycle." % timeout)


def test_connections_are_closed_gracefully():
    """An abrupt disconnect crashes Win98 Winsock, taking the box down.

    A watcher that killed the machines it polls would be worse than none, and
    it polls every 20 seconds forever.
    """
    src = _src()
    assert "await c.close()" in src, (
        "the watcher no longer closes connections gracefully; an abrupt RST "
        "crashes Win98 Winsock and this runs against .243 every cycle")


def test_every_host_is_probed_concurrently():
    """Serial probes would make one dead box delay every other box's event."""
    assert "gather(" in _src(), (
        "hosts must be probed concurrently, or the sweep costs the sum of all "
        "timeouts and a newly-booted box waits behind the dead ones")


def test_a_refused_connection_is_retried_before_being_called_down():
    """Refusal means something is LISTENING and turning us away.

    The Win9x agents serve one client at a time and refuse everyone else while
    busy. Measured on .243: a raw TCP connect to 9898 succeeded while five
    consecutive protocol probes were refused, because a sibling agent held the
    single slot. Reporting that as DOWN manufactures a fake power-cycle.
    """
    src = _src()
    assert "ConnectionRefusedError" in src, (
        "the watcher no longer distinguishes a refused connection from an "
        "unreachable host; on a single-threaded Win9x agent those mean "
        "opposite things")
    assert "BUSY-OR-REFUSING" in src, (
        "a persistently refusing box must get its own state -- something is "
        "listening, which is not the same fault as silence")


def test_only_refusal_is_retried_inline():
    """Retrying a timeout would delay every real event.

    Most of this fleet is powered off most of the time, so an unreachable host
    is the COMMON case; spending backoff on it on every sweep would make a real
    power-on late for the sake of a rare condition.
    """
    src = _src()
    i = src.index("async def probe(")
    body = src[i:src.index("\nasync def main", i)]
    assert 'return ip, "DOWN"' in body, "probe no longer reports DOWN at all"
    assert body.index("ConnectionRefusedError") < body.index('return ip, "DOWN"'), (
        "the refusal branch must come before the general handler, or refusal "
        "falls through to DOWN and the retry never happens")


def test_a_single_missed_probe_is_not_a_departure():
    """Debounce. A busy box answers slowly; one miss is not evidence.

    Measured 2026-08-31: .143, .240 and .243 were all reported down within
    seconds while three agents drove them hard. Probing with patience showed
    .143 had never left, .240 was genuinely off, and .243 was refusing. Only
    one was a real departure.

    This is not cosmetic: a false DOWN tells the agent working that box that
    its machine vanished, and the standing instruction is then to stop and
    record cells `untested` -- so a flapping watcher manufactures exactly the
    gaps the sweep exists to close.
    """
    src = _src()
    assert "MISSES_BEFORE_DOWN" in src, (
        "the watcher no longer debounces; a single slow answer on a busy box "
        "is reported as a power-cycle")
    tree = ast.parse(src)
    n = None
    for node in ast.walk(tree):
        if (isinstance(node, ast.Assign) and node.targets
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id == "MISSES_BEFORE_DOWN"):
            n = ast.literal_eval(node.value)
    assert n and n >= 2, "MISSES_BEFORE_DOWN must be at least 2, got %r" % n


def test_a_box_COMING_BACK_is_reported_immediately():
    """Debounce must be one-sided.

    A late UP wastes an agent's time waiting; a late DOWN costs nothing. So
    only the transition into a non-answering state is debounced.
    """
    src = _src()
    i = src.index("MISSES_BEFORE_DOWN = ")
    body = src[i:]
    assert 'now in ("DOWN", "BUSY-OR-REFUSING")' in body, (
        "the debounce must apply ONLY to the non-answering states -- if it "
        "also delays UP, every returning box waits an extra sweep for no "
        "benefit")
