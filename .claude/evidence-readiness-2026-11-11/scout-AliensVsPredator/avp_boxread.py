import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = sys.argv[1]
BOXES = sys.argv[2].split(',')
def ror8(x, n): return ((x >> n) | (x << (8 - n))) & 0xff
def obf_state(b):
    if b[:4].lower() == b'grap': return 'PLAIN (de-obfuscated)'
    d = bytes((ror8(c ^ 0xd5, 2) - (i % 0x47)) & 0xff for i, c in enumerate(b[:8]))
    return 'OBFUSCATED (InstallShield)' if d[:4].lower() == b'grap' else 'unknown:' + b[:8].hex()
async def one(o):
    ip = f'192.168.1.{o}'
    res = {'ip': ip}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
        try:
            dl = json.loads(await c.command_text(r'DIRLIST C:\Games\AliensVsPredator', timeout=30))
            ents = dl.get('entries', dl) if isinstance(dl, dict) else dl
            names = {}
            for e in ents:
                n = e.get('name') if isinstance(e, dict) else str(e)
                names[n.lower()] = e
            res['root_entries'] = len(names)
            for k in ('logfile.txt', 'avp.exe', 'fastfile', 'avpkey.cfg', 'ip_address', 'user_profiles', 'graphics'):
                if k in names: res['has_' + k] = names[k]
            res['all_root'] = sorted(names)
        except Exception as e:
            res['dirlist_err'] = repr(e)
        try:
            b = await c.command_binary(r'DOWNLOAD C:\Games\AliensVsPredator\fastfile\ffinfo.txt', timeout=60)
            res['ffinfo'] = {'size': len(b), 'state': obf_state(b)}
        except Exception as e:
            res['ffinfo_err'] = repr(e)
        if 'has_logfile.txt' in res:
            sz = res['has_logfile.txt'].get('size', 0) if isinstance(res['has_logfile.txt'], dict) else 0
            if sz and sz < 2_000_000:
                try:
                    b = await c.command_binary(r'DOWNLOAD C:\Games\AliensVsPredator\LOGFILE.TXT', timeout=90)
                    open(os.path.join(OUT, f'LOGFILE_{o}.TXT'), 'wb').write(b)
                    res['logfile_bytes'] = len(b)
                except Exception as e:
                    res['logfile_err'] = repr(e)
            else:
                res['logfile_skipped_size'] = sz
    except Exception as e:
        res['connect_err'] = repr(e)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return res
async def main():
    rs = await asyncio.gather(*(one(o) for o in BOXES))
    json.dump(rs, open(os.path.join(OUT, 'boxreads.json'), 'w'), indent=1, default=str)
    for r in rs:
        print(r['ip'], {k: v for k, v in r.items() if k not in ('ip', 'all_root')})
asyncio.run(main())
