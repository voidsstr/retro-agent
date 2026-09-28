"""Library changes must actually reach boxes - GAMESYNC will not do it by itself.

WHY THIS EXISTS
---------------
The agent's GAMESYNC startup thread gates on a bare marker check:

    if (gs_file_exists(GS_MARKER)) { ... "already provisioned" ...; return 0; }

No library comparison, no title count. Once `gamesync.done` exists the thread
idles forever, so a title staged today never reaches a box provisioned
yesterday. Rainbow Six landed on .123 only because a human typed GAMESYNC
RESET. That is a silent failure: the library grows, validate-staged-library
says DEPLOYABLE, every box looks healthy, and the new games are simply absent.

autodeploy.py closes that gap. On 2026-09-28 it still missed three things,
all measured that day:

1. It keyed ONLY on the library's title set, so a fix INSIDE a title (new
   icons for Serious Sam, Warcraft I/II, Carmageddon 1, Descent) never reached
   a box that was offline when it was pushed. -> the deploy GENERATION file.
2. It carried a hardcoded IP list naming .246 (ADMIN-PC had moved to .195 via
   DHCP) and lacking .110 (a new Dell). -> discovery, state keyed by hostname,
   IP-keyed records migrated.
3. It did not skip a modern Windows host (WHITEBEAST .249, Win11). -> the
   host_policy skip, before any GAMESYNC command.

Reading gamesync.c while fixing those found three more, each of which made a
failed run look like success or took a box out of rotation silently: the old
code waited for a state "error" the agent never reports, treated "failed" as
busy forever, and accepted the PREVIOUS run's "done" in the window after START.

The network is mocked throughout. Nothing here talks to a box.
"""
import ast
import asyncio
import json
import os
import re
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "scripts" / "fleet" / "autodeploy.py"
AGENT = REPO / "agent" / "src" / "gamesync.c"
sys.path.insert(0, str(REPO / "scripts" / "fleet"))
import autodeploy as ad  # noqa: E402

TITLES = ["DeusEx", "Halo", "Quake1", "SeriousSamFirstEncounter", "WarcraftII"]
GOOD = {"state": "done", "failed_files": 0, "titles_total": len(TITLES),
        "titles_done": len(TITLES) - 1, "titles_gated": 1, "titles_skipped": 0,
        "files_written": 4, "elapsed_s": 3}
MANAGED = {"host_policy": {"modern": False, "managed": True}}


def _src():
    return SRC.read_text(encoding="utf-8")


# --- a fake fleet ----------------------------------------------------------------

class Fleet:
    """Stands in for sweep / HWPROFILE / GAMESYNC STATUS / the sync itself."""

    def __init__(self, **boxes):
        # ip -> {"greeting", "profile", "status", "result", "raise"}
        self.boxes = {ip.replace("_", "."): b for ip, b in boxes.items()}
        self.calls = []

    def box(self, ip, greeting, profile=None, **kw):
        self.boxes[ip] = dict(greeting=greeting,
                              profile=MANAGED if profile is None else profile, **kw)
        return self

    async def sweep(self, ips, expected=(), timeout=8.0):
        self.calls.append(("sweep", tuple(ips)))
        return {ip: b["greeting"] for ip, b in self.boxes.items() if ip in ips}

    async def identify(self, ip, timeout=8.0):
        self.calls.append(("HWPROFILE", ip))
        return dict(self.boxes[ip]["profile"])

    async def gamesync_status(self, ip, timeout=8.0):
        self.calls.append(("STATUS", ip))
        st = self.boxes[ip].get("status", {"state": "done"})
        if st is None:
            return True, None, "GAMESYNC STATUS unparsable"
        return True, dict(st), ""

    async def sync(self, ip, timeout=20.0, wait=True):
        self.calls.append(("SYNC", ip))
        b = self.boxes[ip]
        if b.get("raise"):
            raise b["raise"]
        run = {"t0": time.monotonic(), "saw": False, "library": ad.LIBRARY_UNC}
        if not wait:
            return None, run
        return dict(b.get("result", GOOD)), run

    def asked(self, what, ip=None):
        return [c for c in self.calls if c[0] == what and (ip is None or c[1] == ip)]


