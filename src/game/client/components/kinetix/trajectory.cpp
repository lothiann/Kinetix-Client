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
        // v1.56.183: m_KxShowTrajectory is now the MASTER toggle (checked in OnRender),
        // exactly like g_Config.m_KxAimBot in aimbot.cpp. It is NO LONGER synced to
        // m_aTypes[TRAJ_TEE].m_Show — per-type Show is independent (like AimBot's
        // per-weapon m_Enabled). The ClickGUI parent "Trajectory" toggle reads/writes
        // g_Config.m_KxShowTrajectory directly (clickgui.cpp:1150,2580). Legacy bind
        // "toggle kx_show_trajectory 1 0" controls the master, not Tee's per-type Show.
        // Only m_KxTrajectoryTicks stays two-way synced with Tee's m_PredictionTicks.
        static int s_LastTicks = -1;
        static bool s_First = true;

        STypeSettings &tee = m_aTypes[TRAJ_TEE];

        if(s_First)
        {
                tee.m_PredictionTicks = g_Config.m_KxTrajectoryTicks;
                // v1.56.183: seed Tee's Show from the master toggle on first frame only.
                // Provides backward compat for users who had kx_show_trajectory=1 saved
                // in config — they'll still see the Tee trajectory without needing to
                // manually enable Tee's per-type Show. After this first frame, the
                // master and per-type Show are fully independent (like AimBot).
                tee.m_Show = g_Config.m_KxShowTrajectory != 0;
                s_First = false;
        }
        else
        {
                if(g_Config.m_KxTrajectoryTicks != s_LastTicks)
                        tee.m_PredictionTicks = g_Config.m_KxTrajectoryTicks;
        }

        g_Config.m_KxTrajectoryTicks = tee.m_PredictionTicks;
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
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_TRAJECTORY, (int)i), true);
                        pGraphics->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
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
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_TRAJECTORY, (int)(i - 1)), true);
                        IGraphics::CLineItem Line(vPoints[i - 1], vPoints[i]);
                        pGraphics->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
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

        // v1.56.183: seed with the RENDERED position (m_aClients[cid].m_RenderPos),
        // not the predicted m_Pos. This keeps the trajectory start glued to the
        // visible tee during movement (no jerk from prediction lag). Same approach
        // as aimbot.cpp:77 (MyPos = m_aClients[LocalId].m_RenderPos).
        for(size_t c = 0; c < vPredictIds.size(); c++)
        {
                int cid = vPredictIds[c];
                if(cid >= 0 && cid < MAX_CLIENTS)
                        vPerCharPoints[c].push_back(pGame->m_aClients[cid].m_RenderPos);
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
// v1.56.184: ClientId selects which player to predict for. The aim source differs:
//   - LocalClientId: m_Controls.m_aMousePos (live mouse, most responsive)
//   - Other players: prediction character's m_Core.m_Input.m_TargetX/Y (from snapshots)
// Both are aim OFFSETS from the tee in world units (not world positions).
void CTrajectory::RenderWeaponPredict(int WeaponType, int ClientId)
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
        // v1.56.184: validate ClientId (the player we're predicting for).
        if(ClientId < 0 || ClientId >= MAX_CLIENTS)
                return;
        if(!pGame->m_Snap.m_aCharacters[ClientId].m_Active)
                return;

        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(ClientId);
        if(!pChar)
                return;

        // v1.56.183: use the RENDERED position (m_aClients[ClientId].m_RenderPos) like
        // aimbot.cpp:77, so the trajectory start aligns with the visible tee.
        vec2 MyPos = pGame->m_aClients[ClientId].m_RenderPos;
        // v1.56.184: aim source depends on which player we're predicting for.
        // - Local player: m_Controls.m_aMousePos (live mouse, most responsive)
        // - Other players: their prediction character's m_TargetX/m_TargetY (from snapshots)
        // Both are aim OFFSETS from the tee in world units (NOT world positions), so the
        // direction is normalize(AimPos). Mirrors aimbot.cpp:388 + CCharacter::FireWeapon.
        vec2 AimPos;
        if(ClientId == LocalClientId)
                AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        else
        {
                CNetObj_PlayerInput input = pChar->GetCore().m_Input;
                AimPos = vec2(input.m_TargetX, input.m_TargetY);
        }
        vec2 Dir = normalize(AimPos);
        if(length(Dir) < 1e-6f)
                Dir = vec2(1, 0); // fallback: aim right

        vec2 ProjStartPos = MyPos + Dir * pChar->GetProximityRadius() * 0.75f;

        // Clone the predicted world.
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(&pGame->m_PredictedWorld);

        // v1.56.182: respect SimulatePlayers. When OFF (default), remove other
        // characters to speed up simulation (projectiles only need collision +
        // owner char). When ON, keep them so projectiles can hit other players.
        // v1.56.184: keep ClientId (the predicted player), remove everyone else.
        if(!s.m_SimulatePlayers)
        {
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(i == ClientId)
                                continue;
                        if(CCharacter *pOther = FutureWorld.GetCharacterById(i))
                                FutureWorld.RemoveEntity(pOther);
                }
        }

        int StartTick = FutureWorld.GameTick();
        int TickSpeed = FutureWorld.GameTickSpeed();
        int TuneZone = pChar->GetOverriddenTuneZone();

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
                new CLaser(&FutureWorld, MyPos, Dir, LaserReach, ClientId, laserType);
                vSpawned.push_back({CGameWorld::ENTTYPE_LASER, StartTick});
        }
        else if(WeaponType == WEAPON_GUN)
        {
                int Lifetime = (int)(TickSpeed * FutureWorld.GetTuning(TuneZone)->m_GunLifetime);
                new CProjectile(&FutureWorld, WEAPON_GUN, ClientId, ProjStartPos, Dir, Lifetime, false, false, -1);
                vSpawned.push_back({CGameWorld::ENTTYPE_PROJECTILE, StartTick});
        }
        else if(WeaponType == WEAPON_GRENADE)
        {
                int Lifetime = (int)(TickSpeed * FutureWorld.GetTuning(TuneZone)->m_GrenadeLifetime);
                new CProjectile(&FutureWorld, WEAPON_GRENADE, ClientId, ProjStartPos, Dir, Lifetime, false, true, SOUND_GRENADE_EXPLODE);
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
                        new CProjectile(&FutureWorld, WEAPON_SHOTGUN, ClientId, ProjStartPos, SpreadDir, Lifetime, false, false, -1);
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
                                if(CProjectile *pProj = FindOwnedProjectile(&FutureWorld, ClientId, vSpawned[e].startTick))
                                {
                                        // v1.56.182: CProjectile::Tick does NOT update m_Pos for
                                        // non-bouncing projectiles (pistol/grenade). Use GetPos(Ct)
                                        // to get the real current position at this tick.
                                        float Ct = (FutureWorld.GameTick() - pProj->GetStartTick()) / (float)FutureWorld.GameTickSpeed();
                                        pos = pProj->GetPos(Ct);
                                        alive = true;
                                }
                        }
                        else
                        {
                                if(CLaser *pLaser = FindOwnedLaser(&FutureWorld, ClientId))
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
        // v1.56.181: "Show for current" for pistol/shotgun/grenade — rewritten to use
        // REAL simulation instead of GetPos() parabola. GetPos ignores wall collisions,
        // bounces and explosions, so the old code drew a fake arc through walls.
        //
        // New approach: clone the predicted world, locate the matching projectile by
        // (owner, startTick), remove other characters (speedup), simulate N ticks
        // forward, collect positions. Only the REMAINING path is drawn (from current
        // projectile position to explosion/expiry) — not the already-flown portion.
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

        int LocalClientId = pGame->m_Snap.m_LocalClientId;

        // v1.56.183: collect matching projectiles as (owner, startTick) pairs.
        // When ShowForOtherPlayers is ON, track ALL players' projectiles (not just
        // local). This makes "Show for other players" work for show-for-current.
        struct SProjKey { int owner; int startTick; };
        std::vector<SProjKey> vKeys;
        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
        {
                CProjectile *pProj = static_cast<CProjectile *>(pEnt);
                if(pProj->GetType() != WeaponType)
                        continue;
                if(!s.m_ShowForOtherPlayers && pProj->GetOwner() != LocalClientId)
                        continue;
                vKeys.push_back({pProj->GetOwner(), pProj->GetStartTick()});
        }
        if(vKeys.empty())
                return;

        // Clone the predicted world once for all matching projectiles.
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(pWorld);

        // v1.56.183: build keep set — local + owners of tracked projectiles.
        // When SimulatePlayers is OFF, remove characters NOT in the keep set (speed).
        // When ON, keep all characters so projectiles can hit any player.
        bool keepClient[MAX_CLIENTS] = {false};
        keepClient[LocalClientId] = true;
        for(const auto &k : vKeys)
                if(k.owner >= 0 && k.owner < MAX_CLIENTS)
                        keepClient[k.owner] = true;

        if(!s.m_SimulatePlayers)
        {
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(keepClient[i])
                                continue;
                        if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                                FutureWorld.RemoveEntity(pChar);
                }
        }

        int StartTick = FutureWorld.GameTick();

        // Per-projectile point collection. Seed with the projectile's CURRENT position
        // (not its spawn position — Fix #6: only the remaining path is drawn).
        std::vector<std::vector<vec2>> vPerEntityPoints(vKeys.size());
        for(size_t e = 0; e < vKeys.size(); e++)
        {
                if(CProjectile *pProj = FindOwnedProjectile(&FutureWorld, vKeys[e].owner, vKeys[e].startTick))
                {
                        // v1.56.182: use GetPos(Ct) — m_Pos is not updated by Tick for
                        // non-bouncing projectiles (pistol/grenade).
                        float Ct = (FutureWorld.GameTick() - pProj->GetStartTick()) / (float)FutureWorld.GameTickSpeed();
                        vPerEntityPoints[e].push_back(pProj->GetPos(Ct));
                }
        }

        // Simulate N ticks forward, collecting positions.
        for(int i = 0; i < s.m_PredictionTicks; i++)
        {
                FutureWorld.m_GameTick = StartTick + i + 1;
                FutureWorld.Tick();

                bool anyAlive = false;
                for(size_t e = 0; e < vKeys.size(); e++)
                {
                        if(CProjectile *pProj = FindOwnedProjectile(&FutureWorld, vKeys[e].owner, vKeys[e].startTick))
                        {
                                // v1.56.182: use GetPos(Ct) — m_Pos is not updated by Tick for
                                // non-bouncing projectiles (pistol/grenade).
                                float Ct = (FutureWorld.GameTick() - pProj->GetStartTick()) / (float)FutureWorld.GameTickSpeed();
                                vPerEntityPoints[e].push_back(pProj->GetPos(Ct));
                                anyAlive = true;
                        }
                }
                if(!anyAlive)
                        break;
        }

        // Draw each projectile's remaining trajectory.
        for(size_t e = 0; e < vPerEntityPoints.size(); e++)
        {
                DrawPolyline(vPerEntityPoints[e], s.m_AlphaGradient);
        }
}

