#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <unordered_set>
#include <algorithm>

// Deterministic PRNG for RHEA — replaces rand() so pathfinder produces
// the same path every time for the same input state.
static uint32_t PfRheaState = 12345;
static inline void PfRheaReset() { PfRheaState = 12345; }
static inline uint32_t PfRheaNext()
{
        PfRheaState ^= PfRheaState << 13;
        PfRheaState ^= PfRheaState >> 17;
        PfRheaState ^= PfRheaState << 5;
        return PfRheaState;
}
static inline int PfRheaRand(int max) { return (int)(PfRheaNext() % (uint32_t)max); }
static inline float PfRheaChance() { return (float)PfRheaNext() / 4294967295.0f; }

static uint64_t PfAStarKey(const CCharacterCore &core, int freezeTime, bool grounded)
{
        // v1.56.154: position at tile/8 (4px) instead of tile (32px).
        // Small ChunkSize moves <1 tile/chunk → tile dedup collapsed all
        // candidates into one key → A* gave up. 4px gives 64 sub-cells/tile.
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
                int l = 2*i+1, r = 2*i+2, s = i;
                if(l < n && nodes[heap[l]].F < nodes[heap[s]].F) s = l;
                if(r < n && nodes[heap[r]].F < nodes[heap[s]].F) s = r;
                if(s == i) break;
                std::swap(heap[s], heap[i]);
                i = s;
        }
        return top;
}

// Stuck penalty (v1.56.59): mirrors the legacy PfScoreChunk stall rule — when a
// chunk's end position is within 2 pixels of its start (bot effectively did not
// move during ChunkSize ticks), add a large fixed cost (500) to the child's G.
// This makes stall branches expensive in the A* open-set so the search prefers
// nodes that actually make progress. Gated by KxPfFineStuck (default off, same
// opt-in pattern as the other KxPfFine* penalties).
static float PfAStarStuckPenalty(const vec2 &startPos, const vec2 &endPos)
{
        if(!g_Config.m_KxPfFineStuck)
                return 0.0f;
        if(distance(endPos, startPos) < 2.0f)
                return 500.0f;
        return 0.0f;
}

float CBotNet::PfAStarHeuristic(const vec2 &pos, const vec2 &endPos)
{
        // Heuristic is gated by the same Score Method checkboxes as PfScoreChunk:
        //   KxPfScoreFlow → flow-field direction alignment (cosine with gradient)
        //                    — but for A* H we need a distance estimate, so use
        //                    flow-field distance to finish (fd * 2.0f).
        //   KxPfScoreDist → flow-field distance to finish (BFS, accounts for walls)
        //   Both on        → flow-field distance (same source, both rely on flow field)
        //   Both off       → 0 (Dijkstra, blind search)
        // Note: Dist and Flow both use flow-field distance for H. The difference
        // between them is only in PfScoreChunk (Dist = distance delta, Flow = cosine
        // alignment). For A* heuristic, both resolve to flow-field distance.
        bool flowOn = g_Config.m_KxPfScoreFlow != 0;
        bool distOn = g_Config.m_KxPfScoreDist != 0;

        if(!flowOn && !distOn)
                return 0.0f; // Dijkstra

        if(m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                int tx = pf_clamp((int)(pos.x / 32.0f), 0, m_MapWidth - 1);
                int ty = pf_clamp((int)(pos.y / 32.0f), 0, m_MapHeight - 1);
                float fd = m_PfFlowField[ty * m_MapWidth + tx];
                if(fd < 1e17f)
                        return fd * 2.0f;
        }
        return 0.0f; // flow field unavailable → blind
}

// Setup clean FutureWorld (local variable passed by caller — same as v1.55 PfGenerateChunk)
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
	if(!m_PfBaseWorld->GetCharacterById(LocalID))
		return false;
	if(m_PfFinishTiles.empty())
		return false;

        m_PfANodes.clear();
        m_PfAOpen.clear();
        m_PfABestG.clear();
        m_PfAGoalIdx = -1;
        m_PfAExpandCount = 0;

        vec2 endPos = m_PfFinishTiles[0];
        float bestDist = 1e18f;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                float d = distance(fp, m_PfCurPos);
                if(d < bestDist) { bestDist = d; endPos = fp; }
        }
        dbg_msg("pathfinder", "A* init: start=(%.0f,%.0f) goal=(%.0f,%.0f) dist=%.0f",
                m_PfCurPos.x, m_PfCurPos.y, endPos.x, endPos.y, bestDist);

	AStarNode root;
	root.Core = m_PfBaseWorld->GetCharacterById(LocalID)->GetCore();
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
        root.H = PfAStarHeuristic(m_PfCurPos, endPos);
        root.F = root.G + (float)pf_clamp(g_Config.m_KxPfAStarWeight, 1, 20) * root.H;
        root.ParentIdx = -1;
        root.PrimIdx = -1;
        root.PrimTicks = 0;
        root.TickOffset = 0;
        m_PfANodes.push_back(root);
        m_PfABestG[PfAStarKey(root.Core, root.FreezeTime, root.Grounded)] = 0.0f;
        m_PfAOpen.push_back(0);

        if(fabsf(root.Pos.x - endPos.x) < 32.0f && fabsf(root.Pos.y - endPos.y) < 32.0f)
        {
                m_PfAGoalIdx = 0;
                m_PfAPathReady = true;
                m_PfFullInputs.clear();
                m_PfFullInputsIdx = 0;
                return true;
        }
        return true;
}

