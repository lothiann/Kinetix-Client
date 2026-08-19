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
        // m_FreezeStart: if bot is already frozen, set to a recent tick so
        // anti-flicker (m_FreezeStart < GameTick - GameTickSpeed) is FALSE —
        // prevents Freeze() from resetting the timer every tick on freeze tiles.
        // If not frozen, set to 0 so Freeze() works immediately on freeze tile entry.
        Core.m_FreezeStart = (freezeTime > 0) ? 0x7FFFFFFF : 0;
        Core.m_FreezeEnd = 0;
        pBot->SetCore(Core);
        pBot->m_Pos = pos;
        pBot->m_FreezeTime = freezeTime;
        return pBot;
}

// =========================================================
// PfCheckFreeze — exact copy of DDNet freeze-detection logic.
// Source: src/game/client/prediction/entities/character.cpp
//   DDRacePostCoreTick() line 1108-1161 (anti-skip line)
//   HandleTiles()          line 759-807  (freeze tile check)
// This is a 1:1 copy of the engine's logic — no improvements,
// no switch layer, no LFREEZE, no 0.5px sampling. If the engine
// finds freeze on this line, this function finds it too.
//
// Returns true if a freeze tile was found on the line prevPos->curPos
// (or at curPos if the line had no special tiles). Calls pBot->Freeze()
// exactly like HandleTiles does when freeze is found.
// =========================================================
static inline bool PfCheckFreeze(CGameWorld *pWorld, CCharacter *pBot, vec2 prevPos, vec2 curPos)
{
        if(!pBot || !pWorld)
                return false;
        CCollision *pColl = pWorld->Collision();
        if(!pColl)
                return false;

        // m_Core is private in CCharacter — use GetCore() (returns copy) for reads
        // and SetCore() for writes. IsSuper() is a public getter for m_Core.m_Super.
        CCharacterCore Core = pBot->GetCore();
        bool Super = pBot->IsSuper();
        bool Invincible = Core.m_Invincible;
        bool DeepFrozen = Core.m_DeepFrozen;

        // === Exact copy of DDRacePostCoreTick line 1149-1160 ===
        int CurrentIndex = pColl->GetMapIndex(curPos);

        // === Exact copy of HandleTiles line 759-807 (freeze + dfreeze only) ===
        // anti-skip: GetMapIndices(prevPos, curPos) — no MaxIndices
        std::vector<int> vIndices = pColl->GetMapIndices(prevPos, curPos);

        bool foundFreeze = false;
        bool deepFrozenChanged = false;

        if(!vIndices.empty())
        {
                for(int Index : vIndices)
                {
                        int MapIndex = Index;
                        int TileIndex = pColl->GetTileIndex(MapIndex);
                        int TileFIndex = pColl->GetFrontTileIndex(MapIndex);

                        // freeze (HandleTiles line 780-783)
                        if(((TileIndex == TILE_FREEZE) || (TileFIndex == TILE_FREEZE))
                                && !Super && !Invincible && !DeepFrozen)
                        {
                                pBot->Freeze();
                                foundFreeze = true;
                        }
                        // deep freeze (HandleTiles line 790-793)
                        else if(((TileIndex == TILE_DFREEZE) || (TileFIndex == TILE_DFREEZE))
                                && !Super && !Invincible && !DeepFrozen)
                        {
                                DeepFrozen = true;
                                deepFrozenChanged = true;
                        }
                }
        }
        else
        {
                // HandleTiles(CurrentIndex) — line 1159
                int MapIndex = CurrentIndex;
                int TileIndex = pColl->GetTileIndex(MapIndex);
                int TileFIndex = pColl->GetFrontTileIndex(MapIndex);

                if(((TileIndex == TILE_FREEZE) || (TileFIndex == TILE_FREEZE))
                        && !Super && !Invincible && !DeepFrozen)
                {
                        pBot->Freeze();
                        foundFreeze = true;
                }
                else if(((TileIndex == TILE_DFREEZE) || (TileFIndex == TILE_DFREEZE))
                        && !Super && !Invincible && !DeepFrozen)
                {
                        DeepFrozen = true;
                        deepFrozenChanged = true;
                }
        }

        // Write back m_DeepFrozen if it changed (m_Core is private, use SetCore).
        if(deepFrozenChanged)
        {
                Core.m_DeepFrozen = true;
                pBot->SetCore(Core);
        }

        return foundFreeze;
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
        vec2 prevSimPos = pBot->m_Pos; // v1.56.157: for PfCheckFreeze anti-skip line

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

                // v1.56.157: PfCheckFreeze — exact 1:1 copy of DDNet's HandleTiles
                // + DDRacePostCoreTick anti-skip logic. Run AFTER engine Tick() so
                // if the engine's own HandleTiles missed freeze (e.g. due to gate
                // or m_PrevPos issues), our check catches it. Only if bot is not
                // already frozen (avoids double-freeze / anti-flicker conflict).
                if(pBot->m_FreezeTime <= 0)
                {
                        PfCheckFreeze(pWorld, pBot, prevSimPos, pBot->m_Pos);
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
                prevSimPos = pBot->m_Pos; // v1.56.157: update for next tick's anti-skip line

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

// v1.56.108: Per-component line rendering settings.
// Component indices: 0=AimBot 1=TriggerBot 2=Trajectory
//                   3=LaserUnfreeze 4=Pathfinder 5=ESP
enum EKxLineComponent
{
        KX_LINE_AIMBOT = 0,
        KX_LINE_TRIGGERBOT = 1,
        KX_LINE_TRAJECTORY = 2,
        KX_LINE_LASER_UNFREEZE = 3,
        KX_LINE_PATHFINDER = 4,
        KX_LINE_ESP = 5,
        KX_LINE_COUNT = 6,
};

// =========================================================
// v1.56.201: Per-component line rendering rainbow hue state.
// Updated each frame from CKinetixLines::OnRender. Read by KxLineColor().
extern float g_aLineRainbowHue[KX_LINE_COUNT];

// Update rainbow hues for all components. Call once per frame with delta time.
inline void KxLineUpdateRainbow(float dt)
{
        for(int i = 0; i < KX_LINE_COUNT; i++)
        {
                int speed = 60;
                bool enabled = false;
                EKxLineComponent c = (EKxLineComponent)i;
                switch(c)
                {
                case KX_LINE_AIMBOT: enabled = g_Config.m_KxLineAimBotRainbow != 0; speed = g_Config.m_KxLineAimBotRainbowSpeed; break;
                case KX_LINE_TRIGGERBOT: enabled = g_Config.m_KxLineTriggerBotRainbow != 0; speed = g_Config.m_KxLineTriggerBotRainbowSpeed; break;
                case KX_LINE_TRAJECTORY: enabled = g_Config.m_KxLineTrajectoryRainbow != 0; speed = g_Config.m_KxLineTrajectoryRainbowSpeed; break;
                case KX_LINE_LASER_UNFREEZE: enabled = g_Config.m_KxLineLaserUnfreezeRainbow != 0; speed = g_Config.m_KxLineLaserUnfreezeRainbowSpeed; break;
                case KX_LINE_PATHFINDER: enabled = g_Config.m_KxLinePathfinderRainbow != 0; speed = g_Config.m_KxLinePathfinderRainbowSpeed; break;
                case KX_LINE_ESP: enabled = g_Config.m_KxLineEspRainbow != 0; speed = g_Config.m_KxLineEspRainbowSpeed; break;
                }
                // v1.56.204: speed is stored as display*10 (display range 0.1-10.0).
                // To match Rainbow Color: hue += dt * displaySpeed * 60 = dt * (speed/10) * 60 = dt * speed * 6.
                if(enabled)
                {
                        g_aLineRainbowHue[i] += dt * (float)speed * 6.0f;
                        if(g_aLineRainbowHue[i] >= 360.0f)
                                g_aLineRainbowHue[i] -= 360.0f;
                }
        }
}

// v1.56.108: Per-component line rendering settings.
// Layer (KxLineRenderingLayer) stays global — the deferred
// overlay queue (CKinetixLines) is shared infrastructure.
// =========================================================

// Read per-component line rendering settings from g_Config.
// Returns the 3 visual fields (size, color, alpha). Caller
// applies the global Layer check separately.
inline int KxLineSize(EKxLineComponent c)
{
        switch(c)
        {
        case KX_LINE_AIMBOT: return g_Config.m_KxLineAimBotSize;
        case KX_LINE_TRIGGERBOT: return g_Config.m_KxLineTriggerBotSize;
        case KX_LINE_TRAJECTORY: return g_Config.m_KxLineTrajectorySize;
        case KX_LINE_LASER_UNFREEZE: return g_Config.m_KxLineLaserUnfreezeSize;
        case KX_LINE_PATHFINDER: return g_Config.m_KxLinePathfinderSize;
        case KX_LINE_ESP: return g_Config.m_KxLineEspSize;
        }
        return g_Config.m_KxLineRenderingSize; // fallback
}

inline unsigned KxLineColor(EKxLineComponent c)
{
        // v1.56.201: per-component rainbow mode — if enabled, return HSV-rotated color.
        bool rainbow = false;
        int speed = 60;
        switch(c)
        {
        case KX_LINE_AIMBOT: rainbow = g_Config.m_KxLineAimBotRainbow != 0; speed = g_Config.m_KxLineAimBotRainbowSpeed; break;
        case KX_LINE_TRIGGERBOT: rainbow = g_Config.m_KxLineTriggerBotRainbow != 0; speed = g_Config.m_KxLineTriggerBotRainbowSpeed; break;
        case KX_LINE_TRAJECTORY: rainbow = g_Config.m_KxLineTrajectoryRainbow != 0; speed = g_Config.m_KxLineTrajectoryRainbowSpeed; break;
        case KX_LINE_LASER_UNFREEZE: rainbow = g_Config.m_KxLineLaserUnfreezeRainbow != 0; speed = g_Config.m_KxLineLaserUnfreezeRainbowSpeed; break;
        case KX_LINE_PATHFINDER: rainbow = g_Config.m_KxLinePathfinderRainbow != 0; speed = g_Config.m_KxLinePathfinderRainbowSpeed; break;
        case KX_LINE_ESP: rainbow = g_Config.m_KxLineEspRainbow != 0; speed = g_Config.m_KxLineEspRainbowSpeed; break;
        }
        if(rainbow)
        {
                // Convert current hue (0..360) to RGB via HSV with S=1, V=1.
                float h = g_aLineRainbowHue[c] / 60.0f;
                float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
                float r = 0.0f, g = 0.0f, b = 0.0f;
                if(h < 1.0f) { r = 1.0f; g = x; }
                else if(h < 2.0f) { r = x; g = 1.0f; }
                else if(h < 3.0f) { g = 1.0f; b = x; }
                else if(h < 4.0f) { g = x; b = 1.0f; }
                else if(h < 5.0f) { r = x; b = 1.0f; }
                else { r = 1.0f; b = x; }
                unsigned ur = (unsigned)(r * 255.0f);
                unsigned ug = (unsigned)(g * 255.0f);
                unsigned ub = (unsigned)(b * 255.0f);
                return 0xff000000u | (ur << 16) | (ug << 8) | ub;
        }
        switch(c)
        {
        case KX_LINE_AIMBOT: return g_Config.m_KxLineAimBotColor;
        case KX_LINE_TRIGGERBOT: return g_Config.m_KxLineTriggerBotColor;
        case KX_LINE_TRAJECTORY: return g_Config.m_KxLineTrajectoryColor;
        case KX_LINE_LASER_UNFREEZE: return g_Config.m_KxLineLaserUnfreezeColor;
        case KX_LINE_PATHFINDER: return g_Config.m_KxLinePathfinderColor;
        case KX_LINE_ESP: return g_Config.m_KxLineEspColor;
        }
        return g_Config.m_KxLineRenderingColor;
}

inline float KxLineAlpha(EKxLineComponent c)
{
        switch(c)
        {
        case KX_LINE_AIMBOT: return (float)g_Config.m_KxLineAimBotAlpha / 100.0f;
        case KX_LINE_TRIGGERBOT: return (float)g_Config.m_KxLineTriggerBotAlpha / 100.0f;
        case KX_LINE_TRAJECTORY: return (float)g_Config.m_KxLineTrajectoryAlpha / 100.0f;
        case KX_LINE_LASER_UNFREEZE: return (float)g_Config.m_KxLineLaserUnfreezeAlpha / 100.0f;
        case KX_LINE_PATHFINDER: return (float)g_Config.m_KxLinePathfinderAlpha / 100.0f;
        case KX_LINE_ESP: return (float)g_Config.m_KxLineEspAlpha / 100.0f;
        }
        return (float)g_Config.m_KxLineRenderingAlpha / 100.0f;
}

// v1.56.210: Per-segment line color for gradient mode.
// Gradient is visible ONLY when Rainbow is ON. When Gradient is also ON,
// each segment gets hue = baseHue + segment_index * step (wrapped to 0..360).
// Otherwise returns the same color for every segment (current behavior).
//
// segment_index is the 0-based index of the segment in the line strip
// the component is drawing. Callers that draw a single segment can pass 0.
inline unsigned KxLineColorAt(EKxLineComponent c, int segment_index)
{
        bool rainbow = false;
        bool gradient = false;
        int step = 15;
        switch(c)
        {
        case KX_LINE_AIMBOT:
                rainbow = g_Config.m_KxLineAimBotRainbow != 0;
                gradient = g_Config.m_KxLineAimBotGradient != 0;
                step = g_Config.m_KxLineAimBotGradientStep;
                break;
        case KX_LINE_TRIGGERBOT:
                rainbow = g_Config.m_KxLineTriggerBotRainbow != 0;
                gradient = g_Config.m_KxLineTriggerBotGradient != 0;
                step = g_Config.m_KxLineTriggerBotGradientStep;
                break;
        case KX_LINE_TRAJECTORY:
                rainbow = g_Config.m_KxLineTrajectoryRainbow != 0;
                gradient = g_Config.m_KxLineTrajectoryGradient != 0;
                step = g_Config.m_KxLineTrajectoryGradientStep;
                break;
        case KX_LINE_LASER_UNFREEZE:
                rainbow = g_Config.m_KxLineLaserUnfreezeRainbow != 0;
                gradient = g_Config.m_KxLineLaserUnfreezeGradient != 0;
                step = g_Config.m_KxLineLaserUnfreezeGradientStep;
                break;
        case KX_LINE_PATHFINDER:
                rainbow = g_Config.m_KxLinePathfinderRainbow != 0;
                gradient = g_Config.m_KxLinePathfinderGradient != 0;
                step = g_Config.m_KxLinePathfinderGradientStep;
                break;
        case KX_LINE_ESP:
                rainbow = g_Config.m_KxLineEspRainbow != 0;
                gradient = g_Config.m_KxLineEspGradient != 0;
                step = g_Config.m_KxLineEspGradientStep;
                break;
        }

        // No rainbow → use the static per-component color (existing path).
        if(!rainbow)
                return KxLineColor(c);

        // Rainbow ON, Gradient OFF → uniform rainbow color (existing path).
        if(!gradient)
                return KxLineColor(c);

        // Rainbow ON + Gradient ON → per-segment hue offset.
        float hue = g_aLineRainbowHue[c] + (float)segment_index * (float)step;
        hue = std::fmod(hue, 360.0f);
        if(hue < 0.0f)
                hue += 360.0f;

        // Convert hue (0..360) to RGB via HSV with S=1, V=1 (same as KxLineColor).
        float h = hue / 60.0f;
        float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if(h < 1.0f) { r = 1.0f; g = x; }
        else if(h < 2.0f) { r = x; g = 1.0f; }
        else if(h < 3.0f) { g = 1.0f; b = x; }
        else if(h < 4.0f) { g = x; b = 1.0f; }
        else if(h < 5.0f) { r = x; b = 1.0f; }
        else { r = 1.0f; b = x; }
        unsigned ur = (unsigned)(r * 255.0f);
        unsigned ug = (unsigned)(g * 255.0f);
        unsigned ub = (unsigned)(b * 255.0f);
        return 0xff000000u | (ur << 16) | (ug << 8) | ub;
}

#endif // GAME_CLIENT_COMPONENTS_KINETIX_INTERNAL_H
