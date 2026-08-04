#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <base/vmath.h>
#include <base/math.h>
#include <engine/keys.h>

// v1.56.151: Fly Ride
//
// Pilot = active dummy (g_Config.m_ClDummy). Dummy = nearest connected dummy.
// Anchor = pilot position at enable time; WASD moves it per-tick (1.0f / tick).
//
// Per-tick (50Hz):
//   W: anchor.y -= 1   (DDNet Y grows downward, so -= = up)
//   S: anchor.y += 1
//   A: anchor.x += 1   (note: inverted vs intuition — user spec)
//   D: anchor.x -= 1
//
// Per-frame (consumed by CControls::SnapInput):
//   X threshold: pilot.y < anchor.y → X=120 (up); pilot.y > anchor.y → X=80 (down); else X=65
//   direction: |pilot.x - anchor.x| <= 32 → 0; pilot.x < anchor.x → +1 (right); pilot.x > anchor.x → -1 (left)
//   auto fly (dist<=75): dummy hammer ON (aim at pilot)
//   hook+aim (dist<=X OR dist>275): pilot silent aim at dummy + hook=1
//   middle (X<dist<=275): hook=0, no aim
//
// Pilot direction/hook/jump override injected in CControls::SnapInput (block normal WASD).

void CBotNet::UpdateFlyRide()
{
        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        const bool enabled = g_Config.m_KxFlyRide != 0;

        // Init on enable / cleanup on disable.
        if(enabled && !m_FlyRideWasActive)
        {
                // Just turned on — init anchor to pilot position.
                m_FlyRideAnchor = pGame->m_PredictedChar.m_Pos;
                m_FlyRideWasActive = true;
                m_FlyRideLastAnchorTick = -1;
                m_FlyRideTargetDummy = -1;
        }
        else if(!enabled && m_FlyRideWasActive)
        {
                // Just turned off — release hammer on target dummy.
                if(m_FlyRideTargetDummy >= 0 && m_FlyRideTargetDummy < MAX_DUMMIES)
                {
                        CNetObj_PlayerInput *pDummy = &pGame->m_aDummyInput[m_FlyRideTargetDummy];
                        if(pDummy->m_Fire & 1)
                                pDummy->m_Fire = (pDummy->m_Fire + 1) & ~1; // release
                        pGame->m_Controls.m_aInputData[m_FlyRideTargetDummy] = *pDummy;
                }
                m_FlyRideWasActive = false;
                m_FlyRideTargetDummy = -1;
                m_FlyRidePilotDir = 0;
                m_FlyRidePilotHook = 0;
                return;
        }

        if(!enabled)
                return;

        // Need a local character (pilot) to fly.
        if(!pGame->m_Snap.m_pLocalInfo)
                return;

        const vec2 pilotPos = pGame->m_PredictedChar.m_Pos;

        // ── Per-tick anchor update (WASD) ───────────────────────────────
        // Gate by GameTick (50Hz / TPS) — matches user spec "в цикле (секунда/TPS (50))".
        // KeyIsPressed = held state, so holding WASD produces 1.0f movement per tick.
        const int curTick = Client()->GameTick(g_Config.m_ClDummy);
        if(m_FlyRideLastAnchorTick != curTick)
        {
                m_FlyRideLastAnchorTick = curTick;
                // Raw key state (bind-agnostic — works even if user remapped W/A/S/D).
                if(Input()->KeyIsPressed(KEY_W))
                        m_FlyRideAnchor.y -= 8.0f;
                if(Input()->KeyIsPressed(KEY_S))
                        m_FlyRideAnchor.y += 8.0f;
                if(Input()->KeyIsPressed(KEY_A))
                        m_FlyRideAnchor.x -= 8.0f; // A = left
                if(Input()->KeyIsPressed(KEY_D))
                        m_FlyRideAnchor.x += 8.0f; // D = right
        }


        // ── Find nearest dummy ─────────────────────────────────────────────
        int nearestD = -1;
        float nearestDistSq = 1e18f;
        vec2 nearestDummyPos = pilotPos;
        const int activeD = g_Config.m_ClDummy;

        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                if(D == activeD)
                        continue; // skip pilot
                if(D != 0 && !Client()->DummyConnected(D))
                        continue; // skip disconnected
                const int cid = pGame->m_aLocalIds[D];
                if(cid < 0 || cid >= 128)
                        continue;
                if(!pGame->m_aClients[cid].m_Active)
                        continue;

                const vec2 dPos = pGame->m_aClients[cid].m_Predicted.m_Pos;
                const float dsq = length_squared(dPos - pilotPos);
                if(dsq < nearestDistSq)
                {
                        nearestDistSq = dsq;
                        nearestD = D;
                        nearestDummyPos = dPos;
                }
        }

        if(nearestD < 0)
        {
                // No dummy to fly — reset outputs, keep anchor.
                m_FlyRidePilotDir = 0;
                m_FlyRidePilotHook = 0;
                m_FlyRideTargetDummy = -1;
                return;
        }

        m_FlyRideTargetDummy = nearestD;

        // ── Compute X threshold (vertical) ─────────────────────────────────
        // ±32px deadzone around anchor.y → X=120 (default/hover).
        float X = 100.0f;
        const float dy = pilotPos.y - m_FlyRideAnchor.y;
        if(std::abs(dy) <= 32.0f)
                X = 100.0f; // pilot within deadzone → hover
        else if(pilotPos.y < m_FlyRideAnchor.y)
                X = 125.0f; // pilot above anchor → "need up"
        else
                X = 55.0f; // pilot below anchor → "need down"

        // ── Compute direction (horizontal) ─────────────────────────────────
        int dir = 0;
        const float dx = pilotPos.x - m_FlyRideAnchor.x;
        if(std::abs(dx) <= 32.0f)
                dir = 0;
        else if(pilotPos.x < m_FlyRideAnchor.x)
                dir = 1; // pilot left of anchor → right
        else
                dir = -1; // pilot right of anchor → left

        m_FlyRidePilotDir = dir;

        // ── Set dummy direction (non-active dummy → via m_aDummyInput) ────
        SetDirection(pGame, nearestD, dir < 0, dir > 0);

        // ── Compute distance pilot↔dummy ───────────────────────────────────
        const float dist = std::sqrt(nearestDistSq);

        // ── Hammer/hook/aim logic (3 independent conditions) ──────────────
        // v1.56.153: auto fly zone (<=75px): pilot aim at dummy + dummy hammer.
        //            hook: separate threshold (X), independent of hammer.
        CNetObj_PlayerInput *pDummy = &pGame->m_aDummyInput[nearestD];

        // Auto fly zone: dummy within 75px → dummy hammer ON (only hammer, no pilot aim).
        if(dist <= 70.0f)
        {
                // Dummy: hammer on — switch to hammer, aim at pilot, fire.
                pDummy->m_WantedWeapon = WEAPON_HAMMER + 1;
                const vec2 aim = pilotPos - nearestDummyPos;
                pDummy->m_TargetX = (int)aim.x;
                pDummy->m_TargetY = (int)aim.y;
                pDummy->m_Fire = (pDummy->m_Fire + 1) | 1; // press (parity flip, bit0=1)
        }
        else
        {
                // Outside auto fly zone — release hammer if was pressed.
                if(pDummy->m_Fire & 1)
                        pDummy->m_Fire = (pDummy->m_Fire + 1) & ~1; // release
        }

        // Hook + aim: dist >= X → pilot aim at dummy + hook on. Else hook off.
        if(dist >= X)
        {
                // Pilot: silent aim at dummy (reuses laser-unfreeze channel — highest priority).
                pGame->m_Controls.m_LaserUnfreezeAimActive = true;
                pGame->m_Controls.m_LaserUnfreezeAimOffset = nearestDummyPos - pilotPos;
                m_FlyRidePilotHook = 1;
        }
        else
        {
                m_FlyRidePilotHook = 0;
        }

        pDummy->m_Direction = dir;
        pGame->m_Controls.m_aInputData[nearestD] = *pDummy; // sync for prediction
}

// v1.56.159: Render red semi-transparent tile at anchor position (debug visual).
// Tile size = 32px (one DDNet tile). Alpha = 0.5.
void CBotNet::RenderFlyRideAnchor()
{
        if(!g_Config.m_KxFlyRide)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        // Center the tile on anchor.
        const vec2 p = m_FlyRideAnchor;
        const float s = 16.0f; // half-tile (32px / 2)

        pGame->Graphics()->TextureClear();
        pGame->Graphics()->BlendNormal();
        pGame->Graphics()->QuadsBegin();
        pGame->Graphics()->SetColor(1.0f, 0.0f, 0.0f, 0.5f); // red, 50% alpha
        IGraphics::CFreeformItem Quad(
                p.x - s, p.y - s, p.x + s, p.y - s,
                p.x - s, p.y + s, p.x + s, p.y + s);
        pGame->Graphics()->QuadsDrawFreeform(&Quad, 1);
        pGame->Graphics()->QuadsEnd();
}
