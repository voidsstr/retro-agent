import asyncio,sys,os
sys.path.insert(0,'/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
HOST=os.environ.get('HOST','192.168.1.139')
async def run():
    c=RetroConnection(HOST,9898); await c.connect('retro-agent-secret',timeout=15)
    for cmd in sys.argv[1:]:
        st,d=await c.send_command(cmd); print('=== ',cmd,'\n',d.decode('utf-8','replace'))
    await c.close()
asyncio.run(run())
