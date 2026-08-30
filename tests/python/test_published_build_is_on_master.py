"""Regression: the binary offered to the fleet must be built from master.

Found 2026-08-30, with seven agents working this repo at once. One of them
tagged `v1.72.0`, built, and published to the share -- then did the same again
as `v1.72.1` -- from a commit (`8e10b7f`) that existed only in its own
worktree. `git merge-base --is-ancestor v1.72.1 origin/master` said NO and
`git branch -r --contains v1.72.1` was empty, while the share's
`retro_agent.exe.ver` already read 1.72.1.

WHY THAT IS DANGEROUS, and not merely untidy:

  * **Auto-update fires on version INEQUALITY**, not on "remote is newer"
    (agent/src/autoupdate.c). A second agent had a finished build off
    origin/master -- which by construction did NOT contain the first agent's
    commits. The moment it tagged v1.73.0 and published, every box would have
    pulled a binary with the capability gate and the disk_mb change **silently
    absent**, 1.72.x stepped over, and nothing anywhere pointing at the
    regression. It caught this itself and held; this test is so the next one
    does not have to.
  * **The share is the one artefact a `git revert` cannot roll back.** It is
    the fleet's auto-update source, so a bad publish reaches eight machines on
    their next restart.
  * A binary ahead of master **cannot be rebuilt from source** if the worktree
    that produced it is lost -- and these worktrees are disposable by design.

The rule this encodes: **publish to the share only from a commit already on
`origin/master`.**

Note this is deliberately a check on TAGS, not on the share. The share is not
readable on every dev host, and a test that silently skips when a mount is
missing would let exactly this rot back in -- so the invariant is expressed
against git, which is always present.
"""
import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VER_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def _git(*args):
    p = subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                       text=True, check=False)
    return p.returncode, p.stdout.strip()


def _version_tags():
    _, out = _git("tag", "-l", "v*", "--sort=-v:refname")
    return [t.strip() for t in out.splitlines()
            if t.strip() and VER_RE.match(t.strip())]


def _have_origin_master():
    rc, _ = _git("rev-parse", "--verify", "--quiet", "origin/master")
    return rc == 0


def test_the_highest_version_tag_is_on_origin_master():
    """The tag a bare `make` would compile in must be on master.

    agent/Makefile takes the HIGHEST v* tag, so that tag is the one that
    becomes AGENT_VERSION and therefore the one that gets published. If it is
    not on master, the build that reaches the fleet is not in any mainline
    branch.
    """
    if not _have_origin_master():
        # Not a silent skip: say why, so a green run cannot be mistaken for a
        # verified one on a clone with no remote.
        import pytest
        pytest.skip("no origin/master in this clone - cannot check ancestry")

    tags = _version_tags()
    assert tags, "no vX.Y.Z tags at all - the Makefile would compile no version"
    top = tags[0]

    rc, _ = _git("merge-base", "--is-ancestor", top, "origin/master")
    assert rc == 0, (
        "the highest version tag %s is NOT an ancestor of origin/master.\n\n"
        "That is the tag agent/Makefile compiles into AGENT_VERSION, so a build "
        "made now would be published to the fleet from a commit that is in no "
        "mainline branch. Auto-update fires on version INEQUALITY, so the next "
        "agent to publish from master would silently erase whatever %s "
        "contains from all eight boxes.\n\n"
        "Fix: land the commit -- git fetch origin && git rebase origin/master "
        "&& bash tests/run_all.sh && git push origin HEAD:master -- and "
        "re-point the tag if the rebase moved it. Do NOT delete the tag to make "
        "this pass." % (top, top)
    )


def test_no_version_tag_is_stranded_off_master():
    """Every published version must remain reproducible from master.

    A tag pointing at an orphaned SHA is how the next person gets a binary they
    cannot rebuild -- particularly here, where a rebase routinely moves
    commits and the worktrees are disposable.

    Only the most recent tags are checked: older ones legitimately predate
    history rewrites, and a test that fails on ancient archaeology gets
    ignored, which is worse than not having it.
    """
    if not _have_origin_master():
        import pytest
        pytest.skip("no origin/master in this clone - cannot check ancestry")

    stranded = []
    for tag in _version_tags()[:12]:
        rc, _ = _git("merge-base", "--is-ancestor", tag, "origin/master")
        if rc != 0:
            _, sha = _git("rev-list", "-n1", tag)
            _, subj = _git("log", "-1", "--format=%s", tag)
            stranded.append("%s -> %s %s" % (tag, sha[:9], subj))

    assert not stranded, (
        "these recent version tags are not on origin/master, so the builds "
        "they produced cannot be rebuilt from a fresh clone:\n  %s\n\n"
        "Land the commits rather than deleting the tags." % "\n  ".join(stranded)
    )


def test_autoupdate_still_pulls_on_INEQUALITY_not_on_newer():
    """The premise of this whole file.

    If auto-update ever starts comparing "is the remote NEWER", publishing an
    older build stops being able to downgrade the fleet and these tests are
    guarding something that no longer exists. The comment block in
    autoupdate.c is the contract; assert it is still there so a change to it
    is deliberate.
    """
    src = REPO / "agent" / "src" / "autoupdate.c"
    if not src.exists():
        import pytest
        pytest.skip("agent/src/autoupdate.c not present")
    text = src.read_text(errors="replace").lower()
    assert "ver" in text, "autoupdate.c no longer mentions a version sidecar"
    # A strict > comparison on the parsed version would break the premise.
    assert "strcmp" in text or "mismatch" in text or "differ" in text, (
        "autoupdate.c no longer looks like a string/inequality comparison - "
        "re-read it and update this file's reasoning if it now only upgrades"
    )


def test_recent_version_tags_are_pushed_to_origin():
    """A tag that exists in one clone only is one careless command from gone.

    Learned the hard way on 2026-08-30: `v1.72.1` -- the tag identifying the
    binary ALL EIGHT boxes were running, and the version on the share's
    `.ver` sidecar -- existed only in the local clone. A stray `git tag -d`
    during unrelated work destroyed it outright, with no reflog for tags and
    nothing on the remote to restore from.

    Ancestry (the checks above) is not enough on its own: a tag can point at a
    perfectly good commit on master and still be invisible to everyone else,
    so the record of *which commit produced the binary the fleet is running*
    lives on one disk. Push the tags.
    """
    if not _have_origin_master():
        import pytest
        pytest.skip("no origin/master in this clone - cannot check the remote")

    rc, out = _git("ls-remote", "--tags", "origin")
    if rc != 0:
        import pytest
        pytest.skip("cannot reach origin to list tags")
    remote = {ln.split("refs/tags/")[-1].replace("^{}", "")
              for ln in out.splitlines() if "refs/tags/" in ln}

    missing = [t for t in _version_tags()[:6] if t not in remote]
    assert not missing, (
        "these recent version tags exist only in this clone and are not on "
        "origin:\n  %s\n\n"
        "Tags have no reflog. If one is deleted locally it is gone, and with "
        "it the record of which commit built the binary the fleet is running. "
        "Fix: git push origin --tags" % "\n  ".join(missing)
    )
