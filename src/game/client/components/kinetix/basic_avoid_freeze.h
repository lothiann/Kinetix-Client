// (c) Kinetix. Basic Avoid Freeze component — predicts freeze via brute-force
// input combinations and overrides the player's input to avoid it.
//
// Each tick:
//   1. Simulate TriggerTicks ahead with current input. If freeze happens
//      at any point, activate avoidance.
//   2. Brute-force: try all combinations of {direction, jump, hook}:
//        direction: -1, 0, +1 (if KxBafDirection)
//        jump:      0, 1       (if KxBafJump)
//        hook:      0, 1       (if KxBafHook)
//      Total: up to 3*2*2 = 12 combinations.
//      For each, simulate Ticks ticks. Filter out combinations where freeze
//      still occurs at any tick.
//   3. From surviving combinations, pick the one with the smallest diff
//      from the current input (minimal change). Ties broken by first
//      generated (lowest combination index).
//   4. Override the player's input with the chosen combination.
//
// Prediction uses CGameWorld::CopyWorld (same as laser_unfreeze/pathfinder).

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_BASIC_AVOID_FREEZE_H
#define GAME_CLIENT_COMPONENTS_KINETIX_BASIC_AVOID_FREEZE_H

#include <game/client/component.h>

class CBasicAvoidFreeze : public CComponent
{
public:
	CBasicAvoidFreeze() = default;
	~CBasicAvoidFreeze() override = default;

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnUpdate() override;

private:
	// Simulate 'ticks' ticks with the given input on a fresh copy of the
	// predicted world. Returns true if freeze occurs at any tick.
	bool SimulateFreeze(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const;

	// Count how many fields differ between two inputs (for scoring).
	int InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const;
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_BASIC_AVOID_FREEZE_H
