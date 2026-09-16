"""
v56k_bench / v56k_screen: the Voodoo 5 6000 benchmark campaign runner.

Each test here locks in a defect that was real on 2026-09-12 while measuring
the V5 6000 at .191, and most of them assert the NEGATIVE path - a checker that
can only say OK is the exact failure this project keeps paying for.

The defects, in the order they were found:

1. The old benchmark_runner.py iterated an `fsaa` axis and NEVER APPLIED it,
   writing the requested level into results.csv as though it had taken effect.
   v56k_bench applies SSTH3_SLI_AA_CONFIGURATION and reads it back.
2. Adding the `api` column mid-campaign wrote NEW rows in the NEW order under
   the OLD header, shifting every later field by one while the file stayed
   perfectly well-formed. Two rows were corrupted for real.
3. The stall detector gave up on a run whose log stopped growing - but an id
   engine prints nothing between loading the map and finishing the timedemo, so
   a healthy 4x-AA run at 1600x1200 is silent for minutes.
4. Two of the nine configurations the driver's control panel offers wedge the
   display driver and kill the agent (cfg 2 = 2-chip SLI, cfg 8 = 4-chip 8x AA),
   each costing a trip to the machine.
"""

import csv
import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load(name):
    path = REPO / "scripts" / "benchmarks" / f"{name}.py"
    if not path.exists():
        pytest.skip(f"{path} not present")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def bench():
    return _load("v56k_bench")


@pytest.fixture(scope="module")
def screen():
    return _load("v56k_screen")


# --------------------------------------------------------------------------- #
# 1. the AA axis is the driver's own enumeration, not an invented scale
# --------------------------------------------------------------------------- #

def test_aa_configs_match_the_drivers_own_tweak_map(bench):
    """The chip/AA table is read out of 3dfx Tools' descriptors on the card.

    QuadChipAASLI's "Tweak Map" is 0,5,6,7,8 for Single Chip Only / Fastest
    Performance / 2,4,8 Sample AA; DualChipAASLI's is 0,2,3,4; SingleChip's is
    0,1. Anything that renumbers these is mislabeling every row in the article.
    """
    expect = {
        0: (1, 1), 1: (1, 2),
        2: (2, 1), 3: (2, 2), 4: (2, 4),
        5: (4, 1), 6: (4, 2), 7: (4, 4), 8: (4, 8),
    }
    got = {k: (v["chips"], v["samples"]) for k, v in bench.AA_CONFIGS.items()}
    assert got == expect


def test_eight_sample_aa_requires_four_chips(bench):
    """8x is the card's whole reason for existing and cannot exist on fewer
    chips - if this ever passes with chips<4 the table has been corrupted."""
    assert bench.AA_CONFIGS[8]["chips"] == 4
    assert bench.AA_CONFIGS[8]["samples"] == 8
    for cfg, meta in bench.AA_CONFIGS.items():
        if meta["samples"] == 8:
            assert meta["chips"] == 4, f"cfg {cfg} claims 8 samples on fewer chips"


# --------------------------------------------------------------------------- #
# 2. the CSV column migration - a defect that really corrupted rows
# --------------------------------------------------------------------------- #

def test_migrate_header_rewrites_an_older_header_without_losing_rows(bench, tmp_path):
    csv_path = tmp_path / "results.csv"
    old_cols = [c for c in bench.CSV_COLS if c != "api"]
    with csv_path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=old_cols)
        w.writeheader()
        w.writerow({c: "" for c in old_cols} | {
            "title": "Quake III Arena", "res": "640x480", "width": "640",
            "height": "480", "colordepth": "16", "aa_cfg": "5",
            "chips": "4", "samples": "1", "aa_label": "4chip-noaa",
            "avg_fps": "150.2", "status": "ok"})

    bench.migrate_header(csv_path)

    rows = list(csv.DictReader(csv_path.open()))
    assert len(rows) == 1, "migration must not lose measured rows"
    r = rows[0]
    # The whole point: fields did not shift. width is still a width.
    assert r["width"] == "640" and r["height"] == "480"
    assert r["avg_fps"] == "150.2" and r["status"] == "ok"
    assert r["api"] == "", "a column the old file lacked must read blank, not shifted"
    assert (tmp_path / "results.csv.pre-migration").exists(), \
        "the previous file must be kept, not silently replaced"


