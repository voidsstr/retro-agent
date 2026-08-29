#!/usr/bin/env node
/* Unit tests for the four service panels — game servers, favourites agent,
 * PXE and host services.
 *
 * They drive extension.js's real `_render*` methods through the same stub
 * loader the preview uses, because the thing worth testing is not the string
 * formatting but the DEGENERATE INPUTS. Every one of these panels reads a
 * status file written by a service that may not be running, may be mid-pass,
 * or may have died an hour ago — and a throw inside a renderer means the
 * login screen stops updating. So each panel is exercised against:
 *
 *   - a completely absent section (collector too old, or the service missing)
 *   - the explicit {error: ...} shape the collector emits for a dead service
 *   - a healthy sample
 *   - a failed/degraded sample
 *
 * and asserted on the two mistakes that would actually mislead someone
 * standing at the monitor: bots counted as people, and "not installed"
 * rendered the same as "crashed".
 *
 * Run: node --import ./dashboard/tests/stub-gi.mjs dashboard/tests/test_panels.mjs
 */

globalThis.log = () => {};
globalThis.logError = (err, msg) => {
    throw new Error(`${msg}: ${err}`);
};

const {default: Dashboard} = await import('../extension/extension.js');

let passed = 0;
let failed = 0;

function ok(name, cond, detail = '') {
    if (cond) {
        passed++;
    } else {
        failed++;
        console.log(`  FAIL  ${name}${detail ? ` — ${detail}` : ''}`);
    }
}

function noThrow(name, fn) {
    try {
        fn();
        passed++;
    } catch (err) {
        failed++;
        console.log(`  FAIL  ${name} — threw ${err}`);
    }
}

/* Strip Pango markup so assertions read against what a person would see. */
function plain(markup) {
    return String(markup ?? '')
        .replace(/<[^>]*>/g, '')
        .replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>');
}

class CapturePanel {
    constructor() {
        this.title = '';
        this.markup = '';
    }

    setMarkup(m) {
        this.markup = m;
    }

    setTitle(t) {
        this.title = t;
    }
}

const PANELS = ['cpu', 'memory', 'disk', 'gpu', 'thermals', 'net',
                'fleet', 'games', 'favs', 'agents', 'remote', 'pxe', 'services'];

function render(state) {
    const dash = Object.create(Dashboard.prototype);
    dash._panels = Object.fromEntries(PANELS.map(k => [k, new CapturePanel()]));
    dash._state = state;
    dash._stateError = null;
    const sink = () => ({clutter_text: {set_markup: m => (dash._last = m)}});
    dash._headerInfo = sink();
    dash._heroStats = {clutter_text: {set_markup: m => (dash._hero = m)}};
    dash._footerLeft = sink();
    dash._clock = {};
    dash._date = {};
    dash._renderClock = () => {};
    dash._render();
    return dash;
}

const HEALTHY = {
    ts: Date.now() / 1000,
    gameservers: {
        up: 2, total: 2, players: 4, humans: 0, bots: 4, down: [],
        servers: [
            {unit: 'quake3-server', label: 'Quake III', installed: true, up: true,
             players: 4, bots: 4, max_players: 16, map: 'q3dm7', ping_ms: 0.2,
             unit_state: 'active'},
            {unit: 'cs16-server', label: 'CS 1.6', installed: true, up: true,
             players: 0, bots: 0, max_players: 16, map: 'de_dust2', ping_ms: 11,
             unit_state: 'active'},
            {unit: 'tribes2-server', label: 'Tribes 2', installed: false, up: false,
             unit_state: 'absent'},
        ],
        proxies: [{unit: 'a2s-proxy-cs16', label: 'a2s', port: 27015,
                   installed: true, up: true}],
        watchdog: {enabled: true, actions: []},
    },
    gameindex: {
        ok: true, phase: 'idle', ts: Date.now() / 1000 - 60, duration_sec: 28.9,
        next_pass_at: Date.now() / 1000 + 240,
        agents: ['192.168.1.124', '192.168.1.143'],
        writes: {wrote: 6, unchanged: 12, skipped: 40, failed: 0},
        favorites: {files: 27, boxes: 6}, servers_known: 739, errors: [],
    },
    pxe: {
        state: 'active', sub: 'running', uptime_sec: 19000, unit: 'retro-pxe',
        ports: {proxyDHCP: true, TFTP: true, BINL: true}, serving: true,
        holds: [{mac: 'aa:bb', age_sec: 100}], hold_count: 1,
        last_activity_sec: 133, last_file: '\\i386\\HpAHCIsr.sys',
        recent_clients: ['192.168.1.177'], files_served_recent: 268,
    },
    services: {
        up: 2, total: 2, degraded: [],
        services: [
            {label: 'chat brain', unit: 'retro-chat-brain', scope: 'user',
             state: 'active', sub: 'running', uptime_sec: 100, restarts: 0},
            {label: 'pxe server', unit: 'retro-pxe', scope: 'system',
             state: 'active', sub: 'running', uptime_sec: 200, restarts: 0},
        ],
    },
};

/* ---------------------------------------------- the empty-state contract */

