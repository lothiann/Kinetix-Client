#ifndef GAME_CLIENT_COMPONENTS_KINETIX_INTERNAL_H
#define GAME_CLIENT_COMPONENTS_KINETIX_INTERNAL_H

// =========================================================
// kinetix_internal.h - private helpers shared across the
// Kinetix split TUs.  Each kinetix/*.cpp file includes this
// header (in addition to kinetix.h) to get the file-scope
// helpers that used to live at the top of the monolithic
// botnet.cpp.  Behavior is identical to the pre-split code.
// =========================================================

#include "kinetix.h"

#include <game/client/components/kinetix/bot_control.h>

#include <base/color.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/dbg.h>

#include <engine/client.h>
#include <engine/client/enums.h>
#include <engine/graphics.h>
#include <engine/map.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/gameworld.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <cstdint>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <utility> // std::move

// --- Local helpers (DDNet max/min/clamp may not be in scope) ---
static inline int pf_max(int a, int b) { return a > b ? a : b; }
static inline int pf_min(int a, int b) { return a < b ? a : b; }
static inline int pf_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float pf_maxf(float a, float b) { return a > b ? a : b; }
static inline float pf_minf(float a, float b) { return a < b ? a : b; }

// INPUT_STATE_MASK may not be in scope; define locally if missing
#ifndef INPUT_STATE_MASK
#define INPUT_STATE_MASK 3
#endif

// --- Pathfinder heap node ---
struct PfNode
{
        float f; // g + h
        float g; // cost from start
        int r, c; // tile coords
        bool operator>(const PfNode &o) const { return f > o.f; }
};

// =========================================================
// PfChunkResult - shared between pathfinder_astar.cpp and
// pathfinder_tab.cpp (originally file-scope in botnet.cpp).
// =========================================================
// Helper struct for chunk simulation results.
struct PfChunkResult
{
        std::vector<CNetObj_PlayerInput> inputs;
        std::vector<vec2> traj;
        // Per-tick hook segments: tee pos → hook attach/fly pos at each tick where
        // hook is active (FLYING or GRABBED). Captures MULTIPLE hook fire/hold cycles
        // inside one chunk (Advanced search can fire hook on different ticks).
        std::vector<PfHookSeg> hookSegs;
        bool reachedFinish = false;
        bool died = false;
        int freezeTicks = 0;
        int endFreezeTime = 0; // bot's m_FreezeTime at end of chunk (0 = not frozen)
        int endHookState = 0;
        vec2 endHookPos, endHookDir;
        int endHookTick = 0;
        int endJumped = 0;
        int endJumpedTotal = 0;
        vec2 endPos, endVel;
};

// =========================================================
// SetDirection / SetMousePos - input helpers used by
// dummy_helpers.cpp, process_dummy.cpp, kinodynamic.cpp.
// =========================================================
static inline void SetDirection(CGameClient *pGame, int Dummy, bool Left, bool Right)
{
        if(Dummy == g_Config.m_ClDummy)
        {
                pGame->m_Controls.m_aInputDirectionLeft[Dummy] = Left ? 1 : 0;
                pGame->m_Controls.m_aInputDirectionRight[Dummy] = Right ? 1 : 0;
        }
        else
        {
                CNetObj_PlayerInput *pInput = &pGame->m_aDummyInput[Dummy];
                pInput->m_Direction = Right ? 1 : (Left ? -1 : 0);
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
        }
}

static inline void SetMousePos(CGameClient *pGame, int Dummy, vec2 Pos)
{
        pGame->m_Controls.m_aMousePos[Dummy] = Pos;
        CNetObj_PlayerInput *pInput;
        if(Dummy == g_Config.m_ClDummy)
                pInput = &pGame->m_Controls.m_aInputData[Dummy];
        else
                pInput = &pGame->m_aDummyInput[Dummy];
        pInput->m_TargetX = (int)Pos.x;
        pInput->m_TargetY = (int)Pos.y;
        if(Dummy != g_Config.m_ClDummy)
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
}

