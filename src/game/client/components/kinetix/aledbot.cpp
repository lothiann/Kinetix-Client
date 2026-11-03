#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// v6.9: AledBot — greedy per-tick aled controller.
//
// The algorithm was validated on a physics polygon (DDNet-PyEngine) against
// 1- and 2-tile horizontal / vertical / diagonal aled maps.
//
// Every tick, while a frozen (or, with Trigger Ticks, an about-to-freeze)
// dummy exists:
//   1. target dummy = best frozen dummy by kx_aled_sort (nearest to us etc.)
//      kx_aled_trigger_ticks > 0 also accepts a dummy that will enter a freeze
//      within that many ticks - it is treated exactly as if it were frozen
//   2. simulate KxAledTicks ticks of the target with everyone holding their
//      input; the tiles before the first freeze tile are ignored (the target
//      may still be falling through the air) -> the first NON-freeze tile
//      after the freeze run is the target tile (a stationary target: the
//      nearest non-freeze tile around the freeze, on the actor's side)
//   3. enumerate EVERY valid single-tick input of the acting dummy:
//      direction x jump, hook steering while attached, 24 ray-validated hook
//      fire angles and one dedicated ray at the target dummy
//   4. simulate each candidate one tick and score it:
//        closeness(actor, target) + closeness(target, target tile)
//      (hooking the target scores slightly better - it is what closes a
//      vertical gap or drags the target through a 2-tile freeze)
//   5. the best candidate is the input for this tick
//   6. if the target's centre is in a non-freeze tile AND the engine confirms
//      the hammer hit really unfreezes it for good (hit lands, the target is
//      not re-frozen within 3 ticks) -> aim at it and press, else do NOT
//      press (a press that cannot hit only wastes the hammer reload)
//
// No plan buffer, no tree, no backtracking: one decision per tick, re-derived
// from the live predicted world every tick. Hitting through 1-2 freeze tiles
// works because the hammer resolves against the predicted world - a freeze
// tile between the tees neither blocks the strike nor the hook.

static bool AledIsFreezeTile(CGameClient *pGame, vec2 Pos)
{
	CCollision *pCollision = pGame->Collision();
	if(!pCollision)
		return false;
	const int Index = pCollision->GetMapIndex(Pos);
	if(Index < 0)
		return false;
	// HandleTiles checks both layers (game tile and front tile), so the aled
	// validation has to as well.
	const int Tile = pCollision->GetTileIndex(Index);
	const int Front = pCollision->GetFrontTileIndex(Index);
	auto IsFreeze = [](int T) { return T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE; };
	return IsFreeze(Tile) || IsFreeze(Front);
}

// Frozen target detection. The predicted character's freeze timer alone is not
// enough: for the ACTIVE dummy the client prediction can already run the last
// freeze ticks (m_CanMoveInFreeze) while the server still has the tee frozen,
// so the same snapshot check the dummy helpers / freeze flakes use is applied
// as well (m_FreezeEnd / m_LiveFrozen / m_DeepFrozen).
static bool AledIsFrozen(CGameClient *pGame, int Cid, const CCharacter *pChar)
{
	if(!pChar || pChar->Core()->m_DeepFrozen || pChar->Core()->m_LiveFrozen)
		return false;
	if(Cid < 0 || Cid >= MAX_CLIENTS)
		return pChar->m_FreezeTime > 0;
	if(pGame->m_aClients[Cid].m_DeepFrozen)
		return false;
	if(pChar->m_FreezeTime > 0)
		return true;
	return pGame->m_aClients[Cid].m_FreezeEnd != 0 || pGame->m_aClients[Cid].m_LiveFrozen;
}

static void AledSort(int *pSlots, long long *pKeys, int Count)
{
	for(int i = 1; i < Count; i++)
	{
		const int CS = pSlots[i];
		const long long CK = pKeys[i];
		int j = i;
		while(j > 0 && (pKeys[j - 1] > CK || (pKeys[j - 1] == CK && pSlots[j - 1] > CS)))
		{
			pKeys[j] = pKeys[j - 1];
			pSlots[j] = pSlots[j - 1];
			j--;
		}
		pKeys[j] = CK;
		pSlots[j] = CS;
	}
}

// Drop a held input: stop the tee, release the hammer and clear the direct
// weapon request so nothing leaks into the dummy-swap pipeline.
// m_NextWeapon / m_PrevWeapon are deliberately left untouched: they are 6-bit
// counters and HandleWeaponSwitch turns any discontinuity into a burst of
// CountInput "presses", which cycles the dummy's weapon (usually to the gun).
static void AledDropInput(CNetObj_PlayerInput *pInput)
{
	pInput->m_Direction = 0;
	pInput->m_Jump = 0;
	pInput->m_Hook = 0;
	pInput->m_Fire = (pInput->m_Fire + 1) & ~1;
	pInput->m_WantedWeapon = 0;
}

// One predicted-world tick: every character applies its own input, the actor
// uses the candidate input when one is given. Matches the client prediction
// sequence (OnDirectInput -> advance tick -> OnPredictedInput -> Tick).
static void AledTickWorld(CGameWorld *pWorld, int ActorCid, const CNetObj_PlayerInput *pActorIn)
{
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		CCharacter *pChar = pWorld->GetCharacterById(c);
		if(!pChar)
			continue;
		CNetObj_PlayerInput In = pChar->Core()->m_Input;
		if(c == ActorCid && pActorIn)
			In = *pActorIn;
		pChar->OnDirectInput(&In);
	}
	pWorld->m_GameTick = pWorld->GameTick() + 1;
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		CCharacter *pChar = pWorld->GetCharacterById(c);
		if(!pChar)
			continue;
		CNetObj_PlayerInput In = pChar->Core()->m_Input;
		if(c == ActorCid && pActorIn)
			In = *pActorIn;
		pChar->OnPredictedInput(&In);
	}
	pWorld->Tick();
}

// Manual per-tee state copy (the TAS playground pattern). CopyWorld copies the
// entity, but the state of the two tees the search depends on is restored
// explicitly: CCharacterFullState (input queues, reload, tune zone, weapon
// switch, hammer hit list) plus the whole CCharacterCore (position, velocity,
// hook, freeze, and the jump counters m_Jumped / m_JumpedTotal / m_Jumps).
// SetCoreWorld is what keeps the copied core bound to the COPY: hook drags and
// player-vs-player collision inside the simulation then resolve against the
// copied tees instead of the live ones.
static void AledCopyTee(CGameWorld *pDst, CGameWorld *pSrc, int Cid)
{
	CCharacter *pD = pDst->GetCharacterById(Cid);
	CCharacter *pS = pSrc->GetCharacterById(Cid);
	if(!pD || !pS)
		return;
	CCharacterFullState Full;
	pS->SaveFullState(&Full);
	pD->LoadFullState(Full);
	pD->SetCore(pS->GetCore());
	pD->SetCoreWorld(pDst);
	pD->m_Pos = pS->m_Pos;
}

// Trigger Ticks: the dummy is treated as frozen this many ticks BEFORE it
// actually enters the freeze. The target alone is ticked with its own input
// (every other tee is left untouched), so a dummy falling / riding into a
// freeze is picked up while it is still in the air.
static bool AledWillFreeze(CGameClient *pGame, int TargetCid, int Ticks)
{
	if(Ticks <= 0)
		return false;
	CCharacter *pTarget = pGame->m_PredictedWorld.GetCharacterById(TargetCid);
	if(!pTarget || pTarget->Core()->m_DeepFrozen || pTarget->Core()->m_LiveFrozen)
		return false;
	CGameWorld World;
	World.CopyWorld(&pGame->m_PredictedWorld);
	World.m_WorldConfig.m_PredictWeapons = true;
	World.m_WorldConfig.m_PredictDDRace = true;
	World.m_WorldConfig.m_PredictFreeze = 1;
	AledCopyTee(&World, &pGame->m_PredictedWorld, TargetCid);
	for(int t = 0; t < Ticks; t++)
	{
		AledTickWorld(&World, -1, nullptr);
		CCharacter *pT = World.GetCharacterById(TargetCid);
		if(!pT)
			return false;
		if(pT->m_FreezeTime > 0 || pT->Core()->m_DeepFrozen)
			return true;
		if(AledIsFreezeTile(pGame, pT->GetPos()))
			return true;
	}
	return false;
}

