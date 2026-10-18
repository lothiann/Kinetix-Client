# Code Execution (Lua)

Kinetix embeds a full **Lua 5.4** interpreter (bound through [sol2](https://github.com/ThePhD/sol2)). It is exposed by:

- the **Code Execution (Lua)** ClickGUI panel (built-in multiline editor with syntax highlighting, Run/Stop and a file picker),
- the console command `kx_exec "s[code]"`,
- the console command `kx_exec_stop`.

Scripts are not sandboxed: they receive **direct C++ access** to `CGameClient` and nearly every DDNet class, plus filesystem/subprocess helpers. Only run code you trust.

---

## Table of Contents

- [Running scripts](#running-scripts)
- [Execution model](#execution-model)
- [Globals](#globals)
  - [print](#print)
  - [vec2](#vec2)
  - [CONST](#const)
  - [gc — CGameClient](#gc--cgameclient)
  - [cl — IClient](#cl--iclient)
  - [config — CConfig](#config--cconfig)
  - [console](#console)
  - [net](#net)
  - [io / os](#io--os)
  - [gfx](#gfx)
  - [sound](#sound)
  - [input](#input)
  - [text](#text)
- [Exposed classes (usertypes)](#exposed-classes-usertypes)
  - [Game state](#game-state)
  - [Core / physics](#core--physics)
  - [Collision & map](#collision--map)
  - [Prediction entities](#prediction-entities)
  - [Components](#components)
  - [Kinetix components](#kinetix-components)
- [Examples](#examples)
- [Notes & limitations](#notes--limitations)

---

## Running scripts

From the console:

```
kx_exec "print('hello from lua')"
kx_exec_stop
```

From Lua, scripts can also run other code via `console.execute(...)`, load files with `console.execute_file(path)`, run helper processes with `io.popen(...)`, or execute more Lua by calling `gc.m_CodeExec:Execute(code)`.

The built-in editor buffer is `65536` bytes (`gc.m_CodeExec.m_aCodeBuffer`).

---

## Execution model

- Libraries opened: `base`, `string`, `math`, `table`, `coroutine`, `utf8`, `package`, `os`, `io`, `debug`.
- A script runs as a **coroutine**. Every `200000` Lua instructions it automatically `yield`s back to the client, so a long-running loop does not freeze the game indefinitely. It resumes on the next frames.
- `Stop()` (or `kx_exec_stop`) requests an abort; the instruction hook then raises `script stopped`, unwinds the coroutine, and the state returns to `STATE_IDLE`.
- State machine: `STATE_IDLE = 0`, `STATE_RUNNING = 1`, `STATE_ERROR = 2`.
- Runtime errors are printed to the in-game chat (as a `LUA_MSG` line) and also kept in `gc.m_CodeExec:GetLastError()`.
- A UTF-8 BOM or a stray leading continuation byte in the editor buffer is skipped before loading.

Because of the cooperative yielding, write polling loops like:

```lua
while true do
    -- do work
    -- the engine yields automatically every 200k instructions
end
```

To pace work by wall-clock time, use `os.time()` / `os.clock()` from the standard library.

---

## Globals

### print

```lua
print(...)
```

Concatenates all arguments (tab-separated, using Lua's `tostring`) and writes them to the **in-game chat**.

### vec2

```lua
local v = vec2(x, y)      -- or vec2() for (0, 0)
local u = vec2(1, 0)
```

| Member | Description |
|--------|-------------|
| `v.x`, `v.y` | Components (read/write). |
| `v.length()` | Vector length. |
| `v.normalize()` | Normalized vector. |
| `v.dot(w)` | Dot product. |
| `v.distance(w)` | Distance between two vectors. |
| `v.angle()` | Angle of the vector (radians). |
| Operators | `+`, `-`, `*` (vector and scalar), `/` (scalar), unary `-`. |

### CONST

Engine constants.

| Group | Names |
|-------|-------|
| Ticks / sizes | `SERVER_TICK_SPEED`, `MAX_CLIENTS`, `NUM_WEAPONS`, `MAX_DUMMIES` |
| Hook states | `HOOK_RETRACTED`, `HOOK_IDLE`, `HOOK_RETRACT_START`, `HOOK_RETRACT_END`, `HOOK_FLYING`, `HOOK_GRABBED` |
| Core events | `COREEVENT_GROUND_JUMP`, `COREEVENT_AIR_JUMP`, `COREEVENT_HOOK_LAUNCH`, `COREEVENT_HOOK_ATTACH_PLAYER`, `COREEVENT_HOOK_ATTACH_GROUND`, `COREEVENT_HOOK_HIT_NOHOOK`, `COREEVENT_HOOK_RETRACT` |
| Teams | `TEAM_SPECTATORS`, `TEAM_RED`, `TEAM_BLUE` |
| Weapons | `WEAPON_HAMMER`, `WEAPON_GUN`, `WEAPON_SHOTGUN`, `WEAPON_GRENADE`, `WEAPON_LASER`, `WEAPON_NINJA` |
| Spectate | `SPEC_FREEVIEW`, `SPEC_FOLLOW` |
| Flags | `FLAG_MISSING`, `FLAG_ATSTAND`, `FLAG_TAKEN` |
| "Show others" | `SHOW_OTHERS_NOT_SET`, `SHOW_OTHERS_OFF`, `SHOW_OTHERS_ON`, `SHOW_OTHERS_ONLY_TEAM` |

### gc — CGameClient

`gc` is a reference to the live `CGameClient` (the root of all client state). Below are the most useful members; the full list is the same set registered in `code_exec_bindings.cpp`.

**Players & state**

| Member | Description |
|--------|-------------|
| `gc.m_aClients[i]` | `CClientData` for client `i` (see [CClientData](#cclientdata)). |
| `gc.m_aStats[i]` | `CClientStats` for client `i`. |
| `gc.m_aLocalIds[d]` | Server id of local player / dummy `d`. |
| `gc.m_Snap` | `CSnapState` — raw snapshot data. |
| `gc.m_aTuning[d]` | `CTuningParams` per dummy. |
| `gc.m_GameInfo` | `CGameInfo` flags. |
| `gc.m_Teams` | `CTeamsCore`. |
| `gc.m_GameWorld`, `gc.m_PredictedWorld`, `gc.m_PrevPredictedWorld` | `CGameWorld` instances. |
| `gc.m_PredictedChar`, `gc.m_PredictedPrevChar` | Local `CCharacterCore` (cur/prev). |
| `gc.m_LocalCharacterPos` | Local position (`vec2`). |
| `gc.m_aDummyInput[d]`, `gc.m_aHammerInput[d]` | Per-dummy input objects. |
| `gc.m_aDummyFire[d]` | Per-dummy fire flags. |
| `gc.m_ServerMode`, `gc.m_NewTick`, `gc.m_NewPredictedTick` | Misc state. |

**Methods**

`gc:SendKill()`, `gc:SendSwitchTeam(team)`, `gc:Echo(msg)`, `gc:IsTeamPlay()`, `gc:IsWorldPaused()`, `gc:IsLocalCharSuper()`, `gc:CurrentRaceTime()`, `gc:LastRaceTick()`, `gc:AntiPing*()`, `gc:IsOtherTeam(id)`, `gc:SwitchStateTeam()`, `gc:IntersectCharacter(hookPos, newPos, ownId)` → `{ClientId, NewPos2, PlayerPosition}`, `gc:DummyResetInput()`, `gc:Version()`, `gc:DDNetVersion()`, `gc:Collision()`, `gc:Switchers()`, `gc:PredSwitchers()`, `gc:GetTuning(i)`, `gc:Layers()`, `gc:Config()`, `gc:GetItemName(type)`, `gc:Predict()`, `gc:PredictDummy()`, `gc:LoadGameSkin(path)`, `gc:ResetMultiView()`, and more.

### cl — IClient

Low-level client control (namespace table).

| Function | Description |
|----------|-------------|
| `cl.Connect(addr)` / `cl.Disconnect()` | Connect / disconnect. |
| `cl.EnterGame()` | Enter the game from the menu. |
| `cl.RconAuth(user, pass)` / `cl.Rcon(cmd)` / `cl.RconAuthed()` | RCON. |
| `cl.DummyConnect(d)` / `cl.DummyDisconnect(d)` / `cl.DummyConnected(d)` / `cl.DummyCount()` / `cl.DummyName()` | Dummy management. |
| `cl.AnyDummyConnected()`, `cl.LastDummy()`, `cl.DummyAllowed()` | Dummy queries. |
| `cl.GameTick()`, `cl.PrevGameTick()`, `cl.PredGameTick()`, `cl.GameTickSpeed()`, `cl.IntraGameTick()`, `cl.GameTickTime()` | Ticks. |
| `cl.LocalTime()`, `cl.GlobalTime()`, `cl.RenderFrameTime()`, `cl.PredictionTime()` | Timing. |
| `cl.PlayerName()`, `cl.Points()`, `cl.News()`, `cl.ErrorString()` | Identity / info. |
| `cl.ConnectionProblems()`, `cl.State()`, `cl.IsSixup()` | Status. |
| `cl.MapDownloadName()`, `cl.MapDownloadAmount()`, `cl.MapDownloadTotalsize()` | Map download. |
| `cl.ConnectAddress()` | Address string of the current connection. |
| `cl.Notify(title, msg)`, `cl.AutoScreenshot()` | UI / screenshot. |
| `cl.Quit()`, `cl.Restart()` | Lifecycle. |

### config — CConfig

`config` is a reference to the global `g_Config`.

| Member | Description |
|--------|-------------|
| `config.m_ClDummy`, `config.m_ClDummyControl`, `config.m_ClDummyCopyMoves`, `config.m_ClDummyHook`, `config.m_ClDummyJump`, `config.m_ClDummyFire`, `config.m_ClDummyResetOnSwitch`, `config.m_ClDummyRestoreWeapon` | Core dummy config. |
| `config.m_KxHelpers`, `config.m_KxCopyMoves`, `config.m_KxFlyRide`, `config.m_KxDummyAim`, `config.m_KxDummyDirection`, `config.m_KxDummyCopyMovesFilter` | A few Kinetix fields exposed directly. |
| `config.get(name)` | Reads **any** config variable by its script name (e.g. `kx_helpers`); returns the value or `nil`. |
| `config.set(name, value)` | Writes **any** config variable by script name; returns `true` on success. |

> `config.get`/`config.set` handle int, color and string variables dynamically through the config registry. The full list of available names is in [CVars](../README.md#cvars).

### console

| Function | Description |
|----------|-------------|
| `console.execute(line)` | Execute a console command line. |
| `console.print(str)` | Print a line to the console (`lua` output level). |
| `console.execute_file(path)` | Execute a console config file; returns success. |

### net

Asynchronous HTTP on the engine's libcurl thread (non-blocking).

```lua
local job = net.get(url [, timeout_sec])
local job = net.post(url, body [, timeout_sec])
local job = net.post_json(url, json [, headers_table] [, timeout_sec])

if job:done() then
    print(job:status(), job:code(), job:result())
end
job:abort()
```

`NetJob` methods: `done()`, `status()` (queued/running/done/aborted/error), `result()`, `code()`, `abort()`. Default timeout 30 s; max response 16 MiB. Errors do not fail the job automatically (`FailOnErrorStatus(false)`), so always check `code()`/`result()`.

### io / os

Both are replacements that run commands **without flashing a console window** (the stock `_popen`/`_system` would spawn `cmd.exe` visibly and block rendering).

```lua
-- non-blocking, runs on a background thread
local job = io.popen("dir C:\\")
if job:done() then print(job:output(), job:exitcode()) end
-- blocking helpers:
job:read("*a")   -- waits until done, returns full output
job:close()      -- waits until done, returns exit code

-- synchronous, no window; returns ok, "exit", code
local ok, what, code = os.execute("whoami")
```

| `io.popen` job | Description |
|----------------|-------------|
| `job:done()` | Finished? |
| `job:output()` | Captured stdout+stderr. |
| `job:exitcode()` | Process exit code (-1 if it failed to start). |
| `job:read([fmt])` | Blocking; returns full output. |
| `job:close()` | Blocking; returns exit code. |

### gfx

| Function | Description |
|----------|-------------|
| `gfx.width()`, `gfx.height()`, `gfx.aspect()` | Screen size. |
| `gfx.window_width()`, `gfx.window_height()` | Window size. |
| `gfx.map_screen(x0, y0, x1, y1)` | Set the screen mapping. |
| `gfx.map_screen_to_world(cx, cy [, zoom])` | Map a zoomed view; returns `{x0,y0,x1,y1}`. |
| `gfx.map_screen_to_interface(cx, cy [, zoom])` | Interface mapping. |
| `gfx.draw_rect(x, y, w, h, r, g, b, a [, corners] [, rounding])` | Filled rect. |
| `gfx.draw_circle(cx, cy, r [, segments])` | Filled circle. |
| `gfx.draw_line(x0, y0, x1, y1, r, g, b, a)` | Line. |
| `gfx.set_color(r, g, b, a)` | Set draw colour. |
| `gfx.blend_normal()`, `gfx.blend_additive()` | Blend modes. |
| `gfx.texture_clear()` | Clear texture binding. |

### sound

Channel constants: `sound.CHN_GUI`, `sound.CHN_MUSIC`, `sound.CHN_WORLD`, `sound.CHN_GLOBAL`.

| Function | Description |
|----------|-------------|
| `sound.play(sampleId, vol)`, `sound.play_at(sampleId, vol, x, y)` | Play a sample (global / at a world position). |
| `sound.stop(sampleId)`, `sound.stop_all()` | Stop sample(s). |
| `sound.enabled()` | Sound enabled? |
| `sound.load(name)` | Load `.opus` (else `.wv`); returns sample id. |
| `sound.unload_sample(id)`, `sound.pause(id)` | Manage samples. |
| `sound.sample_time(id)`, `sound.sample_total_time(id)` | Playback timing. |

### input

Key constants: `input.KEY_W/A/S/D`, `KEY_SPACE`, `KEY_RETURN`, `KEY_ESCAPE`, `KEY_TAB`, `KEY_LSHIFT`, `KEY_RSHIFT`, `KEY_LCTRL`, `KEY_RCTRL`, `KEY_UP/DOWN/LEFT/RIGHT`, `KEY_DELETE`, `KEY_BACKSPACE`, `KEY_INSERT`, `KEY_HOME`, `KEY_END`, `KEY_PAGEUP`, `KEY_PAGEDOWN`, `KEY_F1..KEY_F12`, `KEY_MOUSE_1/2/3`, `KEY_MOUSE_WHEEL_UP/DOWN`.

| Function | Description |
|----------|-------------|
| `input.pressed(key)` | Key currently held. |
| `input.pressed_this_frame(key)` | Key pressed this frame. |
| `input.key_name(key)` / `input.find_key(name)` | Key ↔ name. |
| `input.shift_pressed()`, `input.alt_pressed()`, `input.modifier_pressed()` | Modifiers. |
| `input.mouse_pressed([index])` | Native mouse button state. |
| `input.mouse_pos()` | Native mouse position (`vec2`). |
| `input.clipboard_get()` / `input.clipboard_set(text)` | Clipboard. |

### text

| Function | Description |
|----------|-------------|
| `text.draw(x, y, size, str)` | Draw text. |
| `text.width(size, str)` | Measure text width. |
| `text.bounding_box(size, str)` | Returns `{width, height}`. |
| `text.color(r, g, b, a)` | Text colour. |
| `text.outline_color(r, g, b, a)` | Text outline colour. |

---

## Exposed classes (usertypes)

All of these can be reached from `gc`/`cl` and manipulated directly.

### Game state

| Type | Reached via | Notes |
|------|-------------|-------|
| `CGameClient` | `gc` | Root. |
| `CClientData` | `gc.m_aClients[i]` | `m_aName`, `m_aClan`, `m_Country`, `m_aSkinName`, `m_Team`, `m_Active`, `m_AuthLevel`, `m_ColorBody/Feet`, `m_RenderPos`, `m_Predicted`, `m_PrevPredicted`, `m_aSwitchStates`, `m_DeepFrozen`, `m_LiveFrozen`, `ClientId()`, `UpdateRenderInfo()`, … |
| `CClientStats` | `gc.m_aStats[i]` | Frags/deaths/sprees, flags, `m_aFragsWith`, `m_aDeathsFrom`, `GetFPM()`. |
| `CSnapState` | `gc.m_Snap` | `m_LocalClientId`, `m_NumPlayers`, `m_aCharacters`, `m_apPlayerInfos`, `m_SpecInfo`, `m_pGameInfoObj`, `m_pGameDataObj`, `m_aTeamSize`. |
| `CGameInfo` | `gc.m_GameInfo` | Game mode flags (`m_Race`, `m_Pvp`, `m_DDRaceTeam`, prediction flags, HUD flags…). |
| `CNetObj_*` | snap / character data | `PlayerInput`, `Character`, `PlayerInfo`, `GameInfo`, `GameData`, `Flag`, `SpectatorInfo`, `DDNetCharacter`. |
| `SSwitchers` | `gc:Switchers()` | `m_Initial`, and arrays `m_aStatus`, `m_aEndTick`, `m_aType`, `m_aLastUpdateTick` (Lua tables). |
| `CTeamsCore` | `gc.m_Teams` | `Team(id)`, `SetSolo`, `CanCollide`, `SameTeam`, … |

### Core / physics

| Type | Notes |
|------|-------|
| `CCharacterCore` | `m_Pos`, `m_Vel`, `m_HookPos`, `m_HookDir`, `m_HookedPlayer`, `m_ActiveWeapon`, `m_Jumps`, `m_Input`, `m_Tuning`, `m_aWeapons`, freeze/jetpack/tele flags, `Tick()`, `Move()`, `Quantize()`, `Clone()`, `Init()`, … |
| `CTuningParams` | All tuning fields (`m_Gravity`, `m_HookLength`, `m_GunSpeed`, …), `Set()`, `Get()`, `Name(i)`, `Num()`, `GetWeaponFireDelay(w)`. |
| `CWorldCore` | `m_apCharacters`, `m_vSwitchers`, `RandomOr0`, `InitSwitchers`. |
| `CGameWorld` | `m_Core`, `m_Teams`, `m_GameTick`, `m_WorldConfig`, `FindFirst(type)`, `GetEntity(id,type)`, `GetCharacterById(id)`, `FindEntities(pos,radius,type,max)`, `Tick()`, `GlobalTuning()`, `Collision()`, `Teams()`, `Switchers()`. |

### Collision & map

| Type | Notes |
|------|-------|
| `CCollision` | `CheckPoint`, `GetCollisionAt`, `IntersectLine*`, `MovePoint`, `MoveBox`, `TestBox`, `IsOnGround`, `GetTile`, `GetTileFlags`, `IsSolid`, `IsTeleport`, `IsSpeedup`, `GetSpeedup`, `TeleOuts/Ins/CheckOuts/Others`, `SetCollisionAt`, `Layers`, … |
| `CLayers` | `NumGroups`, `NumLayers`, `GameGroup`, `GameLayer`, `GetGroup/GetLayer`, `TeleLayer`, `SpeedupLayer`, `FrontLayer`, `SwitchLayer`, `TuneLayer`. |
| `CTile` | `m_Index`, `m_Flags`, `m_Skip`. |
| `CMapItemLayer`, `CMapItemGroup`, `CMapItemLayerTilemap` | Layer/group descriptors. |

### Prediction entities

| Type | Base | Notes |
|------|------|-------|
| `CEntity` | — | `GetId`, `GetPos`, `m_Pos`, `m_ProximityRadius`, `GameWorld`, `Collision`, `GetTuning`, `TypeNext`, `NextEntity`. |
| `CCharacter` | `CEntity` | `Core()`. |
| `CProjectile` | `CEntity` | `GetDirection`, `GetOwner`, `GetStartTick`, `GetType`. |
| `CLaser` | `CEntity` | `GetOwner`. |
| `CPickup` | `CEntity` | `Type`. |

### Components

Exposed via `gc.m_<Component>`: `CCamera`, `CChat`, `CControls`, `CEffects`, `CFlow`, `CVoting`, `CSpectator`, `CEmoticon`, `CMotd`, `CBroadcast`, `CGhost`, `CParticles`, `CDamageInd`, `CSounds`, `CMenus`, `CBinds`, `CGameConsole`, `CHud`, `CRaceDemo`, `CLocalServer`, `CImportantAlert`, `CScoreboard`, `CStatboard`, `CSkins`, `CConfig`.

Highlights:

- **`CChat`** — `GetRecentLines(n)` (returns `{time, clientid, team, name, text}` entries), `SendChat(team, msg)`, `SendChatQueued(msg)`, `SendChatFrom(dummy, team, msg)`, `AddLine(cid, team, msg)`, `Echo(msg)`, `RegisterCommand(name, params, help)`.
- **`CControls`** — `m_aInputData[d]`, `m_aLastData[d]`, `m_aMousePos[d]`, `m_aTargetPos[d]`, `m_aInputDirectionLeft/Right[d]`, `ResetInput(d)`.
- **`CBotControl`** — `ActionInput(action, ms, d)`, `ActionAim(dx, dy, d)`, `ActionOverrideAim(x, y, d)`, `ActionStop(d)`, and `IsBotActive*` queries. Mirrors `kx_input`/`kx_aim`/`kx_oaim`/`kx_stop`.
- **`CSounds`** — `Play`, `PlayAt`, `Stop`, `IsPlaying`, `Enqueue`, `PlayAndRecord`.
- **`CBinds`** — `Bind(key, str, free, mod)`, `Get(key, mod)`, `SetDefaults()`, `UnbindAll()`.
- **`CSkins`** — `IsFavorite`, `AddFavorite`, `RemoveFavorite`, `RandomizeSkin(dummy)`, `SkinPrefix`, `IsSpecialSkin`.

### Kinetix components

| Member | Type | Notes |
|--------|------|-------|
| `gc.m_BotNet` | `CBotNet` | `IsDummyActive(d)`, `ResetDummyInputs(d)`, `IsTileWalkable(tx,ty)`, `IsTileFreeze(tx,ty)`, `ProcessDummy(d)`, `m_MapWidth/Height`, `m_MapGridLoaded`. |
| `gc.m_AimBot` | `CAimBot` | Aim/trigger state. |
| `gc.m_Esp` | `CEsp` | ESP state. |
| `gc.m_Trajectory` | `CTrajectory` | Trajectory state. |
| `gc.m_AvoidFreeze` | `CAvoidFreeze` | Avoid-freeze state. |
| `gc.m_KinetixLines` | `CKinetixLines` | Deferred line overlay. |
| `gc.m_ClickGui` | `CClickGui` | The overlay UI. |
| `gc.m_IRC` | `CIRC` | `SendIRC(msg)`, `GetClientPrefix(id)`. |
| `gc.m_CodeExec` | `CCodeExec` | The scripting engine itself. |

> **`CCodeExec`** members: constants `STATE_IDLE`, `STATE_RUNNING`, `STATE_ERROR`; methods `Execute(code [, chunk])` → bool, `Stop()`, `GetState()`, `GetLastError()`, `GetLineCount()`, `SetCodeBuffer(code)`; fields `m_aCodeBuffer`, `m_EditorActive`, `m_CursorPos`, `m_SelectionStart/End`, `m_ScrollY`.

> Note: the `kx_*` settings are **not** members of `CBotNet`; they are `g_Config` globals. Read/write them with `config.get("kx_...")` / `config.set("kx_...", v)` (or the legacy `config.m_Kx*` fields where exposed).

---

## Examples

**Read the local player state:**

```lua
local me = gc.m_aLocalIds[config.m_ClDummy]
local d = gc.m_aClients[me]
print("name:", d.m_aName, "team:", d.m_Team,
      "pos:", d.m_RenderPos.x, d.m_RenderPos.y)
```

**Send a chat message from a specific dummy:**

```lua
gc.m_Chat:SendChatFrom(0, 0, "hello from dummy 0")
```

**Drive a dummy input for 200 ms:**

```lua
gc.m_BotControl:ActionInput("right", 200, 1)
```

**Fetch a URL asynchronously:**

```lua
local job = net.get("https://example.com")
while not job:done() do end
print(job:status(), job:code())
print(job:result())
```

**Draw a circle at the local position every frame (long-running loop):**

```lua
while true do
    local me = gc.m_aLocalIds[config.m_ClDummy]
    local p = gc.m_aClients[me].m_RenderPos
    gfx.set_color(1, 0, 0, 1)
    gfx.draw_circle(p.x, p.y, 32)
end
```

**Toggle a Kinetix feature via config:**

```lua
config.set("kx_helpers", config.get("kx_helpers") == 1 and 0 or 1)
```

---

## Notes & limitations

- Only one script runs at a time; executing a new script stops the previous one.
- No true sandbox — scripts can read/write files (via `io`/`os` and DDNet's `io` storage) and spawn processes.
- Drawing calls issued from a script happen during the coroutine resume in `OnRender`; screen mapping and text colour are saved/restored around the resume.
- `io.popen` / `os.execute` are Windows-oriented (`CreateProcessA` with `CREATE_NO_WINDOW`).
- All references (e.g. `m_aClients[i]`, `m_aLocalIds`) are Lua **tables** built per access; modifications must go through the object they reference.
