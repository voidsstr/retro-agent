#!/usr/bin/env python3
"""retro-infer fleet ML console (roadmap M8) — a live, menu-driven terminal
dashboard over the agent AI transport, in the spirit of btop: a full-screen
view that keeps repainting while actions run in the background, rather than
a classic argument-driven CLI you invoke once per action.

Why it looks the way it does: the agent/engine protocol has no streaming
telemetry primitive (see scripts/ai_status_bus.py's docstring for the full
rationale) — every "live" figure here is synthesized from status blobs that
training/inference scripts publish to /tmp/retro-ai/status/ as they run, or
from a direct real-time probe (nvidia-smi, NVIDIA box only). Nothing on
screen is fabricated: a box with no live GPU-backend workload and no vendor
utilization tool shows CPU ops/sec instead of a guessed GPU percentage.

Launch: python3 scripts/retro_infer_console.py   (needs `pip install -r
requirements.txt`; unix terminal, uses cbreak-mode raw stdin).

Keys: [d]iscover [t]rain [n] dist-infer [i]nfer [b]ench [p]ipeline
      [l]eaderboard [g]c bus [k]ill a run [Tab] focus next machine
      [?] help  [q]uit
Full walkthrough: retro-infer/docs/OPERATIONS.md
"""
import asyncio
import os
import sys
import termios
import time
import tty

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rich.align import Align  # noqa: E402
from rich.console import Console as RichConsole  # noqa: E402
from rich.layout import Layout  # noqa: E402
from rich.live import Live  # noqa: E402
from rich.panel import Panel  # noqa: E402
from rich.progress_bar import ProgressBar  # noqa: E402
from rich.table import Table  # noqa: E402
from rich.text import Text  # noqa: E402
from rich.theme import Theme  # noqa: E402

from client.retro_protocol import RetroConnection  # noqa: E402
from client.retro_ai import RetroAI  # noqa: E402
from client.retro_discovery import discover_retro_pcs  # noqa: E402
import ai_status_bus as bus  # noqa: E402
import ai_metrics  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
OUT = os.path.join(os.path.dirname(__file__), "..", "tools", "rim", "out")
FLEET_SCRIPT = os.path.join(os.path.dirname(__file__), "retro_ai_fleet.py")
PIPELINE_SCRIPT = os.path.join(os.path.dirname(__file__), "retro_ai_pipeline.py")

# --- color scheme: this project's "dark hacker" identity (agent/src/
# retrowall.c's green-on-black fleet theme), not btop's default look. ---
RETRO_THEME = Theme({
    "border":       "bold green",
    "title":        "bold bright_green",
    "header":       "bold bright_green on grey3",
    "label":        "cyan",
    "value":        "bright_white",
    "ok":           "bright_green",
    "training":     "bold bright_cyan",
    "inferring":    "bold blue",
    "warn":         "bold yellow3",
    "est":          "yellow3",
    "gpu":          "bold magenta",
    "dead":         "bold red",
})
console = RichConsole(theme=RETRO_THEME, style="on grey3")

SPARK = "▁▂▃▄▅▆▇█"


def sparkline(values, width=24):
    if not values:
        return " " * width
    vals = [v for v in values[-width:] if v is not None]
    if not vals:
        return " " * width
    lo, hi = min(vals), max(vals)
    rng = (hi - lo) or 1.0
    return "".join(SPARK[min(7, max(0, int((v - lo) / rng * 7)))] for v in vals)


PHASE_STYLE = {
    "train": "training", "infer": "inferring", "infer_dist": "inferring",
    "bench": "inferring", "pipeline": "inferring",
}
LIVENESS_STYLE = {
    "live": "ok", "stalled": "warn", "dead": "dead",
    "completed": "value", "failed": "dead",
}


