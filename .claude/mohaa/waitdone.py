import asyncio, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
IP = sys.argv[1]; MARK = sys.argv[2]
async def main():
    while True:
        try:
            c = RetroConnection(IP, 9898)
            await c.connect('retro-agent-secret', timeout=15.0)
            st, d = await c.send_command('EXEC cmd /c if exist %s echo YESDONE' % MARK, timeout=90)
            out = d.decode('ascii', 'replace')
            await c.close()
            if 'YESDONE' in out:
                print('DONE marker present'); return
        except Exception as e:
            print('poll error: %s' % e)
        await asyncio.sleep(30)
asyncio.run(main())
