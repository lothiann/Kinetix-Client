# AGENTS.md

## About the project

**Kinetix** is a feature-rich client-side fork of **DDNet** (a DDRace mod for Teeworlds). It adds an in-game overlay (ClickGUI), a multi-dummy toolkit, movement bots, pathfinding, TAS tooling, Lua scripting and much more. Everything runs on the client; no server-side support is required.

- **Current upstream base:** DDNet **20.2**
- **Kinetix code:** all Kinetix-specific logic lives in `src/game/client/components/kinetix`
- **Current release line:** `KINETIX_VERSION` (`src/game/client/components/kinetix/clickgui.cpp`)

## Versioning

Commit versions use the scheme `v{KINETIX_VERSION}.{NUMBER}`, for example `v6.0`, `v6.1`, `v7.15`, `v7.100`.

- `KINETIX_VERSION` is the release version taken directly from the `KINETIX_VERSION` macro. Do not duplicate it elsewhere.
- `NUMBER` is the subversion and must be **unique among current commits**. Deleted/reverted commits are not counted, and neither are commits on other branches.
- `NUMBER` is always the previous commit's number **+ 1**. The only exception is a change of `KINETIX_VERSION`, in which case numbering starts from `0`.
- Never put `beta`/`alpha`/etc. in the first number (the release version).

## Updating the client to a new DDNet version

When porting Kinetix to a newer DDNet release, do **not** diff the current Kinetix tree against the old upstream tag — that shows Kinetix's own features as if they were upstream changes. Diff the **old upstream tag against the new upstream tag** and apply only those changes on top of the Kinetix tree.

1. Fetch the new tag/commit (`git fetch upstream <tag>`), then list the upstream changes (compares trees only, works offline): `git diff --name-status --no-renames <old-tag> <new-tag>`.
2. For every changed path compare the Kinetix working-tree file with the old tag's file:
   - identical → Kinetix never touched it: copy the new tag's version verbatim (or delete the file if upstream removed it);
   - different → three-way merge with base = old tag file, ours = Kinetix file, theirs = new tag file.
3. Kinetix-only paths (`src/game/client/components/kinetix/**`, vendored `src/engine/external/{lua,sol2,yoga,wavpack}/**`, `data/scripts/**`, `ddnet-libs/**`, …) never appear in the upstream diff and must stay untouched.
4. The tree mixes tab- and space-indented files. Normalize leading indentation (expand tabs to 8 spaces in ours/base/theirs) before merging, otherwise every reindented line becomes a bogus conflict. Resolve real conflicts by hand, keeping Kinetix functionality and the file's local style.
5. Propagate upstream API renames to Kinetix code (grep for the old names). Examples from 20.2: `ELoadingStateDetail` / `ELoadingCallbackDetail` / `EConnectivity` became `enum class`; `CGameClient::m_Tooltips` moved to `CUi::DoToolTip` / `CUi::Tooltips()`; `GetRenderTeeOffsetToRenderedTee` usages were replaced by `RenderIdleTeeAtCenter`. This fork has **no** `NUM_DUMMIES` — replace it with `MAX_DUMMIES` in any upstream code that uses it.
6. `src/game/version.h`: set `DDNET_VERSION_NUMBER` and `GAME_RELEASE_VERSION_INTERNAL` to the **release** version (e.g. `20020` / `20.2`). The new tag often already contains the post-release dev bump (the 20.2 tag says 20.3) — do not copy that.
7. Check `grep -rn '^<<<<<<< ' src/` for leftover conflict markers, update the "Current upstream base" line above, report the changed files, then wait for an explicit build request.

## Rules

- **Commits** — only on an explicit request from the user. Never commit on your own after finishing a task: make the changes, report, and wait.
- **Commit messages** — short, one line, in the form `v6.0: short description`. Do not write huge lists of every change.
- **Commit language** — every commit message must be written in English only.
- **Builds** — do not build by default: after changes, report that everything is ready for building and verification. If the user explicitly says "compile"/"build"/"rebuild", then build the Windows/VS target.
- **Android builds** — build Android only on an explicit request, e.g. **«Скомпилируй на Android»** or a close equivalent. A plain "compile"/"build" means the Windows build.
- **Field order in header files** — if you changed the order of fields in a `.h` file, an incremental rebuild is not enough: run a full rebuild (Rebuild All). Adding, removing or reordering `#include` directives in a header is **not** a field-order change and does not require a full rebuild. This rule is for the Windows/VS build only — the Android toolchain tracks header dependencies itself and never needs a clean/full rebuild for this.

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

`rebuild` is required when the order of fields in `.h` files changes (Windows/VS build only — see the Android note below).

### Android

The Android build uses `scripts/android/cmake_android.sh` and does **not** use the Visual Studio / Ninja setup above. The APK is signed with the debug keystore and written to `build-android-release`.

By default build **both architectures** — two chained invocations. The two ABIs end up merged into a **single `.apk`** (not two separate APKs):

```bash
TW_KEY_NAME="C:/Users/admin/.android/debug.keystore" bash scripts/android/cmake_android.sh arm64 "Kinetix" "org.kinetix.client" Release build-android-release && \
TW_KEY_NAME="C:/Users/admin/.android/debug.keystore" bash scripts/android/cmake_android.sh arm "Kinetix" "org.kinetix.client" Release build-android-release
```

- The Android toolchain tracks `.h` dependencies itself: a changed header (including added/removed/reordered fields) does **not** require a clean/full rebuild — just run the Android build again. The Windows "field order ⇒ Rebuild All" rule does not apply here.
- Build Android **only on an explicit request**: the user has to say something like **«Скомпилируй на Android»** (a close equivalent is fine). A generic "compile"/"build" means the Windows build.