"""A fake retro agent for the chat daemon/brain tests (a helper, not a test).

It speaks the agent's real framed protocol (AUTH, PROMPT_WAIT, LOG_APPEND,
LOG_APPEND2, STATUS_SET, PING, LOG_WAIT, PROXY_GET, EXEC/EXECW) on a localhost
port, so the daemon under test drives real sockets through the real
RetroConnection. It records what a person at the retro box would see (the
chat log text and the status line) and HOW every connection ended: 'eof' is a
graceful FIN, 'reset' is the RST that can take a Win98 box's Winsock down.

Knobs mirror agent behaviours the daemon has to survive:
  * append2        - agent 1.85.0+: LOG_APPEND2 <id> <text>, 'OK dup' on repeats
  * slow_os        - greet as Win98 ('Win4.10'), i.e. a single-threaded agent
  * delay[cmd]     - seconds before answering a command (a busy/slow agent);
                     the command still takes effect first, like a real agent
                     whose reply is late
  * idle_drop_s    - close a client that says nothing this long (NT: 120 s)
  * up / set_up()  - stop/start listening (a box powered off / on)
"""

import asyncio
import struct

RESP_OK_TEXT = 0x00
RESP_ERROR = 0xFF


def _frame(status: int, text: str) -> bytes:
    payload = bytes([status]) + text.encode("ascii", "replace")
    return struct.pack("<I", len(payload)) + payload


class FakeAgent:
    def __init__(self, *, append2=False, slow_os=False, poll_ms=200,
                 owner="", idle_drop_s=None, hostname="FAKE"):
        self.append2 = append2
        self.os = "Win4.10" if slow_os else "Win5.1"
        self.poll_ms = poll_ms
        self.owner = owner
        self.idle_drop_s = idle_drop_s
        self.hostname = hostname
        self.delay: dict[str, float] = {}
        self.log = ""
        self.statuses: list[str] = []
        self.commands: list[str] = []
        self.prompts: asyncio.Queue = asyncio.Queue()
        self.seen_ids: set = set()
        self.conns: list[dict] = []
        self.port = None
        self._server = None

    # -- lifecycle --
    async def start(self, port=0):
        self._server = await asyncio.start_server(self._client, "127.0.0.1", port,
                                                  reuse_address=True)
        self.port = self._server.sockets[0].getsockname()[1]
        return self

    async def set_up(self, up: bool):
        if up and self._server is None:
            await self.start(self.port)
        elif not up and self._server is not None:
            self._server.close()          # stop accepting: connects are refused
            for c in self.conns:          # a powered-off box drops everything
                w = c.get("writer")
                if w is not None and c["end"] == "open":
                    c["end"] = "killed"
                    w.transport.abort()
            # (Server.wait_closed() waits for every connection, so only after
            # they are gone)
            await self._server.wait_closed()
            self._server = None

    async def stop(self):
        await self.set_up(False)

    def push_prompt(self, text: str):
        self.prompts.put_nowait(text)

    def open_conns(self):
        return [c for c in self.conns if c["end"] == "open"]

    # -- protocol --
    async def _read_frame(self, reader, timeout):
        hdr = await asyncio.wait_for(reader.readexactly(4), timeout)
        (n,) = struct.unpack("<I", hdr)
        return (await reader.readexactly(n)).decode("ascii", "replace") if n else ""

    async def _client(self, reader, writer):
        rec = {"end": "open", "cmds": [], "writer": writer}
        self.conns.append(rec)
        try:
            first = await self._read_frame(reader, 5)
            if not first.startswith("AUTH "):
                writer.write(_frame(RESP_ERROR, "ERR auth"))
                return
            writer.write(_frame(RESP_OK_TEXT, f"OK {self.hostname} {self.os}"))
            await writer.drain()
            while True:
                try:
                    cmd = await self._read_frame(reader, self.idle_drop_s or 3600)
                except asyncio.TimeoutError:
                    rec["end"] = "agent-dropped-idle"
                    writer.close()        # a graceful close by the agent (FIN)
                    return
                rec["cmds"].append(cmd)
                self.commands.append(cmd)
                status, text = await self._handle(cmd)
                d = self.delay.get(cmd.split(" ", 1)[0], 0)
                if d:
                    await asyncio.sleep(d)
                writer.write(_frame(status, text))
                await writer.drain()
                if d and reader.at_eof():
                    # The client half-closed while we were busy. If it then
                    # closed for good before reading our late reply, its
                    # kernel answered with an RST; a second write surfaces
                    # it (harmless otherwise: a draining client discards it).
                    await asyncio.sleep(0.3)
                    writer.write(b"\0" * 8)
                    await writer.drain()
        except asyncio.IncompleteReadError as e:
            if rec["end"] == "open":
                rec["end"] = "eof" if not e.partial else "eof-mid-frame"
        except (ConnectionResetError, BrokenPipeError):
            if rec["end"] == "open":
                rec["end"] = "reset"
        except asyncio.CancelledError:
            raise
        finally:
            if rec["end"] == "open":
                rec["end"] = "closed"
            try:
                writer.close()
            except Exception:
                pass

    async def _handle(self, cmd: str):
        name, _, args = cmd.partition(" ")
        if name == "PING":
            return RESP_OK_TEXT, "PONG"
        if name == "LOG_WAIT":
            return RESP_OK_TEXT, f"{len(self.log)}\n"
        if name == "PROXY_GET":
            return RESP_OK_TEXT, self.owner
        if name == "PROMPT_WAIT":
            try:
                p = await asyncio.wait_for(self.prompts.get(), self.poll_ms / 1000)
                return RESP_OK_TEXT, p
            except asyncio.TimeoutError:
                return RESP_OK_TEXT, ""
        if name == "LOG_APPEND":
            self.log += args
            return RESP_OK_TEXT, "OK"
        if name == "LOG_APPEND2":
            if not self.append2:
                return RESP_ERROR, "Unknown command"
            cid, _, text = args.partition(" ")
            if cid in self.seen_ids:
                return RESP_OK_TEXT, "OK dup"
            self.seen_ids.add(cid)
            self.log += text
            return RESP_OK_TEXT, "OK"
        if name == "STATUS_SET":
            self.statuses.append(args)
            return RESP_OK_TEXT, "OK"
        if name in ("EXEC", "EXECW"):
            return RESP_OK_TEXT, f"ran {args}"
        return RESP_OK_TEXT, "OK"


