"""The Fleet AI engine cannot turn itself back on (agent 1.99.0).

WHY THIS TEST EXISTS
--------------------
The AI engine (retro-infer.exe) has been "opt-in" since 1.17.0, and that word
was doing more work than the code was. The only gate was
HKLM\\Software\\RetroAgent\\AIEngine; `AI_ENABLE` set it, pulled the binary
from the share, spawned the engine, and the flag then survived every reboot.
So the documented "OFF by default" lasted until something issued one command -
after which a single-core vintage box quietly spent cycles on inference,
skewing every benchmark taken on it, with nothing on the box saying why.

User directive 2026-11-08: the fleet AI is off, its files are removed, and the
agent must not turn it back on. That makes four properties load-bearing, and
every one of them is invisible when it breaks - hence this file:

1. **The permission is a SECOND value**, `AIEngineAllowed`, absent by default.
2. **Nothing in the agent ever WRITES it.** This is the whole directive. A
   persisted "allow" that the agent can grant itself is exactly the automatic
   behaviour that was forbidden (same reasoning as drvsafe.h's deliberate
   absence of an ALLOW3DFX registry switch).
3. **`AI_ENABLE` refuses** rather than setting the flag and answering OK. A
   command that reports success and changes nothing is this project's
   signature failure shape.
4. **The purge is deliberately NOT hostpolicy-guarded.** On 2026-11-08 the two
   boxes actually carrying a staged retro-infer.exe were .145 and .139 - and
   .139 is the modern Windows copier host. A hostpolicy guard would have
   skipped the one box that most needed cleaning. Pinned in both directions so
   nobody "fixes" it by adding a guard, and nobody adds a host-reconfiguring
   step to this pass by mistake.
"""

import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]
AI_C = REPO / "agent" / "src" / "ai.c"
AIGATE_H = REPO / "agent" / "shared" / "aigate.h"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


def _function_body(src, name):
    """Body of a C function, brace-matched."""
    m = re.search(r"^[A-Za-z_][\w \t\*]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{",
                  src, re.M)
    if not m:
        return None
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


# --- 1. the permission exists and is a separate value ---------------------

def test_the_permission_is_its_own_registry_value():
    s = read(AI_C)
    assert '#define AI_ALLOW_VALUE  "AIEngineAllowed"' in s, \
        "the AI permission must be its own registry value, not the old flag"
    assert '#define AI_ENABLE_VALUE "AIEngine"' in s, \
        "the old opt-in flag stays - it is the second half of the gate"


def test_runnable_requires_both_values():
    body = _function_body(read(AI_C), "ai_engine_enabled")
    assert body is not None
    assert "ai_gate_may_run" in body, \
        "ai_engine_enabled must go through the shared decision, not re-derive it"
    assert "AI_ALLOW_VALUE" in body and "AI_ENABLE_VALUE" in body, \
        "both the permission and the opt-in flag must be read"


def test_a_wrong_typed_value_is_not_a_permission():
    """A REG_SZ "1" written by hand must not unlock the box."""
    body = _function_body(read(AI_C), "ai_read_flag")
    assert body is not None
    assert "REG_DWORD" in body and "type ==" in body, \
        "ai_read_flag must require the value really be a REG_DWORD"


# --- 2. THE DIRECTIVE: the agent never grants itself permission ----------

def test_nothing_in_the_agent_ever_writes_the_permission():
    """The heart of the 2026-11-08 directive.

    Any RegSetValueEx / RegCreateKey write naming AIEngineAllowed, anywhere in
    the agent, would let the agent re-arm its own AI. There must be exactly
    one mention of the value in a writing context: none.
    """
    src_dir = REPO / "agent"
    offenders = []
    for path in sorted(src_dir.rglob("*.c")) + sorted(src_dir.rglob("*.h")):
        if "shared/aigate.h" in path.as_posix():
            continue   # the decision header only ever reads
        text = read(path)
        if "AIEngineAllowed" not in text and "AI_ALLOW_VALUE" not in text:
            continue
        for m in re.finditer(r"(AIEngineAllowed|AI_ALLOW_VALUE)", text):
            # look at the enclosing statement for a write
            start = text.rfind(";", 0, m.start()) + 1
            end = text.find(";", m.end())
            stmt = text[start:end if end != -1 else len(text)]
            if re.search(r"RegSetValue|RegCreateKeyEx\w*\([^)]*KEY_SET_VALUE|"
                         r"RegDeleteValue", stmt):
                offenders.append(f"{path.relative_to(REPO)}: {stmt.strip()[:120]}")
    assert not offenders, (
        "the agent must never write AIEngineAllowed - it would be granting "
        "itself the AI permission:\n  " + "\n  ".join(offenders)
    )


def test_there_is_no_command_to_grant_the_permission():
    """Granting is a deliberate REGWRITE by an operator, not an AI command."""
    handlers = read(REPO / "agent" / "src" / "handlers.c")
    table = handlers[handlers.index("AI_HELLO"):]
    table = table[:table.index("\n};") if "\n};" in table else len(table)]
    assert "AI_ALLOW" not in table and "AI_PERMIT" not in table, \
        "no agent command may grant the AI permission"


# --- 3. AI_ENABLE refuses, and says why ----------------------------------

