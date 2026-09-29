"""Quake III Arena baseq3 in-game Video Mode menu - provisioning/patches/q3-baseq3-ui.

The defect (measured 2026-09-29 on .123 .145 .195 .240): both engines load id's
1.32 UI from baseq3/pak8.pk3, whose Video Mode list is a fixed 12-entry table;
at the launcher's r_mode -1 + r_customwidth/height (1920x1080 on the LCDs) it
shows "640x480" and ACCEPT writes r_mode 3. The fix is baseq3/zz-fleetres-ui.pk3,
id's q3_ui rebuilt from the GPL source with one change to ui_video.c
(ui_video-fleetres.patch + ui_fleetres.h), built and gated by apply.py.

What these tests pin:
  - the pure logic of ui_fleetres.h, compiled NATIVELY from the same file the
    QVM is built from (both the fixed values and id's old ones);
  - the QVM/pk3 format rules (VM_MAGIC, 32-byte header, no jump table - a VER2
    QVM is ERR_FATAL on retail 1.32c), the pk3 name and sort order, a
    deterministic single-member pk3;
  - the Q3 VM interpreter (qvmsim.py) the behaviour gate runs on;
  - the patch file's shape.
Tests that need the NAS share, the cached GPL sources or a --build output
SKIP LOUDLY when those are absent, and say what was NOT checked.
"""
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import zipfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PATCH_DIR = os.path.join(REPO, "provisioning", "patches", "q3-baseq3-ui")
LIBRARY = "/mnt/retro-share/Files/Games-Library"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


apply = _load("q3ui_apply", os.path.join(PATCH_DIR, "apply.py"))
qvmsim = _load("q3ui_qvmsim", os.path.join(PATCH_DIR, "qvmsim.py"))
OUT = apply.DEFAULT_OUT
MANIFEST = os.path.join(OUT, "manifest.json")
BUILD = os.path.join(OUT, "_build")


# ----------------------------------------------------------------------------
# synthetic QVMs
# ----------------------------------------------------------------------------
def encode(prog):
    """[(opname, operand)] -> code bytes (original Q3 VM encoding)."""
    out = bytearray()
    for name, arg in prog:
        op = qvmsim.__dict__["OP_" + name]
        out.append(op)
        if op in qvmsim._OPERAND4:
            out += struct.pack("<i", arg)
        elif op == qvmsim.OP_ARG:
            out.append(arg)
    return bytes(out)


def make_qvm(prog, data=b"\0" * 4, lit=b"", bss=64, magic=qvmsim.VM_MAGIC, jtrg=b""):
    code = encode(prog)
    hdr_len = 32 if magic == qvmsim.VM_MAGIC else 36
    fields = [magic, len(prog), hdr_len, len(code), hdr_len + len(code), len(data), len(lit), bss]
    if hdr_len == 36:
        fields.append(len(jtrg))
    hdr = struct.pack("<%dI" % len(fields), *[f & 0xffffffff for f in fields])
    return hdr + code + data + lit + jtrg


def f2i(f):
    return struct.unpack("<i", struct.pack("<f", f))[0]


# vmMain(cmd, a0, a1): cmd 0 -> a0*3+1; otherwise syscall 5(a0, a1)
PROG_CALL = [
    ("ENTER", 16), ("LOCAL", 24), ("LOAD4", 0), ("CONST", 0), ("NE", 12),
    ("LOCAL", 28), ("LOAD4", 0), ("CONST", 3), ("MULI", 0), ("CONST", 1), ("ADD", 0),
    ("LEAVE", 16),
    ("LOCAL", 28), ("LOAD4", 0), ("ARG", 8), ("LOCAL", 32), ("LOAD4", 0), ("ARG", 12),
    ("CONST", -6), ("CALL", 0), ("LEAVE", 16),
]


# ----------------------------------------------------------------------------
# the output's name, format and packaging
# ----------------------------------------------------------------------------
def test_pk3_name_is_launchable_and_sorts_last():
    name = apply.PK3_NAME
    assert name.lower().endswith(".pk3")
    for bad in " ()":
        assert bad not in name, "a generated filename must carry no %r (CLAUDE.md)" % bad
    # both engines search the LAST pk3 in case-insensitive order first
    for pak in ["pak0.pk3", "pak8.pk3", "PAK8.PK3", "Pak8.pk3", "pak9.pk3"]:
        assert apply.pk3_order_key(name) > apply.pk3_order_key(pak)
    assert apply.PK3_REL == "baseq3/" + name
    assert apply.PK3_MEMBER == "vm/ui.qvm"


def test_pk3_order_is_fs_pathcmp_not_a_lower_case_compare():
    """Both engines qsort pk3 names with FS_PathCmp, which UPPER-cases letters:
    '[', '\\', ']', '^', '_' and '`' then sort AFTER every letter. The first key
    this file shipped with was name.lower(), which puts them BEFORE - so the
    --check "nothing in baseq3 shadows our pk3" guard passed names the engines
    would load over it (review 2026-09-29)."""
    k, ours = apply.pk3_order_key, apply.PK3_NAME
    for shadow in ("zz-fleetres-u_.pk3", "zz-^.pk3", "zz-fleetres-ui`.pk3", "ZZ-FLEETRES-UJ.PK3"):
        assert k(shadow) > k(ours), "%s is searched before ours" % shadow
    # the OLD key got the first two wrong - that is the defect this pins
    assert "zz-fleetres-u_.pk3".lower() < ours.lower() and "zz-^.pk3".lower() < ours.lower()
    assert k("ZZ-FLEETRES-UI.PK3") == k("Zz-FleetRes-Ui.pk3") == k(ours)
    assert k("a\\b.pk3") == k("a/b.pk3") == k("a:b.pk3")
    assert k("zz.pk3") < k("zz.pk3x"), "a prefix sorts first (the NUL terminator)"
    assert k("zz\xe9.pk3") < k("zz.pk3"), "bytes >= 0x80 are negative (signed char)"


