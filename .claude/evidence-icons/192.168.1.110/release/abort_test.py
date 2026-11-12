import asyncio, json, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
DESK = r'C:\Documents and Settings\All Users\Desktop'
async def cmd(c, s):
    st, d = await c.send_command(s); return d.decode('latin1', 'replace')
async def lnkn(c):
    return len([e for e in json.loads(await cmd(c, 'DIRLIST ' + DESK)) if e['name'].lower().endswith('.lnk')])
async def main():
    c = RetroConnection('192.168.1.110', 9898); await c.connect('retro-agent-secret', timeout=15)
    print(time.strftime('%T'), 'before lnk', await lnkn(c), flush=True)
    print('RESET', (await cmd(c, 'GAMESYNC RESET')).strip(), 'START', (await cmd(c, 'GAMESYNC START')).strip(), flush=True)
    for _ in range(100):
        st = json.loads(await cmd(c, 'GAMESYNC STATUS'))
        if st.get('state') == 'sizing': break
        await asyncio.sleep(0.1)
    print(time.strftime('%T'), 'at abort:', st.get('state'), st.get('message'), flush=True)
    print('ABORT ->', (await cmd(c, 'GAMESYNC ABORT')).strip(), flush=True)
    for _ in range(120):
        st = json.loads(await cmd(c, 'GAMESYNC STATUS'))
        print(time.strftime('%T'), st.get('state'), st.get('message'), 'lnk', await lnkn(c), flush=True)
        if st.get('state') not in ('sizing', 'copying', 'starting', 'running'): break
        await asyncio.sleep(1)
    json.dump(st, open('abort-status.json', 'w'), indent=1)
    await asyncio.sleep(3)
    print(time.strftime('%T'), 'after lnk', await lnkn(c), flush=True)
    await c.close()
asyncio.run(main())
