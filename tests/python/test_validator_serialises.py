"""The library validator must not be runnable 15 times at once.

WHY. `validate-staged-library.py` walks ~40 GB of staged tree over SMB. On
2026-08-30 several agents ran it concurrently: **15 processes, 9 of them stuck
in uninterruptible IO (D state), the oldest 19 minutes in, none able to
finish.** The damage was not the slowness:

  * three separate agents read their own stall as a test failure and reported
    master as red when it was not;
  * one nearly reported a PASS that had actually been SIGTERMed at its timeout
    -- the wrapper exited 0 while the validator itself exited 143, which is the
    "check the exit code of the thing that did the work" trap this repo already
    has a rule about;
  * a D-state process cannot even be killed until its IO completes, so the
    pile-up had to drain on its own.

A slow check is tolerable. **A check that cannot finish, and whose stall is
indistinguishable from a failure, is worse than no check.** So it serialises.

The lock is deliberately ADVISORY and best-effort: if `fcntl` is unavailable or
anything about locking fails, the validator still runs. Refusing to check the
library because a lock could not be taken would be a worse failure than the
contention it guards against -- and that trade-off is itself asserted here, so
nobody "hardens" it into a hard dependency later.
"""
import importlib.util
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "validate-staged-library.py")

spec = importlib.util.spec_from_file_location("validate_staged_library", SRC)
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)


def test_a_second_run_does_not_start_while_one_holds_the_lock():
    """The whole point: two concurrent walks of the share must not happen."""
    first = v._acquire_lock(0, quiet=True)
    assert first not in (None,), "could not take the lock at all"
    if first == "unsupported":
        import pytest
        pytest.skip("no fcntl on this platform - locking is best-effort")
    try:
        second = v._acquire_lock(0, quiet=True)
        assert second is None, (
            "a second validator acquired the lock while the first held it -- "
            "concurrent runs are exactly what wedged the share"
        )
    finally:
        first.close()


def test_the_lock_is_released_when_the_holder_closes():
    """A lock that outlives its holder would wedge every later run."""
    first = v._acquire_lock(0, quiet=True)
    if first == "unsupported":
        import pytest
        pytest.skip("no fcntl on this platform")
    first.close()
    second = v._acquire_lock(0, quiet=True)
    assert second not in (None, "unsupported"), (
        "the lock was not released when the holder closed it"
    )
    second.close()


def test_no_wait_is_a_DISTINCT_exit_code_from_problems_found():
    """"I could not run" and "the library is broken" are different answers.

    Returning 1 for a declined run would make a busy share look exactly like a
    library that fails its contract -- which is the failure mode that had
    agents reporting master as red all afternoon.
    """
    src = open(SRC, encoding="utf-8").read()
    assert "return 75" in src, (
        "the --no-wait path must return its own exit code (75/EX_TEMPFAIL), "
        "not 1, or a contended share is indistinguishable from a broken library"
    )
    assert "--no-wait" in src and "--lock-wait" in src


def test_locking_failure_still_runs_the_validator():
    """Best-effort by design. Do not turn this into a hard dependency.

    If fcntl is missing or the lock file cannot be opened, `_acquire_lock`
    returns the sentinel "unsupported" and main() proceeds. A validator that
    refused to check the library because it could not take a lock would be a
    worse failure than the contention.
    """
    src = open(SRC, encoding="utf-8").read()
    assert '"unsupported"' in src, (
        "the best-effort sentinel is gone -- a platform without fcntl would "
        "now be unable to validate at all"
    )
    i = src.index("def _acquire_lock")
    body = src[i:i + 2000]
    assert "except ImportError" in body and "except OSError" in body, (
        "_acquire_lock no longer degrades gracefully when locking is "
        "unavailable"
    )


def test_a_waiter_says_what_it_is_waiting_for():
    """A mute 20-minute stall is what got read as a hang three times today."""
    src = open(SRC, encoding="utf-8").read()
    i = src.index("def _acquire_lock")
    body = src[i:i + 2000]
    assert "waiting:" in body, (
        "a queued run prints nothing, so its stall is indistinguishable from "
        "the wedge it exists to prevent"
    )
