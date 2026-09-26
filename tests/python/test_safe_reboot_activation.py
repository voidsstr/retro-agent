"""A reboot must not strand an unactivated XP box.

CLAUDE.md has carried this as **REQUIRED** since 2026-08-29 and **nothing
enforced it**, which is exactly why it kept happening. `safe-reboot.py` guarded
the PXE re-image risk beautifully and never once asked the question that
actually loses a machine.

THE FAILURE. An unactivated XP box is fine while it is logged in and
**unreachable the moment it restarts**: when the activation grace expires
Windows blocks logon entirely, so the console session never starts, the
`HKLM\\...\\Run\\RetroAgent` value never fires, and the machine comes back with
networking up (445/139/135 open) and the agent DEAD. It reads as a failed boot;
it is a locked activation screen, and there is no remote path back -- recovery
needs someone at the keyboard.

It cost `.171` a day. That box had been flagged *weeks* earlier as "not
activated, wpabaln.exe runs at logon, not blocking yet". Both facts were known
and written down; nothing made them meet at the moment of the reboot.

Measured 2026-08-31 with six agents live on the fleet: `.123` and `.133` were
both running `wpabaln.exe`, i.e. two of seven boxes would not have survived a
reboot any of those agents might have issued.

The guard is deliberately a REFUSAL with an explicit override rather than a
warning: a warning printed into a busy agent's log is a warning nobody reads.
"""
import importlib.util
import os

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "safe-reboot.py")

spec = importlib.util.spec_from_file_location("safe_reboot", SRC)
sr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sr)


def _src():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_the_activation_probe_exists_and_is_read_only():
    """LICSTATUS only reports; the probe must never try to change activation."""
    assert hasattr(sr, "activation_risk"), "the activation guard is gone"
    src = _src()
    assert "LICSTATUS" in src, "the probe no longer asks the agent for licence state"
    for forbidden in ("slmgr", "wpa.dbl", "OOBETimer\", \"REGWRITE", "msoobe"):
        assert forbidden not in src, (
            "safe-reboot must not attempt to MODIFY activation state (%s); it "
            "is a reboot tool, and altering licensing is the operator's call"
            % forbidden)


def test_it_refuses_before_it_reboots_not_after():
    """Order matters: arming a PXE hold protects the disk and does nothing for
    a box that will never reach a logon again."""
    src = _src()
    i_check = src.index("activation_risk(a.ip)")
    i_reboot = src.index("await reboot(a.ip)")
    assert i_check < i_reboot, (
        "the activation check runs AFTER the reboot call - by then the box is "
        "already gone")
    i_mac = src.index("await agent_mac(a.ip)")
    assert i_check < i_mac, (
        "the activation check should come before the MAC/PXE work: there is no "
        "point arming a boot hold for a box that cannot log in")


def test_a_risky_box_gets_its_own_exit_code():
    """"I refused" and "something broke" are different answers.

    The existing codes are 2 (no MAC) and 3 (could not arm a hold); a refusal
    on activation grounds must be distinguishable from both, or a caller
    cannot tell a protective refusal from a tool failure.
    """
    src = _src()
    assert "return 4" in src, (
        "the activation refusal shares an exit code with another failure mode")


def test_the_override_exists_and_is_explicit():
    """A hard block with no escape hatch gets worked around by copy-pasting the
    REBOOT command, which loses the guard entirely. The escape must exist and
    must be deliberate."""
    src = _src()
    assert "--ignore-activation" in src
    assert "physically at the machine" in src, (
        "the override must say WHEN it is legitimate, not merely exist")


@pytest.mark.parametrize("nag,is_xp,flag,expected", [
    (True,  True,  "unknown", True),   # the nag is running -> refuse
    (False, True,  "present", True),   # Winlogon says activation required
    (False, True,  "unknown", False),  # clean XP -> allow
    (False, False, "unknown", False),  # not XP at all -> rule does not apply
])
def test_the_verdict_table(nag, is_xp, flag, expected, monkeypatch):
    """Pins each branch, including the two that must ALLOW.

    A guard that only ever says no is as useless as one that only says yes: it
    would block every legitimate reboot on the fleet and be disabled within a
    day.
    """
    import asyncio
    import json as _json

    class FakeConn:
        def __init__(self, *a, **k): pass
        async def connect(self, *a, **k): return None
        async def close(self): return None
        async def command_text(self, cmd, timeout=None):
            if cmd == "LICSTATUS":
                return _json.dumps({"is_winxp": is_xp, "values": [
                    {"id": "activation_required", "observed": flag}]})
            if cmd == "EXEC tasklist":
                return "wpabaln.exe\n" if nag else "explorer.exe\n"
            raise AssertionError("unexpected command %r" % cmd)

    monkeypatch.setattr(sr, "RetroConnection", FakeConn)
    risky, why = asyncio.run(sr.activation_risk("192.0.2.1"))
    assert risky is expected, "nag=%s is_xp=%s flag=%s -> %s (%s)" % (
        nag, is_xp, flag, risky, why)
    assert why, "every verdict must carry its reason, for the operator's log"


