#!/usr/bin/env bash
# Run AllPowers R4000 BLE emulator (macOS / Linux).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV="${ROOT}/.venv-emulate"

if [[ ! -d "$VENV" ]]; then
  python3 -m venv "$VENV"
  # shellcheck disable=SC1091
  source "$VENV/bin/activate"
  pip install -q -r "$ROOT/requirements-emulate.txt"
else
  # shellcheck disable=SC1091
  source "$VENV/bin/activate"
fi

exec python3 "$ROOT/emulate_r4000.py" "$@"
