// (c) Kinetix. TAS component — input recording/playback inside an isolated
// client-side "playground" (a copy of the predicted world with all other
// players and entities removed). Panel: ClickGUI → "TAS".
//
// UI (prototype):
//   [dropdown] Mode: Play / Record
//   [button]   Play/Stop or Record/Stop (label depends on Mode)
//   [button]   Join Playground / Leave Playground
//   [slider]   TPS 1..50 — playground ticks per second
//   [toggle]   Pause — no automatic tick advance (Forward/Rewind still work)
//   [button]   Forward — emulate the next tick
//   [button]   Rewind — emulate the previous tick (restored from memory)
//
// Memory model:
//   m_vPlaygroundHistory + m_vPlaygroundInputs — "playground memory": a full physics
//   snapshot per tick, used by Rewind. Cleared only on Join/Leave — never
//   by Record/Play Stop.
//   m_vRecord — record memory: input + resulting state per tick. Cleared on
//   each new Record start.
//   m_vSaved — separate saved memory: a copy of m_vRecord made on Record
//   Stop. Play replays from this.
//
// Stored per tick: tick number, full input (direction/jump/fire/hook/aim/...),
// the character core (pos/vel/all hook stats/weapons/ninja/jumped), freeze
// time and every fake-world projectile (CProjectileData) — inputs + the
// whole game physics, enough for exact forward/backward tick emulation.

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_TAS_H
#define GAME_CLIENT_COMPONENTS_KINETIX_TAS_H

#include <base/math.h>
#include <base/vmath.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/laser_data.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/client/projectile_data.h>
#include <game/client/render.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct CPickupState
{
	vec2 Pos = vec2(0.0f, 0.0f);
	int Type = 0;
	int Subtype = 0;
	int Flags = 0;
	int SwitchNumber = 0;
};

// Full state of one weapon beam (rifle shots and the DDRace shotgun are both
// CLaser entities) for the rewind snapshot.
struct CLaserState
{
	CLaserData Data{};
	vec2 Dir = vec2(0.0f, 0.0f);
	vec2 PrevPos = vec2(0.0f, 0.0f);
	float Energy = 0.0f;
	int Bounces = 0;
	int Owner = 0;
	bool ZeroEnergyBounce = false;
};

struct SSpawnedEntSnapshot;

// Complete physics state of the playground at one tick.
struct CTasState
{
	int Tick = 0;
	CCharacterCore Core;
	// Full character snapshot: input queues, weapon switch state, hammer hit
	// list, tune zone, ... Without it a rewind leaks the presses of the undone
	// ticks (the fake tee swings/re-hits right after an auto-rewind).
	CCharacterFullState Char{};
	int FreezeTime = 0;
	int ReloadTimer = 0;
	int AttackTick = 0;
	// Latest direct input of the step that produced this state — the press
	// baseline (fire/weapon-switch counters) for the tick that follows.
	CNetObj_PlayerInput LatestInput{};
	vec2 PrevPos = vec2(0.0f, 0.0f);
	vec2 PrevPrevPos = vec2(0.0f, 0.0f);
	std::vector<CProjectileData> vProjectiles;
	std::vector<CLaserState> vLasers;
	std::vector<CPickupState> vPickups;
	// v6.3: state of every Spawner-owned entity at this tick (tees, spawner
	// projectiles and spawner lasers), so Rewind moves them back too.
	std::vector<SSpawnedEntSnapshot> vSpawnedEnts;
};

// One recorded tick: input used + physics state after the tick.
struct CTasTick
{
	int Tick = 0;
	CNetObj_PlayerInput Input{};
	CCharacterCore Core;
	int FreezeTime = 0;
	std::vector<CProjectileData> vProjectiles;
};

// ---------------------------------------------------------------------------
// Spawner (TAS playground only, .tas v2.0)
// ---------------------------------------------------------------------------

enum
{
	TAS_SPAWN_MODE_ENTITY = 0,
	TAS_SPAWN_MODE_TILE = 1,
};

