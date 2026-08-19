#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// PfClearSearchState (v1.56.165, BUG4): release ALL A*/RHEA search buffers.
//
// Root cause of BUG4 (crash on pathfinder restart after idle):
//   The FINISHED→IDLE transitions in clickgui.cpp (Pathfinding button click)
//   and ConPfLive (kx_pf_live 0) were clearing ONLY m_PfVPath — the heavy
//   search buffers (m_PfANodes with per-node Traj/Inputs/HookSegs vectors,
//   m_PfRHAPopulation with per-individual inputs/traj/hookSegs vectors)
//   were left alive across the entire idle period.
//
//   During the search those buffers grew to hundreds of MB (RHEA can hold
//   popSize * (inputs + traj + hookSegs) per individual × thousands of nodes
//   in m_PfANodes). Their internal vector headers live next to other heap
//   allocations; any heap-metadata corruption elsewhere (e.g. an OOB write
//   in a sibling allocation during the long sim) lands inside those vector
//   control blocks. When the user clicks "Pathfinding" again to restart,
//   PfResetRun() → m_PfRHAPopulation.clear() walks the corrupted vector
//   headers and crashes in ~vector<CNetObj_PlayerInput> reading
//   [r8-8] = 0x00000004FFFFFFF8 (the 0x500000000 in r8 is a corrupted
//   _Myfirst pointer — small integer value that landed in the high 32 bits).
//
//   Disassembly confirms it: the crash path runs the deallocation branch
//   only when capacity*40 >= 0x1000 (i.e. capacity >= 103). A well-formed
//   RheaIndividual::inputs has capacity <= ChunkSize (max 50) → never hits
//   that branch. Only a corrupted vector with garbage capacity value does.
//
// Fix: free the search buffers the moment the state machine leaves the
//   search lifecycle (RUNNING/FINISHED) — not only when PfResetRun snaps
//   to the player position. This keeps no stale heavy buffers alive across
//   idle, so nothing is left to corrupt.
//
// Why a separate method (not inlined into PfResetRun):
//   PfResetRun needs the player's current position + map grid loaded
//   before it can rebuild the flow field and seed m_PfVPath. The IDLE
//   transition can run at any time (including right after a map vote
//   before OnMapLoad finishes) — calling PfResetRun there would log
//   spurious "no predicted character" / "map grid failed" errors and
//   skip the cleanup we actually need. PfClearSearchState is safe to
//   call from any state — it touches only the search buffers, never
//   the player or the map.
void CBotNet::PfClearSearchState()
{
        m_PfANodes.clear();
        m_PfAOpen.clear();
        m_PfABestG.clear();
        m_PfAGoalIdx = -1;
        m_PfAStarted = false;
        m_PfAPathReady = false;
        m_PfAExpandCount = 0;
        m_PfRHAInitialized = false;
        m_PfRHAPopulation.clear();
        m_PfFullInputs.clear();
        m_PfFullInputsIdx = 0;
        m_PfGoActive = false;
        m_PfGoIdx = 0;
}

void CBotNet::PfResetRun()
{
        m_PfVPath.clear();
        m_PfChunkCount = 0;
        m_PfTickCounter = 0;

        CGameClient *pGame = GameClient();
        if(!pGame || !pGame->m_Snap.m_pLocalInfo)
        {
                dbg_msg("pathfinder", "PfResetRun: no game or no local info");
                return;
        }

        // Make sure the map grid is loaded BEFORE building the flow field
        // (PfComputeFlowField needs m_pMapGrid / m_pFrontGrid).
        if(!m_MapGridLoaded)
                LoadMapGrid();
        if(!m_MapGridLoaded)
        {
                dbg_msg("pathfinder", "PfResetRun: map grid failed to load");
                return;
        }

        int LocalID = pGame->m_Snap.m_LocalClientId;
        if(LocalID < 0)
        {
                dbg_msg("pathfinder", "PfResetRun: no local client id");
                return;
        }
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalID);
        if(!pChar)
        {
                dbg_msg("pathfinder", "PfResetRun: no predicted character");
                return;
        }

        const CCharacterCore &Core = pChar->GetCore();
        m_PfStartPos = Core.m_Pos;
        m_PfCurPos = Core.m_Pos;
        m_PfCurVel = Core.m_Vel;
        m_PfCurHookState = Core.m_HookState;
        m_PfCurHookPos = Core.m_HookPos;
        m_PfCurHookDir = Core.m_HookDir;
        m_PfCurHookTick = Core.m_HookTick;
        m_PfCurFreezeTime = pChar->m_FreezeTime;
        m_PfCurJumped = Core.m_Jumped;
        m_PfTotalFreezeTicks = 0;
        m_PfBacktrackIdx = 0;
        for(int i = 0; i < PF_BACKTRACK_DEPTH; i++)
                m_PfBacktrack[i].Reset();

        // Reset all A*/RHEA search buffers (shared with PfClearSearchState —
        // see BUG4 comment below). Rebuild is mandatory here because PfResetRun
        // starts a fresh search from the player's CURRENT position.
        PfClearSearchState();

        m_PfVPath.push_back(Core.m_Pos);

        // Force flow-field rebuild (clear cached finish tiles so PfComputeFlowField
        // re-scans the map).
        if(m_PfFlowField)
        {
                delete[] m_PfFlowField;
                m_PfFlowField = nullptr;
        }
        m_PfFinishTiles.clear();
        PfComputeFlowField();

        dbg_msg("pathfinder", "PfResetRun: start=(%.0f,%.0f) finish_tiles=%d flowfield=%s",
                Core.m_Pos.x, Core.m_Pos.y, (int)m_PfFinishTiles.size(),
                m_PfFlowField ? "ok" : "null");
}

// kx_pf_play: apply next path tick to real player input.
// Called from CControls::SnapInput each tick when m_PfGoActive is true.
// Advances m_PfGoIdx; auto-stops when end of m_PfFullInputs reached.
// v1.56.204: Aim (TargetX/Y) is only applied on the rising edge of m_Hook
// (new hook press), matching Fake Aim's fire/hookPress condition. On all
// other ticks, aim is left untouched so Fake Aim can coexist with PfGo.
bool CBotNet::ApplyPfGoInput(CNetObj_PlayerInput *pInput)
{
        // Reset before early return — otherwise stopping PfGo leaves the flag
        // true and permanently blocks Fake Aim generation.
        m_PfGoAimedThisTick = false;
        if(!m_PfGoActive || !pInput)
                return false;
        if(m_PfGoIdx >= m_PfFullInputs.size())
        {
                // Playback finished.
                m_PfGoActive = false;
                dbg_msg("pathfinder", "kx_pf_play: playback finished (%d ticks)", (int)m_PfFullInputs.size());
                return false;
        }
        const CNetObj_PlayerInput &In = m_PfFullInputs[m_PfGoIdx];
        // Apply direction, jump, hook — full input except fire/weapon (let player keep their own).
        pInput->m_Direction = In.m_Direction;
        pInput->m_Jump = In.m_Jump;
        pInput->m_Hook = In.m_Hook;
        // v1.56.204: Only apply aim on hook rising edge (new hook), like Fake Aim's hookPress.
        // This prevents PfGo from overwriting aim every tick, allowing Fake Aim to work.
        // v1.56.207: Store aim offset so Robot Aim can remember the PfGo target.
        if(In.m_Hook != 0 && m_PfGoPrevHook == 0)
        {
                pInput->m_TargetX = In.m_TargetX;
                pInput->m_TargetY = In.m_TargetY;
                m_PfGoAimOffset = vec2((float)In.m_TargetX, (float)In.m_TargetY);
                m_PfGoAimedThisTick = true;
        }
        m_PfGoPrevHook = In.m_Hook;
        m_PfGoIdx++;
        return true;
}