int CBotNet::PfAStarStep()
{
        if(m_PfAOpen.empty())
                return -1;

        int maxNodes = pf_clamp(g_Config.m_KxPfAStarNodeBudget, 1000, 1000000);
        int W = pf_clamp(g_Config.m_KxPfAStarWeight, 1, 20);
        const int maxTicks = 4000;
        const float hookLength = 380.0f;

        if((int)m_PfANodes.size() > maxNodes)
                return -1;

        m_PfAExpandCount++;
        if(m_PfAExpandCount % 1000 == 0)
                dbg_msg("pathfinder", "A* progress: %d expansions, %zu nodes, open=%zu",
                        m_PfAExpandCount, m_PfANodes.size(), m_PfAOpen.size());

        vec2 endPos = m_PfFinishTiles[0];
        float bestDist = 1e18f;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                float d = distance(fp, m_PfCurPos);
                if(d < bestDist) { bestDist = d; endPos = fp; }
        }

        int curIdx = PfAStarHeapPop(m_PfAOpen, m_PfANodes);
        AStarNode cur = m_PfANodes[curIdx];

        uint64_t curKey = PfAStarKey(cur.Core, cur.FreezeTime, cur.Grounded);
        auto itCur = m_PfABestG.find(curKey);
        if(itCur == m_PfABestG.end() || itCur->second < cur.G - 0.001f)
                return 0;

        if(fabsf(cur.Pos.x - endPos.x) < 32.0f && fabsf(cur.Pos.y - endPos.y) < 32.0f)
        {
                m_PfAGoalIdx = curIdx;
                return 1;
        }

        if(cur.G >= (float)maxTicks)
                return 0;

	int LocalID = m_PfLocalID;

	CGameWorld FutureWorld;
	PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
        CCollision *pColl = FutureWorld.Collision();
        if(!pColl)
                return 0;

        int RootStartTick = FutureWorld.GameTick();
        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
        CCharacter *pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                          cur.Core.m_HookState, cur.Core.m_HookPos,
                                          cur.Core.m_HookDir, cur.Core.m_HookTick,
                                          cur.FreezeTime, cur.Core.m_Jumped);
        if(!pBot)
                return 0;
        // Restore m_JumpedTotal (PfSpawnSimBot copies from predicted world, not from node)
        {
                CCharacterCore c = pBot->GetCore();
                c.m_JumpedTotal = cur.Core.m_JumpedTotal;
                pBot->SetCore(c);
        }

        // FREEZE_SKIP
        if(pBot->m_FreezeTime > 0)
        {
                int ft = pBot->m_FreezeTime;
                std::vector<CNetObj_PlayerInput> emptyInputs;
                emptyInputs.resize(ft);
                for(int t = 0; t < ft; t++)
                {
                        mem_zero(&emptyInputs[t], sizeof(CNetObj_PlayerInput));
                        emptyInputs[t].m_TargetY = -1;
                }
                PfChunkResult res;
                PfSimulateChunk(&FutureWorld, LocalID, pBot, emptyInputs, RootStartTick + cur.TickOffset, res);
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
                bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                float score = PfScoreChunk(cur.Pos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);
                float newG = cur.G + (float)ft + PfAStarStuckPenalty(cur.Pos, res.endPos) - score;
                auto it = m_PfABestG.find(newKey);
                if(it != m_PfABestG.end() && it->second <= newG)
                        return 0;
                m_PfABestG[newKey] = newG;

                AStarNode child;
                child.Core = endCore;
                child.Pos = res.endPos;
                child.FreezeTime = res.endFreezeTime;
                child.Grounded = endGrounded;
                child.G = newG;
                child.H = PfAStarHeuristic(child.Pos, endPos);
                child.F = child.G + (float)W * child.H;
                child.ParentIdx = curIdx;
                child.PrimIdx = 99;
                child.PrimTicks = ft;
                child.TickOffset = cur.TickOffset + ft;
                child.Traj = res.traj;
                        child.HookSegs = res.hookSegs;
                m_PfANodes.push_back(child);
                PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                return 0;
        }

        // === Parameter sweep (same as v1.55 PfGenerateChunk) ===
        // direction × jumpTick × hook × hookAngle
        int ChunkSize = pf_clamp(g_Config.m_KxPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
        int HookAngles = pf_clamp(g_Config.m_KxPfHookAngles, 4, 32);
        static const int aDirs[] = {-1, 0, 1};

        // Precompute hook raycast (same as v1.55)
        struct HookRayResult { float angle; bool hasTarget; };
        HookRayResult aHookRays[32];
        if(cur.Core.m_HookState == HOOK_IDLE)
        {
                for(int i = 0; i < HookAngles; i++)
                {
                        float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
                        float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
                        aHookRays[i].angle = angle;
                        vec2 endPos = cur.Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
                        vec2 hitPos;
                        int hit = pColl->IntersectLineTeleHook(cur.Pos, endPos, &hitPos, nullptr);
                        if(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH))
                                aHookRays[i].hasTarget = true;
                        else
                                aHookRays[i].hasTarget = false;
                }
        }

        bool hookIdle = (cur.Core.m_HookState == HOOK_IDLE);
        bool hookActive = (cur.Core.m_HookState == HOOK_FLYING || cur.Core.m_HookState == HOOK_GRABBED);

        // === Advanced search (v1.56.14): per-tick input combinations ===
        // =====================================================
        // RHEA (Rolling Horizon Evolutionary Algorithm)
        // Replaces flat-counter Advanced Search. Evolves a population of
        // input sequences per chunk using genetic operators.
        // =====================================================
        if(g_Config.m_KxPfAdvancedSearch)
        {
                auto fnAimFor = [&](int angleIdx) -> vec2 {
                        float a = aHookRays[angleIdx].angle;
                        return vec2(cosf(a), sinf(a)) * 256.0f;
                };
                auto fnAimHooked = [&]() -> vec2 { return cur.Core.m_HookPos - cur.Pos; };

                // Generate a random valid input for one tick.
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

                // Evaluate one individual: simulate ChunkSize ticks, store result + fitness.
                auto fnEvaluate = [&](RheaIndividual &ind) {
                        if(!FutureWorld.GetCharacterById(LocalID))
                                PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                             cur.Core.m_HookState, cur.Core.m_HookPos,
                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                             cur.FreezeTime, cur.Core.m_Jumped);
                        if(!pBot)
                        {
                                ind.fitness = -1e18f;
                                ind.died = true;
                                return;
                        }
                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }
                        PfChunkResult res;
                        PfSimulateChunk(&FutureWorld, LocalID, pBot, ind.inputs, RootStartTick + cur.TickOffset, res);
                        ind.endPos = res.endPos; ind.endVel = res.endVel;
                        ind.endHookState = res.endHookState; ind.endHookPos = res.endHookPos;
                        ind.endHookDir = res.endHookDir; ind.endHookTick = res.endHookTick;
                        ind.endJumped = res.endJumped; ind.endJumpedTotal = res.endJumpedTotal;
                        ind.endFreezeTime = res.endFreezeTime;
                        ind.reachedFinish = res.reachedFinish;
                        ind.died = res.died && !res.reachedFinish;
                        ind.freezeTicks = res.freezeTicks;
                        ind.fitness = PfScoreChunk(cur.Pos, res.endPos, res.reachedFinish, ind.died, res.freezeTicks);
                        // v1.56.161 (BUG2): cache traj + hookSegs here so fnPushToAStar
                        // does NOT re-simulate (re-sim was racy → discontinuity).
                        ind.traj = res.traj;
                        ind.hookSegs = res.hookSegs;
                };

                // Push an individual as A* child node.
                auto fnPushToAStar = [&](const RheaIndividual &ind) {
                        if(ind.died && !ind.reachedFinish)
                                return;
                        CCharacterCore endCore = cur.Core;
                        endCore.m_Pos = ind.endPos; endCore.m_Vel = ind.endVel;
                        endCore.m_HookState = ind.endHookState; endCore.m_HookPos = ind.endHookPos;
                        endCore.m_HookDir = ind.endHookDir; endCore.m_HookTick = ind.endHookTick;
                        endCore.m_Jumped = ind.endJumped;
                        endCore.m_JumpedTotal = ind.endJumpedTotal;
                        bool endGrounded = pColl->IsOnGround(ind.endPos, CCharacterCore::PhysicalSize());
                        uint64_t newKey = PfAStarKey(endCore, ind.endFreezeTime, endGrounded);
                        float newG = cur.G + (float)ChunkSize + PfAStarStuckPenalty(cur.Pos, ind.endPos) - ind.fitness;
                        auto it = m_PfABestG.find(newKey);
                        if(it != m_PfABestG.end() && it->second <= newG)
                                return;
                        m_PfABestG[newKey] = newG;
                        AStarNode child;
                        child.Core = endCore; child.Pos = ind.endPos;
                        child.FreezeTime = ind.endFreezeTime; child.Grounded = endGrounded;
                        child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                        child.F = child.G + (float)W * child.H;
                        child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                        child.TickOffset = cur.TickOffset + ChunkSize;
                        // v1.56.161 (BUG2): use cached traj/hookSegs from fnEvaluate.
                        // Previously this re-simulated the chunk to obtain traj/hookSegs
                        // for rendering. But FutureWorld state drifts between fnEvaluate
                        // and fnPushToAStar (switchers, tune, hook interactions), so the
                        // re-simulation could diverge from the original:
                        //   child.Pos = ind.endPos        (from fnEvaluate)
                        //   child.Traj[end] = res.endPos  (from re-simulate)
                        // If they differ → discontinuity in m_PfVPath at this chunk
                        // boundary → "sharp line at chunk boundary" + "stale chunk
                        // hanging" (only visible at low FPS because the broken chunk
                        // flashes 1 frame at high FPS). Using the cached traj guarantees
                        // child.Pos and child.Traj[end] come from the SAME simulation.
                        // Bonus: 1 simulation per pushed individual instead of 2.
                        child.Traj = ind.traj;
                        child.HookSegs = ind.hookSegs;
                        child.Inputs = ind.inputs;
                        m_PfANodes.push_back(child);
                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                };

                int popSize = pf_clamp(g_Config.m_KxPfCandidates, CConfig::PF_CANDIDATES_MIN, CConfig::PF_CANDIDATES_MAX);
                int numGenerations = pf_clamp(g_Config.m_KxPfRheaGenerations, 1, 100);
                int pushCount = pf_clamp(g_Config.m_KxPfBacktrackCandidates, 1, popSize);

                // === Step 1: Initialize population ===
                // Shift buffer: carry population from previous chunk, shift inputs.
                if(m_PfRHAInitialized && (int)m_PfRHAPopulation.size() == popSize)
                {
                        for(auto &ind : m_PfRHAPopulation)
                        {
                                if((int)ind.inputs.size() > 1)
                                        ind.inputs.erase(ind.inputs.begin());
                                ind.inputs.push_back(fnRandomInput());
                                ind.fitness = -1e18f;
                        }
                }
                else
                {
                        m_PfRHAPopulation.clear();
                        m_PfRHAPopulation.resize(popSize);
                        for(auto &ind : m_PfRHAPopulation)
                        {
                                ind.inputs.resize(ChunkSize);
                                for(int t = 0; t < ChunkSize; t++)
                                        ind.inputs[t] = fnRandomInput();
                        }
                }

                // === Step 2: One-step-lookahead seeding ===
                // Run all combos on tick 0, keep top-K as seed individuals.
                {
                        int nDir = 3, nJump = 2, nAngle = HookAngles, nHook = 2;
                        struct SeedResult { float score; CNetObj_PlayerInput inp; };
                        std::vector<SeedResult> seeds;
                        for(int d = 0; d < nDir; d++)
                        for(int j = 0; j < nJump; j++)
                        for(int a = 0; a < nAngle; a++)
                        for(int h = 0; h < nHook; h++)
                        {
                                int hookVal = h;
                                if(hookVal == 1 && !hookIdle && !hookActive) continue;
                                if(hookVal == 1 && hookIdle && !aHookRays[a].hasTarget) continue;
                                CNetObj_PlayerInput inp;
                                mem_zero(&inp, sizeof(inp));
                                inp.m_Direction = aDirs[d];
                                inp.m_Jump = j;
                                inp.m_Hook = hookVal;
                                vec2 aim;
                                if(hookVal == 1 && hookActive) aim = fnAimHooked();
                                else if(hookVal == 1 && hookIdle) aim = fnAimFor(a);
                                else aim = vec2((float)aDirs[d] * 256.0f, -256.0f);
                                inp.m_TargetX = (int)aim.x;
                                inp.m_TargetY = (int)aim.y;
                                std::vector<CNetObj_PlayerInput> oneTick = {inp};
                                if(!FutureWorld.GetCharacterById(LocalID))
                                        PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
                                FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                     cur.Core.m_HookState, cur.Core.m_HookPos,
                                     cur.Core.m_HookDir, cur.Core.m_HookTick,
                                     cur.FreezeTime, cur.Core.m_Jumped);
                                if(!pBot) continue;
                                { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }
                                PfChunkResult res;
                                PfSimulateChunk(&FutureWorld, LocalID, pBot, oneTick, RootStartTick + cur.TickOffset, res);
                                float s = PfScoreChunk(cur.Pos, res.endPos, res.reachedFinish, res.died && !res.reachedFinish, res.freezeTicks);
                                seeds.push_back({s, inp});
                        }
                        std::sort(seeds.begin(), seeds.end(), [](const SeedResult &a, const SeedResult &b) { return a.score > b.score; });
                        int seedCount = pf_min((int)seeds.size(), popSize / 4);
                        for(int i = 0; i < seedCount; i++)
                        {
                                int idx = popSize - 1 - i;
                                m_PfRHAPopulation[idx].inputs.clear();
                                m_PfRHAPopulation[idx].inputs.resize(ChunkSize, seeds[i].inp);
                                m_PfRHAPopulation[idx].inputs[0] = seeds[i].inp;
                                for(int t = 1; t < ChunkSize; t++)
                                        m_PfRHAPopulation[idx].inputs[t] = fnRandomInput();
                                m_PfRHAPopulation[idx].fitness = -1e18f;
                        }
                }

                // === Step 3: Evaluate initial population ===
                for(auto &ind : m_PfRHAPopulation)
                        if(ind.fitness <= -1e17f)
                                fnEvaluate(ind);

                // === Step 4: Evolution loop ===
                for(int gen = 0; gen < numGenerations; gen++)
                {
                        std::sort(m_PfRHAPopulation.begin(), m_PfRHAPopulation.end(),
                                [](const RheaIndividual &a, const RheaIndividual &b) { return a.fitness > b.fitness; });
                        auto fnTournament = [&]() -> int {
                                int a = PfRheaRand(popSize / 2 + 1);
                                int b = PfRheaRand(popSize / 2 + 1);
                                return (m_PfRHAPopulation[a].fitness > m_PfRHAPopulation[b].fitness) ? a : b;
                        };
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
                                int worstIdx = popSize - 1 - i;
                                if(child.fitness > m_PfRHAPopulation[worstIdx].fitness)
                                        m_PfRHAPopulation[worstIdx] = child;
                        }
                }

                // === Step 5: Sort and push top-N to A* ===
                std::sort(m_PfRHAPopulation.begin(), m_PfRHAPopulation.end(),
                        [](const RheaIndividual &a, const RheaIndividual &b) { return a.fitness > b.fitness; });
                std::unordered_set<uint64_t> seenKeys;
                int pushed = 0;
                for(const auto &ind : m_PfRHAPopulation)
                {
                        if(pushed >= pushCount)
                                break;
                        CCharacterCore tmpCore = cur.Core;
                        tmpCore.m_Pos = ind.endPos;
                        bool endGrounded = pColl->IsOnGround(ind.endPos, CCharacterCore::PhysicalSize());
                        uint64_t key = PfAStarKey(tmpCore, ind.endFreezeTime, endGrounded);
                        if(seenKeys.count(key))
                                continue;
                        seenKeys.insert(key);
                        fnPushToAStar(ind);
                        pushed++;
                }

                m_PfRHAInitialized = true;
                return 0;
        }

        // === Beam Search (v1.57: replaces flat parameter sweep) ===
        // Per-tick beam search with per-tick input combinations.
        // Maintains a beam of K states. At each tick, expands each state by
        // all valid single-tick inputs, simulates 1 tick, scores by cumulative
        // PfScoreChunk, keeps top-K. Each tick has its OWN input combo.
        {
                struct BeamState {
                        CCharacterCore core;
                        vec2 pos;
                        int freezeTime;
                        int jumpedTotal;
                        std::vector<CNetObj_PlayerInput> inputs;
                        std::vector<vec2> traj;
                        std::vector<PfHookSeg> hookSegs;
                        float score;
                        bool died;
                        bool reachedFinish;
                        int freezeTicks;
                };

                int BeamWidth = pf_clamp(g_Config.m_KxPfBeamWidth, 4, 1024);
                int pushCount = pf_clamp(g_Config.m_KxPfBacktrackCandidates, 1, BeamWidth);
                bool hookRaysReady = (cur.Core.m_HookState == HOOK_IDLE);

                std::vector<BeamState> beam;
                {
                        BeamState s;
                        s.core = cur.Core;
                        s.pos = cur.Pos;
                        s.freezeTime = cur.FreezeTime;
                        s.jumpedTotal = cur.Core.m_JumpedTotal;
                        s.score = 0.0f;
                        s.died = false;
                        s.reachedFinish = false;
                        s.freezeTicks = 0;
                        s.traj.push_back(cur.Pos);
                        beam.push_back(std::move(s));
                }

                for(int tick = 0; tick < ChunkSize; tick++)
                {
                        std::vector<BeamState> newBeam;
                        newBeam.reserve(beam.size() * 12);

                        for(auto &st : beam)
                        {
                                if(st.died || st.reachedFinish)
                                {
                                        newBeam.push_back(st);
                                        continue;
                                }

                                bool stHookIdle = (st.core.m_HookState == HOOK_IDLE);
                                bool stHookActive = (st.core.m_HookState == HOOK_FLYING || st.core.m_HookState == HOOK_GRABBED);

                                CNetObj_PlayerInput cands[128];
                                int nCands = 0;

                                for(int d = 0; d < 3; d++)
                                {
                                        for(int j = 0; j < 2; j++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = aDirs[d];
                                                inp.m_Jump = j;
                                                inp.m_Hook = 0;
                                                inp.m_TargetX = aDirs[d] * 256;
                                                inp.m_TargetY = -256;
                                                cands[nCands++] = inp;
                                        }
                                }

                                if(stHookActive)
                                {
                                        for(int d = 0; d < 3; d++)
                                        {
                                                for(int j = 0; j < 2; j++)
                                                {
                                                        CNetObj_PlayerInput inp;
                                                        mem_zero(&inp, sizeof(inp));
                                                        inp.m_Direction = aDirs[d];
                                                        inp.m_Jump = j;
                                                        inp.m_Hook = 1;
                                                        vec2 aimRel = st.core.m_HookPos - st.pos;
                                                        inp.m_TargetX = (int)aimRel.x;
                                                        inp.m_TargetY = (int)aimRel.y;
                                                        cands[nCands++] = inp;
                                                }
                                        }
                                }

                                if(stHookIdle && hookRaysReady)
                                {
                                        for(int a = 0; a < HookAngles; a++)
                                        {
                                                if(!aHookRays[a].hasTarget)
                                                        continue;
                                                for(int d = 0; d < 3; d++)
                                                {
                                                        CNetObj_PlayerInput inp;
                                                        mem_zero(&inp, sizeof(inp));
                                                        inp.m_Direction = aDirs[d];
                                                        inp.m_Jump = 0;
                                                        inp.m_Hook = 1;
                                                        vec2 aimRel = vec2(cosf(aHookRays[a].angle), sinf(aHookRays[a].angle)) * 256.0f;
                                                        inp.m_TargetX = (int)aimRel.x;
                                                        inp.m_TargetY = (int)aimRel.y;
                                                        cands[nCands++] = inp;
                                                }
                                        }
                                }

                                for(int ci = 0; ci < nCands; ci++)
                                {
                                        if(!FutureWorld.GetCharacterById(LocalID))
                                                PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
                                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset + tick + 1;
                                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, st.pos, st.core.m_Vel,
                                                             st.core.m_HookState, st.core.m_HookPos,
                                                             st.core.m_HookDir, st.core.m_HookTick,
                                                             st.freezeTime, st.core.m_Jumped);
                                        if(!pBot)
                                                continue;
                                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = st.jumpedTotal; pBot->SetCore(c); }

                                        PfChunkResult res;
                                        std::vector<CNetObj_PlayerInput> oneInput = {cands[ci]};
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, oneInput,
                                                        RootStartTick + cur.TickOffset + tick, res);

                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        BeamState ns;
                                        ns.core = st.core;
                                        ns.core.m_Pos = res.endPos;
                                        ns.core.m_Vel = res.endVel;
                                        ns.core.m_HookState = res.endHookState;
                                        ns.core.m_HookPos = res.endHookPos;
                                        ns.core.m_HookDir = res.endHookDir;
                                        ns.core.m_HookTick = res.endHookTick;
                                        ns.core.m_Jumped = res.endJumped;
                                        ns.core.m_JumpedTotal = res.endJumpedTotal;
                                        ns.jumpedTotal = res.endJumpedTotal;
                                        ns.pos = res.endPos;
                                        ns.freezeTime = res.endFreezeTime;
                                        ns.died = res.died;
                                        ns.reachedFinish = res.reachedFinish;
                                        ns.freezeTicks = st.freezeTicks + res.freezeTicks;
                                        ns.inputs = st.inputs;
                                        ns.inputs.push_back(cands[ci]);
                                        ns.traj = st.traj;
                                        ns.traj.push_back(res.endPos);
                                        ns.hookSegs = st.hookSegs;
                                        for(const auto &hs : res.hookSegs)
                                                ns.hookSegs.push_back(hs);
                                        ns.score = PfScoreChunk(cur.Pos, res.endPos, ns.reachedFinish,
                                                                ns.died && !ns.reachedFinish, ns.freezeTicks);

                                        newBeam.push_back(std::move(ns));

                                        if(ns.reachedFinish)
                                                break;
                                }
                        }

                        if(newBeam.empty())
                                break;

                        std::sort(newBeam.begin(), newBeam.end(),
                                [](const BeamState &a, const BeamState &b) { return a.score > b.score; });

                        if((int)newBeam.size() > BeamWidth)
                                newBeam.resize(BeamWidth);

                        beam = std::move(newBeam);

                        bool allTerminal = true;
                        for(auto &s : beam)
                        {
                                if(!s.died && !s.reachedFinish)
                                {
                                        allTerminal = false;
                                        break;
                                }
                        }
                        if(allTerminal)
                                break;
                }

                std::unordered_set<uint64_t> pushedKeys;
                int pushed = 0;
                for(auto &st : beam)
                {
                        if(pushed >= pushCount)
                                break;
                        if(st.died && !st.reachedFinish)
                                continue;
                        if(st.inputs.empty())
                                continue;

                        bool endGrounded = pColl->IsOnGround(st.pos, CCharacterCore::PhysicalSize());
                        uint64_t newKey = PfAStarKey(st.core, st.freezeTime, endGrounded);
                        if(pushedKeys.count(newKey))
                                continue;
                        pushedKeys.insert(newKey);

                        float newG = cur.G + (float)ChunkSize + PfAStarStuckPenalty(cur.Pos, st.pos) - st.score;
                        auto it = m_PfABestG.find(newKey);
                        if(it != m_PfABestG.end() && it->second <= newG)
                                continue;
                        m_PfABestG[newKey] = newG;

                        AStarNode child;
                        child.Core = st.core;
                        child.Pos = st.pos;
                        child.FreezeTime = st.freezeTime;
                        child.Grounded = endGrounded;
                        child.G = newG;
                        child.H = PfAStarHeuristic(child.Pos, endPos);
                        child.F = child.G + (float)W * child.H;
                        child.ParentIdx = curIdx;
                        child.PrimIdx = 0;
                        child.PrimTicks = ChunkSize;
                        child.TickOffset = cur.TickOffset + ChunkSize;
                        child.Traj = st.traj;
                        child.HookSegs = st.hookSegs;
                        child.Inputs = st.inputs;
                        m_PfANodes.push_back(child);
                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                        pushed++;
                }
        }

        return 0;

