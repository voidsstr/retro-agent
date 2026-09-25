"""The chat brain survives restarts, runaway prompts and dead CLI processes.

Driven with a stub `claude_agent_sdk` (the real one is only in the brain's
venv, and a test must not spend API calls). Each test pins a failure:

  * a brain restart LOST in-flight prompts: the inbox file was deleted the
    moment it was dispatched. Now a prompt moves inbox -> queued ->
    processing and is deleted when its answer is done; after a restart queued
    prompts run, a prompt that was RUNNING is not replayed (it may have been a
    fleet operation) and its box is told to resend. A restarted brain's live
    claude session resumes the machine's persisted conversation
    (SessionStore / portable_resume, test_chat_brain_session_resume.py).
  * one runaway prompt held a machine's chat forever (unlimited turns, strict
    FIFO). A prompt queued behind another now says so, and a prompt is
    stopped after a wall-clock cap with a message to the user.
  * every prompt spawned a new `claude` CLI (~2-3 s floor on a trivial
    prompt). One live session per (machine, account) now serves consecutive
    prompts; a session that died is recreated rather than blamed on the
    account, and an account that cannot authenticate still fails over.

Run: pytest tests/python/test_chat_brain_resilience.py
"""

import asyncio
import importlib.util
import itertools
import json
import sys
import types
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parent.parent.parent
_BRAIN = _REPO / "scripts" / "retro_chat_brain.py"
_n = itertools.count()
HOST = "192.168.1.150"


# --- a stub Agent SDK ---------------------------------------------------------

def make_sdk(behave):
    """`behave(account_home, prompt, client)` -> list of messages, or raises."""
    sdk = types.ModuleType("claude_agent_sdk")

    class _Msg:
        def __init__(self, **kw):
            self.__dict__.update(kw)

    class AssistantMessage(_Msg): pass
    class SystemMessage(_Msg): pass
    class ResultMessage(_Msg): pass
    class StreamEvent(_Msg): pass
    class TextBlock(_Msg): pass
    class ToolUseBlock(_Msg): pass

    class ClaudeAgentOptions:
        def __init__(self, **kw):
            self.__dict__.update(kw)

    class HookMatcher:
        def __init__(self, **kw):
            self.__dict__.update(kw)

    class ClaudeSDKClient:
        instances = []

        def __init__(self, options=None):
            self.options = options
            self.connects = 0
            self.disconnected = False
            self.prompts = []
            ClaudeSDKClient.instances.append(self)

        @property
        def account(self):
            env = getattr(self.options, "env", None)
            return env.get("HOME") if env else None

        async def connect(self, prompt=None):
            self.connects += 1

        async def query(self, prompt, session_id="default"):
            async for m in prompt:
                self.prompts.append(m["message"]["content"])

        async def receive_response(self):
            for m in await behave(self.account, self.prompts[-1], self):
                yield m

        async def disconnect(self):
            self.disconnected = True

    query_calls = []

    async def query(prompt=None, options=None):
        text = None
        async for m in prompt:
            text = m["message"]["content"]
        query_calls.append(text)
        env = getattr(options, "env", None)
        for m in await behave(env.get("HOME") if env else None, text, None):
            yield m

    for name, obj in list(locals().items()):
        if isinstance(obj, type) or callable(obj):
            setattr(sdk, name, obj)
    sdk.query_calls = query_calls
    return sdk


def answer(sdk, text, sid="sid-1"):
    return [sdk.StreamEvent(parent_tool_use_id=None,
                            event={"type": "content_block_delta",
                                   "delta": {"type": "text_delta", "text": text}}),
            sdk.ResultMessage(session_id=sid, result=text, is_error=False)]


def load_brain(tmp_path, monkeypatch, behave, persistent=True):
    sdk = make_sdk(behave)
    monkeypatch.setitem(sys.modules, "claude_agent_sdk", sdk)
    tools = types.ModuleType("scripts.retro_brain_tools")
    tools.TOOL_NAMES = []
    tools.build_retro_server = lambda host: {"type": "sdk", "name": "retro"}
    guard = types.ModuleType("scripts.retro_brain_guard")
    guard.build_pretooluse_hook = lambda *a, **k: (lambda *a, **k: None)
    guard.build_can_use_tool = lambda *a, **k: None
    monkeypatch.setitem(sys.modules, "scripts.retro_brain_tools", tools)
    monkeypatch.setitem(sys.modules, "scripts.retro_brain_guard", guard)
    monkeypatch.setenv("RETRO_CHAT_ROOT", str(tmp_path / "chat"))
    monkeypatch.setenv("RETRO_BRAIN_SESSIONS", str(tmp_path / "state" / "brain-sessions.json"))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("CLAUDE_POOL_ROOT", str(tmp_path / "no-pool"))
    monkeypatch.setenv("RETRO_BRAIN_PERSISTENT", "1" if persistent else "0")
    spec = importlib.util.spec_from_file_location(f"retro_chat_brain_t{next(_n)}", _BRAIN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    for d in (mod.ROOT, mod.INBOX, mod.OUTBOX, mod.STATUS_OUTBOX, mod.QUEUED,
              mod.PROCESSING, mod.INTERRUPTED):
        d.mkdir(parents=True, exist_ok=True)
    mod.sdk = sdk
    return mod


def chat_text(mod, host=HOST):
    files = sorted(mod.OUTBOX.glob(f"{host}-*.json"),
                   key=lambda p: (json.loads(p.read_text())["seq"], p.name))
    return "".join("".join(json.loads(p.read_text())["chunks"]) for p in files)


def statuses(mod, host=HOST):
    return [json.loads(p.read_text())["text"]
            for p in sorted(mod.STATUS_OUTBOX.glob(f"{host}-*.json"))]


def put_prompt(mod, seq, text, host=HOST):
    (mod.INBOX / f"{host}-{seq}.json").write_text(
        json.dumps({"host": host, "seq": seq, "prompt": text}))


async def until(pred, timeout=5.0):
    for _ in range(int(timeout / 0.01)):
        if pred():
            return True
        await asyncio.sleep(0.01)
    return pred()


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 30))


