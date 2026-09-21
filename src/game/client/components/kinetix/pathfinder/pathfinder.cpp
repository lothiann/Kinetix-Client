#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <chrono>
#include <thread>

// =========================================================
// HELPERS PATHFINDER — the whole movement stack, one module
// =========================================================
// Layers, top to bottom (all in this file on purpose):
//
//   1. ComputePathfinder / ComputePathfinderRescue
//      Grid guidance field (m_pfDist): both targeting and rescue use
//      the Pathfinder tab's FMM fill (PfBfsFill); rescue runs it in
//      two phases — freeze impassable first, freeze passable only
//      when nothing else reaches the bot.
//   2. ComputeFlowForTile / RescueFlowForTile / GetMovementFromFlow
//      Per-tile direction extraction from the field (rays + LOS for
//      targeting, field gradient for rescue) and the quantised
//      left/right/jump output used as fallback.
//   3. ComputeHelpersPath / ApplyHelpersPlan
//      The physics plan: greedy 12-tick windows of real CCharacter
//      simulation (other players included — their hooks and hammers
//      are real) scored by progress along the field.  Any freeze
//      contact kills a window.  This is what actually walks the bot.
//   4. RenderPackage / RenderVectorField
//      Debug rendering of the plan and the field.
// =========================================================

void CBotNet::ComputePathfinder(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
	if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
		return;

	targetTX = pf_clamp(targetTX, 0, m_MapWidth - 1);
	targetTY = pf_clamp(targetTY, 0, m_MapHeight - 1);
	botTX = pf_clamp(botTX, 0, m_MapWidth - 1);
	botTY = pf_clamp(botTY, 0, m_MapHeight - 1);

	if(!IsTileWalkable(targetTX, targetTY) || !IsTileWalkable(botTX, botTY))
	{
		State.m_PathFound = false;
		State.m_RescueField = false;
		State.m_FlowThroughFreeze = false;
		State.m_FlowDir = vec2(0, 0);
		State.m_PfHookTile = vec2(0, 0);
		return;
	}

	// Single source = the target tile; freeze tiles are WALLS — the
	// physics plan must never touch freeze.  Solver = the tab's FMM
	// fill (PfBfsFill), the same one rescue uses.
	auto fnTileWalk = [this](int x, int y) -> bool {
		return x >= 0 && y >= 0 && x < m_MapWidth && y < m_MapHeight && IsTileWalkable(x, y);
	};
	auto fnTileFreeze = [this](int x, int y) -> bool {
		return IsTileFreeze(x, y);
	};
	std::vector<std::pair<int, int>> sources{{targetTX, targetTY}};
	PfBfsFill(m_pfDist, m_MapWidth, m_MapHeight, sources, fnTileWalk, fnTileFreeze, false);
	bool reached = m_pfDist[botTY * m_MapWidth + botTX] < 1e17f;

	State.m_PathFound = reached;
	State.m_RescueField = false; // targeting field — not the rescue FMM
	State.m_FlowThroughFreeze = false; // targeting never crosses freeze
	State.m_LastTargetTX = targetTX;
	State.m_LastTargetTY = targetTY;
	State.m_LastBotTX = botTX;
	State.m_LastBotTY = botTY;

	if(reached)
		ComputeFlowForTile(botTY, botTX, State);
	else
	{
		State.m_FlowDir = vec2(0, 0);
		State.m_PfHookTile = vec2(0, 0);
	}
}

// =========================================================
// PATHFINDER RESCUE — safe-tile FMM field around a frozen ally
// =========================================================
// Ray-fan around the (frozen) target collects walkable, non-freeze
// tiles within hook reach — candidate "safe spots" the bot can pull
// the target to.  All of them seed the SAME field solver the
// Pathfinder tab uses (PfBfsFill, multi-source FMM/Eikonal), in the
// tab's two phases: Phase 1 freeze impassable; only if Phase 1 did
// not reach the bot, Phase 2 refills with freeze passable — a risky
// rescue beats no rescue.  Helpers does NOT read KxPfFreezeSupport:
// Phase 2 always runs when it is needed.
// The flow is the tab-style gradient — descend to the neighbor with
// the lowest field value (RescueFlowForTile).
// =========================================================

void CBotNet::ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
	State.m_PathFound = false;
	State.m_FlowDir = vec2(0, 0);
	State.m_RescueField = false;
	State.m_FlowThroughFreeze = false;
	if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
		return;

	targetTX = pf_clamp(targetTX, 0, m_MapWidth - 1);
	targetTY = pf_clamp(targetTY, 0, m_MapHeight - 1);
	botTX = pf_clamp(botTX, 0, m_MapWidth - 1);
	botTY = pf_clamp(botTY, 0, m_MapHeight - 1);

	if(!IsTileWalkable(botTX, botTY))
		return;

	const int rays = 24;
	int hookRadiusTiles = pf_max(1, (int)(g_Config.m_KxHookDist / 32.0f));

	// All safe spots around the ally seed the field together
	// (multi-source, same as the tab seeds its finish tiles).
	std::vector<std::pair<int, int>> vSources;
	for(int i = 0; i < rays; i++)
	{
		float angle = 2.0f * pi * i / rays;
		float rayDX = cosf(angle);
		float rayDY = sinf(angle);

		for(int step = 1; step <= hookRadiusTiles; step++)
		{
			int tc = targetTX + (int)roundf(rayDX * step);
			int tr = targetTY + (int)roundf(rayDY * step);

			if(tc < 0 || tr < 0 || tc >= m_MapWidth || tr >= m_MapHeight)
				break;
			if(!IsTileWalkable(tc, tr))
				break;
			if(IsTileFreeze(tc, tr))
				continue;
			vSources.emplace_back(tc, tr);
		}
	}

	if(vSources.empty())
		return;

	auto fnTileWalk = [this](int x, int y) -> bool {
		return x >= 0 && y >= 0 && x < m_MapWidth && y < m_MapHeight && IsTileWalkable(x, y);
	};
	auto fnTileFreeze = [this](int x, int y) -> bool {
		return IsTileFreeze(x, y);
	};

	// Phase 1: freeze impassable.  Phase 2 (only when Phase 1 did not
	// reach the bot): freeze passable — the tab's two-phase fill.
	// The flag tells the movement layer the flow legitimately crosses
	// freeze, so the survival veto stands down for this field.
	PfBfsFill(m_pfDist, m_MapWidth, m_MapHeight, vSources, fnTileWalk, fnTileFreeze, false);
	bool throughFreeze = m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f;
	if(throughFreeze)
		PfBfsFill(m_pfDist, m_MapWidth, m_MapHeight, vSources, fnTileWalk, fnTileFreeze, true);
	if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
		return; // no route at all — neither around nor through the pool

	State.m_PathFound = true;
	State.m_RescueField = true;
	State.m_FlowThroughFreeze = throughFreeze;
	State.m_LastTargetTX = targetTX;
	State.m_LastTargetTY = targetTY;
	State.m_LastBotTX = botTX;
	State.m_LastBotTY = botTY;
	RescueFlowForTile(botTY, botTX, State);
}

// =========================================================
// RESCUE FLOW — tab-style gradient: from the tile, step to the
// 8-neighbor with the lowest field value (the same descent
// RenderVectorField draws as arrows).
// =========================================================
void CBotNet::RescueFlowForTile(int r, int c, CBotNetDummy &State)
{
	State.m_PfHookTile = vec2(0, 0);

	if(!m_MapGridLoaded || r < 0 || c < 0 || r >= m_MapHeight || c >= m_MapWidth)
	{
		State.m_FlowDir = vec2(0, 0);
		return;
	}

	int idx = r * m_MapWidth + c;
	if(m_pfDist[idx] >= 1e17f)
	{
		State.m_FlowDir = vec2(0, 0);
		return;
	}

	float bestDist = m_pfDist[idx];
	float bestDX = 0, bestDY = 0;
	int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
	int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

	for(int d = 0; d < 8; d++)
	{
		int nr = r + dr[d];
		int nc = c + dc[d];
		if(nr < 0 || nc < 0 || nr >= m_MapHeight || nc >= m_MapWidth)
			continue;
		int nIdx = nr * m_MapWidth + nc;
		if(m_pfDist[nIdx] < bestDist)
		{
			bestDist = m_pfDist[nIdx];
			bestDX = (float)(nc - c);
			bestDY = (float)(nr - r);
		}
	}

	float len = sqrtf(bestDX * bestDX + bestDY * bestDY);
	if(len > 0.001f)
		State.m_FlowDir = vec2(bestDX / len, bestDY / len);
	else
		State.m_FlowDir = vec2(0, 0);
}

