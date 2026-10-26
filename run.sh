#!/usr/bin/env bash
set -euo pipefail
root=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
exe="$root/out/build/linux-release/MC3RemixXBE"
if [[ ! -x $exe ]]; then
    echo 'Build the Linux runtime first with ./build.sh.' >&2
    exit 1
fi
game=${MC3_GAME_DIR:-$root/assets}
game=$(realpath -- "$game")
cd -- "$(dirname -- "$exe")"
exec "$exe" "$game" "$@"
