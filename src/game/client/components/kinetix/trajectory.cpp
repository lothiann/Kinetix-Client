// (c) Kinetix. Trajectory prediction component — v1.56.178.
//
// See trajectory.h for the full architecture overview. This file implements
// the 5 per-type renderers + the "Show for current" entity iteration.
//
// The renderers all share the KX_LINE_TRAJECTORY line rendering settings
// (color / size / alpha) — per-type colors are out of scope (YAGNI).

#include <game/client/components/kinetix/trajectory.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <game/client/prediction/entities/projectile.h>
#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Tuning access for grenade/laser prediction. TuningList() macro lives in
// gamecore.h (included via kinetix_internal.h) and resolves to the active
// tune zone's tuning struct.
#include <game/gamecore.h>

void CTrajectory::SyncLegacyConfig()
{
        // v1.56.178: two-way sync between the legacy config vars and m_aTypes[TRAJ_TEE].
        // - If the user runs "toggle kx_show_trajectory 1 0" the config var changes,
        //   and we reflect that into m_aTypes[TRAJ_TEE].m_Show so the new ClickGUI
        //   toggle stays in sync.
        // - If the user toggles via the new ClickGUI toggle, m_aTypes[TRAJ_TEE].m_Show
        //   changes and we write back to the config var so existing bind commands
        //   still see the right state.
        // Ticks are one-way (config -> settings) because the new slider writes
        // directly to m_aTypes[TRAJ_TEE].m_PredictionTicks and we don't want
        // legacy binds to fight with it; but we do write back so they match.
        static bool s_LastShow = false;
        static int s_LastTicks = -1;
        static bool s_First = true;

        STypeSettings &tee = m_aTypes[TRAJ_TEE];

        if(s_First)
        {
                // Seed Tee settings from legacy config on first frame.
                tee.m_Show = g_Config.m_KxShowTrajectory != 0;
                tee.m_PredictionTicks = g_Config.m_KxTrajectoryTicks;
                s_First = false;
        }
        else
        {
                // Detect external config change (bind command).
                bool cfgShow = g_Config.m_KxShowTrajectory != 0;
                if(cfgShow != s_LastShow)
                        tee.m_Show = cfgShow;
                if(g_Config.m_KxTrajectoryTicks != s_LastTicks)
                        tee.m_PredictionTicks = g_Config.m_KxTrajectoryTicks;
        }

        // Write back so legacy config always mirrors the Tee settings.
        g_Config.m_KxShowTrajectory = tee.m_Show ? 1 : 0;
        g_Config.m_KxTrajectoryTicks = tee.m_PredictionTicks;

        s_LastShow = tee.m_Show;
        s_LastTicks = tee.m_PredictionTicks;
}

void CTrajectory::DrawPolyline(const std::vector<vec2> &vPoints, bool AlphaGradient)
{
        if(vPoints.size() < 2)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        IGraphics *pGraphics = pGame->Graphics();
        if(!pGraphics)
                return;

        ColorRGBA BaseColor = ColorRGBA(KxLineColor(KX_LINE_TRAJECTORY), true);
        float ConfigAlpha = KxLineAlpha(KX_LINE_TRAJECTORY);
        int LineSize = KxLineSize(KX_LINE_TRAJECTORY);

        pGraphics->TextureClear();

        if(LineSize > 0)
        {
                std::vector<IGraphics::CFreeformItem> vQuads;
                vQuads.reserve(vPoints.size() - 1);
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;

                for(size_t i = 1; i < vPoints.size(); i++)
                {
                        vec2 p0 = vPoints[i - 1];
                        vec2 p1 = vPoints[i];
                        vec2 Dir = normalize(p1 - p0);
                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;

                        vQuads.emplace_back(
                                p0.x - Perp.x, p0.y - Perp.y,
                                p0.x + Perp.x, p0.y + Perp.y,
                                p1.x - Perp.x, p1.y - Perp.y,
                                p1.x + Perp.x, p1.y + Perp.y);
                }

                pGraphics->QuadsBegin();
                for(size_t i = 0; i < vQuads.size(); i++)
                {
                        float Alpha;
                        if(AlphaGradient)
                        {
                                float t = (float)(i + 1) / (float)vQuads.size();
                                Alpha = ConfigAlpha * (1.0f - t * 0.7f);
                        }
                        else
                        {
                                Alpha = ConfigAlpha;
                        }
                        pGraphics->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, Alpha);
                        pGraphics->QuadsDrawFreeform(&vQuads[i], 1);
                }
                pGraphics->QuadsEnd();
        }
        else
        {
                pGraphics->LinesBegin();
                for(size_t i = 1; i < vPoints.size(); i++)
                {
                        float Alpha;
                        if(AlphaGradient)
                        {
                                float t = (float)i / (float)vPoints.size();
                                Alpha = ConfigAlpha * (1.0f - t * 0.7f);
                        }
                        else
                        {
                                Alpha = ConfigAlpha;
                        }
                        IGraphics::CLineItem Line(vPoints[i - 1], vPoints[i]);
                        pGraphics->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, Alpha);
                        pGraphics->LinesDraw(&Line, 1);
                }
                pGraphics->LinesEnd();
        }
}