FS_PATHCMP_HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
%s
static int cmp(const void *a, const void *b) { return FS_PathCmp(*(char **)a, *(char **)b); }
int main(int argc, char **argv) {
	int i;
	qsort(argv + 1, argc - 1, sizeof(char *), cmp);
	for (i = 1; i < argc; i++) printf("%%s\n", argv[i]);
	return 0;
}
'''


def test_pk3_order_key_matches_the_engines_own_fs_pathcmp(tmp_path):
    """Compile id's FS_PathCmp from the pinned GPL source and let IT sort."""
    import re
    repo = os.path.join(apply.SRC_CACHE, "id-q3a")
    cc = apply.host_cc()
    if not os.path.isdir(os.path.join(repo, ".git")) or not shutil.which("git") or not cc:
        pytest.skip("SKIPPED LOUDLY: %s, git or a C compiler absent - the pk3 order was NOT "
                    "checked against the engine's FS_PathCmp; run apply.py --build once" % repo)
    src = subprocess.run(["git", "-C", repo, "show", apply.ID_COMMIT + ":code/qcommon/files.c"],
                         capture_output=True, text=True, check=True).stdout
    fn = re.search(r"^int FS_PathCmp\( const char \*s1, const char \*s2 \) \{.*?^\}", src, re.S | re.M)
    assert fn, "FS_PathCmp not found in id's files.c"
    (tmp_path / "t.c").write_text(FS_PATHCMP_HARNESS % fn.group(0))
    exe = tmp_path / "t"
    subprocess.run([cc, "-o", str(exe), str(tmp_path / "t.c")], check=True, capture_output=True)
    names = ["pak0.pk3", "PAK8.pk3", "pak10.pk3", apply.PK3_NAME, "zz-fleetres-u_.pk3", "zz-^.pk3",
             "zz_x.pk3", "zzz.pk3", "ZZ-a.pk3", "zz-[.pk3", "zz.pk3", "zz.pk3x", "a:b.pk3", "a/c.pk3"]
    engine = subprocess.run([str(exe)] + names, capture_output=True, text=True,
                            check=True).stdout.split()
    assert sorted(names, key=apply.pk3_order_key) == engine
    assert engine.index("zz-fleetres-u_.pk3") > engine.index(apply.PK3_NAME)


def test_pk3_is_deterministic_and_holds_only_the_qvm():
    q = make_qvm([("ENTER", 8), ("CONST", 7), ("LEAVE", 8)])
    a, b = apply.make_pk3(q), apply.make_pk3(q)
    assert a == b, "same QVM must give the same pk3 bytes (same pure checksum)"
    z = zipfile.ZipFile(io.BytesIO(a))
    assert z.namelist() == ["vm/ui.qvm"]
    zi = z.getinfo("vm/ui.qvm")
    assert zi.compress_type == zipfile.ZIP_STORED
    assert zi.date_time == apply.PK3_DATE
    assert z.read("vm/ui.qvm") == q
    assert apply.pk3_problems(a) == []
    # a second member, or a VER2 QVM inside, is refused
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z2:
        z2.writestr("vm/ui.qvm", q)
        z2.writestr("vm/cgame.qvm", q)
    assert apply.pk3_problems(buf.getvalue())
    v2 = make_qvm([("ENTER", 8), ("CONST", 7), ("LEAVE", 8)], magic=qvmsim.VM_MAGIC + 1, jtrg=b"\0" * 8)
    assert any("VER2" in p for p in apply.pk3_problems(apply.make_pk3(v2)))


def test_qvm_format_rules():
    good = make_qvm([("ENTER", 8), ("CONST", 7), ("LEAVE", 8)])
    assert apply.qvm_format_problems(good) == []
    h = apply.qvm_header(good)
    assert h["vmMagic"] == 0x12721444 and h["codeOffset"] == 32
    # VER2 (what ioq3's q3asm writes WITHOUT -vq3): retail 1.32c ERR_FATALs on it
    ver2 = make_qvm([("ENTER", 8), ("CONST", 7), ("LEAVE", 8)], magic=0x12721445, jtrg=b"\0" * 12)
    probs = apply.qvm_format_problems(ver2)
    assert any("-vq3" in p for p in probs)
    assert any("codeOffset 36" in p for p in probs)
    # trailing bytes past data+lit = a jump-table section
    assert any("jump-table" in p for p in apply.qvm_format_problems(good + b"\0" * 8))
    assert any("bad magic" in p for p in apply.qvm_format_problems(b"\0" * 40))
    assert apply.qvm_format_problems(b"\0" * 10)


def test_qvm_decode_counts_and_operands():
    q = make_qvm(PROG_CALL)
    insns = apply.qvm_decode(q)
    assert len(insns) == len(PROG_CALL)
    assert insns[0] == (apply.OP["ENTER"], 16)
    assert insns[4] == (apply.OP["NE"], 12)
    assert insns[14] == (apply.OP["ARG"], 8)
    assert insns[18] == (apply.OP["CONST"], -6)
    # header claims one more instruction than the code holds
    bad = bytearray(q)
    struct.pack_into("<i", bad, 4, len(PROG_CALL) + 1)
    with pytest.raises(ValueError):
        apply.qvm_decode(bytes(bad))


def test_function_streams_ignore_relocation_but_not_code():
    """The drift gate: a function that only MOVED compares equal; one whose
    code changed does not; q3asm's _stackStart/_stackEnd (bss) are not code."""
    f = [(apply.OP["ENTER"], 8), (apply.OP["CONST"], 6), (apply.OP["CALL"], None),
         (apply.OP["CONST"], 1), (apply.OP["EQ"], 5), (apply.OP["LEAVE"], 8)]
    g = [(apply.OP["ENTER"], 8), (apply.OP["CONST"], 0), (apply.OP["LEAVE"], 8)]
    a = f + g                                   # f at 0 calls g at 6
    b = [(apply.OP["IGNORE"], None)] * 3 + [
        (op, (arg + 3 if op in apply.BRANCHES else arg)) for op, arg in f] + g
    b[3 + 1] = (apply.OP["CONST"], 9)          # the call target moved with g (now at 9)
    sa = apply.function_streams(a, {"f": 0, "g": 6, "_stackStart": 999})
    sb = apply.function_streams(b, {"pad": 0, "f": 3, "g": 9, "_stackEnd": 5000})
    assert sa["f"] == sb["f"] and sa["g"] == sb["g"]
    b2 = list(b)
    b2[3 + 3] = (apply.OP["CONST"], 2)         # a different constant is not code drift...
    assert apply.function_streams(b2, {"f": 3, "g": 9})["f"] == sa["f"]
    b2[3 + 4] = (apply.OP["NE"], 8)            # ...a different branch IS
    assert apply.function_streams(b2, {"f": 3, "g": 9})["f"] != sa["f"]
    assert "_stackStart" not in sa and "_stackEnd" not in sb


def test_map_segments_are_relative_to_their_own_base():
    """q3asm -m writes data/lit/bss symbols relative to their segment: bss
    symbols must be offset by dataLength+litLength (measured: memset(&s_main)
    lands at map + data + lit). Getting this wrong read uis.activemenu as 0."""
    m = "0        0 vmMain\n0 ffffffff trap_Error\n1       30 cvarTable\n2       10 $lit\n3     1b28c uis\n"
    s = qvmsim.load_map(m, 11764, 21536)
    assert s["vmMain"] == 0 and s["trap_Error"] == -1
    assert s["cvarTable"] == 0x30
    assert s["$lit"] == 11764 + 0x10
    assert s["uis"] == 0x1b28c + 11764 + 21536


# ----------------------------------------------------------------------------
# the Q3 VM interpreter the behaviour gate runs on
# ----------------------------------------------------------------------------
def test_qvmsim_calling_convention_and_syscalls():
    vm = qvmsim.QVM(make_qvm(PROG_CALL))
    seen = []

    def sys_(vm_, num, base):
        seen.append((num, vm_.rd32(base + 4), vm_.rd32(base + 8)))
        return 100

    assert vm.call(0, [0, 5], sys_) == 16                 # 5*3+1
    assert vm.call(0, [1, 7, 9], sys_) == 100
    assert seen == [(5, 7, 9)]                            # trap -6 is call number 5
    assert vm.stack_top == len(vm.mem)                    # the stack is restored


@pytest.mark.parametrize("a,b,q,r", [(7, 2, 3, 1), (-7, 2, -3, -1), (7, -2, -3, 1), (-7, -2, 3, -1)])
def test_qvmsim_integer_division_truncates_like_c(a, b, q, r):
    prog = [("ENTER", 8), ("LOCAL", 16), ("LOAD4", 0), ("CONST", 0), ("NE", 11),
            ("LOCAL", 20), ("LOAD4", 0), ("LOCAL", 24), ("LOAD4", 0), ("DIVI", 0), ("LEAVE", 8),
            ("LOCAL", 20), ("LOAD4", 0), ("LOCAL", 24), ("LOAD4", 0), ("MODI", 0), ("LEAVE", 8)]
    vm = qvmsim.QVM(make_qvm(prog))
    assert vm.call(0, [0, a, b], None) == q
    assert vm.call(0, [1, a, b], None) == r


def test_qvmsim_float_ops():
    # (int)((float)a0 * 1.5f): truncation toward zero
    prog = [("ENTER", 8), ("LOCAL", 20), ("LOAD4", 0), ("CVIF", 0), ("CONST", f2i(1.5)),
            ("MULF", 0), ("CVFI", 0), ("LEAVE", 8)]
    vm = qvmsim.QVM(make_qvm(prog))
    assert vm.call(0, [0, 3], None) == 4
    assert vm.call(0, [0, -3], None) == -4


def test_qvmsim_refuses_a_ver2_image():
    with pytest.raises(qvmsim.VMError):
        qvmsim.QVM(make_qvm([("ENTER", 8), ("LEAVE", 8)], magic=0x12721445, jtrg=b"\0" * 4))


# ----------------------------------------------------------------------------
# ui_fleetres.h, compiled natively from the file the QVM is built from
# ----------------------------------------------------------------------------
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "ui_fleetres.h"
static frModeList_t L;
int main(int argc, char **argv) {
	int i, w, h, m;
	FR_Build(&L, argv[1]);
	FR_Select(&L, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
	printf("{\"detected\":%d,\"count\":%d,\"current\":%d,\"entries\":[", L.detected, L.count, L.current);
	for (i = 0; i < L.count; i++)
		printf("%s[\"%s\",%d,%d,%d]", i ? "," : "", L.names[i], L.mode[i], L.width[i], L.height[i]);
	printf("],\"terminated\":%d,\"ops\":[", L.names[L.count] == 0);
	for (i = 7; i < argc; i++) {
		int arg = atoi(argv[i] + 2);
		if (argv[i][0] == 'a') {			/* a:<entry>  what ACCEPT writes */
			m = FR_Accept(&L, arg, &w, &h);
			printf("%s[%d,%d,%d]", i > 7 ? "," : "", m, w, h);
		} else if (argv[i][0] == 'p') {		/* p:<mode>   a Graphics Settings preset */
			m = FR_PresetIndex(&L, arg);
			printf("%s[%d,%d]", i > 7 ? "," : "", m, L.count);
		} else {							/* m:<entry>  FR_ModeOf */
			printf("%s[%d]", i > 7 ? "," : "", FR_ModeOf(&L, arg));
		}
	}
	printf("]}\n");
	return 0;
}
'''


