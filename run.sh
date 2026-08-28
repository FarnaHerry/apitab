#!/usr/bin/env bash
# run.sh — 运行 CMake 构建出的 apitab。
#
# mcpp 时代这里要用系统 ld.so 绕开私有 glibc；GCC 直接链接系统 glibc 后不再需要，
# 只保留 Intel Panther Lake (Arc B390) 的 INTEL_FORCE_PROBE（否则 iris DRI 拒载）。
# 工作目录切到仓库根，保证 eui 能按 "assets/字体" 解析资源。
set -euo pipefail
cd "$(dirname "$0")"

BIN="${APITAB_BIN:-build/apitab}"
if [ ! -x "$BIN" ]; then
    echo "binary not found — run \`cmake -B build && cmake --build build\` first" >&2
    exit 1
fi

export INTEL_FORCE_PROBE="${INTEL_FORCE_PROBE:-1}"
exec "./$BIN" "$@"
