"""IPXSETUP (agent 1.97.0) - the IPX/SPX protocol on every fleet box that can
have it: NWLink through INetCfg on Windows 2000/XP, a validated registry
template + VxD payload on Windows 98 SE, a report everywhere else.

tests/native/test_ipxplan.c proves the decisions in agent/shared/ipxplan.h (the
state table, the reboot rule, the frame encodings, the payload CRC, the Win98
template's rules). Pinned here is the Win32 wiring a logic test cannot see:

* NO NEW STATIC IMPORT: ole32's COM entry points, SetupSetNonInteractiveMode,
  WSAEnumProtocolsA and WSCInstallProvider are only ever reached through
  GetProcAddress; ole32 is LoadLibrary'd; no -lole32/-luuid, no initguid. One
  import the Win98 loader cannot resolve takes .243 dark before main()
  (test_agent_win9x_imports.py checks the built binary).
* THE AGENT NEVER REBOOTS AN NT BOX: ipxnt.c has no reboot path at all. The 9x
  reboot reads IpxSetupReboot, writes IpxRebootLast and flushes the registry
  BEFORE it calls the shell's reboot (the postskip 1Bh order).
* the startup thread asks the host policy first, is spawned after gamesync, and
  takes the shared driver-install lock around the install; the INetCfg worker
  runs under a watchdog; a Win9x command never blocks the one serving thread;
  the Win98 writes are shut until IpxSetup9xTemplateOk=1.
* the chat brain gates the two switches that arm an unattended Win98 reboot or
  open the unvalidated registry template.
"""
import importlib.util
import re
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "agent" / "src"
IPX = (SRC / "ipxsetup.c").read_text()
NT = (SRC / "ipxnt.c").read_text()
W9 = (SRC / "ipx9x.c").read_text()
MAIN = (SRC / "main.c").read_text()
HDR = (ROOT / "agent" / "shared" / "ipxplan.h").read_text()


def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def no_strings(text):
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
                  src, re.M | re.S)
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
    raise AssertionError(name)


DYNAMIC_ONLY = ("CoInitializeEx", "CoInitialize", "CoCreateInstance", "CoUninitialize",
                "CoTaskMemFree", "SetupSetNonInteractiveMode", "WSAEnumProtocolsA",
                "WSCInstallProvider", "WSCDeinstallProvider")


# --------------------------------------------------------------------------
# Win98 must still LOAD the EXE
# --------------------------------------------------------------------------
def test_com_and_the_ws2_spi_are_resolved_never_linked():
    for name, src in (("ipxsetup.c", IPX), ("ipxnt.c", NT), ("ipx9x.c", W9)):
        code = no_strings(code_only(src))
        for fn in DYNAMIC_ONLY:
            assert not re.search(r"(?<![\w.>])" + fn + r"\s*\(", code), \
                f"{name} calls {fn} directly - that is a static import"
    assert 'LoadLibraryA("ole32.dll")' in NT
    for fn in ("CoInitializeEx", "CoCreateInstance", "CoUninitialize", "CoTaskMemFree"):
        assert f'GetProcAddress(ole, "{fn}")' in NT, fn
    assert '"SetupSetNonInteractiveMode"' in NT and '"WSAEnumProtocolsA"' in IPX
    mk = (ROOT / "agent" / "Makefile").read_text()
    libs = [l for l in mk.splitlines() if l.startswith("LIBS")]
    assert libs and "ole32" not in libs[0] and "uuid" not in libs[0], libs
    for src in (IPX, NT, W9):
        assert "initguid" not in code_only(src).lower(), \
            "GUIDs are spelled out (ipxplan.h), never #include <initguid.h>"


def test_the_guids_are_local_and_the_header_has_none_linked():
    assert "static const ipx_guid_t g_clsid_cnetcfg  = IPX_CLSID_CNETCFG;" in NT
    assert "sizeof(ipx_guid_t) == sizeof(GUID)" in NT
    code = no_strings(code_only(NT))
    for sym in ("&CLSID_CNetCfg", "&IID_INetCfg", "&GUID_DEVCLASS_NETTRANS"):
        assert sym not in code, f"{sym} would need libuuid"


# --------------------------------------------------------------------------
# reboots
# --------------------------------------------------------------------------
def test_the_nt_path_has_no_reboot():
    code = no_strings(code_only(NT))
    for f in ("agent_self_reboot_9x", "ExitWindowsEx", "InitiateSystemShutdown",
              "do_system_power", "shutdown"):
        assert f not in code, f
    assert "ipx_should_reboot(" in HDR and "if (mech != IPX_MECH_9X)\n        return 0;" in HDR


