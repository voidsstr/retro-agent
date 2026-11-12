#!/usr/bin/env python3
"""DOWNLOAD a fixed list of small per-box config files (read-only) and save them
under evidence/boxes/<box>/<Title>/... Missing files are recorded, not fatal."""
import asyncio, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EVID = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/secondary-shooters-review/boxes'
FILES = [
    r'UnrealGold\System\Unreal.ini',
    r'UnrealTournament\System\UnrealTournament.ini',
    r'UnrealTournament436\System\UnrealTournament.ini',
    r'DeusEx\System\DeusEx.ini',
    r'UT2004\System\UT2004.ini',
    r'Shogo\autoexec.cfg',
    r'SeriousSamFirstEncounter\Scripts\PersistentSymbols.ini',
    r'SeriousSamFirstEncounter\Scripts\Game_startup.ini',
    r'SeriousSamSecondEncounter\Scripts\PersistentSymbols.ini',
    r'SeriousSamSecondEncounter\Scripts\Game_startup.ini',
    r'FarCry\System.cfg',
    r'Tribes2\GameData\base\prefs\ClientPrefs.cs',
    r'BF1942\Mods\bf1942\Settings\Profiles\Default\Video.con',
    r'BF1942\Mods\bf1942\Settings\Profiles\Custom\Video.con',
    r'Turok2\Data\config.ned',
]
async def main(ip, files):
    tag = ip.split('.')[-1]
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        for rel in files:
            path = 'C:\\Games\\' + rel
            st, d = await c.send_command('DOWNLOAD ' + path)
            out = os.path.join(EVID, tag, *rel.split('\\'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if not d.startswith(b"Cannot open file"):
                open(out, 'wb').write(d)
                print('%s OK   %6d  %s' % (tag, len(d), rel))
            else:
                open(out + '.MISSING', 'w').write(d.decode('latin-1', 'replace'))
                print('%s MISS        %s  (%s)' % (tag, rel, d[:80]))
    finally:
        await c.close()
ip = sys.argv[1]
asyncio.run(main(ip, sys.argv[2:] or FILES))
