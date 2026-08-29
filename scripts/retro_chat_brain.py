#!/usr/bin/env python3
"""Retro Chat Brain — a standalone Claude-Code-grade agent for the retro chat.

Replaces the "spawn a Claude Code subagent" processor. This is a long-running
service that turns the retro chat into a full agentic assistant WITHOUT needing
an interactive Claude Code session on this machine — exactly so the fleet's
retro PCs can chat with Claude even when no Claude Code window is open.

Pipeline (the daemon is unchanged — same inbox/outbox/heartbeat contract):

    retro_chat.exe - retro_agent.exe - retro_chat_daemon.py -+- inbox/  <- prompts
                                                             +- outbox/ -> answers
                                                                   ^
                                                   retro_chat_brain.py (this) - Claude Agent SDK

Real-time + resilient design (LAN, multiple machines, multiple accounts):

  * Concurrency: one asyncio worker PER machine. Prompts from different retro
    PCs run in parallel; a long job on one never freezes the chat on another.
    Within a machine prompts stay strictly ordered (one worker, FIFO queue).

  * Multiple accounts + FAILOVER: machines are pinned round-robin to a Claude
    account (default login + every healthy claude-pool profile). If a machine's
    account can't authenticate (logged out / expired) or is rate-limited, the
    brain transparently retries the SAME prompt on the next account, trying
    ALL of them before it ever surfaces an error. So the client always gets a
    real answer as long as at least one account works. A machine that fails
    over is re-pinned to the account that succeeded.

  * Live status: every tool call / thinking delta is pushed to the client via
    the daemon's status_outbox within milliseconds; the answer streams on line
    boundaries. Failover attempts that fail auth emit nothing to the client
    (the CLI dies before any assistant token), so a retry is invisible.

Run:  scripts/.brain-venv/bin/python scripts/retro_chat_brain.py
(usually via the systemd unit or supervisor — see scripts/README-chat-brain.md)
"""

import asyncio
import json
import logging
import os
import shutil
import sys
import time
import unicodedata
from pathlib import Path

# The retro console renders ASCII only (the client strips high-bit bytes, and
# the daemon's send path is ASCII). Fold the model's Unicode down to ASCII so
# replies actually deliver instead of failing on a stray smart-quote or check.
_ASCII_MAP = {
    "–": "-", "—": "-", "−": "-",          # dashes / minus
    "‘": "'", "’": "'", "‚": "'",            # single quotes
    "“": '"', "”": '"', "„": '"',            # double quotes
    "•": "-", "·": "-", "●": "-", "▪": "-",  # bullets
    "…": "...", " ": " ", " ": " ", " ": " ",
    "✓": "[ok]", "✔": "[ok]", "✗": "x", "✘": "x",
    "→": "->", "←": "<-", "⇒": "=>",
    "≥": ">=", "≤": "<=", "×": "x", "°": " deg",
}


def ascii_clean(s):
    for k, v in _ASCII_MAP.items():
        s = s.replace(k, v)
    s = unicodedata.normalize("NFKD", s)
    return s.encode("ascii", "ignore").decode("ascii")

_REPO = Path(__file__).resolve().parent.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from claude_agent_sdk import (  # noqa: E402
    AssistantMessage,
    ClaudeAgentOptions,
    HookMatcher,
    ResultMessage,
    StreamEvent,
    SystemMessage,
    TextBlock,
    ToolUseBlock,
    query,
)

import scripts.retro_brain_tools as fleet  # noqa: E402
import scripts.retro_brain_guard as guard  # noqa: E402

# ---------------------------------------------------------------------------
# Daemon contract (must match retro_chat_daemon.py)
# ---------------------------------------------------------------------------
ROOT = Path(os.environ.get("RETRO_CHAT_ROOT", "/tmp/retro-chat"))
INBOX = ROOT / "inbox"
OUTBOX = ROOT / "outbox"
STATUS_OUTBOX = ROOT / "status_outbox"
HEARTBEAT = ROOT / "processor.heartbeat"
BRAIN_LOG = ROOT / "brain.log"

