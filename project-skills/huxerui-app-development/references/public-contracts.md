# Public application contracts

Adapted from the existing HuxerUI app-development skill snapshot.
For apitab, source/SDK selection follows the project dependency skill.


- Components are ordinary C++ functions returning `View`.
- `View` is a lightweight copy-on-write declaration value. Pass and return it by value; do not add `std::move` to temporary chains, container children, or return statements as a routine optimization.
- Mark a reusable function `[[huxerui::composable]]` when it directly calls a composition-bound `UseXxx()` API or needs its own recomposition lifetime. Do not annotate the application root.
- Keep controlled values authoritative in app state and write emitted changes back for the next composition.
- Use stable semantic keys for dynamic stateful siblings that insert, remove, or reorder.
- Preserve complete `TextEditingValue` state for `TextField`.
- Use only public headers, public CMake targets, generated project conventions, and platform-specific public headers.
- Prefer generated `ImageResource` values for static artwork displayed by HuxerUI components. Keep scalable in-UI artwork as SVG under `resources/images`; use `VectorAsset::Create` or `Canvas` only for genuinely programmatic or dynamic drawing. Platform application and system-tray icons follow their separate packaging contracts.
- Compose built-in layouts and their modifiers before deriving `Layout` or `VirtualLayout`. A custom layout is for a genuinely new measurement or placement algorithm, not ordinary alignment, spacing, growth, wrapping, overlay, paging, scrolling, or responsiveness.
- Start application visuals from `FlatTheme`, `FlatDarkTheme`, `MaterialTheme`, or `MaterialDarkTheme`. Customize their tokens or typed styles instead of recreating a built-in theme with a bare `ThemeDefinition`.
- Choose the narrowest extension mechanism after exhausting its built-in equivalent. Prefer an ordinary component, existing event or modifier, Environment/Theme value, presentation service, or root service. Use a custom `Layout`, retained modifier with `NodeExtension`, `PlatformModule`, `PlatformView`, or `ExternalTexture` only when its distinct geometry, mounted lifetime, or platform boundary is actually required.
- Do not invent APIs, legacy names, private includes, or source-checkout paths.

## Verify affected behavior


Inspect the project's existing CMake target, entry point, theme, state ownership, component patterns, and platform configuration before editing. Preserve its generated structure and current generator/compiler. Do not add platforms, compatibility layers, or a new architectural abstraction unless requested.

After editing, check the items applicable to the current change:

- layout under bounded, unbounded, narrow, and expanded constraints relevant to the task;
- controlled values, stable keys, focus, keyboard, hover, pressed, disabled, and semantics;
- reduced-motion behavior for animation;
- platform-specific public contracts and unavailable-platform limits;
- HuxerUI DSL formatting from `dsl-style.md`;
- build and run results only for the current host and affected platforms that are actually available.

When an API is uncertain, inspect the active SDK header, CMake package, or CLI help instead of guessing.
