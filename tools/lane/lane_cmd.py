#!/usr/bin/env python3
"""Send one command to a trainer node and print its ACK.

Examples:
  lane_cmd.py 1 get capacity
  lane_cmd.py 2 set auto_rpm 900
  lane_cmd.py 1 inject
  lane_cmd.py 2 set stoppage_mode 2 && lane_cmd.py 2 set stoppage_prob_pm 200
"""
from __future__ import annotations

import argparse
import sys

import can

from protocol import FN_CMD_ACK, KEYS, OPCODES, STATUS_TEXT, Protocol, can_id


def send_cmd(bus: can.BusABC, proto: Protocol, node: int, opcode: int, key: int = 0,
             value: int = 0, timeout: float = 1.0, retries: int = 2) -> tuple[int, int]:
    """Send CMD and wait for the matching CMD_ACK. Returns (status, value)."""
    frame_id, data = proto.encode_cmd(node, opcode, key, value)
    ack_id = can_id(FN_CMD_ACK, node)
    for _ in range(retries + 1):
        bus.send(can.Message(arbitration_id=frame_id, data=data, is_extended_id=False))
        while True:
            msg = bus.recv(timeout)
            if msg is None:
                break  # timeout: retry
            if msg.arbitration_id == ack_id and msg.data[1] == opcode and msg.data[2] == key:
                d = proto.decode(msg.arbitration_id, bytes(msg.data))
                return int(d.signals["status"]), int(d.signals["value"])
    raise TimeoutError(f"no CMD_ACK from node {node}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("node", type=int)
    p.add_argument("op", choices=OPCODES)
    p.add_argument("key", nargs="?", default=None, help=", ".join(KEYS))
    p.add_argument("value", nargs="?", type=int, default=0)
    p.add_argument("--channel", default="vcan0")
    p.add_argument("--interface", default="socketcan")
    a = p.parse_args()

    key = KEYS.get(a.key, 0) if a.key else 0
    if a.op in ("get", "set") and a.key not in KEYS:
        p.error(f"key must be one of: {', '.join(KEYS)}")

    proto = Protocol()
    with can.Bus(interface=a.interface, channel=a.channel, receive_own_messages=False) as bus:
        status, value = send_cmd(bus, proto, a.node, OPCODES[a.op], key, a.value)
    print(f"node {a.node} {a.op} {a.key or ''} -> {STATUS_TEXT.get(status, status)} value={value}")
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
