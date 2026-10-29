#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

bool CBotNet::HandleFrozenDummy(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, int LocalID)
{
        // Frozen / deep-frozen dummies can't act — zero inputs and bail for
        // the rest of the tick. v1.56.258: Kill On Freeze removed.
        if(pGame->m_aClients[LocalID].m_FreezeEnd > 0 || pGame->m_aClients[LocalID].m_DeepFrozen)
        {
                ResetAndCommitInput(pGame, pInput, Dummy);
                return true;
        }
        return false;
}

bool CBotNet::HandleNoFeatureIdle(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State)
{
        // Nothing to do this tick — clear inputs and return.
        if(!g_Config.m_KxHelpers && !g_Config.m_KxCopyMoves && !State.m_MacroPlaying && !State.m_PathfinderGoActive)
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
        // fall back to Attack.
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

// v6.7: shared dummy filter check — same semantics as CEsp::PassesFiltersImpl
// (team / DDrace team / friend / clanmate / targets / freeze), evaluated on the
// dummy's server client. No Dummy filter: the candidates ARE our own dummies.
bool CBotNet::DummyPassesFilters(int Dummy, int teamFilter, int ddraceTeamFilter, int ddraceTeamId,
        int friendFilter, int clanmateFilter, int targetsFilterMode, const char *pTargetsIds, int freezeFilter) const
{
        CGameClient *pGame = GameClient();
        const int Cid = pGame->m_aLocalIds[Dummy];
        // v6.9: only require the player info (always snapped). The character
        // object itself is dropped from the snapshot when it is outside the
        // active tee's view (CCharacter::IsSnappingCharacterInView), so a far /
        // off-screen dummy must still be a valid switch candidate — SDS has to
        // reach dummies in the state where they are unloaded.
        if(Cid < 0 || Cid >= 128 || !pGame->m_aClients[Cid].m_Active)
                return false;
        const int LocalId = pGame->m_aLocalIds[g_Config.m_ClDummy];
        if(LocalId < 0 || LocalId >= 128)
                return false;

        // Team filter: 0=both 1=my 2=war 3=blue 4=red ("Team" = REAL team).
        const int MyRealTeam = pGame->m_aClients[LocalId].m_Team;
        if(teamFilter == 1)
        {
                if(pGame->m_aClients[Cid].m_Team != MyRealTeam)
                        return false;
        }
        else if(teamFilter == 2)
        {
                if(pGame->m_aClients[Cid].m_Team == MyRealTeam)
                        return false;
        }
        else if(teamFilter == 3)
        {
                if(pGame->m_aClients[Cid].m_Team != TEAM_BLUE)
                        return false;
        }
        else if(teamFilter == 4)
        {
                if(pGame->m_aClients[Cid].m_Team != TEAM_RED)
                        return false;
        }
        // DDrace team filter: 0=both 1=my 2=other 3=target id.
        if(ddraceTeamFilter == 1 && !pGame->m_Teams.SameTeam(LocalId, Cid))
                return false;
        if(ddraceTeamFilter == 2 && pGame->m_Teams.SameTeam(LocalId, Cid))
                return false;
        if(ddraceTeamFilter == 3 && pGame->m_Teams.Team(Cid) != ddraceTeamId)
                return false;
        // Friend filter: 0=both 1=ignore 2=only.
        if(friendFilter == 1 && pGame->m_aClients[Cid].m_Friend)
                return false;
        if(friendFilter == 2 && !pGame->m_aClients[Cid].m_Friend)
                return false;
        // Clanmate filter: 0=both 1=ignore 2=only.
        if(clanmateFilter == 1 && str_comp(pGame->m_aClients[Cid].m_aClan, pGame->m_aClients[LocalId].m_aClan) == 0)
                return false;
        if(clanmateFilter == 2 && str_comp(pGame->m_aClients[Cid].m_aClan, pGame->m_aClients[LocalId].m_aClan) != 0)
                return false;
        // Targets filter: 0=both 1=only 2=ignore (by server client id).
        if(targetsFilterMode != 0)
        {
                bool found = false;
                bool anyValid = false;
                const char *pIds = pTargetsIds;
                while(*pIds)
                {
                        while(*pIds && (*pIds == ' ' || *pIds == ',' || *pIds == ';'))
                                pIds++;
                        if(!*pIds)
                                break;
                        if(*pIds >= '0' && *pIds <= '9')
                        {
                                const int id = atoi(pIds);
                                if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
                                {
                                        anyValid = true;
                                        if(id == Cid)
                                                found = true;
                                }
                        }
                        while(*pIds && *pIds != ' ' && *pIds != ',' && *pIds != ';')
                                pIds++;
                }
                if(targetsFilterMode == 1 && anyValid && !found)
                        return false;
                if(targetsFilterMode == 2 && anyValid && found)
                        return false;
        }
        // Freeze filter: 0=both 1=freeze 2=no_freeze. The freeze state only
        // lives in the character object, which is missing while the dummy is
        // outside the active tee's view. Unknown state counts as not frozen, so
        // an unloaded dummy never passes the "freeze only" filter.
        const bool CharLoaded = pGame->m_Snap.m_aCharacters[Cid].m_Active;
        const bool isFrozen = CharLoaded && (pGame->m_aClients[Cid].m_FreezeEnd != 0 || pGame->m_aClients[Cid].m_LiveFrozen);
        if(freezeFilter == 1 && !isFrozen)
                return false;
        if(freezeFilter == 2 && isFrozen)
                return false;
        return true;
}

void CBotNet::SdsResetVisited()
{
        m_SdsVisitedCount = 0;
}

// v6.7: Smart Copy Moves — rebuild the per-dummy allow list: among the
// filter-passing candidates (main slot or connected dummy, not the active
// one), the top-N by the selected sort metric receives copy moves.
void CBotNet::SmartCopyRecompute()
{
        for(int D = 0; D < MAX_DUMMIES; D++)
                m_aSmartCopyAllow[D] = false;
        if(!g_Config.m_KxSmartCopyMoves || !g_Config.m_ClDummyCopyMoves)
                return;

        CGameClient *pGame = GameClient();
        const int Cur = g_Config.m_ClDummy;
        const int Order = g_Config.m_KxSmartCopySort;
        const int activeId = pGame->m_aLocalIds[Cur];

        vec2 ActivePos(0.0f, 0.0f);
        vec2 AimPoint(0.0f, 0.0f);
        const bool haveRef = activeId >= 0 && activeId < 128 && pGame->m_aClients[activeId].m_Active;
        if(haveRef)
        {
                ActivePos = pGame->m_aClients[activeId].m_Predicted.m_Pos;
                // v6.7: zoom-accurate world aim point — same conversion as the
                // spawner cursor (tas.cpp SpawnerTargetPos): m_aTargetPos is stored
                // in unzoomed camera space, so the world point under the crosshair
                // is cam + (target - cam) * zoom.
                const CNetObj_PlayerInput &In = pGame->m_Controls.m_aInputData[Cur];
                const vec2 Cam = pGame->m_Camera.m_Center;
                const float Zoom = pGame->m_Camera.m_Zoom;
                if(Zoom > 0.001f)
                        AimPoint = Cam + (pGame->m_Controls.m_aTargetPos[Cur] - Cam) * Zoom;
                else
                        AimPoint = ActivePos + vec2((float)In.m_TargetX, (float)In.m_TargetY);
        }

        int aCand[MAX_DUMMIES];
        long long aKey[MAX_DUMMIES];
        int NumCand = 0;
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                if(D == Cur)
                        continue;
                if(!(D == 0 || Client()->DummyConnected(D)))
                        continue;

                long long Key = (long long)D;
                const int Cid = pGame->m_aLocalIds[D];
                // Position of the candidate dummy — from the active snapshot when
                // the character is in view, otherwise from the dummy's own
                // connection snapshot (the server always snaps the snapping
                // client's own character).
                bool HavePos = false;
                vec2 DummyPos(0.0f, 0.0f);
                if(Cid >= 0 && Cid < 128)
                {
                        if(pGame->m_Snap.m_aCharacters[Cid].m_Active)
                        {
                                DummyPos = pGame->m_aClients[Cid].m_Predicted.m_Pos;
                                HavePos = true;
                        }
                        else
                        {
                                const CNetObj_Character *pChar = (const CNetObj_Character *)Client()->SnapFindItemConn(
                                        D, IClient::SNAP_CURRENT, NETOBJTYPE_CHARACTER, Cid);
                                if(pChar)
                                {
                                        DummyPos = vec2((float)pChar->m_X, (float)pChar->m_Y);
                                        HavePos = true;
                                }
                        }
                }
                if(Order == 3)
                {
                        // Last Active: most recently active first, never-active last.
                        Key = -(long long)(m_aDummyActiveSeq[D] + 1);
                }
                else if(Order != 0)
                {
                        if(haveRef && HavePos)
                        {
                                if(Order == 1)
                                {
                                        const vec2 d = DummyPos - ActivePos;
                                        Key = (long long)(d.x * d.x + d.y * d.y);
                                }
                                else // Nearest To Aim
                                {
                                        const vec2 d = DummyPos - AimPoint;
                                        Key = (long long)(d.x * d.x + d.y * d.y);
                                }
                        }
                        else
                        {
                                // No character at all (dead / not spawned yet):
                                // keep it after every positioned candidate.
                                Key = 0x4000000000000000LL;
                        }
                }

                if(!DummyPassesFilters(D, g_Config.m_KxSmartCopyTeam, g_Config.m_KxSmartCopyDdraceTeam,
                        g_Config.m_KxSmartCopyDdraceTeamId, g_Config.m_KxSmartCopyFriend,
                        g_Config.m_KxSmartCopyClanmate, g_Config.m_KxSmartCopyTargetsMode,
                        g_Config.m_KxSmartCopyTargets, g_Config.m_KxSmartCopyFreeze))
                        continue;

                aCand[NumCand] = D;
                aKey[NumCand] = Key;
                NumCand++;
        }

        for(int i = 1; i < NumCand; i++)
        {
                const int CD = aCand[i];
                const long long CK = aKey[i];
                int j = i;
                while(j > 0 && (aKey[j - 1] > CK || (aKey[j - 1] == CK && aCand[j - 1] > CD)))
                {
                        aKey[j] = aKey[j - 1];
                        aCand[j] = aCand[j - 1];
                        j--;
                }
                aKey[j] = CK;
                aCand[j] = CD;
        }

        // Top-N, clamped by the Dummy Manager limit (incl. main → N ≤ limit-1).
        int Count = g_Config.m_KxSmartCopyCount;
        const int Limit = Client()->DummyLimit();
        if(Limit >= 1 && Count > Limit - 1)
                Count = Limit - 1;
        if(Count < 1)
                Count = 1;
        for(int i = 0; i < NumCand && i < Count; i++)
                m_aSmartCopyAllow[aCand[i]] = true;
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

        if(true) // v1.56.258: grid flow always used for PathfinderGo (KxAtkPathfinder removed)
        {
                // Shared 5s reload slot with Attack-BranchA (legacy behaviour).
                MaybeReloadMapGrid(0);

                if(m_MapGridLoaded)
                {
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

// v1.56.260: ProcessRandomAim removed (Random Aim was an old test feature).