noThrow('renders with every new section absent', () => render({ts: Date.now() / 1000}));
noThrow('renders with a totally empty state object', () => render({}));

{
    // The collector's "service is not running" shape. This is the single most
    // likely real-world input, because the services are only started by hand.
    const d = render({
        ts: Date.now() / 1000,
        gameservers: {error: 'not running', servers: [], up: 0, total: 0},
        gameindex: {error: 'not running'},
        pxe: {state: 'absent'},
        services: {up: 0, total: 0, services: [], degraded: []},
    });
    ok('games panel names the dead watchdog',
        plain(d._panels.games.markup).includes('watchdog not running'),
        plain(d._panels.games.markup));
    ok('games panel says how to start it',
        plain(d._panels.games.markup).includes('retro-gameservers-watch'));
    ok('favourites panel names the dead agent',
        plain(d._panels.favs.markup).includes('favourites agent not running'));
    ok('favourites panel says how to start it',
        plain(d._panels.favs.markup).includes('retro-gameindex'));
}

{
    // The unit is up but the file is missing (a sandbox that cannot reach
    // /run/user, or a first pass still in flight). Telling someone to start a
    // running service sends them in exactly the wrong direction.
    const d = render({
        ts: Date.now() / 1000,
        gameservers: {error: 'running, but no status file yet',
                      hint: '/run/user/1000/retro-gameservers/status.json',
                      servers: [], up: 0, total: 0},
        gameindex: {error: 'running, but no status file yet', hint: '/run/user/1000/x'},
    });
    const games = plain(d._panels.games.markup);
    ok('a live-but-silent watchdog is not called "not running"',
        !games.includes('watchdog not running'), games);
    ok('it points at the file instead of a start command',
        games.includes('waiting on /run/user/1000/retro-gameservers/status.json'),
        games);
    ok('it does not tell you to start a running service',
        !games.includes('systemctl --user start'), games);
    ok('the favourites panel does the same',
        plain(d._panels.favs.markup).includes('waiting on'),
        plain(d._panels.favs.markup));
}

/* -------------------------------------------------------------- healthy */

{
    const d = render(HEALTHY);
    const games = plain(d._panels.games.markup);

    ok('game rows carry player counts', games.includes('4/16'), games);
    ok('game rows carry the map', games.includes('q3dm7'), games);
    ok('game rows carry the query RTT', games.includes('11ms'), games);

    // The bug this catches: a Q3 server pinned at bot_minplayers reports four
    // players forever, and a title reading "4 playing" would have the wall
    // permanently claiming company that is not there.
    ok('bots are not called players in the title',
        !d._panels.games.title.includes('4 playing'), d._panels.games.title);
    ok('bots are labelled as bots in the title',
        d._panels.games.title.includes('4 bots'), d._panels.games.title);
    ok('bot count is shown on the row', games.includes('4 bots'), games);
    ok('bot markup is not escaped into the text',
        !games.includes('span color'), games);

    // A server that was never installed here is not a fault and must not
    // occupy a row that looks like one.
    ok('an uninstalled server gets no row', !games.includes('Tribes 2'), games);
    ok('uninstalled servers are out of the up/total',
        d._panels.games.title.includes('2/2'), d._panels.games.title);

    const favs = plain(d._panels.favs.markup);
    ok('favourites shows boxes reached', favs.includes('2 reached'), favs);
    ok('favourites shows what it wrote', favs.includes('6 written'), favs);
    ok('favourites shows the live-server count', favs.includes('739 known'), favs);
    ok('favourites shows when the next pass is', favs.includes('next'), favs);

    const pxe = plain(d._panels.pxe.markup);
    ok('pxe reports serving', d._panels.pxe.title.includes('serving'),
        d._panels.pxe.title);
    ok('pxe lists its bound ports', pxe.includes('TFTP ✓'), pxe);
    ok('pxe reports boot holds', pxe.includes('1 machine served'), pxe);

    const hero = plain(d._hero);
    ok('hero carries the game-server count', hero.includes('GAMES 2/2'), hero);
    ok('hero carries the service count', hero.includes('SVC 2/2'), hero);
    ok('hero does not count bots as people', !hero.includes('4P'), hero);
}

/* ------------------------------------------------------------- degraded */

