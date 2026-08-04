#ifndef GAME_CLIENT_COMPONENTS_BOT_CONTROL_H
#define GAME_CLIENT_COMPONENTS_BOT_CONTROL_H

#include <engine/client.h>
#include <engine/console.h>
#include <engine/shared/config.h>

#include <game/client/component.h>

// Per-dummy bot input state
struct CBotDummyState
{
	bool m_Jump;
	bool m_Hook;
	bool m_Fire;
	int m_Direction;

	// Relative aim offset (kx_aim)
	int m_TargetX;
	int m_TargetY;

	// Absolute aim override (kx_oaim)
	int m_OverrideTargetX;
	int m_OverrideTargetY;

	int64_t m_JumpEndTick;
	int64_t m_HookEndTick;
	int64_t m_FireEndTick;
	int64_t m_MoveEndTick;
	int64_t m_AimEndTick;
	int64_t m_OverrideAimEndTick;

	void Reset()
	{
		m_Jump = m_Hook = m_Fire = false;
		m_Direction = 0;
		m_TargetX = m_TargetY = 0;
		m_OverrideTargetX = m_OverrideTargetY = 0;
		m_JumpEndTick = m_HookEndTick = m_FireEndTick = 0;
		m_MoveEndTick = m_AimEndTick = m_OverrideAimEndTick = 0;
	}

	CBotDummyState() { Reset(); }
};

class CBotControl : public CComponent
{
	CBotDummyState m_aState[MAX_DUMMIES];

public:
	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnUpdate() override;
	void OnReset() override;

	// Access per-dummy state
	CBotDummyState &State(int Dummy) { return m_aState[Dummy]; }
	const CBotDummyState &State(int Dummy) const { return m_aState[Dummy]; }

	// Check if any bot action is active for a specific dummy
	bool IsBotActiveJump(int Dummy) const;
	bool IsBotActiveHook(int Dummy) const;
	bool IsBotActiveFire(int Dummy) const;
	bool IsBotActiveMove(int Dummy) const;
	bool IsBotActiveAim(int Dummy) const;
	bool IsBotActiveOverrideAim(int Dummy) const;
	bool IsBotActiveAny(int Dummy) const;

	// Actions — Dummy is the target dummy index (0=main, 1-7=dummies)
	void ActionInput(const char *pName, int MS, int Dummy);
	void ActionAim(int dx, int dy, int Dummy);
	void ActionOverrideAim(int x, int y, int Dummy);
	void ActionStop(int Dummy);

private:
	static void ConInput(IConsole::IResult *pResult, void *pUserData);
	static void ConStop(IConsole::IResult *pResult, void *pUserData);
	static void ConAim(IConsole::IResult *pResult, void *pUserData);
	static void ConOverrideAim(IConsole::IResult *pResult, void *pUserData);
};

#endif
