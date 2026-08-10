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

// v1.56.179: Prediction-based weapon trajectory.
// Spawns a real CProjectile/CLaser in a cloned CGameWorld and simulates N ticks,
// collecting positions each tick. This gives the REAL trajectory (with wall
// collisions, bounces, explosions) — not a hand-rolled approximation.
//
// Weapon type -> entity mapping (mirrors CCharacter::FireWeapon):
//   WEAPON_GUN      -> CProjectile (1 projectile, straight line, short lifetime)
//   WEAPON_GRENADE  -> CProjectile (1 projectile, parabolic arc, bounces off walls)
//   WEAPON_SHOTGUN  -> CProjectile (5 projectiles with spread) in vanilla,
//                      CLaser (WEAPON_SHOTGUN type) in DDRace
//   WEAPON_LASER    -> CLaser (bounces off walls until energy runs out)

// Helper: find a projectile owned by `ownerId` with matching `startTick`.
// Returns nullptr if the projectile was destroyed (exploded, expired).
static CProjectile *FindOwnedProjectile(CGameWorld *pWorld, int ownerId, int startTick)
{
        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
        {
                CProjectile *pProj = static_cast<CProjectile *>(pEnt);
                if(pProj->GetOwner() == ownerId && pProj->GetStartTick() == startTick)
                        return pProj;
        }
        return nullptr;
}

// Helper: find a laser owned by `ownerId`. Lasers don't expose StartTick via
// getter, so we match by owner only (there's typically only one laser per owner
// in flight at a time). Returns nullptr if the laser was destroyed.
static CLaser *FindOwnedLaser(CGameWorld *pWorld, int ownerId)
{
        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
        {
                CLaser *pLaser = static_cast<CLaser *>(pEnt);
                if(pLaser->GetOwner() == ownerId)
                        return pLaser;
        }
        return nullptr;
}

