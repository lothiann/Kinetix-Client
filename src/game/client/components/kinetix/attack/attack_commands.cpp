#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

void CBotNet::ConAttackHookDelay(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_HookDelay = pResult->GetInteger(0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_HookTickTimer = 0;
}

void CBotNet::ConAttackDists(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_FireDist = pResult->GetFloat(0);
        p->m_HookDist = pResult->GetFloat(1);
        p->m_RescueRadius = pResult->GetFloat(2);
        if(pResult->NumArguments() > 3)
                p->m_TargetDist = pResult->GetFloat(3);
        if(pResult->NumArguments() > 4)
                p->m_MainDist = pResult->GetFloat(4);
        if(pResult->NumArguments() > 5)
                p->m_StandDist = pResult->GetFloat(5);
        if(pResult->NumArguments() > 6)
                p->m_MainStandDist = pResult->GetFloat(6);
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
        p->m_AllTarget = pResult->GetInteger(0) != 0;
}

void CBotNet::ConAttackSettings(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoAim = pResult->GetInteger(0) != 0;
        p->m_AutoFire = pResult->GetInteger(1) != 0;
        p->m_AutoHook = pResult->GetInteger(2) != 0;
        p->m_MoveEnabled = pResult->GetInteger(3) != 0;
        p->m_StandEnabled = pResult->GetInteger(4) != 0;
        p->m_RescueFrozen = pResult->GetInteger(5) != 0;
        p->m_RescueAll = pResult->GetInteger(6) != 0;
        p->m_SmartDetect = pResult->GetInteger(7) != 0;
        p->m_SmartRescue = pResult->GetInteger(8) != 0;
        p->m_KillOnFreeze = pResult->GetInteger(9) != 0;
        p->m_AttackMain = pResult->GetInteger(10) != 0;
        p->m_AutoHammer = pResult->GetInteger(11) != 0;
        {
                bool newSim = pResult->GetInteger(12) != 0;
                if(newSim != p->m_PfSimulatePlayers)
                {
                        p->m_PfSimulatePlayers = newSim;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                p->m_aDummies[D].m_LastTargetTX = -1;
                                p->m_aDummies[D].m_LastTargetTY = -1;
                                p->m_aDummies[D].m_PathFound = false;
                        }
                }
        }
        p->m_AvoidFreeze = pResult->GetInteger(13) != 0;
        {
                bool newPfHook = pResult->GetInteger(14) != 0;
                if(newPfHook != p->m_PfHookEnabled)
                {
                        p->m_PfHookEnabled = newPfHook;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                                p->m_aDummies[D].m_PfHookTile = vec2(0, 0);
                }
        }
        {
                bool newKino = pResult->GetInteger(15) != 0;
                if(newKino != p->m_KinodynamicEnabled)
                {
                        p->m_KinodynamicEnabled = newKino;
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
        p->m_AttackEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_AttackEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSetMain(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_MainID = pResult->GetInteger(0);
}

void CBotNet::ConRandomAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        pSelf->m_RandomAim = pResult->GetInteger(0) != 0;
        if(pResult->NumArguments() > 1)
                pSelf->m_RandomAimInterval = pResult->GetInteger(1);
}

void CBotNet::ConCopyMoves(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        int ID = pResult->GetInteger(0);
        pSelf->m_CopyTargetID = ID;
        pSelf->m_CopyMoves = (ID >= 0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                pSelf->m_aDummies[D].m_LastTargetAttackTick = -1;
}

void CBotNet::ConClientDelay(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_ClientDelay = pResult->GetInteger(0);
}

void CBotNet::ConStandOnX(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_StandOnXOnly = pResult->GetInteger(0) != 0;
}

void CBotNet::ConPathfinder(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PathfinderEnabled = pResult->GetInteger(0) != 0;
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
        p->m_PfRays = pf_clamp(pResult->GetInteger(0), 12, 90);
}

void CBotNet::ConPathfinderRaysDist(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfViewRadius = pf_clamp(pResult->GetInteger(0), 1, 128);
}

void CBotNet::ConPathfinderSnap(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfSnap = pResult->GetInteger(0) != 0;
}

void CBotNet::ConPathfinderSps(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfSimulateMethod = pResult->GetInteger(0);
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
        p->m_StandEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_StandEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoAim = pResult->GetInteger(0) != 0;
        if(!p->m_AutoAim)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoFire(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoFire = pResult->GetInteger(0) != 0;
        if(!p->m_AutoFire)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoHook(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoHook = pResult->GetInteger(0) != 0;
        if(!p->m_AutoHook)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConMove(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_MoveEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_MoveEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConRescue(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_RescueFrozen = pResult->GetInteger(0) != 0;
        if(!p->m_RescueFrozen)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConKillFrz(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KillOnFreeze = pResult->GetInteger(0) != 0;
        if(!p->m_KillOnFreeze)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAtkMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AttackMain = pResult->GetInteger(0) != 0;
        if(!p->m_AttackMain)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoMain = pResult->GetInteger(0) != 0;
}

void CBotNet::ConHammer(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoHammer = pResult->GetInteger(0) != 0;
        if(!p->m_AutoHammer)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSmartDetect(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_SmartDetect = pResult->GetInteger(0) != 0;
        if(!p->m_SmartDetect)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSmartRescue(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_SmartRescue = pResult->GetInteger(0) != 0;
        if(!p->m_SmartRescue)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAvoidFreeze(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AvoidFreeze = pResult->GetInteger(0) != 0;
        if(!p->m_AvoidFreeze)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConPfHook(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfHookEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_PfHookEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConKinodynamic(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinodynamicEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_KinodynamicEnabled)
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
                p->m_KinoCandidates = v;
}

void CBotNet::ConKinoTicks(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 3 && v <= 20)
                p->m_KinoTicks = v;
}

void CBotNet::ConKinoHookAngles(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 4 && v <= 32)
                p->m_KinoHookAngles = v;
}

void CBotNet::ConKinoCacheTicks(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 1 && v <= 128)
                p->m_KinoCacheTicks = v;
}

void CBotNet::ConKinoShowPath(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinoShowPath = pResult->GetInteger(0) != 0;
}

void CBotNet::ConKinoShowField(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinoShowField = pResult->GetInteger(0) != 0;
}

void CBotNet::ConKinoAggressive(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinoAggressive = pResult->GetInteger(0) != 0;
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
// RenderPathfinderPath draws m_PfVPath using the shared m_KxTrajectoryColor/
// Alpha/LineSize settings (same as Advanced→Trajectory).
//
// SIMULATION-ONLY: does NOT write inputs to any dummy.

// --- console command handlers ---
