"""The 3dfx rule (agent 1.87.0): 3dfx drivers are never changed unless the
chat explicitly asks (user directive 2026-09-27).

Until 1.87.0 nothing enforced it. The XP missing-driver pass listed every device
with a problem code in every class, and the image's C:\\D carries four 3dfx
INFs that the agent's own matcher would pick for a Voodoo. The rule itself -
what counts as 3dfx - is agent/shared/drvsafe.h, tested natively by
tests/native/test_drvsafe.c. Pinned here: that EVERY driver path consults it,
that the one way past it is the explicit ALLOW3DFX token, that the chat brain
gates that token, and that no persisted switch can turn it on.
"""
import importlib.util
import re
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GS = re.sub(r"/\*.*?\*/", "", (ROOT / "agent" / "src" / "gamesync.c").read_text(), flags=re.S)
PCIR = re.sub(r"/\*.*?\*/", "", (ROOT / "agent" / "src" / "pcirescue.c").read_text(), flags=re.S)


def body(code, name):
    start = code.index(name)
    depth, i = 0, code.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[start:i + 1]
        i += 1


def test_every_problem_device_carries_a_3dfx_verdict():
    pd = body(GS, "static int gs_problem_devices(")
    assert "out[n].excl = gs_device_3dfx(set, &dev, out[n].hw, out[n].compat);" in pd
    cls = body(GS, "static int gs_device_3dfx(")
    order = [cls.index("drvsafe_ids_3dfx(hw, compat)"),
             cls.index("drvsafe_driver_is_3dfx(prov, mfg, desc, infp, service)"),
             cls.index("ntdyn_CM_Get_Parent"),
             cls.index("drvsafe_is_pci_bridge(hw, compat) && gs_subtree_3dfx(")]
    assert order == sorted(order)


def test_the_install_loop_never_touches_a_3dfx_device():
    loop = body(GS, "static void gs_install_missing_drivers(")
    excl = loop.index("if (pd[k].excl) {")
    assert excl < loop.index("gs_prefer_claims(&pd[k])") < loop.index("gs_force_install(")
    assert "gs_dv_record(pd[k].hw, DRVMATCH_V_EXCLUDED);" in loop[excl:excl + 600]


def test_a_3dfx_inf_is_never_a_candidate():
    scan = body(GS, "static int gs_scan_driver_tree(")
    assert scan.index("if (drvsafe_inf_is_3dfx(buf))") < scan.index("drvmatch_prepare(buf);"), \
        "the WHOLE text is checked, before it is cut down to model lines"
    assert "if (d[k].excl)" in scan


def test_a_3dfx_device_never_holds_the_driver_tree():
    guard = body(GS, "static int gs_devices_unconfigured(")
    assert "if (!pd[k].excl && gs_dv_lookup(pd[k].hw) == DRVMATCH_V_UNSEEN)" in guard
    assert guard.index("if (pd[k].excl) {") < guard.index("drvmatch_keeps_tree(v, scan == 0, confirmed)")
    dm = (ROOT / "agent" / "shared" / "drvmatch.h").read_text()
    assert "DRVMATCH_V_EXCLUDED" in dm and re.search(r"case DRVMATCH_V_EXCLUDED:[^\n]*\n\s*return 0;", dm)


def test_prefer_txt_never_forces_a_3dfx_driver():
    prefs = body(GS, "static int gs_prefs_pass(")
    chk = prefs.index("gs_hwid_touches_3dfx(hwid)")
    assert prefs.index("drvpref_present(ids, hwid)") < chk < prefs.index("update(NULL, hwid, inf")
    assert "gs_inf_is_3dfx(inf)" in prefs[chk:chk + 400]


def test_drvupdate_refuses_3dfx_without_the_explicit_token():
    h = body(GS, "void handle_drvupdate(")
    assert "allow3dfx = drvsafe_args_allow(args_in);" in h
    touch = h.index("why3dfx = gs_hwid_touches_3dfx(hwid);")
    refuse = h.index("if (why3dfx != DRVSAFE_OK && !allow3dfx) {")
    assert touch < refuse < h.index("update(NULL, hwid, inf")
    assert "if (allow3dfx && !inf[0]) {" in h, "a 3dfx INF is never CHOSEN, even when allowed"
    assert h.index("if (!allow3dfx && gs_inf_is_3dfx(inf)) {") < h.index("update(NULL, hwid, inf")
    assert "EXPLICIT 3dfx REQUEST" in h


