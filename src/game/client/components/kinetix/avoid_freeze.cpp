#include <game/client/components/kinetix/avoid_freeze.h>

#include <engine/engine.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/entities/pickup.h>

#include <game/mapitems.h>

#include <base/mem.h>
#include <base/time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace
{
	vec2 InputAim(const CNetObj_PlayerInput &Input)
	{
		return vec2((float)Input.m_TargetX, (float)Input.m_TargetY);
	}

	void AimAt(CNetObj_PlayerInput &Input, vec2 Point, vec2 Position)
	{
		const vec2 Aim = Point - Position;
		Input.m_TargetX = (int)Aim.x;
		Input.m_TargetY = (int)Aim.y;
	}
}

CAvoidFreeze::CWorld::CWorld(const CWorld &Other)
{
	// Isolated copy: the source world is never written to, so several threads
	// may clone the same avoid world concurrently. CopyWorld would link the
	// copy as the source's child — entity removals in the speculative ticks
	// would then destroy entities in the live world.
	m_World.CopyWorldIsolated(&Other.m_World);
	m_LocalId = Other.m_LocalId;
	m_pGame = Other.m_pGame;
	m_pJobEngine = Other.m_pJobEngine;
	m_pLocal = m_World.GetCharacterById(m_LocalId);
	m_AvoidFreeze = Other.m_AvoidFreeze;
	m_AvoidTeleport = Other.m_AvoidTeleport;
	m_AvoidDeath = Other.m_AvoidDeath;
}

void CAvoidFreeze::CWorld::CopyFrom(const CGameWorld &Source, int LocalId, CGameClient *pGame, bool PredictPlayers)
{
	// See the copy constructor: the snapshot must be isolated from the live
	// predicted world.
	m_World.CopyWorldIsolated(&Source);
	m_LocalId = LocalId;
	m_pGame = pGame;
	m_pJobEngine = pGame ? pGame->Engine() : nullptr;
	m_pLocal = m_World.GetCharacterById(LocalId);
	if(!m_pLocal)
		return;
	const float Radius = HookLength() + 56.0f;
	for(int Id = 0; Id < MAX_CLIENTS; ++Id)
	{
		CCharacter *pCharacter = m_World.GetCharacterById(Id);
		if(!pCharacter || Id == LocalId || pCharacter->m_IsLocal)
			continue;
		// Keep tees only when player prediction is on, they are close, active
		// and not on the other team. Stale snapshots beyond 10 ticks are dropped.
		if(!PredictPlayers || distance(m_pLocal->m_Pos, pCharacter->m_Pos) > Radius ||
			(pGame && ((!pGame->m_Snap.m_aCharacters[Id].m_Active && pCharacter->m_SnapTicks > 10) || pGame->IsOtherTeam(Id))))
			delete pCharacter;
	}
	// Reference speculative-world setup: death-tile contact does not put the
	// tee into m_IsInFreeze, and explosions use the local tuning slot instead
	// of the projectile owner's tune zone. Clones inherit these flags through
	// the copied world config.
	m_World.m_WorldConfig.m_KxAfNoDeathContact = true;
	m_World.m_WorldConfig.m_KxAfExplosionTuning = true;
	m_World.m_WorldConfig.m_KxAfExplosionStrength = pGame ? pGame->m_aTuning[g_Config.m_ClDummy].m_ExplosionStrength : 6.0f;
}

void CAvoidFreeze::CWorld::Tick(const CNetObj_PlayerInput &Input)
{
	if(!m_pLocal)
		return;
	CNetObj_PlayerInput Copy = Input;
	m_pLocal->OnDirectInput(&Copy);
	++m_World.m_GameTick;
	m_pLocal->OnPredictedInput(&Copy);
	m_World.Tick();
	m_pLocal = m_World.GetCharacterById(m_LocalId);
}