// TODO(bug1-resolved): The original DDNet engine's freeze detection
// (HandleTiles + DDRacePostCoreTick) does NOT reliably fire inside the
// pathfinder's cloned CGameWorld sim. PfCheckFreeze (kinetix_internal.h)
// was added as a 1:1 manual copy of that logic and is run after each sim
// tick in PfSimulateChunk to guarantee freeze is detected.
//
// After a FULL clean recompile (not incremental), the original engine
// logic started working correctly — strongly suggesting the earlier
// failures were due to an incremental-build artifact (stale .o / .pdb
// picked up an outdated struct layout), NOT a logic bug.
//
// Investigate later: confirm whether the original HandleTiles path can
// now be relied on in sim (so PfCheckFreeze can be removed), or keep
// PfCheckFreeze as a permanent safety net.

// Multi-source BFS: distance field to the NEAREST finish tile.

void CBotNet::PfComputeFlowField()
{
        if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
                return;

        int Size = m_MapWidth * m_MapHeight;
        if(!m_PfFlowField || (int)m_PfFinishTiles.empty())
        {
                if(m_PfFlowField)
                        delete[] m_PfFlowField;
                m_PfFlowField = new float[Size];
                m_PfFinishTiles.clear();
                for(int ty = 0; ty < m_MapHeight; ty++)
                {
                        for(int tx = 0; tx < m_MapWidth; tx++)
                        {
                                int idx = ty * m_MapWidth + tx;
                                if(m_pMapGrid[idx] == TILE_FINISH || m_pFrontGrid[idx] == TILE_FINISH)
                                        m_PfFinishTiles.push_back(vec2(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f));
                        }
                }
        }

        // === Phase 1: BFS from finish, STRICTLY skip freeze tiles. ===
        // Freeze tiles stay 1e18 (unreachable). If the bot's start position is
        // reachable in Phase 1, we're done — path exists without freeze.
        // Otherwise, Phase 2 takes over with a full reset.
        for(int i = 0; i < Size; i++)
                m_PfFlowField[i] = 1e18f;

        if(m_PfFinishTiles.empty())
                return;

        const float SQRT2 = 1.4142135623730951f;
        const int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        const int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

        auto fnIsFreeze = [&](int tx, int ty) -> bool {
                int idx = ty * m_MapWidth + tx;
                return m_pMapGrid[idx] == TILE_FREEZE || m_pMapGrid[idx] == TILE_DFREEZE ||
                       m_pFrontGrid[idx] == TILE_FREEZE || m_pFrontGrid[idx] == TILE_DFREEZE;
        };

        std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                int tx = (int)(fp.x / 32.0f);
                int ty = (int)(fp.y / 32.0f);
                if(!IsTileWalkable(tx, ty))
                        continue;
                m_PfFlowField[ty * m_MapWidth + tx] = 0.0f;
                open.push({0.0f, 0.0f, ty, tx});
        }

        while(!open.empty())
        {
                PfNode cur = open.top();
                open.pop();
                int cIdx = cur.r * m_MapWidth + cur.c;
                if(cur.g > m_PfFlowField[cIdx])
                        continue;

                for(int d = 0; d < 8; d++)
                {
                        int nr = cur.r + dr[d];
                        int nc = cur.c + dc[d];
                        if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
                                continue;
                        if(!IsTileWalkable(nc, nr))
                                continue;
                        if(fnIsFreeze(nc, nr))
                                continue; // Phase 1: strictly skip freeze
                        if(abs(dr[d]) + abs(dc[d]) == 2)
                        {
                                if(!IsTileWalkable(cur.c, cur.r + dr[d]) || !IsTileWalkable(cur.c + dc[d], cur.r))
                                        continue;
                        }
                        float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 : 1.0f;
                        float newG = cur.g + moveCost;
                        int nIdx = nr * m_MapWidth + nc;
                        if(newG < m_PfFlowField[nIdx])
                        {
                                m_PfFlowField[nIdx] = newG;
                                open.push({newG, newG, nr, nc});
                        }
                }
        }

        // === Phase 1 reachability check: did Phase 1 reach the bot's start? ===
        // m_PfStartPos is set in PfResetRun (bot pos at run start). If it's still
        // (0,0) or out of map, skip Phase 2 (no start to check).
        int startTX = (int)(m_PfStartPos.x / 32.0f);
        int startTY = (int)(m_PfStartPos.y / 32.0f);
        bool phase1ReachedStart = false;
        if(startTX >= 0 && startTY >= 0 && startTX < m_MapWidth && startTY < m_MapHeight &&
           (m_PfStartPos.x != 0.0f || m_PfStartPos.y != 0.0f))
        {
                phase1ReachedStart = m_PfFlowField[startTY * m_MapWidth + startTX] < 1e17f;
        }

        // === Phase 2: full reset + BFS with freeze passable (cost 1). ===
        // Only if Phase 1 did NOT reach the bot's start (path requires freeze).
        if(!phase1ReachedStart)
        {
                for(int i = 0; i < Size; i++)
                        m_PfFlowField[i] = 1e18f;

                std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open2;
                for(const vec2 &fp : m_PfFinishTiles)
                {
                        int tx = (int)(fp.x / 32.0f);
                        int ty = (int)(fp.y / 32.0f);
                        if(!IsTileWalkable(tx, ty))
                                continue;
                        m_PfFlowField[ty * m_MapWidth + tx] = 0.0f;
                        open2.push({0.0f, 0.0f, ty, tx});
                }

                while(!open2.empty())
                {
                        PfNode cur = open2.top();
                        open2.pop();
                        int cIdx = cur.r * m_MapWidth + cur.c;
                        if(cur.g > m_PfFlowField[cIdx])
                                continue;

                        for(int d = 0; d < 8; d++)
                        {
                                int nr = cur.r + dr[d];
                                int nc = cur.c + dc[d];
                                if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
                                        continue;
                                if(!IsTileWalkable(nc, nr))
                                        continue;
                                // Phase 2: freeze passable at cost 1 (same as normal tiles).
                                if(abs(dr[d]) + abs(dc[d]) == 2)
                                {
                                        if(!IsTileWalkable(cur.c, cur.r + dr[d]) || !IsTileWalkable(cur.c + dc[d], cur.r))
                                                continue;
                                }
                                float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 : 1.0f;
                                float newG = cur.g + moveCost;
                                int nIdx = nr * m_MapWidth + nc;
                                if(newG < m_PfFlowField[nIdx])
                                {
                                        m_PfFlowField[nIdx] = newG;
                                        open2.push({newG, newG, nr, nc});
                                }
                        }
                }
        }

        // === Score field: 4× resolution BFS (8px cells) for distance reduction. ===
        // Same two-phase BFS as flow field but with 4× finer grid.
        // Phase 1: skip freeze tiles (freeze = unreachable).
        // Phase 2: if Phase 1 didn't reach start, redo with freeze passable (cost 1).
        m_PfScoreFieldW = m_MapWidth * 4;
        m_PfScoreFieldH = m_MapHeight * 4;
        int scoreSize = m_PfScoreFieldW * m_PfScoreFieldH;
        if(m_PfScoreField)
                delete[] m_PfScoreField;
        m_PfScoreField = new float[scoreSize];
        for(int i = 0; i < scoreSize; i++)
                m_PfScoreField[i] = 1e18f;

        // Check if an 8px sub-cell is walkable (not solid, not death).
        auto subWalkable = [&](int scx, int scy) -> bool {
                int tx = scx / 4;
                int ty = scy / 4;
                if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
                        return false;
                int idx = ty * m_MapWidth + tx;
                unsigned char t = m_pMapGrid[idx];
                unsigned char ft = m_pFrontGrid[idx];
                return t != TILE_SOLID && t != TILE_DEATH && ft != TILE_DEATH;
        };

        // Check if an 8px sub-cell is a freeze tile (mirrors fnIsFreeze from flow field).
        auto subIsFreeze = [&](int scx, int scy) -> bool {
                int tx = scx / 4;
                int ty = scy / 4;
                if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
                        return false;
                int idx = ty * m_MapWidth + tx;
                return m_pMapGrid[idx] == TILE_FREEZE || m_pMapGrid[idx] == TILE_DFREEZE ||
                       m_pFrontGrid[idx] == TILE_FREEZE || m_pFrontGrid[idx] == TILE_DFREEZE;
        };

        // ── Phase 1: BFS strictly skip freeze tiles. ──
        std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> scoreOpen;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                int scx = (int)(fp.x / 8.0f);
                int scy = (int)(fp.y / 8.0f);
                if(!subWalkable(scx, scy))
                        continue;
                m_PfScoreField[scy * m_PfScoreFieldW + scx] = 0.0f;
                scoreOpen.push({0.0f, 0.0f, scy, scx});
        }
        while(!scoreOpen.empty())
        {
                PfNode cur = scoreOpen.top();
                scoreOpen.pop();
                int cIdx = cur.r * m_PfScoreFieldW + cur.c;
                if(cur.g > m_PfScoreField[cIdx])
                        continue;
                for(int d = 0; d < 8; d++)
                {
                        int nr = cur.r + dr[d];
                        int nc = cur.c + dc[d];
                        if(nc < 0 || nr < 0 || nc >= m_PfScoreFieldW || nr >= m_PfScoreFieldH)
                                continue;
                        if(!subWalkable(nc, nr))
                                continue;
                        if(subIsFreeze(nc, nr))
                                continue; // Phase 1: strictly skip freeze
                        if(abs(dr[d]) + abs(dc[d]) == 2)
                        {
                                if(!subWalkable(cur.c, cur.r + dr[d]) || !subWalkable(cur.c + dc[d], cur.r))
                                        continue;
                        }
                        float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 : 1.0f;
                        float newG = cur.g + moveCost;
                        int nIdx = nr * m_PfScoreFieldW + nc;
                        if(newG < m_PfScoreField[nIdx])
                        {
                                m_PfScoreField[nIdx] = newG;
                                scoreOpen.push({newG, newG, nr, nc});
                        }
                }
        }

        // ── Phase 1 reachability check: did Phase 1 reach the bot's start? ──
        int startSCX = (int)(m_PfStartPos.x / 8.0f);
        int startSCY = (int)(m_PfStartPos.y / 8.0f);
        bool scorePhase1ReachedStart = false;
        if(startSCX >= 0 && startSCY >= 0 && startSCX < m_PfScoreFieldW && startSCY < m_PfScoreFieldH &&
           (m_PfStartPos.x != 0.0f || m_PfStartPos.y != 0.0f))
        {
                scorePhase1ReachedStart = m_PfScoreField[startSCY * m_PfScoreFieldW + startSCX] < 1e17f;
        }

        // ── Phase 2: full reset + BFS with freeze passable (cost 1). ──
        // Only if Phase 1 did NOT reach the bot's start (path requires freeze).
        if(!scorePhase1ReachedStart)
        {
                for(int i = 0; i < scoreSize; i++)
                        m_PfScoreField[i] = 1e18f;

                std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> scoreOpen2;
                for(const vec2 &fp : m_PfFinishTiles)
                {
                        int scx = (int)(fp.x / 8.0f);
                        int scy = (int)(fp.y / 8.0f);
                        if(!subWalkable(scx, scy))
                                continue;
                        m_PfScoreField[scy * m_PfScoreFieldW + scx] = 0.0f;
                        scoreOpen2.push({0.0f, 0.0f, scy, scx});
                }
                while(!scoreOpen2.empty())
                {
                        PfNode cur = scoreOpen2.top();
                        scoreOpen2.pop();
                        int cIdx = cur.r * m_PfScoreFieldW + cur.c;
                        if(cur.g > m_PfScoreField[cIdx])
                                continue;
                        for(int d = 0; d < 8; d++)
                        {
                                int nr = cur.r + dr[d];
                                int nc = cur.c + dc[d];
                                if(nc < 0 || nr < 0 || nc >= m_PfScoreFieldW || nr >= m_PfScoreFieldH)
                                        continue;
                                if(!subWalkable(nc, nr))
                                        continue;
                                // Phase 2: freeze passable at cost 1 (same as normal tiles).
                                if(abs(dr[d]) + abs(dc[d]) == 2)
                                {
                                        if(!subWalkable(cur.c, cur.r + dr[d]) || !subWalkable(cur.c + dc[d], cur.r))
                                                continue;
                                }
                                float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 : 1.0f;
                                float newG = cur.g + moveCost;
                                int nIdx = nr * m_PfScoreFieldW + nc;
                                if(newG < m_PfScoreField[nIdx])
                                {
                                        m_PfScoreField[nIdx] = newG;
                                        scoreOpen2.push({newG, newG, nr, nc});
                                }
                        }
                }
        }
}
// Simplified: flowProgress + finishBonus - deathPenalty - freezePenalty - stallPenalty.

