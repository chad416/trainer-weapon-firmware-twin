#!/usr/bin/env bash
# Build the firmware for the STM32H7 Renode Reference Platform.
# Usage (inside WSL or Linux): scripts/build.sh [extra west build args]
#   TRAINER_IMAGE=test  (default) prj.conf + debug.conf: the image the Renode
#                       tests and the demo run; it has the debug shell commands
#   TRAINER_IMAGE=field prj.conf alone: no debug commands
# Output: $TRAINER_BUILD (default $TRAINER_WS/build/app), see _env.sh.
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/_env.sh"

case "${TRAINER_IMAGE:-test}" in
test) EXTRA=(-DEXTRA_CONF_FILE=debug.conf) ;;
field) EXTRA=() ;;
*) echo "TRAINER_IMAGE must be test or field" >&2; exit 2 ;;
esac

cd "$WS"
west build -p auto -b stm32h7_renode_reference_board -d "$BUILD" "$REPO/app" "$@" -- "${EXTRA[@]}"
