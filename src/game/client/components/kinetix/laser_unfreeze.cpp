#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <base/vmath.h>
#include <base/math.h>
#include <base/time.h>

#include <algorithm>
#include <climits>
#include <vector>

static void KxLaserTick(CGameWorld &World, int ClientId, const CNetObj_PlayerInput &Input)
{
    if(CCharacter *pLocal = World.GetCharacterById(ClientId))
    {
        pLocal->OnDirectInput(&Input);
        ++World.m_GameTick;
        pLocal->OnPredictedInput(&Input);
        World.Tick();
    }
}

static void KxLaserRemoveLasers(CGameWorld &World)
{
    for(CEntity *pEntity = World.FindFirst(CGameWorld::ENTTYPE_LASER); pEntity;)
    {
        CEntity *pNext = pEntity->TypeNext();
        delete pEntity;
        pEntity = pNext;
    }
}

static bool KxLaserFrozen(const CCharacter *pCharacter)
{
    return !pCharacter || pCharacter->m_FreezeTime > 0 || pCharacter->Core()->m_DeepFrozen || pCharacter->Core()->m_IsInFreeze;
}

struct SKxBeamResult
{
    bool m_Found = false;
    vec2 m_Aim = vec2(0, 0);
    int m_Bounces = 0;
    std::vector<vec2> m_vPath;
};

// Simulates a laser fired in the given direction: the beam is followed until it
// hits the local tee (the only character left in the copied world) and the
// freeze clears. The bounce count of that hit is returned.
static SKxBeamResult KxProbeBeam(CGameWorld &Source, int ClientId, CNetObj_PlayerInput Input, int Ticks)
{
    SKxBeamResult Result;
    CGameWorld World;
    World.CopyWorld(&Source);
    World.m_WorldConfig.m_PredictWeapons = true;
    World.m_WorldConfig.m_InfiniteAmmo = true;
    if(!World.GetCharacterById(ClientId))
        return Result;
    for(int Id = 0; Id < MAX_CLIENTS; ++Id)
    {
        if(Id == ClientId)
            continue;
        if(CCharacter *pChar = World.GetCharacterById(Id))
            delete pChar;
    }
    KxLaserRemoveLasers(World);

    Input.m_Fire = 1;
    int LastBounce = -1;
    for(int Step = 0; Step <= Ticks; ++Step)
    {
        KxLaserTick(World, ClientId, Input);
        CCharacter *pLocal = World.GetCharacterById(ClientId);
        if(!pLocal)
            return Result;
        for(CEntity *pEntity = World.FindFirst(CGameWorld::ENTTYPE_LASER); pEntity; pEntity = pEntity->TypeNext())
        {
            CLaser *pLaser = static_cast<CLaser *>(pEntity);
            if(pLaser->GetOwner() != ClientId)
                continue;
            Result.m_vPath.push_back(pLaser->GetPos());
            if(pLaser->GetEnergy() < 0.0f)
            {
                // A laser only dies when it hits a character; every other
                // character was removed above, so this is a self hit.
                Input.m_Fire = 0;
                if(LastBounce < 0)
                    LastBounce = pLaser->GetBounces();
            }
        }
        if(LastBounce >= 0 && !KxLaserFrozen(pLocal))
        {
            Result.m_Found = true;
            Result.m_Bounces = LastBounce;
            return Result;
        }
    }
    return Result;
}

static SKxBeamResult KxSearchUnfreeze(CGameWorld &Source, int ClientId, const CNetObj_PlayerInput &Input,
    vec2 Mouse, int Ticks, int Points, float Fov, bool MostBounces, float MouseRadius)
{
    SKxBeamResult Best;
    CCharacter *pLocal = Source.GetCharacterById(ClientId);
    if(!pLocal || Points <= 0 || Fov <= 0)
        return Best;

    struct SCandidate
    {
        vec2 m_Aim;
        int m_Bounces;
    };
    std::vector<SCandidate> vCandidates;
    const float Span = Fov < 315.0f ? Fov * pi / 180.0f : 2.0f * pi;
    const float Center = angle(Mouse);
    for(int Index = 0; Index < Points; ++Index)
    {
        const float Radians = Index * (Span / std::max(1, Points - 1)) + (Center - Span * 0.5f);
        const vec2 Direction = direction(Radians);
        CNetObj_PlayerInput Candidate = Input;
        Candidate.m_TargetX = (int)(Direction.x * MouseRadius);
        Candidate.m_TargetY = (int)(Direction.y * MouseRadius);
        const SKxBeamResult Probe = KxProbeBeam(Source, ClientId, Candidate, Ticks);
        if(Probe.m_Found)
            vCandidates.push_back({Direction * MouseRadius, Probe.m_Bounces});
    }
    if(vCandidates.empty())
        return Best;
    std::sort(vCandidates.begin(), vCandidates.end(), [&](const SCandidate &A, const SCandidate &B) {
        return MostBounces ? A.m_Bounces > B.m_Bounces : A.m_Bounces < B.m_Bounces;
    });
    Best.m_Found = true;
    Best.m_Aim = vCandidates.front().m_Aim;
    Best.m_Bounces = vCandidates.front().m_Bounces;
    return Best;
}

