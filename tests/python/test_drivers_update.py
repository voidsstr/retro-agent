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
                            ("R001", "arcsas.inf", INF_ADAPTEC)):
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


def test_manifest_is_published_last_and_each_file_verified():
    b = read(ROOT / "scripts/fleet/driverstore.py")
    assert '+ ["MANIFEST.TXT"]' in b, "the manifest must go last: it is what says the index is whole"
    assert "MNT / INDEX_REL / name" in b, "each file is hashed back through /mnt before the next"


# ---------------------------------------------------------------- the agent
def test_update_obeys_the_host_policy_and_refuses_win9x():
    b = body(read(SRC / "video.c"), "handle_drivers")
    i = b.index('"UPDATE"')
    upd = b[i:b.index("gs_drivers_update(", i)]
    assert "host_manages_this_box" in upd, "DRIVERS UPDATE changes the host: a modern box must refuse it"
    assert "0x80000000" in upd, "Win9x has no driver store yet: it must refuse, not half-run"


def test_update_core_never_touches_3dfx_or_an_unfixable_problem():
    b = body(read(SRC / "gamesync.c"), "gs_drivers_update_locked")
    assert "pd[k].excl" in b and '"excluded_3dfx"' in b
    assert "drvmatch_problem_driver_fixable(pd[k].problem)" in b
    assert 'allow_display || _stricmp(cls, "Display") != 0' in b
    # the stub-display tier also asks the 3dfx rule
    gen = b[b.index("want_generic && allow_display"):]
    assert "gs_device_3dfx" in gen[:gen.index("/* candidates */")]


def test_every_candidate_is_checked_before_it_is_forced():
    b = body(read(SRC / "gamesync.c"), "gs_drivers_update_locked")
    force = b.index("gs_force_install(")
    assert b.index("gs_inf_is_3dfx(inf)") < force
    assert b.index("gs_candidate_ok(") < force
    assert b.index('"tried_out"') < force, "at most two boots per device"
    assert "gs_signing_restore" in b and "g_sdi_nonint(was_nonint)" in b


def test_store_paths_are_validated_before_joining():
    s = read(SRC / "gamesync.c")
    assert "drvstore_rel_ok(rel)" in body(s, "gs_store_fetch")
    assert "drvstore_rel_ok(rel)" in body(s, "gs_store_candidates")


def test_autopass_is_missing_only_non_display_and_switchable():
    s = read(SRC / "gamesync.c")
    b = body(s, "gs_drivers_autopass")
    assert "gs_drivers_update_core(0, 0, 0, 1," in b, \
        "automatic = no generic tier, no display, not dry, and devices the image pass judged are left to it"
    assert '"DriverUpdate"' in b and '"DriverUpdateBoot"' in b
    t = body(s, "gamesync_thread")
    call = t.index("gs_drivers_autopass()")
    assert t.index("host_manages_this_box") < call
    guard = t[t.rfind("if", 0, call):call]
    assert "0x80000000" in guard
    # newimage.flag is never deleted, so "fresh" is true on every boot of a
    # PXE-imaged box: a !fresh gate would never fire where it is needed (.110)
    assert "!fresh" not in guard
    assert t.index("gs_install_missing_drivers();") < call, "the image pass judges first"
    core = body(s, "gs_drivers_update_locked")
    assert "skip_seen && gs_dv_lookup(pd[k].hw) != DRVMATCH_V_UNSEEN" in core


def test_no_registry_switch_allows_3dfx():
    s = read(SRC / "gamesync.c")
    for name in ("DriverUpdate3dfx", "Allow3dfx", "AllowThreeDfx"):
        assert name not in s


def test_one_driver_install_at_a_time():
    """The startup passes, DRIVERS UPDATE and DRVUPDATE all force installs; the
    passes also flip the signing policy and SetupAPI's non-interactive mode
    around them. Two at once would restore each other's saved state."""
    s = read(SRC / "gamesync.c")
    core = body(s, "gs_drivers_update_core")
    assert "gs_drv_busy_enter()" in core and "gs_drv_busy_leave()" in core
    assert "gs_drivers_update_locked(" in core
    drv = body(s, "handle_drvupdate")
    assert drv.index("gs_drv_busy_enter()") < drv.index("update(NULL, hwid, inf")
    t = body(s, "gamesync_thread")
    assert t.index("gs_drv_busy_wait()") < t.index("gs_install_missing_drivers();") < t.index("gs_drv_busy_leave();")
