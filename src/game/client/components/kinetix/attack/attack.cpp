#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// =========================================================
// BLOCK (attack / rescue / escort) — decision + action layer
// =========================================================
// One dummy tick flows through UpdateHelpers in four steps:
//
//   1. FindHelpersTarget    rescue first (nearest REACHABLE frozen ally,
//                         pathability checked by ComputePathfinderRescue),
//                         then attack (explicit Target IDs chased anywhere,
//                         else the nearest VISIBLE enemy), then escort
//                         the main.
//   2. MoveToHelpersTarget  the shared movement stack: guidance field ->
//                         physics plan (ComputeHelpersPath) -> replay, with
//                         a grid-flow fallback that jumps over blockers.
//   3. Aim                the plan owns the aim while it replays,
//                         otherwise the dummy aims straight at the target.
//   4. Hook + Fire        the old AutoHook pulse (range + LOS + no other
//                         tee in between) and the AutoFire hammer.
//
// With no target at all the bot does not blindly idle: MoveToSafety
// retreats it away from the freeze edge, so a stray hook or hammer can
// never drop an AFK bot into freeze.
//
// The movement primitives themselves live in pathfinder/pathfinder.cpp:
// grid field (ComputePathfinder / ComputePathfinderRescue), flow
// (ComputeFlowForTile) and the physics plan (ComputeHelpersPath).
// =========================================================

bool CBotNet::HasLineOfSightTiles(int r1, int c1, int r2, int c2)
{
        float x1 = c1 + 0.5f, y1 = r1 + 0.5f;
        float x2 = c2 + 0.5f, y2 = r2 + 0.5f;
        float d = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
        int steps = pf_max((int)(d * 3.0f), 1);
        int prevR = -1, prevC = -1;

        for(int i = 0; i <= steps; i++)
        {
                float t = (float)i / steps;
                float x = x1 + (x2 - x1) * t;
                float y = y1 + (y2 - y1) * t;
                int r = (int)y, c = (int)x;
                if(r == prevR && c == prevC)
                        continue;
                if(!IsTileWalkable(c, r))
                        return false;
                if(prevR >= 0 && abs(r - prevR) + abs(c - prevC) == 2)
                {
                        if(!IsTileWalkable(c, prevR) && !IsTileWalkable(prevC, r))
                                return false;
                }
                prevR = r;
                prevC = c;
        }
        return true;
}

// =========================================================
// SAFE SPOT — shared survival check
// =========================================================
// A tile is safe when it is walkable and no freeze with a clear line
// sits within SAFETY_MARGIN tiles of it — close enough for a hook or
// hammer yank to drop the bot in.  Used by the survival gate in
// UpdateHelpers and by the retreat goals in MoveToSafety.
// =========================================================

bool CBotNet::IsSafeSpot(int tx, int ty)
{
        const int SAFETY_MARGIN = 2;
        if(!IsTileWalkable(tx, ty))
                return false;
        for(int oy = -SAFETY_MARGIN; oy <= SAFETY_MARGIN; oy++)
        {
                for(int ox = -SAFETY_MARGIN; ox <= SAFETY_MARGIN; ox++)
                {
                        int fx = tx + ox, fy = ty + oy;
                        if(fx < 0 || fy < 0 || fx >= m_MapWidth || fy >= m_MapHeight)
                                continue;
                        if(IsTileFreeze(fx, fy) && HasLineOfSightTiles(ty, tx, fy, fx))
                                return false;
                }
        }
        return true;
}

// =========================================================
// STEP 1 — TARGET SELECTION (priority: rescue > attack > escort)
// =========================================================