bool CAvoidFreeze::CWorld::ParallelSearchSafe(int NumInputs)
{
	if(!m_pJobEngine || NumInputs <= 1)
		return false;
	// Laser bounces temporarily patch the shared collision grid. Neither
	// existing lasers nor weapons/pickups that can spawn them may run in
	// parallel.
	if(m_World.FindFirst(CGameWorld::ENTTYPE_LASER))
		return false;
	if(m_World.m_WorldConfig.m_PredictWeapons)
	{
		for(auto *pCharacter = static_cast<CCharacter *>(m_World.FindFirst(CGameWorld::ENTTYPE_CHARACTER)); pCharacter;
			pCharacter = static_cast<CCharacter *>(pCharacter->TypeNext()))
			if(pCharacter->GetWeaponGot(WEAPON_LASER) || pCharacter->GetWeaponGot(WEAPON_SHOTGUN))
				return false;
		for(auto *pPickup = static_cast<CPickup *>(m_World.FindFirst(CGameWorld::ENTTYPE_PICKUP)); pPickup;
			pPickup = static_cast<CPickup *>(pPickup->TypeNext()))
			if(pPickup->Type() == POWERUP_WEAPON && (pPickup->Subtype() == WEAPON_LASER || pPickup->Subtype() == WEAPON_SHOTGUN))
				return false;
	}
	return true;
}

bool CAvoidFreeze::CWorld::Frozen() const
{
	if(!m_pLocal)
		return true;
	if(m_AvoidFreeze && (m_pLocal->m_FreezeTime > 0 || m_pLocal->Core()->m_DeepFrozen || m_pLocal->Core()->m_IsInFreeze))
		return true;
	if(m_AvoidTeleport || m_AvoidDeath)
	{
		CCollision *pCollision = const_cast<CGameWorld &>(m_World).Collision();
		if(pCollision)
		{
			const vec2 Pos((float)(int)m_pLocal->m_Pos.x, (float)(int)m_pLocal->m_Pos.y);
			const int Index = pCollision->GetPureMapIndex(Pos);
			if(m_AvoidDeath && pCollision->GetTileIndex(Index) == TILE_DEATH)
				return true;
			if(m_AvoidTeleport && (pCollision->IsTeleport(Index) || pCollision->IsEvilTeleport(Index)))
				return true;
		}
	}
	return false;
}

bool CAvoidFreeze::CWorld::OnUnfreezeTile() const
{
	if(!m_pLocal)
		return false;
	// Raw game-layer tile: GetCollisionAt filters out the unfreeze tile.
	const vec2 Pos((float)(int)m_pLocal->m_Pos.x, (float)(int)m_pLocal->m_Pos.y);
	const CCollision *pCollision = m_World.Collision();
	return pCollision->GetTileIndex(pCollision->GetPureMapIndex(Pos)) == TILE_UNFREEZE;
}

vec2 CAvoidFreeze::CWorld::Position() const
{
	return m_pLocal ? m_pLocal->m_Pos : vec2(0.0f, 0.0f);
}

float CAvoidFreeze::CWorld::HookLength() const
{
	// The reference reads the first tuning slot, independently of the dummy.
	if(m_pGame)
		return m_pGame->m_aTuning[0].m_HookLength;
	return m_pLocal ? m_pLocal->Core()->m_Tuning.m_HookLength : 380.0f;
}

std::optional<vec2> CAvoidFreeze::CWorld::HookPoint(vec2 Aim) const
{
	if(!m_pLocal)
		return std::nullopt;
	const vec2 QuantizedAim((float)(int)Aim.x, (float)(int)Aim.y);
	if(QuantizedAim == vec2(0.0f, 0.0f))
		return std::nullopt;
	const vec2 To = m_pLocal->m_Pos + normalize(QuantizedAim) * (HookLength() - 28.0f);
	vec2 HitPos = To;
	// The reference tests the live interpolated characters before tracing the
	// map, so the speculative world cannot hide a body in the way.
	if(m_pGame)
	{
		if(m_pGame->IntersectCharacter(m_pLocal->m_Pos, To, HitPos, m_LocalId) != -1)
			return std::nullopt;
	}
	else if(const_cast<CGameWorld &>(m_World).IntersectCharacter(m_pLocal->m_Pos, To, 0.0f, HitPos, m_pLocal, m_LocalId) != nullptr)
	{
		return std::nullopt;
	}
	int TeleNumber = 0;
	const int Hit = const_cast<CGameWorld &>(m_World).Collision()->IntersectLineTeleHook(m_pLocal->m_Pos, To, &HitPos, nullptr, &TeleNumber);
	if(Hit == TILE_SOLID && HitPos != vec2(0.0f, 0.0f))
		return HitPos;
	return std::nullopt;
}

