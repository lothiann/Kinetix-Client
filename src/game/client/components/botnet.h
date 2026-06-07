#ifndef GAME_CLIENT_COMPONENTS_BOTNET_H
#define GAME_CLIENT_COMPONENTS_BOTNET_H

#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>

#include <string>
#include <vector>

// Pathfinder constants
static const int PF_MAX_MAP_SIZE = 2048;
static const float PF_FREEZE_COST = 50.0f;
static const float PF_PLAYER_COST = 1e18f;
static const float PF_FREEZE_REPEL_WEIGHT = 3.0f;

class CBotNet : public CComponent
{
public:
	bool m_RandomAim;
	int m_RandomAimInterval;
	int64_t m_NextRandomAimTick;

	bool m_CopyMoves;
	int m_CopyTargetID;
	int m_LastTargetAttackTick;
	int m_JumpTicks;

	bool m_AttackEnabled;
	int m_MainID;
	bool m_AllTarget;
	bool m_TargetList[128];
	bool m_BotsList[128];

	bool m_AutoAim;
	bool m_AutoFire;
	bool m_AutoHook;
	bool m_MoveEnabled;
	bool m_StandEnabled;
	bool m_RescueFrozen;
	bool m_RescueAll;
	bool m_KillOnFreeze;
	bool m_AttackMain;
	bool m_AutoHammer;
	bool m_SmartDetect;
	bool m_SmartRescue;
	bool m_AvoidFreeze;

	int m_ClientDelay;
	bool m_StandOnXOnly;

	float m_FireDist;
	float m_HookDist;
	float m_RescueRadius;
	float m_TargetDist;
	float m_MainDist;
	float m_StandDist;

	bool m_RescueList[128];

	int m_HookDelay;
	int m_HookTickTimer;

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

	// --- Pathfinder ---
	bool m_PathfinderEnabled;
	bool m_PfSimulatePlayers;
	int m_PfSimulateMethod; // 0 = walls, 1 = score
	float m_PfSimulateScore;
	int m_PfRays;
	int m_PfViewRadius;
	bool m_PfSnap;
	bool m_PfHookEnabled;
	vec2 m_PfHookTile;

	int m_MapWidth;
	int m_MapHeight;
	unsigned char *m_pMapGrid;
	unsigned char *m_pFrontGrid;
	float *m_PfPlayerPenalty;
	bool m_MapGridLoaded;
	char m_aLastMapName[256];

	float *m_pfDist;
	bool *m_pfVisited;

	vec2 m_FlowDir;
	int m_FlowTargetTX, m_FlowTargetTY;

	int m_LastTargetTX, m_LastTargetTY;
	int m_LastBotTX, m_LastBotTY;
	bool m_PathFound;

	vec2 m_LastPos;
	int m_StuckTicks;

	bool m_PathfinderGoActive;
	vec2 m_PathfinderGoPos;

	CBotNet();
	~CBotNet();

	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnUpdate() override;
	void OnReset() override;

	// Pathfinder
	void LoadMapGrid();
	bool IsTileWalkable(int tx, int ty);
	bool IsTileFreeze(int tx, int ty);
	float GetTileCost(int tx, int ty);
	void ComputePathfinder(int botTX, int botTY, int targetTX, int targetTY);
	void ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY);
	bool HasLineOfSightTiles(int r1, int c1, int r2, int c2);
	void ComputeFlowForTile(int r, int c);
	vec2 ComputeFreezeRepel(int botTX, int botTY);
	void UpdatePlayerPenalty(int botTX, int botTY, int excludeTX, int excludeTY, int LocalID);
	void GetMovementFromFlow(bool &outLeft, bool &outRight, bool &outJump);

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
};

#endif
