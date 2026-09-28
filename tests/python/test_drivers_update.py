"""DRIVERS UPDATE (agent 1.89.0) - "update all of the drivers on the box except
for 3dfx" (user directive 2026-09-27), and the driver-store index it reads.

Three layers, each pinned here:

  * scripts/fleet/drvindex.c indexes the share's driver store with the AGENT'S
    OWN matcher (agent/shared/drvmatch.h) and 3dfx rule (drvsafe.h) - compiled
    and run over a small fake store, so a commented-out id, a 3dfx INF and the
    Adaptec "Voodoo" codename are all decided the way the agent decides them;
  * scripts/fleet/driverstore.py buckets and publishes that index OUTSIDE the
    $OEM$ tree, manifest last;
  * agent/src/gamesync.c + video.c: which devices the command and the
    automatic startup pass may touch (never 3dfx, never display automatically,
    never on a modern Windows host, never on Win9x yet).
"""
import importlib.util
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "agent" / "src"


def read(p):
    return Path(p).read_text(encoding="utf-8", errors="replace")


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.M | re.S)
    assert m, f"{name} not found"
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError(f"unbalanced {name}")


# ---------------------------------------------------------------- the indexer
INF_RTL = (
    '[Version]\r\nSignature="$Windows NT$"\r\nClass=Net\r\nDriverVer=05/01/2004,5.1.2.3\r\n\r\n'
    "[Manufacturer]\r\n%Mfg%=Models\r\n\r\n[Models]\r\n%Dev%=Inst, PCI\\VEN_10EC&DEV_8139\r\n"
    "; %Old%=Inst, PCI\\VEN_10EC&DEV_8129\r\n\r\n"
    '[Strings]\r\nMfg="Realtek"\r\nDev="RTL8139"\r\n'
)
INF_3DFX = (
    '[Version]\r\nSignature="$Windows NT$"\r\nClass=Display\r\n\r\n'
    "[Manufacturer]\r\n%M%=Models\r\n\r\n[Models]\r\n%D%=V, PCI\\VEN_121A&DEV_0009\r\n\r\n"
    '[Strings]\r\nM="3dfx Interactive"\r\nD="Voodoo5"\r\n'
)
# The Adaptec RAID INF that 1.88.x's word list wrongly called 3dfx: its
# codename is "Voodoo". INF text is judged without that word (drvsafe.h).
# nForce: a brace id, a %KEY% id expanded from [Strings], a commented and a
# "DriverVersion" decoy around the real [Version] DriverVer.
INF_NVNET = (
    '[Version]\r\nSignature="$Windows NT$"\r\nClass=Net\r\n; DriverVer=01/01/1999,0.0.0.1\r\n'
    "DriverVer = 03/02/2006, 5.10.2.0\r\n\r\n[Manufacturer]\r\n%M%=Models\r\n\r\n[Models]\r\n"
    "%D%=N, {1A3E09BE-1E45-494B-9174-D7385B45BBF5}\\NVNET_DEV0057, PCI\\VEN_10DE&DEV_0057\r\n"
    "%E%=N, %DEVX%\r\n\r\n"
    '[Strings]\r\nM="NVIDIA"\r\nD="nForce"\r\nE="x"\r\nDEVX="PCI\\VEN_10DE&DEV_0373"\r\nDriverVersion="9.9"\r\n'
)
INF_ADAPTEC = (
    '[Version]\r\nSignature="$Windows NT$"\r\nClass=SCSIAdapter\r\n\r\n'
    "[Manufacturer]\r\n%M%=Models\r\n\r\n[Models]\r\n%D%=R, PCI\\VEN_9005&DEV_0285\r\n\r\n"
    '[Strings]\r\nM="Adaptec"\r\nD="Adaptec 2410SA (Voodoo)"\r\n'
)