POLL_INTERVAL = 0.02        # inbox dispatch poll — only gates pickup, not throughput
HEARTBEAT_INTERVAL = 20     # chat_status.sh flags the processor stale after 120s
ACCOUNT_REFRESH_S = 180     # re-scan the pool so re-logins/limit-resets are picked up
ACCOUNT_COOLDOWN_S = 180    # how long a failed account is deprioritized

MODEL = os.environ.get("RETRO_BRAIN_MODEL", "claude-opus-4-8")
# 'medium' keeps chat snappy (less pre-output thinking) while staying capable
# enough for fleet ops; bump to high/xhigh via env for heavier tasks.
EFFORT = os.environ.get("RETRO_BRAIN_EFFORT", "medium")
# 0 (or any value <= 0) means UNBOUNDED — no agentic-loop cap per prompt. A
# fleet op can be arbitrarily many steps (screenshot->click loops, multi-reboot
# driver work), and there's no human watching to bump a limit, so don't cap by
# default. Set a positive RETRO_BRAIN_MAX_TURNS to re-impose a ceiling.
MAX_TURNS = int(os.environ.get("RETRO_BRAIN_MAX_TURNS", "0"))

# The SDK reads whole JSON messages from the `claude` CLI's stdout into one
# buffer and FATALLY aborts the message reader if a single message exceeds this
# (default 1MB). A retro_screenshot tool result carries a base64 PNG that easily
# blows past 1MB, which used to crash the query deterministically — so every
# failover account hit the same wall and the whole prompt failed. Give it lots
# of headroom (a full-res screenshot base64 is a few MB).
MAX_BUFFER_SIZE = int(os.environ.get("RETRO_BRAIN_MAX_BUFFER", str(64 * 1024 * 1024)))

# claude-pool: each healthy profile is a separate Claude account we can pin a
# machine to (and fail over to). Discovered from the pool's state.json.
POOL_ROOT = Path(os.environ.get(
    "CLAUDE_POOL_ROOT", str(Path.home() / ".reusable-agents" / "claude-pool")))

BUILTIN_TOOLS = [
    "Read", "Write", "Edit", "Bash", "Glob", "Grep",
    "WebSearch", "WebFetch", "Agent",
]
ALLOWED_TOOLS = BUILTIN_TOOLS + fleet.TOOL_NAMES

# Fleet applications the brain maintains live in sibling repos, so cwd alone
# (retro-agent) is not enough reach. Only directories that actually exist are
# passed -- add_dirs with a missing path makes the CLI refuse to start, and a
# brain that will not boot is a worse failure than one that cannot see a repo.
_SIBLING_REPOS = ["retro-agent-private", "nsc-assistant", "retro-3dfx"]
EXTRA_DIRS = [str(_REPO.parent / name) for name in _SIBLING_REPOS
              if (_REPO.parent / name).is_dir()]

