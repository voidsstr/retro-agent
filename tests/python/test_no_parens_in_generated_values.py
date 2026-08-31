"""A `)` inside an expanded .bat value closes its enclosing block.

THIRD OCCURRENCE of this character class, and the third time it found a place
the rule did not cover. The rule said *filename*, so the defect moved into a
**value**: `SoldierOfFortune2`'s generated launcher set

    set "GTITLE=Soldier of Fortune II: Double Helix Gold (single player)"

then echoed `[%GTITLE%]` **inside an `if ( ... )` block**. cmd expands variables
while PARSING the block, so the `)` closed it early, cmd aborted with `] was
unexpected at this time`, and the launcher started nothing -- **on every box**
-- while the compatibility matrix recorded the title as `runs`.

The two earlier occurrences: a launcher *filename* containing `(LAN)`, which
could not be launched through the agent but worked when a person double-clicked
it; and generated `onboard.cmd` game names like `(BC Romania)`, which closed an
`if` block and left onboarding silently unfinished.

WHAT THIS TEST CHECKS, AND WHY NOT THE SPECS. A first draft flagged spec fields
by NAME and produced a false positive immediately -- JediAcademy's spec carries
a display label `Jedi Academy (single player)` that never becomes a cmd
variable; its shipped launcher sets `GTITLE=Jedi Academy`. Guessing which field
reaches a value is unreliable, and a check that cries wolf gets ignored.

So this asserts the ACTUAL dangerous condition on the GENERATED output: a
variable whose value contains a paren, expanded **unquoted**, **inside a
block**. Quoting neutralises it, which is why `"%IMAGE%"` is fine and
`echo image=%IMAGE%` was not -- that one was real, latent in System Shock 2's
mount-failure path, and is fixed in the shared template.
"""
import glob
import os
import re

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIB = "/mnt/retro-share/Files/Games-Library"
TEMPLATE = os.path.join(REPO, "provisioning", "discmount",
                        "mount-launcher-template.bat")

SET_WITH_PAREN = re.compile(r'set\s+"([A-Za-z_]\w*)=[^"]*[()][^"]*"')


def _risky_lines(text):
    """Unquoted expansions of paren-bearing vars at block depth > 0."""
    paren_vars = {m.group(1) for m in SET_WITH_PAREN.finditer(text)}
    out, depth = [], 0
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if depth > 0 and not s.lower().startswith("rem"):
            for v in paren_vars:
                tok = "%" + v + "%"
                if tok not in line:
                    continue
                # A var inside an ENCLOSING quoted string is safe: cmd's block
                # parser respects quotes when matching `)`. Only a bare,
                # unquoted expansion can close the block. Getting this wrong in
                # the other direction matters -- a first draft flagged
                # `call :fail "... %IMAGE% ..."`, and "fixing" it by adding
                # inner quotes produced nested quotes and broke the argument.
                before = line[:line.index(tok)]
                if before.count('"') % 2 == 1:      # we are inside quotes
                    continue
                if '"' + tok + '"' in line:         # explicitly quoted
                    continue
                out.append((n, v, s[:70]))
        depth += line.count("(") - line.count(")")
        if depth < 0:
            depth = 0
    return out


def test_the_shared_template_has_no_unquoted_paren_expansion_in_a_block():
    if not os.path.exists(TEMPLATE):
        pytest.skip("SKIPPED LOUDLY: %s absent - not checked" % TEMPLATE)
    with open(TEMPLATE, encoding="utf-8", errors="replace") as f:
        text = f.read()
    # The template's own IMAGE is a placeholder, so seed the real hazard: the
    # per-title value it is substituted with really does carry "(USA)".
    seeded = text.replace('set "IMAGE=', 'set "IMAGE=x (USA) ', 1)
    risky = _risky_lines(seeded)
    assert not risky, (
        "the shared mount template expands a paren-bearing value UNQUOTED "
        "inside a block:\n  %s\n\ncmd expands while parsing the block, so the "
        "`)` closes it and the launcher dies with `] was unexpected at this "
        "time` having started nothing. Quote it."
        % "\n  ".join("line %d: %%%s%% -- %s" % r for r in risky))


def test_no_shipped_launcher_has_the_hazard():
    if not os.path.isdir(LIB):
        pytest.skip("SKIPPED LOUDLY: %s not mounted - shipped launchers NOT "
                    "checked" % LIB)
    hits = []
    for bat in sorted(glob.glob(os.path.join(LIB, "*", "*.bat"))):
        try:
            with open(bat, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for n, v, s in _risky_lines(text):
            hits.append("%s:%d  %%%s%%  %s"
                        % (os.path.relpath(bat, LIB), n, v, s))
    assert not hits, (
        "these SHIPPED launchers expand a paren-bearing value unquoted inside "
        "a block:\n  %s\n\nFix the GENERATOR, not the share -- a share-only "
        "edit is regenerated away." % "\n  ".join(hits))


def test_the_detector_would_catch_the_string_that_actually_shipped():
    """Guard the checker against being weakened into uselessness."""
    sample = ('set "GTITLE=Soldier of Fortune II: Double Helix Gold '
              '(single player)"\nif exist x (\n    echo [%GTITLE%] hi\n)\n')
    assert _risky_lines(sample), (
        "the exact construct that broke every box would now pass -- the "
        "detector has been weakened")


def test_quoting_is_recognised_as_the_fix():
    """`"%IMAGE%"` must NOT be flagged, or the check cries wolf and gets
    ignored -- which is how the real one gets missed."""
    safe = 'set "IMAGE=disc (USA).cue"\nif exist x (\n    mount "%IMAGE%"\n)\n'
    assert not _risky_lines(safe), (
        "a correctly QUOTED expansion is being reported; this check would then "
        "fire on every disc title and be switched off")
