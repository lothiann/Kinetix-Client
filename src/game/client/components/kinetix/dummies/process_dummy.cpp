#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

void CBotNet::ProcessDummy(int Dummy)
{
        CGameClient *pGame = GameClient();
        CBotNetDummy &State = m_aDummies[Dummy];
        CNetObj_PlayerInput *pInput = &pGame->m_aDummyInput[Dummy];
        int LocalID = pGame->m_aLocalIds[Dummy];
        int64_t CurTick = Client()->GameTick(0);

        // --- Early-return stages (each handler commits inputs + returns true) ---

        // Frozen dummies can't act — zero inputs.  The pre-freeze plan must not
        // replay after the unfreeze (it was simulated from a state the bot is no
        // longer in), so drop it and let the next tick plan from the real pos.
        if(HandleFrozenDummy(pGame, pInput, Dummy, LocalID))
        {
                State.m_HelpersPlan.Reset();
                return;
        }

        // No feature active — clear inputs and bail.
        if(HandleNoFeatureIdle(pGame, pInput, Dummy, State))
                return;

        // Macro playback (sleep gating + input/aim dispatch).
        if(ProcessMacroPlayback(pGame, pInput, Dummy, State))
                return;

        // Macro recording (delta-encoded input/aim capture, with 100k-line guard).
        if(ProcessMacroRecording(pGame, pInput, Dummy, State, LocalID, CurTick))
                return;

        // Throttle actions to one per g_Config.m_KxClientDelay ms (per-dummy static timer).
        if(ApplyClientDelayGate(pGame, pInput, Dummy))
                return;

        // Mirror another client's input.  Falls through when target inactive.
        if(ProcessCopyMoves(pGame, pInput, Dummy))
                return;

        // "Go to position" pathfinder (A* + flow-field movement toward m_PathfinderGoPos).
        if(ProcessPathfinderGo(pGame, pInput, Dummy, State, LocalID))
                return;

        // Helpers (attack / rescue / escort): target selection, movement,
        // aim/hook/fire.  Implemented in attack/attack.cpp on top of the
        // movement stack in pathfinder/pathfinder.cpp.  The "Dummy IDs"
        // field gates which dummies act as helpers.
        if(g_Config.m_KxHelpers && IsHelpersDummy(Dummy))
                UpdateHelpers(pGame, pInput, Dummy, State, LocalID);

        // Random aim (fall-through — overrides aim on a schedule, does not return).

        // Always copy back at end
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
}