SYSTEM_APPEND = """
You are operating as the brain of the **Retro Chat** bridge. A user is chatting
with you from a retro PC (Win98 / Win2K / WinXP) through a text relay, so:

- Output PLAIN ASCII ONLY — the retro console cannot render Unicode. No emoji,
  smart quotes, em-dashes, box-drawing, bullets, or check/cross marks. Use -, ',
  ", ... and plain words instead.
- Keep answers concise and readable on a small CRT — lead with the outcome.
- You are autonomous: the user cannot click approval dialogs, so just act.
- You have the full toolset on THIS Linux host (Bash/Read/Edit/Web/...) AND the
  `mcp__retro__*` tools to operate the retro fleet directly:
    * retro_list_machines — find a machine's IP
    * retro_command       — run any agent command (SYSINFO, EXEC, VIDEODIAG,
                            REGREAD, UICLICK x y, ...) on a fleet machine
    * retro_screenshot    — see a machine's screen (drives screenshot->UICLICK)
- The chat is coming from a specific machine; retro tools default to THAT machine
  when you omit `host`. Operating the *originating* machine can briefly contend
  with the live chat channel — prefer operating other fleet machines by explicit
  IP when you can.
- SAFETY: destructive or irreversible fleet actions (REBOOT, SHUTDOWN, QUIT,
  DELETE, REGDELETE, PROCKILL, disk-wiping EXEC) are gated. `retro_command`
  refuses them unless you pass confirm=true, and you may ONLY set confirm=true
  when the user has explicitly asked for that specific action on that specific
  machine. Never confirm on your own initiative — these machines may need
  physical access to recover.
- FLEETBOOK (the fleet's memory - USE IT): scripts/retro_fleetbook.py is a
  persistent SQLite knowledge base (~/.retro-fleet/fleetbook.db) of solved
  problems and a per-machine change log. Work it into EVERY fix/change cycle:
    * BEFORE diagnosing or fixing anything, search it - we may have solved this
      before:  python3 scripts/retro_fleetbook.py search <keywords>
      then `show <id|slug>` to get the exact recipe, and follow it.
    * AFTER completing any change on a machine (fix, install, config change,
      driver deploy), record it:
        python3 scripts/retro_fleetbook.py log --host <ip> --summary "..." \
            [--detail "..."] [--recipe <slug>]      (--recipe when you applied one)
    * When you solve something NEW the hard way (a gotcha, a working method),
      store the reusable steps:  retro_fleetbook.py add --title ... --problem ...
      --recipe "exact steps" --tags a,b --source "chat YYYY-MM-DD"
      Recipes are for REUSABLE fixes; one-off state changes only need `log`.
    * `history --host <ip>` answers "what changed on this box?".
- GAME INSTALL: to install a game from the SMB share onto a fleet PC, use the
  `game-install` skill. It knows the share's Games layout (~4,200 titles; it
  reads the share's games index), detects the installer type, and does the headless
  silent install (GOG/Inno/NSIS), copy-in, or zip/iso extract - or defers to the
  `gui-install` skill's CLICKSHOT walk when the installer has no silent switch.
  It also handles the per-box traps (dual-boot D:, Win7 UAC, stale Z:, broken
  xcopy). Trigger on "install <game> on <box>", "put <game> on the boxes", "what
  can I install". DOS titles go through the DOS lane (DOSGAME), not this skill.
- GAME SERVERS: this host also runs the fleet's dedicated game servers (CS 1.6,
  The Specialists, Quake III Arena, OpenArena, Quake 2, QuakeWorld, UT99,
  UT2004) as systemctl --user units. When the user asks about them — current or
  available maps, switching maps, bots, installing plugins/mods/maps, restarts,
  "is the server up" — invoke the `game-servers` skill (if it is not listed, it
  is a local symlink into retro-agent-private; say so instead of guessing).
- 3DFX DRIVER WORK (you have the FULL driver dev capability on this host):
  two driver codebases are in play, and you can build, test, diagnose, fix,
  and deploy both — invoke the matching skill FIRST, it encodes the whole
  workflow and the hard-won gotchas:
    * `voodoo3-driver-dev` — OUR clean-room stack (MesaFX ICD + open Glide,
      versions 0.1.x) in voodoo-cleanroom/ here; target box .124 (Voodoo3).
    * `voodoo5-driver-dev` — the vintage 3dfx H5 stack (display + D3D HAL +
      miniport + Glide, built under Wine) in
      /home/voidsstr/development/retro-3dfx/; target box .143 (Voodoo5 5500).
  Both repos are on this host's disk with full source, build toolchains,
  regression suites (tests/run_all.sh here; retro-3dfx/tests/predeploy.sh +
  run_target_tests.py there), findings ledgers (CHANGELOG.md /
  VINTAGE-FIXES.md / FINDINGS.md), and deploy skills (deploy-3dfx-driver,
  driver-install, driver-bench). Use mcp__retro__retro_upload /
  retro_download to move driver binaries and logs between this host and the
  boxes. NEVER deploy without the stack's test gate green, never reboot a box
  without the user's explicit ok, and always verify by renderer string /
  VIDEODIAG after deploy.
- BUILDING AND FIXING THE FLEET'S OWN SOFTWARE (you are a full dev session):
  You are not limited to running commands on boxes. You have Read/Write/Edit/
  Bash on this host and you are expected to work on the fleet's applications
  the same way any Claude Code session would: read the debug logs, find the
  bug, change the code, build it, deploy it to a retro machine, and verify.
  Repos in reach: this one (the agent, DOSGAME, DOSCHAT, voodoo-cleanroom),
  retro-agent-private, nsc-assistant, and retro-3dfx (READ-ONLY - CLAUDE.md
  forbids editing that driver tree).
    * DOS PLAYER LOOP (the common one): DOSGAME.EXE is at scripts/dosgames/
      and DOSCHAT.EXE at agent/doschat/. Pull the box's debug log with
      retro_command DOWNLOAD (or EXEC type), read it here, fix the C, rebuild
      with the Open Watcom toolchain in ~/development/toolchain-dos/, and
      redeploy. On a Win9x box the agent auto-stages C:\\DOSGAME and C:\\DOSCHAT
      from the share, so publishing there reaches the machine; DOSSTAGE force
      re-stages immediately. Read scripts/dosgames/README.md and
      agent/doschat/README.md BEFORE touching a DOS build - the quoted-include
      case sensitivity and the 64K DGROUP ceiling will cost you an hour each.
    * Write deploy scripts when a repeatable one does not exist yet, and keep
      it in the repo rather than as a one-off in /tmp.
    * Follow this repo's CLAUDE.md rules for real: work in a worktree, bump the
      version tag BEFORE building the agent or chat client, keep
      `bash tests/run_all.sh` green, publish to the share, and log the change
      to the fleetbook.
- TWO HARD LIMITS ON THAT (enforced, not advisory - a PreToolUse hook denies
  them before the tool runs, so do not waste turns trying to route around it):
    * NO CLOUD DEPLOYMENT. You ship to the retro fleet over the LAN. az, aws,
      gcloud, kubectl, terraform, docker push, npm publish and the site repos'
      azure/deploy.sh are all blocked.
    * NO CHANGES TO THE SPECPICKS OR AISLEPROMPT SYSTEMS. You may READ them
      (they are often the reference for how something is done), but any write,
      edit or mutating shell command against those trees is refused. They are
      live sites with their own agents; a plausible-looking fix from here is
      worse than none.
  If a request needs either, say plainly that it is out of scope for the retro
  brain and stop - do not improvise a workaround.
- SECURITY POSTURE: a `security-posture` skill is available. If the user asks to
  review the agent's security, run a penetration test, or harden the fleet,
  invoke it and walk them through the interactive checklist and recommendations.
- This repo's CLAUDE.md documents the fleet, the Win98 gotchas (vcache, ghost
  PCI, 3dfx drivers), and the agent command reference — consult it.
"""


