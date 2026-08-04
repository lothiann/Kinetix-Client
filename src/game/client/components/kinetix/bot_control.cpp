#include "bot_control.h"

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>

void CBotControl::OnConsoleInit()
{
        Console()->Register("kx_input", "s[action] i[ms] ?i[dummy]", CFGFLAG_CLIENT, ConInput, this, "Send input to dummy (action: jump/hook/fire/left/right, ms duration, optional dummy id)");
        Console()->Register("kx_stop", "?i[dummy]", CFGFLAG_CLIENT, ConStop, this, "Stop all bot actions for dummy (optional dummy id, default current)");
        Console()->Register("kx_aim", "i[dx] i[dy] ?i[dummy]", CFGFLAG_CLIENT, ConAim, this, "Relative aim offset for dummy (optional dummy id, default current)");
        Console()->Register("kx_oaim", "i[x] i[y] ?i[dummy]", CFGFLAG_CLIENT, ConOverrideAim, this, "Absolute aim position for dummy (optional dummy id, default current)");
}

void CBotControl::OnReset()
{
        for(int D = 0; D < MAX_DUMMIES; D++)
                m_aState[D].Reset();
}

void CBotControl::OnUpdate()
{
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                CBotDummyState &S = m_aState[D];
                int64_t Cur = Client()->GameTick(D);

                // Clear action flags when duration expires
                if(S.m_JumpEndTick > 0 && Cur >= S.m_JumpEndTick)
                        S.m_Jump = false;
                if(S.m_HookEndTick > 0 && Cur >= S.m_HookEndTick)
                        S.m_Hook = false;
                if(S.m_FireEndTick > 0 && Cur >= S.m_FireEndTick)
                        S.m_Fire = false;
                if(S.m_MoveEndTick > 0 && Cur >= S.m_MoveEndTick)
                        S.m_Direction = 0;

                // Clean up end ticks after they're past
                if(S.m_JumpEndTick > 0 && Cur > S.m_JumpEndTick + 2)
                        S.m_JumpEndTick = 0;
                if(S.m_HookEndTick > 0 && Cur > S.m_HookEndTick + 2)
                        S.m_HookEndTick = 0;
                if(S.m_FireEndTick > 0 && Cur > S.m_FireEndTick + 2)
                        S.m_FireEndTick = 0;
                if(S.m_MoveEndTick > 0 && Cur > S.m_MoveEndTick + 2)
                        S.m_MoveEndTick = 0;

                // Clear relative aim after expiry
                if(S.m_AimEndTick > 0 && Cur > S.m_AimEndTick + 2)
                {
                        S.m_TargetX = 0;
                        S.m_TargetY = 0;
                        S.m_AimEndTick = 0;
                }

                // Clear absolute aim after expiry
                if(S.m_OverrideAimEndTick > 0 && Cur > S.m_OverrideAimEndTick + 2)
                {
                        S.m_OverrideTargetX = 0;
                        S.m_OverrideTargetY = 0;
                        S.m_OverrideAimEndTick = 0;
                }
        }
}

bool CBotControl::IsBotActiveJump(int Dummy) const
{
        const CBotDummyState &S = m_aState[Dummy];
        return S.m_JumpEndTick > 0 && Client()->GameTick(Dummy) <= S.m_JumpEndTick + 1;
}

bool CBotControl::IsBotActiveHook(int Dummy) const
{
        const CBotDummyState &S = m_aState[Dummy];
        return S.m_HookEndTick > 0 && Client()->GameTick(Dummy) <= S.m_HookEndTick + 1;
}

bool CBotControl::IsBotActiveFire(int Dummy) const
{
        const CBotDummyState &S = m_aState[Dummy];
        return S.m_FireEndTick > 0 && Client()->GameTick(Dummy) <= S.m_FireEndTick + 1;
}

bool CBotControl::IsBotActiveMove(int Dummy) const
{
        const CBotDummyState &S = m_aState[Dummy];
        return S.m_MoveEndTick > 0 && Client()->GameTick(Dummy) <= S.m_MoveEndTick + 1;
}

bool CBotControl::IsBotActiveAim(int Dummy) const
{
        const CBotDummyState &S = m_aState[Dummy];
        return S.m_AimEndTick > 0 && Client()->GameTick(Dummy) <= S.m_AimEndTick + 1;
}

bool CBotControl::IsBotActiveOverrideAim(int Dummy) const
{
        const CBotDummyState &S = m_aState[Dummy];
        return S.m_OverrideAimEndTick > 0 && Client()->GameTick(Dummy) <= S.m_OverrideAimEndTick + 1;
}

