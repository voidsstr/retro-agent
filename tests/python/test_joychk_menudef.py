"""Two small helpers the Win9x DOS titles ship (scripts/dosgames/stage_win9x_dos.py).

JOYCHK.COM (scripts/dosgames/joychk) - "is a stick on the game port?" as an
ERRORLEVEL. F-19 and F-117A ask "Do you have a joystick (Y/N)?" in text mode
at every start; their launchers answer it from the port, so .243 (which has a
stick) gets /J or Y and a box without one gets /NJ or N instead of waiting
forever in a calibration that polls for fire buttons. Measured in DOSBox:
joysticktype=none -> 0, 2axis and 4axis -> 1.

MENUDEF9.EXE (scripts/fleet/win9x/menudef9x.c) - points CONFIG.SYS's boot-menu
default at [EMS] for one boot, for the real-DOS titles that need EMS or upper
memory (Falcon 3.0, Tornado, Pacific Strike). Measured in the Win98 build VM:
EMS set with only the block name changed, an unknown block refused, BASE set
back byte for byte. It runs on Windows 98, so it may import nothing but
KERNEL32 and USER32 (CLAUDE.md: a static import Win9x lacks kills the exe).
"""
import os
import shutil
import struct
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JOY = os.path.join(REPO, "scripts", "dosgames", "joychk")
MENUDEF = os.path.join(REPO, "scripts", "dosgames", "rundos", "MENUDEF9.EXE")


@pytest.mark.skipif(not (shutil.which("as") and shutil.which("ld")), reason="GNU as/ld not installed")
def test_the_committed_joychk_com_is_its_source(tmp_path):
    obj, com = tmp_path / "j.o", tmp_path / "J.COM"
    subprocess.run(["as", "--32", "-o", str(obj), os.path.join(JOY, "joychk.S")], check=True)
    subprocess.run(["ld", "-m", "elf_i386", "-Ttext", "0x100", "--oformat", "binary",
                    "-o", str(com), str(obj)], check=True)
    assert com.read_bytes() == open(os.path.join(JOY, "JOYCHK.COM"), "rb").read()


def test_joychk_reads_both_axes_of_stick_a():
    b = open(os.path.join(JOY, "JOYCHK.COM"), "rb").read()
    assert len(b) < 64
    assert b[:4] == bytes([0xBA, 0x01, 0x02, 0xEE]), "mov dx,201h / out dx,al: fire the one-shots"
    assert bytes([0xA8, 0x03]) in b, "test al,3: stick A's X and Y bits"
    assert bytes([0xB8, 0x00, 0x4C]) in b and bytes([0xB8, 0x01, 0x4C]) in b, "exit 0 and exit 1"


@pytest.mark.skipif(not shutil.which("dosbox"), reason="dosbox not installed")
@pytest.mark.parametrize("jtype,want", [("none", "0"), ("2axis", "1"), ("4axis", "1")])
def test_joychk_in_dosbox(tmp_path, jtype, want):
    c = tmp_path / "c"
    c.mkdir()
    shutil.copy(os.path.join(JOY, "JOYCHK.COM"), c / "JOYCHK.COM")
    # One file per outcome: DOSBox (like COMMAND.COM) opens an IF's redirect
    # target even when the condition is false, so one shared file is ambiguous.
    (c / "T.BAT").write_bytes(b"@echo off\r\nJOYCHK\r\nif errorlevel 1 echo y > J1.TXT\r\n"
                              b"if not errorlevel 1 echo y > J0.TXT\r\n")
    conf = tmp_path / "d.conf"
    conf.write_text("[sdl]\noutput=surface\n[dosbox]\nmemsize=16\n[cpu]\ncycles=max\n[mixer]\n"
                    "nosound=true\n[joystick]\njoysticktype=%s\n[autoexec]\nMOUNT C \"%s\"\nC:\n"
                    "call T.BAT\nexit\n" % (jtype, c))
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", HOME=str(tmp_path))
    subprocess.run(["timeout", "-s", "KILL", "60", "dosbox", "-conf", str(conf), "-userconf-skip"],
                   env=env, capture_output=True)
    got = "1" if (c / "J1.TXT").exists() and (c / "J1.TXT").stat().st_size else \
          "0" if (c / "J0.TXT").exists() and (c / "J0.TXT").stat().st_size else "?"
    assert got == want


def _imports(pe):
    """DLL names from a PE32's import directory."""
    e_lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    assert pe[e_lfanew:e_lfanew + 4] == b"PE\0\0"
    opt = e_lfanew + 24
    assert struct.unpack_from("<H", pe, opt)[0] == 0x10B, "PE32"
    subsystem = struct.unpack_from("<H", pe, opt + 68)[0]
    imp_rva = struct.unpack_from("<I", pe, opt + 104)[0]
    nsec = struct.unpack_from("<H", pe, e_lfanew + 6)[0]
    sec = opt + struct.unpack_from("<H", pe, e_lfanew + 20)[0]

    def off(rva):
        for i in range(nsec):
            va, size, raw = struct.unpack_from("<III", pe, sec + 40 * i + 12)
            if va <= rva < va + max(size, 1):
                return rva - va + raw
        raise ValueError(rva)
    names, d = [], off(imp_rva)
    while True:
        name_rva = struct.unpack_from("<I", pe, d + 12)[0]
        if not name_rva:
            break
        n = off(name_rva)
        names.append(pe[n:pe.index(b"\0", n)].decode().upper())
        d += 20
    return subsystem, names


def test_menudef9_is_a_win98_safe_gui_exe():
    pe = open(MENUDEF, "rb").read()
    subsystem, dlls = _imports(pe)
    assert subsystem == 2, "GUI: no console window flashing up under the launcher"
    assert set(dlls) <= {"KERNEL32.DLL", "USER32.DLL"}, dlls


@pytest.mark.skipif(not shutil.which("i686-w64-mingw32-gcc"), reason="mingw not installed")
def test_menudef9x_builds_from_its_source(tmp_path):
    exe = tmp_path / "m.exe"
    subprocess.run(["i686-w64-mingw32-gcc", "-O1", "-march=i586", "-mwindows", "-nostdlib", "-fno-builtin",
                    "-e", "_start@0", "-o", str(exe),
                    os.path.join(REPO, "scripts", "fleet", "win9x", "menudef9x.c"),
                    "-lkernel32", "-luser32", "-s"], check=True)
    assert set(_imports(exe.read_bytes())[1]) <= {"KERNEL32.DLL", "USER32.DLL"}


def test_menudef9x_changes_only_the_block_name():
    src = open(os.path.join(REPO, "scripts", "fleet", "win9x", "menudef9x.c")).read()
    assert "copy(g_new + vstart, block, blen)" in src, "only the menudefault value is replaced"
    assert "have_block" in src and "no [%s] block" in src, "a block CONFIG.SYS does not have is refused"
    assert "original bytes put back" in src, "a write that fails restores the file"