def test_the_9x_reboot_records_and_flushes_before_it_reboots():
    r = code_only(body(IPX, "ipx_reboot_if_armed_9x"))
    read = r.index("ipx_reg_dword(IPX_REG_REBOOT, 0, NULL)")
    should = r.index("ipx_should_reboot(")
    write = r.index("RegSetValueExA(h, IPX_REG_REBOOTLAST")
    flush = r.index("RegFlushKey(h)")
    boot = r.index('agent_self_reboot_9x("IPXSETUP")')
    assert read < should < write < flush < boot
    assert r.index("log_flush()") < boot
    assert "gamesync_busy()" in r, "never mid-copy"
    # it is only ever reached on Win9x
    run = code_only(body(IPX, "ipx_run"))
    assert "if (g.o.mech == IPX_MECH_9X && outcome != IPX_OUT_HUNG && g.state == IPX_ST_PENDING_REBOOT)" in run
    assert code_only(IPX).count("ipx_reboot_if_armed_9x()") == 1


# --------------------------------------------------------------------------
# the startup pass, the lock, the watchdog
# --------------------------------------------------------------------------
def test_the_startup_thread_asks_the_policy_first():
    t = code_only(body(IPX, "ipxsetup_thread"))
    gate = t.index("host_policy_skip(")
    assert t.index("thread_background()") < gate
    for later in ("ipx_observe(", "ipx_store(", "agent_nap(", "ipx_run("):
        assert gate < t.index(later), later
    assert t.index("agent_nap(IPX_FIRST_DELAY_MS)") < t.index("clockfix_finished()") < t.index("ipx_run(")


def test_spawned_after_gamesync_with_its_init_first():
    m = code_only(MAIN)
    assert m.index('spawn_helper(gamesync_thread, "gamesync")') < m.index("ipxsetup_init();") < \
        m.index('spawn_helper(ipxsetup_thread, "ipxsetup")')


def test_one_install_at_a_time_and_the_watchdog():
    run = code_only(body(IPX, "ipx_run"))
    take = run.index("agent_install_lock_enter() : !agent_install_lock_wait()")
    assert take < run.index("ipxnt_install(&r, IPX_NT_WATCHDOG_MS)") < run.index("agent_install_lock_leave();")
    assert take < run.index("ipx9x_install(&r,")
    # a hang leaves the lock held, like gs_force_install's
    hung = run[run.index("if (rc == IPXNT_HUNG) {"):]
    hung = hung[:hung.index("goto record;")]
    assert "agent_install_lock_leave" not in hung and "InterlockedExchange(&g_ipx_hung, 1)" in hung
    inst = code_only(body(NT, "ipxnt_install"))
    assert "WaitForSingleObject(th, watchdog_ms) != WAIT_OBJECT_0" in inst
    assert "CreateThread(NULL, 0, ipxnt_worker, j, 0, &tid)" in inst
    gs = (SRC / "gamesync.c").read_text()
    assert "int agent_install_lock_enter(void) { return gs_drv_busy_enter(); }" in gs


def test_the_9x_writes_are_shut_until_the_template_is_validated():
    run = code_only(body(IPX, "ipx_run"))
    assert run.index("if (f.o.mech == IPX_MECH_9X && !f.template_ok)") < run.index("ipx9x_install(&r,")
    assert '#define IPX_REG_TEMPLATE_OK "IpxSetup9xTemplateOk"' in HDR
    assert "return template_ok_present && template_ok == 1;" in HDR


def test_the_netcfg_sequence():
    w = code_only(body(NT, "ipxnt_worker"))
    order = ["INetCfgLock_AcquireWriteLock(", "INetCfg_Initialize(", 'INetCfg_FindComponent(nc, L"MS_NWIPX"',
             "INetCfg_QueryNetCfgClass(", 'INetCfgClassSetup_Install(cs, L"MS_NWIPX"', "INetCfg_Apply(nc)"]
    at = [w.index(o) for o in order]
    assert at == sorted(at), order
    assert "if (hr == S_FALSE) {" in w, "a held write lock is BUSY, not a failure"
    assert "INetCfg_Cancel(nc)" in w
    done = w[w.index("done:"):]
    assert done.index("INetCfg_Uninitialize(nc)") < done.index("INetCfgLock_ReleaseWriteLock(lk)")
    assert w.index("nonint(TRUE)") < w.index("INetCfgClassSetup_Install") < w.index("nonint(was)")


def test_a_win9x_command_never_blocks_the_serving_thread():
    h = code_only(body(IPX, "handle_ipxsetup"))
    assert "if (!agent_multiplex_mode())" in h
    assert h.index("if (!agent_multiplex_mode())") < h.index("WaitForSingleObject(g_ipx_done, IPX_CMD_WAIT_MS)")
    assert "CreateThread(NULL, 0, ipx_job_thread, ja, 0, &tid)" in h


def test_apply_is_guarded_and_status_is_read_only():
    h = code_only(body(IPX, "handle_ipxsetup"))
    apply = h[h.index("if (a & IPX_ARG_APPLY) {"):]
    assert apply.index("host_manages_this_box()") < apply.index("CreateThread(")
    assert "if (a & IPX_ARG_BAD)" in h
    obs = code_only(body(IPX, "ipx_observe"))
    for writer in ("RegSetValueExA", "ipx_reg_set_dword", "RegCreateKeyExA", "CoCreateInstance",
                   "ipxnt_install", "ipx9x_install"):
        assert writer not in obs, f"the status probe must not {writer}"
    assert "ipxnt_install" not in code_only(body(NT, "ipxnt_observe"))