def test_migrate_header_is_a_noop_when_the_header_already_matches(bench, tmp_path):
    csv_path = tmp_path / "results.csv"
    with csv_path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=bench.CSV_COLS)
        w.writeheader()
    before = csv_path.read_bytes()
    bench.migrate_header(csv_path)
    assert csv_path.read_bytes() == before
    assert not (tmp_path / "results.csv.pre-migration").exists()


def test_append_row_writes_every_column_in_the_declared_order(bench, tmp_path):
    """append_row must key off CSV_COLS, so a row missing a field lands blank in
    the right place rather than shifting the ones after it."""
    csv_path = tmp_path / "results.csv"
    bench.append_row(csv_path, {"title": "T", "avg_fps": "1.5", "status": "ok"})
    with csv_path.open(newline="") as fh:
        rd = csv.reader(fh)
        assert next(rd) == bench.CSV_COLS
        row = next(rd)
    assert len(row) == len(bench.CSV_COLS)
    d = dict(zip(bench.CSV_COLS, row))
    assert d["title"] == "T" and d["avg_fps"] == "1.5" and d["status"] == "ok"


# --------------------------------------------------------------------------- #
# 3. a quiet log is not a hang until the renderer has identified itself
# --------------------------------------------------------------------------- #

def test_a_silent_log_after_the_renderer_came_up_is_not_a_wedge(bench):
    """id engines print NOTHING through the timedemo. Treating that silence as
    a hang would have recorded every slow high-resolution AA run as broken."""
    assert bench.renderer_up("GL_RENDERER: Mesa Glide v0.63 Voodoo5 6000 (tm)")
    assert bench.renderer_up("...Bound to GlideDrv.dll")
    assert bench.renderer_up("GR_RENDERER: Glide")


def test_a_log_that_never_reached_the_renderer_is_a_wedge(bench):
    """This is the real 8x-AA signature: the log ends at the OpenGL shutdown
    that precedes re-init and no renderer line ever appears."""
    assert not bench.renderer_up("")
    assert not bench.renderer_up(None)
    assert not bench.renderer_up(
        "...setting mode -1\nShutting down OpenGL subsystem\n"
        "...wglMakeCurrent( NULL, NULL ): success")


def test_the_fps_line_is_parsed_in_both_id_punctuation_styles(bench):
    """Quake III/II write '1260 frames, 8.2 seconds: 154.2 fps'; GLQuake writes
    the same numbers with no punctuation at all."""
    q3 = bench.Quake3()
    for raw in ("1260 frames, 8.2 seconds: 154.2 fps",
                "1260 frames 8.2 seconds 154.2 fps"):
        got = q3.parse(raw)
        assert got == {"frames": 1260, "seconds": 8.2, "avg_fps": 154.2}, raw


def test_the_last_timedemo_in_the_log_wins(bench):
    """A log can hold an earlier run; the measured one is the final line."""
    q3 = bench.Quake3()
    raw = ("1260 frames, 50.9 seconds: 24.7 fps\n"
           "...\n1260 frames, 8.2 seconds: 154.2 fps\n")
    assert q3.parse(raw)["avg_fps"] == 154.2


# --------------------------------------------------------------------------- #
# 4. the configurations that kill the box, and the ones merely suspected
# --------------------------------------------------------------------------- #

def test_the_measured_box_killers_are_excluded_by_default(bench):
    """cfg 2 and cfg 8 each took the agent down on .191. A campaign that needs
    someone at the machine halfway through is not a campaign."""
    assert 2 in bench.HAZARD_CONFIGS
    assert 8 in bench.HAZARD_CONFIGS
    for cfg, why in bench.HAZARD_CONFIGS.items():
        assert why.strip(), f"cfg {cfg} is excluded with no stated reason"


def test_suspected_and_measured_are_kept_apart(bench):
    """'We looked and it cannot work' and 'we never looked' are different facts
    and must not render the same - cfg 3 and 4 are the same DualChipAASLI
    family as cfg 2 but have never been tested."""
    assert set(bench.SUSPECT_CONFIGS) == {3, 4}
    assert not set(bench.SUSPECT_CONFIGS) & set(bench.HAZARD_CONFIGS)
    for why in bench.SUSPECT_CONFIGS.values():
        assert "UNTESTED" in why.upper()


