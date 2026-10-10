#ifndef GAME_CLIENT_COMPONENTS_BOTNET_H
#define GAME_CLIENT_COMPONENTS_BOTNET_H

#include <base/vmath.h>

#include <engine/client/enums.h>
#include <engine/console.h>
#include <engine/shared/config.h>

#include <game/client/component.h>
#include <game/client/components/kinetix/dummies/dummy_data.h>
#include <game/gamecore.h>
#include <generated/protocol.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Pathfinder constants
static const int PF_MAX_MAP_SIZE = 2048;

// Helpers plan constants
static const int HELPERS_MAX_TICKS = 1024; // max ticks in one helpers plan

// =========================================================
// Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker
// =========================================================
// State machine:  IDLE (green pathfinding btn) -> RUNNING (red stop btn) -> FINISHED (orange finish btn) -> IDLE
// While RUNNING, UpdatePathfinder() runs one chunk per OnUpdate: generates
// candidate input-sequences, scores each by the
// enabled Score+Fine methods, picks the best, appends its trajectory to
// m_PfVPath, and checks for finish.  On finish (or max-chunks), -> FINISHED.
// The trajectory is rendered every frame via RenderPathfinderPath(), reusing
// the shared m_KxLineRenderingColor/Alpha/LineSize settings (same as Advanced→Trajectory).
enum EPfState
{
        PF_STATE_IDLE = 0,
        PF_STATE_RUNNING = 1,
        PF_STATE_FINISHED = 2,
};
static const int PF_BACKTRACK_DEPTH = 5;

int PfEffortHookAngles();
int PfEffortAStarWeight();
int PfEffortBeamWidth();
int PfEffortRheaPop();
int PfEffortRheaGens();
int PfEffortMctsIters();
bool PfEffortScoreDist();
bool PfEffortScoreFlow();
bool PfEffortFreezeSupport();
bool PfEffortFineDeath();

// Per-tick hook segment: tee position + hook attach/fly position at one tick.
// Used by PfSimulateChunk (PfChunkResult) and AStarNode to capture ALL hook
// fire/hold cycles inside a chunk, not just the final state.
struct PfHookSeg
{
        vec2 teePos;
        vec2 hookPos;
};

// v1.56.280: RT Pathfinder hook segment — same as PfHookSeg plus the absolute
// plan tick index it was captured at (used to split future/past hook lines in
// the renderer: segments with tick > playback cursor are still future).
struct RtPfHookSeg
{
        vec2 teePos;
        vec2 hookPos;
        int tick;
};

// =========================================================
// Pathfinder backtracking buffer
// =========================================================
struct PfBacktrackEntry
{
        vec2 pos;
        vec2 vel;
        vec2 hookPos;
        vec2 hookDir;
        int hookState;
        int hookTick;
        int jumped;
        int freezeTime; // m_PfCurFreezeTime at this backtrack point
        float totalFreezeTicks;
        int vPathSize; // m_PfVPath.size() before this chunk was applied
        int chunkCount; // m_PfChunkCount at this backtrack point
        struct Candidate
        {
                std::vector<CNetObj_PlayerInput> inputs;
                std::vector<vec2> traj;
                float score;
                // End state for direct application (avoids re-simulation)
                vec2 endPos, endVel, endHookPos, endHookDir;
                int endHookState, endHookTick, endJumped;
                int endFreezeTime; // bot's m_FreezeTime at end of chunk (0 = not frozen)
                int freezeTicks;
        };
        std::vector<Candidate> candidates; // top-N candidates (dynamic size)
        int numCandidates;
        int usedCandidateIdx;

        PfBacktrackEntry() { Reset(); }
        void Reset()
        {
                pos = vec2(0, 0);
                vel = vec2(0, 0);
                hookPos = vec2(0, 0);
                hookDir = vec2(0, 0);
                hookState = 0;
                hookTick = 0;
                jumped = 0;
                freezeTime = 0;
                totalFreezeTicks = 0.0f;
                vPathSize = 0;
                chunkCount = 0;
                candidates.clear();
                candidates.shrink_to_fit(); // free heap memory after a backtrack run
                numCandidates = 0;
                usedCandidateIdx = -1;
        }
};

// =========================================================
// Helpers plan — cached physics input package per dummy
// (ComputeHelpersPath builds it, ApplyHelpersPlan replays it)
// =========================================================
struct CHelpersPlan
{
        CNetObj_PlayerInput aInputs[HELPERS_MAX_TICKS]; // the winning input package
        int NumTicks;           // how many ticks in the package
        int CurrentTick;        // which tick of the package we're currently applying
        int ComputeTick;        // game tick when this was computed
        int TargetTX, TargetTY; // target tile at compute time (for invalidation)
        vec2 BotPos;            // bot position at compute time (for invalidation)
        std::vector<vec2> vPath; // predicted positions for rendering

