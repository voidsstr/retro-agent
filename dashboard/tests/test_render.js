#!/usr/bin/gjs -m
/* Unit tests for the dashboard's render primitives.
 *
 * These matter more than they look: the bar/sparkline code is the only part
 * of the extension that runs inside the login screen doing real work, and a
 * thrown exception there is a greeter that draws nothing. So every function
 * is exercised with the degenerate inputs the collector can genuinely produce
 * — empty history on first boot, a zero maximum on an idle NIC, a percentage
 * over 100 from a busy load average.
 *
 * Run: gjs -m dashboard/tests/test_render.js
 */

import * as R from '../extension/render.js';

let passed = 0;
let failed = 0;

function ok(name, cond, detail = '') {
    if (cond) {
        passed++;
    } else {
        failed++;
        print(`  FAIL  ${name}${detail ? ` — ${detail}` : ''}`);
    }
}

function eq(name, actual, expected) {
    ok(name, actual === expected, `got ${JSON.stringify(actual)}, want ${JSON.stringify(expected)}`);
}

function noThrow(name, fn) {
    try {
        fn();
        passed++;
    } catch (err) {
        failed++;
        print(`  FAIL  ${name} — threw ${err}`);
    }
}

print('render.js primitives');

/* ---------------------------------------------------------------- escape */
eq('escape ampersand', R.escape('a & b'), 'a &amp; b');
eq('escape angles', R.escape('<x>'), '&lt;x&gt;');
eq('escape null', R.escape(null), '');
eq('escape undefined', R.escape(undefined), '');

/* Markup safety: a hostname from a fleet box is attacker-adjacent data that
 * ends up inside Pango markup. It must always come back escaped. */
ok('span escapes its text', R.span('#fff', '<b>x</b>').includes('&lt;b&gt;'));

/* ------------------------------------------------------------- gradient */
eq('gradAt clamps low', R.gradAt(-5), R.GRADIENT[0]);
eq('gradAt clamps high', R.gradAt(99), R.GRADIENT[R.GRADIENT.length - 1]);
ok('gradAt midpoint is mid palette', R.GRADIENT.includes(R.gradAt(0.5)));
eq('gradient length matches omenfan', R.GRADIENT.length, 10);

eq('heat cool', R.heat(0.1), R.COLORS.cool);
eq('heat warm', R.heat(0.5), R.COLORS.warm);
eq('heat hot', R.heat(0.95), R.COLORS.hot);

/* ------------------------------------------------------------------ bar */
noThrow('bar at zero', () => R.bar(0, 10));
noThrow('bar at full', () => R.bar(1, 10));
noThrow('bar over 100%', () => R.bar(4.2, 10));
noThrow('bar negative', () => R.bar(-1, 10));
noThrow('bar zero width', () => R.bar(0.5, 0));
ok('bar renders filled cells', R.bar(1, 6).includes('⣿'));
ok('empty bar still spans full width', R.bar(0, 6).includes('⣿'.repeat(6)));

/* --------------------------------------------------------------- spark */
noThrow('spark empty history', () => R.spark([], 20, 100, '#fff'));
noThrow('spark null history', () => R.spark(null, 20, 100, '#fff'));
noThrow('spark zero max', () => R.spark([1, 2, 3], 20, 0, '#fff'));
noThrow('spark negative max', () => R.spark([1, 2, 3], 20, -5, '#fff'));
noThrow('spark values over max', () => R.spark([500, 900], 20, 100, '#fff'));
noThrow('spark longer than width', () =>
    R.spark(Array.from({length: 500}, (_, i) => i % 100), 20, 100, '#fff'));

/* A sparkline must always occupy exactly `width` cells or the monospace
 * columns to its left drift out of alignment. */
{
    const markup = R.spark([10, 20, 30], 20, 100, '#fff');
    const glyphs = markup.replace(/<[^>]*>/g, '');
    eq('spark pads to full width', [...glyphs].length, 20);
}
{
    const markup = R.spark([], 12, 100, '#fff');
    const glyphs = markup.replace(/<[^>]*>/g, '');
    eq('empty spark pads to full width', [...glyphs].length, 12);
}

/* ------------------------------------------------------------ miniBars */
noThrow('miniBars empty', () => R.miniBars([], 100));
noThrow('miniBars null', () => R.miniBars(null, 100));
noThrow('miniBars zero max', () => R.miniBars([1, 2], 0));
{
    const glyphs = R.miniBars([0, 50, 100], 100).replace(/<[^>]*>/g, '');
    eq('miniBars one glyph per core', [...glyphs].length, 3);
}

/* ------------------------------------------------------------- humanize */
eq('bytes B', R.humanBytes(512), '512B');
eq('bytes K', R.humanBytes(2048), '2K');
eq('bytes M', R.humanBytes(5 * 1048576), '5.0M');
eq('bytes G', R.humanBytes(3 * 1073741824), '3.0G');
eq('bytes zero', R.humanBytes(0), '0B');
eq('bytes null', R.humanBytes(null), '0B');

eq('uptime minutes', R.humanUptime(600), '10m');
eq('uptime hours', R.humanUptime(7200), '2h 0m');
eq('uptime days', R.humanUptime(260000), '3d 0h');
eq('uptime zero', R.humanUptime(0), '0m');

eq('age seconds', R.humanAge(30), '30s');
eq('age minutes', R.humanAge(600), '10m');
eq('age hours', R.humanAge(7200), '2h');
eq('age days', R.humanAge(400000), '5d');
eq('age non-numeric', R.humanAge(undefined), '—');

/* ------------------------------------------------------------------ pad */
eq('pad short', R.pad('ab', 5), 'ab   ');
eq('pad exact', R.pad('abcde', 5), 'abcde');
eq('pad truncates', R.pad('abcdefgh', 5), 'abcde');
eq('pad null', R.pad(null, 3), '   ');
eq('padLeft short', R.padLeft('7', 4), '   7');
eq('padLeft truncates from left', R.padLeft('123456', 3), '456');

/* -------------------------------------------------------------- summary */
print('');
print(`  ${passed} passed, ${failed} failed`);
if (failed > 0)
    imports.system.exit(1);