// The aled target predicate: frozen now, standing on a freeze tile, or about
// to be frozen within Trigger Ticks. In the last case the non-freeze state is
// ignored on purpose — the zone, the plan and the press are all built as if
// the dummy were already frozen.
static bool AledTargetEngaged(CGameClient *pGame, int Cid, CCharacter *pChar)
{
	if(!pChar || pChar->Core()->m_DeepFrozen || pChar->Core()->m_LiveFrozen)
		return false;
	if(Cid >= 0 && Cid < MAX_CLIENTS && pGame->m_aClients[Cid].m_DeepFrozen)
		return false;
	if(AledIsFrozen(pGame, Cid, pChar))
		return true;
	if(AledIsFreezeTile(pGame, pChar->GetPos()))
		return true;
	return AledWillFreeze(pGame, Cid, g_Config.m_KxAledTriggerTicks);
}

// Distance-based freeze avoidance for the acting tee. Touching a freeze tile
// locks the tee for three seconds and disables its hammer, so the actor must
// never hug the freeze. The binary penalty in the candidate score only fires
// once the tee is already inside; in the client prediction that happens a tick
// or two before the server sees it, and then EVERY candidate carries the same
// penalty and it stops discriminating — the tee keeps walking deeper toward
// the target (or gets dragged in by its own hook). This margin starts pushing
// one tee radius before the freeze and ranks candidates by how far they get
// back out if the tee already ended up on one.
static float AledFreezeMarginPenalty(CGameClient *pGame, vec2 Pos)
{
	const float Margin = 20.0f; // a little more than the tee radius (14)
	float Best = Margin;
	const int Tx = (int)floorf(Pos.x / 32.0f);
	const int Ty = (int)floorf(Pos.y / 32.0f);
	for(int dy = -1; dy <= 1; dy++)
	{
		for(int dx = -1; dx <= 1; dx++)
		{
			const int X = Tx + dx;
			const int Y = Ty + dy;
			if(!AledIsFreezeTile(pGame, vec2((float)(X * 32 + 16), (float)(Y * 32 + 16))))
				continue;
			const float MinX = (float)(X * 32);
			const float MinY = (float)(Y * 32);
			const float Dx = Pos.x < MinX ? MinX - Pos.x : (Pos.x > MinX + 32.0f ? Pos.x - (MinX + 32.0f) : 0.0f);
			const float Dy = Pos.y < MinY ? MinY - Pos.y : (Pos.y > MinY + 32.0f ? Pos.y - (MinY + 32.0f) : 0.0f);
			const float D = sqrtf(Dx * Dx + Dy * Dy);
			if(D < Best)
				Best = D;
		}
	}
	return (Margin - Best) * 50.0f;
}

// Nearest non-freeze, non-solid tile around the target, tie-broken toward the
// actor: the hook drag is the primary mover, it pulls the target toward us.
static bool AledNearestFreeTile(CGameClient *pGame, vec2 TargetPos, vec2 ActorPos, int &OutX, int &OutY, int Skip = 0)
{
	CCollision *pColl = pGame->Collision();
	if(!pColl)
		return false;
	const int Tx = (int)floorf(TargetPos.x / 32.0f);
	const int Ty = (int)floorf(TargetPos.y / 32.0f);
	for(int r = 1; r <= 8; r++)
	{
		float aD[64];
		int aX[64], aY[64];
		int Num = 0;
		for(int dy = -r; dy <= r; dy++)
		{
			for(int dx = -r; dx <= r; dx++)
			{
				const int Adx = dx < 0 ? -dx : dx; const int Ady = dy < 0 ? -dy : dy; const int Ring = Adx > Ady ? Adx : Ady;
				if(Ring != r)
					continue;
				const int x = Tx + dx, y = Ty + dy;
				const vec2 C((float)(x * 32 + 16), (float)(y * 32 + 16));
				if(pColl->CheckPoint(C))
					continue; // solid
				if(AledIsFreezeTile(pGame, C))
					continue;
				const int Index = pColl->GetMapIndex(C);
				if(Index >= 0 && pColl->GetTileIndex(Index) == TILE_DEATH)
					continue;
				if(Num >= 64)
					break;
				aD[Num] = distance(C, ActorPos);
				aX[Num] = x;
				aY[Num] = y;
				Num++;
			}
		}
		if(Num == 0)
			continue;
		// sorted by distance to the actor: the first cell is the natural
		// target, the next ones are the retry targets
		for(int i = 1; i < Num; i++)
		{
			const float D = aD[i];
			const int X = aX[i], Y = aY[i];
			int j = i;
			while(j > 0 && aD[j - 1] > D)
			{
				aD[j] = aD[j - 1];
				aX[j] = aX[j - 1];
				aY[j] = aY[j - 1];
				j--;
			}
			aD[j] = D;
			aX[j] = X;
			aY[j] = Y;
		}
		if(Skip < Num)
		{
			OutX = aX[Skip];
			OutY = aY[Skip];
			return true;
		}
		Skip -= Num;
	}
	return false;
}

// Trajectory phase: first non-freeze tile the frozen target's centre enters
// within Ticks ticks. Everyone but the acting dummy holds their input (the
// player's hook drag is the target's motor); the actor is idled so its own
// movement cannot affect the trajectory.
static bool AledFindTargetTile(CGameClient *pGame, int ActorCid, int TargetCid, int Ticks, int &OutX, int &OutY, int Skip = 0)
{
	CCharacter *pTarget = pGame->m_PredictedWorld.GetCharacterById(TargetCid);
	CCharacter *pActor = pGame->m_PredictedWorld.GetCharacterById(ActorCid);
	if(!pTarget || !pActor)
		return false;

	// frozen but already off every freeze tile: its own tile is the target
	if(pTarget->m_FreezeTime > 0 && !AledIsFreezeTile(pGame, pTarget->GetPos()))
	{
		OutX = (int)floorf(pTarget->GetPos().x / 32.0f);
		OutY = (int)floorf(pTarget->GetPos().y / 32.0f);
		return true;
	}

	CGameWorld World;
	World.CopyWorld(&pGame->m_PredictedWorld);
	World.m_WorldConfig.m_PredictWeapons = true;
	World.m_WorldConfig.m_PredictDDRace = true;
	World.m_WorldConfig.m_PredictFreeze = 1;
	AledCopyTee(&World, &pGame->m_PredictedWorld, ActorCid);
	AledCopyTee(&World, &pGame->m_PredictedWorld, TargetCid);

	bool SeenFreeze = false;
	// Skip selects which tile along the trajectory becomes the target: 0 = the
	// first non-freeze tile, 1 = the next one, ... Only a tile CHANGE counts,
	// so a target resting in one free tile is not counted over and over.
	int Crossings = 0;
	bool HaveLast = false;
	int LastTx = 0, LastTy = 0;
	for(int t = 0; t < Ticks; t++)
	{
		CCharacter *pT = World.GetCharacterById(TargetCid);
		CCharacter *pA = World.GetCharacterById(ActorCid);
		if(!pT || !pA)
			break;
		const bool WasFrozen = pT->m_FreezeTime > 0;
		const bool WasOnFreeze = AledIsFreezeTile(pGame, pT->GetPos());
		CNetObj_PlayerInput Idle = pA->Core()->m_Input;
		Idle.m_Direction = 0;
		Idle.m_Jump = 0;
		Idle.m_Hook = 0;
		AledTickWorld(&World, ActorCid, &Idle);
		pT = World.GetCharacterById(TargetCid);
		if(!pT)
			break;
		if(WasFrozen && WasOnFreeze)
			SeenFreeze = true;
		if(WasFrozen && pT->m_FreezeTime == 0 && !AledIsFreezeTile(pGame, pT->GetPos()))
			return false; // unfreezes on its own — nothing to aled
		if(WasFrozen && pT->m_FreezeTime > 0 && SeenFreeze && !AledIsFreezeTile(pGame, pT->GetPos()))
		{
			const int Cx = (int)floorf(pT->GetPos().x / 32.0f);
			const int Cy = (int)floorf(pT->GetPos().y / 32.0f);
			if(!HaveLast || Cx != LastTx || Cy != LastTy)
			{
				HaveLast = true;
				LastTx = Cx;
				LastTy = Cy;
				if(Crossings == Skip)
				{
					OutX = Cx;
					OutY = Cy;
					return true;
				}
				Crossings++;
			}
		}
	}
	return AledNearestFreeTile(pGame, pTarget->GetPos(), pActor->GetPos(), OutX, OutY, Skip);
}

