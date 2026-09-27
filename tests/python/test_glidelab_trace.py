"""glidelab --trace N and glidelab_run.py --trace / --collect (2026-09-27).

Three AA configurations froze .124 inside Glide calls (grSstWinOpen for cfg 7
and cfg 1, grLfbReadRegion for cfg 3); glidelab's flushed step log named the
CALL each time and nothing could say which step inside it. Our h5 Glide's
AA-TRACE (fork voidsstr/retro3dfx-glide, FX_GLIDE_TRACE) writes those steps,
each flushed to the disk first. These tests pin the plumbing that turns it on
and brings the file home:

- glidelab.c: --trace 0..2 only (refused before anything switches), the
  Glide settings made where Glide reads them (glide_env, before LoadLibrary),
  the file named after glidelab's own log (<log>.trace), the splash plugin
  off for AA configurations only (a mirror of Glide's own table), a path too
  long for the environment refused rather than truncated, and "trace":N in
  the RESULT so a traced run is never read as a benchmark;
- glidelab_run.py: the old trace deleted before the session and DOWNLOADed
  after it (after the cleanup, when the session had to be reaped), saved in
  the repo tree next to the step log; a trace without its header is a
  "trace_error" that fails the run; an untraced run is exactly as before;
- --collect, for after the power cycle a wedge needs: three DOWNLOADs and
  nothing else - no gate, no upload, no registry, no launch.

Nothing here touches the network: the agent is a fake.
"""
import asyncio
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
TOOLS = KMD / "tools"
sys.path.insert(0, str(TOOLS))

import glidelab_run  # noqa: E402
import mode_sweep  # noqa: E402

GLIDELAB = (TOOLS / "glidelab.c").read_text()
LOG = r"C:\vcr\glidelab\fill.log"
TRACE = LOG + ".trace"
HEADER = b"=== glidetrace pid=1212 tick=5 level=2 file=C:\\vcr\\glidelab\\fill.log.trace\n"
GOOD_LOG = b'step: grSstWinOpen\nRESULT {"mode":"fill","ok":1,"opened_hz":85,"trace":2}\n'
UNTRACED_LOG = b'step: grSstWinOpen\nRESULT {"mode":"fill","ok":1,"opened_hz":85}\n'

CR = REPO / "voodoo-cleanroom"
MAIN_CR = Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom")
GPCI = next((p / "build/retro3dfx-glide/glide3x/h5/glide3/src/gpci.c" for p in (CR, MAIN_CR)
             if (p / "build/retro3dfx-glide/glide3x/h5/glide3/src/gpci.c").is_file()), None)


def _blank(text):
    out, i, n = list(text), 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif text[i] in "\"'":
            j = i + 1
            while j < n and text[j] != text[i]:
                j += 2 if text[j] == "\\" else 1
            j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def _func(text, header):
    """Definition of the C function whose header line contains `header`."""
    s = text.index(header)
    b = _blank(text)
    o = b.index("{", s)
    depth = 0
    for i in range(o, len(b)):
        depth += {"{": 1, "}": -1}.get(b[i], 0)
        if depth == 0:
            return text[s:i + 1]
    raise AssertionError(header)


# ---- glidelab.c ------------------------------------------------------------------


def test_glidelab_turns_the_trace_on_where_glide_reads_it_before_the_dll_loads():
    main = _func(GLIDELAB, "int main(int argc, char **argv)")
    code = _blank(main)
    load = main.index("LoadLibraryA(O.dll)")
    # parsed, and a bad value refused before anything is loaded or switched
    assert "parse_trace(v, &O.trace)" in main
    assert main.index("if (bad_trace) {") < load
    assert re.search(r"if \(bad_trace\) \{[^}]*return 2;", code)
    # the file is named after glidelab's own log, and a path too long for
    # it is refused, not truncated
    assert '"%s.trace", g_logpath' in main
    # every Glide setting through glide_env (msvcrt's copy, which Glide's
    # getenv reads), all before the DLL loads
    for var in ("FX_GLIDE_TRACE_FILE", "FX_GLIDE_TRACE", "FX_GLIDE_NO_SPLASH"):
        at = main.index(f'glide_env("{var}"')
        assert at < load, var
    assert "SetEnvironmentVariableA" not in code
    # the splash plugin is turned off for an AA configuration only
    ns = main.index('glide_env("FX_GLIDE_NO_SPLASH", "1")')
    assert "if (aa_config(O.cfg))" in main[ns - 60:ns]
    # Glide appends: the old file goes first
    assert main.index("DeleteFileA(g_tracepath);") < main.index('glide_env("FX_GLIDE_TRACE_FILE"')
    # nothing of the trace happens without --trace
    blk = main[main.index("if (O.trace > 0) {"):load]
    assert "glide_env(\"FX_GLIDE_TRACE\"" in blk and "DeleteFileA" in blk


