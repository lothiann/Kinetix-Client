#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

static uint64_t PfAStarKey(const CCharacterCore &core, int freezeTime, bool grounded)
{
        int tx = (int)(core.m_Pos.x / 32.0f);
        int ty = (int)(core.m_Pos.y / 32.0f);
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

float CBotNet::PfAStarHeuristic(const vec2 &pos, const vec2 &endPos)
{
        // Heuristic is gated by the same Score Method checkboxes as PfScoreChunk:
        //   ClPfScoreFlow → flow-field direction alignment (cosine with gradient)
        //                    — but for A* H we need a distance estimate, so use
        //                    flow-field distance to finish (fd * 2.0f).
        //   ClPfScoreDist → flow-field distance to finish (BFS, accounts for walls)
        //   Both on        → flow-field distance (same source, both rely on flow field)
        //   Both off       → 0 (Dijkstra, blind search)
        // Note: Dist and Flow both use flow-field distance for H. The difference
        // between them is only in PfScoreChunk (Dist = distance delta, Flow = cosine
        // alignment). For A* heuristic, both resolve to flow-field distance.
        bool flowOn = g_Config.m_ClPfScoreFlow != 0;
        bool distOn = g_Config.m_ClPfScoreDist != 0;

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
static void PfAStarSetupWorld(CGameWorld *pWorld, CGameClient *pGame, int LocalID)
{
        pWorld->CopyWorld(&pGame->m_PredictedWorld);
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
        CGameClient *pGame = GameClient();
        if(!pGame || !pGame->m_Snap.m_pLocalInfo)
                return false;
        int LocalID = pGame->m_Snap.m_LocalClientId;
        if(LocalID < 0)
                return false;
        if(!pGame->m_PredictedWorld.GetCharacterById(LocalID))
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
        root.Core = pGame->m_PredictedWorld.GetCharacterById(LocalID)->GetCore();
        root.Core.m_Pos = m_PfCurPos;
        root.Core.m_Vel = m_PfCurVel;
        root.Core.m_HookState = m_PfCurHookState;
        root.Core.m_HookPos = m_PfCurHookPos;
        root.Core.m_HookDir = m_PfCurHookDir;
        root.Core.m_HookTick = m_PfCurHookTick;
        root.Core.m_Jumped = m_PfCurJumped;
        root.Pos = m_PfCurPos;
        root.FreezeTime = m_PfCurFreezeTime;
        CCollision *pColl = pGame->Collision();
        root.Grounded = pColl ? pColl->IsOnGround(root.Pos, CCharacterCore::PhysicalSize()) : false;
        root.G = 0.0f;
        root.H = PfAStarHeuristic(m_PfCurPos, endPos);
        root.F = root.G + (float)pf_clamp(g_Config.m_ClPfAStarWeight, 1, 20) * root.H;
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

        int maxNodes = pf_clamp(g_Config.m_ClPfAStarNodeBudget, 1000, 1000000);
        int W = pf_clamp(g_Config.m_ClPfAStarWeight, 1, 20);
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

        CGameClient *pGame = GameClient();
        if(!pGame)
                return -1;
        int LocalID = pGame->m_Snap.m_LocalClientId;

        CGameWorld FutureWorld;
        PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
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
                float newG = cur.G + (float)ft;
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
        int ChunkSize = pf_clamp(g_Config.m_ClPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
        int HookAngles = pf_clamp(g_Config.m_ClPfHookAngles, 4, 32);
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
        // Each tick in the chunk gets its OWN input (dir/jump/angle/hook), not one
        // input for the whole chunk. A flat counter over (dir,jump,angle,hook)
        // advances across ticks AND across candidates without restart, so every
        // tick of every candidate gets a unique combination.
        // One candidate = ChunkSize consecutive picks from the counter.
        if(g_Config.m_ClPfAdvancedSearch)
        {
                // Push helper: simulate one inSeq, push bestG + A* open heap.
                auto fnPushAdvanced = [&](std::vector<CNetObj_PlayerInput> &inSeq) {
                        if(!FutureWorld.GetCharacterById(LocalID))
                                PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                             cur.Core.m_HookState, cur.Core.m_HookPos,
                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                             cur.FreezeTime, cur.Core.m_Jumped);
                        if(!pBot)
                                return;
                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                        PfChunkResult res;
                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                        if(res.died && !res.reachedFinish)
                                return;

                        CCharacterCore endCore = cur.Core;
                        endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                        endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                        endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                        endCore.m_Jumped = res.endJumped;
                        endCore.m_JumpedTotal = res.endJumpedTotal;
                        bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                        uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                        float newG = cur.G + (float)ChunkSize;
                        auto it = m_PfABestG.find(newKey);
                        if(it != m_PfABestG.end() && it->second <= newG)
                                return;
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
                };

                auto fnAimFor = [&](int angleIdx) -> vec2 {
                        float a = aHookRays[angleIdx].angle;
                        return vec2(cosf(a), sinf(a)) * 256.0f;
                };
                auto fnAimHooked = [&]() -> vec2 { return cur.Core.m_HookPos - cur.Pos; };

                const int nDir = 3;
                const int nJump = 2;
                const int nAngle = HookAngles;
                const int nHook = 2;
                const long totalCombos = (long)nDir * nJump * nAngle * nHook; // = 12 * HookAngles
                // Number of candidates: bounded by ClPfBacktrackCandidates, by available
                // combos (ceil(totalCombos / ChunkSize)), and by A* node budget.
                const int maxNodes = pf_clamp(g_Config.m_ClPfAStarNodeBudget, 1000, 1000000);
                const int dynAdvMax = (int)((totalCombos + ChunkSize - 1) / ChunkSize); // ceil(combos/CS)
                int numCandidates = pf_clamp(g_Config.m_ClPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynAdvMax);
                if(numCandidates > maxNodes) numCandidates = maxNodes;
                if(numCandidates < 1) numCandidates = 1;

                for(int cand = 0; cand < numCandidates; cand++)
                {
                        std::vector<CNetObj_PlayerInput> inSeq;
                        inSeq.reserve(ChunkSize);
                        // ChunkSize consecutive picks from counter, continuing across candidates.
                        for(int t = 0; t < ChunkSize; t++)
                        {
                                long idx = ((long)cand * ChunkSize + t) % totalCombos;
                                int hookVal = (int)(idx % nHook); idx /= nHook;
                                int angleIdx = (int)(idx % nAngle); idx /= nAngle;
                                int jumpVal = (int)(idx % nJump); idx /= nJump;
                                int dirVal = (int)(idx % nDir);

                                // Skip impossible hook fires (no hook target / not idle nor active).
                                if(hookVal == 1 && !hookIdle && !hookActive)
                                        hookVal = 0; // fall back to no-hook for this tick
                                if(hookVal == 1 && hookIdle && !aHookRays[angleIdx].hasTarget)
                                        hookVal = 0; // ray missed — no hook for this tick

                                CNetObj_PlayerInput inp;
                                mem_zero(&inp, sizeof(inp));
                                inp.m_Direction = aDirs[dirVal];
                                inp.m_Jump = jumpVal;
                                inp.m_Hook = hookVal;
                                vec2 aim;
                                if(hookVal == 1 && hookActive)
                                        aim = fnAimHooked();
                                else if(hookVal == 1 && hookIdle)
                                        aim = fnAimFor(angleIdx);
                                else
                                        aim = vec2((float)aDirs[dirVal] * 256.0f, -256.0f);
                                inp.m_TargetX = (int)aim.x;
                                inp.m_TargetY = (int)aim.y;
                                inSeq.push_back(inp);
                        }
                        fnPushAdvanced(inSeq);
                }

                return 0;
        }

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
                                                        PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
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
                                                float newG = cur.G + (float)ChunkSize;
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
                                                PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
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
                                        float newG = cur.G + (float)ChunkSize;
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
                                                PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
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
                                        float newG = cur.G + (float)ChunkSize;
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

        return 0;
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

        // Build m_PfVPath from cached Traj in each node (no re-simulation — fast)
        m_PfVPath.clear();
        m_PfVPath.push_back(m_PfANodes[vPathIdx[0]].Pos);
        for(size_t i = 1; i < vPathIdx.size(); i++)
        {
                AStarNode &node = m_PfANodes[vPathIdx[i]];
                for(size_t k = 1; k < node.Traj.size(); k++)
                        m_PfVPath.push_back(node.Traj[k]);
        }
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

// Render m_PfVPath (copy of RenderPackage with m_PfVPath substituted).
