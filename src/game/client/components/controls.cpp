/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/vmath.h>
#include <cstdlib>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>

// bot_control integration
#include <game/client/components/kinetix/basic_avoid_freeze.h>
#include <game/client/components/kinetix/bot_control.h>
#include <game/client/components/kinetix/kinetix.h>

CControls::CControls()
{
        mem_zero(&m_aLastData, sizeof(m_aLastData));
        std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
        std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
        std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
        std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
        for(int D = 0; D < MAX_DUMMIES; D++)
                ResetInput(D);

        for(int &AmmoCount : m_aAmmoCount)
                AmmoCount = 0;

        m_LastSendTime = 0;
}

void CControls::ResetInput(int Dummy)
{
        m_aLastData[Dummy].m_Direction = 0;
        // simulate releasing the fire button
        if((m_aLastData[Dummy].m_Fire & 1) != 0)
                m_aLastData[Dummy].m_Fire++;
        m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
        m_aLastData[Dummy].m_Jump = 0;
        m_aInputData[Dummy] = m_aLastData[Dummy];

        m_aInputDirectionLeft[Dummy] = 0;
        m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
        for(int &AmmoCount : m_aAmmoCount)
                AmmoCount = 0;
}

struct CInputState
{
        CControls *m_pControls;
        int *m_apVariables[MAX_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
        CInputState *pState = (CInputState *)pUserData;

        if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
                return;

        *pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
        CInputState *pState = (CInputState *)pUserData;

        if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
                return;

        int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
        if(((*pVariable) & 1) != pResult->GetInteger(0))
                (*pVariable)++;
        *pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
        CControls *m_pControls;
        int *m_apVariables[MAX_DUMMIES];
        int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
        CInputSet *pSet = (CInputSet *)pUserData;
        if(pResult->GetInteger(0))
        {
                *pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
        }
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
        CInputSet *pSet = (CInputSet *)pUserData;
        ConKeyInputCounter(pResult, pSet);
        pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
        // game commands
        {
                static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1], &m_aInputDirectionLeft[2], &m_aInputDirectionLeft[3], &m_aInputDirectionLeft[4], &m_aInputDirectionLeft[5], &m_aInputDirectionLeft[6], &m_aInputDirectionLeft[7]}};
                Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
        }
        {
                static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1], &m_aInputDirectionRight[2], &m_aInputDirectionRight[3], &m_aInputDirectionRight[4], &m_aInputDirectionRight[5], &m_aInputDirectionRight[6], &m_aInputDirectionRight[7]}};
                Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump, &m_aInputData[2].m_Jump, &m_aInputData[3].m_Jump, &m_aInputData[4].m_Jump, &m_aInputData[5].m_Jump, &m_aInputData[6].m_Jump, &m_aInputData[7].m_Jump}};
                Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook, &m_aInputData[2].m_Hook, &m_aInputData[3].m_Hook, &m_aInputData[4].m_Hook, &m_aInputData[5].m_Hook, &m_aInputData[6].m_Hook, &m_aInputData[7].m_Hook}};
                Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire, &m_aInputData[2].m_Fire, &m_aInputData[3].m_Fire, &m_aInputData[4].m_Fire, &m_aInputData[5].m_Fire, &m_aInputData[6].m_Fire, &m_aInputData[7].m_Fire}};
                Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
        }
        {
                static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1], &m_aShowHookColl[2], &m_aShowHookColl[3], &m_aShowHookColl[4], &m_aShowHookColl[5], &m_aShowHookColl[6], &m_aShowHookColl[7]}};
                Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
        }

        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon, &m_aInputData[3].m_WantedWeapon, &m_aInputData[4].m_WantedWeapon, &m_aInputData[5].m_WantedWeapon, &m_aInputData[6].m_WantedWeapon, &m_aInputData[7].m_WantedWeapon}, 1};
                Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon, &m_aInputData[3].m_WantedWeapon, &m_aInputData[4].m_WantedWeapon, &m_aInputData[5].m_WantedWeapon, &m_aInputData[6].m_WantedWeapon, &m_aInputData[7].m_WantedWeapon}, 2};
                Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon, &m_aInputData[3].m_WantedWeapon, &m_aInputData[4].m_WantedWeapon, &m_aInputData[5].m_WantedWeapon, &m_aInputData[6].m_WantedWeapon, &m_aInputData[7].m_WantedWeapon}, 3};
                Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon, &m_aInputData[3].m_WantedWeapon, &m_aInputData[4].m_WantedWeapon, &m_aInputData[5].m_WantedWeapon, &m_aInputData[6].m_WantedWeapon, &m_aInputData[7].m_WantedWeapon}, 4};
                Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon, &m_aInputData[3].m_WantedWeapon, &m_aInputData[4].m_WantedWeapon, &m_aInputData[5].m_WantedWeapon, &m_aInputData[6].m_WantedWeapon, &m_aInputData[7].m_WantedWeapon}, 5};
                Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
        }

        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon, &m_aInputData[2].m_NextWeapon, &m_aInputData[3].m_NextWeapon, &m_aInputData[4].m_NextWeapon, &m_aInputData[5].m_NextWeapon, &m_aInputData[6].m_NextWeapon, &m_aInputData[7].m_NextWeapon}, 0};
                Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon, &m_aInputData[2].m_PrevWeapon, &m_aInputData[3].m_PrevWeapon, &m_aInputData[4].m_PrevWeapon, &m_aInputData[5].m_PrevWeapon, &m_aInputData[6].m_PrevWeapon, &m_aInputData[7].m_PrevWeapon}, 0};
                Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
        }
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
        if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
        {
                CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
                if(g_Config.m_ClAutoswitchWeapons)
                        m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
                // We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
                m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
        }
}

