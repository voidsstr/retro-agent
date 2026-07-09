import asyncio, os, sys
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
HERE = os.path.dirname(os.path.abspath(__file__))
async def run(host, exe):
    c = RetroConnection(host, 9898); await c.connect(SECRET, timeout=15.0)
    try:
        data = open(os.path.join(HERE, exe), "rb").read()
        dst = "C:\\WINDOWS\\TEMP\\" + exe
        await c.send_command("UPLOAD " + dst, binary_payload=data)
        print(host, ":", (await c.command_text("EXEC " + dst)).strip() or "ran " + exe)
    finally:
        await c.close()
if __name__ == "__main__":
    asyncio.run(run(sys.argv[1], sys.argv[2]))