# ---------------------------------------------------------------------------
# 2026-09-26: Windows' OWN verdict, not just the nag and the Winlogon flag.
#
# A Dell Dimension 4600 was PXE-imaged with its CMOS clock in 2004; the agent's
# clockfix then moved the clock to 2026 and spent the whole activation grace.
# LICSTATUS's Winlogon flag read "not present"; WMI's Win32_WindowsProductActivation
# said ActivationRequired=1, RemainingGracePeriod=0. safe-reboot refused only
# because wpabaln.exe happened to be running. These pin the WMI path, which does
# not depend on the nag being alive.
# ---------------------------------------------------------------------------

# Verbatim from the Dell (192.168.1.110), 2026-09-26 10:58, via EXEC.
DELL_WMIC = """ActivationRequired=1
Caption=
Description=
IsNotificationOn=1
ProductID=76487-342-1415037-22042
RemainingEvaluationPeriod=2147483647
RemainingGracePeriod=0
ServerName=nsc-5c5396faf9d
SettingID=
"""
DELL_WMIC_AFTER = DELL_WMIC.replace("ActivationRequired=1", "ActivationRequired=0") \
                           .replace("RemainingGracePeriod=0", "RemainingGracePeriod=2147483647")


def test_wmic_output_parses_as_windows_reports_it():
    assert sr.parse_wmic_wpa(DELL_WMIC) == (True, 0)
    assert sr.parse_wmic_wpa(DELL_WMIC_AFTER) == (False, 0)
    assert sr.parse_wmic_wpa("Node - X\nERROR:\nCode = 0x80041017\n") == (None, None)
    assert sr.parse_wmic_wpa("") == (None, None)


def _run(monkeypatch, lic_values, nag=False, wmic=None):
    import asyncio
    import json as _json

    class FakeConn:
        def __init__(self, *a, **k): pass
        async def connect(self, *a, **k): return None
        async def close(self): return None
        async def command_text(self, cmd, timeout=None):
            if cmd == "LICSTATUS":
                return _json.dumps({"is_winxp": True, "values": lic_values})
            if cmd == "EXEC tasklist":
                return "wpabaln.exe\n" if nag else "explorer.exe\n"
            if cmd.startswith("EXEC wmic path Win32_WindowsProductActivation get"):
                if wmic is None:
                    raise AssertionError("wmic asked although LICSTATUS already answered")
                return wmic
            raise AssertionError("unexpected command %r" % cmd)

    monkeypatch.setattr(sr, "RetroConnection", FakeConn)
    return asyncio.run(sr.activation_risk("192.0.2.1"))


def test_the_dell_is_refused_even_without_the_nag(monkeypatch):
    risky, why = _run(monkeypatch, [
        {"id": "activation_required", "observed": "unknown"},
        {"id": "wpa", "observed": "required", "activation_required": True, "grace_days": 0}])
    assert risky is True
    assert "WILL be refused" in why, why


def test_an_older_agent_is_asked_through_wmic(monkeypatch):
    risky, why = _run(monkeypatch, [{"id": "activation_required", "observed": "unknown"}],
                      wmic=DELL_WMIC)
    assert risky is True and "wmic" in why, why
    risky, why = _run(monkeypatch, [{"id": "activation_required", "observed": "unknown"}],
                      wmic=DELL_WMIC_AFTER)
    assert risky is False, why


def test_an_activated_box_is_allowed(monkeypatch):
    risky, why = _run(monkeypatch, [
        {"id": "activation_required", "observed": "unknown"},
        {"id": "wpa", "observed": "activated", "activation_required": False, "grace_days": 0}])
    assert risky is False and "activated" in why, why


def test_the_probe_only_ever_READS_activation():
    """Win32_WindowsProductActivation also has methods that CHANGE activation
    (ActivateOffline, SetProductKey). A reboot tool must never reach them."""
    src = _src()
    assert "Win32_WindowsProductActivation get" in src
    for forbidden in ("Win32_WindowsProductActivation call", "ActivateOffline",
                      "SetProductKey", "SetConfirmationID"):
        assert forbidden not in src, forbidden
