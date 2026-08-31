"""Two facts the docs had wrong, both of which cost an agent real work.

1. **`smbclient -A` IS an available publish route.** CLAUDE.md said it was not,
   because sudo needs an interactive password here -- but the NAS credentials
   are vaulted (`fleet-nas-192-168-1-122-user` / `-password`), so a headless
   session can write an auth file from the vault and publish with no gvfs and
   no fleet box. Verified 2026-08-31 by listing the share that way.

   The cost of the wrong version: an agent finished a library fix in a headless
   context where gvfs was absent and `/mnt` is read-only, read that the only
   remaining route did not exist, and stopped with the fix undelivered.

2. **BUT `smbclient put` stamps this NAS `Oct 31 2007`** -- correct
   time-of-day, nineteen years stale, on every dialect. Measured directly.
   GAMESYNC's resume test has been size **AND** mtime since v1.62.0, so a
   **same-size** edit published this way is skipped on every box, silently,
   with `state=done` and `failed_files: 0`. That is this project's signature
   failure: the tool reports success and the operator believes it.

3. **"GDI cannot photograph an exclusive-fullscreen surface" is a WINDOWS 7
   fact, not a fleet-wide one.** Measured on `.123` and `.133` (both XP SP3): a
   plain `SCREENSHOT 0` of fullscreen GoldSrc returned the game in 3D at
   1280x960. Only `.246` (Win 7) comes back black -- XP has no DWM, Vista/7
   composite. Written up as universal, it sent agents down the slow
   screenshot-bind path on seven of eight boxes.

Doc-only assertions: the docs are what every agent reads before acting.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLAUDE = os.path.join(REPO, "CLAUDE.md")


def _text():
    with open(CLAUDE, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_smbclient_is_documented_as_available():
    t = _text()
    assert "`smbclient -A` is **not** an available route" not in t, (
        "CLAUDE.md has gone back to claiming smbclient is unavailable. It is "
        "available -- the NAS creds are vaulted -- and the wrong version "
        "stranded a headless agent that had a proven library fix and nowhere "
        "to publish it.")
    assert "fleet-nas-192-168-1-122-user" in t, (
        "the vaulted NAS credential names are gone; without them the smbclient "
        "route is undiscoverable and someone will re-add the 'unavailable' claim")


def test_the_stale_mtime_hazard_is_documented_with_its_consequence():
    """Naming the date is not enough -- the danger is what GAMESYNC does next."""
    t = _text()
    assert "Oct 31 2007" in t, (
        "the smbclient stale-mtime warning is gone")
    low = t.lower()
    assert "same-size" in low and "skipped" in low, (
        "the warning must say WHY it matters: GAMESYNC's resume test is size "
        "AND mtime, so a same-size edit published this way is silently skipped "
        "on every box while reporting success")


def test_the_gdi_black_claim_is_scoped_to_windows_7():
    t = _text()
    assert "It CAN on XP" in t, (
        "CLAUDE.md no longer records that GDI capture WORKS on XP. Stated as "
        "fleet-wide, this sends every agent down the slow screenshot-bind path "
        "on seven of eight boxes.")
    assert "DWM" in t, (
        "the reason is what makes the rule predictable on a new box -- XP has "
        "no DWM, Vista/7 composite. Without it the fact reads as folklore.")


def test_the_fullscreen_rule_still_stands_alongside_these():
    """These two corrections make capture EASIER; they must not be read as
    relaxing the requirement that a verification is a fullscreen observation."""
    t = _text()
    assert "TEST IN FULLSCREEN" in t, (
        "the fullscreen-testing rule has gone; the capture corrections make it "
        "cheaper to satisfy, not optional")