        CHelpersPlan() { Reset(); }

        void Reset()
        {
                NumTicks = 0;
                CurrentTick = 0;
                ComputeTick = 0;
                TargetTX = -1;
                TargetTY = -1;
                BotPos = vec2(0, 0);
                vPath.clear();
        }

        bool IsValid() const { return NumTicks > 0; }
        bool IsExpired(int CacheTicks) const { return CurrentTick >= NumTicks || CurrentTick >= CacheTicks; }
};

// =========================================================
// Per-dummy runtime state (position/timing dependent)
// Each inactive dummy has its own instance
// =========================================================
struct CBotNetDummy
{
        // Pathfinder flow
        vec2 m_FlowDir;
        vec2 m_PfHookTile;
        int m_FlowTargetTX, m_FlowTargetTY;
        int m_LastTargetTX, m_LastTargetTY;
        int m_LastBotTX, m_LastBotTY;
        bool m_PathFound;
        bool m_RescueField; // current field is the rescue FMM (flow = field gradient)
        bool m_FlowThroughFreeze; // rescue Phase 2: field intentionally crosses freeze

        // Attack runtime
        int m_HookTickTimer;
        int m_JumpTicks;

        // Stuck detection
        vec2 m_LastPos;
        int m_StuckTicks;

        // PathfinderGo
        bool m_PathfinderGoActive;
        vec2 m_PathfinderGoPos;

        // Random aim timing

        // Helpers plan (physics movement cache)
        CHelpersPlan m_HelpersPlan;

        // Copy moves
        int m_LastTargetAttackTick;

        // Macro
        bool m_MacroRecording;
        bool m_MacroPlaying;
        int m_MacroCaptureID;
        std::vector<std::string> m_MacroRecordBuffer;
        std::vector<std::string> m_MacroPlayLines;
        int m_MacroPlayIndex;
        int m_MacroSleepTicks;
        int64_t m_MacroSleepUntilTick;
        int64_t m_LastMacroRecordTick;
        int m_LastRecordedDir;
        int m_LastRecordedJump;
        int m_LastRecordedHook;
        int m_LastRecordedFire;
        int m_LastRecordedAimX;
        int m_LastRecordedAimY;
        int m_LastRecordedWeapon;

        CBotNetDummy() { Reset(); }

        void Reset()
        {
                m_FlowDir = vec2(0, 0);
                m_PfHookTile = vec2(0, 0);
                m_FlowTargetTX = m_FlowTargetTY = -1;
                m_LastTargetTX = m_LastTargetTY = -1;
                m_LastBotTX = m_LastBotTY = -1;
                m_PathFound = false;
                m_RescueField = false;
                m_FlowThroughFreeze = false;
                m_HookTickTimer = 0;
                m_JumpTicks = 0;
                m_LastPos = vec2(0, 0);
                m_StuckTicks = 0;
                m_PathfinderGoActive = false;
                m_PathfinderGoPos = vec2(0, 0);
                m_LastTargetAttackTick = -1;
                m_MacroRecording = false;
                m_MacroPlaying = false;
                m_MacroCaptureID = -1;
                m_MacroPlayIndex = 0;
                m_MacroSleepTicks = 0;
                m_MacroSleepUntilTick = 0;
                m_LastMacroRecordTick = 0;
                m_LastRecordedDir = 0;
                m_LastRecordedJump = 0;
                m_LastRecordedHook = 0;
                m_LastRecordedFire = 0;
                m_LastRecordedAimX = 0;
                m_LastRecordedAimY = 0;
                m_LastRecordedWeapon = -1;
        }

        void ResetMacroBuffers()
        {
                m_MacroRecordBuffer.clear();
                m_MacroPlayLines.clear();
                m_MacroPlayIndex = 0;
                m_MacroSleepTicks = 0;
                m_MacroSleepUntilTick = 0;
        }
};

class CGameWorld;

// =========================================================
// CBotNet — botnet component (all inactive dummies)
// =========================================================
struct PfChunkResult;

class CBotNet : public CComponent
{
public:
        // --- Shared settings (same for all dummies) ---
        // v1.56.171 BUG9: all kx_ on/off + numeric settings moved to MACRO_CONFIG_INT
        // cvars in config_variables.h. CBotNet no longer holds them as members.
        bool m_TargetList[128];
        bool m_BotsList[128];
        bool m_RescueList[128];
        // Which dummy slots act as helpers (parsed from the "Dummy IDs" field).
        bool m_HelpersDummyList[MAX_DUMMIES];

