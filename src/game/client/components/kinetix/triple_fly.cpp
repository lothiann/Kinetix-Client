#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <base/vmath.h>
#include <base/math.h>
#include <stdlib.h>

static void TripleFlyReleaseHook(CGameClient *pGame, int Dummy)
{
        if(!pGame || Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;
        CNetObj_PlayerInput *pDummy = &pGame->m_aDummyInput[Dummy];
        if(pDummy->m_Hook)
        {
                pDummy->m_Hook = 0;
                pGame->m_Controls.m_aInputData[Dummy] = *pDummy;
        }
}

constexpr int MAX_PREDICT_TICKS = 20;

static vec2 TripleFlyPredictHookPos(CGameClient *pGame, int TargetCid, vec2 MyPos)
{
        const CTuningParams *pT = pGame->m_PredictedWorld.GlobalTuning();
        if(!pT || pT->m_HookFireSpeed <= 1.0f)
                return pGame->m_aClients[TargetCid].m_Predicted.m_Pos;

        CGameWorld *pPredWorld = new CGameWorld;
        pPredWorld->CopyWorld(&pGame->m_PredictedWorld);

        vec2 aPositions[MAX_CLIENTS][MAX_PREDICT_TICKS + 1];
        bool aValid[MAX_CLIENTS] = {};
        for(int c = 0; c < MAX_CLIENTS; c++)
        {
                if(CCharacter *pChar = pPredWorld->GetCharacterById(c))
                {
                        aPositions[c][0] = pChar->Core()->m_Pos;
                        aValid[c] = true;
                }
        }

        const int startTick = pPredWorld->GameTick();
        for(int t = 1; t <= MAX_PREDICT_TICKS; t++)
        {
                for(int c = 0; c < MAX_CLIENTS; c++)
                {
                        CCharacter *pChar = pPredWorld->GetCharacterById(c);
                        if(!pChar)
                                continue;
                        CNetObj_PlayerInput inp = pChar->Core()->m_Input;
                        pChar->OnDirectInput(&inp);
                }
                pPredWorld->m_GameTick = startTick + t;
                for(int c = 0; c < MAX_CLIENTS; c++)
                {
                        CCharacter *pChar = pPredWorld->GetCharacterById(c);
                        if(!pChar)
                                continue;
                        CNetObj_PlayerInput inp = pChar->Core()->m_Input;
                        pChar->OnPredictedInput(&inp);
                }
                pPredWorld->Tick();
                for(int c = 0; c < MAX_CLIENTS; c++)
                {
                        if(CCharacter *pChar = pPredWorld->GetCharacterById(c))
                                aPositions[c][t] = pChar->Core()->m_Pos;
                        else
                                aValid[c] = false;
                }
        }

        vec2 predictedPos = aPositions[TargetCid][0];
        if(aValid[TargetCid])
        {
                bool foundPrediction = false;
                float prevFlightTime = -1.0f;
                for(int t = 0; t <= MAX_PREDICT_TICKS; t++)
                {
                        float dist = length(aPositions[TargetCid][t] - MyPos);
                        if(dist < 1.0f)
                        {
                                predictedPos = aPositions[TargetCid][t];
                                foundPrediction = true;
                                break;
                        }
                        const float flightTime = dist / pT->m_HookFireSpeed;
                        if(flightTime <= (float)t)
                        {
                                if(t == 0 || prevFlightTime < 0.0f)
                                {
                                        predictedPos = aPositions[TargetCid][t];
                                }
                                else
                                {
                                        const float prevTick = (float)(t - 1);
                                        const float numerator = prevFlightTime - prevTick;
                                        const float denominator = 1.0f - (flightTime - prevFlightTime);
                                        float u;
                                        if(denominator > 0.001f)
                                                u = numerator / denominator;
                                        else
                                                u = 0.5f;
                                        if(u < 0.0f)
                                                u = 0.0f;
                                        if(u > 1.0f)
                                                u = 1.0f;
                                        predictedPos = mix(aPositions[TargetCid][t - 1], aPositions[TargetCid][t], u);
                                }
                                foundPrediction = true;
                                break;
                        }
                        prevFlightTime = flightTime;
                }
                if(!foundPrediction)
                        predictedPos = aPositions[TargetCid][MAX_PREDICT_TICKS];
        }

        delete pPredWorld;
        return predictedPos;
}

static bool TripleFlyHookPathClear(CGameClient *pGame, vec2 From, vec2 To, int SelfCid, int TargetCid)
{
        CGameWorld *pWorld = &pGame->m_PredictedWorld;
        vec2 NewPos = vec2(0, 0);
        CCharacter *pHit = pWorld->IntersectCharacter(From, To, CCharacterCore::PhysicalSize() * 0.5f, NewPos,
                pWorld->GetCharacterById(SelfCid));
        if(!pHit)
                return true;
        return pHit->GetCid() == TargetCid;
}

void CBotNet::UpdateTripleFly()
{
        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        const bool enabled = g_Config.m_KxTripleFly != 0;
        if(!enabled)
        {
                if(m_TripleFlyWasActive)
                {
                        TripleFlyReleaseHook(pGame, m_TripleFlyDummy);
                        m_TripleFlyWasActive = false;
                        m_TripleFlyDummy = -1;
                        m_TripleFlyHooking = false;
                }
                return;
        }

        const bool copyMovesLeavesHookAim = g_Config.m_ClDummyCopyMoves != 0 &&
                                            g_Config.m_KxDummyCopyMovesFilter != 0;
        const bool hammerLeavesHook = g_Config.m_ClDummyHammer == 0 ||
                                      g_Config.m_KxDummyHammerAuto != 0;
        if(!hammerLeavesHook || g_Config.m_KxFlyRide != 0 ||
           g_Config.m_KxCopyMoves != 0 || g_Config.m_KxAttack != 0 ||
           (!copyMovesLeavesHookAim && g_Config.m_ClDummyCopyMoves != 0) ||
           g_Config.m_ClDummyControl != 0)
        {
                if(m_TripleFlyHooking)
                {
                        TripleFlyReleaseHook(pGame, m_TripleFlyDummy);
                        m_TripleFlyHooking = false;
                }
                return;
        }

        if(!pGame->m_Snap.m_pLocalInfo)
                return;

        const int activeD = g_Config.m_ClDummy;
        int actingD = -1;
        if(g_Config.m_KxTripleFlyDummyMode == 0)
        {
                const int activeCid = pGame->m_aLocalIds[activeD];
                if(activeCid < 0 || activeCid >= 128 || !pGame->m_aClients[activeCid].m_Active)
                {
                        if(m_TripleFlyHooking)
                        {
                                TripleFlyReleaseHook(pGame, m_TripleFlyDummy);
                                m_TripleFlyHooking = false;
                        }
                        return;
                }
                const vec2 activePos = pGame->m_aClients[activeCid].m_Predicted.m_Pos;
                float bestSq = 1e18f;
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(D == activeD)
                                continue;
                        if(D != 0 && !Client()->DummyConnected(D))
                                continue;
                        const int cid = pGame->m_aLocalIds[D];
                        if(cid < 0 || cid >= 128 || !pGame->m_aClients[cid].m_Active)
                                continue;
                        const float dsq = length_squared(pGame->m_aClients[cid].m_Predicted.m_Pos - activePos);
                        if(dsq < bestSq)
                        {
                                bestSq = dsq;
                                actingD = D;
                        }
                }
        }
        else
        {
                const int wantD = g_Config.m_KxTripleFlyDummyId;
                if(wantD >= 0 && wantD < MAX_DUMMIES && wantD != activeD &&
                   (wantD == 0 || Client()->DummyConnected(wantD)))
                        actingD = wantD;
        }

        if(actingD < 0 || m_aDummies[actingD].m_MacroPlaying || m_aDummies[actingD].m_PathfinderGoActive)
        {
                if(m_TripleFlyHooking)
                {
                        TripleFlyReleaseHook(pGame, m_TripleFlyDummy);
                        m_TripleFlyHooking = false;
                }
                return;
        }

        if(m_TripleFlyHooking && m_TripleFlyDummy != actingD)
        {
                TripleFlyReleaseHook(pGame, m_TripleFlyDummy);
                m_TripleFlyHooking = false;
        }

        const int actingCid = pGame->m_aLocalIds[actingD];
        if(actingCid < 0 || actingCid >= 128 || !pGame->m_aClients[actingCid].m_Active)
        {
                if(m_TripleFlyHooking)
                {
                        TripleFlyReleaseHook(pGame, actingD);
                        m_TripleFlyHooking = false;
                }
                return;
        }
        const vec2 actingPos = pGame->m_aClients[actingCid].m_Predicted.m_Pos;

        if(g_Config.m_KxDummyHammerAuto != 0)
        {
                const int autoId = pGame->m_aLocalIds[activeD];
                bool canHammerNow = false;
                if(autoId >= 0 && autoId < 128 && pGame->m_aClients[autoId].m_Active)
                {
                        vec2 activeTestPos = pGame->m_aClients[autoId].m_Predicted.m_Pos;
                        vec2 dummyTestPos = actingPos;
                        if(g_Config.m_KxDummyHammerAutoPredict)
                        {
                                activeTestPos += pGame->m_aClients[autoId].m_Predicted.m_Vel;
                                dummyTestPos += pGame->m_aClients[actingCid].m_Predicted.m_Vel;
                        }
                        const vec2 delta = activeTestPos - dummyTestPos;
                        const float maxDist = (float)g_Config.m_KxDummyHammerAutoMaxDist;
                        if(length_squared(delta) <= maxDist * maxDist)
                                canHammerNow = true;
                }
                if(canHammerNow)
                {
                        const int FireDelayTicks = pGame->m_aTuning[actingD].GetWeaponFireDelay(WEAPON_HAMMER) * Client()->GameTickSpeed();
                        if(Client()->GameTick(g_Config.m_ClDummy) - pGame->m_aDummyLastFireTick[actingD] < FireDelayTicks)
                                canHammerNow = false;
                }
                if(canHammerNow)
                {
                        if(m_TripleFlyHooking)
                        {
                                TripleFlyReleaseHook(pGame, actingD);
                                m_TripleFlyHooking = false;
                        }
                        return;
                }
        }

        int targetCid = -1;
        const int targetMode = g_Config.m_KxTripleFlyTargetMode;
        if(targetMode == 0 || targetMode == 1)
        {
                vec2 refPos = actingPos;
                if(targetMode == 1)
                {
                        const int activeCid = pGame->m_aLocalIds[activeD];
                        if(activeCid < 0 || activeCid >= 128 || !pGame->m_aClients[activeCid].m_Active)
                        {
                                if(m_TripleFlyHooking)
                                {
                                        TripleFlyReleaseHook(pGame, actingD);
                                        m_TripleFlyHooking = false;
                                }
                                return;
                        }
                        refPos = pGame->m_aClients[activeCid].m_Predicted.m_Pos + pGame->m_Controls.m_aMousePos[activeD];
                }
                float bestSq = 1e18f;
                for(int cid = 0; cid < 128; cid++)
                {
                        if(!pGame->m_aClients[cid].m_Active)
                                continue;
                        if(cid == actingCid || cid == pGame->m_aLocalIds[activeD])
                                continue;
                        const float dsq = length_squared(pGame->m_aClients[cid].m_Predicted.m_Pos - refPos);
                        if(dsq < bestSq)
                        {
                                bestSq = dsq;
                                targetCid = cid;
                        }
                }
        }
        else
        {
                const char *pIds = g_Config.m_KxTripleFlyTargetIds;
                while(*pIds)
                {
                        while(*pIds && (*pIds == ' ' || *pIds == ',' || *pIds == ';'))
                                pIds++;
                        if(!*pIds)
                                break;
                        if(*pIds < '0' || *pIds > '9')
                        {
                                while(*pIds && *pIds != ' ' && *pIds != ',' && *pIds != ';')
                                        pIds++;
                                continue;
                        }
                        const int id = atoi(pIds);
                        while(*pIds && *pIds != ' ' && *pIds != ',' && *pIds != ';')
                                pIds++;
                        if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
                        {
                                targetCid = id;
                                break;
                        }
                }
        }

        if(targetCid < 0)
        {
                if(m_TripleFlyHooking)
                {
                        TripleFlyReleaseHook(pGame, actingD);
                        m_TripleFlyHooking = false;
                }
                return;
        }

        const vec2 targetPos = pGame->m_aClients[targetCid].m_Predicted.m_Pos;
        const float dist = length(targetPos - actingPos);

        if(!m_MapGridLoaded)
                LoadMapGrid();

        if(dist > (float)g_Config.m_KxTripleFlyTriggerRadius ||
           (m_MapGridLoaded &&
            !HasLineOfSightTiles((int)(actingPos.y / 32.0f), (int)(actingPos.x / 32.0f),
                                 (int)(targetPos.y / 32.0f), (int)(targetPos.x / 32.0f))) ||
           !TripleFlyHookPathClear(pGame, actingPos, targetPos, actingCid, targetCid))
        {
                if(m_TripleFlyHooking)
                {
                        TripleFlyReleaseHook(pGame, actingD);
                        m_TripleFlyHooking = false;
                }
                return;
        }

        CNetObj_PlayerInput *pDummy = &pGame->m_aDummyInput[actingD];
        if(dist > (float)g_Config.m_KxTripleFlyRadius)
        {
                const vec2 aim = TripleFlyPredictHookPos(pGame, targetCid, actingPos) - actingPos;
                pDummy->m_TargetX = (int)aim.x;
                pDummy->m_TargetY = (int)aim.y;
                pDummy->m_Hook = 1;
                m_TripleFlyHooking = true;
        }
        else
        {
                pDummy->m_Hook = 0;
                m_TripleFlyHooking = false;
        }
        pGame->m_Controls.m_aInputData[actingD] = *pDummy;
        m_TripleFlyWasActive = true;
        m_TripleFlyDummy = actingD;
}
