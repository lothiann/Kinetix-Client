#include <game/client/components/kinetix/aimbot.h>

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>
#include <base/vmath.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <game/client/gameclient.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <game/client/components/kinetix/kinetix_lines.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/tuning.h>
#include <generated/protocol.h>

#include <algorithm>
#include <cmath>
#include <vector>

void CAimBot::OnConsoleInit()
{
        // v1.56.171 BUG9: kx_aimbot and kx_triggerbot are now MACRO_CONFIG_INT cvars
        // (KxAimBot, KxTriggerBot). `toggle kx_aimbot 1 0` works in binds.
}

void CAimBot::OnReset()
{
        g_Config.m_KxAimBot = false;
        g_Config.m_KxTriggerBot = false;
        m_LatencyCounter = 0;
        m_PrevFire = false;
        m_PrevHook = false;
}

float CAimBot::GetWeaponRadius(int Weapon) const
{
        CGameClient *pGame = GameClient();
        const CTuningParams *pT = pGame->m_PredictedWorld.GlobalTuning();
        if(!pT)
                return 300.0f;
        switch(Weapon)
        {
        case WEAPON_HAMMER: return 28.0f * 2.0f;
        case WEAPON_GUN: return pT->m_GunSpeed * pT->m_GunLifetime;
        case WEAPON_SHOTGUN: return pT->m_LaserReach; // NOT ShotgunSpeed*Lifetime
        case WEAPON_GRENADE: return pT->m_GrenadeSpeed * pT->m_GrenadeLifetime;
        case WEAPON_LASER: return pT->m_LaserReach;
        case WEAPON_NINJA: return 280.0f;
        case 6: return pT->m_HookLength; // hook
        default: return 300.0f;
        }
}

void CAimBot::OnUpdate()
{
        // AimBot/TriggerBot logic runs in OnUpdate (BEFORE SnapInput) so silent aim
        // via m_LaserUnfreezeAimActive is applied to m_TargetX/Y in SnapInput.
        if(g_Config.m_KxAimBot)
                UpdateAimBot();
        if(g_Config.m_KxTriggerBot)
                UpdateTriggerBot();
}

