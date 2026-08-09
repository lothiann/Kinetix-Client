#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// =========================================================
// INTERNAL HELPERS (v1.56.32 refactor)
// Extracted from ProcessDummy + Compute*Pathfinder to kill duplication.
// Behavior is byte-for-byte identical to the inlined originals.
// =========================================================

void CBotNet::ResetAndCommitInput(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy)
{
        // Zero movement inputs, advance the fire-release bit so a held fire
        // doesn't get stuck, mask fire to the valid 2-bit state.
        pInput->m_Direction = 0;
        pInput->m_Jump = 0;
        pInput->m_Hook = 0;
        if((pInput->m_Fire & 1) != 0)
                pInput->m_Fire++;
        pInput->m_Fire &= INPUT_STATE_MASK;
        // REMOVED: pGame->m_Controls.m_aInputData[Dummy] = *pInput;
        // This line clobbered m_aInputData[D] (which holds the player's last sent
        // input for non-active slots, set by copy_moves/dummy_control in controls.cpp
        // SnapInput) with the dummy's modified input. When OnDummySwap later saves
        // m_aInputData[D] → m_aDummyInput[D] on the next cl_dummy swap, it saved
        // the CLOBBERED value, losing the player's weapon state (m_WantedWeapon).
        // m_aInputData[D] for non-active D should only be written by:
        //   - controls.cpp SnapInput (copy_moves/dummy_control/bot_control blocks)
        //   - gameclient.cpp OnDummySwap (restore path — but only m_Fire now)
}

void CBotNet::IssueKillForDummy(int Dummy)
{
        // "kill" must run on the right dummy slot; temporarily switch ClDummy.
        int OldDummy = g_Config.m_ClDummy;
        g_Config.m_ClDummy = Dummy;
        Console()->ExecuteLine("kill", -1, -1);
        g_Config.m_ClDummy = OldDummy;
}

bool CBotNet::MaybeReloadMapGrid(int reloadSlot)
{
        // reloadSlot 0 is shared between PathfinderGo and Attack-BranchA (legacy
        // behaviour: a single 5s static timer).  reloadSlot 1 is BranchB's own
        // independent timer.  Two slots => two statics => same semantics as before.
        static int64_t s_LastReload[2] = {0, 0};

        bool mapChanged = false;
        if(m_MapGridLoaded)
        {
                CLayers *pLayers = GameClient()->Layers();
                if(pLayers && pLayers->GameLayer())
                {
                        int curW = pLayers->GameLayer()->m_Width;
                        int curH = pLayers->GameLayer()->m_Height;
                        if(curW != m_MapWidth || curH != m_MapHeight)
                                mapChanged = true;
                }
        }

        int64_t now = time_get();
        if(now - s_LastReload[reloadSlot] > time_freq() * 5)
        {
                mapChanged = true;
                s_LastReload[reloadSlot] = now;
        }

        if(!m_MapGridLoaded || mapChanged)
        {
                LoadMapGrid();
                return true;
        }
        return false;
}

float CBotNet::EffectiveStandDist(bool TargetIsMain, bool TargetIsRescue) const
{
        // When targeting the main player and not in attack-main mode (and not a
        // rescue target), use the larger g_Config.m_KxMainStandDist so the bot keeps some
        // distance; otherwise g_Config.m_KxStandDist.
        return (TargetIsMain && !g_Config.m_KxAtkMain && !TargetIsRescue) ? (float)g_Config.m_KxMainStandDist : (float)g_Config.m_KxStandDist;
}

bool CBotNet::IsNearFreeze(int botTX, int botTY) const
{
        // ComputeFreezeRepel returns a unit-ish vec2 pointing away from nearby
        // freeze tiles; nonzero => at least one freeze neighbour is exerting force.
        if(!g_Config.m_KxAvoidFreeze || !m_MapGridLoaded)
                return false;
        vec2 repel = const_cast<CBotNet *>(this)->ComputeFreezeRepel(botTX, botTY);
        return (repel.x * repel.x + repel.y * repel.y) > 0.0001f;
}

void CBotNet::ApplyPfSnap(const vec2 &MyPos, bool &left, bool &right) const
{
        // When no horizontal movement is requested, nudge toward the current
        // tile's X center so the bot rides the tile grid cleanly (PfSnap option).
        int snapTX = (int)roundf(MyPos.x / 32.0f);
        float tileCenterX = snapTX * 32.0f + 16.0f;
        float offsetX = MyPos.x - tileCenterX;
        if(offsetX > 0)
                left = true;
        else if(offsetX < 0)
                right = true;
}

int CBotNet::ComputeHookTicksCycle() const
{
        // Hook pulse period in ticks, derived from g_Config.m_KxAtkHookDelay (ms).  Min 2 ticks.
        int cycle = (50 * g_Config.m_KxAtkHookDelay / 1000);
        return cycle < 2 ? 2 : cycle;
}

// =========================================================
// ID LIST SYNC HELPERS
// =========================================================

void CBotNet::SyncIDsToStr(const bool *pList, char *pStr, int StrSize)
{
        pStr[0] = '\0';
        bool first = true;
        for(int i = 0; i < 128; i++)
        {
                if(pList[i])
                {
                        char aBuf[16];
                        str_format(aBuf, sizeof(aBuf), first ? "%d" : ",%d", i);
                        str_append(pStr, aBuf, StrSize);
                        first = false;
                }
        }
}

void CBotNet::SyncStrToIDs(const char *pStr, bool *pList)
{
        for(int i = 0; i < 128; i++)
                pList[i] = false;
        if(!pStr || !pStr[0])
                return;
        char aBuf[256];
        str_copy(aBuf, pStr, sizeof(aBuf));
        char *pC = aBuf;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        pList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}