class Form:
    """A tiny modal line-editor: a title + ordered (label, default) fields,
    Enter/Tab advances, Esc cancels, Backspace edits. No arrow-key/history
    support — this is a parameter-entry overlay, not a text editor."""

    def __init__(self, title, fields, on_submit):
        self.title = title
        self.fields = [{"label": lbl, "value": "", "default": dflt}
                       for lbl, dflt in fields]
        self.idx = 0
        self.on_submit = on_submit

    def feed(self, ch):
        f = self.fields[self.idx]
        if ch in ("\r", "\n", "\t"):
            self.idx += 1
            if self.idx >= len(self.fields):
                return {fl["label"]: (fl["value"] or fl["default"])
                       for fl in self.fields}
            return None
        if ch == "\x1b":
            return "cancel"
        if ch in ("\x7f", "\x08"):
            f["value"] = f["value"][:-1]
            return None
        if ch.isprintable():
            f["value"] += ch
        return None

    def render(self):
        lines = [Text(self.title, style="title"), Text("")]
        for i, f in enumerate(self.fields):
            marker = "> " if i == self.idx else "  "
            shown = f["value"] or f"[{f['default']}]"
            style = "bright_white" if i == self.idx else "cyan"
            lines.append(Text(f"{marker}{f['label']}: {shown}", style=style))
        lines.append(Text(""))
        lines.append(Text("Enter/Tab: next field & submit on last   Esc: cancel",
                          style="cyan"))
        return Panel(Align.left(Text("\n").join(lines)), title="input",
                    border_style="warn")


