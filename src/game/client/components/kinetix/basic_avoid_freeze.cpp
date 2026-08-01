// (c) Kinetix. Basic Avoid Freeze component — see basic_avoid_freeze.h for overview.

#include <game/client/components/kinetix/basic_avoid_freeze.h>

#include <engine/shared/config.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <generated/protocol.h>

void CBasicAvoidFreeze::OnReset()
{
}

int CBasicAvoidFreeze::InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const
{
        int diff = 0;
        if(A.m_Direction != B.m_Direction) diff++;
        if((A.m_Jump != 0) != (B.m_Jump != 0)) diff++;
        if((A.m_Hook != 0) != (B.m_Hook != 0)) diff++;
        return diff;
}

bool CBasicAvoidFreeze::SimulateFreeze(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const
{
        CGameClient *pGame = GameClient();

        CGameWorld SimWorld;
        SimWorld.CopyWorld(&pGame->m_PredictedWorld);

        // Remove other characters (we only care about local player's freeze).
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                if(CCharacter *pChar = SimWorld.GetCharacterById(i))
                        delete pChar;
        }

        CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
        if(!pChar)
                return false;

        int StartTick = SimWorld.GameTick();
        for(int t = 0; t < Ticks; t++)
        {
                pChar->OnDirectInput(&Input);
                SimWorld.m_GameTick = StartTick + t + 1;
                pChar->OnPredictedInput(&Input);
                SimWorld.Tick();

                if(!SimWorld.GetCharacterById(LocalId))
                        return false; // character died/removed — not freeze

                if(pChar->m_FreezeTime > 0)
                        return true; // freeze detected
        }
        return false;
}

void CBasicAvoidFreeze::OnUpdate()
{
        // v1.56.123: actual override moved to ApplyOverride() which is called
        // from CControls::SnapInput at the very end, after all other input
        // manipulators. OnUpdate is now a no-op.
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

        // Current input (what the player is pressing right now).
        CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
        CNetObj_PlayerInput Current = *pInput;

        int triggerTicks = g_Config.m_KxBafTriggerTicks;
        if(triggerTicks < 1) triggerTicks = 1;
        if(triggerTicks > 20) triggerTicks = 20;

        // Step 1: check if freeze will happen with current input.
        if(!SimulateFreeze(LocalId, Current, triggerTicks))
                return; // no freeze ahead — nothing to avoid

        // Step 2: brute-force combinations.
        int simTicks = g_Config.m_KxBafTicks;
        if(simTicks < 1) simTicks = 1;
        if(simTicks > 20) simTicks = 20;

        bool allowDir = g_Config.m_KxBafDirection != 0;
        bool allowJump = g_Config.m_KxBafJump != 0;
        bool allowHook = g_Config.m_KxBafHook != 0;

        // Direction values to try. If allowDir, try -1, 0, +1; else just current.
        int dirs[3] = {0, 0, 0};
        int dirCount = 1;
        if(allowDir)
        {
                dirs[0] = -1;
                dirs[1] = 0;
                dirs[2] = 1;
                dirCount = 3;
        }
        else
        {
                dirs[0] = Current.m_Direction;
        }

        // Jump values to try.
        int jumps[2] = {0, 0};
        int jumpCount = 1;
        if(allowJump)
        {
                jumps[0] = 0;
                jumps[1] = 1;
                jumpCount = 2;
        }
        else
        {
                jumps[0] = Current.m_Jump != 0 ? 1 : 0;
        }

        // Hook values to try.
        int hooks[2] = {0, 0};
        int hookCount = 1;
        if(allowHook)
        {
                hooks[0] = 0;
                hooks[1] = 1;
                hookCount = 2;
        }
        else
        {
                hooks[0] = Current.m_Hook != 0 ? 1 : 0;
        }

        // Iterate all combinations, find ones that avoid freeze.
        bool found = false;
        CNetObj_PlayerInput bestInput = Current;
        int bestScore = -1;

        for(int di = 0; di < dirCount; di++)
        {
                for(int ji = 0; ji < jumpCount; ji++)
                {
                        for(int hi = 0; hi < hookCount; hi++)
                        {
                                CNetObj_PlayerInput test = Current;
                                test.m_Direction = dirs[di];
                                test.m_Jump = jumps[ji];
                                test.m_Hook = hooks[hi];

                                // Skip the exact current input (already checked above).
                                if(test.m_Direction == Current.m_Direction &&
                                   (test.m_Jump != 0) == (Current.m_Jump != 0) &&
                                   (test.m_Hook != 0) == (Current.m_Hook != 0))
                                        continue;

                                // Simulate. If freeze still happens, skip this combination.
                                if(SimulateFreeze(LocalId, test, simTicks))
                                        continue;

                                // This combination avoids freeze. Score = diff from current.
                                int score = InputDiff(Current, test);
                                if(bestScore < 0 || score < bestScore)
                                {
                                        bestScore = score;
                                        bestInput = test;
                                        found = true;
                                        // First-found wins on ties (we only update on strictly better).
                                }
                        }
                }
        }

        // Step 3: override input with best combination.
        if(found)
        {
                pInput->m_Direction = bestInput.m_Direction;
                pInput->m_Jump = bestInput.m_Jump;
                pInput->m_Hook = bestInput.m_Hook;
                // Keep aim (m_TargetX/Y) and fire as-is.
        }
}