// =========================================================
// FLOW EXTRACTION — best direction for one tile from m_pfDist
// =========================================================

void CBotNet::ComputeFlowForTile(int r, int c, CBotNetDummy &State)
{
	State.m_PfHookTile = vec2(0, 0);

	if(!m_MapGridLoaded)
		return;
	if(r < 0 || c < 0 || r >= m_MapHeight || c >= m_MapWidth)
		return;

	int idx = r * m_MapWidth + c;
	if(m_pfDist[idx] >= 1e17f)
	{
		State.m_FlowDir = vec2(0, 0);
		return;
	}

	float currentD = m_pfDist[idx];
	float bestDist = currentD;
	float bestDX = 0, bestDY = 0;
	const int rays = 24;

	// FMM rays — walkable tiles with lower distance that are directly
	// reachable (line of sight).  A freeze tile stops the ray: the flow
	// must never point into or across freeze (the physics plan refuses
	// freeze, and the grid fallback would walk the bot straight in).
	for(int i = 0; i < rays; i++)
	{
		float angle = 2.0f * pi * i / rays;
		float rayDX = cosf(angle);
		float rayDY = sinf(angle);

		for(int step = 1; step <= 8; step++)
		{
			int tr = r + (int)roundf(rayDY * step);
			int tc = c + (int)roundf(rayDX * step);
			if(tr < 0 || tc < 0 || tr >= m_MapHeight || tc >= m_MapWidth)
				break;
			if(!IsTileWalkable(tc, tr))
				break;
			if(IsTileFreeze(tc, tr))
				break;

			int fIdx = tr * m_MapWidth + tc;
			float adjustedDist = m_pfDist[fIdx];

			if(adjustedDist < currentD && HasLineOfSightTiles(r, c, tr, tc))
			{
				if(adjustedDist < bestDist)
				{
					bestDist = adjustedDist;
					float dx = (tc + 0.5f) - (c + 0.5f);
					float dy = (tr + 0.5f) - (r + 0.5f);
					float len = sqrtf(dx * dx + dy * dy);
					if(len > 0.001f)
					{
						bestDX = dx / len;
						bestDY = dy / len;
					}
				}
			}
		}
	}

	float len = sqrtf(bestDX * bestDX + bestDY * bestDY);
	if(len > 0.001f)
		State.m_FlowDir = vec2(bestDX / len, bestDY / len);
	else
		State.m_FlowDir = vec2(0, 0);
}

void CBotNet::GetMovementFromFlow(const CBotNetDummy &State, bool &outLeft, bool &outRight, bool &outJump)
{
	outLeft = false;
	outRight = false;
	outJump = false;

	if(State.m_FlowDir.x == 0.0f && State.m_FlowDir.y == 0.0f)
		return;

	float angle = atan2f(State.m_FlowDir.y, State.m_FlowDir.x);
	float snap = roundf(angle / (pi / 4.0f)) * (pi / 4.0f);

	float snapX = cosf(snap);
	float snapY = sinf(snap);

	if(snapX > 0.5f)
		outRight = true;
	else if(snapX < -0.5f)
		outLeft = true;

	if(snapY < -0.5f)
		outJump = true;
}

// =========================================================
// BLOCK PHYSICS PLAN — greedy on-the-fly builder
// =========================================================
//
// No tree, no A*, no backtracking, no background thread.
// The plan is built ON THE FLY, one window at a time:
//
//   1. From the current simulated state build the tab's candidate
//      inputs (walk dir x jump aim-up, hook steering while attached,
//      raycast-validated hook fan while idle).
//   2. Simulate every candidate for a short window (12 ticks) with
//      the real CCharacter physics — DDNet freeze/death logic plus
//      the tab's PfCheckFreeze anti-skip.  Death or ANY freeze
//      contact = candidate fails.
//   3. Take the window with the best progress along the freeze-
//      walled m_pfDist field, append it to the plan, advance the
//      simulated state, repeat until the goal / stall / length cap.
//
// When no candidate makes progress the plan simply ends there: the
// replay runs it, then the grid-flow fallback moves the bot and a
// fresh plan is built from the new position.
// =========================================================

// Snapshot of one simulated state
struct KHelpersState
{
	vec2 Pos, Vel, HookPos, HookDir;
	int HookState, HookTick, Jumped, JumpedTotal;
};

// Snapshot of another player kept in the planning world (see ComputeHelpersPath)
struct KHelpersOtherState
{
	int Id;
	CCharacterCore Core;
	int FreezeTime;
};

static void KHelpersReadState(CCharacter *pBot, KHelpersState &st)
{
	CCharacterCore c = pBot->GetCore();
	st.Pos = c.m_Pos;
	st.Vel = c.m_Vel;
	st.HookState = c.m_HookState;
	st.HookPos = c.m_HookPos;
	st.HookDir = c.m_HookDir;
	st.HookTick = c.m_HookTick;
	st.Jumped = c.m_Jumped;
	st.JumpedTotal = c.m_JumpedTotal;
}

// Mirror PfSpawnSimBot: m_FreezeStart is reset so the sim's Freeze()
// is not suppressed by the real core's anti-flicker.
static void KHelpersWriteState(CCharacter *pBot, const KHelpersState &st, int FreezeTime)
{
	CCharacterCore c = pBot->GetCore();
	c.m_Pos = st.Pos;
	c.m_Vel = st.Vel;
	c.m_HookState = st.HookState;
	c.m_HookPos = st.HookPos;
	c.m_HookDir = st.HookDir;
	c.m_HookTick = st.HookTick;
	c.m_Jumped = st.Jumped;
	c.m_JumpedTotal = st.JumpedTotal;
	c.m_FreezeStart = (FreezeTime > 0) ? 0x7FFFFFFF : 0;
	c.m_FreezeEnd = 0;
	pBot->SetCore(c);
	pBot->m_Pos = st.Pos;
	pBot->m_FreezeTime = FreezeTime;
}

// The tab's PfBuildTickCandidates — verbatim: walk dir x jump with aim up,
// hook steering while attached (aim at the hook), raycast-validated hook
// fan while idle (fan -5/6 pi .. +5/6 pi, aim = 256 * (cos, sin), jump 0).
// A hook angle whose line passes through another tee is rejected: the hook
// would grab the tee instead of the wall the plan expects.
static int KHelpersBuildCandidates(const KHelpersState &cur, CGameWorld *pWorld, CCharacter *pBot, CCollision *pColl,
	float hookLength, int HookAngles, CNetObj_PlayerInput aOut[128])
{
	int n = 0;
	static const int aDirs[] = {-1, 0, 1};
	auto add = [&](int dir, int jump, int hook, int tx, int ty) {
		if(n >= 128)
			return;
		CNetObj_PlayerInput &in = aOut[n];
		mem_zero(&in, sizeof(in));
		in.m_Direction = dir;
		in.m_Jump = jump;
		in.m_Hook = hook;
		in.m_TargetX = tx;
		in.m_TargetY = ty;
		n++;
	};
	for(int d = 0; d < 3; d++)
	{
		for(int j = 0; j < 2; j++)
			add(aDirs[d], j, 0, aDirs[d] * 256, -256);
	}
	const bool hookIdle = (cur.HookState == HOOK_IDLE);
	const bool hookActive = (cur.HookState == HOOK_FLYING || cur.HookState == HOOK_GRABBED);
	if(hookActive)
	{
		int tx = (int)(cur.HookPos.x - cur.Pos.x);
		int ty = (int)(cur.HookPos.y - cur.Pos.y);
		for(int d = 0; d < 3; d++)
		{
			for(int j = 0; j < 2; j++)
				add(aDirs[d], j, 1, tx, ty);
		}
	}
	if(hookIdle)
	{
		for(int a = 0; a < HookAngles; a++)
		{
			float t = (float)a / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
			float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			vec2 rayEnd = cur.Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
			vec2 hitPos;
			int hit = pColl->IntersectLineTeleHook(cur.Pos, rayEnd, &hitPos, nullptr);
			if(!(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH)))
				continue;
			// The hook stops on the first tee in the way — reject the angle
			// when another player sits between the bot and the hook point.
			vec2 CharHitPos;
			if(pWorld->IntersectCharacter(cur.Pos, hitPos, CCharacterCore::PhysicalSize(), CharHitPos, pBot))
				continue;
			int tx = (int)(cosf(angle) * 256.0f);
			int ty = (int)(sinf(angle) * 256.0f);
			for(int d = 0; d < 3; d++)
				add(aDirs[d], 0, 2, tx, ty);
		}
	}
	return n;
}