class App:
    def __init__(self):
        self.machines = []          # discover() results
        self.focus = 0              # index into self.machines
        self.log_lines = []
        self.form = None
        self.leaderboard_rows = None
        self.running = True
        self.tasks = set()
        self.procs = {}             # run_id -> asyncio.subprocess.Process
        self._gpu_conns = {}        # ip -> RetroConnection (probe reuse)
        self._sps_history = {}      # ip -> [samples_per_sec,...]
        self._db_ok = None

    def log(self, line):
        for l in str(line).splitlines():
            self.log_lines.append(l[:200])
        self.log_lines = self.log_lines[-300:]

    # ---------- actions (each is a background asyncio task) ----------

    async def do_discover(self):
        self.log("discovering AI-capable agents...")
        pcs = await discover_retro_pcs(timeout=3.0)
        rows = []
        for pc in pcs or []:
            d = pc.to_dict() if hasattr(pc, "to_dict") else {}
            ip = d.get("ip")
            if not d.get("ai"):
                rows.append({"ip": ip, "hostname": d.get("hostname", ""),
                            "backend": "-", "kernels": "-", "status": "no AI",
                            "models": 0, "is_nvidia": False})
                continue
            try:
                conn = RetroConnection(ip, 9898)
                await conn.connect(SECRET, timeout=8.0)
                ai = RetroAI(conn)
                hello = await ai.hello()
                models = [m["name"] for m in
                          (await ai.model_list()).get("models", [])]
                await conn.close()
                host_gpu = hello.get("host_gpu", {})
                rows.append({
                    "ip": ip, "hostname": d.get("hostname", ""),
                    "backend": (hello.get("backends") or ["?"])[0],
                    "kernels": f"{hello.get('kernel_f32')}/{hello.get('kernel_i8')}",
                    "status": "READY" if hello.get("ready") else "not ready",
                    "models": len(models), "is_nvidia": bool(host_gpu.get("is_nvidia")),
                })
            except Exception as e:
                rows.append({"ip": ip, "hostname": d.get("hostname", ""),
                            "backend": "-", "kernels": "-",
                            "status": f"unreachable ({e})", "models": 0,
                            "is_nvidia": False})
        self.machines = rows
        self.focus = 0
        self.log(f"discovered {len(rows)} machine(s)")

    async def do_infer(self, host, model, idx):
        run_id = bus.new_run("single-infer", model=model, phase="infer",
                             precision="i8", command=f"infer {host} {model} #{idx}",
                             nodes=[host])
        bus.publish(run_id, status="running", progress={"total_steps": 1})
        try:
            imgs = open(f"{OUT}/mnist_test_1000.images.bin", "rb").read()
            labels = open(f"{OUT}/mnist_test_1000.labels.bin", "rb").read()
            conn = RetroConnection(host, 9898)
            await conn.connect(SECRET, timeout=8.0)
            ai = RetroAI(conn)
            t0 = time.time()
            logits = await ai.infer_run(model, imgs[idx * 784:(idx + 1) * 784])
            ms = (time.time() - t0) * 1000
            await conn.close()
            pred, true = int(logits.argmax()), labels[idx]
            self.log(f"infer[{host}] {model} sample#{idx}: pred={pred} "
                    f"true={true} rtt={ms:.0f}ms")
            bus.mark_done(run_id, status="completed")
            bus.publish(run_id, metrics={"pred": pred, "true": int(true),
                                         "rtt_ms": round(ms, 1)},
                        progress={"step": 1, "percent": 100.0},
                        nodes={host: {"alive": True, "last_step_ms": round(ms, 1),
                                     "samples_per_sec": round(1000.0 / ms, 1) if ms else None}})
        except Exception as e:
            self.log(f"infer failed: {e}")
            bus.mark_done(run_id, status="failed", error=str(e)[:300])

    async def do_bench(self, host, model, n=100):
        run_id = bus.new_run("bench", model=model, phase="bench", precision="i8",
                             command=f"bench {host} {model} n={n}", nodes=[host])
        bus.publish(run_id, status="running", progress={"total_steps": n})
        try:
            imgs = open(f"{OUT}/mnist_test_1000.images.bin", "rb").read()
            labels = open(f"{OUT}/mnist_test_1000.labels.bin", "rb").read()
            conn = RetroConnection(host, 9898)
            await conn.connect(SECRET, timeout=8.0)
            ai = RetroAI(conn)
            hello = await ai.hello()
            t0 = time.time()
            correct = 0
            for i in range(n):
                logits = await ai.infer_run(model, imgs[i * 784:(i + 1) * 784])
                if logits.argmax() == labels[i]:
                    correct += 1
                if (i + 1) % 10 == 0 or i + 1 == n:
                    elapsed = time.time() - t0
                    sps = (i + 1) / elapsed if elapsed > 0 else None
                    bus.publish(run_id, progress={
                        "step": i + 1, "percent": round(100.0 * (i + 1) / n, 1)},
                        fleet={"samples_per_sec": round(sps, 1) if sps else None},
                        nodes={host: {"alive": True,
                                     "samples_per_sec": round(sps, 1) if sps else None}})
            secs = time.time() - t0
            await conn.close()
            ips_ = n / secs if secs else 0.0
            self.log(f"bench[{host}] {model}: top1={correct/n:.3f} {ips_:.1f} img/s "
                    f"(incl RTT) in {secs:.1f}s")
            bus.mark_done(run_id, status="completed")
            bus.publish(run_id, metrics={"top1": round(correct / n, 3),
                                         "img_per_sec_rtt": round(ips_, 2), "n": n})
            try:
                rid = ai_metrics.log_run(
                    host, model, "bench", (hello.get("backends") or ["?"])[0], "i8",
                    {"top1": correct / n, "img_per_sec_rtt": round(ips_, 2), "n": n},
                    engine_ver=hello.get("version"), dataset="mnist-test",
                    notes="console bench (includes network round-trip)")
                self.log(f"logged to ai_runs id={rid}")
            except Exception as e:
                self.log(f"(DB log failed: {e})")
        except Exception as e:
            self.log(f"bench failed: {e}")
            bus.mark_done(run_id, status="failed", error=str(e)[:300])

    async def _spawn(self, argv, label):
        self.log(f"spawning: {' '.join(argv)}")
        proc = await asyncio.create_subprocess_exec(
            *argv, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT)
        run_key = f"{label}-{proc.pid}"
        self.procs[run_key] = proc
        try:
            while True:
                line = await proc.stdout.readline()
                if not line:
                    break
                self.log(line.decode("utf-8", "replace").rstrip())
        finally:
            rc = await proc.wait()
            self.log(f"{label} (pid {proc.pid}) exited rc={rc}")
            self.procs.pop(run_key, None)

    async def do_train(self, ips, epochs, global_batch, train_n):
        await self._spawn(
            [sys.executable, FLEET_SCRIPT, "dp-train", "--ips", ips,
             "--epochs", str(epochs), "--global-batch", str(global_batch),
             "--train-n", str(train_n)], "dp-train")

    async def do_dpinfer(self, ips, model, n):
        await self._spawn(
            [sys.executable, FLEET_SCRIPT, "dp-infer", "--ips", ips,
             "--model", model, "--n", str(n)], "dp-infer")

    async def do_pipeline(self, a, b, n):
        await self._spawn(
            [sys.executable, PIPELINE_SCRIPT, "--a", a, "--b", b,
             "--n", str(n)], "pipeline")

    def do_leaderboard(self, model, metric):
        try:
            rows = ai_metrics.leaderboard(model or None, metric or "img_per_sec")
            self.leaderboard_rows = (metric, rows)
            self._db_ok = True
            self.log(f"leaderboard: {len(rows)} row(s) for metric={metric}")
        except Exception as e:
            self._db_ok = False
            self.log(f"leaderboard failed (DB unreachable?): {e}")

    def do_gc(self):
        n = bus.gc()
        self.log(f"status-bus gc: removed {n} terminal run(s)")

    def do_kill(self, needle):
        for key, proc in list(self.procs.items()):
            if needle in key:
                proc.terminate()
                self.log(f"sent SIGTERM to {key} (pid {proc.pid})")
                return
        self.log(f"no tracked subprocess matching '{needle}' "
                f"(tracked: {list(self.procs) or 'none'})")

    # ---------- form submission dispatch ----------

    def open_form(self, kind):
        specs = {
            "i": ("infer", [("host", "192.168.1.143"), ("model", "lenet5-int8"),
                            ("sample #", "0")]),
            "b": ("bench", [("host", "192.168.1.143"), ("model", "lenet5-int8"),
                            ("n", "100")]),
            "t": ("train", [("ips", "192.168.1.124,192.168.1.143"),
                            ("epochs", "1"), ("global-batch", "128"),
                            ("train-n", "12800")]),
            "n": ("dist-infer", [("ips", "192.168.1.124,192.168.1.143"),
                                 ("model", "lenet5-int8"), ("n", "200")]),
            "p": ("pipeline", [("stage1 ip", "192.168.1.124"),
                               ("stage2 ip", "192.168.1.143"), ("n", "200")]),
            "l": ("leaderboard", [("model (blank=all)", ""),
                                  ("metric", "img_per_sec")]),
            "k": ("kill", [("run-id substring", "")]),
        }
        title, fields = specs[kind]
        self.form = Form(title, fields, lambda vals: self._submit(kind, vals))

    def _submit(self, kind, vals):
        v = list(vals.values())
        try:
            if kind == "i":
                self._spawn_task(self.do_infer(v[0], v[1], int(v[2] or 0)))
            elif kind == "b":
                self._spawn_task(self.do_bench(v[0], v[1], int(v[2] or 100)))
            elif kind == "t":
                self._spawn_task(self.do_train(v[0], int(v[1] or 1),
                                               int(v[2] or 128), int(v[3] or 12800)))
            elif kind == "n":
                self._spawn_task(self.do_dpinfer(v[0], v[1], int(v[2] or 200)))
            elif kind == "p":
                self._spawn_task(self.do_pipeline(v[0], v[1], int(v[2] or 200)))
            elif kind == "l":
                self.do_leaderboard(v[0], v[1])
            elif kind == "k":
                self.do_kill(v[0])
        except Exception as e:
            self.log(f"action failed to start: {e}")

    def _spawn_task(self, coro):
        t = asyncio.create_task(coro)
        self.tasks.add(t)
        t.add_done_callback(self.tasks.discard)

    # ---------- rendering ----------

    def render_header(self):
        active = len(bus.list_active())
        clock = time.strftime("%H:%M:%S")
        db = {"True": "ok", "False": "unreachable", "None": "unknown"}[
            str(self._db_ok)]
        txt = Text(justify="center")
        txt.append(" RETRO-INFER FLEET ", style="header")
        txt.append(f"  {clock}  ", style="value")
        txt.append(f"db:{db}  ", style="ok" if self._db_ok else "warn")
        txt.append(f"bus:{active} active", style="value")
        return Panel(txt, border_style="border", height=3)

    def render_fleet(self, max_rows=None):
        t = Table(border_style="border", expand=True)
        t.add_column("", width=1)
        t.add_column("IP", style="label")
        t.add_column("Host", style="value")
        t.add_column("Backend", style="value")
        t.add_column("Kernels", style="cyan")
        t.add_column("Status")
        t.add_column("Mdl", justify="right")
        machines = self.machines[:max_rows] if max_rows else self.machines
        for i, m in enumerate(machines):
            marker = "▶" if i == self.focus else " "
            status_style = "ok" if m["status"] == "READY" else (
                "dead" if "unreachable" in m["status"] else "warn")
            gpu_mark = " [gpu]" if m.get("is_nvidia") else ""
            t.add_row(marker, m["ip"], m["hostname"][:10], m["backend"],
                      m["kernels"], Text(m["status"][:16] + gpu_mark,
                                         style=status_style),
                      str(m["models"]))
        if not self.machines:
            t.add_row("", "-", "press [d] to discover", "", "", "", "")
        elif max_rows and len(self.machines) > max_rows:
            t.add_row("", f"+{len(self.machines) - max_rows} more", "", "",
                      "", "", "")
        return Panel(t, title="fleet", border_style="border")

    def render_runs(self, max_rows=None):
        t = Table(border_style="border", expand=True)
        t.add_column("run", style="label")
        t.add_column("phase")
        t.add_column("progress")
        t.add_column("ops/s", justify="right", style="value")
        active = bus.list_active()
        for run in active[:max_rows if max_rows else 12]:
            phase_style = PHASE_STYLE.get(run["phase"], "value")
            live_style = LIVENESS_STYLE.get(run["liveness"], "value")
            prog = run.get("progress") or {}
            pct = prog.get("percent")
            bar = ProgressBar(total=100, completed=pct or 0, width=14) \
                if pct is not None else Text("n/a")
            sps = (run.get("fleet") or {}).get("samples_per_sec")
            t.add_row(
                Text(run["run_id"][-14:], style=live_style),
                Text(run["phase"], style=phase_style),
                bar, f"{sps:.1f}" if sps else "-")
        if not active:
            t.add_row("-", "no active runs", "", "")
        elif max_rows and len(active) > max_rows:
            t.add_row(f"+{len(active) - max_rows} more", "", "", "")
        return Panel(t, title="active runs", border_style="border")

    async def render_gauge(self):
        if not self.machines:
            body = Text("discover the fleet with [d] to see GPU/throughput data",
                        style="cyan")
            return Panel(body, title="gpu / throughput", border_style="border",
                        height=5)
        m = self.machines[self.focus]
        ip = m["ip"]
        lines = []
        if m.get("is_nvidia"):
            conn = self._gpu_conns.get(ip)
            if conn is None or not conn.connected():
                try:
                    conn = RetroConnection(ip, 9898)
                    await conn.connect(SECRET, timeout=5.0)
                    self._gpu_conns[ip] = conn
                except Exception as e:
                    lines.append(Text(f"{ip}: probe connect failed ({e})",
                                      style="dead"))
                    conn = None
            if conn is not None:
                g = await bus.probe_gpu_util(conn, ip)
                if g["available"]:
                    bar = ProgressBar(total=100, completed=g["util_pct"], width=30)
                    lines.append(Text.assemble(
                        (f"{ip} ({m['hostname']}) ", "label"),
                        (f"util {g['util_pct']:.0f}%  ", "gpu"),
                        (f"mem {g['mem_used_mb']:.0f}/{g['mem_total_mb']:.0f}MB  ", "value"),
                        (f"{g['temp_c']:.0f}C {g['power_w']:.0f}W ", "value"),
                        ("(nvidia-smi)", "est")))
                    lines.append(bar)
                else:
                    lines.append(Text(f"{ip}: nvidia-smi unavailable ({g['error']})",
                                      style="warn"))
        else:
            lines.append(Text(f"{ip} ({m['hostname']}): no vendor GPU-utilization "
                              f"tool — showing measured CPU ops/sec instead "
                              f"(honest > guessed).", style="cyan"))
        hist = self._sps_history.get(ip, [])
        lines.append(Text.assemble(("ops/sec (last 60 samples): ", "label"),
                                   (sparkline(hist), "gpu")))
        return Panel(Align.left(Text("\n").join(lines) if lines else Text("")),
                    title="gpu / throughput", border_style="border", height=5)

    def render_log(self):
        if self.form is not None:
            return self.form.render()
        avail = self.log_lines[-4:]
        body = Text("\n").join(Text(l) for l in avail) if avail else Text("(empty)")
        return Panel(body, title="log", border_style="border", height=6)

    def render_footer(self):
        if self.leaderboard_rows is not None:
            metric, rows = self.leaderboard_rows
            hint = f"leaderboard[{metric}]: {len(rows)} rows in log — [Esc] dismiss"
        else:
            hint = ("[d]iscover [t]rain [n]dist-infer [i]nfer [b]ench [p]ipeline "
                   "[l]eaderboard [g]c [k]ill [Tab]focus [?]help [q]uit")
        return Panel(Text(hint, style="cyan"), border_style="border", height=3)

    async def build_layout(self):
        # sample throughput history for the sparkline before rendering
        for m in self.machines:
            sps = None
            for run in bus.list_active():
                node = (run.get("nodes") or {}).get(m["ip"])
                if node and node.get("samples_per_sec") is not None:
                    sps = node["samples_per_sec"]
            hist = self._sps_history.setdefault(m["ip"], [])
            hist.append(sps)
            self._sps_history[m["ip"]] = hist[-60:]

        # Fixed chrome (header/gauge/log/footer) is deliberately kept small
        # so `body` (the fleet + active-runs tables) still gets a usable
        # share of height on a minimal 80x25 terminal — the M8 acceptance
        # bar this console has always targeted. Layout hard-crops overflow
        # rather than scrolling, so table row counts are capped to what the
        # measured console height can actually show (see render_fleet/
        # render_runs max_rows) rather than silently rendering a clipped,
        # half-drawn table.
        HEADER_H, GAUGE_H, LOG_H, FOOTER_H = 3, 5, 6, 3
        fixed = HEADER_H + GAUGE_H + LOG_H + FOOTER_H
        body_h = max(5, console.size.height - fixed)
        # each table panel has a 2-row border + 2-row table header/separator
        max_rows = max(1, body_h - 4)

        root = Layout()
        root.split_column(
            Layout(name="header", size=HEADER_H),
            Layout(name="body", size=body_h),
            Layout(name="gauge", size=GAUGE_H),
            Layout(name="log", size=LOG_H),
            Layout(name="footer", size=FOOTER_H),
        )
        root["body"].split_row(
            Layout(name="fleet", ratio=55), Layout(name="runs", ratio=45))
        root["header"].update(self.render_header())
        root["fleet"].update(self.render_fleet(max_rows=max_rows))
        root["runs"].update(self.render_runs(max_rows=max_rows))
        root["gauge"].update(await self.render_gauge())
        root["log"].update(self.render_log())
        root["footer"].update(self.render_footer())
        return root

    # ---------- key dispatch ----------

    def handle_key(self, ch):
        if self.form is not None:
            result = self.form.feed(ch)
            if result == "cancel":
                self.log(f"{self.form.title}: cancelled")
                self.form = None
            elif isinstance(result, dict):
                submit = self.form.on_submit
                self.form = None
                submit(result)
            return
        if self.leaderboard_rows is not None and ch == "\x1b":
            self.leaderboard_rows = None
            return
        if ch in ("q", "Q"):
            self.running = False
        elif ch in ("d", "D"):
            self._spawn_task(self.do_discover())
        elif ch in ("i", "b", "t", "n", "p", "l", "k"):
            self.open_form(ch)
        elif ch in ("g", "G"):
            self.do_gc()
        elif ch == "\t":
            if self.machines:
                self.focus = (self.focus + 1) % len(self.machines)
        elif ch in ("?",):
            self.log("keys: d=discover t=train n=dist-infer i=infer b=bench "
                    "p=pipeline l=leaderboard g=gc-status-bus k=kill(by run-id "
                    "substring) Tab=focus-next-machine q=quit")


