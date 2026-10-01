#!/usr/bin/env python3
"""Drive a headless Carmageddon 2 into HOSTING a network game, and keep it there.

Runs INSIDE the carmageddon2-server container (retro-wine:bookworm), next to
Carma2_SW.exe under Wine on an Xvfb display. Pure standard library on purpose:
the image has python3 and no PIL, so the X frame is parsed from `xwd` by hand.

Carmageddon 2 has no dedicated server. Multiplayer is peer-hosted: one player
picks NETWORK GAME -> HOST, waits in the "LOADING STATUS" lobby while others
join, and presses START. This script is that player.

It reads the screen rather than trusting a clock, because every way this can
go wrong looks like a healthy process: the game sits on an intro movie, on the
start screen, or on the network menu with the port bound and nothing hosting.

    state      what the frame shows                 action
    -------    ----------------------------------   -------------------------
    unknown    intro movies / anything unrecognised  Escape (at startup only)
    start      SINGLE PLAYER / NETWORK GAME screen   click NETWORK GAME
    netmenu    HOST A GAME / JOIN A GAME             click HOST
    lobby      LOADING STATUS, the player list       count players; START once
                                                     a remote player has sat
                                                     there START_AFTER_S
    racing     anything else after START was clicked wait; cap RACE_MAX_S

It writes /game/_run/state.json on every pass (state, players, timestamps) -
healthcheck.py reads it, and pairs it with a real CAR2MSG protocol probe.

Exits non-zero when it cannot get to (or stay in) a hosting state; entry.sh
then stops Wine, the container exits and systemd (Restart=always) brings up a
fresh one. One race per container is a deliberate simplification.
"""
import json
import os
import struct
import subprocess
import sys
import time

RUN = os.environ.get('C2_RUN_DIR') or (
    '/game/_run' if os.path.isdir('/game/_run') else os.path.dirname(os.path.abspath(__file__)))
REFS = json.load(open(os.path.join(RUN, 'frame_refs.json')))
STATE_FILE = os.path.join(RUN, 'state.json')

MATCH_MAX = 20            # mean |grey diff| below this = that screen
START_AFTER_S = int(os.environ.get('C2_START_AFTER_S', '45'))
RACE_MAX_S = int(os.environ.get('C2_RACE_MAX_S', '1800'))
BOOT_MAX_S = 240          # from launch to first lobby
ESC_MAX = 6
LOST_MAX_S = 300          # out of lobby/racing this long after hosting = restart

# Screen coordinates (the Wine virtual desktop is 640x480 at the root origin).
NETWORK_GAME = (265, 103)  # "NETWORK GAME" under SINGLE PLAYER
HOST_BUTTON = (330, 396)   # "HOST" on the network menu
START_BUTTON = (495, 446)  # "START" in the lobby

# Lobby player list: one row per player, name column from x=150.
ROWS_Y = (96, 420)
ROWS_X = (150, 450)


def log(msg):
    print(time.strftime('[%H:%M:%S] ') + '[host] ' + msg, flush=True)


