#!/usr/bin/env node
/* preview_panels.mjs — render the login-screen panels to the terminal.
 *
 * The login screen CANNOT be screenshotted: mutter scans out its own buffers
 * so /dev/fb0 reads back blank, and the greeter's Screenshot D-Bus method
 * refuses to write to disk. So the only way to see what the wall will look
 * like before committing to it is to run the real renderers against real
 * collector output and print the result — which is what this does.
 *
 * It drives extension.js's own `_render*` methods, not a copy of them, so a
 * panel that would throw on a missing field throws here too. Pango markup is
 * stripped back to plain text (with the colour kept as ANSI, since the colour
 * carries meaning in several panels).
 *
 *   sudo python3 dashboard/collector/dashboard_collector.py --once --stdout > /tmp/s.json
 *   node --import ./dashboard/tests/stub-gi.mjs dashboard/tests/preview_panels.mjs /tmp/s.json
 */
import {readFileSync} from 'node:fs';

globalThis.log = () => {};
globalThis.logError = (err, msg) => console.error(`[logError] ${msg}: ${err}`);

const {default: Dashboard} = await import('../extension/extension.js');

/* ---- Pango markup -> ANSI ------------------------------------------- */

function hexToAnsi(hex) {
    const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
    if (!m)
        return '';
    const [r, g, b] = m.slice(1).map(h => parseInt(h, 16));
    return `\x1b[38;2;${r};${g};${b}m`;
}

function toAnsi(markup) {
    let out = '';
    let i = 0;
    const stack = [];
    while (i < markup.length) {
        if (markup.startsWith('<span', i)) {
            const end = markup.indexOf('>', i);
            const attrs = markup.slice(i, end);
            const color = /color="([^"]+)"/.exec(attrs);
            stack.push(color ? hexToAnsi(color[1]) : '');
            out += stack[stack.length - 1];
            i = end + 1;
        } else if (markup.startsWith('</span>', i)) {
            stack.pop();
            out += '\x1b[0m' + (stack[stack.length - 1] ?? '');
            i += 7;
        } else if (markup[i] === '&') {
            const end = markup.indexOf(';', i);
            const ent = markup.slice(i, end + 1);
            out += {'&amp;': '&', '&lt;': '<', '&gt;': '>'}[ent] ?? ent;
            i = end + 1;
        } else {
            out += markup[i++];
        }
    }
    return out + '\x1b[0m';
}

/* ---- a Panel that captures instead of drawing ------------------------ */

class CapturePanel {
    constructor(title) {
        this.title = title;
        this.markup = '';
    }

    setMarkup(markup) {
        this.markup = markup;
    }

    setTitle(text) {
        this.title = text;
    }
}

const PANELS = ['cpu', 'memory', 'disk', 'gpu', 'thermals', 'net',
                'fleet', 'games', 'favs', 'agents', 'remote', 'pxe', 'services', 'sites'];

const state = JSON.parse(readFileSync(process.argv[2] ?? '/dev/stdin', 'utf8'));

const dash = Object.create(Dashboard.prototype);
dash._panels = Object.fromEntries(PANELS.map(k => [k, new CapturePanel(k.toUpperCase())]));
dash._state = state;
dash._stateError = null;
const labels = {};
const mkLabel = key => ({clutter_text: {set_markup: m => (labels[key] = m)}});
dash._headerInfo = mkLabel('header');
dash._heroStats = mkLabel('hero');
dash._footerLeft = mkLabel('footer');
dash._clock = {};
dash._date = {};
dash._renderClock = () => {};

/* Only the panels asked for, so `preview_panels.mjs s.json games pxe` is a
 * tight loop while iterating on one panel's columns. */
const want = process.argv.slice(3);
dash._render();

const width = 72;
console.log(`\n  ${toAnsi(labels.hero ?? '')}\n`);
for (const key of PANELS) {
    if (want.length && !want.includes(key))
        continue;
    const panel = dash._panels[key];
    console.log(`  \x1b[36m╭─ ${panel.title} ${'─'.repeat(
        Math.max(0, width - panel.title.length - 4))}╮\x1b[0m`);
    for (const line of toAnsi(panel.markup).split('\n'))
        console.log(`  \x1b[36m│\x1b[0m ${line}`);
    console.log(`  \x1b[36m╰${'─'.repeat(width)}╯\x1b[0m\n`);
}
