// (c) Kinetix. Avoid Freeze component.
//
// Per-tick plan — NO DFS, no backtracking. A plan is a SEQUENCE of inputs,
// one per tick of the horizon. Walking forward tick by tick, at every tick it
// tries all candidates for THAT tick, keeps the best safe one, applies it and
// moves on. The freeze/death/teleport check runs on every tick. The tick-0
// input of the built plan is what gets applied now; the next tick replans.
//
// At every tick the best candidate is: 1) the smallest diff from the previous
// tick's input (tick 0: the player's current input), 2) the position closest
// to the player's own reference trajectory (AF must not jerk the player).
//
// Synchronous (no worker thread): everything runs in CControls::SnapInput,
// gated to once per predicted tick (+ when the raw input changes).

#include <game/client/components/kinetix/avoid_freeze.h>

#include <algorithm>
#include <vector>

#include <engine/shared/config.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/mapitems.h>
#include <generated/protocol.h>
#include <game/client/components/kinetix/kinetix_internal.h> // SetMousePos

namespace
{
constexpr int AF_MAX_COMBOS = 144; // candidates per tick
constexpr int AF_BEAM_WIDTH = 32;  // beam search width

// Strip every player except the local one.
void AfStripOthers(CGameWorld *pWorld, int LocalId)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId)
			continue;
		if(CCharacter *pChar = pWorld->GetCharacterById(i))
			delete pChar;
	}
}

// Danger (freeze / death tile / teleport, per the toggles) at the current state.
bool AfDangerAt(CCharacter *pChar)
{
	if(!pChar)
		return true;
	if(g_Config.m_KxAfAvoidFreeze && pChar->m_FreezeTime > 0)
		return true;

	CCollision *pCol = pChar->Collision();
	if(!pCol)
		return false;
	const int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
	if(Idx < 0)
		return false;
	if(g_Config.m_KxAfAvoidDeath)
	{
		const int Tile = pCol->GetTileIndex(Idx);
		const int FTile = pCol->GetFrontTileIndex(Idx);
		if(Tile == TILE_DEATH || FTile == TILE_DEATH)
			return true;
	}
	if(g_Config.m_KxAfAvoidTeleport && (pCol->IsTeleport(Idx) || pCol->IsEvilTeleport(Idx)))
		return true;
	return false;
}

int AfInputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B)
{
	int diff = 0;
	if(A.m_Direction != B.m_Direction) diff++;
	if((A.m_Jump != 0) != (B.m_Jump != 0)) diff++;
	if((A.m_Hook != 0) != (B.m_Hook != 0)) diff++;
	if(A.m_TargetX != B.m_TargetX || A.m_TargetY != B.m_TargetY) diff++;
	return diff;
}

struct AfBeamPlan
{
	std::vector<CNetObj_PlayerInput> Timeline; // one input per tick, tick-0 first
	CNetObj_PlayerInput First;                 // tick-0 input
	int TotalDiff;
	float TotalDist;
};

// Simulate Count timeline inputs + one extra input from the root on a STACK
// world. Returns false if any tick freezes/dies/teleports; pOutPos gets the
// final position. No heap worlds, no cross-world links -> no crash.
static bool AfSimTimeline(CGameWorld *pRoot, int LocalId,
	const CNetObj_PlayerInput *pTimeline, int Count,
	const CNetObj_PlayerInput &Extra, vec2 *pOutPos)
{
	CGameWorld Sim;
	Sim.CopyWorld(pRoot);
	AfStripOthers(&Sim, LocalId);
	CCharacter *pChar = Sim.GetCharacterById(LocalId);
	if(!pChar)
		return false;
	const int StartTick = Sim.GameTick();
	for(int i = 0; i <= Count; i++)
	{
		const CNetObj_PlayerInput &In = (i < Count) ? pTimeline[i] : Extra;
		pChar->OnDirectInput(&In);
		Sim.m_GameTick = StartTick + i + 1;
		pChar->OnPredictedInput(&In);
		Sim.Tick();
		CCharacter *pAfter = Sim.GetCharacterById(LocalId);
		if(!pAfter || AfDangerAt(pAfter))
			return false;
		if(i == Count && pOutPos)
			*pOutPos = pAfter->m_Pos;
	}
	return true;
}