// Every valid single-tick input of the acting dummy (the same set the physics
// pathfinder expands): direction x jump, hook steering while attached, 24
// ray-validated hook fire angles and one dedicated ray at the target.
#define ALED_MAX_CANDS 96

static int AledBuildCandidates(CGameClient *pGame, CGameWorld *pWorld, int ActorCid, int TargetCid, CNetObj_PlayerInput *pOut, int Max)
{
	CCharacter *pActor = pWorld->GetCharacterById(ActorCid);
	CCharacter *pTarget = pWorld->GetCharacterById(TargetCid);
	if(!pActor || !pTarget)
		return 0;
	CCollision *pColl = pGame->Collision();
	if(!pColl)
		return 0;

	const vec2 AP = pActor->GetPos();
	const vec2 TP = pTarget->GetPos();
	const float Dx = TP.x - AP.x;
	const int Toward = Dx > 6.0f ? 1 : (Dx < -6.0f ? -1 : 0);
	const int Ax = (int)(TP.x - AP.x);
	const int Ay = (int)(TP.y - AP.y);
	const CCharacterCore *pCore = pActor->Core();

	int n = 0;
	auto Add = [&](int Dir, int Jump, int Hook, int Tx, int Ty) {
		if(n >= Max)
			return;
		CNetObj_PlayerInput In;
		mem_zero(&In, sizeof(In));
		In.m_Direction = Dir;
		In.m_Jump = Jump;
		In.m_Hook = Hook;
		In.m_TargetX = Tx;
		In.m_TargetY = Ty;
		In.m_WantedWeapon = WEAPON_HAMMER + 1;
		for(int i = 0; i < n; i++)
			if(mem_comp(&pOut[i], &In, sizeof(In)) == 0)
				return;
		pOut[n++] = In;
	};

	// 1) base movement, aim at the target (hammer stays ready)
	for(int d = -1; d <= 1; d++)
		for(int j = 0; j <= 1; j++)
			Add(d, j, 0, Ax, Ay);

	// 2) hook steering while attached
	if(pCore->m_HookState == HOOK_FLYING || pCore->m_HookState == HOOK_GRABBED)
	{
		const int Hx = (int)(pCore->m_HookPos.x - AP.x);
		const int Hy = (int)(pCore->m_HookPos.y - AP.y);
		for(int d = -1; d <= 1; d++)
			for(int j = 0; j <= 1; j++)
				Add(d, j, 1, Hx, Hy);
	}

	// 3) 24 ray-validated hook fire angles
	if(pCore->m_HookState == HOOK_IDLE)
	{
		const float HookLen = pCore->m_Tuning.m_HookLength;
		for(int a = 0; a < 24; a++)
		{
			const float t = (float)a / 23.0f;
			const float Ang = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			const vec2 End = AP + vec2(cosf(Ang), sinf(Ang)) * HookLen;
			vec2 HitPos;
			const int Hit = pColl->IntersectLineTeleHook(AP, End, &HitPos, nullptr);
			if(!(Hit == TILE_SOLID || Hit == TILE_TELEINHOOK || (Hit > 0 && Hit != TILE_NOHOOK && Hit != TILE_DEATH)))
				continue;
			const int Tx = (int)(cosf(Ang) * 256.0f);
			const int Ty = (int)(sinf(Ang) * 256.0f);
			Add(0, 0, 1, Tx, Ty);
			if(Toward != 0)
				Add(Toward, 0, 1, Tx, Ty);
		}
		// 4) the dedicated ray at the target dummy
		vec2 HitPos;
		const int Hit = pColl->IntersectLineTeleHook(AP, TP, &HitPos, nullptr);
		const bool Blocked = Hit != 0 && distance(AP, HitPos) < distance(AP, TP) - 32.0f;
		if(!Blocked)
		{
			Add(0, 0, 1, Ax, Ay);
			if(Toward != 0)
				Add(Toward, 0, 1, Ax, Ay);
		}
	}
	return n;
}

// ---------------------------------------------------------------------------
// Rescue zone.
//
// The frozen target is simulated along its own trajectory. When its centre
// reaches the first non-freeze tile, the wall thickness is known (how many
// freeze tiles it crossed). For a 1-tile wall a small BFS is grown from that
// exit tile (radius 5), keeping only free cells 4-directionally adjacent to a
// freeze tile - the edge of the freeze, where the rescued tee cannot fall
// straight back in. For a 2-tile wall the exit tile alone is used. Every cell
// carries a score (BFS depth, the exit tile is best).
// ---------------------------------------------------------------------------

struct SAledZoneCell
{
	int X, Y;
	int Score;
};

struct SAledZone
{
	int Thickness;
	int ExitX, ExitY;
	SAledZoneCell aCells[64];
	int Num;
};

static bool AledTileAdjacentToFreeze(CGameClient *pGame, int X, int Y)
{
	return AledIsFreezeTile(pGame, vec2((float)(X * 32 + 16), (float)(Y * 32 + 16 - 32))) ||
		AledIsFreezeTile(pGame, vec2((float)(X * 32 + 16), (float)(Y * 32 + 16 + 32))) ||
		AledIsFreezeTile(pGame, vec2((float)(X * 32 + 16 - 32), (float)(Y * 32 + 16))) ||
		AledIsFreezeTile(pGame, vec2((float)(X * 32 + 16 + 32), (float)(Y * 32 + 16)));
}