# --------------------------------------------------------------------------
# the Win98 install
# --------------------------------------------------------------------------
def test_the_9x_install_refuses_before_it_writes():
    b = code_only(body(W9, "ipx9x_install"))
    first_write = b.index("RegCreateKeyExA(HKEY_LOCAL_MACHINE, keys[i].key")
    for check in ("if (x.nic_count != 1)", "ipx9x_template_check(", "if (conflicts) {",
                  "x9_stage(sys, r, why", "x9_write_undo("):
        assert b.index(check) < first_write, check
    # the flush comes after the value loop, and the read-back after the flush
    loop_end = b.index("r->values_written++;")
    flush = b.index("RegFlushKey(HKEY_LOCAL_MACHINE);", loop_end)
    assert flush < b.index("x9_value_state(e) != X9_HOLDS", flush), "read back after the flush"
    assert "CreateProcess" not in code_only(W9), "no regedit child on 9x"
    assert "MoveFileExA" not in code_only(W9)
    st = code_only(body(W9, "x9_stage"))
    copy = st.index("CopyFileA(src, tmp, FALSE)")
    assert copy < st.index("ipx_payload_ok(f, size, crc)", copy) < st.index("MoveFileA(tmp, dst)"), \
        "the copied temp is CRC-checked before it becomes the system file"
    # an existing file that is not exactly the manifest's build is never replaced
    pre = st[:copy]
    assert "if (x9_file_exists(dst)) {" in pre and "not replaced" in st


def test_the_backup_is_written_once_and_first():
    u = code_only(body(W9, "x9_write_undo"))
    assert u.index("if (x9_file_exists(IPX_BACKUP_FILE))") < u.index("CreateFileA(IPX_BACKUP_FILE")
    assert "CREATE_NEW" in u
    assert '#define IPX_BACKUP_FILE     "C:\\\\RETRO_AGENT\\\\IPX9X.BAK"' in HDR


# --------------------------------------------------------------------------
# rows, Makefile, HWPROFILE, the stagger
# --------------------------------------------------------------------------
def test_the_handler_row_makefile_and_hwprofile():
    handlers = (SRC / "handlers.c").read_text()
    assert re.search(r'\{\s*"IPXSETUP",\s*1,\s*NULL,\s*handle_ipxsetup,\s*0\s*\}', handlers)
    mk = (ROOT / "agent" / "Makefile").read_text()
    for f in ("ipxsetup.c", "ipxnt.c", "ipx9x.c"):
        assert f"$(SRCDIR)/{f}" in mk, f
    hw = code_only((SRC / "hwprofile.c").read_text())
    assert hw.index("hwextra_emit_network(&j);") < hw.index("ipxsetup_emit_hwprofile(&j);")
    e = code_only(body(IPX, "ipxsetup_emit_hwprofile"))
    assert 'json_key(j, "ipx")' in e


def test_the_startup_delay_does_not_collide():
    m = re.search(r"#define\s+IPX_FIRST_DELAY_MS\s+(\d+)", IPX)
    assert m and int(m.group(1)) == 100000, "after hwpublish (90 s), before gameindex (120 s)"


# --------------------------------------------------------------------------
# the chat brain's guard
# --------------------------------------------------------------------------
def _load_brain_tools(monkeypatch):
    sdk = types.ModuleType("claude_agent_sdk")
    sdk.create_sdk_mcp_server = lambda *a, **k: None
    sdk.tool = lambda *a, **k: (lambda f: f)
    monkeypatch.setitem(sys.modules, "claude_agent_sdk", sdk)
    spec = importlib.util.spec_from_file_location("retro_brain_tools_ipx", ROOT / "scripts" / "retro_brain_tools.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_brain_gates_arming_an_unattended_win98_reboot(monkeypatch):
    t = _load_brain_tools(monkeypatch)
    gated = [
        "REGWRITE HKLM Software\\RetroAgent IpxSetupReboot REG_DWORD 1",
        "regwrite hklm software\\retroagent ipxsetupreboot reg_dword 1",
        "REGWRITE HKLM Software\\RetroAgent IpxSetup9xTemplateOk REG_DWORD 1",
        'EXEC reg add HKLM\\Software\\RetroAgent /v IpxSetupReboot /t REG_DWORD /d 1 /f',
        "REGWRITE HKLM Software\\RetroAgent IpxSetupReboot",       # data missing: unclear = gated
    ]
    for c in gated:
        assert t._gate_reason(c), c
    for c in ("REGWRITE HKLM Software\\RetroAgent IpxSetupReboot REG_DWORD 0",   # disarming
              "REGWRITE HKLM Software\\RetroAgent IpxSetup REG_DWORD 0",
              "IPXSETUP", "IPXSETUP status", "IPXSETUP apply", "QBINDS apply"):
        assert t._gate_reason(c) is None, c
