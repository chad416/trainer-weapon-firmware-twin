#!/usr/bin/env bash
# Run the Robot Framework integration tests in Renode against the built firmware.
# Usage (inside WSL): scripts/test-sim.sh [extra renode-test args]
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/_env.sh"

OUT="${TRAINER_ROBOT_OUT:-$WS/robot-out}"
mkdir -p "$OUT"
# renode-test mishandles a current directory containing spaces, and writes
# robot_output.xml, log.html and report.html to the current directory.
cd "$OUT"
renode-test -r "$OUT" --variable "ELF:$BUILD/zephyr/zephyr.elf" "$@" "$REPO/tests/robot/trainer.robot"
