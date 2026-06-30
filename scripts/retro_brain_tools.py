"""Retro fleet tools for the chat brain — an in-process MCP tool server.

Exposes the retro_agent fleet to the Claude Agent SDK as native tools so the
chat brain can actually operate the retro PCs (not just talk about them). Backed
by `client/retro_protocol.RetroConnection`.

Tools (model sees them as ``mcp__retro__<name>``):
  - retro_list_machines : known/discovered agents on the LAN
  - retro_command       : run ANY agent protocol command (SYSINFO, EXEC, VIDEODIAG,
                          REGREAD, UICLICK x y, ...) and return its text
  - retro_screenshot    : capture the screen as a PNG the model can SEE (vision)

Origin host: the machine the chat is coming from is set per-prompt via
``set_origin_host()``; tools default to it when ``host`` is omitted.

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

# Set per-prompt by the brain; tools default to it when host is omitted.
_ORIGIN_HOST = None


def set_origin_host(host):
    """Record the machine the current chat prompt originated from."""
    global _ORIGIN_HOST
    _ORIGIN_HOST = host


def _resolve_host(args):
    host = (args or {}).get("host") or _ORIGIN_HOST
    if not host:
        raise ValueError("no host specified and no origin host set")
    return str(host).strip()


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
    if _ORIGIN_HOST:
        lines.append(f"origin (this chat): {_ORIGIN_HOST}")
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
    "see. Use after LAUNCHing a GUI installer to drive a screenshot→UICLICK loop. "
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
            # Cap the long edge so the base64 payload stays manageable for vision.
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
            return _err(f"BMP→PNG conversion failed ({len(bmp)} bytes): {e}")
        return {
            "content": [
                {"type": "image", "data": b64, "mimeType": "image/png"},
                {"type": "text", "text": f"Screenshot of {host} ({img.width}x{img.height})."},
            ]
        }

    return await _with_conn(host, run)


def build_retro_server():
    """Return the SDK MCP server config to pass to ClaudeAgentOptions.mcp_servers."""
    return create_sdk_mcp_server(
        "retro",
        version="1.0.0",
        tools=[retro_list_machines, retro_command, retro_screenshot],
    )


# Fully-qualified tool names for allowed_tools.
TOOL_NAMES = [
    "mcp__retro__retro_list_machines",
    "mcp__retro__retro_command",
    "mcp__retro__retro_screenshot",
]