@pytest.fixture
def fleet(monkeypatch):
    f = Fleet()
    for name in ("sweep", "identify", "gamesync_status", "sync"):
        monkeypatch.setattr(ad, name, getattr(f, name))
    return f


def run_pass(tmp_path, st, generation="0", titles=TITLES, argv=(), w=None, roster=None):
    a = ad.parse_args(["--state", str(tmp_path / "state.json"), *argv])
    w = w or ad.Watch(roster=roster or {})
    asyncio.run(ad.pass_once(a, w, titles, generation, st))
    return w


def saved(tmp_path):
    p = tmp_path / "state.json"
    return json.loads(p.read_text()) if p.exists() else None


# --- premise ---------------------------------------------------------------------

def test_it_parses_and_has_no_escape_warnings():
    """`\\G` in the docstring is an invalid escape python will one day reject."""
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("error", SyntaxWarning)
        ast.parse(_src())


def test_the_marker_gate_it_compensates_for_is_still_a_bare_check():
    """If the agent ever learns to compare libraries, this tool is redundant."""
    c = AGENT.read_text(encoding="utf-8", errors="replace")
    i = c.find("already provisioned")
    assert i > 0, "the startup log line moved - re-check the gate"
    assert "gs_file_exists(GS_MARKER)" in c[max(0, i - 300):i], (
        "the startup gate changed shape - if it now compares the library, "
        "autodeploy.py's whole premise needs revisiting")


def test_the_status_states_it_knows_are_the_ones_the_agent_reports():
    """The old code waited for "error", a state gamesync.c has never had."""
    c = AGENT.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'const char \*names\[\] = \{([^}]*)\}', c)
    assert m, "handle_gamesync's state-name table moved"
    agent_states = set(re.findall(r'"(\w+)"', m.group(1)))
    assert agent_states == set(ad.AT_REST) | set(ad.BUSY), agent_states
    assert "error" not in agent_states      # what the old poll loop waited for


# --- (a) the deploy generation ---------------------------------------------------

def _lib(tmp_path, gen_name=None, gen_text=None):
    lib = tmp_path / "lib"
    for t in TITLES:
        (lib / t).mkdir(parents=True)
    (lib / "_patches").mkdir()
    if gen_name:
        (lib / gen_name).write_text(gen_text)
    return str(lib)


def test_absent_generation_file_is_generation_zero(tmp_path):
    gen, how = ad.read_generation(_lib(tmp_path))
    assert gen == "0" and "no _deploy_generation.txt" in how


def test_generation_is_the_first_real_line_and_the_name_is_case_insensitive(tmp_path):
    lib = _lib(tmp_path, "_DEPLOY_GENERATION.TXT",
               "# bump this line to push a staged fix fleet-wide\n\n"
               "  2026-09-28a icons: SS, WC1/2, Carma1, Descent  \nignored\n")
    assert ad.read_generation(lib)[0] == "2026-09-28a icons: SS, WC1/2, Carma1, Descent"


def test_a_generation_file_that_declares_nothing_says_so(tmp_path):
    """A gvfs write that dropped its bytes looks exactly like this."""
    gen, how = ad.read_generation(_lib(tmp_path, "_deploy_generation.txt", "# x\n\n"))
    assert gen == "0" and "WARNING" in how


def test_an_unreadable_library_is_not_generation_zero(tmp_path):
    """A flaky mount must not read as a generation change."""
    gen, how = ad.read_generation(str(tmp_path / "not-mounted"))
    assert gen is None and "unreadable" in how


def test_generation_file_is_not_a_title(tmp_path):
    lib = _lib(tmp_path, "_deploy_generation.txt", "1\n")
    assert ad.library_titles(lib) == TITLES


