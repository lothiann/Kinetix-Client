/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>

// bot_control integration
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

                                // Aim: absolute (c_oaim) takes priority over relative (c_aim)
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
                        CBotNet &BN = GameClient()->m_BotNet;
                        if(BN.m_AttackEnabled || BN.m_CopyMoves || BN.m_RandomAim || BN.m_PathfinderEnabled)
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
                        const bool filter = g_Config.m_ClDummyCopyMovesFilter != 0;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy)
                                        continue;
                                if(D != 0 && !GameClient()->Client()->DummyConnected(D))
                                        continue;

                                CNetObj_PlayerInput *pDummyInput = &GameClient()->m_aDummyInput[D];

                                // Don't copy any input to dummy when spectating others
                                if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
                                {
                                        // Copy Moves Filter: when on, only copy selected fields.
                                        // When off, copy everything (original behavior).
                                        const bool copyJump      = !filter || g_Config.m_ClDummyCopyMovesFilterJump;
                                        const bool copyDirection = !filter || g_Config.m_ClDummyCopyMovesFilterDirection;
                                        const bool copyHook      = !filter || g_Config.m_ClDummyCopyMovesFilterHook;
                                        const bool copyAim       = !filter || g_Config.m_ClDummyCopyMovesFilterAim;
                                        const bool copyFire      = !filter || g_Config.m_ClDummyCopyMovesFilterFire;

                                        if(copyDirection)
                                                pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
                                        if(copyHook)
                                                pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
                                        if(copyJump)
                                                pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
                                        pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
                                        if(copyAim)
                                        {
                                                pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
                                                pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
                                        }
                                        pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

                                        if(!g_Config.m_ClDummyControl && copyFire)
                                                pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

                                        pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
                                        pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
                                }

                                m_aInputData[D] = *pDummyInput;

                                // Apply mirror direction / mirror aim / smart aim.
                                GameClient()->ApplyCopyMovesTransforms(D);
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
                                if(g_Config.m_ClDummyAim)
                                {
                                        pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
                                        pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
                                }

                                // Direction: copy player's movement direction to dummy.
                                if(g_Config.m_ClDummyDirection)
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

                                // Aim: absolute (c_oaim) takes priority over relative (c_aim)
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
                        CBotNet &BN = GameClient()->m_BotNet;
                        if(BN.m_AttackEnabled || BN.m_CopyMoves || BN.m_RandomAim || BN.m_PathfinderEnabled)
                                Send = true;
                }
        }

        // copy and return size
        m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

        if(!Send)
                return 0;

        m_LastSendTime = time_get();
        mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
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
