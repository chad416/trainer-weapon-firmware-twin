#!/usr/bin/env python3
"""Lane console: live view of every trainer weapon on the lane CAN bus.

- decodes all frames with protocol/trainer.dbc
- sends SYNC (lane time) every 10 ms, so shot timestamps are in lane time
- shows per-weapon status, the shot log with trigger/IMU metrics, and events
- logs every decoded frame to a CSV for session_report.py
- optionally records the screen as SVG frames (for screenshots and video)

Usage:
  lane_console.py [--channel vcan0] [--log session.csv] [--duration 60]
                  [--record-dir frames/ --record-every 0.5] [--no-sync]
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import pathlib
import signal
import threading
import time
from dataclasses import dataclass, field

import can
from rich.console import Console, Group
from rich.live import Live
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from protocol import (EVENTS, FN_BIT_RESULT, FN_EVENT, FN_INFO, FN_SHOT_DETAIL, FN_SHOT_EVENT,
                      FN_STATUS, FN_SYNC, SELECTORS, STATES, Protocol)

SYNC_PERIOD_S = 0.010


@dataclass
class Node:
    node: int
    status: dict = field(default_factory=dict)
    info: dict = field(default_factory=dict)
    bit: dict = field(default_factory=dict)
    shots: int = 0
    last_seen: float = 0.0


@dataclass
class Shot:
    node: int
    shot_no: int
    t_us: int
    mode: int
    rounds_left: int
    detail: dict = field(default_factory=dict)


class Lane:
    """Thread-safe lane state built from decoded frames."""

    def __init__(self, proto: Protocol, log_path: pathlib.Path | None):
        self.proto = proto
        self.lock = threading.Lock()
        self.nodes: dict[int, Node] = {}
        self.shots: collections.deque[Shot] = collections.deque(maxlen=14)
        self.pending: dict[tuple[int, int], Shot] = {}
        self.events: collections.deque[str] = collections.deque(maxlen=8)
        self.frames = 0
        self.t0 = time.monotonic()
        self.log_file = log_path.open("w", newline="") if log_path else None
        self.log = csv.writer(self.log_file) if self.log_file else None
        if self.log:
            self.log.writerow(["host_s", "id", "node", "message", "signals"])

    def on_frame(self, msg: can.Message) -> None:
        d = self.proto.decode(msg.arbitration_id, bytes(msg.data))
        if d is None:
            return
        now = time.monotonic() - self.t0
        with self.lock:
            self.frames += 1
            if self.log and d.function != FN_SYNC:
                self.log.writerow([f"{now:.4f}", f"0x{msg.arbitration_id:03X}", d.node, d.name,
                                   json.dumps({k: (float(v) if not isinstance(v, int) else v)
                                               for k, v in d.signals.items()})])
            if d.function == FN_SYNC:
                return
            n = self.nodes.setdefault(d.node, Node(d.node))
            n.last_seen = now
            s = d.signals
            if d.function == FN_STATUS:
                n.status = s
            elif d.function == FN_INFO:
                n.info = s
                self.events.appendleft(f"{now:7.2f}s  W{d.node}  INFO v{s['ver_major']}.{s['ver_minor']}.{s['ver_patch']} boot={s['boot_count']}")
            elif d.function == FN_BIT_RESULT:
                n.bit = s
                self.events.appendleft(f"{now:7.2f}s  W{d.node}  BIT faults=0x{int(s['fault_mask']):04X}{' CRITICAL' if s['critical'] else ''}")
            elif d.function == FN_SHOT_EVENT:
                n.shots += 1
                shot = Shot(d.node, int(s["shot_no"]), int(s["t_us"]), int(s["mode"]), int(s["rounds_left"]))
                self.pending[(d.node, shot.shot_no)] = shot
                self.shots.appendleft(shot)
            elif d.function == FN_SHOT_DETAIL:
                shot = self.pending.pop((d.node, int(s["shot_no"])), None)
                if shot:
                    shot.detail = s
            elif d.function == FN_EVENT:
                kind = EVENTS.get(int(s["type"]), str(s["type"]))
                if kind in ("STATE",):
                    return
                value = int(s["value"])
                extra = {"SELECTOR": SELECTORS[value] if value < 3 else value,
                         "CLEARED": f"{value} ms", "CLEAR_LATE": f"{value} ms"}.get(kind, "")
                self.events.appendleft(f"{now:7.2f}s  W{d.node}  {kind} {extra}".rstrip())

    def close(self) -> None:
        if self.log_file:
            self.log_file.close()

    # ---- rendering -------------------------------------------------------------

    def render(self, synced: bool) -> Group:
        with self.lock:
            weapons = Table(title="Weapons", expand=True, title_justify="left")
            for col in ("node", "state", "selector", "mag", "rounds", "shots", "faults", "fw"):
                weapons.add_column(col)
            for n in sorted(self.nodes.values(), key=lambda x: x.node):
                st = n.status
                state = STATES[st["state"]] if "state" in st and st["state"] < len(STATES) else "?"
                style = {"FAULT": "bold red", "STOPPAGE": "bold yellow", "READY": "green",
                         "FIRING": "bold green", "SAFE": "blue"}.get(state, "")
                fw = f"{n.info.get('ver_major', '?')}.{n.info.get('ver_minor', '?')}.{n.info.get('ver_patch', '?')}"
                weapons.add_row(f"W{n.node}", Text(state, style=style),
                                SELECTORS[st["sel"]] if "sel" in st else "?",
                                "in" if st.get("mag_in") else "out", str(st.get("rounds", "")),
                                str(st.get("shots", "")), f"0x{int(st.get('faults', 0)):04X}", fw)

            shots = Table(title="Shots (newest first)", expand=True, title_justify="left")
            for col in ("weapon", "shot", "lane time s", "mode", "left", "take-up ms",
                        "squeeze ms", "jerk", "muzzle deg/s", "cant deg", "flinch deg"):
                shots.add_column(col, justify="right")
            for s in self.shots:
                d = s.detail
                flinch = float(d.get("flinch", 0.0))
                shots.add_row(f"W{s.node}", str(s.shot_no), f"{s.t_us / 1e6:10.4f}",
                              SELECTORS[s.mode] if s.mode < 3 else "?", str(s.rounds_left),
                              str(d.get("takeup_ms", "")), str(d.get("squeeze_ms", "")),
                              str(d.get("jerk", "")), str(d.get("muzzle_rate", "")),
                              str(d.get("cant", "")),
                              Text(f"{flinch:.1f}", style="bold red" if flinch >= 0.8 else ""))

            events = Panel("\n".join(self.events) or "-", title="Events", title_align="left")
            footer = Text(f"frames {self.frames}   SYNC {'on' if synced else 'off'}   "
                          f"t={time.monotonic() - self.t0:6.1f}s", style="dim")
            return Group(weapons, shots, events, footer)


def sync_sender(bus: can.BusABC, proto: Protocol, stop: threading.Event) -> None:
    next_t = time.monotonic()
    while not stop.is_set():
        frame_id, data = proto.encode_sync(int(time.monotonic() * 1e6))
        try:
            bus.send(can.Message(arbitration_id=frame_id, data=data, is_extended_id=False))
        except can.CanError:
            pass
        next_t += SYNC_PERIOD_S
        stop.wait(max(0.0, next_t - time.monotonic()))


def _raise_interrupt(signum, frame) -> None:
    raise KeyboardInterrupt


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--channel", default="vcan0")
    p.add_argument("--interface", default="socketcan")
    p.add_argument("--log", type=pathlib.Path, default=None, help="session CSV")
    p.add_argument("--duration", type=float, default=0.0, help="seconds, 0 = until Ctrl-C")
    p.add_argument("--no-sync", action="store_true", help="do not send SYNC")
    p.add_argument("--record-dir", type=pathlib.Path, default=None, help="save SVG frames here")
    p.add_argument("--record-every", type=float, default=0.5)
    p.add_argument("--headless", action="store_true", help="no live screen (for recording)")
    a = p.parse_args()

    # Stop cleanly on SIGTERM too (background jobs ignore SIGINT), so the log is closed.
    signal.signal(signal.SIGTERM, _raise_interrupt)

    proto = Protocol()
    lane = Lane(proto, a.log)
    stop = threading.Event()
    bus = can.Bus(interface=a.interface, channel=a.channel)
    notifier = can.Notifier(bus, [lane.on_frame])
    sync_thread = None
    if not a.no_sync:
        sync_thread = threading.Thread(target=sync_sender, args=(bus, proto, stop), daemon=True)
        sync_thread.start()

    recorder = Console(record=True, width=118, file=open("/dev/null", "w")) if a.record_dir else None
    if a.record_dir:
        a.record_dir.mkdir(parents=True, exist_ok=True)

    t_end = time.monotonic() + a.duration if a.duration > 0 else None
    next_rec = time.monotonic()
    frame_no = 0
    def tick(live: Live | None) -> None:
        nonlocal next_rec, frame_no
        view = lane.render(not a.no_sync)
        if live:
            live.update(view)
        if recorder and time.monotonic() >= next_rec:
            recorder.print(view)
            recorder.save_svg(str(a.record_dir / f"frame_{frame_no:04d}.svg"),
                              title="lane_console.py", clear=True)
            frame_no += 1
            next_rec += a.record_every

    try:
        if a.headless:
            while t_end is None or time.monotonic() < t_end:
                tick(None)
                time.sleep(0.1)
        else:
            with Live(lane.render(not a.no_sync), refresh_per_second=4, screen=False) as live:
                while t_end is None or time.monotonic() < t_end:
                    tick(live)
                    time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        notifier.stop()
        bus.shutdown()
        lane.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