int CBotNet::FindHelpersTarget(CGameClient *pGame, int LocalID, const vec2 &MyPos, CBotNetDummy &State,
        bool &TargetIsMain, bool &TargetIsRescue, bool &TargetExplicit)
{
        // --- Rescue: the nearest frozen ally that is actually REACHABLE ---
        // SmartDetect is always on: candidates are scanned through walls, but
        // each one must have a freeze-free route to a safe tile next to it
        // (ComputePathfinderRescue fills m_pfDist + flow for the movement).
        if(g_Config.m_KxRescue && m_MapGridLoaded)
        {
                int aCandidates[128];
                float aDists[128];
                int numCandidates = 0;

                for(int i = 0; i < 128; i++)
                {
                        if(i == LocalID || !pGame->m_aClients[i].m_Active)
                                continue;
                        if(pGame->m_aClients[i].m_FreezeEnd == 0)
                                continue;
                        // An ID listed in Target IDs is an enemy — never a
                        // rescue target, even when it is the main.
                        if(!g_Config.m_KxTargetAll && m_TargetList[i])
                                continue;
                        bool canRescue;
                        if(i == g_Config.m_KxMain)
                                canRescue = true; // the main is rescued across the whole map
                        else if(m_BotsList[i])
                                canRescue = true; // dummies rescue each other
                        else if(g_Config.m_KxRescueAll)
                                canRescue = !m_RescueList[i]; // everyone except exclusions
                        else
                                canRescue = m_RescueList[i];
                        if(!canRescue)
                                continue;
                        vec2 TPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                        float d = distance(MyPos, TPos);
                        bool inRadius = g_Config.m_KxRescueRadius <= 0 || i == g_Config.m_KxMain ||
                                d < (float)g_Config.m_KxRescueRadius;
                        if(!inRadius)
                                continue;
                        aCandidates[numCandidates] = i;
                        aDists[numCandidates] = d;
                        numCandidates++;
                }

                // Nearest first (insertion sort — the list is tiny).
                for(int a = 1; a < numCandidates; a++)
                {
                        for(int b = a; b > 0 && aDists[b] < aDists[b - 1]; b--)
                        {
                                int tmpId = aCandidates[b];
                                aCandidates[b] = aCandidates[b - 1];
                                aCandidates[b - 1] = tmpId;
                                float tmpD = aDists[b];
                                aDists[b] = aDists[b - 1];
                                aDists[b - 1] = tmpD;
                        }
                }

                int botTX = pf_clamp((int)(MyPos.x / 32.0f), 0, m_MapWidth - 1);
                int botTY = pf_clamp((int)(MyPos.y / 32.0f), 0, m_MapHeight - 1);

                // First ally with a freeze-free route to one of its safe tiles
                // wins; the rescue field stays in m_pfDist for the movement.
                for(int j = 0; j < numCandidates; j++)
                {
                        int i = aCandidates[j];
                        int resTX = pf_clamp((int)(pGame->m_Snap.m_aCharacters[i].m_Cur.m_X / 32.0f), 0, m_MapWidth - 1);
                        int resTY = pf_clamp((int)(pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y / 32.0f), 0, m_MapHeight - 1);

                        ComputePathfinderRescue(botTX, botTY, resTX, resTY, State);
                        if(State.m_PathFound)
                        {
                                TargetIsRescue = true;
                                return i;
                        }
                }
        }

        // --- Attack: explicit Target IDs first, then the nearest VISIBLE enemy ---
        {
                // An ID explicitly listed in Target IDs is chased across the
                // map — even if it is the main, a bot, or already frozen.
                int explicitID = -1;
                float explicitBest = 1000000.0f;
                int regularID = -1;
                float regularBest = (g_Config.m_KxTargetDist > 0) ? (float)g_Config.m_KxTargetDist : 1000000.0f;

                for(int i = 0; i < 128; i++)
                {
                        bool explicitEnemy = !g_Config.m_KxTargetAll && m_TargetList[i];
                        bool tgtFrozen = pGame->m_aClients[i].m_FreezeEnd != 0 || pGame->m_aClients[i].m_DeepFrozen;
                        if(i == LocalID || !pGame->m_aClients[i].m_Active)
                                continue;
                        if(!explicitEnemy && m_BotsList[i])
                                continue; // bots/dummies don't attack each other
                        if(!explicitEnemy && i == g_Config.m_KxMain)
                                continue; // main is a friend
                        if(!explicitEnemy && tgtFrozen)
                                continue; // already blocked
                        bool isEnemy = g_Config.m_KxTargetAll ? !m_TargetList[i] : m_TargetList[i];
                        if(!isEnemy)
                                continue;
                        vec2 TPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                        // Regular enemies need a clear line of sight.
                        if(!explicitEnemy && pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) > 0)
                                continue;
                        float d = distance(MyPos, TPos);
                        if(explicitEnemy)
                        {
                                if(d < explicitBest)
                                {
                                        explicitBest = d;
                                        explicitID = i;
                                }
                        }
                        else if(d < regularBest)
                        {
                                regularBest = d;
                                regularID = i;
                        }
                }
                // An explicitly named ID always wins over a regular pick.
                if(explicitID != -1)
                {
                        TargetExplicit = true;
                        return explicitID;
                }
                if(regularID != -1)
                        return regularID;
        }

        // --- Escort the main ---
        if(g_Config.m_KxMain >= 0 && g_Config.m_KxMain < 128 && pGame->m_aClients[g_Config.m_KxMain].m_Active)
        {
                vec2 MainPos((float)pGame->m_Snap.m_aCharacters[g_Config.m_KxMain].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[g_Config.m_KxMain].m_Cur.m_Y);
                if(distance(MyPos, MainPos) <= (float)g_Config.m_KxMainDist)
                {
                        TargetIsMain = true;
                        return g_Config.m_KxMain;
                }
        }

        return -1;
}

