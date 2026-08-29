/* Retro Fleet Dashboard — a full-bleed status wall on the GDM login screen.
 *
 * Runs in `gnome-shell --mode=gdm` only (see metadata.json session-modes).
 * GNOME Shell loads it there because extensionSystem.js's
 * _extensionSupportsSessionMode matches the mode name against session-modes.
 *
 * SAFETY, which governs almost every choice in this file: a wedged greeter is
 * a machine nobody can log into. Therefore —
 *
 *   1. The overlay is `reactive: false`. It never takes a grab and never
 *      consumes an event, so the login dialog underneath keeps working even
 *      if every other line of this file is broken. Input is *observed* via a
 *      `captured-event` handler that always returns EVENT_PROPAGATE.
 *   2. Nothing here blocks. The state file is read with
 *      load_contents_async(); there is no synchronous I/O on the main loop.
 *   3. Every timer, signal and actor is tracked and torn down in disable().
 *   4. Anything that throws is caught and shown as a dead-state panel rather
 *      than propagating into the shell.
 *
 * Data comes from dashboard/collector/dashboard_collector.py via
 * /run/retro-dashboard/state.json — /run and not /tmp, because the greeter
 * runs as a systemd DynamicUser and PrivateTmp=yes hides /tmp from it.
 */

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import * as R from './render.js';

const STATE_PATH = '/run/retro-dashboard/state.json';

const REFRESH_MS = 2000;   // matches the collector's fast loop
const CLOCK_MS = 1000;
/* How long after the last keypress/mouse move before the wall comes back.
 * Long enough that reading a password prompt does not flip it mid-typing. */
const IDLE_RETURN_MS = 45000;
const FADE_MS = 400;

/* A sample older than this means the collector died; say so rather than
 * showing stale numbers as if they were live. */
const STALE_AFTER_SEC = 15;

const SPARK_W = 24;
const BAR_W = 14;


/* ------------------------------------------------------------------ panel */

/** One titled box, drawn like omenfan's rounded `_box`. */
class Panel {
    constructor(title, opts = {}) {
        this.actor = new St.BoxLayout({
            style_class: 'rfd-panel',
            vertical: true,
            x_expand: opts.xExpand ?? true,
        });

        if (title) {
            this._title = new St.Label({
                style_class: 'rfd-panel-title',
                text: title,
            });
            this.actor.add_child(this._title);
        }

        this._body = new St.Label({style_class: 'rfd-panel-body'});
        this._body.clutter_text.use_markup = true;
        this._body.clutter_text.line_wrap = false;
        this.actor.add_child(this._body);
    }

    setMarkup(markup) {
        // set_markup on the ClutterText, not St.Label.text — the latter would
        // escape our spans instead of interpreting them.
        this._body.clutter_text.set_markup(markup);
    }

    setTitle(text) {
        if (this._title)
            this._title.text = text;
    }

    hide() {
        this.actor.hide();
    }

    show() {
        this.actor.show();
    }
}


/* -------------------------------------------------------------- extension */

export default class RetroFleetDashboard extends Extension {
    enable() {
        this._state = null;
        this._stateError = null;
        this._lastActivity = GLib.get_monotonic_time();
        this._dismissed = false;
        this._destroyed = false;
        this._reading = false;
        this._timers = [];
        this._stageHandlerId = 0;

        try {
            this._build();
        } catch (err) {
            // A failure here must not take the login screen with it.
            logError(err, 'retro-fleet-dashboard: build failed');
            this._teardown();
            return;
        }

        this._stageHandlerId = global.stage.connect(
            'captured-event', this._onCapturedEvent.bind(this));

        this._addTimer(REFRESH_MS, () => {
            this._readState();
            return GLib.SOURCE_CONTINUE;
        });
        this._addTimer(CLOCK_MS, () => {
            this._renderClock();
            this._maybeReturn();
            return GLib.SOURCE_CONTINUE;
        });

        this._readState();
        this._render();
        log('retro-fleet-dashboard: enabled');
    }

    disable() {
        this._teardown();
    }

    _teardown() {
        this._destroyed = true;

        for (const id of this._timers ?? [])
            GLib.Source.remove(id);
        this._timers = [];

        if (this._stageHandlerId) {
            global.stage.disconnect(this._stageHandlerId);
            this._stageHandlerId = 0;
        }

        this._overlay?.destroy();
        this._overlay = null;
        this._panels = null;
        this._state = null;
    }