// Beam search, width AF_BEAM_WIDTH. Each beam is its input TIMELINE (one input
// per tick) and is re-simulated from the root on demand — no persistent world
// copies are kept, so the heap-CGameWorld use-after-free/double-free that
// crashed the client is gone. At every tick each beam is expanded with all
// candidates for THAT tick, then the best AF_BEAM_WIDTH children survive,
// ranked by 1) total input diff, 2) total deviation from the player's reference
// trajectory. The freeze/death/teleport check runs on every tick. Returns true
// when a full Ticks-long plan exists; pOutFirst receives the tick-0 input.
bool AfBuildBeamPlan(CGameWorld *pRoot, int LocalId, int Ticks,
	const CNetObj_PlayerInput *pCombos, int nCombos,
	const CNetObj_PlayerInput &Current, CNetObj_PlayerInput *pOutFirst)
{
	// Reference trajectory: the player's own input held for Ticks ticks.
	vec2 aRefPos[20];
	{
		CGameWorld Ref;
		Ref.CopyWorld(pRoot);
		AfStripOthers(&Ref, LocalId);
		CCharacter *pRef = Ref.GetCharacterById(LocalId);
		if(!pRef)
			return false;
		const int StartTick = Ref.GameTick();
		for(int t = 0; t < Ticks; t++)
		{
			pRef->OnDirectInput(&Current);
			Ref.m_GameTick = StartTick + t + 1;
			pRef->OnPredictedInput(&Current);
			Ref.Tick();
			CCharacter *pAfter = Ref.GetCharacterById(LocalId);
			aRefPos[t] = pAfter ? pAfter->m_Pos : (t > 0 ? aRefPos[t - 1] : pRef->m_Pos);
		}
	}

	std::vector<AfBeamPlan> vBeams;
	{
		AfBeamPlan root;
		root.First = Current;
		root.TotalDiff = 0;
		root.TotalDist = 0.0f;
		vBeams.push_back(std::move(root));
	}

	for(int t = 0; t < Ticks; t++)
	{
		std::vector<AfBeamPlan> vNext;
		vNext.reserve(vBeams.size() * (size_t)nCombos);
		for(const AfBeamPlan &beam : vBeams)
		{
			const CNetObj_PlayerInput &prev = beam.Timeline.empty() ? Current : beam.Timeline.back();
			for(int c = 0; c < nCombos; c++)
			{
				vec2 EndPos;
				if(!AfSimTimeline(pRoot, LocalId, beam.Timeline.data(), (int)beam.Timeline.size(), pCombos[c], &EndPos))
					continue; // freeze/death/teleport at some tick of this plan

				AfBeamPlan child;
				child.Timeline = beam.Timeline;
				child.Timeline.push_back(pCombos[c]);
				child.First = beam.Timeline.empty() ? pCombos[c] : beam.First;
				child.TotalDiff = beam.TotalDiff + AfInputDiff(prev, pCombos[c]);
				child.TotalDist = beam.TotalDist + distance(EndPos, aRefPos[t]);
				vNext.push_back(std::move(child));
			}
		}

		if(vNext.empty())
			return false; // no beam survived this tick

		std::sort(vNext.begin(), vNext.end(), [](const AfBeamPlan &a, const AfBeamPlan &b) {
			if(a.TotalDiff != b.TotalDiff)
				return a.TotalDiff < b.TotalDiff;
			return a.TotalDist < b.TotalDist;
		});
		if((int)vNext.size() > AF_BEAM_WIDTH)
			vNext.resize(AF_BEAM_WIDTH);
		vBeams = std::move(vNext);
	}

	if(vBeams.empty())
		return false;
	*pOutFirst = vBeams.front().First;
	return true;
}
}

int CAvoidFreeze::InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B)
{
	return AfInputDiff(A, B);
}

int CAvoidFreeze::DangerTick(CGameWorld *pWorld, int LocalId, const CNetObj_PlayerInput &Input, int Ticks)
{
	return DangerTickDelayed(pWorld, LocalId, Input, 0, Input, Ticks);
}

int CAvoidFreeze::DangerTickDelayed(CGameWorld *pWorld, int LocalId, const CNetObj_PlayerInput &DelayInput,
	int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks)
{
	CGameWorld SimWorld;
	SimWorld.CopyWorld(pWorld);
	AfStripOthers(&SimWorld, LocalId);

	CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
	if(!pChar)
		return 0;

	const bool avoidFreeze = g_Config.m_KxAfAvoidFreeze != 0;
	const bool avoidTeleport = g_Config.m_KxAfAvoidTeleport != 0;
	const bool avoidDeath = g_Config.m_KxAfAvoidDeath != 0;
	CCollision *pCol = pChar->Collision();

	const int StartTick = SimWorld.GameTick();
	for(int t = 0; t < Ticks; t++)
	{
		const CNetObj_PlayerInput &In = (t < Delay) ? DelayInput : ComboInput;
		pChar->OnDirectInput(&In);
		SimWorld.m_GameTick = StartTick + t + 1;
		pChar->OnPredictedInput(&In);
		SimWorld.Tick();

		if(!SimWorld.GetCharacterById(LocalId))
			return avoidDeath ? (t + 1) : 0;
		if(avoidFreeze && pChar->m_FreezeTime > 0)
			return t + 1;
		if(avoidDeath)
		{
			const int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
			if(Idx >= 0)
			{
				const int Tile = pCol->GetTileIndex(Idx);
				const int FTile = pCol->GetFrontTileIndex(Idx);
				if(Tile == TILE_DEATH || FTile == TILE_DEATH)
					return t + 1;
			}
		}
		if(avoidTeleport)
		{
			const int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
			if(Idx >= 0 && (pCol->IsTeleport(Idx) || pCol->IsEvilTeleport(Idx)))
				return t + 1;
		}
	}
	return 0;
}

