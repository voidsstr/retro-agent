import asyncio, os, sys
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
HERE = os.path.dirname(os.path.abspath(__file__))
async def run(host, extra=""):
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        with open(os.path.join(HERE, "arrange_icons.exe"), "rb") as f:
            exe = f.read()
        await c.send_command("UPLOAD C:\\WINDOWS\\TEMP\\arrange_icons.exe", binary_payload=exe)
        out = await c.command_text("EXEC C:\\WINDOWS\\TEMP\\arrange_icons.exe " + extra)
        print("%s: %s" % (host, out.strip()))
    finally:
        await c.close()
if __name__ == "__main__":
    asyncio.run(run(sys.argv[1], " ".join(sys.argv[2:])))