def test_win9x_rescue_never_reenumerates_onto_an_unprotected_3dfx_card():
    """The registry alone cannot see a Voodoo Win98 never enumerated (.243,
    2026-09-24): the check reads PCI config space, refuses when it cannot, and
    counts only cards ON THE BUS - a ghost key of a removed card, or a card the
    operator disabled, must not block the Voodoo 2 rescue (Glide-over-RAM)."""
    run = body(PCIR, "static void pcir_run(")
    assert run.index("!allow3dfx && pcir_3dfx_unprotected(dfx, sizeof(dfx))") < run.index("pcir_reenumerate_buses(")
    refuse = run[run.index("pcir_3dfx_unprotected"):run.index("pcir_reenumerate_buses(")]
    assert "r->n_missing_after = r->n_missing_before;" in refuse, "a refusal must not read as rescued"
    unp = body(PCIR, "static int pcir_3dfx_unprotected(")
    assert "pcir_cfg(0, 0, 0, 0)" in unp and "unreadable" in unp and "return 1;" in unp
    assert "(v & 0xFFFF) != 0x121A" in unp and "pcir_3dfx_slot_covered(" in unp
    assert "multi-function" in PCIR or "0x80)) nf = 8" in unp
    cov = body(PCIR, "static int pcir_3dfx_slot_covered(")
    assert '"BUS_%02X&DEV_%02X&FUNC_%02X"' in cov and "pcir_inst_disabled(hinst)" in cov
    assert "pcir_run(&r, 0, 0);" in PCIR, "the startup pass never allows 3dfx"
    assert "allow3dfx = drvsafe_args_allow(args)" in PCIR
    assert '"rescued", !r.error &&' in PCIR
    assert "pcir_3dfx_driverless" not in PCIR, "the registry-only check is gone"


def test_no_persisted_switch_can_allow_3dfx():
    for src in (ROOT / "agent" / "src").glob("*.c"):
        code = re.sub(r"/\*.*?\*/", "", src.read_text(errors="replace"), flags=re.S)
        for m in re.finditer(r'RegQueryValueExA?\([^,]+,\s*"([^"]+)"', code):
            assert "3DFX" not in m.group(1).upper(), f"{src.name} reads a registry value {m.group(1)!r}"


def _load_brain_tools(monkeypatch):
    sdk = types.ModuleType("claude_agent_sdk")
    sdk.create_sdk_mcp_server = lambda *a, **k: None
    sdk.tool = lambda *a, **k: (lambda f: f)
    monkeypatch.setitem(sys.modules, "claude_agent_sdk", sdk)
    spec = importlib.util.spec_from_file_location("retro_brain_tools_3dfx", ROOT / "scripts" / "retro_brain_tools.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_chat_brain_gates_the_token_and_3dfx_driver_commands(monkeypatch):
    t = _load_brain_tools(monkeypatch)
    gated = [
        "DRVUPDATE PCI\\VEN_121A&DEV_0009 C:\\D\\V001\\3dfxvs.inf ALLOW3DFX",
        "DRVUPDATE PCI\\VEN_121A&DEV_0002",
        "PCIRESCAN force ALLOW3DFX",
        "drvupdate pci\\ven_121a&dev_0002 c:\\x\\voodoo2.inf",
    ]
    for c in gated:
        assert t._gate_reason(c), c
    gated_raw = [
        'EXECW 420 C:\\RETRO_AGENT\\V2\\drvupd.exe "C:\\RETRO_AGENT\\V2\\Voodoo2.inf" "PCI\\VEN_121A&DEV_0002"',
        "REGWRITE HKLM SYSTEM\\CurrentControlSet\\Services\\fxgpio Start REG_DWORD 1",
        "LAUNCH C:\\am29\\driver9x\\3dfxvs.inf",
    ]
    for c in gated_raw:
        assert t._gate_reason(c), c
    for c in ("DRVUPDATE PCI\\VEN_10DE&DEV_0150", "PCIRESCAN force", "SYSINFO", "EXEC echo voodoo",
              "EXEC dir C:\\3dfx", "REGWRITE HKLM Software\\RetroAgent PostSkip REG_DWORD 1"):
        assert t._gate_reason(c) is None, c
    prompt = (ROOT / "scripts" / "retro_chat_brain.py").read_text()
    assert "3dfx DRIVERS ARE NEVER CHANGED" in prompt and "ALLOW3DFX" in prompt
