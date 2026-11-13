import asyncio,sys,io
sys.path.insert(0,'/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
from PIL import Image
async def run():
    c=RetroConnection('192.168.1.139',9898); await c.connect('retro-agent-secret',timeout=15)
    d=await c.command_binary('SCREENSHOT 0'); await c.close()
    im=Image.open(io.BytesIO(d)); im.save(sys.argv[1]); print(im.size)
asyncio.run(run())