float CBotNet::PfScoreChunk(const vec2 &startPos, const vec2 &endPos, bool reachedFinish, bool died,
                            int freezeTicks) const
{
        float score = 0.0f;
        const float INF = 1e17f; // unreachable threshold (m_PfFlowField uses 1e18)

        // Score method: flow-field alignment (how well the chunk's movement
        // direction matches the flow-field direction toward finish).
        // Gated by KxPfScoreFlow. Uses cosine of angle between movement vector
        // and flow-field gradient at start tile: +1 = along flow (toward finish),
        // -1 = against flow (away from finish), 0 = perpendicular.
        if(g_Config.m_KxPfScoreFlow && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                vec2 move = endPos - startPos;
                float moveLen = sqrtf(move.x * move.x + move.y * move.y);
                if(moveLen > 0.001f)
                {
                        int sx = pf_clamp((int)(startPos.x / 32.0f), 0, m_MapWidth - 1);
                        int sy = pf_clamp((int)(startPos.y / 32.0f), 0, m_MapHeight - 1);
                        // Flow direction at start tile = gradient of m_PfFlowField
                        // (points toward finish, i.e. toward smaller distance).
                        float fd = m_PfFlowField[sy * m_MapWidth + sx];
                        if(fd < INF)
                        {
                                float fdx_m = (sx > 0) ? m_PfFlowField[sy * m_MapWidth + (sx - 1)] : fd;
                                float fdx_p = (sx < m_MapWidth - 1) ? m_PfFlowField[sy * m_MapWidth + (sx + 1)] : fd;
                                float fdy_m = (sy > 0) ? m_PfFlowField[(sy - 1) * m_MapWidth + sx] : fd;
                                float fdy_p = (sy < m_MapHeight - 1) ? m_PfFlowField[(sy + 1) * m_MapWidth + sx] : fd;
                                // Unreachable neighbors → treat as equal (no spurious gradient).
                                if(fdx_m >= INF) fdx_m = fd;
                                if(fdx_p >= INF) fdx_p = fd;
                                if(fdy_m >= INF) fdy_m = fd;
                                if(fdy_p >= INF) fdy_p = fd;
                                // Gradient: negative of distance increase direction.
                                // gx = fdx_m - fdx_p (points toward smaller distance).
                                float gx = fdx_m - fdx_p;
                                float gy = fdy_m - fdy_p;
                                float gLen = sqrtf(gx * gx + gy * gy);
                                if(gLen > 0.001f)
                                {
                                        // Cosine of angle between move and flow direction.
                                        float cosA = (move.x * gx + move.y * gy) / (moveLen * gLen);
                                        // cosA in [-1, +1]. Scale to score weight.
                                        score += cosA * 10.0f;
                                }
                        }
                }
        }

        // Score method: distance reduction — uses 4× resolution score field (8px cells).
        // Sub-tile precision: movement within one 32px tile still produces non-zero score.
        // Walls accounted for (BFS through walkable sub-cells).
        if(g_Config.m_KxPfScoreDist && m_PfScoreField && m_PfScoreFieldW > 0 && m_PfScoreFieldH > 0)
        {
                int sx = pf_clamp((int)(startPos.x / 8.0f), 0, m_PfScoreFieldW - 1);
                int sy = pf_clamp((int)(startPos.y / 8.0f), 0, m_PfScoreFieldH - 1);
                int ex = pf_clamp((int)(endPos.x / 8.0f), 0, m_PfScoreFieldW - 1);
                int ey = pf_clamp((int)(endPos.y / 8.0f), 0, m_PfScoreFieldH - 1);
                float dStart = m_PfScoreField[sy * m_PfScoreFieldW + sx];
                float dEnd = m_PfScoreField[ey * m_PfScoreFieldW + ex];
                if(dStart < INF && dEnd < INF)
                        score += (dStart - dEnd) * 10.0f;
                else if(dEnd < INF && dStart >= INF)
                        score += 50.0f; // escaped an unreachable cell
                else if(dEnd >= INF && dStart < INF)
                        score -= 50.0f; // moved INTO an unreachable cell
        }

        // Finish bonus
        if(reachedFinish)
                score += 1000000.0f;

        // Death penalty
        if(died)
                score -= 10000.0f;

        // Dynamic freeze penalty (increases over time to discourage freeze routes)
        if(g_Config.m_KxPfFineFreeze)
                score -= freezeTicks * (10.0f + m_PfTotalFreezeTicks / 8.0f);

        // Stall penalty
        if(distance(endPos, startPos) < 2.0f)
                score -= 500.0f;

        return score;
}