void CAimBot::OnRender()
{
        // Render FOV/Radius circles for current weapon (visual only, no logic).
        // Use m_aClients[LocalId].m_RenderPos — the SAME interpolated render position
        // CPlayers::OnRender uses to draw the tee (see players.cpp:299). This keeps the
        // circles glued to the visible tee during movement (no jerk from prediction lag).
        CGameClient *pGame = GameClient();
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0 || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
                return;

        vec2 MyPos = pGame->m_aClients[LocalId].m_RenderPos;
        vec2 LocalAim = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(LocalAim);
        if(length(AimDir) < 0.001f)
                AimDir = vec2(1, 0);

        // Active weapon from predicted world (snapshot doesn't expose it directly).
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        int weapon = pChar ? pChar->GetActiveWeapon() : -1;
        bool hookHeld = pGame->m_Controls.m_aInputData[g_Config.m_ClDummy].m_Hook != 0;
        Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

        float alpha = KxLineAlpha(KX_LINE_AIMBOT); // v1.56.108: per-component

        if(g_Config.m_KxAimBot)
        {
                // Active weapon (0-5)
                if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
                {
                        if(m_AimBotShowFov)
                                RenderFov(m_AimBotWeapons[weapon], MyPos, AimDir, alpha);
                        if(m_AimBotShowRadius)
                                RenderRadius(m_AimBotWeapons[weapon], MyPos, AimDir, alpha);
                }
                // Hook (6) — only show if hook held or AimMode=always
                if(m_AimBotWeapons[6].m_Enabled && (hookHeld || m_AimBotWeapons[6].m_AimMode == 2))
                {
                        if(m_AimBotShowFov)
                                RenderFov(m_AimBotWeapons[6], MyPos, AimDir, alpha);
                        if(m_AimBotShowRadius)
                                RenderRadius(m_AimBotWeapons[6], MyPos, AimDir, alpha);
                }
        }
        if(g_Config.m_KxTriggerBot && weapon >= 0 && weapon < NUM_WEAPONS_TRIGGER && m_TriggerBotWeapons[weapon].m_Enabled)
        {
                if(m_TriggerBotShowFov)
                        RenderFov(m_TriggerBotWeapons[weapon], MyPos, AimDir, alpha * 0.7f);
                if(m_TriggerBotShowRadius)
                        RenderRadius(m_TriggerBotWeapons[weapon], MyPos, AimDir, alpha * 0.7f);
        }

        // TODO: Replace fixed-angle raycasting with shadowcasting or DDA for pixel-perfect gap detection between blocks.
        // v1.56.98: Render raycast lines — Show angles (AB) and/or Show success.
        auto renderRays = [&](const SWeaponSettings &s) {
                bool showAll = s.m_ShowRaycastLinesAB;
                bool showSuccess = s.m_ShowSuccess;
                if(!showAll && !showSuccess)
                        return;
                int angles = std::max(1, std::min(64, s.m_RaycastAnglesAB));
                float halfFov = (s.m_Fov * 0.5f) * (pi / 180.0f);
                float myAng = atan2f(AimDir.y, AimDir.x);
                constexpr float HB_HITBOX = CCharacterCore::PhysicalSize() + 2.0f;
                ColorRGBA normalCol = ColorRGBA(KxLineColor(KX_LINE_AIMBOT), true);
                ColorRGBA successCol = ColorRGBA(s.m_SuccessColor, true);
                // v1.56.98: Cache rays for FindTarget to use (no re-computation in OnUpdate)
                m_DebugRayHits.clear();
                m_DebugShowRays = true;
                Graphics()->TextureClear();
                Graphics()->LinesBegin();
                for(int i = 0; i <= angles; i++)
                {
                        float t = (float)i / (float)angles;
                        float a = myAng - halfFov + t * (2.0f * halfFov);
                        vec2 end = MyPos + vec2(cosf(a), sinf(a)) * s.m_Radius;
                        vec2 hitPos;
                        if(pGame->Collision()->IntersectLineTeleHook(MyPos, end, &hitPos, nullptr) > 0)
                        {
                                end = hitPos;
                                m_DebugRayHits.push_back({end, length(end - MyPos)});
                        }
                        else
                                m_DebugRayHits.push_back({end, s.m_Radius});
                        // Check if this ray hits any player hitbox
                        bool hitsPlayer = false;
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                if(c == LocalId || !pGame->m_aClients[c].m_Active || !pGame->m_Snap.m_aCharacters[c].m_Active)
                                        continue;
                                vec2 prevP = vec2((float)pGame->m_Snap.m_aCharacters[c].m_Prev.m_X, (float)pGame->m_Snap.m_aCharacters[c].m_Prev.m_Y);
                                vec2 curP = vec2((float)pGame->m_Snap.m_aCharacters[c].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[c].m_Cur.m_Y);
                                vec2 pPos = mix(prevP, curP, pGame->Client()->IntraGameTick(g_Config.m_ClDummy));
                                vec2 closestPt;
                                if(closest_point_on_line(MyPos, end, pPos, closestPt))
                                {
                                        if(distance(pPos, closestPt) < HB_HITBOX)
                                        {
                                                hitsPlayer = true;
                                                break;
                                        }
                                }
                        }
                        if(hitsPlayer && showSuccess)
                                Graphics()->SetColor(successCol.r, successCol.g, successCol.b, successCol.a);
                        else if(showAll)
                                Graphics()->SetColor(normalCol.r, normalCol.g, normalCol.b, 0.3f);
                        else
                                continue; // showSuccess only — skip non-hit rays
                        IGraphics::CLineItem Line(MyPos.x, MyPos.y, end.x, end.y);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        };
        if(g_Config.m_KxAimBot && weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
                renderRays(m_AimBotWeapons[weapon]);
        if(g_Config.m_KxAimBot && m_AimBotWeapons[6].m_Enabled && (hookHeld || m_AimBotWeapons[6].m_AimMode == 2))
                renderRays(m_AimBotWeapons[6]);
        if(g_Config.m_KxTriggerBot && weapon >= 0 && weapon < NUM_WEAPONS_TRIGGER && m_TriggerBotWeapons[weapon].m_Enabled)
                renderRays(m_TriggerBotWeapons[weapon]);
}

bool CAimBot::PassesFilters(int ClientId, int LocalId, const SWeaponSettings &Settings)
{
        CGameClient *pGame = GameClient();
        if(!pGame->m_aClients[ClientId].m_Active)
                return false;
        if(!pGame->m_Snap.m_aCharacters[ClientId].m_Active)
                return false;
        // Team filter
        if(Settings.m_TeamFilter == 1)
        {
                if(pGame->m_Teams.SameTeam(LocalId, ClientId))
                        return false;
        }
        else if(Settings.m_TeamFilter == 2)
        {
                if(!pGame->m_Teams.SameTeam(LocalId, ClientId))
                        return false;
        }
        // Friend filter
        if(Settings.m_FriendFilter == 1 && pGame->m_aClients[ClientId].m_Friend)
                return false;
        // Dummy filter
        bool isDummy = false;
        for(int d = 0; d < MAX_DUMMIES; d++)
        {
                if(pGame->m_aLocalIds[d] == ClientId)
                {
                        isDummy = true;
                        break;
                }
        }
        if(Settings.m_DummyFilter == 1 && isDummy)
                return false;
        // Freeze filter
        bool isFrozen = pGame->m_aClients[ClientId].m_FreezeEnd != 0;
        if(Settings.m_FreezeFilter == 1 && !isFrozen)
                return false;
        if(Settings.m_FreezeFilter == 2 && isFrozen)
                return false;
        return true;
}

int CAimBot::FindTarget(const SWeaponSettings &Settings, int Weapon, int LocalId, const vec2 &MyPos, const vec2 &AimDir, const vec2 &AimPos, vec2 &outAimPoint)
{
        CGameClient *pGame = GameClient();
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        constexpr float HB_HITBOX = CCharacterCore::PhysicalSize() + 2.0f;
        constexpr float HB_HALF = HB_HITBOX * 0.5f;

        int bestTarget = -1;
        float bestScore = 1e18f;
        vec2 bestAimPoint = vec2(0, 0);

        // v1.56.196: Real Predict — full physics simulation (CopyWorld + Tick).
        // One CopyWorld per FindTarget call, pre-roll MAX_PREDICT_TICKS ticks
        // ahead, recording each client's position after each tick into a table.
        // Then for each target we walk the table forward one tick at a time:
        //   tick t → dist(MyPos, pos[t]) → flightTime = dist/speed (or dist*50/speed)
        //   if flightTime <= t → projectile caught up between (t-1) and t.
        //     Interpolate the exact sub-tick moment via linear interp of flightTime:
        //       u = (prevFlightTime - (t-1)) / (1 - (flightTime - prevFlightTime))
        //     aim at mix(pos[t-1], pos[t], u).
        // Gravity, friction, wall collisions, and hook physics all apply during
        // the pre-roll — prediction matches real server state much better than
        // linear extrapolation on 10+ tick horizons.
        //
        // Stack usage: positions[64][21] of vec2 = ~10.7 KB. Acceptable.
        CGameWorld *pPredWorld = nullptr;
        vec2 predPositions[MAX_CLIENTS][MAX_PREDICT_TICKS + 1];
        bool predValid[MAX_CLIENTS] = {};
        float projSpeed = 0.0f;
        bool projSpeedPerTick = false; // false = units/sec (gun/grenade), true = units/tick (hook)
        bool predictActive = false;

        if(Settings.m_Predict)
        {
                const CTuningParams *pT = pGame->m_PredictedWorld.GlobalTuning();
                if(pT)
                {
                        switch(Weapon)
                        {
                        // Gun/Grenade speeds are in units/SECOND (used by CalcPos where Time is in seconds).
                        // flightTicks = ceil(dist * TICKS_PER_SEC / speed)
                        case WEAPON_GUN:     projSpeed = pT->m_GunSpeed;     projSpeedPerTick = false; break;
                        case WEAPON_GRENADE: projSpeed = pT->m_GrenadeSpeed; projSpeedPerTick = false; break;
                        // Hook speed is in units/TICK (m_HookPos += HookDir * HookFireSpeed per Tick()).
                        // flightTicks = ceil(dist / speed)
                        case 6:              projSpeed = pT->m_HookFireSpeed; projSpeedPerTick = true; break;
                        default:             break; // instant weapons (hammer/shotgun/laser/ninja) — no predict
                        }
                        if(projSpeed > 1.0f)
                                predictActive = true;
                }
        }

        if(predictActive)
        {
                pPredWorld = new CGameWorld;
                pPredWorld->CopyWorld(&pGame->m_PredictedWorld);

                // Record each client's CURRENT position as tick 0.
                for(int c = 0; c < MAX_CLIENTS; c++)
                {
                        if(CCharacter *pChar = pPredWorld->GetCharacterById(c))
                        {
                                predPositions[c][0] = pChar->Core()->m_Pos;
                                predValid[c] = true;
                        }
                }

                // Pre-roll MAX_PREDICT_TICKS ticks. Each character gets its last known
                // input fed back to it (preserves movement direction, jump state, hook
                // state). This is the same pattern laser_unfreeze uses for its trigger
                // prediction — proven to match real server-side state closely.
                //
                // Per-tick ordering (matches laser_unfreeze exactly):
                //   1. OnDirectInput on all chars (still on previous tick)
                //   2. Advance m_GameTick to the new tick
                //   3. OnPredictedInput on all chars (now on the new tick)
                //   4. World Tick() — applies physics for the new tick
                const int startTick = pPredWorld->GameTick();
                for(int t = 1; t <= MAX_PREDICT_TICKS; t++)
                {
                        // 1. OnDirectInput for all characters (previous tick)
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                CCharacter *pChar = pPredWorld->GetCharacterById(c);
                                if(!pChar)
                                        continue;
                                CNetObj_PlayerInput inp = pChar->Core()->m_Input;
                                pChar->OnDirectInput(&inp);
                        }
                        // 2. Advance tick
                        pPredWorld->m_GameTick = startTick + t;
                        // 3. OnPredictedInput for all characters (new tick)
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                CCharacter *pChar = pPredWorld->GetCharacterById(c);
                                if(!pChar)
                                        continue;
                                CNetObj_PlayerInput inp = pChar->Core()->m_Input;
                                pChar->OnPredictedInput(&inp);
                        }
                        // 4. World Tick — applies physics for this tick
                        pPredWorld->Tick();

                        // Record positions after this tick
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                if(CCharacter *pChar = pPredWorld->GetCharacterById(c))
                                        predPositions[c][t] = pChar->Core()->m_Pos;
                                else
                                        predValid[c] = false; // character disappeared during sim (death/leave)
                        }
                }
        }

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                if(!PassesFilters(i, LocalId, Settings))
                        continue;
                vec2 prevPos = vec2((float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_Y);
                vec2 curPos = vec2((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                float intra = pGame->Client()->IntraGameTick(g_Config.m_ClDummy);
                vec2 targetCenter = mix(prevPos, curPos, intra);
                // v1.56.194: Real Predict — replace targetCenter with the predicted
                // position where the projectile will intersect the target. Walks the
                // pre-rolled position table forward one tick at a time.
                if(predictActive && predValid[i])
                {
                        // v1.56.196: Sub-tick interpolation. flightTime is now a FLOAT
                        // (not ceil'd to int). Walk forward through the pre-rolled position
                        // table. Find the first tick t where flightTime <= t — that's where
                        // the projectile catches up with the target. The real intersection
                        // happened BETWEEN tick (t-1) and tick t (sub-tick moment).
                        //
                        // Linear interp of flightTime between (t-1) and t:
                        //   flightTime(u) = prevFlightTime + (flightTime - prevFlightTime) * u
                        //   where u in [0,1] is the fraction from tick (t-1) to tick t
                        // Hook catches up when flightTime(u) == (t-1) + u:
                        //   prevFlightTime + (flightTime - prevFlightTime) * u = (t-1) + u
                        //   => u = (prevFlightTime - (t-1)) / (1 - (flightTime - prevFlightTime))
                        //
                        // Then aim at mix(pos[t-1], pos[t], u) — the target's position at
                        // the exact sub-tick moment of intersection.
                        vec2 predictedPos = predPositions[i][0]; // fallback
                        bool foundPrediction = false;
                        float prevFlightTime = -1.0f;

                        for(int t = 0; t <= MAX_PREDICT_TICKS; t++)
                        {
                                float dist = length(predPositions[i][t] - MyPos);
                                if(dist < 1.0f)
                                {
                                        // Target literally on top of us — aim at current position.
                                        predictedPos = predPositions[i][t];
                                        foundPrediction = true;
                                        break;
                                }

                                // v1.56.195: Hook speed is units/TICK, gun/grenade are units/SECOND.
                                float flightTime;
                                if(projSpeedPerTick)
                                        flightTime = dist / projSpeed;
                                else
                                        flightTime = dist * (float)SERVER_TICK_SPEED / projSpeed;

                                if(flightTime <= (float)t)
                                {
                                        // Projectile caught up between tick (t-1) and tick t (or at t).
                                        if(t == 0 || prevFlightTime < 0.0f)
                                        {
                                                // Caught up on the very first tick — no previous to interp.
                                                predictedPos = predPositions[i][t];
                                        }
                                        else
                                        {
                                                float prevTick = (float)(t - 1);
                                                float numerator = prevFlightTime - prevTick;
                                                float denominator = 1.0f - (flightTime - prevFlightTime);
                                                float u;
                                                if(denominator > 0.001f)
                                                        u = numerator / denominator;
                                                else
                                                        u = 0.5f; // fallback (denominator<=0 shouldn't happen here)
                                                if(u < 0.0f) u = 0.0f;
                                                if(u > 1.0f) u = 1.0f;
                                                predictedPos = mix(predPositions[i][t - 1], predPositions[i][t], u);
                                        }
                                        foundPrediction = true;
                                        break;
                                }

                                prevFlightTime = flightTime;
                        }

                        if(!foundPrediction)
                        {
                                // Never caught up within MAX_PREDICT_TICKS — use last available position.
                                predictedPos = predPositions[i][MAX_PREDICT_TICKS];
                        }
                        targetCenter = predictedPos;
                }
                vec2 hbPoints[5] = {
                        targetCenter,
                        targetCenter + vec2(-HB_HALF, -HB_HALF),
                        targetCenter + vec2(HB_HALF, -HB_HALF),
                        targetCenter + vec2(-HB_HALF, HB_HALF),
                        targetCenter + vec2(HB_HALF, HB_HALF),
                };
                float bestSampleScore = 1e18f;
                bool anyPass = false;
                vec2 sampleAimPoint = targetCenter;
                for(int s = 0; s < 5; s++)
                {
                        vec2 sp = hbPoints[s];
                        vec2 toSample = sp - MyPos;
                        float dist = length(toSample);
                        if(dist > Settings.m_Radius || dist < 1.0f)
                                continue;
                        vec2 dir = normalize(toSample);
                        float sampleAngle = atan2f(dir.y, dir.x);
                        float angleDiff = fabsf(atan2f(sinf(myAngle - sampleAngle), cosf(myAngle - sampleAngle)));
                        if(angleDiff > halfFovRad)
                                continue;
                        bool visible = false;
                        vec2 rayHitPoint = sp;
                        if(Settings.m_Rules == 0)
                        {
                                visible = true;
                        }
                        else if(Settings.m_Rules == 1)
                        {
                                visible = IsVisible(MyPos, sp);
                        }
                        else if(Settings.m_Rules == 2 && !m_DebugRayHits.empty())
                        {
                                float bestRayDist = 1e18f;
                                for(const auto &rh : m_DebugRayHits)
                                {
                                        vec2 rayDir = normalize(rh.end - MyPos);
                                        float proj = dot(sp - MyPos, rayDir);
                                        if(proj < 0.0f || proj > rh.dist)
                                                continue;
                                        vec2 closestPt;
                                        if(closest_point_on_line(MyPos, rh.end, sp, closestPt) && distance(sp, closestPt) < HB_HITBOX)
                                        {
                                                visible = true;
                                                float centerProj = dot(targetCenter - MyPos, rayDir);
                                                vec2 centerPerp = (targetCenter - MyPos) - rayDir * centerProj;
                                                float centerDist = length(centerPerp);
                                                if(centerDist < bestRayDist)
                                                {
                                                        bestRayDist = centerDist;
                                                        rayHitPoint = MyPos + rayDir * centerProj;
                                                }
                                        }
                                }
                        }
                        if(visible)
                        {
                                float score;
                                if(Settings.m_Priority == 0)
                                        score = angleDiff;
                                else if(Settings.m_Priority == 1)
                                        score = length(AimPos - sp);
                                else
                                        score = dist;
                                if(score < bestSampleScore)
                                {
                                        bestSampleScore = score;
                                        anyPass = true;
                                        sampleAimPoint = Settings.m_UseAngle ? rayHitPoint : targetCenter;
                                }
                        }
                }
                if(anyPass && bestSampleScore < bestScore)
                {
                        bestScore = bestSampleScore;
                        bestTarget = i;
                        bestAimPoint = sampleAimPoint;
                }
        }
        // v1.56.194: Free the predicted world before returning. CopyWorld allocated
        // characters with `new`, so `delete` is correct here (RemoveEntity only unlinks
        // — it does NOT free). See laser_unfreeze Phase 1 comment for the same pattern.
        if(pPredWorld)
                delete pPredWorld;

        outAimPoint = bestAimPoint;
        return bestTarget;
}

bool CAimBot::IsVisible(const vec2 &From, const vec2 &To)
{
        return GameClient()->Collision()->IntersectLineTeleHook(From, To, nullptr, nullptr) == 0;
}

void CAimBot::ApplyAim(int ClientId, const vec2 &MyPos, const SWeaponSettings &Settings, const vec2 &AimPoint)
{
        CGameClient *pGame = GameClient();
        vec2 aimOffset = AimPoint - MyPos;
        if(Settings.m_AimMode == 0) // Silent
        {
                // Silent mode: do NOT change m_aMousePos (visible crosshair stays where the player aims).
                // Set the silent aim override flag — SnapInput (controls.cpp) will use this offset
                // for m_TargetX/Y right before sending to server. Same channel as Laser Unfreeze silent.
                pGame->m_Controls.m_LaserUnfreezeAimActive = true;
                pGame->m_Controls.m_LaserUnfreezeAimOffset = aimOffset;
        }
        else // Trigger (1) or Always (2) — move visible crosshair
        {
                // TODO: remove ghost when show for me — visible crosshair moves to
                // aimbot aim, but for a few frames the player's real angle flickers
                // through. Same issue as fake aim showForMe before v1.56.135 sync.
                pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy] = aimOffset;
        }
}