CNetObj_PlayerInput CAvoidFreeze::Action(const CNetObj_PlayerInput &Input, int Direction, int Hook) const
{
	CNetObj_PlayerInput Result = Input;
	Result.m_Direction = Direction;
	Result.m_Hook = Hook;
	// The reference always releases the jump inside the search, so a held jump
	// key cannot poison every simulated tick. The Jump toggle only decides
	// whether the released jump is also written to the output.
	Result.m_Jump = 0;
	return Result;
}

int CAvoidFreeze::ConstantLife(const CWorld &World, const CNetObj_PlayerInput &Input, int CheckTicks)
{
	CWorld Copy(World);
	if(!Copy.Valid())
		return -5;
	for(int Tick = 0; Tick <= CheckTicks; ++Tick)
	{
		Copy.Tick(Input);
		if(Copy.Frozen())
			return Tick;
	}
	return SPlan::SAFE;
}

int CAvoidFreeze::SequenceLife(const CWorld &World, const std::vector<CNetObj_PlayerInput> &vInputs)
{
	CWorld Copy(World);
	if(!Copy.Valid())
		return -5;
	for(size_t Tick = 0; Tick < vInputs.size(); ++Tick)
	{
		Copy.Tick(vInputs[Tick]);
		if(Copy.Frozen())
			return (int)Tick;
	}
	return (int)vInputs.size();
}

CAvoidFreeze::SPlan CAvoidFreeze::Greedy(const CWorld &World, CNetObj_PlayerInput Input, int CheckTicks,
	std::optional<vec2> Anchor, bool TrackFromFirstTick)
{
	CWorld Copy(World);
	SPlan Result;
	if(!Copy.Valid())
		return Result;
	// The order is part of KRX's tie-breaking.
	static constexpr int ACTIONS[][2] = {{0, 0}, {0, 1}, {1, 0}, {-1, 0}, {1, 1}, {-1, 1}};
	for(int Tick = 0, Remaining = CheckTicks; Tick <= CheckTicks; ++Tick, --Remaining)
	{
		if(Anchor && *Anchor != vec2(0.0f, 0.0f) && (TrackFromFirstTick || Tick > 0))
			AimAt(Input, *Anchor, Copy.Position());
		if(Tick > 0)
		{
			int BestLife = -1;
			CNetObj_PlayerInput BestInput{};
			for(const auto &Next : ACTIONS)
			{
				const CNetObj_PlayerInput Candidate = Action(Input, Next[0], Next[1]);
				const int Life = ConstantLife(Copy, Candidate, Remaining);
				if(Life > BestLife)
				{
					BestInput = Candidate;
					BestLife = Life;
				}
				if(Life > Remaining)
					break;
			}
			Input = BestInput;
			if(BestLife > Remaining)
			{
				Result.m_vInputs.insert(Result.m_vInputs.end(), Remaining, Input);
				Result.m_Life = SPlan::SAFE;
				return Result;
			}
		}
		Copy.Tick(Input);
		if(Copy.Frozen())
		{
			Result.m_Life = Tick;
			return Result;
		}
		Result.m_vInputs.push_back(Input);
	}
	Result.m_Life = SPlan::SAFE;
	return Result;
}

