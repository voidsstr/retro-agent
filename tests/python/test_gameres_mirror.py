"""The agent's per-title resolution rules must agree with the staged library.

WHY THIS EXISTS. `agent/shared/gameres.h` carries a second copy of a decision
that already had a home: `scripts/fleet/stage-fleetres.py` generates the launcher
line for each title, and FLEETRES.EXE applies it at launch. The agent's copy
exists because a launcher cannot undo what GAMESYNC writes *after* it ran - a
staged `install.reg` is merged at the end of every sync and Half-Life's re-pins
the shared GoldSrc mode key to 1024x768 on every box. But a second COPY of one
decision is fine only while something forces the two to stay equal; a second
DECISION is how a fleet ends up with two answers and no way to tell which one a
box used.

So every assertion here is of the form "the C table names the same file, the
same key and the same shape of value as the mechanism that was measured on
hardware". It deliberately does NOT re-derive what the right resolution is -
that is `tests/native/test_gameres.c`'s job, against the same header the agent
compiles.

It SKIPS LOUDLY when the share is not mounted, for the reason
tests/test_staged_library.py does: a silent skip lets the library rot unnoticed.
"""

import os
import re
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, "agent", "shared", "gameres.h")
LIB = "/mnt/retro-share/Files/Games-Library"

OPS = ("GR_OP_INI", "GR_OP_SETLINE", "GR_OP_KV", "GR_OP_REG", "GR_OP_CFG")


def _c_string(tok):
    """A C string literal (or NULL) as Python text."""
    tok = tok.strip()
    if tok == "NULL":
        return None
    assert tok.startswith('"') and tok.endswith('"'), tok
    return tok[1:-1].replace('\\\\', '\\').replace('\\"', '"')


def _split_args(body):
    """Split a rule initialiser on commas that are not inside a string."""
    out, cur, in_str, esc = [], "", False, False
    for ch in body:
        if esc:
            cur += ch
            esc = False
            continue
        if ch == "\\":
            cur += ch
            esc = True
            continue
        if ch == '"':
            in_str = not in_str
            cur += ch
            continue
        if ch == "," and not in_str:
            out.append(cur)
            cur = ""
            continue
        cur += ch
    out.append(cur)
    return out


def load_rules():
    """Parse gr_rules[] out of the header the agent compiles.

    Parsed rather than reimplemented on purpose: a hand-kept Python copy of the
    table would be a THIRD place the answer lives, which is the problem this
    file exists to prevent one level up.
    """
    src = open(HEADER, encoding="utf-8").read()
    start = src.index("GR_DATA const gr_rule_t gr_rules[] = {")
    end = src.index("#define GR_RULE_COUNT", start)
    body = src[start:end]
    # Drop comments so a '{' or a quote inside one cannot be read as a rule.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)

    rules = []
    for m in re.finditer(r"\{([^{}]*)\}", body):
        args = _split_args(m.group(1))
        if len(args) < 3:
            continue
        op = args[1].strip()
        assert op in OPS, "unknown op %r" % op
        rules.append({
            "title": _c_string(args[0]),
            "op": op,
            "file": _c_string(args[2]),
            "arg1": _c_string(args[3]) if len(args) > 3 else None,
            "arg2": _c_string(args[4]) if len(args) > 4 else None,
            "arg3": _c_string(args[5]) if len(args) > 5 else None,
        })
    return rules


RULES = load_rules()


def test_the_table_parsed_at_all():
    """A parser that silently reads zero rules would pass every test below."""
    assert len(RULES) > 40, "only %d rules parsed - the parser is broken, not " \
                            "the table" % len(RULES)
    assert len({r["title"] for r in RULES}) > 15


def test_no_rule_owns_a_launcher():
    """Two mechanisms owning one `.bat` has already destroyed a generated
    disc-mount launcher once (stage-fleetres.py's own DISCMOUNT_MARKERS comment
    records it). The agent must never write into a launcher: those belong to
    stage-fleetres.py."""
    for r in RULES:
        if r["op"] == "GR_OP_REG":
            continue
        assert not r["file"].lower().endswith(".bat"), r