int CControls::SnapInput(int *pData)
{
        // update player state
        if(GameClient()->m_Chat.IsActive())
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
        else if(GameClient()->m_Menus.IsActive())
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
        else
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

        if(GameClient()->m_Scoreboard.IsActive())
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

        if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

        if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

        switch(m_aMouseInputType[g_Config.m_ClDummy])
        {
        case CControls::EMouseInputType::AUTOMATED:
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
                break;
        case CControls::EMouseInputType::ABSOLUTE:
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
                break;
        case CControls::EMouseInputType::RELATIVE:
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
                break;
        }

        bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

        m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

        // we freeze the input if chat or menu is activated
        if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
        {
                if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
                        ResetInput(g_Config.m_ClDummy);

                // bot control — re-apply after freeze so bot inputs bypass menu freeze
                {
                        CBotControl &Bot = GameClient()->m_BotControl;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(!Bot.IsBotActiveAny(D))
                                        continue;

                                CNetObj_PlayerInput *pInput;
                                if(D == g_Config.m_ClDummy)
                                        pInput = &m_aInputData[D];
                                else
                                        pInput = &GameClient()->m_aDummyInput[D];

                                if(Bot.IsBotActiveJump(D))
                                        pInput->m_Jump = Bot.State(D).m_Jump ? 1 : 0;
                                if(Bot.IsBotActiveHook(D))
                                        pInput->m_Hook = Bot.State(D).m_Hook ? 1 : 0;
                                if(Bot.IsBotActiveMove(D))
                                        pInput->m_Direction = Bot.State(D).m_Direction;
                                if(Bot.IsBotActiveFire(D) && Bot.State(D).m_Fire)
                                {
                                        pInput->m_Fire++;
                                        pInput->m_Fire &= INPUT_STATE_MASK;
                                }

                                // Aim: absolute (kx_oaim) takes priority over relative (kx_aim)
                                if(Bot.IsBotActiveOverrideAim(D))
                                {
                                        m_aMousePos[D].x = (float)Bot.State(D).m_OverrideTargetX;
                                        m_aMousePos[D].y = (float)Bot.State(D).m_OverrideTargetY;
                                        pInput->m_TargetX = Bot.State(D).m_OverrideTargetX;
                                        pInput->m_TargetY = Bot.State(D).m_OverrideTargetY;
                                }
                                else if(Bot.IsBotActiveAim(D))
                                {
                                        m_aMousePos[D].x += (float)Bot.State(D).m_TargetX;
                                        m_aMousePos[D].y += (float)Bot.State(D).m_TargetY;
                                        pInput->m_TargetX = (int)m_aMousePos[D].x;
                                        pInput->m_TargetY = (int)m_aMousePos[D].y;
                                }

                                // Copy bot-controlled dummy input back
                                if(D != g_Config.m_ClDummy)
                                        m_aInputData[D] = *pInput;
                        }
                }

                // botnet — sync inactive dummy inputs (menu/chat freeze block)
                {
                        CBotNet &BN = GameClient()->m_BotNet;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy)
                                        continue;
                                if(D != 0 && !GameClient()->Client()->DummyConnected(D))
                                        continue;
                                if(BN.IsDummyActive(D))
                                        m_aInputData[D] = GameClient()->m_aDummyInput[D];
                        }
                }

                mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

                // set the target anyway though so that we can keep seeing our surroundings,
                // even if chat or menu are activated
                m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
                m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

                // send once a second just to be sure
                Send = Send || time_get() > m_LastSendTime + time_freq();

                // force send when bot is active — bypass menu freeze
                {
                        CBotControl &Bot = GameClient()->m_BotControl;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy && Bot.IsBotActiveAny(D))
                                {
                                        Send = true;
                                        break;
                                }
                        }
                }
                // force send when botnet is active
                {
                        // v1.56.171 BUG9: m_AttackEnabled/m_CopyMoves/m_RandomAim/m_PathfinderEnabled
                        // moved to cvars (g_Config.m_KxAttack/m_KxCopyMoves/m_KxRandomAim/m_KxAtkPathfinder)
                        if(g_Config.m_KxAttack || g_Config.m_KxCopyMoves || g_Config.m_KxRandomAim || g_Config.m_KxAtkPathfinder)
                                Send = true;
                }
        }
        else
        {
                m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
                m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

                if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
                {
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePosOnAction[g_Config.m_ClDummy].x;
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePosOnAction[g_Config.m_ClDummy].y;
                        m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
                }

                if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
                {
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
                        m_aMousePos[g_Config.m_ClDummy].x = 1;
                }

                // set direction
                m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
                if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
                        m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
                if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
                        m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

                // dummy copy moves
                if(g_Config.m_ClDummyCopyMoves)
                {
                        const bool filter = g_Config.m_KxDummyCopyMovesFilter != 0;
                        // v1.56.186: Copy Moves Latency. When enabled and >0, inactive
                        // dummies replay the active player's input from N ticks ago
                        // (N = latency_ms * D / tick_time_ms). Recording happens once per
                        // SnapInput; per-dummy lookup uses GetDelayedActiveInput. When
                        // disabled or latency=0 (or history not yet filled), falls back
                        // to live m_aInputData/m_aLastData.
                        // v1.56.187: gated by KxCopyMovesLatencyEnabled master toggle.
                        const int latencyMs = (g_Config.m_KxCopyMovesLatencyEnabled != 0) ? g_Config.m_KxCopyMovesLatency : 0;
                        const int tickSpeed = Client()->GameTickSpeed();
                        if(latencyMs > 0)
                                GameClient()->RecordActiveInput(m_aInputData[g_Config.m_ClDummy]);

                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy)
                                        continue;
                                if(D != 0 && !GameClient()->Client()->DummyConnected(D))
                                        continue;

                                CNetObj_PlayerInput *pDummyInput = &GameClient()->m_aDummyInput[D];

                                // v1.56.186: resolve source input. With latency > 0, use the
                                // history entry from delayTicks ago. delayTicks scales with D
                                // so dummies fire at staggered offsets (D=1: 1x, D=2: 2x...).
                                CNetObj_PlayerInput srcInput = m_aInputData[g_Config.m_ClDummy];
                                CNetObj_PlayerInput srcPrev = m_aLastData[g_Config.m_ClDummy];
                                if(latencyMs > 0 && D > 0)
                                {
                                        const int delayTicks = (latencyMs * D * tickSpeed) / 1000;
                                        CNetObj_PlayerInput delayedCur, delayedPrev;
                                        if(GameClient()->GetDelayedActiveInput(delayTicks, delayedCur))
                                                srcInput = delayedCur;
                                        if(GameClient()->GetDelayedActiveInput(delayTicks + 1, delayedPrev))
                                                srcPrev = delayedPrev;
                                }

                                // Don't copy any input to dummy when spectating others
                                if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
                                {
                                        // Copy Moves Filter: when on, only copy selected fields.
                                        // When off, copy everything (original behavior).
                                        const bool copyJump      = !filter || g_Config.m_KxDummyCopyMovesFilterJump;
                                        const bool copyDirection = !filter || g_Config.m_KxDummyCopyMovesFilterDirection;
                                        const bool copyHook      = !filter || g_Config.m_KxDummyCopyMovesFilterHook;
                                        const bool copyAim       = !filter || g_Config.m_KxDummyCopyMovesFilterAim;
                                        const bool copyFire      = !filter || g_Config.m_KxDummyCopyMovesFilterFire;
                                        const bool copyWeapon    = !filter || g_Config.m_KxDummyCopyMovesFilterWeapon;

                                        if(copyDirection)
                                                pDummyInput->m_Direction = srcInput.m_Direction;
                                        if(copyHook)
                                                pDummyInput->m_Hook = srcInput.m_Hook;
                                        if(copyJump)
                                                pDummyInput->m_Jump = srcInput.m_Jump;
                                        pDummyInput->m_PlayerFlags = srcInput.m_PlayerFlags;
                                        if(copyAim)
                                        {
                                                pDummyInput->m_TargetX = srcInput.m_TargetX;
                                                pDummyInput->m_TargetY = srcInput.m_TargetY;
                                        }
                                        if(copyWeapon)
                                                pDummyInput->m_WantedWeapon = srcInput.m_WantedWeapon;

                                        if(!g_Config.m_ClDummyControl && copyFire)
                                                pDummyInput->m_Fire += srcInput.m_Fire - srcPrev.m_Fire;

                                        if(copyWeapon)
                                        {
                                                pDummyInput->m_NextWeapon += srcInput.m_NextWeapon - srcPrev.m_NextWeapon;
                                                pDummyInput->m_PrevWeapon += srcInput.m_PrevWeapon - srcPrev.m_PrevWeapon;
                                        }
                                }

                                m_aInputData[D] = *pDummyInput;

                                // Apply mirror direction / mirror aim / smart aim.
                                // v1.56.186: pass srcInput so aim transforms use the
                                // delayed source when Copy Moves Latency is active.
                                GameClient()->ApplyCopyMovesTransforms(D, srcInput);
                                m_aInputData[D] = *pDummyInput;
                        }
                }

                // dummy control
                if(g_Config.m_ClDummyControl)
                {
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy)
                                        continue;
                                if(D != 0 && !GameClient()->Client()->DummyConnected(D))
                                        continue;

                                CNetObj_PlayerInput *pDummyInput = &GameClient()->m_aDummyInput[D];
                                pDummyInput->m_Jump = g_Config.m_ClDummyJump;

                                if(g_Config.m_ClDummyFire)
                                        pDummyInput->m_Fire = g_Config.m_ClDummyFire;
                                else if((pDummyInput->m_Fire & 1) != 0)
                                        pDummyInput->m_Fire++;

                                pDummyInput->m_Hook = g_Config.m_ClDummyHook;

                                // Aim: copy player's target X/Y to dummy (like Jump/Fire/Hook).
                                if(g_Config.m_KxDummyAim)
                                {
                                        pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
                                        pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
                                }

                                // Direction: copy player's movement direction to dummy.
                                if(g_Config.m_KxDummyDirection)
                                        pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;

                                m_aInputData[D] = *pDummyInput;
                        }
                }

                // bot control — apply per-dummy bot input
                {
                        CBotControl &Bot = GameClient()->m_BotControl;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(!Bot.IsBotActiveAny(D))
                                        continue;

                                CNetObj_PlayerInput *pInput;
                                if(D == g_Config.m_ClDummy)
                                        pInput = &m_aInputData[D];
                                else
                                        pInput = &GameClient()->m_aDummyInput[D];

                                if(Bot.IsBotActiveJump(D))
                                        pInput->m_Jump = Bot.State(D).m_Jump ? 1 : 0;
                                if(Bot.IsBotActiveHook(D))
                                        pInput->m_Hook = Bot.State(D).m_Hook ? 1 : 0;
                                if(Bot.IsBotActiveMove(D))
                                        pInput->m_Direction = Bot.State(D).m_Direction;
                                if(Bot.IsBotActiveFire(D) && Bot.State(D).m_Fire)
                                {
                                        pInput->m_Fire++;
                                        pInput->m_Fire &= INPUT_STATE_MASK;
                                }

                                // Aim: absolute (kx_oaim) takes priority over relative (kx_aim)
                                if(Bot.IsBotActiveOverrideAim(D))
                                {
                                        m_aMousePos[D].x = (float)Bot.State(D).m_OverrideTargetX;
                                        m_aMousePos[D].y = (float)Bot.State(D).m_OverrideTargetY;
                                        pInput->m_TargetX = Bot.State(D).m_OverrideTargetX;
                                        pInput->m_TargetY = Bot.State(D).m_OverrideTargetY;
                                }
                                else if(Bot.IsBotActiveAim(D))
                                {
                                        m_aMousePos[D].x += (float)Bot.State(D).m_TargetX;
                                        m_aMousePos[D].y += (float)Bot.State(D).m_TargetY;
                                        pInput->m_TargetX = (int)m_aMousePos[D].x;
                                        pInput->m_TargetY = (int)m_aMousePos[D].y;
                                }

                                // Copy bot-controlled dummy input back
                                if(D != g_Config.m_ClDummy)
                                        m_aInputData[D] = *pInput;
                        }
                }

                // botnet — sync inactive dummy inputs
                {
                        CBotNet &BN = GameClient()->m_BotNet;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy)
                                        continue;
                                if(D != 0 && !GameClient()->Client()->DummyConnected(D))
                                        continue;
                                if(BN.IsDummyActive(D))
                                        m_aInputData[D] = GameClient()->m_aDummyInput[D];
                        }
                }

                // stress testing
                if(g_Config.m_DbgStress)
                {
                        float t = Client()->LocalTime();
                        mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

                        m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
                        m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
                        m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
                        m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
                        m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
                }

                // check if we need to send input
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
                Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
                Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));

                // force send when botnet is active
                {
                        // v1.56.171 BUG9: m_AttackEnabled/m_CopyMoves/m_RandomAim/m_PathfinderEnabled
                        // moved to cvars (g_Config.m_KxAttack/m_KxCopyMoves/m_KxRandomAim/m_KxAtkPathfinder)
                        if(g_Config.m_KxAttack || g_Config.m_KxCopyMoves || g_Config.m_KxRandomAim || g_Config.m_KxAtkPathfinder)
                                Send = true;
                }
        }

        // copy and return size
        m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

        if(!Send)
                return 0;

        // v1.56.169 BUG7: BAF.ApplyOverride MUST run before the laserAimActive
        // check below. BAF silent sets m_LaserUnfreezeAimActive (same channel as
        // Laser Unfreeze + AimBot). If BAF runs AFTER the check (as it did since
        // v1.56.130), the flag is deferred to next tick — Fake Aim is not blocked
        // in the current tick and overwrites m_TargetX/Y. Moving BAF here makes
        // the flag visible immediately: line 543 reads it, applies BAF's aim, and
        // line 669 (!laserAimActive) blocks Fake Aim. BAF also modifies
        // dir/jump/hook directly — those go to server via mem_copy at the end.
        // (AimBot + Laser Unfreeze run in OnUpdate before SnapInput, so their
        // flags are already set when BAF runs here — BAF checks !m_LaserUnfreezeAimActive
        // before overwriting, giving Laser Unfreeze/AimBot priority on the aim channel.)
        GameClient()->m_BasicAvoidFreeze.ApplyOverride();

        // Laser unfreeze silent aim — highest priority (consumes flag set by
        // Laser Unfreeze, AimBot, or BAF above).
        bool laserAimActive = m_LaserUnfreezeAimActive;
        if(laserAimActive)
        {
                m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_LaserUnfreezeAimOffset.x;
                m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_LaserUnfreezeAimOffset.y;
                m_LaserUnfreezeAimActive = false;
        }

        // kx_pf_play: replay pathfinder path on real player input.
        // Runs after BAF (BAF has priority for freeze avoidance) but before
        // fake aim + mem_copy so the path input reaches the server.
        GameClient()->m_BotNet.ApplyPfGoInput(&m_aInputData[g_Config.m_ClDummy]);

        // v1.56.151: Fly Ride — pilot input override.
        // Blocks normal WASD (direction/jump) and forces hook state from UpdateFlyRide.
        // Silent aim is already set via m_LaserUnfreezeAimActive (consumed above at line ~543).
        if(g_Config.m_KxFlyRide)
        {
                m_aInputData[g_Config.m_ClDummy].m_Direction = GameClient()->m_BotNet.m_FlyRidePilotDir;
                m_aInputData[g_Config.m_ClDummy].m_Jump = 0; // block W (jump) — W moves anchor instead
                m_aInputData[g_Config.m_ClDummy].m_Hook = GameClient()->m_BotNet.m_FlyRidePilotHook;
        }

        // ── Fake Aim ──────────────────────────────────────────────
        m_FakeAimRenderActive = false;
        bool fakeActive = false;
        vec2 fakeOffset(0, 0);
        bool fakeShowForMe = false;

        // v1.56.209: Show For Me flicker fix.
        // Background — why this is needed:
        //   m_FakeAimRenderOffset is updated only here, once per SnapInput (~50 Hz).
        //   m_aMousePos is updated every frame in OnRender (~60-144 Hz).
        //   players.cpp picks m_FakeAimRenderOffset when m_FakeAimRenderActive is true,
        //   otherwise it falls through to m_aMousePos (live mouse).
        //
        //   On a release tick (laser/aimbot/avoid/pfgo owns the aim, OR
        //   willFire/hookPress), the fake block is either skipped (so
        //   m_FakeAimRenderActive stays false) or sets fakeOffset = realAim
        //   (so m_FakeAimRenderOffset == realAim). In both cases players.cpp
        //   ends up showing the real aim — for 1-3 frames until the next
        //   SnapInput restores the mask.
        //
        // Fix: remember the last "real" fake offset and hold it across
        // release ticks. The actual aim going to the server (m_aInputData /
        // pData) is unchanged — only the rendered character angle is held.
        static vec2 s_LastFakeRenderOffset = vec2(0, 0);
        static bool s_HaveLastFake = false;

        // Fire/hook detection + robot update — ALWAYS runs (even when laser unfreeze active)
        bool willFire = false;
        bool hookPress = false;
        vec2 realAim = m_aMousePos[g_Config.m_ClDummy];

        // Robot frozen aim — declared OUTSIDE if block so generation can access it
        static vec2 s_RobotAim = vec2(0, 0);
        static bool s_RobotInit = false;

        if(g_Config.m_KxFakeAim)
        {
                // Detect weapon fire via 1-tick prediction.
                // This catches EVERY real shot (not just the rising edge of
                // m_Fire), which is what the aim-release system needs: while
                // the player holds fire, the server fires on the weapon's
                // reload cadence, and we must release the fake aim on each
                // of those ticks so the shot lands. A parity rising-edge
                // only catches the first press and breaks Random/Spin/Lag.
                //
                // Cost: one full CGameWorld CopyWorld per SnapInput (~50/sec).
                // This is cheap in practice — CopyWorld on the stack is
                // allocation+free of the world's entities, no leak (the
                // stack CGameWorld destructor runs Clear() which deletes
                // every entity it owns). The FPS collapse in v1.56.55 was
                // caused by the RemoveEntity leak in laser_unfreeze.cpp,
                // NOT by this prediction. That leak is fixed in v1.56.56.
                CGameClient *pGC = GameClient();
                if(pGC && pGC->m_Snap.m_pLocalInfo)
                {
                        int LocalID = pGC->m_Snap.m_LocalClientId;
                        CCharacter *pChar = pGC->m_PredictedWorld.GetCharacterById(LocalID);
                        if(pChar)
                        {
                                int tickBefore = pChar->GetAttackTick();
                                bool wasFrozen = (pChar->m_FreezeTime > 0) || pChar->Core()->m_DeepFrozen;
                                CGameWorld FutureWorld;
                                FutureWorld.CopyWorld(&pGC->m_PredictedWorld);
                                CCharacter *pFuture = FutureWorld.GetCharacterById(LocalID);
                                if(pFuture)
                                {
                                        CNetObj_PlayerInput FutureInput = m_aInputData[g_Config.m_ClDummy];
                                        pFuture->OnDirectInput(&FutureInput);
                                        FutureWorld.m_GameTick = FutureWorld.GameTick() + 1;
                                        pFuture->OnPredictedInput(&FutureInput);
                                        FutureWorld.Tick();
                                        if(CCharacter *pAfter = FutureWorld.GetCharacterById(LocalID))
                                        {
                                                if(pAfter->GetAttackTick() != tickBefore)
                                                        willFire = true;
                                                // v1.56.103: Emulate freeze state in prediction.
                                                // If player was frozen and will unfreeze next tick,
                                                // treat as willFire so fake aim releases correctly.
                                                if(wasFrozen && pAfter->m_FreezeTime == 0 && !pAfter->Core()->m_DeepFrozen)
                                                        willFire = true;
                                        }
                                }
                        }
                }

                // Hook press edge
                static int s_LastHook = 0;
                hookPress = (m_aInputData[g_Config.m_ClDummy].m_Hook != 0 && s_LastHook == 0);
                s_LastHook = m_aInputData[g_Config.m_ClDummy].m_Hook;

                // Robot/Lag: remember last real aim.
                //
                // SILENT Laser Unfreeze case: when laserAimActive is true,
                // Laser Unfreeze is firing at the BOUNCE position
                // (m_LaserUnfreezeAimOffset), but in silent mode it did NOT
                // move the visible crosshair (m_aMousePos). So realAim is
                // still the player's visible aim, NOT where the laser fired.
                // Storing realAim here would make Robot/Lag remember the
                // wrong position. Use the laser's bounce offset instead —
                // it's the actual aim that was sent to the server this tick.
                //
                // Non-silent laser: SetMousePos already moved m_aMousePos to
                // the bounce before SnapInput, so realAim == bounce and
                // using realAim is correct (laserAimActive path is skipped
                // via the guard below anyway).
                if(!s_RobotInit)
                {
                        s_RobotAim = realAim;
                        s_RobotInit = true;
                }
                if(willFire || hookPress)
                {
                        if(laserAimActive)
                                s_RobotAim = m_LaserUnfreezeAimOffset;
                        else if(GameClient()->m_BotNet.m_PfGoAimedThisTick)
                                s_RobotAim = GameClient()->m_BotNet.m_PfGoAimOffset;
                        else
                                s_RobotAim = realAim;
                }
        }

        // Fake aim generation — skipped when laser unfreeze active (laser has priority)
        // v1.56.204: skipped when PfGo applied aim this tick (hook rising edge),
        // but otherwise Fake Aim can coexist with PfGo playback.
        if(g_Config.m_KxFakeAim && !laserAimActive && !GameClient()->m_BotNet.m_PfGoAimedThisTick)
        {
                // Decide: real or fake
                if(willFire || hookPress)
                {
                        fakeActive = true;
                        fakeOffset = realAim;
                        fakeShowForMe = true;
                }
                else
                {
                        int mode = g_Config.m_KxFakeAimMode;
                        int speed = g_Config.m_KxFakeAimSpeed;
                        if(speed < 1) speed = 1;
                        if(speed > 100) speed = 100;
                        constexpr float AIM_DIST = 200.0f;

                        if(mode == 0) // Random
                        {
                                static int s_TickCounter = 0;
                                static float s_RandAngle = 0.0f;
                                s_TickCounter++;
                                if(s_TickCounter >= speed)
                                {
                                        s_RandAngle = ((float)rand() / (float)RAND_MAX) * pi * 2.0f;
                                        s_TickCounter = 0;
                                }
                                fakeOffset = vec2(cosf(s_RandAngle), sinf(s_RandAngle)) * AIM_DIST;
                        }
                        else if(mode == 1) // Robot
                        {
                                fakeOffset = s_RobotAim;
                        }
                        else if(mode == 3) // Lag
                        {
                                static int s_LagCounter = 0;
                                s_LagCounter++;
                                if(s_LagCounter >= speed)
                                {
                                        s_RobotAim = realAim;
                                        s_LagCounter = 0;
                                }
                                fakeOffset = s_RobotAim;
                        }
                        else // Spin
                        {
                                static float s_SpinAngle = 0.0f;
                                s_SpinAngle += (speed / 100.0f) * 0.5f;
                                fakeOffset = vec2(cosf(s_SpinAngle), sinf(s_SpinAngle)) * AIM_DIST;
                        }

                        fakeActive = true;
                        fakeShowForMe = g_Config.m_KxFakeAimShowForMe != 0;
                }
        }

        // ── Apply fake aim override (lower priority than laser unfreeze) ──
        if(fakeActive)
        {
                // Render data for players.cpp
                m_FakeAimRenderActive = true;
                m_FakeAimRenderOffset = fakeOffset;

                if(fakeShowForMe)
                {
                        // Show for me ON: set in m_aInputData (server + prediction)
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)fakeOffset.x;
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)fakeOffset.y;
                }
                // else: don't touch m_aInputData — prediction keeps real aim.
                //       pData modified after mem_copy for server-only fake.
        }

        m_LastSendTime = time_get();
        mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

        // Fake Aim show for me = OFF: modify pData AFTER mem_copy.
        // Server gets fake, local prediction keeps real aim.
        if(fakeActive && !fakeShowForMe)
        {
                CNetObj_PlayerInput *pSend = (CNetObj_PlayerInput *)pData;
                pSend->m_TargetX = (int)fakeOffset.x;
                pSend->m_TargetY = (int)fakeOffset.y;
        }

        // v1.56.209: Hold the fake mask across release ticks.
        // See the long comment near s_LastFakeRenderOffset declaration above
        // for the full explanation. Short version:
        //   - If this tick is a release (another component owns the aim, OR
        //     player is firing / pressing hook), the mask has just dropped.
        //   - Restore it from the last "real" fake offset so players.cpp keeps
        //     drawing the spoofed direction instead of falling through to the
        //     live m_aMousePos.
        // The aim actually sent to the server (m_aInputData / pData) is not
        // touched here — this only affects the rendered character angle.
        if(g_Config.m_KxFakeAim && s_HaveLastFake)
        {
                // Figure out whether this tick is a release tick.
                bool aimOwnedByComponent = laserAimActive || GameClient()->m_BotNet.m_PfGoAimedThisTick;
                bool aimOwnedByPlayerFire = fakeActive && (willFire || hookPress);
                bool isReleaseTick = aimOwnedByComponent || aimOwnedByPlayerFire;

                if(isReleaseTick)
                {
                        // Keep the mask: render the previous fake direction.
                        m_FakeAimRenderActive = true;
                        m_FakeAimRenderOffset = s_LastFakeRenderOffset;
                }
        }

        // Remember the current fake offset for next tick's mask continuation.
        // Only update from genuine fake ticks — not from release ticks where
        // fakeOffset happens to be realAim (willFire/hookPress) — otherwise
        // we'd poison the mask with the real aim and the fix would be useless.
        if(fakeActive && !laserAimActive && !GameClient()->m_BotNet.m_PfGoAimedThisTick &&
           !willFire && !hookPress)
        {
                s_LastFakeRenderOffset = fakeOffset;
                s_HaveLastFake = true;
        }

        return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
        if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
        {
                // Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
                m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
                // Autoswitch weapon if we're out of ammo
                if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
                        GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
                        GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
                        GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
                {
                        int Weapon;
                        for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
                        {
                                if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
                                        continue;
                                if(m_aAmmoCount[Weapon] > 0)
                                        break;
                        }
                        if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
                                m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
                }
        }

        // update target pos
        if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
        {
                // make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
                vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
                float Zoom = GameClient()->m_Camera.m_Zoom;
                m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
        }
        else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
        {
                m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
        }
        else
        {
                m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
        }
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
        if(GameClient()->IsWorldPaused())
                return false;

        if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
        {
                vec2 AbsoluteDirection;
                if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
                {
                        m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
                        GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
                }
                return true;
        }

        float Factor = 1.0f;
        if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
        {
                Factor = g_Config.m_ClDyncamMousesens / 100.0f;
        }
        else
        {
                switch(CursorType)
                {
                case IInput::CURSOR_MOUSE:
                        Factor = g_Config.m_InpMousesens / 100.0f;
                        break;
                case IInput::CURSOR_JOYSTICK:
                        Factor = g_Config.m_InpControllerSens / 100.0f;
                        break;
                default:
                        dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
                }
        }

        if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
                Factor *= GameClient()->m_Camera.m_Zoom;

        m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
        GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
        ClampMousePos();
        return true;
}

void CControls::ClampMousePos()
{
        if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
        {
                m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
                m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
        }
        else
        {
                const float MouseMin = GetMinMouseDistance();
                const float MouseMax = GetMaxMouseDistance();

                float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
                if(MouseDistance < 0.001f)
                {
                        m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
                        m_aMousePos[g_Config.m_ClDummy].y = 0;
                        MouseDistance = 0.001f;
                }
                if(MouseDistance < MouseMin)
                        m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
                MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
                if(MouseDistance > MouseMax)
                        m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;
        }
}

float CControls::GetMinMouseDistance() const
{
        return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
        float CameraMaxDistance = 200.0f;
        float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
        float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
        float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
        return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}
