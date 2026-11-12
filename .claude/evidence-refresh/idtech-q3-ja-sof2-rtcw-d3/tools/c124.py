import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3'
async def main():
    c = RetroConnection('192.168.1.124', 9898)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        st, d = await c.send_command('DIRLIST C:\\', timeout=60)
        ents = json.loads(d.decode('latin-1'))
        logs = [(e['name'], e['size'], e['modified']) for e in ents if e['name'].lower().endswith(('.log', '.txt'))]
        print(logs)
        for n, s, m in logs:
            if 'retrogl' in n.lower() or 'rgl' in n.lower():
                st, data = await c.send_command('DOWNLOAD C:\\' + n, timeout=120)
                if st in (0, 1):
                    open(os.path.join(EV, 'files_124', 'C_' + n), 'wb').write(data)
                    print('downloaded', n, len(data))
    finally:
        await c.close()
asyncio.run(main())