std::vector<CNetObj_PlayerInput> CAvoidFreeze::AimCandidates(const CWorld &World, const CNetObj_PlayerInput &Input, vec2 MouseAim)
{
	std::vector<CNetObj_PlayerInput> vResult;
	if(!m_Settings.m_Aim)
	{
		m_AssistPoint.reset();
		m_AutoAimPoint.reset();
		return vResult;
	}
	if(!World.Valid() || Input.m_Hook == 1)
		return vResult;

	CNetObj_PlayerInput BestAssist = Input;
	CNetObj_PlayerInput BestAuto = Input;
	BestAssist.m_Hook = 1;
	BestAuto.m_Hook = 1;
	int AssistLife = 0;
	int AutoLife = 0;
	const auto OutputsFixed = [&] { return AssistLife == SPlan::SAFE && AutoLife == SPlan::SAFE; };
	float AssistDot = -20.0f;
	const vec2 Mouse = normalize(InputAim(Input));
	float Fov = (float)m_Settings.m_Fov * pi / 180.0f;
	if(m_Settings.m_Fov == 360)
		Fov = 2.0f * pi;
	const float Step = Fov / (float)std::max(1, m_Settings.m_Angles);
	const float Step50 = Step / 50.0f;
	const float Center = angle(MouseAim);
	const float End = Center + Fov * 0.5f;
	const float Radius = World.HookLength() - 28.0f;
	for(float Angle = Center - Fov * 0.5f; Angle < End && !OutputsFixed(); Angle += Step)
	{
		std::optional<vec2> Aim;
		vec2 Hit;
		for(int Probe = 0; Probe < 50; ++Probe)
		{
			const vec2 Candidate = normalize(direction(Angle + (float)Probe * Step50)) * Radius;
			const auto Point = World.HookPoint(Candidate);
			if(Point && *Point != vec2(0.0f, 0.0f))
			{
				Hit = Candidate;
				Aim = Point;
				break;
			}
		}
		if(!Aim)
			continue;
		for(const int Direction : {-1, 0, 1})
		{
			if(OutputsFixed())
				break;
			CNetObj_PlayerInput Candidate = Input;
			Candidate.m_Direction = Direction;
			Candidate.m_TargetX = (int)Hit.x;
			Candidate.m_TargetY = (int)Hit.y;
			Candidate.m_Hook = 1;
			const float Dot = dot(normalize(Hit), Mouse);
			if(AutoLife == SPlan::SAFE && !(Dot > AssistDot))
				continue;
			const int Life = ConstantLife(World, Candidate, 26);
			if(Dot > AssistDot && Life > AssistLife)
			{
				AssistDot = Dot;
				AssistLife = Life;
				BestAssist = Candidate;
				m_AssistPoint = Aim;
			}
			if(Life > AutoLife)
			{
				AutoLife = Life;
				BestAuto = Candidate;
				m_AutoAimPoint = Aim;
			}
		}
	}
	if(m_Settings.m_AimAssist)
		vResult.push_back(BestAssist);
	if(m_Settings.m_AutoAim)
		vResult.push_back(BestAuto);
	return vResult;
}

std::vector<std::vector<CNetObj_PlayerInput>> CAvoidFreeze::CandidateGroups(const CNetObj_PlayerInput &Input) const
{
	using CAction = std::array<int, 2>;
	std::array<std::vector<CAction>, 3> aGroups;
	const int Direction = Input.m_Direction;
	if(Input.m_Hook == 0)
	{
		if(Direction == -1)
			aGroups = {{{{0, 0}, {1, 0}}, {{-1, 1}}, {{0, 1}, {1, 1}}}};
		else if(Direction == 0)
			aGroups = {{{{-1, 0}, {1, 0}}, {{0, 1}}, {{-1, 1}, {1, 1}}}};
		else if(Direction == 1)
			aGroups = {{{{0, 0}, {-1, 0}}, {{1, 1}}, {{0, 1}, {-1, 1}}}};
	}
	else
	{
		if(Direction == -1)
			aGroups = {{{{-1, 0}}, {{0, 1}, {0, 0}}, {{1, 1}, {1, 0}}}};
		else if(Direction == 0)
			aGroups = {{{{0, 0}}, {{-1, 1}, {1, 1}}, {{-1, 0}, {1, 0}}}};
		else if(Direction == 1)
			aGroups = {{{{1, 0}}, {{0, 1}, {0, 0}}, {{-1, 1}, {-1, 0}}}};
	}
	// The complete groups are emitted; Direction/Hook settings are UI only in
	// the reference implementation and do not filter this list.
	std::vector<std::vector<CNetObj_PlayerInput>> vGroups;
	for(const auto &Group : aGroups)
	{
		std::vector<CNetObj_PlayerInput> vGroup;
		for(const auto &Next : Group)
			vGroup.push_back(Action(Input, Next[0], Next[1]));
		if(!vGroup.empty())
			vGroups.push_back(std::move(vGroup));
	}
	return vGroups;
}

std::vector<CNetObj_PlayerInput> CAvoidFreeze::Candidates(const CWorld &World, const CNetObj_PlayerInput &Input, vec2 MouseAim)
{
	struct CScored
	{
		CNetObj_PlayerInput m_Input;
		int m_Life;
	};
	std::vector<CNetObj_PlayerInput> vResult;
	std::vector<CScored> vScored;
	const auto vGroups = CandidateGroups(Input);
	for(int Pass = 0; Pass < 2; ++Pass)
	{
		for(const auto &Group : vGroups)
		{
			for(const auto &Candidate : Group)
				vScored.push_back({Candidate, ConstantLife(World, Candidate, m_Settings.m_CheckTicks)});
			CScored Best{};
			Best.m_Life = -1;
			if(!vScored.empty())
			{
				std::sort(vScored.begin(), vScored.end(), [](const CScored &A, const CScored &B) {
					return A.m_Life > B.m_Life;
				});
				Best = vScored.front();
			}
			if(Best.m_Life >= 0)
			{
				const bool Duplicate = std::any_of(vResult.begin(), vResult.end(), [&](const auto &Existing) {
					return Existing.m_Direction == Best.m_Input.m_Direction && Existing.m_Hook == Best.m_Input.m_Hook;
				});
				if(!Duplicate)
					vResult.push_back(Best.m_Input);
			}
			if(Pass == 1)
				vScored.clear();
			if(Best.m_Life == SPlan::SAFE)
				break;
		}
	}
	const auto vAim = AimCandidates(World, Input, MouseAim);
	vResult.insert(vResult.end(), vAim.begin(), vAim.end());
	return vResult;
}

