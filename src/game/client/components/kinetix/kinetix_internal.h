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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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
        int forbiddenTicks = 0;
        int ticksSimulated = 0; // v6.15: actual ticks executed (chunk early exits)
        int endFreezeTime = 0; // bot's m_FreezeTime at end of chunk (0 = not frozen)
        int endHookState = 0;
        vec2 endHookPos, endHookDir;
        int endHookTick = 0;
        int endJumped = 0;
        int endJumpedTotal = 0;
        vec2 endPos, endVel;
};

// =========================================================
// Dynamic hook targets (Pathfinder tab) — 36 angular segments over
// 360°. A segment stores the best (closest) validated hook target of
// a 90-ray fan. AimPos points 0.75px INSIDE the hit tile so aiming at
// it deterministically lands the ray in that same tile. Targets are
// cached per spatial cell and re-validated against their tile on
// every use; dead or empty segments are re-scanned with 3 probes.
// =========================================================
static inline bool PfHookHitHookable(int Hit)
{
	return Hit == TILE_SOLID || Hit == TILE_TELEINHOOK || (Hit > 0 && Hit != TILE_NOHOOK && Hit != TILE_DEATH);
}

static inline int PfHookSegIndex(float Angle)
{
	int Seg = (int)floorf((Angle + pi) / (2.0f * pi) * 36.0f);
	Seg %= 36;
	if(Seg < 0)
		Seg += 36;
	return Seg;
}

static inline void PfHookTargetSet(PfHookTarget &Out, const vec2 &Origin, const vec2 &Dir, const vec2 &HitPos)
{
	Out.Valid = true;
	Out.SurfacePos = HitPos;
	Out.AimPos = HitPos + Dir * 0.75f;
	Out.TileX = (int)floorf(Out.AimPos.x / 32.0f);
	Out.TileY = (int)floorf(Out.AimPos.y / 32.0f);
	Out.Dist = distance(Origin, HitPos);
}

// Cast one hook ray; returns false when nothing hookable / too close.
static inline bool PfCastHookRay(const CCollision *pColl, const vec2 &Origin, float Angle, float HookLength, PfHookTarget &Out)
{
	const vec2 Dir(cosf(Angle), sinf(Angle));
	vec2 HitPos;
	int Hit = pColl->IntersectLineTeleHook(Origin, Origin + Dir * HookLength, &HitPos, nullptr);
	if(!PfHookHitHookable(Hit))
		return false;
	if(distance(Origin, HitPos) < 24.0f)
		return false; // floor under the feet / wall in the face
	PfHookTargetSet(Out, Origin, Dir, HitPos);
	return true;
}

// Build the segment cache: 90 rays over the full 360° circle (4° step),
// offset by half a step so a cardinal direction never sits exactly on
// a segment border; each segment keeps its closest hit.
static inline void PfBuildHookTargets(const CCollision *pColl, const vec2 &Origin, float HookLength, PfHookTargets &Out)
{
	Out.Built = true;
	Out.Origin = Origin;
	Out.HookLength = HookLength;
	for(int s = 0; s < 36; s++)
		Out.aSeg[s] = PfHookTarget();
	const int NumRays = 90;
	for(int i = 0; i < NumRays; i++)
	{
		const float Angle = -pi + ((float)i + 0.5f) * (2.0f * pi / (float)NumRays);
		PfHookTarget Ray;
		if(!PfCastHookRay(pColl, Origin, Angle, HookLength, Ray))
			continue;
		const int Seg = PfHookSegIndex(Angle);
		PfHookTarget &Slot = Out.aSeg[Seg];
		if(!Slot.Valid || Ray.Dist < Slot.Dist)
			Slot = Ray;
	}
}

// Re-cast a single segment with 3 rays spread across its 10° arc.
static inline void PfRescanHookSeg(const CCollision *pColl, const vec2 &Origin, float HookLength, int Seg, PfHookTarget &Out)
{
	Out.Valid = false;
	static const float s_aFrac[3] = {1.0f / 6.0f, 0.5f, 5.0f / 6.0f};
	const float SegArc = 2.0f * pi / 36.0f;
	for(int f = 0; f < 3; f++)
	{
		const float Angle = -pi + ((float)Seg + s_aFrac[f]) * SegArc;
		PfHookTarget Ray;
		if(!PfCastHookRay(pColl, Origin, Angle, HookLength, Ray))
			continue;
		if(!Out.Valid || Ray.Dist < Out.Dist)
			Out = Ray;
	}
}