def test_sync_reason_keys_on_the_title_set_and_the_generation():
    legacy = {"titles": TITLES, "at": "2026-09-01 18:13:33"}     # no generation
    # OLD RULE: the title set was the only question, so a fix INSIDE a title
    # (same names, generation bumped) read as "already offered"
    assert legacy.get("titles") == TITLES
    # NEW: absent file ("0") against a record from before generations: nothing
    assert ad.sync_reason(legacy, TITLES, "0") is None
    # ...and a bump is a reason on its own
    assert "deploy generation '0' -> '7'" in ad.sync_reason(legacy, TITLES, "7")
    assert ad.sync_reason(dict(legacy, generation="7"), TITLES, "7") is None
    # the title-set rule is kept
    assert "1 new title(s) - Rune" in ad.sync_reason(legacy, TITLES + ["Rune"], "0")
    assert ad.sync_reason(None, TITLES, "0") == "never synced by autodeploy"


def test_a_generation_bump_resyncs_a_box_exactly_once(tmp_path, fleet):
    fleet.box("192.168.1.123", "OK NSC-B20C188E96D Win5.1",
              dict(MANAGED, hostname="NSC-B20C188E96D"))
    st = {"NSC-B20C188E96D": {"titles": TITLES, "generation": "0",
                              "ip": "192.168.1.123"}}
    w = run_pass(tmp_path, st, generation="0")
    assert not fleet.asked("SYNC"), "no bump, nothing to do"
    w = run_pass(tmp_path, st, generation="icons-0928", w=w)
    assert fleet.asked("SYNC") == [("SYNC", "192.168.1.123")]
    assert saved(tmp_path)["NSC-B20C188E96D"]["generation"] == "icons-0928"
    run_pass(tmp_path, st, generation="icons-0928", w=w)
    assert len(fleet.asked("SYNC")) == 1, "a box takes a generation ONCE"


def test_the_generation_recorded_is_the_one_read_before_the_run(tmp_path, fleet):
    """A bump that lands mid-run must re-sync the box, not be credited to it."""
    fleet.box("192.168.1.123", "OK BOX Win5.1", dict(MANAGED, hostname="BOX"))
    st = {}
    w = run_pass(tmp_path, st, generation="1")
    assert st["BOX"]["generation"] == "1"
    run_pass(tmp_path, st, generation="2", w=w)
    assert len(fleet.asked("SYNC")) == 2


# --- (b) discovery and hostname keys ---------------------------------------------

def test_there_is_no_hardcoded_fleet_address_list():
    """BOXES named .246 after ADMIN-PC had moved to .195, and lacked .110."""
    tree = ast.parse(_src())
    ips = [n.value for n in ast.walk(tree)
           if isinstance(n, ast.Constant) and isinstance(n.value, str)
           and re.fullmatch(r"\d+\.\d+\.\d+\.\d+", n.value)]
    assert ips == [], "fleet addresses are discovered, never listed: %s" % ips
    a = ad.parse_args([])
    assert a.boxes == [] and not a.no_discover


def test_the_first_pass_sweeps_the_whole_subnet_and_later_ones_the_live_set(tmp_path, fleet):
    fleet.box("192.168.1.110", "OK NSC-5C5396FAF9D Win5.1",
              dict(MANAGED, hostname="NSC-5C5396FAF9D"))
    st = {}
    w = run_pass(tmp_path, st, argv=["--discover-every", "3"])
    first = fleet.asked("sweep")[0][1]
    assert len(first) == 254 and first[0] == "192.168.1.1" and first[-1] == "192.168.1.254"
    assert fleet.asked("SYNC") == [("SYNC", "192.168.1.110")], "a new box is found and synced"
    run_pass(tmp_path, st, argv=["--discover-every", "3"], w=w)
    assert fleet.asked("sweep")[1][1] == ("192.168.1.110",)
    run_pass(tmp_path, st, argv=["--discover-every", "3"], w=w)
    run_pass(tmp_path, st, argv=["--discover-every", "3"], w=w)
    assert len(fleet.asked("sweep")[3][1]) == 254, "every Nth pass sweeps again"


def test_extra_boxes_are_always_probed_and_no_discover_means_only_them(tmp_path, fleet):
    run_pass(tmp_path, {}, argv=["--boxes", "10.0.0.5", "--no-discover"])
    assert fleet.asked("sweep")[0][1] == ("10.0.0.5",)