def test_registry_rules_declare_root_and_type():
    """A typo in the root or the type prefix makes gr_w_reg() refuse the write,
    and the value is then simply never set - silently, which is the failure
    shape this whole area keeps producing."""
    for r in RULES:
        if r["op"] != "GR_OP_REG":
            continue
        assert r["file"] in ("HKLM", "HKCU"), r
        assert r["arg3"].startswith("dword:") or r["arg3"].startswith("sz:"), r


def test_goldsrc_rule_targets_the_key_install_reg_pins():
    """THE DEFECT THE PASS EXISTS FOR, asserted from both ends.

    HalfLife1/install.reg pins the shared GoldSrc mode key, gs_merge_reg()
    re-applies it on every sync, and there is no Software\\Valve\\CounterStrike
    key at all - so that one value is the mode for every GoldSrc title on the
    box. If the agent's rule ever names a different key, the pass runs, reports
    success and changes nothing."""
    goldsrc = [r for r in RULES
               if r["op"] == "GR_OP_REG"
               and r["arg1"] == "Software\\Valve\\Half-Life\\Settings"]
    assert goldsrc, "no rule covers the shared GoldSrc mode key"

    names = {r["arg2"] for r in goldsrc}
    # BOTH halves. The WON launcher's Screen* and the engine's EngineMode* are
    # two different sets and the engine comes up at its own 400x300 default
    # unless they agree - A/B'd on .133.
    for need in ("ScreenWidth", "ScreenHeight", "EngineModeW", "EngineModeH"):
        assert need in names, "the GoldSrc rule must set %s" % need

    for r in goldsrc:
        if r["arg2"] in ("ScreenWidth", "EngineModeW"):
            assert r["arg3"] == "dword:%W%", \
                "%s must take the panel's own width, not a constant" % r["arg2"]
        if r["arg2"] in ("ScreenHeight", "EngineModeH"):
            assert r["arg3"] == "dword:%H%", r


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted, so the rule table "
                           "was NOT checked against the staged library" % LIB)
def test_every_rule_names_a_real_title():
    """A rule for a title the library does not carry is dead code that reads
    like coverage."""
    have = {d.lower() for d in os.listdir(LIB)
            if os.path.isdir(os.path.join(LIB, d)) and not d.startswith("_")}
    for r in RULES:
        assert r["title"].lower() in have, \
            "%s is not a staged title" % r["title"]


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted" % LIB)
def test_file_rules_name_a_file_the_library_actually_ships():
    """A rule pointing at a path that does not exist writes nothing and says
    nothing, which is indistinguishable from working.

    GR_OP_CFG is exempt from the FILE half - fleetres.cfg is ours and is created
    - but its DIRECTORY must exist, or the agent would grow an empty `xatrix\\`
    on a box the mission pack never reached.
    """
    missing = []
    for r in RULES:
        if r["op"] == "GR_OP_REG":
            continue
        rel = r["file"].replace("\\", "/")
        target = os.path.join(LIB, r["title"], rel)
        if r["op"] == "GR_OP_CFG":
            target = os.path.dirname(target)
        if not os.path.exists(target):
            missing.append("%s: %s" % (r["title"], r["file"]))
    assert not missing, "rules pointing at nothing:\n  " + "\n  ".join(missing)


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted" % LIB)
def test_agent_rules_agree_with_the_staged_launchers():
    """Where a title's launcher already sets the mode through FLEETRES, the
    agent's rule must name the SAME file. Two mechanisms writing two different
    files is how a box ends up with a resolution nobody can account for."""
    disagree = []
    for r in RULES:
        if r["op"] == "GR_OP_REG":
            continue
        tdir = os.path.join(LIB, r["title"])
        bats = [f for f in os.listdir(tdir) if f.lower().endswith(".bat")]
        blob = ""
        for b in bats:
            try:
                blob += open(os.path.join(tdir, b), encoding="utf-8",
                             errors="replace").read()
            except OSError:
                pass
        if "FLEETRES" not in blob:
            continue            # launcher-free title; the agent is the only writer
        base = os.path.basename(r["file"])
        if base.lower() not in blob.lower():
            disagree.append("%s: agent writes %s, no launcher mentions it"
                            % (r["title"], r["file"]))
    assert not disagree, "\n  ".join(disagree)