void CAimBot::FireWeapon()
{
        CNetObj_PlayerInput *pInput = &GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
        if((pInput->m_Fire & 1) == 0)
                pInput->m_Fire = (pInput->m_Fire + 1) | 1;
}

void CAimBot::DoHook()
{
        GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy].m_Hook = 1;
}

void CAimBot::UpdateAimBot()
{
        CGameClient *pGame = GameClient();
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0)
                return;
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pChar)
                return;

        int weapon = pChar->GetActiveWeapon();
        const CNetObj_PlayerInput &Input = pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
        bool fire = (Input.m_Fire & 1) != 0;
        bool hook = Input.m_Hook != 0;
        vec2 MyPos = pChar->Core()->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos);
        if(length(AimDir) < 0.001f)
                AimDir = vec2(1, 0);

        // Silent (0) and Trigger (1) both trigger on willFire — 1-tick CopyWorld prediction
        // (same technique as fake aim in controls.cpp). This catches every real shot
        // (including auto-fire during reload), not just the rising edge of m_Fire.
        // Difference between Silent and Trigger is in ApplyAim (visible vs invisible),
        // NOT in the trigger condition. Always (2) triggers every tick.
        // Hook (weapon 6) has no reload cadence, so Silent/Trigger use edge on m_Hook.
        bool willFire = false;
        bool needWillFire = weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1
                && m_AimBotWeapons[weapon].m_Enabled
                && (m_AimBotWeapons[weapon].m_AimMode == 0 || m_AimBotWeapons[weapon].m_AimMode == 1);
        if(needWillFire)
        {
                int tickBefore = pChar->GetAttackTick();
                CGameWorld FutureWorld;
                FutureWorld.CopyWorld(&pGame->m_PredictedWorld);
                if(CCharacter *pFuture = FutureWorld.GetCharacterById(LocalId))
                {
                        CNetObj_PlayerInput FutureInput = Input;
                        pFuture->OnDirectInput(&FutureInput);
                        FutureWorld.m_GameTick = FutureWorld.GameTick() + 1;
                        pFuture->OnPredictedInput(&FutureInput);
                        FutureWorld.Tick();
                        if(CCharacter *pAfter = FutureWorld.GetCharacterById(LocalId))
                        {
                                if(pAfter->GetAttackTick() != tickBefore)
                                        willFire = true;
                        }
                }
        }

        // Active weapon (0-5)
        if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
        {
                const SWeaponSettings &s = m_AimBotWeapons[weapon];
                // AimMode: 0=silent, 1=trigger — both use willFire; 2=always
                bool trigger = (s.m_AimMode == 2) ? true : willFire;
                if(trigger)
                {
                        vec2 aimPt; int t = FindTarget(s, weapon, LocalId, MyPos, AimDir, AimPos, aimPt);
                        if(t >= 0)
                                ApplyAim(t, MyPos, s, aimPt);
                }
        }
        // Hook (6): Silent/Trigger use hook edge (hook has no reload cadence); Always = every tick
        if(m_AimBotWeapons[6].m_Enabled)
        {
                const SWeaponSettings &s = m_AimBotWeapons[6];
                bool trigger = (s.m_AimMode == 2) ? true : (hook && !m_PrevHook);
                if(trigger)
                {
                        vec2 aimPt; int t = FindTarget(s, 6, LocalId, MyPos, AimDir, AimPos, aimPt);
                        if(t >= 0)
                                ApplyAim(t, MyPos, s, aimPt);
                }
        }
        m_PrevFire = fire;
        m_PrevHook = hook;
}