def publish_json(path, payload):
    """Write one queue file so the daemon can never read it half-finished.

    This is not defensive tidiness, it is the fix for answers vanishing. The
    daemon watches the outbox with **inotify** and parses on the create event,
    then `json.loads` whatever is on disk -- and on a JSONDecodeError it
    DELETES the file. `Path.write_text()` creates the file at its final name
    and then fills it, so the daemon was routinely woken by a zero-byte or
    partial file, failed to parse it, and destroyed the reply. The user typed
    a message, the brain answered, and nothing ever came back.

    Writing to a sibling temp name and `os.replace`-ing it means the final
    name only ever appears complete -- rename is atomic within a filesystem.
    The temp suffix goes AFTER `.json` so it cannot match the daemon's
    `glob('*.json')` while it is being written.

    Same convention as scripts/ai_status_bus.py and the dashboard collector.
    """
    path = Path(path)
    tmp = path.with_name(f"{path.name}.tmp{os.getpid()}")
    try:
        with open(tmp, "w") as fh:
            json.dump(payload, fh)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except Exception:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


def setup():
    for d in (ROOT, INBOX, OUTBOX, STATUS_OUTBOX):
        d.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[logging.FileHandler(str(BRAIN_LOG)), logging.StreamHandler(sys.stderr)],
    )


