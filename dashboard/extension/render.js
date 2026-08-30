/* render.js — omenfan's visual language, ported from curses to Pango markup.
 *
 * omenfan (voidsstr/omen-fan-control) draws its TUI with braille glyphs: solid
 * ⣿ cells for bars, and an 8-level fill ramp for the sparklines so a graph
 * reads as an area-under-the-curve rather than a row of blocks. Those glyph
 * tables and the muted teal→sage→gold→coral→rose gradient are reproduced here
 * exactly, so the login screen and `sudo python3 -m omenfan` look like the same
 * tool.
 *
 * Everything returns Pango markup for an St.Label, because per-cell colour is
 * what makes the gradient work and a plain label has only one colour.
 */

/* omenfan's GRAD_COLORS_256 resolved to hex. xterm-256 cube:
 * value = 16 + 36r + 6g + b, each channel from [0,95,135,175,215,255]. */
export const GRADIENT = [
    '#5faf87', // 72  teal
    '#87af87', // 108 sage
    '#afaf87', // 144
    '#afd787', // 150
    '#d7d787', // 186 gold
    '#d7af87', // 180
    '#d78787', // 174 coral
    '#d7875f', // 173
    '#d75f5f', // 167
    '#af5f5f', // 131 rose
];

/* The named pairs from omenfan/theme.py, as close as a truecolour display
 * gets to the 16-colour curses originals. */
export const COLORS = {
    title:  '#5fd7d7', // C_TITLE  cyan
    border: '#4a6b8a', // C_BORDER blue, dimmed — it is a frame, not content
    dim:    '#8a95a5', // C_DIM    white/grey
    cool:   '#5faf87', // C_COOL   green
    warm:   '#d7d787', // C_WARM   yellow
    hot:    '#d75f5f', // C_HOT    red
    netRx:  '#5faf87', // C_NET_RX green
    netTx:  '#d787d7', // C_NET_TX magenta
    mem:    '#d7d787', // C_MEM    yellow
    text:   '#d8dee9',
    ok:     '#5faf87',
    off:    '#586170',
};

/* omenfan widgets.py _LINE_FILL: braille bit patterns for 9 fill levels.
 * Odd levels put a single dot at the line position with everything below
 * filled, which is what gives the ramp its smooth leading edge. */
const LINE_FILL = [0x00, 0x80, 0xC0, 0xE0, 0xE4, 0xF4, 0xF6, 0xFE, 0xFF];

const BAR_CELL = '⣿'; // ⣿ — all eight dots

