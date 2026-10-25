# AGENTS.md

## About the project

**Kinetix** is a feature-rich client-side fork of **DDNet** (a DDRace mod for Teeworlds). It adds an in-game overlay (ClickGUI), a multi-dummy toolkit, movement bots, pathfinding, TAS tooling, Lua scripting and much more. Everything runs on the client; no server-side support is required.

- **Current upstream base:** DDNet **20.1.1**
- **Kinetix code:** all Kinetix-specific logic lives in `src/game/client/components/kinetix`
- **Current release line:** `KINETIX_VERSION` (`src/game/client/components/kinetix/clickgui.cpp`)

## Versioning

Commit versions use the scheme `v{KINETIX_VERSION}.{NUMBER}`, for example `v6.0`, `v6.1`, `v7.15`, `v7.100`.

- `KINETIX_VERSION` is the release version taken directly from the `KINETIX_VERSION` macro. Do not duplicate it elsewhere.
- `NUMBER` is the subversion and must be **unique among current commits**. Deleted/reverted commits are not counted, and neither are commits on other branches.
- `NUMBER` is always the previous commit's number **+ 1**. The only exception is a change of `KINETIX_VERSION`, in which case numbering starts from `0`.
- Never put `beta`/`alpha`/etc. in the first number (the release version).

## Rules

- **Commits** — only on an explicit request from the user. Never commit on your own after finishing a task: make the changes, report, and wait.
- **Commit messages** — short, one line, in the form `v6.0: short description`. Do not write huge lists of every change.
- **Commit language** — every commit message must be written in English only.
- **Builds** — do not build by default: after changes, report that everything is ready for building and verification. If the user explicitly says "compile"/"build"/"rebuild", then build.
- **Field order in header files** — if you changed the order of fields in a `.h` file, an incremental rebuild is not enough: run a full rebuild (Rebuild All).

## Build

The VS configuration is **x64-Release** (Ninja generator, `CMAKE_BUILD_TYPE=RelWithDebInfo`), build root `out/build/x64-Release`.

**Build only inside the Visual Studio environment** — otherwise `ninja` fails with `LNK1104: cannot open file "shlwapi.lib"` (empty `LIB`/`INCLUDE` outside the VS environment). Before building:

```
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
```

`ninja.exe` from Visual Studio:
`C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`

### Build All

```bash
NINJA="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
"$NINJA" -C "out/build/x64-Release"
```

### Rebuild All

```bash
"$NINJA" -C "out/build/x64-Release" -t clean
"$NINJA" -C "out/build/x64-Release"
```

### Ready-made batch script

The repository contains `scripts/build.bat`: it calls `vcvars64.bat` itself (found via `vswhere`, then the standard VS 18/2022 paths) and locates `ninja.exe` itself. It can be run from a normal cmd/PowerShell; the VS environment is set up inside.

```
scripts/build.bat            incremental build (default)
scripts/build.bat build      same (-b, --build)
scripts/build.bat rebuild    clean + full rebuild (-r, --rebuild)
scripts/build.bat clean      clean only (-c, --clean)
```

`rebuild` is required when the order of fields in `.h` files changes.