class FakeConn:
    """RetroConnection stand-in for the handshake tests."""
    script = {}
    made = []

    def __init__(self, ip, port):
        self.ip, self.port, self.closed = ip, port, []
        FakeConn.made.append(self)

    async def connect(self, secret, timeout=10.0):
        how = FakeConn.script[self.ip]
        if isinstance(how, BaseException):
            raise how
        return how["greeting"]

    async def command_text(self, cmd, timeout=60.0):
        return FakeConn.script[self.ip]["replies"][cmd]

    async def close(self, graceful=True, drain_timeout=2.0):
        self.closed.append(graceful)


@pytest.fixture
def conn(monkeypatch):
    FakeConn.script, FakeConn.made = {}, []
    monkeypatch.setattr(ad, "RetroConnection", FakeConn)
    monkeypatch.setattr(ad, "REFUSAL_BACKOFF", 0)
    return FakeConn


def test_alive_means_a_handshake_and_a_pong_not_a_connect(conn):
    conn.script = {
        "a": {"greeting": "OK A Win5.1", "replies": {"PING": "PONG"}},
        "b": {"greeting": "OK B Win4.10", "replies": {"PING": "ERR busy"}},
        "c": ConnectionResetError("accepts, then nothing"),
    }
    live = asyncio.run(ad.sweep(["a", "b", "c"]))
    assert live == {"a": "OK A Win5.1"}
    assert all(c.closed == [True] for c in conn.made if c.ip in ("a", "b")), \
        "every probe leaves with a FIN - an RST crashes Win98's Winsock"


def test_a_handshake_that_dies_halfway_still_closes_gracefully(conn):
    conn.script = {"x": asyncio.TimeoutError()}
    assert asyncio.run(ad.handshake("x")) is None
    assert conn.made[0].closed == [True]


def test_a_refusal_is_retried_only_where_an_agent_is_expected(conn):
    """Win9x agents refuse while busy; phones and printers refuse forever."""
    conn.script = {"box": ConnectionRefusedError(), "printer": ConnectionRefusedError()}
    asyncio.run(ad.sweep(["box", "printer"], expected={"box"}))
    tries = {ip: sum(1 for c in conn.made if c.ip == ip) for ip in ("box", "printer")}
    assert tries == {"box": 1 + ad.REFUSAL_RETRIES, "printer": 1}


def test_a_moved_box_keeps_its_record(tmp_path, fleet):
    """DHCP moves boxes; an address says where a box is, a name says which."""
    fleet.box("192.168.1.150", "OK DELL Win5.1", dict(MANAGED, hostname="DELL"))
    st = {"DELL": {"titles": TITLES, "generation": "0", "ip": "192.168.1.145"}}
    run_pass(tmp_path, st)
    assert not fleet.asked("SYNC")
    assert st["DELL"]["ip"] == "192.168.1.150"
    # the IP-keyed version would have made a fresh record for the new address
    # and orphaned the old one
    assert "192.168.1.150" not in st and "192.168.1.145" not in st


def test_hwprofile_names_the_box_and_the_greeting_is_the_fallback(tmp_path, fleet):
    fleet.box("192.168.1.30", "OK GREETNAME Win5.1", dict(MANAGED, hostname="profname"))
    fleet.box("192.168.1.31", "OK OLDAGENT Win5.0", {})          # no HWPROFILE
    st = {}
    run_pass(tmp_path, st)
    assert set(st) == {"PROFNAME", "OLDAGENT"}


def test_a_legacy_record_is_migrated_at_its_own_address(tmp_path, fleet):
    fleet.box("192.168.1.243", "OK N5R5L9 Win4.10", dict(MANAGED, hostname="N5R5L9"))
    st = {"192.168.1.243": {"titles": TITLES, "at": "2026-09-25 02:47:11"}}
    run_pass(tmp_path, st, roster={"192.168.1.243": "N5R5L9"})
    assert not fleet.asked("SYNC"), "migration alone must not re-sync the box"
    assert "192.168.1.243" not in st
    assert st["N5R5L9"]["migrated_from"] == "192.168.1.243"
    assert saved(tmp_path)["N5R5L9"]["titles"] == TITLES