def test_a_glide_setting_too_long_for_the_environment_is_refused_not_truncated():
    env = _func(GLIDELAB, "static int glide_env(const char *name, const char *value)")
    assert "char kv[MAX_PATH + 64];" in env
    assert re.search(r"if \(n < 0 \|\| n >= \(int\)sizeof kv\) \{[^}]*return 0;", env)
    assert env.index("return 0;") < env.index("_putenv(kv);")
    assert "kv[sizeof kv - 1] = 0;" not in env                 # the old silent cut


def test_a_traced_result_says_so_and_the_step_log_says_whether_glide_wrote_it():
    tail = _func(GLIDELAB, "static const char *tail_json(void)")
    assert 'if (O.trace > 0)' in tail and ',\\"trace\\":%d' in tail
    chk = _func(GLIDELAB, "static void trace_check(const char *when)")
    assert "GetFileAttributesExA(g_tracepath" in chk and "MISSING" in chk
    main = _func(GLIDELAB, "int main(int argc, char **argv)")
    assert main.index('trace_check("after grSstWinOpen")') > main.index("ctx = open_board(hwnd, rescode, hzcode);\n        if (!ctx)")
    assert 'trace_check("after the cycles")' in main
    # the options struct grew at its END: `cycles` is still the 11th field
    m = re.search(r"\} O = \{([^}]*)\};", GLIDELAB)
    fields = [f.strip() for f in m.group(1).split(",")]
    assert len(fields) == 13 and fields[10] == "2" and fields[12] == "0"


def test_glidelab_aa_configs_are_glides_own():
    """aa_config() must name exactly the SSTH3_SLI_AA_CONFIGURATION values for
    which Glide's gpci.c switch sets an AA sample count (fall-through
    included: case 1 sets 2 samples and falls into case 0)."""
    listed = {int(x) for x in re.findall(r"cfg == (\d+)", _func(GLIDELAB, "static int aa_config(int cfg)"))}
    assert listed == {1, 3, 4, 6, 7, 8}
    if GPCI is None:
        pytest.skip("retro3dfx-glide clone absent - aa_config NOT checked against gpci.c")
    g = GPCI.read_text(encoding="latin-1")
    sw = g[g.index('switch(GLIDE_GETENV("SSTH3_SLI_AA_CONFIGURATION", 2L))'):]
    sw = _blank(sw[:sw.index("_GlideRoot.environment.outputBpp")])
    aa, pending = set(), set()
    for m in re.finditer(r"\bcase\s+(\d+)\s*:|aaSample\s*=\s*(\d+)|\bbreak\b|\bdefault\s*:", sw):
        if m.group(1):
            pending.add(int(m.group(1)))
        elif m.group(2):
            if int(m.group(2)) > 0:
                aa |= pending
        elif m.group(0) == "break":
            pending = set()
    assert aa == listed