// =========================================================
// Main-thread greedy planner: no tree, no rollback
// =========================================================
void CBotNet::ComputeHelpersPath(int Dummy, const vec2 &MyPos, int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
	// The freeze-walled grid field must be ready
	if(!m_MapGridLoaded || !m_pfDist || m_MapWidth <= 0 || m_MapHeight <= 0)
		return;
	if(botTX < 0 || botTY < 0 || botTX >= m_MapWidth || botTY >= m_MapHeight)
		return;
	if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
		return; // no safe route from this tile at all

	CGameClient *pGame = GameClient();
	int PredictedCharId = (Dummy == g_Config.m_ClDummy) ? pGame->m_Snap.m_LocalClientId : Dummy;
	if(!pGame->m_PredictedWorld.GetCharacterById(PredictedCharId))
		return;

	// Planning world: non-character entities are stripped, but other players
	// STAY — their hooks can grab the bot mid-plan, their hammer pushes are
	// real, and hook grabs on them are real.  Crucial for rescue, where the
	// bot works right next to freeze and can be yanked into it any moment.
	CGameWorld FutureWorld;
	FutureWorld.CopyWorld(&pGame->m_PredictedWorld);
	for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
	{
		if(Type == CGameWorld::ENTTYPE_CHARACTER)
			continue;
		std::vector<CEntity *> vRemove;
		for(CEntity *pEnt = FutureWorld.FindLast(Type); pEnt; pEnt = pEnt->TypePrev())
			vRemove.push_back(pEnt);
		for(CEntity *pEnt : vRemove)
			FutureWorld.RemoveEntity(pEnt);
	}

	CCharacter *pBot = FutureWorld.GetCharacterById(PredictedCharId);
	if(!pBot)
		return;
	if(pBot->m_FreezeTime > 0)
	{
		State.m_HelpersPlan.Reset(); // frozen right now — nothing to plan
		return;
	}

	CCollision *pColl = FutureWorld.Collision();
	if(!pColl)
		return;

	int StartTick = FutureWorld.GameTick();

	// Planner parameters
	const int WindowTicks = 12;   // lookahead per step
	const int MaxPlanTicks = 512; // hard length cap
	const int HookAngles = 24;
	float hookLength = 380.0f;
	{
		float hl = pBot->GetCore().m_Tuning.m_HookLength;
		if(hl > 1.0f && hl < 10000.0f)
			hookLength = hl;
	}

	// distance along the freeze-walled field
	auto fieldDist = [&](const vec2 &pos) -> float {
		int tx = pf_clamp((int)(pos.x / 32.0f), 0, m_MapWidth - 1);
		int ty = pf_clamp((int)(pos.y / 32.0f), 0, m_MapHeight - 1);
		return m_pfDist[ty * m_MapWidth + tx];
	};

	KHelpersState cur;
	KHelpersReadState(pBot, cur);
	int curFreeze = pBot->m_FreezeTime; // 0 here (frozen rejected above)

	CNetObj_PlayerInput aPlan[HELPERS_MAX_TICKS];
	int count = 0;
	std::vector<vec2> vPath;
	vPath.reserve(MaxPlanTicks + 1);
	vPath.push_back(cur.Pos);

	float lastDist = fieldDist(cur.Pos);
	bool reachedGoal = (lastDist <= 0.5f);

	// === Greedy on-the-fly build: window by window, no rollback ===
	while(!reachedGoal && count + WindowTicks <= MaxPlanTicks)
	{
		CNetObj_PlayerInput aCands[128];
		int nCands = KHelpersBuildCandidates(cur, &FutureWorld, pBot, pColl, hookLength, HookAngles, aCands);

		// Snapshot the other players at the window's start: every candidate
		// must simulate from the identical world state so the comparison is
		// fair and their physics stays deterministic.
		KHelpersOtherState aOthers[MAX_CLIENTS];
		int numOthers = 0;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == PredictedCharId)
				continue;
			CCharacter *pOther = FutureWorld.GetCharacterById(i);
			if(!pOther)
				continue;
			aOthers[numOthers].Id = i;
			aOthers[numOthers].Core = pOther->GetCore();
			aOthers[numOthers].FreezeTime = pOther->m_FreezeTime;
			numOthers++;
		}
		auto fnRestoreOthers = [&]() {
			for(int oi = 0; oi < numOthers; oi++)
			{
				CCharacter *pOther = FutureWorld.GetCharacterById(aOthers[oi].Id);
				if(!pOther)
					continue;
				pOther->SetCore(aOthers[oi].Core);
				pOther->m_Pos = aOthers[oi].Core.m_Pos;
				pOther->m_FreezeTime = aOthers[oi].FreezeTime;
			}
		};

		float bestScore = -1e9f;
		bool haveBest = false;
		int bestFreezeTicks = -1;
		CNetObj_PlayerInput aBestWin[WindowTicks];
		vec2 aBestWinPos[WindowTicks];
		KHelpersState bestEnd = cur;

		for(int ci = 0; ci < nCands; ci++)
		{
			KHelpersWriteState(pBot, cur, curFreeze);
			fnRestoreOthers();

			CNetObj_PlayerInput aWin[WindowTicks];
			vec2 aWinPos[WindowTicks];
			bool failed = false;
			int adjTicks = 0;
			int freezeTicks = 0;
			vec2 prevPos = cur.Pos;

			for(int t = 0; t < WindowTicks; t++)
			{
				CNetObj_PlayerInput in = aCands[ci];
				if(in.m_Jump && t > 0)
					in.m_Jump = 0; // single jump impulse per window

				int Tick = StartTick + count + t + 1;
				pBot->OnDirectInput(&in);
				FutureWorld.m_GameTick = Tick;
				pBot->OnPredictedInput(&in);
				FutureWorld.Tick();
				if(!FutureWorld.GetCharacterById(PredictedCharId))
				{
					failed = true; // died
					break;
				}

				// Tab-exact anti-skip freeze check (PfCheckFreeze = 1:1 copy
				// of DDNet HandleTiles) after the engine tick; ANY freeze
				// contact (engine or check) fails the candidate.
				if(pBot->m_FreezeTime <= 0)
					PfCheckFreeze(&FutureWorld, pBot, prevPos, pBot->m_Pos);
				if(pBot->m_FreezeTime > 0)
				{
					// Freeze contact is no longer an instant discard: a HUGE
					// penalty (see score) ranks it below every clean candidate,
					// and it can never be committed to the plan (guard below).
					freezeTicks++;
					break;
				}

				aWin[t] = in;
				aWinPos[t] = pBot->m_Pos;
				prevPos = pBot->m_Pos;

				// soft margin — discourage hugging freeze
				int ptx = pf_clamp((int)(pBot->m_Pos.x / 32.0f), 0, m_MapWidth - 1);
				int pty = pf_clamp((int)(pBot->m_Pos.y / 32.0f), 0, m_MapHeight - 1);
				bool adj = false;
				for(int oy = -1; oy <= 1 && !adj; oy++)
					for(int ox = -1; ox <= 1 && !adj; ox++)
						if(IsTileFreeze(pf_clamp(ptx + ox, 0, m_MapWidth - 1), pf_clamp(pty + oy, 0, m_MapHeight - 1)))
							adj = true;
				if(adj)
					adjTicks++;
			}
			if(failed)
				continue;

			float endDist = fieldDist(pBot->m_Pos);
			if(endDist >= 1e17f)
				continue; // wandered off the safe route

			// HUGE freeze penalty — 10000 per contact tick dwarfs any distance
			// progress, so a clean candidate always beats a freezing one.
			float score = (lastDist - endDist) - (float)adjTicks * 0.05f -
				(float)freezeTicks * 10000.0f;
			if(score > bestScore)
			{
				bestScore = score;
				bestFreezeTicks = freezeTicks;
				KHelpersReadState(pBot, bestEnd);
				for(int t = 0; t < WindowTicks; t++)
				{
					aBestWin[t] = aWin[t];
					aBestWinPos[t] = aWinPos[t];
				}
				haveBest = true;
			}
		}

		// No CLEAN candidate made progress — the plan ends here while it is
		// still freeze-free.  A freeze-tainted candidate is never committed:
		// its huge penalty keeps it below the progress threshold.  The replay
		// runs the clean prefix and the fallback takes over from a safe spot.
		if(!haveBest || bestScore <= 0.01f || bestFreezeTicks > 0)
			break;

		// Land the whole world on the chosen candidate's end state: re-run the
		// winning window once (other players included) so the next window is
		// simulated from exactly the state the winner was evaluated in.
		KHelpersWriteState(pBot, cur, curFreeze);
		fnRestoreOthers();
		for(int t = 0; t < WindowTicks; t++)
		{
			CNetObj_PlayerInput in = aBestWin[t];
			if(in.m_Jump && t > 0)
				in.m_Jump = 0;
			int Tick = StartTick + count + t + 1;
			pBot->OnDirectInput(&in);
			FutureWorld.m_GameTick = Tick;
			pBot->OnPredictedInput(&in);
			FutureWorld.Tick();
		}

		for(int t = 0; t < WindowTicks; t++)
		{
			aPlan[count + t] = aBestWin[t];
			vPath.push_back(aBestWinPos[t]);
		}
		count += WindowTicks;
		cur = bestEnd;

		lastDist = fieldDist(cur.Pos);
		int ctx = pf_clamp((int)(cur.Pos.x / 32.0f), 0, m_MapWidth - 1);
		int cty = pf_clamp((int)(cur.Pos.y / 32.0f), 0, m_MapHeight - 1);
		if(lastDist <= 0.5f || (ctx == targetTX && cty == targetTY))
			reachedGoal = true;
	}

	if(count == 0)
	{
		State.m_HelpersPlan.Reset(); // nothing to do — grid-flow fallback
		return;
	}

	// === Store the plan ===
	CHelpersPlan &cache = State.m_HelpersPlan;
	cache.Reset();
	memcpy(cache.aInputs, aPlan, sizeof(CNetObj_PlayerInput) * count);
	cache.NumTicks = count;
	cache.CurrentTick = 0;
	cache.ComputeTick = Client()->GameTick(g_Config.m_ClDummy);
	cache.TargetTX = targetTX;
	cache.TargetTY = targetTY;
	cache.BotPos = MyPos;
	cache.vPath = vPath;
}

