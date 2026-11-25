#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <engine/external/lz4/lz4.h>
#include <algorithm>
#include <chrono>

// =========================================================
// PHYSICS A* — v1.56.245: time-optimal per-tick search.
//
// Uniform-cost A* over simulated character states. Each expansion
// simulates EVERY valid single-tick input (direction x jump, hook
// steering while attached, ray-validated hook-fire angles) through
// the real CCharacter physics (PfSimulateChunk) and pushes each
// surviving end state as a child. The previous candidate generators
// (RHEA evolution, chunk beam search) and the score/penalty ranking
// are gone: G is pure simulated ticks, pruning is done only by the
// BestG state dedup and the flow-field heuristic, so the first
// finish found is the fastest path the quantized state space can
// express instead of the best-scoring sample of one.
// =========================================================

// State dedup key: quantized position (2px), velocity (3px), hook state,
// hook tick, grounded, frozen time, jump flags, hook offset.
// Near-finish stall fallback: if the best H sits within this many heuristic
// ticks of a finish (flow field: tiles × 2, so 30 ticks ≈ 15 tiles) and has
// not improved for this many seconds, the search parks the open set and
// switches to a greedy walk from the lowest-H branch.
static constexpr float PF_A_STALL_H_TICKS = 30.0f;
static constexpr float PF_A_STALL_SECONDS = 10.0f;

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

static inline bool PfAOpenLess(const CBotNet::KxPfOpenEnt &a, const CBotNet::KxPfOpenEnt &b)
{
	if(a.F != b.F)
		return a.F < b.F;
	return a.H < b.H;
}

// Open-set heap over (idx, F, H) entries — nodes are immutable after push,
// so the stored F/H never diverge and heap sifting never touches the node
// store (compressed segments stay untouched during sift).
static void PfAStarHeapPush(std::vector<CBotNet::KxPfOpenEnt> &heap, CBotNet::KxPfOpenEnt ent)
{
	heap.push_back(ent);
	int i = (int)heap.size() - 1;
	while(i > 0)
	{
		int parent = (i - 1) / 2;
		if(!PfAOpenLess(heap[i], heap[parent]))
			break;
		std::swap(heap[parent], heap[i]);
		i = parent;
	}
}

static CBotNet::KxPfOpenEnt PfAStarHeapPop(std::vector<CBotNet::KxPfOpenEnt> &heap)
{
	CBotNet::KxPfOpenEnt top = heap[0];
	heap[0] = heap.back();
	heap.pop_back();
	int n = (int)heap.size();
	int i = 0;
	while(true)
	{
		int l = 2 * i + 1, r = 2 * i + 2, s = i;
		if(l < n && PfAOpenLess(heap[l], heap[s]))
			s = l;
		if(r < n && PfAOpenLess(heap[r], heap[s]))
			s = r;
		if(s == i)
			break;
		std::swap(heap[s], heap[i]);
		i = s;
	}
	return top;
}

// =========================================================
// v6.15 segmented compressed node store. Nodes are immutable after
// push; every PF_NODE_SEG_SIZE nodes the worker deep-serializes the
// active tail into one LZ4 block (real maps push node arrays past GBs
// — compression keeps the same node budgets feasible). Serialization
// is explicit: the vectors inside AStarNode are copied by value, never
// memcpy'd as pointers.
// =========================================================
static void PfNodeSerAppend(std::vector<uint8_t> &vOut, const void *pData, size_t Size)
{
	const uint8_t *pSrc = (const uint8_t *)pData;
	vOut.insert(vOut.end(), pSrc, pSrc + Size);
}

static void PfNodeSerialize(std::vector<uint8_t> &vOut, const CBotNet::AStarNode &Node)
{
	CBotNet::KxPfNodeSer s;
	s.Core = Node.Core;
	s.FreezeTime = Node.FreezeTime;
	s.Grounded = Node.Grounded;
	s.G = Node.G;
	s.H = Node.H;
	s.F = Node.F;
	s.ParentIdx = Node.ParentIdx;
	s.PrimIdx = Node.PrimIdx;
	s.PrimTicks = Node.PrimTicks;
	s.TickOffset = Node.TickOffset;
	s.AimX = Node.AimX;
	s.AimY = Node.AimY;
	s.FreezeChainTicks = Node.FreezeChainTicks;
	s.ForbiddenChainTicks = Node.ForbiddenChainTicks;
	s.TrajCount = (int)Node.Traj.size();
	s.InputsCount = (int)Node.Inputs.size();
	s.SegsCount = (int)Node.HookSegs.size();
	PfNodeSerAppend(vOut, &s, sizeof(s));
	if(!Node.Traj.empty())
		PfNodeSerAppend(vOut, Node.Traj.data(), Node.Traj.size() * sizeof(vec2));
	if(!Node.Inputs.empty())
		PfNodeSerAppend(vOut, Node.Inputs.data(), Node.Inputs.size() * sizeof(CNetObj_PlayerInput));
	if(!Node.HookSegs.empty())
		PfNodeSerAppend(vOut, Node.HookSegs.data(), Node.HookSegs.size() * sizeof(PfHookSeg));
}

static bool PfNodeDeserializeAll(const uint8_t *pData, size_t Size, int NodeCount, std::vector<CBotNet::AStarNode> &vOut)
{
	vOut.clear();
	if(NodeCount <= 0)
		return NodeCount == 0;
	vOut.reserve((size_t)NodeCount);
	const uint8_t *p = pData;
	size_t Left = Size;
	auto Read = [&p, &Left](void *dst, size_t sz) -> bool {
		if(Left < sz)
			return false;
		memcpy(dst, p, sz);
		p += sz;
		Left -= sz;
		return true;
	};
	for(int i = 0; i < NodeCount; i++)
	{
		CBotNet::KxPfNodeSer s;
		if(!Read(&s, sizeof(s)))
			return false;
		CBotNet::AStarNode n;
		n.Core = s.Core;
		n.FreezeTime = s.FreezeTime;
		n.Grounded = s.Grounded;
		n.G = s.G;
		n.H = s.H;
		n.F = s.F;
		n.ParentIdx = s.ParentIdx;
		n.PrimIdx = s.PrimIdx;
		n.PrimTicks = s.PrimTicks;
		n.TickOffset = s.TickOffset;
		n.AimX = s.AimX;
		n.AimY = s.AimY;
		n.FreezeChainTicks = s.FreezeChainTicks;
		n.ForbiddenChainTicks = s.ForbiddenChainTicks;
		n.Traj.resize((size_t)s.TrajCount);
		n.Inputs.resize((size_t)s.InputsCount);
		n.HookSegs.resize((size_t)s.SegsCount);
		if(s.TrajCount && !Read(n.Traj.data(), (size_t)s.TrajCount * sizeof(vec2)))
			return false;
		if(s.InputsCount && !Read(n.Inputs.data(), (size_t)s.InputsCount * sizeof(CNetObj_PlayerInput)))
			return false;
		if(s.SegsCount && !Read(n.HookSegs.data(), (size_t)s.SegsCount * sizeof(PfHookSeg)))
			return false;
		vOut.push_back(std::move(n));
	}
	return true;
}

// Append a node to the active tail; returns its GLOBAL index (compacted
// segments stay addressable by the same index space). Worker-thread only.
int CBotNet::PfNodeAppend(AStarNode &&Node)
{
	m_PfANodes.push_back(std::move(Node));
	return m_PfASegTotal + (int)m_PfANodes.size() - 1;
}

// Read any node by GLOBAL index. Tail reads are lock-free (the worker is
// the only mutator, and renderer tail reads hold m_PfDataMutex); compressed
// reads take m_PfSegMutex for the segment list + 2-slot decompressed LRU.
// Returns a copy — safe against cache eviction.
CBotNet::AStarNode CBotNet::PfNodeGet(int Idx)
{
	const AStarNode DefaultNode{};
	if(Idx < 0 || Idx >= PfNodeTotal())
		return DefaultNode;
	if(Idx >= m_PfASegTotal)
		return m_PfANodes[Idx - m_PfASegTotal];
	std::lock_guard<std::mutex> Lock(m_PfSegMutex);
	for(int s = (int)m_PfASegs.size() - 1; s >= 0; s--)
	{
		const KxPfNodeSeg &Seg = m_PfASegs[s];
		if(Idx < Seg.BaseIdx || Idx >= Seg.BaseIdx + Seg.NodeCount)
			continue;
		KxPfNodeSegCache *pSlot = nullptr;
		for(auto &Slot : m_PfASegCache)
		{
			if(Slot.SegIdx == s)
			{
				pSlot = &Slot;
				break;
			}
		}
		if(!pSlot)
		{
			pSlot = &m_PfASegCache[m_PfASegCacheClock++ % 2];
			std::vector<uint8_t> vRaw;
			vRaw.resize(Seg.RawSize);
			if(LZ4_decompress_safe((const char *)Seg.Data.data(), (char *)vRaw.data(),
				(int)Seg.Data.size(), (int)Seg.RawSize) < 0)
				return DefaultNode;
			if(!PfNodeDeserializeAll(vRaw.data(), vRaw.size(), Seg.NodeCount, pSlot->Nodes))
				return DefaultNode;
			pSlot->SegIdx = s;
		}
		return pSlot->Nodes[Idx - Seg.BaseIdx];
	}
	return DefaultNode;
}

