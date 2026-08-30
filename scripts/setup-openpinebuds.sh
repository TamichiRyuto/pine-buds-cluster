#!/usr/bin/env bash
# Set up the OpenPineBuds SDK checkout and verify host prerequisites.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${REPO_ROOT}/external/OpenPineBuds"
SDK_URL="https://github.com/pine64/OpenPineBuds.git"

if [ -d "${SDK_DIR}/.git" ]; then
    echo "[ok] OpenPineBuds already cloned at ${SDK_DIR}"
else
    echo "[..] Cloning OpenPineBuds (shallow) into ${SDK_DIR}"
    git clone --depth 1 "${SDK_URL}" "${SDK_DIR}"
fi

if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    echo "[ok] docker is available"
else
    echo "[!!] docker is NOT usable in this shell."
    echo "     On WSL2: enable Docker Desktop > Settings > Resources > WSL integration"
    echo "     for this distro, then reopen the shell. Firmware builds need it."
fi

echo
echo "Next steps (run inside ${SDK_DIR}):"
echo "  docker compose run --rm builder ./build.sh      # build firmware"
echo "  docker compose run --rm builder ./backup.sh     # back up stock firmware BEFORE first flash"
echo "  docker compose run --rm builder ./download.sh   # flash (see docs/manual.md for bud handling)"