def test_a_legacy_record_follows_the_roster_to_a_box_dhcp_moved(tmp_path, fleet):
    """Measured 2026-09-28: ADMIN-PC's record was under .246, the box at .195."""
    fleet.box("192.168.1.195", "OK ADMIN-PC Win6.1", dict(MANAGED, hostname="ADMIN-PC"))
    st = {"192.168.1.246": {"titles": TITLES}}
    run_pass(tmp_path, st, roster={"192.168.1.246": "ADMIN-PC"})
    assert st["ADMIN-PC"]["migrated_from"] == "192.168.1.246"
    assert st["ADMIN-PC"]["ip"] == "192.168.1.195"
    assert not fleet.asked("SYNC")


def test_the_roster_vetoes_adopting_another_boxes_record(tmp_path, fleet, capsys):
    """.124 was re-imaged: NSC-C543575F526 now answers where NSC-CABE14B7486 was.

    Inheriting the old install's "already synced" would make the new one miss
    a deploy silently; refusing costs one incremental sync.
    """
    fleet.box("192.168.1.124", "OK NSC-C543575F526 Win5.1",
              dict(MANAGED, hostname="NSC-C543575F526"))
    st = {"192.168.1.124": {"titles": TITLES}}
    run_pass(tmp_path, st, roster={"192.168.1.124": "NSC-CABE14B7486"})
    assert "192.168.1.124" in st, "the other box's record stays where it is"
    assert fleet.asked("SYNC") == [("SYNC", "192.168.1.124")]
    assert st["NSC-C543575F526"]["ip"] == "192.168.1.124"
    assert "belongs to NSC-CABE14B7486" in capsys.readouterr().out


def test_two_boxes_with_one_name_do_not_share_a_record(tmp_path, fleet, capsys):
    for ip in ("192.168.1.60", "192.168.1.61"):
        fleet.box(ip, "OK ADMIN-PC Win6.1", dict(MANAGED, hostname="ADMIN-PC"))
    st = {}
    run_pass(tmp_path, st)
    assert set(st) == {"ADMIN-PC@192.168.1.60", "ADMIN-PC@192.168.1.61"}
    assert len(fleet.asked("SYNC")) == 2
    assert "WARNING: ADMIN-PC answers at" in capsys.readouterr().out


def test_a_non_windows_agent_is_left_alone(tmp_path, fleet):
    fleet.box("192.168.1.70", "OK pi Linux_6.1 linux", {})
    run_pass(tmp_path, {})
    assert not fleet.asked("HWPROFILE") and not fleet.asked("SYNC")


# --- (c) a modern Windows box is not managed -------------------------------------

def test_a_modern_box_gets_no_gamesync_command_at_all(tmp_path, fleet, capsys):
    fleet.box("192.168.1.249", "OK WHITEBEAST Win6.2",
              {"hostname": "WHITEBEAST", "agent_version": "1.85.1",
               "host_policy": {"modern": True, "managed": False}})
    st = {}
    w = run_pass(tmp_path, st, generation="5")
    assert fleet.asked("HWPROFILE", "192.168.1.249")
    assert not fleet.asked("STATUS") and not fleet.asked("SYNC")
    assert st == {} and saved(tmp_path) is None
    assert "WHITEBEAST (192.168.1.249) skipped" in capsys.readouterr().out
    run_pass(tmp_path, st, generation="5", w=w)
    assert "skipped" not in capsys.readouterr().out, "said once, not every 90 s"
    assert len(fleet.asked("HWPROFILE")) == 1, "the answer is cached"


@pytest.mark.parametrize("profile", [{"hostname": "X", "agent_version": "1.81.1"}, {}])
def test_an_old_agent_greeting_win6_2_counts_as_modern(tmp_path, fleet, profile):
    """6.2 is the GetVersionEx shim value on every Windows 8.1/10/11 box."""
    fleet.box("192.168.1.139", "OK X Win6.2", profile)
    run_pass(tmp_path, {})
    assert not fleet.asked("SYNC") and not fleet.asked("STATUS")