// A cached target is alive while the ray from the current origin to its
// stored aim still lands hookable INSIDE the stored tile. Re-aims at the
// current surface point (same tile) so the aim follows the geometry.
static inline bool PfHookTargetAlive(const CCollision *pColl, const vec2 &Origin, float HookLength, PfHookTarget &Target)
{
	if(!Target.Valid)
		return false;
	const vec2 Aim = Target.AimPos - Origin;
	const float AimLen = length(Aim);
	if(AimLen < 1.0f || AimLen > HookLength + 1.0f)
		return false;
	vec2 HitPos;
	int Hit = pColl->IntersectLineTeleHook(Origin, Target.AimPos, &HitPos, nullptr);
	if(!PfHookHitHookable(Hit))
		return false;
	if(distance(Origin, HitPos) < 24.0f)
		return false;
	const vec2 Dir = Aim / AimLen;
	vec2 NewAim = HitPos + Dir * 0.75f;
	if((int)floorf(NewAim.x / 32.0f) != Target.TileX || (int)floorf(NewAim.y / 32.0f) != Target.TileY)
		return false;
	Target.SurfacePos = HitPos;
	Target.AimPos = NewAim;
	Target.Dist = distance(Origin, HitPos);
	return true;
}

// =========================================================
// SetDirection / SetMousePos - input helpers used by
// dummy_helpers.cpp, process_dummy.cpp, pathfinder.cpp.
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
static inline bool PfCheckFreeze(CGameWorld *pWorld, CCharacter *pBot, vec2 prevPos, vec2 curPos, bool FastSim = false)
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
        // anti-skip: GetMapIndices(prevPos, curPos) — no MaxIndices.
        // FastSim: identical traversal/order into a stack buffer (no per-tick
        // heap allocation); on the (impossible in practice) buffer overflow
        // the vector version keeps the check exact.
        int aIndices[192];
        int NumIndices = 0;
        const int *pIndices = nullptr;
        std::vector<int> vIndices;
        if(FastSim)
        {
                NumIndices = pColl->GetMapIndicesBuf(prevPos, curPos, aIndices, (int)(sizeof(aIndices) / sizeof(aIndices[0])));
                if(NumIndices < 0)
                {
                        FastSim = false;
                        NumIndices = 0;
                }
                else
                {
                        pIndices = aIndices;
                }
        }
        if(!FastSim)
        {
                vIndices = pColl->GetMapIndices(prevPos, curPos);
                pIndices = vIndices.data();
                NumIndices = (int)vIndices.size();
        }

        bool foundFreeze = false;
        bool deepFrozenChanged = false;

        if(NumIndices > 0)
        {
                for(int Idx = 0; Idx < NumIndices; Idx++)
                {
                        int MapIndex = pIndices[Idx];
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

static inline bool PfLineTouchesForbidden(const unsigned char *pForbiddenGrid, int GridW, int GridH, vec2 From, vec2 To)
{
        if(!pForbiddenGrid)
                return false;
        vec2 Delta = To - From;
        float Len = length(Delta);
        int Steps = (int)(Len / 4.0f) + 1;
        for(int i = 0; i <= Steps; i++)
        {
                float t = (float)i / (float)Steps;
                vec2 p = From + Delta * t;
                int tx = (int)floorf(p.x / 32.0f);
                int ty = (int)floorf(p.y / 32.0f);
                if(tx < 0 || ty < 0 || tx >= GridW || ty >= GridH)
                        continue;
                if(pForbiddenGrid[ty * GridW + tx])
                        return true;
        }
        return false;
}

// Simulate one candidate input sequence through the REAL CCharacter in a cloned
// CGameWorld.  Freeze/finish/death are detected by DDNet's own logic, not by
// hand-rolled tile checks.
static inline void PfSimulateChunk(CGameWorld *pWorld, int PredictedCharId, CCharacter *pBot,
                            const std::vector<CNetObj_PlayerInput> &inputs, int StartTick,
                            PfChunkResult &out, const unsigned char *pForbiddenGrid, const unsigned char *pCustomFinishGrid,
                            bool FastSim = false)
{
        // FastSim: the 1-tick A* expansion discards inputs/traj/hookSegs, so
        // skip recording them entirely (saves a vector copy + per-tick
        // push_backs). Freeze/forbidden/finish/death checks are unchanged.
        if(!FastSim)
                out.inputs = inputs;
        out.traj.clear();
        if(!FastSim)
                out.traj.push_back(pBot->m_Pos);
        out.hookSegs.clear();
        out.reachedFinish = false;
        out.died = false;
        out.freezeTicks = 0;
        out.forbiddenTicks = 0;
        out.endFreezeTime = 0;

        int prevHookState = pBot->GetCore().m_HookState; // state at start of chunk (before tick 0)
        vec2 prevSimPos = pBot->m_Pos; // v1.56.157: for PfCheckFreeze anti-skip line
        CCollision *pColl = pWorld->Collision();
        int collW = pColl ? pColl->GetWidth() : 0;
        int collH = pColl ? pColl->GetHeight() : 0;
        int TicksRun = 0;

        for(size_t t = 0; t < inputs.size(); t++)
        {
                TicksRun++;
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
                        PfCheckFreeze(pWorld, pBot, prevSimPos, pBot->m_Pos, FastSim);
                }

                if(PfLineTouchesForbidden(pForbiddenGrid, collW, collH, prevSimPos, pBot->m_Pos))
                {
                        out.forbiddenTicks++;
                        out.died = true;
                }

                if(!FastSim)
                        out.traj.push_back(pBot->m_Pos);

                // Record hook segment ONLY at the moment of grab — i.e. when hook state
                // transitions INTO HOOK_GRABBED (prev tick was not GRABBED, this tick is).
                // This gives exactly ONE segment per hook attach, not one per hold tick.
                const CCharacterCore core = pBot->GetCore();
                if(!FastSim && core.m_HookState == HOOK_GRABBED && prevHookState != HOOK_GRABBED)
                {
                        out.hookSegs.push_back({pBot->m_Pos, core.m_HookPos});
                }
                prevHookState = core.m_HookState;
                prevSimPos = pBot->m_Pos; // v1.56.157: update for next tick's anti-skip line

                if(pBot->m_FreezeTime > 0)
                        out.freezeTicks++;

                int tc = round_to_int(core.m_Pos.x / 32.0f);
                int tr = round_to_int(core.m_Pos.y / 32.0f);
                if(!out.died && tc >= 0 && tr >= 0 && tc < collW && tr < collH)
                {
                        int MapIndex = tr * collW + tc;
                        if(pCustomFinishGrid)
                        {
                                if(pCustomFinishGrid[MapIndex])
                                        out.reachedFinish = true;
                        }
                        else if(pColl)
                        {
                                int Tile = pColl->GetTileIndex(MapIndex);
                                int FTile = pColl->GetFrontTileIndex(MapIndex);
                                if(Tile == TILE_FINISH || FTile == TILE_FINISH)
                                        out.reachedFinish = true;
                        }
                }
                if(out.died || out.reachedFinish)
                        break;
        }

        out.ticksSimulated = TicksRun;
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

// =========================================================
// PfSimulateHold — event-terminated chunk rollout (v6.15). Holds ONE input
// for up to MaxTicks ticks but cuts the chunk short at every decision
// point, so a coarse rollout never skips a moment where a different input
// could change the outcome:
//   - hook grab / retract (the fire->flight stretch is ballistic and runs
//     the full chunk; release timing stays a per-expansion choice)
//   - ground <-> air transitions (landing / walking off an edge)
//   - entering freeze (the frozen fast-forward takes over next expansion)
//   - first wall contact (m_Colliding 0 -> nonzero)
//   - death / finish / forbidden (same as PfSimulateChunk)
// Ballistic stretches (falling, hook drag, hook flight) run the full chunk —
// that is where the entire speedup comes from.
// =========================================================
static inline void PfSimulateHold(CGameWorld *pWorld, int PredictedCharId, CCharacter *pBot,
                          const CNetObj_PlayerInput &Input, int StartTick, int MaxTicks,
                          PfChunkResult &out, const unsigned char *pForbiddenGrid, const unsigned char *pCustomFinishGrid,
                          bool FastSim)
{
        if(!FastSim)
        {
                out.inputs.clear();
                out.inputs.push_back(Input);
        }
        out.traj.clear();
        out.traj.push_back(pBot->m_Pos);
        out.hookSegs.clear();
        out.reachedFinish = false;
        out.died = false;
        out.freezeTicks = 0;
        out.forbiddenTicks = 0;
        out.endFreezeTime = 0;

        int prevHookState = pBot->GetCore().m_HookState;
        int prevColliding = pBot->GetCore().m_Colliding;
        CCollision *pColl = pWorld->Collision();
        const bool prevGrounded = pColl ? pColl->IsOnGround(pBot->m_Pos, CCharacterCore::PhysicalSize()) : false;
        vec2 prevSimPos = pBot->m_Pos;
        int collW = pColl ? pColl->GetWidth() : 0;
        int collH = pColl ? pColl->GetHeight() : 0;
        int TicksRun = 0;

        for(int t = 0; t < MaxTicks; t++)
        {
                TicksRun++;
                int Tick = StartTick + t + 1;
                pBot->OnDirectInput(&Input);
                pWorld->m_GameTick = Tick;
                pBot->OnPredictedInput(&Input);
                pWorld->Tick();
                if(!pWorld->GetCharacterById(PredictedCharId))
                {
                        out.died = true;
                        break;
                }

                if(pBot->m_FreezeTime <= 0)
                {
                        PfCheckFreeze(pWorld, pBot, prevSimPos, pBot->m_Pos, FastSim);
                }
                if(PfLineTouchesForbidden(pForbiddenGrid, collW, collH, prevSimPos, pBot->m_Pos))
                {
                        out.forbiddenTicks++;
                        out.died = true;
                }
                out.traj.push_back(pBot->m_Pos);

                const CCharacterCore core = pBot->GetCore();
                if(core.m_HookState == HOOK_GRABBED && prevHookState != HOOK_GRABBED)
                {
                        out.hookSegs.push_back({pBot->m_Pos, core.m_HookPos});
                }
                prevSimPos = pBot->m_Pos;

                if(pBot->m_FreezeTime > 0)
                        out.freezeTicks++;

                int tc = round_to_int(core.m_Pos.x / 32.0f);
                int tr = round_to_int(core.m_Pos.y / 32.0f);
                if(!out.died && tc >= 0 && tr >= 0 && tc < collW && tr < collH)
                {
                        int MapIndex = tr * collW + tc;
                        if(pCustomFinishGrid)
                        {
                                if(pCustomFinishGrid[MapIndex])
                                        out.reachedFinish = true;
                        }
                        else if(pColl)
                        {
                                int Tile = pColl->GetTileIndex(MapIndex);
                                int FTile = pColl->GetFrontTileIndex(MapIndex);
                                if(Tile == TILE_FINISH || FTile == TILE_FINISH)
                                        out.reachedFinish = true;
                        }
                }

                // ---- decision-point events: cut the chunk exactly here ----
                const bool GroundedNow = pColl ? pColl->IsOnGround(pBot->m_Pos, CCharacterCore::PhysicalSize()) : false;
                if(!out.died && !out.reachedFinish)
                {
                        // hook grab / retract (IDLE->FLYING is ballistic, keep going)
                        if(core.m_HookState != prevHookState && !(prevHookState == HOOK_IDLE && core.m_HookState == HOOK_FLYING))
                                break;
                        // freeze: the frozen fast-forward handles the rest deterministically
                        if(out.freezeTicks > 0)
                                break;
                        // landing / walking off an edge
                        if(GroundedNow != prevGrounded)
                                break;
                        // first wall contact
                        if(core.m_Colliding != 0 && prevColliding == 0)
                                break;
                }
                prevHookState = core.m_HookState;
                prevColliding = core.m_Colliding;
                if(out.died || out.reachedFinish)
                        break;
        }

        out.ticksSimulated = TicksRun;
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
//                   3=LaserUnfreeze 4=Pathfinder 5=ESP 6=TAS 7=Hitboxes
// v1.56.280: 8=RT Pathfinder (Real-Time Pathfinder panel).
enum EKxLineComponent
{
	KX_LINE_AIMBOT = 0,
	KX_LINE_TRIGGERBOT = 1,
	KX_LINE_TRAJECTORY = 2,
	KX_LINE_LASER_UNFREEZE = 3,
	KX_LINE_PATHFINDER = 4,
	KX_LINE_ESP = 5,
	KX_LINE_TAS = 6,
	KX_LINE_HITBOXES = 7,
	KX_LINE_RT_PATHFINDER = 8,
	KX_LINE_COUNT = 9,
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
			case KX_LINE_TAS: enabled = g_Config.m_KxLineTasRainbow != 0; speed = g_Config.m_KxLineTasRainbowSpeed; break;
			case KX_LINE_HITBOXES: enabled = g_Config.m_KxLineHitboxesRainbow != 0; speed = g_Config.m_KxLineHitboxesRainbowSpeed; break;
			case KX_LINE_RT_PATHFINDER: enabled = g_Config.m_KxLineRtPathfinderRainbow != 0; speed = g_Config.m_KxLineRtPathfinderRainbowSpeed; break;
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
		case KX_LINE_TAS: return g_Config.m_KxLineTasSize;
		case KX_LINE_HITBOXES: return g_Config.m_KxLineHitboxesSize;
		case KX_LINE_RT_PATHFINDER: return g_Config.m_KxLineRtPathfinderSize;
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
		case KX_LINE_TAS: rainbow = g_Config.m_KxLineTasRainbow != 0; speed = g_Config.m_KxLineTasRainbowSpeed; break;
		case KX_LINE_HITBOXES: rainbow = g_Config.m_KxLineHitboxesRainbow != 0; speed = g_Config.m_KxLineHitboxesRainbowSpeed; break;
		case KX_LINE_RT_PATHFINDER: rainbow = g_Config.m_KxLineRtPathfinderRainbow != 0; speed = g_Config.m_KxLineRtPathfinderRainbowSpeed; break;
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
		case KX_LINE_TAS: return g_Config.m_KxLineTasColor;
		case KX_LINE_HITBOXES: return g_Config.m_KxLineHitboxesColor;
		case KX_LINE_RT_PATHFINDER: return g_Config.m_KxLineRtPathfinderColor;
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
		case KX_LINE_TAS: return (float)g_Config.m_KxLineTasAlpha / 100.0f;
		case KX_LINE_HITBOXES: return (float)g_Config.m_KxLineHitboxesAlpha / 100.0f;
		case KX_LINE_RT_PATHFINDER: return (float)g_Config.m_KxLineRtPathfinderAlpha / 100.0f;
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
        case KX_LINE_TAS:
                rainbow = g_Config.m_KxLineTasRainbow != 0;
                gradient = g_Config.m_KxLineTasGradient != 0;
                step = g_Config.m_KxLineTasGradientStep;
                break;
		case KX_LINE_HITBOXES:
			rainbow = g_Config.m_KxLineHitboxesRainbow != 0;
			gradient = g_Config.m_KxLineHitboxesGradient != 0;
			step = g_Config.m_KxLineHitboxesGradientStep;
			break;
		case KX_LINE_RT_PATHFINDER:
			rainbow = g_Config.m_KxLineRtPathfinderRainbow != 0;
			gradient = g_Config.m_KxLineRtPathfinderGradient != 0;
			step = g_Config.m_KxLineRtPathfinderGradientStep;
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

// =========================================================
// v1.56.260: Visuals-panel world rainbow. Speed cvars store display*10
// (0.1-10.0); hue advances display*60 deg/sec — same convention as the
// ClickGUI accent rainbow (m_RainbowHue += dt * speed * 60).
// =========================================================
inline float KxVisualRainbowHue(float TimeSeconds, int SpeedCvar)
{
	float h = std::fmod(TimeSeconds * (float)SpeedCvar * 6.0f, 360.0f);
	return h < 0.0f ? h + 360.0f : h;
}

// v6.15: Custom colors master gate (kx_custom_colors). Every per-sprite
// enable (KxRainbowTee/Legs/Guns/Hook/Hud/Particles/Emoticons/Ent*) must be
// AND-ed with this — 0 = original game look, nothing recolors.
inline bool KxCustomColorsOn()
{
	return g_Config.m_KxCustomColors != 0;
}

inline ColorRGBA KxVisualRainbowColor(float TimeSeconds, int SpeedCvar, float Alpha)
{
	const float h = KxVisualRainbowHue(TimeSeconds, SpeedCvar) / 60.0f;
	const float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
	float r = 0.0f, g = 0.0f, b = 0.0f;
	if(h < 1.0f) { r = 1.0f; g = x; }
	else if(h < 2.0f) { r = x; g = 1.0f; }
	else if(h < 3.0f) { g = 1.0f; b = x; }
	else if(h < 4.0f) { g = x; b = 1.0f; }
	else if(h < 5.0f) { r = x; b = 1.0f; }
	else { r = 1.0f; b = x; }
	return ColorRGBA(r, g, b, Alpha);
}

// v1.56.275: static color from the per-feature Color/Opacity cvars.
// ColorCvar stores 0xAARRGGBB (MACRO_CONFIG_COL), AlphaCvar is 0-100.
inline ColorRGBA KxVisualFixedColor(int ColorCvar, int AlphaCvar, float AlphaMul = 1.0f)
{
	return ColorRGBA(
		((ColorCvar >> 16) & 0xff) / 255.0f,
		((ColorCvar >> 8) & 0xff) / 255.0f,
		(ColorCvar & 0xff) / 255.0f,
		(AlphaCvar / 100.0f) * AlphaMul);
}

// =========================================================
// PfBfsFill — multi-source FMM/Eikonal field fill, verbatim the
// solver the Pathfinder tab uses for its flow field.
// Two phases come free: freezePassable=false keeps freeze at 1e18;
// a second call with freezePassable=true lets routes cross freeze.
// =========================================================
template<typename TWalk, typename TFreeze>
void PfBfsFill(float *pField, int FW, int FH, const std::vector<std::pair<int, int>> &vSources,
	TWalk fnWalk, TFreeze fnFreeze, bool freezePassable, const float *pHop = nullptr)
{
	int Size = FW * FH;
	for(int i = 0; i < Size; i++)
		pField[i] = 1e18f;

	std::vector<char> state(Size, 0);
	using FNode = std::pair<float, int>;
	std::priority_queue<FNode, std::vector<FNode>, std::greater<FNode>> open;
	static const int dr[] = {-1, 1, 0, 0};
	static const int dc[] = {0, 0, -1, 1};

	auto walkOk = [&](int x, int y) -> bool {
		if(!fnWalk(x, y)) return false;
		if(!freezePassable && fnFreeze(x, y)) return false;
		return true;
	};

	auto eikonal = [&](int x, int y) -> float {
		float hop = pHop ? pHop[y * FW + x] : 1.0f;
		if(hop < 0.0001f)
			hop = 0.0001f;
		float txm = (x > 0 && state[y * FW + (x - 1)] == 2) ? pField[y * FW + (x - 1)] : 1e18f;
		float txp = (x < FW - 1 && state[y * FW + (x + 1)] == 2) ? pField[y * FW + (x + 1)] : 1e18f;
		float tym = (y > 0 && state[(y - 1) * FW + x] == 2) ? pField[(y - 1) * FW + x] : 1e18f;
		float typ = (y < FH - 1 && state[(y + 1) * FW + x] == 2) ? pField[(y + 1) * FW + x] : 1e18f;
		float a = std::min(txm, txp);
		float b = std::min(tym, typ);
		if(a >= 1e17f && b >= 1e17f) return 1e18f;
		if(a >= 1e17f) return b + hop;
		if(b >= 1e17f) return a + hop;
		float diff = a - b;
		if(diff < 0.0f) diff = -diff;
		if(diff >= hop) return std::min(a, b) + hop;
		return (a + b + std::sqrt(2.0f * hop * hop - diff * diff)) * 0.5f;
	};

	auto relax = [&](int x, int y) {
		int idx = y * FW + x;
		if(state[idx] == 2) return;
		float t = eikonal(x, y);
		if(t < pField[idx])
		{
			pField[idx] = t;
			state[idx] = 1;
			open.push({t, idx});
		}
	};

	for(const auto &src : vSources)
	{
		int x = src.first, y = src.second;
		if(x < 0 || y < 0 || x >= FW || y >= FH) continue;
		if(!fnWalk(x, y)) continue;
		int idx = y * FW + x;
		if(state[idx] == 2) continue;
		pField[idx] = 0.0f;
		state[idx] = 2;
	}
	for(const auto &src : vSources)
	{
		int x = src.first, y = src.second;
		if(x < 0 || y < 0 || x >= FW || y >= FH) continue;
		if(!fnWalk(x, y)) continue;
		for(int k = 0; k < 4; k++)
		{
			int nx = x + dc[k], ny = y + dr[k];
			if(nx < 0 || ny < 0 || nx >= FW || ny >= FH) continue;
			if(!walkOk(nx, ny)) continue;
			relax(nx, ny);
		}
	}

	while(!open.empty())
	{
		FNode cur = open.top();
		open.pop();
		int idx = cur.second;
		if(state[idx] == 2) continue;
		if(cur.first > pField[idx]) continue;
		state[idx] = 2;
		int x = idx % FW, y = idx / FW;
		for(int k = 0; k < 4; k++)
		{
			int nx = x + dc[k], ny = y + dr[k];
			if(nx < 0 || ny < 0 || nx >= FW || ny >= FH) continue;
			if(!walkOk(nx, ny)) continue;
			relax(nx, ny);
		}
	}
}

#endif // GAME_CLIENT_COMPONENTS_KINETIX_INTERNAL_H