// Spawnable entity kinds, in the order shown by the "Entity ID" slider.
enum
{
	TAS_SPAWN_ENT_TEE = 0,
	TAS_SPAWN_ENT_PROJECTILE,
	TAS_SPAWN_ENT_LASER,
	TAS_SPAWN_ENT_PICKUP,
	TAS_SPAWN_ENT_DOOR,
	TAS_SPAWN_ENT_DRAGGER,
	TAS_SPAWN_ENT_PLASMA,
	TAS_SPAWN_ENT_COUNT,
};

enum
{
	TAS_SPAWN_COORD_CURSOR = 0,
	TAS_SPAWN_COORD_COORDS = 1,
	TAS_SPAWN_COORD_PREVIOUS = 2,
};

// Number of entries in the ClickGUI tile slider (kept in sync with the table
// in tas.cpp by a static_assert there).
enum
{
	TAS_SPAWNER_TILE_COUNT = 39,
};

// Everything the Spawner panel can configure. The ClickGUI rows write into
// this struct every frame (see SyncTasSpawnerRows) and the engine reads it
// when spawning. Plain POD so it can be mem_copy'd into the .tas file.
struct STasSpawnerSettings
{
	int m_Mode = TAS_SPAWN_MODE_ENTITY;
	int m_Entity = TAS_SPAWN_ENT_TEE;
	// Tee-only fields.
	char m_aName[64] = "Spawned";
	char m_aClan[64] = "";
	int m_Id = 0;
	char m_aSkin[64] = "";
	char m_aColorBody[8] = "";
	char m_aColorFeet[8] = "";
	int m_HookStrength = 10;
	int m_HookStrengthMode = 1; // 0 = Strong, 1 = Weak
	// Weapon loadout: -1 = bare tee (no weapons at all), 0-5 = owns exactly
	// that weapon (0 hammer, 1 gun, 2 shotgun, 3 grenade, 4 laser, 5 ninja).
	int m_WeaponId = 0;
	bool m_PracticeParticles = false;
	// Entity physics / rewind helpers.
	bool m_EntityPhysics = true;
	float m_PrevPosX = 0.0f; // Tee only: delta X of the previous position (physics kick).
	float m_PrevPosY = 0.0f; // Tee only: delta Y of the previous position (physics kick).
	int m_EntityTimelineTick = 0; // Tee only: playground tick the tee is treated as spawned at.
	// Non-tee entity physics (v6.2): direction / velocity / lifetime for
	// projectiles, reach for lasers, length for doors.
	int m_Dir = 0; // direction preset index (0=E, 1=NE, ... 7=SE), see g_aTasSpawnerDirs
	float m_VelX = 0.0f; // extra velocity along the direction (units/tick)
	float m_VelY = 0.0f;
	int m_LifeSpan = -1; // projectile lifespan in ticks (-1 = default)
	int m_LaserReach = 800; // laser/dragger reach
	int m_LaserLength = 800; // door length (only the door entity uses it)
	int m_DraggerStrength = 1; // dragger pull strength
	// v6.3: per-entity parameters.
	bool m_ProjectileFreeze = false; // freeze projectiles
	bool m_ProjectileExplosive = false; // explosive projectiles (grenades are always explosive)
	bool m_ProjectileSound = true; // play the fire sound
	int m_LaserType = 0; // 0 = rifle, 1 = shotgun (freeze laser)
	int m_PickupType = 0; // 0 = health, 1 = armor, 2 = weapon, 3 = ninja
	int m_Subtype = 0; // pickup: weapon id / armor amount; plasma: 1 = explosive, 2 = freeze, 3 = both
	int m_SwitchNumber = 0; // door / dragger / plasma / pickup switch layer number (0 = always active)
	// v6.3: speedup tile parameters (speedup layer).
	int m_SpeedupForce = 50; // acceleration strength
	int m_SpeedupMaxSpeed = 0; // 0 = unlimited
	int m_SpeedupAngle = 0; // push direction in degrees (0 = right, 90 = down)
	// Tile.
	int m_Tile = 1; // TILE_SOLID
	int m_TileNumber = 0; // tile number: tune zone / tele number
	bool m_TilePhysics = true;
	// Placement.
	int m_CoordsMode = TAS_SPAWN_COORD_CURSOR;
	float m_X = 0.0f;
	float m_Y = 0.0f;
	bool m_Snap = false;
	int m_LoopDelay = 1000;
};

