#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <algorithm>
#include <unordered_set>

// =========================================================
// PHYSICS A* — state-lattice search over simulated input chunks.
//
// Runs on the worker thread (PfThreadWorker). Each expansion pops
// the best open node, generates candidate input chunks (RHEA or
// exhaustive BFS combination search), simulates them through the real CCharacter physics
// (PfSimulateChunk) and pushes surviving end states as children.
//
// Cost model (v1.58 rewrite):
//   G = accumulated ticks (+ optional stuck penalty). G is PURE cost —
//   candidate scores only RANK candidates, they never leak into G.
//   The old code subtracted the chunk score from G, which could make
//   G negative, corrupt the BestG dedup and burn the node budget on
//   garbage branches (the "pathfinder gives up with forbidden zones"
//   bug).
//   TickOffset = true simulated ticks from the run start.
// =========================================================

// Deterministic PRNG for RHEA. Reset at the start of every run so the
// same map + start state always produces the same candidate sequences.
static uint32_t PfRheaState = 12345;
static inline void PfRheaReset() { PfRheaState = 12345; }
static inline uint32_t PfRheaNext()
{
	PfRheaState ^= PfRheaState << 13;
	PfRheaState ^= PfRheaState >> 17;
	PfRheaState ^= PfRheaState << 5;
	return PfRheaState;
}
static inline int PfRheaRand(int max) { return max <= 0 ? 0 : (int)(PfRheaNext() % (uint32_t)max); }
static inline float PfRheaChance() { return (float)PfRheaNext() / 4294967295.0f; }

// State dedup key: quantized position (4px), velocity (7 buckets/axis),
// grounded, hook state, frozen flag, jump flags.
static uint64_t PfAStarKey(const CCharacterCore &core, int freezeTime, bool grounded)
{
	int tx = (int)(core.m_Pos.x / 4.0f);
	int ty = (int)(core.m_Pos.y / 4.0f);
	int vbx = pf_clamp((int)(core.m_Vel.x / 5.0f) + 3, 0, 6);
	int vby = pf_clamp((int)(core.m_Vel.y / 5.0f) + 3, 0, 6);
	int frozen = (freezeTime > 0) ? 1 : 0;
	uint64_t k = 0;
	k |= (uint64_t)(uint16_t)(int16_t)tx;
	k |= (uint64_t)(uint16_t)(int16_t)ty << 16;
	k |= (uint64_t)(uint8_t)vbx << 32;
	k |= (uint64_t)(uint8_t)vby << 35;
	k |= (uint64_t)(grounded ? 1 : 0) << 38;
	k |= (uint64_t)(uint8_t)(core.m_HookState + 8) << 39;
	k |= (uint64_t)(frozen & 1) << 43;
	k |= (uint64_t)(uint8_t)(core.m_Jumped & 0xF) << 44;
	return k;
}

static void PfAStarHeapPush(std::vector<int> &heap, const std::vector<CBotNet::AStarNode> &nodes, int idx)
{
	heap.push_back(idx);
	int i = (int)heap.size() - 1;
	while(i > 0)
	{
		int parent = (i - 1) / 2;
		if(nodes[heap[parent]].F <= nodes[heap[i]].F)
			break;
		std::swap(heap[parent], heap[i]);
		i = parent;
	}
}

static int PfAStarHeapPop(std::vector<int> &heap, const std::vector<CBotNet::AStarNode> &nodes)
{
	int top = heap[0];
	heap[0] = heap.back();
	heap.pop_back();
	int n = (int)heap.size();
	int i = 0;
	while(true)
	{
		int l = 2 * i + 1, r = 2 * i + 2, s = i;
		if(l < n && nodes[heap[l]].F < nodes[heap[s]].F)
			s = l;
		if(r < n && nodes[heap[r]].F < nodes[heap[s]].F)
			s = r;
		if(s == i)
			break;
		std::swap(heap[s], heap[i]);
		i = s;
	}
	return top;
}

// Stall cost (gated by KxPfFineStuck): a chunk that ends within 2px of its
// start made no progress — make that branch expensive in the open set.
int PfEffortChunkTicks()
{
	if(g_Config.m_KxPfEffort == 5)
	{
		int v = g_Config.m_KxPfChunkSize;
		return v < 1 ? 1 : (v > 50 ? 50 : v);
	}
	return (g_Config.m_KxPfEffort >= 3) ? 10 : 5;
}

int PfEffortHookAngles()
{
	if(g_Config.m_KxPfEffort == 5)
	{
		int v = g_Config.m_KxPfHookAngles;
		return v < 4 ? 4 : (v > 32 ? 32 : v);
	}
	return (g_Config.m_KxPfEffort >= 3) ? 32 : 24;
}

int PfEffortSearchBudget()
{
	if(g_Config.m_KxPfEffort == 5)
	{
		int v = g_Config.m_KxPfSearchBudget;
		return v < 256 ? 256 : (v > 200000 ? 200000 : v);
	}
	static const int s_aBudgets[5] = {10000, 50000, 100000, 150000, 200000};
	return s_aBudgets[g_Config.m_KxPfEffort];
}

int PfEffortCandidates()
{
	if(g_Config.m_KxPfEffort == 5)
	{
		int v = g_Config.m_KxPfCandidates;
		return v < 50 ? 50 : (v > 2000 ? 2000 : v);
	}
	return 2000;
}

int PfEffortGenerations()
{
	if(g_Config.m_KxPfEffort == 5)
	{
		int v = g_Config.m_KxPfRheaGenerations;
		return v < 1 ? 1 : (v > 100 ? 100 : v);
	}
	static const int s_aGenerations[5] = {5, 10, 10, 10, 15};
	return s_aGenerations[g_Config.m_KxPfEffort];
}