// =========================================================
// STEP 2 — MOVEMENT (field -> physics plan, grid-flow fallback)
// =========================================================

bool CBotNet::MoveToHelpersTarget(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
        int LocalID, int TargetID, const vec2 &MyPos, const vec2 &TPos, float dx, bool TargetIsRescue)
{
        int botTX = pf_clamp((int)(MyPos.x / 32.0f), 0, m_MapWidth - 1);
        int botTY = pf_clamp((int)(MyPos.y / 32.0f), 0, m_MapHeight - 1);
        int gtx, gty;

        if(TargetIsRescue)
        {
                // FindHelpersTarget already ran ComputePathfinderRescue for this
                // ally: m_pfDist is the freeze-walled field seeded by the safe
                // tiles around it.  The plan chases dist==0 — any safe tile
                // next to the ally.
                if(!State.m_PathFound)
                        return false;
                gtx = pf_clamp(State.m_LastTargetTX, 0, m_MapWidth - 1);
                gty = pf_clamp(State.m_LastTargetTY, 0, m_MapHeight - 1);
        }
        else
        {
                // Goal: the target tile, or the nearest walkable tile around
                // it when the target stands on/behind non-walkable ground.
                gtx = pf_clamp((int)(TPos.x / 32.0f), 0, m_MapWidth - 1);
                gty = pf_clamp((int)(TPos.y / 32.0f), 0, m_MapHeight - 1);
                if(!IsTileWalkable(gtx, gty))
                {
                        vec2 bestG(0, 0);
                        float bestGD = 1e9f;
                        for(int oy = -4; oy <= 4; oy++)
                        {
                                for(int ox = -4; ox <= 4; ox++)
                                {
                                        int tx = gtx + ox, ty = gty + oy;
                                        if(!IsTileWalkable(tx, ty))
                                                continue;
                                        vec2 fc((float)tx * 32.0f + 16.0f, (float)ty * 32.0f + 16.0f);
                                        float fd = distance(fc, TPos);
                                        if(fd < bestGD)
                                        {
                                                bestGD = fd;
                                                bestG = fc;
                                        }
                                }
                        }
                        if(bestGD < 1e8f)
                        {
                                gtx = pf_clamp((int)(bestG.x / 32.0f), 0, m_MapWidth - 1);
                                gty = pf_clamp((int)(bestG.y / 32.0f), 0, m_MapHeight - 1);
                        }
                }

                // Recompute the field when the goal moved, the last search
                // failed, or the bot ended up on a tile the field can't see.
                bool botReachable = m_pfDist[botTY * m_MapWidth + botTX] < 1e17f;
                bool needField = (gtx != State.m_LastTargetTX || gty != State.m_LastTargetTY) ||
                        !State.m_PathFound || !botReachable;
                if(needField && IsTileWalkable(gtx, gty))
                        ComputePathfinder(botTX, botTY, gtx, gty, State);
                if(!State.m_PathFound)
                        return false;
        }

        return MoveAlongField(pGame, pInput, Dummy, State, LocalID, MyPos, gtx, gty, dx, TargetID);
}

