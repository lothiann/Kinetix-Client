-- NAME: Functions (API Reference)
-- VERSION: 1.0
-- AUTHOR: Kinetix
-- DESCRIPTION: Full Lua API reference for code_exec scripts - globals, tables and functions with short descriptions. Runs once and prints a demo summary to the console.

-- pid - Player ID (0..MAX_CLIENTS-1, index into gc.m_aClients)
-- cmd - Command
-- msg - Message

-- ================= GLOBALS =================
-- gc          - CGameClient: game state (camera, controls, clients, tuning, components)
-- cl          - IClient: connection, ticks, time, dummy control
-- config      - CConfig: client config variables (bound subset)
-- console     - client console
-- gfx         - graphics
-- sound       - sounds
-- input       - keyboard and mouse
-- text        - text rendering
-- CONST       - game constants (teams, weapons, hook states, core events)
-- vec2(x, y)  - 2D vector constructor

-- ================= console =================
-- console.print(msg)              - print to client console
-- console.execute(cmd)            - execute client console command
-- console.execute_file(path)      - execute config file

-- ================= cl (client) =================
-- cl.Connect(addr)                - connect to server
-- cl.Disconnect()                 - disconnect from server
-- cl.EnterGame()                  - enter game
-- cl.RconAuth(user, pass)         - rcon login
-- cl.Rcon(cmd)                    - send rcon command
-- cl.RconAuthed()                 - true if rcon logged in
-- cl.GameTick()                   - current game tick
-- cl.PrevGameTick()               - previous game tick
-- cl.PredGameTick()               - prediction game tick
-- cl.GameTickSpeed()              - ticks per second (50)
-- cl.IntraGameTick()              - 0..1 tick interpolation
-- cl.GameTickTime()               - game tick time
-- cl.GlobalTime() / cl.LocalTime() - global/local time
-- cl.RenderFrameTime()            - frame time (fraction of a second)
-- cl.PlayerName()                 - local player name
-- cl.State()                      - connection state (IClient::STATE_*)
-- cl.MapDownloadName() / cl.MapDownloadAmount() / cl.MapDownloadTotalsize() - map download
-- cl.DummyConnect(d) / cl.DummyDisconnect(d) - connect/disconnect dummy d
-- cl.DummyConnected(d) / cl.DummyAllowed() / cl.AnyDummyConnected() / cl.DummyCount()
-- cl.LastDummy() / cl.DummyName()
-- cl.ConnectionProblems()         - true if connection problems
-- cl.ErrorString() / cl.News() / cl.Points() / cl.ConnectAddress() / cl.PredictionTime()
-- cl.AutoScreenshot()             - take auto screenshot
-- cl.Notify(title, msg)           - system notification
-- cl.IsSixup()                    - true if 0.7 protocol
-- cl.Quit() / cl.Restart()        - exit / restart client

-- ================= config =================
-- config.m_ClDummy                - active dummy index (0/1)
-- config.m_ClDummyControl / m_ClDummyCopyMoves / m_ClDummyHook / m_ClDummyJump / m_ClDummyFire
-- config.m_ClDummyResetOnSwitch / m_ClDummyRestoreWeapon
-- config.m_KxAttack / m_KxCopyMoves / m_KxRandomAim / m_KxAtkPathfinder / m_KxFlyRide
-- config.m_KxDummyAim / m_KxDummyDirection / m_KxDummyCopyMovesFilter

-- ================= gfx =================
-- gfx.width() / gfx.height()      - screen size in px
-- gfx.window_width() / gfx.window_height() - window size
-- gfx.aspect()                    - screen aspect
-- gfx.map_screen(x0, y0, x1, y1)  - set custom projection rect
-- gfx.map_screen_to_world(cx, cy, [zoom]) - world projection (parallax 100, offset 0)
-- gfx.map_screen_to_interface(cx, cy, [zoom]) - SAME layer + zoom as the game, use for world overlays
-- gfx.texture_clear()             - reset texture state before quads/lines
-- gfx.draw_rect(x, y, w, h, r, g, b, a, [corners], [rounding])
-- gfx.draw_circle(cx, cy, r, [segments])
-- gfx.draw_line(x0, y0, x1, y1, r, g, b, a)
-- gfx.set_color(r, g, b, a)
-- gfx.blend_normal() / gfx.blend_additive()

-- ================= text =================
-- text.draw(x, y, size, str)      - draw text in current projection
-- text.width(size, str)           - text width
-- text.color(r, g, b, a) / text.outline_color(r, g, b, a)
-- text.bounding_box(size, str)    - returns {width, height}

