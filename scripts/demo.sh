#!/usr/bin/env bash
# Live lane demo: two emulated weapons in Renode bridged to SocketCAN vcan0, the
# lane console recording, and a scripted session (about 36 s). Produces the
# session CSV, report plots, console screenshots and an MP4 in docs/.
# Usage (inside WSL, as root for vcan): scripts/demo.sh
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/_env.sh"

OUT="${TRAINER_DEMO_OUT:-$WS/demo-out}"
DOCS="$REPO/docs"
PORT=4444
rm -rf "$OUT" && mkdir -p "$OUT"

ip link show vcan0 >/dev/null 2>&1 || { modprobe vcan; ip link add dev vcan0 type vcan; }
ip link set up vcan0

renode --disable-gui -P "$PORT" > "$OUT/renode.log" 2>&1 &
RENODE_PID=$!
trap 'kill $RENODE_PID 2>/dev/null || true' EXIT

cd "$REPO/tools/lane"
python3 - <<EOF
from renode_ctl import RenodeMonitor
m = RenodeMonitor(port=$PORT)
m.cmd('\$elf=@$BUILD/zephyr/zephyr.elf')
m.cmd('include @$REPO/sim/lane_live.resc', settle=3)
m.close()
EOF

# Console first, so it sees the boot frames (INFO, BIT_RESULT); it runs until the scenario ends.
python3 lane_console.py --headless --log "$OUT/session.csv" \
	--record-dir "$OUT/frames" --record-every 0.5 &
CONSOLE_PID=$!
sleep 1
python3 - <<EOF
from renode_ctl import RenodeMonitor
m = RenodeMonitor(port=$PORT)
m.cmd('start', settle=1)
m.close()
EOF
python3 demo_scenario.py | tee "$OUT/scenario.log"
sleep 1
kill -TERM "$CONSOLE_PID"
wait "$CONSOLE_PID" || true

python3 session_report.py "$OUT/session.csv" --out "$DOCS/session"
cp "$OUT/session.csv" "$DOCS/session/session.csv"
python3 frames_to_media.py "$OUT/frames" --out "$DOCS/media" --fps 2 --shots 0.45,0.99
cp /tmp/trainer_weapon1_uart.log "$OUT/" 2>/dev/null || true
echo "demo done: $DOCS/session, $DOCS/media"
