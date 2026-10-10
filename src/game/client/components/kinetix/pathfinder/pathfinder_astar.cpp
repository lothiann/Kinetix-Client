#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <algorithm>

// =========================================================
// PHYSICS A* — MACRO: chunked-primitive search (v1.56.246).
//
// Replaces the per-tick expansion (v1.56.245). A* over simulated
// character states, but the tree is built over MACRO-ACTIONS
// (chunks) simulated with the real CCharacter physics:
//   - run chunk: hold (dir, jump); commits at jump apex, ledge
//     walk-off, landing, an early tick (short walk / brake /
//     mid-rise hook window) or the tick cap;
//   - hook chunk: fire a ray-validated hook angle (or continue an
//     active hook) with a steering dir, hold while grabbed, release
//     when close to the hook point (pull), slow inside the no-drag
//     zone, or swinging fast toward the steer side (pendulum), then
//     ballistic until grounded / coast apex / cap.
// G stays pure simulated ticks; dedup and the flow-field heuristic
// are unchanged, so the first finish popped is the fastest path the
// chunk tree can express. Freeze contact and forbidden zones kill a
// chunk. Port of the validated Python reference (macro.py): 2 knobs
// total — the A* weight slider (KxPfAStarWeight) and the pump
// steered-hook variants, which are fixed ON here.
// =========================================================

// State dedup key: quantized position (2px), velocity (3px), hook state,
// hook tick, grounded, frozen time, jump flags, hook offset.
static uint64_t PfAStarKey(const CBotNet::PfAState &core, int freezeTime, bool grounded)
{
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](uint64_t v) {
		h ^= v * 1099511628211ull;
		h ^= h >> 27;
	};
	mix((uint64_t)(uint16_t)(int16_t)(int)roundf(core.Pos.x / 2.0f));
	mix((uint64_t)(uint16_t)(int16_t)(int)roundf(core.Pos.y / 2.0f));
	mix((uint64_t)(uint16_t)(int16_t)(int)roundf(core.Vel.x / 3.0f));
	mix((uint64_t)(uint16_t)(int16_t)(int)roundf(core.Vel.y / 3.0f));
	mix((uint64_t)(core.HookState + 8));
	mix((uint64_t)(uint16_t)pf_max(0, core.HookTick));
	mix((uint64_t)(grounded ? 1u : 0u));
	mix((uint64_t)(uint16_t)pf_max(0, freezeTime / 2));
	mix((uint64_t)(uint8_t)(core.Jumped & 0xF));
	mix((uint64_t)(uint8_t)(core.JumpedTotal & 0xFF));
	mix((uint64_t)(uint16_t)(int16_t)(int)roundf((core.HookPos.x - core.Pos.x) / 4.0f));
	mix((uint64_t)(uint16_t)(int16_t)(int)roundf((core.HookPos.y - core.Pos.y) / 4.0f));
	return h;
}

static inline bool PfAHeapLess(const CBotNet::AStarNode &a, const CBotNet::AStarNode &b)
{
	if(a.F != b.F)
		return a.F < b.F;
	return a.H < b.H;
}

static void PfAStarHeapPush(std::vector<int> &heap, const std::vector<CBotNet::AStarNode> &nodes, int idx)
{
	heap.push_back(idx);
	int i = (int)heap.size() - 1;
	while(i > 0)
	{
		int parent = (i - 1) / 2;
		if(!PfAHeapLess(nodes[heap[i]], nodes[heap[parent]]))
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
		if(l < n && PfAHeapLess(nodes[heap[l]], nodes[heap[s]]))
			s = l;
		if(r < n && PfAHeapLess(nodes[heap[r]], nodes[heap[s]]))
			s = r;
		if(s == i)
			break;
		std::swap(heap[s], heap[i]);
		i = s;
	}
	return top;
}

// =========================================================
// MACRO chunk primitives (v1.56.246). A chunk is a macro-action
// simulated tick-by-tick with the real CCharacter physics:
//
//   run chunk  — hold (dir, jump); commits at jump apex, ledge
//                walk-off, landing, an early tick (short walk /
//                brake / mid-rise hook window) or the tick cap;
//   hook chunk — fire a ray-validated hook angle (or continue an
//                active hook) with a steering dir, hold while
//                grabbed, release when close to the hook point
//                (pull), slow inside the no-drag zone, or swinging
//                fast toward the steer side (pendulum), then
//                ballistic until grounded / coast apex / cap.
//
// Freeze contact, a forbidden-zone touch or engine death kills the
// chunk (1:1 with the validated Python reference macro.py).
// =========================================================

static const int MACRO_RUN_CAP = 28;           // max ticks for a run/jump chunk
static const int MACRO_HOOK_CAP = 64;          // max ticks for a hook chunk
static const int MACRO_RELEASE_CAP = 28;       // max ballistic ticks after release
static const float MACRO_GRAB_DIST2 = 1600.0f; // 40px squared: unconditional pull release
static const float MACRO_SWING_VEL = 3.0f;     // pendulum release: vel.x * dir above this
static const int MACRO_MIN_GRAB_TICKS = 4;     // min grabbed ticks before a swing release
static const int MACRO_EARLY_TICKS = 8;        // early-commit tick for run chunks

struct PfMacroCand
{
	int kind;     // 0 = run, 1 = hook fire, 2 = hook continue
	int jump;     // run: jump flag held through the chunk
	int angleIdx; // hook fire: fan index
	int dir;      // steering direction
	int early;    // run: early-commit tick (0 = none)
};

struct PfMacroChunk
{
	std::vector<CNetObj_PlayerInput> inputs;
	std::vector<vec2> traj;
	std::vector<PfHookSeg> hookSegs;
	int ticks = 0;
	CBotNet::PfAState end{};
	int endFreezeTime = 0;
	bool hitFinish = false;
};

