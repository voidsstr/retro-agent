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
  registry READS - no gate, no upload, no launch, and no registry write
  unless --restore-cfg N is given (then only where an AA value was found,
  read back);
- (review 2026-09-27) the Glide on the box is identified (md5, and whether it
  carries RETRO3DFX_SLIAA_GUARD / RETRO3DFX_AA_TRACE) and an AA open is
  refused without the guard - by glidelab_run for an AA --cfg before
  anything is written or launched, and by glidelab.exe itself from
  GR_EXTENSION, for the configuration Glide will REALLY open (--cfg, or its
  environment/registry chain); the splash plugin is kept out of a traced AA
  open (FX_GLIDE_NO_PLUGIN, not only NO_SPLASH); --trace-cfg is the only way
  to the config-space dumps; an AA --cfg written to the registry is put
  back after the session.

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
    for var in ("FX_GLIDE_TRACE_FILE", "FX_GLIDE_TRACE", "FX_GLIDE_NO_SPLASH", "FX_GLIDE_NO_PLUGIN",
                "FX_GLIDE_TRACE_CFG"):
        at = main.index(f'glide_env("{var}"')
        assert at < load, var
    assert "SetEnvironmentVariableA" not in code
    # the splash AND its plugin are turned off for an AA open only - the one
    # Glide will really open (effective_config), not only --cfg's
    ns = main.index('glide_env("FX_GLIDE_NO_SPLASH", "1")')
    assert "if (g_eff_aa) {" in main[ns - 60:ns]
    assert main.index('glide_env("FX_GLIDE_NO_PLUGIN", "1")') < main.index("}", ns)
    assert "aa_config(O.cfg)" not in code
    assert main.index("effective_config();") < main.index("if (O.trace > 0) {")
    # the config dumps only when asked, and never without a trace
    tc = main.index('glide_env("FX_GLIDE_TRACE_CFG", "1")')
    assert "if (O.trace_cfg)" in main[tc - 40:tc]
    assert re.search(r"if \(O\.trace_cfg && O\.trace <= 0\) \{[^}]*return 2;", code)
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
    assert len(fields) == 14 and fields[10] == "2" and fields[12] == "0" and fields[13] == "0"


def test_glidelab_aa_configs_are_glides_own():
    """aa_config() must name exactly the SSTH3_SLI_AA_CONFIGURATION values for
    which Glide's switch sets an AA sample count (fall-through included: case 1
    sets 2 samples and falls into case 0). Since the fork's SLIAA-GUARD
    (2026-09-27) gpci.c hands the setting to h5SliAaConfigEnv() in
    minihwc/h5sliaa.h, and the switch is read there."""
    listed = {int(x) for x in re.findall(r"cfg == (\d+)", _func(GLIDELAB, "static int aa_config(int cfg)"))}
    assert listed == {1, 3, 4, 6, 7, 8}
    if GPCI is None:
        pytest.skip("retro3dfx-glide clone absent - aa_config NOT checked against gpci.c")
    g = GPCI.read_text(encoding="latin-1")
    if 'h5SliAaConfigEnv(GLIDE_GETENV("SSTH3_SLI_AA_CONFIGURATION", 2L)' in g:
        hdr = (GPCI.parents[2] / "minihwc/h5sliaa.h").read_text(encoding="latin-1")
        body = _func(hdr, "h5SliAaConfigEnv(long cfg")
        sw = _blank(body[body.index("switch (cfg)"):])
    else:
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


CLASS_KEY = glidelab_run.vb.GLIDE_KEY_TMPL.format(inst="0003")
HWPROFILE = json.dumps({"video_cards": [{"attached_to_desktop": True, "instance": "0003"}]})
BANSHEE = glidelab_run.GLIDE_SERVICE_KEYS[1]
GUARDED = b"MZ\0 PIXEXT COMBINE TEXFMT RETRO3DFX_PARTIALROW RETRO3DFX_SLIAA_GUARD RETRO3DFX_AA_TRACE \0"
UNGUARDED = b"MZ\0 PIXEXT COMBINE TEXFMT RETRO3DFX_PARTIALROW \0"


