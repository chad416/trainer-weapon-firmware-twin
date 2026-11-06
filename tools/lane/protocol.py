"""Lane-side view of the trainer CAN protocol (protocol/trainer.dbc).

ID = (function << 7) | node. The DBC defines each node-addressed message for
node 1; other nodes use the same layout at ID + (node - 1).
"""
from __future__ import annotations

import pathlib
from dataclasses import dataclass

import cantools

ROOT = pathlib.Path(__file__).resolve().parents[2]
DBC_PATH = ROOT / "protocol" / "trainer.dbc"

FN_SYNC, FN_SHOT_EVENT, FN_SHOT_DETAIL, FN_STATUS = 1, 2, 3, 4
FN_EVENT, FN_BIT_RESULT, FN_CMD, FN_CMD_ACK, FN_INFO = 5, 6, 7, 8, 9

STATES = ["SAFE", "ARMED", "EMPTY", "LOADED", "READY", "FIRING", "STOPPAGE", "FAULT"]
SELECTORS = ["SAFE", "SEMI", "AUTO"]
EVENTS = {1: "DRY_FIRE", 2: "MAG_IN", 3: "MAG_OUT", 4: "RACK", 5: "STOPPAGE",
          6: "CLEARED", 7: "SELECTOR", 8: "TAP", 9: "STATE", 10: "CLEAR_LATE"}
KEYS = {"node_id": 1, "capacity": 2, "auto_rpm": 3, "emitter_us": 4, "valve_ms": 5,
        "break_pm": 6, "reset_pm": 7, "boot_count": 8, "stoppage_mode": 9,
        "stoppage_round": 10, "stoppage_prob_pm": 11, "clear_window_ms": 12}
OPCODES = {"get": 1, "set": 2, "bit": 3, "inject": 4, "reboot": 5, "defaults": 6}
STATUS_TEXT = {0: "OK", 1: "BAD_OPCODE", 2: "BAD_KEY", 3: "BAD_VALUE", 4: "SAVE_FAILED",
               5: "READ_ONLY"}


def can_id(function: int, node: int) -> int:
    return (function << 7) | (node & 0x7F)


@dataclass
class Decoded:
    function: int
    node: int
    name: str
    signals: dict


class Protocol:
    def __init__(self, dbc_path: pathlib.Path = DBC_PATH):
        self.db = cantools.database.load_file(str(dbc_path))

    def decode(self, frame_id: int, data: bytes) -> Decoded | None:
        function, node = frame_id >> 7, frame_id & 0x7F
        base = 0x080 if function == FN_SYNC else can_id(function, 1)
        try:
            msg = self.db.get_message_by_frame_id(base)
        except KeyError:
            return None
        try:
            sig = msg.decode(data.ljust(msg.length, b"\x00"), decode_choices=False)
        except Exception:  # malformed or foreign frame on a shared bus
            return None
        name = msg.name.removesuffix("_N1")
        return Decoded(function, node, name, dict(sig))

    def encode_sync(self, lane_time_us: int) -> tuple[int, bytes]:
        msg = self.db.get_message_by_name("SYNC")
        return msg.frame_id, msg.encode({"isotp_pci": 4, "lane_time_us": lane_time_us & 0xFFFFFFFF})

    def encode_cmd(self, node: int, opcode: int, key: int = 0, value: int = 0) -> tuple[int, bytes]:
        msg = self.db.get_message_by_name("CMD_N1")
        data = msg.encode({"isotp_pci": 6, "opcode": opcode, "key": key, "value": value})
        return can_id(FN_CMD, node), data
