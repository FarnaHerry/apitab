#!/usr/bin/env bash
# run.sh — launch apitab under the SYSTEM glibc loader.
#
# Why: the mcpp toolchain links the binary against its private glibc
# (PT_INTERP points into the mcpp install), but the only usable OpenGL/GLX
# stack on this machine is the system Mesa, which requires a newer GLIBC.
# Running through the system ld.so with /usr/lib64 ahead of RUNPATH loads
# system glibc + system Mesa; the bundled X11 libs are forward-compatible.
# (`mcpp run` uses the mcpp interp and may exit silently with -1.)
set -euo pipefail
cd "$(dirname "$0")"

# Intel Panther Lake (Arc B390) requires INTEL_FORCE_PROBE=1, otherwise the
# iris DRI driver refuses to load → "failed to load driver: iris".
export INTEL_FORCE_PROBE=1

BIN=$(ls -dt target/x86_64-linux-gnu/*/bin 2>/dev/null | head -1)
if [ -z "$BIN" ] || [ ! -x "$BIN/apitab" ]; then
    echo "binary not found — run \`mcpp build\` first" >&2
    exit 1
fi

# --inhibit-rpath '' ignores the EXECUTABLE's DT_RPATH (mcpp toolchain glibc).
# With it inhibited, everything resolves via --library-path: /usr/lib64
# (system glibc + system Mesa) first, then $BIN for anything bundled.
exec /lib64/ld-linux-x86-64.so.2 --inhibit-rpath '' --library-path "/usr/lib64:$PWD/$BIN" "$PWD/$BIN/apitab" "$@"