void CAimBot::UpdateTriggerBot()
{
        CGameClient *pGame = GameClient();
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0)
                return;
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pChar)
                return;
        int weapon = pChar->GetActiveWeapon();
        if(weapon < 0 || weapon >= NUM_WEAPONS_TRIGGER)
                return;
        if(!m_TriggerBotWeapons[weapon].m_Enabled)
                return;

        const SWeaponSettings &s = m_TriggerBotWeapons[weapon];
        vec2 MyPos = pChar->Core()->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos);
        if(length(AimDir) < 0.001f)
                AimDir = vec2(1, 0);

        vec2 aimPtTb; int target = FindTarget(s, weapon, LocalId, MyPos, AimDir, AimPos, aimPtTb);
        bool targetInZone = (target >= 0);

        // Random latency: delay the trigger reaction by a random number of ticks.
        // Counter counts down while target is in zone; when it hits 0, trigger fires.
        if(targetInZone && s.m_RandomLatency > 0)
        {
                if(m_LatencyCounter <= 0)
                        m_LatencyCounter = (rand() % (s.m_RandomLatency + 1));
                if(m_LatencyCounter > 0)
                {
                        m_LatencyCounter--;
                        // While waiting, treat as "not yet triggered" — but still mark active
                        // so one_tick edge doesn't fire prematurely on the next tick.
                        m_TriggerBotWasActive = true;
                        return;
                }
        }

        CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
        int curTick = Client()->GameTick(g_Config.m_ClDummy);

        // one_tick: release the trigger on the tick AFTER it was pressed (exactly 1 tick hold).
        // Done BEFORE the targetInZone check so it runs regardless of current target state.
        if(m_TriggerBotReleaseTick >= 0 && curTick >= m_TriggerBotReleaseTick)
        {
                if(s.m_Trigger == 0)
                        pInput->m_Fire &= ~1;
                else
                        pInput->m_Hook = 0;
                m_TriggerBotReleaseTick = -1;
        }

        if(targetInZone)
        {
                // TriggerMode:
                // 0=one_tick  — press trigger for exactly 1 tick (only on edge: target just entered zone),
                //               then release on the next tick. Don't press again until target leaves and re-enters.
                // 1=hold      — hold trigger while target in zone, release when it leaves (good for guns)
                // 2=every_tick — press trigger every tick (spam edge) while target in zone (good for hammer)
                if(s.m_TriggerMode == 0) // one_tick
                {
                        if(!m_TriggerBotWasActive) // edge: target just entered zone
                        {
                                if(s.m_Trigger == 0)
                                        FireWeapon();
                                else
                                        DoHook();
                                m_TriggerBotReleaseTick = curTick + 1; // release on next tick
                        }
                }
                else if(s.m_TriggerMode == 1) // hold
                {
                        if(s.m_Trigger == 0)
                                pInput->m_Fire |= 1;
                        else
                                pInput->m_Hook = 1;
                }
                else // every_tick (2) — spam: force a new edge every tick
                {
                        if(s.m_Trigger == 0)
                                pInput->m_Fire = (pInput->m_Fire + 1) | 1; // increment + set bit = new edge
                        else
                                pInput->m_Hook = (pInput->m_Hook == 0) ? 1 : 0; // toggle for hook edge spam
                }
                m_TriggerBotWasActive = true;
        }
        else
        {
                // Target left zone — release for hold/every_tick modes.
                // one_tick already released via m_TriggerBotReleaseTick (no action needed here).
                if(s.m_TriggerMode != 0)
                {
                        // Only release if we were active (don't clobber player's own input).
                        if(m_TriggerBotWasActive)
                        {
                                if(s.m_Trigger == 0)
                                        pInput->m_Fire &= ~1;
                                else
                                        pInput->m_Hook = 0;
                        }
                }
                m_TriggerBotWasActive = false;
                m_LatencyCounter = 0;
        }
}