bool PfEffortScoreDist() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfScoreDist != 0 : true; }
bool PfEffortScoreFlow() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfScoreFlow != 0 : true; }
bool PfEffortFineFreeze() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineFreeze != 0 : false; }
bool PfEffortFreezeSupport() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFreezeSupport != 0 : false; }
bool PfEffortFineRetract() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineRetract != 0 : false; }
bool PfEffortFineDeath() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineDeath != 0 : true; }
bool PfEffortFineWall() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineWall != 0 : false; }
bool PfEffortFineAir() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineAir != 0 : false; }
bool PfEffortFineHookFire() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineHook != 0 : false; }
bool PfEffortFineStuck() { return g_Config.m_KxPfEffort == 5 ? g_Config.m_KxPfFineStuck != 0 : true; }

static float PfAStarStuckPenalty(const vec2 &startPos, const vec2 &endPos)
{
	if(!PfEffortFineStuck())
		return 0.0f;
	if(distance(endPos, startPos) < 2.0f)
		return 500.0f;
	return 0.0f;
}

static vec2 PfNearestFinishPos(const std::vector<vec2> &vFinishes, const vec2 &pos)
{
	vec2 best = vFinishes[0];
	float bestDist = 1e18f;
	for(const vec2 &fp : vFinishes)
	{
		float d = distance(fp, pos);
		if(d < bestDist)
		{
			bestDist = d;
			best = fp;
		}
	}
	return best;
}

// Goal test against ANY active finish tile — a candidate may legitimately
// reach a different finish than the one nearest to the run start.
static bool PfIsAtFinishPos(const std::vector<vec2> &vFinishes, const vec2 &pos)
{
	for(const vec2 &fp : vFinishes)
	{
		int tx = (int)floorf(fp.x / 32.0f);
		int ty = (int)floorf(fp.y / 32.0f);
		if(round_to_int(pos.x / 32.0f) == tx && round_to_int(pos.y / 32.0f) == ty)
			return true;
	}
	return false;
}