{
    const broken = structuredClone(HEALTHY);
    broken.gameservers.up = 1;
    broken.gameservers.down = ['cs16-server'];
    Object.assign(broken.gameservers.servers[1], {
        up: false, unit_state: 'failed', players: null, map: null,
        watchdog: 'unit failed — restarting',
    });
    broken.gameservers.watchdog.actions = [
        {ts: Date.now() / 1000 - 120, unit: 'cs16-server', ok: true,
         reason: 'unit failed', detail: 'restarted'},
    ];
    broken.gameindex = {...broken.gameindex, ok: false, phase: 'failed',
                        errors: ['192.168.1.124: ERROR TimeoutError']};
    broken.pxe = {...broken.pxe, serving: false,
                  ports: {proxyDHCP: true, TFTP: false, BINL: true}};
    broken.services = {
        up: 1, total: 2, degraded: ['retro-gameindex'],
        services: [
            broken.services.services[0],
            {label: 'favourites', unit: 'retro-gameindex', scope: 'user',
             state: 'failed', sub: 'failed', restarts: 3},
            {label: 'gamesrv watch', unit: 'retro-gameservers-watch',
             scope: 'user', state: 'absent'},
        ],
    };

    const d = render(broken);
    const games = plain(d._panels.games.markup);
    ok('a down server says DOWN', games.includes('DOWN'), games);
    ok('a down server says why', games.includes('unit failed'), games);
    ok('the watchdog reports what it restarted',
        games.includes('restarted cs16-server'), games);

    const favs = plain(d._panels.favs.markup);
    ok('a failed pass says failed', favs.includes('failed'), favs);
    ok('the pass error is shown verbatim',
        favs.includes('TimeoutError'), favs);

    const pxe = plain(d._panels.pxe.markup);
    ok('an active pxe with no TFTP socket is not called serving',
        !d._panels.pxe.title.includes('serving'), d._panels.pxe.title);
    ok('the missing socket is marked', pxe.includes('TFTP ✗'), pxe);

    const svc = plain(d._panels.services.markup);
    ok('a failed service is listed', svc.includes('failed'), svc);
    ok('restart count is surfaced', svc.includes('3r'), svc);
    // "never installed" and "crashed" are different calls to action.
    ok('an absent unit reads as not installed',
        svc.includes('not installed'), svc);
    ok('degraded units are named', svc.includes('retro-gameindex not running'), svc);
}

/* ------------------------------------- a server with no player count */

{
    // Tribes 2: alive, but TribesNext encrypts its info response so the count
    // is unknowable. Rendering that as `0` asserts an empty server we cannot
    // actually see into.
    const t2 = structuredClone(HEALTHY);
    t2.gameservers.servers.push({
        unit: 'tribes2-server', label: 'Tribes 2', installed: true, up: true,
        manager: 'docker', players: null, max_players: null, map: null,
        ping_ms: 0.9, unit_state: 'active',
    });
    t2.gameservers.up = 3;
    t2.gameservers.total = 3;
    const d = render(t2);
    const games = plain(d._panels.games.markup);
    const row = games.split('\n').find(l => l.includes('Tribes 2'));
    ok('a server with no player count gets a row', !!row, games);
    ok('an unknown player count is not printed as 0',
        row && !/\b0\b/.test(row), row);
    ok('an unknown player count reads as unknown',
        row && row.includes('—'), row);
    ok('it still shows its ping', row && row.includes('0.9ms') || row.includes('1ms'),
        row);
}

/* --------------------------------------------------------------- stale */

{
    const stale = structuredClone(HEALTHY);
    stale.gameservers.stale_sec = 900;
    stale.gameindex.stale_sec = 1800;
    const d = render(stale);
    ok('a silent watchdog is called out',
        plain(d._panels.games.markup).includes('watchdog silent'),
        plain(d._panels.games.markup));
    ok('a missed favourites pass is called out',
        plain(d._panels.favs.markup).includes('no pass for'),
        plain(d._panels.favs.markup));
}

/* ---------------------------------------------------- partial / garbage */

noThrow('game server row with every optional field missing', () => render({
    ts: Date.now() / 1000,
    gameservers: {up: 1, total: 1, servers: [{unit: 'x', label: 'X',
                                             installed: true, up: true}]},
}));
noThrow('favourites report mid-pass with no timings', () => render({
    ts: Date.now() / 1000,
    gameindex: {phase: 'probing servers', agents: [], writes: {}},
}));
noThrow('pxe with no log history at all', () => render({
    ts: Date.now() / 1000, pxe: {state: 'active', ports: {}},
}));
noThrow('services list that is empty', () => render({
    ts: Date.now() / 1000, services: {services: [], up: 0, total: 0},
}));

/* ------------------------------------------- columns must not collide */

{
    // Real fleet hostnames run to 16 characters (NSC-5B996B81319), which
    // exactly filled the old 15-wide column and ran into the OS field:
    // "NSC-5B996B81319Win5.1".
    const d = render({
        ts: Date.now() / 1000,
        fleet: {up: 1, total: 1, nodes: [
            {ip: '192.168.1.171', label: 'x', up: true,
             name: 'NSC-5B996B81319', os: 'Win5.1', rtt_ms: 5},
        ]},
    });
    const row = plain(d._panels.fleet.markup).split('\n')[0];
    ok('a 16-char hostname does not touch the OS column',
        !/[0-9A-Z]Win5\.1/.test(row), row);
    ok('the hostname is still shown in full',
        row.includes('NSC-5B996B81319'), row);
}

{
    const d = render({
        ts: Date.now() / 1000,
        gameindex: {ok: true, phase: 'idle', ts: Date.now() / 1000,
                    agents: [], writes: {}, favorites: {},
                    servers_known: 675, errors: []},
    });
    const favs = plain(d._panels.favs.markup);
    ok('the live-server label does not run into its value',
        !favs.includes('live servers675'), favs);
    ok('the value is still there', favs.includes('675 known'), favs);
}

console.log('');
console.log(`  ${passed} passed, ${failed} failed`);
if (failed > 0)
    process.exit(1);