def test_glidelab_trace_helpers_run():
    """parse_trace and aa_config, compiled from glidelab.c and run."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        pytest.skip("no host C compiler - the glidelab helpers were NOT run")
    import tempfile
    body = "\n".join((
        "#include <stdio.h>",
        "#define GLIDELAB_MAX_TRACE %d" % int(re.search(r"#define GLIDELAB_MAX_TRACE (\d+)", GLIDELAB).group(1)),
        _func(GLIDELAB, "static int aa_config(int cfg)"),
        _func(GLIDELAB, "static int parse_trace(const char *v, int *out)"),
        'int main(void) {',
        '  const char *in[] = {"0", "1", "2", "3", "9", "-1", "", "1x", "12", " 1", 0};',
        '  int i, v;',
        '  for (i = 0; in[i]; i++) { int ok; v = -7; ok = parse_trace(in[i], &v);',
        '                            printf("%s=%d:%d\\n", in[i], ok, v); }',
        '  for (i = -1; i <= 9; i++) printf("aa%d=%d\\n", i, aa_config(i));',
        '  printf("null=%d\\n", parse_trace(0, &v));',
        '  return 0; }'))
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "h.c").write_text(body)
        r = subprocess.run([cc, "-Wall", "-Werror", "-o", str(Path(d) / "h"), str(Path(d) / "h.c")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        out = subprocess.run([str(Path(d) / "h")], capture_output=True, text=True).stdout
    got = dict(ln.split("=", 1) for ln in out.splitlines())
    for ok, lvl in (("0", 0), ("1", 1), ("2", 2)):
        assert got[ok] == f"1:{lvl}"
    for bad in ("3", "9", "-1", "", "1x", "12", " 1"):
        assert got[bad] == "0:-7", bad                      # refused, and nothing stored
    assert got["null"] == "0"
    assert {i for i in range(-1, 10) if got[f"aa{i}"] == "1"} == {1, 3, 4, 6, 7, 8}


# ---- glidelab_run.py --------------------------------------------------------------


class FakeBox:
    """v56k_bench.Box's interface; records every command in order. `files`
    maps a remote path to bytes, None (absent) or an exception to raise."""

    def __init__(self, files=None, execw=""):
        self.files = dict(files or {})
        self.execw = execw
        self.timeline = []

    async def cmd(self, command, timeout=60.0):
        self.timeline.append(command)
        if command == "PROCLIST":
            return 0, json.dumps([{"name": "explorer.exe", "pid": 1}])
        if command.startswith("EXECW"):
            if isinstance(self.execw, Exception):
                raise self.execw
            return 0, self.execw
        return 0, ""

    async def text(self, command, timeout=60.0):
        return (await self.cmd(command, timeout))[1]

    async def exec_(self, cmdline, timeout=90.0):
        return (await self.cmd(f"EXEC {cmdline}", timeout))[1]

    async def upload(self, remote, data):
        self.timeline.append(f"UPLOAD {remote}")

    async def download(self, remote):
        self.timeline.append(f"DOWNLOAD {remote}")
        v = self.files.get(remote)
        if isinstance(v, Exception):
            raise v
        return v

    def at(self, needle):
        return next(i for i, c in enumerate(self.timeline) if needle in c)


def _parsed(argv):
    """The Namespace glidelab_run.main() builds for argv, without running it."""
    got = {}

    def fake_run(coro):
        got["a"] = coro.cr_frame.f_locals["a"]
        coro.close()
        return 0

    with pytest.MonkeyPatch.context() as m:
        m.setattr(sys, "argv", ["glidelab_run.py"] + argv)
        m.setattr(glidelab_run.asyncio, "run", fake_run)
        with pytest.raises(SystemExit) as ex:
            glidelab_run.main()
    return got.get("a"), ex.value.code


def _args(tmp_path, *extra):
    a, code = _parsed(["h", "fill", "--res", "1024x768", "--refresh", "85",
                       "--save-dir", str(tmp_path / "saved")] + list(extra))
    assert code == 0
    return a


@pytest.fixture(autouse=True)
def _no_host_waits(monkeypatch):
    async def no_wait(*a, **k):
        return None
    monkeypatch.setattr(mode_sweep.asyncio, "sleep", no_wait)


def test_the_trace_level_is_bounded_before_anything_connects():
    a, code = _parsed(["h", "fill", "--trace", "3"])
    assert code == 2 and a is None                      # argparse refused it
    a, code = _parsed(["h", "fill", "--trace", "2"])
    assert code == 0 and a.trace == 2
    a, code = _parsed(["h", "fill"])
    assert code == 0 and a.trace == 0 and not a.collect
    # the host's levels are glidelab's
    assert max(glidelab_run.TRACE_LEVELS) == int(
        re.search(r"#define GLIDELAB_MAX_TRACE (\d+)", GLIDELAB).group(1))
    # and the file glidelab names is the one the host fetches
    assert glidelab_run.trace_path(LOG) == TRACE and '"%s.trace", g_logpath' in GLIDELAB


def test_a_traced_session_deletes_the_old_trace_first_and_brings_it_home(tmp_path):
    a = _args(tmp_path, "--trace", "2")
    trace = HEADER + b"100 winopen: fifo renderMode reg=00000000 val=00000000\n" \
                     b"101 executed: winopen: fifo renderMode reg=00000000 val=00000000\n"
    box = FakeBox({LOG: GOOD_LOG, TRACE: trace})
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    execw = box.at("EXECW")
    assert "--trace 2" in box.timeline[execw]
    assert box.at(f'del /f /q "{TRACE}"') < execw < box.at(f"DOWNLOAD {TRACE}")
    assert "error" not in res and "trace_error" not in res
    assert res["trace"] == 2                            # glidelab's RESULT: the level
    t = res["trace_file"]
    assert t["header"] and t["lines"] == 3 and t["last"].startswith("101 executed: winopen")
    saved = Path(t["saved"])
    assert saved.read_bytes() == trace and saved.parent == tmp_path / "saved"
    assert Path(t["log_saved"]).read_bytes() == GOOD_LOG


def test_an_untraced_session_is_exactly_as_before(tmp_path):
    a = _args(tmp_path)
    box = FakeBox({LOG: UNTRACED_LOG})
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert "trace" not in res and "trace_file" not in res and "error" not in res
    assert "--trace" not in box.timeline[box.at("EXECW")]
    assert not [c for c in box.timeline if ".trace" in c]
    assert not (tmp_path / "saved").exists()


@pytest.mark.parametrize("trace", (None, b"", b"a line that is not a trace\n", OSError("dropped")),
                         ids=("absent", "empty", "no-header", "download-raised"))
def test_a_trace_asked_for_and_not_produced_fails_the_run_without_a_reap(tmp_path, trace):
    a = _args(tmp_path, "--trace", "1")
    box = FakeBox({LOG: GOOD_LOG, TRACE: trace})
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert res["trace_error"] and "error" not in res
    # the session itself was fine: no cleanup ran (one PROCLIST, pre-launch)
    assert box.timeline.count("PROCLIST") == 1 and "cleanup" not in res


def _main(monkeypatch, tmp_path, box, argv):
    a, code = _parsed(argv)
    assert code == 0
    (tmp_path / "out").mkdir(exist_ok=True)
    (tmp_path / "out" / "glidelab.exe").write_bytes(b"MZ")
    monkeypatch.setattr(glidelab_run, "KMD", tmp_path)
    monkeypatch.setattr(glidelab_run.vb, "Box", lambda ip: box)

    async def gate(b, a, modes):
        return {}, {}, None
    monkeypatch.setattr(glidelab_run, "gate", gate)
    monkeypatch.setattr(glidelab_run.ms, "rates", lambda calc, m: "")
    monkeypatch.setattr(glidelab_run.ms, "describe", lambda rng: "")
    return asyncio.run(glidelab_run.main_async(a))


def test_the_run_fails_when_its_trace_is_missing_and_passes_when_it_is_there(monkeypatch, tmp_path):
    argv = ["h", "fill", "--res", "1024x768", "--refresh", "85", "--trace", "1",
            "--save-dir", str(tmp_path / "s")]
    assert _main(monkeypatch, tmp_path, FakeBox({LOG: GOOD_LOG, TRACE: None}), argv) == 1
    assert _main(monkeypatch, tmp_path, FakeBox({LOG: GOOD_LOG, TRACE: HEADER}), argv) == 0
    # untraced, as before
    assert _main(monkeypatch, tmp_path, FakeBox({LOG: UNTRACED_LOG}),
                 ["h", "fill", "--res", "1024x768", "--refresh", "85"]) == 0


def test_after_a_timed_out_session_the_trace_is_read_after_the_cleanup(tmp_path):
    """A session that outlived its EXECW may still be appending: the trace is
    DOWNLOADed after the reap, and a dead box's silence is reported, not
    fatal to the host."""
    a = _args(tmp_path, "--trace", "2")
    box = FakeBox({LOG: None, TRACE: HEADER + b"7 lfb read: scanline reg=00000100 val=00080000\n"},
                  execw=asyncio.TimeoutError("the host gave up"))
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert res["timed_out"] and res["cleanup"]
    procs = [i for i, c in enumerate(box.timeline) if c == "PROCLIST"]
    assert len(procs) >= 2 and box.at(f"DOWNLOAD {TRACE}") > procs[1]
    assert res["trace_file"]["last"].endswith("lfb read: scanline reg=00000100 val=00080000")
    assert "log_saved" not in res["trace_file"]             # no log came back: none saved


def test_collect_downloads_three_files_and_does_nothing_else(monkeypatch, tmp_path, capsys):
    files = {LOG: b"step: grSstWinOpen 1024x768 60Hz origin upper\n",
             TRACE: HEADER + b"9 SLI_AA_REQUEST: ExtEscape (kernel programs SLI/AA) reg=00000013 val=0\n",
             glidelab_run.DEFAULT_TRACE: None}
    box = FakeBox(files)
    rc = _main(monkeypatch, tmp_path, box, ["h", "fill", "--collect", "--save-dir", str(tmp_path / "c")])
    assert rc == 0
    assert box.timeline == [f"DOWNLOAD {LOG}", f"DOWNLOAD {TRACE}",
                            f"DOWNLOAD {glidelab_run.DEFAULT_TRACE}"]
    out = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert out["collect"] and out["log"]["last"].startswith("step: grSstWinOpen")
    assert out["trace"]["header"] and "SLI_AA_REQUEST: ExtEscape" in out["trace"]["last"]
    assert out["default_trace"]["error"] == "absent"
    assert Path(out["trace"]["saved"]).read_bytes() == files[TRACE]
    # the default path is the fork's HWC_TRACE_PATH
    assert glidelab_run.DEFAULT_TRACE == r"C:\vcr\glidelab\glidetrace.log"
    # no step log: nothing collected, and it says so
    box = FakeBox({})
    assert _main(monkeypatch, tmp_path, box, ["h", "fill", "--collect", "--save-dir",
                                              str(tmp_path / "c")]) == 1
    assert all(c.startswith("DOWNLOAD ") for c in box.timeline)
    assert "NOTHING COLLECTED" in capsys.readouterr().out
