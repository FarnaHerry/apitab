#!/usr/bin/env bash
# apitab-wrapper.sh — 安装到 /usr/bin/apitab 的启动包装器。
# apitab 按 <exeDir>/assets 解析字体/资源，切到安装目录再启动。
export INTEL_FORCE_PROBE="${INTEL_FORCE_PROBE:-1}"
cd /opt/apitab
exec ./apitab "$@"