        // v1.56.171 BUG9: previous cvar values for change-detection in OnUpdate.
        // When a cvar transitions 1→0, we run the side effects that used to live
        // in the old Con-handlers (ResetDummyInputs, KinoCache.Reset, etc.).
        int m_PrevKxHelpers = 0;
        int m_PrevKxRescue = 0;
        int m_PrevKxCopyMoves = 0;
        // v1.56.258: m_PrevKx* members for removed legacy attack cvars deleted.

        // String buffers for UI input fields (comma-separated IDs)
        char m_aTargetIDsStr[256];
        char m_aBotsIDsStr[256];
        char m_aRescueIDsStr[256];
        char m_aHelpersDummyIDsStr[256];

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker.
        // State machine driven by m_PfState; trajectory in m_PfVPath.
        // m_PfStartPos = active-player position when RUNNING started (for re-anchor).
        // m_PfChunkCount = chunks generated so far.
        // m_PfFlowField = multi-source BFS distance field to ALL finish tiles.
        int m_PfState;                  // EPfState
        int m_PfChunkCount;             // chunks generated this run
        vec2 m_PfStartPos;              // active-player pos at run start
        vec2 m_PfCurPos;                // current sim position (end of last chunk)
        vec2 m_PfCurVel;                // current sim velocity
        int m_PfCurHookState;           // current sim hook state
        vec2 m_PfCurHookPos;            // current sim hook position
        vec2 m_PfCurHookDir;            // current sim hook direction
        int m_PfCurHookTick;            // current sim hook tick counter
        int m_PfCurFreezeTime;          // current sim freeze time
        int m_PfCurJumped;              // current sim jump flags
        int m_PfTickCounter;            // throttle: counts ticks until next chunk (perf% spread)
        std::vector<vec2> m_PfVPath;    // accumulated trajectory (start..current)
        std::vector<vec2> m_PfFinishTiles; // cached finish tile centers (multi-source BFS sources)
        float *m_PfFlowField;           // BFS distance field to nearest finish (32px tiles)
        float *m_PfScoreField = nullptr; // BFS distance field 4× resolution (8px cells) for distance reduction scoring
        int m_PfScoreFieldW = 0;        // score field width (m_MapWidth * 4)
        int m_PfScoreFieldH = 0;        // score field height (m_MapHeight * 4)
        int m_PfTotalFreezeTicks;       // running total for dynamic freeze penalty
        PfBacktrackEntry m_PfBacktrack[PF_BACKTRACK_DEPTH];
        int m_PfBacktrackIdx;           // stack index (next write position, capped at PF_BACKTRACK_DEPTH)

        // ── State-Lattice A* (v1.56) ──
        struct PfAState
        {
                vec2 Pos;
                vec2 Vel;
                vec2 HookPos;
                vec2 HookDir;
                int HookState;
                int HookTick;
                int Jumped;
                int JumpedTotal;
        };
        struct AStarNode
        {
                PfAState Core;
                int FreezeTime;
                bool Grounded;
                float G, H, F;
                int ParentIdx;   // -1 = root
                int PrimIdx;     // -1 = root, 0 = frozen fast-forward, >=1 = 1+packed single-tick input
                int PrimTicks;
                int TickOffset;
                int FreezeChainTicks = 0;
                int ForbiddenChainTicks = 0;
                std::vector<vec2> Traj; // frozen fast-forward only; single-tick nodes derive parent.Pos → Pos
                std::vector<CNetObj_PlayerInput> Inputs; // frozen fast-forward only; single-tick decodes from PrimIdx
                // Hook segments, frozen fast-forward only (single-tick derives the grab seg from states).
                std::vector<PfHookSeg> HookSegs;
        };
        std::vector<AStarNode> m_PfANodes;
        std::vector<int> m_PfAOpen;
        std::vector<int> m_PfABeamReserve;
        std::unordered_map<uint64_t, float> m_PfABestG;
        int m_PfAGoalIdx;
        bool m_PfAStarted;
	bool m_PfAPathReady;
	int m_PfAExpandCount;
	std::atomic<float> m_PfABestH{1e18f};
	float m_PfAHRoot = 1e18f;
	int m_PfUiBestDone = 0;
	int m_PfUiBestTotal = 0;
        std::vector<CNetObj_PlayerInput> m_PfFullInputs;
        size_t m_PfFullInputsIdx;

