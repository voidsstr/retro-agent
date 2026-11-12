import asyncio, json, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
IP = '192.168.1.110'
DESK = r'C:\Documents and Settings\All Users\Desktop'
async def cmd(c, s):
    st, d = await c.send_command(s); return d.decode('latin1', 'replace')
async def main():
    c = RetroConnection(IP, 9898); await c.connect('retro-agent-secret', timeout=15)
    spin = b'Do\r\nLoop\r\n'
    st, d = await c.send_command(r'UPLOAD C:\RETRO_AGENT\spin.vbs', binary_payload=spin)
    print('upload', st, d[:60], flush=True)
    print('launch', (await cmd(c, r'LAUNCH wscript.exe //B //Nologo C:\RETRO_AGENT\spin.vbs')).strip(), flush=True)
    await asyncio.sleep(3)
    pl = json.loads(await cmd(c, 'PROCLIST'))
    procs = pl if isinstance(pl, list) else pl.get('processes', [])
    print('wscript running:', [p for p in procs if 'wscript' in json.dumps(p).lower()][:2], flush=True)
    print('RESET', (await cmd(c, 'GAMESYNC RESET')).strip(), 'START', (await cmd(c, 'GAMESYNC START')).strip(), flush=True)
    t0 = time.time(); seen = False; killed = False; samples = []
    while time.time() - t0 < 900:
        st = json.loads(await cmd(c, 'GAMESYNC STATUS'))
        s = st.get('state')
        samples.append(st)
        print(time.strftime('%T'), s, 'starved_s', st.get('starved_s'), 'stalled_s', st.get('stalled_s'),
              'since_prog', st.get('since_progress_s'), 'cpu', st.get('cpu_busy_pct'), '|', st.get('message'), flush=True)
        if 'STARVED OF CPU' in (st.get('message') or ''):
            seen = True
        if not killed and (seen and (st.get('starved_s') or 0) >= 60 or time.time() - t0 > 420):
            print('killing spinner:', (await cmd(c, 'EXEC taskkill /f /im wscript.exe')).strip(), flush=True)
            killed = True
        if s not in ('sizing', 'copying', 'starting', 'running', 'idle') and killed:
            break
        await asyncio.sleep(10)
    if not killed:
        print('killing spinner:', (await cmd(c, 'EXEC taskkill /f /im wscript.exe')).strip(), flush=True)
    print('delete vbs', (await cmd(c, r'DELETE C:\RETRO_AGENT\spin.vbs')).strip(), flush=True)
    json.dump({'samples': samples, 'seen_starved_message': seen}, open('starve-samples.json', 'w'), indent=1)
    await c.close()
asyncio.run(main())
