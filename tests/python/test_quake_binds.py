"""id-engine autoexec.cfg: comment semicolons, the Quake II unbindall, fleetkey.cfg.

MEASURED (R5, .243's Quake II console at every start): a semicolon inside a //
comment runs the rest of the line as a command - `Can't "cmd", not connected`
from "invuse is its action key", `Unknown command "nobody"` from "nobody wants
1997 walk speed". Cbuf_Execute splits a line at every ';' outside quotes BEFORE
the tokenizer sees the '//'.

And (R4 E8) the staged Quake II block opened with `unbindall`, which runs AFTER
config.cfg and rebinds only about two dozen keys - weapon keys 1-0, [ ], ENTER,
the arrows and F1-F4 were unbound on every launch.

The library now carries `exec fleetkey.cfg` - the per-box layout the retro agent
will write, never shipped, so GAMESYNC never fights it - right before
`exec fleetres.cfg` (last live line in Quake 1). These tests run the configs
through a small model of the engine's command buffer, so they assert what the
ENGINE does with the text, not merely what the text says.
"""
import importlib.util
import os
import struct
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sf = _load("stage_fleetres_qb", os.path.join(REPO, "scripts", "fleet", "stage-fleetres.py"))
vl = _load("validate_staged_library_qb", os.path.join(REPO, "scripts", "validate-staged-library.py"))

LIB = "/mnt/retro-share/Files/Games-Library"
_share = pytest.mark.skipif(not os.path.isdir(LIB),
                            reason="staged library not mounted at %s - the share "
                                   "side was NOT checked" % LIB)


# --- a model of Quake's command buffer -------------------------------------------

def _commands(line):
    """Cbuf_Execute: a line splits at every ';' outside double quotes."""
    out, cur, q = [], [], 0
    for c in line:
        if c == '"':
            q += 1
        if c == ";" and q % 2 == 0:
            out.append("".join(cur))
            cur = []
            continue
        cur.append(c)
    out.append("".join(cur))
    return out


def _tokens(cmd):
    """COM_Parse: whitespace-separated, quoted strings whole, // ends the line."""
    toks, i = [], 0
    while i < len(cmd):
        c = cmd[i]
        if c.isspace():
            i += 1
        elif cmd[i:i + 2] == "//":
            break
        elif c == '"':
            j = cmd.find('"', i + 1)
            j = len(cmd) if j < 0 else j
            toks.append(cmd[i + 1:j])
            i = j + 1
        else:
            j = i
            while j < len(cmd) and not cmd[j].isspace():
                j += 1
            toks.append(cmd[i:j])
            i = j
    return toks


def run_cfg(text, binds, ran):
    for line in text.replace("\r\n", "\n").split("\n"):
        for cmd in _commands(line):
            t = _tokens(cmd)
            if not t:
                continue
            verb = t[0].lower()
            if verb == "unbindall":
                binds.clear()
            elif verb == "bind" and len(t) >= 3:
                binds[t[1].lower()] = t[2]
            else:
                ran.append(verb)


# Quake II's own default.cfg opens with unbindall and binds the weapon keys
# (pak1.pak, 3,078 B - the share test below runs the real one).
DEFAULT_Q2 = """unbindall
bind 1 "use Blaster"
bind 2 "use Shotgun"
bind 0 "use BFG10K"
bind [ "invprev"
bind ] "invnext"
bind ENTER "invuse"
bind UPARROW "+forward"
bind a "+lookup"
"""

# The staged block, as it was on the share until 2026-10-01 (shape, not bytes).
STAGED_Q2 = """// autoexec.cfg - fleet defaults for Quake II
// ---- movement: WASD, plus F ------------------------------------------------
unbindall
bind w "+forward"
bind s "+back"
bind a "+moveleft"
bind d "+moveright"
bind f "invuse"                 // Quake II has no +use; invuse is its action key
// ---- free look -------------------------------------------------------------
// Quake II has no `freelook` cvar - +mlook IS the mechanism, and putting it in
// autoexec is what makes it permanent rather than a key you have to hold.
+mlook
set cl_run "1"                  // always run; nobody wants 1997 walk speed
bind F11 "screenshot"

// ---- per-box resolution ---------------------------------------
// fleetres.cfg is written by this title's Play ....bat at every
// launch, from the panel FLEETRES.EXE measures on THIS machine.
exec fleetres.cfg
"""


