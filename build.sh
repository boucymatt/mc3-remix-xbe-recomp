#!/usr/bin/env bash
# Build the native Linux SDL/OpenGL/Vulkan game, or the optional headless harness.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build.sh [build|check|headless|check-headless|--help]

  build          Compile the Linux SDL/OpenGL/Vulkan executable (default).
  check          Check native tools and game files without downloading/building.
  headless       Compile the optional headless research harness.
  check-headless Check only the headless prerequisites.

Requires x86-64 Linux, Clang, CMake >= 3.25, Ninja, Git and Python >= 3.9.
Native builds also need pkg-config, SDL3 >= 3.2, libepoxy, shaderc and Vulkan >= 1.3 development files.
The first build downloads pinned Zydis, toml++ and SPIRV-Cross dependencies.
Set XBE_BUILD_JOBS=N to override the default of at most 8 parallel jobs.
The native runtime needs an OpenGL 4.5 or Vulkan 1.3 capable graphics driver.
EOF
}

mode=${1:-build}
if (( $# > 1 )); then usage >&2; exit 2; fi
case "$mode" in
    build|check|headless|check-headless) ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

cd -- "$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")"
trap 'echo "BUILD FAILED. See the error above." >&2' ERR
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo 'This build requires x86-64 Linux.' >&2
    exit 1
fi

missing=0
for tool in clang clang++ cmake ninja git python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing build tool: $tool (see DEPENDENCIES.md)" >&2
        missing=1
    fi
done
if (( missing )); then exit 1; fi

preset=linux-release
if [[ $mode == headless || $mode == check-headless ]]; then preset=linux-headless; fi
if [[ $preset == linux-release ]]; then
    if ! command -v pkg-config >/dev/null; then echo 'Missing pkg-config (pkgconf on Arch Linux).' >&2; exit 1; fi
    for dependency in 'sdl3 >= 3.2' epoxy shaderc 'vulkan >= 1.3'; do
        if ! pkg-config --exists "$dependency"; then
            echo "Missing native development dependency: $dependency (see DEPENDENCIES.md)" >&2
            missing=1
        fi
    done
    if (( missing )); then exit 1; fi
fi

jobs=${XBE_BUILD_JOBS:-$(nproc)}
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    echo 'XBE_BUILD_JOBS must be a positive integer.' >&2
    exit 2
fi
if [[ -z ${XBE_BUILD_JOBS:-} ]] && (( jobs > 8 )); then jobs=8; fi

python3 - <<'PY'
import hashlib
from pathlib import Path
import re
import subprocess
import sys

if sys.version_info < (3, 9):
    sys.exit('Python 3.9 or newer is required.')
version = subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0]
match = re.search(r'(\d+)\.(\d+)\.(\d+)', version)
if not match or tuple(map(int, match.groups())) < (3, 25, 0):
    sys.exit('CMake 3.25 or newer is required.')
expected = dict(line.split('=', 1) for line in Path('config/supported_xbe.txt').read_text().splitlines()
                if line and not line.startswith('#'))
xbe = Path('assets/default.xbe')
if not xbe.is_file():
    sys.exit('Missing assets/default.xbe. Put your extracted game in assets/; see DEPENDENCIES.md.')
digest = hashlib.sha1(xbe.read_bytes()).hexdigest()
if xbe.stat().st_size != int(expected['size']) or digest != expected['sha1']:
    sys.exit(f'assets/default.xbe is not the supported build.\nFound SHA-1: {digest}\nExpected: {expected["sha1"]}')
for data in ('assets_x.dat', 'assets_x.dat_2'):
    if not Path('assets', data).is_file():
        sys.exit(f'Missing assets/{data}; the extracted game data is required for the build.')
print(version)
print(f'Python {sys.version.split()[0]}')
print(f'Supported game image verified: {digest}')
PY

if [[ $mode == check || $mode == check-headless ]]; then
    echo "Prerequisites passed for $preset."
    exit 0
fi

echo "Building $preset with $jobs parallel jobs."
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel "$jobs"
echo "Build passed: out/build/$preset/MC3RemixXBE"
if [[ $preset == linux-release ]]; then echo 'Run ./run.sh to play.'; fi
echo 'Generated game code stays local.'
