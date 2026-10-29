# Kinetix

**Kinetix** — a feature-rich client for **DDNet / DDRace** (a Teeworlds mod). It ships a full in-game overlay (ClickGUI), a large multi-dummy toolkit, movement bots, pathfinding, TAS tooling, Lua scripting and much more.

[![TikTok](https://img.shields.io/badge/TikTok-@KinetixClient-000000?style=for-the-badge&logo=tiktok)](https://tiktok.com/@KinetixClient)
[![Telegram](https://img.shields.io/badge/Telegram-@KinetixClient-26A5E4?style=for-the-badge&logo=telegram)](https://t.me/KinetixClient)

- **Base:** [DDNet](https://github.com/ddnet/ddnet)
- **Version:** `20.1.1`
- **Build guide:** [docs/BUILDING.md](docs/BUILDING.md)

---

## Table of Contents

- [Overview](#overview)
- [Navigation](#navigation)
  - [Functions](#functions)
  - [Visuals](#visuals)
  - [Settings](#settings)
  - [Pathfinder](#pathfinder)
  - [Binds](#binds)
  - [Info](#info)
  - [Code Execution (Lua)](#code-execution-lua)
  - [TAS](#tas)
  - [Real-Time Pathfinder](#real-time-pathfinder)
  - [Dummy Manager](#dummy-manager)
  - [Search](#search)
- [Components & Architecture](#components--architecture)
- [ClickGUI Panels](#clickgui-panels)
- [Console Commands](#console-commands)
- [Placeholders](#placeholders)
- [Code Execution](#code-execution)
- [CVars](#cvars)
- [Files](#files)
- [FAQ](#faq)

---

## Overview

Kinetix is a **client-side mod** built on DDNet. Everything runs on your own game client — it does not require any server-side support, except for the features that are driven by the normal in-game inputs you already send (bots, dummies, TAS, etc.).

Main feature groups:

1. **ClickGUI** — a Yoga (flexbox) layout overlay with 11 draggable panels, smooth animations, tooltips, drag-to-sort, search, live resize, per-panel scroll, DPI-independent scaling and BG-blur.
2. **Multi-dummy** — up to **128 dummies** (`MAX_DUMMIES`) with per-dummy profiles, per-dummy **SOCKS5 proxy** support, smart dummy switch and batched commands.
3. **Movement & combat bots** — Helpers (Block), AimBot, TriggerBot, Auto Fire, BalanceBot, Auto/Triple Fly, Fly Ride, Avoid Freeze, Laser Unfreeze, Fake Aim.
4. **Pathfinding** — the classic Pathfinder (BFS / Beam / RHEA / BMCTS with a state-lattice A*), a chunk-based **mini TAS maker**, and the **Real-Time Pathfinder** (worker-thread planner reproduced on the live player).
5. **TAS** — record/play/replay, rewind/forward, auto rewind-forward per tile, Fake Aim during playback.
6. **Scripting** — **Code Execution (Lua)** via sol2, with near-total access to the DDNet classes.
7. **Visuals** — Trajectory prediction, ESP, Hitboxes, custom/rainbow colours for sprites & entities, line rendering (size/colour/opacity/rainbow/gradient/layer).
8. **Utility** — Spoofer (client name/version/hash, zoom, spectate), IRC over emotes, Admin Alarm, smart binds.

### What is this for?

Multi-dummy automation, raiding, advertising, movement-assist, TAS making, and writing your own tools via [Code Execution](#code-execution).

---

## Navigation

ClickGUI is opened with the `cl_clickgui` toggle (bind a key: `bind <key> toggle cl_clickgui 1 0`). The tree below mirrors the panels, their rows, and the rows' children (only [Expandable](#clickgui-panels) rows have children).

- **[Functions](#functions)**
  - Helpers *(expandable)* → Auto Main, Main ID, All Target, Target IDs, Rescue, All Rescue, Rescue IDs, Dummy IDs, Fire Dist, Hook Dist, Main Dist, Target Dist, Rescue Dist, Stand Dist, MStand Dist
  - AimBot *(expandable)* → Weapon, Enable, FOV, Radius, Show FOV, Show Radius, Auto Switch Weapon, Use raycast (Show), Rules, Use raycast angle (AB), Predict, Jitter, Silent, Mode, Smooth Speed, Center snap, Team, Friend, Dummy, Clanmate, Freeze, Off while freeze, FNG Freeze, Priority, Targets, Targets IDs
  - TriggerBot *(expandable)* → Weapon, Enable, Trigger, Trigger mode, FOV, Radius, Show FOV, Show Radius, Auto Switch Weapon, Use raycast (Show), Rules, Latency, Team, Friend, Dummy, Clanmate, Freeze, Off while freeze, FNG Freeze, Priority, Targets, Targets IDs, Use silent channel AB
  - Auto Fire *(expandable)* → Delay
  - BalanceBot *(expandable)* → Vertical Filter, Radius Filter, Stand Radius, DDrace Team, Target ID, Team, Friend, Dummy, Clanmate, Freeze, Off while freeze, FNG Freeze, Targets, Targets IDs
  - Preserve input
  - Custom latency *(expandable)* → Latency (ms)
  - Auto fly
  - Auto Triple Fly *(expandable)* → Dummy mode, Dummy ID, Target mode, Target IDs, Trigger radius, Radius
  - Copy Moves Filter *(expandable)* → Filter: Jump, Filter: Direction, Filter: Hook, Filter: Aim, Filter: Fire, Filter: Weapon
  - Mirror direction
  - Aim mode
  - Mirror aim X / Mirror aim Y *(visible when Aim mode = Mirror)*
  - Smart aim *(visible when Aim mode = Smart)*
  - Fly Ride
  - Copy Moves Latency *(expandable)* → Latency (ms)
  - Laser unfreeze *(expandable)* → Auto laser, FOV, Angles, Ticks, Trigger ticks, Silent, Show attempt
  - Fake Aim *(expandable)* → Mode, Speed, Jitter, Distance from center, Show for me, Show Server-Side Aim, Predict
  - Avoid Freeze *(expandable)* → Freeze, Teleport, Death, Direction, Jump, Hook, Aim, FOV, Angles, Silent, Ticks, Ticks Before
- **[Visuals](#visuals)**
  - Custom colors *(expandable)* → Sprite, Entity, Enable, Color, Opacity, Rainbow, Speed
  - Hide particles
  - Trajectory *(expandable)* → Type, Show, Prediction Ticks, Alpha Gradient, Simulate Players, Show for other players, Show for current
  - ESP *(expandable)* → Pointer, Pointer Style, Pointer Speed, Box, Box Style, Box Speed, X Box scale, Y Box scale, DDrace Team, Target ID, Team, Friend, Dummy, Clanmate, Freeze, FNG Freeze, Mode, X, Y, Targets, Targets IDs
  - Hitboxes *(expandable)* → Tee tile, Hitbox Style, Box Speed, Tee trigger, Hitbox Style, Box Speed, Show for others, DDrace Team, Target ID, Team, Friend, Dummy, Clanmate, Freeze, FNG Freeze, Targets, Targets IDs
  - Bypass Zoom Lock
  - Bypass Asset Lock
  - Admin Alarm *(expandable)* → Admin level, Message, Repeat, Every ms
- **[Settings](#settings)**
  - ClickGUI Settings *(expandable)* → Main Color, Rainbow Main Color, Speed, Panel Color, BG Blackout, BG Blur, Blur Radius, ClickGUI Scale, Panels Start X, Panels Start Y, Panels X-indentation, Panels Y-indentation, Dynamic scale (Beta), Panels width, Panels height, Drag-To-Sort
  - Line rendering *(expandable)* → Component, Line color, Line size, Line opacity, Rainbow, Speed, Gradient, Step, Layer
  - IRC *(expandable)* → Reveal join, Nameplate tag
  - Spoofer *(expandable)* → Version ID, Client preset, Custom client name, Client version, Git hash, Git hash value, Extra NETMSGs
  - Spoof Zoom *(expandable)* → Fake Zoom
  - Spoof Spectate *(expandable)* → Mode, Fake spectate X, Fake spectate Y, Fake Follow ID
- **[Pathfinder](#pathfinder)**
  - Effort *(dropdown)*
  - Algorithm *(dropdown)*
  - Freedom Path *(expandable)* → Ratio
  - Settings *(expandable)* → A* Weight, Evolutions, Beam width, Performance, Freeze Support (Beta)
  - Visuals *(expandable)* → Show field, Show hooks, Show branches, Show speed
  - Pathfinding / Paste *(double button)*
  - Editor Forbidden Tiles / Clear Forbidden Tiles *(double button)*
  - Tunnel size *(slider)*
  - Generate tunnel *(button)*
  - Editor Finish Tiles / Clear Finish Tiles *(double button)*
  - Progress / ETA / Time *(labels)*
- **[Binds](#binds)**
  - Bind Mode
  - Binds *(expandable)* → ClickGUI, AimBot, TriggerBot, Fake Aim, Laser Unfreeze, Copy Moves Filter, Copy Moves Latency, Trajectory, ESP, Avoid Freeze, Helpers, Auto Triple Fly, Auto Fire, BalanceBot, TAS Record, TAS Pause, TAS Rewind, TAS Forward
  - Edge bind *(expandable)* → Buttons, Edge in ms
  - Smart dummy switch *(expandable)* → Button, Mode, Dummy IDs
- **[Info](#info)**
  - Show spoofed, Client Name, Client Version, Client Hash, Kinetix Version, AVG FPS, Exe Hash, Telegram, TikTok
- **[Code Execution (Lua)](#code-execution-lua)**
  - Pick File, Path, Name, Version, Author, Description, Execute
- **[TAS](#tas)**
  - Mode, Pick File / Unpick File, Action, Map, Name, Author, Save, Join, TPS, Pause
  - Auto Rewind/Forward *(expandable)* → Tile, Mode, Rewind Ticks
  - Fake Aim *(expandable)* → Mode
  - Visuals *(expandable)* → Show Path, Show Hooks, Show Speed, Trajectory
  - Forward, Rewind, Status
- **[Real-Time Pathfinder](#real-time-pathfinder)**
  - Reconstruct *(expandable)* → Reconstruct Trigger
  - Future Limit *(expandable)* → Future Ticks
  - Visuals *(expandable)* → Show Path, Show Hooks, Show Field, Show Previous Path, Show Speed
  - RT Pathfinding *(button)*
  - RT Editor Forbidden Tiles / Clear Forbidden Tiles *(double button)*
  - RT Editor Finish Tiles / Clear Finish Tiles *(double button)*
- **[Dummy Manager](#dummy-manager)**
  - Dummies *(slider — runtime limit)*
  - Dummies *(expandable)* → Connect Dummy, Switch to Main, D1 Connect/Switch … D127 Connect/Switch
  - Proxy manager *(expandable)* → Main, Dummy 1 … Dummy 127 (toggle + `u:p@socks5:proxy` input)
  - Dummy Profiles *(expandable)* → Edit Mode, Open Main Profile, per-dummy toggles, Open Targets Profile, Open Dummy N Profile, Open All Profile
  - Dummy Tee *(expandable)* → Edit Mode, Open Main Tee, per-dummy toggles, Open Targets Tee, Open Dummy N Tee, Open All Tee
  - Reconnect (Apply) *(button — visible only while proxy changes are staged)*
- **[Search](#search)**
  - Search *(input)*
  - Search parameters *(expandable)* → Case Sensitive, Whole Word, Use Regex, Fuzzy Match

---

## Components & Architecture

All Kinetix game logic lives under [`src/game/client/components/kinetix`](src/game/client/components/kinetix). Each component is registered in `CGameClient::OnInit()` and follows the folder convention documented in [`kinetix/README.md`](src/game/client/components/kinetix/README.md): one `.cpp` + one `.h` → files in the `kinetix/` root; two or more `.cpp`/`.h` → a subfolder.

| Component | Class | Role |
|-----------|-------|------|
| `kinetix.cpp/.h` + `kinetix_internal.h` | `CBotNet` | Core: lifecycle, dummy processing, admin alarm, pathfinder/RT entry points |
| `clickgui.cpp/.h` | `CClickGui` | The overlay UI (Yoga layout, animations, input) |
| `aimbot.cpp/.h` | `CAimBot` | Per-weapon aim bot + trigger bot |
| `bot_control.cpp/.h` | `CBotControl` | `kx_input`, `kx_stop`, `kx_aim`, `kx_oaim` runtime input control |
| `esp.cpp/.h` | `CEsp` | ESP lines / boxes / hitboxes |
| `trajectory.cpp/.h` | `CTrajectory` | Per-type trajectory prediction (Tee/Pistol/Shotgun/Grenade/Laser) |
| `avoid_freeze.cpp/.h` | `CAvoidFreeze` | Brute-force freeze avoidance (per predicted tick) |
| `laser_unfreeze.cpp` | `CBotNet` methods | Predictive laser self-unfreeze |
| `fly_ride.cpp` | `CBotNet` methods | Pilot hooks/hammers a dummy (WASD moves anchor) |
| `triple_fly.cpp` | `CBotNet` methods | Auto triple fly |
| `fake_aim.cpp` | `CBotNet` methods | Fake/real aim modes for rendering |
| `tas.cpp/.h` | `CTas` | TAS record/play/replay + rewind/forward |
| `irc.cpp/.h` | `CIRC` | Emote-encoded IRC messaging |
| `socks5_proxy.cpp/.h` | `CSocks5Proxy` | Per-dummy SOCKS5 UDP proxy client |
| `kinetix_lines.cpp/.h` | `CKinetixLines` | Deferred line-render overlay (layer system) |
| `console_commands.cpp` | `CBotNet` methods | `kx_send` + placeholder replacement |
| `attack/` | `CBotNet` methods | Block/attack logic + `kx_atk_dists` etc. |
| `dummies/` | `CDummyData` | Per-dummy profiles, proxy config, connect/switch |
| `pathfinder/` | `CBotNet` methods | Grid, A*/RHEA/BMCTS search, tunnel gen, pathfinder tab |
| `macro/` | `CBotNet` methods | `kx_macro_*` macro recording/playback |
| `code_exec/` | `CCodeExec` | Lua scripting engine + file dialog |

---

## ClickGUI Panels

Each row type is one of: `Toggle`, `ToggleDropdown`, `Slider`, `Input`, `ToggleInput`, `Dropdown`, `Expandable`, `Button`, `DoubleButton`, `Label`. Only `Expandable` rows have children ("subfunctions").

### Functions

Combat, dummy control and reaction bots.

#### Helpers
The Block/Helpers bot. Movement, fire, hook and rescue are driven by the Block pathfinder; most toggles are always on now.

| Function | Description |
|----------|-------------|
| Auto Main | Automatically set the main ID to the currently active dummy. |
| Main ID | Main player server ID. |
| All Target | Target all players (Target IDs becomes an exclusion list). |
| Target IDs | Comma-separated target server IDs. |
| Rescue | Enable rescuing frozen teammates. |
| All Rescue | Rescue all frozen players (Rescue IDs becomes an exclusion list). |
| Rescue IDs | Specific server IDs to rescue / unrescue. |
| Dummy IDs | Which dummy slots act as helpers (`0,1,2,...`). |
| Dists: | Label separating the distance settings. |
| Fire Dist | Distance at which the bot fires (32–64). |
| Hook Dist | Distance at which the bot hooks (64–400). |
| Main Dist | Radius to stay near main. |
| Target Dist | Max distance to consider a target. |
| Rescue Dist | Radius to search for frozen players. |
| Stand Dist | Don't move if within N units of the target/main. |
| MStand Dist | Stand distance for main. |

#### AimBot
Per-weapon aim bot + trigger bot. Settings are selected by the **Weapon** dropdown (same pattern for TriggerBot).

| Function | Description |
|----------|-------------|
| Weapon | Which weapon's settings are edited: Hammer/Pistol/Shotgun/Grenade/Laser/Ninja/Hook. |
| Enable | Enable the aim bot for the selected weapon. |
| FOV | Field of view for target selection (degrees). |
| Radius | Detection radius around the aim. |
| Show FOV / Show Radius | Draw the FOV cone / radius. |
| Auto Switch Weapon | Switch to this weapon when it has a target in its zone. |
| Use raycast (Show) | Draw the zone using raycast. |
| Rules | `None` / `Insert line` / `Raycast`. |
| Use raycast angle (AB) | Derive the aim angle from the raycast. |
| Predict | Iterative time-of-flight prediction. |
| Jitter | Randomize the aim point within the hitbox (%). |
| Silent | Silent aim (separate toggle). |
| Mode | `Edge` / `Trigger` / `Always` / `Smooth` (Trigger hidden while Silent). |
| Smooth Speed | Aim smoothing speed. |
| Center snap | Snap to the player centre. |
| Team | `Both` / `My` / `War` / `Blue` / `Red`. |
| Friend / Dummy / Clanmate | Filters: `Both` / `Ignore` / `Only`. |
| Freeze | `Both` / `Freeze` / `No freeze`. |
| Off while freeze | Pause the bot while the local tee is frozen. |
| FNG Freeze | Treat ninja as frozen (FNG does not sync freeze). |
| Priority | `Nearest FOV` / `Nearest Aim` / `Nearest to me`. |
| Targets | `Both` / `Only` / `Ignore`. |
| Targets IDs | Comma-separated target IDs (used with the Targets filter). |

#### TriggerBot
Fires/hooks automatically when a target enters the weapon's zone.

| Function | Description |
|----------|-------------|
| Weapon | Selected weapon (no Hook). |
| Enable | Enable the trigger bot for the weapon. |
| Trigger | `Fire` / `Hook`. |
| Trigger mode | `One tick` / `Hold` / `Every tick`. |
| FOV / Radius / Show FOV / Show Radius | Same as AimBot. |
| Auto Switch Weapon | Same as AimBot. |
| Use raycast (Show) / Rules | Same as AimBot. |
| Latency | Delay in ticks before firing (0–10). |
| Team / Friend / Dummy / Clanmate / Freeze | Same filters as AimBot. |
| Off while freeze / FNG Freeze | Same as AimBot. |
| Priority / Targets / Targets IDs | Same as AimBot. |
| Use silent channel AB | While the AimBot's silent aim is active, this weapon's zone follows the silent direction. |

#### Auto Fire
| Function | Description |
|----------|-------------|
| Delay | Auto-fire every N ticks while fire is held (0–50). |

#### BalanceBot
Keeps X-axis balance against the nearest matching player (drives the current dummy only).

| Function | Description |
|----------|-------------|
| Vertical Filter | `Below` / `Above` / `Both`. |
| Radius Filter | Search radius (0–1000). |
| Stand Radius | Balance tolerance (1–10). |
| DDrace Team / Target ID / Team / Friend / Dummy / Clanmate / Freeze | Player filters (same options as AimBot). |
| Off while freeze / FNG Freeze | Same as AimBot. |
| Targets / Targets IDs | Same as AimBot. |

#### Fly
| Function | Description |
|----------|-------------|
| Preserve input | Keep dummy movement inputs while flying. |
| Custom latency | Expandable: adjust the input replay latency (ms). |
| Auto fly | Auto-hammer for hammerfly: every tick it checks whether a hammer press would really hit the active player (reload timer, freeze, weapon, team collision, strike geometry). Only then it aims and presses for one tick; otherwise the dummy's own input and aim stay untouched. |
| Auto Triple Fly | Acting dummy auto-hooks a target entering the trigger radius and holds while farther than the radius. Children: Dummy mode, Dummy ID, Target mode, Target IDs, Trigger radius, Radius. |
| Fly Ride | Pilot (active dummy) hooks/hammers the nearest dummy; WASD moves the anchor. |

#### Copy Moves
| Function | Description |
|----------|-------------|
| Copy Moves Filter | Expandable: choose which inputs are copied (Jump/Direction/Hook/Aim/Fire/Weapon). |
| Mirror direction | Invert the copied direction. |
| Aim mode | `Smart` (same world point) / `Mirror`. |
| Mirror aim X / Mirror aim Y | Visible when Aim mode = Mirror. |
| Smart aim | Visible when Aim mode = Smart. |
| Copy Moves Latency | Expandable: staggered replay delay — dummy N replays input from `latency × N` ms ago. |

#### Reaction / Aim modifiers
| Function | Description |
|----------|-------------|
| Laser unfreeze | Expandable: predictive laser self-unfreeze — Auto laser, FOV, Angles, Ticks, Trigger ticks, Silent, Show attempt. |
| Fake Aim | Expandable: Mode (Random/Robot/Spin/Lag/Chaos/Real), Speed, Jitter, Distance from center, Show for me, Show Server-Side Aim, Predict. |
| Avoid Freeze | Expandable: brute-forces input combos per predicted tick — Freeze, Teleport, Death, Direction, Jump, Hook, Aim, FOV, Angles, Silent, Ticks, Ticks Before. |

---

### Visuals

| Function | Description |
|----------|-------------|
| Custom colors | Expandable: per-sprite custom colour — Sprite (Tee/Legs/Entities/Guns/Hook/HUD/Particles/Emotions), Entity category, Enable, Color (hex), Opacity, Rainbow, Speed. |
| Hide particles | Hide in-game particle effects. |
| Trajectory | Expandable: per-type prediction (Type = Tee/Pistol/Shotgun/Grenade/Laser) — Show, Prediction Ticks, Alpha Gradient, Simulate Players, Show for other players, Show for current. |
| ESP | Expandable: lines/boxes to players — Pointer/Box toggles + styles/speeds, X/Y Box scale, DDrace Team + Target ID, Team/Friend/Dummy/Clanmate/Freeze/FNG filters, Mode (Active player / Screen coordinates), X, Y, Targets, Targets IDs. |
| Hitboxes | Expandable: Tee tile + Tee trigger hitboxes with styles/speeds, Show for others + full filter set. |
| Bypass Zoom Lock | Ignore the server-imposed zoom restriction. |
| Bypass Asset Lock | Ignore the server-imposed asset restriction. |
| Admin Alarm | Expandable: local chat echo when an admin is online — Admin level, Message (`{n}` = nickname), Repeat, Every ms. |

---

### Settings

| Function | Description |
|----------|-------------|
| ClickGUI Settings | Expandable (header = master toggle): Main Color, Rainbow Main Color, Speed, Panel Color, BG Blackout, BG Blur, Blur Radius, ClickGUI Scale, Panels Start X/Y, Panels X/Y-indentation, Dynamic scale (Beta) — drag-to-resize edges, Panels width/height (hidden when Dynamic scale is on), Drag-To-Sort. |
| Line rendering | Expandable: per-component line styling — Component (AimBot/TriggerBot/Trajectory/Laser Unfreeze/Pathfinder/ESP/TAS/Hitboxes/RT Pathfinder), Line color, Line size, Line opacity, Rainbow, Speed, Gradient, Step, Layer. |
| IRC | Expandable: Reveal join, Nameplate tag. |
| Spoofer | Expandable: Version ID, Client preset (DDNet/TClient/BestClient/Custom), Custom client name, Client version, Git hash, Git hash value, Extra NETMSGs. |
| Spoof Zoom | Expandable (header = master toggle): send a fake zoom to the server — Fake Zoom. |
| Spoof Spectate | Expandable (header = master toggle): Mode (Follow Coordinates / Follow Player), Fake spectate X/Y, Fake Follow ID. |

---

### Pathfinder

Chunk-based search (mini TAS maker) that simulates the real `CCharacter` in a cloned world.

| Function | Description |
|----------|-------------|
| Effort | `Very Low` … `Max` / `Custom` — preset for the search budget. |
| Algorithm | `BFS` / `Beam` / `RHEA` / `BMCTS`. |
| Freedom Path | Expandable: Ratio. |
| Settings | Expandable: A* Weight, Evolutions (RHEA), Beam width, Performance (%), Freeze Support (Beta). |
| Visuals | Expandable: Show field, Show hooks, Show branches, Show speed. |
| Pathfinding / Paste | Start the search / paste the found path into TAS Playground. |
| Editor Forbidden Tiles / Clear Forbidden Tiles | Paint tiles the path must avoid. |
| Tunnel size / Generate tunnel | Generate straight tunnels between obstacles. |
| Editor Finish Tiles / Clear Finish Tiles | Paint custom finish tiles. |
| Progress / ETA / Time | Live search labels. |

---

### Binds

| Function | Description |
|----------|-------------|
| Bind Mode | `Merge` / `Override`. |
| Binds | Expandable: input fields that commit `bind <key> toggle <cvar> 1 0` — ClickGUI, AimBot, TriggerBot, Fake Aim, Laser Unfreeze, Copy Moves Filter/Latency, Trajectory, ESP, Avoid Freeze, Helpers, Auto Triple Fly, Auto Fire, BalanceBot, TAS Record/Pause/Rewind/Forward. |
| Edge bind | Expandable: Buttons (comma list) + Edge in ms — listed keys release after the hold time. |
| Smart dummy switch | Expandable: Button, Mode (`Loop all` / `Order targets`), Dummy IDs. |

---

### Info

| Function | Description |
|----------|-------------|
| Show spoofed | Show the spoofed identity instead of the real one. |
| Client Name / Version / Hash | Real (or spoofed) name, version and git hash. |
| Kinetix Version | `Kinetix Version: v6`. |
| AVG FPS | Rolling average FPS. |
| Exe Hash | Executable hash. |
| Telegram / TikTok | Open the Kinetix social links. |

---

### Code Execution (Lua)

| Function | Description |
|----------|-------------|
| Pick File | Choose a `.lua` script. |
| Path / Name / Version / Author / Description | Metadata read from the selected script's header. |
| Execute | Run the selected script. |

See [Code Execution](#code-execution) and [`docs/CODE_EXECUTION.md`](docs/CODE_EXECUTION.md) for the full Lua API.

---

### TAS

| Function | Description |
|----------|-------------|
| Mode | `Play` / `Record`. |
| Pick File / Unpick File | Select the TAS file. |
| Action | Play/Record (label changes with mode). |
| Map / Name / Author | TAS metadata. |
| Save / Join Playground | Save the TAS / join the TAS playground. |
| TPS | Ticks per second for playback (1–50). |
| Pause | Pause playback. |
| Auto Rewind/Forward | Expandable: per-tile auto actions — Tile (Freeze/Teleport/Death), Mode (Rewind/Forward), Rewind Ticks. |
| Fake Aim | Expandable: rendered-aim mode during playback — Mode (Real/Smooth/Random/Robot/Spin/Lag). |
| Visuals | Expandable: Show Path, Show Hooks, Show Speed, Trajectory. |
| Forward / Rewind | Step one tick. |
| Status | Live status label. |

---

### Real-Time Pathfinder

Single-branch planner on a worker thread, reproduced on the live player. **Reconstruct** re-anchors the plan from the live position when the deviation exceeds the trigger (anti-TAS chaos). Tile editors are independent from the Pathfinder tab.

| Function | Description |
|----------|-------------|
| Reconstruct | Expandable (header = master toggle): Reconstruct Trigger. |
| Future Limit | Expandable (header = master toggle): Future Ticks. |
| Visuals | Expandable: Show Path, Show Hooks, Show Field, Show Previous Path, Show Speed. |
| RT Pathfinding | Start / stop (label turns green/red). |
| RT Editor Forbidden Tiles / Clear Forbidden Tiles | Paint forbidden tiles. |
| RT Editor Finish Tiles / Clear Finish Tiles | Paint finish tiles. |

---

### Dummy Manager

| Function | Description |
|----------|-------------|
| Dummies (slider) | Runtime dummy limit including main (1–128), enforced engine-side. |
| Dummies | Expandable: Connect Dummy, Switch to Main, then `D<n> Connect` / `D<n> Switch` for every dummy slot. |
| Proxy manager | Expandable: a toggle + proxy input (`user:pass@host:port`) per slot (slot 0 = Main). |
| Dummy Profiles | Expandable: Edit Mode (Per-Dummy/All/Targets), Open Main Profile, per-dummy toggles, Open Targets Profile, per-dummy "Open Dummy N Profile" buttons, Open All Profile. |
| Dummy Tee | Same as Dummy Profiles but for the tee/body settings. |
| Reconnect (Apply) | Visible only while proxy changes are staged; pushes proxies and reconnects the affected dummies (main does a full reconnect). |

---

### Search

Real-time filtering of every other panel.

| Function | Description |
|----------|-------------|
| Search | Query input; empty shows everything. |
| Search parameters | Expandable: Case Sensitive, Whole Word, Use Regex, Fuzzy Match. |

---

## Console Commands

Commands registered by Kinetix (in addition to the `kx_*` cvars, which are also settable directly — see [CVars](#cvars)).

| Command | Signature | Description |
|---------|-----------|-------------|
| `cl_clickgui` | `0/1` | Show/hide the ClickGUI overlay (bindable). |
| `kx_input` | `s[action] i[ms] ?i[dummy]` | Send an input to a dummy: `jump`/`hook`/`fire`/`left`/`right` for `ms` on the optional dummy. |
| `kx_stop` | `?i[dummy]` | Stop all bot actions for a dummy (default: current). |
| `kx_aim` | `i[dx] i[dy] ?i[dummy]` | Relative aim offset for a dummy. |
| `kx_oaim` | `i[x] i[y] ?i[dummy]` | Absolute aim position for a dummy. |
| `kx_irc_send` | `r[text]` | Send an emote-encoded IRC message. |
| `kx_tas_forward` | — | TAS: step one tick forward. |
| `kx_tas_rewind` | — | TAS: step one tick backward. |
| `kx_targets` | `s[ids]` | Set the target IDs. |
| `kx_bots` | `s[ids]` | Set the bot (ally) IDs. |
| `kx_atk_dists` | `fffffff` | Set radii: fire, hook, rescue, target, main, stand, main stand. |
| `kx_rescue_ids` | `s[ids]` | Rescue/Unrescue IDs. |
| `kx_pathfinder_go` | `i[on] ?i[x] ?i[y]` | Move to a position (`0` disables, `1 x y` enables and sets the target). |
| `kx_macro_load` | `s[path]` | Load a macro from file. |
| `kx_macro_play` | `i[on]` | Play the loaded macro (1=start, 0=stop). |
| `kx_macro_record` | `i[on]` | Record a macro (1=start, 0=stop). |
| `kx_macro_save` | `s[path]` | Save the recorded macro to file. |
| `kx_macro_capture` | `i[id]` | Set the capture ID for macro recording. |
| `kx_send` | `s[dummyids] r[command]` | Execute a command on the given dummies (`-1` = all). |
| `kx_pf_live` | `i[state]` | Pathfinder tab: 0=idle, 1=running, 2=finished. |
| `kx_pf_paste` | — | Paste the found path into TAS Playground as the current run. |
| `kx_dummy_switch` | — | Switch to the next dummy (smart dummy switch). |
| `kx_exec` | `s[code]` | Execute Lua code. |
| `kx_exec_stop` | — | Stop the running Lua script. |
| `kx_dummy_limit` | `i[limit]` | Dummy limit incl. main (1–128). |
| `dummy<N>_name` | `s[value]` | Dummy N profile name (empty = shared `dummy_name`). |
| `dummy<N>_clan` | `s[value]` | Dummy N profile clan. |
| `dummy<N>_country` | `i[value]` | Dummy N country (-1 = inherit `dummy_country`). |
| `dummy<N>_skin` | `s[value]` | Dummy N skin. |
| `dummy<N>_use_custom_color` | `i[value]` | Dummy N custom-colour toggle (-1 = inherit). |
| `dummy<N>_color_body` / `dummy<N>_color_feet` | `i[value]` | Dummy N colours. |
| `dummy<N>_default_eyes` | `i[value]` | Dummy N default eyes. |
| `dummy<N>_proxy` | `s[value]` | Dummy N SOCKS5 proxy (`user:pass@host:port`). |
| `dummy<N>_proxy_on` | `i[value]` | Dummy N proxy toggle. |

---

## Placeholders

`kx_send` substitutes these placeholders (per-dummy) in the forwarded command:

| Placeholder | Description |
|-------------|-------------|
| `{i}` | Rank of the target dummy among the connected dummies (1-based). |
| `{r}` | One random character from `a-z A-Z 0-9 _ . -`. |
| `{ri-N}` | Random integer from 0 to N (e.g. `{ri-100}`). |
| `{c}` | Random CJK character (Unicode U+4E00 – U+9FFF). |

---

## Code Execution

Kinetix embeds a Lua interpreter (sol2) exposed through the **Code Execution (Lua)** panel and the `kx_exec` command. Scripts get direct access to virtually all DDNet client classes (`gc`), the `IClient` API (`cl`), the config (`config`), the console, HTTP, subprocesses, and graphics/sound/input/text helpers.

```lua
-- example: print the local player's position every second
while true do
    local me = gc.m_aLocalIds[config.m_ClDummy]
    print("pos:", gc.m_aClients[me].m_RenderPos.x, gc.m_aClients[me].m_RenderPos.y)
    sleep(1000)
end
```

**Full API reference:** [`docs/CODE_EXECUTION.md`](docs/CODE_EXECUTION.md).

Key globals:

| Global | Purpose |
|--------|---------|
| `print(...)` | Print to the in-game chat. |
| `vec2(x, y)` | Vector with operators, `length`, `normalize`, `dot`, `distance`, `angle`. |
| `CONST` | Engine constants (`HOOK_*`, `WEAPON_*`, `TEAM_*`, `MAX_DUMMIES`, …). |
| `gc` | `CGameClient` — the entire game state (chat, clients, snap, worlds, controls, camera, `m_BotNet`, all components). |
| `cl` | `IClient` — Connect/Disconnect, Rcon, DummyConnect/Connected/Count, ticks, player name, … |
| `config` | `config.get(name)` / `config.set(name, value)` for any cvar (by script name). |
| `console` | `execute(line)`, `print(s)`, `execute_file(f)`. |
| `net` | `net.get/post/post_json` → job with `done/status/result/code/abort`. |
| `io` / `os` | `io.popen(cmd)` and `os.execute(cmd)` run windowless (no console flash). |
| `gfx` / `sound` / `input` / `text` | Rendering, audio, input and text helpers. |

---

## CVars

All Kinetix settings are `kx_*` cvars, plus the overlay toggle `cl_clickgui`. They can be set in the console, bound with `bind <key> toggle <cvar> 1 0`, or changed from the ClickGUI. Values are saved unless noted.

| Cvar | Description |
|------|-------------|
| `cl_clickgui` | Show/hide the Kinetix ClickGUI overlay |
| `kx_ab_grenade_aim_mode` | AimBot grenade: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_grenade_auto_switch` | AimBot grenade: auto switch to this weapon when it has a target |
| `kx_ab_grenade_clanmate` | AimBot grenade: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_grenade_dummy` | AimBot grenade: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_grenade_enabled` | AimBot grenade: 1=enabled |
| `kx_ab_grenade_fng_freeze` | AimBot grenade: treat ninja as frozen (FNG mode) |
| `kx_ab_grenade_fov` | AimBot grenade: FOV degrees (1-360) |
| `kx_ab_grenade_freeze` | AimBot grenade: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_grenade_friend` | AimBot grenade: friend filter 0=both 1=ignore 2=only |
| `kx_ab_grenade_jitter` | AimBot grenade: aim jitter % of hitbox (0-100) |
| `kx_ab_grenade_off_while_freeze` | AimBot grenade: pause while you are frozen |
| `kx_ab_grenade_predict` | AimBot grenade: predict target position |
| `kx_ab_grenade_priority` | AimBot grenade: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_grenade_radius` | AimBot grenade: max radius (1-2000) |
| `kx_ab_grenade_raycast_show` | AimBot grenade: use raycast for show rendering |
| `kx_ab_grenade_rules` | AimBot grenade: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_grenade_silent` | AimBot grenade: silent aim (does not move the visible crosshair) |
| `kx_ab_grenade_smooth_center_snap` | AimBot grenade: smooth center snap (rotate around the player center) |
| `kx_ab_grenade_smooth_speed` | AimBot grenade: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_grenade_targets_filter` | AimBot grenade: targets filter 0=both 1=only 2=ignore |
| `kx_ab_grenade_targets_ids` | AimBot grenade: targets filter IDs |
| `kx_ab_grenade_team` | AimBot grenade: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_grenade_use_angle` | AimBot grenade: aim at raycast hit point |
| `kx_ab_hammer_aim_mode` | AimBot hammer: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_hammer_auto_switch` | AimBot hammer: auto switch to this weapon when it has a target |
| `kx_ab_hammer_clanmate` | AimBot hammer: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_hammer_dummy` | AimBot hammer: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_hammer_enabled` | AimBot hammer: 1=enabled |
| `kx_ab_hammer_fng_freeze` | AimBot hammer: treat ninja as frozen (FNG mode) |
| `kx_ab_hammer_fov` | AimBot hammer: FOV degrees (1-360) |
| `kx_ab_hammer_freeze` | AimBot hammer: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_hammer_friend` | AimBot hammer: friend filter 0=both 1=ignore 2=only |
| `kx_ab_hammer_jitter` | AimBot hammer: aim jitter % of hitbox (0-100) |
| `kx_ab_hammer_off_while_freeze` | AimBot hammer: pause while you are frozen |
| `kx_ab_hammer_predict` | AimBot hammer: predict target position |
| `kx_ab_hammer_priority` | AimBot hammer: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_hammer_radius` | AimBot hammer: max radius (1-2000) |
| `kx_ab_hammer_raycast_show` | AimBot hammer: use raycast for show rendering |
| `kx_ab_hammer_rules` | AimBot hammer: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_hammer_silent` | AimBot hammer: silent aim (does not move the visible crosshair) |
| `kx_ab_hammer_smooth_center_snap` | AimBot hammer: smooth center snap (rotate around the player center) |
| `kx_ab_hammer_smooth_speed` | AimBot hammer: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_hammer_targets_filter` | AimBot hammer: targets filter 0=both 1=only 2=ignore |
| `kx_ab_hammer_targets_ids` | AimBot hammer: targets filter IDs |
| `kx_ab_hammer_team` | AimBot hammer: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_hammer_use_angle` | AimBot hammer: aim at raycast hit point |
| `kx_ab_hook_aim_mode` | AimBot hook: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_hook_auto_switch` | AimBot hook: auto switch to this weapon when it has a target |
| `kx_ab_hook_clanmate` | AimBot hook: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_hook_dummy` | AimBot hook: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_hook_enabled` | AimBot hook: 1=enabled |
| `kx_ab_hook_fng_freeze` | AimBot hook: treat ninja as frozen (FNG mode) |
| `kx_ab_hook_fov` | AimBot hook: FOV degrees (1-360) |
| `kx_ab_hook_freeze` | AimBot hook: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_hook_friend` | AimBot hook: friend filter 0=both 1=ignore 2=only |
| `kx_ab_hook_jitter` | AimBot hook: aim jitter % of hitbox (0-100) |
| `kx_ab_hook_off_while_freeze` | AimBot hook: pause while you are frozen |
| `kx_ab_hook_predict` | AimBot hook: predict target position |
| `kx_ab_hook_priority` | AimBot hook: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_hook_radius` | AimBot hook: max radius (1-2000) |
| `kx_ab_hook_raycast_show` | AimBot hook: use raycast for show rendering |
| `kx_ab_hook_rules` | AimBot hook: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_hook_silent` | AimBot hook: silent aim (does not move the visible crosshair) |
| `kx_ab_hook_smooth_center_snap` | AimBot hook: smooth center snap (rotate around the player center) |
| `kx_ab_hook_smooth_speed` | AimBot hook: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_hook_targets_filter` | AimBot hook: targets filter 0=both 1=only 2=ignore |
| `kx_ab_hook_targets_ids` | AimBot hook: targets filter IDs |
| `kx_ab_hook_team` | AimBot hook: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_hook_use_angle` | AimBot hook: aim at raycast hit point |
| `kx_ab_laser_aim_mode` | AimBot laser: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_laser_auto_switch` | AimBot laser: auto switch to this weapon when it has a target |
| `kx_ab_laser_clanmate` | AimBot laser: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_laser_dummy` | AimBot laser: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_laser_enabled` | AimBot laser: 1=enabled |
| `kx_ab_laser_fng_freeze` | AimBot laser: treat ninja as frozen (FNG mode) |
| `kx_ab_laser_fov` | AimBot laser: FOV degrees (1-360) |
| `kx_ab_laser_freeze` | AimBot laser: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_laser_friend` | AimBot laser: friend filter 0=both 1=ignore 2=only |
| `kx_ab_laser_jitter` | AimBot laser: aim jitter % of hitbox (0-100) |
| `kx_ab_laser_off_while_freeze` | AimBot laser: pause while you are frozen |
| `kx_ab_laser_predict` | AimBot laser: predict target position |
| `kx_ab_laser_priority` | AimBot laser: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_laser_radius` | AimBot laser: max radius (1-2000) |
| `kx_ab_laser_raycast_show` | AimBot laser: use raycast for show rendering |
| `kx_ab_laser_rules` | AimBot laser: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_laser_silent` | AimBot laser: silent aim (does not move the visible crosshair) |
| `kx_ab_laser_smooth_center_snap` | AimBot laser: smooth center snap (rotate around the player center) |
| `kx_ab_laser_smooth_speed` | AimBot laser: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_laser_targets_filter` | AimBot laser: targets filter 0=both 1=only 2=ignore |
| `kx_ab_laser_targets_ids` | AimBot laser: targets filter IDs |
| `kx_ab_laser_team` | AimBot laser: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_laser_use_angle` | AimBot laser: aim at raycast hit point |
| `kx_ab_ninja_aim_mode` | AimBot ninja: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_ninja_auto_switch` | AimBot ninja: auto switch to this weapon when it has a target |
| `kx_ab_ninja_clanmate` | AimBot ninja: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_ninja_dummy` | AimBot ninja: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_ninja_enabled` | AimBot ninja: 1=enabled |
| `kx_ab_ninja_fng_freeze` | AimBot ninja: treat ninja as frozen (FNG mode) |
| `kx_ab_ninja_fov` | AimBot ninja: FOV degrees (1-360) |
| `kx_ab_ninja_freeze` | AimBot ninja: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_ninja_friend` | AimBot ninja: friend filter 0=both 1=ignore 2=only |
| `kx_ab_ninja_jitter` | AimBot ninja: aim jitter % of hitbox (0-100) |
| `kx_ab_ninja_off_while_freeze` | AimBot ninja: pause while you are frozen |
| `kx_ab_ninja_predict` | AimBot ninja: predict target position |
| `kx_ab_ninja_priority` | AimBot ninja: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_ninja_radius` | AimBot ninja: max radius (1-2000) |
| `kx_ab_ninja_raycast_show` | AimBot ninja: use raycast for show rendering |
| `kx_ab_ninja_rules` | AimBot ninja: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_ninja_silent` | AimBot ninja: silent aim (does not move the visible crosshair) |
| `kx_ab_ninja_smooth_center_snap` | AimBot ninja: smooth center snap (rotate around the player center) |
| `kx_ab_ninja_smooth_speed` | AimBot ninja: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_ninja_targets_filter` | AimBot ninja: targets filter 0=both 1=only 2=ignore |
| `kx_ab_ninja_targets_ids` | AimBot ninja: targets filter IDs |
| `kx_ab_ninja_team` | AimBot ninja: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_ninja_use_angle` | AimBot ninja: aim at raycast hit point |
| `kx_ab_pistol_aim_mode` | AimBot pistol: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_pistol_auto_switch` | AimBot pistol: auto switch to this weapon when it has a target |
| `kx_ab_pistol_clanmate` | AimBot pistol: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_pistol_dummy` | AimBot pistol: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_pistol_enabled` | AimBot pistol: 1=enabled |
| `kx_ab_pistol_fng_freeze` | AimBot pistol: treat ninja as frozen (FNG mode) |
| `kx_ab_pistol_fov` | AimBot pistol: FOV degrees (1-360) |
| `kx_ab_pistol_freeze` | AimBot pistol: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_pistol_friend` | AimBot pistol: friend filter 0=both 1=ignore 2=only |
| `kx_ab_pistol_jitter` | AimBot pistol: aim jitter % of hitbox (0-100) |
| `kx_ab_pistol_off_while_freeze` | AimBot pistol: pause while you are frozen |
| `kx_ab_pistol_predict` | AimBot pistol: predict target position |
| `kx_ab_pistol_priority` | AimBot pistol: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_pistol_radius` | AimBot pistol: max radius (1-2000) |
| `kx_ab_pistol_raycast_show` | AimBot pistol: use raycast for show rendering |
| `kx_ab_pistol_rules` | AimBot pistol: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_pistol_silent` | AimBot pistol: silent aim (does not move the visible crosshair) |
| `kx_ab_pistol_smooth_center_snap` | AimBot pistol: smooth center snap (rotate around the player center) |
| `kx_ab_pistol_smooth_speed` | AimBot pistol: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_pistol_targets_filter` | AimBot pistol: targets filter 0=both 1=only 2=ignore |
| `kx_ab_pistol_targets_ids` | AimBot pistol: targets filter IDs |
| `kx_ab_pistol_team` | AimBot pistol: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_pistol_use_angle` | AimBot pistol: aim at raycast hit point |
| `kx_ab_shotgun_aim_mode` | AimBot shotgun: mode 0=edge 1=trigger 2=always 3=smooth |
| `kx_ab_shotgun_auto_switch` | AimBot shotgun: auto switch to this weapon when it has a target |
| `kx_ab_shotgun_clanmate` | AimBot shotgun: clanmate filter 0=both 1=ignore 2=only |
| `kx_ab_shotgun_dummy` | AimBot shotgun: dummy filter 0=both 1=ignore 2=only |
| `kx_ab_shotgun_enabled` | AimBot shotgun: 1=enabled |
| `kx_ab_shotgun_fng_freeze` | AimBot shotgun: treat ninja as frozen (FNG mode) |
| `kx_ab_shotgun_fov` | AimBot shotgun: FOV degrees (1-360) |
| `kx_ab_shotgun_freeze` | AimBot shotgun: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_ab_shotgun_friend` | AimBot shotgun: friend filter 0=both 1=ignore 2=only |
| `kx_ab_shotgun_jitter` | AimBot shotgun: aim jitter % of hitbox (0-100) |
| `kx_ab_shotgun_off_while_freeze` | AimBot shotgun: pause while you are frozen |
| `kx_ab_shotgun_predict` | AimBot shotgun: predict target position |
| `kx_ab_shotgun_priority` | AimBot shotgun: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_ab_shotgun_radius` | AimBot shotgun: max radius (1-2000) |
| `kx_ab_shotgun_raycast_show` | AimBot shotgun: use raycast for show rendering |
| `kx_ab_shotgun_rules` | AimBot shotgun: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_ab_shotgun_silent` | AimBot shotgun: silent aim (does not move the visible crosshair) |
| `kx_ab_shotgun_smooth_center_snap` | AimBot shotgun: smooth center snap (rotate around the player center) |
| `kx_ab_shotgun_smooth_speed` | AimBot shotgun: smooth speed x10 (1-500 = 0.1-50.0 px per tick) |
| `kx_ab_shotgun_targets_filter` | AimBot shotgun: targets filter 0=both 1=only 2=ignore |
| `kx_ab_shotgun_targets_ids` | AimBot shotgun: targets filter IDs |
| `kx_ab_shotgun_team` | AimBot shotgun: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_ab_shotgun_use_angle` | AimBot shotgun: aim at raycast hit point |
| `kx_admin_alarm` | Admin Alarm: 1=on (local chat echo alert when an RCON admin is on the server) |
| `kx_admin_alarm_level` | Admin Alarm: level being edited 0=helper 1=moderator 2=admin |
| `kx_admin_alarm_ms1` | Admin Alarm helper: repeat interval in ms (only when Repeat is on) |
| `kx_admin_alarm_ms2` | Admin Alarm moderator: repeat interval in ms (only when Repeat is on) |
| `kx_admin_alarm_ms3` | Admin Alarm admin: repeat interval in ms (only when Repeat is on) |
| `kx_admin_alarm_msg1` | Admin Alarm helper: echo message ({n} = admin nickname) |
| `kx_admin_alarm_msg2` | Admin Alarm moderator: echo message ({n} = admin nickname) |
| `kx_admin_alarm_msg3` | Admin Alarm admin: echo message ({n} = admin nickname) |
| `kx_admin_alarm_rep1` | Admin Alarm helper: repeat the echo while a helper is online |
| `kx_admin_alarm_rep2` | Admin Alarm moderator: repeat the echo while a moderator is online |
| `kx_admin_alarm_rep3` | Admin Alarm admin: repeat the echo while an admin is online |
| `kx_af_aim` | AF: allow aim angle variation in brute-force |
| `kx_af_angles` | AF: number of aim angles to try (within FOV) |
| `kx_af_avoid_death` | AF: avoid death tiles (TILE_DEATH / m_MarkedForDestroy) |
| `kx_af_avoid_freeze` | AF: avoid freeze tiles (m_FreezeTime > 0) |
| `kx_af_avoid_teleport` | AF: avoid teleport tiles (IsTeleport/IsEvilTeleport) |
| `kx_af_direction` | AF: allow direction changes (-1/0/1) |
| `kx_af_fov` | AF: aim FOV in degrees (spread angles within this cone) |
| `kx_af_hook` | AF: allow hook toggle |
| `kx_af_jump` | AF: allow jump toggle |
| `kx_af_silent` | AF: silent aim (don't move visible crosshair) |
| `kx_af_ticks` | AF: ticks to simulate per combination |
| `kx_af_ticks_before` | AF: ticks before the freeze to start acting (lead time) |
| `kx_aimbot` | AimBot: 1=on 0=off |
| `kx_aimbot_raycast_show` | AimBot: use raycast for show rendering |
| `kx_aimbot_show_fov` | AimBot: show FOV |
| `kx_aimbot_show_radius` | AimBot: show radius |
| `kx_asset_unlock` | Visuals: bypass asset lock (always use DDNet entities) |
| `kx_auto_fire` | Auto Fire: auto-fire every Delay ticks while fire is held |
| `kx_auto_fire_delay` | Auto Fire: ticks between auto shots (0 = every tick) |
| `kx_auto_main` | Auto-set main to active dummy |
| `kx_avoid_freeze` | Avoid Freeze: predict freeze and override input to avoid it |
| `kx_balance_bot` | BalanceBot: keep X distance to the nearest matching player (current dummy only) |
| `kx_balance_bot_clanmate` | BalanceBot: clanmate filter 0=both 1=ignore 2=only |
| `kx_balance_bot_ddrace_team` | BalanceBot: DDrace team filter 0=both 1=my 2=other 3=target |
| `kx_balance_bot_ddrace_team_id` | BalanceBot: DDrace team id (used when DDrace team filter = target) |
| `kx_balance_bot_dummy` | BalanceBot: dummy filter 0=both 1=ignore 2=only |
| `kx_balance_bot_fng_freeze` | BalanceBot: treat ninja as frozen (FNG mode) |
| `kx_balance_bot_freeze` | BalanceBot: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_balance_bot_friend` | BalanceBot: friend filter 0=both 1=ignore 2=only |
| `kx_balance_bot_off_while_freeze` | BalanceBot: pause while you are frozen |
| `kx_balance_bot_radius` | BalanceBot: search radius in units (players behind walls are excluded) |
| `kx_balance_bot_stand_radius` | BalanceBot: stop balancing inside this X radius (units) |
| `kx_balance_bot_targets_filter` | BalanceBot: targets filter 0=both 1=only 2=ignore |
| `kx_balance_bot_targets_ids` | BalanceBot: targets filter IDs |
| `kx_balance_bot_team` | BalanceBot: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_balance_bot_vertical` | BalanceBot: vertical filter 0=below 1=above 2=both |
| `kx_binds_mode` | Binds panel bind mode: 0=merge (append to existing bind), 1=override |
| `kx_bot_action_duration` | Duration in ms for bot input actions (kx_input UI buttons) |
| `kx_bot_target_dummy` | Target dummy for bot control UI (0=main, 1-127=dummies) |
| `kx_clickgui_scale` | ClickGUI scale (x10: 10 = 1.0x default, 1-20 = 0.1x-2.0x) |
| `kx_client_delay` | Client delay in ms (0 to disable) |
| `kx_copy_moves` | Copy Moves: 1=on 0=off |
| `kx_copy_moves_latency` | Copy Moves Latency: delay (ms) per dummy index before inactive dummies replay active player's input. N = latency_ms * dummy_index. Only applies when kx_copy_moves_latency_enabled is on. |
| `kx_copy_moves_latency_enabled` | Copy Moves Latency master toggle. When off, no latency is applied (live copy). When on, inactive dummies replay active player's input from N ms ago (N = kx_copy_moves_latency * dummy_index). |
| `kx_copy_target_id` | Copy Moves: target player ID (-1=disabled) |
| `kx_custom_colors` | Visuals: custom colors (master; 0 = original look) |
| `kx_drag_to_sort` | ClickGUI: drag-to-sort (1=swap by dragging over slot, 0=free positioning) |
| `kx_dummy_aim` | Whether dummy is aiming (requires cl_dummy_control 1) |
| `kx_dummy_copy_aim_mode` | Aim mode for copy moves: 0 = Smart (world coords), 1 = Mirror |
| `kx_dummy_copy_mirror_aim_x` | Mirror aim X of inactive dummies when copy moves is on (mirror mode) |
| `kx_dummy_copy_mirror_aim_y` | Mirror aim Y of inactive dummies when copy moves is on (mirror mode) |
| `kx_dummy_copy_mirror_dir` | Mirror direction of inactive dummies by XOR when copy moves is on |
| `kx_dummy_copy_moves_filter` | When on with copy_moves, only copy selected inputs (jump/direction/hook/aim/fire) instead of all |
| `kx_dummy_copy_moves_filter_aim` | Copy Moves Filter: copy aim (target X/Y) |
| `kx_dummy_copy_moves_filter_direction` | Copy Moves Filter: copy direction |
| `kx_dummy_copy_moves_filter_fire` | Copy Moves Filter: copy fire |
| `kx_dummy_copy_moves_filter_hook` | Copy Moves Filter: copy hook |
| `kx_dummy_copy_moves_filter_jump` | Copy Moves Filter: copy jump |
| `kx_dummy_copy_moves_filter_weapon` | Copy Moves Filter: copy weapon (wanted/next/prev weapon) |
| `kx_dummy_copy_smart_aim` | Smart aim: inactive dummies aim at the same world point as the player (smart mode) |
| `kx_dummy_direction` | Whether dummy is moving (requires cl_dummy_control 1) |
| `kx_dummy_hammer_auto` | Auto Fly: every tick, check whether a hammer press would really hit the active player (one-tick simulation: reload timer, freeze, weapon, team collision, strike geometry). If it would — aim at the player and press for one tick; otherwise the dummy's own input (and aim) is sent untouched |
| `kx_dummy_hammer_custom_latency` | Use custom hammer latency instead of default 500ms |
| `kx_dummy_hammer_keep_inputs` | Preserve dummy movement inputs (direction/jump/hook) during hammerfly; aim is still overridden for hammer direction |
| `kx_dummy_hammer_latency_ms` | Hammer latency in milliseconds (only used when kx_dummy_hammer_custom_latency is on) |
| `kx_edge_bind` | Edge Bind: listed keys release after a fixed hold time — the release fires after Edge in ms even if the key is still held |
| `kx_edge_bind_buttons` | Edge Bind: comma-separated key names (a,b,c,f1,mouse5) |
| `kx_edge_bind_ms` | Edge Bind: hold duration in ms before the release is sent |
| `kx_esp` | ESP: draw lines from point A to all matching players |
| `kx_esp_box` | ESP: show the box outline around targets |
| `kx_esp_box_scale_x` | ESP box width scale in % (100 = 64px wide, scales the box size only) |
| `kx_esp_box_scale_y` | ESP box height scale in % (100 = 64px tall, scales the box size only) |
| `kx_esp_box_speed` | ESP dotted box animation speed (0-100, only for dotted box) |
| `kx_esp_box_style` | ESP box style: 0=Box 1=Dotted box |
| `kx_esp_clanmate_filter` | ESP clanmate filter: 0=both 1=ignore 2=only |
| `kx_esp_ddrace_team` | ESP DDrace team filter: 0=both 1=my 2=other 3=target |
| `kx_esp_ddrace_team_id` | ESP DDrace team id (used when DDrace team filter = target) |
| `kx_esp_dummy_filter` | ESP dummy filter: 0=both 1=ignore 2=only |
| `kx_esp_fng_freeze` | ESP: treat ninja as frozen (FNG mode) |
| `kx_esp_freeze_filter` | ESP freeze filter: 0=both 1=freeze 2=no_freeze |
| `kx_esp_friend_filter` | ESP friend filter: 0=both 1=ignore 2=only |
| `kx_esp_mode` | ESP point A source: 0=active player 1=screen coordinates |
| `kx_esp_pointer` | ESP: show the pointer lines |
| `kx_esp_screen_x` | ESP screen X coordinate (used when mode=screen coordinates) |
| `kx_esp_screen_y` | ESP screen Y coordinate (used when mode=screen coordinates) |
| `kx_esp_speed` | ESP dotted animation speed (0-100, only for dotted styles) |
| `kx_esp_style` | ESP line style: 0=Line 1=Arrow 2=Dotted line 3=Dotted Arrow |
| `kx_esp_targets_filter` | ESP targets filter IDs (empty or no valid IDs = off) |
| `kx_esp_targets_filter_mode` | ESP targets filter: 0=both 1=only 2=ignore |
| `kx_esp_team_filter` | ESP team filter: 0=both 1=my 2=war 3=blue 4=red |
| `kx_fake_aim` | Fake Aim: send fake aim coordinates to server |
| `kx_fake_aim_dist_center` | Fake Aim: distance from center — rescale the aim sent to the server to exactly this distance from (0,0), preserving direction (0 = off). Applies to the real player aim, the laser unfreeze aim channel and all Fake Aim modes |
| `kx_fake_aim_jitter` | Fake Aim Chaos mode: jitter strength — max random offset (in world units) added to the real aim |
| `kx_fake_aim_mode` | Fake Aim mode: 0=Random 1=Robot 2=Spin 3=Lag 4=Chaos (jitters the real aim) 5=Real (sends the real aim unchanged) |
| `kx_fake_aim_predict` | Fake Aim: Predict — rendered aim is advanced between ticks at frame rate; Real renders exactly like no Fake Aim, Spin rotates smoothly |
| `kx_fake_aim_show_for_me` | Fake Aim: show fake aim on player character (not just server) |
| `kx_fake_aim_show_server_side` | Fake Aim: render the aim the server actually sees (visible only with Show For Me) |
| `kx_fake_aim_speed` | Fake Aim: speed (how often to change for Random, rotation speed for Spin) |
| `kx_fire_dist` | Fire distance (pixels, 32-64) |
| `kx_fly_ride` | Fly Ride: pilot (active dummy) hooks/hammers nearest dummy, WASD moves anchor point |
| `kx_helpers` | Helpers Mode: 1=on 0=off |
| `kx_hide_particles` | Visuals: hide Particles sprites |
| `kx_hitboxes` | Hitboxes master: draw the tile/trigger hitboxes (toggle on the Hitboxes header) |
| `kx_hitboxes_clanmate_filter` | Hitboxes clanmate filter: 0=both 1=ignore 2=only |
| `kx_hitboxes_ddrace_team` | Hitboxes DDrace team filter: 0=both 1=my 2=other 3=target |
| `kx_hitboxes_ddrace_team_id` | Hitboxes DDrace team id (used when DDrace team filter = target) |
| `kx_hitboxes_dummy_filter` | Hitboxes dummy filter: 0=both 1=ignore 2=only |
| `kx_hitboxes_fng_freeze` | Hitboxes: treat ninja as frozen (FNG mode) |
| `kx_hitboxes_freeze_filter` | Hitboxes freeze filter: 0=both 1=freeze 2=no_freeze |
| `kx_hitboxes_friend_filter` | Hitboxes friend filter: 0=both 1=ignore 2=only |
| `kx_hitboxes_show_others` | Hitboxes: also draw the hitboxes for other players |
| `kx_hitboxes_targets_filter` | Hitboxes targets filter IDs (empty or no valid IDs = off) |
| `kx_hitboxes_targets_filter_mode` | Hitboxes targets filter: 0=both 1=only 2=ignore |
| `kx_hitboxes_team_filter` | Hitboxes team filter: 0=both 1=my 2=war 3=blue 4=red |
| `kx_hitboxes_tee_tile` | Hitboxes: show the tee tile-collision hitbox (28x28 square) on the active dummy |
| `kx_hitboxes_tee_trigger` | Hitboxes: show the tee trigger hitbox (round, radius 28 - lasers/shotgun/hook) on the active dummy |
| `kx_hitboxes_tile_speed` | Hitboxes dotted box animation speed for the tile hitbox (0-100, only for Dotted Box) |
| `kx_hitboxes_tile_style` | Hitboxes tile style: 0=Filled 1=Box 2=Dotted Box |
| `kx_hitboxes_trigger_speed` | Hitboxes dotted box animation speed for the trigger hitbox (0-100, only for Dotted Box) |
| `kx_hitboxes_trigger_style` | Hitboxes trigger style: 0=Filled 1=Box 2=Dotted Box |
| `kx_hook_dist` | Hook distance (pixels, 64-400) |
| `kx_irc_enabled` | Enable IRC emote communication and decoding |
| `kx_irc_nameplate_tag` | Show client tag in player nameplates if IRC user detected |
| `kx_irc_reveal_join` | Send handshake on join to reveal yourself as IRC user |
| `kx_laser_unfreeze` | Laser unfreeze: predict freeze and auto-fire laser to unfreeze self |
| `kx_laser_unfreeze_angles` | Laser unfreeze: number of raycast angles to test (1-144) |
| `kx_laser_unfreeze_auto` | Laser unfreeze: auto switch to laser weapon before firing |
| `kx_laser_unfreeze_fov` | Laser unfreeze: field of view in degrees (5-360) |
| `kx_laser_unfreeze_show_attempt` | Laser unfreeze: render successful laser path as blue line with 5s fade |
| `kx_laser_unfreeze_silent` | Laser unfreeze: silent mode — don't change player's crosshair, only send aim to server |
| `kx_laser_unfreeze_ticks` | Laser unfreeze: ticks to predict for raycast simulation (1-50) |
| `kx_laser_unfreeze_trigger_ticks` | Laser unfreeze: ticks to predict for freeze trigger (1-5) |
| `kx_line_aimbot_alpha` | AimBot line opacity |
| `kx_line_aimbot_gradient` | AimBot line gradient (visible only with Rainbow) |
| `kx_line_aimbot_gradient_step` | AimBot line gradient hue step per segment |
| `kx_line_aimbot_rainbow` | AimBot line rainbow mode |
| `kx_line_aimbot_rainbow_speed` | AimBot line rainbow speed (display = value/10) |
| `kx_line_aimbot_size` | AimBot line width |
| `kx_line_component` | Selected component for Line rendering panel (0=AimBot 1=TriggerBot 2=Trajectory 3=LaserUnfreeze 4=Pathfinder 5=ESP 6=TAS 7=Hitboxes) |
| `kx_line_esp_alpha` | ESP line opacity |
| `kx_line_esp_gradient` | ESP line gradient (visible only with Rainbow) |
| `kx_line_esp_gradient_step` | ESP line gradient hue step per segment |
| `kx_line_esp_rainbow` | ESP line rainbow mode |
| `kx_line_esp_rainbow_speed` | ESP line rainbow speed (display = value/10) |
| `kx_line_esp_size` | ESP line width |
| `kx_line_hitboxes_alpha` | Hitboxes line opacity |
| `kx_line_hitboxes_gradient` | Hitboxes line gradient (visible only with Rainbow) |
| `kx_line_hitboxes_gradient_step` | Hitboxes line gradient hue step per segment |
| `kx_line_hitboxes_rainbow` | Hitboxes line rainbow mode |
| `kx_line_hitboxes_rainbow_speed` | Hitboxes line rainbow speed (display = value/10) |
| `kx_line_hitboxes_size` | Hitboxes line width |
| `kx_line_laser_unfreeze_alpha` | Laser Unfreeze line opacity |
| `kx_line_laser_unfreeze_gradient` | Laser Unfreeze line gradient (visible only with Rainbow) |
| `kx_line_laser_unfreeze_gradient_step` | Laser Unfreeze line gradient hue step per segment |
| `kx_line_laser_unfreeze_rainbow` | Laser Unfreeze line rainbow mode |
| `kx_line_laser_unfreeze_rainbow_speed` | Laser Unfreeze line rainbow speed (display = value/10) |
| `kx_line_laser_unfreeze_size` | Laser Unfreeze line width |
| `kx_line_pathfinder_alpha` | Pathfinder line opacity |
| `kx_line_pathfinder_gradient` | Pathfinder line gradient (visible only with Rainbow, overridden by Show Speed) |
| `kx_line_pathfinder_gradient_step` | Pathfinder line gradient hue step per segment |
| `kx_line_pathfinder_rainbow` | Pathfinder line rainbow mode |
| `kx_line_pathfinder_rainbow_speed` | Pathfinder line rainbow speed (display = value/10) |
| `kx_line_pathfinder_size` | Pathfinder line width |
| `kx_line_rendering_alpha` | Trajectory line opacity (0-100) |
| `kx_line_rendering_layer` | Line render layer: 0=behind foreground blocks, 1+=on top of blocks (deferred overlay pass) |
| `kx_line_rendering_size` | Trajectory line width in pixels (0=thin default) |
| `kx_line_rtpathfinder_alpha` | RT Pathfinder line opacity |
| `kx_line_rtpathfinder_gradient` | RT Pathfinder line gradient (visible only with Rainbow, overridden by Show Speed) |
| `kx_line_rtpathfinder_gradient_step` | RT Pathfinder line gradient hue step per segment |
| `kx_line_rtpathfinder_rainbow` | RT Pathfinder line rainbow mode |
| `kx_line_rtpathfinder_rainbow_speed` | RT Pathfinder line rainbow speed (display = value/10) |
| `kx_line_rtpathfinder_size` | RT Pathfinder line width |
| `kx_line_tas_alpha` | TAS line opacity |
| `kx_line_tas_gradient` | TAS line gradient (visible only with Rainbow) |
| `kx_line_tas_gradient_step` | TAS line gradient hue step per segment |
| `kx_line_tas_rainbow` | TAS line rainbow mode |
| `kx_line_tas_rainbow_speed` | TAS line rainbow speed (display = value/10) |
| `kx_line_tas_size` | TAS line width |
| `kx_line_trajectory_alpha` | Trajectory line opacity |
| `kx_line_trajectory_gradient` | Trajectory line gradient (visible only with Rainbow) |
| `kx_line_trajectory_gradient_step` | Trajectory line gradient hue step per segment |
| `kx_line_trajectory_rainbow` | Trajectory line rainbow mode |
| `kx_line_trajectory_rainbow_speed` | Trajectory line rainbow speed (display = value/10) |
| `kx_line_trajectory_size` | Trajectory line width |
| `kx_line_triggerbot_alpha` | TriggerBot line opacity |
| `kx_line_triggerbot_gradient` | TriggerBot line gradient (visible only with Rainbow) |
| `kx_line_triggerbot_gradient_step` | TriggerBot line gradient hue step per segment |
| `kx_line_triggerbot_rainbow` | TriggerBot line rainbow mode |
| `kx_line_triggerbot_rainbow_speed` | TriggerBot line rainbow speed (display = value/10) |
| `kx_line_triggerbot_size` | TriggerBot line width |
| `kx_main` | Main target ID (-1 = none) |
| `kx_main_dist` | Main distance (pixels, 100000=infinity) |
| `kx_main_stand_dist` | Main stand distance (pixels) |
| `kx_panels_dynamic_scale` | ClickGUI: dynamic panel scale - drag edges to resize (beta) |
| `kx_panels_height` | ClickGUI: panels body max height (px) |
| `kx_panels_indent_x` | ClickGUI: panels X indentation (px) |
| `kx_panels_indent_y` | ClickGUI: panels Y indentation (px) |
| `kx_panels_start_x` | ClickGUI: panels start X (px) |
| `kx_panels_start_y` | ClickGUI: panels start Y (px) |
| `kx_panels_width` | ClickGUI: panels width (px) |
| `kx_pf_algorithm` | Pathfinder algorithm: 0=BFS (per-tick A*) 1=Beam 2=RHEA 3=BMCTS |
| `kx_pf_astar_node_budget` | Pathfinder: A* max nodes before giving up (1000-100000000) |
| `kx_pf_astar_weight` | Pathfinder: A* heuristic weight x100 (100 = 1.00 optimal, higher = faster but suboptimal) |
| `kx_pf_beam_width` | Pathfinder Beam: max children kept per expansion (Custom effort uses this value) |
| `kx_pf_effort` | Pathfinder effort: 0=Very Low 1=Low 2=Medium(default) 3=High 4=Ultra 5=Max 6=Custom. Presets override chunk size / hook angles / candidates / search budget and score/penalty toggles at READ time; Custom uses the stored values. |
| `kx_pf_freedom` | Freedom Path: FMM flow field prefers tiles far from walls/freeze/teleports (master toggle, see kx_pf_freedom_ratio) |
| `kx_pf_freedom_ratio` | Freedom Path: how aggressively the FMM flow field prefers tiles far from walls/freeze/teleports (ratio x10: 10 = 1.0x default) |
| `kx_pf_freeze_support` | Pathfinder freeze support (beta): ON allows phase-2 route analysis through freeze tiles and shows the Freeze penalty toggle. OFF rejects ANY candidate that touches freeze for even 1 tick. |
| `kx_pf_perf` | Pathfinder: performance budget per tick in % (1-100; lower = slower but smoother, spreads chunk generation across many ticks) |
| `kx_pf_rhea_generations` | Pathfinder RHEA: number of evolution generations per chunk (more = better candidates but slower) |
| `kx_pf_show_branches` | Visuals: show other branches (semi-transparent) |
| `kx_pf_show_field` | Visuals: show flow field |
| `kx_pf_show_hooks` | Visuals: show hook lines (blue) |
| `kx_pf_show_speed` | Visuals: show speed gradient (red=slow, blue=fast) |
| `kx_pf_tunnel_size` | Pathfinder tunnel size 0-10, free corridor half-width around the FMM path |
| `kx_rainbow_emoticons` | Visuals: rainbow emoticons (master) |
| `kx_rainbow_emoticons_alpha` | Visuals: emoticons opacity % |
| `kx_rainbow_emoticons_anim` | Visuals: emoticons rainbow animation |
| `kx_rainbow_emoticons_speed` | Visuals: rainbow emoticons speed x10 |
| `kx_rainbow_ent` | Visuals: rainbow entities (master) |
| `kx_rainbow_ent_alpha` | Visuals: entities opacity % |
| `kx_rainbow_ent_anim` | Visuals: entities rainbow animation |
| `kx_rainbow_ent_death` | Visuals: death entities custom color (enable) |
| `kx_rainbow_ent_death_alpha` | Visuals: death entities opacity % |
| `kx_rainbow_ent_death_anim` | Visuals: death entities rainbow animation |
| `kx_rainbow_ent_death_speed` | Visuals: death entities rainbow speed x10 |
| `kx_rainbow_ent_freeze` | Visuals: freeze entities custom color (enable) |
| `kx_rainbow_ent_freeze_alpha` | Visuals: freeze entities opacity % |
| `kx_rainbow_ent_freeze_anim` | Visuals: freeze entities rainbow animation |
| `kx_rainbow_ent_freeze_speed` | Visuals: freeze entities rainbow speed x10 |
| `kx_rainbow_ent_hookable` | Visuals: hookable entities custom color (enable) |
| `kx_rainbow_ent_hookable_alpha` | Visuals: hookable entities opacity % |
| `kx_rainbow_ent_hookable_anim` | Visuals: hookable entities rainbow animation |
| `kx_rainbow_ent_hookable_speed` | Visuals: hookable entities rainbow speed x10 |
| `kx_rainbow_ent_others` | Visuals: other entities custom color (enable) |
| `kx_rainbow_ent_others_alpha` | Visuals: other entities opacity % |
| `kx_rainbow_ent_others_anim` | Visuals: other entities rainbow animation |
| `kx_rainbow_ent_others_speed` | Visuals: other entities rainbow speed x10 |
| `kx_rainbow_ent_speed` | Visuals: rainbow entities speed x10 |
| `kx_rainbow_ent_unhookable` | Visuals: unhookable entities custom color (enable) |
| `kx_rainbow_ent_unhookable_alpha` | Visuals: unhookable entities opacity % |
| `kx_rainbow_ent_unhookable_anim` | Visuals: unhookable entities rainbow animation |
| `kx_rainbow_ent_unhookable_speed` | Visuals: unhookable entities rainbow speed x10 |
| `kx_rainbow_guns` | Visuals: rainbow guns (master) |
| `kx_rainbow_guns_alpha` | Visuals: guns opacity % |
| `kx_rainbow_guns_anim` | Visuals: guns rainbow animation |
| `kx_rainbow_guns_speed` | Visuals: rainbow guns speed x10 |
| `kx_rainbow_hook` | Visuals: rainbow hook (master) |
| `kx_rainbow_hook_alpha` | Visuals: hook opacity % |
| `kx_rainbow_hook_anim` | Visuals: hook rainbow animation |
| `kx_rainbow_hook_speed` | Visuals: rainbow hook speed x10 |
| `kx_rainbow_hud` | Visuals: rainbow HUD (master) |
| `kx_rainbow_hud_alpha` | Visuals: HUD opacity % |
| `kx_rainbow_hud_anim` | Visuals: HUD rainbow animation |
| `kx_rainbow_hud_speed` | Visuals: rainbow HUD speed x10 |
| `kx_rainbow_legs` | Visuals: rainbow tee legs (master) |
| `kx_rainbow_legs_alpha` | Visuals: legs opacity % |
| `kx_rainbow_legs_anim` | Visuals: legs rainbow animation |
| `kx_rainbow_legs_speed` | Visuals: rainbow legs speed x10 |
| `kx_rainbow_particles` | Visuals: rainbow particles (master) |
| `kx_rainbow_particles_alpha` | Visuals: particles opacity % |
| `kx_rainbow_particles_anim` | Visuals: particles rainbow animation |
| `kx_rainbow_particles_speed` | Visuals: rainbow particles speed x10 |
| `kx_rainbow_tee` | Visuals: rainbow tee body (master) |
| `kx_rainbow_tee_alpha` | Visuals: tee opacity % |
| `kx_rainbow_tee_anim` | Visuals: tee rainbow animation |
| `kx_rainbow_tee_speed` | Visuals: rainbow tee speed x10 (1-100 = 0.1-10.0) |
| `kx_rescue` | Rescue frozen allies |
| `kx_rescue_all` | Rescue all (not just list) |
| `kx_rescue_radius` | Rescue radius (pixels) |
| `kx_rt_pf_future_limit` | RT Pathfinder: enable the future ticks limit (off = the worker plans as far ahead as the solver needs) |
| `kx_rt_pf_future_ticks` | RT Pathfinder: future ticks limit (the worker keeps this many unreproduced ticks planned) |
| `kx_rt_pf_reconstruct` | RT Pathfinder: reconstruct the path when the real position deviates from the plan |
| `kx_rt_pf_show_field` | RT Pathfinder visuals: show flow field |
| `kx_rt_pf_show_hooks` | RT Pathfinder visuals: show hook lines (blue) |
| `kx_rt_pf_show_path` | RT Pathfinder visuals: show generated path (already passed part is trimmed) |
| `kx_rt_pf_show_prev_path` | RT Pathfinder visuals: show already reproduced path (half opacity) |
| `kx_rt_pf_show_speed` | RT Pathfinder visuals: speed gradient (affects Show Path and Show Previous Path) |
| `kx_rt_pf_trigger` | RT Pathfinder: reconstruct trigger in units (1-32) |
| `kx_sds_dummy_ids` | Smart dummy switch: comma-separated dummy ids (0-7) |
| `kx_sds_mode` | Smart dummy switch mode: 0=loop all connected dummies, 1=order targets |
| `kx_show_trajectory` | Show trajectory prediction for active player |
| `kx_spoof_bestclient_extra_netmsgs` | Send BestClient extra identification NETMSGs on connect (BestClient preset only) |
| `kx_spoof_client_name` | Client name to spoof (replaces GAME_NAME in DDNetVersionStr) |
| `kx_spoof_client_preset` | Client preset: 0=DDNet, 1=TClient, 2=BestClient, 3=Custom |
| `kx_spoof_client_str` | Spoof client name/version string sent to server |
| `kx_spoof_client_version` | Client version string to spoof (replaces GAME_RELEASE_VERSION). v1.56.174: bumped 16->32 to fit '2.1.1 stable-beta' (17 chars + null = 18). |
| `kx_spoof_git_hash` | Git hash to spoof (empty = use real, 'none' = hide). v1.56.174: bumped 24->40 to fit 32-char BestClient hash (--short=32 format). |
| `kx_spoof_git_hash_enabled` | Enable custom git hash spoof |
| `kx_spoof_spectate` | Spoof Spectate: send fake spectate data to the server |
| `kx_spoof_spectate_follow_id` | Spoof Spectate: fake follow client ID (Follow Player) |
| `kx_spoof_spectate_mode` | Spoof Spectate mode: 0=Follow Coordinates, 1=Follow Player |
| `kx_spoof_spectate_x` | Spoof Spectate: fake spectate X (Follow Coordinates) |
| `kx_spoof_spectate_y` | Spoof Spectate: fake spectate Y (Follow Coordinates) |
| `kx_spoof_tclient_extra_netmsgs` | Send TClient extra identification NETMSGs on connect (TClient preset only) |
| `kx_spoof_zoom` | Spoof Zoom: send a fake zoom value to the server (CameraInfo) |
| `kx_spoof_zoom_value` | Spoof Zoom: fake zoom level x10 (1.0-50.0 step 0.1; inverted: 1.0 = zoom out, 50.0 = zoom in, 10.0 = default view) |
| `kx_stand_dist` | Stand distance (pixels) |
| `kx_target_all` | Attack all (targets become blacklist): 1=on 0=off |
| `kx_target_dist` | Target distance (pixels) |
| `kx_tas_auto_rf` | TAS Auto RF: master toggle (Record mode only) |
| `kx_tas_auto_rf_d_mode` | TAS Auto RF Death: 0=Rewind 1=Forward |
| `kx_tas_auto_rf_d_ticks` | TAS Auto RF Death: action ticks (0=off) |
| `kx_tas_auto_rf_f_mode` | TAS Auto RF Freeze: 0=Rewind 1=Forward |
| `kx_tas_auto_rf_f_ticks` | TAS Auto RF Freeze: action ticks (0=off) |
| `kx_tas_auto_rf_t_mode` | TAS Auto RF Teleport: 0=Rewind 1=Forward |
| `kx_tas_auto_rf_t_ticks` | TAS Auto RF Teleport: action ticks (0=off) |
| `kx_tas_fake_aim` | TAS Fake Aim: mask the rendered aim of the replaying tee |
| `kx_tas_fake_aim_mode` | TAS Fake Aim mode: 0=Real 1=Smooth 2=Random 3=Robot 4=Spin 5=Lag |
| `kx_tas_pause` | TAS: pause state (0=running, 1=paused, toggles) |
| `kx_tas_record` | TAS: record state (0=off, 1=recording, toggles) |
| `kx_tas_show_hooks` | TAS Visuals: draw hook segments along the saved path |
| `kx_tas_show_path` | TAS Visuals: draw the saved path polyline |
| `kx_tas_show_speed` | TAS Visuals: show speed labels along the saved path |
| `kx_tas_show_trajectory` | TAS Visuals: simulate and draw the upcoming trajectory of the replay |
| `kx_tb_grenade_auto_switch` | TriggerBot grenade: auto switch to this weapon when it has a target |
| `kx_tb_grenade_clanmate` | TriggerBot grenade: clanmate filter 0=both 1=ignore 2=only |
| `kx_tb_grenade_dummy` | TriggerBot grenade: dummy filter 0=both 1=ignore 2=only |
| `kx_tb_grenade_enabled` | TriggerBot grenade: 1=enabled |
| `kx_tb_grenade_fng_freeze` | TriggerBot grenade: treat ninja as frozen (FNG mode) |
| `kx_tb_grenade_fov` | TriggerBot grenade: FOV degrees (1-360) |
| `kx_tb_grenade_freeze` | TriggerBot grenade: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_tb_grenade_friend` | TriggerBot grenade: friend filter 0=both 1=ignore 2=only |
| `kx_tb_grenade_latency` | TriggerBot grenade: random latency ticks (0-10) |
| `kx_tb_grenade_off_while_freeze` | TriggerBot grenade: pause while you are frozen |
| `kx_tb_grenade_priority` | TriggerBot grenade: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_tb_grenade_radius` | TriggerBot grenade: max radius (1-2000) |
| `kx_tb_grenade_raycast_show` | TriggerBot grenade: use raycast for show rendering |
| `kx_tb_grenade_rules` | TriggerBot grenade: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_tb_grenade_targets_filter` | TriggerBot grenade: targets filter 0=both 1=only 2=ignore |
| `kx_tb_grenade_targets_ids` | TriggerBot grenade: targets filter IDs |
| `kx_tb_grenade_team` | TriggerBot grenade: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_tb_grenade_trigger` | TriggerBot grenade: trigger 0=fire 1=hook |
| `kx_tb_grenade_trigger_mode` | TriggerBot grenade: trigger mode 0=one_tick 1=hold 2=every_tick |
| `kx_tb_grenade_use_silent_ab` | TriggerBot grenade: zone follows the AimBot's silent aim while it is active |
| `kx_tb_hammer_auto_switch` | TriggerBot hammer: auto switch to this weapon when it has a target |
| `kx_tb_hammer_clanmate` | TriggerBot hammer: clanmate filter 0=both 1=ignore 2=only |
| `kx_tb_hammer_dummy` | TriggerBot hammer: dummy filter 0=both 1=ignore 2=only |
| `kx_tb_hammer_enabled` | TriggerBot hammer: 1=enabled |
| `kx_tb_hammer_fng_freeze` | TriggerBot hammer: treat ninja as frozen (FNG mode) |
| `kx_tb_hammer_fov` | TriggerBot hammer: FOV degrees (1-360) |
| `kx_tb_hammer_freeze` | TriggerBot hammer: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_tb_hammer_friend` | TriggerBot hammer: friend filter 0=both 1=ignore 2=only |
| `kx_tb_hammer_latency` | TriggerBot hammer: random latency ticks (0-10) |
| `kx_tb_hammer_off_while_freeze` | TriggerBot hammer: pause while you are frozen |
| `kx_tb_hammer_priority` | TriggerBot hammer: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_tb_hammer_radius` | TriggerBot hammer: max radius (1-2000) |
| `kx_tb_hammer_raycast_show` | TriggerBot hammer: use raycast for show rendering |
| `kx_tb_hammer_rules` | TriggerBot hammer: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_tb_hammer_targets_filter` | TriggerBot hammer: targets filter 0=both 1=only 2=ignore |
| `kx_tb_hammer_targets_ids` | TriggerBot hammer: targets filter IDs |
| `kx_tb_hammer_team` | TriggerBot hammer: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_tb_hammer_trigger` | TriggerBot hammer: trigger 0=fire 1=hook |
| `kx_tb_hammer_trigger_mode` | TriggerBot hammer: trigger mode 0=one_tick 1=hold 2=every_tick |
| `kx_tb_hammer_use_silent_ab` | TriggerBot hammer: zone follows the AimBot's silent aim while it is active |
| `kx_tb_laser_auto_switch` | TriggerBot laser: auto switch to this weapon when it has a target |
| `kx_tb_laser_clanmate` | TriggerBot laser: clanmate filter 0=both 1=ignore 2=only |
| `kx_tb_laser_dummy` | TriggerBot laser: dummy filter 0=both 1=ignore 2=only |
| `kx_tb_laser_enabled` | TriggerBot laser: 1=enabled |
| `kx_tb_laser_fng_freeze` | TriggerBot laser: treat ninja as frozen (FNG mode) |
| `kx_tb_laser_fov` | TriggerBot laser: FOV degrees (1-360) |
| `kx_tb_laser_freeze` | TriggerBot laser: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_tb_laser_friend` | TriggerBot laser: friend filter 0=both 1=ignore 2=only |
| `kx_tb_laser_latency` | TriggerBot laser: random latency ticks (0-10) |
| `kx_tb_laser_off_while_freeze` | TriggerBot laser: pause while you are frozen |
| `kx_tb_laser_priority` | TriggerBot laser: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_tb_laser_radius` | TriggerBot laser: max radius (1-2000) |
| `kx_tb_laser_raycast_show` | TriggerBot laser: use raycast for show rendering |
| `kx_tb_laser_rules` | TriggerBot laser: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_tb_laser_targets_filter` | TriggerBot laser: targets filter 0=both 1=only 2=ignore |
| `kx_tb_laser_targets_ids` | TriggerBot laser: targets filter IDs |
| `kx_tb_laser_team` | TriggerBot laser: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_tb_laser_trigger` | TriggerBot laser: trigger 0=fire 1=hook |
| `kx_tb_laser_trigger_mode` | TriggerBot laser: trigger mode 0=one_tick 1=hold 2=every_tick |
| `kx_tb_laser_use_silent_ab` | TriggerBot laser: zone follows the AimBot's silent aim while it is active |
| `kx_tb_ninja_auto_switch` | TriggerBot ninja: auto switch to this weapon when it has a target |
| `kx_tb_ninja_clanmate` | TriggerBot ninja: clanmate filter 0=both 1=ignore 2=only |
| `kx_tb_ninja_dummy` | TriggerBot ninja: dummy filter 0=both 1=ignore 2=only |
| `kx_tb_ninja_enabled` | TriggerBot ninja: 1=enabled |
| `kx_tb_ninja_fng_freeze` | TriggerBot ninja: treat ninja as frozen (FNG mode) |
| `kx_tb_ninja_fov` | TriggerBot ninja: FOV degrees (1-360) |
| `kx_tb_ninja_freeze` | TriggerBot ninja: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_tb_ninja_friend` | TriggerBot ninja: friend filter 0=both 1=ignore 2=only |
| `kx_tb_ninja_latency` | TriggerBot ninja: random latency ticks (0-10) |
| `kx_tb_ninja_off_while_freeze` | TriggerBot ninja: pause while you are frozen |
| `kx_tb_ninja_priority` | TriggerBot ninja: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_tb_ninja_radius` | TriggerBot ninja: max radius (1-2000) |
| `kx_tb_ninja_raycast_show` | TriggerBot ninja: use raycast for show rendering |
| `kx_tb_ninja_rules` | TriggerBot ninja: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_tb_ninja_targets_filter` | TriggerBot ninja: targets filter 0=both 1=only 2=ignore |
| `kx_tb_ninja_targets_ids` | TriggerBot ninja: targets filter IDs |
| `kx_tb_ninja_team` | TriggerBot ninja: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_tb_ninja_trigger` | TriggerBot ninja: trigger 0=fire 1=hook |
| `kx_tb_ninja_trigger_mode` | TriggerBot ninja: trigger mode 0=one_tick 1=hold 2=every_tick |
| `kx_tb_ninja_use_silent_ab` | TriggerBot ninja: zone follows the AimBot's silent aim while it is active |
| `kx_tb_pistol_auto_switch` | TriggerBot pistol: auto switch to this weapon when it has a target |
| `kx_tb_pistol_clanmate` | TriggerBot pistol: clanmate filter 0=both 1=ignore 2=only |
| `kx_tb_pistol_dummy` | TriggerBot pistol: dummy filter 0=both 1=ignore 2=only |
| `kx_tb_pistol_enabled` | TriggerBot pistol: 1=enabled |
| `kx_tb_pistol_fng_freeze` | TriggerBot pistol: treat ninja as frozen (FNG mode) |
| `kx_tb_pistol_fov` | TriggerBot pistol: FOV degrees (1-360) |
| `kx_tb_pistol_freeze` | TriggerBot pistol: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_tb_pistol_friend` | TriggerBot pistol: friend filter 0=both 1=ignore 2=only |
| `kx_tb_pistol_latency` | TriggerBot pistol: random latency ticks (0-10) |
| `kx_tb_pistol_off_while_freeze` | TriggerBot pistol: pause while you are frozen |
| `kx_tb_pistol_priority` | TriggerBot pistol: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_tb_pistol_radius` | TriggerBot pistol: max radius (1-2000) |
| `kx_tb_pistol_raycast_show` | TriggerBot pistol: use raycast for show rendering |
| `kx_tb_pistol_rules` | TriggerBot pistol: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_tb_pistol_targets_filter` | TriggerBot pistol: targets filter 0=both 1=only 2=ignore |
| `kx_tb_pistol_targets_ids` | TriggerBot pistol: targets filter IDs |
| `kx_tb_pistol_team` | TriggerBot pistol: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_tb_pistol_trigger` | TriggerBot pistol: trigger 0=fire 1=hook |
| `kx_tb_pistol_trigger_mode` | TriggerBot pistol: trigger mode 0=one_tick 1=hold 2=every_tick |
| `kx_tb_pistol_use_silent_ab` | TriggerBot pistol: zone follows the AimBot's silent aim while it is active |
| `kx_tb_shotgun_auto_switch` | TriggerBot shotgun: auto switch to this weapon when it has a target |
| `kx_tb_shotgun_clanmate` | TriggerBot shotgun: clanmate filter 0=both 1=ignore 2=only |
| `kx_tb_shotgun_dummy` | TriggerBot shotgun: dummy filter 0=both 1=ignore 2=only |
| `kx_tb_shotgun_enabled` | TriggerBot shotgun: 1=enabled |
| `kx_tb_shotgun_fng_freeze` | TriggerBot shotgun: treat ninja as frozen (FNG mode) |
| `kx_tb_shotgun_fov` | TriggerBot shotgun: FOV degrees (1-360) |
| `kx_tb_shotgun_freeze` | TriggerBot shotgun: freeze filter 0=both 1=freeze 2=no_freeze |
| `kx_tb_shotgun_friend` | TriggerBot shotgun: friend filter 0=both 1=ignore 2=only |
| `kx_tb_shotgun_latency` | TriggerBot shotgun: random latency ticks (0-10) |
| `kx_tb_shotgun_off_while_freeze` | TriggerBot shotgun: pause while you are frozen |
| `kx_tb_shotgun_priority` | TriggerBot shotgun: priority 0=nearest_fov 1=nearest_aim 2=nearest_to_me |
| `kx_tb_shotgun_radius` | TriggerBot shotgun: max radius (1-2000) |
| `kx_tb_shotgun_raycast_show` | TriggerBot shotgun: use raycast for show rendering |
| `kx_tb_shotgun_rules` | TriggerBot shotgun: rules 0=none 1=insert_line 2=insert_line+raycast |
| `kx_tb_shotgun_targets_filter` | TriggerBot shotgun: targets filter 0=both 1=only 2=ignore |
| `kx_tb_shotgun_targets_ids` | TriggerBot shotgun: targets filter IDs |
| `kx_tb_shotgun_team` | TriggerBot shotgun: team filter 0=both 1=my 2=war 3=blue 4=red |
| `kx_tb_shotgun_trigger` | TriggerBot shotgun: trigger 0=fire 1=hook |
| `kx_tb_shotgun_trigger_mode` | TriggerBot shotgun: trigger mode 0=one_tick 1=hold 2=every_tick |
| `kx_tb_shotgun_use_silent_ab` | TriggerBot shotgun: zone follows the AimBot's silent aim while it is active |
| `kx_traj_grenade_for_current` | Trajectory grenade: show for current |
| `kx_traj_grenade_for_others` | Trajectory grenade: show for other players |
| `kx_traj_grenade_gradient` | Trajectory grenade: alpha gradient |
| `kx_traj_grenade_show` | Trajectory grenade: show (per-type) |
| `kx_traj_grenade_simulate` | Trajectory grenade: simulate players |
| `kx_traj_grenade_ticks` | Trajectory grenade: prediction ticks (1-50) |
| `kx_traj_laser_for_current` | Trajectory laser: show for current |
| `kx_traj_laser_for_others` | Trajectory laser: show for other players |
| `kx_traj_laser_gradient` | Trajectory laser: alpha gradient |
| `kx_traj_laser_show` | Trajectory laser: show (per-type) |
| `kx_traj_laser_simulate` | Trajectory laser: simulate players |
| `kx_traj_laser_ticks` | Trajectory laser: prediction ticks (1-50) |
| `kx_traj_pistol_for_current` | Trajectory pistol: show for current |
| `kx_traj_pistol_for_others` | Trajectory pistol: show for other players |
| `kx_traj_pistol_gradient` | Trajectory pistol: alpha gradient |
| `kx_traj_pistol_show` | Trajectory pistol: show (per-type) |
| `kx_traj_pistol_simulate` | Trajectory pistol: simulate players |
| `kx_traj_pistol_ticks` | Trajectory pistol: prediction ticks (1-50) |
| `kx_traj_shotgun_for_current` | Trajectory shotgun: show for current |
| `kx_traj_shotgun_for_others` | Trajectory shotgun: show for other players |
| `kx_traj_shotgun_gradient` | Trajectory shotgun: alpha gradient |
| `kx_traj_shotgun_show` | Trajectory shotgun: show (per-type) |
| `kx_traj_shotgun_simulate` | Trajectory shotgun: simulate players |
| `kx_traj_shotgun_ticks` | Trajectory shotgun: prediction ticks (1-50) |
| `kx_traj_tee_for_current` | Trajectory tee: show for current |
| `kx_traj_tee_for_others` | Trajectory tee: show for other players |
| `kx_traj_tee_gradient` | Trajectory tee: alpha gradient |
| `kx_traj_tee_show` | Trajectory tee: show (per-type) |
| `kx_traj_tee_simulate` | Trajectory tee: simulate players |
| `kx_traj_tee_ticks` | Trajectory tee: prediction ticks (1-50) |
| `kx_trajectory_ticks` | Number of ticks to predict trajectory (1-50) |
| `kx_triggerbot` | TriggerBot: 1=on 0=off |
| `kx_triggerbot_raycast_show` | TriggerBot: use raycast for show rendering |
| `kx_triggerbot_show_fov` | TriggerBot: show FOV |
| `kx_triggerbot_show_radius` | TriggerBot: show radius |
| `kx_triple_fly` | Auto Triple Fly: acting dummy auto-hooks a target that enters trigger radius and holds hook while farther than radius |
| `kx_triple_fly_dummy_id` | Auto Triple Fly: acting dummy index when dummy mode = ID (0 = main, 1-127 = dummies) |
| `kx_triple_fly_dummy_mode` | Auto Triple Fly: acting dummy selection (0 = closest to active dummy, 1 = by ID) |
| `kx_triple_fly_radius` | Auto Triple Fly: hold radius (units) — hook releases when target is closer than this |
| `kx_triple_fly_target_ids` | Auto Triple Fly: comma-separated server IDs of targets when target mode = IDs |
| `kx_triple_fly_target_mode` | Auto Triple Fly: target selection (0 = closest to acting dummy, 1 = closest to active dummy aim, 2 = server IDs) |
| `kx_triple_fly_trigger_radius` | Auto Triple Fly: start radius (units) around acting dummy — function engages when target enters it |
| `kx_ui_accent_color` | ClickGUI accent color (hex RGB) |
| `kx_ui_bg_blur` | ClickGUI: blur the game behind the panels |
| `kx_ui_bg_blur_radius` | ClickGUI: background blur radius (0-100) |
| `kx_ui_blackout` | ClickGUI: background blackout percent (0-100) |
| `kx_ui_panel_color` | ClickGUI panel color (hex RGB) |
| `kx_ui_rainbow` | ClickGUI: rainbow accent color animation |
| `kx_ui_rainbow_speed` | ClickGUI: rainbow speed x10 (1-100 = 0.1-10.0) |
| `kx_ui_settings_on` | ClickGUI: apply UI settings (master; 0 = built-in defaults) |
| `kx_version_spoof` | Spoof DDNet client version number |
| `kx_version_spoof_id` | DDNet version number to spoof (10000-21000) |
| `kx_zoom_unlock` | Visuals: bypass zoom lock (always allow zoom) |

---

## Files

The Kinetix source lives in [`src/game/client/components/kinetix`](src/game/client/components/kinetix). CVars are declared in [`src/engine/shared/config_variables.h`](src/engine/shared/config_variables.h); the per-dummy profile/proxy state is persisted to `kinetix_dummies.cfg` in the save folder.

---

## FAQ

**Where do I open the ClickGUI?**
Bind a key: `bind x toggle cl_clickgui 1 0`, or type `cl_clickgui 1` in the console.

**How do I add more dummies?**
Use the **Dummies** slider in the Dummy Manager panel, or `kx_dummy_limit <n>` (1–128).

**How do I route dummies through proxies?**
Set `dummy<N>_proxy user:pass@host:port` and `dummy<N>_proxy_on 1`, or use the **Proxy manager** panel, then press **Reconnect (Apply)**.

**How can I write my own features?**
Use [Code Execution](#code-execution) (`kx_exec` / the Lua panel).

---

**Enjoy! =D**