void CTrajectory::RenderCurrentLasers()
{
        // v1.56.181: "Show for current" for laser — rewritten to use REAL simulation
        // (same approach as RenderCurrentProjectiles). The old hand-rolled raycast
        // had two bugs:
        //   (1) MaxBounces=2 cap — real laser bounces ~8 times (LaserReach/BounceCost),
        //       so only 2 bounces were drawn ("мало углов").
        //   (2) Drew From -> Pos (already-flown segment) + only 2 future bounces.
        // New approach: clone world, find the laser, simulate N ticks (1 bounce/tick),
        // collect positions. Only the REMAINING path is drawn (from current m_Pos).
        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        CGameWorld *pWorld = &pGame->m_PredictedWorld;
        if(!pWorld)
                return;

        const STypeSettings &s = m_aTypes[TRAJ_LASER];

        int LocalClientId = pGame->m_Snap.m_LocalClientId;

        // v1.56.183: build keep set from matching lasers' owners (do this on the
        // live world before cloning so we know which characters to keep).
        // When ShowForOtherPlayers is ON, track ALL players' lasers.
        bool keepClient[MAX_CLIENTS] = {false};
        keepClient[LocalClientId] = true;
        int matchCount = 0;
        for(CEntity *pEnt = pWorld->FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
        {
                CLaser *pLaser = static_cast<CLaser *>(pEnt);
                if(!s.m_ShowForOtherPlayers && pLaser->GetOwner() != LocalClientId)
                        continue;
                if(pLaser->GetOwner() >= 0 && pLaser->GetOwner() < MAX_CLIENTS)
                        keepClient[pLaser->GetOwner()] = true;
                matchCount++;
        }
        if(matchCount == 0)
                return;

        // Clone the predicted world.
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(pWorld);

        // v1.56.183: remove characters not in keep set (unless SimulatePlayers is on).
        if(!s.m_SimulatePlayers)
        {
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(keepClient[i])
                                continue;
                        if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                                FutureWorld.RemoveEntity(pChar);
                }
        }

        // Collect matching lasers from the CLONE (positions are post-copy, so they
        // reflect the live state). We'll re-find them each tick by iteration.
        std::vector<vec2> vInitialPos;
        for(CEntity *pEnt = FutureWorld.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
        {
                CLaser *pLaser = static_cast<CLaser *>(pEnt);
                if(!s.m_ShowForOtherPlayers && pLaser->GetOwner() != LocalClientId)
                        continue;
                vInitialPos.push_back(pLaser->m_Pos);
        }
        if(vInitialPos.empty())
                return;

        // Per-laser point collection. Seed with current position (Fix #6: only
        // remaining path, not the already-flown From -> Pos segment).
        std::vector<std::vector<vec2>> vPerLaserPoints(vInitialPos.size());
        for(size_t e = 0; e < vInitialPos.size(); e++)
                vPerLaserPoints[e].push_back(vInitialPos[e]);

        int StartTick = FutureWorld.GameTick();

        // Simulate N ticks forward. CLaser::Tick calls DoBounce() once per tick
        // (when LaserBounceDelay has elapsed, which is 0 by default → every tick),
        // so each tick produces one bounce point. With PredictionTicks=10 we get
        // up to 10 bounces — matching the original engine's laser behaviour.
        // v1.56.183: iterate all lasers and assign by index (preserves multi-laser
        // per owner tracking; ShowForOtherPlayers extends filter to all owners).
        for(int i = 0; i < s.m_PredictionTicks; i++)
        {
                FutureWorld.m_GameTick = StartTick + i + 1;
                FutureWorld.Tick();

                bool anyAlive = false;
                size_t e = 0;
                for(CEntity *pEnt = FutureWorld.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt && e < vPerLaserPoints.size(); pEnt = pEnt->TypeNext())
                {
                        CLaser *pLaser = static_cast<CLaser *>(pEnt);
                        if(!s.m_ShowForOtherPlayers && pLaser->GetOwner() != LocalClientId)
                                continue;
                        vPerLaserPoints[e].push_back(pLaser->m_Pos);
                        anyAlive = true;
                        e++;
                }
                if(!anyAlive)
                        break;
        }

        // Draw each laser's remaining bounce path.
        for(size_t e = 0; e < vPerLaserPoints.size(); e++)
        {
                DrawPolyline(vPerLaserPoints[e], s.m_AlphaGradient);
        }
}

void CTrajectory::OnRender()
{
        SyncLegacyConfig();

        // v1.56.183: master toggle — like g_Config.m_KxAimBot in aimbot.cpp:60,91.
        // When OFF, nothing renders (no tee, no weapon preview, no show-for-current).
        // The ClickGUI parent "Trajectory" toggle and the legacy bind
        // "toggle kx_show_trajectory 1 0" both control this config var.
        if(!g_Config.m_KxShowTrajectory)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        int LocalClientId = pGame->m_Snap.m_LocalClientId;
        if(LocalClientId < 0 || !pGame->m_Snap.m_aCharacters[LocalClientId].m_Active)
                return;

        // v1.56.183: set up screen transform like aimbot.cpp:87 so world-space
        // coordinates map correctly to the visible screen.
        Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

        RenderTee();

        // v1.56.182: only show the weapon trajectory PREVIEW for the currently held
        // weapon (like aimbot). Previously all 4 weapon types were rendered
        // unconditionally (each checked its own m_Show), so enabling Show for
        // multiple types drew them all at once regardless of held weapon.
        // v1.56.184: also predict for OTHER players when their weapon type's
        // Show+ShowForOtherPlayers are both ON. Each player gets their own
        // RenderWeaponPredict call (own cloned world + own aim direction).
        {
                CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalClientId);
                if(pLocalChar)
                {
                        int weapon = pLocalChar->GetActiveWeapon();
                        // RenderWeaponPredict handles GUN/SHOTGUN/GRENADE/LASER; other
                        // weapons (hammer/ninja) hit the default: return in the switch.
                        RenderWeaponPredict(weapon, LocalClientId);
                }

                // v1.56.184: predict for other players. For each active other player,
                // check their held weapon's type settings — if Show+ShowForOtherPlayers
                // are both ON, predict their trajectory too.
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(i == LocalClientId)
                                continue;
                        if(!pGame->m_Snap.m_aCharacters[i].m_Active)
                                continue;
                        CCharacter *pOtherChar = pGame->m_PredictedWorld.GetCharacterById(i);
                        if(!pOtherChar)
                                continue;
                        int otherWeapon = pOtherChar->GetActiveWeapon();
                        // Map weapon to traj index (same switch as RenderWeaponPredict).
                        int otherTrajIdx;
                        switch(otherWeapon)
                        {
                        case WEAPON_GUN: otherTrajIdx = TRAJ_PISTOL; break;
                        case WEAPON_SHOTGUN: otherTrajIdx = TRAJ_SHOTGUN; break;
                        case WEAPON_GRENADE: otherTrajIdx = TRAJ_GRENADE; break;
                        case WEAPON_LASER: otherTrajIdx = TRAJ_LASER; break;
                        default: continue; // hammer/ninja: no preview
                        }
                        const STypeSettings &os = m_aTypes[otherTrajIdx];
                        if(!os.m_Show || !os.m_ShowForOtherPlayers)
                                continue;
                        RenderWeaponPredict(otherWeapon, i);
                }
        }

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
