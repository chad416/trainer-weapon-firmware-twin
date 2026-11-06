#!/usr/bin/env python3
"""Scripted lane session for the demo: drives two emulated weapons through the
Renode monitor (buttons, trigger potentiometer, IMU samples) and the lane
through CAN commands. Real-time script; the emulation runs alongside it.

Run by scripts/demo.sh, after Renode has loaded sim/lane_live.resc and started.
"""
from __future__ import annotations

import time

import can

from lane_cmd import send_cmd
from protocol import KEYS, OPCODES, Protocol
from renode_ctl import RenodeMonitor

W1, W2 = "weapon1", "weapon2"
IMU = "sysbus.i2c1.lsm6dso"


class Demo:
    def __init__(self, mon: RenodeMonitor, bus: can.BusABC):
        self.mon, self.bus, self.proto = mon, bus, Protocol()

    def say(self, text: str) -> None:
        print(f"[demo] {text}", flush=True)

    def press(self, w: str, sw: str, hold: float = 0.25) -> None:
        self.mon.on(w, f"sysbus.gpioPortG.{sw} Press")
        time.sleep(hold)  # longer than the 30 ms debounce in emulated time
        self.mon.on(w, f"sysbus.gpioPortG.{sw} Release")
        time.sleep(0.15)

    def trigger(self, w: str, pct: int) -> None:
        self.mon.on(w, f"sysbus.adcM1S2.potentiometer SetPercentage {pct}")

    def squeeze(self, w: str, steps=(15, 25, 35, 45, 55, 62, 68, 74, 100), dwell: float = 0.04) -> None:
        """Aimed shot: take-up, then a progressive squeeze through the break."""
        for pct in steps:
            self.trigger(w, pct)
            time.sleep(dwell)
        time.sleep(0.2)
        self.trigger(w, 0)
        time.sleep(0.8)
        self.mon.drain()

    def level(self, w: str, roll_g=(0.0, 1.0)) -> None:
        self.mon.on(w, f"{IMU} FeedAccelerationSample 0 {roll_g[0]} {roll_g[1]} 1")
        self.mon.on(w, f"{IMU} FeedAngularRateSample 0.1 -0.2 0.1 1")

    def cmd(self, node: int, op: str, key: str | None = None, value: int = 0) -> None:
        status, _ = send_cmd(self.bus, self.proto, node, OPCODES[op], KEYS.get(key, 0) if key else 0, value)
        self.say(f"lane -> W{node} {op} {key or ''} {value if op == 'set' else ''} : status {status}")

    def drill(self, w: str) -> None:
        self.press(w, "sw4")  # tap
        self.press(w, "sw3")  # rack

    def run(self) -> None:
        self.say("weapons booting")
        time.sleep(3.0)
        for w in (W1, W2):
            self.level(w)

        self.say("W1: SEMI, magazine, rack")
        self.press(W1, "sw1")
        self.press(W1, "sw2")
        self.press(W1, "sw3")
        self.say("W1: four aimed shots")
        for _ in range(4):
            self.squeeze(W1)

        self.say("W1: flinch on the next shot (muzzle dips just before the break)")
        self.trigger(W1, 60)
        time.sleep(0.1)
        # Outside FIFO mode the Renode IMU model holds the latest fed value, so the dip
        # lasts until the next feed: hold it through the break, then level out.
        self.mon.on(W1, f"{IMU} FeedAngularRateSample 0 28 0 1")
        time.sleep(0.12)
        self.squeeze(W1, steps=(100,), dwell=0.05)
        self.level(W1)

        self.say("W1: canted shot (rolled 10 deg)")
        self.level(W1, roll_g=(0.17365, 0.98481))
        self.squeeze(W1)
        self.level(W1)

        self.say("W2: 900 rpm, AUTO, magazine, rack, burst")
        self.cmd(2, "set", "auto_rpm", 900)
        self.press(W2, "sw1")
        self.press(W2, "sw1")
        self.press(W2, "sw2")
        self.press(W2, "sw3")
        self.trigger(W2, 100)
        time.sleep(0.9)
        self.trigger(W2, 0)
        time.sleep(0.5)

        self.say("Instructor injects a stoppage on W1; trainee clears it")
        self.cmd(1, "inject")
        time.sleep(0.8)
        self.squeeze(W1, steps=(100,), dwell=0.1)  # click: blocked
        self.drill(W1)
        self.squeeze(W1)

        self.say("W2: random stoppages, 30 % per round")
        self.cmd(2, "set", "stoppage_mode", 2)
        self.cmd(2, "set", "stoppage_prob_pm", 300)
        self.trigger(W2, 100)
        time.sleep(1.5)
        self.trigger(W2, 0)
        time.sleep(0.4)
        self.drill(W2)
        self.cmd(2, "set", "stoppage_mode", 0)

        self.say("W1: 1 s clearance window, slow drill -> CLEAR_LATE")
        self.cmd(1, "set", "clear_window_ms", 1000)
        self.cmd(1, "inject")
        time.sleep(2.0)
        self.drill(W1)
        self.squeeze(W1)

        self.say("W2: empty the magazine and reload")
        self.trigger(W2, 100)
        time.sleep(3.0)
        self.trigger(W2, 0)
        time.sleep(0.3)
        self.press(W2, "sw2")
        self.press(W2, "sw2")
        self.press(W2, "sw3")
        self.say("done")


def main() -> int:
    mon = RenodeMonitor()
    with can.Bus(interface="socketcan", channel="vcan0") as bus:
        Demo(mon, bus).run()
    mon.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
