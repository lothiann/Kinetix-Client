// (c) Kinetix. Basic Avoid Freeze component.
// Algorithm: "Latest Safe Tick Search" by Claude + Survival Score
//
// Each frame:
//   Test A: "Can I wait 1 more tick?" — simulate 1 tick with current input,
//           then brute-force all combos on remaining ticks. If ANY combo
//           eliminates danger (danger=0) → wait, don't intervene.
//   Test B: "If I act NOW — will it work?" — brute-force all combos from
//           tick 0. Score by survival (how long before danger). Higher = better.
//           Among same survival, prefer minimal input diff.
//
//   If Test A passes → wait (don't intervene).
//   If Test A fails but Test B passes → LAST MOMENT, apply best combo.
//   If both fail → too late, danger inevitable.
//
// Combos include: direction(-1,0,1) × jump(0,1) × hook(0,1) × aim angles.
// Aim angles are centered on current aim, spread evenly over 360°.

#include <game/client/components/kinetix/basic_avoid_freeze.h>

#include <engine/shared/config.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/mapitems.h>
#include <generated/protocol.h>
#include <game/client/components/kinetix/kinetix_internal.h> // SetMousePos

void CBasicAvoidFreeze::OnReset()
{
        m_WasOverriding = false;
        m_OverrideHook = 0;
}

int CBasicAvoidFreeze::InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const
{
        int diff = 0;
        if(A.m_Direction != B.m_Direction) diff++;
        if((A.m_Jump != 0) != (B.m_Jump != 0)) diff++;
        if((A.m_Hook != 0) != (B.m_Hook != 0)) diff++;
        if(A.m_TargetX != B.m_TargetX || A.m_TargetY != B.m_TargetY) diff++;
        return diff;
}

int CBasicAvoidFreeze::SimulateDangerTick(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const
{
        return SimulateDangerTickDelayed(LocalId, Input, 0, Input, Ticks);
}

int CBasicAvoidFreeze::SimulateDangerTickDelayed(int LocalId, const CNetObj_PlayerInput &DelayInput,
        int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks) const
{
        CGameClient *pGame = GameClient();
        CGameWorld SimWorld;
        SimWorld.CopyWorld(&pGame->m_PredictedWorld);

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                if(CCharacter *pChar = SimWorld.GetCharacterById(i))
                        delete pChar;
        }

        CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
        if(!pChar)
                return 0;

        bool avoidFreeze = g_Config.m_KxBafAvoidFreeze != 0;
        bool avoidTeleport = g_Config.m_KxBafAvoidTeleport != 0;
        bool avoidDeath = g_Config.m_KxBafAvoidDeath != 0;
        CCollision *pCol = pChar->Collision();

        int StartTick = SimWorld.GameTick();
        for(int t = 0; t < Ticks; t++)
        {
                const CNetObj_PlayerInput &In = (t < Delay) ? DelayInput : ComboInput;
                pChar->OnDirectInput(&In);
                SimWorld.m_GameTick = StartTick + t + 1;
                pChar->OnPredictedInput(&In);
                SimWorld.Tick();

                if(!SimWorld.GetCharacterById(LocalId))
                        return avoidDeath ? (t + 1) : 0;

                if(avoidFreeze && pChar->m_FreezeTime > 0)
                        return t + 1;

                if(avoidDeath)
                {
                        int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
                        if(Idx >= 0)
                        {
                                int Tile = pCol->GetTileIndex(Idx);
                                int FTile = pCol->GetFrontTileIndex(Idx);
                                if(Tile == TILE_DEATH || FTile == TILE_DEATH)
                                        return t + 1;
                        }
                }

                if(avoidTeleport)
                {
                        int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
                        if(Idx >= 0)
                        {
                                if(pCol->IsTeleport(Idx) || pCol->IsEvilTeleport(Idx))
                                        return t + 1;
                        }
                }
        }
        return 0;
}

void CBasicAvoidFreeze::OnUpdate()
{
}

