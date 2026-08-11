"""Regression: a bare `make` must never compile an agent OLDER than the source.

Found 2026-08-11 while updating .124 (retro-3dfx/FINDINGS.md "Fleet auto-update
was dead"). agent/Makefile derives the compiled AGENT_VERSION from

    git tag -l 'v*' --sort=-v:refname | head -1

but the working clone's tags stopped at v1.9.2 while the agent source had
advanced to v1.25.1 — tags 1.10.0..1.25.1 were never created here. So `make`
would have stamped 1.9.2 onto a v1.25.1 binary.

That is not a cosmetic mislabel. Auto-update fires on version **inequality**
(agent/src/autoupdate.c compares its AGENT_VERSION against the share's
retro_agent.exe.ver and pulls on any mismatch), so publishing that build would
have pushed 1.25.1-source-as-1.9.2 to the whole fleet and then let every box
flip-flop. The fix was to create the missing tag; this test keeps it fixed.

Invariant: the version the Makefile would derive is >= the highest version
number mentioned in any commit message touching agent/.
"""

import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAKEFILE = REPO / "agent" / "Makefile"

VER_RE = re.compile(r"\bv?(\d+)\.(\d+)\.(\d+)\b")


def _git(*args):
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, text=True, check=False
    ).stdout


def _derived_version():
    """Exactly what agent/Makefile's VERSION line computes."""
    out = _git("tag", "-l", "v*", "--sort=-v:refname")
    tags = [t.strip() for t in out.splitlines() if t.strip()]
    return tags[0].lstrip("v") if tags else None


def _tuple(v):
    m = VER_RE.search(v)
    return tuple(int(g) for g in m.groups()) if m else None


def test_makefile_still_derives_version_from_git_tags():
    """If this line changes shape, the rest of this test is measuring nothing."""
    text = MAKEFILE.read_text()
    assert "git tag -l 'v*' --sort=-v:refname" in text, (
        "agent/Makefile no longer derives VERSION from git tags — update this test "
        "to match however the version is now computed."
    )


def test_derived_version_is_not_behind_the_source():
    derived = _derived_version()
    assert derived, (
        "no v* git tags in this clone, so `make` would stamp VERSION=0.0.0 and "
        "publishing it would downgrade every agent on the fleet. Create the tag "
        "for the current agent version (see agent/ commit messages)."
    )

    # Highest vX.Y.Z named in any commit message that touched agent/.
    subjects = _git("log", "--format=%s", "--", "agent/").splitlines()
    claimed = [t for t in (_tuple(s) for s in subjects if VER_RE.search(s)) if t]
    if not claimed:
        return  # shallow clone / no history to compare against

    newest_claimed = max(claimed)
    got = _tuple(derived)
    assert got >= newest_claimed, (
        f"agent/Makefile would compile AGENT_VERSION={derived}, but agent/ commits "
        f"claim {'.'.join(map(str, newest_claimed))}. A bare `make` here produces a "
        f"binary stamped OLDER than its own source; auto-update compares versions by "
        f"inequality, so publishing it downgrades the fleet. "
        f"Fix: git tag v{'.'.join(map(str, newest_claimed))} <commit>"
    )