// =========================================================
// PfSpawnSimBot / PfSimulateChunk - chunk-simulation helpers
// shared between pathfinder_astar.cpp and pathfinder_tab.cpp.
// =========================================================
// Spawn the sim bot at the given state in a cloned world.  The bot keeps its
// real CCharacter identity so DDNet freeze/finish/death logic runs unmodified
// (via OnPredictedInput -> DDNet CCharacter::Tick).
static inline CCharacter *PfSpawnSimBot(CGameWorld *pWorld, int PredictedCharId, const vec2 &pos,
                                 const vec2 &vel, int hookState,
                                 const vec2 &hookPos, const vec2 &hookDir, int hookTick,
                                 int freezeTime, int jumped)
{
        CCharacter *pBot = pWorld->GetCharacterById(PredictedCharId);
        if(!pBot)
                return nullptr;
        CCharacterCore Core = pBot->GetCore();
        Core.m_Pos = pos;
        Core.m_Vel = vel;
        Core.m_HookState = hookState;
        Core.m_HookPos = hookPos;
        Core.m_HookDir = hookDir;
        Core.m_HookTick = hookTick;
        Core.m_Jumped = jumped;
        Core.m_FreezeStart = 0; // reset anti-flicker so Freeze() always works in sim
        pBot->SetCore(Core);
        pBot->m_Pos = pos;
        pBot->m_FreezeTime = freezeTime;
        return pBot;
}

// Simulate one candidate input sequence through the REAL CCharacter in a cloned
// CGameWorld.  Freeze/finish/death are detected by DDNet's own logic, not by
// hand-rolled tile checks.
static inline void PfSimulateChunk(CGameWorld *pWorld, int PredictedCharId, CCharacter *pBot,
                            const std::vector<CNetObj_PlayerInput> &inputs, int StartTick,
                            PfChunkResult &out)
{
        out.inputs = inputs;
        out.traj.clear();
        out.traj.push_back(pBot->m_Pos);
        out.hookSegs.clear();
        out.reachedFinish = false;
        out.died = false;
        out.freezeTicks = 0;
        out.endFreezeTime = 0;

        int prevHookState = pBot->GetCore().m_HookState; // state at start of chunk (before tick 0)

        for(size_t t = 0; t < inputs.size(); t++)
        {
                int Tick = StartTick + (int)t + 1;
                pBot->OnDirectInput(&inputs[t]);
                pWorld->m_GameTick = Tick;
                pBot->OnPredictedInput(&inputs[t]);
                pWorld->Tick();
                if(!pWorld->GetCharacterById(PredictedCharId))
                {
                        out.died = true;
                        break;
                }
                out.traj.push_back(pBot->m_Pos);

                // Record hook segment ONLY at the moment of grab — i.e. when hook state
                // transitions INTO HOOK_GRABBED (prev tick was not GRABBED, this tick is).
                // This gives exactly ONE segment per hook attach, not one per hold tick.
                const CCharacterCore &core = pBot->GetCore();
                if(core.m_HookState == HOOK_GRABBED && prevHookState != HOOK_GRABBED)
                {
                        out.hookSegs.push_back({pBot->m_Pos, core.m_HookPos});
                }
                prevHookState = core.m_HookState;

                if(pBot->m_FreezeTime > 0)
                        out.freezeTicks++;

                // Finish detection: check 5 sample points around the tee using
                // GetTileIndex/GetFrontTileIndex (raw m_Index — NOT GetTile which
                // filters to [SOLID..NOLASER] and would miss TILE_FINISH=34).
                const CCharacterCore &c = pBot->GetCore();
                float r = CCharacterCore::PhysicalSize() / 3.0f;
                const vec2 pts[5] = {
                        c.m_Pos,
                        vec2(c.m_Pos.x + r, c.m_Pos.y - r),
                        vec2(c.m_Pos.x + r, c.m_Pos.y + r),
                        vec2(c.m_Pos.x - r, c.m_Pos.y - r),
                        vec2(c.m_Pos.x - r, c.m_Pos.y + r)};
                CCollision *pColl = pWorld->Collision();
                for(int k = 0; k < 5; k++)
                {
                        int MapIndex = pColl->GetIndex(round_to_int(pts[k].x / 32.0f), round_to_int(pts[k].y / 32.0f));
                        int Tile = pColl->GetTileIndex(MapIndex);
                        int FTile = pColl->GetFrontTileIndex(MapIndex);
                        if(Tile == TILE_FINISH || FTile == TILE_FINISH)
                        {
                                out.reachedFinish = true;
                                break;
                        }
                }
                if(out.reachedFinish)
                        break;
        }

        // Capture final state for direct application (avoids re-simulation).
        out.endFreezeTime = pBot->m_FreezeTime;
        out.endPos = pBot->m_Pos;
        out.endVel = pBot->GetCore().m_Vel;
        out.endHookState = pBot->GetCore().m_HookState;
        out.endHookPos = pBot->GetCore().m_HookPos;
        out.endHookDir = pBot->GetCore().m_HookDir;
        out.endHookTick = pBot->GetCore().m_HookTick;
        out.endJumped = pBot->GetCore().m_Jumped;
        out.endJumpedTotal = pBot->GetCore().m_JumpedTotal;
}

#endif // GAME_CLIENT_COMPONENTS_KINETIX_INTERNAL_H
