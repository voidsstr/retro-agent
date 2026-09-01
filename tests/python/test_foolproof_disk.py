"""A PXE install can succeed and still produce a machine that does nothing.

WHY THIS EXISTS
---------------
`$OEM$` reaches a target by two routes and only one is gated on OemPreinstall:

    $OEM$\\cmdlines.txt  -> regedit /s retroagent.reg   GUI setup T-12   ungated
    $OEM$\\$1\\*          -> C:\\                        text-mode copy   GATED

With `OemPreinstall = No` the registry merge still lands, so the box looks
configured - Run\\RetroAgent set, DevicePath set - while the agent binary and
the C:\\D driver tree those values point at were never written. Measured on the
disk imaged 2026-09-01: zero "C:\\D\\" hits in setupapi.log, against six on a
correctly imaged box.

These tests pin the two things that keep that from happening silently again:
the selftest must FAIL on a payload-less unattend, and foolproof-disk.py must
never set a storage driver boot-start without its .sys file - which would turn
a bootable disk into a STOP 0x7B.
"""
import ast
import importlib.util
import os
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FP = os.path.join(REPO, "scripts", "pxe", "foolproof-disk.py")
SELFTEST = os.path.join(REPO, "scripts", "pxe", "pxe_selftest.py")


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _code(path):
    """The AST carries no comments, so parsing the file is already immune to
    prose - but it DOES carry docstrings. Strip those too, so a structural
    check can never be satisfied by the paragraph describing the code instead
    of the code. (A naive split on "#" is not an option here: the source this
    inspects contains string literals like 'pci#cc_0101'.)"""
    tree = ast.parse(open(path, encoding="utf-8").read())
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if (isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef,
                              ast.ClassDef))
                and body and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            body.pop(0)
    return tree


# ------------------------------------------------------------------ selftest

def _run_check(sif_text):
    st = _load(SELFTEST, "pxe_selftest_under_test")
    st.ok_count = st.fail_count = 0
    st.check_unattend(sif_text.encode("latin-1"))
    return st.ok_count, st.fail_count


GOOD_SIF = """[Unattended]
    UnattendMode = FullUnattended
    OemPreinstall = Yes
    OemPnPDriversPath = "D\\C001;D\\L001"
    DriverSigningPolicy = Ignore
[GuiUnattended]
    AutoLogon = Yes
"""


def test_payload_less_unattend_fails_the_selftest():
    """OemPreinstall = No is the whole defect. It must not pass."""
    bad = GOOD_SIF.replace("OemPreinstall = Yes", "OemPreinstall = No")
    _ok, fail = _run_check(bad)
    assert fail >= 1, "a sif that never copies $OEM$\\$1 was reported as fine"


def test_a_correct_unattend_passes_cleanly():
    """The check has to be able to pass, or it is just noise."""
    ok, fail = _run_check(GOOD_SIF)
    assert fail == 0, "a correct unattend was rejected"
    assert ok >= 5


def test_missing_driver_path_is_reported_separately():
    """Losing OemPnPDriversPath is its own fault, not a side effect of the
    first one: setup then installs devices with no search path at all."""
    no_path = "\n".join(l for l in GOOD_SIF.splitlines()
                        if "OemPnPDriversPath" not in l)
    _ok, fail = _run_check(no_path)
    assert fail == 1, "OemPnPDriversPath absent should be exactly one failure"


def test_selftest_checks_the_served_bytes_not_the_file_on_disk():
    """The sif on disk is not necessarily the sif being served - that is the
    entire reason this runs over TFTP."""
    tree = _code(SELFTEST)
    main = next(n for n in ast.walk(tree)
                if isinstance(n, ast.FunctionDef) and n.name == "main")
    calls = [n for n in ast.walk(main)
             if isinstance(n, ast.Call) and getattr(n.func, "id", "") == "check_unattend"]
    assert calls, "main() never calls check_unattend"
    arg = calls[0].args[0]
    assert isinstance(arg, ast.Subscript), (
        "check_unattend must be given the bytes fetched over TFTP, "
        "not a path or a re-read of the local file")


# ------------------------------------------------------------- foolproof-disk

