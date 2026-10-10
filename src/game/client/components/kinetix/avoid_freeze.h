// (c) Kinetix. Avoid Freeze component — brute force over the input
// combinations, evaluated once per PREDICTED tick (synchronous; the separate
// worker thread was removed completely).
//
// Each tick (per-tick gate via Client()->PredGameTick):
//   1. Danger probes ("will I fall into freeze?") — with the current input
//      (including the user's hook) and without the hook.
//   2. Ticks Before: start acting this many ticks before the real freeze
//      instead of waiting for the very last tick.
//   3. Test A "can I wait 1 more tick?" + Test B "act NOW" (hard-filtered best
//      combo by the score).
//
// The candidate combos: direction x jump x hook x aim, per the AF toggles.

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_AVOID_FREEZE_H
#define GAME_CLIENT_COMPONENTS_KINETIX_AVOID_FREEZE_H

#include <base/vmath.h>

#include <game/client/component.h>
#include <generated/protocol.h>

class CGameWorld;

class CAvoidFreeze : public CComponent
{
public:
	CAvoidFreeze() = default;
	~CAvoidFreeze() override = default;

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnUpdate() override;
	// Called from CControls::SnapInput before the input is sent (after all
	// other input manipulators) so it can override ANY input field.
	void ApplyOverride();

private:
	// Returns tick (1-based) when danger first occurs, 0 if none.
	static int DangerTick(CGameWorld *pWorld, int LocalId, const CNetObj_PlayerInput &Input, int Ticks);
	// First 'Delay' ticks with DelayInput, then ComboInput. Returns danger tick.
	static int DangerTickDelayed(CGameWorld *pWorld, int LocalId, const CNetObj_PlayerInput &DelayInput,
		int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks);
	// Position after ONE tick of the given input.
	static vec2 NextTickPos(CGameWorld *pWorld, int LocalId, const CNetObj_PlayerInput &Input);
	static int InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B);

	// Hook release tracking. Set when ApplyOverride forces hook=1; consumed
	// next tick when danger clears so AF can release the override hook.
	bool m_WasOverriding = false;
	int m_OverrideHook = 0;

	// Per-tick gate: the last predicted tick this component evaluated.
	int m_LastEvalTick = -1;
	// Last evaluated raw input — the gate also re-runs when it changes, so a
	// player's keypress between ticks is reacted to immediately.
	CNetObj_PlayerInput m_LastEvalInput{};
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_AVOID_FREEZE_H