static bool PfMacroRunChunk(CGameWorld *pWorld, int LocalID, CCharacter *pBot,
	const CBotNet::PfAState &startCore, bool startedGrounded, const PfMacroCand &cand,
	int HookAngles, int StartTick,
	const unsigned char *pForbiddenGrid, const unsigned char *pCustomFinishGrid,
	PfMacroChunk &out)
{
	CCollision *pColl = pWorld->Collision();
	if(!pColl || !pBot)
		return false;
	const int cw = pColl->GetWidth();
	const int chh = pColl->GetHeight();
	const int cap = (cand.kind == 0) ? MACRO_RUN_CAP : MACRO_HOOK_CAP;

	float fireAngle = 0.0f;
	if(cand.kind == 1)
	{
		float fa = (float)cand.angleIdx / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
		fireAngle = (-5.0f / 6.0f) * pi + fa * (10.0f / 6.0f) * pi;
	}

	out.inputs.clear();
	out.traj.clear();
	out.traj.push_back(pBot->m_Pos);
	out.hookSegs.clear();
	out.ticks = 0;
	out.hitFinish = false;
	out.endFreezeTime = 0;

	int prevHookState = startCore.HookState;
	int grabT = 0;
	int releaseT = -1;
	int stuck = 0;
	vec2 prevSimPos = pBot->m_Pos;
	bool grounded = false;
	int t = 0;
	while(t < cap)
	{
		t++;
		CNetObj_PlayerInput in;
		mem_zero(&in, sizeof(in));
		if(cand.kind == 0)
		{
			in.m_Direction = cand.dir;
			in.m_Jump = cand.jump;
			in.m_TargetX = cand.dir * 256;
			in.m_TargetY = -256;
		}
		else if(releaseT >= 0)
		{
			in.m_Direction = cand.dir;
			in.m_TargetX = cand.dir * 256;
			in.m_TargetY = -256;
			releaseT++;
		}
		else
		{
			in.m_Hook = 1;
			in.m_Direction = cand.dir;
			if(t == 1 && cand.kind == 1)
			{
				in.m_TargetX = (int)(cosf(fireAngle) * 256.0f);
				in.m_TargetY = (int)(sinf(fireAngle) * 256.0f);
			}
			else
			{
				CCharacterCore hc = pBot->GetCore();
				in.m_TargetX = (int)(hc.m_HookPos.x - hc.m_Pos.x);
				in.m_TargetY = (int)(hc.m_HookPos.y - hc.m_Pos.y);
			}
		}
		out.inputs.push_back(in);

		pBot->OnDirectInput(&in);
		pWorld->m_GameTick = StartTick + t;
		pBot->OnPredictedInput(&in);
		pWorld->Tick();
		if(!pWorld->GetCharacterById(LocalID))
			return false; // engine death / out of world

		if(pBot->m_FreezeTime <= 0)
			PfCheckFreeze(pWorld, pBot, prevSimPos, pBot->m_Pos);
		if(pBot->m_FreezeTime > 0)
			return false; // freeze contact kills the chunk
		if(PfLineTouchesForbidden(pForbiddenGrid, cw, chh, prevSimPos, pBot->m_Pos))
			return false;
		prevSimPos = pBot->m_Pos;

		out.traj.push_back(pBot->m_Pos);

		CCharacterCore c = pBot->GetCore();
		if(c.m_HookState == HOOK_GRABBED && prevHookState != HOOK_GRABBED)
			out.hookSegs.push_back({pBot->m_Pos, c.m_HookPos});

		// finish contact truncates the chunk right here
		{
			int tc = round_to_int(c.m_Pos.x / 32.0f);
			int tr = round_to_int(c.m_Pos.y / 32.0f);
			if(tc >= 0 && tr >= 0 && tc < cw && tr < chh)
			{
				int MapIndex = tr * cw + tc;
				bool fin;
				if(pCustomFinishGrid)
					fin = pCustomFinishGrid[MapIndex] != 0;
				else
				{
					int Tile = pColl->GetTileIndex(MapIndex);
					int FTile = pColl->GetFrontTileIndex(MapIndex);
					fin = (Tile == TILE_FINISH || FTile == TILE_FINISH);
				}
				if(fin)
				{
					out.hitFinish = true;
					break;
				}
			}
		}

		grounded = pColl->IsOnGround(pBot->m_Pos, CCharacterCore::PhysicalSize());
		const int hs = c.m_HookState;

		if(cand.kind == 0)
		{
			if(cand.early > 0 && t >= cand.early)
				break; // early commit: short walk / brake / mid-rise hook window
			if(cand.jump == 0 && grounded && fabsf(c.m_Vel.x) < 0.1f)
			{
				stuck++;
				if(stuck >= 10)
					return false; // pressed into a wall with no jump: dead end
			}
			else
				stuck = 0;
			if(startedGrounded)
			{
				if(!grounded && c.m_Vel.y >= 0.0f)
					break; // ledge walk-off or jump apex: decision point
			}
			else
			{
				if(grounded)
					break; // landed
			}
		}
		else
		{
			if(releaseT >= 0)
			{
				if(grounded || c.m_Vel.y >= 0.0f || releaseT >= MACRO_RELEASE_CAP)
					break; // grounded / coast apex / cap
			}
			else if(hs == HOOK_GRABBED)
			{
				grabT++;
				const float dxh = c.m_HookPos.x - c.m_Pos.x;
				const float dyh = c.m_HookPos.y - c.m_Pos.y;
				const float d2h = dxh * dxh + dyh * dyh;
				const float sp2 = c.m_Vel.x * c.m_Vel.x + c.m_Vel.y * c.m_Vel.y;
				if(d2h < MACRO_GRAB_DIST2)
					releaseT = 0; // pulled in: release, go ballistic
				else if(d2h < 48.0f * 48.0f && sp2 < 4.0f)
					releaseT = 0; // slow inside the no-drag zone: pull is done
				else if(cand.dir != 0 && grabT >= MACRO_MIN_GRAB_TICKS && c.m_Vel.x * cand.dir > MACRO_SWING_VEL)
					releaseT = 0; // pendulum release on the forward swing
				else if(grounded && grabT >= 12 && sp2 < 0.25f)
					return false; // pinned on the ground by a hook: dead end
			}
			else if(hs == HOOK_IDLE)
			{
				if(prevHookState == HOOK_FLYING)
					return false; // fired and missed
				if(prevHookState == HOOK_GRABBED)
					releaseT = 0; // hook timed out, go ballistic
				else if(t == 1)
					return false; // fire produced nothing
			}
		}
		prevHookState = hs;
	}

	CCharacterCore c = pBot->GetCore();
	out.end = startCore;
	out.end.Pos = c.m_Pos;
	out.end.Vel = c.m_Vel;
	out.end.HookState = c.m_HookState;
	out.end.HookPos = c.m_HookPos;
	out.end.HookDir = c.m_HookDir;
	out.end.HookTick = c.m_HookTick;
	out.end.Jumped = c.m_Jumped;
	out.end.JumpedTotal = c.m_JumpedTotal;
	out.endFreezeTime = pBot->m_FreezeTime;
	out.ticks = t;
	return true;
}

int PfEffortHookAngles()
{
	// v1.56.283: kx_pf_hook_angles removed - the hook fan is always 36 rays.
	return 36;
}

static int PfEffortIndex()
{
	return g_Config.m_KxPfEffort < 0 ? 0 : (g_Config.m_KxPfEffort > 5 ? 5 : g_Config.m_KxPfEffort);
}

int PfEffortBeamWidth()
{
	if(g_Config.m_KxPfEffort == 6)
	{
		int v = g_Config.m_KxPfBeamWidth;
		return v < 8 ? 8 : (v > 4096 ? 4096 : v);
	}
	static const int s_aBeamWidths[] = {2048, 2048, 4096, 4096, 4096, 4096};
	return s_aBeamWidths[PfEffortIndex()];
}

int PfEffortAStarWeight()
{
	if(g_Config.m_KxPfEffort == 6)
	{
		int v = g_Config.m_KxPfAStarWeight;
		return v < 100 ? 100 : (v > 2000 ? 2000 : v);
	}
	static const int s_aWeights[] = {200, 150, 130, 120, 110, 100};
	return s_aWeights[PfEffortIndex()];
}

