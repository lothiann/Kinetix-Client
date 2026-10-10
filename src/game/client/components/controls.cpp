/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
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
#include <game/client/components/kinetix/avoid_freeze.h>
#include <game/client/components/kinetix/bot_control.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/tas.h>

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

void CControls::ConKeyInputPfEditFire(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;
	CBotNet &BN = pState->m_pControls->GameClient()->m_BotNet;
	if(BN.m_PfEditorMode != 0)
	{
		BN.m_PfEditFireHeld = pResult->GetInteger(0) != 0;
		return;
	}
	BN.m_PfEditFireHeld = false;
	ConKeyInputCounter(pResult, pUserData);
}

void CControls::ConKeyInputPfEditHook(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;
	CBotNet &BN = pState->m_pControls->GameClient()->m_BotNet;
	if(BN.m_PfEditorMode != 0)
	{
		BN.m_PfEditHookHeld = pResult->GetInteger(0) != 0;
		return;
	}
	BN.m_PfEditHookHeld = false;
	ConKeyInputState(pResult, pUserData);
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
                Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputPfEditHook, &s_State, "Hook");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire, &m_aInputData[2].m_Fire, &m_aInputData[3].m_Fire, &m_aInputData[4].m_Fire, &m_aInputData[5].m_Fire, &m_aInputData[6].m_Fire, &m_aInputData[7].m_Fire}};
                Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputPfEditFire, &s_State, "Fire");
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
                        // moved to cvars (g_Config.m_KxHelpers/m_KxCopyMoves)
                        if(g_Config.m_KxHelpers || g_Config.m_KxCopyMoves)
                                Send = true;
                }
        }
        else
        {
                // v1.56.262: Auto Fire — while fire is held, force a new fire edge
                // every Delay ticks (0 = every tick). Runs BEFORE the Fake Aim
                // willFire prediction below, so the forced shot releases the fake
                // aim and triggers AimBot on the same tick.
                if(g_Config.m_KxAutoFire)
                {
                        static int s_AutoFireCounter = 0;
                        CNetObj_PlayerInput *pAf = &m_aInputData[g_Config.m_ClDummy];
                        if((pAf->m_Fire & 1) != 0)
                        {
                                const int AfDelay = g_Config.m_KxAutoFireDelay;
                                if(AfDelay <= 0 || s_AutoFireCounter >= AfDelay - 1)
                                {
                                        pAf->m_Fire = (pAf->m_Fire + 1) | 1;
                                        pAf->m_Fire &= INPUT_STATE_MASK;
                                        s_AutoFireCounter = 0;
                                }
                                else
                                        s_AutoFireCounter++;
                        }
                        else
                                s_AutoFireCounter = 0;
                }

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

                // v1.56.262: BalanceBot — keep X distance to the nearest matching
                // player. Overrides the CURRENT dummy's direction EVERY tick (the
                // player's own keys pass through only inside the Stand Radius).
                // v1.56.277: Off while freeze — pause the BalanceBot while we are
                // frozen (ninja counts as frozen when BB FNG Freeze is on). Same
                // freeze check as the target filter below.
                const int BbSelfId = GameClient()->m_Snap.m_LocalClientId;
                const bool BbSelfFrozen = BbSelfId >= 0 &&
                        (GameClient()->m_aClients[BbSelfId].m_FreezeEnd != 0 || GameClient()->m_aClients[BbSelfId].m_LiveFrozen ||
                                (g_Config.m_KxBbFngFreeze && GameClient()->m_Snap.m_aCharacters[BbSelfId].m_Active &&
                                        GameClient()->m_Snap.m_aCharacters[BbSelfId].m_Cur.m_Weapon == WEAPON_NINJA));
                if(g_Config.m_KxBalanceBot && GameClient()->m_Snap.m_LocalClientId >= 0 && !BbSelfFrozen)
                {
                        CGameClient *pBb = GameClient();
                        const int BbLocal = pBb->m_Snap.m_LocalClientId;
                        CCharacter *pBbMe = pBb->m_PredictedWorld.GetCharacterById(BbLocal);
                        if(pBbMe)
                        {
                                const vec2 MyPos = pBbMe->Core()->m_Pos;
                                const float BbIntra = pBb->Client()->IntraGameTick(g_Config.m_ClDummy);
                                const float BbRadiusSq = (float)g_Config.m_KxBalanceBotRadius * (float)g_Config.m_KxBalanceBotRadius;
                                const float StandRadius = (float)g_Config.m_KxBalanceBotStandRadius;

                                int best = -1;
                                float bestDistSq = 1e18f;
                                vec2 bestPos(0, 0);
                                for(int i = 0; i < MAX_CLIENTS; i++)
                                {
                                        if(i == BbLocal)
                                                continue;
                                        if(!pBb->m_aClients[i].m_Active || !pBb->m_Snap.m_aCharacters[i].m_Active)
                                                continue;
                                        const vec2 prevPos((float)pBb->m_Snap.m_aCharacters[i].m_Prev.m_X, (float)pBb->m_Snap.m_aCharacters[i].m_Prev.m_Y);
                                        const vec2 curPos((float)pBb->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pBb->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                                        const vec2 tPos = mix(prevPos, curPos, BbIntra);

                                        // Vertical filter: Below / Above / Both (relative to us).
                                        const int vert = g_Config.m_KxBalanceBotVertical;
                                        if(vert == 0 && tPos.y <= MyPos.y)
                                                continue;
                                        if(vert == 1 && tPos.y >= MyPos.y)
                                                continue;

                                        const float dx = tPos.x - MyPos.x;
                                        const float dy = tPos.y - MyPos.y;
                                        const float distSq = dx * dx + dy * dy;
                                        if(distSq > BbRadiusSq || distSq >= bestDistSq)
                                                continue;

                                        // Exclude players through walls (insert line).
                                        if(pBb->Collision()->IntersectLineTeleHook(MyPos, tPos, nullptr, nullptr) != 0)
                                                continue;

                                        // Team filter: 0=both 1=my 2=war 3=blue 4=red (v1.56.275).
                                        // "Team" = REAL team (red/blue); My/War compare with ours.
                                        if(g_Config.m_KxBbTeam == 1 && pBb->m_aClients[i].m_Team != pBb->m_aClients[BbLocal].m_Team)
                                                continue;
                                        if(g_Config.m_KxBbTeam == 2 && pBb->m_aClients[i].m_Team == pBb->m_aClients[BbLocal].m_Team)
                                                continue;
                                        if(g_Config.m_KxBbTeam == 3 && pBb->m_aClients[i].m_Team != TEAM_BLUE)
                                                continue;
                                        if(g_Config.m_KxBbTeam == 4 && pBb->m_aClients[i].m_Team != TEAM_RED)
                                                continue;
                                        // DDrace team filter: 0=both 1=my 2=other 3=target (v1.56.275)
                                        if(g_Config.m_KxBbDDraceTeam == 1 && !pBb->m_Teams.SameTeam(BbLocal, i))
                                                continue;
                                        if(g_Config.m_KxBbDDraceTeam == 2 && pBb->m_Teams.SameTeam(BbLocal, i))
                                                continue;
                                        if(g_Config.m_KxBbDDraceTeam == 3 && pBb->m_Teams.Team(i) != g_Config.m_KxBbDDraceTeamId)
                                                continue;
                                        // Friend filter
                                        if(g_Config.m_KxBbFriend == 1 && pBb->m_aClients[i].m_Friend)
                                                continue;
                                        if(g_Config.m_KxBbFriend == 2 && !pBb->m_aClients[i].m_Friend)
                                                continue;
                                        // Dummy filter
                                        bool isDummy = false;
                                        for(int d = 0; d < MAX_DUMMIES; d++)
                                        {
                                                if(pBb->m_aLocalIds[d] == i)
                                                {
                                                        isDummy = true;
                                                        break;
                                                }
                                        }
                                        if(g_Config.m_KxBbDummy == 1 && isDummy)
                                                continue;
                                        if(g_Config.m_KxBbDummy == 2 && !isDummy)
                                                continue;
                                        // Clanmate filter
                                        const bool isClanmate = str_comp(pBb->m_aClients[i].m_aClan, pBb->m_aClients[BbLocal].m_aClan) == 0;
                                        if(g_Config.m_KxBbClanmate == 1 && isClanmate)
                                                continue;
                                        if(g_Config.m_KxBbClanmate == 2 && !isClanmate)
                                                continue;
                                        // Freeze filter (LiveFrozen covers servers like FNG that hide
                                        // the unfreeze tick — same check as the freeze flakes rendering)
                                        // v1.56.275: FNG Freeze — ninja counts as frozen.
                                        bool isFrozen = pBb->m_aClients[i].m_FreezeEnd != 0 ||
                                                pBb->m_aClients[i].m_LiveFrozen;
                                        if(g_Config.m_KxBbFngFreeze && pBb->m_Snap.m_aCharacters[i].m_Cur.m_Weapon == WEAPON_NINJA)
                                                isFrozen = true;
                                        if(g_Config.m_KxBbFreeze == 1 && !isFrozen)
                                                continue;
                                        if(g_Config.m_KxBbFreeze == 2 && isFrozen)
                                                continue;
                                        // Targets filter (Both / Only / Ignore + IDs)
                                        if(g_Config.m_KxBbTargetsFilter != 0)
                                        {
                                                bool found = false;
                                                bool anyValid = false;
                                                const char *pIds = g_Config.m_KxBbTargetsIds;
                                                while(*pIds)
                                                {
                                                        while(*pIds && (*pIds == ' ' || *pIds == ',' || *pIds == ';'))
                                                                pIds++;
                                                        if(!*pIds)
                                                                break;
                                                        if(*pIds >= '0' && *pIds <= '9')
                                                        {
                                                                const int id = atoi(pIds);
                                                                if(id >= 0 && id < 128 && pBb->m_aClients[id].m_Active)
                                                                {
                                                                        anyValid = true;
                                                                        if(id == i)
                                                                                found = true;
                                                                }
                                                        }
                                                        while(*pIds && *pIds != ' ' && *pIds != ',' && *pIds != ';')
                                                                pIds++;
                                                }
                                                if(g_Config.m_KxBbTargetsFilter == 1 && anyValid && !found)
                                                        continue;
                                                if(g_Config.m_KxBbTargetsFilter == 2 && anyValid && found)
                                                        continue;
                                        }

                                        bestDistSq = distSq;
                                        best = i;
                                        bestPos = tPos;
                                }

                                // Outside the Stand Radius — walk toward the target on X
                                // (Y ignored). Inside it — pass the player's own keys through.
                                if(best >= 0)
                                {
                                        const float dx = bestPos.x - MyPos.x;
                                        if(fabsf(dx) > StandRadius)
                                                m_aInputData[g_Config.m_ClDummy].m_Direction = dx > 0 ? 1 : -1;
                                }
                        }
                }

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
                                        const bool tfHooking = GameClient()->m_BotNet.m_TripleFlyHooking && GameClient()->m_BotNet.m_TripleFlyDummy == D;
                                        const bool copyHook      = (!filter || g_Config.m_KxDummyCopyMovesFilterHook) && !tfHooking;
                                        const bool copyAim       = (!filter || g_Config.m_KxDummyCopyMovesFilterAim) && !tfHooking;
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
                                                // v1.56.270: when Fake Aim is active, copy the FAKE aim
                                                // (what the server sees for the active player), not the
                                                // real crosshair. m_FakeAimRenderOffset holds the last
                                                // fake offset — the fresh one is generated later in this
                                                // call, so the copy lags by at most one send tick.
                                                // v1.56.273: skipped when Smart aim is on — smart owns the
                                                // dummy aim, the fake aim copy is disabled (no conflict).
                                                if(g_Config.m_KxFakeAim && m_FakeAimRenderActive &&
                                                        !(g_Config.m_KxDummyCopyAimMode == 0 && g_Config.m_KxDummyCopySmartAim))
                                                {
                                                        pDummyInput->m_TargetX = (int)m_FakeAimRenderOffset.x;
                                                        pDummyInput->m_TargetY = (int)m_FakeAimRenderOffset.y;
                                                }
                                                else
                                                {
                                                        pDummyInput->m_TargetX = srcInput.m_TargetX;
                                                        pDummyInput->m_TargetY = srcInput.m_TargetY;
                                                }
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

                // RT Pathfinder playback: the plan input must be applied and sent
                // EVERY game tick. The send-on-change throttle below skips this
                // whole tail on unchanged-input ticks — then the hook never runs,
                // plan inputs get skipped (jump impulses, direction changes) and
                // the tee runs ahead of the plan (desync).
                if(GameClient()->m_BotNet.m_RtPfState == 1)
                        Send = true;
                Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));

                // force send when botnet is active
                {
                        // v1.56.171 BUG9: m_AttackEnabled/m_CopyMoves/m_RandomAim/m_PathfinderEnabled
                        // moved to cvars (g_Config.m_KxHelpers/m_KxCopyMoves)
                        if(g_Config.m_KxHelpers || g_Config.m_KxCopyMoves)
                                Send = true;
                }

                // TAS real replay: force-send every tick so the recorded take
                // plays back at exact tick timing.
                if(GameClient()->m_Tas.IsRealPlaying())
                        Send = true;
        }

        // copy and return size
        m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

        // v1.57: AF runs on EVERY input production call — AFTER the
        // m_aLastData snapshot above (an unchanged raw input after an AF
        // override then never re-triggers sends: the server keeps applying
        // the last sent override exactly like it keeps a held key) and
        // BEFORE the Send gate + the silentAimActive check below (the
        // v1.56.169 BUG7 ordering). With nothing pressed the raw input is
        // byte-identical call after call, so the change throttle + the
        // 25 Hz keepalive used to skip this whole tail — AF included — for
        // up to a whole keepalive period: the last-moment window (~10
        // ticks) slipped by and the escape fired late or not at all. AF's
        // own edits are folded into the Send decision right below, so the
        // chosen escape transmits on this very call. AF evaluates once per
        // predicted tick (its own guard) and skips menu/chat (frozen input).
        if(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING)
        {
                CNetObj_PlayerInput InputBeforeAF = m_aInputData[g_Config.m_ClDummy];
                GameClient()->m_AvoidFreeze.ApplyOverride();
                if(mem_comp(&m_aInputData[g_Config.m_ClDummy], &InputBeforeAF, sizeof(InputBeforeAF)))
                        Send = true;
        }

        if(!Send)
                return 0;

        // Laser unfreeze silent aim — highest priority (consumes flag set by
        // Laser Unfreeze, AimBot, or AF above).
        bool silentAimActive = m_SilentAimActive;
        if(silentAimActive)
        {
                m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_SilentAimOffset.x;
                m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_SilentAimOffset.y;
                m_SilentAimActive = false;
        }

        // v1.56.151: Fly Ride — pilot input override.
        // Blocks normal WASD (direction/jump) and forces hook state from UpdateFlyRide.
        // Silent aim is already set via m_SilentAimActive (consumed above at line ~543).
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

        if(g_Config.m_KxFakeAim && !GameClient()->m_Tas.IsRealPlaying())
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
                // SILENT Laser Unfreeze case: when silentAimActive is true,
                // Laser Unfreeze is firing at the BOUNCE position
                // (m_SilentAimOffset), but in silent mode it did NOT
                // move the visible crosshair (m_aMousePos). So realAim is
                // still the player's visible aim, NOT where the laser fired.
                // Storing realAim here would make Robot/Lag remember the
                // wrong position. Use the laser's bounce offset instead —
                // it's the actual aim that was sent to the server this tick.
                //
                // Non-silent laser: SetMousePos already moved m_aMousePos to
                // the bounce before SnapInput, so realAim == bounce and
                // using realAim is correct (silentAimActive path is skipped
                // via the guard below anyway).
                if(!s_RobotInit)
                {
                        s_RobotAim = realAim;
                        s_RobotInit = true;
                }
                if(willFire || hookPress)
                {
                        if(silentAimActive)
                                s_RobotAim = m_SilentAimOffset;
                        else
                                s_RobotAim = realAim;
                }
        }

        // Fake aim generation — skipped when laser unfreeze active (laser has priority)
        // v1.56.269: fully disable the original Fake Aim during TAS real
        // playback. The old guard (take has fake aim) left the original fake
        // aim running while replaying a take WITHOUT fake aim — conflicting
        // with the replay. Same for the fire-detection, mask-hold and Predict
        // guards below.
        if(g_Config.m_KxFakeAim && !GameClient()->m_Tas.IsRealPlaying())
        {
                // Decide: real or fake
                // v1.56.279: no silent-channel bypass — the AimBot's silent aim is
                // masked by the fake pattern like everything else and released
                // only on a real shot/hook.
                // Real (5) never masks — while the silent channel is in action
                // the silent aim goes through raw and Show Server-Side shows it;
                // the masking modes (random/robot/lag/spin/chaos) hold the
                // pattern and release the silent aim only on a real shot/hook.
                if(willFire || hookPress)
                {
                        fakeActive = true;
                        fakeOffset = silentAimActive ? m_SilentAimOffset : realAim;
                        fakeShowForMe = true;
                }
                else if(!(g_Config.m_KxFakeAimMode == 5 && silentAimActive))
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
                        else if(mode == 4) // Chaos — jitter the real aim (v1.56.247)
                        {
                                // v1.56.247: Chaos — jitters the CURRENT (real) aim with
                                // randomness. Speed = ticks between jitter re-rolls (same
                                // cadence semantics as Random), Jitter = max random offset
                                // magnitude in world units. The jitter follows the live
                                // mouse — it never locks to a fixed point.
                                static int s_ChCounter = 0;
                                static vec2 s_ChOffset(0, 0);
                                s_ChCounter++;
                                if(s_ChCounter >= speed)
                                {
                                        const float ChAngle = ((float)rand() / (float)RAND_MAX) * pi * 2.0f;
                                        const float ChMag = ((float)rand() / (float)RAND_MAX) * (float)g_Config.m_KxFakeAimJitter;
                                        s_ChOffset = vec2(cosf(ChAngle), sinf(ChAngle)) * ChMag;
                                        s_ChCounter = 0;
                                }
                                fakeOffset = realAim + s_ChOffset;
                        }
                        else if(mode == 5) // Real — send the real aim unchanged (v1.56.247)
                        {
                                // v1.56.247: Real — the "fake" aim is simply the real aim.
                                // No-op mode: laser unfreeze / aimbot keep full priority (the
                                // silentAimActive guard above skips fake generation), while the
                                // Distance-from-center slider and Show-for-me still apply.
                                fakeOffset = realAim;
                        }
                        else // Spin
                        {
                                // v1.56.263: the authoritative spin state lives in the class —
                                // Fake Aim Predict advances it by the intra-tick fraction in
                                // OnRender for frame-rate smooth rotation.
                                m_FakeAimSpinAngle += (speed / 100.0f) * 0.5f;
                                fakeOffset = vec2(cosf(m_FakeAimSpinAngle), sinf(m_FakeAimSpinAngle)) * AIM_DIST;
                        }

                        // v1.56.247: Distance from center — rescale the fake aim to an
                        // exact distance from (0,0), preserving direction (0 = off).
                        if(g_Config.m_KxFakeAimDistCenter > 0)
                        {
                                float FakeLen = length(fakeOffset);
                                if(FakeLen > 0.5f)
                                        fakeOffset = fakeOffset * ((float)g_Config.m_KxFakeAimDistCenter / FakeLen);
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

        // TAS Fake Aim: while a take is executed the rendered aim comes from
        // the TAS (Real/Smooth/...) instead of the live mouse. Render-only —
        // the input sent to the server stays the recorded one.
        if(GameClient()->m_Tas.IsRealPlaying() && GameClient()->m_Tas.FakeAimActive())
        {
                vec2 TasAim(0.0f, 0.0f);
                if(GameClient()->m_Tas.FakeAimOffset(&TasAim))
                {
                        m_FakeAimRenderActive = true;
                        m_FakeAimRenderOffset = TasAim;
                }
        }

        // TAS real replay: execute the recorded take on the real player.
        // Last among the overrides (right before mem_copy) so the recorded
        // movement/aim/fire win over every other input source.
        GameClient()->m_Tas.ApplyTasRealInput(&m_aInputData[g_Config.m_ClDummy]);

        // RT Pathfinder real-time playback: apply the next planned input to the
        // live player (same override slot as the TAS real replay; TAS wins when
        // both are active — ApplyRtPfRealInput bails while TAS replays).
        GameClient()->m_BotNet.ApplyRtPfRealInput(&m_aInputData[g_Config.m_ClDummy]);

        // v1.56.247: Distance from center — applies to EVERY aim sent to the
        // server: the real player aim, the laser unfreeze silent-aim channel and
        // Show-for-me fake aims (the pData-only fake path is rescaled where
        // fakeOffset is generated). Rescales the aim to exactly
        // kx_fake_aim_dist_center units from (0,0), preserving direction.
        // 0 = off. Skipped during TAS real replay and RT pathfinder playback so both replay as-is.
        if(g_Config.m_KxFakeAimDistCenter > 0 && !GameClient()->m_Tas.IsRealPlaying() && GameClient()->m_BotNet.m_RtPfState != 1)
        {
                vec2 SendTarget((float)m_aInputData[g_Config.m_ClDummy].m_TargetX, (float)m_aInputData[g_Config.m_ClDummy].m_TargetY);
                float SendLen = length(SendTarget);
                if(SendLen > 0.5f)
                {
                        vec2 Scaled = SendTarget * ((float)g_Config.m_KxFakeAimDistCenter / SendLen);
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Scaled.x;
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Scaled.y;
                }
        }

        m_LastSendTime = time_get();
        mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

        // Fake Aim show for me = OFF: modify pData AFTER mem_copy.
        // Server gets fake, local prediction keeps real aim.
        if(fakeActive && !fakeShowForMe && !GameClient()->m_Tas.IsRealPlaying())
        {
                CNetObj_PlayerInput *pSend = (CNetObj_PlayerInput *)pData;
                pSend->m_TargetX = (int)fakeOffset.x;
                pSend->m_TargetY = (int)fakeOffset.y;
        }

        // v1.56.274: Spoof Spectate — while spectating, the input target is the
        // view data the server receives.
        // Follow Coordinates (0): replace it with the fake X/Y.
        // Follow Player (1): replace it with the world position of the fake
        // followed player — the spec view "behind" that player.
        if(g_Config.m_KxSpoofSpectate && GameClient()->m_Snap.m_SpecInfo.m_Active)
        {
                CNetObj_PlayerInput *pSpecSend = (CNetObj_PlayerInput *)pData;
                if(g_Config.m_KxSpoofSpectateMode == 0)
                {
                        pSpecSend->m_TargetX = g_Config.m_KxSpoofSpectateX;
                        pSpecSend->m_TargetY = g_Config.m_KxSpoofSpectateY;
                }
                else
                {
                        const int FakeId = g_Config.m_KxSpoofSpectateFollowId;
                        if(FakeId >= 0 && FakeId < MAX_CLIENTS && GameClient()->m_aClients[FakeId].m_Active)
                        {
                                pSpecSend->m_TargetX = (int)GameClient()->m_aClients[FakeId].m_RenderPos.x;
                                pSpecSend->m_TargetY = (int)GameClient()->m_aClients[FakeId].m_RenderPos.y;
                        }
                }
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
        if(g_Config.m_KxFakeAim && s_HaveLastFake && !GameClient()->m_Tas.IsRealPlaying())
        {
                // Figure out whether this tick is a release tick.
                // Real (5) never masks — with the silent channel in action there
                // is no fake to hold, fall through to the real aim render
                // (otherwise the last fake pattern would freeze on screen).
                bool aimOwnedByComponent = silentAimActive && g_Config.m_KxFakeAimMode != 5;
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
        if(fakeActive && !willFire && !hookPress)
        {
                s_LastFakeRenderOffset = fakeOffset;
                s_HaveLastFake = true;
        }

        // v1.56.262: Show Server-Side Aim — render what the server actually sees
        // this tick (the aim after every override: laser unfreeze silent channel,
        // AimBot silent/edge/smooth, Fake Aim, Distance from center). Must run on
        // EVERY tick while enabled: on ticks owned by other components BOTH
        // fakeActive and the local fakeShowForMe stay false (Fake Aim generation
        // is skipped for them), yet the aim SENT is exactly that component's aim —
        // precisely what this toggle must reveal. Gate on the CVAR, not the local.
        if(g_Config.m_KxFakeAim && g_Config.m_KxFakeAimShowServerSide && g_Config.m_KxFakeAimShowForMe)
        {
                m_FakeAimRenderActive = true;
                m_FakeAimRenderOffset = vec2((float)m_aInputData[g_Config.m_ClDummy].m_TargetX,
                        (float)m_aInputData[g_Config.m_ClDummy].m_TargetY);
        }

        // v1.56.263: Fake Aim Predict — pick what OnRender can advance per frame.
        // Real (and fire/hook release ticks, where the sent aim IS the mouse) →
        // the live mouse, rendered exactly like no Fake Aim. Spin → the spin
        // angle advanced by the intra-tick fraction (smooth rotation). Random /
        // Robot / Lag / Chaos hold a constant offset between re-rolls, nothing to
        // predict there. Component-owned ticks (laser/aimbot silent channel) and
        // the TAS replay are never predicted.
        m_FakeAimPredictMode = 0;
        if(g_Config.m_KxFakeAimPredict != 0 && m_FakeAimRenderActive && !GameClient()->m_Tas.IsRealPlaying() &&
                fakeActive)
        {
                if(willFire || hookPress || g_Config.m_KxFakeAimMode == 5) // Real
                        m_FakeAimPredictMode = 1;
                else if(g_Config.m_KxFakeAimMode == 2) // Spin
                {
                        m_FakeAimPredictMode = 2;
                        int speed = g_Config.m_KxFakeAimSpeed;
                        if(speed < 1)
                                speed = 1;
                        if(speed > 100)
                                speed = 100;
                        m_FakeAimSpinStep = (speed / 100.0f) * 0.5f;
                }
        }

        // v1.56.271: re-aim the copy-moves dummies AFTER the fake aim for this
        // tick exists. The copy at the top of SnapInput runs BEFORE the fake
        // aim generation, so the dummy received a one-tick-stale aim together
        // with the FRESH hook/fire — it hooked first and re-aimed after, while
        // the active player aims first and hooks after. Now the dummy gets the
        // same fresh server-visible aim the active player has this tick.
        if(g_Config.m_ClDummyCopyMoves && g_Config.m_KxFakeAim)
        {
                // v1.56.273: Smart aim owns the dummy aim — no fake aim copy at all,
                // no conflict with ApplyCopyMovesTransforms.
                const bool smartOn = (g_Config.m_KxDummyCopyAimMode == 0) && g_Config.m_KxDummyCopySmartAim;
                if(!smartOn)
                {
                        // v1.56.273: mirror the ACTUAL sent aim (pData), not
                        // m_FakeAimRenderOffset. The render offset is render-only: on
                        // release ticks (fire/hook) the mask hold overwrites it with the
                        // STALE held fake, so with Show for me / Show Server-Side Aim off
                        // the dummy hooked with a one-tick-old fake aim again. pData holds
                        // exactly what this tick sends to the server for the active
                        // player.
                        const CNetObj_PlayerInput *pSent = (const CNetObj_PlayerInput *)pData;
                        const int Sx = pSent->m_TargetX;
                        const int Sy = pSent->m_TargetY;
                        const bool filter = g_Config.m_KxDummyCopyMovesFilter != 0;
                        const bool wantMirror = (g_Config.m_KxDummyCopyAimMode == 1) && (g_Config.m_KxDummyCopyMirrorAimX || g_Config.m_KxDummyCopyMirrorAimY);
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                if(D == g_Config.m_ClDummy)
                                        continue;
                                if(D != 0 && !GameClient()->Client()->DummyConnected(D))
                                        continue;
                                if(filter && !g_Config.m_KxDummyCopyMovesFilterAim)
                                        continue;
                                const bool tfHooking = GameClient()->m_BotNet.m_TripleFlyHooking && GameClient()->m_BotNet.m_TripleFlyDummy == D;
                                if(tfHooking)
                                        continue;
                                int Tx = Sx;
                                int Ty = Sy;
                                if(wantMirror)
                                {
                                        if(g_Config.m_KxDummyCopyMirrorAimX)
                                                Tx = -Tx;
                                        if(g_Config.m_KxDummyCopyMirrorAimY)
                                                Ty = -Ty;
                                }
                                GameClient()->m_aDummyInput[D].m_TargetX = Tx;
                                GameClient()->m_aDummyInput[D].m_TargetY = Ty;
                                m_aInputData[D].m_TargetX = Tx;
                                m_aInputData[D].m_TargetY = Ty;
                        }
                }
        }

        return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
		// v1.56.264: Fake Aim Predict — advance the rendered aim between sends.
		// Real (and fire/hook release ticks) → the live mouse, exactly like no
		// Fake Aim. Spin → interpolate between the last two SENT spin angles
		// over the real send interval. Input is sent at a 25 Hz floor, NOT every
		// game tick — extrapolating by IntraGameTick overshot each send interval
		// and snapped back at every tick boundary (sawtooth = jerky rotation).
		// Other modes hold a constant offset between re-rolls, nothing to advance.
		if(m_FakeAimPredictMode == 1)
			m_FakeAimRenderOffset = m_aMousePos[g_Config.m_ClDummy];
		else if(m_FakeAimPredictMode == 2)
		{
			float Frac = (time_get() - m_LastSendTime) / (float)(time_freq() / 25);
			if(Frac < 0.0f)
				Frac = 0.0f;
			if(Frac > 1.0f)
				Frac = 1.0f;
			// Sweep from the previous sent angle to the current one; the next
			// send advances the base by exactly one step → no backward jumps.
			const float Angle = m_FakeAimSpinAngle - m_FakeAimSpinStep + Frac * m_FakeAimSpinStep;
			// Keep the radius the tick actually produced (Spin * AIM_DIST,
			// possibly rescaled by Distance from center), not a hardcoded 200.
			float Radius = length(m_FakeAimRenderOffset);
			if(Radius < 0.5f)
				Radius = 200.0f;
			m_FakeAimRenderOffset = vec2(cosf(Angle), sinf(Angle)) * Radius;
		}

		// v1.56.276: Smooth Predict — glide the rendered server-side aim marker
		// between AimBot Smooth mode's per-tick steps (same idea as the Spin
		// predict above). ApplyAim publishes prev/cur each game tick; here they
		// are lerped over the intra-tick fraction. A publish delayed by one
		// tick clamps to the reached position (no boundary flicker); a stale
		// tick (target lost, smooth off) disables the prediction and the
		// marker falls back to the stepped sent aim.
		{
			CAimBot &SmoothAB = GameClient()->m_AimBot;
			const int MarkerTickDelta = Client()->GameTick(g_Config.m_ClDummy) - SmoothAB.m_SmoothPredictTick;
			// the fake pattern owns the sent aim between shots (Show
			// Server-Side shows it).
			if(MarkerTickDelta >= 0 && MarkerTickDelta <= 1 &&
				g_Config.m_KxFakeAim && g_Config.m_KxFakeAimShowServerSide && g_Config.m_KxFakeAimShowForMe)
			{
				float Frac = Client()->IntraGameTick(g_Config.m_ClDummy) + MarkerTickDelta;
				if(Frac > 1.0f)
					Frac = 1.0f;
				m_FakeAimRenderActive = true;
				m_FakeAimRenderOffset = SmoothAB.m_SmoothPredictPrev + (SmoothAB.m_SmoothPredictCurr - SmoothAB.m_SmoothPredictPrev) * Frac;
			}
		}

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
		// TAS fake world: anchor the cursor to the fake tee, not the real one
		const vec2 CursorAnchor = (GameClient()->m_Tas.IsJoined() && !GameClient()->m_Tas.IsDead()) ? GameClient()->m_Tas.PlaygroundTeeRenderPos() : GameClient()->m_LocalCharacterPos;
		m_aTargetPos[g_Config.m_ClDummy] = CursorAnchor + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
        }
        else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
        {
                m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
        }
        else
        {
                m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
        }

        // v1.56.278: Smooth Predict — glide the VISIBLE crosshair between the
        // per-tick Smooth-mode steps. The glide lands once per game tick in
        // m_aMousePos (that is what made the cursor step at 50 Hz while the
        // weapon angle is snapshot-interpolated and thus looked smooth). The
        // cursor now interpolates prev -> cur over the intra-tick fraction
        // (same trick as the Spin fake-aim predict above); a publish delayed
        // by one tick clamps to the reached position, so it never jumps.
        // Non-silent only: Silent Smooth moves the server-side aim, not the
        // crosshair (that marker is covered by the predict block above).
        {
                CAimBot &ab = GameClient()->m_AimBot;
                const int W = GameClient()->m_Snap.m_pLocalCharacter ? GameClient()->m_Snap.m_pLocalCharacter->m_Weapon : -1;
                const int TickDelta = Client()->GameTick(g_Config.m_ClDummy) - ab.m_SmoothPredictTick;
                if(TickDelta >= 0 && TickDelta <= 1 && W >= 0 && W < CAimBot::NUM_WEAPONS_AIM - 1 &&
                        g_Config.m_KxAimBot && ab.m_AimBotWeapons[W].m_Enabled &&
                        ab.m_AimBotWeapons[W].m_AimMode == 3 && !ab.m_AimBotWeapons[W].m_Silent)
                {
                        float Frac = Client()->IntraGameTick(g_Config.m_ClDummy) + TickDelta;
                        if(Frac > 1.0f)
                                Frac = 1.0f;
                        const vec2 Interp = ab.m_SmoothPredictPrev + (ab.m_SmoothPredictCurr - ab.m_SmoothPredictPrev) * Frac;
                        m_aTargetPos[g_Config.m_ClDummy] += Interp - m_aMousePos[g_Config.m_ClDummy];
                }
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