// One spawn action, recorded for the .tas file (v2.0).
struct STasSpawnerRecord
{
	int m_Tick = 0;
	int m_Mode = TAS_SPAWN_MODE_ENTITY;
	int m_Entity = TAS_SPAWN_ENT_TEE;
	int m_Tile = 0;
	bool m_Physics = true;
	vec2 m_Pos = vec2(0.0f, 0.0f);
	STasSpawnerSettings m_Settings{};
};

// v6.3: one Spawner-owned entity inside a rewind snapshot. Tees need their
// full core + freeze/prev-pos state; spawner projectiles and lasers carry
// their own data so the exact trajectory/beam is restored.
struct SSpawnedEntSnapshot
{
	int m_Entity = 0;
	// Client slot of a spawned tee, restored exactly (hooking/collision uses it).
	int m_Id = 0;
	int m_Tick = 0;
	vec2 m_Pos = vec2(0.0f, 0.0f);
	bool m_Inserted = false;
	STasSpawnerSettings m_Settings{};
	// Tee.
	CCharacterCore Core{};
	int FreezeTime = 0;
	int ReloadTimer = 0;
	int AttackTick = 0;
	vec2 PrevPos = vec2(0.0f, 0.0f);
	vec2 PrevPrevPos = vec2(0.0f, 0.0f);
	// Spawner projectile.
	CProjectileData ProjData{};
	// Spawner laser.
	CLaserState Laser{};
};