        // v1.57: Threaded pathfinder — A* search runs in a background thread.
        // m_PfBaseWorld = pristine snapshot of PredictedWorld (taken in PfResetRun
        //   on the main thread). Worker reads from this; never touches GameClient().
        // m_PfLocalID = LocalClientId captured at snapshot time.
        // m_PfThreadResult: 0=running, 1=goal found, -1=failed/cancelled.
        // m_PfDataMutex: protects m_PfVPath (written by worker, read by renderer).
        CGameWorld *m_PfBaseWorld = nullptr;
        CGameWorld *m_PfSimWorld = nullptr;
        int m_PfSimStartTick = 0;
        int m_PfLocalID = -1;
        std::thread m_PfThread;
        std::atomic<bool> m_PfThreadRunning{false};
        std::atomic<bool> m_PfThreadCancel{false};
        std::atomic<int> m_PfThreadResult{0};
        std::mutex m_PfDataMutex;
        std::vector<PfHookSeg> m_PfVHookSegs;
        std::vector<vec2> m_PfVPathRender;
        std::vector<PfHookSeg> m_PfVHookSegsRender;
        std::vector<std::pair<vec2, vec2>> m_PfVBranchSegsRender;

	int m_PfEditorMode = 0;
	bool m_PfEditFireHeld = false;
	bool m_PfEditHookHeld = false;
	bool m_PfEditCleaned = false;
	double m_PfGenStartTime = 0.0;
	double m_PfGenEndTime = 0.0;
	int m_PfGenLastState = 0;
	bool m_PfSpecFollow = false;
	bool m_PfSpecWasInSpec = false;
	bool m_PfSpecCamInit = false;
	vec2 m_PfSpecCamPos = vec2(0.0f, 0.0f);

	// =========================================================
	// ── Real-Time Pathfinder (v1.56.280) ──
	// Single-branch greedy planner ("kinodynamic" style, no tree) that builds
	// a rolling Future-Ticks plan on a WORKER THREAD and reproduces it on the
	// live player in real time through CControls::SnapInput. Reconstruct: when
	// the real position deviates from the planned one by >= trigger (units),
	// the plan is cleared and rebuilt from the current position on the fly
	// (Anti-TAS chaos: tunings get randomized in tiny values so a long static
	// plan desyncs constantly).
	// =========================================================
	int m_RtPfState = 0; // 0 = idle, 1 = running (no FINISHED state)
	// Worker thread (own world snapshot + sim world, like the A* worker).
	CGameWorld *m_RtPfBaseWorld = nullptr; // snapshot taken on the main thread
	CGameWorld *m_RtPfSimWorld = nullptr;  // worker-owned planning world
	int m_RtPfLocalID = -1;
	std::thread m_RtPfThread;
	std::atomic<bool> m_RtPfThreadRunning{false};
	std::atomic<bool> m_RtPfThreadCancel{false};
	std::atomic<bool> m_RtPfRebuildPending{false}; // root request: main posts the live state, worker re-anchors its chain
	std::mutex m_RtPfDataMutex; // guards the plan + the worker-to-main mailbox
	// The plan — written ONLY by the main thread (UpdateRtPathfinder commits
	// the chunks the worker generates).
	std::vector<CNetObj_PlayerInput> m_RtPfInputs; // inputs[m] applied at plan tick m
	std::vector<vec2> m_RtPfPath;                  // path[0] = plan start pos, path[m+1] = pos after tick m
	std::vector<RtPfHookSeg> m_RtPfHookSegs;       // planned hook segments (future, absolute tick idx)
	int m_RtPfPlayIdx = 0;                         // playback cursor (advanced by UpdateRtPathfinder, once per predicted tick)
	bool m_RtPfReady = false;                      // playback gate: the tail is buffered to Future Ticks before (re)starting
	int m_RtPfAnchorTick = -1;                     // tick whose state the posted plan root describes — playback resumes at AnchorTick+1 only (freshness check)
	// Root request payload (main thread fills, worker consumes).
	PfAState m_RtPfRebuildCore;
	int m_RtPfRebuildFreeze = 0;
	// Worker-to-main mailbox: the generated path data (the worker writes
	// ONLY this; the main thread commits it to the plan).
	bool m_RtPfOutValid = false;
	std::vector<CNetObj_PlayerInput> m_RtPfOutInputs;
	std::vector<vec2> m_RtPfOutPath;
	std::vector<RtPfHookSeg> m_RtPfOutSegs;
	int m_RtPfOutTruncateTo = -1; // backtrack: worker asks the main to shrink the plan to this size
	int m_RtPfLastProcessedTick = -1; // per-tick gate for OnUpdate processing
	// Already reproduced history (rendered by Show Previous Path with alpha/2).
	std::vector<vec2> m_RtPfPassedPath;
	std::vector<RtPfHookSeg> m_RtPfPassedHookSegs;
	int m_RtPfPrevHookState = 0; // hook grab transition detection during playback
	// Own flow field (separate from the Pathfinder tab's m_PfFlowField).
	float *m_RtPfFlowField = nullptr;
	std::vector<vec2> m_RtPfFinishTiles; // natural finish tile centers
	// SEPARATE tile editor (forbidden zones + custom finish) for RT Pathfinder.
	std::vector<vec2> m_RtPfForbiddenTiles;
	std::vector<vec2> m_RtPfCustomFinishTiles;
	unsigned char *m_pRtForbiddenGrid = nullptr;
	unsigned char *m_pRtCustomFinishGrid = nullptr;
	int m_RtPfEditorMode = 0; // 0 = off, 1 = forbidden editor, 2 = finish editor
	bool m_RtPfEditFireHeld = false;
	bool m_RtPfEditHookHeld = false;
	bool m_RtPfEditCleaned = false;

