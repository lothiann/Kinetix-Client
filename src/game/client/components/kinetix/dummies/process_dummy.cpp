#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

void CBotNet::ProcessDummy(int Dummy)
{
        CGameClient *pGame = GameClient();
        CBotNetDummy &State = m_aDummies[Dummy];
        CNetObj_PlayerInput *pInput = &pGame->m_aDummyInput[Dummy];
        int LocalID = pGame->m_aLocalIds[Dummy];
        int64_t CurTick = Client()->GameTick(0);

        // --- Early-return stages (each handler commits inputs + returns true) ---

        // Frozen dummies can't act — optionally kill-on-freeze, then zero inputs.
        if(HandleFrozenDummy(pGame, pInput, Dummy, LocalID))
                return;

        // No feature active — clear inputs and bail.
        if(HandleNoFeatureIdle(pGame, pInput, Dummy, State))
                return;

        // Macro playback (sleep gating + input/aim dispatch).
        if(ProcessMacroPlayback(pGame, pInput, Dummy, State))
                return;

        // Macro recording (delta-encoded input/aim capture, with 100k-line guard).
        if(ProcessMacroRecording(pGame, pInput, Dummy, State, LocalID, CurTick))
                return;

        // Throttle actions to one per g_Config.m_KxClientDelay ms (per-dummy static timer).
        if(ApplyClientDelayGate(pGame, pInput, Dummy))
                return;

        // Kill-on-freeze (fall-through — does NOT return; later stages still run).
        if(g_Config.m_KxKillFrz && pGame->m_aClients[LocalID].m_FreezeEnd > 0)
                IssueKillForDummy(Dummy);

        // Mirror another client's input.  Falls through when target inactive.
        if(ProcessCopyMoves(pGame, pInput, Dummy))
                return;

        // "Go to position" pathfinder (A* + flow-field movement toward m_PathfinderGoPos).
        if(ProcessPathfinderGo(pGame, pInput, Dummy, State, LocalID))
                return;

        // ===== ATTACK =====
        if(g_Config.m_KxAttack)
        {
                int TargetID = -1;
                float MinDist = 1000000.0f;
                vec2 MyPos((float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Y);
                bool TargetIsMain = false;
                bool TargetIsRescue = false;

                // ===== RESCUE FROZEN + SMART RESCUE =====
                bool usingSmartRescue = false;
                if(g_Config.m_KxRescue)
                {
                        int rescueCandidates[128];
                        float rescueDist[128];
                        int numCandidates = 0;

                        for(int i = 0; i < 128; i++)
                        {
                                if(i == LocalID || !pGame->m_aClients[i].m_Active)
                                        continue;
                                if(pGame->m_aClients[i].m_FreezeEnd == 0)
                                        continue;

                                bool canRescue = false;
                                if(i == g_Config.m_KxMain)
                                        canRescue = true;
                                else if(m_BotsList[i])
                                        canRescue = true;
                                else if(g_Config.m_KxRescueAll)
                                {
                                        canRescue = g_Config.m_KxTargetAll ? m_TargetList[i] : !m_TargetList[i];
                                        if(m_RescueList[i])
                                                canRescue = false;
                                }
                                else if(m_RescueList[i])
                                        canRescue = true;

                                if(!canRescue)
                                        continue;

                                vec2 TPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                                if(!g_Config.m_KxSmartDetect)
                                {
                                        if(pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) > 0)
                                                continue;
                                }
                                float d = distance(MyPos, TPos);
                                if(d < g_Config.m_KxRescueRadius)
                                {
                                        rescueCandidates[numCandidates] = i;
                                        rescueDist[numCandidates] = d;
                                        numCandidates++;
                                }
                        }

                        for(int a = 0; a < numCandidates - 1; a++)
                                for(int b = a + 1; b < numCandidates; b++)
                                        if(rescueDist[b] < rescueDist[a])
                                        {
                                                int tmp = rescueCandidates[a];
                                                rescueCandidates[a] = rescueCandidates[b];
                                                rescueCandidates[b] = tmp;
                                                float td = rescueDist[a];
                                                rescueDist[a] = rescueDist[b];
                                                rescueDist[b] = td;
                                        }

                        for(int j = 0; j < numCandidates; j++)
                        {
                                int i = rescueCandidates[j];
                                TargetID = i;
                                TargetIsRescue = true;

                                if(g_Config.m_KxSmartRescue && g_Config.m_KxAtkPathfinder && m_MapGridLoaded)
                                {
                                        int botTX = (int)(MyPos.x / 32.0f);
                                        int botTY = (int)(MyPos.y / 32.0f);
                                        int resTX = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X / 32.0f);
                                        int resTY = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y / 32.0f);

                                        UpdatePlayerPenalty(botTX, botTY, resTX, resTY, LocalID);

                                        ComputePathfinderRescue(botTX, botTY, resTX, resTY, State);

                                        if(State.m_PathFound)
                                        {
                                                usingSmartRescue = true;
                                                break;
                                        }
                                }
                                else
                                {
                                        break;
                                }

                                TargetID = -1;
                                TargetIsRescue = false;
                        }
                }

                if(TargetID == -1)
                {
                        for(int i = 0; i < 128; i++)
                        {
                                if(i == LocalID || i == g_Config.m_KxMain || !pGame->m_aClients[i].m_Active)
                                        continue;
                                if(m_BotsList[i])
                                        continue;
                                if(pGame->m_aClients[i].m_FreezeEnd != 0)
                                        continue;

                                bool isEnemy = (g_Config.m_KxTargetAll ? !m_TargetList[i] : m_TargetList[i]);
                                if(!isEnemy)
                                        continue;

                                vec2 TPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                                if(pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) > 0)
                                        continue;
                                float d = distance(MyPos, TPos);

                                if(g_Config.m_KxTargetDist > 0 && d > g_Config.m_KxTargetDist)
                                        continue;

                                if(d < MinDist)
                                {
                                        MinDist = d;
                                        TargetID = i;
                                }
                        }
                }

                if(TargetID == -1 && g_Config.m_KxMain >= 0 && g_Config.m_KxMain < 128)
                {
                        if(pGame->m_aClients[g_Config.m_KxMain].m_Active)
                        {
                                vec2 MainPos((float)pGame->m_Snap.m_aCharacters[g_Config.m_KxMain].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[g_Config.m_KxMain].m_Cur.m_Y);
                                float MainDist = distance(MyPos, MainPos);
                                if(MainDist <= g_Config.m_KxMainDist)
                                {
                                        TargetID = g_Config.m_KxMain;
                                        TargetIsMain = true;
                                }
                        }
                }

                if(TargetID != -1)
                {
                        auto &TChar = pGame->m_Snap.m_aCharacters[TargetID].m_Cur;
                        float dx = (float)TChar.m_X - MyPos.x;
                        float dy = (float)TChar.m_Y - MyPos.y;
                        float Dist = distance(MyPos, vec2((float)TChar.m_X, (float)TChar.m_Y));

                        if(g_Config.m_KxAutoAim)
                                SetMousePos(pGame, Dummy, vec2(dx, dy));

                        bool standActive = false;

                        if(g_Config.m_KxMove)
                        {
                                bool left = false, right = false, jump = false;

                                if(g_Config.m_KxAtkPathfinder && !usingSmartRescue)
                                {
                                        // --- PATHFINDER MOVEMENT ---
                                        // Shared 5s reload slot with PathfinderGo (legacy behaviour).
                                        MaybeReloadMapGrid(0);

                                        if(m_MapGridLoaded)
                                        {
                                                int botTX = (int)(MyPos.x / 32.0f);
                                                int botTY = (int)(MyPos.y / 32.0f);
                                                int targetTX = (int)((float)TChar.m_X / 32.0f);
                                                int targetTY = (int)((float)TChar.m_Y / 32.0f);

                                                UpdatePlayerPenalty(botTX, botTY, targetTX, targetTY, LocalID);

                                                bool needRecalc = false;
                                                if(targetTX != State.m_LastTargetTX || targetTY != State.m_LastTargetTY)
                                                        needRecalc = true;
                                                if(!State.m_PathFound && (State.m_LastTargetTX == -1 || targetTX != State.m_LastTargetTX || targetTY != State.m_LastTargetTY))
                                                        needRecalc = true;
                                                if(State.m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                                {
                                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                                needRecalc = true;
                                                }
                                                if(g_Config.m_KxPfSimulatePlayers)
                                                        needRecalc = true;

                                                if(needRecalc && IsTileWalkable(targetTX, targetTY))
                                                        ComputePathfinder(botTX, botTY, targetTX, targetTY, State);

                                                if(State.m_PathFound && (botTX != State.m_LastBotTX || botTY != State.m_LastBotTY))
                                                {
                                                        ComputeFlowForTile(botTY, botTX, State);
                                                        State.m_LastBotTX = botTX;
                                                        State.m_LastBotTY = botTY;
                                                }

                                                // --- KINODYNAMIC A* MOVEMENT ---
                                                // Cache near-freeze once: shared by the kinodynamic gate and the stand check.
                                                bool nearFreeze = IsNearFreeze(botTX, botTY);

                                                if(g_Config.m_KxKinodynamic && State.m_PathFound && !nearFreeze)
                                                {
                                                        CKinodynamicCache &kinoCache = State.m_KinoCache;

                                                        // Check if cache needs recomputation
                                                        bool kinoNeedRecompute = false;
                                                        if(!kinoCache.IsValid())
                                                                kinoNeedRecompute = true;
                                                        else if(kinoCache.IsExpired(g_Config.m_KxKinoCacheTicks))
                                                                kinoNeedRecompute = true;
                                                        else if(kinoCache.TargetTX != targetTX || kinoCache.TargetTY != targetTY)
                                                                kinoNeedRecompute = true;
                                                        else
                                                        {
                                                                // Check if bot drifted from the *expected* position on the
                                                                // planned path (not from the compute-time start — normal
                                                                // movement is not drift).  Compare against vPath[CurrentTick].
                                                                vec2 expectedPos = kinoCache.BotPos;
                                                                if(kinoCache.CurrentTick >= 0 && kinoCache.CurrentTick < (int)kinoCache.vPath.size())
                                                                        expectedPos = kinoCache.vPath[kinoCache.CurrentTick];
                                                                float drift = distance(MyPos, expectedPos);
                                                                if(drift > 48.0f) // 1.5 tiles off the planned trajectory
                                                                        kinoNeedRecompute = true;
                                                        }

                                                        if(kinoNeedRecompute)
                                                                ComputeKinodynamic(Dummy, MyPos, botTX, botTY, targetTX, targetTY, State);

                                                        // Apply cached kinodynamic inputs
                                                        if(kinoCache.IsValid())
                                                        {
                                                                // Check if bot should stand (close enough to target)
                                                                bool kinoStandActive = false;
                                                                float standDist = EffectiveStandDist(TargetIsMain, TargetIsRescue);
                                                                if(g_Config.m_KxStand && !nearFreeze &&
                                                                        ((!g_Config.m_KxStandOnX && Dist < standDist) ||
                                                                                (g_Config.m_KxStandOnX && absolute(dx) < standDist)))
                                                                {
                                                                        kinoStandActive = true;
                                                                }

                                                                if(kinoStandActive)
                                                                {
                                                                        // Stand: clear movement, let normal SetDirection handle it
                                                                        left = false;
                                                                        right = false;
                                                                        jump = false;
                                                                        pInput->m_Hook = 0;
                                                                        // Don't goto after_kino_movement — let SetDirection + stand logic run
                                                                }
                                                                else
                                                                {
                                                                        ApplyKinodynamicCache(Dummy, pInput, State);
                                                                        // Skip the normal movement flow
                                                                        goto after_kino_movement;
                                                                }
                                                        }

                                                        // Cache invalid after compute (no valid path found) — fall through to normal movement
                                                }

                                                // Normal pathfinder movement — used when:
                                                // 1. Kinodynamic is disabled
                                                // 2. Bot is near freeze (kinodynamic skips to let avoid-freeze logic work)
                                                // 3. Kinodynamic cache is invalid (no valid path found by simulation)
                                                if(!g_Config.m_KxKinodynamic || nearFreeze || !State.m_KinoCache.IsValid())
                                                {
                                                        GetMovementFromFlow(State, left, right, jump);
                                                        // When falling back from kinodynamic to normal movement, clear hook
                                                        // because auto-hook code below is skipped when kinodynamic is enabled
                                                        if(g_Config.m_KxKinodynamic)
                                                                pInput->m_Hook = 0;
                                                }

                                                if(g_Config.m_KxAtkPathfinderSnap && State.m_PathFound && !left && !right)
                                                        ApplyPfSnap(MyPos, left, right);

                                                // nearFreeze cached above (shared with the kinodynamic gate).
                                                float standDist = EffectiveStandDist(TargetIsMain, TargetIsRescue);
                                                if(g_Config.m_KxStand && !nearFreeze &&
                                                        ((!g_Config.m_KxStandOnX && Dist < standDist) ||
                                                                (g_Config.m_KxStandOnX && absolute(dx) < standDist)))
                                                {
                                                        left = false;
                                                        right = false;
                                                        standActive = true;
                                                }

                                                {
                                                        bool doJump = jump;

                                                        if(!g_Config.m_KxPfSimulatePlayers)
                                                        {
                                                                vec2 IntersectPos;
                                                                int HitPlayer = pGame->IntersectCharacter(MyPos, MyPos + vec2((dx > 0 ? 1.0f : -1.0f) * 40.0f, 0), IntersectPos, LocalID);
                                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                                        doJump = true;
                                                        }

                                                        jump = doJump;
                                                }
                                        }
                                }
                                else if(g_Config.m_KxAtkPathfinder && usingSmartRescue)
                                {
                                        // --- SMART RESCUE PATHFINDER MOVEMENT ---
                                        // Separate 5s reload slot (BranchB keeps its own timer, legacy behaviour).
                                        MaybeReloadMapGrid(1);

                                        if(m_MapGridLoaded)
                                        {
                                                int botTX = (int)(MyPos.x / 32.0f);
                                                int botTY = (int)(MyPos.y / 32.0f);

                                                int resTX = (int)((float)TChar.m_X / 32.0f);
                                                int resTY = (int)((float)TChar.m_Y / 32.0f);

                                                bool needRecalc = false;
                                                if(resTX != State.m_LastTargetTX || resTY != State.m_LastTargetTY)
                                                        needRecalc = true;
                                                if(!State.m_PathFound)
                                                        needRecalc = true;
                                                if(State.m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                                {
                                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                                needRecalc = true;
                                                }
                                                if(g_Config.m_KxPfSimulatePlayers)
                                                        needRecalc = true;

                                                UpdatePlayerPenalty(botTX, botTY, -1, -1, LocalID);

                                                if(needRecalc)
                                                        ComputePathfinderRescue(botTX, botTY, resTX, resTY, State);

                                                if(State.m_PathFound && (botTX != State.m_LastBotTX || botTY != State.m_LastBotTY))
                                                {
                                                        ComputeFlowForTile(botTY, botTX, State);
                                                        State.m_LastBotTX = botTX;
                                                        State.m_LastBotTY = botTY;
                                                }

                                                GetMovementFromFlow(State, left, right, jump);

                                                bool nearFreeze = IsNearFreeze(botTX, botTY);
                                                float standDist = EffectiveStandDist(TargetIsMain, TargetIsRescue);
                                                if(g_Config.m_KxStand && !nearFreeze &&
                                                        ((!g_Config.m_KxStandOnX && Dist < standDist) ||
                                                                (g_Config.m_KxStandOnX && absolute(dx) < standDist)))
                                                {
                                                        left = false;
                                                        right = false;
                                                        standActive = true;
                                                }

                                                if(g_Config.m_KxAtkPathfinderSnap && State.m_PathFound && !left && !right)
                                                        ApplyPfSnap(MyPos, left, right);
                                        }
                                }
                                else
                                {
                                        float standDist = EffectiveStandDist(TargetIsMain, TargetIsRescue);
                                        if(!g_Config.m_KxStand || (!g_Config.m_KxStandOnX && Dist >= standDist) || (g_Config.m_KxStandOnX && absolute(dx) >= standDist))
                                        {
                                                left = (dx < -20.0f);
                                                right = (dx > 20.0f);
                                        }
                                        else
                                        {
                                                standActive = true;
                                        }

                                        SetDirection(pGame, Dummy, left, right);

                                        {
                                                bool doJump = false;

                                                if(pGame->Collision()->CheckPoint(vec2(MyPos.x + (dx > 0 ? 35 : -35), MyPos.y)))
                                                        doJump = true;

                                                if(dy < -60.0f && absolute(dx) < 128.0f)
                                                        doJump = true;

                                                vec2 IntersectPos;
                                                int HitPlayer = pGame->IntersectCharacter(MyPos, MyPos + vec2((dx > 0 ? 1.0f : -1.0f) * 40.0f, 0), IntersectPos, LocalID);
                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                        doJump = true;

                                                if(doJump)
                                                {
                                                        if(State.m_JumpTicks == 0)
                                                        {
                                                                pInput->m_Jump = 1;
                                                                State.m_JumpTicks = 1;
                                                        }
                                                        else
                                                        {
                                                                pInput->m_Jump = 0;
                                                                State.m_JumpTicks = 0;
                                                        }
                                                }
                                                else
                                                {
                                                        pInput->m_Jump = 0;
                                                        State.m_JumpTicks = 0;
                                                }
                                        }

                                        goto after_movement;
                                }

                                SetDirection(pGame, Dummy, left, right);

                                if(jump)
                                {
                                        if(State.m_JumpTicks == 0)
                                        {
                                                pInput->m_Jump = 1;
                                                State.m_JumpTicks = 1;
                                        }
                                        else
                                        {
                                                pInput->m_Jump = 0;
                                                State.m_JumpTicks = 0;
                                        }
                                }
                                else
                                {
                                        pInput->m_Jump = 0;
                                        State.m_JumpTicks = 0;
                                }
                        }

        after_kino_movement:
                after_movement:

                        bool CanShoot = !TargetIsMain || g_Config.m_KxAtkMain;
                        if(g_Config.m_KxAutoFire && Dist < g_Config.m_KxFireDist && CanShoot)
                                pInput->m_Fire++;

                        // Laser rescue: skip when Kinodynamic A* is active (it manages hook/aim itself)
                        bool usingLaserRescue = false;
                        if(!g_Config.m_KxKinodynamic && g_Config.m_KxLaserRescue)
                        {
                                if(TargetIsRescue)
                                {
                                        vec2 TPos((float)TChar.m_X, (float)TChar.m_Y);
                                        int targetMapIdx = pGame->Collision()->GetPureMapIndex(TPos);
                                        int targetTile = pGame->Collision()->GetTileIndex(targetMapIdx);
                                        int targetFrontTile = pGame->Collision()->GetFrontTileIndex(targetMapIdx);
                                        bool targetInFreezeTile = (targetTile == TILE_FREEZE || targetFrontTile == TILE_FREEZE);

                                        if(!targetInFreezeTile)
                                        {
                                                bool hasLaser = pGame->m_aClients[LocalID].m_Predicted.m_aWeapons[4].m_Got; // WEAPON_LASER=4

                                                if(hasLaser)
                                                {
                                                        bool blocked = (pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) != 0);
                                                        if(!blocked)
                                                        {
                                                                vec2 IntersectPos;
                                                                int HitPlayer = pGame->IntersectCharacter(MyPos, TPos, IntersectPos, LocalID);
                                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                                        blocked = true;
                                                        }

                                                        if(!blocked && Dist < g_Config.m_KxLaserRescueDist)
                                                        {
                                                                SetMousePos(pGame, Dummy, vec2(dx, dy));
                                                                pInput->m_WantedWeapon = 5; // WEAPON_LASER + 1 (1-based)
                                                                pInput->m_Fire++;
                                                                usingLaserRescue = true;
                                                        }
                                                }
                                        }
                                }
                        }

                        if(g_Config.m_KxHammer && pInput->m_Fire > 0 && !usingLaserRescue)
                                pInput->m_WantedWeapon = 1;

                        // Auto-hook: skip when Kinodynamic A* is active (it manages hook itself)
                        if(!g_Config.m_KxKinodynamic)
                        {
                                if(g_Config.m_KxAutoHook && Dist < g_Config.m_KxHookDist && CanShoot)
                                {
                                        vec2 TPos((float)TChar.m_X, (float)TChar.m_Y);
                                        bool canHook = (pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) == 0);
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
                                else
                                {
                                        pInput->m_Hook = 0;
                                        State.m_HookTickTimer = 0;
                                }
                        }

                        // === Pathfinder Hook === (skipped when Kinodynamic A* is active — it manages hook itself)
                        if(!g_Config.m_KxKinodynamic)
                        {
                                if(!g_Config.m_KxPfHook || standActive || pInput->m_Hook != 0 ||
                                        (State.m_PfHookTile.x == 0 && State.m_PfHookTile.y == 0))
                                {
                                        // v1.56.168 BUG6: do NOT reset m_HookTickTimer here.
                                        // This if-branch runs every tick when pathfinder hook is inactive
                                        // (g_Config.m_KxPfHook=false, or standActive, or AutoHook already
                                        // set pInput->m_Hook, or no PfHookTile). The timer is SHARED with
                                        // AutoHook above (line 550 increments it, line 553 derives
                                        // pInput->m_Hook from it). Resetting it here every tick killed
                                        // AutoHook's pulse cycle: timer never reached TicksCycle, so
                                        // pInput->m_Hook stayed 1 forever and the dummy hooked once and
                                        // never re-hooked. The timer is owned by AutoHook (which resets
                                        // it in its own else-branches on lines 558/564) and by the
                                        // pathfinder-hook else-branch below (lines 591-600) when
                                        // pathfinder hook is actually active. Doing nothing here is safe.
                                }
                                else
                                {
                                        int TicksCycle = ComputeHookTicksCycle();

                                        float hDx = State.m_PfHookTile.x - MyPos.x;
                                        float hDy = State.m_PfHookTile.y - MyPos.y;
                                        float hDist = sqrtf(hDx * hDx + hDy * hDy);
                                        if(hDist > 0.001f && hDist < g_Config.m_KxHookDist)
                                        {
                                                SetMousePos(pGame, Dummy, vec2(hDx, hDy));
                                                pInput->m_Hook = 1;
                                        }

                                        if(pInput->m_Hook != 0)
                                        {
                                                State.m_HookTickTimer++;
                                                if(State.m_HookTickTimer >= TicksCycle)
                                                {
                                                        pInput->m_Hook = 0;
                                                        State.m_HookTickTimer = 0;
                                                }
                                        }
                                        else
                                        {
                                                State.m_HookTickTimer = 0;
                                        }
                                }
                        }

                        pInput->m_PlayerFlags |= 1;
                }
                else
                {
                        SetDirection(pGame, Dummy, false, false);
                        pInput->m_Hook = 0;
                        pInput->m_Fire = 0;
                        pInput->m_WantedWeapon = 0;
                        State.m_HookTickTimer = 0;
                }
        }

        // Random aim (fall-through — overrides aim on a schedule, does not return).
        ProcessRandomAim(Dummy, State, CurTick);

        // Always copy back at end
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
}

// =========================================================
// IS DUMMY ACTIVE
// =========================================================