int PfEffortRheaPop()
{
	static const int s_aPops[] = {12, 12, 24, 48, 96, 128};
	return s_aPops[PfEffortIndex()];
}

int PfEffortRheaGens()
{
	if(g_Config.m_KxPfEffort == 6)
	{
		int v = g_Config.m_KxPfRheaGenerations;
		return v < 1 ? 1 : (v > 100 ? 100 : v);
	}
	static const int s_aGens[] = {5, 5, 10, 10, 10, 15};
	return s_aGens[PfEffortIndex()];
}

int PfEffortMctsIters()
{
	static const int s_aIters[] = {300, 300, 600, 1200, 2400, 4800};
	return s_aIters[PfEffortIndex()];
}

bool PfEffortScoreDist() { return true; }
bool PfEffortScoreFlow() { return true; }
bool PfEffortFreezeSupport() { return g_Config.m_KxPfEffort == 6 ? g_Config.m_KxPfFreezeSupport != 0 : false; }
bool PfEffortFineDeath() { return true; }

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

static CCharacter *PfSimWorldBot(CGameWorld *pSimWorld, CGameWorld *pBaseWorld, int LocalID)
{
	if(CCharacter *pBot = pSimWorld->GetCharacterById(LocalID))
		return pBot;
	CCharacter *pBase = pBaseWorld->GetCharacterById(LocalID);
	if(!pBase)
		return nullptr;
	if(CCharacter *pDead = (CCharacter *)pSimWorld->FindFirst(CGameWorld::ENTTYPE_CHARACTER))
		delete pDead;
	CCharacter *pCopy = new CCharacter(*pBase);
	pCopy->m_pParent = pBase;
	pBase->m_pChild = pCopy;
	pSimWorld->InsertEntity(pCopy);
	return pCopy;
}

static int PfBuildTickCandidates(const CBotNet::PfAState &core, CCollision *pColl, float hookLength, int HookAngles,
	CNetObj_PlayerInput aOut[128], int aCodes[128])
{
	int n = 0;
	static const int aDirs[] = {-1, 0, 1};
	auto add = [&](int dir, int jump, int hook, int angleIdx, int tx, int ty) {
		if(n >= 128)
			return;
		if(aCodes)
			aCodes[n] = (2 + dir) | (jump << 2) | (hook << 3) | (angleIdx << 5) | ((HookAngles - 1) << 11);
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
			add(aDirs[d], j, 0, 0, aDirs[d] * 256, -256);
	}
	const bool hookIdle = (core.HookState == HOOK_IDLE);
	const bool hookActive = (core.HookState == HOOK_FLYING || core.HookState == HOOK_GRABBED);
	if(hookActive)
	{
		int tx = (int)(core.HookPos.x - core.Pos.x);
		int ty = (int)(core.HookPos.y - core.Pos.y);
		for(int d = 0; d < 3; d++)
		{
			for(int j = 0; j < 2; j++)
				add(aDirs[d], j, 1, 0, tx, ty);
		}
	}
	if(hookIdle)
	{
		for(int a = 0; a < HookAngles; a++)
		{
			float t = (float)a / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
			float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			vec2 rayEnd = core.Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
			vec2 hitPos;
			int hit = pColl->IntersectLineTeleHook(core.Pos, rayEnd, &hitPos, nullptr);
			if(!(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH)))
				continue;
			int tx = (int)(cosf(angle) * 256.0f);
			int ty = (int)(sinf(angle) * 256.0f);
			for(int d = 0; d < 3; d++)
				add(aDirs[d], 0, 2, a, tx, ty);
		}
	}
	return n;
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

	delete m_PfSimWorld;
	m_PfSimWorld = new CGameWorld();
	PfAStarSetupWorld(m_PfSimWorld, m_PfBaseWorld, LocalID);
	if(!m_PfSimWorld->Collision())
	{
		delete m_PfSimWorld;
		m_PfSimWorld = nullptr;
		return false;
	}
	m_PfSimStartTick = m_PfSimWorld->GameTick();

	m_PfANodes.clear();
	m_PfANodes.reserve(4096);
	m_PfAOpen.clear();
	m_PfAOpen.reserve(4096);
	m_PfABeamReserve.clear();
	m_PfABestG.clear();
	m_PfABestG.reserve(1 << 14);
	m_PfAGoalIdx = -1;
	m_PfAExpandCount = 0;

	float W = PfEffortAStarWeight() / 100.0f;
if(W < 1.0f)
	W = 1.0f;
if(W > 20.0f)
	W = 20.0f;

	AStarNode root;
	root.Core.Pos = m_PfCurPos;
	root.Core.Vel = m_PfCurVel;
	root.Core.HookState = m_PfCurHookState;
	root.Core.HookPos = m_PfCurHookPos;
	root.Core.HookDir = m_PfCurHookDir;
	root.Core.HookTick = m_PfCurHookTick;
	root.Core.Jumped = m_PfCurJumped;
	root.Core.JumpedTotal = pChar->GetCore().m_JumpedTotal;
	root.FreezeTime = m_PfCurFreezeTime;
	CCollision *pColl = m_PfBaseWorld->Collision();
	root.Grounded = pColl ? pColl->IsOnGround(root.Core.Pos, CCharacterCore::PhysicalSize()) : false;
	root.G = 0.0f;
	root.H = PfAStarHeuristic(root.Core.Pos, PfNearestFinishPos(vFinishes, root.Core.Pos));
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

	if(PfIsAtFinishPos(vFinishes, root.Core.Pos))
		m_PfAGoalIdx = 0;

	dbg_msg("pathfinder", "A* start: pos=(%.0f,%.0f) finishes=%d h=%.1f",
		root.Core.Pos.x, root.Core.Pos.y, (int)vFinishes.size(), root.H);
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
	float W = PfEffortAStarWeight() / 100.0f;
if(W < 1.0f)
	W = 1.0f;