// Per-update chunk step.

void CBotNet::UpdatePathfinder()
{
        if(m_PfState != PF_STATE_RUNNING)
                return;
        if(!m_MapGridLoaded)
                LoadMapGrid();
        if(!m_MapGridLoaded)
        {
                m_PfState = PF_STATE_FINISHED;
                return;
        }

        if(m_PfFlowField == nullptr || m_PfFinishTiles.empty())
                PfComputeFlowField();
        if(m_PfFinishTiles.empty())
        {
                m_PfState = PF_STATE_FINISHED;
                return;
        }

        // ── Playback phase: A* found a path, emit ChunkSize ticks per call ──
        if(m_PfAPathReady)
        {
                m_PfTickCounter++;
                int perf = pf_clamp(g_Config.m_KxPfPerf, 1, 100);
                int tickInterval = 6000 / perf;
                if(tickInterval < 1)
                        tickInterval = 1;
                if(m_PfTickCounter < tickInterval)
                        return;
                m_PfTickCounter = 0;

                if(m_PfFullInputsIdx >= m_PfFullInputs.size())
                {
                        m_PfState = PF_STATE_FINISHED;
                        return;
                }
                int ChunkSize = pf_clamp(g_Config.m_KxPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
                size_t endIdx = m_PfFullInputsIdx + (size_t)ChunkSize;
                if(endIdx > m_PfFullInputs.size())
                        endIdx = m_PfFullInputs.size();
                m_PfFullInputsIdx = endIdx;
                m_PfChunkCount++;
                if(m_PfFullInputsIdx >= m_PfFullInputs.size())
                        m_PfState = PF_STATE_FINISHED;
                return;
        }

        // ── A* phase ──
        // Perf throttle: 6000/perf frames between expansions (same as v1.55)
        m_PfTickCounter++;
        int perf = pf_clamp(g_Config.m_KxPfPerf, 1, 100);
        int tickInterval = 6000 / perf;
        if(tickInterval < 1)
                tickInterval = 1;
        if(m_PfTickCounter < tickInterval)
                return;
        m_PfTickCounter = 0;

        if(!m_PfAStarted)
        {
                if(!PfAStarInit())
                {
                        m_PfState = PF_STATE_FINISHED;
                        return;
                }
                m_PfAStarted = true;
                PfAStarUpdatePreview();
                return;
        }

        int result = PfAStarStep();
        if(result == 1)
        {
                PfAStarReconstruct();
                m_PfAPathReady = true;
                m_PfFullInputsIdx = 0;
                dbg_msg("pathfinder", "A* path found: %zu ticks, %zu nodes, %d expansions",
                        m_PfFullInputs.size(), m_PfANodes.size(), m_PfAExpandCount);
                return;
        }
        else if(result < 0)
        {
                dbg_msg("pathfinder", "A* failed: %d expansions, %zu nodes",
                        m_PfAExpandCount, m_PfANodes.size());
                m_PfState = PF_STATE_FINISHED;
                return;
        }
        PfAStarUpdatePreview();
}

// Try an alternative candidate from the backtrack buffer.
// Returns true if a candidate was applied, false if the buffer is exhausted.
bool CBotNet::PfBacktrack()
{
        while(m_PfBacktrackIdx > 0)
        {
                m_PfBacktrackIdx--;
                PfBacktrackEntry &entry = m_PfBacktrack[m_PfBacktrackIdx];

                // Try next candidate (usedCandidateIdx + 1)
                int nextIdx = entry.usedCandidateIdx + 1;
                if(nextIdx >= entry.numCandidates || nextIdx >= (int)entry.candidates.size())
                        continue; // no more candidates at this level, pop further

                // Restore anchor state
                m_PfCurPos = entry.pos;
                m_PfCurVel = entry.vel;
                m_PfCurHookPos = entry.hookPos;
                m_PfCurHookDir = entry.hookDir;
                m_PfCurHookState = entry.hookState;
                m_PfCurHookTick = entry.hookTick;
                m_PfCurJumped = entry.jumped;
                m_PfCurFreezeTime = entry.freezeTime;
                m_PfTotalFreezeTicks = entry.totalFreezeTicks;

                // Truncate trajectory and restore chunk count to the active
                // trajectory length (active chunks, not total attempts — otherwise
                // backtracking burns the budget on retries).
                m_PfVPath.resize(entry.vPathSize);
                m_PfChunkCount = entry.chunkCount;

                // Apply the alternative candidate.
                // TODO(v1.56.153): on low FPS (8 FPS during Advanced Search), cand.traj[0]
                // may not match m_PfVPath.back() (gap from incomplete simulation race).
                // This causes the rendered visual path to break/jump at this chunk boundary.
                // Need a continuity check: if distance(m_PfVPath.back(), cand.traj[0]) > 4px,
                // bridge with cand.endPos before appending traj. Currently unfixed —
                // happens rarely, only on low FPS, doesn't affect pathfinding logic.
                PfBacktrackEntry::Candidate &cand = entry.candidates[nextIdx];
                for(size_t i = 1; i < cand.traj.size(); i++)
                        m_PfVPath.push_back(cand.traj[i]);

                m_PfCurPos = cand.endPos;
                m_PfCurVel = cand.endVel;
                m_PfCurHookPos = cand.endHookPos;
                m_PfCurHookDir = cand.endHookDir;
                m_PfCurHookState = cand.endHookState;
                m_PfCurHookTick = cand.endHookTick;
                m_PfCurJumped = cand.endJumped;
                m_PfCurFreezeTime = cand.endFreezeTime;
                m_PfTotalFreezeTicks += cand.freezeTicks;

                entry.usedCandidateIdx = nextIdx;
                m_PfBacktrackIdx++; // push back (we're using this entry now)

                dbg_msg("pathfinder", "backtrack: using candidate %d at depth %d", nextIdx, m_PfBacktrackIdx - 1);
                return true;
        }

        return false; // backtrack exhausted
}

// =========================================================
// State-Lattice A* (v1.56)
// =========================================================

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

void CBotNet::RenderPathfinderPath()
{
        if(m_PfVPath.empty())
                return;

        // CBotNet is registered in m_vpAll BEFORE m_MapLayersBackground, so the
        // world projection (normally set up by CMapRenderer via MapScreenToInterface)
        // is NOT yet active when OnRender is called.  Without this, m_PfVPath
        // coordinates (world space) would be drawn against the UI/default screen
        // and end up off-screen / invisible.  Set the world projection explicitly
        // here so the trajectory renders in the same space as the map.
        CGameClient *pGame = GameClient();
        if(pGame)
                Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

        Graphics()->TextureClear();

        ColorRGBA BaseColor = ColorRGBA(KxLineColor(KX_LINE_PATHFINDER), true);
        float ConfigAlpha = KxLineAlpha(KX_LINE_PATHFINDER);
        int LineSize = KxLineSize(KX_LINE_PATHFINDER);

        // If only the start point exists (no chunk generated yet, or all chunks
        // failed), render a small marker so the user sees the pathfinder is
        // active but stuck — instead of an invisible empty path.
        if(m_PfVPath.size() == 1)
        {
                Graphics()->QuadsBegin();
                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha);
                vec2 p = m_PfVPath[0];
                float s = 8.0f;
                IGraphics::CFreeformItem Quad(
                        p.x - s, p.y - s, p.x + s, p.y - s,
                        p.x - s, p.y + s, p.x + s, p.y + s);
                Graphics()->QuadsDrawFreeform(&Quad, 1);
                Graphics()->QuadsEnd();
                return;
        }

        // === Show flow field (vector arrows pointing toward finish) ===
        if(g_Config.m_KxPfShowField && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                int tx0 = pf_clamp((int)(pGame->m_Camera.m_Center.x / 32.0f) - 30, 0, m_MapWidth - 1);
                int ty0 = pf_clamp((int)(pGame->m_Camera.m_Center.y / 32.0f) - 20, 0, m_MapHeight - 1);
                int tx1 = pf_clamp(tx0 + 60, 0, m_MapWidth - 1);
                int ty1 = pf_clamp(ty0 + 40, 0, m_MapHeight - 1);
                Graphics()->LinesBegin();
                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.3f);
                for(int ty = ty0; ty <= ty1; ty++)
                {
                        for(int tx = tx0; tx <= tx1; tx++)
                        {
                                float fd = m_PfFlowField[ty * m_MapWidth + tx];
                                if(fd >= 1e17f || fd == 0.0f)
                                        continue; // unreachable or already at finish

                                // Gradient-based direction (360°, not 8-directional).
                                // Unreachable neighbors (>= 1e17, e.g. walls/freeze) are
                                // treated as equal to the current tile so they don't
                                // create a spurious "away from wall" gradient component.
                                float fdx_m = (tx > 0) ? m_PfFlowField[ty * m_MapWidth + (tx - 1)] : fd;
                                float fdx_p = (tx < m_MapWidth - 1) ? m_PfFlowField[ty * m_MapWidth + (tx + 1)] : fd;
                                float fdy_m = (ty > 0) ? m_PfFlowField[(ty - 1) * m_MapWidth + tx] : fd;
                                float fdy_p = (ty < m_MapHeight - 1) ? m_PfFlowField[(ty + 1) * m_MapWidth + tx] : fd;
                                if(fdx_m >= 1e17f) fdx_m = fd;
                                if(fdx_p >= 1e17f) fdx_p = fd;
                                if(fdy_m >= 1e17f) fdy_m = fd;
                                if(fdy_p >= 1e17f) fdy_p = fd;

                                float gx = fdx_m - fdx_p; // negative = finish is to the left
                                float gy = fdy_m - fdy_p; // negative = finish is above
                                float glen = sqrtf(gx * gx + gy * gy);
                                if(glen < 0.001f)
                                        continue;
                                vec2 dir = vec2(gx / glen, gy / glen);

                                // Arrow centered on tile: from center-dir*6 to center+dir*6
                                float cx = tx * 32.0f + 16.0f;
                                float cy = ty * 32.0f + 16.0f;
                                float half = 6.0f;
                                vec2 start = vec2(cx, cy) - dir * half;
                                vec2 end = vec2(cx, cy) + dir * half;
                                vec2 perp = vec2(-dir.y, dir.x) * 4.0f;
                                vec2 arrowBack = end - dir * 5.0f;

                                // Main line
                                IGraphics::CLineItem Main(start.x, start.y, end.x, end.y);
                                Graphics()->LinesDraw(&Main, 1);
                                // Arrowhead: two small lines
                                IGraphics::CLineItem A1(end.x, end.y, arrowBack.x + perp.x, arrowBack.y + perp.y);
                                Graphics()->LinesDraw(&A1, 1);
                                IGraphics::CLineItem A2(end.x, end.y, arrowBack.x - perp.x, arrowBack.y - perp.y);
                                Graphics()->LinesDraw(&A2, 1);
                        }
                }
                Graphics()->LinesEnd();
        }

        // === Show other branches (semi-transparent, using trajectory settings) ===
        if(g_Config.m_KxPfShowBranches && m_PfANodes.size() > 1)
        {
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
                if(LineSize > 0)
                {
                        std::vector<IGraphics::CFreeformItem> vQuads;
                        for(size_t i = 1; i < m_PfANodes.size(); i++)
                        {
                                AStarNode &node = m_PfANodes[i];
                                if(node.ParentIdx < 0 || node.Traj.size() < 2)
                                        continue;
                                for(size_t k = 1; k < node.Traj.size(); k++)
                                {
                                        vec2 p0 = node.Traj[k - 1];
                                        vec2 p1 = node.Traj[k];
                                        vec2 Dir = normalize(p1 - p0);
                                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
                                        vQuads.emplace_back(
                                                p0.x - Perp.x, p0.y - Perp.y,
                                                p0.x + Perp.x, p0.y + Perp.y,
                                                p1.x - Perp.x, p1.y - Perp.y,
                                                p1.x + Perp.x, p1.y + Perp.y);
                                }
                        }
                        Graphics()->QuadsBegin();
                        for(size_t i = 0; i < vQuads.size(); i++)
                        {
                                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.2f);
                                Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
                        }
                        Graphics()->QuadsEnd();
                }
                else
                {
                        Graphics()->LinesBegin();
                        Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.2f);
                        for(size_t i = 1; i < m_PfANodes.size(); i++)
                        {
                                AStarNode &node = m_PfANodes[i];
                                if(node.ParentIdx < 0 || node.Traj.size() < 2)
                                        continue;
                                for(size_t k = 1; k < node.Traj.size(); k++)
                                {
                                        IGraphics::CLineItem Line(node.Traj[k - 1], node.Traj[k]);
                                        Graphics()->LinesDraw(&Line, 1);
                                }
                        }
                        Graphics()->LinesEnd();
                }
        }

        // === Show hooks (blue lines, using trajectory settings for width) ===
        // Draws ALL hook fire/hold segments along the best path, captured per-tick
        // in PfSimulateChunk (node.HookSegs). This shows multiple hook cycles inside
        // a single chunk (e.g. Advanced search per-tick hook fire), not just the
        // final HOOK_GRABBED state.
        if(g_Config.m_KxPfShowHooks && m_PfANodes.size() > 0)
        {
                // Find best path (same as preview)
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
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
                if(LineSize > 0 && bestIdx >= 0)
                {
                        std::vector<IGraphics::CFreeformItem> vQuads;
                        for(int idx = bestIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
                        {
                                AStarNode &node = m_PfANodes[idx];
                                for(const auto &seg : node.HookSegs)
                                {
                                        vec2 p0 = seg.teePos;
                                        vec2 p1 = seg.hookPos;
                                        vec2 d = p1 - p0;
                                        float len = length(d);
                                        if(len < 0.001f)
                                                continue;
                                        vec2 Dir = d / len;
                                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
                                        vQuads.emplace_back(
                                                p0.x - Perp.x, p0.y - Perp.y,
                                                p0.x + Perp.x, p0.y + Perp.y,
                                                p1.x - Perp.x, p1.y - Perp.y,
                                                p1.x + Perp.x, p1.y + Perp.y);
                                }
                        }
                        Graphics()->QuadsBegin();
                        for(size_t i = 0; i < vQuads.size(); i++)
                        {
                                Graphics()->SetColor(0.2f, 0.4f, 1.0f, ConfigAlpha);
                                Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
                        }
                        Graphics()->QuadsEnd();
                }
                else if(bestIdx >= 0)
                {
                        Graphics()->LinesBegin();
                        Graphics()->SetColor(0.2f, 0.4f, 1.0f, ConfigAlpha);
                        for(int idx = bestIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
                        {
                                AStarNode &node = m_PfANodes[idx];
                                for(const auto &seg : node.HookSegs)
                                {
                                        IGraphics::CLineItem Line(seg.teePos, seg.hookPos);
                                        Graphics()->LinesDraw(&Line, 1);
                                }
                        }
                        Graphics()->LinesEnd();
                }
        }

        // === Main path render (with optional speed gradient) ===
        // Speed gradient: full rainbow via HSL hue sweep.
        // Speed range [0, 30] px/tick mapped to hue [0°, 300°]:
        //   0    px/tick → red    (hue 0)
        //   5    px/tick → orange (hue 50)
        //   10   px/tick → yellow (hue 100)
        //   15   px/tick → green  (hue 150)
        //   20   px/tick → cyan   (hue 200)
        //   25   px/tick → blue   (hue 250)
        //   30+  px/tick → purple (hue 300)
        // 30 px/tick covers typical DDNet speeds (run ~10, jump ~12, fall ~17, hook-swing 20-30).
        // S=1, L=0.5 → pure rainbow colors, smooth interpolation across the whole range.
        auto fnSpeedColor = [](float speed) -> ColorRGBA {
                const float MAX_SPEED = 30.0f;
                float t = std::clamp(speed / MAX_SPEED, 0.0f, 1.0f);
                float hue = t * 300.0f; // 0..300 degrees
                return color_cast<ColorRGBA>(ColorHSLA(hue / 360.0f, 1.0f, 0.5f, true));
        };

        if(LineSize > 0)
        {
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
                Graphics()->QuadsBegin();
                for(size_t i = 1; i < m_PfVPath.size(); i++)
                {
                        vec2 p0 = m_PfVPath[i - 1];
                        vec2 p1 = m_PfVPath[i];
                        vec2 Dir = normalize(p1 - p0);
                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
                        IGraphics::CFreeformItem Quad(
                                p0.x - Perp.x, p0.y - Perp.y,
                                p0.x + Perp.x, p0.y + Perp.y,
                                p1.x - Perp.x, p1.y - Perp.y,
                                p1.x + Perp.x, p1.y + Perp.y);

                        if(g_Config.m_KxPfShowSpeed)
                        {
                                // Speed at start and end of this segment.
                                float speed0 = (i > 1) ? distance(m_PfVPath[i - 2], m_PfVPath[i - 1]) : distance(p0, p1);
                                float speed1 = distance(p0, p1);
                                ColorRGBA c0 = fnSpeedColor(speed0);
                                ColorRGBA c1 = fnSpeedColor(speed1);
                                // Per-vertex: start vertices get c0, end vertices get c1
                                Graphics()->SetColor4(
                                        ColorRGBA(c0.r, c0.g, c0.b, ConfigAlpha), // TL (start)
                                        ColorRGBA(c0.r, c0.g, c0.b, ConfigAlpha), // TR (start)
                                        ColorRGBA(c1.r, c1.g, c1.b, ConfigAlpha), // BL (end)
                                        ColorRGBA(c1.r, c1.g, c1.b, ConfigAlpha)); // BR (end)
                        }
                        else
                        {
                                // v1.56.210: per-segment gradient color (Rainbow+Gradient both ON).
                                ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_PATHFINDER, (int)i - 1), true);
                                Graphics()->SetColor(segCol.r, segCol.g, segCol.b, ConfigAlpha);
                        }
                        Graphics()->QuadsDrawFreeform(&Quad, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                for(size_t i = 1; i < m_PfVPath.size(); i++)
                {
                        if(g_Config.m_KxPfShowSpeed)
                        {
                                float speed = distance(m_PfVPath[i - 1], m_PfVPath[i]);
                                ColorRGBA c = fnSpeedColor(speed);
                                Graphics()->SetColor(c.r, c.g, c.b, ConfigAlpha);
                        }
                        else
                        {
                                // v1.56.210: per-segment gradient color (Rainbow+Gradient both ON).
                                ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_PATHFINDER, (int)i - 1), true);
                                Graphics()->SetColor(segCol.r, segCol.g, segCol.b, ConfigAlpha);
                        }
                        IGraphics::CLineItem Line(m_PfVPath[i - 1], m_PfVPath[i]);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

bool CBotNet::PfGenerateChunk(std::vector<CNetObj_PlayerInput> &outInputs, std::vector<vec2> &outTraj,
                              bool &outReachedFinish, bool &outDied, int &outFreezeTicks)
{
        CGameClient *pGame = GameClient();
        if(!pGame)
                return false;

        int LocalID = pGame->m_Snap.m_LocalClientId;
        if(LocalID < 0)
                return false;

        if(!pGame->m_PredictedWorld.GetCharacterById(LocalID))
                return false;

        int ChunkSize = pf_clamp(g_Config.m_KxPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
        int HookAngles = pf_clamp(g_Config.m_KxPfHookAngles, 4, 32);

        // Clean planning world (bot + collision only).  Other entities are
        // removed so they don't interfere with the prediction.
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
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalID)
                        continue;
                if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                        FutureWorld.RemoveEntity(pChar);
        }

        CCharacter *pBot = PfSpawnSimBot(&FutureWorld, LocalID, m_PfCurPos, m_PfCurVel,
                                         m_PfCurHookState, m_PfCurHookPos, m_PfCurHookDir,
                                         m_PfCurHookTick, m_PfCurFreezeTime, m_PfCurJumped);
        if(!pBot)
                return false;

        int StartTick = FutureWorld.GameTick();
        CCollision *pColl = FutureWorld.Collision();
        if(!pColl)
                return false;

        // ── Freeze-skip (v1.55): if the bot is frozen, fast-forward through
        // all freeze ticks as ONE chunk with empty input. This avoids wasting
        // multiple chunks on freeze stall. The frozen bot still experiences
        // gravity/friction (real DDNet behavior via CCharacter), so it
        // falls/slides correctly during the skip.
        // Discovered via Python benchmark: maps with freeze (e.g. map 9 with
        // 3 freeze walls = 450 ticks) are unsolvable without this macro-step.
        if(pBot->m_FreezeTime > 0)
        {
                int freezeTicks = pBot->m_FreezeTime;
                std::vector<CNetObj_PlayerInput> emptyInputs;
                emptyInputs.reserve(freezeTicks);
                for(int t = 0; t < freezeTicks; t++)
                {
                        CNetObj_PlayerInput inp;
                        mem_zero(&inp, sizeof(inp));
                        inp.m_TargetY = -1; // aim up (avoid (0,0) target)
                        emptyInputs.push_back(inp);
                }
                PfChunkResult res;
                PfSimulateChunk(&FutureWorld, LocalID, pBot, emptyInputs, StartTick, res);
                if(res.died && !res.reachedFinish)
                        return false;
                // Return this as the chunk
                outInputs = res.inputs;
                outTraj = res.traj;
                outReachedFinish = res.reachedFinish;
                outDied = res.died;
                outFreezeTicks = res.freezeTicks;
                // Update sim state from end of freeze
                m_PfCurPos = res.endPos;
                m_PfCurVel = res.endVel;
                m_PfCurHookPos = res.endHookPos;
                m_PfCurHookDir = res.endHookDir;
                m_PfCurHookState = res.endHookState;
                m_PfCurHookTick = res.endHookTick;
                m_PfCurJumped = res.endJumped;
                m_PfCurFreezeTime = res.endFreezeTime;
                return true;
        }

        float hookLength = pBot->GetCore().m_Tuning.m_HookLength;
        if(!(hookLength > 1.0f) || !(hookLength < 10000.0f))
                hookLength = 900.0f;

        // Hook ray precompute: same angles as before, filtered by raycast.
        struct HookRayResult {
                float angle;
                bool hasTarget;
        };
        HookRayResult aHookRays[32];
        if(pBot->GetCore().m_HookState == HOOK_IDLE)
        {
                for(int i = 0; i < HookAngles; i++)
                {
                        // Evenly spaced from -5pi/6 to +5pi/6
                        float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
                        float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
                        aHookRays[i].angle = angle;
                        vec2 endPos = pBot->m_Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
                        vec2 hitPos;
                        int hit = pColl->IntersectLineTeleHook(pBot->m_Pos, endPos, &hitPos, nullptr);
                        if(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH))
                                aHookRays[i].hasTarget = true;
                        else
                                aHookRays[i].hasTarget = false;
                }
        }

        static const int aDirs[] = {-1, 0, 1};

        // Track top-3 candidates for backtrack buffer.
        struct CandidateResult {
                std::vector<CNetObj_PlayerInput> inputs;
                std::vector<vec2> traj;
                float score;
                bool reachedFinish;
                bool died;
                int freezeTicks;
                int endFreezeTime;
                vec2 endPos, endVel, endHookPos, endHookDir;
                int endHookState, endHookTick, endJumped;
        };
        // Top-N candidates (N = KxPfBacktrackCandidates, dynamic). Using a vector
        // keeps memory bounded — only N slots per chunk, freed on reset.
        // Max for current params = 3 * (ChunkSize+1) * (1+HookAngles) (see PF_CANDIDATES_MAX).
        const int dynMax = 3 * (ChunkSize + 1) * (1 + HookAngles);
        const int N = pf_clamp(g_Config.m_KxPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynMax);
        std::vector<CandidateResult> topCandidates(N);
        for(int i = 0; i < N; i++)
                topCandidates[i].score = -1e18f;

        // === Advanced search (v1.56.14): per-tick input combinations ===
        // Same flat-counter mechanism as in PfAStarStep — each tick of every
        // candidate gets a unique (dir,jump,angle,hook) combination, the counter
        // keeps advancing across ticks and candidates without restart.
        if(g_Config.m_KxPfAdvancedSearch)
        {
                bool hookIdleAdv = (pBot->GetCore().m_HookState == HOOK_IDLE);
                bool hookActiveAdv = (pBot->GetCore().m_HookState == HOOK_FLYING || pBot->GetCore().m_HookState == HOOK_GRABBED);

                auto fnAimForAdv = [&](int angleIdx) -> vec2 {
                        float a = aHookRays[angleIdx].angle;
                        return vec2(cosf(a), sinf(a)) * 256.0f;
                };
                auto fnAimHookedAdv = [&]() -> vec2 { return pBot->GetCore().m_HookPos - pBot->m_Pos; };

                const int nDir = 3;
                const int nJump = 2;
                const int nAngle = HookAngles;
                const int nHook = 2;
                const long totalCombos = (long)nDir * nJump * nAngle * nHook; // = 12 * HookAngles
                const int dynAdvMax = (int)((totalCombos + ChunkSize - 1) / ChunkSize);
                int numCandidates = pf_clamp(g_Config.m_KxPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynAdvMax);
                if(numCandidates < 1) numCandidates = 1;

                for(int cand = 0; cand < numCandidates; cand++)
                {
                        std::vector<CNetObj_PlayerInput> inSeq;
                        inSeq.reserve(ChunkSize);
                        for(int t = 0; t < ChunkSize; t++)
                        {
                                long idx = ((long)cand * ChunkSize + t) % totalCombos;
                                int hookVal = (int)(idx % nHook); idx /= nHook;
                                int angleIdx = (int)(idx % nAngle); idx /= nAngle;
                                int jumpVal = (int)(idx % nJump); idx /= nJump;
                                int dirVal = (int)(idx % nDir);

                                if(hookVal == 1 && !hookIdleAdv && !hookActiveAdv)
                                        hookVal = 0;
                                if(hookVal == 1 && hookIdleAdv && !aHookRays[angleIdx].hasTarget)
                                        hookVal = 0;

                                CNetObj_PlayerInput inp;
                                mem_zero(&inp, sizeof(inp));
                                inp.m_Direction = aDirs[dirVal];
                                inp.m_Jump = jumpVal;
                                inp.m_Hook = hookVal;
                                vec2 aim;
                                if(hookVal == 1 && hookActiveAdv)
                                        aim = fnAimHookedAdv();
                                else if(hookVal == 1 && hookIdleAdv)
                                        aim = fnAimForAdv(angleIdx);
                                else
                                        aim = vec2((float)aDirs[dirVal] * 256.0f, -256.0f);
                                inp.m_TargetX = (int)aim.x;
                                inp.m_TargetY = (int)aim.y;
                                inSeq.push_back(inp);
                        }

                        // Save bot state, simulate, restore (same pattern as main sweep).
                        CCharacterCore savedCore = pBot->GetCore();
                        vec2 savedPos = pBot->m_Pos;
                        int savedFreeze = pBot->m_FreezeTime;

                        PfChunkResult res;
                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                        pBot->SetCore(savedCore);
                        pBot->m_Pos = savedPos;
                        pBot->m_FreezeTime = savedFreeze;

                        if(res.died && !res.reachedFinish)
                                continue;

                        float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                        for(int k = 0; k < N; k++)
                        {
                                if(score > topCandidates[k].score)
                                {
                                        for(int m = N - 1; m > k; m--)
                                                topCandidates[m] = topCandidates[m - 1];
                                        topCandidates[k].inputs = res.inputs;
                                        topCandidates[k].traj = res.traj;
                                        topCandidates[k].score = score;
                                        topCandidates[k].reachedFinish = res.reachedFinish;
                                        topCandidates[k].died = res.died;
                                        topCandidates[k].freezeTicks = res.freezeTicks;
                                        topCandidates[k].endPos = res.endPos;
                                        topCandidates[k].endVel = res.endVel;
                                        topCandidates[k].endHookPos = res.endHookPos;
                                        topCandidates[k].endHookDir = res.endHookDir;
                                        topCandidates[k].endHookState = res.endHookState;
                                        topCandidates[k].endHookTick = res.endHookTick;
                                        topCandidates[k].endJumped = res.endJumped;
                                        topCandidates[k].endFreezeTime = res.endFreezeTime;
                                        break;
                                }
                        }
                }
                // Fall through to the existing topCandidates consumer code below.
        }
        else
        // Parameter sweep: direction x jumpTick x hook x hookAngle
        for(int dirIdx = 0; dirIdx < 3; dirIdx++)
        {
                int dir = aDirs[dirIdx];
                for(int jumpTick = -1; jumpTick < ChunkSize; jumpTick++)
                {
                        for(int hookOn = 0; hookOn <= 1; hookOn++)
                        {
                                bool hookIdle = (pBot->GetCore().m_HookState == HOOK_IDLE);
                                bool hookActive = (pBot->GetCore().m_HookState == HOOK_FLYING || pBot->GetCore().m_HookState == HOOK_GRABBED);
                                if(hookOn == 1 && !hookIdle && !hookActive)
                                        continue;

                                if(hookOn == 1 && hookIdle)
                                {
                                        // Fire new hook: sweep over hook angles (with raycast filter).
                                        for(int angleIdx = 0; angleIdx < HookAngles; angleIdx++)
                                        {
                                                if(!aHookRays[angleIdx].hasTarget)
                                                        continue;
                                                float hookAngle = aHookRays[angleIdx].angle;

                                                // Build the per-tick input for this candidate.
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
                                                        inp.m_Fire = 0;
                                                        inp.m_WantedWeapon = 0;
                                                        inp.m_PlayerFlags = 0;
                                                        inp.m_NextWeapon = 0;
                                                        inp.m_PrevWeapon = 0;
                                                        inSeq.push_back(inp);
                                                }

                                                // v1.48 fix: save full bot state (core + pos + m_FreezeTime)
                                                // before simulating, restore after — otherwise freeze state
                                                // leaks across candidates.
                                                CCharacterCore savedCore = pBot->GetCore();
                                                vec2 savedPos = pBot->m_Pos;
                                                int savedFreeze = pBot->m_FreezeTime;

                                                PfChunkResult res;
                                                PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                                                pBot->SetCore(savedCore);
                                                pBot->m_Pos = savedPos;
                                                pBot->m_FreezeTime = savedFreeze;

                                                if(res.died && !res.reachedFinish)
                                                        continue;

                                                float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                                                for(int k = 0; k < N; k++)
                                                {
                                                        if(score > topCandidates[k].score)
                                                        {
                                                                for(int m = N - 1; m > k; m--)
                                                                        topCandidates[m] = topCandidates[m - 1];
                                                                topCandidates[k].inputs = res.inputs;
                                                                topCandidates[k].traj = res.traj;
                                                                topCandidates[k].score = score;
                                                                topCandidates[k].reachedFinish = res.reachedFinish;
                                                                topCandidates[k].died = res.died;
                                                                topCandidates[k].freezeTicks = res.freezeTicks;
                                                                topCandidates[k].endPos = res.endPos;
                                                                topCandidates[k].endVel = res.endVel;
                                                                topCandidates[k].endHookPos = res.endHookPos;
                                                                topCandidates[k].endHookDir = res.endHookDir;
                                                                topCandidates[k].endHookState = res.endHookState;
                                                                topCandidates[k].endHookTick = res.endHookTick;
                                                                topCandidates[k].endJumped = res.endJumped;
                                                                topCandidates[k].endFreezeTime = res.endFreezeTime;
                                                                break;
                                                        }
                                                }
                                        }
                                }
                                else if(hookOn == 1 && hookActive)
                                {
                                        // Hold existing hook (no angle sweep needed).
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 1;
                                                vec2 aimRel = pBot->GetCore().m_HookPos - pBot->m_Pos;
                                                inp.m_TargetX = (int)aimRel.x;
                                                inp.m_TargetY = (int)aimRel.y;
                                                inp.m_Fire = 0;
                                                inp.m_WantedWeapon = 0;
                                                inp.m_PlayerFlags = 0;
                                                inp.m_NextWeapon = 0;
                                                inp.m_PrevWeapon = 0;
                                                inSeq.push_back(inp);
                                        }

                                        CCharacterCore savedCore = pBot->GetCore();
                                        vec2 savedPos = pBot->m_Pos;
                                        int savedFreeze = pBot->m_FreezeTime;

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                                        pBot->SetCore(savedCore);
                                        pBot->m_Pos = savedPos;
                                        pBot->m_FreezeTime = savedFreeze;

                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                                        for(int k = 0; k < N; k++)
                                        {
                                                if(score > topCandidates[k].score)
                                                {
                                                        for(int m = N - 1; m > k; m--)
                                                                topCandidates[m] = topCandidates[m - 1];
                                                        topCandidates[k].inputs = res.inputs;
                                                        topCandidates[k].traj = res.traj;
                                                        topCandidates[k].score = score;
                                                        topCandidates[k].reachedFinish = res.reachedFinish;
                                                        topCandidates[k].died = res.died;
                                                        topCandidates[k].freezeTicks = res.freezeTicks;
                                                        topCandidates[k].endPos = res.endPos;
                                                        topCandidates[k].endVel = res.endVel;
                                                        topCandidates[k].endHookPos = res.endHookPos;
                                                        topCandidates[k].endHookDir = res.endHookDir;
                                                        topCandidates[k].endHookState = res.endHookState;
                                                        topCandidates[k].endHookTick = res.endHookTick;
                                                        topCandidates[k].endJumped = res.endJumped;
                                                        topCandidates[k].endFreezeTime = res.endFreezeTime;
                                                        break;
                                                }
                                        }
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
                                                inp.m_Fire = 0;
                                                inp.m_WantedWeapon = 0;
                                                inp.m_PlayerFlags = 0;
                                                inp.m_NextWeapon = 0;
                                                inp.m_PrevWeapon = 0;
                                                inSeq.push_back(inp);
                                        }

                                        CCharacterCore savedCore = pBot->GetCore();
                                        vec2 savedPos = pBot->m_Pos;
                                        int savedFreeze = pBot->m_FreezeTime;

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                                        pBot->SetCore(savedCore);
                                        pBot->m_Pos = savedPos;
                                        pBot->m_FreezeTime = savedFreeze;

                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                                        for(int k = 0; k < N; k++)
                                        {
                                                if(score > topCandidates[k].score)
                                                {
                                                        for(int m = N - 1; m > k; m--)
                                                                topCandidates[m] = topCandidates[m - 1];
                                                        topCandidates[k].inputs = res.inputs;
                                                        topCandidates[k].traj = res.traj;
                                                        topCandidates[k].score = score;
                                                        topCandidates[k].reachedFinish = res.reachedFinish;
                                                        topCandidates[k].died = res.died;
                                                        topCandidates[k].freezeTicks = res.freezeTicks;
                                                        topCandidates[k].endPos = res.endPos;
                                                        topCandidates[k].endVel = res.endVel;
                                                        topCandidates[k].endHookPos = res.endHookPos;
                                                        topCandidates[k].endHookDir = res.endHookDir;
                                                        topCandidates[k].endHookState = res.endHookState;
                                                        topCandidates[k].endHookTick = res.endHookTick;
                                                        topCandidates[k].endJumped = res.endJumped;
                                                        topCandidates[k].endFreezeTime = res.endFreezeTime;
                                                        break;
                                                }
                                        }
                                }
                        }
                }
        }

        if(topCandidates[0].score <= -1e17f)
                return false; // no valid candidates

        // Save backtrack entry.
        if(m_PfBacktrackIdx < PF_BACKTRACK_DEPTH)
        {
                PfBacktrackEntry &entry = m_PfBacktrack[m_PfBacktrackIdx];
                entry.pos = m_PfCurPos;
                entry.vel = m_PfCurVel;
                entry.hookPos = m_PfCurHookPos;
                entry.hookDir = m_PfCurHookDir;
                entry.hookState = m_PfCurHookState;
                entry.hookTick = m_PfCurHookTick;
                entry.jumped = m_PfCurJumped;
                entry.freezeTime = m_PfCurFreezeTime;
                entry.totalFreezeTicks = m_PfTotalFreezeTicks;
                entry.vPathSize = (int)m_PfVPath.size();
                entry.chunkCount = m_PfChunkCount;
                entry.numCandidates = 0;
                entry.usedCandidateIdx = 0;

                // Reserve exact capacity for this entry's candidates (avoids
                // reallocation as we push_back). Vector is cleared on Reset()
                // so no leak across runs.
                entry.candidates.clear();
                entry.candidates.reserve(N);
                for(int k = 0; k < N; k++)
                {
                        if(topCandidates[k].score <= -1e17f)
                                break;
                        PfBacktrackEntry::Candidate c;
                        c.inputs = topCandidates[k].inputs;
                        c.traj = topCandidates[k].traj;
                        c.score = topCandidates[k].score;
                        c.endPos = topCandidates[k].endPos;
                        c.endVel = topCandidates[k].endVel;
                        c.endHookPos = topCandidates[k].endHookPos;
                        c.endHookDir = topCandidates[k].endHookDir;
                        c.endHookState = topCandidates[k].endHookState;
                        c.endHookTick = topCandidates[k].endHookTick;
                        c.endJumped = topCandidates[k].endJumped;
                        c.endFreezeTime = topCandidates[k].endFreezeTime;
                        c.freezeTicks = topCandidates[k].freezeTicks;
                        entry.candidates.push_back(std::move(c));
                        entry.numCandidates++;
                }
                m_PfBacktrackIdx++;
        }

        // Apply best candidate.
        CandidateResult &best = topCandidates[0];
        outInputs = best.inputs;
        outTraj = best.traj;
        outReachedFinish = best.reachedFinish;
        outDied = best.died;
        outFreezeTicks = best.freezeTicks;

        m_PfCurPos = best.endPos;
        m_PfCurVel = best.endVel;
        m_PfCurHookPos = best.endHookPos;
        m_PfCurHookDir = best.endHookDir;
        m_PfCurHookState = best.endHookState;
        m_PfCurHookTick = best.endHookTick;
        m_PfCurJumped = best.endJumped;
        m_PfCurFreezeTime = best.endFreezeTime;

        return true;
}