void CTrajectory::RenderTee()
{
        const STypeSettings &s = m_aTypes[TRAJ_TEE];
        if(!s.m_Show)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        int LocalClientId = pGame->m_Snap.m_LocalClientId;
        if(LocalClientId < 0 || !pGame->m_Snap.m_aCharacters[LocalClientId].m_Active)
                return;

        CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalClientId);
        if(!pLocalChar)
                return;

        int NumTicks = s.m_PredictionTicks;
        if(NumTicks <= 0)
                return;

        // Determine which characters to predict.
        // - Local player: always (if Show is on).
        // - Other players: only if m_ShowForOtherPlayers is on.
        std::vector<int> vPredictIds;
        vPredictIds.push_back(LocalClientId);
        if(s.m_ShowForOtherPlayers)
        {
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(i == LocalClientId)
                                continue;
                        if(pGame->m_Snap.m_aCharacters[i].m_Active)
                                vPredictIds.push_back(i);
                }
        }

        // Collect trajectory points for all predicted characters.
        // We use ONE cloned world for all characters (cheaper than per-character).
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(&pGame->m_PredictedWorld);

        // If Simulate Players is OFF, remove other characters (keep local + entities).
        if(!s.m_SimulatePlayers)
        {
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(i == LocalClientId)
                                continue;
                        // Don't remove chars we want to predict.
                        bool keep = false;
                        for(int id : vPredictIds)
                        {
                                if(id == i)
                                {
                                        keep = true;
                                        break;
                                }
                        }
                        if(!keep)
                        {
                                if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                                        FutureWorld.RemoveEntity(pChar);
                        }
                }
        }

        int StartTick = FutureWorld.GameTick();

        // Per-character point collection.
        std::vector<std::vector<vec2>> vPerCharPoints(vPredictIds.size());

        // Seed initial positions.
        for(size_t c = 0; c < vPredictIds.size(); c++)
        {
                CCharacter *pChar = FutureWorld.GetCharacterById(vPredictIds[c]);
                if(pChar)
                        vPerCharPoints[c].push_back(pChar->m_Pos);
        }

        // Simulate N ticks.
        for(int i = 0; i < NumTicks; i++)
        {
                int Tick = StartTick + i + 1;

                // Apply each character's last known input.
                for(size_t c = 0; c < vPredictIds.size(); c++)
                {
                        CCharacter *pChar = FutureWorld.GetCharacterById(vPredictIds[c]);
                        if(!pChar)
                                continue;
                        CNetObj_PlayerInput FutureInput = pChar->GetCore().m_Input;
                        pChar->OnDirectInput(&FutureInput);
                }

                FutureWorld.m_GameTick = Tick;

                for(size_t c = 0; c < vPredictIds.size(); c++)
                {
                        CCharacter *pChar = FutureWorld.GetCharacterById(vPredictIds[c]);
                        if(!pChar)
                                continue;
                        CNetObj_PlayerInput FutureInput = pChar->GetCore().m_Input;
                        pChar->OnPredictedInput(&FutureInput);
                }

                FutureWorld.Tick();

                // Collect positions (re-fetch in case a char was destroyed).
                for(size_t c = 0; c < vPredictIds.size(); c++)
                {
                        CCharacter *pChar = FutureWorld.GetCharacterById(vPredictIds[c]);
                        if(!pChar)
                                continue;
                        vPerCharPoints[c].push_back(pChar->m_Pos);
                }
        }

        // Draw each character's trajectory.
        for(size_t c = 0; c < vPerCharPoints.size(); c++)
        {
                DrawPolyline(vPerCharPoints[c], s.m_AlphaGradient);
        }
}