#if 0 // Legacy flat parameter sweep — replaced by beam search above
        for(int dirIdx = 0; dirIdx < 3; dirIdx++)
        {
                int dir = aDirs[dirIdx];
                for(int jumpTick = -1; jumpTick < ChunkSize; jumpTick++)
                {
                        for(int hookOn = 0; hookOn <= 1; hookOn++)
                        {
                                if(hookOn == 1 && !hookIdle && !hookActive)
                                        continue;

                                if(hookOn == 1 && hookIdle)
                                {
                                        // Fire new hook: sweep over hook angles (with raycast filter)
                                        for(int angleIdx = 0; angleIdx < HookAngles; angleIdx++)
                                        {
                                                if(!aHookRays[angleIdx].hasTarget)
                                                        continue;
                                                float hookAngle = aHookRays[angleIdx].angle;

                                                // Build inputs (same as v1.55)
                                                std::vector<CNetObj_PlayerInput> inSeq;
                                                inSeq.reserve(ChunkSize);
                                                vec2 aimRel = vec2(cosf(hookAngle), sinf(hookAngle)) * 256.0f;
                                                for(int t = 0; t < ChunkSize; t++)
                                                {
                                                        CNetObj_PlayerInput inp;
                                                        mem_zero(&inp, sizeof(inp));
                                                        inp.m_Direction = dir;
                                                        inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                        inp.m_Hook = 1;
                                                        inp.m_TargetX = (int)aimRel.x;
                                                        inp.m_TargetY = (int)aimRel.y;
                                                        inSeq.push_back(inp);
                                                }

                                                // Re-spawn bot
                                                if(!FutureWorld.GetCharacterById(LocalID))
                                                        PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
                                                FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                                pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                                     cur.Core.m_HookState, cur.Core.m_HookPos,
                                                     cur.Core.m_HookDir, cur.Core.m_HookTick,
                                                     cur.FreezeTime, cur.Core.m_Jumped);
                                                if(!pBot)
                                                        continue;
                                                { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                                                PfChunkResult res;
                                                PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                                                if(res.died && !res.reachedFinish)
                                                        continue;

                                                CCharacterCore endCore = cur.Core;
                                                endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                                                endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                                                endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                                                endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                                                bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                                                uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                                                float newG = cur.G + (float)ChunkSize + PfAStarStuckPenalty(cur.Pos, res.endPos);
                                                auto it = m_PfABestG.find(newKey);
                                                if(it != m_PfABestG.end() && it->second <= newG)
                                                        continue;
                                                m_PfABestG[newKey] = newG;

                                                AStarNode child;
                                                child.Core = endCore; child.Pos = res.endPos;
                                                child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                                                child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                                                child.F = child.G + (float)W * child.H;
                                                child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                                                child.TickOffset = cur.TickOffset + ChunkSize;
                                                child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                                                m_PfANodes.push_back(child);
                                                PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                                        }
                                }
                                else if(hookOn == 1 && hookActive)
                                {
                                        // Hold existing hook (no angle sweep needed)
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 1;
                                                vec2 aimRel = cur.Core.m_HookPos - cur.Pos;
                                                inp.m_TargetX = (int)aimRel.x;
                                                inp.m_TargetY = (int)aimRel.y;
                                                inSeq.push_back(inp);
                                        }

                                        if(!FutureWorld.GetCharacterById(LocalID))
                                                PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
                                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                             cur.Core.m_HookState, cur.Core.m_HookPos,
                                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                                             cur.FreezeTime, cur.Core.m_Jumped);
                                        if(!pBot)
                                                continue;
                                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        CCharacterCore endCore = cur.Core;
                                        endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                                        endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                                        endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                                        endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                                        bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                                        uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                                        float newG = cur.G + (float)ChunkSize + PfAStarStuckPenalty(cur.Pos, res.endPos);
                                        auto it = m_PfABestG.find(newKey);
                                        if(it != m_PfABestG.end() && it->second <= newG)
                                                continue;
                                        m_PfABestG[newKey] = newG;

                                        AStarNode child;
                                        child.Core = endCore; child.Pos = res.endPos;
                                        child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                                        child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                                        child.F = child.G + (float)W * child.H;
                                        child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                                        child.TickOffset = cur.TickOffset + ChunkSize;
                                        child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                                        m_PfANodes.push_back(child);
                                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                                }
                                else // hookOn == 0: no hook
                                {
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 0;
                                                inp.m_TargetX = dir * 256;
                                                inp.m_TargetY = -256;
                                                inSeq.push_back(inp);
                                        }

                                        if(!FutureWorld.GetCharacterById(LocalID))
                                                PfAStarSetupWorld(&FutureWorld, m_PfBaseWorld, LocalID);
                                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                             cur.Core.m_HookState, cur.Core.m_HookPos,
                                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                                             cur.FreezeTime, cur.Core.m_Jumped);
                                        if(!pBot)
                                                continue;
                                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        CCharacterCore endCore = cur.Core;
                                        endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                                        endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                                        endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                                        endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                                        bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                                        uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                                        float newG = cur.G + (float)ChunkSize + PfAStarStuckPenalty(cur.Pos, res.endPos);
                                        auto it = m_PfABestG.find(newKey);
                                        if(it != m_PfABestG.end() && it->second <= newG)
                                                continue;
                                        m_PfABestG[newKey] = newG;

                                        AStarNode child;
                                        child.Core = endCore; child.Pos = res.endPos;
                                        child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                                        child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                                        child.F = child.G + (float)W * child.H;
                                        child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                                        child.TickOffset = cur.TickOffset + ChunkSize;
                                        child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                                        m_PfANodes.push_back(child);
                                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                                }
                        }
                }
        }