class CTas : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnReset() override;
	void OnUpdate() override;
	void OnRender() override;

	// UI wiring (the ClickGUI TAS panel owns these values).
	void SetTps(float Tps) { m_Tps = std::clamp(Tps, 1.0f, 50.0f); }
	void SetPaused(bool Paused) { m_Paused = Paused; }
	void SetMode(int Mode) { m_Mode = Mode; } // 0 = Play, 1 = Record
	float Tps() const { return m_Tps; }
	bool Paused() const { return m_Paused; }
	int Mode() const { return m_Mode; }

	// TAS Fake Aim (expandable in the panel, shown in Play mode only). While
	// active it masks the RENDERED aim of the replaying tee; the physics
	// input stays the recorded one. Persisted in the KxTasFakeAim* cvars.
	void SetFakeAimEnabled(bool On) { g_Config.m_KxTasFakeAim = On ? 1 : 0; }
	void SetFakeAimMode(int Mode) { g_Config.m_KxTasFakeAimMode = std::clamp(Mode, 0, 5); }
	bool FakeAimEnabled() const { return g_Config.m_KxTasFakeAim != 0; }
	int FakeAimMode() const { return g_Config.m_KxTasFakeAimMode; }
	bool FakeAimActive() const { return FakeAimEnabled() && (m_Playing || m_RealPlaying); }
	// Rendered aim offset for the current playback position; false = inactive.
	bool FakeAimOffset(vec2 *pOffset);

	// Auto Rewind/Forward (Record-mode convenience): when the fake tee hits a
	// configured tile, jump back/ahead by a fixed tick count. Per-tile mode
	// and tick count; 0 ticks disables the tile.
	enum
	{
		TAS_AUTOTILE_FREEZE = 0,
		TAS_AUTOTILE_TELEPORT = 1,
		TAS_AUTOTILE_DEATH = 2,
		TAS_AUTOTILE_COUNT = 3,
	};
	void SetAutoRfEnabled(bool On);
	bool AutoRfEnabled() const;
	void SetAutoRfTile(int Tile) { m_AutoRfTile = std::clamp(Tile, 0, TAS_AUTOTILE_COUNT - 1); }
	int AutoRfTile() const { return m_AutoRfTile; }
	void SetAutoRfMode(int Mode);
	int AutoRfMode() const;
	bool AutoRfForward() const;
	void SetAutoRfTicks(int Ticks);
	int AutoRfTicks() const;
	int AutoRfModeFor(int Tile) const;
	int AutoRfTicksFor(int Tile) const;

	// Status for labels/camera/render.
	bool IsJoined() const { return m_Joined; }
	bool IsPlaying() const { return m_Playing; }
	bool IsRecording() const { return m_Recording; }
	bool Desynced() const { return m_Desynced; }
	int PlaygroundTick() const { return m_PlaygroundTick; }
	int HistoryCount() const { return (int)m_vPlaygroundInputs.size(); }
	int RecordCount() const { return (int)m_vRecord.size(); }
	int SavedCount() const { return (int)m_vSaved.size(); }
	int PlayIdx() const { return m_PlayIdx; }
	bool FutureSavedInput(int Idx, CNetObj_PlayerInput *pOut) const;
	bool IsDead();
	vec2 PlaygroundTeePos();
	int PlaygroundWeapon();
	vec2 PlaygroundTeeRenderPos();
	// Playground freeze bar: called from CFreezeBars::OnRender so the bar renders
	// on the same layer as the real freeze bars.
	void RenderFreezeBar();
	// Spawned tele/speedup tile self-draw, rendered after m_MapLayersForeground
	// (called from CKinetixLines) so it stacks like native entity layers.
	void RenderSpawnedTilesOverlay();
	// Playground access for the Trajectory Tee prediction inside the playground.
	CGameWorld *Playground() { return &m_Playground; }
	int PlaygroundLocalId() const { return m_LocalId; }
	// HUD data from the fake tee: the full core + a snapshot-style character
	// (active weapon, ammo, position) for the HUD while in the playground.
	bool PlaygroundHudState(CCharacterCore *pCore, CNetObj_Character *pPlayer);

	// Real-world execution (Play outside the playground): the saved inputs
	// are fed to the real player through CControls::SnapInput.
	bool IsRealPlaying() const { return m_RealPlaying; }
	int RealPlayIdx() const { return m_RealIdx; }
	bool ApplyTasRealInput(CNetObj_PlayerInput *pInput);

	// Buttons.
	void JoinLeavePlayground();
	void TogglePlay();
	void ToggleRecord();
	void StepForward();
	void StepRewind();
	void PasteRun(const std::vector<CNetObj_PlayerInput> &vInputs, const std::vector<vec2> &vPath, const std::vector<std::pair<vec2, vec2>> &vHookSegs);
	bool HasRun(const std::vector<CNetObj_PlayerInput> &vInputs) const;

	bool FilePicked() const { return m_FilePicked; }
	const char *PickedPath() const { return m_aPickedPath; }
	const char *PickedMap() const { return m_aPickedMap; }
	const char *PickedName() const { return m_aPickedName; }
	const char *PickedAuthor() const { return m_aPickedAuthor; }
	const char *PickedCreated() const { return m_aPickedCreated; }
	void PickFile(const char *pPath);
	void UnpickFile();
	void SaveTake(const char *pMap, const char *pName, const char *pAuthor);

	// v6.3: "Reset Record" — clears the record/saved take and unloads the
	// picked .tas file. The playground world is NOT touched.
	bool RecordDirty() const { return !m_vRecord.empty() || !m_vSaved.empty() || m_FilePicked; }
	void ResetRecord();

	// ---- Spawner (TAS playground only) ----
	STasSpawnerSettings &SpawnerSettings() { return m_Spawner; }
	const STasSpawnerSettings &SpawnerSettings() const { return m_Spawner; }
	bool SpawnerSpawn();
	bool SpawnerKill();
	// Removes every spawned entity (or tile, in tile mode) of the current
	// spawner mode, including the records.
	void SpawnerKillAll();
	void SpawnerToggleLoop();
	bool SpawnerLoopActive() const { return m_SpawnerLoop; }
	bool SpawnerHasPrevious() const { return m_SpawnerHasPrev; }
	int SpawnerRecordCount() const { return (int)m_vSpawnerRecords.size(); }
	static int SpawnerEntityCount() { return TAS_SPAWN_ENT_COUNT; }
	static const char *SpawnerEntityName(int Index);
	static int SpawnerTileCount() { return TAS_SPAWNER_TILE_COUNT; }
	static const char *SpawnerTileName(int Index);
	static int SpawnerTileId(int Index);

	// ---- Playground settings (v6.2) ----
	// Virtual RCON / Virtual Chat / Practice: client-side rcon+chat inside the
	// playground. The buffers survive toggling and are saved in the .tas file.
	void SetPgVirtualRcon(bool On);
	bool PgVirtualRcon() const { return m_PgVirtualRcon; }
	void SetPgVirtualChat(bool On);
	bool PgVirtualChat() const { return m_PgVirtualChat; }
	void SetPgPractice(bool On);
	bool PgPractice() const { return m_PgPractice; }
	// One line of the playground chat buffer.
	struct SPgChatLine
	{
		int m_Team = 0; // 0 = all, 1 = team
		int m_ClientId = -1; // -1 = server/system line
		char m_aName[64] = "";
		char m_aText[512] = "";
	};
	// Appends a line to the virtual chat buffer (if Virtual Chat is on, the
	// real chat component routes here instead of the real buffer).
	void PgChatAddLine(int ClientId, int Team, const char *pName, const char *pText);
	const std::vector<SPgChatLine> &PgChatLines() const { return m_vPgChat; }
	void PgChatClear() { m_vPgChat.clear(); }
	// Handles a "/command" typed in the playground chat. Returns true when the
	// command was consumed here (pOut receives an optional status line).
	bool PgChatHandleCommand(const char *pLine, char *pOut, int OutSize);
	// Executes a line in the virtual rcon (practice commands like telecursor).
	// It never echoes the command itself — callers that do not print the line
	// (the rcon console command, the ClickGUI field) use PgRconEcho first.
	void PgRconExecute(const char *pLine);
	// Records "> line" in the backlog without printing (the F2 console already
	// printed it) / records and prints it (rcon command + GUI field).
	void PgRconLogCommand(const char *pLine);
	void PgRconEcho(const char *pLine);
	// Virtual rcon backlog ("> cmd" + responses), for the remote console.
	const std::vector<std::string> &PgRconLog() const { return m_vPgRcon; }
	// Master switch of the whole Playground Settings group.
	void SetPgMaster(bool On);
	bool PgMaster() const { return m_PgMaster; }
	// /spec (PAUSE_SPEC): the tee stays paused in the world and the camera is
	// detached to the spectator view.
	bool IsSpec() const { return m_PgSpec; }
	// Teleports the playground tee to the current spawn target (cursor).
	void PgTeleCursor();
	// Pass-through to the real server rcon (used by the shadowed console command).
	void SendRealRcon(const char *pLine);
	// True when the playground has spawner objects / recordings / chat that a
	// Reset would drop (drives the "Reset Playground" button visibility).
	bool PlaygroundDirty() const;
	// Drops everything the Spawner created (objects, tiles, records, loop) and
	// clears the virtual chat buffer — the playground becomes pristine again.
	void PlaygroundReset();
	// Incremented by PlaygroundReset so the ClickGUI can reset its rows.
	int PgResetSerial() const { return m_PgResetSerial; }

	static constexpr int MAX_HISTORY = 20000; // rewind depth cap (steps)
	static constexpr int TRIM_CHUNK = 5000; // entries dropped when the cap is hit