void CTrajectory::RenderPistol()
{
        const STypeSettings &s = m_aTypes[TRAJ_PISTOL];
        if(!s.m_Show)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        int LocalClientId = pGame->m_Snap.m_LocalClientId;
        if(LocalClientId < 0 || !pGame->m_Snap.m_aCharacters[LocalClientId].m_Active)
                return;

        CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalClientId);
        if(!pLocalChar)
                return;

        vec2 MyPos = pLocalChar->m_Pos;
        // Aim direction = direction from player to mouse target (in world space).
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos - MyPos);
        if(length(AimDir) < 1e-6f)
                return;

        // Raycast to wall.
        vec2 OutCol, OutBefore;
        CCollision *pColl = pGame->Collision();
        if(!pColl)
                return;

        vec2 End = MyPos + AimDir * 1000.0f;
        pColl->IntersectLine(MyPos, End, &OutCol, &OutBefore);

        std::vector<vec2> vPoints;
        vPoints.push_back(MyPos);
        vPoints.push_back(OutCol);
        DrawPolyline(vPoints, s.m_AlphaGradient);
}

void CTrajectory::RenderShotgun()
{
        const STypeSettings &s = m_aTypes[TRAJ_SHOTGUN];
        if(!s.m_Show)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        int LocalClientId = pGame->m_Snap.m_LocalClientId;
        if(LocalClientId < 0 || !pGame->m_Snap.m_aCharacters[LocalClientId].m_Active)
                return;

        CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalClientId);
        if(!pLocalChar)
                return;

        CCollision *pColl = pGame->Collision();
        if(!pColl)
                return;

        vec2 MyPos = pLocalChar->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos - MyPos);
        if(length(AimDir) < 1e-6f)
                return;

        // Shotgun spread: 5 rays at offsets. DDNet shotgun uses a fixed spread
        // pattern; we approximate with a symmetric fan around the aim direction.
        // Spread angles (radians): -0.14, -0.07, 0, 0.07, 0.14 (~ -8°..+8°).
        const float SpreadAngles[5] = {-0.14f, -0.07f, 0.0f, 0.07f, 0.14f};
        const float BaseAngle = atan2(AimDir.y, AimDir.x);

        for(int i = 0; i < 5; i++)
        {
                float a = BaseAngle + SpreadAngles[i];
                vec2 Dir = vec2(cosf(a), sinf(a));
                vec2 End = MyPos + Dir * 1000.0f;
                vec2 OutCol, OutBefore;
                pColl->IntersectLine(MyPos, End, &OutCol, &OutBefore);

                std::vector<vec2> vPoints;
                vPoints.push_back(MyPos);
                vPoints.push_back(OutCol);
                DrawPolyline(vPoints, s.m_AlphaGradient);
        }
}

void CTrajectory::RenderGrenade()
{
        const STypeSettings &s = m_aTypes[TRAJ_GRENADE];
        if(!s.m_Show)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        int LocalClientId = pGame->m_Snap.m_LocalClientId;
        if(LocalClientId < 0 || !pGame->m_Snap.m_aCharacters[LocalClientId].m_Active)
                return;

        CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalClientId);
        if(!pLocalChar)
                return;

        CCollision *pColl = pGame->Collision();
        if(!pColl)
                return;

        vec2 MyPos = pLocalChar->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos - MyPos);
        if(length(AimDir) < 1e-6f)
                return;

        // Grenade projectile simulation: initial velocity = AimDir * grenade_speed,
        // then apply gravity + bounce off walls for PredictionTicks ticks.
        // DDNet grenade tuning: speed=10, gravity=0.5, bounce factor varies.
        // We use a simplified physics model that matches DDNet's projectile arc.
        const float GrenadeSpeed = 10.0f; // tiles per tick
        const float Gravity = 0.5f; // per tick
        const float BounceFriction = 0.5f; // velocity retained on bounce

        vec2 Pos = MyPos + AimDir * 4.0f; // spawn offset (matches DDNet projectile spawn)
        vec2 Vel = AimDir * GrenadeSpeed;

        std::vector<vec2> vPoints;
        vPoints.push_back(Pos);

        for(int i = 0; i < s.m_PredictionTicks; i++)
        {
                vec2 NewPos = Pos + Vel;

                // Wall collision (X and Y axis separately for bouncing).
                vec2 OutCol, OutBefore;
                if(pColl->IntersectLine(Pos, NewPos, &OutCol, &OutBefore))
                {
                        // Hit wall — bounce.
                        NewPos = OutCol;
                        // Determine bounce axis: check which component caused the hit.
                        vec2 TestX = vec2(NewPos.x + (Vel.x > 0 ? 1.0f : -1.0f), NewPos.y);
                        vec2 Dummy1, Dummy2;
                        if(pColl->IntersectLine(NewPos, TestX, &Dummy1, &Dummy2))
                                Vel.x = -Vel.x * BounceFriction;

                        vec2 TestY = vec2(NewPos.x, NewPos.y + (Vel.y > 0 ? 1.0f : -1.0f));
                        if(pColl->IntersectLine(NewPos, TestY, &Dummy1, &Dummy2))
                                Vel.y = -Vel.y * BounceFriction;
                }

                Vel.y += Gravity;
                Pos = NewPos;
                vPoints.push_back(Pos);
        }

        DrawPolyline(vPoints, s.m_AlphaGradient);
}

