/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
        float GetMinMouseDistance() const;
        float GetMaxMouseDistance() const;

        enum class EMouseInputType
        {
                ABSOLUTE,
                RELATIVE,
                AUTOMATED,
        };

        vec2 m_aMousePos[MAX_DUMMIES];
        vec2 m_aMousePosOnAction[MAX_DUMMIES];
        vec2 m_aTargetPos[MAX_DUMMIES];

        EMouseInputType m_aMouseInputType[MAX_DUMMIES];

        int m_aAmmoCount[NUM_WEAPONS];

        int64_t m_LastSendTime;
        CNetObj_PlayerInput m_aInputData[MAX_DUMMIES];
        CNetObj_PlayerInput m_aLastData[MAX_DUMMIES];
        int m_aInputDirectionLeft[MAX_DUMMIES];
        int m_aInputDirectionRight[MAX_DUMMIES];
        int m_aShowHookColl[MAX_DUMMIES];

        // Laser unfreeze silent aim — when active, SnapInput uses this offset
        // instead of m_aMousePos for m_TargetX/Y. Set by UpdateLaserUnfreeze
        // when silent mode is on. Consumed (reset to false) by SnapInput.
        bool m_SilentAimActive = false;
        vec2 m_SilentAimOffset = vec2(0, 0);

        // Fake Aim render data — set in SnapInput, read in players.cpp.
        bool m_FakeAimRenderActive = false;
        vec2 m_FakeAimRenderOffset = vec2(0, 0);

        // v1.56.263: Fake Aim Predict — per-frame prediction of the rendered aim
        // (see SnapInput / OnRender): 0 = off, 1 = live mouse (Real mode and
        // fire/hook release ticks), 2 = Spin angle advanced by the intra-tick
        // fraction. The authoritative spin state lives here so SnapInput and
        // OnRender share it.
        int m_FakeAimPredictMode = 0;
        float m_FakeAimSpinAngle = 0.0f;
        float m_FakeAimSpinStep = 0.0f;

        CControls();
        int Sizeof() const override { return sizeof(*this); }

        void OnReset() override;
        void OnRender() override;
        void OnMessage(int MsgType, void *pRawMsg) override;
        bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
        void OnConsoleInit() override;
        virtual void OnPlayerDeath();

        int SnapInput(int *pData);
        void ClampMousePos();
        void ResetInput(int Dummy);

private:
        static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
        static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
        static void ConKeyInputPfEditFire(IConsole::IResult *pResult, void *pUserData);
        static void ConKeyInputPfEditHook(IConsole::IResult *pResult, void *pUserData);
        static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
        static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
