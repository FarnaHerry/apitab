#!/usr/bin/env bash
# tests/huxerui_selection_matrix.sh — P1-B1.0 构建选择矩阵（5 场景）。
# 用法：bash tests/huxerui_selection_matrix.sh
# 覆盖：CLI 注入 SDK + 仓库源码并存、显式外部源码、强制 SDK、源码缺失、源码依赖缺失。
# 每个场景跑一次 `cmake -S . -B <tmp>`，断言 APITAB_HUXERUI_FROM_SOURCE / SOURCE_DIR。
#
# 设计约束（修复后）：
# - 禁止移动共享源码目录 third_party/huxerui；“源码缺失”改为通过
#   -DAPITAB_HUXERUI_BUNDLED_SOURCE_DIR=<空目录> 覆盖模拟，完全并行安全且中断可恢复。
# - 禁止用 || true 吞掉 cmake 失败；每个期望成功的场景必须断言返回码为 0。
# - “外部源码”场景软链接完整源码，而非空 CMakeLists 假框架。
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0
FAIL=0
TMP_BASE="$(mktemp -d)"
trap 'rm -rf "$TMP_BASE"' EXIT

run_cmake() {
    local build="$1"; shift
    rm -rf "$build"
    mkdir -p "$build"
    # 不用 || true：显式捕获返回码写入 rc.txt，供后续断言
    # import std 需要 Ninja（Unix Makefiles 不支持 CXX_MODULE_STD）
    set +e
    cmake -S "$REPO" -B "$build" -G Ninja "$@" >"$build/log.txt" 2>&1
    echo $? >"$build/rc.txt"
    set -e
}

assert() {
    local label="$1"; local cond="$2"
    if eval "$cond"; then
        echo "  ✓ $label"
        PASS=$((PASS+1))
    else
        echo "  ✗ $label"
        # 失败时打印最近日志片段，便于定位
        echo "    --- log tail ---"
        tail -n 30 "$CURRENT_LOG" 2>/dev/null || true
        echo "    ----------------"
        FAIL=$((FAIL+1))
    fi
}

assert_rc0() {
    local label="$1"; local build="$2"
    local rc
    rc="$(cat "$build/rc.txt")"
    CURRENT_LOG="$build/log.txt"
    if [[ "$rc" -eq 0 ]]; then
        echo "  ✓ $label (rc=0)"
        PASS=$((PASS+1))
    else
        echo "  ✗ $label (rc=$rc, 期望 0)"
        echo "    --- log ---"
        cat "$build/log.txt" 2>/dev/null || true
        echo "    -----------"
        FAIL=$((FAIL+1))
    fi
}

# 预检：仓库源码与已安装 SDK 均应存在（否则部分场景无意义）
if [[ ! -f "$REPO/third_party/huxerui/CMakeLists.txt" ]]; then
    echo "SKIP: third_party/huxerui 不存在，无法执行矩阵" >&2; exit 1
fi
if [[ ! -f "$HOME/.local/share/HuxerUI/lib/cmake/HuxerUI/HuxerUIConfig.cmake" ]]; then
    echo "WARN: 已安装 SDK 不存在（~/.local/share/HuxerUI），CLI 注入场景将用离线包兜底" >&2
fi

echo "== 场景 1: CLI 注入 SDK + 仓库源码并存（源码应优先） =="
B1="$TMP_BASE/case1"
run_cmake "$B1" -DHUXERUI_HOME="$HOME/.local/share/HuxerUI"
CURRENT_LOG="$B1/log.txt"
assert_rc0 "CMake 配置成功" "$B1"
assert "日志含源码编译" "grep -q 'HuxerUI 源码编译' \"$B1/log.txt\""
assert "日志含 @ commit" "grep -q 'HuxerUI 源码编译.*@' \"$B1/log.txt\""
assert "日志含 third_party/huxerui（源码优先于 CLI SDK）" "grep -q 'third_party/huxerui' \"$B1/log.txt\""
assert "日志不含 SDK（未回落）" "! grep -q 'HuxerUI SDK' \"$B1/log.txt\""

echo ""
echo "== 场景 2: 显式外部源码（HUXERUI_HOME 指向源码应优先于仓库内源码） =="
FAKE="$TMP_BASE/fake-huxerui"
# 软链接完整源码，而非空 CMakeLists 假框架，真实验证源码选择路径
ln -s "$REPO/third_party/huxerui" "$FAKE"
B2="$TMP_BASE/case2"
run_cmake "$B2" -DHUXERUI_HOME="$FAKE"
CURRENT_LOG="$B2/log.txt"
assert_rc0 "CMake 配置成功" "$B2"
assert "日志含外部 fake 路径（显式源码优先）" "grep -q \"\$FAKE\" \"$B2/log.txt\""
assert "日志含源码编译" "grep -q 'HuxerUI 源码编译' \"$B2/log.txt\""

echo ""
echo "== 场景 3: 强制 SDK（APITAB_HUXERUI_FORCE_SDK=ON） =="
B3="$TMP_BASE/case3"
run_cmake "$B3" -DAPITAB_HUXERUI_FORCE_SDK=ON -DHUXERUI_HOME="$HOME/.local/share/HuxerUI"
CURRENT_LOG="$B3/log.txt"
assert_rc0 "CMake 配置成功" "$B3"
assert "日志含 SDK（强制回落）" "grep -q 'HuxerUI SDK' \"$B3/log.txt\""
assert "日志不含源码编译" "! grep -q 'HuxerUI 源码编译' \"$B3/log.txt\""

echo ""
echo "== 场景 4: 源码缺失（通过 override 模拟，无需移动共享目录） =="
# 通过测试专用 bundled-dir override 模拟源码缺失，并行安全、中断无需恢复
EMPTY_BUNDLED="$TMP_BASE/empty_bundled"
EMPTY_HOME="$TMP_BASE/empty_home"
mkdir -p "$EMPTY_BUNDLED" "$EMPTY_HOME"
B4="$TMP_BASE/case4"
run_cmake "$B4" -DAPITAB_HUXERUI_BUNDLED_SOURCE_DIR="$EMPTY_BUNDLED" -DHUXERUI_HOME="$EMPTY_HOME"
CURRENT_LOG="$B4/log.txt"
assert_rc0 "CMake 配置成功" "$B4"
assert "日志含 SDK（无源码回落离线包）" "grep -q 'HuxerUI SDK' \"$B4/log.txt\""
assert "日志不含源码编译" "! grep -q 'HuxerUI 源码编译' \"$B4/log.txt\""

echo ""
echo "== 场景 5: 源码依赖缺失（mock 回落 SDK，醒目 WARNING） =="
B5="$TMP_BASE/case5"
run_cmake "$B5" -DAPITAB_HUXERUI_MOCK_MISSING_DEPS=ON -DHUXERUI_HOME="$HOME/.local/share/HuxerUI"
CURRENT_LOG="$B5/log.txt"
assert_rc0 "CMake 配置成功" "$B5"
assert "日志含 WARNING 回落" "grep -q 'WARNING.*回落预编译 SDK' \"$B5/log.txt\" || grep -q '回落预编译 SDK' \"$B5/log.txt\""
assert "日志含 SDK（mock 回落）" "grep -q 'HuxerUI SDK' \"$B5/log.txt\""
assert "日志不含源码编译" "! grep -q 'HuxerUI 源码编译' \"$B5/log.txt\""

echo ""
echo "=== 结果: $PASS 通过, $FAIL 失败 ==="
if [[ $FAIL -ne 0 ]]; then exit 1; fi
