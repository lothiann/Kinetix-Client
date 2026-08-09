#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

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
                State.m_FlowDir = vec2(0, 0);
                State.m_PfHookTile = vec2(0, 0);
                return;
        }

        // Single source = the target tile; freeze passable (cost-penalised).
        std::vector<std::pair<int, int>> sources{{targetTX, targetTY}};
        bool reached = PfAStarSearch(botTX, botTY, sources, /*skipFreeze=*/false);

        State.m_PathFound = reached;
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
// PATHFINDER RESCUE
// =========================================================
void CBotNet::ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
        if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
                return;

        targetTX = pf_clamp(targetTX, 0, m_MapWidth - 1);
        targetTY = pf_clamp(targetTY, 0, m_MapHeight - 1);
        botTX = pf_clamp(botTX, 0, m_MapWidth - 1);
        botTY = pf_clamp(botTY, 0, m_MapHeight - 1);

        if(!IsTileWalkable(botTX, botTY))
        {
                State.m_PathFound = false;
                State.m_FlowDir = vec2(0, 0);
                return;
        }

        // Ray-fan around the (frozen) target to find nearby walkable, non-freeze
        // tiles — these are candidate "safe spots" the bot can pull the target to.
        int hookRadiusTiles = (int)(g_Config.m_KxHookDist / 32.0f);
        if(hookRadiusTiles < 1)
                hookRadiusTiles = 1;

        struct SafeTile
        {
                int tx, ty;
                int dist; // squared distance to target (for sort + grouping)
        };
        SafeTile safeTiles[1024];
        int numSafe = 0;

        for(int i = 0; i < g_Config.m_KxAtkPathfinderRays; i++)
        {
                float angle = 2.0f * pi * i / g_Config.m_KxAtkPathfinderRays;
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

                        if(numSafe < 1024)
                        {
                                safeTiles[numSafe].tx = tc;
                                safeTiles[numSafe].ty = tr;
                                int ddx = tc - targetTX;
                                int ddy = tr - targetTY;
                                safeTiles[numSafe].dist = ddx * ddx + ddy * ddy;
                                numSafe++;
                        }
                }
        }

        if(numSafe == 0)
        {
                State.m_PathFound = false;
                State.m_FlowDir = vec2(0, 0);
                return;
        }

        // Sort by squared distance (bubble sort — numSafe is small).
        for(int a = 0; a < numSafe - 1; a++)
                for(int b = a + 1; b < numSafe; b++)
                        if(safeTiles[b].dist < safeTiles[a].dist)
                        {
                                SafeTile tmp = safeTiles[a];
                                safeTiles[a] = safeTiles[b];
                                safeTiles[b] = tmp;
                        }

        // Try safe tiles in distance rings: all tiles at the same squared distance
        // from the target seed the A* together (multi-source).  Stop at the first
        // ring that reaches the bot — nearest safe spot wins.
        int startIdx = 0;
        while(startIdx < numSafe)
        {
                int curDist = safeTiles[startIdx].dist;
                std::vector<std::pair<int, int>> sources;
                int endIdx = startIdx;
                while(endIdx < numSafe && safeTiles[endIdx].dist == curDist)
                {
                        sources.emplace_back(safeTiles[endIdx].tx, safeTiles[endIdx].ty);
                        endIdx++;
                }

                // Rescue variant: freeze tiles are impassable (skipFreeze=true).
                bool reached = PfAStarSearch(botTX, botTY, sources, /*skipFreeze=*/true);
                if(reached)
                {
                        State.m_PathFound = true;
                        State.m_LastTargetTX = targetTX;
                        State.m_LastTargetTY = targetTY;
                        State.m_LastBotTX = botTX;
                        State.m_LastBotTY = botTY;
                        ComputeFlowForTile(botTY, botTX, State);
                        return;
                }

                startIdx = endIdx;
        }

        State.m_PathFound = false;
        State.m_FlowDir = vec2(0, 0);
}

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

        // === Step 1: A* rays ===
        for(int i = 0; i < g_Config.m_KxAtkPathfinderRays; i++)
        {
                float angle = 2.0f * pi * i / g_Config.m_KxAtkPathfinderRays;
                float rayDX = cosf(angle);
                float rayDY = sinf(angle);

                float rayFreezePenalty = 0;

                for(int step = 1; step <= g_Config.m_KxAtkPathfinderRaysDist; step++)
                {
                        int tr = r + (int)roundf(rayDY * step);
                        int tc = c + (int)roundf(rayDX * step);
                        if(tr < 0 || tc < 0 || tr >= m_MapHeight || tc >= m_MapWidth)
                                break;
                        if(!IsTileWalkable(tc, tr))
                                break;

                        int fIdx = tr * m_MapWidth + tc;

                        if(g_Config.m_KxPfSimulatePlayers && m_PfPlayerPenalty && m_PfPlayerPenalty[fIdx] > 0)
                        {
                                if(g_Config.m_KxAtkPathfinderSps == 0)
                                        break;
                                else
                                        rayFreezePenalty += m_PfPlayerPenalty[fIdx];
                        }

                        if(m_pMapGrid[fIdx] == TILE_FREEZE || m_pMapGrid[fIdx] == TILE_DFREEZE || m_pMapGrid[fIdx] == TILE_LFREEZE)
                                rayFreezePenalty += PF_FREEZE_COST;
                        if(m_pFrontGrid[fIdx] == TILE_FREEZE || m_pFrontGrid[fIdx] == TILE_DFREEZE || m_pFrontGrid[fIdx] == TILE_LFREEZE)
                                rayFreezePenalty += PF_FREEZE_COST;

                        int tIdx = tr * m_MapWidth + tc;
                        float adjustedDist = m_pfDist[tIdx] + rayFreezePenalty;

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

        // === Step 2: Freeze repulsion ===
        if(g_Config.m_KxAvoidFreeze)
        {
                vec2 repel = ComputeFreezeRepel(c, r);
                float repelLen = sqrtf(repel.x * repel.x + repel.y * repel.y);
                if(repelLen > 0.001f)
                {
                        float normX = repel.x / repelLen;
                        float normY = repel.y / repelLen;

                        float repelWeight = repelLen * PF_FREEZE_REPEL_WEIGHT;
                        float virtualDist = currentD - repelWeight;

                        if(virtualDist < bestDist)
                        {
                                bestDist = virtualDist;
                                bestDX = normX;
                                bestDY = normY;
                        }
                }
        }

        // === Step 3: Pathfinder Hook ===
        if(g_Config.m_KxPfHook)
        {
                float flLen = sqrtf(bestDX * bestDX + bestDY * bestDY);
                if(flLen > 0.001f)
                {
                        float fDirX = bestDX / flLen;
                        float fDirY = bestDY / flLen;
                        int hookRange = pf_min(g_Config.m_KxAtkPathfinderRaysDist, (int)(g_Config.m_KxHookDist / 32.0f));
                        int bestStep = 999999;

                        for(int i = 0; i < g_Config.m_KxAtkPathfinderRays; i++)
                        {
                                float angle = 2.0f * pi * i / g_Config.m_KxAtkPathfinderRays;
                                float rdx = cosf(angle);
                                float rdy = sinf(angle);

                                if(rdx * fDirX + rdy * fDirY < 0.7f)
                                        continue;

                                for(int step = 1; step <= hookRange; step++)
                                {
                                        int tr = r + (int)roundf(rdy * step);
                                        int tc = c + (int)roundf(rdx * step);
                                        if(tr < 0 || tc < 0 || tr >= m_MapHeight || tc >= m_MapWidth)
                                                break;

                                        int hIdx = tr * m_MapWidth + tc;

                                        if(m_PfPlayerPenalty[hIdx] > 0.0f)
                                                break;

                                        unsigned char tile = m_pMapGrid[hIdx];

                                        if(tile == TILE_SOLID)
                                        {
                                                if(step < bestStep)
                                                {
                                                        bestStep = step;
                                                        State.m_PfHookTile = vec2(tc * 32.0f + 16.0f, tr * 32.0f + 16.0f);
                                                }
                                                break;
                                        }

                                        if(!IsTileWalkable(tc, tr))
                                                break;
                                }
                        }
                }
        }

        float len = sqrtf(bestDX * bestDX + bestDY * bestDY);
        if(len > 0.001f)
        {
                State.m_FlowDir = vec2(bestDX / len, bestDY / len);
        }
        else
        {
                State.m_FlowDir = vec2(0, 0);
        }
}

vec2 CBotNet::ComputeFreezeRepel(int botTX, int botTY)
{
        vec2 repel = vec2(0, 0);
        const int scanRadius = g_Config.m_KxAvoidFreezeRadius;

        for(int i = 0; i < g_Config.m_KxAtkPathfinderRays; i++)
        {
                float angle = 2.0f * pi * i / g_Config.m_KxAtkPathfinderRays;
                float rayDX = cosf(angle);
                float rayDY = sinf(angle);

                for(int step = 1; step <= scanRadius; step++)
                {
                        int tc = botTX + (int)roundf(rayDX * step);
                        int tr = botTY + (int)roundf(rayDY * step);

                        if(tc < 0 || tr < 0 || tc >= m_MapWidth || tr >= m_MapHeight)
                                break;
                        if(!IsTileWalkable(tc, tr))
                                break;

                        if(IsTileFreeze(tc, tr))
                        {
                                float dx = (float)(tc - botTX);
                                float dy = (float)(tr - botTY);
                                float distSq = dx * dx + dy * dy;
                                if(distSq < 0.001f)
                                        continue;

                                repel.x += -dx / distSq;
                                repel.y += -dy / distSq;
                        }
                }
        }

        return repel;
}

void CBotNet::UpdatePlayerPenalty(int botTX, int botTY, int excludeTX, int excludeTY, int LocalID)
{
        if(!m_PfPlayerPenalty)
                return;

        mem_zero(m_PfPlayerPenalty, m_MapWidth * m_MapHeight * sizeof(float));

        if(!g_Config.m_KxPfSimulatePlayers && !g_Config.m_KxPfHook)
                return;

        CGameClient *pGame = GameClient();
        for(int i = 0; i < 128; i++)
        {
                if(i == LocalID || !pGame->m_aClients[i].m_Active || !pGame->m_Snap.m_aCharacters[i].m_Active)
                        continue;
                int ptx = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X / 32.0f);
                int pty = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y / 32.0f);
                if(ptx < 0 || pty < 0 || ptx >= m_MapWidth || pty >= m_MapHeight)
                        continue;
                if(ptx == botTX && pty == botTY)
                        continue;
                int idx = pty * m_MapWidth + ptx;
                if(m_pMapGrid[idx] == TILE_SOLID || m_pMapGrid[idx] == TILE_DEATH)
                        continue;

                bool isTarget = (ptx == excludeTX && pty == excludeTY);

                if(g_Config.m_KxPfSimulatePlayers && !isTarget)
                {
                        if(g_Config.m_KxAtkPathfinderSps == 1)
                                m_PfPlayerPenalty[idx] += g_Config.m_KxPfSimulateScore;
                        else
                                m_PfPlayerPenalty[idx] += PF_PLAYER_COST;
                }

                if(g_Config.m_KxPfHook)
                        m_PfPlayerPenalty[idx] += 1.0f;
        }
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
// INPUT HELPERS
// =========================================================
