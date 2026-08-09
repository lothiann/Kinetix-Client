#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

bool CBotNet::HandleFrozenDummy(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, int LocalID)
{
        // Frozen / deep-frozen dummies can't act.  Optionally kill on freeze,
        // then zero inputs and bail for the rest of the tick.
        if(pGame->m_aClients[LocalID].m_FreezeEnd > 0 || pGame->m_aClients[LocalID].m_DeepFrozen)
        {
                if(g_Config.m_KxKillFrz && pGame->m_aClients[LocalID].m_FreezeEnd > 0)
                        IssueKillForDummy(Dummy);
                ResetAndCommitInput(pGame, pInput, Dummy);
                return true;
        }
        return false;
}

bool CBotNet::HandleNoFeatureIdle(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State)
{
        // Nothing to do this tick — clear inputs and return.
        if(!g_Config.m_KxAttack && !g_Config.m_KxCopyMoves && !g_Config.m_KxRandomAim && !State.m_MacroPlaying && !State.m_PathfinderGoActive)
        {
                ResetAndCommitInput(pGame, pInput, Dummy);
                return true;
        }
        return false;
}

bool CBotNet::ProcessMacroPlayback(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State)
{
        if(!State.m_MacroPlaying)
                return false;

        // Sleep gating: wait until the recorded sleep window elapses.
        if(State.m_MacroSleepTicks > 0)
        {
                int64_t Now = Client()->GameTick(0);
                if(Now >= State.m_MacroSleepUntilTick)
                        State.m_MacroSleepTicks = 0;
                else
                {
                        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                        return true;
                }
        }

        if(State.m_MacroPlayIndex < (int)State.m_MacroPlayLines.size())
        {
                const std::string &line = State.m_MacroPlayLines[State.m_MacroPlayIndex];
                State.m_MacroPlayIndex++;

                std::istringstream iss(line);
                std::string cmd;
                iss >> cmd;

                if(cmd == "sleep")
                {
                        int ms;
                        if(iss >> ms)
                        {
                                int TickSpeed = Client()->GameTickSpeed();
                                if(TickSpeed <= 0)
                                        TickSpeed = 50;
                                State.m_MacroSleepTicks = 1;
                                State.m_MacroSleepUntilTick = Client()->GameTick(0) + (int64_t)ms * TickSpeed / 1000;
                        }
                        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                        return true;
                }
                else if(cmd == "input")
                {
                        std::string action;
                        iss >> action;
                        if(action == "left")
                        {
                                int val = 1;
                                iss >> val;
                                SetDirection(pGame, Dummy, val != 0, false);
                        }
                        else if(action == "right")
                        {
                                int val = 1;
                                iss >> val;
                                SetDirection(pGame, Dummy, false, val != 0);
                        }
                        else if(action == "jump")
                        {
                                int val = 1;
                                iss >> val;
                                pInput->m_Jump = (val != 0) ? 1 : 0;
                        }
                        else if(action == "hook")
                        {
                                int val = 1;
                                iss >> val;
                                pInput->m_Hook = (val != 0) ? 1 : 0;
                        }
                        else if(action == "fire")
                        {
                                pInput->m_Fire++;
                        }
                        else if(action == "weapon")
                        {
                                int val = 1;
                                if(iss >> val)
                                        pInput->m_WantedWeapon = val;
                        }
                }
                else if(cmd == "aim")
                {
                        int x, y;
                        if(iss >> x >> y)
                                SetMousePos(pGame, Dummy, vec2(x, y));
                }
        }
        else
        {
                State.m_MacroPlaying = false;
                SetDirection(pGame, Dummy, false, false);
                ResetAndCommitInput(pGame, pInput, Dummy);
                GameClient()->m_BotControl.ActionStop(Dummy);
                dbg_msg("botnet_macro", "Playback finished.");
        }

        pInput->m_PlayerFlags |= 1;
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
        return true;
}

