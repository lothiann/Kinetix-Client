#include <game/client/components/kinetix/aimbot/aimbot.h>

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>
#include <base/vmath.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <game/client/gameclient.h>
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
        Console()->Register("kx_aimbot", "i[0/1]", CFGFLAG_CLIENT, ConAimBot, this, "AimBot: 1=on 0=off");
        Console()->Register("kx_triggerbot", "i[0/1]", CFGFLAG_CLIENT, ConTriggerBot, this, "TriggerBot: 1=on 0=off");
}

void CAimBot::OnReset()
{
        m_AimBotEnabled = false;
        m_TriggerBotEnabled = false;
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
        if(m_AimBotEnabled)
                UpdateAimBot();
        if(m_TriggerBotEnabled)
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

        float alpha = (float)g_Config.m_KxLineRenderingAlpha / 100.0f;

        if(m_AimBotEnabled)
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
        if(m_TriggerBotEnabled && weapon >= 0 && weapon < NUM_WEAPONS_TRIGGER && m_TriggerBotWeapons[weapon].m_Enabled)
        {
                if(m_TriggerBotShowFov)
                        RenderFov(m_TriggerBotWeapons[weapon], MyPos, AimDir, alpha * 0.7f);
                if(m_TriggerBotShowRadius)
                        RenderRadius(m_TriggerBotWeapons[weapon], MyPos, AimDir, alpha * 0.7f);
        }
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

int CAimBot::FindTarget(const SWeaponSettings &Settings, int LocalId, const vec2 &MyPos, const vec2 &AimDir, const vec2 &AimPos)
{
        // Hitbox-aware target search (variant A): target is in FOV+Radius if ANY point of its
        // 28×28 square hitbox (CCharacterCore::PhysicalSize = 28.0f, half = 14.0f) falls inside
        // the FOV cone AND within Radius. We sample 5 points: center + 4 corners of the hitbox.
        // If any sample passes FOV+Radius+raycast, the target qualifies. Score uses the best
        // (smallest) score among passing samples so the chosen target is the closest one.
        CGameClient *pGame = GameClient();
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        constexpr float HB_HALF = CCharacterCore::PhysicalSize() * 0.5f; // 14.0f
        int bestTarget = -1;
        float bestScore = 1e18f;
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                if(!PassesFilters(i, LocalId, Settings))
                        continue;
                vec2 targetCenter = vec2((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);

                // Sample 5 points of the hitbox: center + 4 corners.
                vec2 hbPoints[5] = {
                        targetCenter,
                        targetCenter + vec2(-HB_HALF, -HB_HALF),
                        targetCenter + vec2(HB_HALF, -HB_HALF),
                        targetCenter + vec2(-HB_HALF, HB_HALF),
                        targetCenter + vec2(HB_HALF, HB_HALF),
                };

                // Find the best (smallest score) sample that passes FOV + Radius + raycast.
                bool anyPass = false;
                float bestSampleScore = 1e18f;
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
                        if(Settings.m_UseRaycast && !IsVisible(MyPos, sp))
                                continue;
                        float score;
                        if(Settings.m_Priority == 0)
                                score = angleDiff; // nearest FOV: smallest angle from aim direction
                        else if(Settings.m_Priority == 1)
                                score = length(sp - AimPos); // nearest aim: closest to crosshair coordinates
                        else
                                score = dist; // nearest to me: closest to my position
                        if(score < bestSampleScore)
                        {
                                bestSampleScore = score;
                                anyPass = true;
                        }
                }
                if(anyPass && bestSampleScore < bestScore)
                {
                        bestScore = bestSampleScore;
                        bestTarget = i;
                }
        }
        return bestTarget;
}

bool CAimBot::IsVisible(const vec2 &From, const vec2 &To)
{
        return GameClient()->Collision()->IntersectLine(From, To, nullptr, nullptr) == 0;
}

