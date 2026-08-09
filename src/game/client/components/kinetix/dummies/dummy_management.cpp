#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

bool CBotNet::IsDummyActive(int Dummy) const
{
        if(g_Config.m_KxAttack || g_Config.m_KxCopyMoves || g_Config.m_KxRandomAim || g_Config.m_KxAtkPathfinder)
                return true;
        if(Dummy >= 0 && Dummy < MAX_DUMMIES)
        {
                const CBotNetDummy &S = m_aDummies[Dummy];
                if(S.m_MacroPlaying || S.m_PathfinderGoActive)
                        return true;
        }
        return false;
}

// =========================================================
// RESET DUMMY INPUTS
// =========================================================

void CBotNet::ResetDummyInputs(int Dummy)
{
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;
        CGameClient *pGame = GameClient();
        CNetObj_PlayerInput *pInput = &pGame->m_aDummyInput[Dummy];
        ResetAndCommitInput(pGame, pInput, Dummy);
}