void CBotNet::UpdateLaserUnfreeze()
{
    static bool s_WasFiring = false;

    CGameClient *pGame = GameClient();
    const int Slot = g_Config.m_ClDummy;
    const int LocalId = pGame ? pGame->m_aLocalIds[Slot] : -1;
    CNetObj_PlayerInput *pInput = pGame ? &pGame->m_Controls.m_aInputData[Slot] : nullptr;

    auto ReleaseFire = [&]() {
        if(s_WasFiring && pInput && (pInput->m_Fire & 1))
            pInput->m_Fire = (pInput->m_Fire + 1) & ~1;
        s_WasFiring = false;
    };

    if(!g_Config.m_KxLaserUnfreeze || !pGame || !pGame->m_Snap.m_pLocalInfo || LocalId < 0 || LocalId >= MAX_CLIENTS)
    {
        ReleaseFire();
        return;
    }

    const bool Playing = (pInput->m_PlayerFlags & PLAYERFLAG_PLAYING) && !pGame->m_Snap.m_SpecInfo.m_Active &&
                         Client()->State() == IClient::STATE_ONLINE;
    if(!Playing || (pInput->m_Fire & 1))
    {
        s_WasFiring = false;
        return;
    }

    CGameWorld &Source = pGame->m_PredictedWorld;
    CCharacter *pLocal = Source.GetCharacterById(LocalId);
    if(!pLocal || KxLaserFrozen(pLocal) || !pLocal->GetWeaponGot(WEAPON_LASER))
    {
        ReleaseFire();
        return;
    }

    CNetObj_PlayerInput ProbeInput = *pInput;
    ProbeInput.m_TargetX = (int)pGame->m_Controls.m_aMousePos[Slot].x;
    ProbeInput.m_TargetY = (int)pGame->m_Controls.m_aMousePos[Slot].y;

    // Only bother when the tee is about to freeze within the next three ticks.
    {
        bool WillFreeze = false;
        CGameWorld LifeWorld;
        LifeWorld.CopyWorld(&Source);
        LifeWorld.m_WorldConfig.m_PredictWeapons = true;
        if(!LifeWorld.GetCharacterById(LocalId))
            return;
        for(int Step = 0; Step <= 3; ++Step)
        {
            KxLaserTick(LifeWorld, LocalId, ProbeInput);
            if(KxLaserFrozen(LifeWorld.GetCharacterById(LocalId)))
            {
                WillFreeze = Step <= 2;
                break;
            }
        }
        if(!WillFreeze)
        {
            ReleaseFire();
            return;
        }
    }

    // The weapon is prepared before the search, like the reference does; the
    // probe itself uses the already copied input.
    pInput->m_WantedWeapon = WEAPON_LASER + 1;

    const float MouseRadius = pGame->m_Controls.GetMaxMouseDistance();
    const vec2 Mouse = pGame->m_Controls.m_aMousePos[Slot];

    vec2 Aim;
    SKxBeamResult Result = KxProbeBeam(Source, LocalId, ProbeInput, g_Config.m_KxLaserUnfreezeCurDirTicks);
    if(Result.m_Found)
        Aim = vec2(ProbeInput.m_TargetX, ProbeInput.m_TargetY);
    else
    {
        Result = KxSearchUnfreeze(Source, LocalId, ProbeInput, Mouse, g_Config.m_KxLaserUnfreezeTicks,
            g_Config.m_KxLaserUnfreezeAngles, (float)g_Config.m_KxLaserUnfreezeFov,
            g_Config.m_KxLaserUnfreezeMostBounces != 0, MouseRadius);
        if(!Result.m_Found)
            return;
        Aim = Result.m_Aim;
    }

    if(g_Config.m_KxLaserUnfreezeSilent)
    {
        pGame->m_Controls.m_SilentAimActive = true;
        pGame->m_Controls.m_SilentAimOffset = Aim;
    }
    else
        SetMousePos(pGame, Slot, Aim);

    if(g_Config.m_KxLaserUnfreezeShowAttempt)
    {
        CNetObj_PlayerInput PathInput = ProbeInput;
        PathInput.m_TargetX = (int)Aim.x;
        PathInput.m_TargetY = (int)Aim.y;
        SKxBeamResult Path = KxProbeBeam(Source, LocalId, PathInput, g_Config.m_KxLaserUnfreezeTicks);
        if(Path.m_vPath.size() >= 2)
        {
            m_LaserUnfreezePath = std::move(Path.m_vPath);
            m_LaserUnfreezePathTime = time_get();
        }
    }

    if((pInput->m_Fire & 1) == 0)
        pInput->m_Fire = (pInput->m_Fire + 1) | 1;
    s_WasFiring = true;
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