void CAimBot::ApplyAim(int ClientId, const vec2 &MyPos, const SWeaponSettings &Settings)
{
        CGameClient *pGame = GameClient();
        vec2 targetPos = vec2((float)pGame->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
        vec2 aimOffset = targetPos - MyPos;
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
                        int t = FindTarget(s, LocalId, MyPos, AimDir, AimPos);
                        if(t >= 0)
                                ApplyAim(t, MyPos, s);
                }
        }
        // Hook (6): Silent/Trigger use hook edge (hook has no reload cadence); Always = every tick
        if(m_AimBotWeapons[6].m_Enabled)
        {
                const SWeaponSettings &s = m_AimBotWeapons[6];
                bool trigger = (s.m_AimMode == 2) ? true : (hook && !m_PrevHook);
                if(trigger)
                {
                        int t = FindTarget(s, LocalId, MyPos, AimDir, AimPos);
                        if(t >= 0)
                                ApplyAim(t, MyPos, s);
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

        int target = FindTarget(s, LocalId, MyPos, AimDir, AimPos);
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
        ColorRGBA col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_KxLineRenderingColor, true));
        int LineSize = g_Config.m_KxLineRenderingSize;
        float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
        bool useRay = m_AimBotUseRaycastShow || m_TriggerBotUseRaycastShow;

        // Build line segments. Each segment is a pair (p0, p1).
        std::vector<std::pair<vec2, vec2>> segments;
        if(useRay)
        {
                int angles = std::max(1, std::min(64, Settings.m_RaycastAngles));
                float leftAngle = myAngle - halfFovRad;
                float rightAngle = myAngle + halfFovRad;
                for(int i = 0; i <= angles; i++)
                {
                        float t = (float)i / (float)angles;
                        float a = leftAngle + t * (rightAngle - leftAngle);
                        vec2 end = MyPos + vec2(cosf(a), sinf(a)) * radius;
                        vec2 hitPos;
                        if(GameClient()->Collision()->IntersectLine(MyPos, end, &hitPos, nullptr) > 0)
                                segments.push_back({MyPos, hitPos});
                        else
                                segments.push_back({MyPos, end});
                }
        }
        else
        {
                segments.push_back({MyPos, MyPos + vec2(cosf(myAngle - halfFovRad), sinf(myAngle - halfFovRad)) * radius});
                segments.push_back({MyPos, MyPos + vec2(cosf(myAngle + halfFovRad), sinf(myAngle + halfFovRad)) * radius});
        }

        Graphics()->TextureClear();
        if(LineSize > 0)
        {
                Graphics()->QuadsBegin();
                for(const auto &seg : segments)
                {
                        vec2 p0 = seg.first;
                        vec2 p1 = seg.second;
                        vec2 dir = normalize(p1 - p0);
                        if(length(dir) < 0.001f)
                                continue;
                        vec2 perp = vec2(dir.y, -dir.x) * HalfWidth;
                        IGraphics::CFreeformItem q(
                                p0.x - perp.x, p0.y - perp.y, p0.x + perp.x, p0.y + perp.y,
                                p1.x - perp.x, p1.y - perp.y, p1.x + perp.x, p1.y + perp.y);
                        Graphics()->SetColor(col.r, col.g, col.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&q, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                Graphics()->SetColor(col.r, col.g, col.b, Alpha);
                for(const auto &seg : segments)
                {
                        IGraphics::CLineItem Line(seg.first.x, seg.first.y, seg.second.x, seg.second.y);
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
        ColorRGBA col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_KxLineRenderingColor, true));
        int LineSize = g_Config.m_KxLineRenderingSize;
        float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
        bool useRay = m_AimBotUseRaycastShow || m_TriggerBotUseRaycastShow;
        int segments = 48;

        std::vector<vec2> points;
        for(int i = 0; i <= segments; i++)
        {
                float t = (float)i / (float)segments;
                float a = myAngle - halfFovRad + t * (2.0f * halfFovRad);
                vec2 p = MyPos + vec2(cosf(a), sinf(a)) * radius;
                if(useRay)
                {
                        vec2 hit;
                        if(GameClient()->Collision()->IntersectLine(MyPos, p, &hit, nullptr) > 0)
                                p = hit;
                }
                points.push_back(p);
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
                        Graphics()->SetColor(col.r, col.g, col.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&q, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                Graphics()->SetColor(col.r, col.g, col.b, Alpha);
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        IGraphics::CLineItem Line(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

void CAimBot::ConAimBot(IConsole::IResult *pResult, void *pUserData)
{
        static_cast<CAimBot *>(pUserData)->m_AimBotEnabled = pResult->GetInteger(0) != 0;
}

void CAimBot::ConTriggerBot(IConsole::IResult *pResult, void *pUserData)
{
        static_cast<CAimBot *>(pUserData)->m_TriggerBotEnabled = pResult->GetInteger(0) != 0;
}