void CBotNet::ApplyHelpersPlan(int Dummy, CNetObj_PlayerInput *pInput, CBotNetDummy &State)
{
	CHelpersPlan &cache = State.m_HelpersPlan;
	if(!cache.IsValid())
		return;

	CGameClient *pGame = GameClient();

	if(cache.CurrentTick < cache.NumTicks)
	{
		CNetObj_PlayerInput &src = cache.aInputs[cache.CurrentTick];

		// Apply direction
		if(Dummy == g_Config.m_ClDummy)
		{
			pGame->m_Controls.m_aInputDirectionLeft[Dummy] = (src.m_Direction < 0) ? 1 : 0;
			pGame->m_Controls.m_aInputDirectionRight[Dummy] = (src.m_Direction > 0) ? 1 : 0;
		}
		else
		{
			pInput->m_Direction = src.m_Direction;
		}

		// Apply jump
		pInput->m_Jump = src.m_Jump;

		// Apply hook and aim — exactly as planned. The plan is simulated
		// with freeze as a dead branch, so any hand-of-god hook edit here
		// would only desync the replay mid-swing.
		pInput->m_Hook = src.m_Hook;
		if(src.m_Hook || src.m_TargetX != 0 || src.m_TargetY != 0)
		{
			SetMousePos(pGame, Dummy, vec2((float)src.m_TargetX, (float)src.m_TargetY));
		}

		pInput->m_PlayerFlags |= 1;
		cache.CurrentTick++;
	}
	else
	{
		// Cache exhausted, mark for recomputation
		// IMPORTANT: Clear hook/jump to prevent them leaking and getting stuck
		pInput->m_Hook = 0;
		pInput->m_Jump = 0;
		cache.Reset();
	}
}

// =========================================================
// DEBUG RENDERING — block plan path + guidance field
// =========================================================

void CBotNet::RenderPackage(int Dummy)
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return;

	const CHelpersPlan &cache = m_aDummies[Dummy].m_HelpersPlan;
	if(!cache.IsValid() || cache.vPath.size() < 2)
		return;

	// Render the planned physics path — fixed alpha, independent of trajectory settings
	Graphics()->TextureClear();

	const ColorRGBA HelpersColor = ColorRGBA(0.2f, 1.0f, 0.4f, 1.0f);
	const float PathAlpha = 0.8f;
	const float HalfWidth = 1.5f;

	{
		std::vector<IGraphics::CFreeformItem> vQuads;
		vQuads.reserve(cache.vPath.size() - 1);

		for(size_t i = 1; i < cache.vPath.size(); i++)
		{
			vec2 p0 = cache.vPath[i - 1];
			vec2 p1 = cache.vPath[i];
			vec2 Dir = normalize(p1 - p0);
			vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;

			vQuads.emplace_back(
				p0.x - Perp.x, p0.y - Perp.y,
				p0.x + Perp.x, p0.y + Perp.y,
				p1.x - Perp.x, p1.y - Perp.y,
				p1.x + Perp.x, p1.y + Perp.y);
		}

		Graphics()->QuadsBegin();
		for(size_t i = 0; i < vQuads.size(); i++)
		{
			float t = (float)(i + 1) / (float)cache.vPath.size();
			float Alpha = PathAlpha * (1.0f - t * 0.7f);
			Graphics()->SetColor(HelpersColor.r, HelpersColor.g, HelpersColor.b, Alpha);
			Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
		}
		Graphics()->QuadsEnd();
	}
}