log = logging.getLogger("retro_chat_brain")


def beat():
    try:
        HEARTBEAT.touch()
    except Exception:  # noqa: BLE001
        pass


def _rate_limited(info):
    """True only if a usage-limit reset is still in the FUTURE. Past reset
    timestamps mean the limit has already lifted (the account is usable again)."""
    from datetime import datetime, timezone
    resets = info.get("limit_resets_at") or {}
    if not resets:
        return False
    now = datetime.now(timezone.utc)
    for ts in resets.values():
        try:
            if datetime.fromisoformat(str(ts).replace("Z", "+00:00")) > now:
                return True
        except Exception:  # noqa: BLE001
            continue
    return False


def discover_accounts():
    """Return (homes, labels) for every usable Claude account.

    Always includes the default login (None => don't override HOME). Adds each
    claude-pool profile that is authenticated and not currently rate-limited.
    """
    homes = [None]
    labels = ["default(~)"]
    try:
        state = json.loads((POOL_ROOT / "state.json").read_text())
        for pid, info in sorted(state.items()):
            if not isinstance(info, dict) or not info.get("authenticated"):
                continue
            if _rate_limited(info):
                continue
            home = info.get("home")
            if home and (Path(home) / ".claude" / ".credentials.json").is_file():
                homes.append(home)
                labels.append(info.get("label") or pid)
    except FileNotFoundError:
        pass
    except Exception as e:  # noqa: BLE001
        log.warning("account discovery failed (%s); using default login only", e)
    return homes, labels


class AccountManager:
    """Tracks usable Claude accounts and each machine's preferred account,
    with failover: order_for(host) always lists EVERY account (preferred +
    healthy first, cooled-down/bad last) so a prompt can try them all."""

    def __init__(self):
        self.homes = []
        self.labels = {}
        self.pin = {}      # host -> preferred account home
        self.bad = {}      # account home -> cooldown-until epoch
        self._rr = 0
        self.refresh()

    def refresh(self):
        homes, labels = discover_accounts()
        self.homes = homes
        self.labels = {h: l for h, l in zip(homes, labels)}
        self.bad = {h: t for h, t in self.bad.items() if h in homes}  # forget vanished

    def label(self, home):
        return self.labels.get(home, home or "default(~)")

    def _healthy(self):
        now = time.time()
        return [h for h in self.homes if self.bad.get(h, 0) <= now]

    def assign(self, host):
        pool = self._healthy() or self.homes
        acct = pool[self._rr % len(pool)]
        self._rr += 1
        self.pin[host] = acct
        return acct

    def order_for(self, host):
        """Full failover order for a machine: preferred (if healthy) -> other
        healthy -> everything else (last resort). Never empty while any account
        exists, so we exhaust them all before erroring."""
        if host not in self.pin:
            self.assign(host)
        pinned = self.pin[host]
        healthy = self._healthy()
        order = []
        if pinned in healthy:
            order.append(pinned)
        for h in healthy:
            if h not in order:
                order.append(h)
        for h in self.homes:            # last resort: retry even cooled-down ones
            if h not in order:
                order.append(h)
        return order

    def mark_bad(self, home):
        self.bad[home] = time.time() + ACCOUNT_COOLDOWN_S

    def mark_ok(self, host, home):
        self.pin[host] = home
        self.bad.pop(home, None)


def write_status(host, text):
    """Tiny one-shot the daemon forwards as '[subagent: <text>]' instantly.

    An empty text clears the client's status line (used when a turn finishes).
    """
    try:
        p = STATUS_OUTBOX / f"{host}-{int(time.time()*1000)}.json"
        publish_json(p, {"host": host, "text": ascii_clean(text)})
    except Exception:  # noqa: BLE001
        pass


def _find_cli():
    """Locate the `claude` binary — PATH may be bare under systemctl --user."""
    cli = shutil.which("claude")
    if cli:
        return cli
    for cand in (Path.home() / ".local/bin/claude", Path("/usr/local/bin/claude")):
        if cand.exists():
            return str(cand)
    return None