    _addTimer(intervalMs, fn) {
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, intervalMs, () => {
            if (this._destroyed)
                return GLib.SOURCE_REMOVE;
            try {
                return fn();
            } catch (err) {
                logError(err, 'retro-fleet-dashboard: timer');
                return GLib.SOURCE_CONTINUE;
            }
        });
        this._timers.push(id);
        return id;
    }

    /* ------------------------------------------------------------ layout */

    _build() {
        // reactive:false is the safety property — see the header comment.
        this._overlay = new St.Widget({
            style_class: 'rfd-overlay',
            layout_manager: new Clutter.BinLayout(),
            reactive: false,
            can_focus: false,
            track_hover: false,
            x_expand: true,
            y_expand: true,
        });

        const root = new St.BoxLayout({
            style_class: 'rfd-root',
            vertical: true,
            x_expand: true,
            y_expand: true,
        });
        this._overlay.add_child(root);

        /* --- header ------------------------------------------------- */
        const header = new St.BoxLayout({style_class: 'rfd-header', x_expand: true});
        this._headerTitle = new St.Label({
            style_class: 'rfd-header-title',
            text: 'OMEN · FLEET CONTROL',
        });
        this._headerInfo = new St.Label({style_class: 'rfd-header-info'});
        this._headerInfo.clutter_text.use_markup = true;
        header.add_child(this._headerTitle);
        header.add_child(new St.Widget({x_expand: true}));
        header.add_child(this._headerInfo);
        root.add_child(header);

        /* --- hero clock --------------------------------------------- */
        const hero = new St.BoxLayout({style_class: 'rfd-hero', x_expand: true});
        const heroLeft = new St.BoxLayout({vertical: true});
        this._clock = new St.Label({style_class: 'rfd-clock', text: '--:--'});
        this._date = new St.Label({style_class: 'rfd-date', text: ''});
        heroLeft.add_child(this._clock);
        heroLeft.add_child(this._date);
        hero.add_child(heroLeft);
        hero.add_child(new St.Widget({x_expand: true}));

        this._heroStats = new St.Label({style_class: 'rfd-hero-stats'});
        this._heroStats.clutter_text.use_markup = true;
        hero.add_child(this._heroStats);
        root.add_child(hero);

        /* --- three columns of panels -------------------------------- */
        const columns = new St.BoxLayout({
            style_class: 'rfd-columns',
            x_expand: true,
            y_expand: true,
        });

        const mkCol = () => {
            const c = new St.BoxLayout({
                style_class: 'rfd-column',
                vertical: true,
                x_expand: true,
            });
            columns.add_child(c);
            return c;
        };

        const col1 = mkCol();
        const col2 = mkCol();
        const col3 = mkCol();

        this._panels = {
            cpu:      new Panel('CPU'),
            memory:   new Panel('MEMORY'),
            disk:     new Panel('DISK'),
            gpu:      new Panel('GPU'),
            thermals: new Panel('TEMPERATURES'),
            net:      new Panel('NETWORK'),
            fleet:    new Panel('FLEET'),
            games:    new Panel('GAME SERVERS'),
            favs:     new Panel('FAVOURITES'),
            agents:   new Panel('AGENTS'),
            remote:   new Panel('REMOTE'),
            pxe:      new Panel('PXE'),
            services: new Panel('SERVICES'),
        };

        /* Three columns, not four. The physical monitor is 1600x1200, so
         * width is the scarce dimension — a fourth column would cut each
         * panel to about 40 monospace characters and start truncating server
         * names and file paths. Height is not scarce: the original three
         * columns used barely half of it. So the new panels go on the bottom
         * of the existing columns rather than beside them. */
        col1.add_child(this._panels.cpu.actor);
        col1.add_child(this._panels.memory.actor);
        col1.add_child(this._panels.disk.actor);
        col1.add_child(this._panels.remote.actor);

        col2.add_child(this._panels.gpu.actor);
        col2.add_child(this._panels.thermals.actor);
        col2.add_child(this._panels.net.actor);
        col2.add_child(this._panels.pxe.actor);
        col2.add_child(this._panels.services.actor);

        col3.add_child(this._panels.fleet.actor);
        col3.add_child(this._panels.games.actor);
        col3.add_child(this._panels.favs.actor);
        col3.add_child(this._panels.agents.actor);

        root.add_child(columns);

        /* --- footer hint -------------------------------------------- */
        const footer = new St.BoxLayout({style_class: 'rfd-footer', x_expand: true});
        this._footerLeft = new St.Label({style_class: 'rfd-footer-note'});
        this._footerLeft.clutter_text.use_markup = true;
        this._hint = new St.Label({
            style_class: 'rfd-hint',
            text: 'press any key to log in',
        });
        footer.add_child(this._footerLeft);
        footer.add_child(new St.Widget({x_expand: true}));
        footer.add_child(this._hint);
        footer.add_child(new St.Widget({x_expand: true}));
        root.add_child(footer);

        // screenShieldGroup is the greeter's top layer; ScreenShield added its
        // own children in its constructor, so appending here puts the wall
        // above the login dialog rather than behind it.
        Main.layoutManager.screenShieldGroup.add_child(this._overlay);
        this._trackGeometry();
    }

    /** Keep the overlay covering the whole primary monitor across hotplug. */
    _trackGeometry() {
        const fit = () => {
            const mon = Main.layoutManager.primaryMonitor;
            if (!mon || !this._overlay)
                return;
            this._overlay.set_position(mon.x, mon.y);
            this._overlay.set_size(mon.width, mon.height);
        };
        fit();
        this._monitorsId = Main.layoutManager.connect('monitors-changed', fit);
    }

    /* ------------------------------------------------------ dismiss/return */

    _onCapturedEvent(_actor, event) {
        // Never consume: always fall through to the login dialog. The whole
        // point is that this handler is invisible to the rest of the shell.
        try {
            const type = event.type();
            if (type === Clutter.EventType.KEY_PRESS ||
                type === Clutter.EventType.BUTTON_PRESS ||
                type === Clutter.EventType.TOUCH_BEGIN ||
                type === Clutter.EventType.MOTION ||
                type === Clutter.EventType.SCROLL) {
                this._lastActivity = GLib.get_monotonic_time();
                if (!this._dismissed)
                    this._dismiss();
            }
        } catch (err) {
            logError(err, 'retro-fleet-dashboard: captured-event');
        }
        return Clutter.EVENT_PROPAGATE;
    }

    _dismiss() {
        this._dismissed = true;
        if (!this._overlay)
            return;
        this._overlay.ease({
            opacity: 0,
            duration: FADE_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => this._overlay?.hide(),
        });
    }

    _restore() {
        this._dismissed = false;
        if (!this._overlay)
            return;
        this._overlay.show();
        this._overlay.ease({
            opacity: 255,
            duration: FADE_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
    }

    _maybeReturn() {
        if (!this._dismissed)
            return;
        const idleMs = (GLib.get_monotonic_time() - this._lastActivity) / 1000;
        if (idleMs >= IDLE_RETURN_MS)
            this._restore();
    }

    /* --------------------------------------------------------- data read */

    _readState() {
        if (this._reading || this._destroyed)
            return;
        this._reading = true;

        const file = Gio.File.new_for_path(STATE_PATH);
        file.load_contents_async(null, (src, res) => {
            this._reading = false;
            if (this._destroyed)
                return;
            try {
                const [ok, contents] = src.load_contents_finish(res);
                if (!ok)
                    throw new Error('load_contents returned false');
                const text = new TextDecoder().decode(contents);
                this._state = JSON.parse(text);
                this._stateError = null;
            } catch (err) {
                this._state = null;
                this._stateError = err.message ?? String(err);
            }
            try {
                this._render();
            } catch (err) {
                logError(err, 'retro-fleet-dashboard: render');
            }
        });
    }

    /* ------------------------------------------------------------ render */

    _renderClock() {
        if (!this._clock)
            return;
        const now = GLib.DateTime.new_now_local();
        this._clock.text = now.format('%H:%M');
        this._date.text = now.format('%A %-d %B');
    }

    _render() {
        if (!this._panels)
            return;
        this._renderClock();

        const s = this._state;
        if (!s) {
            const why = this._stateError ?? 'no data';
            this._headerInfo.clutter_text.set_markup(
                R.span(R.COLORS.hot, 'collector offline', {bold: true}));
            this._heroStats.clutter_text.set_markup(
                R.span(R.COLORS.dim, why));
            for (const key of Object.keys(this._panels)) {
                this._panels[key].setMarkup(
                    R.span(R.COLORS.off, `  ${STATE_PATH}\n  unavailable`));
            }
            this._footerLeft.clutter_text.set_markup('');
            return;
        }

        const ageSec = Math.max(0, Date.now() / 1000 - (s.ts ?? 0));
        const stale = ageSec > STALE_AFTER_SEC;

        // The login screen cannot be screenshotted (mutter scans out its own
        // buffers and the greeter's Screenshot D-Bus method refuses to write
        // to disk), so this one line is how you confirm from a shell that the
        // wall is actually drawing. Logged only on transitions, never per
        // frame — a 2s heartbeat would flood the journal.
        const fleet = s.fleet ?? {};
        const games = s.gameservers ?? {};
        const svc = s.services ?? {};
        const beat = `${stale ? 'stale' : 'live'} ${fleet.up ?? 0}/${fleet.total ?? 0} ` +
            `${games.up ?? 0}/${games.total ?? 0} ${svc.up ?? 0}/${svc.total ?? 0}`;
        if (this._lastBeat !== beat) {
            this._lastBeat = beat;
            log(`retro-fleet-dashboard: rendering (${stale ? 'stale' : 'live'}), ` +
                `fleet ${fleet.up ?? 0}/${fleet.total ?? 0} up, ` +
                `game servers ${games.up ?? 0}/${games.total ?? 0}, ` +
                `services ${svc.up ?? 0}/${svc.total ?? 0}`);
        }

        this._renderHeader(s, stale, ageSec);
        this._renderHero(s);
        this._renderCpu(s);
        this._renderMemory(s);
        this._renderDisk(s);
        this._renderGpu(s);
        this._renderThermals(s);
        this._renderNet(s);
        this._renderFleet(s);
        this._renderGames(s);
        this._renderFavourites(s);
        this._renderAgents(s);
        this._renderPxe(s);
        this._renderServices(s);
        this._renderRemote(s);
    }

    _renderHeader(s, stale, ageSec) {
        const h = s.host ?? {};
        const parts = [];
        if (h.cpu_model)
            parts.push(R.span(R.COLORS.dim, h.cpu_model));
        if (h.cpu_cores)
            parts.push(R.span(R.COLORS.dim, `${h.cpu_cores}C`));
        if (h.ram_gb)
            parts.push(R.span(R.COLORS.dim, `${Math.round(h.ram_gb)} GB`));
        if (h.distro)
            parts.push(R.span(R.COLORS.dim, h.distro));
        if (stale) {
            parts.push(R.span(R.COLORS.hot,
                `stale ${R.humanAge(ageSec)}`, {bold: true}));
        }
        this._headerInfo.clutter_text.set_markup(
            parts.join(R.span(R.COLORS.off, '  ·  ')));
    }

    _renderHero(s) {
        const h = s.host ?? {};
        const cpu = s.cpu ?? {};
        const gpu = s.gpu ?? {};
        const fleet = s.fleet ?? {};

        const line = [];
        if (h.uptime_sec)
            line.push(R.span(R.COLORS.dim, `up ${R.humanUptime(h.uptime_sec)}`));

        const cpuPct = cpu.usage_pct ?? 0;
        line.push(`${R.span(R.COLORS.dim, 'CPU ')}${
            R.span(R.heat(cpuPct / 100), `${Math.round(cpuPct)}%`, {bold: true})}`);

        if (gpu.util_pct !== undefined) {
            line.push(`${R.span(R.COLORS.dim, 'GPU ')}${
                R.span(R.heat((gpu.util_pct ?? 0) / 100),
                    `${gpu.util_pct}%`, {bold: true})}`);
        }
        if (gpu.temp_c) {
            line.push(`${R.span(R.COLORS.dim, '')}${
                R.span(R.heat(gpu.temp_c / 90), `${gpu.temp_c}°`, {bold: true})}`);
        }
        if (fleet.total) {
            const up = fleet.up ?? 0;
            line.push(`${R.span(R.COLORS.dim, 'FLEET ')}${
                R.span(up > 0 ? R.COLORS.ok : R.COLORS.off,
                    `${up}/${fleet.total}`, {bold: true})}`);
        }

        /* Game servers and host services are the two counts that should make
         * someone walk over to the machine, so they sit in the hero line
         * beside the fleet count rather than only inside their panels. Unlike
         * the fleet — which is powered on demand and legitimately all-down —
         * anything less than every server up is a fault worth colouring. */
        const games = s.gameservers ?? {};
        if (games.total) {
            const allUp = games.up === games.total;
            line.push(`${R.span(R.COLORS.dim, 'GAMES ')}${
                R.span(allUp ? R.COLORS.ok : R.COLORS.hot,
                    `${games.up}/${games.total}`, {bold: true})}`);
            // Same reason as the panel title: bots must not read as company.
            if (games.humans)
                line.push(R.span(R.COLORS.warm, `${games.humans}P`, {bold: true}));
        }
        const svc = s.services ?? {};
        if (svc.total) {
            const allUp = svc.up === svc.total;
            line.push(`${R.span(R.COLORS.dim, 'SVC ')}${
                R.span(allUp ? R.COLORS.ok : R.COLORS.hot,
                    `${svc.up}/${svc.total}`, {bold: true})}`);
        }
        this._heroStats.clutter_text.set_markup(
            line.join(R.span(R.COLORS.off, '   ')));
    }

    _renderCpu(s) {
        const cpu = s.cpu;
        if (!cpu)
            return this._panels.cpu.setMarkup(R.span(R.COLORS.off, '  no data'));

        const hist = s.history ?? {};
        const rows = [];

        const usage = (cpu.usage_pct ?? 0) / 100;
        rows.push(`${R.span(R.COLORS.dim, R.pad('Usage', 6))}${R.bar(usage, BAR_W)} ${
            R.span(R.heat(usage), R.padLeft(`${Math.round(cpu.usage_pct)}%`, 4), {bold: true})}`);

        if (cpu.freq_mhz) {
            const maxGhz = (cpu.freq_max_mhz || 6000) / 1000;
            const ghz = cpu.freq_mhz / 1000;
            rows.push(`${R.span(R.COLORS.dim, R.pad('Freq', 6))}${
                R.bar(ghz / maxGhz, BAR_W)} ${
                R.span(R.COLORS.title, R.padLeft(`${ghz.toFixed(1)}G`, 4), {bold: true})}`);
        }

        const load = cpu.load ?? [];
        if (load.length === 3) {
            const cores = (s.host?.cpu_cores) || 1;
            const lc = R.heat(load[0] / cores);
            rows.push(`${R.span(R.COLORS.dim, R.pad('Load', 6))}${
                R.span(lc, load.map(l => l.toFixed(2)).join('  '))}   ${
                R.span(R.COLORS.dim, `${cpu.proc_running}/${cpu.proc_total}P`, {dim: true})}`);
        }

        rows.push(`${R.span(R.COLORS.dim, R.pad('use', 6), {dim: true})}${
            R.spark(hist.cpu, SPARK_W, 100, R.COLORS.title)}`);

        if (hist.cpu_temp?.length) {
            const last = hist.cpu_temp[hist.cpu_temp.length - 1];
            rows.push(`${R.span(R.COLORS.dim, R.pad('tmp', 6), {dim: true})}${
                R.spark(hist.cpu_temp, SPARK_W, 100, R.heat(last / 90))} ${
                R.span(R.heat(last / 90), `${Math.round(last)}°`)}`);
        }

        if (cpu.per_core_pct?.length) {
            rows.push(`${R.span(R.COLORS.dim, R.pad('cores', 6), {dim: true})}${
                R.miniBars(cpu.per_core_pct, 100)}`);
        }

        this._panels.cpu.setMarkup(rows.join('\n'));
    }

    _renderMemory(s) {
        const m = s.memory;
        if (!m)
            return this._panels.memory.setMarkup(R.span(R.COLORS.off, '  no data'));

        const rows = [];
        const used = (m.used_pct ?? 0) / 100;
        rows.push(`${R.span(R.COLORS.dim, R.pad('RAM', 6))}${R.bar(used, BAR_W)} ${
            R.span(R.heat(used), R.padLeft(`${Math.round(m.used_pct)}%`, 4), {bold: true})}`);
        rows.push(`${R.span(R.COLORS.dim, R.pad('', 6))}${
            R.span(R.COLORS.dim, `${m.used_gb} / ${m.total_gb} GB`, {dim: true})}   ${
            R.span(R.COLORS.dim, `cache ${m.cached_gb}G`, {dim: true})}`);

        if (m.swap_total_gb > 0) {
            const sw = (m.swap_used_pct ?? 0) / 100;
            rows.push(`${R.span(R.COLORS.dim, R.pad('Swap', 6))}${R.bar(sw, BAR_W)} ${
                R.span(R.heat(sw), R.padLeft(`${Math.round(m.swap_used_pct)}%`, 4), {bold: true})}`);
        }

        const hist = s.history ?? {};
        rows.push(`${R.span(R.COLORS.dim, R.pad('use', 6), {dim: true})}${
            R.spark(hist.mem, SPARK_W, 100, R.COLORS.mem)}`);

        this._panels.memory.setMarkup(rows.join('\n'));
    }

    _renderDisk(s) {
        const disks = s.disks ?? [];
        const io = s.disk_io ?? {};
        if (!disks.length)
            return this._panels.disk.setMarkup(R.span(R.COLORS.off, '  no data'));

        const rows = [];
        for (const d of disks.slice(0, 3)) {
            const f = (d.used_pct ?? 0) / 100;
            rows.push(`${R.span(R.COLORS.dim, R.pad(d.mount, 6))}${R.bar(f, BAR_W)} ${
                R.span(R.heat(f), R.padLeft(`${Math.round(d.used_pct)}%`, 4), {bold: true})}`);
            rows.push(`${R.span(R.COLORS.dim, R.pad('', 6))}${
                R.span(R.COLORS.dim, `${Math.round(d.used_gb)} / ${Math.round(d.total_gb)} GB`, {dim: true})}${
                d.temp_c ? `   ${R.span(R.heat(d.temp_c / 70), `${d.temp_c}°`)}` : ''}`);
        }
        rows.push(`${R.span(R.COLORS.dim, R.pad('I/O', 6), {dim: true})}${
            R.span(R.COLORS.netRx, `r ${R.humanBytes(io.read_bps)}/s`)}   ${
            R.span(R.COLORS.netTx, `w ${R.humanBytes(io.write_bps)}/s`)}`);

        this._panels.disk.setMarkup(rows.join('\n'));
    }

    _renderGpu(s) {
        const g = s.gpu;
        if (!g) {
            this._panels.gpu.setMarkup(R.span(R.COLORS.off, '  no NVIDIA GPU'));
            return;
        }
        const hist = s.history ?? {};
        const rows = [];
        rows.push(R.span(R.COLORS.title, g.name ?? 'GPU', {bold: true}));

        const util = (g.util_pct ?? 0) / 100;
        rows.push(`${R.span(R.COLORS.dim, R.pad('Util', 6))}${R.bar(util, BAR_W)} ${
            R.span(R.heat(util), R.padLeft(`${g.util_pct}%`, 4), {bold: true})}`);

        if (g.temp_c) {
            const t = g.temp_c / 90;
            rows.push(`${R.span(R.COLORS.dim, R.pad('Temp', 6))}${R.bar(t, BAR_W)} ${
                R.span(R.heat(t), R.padLeft(`${g.temp_c}°`, 4), {bold: true})}`);
        }
        if (g.vram_total_mb) {
            const v = (g.vram_used_pct ?? 0) / 100;
            rows.push(`${R.span(R.COLORS.dim, R.pad('VRAM', 6))}${R.bar(v, BAR_W)} ${
                R.span(R.heat(v), R.padLeft(`${Math.round(g.vram_used_pct)}%`, 4), {bold: true})}`);
            rows.push(`${R.span(R.COLORS.dim, R.pad('', 6))}${
                R.span(R.COLORS.dim,
                    `${(g.vram_used_mb / 1024).toFixed(1)} / ${(g.vram_total_mb / 1024).toFixed(1)} GB`,
                    {dim: true})}`);
        }

        const tail = [];
        if (g.fan_pct !== undefined && g.fan_pct !== null)
            tail.push(R.span(R.COLORS.cool, `fan ${g.fan_pct}%`));
        if (g.power_w)
            tail.push(R.span(R.COLORS.warm, `${g.power_w}W`));
        if (g.pstate)
            tail.push(R.span(R.COLORS.dim, g.pstate, {dim: true}));
        if (tail.length)
            rows.push(`${R.pad('', 6)}${tail.join(R.span(R.COLORS.off, '  ·  '))}`);

        rows.push(`${R.span(R.COLORS.dim, R.pad('util', 6), {dim: true})}${
            R.spark(hist.gpu, SPARK_W, 100, R.COLORS.cool)}`);

        this._panels.gpu.setMarkup(rows.join('\n'));
    }

    _renderThermals(s) {
        const t = s.thermals ?? [];
        const fans = s.fans ?? [];
        if (!t.length && !fans.length)
            return this._panels.thermals.setMarkup(R.span(R.COLORS.off, '  no sensors'));

        const rows = [];
        // Collapse the repeated coretemp packages the kernel exposes; the
        // hottest one is the number that actually matters.
        const seen = new Map();
        for (const z of t) {
            const prev = seen.get(z.name);
            if (prev === undefined || z.temp_c > prev)
                seen.set(z.name, z.temp_c);
        }
        const sorted = [...seen.entries()].sort((a, b) => b[1] - a[1]).slice(0, 5);
        for (const [name, temp] of sorted) {
            const f = temp / 90;
            rows.push(`${R.span(R.COLORS.dim, R.pad(name, 10))}${R.bar(f, 10)} ${
                R.span(R.heat(f), R.padLeft(`${Math.round(temp)}°`, 4), {bold: true})}`);
        }
        for (const fan of fans) {
            const label = R.pad(`fan ${fan.name}`, 10);
            const value = fan.rpm ? `${fan.rpm} rpm` : `${fan.pct}%`;
            const f = fan.pct ? fan.pct / 100 : 0.3;
            rows.push(`${R.span(R.COLORS.dim, label)}${R.bar(f, 10)} ${
                R.span(R.COLORS.cool, R.padLeft(value, 7))}`);
        }
        this._panels.thermals.setMarkup(rows.join('\n'));
    }

    _renderNet(s) {
        const nets = s.net ?? [];
        const hist = s.history ?? {};
        if (!nets.length)
            return this._panels.net.setMarkup(R.span(R.COLORS.off, '  no interfaces'));

        const rows = [];
        for (const n of nets.slice(0, 3)) {
            rows.push(`${R.span(R.COLORS.title, R.pad(n.name, 12))}${
                R.span(R.COLORS.netRx, `↓ ${R.padLeft(R.humanBytes(n.rx_bps), 6)}/s`)}  ${
                R.span(R.COLORS.netTx, `↑ ${R.padLeft(R.humanBytes(n.tx_bps), 6)}/s`)}`);
        }
        rows.push(`${R.span(R.COLORS.dim, R.pad('rx', 6), {dim: true})}${
            R.spark(hist.net_rx, SPARK_W, Math.max(1, ...(hist.net_rx ?? [1])), R.COLORS.netRx)}`);
        rows.push(`${R.span(R.COLORS.dim, R.pad('tx', 6), {dim: true})}${
            R.spark(hist.net_tx, SPARK_W, Math.max(1, ...(hist.net_tx ?? [1])), R.COLORS.netTx)}`);

        this._panels.net.setMarkup(rows.join('\n'));
    }

    _renderFleet(s) {
        const fleet = s.fleet ?? {};
        const nodes = fleet.nodes ?? [];
        this._panels.fleet.setTitle(
            `FLEET   ${fleet.up ?? 0}/${fleet.total ?? 0} up`);

        if (fleet.disabled)
            return this._panels.fleet.setMarkup(R.span(R.COLORS.off, '  polling disabled'));
        if (!nodes.length)
            return this._panels.fleet.setMarkup(R.span(R.COLORS.off, '  no nodes configured'));

        const rows = [];
        for (const n of nodes.slice(0, 10)) {
            const dot = n.up
                ? R.span(R.COLORS.ok, '●')
                : R.span(R.COLORS.off, '○');
            const name = n.up ? (n.name || n.label) : n.label;
            const nameCol = R.span(n.up ? R.COLORS.text : R.COLORS.off, R.pad(name, 15));
            const detail = n.up
                ? R.span(R.COLORS.dim, `${R.pad(n.os ?? '', 8)}${R.padLeft(`${n.rtt_ms}ms`, 6)}`, {dim: true})
                : R.span(R.COLORS.off, R.pad('offline', 14), {dim: true});
            rows.push(`${dot} ${nameCol}${detail}`);
        }
        // The retro fleet is powered on demand, so an all-offline list is the
        // resting state, not a fault — say so instead of looking broken.
        if ((fleet.up ?? 0) === 0)
            rows.push(R.span(R.COLORS.off, '\n  fleet powered down', {dim: true}));

        this._panels.fleet.setMarkup(rows.join('\n'));
    }

    /* ---------------------------------------------------------- games */

    _renderGames(s) {
        const g = s.gameservers ?? {};
        const servers = g.servers ?? [];

        if (g.error) {
            this._panels.games.setTitle('GAME SERVERS');
            // "the watchdog is not running" and "the servers are down" are
            // completely different problems; never let one read as the other.
            // And when the collector says the unit is actually up, do not tell
            // anyone to start it — point at the file it cannot read instead.
            const fix = g.hint
                ? `  waiting on ${g.hint}`
                : '  systemctl --user start retro-gameservers-watch';
            return this._panels.games.setMarkup(
                `${R.span(R.COLORS.warm, `  watchdog ${g.error}`)}\n${
                    R.span(R.COLORS.off, fix, {dim: true})}`);
        }

        const allUp = g.total > 0 && g.up === g.total;
        /* Humans, not players. A Q3 arena sitting at bot_minplayers 4 reports
         * four players forever; a title that says "4 playing" would have the
         * wall permanently claiming someone is on the server. */
        const humans = g.humans ?? 0;
        const bots = g.bots ?? 0;
        this._panels.games.setTitle(
            `GAME SERVERS   ${g.up ?? 0}/${g.total ?? 0} up` +
            (humans ? `  ·  ${humans} playing` : '') +
            (!humans && bots ? `  ·  ${bots} bots` : ''));

        if (!servers.length)
            return this._panels.games.setMarkup(R.span(R.COLORS.off, '  no servers configured'));

        const rows = [];
        for (const srv of servers) {
            if (srv.installed === false)
                continue;   // never installed here — not a fault, not a row
            const dot = srv.up
                ? R.span(R.COLORS.ok, '●')
                : R.span(R.COLORS.hot, '●');
            const name = R.span(srv.up ? R.COLORS.text : R.COLORS.hot,
                R.pad(srv.label, 16));

            if (!srv.up) {
                // Say what systemd thinks, and what the watchdog is doing
                // about it — a red dot with no reason sends you to a terminal.
                const why = srv.watchdog || `unit ${srv.unit_state}`;
                rows.push(`${dot} ${name}${R.span(R.COLORS.hot, R.pad('DOWN', 7))}${
                    R.span(R.COLORS.warm, why.slice(0, 30), {dim: true})}`);
                continue;
            }

            // Null is "this engine will not tell us", not zero. Tribes 2
            // under TribesNext encrypts its info response, so printing 0
            // there would assert an empty server we cannot actually see into.
            const known = srv.players !== null && srv.players !== undefined;
            const cap = known && srv.max_players ? `/${srv.max_players}` : '';
            const count = known ? `${srv.players}${cap}` : '—';
            // Colour by occupancy: an empty server is normal, a busy one is
            // the thing you want to notice from across the room.
            const pcol = (srv.players ?? 0) > (srv.bots ?? 0)
                ? R.COLORS.warm : R.COLORS.dim;
            const ping = srv.ping_ms === null || srv.ping_ms === undefined
                ? '—' : `${Math.round(srv.ping_ms)}ms`;
            // Build the tail out of finished spans and concatenate. Wrapping
            // markup in another R.span() escapes it, and the bot count came
            // out as literal `<span color=...>4b</span>` on the wall.
            const mapText = srv.map ?? '';
            const tail = srv.bots
                ? R.span(R.COLORS.dim, R.pad(mapText, 13), {dim: true}) +
                  R.span(R.COLORS.off, `${srv.bots} bots`, {dim: true})
                : R.span(R.COLORS.dim, mapText, {dim: true});
            rows.push(`${dot} ${name}${R.span(pcol, R.padLeft(count, 6), {bold: true})}${
                R.span(R.COLORS.dim, R.padLeft(ping, 7), {dim: true})}  ${tail}`);
        }

        const proxies = (g.proxies ?? []).filter(x => x.installed);
        if (proxies.length) {
            rows.push(R.span(R.COLORS.off,
                `  browser proxies  ${proxies.map(
                    x => `${x.port} ${x.up ? '✓' : '✗'}`).join('  ')}`, {dim: true}));
        }

        const wd = g.watchdog ?? {};
        const acts = wd.actions ?? [];
        if (acts.length) {
            const a = acts[0];
            const ago = R.humanAge(Date.now() / 1000 - (a.ts ?? 0));
            rows.push(R.span(a.ok ? R.COLORS.warm : R.COLORS.hot,
                `  watchdog ${a.ok ? 'restarted' : 'FAILED to restart'} ${a.unit} ${ago} ago`));
        } else if (wd.enabled === false) {
            rows.push(R.span(R.COLORS.warm, '  watchdog: restarts disabled', {dim: true}));
        } else if (allUp) {
            rows.push(R.span(R.COLORS.off, '  all servers up · watchdog armed', {dim: true}));
        }
        if (g.stale_sec) {
            rows.push(R.span(R.COLORS.hot,
                `  watchdog silent ${R.humanAge(g.stale_sec)}`, {bold: true}));
        }

        this._panels.games.setMarkup(rows.join('\n'));
    }

    /* ----------------------------------------------------- favourites */

    _renderFavourites(s) {
        const f = s.gameindex ?? {};
        if (f.error) {
            this._panels.favs.setTitle('FAVOURITES');
            const fix = f.hint
                ? `  waiting on ${f.hint}`
                : '  systemctl --user start retro-gameindex';
            return this._panels.favs.setMarkup(
                `${R.span(R.COLORS.warm, `  favourites agent ${f.error}`)}\n${
                    R.span(R.COLORS.off, fix, {dim: true})}`);
        }

        const running = f.phase && f.phase !== 'idle' && f.phase !== 'failed';
        const bad = f.ok === false || f.phase === 'failed' || f.stale_sec;
        const col = bad ? R.COLORS.hot : (running ? R.COLORS.warm : R.COLORS.ok);
        this._panels.favs.setTitle('FAVOURITES AGENT');

        const rows = [];
        rows.push(`${R.span(col, '●')} ${
            R.span(R.COLORS.text, R.pad('server lists', 14))}${
            R.span(col, R.pad(running ? f.phase : (bad ? 'failed' : 'idle'), 20))}`);

        const w = f.writes ?? {};
        const fav = f.favorites ?? {};
        const ago = f.ts ? R.humanAge(Date.now() / 1000 - f.ts) : '—';
        const next = f.next_pass_at
            ? R.humanAge(Math.max(0, f.next_pass_at - Date.now() / 1000))
            : '—';
        rows.push(`  ${R.span(R.COLORS.dim, R.pad('last pass', 12), {dim: true})}${
            R.span(R.COLORS.text, `${ago} ago`)}${
            f.duration_sec ? R.span(R.COLORS.dim, ` (${f.duration_sec}s)`, {dim: true}) : ''}${
            R.span(R.COLORS.off, ` · next ${next}`, {dim: true})}`);

        const boxes = (f.agents ?? []).map(ip => ip.split('.').slice(-1)[0]);
        rows.push(`  ${R.span(R.COLORS.dim, R.pad('boxes', 12), {dim: true})}${
            R.span(boxes.length ? R.COLORS.ok : R.COLORS.off,
                `${boxes.length} reached`)}${
            boxes.length ? R.span(R.COLORS.dim, `  .${boxes.join(' .')}`, {dim: true}) : ''}`);

        rows.push(`  ${R.span(R.COLORS.dim, R.pad('favourites', 12), {dim: true})}${
            R.span(w.wrote ? R.COLORS.warm : R.COLORS.dim, `${w.wrote ?? 0} written`)}${
            R.span(R.COLORS.dim, `  ${w.unchanged ?? 0} same`, {dim: true})}${
            w.failed ? R.span(R.COLORS.hot, `  ${w.failed} failed`, {bold: true}) : ''}`);

        rows.push(`  ${R.span(R.COLORS.dim, R.pad('live servers', 12), {dim: true})}${
            R.span(R.COLORS.text, `${f.servers_known ?? 0} known`)}${
            fav.files ? R.span(R.COLORS.dim,
                `  · ${fav.files} files on ${fav.boxes} boxes`, {dim: true}) : ''}`);

        // Errors are the point of a status wall: show the first, verbatim.
        for (const err of (f.errors ?? []).slice(0, 2))
            rows.push(R.span(R.COLORS.hot, `  ${err.slice(0, 52)}`, {dim: true}));
        if (f.stale_sec) {
            rows.push(R.span(R.COLORS.hot,
                `  no pass for ${R.humanAge(f.stale_sec)}`, {bold: true}));
        }

        this._panels.favs.setMarkup(rows.join('\n'));
    }

    /* ------------------------------------------------------------- pxe */

    _renderPxe(s) {
        const p = s.pxe ?? {};
        const active = p.state === 'active';
        const serving = !!p.serving;
        this._panels.pxe.setTitle(
            `PXE   ${serving ? 'serving' : (active ? 'no sockets' : (p.state ?? '—'))}`);

        const rows = [];
        const col = serving ? R.COLORS.ok : (active ? R.COLORS.warm : R.COLORS.hot);
        rows.push(`${R.span(col, serving ? '●' : '○')} ${
            R.span(R.COLORS.text, R.pad('retro-pxe', 12))}${
            R.span(col, R.pad(p.state ?? 'unknown', 10))}${
            p.uptime_sec !== undefined
                ? R.span(R.COLORS.dim, `up ${R.humanUptime(p.uptime_sec)}`, {dim: true}) : ''}`);

        // An `active` unit that has lost its sockets serves nothing while
        // looking perfectly healthy, so the bound ports get their own line.
        const ports = p.ports ?? {};
        const portBits = Object.keys(ports).map(name =>
            R.span(ports[name] ? R.COLORS.ok : R.COLORS.hot,
                `${name} ${ports[name] ? '✓' : '✗'}`));
        if (portBits.length) {
            rows.push(`  ${R.span(R.COLORS.dim, R.pad('ports', 12), {dim: true})}${
                portBits.join(R.span(R.COLORS.off, '  '))}`);
        }

        rows.push(`  ${R.span(R.COLORS.dim, R.pad('boot holds', 12), {dim: true})}${
            R.span(p.hold_count ? R.COLORS.text : R.COLORS.off,
                `${p.hold_count ?? 0} machine${p.hold_count === 1 ? '' : 's'} served`)}${
            R.span(R.COLORS.off, ' · will not reinstall', {dim: true})}`);

        if (p.last_activity_sec !== undefined) {
            const clients = (p.recent_clients ?? []).slice(0, 2).join(' ');
            rows.push(`  ${R.span(R.COLORS.dim, R.pad('last seen', 12), {dim: true})}${
                R.span(R.COLORS.text, `${R.humanAge(p.last_activity_sec)} ago`)}${
                clients ? R.span(R.COLORS.dim, `  ${clients}`, {dim: true}) : ''}`);
        }
        if (p.last_file) {
            rows.push(`  ${R.span(R.COLORS.dim, R.pad('last file', 12), {dim: true})}${
                R.span(R.COLORS.dim, p.last_file.slice(-40), {dim: true})}`);
        }
        if (p.files_served_recent) {
            rows.push(`  ${R.span(R.COLORS.dim, R.pad('served', 12), {dim: true})}${
                R.span(R.COLORS.dim, `${p.files_served_recent} files recently`, {dim: true})}`);
        }

        this._panels.pxe.setMarkup(rows.join('\n'));
    }

    /* -------------------------------------------------------- services */

    _renderServices(s) {
        const sv = s.services ?? {};
        const rows = [];
        const list = sv.services ?? [];
        this._panels.services.setTitle(
            `SERVICES   ${sv.up ?? 0}/${sv.total ?? 0} up`);

        if (!list.length)
            return this._panels.services.setMarkup(R.span(R.COLORS.off, '  no data'));

        for (const svc of list) {
            const ok = svc.state === 'active';
            // `absent` means the unit was never installed on this host, which
            // is a different call to action from a unit that has failed.
            const missing = svc.state === 'absent';
            const col = ok ? R.COLORS.ok : (missing ? R.COLORS.off : R.COLORS.hot);
            const detail = ok && svc.uptime_sec !== undefined
                ? `up ${R.humanUptime(svc.uptime_sec)}`
                : (missing ? 'not installed' : (svc.sub || svc.result || ''));
            rows.push(`${R.span(col, ok ? '●' : '○')} ${
                R.span(ok ? R.COLORS.text : col, R.pad(svc.label, 15))}${
                R.span(col, R.pad(svc.state ?? '—', 10))}${
                R.span(R.COLORS.dim, detail, {dim: true})}${
                svc.restarts ? R.span(R.COLORS.warm, `  ${svc.restarts}r`, {dim: true}) : ''}`);
        }

        if ((sv.degraded ?? []).length) {
            rows.push(R.span(R.COLORS.hot,
                `  ${sv.degraded.join(', ')} not running`, {bold: true}));
        }

        this._panels.services.setMarkup(rows.join('\n'));
    }

    _renderAgents(s) {
        const a = s.agents ?? {};
        const rows = [];

        const stateColor = st => {
            if (st === 'running' || st === 'alive')
                return R.COLORS.ok;
            if (st === 'stale' || st === 'unknown')
                return R.COLORS.warm;
            return R.COLORS.off;
        };
        const dot = st => (st === 'running' || st === 'alive' ? '●' : '○');

        const d = a.daemon ?? {};
        rows.push(`${R.span(stateColor(d.state), dot(d.state))} ${
            R.span(R.COLORS.text, R.pad('chat daemon', 13))}${
            R.span(stateColor(d.state), R.pad(d.state ?? '—', 9))}${
            d.pid ? R.span(R.COLORS.dim, `pid ${d.pid}`, {dim: true}) : ''}`);

        const b = a.brain ?? {};
        rows.push(`${R.span(stateColor(b.state), dot(b.state))} ${
            R.span(R.COLORS.text, R.pad('chat brain', 13))}${
            R.span(stateColor(b.state), R.pad(b.state ?? '—', 9))}${
            b.age_sec !== undefined
                ? R.span(R.COLORS.dim, `hb ${R.humanAge(b.age_sec)}`, {dim: true})
                : ''}`);

        const q = a.queue ?? {};
        const h = a.history ?? {};
        rows.push(`${R.span(R.COLORS.dim, '  ' + R.pad('queue', 13), {dim: true})}${
            R.span(q.inbox ? R.COLORS.warm : R.COLORS.dim, `${q.inbox ?? 0} in`)}  ${
            R.span(q.outbox ? R.COLORS.warm : R.COLORS.dim, `${q.outbox ?? 0} out`)}  ${
            R.span(q.failed ? R.COLORS.hot : R.COLORS.dim, `${q.failed ?? 0} fail`)}`);
        if (h.prompts !== undefined) {
            rows.push(`${R.span(R.COLORS.dim, '  ' + R.pad('history', 13), {dim: true})}${
                R.span(R.COLORS.dim, `${h.prompts} prompts · ${h.hosts} hosts`, {dim: true})}`);
        }

        const runs = a.runs ?? [];
        if (runs.length) {
            rows.push('');
            rows.push(R.span(R.COLORS.title, '  current work', {bold: true}));
            for (const r of runs.slice(0, 4)) {
                const prog = r.progress ?? {};
                const bits = [];
                if (prog.epoch !== undefined)
                    bits.push(`e${prog.epoch}`);
                if (prog.step !== undefined)
                    bits.push(`s${prog.step}`);
                const loss = (r.metrics ?? {}).loss;
                if (loss !== undefined)
                    bits.push(`loss ${Number(loss).toFixed(3)}`);
                rows.push(`  ${R.span(R.COLORS.cool, R.pad(r.kind ?? '?', 10))}${
                    R.span(R.COLORS.dim, R.pad(r.model ?? '', 12), {dim: true})}${
                    R.span(R.COLORS.warm, bits.join(' '))}`);
            }
        } else {
            rows.push('');
            rows.push(R.span(R.COLORS.off, '  no AI runs in flight', {dim: true}));
        }

        // The daemon's own last log line is the best one-line answer to
        // "what is it doing right now".
        const last = d.last ?? b.last;
        if (last) {
            const trimmed = last.length > 58 ? `${last.slice(-58)}` : last;
            rows.push('');
            rows.push(R.span(R.COLORS.off, `  ${trimmed}`, {dim: true}));
        }

        this._panels.agents.setMarkup(rows.join('\n'));
    }

    _renderRemote(s) {
        const r = s.remote ?? {};
        const rows = [];

        const crd = r.crd ?? {};
        const crdLive = crd.state === 'connected';
        rows.push(`${R.span(crdLive ? R.COLORS.ok : R.COLORS.off, crdLive ? '●' : '○')} ${
            R.span(R.COLORS.text, R.pad('Chrome RD', 12))}${
            R.span(crdLive ? R.COLORS.ok : R.COLORS.dim, R.pad(crd.state ?? 'off', 11))}${
            crd.user ? R.span(R.COLORS.dim, crd.user, {dim: true}) : ''}`);

        const rdp = r.rdp ?? {};
        const rdpLive = rdp.state === 'connected';
        rows.push(`${R.span(rdpLive ? R.COLORS.ok : R.COLORS.off, rdpLive ? '●' : '○')} ${
            R.span(R.COLORS.text, R.pad('RDP', 12))}${
            R.span(rdpLive ? R.COLORS.ok : R.COLORS.dim, R.pad(rdp.state ?? 'off', 11))}${
            (rdp.peers ?? []).length
                ? R.span(R.COLORS.dim, rdp.peers.join(' '), {dim: true})
                : ''}`);

        const con = r.console ?? {};
        rows.push(`${R.span(con.occupied ? R.COLORS.ok : R.COLORS.off, con.occupied ? '●' : '○')} ${
            R.span(R.COLORS.text, R.pad('console', 12))}${
            R.span(con.occupied ? R.COLORS.ok : R.COLORS.dim,
                R.pad(con.occupied ? 'logged in' : 'free', 11))}${
            con.user ? R.span(R.COLORS.dim, con.user, {dim: true}) : ''}`);

        const sessions = r.sessions ?? [];
        if (sessions.length) {
            rows.push('');
            rows.push(R.span(R.COLORS.title, '  sessions', {bold: true}));
            for (const sess of sessions.slice(0, 4)) {
                rows.push(`  ${R.span(R.COLORS.dim, R.pad(`#${sess.id}`, 5), {dim: true})}${
                    R.span(R.COLORS.text, R.pad(sess.user ?? '?', 10))}${
                    R.span(R.COLORS.dim, R.pad(sess.type ?? '', 9), {dim: true})}${
                    R.span(sess.remote ? R.COLORS.warm : R.COLORS.cool,
                        sess.remote ? 'remote' : `seat ${sess.seat ?? '-'}`)}`);
            }
        }

        this._panels.remote.setMarkup(rows.join('\n'));
        this._footerLeft.clutter_text.set_markup(
            R.span(R.COLORS.off, `  updated ${GLib.DateTime.new_now_local().format('%H:%M:%S')}`, {dim: true}));
    }
}