@pytest.fixture(scope="module")
def fr(tmp_path_factory):
    cc = apply.host_cc()
    if not cc:
        pytest.skip("SKIPPED LOUDLY: no host C compiler ($CC/gcc/cc/clang) - ui_fleetres.h "
                    "was NOT compiled or tested")
    d = tmp_path_factory.mktemp("fr")
    shutil.copyfile(apply.HEADER_FILE, d / "ui_fleetres.h")
    (d / "t.c").write_text(HARNESS)
    exe = d / "t"
    # C89 + pedantic errors: the header must stay lcc-compatible (no // comments,
    # no mixed declarations) - lcc is the compiler the QVM is really built with
    r = subprocess.run([cc, "-std=c89", "-pedantic-errors", "-Wall", "-Wno-unused-function",
                        "-o", str(exe), str(d / "t.c")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    def run(modes, rmode, cw=0, ch=0, vw=0, vh=0, *ops):
        out = subprocess.run([str(exe), modes, str(rmode), str(cw), str(ch), str(vw), str(vh)] + list(ops),
                             capture_output=True, text=True, check=True).stdout
        return json.loads(out)
    return run


ID_TABLE = ["320x240", "400x300", "512x384", "640x480", "800x600", "960x720", "1024x768",
            "1152x864", "1280x1024", "1600x1200", "2048x1536", "856x480 wide screen"]
IOQ3 = "1920x1080 1680x1050 1600x1200 1280x1024 1024x768 800x600 640x480"


def test_retail_lcd_lists_and_selects_1920x1080(fr):
    r = fr("", -1, 1920, 1080, 1920, 1080, "a:12", "a:3", "m:12", "m:3")
    names = [e[0] for e in r["entries"]]
    assert r["detected"] == 0 and r["terminated"] == 1
    assert names[:12] == ID_TABLE, "id's table, in id's order: entry N is r_mode N"
    assert [e[1] for e in r["entries"][:12]] == list(range(12))
    # OLD (id 1.32): r_mode -1 selected entry 3 "640x480" and ACCEPT wrote r_mode 3
    assert names[3] == "640x480"
    # NEW: the running mode is appended and selected; ACCEPT writes it back
    assert r["count"] == 13 and r["current"] == 12 and names[12] == "1920x1080"
    assert r["entries"][12][1:] == [-1, 1920, 1080]
    assert r["ops"][0] == [-1, 1920, 1080]
    assert r["ops"][1] == [3, 640, 480], "any OTHER entry is id's r_mode"
    assert r["ops"][2:] == [[-1], [3]]


def test_ioquake3_lists_the_driver_modes(fr):
    r = fr(IOQ3, -1, 1920, 1080, 1920, 1080, "a:0", "a:1", "a:4", "a:5")
    assert r["detected"] == 1 and r["count"] == 7 and r["current"] == 0
    assert [e[0] for e in r["entries"]] == IOQ3.split()
    # a driver mode that is one of id's sizes carries id's r_mode (ioquake3's own UI does the same)
    assert [e[1] for e in r["entries"]] == [-1, -1, 9, 8, 6, 4, 3]
    assert r["ops"] == [[-1, 1920, 1080], [-1, 1680, 1050], [6, 1024, 768], [4, 800, 600]]


def test_the_running_mode_is_appended_when_the_driver_list_lacks_it(fr):
    r = fr("1680x1050 1280x1024", -1, 1920, 1080, 1920, 1080, "a:2")
    assert [e[0] for e in r["entries"]] == ["1680x1050", "1280x1024", "1920x1080"]
    assert r["current"] == 2 and r["ops"] == [[-1, 1920, 1080]]


def test_crt_at_an_id_size_keeps_r_mode_minus_1_when_untouched(fr):
    # .124-style tube: r_mode -1 at 1024x768 selects id's own 1024x768 entry...
    r = fr("", -1, 1024, 768, 1024, 768, "a:6", "a:4")
    assert r["count"] == 12 and r["current"] == 6
    # ...and an ACCEPT that did not touch Video Mode writes back what was running
    assert r["ops"][0] == [-1, 1024, 768]
    assert r["ops"][1] == [4, 800, 600]


def test_a_plain_id_mode_behaves_exactly_like_id(fr):
    r = fr("", 6, 1600, 1024, 1024, 768, "a:6", "a:5")
    assert r["count"] == 12 and r["current"] == 6
    assert r["ops"] == [[6, 1024, 768], [5, 960, 720]]
    r = fr("", 11, 0, 0, 856, 480, "a:11")
    assert r["current"] == 11 and r["ops"] == [[11, 856, 480]]
    # ioquake3 at r_mode 11 when the driver does not offer 856x480: id's label is appended
    r = fr(IOQ3, 11, 1920, 1080, 856, 480, "a:7")
    assert r["entries"][7] == ["856x480 wide screen", 11, 856, 480] and r["current"] == 7
    assert r["ops"] == [[11, 856, 480]]


def test_an_engine_mode_outside_ids_table_is_written_back_unchanged(fr):
    # e.g. Quake3e's r_mode -2 (desktop): shown by the renderer's size, ACCEPT keeps -2
    r = fr("", -2, 0, 0, 1366, 768, "a:12")
    assert [e[0] for e in r["entries"]][12] == "1366x768" and r["current"] == 12
    assert r["ops"] == [[-2, 1366, 768]]
    # r_mode -1 with no usable custom size and no renderer size: nothing invented
    r = fr("", -1, 0, 0, 0, 0, "a:0")
    assert r["current"] == 0 and r["ops"] == [[-1, 0, 0]]


def test_presets_map_to_an_entry_of_their_size(fr):
    r = fr(IOQ3, -1, 1920, 1080, 1920, 1080, "p:4", "p:3", "p:2", "p:2")
    # 800x600 and 640x480 exist; 512x384 (the "Fast" presets) is appended once
    assert r["ops"] == [[5, 7], [6, 7], [7, 8], [7, 8]]
    r = fr("", -1, 1920, 1080, 1920, 1080, "p:4", "p:99", "p:-1")
    assert r["ops"] == [[4, 13], [-1, 13], [-1, 13]]


def test_bad_input_is_tolerated(fr):
    many = " ".join("%dx%d" % (100 + i, 100 + i) for i in range(40))
    r = fr(many, -1, 1920, 1080, 1920, 1080, "a:32", "a:99", "m:99")
    assert r["count"] == 33, "32 driver modes + the running one"
    assert r["current"] == 32 and r["entries"][32][0] == "1920x1080"
    assert r["ops"][0] == [-1, 1920, 1080]
    assert r["ops"][1] == [-1, 1920, 1080], "an out-of-range entry means the running one"
    assert r["ops"][2] == [-1000], "FR_NO_ENTRY is never a real r_mode"
    r = fr("  junk 1x 1920x1080  x5 800x600 99999999x5 ", -1, 1920, 1080, 1920, 1080)
    assert [e[0] for e in r["entries"]] == ["1920x1080", "800x600"]


# ----------------------------------------------------------------------------
# the patch file and apply.py's recorded facts
# ----------------------------------------------------------------------------
def _hunks(text):
    files, hunks = [], []
    for line in text.splitlines():
        if line.startswith("+++ ") or line.startswith("--- "):
            files.append(line[4:].strip())
        elif line.startswith("@@"):
            hunks.append([line])
        elif hunks and line[:1] in (" ", "+", "-"):
            hunks[-1].append(line)
    return files, hunks


def test_patch_touches_only_ui_video_and_is_well_formed():
    text = open(apply.PATCH_FILE).read()
    files, hunks = _hunks(text)
    assert files == ["a/code/q3_ui/ui_video.c", "b/code/q3_ui/ui_video.c"]
    assert hunks
    for h in hunks:
        # "@@ -21,6 +21,7 @@"; diff omits ",N" when N is 1
        old_n, new_n = [int(x.split(",")[1]) if "," in x else 1 for x in h[0].split()[1:3]]
        body = h[1:]
        assert sum(1 for l in body if l[:1] in " -") == old_n, h[0]
        assert sum(1 for l in body if l[:1] in " +") == new_n, h[0]
    removed = [l[1:] for h in hunks for l in h[1:] if l.startswith("-")]
    added = [l[1:] for h in hunks for l in h[1:] if l.startswith("+")]
    # id's two lines that ARE the defect are gone...
    assert '\ttrap_Cvar_SetValue( "r_mode", s_graphicsoptions.mode.curvalue );' in removed
    assert "\t\ts_graphicsoptions.mode.curvalue = 3;" in removed
    # ...and the module is wired in
    assert any('#include "ui_fleetres.h"' in l for l in added)
    assert any("FR_Accept( &s_fr" in l for l in added)
    assert any('"r_availableModes"' in l for l in added)
    # CRLF churn would break git apply on the LF source
    assert "\r" not in text


def test_apply_records_the_release_recipe():
    assert apply.Q3ASM_ORDER[0] == "ui_main", "vmMain's file must link first"
    assert apply.Q3ASM_ORDER[-1].endswith("ui_syscalls")
    for unit in ("ui_loadconfig", "ui_saveconfig", "ui_video", "bg_lib"):
        assert unit in apply.Q3ASM_ORDER, "pak8 contains %s" % unit
    assert len(apply.Q3ASM_ORDER) == len(set(apply.Q3ASM_ORDER)) == 44
    old, new = apply.RELEASE_132_TR_TYPES
    assert "Q3_VM" in old and "Q3_VM" not in new
    assert apply.PAK8_UI_MD5 == "3e6b8fadb2a970c2b754e48e63a054d7"
    assert apply.PAK8_UI_HEADER[0] == 0x12721444 and apply.PAK8_UI_HEADER[2] == 32
    assert apply.PAK8_UI_STRINGS[0x43d0d] == "640x480"
    assert "r_availableModes" in apply.ENGINES["quake3.exe"]["absent"]
    assert apply.ENGINES["ioquake3.x86.exe"]["strings"][0x135308] == "r_availableModes"
    for q in ("-vq3",):
        assert q in open(os.path.join(PATCH_DIR, "apply.py")).read()


def test_share_paths_and_backups():
    assert apply.BACKUP_REL == "Files/Games-Library/_patches/Quake3-TeamArena/originals-2026-09-29"
    pk3_dest = "Files/Games-Library/Quake3-TeamArena/baseq3/zz-fleetres-ui.pk3"
    assert apply.backup_rel(pk3_dest) == apply.BACKUP_REL + "/baseq3/zz-fleetres-ui.pk3"
    src = apply.SOURCE_REL + "/ui_fleetres.h"
    assert apply.backup_rel(src) == apply.BACKUP_REL + "/zz-fleetres-ui-source/ui_fleetres.h"
    with pytest.raises(ValueError):
        apply.backup_rel("Files/Games-Library/Other/x.pk3")
    for p in (pk3_dest, apply.SOURCE_REL, apply.BACKUP_REL):
        assert "(" not in p and ")" not in p
    # the source bundle goes under _patches, which GAMESYNC never copies to a box
    assert apply.SOURCE_REL.startswith("Files/Games-Library/_patches/")


def test_behaviour_table_encodes_the_measured_defect():
    """The stock rows of the behaviour gate are the HARDWARE measurement
    (.123/.145/.195/.240): "640x480" shown, ACCEPT writes r_mode 3."""
    lcd = [s for s in apply.BEHAVIOUR if s["cvars"].get("r_customwidth") == "1920" and "stock" in s]
    assert len(lcd) == 2
    for s in lcd:
        assert s["stock"] == {"label": "640x480", "r_mode": "3"}
        assert s["patched"]["label"] == "1920x1080" and s["patched"]["r_mode"] == "-1"
    assert any("r_availableModes" in s["cvars"] for s in apply.BEHAVIOUR)
    assert any("r_availableModes" not in s["cvars"] for s in apply.BEHAVIOUR)


# ----------------------------------------------------------------------------
# --publish / --install-server, against a FAKE share and a FAKE server dir
# (the real ones are never touched by the test suite)
# ----------------------------------------------------------------------------
@pytest.fixture
def fake_out(tmp_path, monkeypatch):
    """An OUTDIR with a manifest like --build writes: two source files, then the pk3."""
    out = tmp_path / "out"
    (out / "source").mkdir(parents=True)
    (out / "baseq3").mkdir()
    files = []
    for name, data in (("ui_fleetres.h", b"/* h */\n"), ("SOURCE.txt", b"src\r\n")):
        p = out / "source" / name
        p.write_bytes(data)
        files.append({"share_path": apply.SOURCE_REL + "/" + name, "local_path": str(p),
                      "md5": apply.md5_bytes(data), "size": len(data), "kind": "source"})
    pk3 = apply.make_pk3(make_qvm([("ENTER", 8), ("CONST", 7), ("LEAVE", 8)]))
    pp = out / "baseq3" / apply.PK3_NAME
    pp.write_bytes(pk3)
    files.append({"share_path": "%s/%s/%s" % (apply.SHARE_LIB_REL, apply.TITLE, apply.PK3_REL),
                  "local_path": str(pp), "md5": apply.md5_bytes(pk3), "size": len(pk3), "kind": "pk3"})
    (out / "manifest.json").write_text(json.dumps(
        {"outputs": files, "pk3": {"local_path": str(pp), "md5": apply.md5_bytes(pk3), "size": len(pk3)}}))
    share = tmp_path / "share"
    share.mkdir()
    monkeypatch.setattr(apply, "MNT", str(share))
    monkeypatch.setattr(apply, "check", lambda *a, **k: (True, {}))
    puts = []

    def fake_sharewrite(local, dest, dry_run):
        puts.append((dest, dry_run))
        if dest == fake_sharewrite.fail_on:
            return 2
        if not dry_run:
            target = share.joinpath(*dest.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(local, str(target))
        return 0
    fake_sharewrite.fail_on = None
    monkeypatch.setattr(apply, "sharewrite", fake_sharewrite)
    return {"out": str(out), "share": share, "puts": puts, "files": files, "sw": fake_sharewrite,
            "pk3": pk3}


def test_publish_puts_sources_first_and_the_pk3_last(fake_out):
    apply.publish(fake_out["out"], dry_run=False)
    dests = [d for d, _ in fake_out["puts"]]
    assert dests == [f["share_path"] for f in fake_out["files"]]
    assert dests[-1].endswith("/baseq3/" + apply.PK3_NAME)
    # run again: everything is current, nothing is written (idempotent)
    del fake_out["puts"][:]
    apply.publish(fake_out["out"], dry_run=False)
    assert fake_out["puts"] == []


def test_publish_backs_up_a_different_copy_before_replacing_it(fake_out):
    pk3_dest = fake_out["files"][-1]["share_path"]
    old = fake_out["share"].joinpath(*pk3_dest.split("/"))
    old.parent.mkdir(parents=True)
    old.write_bytes(b"an older build")
    apply.publish(fake_out["out"], dry_run=False)
    dests = [d for d, _ in fake_out["puts"]]
    bak = apply.BACKUP_REL + "/baseq3/" + apply.PK3_NAME
    assert dests.index(bak) == dests.index(pk3_dest) - 1, "backup immediately before the replace"
    assert fake_out["share"].joinpath(*bak.split("/")).read_bytes() == b"an older build"


def test_publish_stops_at_the_first_failure(fake_out):
    first = fake_out["files"][0]["share_path"]
    fake_out["sw"].fail_on = first
    with pytest.raises(SystemExit):
        apply.publish(fake_out["out"], dry_run=False)
    assert [d for d, _ in fake_out["puts"]] == [first], "nothing after a failed put is attempted"


def test_publish_dry_run_writes_nothing(fake_out):
    apply.publish(fake_out["out"], dry_run=True)
    assert all(dry for _, dry in fake_out["puts"])
    assert not any(fake_out["share"].rglob("*.pk3"))


def test_publish_refuses_a_manifest_that_no_longer_matches(fake_out):
    with open(fake_out["files"][-1]["local_path"], "ab") as f:
        f.write(b"x")
    with pytest.raises(SystemExit):
        apply.publish(fake_out["out"], dry_run=False)
    assert fake_out["puts"] == []


def _fake_fleet(tmp_path, monkeypatch, records):
    """retro-autodeploy's state file, FAKE - the real ~/.retro-fleet one is never read here."""
    p = tmp_path / "autodeploy.json"
    p.write_text(json.dumps(records))
    monkeypatch.setattr(apply, "AUTODEPLOY_STATE", str(p))
    return p


def _at(offset_s):
    import time
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(time.time() + offset_s))


def test_install_server_refuses_before_the_library_has_the_pk3(fake_out, tmp_path, monkeypatch):
    srv = tmp_path / "srv"
    srv.mkdir()
    (srv / "pak8.pk3").write_bytes(b"pak8")
    monkeypatch.setattr(apply, "server_baseq3", lambda: str(srv))   # never the real server
    monkeypatch.setattr(apply, "PAK8_MD5", apply.md5_bytes(b"pak8"))
    _fake_fleet(tmp_path, monkeypatch, {"DELL": {"hostname": "DELL", "ip": "192.168.1.145",
                                                 "at": _at(3600)}})
    # the library does not carry it yet: a pure server listing it would drop every client
    with pytest.raises(SystemExit) as e:
        apply.install_server(fake_out["out"], dry_run=False, force=False)
    assert "REFUSED" in str(e.value)
    assert not (srv / apply.PK3_NAME).exists()
    # published: dry-run changes nothing, a real run installs a verified copy
    apply.publish(fake_out["out"], dry_run=False)
    apply.install_server(fake_out["out"], dry_run=True, force=False)
    assert not (srv / apply.PK3_NAME).exists()
    apply.install_server(fake_out["out"], dry_run=False, force=False)
    assert (srv / apply.PK3_NAME).read_bytes() == fake_out["pk3"]
    assert not list(srv.glob("*.tmp"))


def test_install_server_refuses_while_a_box_has_not_synced_since_the_publish(fake_out, tmp_path,
                                                                            monkeypatch):
    """The library having the pk3 is not the boxes having it: a pure server that
    lists it drops every box still without it ("Unpure Client ... set
    cl_allowDownload 1", and the fleet leaves that at 0). The gate reads
    retro-autodeploy's records and refuses while one predates the publish."""
    srv = tmp_path / "srv"
    srv.mkdir()
    (srv / "pak8.pk3").write_bytes(b"pak8")
    monkeypatch.setattr(apply, "server_baseq3", lambda: str(srv))   # never the real server
    monkeypatch.setattr(apply, "PAK8_MD5", apply.md5_bytes(b"pak8"))
    apply.publish(fake_out["out"], dry_run=False)                   # into the FAKE share
    _fake_fleet(tmp_path, monkeypatch, {
        "DELL": {"hostname": "DELL", "ip": "192.168.1.145", "at": _at(3600)},
        "NSC-B20C188E96D": {"hostname": "NSC-B20C188E96D", "ip": "192.168.1.123",
                            "at": "2026-09-28 15:02:07"},             # synced BEFORE the publish
    })
    with pytest.raises(SystemExit) as e:
        apply.install_server(fake_out["out"], dry_run=False, force=False)
    assert "REFUSED" in str(e.value) and "NSC-B20C188E96D" in str(e.value)
    assert "DELL" not in str(e.value).split("published:")[1].split(" - ")[0]
    assert not (srv / apply.PK3_NAME).exists()
    # no records, or an unreadable file, is NOT "ready"
    _fake_fleet(tmp_path, monkeypatch, {})
    with pytest.raises(SystemExit):
        apply.install_server(fake_out["out"], dry_run=False, force=False)
    monkeypatch.setattr(apply, "AUTODEPLOY_STATE", str(tmp_path / "absent.json"))
    with pytest.raises(SystemExit):
        apply.install_server(fake_out["out"], dry_run=False, force=False)
    assert not (srv / apply.PK3_NAME).exists()
    # --force installs anyway, after printing the list
    apply.install_server(fake_out["out"], dry_run=False, force=True)
    assert (srv / apply.PK3_NAME).read_bytes() == fake_out["pk3"]


def test_fleet_readiness_reads_autodeploy_records(tmp_path, monkeypatch):
    import time
    p = _fake_fleet(tmp_path, monkeypatch, {
        "DELL": {"hostname": "DELL", "ip": "192.168.1.145", "at": "2026-09-29 06:00:00"},
        "ADMIN-PC": {"hostname": "ADMIN-PC", "ip": "192.168.1.195", "at": "2026-09-29 04:00:00"},
        # a legacy address-keyed record the hostname record above now covers: left out
        "192.168.1.145": {"at": "2026-09-01 00:00:00", "titles": []},
        # a legacy record nobody re-keyed yet: an unknown box, so NOT ready
        "192.168.1.143": {"at": "2026-09-26 16:24:17", "titles": []},
        "BROKEN": {"hostname": "BROKEN", "ip": "192.168.1.9", "at": "yesterday"},
    })
    t = time.mktime(time.strptime("2026-09-29 05:00:00", "%Y-%m-%d %H:%M:%S"))
    rows, err = apply.fleet_readiness(t, str(p))
    assert err is None
    got = {box: ready for box, ip, at, ready in rows}
    assert got == {"DELL": True, "ADMIN-PC": False, "192.168.1.143": False, "BROKEN": False}
    rows, err = apply.fleet_readiness(t, str(tmp_path / "absent.json"))
    assert rows == [] and err


def test_publish_never_overwrites_a_backup(fake_out):
    """The first backup at the canonical path may be the only copy of an
    original: a later replace keeps its copy beside it, never on top of it."""
    pk3_dest = fake_out["files"][-1]["share_path"]
    live = fake_out["share"].joinpath(*pk3_dest.split("/"))
    bak = apply.BACKUP_REL + "/baseq3/" + apply.PK3_NAME
    bak_path = fake_out["share"].joinpath(*bak.split("/"))
    live.parent.mkdir(parents=True)
    live.write_bytes(b"the original")
    apply.publish(fake_out["out"], dry_run=False)
    assert bak_path.read_bytes() == b"the original"
    live.write_bytes(b"a later build")            # somebody replaced it since
    apply.publish(fake_out["out"], dry_run=False)
    assert bak_path.read_bytes() == b"the original", "the first backup was overwritten"
    second = bak_path.parent / ("%s.%s" % (apply.PK3_NAME, apply.md5_bytes(b"a later build")[:8]))
    assert second.read_bytes() == b"a later build"
    assert live.read_bytes() == fake_out["pk3"]


def test_server_baseq3_comes_from_the_units_fs_homepath(tmp_path, monkeypatch):
    unit = tmp_path / "quake3-server.service"
    unit.write_text("[Service]\nWorkingDirectory=/x\nExecStart=/usr/lib/ioquake3/ioq3ded +set "
                    "fs_homepath /srv/q3/.q3a +set net_port 27961 +exec server.cfg\n")
    monkeypatch.setattr(apply, "SERVER_UNIT_FILE", str(unit))
    assert apply.server_baseq3() == "/srv/q3/.q3a/baseq3"
    monkeypatch.setattr(apply, "SERVER_UNIT_FILE", str(tmp_path / "absent.service"))
    assert apply.server_baseq3() == apply.SERVER_BASEQ3


def test_install_server_refuses_a_foreign_pak8(fake_out, tmp_path, monkeypatch):
    srv = tmp_path / "srv"
    srv.mkdir()
    (srv / "pak8.pk3").write_bytes(b"not id's pak8")
    monkeypatch.setattr(apply, "server_baseq3", lambda: str(srv))   # never the real server
    with pytest.raises(SystemExit):
        apply.install_server(fake_out["out"], dry_run=False, force=True)
    assert not (srv / apply.PK3_NAME).exists()


# ----------------------------------------------------------------------------
# needs the cached GPL source (fetched by --build)
# ----------------------------------------------------------------------------
def test_patch_applies_to_the_pinned_id_source(tmp_path):
    repo = os.path.join(apply.SRC_CACHE, "id-q3a")
    if not os.path.isdir(os.path.join(repo, ".git")) or not shutil.which("git"):
        pytest.skip("SKIPPED LOUDLY: %s (or git) absent - the patch was NOT applied to id's "
                    "source; run apply.py --build once" % repo)
    apply.export_tree(repo, apply.ID_COMMIT, ["code/q3_ui/ui_video.c"], str(tmp_path))
    r = subprocess.run(["git", "apply", "--check", "-p1", apply.PATCH_FILE], cwd=str(tmp_path),
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


# ----------------------------------------------------------------------------
# needs a --build output (outside git: ~/.retro-fleet/patch-out/q3-baseq3-ui)
# ----------------------------------------------------------------------------
def _manifest():
    if not os.path.isfile(MANIFEST):
        pytest.skip("SKIPPED LOUDLY: %s absent - the built pk3/QVM were NOT checked; "
                    "run apply.py --build" % MANIFEST)
    return json.load(open(MANIFEST))


def test_built_pk3_matches_its_manifest_and_format():
    m = _manifest()
    pk3 = open(m["pk3"]["local_path"], "rb").read()
    assert apply.md5_bytes(pk3) == m["pk3"]["md5"]
    assert apply.pk3_problems(pk3) == []
    qvm = zipfile.ZipFile(io.BytesIO(pk3)).read("vm/ui.qvm")
    assert apply.md5_bytes(qvm) == m["qvm"]["md5"]
    h = apply.qvm_header(qvm)
    assert h["vmMagic"] & 0xffffffff == 0x12721444 and h["codeOffset"] == 32
    assert len(qvm) == h["dataOffset"] + h["dataLength"] + h["litLength"], "no jump table"
    lit = set(apply.qvm_lit_strings(qvm))
    for s in apply.NEW_STRINGS + list(apply.PAK8_UI_STRINGS.values()):
        assert s in lit
    g = m["gates"]
    assert g["reproduction"]["identical"] and g["reproduction"]["stock_md5"] == apply.PAK8_UI_MD5
    assert g["drift"]["asm_changed"] == ["ui_video"]
    assert g["drift"]["functions_compared"] > 500
    assert [o["kind"] for o in m["outputs"]][-1] == "pk3", "the pk3 is published LAST"


def test_built_qvms_behave_in_the_menu():
    """Re-run the behaviour gate on the built stock (== pak8) and patched QVMs."""
    _manifest()
    builds = {}
    for kind in ("stock", "patched"):
        vm_dir = os.path.join(BUILD, kind, "code", "q3_ui", "vm")
        if not os.path.isfile(os.path.join(vm_dir, "ui.map")):
            pytest.skip("SKIPPED LOUDLY: %s has no ui.map - behaviour NOT re-checked" % vm_dir)
        builds[kind] = (open(os.path.join(vm_dir, "ui.qvm"), "rb").read(),
                        open(os.path.join(vm_dir, "ui.map")).read(), {})
    assert apply.md5_bytes(builds["stock"][0]) == apply.PAK8_UI_MD5
    rows = apply.behaviour_gate(builds["stock"], builds["patched"])
    assert len(rows) == len(apply.BEHAVIOUR)
    first = rows[0]
    assert first["stock"]["label"] == "640x480" and first["stock"]["r_mode"] == "3"
    assert first["patched"]["label"] == "1920x1080" and first["patched"]["r_mode"] == "-1"


# ----------------------------------------------------------------------------
# needs the NAS share (read-only /mnt)
# ----------------------------------------------------------------------------
def test_the_staged_originals_are_what_the_patch_was_built_on():
    if not os.path.isdir(LIBRARY):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - the staged pak8/engines were NOT checked"
                    % LIBRARY)
    ok, rep = apply.check(LIBRARY, verbose=False)
    assert ok, rep["problems"]
    assert rep["facts"]["pak8_ui"]["md5"] == apply.PAK8_UI_MD5
    later = [p for p in rep["facts"]["baseq3_pk3s"]
             if apply.pk3_order_key(p) > apply.pk3_order_key(apply.PK3_NAME)]
    assert later == []
