"""The host-duty check must classify honestly, and must be able to say NO.

It exists because the dev host's duties are spread across three managers
(`systemctl --user`, system units, docker) and after a reboot "is everything
back?" took a dozen ad-hoc commands and three different status vocabularies.

The risk in a checker like this is that it can only ever say OK -- every
serious defect in this project reported success and was believed. So most of
what is asserted here is the NEGATIVE path: that a missing unit, a stopped
container, and a running-but-not-enabled service each produce a distinct,
visible fault, and that a service which has simply never been installed on this
host does NOT.

Nothing here touches the network or the fleet; the systemd/docker calls are
stubbed, so it runs in milliseconds on any machine.
"""
import importlib.util
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "host-duties.py")

spec = importlib.util.spec_from_file_location("host_duties", SRC)
hd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hd)


def _stub_systemctl(mapping):
    """Fake `systemctl is-active` / `is-enabled` from {(unit, verb): reply}."""
    def fake(user, verb, unit):
        return mapping.get((unit, verb), "not-found")
    return fake


def test_a_healthy_unit_is_ok(monkeypatch=None):
    hd._systemctl = _stub_systemctl({("u", "is-active"): "active",
                                     ("u", "is-enabled"): "enabled"})
    assert hd.check_unit("u", True, "u", "why")["state"] == "ok"


def test_a_dead_unit_is_a_fault():
    """The whole point: a unit that died must not read like one that is fine."""
    hd._systemctl = _stub_systemctl({("u", "is-active"): "failed",
                                     ("u", "is-enabled"): "enabled"})
    assert hd.check_unit("u", True, "u", "why")["state"] == "down"


def test_running_but_not_enabled_is_a_fault_even_though_it_is_active():
    """A hand-started service is invisible until the reboot that loses it.

    This is the failure this tool exists to surface: `is-active` says active,
    everything looks healthy, and the duty silently does not come back.
    """
    hd._systemctl = _stub_systemctl({("u", "is-active"): "active",
                                     ("u", "is-enabled"): "disabled"})
    r = hd.check_unit("u", True, "u", "why")
    assert r["state"] == "wont-survive-reboot", r
    assert r["state"] != "ok", "an unenabled service must never render as OK"


def test_never_installed_here_is_NOT_reported_as_an_outage():
    """"Not installed" and "crashed" must never render the same.

    claude-csbot and mohaa-server are named in the docs but have never run on
    this host. Rendering their absence as a fault would put a permanent red
    light on the board and train everyone to ignore it.
    """
    hd._systemctl = _stub_systemctl({})       # nothing exists
    for unit in ("claude-csbot", "mohaa-server"):
        assert unit in hd.NEVER_INSTALLED_HERE
        r = hd.check_unit(unit, True, unit, "why")
        assert r["state"] == "absent", (unit, r)
        assert r["state"] != "missing", "%s has never been installed here" % unit


def test_rtcw_is_no_longer_excused_as_never_installed():
    """The other half of the same rule, and the one that goes stale.

    `rtcw-server` was on NEVER_INSTALLED_HERE while it genuinely did not
    exist. It was installed on 2026-09-01 (:27963, ioRTCW 1.51c, proven
    two-box), and leaving it on the excuse list would render a REAL outage as
    the reassuring word "absent" -- forever, and silently. An excuse for a
    thing that now exists is worse than no excuse at all.
    """
    assert "rtcw-server" not in hd.NEVER_INSTALLED_HERE
    hd._systemctl = _stub_systemctl({})       # nothing exists
    r = hd.check_unit("rtcw-server", True, "RTCW", "why")
    assert r["state"] == "missing", r
    assert r["state"] != "absent", (
        "rtcw-server is installed on this host; its absence is a fault")


def test_a_unit_that_SHOULD_exist_but_does_not_is_a_fault():
    """The counterpart: an unlisted unit going missing is a real fault."""
    hd._systemctl = _stub_systemctl({})
    r = hd.check_unit("retro-chat-brain", True, "b", "why")
    assert r["state"] == "missing", r


def test_static_and_indirect_count_as_enabled():
    """Systemd has more than two enablement words; treating the others as
    'disabled' would cry wolf, and a checker that cries wolf gets ignored."""
    for word in ("enabled", "enabled-runtime", "static", "generated", "indirect", "alias"):
        hd._systemctl = _stub_systemctl({("u", "is-active"): "active",
                                         ("u", "is-enabled"): word})
        assert hd.check_unit("u", True, "u", "why")["state"] == "ok", word