#endif // end of legacy flat parameter sweep
}

static void PfAStarBuildInputs(int primI, int pTicks, float aHookX[8], float aHookY[8],
                               std::vector<CNetObj_PlayerInput> &outSeq)
{
        int pDir = 0, pJumpTick = -1;
        bool pHookFire = false, pHookHold = false;
        int pHookAngleIdx = 0;
        bool isFreezeSkip = (primI == 99);
        if(!isFreezeSkip)
        {
                switch(primI)
                {
                        case 0: pDir = -1; break;
                        case 1: pDir = +1; break;
                        case 2: pDir = -1; pJumpTick = 0; break;
                        case 3: pDir = +1; pJumpTick = 0; break;
                        case 4: pDir = 0; pJumpTick = 0; break;
                        case 5: case 6: case 7: case 8:
                        case 9: case 10: case 11: case 12:
                                pHookFire = true; pHookAngleIdx = primI - 5; break;
                        case 13: pHookHold = true; break;
                        default: break;
                }
        }
        outSeq.clear();
        outSeq.reserve(pTicks);
        for(int t = 0; t < pTicks; t++)
        {
                CNetObj_PlayerInput inp;
                mem_zero(&inp, sizeof(inp));
                inp.m_Direction = pDir;
                inp.m_Jump = (t == pJumpTick) ? 1 : 0;
                inp.m_Hook = (pHookFire || pHookHold) ? 1 : 0;
                if(pHookFire)
                {
                        inp.m_TargetX = (int)(aHookX[pHookAngleIdx] * 256.0f);
                        inp.m_TargetY = (int)(aHookY[pHookAngleIdx] * 256.0f);
                }
                else
                {
                        inp.m_TargetX = pDir * 256;
                        inp.m_TargetY = -256;
                }
                outSeq.push_back(inp);
        }
}

