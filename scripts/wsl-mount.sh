#!/usr/bin/env bash
# Bind-mount this repository at a path without spaces. Only needed when the
# checkout path contains a space (for example a Windows folder seen from WSL):
# Zephyr's build and twister break on such paths, and a symlink is not enough
# because twister resolves it to the real path. _env.sh calls this
# automatically in that case. mount --bind needs root: run as root, or clone
# the repository to a path without spaces and this is never called.
# Usage: bash "<repo>/scripts/wsl-mount.sh" [mount point]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-$HOME/trainer-src}"

if mountpoint -q "$TARGET"; then
	echo "already mounted: $TARGET"
	exit 0
fi

[ -L "$TARGET" ] && rm "$TARGET"
mkdir -p "$TARGET"
mount --bind "$REPO" "$TARGET"
echo "mounted $REPO -> $TARGET"