float CBotNet::PfAStarHeuristic(const vec2 &pos, const vec2 &endPos)
{
	// Wall-aware flow-field distance (tiles × ~2 ticks) when available,
	// otherwise straight-line air distance. The fallback keeps the search
	// directed even where the grid BFS cannot reach the position (jump gaps,
	// regions sealed by forbidden zones) instead of degenerating into blind
	// Dijkstra that burns the whole node budget.
	if(m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
	{
		int tx = pf_clamp((int)(pos.x / 32.0f), 0, m_MapWidth - 1);
		int ty = pf_clamp((int)(pos.y / 32.0f), 0, m_MapHeight - 1);
		float fd = m_PfFlowField[ty * m_MapWidth + tx];
		if(fd < 1e17f)
			return fd * 2.0f;
	}
	return distance(pos, endPos) / 16.0f;
}

// Clone the base world and strip everything except the local character:
// other entities and players must not influence the simulation.
static void PfAStarSetupWorld(CGameWorld *pWorld, CGameWorld *pSrc, int LocalID)
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

bool CBotNet::PfAStarInit()
{
	if(!m_PfBaseWorld)
		return false;
	int LocalID = m_PfLocalID;
	if(LocalID < 0)
		return false;
	CCharacter *pChar = m_PfBaseWorld->GetCharacterById(LocalID);
	if(!pChar)
		return false;
	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	if(vFinishes.empty())
		return false;

	PfRheaReset();

	m_PfANodes.clear();
	m_PfAOpen.clear();
	m_PfABestG.clear();
	m_PfAGoalIdx = -1;
	m_PfAExpandCount = 0;

	int W = pf_clamp(g_Config.m_KxPfAStarWeight, 1, 20);

	AStarNode root;
	root.Core = pChar->GetCore();
	root.Core.m_Pos = m_PfCurPos;
	root.Core.m_Vel = m_PfCurVel;
	root.Core.m_HookState = m_PfCurHookState;
	root.Core.m_HookPos = m_PfCurHookPos;
	root.Core.m_HookDir = m_PfCurHookDir;
	root.Core.m_HookTick = m_PfCurHookTick;
	root.Core.m_Jumped = m_PfCurJumped;
	root.Pos = m_PfCurPos;
	root.FreezeTime = m_PfCurFreezeTime;
	CCollision *pColl = m_PfBaseWorld->Collision();
	root.Grounded = pColl ? pColl->IsOnGround(root.Pos, CCharacterCore::PhysicalSize()) : false;
	root.G = 0.0f;
	root.H = PfAStarHeuristic(root.Pos, PfNearestFinishPos(vFinishes, root.Pos));
	m_PfAHRoot = root.H;
	m_PfABestH.store(root.H, std::memory_order_relaxed);
	root.F = root.G + (float)W * root.H;
	root.ParentIdx = -1;
	root.PrimIdx = -1;
	root.PrimTicks = 0;
	root.TickOffset = 0;
	m_PfABestG[PfAStarKey(root.Core, root.FreezeTime, root.Grounded)] = 0.0f;
	{
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		m_PfANodes.push_back(root);
		m_PfAOpen.push_back(0);
	}

	if(PfIsAtFinishPos(vFinishes, root.Pos))
		m_PfAGoalIdx = 0;

	dbg_msg("pathfinder", "A* start: pos=(%.0f,%.0f) finishes=%d h=%.1f",
		root.Pos.x, root.Pos.y, (int)vFinishes.size(), root.H);
	return true;
}

int CBotNet::PfAStarStep()
{
	if(m_PfAOpen.empty())
	{
		dbg_msg("pathfinder", "A* exhausted: open set empty (no route to a finish tile — blocked by walls/forbidden zones?)");
		return -1;
	}

	int maxNodes = pf_clamp(g_Config.m_KxPfAStarNodeBudget, 1000, 100000000);
	int W = pf_clamp(g_Config.m_KxPfAStarWeight, 1, 20);

	if((int)m_PfANodes.size() > maxNodes)
	{
		dbg_msg("pathfinder", "A* aborted: node budget exhausted (%d nodes)", (int)m_PfANodes.size());
		return -1;
	}

	m_PfAExpandCount++;
	if(m_PfAExpandCount % 4096 == 0)
		dbg_msg("pathfinder", "A* progress: %d expansions, %zu nodes, open=%zu",
			m_PfAExpandCount, m_PfANodes.size(), m_PfAOpen.size());

	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	if(vFinishes.empty())
		return -1;

	int curIdx = PfAStarHeapPop(m_PfAOpen, m_PfANodes);
	AStarNode cur = m_PfANodes[curIdx];

	uint64_t curKey = PfAStarKey(cur.Core, cur.FreezeTime, cur.Grounded);
	auto itCur = m_PfABestG.find(curKey);
	if(itCur == m_PfABestG.end() || itCur->second < cur.G - 0.001f)
		return 0; // stale — a cheaper path to this state exists

	if(PfIsAtFinishPos(vFinishes, cur.Pos))
	{
		if(!PfEffortFreezeSupport() && cur.FreezeChainTicks > 0)
			return 0;
		if(cur.ForbiddenChainTicks > 0)
			return 0;
		m_PfAGoalIdx = curIdx;
		return 1;
	}

	int LocalID = m_PfLocalID;

	CGameWorld FutureWorld;
	PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
	CCollision *pColl = FutureWorld.Collision();
	if(!pColl)
		return 0;

	int RootStartTick = FutureWorld.GameTick();

	// Shared child push. G is pure cost; extraCost carries the stuck
	// penalty. Dedup via BestG with a monotonic G makes this sound.
	auto fnPushChild = [&](const CCharacterCore &endCore, int endFreezeTime,
		const std::vector<CNetObj_PlayerInput> &inputs, const std::vector<vec2> &traj,
		const std::vector<PfHookSeg> &hookSegs, int ticks, float extraCost, int chunkFreezeTicks, int chunkForbiddenTicks, int primIdx) -> bool
	{
		if(!PfEffortFreezeSupport() && cur.FreezeChainTicks + chunkFreezeTicks > 0)
			return false;
		if(cur.ForbiddenChainTicks + chunkForbiddenTicks > 0)
			return false;
		vec2 childPos = endCore.m_Pos;
		bool endGrounded = pColl->IsOnGround(childPos, CCharacterCore::PhysicalSize());
		uint64_t key = PfAStarKey(endCore, endFreezeTime, endGrounded);
		float newG = cur.G + (float)ticks + extraCost;
		auto it = m_PfABestG.find(key);
		if(it != m_PfABestG.end() && it->second <= newG)
			return false;
		m_PfABestG[key] = newG;
		AStarNode child;
		child.Core = endCore;
		child.Pos = childPos;
		child.FreezeTime = endFreezeTime;
		child.Grounded = endGrounded;
		child.G = newG;
		child.H = PfAStarHeuristic(childPos, PfNearestFinishPos(vFinishes, childPos));
		if(child.H < m_PfABestH.load(std::memory_order_relaxed))
			m_PfABestH.store(child.H, std::memory_order_relaxed);
		child.F = child.G + (float)W * child.H;
		child.ParentIdx = curIdx;
		child.PrimIdx = primIdx;
		child.PrimTicks = ticks;
		child.TickOffset = cur.TickOffset + ticks;
		child.FreezeChainTicks = cur.FreezeChainTicks + chunkFreezeTicks;
		child.ForbiddenChainTicks = cur.ForbiddenChainTicks + chunkForbiddenTicks;
		child.Traj = traj;
		child.HookSegs = hookSegs;
		child.Inputs = inputs;
		// m_PfANodes is appended by the worker and read by the renderer —
		// guard the mutation so a concurrent render snapshot never sees a
		// reallocation mid-read.
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		m_PfANodes.push_back(std::move(child));
		PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
		return true;
	};

	// Spawn (or re-spawn after a death) the sim bot at the given state.
	auto fnSpawnBot = [&](const CCharacterCore &core, const vec2 &pos, int freezeTime, int tickOffset) -> CCharacter *
	{
		if(!FutureWorld.GetCharacterById(LocalID))
			PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
		FutureWorld.m_GameTick = RootStartTick + tickOffset;
		CCharacter *pBot = PfSpawnSimBot(&FutureWorld, LocalID, pos, core.m_Vel,
			core.m_HookState, core.m_HookPos, core.m_HookDir, core.m_HookTick,
			freezeTime, core.m_Jumped);
		if(pBot)
		{
			CCharacterCore c = pBot->GetCore();
			c.m_JumpedTotal = core.m_JumpedTotal;
			pBot->SetCore(c);
		}
		return pBot;
	};

	// ── Frozen node: fast-forward through all freeze ticks as one chunk ──
	if(cur.FreezeTime > 0)
	{
		CCharacter *pBot = fnSpawnBot(cur.Core, cur.Pos, cur.FreezeTime, cur.TickOffset);
		if(!pBot)
			return 0;
		int ft = pBot->m_FreezeTime;
		if(ft <= 0)
			ft = 1;
		std::vector<CNetObj_PlayerInput> emptyInputs;
		emptyInputs.resize(ft);
		for(int t = 0; t < ft; t++)
		{
			mem_zero(&emptyInputs[t], sizeof(CNetObj_PlayerInput));
			emptyInputs[t].m_TargetY = -1; // aim up, avoid (0,0) target
		}
		PfChunkResult res;
		PfSimulateChunk(&FutureWorld, LocalID, pBot, emptyInputs, RootStartTick + cur.TickOffset, res,
			m_pForbiddenGrid, PfCustomFinishSimGrid());
		if(res.died && !res.reachedFinish)
			return 0;
		CCharacterCore endCore = cur.Core;
		endCore.m_Pos = res.endPos;
		endCore.m_Vel = res.endVel;
		endCore.m_HookState = res.endHookState;
		endCore.m_HookPos = res.endHookPos;
		endCore.m_HookDir = res.endHookDir;
		endCore.m_HookTick = res.endHookTick;
		endCore.m_Jumped = res.endJumped;
		endCore.m_JumpedTotal = res.endJumpedTotal;
		fnPushChild(endCore, res.endFreezeTime, res.inputs, res.traj, res.hookSegs, ft, 0.0f, ft, res.forbiddenTicks, 99);
		return 0;
	}

	int ChunkSize = PfEffortChunkTicks();
	int HookAngles = PfEffortHookAngles();
	static const int aDirs[] = {-1, 0, 1};

	float hookLength = 380.0f;
	if(CCharacter *pBaseChar = m_PfBaseWorld->GetCharacterById(LocalID))
	{
		float hl = pBaseChar->GetCore().m_Tuning.m_HookLength;
		if(hl > 1.0f && hl < 10000.0f)
			hookLength = hl;
	}

	// Hook ray precompute: fan from -5pi/6 to +5pi/6, filtered by raycast so
	// we only fire hooks that can actually grab something.
	struct HookRayResult
	{
		float angle;
		bool hasTarget;
	};
	HookRayResult aHookRays[32];
	bool hookRaysReady = false;
	if(cur.Core.m_HookState == HOOK_IDLE)
	{
		hookRaysReady = true;
		for(int i = 0; i < HookAngles; i++)
		{
			float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
			float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			aHookRays[i].angle = angle;
			vec2 rayEnd = cur.Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
			vec2 hitPos;
			int hit = pColl->IntersectLineTeleHook(cur.Pos, rayEnd, &hitPos, nullptr);
			if(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH))
				aHookRays[i].hasTarget = true;
			else
				aHookRays[i].hasTarget = false;
		}
	}

	bool hookIdle = (cur.Core.m_HookState == HOOK_IDLE);
	bool hookActive = (cur.Core.m_HookState == HOOK_FLYING || cur.Core.m_HookState == HOOK_GRABBED);

	// =====================================================
	// RHEA — Rolling Horizon Evolutionary Algorithm.
	// Fresh population PER NODE (the old shift buffer carried inputs
	// evolved for a different character state — erratic results).
	// (mu+lambda) evolution, deterministic RNG.
	// =====================================================
	if(g_Config.m_KxPfAdvancedSearch)
	{
		int popSize = PfEffortCandidates();
		int numGenerations = PfEffortGenerations();
		int pushCount = pf_clamp(g_Config.m_KxPfBacktrackCandidates, 1, popSize);

		auto fnAimFor = [&](int angleIdx) -> vec2 {
			float a = aHookRays[angleIdx].angle;
			return vec2(cosf(a), sinf(a)) * 256.0f;
		};
		auto fnAimHooked = [&]() -> vec2 { return cur.Core.m_HookPos - cur.Pos; };

		auto fnRandomInput = [&]() -> CNetObj_PlayerInput {
			CNetObj_PlayerInput inp;
			mem_zero(&inp, sizeof(inp));
			inp.m_Direction = aDirs[PfRheaRand(3)];
			inp.m_Jump = PfRheaRand(2);
			int hookVal = PfRheaRand(2);
			if(hookVal == 1 && !hookIdle && !hookActive)
				hookVal = 0;
			int angleIdx = PfRheaRand(HookAngles);
			if(hookVal == 1 && hookIdle && !aHookRays[angleIdx].hasTarget)
				hookVal = 0;
			inp.m_Hook = hookVal;
			vec2 aim;
			if(hookVal == 1 && hookActive)
				aim = fnAimHooked();
			else if(hookVal == 1 && hookIdle)
				aim = fnAimFor(angleIdx);
			else
				aim = vec2((float)inp.m_Direction * 256.0f, -256.0f);
			inp.m_TargetX = (int)aim.x;
			inp.m_TargetY = (int)aim.y;
			return inp;
		};

		m_PfRHAPopulation.clear();
		m_PfRHAPopulation.resize(popSize);
		m_PfRHAInitialized = false;

		// Seed with constant, sensible sequences so evolution starts from
		// directed movement instead of pure noise.
		{
			auto fnConstantSeq = [&](int dir, int jumpTick) {
				std::vector<CNetObj_PlayerInput> seq(ChunkSize);
				for(int t = 0; t < ChunkSize; t++)
				{
					mem_zero(&seq[t], sizeof(CNetObj_PlayerInput));
					seq[t].m_Direction = dir;
					seq[t].m_Jump = (t == jumpTick) ? 1 : 0;
					seq[t].m_TargetX = dir * 256;
					seq[t].m_TargetY = -256;
				}
				return seq;
			};
			auto fnHookSeq = [&](int dir, int angleIdx) {
				std::vector<CNetObj_PlayerInput> seq(ChunkSize);
				vec2 aim = (angleIdx >= 0) ? fnAimFor(angleIdx) : fnAimHooked();
				for(int t = 0; t < ChunkSize; t++)
				{
					mem_zero(&seq[t], sizeof(CNetObj_PlayerInput));
					seq[t].m_Direction = dir;
					seq[t].m_Hook = 1;
					seq[t].m_TargetX = (int)aim.x;
					seq[t].m_TargetY = (int)aim.y;
				}
				return seq;
			};

			std::vector<std::vector<CNetObj_PlayerInput>> vSeeds;
			vSeeds.push_back(fnConstantSeq(-1, -1));
			vSeeds.push_back(fnConstantSeq(1, -1));
			vSeeds.push_back(fnConstantSeq(0, -1));
			vSeeds.push_back(fnConstantSeq(-1, 0));
			vSeeds.push_back(fnConstantSeq(1, 0));
			vSeeds.push_back(fnConstantSeq(0, 0));
			if(hookActive)
			{
				vSeeds.push_back(fnHookSeq(-1, -1));
				vSeeds.push_back(fnHookSeq(1, -1));
			}
			else if(hookIdle && hookRaysReady)
			{
				std::vector<int> vTargets;
				for(int i = 0; i < HookAngles; i++)
					if(aHookRays[i].hasTarget)
						vTargets.push_back(i);
				if(!vTargets.empty())
				{
					int pick[3] = {vTargets.front(), vTargets[vTargets.size() / 2], vTargets.back()};
					for(int pIdx : pick)
						vSeeds.push_back(fnHookSeq(1, pIdx));
				}
			}

			int numSeeds = pf_min((int)vSeeds.size(), pf_max(1, popSize / 4));
			for(int i = 0; i < numSeeds; i++)
				m_PfRHAPopulation[i].inputs = vSeeds[i];
		}

		for(auto &ind : m_PfRHAPopulation)
		{
			if((int)ind.inputs.size() != ChunkSize)
			{
				ind.inputs.resize(ChunkSize);
				for(int t = 0; t < ChunkSize; t++)
					ind.inputs[t] = fnRandomInput();
			}
			ind.fitness = -1e18f;
		}

		auto fnEvaluate = [&](RheaIndividual &ind) {
			CCharacter *pBot = fnSpawnBot(cur.Core, cur.Pos, cur.FreezeTime, cur.TickOffset);
			if(!pBot)
			{
				ind.fitness = -1e18f;
				ind.died = true;
				ind.reachedFinish = false;
				return;
			}
			PfChunkResult res;
			PfSimulateChunk(&FutureWorld, LocalID, pBot, ind.inputs, RootStartTick + cur.TickOffset, res,
				m_pForbiddenGrid, PfCustomFinishSimGrid());
			ind.endPos = res.endPos;
			ind.endVel = res.endVel;
			ind.endHookState = res.endHookState;
			ind.endHookPos = res.endHookPos;
			ind.endHookDir = res.endHookDir;
			ind.endHookTick = res.endHookTick;
			ind.endJumped = res.endJumped;
			ind.endJumpedTotal = res.endJumpedTotal;
			ind.endFreezeTime = res.endFreezeTime;
			ind.reachedFinish = res.reachedFinish;
			ind.died = res.died && !res.reachedFinish;
			ind.freezeTicks = res.freezeTicks;
			ind.forbiddenTicks = res.forbiddenTicks;
			ind.fitness = PfScoreChunk(cur.Pos, res.endPos, res.reachedFinish, ind.died, res.freezeTicks);
			// Cache traj + hookSegs from THIS simulation — pushing must never
			// re-simulate (the world drifts between sims, v1.56.161 BUG2).
			ind.traj = res.traj;
			ind.hookSegs = res.hookSegs;
		};

		for(auto &ind : m_PfRHAPopulation)
			fnEvaluate(ind);

		std::vector<RheaIndividual> vOffspring;
		for(int gen = 0; gen < numGenerations; gen++)
		{
			std::sort(m_PfRHAPopulation.begin(), m_PfRHAPopulation.end(),
				[](const RheaIndividual &a, const RheaIndividual &b) { return a.fitness > b.fitness; });
			auto fnTournament = [&]() -> int {
				int a = PfRheaRand(popSize / 2 + 1);
				int b = PfRheaRand(popSize / 2 + 1);
				return (m_PfRHAPopulation[a].fitness > m_PfRHAPopulation[b].fitness) ? a : b;
			};
			vOffspring.clear();
			int offspringCount = popSize / 2;
			for(int i = 0; i < offspringCount; i++)
			{
				int p1 = fnTournament();
				int p2 = fnTournament();
				RheaIndividual child;
				child.inputs.resize(ChunkSize);
				int xpoint = PfRheaRand(ChunkSize);
				for(int t = 0; t < xpoint; t++)
					child.inputs[t] = m_PfRHAPopulation[p1].inputs[t];
				for(int t = xpoint; t < ChunkSize; t++)
					child.inputs[t] = m_PfRHAPopulation[p2].inputs[t];
				if(PfRheaChance() < 0.3f)
				{
					int mpoint = 1 + PfRheaRand(pf_max(1, ChunkSize - 1));
					for(int t = mpoint; t < ChunkSize; t++)
						child.inputs[t] = fnRandomInput();
				}
				fnEvaluate(child);
				vOffspring.push_back(std::move(child));
			}
			for(auto &ind : vOffspring)
				m_PfRHAPopulation.push_back(std::move(ind));
			std::sort(m_PfRHAPopulation.begin(), m_PfRHAPopulation.end(),
				[](const RheaIndividual &a, const RheaIndividual &b) { return a.fitness > b.fitness; });
			m_PfRHAPopulation.resize(popSize);
		}

		// Push the best unique survivors as A* children. Dead individuals
		// (forbidden zones / death tiles) are simply never pushed — the open
		// set keeps other branches alive, the search routes around them.
		std::sort(m_PfRHAPopulation.begin(), m_PfRHAPopulation.end(),
			[](const RheaIndividual &a, const RheaIndividual &b) { return a.fitness > b.fitness; });
		int pushed = 0;
		for(const auto &ind : m_PfRHAPopulation)
		{
			if(pushed >= pushCount)
				break;
			if(ind.died && !ind.reachedFinish)
				continue;
			if(ind.inputs.empty())
				continue;
			CCharacterCore endCore = cur.Core;
			endCore.m_Pos = ind.endPos;
			endCore.m_Vel = ind.endVel;
			endCore.m_HookState = ind.endHookState;
			endCore.m_HookPos = ind.endHookPos;
			endCore.m_HookDir = ind.endHookDir;
			endCore.m_HookTick = ind.endHookTick;
			endCore.m_Jumped = ind.endJumped;
			endCore.m_JumpedTotal = ind.endJumpedTotal;
			if(fnPushChild(endCore, ind.endFreezeTime, ind.inputs, ind.traj, ind.hookSegs,
				ChunkSize, PfAStarStuckPenalty(cur.Pos, ind.endPos), ind.freezeTicks, ind.forbiddenTicks, 0))
				pushed++;
		}
		return 0;
	}

	{
		struct ExNode
		{
			CCharacterCore core;
			vec2 pos;
			int freezeTime = 0;
			int tick = 0;
			float score = 0.0f;
			int freezeTicks = 0;
			int forbiddenTicks = 0;
			int parentIdx = -1;
			CNetObj_PlayerInput action;
			std::vector<PfHookSeg> edgeSegs;
			bool reachedFinish = false;
			bool blocked = false;
		};

		int simBudget = PfEffortSearchBudget();
		int pushCount = pf_clamp(g_Config.m_KxPfBacktrackCandidates, 1, 4096);
		int simsUsed = 0;

		std::vector<ExNode> pool;
		pool.push_back(ExNode());
		pool[0].core = cur.Core;
		pool[0].pos = cur.Pos;
		pool[0].freezeTime = cur.FreezeTime;

		auto fnGrounded = [&](const vec2 &pos) -> bool {
			return pColl->IsOnGround(pos, CCharacterCore::PhysicalSize());
		};

		auto fnBuildCandidates = [&](const ExNode &node, CNetObj_PlayerInput *cands) -> int {
			int n = 0;
			bool nodeHookIdle = (node.core.m_HookState == HOOK_IDLE);
			bool nodeHookActive = (node.core.m_HookState == HOOK_FLYING || node.core.m_HookState == HOOK_GRABBED);
			for(int d = 0; d < 3 && n < 128; d++)
			{
				for(int j = 0; j < 2 && n < 128; j++)
				{
					CNetObj_PlayerInput inp;
					mem_zero(&inp, sizeof(inp));
					inp.m_Direction = aDirs[d];
					inp.m_Jump = j;
					inp.m_Hook = 0;
					inp.m_TargetX = aDirs[d] * 256;
					inp.m_TargetY = -256;
					cands[n++] = inp;
				}
			}
			if(nodeHookActive)
			{
				for(int d = 0; d < 3 && n < 128; d++)
				{
					for(int j = 0; j < 2 && n < 128; j++)
					{
						CNetObj_PlayerInput inp;
						mem_zero(&inp, sizeof(inp));
						inp.m_Direction = aDirs[d];
						inp.m_Jump = j;
						inp.m_Hook = 1;
						vec2 aimRel = node.core.m_HookPos - node.pos;
						inp.m_TargetX = (int)aimRel.x;
						inp.m_TargetY = (int)aimRel.y;
						cands[n++] = inp;
					}
				}
			}
			if(nodeHookIdle && hookRaysReady)
			{
				for(int a = 0; a < HookAngles && n < 128; a++)
				{
					if(!aHookRays[a].hasTarget)
						continue;
					for(int d = 0; d < 3 && n < 128; d++)
					{
						CNetObj_PlayerInput inp;
						mem_zero(&inp, sizeof(inp));
						inp.m_Direction = aDirs[d];
						inp.m_Jump = 0;
						inp.m_Hook = 1;
						vec2 aimRel = vec2(cosf(aHookRays[a].angle), sinf(aHookRays[a].angle)) * 256.0f;
						inp.m_TargetX = (int)aimRel.x;
						inp.m_TargetY = (int)aimRel.y;
						cands[n++] = inp;
					}
				}
			}
			return n;
		};

		auto fnStateKey = [&](int idx) -> uint64_t {
			const ExNode &n = pool[idx];
			uint64_t h = 1469598103934665603ull;
			auto mix = [&h](uint64_t v) {
				h ^= v * 1099511628211ull;
				h ^= h >> 27;
			};
			mix((uint64_t)(uint16_t)(int16_t)(int)roundf(n.pos.x / 2.0f));
			mix((uint64_t)(uint16_t)(int16_t)(int)roundf(n.pos.y / 2.0f));
			mix((uint64_t)(uint16_t)(int16_t)(int)roundf(n.core.m_Vel.x));
			mix((uint64_t)(uint16_t)(int16_t)(int)roundf(n.core.m_Vel.y));
			mix((uint64_t)n.core.m_HookState);
			mix((uint64_t)(uint16_t)pf_max(0, n.core.m_HookTick));
			mix((uint64_t)(fnGrounded(n.pos) ? 1u : 0u));
			mix((uint64_t)(uint16_t)pf_max(0, n.freezeTime));
			mix((uint64_t)(uint8_t)(n.core.m_Jumped & 0xF));
			mix((uint64_t)(uint8_t)(n.core.m_JumpedTotal & 0xFF));
			mix((uint64_t)(uint16_t)(int16_t)(int)roundf((n.core.m_HookPos.x - n.pos.x) / 2.0f));
			mix((uint64_t)(uint16_t)(int16_t)(int)roundf((n.core.m_HookPos.y - n.pos.y) / 2.0f));
			return h;
		};

		auto fnSimulate = [&](int parentIdx, const CNetObj_PlayerInput &inp) -> int {
			ExNode par = pool[parentIdx];
			CCharacter *pBot = fnSpawnBot(par.core, par.pos, par.freezeTime, cur.TickOffset + par.tick);
			if(!pBot)
				return -1;
			PfChunkResult res;
			std::vector<CNetObj_PlayerInput> oneInput(1, inp);
			PfSimulateChunk(&FutureWorld, LocalID, pBot, oneInput,
				RootStartTick + cur.TickOffset + par.tick, res,
				m_pForbiddenGrid, PfCustomFinishSimGrid());
			simsUsed++;
			if(res.died && !res.reachedFinish)
				return -1;
			const bool parentBlocked = pool[parentIdx].blocked;
			const bool thisBlocked = res.freezeTicks > 0;
			ExNode ch;
			ch.core = par.core;
			ch.core.m_Pos = res.endPos;
			ch.core.m_Vel = res.endVel;
			ch.core.m_HookState = res.endHookState;
			ch.core.m_HookPos = res.endHookPos;
			ch.core.m_HookDir = res.endHookDir;
			ch.core.m_HookTick = res.endHookTick;
			ch.core.m_Jumped = res.endJumped;
			ch.core.m_JumpedTotal = res.endJumpedTotal;
			ch.pos = res.endPos;
			ch.freezeTime = res.endFreezeTime;
			ch.tick = par.tick + 1;
			ch.score = par.score + PfScoreChunk(par.pos, res.endPos,
				res.reachedFinish, res.died && !res.reachedFinish, par.freezeTicks + res.freezeTicks);
			ch.freezeTicks = par.freezeTicks + res.freezeTicks;
			ch.forbiddenTicks = par.forbiddenTicks + res.forbiddenTicks;
			ch.blocked = parentBlocked || thisBlocked;
			ch.parentIdx = parentIdx;
			ch.action = inp;
			ch.edgeSegs = res.hookSegs;
			ch.reachedFinish = res.reachedFinish;
			pool.push_back(std::move(ch));
			return (int)pool.size() - 1;
		};

		std::vector<int> frontier;
		frontier.push_back(0);
		std::vector<int> vFinished;
		std::vector<int> vDeadEnds;
		bool bFinishFound = false;

		for(int level = 0; level < ChunkSize && !frontier.empty(); level++)
		{
			std::vector<int> kids;
			kids.reserve(frontier.size() * 12);
			int candSum = 0;
			int expandedParents = 0;
			for(size_t pi = 0; pi < frontier.size(); pi++)
			{
				if(simsUsed >= simBudget)
					break;
				ExNode snap = pool[frontier[pi]];
				CNetObj_PlayerInput cands[128];
				int nCands = fnBuildCandidates(snap, cands);
				if(nCands <= 0)
				{
					vDeadEnds.push_back(frontier[pi]);
					continue;
				}
				expandedParents++;
				candSum += nCands;
				for(int ci = 0; ci < nCands; ci++)
				{
					if(simsUsed >= simBudget)
						break;
					int chIdx = fnSimulate(frontier[pi], cands[ci]);
					if(chIdx < 0)
						continue;
					if(pool[chIdx].reachedFinish)
					{
						vFinished.push_back(chIdx);
						bFinishFound = true;
					}
					else
						kids.push_back(chIdx);
				}
			}
			frontier.clear();
			if(bFinishFound || kids.empty())
				break;
			std::sort(kids.begin(), kids.end(),
				[&](int a, int b) { return pool[a].score > pool[b].score; });
			std::unordered_set<uint64_t> seenKeys;
			seenKeys.reserve(kids.size() * 2);
			int width = 4096;
			int levelsLeft = ChunkSize - level - 1;
			if(levelsLeft > 0 && simsUsed < simBudget)
			{
				int avgBranch = pf_max(1, candSum / pf_max(1, expandedParents));
				width = pf_clamp((simBudget - simsUsed) / (levelsLeft * avgBranch), 8, 4096);
			}
			for(size_t ki = 0; ki < kids.size() && (int)frontier.size() < width; ki++)
			{
				uint64_t key = fnStateKey(kids[ki]);
				if(seenKeys.emplace(key).second)
					frontier.push_back(kids[ki]);
			}
		}

		auto fnReconstruct = [&](int idx, std::vector<CNetObj_PlayerInput> &inputsOut,
			std::vector<vec2> &trajOut, std::vector<PfHookSeg> &segsOut) {
			inputsOut.clear();
			trajOut.clear();
			segsOut.clear();
			std::vector<int> rev;
			for(int i = idx; i != -1; i = pool[i].parentIdx)
				rev.push_back(i);
			std::reverse(rev.begin(), rev.end());
			trajOut.push_back(pool[rev[0]].pos);
			for(size_t k = 1; k < rev.size(); k++)
			{
				inputsOut.push_back(pool[rev[k]].action);
				trajOut.push_back(pool[rev[k]].pos);
				for(const auto &hs : pool[rev[k]].edgeSegs)
					segsOut.push_back(hs);
			}
		};

		std::vector<int> vResults;
		vResults.insert(vResults.end(), vFinished.begin(), vFinished.end());
		vResults.insert(vResults.end(), vDeadEnds.begin(), vDeadEnds.end());
		vResults.insert(vResults.end(), frontier.begin(), frontier.end());
		const bool penOn = PfEffortFineFreeze();
		std::sort(vResults.begin(), vResults.end(), [&](int a, int b) {
			if(penOn && pool[a].blocked != pool[b].blocked)
				return pool[b].blocked;
			return pool[a].score > pool[b].score;
		});

	int pushed = 0;
	for(size_t ri = 0; ri < vResults.size() && pushed < pushCount; ri++)
	{
		int idx = vResults[ri];
		std::vector<CNetObj_PlayerInput> inputs;
		std::vector<vec2> traj;
		std::vector<PfHookSeg> segs;
		fnReconstruct(idx, inputs, traj, segs);
		if(inputs.empty())
			continue;
		if(fnPushChild(pool[idx].core, pool[idx].freezeTime, inputs, traj, segs,
			pf_max(1, (int)inputs.size()), PfAStarStuckPenalty(cur.Pos, pool[idx].pos), pool[idx].freezeTicks, pool[idx].forbiddenTicks, 0))
			pushed++;
	}
	}

	return 0;
}