// v1.56.179: unified prediction-based weapon trajectory renderer.
// Spawns the weapon entity in a cloned world, simulates N ticks, draws the path.
void CTrajectory::RenderWeaponPredict(int WeaponType)
{
        // Map WEAPON_* to TRAJ_* index for settings lookup.
        int trajIdx;
        switch(WeaponType)
        {
        case WEAPON_GUN: trajIdx = TRAJ_PISTOL; break;
        case WEAPON_SHOTGUN: trajIdx = TRAJ_SHOTGUN; break;
        case WEAPON_GRENADE: trajIdx = TRAJ_GRENADE; break;
        case WEAPON_LASER: trajIdx = TRAJ_LASER; break;
        default: return;
        }
        const STypeSettings &s = m_aTypes[trajIdx];
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
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 Dir = normalize(AimPos - MyPos);
        if(length(Dir) < 1e-6f)
                return;

        vec2 ProjStartPos = MyPos + Dir * pLocalChar->GetProximityRadius() * 0.75f;

        // Clone the predicted world. We remove other characters to speed up
        // simulation (projectiles only need collision + owner char).
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(&pGame->m_PredictedWorld);

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalClientId)
                        continue;
                if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                        FutureWorld.RemoveEntity(pChar);
        }

        int StartTick = FutureWorld.GameTick();
        int TickSpeed = FutureWorld.GameTickSpeed();
        int TuneZone = pLocalChar->GetOverriddenTuneZone();

        // Determine if shotgun uses laser (DDRace) or projectile (vanilla).
        const bool isShotgunLaser = (WeaponType == WEAPON_SHOTGUN && FutureWorld.m_WorldConfig.m_IsDDRace);

        // For vanilla shotgun: spawn 5 projectiles with spread.
        // For everything else: spawn 1 entity.
        struct SSpawned
        {
                int entType; // CGameWorld::ENTTYPE_PROJECTILE or ENTTYPE_LASER
                int startTick; // for projectile matching
        };
        std::vector<SSpawned> vSpawned;

        if(WeaponType == WEAPON_LASER || isShotgunLaser)
        {
                // Spawn CLaser. LaserReach from tuning.
                float LaserReach = FutureWorld.GetTuning(TuneZone)->m_LaserReach;
                int laserType = (WeaponType == WEAPON_LASER) ? WEAPON_LASER : WEAPON_SHOTGUN;
                new CLaser(&FutureWorld, MyPos, Dir, LaserReach, LocalClientId, laserType);
                vSpawned.push_back({CGameWorld::ENTTYPE_LASER, StartTick});
        }
        else if(WeaponType == WEAPON_GUN)
        {
                int Lifetime = (int)(TickSpeed * FutureWorld.GetTuning(TuneZone)->m_GunLifetime);
                new CProjectile(&FutureWorld, WEAPON_GUN, LocalClientId, ProjStartPos, Dir, Lifetime, false, false, -1);
                vSpawned.push_back({CGameWorld::ENTTYPE_PROJECTILE, StartTick});
        }
        else if(WeaponType == WEAPON_GRENADE)
        {
                int Lifetime = (int)(TickSpeed * FutureWorld.GetTuning(TuneZone)->m_GrenadeLifetime);
                new CProjectile(&FutureWorld, WEAPON_GRENADE, LocalClientId, ProjStartPos, Dir, Lifetime, false, true, SOUND_GRENADE_EXPLODE);
                vSpawned.push_back({CGameWorld::ENTTYPE_PROJECTILE, StartTick});
        }
        else if(WeaponType == WEAPON_SHOTGUN)
        {
                // Vanilla shotgun: 5 projectiles with spread.
                const float aSpreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
                int ShotSpread = 2;
                int Lifetime = (int)(TickSpeed * FutureWorld.GetTuning(TuneZone)->m_ShotgunLifetime);
                for(int i = -ShotSpread; i <= ShotSpread; ++i)
                {
                        float a = angle(Dir) + aSpreading[i + 2];
                        float v = 1 - (absolute(i) / (float)ShotSpread);
                        float Speed = mix((float)FutureWorld.GlobalTuning()->m_ShotgunSpeeddiff, 1.0f, v);
                        vec2 SpreadDir = direction(a) * Speed;
                        new CProjectile(&FutureWorld, WEAPON_SHOTGUN, LocalClientId, ProjStartPos, SpreadDir, Lifetime, false, false, -1);
                }
                // All 5 projectiles share the same StartTick — we'll draw the first
                // one's path (they spread out so individual paths aren't very useful,
                // but the simulation still runs for all 5).
                vSpawned.push_back({CGameWorld::ENTTYPE_PROJECTILE, StartTick});
        }

        // Simulate N ticks, collecting positions for each spawned entity.
        // We draw one polyline per spawned entity (vanilla shotgun = 5 lines,
        // everything else = 1 line).
        std::vector<std::vector<vec2>> vPerEntityPoints(vSpawned.size());
        for(size_t e = 0; e < vSpawned.size(); e++)
        {
                vPerEntityPoints[e].push_back(ProjStartPos); // seed with spawn pos
        }

        for(int i = 0; i < s.m_PredictionTicks; i++)
        {
                FutureWorld.m_GameTick = StartTick + i + 1;
                FutureWorld.Tick();

                // Collect current position of each spawned entity.
                bool anyAlive = false;
                for(size_t e = 0; e < vSpawned.size(); e++)
                {
                        vec2 pos;
                        bool alive = false;
                        if(vSpawned[e].entType == CGameWorld::ENTTYPE_PROJECTILE)
                        {
                                if(CProjectile *pProj = FindOwnedProjectile(&FutureWorld, LocalClientId, vSpawned[e].startTick))
                                {
                                        pos = pProj->m_Pos;
                                        alive = true;
                                }
                        }
                        else
                        {
                                if(CLaser *pLaser = FindOwnedLaser(&FutureWorld, LocalClientId))
                                {
                                        pos = pLaser->m_Pos;
                                        alive = true;
                                }
                        }
                        if(alive)
                        {
                                vPerEntityPoints[e].push_back(pos);
                                anyAlive = true;
                        }
                }
                if(!anyAlive)
                        break;
        }

        // Draw each entity's trajectory.
        for(size_t e = 0; e < vPerEntityPoints.size(); e++)
        {
                DrawPolyline(vPerEntityPoints[e], s.m_AlphaGradient);
        }
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

        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
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

        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
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
        // v1.56.179: weapon types now use prediction-based simulation (spawn real
        // CProjectile/CLaser in cloned world, simulate N ticks, draw path).
        RenderTee();
        RenderWeaponPredict(WEAPON_GUN);
        RenderWeaponPredict(WEAPON_SHOTGUN);
        RenderWeaponPredict(WEAPON_GRENADE);
        RenderWeaponPredict(WEAPON_LASER);

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