def test_ai_enable_refuses_before_it_sets_anything():
    body = _function_body(read(AI_C), "handle_ai_enable")
    assert body is not None
    assert "ai_gate_may_enable" in body, "AI_ENABLE must consult the gate"
    gate = body.index("ai_gate_may_enable")
    setflag = body.index("ai_engine_set_flag")
    assert gate < setflag, \
        "the gate check must come BEFORE the flag is written, or a locked box " \
        "still gets AIEngine=1 on disk"
    assert "send_error_response" in body, \
        "a refusal must be an ERROR, not an OK that changed nothing"
    assert "AIEngineAllowed" in body, \
        "the refusal must name the value an operator has to set"


def test_every_ai_command_reports_the_reason():
    """"AI engine unavailable" is true of a locked box, an unstaged box and a
    crashed engine alike - three different actions for the operator."""
    s = read(AI_C)
    for func in ("infer_proxy", "handle_ai_hello"):
        body = _function_body(s, func)
        assert body is not None, func
        assert "ai_engine_why" in body, \
            f"{func} must answer with the gate reason, not a generic string"


# --- 4. the purge: it undoes an earlier enable, on EVERY box -------------

def test_the_startup_pass_purges_while_locked():
    body = _function_body(read(AI_C), "ai_status_thread")
    assert body is not None
    assert "ai_gate_should_purge" in body, \
        "a locked box must UNDO an earlier enable at startup, not merely " \
        "decline to start the engine"
    assert "ai_engine_purge" in body
    assert "ai_engine_note_boot" in body, \
        "the post-condition must be readable off the box (AIEngineBoot), not " \
        "only from a log that rotates"
    # and it must not probe/stage on the locked path
    purge_at = body.index("ai_gate_should_purge")
    tail = body[purge_at:]
    first_return = tail.index("return 0;")
    assert "infer_roundtrip" not in tail[:first_return], \
        "the locked path must never probe the engine - probing spawns it"
    assert "stage_engine_from_share" not in tail[:first_return], \
        "the locked path must never stage the engine binary"


def test_the_purge_removes_the_flag_and_the_binary():
    body = _function_body(read(AI_C), "ai_engine_purge")
    assert body is not None
    assert "RegDeleteValueA" in body and "AI_ENABLE_VALUE" in body, \
        "the stray opt-in flag must be DELETED (steady state = absent)"
    assert "DeleteFileA" in body, \
        "the staged retro-infer.exe must go - it is what makes re-enabling " \
        "instant"
    assert "ai_taskkill_engine" in body, "a stray running engine must be killed"


def test_a_failed_delete_is_reported_not_concealed():
    body = _function_body(read(AI_C), "ai_engine_purge")
    assert "DELETE FAILED" in body, \
        "a tolerated failure must SAY it failed - every serious defect in this " \
        "project reported success and was believed"
    assert "GetLastError" in body, "and name the error code"


def test_the_purge_is_deliberately_not_hostpolicy_guarded():
    """Pinned in BOTH directions.

    .139 (Windows 10/11, the copier host) was one of only two boxes carrying a
    staged retro-infer.exe, so a hostpolicy guard here would skip the machine
    that most needed cleaning. Deleting a file the agent staged into its own
    directory is self-maintenance, like auto-update - not reconfiguring
    somebody's PC. If that judgement is ever revisited, this test is the place
    to argue it.
    """
    body = _function_body(read(AI_C), "ai_engine_purge")
    assert "host_policy_skip" not in body and "host_manages_this_box" not in body, \
        "the purge must run on modern Windows too (.139 carried the binary)"
    # ...and it must stay confined to the agent's own directory + its own key
    assert "agent_dir" in body, "the binary it deletes must be the one next to the agent"
    for forbidden in ("C:\\\\Games", "SystemRoot", "Desktop", "GAMESYNC"):
        assert forbidden not in body, \
            f"the purge touches only the agent's own files - found {forbidden!r}"


def test_disable_also_removes_so_off_cannot_be_undone_in_one_command():
    body = _function_body(read(AI_C), "handle_ai_disable")
    assert body is not None
    assert "ai_engine_purge" in body, \
        "AI_DISABLE must remove the binary too, or 'off' is one AI_ENABLE away"
    assert "removed" in body, "and report the post-condition, not OK"


def test_staging_refuses_on_a_locked_box():
    body = _function_body(read(AI_C), "stage_engine_from_share")
    assert body is not None
    assert "ai_engine_allowed" in body, \
        "the one function that puts the binary on disk must refuse on its own " \
        "account, not rely on its callers"
    allow_at = body.index("ai_engine_allowed")
    assert "CopyFileA" in body
    assert allow_at < body.index("CopyFileA"), \
        "the refusal must precede the copy"


def _strip_c_comments(s):
    return re.sub(r"/\*.*?\*/", " ", s, flags=re.S)


def test_the_decision_header_is_win32_free():
    """So tests/native/test_aigate.c compiles the exact code the agent runs.

    Checked against the CODE, not the prose: the header's rationale names
    DWORD and the registry freely, and a naive substring test over the whole
    file fails on its own documentation.
    """
    code = _strip_c_comments(read(AIGATE_H))
    for tok in ("windows.h", "HKEY", "RegOpenKey", "DWORD", "WINAPI", "MAX_PATH"):
        assert tok not in code, f"aigate.h must stay Win32-free - found {tok}"
    assert "unsigned long" in code, \
        "the flag struct uses plain C types so the native test can compile it"