static bool AledBuildZoneIn(CGameClient *pGame, CGameWorld *pSrcWorld, int ActorCid, int TargetCid, int Ticks, SAledZone *pOut)
{
	pOut->Num = 0;
	pOut->Thickness = 1;
	pOut->ExitX = 0;
	pOut->ExitY = 0;
	CCharacter *pTarget = pSrcWorld->GetCharacterById(TargetCid);
	CCharacter *pActor = pSrcWorld->GetCharacterById(ActorCid);
	if(!pTarget || !pActor)
		return false;

	CGameWorld World;
	World.CopyWorld(pSrcWorld);
	World.m_WorldConfig.m_PredictWeapons = true;
	World.m_WorldConfig.m_PredictDDRace = true;
	World.m_WorldConfig.m_PredictFreeze = 1;
	AledCopyTee(&World, pSrcWorld, ActorCid);
	AledCopyTee(&World, pSrcWorld, TargetCid);

	// Trigger Ticks: the target may still be in the air on its way into the
	// freeze, so the tiles before the first freeze tile are ignored. Only the
	// freeze run defines the wall; the first tile after it is the exit.
	bool SeenFreeze = false;
	bool InFreeze = false;
	int Thickness = 0;
	int LastFx = -9999, LastFy = -9999;
	bool HaveExit = false;
	int Ex = 0, Ey = 0;
	for(int t = 0; t < Ticks; t++)
	{
		CCharacter *pT = World.GetCharacterById(TargetCid);
		CCharacter *pA = World.GetCharacterById(ActorCid);
		if(!pT || !pA)
			break;
		const bool OnFreeze = AledIsFreezeTile(pGame, pT->GetPos());
		const int Px = (int)floorf(pT->GetPos().x / 32.0f);
		const int Py = (int)floorf(pT->GetPos().y / 32.0f);
		if(OnFreeze)
		{
			SeenFreeze = true;
			InFreeze = true;
			if(Px != LastFx || Py != LastFy)
			{
				LastFx = Px;
				LastFy = Py;
				Thickness++;
			}
		}
		else if(InFreeze)
		{
			Ex = Px;
			Ey = Py;
			HaveExit = true;
			break;
		}
		// the hook drag is the target's motor: keep it in the simulation
		CNetObj_PlayerInput Idle = pA->Core()->m_Input;
		Idle.m_Direction = 0;
		Idle.m_Jump = 0;
		if(pA->Core()->m_HookState == HOOK_FLYING || pA->Core()->m_HookState == HOOK_GRABBED)
		{
			Idle.m_Hook = 1;
			Idle.m_TargetX = (int)(pA->Core()->m_HookPos.x - pA->GetPos().x);
			Idle.m_TargetY = (int)(pA->Core()->m_HookPos.y - pA->GetPos().y);
		}
		else
		{
			Idle.m_Hook = 0;
			Idle.m_TargetX = 0;
			Idle.m_TargetY = 0;
		}
		AledTickWorld(&World, ActorCid, &Idle);
	}

	if(!HaveExit)
	{
		// The target's trajectory does not cross freeze within the horizon
		// (it is frozen while already off the freeze, e.g. after being
		// dragged out).  The press spot is still STRICTLY the BFS area
		// hugging the nearest freeze boundary; if no freeze tile is within
		// the BFS radius there is no zone and no press at all.
		const int Tx = (int)floorf(pTarget->GetPos().x / 32.0f);
		const int Ty = (int)floorf(pTarget->GetPos().y / 32.0f);
		int BestX = 0, BestY = 0, BestD = 1 << 30;
		for(int dy = -5; dy <= 5; dy++)
			for(int dx = -5; dx <= 5; dx++)
			{
				const int X = Tx + dx, Y = Ty + dy;
				if(!AledIsFreezeTile(pGame, vec2((float)(X * 32 + 16), (float)(Y * 32 + 16))))
					continue;
				const int D = dx * dx + dy * dy;
				if(D < BestD)
				{
					BestD = D;
					BestX = X;
					BestY = Y;
				}
			}
		if(BestD == (1 << 30))
			return false;
		static const int aDX[4] = {1, -1, 0, 0};
		static const int aDY[4] = {0, 0, 1, -1};
		CCollision *pColl = pGame->Collision();
		int SeedX = 0, SeedY = 0, SeedD = 1 << 30;
		for(int i = 0; i < 4; i++)
		{
			const int X = BestX + aDX[i], Y = BestY + aDY[i];
			const vec2 C((float)(X * 32 + 16), (float)(Y * 32 + 16));
			if(AledIsFreezeTile(pGame, C) || (pColl && pColl->CheckPoint(C)))
				continue;
			const int D = (X - Tx) * (X - Tx) + (Y - Ty) * (Y - Ty);
			if(D < SeedD)
			{
				SeedD = D;
				SeedX = X;
				SeedY = Y;
			}
		}
		if(SeedD == (1 << 30))
			return false;
		Ex = SeedX;
		Ey = SeedY;
		Thickness = 1;
	}

	pOut->Thickness = Thickness;
	pOut->ExitX = Ex;
	pOut->ExitY = Ey;

	if(Thickness >= 2)
	{
		// 2-tile wall: the exit tile alone
		pOut->aCells[0].X = Ex;
		pOut->aCells[0].Y = Ey;
		pOut->aCells[0].Score = 0;
		pOut->Num = 1;
		return true;
	}

	// 1-tile wall: BFS around the exit (radius 5), only free cells 4-adjacent
	// to a freeze tile
	struct SAledQueue
	{
		int X, Y, D;
	};
	SAledQueue aQueue[256];
	int Head = 0, Tail = 0;
	bool aSeen[11 * 11];
	mem_zero(aSeen, sizeof(aSeen));
	aQueue[Tail].X = Ex;
	aQueue[Tail].Y = Ey;
	aQueue[Tail].D = 0;
	Tail++;
	aSeen[5 * 11 + 5] = true;
	CCollision *pColl = pGame->Collision();
	while(Head < Tail && pOut->Num < 64)
	{
		const SAledQueue C = aQueue[Head++];
		const vec2 Center((float)(C.X * 32 + 16), (float)(C.Y * 32 + 16));
		if(pColl && !pColl->CheckPoint(Center) && !AledIsFreezeTile(pGame, Center) &&
			AledTileAdjacentToFreeze(pGame, C.X, C.Y))
		{
			pOut->aCells[pOut->Num].X = C.X;
			pOut->aCells[pOut->Num].Y = C.Y;
			pOut->aCells[pOut->Num].Score = C.D;
			pOut->Num++;
		}
		static const int aDX[4] = {1, -1, 0, 0};
		static const int aDY[4] = {0, 0, 1, -1};
		for(int i = 0; i < 4; i++)
		{
			const int Nx = C.X + aDX[i];
			const int Ny = C.Y + aDY[i];
			const int Rx = Nx - Ex;
			const int Ry = Ny - Ey;
			const int Adx = Rx < 0 ? -Rx : Rx;
			const int Ady = Ry < 0 ? -Ry : Ry;
			if(Adx > 5 || Ady > 5)
				continue;
			const int Idx = (Ry + 5) * 11 + (Rx + 5);
			if(aSeen[Idx])
				continue;
			aSeen[Idx] = true;
			aQueue[Tail].X = Nx;
			aQueue[Tail].Y = Ny;
			aQueue[Tail].D = C.D + 1;
			Tail++;
		}
	}
	if(pOut->Num == 0)
	{
		pOut->aCells[0].X = Ex;
		pOut->aCells[0].Y = Ey;
		pOut->aCells[0].Score = 0;
		pOut->Num = 1;
	}
	return true;
}

static bool AledBuildZone(CGameClient *pGame, int ActorCid, int TargetCid, int Ticks, SAledZone *pOut)
{
	return AledBuildZoneIn(pGame, &pGame->m_PredictedWorld, ActorCid, TargetCid, Ticks, pOut);
}

static float AledZoneNearestDist(const SAledZone *pZone, vec2 Pos)
{
	float Best = 1e18f;
	for(int i = 0; i < pZone->Num; i++)
	{
		const vec2 C((float)(pZone->aCells[i].X * 32 + 16), (float)(pZone->aCells[i].Y * 32 + 16));
		const float S = distance(Pos, C) + (float)pZone->aCells[i].Score * 12.0f;
		if(S < Best)
			Best = S;
	}
	return Best;
}

static bool AledZoneContains(const SAledZone *pZone, int X, int Y)
{
	for(int i = 0; i < pZone->Num; i++)
		if(pZone->aCells[i].X == X && pZone->aCells[i].Y == Y)
			return true;
	return false;
}

// A knock press on a still-frozen target: the hammer unfreezes it for a moment
// and the knock pushes it toward the rescue zone. Only useful when the hook is
// not already dragging it (otherwise the reload is wasted and the final press
// gets blocked).
static bool AledKnockWouldHelp(CGameClient *pGame, int ActorCid, int TargetCid, const SAledZone *pZone)
{
	CCharacter *pActor = pGame->m_PredictedWorld.GetCharacterById(ActorCid);
	CCharacter *pTarget = pGame->m_PredictedWorld.GetCharacterById(TargetCid);
	if(!pActor || !pTarget || pActor->GetReloadTimer() != 0)
		return false;
	if(pActor->Core()->m_HookState == HOOK_GRABBED && pActor->Core()->HookedPlayer() == TargetCid)
		return false;
	CGameWorld World;
	World.CopyWorld(&pGame->m_PredictedWorld);
	World.m_WorldConfig.m_PredictWeapons = true;
	World.m_WorldConfig.m_PredictDDRace = true;
	World.m_WorldConfig.m_PredictFreeze = 1;
	AledCopyTee(&World, &pGame->m_PredictedWorld, ActorCid);
	AledCopyTee(&World, &pGame->m_PredictedWorld, TargetCid);
	CCharacter *pA = World.GetCharacterById(ActorCid);
	CCharacter *pT = World.GetCharacterById(TargetCid);
	if(!pA || !pT)
		return false;
	CNetObj_PlayerInput In = pA->Core()->m_Input;
	In.m_Direction = 0;
	In.m_Jump = 0;
	In.m_Hook = 0;
	In.m_WantedWeapon = WEAPON_HAMMER + 1;
	In.m_TargetX = (int)(pT->GetPos().x - pA->GetPos().x);
	In.m_TargetY = (int)(pT->GetPos().y - pA->GetPos().y);
	In.m_Fire = (pA->Core()->m_Input.m_Fire + 1) | 1;
	const int ReloadBefore = pA->GetReloadTimer();
	const float DistBefore = AledZoneNearestDist(pZone, pTarget->GetPos());
	AledTickWorld(&World, ActorCid, &In);
	pA = World.GetCharacterById(ActorCid);
	pT = World.GetCharacterById(TargetCid);
	if(!pA || !pT)
		return false;
	if(pA->GetReloadTimer() <= ReloadBefore)
		return false; // no hit
	const float DistAfter = AledZoneNearestDist(pZone, pT->GetPos());
	return DistAfter < DistBefore - 8.0f;
}

