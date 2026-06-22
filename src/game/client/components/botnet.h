#ifndef GAME_CLIENT_COMPONENTS_BOTNET_H
#define GAME_CLIENT_COMPONENTS_BOTNET_H

#include <base/vmath.h>

#include <engine/client/enums.h>
#include <engine/console.h>
#include <engine/shared/config.h>

#include <game/client/component.h>
#include <generated/protocol.h>

#include <string>
#include <vector>

// Pathfinder constants
static const int PF_MAX_MAP_SIZE = 2048;
static const float PF_FREEZE_COST = 50.0f;
static const float PF_PLAYER_COST = 1e18f;
static const float PF_FREEZE_REPEL_WEIGHT = 3.0f;

// Kinodynamic A* (State-Lattice) constants
static const int KINO_MAX_TICKS = 128; // room for a full primitive plan

// State-Lattice A* constants
static const int LATTICE_MAX_NODES = 2000; // hard cap on A* node budget
static const int LATTICE_MAX_DEPTH = 12; // max primitives in a plan
static const int LATTICE_MAX_PRIMITIVES = 40; // 6 base + up to 32 hook angles
static const int LATTICE_PRIM_MAX_TICKS = 12; // longest single primitive
static const float LATTICE_FREEZE_PENALTY = 5.0f; // cost added per freeze tile crossed
static const float LATTICE_H_SCALE = 2.0f; // heuristic multiplier (admissible: <3.2 ticks/tile)

// =========================================================
// Kinodynamic A* — cached input package per dummy
// =========================================================
struct CKinodynamicCache
{
        CNetObj_PlayerInput aInputs[KINO_MAX_TICKS]; // the winning input package
        int NumTicks;           // how many ticks in the package
        int CurrentTick;        // which tick of the package we're currently applying
        int ComputeTick;        // game tick when this was computed
        int TargetTX, TargetTY; // target tile at compute time (for invalidation)
        vec2 BotPos;            // bot position at compute time (for invalidation)
        std::vector<vec2> vPath; // predicted positions for rendering

        CKinodynamicCache() { Reset(); }

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
        int64_t m_NextRandomAimTick;

        // Kinodynamic A* cache
        CKinodynamicCache m_KinoCache;

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
                m_HookTickTimer = 0;
                m_JumpTicks = 0;
                m_LastPos = vec2(0, 0);
                m_StuckTicks = 0;
                m_PathfinderGoActive = false;
                m_PathfinderGoPos = vec2(0, 0);
                m_NextRandomAimTick = 0;
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

// =========================================================
// CBotNet — botnet component (all inactive dummies)
// =========================================================
class CBotNet : public CComponent
{
public:
        // --- Shared settings (same for all dummies) ---
        bool m_AttackEnabled;
        bool m_AutoAim;
        bool m_AutoFire;
        bool m_AutoHook;
        bool m_MoveEnabled;
        bool m_StandEnabled;
        bool m_RescueFrozen;
        bool m_RescueAll;
        bool m_KillOnFreeze;
        bool m_AttackMain;
        bool m_AutoMain; // automatically set main ID to active dummy
        bool m_AutoHammer;
        bool m_SmartDetect;
        bool m_SmartRescue;
        bool m_AvoidFreeze;

        int m_MainID;
        bool m_AllTarget;
        bool m_TargetList[128];
        bool m_BotsList[128];
        bool m_RescueList[128];

        // String buffers for UI input fields (comma-separated IDs)
        char m_aTargetIDsStr[256];
        char m_aBotsIDsStr[256];
        char m_aRescueIDsStr[256];

        float m_FireDist;
        float m_HookDist;
        float m_RescueRadius;
        float m_TargetDist;
        float m_MainDist;
        float m_StandDist;
        float m_MainStandDist;

        bool m_LaserRescue;
        float m_LaserRescueDist;

        int m_AvoidFreezeRadius;

        int m_HookDelay;
        int m_ClientDelay;
        bool m_StandOnXOnly;

        bool m_CopyMoves;
        int m_CopyTargetID;

        bool m_RandomAim;
        int m_RandomAimInterval;

        // Pathfinder settings (shared)
        bool m_PathfinderEnabled;
        bool m_PfSimulatePlayers;
        int m_PfSimulateMethod;
        float m_PfSimulateScore;
        int m_PfRays;
        int m_PfViewRadius;
        bool m_PfSnap;
        bool m_PfHookEnabled;

        // Kinodynamic A* (State-Lattice) settings (shared)
        bool m_KinodynamicEnabled;
        int m_KinoCandidates;   // A* node budget — max nodes to expand (100-2000)
        int m_KinoTicks;        // max search depth — primitives in a plan (3-12)
        int m_KinoHookAngles;   // number of hook-angle primitives (4-32)
        int m_KinoCacheTicks;   // receding horizon: ticks to apply before replanning (1-30)
        bool m_KinoShowPath;    // render kinodynamic predicted path
        bool m_KinoShowField;   // render pathfinder vector field

        // Per-dummy runtime state
        CBotNetDummy m_aDummies[MAX_DUMMIES];

        // Map grid (shared — same map for all)
        int m_MapWidth;
        int m_MapHeight;
        unsigned char *m_pMapGrid;
        unsigned char *m_pFrontGrid;
        float *m_PfPlayerPenalty;
        bool m_MapGridLoaded;
        char m_aLastMapName[256];
        float *m_pfDist;
        bool *m_pfVisited;

