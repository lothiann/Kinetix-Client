#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// v1.56.258: ConAttackHookDelay removed — hook pulse is fixed at 1s now.

void CBotNet::ConAttackDists(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        (void)p;
        g_Config.m_KxFireDist = (int)pResult->GetFloat(0);
        g_Config.m_KxHookDist = (int)pResult->GetFloat(1);
        g_Config.m_KxRescueRadius = (int)pResult->GetFloat(2);
        if(pResult->NumArguments() > 3)
                g_Config.m_KxTargetDist = (int)pResult->GetFloat(3);
        if(pResult->NumArguments() > 4)
                g_Config.m_KxMainDist = (int)pResult->GetFloat(4);
        if(pResult->NumArguments() > 5)
                g_Config.m_KxStandDist = (int)pResult->GetFloat(5);
        if(pResult->NumArguments() > 6)
                g_Config.m_KxMainStandDist = (int)pResult->GetFloat(6);
}

void CBotNet::ConSetTargets(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        for(int i = 0; i < 128; i++)
                p->m_TargetList[i] = false;
        const char *pL = pResult->GetString(0);
        if(!pL || !pL[0])
                return;
        char aB[256];
        str_copy(aB, pL, sizeof(aB));
        char *pC = aB;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        p->m_TargetList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}

void CBotNet::ConSetBots(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        for(int i = 0; i < 128; i++)
                p->m_BotsList[i] = false;
        const char *pL = pResult->GetString(0);
        if(!pL || !pL[0])
                return;
        char aB[256];
        str_copy(aB, pL, sizeof(aB));
        char *pC = aB;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        p->m_BotsList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}

void CBotNet::ConRescueIds(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        for(int i = 0; i < 128; i++)
                p->m_RescueList[i] = false;
        const char *pL = pResult->GetString(0);
        if(!pL || !pL[0])
                return;
        char aB[256];
        str_copy(aB, pL, sizeof(aB));
        char *pC = aB;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        p->m_RescueList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}

void CBotNet::ConSetTargetAll(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxTargetAll = pResult->GetInteger(0) != 0;
}

// v1.56.258: ConAttackSettings (kx_atk_set, 16 params) removed — legacy cvars gone.

void CBotNet::ConAttackEnable(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxHelpers = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxHelpers)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSetMain(IConsole::IResult *pResult, void *pUserData)
{
        g_Config.m_KxMain = pResult->GetInteger(0);
}

// v1.56.260: ConRandomAim removed (old test feature).

void CBotNet::ConCopyMoves(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        int ID = pResult->GetInteger(0);
        g_Config.m_KxCopyTargetId = ID;
        g_Config.m_KxCopyMoves = (ID >= 0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                pSelf->m_aDummies[D].m_LastTargetAttackTick = -1;
}

void CBotNet::ConClientDelay(IConsole::IResult *pResult, void *pUserData)
{
        g_Config.m_KxClientDelay = pResult->GetInteger(0);
}

// v1.56.258: ConStandOnX/ConPathfinder*/ConPathfinderRays/RaysDist/Snap/Sps removed.

void CBotNet::ConPathfinderGo(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);

        if(on == 0)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_PathfinderGoActive = false;
                        p->m_aDummies[D].m_LastTargetTX = -1;
                        p->m_aDummies[D].m_LastTargetTY = -1;
                        p->m_aDummies[D].m_PathFound = false;
                }
                dbg_msg("botnet", "Pathfinder go disabled");
                return;
        }

        if(pResult->NumArguments() >= 3)
        {
                int x = pResult->GetInteger(1);
                int y = pResult->GetInteger(2);
                vec2 pos = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_PathfinderGoActive = true;
                        p->m_aDummies[D].m_PathfinderGoPos = pos;
                }
                dbg_msg("botnet", "Pathfinder go to tile (%d, %d) -> pos (%.0f, %.0f)", x, y, pos.x, pos.y);
        }
}

// =========================================================
// INDIVIDUAL TOGGLE COMMANDS
// =========================================================

// v1.56.258: ConStand/ConAutoAim/ConAutoFire/ConAutoHook/ConMove removed.

void CBotNet::ConRescue(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxRescue = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxRescue)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

// v1.56.258: ConKillFrz/ConAtkMain removed.

void CBotNet::ConAutoMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAutoMain = pResult->GetInteger(0) != 0;
}

// v1.56.258: ConHammer/ConSmartDetect/ConSmartRescue/ConAvoidFreeze/ConPfHook/
// ConKinodynamic/ConKino* (Candidates/Ticks/HookAngles/CacheTicks/ShowPath/
// ShowField/Aggressive) removed.

// =========================================================
// PATHFINDER TAB (Kinetix→Pathfinder) — chunk-based mini TAS maker
// =========================================================
//
// State machine:  IDLE --[user clicks "pathfinding"]--> RUNNING
//                      --[chunk reaches finish / died / max-chunks]--> FINISHED
//                      --[user clicks "finish"]--> IDLE (path disappears)
//                 RUNNING --[user clicks "stop"]--> IDLE (cancel)
//
// Per OnUpdate (RUNNING): one chunk = generate candidates (v1.42 motion
// primitives: 6 BASE + HOOK_FIRE × hookAngles + 3 HOOK_HOLD + HOOK_RELEASE),
// simulate each in a cloned world, score by enabled Score+Fine methods,
// pick the best, append trajectory to m_PfVPath, advance sim state.
//
// RenderPathfinderPath draws m_PfVPath using the shared m_KxLineRenderingColor/
// Alpha/LineSize settings (same as Advanced→Trajectory).
//
// SIMULATION-ONLY: does NOT write inputs to any dummy.

// --- console command handlers ---