void CBotNet::PfAStarResimulatePath(int targetIdx)
{
	if(targetIdx < 0 || targetIdx >= (int)m_PfANodes.size())
		return;

	// Walk parents from target to root
	std::vector<int> vPathIdx;
	for(int idx = targetIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
		vPathIdx.push_back(idx);
	std::reverse(vPathIdx.begin(), vPathIdx.end());

	// Build path in a local buffer, then swap under lock (minimises mutex hold time)
	std::vector<vec2> newPath;
	newPath.push_back(m_PfANodes[vPathIdx[0]].Pos);
	for(size_t i = 1; i < vPathIdx.size(); i++)
	{
		AStarNode &node = m_PfANodes[vPathIdx[i]];
		for(size_t k = 1; k < node.Traj.size(); k++)
			newPath.push_back(node.Traj[k]);
	}

	std::lock_guard<std::mutex> lock(m_PfDataMutex);
	m_PfVPath = std::move(newPath);
}

void CBotNet::PfAStarUpdatePreview()
{
        if(m_PfANodes.empty())
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

void CBotNet::PfAStarReconstruct()
{
	PfThreadStop();
	if(m_PfAGoalIdx < 0)
		return;

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
// v1.57: Threaded pathfinder worker
// =========================================================

void CBotNet::PfThreadWorker()
{
	if(!PfAStarInit())
	{
		m_PfThreadResult.store(-1);
		m_PfThreadRunning.store(false);
		return;
	}

	while(!m_PfThreadCancel.load(std::memory_order_relaxed))
	{
		int result = PfAStarStep();

		PfAStarUpdatePreview();

		if(m_PfAExpandCount % 1000 == 0)
			dbg_msg("pathfinder", "A* thread progress: %d expansions, %zu nodes, open=%zu",
				m_PfAExpandCount, m_PfANodes.size(), m_PfAOpen.size());

		if(result == 1)
		{
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

// Render m_PfVPath (copy of RenderPackage with m_PfVPath substituted).
