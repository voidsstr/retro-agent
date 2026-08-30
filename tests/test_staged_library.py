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
    print("  FAIL  the library would NOT deploy cleanly - see above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