// Compress the active tail into a new segment. Called on the worker thread
// when the tail reaches PF_NODE_SEG_SIZE. Serialization runs outside any
// lock (nodes are immutable and the renderer reads them under m_PfDataMutex);
// publishing the segment takes m_PfSegMutex, freeing the tail m_PfDataMutex.
void CBotNet::PfCompactNodeSegment()
{
	if(m_PfANodes.empty())
		return;
	const int Count = (int)m_PfANodes.size();
	std::vector<uint8_t> vRaw;
	vRaw.reserve(m_PfANodes.size() * sizeof(KxPfNodeSer) + 64);
	for(const AStarNode &Node : m_PfANodes)
		PfNodeSerialize(vRaw, Node);
	std::vector<uint8_t> vComp;
	vComp.resize(LZ4_compressBound((int)vRaw.size()));
	const int CompSize = LZ4_compress_default((const char *)vRaw.data(), (char *)vComp.data(),
		(int)vRaw.size(), (int)vComp.size());
	if(CompSize <= 0)
		return; // keep the tail active and retry on the next trigger
	vComp.resize((size_t)CompSize);
	KxPfNodeSeg Seg;
	Seg.BaseIdx = m_PfASegTotal;
	Seg.NodeCount = Count;
	Seg.RawSize = vRaw.size();
	Seg.Data = std::move(vComp);
	const float RawMB = (float)Seg.RawSize / 1048576.0f;
	const float CompMB = (float)Seg.Data.size() / 1048576.0f;
	{
		std::lock_guard<std::mutex> SegLock(m_PfSegMutex);
		m_PfASegs.push_back(std::move(Seg));
		m_PfASegTotal += Count;
	}
	// The serialized deep copy owns everything now; free the live tail.
	{
		std::lock_guard<std::mutex> DataLock(m_PfDataMutex);
		m_PfANodes.clear();
	}
	dbg_msg("pathfinder", "node compaction: %d nodes, %.1f MB -> %.1f MB (%d segments)",
		Count, RawMB, CompMB, (int)m_PfASegs.size());
}

void CBotNet::PfNodeStoreReset()
{
	{
		std::lock_guard<std::mutex> SegLock(m_PfSegMutex);
		m_PfASegs.clear();
		m_PfASegs.shrink_to_fit();
		m_PfASegTotal = 0;
		for(auto &Slot : m_PfASegCache)
		{
			Slot.SegIdx = -1;
			Slot.Nodes.clear();
			Slot.Nodes.shrink_to_fit();
		}
	}
	std::vector<AStarNode>().swap(m_PfANodes);
}

int PfEffortHookAngles()
{
	return (g_Config.m_KxPfEffort >= 2) ? 32 : 24;
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

// v6.18: TAS dummy preparation needs a unit-exact start position, not just a
// finish tile. These two wrappers keep the normal Pathfinder behaviour intact
// and only switch to the exact goal while m_PfUseExactGoal is set.
bool CBotNet::PfAtGoal(const vec2 &Pos) const
{
	if(m_PfUseExactGoal)
		return distance(Pos, m_PfExactGoal) <= 1.0f;
	return PfIsAtFinishPos(PfActiveFinishTiles(), Pos);
}

vec2 CBotNet::PfGoalPos(const std::vector<vec2> &vFinishes, const vec2 &Pos) const
{
	return m_PfUseExactGoal ? m_PfExactGoal : PfNearestFinishPos(vFinishes, Pos);
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
		// The copies are throwaway: delete them (the dtor unlinks from the
		// world), a bare RemoveEntity would orphan them and leak the snapshot.
		for(CEntity *pEnt : vRemove)
			delete pEnt;
	}
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalID)
			continue;
		if(CCharacter *pChar = pWorld->GetCharacterById(i))
			delete pChar;
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

