#!/usr/bin/env python3
"""The staged library must be deployable to a brand-new box, right now.

WHY THIS EXISTS. "Staged" is a promise: the agent moves a title onto a freshly
imaged machine and it simply works — no installer, no wizard, nobody at the
keyboard. Every way we have broken that promise so far was SILENT:

  * a launch.txt naming a file that is not in the tree — no shortcut is made and
    nothing says why
  * a data line pushed past the agent's 1023-byte read by comments above it —
    that game silently loses its shortcut
  * a launcher whose filename contains parentheses — unlaunchable through the
    agent, and perfectly fine from a desktop double-click, so it survives review
  * an icon path with a typo — degrades quietly to the auto-resolved icon, i.e.
    exactly the wrong artwork the explicit field exists to prevent
  * a DOSBox conf that never sets fullscreen=true

None of those appear until a box tries them, and by then the box is wrong. This
runs scripts/validate-staged-library.py against the real share so the answer is
about the library as it actually stands, not a fixture.

It SKIPS when the share is not mounted — a dev host without the SMB mount must
not fail the suite — and that skip is loud, because a silent skip would let the
library rot unnoticed.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VALIDATOR = os.path.join(REPO, "scripts", "validate-staged-library.py")
LIB = "/mnt/retro-share/Files/Games-Library"


def main():
    print("== the staged library would deploy cleanly to a new box ==")

    if not os.path.isfile(VALIDATOR):
        print("  FAIL  %s is missing" % VALIDATOR)
        return 1
    if not os.path.isdir(LIB):
        print("  SKIP  %s not mounted - the library was NOT checked" % LIB)
        print("        (run this on a host with the share mounted before any")
        print("         imaging run; a fresh box is what this protects)")
        return 0

    r = subprocess.run([sys.executable, VALIDATOR, "--quiet"],
                       capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    for line in out.splitlines():
        print("  " + line)

    if r.returncode == 0:
        print("  PASS  every staged title satisfies the contract")
        return 0

    # "THE VALIDATOR DID NOT FINISH" AND "THE LIBRARY IS BROKEN" ARE DIFFERENT
    # CALLS TO ACTION, AND THEY USED TO RENDER IDENTICALLY.
    #
    # This really happened on 2026-08-30. The validator walks every title's
    # whole tree over CIFS; with two dozen of them running at once from
    # different fleet agents, one was killed and its captured buffers went with
    # it - so the wrapper printed the FAIL banner with NOTHING above it and the
    # suite said "the library would NOT deploy cleanly". Three independent
    # validator runs either side of it all returned DEPLOYABLE. The library was
    # never broken; the measurement was.
    #
    # A negative returncode is a signal (killed). Empty output on a nonzero
    # exit means it died before it could report. Neither is a verdict about the
    # library, and reporting one as if it were is how a phantom bug report
    # sends everybody chasing something that was never there.
    if r.returncode < 0:
        print("  ERROR the validator was KILLED by signal %d - the library was"
              % -r.returncode)
        print("        NOT checked. This is not a library failure.")
        print("        Usual cause: several agents running this at once against")
        print("        the same SMB share. Re-run when it is quiet, or point it")
        print("        at the gvfs transport with --library.")
        return 1
    if not out.strip():
        print("  ERROR the validator exited %d without reporting anything - the"
              % r.returncode)
        print("        library was NOT checked. This is not a library failure.")
        return 1

    print("  FAIL  the library would NOT deploy cleanly - see above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
