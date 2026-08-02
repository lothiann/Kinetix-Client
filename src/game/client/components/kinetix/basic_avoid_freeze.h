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
#include <generated/protocol.h>

class CBasicAvoidFreeze : public CComponent
{
public:
        CBasicAvoidFreeze() = default;
        ~CBasicAvoidFreeze() override = default;

        int Sizeof() const override { return sizeof(*this); }
        void OnReset() override;
        void OnUpdate() override;
        // Called from CControls::SnapInput at the very end (after all other
        // input manipulators: bot control, fake aim, laser unfreeze) so it
        // can override ANY input field including the player's own.
        void ApplyOverride();

private:
        // Returns tick (1-based) when danger first occurs, 0 if none.
        int SimulateDangerTick(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const;
        // First 'delay' ticks with delayInput, then comboInput. Returns danger tick.
        int SimulateDangerTickDelayed(int LocalId, const CNetObj_PlayerInput &DelayInput,
                int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks) const;
        int InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const;

        // Hook release tracking. Set when ApplyOverride forces hook=1; consumed
        // next tick when danger clears (NoHook simulation returns 0) so BAF can
        // release the override hook and let the player's own hook state take over.
        bool m_WasOverriding = false;
        int m_OverrideHook = 0;
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_BASIC_AVOID_FREEZE_H
