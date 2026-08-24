# EUI-NEO Compatibility

apitab uses EUI-NEO `0.5.7` through `compat.eui-neo` in `mcpp.toml`.

## Required Reading Order

1. `docs/skills/eui-neo-ui-replicator/SKILL.md` — pinned official upstream guidance.
2. The installed EUI-NEO `0.5.7` DSL, component, and workshop documentation.
3. `/home/farna/.claude/skills/eui-development/SKILL.md` — apitab/tinynext implementation overlay.
4. This repository's `CLAUDE.md` and the target UI/store modules.

The upstream snapshot is recorded in `docs/skills/eui-neo-ui-replicator/UPSTREAM.toml`. It targets the EUI-NEO v0.5.7 input-event model; validate every API used against the installed `0.5.7` headers before changing UI code.

## Project Adaptations

- Keep apitab's `src/` C++23 module layout, `mcpp build`, and `./run.sh`; do not apply upstream CMake or `apps/<target>` structure literally.
- Keep composition event-driven: no ordinary `.onFrame` work and no decorative infinite animation.
- Use the project-local compact `eui_ui.h` include convention in UI modules.
- This is a dense operational API tool: avoid decorative remote media and retain direct, information-focused surfaces.
- Element IDs are globally unique after page/loader resolution. Use stable caller prefixes and entity keys; see the global EUI overlay for collision diagnostics.