// =========================================================
// SHARED FIELD-WALKER — used by targeting and by idle safety
// =========================================================
// The guidance field must already be computed (m_PathFound true, goal in
// LastTarget).  Refreshes the per-tile flow, replays/builds the physics
// plan toward (gtx, gty) and falls back to grid flow.  Returns true while
// the plan owns the inputs.
// =========================================================

bool CBotNet::MoveAlongField(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
        int LocalID, const vec2 &MyPos, int gtx, int gty, float JumpProbeDX, int BlockerId)
{
        int botTX = pf_clamp((int)(MyPos.x / 32.0f), 0, m_MapWidth - 1);
        int botTY = pf_clamp((int)(MyPos.y / 32.0f), 0, m_MapHeight - 1);

        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                return false; // off the safe field — fall back to grid flow

        // Refresh the per-tile flow when the bot crosses a tile border.
        if(botTX != State.m_LastBotTX || botTY != State.m_LastBotTY)
        {
                if(State.m_RescueField)
                        RescueFlowForTile(botTY, botTX, State); // rescue FMM gradient
                else
                        ComputeFlowForTile(botTY, botTX, State);
                State.m_LastBotTX = botTX;
                State.m_LastBotTY = botTY;
        }

        // Physics plan: recompute on goal change, spent cache or drift from
        // the planned trajectory.  Near freeze the drift limit drops to 16px:
        // a hook/hammer yank by the pool edge must replan at once — 48px is
        // enough to be dragged a full tile in before the replan triggers.
        bool nearFreeze = false;
        for(int oy = -2; oy <= 2 && !nearFreeze; oy++)
                for(int ox = -2; ox <= 2 && !nearFreeze; ox++)
                {
                        int fx = botTX + ox, fy = botTY + oy;
                        if(fx < 0 || fy < 0 || fx >= m_MapWidth || fy >= m_MapHeight)
                                continue;
                        if(IsTileFreeze(fx, fy) && HasLineOfSightTiles(botTY, botTX, fy, fx))
                                nearFreeze = true;
                }
        float driftLimit = nearFreeze ? 16.0f : 48.0f;
        CHelpersPlan &plan = State.m_HelpersPlan;
        bool needPlan = !plan.IsValid() || plan.TargetTX != gtx || plan.TargetTY != gty ||
                plan.CurrentTick >= plan.NumTicks;
        if(plan.IsValid() && plan.CurrentTick >= 0 && plan.CurrentTick < (int)plan.vPath.size() &&
                distance(MyPos, plan.vPath[plan.CurrentTick]) > driftLimit)
                needPlan = true;
        if(needPlan)
                ComputeHelpersPath(Dummy, MyPos, botTX, botTY, gtx, gty, State);
        if(plan.IsValid())
        {
                ApplyHelpersPlan(Dummy, pInput, State);
                return true; // the plan owns direction/jump/hook/aim this tick
        }

        // Grid-flow fallback (shared helpers with PathfinderGo) + jump when a
        // tee blocks the direct line (old jump-over behaviour).  The probe
        // direction is the target side, or the flow's horizontal sign when
        // there is no target.
        // Survival veto: with no valid plan the blind flow is not trusted
        // next to freeze — the bot brakes instead of walking on unvalidated
        // directions by the pool edge.  A fresh plan is built next tick.
        // Rescue Phase 2 stands down: its route deliberately crosses the
        // pool, so braking by the edge would cancel the rescue entirely.
        if(!State.m_FlowThroughFreeze && !IsSafeSpot(botTX, botTY))
        {
                SetDirection(pGame, Dummy, false, false);
                pInput->m_Jump = 0;
                pInput->m_Hook = 0;
                return false;
        }
        bool left = false, right = false, jump = false;
        GetMovementFromFlow(State, left, right, jump);
        float probe = (JumpProbeDX > 0.0f) ? 1.0f : (JumpProbeDX < 0.0f ? -1.0f : (right ? 1.0f : -1.0f));
        vec2 IntersectPos;
        int HitPlayer = pGame->IntersectCharacter(MyPos, MyPos + vec2(probe * 40.0f, 0), IntersectPos, LocalID);
        if(HitPlayer != -1 && HitPlayer != BlockerId)
                jump = true;
        SetDirection(pGame, Dummy, left, right);
        pInput->m_Jump = jump ? 1 : 0;
        pInput->m_Hook = 0;
        return false;
}