# ---------------------------------------------------------------------------
# Loading the daemon under test
# ---------------------------------------------------------------------------
# The daemon is server-side and lives in the sibling nsc-assistant repo. Point
# NSC_ASSISTANT_DIR at a worktree to test a change before it lands there.

import importlib.util
import itertools
import os
import sys
from pathlib import Path

NSC = Path(os.environ.get("NSC_ASSISTANT_DIR",
                          str(Path.home() / "development" / "nsc-assistant")))
DAEMON = NSC / "agent" / "tools" / "retro_chat_daemon.py"
_n = itertools.count()


def load_daemon(tmp_path):
    """A fresh daemon module whose queues live under tmp_path, with timings
    shrunk so behaviour that takes minutes on the fleet takes milliseconds."""
    saved_path = list(sys.path)
    spec = importlib.util.spec_from_file_location(f"retro_chat_daemon_t{next(_n)}", DAEMON)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    finally:
        # the daemon puts nsc-assistant's root first on sys.path; leaving it
        # there would shadow this repo's own `scripts` package for later tests
        sys.path[:] = saved_path
    root = Path(tmp_path) / "chat"
    mod.ROOT = root
    mod.INBOX = root / "inbox"
    mod.OUTBOX = root / "outbox"
    mod.STATUS_OUTBOX = root / "status_outbox"
    mod.HISTORY = root / "history"
    mod.FAILED = root / "failed"
    mod.LOG_FILE = root / "daemon.log"
    mod.PID_FILE = root / "daemon.pid"
    mod.TASKS = Path(tmp_path) / "fleet-state" / "chat-tasks"
    mod.LEGACY_TASKS = root / "tasks"
    for d in (mod.INBOX, mod.OUTBOX, mod.STATUS_OUTBOX, mod.HISTORY, mod.FAILED, mod.TASKS):
        d.mkdir(parents=True, exist_ok=True)
    mod.WAIT_TIMEOUT_MS = 200
    mod.POLL_GRACE_S = 1
    mod.SLOW_POLL_GRACE_S = 1
    mod.CMD_TIMEOUT_S = 0.5
    mod.SLOW_CMD_TIMEOUT_S = 0.5
    mod.SENDER_TICK_S = 0.05
    mod.SENDER_BACKOFF_MAX_S = 0.2
    mod.SHUTDOWN_GRACE_S = 1.0
    mod.RECONNECT_BASE_S = 0.2
    mod.RECONNECT_MAX_S = 0.5
    mod._inotify = None
    mod.get_inotify = lambda: None       # the tests call dispatch_queues() directly
    mod.OUTBOX_POLL_S = 0.02
    return mod


def point_at(mod, agents: dict):
    """Route the daemon's connections for each fake IP to its fake agent."""
    def new_conn(ip):
        return mod.RetroConnection("127.0.0.1", agents[ip].port)
    mod._new_conn = new_conn


async def wait_until(pred, timeout=5.0, step=0.02):
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    while loop.time() < end:
        if pred():
            return True
        await asyncio.sleep(step)
    return pred()


def write_outbox(mod, host, seq, text, stream=True, name=None):
    import json
    p = mod.OUTBOX / (name or f"{host}-{seq}-{next(_n):06d}.json")
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(json.dumps({"host": host, "seq": seq, "chunks": [text], "stream": stream}))
    os.replace(tmp, p)
    return p


def write_status(mod, host, text, age_s=0.0, name=None):
    import json, time
    p = mod.STATUS_OUTBOX / (name or f"{host}-{int(time.time() * 1000)}{next(_n):04d}.json")
    p.write_text(json.dumps({"host": host, "text": text}))
    if age_s:
        t = time.time() - age_s
        os.utime(p, (t, t))
    return p
