"""The staged-library validator refuses a Windows 9x shortcut whose icon 9x cannot draw.

WHY. On 2026-09-29 .243 (Win98 SE, a 256-colour desktop) showed the generic
MS-DOS icon for "Descent - DOS" although launch.txt named d1x-rebirth.ico, a
file that exists and that every XP box draws: it holds only 32-bit alpha
images, which Windows 9x cannot use. The validator passed it, because
icon_xp_problem() only asks whether XP can draw an icon.

MEASURED on .243 the same day, one desktop shortcut per kind side by side: an
8-bit icon and a 24-bit-only icon (Redneck Rampage's rampage.ico) both drew;
the 32-bit-only d1x-rebirth.ico showed the generic icon.

icon_9x_problem() in scripts/validate-staged-library.py encodes that, and
check_title() FAILS it for a shortcut that exists only for 9x boxes (max_os
win9x) and WARNS once per title where a 9x box could receive it.
"""
import importlib.util
import os
import struct

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location(
    "validate_staged_library", os.path.join(REPO, "scripts", "validate-staged-library.py"))
vl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vl)

PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 32


def _dib(bpp):
    return struct.pack("<IiiHH", 40, 32, 64, 1, bpp) + b"\0" * 64


def _ico(entries):
    head = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    dirs, body = b"", b""
    for p in entries:
        dirs += struct.pack("<BBBBHHII", 32, 32, 0, 0, 1, 32, len(p), off + len(body))
        body += p
    return head + dirs + body


def _write(path, data):
    path.write_bytes(data)
    return str(path)


def test_32_bit_only_is_refused_24_and_8_bit_are_drawn(tmp_path):
    # d1x-rebirth.ico: four 32-bit images and nothing else -> the generic icon.
    prob = vl.icon_9x_problem(_write(tmp_path / "d1x.ico", _ico([_dib(32)] * 4)))
    assert prob and "32-bit" in prob and "generic" in prob
    # rampage.ico: 24-bit only - drew on .243.
    assert vl.icon_9x_problem(_write(tmp_path / "rr.ico", _ico([_dib(24)]))) is None
    # DESCENT9.ICO: 8-bit - drew on .243. 4-bit is the pre-XP norm.
    assert vl.icon_9x_problem(_write(tmp_path / "d9.ico", _ico([_dib(8)]))) is None
    assert vl.icon_9x_problem(_write(tmp_path / "h2.ico", _ico([_dib(4)]))) is None
    # One drawable image is enough: 9x picks it.
    assert vl.icon_9x_problem(_write(tmp_path / "mix.ico", _ico([_dib(32), _dib(8)]))) is None


def test_png_only_is_refused_and_an_exe_is_not_this_checks_business(tmp_path):
    prob = vl.icon_9x_problem(_write(tmp_path / "gog.ico", _ico([PNG])))
    assert prob and "PNG" in prob
    # An .exe's icon is icon_xp_problem()'s to judge.
    assert vl.icon_9x_problem(os.path.join(REPO, "agent", "tools", "retro_chat.exe")) is None


def test_the_old_check_passed_the_icon_9x_cannot_draw(tmp_path):
    """The buggy state this closes: XP's check alone calls it drawable."""
    ico = _write(tmp_path / "d1x.ico", _ico([_dib(32)] * 4))
    assert vl.icon_xp_problem(ico) is None          # the old answer: fine
    assert vl.icon_9x_problem(ico) is not None      # the fixed answer: not on 9x


def _title(lib, name, launch_lines, requires, icons):
    t = lib / name
    t.mkdir(parents=True)
    for target, _disp, _icon in launch_lines:
        (t / target).write_bytes(b"@echo off\r\n")
    for icon, data in icons.items():
        (t / icon).write_bytes(data)
    (t / "launch.txt").write_bytes("".join(
        "%s\t%s\t%s\r\n" % line for line in launch_lines).encode("latin-1"))
    if requires is not None:
        import json
        (t / "requires.json").write_text(json.dumps(requires))
    return str(lib)


def _problems(lib, name):
    return [(p.severity, p.check, p.detail) for p in vl.check_title(lib, name)
            if p.check == "icon"]


def test_a_win9x_only_shortcut_with_a_32_bit_icon_fails(tmp_path):
    lib = _title(tmp_path, "Descent1",
                 [("Play Descent - DOS.bat", "Descent - DOS", "d1x-rebirth.ico")],
                 {"shortcuts": {"Play Descent - DOS.bat": {"max_os": "win9x"}}},
                 {"d1x-rebirth.ico": _ico([_dib(32)] * 4)})
    probs = _problems(lib, "Descent1")
    assert [p[0] for p in probs] == ["fail"], probs
    assert "Windows 9x shortcut" in probs[0][2]


def test_the_title_level_max_os_counts_too(tmp_path):
    lib = _title(tmp_path, "Falcon3", [("Play Falcon 3.0.bat", "Falcon 3.0", "ICON.ICO")],
                 {"max_os": "win9x"}, {"ICON.ICO": _ico([_dib(32)])})
    assert [p[0] for p in _problems(lib, "Falcon3")] == ["fail"]


def test_a_shortcut_a_9x_box_could_receive_warns_once_per_icon(tmp_path):
    # Descent1's four DOSBox/Rebirth launchers: gated off .243 by a CPU floor,
    # not by the OS - latent for the next 9x box, so a warning, and one.
    lines = [("Play Descent.bat", "Descent", "d1x-rebirth.ico"),
             ("Host Descent - LAN.bat", "Descent - Host", "d1x-rebirth.ico"),
             ("Join Descent - LAN.bat", "Descent - Join", "d1x-rebirth.ico")]
    lib = _title(tmp_path, "Descent1", lines,
                 {"shortcuts": {l[0]: {"min_cpu_mhz": 350} for l in lines}},
                 {"d1x-rebirth.ico": _ico([_dib(32)] * 4)})
    assert [p[0] for p in _problems(lib, "Descent1")] == ["warn"]


def test_a_shortcut_no_9x_box_can_receive_says_nothing(tmp_path):
    lib = _title(tmp_path, "FarCry", [("Play Far Cry.bat", "Far Cry", "fc.ico")],
                 {"min_os": "winxp"}, {"fc.ico": _ico([_dib(32)])})
    assert _problems(lib, "FarCry") == []
    lib = _title(tmp_path / "b", "Halo", [("Play Halo.bat", "Halo", "halo.ico")],
                 {"shortcuts": {"Play Halo.bat": {"min_os": "win2k"}}},
                 {"halo.ico": _ico([_dib(32)])})
    assert _problems(lib, "Halo") == []


def test_a_drawable_icon_says_nothing(tmp_path):
    lib = _title(tmp_path, "Descent1",
                 [("Play Descent - DOS.bat", "Descent - DOS", "DESCENT9.ICO")],
                 {"shortcuts": {"Play Descent - DOS.bat": {"max_os": "win9x"}}},
                 {"DESCENT9.ICO": _ico([_dib(8), _dib(8)])})
    assert _problems(lib, "Descent1") == []


def test_no_requires_json_means_a_9x_box_could_receive_it(tmp_path):
    # Absent data never blocks a title (the gate is fail-open), so a title
    # with no requirements CAN reach a 9x box - warn, do not stay silent.
    lib = _title(tmp_path, "Mystery", [("Play.bat", "Mystery", "m.ico")], None,
                 {"m.ico": _ico([_dib(32)])})
    assert [p[0] for p in _problems(lib, "Mystery")] == ["warn"]