def test_the_kv_writer_is_handed_a_composed_line():
    """SOURCE ASSERTION for the one defect this pass shipped and then caught.

    `gr_w_line(..., kv=1)` replaces a whole LINE, so the GR_OP_KV branch must
    hand it "key=value". The first version handed it the bare value, which
    replaced `ResolutionX=1024` with `1024`; that no longer parses as key=value,
    so every later pass matched nothing and APPENDED another `1024`. Three runs
    on .191 left Descent 2's DESCENT.CFG with six junk lines and no resolution.

    tests/native/test_gameres.c pins gr_kv_line() itself. This pins that the
    caller USES it - which is the half that was actually wrong, and a test of
    the helper alone would have passed against the broken code.
    """
    src = open(os.path.join(REPO, "agent", "src", "gameres.c"),
               encoding="utf-8").read()
    branch = src[src.index("case GR_OP_KV"):]
    branch = branch[:branch.index("case GR_OP_CFG")]
    assert "gr_kv_line(" in branch, \
        "the GR_OP_KV branch must compose key=value with gr_kv_line()"
    assert "gr_w_line(path, a1, kvline, 1)" in branch, \
        "the composed line, not the bare value, is what gets written"


def _cfg_bodies():
    """The GR_OP_CFG bodies out of gr_cfg_body(), as {kind: [setting lines]}."""
    src = open(HEADER, encoding="utf-8").read()
    fn = src[src.index("GR_FN const char *gr_cfg_body"):]
    fn = fn[:fn.index("\n}\n")]
    out, kind = {}, None
    for m in re.finditer(r'(?:if \(!strcmp\(kind, "([^"]+)"\)\)|return)\s*\n?|'
                         r'"((?:[^"\\]|\\.)*)"', fn):
        if m.group(1):
            kind = m.group(1)
        elif m.group(2) is not None:
            line = m.group(2).replace('\\"', '"').replace("\\n", "")
            if line and not line.startswith("//"):
                out.setdefault(kind, []).append(line)
    return out


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted" % LIB)
def test_a_shared_cfg_is_written_identically_by_both_writers():
    """A file BOTH the agent and the title's launcher write must get the same
    settings from each, or they rewrite one another forever.

    The launcher rebuilds `fleetres.cfg` from scratch at every start (`>` then
    `>>`); the agent writes it at every sync and rewrites it whenever one of
    its settings is missing. If the two disagree by one line - a refresh rate,
    say - every sync reports a change on a box where the game was launched, and
    the "0 value(s) changed" signal that catches real faults is gone.

    So: every setting line the agent writes must appear, with the SAME value,
    among the `echo` lines the launcher writes to the same file. The launcher
    may write MORE (SoF2's single-player launcher adds r_customaspect) - that
    is fine, the agent's containment check tolerates extras. The reverse is not.
    """
    bodies = _cfg_bodies()
    assert bodies, "gr_cfg_body() parsed to nothing - the parser is broken"

    # FR_* is the launcher's spelling of the agent's %TOKEN%.
    tok = {"%W%": "%FR_W%", "%H%": "%FR_H%", "%W43%": "%FR_W43%",
           "%H43%": "%FR_H43%", "%FOV%": "%FR_FOV%", "%Q2MODE%": "%FR_Q2MODE%",
           "%Q3MODE%": "%FR_Q3MODE%", "%FRHZ%": "%FR_HZ%", "%HZ%": "%FR_HZ%"}

    problems = []
    for r in RULES:
        if r["op"] != "GR_OP_CFG":
            continue
        tdir = os.path.join(LIB, r["title"])
        echoes = ""
        for b in os.listdir(tdir):
            if b.lower().endswith(".bat"):
                echoes += open(os.path.join(tdir, b), encoding="utf-8",
                               errors="replace").read()
        base = os.path.basename(r["file"])
        if base not in echoes:
            continue                    # the launcher does not write this file
        for line in bodies.get(r["arg1"], []):
            want = line
            for a, b in tok.items():
                want = want.replace(a, b)
            if want not in echoes:
                problems.append("%s (%s): the agent writes %r but no launcher "
                                "echoes it — the two will rewrite each other "
                                "on every sync" % (r["title"], r["file"], want))
    assert not problems, "\n  " + "\n  ".join(problems)