void CTrajectory::RenderLaser()
{
        const STypeSettings &s = m_aTypes[TRAJ_LASER];
        if(!s.m_Show)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        int LocalClientId = pGame->m_Snap.m_LocalClientId;
        if(LocalClientId < 0 || !pGame->m_Snap.m_aCharacters[LocalClientId].m_Active)
                return;

        CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalClientId);
        if(!pLocalChar)
                return;

        CCollision *pColl = pGame->Collision();
        if(!pColl)
                return;

        vec2 MyPos = pLocalChar->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos - MyPos);
        if(length(AimDir) < 1e-6f)
                return;

        // Laser prediction with bounces. We simulate the laser bouncing off walls
        // until either PredictionTicks bounces are reached or energy runs out.
        // DDNet laser tuning: StartEnergy=800, BounceCost=100, BounceNum=2.
        // We use a fixed iteration count = PredictionTicks to bound the work.
        const float StartEnergy = 800.0f;
        const float BounceCost = 100.0f;
        const int MaxBounces = 2;

        vec2 Pos = MyPos;
        vec2 Dir = AimDir;
        float Energy = StartEnergy;
        int Bounces = 0;

        std::vector<vec2> vPoints;
        vPoints.push_back(Pos);

        for(int i = 0; i < s.m_PredictionTicks && Bounces <= MaxBounces && Energy > 0.0f; i++)
        {
                vec2 To = Pos + Dir * Energy;
                vec2 OutCol, OutBefore;
                int Res = pColl->IntersectLine(Pos, To, &OutCol, &OutBefore);
                if(Res)
                {
                        // Hit wall — bounce.
                        vec2 HitPos = OutCol;
                        vPoints.push_back(HitPos);

                        // Reflect direction off the wall normal.
                        // Simple reflection: invert the dominant axis component.
                        vec2 ToX = vec2(HitPos.x + (Dir.x > 0 ? 1.0f : -1.0f), HitPos.y);
                        vec2 ToY = vec2(HitPos.x, HitPos.y + (Dir.y > 0 ? 1.0f : -1.0f));
                        vec2 D1, D2;
                        bool HitX = pColl->IntersectLine(HitPos, ToX, &D1, &D2);
                        bool HitY = pColl->IntersectLine(HitPos, ToY, &D1, &D2);
                        if(HitX)
                                Dir.x = -Dir.x;
                        if(HitY)
                                Dir.y = -Dir.y;
                        Dir = normalize(Dir);

                        Energy -= distance(Pos, HitPos) + BounceCost;
                        Pos = HitPos;
                        Bounces++;
                }
                else
                {
                        // No wall hit — laser ends here.
                        vPoints.push_back(To);
                        break;
                }
        }

        DrawPolyline(vPoints, s.m_AlphaGradient);
}

void CTrajectory::RenderCurrentProjectiles(int WeaponType)
{
        // "Show for current" for pistol/shotgun/grenade: iterate the predicted
        // world's CProjectile entities matching the given weapon type and draw
        // their trajectory from StartTick to the current tick (already-flown
        // portion) plus remaining life (forward simulation up to LifeSpan).
        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        CGameWorld *pWorld = &pGame->m_PredictedWorld;
        if(!pWorld)
                return;

        // Map WEAPON_* to TRAJ_* index. Explicit switch — don't rely on enum value
        // arithmetic (WEAPON_GUN may not equal TRAJ_PISTOL on all builds).
        int trajIdx;
        switch(WeaponType)
        {
        case WEAPON_GUN: trajIdx = TRAJ_PISTOL; break;
        case WEAPON_SHOTGUN: trajIdx = TRAJ_SHOTGUN; break;
        case WEAPON_GRENADE: trajIdx = TRAJ_GRENADE; break;
        default: return; // unsupported weapon type (hammer/laser/ninja have no "current" projectile arc)
        }
        const STypeSettings &s = m_aTypes[trajIdx];

        int CurTick = pWorld->GameTick();
        int TickSpeed = pWorld->GameTickSpeed();

        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->m_pNextTypeEntity)
        {
                CProjectile *pProj = static_cast<CProjectile *>(pEnt);
                if(pProj->GetType() != WeaponType)
                        continue;
                if(pProj->GetOwner() != pGame->m_Snap.m_LocalClientId)
                        continue;

                // Build trajectory: from spawn (StartTick) to current tick,
                // then forward to LifeSpan end.
                const vec2 &Dir = pProj->GetDirection();
                int StartTick = pProj->GetStartTick();

                // Use GetPos to compute positions at each tick.
                // GetPos(Time) where Time is in seconds relative to StartTick.
                std::vector<vec2> vPoints;
                int EndTick = StartTick + 200; // generous upper bound; projectile self-removes earlier
                int FromTick = std::min(StartTick, CurTick);
                int ToTick = std::max(CurTick, EndTick);

                // Sample every 2 ticks for performance.
                for(int t = FromTick; t <= ToTick; t += 2)
                {
                        float Time = (float)(t - StartTick) / (float)TickSpeed;
                        vec2 Pos = pProj->GetPos(Time);
                        vPoints.push_back(Pos);
                        // Stop if projectile would have expired (simplified: stop at CurTick + LifeSpan).
                        if(t > CurTick + 100)
                                break;
                }

                DrawPolyline(vPoints, s.m_AlphaGradient);
        }
}