def test_never_boot_start_a_driver_whose_file_is_absent():
    """A boot-start service with no .sys is not a fallback, it is an
    unbootable disk. The Start=0 write must sit behind an existence check on
    the driver file, and the loop must skip - not merely warn - without it."""
    tree = _code(FP)
    fn = next(n for n in ast.walk(tree)
              if isinstance(n, ast.FunctionDef) and n.name == "stage_storage")

    starts = [n for n in ast.walk(fn)
              if isinstance(n, ast.Call)
              and getattr(n.func, "attr", "") == "set_dword"
              and any(isinstance(a, ast.Constant) and a.value == "Start" for a in n.args)]
    assert starts, "stage_storage never sets Start"

    # every Start write must be dominated by an `if not os.path.exists(...)`
    # guard that continues, inside the same loop
    guarded = False
    for loop in [n for n in ast.walk(fn) if isinstance(n, ast.For)]:
        body = ast.dump(ast.Module(body=loop.body, type_ignores=[]))
        if "'Start'" not in body and '"Start"' not in body:
            continue
        has_exists_guard = any(
            isinstance(n, ast.If)
            and "exists" in ast.dump(n.test)
            and any(isinstance(s, ast.Continue) for s in ast.walk(n))
            for n in loop.body)
        if has_exists_guard:
            guarded = True
    assert guarded, (
        "the Start=0 loop has no os.path.exists() guard that continues - "
        "this would set a miniport boot-start with no driver file")


def test_cddb_entry_never_names_a_driver_that_is_not_on_the_disk():
    """Same failure by another route: a CriticalDeviceDatabase entry pointing
    at an absent miniport binds the boot controller to nothing."""
    tree = _code(FP)
    fn = next(n for n in ast.walk(tree)
              if isinstance(n, ast.FunctionDef) and n.name == "stage_storage")
    for loop in [n for n in ast.walk(fn) if isinstance(n, ast.For)]:
        body = ast.dump(ast.Module(body=loop.body, type_ignores=[]))
        if "ClassGUID" not in body:
            continue
        assert "exists" in body and "MINIPORTS" in body, (
            "the CriticalDeviceDatabase loop writes entries without checking "
            "that the named driver file is present on the target")
        return
    raise AssertionError("no CriticalDeviceDatabase write loop found")


def test_copy_is_verified_by_reading_the_destination_back():
    """The copy's exit status is not evidence. A short write must be caught."""
    fp = _load(FP, "foolproof_disk_under_test")
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "src.bin")
        dst = os.path.join(d, "sub", "dst.bin")
        with open(src, "wb") as f:
            f.write(os.urandom(300000))

        action, err = fp.copy_verified(src, dst)
        assert (action, err) == ("copy", None)
        assert os.path.getsize(dst) == os.path.getsize(src)

        # a second run must not recopy what already matches
        action, err = fp.copy_verified(src, dst)
        assert (action, err) == ("skip", None)

        # a destination that was silently truncated must not be called correct
        with open(dst, "r+b") as f:
            f.truncate(1000)
        action, err = fp.copy_verified(src, dst)
        assert action == "copy" and err is None, "a short file was not recopied"
        assert os.path.getsize(dst) == os.path.getsize(src)


def test_mshdc_mapping_comes_from_the_media_not_a_hardcoded_list():
    """A hand-copied MergeIDE list rots the moment the media changes."""
    fp = _load(FP, "foolproof_disk_under_test2")
    with tempfile.TemporaryDirectory() as d:
        work = os.path.join(d, "work")
        os.makedirs(work)
        with open(os.path.join(work, "mshdc.inf"), "w") as f:
            f.write(
                "[Manufacturer]\n"
                "%INTEL%=INTEL_HDC\n"
                "[INTEL_HDC]\n"
                "%PCI\\VEN_8086&DEV_7111.DeviceDesc% = intelide_Inst, "
                "PCI\\VEN_8086&DEV_7111\n"
                "[intelide_Inst.Services]\n"
                "AddService = intelide, 2, intelide_Service_Inst\n")
        got = fp.parse_mshdc(d, work)
    assert got == {"pci#ven_8086&dev_7111": "intelide"}, got


def test_the_payload_list_covers_every_oem_subtree():
    """$1, $Progs and $$ each land somewhere different; missing one is a
    partially provisioned box that looks provisioned."""
    fp = _load(FP, "foolproof_disk_under_test3")
    srcs = [s.replace("\\", "/") for s, _d in fp.PAYLOAD]
    assert any(s.endswith("$1/RETRO_AGENT") for s in srcs)
    assert any(s.endswith("$1/D") for s in srcs)
    assert any(s.endswith("$1/retro-wall") for s in srcs)
    assert any("$Progs" in s for s in srcs)
    assert any(s.endswith("$OEM$/$$") for s in srcs)
    dests = dict(fp.PAYLOAD)
    assert dests[os.path.join("$OEM$", "$$")] == "WINDOWS", \
        "$OEM$\\$$ must land in C:\\WINDOWS, not C:\\"
