#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <base/vmath.h>
#include <base/math.h>
#include <base/time.h>
#include <climits>

// Laser unfreeze — predictive laser self-unfreeze.
//
// PHASE 1 (trigger): predict next `trigger_ticks` ticks without laser.
//   If character becomes frozen → activate raycast phase.
//   If character is ALREADY frozen → skip entirely (don't fire).
//
// PHASE 2 (raycast): for each angle in FOV/Angles:
//   - Create a fresh FutureWorld copy
//   - Spawn a CLaser at current pos, aimed at the test angle
//   - Predict `ticks` ticks, tracking ALL bounce positions
//   - Score: tick count when character unfreezes (lower = better)
//   - If never unfreezes → score = INT_MAX (skip)
//
// PHASE 3 (execute): if best candidate found (score < INT_MAX):
//   - Switch to laser weapon (m_WantedWeapon = 5)
//   - Set aim to FIRST BOUNCE POSITION (world coords → screen offset)
//   - Silent mode: set m_LaserUnfreezeAimActive instead of SetMousePos
//   - Show attempt: store full laser path for rendering
//   - Fire (m_Fire parity press)

void CBotNet::UpdateLaserUnfreeze()
{
    static bool s_WasFiring = false;
    // Throttle: Phase 1 (trigger prediction) is expensive (full CopyWorld).
    // Run it only every Nth tick instead of every tick. Predicting 1-5 ticks
    // ahead every 5 ticks still catches freeze well before it happens.
    // This cuts the per-tick CopyWorld cost by ~5x and avoids the previous
    // 50x/sec full-world-clone that caused the FPS collapse.
    static int s_ThrottleCounter = 0;

    if(!g_Config.m_KxLaserUnfreeze)
    {
        if(s_WasFiring)
        {
            CGameClient *pGame = GameClient();
            if(pGame)
            {
                CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
                if((pInput->m_Fire & 1) != 0)
                    pInput->m_Fire = (pInput->m_Fire + 1) & ~1;
            }
            s_WasFiring = false;
        }
        s_ThrottleCounter = 0;
        return;
    }

    // Throttle Phase 1: only run trigger prediction every 5 ticks.
    // On off-ticks we keep the last willFreeze decision (held in static).
    // Phase 2 (raycast) only runs when willFreeze is true, so it is naturally
    // rare and does not need its own throttle.
    constexpr int THROTTLE_PHASE1 = 5;
    static bool s_HeldWillFreeze = false;

    s_ThrottleCounter++;
    if(s_ThrottleCounter < THROTTLE_PHASE1)
    {
        // Reuse last decision on off-ticks. Still must release fire if we
        // were firing but the held decision flipped to false.
        if(!s_HeldWillFreeze && s_WasFiring)
        {
            CGameClient *pGame = GameClient();
            if(pGame)
            {
                CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
                if((pInput->m_Fire & 1) != 0)
                    pInput->m_Fire = (pInput->m_Fire + 1) & ~1;
            }
            s_WasFiring = false;
        }
        // If held willFreeze is true, fall through to Phase 2 on every tick
        // (cheap relative to Phase 1, and only fires while about to freeze).
        if(!s_HeldWillFreeze)
            return;
    }
    else
    {
        s_ThrottleCounter = 0; // reset for next window
        // fall through to run Phase 1 this tick
    }

    CGameClient *pGame = GameClient();
    if(!pGame || !pGame->m_Snap.m_pLocalInfo)
        return;

    int LocalID = pGame->m_Snap.m_LocalClientId;
    if(LocalID < 0)
        return;

    CCharacter *pPredChar = pGame->m_PredictedWorld.GetCharacterById(LocalID);
    if(!pPredChar)
        return;

    // ── SKIP if already frozen ───────────────────────────────────
    if(pPredChar->m_FreezeTime > 0 || pPredChar->Core()->m_DeepFrozen)
    {
        if(s_WasFiring)
        {
            CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
            if((pInput->m_Fire & 1) != 0)
                pInput->m_Fire = (pInput->m_Fire + 1) & ~1;
            s_WasFiring = false;
        }
        s_HeldWillFreeze = false;
        return;
    }

    // ── PHASE 1: trigger prediction (throttled — runs every 5 ticks) ─
    bool willFreeze = s_HeldWillFreeze; // keep last if skipping Phase 1
    if(s_ThrottleCounter == 0) // we just reset it above => this is a Phase 1 tick
    {
        int triggerTicks = g_Config.m_KxLaserUnfreezeTriggerTicks;
        if(triggerTicks < 1) triggerTicks = 1;
        if(triggerTicks > 5) triggerTicks = 5;

        willFreeze = false;

        {
            CGameWorld TriggerWorld;
            TriggerWorld.CopyWorld(&pGame->m_PredictedWorld);

            // IMPORTANT: use `delete` (not RemoveEntity) for characters we
            // don't want. CopyWorld allocates them with `new`, but
            // RemoveEntity only unlinks from the list — it does NOT free the
            // object. Using RemoveEntity here leaked up to MAX_CLIENTS-1
            // CCharacter objects every tick (thousands/sec), which was the
            // root cause of the FPS collapse. `delete` runs the destructor
            // which itself calls RemoveEntity, then frees the memory.
            for(int i = 0; i < MAX_CLIENTS; i++)
            {
                if(i == LocalID)
                    continue;
                if(CCharacter *pChar = TriggerWorld.GetCharacterById(i))
                    delete pChar;
            }

            CCharacter *pTriggerChar = TriggerWorld.GetCharacterById(LocalID);
            if(!pTriggerChar)
                return;

            CNetObj_PlayerInput TriggerInput = pTriggerChar->Core()->m_Input;
            int StartTick = TriggerWorld.GameTick();

            for(int t = 0; t < triggerTicks; t++)
            {
                pTriggerChar->OnDirectInput(&TriggerInput);
                TriggerWorld.m_GameTick = StartTick + t + 1;
                pTriggerChar->OnPredictedInput(&TriggerInput);
                TriggerWorld.Tick();

                if(!TriggerWorld.GetCharacterById(LocalID))
                    break;

                if(pTriggerChar->m_FreezeTime > 0)
                {
                    willFreeze = true;
                    break;
                }
            }
        }
        s_HeldWillFreeze = willFreeze;
    }

    if(!willFreeze)
    {
        if(s_WasFiring)
        {
            CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
            if((pInput->m_Fire & 1) != 0)
                pInput->m_Fire = (pInput->m_Fire + 1) & ~1;
            s_WasFiring = false;
        }
        return;
    }

    // ── PHASE 2: raycast — find best angle ───────────────────────
    int fov = g_Config.m_KxLaserUnfreezeFov;
    int numAngles = g_Config.m_KxLaserUnfreezeAngles;
    int simTicks = g_Config.m_KxLaserUnfreezeTicks;

    if(fov < 5) fov = 5;
    if(fov > 360) fov = 360;
    if(numAngles < 1) numAngles = 1;
    if(numAngles > 144) numAngles = 144;
    if(simTicks < 1) simTicks = 1;
    if(simTicks > 50) simTicks = 50;

    vec2 curAim = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
    float curAngle = 0.0f;
    if(length(curAim) > 0.001f)
        curAngle = atan2f(curAim.y, curAim.x);

    float laserReach = pPredChar->Core()->m_Tuning.m_LaserReach;
    if(laserReach <= 0.0f)
        laserReach = 800.0f;

    vec2 myPos = pPredChar->m_Pos;

    float halfFovRad = (fov * 0.5f) * (pi / 180.0f);
    float angleStep = (numAngles > 1) ? (2.0f * halfFovRad) / (numAngles - 1) : 0.0f;
    float startAngle = curAngle - halfFovRad;

    int bestScore = INT_MAX;
    vec2 bestBouncePos = vec2(0, 0);
    std::vector<vec2> bestPath;  // Full laser path for Show attempt
    bool foundCandidate = false;

    for(int a = 0; a < numAngles; a++)
    {
        float testAngle = startAngle + a * angleStep;
        vec2 laserDir = vec2(cosf(testAngle), sinf(testAngle));

        CGameWorld RayWorld;
        RayWorld.CopyWorld(&pGame->m_PredictedWorld);

        // `delete` (not RemoveEntity) — see Phase 1 comment above.
        // RemoveEntity only unlinks and leaks the new'd object.
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
            if(i == LocalID)
                continue;
            if(CCharacter *pChar = RayWorld.GetCharacterById(i))
                delete pChar;
        }

        CCharacter *pRayChar = RayWorld.GetCharacterById(LocalID);
        if(!pRayChar)
            continue;

        CLaser *pLaser = new CLaser(&RayWorld, myPos, laserDir, laserReach, LocalID, WEAPON_LASER);

        CNetObj_PlayerInput RayInput = pRayChar->Core()->m_Input;
        int StartTick = RayWorld.GameTick();
        int score = INT_MAX;

        // Track full laser path for Show attempt
        std::vector<vec2> laserPath;
        laserPath.push_back(myPos);
        vec2 prevFrom = myPos;
        bool gotBounce = false;

        for(int t = 0; t < simTicks; t++)
        {
            pRayChar->OnDirectInput(&RayInput);
            RayWorld.m_GameTick = StartTick + t + 1;
            pRayChar->OnPredictedInput(&RayInput);
            RayWorld.Tick();

            // Track bounce positions
            if(pLaser)
            {
                vec2 laserFrom = pLaser->GetFrom();
                if(distance(laserFrom, prevFrom) > 1.0f)
                {
                    laserPath.push_back(laserFrom);
                    prevFrom = laserFrom;
                    gotBounce = true;
                }
            }

            if(!RayWorld.GetCharacterById(LocalID))
                break;

            if(pRayChar->m_FreezeTime == 0 && !pRayChar->Core()->m_DeepFrozen)
            {
                score = t + 1;
                // Add final laser position to path
                if(pLaser)
                    laserPath.push_back(pLaser->GetPos());
                break;
            }
        }

        if(score < bestScore && gotBounce && laserPath.size() >= 2)
        {
            bestScore = score;
            bestBouncePos = laserPath[1];  // First bounce position
            bestPath = laserPath;
            foundCandidate = true;
        }
    }

    if(!foundCandidate)
        return;

    // ── PHASE 3: execute ─────────────────────────────────────────
    vec2 aimOffset = bestBouncePos - myPos;

    if(g_Config.m_KxLaserUnfreezeSilent)
    {
        // Silent mode: don't change m_aMousePos (visible crosshair).
        // Set the aim override flag — SnapInput will use this offset
        // for m_TargetX/Y right before sending to server.
        pGame->m_Controls.m_LaserUnfreezeAimActive = true;
        pGame->m_Controls.m_LaserUnfreezeAimOffset = aimOffset;
    }
    else
    {
        // Normal mode: set both m_aMousePos and m_TargetX/Y
        // TODO: remove ghost when show for me — visible crosshair moves to
        // laser bounce aim, but for a few frames the player's real angle
        // flickers through. Same issue as fake aim showForMe before
        // v1.56.135 sync.
        SetMousePos(pGame, g_Config.m_ClDummy, aimOffset);
    }

    CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];

    if(g_Config.m_KxLaserUnfreezeAuto)
        pInput->m_WantedWeapon = WEAPON_LASER + 1;

    if((pInput->m_Fire & 1) == 0)
        pInput->m_Fire = (pInput->m_Fire + 1) | 1;
    s_WasFiring = true;

    // Store path for Show attempt rendering
    if(g_Config.m_KxLaserUnfreezeShowAttempt && bestPath.size() >= 2)
    {
        m_LaserUnfreezePath = bestPath;
        m_LaserUnfreezePathTime = time_get();
    }
}