def test_tribes2_is_checked_through_docker_not_systemd():
    """Tribes 2 needs a 2001 userland so it is a container.

    Anything enumerating the game servers through systemd alone gets
    `not-found` and silently drops a running server off the board -- which is
    exactly the bug CLAUDE.md records.
    """
    rows = hd._game_units()
    managers = {u: mgr for u, _lbl, mgr in rows}
    assert managers.get("tribes2-server") == "docker", (
        "Tribes 2 must not be probed as a systemd unit -- it would read "
        "not-found while the server is up")
    unit_names = [u for u, _ in hd.USER_UNITS]
    assert "tribes2-server" not in unit_names


def test_a_container_with_no_restart_policy_wont_survive_a_reboot():
    """restart=no is the docker equivalent of a disabled unit."""
    hd._run = lambda cmd, timeout=15: (0, "true no", "")
    hd.shutil.which = lambda _x: "/usr/bin/docker"
    assert hd.check_docker("c", "why")["state"] == "wont-survive-reboot"


def test_a_stopped_container_is_down():
    hd._run = lambda cmd, timeout=15: (0, "false unless-stopped", "")
    hd.shutil.which = lambda _x: "/usr/bin/docker"
    assert hd.check_docker("c", "why")["state"] == "down"


def test_a_running_container_with_a_restart_policy_is_ok():
    hd._run = lambda cmd, timeout=15: (0, "true unless-stopped", "")
    hd.shutil.which = lambda _x: "/usr/bin/docker"
    assert hd.check_docker("c", "why")["state"] == "ok"


def test_linger_off_is_a_fault_because_it_silently_kills_every_user_unit():
    """Without linger NO --user unit starts until somebody logs in.

    It is the single point of failure for seven duties and completely silent:
    every unit still reads `enabled` while the box comes up with the fleet
    bridge dead. So `Linger=no` must be a fault, not a note.
    """
    hd._run = lambda cmd, timeout=15: (0, "Linger=no", "")
    assert hd.check_linger()["state"] == "down"

    hd._run = lambda cmd, timeout=15: (0, "Linger=yes", "")
    assert hd.check_linger()["state"] == "ok"


def test_cannot_ask_is_distinct_from_it_died():
    """Three states, never two: unknown is not a fault, down is."""
    hd._run = lambda cmd, timeout=15: (1, "", "no session")
    assert hd.check_linger()["state"] == "unknown"


def test_systemctl_user_as_root_is_pointed_at_uid_1000():
    """`systemctl --user` under root queries ROOT's manager, which holds none
    of these -- every service then reads "not found", indistinguishable from
    every service having died. The helper must set XDG_RUNTIME_DIR."""
    src = open(SRC).read()
    assert "XDG_RUNTIME_DIR" in src and "/run/user/1000" in src, (
        "the --user calls must be pointed at uid 1000's bus, or a root-run "
        "check reports the entire fleet as missing")


def test_every_duty_in_CLAUDE_md_s_host_services_table_is_covered():
    """The docs list seven host services; none may be quietly dropped."""
    covered = ({u for u, _ in hd.USER_UNITS} | {u for u, _ in hd.SYSTEM_UNITS}
               | {u for u, _lbl, _mgr in hd._game_units()})
    for required in ("retro-chat-daemon", "retro-chat-brain", "retro-gameindex",
                     "retro-gameservers-watch", "retro-dosgames-http",
                     "retro-pxe", "retro-dashboard-collector"):
        assert required in covered, "%s is a documented host duty" % required


def test_game_servers_come_from_gameservers_py_not_a_second_hand_kept_list():
    """One source of truth for what this host runs.

    host-duties.py used to keep its own copy of the game-server list, and it
    rotted exactly as a duplicate does: by 2026-09-01 it named NINE servers
    while the host ran twenty-four -- so the tool you run after a reboot to
    ask "is the host back?" answered ALL HOST DUTIES UP with fifteen servers
    it had never heard of, any of which could have been dead.

    So: every row gameservers.py declares must appear, and the module must not
    reintroduce a hardcoded list beside it.
    """
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "gameservers",
        os.path.join(REPO, "scripts", "game-servers", "gameservers.py"))
    gs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gs)

    got = {u for u, _lbl, _mgr in hd._game_units()}
    for row in gs.SERVERS:
        assert row["unit"] in got, (
            "%s is declared in gameservers.py and missing from host-duties"
            % row["unit"])
    for row in gs.PROXIES:
        assert row["unit"] in got, row["unit"]

    # The servers added 2026-09-01 are the concrete case this guards.
    for unit in ("rtcw-server", "doom3-server", "deusex-server",
                 "unrealgold-server", "ssam-tfe-server", "ssam-tse-server",
                 "shogo-server", "descent3-server", "farcry-server"):
        assert unit in got, unit

    src = open(SRC).read()
    assert "GAME_UNITS = [" not in src, (
        "the hand-kept game-server list is back; it is the thing that rotted")