void CBotNet::RenderVectorField()
{
	if(!m_MapGridLoaded)
		return;
	if(!m_pfDist)
		return;

	CGameClient *pGame = GameClient();

	// Get camera position for viewport culling
	float CamX = pGame->m_Camera.m_Center.x;
	float CamY = pGame->m_Camera.m_Center.y;
	float CamW = pGame->m_Camera.m_Zoom * Graphics()->ScreenWidth() / 2.0f;
	float CamH = pGame->m_Camera.m_Zoom * Graphics()->ScreenHeight() / 2.0f;

	int minTX = pf_max(0, (int)((CamX - CamW) / 32.0f) - 1);
	int maxTX = pf_min(m_MapWidth - 1, (int)((CamX + CamW) / 32.0f) + 1);
	int minTY = pf_max(0, (int)((CamY - CamH) / 32.0f) - 1);
	int maxTY = pf_min(m_MapHeight - 1, (int)((CamY + CamH) / 32.0f) + 1);

	// Sample every 2 tiles for performance
	int step = 2;

	Graphics()->TextureClear();
	Graphics()->LinesBegin();

	for(int ty = minTY; ty <= maxTY; ty += step)
	{
		for(int tx = minTX; tx <= maxTX; tx += step)
		{
			int idx = ty * m_MapWidth + tx;
			if(m_pfDist[idx] >= 1e17f)
				continue;
			if(!IsTileWalkable(tx, ty))
				continue;

			float currentD = m_pfDist[idx];

			// Find the neighbor with the smallest distance (gradient direction)
			float bestDx = 0, bestDy = 0;
			float bestDist = currentD;
			int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
			int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

			for(int d = 0; d < 8; d++)
			{
				int nr = ty + dr[d];
				int nc = tx + dc[d];
				if(nr < 0 || nc < 0 || nr >= m_MapHeight || nc >= m_MapWidth)
					continue;
				int nIdx = nr * m_MapWidth + nc;
				if(m_pfDist[nIdx] < bestDist)
				{
					bestDist = m_pfDist[nIdx];
					bestDx = (float)(nc - tx);
					bestDy = (float)(nr - ty);
				}
			}

			if(bestDx == 0 && bestDy == 0)
				continue;

			float len = sqrtf(bestDx * bestDx + bestDy * bestDy);
			if(len < 0.001f)
				continue;

			bestDx /= len;
			bestDy /= len;

			float cx = tx * 32.0f + 16.0f;
			float cy = ty * 32.0f + 16.0f;
			float arrowLen = 12.0f;

			vec2 start(cx, cy);
			vec2 end(cx + bestDx * arrowLen, cy + bestDy * arrowLen);

			// Color based on distance: close to target = green, far = yellow
			float maxVisDist = 100.0f;
			float ratio = pf_min(currentD / maxVisDist, 1.0f);
			Graphics()->SetColor(1.0f, 1.0f - ratio * 0.5f, 0.0f, 0.4f);
			IGraphics::CLineItem Line(start, end);
			Graphics()->LinesDraw(&Line, 1);
		}
	}

	Graphics()->LinesEnd();
}

// =========================================================
// REAL-TIME PATHFINDER — worker-thread DFS planner (always finds)
// =========================================================
// Single-branch rolling planner with real backtracking. Every plan tick is
// one DFS node: candidates are simulated over a short fixed lookahead and
// ranked by progress along the RT flow field. Death and forbidden zones
// reject a candidate outright; freeze contact and leaving the field are
// allowed as a last resort (huge penalty) — so a route exists on freeze-
// heavy maps too, even if it is slow. Zero/negative-progress moves commit
// while the stall counter is low (setup moves, lateral crawls around walls;
// net movement away from the stall anchor resets the counter); a long stall
// turns "no progress" into a dead end and the DFS pops committed ticks back
// to the last choice, bans the input chosen there and searches other
// branches. Unpublished tail ticks pop worker-locally; published ones are
// retracted through the mailbox (out-box pop, or one TruncateTo when the
// main thread already merged them — never touching played/playing ticks).
// Generated ticks go into the m_RtPfOut* mailbox and the MAIN thread
// commits them (UpdateRtPathfinder); the chain rolls continuously and
// re-anchors on demand.
// =========================================================

// Clone the base world and strip everything except the local character
// (same as the Pathfinder tab's setup: other players/entities removed so
// the plan stays deterministic).
static void RtPfSetupWorld(CGameWorld *pWorld, CGameWorld *pSrc, int LocalID)
{
	pWorld->CopyWorld(pSrc);
	for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
	{
		if(Type == CGameWorld::ENTTYPE_CHARACTER)
			continue;
		std::vector<CEntity *> vRemove;
		for(CEntity *pEnt = pWorld->FindLast(Type); pEnt; pEnt = pEnt->TypePrev())
			vRemove.push_back(pEnt);
		for(CEntity *pEnt : vRemove)
			pWorld->RemoveEntity(pEnt);
	}
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalID)
			continue;
		if(CCharacter *pChar = pWorld->GetCharacterById(i))
			pWorld->RemoveEntity(pChar);
	}
}

// (Re)spawn the sim bot at cur state. Mirrors the tab's PfSimWorldBot:
// a dead character is replaced with a copy of the base one so the sim can
// continue after deaths in the planning world.
static CCharacter *RtPfEnsureBot(CGameWorld *pSim, CGameWorld *pBaseWorld, int LocalID)
{
	if(CCharacter *pBot = pSim->GetCharacterById(LocalID))
		return pBot;
	CCharacter *pBaseChar = pBaseWorld->GetCharacterById(LocalID);
	if(!pBaseChar)
		return nullptr;
	if(CCharacter *pDead = (CCharacter *)pSim->FindFirst(CGameWorld::ENTTYPE_CHARACTER))
		delete pDead;
	CCharacter *pCopy = new CCharacter(*pBaseChar);
	pCopy->m_pParent = pBaseChar;
	pBaseChar->m_pChild = pCopy;
	pSim->InsertEntity(pCopy);
	return pCopy;
}

// Write the planner state into the sim bot (mirror PfSpawnSimBot + JumpedTotal).
static CCharacter *RtPfPlaceBot(CGameWorld *pSim, CGameWorld *pBase, int LocalID,
	const CBotNet::PfAState &cur, int curFreeze, int Tick)
{
	CCharacter *pBot = RtPfEnsureBot(pSim, pBase, LocalID);
	if(!pBot)
		return nullptr;
	pBot = PfSpawnSimBot(pSim, LocalID, cur.Pos, cur.Vel,
		cur.HookState, cur.HookPos, cur.HookDir, cur.HookTick, curFreeze, cur.Jumped);
	if(!pBot)
		return nullptr;
	CCharacterCore c = pBot->GetCore();
	c.m_JumpedTotal = cur.JumpedTotal;
	pBot->SetCore(c);
	pSim->m_GameTick = Tick;
	return pBot;
}

// One committed plan tick, kept for backtracking. The chain is a stack:
// committing a tick pushes its step, a dead end pops steps down to the last
// CHOICE step, restores the state from it and bans the input chosen there
// (Tried) — the DFS escape from dead ends. Burn steps (freeze fast-forward
// ticks) are deterministic and carry no choice: a dead end pops straight
// through them to the choice below.
struct RtPfChainStep
{
	CBotNet::PfAState Prev;
	int PrevFreeze;
	int PrevStall;
	vec2 PrevStallAnchor;
	bool Burn;
	std::vector<CNetObj_PlayerInput> Tried;
};

// Tail hook segment: the chunk-relative index is resolved on publish.
struct RtPfTailSeg
{
	vec2 TeePos;
	vec2 HookPos;
	int Idx;
};