def xdo(*args):
    subprocess.run(['xdotool', *map(str, args)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=15)


def click(x, y):
    # A bare `click` is lost by the game's DirectInput polling; it needs the
    # button held for a moment.
    xdo('mousemove', x, y)
    time.sleep(0.3)
    xdo('mousedown', 1)
    time.sleep(0.3)
    xdo('mouseup', 1)


class Frame:
    """The root window, decoded from `xwd -root`.

    Xvfb here reports depth 24 with a 24-bit packed ZPixmap and a DirectColor
    visual once Wine has set up the game's 8-bit palette: each channel byte
    is an index into the colormap. Ignore either fact and the picture is
    striped or false-coloured - which is how the first frames of this work
    looked.
    """

    def __init__(self, raw):
        h = struct.unpack('>25I', raw[:100])
        self.hsize, self.w, self.h = h[0], h[4], h[5]
        self.lsb = h[7] == 0
        self.bpp, self.bpl, vclass, ncolors = h[11], h[12], h[13], h[19]
        self.step = self.bpp // 8
        self.data = raw[self.hsize + ncolors * 12:]
        self.lut = None
        if vclass == 5 and ncolors >= 256:
            cm = [struct.unpack('>IHHHBB', raw[self.hsize + i * 12:self.hsize + i * 12 + 12])
                  for i in range(256)]
            self.lut = ([c[1] >> 8 for c in cm], [c[2] >> 8 for c in cm], [c[3] >> 8 for c in cm])

    def rgb(self, x, y):
        o = y * self.bpl + x * self.step
        b = self.data[o:o + 3]
        r, g, bl = (b[2], b[1], b[0]) if self.lsb else (b[0], b[1], b[2])
        if self.lut:
            r, g, bl = self.lut[0][r], self.lut[1][g], self.lut[2][bl]
        return r, g, bl

    def grey(self, x, y):
        r, g, b = self.rgb(x, y)
        return (r * 299 + g * 587 + b * 114) // 1000


def grab():
    raw = subprocess.run(['xwd', '-root', '-silent'], capture_output=True, timeout=20).stdout
    if len(raw) < 100:
        return None
    try:
        return Frame(raw)
    except (struct.error, IndexError):
        return None


def classify(f):
    """Name the screen, or 'unknown'.

    Each screen is matched on SEVERAL small regions and every one must agree.
    The regions are chosen to avoid the red headings: the lobby's "LOADING
    STATUS" title BLINKS, so a reference that included it matched only every
    other frame and the driver flapped between "lobby" and "unknown".
    """
    best, best_d = 'unknown', 1e9
    for name, refs in REFS.items():
        worst = 0.0
        for ref in refs:
            x0, y0, x1, y1 = ref['box']
            sig = ref['sig']
            i = d = 0
            for y in range(y0, y1, 2):
                for x in range(x0, x1, 2):
                    d += abs(f.grey(x, y) - sig[i])
                    i += 1
            worst = max(worst, d / len(sig))
        if worst < best_d:
            best, best_d = name, worst
    return (best if best_d < MATCH_MAX else 'unknown'), best_d


def count_players(f):
    """Rows of bright-green text in the lobby's name column."""
    rows, in_row = 0, False
    for y in range(*ROWS_Y):
        n = 0
        for x in range(ROWS_X[0], ROWS_X[1], 2):
            r, g, b = f.rgb(x, y)
            if g > 150 and r < 170:
                n += 1
        if n >= 5 and not in_row:
            rows += 1
        in_row = n >= 5
    return rows


def write_state(**kw):
    kw['updated'] = int(time.time())
    tmp = STATE_FILE + '.tmp'
    with open(tmp, 'w') as fh:
        json.dump(kw, fh)
    os.replace(tmp, STATE_FILE)


def snapshot(name):
    try:
        with open(os.path.join(RUN, name + '.xwd'), 'wb') as fh:
            fh.write(subprocess.run(['xwd', '-root', '-silent'], capture_output=True,
                                    timeout=20).stdout)
    except OSError:
        pass


def main():
    t0 = time.time()
    hosting_since = None          # first time the lobby was reached
    last_good = time.time()
    race_started = None
    races = 0
    lobby_players = 0
    lobby_changed = time.time()
    last_esc = 0
    escapes = 0
    seen_menu = False             # once a menu is up, never press Escape again
    prev = None

    while True:
        f = grab()
        state, dist = classify(f) if f else ('noframe', 0)
        now = time.time()

        if state != prev:
            log(f'screen: {state} (diff {dist:.1f})')
            prev = state

        players = 0
        if state in ('start', 'netmenu', 'lobby'):
            seen_menu = True
        if state == 'unknown':
            if race_started:
                state = 'racing'
            elif not seen_menu and escapes < ESC_MAX and now - last_esc > 4:
                # Skip the intro movies. Bounded: an Escape that lands on the
                # start screen instead opens the main menu, and more Escapes
                # there walk towards "quit".
                xdo('key', 'Escape')
                last_esc, escapes = now, escapes + 1
        elif state == 'start':
            click(*NETWORK_GAME)
            time.sleep(3)
        elif state == 'netmenu':
            race_started = None
            click(*HOST_BUTTON)
            time.sleep(3)
        elif state == 'lobby':
            race_started = None
            players = count_players(f)
            if hosting_since is None:
                hosting_since = now
                log(f'HOSTING - lobby is up ({players} player(s))')
                snapshot('hosting')
            if players != lobby_players:
                log(f'lobby: {players} player(s) (was {lobby_players})')
                lobby_players, lobby_changed = players, now
            if players >= 2 and now - lobby_changed >= START_AFTER_S:
                log(f'starting the race: {players} player(s), '
                    f'no change for {START_AFTER_S}s')
                snapshot('race-start')
                click(*START_BUTTON)
                race_started = now
                races += 1
                time.sleep(5)

        if state in ('lobby', 'racing'):
            last_good = now
        write_state(state=state, players=players, hosting_since=hosting_since,
                    race_started=race_started, races=races, pid=os.getpid())

        if hosting_since is None and now - t0 > BOOT_MAX_S:
            log(f'FAILED: no lobby {BOOT_MAX_S}s after launch (stuck on "{state}")')
            snapshot('failed-boot')
            return 2
        if hosting_since is not None and now - last_good > LOST_MAX_S:
            log(f'FAILED: out of the lobby for {LOST_MAX_S}s (on "{state}")')
            snapshot('failed-lost')
            return 3
        if race_started and now - race_started > RACE_MAX_S:
            log(f'race has run {RACE_MAX_S}s - restarting to host a fresh lobby')
            return 4
        time.sleep(3)


if __name__ == '__main__':
    sys.exit(main())