if(W > 20.0f)
	W = 20.0f;

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

	if(PfIsAtFinishPos(vFinishes, cur.Core.Pos))
	{
		if(!PfEffortFreezeSupport() && cur.FreezeChainTicks > 0)
			return 0;
		if(cur.ForbiddenChainTicks > 0)
			return 0;
		m_PfAGoalIdx = curIdx;
		return 1;
	}

	int LocalID = m_PfLocalID;

	if(!m_PfSimWorld)
		return 0;
	m_PfSimWorld->m_Teams = m_PfBaseWorld->m_Teams;
	m_PfSimWorld->m_Core.m_vSwitchers = m_PfBaseWorld->m_Core.m_vSwitchers;
	m_PfSimWorld->m_PredictedEvents.clear();
	CCollision *pColl = m_PfSimWorld->Collision();
	if(!pColl)
		return 0;

	int RootStartTick = m_PfSimStartTick;

	// Shared child push. G is pure simulated ticks — nothing else leaks
	// into the cost. Dedup via BestG with a monotonic G keeps the search
	// loop-free; the open set does all the pruning.
	std::vector<AStarNode> vPending;
	auto fnPushChild = [&](const PfAState &endCore, int endFreezeTime,
		const std::vector<CNetObj_PlayerInput> &inputs, const std::vector<vec2> &traj,
		const std::vector<PfHookSeg> &hookSegs, int ticks, int chunkFreezeTicks, int chunkForbiddenTicks, int primIdx) -> bool
	{
		if(!PfEffortFreezeSupport() && cur.FreezeChainTicks + chunkFreezeTicks > 0)
			return false;
		if(cur.ForbiddenChainTicks + chunkForbiddenTicks > 0)
			return false;
		vec2 childPos = endCore.Pos;
		bool endGrounded = pColl->IsOnGround(childPos, CCharacterCore::PhysicalSize());
		uint64_t key = PfAStarKey(endCore, endFreezeTime, endGrounded);
		float newG = cur.G + (float)ticks;
		auto it = m_PfABestG.find(key);
		if(it != m_PfABestG.end() && it->second <= newG)
			return false;
		m_PfABestG[key] = newG;
		AStarNode child;
		child.Core = endCore;
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
		vPending.push_back(std::move(child));
		return true;
	};
	// m_PfANodes is appended by the worker and read by the renderer —
	// guard the mutation so a concurrent render snapshot never sees a
	// reallocation mid-read.
	auto fnFlushPending = [&]() {
		if(vPending.empty())
			return;
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		for(AStarNode &pending : vPending)
		{
			m_PfANodes.push_back(std::move(pending));
			PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
		}
		vPending.clear();
	};

	// Spawn (or re-spawn after a death) the sim bot at the given state.
	auto fnSpawnBot = [&](const PfAState &core, const vec2 &pos, int freezeTime, int tickOffset) -> CCharacter *
	{
		if(!PfSimWorldBot(m_PfSimWorld, m_PfBaseWorld, LocalID))
			return nullptr;
		m_PfSimWorld->m_GameTick = RootStartTick + tickOffset;
		CCharacter *pBot = PfSpawnSimBot(m_PfSimWorld, LocalID, pos, core.Vel,
			core.HookState, core.HookPos, core.HookDir, core.HookTick,
			freezeTime, core.Jumped);
		if(pBot)
		{
			CCharacterCore c = pBot->GetCore();
			c.m_JumpedTotal = core.JumpedTotal;
			pBot->SetCore(c);
		}
		return pBot;
	};

	// ── Frozen node: fast-forward through all freeze ticks as one chunk ──
	if(cur.FreezeTime > 0)
	{
		CCharacter *pBot = fnSpawnBot(cur.Core, cur.Core.Pos, cur.FreezeTime, cur.TickOffset);
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
		PfSimulateChunk(m_PfSimWorld, LocalID, pBot, emptyInputs, RootStartTick + cur.TickOffset, res,
			m_pForbiddenGrid, PfCustomFinishSimGrid());
		if(res.died && !res.reachedFinish)
			return 0;
		PfAState endCore = cur.Core;
		endCore.Pos = res.endPos;
		endCore.Vel = res.endVel;
		endCore.HookState = res.endHookState;
		endCore.HookPos = res.endHookPos;
		endCore.HookDir = res.endHookDir;
		endCore.HookTick = res.endHookTick;
		endCore.Jumped = res.endJumped;
		endCore.JumpedTotal = res.endJumpedTotal;
			fnPushChild(endCore, res.endFreezeTime, res.inputs, res.traj, res.hookSegs, ft, ft, res.forbiddenTicks, 0);
			fnFlushPending();
			return 0;
	}

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
	HookRayResult aHookRays[64]; // v1.56.283: 36-ray fan (was 32)
	bool hookRaysReady = false;
	int HookAngles = PfEffortHookAngles();
	if(cur.Core.HookState == HOOK_IDLE)
	{
		hookRaysReady = true;
		for(int i = 0; i < HookAngles; i++)
		{
			float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
			float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			aHookRays[i].angle = angle;
			vec2 rayEnd = cur.Core.Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
			vec2 hitPos;
			int hit = pColl->IntersectLineTeleHook(cur.Core.Pos, rayEnd, &hitPos, nullptr);
			if(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH))
				aHookRays[i].hasTarget = true;
			else
				aHookRays[i].hasTarget = false;
		}
	}

	bool hookIdle = (cur.Core.HookState == HOOK_IDLE);
	bool hookActive = (cur.Core.HookState == HOOK_FLYING || cur.Core.HookState == HOOK_GRABBED);

	// ── MACRO candidate list ──
	// grounded: run chunks (dir × jump) in full + early variants;
	// air: run chunks (dir, no jump) in full + early variants (turn fix);
	// hook idle: fire each ray-valid angle unsteered (+ steered pump
	// variants); hook active: continue the hook unsteered (+ steered).
	static const int aRunGrounded[][2] = {{-1, 0}, {1, 0}, {-1, 1}, {1, 1}, {0, 1}, {0, 0}};
	static const int aRunAir[][2] = {{-1, 0}, {1, 0}, {0, 0}};
	PfMacroCand aCands[128];
	int nCands = 0;
	auto fnAddCand = [&](int kind, int jump, int angleIdx, int dir, int early) {
		if(nCands >= 128)
			return;
		aCands[nCands].kind = kind;
		aCands[nCands].jump = jump;
		aCands[nCands].angleIdx = angleIdx;
		aCands[nCands].dir = dir;
		aCands[nCands].early = early;
		nCands++;
	};
	if(cur.Grounded)
	{
		for(const auto &rd : aRunGrounded)
		{
			if(rd[0] == 0 && rd[1] == 0)
				fnAddCand(0, rd[1], 0, rd[0], MACRO_EARLY_TICKS); // idle: short-wait chunk only
			else
			{
				fnAddCand(0, rd[1], 0, rd[0], 0);
				fnAddCand(0, rd[1], 0, rd[0], 4); // tight-turn commit
				fnAddCand(0, rd[1], 0, rd[0], MACRO_EARLY_TICKS);
			}
		}
	}
	else
	{
		for(const auto &rd : aRunAir)
		{
			fnAddCand(0, rd[1], 0, rd[0], 0);
			fnAddCand(0, rd[1], 0, rd[0], 4); // mid-air turn control (the wide-arc fix)
			fnAddCand(0, rd[1], 0, rd[0], MACRO_EARLY_TICKS);
		}
	}
	if(hookIdle && hookRaysReady)
	{
		for(int a = 0; a < HookAngles; a++)
		{
			if(!aHookRays[a].hasTarget)
				continue;
			fnAddCand(1, 0, a, 0, 0);
			// pump: steered fire variants (fixed on — part of the algorithm)
			const float ax = cosf(aHookRays[a].angle);
			const float ay = sinf(aHookRays[a].angle);
			if(ay < 0.0f)
			{
				if(ax != 0.0f)
				{
					fnAddCand(1, 0, a, 1, 0);
					fnAddCand(1, 0, a, -1, 0);
				}
			}
			else if(ax != 0.0f)
				fnAddCand(1, 0, a, ax > 0.0f ? 1 : -1, 0);
		}
	}
	else if(hookActive)
	{
		fnAddCand(2, 0, -1, 0, 0);
		fnAddCand(2, 0, -1, 1, 0);
		fnAddCand(2, 0, -1, -1, 0);
	}

	// ── MACRO chunk expansion: each candidate simulates to its commit
	// point; the surviving end state becomes one child with G += ticks.
	// The A* open set + BestG dedup prune; freeze/forbidden/death kill
	// the chunk inside PfMacroRunChunk. ──
	for(int ci = 0; ci < nCands; ci++)
	{
		CCharacter *pBot = fnSpawnBot(cur.Core, cur.Core.Pos, cur.FreezeTime, cur.TickOffset);
		if(!pBot)
			continue;
		PfMacroChunk chunk;
		if(!PfMacroRunChunk(m_PfSimWorld, LocalID, pBot, cur.Core, cur.Grounded,
				aCands[ci], HookAngles, RootStartTick + cur.TickOffset,
				m_pForbiddenGrid, PfCustomFinishSimGrid(), chunk))
			continue;
		fnPushChild(chunk.end, chunk.endFreezeTime, chunk.inputs, chunk.traj,
			chunk.hookSegs, chunk.ticks, 0, 0, 0);
	}
	fnFlushPending();
	return 0;
}