def test_the_manage_modern_windows_override_is_respected(tmp_path, fleet):
    fleet.box("192.168.1.139", "OK X Win6.2",
              {"hostname": "X", "host_policy": {"modern": True, "managed": True}})
    run_pass(tmp_path, {})
    assert fleet.asked("SYNC") == [("SYNC", "192.168.1.139")]


def test_the_policy_agrees_with_the_favourites_push():
    """One rule, two host tools: scripts/gameindex/sync.py decides the same way."""
    sys.path.insert(0, str(REPO / "scripts" / "gameindex"))
    import sync as gi  # noqa: E402

    class Conn:
        def __init__(self, prof):
            self.prof = prof

        async def command_text(self, cmd, timeout=30):
            if self.prof is None:
                raise RuntimeError("no answer")
            return json.dumps(self.prof)

    profiles = [{"host_policy": {"modern": True, "managed": False}},
                {"host_policy": {"modern": True, "managed": True}},
                {"host_policy": {"modern": False, "managed": True}},
                {"hostname": "OLD"}, None]
    for greeting in ("OK WB Win6.2", "OK WB Win10.0", "OK XP Win5.1",
                     "OK W7 Win6.1", "OK P1 Win4.10"):
        for prof in profiles:
            hp = (prof or {}).get("host_policy") or {}
            winver = ad.parse_greeting(greeting)[1]
            if winver < (6, 2) and hp.get("managed") is False:
                continue    # impossible: the agent is managed below 6.2
            theirs = asyncio.run(gi.unmanaged_modern_host(Conn(prof), greeting))
            ours = ad.unmanaged_reason(winver, prof or {})
            assert bool(theirs) == bool(ours), (greeting, prof, theirs, ours)


# --- (d) the safety that was already there, and what it was missing --------------

@pytest.mark.parametrize("state", ["sizing", "copying"])
def test_a_box_mid_sync_is_left_alone(tmp_path, fleet, state):
    fleet.box("192.168.1.9", "OK B Win5.1", dict(MANAGED, hostname="B"),
              status={"state": state})
    run_pass(tmp_path, {})
    assert not fleet.asked("SYNC")


@pytest.mark.parametrize("state", ["failed", "skipped"])
def test_failed_and_skipped_are_finished_states_not_busy_ones(tmp_path, fleet, state):
    """The old rule `gs not in (None, "idle", "done")` called these busy.

    The agent keeps its last state until it restarts, so one aborted run took
    the box out of rotation while the log promised a retry next pass.
    """
    assert state not in (None, "idle", "done")                   # old: busy forever
    fleet.box("192.168.1.9", "OK B Win5.1", dict(MANAGED, hostname="B"),
              status={"state": state})
    run_pass(tmp_path, {})
    assert fleet.asked("SYNC") == [("SYNC", "192.168.1.9")]


def test_an_unreadable_status_is_not_permission_to_reset(tmp_path, fleet):
    fleet.box("192.168.1.9", "OK B Win5.1", dict(MANAGED, hostname="B"), status=None)
    run_pass(tmp_path, {})
    assert fleet.asked("STATUS") and not fleet.asked("SYNC")


def test_a_run_is_only_accepted_once_it_is_ours():
    """START returns before the worker sets "sizing" (it sweeps the desktop and
    stages wallpapers first), so STATUS still shows the PREVIOUS run's "done".
    """
    t0 = 1000.0
    stale = {"state": "done", "failed_files": 0, "elapsed_s": 86400}
    ours = {"state": "done", "failed_files": 0, "elapsed_s": 25}
    # OLD: the first finished-looking state after 20 s was taken as the result
    assert stale["state"] in ("done", "error")
    assert ad.assess(stale, {"t0": t0, "saw": False}, now=t0 + 30) == "starting"
    assert ad.assess(ours, {"t0": t0, "saw": False}, now=t0 + 30) == "finished"
    run = {"t0": t0, "saw": False}
    assert ad.assess({"state": "sizing"}, run, now=t0 + 40) == "running" and run["saw"]
    assert ad.assess(stale, run, now=t0 + 900) == "finished", "watched it run"
    assert ad.assess({"state": "idle"}, run, now=t0 + 900) == "lost", \
        "an agent restarted mid-run"
    assert ad.assess(stale, {"t0": t0, "saw": False},
                     now=t0 + ad.START_GRACE + 1) == "lost"