static int PfBuildTickCandidates(const CBotNet::PfAState &core, CCollision *pColl, float hookLength, const PfHookTargets *pTargets,
	CNetObj_PlayerInput aOut[128], int aCodes[128])
{
	int n = 0;
	static const int aDirs[] = {-1, 0, 1};
	auto add = [&](int dir, int jump, int hook, int tx, int ty) {
		if(n >= 128)
			return;
		if(aCodes)
			aCodes[n] = (2 + dir) | (jump << 2) | (hook << 3);
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
	const bool hookIdle = (core.HookState == HOOK_IDLE);
	const bool hookActive = (core.HookState == HOOK_FLYING || core.HookState == HOOK_GRABBED);
	if(hookActive)
	{
		int tx = (int)(core.HookPos.x - core.Pos.x);
		int ty = (int)(core.HookPos.y - core.Pos.y);
		for(int d = 0; d < 3; d++)
		{
			for(int j = 0; j < 2; j++)
				add(aDirs[d], j, 1, tx, ty);
		}
	}
	// Dynamic hook targets: aim at the cached segment hit coordinates, not
	// at a fan angle. A segment contributes candidates only while its ray
	// still lands on the same tile (validated by the caller's cache pass).
	if(hookIdle && pTargets)
	{
		for(int s = 0; s < 36; s++)
		{
			const PfHookTarget &t = pTargets->aSeg[s];
			if(!t.Valid)
				continue;
			const vec2 Aim = t.AimPos - core.Pos;
			const float d2 = Aim.x * Aim.x + Aim.y * Aim.y;
			if(d2 < 24.0f * 24.0f || d2 > hookLength * hookLength)
				continue;
			for(int d = 0; d < 3; d++)
				add(aDirs[d], 0, 2, (int)Aim.x, (int)Aim.y);
		}
	}
	return n;
}

// Per-cell dynamic hook target cache (built on the first node in a 64px
// cell, then re-validated segment by segment on every use: a live target
// must still land on its stored tile; dead or empty segments are re-scanned
// with 3 probes, empty ones only after the cache origin moved 8px+ so a
// stationary node does not burn 36 raycasts per expansion into the sky).
const PfHookTargets *CBotNet::PfAStarHookTargetsFor(const PfAState &core, CCollision *pColl, float hookLength)
{
	if(!pColl)
		return nullptr;
	const int CellX = (int)floorf(core.Pos.x / 64.0f);
	const int CellY = (int)floorf(core.Pos.y / 64.0f);
	const uint64_t Key = ((uint64_t)(uint32_t)CellX << 32) | (uint32_t)(uint32_t)CellY;
	auto it = m_PfAHookCells.find(Key);
	if(it == m_PfAHookCells.end())
	{
		constexpr size_t MAX_HOOK_CELLS = 2048;
		if(m_PfAHookCellOrder.size() - m_PfAHookCellOrderHead >= MAX_HOOK_CELLS)
		{
			m_PfAHookCells.erase(m_PfAHookCellOrder[m_PfAHookCellOrderHead]);
			m_PfAHookCellOrderHead++;
		}
		PfHookCell Cell;
		PfBuildHookTargets(pColl, core.Pos, hookLength, Cell.Targets);
		m_PfAHookCells[Key] = Cell;
		m_PfAHookCellOrder.push_back(Key);
		it = m_PfAHookCells.find(Key);
	}
	PfHookTargets &T = it->second.Targets;
	const bool RescanEmpty = distance(T.Origin, core.Pos) > 8.0f;
	for(int s = 0; s < 36; s++)
	{
		if(T.aSeg[s].Valid)
		{
			if(!PfHookTargetAlive(pColl, core.Pos, hookLength, T.aSeg[s]))
				PfRescanHookSeg(pColl, core.Pos, hookLength, s, T.aSeg[s]);
		}
		else if(RescanEmpty)
			PfRescanHookSeg(pColl, core.Pos, hookLength, s, T.aSeg[s]);
	}
	T.Origin = core.Pos;
	T.HookLength = hookLength;
	T.Built = true;
	return &T;
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

	PfNodeStoreReset();
	m_PfANodes.reserve(4096);
	m_PfAOpen.reserve(4096);
	m_PfABeamReserve.reserve(4096);
	m_PfABestG.Reserve(1 << 14);
	m_PfAGreedy = false;
	m_PfAGreedyTried = false;
	m_PfAOpenBackup.clear();
	m_PfAGreedyG.Clear();
	m_PfAGoalIdx = -1;
	m_PfAExpandCount = 0;
	m_PfARefined = false;
	m_PfARefinedInputs.clear();
	m_PfARefinedPath.clear();
	m_PfARefinedSegs.clear();

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
	root.H = PfAStarHeuristic(root.Core.Pos, PfGoalPos(vFinishes,root.Core.Pos));
	m_PfAHRoot = root.H;
	m_PfABestH.store(root.H, std::memory_order_relaxed);
	root.F = root.G + (float)W * root.H;
	root.ParentIdx = -1;
	root.PrimIdx = -1;
	root.PrimTicks = 0;
	root.TickOffset = 0;
	const float RootF = root.F;
	const float RootH = root.H;
	dbg_msg("pathfinder", "A* start: pos=(%.0f,%.0f) finishes=%d h=%.1f",
		root.Core.Pos.x, root.Core.Pos.y, (int)vFinishes.size(), RootH);
	m_PfABestG.Set(PfAStarKey(root.Core, root.FreezeTime, root.Grounded), 0.0f);
	{
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		const int RootIdx = PfNodeAppend(std::move(root));
		m_PfAOpen.push_back({RootIdx, RootF, RootH});
	}

	if(PfAtGoal(root.Core.Pos))
		m_PfAGoalIdx = 0;
	return true;
}

int CBotNet::PfAStarStep()
{
	if(m_PfAOpen.empty())
	{
		if(g_Config.m_KxPfAlgorithm == 1 && !m_PfABeamReserve.empty())
		{
			int cap = PfEffortBeamWidth() * 4;
			std::sort(m_PfABeamReserve.begin(), m_PfABeamReserve.end(), PfAOpenLess);
			int take = (cap < (int)m_PfABeamReserve.size()) ? cap : (int)m_PfABeamReserve.size();
			std::lock_guard<std::mutex> lock(m_PfDataMutex);
			for(int i = 0; i < take; i++)
				PfAStarHeapPush(m_PfAOpen, m_PfABeamReserve[i]);
			m_PfABeamReserve.erase(m_PfABeamReserve.begin(), m_PfABeamReserve.begin() + take);
			return 0;
		}
		dbg_msg("pathfinder", "A* exhausted: open set empty (no route to a finish tile — blocked by walls/forbidden zones?)");
		return -1;
	}

	int maxNodes = pf_clamp(g_Config.m_KxPfAStarNodeBudget, 1000, 100000000);
	float W = PfEffortAStarWeight() / 100.0f;
if(W < 1.0f)
	W = 1.0f;
if(W > 20.0f)
	W = 20.0f;

	if(PfNodeTotal() > maxNodes)
	{
		dbg_msg("pathfinder", "A* aborted: node budget exhausted (%d nodes)", PfNodeTotal());
		return -1;
	}

	m_PfAExpandCount++;
	if(m_PfAExpandCount % 4096 == 0)
		dbg_msg("pathfinder", "A* progress: %d expansions, %d nodes, open=%zu",
			m_PfAExpandCount, PfNodeTotal(), m_PfAOpen.size());

	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	if(vFinishes.empty())
		return -1;

	CBotNet::KxPfOpenEnt curEnt = PfAStarHeapPop(m_PfAOpen);
	int curIdx = curEnt.Idx;
	AStarNode cur = PfNodeGet(curIdx);

	uint64_t curKey = PfAStarKey(cur.Core, cur.FreezeTime, cur.Grounded);
	const float *pCurG = m_PfAGreedy ? m_PfAGreedyG.Find(curKey) : m_PfABestG.Find(curKey);
	if(!pCurG || *pCurG < cur.G - 0.001f)
		return 0; // stale — a cheaper path to this state exists

	if(PfAtGoal(cur.Core.Pos))
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
		const std::vector<PfHookSeg> &hookSegs, int ticks, int chunkFreezeTicks, int chunkForbiddenTicks, int primIdx,
		int aimX, int aimY) -> bool
	{
		if(!PfEffortFreezeSupport() && cur.FreezeChainTicks + chunkFreezeTicks > 0)
			return false;
		if(cur.ForbiddenChainTicks + chunkForbiddenTicks > 0)
			return false;
		vec2 childPos = endCore.Pos;
		bool endGrounded = pColl->IsOnGround(childPos, CCharacterCore::PhysicalSize());
		uint64_t key = PfAStarKey(endCore, endFreezeTime, endGrounded);
		float newG = cur.G + (float)ticks;
		// Greedy walk dedups against its own visited map: plateau-fan states
		// already blocked by the global BestG are allowed again, otherwise
		// the walk would die on the first revisit of the stalled front.
		const float *pOldG = m_PfAGreedy ? m_PfAGreedyG.Find(key) : m_PfABestG.Find(key);
		if(pOldG && *pOldG <= newG)
			return false;
		if(m_PfAGreedy)
			m_PfAGreedyG.Set(key, newG);
		else
			m_PfABestG.Set(key, newG);
		AStarNode child;
		child.Core = endCore;
		child.FreezeTime = endFreezeTime;
		child.Grounded = endGrounded;
		child.G = newG;
		child.H = PfAStarHeuristic(childPos, PfGoalPos(vFinishes,childPos));
		if(child.H < m_PfABestH.load(std::memory_order_relaxed))
			m_PfABestH.store(child.H, std::memory_order_relaxed);
		child.F = child.G + (float)W * child.H;
		child.ParentIdx = curIdx;
		child.PrimIdx = primIdx;
		child.PrimTicks = ticks;
		child.TickOffset = cur.TickOffset + ticks;
		child.AimX = aimX;
		child.AimY = aimY;
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
			const float PendingF = pending.F;
			const float PendingH = pending.H;
			const int PendingIdx = PfNodeAppend(std::move(pending));
			// Greedy walk pushes F = H: the heap then pops pure best-first by
			// distance to the finish — the walk follows its best branch and
			// never rolls back to an older one.
			PfAStarHeapPush(m_PfAOpen, {PendingIdx, m_PfAGreedy ? PendingH : PendingF, PendingH});
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
			m_pForbiddenGrid, PfCustomFinishSimGrid(), false, m_PfUseExactGoal, m_PfExactGoal);
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
			fnPushChild(endCore, res.endFreezeTime, res.inputs, res.traj, res.hookSegs, ft, ft, res.forbiddenTicks, 0, 0, 0);
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

		// Dynamic hook targets for this node (36 segments over 360°, cached per
		// 64px cell and re-validated against the stored tile from this position).
		const PfHookTargets *pHookTargets = nullptr;
		if(cur.Core.HookState == HOOK_IDLE)
			pHookTargets = PfAStarHookTargetsFor(cur.Core, pColl, hookLength);

		bool hookIdle = (cur.Core.HookState == HOOK_IDLE);
		bool hookActive = (cur.Core.HookState == HOOK_FLYING || cur.Core.HookState == HOOK_GRABBED);

		// Per-tick systematic expansion: build EVERY valid single-tick input for
		// this state, simulate each one tick and push each surviving end state.
		// No candidate scoring, no beam width — the A* open set + BestG dedup
		// decide which branches survive, so G stays pure time and the first
		// finish reached is the fastest path the search can express.
		static const int aDirs[] = {-1, 0, 1};
		CNetObj_PlayerInput aCands[128];
		int aCandCodes[128];
		int nCands = 0;
		auto fnAddCand = [&](int dirIdx, int jump, int mode, vec2 aim) {
			if(nCands >= 128)
				return;
			mem_zero(&aCands[nCands], sizeof(CNetObj_PlayerInput));
			aCands[nCands].m_Direction = aDirs[dirIdx];
			aCands[nCands].m_Jump = jump;
			aCands[nCands].m_Hook = (mode != 0) ? 1 : 0;
			aCands[nCands].m_TargetX = (int)aim.x;
			aCands[nCands].m_TargetY = (int)aim.y;
			aCandCodes[nCands] = (1 + dirIdx) | (jump << 2) | (mode << 3);
			nCands++;
		};
		for(int d = 0; d < 3; d++)
		{
			for(int j = 0; j < 2; j++)
				fnAddCand(d, j, 0, vec2((float)aDirs[d] * 256.0f, -256.0f));
		}
		if(hookActive)
		{
			vec2 aimRel = cur.Core.HookPos - cur.Core.Pos;
			for(int d = 0; d < 3; d++)
			{
				for(int j = 0; j < 2; j++)
					fnAddCand(d, j, 1, aimRel);
			}
		}
		if(hookIdle && pHookTargets)
		{
			for(int s = 0; s < 36; s++)
			{
				const PfHookTarget &t = pHookTargets->aSeg[s];
				if(!t.Valid)
					continue;
				vec2 aim = t.AimPos - cur.Core.Pos;
				float d2 = aim.x * aim.x + aim.y * aim.y;
				if(d2 < 24.0f * 24.0f || d2 > hookLength * hookLength)
					continue;
				for(int d = 0; d < 3; d++)
					fnAddCand(d, 0, 2, aim);
			}
		}

		// Fast Sim (v6.15): one shared chunk result + one reusable input buffer
		// across all candidates — no per-candidate heap traffic — and the light
		// simulation path skips recording the trajectory/inputs/hook segs the
		// expansion never reads. Event-chunked rollout: each candidate holds its
		// input up to m_PfAChunkCur ticks but the chunk is cut short at every
		// decision point (PfSimulateHold), so coarse search keeps the per-tick
		// decision granularity. G stays pure simulated ticks.
		const bool FastSim = g_Config.m_KxPfFastSim != 0;
		const int Chunk = pf_max(1, m_PfAChunkCur);
		PfChunkResult res;
		const std::vector<CNetObj_PlayerInput> emptyInputs;
		const std::vector<vec2> emptyTraj;
		const std::vector<PfHookSeg> emptySegs;
		for(int ci = 0; ci < nCands; ci++)
		{
			CCharacter *pBot = fnSpawnBot(cur.Core, cur.Core.Pos, cur.FreezeTime, cur.TickOffset);
			if(!pBot)
				continue;
			PfSimulateHold(m_PfSimWorld, LocalID, pBot, aCands[ci],
				RootStartTick + cur.TickOffset, Chunk, res,
				m_pForbiddenGrid, PfCustomFinishSimGrid(), FastSim, m_PfUseExactGoal, m_PfExactGoal);
			if(res.died && !res.reachedFinish)
				continue;
			PfAState endCore = cur.Core;
			endCore.Pos = res.endPos;
			endCore.Vel = res.endVel;
			endCore.HookState = res.endHookState;
			endCore.HookPos = res.endHookPos;
			endCore.HookDir = res.endHookDir;
			endCore.HookTick = res.endHookTick;
			endCore.Jumped = res.endJumped;
			endCore.JumpedTotal = res.endJumpedTotal;
			// Chunk edges (more than 1 tick) carry their per-tick trajectory +
			// hook segments so the renderer draws the real arc (old chunk
			// rendering scheme); 1-tick edges stay exact chords.
			const bool ChunkEdge = res.ticksSimulated > 1;
			const std::vector<vec2> &TrajUse = ChunkEdge ? res.traj : emptyTraj;
			const std::vector<PfHookSeg> &SegsUse = ChunkEdge ? res.hookSegs : emptySegs;
			fnPushChild(endCore, res.endFreezeTime, emptyInputs, TrajUse, SegsUse, res.ticksSimulated, res.freezeTicks, res.forbiddenTicks, aCandCodes[ci],
				aCands[ci].m_TargetX, aCands[ci].m_TargetY);
		}
	if(g_Config.m_KxPfAlgorithm == 1)
	{
		int beamK = PfEffortBeamWidth();
		if((int)vPending.size() > beamK)
		{
			std::sort(vPending.begin(), vPending.end(), [](const AStarNode &a, const AStarNode &b) { return a.F < b.F; });
			std::lock_guard<std::mutex> lock(m_PfDataMutex);
			for(int i = beamK; i < (int)vPending.size(); i++)
			{
				const float PendingF = vPending[i].F;
				const float PendingH = vPending[i].H;
				const int PendingIdx = PfNodeAppend(std::move(vPending[i]));
				m_PfABeamReserve.push_back({PendingIdx, PendingF, PendingH});
			}
			vPending.resize(beamK);
		}
	}
	fnFlushPending();
	// v6.15: beam trim with slack. The open set sits at the cap for most of a
	// Beam run, and the old full std::sort of all cap entries ran on EVERY
	// expansion (open pinned at beamWidth*4 = ~230k comparisons per expansion
	// — that alone collapsed the expansion rate near the finish). nth_element
	// partitions in O(n), the kept prefix is re-heapified (reversed comparator
	// — the manual heap functions keep a min-heap), the reserve order is
	// irrelevant (it is sorted on re-seed). The trim now fires once per cap
	// new entries instead of once per expansion.
	if(g_Config.m_KxPfAlgorithm == 1 && (int)m_PfAOpen.size() > PfEffortBeamWidth() * 8)
	{
		const int cap = PfEffortBeamWidth() * 4;
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		std::nth_element(m_PfAOpen.begin(), m_PfAOpen.begin() + cap, m_PfAOpen.end(), PfAOpenLess);
		m_PfABeamReserve.insert(m_PfABeamReserve.end(), m_PfAOpen.begin() + cap, m_PfAOpen.end());
		m_PfAOpen.resize(cap);
		auto HeapGreater = [](const CBotNet::KxPfOpenEnt &a, const CBotNet::KxPfOpenEnt &b) { return PfAOpenLess(b, a); };
		std::make_heap(m_PfAOpen.begin(), m_PfAOpen.end(), HeapGreater);
	}

	return 0;
}