class RegBox(FakeBox):
    """FakeBox with a registry (REGREAD / REGWRITE on {(root, key): {name: data}})
    and HWPROFILE; text() raises like v56k_bench.Box on an error status."""

    def __init__(self, files=None, execw="", reg=None, write_fails=None):
        super().__init__(files, execw)
        self.reg = {k: dict(v) for k, v in (reg or {}).items()}
        self.write_fails = write_fails          # None, "silent", or an exception

    async def cmd(self, command, timeout=60.0):
        if command == "HWPROFILE":
            self.timeline.append(command)
            return 0, HWPROFILE
        if command.startswith("REGREAD "):
            self.timeline.append(command)
            _, root, key = command.split(" ", 2)
            if (root, key) not in self.reg:
                return 1, "cannot open key"
            return 0, json.dumps({"values": [{"name": n, "type": "REG_SZ", "data": d}
                                             for n, d in self.reg[(root, key)].items()]})
        if command.startswith("REGWRITE "):
            self.timeline.append(command)
            if isinstance(self.write_fails, Exception):
                raise self.write_fails
            _, root, rest = command.split(" ", 2)
            key, name, _typ, data = rest.rsplit(" ", 3)
            if self.write_fails != "silent":
                self.reg.setdefault((root, key), {})[name] = data
            return 0, "OK"
        return await super().cmd(command, timeout)

    async def text(self, command, timeout=60.0):
        st, out = await self.cmd(command, timeout)
        if st != 0:
            raise glidelab_run.vb.RetroProtocolError(out)
        return out


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
    box = RegBox(files, reg={(("HKLM", CLASS_KEY)): {"SSTH3_SLI_AA_CONFIGURATION": "5"}})
    rc = _main(monkeypatch, tmp_path, box, ["h", "fill", "--collect", "--save-dir", str(tmp_path / "c")])
    assert rc == 0
    # three DOWNLOADs, then registry READS only: no write, upload, launch
    assert box.timeline[:3] == [f"DOWNLOAD {LOG}", f"DOWNLOAD {TRACE}",
                                f"DOWNLOAD {glidelab_run.DEFAULT_TRACE}"]
    assert all(c == "HWPROFILE" or c.startswith("REGREAD ") for c in box.timeline[3:])
    assert not [c for c in box.timeline if c.split()[0] in ("REGWRITE", "UPLOAD", "EXECW", "EXEC")]
    out = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert out["collect"] and out["log"]["last"].startswith("step: grSstWinOpen")
    assert out["trace"]["header"] and "SLI_AA_REQUEST: ExtEscape" in out["trace"]["last"]
    assert out["default_trace"]["error"] == "absent"
    assert Path(out["trace"]["saved"]).read_bytes() == files[TRACE]
    # the default path is the fork's HWC_TRACE_PATH
    assert glidelab_run.DEFAULT_TRACE == r"C:\vcr\glidelab\glidetrace.log"
    # no step log: nothing collected, and it says so
    box = RegBox({})
    assert _main(monkeypatch, tmp_path, box, ["h", "fill", "--collect", "--save-dir",
                                              str(tmp_path / "c")]) == 1
    assert all(c.startswith(("DOWNLOAD ", "REGREAD ")) or c == "HWPROFILE" for c in box.timeline)
    assert "NOTHING COLLECTED" in capsys.readouterr().out


# ---- review 2026-09-27: the Glide on the box, the configuration it opens -------------------


