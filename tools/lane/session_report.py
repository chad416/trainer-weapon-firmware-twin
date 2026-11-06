#!/usr/bin/env python3
"""Turn a lane_console session CSV into plots and a short Markdown summary.

Outputs (in --out):
  shot_timeline.png    shot number against lane time, per weapon
  trigger_squeeze.png  take-up and squeeze time per aimed shot
  imu_per_shot.png     flinch and muzzle rate per shot
  summary.md           counts, stoppages and clearance times

Usage: session_report.py session.csv --out docs/session/
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def load(path: pathlib.Path):
    shots = collections.defaultdict(list)    # node -> [(shot_no, t_us, mode)]
    details = collections.defaultdict(dict)  # node -> shot_no -> signals
    events = []
    for row in csv.DictReader(path.open()):
        node = int(row["node"])
        s = json.loads(row["signals"])
        if row["message"] == "SHOT_EVENT":
            shots[node].append((int(s["shot_no"]), int(s["t_us"]), int(s["mode"])))
        elif row["message"] == "SHOT_DETAIL":
            details[node][int(s["shot_no"])] = s
        elif row["message"] == "EVENT":
            events.append((float(row["host_s"]), node, int(s["type"]), int(s["value"])))
    return shots, details, events


def unwrap_s(t_us_list):
    """Lane time in seconds, unwrapping the 32-bit microsecond counter."""
    out, offset, prev = [], 0, None
    for t in t_us_list:
        if prev is not None and t < prev and (prev - t) > 2**31:
            offset += 2**32
        out.append((t + offset) / 1e6)
        prev = t
    return out


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("csv", type=pathlib.Path)
    p.add_argument("--out", type=pathlib.Path, default=pathlib.Path("session_report"))
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    shots, details, events = load(a.csv)
    nodes = sorted(shots)
    colours = {1: "tab:blue", 2: "tab:orange"}

    # 1. shot timeline (all weapons on the one lane clock, so one common zero)
    fig, ax = plt.subplots(figsize=(8, 4))
    times = {n: unwrap_s([x[1] for x in shots[n]]) for n in nodes}
    t0 = min((t[0] for t in times.values() if t), default=0.0)
    for n in nodes:
        ax.plot([x - t0 for x in times[n]], [x[0] for x in shots[n]], ".", ms=4,
                color=colours.get(n), label=f"weapon {n}")
    ax.set_xlabel("lane time since the first shot on the lane (s)")
    ax.set_ylabel("shot number")
    ax.set_title("Shot timeline (SHOT_EVENT)")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(a.out / "shot_timeline.png", dpi=120)
    plt.close(fig)

    # 2. trigger squeeze per shot (SEMI shots only: AUTO repeats the first break's metrics)
    fig, ax = plt.subplots(figsize=(8, 4))
    width = 0.4
    for i, n in enumerate(nodes):
        semi = [x[0] for x in shots[n] if x[2] == 1 and x[0] in details[n]]
        if not semi:
            continue
        xs = [k + i * width for k in semi]
        ax.bar(xs, [details[n][k]["takeup_ms"] for k in semi], width, color=colours.get(n),
               alpha=0.5, label=f"W{n} take-up")
        ax.bar(xs, [details[n][k]["squeeze_ms"] for k in semi], width, color=colours.get(n),
               bottom=[details[n][k]["takeup_ms"] for k in semi], label=f"W{n} squeeze")
    ax.set_xlabel("shot number (aimed SEMI shots)")
    ax.set_ylabel("ms")
    ax.set_title("Trigger: take-up + squeeze time per aimed shot (SHOT_DETAIL)")
    ax.grid(alpha=0.3, axis="y")
    ax.legend()
    fig.tight_layout()
    fig.savefig(a.out / "trigger_squeeze.png", dpi=120)
    plt.close(fig)

    # 3. IMU metrics per shot
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 5), sharex=True)
    for n in nodes:
        ks = sorted(details[n])
        ax1.plot(ks, [details[n][k]["flinch"] for k in ks], "o-", ms=3, color=colours.get(n), label=f"W{n}")
        ax2.plot(ks, [details[n][k]["muzzle_rate"] for k in ks], "o-", ms=3, color=colours.get(n), label=f"W{n}")
    ax1.axhline(0.8, color="red", lw=0.8, ls="--", label="flinch flag (0.8 deg)")
    ax1.set_ylabel("flinch (deg)")
    ax2.set_ylabel("muzzle rate (deg/s)")
    ax2.set_xlabel("shot number")
    ax1.set_title("IMU before each shot (SHOT_DETAIL)")
    for ax in (ax1, ax2):
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(a.out / "imu_per_shot.png", dpi=120)
    plt.close(fig)

    # 4. summary
    clears = [(n, v) for (_, n, typ, v) in events if typ == 6]
    late = [(n, v) for (_, n, typ, v) in events if typ == 10]
    stoppages = sum(1 for e in events if e[2] == 5)
    dry = sum(1 for e in events if e[2] == 1)
    lines = ["# Session summary", "", f"Source: `{a.csv.name}`", "",
             "| weapon | shots | aimed (SEMI) | flinch >= 0.8 deg |", "|---|---|---|---|"]
    for n in nodes:
        semi = sum(1 for x in shots[n] if x[2] == 1)
        flinches = sum(1 for d in details[n].values() if d["flinch"] >= 0.8)
        lines.append(f"| W{n} | {len(shots[n])} | {semi} | {flinches} |")
    lines += ["", f"- Stoppages: {stoppages}, dry fires: {dry}",
              f"- Clearance times (ms): {', '.join(f'W{n} {v}' for n, v in clears) or '-'}",
              f"- Late clearances: {', '.join(f'W{n} {v} ms' for n, v in late) or 'none'}",
              "", "![shot timeline](shot_timeline.png)", "",
              "![trigger squeeze](trigger_squeeze.png)", "", "![imu per shot](imu_per_shot.png)", ""]
    (a.out / "summary.md").write_text("\n".join(lines))
    print(f"wrote {a.out}/summary.md and 3 plots")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