// Build the visual path + hook segments along the parent chain (no
// re-simulation): frozen nodes replay their cached per-tick trajectory,
// single-tick nodes derive the parent.Pos → Pos segment and the grab seg.
void CBotNet::PfAStarResimulatePath(int targetIdx)
{
	if(targetIdx < 0 || targetIdx >= PfNodeTotal())
		return;

	std::vector<int> vPathIdx;
	for(int idx = targetIdx; idx >= 0;)
	{
		AStarNode Node = PfNodeGet(idx);
		vPathIdx.push_back(idx);
		idx = Node.ParentIdx;
	}
	std::reverse(vPathIdx.begin(), vPathIdx.end());

	// Chunk edges carry their stored per-tick trajectory (v6.15, same scheme
	// as the old chunk pathfinder): the renderer replays node.Traj so the
	// drawn path follows the real arc instead of a straight parent→child
	// chord that looked like a gap. 1-tick edges stay exact chords.
	std::vector<vec2> newPath;
	std::vector<PfHookSeg> newHookSegs;
	newPath.push_back(PfNodeGet(vPathIdx[0]).Core.Pos);
	for(size_t i = 1; i < vPathIdx.size(); i++)
	{
		const AStarNode node = PfNodeGet(vPathIdx[i]);
		if(!node.Traj.empty())
		{
			for(size_t k = 1; k < node.Traj.size(); k++)
				newPath.push_back(node.Traj[k]);
			for(const auto &seg : node.HookSegs)
				newHookSegs.push_back(seg);
			continue;
		}
		newPath.push_back(node.Core.Pos);
		const AStarNode parent = PfNodeGet(vPathIdx[i - 1]);
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
	if(PfNodeTotal() == 0 || m_PfAOpen.empty())
		return;

	// The open heap stores (idx, F, H) — the scan needs no node reads.
	int bestIdx = -1;
	float bestH = 1e18f;
	for(const KxPfOpenEnt &ent : m_PfAOpen)
	{
		if(ent.Idx < 0 || ent.Idx >= PfNodeTotal())
			continue;
		if(ent.H < bestH)
		{
			bestH = ent.H;
			bestIdx = ent.Idx;
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
	// v6.15: the replay-verified refined package replaces the chunked node chain.
	if(m_PfARefined)
	{
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		m_PfVPath = m_PfARefinedPath;
		m_PfVHookSegs = m_PfARefinedSegs;
		m_PfFullInputs = m_PfARefinedInputs;
		m_PfFullInputsIdx = 0;
		m_PfCurPos = m_PfARefinedEnd.Pos;
		m_PfCurVel = m_PfARefinedEnd.Vel;
		m_PfCurHookState = m_PfARefinedEnd.HookState;
		m_PfCurHookPos = m_PfARefinedEnd.HookPos;
		m_PfCurHookDir = m_PfARefinedEnd.HookDir;
		m_PfCurHookTick = m_PfARefinedEnd.HookTick;
		m_PfCurJumped = m_PfARefinedEnd.Jumped;
		m_PfCurFreezeTime = m_PfARefinedEndFreeze;
		return;
	}
	if(m_PfAGoalIdx < 0 || m_PfAGoalIdx >= PfNodeTotal())
		return;
	const AStarNode GoalNode = PfNodeGet(m_PfAGoalIdx);
	if(!PfEffortFreezeSupport() && GoalNode.FreezeChainTicks > 0)
	{
		dbg_msg("pathfinder", "reconstruction rejected: goal node carries %d freeze ticks", GoalNode.FreezeChainTicks);
		m_PfAGoalIdx = -1;
		return;
	}
	if(GoalNode.ForbiddenChainTicks > 0)
	{
		dbg_msg("pathfinder", "reconstruction rejected: goal node carries %d forbidden ticks", GoalNode.ForbiddenChainTicks);
		m_PfAGoalIdx = -1;
		return;
	}

	PfAStarResimulatePath(m_PfAGoalIdx);

	std::vector<int> vPathIdx;
	for(int idx = m_PfAGoalIdx; idx >= 0;)
	{
		AStarNode Node = PfNodeGet(idx);
		vPathIdx.push_back(idx);
		idx = Node.ParentIdx;
	}
	std::reverse(vPathIdx.begin(), vPathIdx.end());

	m_PfFullInputs.clear();
	for(size_t i = 1; i < vPathIdx.size(); i++)
	{
		const AStarNode node = PfNodeGet(vPathIdx[i]);
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
			const AStarNode parent = PfNodeGet(vPathIdx[i - 1]);
			in.m_TargetX = (int)(parent.Core.HookPos.x - parent.Core.Pos.x);
			in.m_TargetY = (int)(parent.Core.HookPos.y - parent.Core.Pos.y);
		}
		else
		{
			// Hook fire: aim at the exact coordinates the search validated
			// (dynamic hook target hit point), stored on the node itself.
			in.m_TargetX = node.AimX;
			in.m_TargetY = node.AimY;
		}
		// v6.15: chunked rollout — the held input was validated for PrimTicks
		// ticks (1 for per-tick nodes / refined edges).
		const int Repeat = pf_max(1, node.PrimTicks);
		for(int t = 0; t < Repeat; t++)
			m_PfFullInputs.push_back(in);
	}

	m_PfFullInputsIdx = 0;

	if(!vPathIdx.empty())
	{
		const AStarNode last = PfNodeGet(vPathIdx.back());
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
// v6.15 windowed per-tick refinement ("polish"). The chunked search finds a
// path fast, then every window (adjacent chunk-edge pair) is re-searched with
// an exact per-tick W=1 A* to the SAME bit-exact end state — a replacement is
// accepted only when strictly fewer ticks are found, and the assembled input
// package is replay-verified from the root before it replaces the chunk
// result (verify failure → the chunk path is kept, refinement discarded).
// =========================================================
static bool PfRefStateEq(const CBotNet::PfAState &a, const CBotNet::PfAState &b)
{
	return a.Pos.x == b.Pos.x && a.Pos.y == b.Pos.y &&
		a.Vel.x == b.Vel.x && a.Vel.y == b.Vel.y &&
		a.HookState == b.HookState &&
		a.HookPos.x == b.HookPos.x && a.HookPos.y == b.HookPos.y &&
		a.HookDir.x == b.HookDir.x && a.HookDir.y == b.HookDir.y &&
		a.HookTick == b.HookTick &&
		a.Jumped == b.Jumped && a.JumpedTotal == b.JumpedTotal;
}

int CBotNet::PfRefineWindowSearch(const PfAState &Start, int StartFreeze, bool StartGrounded, int StartTickOffset,
	const PfAState &Target, int TargetFreeze, bool TargetGrounded, bool ToFinish, int NodeBudget, int64_t &TickBudget,
	std::vector<CNetObj_PlayerInput> &OutInputs, std::vector<PfAState> &OutStates)
{
	OutInputs.clear();
	OutStates.clear();
	if(TickBudget <= 0 || NodeBudget <= 0 || !m_PfSimWorld)
		return -1;
	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	if(vFinishes.empty())
		return -1;
	const int LocalID = m_PfLocalID;
	CCollision *pColl = m_PfSimWorld->Collision();
	if(!pColl || !m_PfBaseWorld->GetCharacterById(LocalID))
		return -1;
	float hookLength = 380.0f;
	if(CCharacter *pBaseChar = m_PfBaseWorld->GetCharacterById(LocalID))
	{
		float hl = pBaseChar->GetCore().m_Tuning.m_HookLength;
		if(hl > 1.0f && hl < 10000.0f)
			hookLength = hl;
	}
	const bool freezeSupport = PfEffortFreezeSupport();
	const bool FastSim = g_Config.m_KxPfFastSim != 0;

	std::vector<AStarNode> vNodes;
	std::vector<KxPfOpenEnt> vOpen;
	KxPfGMap BestG;
	vNodes.reserve(1024);
	vOpen.reserve(1024);

	AStarNode root;
	root.Core = Start;
	root.FreezeTime = StartFreeze;
	root.Grounded = StartGrounded;
	root.G = 0.0f;
	root.H = PfAStarHeuristic(Start.Pos, PfGoalPos(vFinishes,Start.Pos));
	root.F = root.G + root.H; // W = 1: the window replacement is locally optimal
	root.ParentIdx = -1;
	root.PrimIdx = -1;
	root.PrimTicks = 0;
	root.TickOffset = 0;
	root.AimX = 0;
	root.AimY = 0;
	root.FreezeChainTicks = 0;
	root.ForbiddenChainTicks = 0;
	vNodes.push_back(std::move(root));
	BestG.Set(PfAStarKey(Start, StartFreeze, StartGrounded), 0.0f);
	PfAStarHeapPush(vOpen, {0, vNodes[0].F, vNodes[0].H});

	const uint64_t TargetKey = PfAStarKey(Target, TargetFreeze, TargetGrounded);
	int GoalIdx = -1;

	auto fnSpawn = [&](const PfAState &st, int freezeTime, int tickOffset) -> CCharacter * {
		CCharacter *pBot = PfSimWorldBot(m_PfSimWorld, m_PfBaseWorld, LocalID);
		if(!pBot)
			return nullptr;
		pBot = PfSpawnSimBot(m_PfSimWorld, LocalID, st.Pos, st.Vel,
			st.HookState, st.HookPos, st.HookDir, st.HookTick, freezeTime, st.Jumped);
		if(!pBot)
			return nullptr;
		CCharacterCore c = pBot->GetCore();
		c.m_JumpedTotal = st.JumpedTotal;
		pBot->SetCore(c);
		return pBot;
	};

	PfChunkResult res;
	std::vector<CNetObj_PlayerInput> oneInput(1);

	while(!vOpen.empty() && !m_PfThreadCancel.load(std::memory_order_relaxed) &&
		(int)vNodes.size() < NodeBudget && TickBudget > 0)
	{
		const KxPfOpenEnt ent = PfAStarHeapPop(vOpen);
		const int curIdx = ent.Idx;
		AStarNode cur = vNodes[curIdx];
		{
			const float *pG = BestG.Find(PfAStarKey(cur.Core, cur.FreezeTime, cur.Grounded));
			if(!pG || *pG < cur.G - 0.001f)
				continue; // stale
		}

		if(ToFinish)
		{
			if(PfAtGoal(cur.Core.Pos) && (freezeSupport || cur.FreezeChainTicks == 0) && cur.ForbiddenChainTicks == 0)
			{
				GoalIdx = curIdx;
				break;
			}
		}
		else if(cur.FreezeTime == TargetFreeze && PfRefStateEq(cur.Core, Target))
		{
			GoalIdx = curIdx;
			break;
		}

		if(!m_PfSimWorld)
			continue;
		m_PfSimWorld->m_Teams = m_PfBaseWorld->m_Teams;
		m_PfSimWorld->m_Core.m_vSwitchers = m_PfBaseWorld->m_Core.m_vSwitchers;
		m_PfSimWorld->m_PredictedEvents.clear();

		static const int aDirs[] = {-1, 0, 1};
		CNetObj_PlayerInput aCands[128];
		int aCandCodes[128];
		int nCands = 0;
		auto fnAddCand = [&](int dirIdx, int jump, int mode, vec2 aim) {
			if(nCands >= 128)
				return;
			mem_zero(&aCands[nCands], sizeof(CNetObj_PlayerInput));
			aCands[nCands].m_Direction = aDirs[dirIdx];
			aCands[nCands].m_Jump = jump;
			aCands[nCands].m_Hook = (mode != 0) ? 1 : 0;
			aCands[nCands].m_TargetX = (int)aim.x;
			aCands[nCands].m_TargetY = (int)aim.y;
			aCandCodes[nCands] = (1 + dirIdx) | (jump << 2) | (mode << 3);
			nCands++;
		};
		for(int d = 0; d < 3; d++)
		{
			for(int j = 0; j < 2; j++)
				fnAddCand(d, j, 0, vec2((float)aDirs[d] * 256.0f, -256.0f));
		}
		const bool hookIdle = (cur.Core.HookState == HOOK_IDLE);
		const bool hookActive = (cur.Core.HookState == HOOK_FLYING || cur.Core.HookState == HOOK_GRABBED);
		if(hookActive)
		{
			vec2 aimRel = cur.Core.HookPos - cur.Core.Pos;
			for(int d = 0; d < 3; d++)
			{
				for(int j = 0; j < 2; j++)
					fnAddCand(d, j, 1, aimRel);
			}
		}
		if(hookIdle)
		{
			const PfHookTargets *pHookTargets = PfAStarHookTargetsFor(cur.Core, pColl, hookLength);
			if(pHookTargets)
			{
				for(int s = 0; s < 36; s++)
				{
					const PfHookTarget &t = pHookTargets->aSeg[s];
					if(!t.Valid)
						continue;
					vec2 aim = t.AimPos - cur.Core.Pos;
					float d2 = aim.x * aim.x + aim.y * aim.y;
					if(d2 < 24.0f * 24.0f || d2 > hookLength * hookLength)
						continue;
					for(int d = 0; d < 3; d++)
						fnAddCand(d, 0, 2, aim);
				}
			}
		}

		for(int ci = 0; ci < nCands; ci++)
		{
			CCharacter *pBot = fnSpawn(cur.Core, cur.FreezeTime, cur.TickOffset);
			if(!pBot)
				continue;
			oneInput[0] = aCands[ci];
			PfSimulateChunk(m_PfSimWorld, LocalID, pBot, oneInput,
				m_PfSimStartTick + cur.TickOffset, res,
				m_pForbiddenGrid, PfCustomFinishSimGrid(), FastSim, m_PfUseExactGoal, m_PfExactGoal);
			TickBudget -= pf_max(1, res.ticksSimulated);
			if(res.died && !res.reachedFinish)
				continue;
			if(res.forbiddenTicks > 0)
				continue;
			if(res.freezeTicks > 0)
				continue; // conservative: windows never pass through freeze
			PfAState endCore = cur.Core;
			endCore.Pos = res.endPos;
			endCore.Vel = res.endVel;
			endCore.HookState = res.endHookState;
			endCore.HookPos = res.endHookPos;
			endCore.HookDir = res.endHookDir;
			endCore.HookTick = res.endHookTick;
			endCore.Jumped = res.endJumped;
			endCore.JumpedTotal = res.endJumpedTotal;
			const bool endGrounded = pColl->IsOnGround(endCore.Pos, CCharacterCore::PhysicalSize());
			const float newG = cur.G + (float)pf_max(1, res.ticksSimulated);
			const uint64_t key = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
			const float *pOldG = BestG.Find(key);
			if(pOldG && *pOldG <= newG)
				continue;
			BestG.Set(key, newG);
			AStarNode child;
			child.Core = endCore;
			child.FreezeTime = res.endFreezeTime;
			child.Grounded = endGrounded;
			child.G = newG;
			child.H = PfAStarHeuristic(endCore.Pos, PfGoalPos(vFinishes,endCore.Pos));
			child.F = child.G + child.H;
			child.ParentIdx = curIdx;
			child.PrimIdx = aCandCodes[ci];
			child.PrimTicks = pf_max(1, res.ticksSimulated);
			child.TickOffset = cur.TickOffset + child.PrimTicks;
			child.AimX = aCands[ci].m_TargetX;
			child.AimY = aCands[ci].m_TargetY;
			child.FreezeChainTicks = cur.FreezeChainTicks;
			child.ForbiddenChainTicks = cur.ForbiddenChainTicks;
			child.Traj.clear();
			child.Inputs.clear();
			child.HookSegs.clear();
			vNodes.push_back(std::move(child));
			PfAStarHeapPush(vOpen, {(int)vNodes.size() - 1, vNodes.back().F, vNodes.back().H});
		}
	}

	if(GoalIdx < 0)
		return -1;
	// Reconstruct the window inputs + per-tick states (root → goal).
	std::vector<int> vPath;
	for(int idx = GoalIdx; idx >= 0; idx = vNodes[idx].ParentIdx)
		vPath.push_back(idx);
	std::reverse(vPath.begin(), vPath.end());
	for(size_t k = 1; k < vPath.size(); k++)
	{
		const AStarNode &node = vNodes[vPath[k]];
		const int bits = node.PrimIdx - 1;
		const int dirIdx = bits & 3;
		const int jump = (bits >> 2) & 1;
		const int mode = (bits >> 3) & 3;
		CNetObj_PlayerInput in;
		mem_zero(&in, sizeof(in));
		in.m_Direction = dirIdx - 1;
		in.m_Jump = jump;
		in.m_Hook = (mode != 0) ? 1 : 0;
		in.m_TargetX = node.AimX;
		in.m_TargetY = node.AimY;
		const int Repeat = pf_max(1, node.PrimTicks);
		for(int t = 0; t < Repeat; t++)
		{
			OutInputs.push_back(in);
			OutStates.push_back(node.Core);
		}
	}
	return (int)vNodes[GoalIdx].G;
}

void CBotNet::PfRefineFoundPath()
{
	if(m_PfAGoalIdx < 0 || m_PfAGoalIdx >= PfNodeTotal())
		return;
	struct RefEnt
	{
		PfAState Core;
		int FreezeTime;
		bool Grounded;
		int Ticks;   // edge ticks from the previous entry
		int PrimIdx; // packed input (+1), 0 = frozen fast-forward, -1 = root
		int AimX, AimY;
		int TickOffset;
	};
	std::vector<RefEnt> vChain;
	{
		for(int idx = m_PfAGoalIdx; idx >= 0;)
		{
			const AStarNode Node = PfNodeGet(idx);
			RefEnt e;
			e.Core = Node.Core;
			e.FreezeTime = Node.FreezeTime;
			e.Grounded = Node.Grounded;
			e.Ticks = Node.PrimTicks;
			e.PrimIdx = Node.PrimIdx;
			e.AimX = Node.AimX;
			e.AimY = Node.AimY;
			e.TickOffset = 0;
			vChain.push_back(e);
			idx = Node.ParentIdx;
		}
		std::reverse(vChain.begin(), vChain.end());
	}
	if(vChain.empty() || vChain[0].PrimIdx != -1)
		return;
	for(const RefEnt &e : vChain)
	{
		if(e.PrimIdx == 0)
		{
			// frozen fast-forward edges carry per-tick zero inputs stored on the
			// node — refinement does not touch freeze routes (conservative).
			dbg_msg("pathfinder", "refinement skipped: path passes through freeze");
			return;
		}
	}

	int64_t TickBudget = 1500000;
	const int Passes = pf_clamp(g_Config.m_KxPfPolish, 0, 8);
	int SavedTicks = 0;
	for(int pass = 0; pass < Passes && TickBudget > 0; pass++)
	{
		bool Improved = false;
		size_t i = 0;
		while(i + 1 < vChain.size() && TickBudget > 0)
		{
			const size_t j = (i + 2 < vChain.size()) ? i + 2 : vChain.size() - 1;
			if(j <= i + 1)
			{
				i++;
				continue;
			}
			int windowTicks = 0;
			for(size_t k = i + 1; k <= j; k++)
				windowTicks += vChain[k].Ticks;
			if(windowTicks <= 1)
			{
				i++;
				continue;
			}
			std::vector<CNetObj_PlayerInput> vWinInputs;
			std::vector<PfAState> vWinStates;
			const bool ToFinish = (j == vChain.size() - 1);
			const int NodeBudget = pf_clamp(windowTicks * 4000, 20000, 120000);
			const int found = PfRefineWindowSearch(vChain[i].Core, vChain[i].FreezeTime, vChain[i].Grounded, vChain[i].TickOffset,
				vChain[j].Core, vChain[j].FreezeTime, vChain[j].Grounded, ToFinish, NodeBudget, TickBudget,
				vWinInputs, vWinStates);
			if(found >= 0 && found < windowTicks && (int)vWinStates.size() == found)
			{
				std::vector<RefEnt> vNew;
				vNew.reserve(vWinStates.size());
				CCollision *pColl = m_PfSimWorld ? m_PfSimWorld->Collision() : nullptr;
				for(size_t k = 0; k < vWinStates.size(); k++)
				{
					const CNetObj_PlayerInput &in = vWinInputs[k];
					RefEnt e;
					e.Core = vWinStates[k];
					e.FreezeTime = vChain[j].FreezeTime;
					e.Grounded = pColl ? pColl->IsOnGround(e.Core.Pos, CCharacterCore::PhysicalSize()) : false;
					e.Ticks = 1;
					const int dirIdx = in.m_Direction + 1;
					const int jump = in.m_Jump ? 1 : 0;
					int mode = 0;
					if(in.m_Hook)
					{
						const PfAState &Prev = (k == 0) ? vChain[i].Core : vWinStates[k - 1];
						mode = (Prev.HookState == HOOK_FLYING || Prev.HookState == HOOK_GRABBED) ? 1 : 2;
					}
					e.PrimIdx = 1 + (dirIdx | (jump << 2) | (mode << 3));
					e.AimX = in.m_TargetX;
					e.AimY = in.m_TargetY;
					e.TickOffset = 0;
					vNew.push_back(e);
				}
				vChain.erase(vChain.begin() + (i + 1), vChain.begin() + (j + 1));
				vChain.insert(vChain.begin() + (i + 1), vNew.begin(), vNew.end());
				SavedTicks += windowTicks - found;
				Improved = true;
			}
			i += 1;
		}
		if(!Improved)
			break; // fixpoint: a full pass without a strict improvement
		int Offset = 0;
		for(RefEnt &e : vChain)
		{
			e.TickOffset = Offset;
			Offset += e.Ticks;
		}
	}

	// Assemble + replay-verify the whole refined package.
	std::vector<CNetObj_PlayerInput> vInputs;
	int ChainTicks = 0;
	for(size_t k = 1; k < vChain.size(); k++)
	{
		const RefEnt &e = vChain[k];
		ChainTicks += e.Ticks;
		const int bits = e.PrimIdx - 1;
		const int dirIdx = bits & 3;
		const int jump = (bits >> 2) & 1;
		const int mode = (bits >> 3) & 3;
		CNetObj_PlayerInput in;
		mem_zero(&in, sizeof(in));
		in.m_Direction = dirIdx - 1;
		in.m_Jump = jump;
		in.m_Hook = (mode != 0) ? 1 : 0;
		in.m_TargetX = e.AimX;
		in.m_TargetY = e.AimY;
		const int Repeat = pf_max(1, e.Ticks);
		for(int t = 0; t < Repeat; t++)
			vInputs.push_back(in);
	}
	PfChunkResult res;
	if(vInputs.empty() || !PfEvalSequence(vChain[0].Core, vChain[0].FreezeTime, 0, vInputs, res, false) || !res.reachedFinish)
	{
		dbg_msg("pathfinder", "refinement discarded: replay verification failed");
		return;
	}
	m_PfARefined = true;
	m_PfARefinedInputs = std::move(vInputs);
	m_PfARefinedPath = std::move(res.traj);
	m_PfARefinedSegs = std::move(res.hookSegs);
	m_PfARefinedEnd = vChain[0].Core;
	PfSeqApplyEnd(m_PfARefinedEnd, res);
	m_PfARefinedEndFreeze = res.endFreezeTime;
	dbg_msg("pathfinder", "refinement ok: %d ticks (saved %d, chunk path %d ticks)",
		(int)m_PfARefinedInputs.size(), SavedTicks, ChainTicks);
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

	// v6.15: event-chunked A*/Beam with an automatic halving ladder. A coarse
	// chunk that cannot reach a finish is retried at half the chunk size down
	// to 1 (= the exact per-tick search), so chunking can never make a
	// findable path unfindable — it only fails slower.
	m_PfAChunkCur = pf_clamp(g_Config.m_KxPfChunk, 1, 32);
	bool FirstRun = true;
	while(!m_PfThreadCancel.load(std::memory_order_relaxed))
	{
		if(!FirstRun)
		{
			if(!PfAStarInit())
			{
				dbg_msg("pathfinder", "A* init failed (no world snapshot, character or finish tiles)");
				break;
			}
		}
		FirstRun = false;
		// Near-finish stall watch: track the best H across steps. If it sits
		// inside the terminal zone and has not moved for PF_A_STALL_SECONDS,
		// park the open set and restart from the lowest-H node as a greedy
		// walk (own visited map, no rollbacks). One attempt per search run;
		// the ladder re-init resets the latch.
		float StallBestH = 1e18f;
		auto NowMs = []() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count();
		};
		int64_t StallLastMs = NowMs();
		int result = 0;
		while(!m_PfThreadCancel.load(std::memory_order_relaxed))
		{
			if(!m_PfAGreedy && !m_PfAGreedyTried)
			{
				const float BestH = m_PfABestH.load(std::memory_order_relaxed);
				if(BestH < StallBestH - 0.01f)
				{
					StallBestH = BestH;
					StallLastMs = NowMs();
				}
				else if(BestH <= PF_A_STALL_H_TICKS &&
					NowMs() - StallLastMs >= (int64_t)(PF_A_STALL_SECONDS * 1000.0f))
				{
					// Pick the open node closest to a finish (same scan the
					// live preview uses).
					int BestIdx = -1;
					float BestEntH = 1e18f;
					KxPfOpenEnt BestEnt{};
					for(const KxPfOpenEnt &ent : m_PfAOpen)
					{
						if(ent.Idx >= 0 && ent.Idx < PfNodeTotal() && ent.H < BestEntH)
						{
							BestEntH = ent.H;
							BestEnt = ent;
							BestIdx = ent.Idx;
						}
					}
					if(BestIdx >= 0)
					{
						AStarNode Branch = PfNodeGet(BestIdx);
						m_PfAOpenBackup = std::move(m_PfAOpen);
						m_PfAOpen.clear();
						m_PfAGreedyG.Clear();
						m_PfAGreedyG.Reserve(1 << 14);
						m_PfAGreedyG.Set(PfAStarKey(Branch.Core, Branch.FreezeTime, Branch.Grounded), Branch.G);
						PfAStarHeapPush(m_PfAOpen, {BestIdx, BestEnt.H, BestEnt.H});
						m_PfAGreedy = true;
						m_PfAGreedyTried = true;
						dbg_msg("pathfinder", "near-finish stall: greedy walk from node %d (h=%.1f, open parked %zu)",
							BestIdx, BestEnt.H, m_PfAOpenBackup.size());
					}
				}
			}
			result = PfAStarStep();

			// The greedy walk exhausted its branch (open empty): restore the
			// parked open set and resume the normal search. No re-init — the
			// stall fallback must not roll the search back to the root.
			if(result != 0 && m_PfAGreedy && m_PfAOpen.empty())
			{
				m_PfAOpen = std::move(m_PfAOpenBackup);
				m_PfAOpenBackup.clear();
				m_PfAGreedy = false;
				dbg_msg("pathfinder", "greedy walk failed (no finish reachable from the best branch) — resuming search");
				result = 0;
			}

			// v6.15: compress old nodes once the active tail fills up.
			if(m_PfANodes.size() >= PF_NODE_SEG_SIZE)
				PfCompactNodeSegment();

			// Live preview: often early (so the first chunk shows up fast),
			// then throttled — the full path copy under the mutex is not free.
			if(m_PfAExpandCount <= 8 || m_PfAExpandCount % 256 == 0)
				PfAStarUpdatePreview();

			if(result != 0)
				break;
		}
		if(result == 1)
		{
			dbg_msg("pathfinder", "A* goal reached: %d expansions, %d nodes (chunk %d)",
				m_PfAExpandCount, PfNodeTotal(), m_PfAChunkCur);
			if(m_PfAChunkCur > 1 && g_Config.m_KxPfPolish > 0)
				PfRefineFoundPath();
			m_PfThreadResult.store(1);
			m_PfThreadRunning.store(false);
			return;
		}
		if(m_PfAChunkCur > 1)
		{
			dbg_msg("pathfinder", "chunk %d found no path — retrying with chunk %d",
				m_PfAChunkCur, m_PfAChunkCur / 2);
			m_PfAChunkCur /= 2;
			continue;
		}
		break;
	}

	m_PfThreadResult.store(-1);
	m_PfThreadRunning.store(false);
}

bool CBotNet::PfEvalSequence(const PfAState &cur, int curFreeze, int tickOffset,
	const std::vector<CNetObj_PlayerInput> &inSeq, PfChunkResult &out, bool FastSim)
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
		m_pForbiddenGrid, PfCustomFinishSimGrid(), FastSim, m_PfUseExactGoal, m_PfExactGoal);
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
	return -PfAStarHeuristic(res.endPos, PfGoalPos(vFinishes,res.endPos));
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
	m_PfAGoalIdx = PfNodeAppend(std::move(node));
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
	const bool FastSim = g_Config.m_KxPfFastSim != 0;
	std::vector<CNetObj_PlayerInput> one(1);
	PfChunkResult res;
	while(!done && !m_PfThreadCancel.load(std::memory_order_relaxed) && PfNodeTotal() < simBudget)
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
				parentIdx = (PfNodeGet(a).F <= PfNodeGet(b).F) ? a : b;
			}
			AStarNode parent = PfNodeGet(parentIdx);
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
				PfChunkResult resFF;
				sims++;
				if(!PfEvalSequence(parent.Core, parent.FreezeTime, parent.TickOffset, ff, resFF, false)
					|| (resFF.died && !resFF.reachedFinish) || resFF.forbiddenTicks > 0)
					continue;
				AStarNode child;
				child.Core = parent.Core;
				PfSeqApplyEnd(child.Core, resFF);
				child.FreezeTime = resFF.endFreezeTime;
				child.Grounded = pColl->IsOnGround(child.Core.Pos, CCharacterCore::PhysicalSize());
				child.G = parent.G + (float)ft;
				child.H = PfAStarHeuristic(child.Core.Pos, PfGoalPos(vFinishes,child.Core.Pos));
				child.F = child.G + W * child.H;
				child.ParentIdx = parentIdx;
				child.PrimIdx = 0;
				child.PrimTicks = ft;
				child.TickOffset = parent.TickOffset + ft;
				child.FreezeChainTicks = parent.FreezeChainTicks + ft;
				child.ForbiddenChainTicks = parent.ForbiddenChainTicks + resFF.forbiddenTicks;
				child.Traj = std::move(resFF.traj);
				child.Inputs = std::move(ff);
				child.HookSegs = std::move(resFF.hookSegs);
				const bool ChildFinish = resFF.reachedFinish;
				const int ChildFreezeChain = child.FreezeChainTicks;
				const int ChildForbiddenChain = child.ForbiddenChainTicks;
				int childIdx;
				{
					std::lock_guard<std::mutex> lock(m_PfDataMutex);
					childIdx = PfNodeAppend(std::move(child));
				}
				if(ChildFinish && (freezeSupport || ChildFreezeChain == 0) && ChildForbiddenChain == 0)
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
			const PfHookTargets *pTargets = (parent.Core.HookState == HOOK_IDLE)
				? PfAStarHookTargetsFor(parent.Core, pColl, hookLength)
				: nullptr;
			int nC = PfBuildTickCandidates(parent.Core, pColl, hookLength, pTargets, aC, aCodes);
			for(int ci = 0; ci < nC && !done; ci++)
			{
				one[0] = aC[ci];
				sims++;
				if(!PfEvalSequence(parent.Core, parent.FreezeTime, parent.TickOffset, one, res, FastSim))
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
				const float *pOldG = m_PfABestG.Find(key);
				if(pOldG && *pOldG <= newG)
					continue;
				m_PfABestG.Set(key, newG);
				AStarNode child;
				child.Core = endCore;
				child.FreezeTime = res.endFreezeTime;
				child.Grounded = endGrounded;
				child.G = newG;
				child.H = PfAStarHeuristic(endCore.Pos, PfGoalPos(vFinishes,endCore.Pos));
				child.F = child.G + W * child.H;
				child.ParentIdx = parentIdx;
				child.PrimIdx = aCodes[ci];
				child.PrimTicks = 1;
				child.TickOffset = parent.TickOffset + 1;
				child.FreezeChainTicks = parent.FreezeChainTicks + res.freezeTicks;
				child.ForbiddenChainTicks = parent.ForbiddenChainTicks + res.forbiddenTicks;
				if(child.H < m_PfABestH.load(std::memory_order_relaxed))
					m_PfABestH.store(child.H, std::memory_order_relaxed);
				const bool ChildFinish = res.reachedFinish;
				const int ChildFreezeChain = child.FreezeChainTicks;
				const int ChildForbiddenChain = child.ForbiddenChainTicks;
				int childIdx;
				{
					std::lock_guard<std::mutex> lock(m_PfDataMutex);
					childIdx = PfNodeAppend(std::move(child));
				}
				if(ChildFinish && (freezeSupport || ChildFreezeChain == 0) && ChildForbiddenChain == 0)
				{
					m_PfAGoalIdx = childIdx;
					done = true;
					break;
				}
				pop.push_back(childIdx);
			}
		}
		m_PfAExpandCount = gens;
		if(m_PfANodes.size() >= PF_NODE_SEG_SIZE)
			PfCompactNodeSegment();
		if(!done)
		{
			std::sort(pop.begin(), pop.end(), [&](int a, int b) { return PfNodeGet(a).F < PfNodeGet(b).F; });
			if((int)pop.size() > Pop)
				pop.resize(Pop);
			if(pop.empty())
				break;
		}
		if(!done && !pop.empty() && gens % 256 == 0)
			PfAStarResimulatePath(pop[0]);
		if(!done && gens % 4096 == 0 && gens > 0)
			dbg_msg("pathfinder", "RHEA progress: %d generations, %lld sims, %d nodes", gens, (long long)sims, PfNodeTotal());
	}
	if(done)
	{
		dbg_msg("pathfinder", "RHEA done: %d ticks (%lld sims, %d nodes)", PfNodeGet(m_PfAGoalIdx).TickOffset, (long long)sims, PfNodeTotal());
		m_PfThreadResult.store(1);
		m_PfThreadRunning.store(false);
		return;
	}
	dbg_msg("pathfinder", "RHEA stopped: %lld sims, %d nodes", (long long)sims, PfNodeTotal());
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
	{
		const AStarNode Root = PfNodeGet(0);
		rootNode.st = Root.Core;
		rootNode.freeze = Root.FreezeTime;
	}
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
					if(!PfEvalSequence(curSt, curFreeze, curDepth, edge, r1, false))
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
					const PfHookTargets *pTargets = (curSt.HookState == HOOK_IDLE)
						? PfAStarHookTargetsFor(curSt, pColl, hookLength)
						: nullptr;
					nCand = PfBuildTickCandidates(curSt, pColl, hookLength, pTargets, aTmp, nullptr);
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
					const PfHookTargets *pTargets = (curSt.HookState == HOOK_IDLE)
						? PfAStarHookTargetsFor(curSt, pColl, hookLength)
						: nullptr;
					PfBuildTickCandidates(curSt, pColl, hookLength, pTargets, aC, nullptr);
					int tried = (int)nodes[node].children.size();
					int pick = tried + rndInt(nCand - tried);
					std::swap(aC[tried], aC[pick]);
					std::vector<CNetObj_PlayerInput> edge(1, aC[tried]);
					PfChunkResult r1;
					sims++;
					if(!PfEvalSequence(curSt, curFreeze, curDepth, edge, r1, g_Config.m_KxPfFastSim != 0))
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
		const AStarNode Root = PfNodeGet(0);
		PfChunkResult res;
		if(PfEvalSequence(Root.Core, Root.FreezeTime, 0, winPath, res, false) && res.reachedFinish)
		{
			std::vector<vec2> fullTraj;
			fullTraj.push_back(Root.Core.Pos);
			std::vector<PfHookSeg> fullSegs;
			std::vector<CNetObj_PlayerInput> fullInputs;
			PfSeqAppend(fullTraj, fullSegs, fullInputs, res, winPath);
			int totalTicks = (int)res.traj.size() - 1;
			PfAState endSt = Root.Core;
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
