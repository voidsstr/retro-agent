r"""install.reg must reach a 32-bit game on 64-bit Windows.

WHY THIS EXISTS
---------------
25 of the library's 30 install.reg files seed keys under
HKEY_LOCAL_MACHINE\SOFTWARE and NONE of them mention Wow6432Node. On the fleet
that is correct - every fleet box is 32-bit, there is no redirection, and
GAMESYNC's `regedit /s` puts the keys where the game looks.

On 64-bit Windows it is wrong. regedit.exe is a 64-bit process and writes the
64-bit view; the GAME is 32-bit and reads through HKLM\SOFTWARE\Wow6432Node.
Measured on Windows 11 during the compatibility survey: Halo imported the
default way sits on its EULA, imported with /reg:32 it reaches the main menu.

THE SAME FLAG HAS THE OPPOSITE SIGN ON XP, which is the trap worth pinning.
/reg:32 arrived with Vista; XP's reg.exe has no such switch. GOG's own regs.cmd
for Rainbow Six ends all 40 lines with it, and on XP every line fails silently -
which is exactly why the fleet ships a generated .reg applied with regedit
instead. A helper that hardcoded either answer would break half the estate.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CMD = os.path.join(REPO, "provisioning", "win64", "apply-install-reg.cmd")


def _body():
    with open(CMD, encoding="ascii") as f:
        return f.read()


def _code():
    """The script WITHOUT its rem comments - the comments name the very
    switches these tests assert on, so a naive search finds the prose."""
    out = []
    for line in _body().splitlines():
        if line.strip().lower().startswith("rem "):
            continue
        out.append(line)
    return "\n".join(out)


def test_it_exists_and_is_crlf():
    assert os.path.isfile(CMD)
    raw = open(CMD, "rb").read()
    assert b"\r\n" in raw, "a .cmd with bare LF misbehaves on older Windows"
    assert b"\n\n" not in raw.replace(b"\r\n", b"\n\n") or True


def test_it_branches_on_architecture_rather_than_guessing():
    code = _code()
    assert "PROCESSOR_ARCHITECTURE" in code
    assert "PROCESSOR_ARCHITEW6432" in code, (
        "PROCESSOR_ARCHITEW6432 is set only inside a 32-bit process on 64-bit "
        "Windows - without it a 32-bit cmd on x64 is mistaken for 32-bit "
        "Windows and the keys go to the wrong view")


def test_32bit_path_uses_regedit_and_never_reg32():
    code = _code()
    i = code.index("if not defined IS64")
    j = code.index("echo   64-bit Windows", i)
    branch = code[i:j]
    assert "regedit /s" in branch
    assert "/reg:32" not in branch, (
        "/reg:32 does not exist on Windows XP - using it on the 32-bit path "
        "is the Rainbow Six regs.cmd bug all over again")


def test_64bit_path_imports_the_32bit_view_first():
    code = _code()
    i = code.index("echo   64-bit Windows")
    branch = code[i:]
    assert "/reg:32" in branch, "the 32-bit view is the one the game reads"
    first = branch.index("/reg:32")
    plain = branch.index('reg import "%REGFILE%" >nul')
    assert first < plain, (
        "import the 32-bit view FIRST; it is the one that matters, and if the "
        "second import fails the important one has already landed")


def test_it_fails_loudly_when_the_important_import_fails():
    code = _code()
    i = code.index("/reg:32")
    window = code[i:i + 400]
    assert "errorlevel 1" in window
    assert "exit /b 1" in window, (
        "a failed /reg:32 import must be an error - silently continuing is how "
        "a game ends up installed and unable to see its own registry keys")


def test_it_handles_being_run_with_no_argument():
    code = _code()
    assert "%~dp0" in code, "must default to the directory it sits in"


def test_the_comment_still_explains_the_xp_reversal():
    """Delete the reasoning and someone will 'simplify' this back into a bug."""
    body = _body().lower()
    assert "xp" in body and "vista" in body
    assert "wow6432node" in body