// ── Render laser unfreeze path (Show attempt) ───────────────────
// Draws the successful laser path with 5-second linear fade.
// Uses trajectory line size + laser rifle inner color from settings.
void CBotNet::RenderLaserUnfreezePath()
{
    if(!g_Config.m_KxLaserUnfreezeShowAttempt)
        return;
    if(m_LaserUnfreezePath.empty())
        return;

    // 5-second fade
    constexpr float FADE_DURATION = 5.0f;
    float elapsed = (float)(time_get() - m_LaserUnfreezePathTime) / (float)time_freq();
    if(elapsed >= FADE_DURATION)
    {
        m_LaserUnfreezePath.clear();
        return;
    }

    float fadeAlpha = 1.0f - (elapsed / FADE_DURATION);

    CGameClient *pGame = GameClient();
    if(!pGame)
        return;

    // Color: laser rifle inner color (same as in-game laser)
    ColorRGBA BaseColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClLaserRifleInnerColor, true));
    // Alpha: trajectory alpha setting * fade
    float ConfigAlpha = KxLineAlpha(KX_LINE_LASER_UNFREEZE) * fadeAlpha; // v1.56.108: per-component
    // Line size: trajectory line size setting
    int LineSize = KxLineSize(KX_LINE_LASER_UNFREEZE);

    // v1.56.210: per-segment gradient color (only when Rainbow + Gradient are both ON).
    // Falls back to the laser rifle inner color otherwise (so the laser beam still
    // matches in-game laser rendering when gradient is off).
    bool LaserGradientActive =
        g_Config.m_KxLineLaserUnfreezeRainbow != 0 && g_Config.m_KxLineLaserUnfreezeGradient != 0;
    auto segCol = [&](int segIdx) -> ColorRGBA {
        if(LaserGradientActive)
        {
            ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_LASER_UNFREEZE, segIdx), true);
            c.a = ConfigAlpha;
            return c;
        }
        return ColorRGBA(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha);
    };

    pGame->Graphics()->TextureClear();
    pGame->Graphics()->BlendNormal();

    if(LineSize > 0)
    {
        // Thick line rendering using quads (same as RenderTrajectory)
        std::vector<IGraphics::CFreeformItem> vQuads;
        vQuads.reserve(m_LaserUnfreezePath.size() - 1);
        float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;

        for(size_t i = 1; i < m_LaserUnfreezePath.size(); i++)
        {
            vec2 p0 = m_LaserUnfreezePath[i - 1];
            vec2 p1 = m_LaserUnfreezePath[i];
            vec2 Dir = normalize(p1 - p0);
            vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;

            vQuads.emplace_back(
                p0.x - Perp.x, p0.y - Perp.y,
                p0.x + Perp.x, p0.y + Perp.y,
                p1.x - Perp.x, p1.y - Perp.y,
                p1.x + Perp.x, p1.y + Perp.y);
        }

        pGame->Graphics()->QuadsBegin();
        for(size_t i = 0; i < vQuads.size(); i++)
        {
            ColorRGBA c = segCol((int)i);
            pGame->Graphics()->SetColor(c.r, c.g, c.b, c.a);
            pGame->Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
        }
        pGame->Graphics()->QuadsEnd();
    }
    else
    {
        // Thin line rendering (LineSize == 0)
        pGame->Graphics()->LinesBegin();

        for(size_t i = 1; i < m_LaserUnfreezePath.size(); i++)
        {
            ColorRGBA c = segCol((int)(i - 1));
            pGame->Graphics()->SetColor(c.r, c.g, c.b, c.a);
            IGraphics::CLineItem Line(
                m_LaserUnfreezePath[i - 1].x, m_LaserUnfreezePath[i - 1].y,
                m_LaserUnfreezePath[i].x, m_LaserUnfreezePath[i].y);
            pGame->Graphics()->LinesDraw(&Line, 1);
        }

        pGame->Graphics()->LinesEnd();
    }
}
