#!/bin/sh
# Example Linux launcher for Starsiege with sswrap under Wine.
# Adjust the three paths below. Optional: mount your disc image first (the game checks for its CD).
WINEPREFIX=${WINEPREFIX:-$HOME/.wine}
GAME_DIR=${GAME_DIR:-$WINEPREFIX/drive_c/Dynamix/Starsiege}
CPU_CORE=${CPU_CORE:-2}          # pin to one core: the 1999 engine stutters when threads migrate
export WINEPREFIX

cd "$GAME_DIR" || exit 1

# The game rewrites defaultPrefs.cs on exit and can save itself into windowed mode, where it
# falls back to the Software renderer. Keep it on OpenGL + fullscreen (resolution stays yours).
sed -i -E 's/^(\$pref::GWC::SIM_FS_DEVICE = )".*";/\1"OpenGL";/; s/^(\$pref::GWC::SIM_IS_FULLSCREEN = )".*";/\1"True";/' defaultPrefs.cs

# Pin the game to one core once it has started (threads created later inherit the pin).
( for i in $(seq 60); do
    P=$(pgrep -x Starsiege.exe | head -1)
    if [ -n "$P" ]; then for t in /proc/$P/task/*; do taskset -p -c "$CPU_CORE" "${t##*/}" >/dev/null 2>&1; done; exit 0; fi
    sleep 0.5
  done ) &

# NVIDIA: threaded GL optimizations hurt this renderer; force 16x anisotropic filtering.
export __GL_THREADED_OPTIMIZATIONS=0 __GL_LOG_MAX_ANISO=4
exec wine Starsiege.exe "$@"