        std::vector<vec2> m_PfForbiddenTiles;
        std::vector<vec2> m_PfCustomFinishTiles;
        unsigned char *m_pForbiddenGrid = nullptr;
        unsigned char *m_pCustomFinishGrid = nullptr;

        // Per-dummy runtime state
        CBotNetDummy m_aDummies[MAX_DUMMIES];

        // Kinetix Dummy Manager (v1.56.282): per-dummy profiles + proxies.
        // Exposed as runtime commands (dummy%d_* / kx_dummy_limit), persisted
        // to kinetix_dummies.cfg — deliberately NOT in g_Config.
        CDummyData m_Dummy;

        // Map grid (shared — same map for all)
        int m_MapWidth;
        int m_MapHeight;
        unsigned char *m_pMapGrid;
        unsigned char *m_pFrontGrid;
        bool m_MapGridLoaded;
        char m_aLastMapName[256];
        float *m_pfDist;
        unsigned char *m_pfFreezeDist; // BFS route distance to nearest freeze, 255 = none in reach

        CBotNet();
        ~CBotNet();

        int Sizeof() const override { return sizeof(*this); }
        void OnConsoleInit() override;
        void OnUpdate() override;
        void OnRender() override;
        void OnReset() override;
        void OnMapLoad() override;
        bool OnInput(const IInput::CEvent &Event) override;

        // Process one inactive dummy
        void ProcessDummy(int Dummy);

        // Helpers (attack/rescue/escort) — the whole feature for one dummy
        // tick: target selection, movement, aim/hook/fire (attack.cpp).
        void UpdateHelpers(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID);

        // Check if botnet is active for a specific dummy
        bool IsDummyActive(int Dummy) const;

        // Reset inputs for a specific dummy
        void ResetDummyInputs(int Dummy);

        // Sync ID list string <-> bool array
        void SyncIDsToStr(const bool *pList, char *pStr, int StrSize);
        void SyncStrToIDs(const char *pStr, bool *pList);
        void SyncStrToDummyIDs(const char *pStr, bool *pList); // dummy slots (empty = all)
        bool IsHelpersDummy(int Dummy); // listed slot AND not the current dummy