vec2 CAvoidFreeze::NextTickPos(CGameWorld *pWorld, int LocalId, const CNetObj_PlayerInput &Input)
{
	CGameWorld SimWorld;
	SimWorld.CopyWorld(pWorld);
	AfStripOthers(&SimWorld, LocalId);

	CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
	if(!pChar)
		return vec2(0.0f, 0.0f);

	pChar->OnDirectInput(&Input);
	SimWorld.m_GameTick = SimWorld.GameTick() + 1;
	pChar->OnPredictedInput(&Input);
	SimWorld.Tick();

	if(CCharacter *pAfter = SimWorld.GetCharacterById(LocalId))
		return pAfter->m_Pos;
	return pChar->m_Pos;
}

void CAvoidFreeze::OnReset()
{
	m_WasOverriding = false;
	m_OverrideHook = 0;
	m_LastEvalTick = -1;
	m_LastEvalInput = CNetObj_PlayerInput{};
}

void CAvoidFreeze::OnUpdate()
{
}

void CAvoidFreeze::ApplyOverride()
{
	if(!g_Config.m_KxAvoidFreeze)
	{
		m_WasOverriding = false;
		m_OverrideHook = 0;
		return;
	}

	CGameClient *pGame = GameClient();
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0 || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
		return;
	CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pLocalChar)
		return;

	CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
	const CNetObj_PlayerInput Current = *pInput;

	// Per-tick gate + input-change gate.
	const int Tick = Client()->PredGameTick(g_Config.m_ClDummy);
	const bool InputChanged = mem_comp(&Current, &m_LastEvalInput, sizeof(Current)) != 0;
	if(Tick == m_LastEvalTick && !InputChanged)
		return;
	m_LastEvalTick = Tick;
	m_LastEvalInput = Current;

	int simTicks = g_Config.m_KxAfTicks;
	if(simTicks < 1) simTicks = 1;
	if(simTicks > 20) simTicks = 20;

	int ticksBefore = g_Config.m_KxAfTicksBefore;
	if(ticksBefore < 1) ticksBefore = 1;
	if(ticksBefore > 10) ticksBefore = 10;

	// "Will I fall into freeze?" — with the current input (including the
	// user's hook state) and without the hook.
	const int dangerWithCurrent = DangerTick(&pGame->m_PredictedWorld, LocalId, Current, simTicks);
	CNetObj_PlayerInput NoHook = Current;
	NoHook.m_Hook = 0;
	const int dangerWithoutHook = DangerTick(&pGame->m_PredictedWorld, LocalId, NoHook, simTicks);

	// No danger at all — safe. Release any override.
	if(dangerWithCurrent == 0 && dangerWithoutHook == 0)
	{
		if(m_WasOverriding)
		{
			if(m_OverrideHook != 0)
				pInput->m_Hook = 0; // retract AF's forced hook=1
			m_WasOverriding = false;
		}
		return;
	}

	// Danger WITH hook, but safe WITHOUT — the user's hook IS the problem.
	if(dangerWithCurrent > 0 && dangerWithoutHook == 0)
	{
		if(dangerWithCurrent > ticksBefore &&
			DangerTickDelayed(&pGame->m_PredictedWorld, LocalId, Current, 1, NoHook, simTicks) == 0)
			return; // hook is safe for now
		pInput->m_Hook = 0;
		m_WasOverriding = true;
		m_OverrideHook = 0;
		return;
	}

	// Build the per-tick candidate set: direction x jump x hook x aim.
	const bool allowDir = g_Config.m_KxAfDirection != 0;
	const bool allowJump = g_Config.m_KxAfJump != 0;
	const bool allowHook = g_Config.m_KxAfHook != 0;
	const bool allowAim = g_Config.m_KxAfAim != 0;

	int dirs[3] = {0, 0, 0};
	int dirCount = 1;
	if(allowDir) { dirs[0] = -1; dirs[1] = 0; dirs[2] = 1; dirCount = 3; }
	else { dirs[0] = Current.m_Direction; }

	int jumps[2] = {0, 0};
	int jumpCount = 1;
	if(allowJump) { jumps[0] = 0; jumps[1] = 1; jumpCount = 2; }
	else { jumps[0] = Current.m_Jump != 0 ? 1 : 0; }

	int hooks[2] = {0, 0};
	int hookCount = 1;
	if(allowHook) { hooks[0] = 0; hooks[1] = 1; hookCount = 2; }
	else { hooks[0] = Current.m_Hook != 0 ? 1 : 0; }

	// Aim angles: centered on the current aim, spread within the FOV cone.
	vec2 aimTargets[145];
	int aimCount = 1;
	aimTargets[0] = vec2((float)Current.m_TargetX, (float)Current.m_TargetY);
	if(allowAim)
	{
		int numAngles = g_Config.m_KxAfAngles;
		if(numAngles < 1) numAngles = 1;
		if(numAngles > 144) numAngles = 144;
		const float fovRad = (float)g_Config.m_KxAfFov * (3.14159265f / 180.0f);
		const float curAngle = atan2f((float)Current.m_TargetY, (float)Current.m_TargetX);
		float aimDist = sqrtf((float)Current.m_TargetX * Current.m_TargetX + (float)Current.m_TargetY * Current.m_TargetY);
		if(aimDist < 1.0f) aimDist = 100.0f;
		aimCount = 0;
		for(int a = 0; a < numAngles; a++)
		{
			const float t = (numAngles == 1) ? 0.0f : (float)a / (float)(numAngles - 1) - 0.5f;
			const float angle = curAngle + t * fovRad;
			aimTargets[aimCount++] = vec2(cosf(angle) * aimDist, sinf(angle) * aimDist);
		}
	}

	CNetObj_PlayerInput aCombos[AF_MAX_COMBOS];
	int nCombos = 0;
	auto fnAdd = [&](const CNetObj_PlayerInput &In) {
		if(nCombos < AF_MAX_COMBOS)
			aCombos[nCombos++] = In;
	};
	for(int di = 0; di < dirCount; di++)
		for(int ji = 0; ji < jumpCount; ji++)
			for(int hi = 0; hi < hookCount; hi++)
			{
				CNetObj_PlayerInput In = Current;
				In.m_Direction = dirs[di];
				In.m_Jump = jumps[ji];
				In.m_Hook = hooks[hi];
				fnAdd(In);
			}
	const int fireHook = allowHook ? 1 : (Current.m_Hook != 0 ? 1 : 0);
	if(fireHook)
		for(int ai = 1; ai < aimCount; ai++)
			for(int di = 0; di < dirCount; di++)
				for(int ji = 0; ji < jumpCount; ji++)
				{
					CNetObj_PlayerInput In = Current;
					In.m_Direction = dirs[di];
					In.m_Jump = jumps[ji];
					In.m_Hook = fireHook;
					In.m_TargetX = (int)aimTargets[ai].x;
					In.m_TargetY = (int)aimTargets[ai].y;
					fnAdd(In);
				}

	if(nCombos <= 0)
		return;

	// Beam search (width 32): every tick of the horizon gets its OWN input, and
	// the best 32 partial plans survive at every tick. The tick-0 input of the
	// best plan is applied now.
	CNetObj_PlayerInput OutFirst = Current;
	const bool Found = AfBuildBeamPlan(&pGame->m_PredictedWorld, LocalId, simTicks,
		aCombos, nCombos, Current, &OutFirst);

	if(!Found || InputDiff(Current, OutFirst) == 0)
		return; // no clean plan, or the player's own input already works

	pInput->m_Direction = OutFirst.m_Direction;
	pInput->m_Jump = OutFirst.m_Jump;
	pInput->m_Hook = OutFirst.m_Hook;
	m_WasOverriding = true;
	m_OverrideHook = OutFirst.m_Hook;

	if(allowAim && (OutFirst.m_TargetX != Current.m_TargetX || OutFirst.m_TargetY != Current.m_TargetY))
	{
		if(g_Config.m_KxAfSilent)
		{
			if(!pGame->m_Controls.m_SilentAimActive)
			{
				pGame->m_Controls.m_SilentAimActive = true;
				pGame->m_Controls.m_SilentAimOffset = vec2((float)OutFirst.m_TargetX, (float)OutFirst.m_TargetY);
			}
		}
		else
		{
			SetMousePos(pGame, g_Config.m_ClDummy, vec2((float)OutFirst.m_TargetX, (float)OutFirst.m_TargetY));
		}
	}
}