bool CBotControl::IsBotActiveAny(int Dummy) const
{
        return IsBotActiveJump(Dummy) || IsBotActiveHook(Dummy) || IsBotActiveFire(Dummy) ||
                IsBotActiveMove(Dummy) || IsBotActiveAim(Dummy) || IsBotActiveOverrideAim(Dummy);
}

void CBotControl::ActionInput(const char *pName, int MS, int Dummy)
{
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;

        int TickSpeed = Client()->GameTickSpeed();
        if(TickSpeed <= 0)
                TickSpeed = 50;

        int64_t Cur = Client()->GameTick(Dummy);
        int64_t EndTick = Cur + (int64_t)TickSpeed * MS / 1000;

        CBotDummyState &S = m_aState[Dummy];

        if(str_comp(pName, "jump") == 0)
        {
                S.m_Jump = true;
                S.m_JumpEndTick = EndTick;
        }
        else if(str_comp(pName, "hook") == 0)
        {
                S.m_Hook = true;
                S.m_HookEndTick = EndTick;
        }
        else if(str_comp(pName, "fire") == 0)
        {
                S.m_Fire = true;
                S.m_FireEndTick = EndTick;
        }
        else if(str_comp(pName, "left") == 0)
        {
                S.m_Direction = -1;
                S.m_MoveEndTick = EndTick;
        }
        else if(str_comp(pName, "right") == 0)
        {
                S.m_Direction = 1;
                S.m_MoveEndTick = EndTick;
        }
}

void CBotControl::ActionAim(int dx, int dy, int Dummy)
{
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;

        CBotDummyState &S = m_aState[Dummy];
        S.m_TargetX = dx;
        S.m_TargetY = dy;
        S.m_AimEndTick = Client()->GameTick(Dummy) + 2;
}

void CBotControl::ActionOverrideAim(int x, int y, int Dummy)
{
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;

        CBotDummyState &S = m_aState[Dummy];
        S.m_OverrideTargetX = x;
        S.m_OverrideTargetY = y;
        S.m_OverrideAimEndTick = Client()->GameTick(Dummy) + 2;
}

void CBotControl::ActionStop(int Dummy)
{
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;

        m_aState[Dummy].Reset();

        // Also clear the actual input data so HUD/visuals update immediately,
        // even when the menu/ESC is open and the normal input flow is frozen
        CNetObj_PlayerInput *pInput;
        if(Dummy == g_Config.m_ClDummy)
                pInput = &GameClient()->m_Controls.m_aInputData[Dummy];
        else
                pInput = &GameClient()->m_aDummyInput[Dummy];

        pInput->m_Jump = 0;
        pInput->m_Hook = 0;
        // Simulate fire release (increment so server sees the "let go" transition)
        if((pInput->m_Fire & 1) != 0)
                pInput->m_Fire++;
        pInput->m_Fire &= INPUT_STATE_MASK;
        pInput->m_Direction = 0;
}

// Resolve dummy ID from console argument, defaulting to current g_Config.m_ClDummy
static int ResolveDummyId(IConsole::IResult *pResult, int ArgIndex)
{
        if(pResult->NumArguments() > ArgIndex)
        {
                int D = pResult->GetInteger(ArgIndex);
                if(D >= 0 && D < MAX_DUMMIES)
                        return D;
        }
        return g_Config.m_ClDummy;
}

void CBotControl::ConInput(IConsole::IResult *pResult, void *pUserData)
{
        CBotControl *pSelf = (CBotControl *)pUserData;
        const char *pAction = pResult->GetString(0);
        int MS = pResult->GetInteger(1);
        int Dummy = ResolveDummyId(pResult, 2);
        pSelf->ActionInput(pAction, MS, Dummy);
}

void CBotControl::ConStop(IConsole::IResult *pResult, void *pUserData)
{
        CBotControl *pSelf = (CBotControl *)pUserData;
        int Dummy = ResolveDummyId(pResult, 0);
        pSelf->ActionStop(Dummy);
}

void CBotControl::ConAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotControl *pSelf = (CBotControl *)pUserData;
        int dx = pResult->GetInteger(0);
        int dy = pResult->GetInteger(1);
        int Dummy = ResolveDummyId(pResult, 2);
        pSelf->ActionAim(dx, dy, Dummy);
}

void CBotControl::ConOverrideAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotControl *pSelf = (CBotControl *)pUserData;
        int x = pResult->GetInteger(0);
        int y = pResult->GetInteger(1);
        int Dummy = ResolveDummyId(pResult, 2);
        pSelf->ActionOverrideAim(x, y, Dummy);
}