# --- persistent sessions ----------------------------------------------------

def test_one_live_claude_process_serves_consecutive_prompts(tmp_path, monkeypatch):
    async def behave(acct, prompt, client):
        return answer(mod.sdk, f"re: {prompt}\n")

    mod = load_brain(tmp_path, monkeypatch, behave)

    async def go():
        brain = mod.Brain(mod.AccountManager(), mod.SessionStore(mod.SESSIONS_FILE),
                          mod.SessionPool())
        put_prompt(mod, 1, "reply PONG")
        brain.dispatch()
        assert await until(lambda: "re: reply PONG\n" in chat_text(mod))
        put_prompt(mod, 2, "again")
        brain.dispatch()
        assert await until(lambda: "re: again\n" in chat_text(mod))

    run(go())
    clients = mod.sdk.ClaudeSDKClient.instances
    assert len(clients) == 1 and clients[0].connects == 1, \
        "a new claude process per prompt is the 2-3 s floor this removes"
    assert clients[0].prompts == ["reply PONG", "again"]
    assert not mod.sdk.query_calls


def test_the_one_shot_path_is_still_there(tmp_path, monkeypatch):
    async def behave(acct, prompt, client):
        return answer(mod.sdk, "ok\n")

    mod = load_brain(tmp_path, monkeypatch, behave, persistent=False)
    assert mod.PERSISTENT is False

    async def go():
        await mod.run_prompt(HOST, 1, "hi", mod.SessionStore(mod.SESSIONS_FILE),
                             mod.AccountManager(), None)

    run(go())
    assert mod.sdk.query_calls == ["hi"] and chat_text(mod) == "ok\n"


def test_a_dead_session_is_recreated_not_blamed_on_the_account(tmp_path, monkeypatch):
    state = {"n": 0}

    async def behave(acct, prompt, client):
        state["n"] += 1
        if state["n"] == 2:
            raise ConnectionError("claude CLI exited")   # died between prompts
        return answer(mod.sdk, f"answer {state['n']}\n")

    mod = load_brain(tmp_path, monkeypatch, behave)
    accounts = mod.AccountManager()

    async def go():
        pool = mod.SessionPool()
        sessions = mod.SessionStore(mod.SESSIONS_FILE)
        await mod.run_prompt(HOST, 1, "one", sessions, accounts, pool)
        await mod.run_prompt(HOST, 2, "two", sessions, accounts, pool)

    run(go())
    assert chat_text(mod) == "answer 1\nanswer 3\n"
    assert not accounts.bad, "a dead CLI process is not an account failure"
    assert len(mod.sdk.ClaudeSDKClient.instances) == 2
    assert mod.sdk.ClaudeSDKClient.instances[0].disconnected


def test_an_unauthenticated_account_still_fails_over(tmp_path, monkeypatch):
    async def behave(acct, prompt, client):
        if acct is None:
            return [mod.sdk.ResultMessage(session_id=None, is_error=True,
                                          result="Invalid API key - Please run /login")]
        return answer(mod.sdk, "from account two\n", sid="sid-two")

    mod = load_brain(tmp_path, monkeypatch, behave)
    accounts = mod.AccountManager()
    accounts.homes = [None, str(tmp_path / "acct2")]
    accounts.labels = {None: "default", str(tmp_path / "acct2"): "two"}

    async def go():
        sessions = mod.SessionStore(mod.SESSIONS_FILE)
        await mod.run_prompt(HOST, 1, "hi", sessions, accounts, mod.SessionPool())
        return sessions

    sessions = run(go())
    assert chat_text(mod) == "from account two\n"
    assert None in accounts.bad and accounts.pin[HOST] == str(tmp_path / "acct2")
    assert sessions[(HOST, str(tmp_path / "acct2"))] == "sid-two"


# --- restart safety ---------------------------------------------------------