-- ================= input =================
-- input.pressed(key)              - key held
-- input.pressed_this_frame(key)   - key pressed this frame
-- input.key_name(key) / input.find_key(name)
-- input.shift_pressed() / input.alt_pressed() / input.modifier_pressed()
-- input.mouse_pressed([idx])      - mouse button held
-- input.mouse_pos()               - mouse position (px)
-- input.clipboard_get() / input.clipboard_set(str)
-- Keys: input.KEY_W/A/S/D, KEY_SPACE, KEY_RETURN, KEY_ESCAPE, KEY_TAB,
--       KEY_LSHIFT/KEY_RSHIFT, KEY_LCTRL/KEY_RCTRL, KEY_UP/DOWN/LEFT/RIGHT,
--       KEY_DELETE, KEY_BACKSPACE, KEY_INSERT, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
--       KEY_F1..KEY_F12, KEY_MOUSE_1/2/3, KEY_MOUSE_WHEEL_UP/DOWN

-- ================= sound =================
-- sound.play(id, vol) / sound.play_at(id, vol, x, y)
-- sound.stop(id) / sound.stop_all() / sound.enabled()
-- sound.load(name)                - load .opus/.wv sample, returns id
-- sound.unload_sample(id) / sound.pause(id)
-- sound.sample_time(id) / sound.sample_total_time(id)
-- Channels: sound.CHN_GUI / CHN_MUSIC / CHN_WORLD / CHN_GLOBAL

-- ================= CONST =================
-- CONST.MAX_CLIENTS, CONST.SERVER_TICK_SPEED, CONST.NUM_WEAPONS, CONST.MAX_DUMMIES
-- CONST.TEAM_RED / TEAM_BLUE / TEAM_SPECTATORS
-- CONST.WEAPON_HAMMER / WEAPON_GUN / WEAPON_SHOTGUN / ...
-- CONST.HOOK_* (hook states), CONST.COREEVENT_* (core events)

-- ================= gc (CGameClient) =================
-- Camera:  gc.m_Camera.m_Center (vec2), gc.m_Camera.m_Zoom
-- Aim:     gc.m_Controls.m_aTargetPos[config.m_ClDummy] (vec2, 0-based table)
-- Clients: gc.m_aClients[i] (i = 0..MAX_CLIENTS-1, 0-based table, .m_Active + client fields)
-- Tuning:  gc.m_aReceivedTuning (table of CTuningParams)
-- Collision: gc:Collision()
--   GetTile(tx, ty), IsSolid(tx, ty), GetTileIndex, GetTileFlags, GetIndex, GetFrontIndex,
--   IsThrough, IsHookBlocker, IsFrontNoLaser, IsTeleCheckpoint, IsCheckTeleport, IsCheckEvilTeleport,
--   IsTeleportWeapon, IsTeleportHook, MoverSpeed, GetMoveRestrictions, IntersectAir, IntersectNoLaserNoWalls,
--   SetCollisionAt, SetDoorCollisionAt,
--   TeleOuts / TeleIns / TeleCheckOuts / TeleOthers (tile tables of vec2), TeleAllSize / TeleAllGet
-- Layers:  gc:Layers() - GameLayer() (m_Width/m_Height), GetGroup, GetLayer, NumGroups, NumLayers
-- Race:    gc:CurrentRaceTime() - race time in seconds
-- Misc:    gc:Config(), gc:GetItemName(id), gc:NetVersion(), gc:DDNetVersionStr(), gc:ClientVersion7(),
--          gc:Predict(), gc:PredictDummy()
-- Broadcast: gc.m_Broadcast:DoBroadcast(str)

-- ================= DEMO (runs once) =================
console.print("=== functions.lua demo ===")
console.print("player: " .. cl.PlayerName())
console.print("state: " .. cl.State() .. "  tick: " .. cl.GameTick())
console.print("dummy: " .. config.m_ClDummy .. "  dummy_allowed: " .. tostring(cl.DummyAllowed()))
console.print("screen: " .. gfx.width() .. "x" .. gfx.height())
console.print("camera: " .. string.format("%.0f,%.0f zoom %.2f", gc.m_Camera.m_Center.x, gc.m_Camera.m_Center.y, gc.m_Camera.m_Zoom))
console.print("max_clients: " .. CONST.MAX_CLIENTS .. "  weapon_gun: " .. CONST.WEAPON_GUN)
local n = 0
for i = 0, CONST.MAX_CLIENTS - 1 do
    local c = gc.m_aClients[i]
    if c and c.m_Active then n = n + 1 end
end
console.print("players: " .. n)
console.print("=== done, full API list is in the comments above ===")