async def input_loop(app, loop):
    queue = asyncio.Queue()
    fd = sys.stdin.fileno()

    def on_ready():
        try:
            data = os.read(fd, 1)
        except OSError:
            return
        if data:
            queue.put_nowait(data.decode("utf-8", "ignore"))

    loop.add_reader(fd, on_ready)
    try:
        while app.running:
            ch = await queue.get()
            app.handle_key(ch)
    finally:
        loop.remove_reader(fd)


async def render_loop(app, live):
    while app.running:
        try:
            live.update(await app.build_layout())
            live.refresh()
        except Exception as e:
            app.log(f"(render error: {e})")
        await asyncio.sleep(0.125)


async def async_main():
    app = App()
    try:
        ai_metrics.connect().close()
        app._db_ok = True
    except Exception as e:
        app._db_ok = False
        app.log(f"(ai_runs DB unreachable at startup: {e})")
    app.log("retro-infer console ready. Press ? for help, d to discover the fleet.")
    with Live(console=console, screen=True, auto_refresh=False,
             transient=False) as live:
        loop = asyncio.get_event_loop()
        await asyncio.gather(input_loop(app, loop), render_loop(app, live))
    for conn in app._gpu_conns.values():
        try:
            await conn.close()
        except Exception:
            pass


def main():
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setcbreak(fd)
    try:
        asyncio.run(async_main())
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


if __name__ == "__main__":
    main()