void CTrajectory::RenderCurrentLasers()
{
        // "Show for current" for laser: iterate CLaser entities and draw their
        // full bounce path (From -> current Pos -> predicted future bounces).
        // We re-simulate the laser's bounces forward from its current state.
        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        CGameWorld *pWorld = &pGame->m_PredictedWorld;
        if(!pWorld)
                return;

        const STypeSettings &s = m_aTypes[TRAJ_LASER];

        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->m_pNextTypeEntity)
        {
                CLaser *pLaser = static_cast<CLaser *>(pEnt);
                if(pLaser->GetOwner() != pGame->m_Snap.m_LocalClientId)
                        continue;

                // Draw the existing path: From -> Pos (the current segment).
                std::vector<vec2> vPoints;
                vPoints.push_back(pLaser->GetFrom());
                vPoints.push_back(pLaser->m_Pos);

                // Predict remaining bounces using the laser's current direction.
                CCollision *pColl = pWorld->Collision();
                if(!pColl)
                {
                        DrawPolyline(vPoints, s.m_AlphaGradient);
                        continue;
                }

                const float BounceCost = 100.0f;
                const int MaxBounces = 2;
                vec2 Pos = pLaser->m_Pos;
                vec2 Dir = pLaser->GetDir();
                vec2 From = pLaser->GetFrom();

                float Energy = 800.0f - distance(From, Pos);
                int Bounces = 0;

                for(int i = 0; i < s.m_PredictionTicks && Bounces <= MaxBounces && Energy > 0.0f; i++)
                {
                        vec2 To = Pos + Dir * Energy;
                        vec2 OutCol, OutBefore;
                        int Res = pColl->IntersectLine(Pos, To, &OutCol, &OutBefore);
                        if(Res)
                        {
                                vPoints.push_back(OutCol);
                                vec2 ToX = vec2(OutCol.x + (Dir.x > 0 ? 1.0f : -1.0f), OutCol.y);
                                vec2 ToY = vec2(OutCol.x, OutCol.y + (Dir.y > 0 ? 1.0f : -1.0f));
                                vec2 D1, D2;
                                bool HitX = pColl->IntersectLine(OutCol, ToX, &D1, &D2);
                                bool HitY = pColl->IntersectLine(OutCol, ToY, &D1, &D2);
                                if(HitX)
                                        Dir.x = -Dir.x;
                                if(HitY)
                                        Dir.y = -Dir.y;
                                Dir = normalize(Dir);

                                Energy -= distance(Pos, OutCol) + BounceCost;
                                Pos = OutCol;
                                Bounces++;
                        }
                        else
                        {
                                vPoints.push_back(To);
                                break;
                        }
                }

                DrawPolyline(vPoints, s.m_AlphaGradient);
        }
}

void CTrajectory::OnRender()
{
        SyncLegacyConfig();

        // Per-type prediction renderers.
        RenderTee();
        RenderPistol();
        RenderShotgun();
        RenderGrenade();
        RenderLaser();

        // "Show for current" — iterate existing entities.
        if(m_aTypes[TRAJ_PISTOL].m_ShowForCurrent)
                RenderCurrentProjectiles(WEAPON_GUN);
        if(m_aTypes[TRAJ_SHOTGUN].m_ShowForCurrent)
                RenderCurrentProjectiles(WEAPON_SHOTGUN);
        if(m_aTypes[TRAJ_GRENADE].m_ShowForCurrent)
                RenderCurrentProjectiles(WEAPON_GRENADE);
        if(m_aTypes[TRAJ_LASER].m_ShowForCurrent)
                RenderCurrentLasers();
}
