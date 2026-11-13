import asyncio,sys
sys.path.insert(0,'/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
async def run():
    c=RetroConnection('192.168.1.139',9898); await c.connect('retro-agent-secret',timeout=15)
    for src,dst in zip(sys.argv[1::2],sys.argv[2::2]):
        st,d=await c.send_command('UPLOAD '+dst,binary_payload=open(src,'rb').read()); print(dst,d[:80])
    await c.close()
asyncio.run(run())