_CLI_PATH = _find_cli()


def options_for(host, resume, account_home=None):
    opts = dict(
        model=MODEL,
        effort=EFFORT,
        system_prompt={"type": "preset", "preset": "claude_code", "append": SYSTEM_APPEND},
        allowed_tools=ALLOWED_TOOLS,
        mcp_servers={"retro": fleet.build_retro_server(host)},  # origin baked per-query
        # Autonomous: no human is watching the chat to approve each step, so the
        # SDK can't prompt. The compensating control is the fleet-safety guardrail
        # in retro_brain_tools.retro_command (destructive agent commands require an
        # explicit confirm=true, which the system prompt only allows when the user
        # asked) — see that module and the security-posture skill.
        permission_mode="bypassPermissions",
        # The fleet-scope policy. PreToolUse is the load-bearing one: under
        # bypassPermissions the permission callback can legitimately never be
        # consulted, but hooks fire either way. can_use_tool is a second layer.
        hooks={"PreToolUse": [HookMatcher(
            matcher="*", hooks=[guard.build_pretooluse_hook(str(_REPO), log)])]},
        can_use_tool=guard.build_can_use_tool(str(_REPO), log),
        add_dirs=EXTRA_DIRS,                   # sibling fleet repos
        cwd=str(_REPO),                        # loads this repo's CLAUDE.md
        skills="all",                          # enable filesystem skills (injects the
                                               # Skill tool + discovers installed skills)
        setting_sources=["project"],           # discover .claude/skills from the repo
                                               # (retro-wallpaper, security-posture) + CLAUDE.md
        resume=resume,
        include_partial_messages=True,         # token-level deltas -> live streaming
        max_buffer_size=MAX_BUFFER_SIZE,       # don't die on big screenshot results
    )
    if MAX_TURNS > 0:                          # 0/negative => unbounded (omit the cap)
        opts["max_turns"] = MAX_TURNS
    if _CLI_PATH:
        opts["cli_path"] = _CLI_PATH
    if account_home:
        # Run this machine's `claude` under a specific account's HOME so it
        # uses that account's credentials (spreads load, enables failover).
        opts["env"] = {**os.environ, "HOME": account_home}
    return ClaudeAgentOptions(**opts)


def tool_status(name, tool_input):
    """One-line 'what the brain is doing' summary for the live status feed."""
    ti = tool_input or {}
    base = os.path.basename
    if name == "Bash":
        return "bash: " + (ti.get("command") or "").replace("\n", " ")[:48]
    if name == "Read":
        return "read: " + base(ti.get("file_path", ""))
    if name in ("Edit", "Write"):
        return name.lower() + ": " + base(ti.get("file_path", ""))
    if name == "Grep":
        return "search: " + str(ti.get("pattern", ""))[:32]
    if name == "Glob":
        return "glob: " + str(ti.get("pattern", ""))[:32]
    if name == "WebSearch":
        return "web search: " + str(ti.get("query", ""))[:40]
    if name == "WebFetch":
        return "fetch: " + str(ti.get("url", ""))[:40]
    if name == "Agent":
        return "subagent: " + str(ti.get("description", ""))[:40]
    if "retro_command" in name:
        return "retro: " + str(ti.get("command", ""))[:40]
    if "retro_screenshot" in name:
        return "retro: screenshot " + str(ti.get("host", "") or "")
    if "retro_list" in name:
        return "retro: list machines"
    return "running: " + name


