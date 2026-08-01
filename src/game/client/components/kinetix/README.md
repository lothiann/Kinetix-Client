# Kinetix components — folder structure rules

This folder holds all Kinetix-specific client components. To keep the tree
flat and navigable, follow these rules when adding new code.

## Rule: when to use a subfolder

A component gets its own subfolder **only** when it has **2 or more files of
the same type** (`.cpp` or `.h`). Otherwise the files live directly in this
folder.

| Component files                         | Location                              |
|-----------------------------------------|---------------------------------------|
| `foo.cpp` + `foo.h` (1 + 1)             | `kinetix/foo.cpp`, `kinetix/foo.h`    |
| `foo.cpp` only (1 + 0)                  | `kinetix/foo.cpp`                     |
| `foo.cpp` + `foo_extra.cpp` (2 + 0)     | `kinetix/foo/foo.cpp`, `foo_extra.cpp`|
| `foo.cpp` + `foo.h` + `foo_internal.h` (1 + 2) | `kinetix/foo/foo.cpp`, `foo.h`, `foo_internal.h` |
| `foo.cpp` + `foo_render.cpp` + `foo.h` (2 + 1) | `kinetix/foo/...`                   |

In short:
- **1 `.cpp` AND 1 `.h`** (or fewer of either) → **no subfolder**, files at the
  root of `kinetix/`.
- **2+ `.cpp` OR 2+ `.h`** → **subfolder** named after the component.

## Adding a new component

1. Pick the location using the rule above.
2. Register the source file(s) in `CMakeLists.txt` (search for the
   `# Kinetix sources` block near line 2570).
3. If the component exposes a class, include its header from
   `src/game/client/gameclient.h` and add a member to `CGameClient`.
4. Register the component instance in `CGameClient::OnInit()` (see how
   `m_AimBot`, `m_BotControl`, etc. are registered).

## Naming

- Component class: `CName` (e.g. `CAimBot`, `CEsp`).
- Files: lowercase, match the class without the `C` prefix
  (`aimbot.cpp`/`aimbot.h`, `esp.cpp`/`esp.h`).
- Console config vars: `KxName` → `kx_name` (`MACRO_CONFIG_INT(KxEsp, ...)`).

## Shared helpers

Cross-component helpers live in `helpers.cpp` (no header — declared in
`kinetix_internal.h`). Add new shared utilities there rather than duplicating.
