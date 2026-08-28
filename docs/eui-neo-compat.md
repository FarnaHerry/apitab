# EUI-NEO Compatibility

apitab vendors EUI-NEO `0.5.7` under `third_party/` (source of truth:
`third_party/CMakeLists.txt` + `third_party/README.md`).

## Required Reading Order

1. `docs/skills/eui-neo-ui-replicator/SKILL.md` — pinned official upstream guidance.
2. The installed EUI-NEO `0.5.7` DSL, component, and workshop documentation.
3. `/home/farna/.claude/skills/eui-development/SKILL.md` — apitab/tinynext implementation overlay.
4. This repository's `CLAUDE.md` and the target UI/store modules.

The upstream snapshot is recorded in `docs/skills/eui-neo-ui-replicator/UPSTREAM.toml`. It targets the EUI-NEO v0.5.7 input-event model; validate every API used against the installed `0.5.7` headers before changing UI code.

## Project Adaptations

- Keep apitab's `src/` C++23 module layout, the CMake build, and `./run.sh`; do not apply upstream `apps/<target>` structure literally. The vendored upstream tree is built as a plain `eui_neo` static lib (no tray/markdown, bundled 3rd-party) — see `third_party/CMakeLists.txt`.
- Keep composition event-driven: no ordinary `.onFrame` work and no decorative infinite animation.
- Use the project-local compact `eui_ui.h` include convention in UI modules.
- This is a dense operational API tool: avoid decorative remote media and retain direct, information-focused surfaces.
- Element IDs are globally unique after page/loader resolution. Use stable caller prefixes and entity keys; see the global EUI overlay for collision diagnostics.

## Pending Upstream Features（升级 EUI 时逐项复查）

- **最小窗口尺寸**：0.5.7 的 `DslAppConfig` 无 min-size 接口；Linux X11/XWayland 下 `src/app.cpp::applyMinWindowSize` 用 X11 `WM_NORMAL_HINTS`（dlopen libX11 + dlsym，无新链接依赖）实现最小宽度 700 逻辑像素。Windows/macOS 使用 no-op fallback，由初始窗口尺寸和页面自适应布局处理。升级 EUI 时先查新版 `dsl_app.h` 是否已有 min-size API，有则删除该 workaround。
  - 升级检查：`grep -n 'min.*[Ss]ize\|minWindow' <新版>/include/eui/dsl_app.h`，若 `DslAppConfig` 出现 minWindowSize/minimumSize 类接口，改用它并删除 app.cpp 的整块 X11 workaround（`X11Fns` / `findWindowByTitle` / `applyMinWindowSize` / `kMinWindowWidthLogical` + compose 里的调用）。
