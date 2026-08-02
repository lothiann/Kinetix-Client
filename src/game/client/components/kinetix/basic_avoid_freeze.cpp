// (c) Kinetix. Basic Avoid Freeze component.
// Algorithm: "Latest Safe Tick Search" by Claude + Survival Score
//
// Each frame:
//   Test A: "Can I wait 1 more tick?" — simulate 1 tick with current input,
//           then brute-force all combos on remaining ticks. If ANY combo
//           eliminates danger (danger=0) → wait, don't intervene.
//   Test B: "If I act NOW — will it work?" — brute-force all combos from
//           tick 0. Score by survival (how long before danger). Higher = better.
//           Among same survival, prefer minimal input diff.
//
//   If Test A passes → wait (don't intervene).
//   If Test A fails but Test B passes → LAST MOMENT, apply best combo.
//   If both fail → too late, danger inevitable.
//
// Optimization: only run when there's danger within simTicks with current input.

#include <game/client/components/kinetix/basic_avoid_freeze.h>

#include <engine/shared/config.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/mapitems.h>
#include <generated/protocol.h>

void CBasicAvoidFreeze::OnReset()
{
}

int CBasicAvoidFreeze::InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const
{
	int diff = 0;
	if(A.m_Direction != B.m_Direction) diff++;
	if((A.m_Jump != 0) != (B.m_Jump != 0)) diff++;
	if((A.m_Hook != 0) != (B.m_Hook != 0)) diff++;
	return diff;
}

int CBasicAvoidFreeze::SimulateDangerTick(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const
{
	return SimulateDangerTickDelayed(LocalId, Input, 0, Input, Ticks);
}

int CBasicAvoidFreeze::SimulateDangerTickDelayed(int LocalId, const CNetObj_PlayerInput &DelayInput,
	int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks) const
{
	CGameClient *pGame = GameClient();
	CGameWorld SimWorld;
	SimWorld.CopyWorld(&pGame->m_PredictedWorld);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId)
			continue;
		if(CCharacter *pChar = SimWorld.GetCharacterById(i))
			delete pChar;
	}

	CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
	if(!pChar)
		return 0;

	bool avoidFreeze = g_Config.m_KxBafAvoidFreeze != 0;
	bool avoidTeleport = g_Config.m_KxBafAvoidTeleport != 0;
	bool avoidDeath = g_Config.m_KxBafAvoidDeath != 0;
	CCollision *pCol = pChar->Collision();

	int StartTick = SimWorld.GameTick();
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
			int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
			if(Idx >= 0)
			{
				int Tile = pCol->GetTileIndex(Idx);
				int FTile = pCol->GetFrontTileIndex(Idx);
				if(Tile == TILE_DEATH || FTile == TILE_DEATH)
					return t + 1;
			}
		}

		if(avoidTeleport)
		{
			int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
			if(Idx >= 0)
			{
				if(pCol->IsTeleport(Idx) || pCol->IsEvilTeleport(Idx))
					return t + 1;
			}
		}
	}
	return 0;
}

void CBasicAvoidFreeze::OnUpdate()
{
}

void CBasicAvoidFreeze::ApplyOverride()
{
	if(!g_Config.m_KxBasicAvoidFreeze)
		return;

	CGameClient *pGame = GameClient();
	int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0 || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
		return;

	CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pLocalChar)
		return;

	CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
	CNetObj_PlayerInput Current = *pInput;

	int simTicks = g_Config.m_KxBafTicks;
	if(simTicks < 1) simTicks = 1;
	if(simTicks > 20) simTicks = 20;

	// Optimization: only run if there's danger with current input within simTicks.
	int currentDanger = SimulateDangerTick(LocalId, Current, simTicks);
	if(currentDanger == 0)
		return; // no danger at all — nothing to avoid

	// Build combo lists.
	bool allowDir = g_Config.m_KxBafDirection != 0;
	bool allowJump = g_Config.m_KxBafJump != 0;
	bool allowHook = g_Config.m_KxBafHook != 0;

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

	// =====================================================
	// Test A: "Can I wait 1 more tick?"
	// Simulate 1 tick with Current, then each combo on remaining ticks.
	// If ANY combo eliminates danger (danger=0) → can wait.
	// =====================================================
	bool canWait = false;
	for(int di = 0; di < dirCount && !canWait; di++)
	{
		for(int ji = 0; ji < jumpCount && !canWait; ji++)
		{
			for(int hi = 0; hi < hookCount && !canWait; hi++)
			{
				CNetObj_PlayerInput test = Current;
				test.m_Direction = dirs[di];
				test.m_Jump = jumps[ji];
				test.m_Hook = hooks[hi];

				// 1 tick with Current, then combo on remaining ticks.
				int delayDanger = SimulateDangerTickDelayed(LocalId, Current, 1, test, simTicks);
				if(delayDanger == 0)
				{
					// Combo eliminates danger even after waiting 1 tick → can wait.
					canWait = true;
				}
			}
		}
	}

	// If we can wait 1 more tick — don't intervene.
	if(canWait)
		return;

	// =====================================================
	// Test B: "Act NOW" — brute-force all combos from tick 0.
	// Score by survival: how many ticks before danger.
	// danger=0 → fully safe → survival = simTicks (best).
	// danger=N → danger at tick N → survival = N-1.
	// Higher survival = better. Among same survival, prefer minimal diff.
	// =====================================================
	bool found = false;
	CNetObj_PlayerInput bestInput = Current;
	int bestSurvival = -1;
	int bestDiff = 999;

	for(int di = 0; di < dirCount; di++)
	{
		for(int ji = 0; ji < jumpCount; ji++)
		{
			for(int hi = 0; hi < hookCount; hi++)
			{
				CNetObj_PlayerInput test = Current;
				test.m_Direction = dirs[di];
				test.m_Jump = jumps[ji];
				test.m_Hook = hooks[hi];

				// Skip exact current input.
				if(test.m_Direction == Current.m_Direction &&
				   (test.m_Jump != 0) == (Current.m_Jump != 0) &&
				   (test.m_Hook != 0) == (Current.m_Hook != 0))
					continue;

				int danger = SimulateDangerTick(LocalId, test, simTicks);
				// Convert danger tick to survival: 0 = simTicks (fully safe), N = N-1.
				int survival = (danger == 0) ? simTicks : danger - 1;
				int diff = InputDiff(Current, test);

				if(bestSurvival < 0 ||
				   survival > bestSurvival ||
				   (survival == bestSurvival && diff < bestDiff))
				{
					bestSurvival = survival;
					bestDiff = diff;
					bestInput = test;
					found = true;
				}
			}
		}
	}

	// Apply if best combo survives longer than current.
	// currentDanger is 1-based tick. currentSurvival = currentDanger - 1.
	int currentSurvival = currentDanger - 1;
	if(found && bestSurvival > currentSurvival)
	{
		pInput->m_Direction = bestInput.m_Direction;
		pInput->m_Jump = bestInput.m_Jump;
		pInput->m_Hook = bestInput.m_Hook;
	}
}