// Build the visual path + hook segments along the parent chain (no
// re-simulation): frozen nodes replay their cached per-tick trajectory,
// single-tick nodes derive the parent.Pos → Pos segment and the grab seg.
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
	newPath.push_back(m_PfANodes[vPathIdx[0]].Core.Pos);
	for(size_t i = 1; i < vPathIdx.size(); i++)
	{
		AStarNode &node = m_PfANodes[vPathIdx[i]];
		if(node.PrimIdx == 0)
		{
			for(size_t k = 1; k < node.Traj.size(); k++)
				newPath.push_back(node.Traj[k]);
			for(const auto &seg : node.HookSegs)
				newHookSegs.push_back(seg);
			continue;
		}
		newPath.push_back(node.Core.Pos);
		const AStarNode &parent = m_PfANodes[vPathIdx[i - 1]];
		if(node.Core.HookState == HOOK_GRABBED && parent.Core.HookState != HOOK_GRABBED)
			newHookSegs.push_back({node.Core.Pos, node.Core.HookPos});
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
		const AStarNode &node = m_PfANodes[vPathIdx[i]];
		if(node.PrimIdx == 0)
		{
			for(size_t k = 0; k < node.Inputs.size(); k++)
				m_PfFullInputs.push_back(node.Inputs[k]);
			continue;
		}
		const int bits = node.PrimIdx - 1;
		const int dirIdx = bits & 3;
		const int jump = (bits >> 2) & 1;
		const int mode = (bits >> 3) & 3;
const int angleIdx = (bits >> 5) & 63;
			const int angles = ((bits >> 11) & 63) + 1;
		CNetObj_PlayerInput in;
		mem_zero(&in, sizeof(in));
		in.m_Direction = dirIdx - 1;
		in.m_Jump = jump;
		in.m_Hook = (mode != 0) ? 1 : 0;
		if(mode == 0)
		{
			in.m_TargetX = (dirIdx - 1) * 256;
			in.m_TargetY = -256;
		}
		else if(mode == 1)
		{
			const AStarNode &parent = m_PfANodes[vPathIdx[i - 1]];
			in.m_TargetX = (int)(parent.Core.HookPos.x - parent.Core.Pos.x);
			in.m_TargetY = (int)(parent.Core.HookPos.y - parent.Core.Pos.y);
		}
		else
		{
			float t = (angles <= 1) ? 0.0f : (float)angleIdx / (float)(angles - 1);
			float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			in.m_TargetX = (int)(cosf(angle) * 256.0f);
			in.m_TargetY = (int)(sinf(angle) * 256.0f);
		}
		m_PfFullInputs.push_back(in);
	}

	m_PfFullInputsIdx = 0;

	if(!vPathIdx.empty())
	{
		AStarNode &last = m_PfANodes[vPathIdx.back()];
		m_PfCurPos = last.Core.Pos;
		m_PfCurVel = last.Core.Vel;
		m_PfCurHookState = last.Core.HookState;
		m_PfCurHookPos = last.Core.HookPos;
		m_PfCurHookDir = last.Core.HookDir;
		m_PfCurHookTick = last.Core.HookTick;
		m_PfCurJumped = last.Core.Jumped;
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

	int algo = pf_clamp(g_Config.m_KxPfAlgorithm, 0, 3);
	if(algo == 2)
	{
		PfRheaWorker();
		return;
	}
	if(algo == 3)
	{
		PfBmctsWorker();
		return;
	}

	while(!m_PfThreadCancel.load(std::memory_order_relaxed))
	{
		int result = PfAStarStep();

		// Live preview: often early (so the first chunk shows up fast),
		// then throttled — the full path copy under the mutex is not free.
		if(m_PfAExpandCount <= 8 || m_PfAExpandCount % 256 == 0)
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

bool CBotNet::PfEvalSequence(const PfAState &cur, int curFreeze, int tickOffset,
	const std::vector<CNetObj_PlayerInput> &inSeq, PfChunkResult &out)
{
	int LocalID = m_PfLocalID;
	CCharacter *pBot = PfSimWorldBot(m_PfSimWorld, m_PfBaseWorld, LocalID);
	if(!pBot)
		return false;
	pBot = PfSpawnSimBot(m_PfSimWorld, LocalID, cur.Pos, cur.Vel,
		cur.HookState, cur.HookPos, cur.HookDir, cur.HookTick, curFreeze, cur.Jumped);
	if(!pBot)
		return false;
	CCharacterCore c = pBot->GetCore();
	c.m_JumpedTotal = cur.JumpedTotal;
	pBot->SetCore(c);
	CCharacterCore savedCore = pBot->GetCore();
	vec2 savedPos = pBot->m_Pos;
	int savedFreeze = pBot->m_FreezeTime;
	PfSimulateChunk(m_PfSimWorld, LocalID, pBot, inSeq, m_PfSimStartTick + tickOffset, out,
		m_pForbiddenGrid, PfCustomFinishSimGrid());
	if((pBot = m_PfSimWorld->GetCharacterById(LocalID)))
	{
		pBot->SetCore(savedCore);
		pBot->m_Pos = savedPos;
		pBot->m_FreezeTime = savedFreeze;
	}
	return true;
}

float CBotNet::PfSeqScore(const std::vector<vec2> &vFinishes, const PfChunkResult &res, bool freezeSupport)
{
	if(res.died && !res.reachedFinish)
		return -1e18f;
	if(res.forbiddenTicks > 0)
		return -1e18f;
	if(!freezeSupport && res.freezeTicks > 0)
		return -1e18f;
	if(res.reachedFinish)
		return 1e9f - (float)res.traj.size();
	return -PfAStarHeuristic(res.endPos, PfNearestFinishPos(vFinishes, res.endPos));
}

void CBotNet::PfSeqApplyEnd(PfAState &st, const PfChunkResult &res)
{
	st.Pos = res.endPos;
	st.Vel = res.endVel;
	st.HookState = res.endHookState;
	st.HookPos = res.endHookPos;
	st.HookDir = res.endHookDir;
	st.HookTick = res.endHookTick;
	st.Jumped = res.endJumped;
	st.JumpedTotal = res.endJumpedTotal;
}

void CBotNet::PfSeqAppend(std::vector<vec2> &fullTraj, std::vector<PfHookSeg> &fullSegs,
	std::vector<CNetObj_PlayerInput> &fullInputs, const PfChunkResult &res, const std::vector<CNetObj_PlayerInput> &inSeq)
{
	int used = (int)res.traj.size() - 1;
	for(int t = 0; t < used && t < (int)inSeq.size(); t++)
		fullInputs.push_back(inSeq[t]);
	for(size_t k = 1; k < res.traj.size(); k++)
		fullTraj.push_back(res.traj[k]);
	for(const PfHookSeg &s : res.hookSegs)
		fullSegs.push_back(s);
}

void CBotNet::PfSeqFinalize(PfAState cur, int curFreeze, int tickOffset, std::vector<vec2> &fullTraj,
	std::vector<PfHookSeg> &fullSegs, std::vector<CNetObj_PlayerInput> &fullInputs)
{
	std::lock_guard<std::mutex> lock(m_PfDataMutex);
	AStarNode node;
	node.Core = cur;
	node.FreezeTime = curFreeze;
	node.Grounded = false;
	node.G = (float)tickOffset;
	node.H = 0.0f;
	node.F = node.G;
	node.ParentIdx = 0;
	node.PrimIdx = 0;
	node.PrimTicks = 0;
	node.TickOffset = tickOffset;
	node.FreezeChainTicks = 0;
	node.ForbiddenChainTicks = 0;
	node.Traj = std::move(fullTraj);
	node.Inputs = std::move(fullInputs);
	node.HookSegs = std::move(fullSegs);
	m_PfANodes.push_back(std::move(node));
	m_PfAGoalIdx = (int)m_PfANodes.size() - 1;
}

void CBotNet::PfRheaWorker()
{
	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	int LocalID = m_PfLocalID;
	int Pop = PfEffortRheaPop();
	int Gens = PfEffortRheaGens();
	int HookAngles = PfEffortHookAngles();
	float W = PfEffortAStarWeight() / 100.0f;
	if(W < 1.0f)
		W = 1.0f;
	if(W > 20.0f)
		W = 20.0f;
	CCollision *pColl = m_PfSimWorld->Collision();
	float hookLength = 380.0f;
	if(CCharacter *pBaseChar = m_PfBaseWorld->GetCharacterById(LocalID))
	{
		float hl = pBaseChar->GetCore().m_Tuning.m_HookLength;
		if(hl > 1.0f && hl < 10000.0f)
			hookLength = hl;
	}
	bool freezeSupport = PfEffortFreezeSupport();
	int64_t simBudget = pf_clamp(g_Config.m_KxPfAStarNodeBudget, 1000, 100000000);

	uint64_t rngState = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)m_PfSimStartTick;
	auto rnd = [&]() -> uint64_t {
		rngState ^= rngState << 13;
		rngState ^= rngState >> 7;
		rngState ^= rngState << 17;
		return rngState;
	};
	auto rndInt = [&](int n) -> int { return (int)(rnd() % (uint64_t)(n > 0 ? n : 1)); };

	std::vector<int> pop;
	pop.push_back(0);

	int64_t sims = 0;
	int gens = 0;
	bool done = false;
	while(!done && !m_PfThreadCancel.load(std::memory_order_relaxed) && (int)m_PfANodes.size() < simBudget)
	{
		for(int g = 0; g < Gens && !done; g++)
		{
			if(pop.empty())
				break;
			int parentIdx = pop[0];
			if(pop.size() > 1)
			{
				int a = pop[rndInt((int)pop.size())];
				int b = pop[rndInt((int)pop.size())];
				parentIdx = (m_PfANodes[a].F <= m_PfANodes[b].F) ? a : b;
			}
			AStarNode parent = m_PfANodes[parentIdx];
			for(size_t pi = 0; pi < pop.size(); pi++)
			{
				if(pop[pi] == parentIdx)
				{
					pop.erase(pop.begin() + pi);
					break;
				}
			}
			gens++;
			if(parent.FreezeTime > 0)
			{
				int ft = parent.FreezeTime;
				std::vector<CNetObj_PlayerInput> ff(ft);
				for(int t = 0; t < ft; t++)
				{
					mem_zero(&ff[t], sizeof(ff[t]));
					ff[t].m_TargetY = -1;
				}
				PfChunkResult res;
				sims++;
				if(!PfEvalSequence(parent.Core, parent.FreezeTime, parent.TickOffset, ff, res)
					|| (res.died && !res.reachedFinish) || res.forbiddenTicks > 0)
					continue;
				AStarNode child;
				child.Core = parent.Core;
				PfSeqApplyEnd(child.Core, res);
				child.FreezeTime = res.endFreezeTime;
				child.Grounded = pColl->IsOnGround(child.Core.Pos, CCharacterCore::PhysicalSize());
				child.G = parent.G + (float)ft;
				child.H = PfAStarHeuristic(child.Core.Pos, PfNearestFinishPos(vFinishes, child.Core.Pos));
				child.F = child.G + W * child.H;
				child.ParentIdx = parentIdx;
				child.PrimIdx = 0;
				child.PrimTicks = ft;
				child.TickOffset = parent.TickOffset + ft;
				child.FreezeChainTicks = parent.FreezeChainTicks + ft;
				child.ForbiddenChainTicks = parent.ForbiddenChainTicks + res.forbiddenTicks;
				child.Traj = std::move(res.traj);
				child.Inputs = std::move(ff);
				child.HookSegs = std::move(res.hookSegs);
				{
					std::lock_guard<std::mutex> lock(m_PfDataMutex);
					m_PfANodes.push_back(std::move(child));
				}
				int childIdx = (int)m_PfANodes.size() - 1;
				if(res.reachedFinish && (freezeSupport || child.FreezeChainTicks == 0) && child.ForbiddenChainTicks == 0)
				{
					m_PfAGoalIdx = childIdx;
					done = true;
					break;
				}
				pop.push_back(childIdx);
				continue;
			}
			CNetObj_PlayerInput aC[128];
			int aCodes[128];
			int nC = PfBuildTickCandidates(parent.Core, pColl, hookLength, HookAngles, aC, aCodes);
			for(int ci = 0; ci < nC && !done; ci++)
			{
				std::vector<CNetObj_PlayerInput> one(1, aC[ci]);
				PfChunkResult res;
				sims++;
				if(!PfEvalSequence(parent.Core, parent.FreezeTime, parent.TickOffset, one, res))
					continue;
				if(res.died && !res.reachedFinish)
					continue;
				if(res.forbiddenTicks > 0)
					continue;
				if(!freezeSupport && res.freezeTicks > 0)
					continue;
				PfAState endCore = parent.Core;
				PfSeqApplyEnd(endCore, res);
				bool endGrounded = pColl->IsOnGround(endCore.Pos, CCharacterCore::PhysicalSize());
				float newG = parent.G + 1.0f;
				uint64_t key = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
				auto it = m_PfABestG.find(key);
				if(it != m_PfABestG.end() && it->second <= newG)
					continue;
				m_PfABestG[key] = newG;
				AStarNode child;
				child.Core = endCore;
				child.FreezeTime = res.endFreezeTime;
				child.Grounded = endGrounded;
				child.G = newG;
				child.H = PfAStarHeuristic(endCore.Pos, PfNearestFinishPos(vFinishes, endCore.Pos));
				child.F = child.G + W * child.H;
				child.ParentIdx = parentIdx;
				child.PrimIdx = aCodes[ci];
				child.PrimTicks = 1;
				child.TickOffset = parent.TickOffset + 1;
				child.FreezeChainTicks = parent.FreezeChainTicks + res.freezeTicks;
				child.ForbiddenChainTicks = parent.ForbiddenChainTicks + res.forbiddenTicks;
				if(child.H < m_PfABestH.load(std::memory_order_relaxed))
					m_PfABestH.store(child.H, std::memory_order_relaxed);
				{
					std::lock_guard<std::mutex> lock(m_PfDataMutex);
					m_PfANodes.push_back(std::move(child));
				}
				int childIdx = (int)m_PfANodes.size() - 1;
				if(res.reachedFinish && (freezeSupport || child.FreezeChainTicks == 0) && child.ForbiddenChainTicks == 0)
				{
					m_PfAGoalIdx = childIdx;
					done = true;
					break;
				}
				pop.push_back(childIdx);
			}
		}
		m_PfAExpandCount = gens;
		if(!done)
		{
			std::sort(pop.begin(), pop.end(), [&](int a, int b) { return m_PfANodes[a].F < m_PfANodes[b].F; });
			if((int)pop.size() > Pop)
				pop.resize(Pop);
			if(pop.empty())
				break;
		}
		if(!done && !pop.empty() && gens % 256 == 0)
			PfAStarResimulatePath(pop[0]);
		if(!done && gens % 4096 == 0 && gens > 0)
			dbg_msg("pathfinder", "RHEA progress: %d generations, %lld sims, %zu nodes", gens, (long long)sims, m_PfANodes.size());
	}
	if(done)
	{
		dbg_msg("pathfinder", "RHEA done: %d ticks (%lld sims, %zu nodes)", m_PfANodes[m_PfAGoalIdx].TickOffset, (long long)sims, m_PfANodes.size());
		m_PfThreadResult.store(1);
		m_PfThreadRunning.store(false);
		return;
	}
	dbg_msg("pathfinder", "RHEA stopped: %lld sims, %zu nodes", (long long)sims, m_PfANodes.size());
	m_PfThreadResult.store(-1);
	m_PfThreadRunning.store(false);
}

void CBotNet::PfBmctsWorker()
{
	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	int LocalID = m_PfLocalID;
	int Iters = PfEffortMctsIters();
	int HookAngles = PfEffortHookAngles();
	CCollision *pColl = m_PfSimWorld->Collision();
	float hookLength = 380.0f;
	if(CCharacter *pBaseChar = m_PfBaseWorld->GetCharacterById(LocalID))
	{
		float hl = pBaseChar->GetCore().m_Tuning.m_HookLength;
		if(hl > 1.0f && hl < 10000.0f)
			hookLength = hl;
	}
	bool freezeSupport = PfEffortFreezeSupport();
	int64_t simBudget = pf_clamp(g_Config.m_KxPfAStarNodeBudget, 1000, 100000000);

	uint64_t rngState = 0xC0FFEE123456789ull ^ (uint64_t)(uint32_t)m_PfSimStartTick;
	auto rnd = [&]() -> uint64_t {
		rngState ^= rngState << 13;
		rngState ^= rngState >> 7;
		rngState ^= rngState << 17;
		return rngState;
	};
	auto rndInt = [&](int n) -> int { return (int)(rnd() % (uint64_t)(n > 0 ? n : 1)); };

	struct BNode
	{
		int parent;
		int depth;
		std::vector<CNetObj_PlayerInput> in;
		PfAState st;
		int freeze;
		int nCand;
		int chainFreeze;
		int chainForbidden;
		double sum;
		int visits;
		std::vector<int> children;
	};
	std::vector<BNode> nodes;
	BNode rootNode;
	rootNode.parent = -1;
	rootNode.depth = 0;
	rootNode.st = m_PfANodes[0].Core;
	rootNode.freeze = m_PfANodes[0].FreezeTime;
	rootNode.nCand = -1;
	rootNode.chainFreeze = 0;
	rootNode.chainForbidden = 0;
	rootNode.sum = 0.0;
	rootNode.visits = 0;
	nodes.push_back(rootNode);

	int64_t sims = 0;
	int iters = 0;
	bool done = false;
	std::vector<CNetObj_PlayerInput> winPath;
	int bestLeaf = -1;
	double bestLeafScore = -1e300;
	auto buildPreview = [&](int leafIdx) {
		std::vector<int> chain;
		for(int b = leafIdx; b >= 0; b = nodes[b].parent)
			chain.push_back(b);
		std::reverse(chain.begin(), chain.end());
		std::vector<vec2> path;
		std::vector<PfHookSeg> segs;
		path.push_back(nodes[0].st.Pos);
		for(size_t i = 1; i < chain.size(); i++)
		{
			const BNode &c = nodes[chain[i]];
			const BNode &p = nodes[chain[i - 1]];
			path.push_back(c.st.Pos);
			if(c.st.HookState == HOOK_GRABBED && p.st.HookState != HOOK_GRABBED)
				segs.push_back({c.st.Pos, c.st.HookPos});
		}
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		m_PfVPath = std::move(path);
		m_PfVHookSegs = std::move(segs);
	};

	while(!done && !m_PfThreadCancel.load(std::memory_order_relaxed) && (int)nodes.size() < simBudget)
	{
		for(int it = 0; it < Iters && !done; it++)
		{
			if(m_PfThreadCancel.load(std::memory_order_relaxed) || (int)nodes.size() >= simBudget)
				break;
			int node = 0;
			while(true)
			{
				int curFreeze = nodes[node].freeze;
				int curDepth = nodes[node].depth;
				PfAState curSt = nodes[node].st;
				if(curFreeze > 0)
				{
					if(!nodes[node].children.empty())
					{
						node = nodes[node].children[0];
						continue;
					}
					std::vector<CNetObj_PlayerInput> edge(curFreeze);
					for(int t = 0; t < curFreeze; t++)
					{
						mem_zero(&edge[t], sizeof(edge[t]));
						edge[t].m_TargetY = -1;
					}
					PfChunkResult r1;
					sims++;
					if(!PfEvalSequence(curSt, curFreeze, curDepth, edge, r1))
						break;
					BNode ch;
					ch.parent = node;
					ch.depth = curDepth + (int)edge.size();
					ch.in = std::move(edge);
					ch.st = curSt;
					PfSeqApplyEnd(ch.st, r1);
					ch.freeze = r1.endFreezeTime;
					ch.nCand = -1;
					ch.chainFreeze = nodes[node].chainFreeze + curFreeze;
					ch.chainForbidden = nodes[node].chainForbidden + r1.forbiddenTicks;
					ch.sum = 0.0;
					ch.visits = 0;
					nodes.push_back(std::move(ch));
					int childIdx = (int)nodes.size() - 1;
					nodes[node].children.push_back(childIdx);
					if(r1.reachedFinish && (freezeSupport || nodes[childIdx].chainFreeze == 0) && nodes[childIdx].chainForbidden == 0)
					{
						for(int b = childIdx; b >= 0; b = nodes[b].parent)
						{
							nodes[b].sum += 1e6;
							nodes[b].visits++;
						}
						winPath.clear();
						for(int b = childIdx; b > 0; b = nodes[b].parent)
							winPath.insert(winPath.begin(), nodes[b].in.begin(), nodes[b].in.end());
						done = true;
					}
					else if((r1.died && !r1.reachedFinish) || r1.forbiddenTicks > 0 || nodes[childIdx].chainForbidden > 0 || (!freezeSupport && nodes[childIdx].chainFreeze > 0))
					{
						for(int b = childIdx; b >= 0; b = nodes[b].parent)
						{
							nodes[b].sum += -1e5;
							nodes[b].visits++;
						}
					}
					else
					{
						double sc = (double)PfSeqScore(vFinishes, r1, freezeSupport);
						for(int b = childIdx; b >= 0; b = nodes[b].parent)
						{
							nodes[b].sum += sc;
							nodes[b].visits++;
						}
						if(sc > bestLeafScore)
						{
							bestLeafScore = sc;
							bestLeaf = childIdx;
						}
					}
					break;
				}
				int nCand = nodes[node].nCand;
				if(nCand < 0)
				{
					CNetObj_PlayerInput aTmp[128];
					nCand = PfBuildTickCandidates(curSt, pColl, hookLength, HookAngles, aTmp, nullptr);
					nodes[node].nCand = nCand;
				}
				if(nCand == 0)
				{
					for(int b = node; b >= 0; b = nodes[b].parent)
					{
						nodes[b].sum += -1e5;
						nodes[b].visits++;
					}
					break;
				}
				if((int)nodes[node].children.size() < nCand)
				{
					CNetObj_PlayerInput aC[128];
					PfBuildTickCandidates(curSt, pColl, hookLength, HookAngles, aC, nullptr);
					int tried = (int)nodes[node].children.size();
					int pick = tried + rndInt(nCand - tried);
					std::swap(aC[tried], aC[pick]);
					std::vector<CNetObj_PlayerInput> edge(1, aC[tried]);
					PfChunkResult r1;
					sims++;
					if(!PfEvalSequence(curSt, curFreeze, curDepth, edge, r1))
						break;
					BNode ch;
					ch.parent = node;
					ch.depth = curDepth + 1;
					ch.in = std::move(edge);
					ch.st = curSt;
					PfSeqApplyEnd(ch.st, r1);
					ch.freeze = r1.endFreezeTime;
					ch.nCand = -1;
					ch.chainFreeze = nodes[node].chainFreeze + r1.freezeTicks;
					ch.chainForbidden = nodes[node].chainForbidden + r1.forbiddenTicks;
					ch.sum = 0.0;
					ch.visits = 0;
					nodes.push_back(std::move(ch));
					int childIdx = (int)nodes.size() - 1;
					nodes[node].children.push_back(childIdx);
					if(r1.reachedFinish && (freezeSupport || nodes[childIdx].chainFreeze == 0) && nodes[childIdx].chainForbidden == 0)
					{
						for(int b = childIdx; b >= 0; b = nodes[b].parent)
						{
							nodes[b].sum += 1e6;
							nodes[b].visits++;
						}
						winPath.clear();
						for(int b = childIdx; b > 0; b = nodes[b].parent)
							winPath.insert(winPath.begin(), nodes[b].in.begin(), nodes[b].in.end());
						done = true;
					}
					else if((r1.died && !r1.reachedFinish) || r1.forbiddenTicks > 0 || nodes[childIdx].chainForbidden > 0 || (!freezeSupport && nodes[childIdx].chainFreeze > 0))
					{
						for(int b = childIdx; b >= 0; b = nodes[b].parent)
						{
							nodes[b].sum += -1e5;
							nodes[b].visits++;
						}
					}
					else
					{
						double sc = (double)PfSeqScore(vFinishes, r1, freezeSupport);
						for(int b = childIdx; b >= 0; b = nodes[b].parent)
						{
							nodes[b].sum += sc;
							nodes[b].visits++;
						}
						if(sc > bestLeafScore)
						{
							bestLeafScore = sc;
							bestLeaf = childIdx;
						}
					}
					break;
				}
				float logN = logf((float)nodes[node].visits + 1.0f);
				double bestU = -1e300;
				int bestChild = -1;
				for(int cIdx : nodes[node].children)
				{
					const BNode &c = nodes[cIdx];
					double exploit = (c.visits > 0) ? c.sum / (double)c.visits : 1e18;
					double explore = 1.4 * sqrtf(logN / (float)(c.visits > 0 ? c.visits : 1));
					double u = exploit + explore;
					if(u > bestU)
					{
						bestU = u;
						bestChild = cIdx;
					}
				}
				if(bestChild < 0)
					break;
				node = bestChild;
			}
			iters++;
		}
		m_PfAExpandCount = iters;
		if(!done && bestLeaf >= 0)
			buildPreview(bestLeaf);
	}
	if(done)
	{
		PfChunkResult res;
		if(PfEvalSequence(m_PfANodes[0].Core, m_PfANodes[0].FreezeTime, 0, winPath, res) && res.reachedFinish)
		{
			std::vector<vec2> fullTraj;
			fullTraj.push_back(m_PfANodes[0].Core.Pos);
			std::vector<PfHookSeg> fullSegs;
			std::vector<CNetObj_PlayerInput> fullInputs;
			PfSeqAppend(fullTraj, fullSegs, fullInputs, res, winPath);
			int totalTicks = (int)res.traj.size() - 1;
			PfAState endSt = m_PfANodes[0].Core;
			PfSeqApplyEnd(endSt, res);
			PfSeqFinalize(endSt, res.endFreezeTime, totalTicks, fullTraj, fullSegs, fullInputs);
			dbg_msg("pathfinder", "BMCTS done: %d ticks (%lld sims)", totalTicks, (long long)sims);
			m_PfThreadResult.store(1);
			m_PfThreadRunning.store(false);
			return;
		}
	}
	if(bestLeaf >= 0)
		buildPreview(bestLeaf);
	dbg_msg("pathfinder", "BMCTS stopped: %lld sims, %d tree nodes", (long long)sims, (int)nodes.size());
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