async def run_prompt(host, seq, prompt, sessions, accounts):
    """Stream one prompt through the agent loop with account failover.

    Tries the machine's preferred account, then every other account, until one
    authenticates and answers. Failed (unauthenticated) attempts emit NOTHING
    to the client, so the retry is invisible — the user just sees the answer
    from whichever account works. Only if EVERY account fails do we surface an
    error."""
    write_status(host, "thinking...")
    idx = {"n": 0}
    buf = {"s": ""}

    def emit(text):
        if not text:
            return
        idx["n"] += 1
        p = OUTBOX / f"{host}-{seq}-{idx['n']:06d}.json"
        publish_json(p, {"host": host, "seq": seq,
                         "chunks": [ascii_clean(text)], "stream": True})

    def feed(delta, st):
        buf["s"] += delta
        if "\n" in buf["s"]:
            cut = buf["s"].rfind("\n") + 1
            emit(buf["s"][:cut])
            buf["s"] = buf["s"][cut:]
            st["text"] = True
        elif len(buf["s"]) >= 160:
            emit(buf["s"])
            buf["s"] = ""
            st["text"] = True

    async def attempt(account_home):
        """Run the query on one account. Returns a state dict. `authed` is True
        once ANY assistant activity (text/thinking/tool) arrives — i.e. the CLI
        got past authentication. Auth/login failures never set it."""
        resume = sessions.get((host, account_home))
        st = {"authed": False, "text": False, "think": "", "shown": 0,
              "sid": resume, "err": None, "result": None}
        try:
            async for msg in query(prompt=prompt,
                                   options=options_for(host, resume, account_home)):
                if isinstance(msg, StreamEvent):
                    if msg.parent_tool_use_id:      # subagent internal stream — skip
                        continue
                    ev = msg.event or {}
                    et = ev.get("type")
                    if et == "content_block_delta":
                        d = ev.get("delta") or {}
                        if d.get("type") == "text_delta":
                            st["authed"] = True
                            feed(d.get("text", ""), st)
                        elif d.get("type") == "thinking_delta":
                            st["authed"] = True
                            t = d.get("thinking", "")
                            if t:
                                st["think"] += t
                                if len(st["think"]) - st["shown"] >= 30:
                                    st["shown"] = len(st["think"])
                                    snip = st["think"].replace("\n", " ").strip()
                                    if snip:
                                        write_status(host, "thinking: " + snip[-70:])
                    elif et == "content_block_start":
                        if (ev.get("content_block") or {}).get("type") == "thinking":
                            st["authed"] = True
                            write_status(host, "thinking...")
                elif isinstance(msg, AssistantMessage):
                    for block in getattr(msg, "content", []) or []:
                        if isinstance(block, ToolUseBlock):
                            st["authed"] = True
                            write_status(host, tool_status(block.name, block.input))
                elif isinstance(msg, SystemMessage):
                    data = getattr(msg, "data", {}) or {}
                    if data.get("session_id"):
                        st["sid"] = data["session_id"]
                elif isinstance(msg, ResultMessage):
                    sid = getattr(msg, "session_id", None)
                    if sid:
                        st["sid"] = sid
                    st["result"] = getattr(msg, "result", None)
                    if getattr(msg, "is_error", False):
                        st["err"] = st["result"] or "error"
        except Exception as e:  # noqa: BLE001
            st["err"] = str(e)
        return st

    order = accounts.order_for(host)
    last_err = None
    for account_home in order:
        buf["s"] = ""                       # nothing carried between attempts
        try:
            st = await attempt(account_home)
        except Exception as e:              # noqa: BLE001
            last_err = str(e)
            accounts.mark_bad(account_home)
            log.warning("host=%s seq=%s account %s crashed (%s) -> failover",
                        host, seq, accounts.label(account_home), last_err[:80])
            continue

        answered = st["text"] or (st["result"] and not st["err"])
        if answered:
            if buf["s"]:                    # flush the trailing partial line
                emit(buf["s"])
                st["text"] = True
                buf["s"] = ""
            if not st["text"]:              # authed but only a final result blob
                emit(str(st["result"]) + "\n")
            if st["err"]:                   # streamed then errored late — note it
                emit(f"\n[note: {str(st['err'])[:80]}]\n")
            if st["sid"]:
                sessions[(host, account_home)] = st["sid"]
            accounts.mark_ok(host, account_home)
            log.info("host=%s seq=%s answered by %s", host, seq, accounts.label(account_home))
            write_status(host, "")
            return

        # The query authenticated (real assistant activity arrived) but then
        # failed mid-turn — NOT an account/auth problem. Failing over would just
        # replay the same deterministic crash on every account, so surface the
        # error here and stop. Flush any partial line we already have first.
        if st["authed"]:
            if buf["s"]:
                emit(buf["s"])
                buf["s"] = ""
            accounts.mark_ok(host, account_home)   # account is fine; don't cool it
            if st["sid"]:
                sessions[(host, account_home)] = st["sid"]
            log.error("host=%s seq=%s authed but failed mid-turn (%s)",
                      host, seq, str(st["err"])[:120])
            emit("\n[The request hit an error mid-answer: " + str(st["err"])[:120] +
                 ". Try again or narrow the task.]\n")
            write_status(host, "")
            return

        # Unauthenticated / no answer -> this account is unusable; fail over.
        last_err = st["err"] or "no response"
        accounts.mark_bad(account_home)
        log.warning("host=%s seq=%s account %s unusable (%s) -> failover",
                    host, seq, accounts.label(account_home), str(last_err)[:80])

    # Every account failed.
    log.error("host=%s seq=%s ALL accounts failed: %s", host, seq, str(last_err)[:120])
    emit("[No Claude account on the brain is usable right now (" + str(last_err)[:60] +
         "). Re-login one on the server: HOME=~/.reusable-agents/claude-pool/profile-1 "
         "claude /login  — chat will recover automatically.]\n")
    write_status(host, "")