def _live(text):
    return [sf._live(l) for l in text.split("\n") if sf._live(l)]


def test_the_old_block_wiped_the_weapon_keys_and_ran_its_comments():
    binds, ran = {}, []
    run_cfg(DEFAULT_Q2, binds, ran)
    run_cfg(STAGED_Q2, binds, ran)
    assert "1" not in binds and "[" not in binds and "enter" not in binds
    assert "invuse" in ran and "nobody" in ran, ran


def test_the_new_block_keeps_them_and_runs_no_prose():
    new = sf.fleetkey_autoexec(STAGED_Q2)
    binds, ran = {}, []
    run_cfg(DEFAULT_Q2, binds, ran)
    run_cfg(new, binds, ran)
    assert binds["1"] == "use Blaster" and binds["0"] == "use BFG10K"
    assert binds["["] == "invprev" and binds["enter"] == "invuse"
    assert binds["w"] == "+forward" and binds["a"] == "+moveleft", "WASD still applies"
    assert "nobody" not in ran and "invuse" not in ran, ran
    assert ran[-2:] == ["exec", "exec"]


def test_fleetkey_goes_right_before_fleetres_and_is_idempotent():
    new = sf.fleetkey_autoexec(STAGED_Q2)
    live = _live(new)
    assert live[-2:] == ["exec fleetkey.cfg", "exec fleetres.cfg"]
    assert "unbindall" not in [l.lower() for l in live]
    assert not any(sf.comment_semicolons(l) for l in new.split("\n"))
    assert "Quake II has no `freelook` cvar" not in new and "freelook" in new
    assert sf.fleetkey_autoexec(new) == new
    # the fleetres.cfg comment block stays whole, after the fleetkey block
    assert new.index("// ---- per-box key binds") < new.index("// ---- per-box resolution")
    for line in sf.FLEETKEY_BLOCK + sf.UNBINDALL_NOTE:
        assert ";" not in line and line.count('"') % 2 == 0, line


def test_quake1_gets_fleetkey_as_its_last_live_line():
    q1 = 'bind F11 "screenshot"\n\n// NO SEMICOLONS IN COMMENTS\ngamma 0.7\n'
    new = sf.fleetkey_autoexec(q1)
    assert _live(new)[-1] == "exec fleetkey.cfg"
    assert _live(new)[:2] == ['bind F11 "screenshot"', "gamma 0.7"]
    assert sf.fleetkey_autoexec(new) == new


def test_a_comment_semicolon_in_quotes_or_code_is_left_alone():
    assert not sf.comment_semicolons('bind F9 "echo a;wait;load quick"')
    assert not sf.comment_semicolons('// see "a;b" here')
    assert sf.comment_semicolons('// a; b')
    assert sf.comment_semicolons('set x "1" // a; b')
    assert sf.fix_comment_semis('set x "1" // a; b\n') == 'set x "1" // a, b\n'
    # the awk example in SiN's autoexec is rewritten, not broken
    old, new = sf.SIN_AWK
    assert sf.comment_semicolons(old) and not sf.comment_semicolons(new)
    assert sf.fix_comment_semis(old + "\n") == new + "\n"


@pytest.mark.parametrize("line", [
    'bind f "invuse" // Quake II has no +use; invuse is its action key',
    '// a "quoted; thing" then; more', '// fine', 'bind x "a;b"', '// x; y "z',
    'say "unclosed // ; still quoted',
])
def test_the_validator_and_the_fixer_agree(line):
    assert vl.comment_semicolon(line) == sf.comment_semicolons(line), line


# --- the validator fires (a check that never fires is a lie) ---------------------

def _title(tmp_path, files):
    d = tmp_path / "T"
    for rel, body in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(body.encode("latin-1") if isinstance(body, str) else body)
    return str(d)


def test_validator_fails_a_comment_semicolon_in_an_id_engine_title(tmp_path):
    d = _title(tmp_path, {"quake2.exe": b"MZ", "baseq2/autoexec.cfg": STAGED_Q2})
    probs = vl.id_engine_cfg_problems(d)
    assert [p[:2] for p in probs] == [("fail", "config-semicolon")] * 2, probs
    fixed = _title(tmp_path / "x", {"quake2.exe": b"MZ",
                                    "baseq2/autoexec.cfg": sf.fleetkey_autoexec(STAGED_Q2)})
    assert vl.id_engine_cfg_problems(fixed) == []