// =========================================================
// IDLE SAFETY — an AFK bot never sits where it can be shoved
// into freeze
// =========================================================
// When the bot has no target it would otherwise stand still.  If the
// tile it stands on has freeze within SAFETY_MARGIN tiles — close
// enough for a hook or hammer to drop it in — it retreats to the
// nearest safe tile using the same field + plan pipeline as targeting.
// Returns 0 = nothing to do (caller idles), 1 = plan active,
// 2 = grid fallback already drove the bot toward safety.
// =========================================================

int CBotNet::MoveToSafety(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
        int LocalID, const vec2 &MyPos)
{
        if(!m_MapGridLoaded)
                return 0;

        int botTX = pf_clamp((int)(MyPos.x / 32.0f), 0, m_MapWidth - 1);
        int botTY = pf_clamp((int)(MyPos.y / 32.0f), 0, m_MapHeight - 1);

        // Safe-spot check is shared: CBotNet::IsSafeSpot (walkable, no
        // LOS-visible freeze within SAFETY_MARGIN tiles).

        // Already standing in a safe spot — nothing to retreat from.
        if(IsSafeSpot(botTX, botTY))
                return 0;

        // Nearest safe tiles first (ring scan around the bot).  Grounded
        // spots — solid ground directly below — are collected ahead of
        // mid-air ones: the bot can't be bumped off solid ground.
        const int MAX_SCAN_RADIUS = 12;
        int goalTX[8], goalTY[8];
        int airTX[8], airTY[8];
        int numGoals = 0;
        int numAir = 0;
        for(int r = 1; r <= MAX_SCAN_RADIUS && numGoals < 8; r++)
        {
                for(int oy = -r; oy <= r && numGoals < 8; oy++)
                {
                        for(int ox = -r; ox <= r && numGoals < 8; ox++)
                        {
                                if(abs(ox) != r && abs(oy) != r)
                                        continue; // ring border only
                                int gx = botTX + ox, gy = botTY + oy;
                                if(!IsSafeSpot(gx, gy))
                                        continue;
                                if(gy + 1 < m_MapHeight && !IsTileWalkable(gx, gy + 1))
                                {
                                        goalTX[numGoals] = gx;
                                        goalTY[numGoals] = gy;
                                        numGoals++;
                                }
                                else if(numAir < 8)
                                {
                                        airTX[numAir] = gx;
                                        airTY[numAir] = gy;
                                        numAir++;
                                }
                        }
                }
        }
        // Mid-air fallback when solid ground is out of reach.
        for(int a = 0; a < numAir && numGoals < 8; a++)
        {
                goalTX[numGoals] = airTX[a];
                goalTY[numGoals] = airTY[a];
                numGoals++;
        }
        if(numGoals == 0)
        {
                // No fully safe spot in reach — retreat to the best available:
                // the reachable walkable tile with the largest route distance
                // from freeze (m_pfFreezeDist, walls respected).  Getting
                // away from the pool beats idling on its edge.
                if(!m_pfFreezeDist)
                        return 0;
                int curFd = m_pfFreezeDist[botTY * m_MapWidth + botTX];
                int bestFd = curFd, bestX = -1, bestY = -1;
                for(int r = 1; r <= MAX_SCAN_RADIUS; r++)
                {
                        for(int oy = -r; oy <= r; oy++)
                        {
                                for(int ox = -r; ox <= r; ox++)
                                {
                                        if(abs(ox) != r && abs(oy) != r)
                                                continue;
                                        int gx = botTX + ox, gy = botTY + oy;
                                        if(gx < 0 || gy < 0 || gx >= m_MapWidth || gy >= m_MapHeight)
                                                continue;
                                        if(!IsTileWalkable(gx, gy))
                                                continue;
                                        int fd = m_pfFreezeDist[gy * m_MapWidth + gx];
                                        if(fd > bestFd)
                                        {
                                                bestFd = fd;
                                                bestX = gx;
                                                bestY = gy;
                                        }
                                }
                        }
                }
                if(bestX == -1)
                        return 0; // already at the safest reachable spot
                bool sameGoal = State.m_PathFound && State.m_LastTargetTX == bestX &&
                        State.m_LastTargetTY == bestY &&
                        m_pfDist[botTY * m_MapWidth + botTX] < 1e17f;
                if(!sameGoal)
                {
                        State.m_PathFound = false;
                        ComputePathfinder(botTX, botTY, bestX, bestY, State);
                        if(!State.m_PathFound)
                                return 0; // unreachable — idle
                }
                return MoveAlongField(pGame, pInput, Dummy, State, LocalID, MyPos, bestX, bestY, 0.0f, -1) ? 1 : 2;
        }

        // Keep the current safety goal while the freeze-walled field still
        // reaches it; otherwise pick the nearest safe tile with a route.
        bool botReachable = m_pfDist[botTY * m_MapWidth + botTX] < 1e17f;
        bool haveGoal = State.m_PathFound && State.m_LastTargetTX >= 0 &&
                IsSafeSpot(State.m_LastTargetTX, State.m_LastTargetTY) && botReachable;
        if(!haveGoal)
        {
                State.m_PathFound = false;
                for(int g = 0; g < numGoals && !State.m_PathFound; g++)
                        ComputePathfinder(botTX, botTY, goalTX[g], goalTY[g], State);
                if(!State.m_PathFound)
                        return 0; // no freeze-free route out — idle
        }
        int gtx = State.m_LastTargetTX;
        int gty = State.m_LastTargetTY;

        return MoveAlongField(pGame, pInput, Dummy, State, LocalID, MyPos, gtx, gty, 0.0f, -1) ? 1 : 2;
}