def test_the_post_condition_is_more_than_state_done():
    n = len(TITLES)
    assert ad.judge(GOOD, n, ad.LIBRARY_UNC)[0]
    assert not ad.judge(dict(GOOD, failed_files=2), n)[0]
    assert not ad.judge(dict(GOOD, state="failed"), n)[0]
    assert not ad.judge(None, n)[0]
    # the .243 truncated enumeration: 25 == 25 by the box's own count
    trunc = dict(GOOD, titles_total=25, titles_done=24, titles_gated=1)
    ok, why = ad.judge(trunc, 46, "\\\\192.168.1.122\\FILES\\Files\\Games-Library\\")
    assert not ok and "25 title(s)" in why and "46" in why
    # a box pointed at another library cannot be held to this one's count
    assert ad.judge(trunc, 46, r"\\otherbox\lib")[0]
    assert not ad.judge(dict(GOOD, titles_gated=0), n)[0], "counters must add up"


def test_a_failed_run_is_not_recorded_and_backs_off(tmp_path, fleet, capsys):
    fleet.box("192.168.1.9", "OK B Win5.1", dict(MANAGED, hostname="B"),
              result=dict(GOOD, failed_files=1, failed_file="sin.exe"))
    st = {}
    w = run_pass(tmp_path, st)
    assert "B" not in st and saved(tmp_path) is None
    assert "NOT recorded (failed_files=1 (sin.exe))" in capsys.readouterr().out
    run_pass(tmp_path, st, w=w)
    assert len(fleet.asked("SYNC")) == 1, "not every 90 s"
    n, _, when = w.fails["B"]
    w.fails["B"] = (n, 0, when)                   # the back-off has elapsed
    fleet.boxes["192.168.1.9"]["result"] = GOOD
    run_pass(tmp_path, st, w=w)
    assert len(fleet.asked("SYNC")) == 2 and st["B"]["titles"] == TITLES
    assert "B" not in w.fails


def test_the_back_off_doubles_and_is_capped():
    w = ad.Watch(roster={})
    waits = []
    for _ in range(8):
        before = time.monotonic()
        ad._failed(w, "B", "x")
        waits.append(round(w.fails["B"][1] - before))
    assert waits[0] == ad.FAIL_BACKOFF and waits[1] == 2 * ad.FAIL_BACKOFF
    assert max(waits) == ad.FAIL_BACKOFF_MAX


def test_one_boxs_exception_does_not_end_the_pass(tmp_path, fleet):
    fleet.box("192.168.1.8", "OK A Win5.1", dict(MANAGED, hostname="A"),
              **{"raise": ConnectionResetError("gone mid-START")})
    fleet.box("192.168.1.9", "OK B Win5.1", dict(MANAGED, hostname="B"))
    st = {}
    run_pass(tmp_path, st)
    assert "A" not in st and "B" in st


def test_dry_run_sends_nothing_that_writes_and_saves_nothing(tmp_path, fleet, capsys):
    fleet.box("192.168.1.243", "OK N5R5L9 Win4.10", dict(MANAGED, hostname="N5R5L9"))
    fleet.box("192.168.1.110", "OK DELL2 Win5.1", dict(MANAGED, hostname="DELL2"))
    st = {"192.168.1.243": {"titles": TITLES}}
    run_pass(tmp_path, st, generation="9", argv=["--dry-run"],
             roster={"192.168.1.243": "N5R5L9"})
    assert not fleet.asked("SYNC")
    assert not (tmp_path / "state.json").exists(), "not even the migration"
    out = capsys.readouterr().out
    assert "dry run - not saved" in out and out.count("dry run - not syncing") == 2