        // Pathfinder (shared map, per-dummy flow)
        void LoadMapGrid();
        bool IsTileWalkable(int tx, int ty);
        bool IsTileFreeze(int tx, int ty);
        void ComputePathfinder(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State);
        // Rescue guidance: multi-source safe-tile rings around a frozen ally,
        // freeze tiles impassable.  Fills m_pfDist + flow on success.
        void ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State);
        // Route distance to the nearest freeze tile for the whole grid:
        // multi-source BFS from freeze tiles through walkable tiles only
        // (walls are never crossed).  255 = no freeze in reach.  Feeds the
        // A* danger cost and the survival retreat.
        void ComputeFreezeDistField();
        bool HasLineOfSightTiles(int r1, int c1, int r2, int c2);
        // walkable + no LOS-visible freeze within SAFETY_MARGIN — survival check
        bool IsSafeSpot(int tx, int ty);
        void ComputeFlowForTile(int r, int c, CBotNetDummy &State);
        void RescueFlowForTile(int r, int c, CBotNetDummy &State); // rescue FMM gradient
        void GetMovementFromFlow(const CBotNetDummy &State, bool &outLeft, bool &outRight, bool &outJump);

        // Helpers physics plan (pathfinder.cpp) — greedy on-the-fly builder:
        // the plan is built window by window (12 ticks of real physics per
        // step) straight along the freeze-walled grid field; any freeze
        // contact kills the window.  Drives attack/rescue/escort movement.
        void ComputeHelpersPath(int Dummy, const vec2 &MyPos, int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State);
        void ApplyHelpersPlan(int Dummy, CNetObj_PlayerInput *pInput, CBotNetDummy &State);
        void RenderPackage(int Dummy);
        void RenderVectorField();

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker
        void UpdatePathfinder();            // called from OnUpdate; runs one chunk if RUNNING
        void RenderPathfinderPath();        // called from OnRender; draws m_PfVPath like RenderPackage
        void PfResetRun();                  // clear vPath + sim state (start fresh from active player)
        void PfClearSearchState();          // BUG4 v1.56.165: release A*/RHEA buffers (safe from any state)
        void PfPasteToTas();                // kx_pf_paste: paste the found path into TAS Playground
        void UpdateLaserUnfreeze();         // called from OnUpdate; predictive laser self-unfreeze
        void RenderLaserUnfreezePath();     // called from OnRender; draws successful laser path with fade
        void UpdateFakeAim();               // called from OnUpdate; fake aim for fun
        // v1.56.151: Fly Ride — pilot (active dummy) hooks/hammers nearest dummy.
        // WASD moves anchor point. Called from OnUpdate (per-frame); anchor update gated per-tick.
        void UpdateFlyRide();
        void RenderFlyRideAnchor(); // called from OnRender — draws red semi-transparent tile at anchor
        int m_FlyRideLastAnchorTick = -1;   // last tick anchor was updated (per-tick gate)
        bool m_FlyRideWasActive = false;    // for init-on-enable + cleanup-on-disable
        vec2 m_FlyRideAnchor = vec2(0, 0);  // anchor point (pilot pos at enable; WASD moves it)
        int m_FlyRidePilotDir = 0;          // -1 left, 0 none, +1 right (read by CControls::SnapInput)
        int m_FlyRidePilotHook = 0;         // 0 or 1 (read by CControls::SnapInput)
        int m_FlyRideTargetDummy = -1;      // nearest dummy index (for cleanup)
        void UpdateTripleFly();
        // v1.56.262: Admin Alarm — chat alert when an RCON admin is on the server.
        void UpdateAdminAlarm();
        bool m_TripleFlyWasActive = false;
        int m_TripleFlyDummy = -1;
        bool m_TripleFlyHooking = false;

        // Laser unfreeze path storage (for Show attempt rendering)
        std::vector<vec2> m_LaserUnfreezePath;
        int64_t m_LaserUnfreezePathTime = 0;
        void PfComputeFlowField(const vec2 &StartPos); // multi-source BFS to all finish tiles -> m_PfFlowField
        void PfGetUiStats(bool &outRunning, bool &outFinished, float &outPercent, int &outDoneTiles,
                int &outTotalTiles, double &outElapsedSec, double &outEtaSec,
                double &outPathTimeSec, double &outGenTimeSec);
        bool PfBacktrack();              // try alternative candidates from backtrack buffer

        void UpdatePfTileEditor();
        void RenderPfTileEditor();
        bool PfTileCanBeCustomFinish(int tx, int ty);
        void PfAddForbiddenTile(int tx, int ty);
        void PfRemoveForbiddenTile(int tx, int ty);
        void PfAddCustomFinishTile(int tx, int ty);
        void PfRemoveCustomFinishTile(int tx, int ty);
        void PfClearForbiddenTiles();
        void PfClearCustomFinishTiles();
        void PfEditorBeginChange();
        void PfEditorEndChange();
        void PfGenerateTunnel();            // tunnel generator (Path/Map mode) -> forbidden zones
        const std::vector<vec2> &PfActiveFinishTiles() const
        {
                return m_PfCustomFinishTiles.empty() ? m_PfFinishTiles : m_PfCustomFinishTiles;
        }
        const unsigned char *PfCustomFinishSimGrid() const
        {
                return m_PfCustomFinishTiles.empty() ? nullptr : m_pCustomFinishGrid;
        }

        // --- State-Lattice A* (v1.56) ---
        bool PfAStarInit();
        int PfAStarStep();               // 0=cont, 1=goal, -1=failed
        void PfAStarReconstruct();
        void PfAStarUpdatePreview();
        void PfAStarResimulatePath(int targetIdx);  // re-simulate path root→target, fill m_PfVPath
        float PfAStarHeuristic(const vec2 &pos, const vec2 &endPos);
        void PfThreadWorker();
        void PfRheaWorker();
        void PfBmctsWorker();
        bool PfEvalSequence(const PfAState &cur, int curFreeze, int tickOffset,
                const std::vector<CNetObj_PlayerInput> &inSeq, PfChunkResult &out);
        float PfSeqScore(const std::vector<vec2> &vFinishes, const PfChunkResult &res, bool freezeSupport);
        void PfSeqApplyEnd(PfAState &st, const PfChunkResult &res);
        void PfSeqAppend(std::vector<vec2> &fullTraj, std::vector<PfHookSeg> &fullSegs,
                std::vector<CNetObj_PlayerInput> &fullInputs, const PfChunkResult &res, const std::vector<CNetObj_PlayerInput> &inSeq);
        void PfSeqFinalize(PfAState cur, int curFreeze, int tickOffset, std::vector<vec2> &fullTraj,
                std::vector<PfHookSeg> &fullSegs, std::vector<CNetObj_PlayerInput> &fullInputs);
	void PfThreadStart();
	void PfThreadStop();
	void PfSpectateStart();
	void PfSpectateStop();
	void PfSpectateReset();
	bool PfSpectateUpdateCamera(vec2 &outCamPos);

	// --- Real-Time Pathfinder (v1.56.280) ---
	void RtPfResetRun();                    // start: snapshot world, build field, start worker
	void RtPfStop();                        // stop: kill worker + clear all RT state
	void UpdateRtPathfinder();              // per-tick playback + reconstruct (OnUpdate)
	void RenderRtPathfinderPath();          // OnRender visuals (path/hooks/field/prev path)
	void RtPfThreadWorker();                // planner loop (worker thread)
	void RtPfComputeFlowField();            // own flow field into m_RtPfFlowField
	const std::vector<vec2> &RtPfActiveFinishTiles() const;
	bool RtPfIsAtFinishPos(const vec2 &Pos) const; // goal test: position on an active finish tile
	void RtPfEnsureEditorGrids();
	void RtPfThreadStop();                  // join the worker + free the sim world
	void RtPfAfterEditorChange();           // invalidate the field, re-anchor if running
	// Live playback: apply the next planned input to the real player.
	// Called from CControls::SnapInput (same slot as the TAS real replay).
	bool ApplyRtPfRealInput(CNetObj_PlayerInput *pInput);
	void UpdateRtPfTileEditor();            // separate RT tile editor (forbidden + finish)
	void RenderRtPfTileEditor();
	bool RtPfTileCanBeCustomFinish(int tx, int ty);
	void RtPfAddForbiddenTile(int tx, int ty);
	void RtPfRemoveForbiddenTile(int tx, int ty);
	void RtPfAddCustomFinishTile(int tx, int ty);
	void RtPfRemoveCustomFinishTile(int tx, int ty);
	void RtPfClearForbiddenTiles();
	void RtPfClearCustomFinishTiles();

        // --- Console commands ---
        static void ConCopyMoves(IConsole::IResult *pResult, void *pUserData);
        static void ConAttackEnable(IConsole::IResult *pResult, void *pUserData);
        static void ConSetMain(IConsole::IResult *pResult, void *pUserData);
        static void ConSetTargets(IConsole::IResult *pResult, void *pUserData);
        static void ConSetBots(IConsole::IResult *pResult, void *pUserData);
        static void ConSetTargetAll(IConsole::IResult *pResult, void *pUserData);
        static void ConAttackDists(IConsole::IResult *pResult, void *pUserData);
        static void ConClientDelay(IConsole::IResult *pResult, void *pUserData);
        static void ConRescueIds(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinderGo(IConsole::IResult *pResult, void *pUserData);
        // v1.56.258: ConAttackSettings/ConAttackHookDelay/ConStandOnX/ConPathfinder*/
        // ConPathfinderRays*/ConPathfinderSnap/ConPathfinderSps removed.
        static void ConMacroLoad(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroPlay(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroRecord(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroSave(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroCapture(IConsole::IResult *pResult, void *pUserData);

        // v1.56.258: legacy individual toggle commands removed (ConStand/
        // ConAutoAim/ConAutoFire/ConAutoHook/ConMove/ConKillFrz/ConAtkMain/
        // ConHammer/ConSmartDetect/ConSmartRescue/ConAvoidFreeze/ConPfHook/
        // ConKinodynamic/ConKino*).
        static void ConRescue(IConsole::IResult *pResult, void *pUserData);
        static void ConAutoMain(IConsole::IResult *pResult, void *pUserData);
        static void ConSendDummy(IConsole::IResult *pResult, void *pUserData);

        // Pathfinder tab (Kinetix→Pathfinder) console commands
        static void ConPfLive(IConsole::IResult *pResult, void *pUserData);     // kx_pf_live 0|1|2  (0=idle, 1=running, 2=finished)
        static void ConPfPaste(IConsole::IResult *pResult, void *pUserData);       // kx_pf_paste

	// Placeholder processing for kx_send and UI Send Command
	static std::string ReplacePlaceholders(const std::string &Cmd, int DummyIndex, CGameClient *pGame);

	void SmartDummySwitch();

private:
        // =========================================================
        // Internal helpers (v1.56.32 refactor — extracted from ProcessDummy
        // and the two Compute*Pathfinder methods to kill duplication and
        // make the per-dummy tick readable.  Behavior is identical.)
        // =========================================================

        // Zero direction/jump/hook, advance the fire-release bit, mask fire,
        // and copy back into m_Controls.  Used by every early-return path.
        void ResetAndCommitInput(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy);

        // Issue a "kill" console command on the given dummy by temporarily
        // switching g_Config.m_ClDummy.  No-op safe (restores prior value).
        void IssueKillForDummy(int Dummy);

        // Detect map dimension change / 5-second reload timer.  Two slots so
        // PathfinderGo + Attack-BranchA share one timer (legacy behaviour)
        // while Attack-BranchB keeps its own independent timer.
        //   reloadSlot 0 = shared (PathfinderGo + Attack-BranchA)
        //   reloadSlot 1 = separate (Attack-BranchB / smart-rescue)
        // Returns true when LoadMapGrid() was called.
        bool MaybeReloadMapGrid(int reloadSlot);

        // The stand-distance used by the "stand when close" check, depending
        // on whether the current target is the main player and not a rescue
        // target.  Replaces the 6x duplicated ternary.
        float EffectiveStandDist(bool TargetIsMain, bool TargetIsRescue) const;

        // Hook pulse cycle (ticks) — fixed 1s pulse (v1.56.258).  Replaces 2x identical calc.
        int ComputeHookTicksCycle() const;

        // --- Helpers internals (attack.cpp) ---
        // Target selection: rescue → attack → escort.  For rescue it runs
        // ComputePathfinderRescue, leaving the rescue field in m_pfDist.
        // Returns the target client id or -1.
        int FindHelpersTarget(CGameClient *pGame, int LocalID, const vec2 &MyPos, CBotNetDummy &State,
                bool &TargetIsMain, bool &TargetIsRescue, bool &TargetExplicit);
        // Movement for the picked target: guidance field + physics plan with a
        // grid-flow fallback.  Returns true while the plan owns the inputs.
        bool MoveToHelpersTarget(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
                int LocalID, int TargetID, const vec2 &MyPos, const vec2 &TPos, float dx, bool TargetIsRescue);
        // Old AutoHook pulse: hook the target within range/LOS with no other
        // tee in between.  Frozen targets are hooked only for rescue.
        void UpdateHelpersHook(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
                int LocalID, int TargetID, const vec2 &MyPos, const vec2 &TPos, float Dist,
                bool CanShoot, bool TargetIsRescue, bool tgtFrozen);
        // Shared field-walker for targeting and idle safety: refreshes the
        // per-tile flow, replays/builds the physics plan toward (gtx, gty)
        // and falls back to grid flow.  JumpProbeDX gives the jump-over probe
        // direction (0 = use the flow's horizontal sign); BlockerId is the
        // tee the probe ignores (-1 = jump over anyone).  Returns true while
        // the plan owns the inputs.
        bool MoveAlongField(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
                int LocalID, const vec2 &MyPos, int gtx, int gty, float JumpProbeDX, int BlockerId);
        // Idle safety: an AFK bot never sits where a hook/hammer can shove it
        // into freeze — it retreats to the nearest tile with no freeze within
        // margin.  Returns 0 = nothing to do (caller idles), 1 = plan active,
        // 2 = grid fallback already drove the bot.
        int MoveToSafety(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State,
                int LocalID, const vec2 &MyPos);

        // --- ProcessDummy sub-steps (each returns true when it handled the
        //     tick and ProcessDummy should return immediately; false = fall
        //     through to the next stage). ---
        bool HandleFrozenDummy(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, int LocalID);
        bool HandleNoFeatureIdle(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State);
        bool ProcessMacroPlayback(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State);
        bool ProcessMacroRecording(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID, int64_t CurTick);
        bool ApplyClientDelayGate(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy);
        bool ProcessCopyMoves(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy);
        bool ProcessPathfinderGo(CGameClient *pGame, CNetObj_PlayerInput *pInput, int Dummy, CBotNetDummy &State, int LocalID);

};

#endif