def _glidelab_unit():
    """glidelab.c's configuration and guard helpers, compiled on the host
    against a fake registry."""
    return "\n".join((
        "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
        "typedef void *HKEY; typedef unsigned long DWORD; typedef long LONG;",
        "typedef unsigned char BYTE;",
        "#define ERROR_SUCCESS 0L", "#define KEY_READ 1", "#define REG_SZ 1", "#define REG_DWORD 4",
        "#define _snprintf snprintf",
        "#define HKEY_CURRENT_USER ((HKEY)1)", "#define HKEY_LOCAL_MACHINE ((HKEY)2)",
        "static struct { int cfg; } O = { -1 };",
        "struct fake { HKEY root; const char *key, *name; DWORD type; const char *val; };",
        "static struct fake REG[8]; static int nreg, dev0;",
        "static struct { HKEY root; char key[200]; } OPEN[16]; static int nopen;",
        "static LONG RegOpenKeyExA(HKEY root, const char *key, DWORD o, DWORD a, HKEY *out)",
        "{ int i, ok = dev0 && root == HKEY_LOCAL_MACHINE &&",
        "      !strcmp(key, \"SYSTEM\\\\CurrentControlSet\\\\Services\\\\3dfxvs\\\\Device0\");",
        "  (void) o; (void) a;",
        "  for (i = 0; i < nreg; i++) if (REG[i].root == root && !strcmp(REG[i].key, key)) ok = 1;",
        "  if (!ok) return 2;",
        "  OPEN[nopen].root = root; snprintf(OPEN[nopen].key, 200, \"%s\", key);",
        "  *out = (HKEY)(long)(100 + nopen++); return ERROR_SUCCESS; }",
        "static LONG RegQueryValueExA(HKEY h, const char *name, DWORD *r, DWORD *type, BYTE *buf, DWORD *n)",
        "{ int i, k = (int)(long)h - 100; (void) r;",
        "  for (i = 0; i < nreg; i++)",
        "    if (REG[i].root == OPEN[k].root && !strcmp(REG[i].key, OPEN[k].key) && !strcmp(REG[i].name, name)) {",
        "      *type = REG[i].type; snprintf((char *)buf, *n, \"%s\", REG[i].val);",
        "      *n = (DWORD)strlen(REG[i].val) + 1; return ERROR_SUCCESS; }",
        "  return 2; }",
        "static LONG RegCloseKey(HKEY h) { (void) h; return 0; }",
        _func(GLIDELAB, "static const char *json_esc(char *dst, size_t n, const char *s)"),
        _func(GLIDELAB, "static int aa_config(int cfg)"),
        _func(GLIDELAB, "static int glide_guarded(const char *ext)"),
        re.search(r"#define GLIDE_KEY_3DFXVS[^\n]*\n#define GLIDE_KEY_BANSHEE[^\n]*", GLIDELAB).group(0),
        _func(GLIDELAB, "static int reg_sz(HKEY root, const char *key, const char *name, char *val, DWORD vlen)"),
        _func(GLIDELAB, "static int glide_setting(const char *name, char *val, DWORD vlen, char *src, size_t slen)"),
        "static int g_eff_cfg = 2, g_eff_aa;", "static int g_eff_samples = 1;",
        "static char g_eff_why[400];",
        _func(GLIDELAB, "static void effective_config(void)"),
        "#define BAN \"SYSTEM\\\\CurrentControlSet\\\\Services\\\\banshee\\\\Device0\\\\glide\"",
        "#define DFX \"SYSTEM\\\\CurrentControlSet\\\\Services\\\\3dfxvs\\\\Device0\\\\glide\"",
        "static void add(HKEY r, const char *k, const char *n, DWORD t, const char *v)",
        "{ REG[nreg].root = r; REG[nreg].key = k; REG[nreg].name = n; REG[nreg].type = t;",
        "  REG[nreg++].val = v; }",
        "static void reset(int cfg) { nreg = 0; dev0 = 0; nopen = 0; O.cfg = cfg;",
        "  unsetenv(\"SSTH3_SLI_AA_CONFIGURATION\"); unsetenv(\"FX_GLIDE_AA_SAMPLE\"); }",
        "static void show(const char *tag) { effective_config();",
        "  printf(\"%s=%d %d|%s\\n\", tag, g_eff_cfg, g_eff_aa, g_eff_why); }",
        "int main(void) { char b[64];",
        "  reset(7); show(\"cfg\");",
        "  reset(-1); setenv(\"SSTH3_SLI_AA_CONFIGURATION\", \"3\", 1); show(\"env\");",
        "  reset(-1); add(HKEY_LOCAL_MACHINE, BAN, \"SSTH3_SLI_AA_CONFIGURATION\", REG_SZ, \"7\"); show(\"banshee\");",
        "  printf(\"old_banshee=%d\\n\", aa_config(O.cfg));",
        "  reset(-1); dev0 = 1; add(HKEY_CURRENT_USER, DFX, \"SSTH3_SLI_AA_CONFIGURATION\", REG_SZ, \"1\");",
        "  add(HKEY_LOCAL_MACHINE, BAN, \"SSTH3_SLI_AA_CONFIGURATION\", REG_SZ, \"5\"); show(\"hkcu3dfx\");",
        "  reset(-1); dev0 = 1; add(HKEY_LOCAL_MACHINE, BAN, \"SSTH3_SLI_AA_CONFIGURATION\", REG_SZ, \"7\"); show(\"dev0_skips_banshee\");",
        "  reset(-1); show(\"none\");",
        "  reset(-1); add(HKEY_LOCAL_MACHINE, BAN, \"SSTH3_SLI_AA_CONFIGURATION\", REG_DWORD, \"7\"); show(\"dword\");",
        "  reset(7); setenv(\"FX_GLIDE_AA_SAMPLE\", \"0\", 1); show(\"samples0\");",
        "  reset(5); setenv(\"FX_GLIDE_AA_SAMPLE\", \"4\", 1); show(\"samples4\");",
        "  printf(\"g1=%d g2=%d g3=%d g4=%d\\n\",",
        "    glide_guarded(\" PIXEXT RETRO3DFX_SLIAA_GUARD RETRO3DFX_AA_TRACE \"),",
        "    glide_guarded(\" PIXEXT RETRO3DFX_SLIAA_GUARD_OLD \"), glide_guarded(0),",
        "    glide_guarded(\" PIXEXT COMBINE TEXFMT RETRO3DFX_PARTIALROW \"));",
        "  printf(\"esc=%s\\n\", json_esc(b, sizeof b, \"C:\\\\a\\\"b\"));",
        "  return 0; }"))


