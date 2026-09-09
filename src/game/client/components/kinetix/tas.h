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

#include <algorithm>
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

// Complete physics state of the playground at one tick.
struct CTasState
{
	int Tick = 0;
	CCharacterCore Core;
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

	static constexpr int MAX_HISTORY = 20000; // rewind depth cap (steps)
	static constexpr int TRIM_CHUNK = 5000; // entries dropped when the cap is hit

private:
	void JoinPlayground();
	void LeavePlayground();
	CNetObj_PlayerInput LiveInput() const;
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
	int m_FireRebase = 0;
	int m_NextWeaponRebase = 0;
	int m_PrevWeaponRebase = 0;

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
