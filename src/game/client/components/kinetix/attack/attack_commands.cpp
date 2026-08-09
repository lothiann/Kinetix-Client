#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

void CBotNet::ConAttackHookDelay(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkHookDelay = pResult->GetInteger(0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_HookTickTimer = 0;
}

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

void CBotNet::ConAttackSettings(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAutoAim = pResult->GetInteger(0) != 0;
        g_Config.m_KxAutoFire = pResult->GetInteger(1) != 0;
        g_Config.m_KxAutoHook = pResult->GetInteger(2) != 0;
        g_Config.m_KxMove = pResult->GetInteger(3) != 0;
        g_Config.m_KxStand = pResult->GetInteger(4) != 0;
        g_Config.m_KxRescue = pResult->GetInteger(5) != 0;
        g_Config.m_KxRescueAll = pResult->GetInteger(6) != 0;
        g_Config.m_KxSmartDetect = pResult->GetInteger(7) != 0;
        g_Config.m_KxSmartRescue = pResult->GetInteger(8) != 0;
        g_Config.m_KxKillFrz = pResult->GetInteger(9) != 0;
        g_Config.m_KxAtkMain = pResult->GetInteger(10) != 0;
        g_Config.m_KxHammer = pResult->GetInteger(11) != 0;
        {
                bool newSim = pResult->GetInteger(12) != 0;
                if(newSim != g_Config.m_KxPfSimulatePlayers)
                {
                        g_Config.m_KxPfSimulatePlayers = newSim;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                p->m_aDummies[D].m_LastTargetTX = -1;
                                p->m_aDummies[D].m_LastTargetTY = -1;
                                p->m_aDummies[D].m_PathFound = false;
                        }
                }
        }
        g_Config.m_KxAvoidFreeze = pResult->GetInteger(13) != 0;
        {
                bool newPfHook = pResult->GetInteger(14) != 0;
                if(newPfHook != g_Config.m_KxPfHook)
                {
                        g_Config.m_KxPfHook = newPfHook;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                                p->m_aDummies[D].m_PfHookTile = vec2(0, 0);
                }
        }
        {
                bool newKino = pResult->GetInteger(15) != 0;
                if(newKino != g_Config.m_KxKinodynamic)
                {
                        g_Config.m_KxKinodynamic = newKino;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                p->m_aDummies[D].m_KinoCache.Reset();
                                p->ResetDummyInputs(D);
                        }
                }
        }
}

void CBotNet::ConAttackEnable(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAttack = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxAttack)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSetMain(IConsole::IResult *pResult, void *pUserData)
{
        g_Config.m_KxMain = pResult->GetInteger(0);
}

void CBotNet::ConRandomAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        g_Config.m_KxRandomAim = pResult->GetInteger(0) != 0;
        if(pResult->NumArguments() > 1)
                g_Config.m_KxRandomAimInterval = pResult->GetInteger(1);
}

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

void CBotNet::ConStandOnX(IConsole::IResult *pResult, void *pUserData)
{
        g_Config.m_KxStandOnX = pResult->GetInteger(0) != 0;
}

void CBotNet::ConPathfinder(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkPathfinder = pResult->GetInteger(0) != 0;
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                p->m_aDummies[D].m_LastTargetTX = -1;
                p->m_aDummies[D].m_LastTargetTY = -1;
                p->m_aDummies[D].m_PathFound = false;
        }
}

void CBotNet::ConPathfinderRays(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkPathfinderRays = pf_clamp(pResult->GetInteger(0), 12, 90);
}

void CBotNet::ConPathfinderRaysDist(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkPathfinderRaysDist = pf_clamp(pResult->GetInteger(0), 1, 128);
}

void CBotNet::ConPathfinderSnap(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkPathfinderSnap = pResult->GetInteger(0) != 0;
}

void CBotNet::ConPathfinderSps(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkPathfinderSps = pResult->GetInteger(0);
}

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

void CBotNet::ConStand(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxStand = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxStand)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAutoAim = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxAutoAim)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoFire(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAutoFire = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxAutoFire)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoHook(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAutoHook = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxAutoHook)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConMove(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxMove = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxMove)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

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

void CBotNet::ConKillFrz(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxKillFrz = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxKillFrz)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAtkMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAtkMain = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxAtkMain)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAutoMain = pResult->GetInteger(0) != 0;
}

void CBotNet::ConHammer(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxHammer = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxHammer)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSmartDetect(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxSmartDetect = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxSmartDetect)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSmartRescue(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxSmartRescue = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxSmartRescue)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAvoidFreeze(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxAvoidFreeze = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxAvoidFreeze)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConPfHook(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxPfHook = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxPfHook)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConKinodynamic(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxKinodynamic = pResult->GetInteger(0) != 0;
        if(!g_Config.m_KxKinodynamic)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_KinoCache.Reset();
                        p->ResetDummyInputs(D);
                }
        }
}

void CBotNet::ConKinoCandidates(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 100 && v <= 2000)
                g_Config.m_KxKinoCandidates = v;
}

void CBotNet::ConKinoTicks(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 3 && v <= 20)
                g_Config.m_KxKinoTicks = v;
}

void CBotNet::ConKinoHookAngles(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 4 && v <= 32)
                g_Config.m_KxKinoHookAngles = v;
}

void CBotNet::ConKinoCacheTicks(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 1 && v <= 128)
                g_Config.m_KxKinoCacheTicks = v;
}

void CBotNet::ConKinoShowPath(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxKinoShowPath = pResult->GetInteger(0) != 0;
}

void CBotNet::ConKinoShowField(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxKinoShowField = pResult->GetInteger(0) != 0;
}

void CBotNet::ConKinoAggressive(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        g_Config.m_KxKinoAggressive = pResult->GetInteger(0) != 0;
}

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
