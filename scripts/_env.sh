# Sourced by the other scripts. Sets REPO, WS and BUILD, and activates the
# Python venv. Environment variables (all optional):
#   TRAINER_WS     west workspace (zephyr/, modules/); default $HOME/trainer-ws
#   TRAINER_VENV   Python venv with west, cantools, Robot; default $HOME/zvenv
#   TRAINER_BUILD  firmware build directory; default $TRAINER_WS/build/app
# Zephyr's build and twister break on paths that contain a space. If the
# repository path has one (for example a Windows folder seen from WSL), it is
# bind-mounted at $HOME/trainer-src first (scripts/wsl-mount.sh, needs root).

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$REPO" == *" "* ]]; then
	bash "$REPO/scripts/wsl-mount.sh" >/dev/null
	REPO="$HOME/trainer-src"
fi
WS="${TRAINER_WS:-$HOME/trainer-ws}"
BUILD="${TRAINER_BUILD:-$WS/build/app}"

# shellcheck disable=SC1091
source "${TRAINER_VENV:-$HOME/zvenv}/bin/activate"