void CBasicAvoidFreeze::ApplyOverride()
{
        if(!g_Config.m_KxBasicAvoidFreeze)
                return;

        CGameClient *pGame = GameClient();
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0 || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
                return;

        CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pLocalChar)
                return;

        CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
        CNetObj_PlayerInput Current = *pInput;

        int simTicks = g_Config.m_KxBafTicks;
        if(simTicks < 1) simTicks = 1;
        if(simTicks > 20) simTicks = 20;

        // Check danger WITH the current input (including the user's hook state).
        // This detects when the hook itself is pulling the player into freeze.
        int dangerWithCurrent = SimulateDangerTick(LocalId, Current, simTicks);

        // Check danger WITHOUT hook (m_Hook=0). This is the "natural" danger state —
        // if the player isn't holding hook, are they in danger? This prevents BAF
        // from staying inactive when the player holds hook that masks the danger
        // (hook keeps them safe temporarily, but danger is still there without it).
        CNetObj_PlayerInput NoHook = Current;
        NoHook.m_Hook = 0;
        int dangerWithoutHook = SimulateDangerTick(LocalId, NoHook, simTicks);

        // No danger at all (with or without hook) — safe. Release any BAF override.
        if(dangerWithCurrent == 0 && dangerWithoutHook == 0)
        {
                if(m_WasOverriding)
                {
                        if(m_OverrideHook != 0)
                                pInput->m_Hook = 0; // Retract BAF's forced hook=1
                        m_WasOverriding = false;
                }
                return;
        }

        // Danger WITH hook, but safe WITHOUT hook — the user's hook IS the problem.
        // Use "latest safe tick search" to release at the VERY last moment
        // (same pattern as BAF's override hook in Test A below).
        if(dangerWithCurrent > 0 && dangerWithoutHook == 0)
        {
                // Test A: Can we wait 1 more tick with hook still active?
                // Simulate 1 tick with current input (hook=1), then release hook
                // for remaining ticks. If safe → wait, don't intervene yet.
                int delayedDanger = SimulateDangerTickDelayed(LocalId, Current, 1, NoHook, simTicks);

                if(delayedDanger == 0)
                {
                        // Can wait 1 more tick — hook is safe for now.
                        return;
                }

                // Can't wait — LAST MOMENT. Release hook NOW.
                pInput->m_Hook = 0;
                m_WasOverriding = true;
                m_OverrideHook = 0;
                return;
        }

        // Danger exists even without hook — use brute-force to find alternative.
        int currentDanger = dangerWithoutHook;

        // Build combo lists.
        bool allowDir = g_Config.m_KxBafDirection != 0;
        bool allowJump = g_Config.m_KxBafJump != 0;
        bool allowHook = g_Config.m_KxBafHook != 0;
        bool allowAim = g_Config.m_KxBafAim != 0;

        int dirs[3] = {0, 0, 0};
        int dirCount = 1;
        if(allowDir) { dirs[0] = -1; dirs[1] = 0; dirs[2] = 1; dirCount = 3; }
        else { dirs[0] = Current.m_Direction; }

        int jumps[2] = {0, 0};
        int jumpCount = 1;
        if(allowJump) { jumps[0] = 0; jumps[1] = 1; jumpCount = 2; }
        else { jumps[0] = Current.m_Jump != 0 ? 1 : 0; }

        int hooks[2] = {0, 0};
        int hookCount = 1;
        if(allowHook) { hooks[0] = 0; hooks[1] = 1; hookCount = 2; }
        else { hooks[0] = Current.m_Hook != 0 ? 1 : 0; }

        // Aim angles: centered on current aim, spread within FOV cone.
        vec2 aimTargets[145];
        int aimCount = 1;
        aimTargets[0] = vec2((float)Current.m_TargetX, (float)Current.m_TargetY);
        if(allowAim)
        {
                int numAngles = g_Config.m_KxBafAngles;
                if(numAngles < 1) numAngles = 1;
                if(numAngles > 144) numAngles = 144;

                float fovRad = (float)g_Config.m_KxBafFov * (3.14159265f / 180.0f);
                float curAngle = atan2f((float)Current.m_TargetY, (float)Current.m_TargetX);
                float aimDist = sqrtf((float)Current.m_TargetX * Current.m_TargetX + (float)Current.m_TargetY * Current.m_TargetY);
                if(aimDist < 1.0f) aimDist = 100.0f;

                aimCount = 0;
                for(int a = 0; a < numAngles; a++)
                {
                        // Spread evenly within [-FOV/2, +FOV/2] from current aim.
                        float t = (numAngles == 1) ? 0.0f : (float)a / (float)(numAngles - 1) - 0.5f;
                        float angleOffset = t * fovRad;
                        float angle = curAngle + angleOffset;
                        aimTargets[aimCount] = vec2(cosf(angle) * aimDist, sinf(angle) * aimDist);
                        aimCount++;
                }
        }

        // =====================================================
        // Test A: "Can I wait 1 more tick?"
        // =====================================================
        bool canWait = false;
        for(int di = 0; di < dirCount && !canWait; di++)
        {
                for(int ji = 0; ji < jumpCount && !canWait; ji++)
                {
                        for(int hi = 0; hi < hookCount && !canWait; hi++)
                        {
                                for(int ai = 0; ai < aimCount && !canWait; ai++)
                                {
                                        CNetObj_PlayerInput test = Current;
                                        test.m_Direction = dirs[di];
                                        test.m_Jump = jumps[ji];
                                        test.m_Hook = hooks[hi];
                                        test.m_TargetX = (int)aimTargets[ai].x;
                                        test.m_TargetY = (int)aimTargets[ai].y;

                                        int delayDanger = SimulateDangerTickDelayed(LocalId, Current, 1, test, simTicks);
                                        if(delayDanger == 0)
                                        {
                                                canWait = true;
                                        }
                                }
                        }
                }
        }

        if(canWait)
                return;

        // =====================================================
        // Test B: "Act NOW" — brute-force all combos from tick 0.
        // =====================================================
        bool found = false;
        CNetObj_PlayerInput bestInput = Current;
        int bestSurvival = -1;
        int bestDiff = 999;

        for(int di = 0; di < dirCount; di++)
        {
                for(int ji = 0; ji < jumpCount; ji++)
                {
                        for(int hi = 0; hi < hookCount; hi++)
                        {
                                for(int ai = 0; ai < aimCount; ai++)
                                {
                                        CNetObj_PlayerInput test = Current;
                                        test.m_Direction = dirs[di];
                                        test.m_Jump = jumps[ji];
                                        test.m_Hook = hooks[hi];
                                        test.m_TargetX = (int)aimTargets[ai].x;
                                        test.m_TargetY = (int)aimTargets[ai].y;

                                        // Skip exact current input.
                                        if(test.m_Direction == Current.m_Direction &&
                                           (test.m_Jump != 0) == (Current.m_Jump != 0) &&
                                           (test.m_Hook != 0) == (Current.m_Hook != 0) &&
                                           test.m_TargetX == Current.m_TargetX &&
                                           test.m_TargetY == Current.m_TargetY)
                                                continue;

                                        int danger = SimulateDangerTick(LocalId, test, simTicks);
                                        int survival = (danger == 0) ? simTicks : danger - 1;
                                        int diff = InputDiff(Current, test);

                                        if(bestSurvival < 0 ||
                                           survival > bestSurvival ||
                                           (survival == bestSurvival && diff < bestDiff))
                                        {
                                                bestSurvival = survival;
                                                bestDiff = diff;
                                                bestInput = test;
                                                found = true;
                                        }
                                }
                        }
                }
        }

        // Apply if best combo survives longer than current.
        int currentSurvival = currentDanger - 1;
        if(found && bestSurvival > currentSurvival)
        {
                pInput->m_Direction = bestInput.m_Direction;
                pInput->m_Jump = bestInput.m_Jump;
                pInput->m_Hook = bestInput.m_Hook;

                // Track hook override so next tick's NoHook danger check can release it
                // once danger clears (see NoHook block at the top of ApplyOverride).
                m_WasOverriding = true;
                m_OverrideHook = bestInput.m_Hook;

                // Apply aim: silent or visible.
                if(allowAim && (bestInput.m_TargetX != Current.m_TargetX || bestInput.m_TargetY != Current.m_TargetY))
                {
                        if(g_Config.m_KxBafSilent)
                        {
                                // Silent: use laser unfreeze aim channel (same as aimbot).
                                // SnapInput reads m_LaserUnfreezeAimActive and applies offset
                                // to m_aInputData.m_TargetX/Y right before sending to server.
                                // Local prediction keeps the real aim (no visible crosshair move).
                                // Fake aim is automatically blocked (checks !laserAimActive).
                                //
                                // v1.56.169 BUG7: guard !m_LaserUnfreezeAimActive — Laser Unfreeze
                                // and AimBot run in OnUpdate (before SnapInput), so if they already
                                // claimed the channel this tick, don't overwrite their offset.
                                // BAF still applies dir/jump/hook (those are separate fields).
                                if(!pGame->m_Controls.m_LaserUnfreezeAimActive)
                                {
                                        pGame->m_Controls.m_LaserUnfreezeAimActive = true;
                                        pGame->m_Controls.m_LaserUnfreezeAimOffset = vec2((float)bestInput.m_TargetX, (float)bestInput.m_TargetY);
                                }
                        }
                        else
                        {
                                // Visible: move m_aMousePos + m_aInputData via SetMousePos helper,
                                // exactly like Laser Unfreeze non-silent (laser_unfreeze.cpp:312)
                                // and AimBot Trigger/Always (aimbot.cpp:353).
                                // Fixes broken visible mode: Aim wasn't writing coordinates correctly.
                                SetMousePos(pGame, g_Config.m_ClDummy, vec2((float)bestInput.m_TargetX, (float)bestInput.m_TargetY));
                        }
                }
        }
}
