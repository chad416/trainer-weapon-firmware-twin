#!/usr/bin/env python3
"""Generate synthetic IMU recordings (RESD) for the trainer's IMU tests.

Each profile is 1 kHz, weapon frame (x forward, y left, z up):
  steady  - well-held weapon, tiny tremor
  shaky   - large aim wobble (muzzle rate well above steady)
  flinch  - steady for 500 ms, then the muzzle dips (gy = +25 deg/s) for 100 ms
  canted  - steady, rolled 10 degrees

Pull the trigger about 550 ms after starting the replay: the 50 ms before the
shot then lies inside the dip of the flinch profile (flinch ~ 1.25 deg).

RESD units (Renode LSM6DSO model): acceleration in micro-g, angular rate in
1e-5 rad/s. The CSVs are kept next to the RESD files for inspection.

Usage: python3 sim/data/gen_imu_profiles.py [--csv2resd /opt/renode/tools/csv2resd/csv2resd.py]
"""
import argparse
import csv
import math
import pathlib
import random
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
RATE_HZ = 1000
DURATION_S = 0.6
DEG = math.pi / 180.0


def profile(name: str, t: float, rng: random.Random):
    """Return (ax, ay, az) in g and (gx, gy, gz) in deg/s at time t."""
    tremor = lambda: rng.gauss(0.0, 0.2)  # noqa: E731
    roll = 10.0 * DEG if name == "canted" else 0.0
    acc = (0.0, math.sin(roll), math.cos(roll))

    if name == "shaky":
        gyr = (0.0, 12.0 * math.sin(2 * math.pi * 3 * t), 9.0 * math.cos(2 * math.pi * 2 * t))
    elif name == "flinch" and t >= 0.5:
        gyr = (0.0, 25.0, 0.0)
    else:
        gyr = (tremor(), tremor(), tremor())
    return acc, gyr


def write_profile(name: str, csv2resd: str) -> None:
    rng = random.Random(42)  # deterministic
    csv_path = HERE / f"imu_{name}.csv"
    resd_path = HERE / f"imu_{name}.resd"
    n = int(RATE_HZ * DURATION_S)

    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ax", "ay", "az", "gx", "gy", "gz"])
        for i in range(n):
            acc, gyr = profile(name, i / RATE_HZ, rng)
            w.writerow([round(a * 1e6) for a in acc] + [round(g * DEG * 1e5) for g in gyr])

    subprocess.run(
        [sys.executable, csv2resd,
         "--input", str(csv_path),
         "--map", "acceleration:ax,ay,az:x,y,z:0",
         "--map", "angular_rate:gx,gy,gz:x,y,z:0",
         "--start-time", "0",
         "--frequency", str(RATE_HZ),
         str(resd_path)],
        check=True,
    )
    print(f"wrote {resd_path.name} ({n} samples)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv2resd", default="/opt/renode/tools/csv2resd/csv2resd.py")
    args = parser.parse_args()
    for name in ("steady", "shaky", "flinch", "canted"):
        write_profile(name, args.csv2resd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