// =========================================================
// STEP 4a — HOOK (the old AutoHook pulse)
// =========================================================

void CBotNet::UpdateHelpersHook(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
        int LocalID, int TargetID, const vec2 &MyPos, const vec2 &TPos, float Dist,
        bool CanShoot, bool TargetIsRescue, bool tgtFrozen)
{
        // The hook drags the target toward the bot: pulls a frozen ally out
        // of the freeze, drags an enemy in.  A frozen attack target is never
        // hooked — that would pull it back out of the freeze.
        bool canHook = CanShoot && Dist < (float)g_Config.m_KxHookDist &&
                (TargetIsRescue || !tgtFrozen) &&
                pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) == 0;
        if(canHook)
        {
                vec2 IntersectPos;
                int HitPlayer = pGame->IntersectCharacter(MyPos, TPos, IntersectPos, LocalID);
                if(HitPlayer != -1 && HitPlayer != TargetID)
                        canHook = false;
        }
        if(canHook)
        {
                int TicksCycle = ComputeHookTicksCycle();
                State.m_HookTickTimer++;
                if(State.m_HookTickTimer >= TicksCycle)
                        State.m_HookTickTimer = 0;
                pInput->m_Hook = (State.m_HookTickTimer < TicksCycle - 2);
        }
        else
        {
                pInput->m_Hook = 0;
                State.m_HookTickTimer = 0;
        }
}

// =========================================================
// ENTRY — one dummy tick of the Helpers feature
// =========================================================

