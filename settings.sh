#!/usr/bin/env bash
# Edit Linux graphics settings; changes apply on the next launch.
set -euo pipefail
root=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
exec python3 "$root/tools/linux_settings.py" "$@"