CAvoidFreeze::CGreedyJob::CGreedyJob(CAvoidFreeze *pOwner, const CWorld &World, CNetObj_PlayerInput Input, int CheckTicks) :
	m_pOwner(pOwner), m_World(World), m_Input(Input), m_CheckTicks(CheckTicks)
{
	Abortable(false);
}

void CAvoidFreeze::CGreedyJob::Run()
{
	RunInline();
}

void CAvoidFreeze::CGreedyJob::RunInline()
{
	if(m_Claimed.exchange(true, std::memory_order_acq_rel))
		return;
	m_Result.set_value(m_pOwner->Greedy(m_World, m_Input, m_CheckTicks));
}

std::vector<CAvoidFreeze::SPlan> CAvoidFreeze::SearchCandidates(CWorld &World, const std::vector<CNetObj_PlayerInput> &vInputs, int CheckTicks)
{
	if(!World.ParallelSearchSafe((int)vInputs.size()))
	{
		std::vector<SPlan> vResults;
		vResults.reserve(vInputs.size());
		for(const auto &Input : vInputs)
			vResults.push_back(Greedy(World, Input, CheckTicks));
		return vResults;
	}

	// Reference 0x1402d30b0 waits for all independent greedy jobs, then selects
	// by input index. Futures are stored in that order, never by completion
	// time. A job the pool has not picked up yet runs inline on the input
	// thread, so the pool can still work through the remaining candidates.
	std::vector<std::future<SPlan>> vPending;
	std::vector<std::shared_ptr<CGreedyJob>> vJobs;
	vPending.reserve(vInputs.size());
	vJobs.reserve(vInputs.size());
	for(const auto &Input : vInputs)
	{
		auto pJob = std::make_shared<CGreedyJob>(this, World, Input, CheckTicks);
		vPending.push_back(pJob->Result());
		vJobs.push_back(pJob);
		World.JobEngine()->AddJob(std::move(pJob));
	}
	for(size_t Index = 0; Index < vPending.size(); ++Index)
	{
		if(vPending[Index].wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			vJobs[Index]->RunInline();
		vPending[Index].wait();
	}
	std::vector<SPlan> vResults;
	vResults.reserve(vPending.size());
	for(auto &Pending : vPending)
		vResults.push_back(Pending.get());
	return vResults;
}

void CAvoidFreeze::UpdateTracking(CWorld &World, CNetObj_PlayerInput &Input, vec2 MouseAim)
{
	if(!m_Settings.m_TrackPoint || (m_TrackPoint && distance(World.Position(), *m_TrackPoint) >= World.HookLength()))
		m_TrackPoint.reset();
	if(!m_Settings.m_TrackPoint)
		return;
	if(m_TrackPoint)
		AimAt(Input, *m_TrackPoint, World.Position());
	const auto Point = World.HookPoint(MouseAim);
	if(!Point || *Point == vec2(0.0f, 0.0f))
		return;
	auto Candidate = Input;
	AimAt(Candidate, *Point, World.Position());
	if(!m_Settings.m_SafeTracking || m_Settings.m_Unfreeze ||
		Greedy(World, Candidate, m_Settings.m_CheckTicks, Point).m_Life == SPlan::SAFE)
	{
		m_TrackPoint = Point;
		Input = Candidate;
	}
}

void CAvoidFreeze::SynchronizeFireCounter(int Fire)
{
	for(auto &Input : m_vLastPlan)
		Input.m_Fire = Fire;
}

std::optional<CNetObj_PlayerInput> CAvoidFreeze::Decide(CWorld &World, CNetObj_PlayerInput Input, vec2 MouseAim)
{
	if(!World.Valid())
		return std::nullopt;

	UpdateTracking(World, Input, MouseAim);
	if(m_Settings.m_TrackPoint && m_TrackPoint)
		AimAt(Input, *m_TrackPoint, World.Position());

	if(ConstantLife(World, Input, m_Settings.m_KickTicks) == SPlan::SAFE)
		return Input;

	const auto Guard = Greedy(World, Input, m_Settings.m_CheckTicks, m_TrackPoint);
	if(Guard.m_Life == SPlan::SAFE)
	{
		m_vLastPlan = Guard.m_vInputs;
		return Input;
	}

	if(m_Settings.m_Unfreeze)
	{
		CWorld Unfreeze(World);
		bool WasFrozen = Unfreeze.Frozen();
		for(int Tick = 0; Tick <= m_Settings.m_UnfreezeTicks; ++Tick)
		{
			Unfreeze.Tick(Input);
			if(!Unfreeze.Frozen() && WasFrozen && Unfreeze.OnUnfreezeTile())
			{
				if(Tick > 0)
				{
					const auto Plan = Greedy(Unfreeze, Input, m_Settings.m_CheckTicks, m_TrackPoint);
					if(Plan.m_Life == SPlan::SAFE && !Plan.m_vInputs.empty())
					{
						m_vLastPlan = Plan.m_vInputs;
						return Input;
					}
				}
				break;
			}
			WasFrozen |= Unfreeze.Frozen();
		}
	}

	SPlan Best;
	const auto vCandidates = Candidates(World, Input, MouseAim);
	const auto vResults = SearchCandidates(World, vCandidates, m_Settings.m_CheckTicks);
	for(const auto &Plan : vResults)
	{
		if(Plan.m_Life == SPlan::SAFE)
		{
			m_vLastPlan = Plan.m_vInputs;
			if(m_vLastPlan.empty())
				return std::nullopt;
			return m_vLastPlan.front();
		}
		if(Plan.m_Life > Best.m_Life)
			Best = Plan;
	}

	if(Best.m_vInputs.empty() || m_vLastPlan.size() < 2 || !m_Settings.m_Nsif)
		return std::nullopt;
	const std::vector<CNetObj_PlayerInput> vTail(m_vLastPlan.begin() + 1, m_vLastPlan.end());
	if(SequenceLife(World, vTail) == (int)vTail.size())
	{
		const CNetObj_PlayerInput Output = m_vLastPlan[1];
		m_vLastPlan.erase(m_vLastPlan.begin() + 1);
		return Output;
	}
	if(Best.m_Life > 0 && !Best.m_vInputs.empty())
		return Best.m_vInputs.front();
	return std::nullopt;
}

void CAvoidFreeze::OnReset()
{
	m_vLastPlan.clear();
	m_TrackPoint.reset();
	m_AssistPoint.reset();
	m_AutoAimPoint.reset();
	m_LastEvalTick = -1;
	m_LastEvalInput = {};
	m_LastInputChangeTime = 0.0;
}

void CAvoidFreeze::OnRender()
{
	if(!g_Config.m_KxAvoidFreeze)
		return;
	CGameClient *pGame = GameClient();
	if(!pGame)
		return;
	const int Slot = g_Config.m_ClDummy;
	const vec2 From = pGame->m_LocalCharacterPos;
	const float HookLength = pGame->m_aTuning[Slot].m_HookLength;
	if(m_AssistPoint && distance(From, *m_AssistPoint) > HookLength)
		m_AssistPoint.reset();
	if(m_AutoAimPoint && distance(From, *m_AutoAimPoint) > HookLength)
		m_AutoAimPoint.reset();
	if(m_TrackPoint && distance(From, *m_TrackPoint) > HookLength)
		m_TrackPoint.reset();
	if(!m_TrackPoint && !m_AssistPoint && !m_AutoAimPoint)
		return;
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
	auto Draw = [&](vec2 Point, ColorRGBA Circle, ColorRGBA Line) {
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(Circle);
		Graphics()->DrawCircle(Point.x, Point.y, 3.25f, 64);
		if(distance(From, Point) >= 1.0f)
		{
			const vec2 Offset = normalize(vec2(Point.y - From.y, From.x - Point.x)) * (1.5f * pGame->m_Camera.m_Zoom);
			Graphics()->SetColor(Line);
			IGraphics::CFreeformItem Quad(Point - Offset, Point + Offset, From - Offset, From + Offset);
			Graphics()->QuadsDrawFreeform(&Quad, 1);
		}
		Graphics()->QuadsEnd();
	};
	if(m_Settings.m_DrawAim && m_Settings.m_Aim)
	{
		if(m_Settings.m_AimAssist && m_AssistPoint)
			Draw(*m_AssistPoint, ColorRGBA(0.0f, 0.0f, 1.0f, 0.5f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
		if(m_Settings.m_AutoAim && m_AutoAimPoint)
			Draw(*m_AutoAimPoint, ColorRGBA(1.0f, 0.0f, 0.0f, 0.5f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
	}
	if(m_Settings.m_DrawTrackPoint && m_Settings.m_TrackPoint && m_TrackPoint)
	{
		bool Hookable = false;
		const vec2 Mouse = pGame->m_Controls.m_aMousePos[Slot];
		const vec2 Quantized((float)(int)Mouse.x, (float)(int)Mouse.y);
		if(Quantized != vec2(0.0f, 0.0f))
		{
			const vec2 To = From + normalize(Quantized) * (HookLength - 28.0f);
			vec2 Hit = To;
			CGameWorld &World = pGame->m_PredictedWorld;
			const int LocalId = pGame->m_aLocalIds[Slot];
			CCharacter *pLocal = World.GetCharacterById(LocalId);
			const bool Blocked = pLocal && World.IntersectCharacter(From, To, 0.0f, Hit, pLocal, LocalId) != nullptr;
			int Tele = 0;
			Hookable = !Blocked && World.Collision()->IntersectLineTeleHook(From, To, &Hit, nullptr, &Tele) == TILE_SOLID && Hit != vec2(0.0f, 0.0f);
		}
		Draw(*m_TrackPoint, ColorRGBA(0.0f, 1.0f, 0.0f, 0.7f), Hookable ? ColorRGBA(1.0f, 1.0f, 1.0f, 0.7f) : ColorRGBA(1.0f, 1.0f, 0.0f, 1.0f));
	}
}

void CAvoidFreeze::ApplyOverride()
{
	CGameClient *pGame = GameClient();
	const int Slot = g_Config.m_ClDummy;
	const int LocalId = pGame ? pGame->m_aLocalIds[Slot] : -1;
	CNetObj_PlayerInput *pInput = pGame ? &pGame->m_Controls.m_aInputData[Slot] : nullptr;
	if(!pGame || !pInput || !g_Config.m_KxAvoidFreeze)
		return;
	if(Client()->State() != IClient::STATE_ONLINE || pGame->m_Snap.m_SpecInfo.m_Active)
		return;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
		return;

	// SnapInput rebuilds m_aInputData from the keys and the mouse on every call
	// and can run several times per predicted tick. Skipping a second
	// evaluation in the same tick reverted the override and sent the raw
	// input, so the escape fired late or not at all. Evaluate on every call,
	// exactly like the reference AdjustInput.
	const bool InputChanged = mem_comp(pInput, &m_LastEvalInput, sizeof(*pInput)) != 0;
	m_LastEvalInput = *pInput;
	if(InputChanged)
		m_LastInputChangeTime = (double)time_get() / (double)time_freq();

	m_Settings.m_AvoidFreeze = g_Config.m_KxAfAvoidFreeze != 0;
	m_Settings.m_AvoidTeleport = g_Config.m_KxAfAvoidTeleport != 0;
	m_Settings.m_AvoidDeath = g_Config.m_KxAfAvoidDeath != 0;
	m_Settings.m_Unfreeze = g_Config.m_KxAfUnfreeze != 0;
	m_Settings.m_UnfreezeTicks = g_Config.m_KxAfUnfreezeTicks;
	m_Settings.m_Nsif = g_Config.m_KxAfNsif != 0;
	m_Settings.m_PlayerPrediction = g_Config.m_KxAfPlayerPrediction != 0;
	m_Settings.m_AfkProtectionMs = g_Config.m_KxAfAfkProtection;
	m_Settings.m_CheckTicks = g_Config.m_KxAfCheckTicks;
	m_Settings.m_KickTicks = g_Config.m_KxAfKickTicks;
	m_Settings.m_Hook = g_Config.m_KxAfHook != 0;
	m_Settings.m_Fire = g_Config.m_KxAfFire != 0;
	m_Settings.m_Direction = g_Config.m_KxAfDirection != 0;
	m_Settings.m_Jump = g_Config.m_KxAfJump != 0;
	m_Settings.m_Aim = g_Config.m_KxAfAim != 0;
	m_Settings.m_Angles = g_Config.m_KxAfAngles;
	m_Settings.m_Fov = g_Config.m_KxAfFov;
	m_Settings.m_Silent = g_Config.m_KxAfSilent != 0;
	m_Settings.m_AutoAim = g_Config.m_KxAfAutoAim != 0;
	m_Settings.m_AimAssist = g_Config.m_KxAfAimAssist != 0;
	m_Settings.m_TrackPoint = g_Config.m_KxAfTrackPoint != 0;
	m_Settings.m_SafeTracking = g_Config.m_KxAfSafeTracking != 0;
	m_Settings.m_DrawTrackPoint = g_Config.m_KxAfDrawTrackPoint != 0;
	m_Settings.m_DrawAim = g_Config.m_KxAfDrawAim != 0;

	if(m_Settings.m_AfkProtectionMs > 0 && m_LastInputChangeTime > 0.0 &&
		((double)time_get() / (double)time_freq() - m_LastInputChangeTime) * 1000.0 > (double)m_Settings.m_AfkProtectionMs)
		return;

	// A respawn/dummy switch must not consume another tee's stored NSIF plan.
	if(LocalId != m_LastClientId || Slot != m_LastSlot)
	{
		m_vLastPlan.clear();
		m_TrackPoint.reset();
		m_AssistPoint.reset();
		m_AutoAimPoint.reset();
	}
	m_LastClientId = LocalId;
	m_LastSlot = Slot;

	CWorld World;
	World.CopyFrom(pGame->m_PredictedWorld, LocalId, pGame, m_Settings.m_PlayerPrediction);
	if(!World.Valid())
		return;
	World.m_AvoidFreeze = m_Settings.m_AvoidFreeze;
	World.m_AvoidTeleport = m_Settings.m_AvoidTeleport;
	World.m_AvoidDeath = m_Settings.m_AvoidDeath;

	// Reference gate: KRX decides from the predicted world but refuses to steer
	// while the authoritative character is already frozen.
	const CCharacter *pAuthoritative = pGame->m_GameWorld.GetCharacterById(LocalId);
	if(pAuthoritative)
	{
		// FrozenGate: on non-FNG DDNet servers any freeze state gates, on FNG
		// and other servers only a real freeze timer or an active ninja.
		const bool Fng = pGame->m_GameWorld.m_WorldConfig.m_IsFNG ||
				 str_find_nocase(Client()->ServerInfo().m_aGameType, "fng") != nullptr;
		const bool NamedDDNet = str_find_nocase(Client()->ServerInfo().m_aName, "DDNet") != nullptr;
		const bool AuthoritativeFrozen = (!Fng && NamedDDNet)
			? (pAuthoritative->m_FreezeTime > 0 || pAuthoritative->Core()->m_DeepFrozen || pAuthoritative->Core()->m_IsInFreeze)
			: (pAuthoritative->m_FreezeTime > 0 || pGame->m_Snap.m_aCharacters[LocalId].m_Cur.m_Weapon == WEAPON_NINJA);
		if(AuthoritativeFrozen)
			return;
	}

	// Fire is a live edge counter: the stored NSIF tail must simulate the
	// current press, not the one it was recorded with.
	SynchronizeFireCounter(pInput->m_Fire);
	const int RawTargetX = pInput->m_TargetX;
	const int RawTargetY = pInput->m_TargetY;
	const auto Decision = Decide(World, *pInput, pGame->m_Controls.m_aMousePos[Slot]);
	// The reference writes the whole decision slot verbatim: fire, jump and aim
	// come from the plan the simulation validated. Rewriting any of them makes
	// the sent input diverge from the simulated plan and the escape miss.
	if(Decision)
	{
		*pInput = *Decision;
		// Silent keeps the visible crosshair on the mouse: the avoid aim is
		// still carried to the server by the decision written above. Without
		// Silent the visible aim follows the decision (the reference behavior).
		if(!m_Settings.m_Silent && (Decision->m_TargetX != RawTargetX || Decision->m_TargetY != RawTargetY))
			pGame->m_Controls.m_aMousePos[Slot] = vec2((float)Decision->m_TargetX, (float)Decision->m_TargetY);
	}
}