@pytest.fixture(scope="module")
def indexed(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("SKIPPED LOUDLY: no host C compiler - drvindex.c is untested")
    d = tmp_path_factory.mktemp("store")
    for sub, name, text in (("A001", "rtl.inf", INF_RTL), ("V001", "voodoo.inf", INF_3DFX),
                            ("R001", "arcsas.inf", INF_ADAPTEC), ("N001", "nvnet.inf", INF_NVNET)):
        (d / sub).mkdir()
        (d / sub / name).write_bytes(text.encode("latin-1"))
    exe = d / "drvindex"
    subprocess.run([cc, "-O2", "-w", "-o", str(exe), str(ROOT / "scripts/fleet/drvindex.c")], check=True)
    out = subprocess.run([str(exe), str(d)], check=True, capture_output=True, text=True).stdout
    return out.splitlines()


def test_indexer_emits_model_line_ids_with_bucket_and_version(indexed):
    assert "PCI_10EC\tPCI\\VEN_10EC&DEV_8139\tA001\\rtl.inf\t05/01/2004,5.1.2.3" in indexed


def test_indexer_ignores_an_id_in_a_comment(indexed):
    assert not any("DEV_8129" in ln for ln in indexed), \
        "a ';'-commented model line was indexed - the store would offer an INF that does not serve the device"


def test_indexer_never_offers_a_3dfx_inf(indexed):
    assert not any(ln.startswith("PCI_121A") for ln in indexed)
    assert any(ln.startswith("#skip3dfx\tV001\\voodoo.inf") for ln in indexed), \
        "the 3dfx INF must be listed as skipped, not silently absent"


def test_indexer_keeps_the_adaptec_voodoo_codename(indexed):
    assert "PCI_9005\tPCI\\VEN_9005&DEV_0285\tR001\\arcsas.inf\t-" in indexed


def test_indexer_keeps_a_brace_id_whole(indexed):
    """drvmatch_idch has no braces; splitting there indexed '\\NVNET_DEV0057',
    which no device carries, so the store could never serve an nForce NIC."""
    assert "OTHER\t{1A3E09BE-1E45-494B-9174-D7385B45BBF5}\\NVNET_DEV0057\tN001\\nvnet.inf\t03/02/2006,5.10.2.0" in indexed
    assert not any(ln.split("\t")[1:2] == ["\\NVNET_DEV0057"] for ln in indexed)


def test_indexer_expands_a_strings_key_id(indexed):
    assert "PCI_10DE\tPCI\\VEN_10DE&DEV_0373\tN001\\nvnet.inf\t03/02/2006,5.10.2.0" in indexed


def test_driverver_comes_from_the_version_section_only(indexed):
    assert not any("01/01/1999" in ln or ln.endswith("\t9.9") for ln in indexed)


def test_indexer_compiles_the_agent_headers_not_a_copy():
    src = read(ROOT / "scripts/fleet/drvindex.c")
    for h in ("agent/shared/drvmatch.h", "agent/shared/drvsafe.h", "agent/shared/drvstore.h"):
        assert h in src
    assert "drvsafe_inf_text_hit" in src and "drvmatch_prepare" in src


# ---------------------------------------------------------------- the publisher
def _driverstore():
    spec = importlib.util.spec_from_file_location("driverstore", ROOT / "scripts/fleet/driverstore.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_index_lives_outside_the_oem_tree():
    ds = _driverstore()
    assert "$OEM$" in ds.STORE_REL and "$OEM$" not in ds.INDEX_REL, \
        "an index inside $OEM$ would be copied onto every freshly imaged box"


def test_group_buckets_and_keeps_the_3dfx_skips():
    ds = _driverstore()
    buckets, skipped = ds.group([
        "PCI_10EC\tPCI\\VEN_10EC&DEV_8139\tA001\\rtl.inf\t-",
        "PCI_10EC\tPCI\\VEN_10EC&DEV_8139\tA001\\rtl.inf\t-",
        "#skip3dfx\tV001\\voodoo.inf\tVEN_121A",
        "garbage",
    ])
    assert buckets == {"PCI_10EC": ["PCI\\VEN_10EC&DEV_8139\tA001\\rtl.inf\t-"]}
    assert skipped == ["V001\\voodoo.inf\tVEN_121A"]


def test_stale_buckets_are_found_and_check_flags_them(tmp_path, monkeypatch):
    ds = _driverstore()
    monkeypatch.setattr(ds, "MNT", tmp_path)
    d = tmp_path / ds.INDEX_REL
    d.mkdir(parents=True)
    (d / "PCI_10EC.TXT").write_bytes(b"x\r\n")
    (d / "PCI_DEAD.TXT").write_bytes(b"old\r\n")
    (d / "MANIFEST.TXT").write_bytes(b"m\r\n")
    files = {"PCI_10EC.TXT": b"x\r\n", "MANIFEST.TXT": b"m\r\n"}
    assert ds.stale_files(files) == ["PCI_DEAD.TXT"]
    assert ds.check(files) == ["extra:PCI_DEAD.TXT"], "a bucket the build no longer makes is not 'current'"


def test_manifest_is_published_last_and_each_file_verified():
    b = read(ROOT / "scripts/fleet/driverstore.py")
    assert '+ ["MANIFEST.TXT"]' in b, "the manifest must go last: it is what says the index is whole"
    assert "MNT / INDEX_REL / name" in b, "each file is hashed back through /mnt before the next"
    assert "stale_files(files) if not bad" in b, "stale buckets go only once the new set is whole"


# ---------------------------------------------------------------- the agent
GS = SRC / "gamesync.c"


def test_update_obeys_the_host_policy_and_refuses_win9x():
    b = body(read(SRC / "video.c"), "handle_drivers")
    i = b.index('"UPDATE"')
    upd = b[i:b.index("gs_drivers_update(", i)]
    assert "host_manages_this_box" in upd, "DRIVERS UPDATE changes the host: a modern box must refuse it"
    assert "0x80000000" in upd, "Win9x has no driver store yet: it must refuse, not half-run"


def test_update_is_xp_only():
    """The store holds XP x86 drivers; Windows 7 accepts an undecorated XP
    models section, so without this an XP driver is forced onto .246."""
    s = read(GS)
    ok = body(s, "gs_drvupd_os_ok")
    assert "LOBYTE(LOWORD(v)) == 5" in ok and "HIBYTE(LOWORD(v)) == 1" in ok
    assert "gs_drvupd_os_ok()" in body(s, "gs_drivers_update")
    assert "gs_drvupd_os_ok()" in body(s, "gs_drivers_autopass")


def test_arguments_are_whole_words():
    b = body(read(GS), "gs_drivers_update")
    assert "drvplan_parse_update(" in b and "drvplan_icontains" not in b


def test_targets_never_3dfx_disabled_or_display_automatically():
    b = body(read(GS), "gs_drivers_update_locked")
    assert 'why = "excluded_3dfx"' in b
    assert "drvmatch_problem_driver_fixable(pd[k].problem)" in b
    assert "!o->allow_display && gs_drv_is_display(cls, &pd[k])" in b
    gen = b[b.index("o->want_generic && o->allow_display"):b.index("/* candidates */")]
    assert "gs_device_3dfx" in gen and '"excluded_3dfx"' in gen, "a 3dfx stub display is reported, not dropped"
    assert "problem == 22 || problem == 29" in gen, "a disabled display adapter is left alone"
    assert "pd[i].dev.DevInst == dev.DevInst" in gen, "a device already targeted is not added twice"


def test_a_display_adapter_without_a_driver_is_still_display():
    """With no driver XP files it under Other devices, class not 'Display'."""
    b = body(read(GS), "gs_drv_is_display")
    assert '"PCI\\\\CC_03"' in b


def test_the_automatic_pass_refuses_a_display_class_inf():
    b = body(read(GS), "gs_drivers_update_locked")
    assert "!o->allow_display && gs_inf_is_display_class(inf, buf)" in b


def test_every_candidate_is_checked_before_it_is_forced():
    b = body(read(GS), "gs_drivers_update_locked")
    force = b.index("gs_force_install(")
    assert b.index("gs_inf_is_3dfx(inf) || gs_hwid_touches_3dfx(id)") < force, \
        "the forced id reaches EVERY present device carrying it - each must pass the 3dfx rule"
    assert b.index("gs_candidate_ok(") < force
    assert b.index('"tried_out"') < force, "at most two boots per device"
    assert "!o->retry" in b[:force], "a manual `retry` may pass the cap; nothing else"
    assert b.index('"same_as_earlier_device"') < force, "one forced install covers identical devices"
    assert b.index('"fixed_by_earlier_install"') < force


def test_dry_run_changes_nothing():
    b = body(read(GS), "gs_drivers_update_locked")
    loop = b[b.index("for (c = 0; c < pd[k].cand.n; c++)"):]
    assert loop.index("} else if (o->dry) {") < loop.index("gs_store_fetch("), "a dry run copies nothing"
    assert "if (!o->dry && !hung_before) {\n        newdev = LoadLibraryA" in b, "a dry run loads no installer"
    assert "if (!o->dry && !hung_before) {\n        gs_sdi_resolve();" in b, "no signing/non-interactive flip"
    assert "if (!bumped++)" in loop and loop.index('r->outcome = "would_install"') < loop.index("if (!bumped++)")


def test_a_hung_install_is_not_pulled_out_from_under():
    s = read(GS)
    b = body(s, "gs_drivers_update_locked")
    assert "if (g_sdi_nonint && !g_gs_install_hung)" in b, "prompts stay off under a hung install"
    assert "if (newdev && !g_gs_install_hung)" in b, "newdev.dll stays loaded while a thread is inside it"
    assert '"skipped_install_hung"' in b, "devices after a hang are listed, not dropped"
    core = body(s, "gs_drivers_update_core")
    assert "if (!g_gs_install_hung)\n        gs_drv_busy_leave();" in core
    assert "if (g_gs_install_hung) {" in body(s, "handle_drvupdate")


def test_failures_are_visible():
    b = body(read(GS), "gs_drivers_update_locked")
    for outcome in ('"index_unreachable"', '"fetch_failed"', '"skipped_install_hung"', '"hung"', '"failed"'):
        assert outcome in b
    assert "MANIFEST.TXT" in b and "DRIVER-STORE INDEX UNREACHABLE" in b
    assert "INDEX CACHE FULL" in b and "DEVICE LIST TRUNCATED" in b


def test_store_fetch_is_size_and_mtime_checked_and_cleaned_up():
    s = read(GS)
    f = body(s, "gs_store_fetch")
    assert "drvstore_rel_ok(rel)" in f
    assert "CompareFileTime(&a.ftLastWriteTime, &fd.ftLastWriteTime) == 0" in f, \
        "size alone hides an edit (the v1.62.0 GAMESYNC lesson)"
    assert "if (!CopyFileA(src, dst, FALSE))" in f
    assert "gs_free_bytes(" in f and "gs_free_margin()" in f
    assert "gs_store_drop(" in body(s, "gs_drivers_update_locked")
    assert "drvstore_rel_ok(rel)" in body(s, "gs_store_candidates")


def test_autopass_is_missing_only_non_display_and_switchable():
    s = read(GS)
    b = body(s, "gs_drivers_autopass")
    assert "memset(&o, 0, sizeof(o));" in b and "o.skip_seen = 1;" in b
    for f in ("want_generic", "allow_display", "dry", "retry"):
        assert f"o.{f} = 1" not in b, f"the automatic pass must not set {f}"
    assert '"DriverUpdate"' in b
    assert 'gs_drvupd_record("PENDING' in b, "the last boot's record is never read as today's"
    assert '"DriverUpdateBoot"' in body(s, "gs_drvupd_record") and "GetLocalTime" in body(s, "gs_drvupd_record")
    t = body(s, "gamesync_thread")
    call = t.index("gs_drivers_autopass()")
    assert t.index("host_manages_this_box") < call
    # newimage.flag is never deleted, so "fresh" is true on every boot of a
    # PXE-imaged box: a !fresh gate would never fire where it is needed (.110)
    before = t[:call].rstrip()
    assert before.endswith("*/"), "the automatic pass is an unconditional statement"
    stmt = before[:before.rindex("/*")].rstrip()
    assert stmt.endswith(";") or stmt.endswith("}"), "no if() gates it (a !fresh gate never fires on an imaged box)"
    assert t.index("gs_install_missing_drivers();") < call, "the image pass judges first"
    assert "o->skip_seen && gs_dv_lookup(pd[k].hw) != DRVMATCH_V_UNSEEN" in body(s, "gs_drivers_update_locked")


def test_no_registry_switch_allows_3dfx():
    s = read(GS)
    for name in ("DriverUpdate3dfx", "Allow3dfx", "AllowThreeDfx"):
        assert name not in s


def test_one_driver_install_at_a_time():
    """The startup passes, DRIVERS UPDATE and DRVUPDATE all force installs; the
    passes also flip the signing policy and SetupAPI's non-interactive mode
    around them. Two at once would restore each other's saved state."""
    s = read(GS)
    core = body(s, "gs_drivers_update_core")
    assert "gs_drv_busy_enter()" in core and "gs_drv_busy_leave()" in core
    assert "gs_drivers_update_locked(" in core
    drv = body(s, "handle_drvupdate")
    assert drv.index("gs_drv_busy_enter()") < drv.index("update(NULL, hwid, inf")
    t = body(s, "gamesync_thread")
    assert t.index("gs_drv_busy_wait()") < t.index("gs_install_missing_drivers();") < t.index("gs_drv_busy_leave();")
