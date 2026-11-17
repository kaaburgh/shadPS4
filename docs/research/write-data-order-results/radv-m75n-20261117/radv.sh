#!/bin/bash
# Runs the given command inside a private headless weston session on RADV.
# Modelled on /mnt/UsbSSD447G/uma-e3-radv-review54-u2jbnp/radv.sh; all state lives under
# run-env/ next to this script. WESTON_LOG may name the weston log file
# (default: run-env/weston-<pid>.log).
#   ./radv.sh COMMAND [ARGS...]
set -eu
d="$(cd -- "$(dirname -- "$0")" && pwd)"
env_dir="$d/run-env"
mkdir -p "$env_dir/runtime" "$env_dir/cache" "$env_dir/tmp"
chmod 700 "$env_dir/runtime"
log="${WESTON_LOG:-$env_dir/weston-$$.log}"
export XDG_RUNTIME_DIR="$env_dir/runtime" XDG_CACHE_HOME="$env_dir/cache" TMPDIR="$env_dir/tmp"
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json SDL_VIDEO_DRIVER=wayland SDL_AUDIO_DRIVER=dummy
export WAYLAND_DISPLAY="e1c-wd-$$"
ulimit -c 0
exec weston --backend=headless --renderer=gl --no-config --idle-time=0 \
    "--socket=$WAYLAND_DISPLAY" --width=1280 --height=720 "--log=$log" -- "$@"