// Engine-verified press: the hammer hit lands, the target ends the tick
// unfrozen off every freeze tile and is not re-frozen within 3 more ticks.
// A press that cannot hit yet (out of reach, blocked, reload not ready)
// returns false, so the bot never wastes the 320 ms hammer reload.
static bool AledPressWouldWorkIn(CGameClient *pGame, CGameWorld *pSrcWorld, int ActorCid, int TargetCid, const SAledZone *pZone)
{
	CCharacter *pActor = pSrcWorld->GetCharacterById(ActorCid);
	CCharacter *pTarget = pSrcWorld->GetCharacterById(TargetCid);
	if(!pActor || !pTarget)
		return false;
	if(pActor->GetReloadTimer() != 0)
		return false;

	CGameWorld World;
	World.CopyWorld(pSrcWorld);
	World.m_WorldConfig.m_PredictWeapons = true;
	World.m_WorldConfig.m_PredictDDRace = true;
	World.m_WorldConfig.m_PredictFreeze = 1;
	AledCopyTee(&World, pSrcWorld, ActorCid);
	AledCopyTee(&World, pSrcWorld, TargetCid);

	CCharacter *pA = World.GetCharacterById(ActorCid);
	CCharacter *pT = World.GetCharacterById(TargetCid);
	if(!pA || !pT)
		return false;

	CNetObj_PlayerInput In = pA->Core()->m_Input;
	In.m_Direction = 0;
	In.m_Jump = 0;
	In.m_Hook = 0;
	In.m_WantedWeapon = WEAPON_HAMMER + 1;
	In.m_TargetX = (int)(pT->GetPos().x - pA->GetPos().x);
	In.m_TargetY = (int)(pT->GetPos().y - pA->GetPos().y);
	In.m_Fire = (pA->Core()->m_Input.m_Fire + 1) | 1;

	// The hit resolves on the tick the input is applied, i.e. with the target
	// at its position BEFORE that tick: that is the tile which has to be inside
	// the rescue zone. Checking the post-move tile instead rejects valid hits
	// whenever the target crosses a tile boundary during the hit tick, which a
	// falling target does almost every tick.
	const int PreX = (int)floorf(pT->GetPos().x / 32.0f);
	const int PreY = (int)floorf(pT->GetPos().y / 32.0f);
	if(!AledZoneContains(pZone, PreX, PreY))
		return false;

	const int ReloadBefore = pA->GetReloadTimer();
	AledTickWorld(&World, ActorCid, &In);

	pA = World.GetCharacterById(ActorCid);
	pT = World.GetCharacterById(TargetCid);
	if(!pA || !pT)
		return false;
	// only a hammer hit sets the reload timer
	if(pA->GetReloadTimer() <= ReloadBefore)
		return false;
	if(pT->m_FreezeTime != 0 || pT->Core()->m_DeepFrozen)
		return false;
	if(AledIsFreezeTile(pGame, pT->GetPos()))
		return false;
	// a frozen actor cannot swing the hammer: such a press never reaches the
	// server and only burns the reload
	if(pA->m_FreezeTime > 0 || AledIsFreezeTile(pGame, pA->GetPos()))
		return false;
	for(int i = 0; i < 5; i++)
	{
		AledTickWorld(&World, ActorCid, nullptr);
		pT = World.GetCharacterById(TargetCid);
		if(!pT || pT->m_FreezeTime != 0)
			return false;
	}
	return true;
}

static bool AledPressWouldWork(CGameClient *pGame, int ActorCid, int TargetCid, const SAledZone *pZone)
{
	return AledPressWouldWorkIn(pGame, &pGame->m_PredictedWorld, ActorCid, TargetCid, pZone);
}

enum
{
	ALED_PLAN_TICKS = 25,
	ALED_BUFFER_MAX = 32,
	ALED_PRESS_GATE = 63,
};

// Deterministic input policies: one whole plan each. The search simulates
// every policy to the end and keeps the plan whose engine-verified press
// (target STRICTLY inside the BFS zone) happens earliest; jumps break ties.
enum
{
	ALED_POL_WAIT,
	ALED_POL_WAIT_LEFT,
	ALED_POL_WAIT_RIGHT,
	ALED_POL_HOOK,
	ALED_POL_HOOK_LEFT,
	ALED_POL_HOOK_RIGHT,
	ALED_POL_HOOK_JUMP,
	ALED_POL_HOOK_JUMP_LEFT,
	ALED_POL_HOOK_JUMP_RIGHT,
	ALED_POL_HOOK_JUMP6,
	ALED_POL_CREEP_LEFT,
	ALED_POL_CREEP_RIGHT,
	ALED_POL_HOOK_RAY_FIRST,
	ALED_POL_HOOK_RAY_LAST = ALED_POL_HOOK_RAY_FIRST + 23,
	ALED_POL_CEILING21 = ALED_POL_HOOK_RAY_LAST + 1,
	ALED_POL_COUNT,
};

struct SAledStep
{
	CNetObj_PlayerInput In;
	vec2 ActorPos;
	vec2 TargetPos;
	bool Press;
};

struct SAledPlan
{
	SAledStep aSteps[ALED_BUFFER_MAX];
	int Num;
	int Next;
	int TargetCid;
	int Policy;
	bool Valid;
};

static SAledPlan s_aAledPlan[MAX_DUMMIES];
// Policies whose plan already failed against the current target (the press
// did not unfreeze it in the predicted world). The next search must not pick
// them again, otherwise the bot keeps replaying the same failing plan while
// the world around the target has not changed at all.
static unsigned long long s_aAledSkipMask[MAX_DUMMIES];

// One plan per trigger. `Built` marks that this dummy already planned for the
// current trigger (the target entered the freeze, or the player switched the
// current dummy); `Zone` is the BFS zone filled exactly ONCE at that moment
// and never rebuilt afterwards.
struct SAledSession
{
	bool Built;
	bool ZoneValid;
	int Cur;
	int TargetCid;
	SAledZone Zone;
};
static SAledSession s_aAledSession[MAX_DUMMIES];

// True when a hook fired at `To` from `From` would grab solid ground or a
// nohook tile instead of the tee: any hit on the segment counts.
static bool AledHookRayBlocked(CGameClient *pGame, vec2 From, vec2 To)
{
	CCollision *pColl = pGame->Collision();
	if(!pColl)
		return false;
	vec2 HitPos;
	const int Hit = pColl->IntersectLineTeleHook(From, To, &HitPos, nullptr);
	return Hit != 0;
}

// A hook ray that ends on a tile the hook can actually attach to (solid or
// teleport). nohook / death / nothing at all are not worth firing: the hook
// would only retract and waste ticks.
static bool AledHookRayHookable(CGameClient *pGame, vec2 From, vec2 To)
{
	CCollision *pColl = pGame->Collision();
	if(!pColl)
		return false;
	vec2 HitPos;
	const int Hit = pColl->IntersectLineTeleHook(From, To, &HitPos, nullptr);
	// a hook only attaches to solid ground (or a hook-teleporter); nohook,
	// death and every other special tile are not grab points
	return Hit == TILE_SOLID || Hit == TILE_TELEINHOOK;
}