def test_session_ids_survive_a_restart(tmp_path, monkeypatch):
    async def behave(acct, prompt, client):
        return answer(mod.sdk, "hello\n", sid="sid-persisted")

    mod = load_brain(tmp_path, monkeypatch, behave)

    async def go(m):
        await m.run_prompt(HOST, 1, "hi", m.SessionStore(m.SESSIONS_FILE),
                           m.AccountManager(), m.SessionPool())

    run(go(mod))
    assert (tmp_path / "state" / "brain-sessions.json").is_file()
    # the CLI keeps the transcript under the account's HOME
    t = tmp_path / "home" / ".claude" / "projects" / "-retro" / "sid-persisted.jsonl"
    t.parent.mkdir(parents=True)
    t.write_text("{}\n")

    mod = load_brain(tmp_path, monkeypatch, behave)          # the brain restarted
    restored = mod.SessionStore(mod.SESSIONS_FILE)
    assert restored.get((HOST, None)) == "sid-persisted"
    run(go(mod))
    assert mod.sdk.ClaudeSDKClient.instances[-1].options.resume == "sid-persisted", \
        "the new claude process must resume the machine's conversation"


def test_restart_reruns_queued_prompts_and_never_replays_a_running_one(tmp_path, monkeypatch):
    async def behave(acct, prompt, client):
        return answer(mod.sdk, "x\n")

    mod = load_brain(tmp_path, monkeypatch, behave)
    (mod.PROCESSING / f"{HOST}-1790000001.json").write_text(
        json.dumps({"host": HOST, "seq": 1790000001, "prompt": "REBOOT .143"}))
    (mod.QUEUED / f"{HOST}-1790000002.json").write_text(
        json.dumps({"host": HOST, "seq": 1790000002, "prompt": "what time is it"}))
    requeued, told = mod.recover_after_restart()
    assert (requeued, told) == (1, 1)
    assert (mod.INBOX / f"{HOST}-1790000002.json").exists()
    assert (mod.INTERRUPTED / f"{HOST}-1790000001.json").exists()
    assert "interrupted" in chat_text(mod) and "NOT re-run" in chat_text(mod)
    assert not mod.sdk.ClaudeSDKClient.instances and not mod.sdk.query_calls


def test_a_prompt_stays_on_disk_until_its_answer_is_done(tmp_path, monkeypatch):
    gate = {}

    async def behave(acct, prompt, client):
        await gate["release"].wait()
        return answer(mod.sdk, "done\n")

    mod = load_brain(tmp_path, monkeypatch, behave)

    async def go():
        gate["release"] = asyncio.Event()
        brain = mod.Brain(mod.AccountManager(), mod.SessionStore(mod.SESSIONS_FILE),
                          mod.SessionPool())
        put_prompt(mod, 5, "slow one")
        brain.dispatch()
        assert await until(lambda: list(mod.PROCESSING.glob("*.json")))
        assert not list(mod.INBOX.glob("*.json"))
        gate["release"].set()
        assert await until(lambda: "done\n" in chat_text(mod))
        assert await until(lambda: not list(mod.PROCESSING.glob("*.json")))

    run(go())


# --- bounded ----------------------------------------------------------------

def test_a_prompt_behind_another_says_it_is_queued(tmp_path, monkeypatch):
    gate = {}

    async def behave(acct, prompt, client):
        if prompt == "first":
            await gate["release"].wait()
        return answer(mod.sdk, f"{prompt}\n")

    mod = load_brain(tmp_path, monkeypatch, behave)

    async def go():
        gate["release"] = asyncio.Event()
        brain = mod.Brain(mod.AccountManager(), mod.SessionStore(mod.SESSIONS_FILE),
                          mod.SessionPool())
        put_prompt(mod, 1, "first")
        brain.dispatch()
        assert await until(lambda: brain.busy.get(HOST))
        put_prompt(mod, 2, "second")
        brain.dispatch()
        assert mod.QUEUED_NOTICE in statuses(mod)
        gate["release"].set()
        assert await until(lambda: chat_text(mod) == "first\nsecond\n")

    run(go())


def test_a_runaway_prompt_is_stopped_and_the_user_is_told(tmp_path, monkeypatch):
    async def behave(acct, prompt, client):
        if prompt == "forever":
            await asyncio.sleep(3600)
        return answer(mod.sdk, "next one works\n")

    mod = load_brain(tmp_path, monkeypatch, behave)

    async def go():
        brain = mod.Brain(mod.AccountManager(), mod.SessionStore(mod.SESSIONS_FILE),
                          mod.SessionPool(), prompt_timeout=0.5)
        put_prompt(mod, 1, "forever")
        put_prompt(mod, 2, "after")
        brain.dispatch()
        assert await until(lambda: "next one works" in chat_text(mod), 5)

    run(go())
    text = chat_text(mod)
    assert "[Stopped:" in text and text.index("[Stopped:") < text.index("next one works")
    first = mod.sdk.ClaudeSDKClient.instances[0]
    assert first.disconnected, "the runaway claude process (and its tools) must be stopped"
    assert "" in statuses(mod)