export function escape(text) {
    return String(text ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
}

export function span(color, text, opts = {}) {
    const weight = opts.bold ? ' weight="bold"' : '';
    const alpha = opts.dim ? ' alpha="60%"' : '';
    return `<span color="${color}"${weight}${alpha}>${escape(text)}</span>`;
}

/* Gradient colour for a 0..1 position, matching theme.grad_pair(). */
export function gradAt(frac) {
    const f = Math.max(0, Math.min(1, frac));
    const idx = Math.round(f * (GRADIENT.length - 1));
    return GRADIENT[Math.max(0, Math.min(GRADIENT.length - 1, idx))];
}

/* A value's colour by how alarming it is — omenfan's cool/warm/hot triad. */
export function heat(frac) {
    if (frac < 0.4)
        return COLORS.cool;
    if (frac < 0.7)
        return COLORS.warm;
    return COLORS.hot;
}

/**
 * Braille bar. Each filled cell takes its colour from its own position along
 * the bar, so the gradient encodes *where* you are, not just how full.
 * This is widgets.py `_bar` verbatim.
 */
export function bar(frac, width) {
    const f = Math.max(0, Math.min(1, frac));
    const filled = Math.round(f * width);
    let out = '';
    for (let i = 0; i < filled; i++)
        out += `<span color="${gradAt(i / Math.max(1, width - 1))}">${BAR_CELL}</span>`;
    if (filled < width)
        out += `<span color="${COLORS.off}" alpha="35%">${BAR_CELL.repeat(width - filled)}</span>`;
    return out;
}

/**
 * Filled line graph, widgets.py `_spark`. History is oldest-first; the series
 * is right-aligned so the newest sample sits at the right edge and older data
 * scrolls off the left, the way the TUI does it.
 */
export function spark(history, width, maxVal, color) {
    if (!history || !history.length || !(maxVal > 0))
        return `<span color="${COLORS.off}" alpha="30%">${'⠀'.repeat(width)}</span>`;

    const data = history.slice(-width);
    const pad = width - data.length;
    let out = '';
    if (pad > 0)
        out += '⠀'.repeat(pad);

    let glyphs = '';
    for (const v of data) {
        const level = Math.max(0, Math.min(8, Math.round((v / maxVal) * 8)));
        glyphs += String.fromCharCode(0x2800 + LINE_FILL[level]);
    }
    return `${out}<span color="${color}">${glyphs}</span>`;
}

/** Per-core mini bars — one eighth-block column per core. */
const EIGHTHS = ' ▏▎▍▌▋▊▉█';

export function miniBars(values, maxVal) {
    if (!values || !values.length)
        return '';
    let out = '';
    for (const v of values) {
        const f = Math.max(0, Math.min(1, v / maxVal));
        const ch = EIGHTHS[Math.max(1, Math.round(f * 8))] || EIGHTHS[1];
        out += `<span color="${gradAt(f)}">${escape(ch)}</span>`;
    }
    return out;
}

/* ---------------------------------------------------------------- format */

export function humanBytes(bps) {
    const n = Number(bps) || 0;
    if (n >= 1024 * 1024 * 1024)
        return `${(n / 1073741824).toFixed(1)}G`;
    if (n >= 1024 * 1024)
        return `${(n / 1048576).toFixed(1)}M`;
    if (n >= 1024)
        return `${(n / 1024).toFixed(0)}K`;
    return `${n.toFixed(0)}B`;
}

export function humanUptime(sec) {
    const s = Number(sec) || 0;
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    const m = Math.floor((s % 3600) / 60);
    if (d > 0)
        return `${d}d ${h}h`;
    if (h > 0)
        return `${h}h ${m}m`;
    return `${m}m`;
}

export function humanAge(sec) {
    const s = Number(sec);
    if (!Number.isFinite(s))
        return '—';
    if (s < 90)
        return `${Math.round(s)}s`;
    if (s < 5400)
        return `${Math.round(s / 60)}m`;
    if (s < 172800)
        return `${Math.round(s / 3600)}h`;
    return `${Math.round(s / 86400)}d`;
}

/** Fixed-width left pad/truncate, so columns line up in a monospace label. */
export function pad(text, width) {
    const s = String(text ?? '');
    if (s.length >= width)
        return s.slice(0, width);
    return s + ' '.repeat(width - s.length);
}

export function padLeft(text, width) {
    const s = String(text ?? '');
    if (s.length >= width)
        return s.slice(-width);
    return ' '.repeat(width - s.length) + s;
}

/* ------------------------------------------------------------ status
 *
 * ONE status vocabulary for every panel on this wall.
 *
 * Before this existed each panel invented its own: some drew ● / ○, some
 * ✓ / ✗, some coloured a dot green-or-red and nothing else. Two problems
 * followed, and both are the reason this is now shared code.
 *
 * 1. A red dot in one panel and a red dot in another did not mean the same
 *    thing, so the wall could not be read at a glance -- which is the entire
 *    point of a wall you walk past.
 *
 * 2. Everything that was not plainly healthy collapsed into "bad". But
 *    "nobody ever installed this", "this is switched off on purpose", "this
 *    ran and failed", "this is waiting for a person" and "I could not find
 *    out" are five different calls to action, and only two of them are
 *    faults. Rendering them identically has repeatedly sent us to look at the
 *    wrong thing -- it is the same mistake as a systemd LoadState=not-found
 *    reading as a crash, and as a failed file read reading as an empty file.
 *
 * So: eight states, each with its own glyph AND its own colour (never colour
 * alone -- this is displayed across a room, and the glyph survives both a bad
 * viewing angle and colour-blindness). Faults are the only ones that are red.
 */
export const STATUS = {
    ok:      {glyph: '●', color: COLORS.ok,    rank: 0, label: 'ok'},
    busy:    {glyph: '◐', color: COLORS.title, rank: 1, label: 'running'},
    off:     {glyph: '○', color: COLORS.off,   rank: 2, label: 'off'},
    absent:  {glyph: '·', color: COLORS.off,   rank: 3, label: 'absent'},
    unknown: {glyph: '?', color: COLORS.dim,   rank: 4, label: 'unknown'},
    stale:   {glyph: '⋯', color: COLORS.dim,   rank: 5, label: 'stale'},
    warn:    {glyph: '▲', color: COLORS.warm,  rank: 6, label: 'warn'},
    blocked: {glyph: '‖', color: COLORS.netTx, rank: 7, label: 'blocked'},
    fail:    {glyph: '✕', color: COLORS.hot,   rank: 8, label: 'fail'},
};

/** Only these demand action tonight. Used for the header's fault count. */
export const FAULT_STATES = ['fail', 'blocked', 'warn'];

export function isFault(state) {
    return FAULT_STATES.includes(state);
}

/** The coloured glyph for a state. Unknown names degrade to `unknown`. */
export function statusMark(state) {
    const s = STATUS[state] || STATUS.unknown;
    return `<span color="${s.color}">${escape(s.glyph)}</span>`;
}

/**
 * A standard status line: `● name        detail`.
 *
 * Every panel builds its rows through this, so a row means the same thing
 * wherever it appears. `detail` is already-escaped markup when opts.markup is
 * set, otherwise it is escaped here.
 */
export function statusRow(state, name, detail = '', opts = {}) {
    const s = STATUS[state] || STATUS.unknown;
    const width = opts.width ?? 18;
    const nm = span(opts.nameColor || COLORS.text, pad(name, width));
    const det = opts.markup ? detail : span(s.color, detail);
    return `${statusMark(state)} ${nm} ${det}`;
}

/**
 * Worst state in a list -- what a group's single summary glyph should be.
 * Ranked so a real fault always wins over an "I could not tell", and an
 * "I could not tell" always wins over a healthy sibling: a group is only
 * green when everything in it is actually green.
 */
export function worstStatus(states) {
    let worst = 'ok';
    for (const st of states || []) {
        const a = STATUS[st] || STATUS.unknown;
        if (a.rank > (STATUS[worst] || STATUS.unknown).rank)
            worst = STATUS[st] ? st : 'unknown';
    }
    return worst;
}

/**
 * Age a reading and say whether it can still be trusted.
 *
 * Returns a state, so a panel whose source stopped updating goes `stale`
 * rather than continuing to show its last value as though it were live.
 * A number that is quietly ten hours old is worse than no number, because
 * it is indistinguishable from a working system.
 */
export function freshness(ageSec, softSec, hardSec) {
    const a = Number(ageSec);
    if (!Number.isFinite(a))
        return 'unknown';
    if (a > (hardSec ?? softSec * 4))
        return 'fail';
    if (a > softSec)
        return 'stale';
    return 'ok';
}

/** "as of 4m ago", or an em dash when we have no timestamp at all. */
export function asOf(ageSec) {
    const a = Number(ageSec);
    if (!Number.isFinite(a))
        return 'as of —';
    return `as of ${humanAge(a)} ago`;
}