async def heartbeat_loop():
    while True:
        beat()
        await asyncio.sleep(HEARTBEAT_INTERVAL)


async def account_refresh_loop(accounts):
    """Periodically re-scan the pool so re-logins and limit-resets come back
    online without a brain restart."""
    while True:
        await asyncio.sleep(ACCOUNT_REFRESH_S)
        before = set(accounts.homes)
        accounts.refresh()
        after = set(accounts.homes)
        if before != after:
            log.info("accounts refreshed: %s",
                     ", ".join(accounts.label(h) for h in accounts.homes))


async def main():
    setup()
    beat()
    accounts = AccountManager()
    log.info(
        "retro_chat_brain started (model=%s effort=%s) — %d account(s): %s",
        MODEL, EFFORT, len(accounts.homes),
        ", ".join(accounts.label(h) for h in accounts.homes),
    )
    asyncio.create_task(heartbeat_loop())
    asyncio.create_task(account_refresh_loop(accounts))

    sessions = {}          # (host, account_home) -> Agent SDK session_id
    host_queues = {}       # host -> asyncio.Queue of (seq, prompt)
    host_tasks = {}        # host -> worker Task

    async def host_worker(host, q):
        """Process one machine's prompts strictly in order, concurrently with
        other machines. Failover picks the account per prompt."""
        while True:
            seq, prompt = await q.get()
            try:
                log.info("host=%s seq=%s: %.80s", host, seq, prompt.replace("\n", " "))
                await run_prompt(host, seq, prompt, sessions, accounts)
            except Exception:  # noqa: BLE001
                log.exception("worker error host=%s seq=%s", host, seq)
            finally:
                q.task_done()

    def ensure_host(host):
        if host in host_queues:
            return
        q = asyncio.Queue()
        host_queues[host] = q
        acct = accounts.assign(host)
        log.info("new machine %s -> account %s", host, accounts.label(acct))
        host_tasks[host] = asyncio.create_task(host_worker(host, q))

    # Dispatcher: route inbox prompts to per-host queues (fast, non-blocking).
    while True:
        beat()
        for f in sorted(INBOX.glob("*.json")):
            try:
                data = json.loads(f.read_text())
            except Exception:  # noqa: BLE001
                log.warning("bad inbox file %s, removing", f.name)
                f.unlink(missing_ok=True)
                continue
            host = data.get("host")
            seq = data.get("seq")
            prompt = data.get("prompt", "")
            f.unlink(missing_ok=True)  # consume now so it isn't reprocessed
            if not host or seq is None:
                continue
            ensure_host(host)
            host_queues[host].put_nowait((seq, prompt))
        await asyncio.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