def test_validator_ignores_another_engine(tmp_path):
    d = _title(tmp_path, {"Shogo.exe": b"MZ", "autoexec.cfg": "// a; b\n"})
    assert vl.id_engine_cfg_problems(d) == []


def test_validator_warns_on_a_staged_game_written_config(tmp_path):
    d = _title(tmp_path, {"quake2.exe": b"MZ",
                          "rogue/config.cfg": "// generated by quake, do not modify\nset x 1\n"})
    probs = vl.id_engine_cfg_problems(d)
    assert [p[:2] for p in probs] == [("warn", "per-box-config")], probs


# --- the recipes cover every id-engine autoexec that had the bug ------------------

def test_the_recipes_name_every_file_that_carried_a_comment_semicolon():
    """The 2026-10-01 scan of the share - 13 autoexecs across 8 titles."""
    covered = set()
    for title, spec in sf.TITLES.items():
        for key in ("cfg_semis", "cfg_fleetkey"):
            covered.update("%s/%s" % (title, rel) for rel in spec.get(key, []))
    assert covered >= {
        "JediAcademy/base/autoexec.cfg", "Quake2Complete/baseq2/autoexec.cfg",
        "Quake2Complete/rogue/autoexec.cfg", "Quake2Complete/xatrix/autoexec.cfg",
        "Quake2Win9x/baseq2/autoexec.cfg", "Quake3-TeamArena/baseq3/autoexec.cfg",
        "Quake3-TeamArena/missionpack/autoexec.cfg", "SiNGold/base/autoexec.cfg",
        "SiNGold/2015/autoexec.cfg", "SiNGold/ctf/autoexec.cfg",
        "SoldierOfFortune/base/autoexec.cfg", "SoldierOfFortune2/base/autoexec.cfg",
        "SoldierOfFortune2/base/mp/autoexec.cfg"}
    assert sf.TITLES["Quake2Win9x"]["payload"] is False, \
        "a Win9x-only title must not be handed FLEETRES.BAT (cmd.exe dialect)"
    assert set(sf.TITLES["Quake2Complete"]["remove"]) == {"rogue/config.cfg",
                                                          "xatrix/config.cfg"}


# --- the share ---------------------------------------------------------------------

def _pak_file(pak, name):
    with open(pak, "rb") as fh:
        head = fh.read(12)
        assert head[:4] == b"PACK"
        off, length = struct.unpack("<II", head[4:])
        fh.seek(off)
        dirb = fh.read(length)
        for i in range(0, length, 64):
            n = dirb[i:i + 56].split(b"\0")[0].decode("latin-1")
            pos, size = struct.unpack("<II", dirb[i + 56:i + 64])
            if n.lower() == name:
                fh.seek(pos)
                return fh.read(size).decode("latin-1")
    raise AssertionError("%s not in %s" % (name, pak))


@_share
@pytest.mark.parametrize("title", ["Quake2Win9x", "Quake2Complete"])
def test_the_real_default_cfg_then_the_staged_autoexec_keeps_the_weapon_keys(title):
    default = _pak_file(os.path.join(LIB, "Quake2Complete", "baseq2", "pak1.pak"),
                        "default.cfg")
    binds, ran = {}, []
    run_cfg(default, binds, ran)
    assert binds.get("1") == "use Blaster", "default.cfg changed shape - re-read it"
    staged = open(os.path.join(LIB, title, "baseq2", "autoexec.cfg"), "rb").read().decode("latin-1")
    run_cfg(staged, binds, ran)
    for key in ("1", "2", "0", "[", "]", "enter"):
        assert key in binds, "%s: key %r was unbound by the staged autoexec" % (title, key)
    assert binds["w"] == "+forward"
    assert _live(staged.replace("\r\n", "\n"))[-2:] == ["exec fleetkey.cfg", "exec fleetres.cfg"]


@_share
def test_the_engine_written_configs_are_no_longer_staged():
    for rel in ("rogue/config.cfg", "xatrix/config.cfg"):
        assert not os.path.exists(os.path.join(LIB, "Quake2Complete", rel)), rel


@_share
def test_quake1_execs_fleetkey_last():
    text = open(os.path.join(LIB, "Quake1", "ID1", "autoexec.cfg"), "rb").read().decode("latin-1")
    assert _live(text.replace("\r\n", "\n"))[-1] == "exec fleetkey.cfg"