static void AledPolicyInput(CGameClient *pGame, CGameWorld *pWorld, int ActorCid, int TargetCid, int Policy, int Tick, CNetObj_PlayerInput *pOut)
{
	mem_zero(pOut, sizeof(*pOut));
	pOut->m_WantedWeapon = WEAPON_HAMMER + 1;
	CCharacter *pA = pWorld->GetCharacterById(ActorCid);
	CCharacter *pT = pWorld->GetCharacterById(TargetCid);
	if(!pA || !pT)
		return;
	const vec2 AP = pA->GetPos();
	const vec2 TP = pT->GetPos();
	pOut->m_TargetX = (int)(TP.x - AP.x);
	pOut->m_TargetY = (int)(TP.y - AP.y);

	const bool JumpNow =
		((Policy == ALED_POL_HOOK_JUMP || Policy == ALED_POL_HOOK_JUMP_LEFT ||
		  Policy == ALED_POL_HOOK_JUMP_RIGHT) && Tick == 1) ||
		(Policy == ALED_POL_HOOK_JUMP6 && Tick == 6) ||
		(Policy == ALED_POL_CEILING21 && Tick == 21);
	pOut->m_Jump = JumpNow ? 1 : 0;

	// 24 ray-validated hook fire angles (the same sweep the old input
	// enumeration used); the plan builder only keeps rays that end on a
	// hookable tile
	if(Policy >= ALED_POL_HOOK_RAY_FIRST && Policy <= ALED_POL_HOOK_RAY_LAST)
	{
		if(pA->Core()->m_HookState == HOOK_IDLE)
		{
			const int Ray = Policy - ALED_POL_HOOK_RAY_FIRST;
			const float Ang = (-5.0f / 6.0f) * pi + ((float)Ray / 23.0f) * (10.0f / 6.0f) * pi;
			pOut->m_Hook = 1;
			pOut->m_TargetX = (int)(cosf(Ang) * 256.0f);
			pOut->m_TargetY = (int)(sinf(Ang) * 256.0f);
			return;
		}
		pOut->m_Hook = 1;
		pOut->m_TargetX = (int)(pA->Core()->m_HookPos.x - AP.x);
		pOut->m_TargetY = (int)(pA->Core()->m_HookPos.y - AP.y);
		return;
	}

	// ceiling anchor: hang on the solid ceiling so the actor never steps on a
	// freeze floor, release, then hook the target and drag it up
	if(Policy == ALED_POL_CEILING21 && Tick <= 3)
	{
		pOut->m_Hook = 1;
		pOut->m_TargetX = 0;
		pOut->m_TargetY = -256;
		return;
	}
	if(Policy == ALED_POL_CEILING21 && Tick == 4)
		return;

	int Dir = 0;
	bool Creep = false;
	switch(Policy)
	{
	case ALED_POL_WAIT_LEFT: Dir = -1; break;
	case ALED_POL_WAIT_RIGHT: Dir = 1; break;
	case ALED_POL_HOOK_LEFT: Dir = -1; break;
	case ALED_POL_HOOK_RIGHT: Dir = 1; break;
	case ALED_POL_HOOK_JUMP_LEFT: Dir = -1; break;
	case ALED_POL_HOOK_JUMP_RIGHT: Dir = 1; break;
	case ALED_POL_CREEP_LEFT: Dir = -1; Creep = true; break;
	case ALED_POL_CREEP_RIGHT: Dir = 1; Creep = true; break;
	default: Dir = 0; break;
	}

	const bool WantHook = Policy != ALED_POL_WAIT && Policy != ALED_POL_WAIT_LEFT &&
		Policy != ALED_POL_WAIT_RIGHT;
	pOut->m_Direction = Dir;
	if(!WantHook)
		return;
	if(pA->Core()->m_HookState == HOOK_IDLE)
	{
		if(!AledHookRayBlocked(pGame, AP, TP))
		{
			pOut->m_Hook = 1;
			return;
		}
		if(Creep)
		{
			int Step = Dir != 0 ? Dir : 1;
			if((pA->Core()->m_Vel.x > 2.0f && Step > 0) || (pA->Core()->m_Vel.x < -2.0f && Step < 0))
				Step = -Step;
			pOut->m_Direction = Step;
			return;
		}
		return;
	}
	pOut->m_Hook = 1;
	pOut->m_TargetX = (int)(pA->Core()->m_HookPos.x - AP.x);
	pOut->m_TargetY = (int)(pA->Core()->m_HookPos.y - AP.y);
}

static bool AledBuildPlan(CGameClient *pGame, int ActorCid, int TargetCid, const SAledZone *pZone, unsigned long long SkipMask, SAledPlan *pOut)
{
	int BestKey = 1 << 30;
	SAledPlan Best;
	Best.Num = 0;
	Best.Policy = -1;
	Best.Valid = false;

	for(int Pol = 0; Pol < ALED_POL_COUNT; Pol++)
	{
		if(Pol < 64 && ((SkipMask >> Pol) & 1ull))
			continue; // this plan already failed against this target
		CGameWorld World;
		World.CopyWorld(&pGame->m_PredictedWorld);
		World.m_WorldConfig.m_PredictWeapons = true;
		World.m_WorldConfig.m_PredictDDRace = true;
		World.m_WorldConfig.m_PredictFreeze = 1;
		AledCopyTee(&World, &pGame->m_PredictedWorld, ActorCid);
		AledCopyTee(&World, &pGame->m_PredictedWorld, TargetCid);

		// Only hook rays that really end on a hookable tile are worth a plan:
		// firing at a nohook / death wall (or into empty space) only retracts
		// the hook and wastes ticks.
		{
			CCharacter *pA0 = World.GetCharacterById(ActorCid);
			if(!pA0)
				continue;
			const vec2 From = pA0->GetPos();
			const float HookLen = pA0->Core()->m_Tuning.m_HookLength;
			if(Pol == ALED_POL_CEILING21)
			{
				if(!AledHookRayHookable(pGame, From, From + vec2(0.0f, -HookLen)))
					continue;
			}
			else if(Pol >= ALED_POL_HOOK_RAY_FIRST && Pol <= ALED_POL_HOOK_RAY_LAST)
			{
				const int Ray = Pol - ALED_POL_HOOK_RAY_FIRST;
				const float Ang = (-5.0f / 6.0f) * pi + ((float)Ray / 23.0f) * (10.0f / 6.0f) * pi;
				const vec2 End = From + vec2(cosf(Ang), sinf(Ang)) * HookLen;
				if(!AledHookRayHookable(pGame, From, End))
					continue;
			}
		}

		SAledPlan Plan;
		Plan.Num = 0;
		Plan.Policy = Pol;
		Plan.Valid = false;
		int HitTick = -1;
		int Jumps = 0;
		for(int i = 0; i < ALED_PLAN_TICKS; i++)
		{
			CCharacter *pA = World.GetCharacterById(ActorCid);
			CCharacter *pT = World.GetCharacterById(TargetCid);
			if(!pA || !pT)
				break;
			const float Dist = distance(pA->GetPos(), pT->GetPos());
			// a verified press needs the target off freeze, inside the strict
			// hammer reach and (by construction of the zone) on a free tile
			// next to a freeze tile - the gate only skips hopeless ticks
			if(pT->m_FreezeTime > 0 && !AledIsFreezeTile(pGame, pT->GetPos()) && Dist <= (float)ALED_PRESS_GATE)
			{
				// the zone was filled once at the trigger and never changes
				if(AledPressWouldWorkIn(pGame, &World, ActorCid, TargetCid, pZone))
				{
					SAledStep S;
					mem_zero(&S, sizeof(S));
					S.In.m_TargetX = (int)(pT->GetPos().x - pA->GetPos().x);
					S.In.m_TargetY = (int)(pT->GetPos().y - pA->GetPos().y);
					S.In.m_WantedWeapon = WEAPON_HAMMER + 1;
					S.ActorPos = pA->GetPos();
					S.TargetPos = pT->GetPos();
					S.Press = true;
					if(Plan.Num < ALED_BUFFER_MAX)
						Plan.aSteps[Plan.Num++] = S;
					HitTick = i;
					break;
				}
			}
			SAledStep S;
			mem_zero(&S, sizeof(S));
			AledPolicyInput(pGame, &World, ActorCid, TargetCid, Pol, i, &S.In);
			S.ActorPos = pA->GetPos();
			S.TargetPos = pT->GetPos();
			S.Press = false;
			if(S.In.m_Jump)
				Jumps++;
			if(Plan.Num < ALED_BUFFER_MAX)
				Plan.aSteps[Plan.Num++] = S;
			AledTickWorld(&World, ActorCid, &S.In);
			pA = World.GetCharacterById(ActorCid);
			if(pA && (pA->m_FreezeTime > 0 || AledIsFreezeTile(pGame, pA->GetPos())))
				break;
		}
		if(HitTick < 0)
			continue;
		const int Key = (HitTick * 100 + Jumps) * 100 + Pol;
		if(Key < BestKey)
		{
			BestKey = Key;
			Best = Plan;
			Best.Valid = true;
		}
	}
	if(!Best.Valid)
		return false;
	*pOut = Best;
	return true;
}