def test_the_dangerous_game_local_glide_is_refused_not_merely_noted(bench):
    """989,027 bytes is the clean-room glide3x_h5, which hard-freezes this
    4-chip board (FINDINGS 2026-09-04) and is staged in Quake2Complete right
    now. 1,310,720 is the nGlide wrapper, which merely bypasses the card."""
    assert 989027 in bench.DANGEROUS_GLIDE
    assert 1310720 in bench.WRAPPER_GLIDE
    assert not set(bench.DANGEROUS_GLIDE) & set(bench.WRAPPER_GLIDE)


# --------------------------------------------------------------------------- #
# 5. the screener keeps refused and hung apart
# --------------------------------------------------------------------------- #

def test_screener_distinguishes_a_clean_refusal_from_a_hang(screen):
    """Both fail to render, but a clean refusal costs nothing and a hang costs a
    walk to the machine - collapsing them loses the only operational difference."""
    assert screen.classify("RESULT: ok")[0] == "open"
    assert screen.classify("RESULT: winopen-refused")[0] == "refused"
    assert screen.classify("RESULT: probe-ok-noopen")[0] == "noopen"

    verdict, detail = screen.classify(
        "step: grSstWinOpen(hWnd=0x00070123, res=640x480, 60Hz, ABGR, ...)")
    assert verdict == "hung"
    assert "grSstWinOpen" in detail, "a hang must name where it died"


def test_screener_reports_a_hang_when_there_is_no_result_line_at_all(screen):
    """The probe flushes each step to disk, so a truncated log IS the evidence;
    absence of RESULT must never be read as success."""
    assert screen.classify("")[0] == "hung"
    assert screen.classify("glideprobe: res=640x480\nstep: grGlideInit()")[0] == "hung"


# --------------------------------------------------------------------------- #
# v56k_sweep wedge recovery + the agent watchdog that makes it possible
#
# Added 2026-09-15. Until the watchdog existed, a wedge took the agent's
# process down with it and the box was gone until somebody walked to it - so
# the only safe thing the sweep could do was stop, which it did, after ONE
# measured cell of a nine-config matrix. These lock in the two halves of the
# fix: the box restarts its own agent, and the sweep spends another boot only
# when the last one actually measured something.
# --------------------------------------------------------------------------- #

@pytest.fixture(scope="module")
def sweep():
    return _load("v56k_sweep")