        CBotNet();
        ~CBotNet();

        int Sizeof() const override { return sizeof(*this); }
        void OnConsoleInit() override;
        void OnUpdate() override;
        void OnRender() override;
        void OnReset() override;

        // Process one inactive dummy
        void ProcessDummy(int Dummy);

        // Check if botnet is active for a specific dummy
        bool IsDummyActive(int Dummy) const;

        // Reset inputs for a specific dummy
        void ResetDummyInputs(int Dummy);

        // Sync ID list string <-> bool array
        void SyncIDsToStr(const bool *pList, char *pStr, int StrSize);
        void SyncStrToIDs(const char *pStr, bool *pList);

        // Pathfinder (shared map, per-dummy flow)
        void LoadMapGrid();
        bool IsTileWalkable(int tx, int ty);
        bool IsTileFreeze(int tx, int ty);
        float GetTileCost(int tx, int ty);
        void ComputePathfinder(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State);
        void ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State);
        bool HasLineOfSightTiles(int r1, int c1, int r2, int c2);
        void ComputeFlowForTile(int r, int c, CBotNetDummy &State);
        vec2 ComputeFreezeRepel(int botTX, int botTY);
        void UpdatePlayerPenalty(int botTX, int botTY, int excludeTX, int excludeTY, int LocalID);
        void GetMovementFromFlow(const CBotNetDummy &State, bool &outLeft, bool &outRight, bool &outJump);

        // Kinodynamic A* — physics-aware input planning
        void ComputeKinodynamic(int Dummy, const vec2 &MyPos, int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State);
        void ApplyKinodynamicCache(int Dummy, CNetObj_PlayerInput *pInput, CBotNetDummy &State);
        bool IsKinodynamicCacheValid(int targetTX, int targetTY, const vec2 &MyPos) const;
        void RenderPackage(int Dummy);
        void RenderVectorField();

        // --- Console commands ---
        static void ConRandomAim(IConsole::IResult *pResult, void *pUserData);
        static void ConCopyMoves(IConsole::IResult *pResult, void *pUserData);
        static void ConAttackEnable(IConsole::IResult *pResult, void *pUserData);
        static void ConSetMain(IConsole::IResult *pResult, void *pUserData);
        static void ConSetTargets(IConsole::IResult *pResult, void *pUserData);
        static void ConSetBots(IConsole::IResult *pResult, void *pUserData);
        static void ConSetTargetAll(IConsole::IResult *pResult, void *pUserData);
        static void ConAttackSettings(IConsole::IResult *pResult, void *pUserData);
        static void ConAttackDists(IConsole::IResult *pResult, void *pUserData);
        static void ConAttackHookDelay(IConsole::IResult *pResult, void *pUserData);
        static void ConClientDelay(IConsole::IResult *pResult, void *pUserData);
        static void ConStandOnX(IConsole::IResult *pResult, void *pUserData);
        static void ConRescueIds(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinder(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinderRays(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinderRaysDist(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinderSnap(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinderSps(IConsole::IResult *pResult, void *pUserData);
        static void ConPathfinderGo(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroLoad(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroPlay(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroRecord(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroSave(IConsole::IResult *pResult, void *pUserData);
        static void ConMacroCapture(IConsole::IResult *pResult, void *pUserData);

        // Individual toggle commands (easier than c_atk_set with 15 params)
        static void ConStand(IConsole::IResult *pResult, void *pUserData);
        static void ConAutoAim(IConsole::IResult *pResult, void *pUserData);
        static void ConAutoFire(IConsole::IResult *pResult, void *pUserData);
        static void ConAutoHook(IConsole::IResult *pResult, void *pUserData);
        static void ConMove(IConsole::IResult *pResult, void *pUserData);
        static void ConRescue(IConsole::IResult *pResult, void *pUserData);
        static void ConKillFrz(IConsole::IResult *pResult, void *pUserData);
        static void ConAtkMain(IConsole::IResult *pResult, void *pUserData);
        static void ConAutoMain(IConsole::IResult *pResult, void *pUserData);
        static void ConHammer(IConsole::IResult *pResult, void *pUserData);
        static void ConSmartDetect(IConsole::IResult *pResult, void *pUserData);
        static void ConSmartRescue(IConsole::IResult *pResult, void *pUserData);
        static void ConAvoidFreeze(IConsole::IResult *pResult, void *pUserData);
        static void ConPfHook(IConsole::IResult *pResult, void *pUserData);
        static void ConKinodynamic(IConsole::IResult *pResult, void *pUserData);
        static void ConKinoCandidates(IConsole::IResult *pResult, void *pUserData);
        static void ConKinoTicks(IConsole::IResult *pResult, void *pUserData);
        static void ConKinoHookAngles(IConsole::IResult *pResult, void *pUserData);
        static void ConKinoCacheTicks(IConsole::IResult *pResult, void *pUserData);
        static void ConKinoShowPath(IConsole::IResult *pResult, void *pUserData);
        static void ConKinoShowField(IConsole::IResult *pResult, void *pUserData);
        static void ConSendDummy(IConsole::IResult *pResult, void *pUserData);

        // Placeholder processing for c_send and UI Send Command
        static std::string ReplacePlaceholders(const std::string &Cmd, int DummyIndex, CGameClient *pGame);
};

#endif