private:
	void JoinPlayground();
	void LeavePlayground();
	CNetObj_PlayerInput LiveInput();
	CTasState CaptureState();
	void RestoreState(const CTasState &State);
	void StepWorld(const CNetObj_PlayerInput &Input);
	void DoStep(const CNetObj_PlayerInput &Input);
	void AdvanceOne();
	void TrimHistory();
	void RebaseCounters();
	void DoAutoTiles(const vec2 &PosBefore);
	void TriggerAutoTile(int Tile);
	void RefreshAutoTileFlags();
	float RenderIntraAlpha() const;
	void RenderVisuals();
	bool LoadTakeFile(const char *pPath);
	bool WriteTakeFile(const char *pPath, const char *pMap, const char *pName, const char *pAuthor, const char *pCreated);

	// Playground command core (shared by the virtual chat and the virtual rcon).
	// Returns true when the command was understood; vOut receives the status
	// lines (multi-line commands like "tunes" print one line per tuning).
	bool PgRunCommand(const char *pCmdLine, std::vector<std::string> &vOut);
	void PgRconPrint(const char *pText);
	void PgRespawn();
	void PgGiveWeapon(int Weapon);

	// Spawner internals.
	bool SpawnerSpawnAt(const STasSpawnerSettings &Settings, vec2 Pos, int Tick, bool Record);
	bool SpawnerSpawnEntity(const STasSpawnerSettings &Settings, vec2 Pos, int Tick, bool Record);
	bool SpawnerSpawnTile(const STasSpawnerSettings &Settings, vec2 Pos, int Tick, bool Record);
	void SpawnerClearLive();
	// Leaving the playground destroys the world, but the Spawner state (settings,
	// spawned objects, tiles, loop) must survive it: detach the entities without
	// touching the records and re-create them on the next join.
	void SpawnerDetachLive();
	void SpawnerReattachLive();
	void SpawnerApplyRecords();
	// The playground deletes entities it destroys (death tiles, expired
	// projectiles); drop the ones that no longer exist so we never touch a
	// freed pointer or double-delete.
	void SpawnerPruneRemoved();
	bool SpawnerOwnsEntity(const CEntity *pEnt) const;
	void RenderSpawned();
	void RenderPlaygroundHookColl();
	vec2 SpawnerTargetPos();
	// The current aim (crosshair) point in world coordinates, regardless of
	// the spawner Coordinates Mode. Used by /tc (telecursor).
	vec2 SpawnerAimPos();
	// Render info of a spawned tee: honors the custom skin name (loaded like
	// any community skin) and the custom body/feet colors; falls back to the
	// local player's look.
	CTeeRenderInfo SpawnerTeeRenderInfo(const STasSpawnerSettings &S);

	struct SSpawnedEnt
	{
		CEntity *m_pEnt = nullptr;
		bool m_Inserted = false;
		int m_Entity = 0;
		int m_Tick = 0;
		vec2 m_Pos = vec2(0.0f, 0.0f);
		// Previous / current tick positions, tracked by the renderer so the
		// spawned tee glides between ticks instead of jumping.
		vec2 m_RenderPrevPos = vec2(0.0f, 0.0f);
		vec2 m_RenderCurPos = vec2(0.0f, 0.0f);
		CLaserData m_LaserData{};
		STasSpawnerSettings m_Settings{};
	};
	struct SSpawnedTile
	{
		int m_Index = -1;
		int m_OldTile = 0;
		int m_Tile = 0;
		int m_Number = 0; // tune zone (TILE_TUNE) / tele number (tele tiles)
		bool m_Physics = true;
		// Tune layer state (TILE_TUNE spawns rewrite the tune zone here).
		bool m_TuneSet = false;
		int m_OldTuneType = 0;
		int m_OldTuneNumber = 0;
		// Tele layer state (tele tile spawns). The tile is drawn by the TAS
		// renderer itself when the map has no tele layer.
		bool m_TeleSet = false;
		bool m_TeleSelfDraw = false;
		int m_OldTeleType = 0;
		int m_OldTeleNumber = 0;
		// Speedup layer state (TILE_SPEED_BOOST spawns). The arrow is drawn by
		// the TAS renderer itself when the map has no speedup layer.
		bool m_SpeedupSet = false;
		bool m_SpeedupSelfDraw = false;
		int m_OldSpeedupType = 0;
		int m_OldSpeedupForce = 0;
		int m_OldSpeedupMaxSpeed = 0;
		int m_OldSpeedupAngle = 0;
		int m_Tick = 0;
		vec2 m_Pos = vec2(0.0f, 0.0f);
	};

	std::vector<SSpawnedEnt> m_vSpawnedEnts;
	// Cached managed skins for spawned tees, keyed by "skin|body|feet".
	std::map<std::string, std::shared_ptr<class CManagedTeeRenderInfo>> m_SpawnerSkinCache;
	std::vector<SSpawnedTile> m_vSpawnedTiles;
	std::vector<STasSpawnerRecord> m_vSpawnerRecords;
	STasSpawnerSettings m_Spawner;
	bool m_SpawnerHasPrev = false;
	vec2 m_SpawnerPrevPos = vec2(0.0f, 0.0f);
	bool m_SpawnerLoop = false;
	float m_SpawnerLoopMs = 0.0f;
	// Playground tick the spawned render positions were last refreshed for.
	int m_SpawnerRenderTick = -1;
	// Set whenever spawned tiles were written to (or restored in) the map's
	// game layer: the map renderer keeps pre-built buffers, so they must be
	// rebuilt for the change to be visible.
	bool m_TileVisualDirty = false;

	// ---- Playground settings state (v6.2) ----
	bool m_PgMaster = false;
	bool m_PgVirtualRcon = false;
	bool m_PgVirtualChat = false;
	bool m_PgPractice = false;
	// Practice cheats state.
	bool m_PgInvincible = false;
	bool m_PgSuper = false;
	// /pause and /spec: the player is paused (tee stands still), the world keeps
	// ticking — the engine's ConTogglePause semantics, not a world pause.
	bool m_PgPaused = false;
	// /spec (PAUSE_SPEC): the tee stays paused in the world and the camera is
	// detached to the spectator view.
	bool m_PgSpec = false;
	// True once m_aPgTuning has been seeded from the client tuning list. It is
	// reset on map change / Reset, so leaving and re-entering the playground
	// keeps every tune the user applied (v6.2.1 fix).
	bool m_PgTuningInit = false;
	bool m_PgTuningDirty = false;
	int m_PgResetSerial = 0;
	// Private tuning list for the playground: virtual rcon "tune" writes here
	// so the real world's tuning is never touched.
	CTuningParams m_aPgTuning[TuneZone::NUM];
	// Virtual chat buffer: written while Virtual Chat is ON, kept (frozen)
	// while OFF, saved/loaded with the .tas file.
	std::vector<SPgChatLine> m_vPgChat;
	// Virtual rcon backlog ("> line" + command echoes), shown in the remote
	// console while Virtual RCON is ON.
	std::vector<std::string> m_vPgRcon;

	CGameWorld m_Playground;
	bool m_Joined = false;
	int m_LocalId = -1;
	int m_PlaygroundTick = 0;

	// Playground memory. history[0] = join state, history[i] = state after
	// step i; inputs[i] = input used by step i+1 (so inputs.size() ==
	// history.size() - 1 while at the head of the timeline).
	std::vector<CTasState> m_vPlaygroundHistory;
	std::vector<CNetObj_PlayerInput> m_vPlaygroundInputs;

	// Record memory (cleared on each Record start) + saved memory.
	std::vector<CTasTick> m_vRecord;
	std::vector<CTasTick> m_vSaved;

	// World state at the moment Record started; Play restores it so the
	// recorded inputs replay from exactly the state they were recorded in.
	CTasState m_RecordStart;
	CTasState m_SavedStart;

	bool m_Playing = false;
	bool m_Recording = false;
	bool m_Paused = false;
	bool m_Desynced = false;
	int m_CvarRecordShadow = 0;
	int m_CvarPauseShadow = 0;
	int m_Mode = 0; // 0 = Play, 1 = Record
	int m_PlayIdx = 0; // which tick of m_vSaved is being replayed
	float m_Tps = 50.0f;
	// Playground ticks owed (fractional), accumulated per real client tick.
	float m_TickAccum = 0.0f;
	float m_LastPredClock = -1.0f; // last smooth prediction clock (PredGameTick + intra) used for stepping
	CNetObj_PlayerInput m_LastInput{};

	// Rewind rebasing of the global client counters (fire / weapon switch):
	// clicks undone by a rewind stay baked into m_aInputData, so LiveInput
	// subtracts these offsets and the fake timeline sees no ghost presses.
	// Fire is derived from the physical held bit in LiveInput() (an absolute
	// rebase could flip that bit and invert the bind). Weapon-switch counters
	// are pure press counters: accumulate the real edge deltas instead.
	int m_FakeNextWeapon = 0;
	int m_FakePrevWeapon = 0;
	int m_LastLiveNextWeapon = 0;
	int m_LastLivePrevWeapon = 0;

	// Auto Rewind/Forward state (settings in the KxTasAutoRf* config vars;
	// m_AutoRfTile is the tile being edited in the UI).
	int m_AutoRfTile = 0;
	bool m_AutoRfWasFrozen = false;
	bool m_AutoRfWasTele = false;
	bool m_AutoRfWasDead = false;

	// Real-world execution state: replay position + the running fire
	// counter (rebased onto the live one so the press/release parity edges
	// reproduce exactly).
	bool m_RealPlaying = false;
	int m_RealIdx = 0;
	int m_RealFire = 0;

	bool m_FilePicked = false;
	bool m_TakeFromFile = false;
	char m_aPickedPath[512] = {};
	char m_aPickedMap[128] = {};
	char m_aPickedName[128] = {};
	char m_aPickedAuthor[128] = {};
	char m_aPickedCreated[64] = {};

};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_TAS_H