@pytest.fixture(scope="module")
def watchdog():
    path = REPO / "scripts" / "fleet" / "install-agent-watchdog.py"
    if not path.exists():
        pytest.skip(f"{path} not present")
    spec = importlib.util.spec_from_file_location("install_agent_watchdog", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["install_agent_watchdog"] = mod
    spec.loader.exec_module(mod)
    return mod


def _write_csv(path, rows):
    cols = ["title", "api", "res", "colordepth", "aa_cfg", "status", "avg_fps"]
    with path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def test_measured_cells_counts_only_verified_rows(sweep, tmp_path):
    """A wedged pass still APPENDS its failed cell, so counting rows would
    report progress on a pass that measured nothing and buy it another boot,
    forever."""
    _write_csv(tmp_path / "results.csv", [
        {"title": "Quake III Arena", "api": "opengl-icd", "res": "640x480",
         "colordepth": "16", "aa_cfg": "1", "status": "ok", "avg_fps": "106.9"},
        {"title": "Quake III Arena", "api": "opengl-icd", "res": "800x600",
         "colordepth": "16", "aa_cfg": "1", "status": "wedged", "avg_fps": ""},
    ])
    assert sweep.measured_cells(tmp_path) == 1


def test_measured_cells_is_zero_before_the_first_run(sweep, tmp_path):
    assert sweep.measured_cells(tmp_path) == 0


def test_a_config_gets_more_than_one_attempt_by_default(sweep):
    """One wedge must cost a reboot, not the sweep."""
    import argparse as _ap
    p = [a for a in _parser_actions(sweep) if a.dest == "attempts"]
    assert p and p[0].default > 1


def _parser_actions(mod):
    import argparse
    ap = argparse.ArgumentParser()
    # rebuild the parser the way main() does, without running it
    src = (REPO / "scripts" / "benchmarks" / "v56k_sweep.py").read_text()
    ns = {"argparse": argparse}
    body = src[src.index("    ap = argparse.ArgumentParser("):src.index("    a = ap.parse_args()")]
    exec("import argparse\n" + "\n".join(l[4:] for l in body.splitlines()), ns)
    return ns["ap"]._actions


def test_the_watchdog_is_a_separate_run_value_from_the_agent(watchdog):
    """Clobbering HKLM Run\\RetroAgent would replace the thing it exists to
    restart - and auto-login is what starts the agent after a reboot."""
    assert watchdog.RUN_VAL != "RetroAgent"


def test_the_watchdog_never_puts_a_password_in_argv(watchdog):
    """The reason this is a Run-key loop and not `schtasks /ru <user> /rp
    <password>`. The console password is a documented fleet convention rather
    than a secret, but argv lands in transcripts regardless."""
    # Only what actually reaches the box - the module docstring EXPLAINS why
    # /rp is avoided, and scanning it fails the test for saying so.
    shipped = watchdog.WATCHDOG + watchdog.WD_PATH + watchdog.RUN_KEY + watchdog.RUN_VAL
    assert "/rp" not in shipped
    assert "password" not in shipped.lower()
    assert "schtasks" not in shipped.lower()


def test_the_watchdog_relaunches_detached_and_keeps_looping(watchdog):
    """`start ""` so the loop does not block on the agent it just started, and
    a goto so one restart is not the end of the supervision."""
    assert 'start ""' in watchdog.WATCHDOG
    assert "goto loop" in watchdog.WATCHDOG
    assert "ping -n" in watchdog.WATCHDOG, "XP's shell has no `timeout` command"


def test_the_watchdog_batch_is_crlf(watchdog):
    """A LF-only .cmd is not reliably parsed by XP's command processor."""
    assert "\r\n" in watchdog.WATCHDOG
    assert "\n" not in watchdog.WATCHDOG.replace("\r\n", "")


def test_the_watchdog_logs_every_restart(watchdog):
    """A supervisor that silently fixes things hides how often the driver
    wedges - which is the measurement this campaign is actually for."""
    assert ">>" in watchdog.WATCHDOG and "agentwd.log" in watchdog.WATCHDOG


# --------------------------------------------------------------------------- #
# v56k_shots - the image-quality pass
#
# The first version of that file carried its OWN copy of the cvar dialect, and
# the copy was wrong in two ways the fps campaign had already paid to learn: it
# set LATCHED cvars from a command-line exec (which lands after R_Init, so they
# either do nothing or need the vid_restart that hung the driver at 8x AA), and
# it wrote Quake III's cvars into Quake II. These pin the engine facts.
# --------------------------------------------------------------------------- #

@pytest.fixture(scope="module")
def shots():
    return _load("v56k_shots")


def test_quake2_waits_one_frame_per_line_because_its_wait_takes_no_argument(shots):
    """id Tech 2's `wait` is not id Tech 3's. `wait 250` there delays a SINGLE
    frame and photographs the opening frame of the demo - a shot that looks
    perfectly fine and is the wrong scene at every AA level."""
    cfg = shots.Q2Shot(wait_frames=250).bench_cfg()
    lines = [l.strip() for l in cfg.splitlines()]
    assert lines.count("wait") >= 250
    assert not any(l.startswith("wait ") for l in lines), \
        "an argument to id Tech 2's wait is silently ignored"


def test_quake3_keeps_the_latched_cvars_out_of_the_exec_file(shots):
    """r_mode/r_customwidth/r_colorbits are CVAR_LATCH. In the file the command
    line execs they arrive after R_Init, and 'fixing' that with vid_restart is
    what wedged the board at 4 chips / 8x AA."""
    cfg = shots.Q3Shot().bench_cfg()
    for latched in ("r_mode", "r_customwidth", "r_customheight",
                    "r_colorbits", "r_texturebits", "r_fullscreen"):
        assert latched not in cfg, f"{latched} is latched - it belongs in fleetres.cfg"
    assert "vid_restart" not in cfg


def test_quake3_still_sets_the_latched_cvars_somewhere(shots):
    """The negative above is only safe because the launcher sets them BEFORE
    R_Init, by both routes that agree."""
    g = shots.Q3Shot()
    assert 'r_customwidth "800"' in g.fleetres_cfg(800, 600, 16)
    assert "+set r_customwidth 800" in g.setargs(800, 600, 16)


def test_rtcw_never_asks_for_r_mode_minus_one(shots):
    """RtCW's id Tech 3 fork has no r_mode -1 branch: it renders 640x480 rather
    than erroring, so the whole column would be the wrong resolution and look
    fine."""
    bat = shots.RTCWShot().launch_bat(1024, 768, 16, {})
    assert "+set r_mode 6" in bat
    assert "r_mode -1" not in bat
    assert "r_customwidth" not in bat


def test_rtcw_refuses_a_resolution_its_mode_table_does_not_have(shots):
    """Rounding to a nearby mode would silently mislabel the capture."""
    g = shots.RTCWShot()
    assert g.supports(1024, 768, 16) is None
    why = g.supports(1280, 960, 16)
    assert why and "mode table" in why


def test_every_shot_title_photographs_a_fixed_scene(shots):
    """A shot taken 'a few seconds in' lands on a different frame each run, and
    edge-quality differences are subtler than scene differences."""
    for make in shots.GAMES.values():
        g = make()
        cfg = g.bench_cfg() if hasattr(g, "bench_cfg") else g.shot_cfg()
        assert "screenshot" in cfg.lower()
        assert "wait" in cfg
        assert cfg.strip().endswith("quit")


def test_the_sweep_refuses_a_results_directory_that_will_not_survive(sweep, tmp_path, capsys):
    """A results path that can vanish is worse than none: every reboot the
    sweep spends is unrecoverable hardware time and the rows are the only
    record of it. A session scratchpad under /tmp is deleted when the session
    ends - which really happened, taking 14 measured rows with it."""
    import argparse
    ap = [a for a in _parser_actions(sweep) if a.dest == "allow_volatile_outdir"]
    assert ap, "the escape hatch must exist, and be explicit"
    assert ap[0].default is False, "volatile results must be opt-in"
    src = (REPO / "scripts" / "benchmarks" / "v56k_sweep.py").read_text()
    assert "/tmp/" in src and "REFUSING" in src


# --------------------------------------------------------------------------- #
# Version tracking (user requirement, 2026-09-15): every benchmark row must
# record WHAT WAS RUNNING - the game binary included - not just how fast it ran.
# --------------------------------------------------------------------------- #

def test_every_row_records_the_game_and_driver_under_test(bench):
    """A number without its versions cannot be compared against a later one."""
    for col in ("game_exe", "game_size", "game_md5",
                "driver_pkg", "driver_ver", "glide3x_md5", "icd_md5",
                "os_build", "agent_ver", "gpu"):
        assert col in bench.CSV_COLS, f"{col} must be stamped on every row"


def test_the_version_columns_are_at_the_end_so_older_rows_still_migrate(bench):
    """migrate_header rewrites an older CSV, but status/notes must stay last:
    they are what a reader scans for, and a column added AFTER them would put
    the failure reason in the middle of a hash soup."""
    assert bench.CSV_COLS[-2:] == ["status", "notes"]


def test_versions_are_identified_by_hash_not_only_by_version_string(bench):
    """A driver DLL here may carry no version resource, or carry the version of
    the package it was rebranded from - AmigaMerlin ships rebranded 3dfx
    binaries. Only a hash actually distinguishes two builds."""
    assert "glide3x" in bench.VERSION_FILES and "icd" in bench.VERSION_FILES
    src = (REPO / "scripts" / "benchmarks" / "v56k_bench.py").read_text()
    assert "hashlib.md5" in src


def test_a_retroactive_probe_never_overwrites_a_runtime_capture():
    """A value captured while the run happened is evidence; one probed days
    later is an assumption, and must not be able to replace the first."""
    path = REPO / "scripts" / "benchmarks" / "v56k_versions.py"
    if not path.exists():
        pytest.skip("v56k_versions.py not present")
    src = path.read_text()
    assert "retroactive" in src
    # the guard itself: only fill a cell that is empty
    assert 'not (r.get(k) or "").strip()' in src
    assert "versions backfilled retroactively" in src, \
        "a backfilled row must say so in the row"
