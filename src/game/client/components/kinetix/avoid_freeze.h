#ifndef GAME_CLIENT_COMPONENTS_KINETIX_AVOID_FREEZE_H
#define GAME_CLIENT_COMPONENTS_KINETIX_AVOID_FREEZE_H

#include <base/vmath.h>

#include <engine/shared/jobs.h>

#include <game/client/component.h>
#include <game/client/prediction/gameworld.h>
#include <generated/protocol.h>

#include <atomic>
#include <future>
#include <optional>
#include <vector>

class IEngine;

class CAvoidFreeze : public CComponent
{
public:
	struct SBlatantSettings
	{
		bool m_AvoidFreeze = true;
		bool m_AvoidTeleport = false;
		bool m_AvoidDeath = false;
		bool m_Unfreeze = false;
		int m_UnfreezeTicks = 26;
		bool m_Nsif = true;
		bool m_PlayerPrediction = true;
		int m_AfkProtectionMs = 0;
		int m_CheckTicks = 26;
		int m_KickTicks = 26;
		bool m_Hook = true;
		bool m_Fire = false;
		bool m_Direction = true;
		bool m_Jump = false;
		bool m_Aim = false;
		int m_Angles = 5;
		int m_Fov = 90;
		bool m_Silent = false;
		bool m_AutoAim = false;
		bool m_AimAssist = true;
		bool m_TrackPoint = false;
		bool m_SafeTracking = false;
		bool m_DrawTrackPoint = false;
		bool m_DrawAim = false;
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnRender() override;
	void ApplyOverride();

private:
	class CWorld
	{
	public:
		CWorld() = default;
		CWorld(const CWorld &Other);
		CWorld &operator=(const CWorld &Other) = delete;

		bool Valid() const { return m_pLocal != nullptr; }
		void CopyFrom(const CGameWorld &Source, int LocalId, CGameClient *pGame, bool PredictPlayers);
		void Tick(const CNetObj_PlayerInput &Input);
		bool Frozen() const;
		bool OnUnfreezeTile() const;
		vec2 Position() const;
		float HookLength() const;
		std::optional<vec2> HookPoint(vec2 Aim) const;
		// Parallel greedy search is only safe without shared-collision writers:
		// laser bounces temporarily patch the collision grid, so neither existing
		// lasers nor weapons/pickups that can spawn them may run concurrently.
		bool ParallelSearchSafe(int NumInputs);
		IEngine *JobEngine() const { return m_pJobEngine; }

		bool m_AvoidFreeze = true;
		bool m_AvoidTeleport = false;
		bool m_AvoidDeath = false;

	private:
		CGameWorld m_World;
		CCharacter *m_pLocal = nullptr;
		int m_LocalId = -1;
		CGameClient *m_pGame = nullptr;
		IEngine *m_pJobEngine = nullptr;
	};

	struct SPlan
	{
		static constexpr int SAFE = 9999;
		std::vector<CNetObj_PlayerInput> m_vInputs;
		int m_Life = -1;
	};

	// One Greedy plan per candidate, executed on the shared engine job pool.
	// A queued job may be claimed inline by the input thread when the pool is
	// busy; a late worker then returns without ever dereferencing the
	// (possibly already destroyed) world.
	class CGreedyJob final : public IJob
	{
	public:
		CGreedyJob(CAvoidFreeze *pOwner, const CWorld &World, CNetObj_PlayerInput Input, int CheckTicks);
		void Run() override;
		void RunInline();
		std::future<SPlan> Result() { return m_Result.get_future(); }

	private:
		CAvoidFreeze *m_pOwner;
		const CWorld &m_World;
		CNetObj_PlayerInput m_Input;
		int m_CheckTicks;
		std::promise<SPlan> m_Result;
		std::atomic<bool> m_Claimed{false};
	};

	CNetObj_PlayerInput Action(const CNetObj_PlayerInput &Input, int Direction, int Hook) const;
	static int ConstantLife(const CWorld &World, const CNetObj_PlayerInput &Input, int CheckTicks);
	static int SequenceLife(const CWorld &World, const std::vector<CNetObj_PlayerInput> &vInputs);
	SPlan Greedy(const CWorld &World, CNetObj_PlayerInput Input, int CheckTicks,
		std::optional<vec2> Anchor = std::nullopt, bool TrackFromFirstTick = true);
	std::vector<CNetObj_PlayerInput> AimCandidates(const CWorld &World, const CNetObj_PlayerInput &Input, vec2 MouseAim);
	std::vector<std::vector<CNetObj_PlayerInput>> CandidateGroups(const CNetObj_PlayerInput &Input) const;
	std::vector<CNetObj_PlayerInput> Candidates(const CWorld &World, const CNetObj_PlayerInput &Input, vec2 MouseAim);
	// Reference 0x1402d30b0: one Greedy job per candidate on the shared engine
	// pool; results stay in candidate order, never in worker completion order.
	std::vector<SPlan> SearchCandidates(CWorld &World, const std::vector<CNetObj_PlayerInput> &vInputs, int CheckTicks);
	void UpdateTracking(CWorld &World, CNetObj_PlayerInput &Input, vec2 MouseAim);
	// Fire is a live edge counter, not a replayable movement command. Keep the
	// NSIF tail from resurrecting an already released press.
	void SynchronizeFireCounter(int Fire);
	std::optional<CNetObj_PlayerInput> Decide(CWorld &World, CNetObj_PlayerInput Input, vec2 MouseAim);

	SBlatantSettings m_Settings;
	std::vector<CNetObj_PlayerInput> m_vLastPlan;
	std::optional<vec2> m_TrackPoint;
	std::optional<vec2> m_AssistPoint;
	std::optional<vec2> m_AutoAimPoint;
	int m_LastEvalTick = -1;
	CNetObj_PlayerInput m_LastEvalInput{};
	double m_LastInputChangeTime = 0.0;
	// The reference clears the stored plan when the local tee or the active
	// slot changes: a respawn or dummy switch must not consume another tee's
	// stored NSIF tail.
	int m_LastClientId = -1;
	int m_LastSlot = -1;
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_AVOID_FREEZE_H