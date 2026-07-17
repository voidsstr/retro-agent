"""Retro fleet tools for the chat brain — an in-process MCP tool server.

Exposes the retro_agent fleet to the Claude Agent SDK as native tools so the
chat brain can actually operate the retro PCs (not just talk about them). Backed
by `client/retro_protocol.RetroConnection`.

Tools (model sees them as ``mcp__retro__<name>``):
  - retro_list_machines : known/discovered agents on the LAN
  - retro_command       : run ANY agent protocol command (SYSINFO, EXEC, VIDEODIAG,
                          REGREAD, UICLICK x y, ...) and return its text
  - retro_screenshot    : capture the screen as a PNG the model can SEE (vision)

Origin host: the machine a chat is coming from is baked into the MCP server at
construction time — ``build_retro_server(origin_host)`` returns a server whose
tools default to that host when ``host`` is omitted. This is per-query state
captured in a closure (NOT a module global), so many machines can be served
concurrently without their origins clobbering each other.

⚠️ Single-connection caveat: the chat daemon holds a persistent connection to the
originating machine for the chat channel. Operating that *same* machine through
these tools opens a second connection to a single-threaded agent and can contend
with the live chat (brief stalls, not crashes — closes are always graceful).
Prefer operating *other* fleet machines, or run the chat from a different box than
the one being fixed. Targeting a different IP than the origin is always safe.
"""

import base64
import io
import os
import sys

# Make `client.retro_protocol` importable regardless of CWD.
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from client.retro_protocol import RetroConnection, RetroProtocolError  # noqa: E402

try:
    from client.retro_discovery import discover_retro_pcs, get_known_pcs
except Exception:  # discovery is optional
    discover_retro_pcs = None
    get_known_pcs = None

from claude_agent_sdk import create_sdk_mcp_server, tool  # noqa: E402

AGENT_SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
AGENT_PORT = int(os.environ.get("RETRO_AGENT_PORT", "9898"))
CONNECT_TIMEOUT = 15.0
COMMAND_TIMEOUT = 90.0

# ---------------------------------------------------------------------------
# Fleet-safety guardrail (compensating control for the brain's autonomy)
# ---------------------------------------------------------------------------
# The brain runs unattended — there is no human to approve each action — so the
# most damaging, hard-to-reverse agent commands are gated HERE at the tool
# boundary instead of being trusted to prompt discipline alone. A gated command
# is refused unless the caller passes confirm=true, and the system prompt only
# permits confirm=true when the USER has explicitly asked for that action. This
# gives defense in depth: even if the model misjudges, the destructive verb does
# not execute without an explicit, logged confirmation.
#
# Set RETRO_BRAIN_REQUIRE_CONFIRM=0 to disable (not recommended).
_REQUIRE_CONFIRM = os.environ.get("RETRO_BRAIN_REQUIRE_CONFIRM", "1") != "0"

# Agent verbs that are irreversible or need physical access to recover from.
_GATED_VERBS = {"REBOOT", "SHUTDOWN", "QUIT", "PROCKILL", "DELETE", "REGDELETE"}

# Destructive shell substrings inside EXEC/EXECW/LAUNCH payloads.
_GATED_SHELL_PATTERNS = (
    "format ", "fdisk", "diskpart", "deltree", "rd /s", "rmdir /s",
    "del /f /s", "del /s /f", "cipher /w", "reg delete", "rd/s",
)


def _gate_reason(command):
    """Return a human reason if `command` is a gated destructive action, else None."""
    if not command:
        return None
    verb = command.split(None, 1)[0].upper()
    if verb in _GATED_VERBS:
        return f"{verb} is irreversible or needs physical access to recover"
    low = " " + command.lower()
    for pat in _GATED_SHELL_PATTERNS:
        if pat in low:
            return f"command contains a destructive pattern ('{pat.strip()}')"
    return None


def _text(s):
    return {"content": [{"type": "text", "text": s}]}


def _err(s):
    return {"content": [{"type": "text", "text": f"ERROR: {s}"}], "is_error": True}