bool CBotNet::ProcessMacroRecording(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID, int64_t CurTick)
{
        if(!State.m_MacroRecording)
                return false;

        // Emergency stop: prevent unbounded buffer growth.
        if(State.m_MacroRecordBuffer.size() > 100000)
        {
                dbg_msg("botnet_macro", "EMERGENCY STOP: Buffer exceeded 100,000 lines! Stopping recording to prevent crash.");
                State.m_MacroRecording = false;
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                return true;
        }

        int captureID = (State.m_MacroCaptureID >= 0 && State.m_MacroCaptureID < 128) ? State.m_MacroCaptureID : LocalID;

        // Sample current input state — either from the local dummy or from a
        // remote capture target (its snapshot).
        int curDir = 0, curJump = 0, curHook = 0, curFire = 0, curAimX = 0, curAimY = 0, curWeapon = 0;
        if(captureID == LocalID)
        {
                if(pInput->m_Direction == -1)
                        curDir = -1;
                else if(pInput->m_Direction == 1)
                        curDir = 1;
                curJump = (pInput->m_Jump != 0) ? 1 : 0;
                curHook = (pInput->m_Hook != 0) ? 1 : 0;
                curFire = pInput->m_Fire;
                curAimX = pInput->m_TargetX;
                curAimY = pInput->m_TargetY;
                curWeapon = pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Weapon;
        }
        else
        {
                const auto &TChar = pGame->m_Snap.m_aCharacters[captureID];
                if(TChar.m_Active)
                {
                        curDir = TChar.m_Cur.m_Direction;
                        curJump = (TChar.m_Cur.m_Jumped & 1);
                        curHook = (TChar.m_Cur.m_HookState > 0) ? 1 : 0;
                        curFire = TChar.m_Cur.m_AttackTick;
                        curAimX = TChar.m_HasExtendedData ? TChar.m_ExtendedData.m_TargetX : 0;
                        curAimY = TChar.m_HasExtendedData ? TChar.m_ExtendedData.m_TargetY : 0;
                        curWeapon = TChar.m_Cur.m_Weapon;
                }
                else
                {
                        // Capture target went away — bail this tick.
                        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                        return true;
                }
        }

        // Emit delta-encoded lines only when something changed since last sample.
        bool changed = (curDir != State.m_LastRecordedDir) ||
                (curJump != State.m_LastRecordedJump) ||
                (curHook != State.m_LastRecordedHook) ||
                (curAimX != State.m_LastRecordedAimX || curAimY != State.m_LastRecordedAimY) ||
                (curFire != State.m_LastRecordedFire) ||
                (curWeapon != State.m_LastRecordedWeapon);

        if(changed)
        {
                int64_t deltaTick = CurTick - State.m_LastMacroRecordTick;
                int TickSpeed = Client()->GameTickSpeed();
                if(TickSpeed <= 0)
                        TickSpeed = 50;
                int deltaMs = (int)(deltaTick * 1000 / TickSpeed);

                if(deltaMs > 1 && !State.m_MacroRecordBuffer.empty())
                        State.m_MacroRecordBuffer.push_back("sleep " + std::to_string(deltaMs));

                if(curDir != State.m_LastRecordedDir)
                {
                        if(curDir == -1)
                                State.m_MacroRecordBuffer.push_back("input left 1");
                        else if(curDir == 1)
                                State.m_MacroRecordBuffer.push_back("input right 1");
                        else
                                State.m_MacroRecordBuffer.push_back("input left 0");
                }
                if(curJump != State.m_LastRecordedJump)
                        State.m_MacroRecordBuffer.push_back(curJump != 0 ? "input jump 1" : "input jump 0");
                if(curHook != State.m_LastRecordedHook)
                        State.m_MacroRecordBuffer.push_back(curHook != 0 ? "input hook 1" : "input hook 0");
                if(curAimX != State.m_LastRecordedAimX || curAimY != State.m_LastRecordedAimY)
                        State.m_MacroRecordBuffer.push_back("aim " + std::to_string(curAimX) + " " + std::to_string(curAimY));
                if(curFire != State.m_LastRecordedFire)
                        State.m_MacroRecordBuffer.push_back("input fire");
                if(curWeapon != State.m_LastRecordedWeapon)
                        State.m_MacroRecordBuffer.push_back("input weapon " + std::to_string(curWeapon + 1));

                State.m_LastRecordedDir = curDir;
                State.m_LastRecordedJump = curJump;
                State.m_LastRecordedHook = curHook;
                State.m_LastRecordedFire = curFire;
                State.m_LastRecordedAimX = curAimX;
                State.m_LastRecordedAimY = curAimY;
                State.m_LastRecordedWeapon = curWeapon;
                State.m_LastMacroRecordTick = CurTick;
        }
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
        return true;
}

bool CBotNet::ApplyClientDelayGate(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy)
{
        // Throttle dummy actions to one per g_Config.m_KxClientDelay ms (per-dummy timer).
        if(g_Config.m_KxClientDelay <= 0)
                return false;

        static int64_t s_LastActionTick[MAX_DUMMIES] = {};
        int64_t Now = Client()->GameTick(0);
        int64_t TickDelay = ((int64_t)g_Config.m_KxClientDelay * Client()->GameTickSpeed()) / 1000;
        if(TickDelay < 1)
                TickDelay = 1;
        if(Now - s_LastActionTick[Dummy] < TickDelay)
        {
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                return true;
        }
        s_LastActionTick[Dummy] = Now;
        return false;
}