def test_no_wait_judges_its_run_on_a_later_pass(tmp_path, fleet):
    """--no-wait used to never record, so a box re-synced every pass forever."""
    fleet.box("192.168.1.9", "OK B Win5.1", dict(MANAGED, hostname="B"))
    st = {}
    w = run_pass(tmp_path, st, argv=["--no-wait"])
    assert "B" in w.pending and "B" not in st
    fleet.boxes["192.168.1.9"]["status"] = {"state": "copying"}
    run_pass(tmp_path, st, argv=["--no-wait"], w=w)
    assert len(fleet.asked("SYNC")) == 1 and "B" not in st
    fleet.boxes["192.168.1.9"]["status"] = GOOD
    run_pass(tmp_path, st, argv=["--no-wait"], w=w)
    assert st["B"]["titles"] == TITLES and not w.pending
    run_pass(tmp_path, st, argv=["--no-wait"], w=w)
    assert len(fleet.asked("SYNC")) == 1


def test_sync_waits_past_the_previous_runs_status(monkeypatch):
    seq = [(True, {"state": "done", "failed_files": 0, "elapsed_s": 99999}, ""),
           (False, None, "refused"),
           (True, {"state": "copying"}, ""),
           (True, dict(GOOD, elapsed_s=99999), "")]

    async def status(ip, timeout=8.0):
        return seq.pop(0)

    async def start(ip, timeout=20.0):
        return {"t0": time.monotonic(), "saw": False, "library": ""}

    monkeypatch.setattr(ad, "gamesync_status", status)
    monkeypatch.setattr(ad, "start_run", start)
    monkeypatch.setattr(ad, "POLL", 0)
    res, run = asyncio.run(ad.sync("x"))
    assert res["titles_total"] == len(TITLES) and not seq and run["saw"]


def test_a_start_that_is_not_ours_is_not_waited_on(conn):
    conn.script = {"x": {"greeting": "OK X Win5.1", "replies": {
        "GAMESYNC LIBRARY": '{"state":"copying"}',          # pre-LIBRARY agent
        "GAMESYNC RESET": "OK marker cleared",
        "GAMESYNC START": "OK already running"}}}
    res, run = asyncio.run(ad.sync("x"))
    assert res is None and "already running" in run["error"]
    assert conn.made[0].closed == [True]


def test_it_never_reboots_anything():
    """Check the CODE, not the prose - the docstring says "NEVER reboots"."""
    tree = ast.parse(_src())
    code = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Name):
            code.append(node.id)
        elif isinstance(node, ast.Attribute):
            code.append(node.attr)
    joined = " ".join(code).lower()
    for forbidden in ("reboot", "shutdown", "netsh", "restart"):
        assert forbidden not in joined, (
            "autodeploy calls something named %r - unactivated boxes must "
            "never be rebooted" % forbidden)
    # every command it sends is a literal, and one of these
    calls = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
             and isinstance(n.func, ast.Attribute)
             and n.func.attr in ("command_text", "command_binary", "send_command")]
    assert calls, "the command calls moved - re-point this test"
    sent = set()
    for n in calls:
        arg = n.args[0]
        assert isinstance(arg, ast.Constant), \
            "line %d sends a computed command" % n.lineno
        sent.add(arg.value)
    assert sent == {"GAMESYNC RESET", "GAMESYNC START", "GAMESYNC STATUS",
                    "GAMESYNC LIBRARY", "HWPROFILE", "PING"}, sent


def test_status_prints_without_touching_the_network(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(ad, "LIBRARY", _lib(tmp_path, "_deploy_generation.txt", "3\n"))
    p = tmp_path / "s.json"
    p.write_text(json.dumps({"192.168.1.133": {"titles": TITLES},
                             "DELL": {"titles": TITLES, "generation": "3",
                                      "ip": "192.168.1.145"}}))
    monkeypatch.setattr(ad, "RetroConnection", None)       # any use would crash
    ad.main(["--status", "--state", str(p)])
    out = capsys.readouterr().out
    assert "deploy generation '3'" in out and "not yet migrated" in out
    assert re.search(r"DELL\s+192\.168\.1\.145\s+3\s+5", out)