// Plan-based decision: whole input plans, executed open-loop. A plan is a
// sequence of inputs produced by one deterministic policy; it wins when it
// reaches an engine-verified hammer hit with the target's centre STRICTLY
// inside the BFS zone, and the earliest hit wins. The plan is regenerated
// when it runs out or the world stops matching its simulated continuation.
static bool AledDecide(CGameClient *pGame, int ActorCid, int TargetCid, CNetObj_PlayerInput *pOut, bool *pPress, int TileSkip, int Dummy)
{
	(void)TileSkip;
	CCharacter *pActor = pGame->m_PredictedWorld.GetCharacterById(ActorCid);
	CCharacter *pTarget = pGame->m_PredictedWorld.GetCharacterById(TargetCid);
	if(!pActor || !pTarget)
		return false;
	*pPress = false;
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return false;
	SAledPlan &Buf = s_aAledPlan[Dummy];

	// a frozen actor cannot swing the hammer: nothing to decide
	if(pActor->m_FreezeTime > 0 || AledIsFreezeTile(pGame, pActor->GetPos()))
	{
		Buf.Valid = false;
		return false;
	}

	SAledSession &Ses = s_aAledSession[Dummy];
	const int Cur = g_Config.m_ClDummy;

	// One build per trigger: the target entering the freeze (the aledbot
	// engages it) or the player switching the current dummy. The BFS zone is
	// filled exactly once at that moment and never rebuilt afterwards, and a
	// plan that ran out or drifted is NOT regenerated for the same freeze.
	if(Ses.Cur != Cur || Ses.TargetCid != TargetCid)
	{
		Ses.Built = false;
		Buf.Valid = false;
	}
	if(!AledTargetEngaged(pGame, TargetCid, pTarget))
	{
		// the target is free again: the next freeze is a new trigger
		Ses.Built = false;
		Buf.Valid = false;
		return false;
	}

	// The trigger fills the BFS zone exactly once - at the moment the target
	// enters the freeze - and it is never rebuilt afterwards.
	if(!Ses.Built)
	{
		Ses.Built = true;
		Ses.Cur = Cur;
		Ses.TargetCid = TargetCid;
		Ses.ZoneValid = AledBuildZone(pGame, ActorCid, TargetCid, g_Config.m_KxAledTicks, &Ses.Zone);
		Buf.Valid = false;
	}
	if(!Ses.ZoneValid)
		return false;

	// Open-loop execution: a valid plan is applied exactly as built, without
	// live re-verification.
	if(Buf.Valid && Buf.TargetCid == TargetCid && Buf.Next < Buf.Num)
	{
		const SAledStep &S = Buf.aSteps[Buf.Next];
		if(distance(pActor->GetPos(), S.ActorPos) < 2.0f &&
			distance(pTarget->GetPos(), S.TargetPos) < 2.0f)
		{
			Buf.Next++;
			*pOut = S.In;
			if(S.Press)
			{
				*pPress = true;
				Buf.Valid = false;
			}
			return true;
		}
		// drifted more than 2 px: the plan is dead, build a new one
		Buf.Valid = false;
	}

	// A new plan is built when the old one ran out or drifted; it is always
	// built against the SAME zone that was filled at the trigger.
	SAledPlan Plan;
	bool Have = AledBuildPlan(pGame, ActorCid, TargetCid, &Ses.Zone, s_aAledSkipMask[Dummy], &Plan);
	if(!Have && s_aAledSkipMask[Dummy])
	{
		// every policy was already excluded by an earlier failure
		s_aAledSkipMask[Dummy] = 0;
		Have = AledBuildPlan(pGame, ActorCid, TargetCid, &Ses.Zone, 0, &Plan);
	}
	if(!Have)
	{
		Buf.Valid = false;
		return false;
	}
	Buf = Plan;
	Buf.TargetCid = TargetCid;
	Buf.Next = 0;
	const SAledStep &S0 = Buf.aSteps[0];
	Buf.Next = 1;
	*pOut = S0.In;
	if(S0.Press)
	{
		*pPress = true;
		Buf.Valid = false;
	}
	return true;
}