bool CBotNet::ProcessCopyMoves(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy)
{
        // Mirror another client's input onto this dummy.  Falls through (returns
        // false) when the target is valid but inactive — preserves the ability to
        // fall back to Attack/RandomAim.
        if(!(g_Config.m_KxCopyMoves && g_Config.m_KxCopyTargetId >= 0 && g_Config.m_KxCopyTargetId < 128))
                return false;

        const auto &TChar = pGame->m_Snap.m_aCharacters[g_Config.m_KxCopyTargetId];
        if(!(pGame->m_aClients[g_Config.m_KxCopyTargetId].m_Active && TChar.m_Active))
                return false; // fall through

        SetDirection(pGame, Dummy, TChar.m_Cur.m_Direction == -1, TChar.m_Cur.m_Direction == 1);
        pInput->m_Jump = (TChar.m_Cur.m_Jumped & 1);
        pInput->m_Hook = (TChar.m_Cur.m_HookState > 0);
        pInput->m_Fire = TChar.m_Cur.m_AttackTick;
        pInput->m_WantedWeapon = TChar.m_Cur.m_Weapon + 1;
        float WorldX = (float)(TChar.m_HasExtendedData ? TChar.m_ExtendedData.m_TargetX : 0);
        float WorldY = (float)(TChar.m_HasExtendedData ? TChar.m_ExtendedData.m_TargetY : 0);
        GameClient()->m_BotControl.ActionOverrideAim((int)WorldX, (int)WorldY, Dummy);

        pInput->m_PlayerFlags |= 1;
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
        return true;
}

bool CBotNet::ProcessPathfinderGo(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID)
{
        // "Go to position" pathfinder.  Returns true when active (always commits
        // and ends the tick for this dummy).
        if(!State.m_PathfinderGoActive)
                return false;

        vec2 MyPos((float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_X,
                (float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Y);

        int botTX = (int)(MyPos.x / 32.0f);
        int botTY = (int)(MyPos.y / 32.0f);
        int targetTX = (int)(State.m_PathfinderGoPos.x / 32.0f);
        int targetTY = (int)(State.m_PathfinderGoPos.y / 32.0f);

        if(botTX == targetTX && botTY == targetTY)
        {
                State.m_PathfinderGoActive = false;
                SetDirection(pGame, Dummy, false, false);
                pInput->m_Jump = 0;
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                dbg_msg("botnet", "Reached destination");
                return true;
        }

        if(g_Config.m_KxAtkPathfinder)
        {
                // Shared 5s reload slot with Attack-BranchA (legacy behaviour).
                MaybeReloadMapGrid(0);

                if(m_MapGridLoaded)
                {
                        UpdatePlayerPenalty(botTX, botTY, targetTX, targetTY, LocalID);

                        // Recompute the A* field only when target moved, no path yet, or
                        // the bot ended up on an unreachable tile.
                        bool needRecalc = (targetTX != State.m_LastTargetTX || targetTY != State.m_LastTargetTY) || !State.m_PathFound;
                        if(State.m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                        {
                                if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                        needRecalc = true;
                        }

                        if(needRecalc && IsTileWalkable(targetTX, targetTY))
                                ComputePathfinder(botTX, botTY, targetTX, targetTY, State);

                        if(State.m_PathFound && (botTX != State.m_LastBotTX || botTY != State.m_LastBotTY))
                        {
                                ComputeFlowForTile(botTY, botTX, State);
                                State.m_LastBotTX = botTX;
                                State.m_LastBotTY = botTY;
                        }

                        bool left = false, right = false, jump = false;
                        GetMovementFromFlow(State, left, right, jump);

                        SetDirection(pGame, Dummy, left, right);
                        pInput->m_Jump = jump ? 1 : 0;
                        pInput->m_PlayerFlags |= 1;
                }
        }
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
        return true;
}

void CBotNet::ProcessRandomAim(int Dummy, CBotNetDummy &State, int64_t CurTick)
{
        // Fall-through (no return): override aim to a random point on a schedule.
        // Guarded by !g_Config.m_KxAttack && !g_Config.m_KxCopyMoves — not redundant with the
        // no-feature-idle check because CopyMoves may fall through with an inactive
        // target while g_Config.m_KxCopyMoves is still true.
        if(!(g_Config.m_KxRandomAim && !g_Config.m_KxAttack && !g_Config.m_KxCopyMoves))
                return;

        if(CurTick >= State.m_NextRandomAimTick)
        {
                GameClient()->m_BotControl.ActionOverrideAim((rand() % 2001) - 1000, (rand() % 2001) - 1000, Dummy);
                int TS = Client()->GameTickSpeed();
                State.m_NextRandomAimTick = CurTick + (int64_t)(TS != 0 ? TS : 50) * g_Config.m_KxRandomAimInterval / 1000;
        }
}
