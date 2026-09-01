"""New titles are installed in the BUILD VM, not on a fleet box.

USER DIRECTIVE, 2026-09-01: *"make sure this approach is used going forward, is
fully documented and your instructions tell you to use that method."*

WHY IT NEEDED WRITING DOWN. An agent with a detailed brief installed Halo 2
directly onto `.123`, because the method was established practice and appeared
in no document. The checklist said "the installed tree, not the installer" and
never said WHERE setup runs, so the reasonable reading was "on a fleet box" --
which is what `.claude/skills/game-install/SKILL.md` describes.

WHY THE VM IS RIGHT, in reasons this project has already paid for:

  * A tree captured from a real machine carries THAT machine's fingerprints.
    Serious Sam's PersistentSymbols.ini was staged from one box and then reset
    every other box on each sync, and would have carried one machine's detected
    renderer to all eight.
  * A failed install is free in a VM and permanent on a fleet box, because
    GAMESYNC never deletes -- `_fstest*.bat`, an empty SoldierOfFortune2.wip and
    a 44 MB C:\\H2SRC all had to be removed by hand.
  * `.123` and `.133` must never be rebooted (unactivated XP) and installers
    routinely want one.
  * The fleet is powered on demand and hardware is swapped constantly, so a box
    can vanish mid-install. The VM cannot.

The exception that must survive: anything HARDWARE-dependent -- Glide, a per-box
resolution, a driver -- has to be proven on the metal, because the VM's Cirrus
adapter is not that hardware. Install in the VM; verify on the machine.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLAUDE = os.path.join(REPO, "CLAUDE.md")
VM_DIR = os.path.expanduser("~/retro-vm")


def _text():
    with open(CLAUDE, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_the_rule_is_stated_and_marked_required():
    t = _text()
    assert "INSTALL IN THE BUILD VM" in t, (
        "CLAUDE.md no longer tells agents to install new titles in the build "
        "VM. Without it the reasonable reading is 'install on a fleet box', "
        "which is exactly the mistake this section exists to prevent.")
    i = t.index("INSTALL IN THE BUILD VM")
    assert "REQUIRED" in t[i:i + 120]


def test_it_names_the_script_the_image_and_the_port():
    """A rule an agent cannot act on is not a rule."""
    t = _text()
    for token in ("run-build.sh", "xp3.qcow2", "19898", "vmagent.py"):
        assert token in t, (
            "CLAUDE.md does not name %r. An agent needs the script, the image, "
            "the forwarded port and the driver to actually use the VM." % token)


def test_it_records_WHY_not_just_what():
    """A rule with no reason gets 'optimised away' by the next agent."""
    t = _text().lower()
    assert "persistentsymbols" in t or "fingerprints" in t, (
        "the contamination reason is gone -- without it, installing on a fleet "
        "box looks equally good and someone will do it again")
    assert "gamesync never deletes" in t, (
        "the 'a failed install is permanent on a box' reason is gone")


def test_the_hardware_exception_survives():
    """Do not let this harden into 'never touch the metal'."""
    t = _text()
    assert "Install in the VM; verify on the metal" in t or \
           "verify on the metal" in t, (
        "the hardware exception is gone. Glide, per-box resolutions and drivers "
        "CANNOT be proven in a VM whose adapter is Cirrus, and a rule that "
        "forbade it would push agents into verifying the wrong thing.")


def test_the_readiness_lesson_is_carried_across():
    """The same mistake exists on both sides of the fence."""
    t = _text()
    assert "never by `connect()`" in t or "never by connect()" in t, (
        "vmagent.py's own warning is missing: QEMU binds the forwarded port at "
        "startup whether or not anything listens inside, so a TCP connect "
        "proves nothing -- the same shape as a dead fleet agent whose :9897 "
        "still accepted sockets")


def test_the_vm_actually_exists():
    """Documenting a VM nobody has would be worse than documenting nothing."""
    import pytest
    if not os.path.isdir(VM_DIR):
        pytest.skip("SKIPPED LOUDLY: %s absent on this host - the documented "
                    "build VM could not be confirmed" % VM_DIR)
    assert os.path.exists(os.path.join(VM_DIR, "run-build.sh")), (
        "~/retro-vm/run-build.sh is gone but CLAUDE.md still tells agents to "
        "use it")