// Build the visual path + hook segments from the cached per-node
// trajectories along the parent chain (no re-simulation).
void CBotNet::PfAStarResimulatePath(int targetIdx)
{
	if(targetIdx < 0 || targetIdx >= (int)m_PfANodes.size())
		return;

	std::vector<int> vPathIdx;
	for(int idx = targetIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
		vPathIdx.push_back(idx);
	std::reverse(vPathIdx.begin(), vPathIdx.end());

	std::vector<vec2> newPath;
	std::vector<PfHookSeg> newHookSegs;
	newPath.push_back(m_PfANodes[vPathIdx[0]].Pos);
	for(size_t i = 1; i < vPathIdx.size(); i++)
	{
		AStarNode &node = m_PfANodes[vPathIdx[i]];
		for(size_t k = 1; k < node.Traj.size(); k++)
			newPath.push_back(node.Traj[k]);
		for(const auto &seg : node.HookSegs)
			newHookSegs.push_back(seg);
	}

	std::lock_guard<std::mutex> lock(m_PfDataMutex);
	m_PfVPath = std::move(newPath);
	m_PfVHookSegs = std::move(newHookSegs);
}

// Live preview while searching: show the path to the open node closest
// to a finish (lowest H). Throttled by the caller.
void CBotNet::PfAStarUpdatePreview()
{
	if(m_PfANodes.empty() || m_PfAOpen.empty())
		return;

	int bestIdx = -1;
	float bestH = 1e18f;
	for(int idx : m_PfAOpen)
	{
		if(idx < 0 || idx >= (int)m_PfANodes.size())
			continue;
		if(m_PfANodes[idx].H < bestH)
		{
			bestH = m_PfANodes[idx].H;
			bestIdx = idx;
		}
	}
	if(bestIdx < 0)
		return;

	PfAStarResimulatePath(bestIdx);
}

// Final reconstruction: full input package + trajectory from the root to
// the goal node. Stops the worker first — it is called from the main
// thread after the search result arrives (or from the Stop button).
void CBotNet::PfAStarReconstruct()
{
	PfThreadStop();
	if(m_PfAGoalIdx < 0 || m_PfAGoalIdx >= (int)m_PfANodes.size())
		return;
	if(!PfEffortFreezeSupport() && m_PfANodes[m_PfAGoalIdx].FreezeChainTicks > 0)
	{
		dbg_msg("pathfinder", "reconstruction rejected: goal node carries %d freeze ticks", m_PfANodes[m_PfAGoalIdx].FreezeChainTicks);
		m_PfAGoalIdx = -1;
		return;
	}
	if(m_PfANodes[m_PfAGoalIdx].ForbiddenChainTicks > 0)
	{
		dbg_msg("pathfinder", "reconstruction rejected: goal node carries %d forbidden ticks", m_PfANodes[m_PfAGoalIdx].ForbiddenChainTicks);
		m_PfAGoalIdx = -1;
		return;
	}

	PfAStarResimulatePath(m_PfAGoalIdx);

	std::vector<int> vPathIdx;
	for(int idx = m_PfAGoalIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
		vPathIdx.push_back(idx);
	std::reverse(vPathIdx.begin(), vPathIdx.end());

	m_PfFullInputs.clear();
	for(size_t i = 1; i < vPathIdx.size(); i++)
	{
		AStarNode &node = m_PfANodes[vPathIdx[i]];
		for(size_t k = 0; k < node.Inputs.size(); k++)
			m_PfFullInputs.push_back(node.Inputs[k]);
	}

	m_PfFullInputsIdx = 0;

	if(!vPathIdx.empty())
	{
		AStarNode &last = m_PfANodes[vPathIdx.back()];
		m_PfCurPos = last.Pos;
		m_PfCurVel = last.Core.m_Vel;
		m_PfCurHookState = last.Core.m_HookState;
		m_PfCurHookPos = last.Core.m_HookPos;
		m_PfCurHookDir = last.Core.m_HookDir;
		m_PfCurHookTick = last.Core.m_HookTick;
		m_PfCurJumped = last.Core.m_Jumped;
		m_PfCurFreezeTime = last.FreezeTime;
	}
}

// =========================================================
// Worker thread
// =========================================================

void CBotNet::PfThreadWorker()
{
	if(!PfAStarInit())
	{
		dbg_msg("pathfinder", "A* init failed (no world snapshot, character or finish tiles)");
		m_PfThreadResult.store(-1);
		m_PfThreadRunning.store(false);
		return;
	}

	if(m_PfAGoalIdx >= 0)
	{
		m_PfThreadResult.store(1);
		m_PfThreadRunning.store(false);
		return;
	}

	while(!m_PfThreadCancel.load(std::memory_order_relaxed))
	{
		int result = PfAStarStep();

		// Live preview: often early (so the first chunk shows up fast),
		// then throttled — the full path copy under the mutex is not free.
		if(m_PfAExpandCount <= 8 || m_PfAExpandCount % 64 == 0)
			PfAStarUpdatePreview();

		if(result == 1)
		{
			dbg_msg("pathfinder", "A* goal reached: %d expansions, %zu nodes",
				m_PfAExpandCount, m_PfANodes.size());
			m_PfThreadResult.store(1);
			m_PfThreadRunning.store(false);
			return;
		}
		if(result < 0)
		{
			m_PfThreadResult.store(-1);
			m_PfThreadRunning.store(false);
			return;
		}
	}

	m_PfThreadResult.store(-1);
	m_PfThreadRunning.store(false);
}

void CBotNet::PfThreadStart()
{
	PfThreadStop();
	m_PfThreadResult.store(0);
	m_PfThreadCancel.store(false);
	m_PfThreadRunning.store(true);
	m_PfThread = std::thread(&CBotNet::PfThreadWorker, this);
}

void CBotNet::PfThreadStop()
{
	if(!m_PfThread.joinable())
		return;
	m_PfThreadCancel.store(true);
	m_PfThread.join();
	m_PfThreadRunning.store(false);
	m_PfThreadCancel.store(false);
}