void CAimBot::RenderFov(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha)
{
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float radius = Settings.m_Radius;
        int LineSize = KxLineSize(KX_LINE_AIMBOT);
        float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
        bool useRay = Settings.m_UseRaycastShow;

        // Build line segments. Each segment is a pair (p0, p1).
        std::vector<std::pair<vec2, vec2>> segments;
        if(useRay)
        {
                // v1.56.92: Show only 2 edge lines (left + right) with wall collision.
                // The arc (radius circle) is drawn by RenderRadius with N raycast points.
                float leftAngle = myAngle - halfFovRad;
                float rightAngle = myAngle + halfFovRad;
                vec2 leftEnd = MyPos + vec2(cosf(leftAngle), sinf(leftAngle)) * radius;
                vec2 rightEnd = MyPos + vec2(cosf(rightAngle), sinf(rightAngle)) * radius;
                vec2 hitPos;
                if(GameClient()->Collision()->IntersectLineTeleHook(MyPos, leftEnd, &hitPos, nullptr) > 0)
                        segments.push_back({MyPos, hitPos});
                else
                        segments.push_back({MyPos, leftEnd});
                if(GameClient()->Collision()->IntersectLineTeleHook(MyPos, rightEnd, &hitPos, nullptr) > 0)
                        segments.push_back({MyPos, hitPos});
                else
                        segments.push_back({MyPos, rightEnd});
        }
        else
        {
                segments.push_back({MyPos, MyPos + vec2(cosf(myAngle - halfFovRad), sinf(myAngle - halfFovRad)) * radius});
                segments.push_back({MyPos, MyPos + vec2(cosf(myAngle + halfFovRad), sinf(myAngle + halfFovRad)) * radius});
        }

        // v1.56.104: Layer > 0 → defer to CKinetixLines (renders on top of fg blocks).
        if(g_Config.m_KxLineRenderingLayer > 0)
        {
                float hw = LineSize > 0 ? HalfWidth : 0.0f;
                for(size_t i = 0; i < segments.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        c.a = Alpha;
                        KinetixEnqueueLine(segments[i].first, segments[i].second, c, hw);
                }
                return;
        }

        Graphics()->TextureClear();
        if(LineSize > 0)
        {
                Graphics()->QuadsBegin();
                for(size_t i = 0; i < segments.size(); i++)
                {
                        vec2 p0 = segments[i].first;
                        vec2 p1 = segments[i].second;
                        vec2 dir = normalize(p1 - p0);
                        if(length(dir) < 0.001f)
                                continue;
                        vec2 perp = vec2(dir.y, -dir.x) * HalfWidth;
                        IGraphics::CFreeformItem q(
                                p0.x - perp.x, p0.y - perp.y, p0.x + perp.x, p0.y + perp.y,
                                p1.x - perp.x, p1.y - perp.y, p1.x + perp.x, p1.y + perp.y);
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&q, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                for(size_t i = 0; i < segments.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        IGraphics::CLineItem Line(segments[i].first.x, segments[i].first.y, segments[i].second.x, segments[i].second.y);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

void CAimBot::RenderRadius(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha)
{
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float radius = Settings.m_Radius;
        int LineSize = KxLineSize(KX_LINE_AIMBOT);
        float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
        bool useRay = Settings.m_UseRaycastShow;
        int segments = useRay ? std::max(1, std::min(64, Settings.m_RaycastAnglesShow)) : 48;

        std::vector<vec2> points;
        for(int i = 0; i <= segments; i++)
        {
                float t = (float)i / (float)segments;
                float a = myAngle - halfFovRad + t * (2.0f * halfFovRad);
                vec2 p = MyPos + vec2(cosf(a), sinf(a)) * radius;
                if(useRay)
                {
                        vec2 hit;
                        if(GameClient()->Collision()->IntersectLineTeleHook(MyPos, p, &hit, nullptr) > 0)
                                p = hit;
                }
                points.push_back(p);
        }

        // v1.56.104: Layer > 0 → defer to CKinetixLines (renders on top of fg blocks).
        if(g_Config.m_KxLineRenderingLayer > 0)
        {
                float hw = LineSize > 0 ? HalfWidth : 0.0f;
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        c.a = Alpha;
                        KinetixEnqueueLine(points[i], points[i + 1], c, hw);
                }
                return;
        }

        Graphics()->TextureClear();
        if(LineSize > 0)
        {
                Graphics()->QuadsBegin();
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        vec2 p0 = points[i];
                        vec2 p1 = points[i + 1];
                        vec2 dir = normalize(p1 - p0);
                        if(length(dir) < 0.001f)
                                continue;
                        vec2 perp = vec2(dir.y, -dir.x) * HalfWidth;
                        IGraphics::CFreeformItem q(
                                p0.x - perp.x, p0.y - perp.y, p0.x + perp.x, p0.y + perp.y,
                                p1.x - perp.x, p1.y - perp.y, p1.x + perp.x, p1.y + perp.y);
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&q, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        IGraphics::CLineItem Line(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

// v1.56.196: ComputePredictedAimPoint removed. Real predict now lives inline
// in FindTarget — one CopyWorld + pre-roll of MAX_PREDICT_TICKS ticks builds a
// positions[MAX_CLIENTS][MAX_PREDICT_TICKS+1] table, then linear tick-scan with
// sub-tick interpolation (linear interp of flightTime between adjacent ticks).

void CAimBot::ConAimBot(IConsole::IResult *pResult, void *pUserData)
{
        (void)pUserData;
        g_Config.m_KxAimBot = pResult->GetInteger(0) != 0;
}

void CAimBot::ConTriggerBot(IConsole::IResult *pResult, void *pUserData)
{
        (void)pUserData;
        g_Config.m_KxTriggerBot = pResult->GetInteger(0) != 0;
}