async def _with_conn(host, fn):
    """Open a graceful connection to `host`, run async `fn(conn)`, always close.

    Graceful close is mandatory — abrupt TCP disconnects crash Win98 Winsock.
    """
    conn = RetroConnection(host, AGENT_PORT)
    try:
        await conn.connect(AGENT_SECRET, timeout=CONNECT_TIMEOUT)
    except Exception as e:  # noqa: BLE001
        return _err(f"could not connect to {host}:{AGENT_PORT} — {e}")
    try:
        return await fn(conn)
    finally:
        try:
            await conn.close()
        except Exception:  # noqa: BLE001
            pass


# Fully-qualified tool names for allowed_tools (stable regardless of origin).
TOOL_NAMES = [
    "mcp__retro__retro_list_machines",
    "mcp__retro__retro_command",
    "mcp__retro__retro_screenshot",
    "mcp__retro__ai_list",
    "mcp__retro__ai_load",
    "mcp__retro__ai_infer",
]


def build_retro_server(origin_host=None):
    """Return an SDK MCP server whose tools default to `origin_host`.

    The origin is captured in a closure so concurrent per-machine chats each
    get their own correctly-scoped tool set — no shared mutable module state.
    """

    def _resolve_host(args):
        host = (args or {}).get("host") or origin_host
        if not host:
            raise ValueError("no host specified and no origin host set")
        return str(host).strip()

    @tool(
        "retro_list_machines",
        "List the retro PCs known/discoverable on the LAN (IP, hostname, OS where "
        "available). Use this to find a machine's IP before operating it.",
        {"type": "object", "properties": {}},
    )
    async def retro_list_machines(args):
        lines = []
        if get_known_pcs is not None:
            try:
                for pc in get_known_pcs() or []:
                    lines.append(f"known: {pc}")
            except Exception:  # noqa: BLE001
                pass
        if discover_retro_pcs is not None:
            try:
                found = await discover_retro_pcs(timeout=3.0)
                for pc in found or []:
                    ip = getattr(pc, "ip", pc)
                    port = getattr(pc, "port", AGENT_PORT)
                    lines.append(f"discovered: {ip}:{port}")
            except Exception as e:  # noqa: BLE001
                lines.append(f"(discovery failed: {e})")
        if origin_host:
            lines.append(f"origin (this chat): {origin_host}")
        return _text("\n".join(lines) if lines else "no machines found")

    @tool(
        "retro_command",
        "Run a single retro_agent protocol command on a fleet machine and return its "
        "text output. Accepts any agent command, e.g. 'SYSINFO', 'VIDEODIAG', "
        "'AUDIOINFO', 'PROCLIST', 'EXEC dir C:\\\\WINDOWS', 'REGREAD HKLM "
        "Software\\\\...', 'UICLICK 320 240', 'UIKEY enter', 'DIRLIST C:\\\\'. "
        "EXEC = hidden CLI (blocks, captures output); LAUNCH = visible GUI app. "
        "Omit 'host' to target the machine this chat is coming from.",
        {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Full agent command line"},
                "host": {"type": "string", "description": "Target IP (default: origin)"},
                "confirm": {
                    "type": "boolean",
                    "description": "Set true ONLY when the user has explicitly asked for a "
                    "destructive/irreversible action (REBOOT, SHUTDOWN, DELETE, REGDELETE, "
                    "PROCKILL, disk-wiping EXEC). Non-destructive commands ignore this.",
                },
            },
            "required": ["command"],
        },
    )
    async def retro_command(args):
        try:
            host = _resolve_host(args)
        except ValueError as e:
            return _err(str(e))
        command = (args.get("command") or "").strip()
        if not command:
            return _err("empty command")

        # Fleet-safety guardrail: destructive actions require explicit confirmation.
        if _REQUIRE_CONFIRM and not bool((args or {}).get("confirm")):
            reason = _gate_reason(command)
            if reason:
                return _err(
                    f"BLOCKED by fleet-safety guardrail: {reason}. "
                    "This is not run automatically. Confirm with the user that they want "
                    f"this exact action on {host}, then retry the SAME command with "
                    "confirm=true. (REBOOT/SHUTDOWN may need physical access to recover.)"
                )

        async def run(conn):
            try:
                out = await conn.command_text(command, timeout=COMMAND_TIMEOUT)
                return _text(out if out else "(no output)")
            except RetroProtocolError as e:
                return _err(f"agent rejected '{command}': {e}")
            except Exception as e:  # noqa: BLE001
                return _err(f"'{command}' failed: {e}")

        return await _with_conn(host, run)

    @tool(
        "retro_screenshot",
        "Capture the screen of a fleet machine and return it as a PNG image you can "
        "see. Use after LAUNCHing a GUI installer to drive a screenshot->UICLICK loop. "
        "quality: 0=full, 1=half, 2=quarter resolution. Omit 'host' for the origin "
        "machine.",
        {
            "type": "object",
            "properties": {
                "host": {"type": "string", "description": "Target IP (default: origin)"},
                "quality": {"type": "integer", "description": "0 full, 1 half, 2 quarter"},
            },
        },
    )
    async def retro_screenshot(args):
        try:
            host = _resolve_host(args)
        except ValueError as e:
            return _err(str(e))
        quality = int((args or {}).get("quality", 0))
        if quality not in (0, 1, 2):
            quality = 0

        async def run(conn):
            try:
                bmp = await conn.command_binary(f"SCREENSHOT {quality}", timeout=COMMAND_TIMEOUT)
            except Exception as e:  # noqa: BLE001
                return _err(f"SCREENSHOT failed: {e}")
            try:
                from PIL import Image

                img = Image.open(io.BytesIO(bmp)).convert("RGB")
                longest = max(img.size)
                if longest > 1400:
                    scale = 1400 / longest
                    img = img.resize(
                        (int(img.width * scale), int(img.height * scale)), Image.LANCZOS
                    )
                buf = io.BytesIO()
                img.save(buf, format="PNG", optimize=True)
                b64 = base64.b64encode(buf.getvalue()).decode("ascii")
            except Exception as e:  # noqa: BLE001
                return _err(f"BMP->PNG conversion failed ({len(bmp)} bytes): {e}")
            return {
                "content": [
                    {"type": "image", "data": b64, "mimeType": "image/png"},
                    {"type": "text", "text": f"Screenshot of {host} ({img.width}x{img.height})."},
                ]
            }

        return await _with_conn(host, run)

    @tool(
        "ai_list",
        "Fleet AI: list which retro machines can run ML (retro-infer engine), with "
        "backend/precision/kernel detail and resident models. Answers 'which "
        "machines can do AI?'. Checks the known fleet + discovery beacon ai flag, "
        "then queries AI_HELLO + MODEL_LIST on each AI-capable box.",
        {"type": "object", "properties": {}},
    )
    async def ai_list(args):
        del args
        hosts = []
        if get_known_pcs is not None:
            try:
                for pc in get_known_pcs() or []:
                    ip = getattr(pc, "ip", None) or str(pc)
                    hosts.append(ip)
            except Exception:  # noqa: BLE001
                pass
        if discover_retro_pcs is not None:
            try:
                for pc in await discover_retro_pcs(timeout=3.0) or []:
                    ip = getattr(pc, "ip", None)
                    if ip and ip not in hosts:
                        hosts.append(ip)
            except Exception:  # noqa: BLE001
                pass
        if origin_host and origin_host not in hosts:
            hosts.append(origin_host)
        if not hosts:
            return _text("no machines found")

        lines = []
        for ip in hosts:
            async def probe(conn):
                import json as _json
                try:
                    hello = _json.loads(await conn.command_text("AI_HELLO", timeout=30))
                except Exception as e:  # noqa: BLE001
                    return f"{ip}: no AI engine ({e})"
                try:
                    models = _json.loads(await conn.command_text("MODEL_LIST", timeout=15))
                    names = [m["name"] for m in models.get("models", [])]
                except Exception:  # noqa: BLE001
                    names = []
                return (f"{ip}: AI READY engine v{hello.get('version')} "
                        f"backends={hello.get('backends')} "
                        f"kernels={hello.get('kernel_f32')}/{hello.get('kernel_i8')} "
                        f"models={names or '(none)'}")

            try:
                res = await _with_conn(ip, probe)
                lines.append(res if isinstance(res, str) else
                             res["content"][0]["text"])
            except Exception as e:  # noqa: BLE001
                lines.append(f"{ip}: unreachable ({e})")
        return _text("\n".join(lines))

    @tool(
        "ai_load",
        "Fleet AI: push a .rim model file from this server's disk to a retro "
        "machine and make it resident for INFER_RUN. model_path is a local path "
        "(e.g. under tools/rim/out/); name is the resident model name.",
        {
            "type": "object",
            "properties": {
                "host": {"type": "string", "description": "Target IP (default: origin)"},
                "name": {"type": "string", "description": "Resident model name (alnum/-/_)"},
                "model_path": {"type": "string", "description": "Local .rim file path"},
            },
            "required": ["name", "model_path"],
        },
    )
    async def ai_load(args):
        try:
            host = _resolve_host(args)
        except ValueError as e:
            return _err(str(e))
        name = (args.get("name") or "").strip()
        path = (args.get("model_path") or "").strip()
        try:
            rim = open(path, "rb").read()
        except OSError as e:
            return _err(f"cannot read {path}: {e}")

        async def run(conn):
            status, data = await conn.send_command(
                f"MODEL_LOAD {name}", binary_payload=rim, timeout=120)
            text = data.decode("ascii", errors="replace")
            if status == 0xFF:
                return _err(f"MODEL_LOAD failed: {text}")
            return _text(f"loaded '{name}' ({len(rim)} bytes) on {host}: {text}")

        return await _with_conn(host, run)

    @tool(
        "ai_infer",
        "Fleet AI: run inference on a resident model on a retro machine. Input is "
        "a local file of raw input bytes (e.g. one 784-byte MNIST image), or omit "
        "input_path and pass sample_index to slice one sample from a local eval "
        "images.bin whose sample size matches the model. Returns the logits and "
        "argmax class.",
        {
            "type": "object",
            "properties": {
                "host": {"type": "string", "description": "Target IP (default: origin)"},
                "name": {"type": "string", "description": "Resident model name"},
                "input_path": {"type": "string", "description": "Local raw input file"},
                "sample_index": {"type": "integer",
                                  "description": "Sample # when input_path holds many"},
                "sample_bytes": {"type": "integer",
                                  "description": "Bytes per sample (with sample_index)"},
            },
            "required": ["name", "input_path"],
        },
    )
    async def ai_infer(args):
        try:
            host = _resolve_host(args)
        except ValueError as e:
            return _err(str(e))
        name = (args.get("name") or "").strip()
        path = (args.get("input_path") or "").strip()
        try:
            blob = open(path, "rb").read()
        except OSError as e:
            return _err(f"cannot read {path}: {e}")
        idx = args.get("sample_index")
        if idx is not None:
            sb = int(args.get("sample_bytes") or 784)
            blob = blob[int(idx) * sb:(int(idx) + 1) * sb]
        if not blob:
            return _err("empty input")

        async def run(conn):
            import struct as _struct
            status, data = await conn.send_command(
                f"INFER_RUN {name}", binary_payload=blob, timeout=120)
            if status == 0xFF:
                return _err(
                    f"INFER_RUN failed: {data.decode('ascii', errors='replace')}")
            n = len(data) // 4
            logits = _struct.unpack(f"<{n}f", data[: n * 4])
            best = max(range(n), key=lambda i: logits[i])
            return _text(
                f"{host} model={name} argmax={best} logits=" +
                "[" + ", ".join(f"{v:.5f}" for v in logits) + "]")

        return await _with_conn(host, run)

    return create_sdk_mcp_server(
        "retro",
        version="1.0.0",
        tools=[retro_list_machines, retro_command, retro_screenshot,
               ai_list, ai_load, ai_infer],
    )