bool CBotNet::ProcessAledBot(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID)
{
	const int Cur = g_Config.m_ClDummy;
	const int NowTick = (int)Client()->GameTick(0);

	// Reactive per-dummy state (one decision per game tick, reused by every
	// frame of that tick).
	static int s_AledLastTick[MAX_DUMMIES] = {};
	static int s_AledFire[MAX_DUMMIES] = {};
	static bool s_AledFireInit[MAX_DUMMIES] = {};
	static bool s_AledFireChanged[MAX_DUMMIES] = {};
	static CNetObj_PlayerInput s_AledOut[MAX_DUMMIES];
	static bool s_AledPress[MAX_DUMMIES] = {};
	// v6.9: tile-retry state (see the decision block below).
	static bool s_AledRetryInit[MAX_DUMMIES] = {};
	static int s_AledRetryTarget[MAX_DUMMIES] = {};
	static int s_AledTileSkip[MAX_DUMMIES] = {};
	static int s_AledTileSwitches[MAX_DUMMIES] = {};
	static int s_AledAttemptStart[MAX_DUMMIES] = {};
	static int s_AledPressTick[MAX_DUMMIES] = {};
	static int s_AledPressPolicy[MAX_DUMMIES] = {};

	// The dummy input buffer can be overwritten by a dummy swap or by the
	// frozen handler; follow the last value the server knows so the press
	// stays a fire edge.
	if(!s_AledFireInit[Dummy] || (s_AledFire[Dummy] & 1) != (pInput->m_Fire & 1))
	{
		s_AledFire[Dummy] = pInput->m_Fire;
		s_AledFireInit[Dummy] = true;
	}

	// Nothing to do: stop the dummy and release the hammer. Committed to BOTH
	// input arrays on purpose — cl_show_direction (nameplates) reads
	// m_Controls.m_aInputData[Dummy] while m_aDummyInput is what OnSnapInput
	// sends, so only committing both actually clears the HUD arrows and stops
	// the tee. No forced direction here: that was what kept the arrows on.
	// Returns false when another botnet feature owns the dummy so it can run.
	auto Neutral = [&]() -> bool
	{
		if(g_Config.m_KxHelpers || g_Config.m_KxCopyMoves ||
			State.m_MacroPlaying || State.m_PathfinderGoActive)
			return false;
		AledDropInput(pInput);
		pGame->m_Controls.m_aInputData[Dummy] = *pInput;
		s_AledFire[Dummy] = pInput->m_Fire;
		s_AledFireChanged[Dummy] = false;
		return true;
	};

	if(!g_Config.m_KxAledEnabled)
		return Neutral();
	if(LocalID < 0 || LocalID >= MAX_CLIENTS)
		return false;
	if(g_Config.m_KxAledUseDummies == 0)
		return Neutral();

	// ---- frozen dummies -> sort -> target ------------------------------
	const int ActiveCid = pGame->m_aLocalIds[Cur];
	vec2 ActivePos(0.0f, 0.0f), AimPoint(0.0f, 0.0f);
	bool HaveRef = false;
	if(ActiveCid >= 0 && ActiveCid < MAX_CLIENTS && pGame->m_aClients[ActiveCid].m_Active)
	{
		ActivePos = pGame->m_aClients[ActiveCid].m_Predicted.m_Pos;
		HaveRef = true;
		const CNetObj_PlayerInput &In = pGame->m_Controls.m_aInputData[Cur];
		const vec2 Cam = pGame->m_Camera.m_Center;
		const float Zoom = pGame->m_Camera.m_Zoom;
		if(Zoom > 0.001f)
			AimPoint = Cam + (pGame->m_Controls.m_aTargetPos[Cur] - Cam) * Zoom;
		else
			AimPoint = ActivePos + vec2((float)In.m_TargetX, (float)In.m_TargetY);
	}

	int aCand[MAX_DUMMIES];
	long long aKey[MAX_DUMMIES];
	int NumCand = 0;
	for(int D = 0; D < MAX_DUMMIES; D++)
	{
		if(D == Dummy)
			continue;
		if(!(D == 0 || Client()->DummyConnected(D)))
			continue;
		const int Cid = pGame->m_aLocalIds[D];
		if(Cid < 0 || Cid >= MAX_CLIENTS)
			continue;
		if(!pGame->m_aClients[Cid].m_Active)
			continue;
		CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(Cid);
		// Trigger Ticks: a dummy about to fall into a freeze is already a
		// target, exactly as if it were frozen already.
		if(!AledTargetEngaged(pGame, Cid, pChar))
			continue;
		const bool IsActiveTarget = (D == Cur);
		if(g_Config.m_KxAledUseDummies == 1 && IsActiveTarget)
			continue; // Dummy Freezes: target must not be the active dummy
		if(g_Config.m_KxAledUseDummies == 2 && !IsActiveTarget)
			continue; // Active Dummy Freezes: target must be the active dummy
		if(!DummyPassesFilters(D, g_Config.m_KxAledTeam, g_Config.m_KxAledDdraceTeam,
			g_Config.m_KxAledDdraceTeamId, g_Config.m_KxAledFriend, g_Config.m_KxAledClanmate,
			g_Config.m_KxAledTargetsMode, g_Config.m_KxAledTargets, 0))
			continue;

		long long Key = (long long)D;
		if(g_Config.m_KxAledSort == 3)
			Key = -(long long)(m_aDummyActiveSeq[D] + 1);
		else if(HaveRef)
		{
			const vec2 Ref = (g_Config.m_KxAledSort == 2) ? AimPoint : ActivePos;
			const vec2 d = pChar->GetPos() - Ref;
			Key = (long long)(d.x * d.x + d.y * d.y);
		}
		aCand[NumCand] = D;
		aKey[NumCand] = Key;
		NumCand++;
	}
	if(NumCand == 0)
		return Neutral();
	AledSort(aCand, aKey, NumCand);

	const int TargetD = aCand[0];
	const int TargetCid = pGame->m_aLocalIds[TargetD];
	CCharacter *pTarget = pGame->m_PredictedWorld.GetCharacterById(TargetCid);
	CCharacter *pSelf = pGame->m_PredictedWorld.GetCharacterById(LocalID);
	if(!pTarget || !pSelf || pTarget == pSelf)
		return Neutral();

	// ---- Apply Mode / Apply Dummies ------------------------------------
	// Trigger: only the top-N acting dummies work. Always: every acting dummy
	// works. The target dummy never acts on itself.
	if(g_Config.m_KxAledApplyMode == 0)
	{
		int aAct[MAX_DUMMIES];
		long long aActKey[MAX_DUMMIES];
		int NumAct = 0;
		for(int D = 0; D < MAX_DUMMIES; D++)
		{
			if(D == TargetD)
				continue;
			if(!(D == 0 || Client()->DummyConnected(D)))
				continue;
			const int Cid = pGame->m_aLocalIds[D];
			if(Cid < 0 || Cid >= MAX_CLIENTS)
				continue;
			if(!pGame->m_aClients[Cid].m_Active)
				continue;
			CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(Cid);
			if(!pChar || pChar->m_FreezeTime > 0)
				continue; // frozen dummies are targets, not actors
			long long Key = (long long)D;
			if(g_Config.m_KxAledSort != 3 && HaveRef)
			{
				const vec2 Ref = (g_Config.m_KxAledSort == 2) ? AimPoint : ActivePos;
				const vec2 d = pChar->GetPos() - Ref;
				Key = (long long)(d.x * d.x + d.y * d.y);
			}
			aAct[NumAct] = D;
			aActKey[NumAct] = Key;
			NumAct++;
		}
		AledSort(aAct, aActKey, NumAct);
		bool Allowed = false;
		const int MaxAct = g_Config.m_KxAledApplyDummies;
		for(int i = 0; i < NumAct && i < MaxAct; i++)
		{
			if(aAct[i] == Dummy)
			{
				Allowed = true;
				break;
			}
		}
		// The acting dummy must still be usable when, besides the target and
		// itself, there is nobody else left to act.
		if(!Allowed && NumAct <= 1)
			Allowed = true;
		if(!Allowed)
			return Neutral();
	}

		// ---- one decision per game tick ------------------------------------
		if(s_AledLastTick[Dummy] != NowTick)
		{
			s_AledLastTick[Dummy] = NowTick;

			// Retry bookkeeping: when a verified press did not actually unfreeze
			// the target (server rejected it / ping skew), move to the NEXT tile
			// along the target's trajectory. At most 2 tile switches per target,
			// so the search never ping-pongs between tiles.
			if(!s_AledRetryInit[Dummy])
			{
				s_AledRetryInit[Dummy] = true;
				s_AledRetryTarget[Dummy] = -1;
			}
				const bool TargetFrozen = AledTargetEngaged(pGame, TargetCid, pTarget);
				// a press that left the target frozen in the predicted world was
				// a local failure: remember that plan's policy and never replay it
				if(s_AledPress[Dummy] && TargetFrozen)
					s_aAledSkipMask[Dummy] |= (1ull << (s_AledPressPolicy[Dummy] & 63));
					if(s_AledRetryTarget[Dummy] != TargetCid || !TargetFrozen)
					{
						s_AledRetryTarget[Dummy] = TargetCid;
						s_AledTileSkip[Dummy] = 0;
						s_AledTileSwitches[Dummy] = 0;
						s_AledAttemptStart[Dummy] = NowTick;
						s_AledPressTick[Dummy] = 0;
						// a new target invalidates the open-loop plan buffer and
						// clears the failed-policy mask
						s_aAledPlan[Dummy].Valid = false;
						s_aAledSkipMask[Dummy] = 0;
					}
			else
			{
				bool Failed = false;
				// a press was sent, but the target is still frozen well after it
				if(s_AledPressTick[Dummy] && NowTick - s_AledPressTick[Dummy] > 12)
					Failed = true;
				// nothing could be pressed for a long time (unreachable tile)
				else if(s_AledAttemptStart[Dummy] && NowTick - s_AledAttemptStart[Dummy] > 200)
					Failed = true;
					if(Failed)
					{
						// the pressed plan did not work: never replay it
						if(s_AledPressTick[Dummy])
							s_aAledSkipMask[Dummy] |= (1ull << (s_AledPressPolicy[Dummy] & 63));
						if(s_AledTileSwitches[Dummy] < 2)
					{
						s_AledTileSwitches[Dummy]++;
						s_AledTileSkip[Dummy] = s_AledTileSwitches[Dummy];
					}
					s_AledAttemptStart[Dummy] = NowTick;
					s_AledPressTick[Dummy] = 0;
				}
			}

			const int PrevFire = s_AledFire[Dummy];
			CNetObj_PlayerInput Decided;
			mem_zero(&Decided, sizeof(Decided));
			bool Press = false;
				if(!AledDecide(pGame, LocalID, TargetCid, &Decided, &Press, s_AledTileSkip[Dummy], Dummy))
					return Neutral();
			if(Press)
				s_AledPressTick[Dummy] = NowTick;

			// m_NextWeapon / m_PrevWeapon are 6-bit counters: never rewrite them
			// (a discontinuity becomes a burst of CountInput presses on the server
			// and cycles the dummy's weapon, usually to the pistol).
			Decided.m_NextWeapon = pInput->m_NextWeapon;
			Decided.m_PrevWeapon = pInput->m_PrevWeapon;
			Decided.m_Fire = Press ? ((PrevFire + 1) | 1) : ((PrevFire + 1) & ~1);

				s_AledOut[Dummy] = Decided;
				s_AledPress[Dummy] = Press;
				if(Press)
					s_AledPressPolicy[Dummy] = s_aAledPlan[Dummy].Policy;
				s_AledFire[Dummy] = Decided.m_Fire;
			s_AledFireChanged[Dummy] = ((Decided.m_Fire ^ PrevFire) & 1) != 0;
		}

	*pInput = s_AledOut[Dummy];

	// The dummy pipeline drops a packet whose direction/jump/hook are all
	// zero. Force a sendable input on the tick that carries the fire edge -
	// but only in the SENT packet: cl_show_direction renders the HUD arrows
	// from m_aInputData, and a fake direction left there never gets cleared
	// once the dummy freezes (the frozen handler bails out before AledBot),
	// so it showed a ghost key until the unfreeze.
	bool ForcedDir = false;
	if(s_AledFireChanged[Dummy] && !pInput->m_Direction && !pInput->m_Jump && !pInput->m_Hook)
	{
		pInput->m_Direction = (pTarget->GetPos().x >= pSelf->GetPos().x) ? 1 : -1;
		ForcedDir = true;
	}

	if(ForcedDir)
	{
		CNetObj_PlayerInput Hud = *pInput;
		Hud.m_Direction = 0;
		pGame->m_Controls.m_aInputData[Dummy] = Hud;
	}
	else
		pGame->m_Controls.m_aInputData[Dummy] = *pInput;
	return true;
}