void CBotNet::RtPfThreadWorker()
{
	int LocalID = m_RtPfLocalID;
	if(!m_RtPfBaseWorld || LocalID < 0)
	{
		m_RtPfThreadRunning.store(false);
		return;
	}

	// Worker owns the sim world for the whole run; RtPfThreadStop joins the
	// thread on the main thread BEFORE deleting it, so plain new/delete is safe.
	delete m_RtPfSimWorld;
	m_RtPfSimWorld = new CGameWorld();
	RtPfSetupWorld(m_RtPfSimWorld, m_RtPfBaseWorld, LocalID);
	if(!m_RtPfSimWorld->Collision())
	{
		dbg_msg("rtpathfinder", "worker: no collision in sim world");
		m_RtPfThreadRunning.store(false);
		return;
	}

	const std::vector<vec2> *pFinishes = &RtPfActiveFinishTiles();
	if(pFinishes->empty())
	{
		dbg_msg("rtpathfinder", "worker: no finish tiles");
		m_RtPfThreadRunning.store(false);
		return;
	}

	CCollision *pColl = m_RtPfSimWorld->Collision();
	float hookLength = 380.0f;
	if(CCharacter *pBaseChar = m_RtPfBaseWorld->GetCharacterById(LocalID))
	{
		float hl = pBaseChar->GetCore().m_Tuning.m_HookLength;
		if(hl > 1.0f && hl < 10000.0f)
			hookLength = hl;
	}

	// Planner state at the plan's end. The worker NEVER writes the shared
	// plan/playback variables — it only GENERATES path data into the
	// m_RtPfOut* mailbox; the main thread commits it (UpdateRtPathfinder).
	PfAState cur;
	mem_zero(&cur, sizeof(cur));
	int curFreeze = 0;
	int simTick = m_RtPfBaseWorld->GameTick();
	bool stateValid = false;
	std::vector<RtPfChainStep> vChain; // committed ticks, for backtracking
	std::vector<CNetObj_PlayerInput> vTried; // inputs already tried at the tick being generated
	std::vector<CNetObj_PlayerInput> vTail; // committed but not yet published ticks
	std::vector<vec2> vTailPath;
	std::vector<RtPfTailSeg> vTailSegs;
	int stallStreak = 0; // consecutive committed ticks without progress
	vec2 stallAnchor(0, 0); // where the current stall began

	// Planner tuning
	const int Lookahead = 12;  // sim depth per candidate
	const int TailMax = 48;    // stalled ticks held unpublished before the tail flushes
	const int StallLimit = 24; // stalled ticks tolerated before the DFS escape kicks in
	const int ChainMax = 512;  // rollback stack cap (only the back is ever popped)

	// guidance mode for the current plan (recomputed at every re-anchor):
	// when the tile field cannot see the anchor — the tab's grid-unreachable
	// case (A* falls back to the air-distance heuristic; hook-only routes and
	// freeze separators blind the tile BFS) — scoring falls back to
	// straight-line air distance so the planner still drives to the finish
	// like the tab does.
	bool guideAir = false;

	// distance guidance for scoring: the RT flow field (field mode) or
	// straight-line distance to the nearest finish scaled like the tab's air
	// heuristic (air mode).
	auto fieldDist = [&](const vec2 &pos) -> float {
		if(guideAir)
		{
			float bestAir = 1e18f;
			for(const vec2 &fp : *pFinishes)
			{
				float dx = pos.x - fp.x;
				float dy = pos.y - fp.y;
				float d = sqrtf(dx * dx + dy * dy);
				if(d < bestAir)
					bestAir = d;
			}
			return (bestAir < 1e18f) ? bestAir / 16.0f : 1e18f;
		}
		if(!m_RtPfFlowField || m_MapWidth <= 0 || m_MapHeight <= 0)
			return 0.0f;
		int tx = pf_clamp((int)(pos.x / 32.0f), 0, m_MapWidth - 1);
		int ty = pf_clamp((int)(pos.y / 32.0f), 0, m_MapHeight - 1);
		return m_RtPfFlowField[ty * m_MapWidth + tx];
	};

	// Flush the unpublished tail into the mailbox. Dropped silently when a
	// re-anchor arrived meanwhile — the fresh root rebuilds everything.
	auto fnFlushTail = [&]() {
		if(vTail.empty())
			return;
		{
			std::lock_guard<std::mutex> lock(m_RtPfDataMutex);
			if(!m_RtPfRebuildPending.load(std::memory_order_relaxed))
			{
				const int Base = (int)m_RtPfOutInputs.size();
				m_RtPfOutInputs.insert(m_RtPfOutInputs.end(), vTail.begin(), vTail.end());
				m_RtPfOutPath.insert(m_RtPfOutPath.end(), vTailPath.begin(), vTailPath.end());
				for(const RtPfTailSeg &Seg : vTailSegs)
					m_RtPfOutSegs.push_back({Seg.TeePos, Seg.HookPos, Base + Seg.Idx});
				m_RtPfOutValid = true;
			}
		}
		vTail.clear();
		vTailPath.clear();
		vTailSegs.clear();
	};

	// DFS escape from a dead end: undo committed ticks down to the last
	// choice step, restore the state from it and return (vTried then bans
	// the input that was chosen there). Tail ticks pop worker-locally;
	// published ones are retracted under the mutex — an out-box pop, or one
	// TruncateTo when the main thread already merged them into the plan
	// (never touching played/playing ticks). False = nothing left to undo.
	auto fnDeadEnd = [&]() -> bool {
		while(!vTail.empty())
		{
			RtPfChainStep Step = std::move(vChain.back());
			vChain.pop_back();
			vTail.pop_back();
			vTailPath.pop_back();
			if(!vTailSegs.empty() && vTailSegs.back().Idx == (int)vTail.size())
				vTailSegs.pop_back();
			simTick--;
			if(!Step.Burn)
			{
				cur = Step.Prev;
				curFreeze = Step.PrevFreeze;
				stallStreak = Step.PrevStall;
				stallAnchor = Step.PrevStallAnchor;
				vTried = std::move(Step.Tried);
				return true;
			}
		}
		std::lock_guard<std::mutex> lock(m_RtPfDataMutex);
		if(!vChain.empty())
		{
			int Run = 0; // trailing deterministic burn steps under the choice
			while(Run < (int)vChain.size() && vChain[vChain.size() - 1 - Run].Burn)
				Run++;
			if(Run >= (int)vChain.size())
				return false; // only burn steps left — no choice to restore
			const int Retract = Run + 1; // burns + the choice tick
			const int OutSize = (int)m_RtPfOutInputs.size();
			if(OutSize >= Retract)
			{
				// the whole run is still pending in the out-box: shrink it here
				m_RtPfOutInputs.resize(OutSize - Retract);
				m_RtPfOutPath.resize((int)m_RtPfOutPath.size() - Retract);
				for(int i = (int)m_RtPfOutSegs.size() - 1; i >= 0; i--)
				{
					if(m_RtPfOutSegs[i].tick >= OutSize - Retract)
						m_RtPfOutSegs.erase(m_RtPfOutSegs.begin() + i);
				}
				m_RtPfOutValid = true;
			}
			else
			{
				// part (or all) of the run is already merged into the plan:
				// retract the merged part with one truncation
				const int Keep = (int)m_RtPfInputs.size() - (Retract - OutSize);
				if(m_RtPfOutTruncateTo >= 0 || Keep < m_RtPfPlayIdx + 2)
					return false; // the tick is playing/played — cannot undo
				if(OutSize > 0)
				{
					// every pending tick belongs to the run — drop them all
					m_RtPfOutInputs.clear();
					m_RtPfOutPath.clear();
					m_RtPfOutSegs.clear();
				}
				m_RtPfOutTruncateTo = Keep;
				m_RtPfOutValid = true;
			}
			RtPfChainStep Step = std::move(vChain[vChain.size() - 1 - Run]);
			vChain.resize((int)vChain.size() - Retract);
			simTick -= Retract;
			cur = Step.Prev;
			curFreeze = Step.PrevFreeze;
			stallStreak = Step.PrevStall;
			stallAnchor = Step.PrevStallAnchor;
			vTried = std::move(Step.Tried);
			return true;
		}
		return false;
	};

	while(!m_RtPfThreadCancel.load(std::memory_order_relaxed))
	{
		int futureTicks = g_Config.m_KxRtPfFutureLimit ? pf_clamp(g_Config.m_KxRtPfFutureTicks, 1, 150) : 4096; // limit off: effectively unbounded (the solver caps itself)
		bool reanchor = false;
		int unplayed = 0;
		{
			std::lock_guard<std::mutex> lock(m_RtPfDataMutex);
			if(m_RtPfRebuildPending.load(std::memory_order_relaxed))
			{
				m_RtPfRebuildPending.store(false, std::memory_order_relaxed);
				reanchor = true;
				cur = m_RtPfRebuildCore;
				curFreeze = m_RtPfRebuildFreeze;
				simTick = m_RtPfAnchorTick; // keep the sim's absolute tick aligned with the live one
			}
			unplayed = (int)m_RtPfInputs.size() - m_RtPfPlayIdx + (int)m_RtPfOutInputs.size();
		}
		if(unplayed < 0)
			unplayed = 0;

		// ── Re-anchor at a new root (run start / reconstruct): the main
		// thread already cleared the plan. Reset the chain and, when the
		// tee is frozen, burn the freeze with empty inputs (same trick as
		// the A* frozen-node expansion) and hand those ticks over first.
		if(reanchor)
		{
			stateValid = true;
			vChain.clear();
			vTried.clear();
			vTail.clear();
			vTailPath.clear();
			vTailSegs.clear();
			stallStreak = 0;
			stallAnchor = cur.Pos;
			// guidance mode for this plan: air-distance fallback when the tile
			// field cannot see the anchor (the tab's grid-unreachable case)
			if(m_RtPfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
			{
				int gx = pf_clamp((int)(cur.Pos.x / 32.0f), 0, m_MapWidth - 1);
				int gy = pf_clamp((int)(cur.Pos.y / 32.0f), 0, m_MapHeight - 1);
				guideAir = m_RtPfFlowField[gy * m_MapWidth + gx] >= 1e17f;
			}
			else
				guideAir = true;
			std::vector<CNetObj_PlayerInput> vFF;
			std::vector<vec2> vFFPath;
			if(curFreeze > 0)
			{
				CCharacter *pBot = RtPfPlaceBot(m_RtPfSimWorld, m_RtPfBaseWorld, LocalID, cur, curFreeze, simTick);
				if(!pBot)
				{
					stateValid = false;
				}
				else
				{
					// Tick-by-tick sim (like ComputeHelpersPath) — freeze countdown
					// with a clear hook state; death inside freeze just ends the FF.
					vec2 prevPos = cur.Pos;
					int used = 0;
					for(int t = 0; t < curFreeze; t++)
					{
						CNetObj_PlayerInput ff;
						mem_zero(&ff, sizeof(CNetObj_PlayerInput));
						ff.m_TargetY = -1;
						pBot->OnDirectInput(&ff);
						m_RtPfSimWorld->m_GameTick = simTick + t + 1;
						pBot->OnPredictedInput(&ff);
						m_RtPfSimWorld->Tick();
						if(!m_RtPfSimWorld->GetCharacterById(LocalID))
							break;
						if(pBot->m_FreezeTime <= 0)
							PfCheckFreeze(m_RtPfSimWorld, pBot, prevPos, pBot->m_Pos);
						prevPos = pBot->m_Pos;
						vFF.push_back(ff);
						vFFPath.push_back(pBot->m_Pos);
						used++;
						if(pBot->m_FreezeTime <= 0)
							break; // unfrozen — plan from here
					}
					if(m_RtPfSimWorld->GetCharacterById(LocalID) && used > 0)
					{
						CCharacterCore c = pBot->GetCore();
						cur.Pos = c.m_Pos;
						cur.Vel = c.m_Vel;
						cur.HookState = c.m_HookState;
						cur.HookPos = c.m_HookPos;
						cur.HookDir = c.m_HookDir;
						cur.HookTick = c.m_HookTick;
						cur.Jumped = c.m_Jumped;
						cur.JumpedTotal = c.m_JumpedTotal;
						curFreeze = pBot->m_FreezeTime;
						simTick += used;
					}
				}
			}
			if(stateValid && !vFF.empty())
			{
				std::lock_guard<std::mutex> lock(m_RtPfDataMutex);
				if(!m_RtPfRebuildPending.load(std::memory_order_relaxed))
				{
					m_RtPfOutInputs.insert(m_RtPfOutInputs.end(), vFF.begin(), vFF.end());
					m_RtPfOutPath.insert(m_RtPfOutPath.end(), vFFPath.begin(), vFFPath.end());
					m_RtPfOutValid = true;
				}
			}
			continue; // resample the pacing state before generating the next tick
		}

		if(!stateValid)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}

		// ── Keep the unplayed tail at Future-Ticks length.
		if(unplayed >= futureTicks)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}

		// ── Goal: the plan reached a finish tile — nothing left to generate.
		// The committed tail still plays out; the main thread stops the run
		// when the LIVE tee arrives at a finish.
		if(RtPfIsAtFinishPos(cur.Pos))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		// ── Frozen: burn the freeze with deterministic empty-input ticks ──
		// One burn tick per iteration; the tail flushes eagerly so the frozen
		// tee keeps ticking. Death inside freeze stops the plan — the main
		// thread's reconstruct re-anchors after the respawn.
		if(curFreeze > 0)
		{
			CCharacter *pBot = RtPfPlaceBot(m_RtPfSimWorld, m_RtPfBaseWorld, LocalID, cur, curFreeze, simTick);
			if(!pBot)
			{
				stateValid = false;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			CNetObj_PlayerInput ff;
			mem_zero(&ff, sizeof(CNetObj_PlayerInput));
			ff.m_TargetY = -1;
			pBot->OnDirectInput(&ff);
			m_RtPfSimWorld->m_GameTick = simTick + 1;
			pBot->OnPredictedInput(&ff);
			m_RtPfSimWorld->Tick();
			if(!m_RtPfSimWorld->GetCharacterById(LocalID))
			{
				stateValid = false;
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}
			RtPfChainStep Step;
			Step.Prev = cur;
			Step.PrevFreeze = curFreeze;
			Step.PrevStall = stallStreak;
			Step.PrevStallAnchor = stallAnchor;
			Step.Burn = true;
			Step.Tried.push_back(ff);
			CCharacterCore c = pBot->GetCore();
			cur.Pos = c.m_Pos;
			cur.Vel = c.m_Vel;
			cur.HookState = c.m_HookState;
			cur.HookPos = c.m_HookPos;
			cur.HookDir = c.m_HookDir;
			cur.HookTick = c.m_HookTick;
			cur.Jumped = c.m_Jumped;
			cur.JumpedTotal = c.m_JumpedTotal;
			curFreeze = pBot->m_FreezeTime;
			if(curFreeze <= 0)
			{
				stallStreak = 0; // freeze is dead time — do not let the old stall cascade
				stallAnchor = cur.Pos;
			}
			simTick++;
			vTail.push_back(ff);
			vTailPath.push_back(cur.Pos);
			vChain.push_back(std::move(Step));
			if((int)vChain.size() > ChainMax)
				vChain.erase(vChain.begin());
			fnFlushTail(); // burn ticks stream at once — the frozen tee must keep ticking
			continue;
		}

		// ── Choose the next plan tick: evaluate every candidate over the fixed
		// lookahead and take the best-ranked survivor. Death and forbidden
		// zones reject outright; freeze contact and leaving the field are
		// allowed as a last resort (huge penalties), so a route exists even on
		// freeze-heavy maps. Clean candidates need no progress while the stall
		// counter is low (setup moves, lateral crawls — net movement away from
		// the stall anchor resets it); a stalled-out tick is a dead end and the
		// DFS backtrack explores other branches.
		CCharacter *pBot = RtPfPlaceBot(m_RtPfSimWorld, m_RtPfBaseWorld, LocalID, cur, curFreeze, simTick);
		if(!pBot)
		{
			stateValid = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}

		int HookAngles = PfEffortHookAngles();
		CNetObj_PlayerInput aCands[128];
		int nCands = 0;
		{
			// KHelpersBuildCandidates works on the Helpers planner snapshot; the RT
			// worker keeps its state as PfAState - same fields, convert explicitly.
			KHelpersState curK;
			curK.Pos = cur.Pos;
			curK.Vel = cur.Vel;
			curK.HookPos = cur.HookPos;
			curK.HookDir = cur.HookDir;
			curK.HookState = cur.HookState;
			curK.HookTick = cur.HookTick;
			curK.Jumped = cur.Jumped;
			curK.JumpedTotal = cur.JumpedTotal;
			nCands = KHelpersBuildCandidates(curK, m_RtPfSimWorld, pBot, pColl, hookLength, HookAngles, aCands);
		}

		float lastDist = fieldDist(cur.Pos);

		float bestScore = -1e9f;
		int best = -1;
		CNetObj_PlayerInput bestIn;
		vec2 bestPos = cur.Pos;
		PfAState bestEnd = cur;
		int bestEndFreeze = curFreeze;
		bool haveSeg = false;
		PfHookSeg bestSeg = {};
		bool bestProgress = false;

		for(int ci = 0; ci < nCands; ci++)
		{
			// backtrack bans: this input already dead-ended from this tick —
			// skip it without re-simulating
			bool banned = false;
			for(const CNetObj_PlayerInput &t : vTried)
			{
				if(t.m_Direction == aCands[ci].m_Direction && t.m_Jump == aCands[ci].m_Jump &&
					t.m_Hook == aCands[ci].m_Hook && t.m_TargetX == aCands[ci].m_TargetX &&
					t.m_TargetY == aCands[ci].m_TargetY)
				{
					banned = true;
					break;
				}
			}
			if(banned)
				continue;
			pBot = RtPfPlaceBot(m_RtPfSimWorld, m_RtPfBaseWorld, LocalID, cur, curFreeze, simTick);
			if(!pBot)
				break;
			vec2 prevPos = cur.Pos;
			int prevHookState = cur.HookState;
			float adjPenalty = 0.0f;
			bool died = false;
			int freezeTicks = 0;
			CCharacterCore firstCore = pBot->GetCore();
			int firstFreeze = curFreeze;
			vec2 firstPos = cur.Pos;
			bool firstGrab = false;
			vec2 firstGrabPos = cur.Pos;

			// fixed lookahead: repeat the candidate's input and score the total
			// progress — like the Pathfinder planner's windows — then commit
			// ONLY this candidate's first tick
			for(int t = 0; t < Lookahead; t++)
			{
				CNetObj_PlayerInput in = aCands[ci];
				if(in.m_Jump && t > 0)
					in.m_Jump = 0; // single jump impulse, like the Pathfinder windows

				pBot->OnDirectInput(&in);
				m_RtPfSimWorld->m_GameTick = simTick + t + 1;
				pBot->OnPredictedInput(&in);
				m_RtPfSimWorld->Tick();
				if(!m_RtPfSimWorld->GetCharacterById(LocalID))
				{
					died = true; // death rejects the candidate outright
					break;
				}

				// Tab-exact anti-skip freeze check after the engine tick.
				if(pBot->m_FreezeTime <= 0)
					PfCheckFreeze(m_RtPfSimWorld, pBot, prevPos, pBot->m_Pos);

				// RT forbidden zones are absolute walls for the planner.
				if(PfLineTouchesForbidden(m_pRtForbiddenGrid, m_MapWidth, m_MapHeight, prevPos, pBot->m_Pos))
				{
					died = true;
					break;
				}

				if(t == 0)
				{
					// capture the committed tick's end state BEFORE the soft
					// freeze exit — a first-tick freeze is a valid (slow) move
					firstCore = pBot->GetCore();
					firstFreeze = pBot->m_FreezeTime;
					firstPos = pBot->m_Pos;
					firstGrab = firstCore.m_HookState == HOOK_GRABBED && prevHookState != HOOK_GRABBED;
					firstGrabPos = firstPos;
				}

				if(pBot->m_FreezeTime > 0)
				{
					if(g_Config.m_KxPfFreezeSupport == 0)
					{
						died = true; // freeze support off — freeze contact rejects the candidate outright
						break;
					}
					freezeTicks++; // ranks below every clean candidate
					break;
				}

				// soft margin — discourage hugging freeze
				int ptx = pf_clamp((int)(pBot->m_Pos.x / 32.0f), 0, m_MapWidth - 1);
				int pty = pf_clamp((int)(pBot->m_Pos.y / 32.0f), 0, m_MapHeight - 1);
				bool adj = false;
				for(int oy = -1; oy <= 1 && !adj; oy++)
					for(int ox = -1; ox <= 1 && !adj; ox++)
						if(IsTileFreeze(pf_clamp(ptx + ox, 0, m_MapWidth - 1), pf_clamp(pty + oy, 0, m_MapHeight - 1)))
							adj = true;
				if(adj)
					adjPenalty += 0.05f;

				prevPos = pBot->m_Pos;
			}
			if(died)
				continue;

			float endDist = fieldDist(pBot->m_Pos);
			bool offRoute = endDist >= 1e17f;
			float progress = offRoute ? 0.0f : (lastDist - endDist);
			float score = progress - adjPenalty;
			if(freezeTicks > 0)
				score -= 10000.0f * freezeTicks;
			if(offRoute)
				score -= 5000.0f;

			if(score > bestScore)
			{
				bestScore = score;
				best = ci;
				bestIn = aCands[ci];
				bestPos = firstPos;
				bestEnd.Pos = firstCore.m_Pos;
				bestEnd.Vel = firstCore.m_Vel;
				bestEnd.HookState = firstCore.m_HookState;
				bestEnd.HookPos = firstCore.m_HookPos;
				bestEnd.HookDir = firstCore.m_HookDir;
				bestEnd.HookTick = firstCore.m_HookTick;
				bestEnd.Jumped = firstCore.m_Jumped;
				bestEnd.JumpedTotal = firstCore.m_JumpedTotal;
				bestEndFreeze = firstFreeze;
				haveSeg = firstGrab;
				if(haveSeg)
					bestSeg = {firstGrabPos, firstCore.m_HookPos};
				bestProgress = !offRoute && freezeTicks == 0 && progress > 0.01f;
			}
		}

		// Commit the winner, or backtrack. Clean progress commits freely;
		// non-progress moves commit while the stall counter allows them (net
		// movement away from the stall anchor also resets it — lateral crawls
		// around walls stay legal); a stalled-out tick is a dead end.
		bool committed = false;
		if(best >= 0 && (bestProgress || stallStreak < StallLimit))
		{
			RtPfChainStep Step;
			Step.Prev = cur;
			Step.PrevFreeze = curFreeze;
			Step.PrevStall = stallStreak;
			Step.PrevStallAnchor = stallAnchor;
			Step.Burn = false;
			Step.Tried = vTried;
			Step.Tried.push_back(bestIn);
			if(bestProgress)
			{
				stallStreak = 0;
				stallAnchor = bestEnd.Pos;
			}
			else
			{
				if(stallStreak == 0)
					stallAnchor = cur.Pos;
				stallStreak++;
				const float dxs = bestEnd.Pos.x - stallAnchor.x;
				const float dys = bestEnd.Pos.y - stallAnchor.y;
				if(dxs * dxs + dys * dys > 64.0f * 64.0f)
				{
					// net movement breaks the stall — lateral crawls stay legal
					stallStreak = 0;
					stallAnchor = bestEnd.Pos;
				}
			}
			const int TailIdx = (int)vTail.size();
			vTail.push_back(bestIn);
			vTailPath.push_back(bestPos);
			if(haveSeg)
				vTailSegs.push_back({bestSeg.teePos, bestSeg.hookPos, TailIdx});
			vChain.push_back(std::move(Step));
			if((int)vChain.size() > ChainMax)
				vChain.erase(vChain.begin()); // rollback stack cap
			cur = bestEnd;
			curFreeze = bestEndFreeze;
			simTick++;
			vTried.clear();
			committed = true;
		}

		if(!committed)
		{
			// Nothing survived (everything dies / hits forbidden zones / was
			// banned / stalled out) — the DFS pops back to the last choice
			// and tries another branch. With nothing left to undo the plan
			// stops growing: the main thread exhausts it and re-anchors via
			// reconstruct.
			if(!fnDeadEnd())
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}

		// Publish: progress ticks and freeze burns stream at once; stalled
		// segments are held worker-locally so backtracking inside them costs
		// nothing, until they resolve or fill the tail.
		if(bestProgress || curFreeze > 0 || (int)vTail.size() >= TailMax)
			fnFlushTail();
	}

	m_RtPfThreadRunning.store(false);
}
