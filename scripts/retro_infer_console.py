#!/usr/bin/env python3
"""retro-infer fleet ML console (roadmap M8) — ASCII TUI over the agent AI
transport. Renders on a 16-color 80x25 terminal; every panel is plain ASCII
so output can be mirrored into Retro Chat.

Keys:
  d  discover AI-capable agents (beacon + AI_HELLO + MODEL_LIST)
  t  fleet data-parallel train (spawns retro_ai_fleet.py, streams progress)
  i  run one inference on a resident model (prompts for host/model/index)
  b  bench: 100-image INFER_RUN loop -> img/s, logged to ai_runs
  q  quit
"""
import asyncio
import curses
import json
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
import numpy as np  # noqa: E402

from client.retro_protocol import RetroConnection  # noqa: E402
from client.retro_ai import RetroAI  # noqa: E402
from client.retro_discovery import discover_retro_pcs  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
OUT = os.path.join(os.path.dirname(__file__), "..", "tools", "rim", "out")


class Console:
    def __init__(self, scr):
        self.scr = scr
        self.log_lines = []
        self.machines = []
        self.busy = None

    def log(self, line):
        for l in str(line).splitlines():
            self.log_lines.append(l[:78])
        self.log_lines = self.log_lines[-200:]
        self.draw()

    def draw(self):
        scr = self.scr
        scr.erase()
        h, w = scr.getmaxyx()
        title = " RETRO-INFER  fleet ML console  ==  transport: retro-agent "
        scr.addnstr(0, 0, "+=" + title + "=" * max(0, w - len(title) - 4) + "+",
                    w - 1)
        row = 1
        if self.machines:
            for m in self.machines[:4]:
                scr.addnstr(row, 0, "| " + m[:w - 4], w - 1)
                row += 1
        else:
            scr.addnstr(row, 0, "| [d] to discover ai-capable agents", w - 1)
            row += 1
        scr.addnstr(row, 0, "+" + "-" * (w - 2) + "+", w - 1)
        row += 1
        avail = h - row - 2
        for l in self.log_lines[-avail:]:
            if row >= h - 2:
                break
            scr.addnstr(row, 0, "| " + l, w - 1)
            row += 1
        status = self.busy or "[t]rain [i]nfer [d]iscover [b]ench [q]uit"
        scr.addnstr(h - 1, 0, "+== " + status + " " +
                    "=" * max(0, w - len(status) - 7) + "+", w - 1)
        scr.refresh()

    def prompt(self, label, default=""):
        h, w = self.scr.getmaxyx()
        curses.echo()
        curses.curs_set(1)
        self.scr.addnstr(h - 2, 0, f"| {label} [{default}]: " + " " * 20, w - 1)
        self.scr.refresh()
        try:
            val = self.scr.getstr(h - 2, len(label) + len(default) + 7,
                                  40).decode("ascii", "replace").strip()
        except Exception:
            val = ""
        curses.noecho()
        curses.curs_set(0)
        return val or default


async def discover(con):
    con.busy = "discovering..."
    con.draw()
    pcs = await discover_retro_pcs(timeout=3.0)
    rows = []
    for pc in pcs or []:
        d = pc.to_dict() if hasattr(pc, "to_dict") else {}
        ip = d.get("ip")
        if not d.get("ai"):
            rows.append(f"{ip:<16} {d.get('hostname', ''):<12} no AI engine")
            continue
        try:
            conn = RetroConnection(ip, 9898)
            await conn.connect(SECRET, timeout=8.0)
            ai = RetroAI(conn)
            hello = await ai.hello()
            models = [m["name"] for m in
                      (await ai.model_list()).get("models", [])]
            await conn.close()
            rows.append(
                f"{ip:<16} {hello.get('backends', ['?'])[0]:<12} "
                f"{hello.get('kernel_f32')}/{hello.get('kernel_i8'):<8} "
                f"READY  models:{len(models)} {','.join(models)[:20]}")
        except Exception as e:
            rows.append(f"{ip:<16} probe failed: {e}")
    con.machines = rows or ["(none found)"]
    con.busy = None
    con.log(f"discovered {len(rows)} machine(s)")


async def infer_one(con, host, model, idx):
    con.busy = f"infer {model}@{host}..."
    con.draw()
    imgs = open(f"{OUT}/mnist_test_1000.images.bin", "rb").read()
    labels = open(f"{OUT}/mnist_test_1000.labels.bin", "rb").read()
    conn = RetroConnection(host, 9898)
    await conn.connect(SECRET, timeout=8.0)
    ai = RetroAI(conn)
    t0 = time.time()
    logits = await ai.infer_run(model, imgs[idx * 784:(idx + 1) * 784])
    ms = (time.time() - t0) * 1000
    await conn.close()
    con.busy = None
    con.log(f"infer[{host}] {model} sample#{idx}: pred={logits.argmax()} "
            f"true={labels[idx]} rtt={ms:.0f}ms")
    con.log("logits: " + " ".join(f"{v:.2f}" for v in logits))


async def bench(con, host, model, n=100):
    con.busy = f"bench {model}@{host} ({n} imgs)..."
    con.draw()
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
    secs = time.time() - t0
    await conn.close()
    ips = n / secs
    con.log(f"bench[{host}] {model}: top1={correct/n:.3f} {ips:.1f} img/s "
            f"(incl. round-trips) in {secs:.1f}s")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ai_metrics
        rid = ai_metrics.log_run(
            host, model, "bench", hello.get("backends", ["?"])[0], "i8",
            {"top1": correct / n, "img_per_sec_rtt": round(ips, 2), "n": n},
            engine_ver=hello.get("version"), dataset="mnist-test",
            notes="console bench (includes network round-trip)")
        con.log(f"logged to ai_runs id={rid}")
    except Exception as e:
        con.log(f"(DB log failed: {e})")
    con.busy = None
    con.draw()


def train_panel(con, ips):
    con.log(f"spawning dp-train on {ips} ...")
    proc = subprocess.Popen(
        [sys.executable, os.path.join(os.path.dirname(__file__),
                                      "retro_ai_fleet.py"),
         "dp-train", "--ips", ips, "--epochs", "1",
         "--global-batch", "128", "--train-n", "12800"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        con.log(line.rstrip())
    proc.wait()
    con.log(f"dp-train exited rc={proc.returncode}")


def main(scr):
    curses.curs_set(0)
    scr.nodelay(False)
    con = Console(scr)
    con.log("retro-infer console ready.")
    con.draw()
    while True:
        c = scr.getch()
        if c in (ord("q"), ord("Q")):
            break
        elif c in (ord("d"), ord("D")):
            asyncio.run(discover(con))
        elif c in (ord("i"), ord("I")):
            host = con.prompt("host", "192.168.1.143")
            model = con.prompt("model", "lenet5-int8")
            idx = int(con.prompt("sample #", "0") or 0)
            try:
                asyncio.run(infer_one(con, host, model, idx))
            except Exception as e:
                con.busy = None
                con.log(f"infer failed: {e}")
        elif c in (ord("b"), ord("B")):
            host = con.prompt("host", "192.168.1.143")
            model = con.prompt("model", "lenet5-int8")
            try:
                asyncio.run(bench(con, host, model))
            except Exception as e:
                con.busy = None
                con.log(f"bench failed: {e}")
        elif c in (ord("t"), ord("T")):
            ips = con.prompt("ips", "192.168.1.124,192.168.1.143")
            try:
                train_panel(con, ips)
            except Exception as e:
                con.log(f"train failed: {e}")


if __name__ == "__main__":
    curses.wrapper(main)