def test_glidelab_knows_the_configuration_glide_will_really_open(tmp_path):
    """Without --cfg glidelab used aa_config(-1) - never AA - while Glide opened
    whatever its environment / registry held: a traced AA open kept its splash
    plugin, and nothing could refuse it. effective_config() follows our Glide's
    own chain (hwcGetenv + getRegPath, NT 5.x)."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        pytest.skip("no host C compiler - effective_config NOT run")
    (tmp_path / "e.c").write_text(_glidelab_unit())
    r = subprocess.run([cc, "-Wall", "-Werror", "-o", str(tmp_path / "e"), str(tmp_path / "e.c")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = subprocess.run([str(tmp_path / "e")], capture_output=True, text=True).stdout
    got = dict(ln.split("=", 1) for ln in out.splitlines())
    def cfg(tag):
        v, why = got[tag].split("|", 1)
        return v, why
    assert cfg("cfg") == ("7 1", "cfg 7 from --cfg -> AA")
    assert cfg("env")[0] == "3 1" and "from environment" in cfg("env")[1]
    assert cfg("banshee")[0] == "7 1" and "HKLM\\SYSTEM\\CurrentControlSet\\Services\\banshee" in cfg("banshee")[1]
    assert got["old_banshee"] == "0"                 # the old decision: never AA without --cfg
    assert cfg("hkcu3dfx")[0] == "1 1" and cfg("hkcu3dfx")[1].startswith("cfg 1 from HKCU\\") and "3dfxvs" in cfg("hkcu3dfx")[1]
    assert cfg("dev0_skips_banshee")[0] == "2 0"     # 3dfxvs\Device0 exists: banshee is not read
    assert cfg("none") == ("2 0", "cfg 2 from Glide's default -> no AA")
    assert cfg("dword")[0] == "2 0"                  # REG_SZ only, as hwcGetenv
    assert cfg("samples0")[0] == "7 0" and "FX_GLIDE_AA_SAMPLE=0" in cfg("samples0")[1]
    assert cfg("samples4")[0] == "5 1"
    assert got["g1"].split() == ["1", "g2=0", "g3=0", "g4=0"] or \
        (got["g1"] == "1 g2=0 g3=0 g4=0")
    assert got["esc"] == 'C:\\\\a\\"b'


def test_glidelab_refuses_an_aa_open_on_a_glide_without_the_guard_before_any_window():
    main = _func(GLIDELAB, "int main(int argc, char **argv)")
    code = _blank(main)
    init = code.index("p_grGlideInit();")
    ext = code.index("p_grGetString(GR_EXTENSION)")
    chk = code.index("if (g_eff_aa && !guarded) {")
    win = code.index("hwnd = make_window(O.w, O.h);")
    assert init < ext < chk < win < code.index("open_board(")
    blk = main[main.index("if (g_eff_aa && !guarded) {"):main.index("hwnd = make_window(O.w, O.h);")]
    assert "RESULT" in blk and "RETRO3DFX_SLIAA_GUARD" in blk and '\\"unguarded_glide\\":true' in blk
    assert _blank(blk).index("shutdown_glide()") < _blank(blk).index("return 13;")
    assert "json_esc(jd, sizeof jd, O.dll)" in blk     # a path in JSON is escaped
    # the marker glidelab looks for is the fork's
    assert glidelab_run.GUARD_MARKER.decode() in _func(GLIDELAB, "static int glide_guarded(const char *ext)")


def test_the_host_aa_set_is_glidelabs():
    listed = {int(x) for x in re.findall(r"cfg == (\d+)", _func(GLIDELAB, "static int aa_config(int cfg)"))}
    assert glidelab_run.AA_CFGS == listed
    assert not set(glidelab_run.SAFE_CFGS) & listed
    for v, aa in (("7", True), (" 3 ", True), ("5", False), ("0", False), ("", False),
                  (None, False), ("x", False)):
        assert glidelab_run.is_aa(v) is aa, v
    rv = glidelab_run.restore_value
    assert rv("5", 5) == "5" and rv("0", 5) == "0" and rv("2", 0) == "2"
    assert rv("7", 5) == "5" and rv(None, 5) == "5" and rv("", 0) == "0" and rv("junk", 5) == "5"


def _aa_argv(tmp_path, *extra):
    return ["h", "fill", "--res", "1024x768", "--refresh", "85",
            "--save-dir", str(tmp_path / "s")] + list(extra)


@pytest.mark.parametrize("dll", (UNGUARDED, None), ids=("unguarded", "unreadable"))
def test_an_aa_cfg_is_refused_on_an_unguarded_glide_before_anything_is_written(
        monkeypatch, tmp_path, capsys, dll):
    box = RegBox({glidelab_run.OUR_GLIDE: dll, LOG: GOOD_LOG},
                 reg={("HKLM", CLASS_KEY): {"SSTH3_SLI_AA_CONFIGURATION": "5"}})
    assert _main(monkeypatch, tmp_path, box, _aa_argv(tmp_path, "--cfg", "7")) == 2
    assert box.timeline == [f"DOWNLOAD {glidelab_run.OUR_GLIDE}"]
    assert box.reg[("HKLM", CLASS_KEY)]["SSTH3_SLI_AA_CONFIGURATION"] == "5"
    out = capsys.readouterr().out
    assert "REFUSED" in out.upper() and "SLIAA_GUARD" in out


def test_a_non_aa_cfg_still_runs_on_any_glide_and_the_plan_names_the_dll(monkeypatch, tmp_path, capsys):
    box = RegBox({glidelab_run.OUR_GLIDE: UNGUARDED, LOG: UNTRACED_LOG},
                 reg={("HKLM", CLASS_KEY): {"SSTH3_SLI_AA_CONFIGURATION": "2"}})
    assert _main(monkeypatch, tmp_path, box, _aa_argv(tmp_path, "--cfg", "5")) == 0
    out = capsys.readouterr().out
    import hashlib
    assert hashlib.md5(UNGUARDED).hexdigest() in out and "SLIAA-GUARD NO" in out
    # a non-AA cfg is not put back: nothing but its own REGWRITE
    assert [c for c in box.timeline if c.startswith("REGWRITE")] == \
        [f"REGWRITE HKLM {CLASS_KEY} SSTH3_SLI_AA_CONFIGURATION REG_SZ 5"]
    assert "aa_restore" not in out


@pytest.mark.parametrize("before,extra,after", (("5", (), "5"), ("0", (), "0"), ("3", (), "5"),
                                                (None, (), "5"), ("3", ("--restore-cfg", "2"), "2")),
                         ids=("was-5", "was-0", "was-aa", "was-absent", "restore-cfg"))
def test_an_aa_cfg_is_put_back_after_the_session(monkeypatch, tmp_path, capsys, before, extra, after):
    """`--cfg 7` is a REGWRITE that survives every reboot: after the session
    the value goes back - to what it was when that was not AA."""
    reg = {("HKLM", CLASS_KEY): {"SSTH3_SLI_AA_CONFIGURATION": before}} if before else {}
    box = RegBox({glidelab_run.OUR_GLIDE: GUARDED, LOG: GOOD_LOG}, reg=reg)
    assert _main(monkeypatch, tmp_path, box, _aa_argv(tmp_path, "--cfg", "7", *extra)) == 0
    writes = [i for i, c in enumerate(box.timeline) if c.startswith("REGWRITE")]
    assert len(writes) == 2 and writes[0] < box.at("EXECW") < writes[1]
    assert box.timeline[writes[1]].endswith(f"REG_SZ {after}")
    assert box.reg[("HKLM", CLASS_KEY)]["SSTH3_SLI_AA_CONFIGURATION"] == after
    rep = [json.loads(ln) for ln in capsys.readouterr().out.splitlines() if '"aa_restore"' in ln]
    assert rep and rep[0]["aa_restore"]["ok"] and rep[0]["aa_restore"]["before"] == before


@pytest.mark.parametrize("fails", ("silent", OSError("box gone")), ids=("no-readback", "dead"))
def test_an_aa_cfg_that_cannot_be_put_back_fails_the_run_loudly(monkeypatch, tmp_path, capsys, fails):
    box = RegBox({glidelab_run.OUR_GLIDE: GUARDED, LOG: GOOD_LOG},
                 reg={("HKLM", CLASS_KEY): {"SSTH3_SLI_AA_CONFIGURATION": "5"}})

    real = box.cmd

    async def cmd(command, timeout=60.0):
        # the first REGWRITE (the --cfg) lands; the put-back fails
        if command.startswith("REGWRITE") and any(c.startswith("EXECW") for c in box.timeline):
            box.write_fails = fails
        return await real(command, timeout)
    box.cmd = cmd
    assert _main(monkeypatch, tmp_path, box, _aa_argv(tmp_path, "--cfg", "7")) == 1
    out = capsys.readouterr().out
    assert "AA CONFIGURATION LEFT ARMED" in out and "--collect --restore-cfg 5" in out


def test_trace_cfg_is_passed_only_with_a_trace(tmp_path):
    a, code = _parsed(["h", "fill", "--trace-cfg"])
    assert code == 2 and a is None
    a = _args(tmp_path, "--trace", "1", "--trace-cfg")
    box = FakeBox({LOG: GOOD_LOG, TRACE: HEADER})
    asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert "--trace 1 --trace-cfg" in box.timeline[box.at("EXECW")]
    a = _args(tmp_path, "--trace", "1")
    box = FakeBox({LOG: GOOD_LOG, TRACE: HEADER})
    asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert "--trace-cfg" not in box.timeline[box.at("EXECW")]
    # glidelab.exe refuses it without a trace too, before anything loads
    main = _func(GLIDELAB, "int main(int argc, char **argv)")
    assert '!strcmp(a, "--trace-cfg")' in main
    assert main.index("if (O.trace_cfg && O.trace <= 0) {") < main.index("LoadLibraryA(O.dll)")


def test_collect_banners_an_armed_aa_value_and_restores_only_when_asked(monkeypatch, tmp_path, capsys):
    reg = {("HKLM", CLASS_KEY): {"SSTH3_SLI_AA_CONFIGURATION": "7"},
           ("HKLM", BANSHEE): {"SSTH3_SLI_AA_CONFIGURATION": "3"},
           ("HKCU", BANSHEE): {"SSTH3_SLI_AA_CONFIGURATION": "5"}}
    files = {LOG: b"step: grSstWinOpen\n"}
    box = RegBox(files, reg=reg)
    rc = _main(monkeypatch, tmp_path, box, ["h", "fill", "--collect", "--save-dir", str(tmp_path / "c")])
    assert rc == 1
    text = capsys.readouterr().out
    assert "AA CONFIGURATION ARMED" in text and "--collect --restore-cfg 5" in text
    out = json.loads([ln for ln in text.splitlines() if ln.startswith("{")][-1])
    armed = {(r["root"], r["key"]) for r in out["registry"] if r["aa"]}
    assert armed == {("HKLM", CLASS_KEY), ("HKLM", BANSHEE)}
    assert not [c for c in box.timeline if c.startswith("REGWRITE")]
    # every place a Glide reads it from was looked at
    for root, key in [("HKLM", CLASS_KEY)] + [(r, k) for k in glidelab_run.GLIDE_SERVICE_KEYS
                                              for r in ("HKCU", "HKLM")]:
        assert f"REGREAD {root} {key}" in box.timeline
    # --restore-cfg 5: written where it was AA, and read back
    box = RegBox(files, reg=reg)
    rc = _main(monkeypatch, tmp_path, box, ["h", "fill", "--collect", "--restore-cfg", "5",
                                            "--save-dir", str(tmp_path / "c")])
    assert rc == 0
    assert sorted(c for c in box.timeline if c.startswith("REGWRITE")) == sorted(
        f"REGWRITE HKLM {k} SSTH3_SLI_AA_CONFIGURATION REG_SZ 5" for k in (CLASS_KEY, BANSHEE))
    assert box.reg[("HKCU", BANSHEE)]["SSTH3_SLI_AA_CONFIGURATION"] == "5"     # untouched
    out = json.loads([ln for ln in capsys.readouterr().out.splitlines() if ln.startswith("{")][-1])
    assert all(r["ok"] for r in out["restored"])
    # an AA value is not something --restore-cfg may write
    a, code = _parsed(["h", "fill", "--collect", "--restore-cfg", "7"])
    assert code == 2 and a is None



# ---- our Glide's other opt-ins: RETRO_GLIDE_AA_LFB_READ and RETRO_GLIDE_MAPLOG --------
# (2026-09-27, a tool gap from the v56k plan's supervised checklist). Our h5
# Glide reads both with a plain getenv - the process environment only, never
# the registry - so glidelab.exe sets them with glide_env before the DLL
# loads. RETRO_GLIDE_AA_LFB_READ=1 re-enables the multi-chip AA LFB read that
# froze .124 on cfg 3, so it takes --i-am-at-the-box, in glidelab_run.py AND
# in glidelab.exe, and without the flag an inherited value is overridden.

MAPLOG = LOG + ".maplog"
MAPLOG_DATA = (b"pid=1212 chips=4 realChips=4\n"
               b"  chip 0 base0 0xd0000000 state=MEM_COMMIT protect=0x204\n")


def test_glidelab_sets_our_glides_opt_ins_in_its_environment_never_the_registry():
    main = _func(GLIDELAB, "int main(int argc, char **argv)")
    code = _blank(main)
    load = main.index("LoadLibraryA(O.dll)")
    # (--maplog's parse is pinned by test_glidelab_maplog_without_a_path_is_refused_not_dropped)
    for flag, dst in (('"--aa-lfb-read"', "g_aa_lfb_read = 1;"), ('"--i-am-at-the-box"', "g_at_box = 1;")):
        assert re.search(r"!strcmp\(a, " + re.escape(flag) + r"\)[^;]*\) \{? ?" + re.escape(dst.split(";")[0]),
                         main), flag
    # refused without the confirmation - rc 2, before the DLL loads or any window
    refuse = main.index('aa_lfb_read_env(g_aa_lfb_read, g_at_box, getenv("RETRO_GLIDE_AA_LFB_READ"), '
                        '&lfb_env) < 0')
    assert refuse < load < main.index("hwnd = make_window(")
    assert re.search(r"&lfb_env\) < 0\) \{[^}]*return 2;", code)
    assert "--i-am-at-the-box: a multi-chip AA LFB read froze .124 (cfg 3)" in main
    # both through glide_env (msvcrt's copy, which Glide's getenv reads), before the load
    for var, val in (("RETRO_GLIDE_AA_LFB_READ", "lfb_env"), ("RETRO_GLIDE_MAPLOG", "g_maplog")):
        assert main.index(f'glide_env("{var}", {val})') < load, var
    # the fork appends to the mapping log: the old one goes first
    assert main.index("DeleteFileA(g_maplog);") < main.index('glide_env("RETRO_GLIDE_MAPLOG"')
    # never the registry: glidelab.c writes no key at all
    for w in ("RegSetValue", "RegCreateKey", "RegDeleteValue", "RegDeleteKey", "SHSetValue"):
        assert w not in _blank(GLIDELAB), w
    # the RESULT says when AA LFB reads were let through
    assert ',\\"aa_lfb_read\\":1' in _func(GLIDELAB, "static const char *tail_json(void)")
    # the pass-throughs live outside O, whose layout the tests above pin
    assert "static int         g_aa_lfb_read, g_at_box;" in GLIDELAB


def test_glidelab_aa_lfb_read_decision_runs():
    """aa_lfb_read_env, compiled from glidelab.c and run: "1" only when asked
    AND confirmed; asked without the confirmation is refused; not asked, an
    inherited value is overridden with "0" (the fork honours exactly "1")."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        pytest.skip("no host C compiler - aa_lfb_read_env was NOT run")
    import tempfile
    body = "\n".join((
        "#include <stdio.h>",
        _func(GLIDELAB, "static int aa_lfb_read_env(int asked, int at_box, const char *inherited, "
                        "const char **value)"),
        "static void t(int a, int b, const char *in) { const char *v = (const char *)1;",
        '  int rc = aa_lfb_read_env(a, b, in, &v);',
        '  printf("%d%d%s=%d:%s\\n", a, b, in ? in : "N", rc, v ? v : "NULL"); }',
        "int main(void) {",
        '  t(0,0,0); t(0,0,"1"); t(0,0,""); t(0,1,"1"); t(0,1,0);',
        '  t(1,0,0); t(1,0,"1"); t(1,1,0); t(1,1,"0"); t(1,1,"1");',
        "  return 0; }"))
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "l.c").write_text(body)
        r = subprocess.run([cc, "-Wall", "-Werror", "-o", str(Path(d) / "l"), str(Path(d) / "l.c")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        out = subprocess.run([str(Path(d) / "l")], capture_output=True, text=True).stdout
    got = dict(ln.split("=", 1) for ln in out.splitlines())
    assert got == {"00N": "0:NULL", "001": "0:0", "00": "0:NULL", "011": "0:0", "01N": "0:NULL",
                   "10N": "-1:NULL", "101": "-1:NULL", "11N": "0:1", "110": "0:1", "111": "0:1"}


def test_aa_lfb_read_needs_the_confirmation_before_anything_connects(monkeypatch, tmp_path, capsys):
    a, code = _parsed(["h", "fill", "--aa-lfb-read"])
    assert code == 2 and a is None                      # argparse: refused
    a = _args(tmp_path, "--aa-lfb-read", "--i-am-at-the-box")
    assert a.aa_lfb_read and a.i_am_at_the_box and not a.maplog
    # a caller's own namespace without the confirmation: nothing is launched
    a.i_am_at_the_box = False
    box = FakeBox({LOG: GOOD_LOG})
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert res["refused"] and "--i-am-at-the-box" in res["error"] and box.timeline == []
    # nor through main_async: refused before the box is asked anything
    a, code = _parsed(["h", "fill", "--aa-lfb-read", "--i-am-at-the-box"])
    box = FakeBox({LOG: GOOD_LOG})
    a.i_am_at_the_box = False
    monkeypatch.setattr(glidelab_run.vb, "Box", lambda ip: box)
    assert asyncio.run(glidelab_run.main_async(a)) == 2 and box.timeline == []


def test_aa_lfb_read_is_passed_to_glidelab_only_when_confirmed(monkeypatch, tmp_path, capsys):
    a = _args(tmp_path, "--aa-lfb-read", "--i-am-at-the-box")
    box = FakeBox({LOG: GOOD_LOG})
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    assert "error" not in res
    assert "--aa-lfb-read --i-am-at-the-box" in box.timeline[box.at("EXECW")]
    a = _args(tmp_path)
    box = FakeBox({LOG: GOOD_LOG})
    asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    ex = box.timeline[box.at("EXECW")]
    assert "--aa-lfb-read" not in ex and "--i-am-at-the-box" not in ex and "--maplog" not in ex
    # the plan says so, loudly
    assert _main(monkeypatch, tmp_path, FakeBox({LOG: GOOD_LOG}),
                 _aa_argv(tmp_path, "--aa-lfb-read", "--i-am-at-the-box")) == 0
    assert "AA LFB READS ALLOWED" in capsys.readouterr().out


def test_a_maplog_session_deletes_the_old_log_and_brings_the_new_one_home(tmp_path):
    a = _args(tmp_path, "--maplog")
    box = FakeBox({LOG: GOOD_LOG, MAPLOG: MAPLOG_DATA})
    res = asyncio.run(glidelab_run.run_mode(box, a, "fill"))
    ex = box.at("EXECW")
    assert f"--maplog {MAPLOG}" in box.timeline[ex]
    assert box.at(f'del /f /q "{MAPLOG}"') < ex < box.at(f"DOWNLOAD {MAPLOG}")
    m = res["maplog_file"]
    assert "maplog_error" not in res and "error" not in res
    assert m["passes"] == 1 and m["lines"] == 2 and m["last"].startswith("  chip 0 base0")
    assert Path(m["saved"]).read_bytes() == MAPLOG_DATA and Path(m["saved"]).parent == tmp_path / "saved"
    assert glidelab_run.maplog_path(LOG) == MAPLOG


@pytest.mark.parametrize("data", (None, b"", b"not a mapping log\n", OSError("dropped"),
                                  b"pid=12 GLIDE FATAL: grSstWinOpen failed\n"),
                         ids=("absent", "empty", "no-pid-line", "download-raised",
                              "fatal-only-is-not-a-mapping-pass"))
def test_a_maplog_asked_for_and_not_produced_fails_the_run(monkeypatch, tmp_path, data):
    a = _args(tmp_path, "--maplog")
    res = asyncio.run(glidelab_run.run_mode(FakeBox({LOG: GOOD_LOG, MAPLOG: data}), a, "fill"))
    assert res["maplog_error"] and "error" not in res
    argv = ["h", "fill", "--res", "1024x768", "--refresh", "85", "--maplog",
            "--save-dir", str(tmp_path / "s")]
    assert _main(monkeypatch, tmp_path, FakeBox({LOG: GOOD_LOG, MAPLOG: data}), argv) == 1
    assert _main(monkeypatch, tmp_path, FakeBox({LOG: GOOD_LOG, MAPLOG: MAPLOG_DATA}), argv) == 0


def test_glidelab_maplog_without_a_path_is_refused_not_dropped():
    """`--maplog` as the last argument, or followed by another option, used to
    be skipped silently (the run went ahead with no map log); it now reaches
    the 'needs a file path' refusal (review 2026-09-27)."""
    src = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "vcr-kmd" / "tools"
           / "glidelab.c").read_text()
    assert "g_maplog = (v && v[0] != '-') ? v : \"\";" in src
    assert '!strcmp(a, "--maplog") && v)' not in src
    assert src.index("g_maplog = (v && v[0] != '-')") < src.index("--maplog needs a file path")