void CBotNet::UpdateHelpers(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID)
{
        vec2 MyPos((float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Y);

        // The grid backs the rescue scan, the guidance field and the plan.
        MaybeReloadMapGrid(0);

        bool TargetIsMain = false;
        bool TargetIsRescue = false;
        bool TargetExplicit = false;
        int TargetID = FindHelpersTarget(pGame, LocalID, MyPos, State, TargetIsMain, TargetIsRescue, TargetExplicit);

        if(TargetID == -1)
        {
                // No task — but an AFK bot must not sit where a hook or hammer
                // can shove it into freeze: retreat to the nearest safe tile
                // first.  0 = nothing to do (idle), 1 = safety plan active,
                // 2 = grid fallback is already driving the bot to safety.
                int Safety = MoveToSafety(pGame, pInput, Dummy, State, LocalID, MyPos);
                if(Safety > 0)
                {
                        // hook is owned by the plan (1) or already cleared by
                        // the fallback (2); fire stays off while AFK
                        pInput->m_Fire = 0;
                        pInput->m_WantedWeapon = 0;
                        pInput->m_PlayerFlags |= 1;
                        return;
                }
                SetDirection(pGame, Dummy, false, false);
                pInput->m_Hook = 0;
                pInput->m_Fire = 0;
                pInput->m_WantedWeapon = 0;
                State.m_HookTickTimer = 0;
                State.m_HelpersPlan.Reset();
                return;
        }

        // LIFE FIRST — survival outranks attack, escort, the walk to the
        // main and rescue alike: while the bot stands where a hook or
        // hammer can drop it into freeze, the safety retreat owns the
        // tick; the task resumes once the spot is safe again (rescue is
        // re-picked the same tick it is safe to approach again).
        if(m_MapGridLoaded &&
                MoveToSafety(pGame, pInput, Dummy, State, LocalID, MyPos) > 0)
        {
                pInput->m_Fire = 0;
                pInput->m_WantedWeapon = 0;
                pInput->m_PlayerFlags |= 1;
                return;
        }

        auto &TChar = pGame->m_Snap.m_aCharacters[TargetID].m_Cur;
        vec2 TPos((float)TChar.m_X, (float)TChar.m_Y);
        float dx = TPos.x - MyPos.x;
        float dy = TPos.y - MyPos.y;
        float Dist = distance(MyPos, TPos);

        // Everything is shootable except the escorted main (an explicitly
        // targeted main is an enemy, not escort).
        bool CanShoot = !TargetIsMain || TargetExplicit;
        bool tgtFrozen = pGame->m_aClients[TargetID].m_FreezeEnd != 0 || pGame->m_aClients[TargetID].m_DeepFrozen;

        // Stand close only while escorting the main — never while rescuing
        // (the bot must reach the frozen ally) and never against an enemy.
        bool stand = TargetIsMain && !TargetIsRescue && !TargetExplicit &&
                Dist < EffectiveStandDist(TargetIsMain, TargetIsRescue);

        bool planActive = false;
        if(!stand && m_MapGridLoaded)
                planActive = MoveToHelpersTarget(pGame, pInput, Dummy, State, LocalID, TargetID, MyPos, TPos, dx, TargetIsRescue);
        if(stand)
        {
                // Standing next to the main is no excuse to sit at the freeze
                // edge: if the spot is unsafe, the safety retreat owns the
                // tick instead of the stand.
                if(m_MapGridLoaded && MoveToSafety(pGame, pInput, Dummy, State, LocalID, MyPos) > 0)
                {
                        pInput->m_Fire = 0;
                        pInput->m_PlayerFlags |= 1;
                        return;
                }
                SetDirection(pGame, Dummy, false, false);
                pInput->m_Jump = 0;
                State.m_HelpersPlan.Reset();
        }

        // === AIM === the plan owns the aim while it replays; otherwise the
        // dummy aims straight at the target.
        if(!planActive)
                SetMousePos(pGame, Dummy, vec2(dx, dy));

        // === HOOK === skipped while the plan replays — it manages the hook itself.
        if(!planActive)
                UpdateHelpersHook(pGame, pInput, Dummy, State, LocalID, TargetID, MyPos, TPos, Dist, CanShoot, TargetIsRescue, tgtFrozen);

        // === FIRE === hammer at anything shootable in range — rescue allies
        // included.  Plain fire++, it never fights the plan or the hook.
        if(CanShoot && Dist < (float)g_Config.m_KxFireDist)
                pInput->m_Fire++;
        if(pInput->m_Fire > 0)
                pInput->m_WantedWeapon = 1;

        pInput->m_PlayerFlags |= 1;
}
