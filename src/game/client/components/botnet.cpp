#include "botnet.h"

#include <game/client/components/bot_control.h>

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
// CONSTRUCTOR / DESTRUCTOR
// =========================================================

CBotNet::CBotNet()
{
        // Shared settings — defaults match user's reference layout
        m_AttackEnabled = false;
        m_AutoAim = m_AutoFire = m_AutoHook = m_MoveEnabled = m_StandEnabled = m_PathfinderEnabled = true;
        m_RescueFrozen = m_AutoHammer = m_SmartDetect = m_SmartRescue = m_AvoidFreeze = m_PfSimulatePlayers = true;
        m_PfSimulateMethod = 1; // SPS on by default
        m_RescueAll = m_KillOnFreeze = m_AttackMain = m_AutoMain = false;
        m_SmartDetect = true;
        m_SmartRescue = true;
        m_AvoidFreeze = true;
        m_StandOnXOnly = false;
        m_AllTarget = false;
        m_RandomAim = m_CopyMoves = false;
        m_MainID = -1;
        m_RandomAimInterval = 100;
        m_FireDist = 65.0f;
        m_HookDist = 400.0f;
        m_RescueRadius = 500.0f;
        m_TargetDist = 300.0f;
        m_MainDist = INFINITY;
        m_StandDist = 64.0f;
        m_MainStandDist = 128.0f;
        m_LaserRescue = false;
        m_LaserRescueDist = 800.0f;
        m_AvoidFreezeRadius = 2;
        m_HookDelay = 1000;
        m_ClientDelay = 0;
        for(int i = 0; i < 128; i++)
        {
                m_TargetList[i] = false;
                m_BotsList[i] = false;
                m_RescueList[i] = false;
        }
        m_CopyTargetID = -1;

        // String buffers for UI input fields
        m_aTargetIDsStr[0] = '\0';
        m_aBotsIDsStr[0] = '\0';
        m_aRescueIDsStr[0] = '\0';

        // Pathfinder init (shared)
        m_PfSimulateScore = 25.0f;
        m_PfRays = 24;
        m_PfViewRadius = 6;
        m_PfSnap = false;
        m_PfHookEnabled = false;

        // Kinodynamic A* (State-Lattice) defaults
        m_KinodynamicEnabled = false;
        m_KinoCandidates = 800; // A* node budget — enough for hook-swing maneuvers
        m_KinoTicks = 15;       // max search depth (primitives) — covers full jump+hook+hold sequences
        m_KinoHookAngles = 8;   // hook-fire primitives (angles in [pi/6, 5pi/6])
        m_KinoCacheTicks = 128; // receding horizon window — 128 = KINO_MAX_TICKS = apply full plan
        // (was 10, which cut plans mid-maneuver and caused the bot to oscillate
        //  in hook-swing scenarios.  Drift check (48px) is the real invalidation.)
        m_KinoShowPath = true;
        m_KinoShowField = false;
        m_KinoAggressive = false;

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker.
        m_PfState = PF_STATE_IDLE;
        m_PfChunkCount = 0;
        m_PfStartPos = vec2(0, 0);
        m_PfCurPos = vec2(0, 0);
        m_PfCurVel = vec2(0, 0);
        m_PfCurHookState = 0;       // HOOK_IDLE
        m_PfCurHookPos = vec2(0, 0);
        m_PfCurHookDir = vec2(0, 0);
        m_PfCurHookTick = 0;
        m_PfCurFreezeTime = 0;
        m_PfCurJumped = 0;
        m_PfTickCounter = 0;
        m_PfFlowField = nullptr;
        m_PfTotalFreezeTicks = 0;
        m_PfBacktrackIdx = 0;

        // State-Lattice A* state
        m_PfANodes.clear();
        m_PfAOpen.clear();
        m_PfABestG.clear();
        m_PfAGoalIdx = -1;
        m_PfAStarted = false;
        m_PfAPathReady = false;
        m_PfAExpandCount = 0;
        m_PfFullInputs.clear();
        m_PfFullInputsIdx = 0;

        m_MapWidth = 0;
        m_MapHeight = 0;
        m_pMapGrid = nullptr;
        m_pFrontGrid = nullptr;
        m_PfPlayerPenalty = nullptr;
        m_MapGridLoaded = false;
        m_pfDist = nullptr;
        m_pfVisited = nullptr;

        // Per-dummy state is auto-initialized by CBotNetDummy constructor
}

CBotNet::~CBotNet()
{
        if(m_pMapGrid) delete[] m_pMapGrid;
        if(m_pFrontGrid) delete[] m_pFrontGrid;
        if(m_pfDist) delete[] m_pfDist;
        if(m_pfVisited) delete[] m_pfVisited;
        if(m_PfPlayerPenalty) delete[] m_PfPlayerPenalty;
        if(m_PfFlowField) delete[] m_PfFlowField;
}

// =========================================================
// ON CONSOLE INIT
// =========================================================

void CBotNet::OnConsoleInit()
{
        Console()->Register("c_random_aim", "i[on] ?i[ms]", CFGFLAG_CLIENT, ConRandomAim, this, "Random Aim");
        Console()->Register("c_copy_moves", "i[target_id]", CFGFLAG_CLIENT, ConCopyMoves, this, "Copy Moves");
        Console()->Register("c_attack", "i[on]", CFGFLAG_CLIENT, ConAttackEnable, this, "Attack Mode");
        Console()->Register("c_main", "i[id]", CFGFLAG_CLIENT, ConSetMain, this, "Main ID");
        Console()->Register("c_targets", "s[ids]", CFGFLAG_CLIENT, ConSetTargets, this, "Targets");
        Console()->Register("c_bots", "s[ids]", CFGFLAG_CLIENT, ConSetBots, this, "Bots (allies)");
        Console()->Register("c_target_all", "i[on]", CFGFLAG_CLIENT, ConSetTargetAll, this, "Attack all (targets become blacklist)");
        Console()->Register("c_atk_set", "iiiiiiiiiiiiiiii", CFGFLAG_CLIENT, ConAttackSettings, this, "Settings (16 params: aim,fire,hook,move,stand,rescue,rescueAll,smartDetect,smartRescue,killFrz,atkMain,hammer,simPlayers,avoidFreeze,pfHook,kinodynamic)");
        Console()->Register("c_atk_dists", "fffffff", CFGFLAG_CLIENT, ConAttackDists, this, "Radii (fire, hook, rescue, target, main, stand, main stand)");
        Console()->Register("c_atk_hook_delay", "i[ms]", CFGFLAG_CLIENT, ConAttackHookDelay, this, "Hook Delay");
        Console()->Register("c_client_delay", "i[ms]", CFGFLAG_CLIENT, ConClientDelay, this, "Client delay in ms (0 to disable)");
        Console()->Register("c_stand_on_x", "i[on]", CFGFLAG_CLIENT, ConStandOnX, this, "Stand only on X axis");
        Console()->Register("c_rescue_ids", "s[ids]", CFGFLAG_CLIENT, ConRescueIds, this, "Rescue/Unrescue IDs");
        Console()->Register("c_atk_pathfinder", "i[on]", CFGFLAG_CLIENT, ConPathfinder, this, "Enable A* pathfinder movement");
        Console()->Register("c_atk_pathfinder_rays", "i[rays]", CFGFLAG_CLIENT, ConPathfinderRays, this, "Number of raycast rays (12-90)");
        Console()->Register("c_atk_pathfinder_rays_dist", "i[tiles]", CFGFLAG_CLIENT, ConPathfinderRaysDist, this, "Raycast view radius (1-128 tiles)");
        Console()->Register("c_atk_pathfinder_snap", "i[on]", CFGFLAG_CLIENT, ConPathfinderSnap, this, "Snap to tile center when flow is vertical");
        Console()->Register("c_atk_pathfinder_sps", "i[method]", CFGFLAG_CLIENT, ConPathfinderSps, this, "Simulate players method: 0=walls, 1=score(push)");
        Console()->Register("c_pathfinder_go", "i[on] ?i[x] ?i[y]", CFGFLAG_CLIENT, ConPathfinderGo, this, "Move to position: 0=disable, 1 x y=enable and set target");
        Console()->Register("c_macro_load", "s[path]", CFGFLAG_CLIENT, ConMacroLoad, this, "Load macro from file");
        Console()->Register("c_macro_play", "i[on]", CFGFLAG_CLIENT, ConMacroPlay, this, "Play loaded macro (1=start, 0=stop)");
        Console()->Register("c_macro_record", "i[on]", CFGFLAG_CLIENT, ConMacroRecord, this, "Record macro (1=start, 0=stop)");
        Console()->Register("c_macro_save", "s[path]", CFGFLAG_CLIENT, ConMacroSave, this, "Save recorded macro to file");
        Console()->Register("c_macro_capture", "i[id]", CFGFLAG_CLIENT, ConMacroCapture, this, "Set capture ID for macro recording");

        // Individual toggle commands
        Console()->Register("c_stand", "i[on]", CFGFLAG_CLIENT, ConStand, this, "Stand when close");
        Console()->Register("c_autoaim", "i[on]", CFGFLAG_CLIENT, ConAutoAim, this, "Auto aim at target");
        Console()->Register("c_autofire", "i[on]", CFGFLAG_CLIENT, ConAutoFire, this, "Auto fire at target");
        Console()->Register("c_autohook", "i[on]", CFGFLAG_CLIENT, ConAutoHook, this, "Auto hook target");
        Console()->Register("c_move", "i[on]", CFGFLAG_CLIENT, ConMove, this, "Move towards target");
        Console()->Register("c_rescue", "i[on]", CFGFLAG_CLIENT, ConRescue, this, "Rescue frozen allies");
        Console()->Register("c_kill_frz", "i[on]", CFGFLAG_CLIENT, ConKillFrz, this, "Kill on freeze");
        Console()->Register("c_atk_main", "i[on]", CFGFLAG_CLIENT, ConAtkMain, this, "Attack main target");
        Console()->Register("c_auto_main", "i[on]", CFGFLAG_CLIENT, ConAutoMain, this, "Auto-set main to active dummy");
        Console()->Register("c_hammer", "i[on]", CFGFLAG_CLIENT, ConHammer, this, "Auto hammer");
        Console()->Register("c_smart_detect", "i[on]", CFGFLAG_CLIENT, ConSmartDetect, this, "Smart detect for rescue");
        Console()->Register("c_smart_rescue", "i[on]", CFGFLAG_CLIENT, ConSmartRescue, this, "Smart rescue pathfinder");
        Console()->Register("c_avoid_freeze", "i[on]", CFGFLAG_CLIENT, ConAvoidFreeze, this, "Avoid freeze tiles");
        Console()->Register("c_pf_hook", "i[on]", CFGFLAG_CLIENT, ConPfHook, this, "Pathfinder hook to walls");
        Console()->Register("c_kinodynamic", "i[on]", CFGFLAG_CLIENT, ConKinodynamic, this, "Enable Kinodynamic A* movement");
        Console()->Register("c_kino_candidates", "i[count]", CFGFLAG_CLIENT, ConKinoCandidates, this, "Kinodynamic: A* node budget (100-2000)");
        Console()->Register("c_kino_ticks", "i[depth]", CFGFLAG_CLIENT, ConKinoTicks, this, "Kinodynamic: max search depth in primitives (3-12)");
        Console()->Register("c_kino_hook_angles", "i[angles]", CFGFLAG_CLIENT, ConKinoHookAngles, this, "Kinodynamic: number of hook angles (4-32)");
        Console()->Register("c_kino_cache_ticks", "i[ticks]", CFGFLAG_CLIENT, ConKinoCacheTicks, this, "Kinodynamic: cache duration in ticks (1-128, 128=full plan)");
        Console()->Register("c_kino_show_path", "i[on]", CFGFLAG_CLIENT, ConKinoShowPath, this, "Kinodynamic: show predicted path");
        Console()->Register("c_kino_show_field", "i[on]", CFGFLAG_CLIENT, ConKinoShowField, this, "Kinodynamic: show vector field");
        Console()->Register("c_kino_aggressive", "i[on]", CFGFLAG_CLIENT, ConKinoAggressive, this, "Kinodynamic: force-apply best partial plan even when no primitive improved on start");
        Console()->Register("c_send", "s[dummyids] r[command]", CFGFLAG_CLIENT, ConSendDummy, this, "Execute command on dummies (-1=all, 0,1,2=specific)");

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker
        Console()->Register("c_pf_live", "i[state]", CFGFLAG_CLIENT, ConPfLive, this, "Pathfinder tab: 0=idle, 1=running, 2=finished");
        Console()->Register("c_pf_algorithm", "i[algo]", CFGFLAG_CLIENT, ConPfAlgorithm, this, "Pathfinder tab: algorithm index 0-14");
        Console()->Register("c_pf_chunk_size", "i[ticks]", CFGFLAG_CLIENT, ConPfChunkSize, this, "Pathfinder tab: ticks per chunk (1-50)");
        Console()->Register("c_pf_candidates", "i[n]", CFGFLAG_CLIENT, ConPfCandidates, this, "Pathfinder tab: max search nodes (50-2000)");
        Console()->Register("c_pf_hook_angles", "i[angles]", CFGFLAG_CLIENT, ConPfHookAngles, this, "Pathfinder tab: hook fan-raycast rays (4-32)");
        Console()->Register("c_pf_horizon", "i[ticks]", CFGFLAG_CLIENT, ConPfHorizon, this, "Pathfinder tab: ticks applied per chunk (1-50)");
}

// =========================================================
// PATHFINDER
// =========================================================

void CBotNet::LoadMapGrid()
{
        m_MapGridLoaded = false;

        if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        CGameClient *pGame = GameClient();

        CCollision *pCol = pGame->Collision();
        if(!pCol)
                return;

        CLayers *pLayers = pGame->Layers();
        if(!pLayers)
                return;

        const CMapItemLayerTilemap *pGameLayer = pLayers->GameLayer();
        if(!pGameLayer)
                return;

        int Width = pGameLayer->m_Width;
        int Height = pGameLayer->m_Height;
        if(Width <= 0 || Height <= 0 || Width > PF_MAX_MAP_SIZE || Height > PF_MAX_MAP_SIZE)
                return;

        IMap *pMap = pGame->Map();
        if(!pMap)
                return;

        if(m_pMapGrid)
                delete[] m_pMapGrid;
        if(m_pFrontGrid)
                delete[] m_pFrontGrid;
        if(m_pfDist)
                delete[] m_pfDist;
        if(m_pfVisited)
                delete[] m_pfVisited;
        if(m_PfPlayerPenalty)
                delete[] m_PfPlayerPenalty;

        m_MapWidth = Width;
        m_MapHeight = Height;
        int Size = Width * Height;

        m_pMapGrid = new unsigned char[Size];
        m_pFrontGrid = new unsigned char[Size];
        m_pfDist = new float[Size];
        m_pfVisited = new bool[Size];
        m_PfPlayerPenalty = new float[Size];

        mem_zero(m_pMapGrid, Size);
        mem_zero(m_pFrontGrid, Size);

        CTile *pTiles = (CTile *)pMap->GetData(pGameLayer->m_Data);
        if(pTiles)
        {
                for(int i = 0; i < Size; i++)
                        m_pMapGrid[i] = pTiles[i].m_Index;
        }
        else
        {
                for(int y = 0; y < Height; y++)
                {
                        for(int x = 0; x < Width; x++)
                        {
                                int idx = y * Width + x;
                                int col = pCol->CheckPoint(vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f));
                                if(col == TILE_SOLID)
                                        m_pMapGrid[idx] = TILE_SOLID;
                                else if(col == TILE_DEATH)
                                        m_pMapGrid[idx] = TILE_DEATH;
                                else if(col == TILE_NOHOOK)
                                        m_pMapGrid[idx] = TILE_NOHOOK;
                        }
                }
        }

        const CMapItemLayerTilemap *pFrontLayer = pLayers->FrontLayer();
        if(pFrontLayer)
        {
                CTile *pFrontTiles = (CTile *)pMap->GetData(pFrontLayer->m_Front);
                if(pFrontTiles)
                {
                        int frontSize = pf_min(Width * Height, pFrontLayer->m_Width * pFrontLayer->m_Height);
                        for(int i = 0; i < frontSize; i++)
                                m_pFrontGrid[i] = pFrontTiles[i].m_Index;
                }
        }

        for(int i = 0; i < Size; i++)
        {
                m_pfDist[i] = 1e18f;
                m_pfVisited[i] = false;
                m_PfPlayerPenalty[i] = 0.0f;
        }

        m_MapGridLoaded = true;
        m_aLastMapName[0] = '\0';
        if(Client())
        {
                const char *pName = Client()->MapDownloadName();
                if(pName)
                        str_copy(m_aLastMapName, pName, sizeof(m_aLastMapName));
        }

        dbg_msg("botnet_pf", "Map grid loaded: %dx%d", Width, Height);
}

bool CBotNet::IsTileWalkable(int tx, int ty)
{
        if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
                return false;
        int idx = ty * m_MapWidth + tx;
        unsigned char tile = m_pMapGrid[idx];
        if(tile == TILE_SOLID || tile == TILE_DEATH || tile == TILE_NOHOOK)
                return false;
        unsigned char ftile = m_pFrontGrid[idx];
        if(ftile == TILE_SOLID || ftile == TILE_DEATH)
                return false;
        return true;
}

bool CBotNet::IsTileFreeze(int tx, int ty)
{
        if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
                return false;
        int idx = ty * m_MapWidth + tx;
        unsigned char tile = m_pMapGrid[idx];
        if(tile == TILE_FREEZE || tile == TILE_DFREEZE || tile == TILE_LFREEZE)
                return true;
        unsigned char ftile = m_pFrontGrid[idx];
        if(ftile == TILE_FREEZE || ftile == TILE_DFREEZE || ftile == TILE_LFREEZE)
                return true;
        return false;
}

float CBotNet::GetTileCost(int tx, int ty)
{
        if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
                return -1.0f;
        int idx = ty * m_MapWidth + tx;
        unsigned char tile = m_pMapGrid[idx];
        if(tile == TILE_SOLID || tile == TILE_DEATH || tile == TILE_NOHOOK)
                return -1.0f;
        unsigned char ftile = m_pFrontGrid[idx];
        if(ftile == TILE_SOLID || ftile == TILE_DEATH)
                return -1.0f;

        float cost = 1.0f;
        if(tile == TILE_FREEZE || tile == TILE_DFREEZE)
                cost += PF_FREEZE_COST;
        if(ftile == TILE_FREEZE || ftile == TILE_DFREEZE)
                cost += PF_FREEZE_COST;

        if(m_PfSimulatePlayers && m_PfPlayerPenalty)
        {
                cost += m_PfPlayerPenalty[idx];
        }

        return cost;
}

bool CBotNet::HasLineOfSightTiles(int r1, int c1, int r2, int c2)
{
        float x1 = c1 + 0.5f, y1 = r1 + 0.5f;
        float x2 = c2 + 0.5f, y2 = r2 + 0.5f;
        float d = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
        int steps = pf_max((int)(d * 3.0f), 1);
        int prevR = -1, prevC = -1;

        for(int i = 0; i <= steps; i++)
        {
                float t = (float)i / steps;
                float x = x1 + (x2 - x1) * t;
                float y = y1 + (y2 - y1) * t;
                int r = (int)y, c = (int)x;
                if(r == prevR && c == prevC)
                        continue;
                if(!IsTileWalkable(c, r))
                        return false;
                if(prevR >= 0 && abs(r - prevR) + abs(c - prevC) == 2)
                {
                        if(!IsTileWalkable(c, prevR) && !IsTileWalkable(prevC, r))
                                return false;
                }
                prevR = r;
                prevC = c;
        }
        return true;
}

void CBotNet::ComputePathfinder(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
        if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
                return;

        int Size = m_MapWidth * m_MapHeight;

        for(int i = 0; i < Size; i++)
        {
                m_pfDist[i] = 1e18f;
                m_pfVisited[i] = false;
        }

        targetTX = pf_clamp(targetTX, 0, m_MapWidth - 1);
        targetTY = pf_clamp(targetTY, 0, m_MapHeight - 1);
        botTX = pf_clamp(botTX, 0, m_MapWidth - 1);
        botTY = pf_clamp(botTY, 0, m_MapHeight - 1);

        if(!IsTileWalkable(targetTX, targetTY) || !IsTileWalkable(botTX, botTY))
        {
                State.m_PathFound = false;
                State.m_FlowDir = vec2(0, 0);
                State.m_PfHookTile = vec2(0, 0);
                return;
        }

        const float SQRT2 = 1.4142135623730951f;

        int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

        auto heuristic = [&](int r, int c) -> float {
                int dy = abs(r - botTY);
                int dx = abs(c - botTX);
                return pf_maxf((float)dy, (float)dx) + (SQRT2 - 1.0f) * pf_minf((float)dy, (float)dx);
        };

        int tIdx = targetTY * m_MapWidth + targetTX;
        m_pfDist[tIdx] = 0.0f;

        std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open;
        open.push({heuristic(targetTY, targetTX), 0.0f, targetTY, targetTX});

        while(!open.empty())
        {
                PfNode cur = open.top();
                open.pop();

                if(m_pfVisited[cur.r * m_MapWidth + cur.c])
                        continue;
                m_pfVisited[cur.r * m_MapWidth + cur.c] = true;

                if(cur.r == botTY && cur.c == botTX)
                        break;

                for(int d = 0; d < 8; d++)
                {
                        int nr = cur.r + dr[d];
                        int nc = cur.c + dc[d];
                        if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
                                continue;

                        float tileCost = GetTileCost(nc, nr);
                        if(tileCost < 0)
                                continue;

                        if(abs(dr[d]) + abs(dc[d]) == 2)
                        {
                                if(!IsTileWalkable(cur.c, cur.r + dr[d]) || !IsTileWalkable(cur.c + dc[d], cur.r))
                                        continue;
                        }

                        float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 * tileCost : tileCost;
                        float newG = cur.g + moveCost;
                        int nIdx = nr * m_MapWidth + nc;

                        if(newG < m_pfDist[nIdx])
                        {
                                m_pfDist[nIdx] = newG;
                                float f = newG + heuristic(nr, nc);
                                open.push({f, newG, nr, nc});
                        }
                }
        }

        State.m_PathFound = m_pfVisited[botTY * m_MapWidth + botTX];
        State.m_LastTargetTX = targetTX;
        State.m_LastTargetTY = targetTY;
        State.m_LastBotTX = botTX;
        State.m_LastBotTY = botTY;

        if(State.m_PathFound)
        {
                ComputeFlowForTile(botTY, botTX, State);
        }
        else
        {
                State.m_FlowDir = vec2(0, 0);
                State.m_PfHookTile = vec2(0, 0);
        }
}

// =========================================================
// PATHFINDER RESCUE
// =========================================================
void CBotNet::ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
        if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
                return;

        targetTX = pf_clamp(targetTX, 0, m_MapWidth - 1);
        targetTY = pf_clamp(targetTY, 0, m_MapHeight - 1);
        botTX = pf_clamp(botTX, 0, m_MapWidth - 1);
        botTY = pf_clamp(botTY, 0, m_MapHeight - 1);

        if(!IsTileWalkable(botTX, botTY))
        {
                State.m_PathFound = false;
                State.m_FlowDir = vec2(0, 0);
                return;
        }

        int hookRadiusTiles = (int)(m_HookDist / 32.0f);
        if(hookRadiusTiles < 1)
                hookRadiusTiles = 1;

        struct SafeTile
        {
                int tx, ty;
                int dist;
        };
        SafeTile safeTiles[1024];
        int numSafe = 0;

        for(int i = 0; i < m_PfRays; i++)
        {
                float angle = 2.0f * pi * i / m_PfRays;
                float rayDX = cosf(angle);
                float rayDY = sinf(angle);

                for(int step = 1; step <= hookRadiusTiles; step++)
                {
                        int tc = targetTX + (int)roundf(rayDX * step);
                        int tr = targetTY + (int)roundf(rayDY * step);

                        if(tc < 0 || tr < 0 || tc >= m_MapWidth || tr >= m_MapHeight)
                                break;

                        if(!IsTileWalkable(tc, tr))
                                break;

                        if(IsTileFreeze(tc, tr))
                                continue;

                        if(numSafe < 1024)
                        {
                                safeTiles[numSafe].tx = tc;
                                safeTiles[numSafe].ty = tr;
                                int ddx = tc - targetTX;
                                int ddy = tr - targetTY;
                                safeTiles[numSafe].dist = ddx * ddx + ddy * ddy;
                                numSafe++;
                        }
                }
        }

        if(numSafe == 0)
        {
                State.m_PathFound = false;
                State.m_FlowDir = vec2(0, 0);
                return;
        }

        for(int a = 0; a < numSafe - 1; a++)
                for(int b = a + 1; b < numSafe; b++)
                        if(safeTiles[b].dist < safeTiles[a].dist)
                        {
                                SafeTile tmp = safeTiles[a];
                                safeTiles[a] = safeTiles[b];
                                safeTiles[b] = tmp;
                        }

        const float SQRT2 = 1.4142135623730951f;
        int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

        auto heuristic = [&](int r, int c) -> float {
                int dy = abs(r - botTY);
                int dx = abs(c - botTX);
                return pf_maxf((float)dy, (float)dx) + (SQRT2 - 1.0f) * pf_minf((float)dy, (float)dx);
        };

        int Size = m_MapWidth * m_MapHeight;

        int startIdx = 0;
        while(startIdx < numSafe)
        {
                int curDist = safeTiles[startIdx].dist;

                for(int i = 0; i < Size; i++)
                {
                        m_pfDist[i] = 1e18f;
                        m_pfVisited[i] = false;
                }

                std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open;

                int endIdx = startIdx;
                while(endIdx < numSafe && safeTiles[endIdx].dist == curDist)
                {
                        int tx = safeTiles[endIdx].tx;
                        int ty = safeTiles[endIdx].ty;
                        int idx = ty * m_MapWidth + tx;
                        m_pfDist[idx] = 0.0f;
                        open.push({heuristic(ty, tx), 0.0f, ty, tx});
                        endIdx++;
                }

                while(!open.empty())
                {
                        PfNode cur = open.top();
                        open.pop();

                        if(m_pfVisited[cur.r * m_MapWidth + cur.c])
                                continue;
                        m_pfVisited[cur.r * m_MapWidth + cur.c] = true;

                        if(cur.r == botTY && cur.c == botTX)
                                break;

                        for(int d = 0; d < 8; d++)
                        {
                                int nr = cur.r + dr[d];
                                int nc = cur.c + dc[d];
                                if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
                                        continue;

                                if(IsTileFreeze(nc, nr))
                                        continue;

                                float tileCost = GetTileCost(nc, nr);
                                if(tileCost < 0)
                                        continue;

                                if(abs(dr[d]) + abs(dc[d]) == 2)
                                {
                                        if(!IsTileWalkable(cur.c, cur.r + dr[d]) || !IsTileWalkable(cur.c + dc[d], cur.r))
                                                continue;
                                }

                                float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 * tileCost : tileCost;
                                float newG = cur.g + moveCost;
                                int nIdx = nr * m_MapWidth + nc;

                                if(newG < m_pfDist[nIdx])
                                {
                                        m_pfDist[nIdx] = newG;
                                        float f = newG + heuristic(nr, nc);
                                        open.push({f, newG, nr, nc});
                                }
                        }
                }

                if(m_pfVisited[botTY * m_MapWidth + botTX])
                {
                        State.m_PathFound = true;
                        State.m_LastTargetTX = targetTX;
                        State.m_LastTargetTY = targetTY;
                        State.m_LastBotTX = botTX;
                        State.m_LastBotTY = botTY;
                        ComputeFlowForTile(botTY, botTX, State);
                        return;
                }

                startIdx = endIdx;
        }

        State.m_PathFound = false;
        State.m_FlowDir = vec2(0, 0);
}

void CBotNet::ComputeFlowForTile(int r, int c, CBotNetDummy &State)
{
        State.m_PfHookTile = vec2(0, 0);

        if(!m_MapGridLoaded)
                return;
        if(r < 0 || c < 0 || r >= m_MapHeight || c >= m_MapWidth)
                return;

        int idx = r * m_MapWidth + c;
        if(m_pfDist[idx] >= 1e17f)
        {
                State.m_FlowDir = vec2(0, 0);
                return;
        }

        float currentD = m_pfDist[idx];
        float bestDist = currentD;
        float bestDX = 0, bestDY = 0;

        // === Step 1: A* rays ===
        for(int i = 0; i < m_PfRays; i++)
        {
                float angle = 2.0f * pi * i / m_PfRays;
                float rayDX = cosf(angle);
                float rayDY = sinf(angle);

                float rayFreezePenalty = 0;

                for(int step = 1; step <= m_PfViewRadius; step++)
                {
                        int tr = r + (int)roundf(rayDY * step);
                        int tc = c + (int)roundf(rayDX * step);
                        if(tr < 0 || tc < 0 || tr >= m_MapHeight || tc >= m_MapWidth)
                                break;
                        if(!IsTileWalkable(tc, tr))
                                break;

                        int fIdx = tr * m_MapWidth + tc;

                        if(m_PfSimulatePlayers && m_PfPlayerPenalty && m_PfPlayerPenalty[fIdx] > 0)
                        {
                                if(m_PfSimulateMethod == 0)
                                        break;
                                else
                                        rayFreezePenalty += m_PfPlayerPenalty[fIdx];
                        }

                        if(m_pMapGrid[fIdx] == TILE_FREEZE || m_pMapGrid[fIdx] == TILE_DFREEZE || m_pMapGrid[fIdx] == TILE_LFREEZE)
                                rayFreezePenalty += PF_FREEZE_COST;
                        if(m_pFrontGrid[fIdx] == TILE_FREEZE || m_pFrontGrid[fIdx] == TILE_DFREEZE || m_pFrontGrid[fIdx] == TILE_LFREEZE)
                                rayFreezePenalty += PF_FREEZE_COST;

                        int tIdx = tr * m_MapWidth + tc;
                        float adjustedDist = m_pfDist[tIdx] + rayFreezePenalty;

                        if(adjustedDist < currentD && HasLineOfSightTiles(r, c, tr, tc))
                        {
                                if(adjustedDist < bestDist)
                                {
                                        bestDist = adjustedDist;
                                        float dx = (tc + 0.5f) - (c + 0.5f);
                                        float dy = (tr + 0.5f) - (r + 0.5f);
                                        float len = sqrtf(dx * dx + dy * dy);
                                        if(len > 0.001f)
                                        {
                                                bestDX = dx / len;
                                                bestDY = dy / len;
                                        }
                                }
                        }
                }
        }

        // === Step 2: Freeze repulsion ===
        if(m_AvoidFreeze)
        {
                vec2 repel = ComputeFreezeRepel(c, r);
                float repelLen = sqrtf(repel.x * repel.x + repel.y * repel.y);
                if(repelLen > 0.001f)
                {
                        float normX = repel.x / repelLen;
                        float normY = repel.y / repelLen;

                        float repelWeight = repelLen * PF_FREEZE_REPEL_WEIGHT;
                        float virtualDist = currentD - repelWeight;

                        if(virtualDist < bestDist)
                        {
                                bestDist = virtualDist;
                                bestDX = normX;
                                bestDY = normY;
                        }
                }
        }

        // === Step 3: Pathfinder Hook ===
        if(m_PfHookEnabled)
        {
                float flLen = sqrtf(bestDX * bestDX + bestDY * bestDY);
                if(flLen > 0.001f)
                {
                        float fDirX = bestDX / flLen;
                        float fDirY = bestDY / flLen;
                        int hookRange = pf_min(m_PfViewRadius, (int)(m_HookDist / 32.0f));
                        int bestStep = 999999;

                        for(int i = 0; i < m_PfRays; i++)
                        {
                                float angle = 2.0f * pi * i / m_PfRays;
                                float rdx = cosf(angle);
                                float rdy = sinf(angle);

                                if(rdx * fDirX + rdy * fDirY < 0.7f)
                                        continue;

                                for(int step = 1; step <= hookRange; step++)
                                {
                                        int tr = r + (int)roundf(rdy * step);
                                        int tc = c + (int)roundf(rdx * step);
                                        if(tr < 0 || tc < 0 || tr >= m_MapHeight || tc >= m_MapWidth)
                                                break;

                                        int hIdx = tr * m_MapWidth + tc;

                                        if(m_PfPlayerPenalty[hIdx] > 0.0f)
                                                break;

                                        unsigned char tile = m_pMapGrid[hIdx];

                                        if(tile == TILE_SOLID)
                                        {
                                                if(step < bestStep)
                                                {
                                                        bestStep = step;
                                                        State.m_PfHookTile = vec2(tc * 32.0f + 16.0f, tr * 32.0f + 16.0f);
                                                }
                                                break;
                                        }

                                        if(!IsTileWalkable(tc, tr))
                                                break;
                                }
                        }
                }
        }

        float len = sqrtf(bestDX * bestDX + bestDY * bestDY);
        if(len > 0.001f)
        {
                State.m_FlowDir = vec2(bestDX / len, bestDY / len);
        }
        else
        {
                State.m_FlowDir = vec2(0, 0);
        }
}

vec2 CBotNet::ComputeFreezeRepel(int botTX, int botTY)
{
        vec2 repel = vec2(0, 0);
        const int scanRadius = m_AvoidFreezeRadius;

        for(int i = 0; i < m_PfRays; i++)
        {
                float angle = 2.0f * pi * i / m_PfRays;
                float rayDX = cosf(angle);
                float rayDY = sinf(angle);

                for(int step = 1; step <= scanRadius; step++)
                {
                        int tc = botTX + (int)roundf(rayDX * step);
                        int tr = botTY + (int)roundf(rayDY * step);

                        if(tc < 0 || tr < 0 || tc >= m_MapWidth || tr >= m_MapHeight)
                                break;
                        if(!IsTileWalkable(tc, tr))
                                break;

                        if(IsTileFreeze(tc, tr))
                        {
                                float dx = (float)(tc - botTX);
                                float dy = (float)(tr - botTY);
                                float distSq = dx * dx + dy * dy;
                                if(distSq < 0.001f)
                                        continue;

                                repel.x += -dx / distSq;
                                repel.y += -dy / distSq;
                        }
                }
        }

        return repel;
}

void CBotNet::UpdatePlayerPenalty(int botTX, int botTY, int excludeTX, int excludeTY, int LocalID)
{
        if(!m_PfPlayerPenalty)
                return;

        mem_zero(m_PfPlayerPenalty, m_MapWidth * m_MapHeight * sizeof(float));

        if(!m_PfSimulatePlayers && !m_PfHookEnabled)
                return;

        CGameClient *pGame = GameClient();
        for(int i = 0; i < 128; i++)
        {
                if(i == LocalID || !pGame->m_aClients[i].m_Active || !pGame->m_Snap.m_aCharacters[i].m_Active)
                        continue;
                int ptx = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X / 32.0f);
                int pty = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y / 32.0f);
                if(ptx < 0 || pty < 0 || ptx >= m_MapWidth || pty >= m_MapHeight)
                        continue;
                if(ptx == botTX && pty == botTY)
                        continue;
                int idx = pty * m_MapWidth + ptx;
                if(m_pMapGrid[idx] == TILE_SOLID || m_pMapGrid[idx] == TILE_DEATH)
                        continue;

                bool isTarget = (ptx == excludeTX && pty == excludeTY);

                if(m_PfSimulatePlayers && !isTarget)
                {
                        if(m_PfSimulateMethod == 1)
                                m_PfPlayerPenalty[idx] += m_PfSimulateScore;
                        else
                                m_PfPlayerPenalty[idx] += PF_PLAYER_COST;
                }

                if(m_PfHookEnabled)
                        m_PfPlayerPenalty[idx] += 1.0f;
        }
}

void CBotNet::GetMovementFromFlow(const CBotNetDummy &State, bool &outLeft, bool &outRight, bool &outJump)
{
        outLeft = false;
        outRight = false;
        outJump = false;

        if(State.m_FlowDir.x == 0.0f && State.m_FlowDir.y == 0.0f)
                return;

        float angle = atan2f(State.m_FlowDir.y, State.m_FlowDir.x);
        float snap = roundf(angle / (pi / 4.0f)) * (pi / 4.0f);

        float snapX = cosf(snap);
        float snapY = sinf(snap);

        if(snapX > 0.5f)
                outRight = true;
        else if(snapX < -0.5f)
                outLeft = true;

        if(snapY < -0.5f)
                outJump = true;
}

// =========================================================
// INPUT HELPERS
// =========================================================

static void SetDirection(CGameClient *pGame, int Dummy, bool Left, bool Right)
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

static void SetMousePos(CGameClient *pGame, int Dummy, vec2 Pos)
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

// =========================================================
// ON UPDATE — loops over all inactive dummies
// =========================================================

void CBotNet::OnUpdate()
{
        if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        // Pathfinder tab: run one chunk per update if RUNNING.  Placed before the
        // dummy loop so it never interferes with the active dummy's inputs (the
        // tab only simulates + renders; it does NOT write inputs to any dummy).
        UpdatePathfinder();

        CGameClient *pGame = GameClient();
        if(!pGame->m_Snap.m_pLocalInfo)
                return;

        // Auto-bots: automatically add all local dummies to bots list
        // so they don't attack each other and will rescue each other
        if(m_AttackEnabled)
        {
                for(int d = 0; d < MAX_DUMMIES; d++)
                {
                        int id = pGame->m_aLocalIds[d];
                        if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
                                m_BotsList[id] = true;
                }
        }

        // Auto main: automatically set main ID to the currently active dummy
        if(m_AutoMain && m_AttackEnabled)
        {
                int ActiveID = pGame->m_aLocalIds[g_Config.m_ClDummy];
                if(ActiveID >= 0 && m_MainID != ActiveID)
                        m_MainID = ActiveID;
        }

        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                if(D == g_Config.m_ClDummy)
                        continue;
                if(D != 0 && !Client()->DummyConnected(D))
                        continue;
                int LocalID = pGame->m_aLocalIds[D];
                if(LocalID < 0 || LocalID >= 64)
                        continue;
                if(!pGame->m_aClients[LocalID].m_Active)
                        continue;
                ProcessDummy(D);
        }
}

// =========================================================
// PROCESS DUMMY — core logic for one inactive dummy
// =========================================================

void CBotNet::ProcessDummy(int Dummy)
{
        CGameClient *pGame = GameClient();
        CBotNetDummy &State = m_aDummies[Dummy];
        CNetObj_PlayerInput *pInput = &pGame->m_aDummyInput[Dummy];
        int LocalID = pGame->m_aLocalIds[Dummy];
        int64_t CurTick = Client()->GameTick(0);

        // Skip frozen dummies — they can't move/act, processing them wastes CPU
        if(pGame->m_aClients[LocalID].m_FreezeEnd > 0 || pGame->m_aClients[LocalID].m_DeepFrozen)
        {
                // Only keep kill-on-freeze check
                if(m_KillOnFreeze && pGame->m_aClients[LocalID].m_FreezeEnd > 0)
                {
                        int OldDummy = g_Config.m_ClDummy;
                        g_Config.m_ClDummy = Dummy;
                        Console()->ExecuteLine("kill", -1, -1);
                        g_Config.m_ClDummy = OldDummy;
                }
                pInput->m_Direction = 0;
                pInput->m_Jump = 0;
                pInput->m_Hook = 0;
                if((pInput->m_Fire & 1) != 0)
                        pInput->m_Fire++;
                pInput->m_Fire &= INPUT_STATE_MASK;
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                return;
        }

        // Early return if no feature active — reset inputs
        if(!m_AttackEnabled && !m_CopyMoves && !m_RandomAim && !State.m_MacroPlaying && !State.m_PathfinderGoActive)
        {
                pInput->m_Direction = 0;
                pInput->m_Jump = 0;
                pInput->m_Hook = 0;
                if((pInput->m_Fire & 1) != 0)
                        pInput->m_Fire++;
                pInput->m_Fire &= INPUT_STATE_MASK;
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                return;
        }

        // ===== MACRO PLAYING =====
        if(State.m_MacroPlaying)
        {
                if(State.m_MacroSleepTicks > 0)
                {
                        int64_t Now = Client()->GameTick(0);
                        if(Now >= State.m_MacroSleepUntilTick)
                                State.m_MacroSleepTicks = 0;
                        else
                        {
                                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                                return;
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
                                return;
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
                        pInput->m_Jump = 0;
                        pInput->m_Hook = 0;
                        if((pInput->m_Fire & 1) != 0)
                                pInput->m_Fire++;
                        pInput->m_Fire &= INPUT_STATE_MASK;
                        GameClient()->m_BotControl.ActionStop(Dummy);
                        dbg_msg("botnet_macro", "Playback finished.");
                }

                pInput->m_PlayerFlags |= 1;
                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                return;
        }

        // ===== MACRO RECORDING =====
        if(State.m_MacroRecording)
        {
                if(State.m_MacroRecordBuffer.size() > 100000)
                {
                        dbg_msg("botnet_macro", "EMERGENCY STOP: Buffer exceeded 100,000 lines! Stopping recording to prevent crash.");
                        State.m_MacroRecording = false;
                        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                        return;
                }

                int captureID = (State.m_MacroCaptureID >= 0 && State.m_MacroCaptureID < 128) ? State.m_MacroCaptureID : LocalID;

                int curDir = 0;
                int curJump = 0;
                int curHook = 0;
                int curFire = 0;
                int curAimX = 0;
                int curAimY = 0;
                int curWeapon = 0;

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
                                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                                return;
                        }
                }

                bool changed = false;
                if(curDir != State.m_LastRecordedDir)
                        changed = true;
                if(curJump != State.m_LastRecordedJump)
                        changed = true;
                if(curHook != State.m_LastRecordedHook)
                        changed = true;
                if(curAimX != State.m_LastRecordedAimX || curAimY != State.m_LastRecordedAimY)
                        changed = true;
                if(curFire != State.m_LastRecordedFire)
                        changed = true;
                if(curWeapon != State.m_LastRecordedWeapon)
                        changed = true;

                if(changed)
                {
                        int64_t deltaTick = CurTick - State.m_LastMacroRecordTick;
                        int TickSpeed = Client()->GameTickSpeed();
                        if(TickSpeed <= 0)
                                TickSpeed = 50;
                        int deltaMs = (int)(deltaTick * 1000 / TickSpeed);

                        if(deltaMs > 1 && !State.m_MacroRecordBuffer.empty())
                        {
                                std::string sleepLine = "sleep " + std::to_string(deltaMs);
                                State.m_MacroRecordBuffer.push_back(sleepLine);
                        }

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
                        {
                                if(curJump != 0)
                                        State.m_MacroRecordBuffer.push_back("input jump 1");
                                else
                                        State.m_MacroRecordBuffer.push_back("input jump 0");
                        }
                        if(curHook != State.m_LastRecordedHook)
                        {
                                if(curHook != 0)
                                        State.m_MacroRecordBuffer.push_back("input hook 1");
                                else
                                        State.m_MacroRecordBuffer.push_back("input hook 0");
                        }
                        if(curAimX != State.m_LastRecordedAimX || curAimY != State.m_LastRecordedAimY)
                        {
                                std::string aimLine = "aim " + std::to_string(curAimX) + " " + std::to_string(curAimY);
                                State.m_MacroRecordBuffer.push_back(aimLine);
                        }
                        if(curFire != State.m_LastRecordedFire)
                        {
                                State.m_MacroRecordBuffer.push_back("input fire");
                        }
                        if(curWeapon != State.m_LastRecordedWeapon)
                        {
                                std::string wpnLine = "input weapon " + std::to_string(curWeapon + 1);
                                State.m_MacroRecordBuffer.push_back(wpnLine);
                        }

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
                return;
        }

        // ===== CLIENT DELAY =====
        if(m_ClientDelay > 0)
        {
                static int64_t s_LastActionTick[MAX_DUMMIES] = {};
                int64_t Now = Client()->GameTick(0);
                int64_t TickDelay = ((int64_t)m_ClientDelay * Client()->GameTickSpeed()) / 1000;
                if(TickDelay < 1)
                        TickDelay = 1;
                if(Now - s_LastActionTick[Dummy] < TickDelay)
                {
                        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                        return;
                }
                s_LastActionTick[Dummy] = Now;
        }

        // ===== KILL ON FREEZE =====
        if(m_KillOnFreeze && pGame->m_aClients[LocalID].m_FreezeEnd > 0)
        {
                int OldDummy = g_Config.m_ClDummy;
                g_Config.m_ClDummy = Dummy;
                Console()->ExecuteLine("kill", -1, -1);
                g_Config.m_ClDummy = OldDummy;
        }

        // ===== COPY MOVES =====
        if(m_CopyMoves && m_CopyTargetID >= 0 && m_CopyTargetID < 128)
        {
                const auto &TChar = pGame->m_Snap.m_aCharacters[m_CopyTargetID];
                if(pGame->m_aClients[m_CopyTargetID].m_Active && TChar.m_Active)
                {
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
                        return;
                }
        }

        // ===== PATHFINDER GO =====
        if(State.m_PathfinderGoActive)
        {
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
                        return;
                }

                if(m_PathfinderEnabled)
                {
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
                        static int64_t s_LastMapReload = 0;
                        int64_t now = time_get();
                        if(now - s_LastMapReload > time_freq() * 5)
                        {
                                mapChanged = true;
                                s_LastMapReload = now;
                        }
                        if(!m_MapGridLoaded || mapChanged)
                                LoadMapGrid();

                        if(m_MapGridLoaded)
                        {
                                UpdatePlayerPenalty(botTX, botTY, targetTX, targetTY, LocalID);

                                bool needRecalc = false;
                                if(targetTX != State.m_LastTargetTX || targetTY != State.m_LastTargetTY)
                                        needRecalc = true;
                                if(!State.m_PathFound)
                                        needRecalc = true;
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
                return;
        }

        // ===== ATTACK =====
        if(m_AttackEnabled)
        {
                int TargetID = -1;
                float MinDist = 1000000.0f;
                vec2 MyPos((float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Y);
                bool TargetIsMain = false;
                bool TargetIsRescue = false;

                // ===== RESCUE FROZEN + SMART RESCUE =====
                bool usingSmartRescue = false;
                if(m_RescueFrozen)
                {
                        int rescueCandidates[128];
                        float rescueDist[128];
                        int numCandidates = 0;

                        for(int i = 0; i < 128; i++)
                        {
                                if(i == LocalID || !pGame->m_aClients[i].m_Active)
                                        continue;
                                if(pGame->m_aClients[i].m_FreezeEnd == 0)
                                        continue;

                                bool canRescue = false;
                                if(i == m_MainID)
                                        canRescue = true;
                                else if(m_BotsList[i])
                                        canRescue = true;
                                else if(m_RescueAll)
                                {
                                        canRescue = m_AllTarget ? m_TargetList[i] : !m_TargetList[i];
                                        if(m_RescueList[i])
                                                canRescue = false;
                                }
                                else if(m_RescueList[i])
                                        canRescue = true;

                                if(!canRescue)
                                        continue;

                                vec2 TPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                                if(!m_SmartDetect)
                                {
                                        if(pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) > 0)
                                                continue;
                                }
                                float d = distance(MyPos, TPos);
                                if(d < m_RescueRadius)
                                {
                                        rescueCandidates[numCandidates] = i;
                                        rescueDist[numCandidates] = d;
                                        numCandidates++;
                                }
                        }

                        for(int a = 0; a < numCandidates - 1; a++)
                                for(int b = a + 1; b < numCandidates; b++)
                                        if(rescueDist[b] < rescueDist[a])
                                        {
                                                int tmp = rescueCandidates[a];
                                                rescueCandidates[a] = rescueCandidates[b];
                                                rescueCandidates[b] = tmp;
                                                float td = rescueDist[a];
                                                rescueDist[a] = rescueDist[b];
                                                rescueDist[b] = td;
                                        }

                        for(int j = 0; j < numCandidates; j++)
                        {
                                int i = rescueCandidates[j];
                                TargetID = i;
                                TargetIsRescue = true;

                                if(m_SmartRescue && m_PathfinderEnabled && m_MapGridLoaded)
                                {
                                        int botTX = (int)(MyPos.x / 32.0f);
                                        int botTY = (int)(MyPos.y / 32.0f);
                                        int resTX = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X / 32.0f);
                                        int resTY = (int)((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y / 32.0f);

                                        UpdatePlayerPenalty(botTX, botTY, resTX, resTY, LocalID);

                                        ComputePathfinderRescue(botTX, botTY, resTX, resTY, State);

                                        if(State.m_PathFound)
                                        {
                                                usingSmartRescue = true;
                                                break;
                                        }
                                }
                                else
                                {
                                        break;
                                }

                                TargetID = -1;
                                TargetIsRescue = false;
                        }
                }

                if(TargetID == -1)
                {
                        for(int i = 0; i < 128; i++)
                        {
                                if(i == LocalID || i == m_MainID || !pGame->m_aClients[i].m_Active)
                                        continue;
                                if(m_BotsList[i])
                                        continue;
                                if(pGame->m_aClients[i].m_FreezeEnd != 0)
                                        continue;

                                bool isEnemy = (m_AllTarget ? !m_TargetList[i] : m_TargetList[i]);
                                if(!isEnemy)
                                        continue;

                                vec2 TPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                                if(pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) > 0)
                                        continue;
                                float d = distance(MyPos, TPos);

                                if(m_TargetDist > 0 && d > m_TargetDist)
                                        continue;

                                if(d < MinDist)
                                {
                                        MinDist = d;
                                        TargetID = i;
                                }
                        }
                }

                if(TargetID == -1 && m_MainID >= 0 && m_MainID < 128)
                {
                        if(pGame->m_aClients[m_MainID].m_Active)
                        {
                                vec2 MainPos((float)pGame->m_Snap.m_aCharacters[m_MainID].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[m_MainID].m_Cur.m_Y);
                                float MainDist = distance(MyPos, MainPos);
                                if(MainDist <= m_MainDist)
                                {
                                        TargetID = m_MainID;
                                        TargetIsMain = true;
                                }
                        }
                }

                if(TargetID != -1)
                {
                        auto &TChar = pGame->m_Snap.m_aCharacters[TargetID].m_Cur;
                        float dx = (float)TChar.m_X - MyPos.x;
                        float dy = (float)TChar.m_Y - MyPos.y;
                        float Dist = distance(MyPos, vec2((float)TChar.m_X, (float)TChar.m_Y));

                        if(m_AutoAim)
                                SetMousePos(pGame, Dummy, vec2(dx, dy));

                        bool standActive = false;

                        if(m_MoveEnabled)
                        {
                                bool left = false, right = false, jump = false;

                                if(m_PathfinderEnabled && !usingSmartRescue)
                                {
                                        // --- PATHFINDER MOVEMENT ---
                                        {
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
                                                static int64_t s_LastMapReload = 0;
                                                int64_t now = time_get();
                                                if(now - s_LastMapReload > time_freq() * 5)
                                                {
                                                        mapChanged = true;
                                                        s_LastMapReload = now;
                                                }
                                                if(!m_MapGridLoaded || mapChanged)
                                                        LoadMapGrid();
                                        }

                                        if(m_MapGridLoaded)
                                        {
                                                int botTX = (int)(MyPos.x / 32.0f);
                                                int botTY = (int)(MyPos.y / 32.0f);
                                                int targetTX = (int)((float)TChar.m_X / 32.0f);
                                                int targetTY = (int)((float)TChar.m_Y / 32.0f);

                                                UpdatePlayerPenalty(botTX, botTY, targetTX, targetTY, LocalID);

                                                bool needRecalc = false;
                                                if(targetTX != State.m_LastTargetTX || targetTY != State.m_LastTargetTY)
                                                        needRecalc = true;
                                                if(!State.m_PathFound && (State.m_LastTargetTX == -1 || targetTX != State.m_LastTargetTX || targetTY != State.m_LastTargetTY))
                                                        needRecalc = true;
                                                if(State.m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                                {
                                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                                needRecalc = true;
                                                }
                                                if(m_PfSimulatePlayers)
                                                        needRecalc = true;

                                                if(needRecalc && IsTileWalkable(targetTX, targetTY))
                                                        ComputePathfinder(botTX, botTY, targetTX, targetTY, State);

                                                if(State.m_PathFound && (botTX != State.m_LastBotTX || botTY != State.m_LastBotTY))
                                                {
                                                        ComputeFlowForTile(botTY, botTX, State);
                                                        State.m_LastBotTX = botTX;
                                                        State.m_LastBotTY = botTY;
                                                }

                                                // --- KINODYNAMIC A* MOVEMENT ---
                                                bool kinoNearFreeze = false;
                                                if(m_AvoidFreeze && m_MapGridLoaded)
                                                {
                                                        vec2 repel = ComputeFreezeRepel(botTX, botTY);
                                                        kinoNearFreeze = (repel.x * repel.x + repel.y * repel.y) > 0.0001f;
                                                }

                                                if(m_KinodynamicEnabled && State.m_PathFound && !kinoNearFreeze)
                                                {
                                                        CKinodynamicCache &kinoCache = State.m_KinoCache;

                                                        // Check if cache needs recomputation
                                                        bool kinoNeedRecompute = false;
                                                        if(!kinoCache.IsValid())
                                                                kinoNeedRecompute = true;
                                                        else if(kinoCache.IsExpired(m_KinoCacheTicks))
                                                                kinoNeedRecompute = true;
                                                        else if(kinoCache.TargetTX != targetTX || kinoCache.TargetTY != targetTY)
                                                                kinoNeedRecompute = true;
                                                        else
                                                        {
                                                                // Check if bot drifted from the *expected* position on the
                                                                // planned path (not from the compute-time start — normal
                                                                // movement is not drift).  Compare against vPath[CurrentTick].
                                                                vec2 expectedPos = kinoCache.BotPos;
                                                                if(kinoCache.CurrentTick >= 0 && kinoCache.CurrentTick < (int)kinoCache.vPath.size())
                                                                        expectedPos = kinoCache.vPath[kinoCache.CurrentTick];
                                                                float drift = distance(MyPos, expectedPos);
                                                                if(drift > 48.0f) // 1.5 tiles off the planned trajectory
                                                                        kinoNeedRecompute = true;
                                                        }

                                                        if(kinoNeedRecompute)
                                                                ComputeKinodynamic(Dummy, MyPos, botTX, botTY, targetTX, targetTY, State);

                                                        // Apply cached kinodynamic inputs
                                                        if(kinoCache.IsValid())
                                                        {
                                                                // Check if bot should stand (close enough to target)
                                                                bool kinoStandActive = false;
                                                                float standDist = (TargetIsMain && !m_AttackMain && !TargetIsRescue) ? m_MainStandDist : m_StandDist;
                                                                if(m_StandEnabled && !kinoNearFreeze &&
                                                                        ((!m_StandOnXOnly && Dist < standDist) ||
                                                                                (m_StandOnXOnly && absolute(dx) < standDist)))
                                                                {
                                                                        kinoStandActive = true;
                                                                }

                                                                if(kinoStandActive)
                                                                {
                                                                        // Stand: clear movement, let normal SetDirection handle it
                                                                        left = false;
                                                                        right = false;
                                                                        jump = false;
                                                                        pInput->m_Hook = 0;
                                                                        // Don't goto after_kino_movement — let SetDirection + stand logic run
                                                                }
                                                                else
                                                                {
                                                                        ApplyKinodynamicCache(Dummy, pInput, State);
                                                                        // Skip the normal movement flow
                                                                        goto after_kino_movement;
                                                                }
                                                        }

                                                        // Cache invalid after compute (no valid path found) — fall through to normal movement
                                                }

                                                // Normal pathfinder movement — used when:
                                                // 1. Kinodynamic is disabled
                                                // 2. Bot is near freeze (kinodynamic skips to let avoid-freeze logic work)
                                                // 3. Kinodynamic cache is invalid (no valid path found by simulation)
                                                if(!m_KinodynamicEnabled || kinoNearFreeze || !State.m_KinoCache.IsValid())
                                                {
                                                        GetMovementFromFlow(State, left, right, jump);
                                                        // When falling back from kinodynamic to normal movement, clear hook
                                                        // because auto-hook code below is skipped when kinodynamic is enabled
                                                        if(m_KinodynamicEnabled)
                                                                pInput->m_Hook = 0;
                                                }

                                                if(m_PfSnap && State.m_PathFound && !left && !right)
                                                {
                                                        int snapTX = (int)roundf(MyPos.x / 32.0f);
                                                        float tileCenterX = snapTX * 32.0f + 16.0f;
                                                        float offsetX = MyPos.x - tileCenterX;

                                                        if(offsetX > 0)
                                                                left = true;
                                                        else if(offsetX < 0)
                                                                right = true;
                                                }

                                                bool nearFreeze = false;
                                                if(m_AvoidFreeze && m_MapGridLoaded)
                                                {
                                                        vec2 repel = ComputeFreezeRepel(botTX, botTY);
                                                        nearFreeze = (repel.x * repel.x + repel.y * repel.y) > 0.0001f;
                                                }
                                                float standDist = (TargetIsMain && !m_AttackMain && !TargetIsRescue) ? m_MainStandDist : m_StandDist;
                                                if(m_StandEnabled && !nearFreeze &&
                                                        ((!m_StandOnXOnly && Dist < standDist) ||
                                                                (m_StandOnXOnly && absolute(dx) < standDist)))
                                                {
                                                        left = false;
                                                        right = false;
                                                        standActive = true;
                                                }

                                                {
                                                        bool doJump = jump;

                                                        if(!m_PfSimulatePlayers)
                                                        {
                                                                vec2 IntersectPos;
                                                                int HitPlayer = pGame->IntersectCharacter(MyPos, MyPos + vec2((dx > 0 ? 1.0f : -1.0f) * 40.0f, 0), IntersectPos, LocalID);
                                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                                        doJump = true;
                                                        }

                                                        jump = doJump;
                                                }
                                        }
                                }
                                else if(m_PathfinderEnabled && usingSmartRescue)
                                {
                                        // --- SMART RESCUE PATHFINDER MOVEMENT ---
                                        {
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
                                                static int64_t s_LastMapReload2 = 0;
                                                int64_t now = time_get();
                                                if(now - s_LastMapReload2 > time_freq() * 5)
                                                {
                                                        mapChanged = true;
                                                        s_LastMapReload2 = now;
                                                }
                                                if(!m_MapGridLoaded || mapChanged)
                                                        LoadMapGrid();
                                        }

                                        if(m_MapGridLoaded)
                                        {
                                                int botTX = (int)(MyPos.x / 32.0f);
                                                int botTY = (int)(MyPos.y / 32.0f);

                                                int resTX = (int)((float)TChar.m_X / 32.0f);
                                                int resTY = (int)((float)TChar.m_Y / 32.0f);

                                                bool needRecalc = false;
                                                if(resTX != State.m_LastTargetTX || resTY != State.m_LastTargetTY)
                                                        needRecalc = true;
                                                if(!State.m_PathFound)
                                                        needRecalc = true;
                                                if(State.m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                                {
                                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                                needRecalc = true;
                                                }
                                                if(m_PfSimulatePlayers)
                                                        needRecalc = true;

                                                UpdatePlayerPenalty(botTX, botTY, -1, -1, LocalID);

                                                if(needRecalc)
                                                        ComputePathfinderRescue(botTX, botTY, resTX, resTY, State);

                                                if(State.m_PathFound && (botTX != State.m_LastBotTX || botTY != State.m_LastBotTY))
                                                {
                                                        ComputeFlowForTile(botTY, botTX, State);
                                                        State.m_LastBotTX = botTX;
                                                        State.m_LastBotTY = botTY;
                                                }

                                                GetMovementFromFlow(State, left, right, jump);

                                                bool nearFreeze = false;
                                                if(m_AvoidFreeze && m_MapGridLoaded)
                                                {
                                                        vec2 repel = ComputeFreezeRepel(botTX, botTY);
                                                        nearFreeze = (repel.x * repel.x + repel.y * repel.y) > 0.0001f;
                                                }
                                                if(m_StandEnabled && !nearFreeze &&
                                                        ((!m_StandOnXOnly && Dist < ((TargetIsMain && !m_AttackMain && !TargetIsRescue) ? m_MainStandDist : m_StandDist)) ||
                                                                (m_StandOnXOnly && absolute(dx) < ((TargetIsMain && !m_AttackMain && !TargetIsRescue) ? m_MainStandDist : m_StandDist))))
                                                {
                                                        left = false;
                                                        right = false;
                                                        standActive = true;
                                                }

                                                if(m_PfSnap && State.m_PathFound && !left && !right)
                                                {
                                                        int snapTX = (int)roundf(MyPos.x / 32.0f);
                                                        float tileCenterX = snapTX * 32.0f + 16.0f;
                                                        float offsetX = MyPos.x - tileCenterX;

                                                        if(offsetX > 0)
                                                                left = true;
                                                        else if(offsetX < 0)
                                                                right = true;
                                                }
                                        }
                                }
                                else
                                {
                                        if(!m_StandEnabled || (!m_StandOnXOnly && Dist >= ((TargetIsMain && !m_AttackMain && !TargetIsRescue) ? m_MainStandDist : m_StandDist)) || (m_StandOnXOnly && absolute(dx) >= ((TargetIsMain && !m_AttackMain && !TargetIsRescue) ? m_MainStandDist : m_StandDist)))
                                        {
                                                left = (dx < -20.0f);
                                                right = (dx > 20.0f);
                                        }
                                        else
                                        {
                                                standActive = true;
                                        }

                                        SetDirection(pGame, Dummy, left, right);

                                        {
                                                bool doJump = false;

                                                if(pGame->Collision()->CheckPoint(vec2(MyPos.x + (dx > 0 ? 35 : -35), MyPos.y)))
                                                        doJump = true;

                                                if(dy < -60.0f && absolute(dx) < 128.0f)
                                                        doJump = true;

                                                vec2 IntersectPos;
                                                int HitPlayer = pGame->IntersectCharacter(MyPos, MyPos + vec2((dx > 0 ? 1.0f : -1.0f) * 40.0f, 0), IntersectPos, LocalID);
                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                        doJump = true;

                                                if(doJump)
                                                {
                                                        if(State.m_JumpTicks == 0)
                                                        {
                                                                pInput->m_Jump = 1;
                                                                State.m_JumpTicks = 1;
                                                        }
                                                        else
                                                        {
                                                                pInput->m_Jump = 0;
                                                                State.m_JumpTicks = 0;
                                                        }
                                                }
                                                else
                                                {
                                                        pInput->m_Jump = 0;
                                                        State.m_JumpTicks = 0;
                                                }
                                        }

                                        goto after_movement;
                                }

                                SetDirection(pGame, Dummy, left, right);

                                if(jump)
                                {
                                        if(State.m_JumpTicks == 0)
                                        {
                                                pInput->m_Jump = 1;
                                                State.m_JumpTicks = 1;
                                        }
                                        else
                                        {
                                                pInput->m_Jump = 0;
                                                State.m_JumpTicks = 0;
                                        }
                                }
                                else
                                {
                                        pInput->m_Jump = 0;
                                        State.m_JumpTicks = 0;
                                }
                        }

        after_kino_movement:
                after_movement:

                        bool CanShoot = !TargetIsMain || m_AttackMain;
                        if(m_AutoFire && Dist < m_FireDist && CanShoot)
                                pInput->m_Fire++;

                        // Laser rescue: skip when Kinodynamic A* is active (it manages hook/aim itself)
                        bool usingLaserRescue = false;
                        if(!m_KinodynamicEnabled && m_LaserRescue)
                        {
                                if(TargetIsRescue)
                                {
                                        vec2 TPos((float)TChar.m_X, (float)TChar.m_Y);
                                        int targetMapIdx = pGame->Collision()->GetPureMapIndex(TPos);
                                        int targetTile = pGame->Collision()->GetTileIndex(targetMapIdx);
                                        int targetFrontTile = pGame->Collision()->GetFrontTileIndex(targetMapIdx);
                                        bool targetInFreezeTile = (targetTile == TILE_FREEZE || targetFrontTile == TILE_FREEZE);

                                        if(!targetInFreezeTile)
                                        {
                                                bool hasLaser = pGame->m_aClients[LocalID].m_Predicted.m_aWeapons[4].m_Got; // WEAPON_LASER=4

                                                if(hasLaser)
                                                {
                                                        bool blocked = (pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) != 0);
                                                        if(!blocked)
                                                        {
                                                                vec2 IntersectPos;
                                                                int HitPlayer = pGame->IntersectCharacter(MyPos, TPos, IntersectPos, LocalID);
                                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                                        blocked = true;
                                                        }

                                                        if(!blocked && Dist < m_LaserRescueDist)
                                                        {
                                                                SetMousePos(pGame, Dummy, vec2(dx, dy));
                                                                pInput->m_WantedWeapon = 5; // WEAPON_LASER + 1 (1-based)
                                                                pInput->m_Fire++;
                                                                usingLaserRescue = true;
                                                        }
                                                }
                                        }
                                }
                        }

                        if(m_AutoHammer && pInput->m_Fire > 0 && !usingLaserRescue)
                                pInput->m_WantedWeapon = 1;

                        // Auto-hook: skip when Kinodynamic A* is active (it manages hook itself)
                        if(!m_KinodynamicEnabled)
                        {
                                if(m_AutoHook && Dist < m_HookDist && CanShoot)
                                {
                                        vec2 TPos((float)TChar.m_X, (float)TChar.m_Y);
                                        bool canHook = (pGame->Collision()->IntersectLine(MyPos, TPos, NULL, NULL) == 0);
                                        if(canHook)
                                        {
                                                vec2 IntersectPos;
                                                int HitPlayer = pGame->IntersectCharacter(MyPos, TPos, IntersectPos, LocalID);
                                                if(HitPlayer != -1 && HitPlayer != TargetID)
                                                        canHook = false;
                                        }

                                        if(canHook)
                                        {
                                                int TicksCycle = (50 * m_HookDelay / 1000);
                                                if(TicksCycle < 2)
                                                        TicksCycle = 2;
                                                State.m_HookTickTimer++;
                                                if(State.m_HookTickTimer >= TicksCycle)
                                                        State.m_HookTickTimer = 0;
                                                pInput->m_Hook = (State.m_HookTickTimer < TicksCycle - 2);
                                        }
                                        else
                                        {
                                                pInput->m_Hook = 0;
                                                State.m_HookTickTimer = 0;
                                        }
                                }
                                else
                                {
                                        pInput->m_Hook = 0;
                                        State.m_HookTickTimer = 0;
                                }
                        }

                        // === Pathfinder Hook === (skipped when Kinodynamic A* is active — it manages hook itself)
                        if(!m_KinodynamicEnabled)
                        {
                                if(!m_PfHookEnabled || standActive || pInput->m_Hook != 0 ||
                                        (State.m_PfHookTile.x == 0 && State.m_PfHookTile.y == 0))
                                {
                                        State.m_HookTickTimer = 0;
                                }
                                else
                                {
                                        int TicksCycle = (50 * m_HookDelay / 1000);
                                        if(TicksCycle < 2)
                                                TicksCycle = 2;

                                        float hDx = State.m_PfHookTile.x - MyPos.x;
                                        float hDy = State.m_PfHookTile.y - MyPos.y;
                                        float hDist = sqrtf(hDx * hDx + hDy * hDy);
                                        if(hDist > 0.001f && hDist < m_HookDist)
                                        {
                                                SetMousePos(pGame, Dummy, vec2(hDx, hDy));
                                                pInput->m_Hook = 1;
                                        }

                                        if(pInput->m_Hook != 0)
                                        {
                                                State.m_HookTickTimer++;
                                                if(State.m_HookTickTimer >= TicksCycle)
                                                {
                                                        pInput->m_Hook = 0;
                                                        State.m_HookTickTimer = 0;
                                                }
                                        }
                                        else
                                        {
                                                State.m_HookTickTimer = 0;
                                        }
                                }
                        }

                        pInput->m_PlayerFlags |= 1;
                }
                else
                {
                        SetDirection(pGame, Dummy, false, false);
                        pInput->m_Hook = 0;
                        pInput->m_Fire = 0;
                        pInput->m_WantedWeapon = 0;
                        State.m_HookTickTimer = 0;
                }
        }

        // ===== RANDOM AIM =====
        if(m_RandomAim && !m_AttackEnabled && !m_CopyMoves)
        {
                if(CurTick >= State.m_NextRandomAimTick)
                {
                        GameClient()->m_BotControl.ActionOverrideAim((rand() % 2001) - 1000, (rand() % 2001) - 1000, Dummy);
                        int TS = Client()->GameTickSpeed();
                        State.m_NextRandomAimTick = CurTick + (int64_t)(TS != 0 ? TS : 50) * m_RandomAimInterval / 1000;
                }
        }

        // Always copy back at end
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
}

// =========================================================
// IS DUMMY ACTIVE
// =========================================================

bool CBotNet::IsDummyActive(int Dummy) const
{
        if(m_AttackEnabled || m_CopyMoves || m_RandomAim || m_PathfinderEnabled)
                return true;
        if(Dummy >= 0 && Dummy < MAX_DUMMIES)
        {
                const CBotNetDummy &S = m_aDummies[Dummy];
                if(S.m_MacroPlaying || S.m_PathfinderGoActive)
                        return true;
        }
        return false;
}

// =========================================================
// RESET DUMMY INPUTS
// =========================================================

void CBotNet::ResetDummyInputs(int Dummy)
{
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;
        CGameClient *pGame = GameClient();
        CNetObj_PlayerInput *pInput = &pGame->m_aDummyInput[Dummy];
        pInput->m_Direction = 0;
        pInput->m_Jump = 0;
        pInput->m_Hook = 0;
        if((pInput->m_Fire & 1) != 0)
                pInput->m_Fire++;
        pInput->m_Fire &= INPUT_STATE_MASK;
        pGame->m_Controls.m_aInputData[Dummy] = *pInput;
}

// =========================================================
// ON RESET
// =========================================================

void CBotNet::OnReset()
{
        m_AttackEnabled = false;
        m_CopyMoves = false;
        m_RandomAim = false;
        m_KinodynamicEnabled = false;
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                m_aDummies[D].Reset();
                ResetDummyInputs(D);
        }
}

void CBotNet::OnRender()
{
        // Pathfinder tab: render the accumulated trajectory whenever the state
        // machine has produced a path (RUNNING or FINISHED).  This is INDEPENDENT
        // of m_KinodynamicEnabled so the tab works even when kinodynamic A* is off.
        if(m_PfState != PF_STATE_IDLE)
                RenderPathfinderPath();

        if(!m_KinodynamicEnabled)
                return;

        // Render kinodynamic package trajectory and vector field
        if(m_KinoShowField)
                RenderVectorField();
        if(m_KinoShowPath)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(!IsDummyActive(D))
                                continue;
                        RenderPackage(D);
                }
        }
}

// =========================================================
// CONSOLE COMMANDS
// =========================================================

void CBotNet::ConMacroLoad(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        const char *path = pResult->GetString(0);
        dbg_msg("botnet_macro", "ConMacroLoad called: %s", path);
        if(!path || !path[0])
                return;

        std::ifstream file(path);
        if(!file.is_open())
        {
                dbg_msg("botnet_macro", "Failed to open macro file: %s", path);
                if(p->Console())
                        p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Failed to open macro file");
                return;
        }

        std::vector<std::string> lines;
        std::string line;
        while(std::getline(file, line))
        {
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if(!line.empty())
                        lines.push_back(line);
        }
        file.close();

        // Load into all dummies
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_MacroPlayLines = lines;

        dbg_msg("botnet_macro", "Loaded %d macro lines", (int)lines.size());
        if(p->Console())
        {
                char aBuf[64];
                str_format(aBuf, sizeof(aBuf), "Loaded %d macro lines", (int)lines.size());
                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", aBuf);
        }
}

void CBotNet::ConMacroPlay(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "ConMacroPlay called: %d", on);
        if(on != 0)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(p->m_aDummies[D].m_MacroPlayLines.empty())
                                continue;
                        p->m_aDummies[D].m_MacroPlaying = true;
                        p->m_aDummies[D].m_MacroPlayIndex = 0;
                        p->m_aDummies[D].m_MacroSleepTicks = 0;
                }
                dbg_msg("botnet_macro", "Playback started.");
        }
        else
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->m_aDummies[D].m_MacroPlaying = false;
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
                p->GameClient()->m_BotControl.ActionStop(-1);
                dbg_msg("botnet_macro", "Playback stopped manually.");
        }
}

void CBotNet::ConMacroRecord(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "ConMacroRecord called with on=%d", on);

        if(on != 0)
        {
                dbg_msg("botnet_macro", "Starting macro recording...");
                int64_t startTick = p->Client() ? p->Client()->GameTick(0) : 0;
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_MacroRecording = true;
                        p->m_aDummies[D].m_MacroRecordBuffer.clear();
                        p->m_aDummies[D].m_LastMacroRecordTick = startTick;
                        p->m_aDummies[D].m_LastRecordedDir = 0;
                        p->m_aDummies[D].m_LastRecordedJump = 0;
                        p->m_aDummies[D].m_LastRecordedHook = 0;
                        p->m_aDummies[D].m_LastRecordedFire = 0;
                        p->m_aDummies[D].m_LastRecordedAimX = 0;
                        p->m_aDummies[D].m_LastRecordedAimY = 0;
                        p->m_aDummies[D].m_LastRecordedWeapon = -1;
                }
                dbg_msg("botnet_macro", "Macro recording started successfully. Start tick: %lld", (long long)startTick);
        }
        else
        {
                dbg_msg("botnet_macro", "Stopping macro recording...");
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->m_aDummies[D].m_MacroRecording = false;
                dbg_msg("botnet_macro", "Macro recording stopped. Total lines in buffer[0]: %d", (int)p->m_aDummies[0].m_MacroRecordBuffer.size());
        }
}

void CBotNet::ConMacroSave(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        const char *path = pResult->GetString(0);
        dbg_msg("botnet_macro", "ConMacroSave called with path: %s", path ? path : "NULL");
        if(!path || !path[0])
                return;

        // Save from dummy 0's buffer
        std::ofstream file(path);
        if(!file.is_open())
        {
                dbg_msg("botnet_macro", "Failed to open file for writing: %s", path);
                if(p->Console())
                        p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Failed to open file for writing");
                return;
        }

        dbg_msg("botnet_macro", "Writing %d lines to file...", (int)p->m_aDummies[0].m_MacroRecordBuffer.size());
        for(const std::string &line : p->m_aDummies[0].m_MacroRecordBuffer)
                file << line << "\n";
        file.close();

        dbg_msg("botnet_macro", "File saved successfully.");
        if(p->Console())
                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Macro saved");
}

void CBotNet::ConMacroCapture(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int id = pResult->GetInteger(0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_MacroCaptureID = id;
        dbg_msg("botnet_macro", "Capture ID set to: %d", id);
}

void CBotNet::ConAttackHookDelay(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_HookDelay = pResult->GetInteger(0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_HookTickTimer = 0;
}

void CBotNet::ConAttackDists(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_FireDist = pResult->GetFloat(0);
        p->m_HookDist = pResult->GetFloat(1);
        p->m_RescueRadius = pResult->GetFloat(2);
        if(pResult->NumArguments() > 3)
                p->m_TargetDist = pResult->GetFloat(3);
        if(pResult->NumArguments() > 4)
                p->m_MainDist = pResult->GetFloat(4);
        if(pResult->NumArguments() > 5)
                p->m_StandDist = pResult->GetFloat(5);
        if(pResult->NumArguments() > 6)
                p->m_MainStandDist = pResult->GetFloat(6);
}

void CBotNet::ConSetTargets(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        for(int i = 0; i < 128; i++)
                p->m_TargetList[i] = false;
        const char *pL = pResult->GetString(0);
        if(!pL || !pL[0])
                return;
        char aB[256];
        str_copy(aB, pL, sizeof(aB));
        char *pC = aB;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        p->m_TargetList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}

void CBotNet::ConSetBots(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        for(int i = 0; i < 128; i++)
                p->m_BotsList[i] = false;
        const char *pL = pResult->GetString(0);
        if(!pL || !pL[0])
                return;
        char aB[256];
        str_copy(aB, pL, sizeof(aB));
        char *pC = aB;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        p->m_BotsList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}

void CBotNet::ConRescueIds(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        for(int i = 0; i < 128; i++)
                p->m_RescueList[i] = false;
        const char *pL = pResult->GetString(0);
        if(!pL || !pL[0])
                return;
        char aB[256];
        str_copy(aB, pL, sizeof(aB));
        char *pC = aB;
        while(pC)
        {
                int id = atoi(pC);
                if(id >= 0 && id < 128)
                        p->m_RescueList[id] = true;
                pC = strchr(pC, ',');
                if(pC)
                        pC++;
        }
}

void CBotNet::ConSetTargetAll(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AllTarget = pResult->GetInteger(0) != 0;
}

void CBotNet::ConAttackSettings(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoAim = pResult->GetInteger(0) != 0;
        p->m_AutoFire = pResult->GetInteger(1) != 0;
        p->m_AutoHook = pResult->GetInteger(2) != 0;
        p->m_MoveEnabled = pResult->GetInteger(3) != 0;
        p->m_StandEnabled = pResult->GetInteger(4) != 0;
        p->m_RescueFrozen = pResult->GetInteger(5) != 0;
        p->m_RescueAll = pResult->GetInteger(6) != 0;
        p->m_SmartDetect = pResult->GetInteger(7) != 0;
        p->m_SmartRescue = pResult->GetInteger(8) != 0;
        p->m_KillOnFreeze = pResult->GetInteger(9) != 0;
        p->m_AttackMain = pResult->GetInteger(10) != 0;
        p->m_AutoHammer = pResult->GetInteger(11) != 0;
        {
                bool newSim = pResult->GetInteger(12) != 0;
                if(newSim != p->m_PfSimulatePlayers)
                {
                        p->m_PfSimulatePlayers = newSim;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                p->m_aDummies[D].m_LastTargetTX = -1;
                                p->m_aDummies[D].m_LastTargetTY = -1;
                                p->m_aDummies[D].m_PathFound = false;
                        }
                }
        }
        p->m_AvoidFreeze = pResult->GetInteger(13) != 0;
        {
                bool newPfHook = pResult->GetInteger(14) != 0;
                if(newPfHook != p->m_PfHookEnabled)
                {
                        p->m_PfHookEnabled = newPfHook;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                                p->m_aDummies[D].m_PfHookTile = vec2(0, 0);
                }
        }
        {
                bool newKino = pResult->GetInteger(15) != 0;
                if(newKino != p->m_KinodynamicEnabled)
                {
                        p->m_KinodynamicEnabled = newKino;
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                p->m_aDummies[D].m_KinoCache.Reset();
                                p->ResetDummyInputs(D);
                        }
                }
        }
}

void CBotNet::ConAttackEnable(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AttackEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_AttackEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSetMain(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_MainID = pResult->GetInteger(0);
}

void CBotNet::ConRandomAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        pSelf->m_RandomAim = pResult->GetInteger(0) != 0;
        if(pResult->NumArguments() > 1)
                pSelf->m_RandomAimInterval = pResult->GetInteger(1);
}

void CBotNet::ConCopyMoves(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        int ID = pResult->GetInteger(0);
        pSelf->m_CopyTargetID = ID;
        pSelf->m_CopyMoves = (ID >= 0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                pSelf->m_aDummies[D].m_LastTargetAttackTick = -1;
}

void CBotNet::ConClientDelay(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_ClientDelay = pResult->GetInteger(0);
}

void CBotNet::ConStandOnX(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_StandOnXOnly = pResult->GetInteger(0) != 0;
}

void CBotNet::ConPathfinder(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PathfinderEnabled = pResult->GetInteger(0) != 0;
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                p->m_aDummies[D].m_LastTargetTX = -1;
                p->m_aDummies[D].m_LastTargetTY = -1;
                p->m_aDummies[D].m_PathFound = false;
        }
}

void CBotNet::ConPathfinderRays(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfRays = pf_clamp(pResult->GetInteger(0), 12, 90);
}

void CBotNet::ConPathfinderRaysDist(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfViewRadius = pf_clamp(pResult->GetInteger(0), 1, 128);
}

void CBotNet::ConPathfinderSnap(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfSnap = pResult->GetInteger(0) != 0;
}

void CBotNet::ConPathfinderSps(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfSimulateMethod = pResult->GetInteger(0);
}

void CBotNet::ConPathfinderGo(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);

        if(on == 0)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_PathfinderGoActive = false;
                        p->m_aDummies[D].m_LastTargetTX = -1;
                        p->m_aDummies[D].m_LastTargetTY = -1;
                        p->m_aDummies[D].m_PathFound = false;
                }
                dbg_msg("botnet", "Pathfinder go disabled");
                return;
        }

        if(pResult->NumArguments() >= 3)
        {
                int x = pResult->GetInteger(1);
                int y = pResult->GetInteger(2);
                vec2 pos = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_PathfinderGoActive = true;
                        p->m_aDummies[D].m_PathfinderGoPos = pos;
                }
                dbg_msg("botnet", "Pathfinder go to tile (%d, %d) -> pos (%.0f, %.0f)", x, y, pos.x, pos.y);
        }
}

// =========================================================
// INDIVIDUAL TOGGLE COMMANDS
// =========================================================

void CBotNet::ConStand(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_StandEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_StandEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoAim(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoAim = pResult->GetInteger(0) != 0;
        if(!p->m_AutoAim)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoFire(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoFire = pResult->GetInteger(0) != 0;
        if(!p->m_AutoFire)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoHook(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoHook = pResult->GetInteger(0) != 0;
        if(!p->m_AutoHook)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConMove(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_MoveEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_MoveEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConRescue(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_RescueFrozen = pResult->GetInteger(0) != 0;
        if(!p->m_RescueFrozen)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConKillFrz(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KillOnFreeze = pResult->GetInteger(0) != 0;
        if(!p->m_KillOnFreeze)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAtkMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AttackMain = pResult->GetInteger(0) != 0;
        if(!p->m_AttackMain)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAutoMain(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoMain = pResult->GetInteger(0) != 0;
}

void CBotNet::ConHammer(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AutoHammer = pResult->GetInteger(0) != 0;
        if(!p->m_AutoHammer)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSmartDetect(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_SmartDetect = pResult->GetInteger(0) != 0;
        if(!p->m_SmartDetect)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConSmartRescue(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_SmartRescue = pResult->GetInteger(0) != 0;
        if(!p->m_SmartRescue)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConAvoidFreeze(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_AvoidFreeze = pResult->GetInteger(0) != 0;
        if(!p->m_AvoidFreeze)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

void CBotNet::ConPfHook(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_PfHookEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_PfHookEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
        }
}

std::string CBotNet::ReplacePlaceholders(const std::string &Cmd, int DummyIndex, CGameClient *pGame)
{
        std::string Result = Cmd;

        // {i} — rank of this dummy among connected dummies (1-based)
        // Original: online = sorted(control_clients.keys()); rank = online.index(client_index) + 1
        {
                // Collect connected dummy indices (sorted ascending, which they already are)
                int Online[MAX_DUMMIES];
                int Count = 0;
                for(int d = 0; d < MAX_DUMMIES; ++d)
                {
                        if(d != 0 && !pGame->Client()->DummyConnected(d))
                                continue;
                        if(pGame->m_aLocalIds[d] < 0)
                                continue;
                        Online[Count++] = d;
                }
                int Rank = DummyIndex + 1; // fallback if not found
                for(int a = 0; a < Count; ++a)
                {
                        if(Online[a] == DummyIndex)
                        {
                                Rank = a + 1;
                                break;
                        }
                }
                size_t Pos;
                while((Pos = Result.find("{i}")) != std::string::npos)
                        Result.replace(Pos, 3, std::to_string(Rank));
        }

        // {r} — random char from a-zA-Z0-9._-
        // Original: random.choice(string.ascii_letters + string.digits + "._-")
        {
                static const char RAND_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
                static const int RAND_CHARS_LEN = sizeof(RAND_CHARS) - 1;
                size_t Pos;
                while((Pos = Result.find("{r}")) != std::string::npos)
                {
                        char Ch = RAND_CHARS[rand() % RAND_CHARS_LEN];
                        Result.replace(Pos, 3, 1, Ch);
                }
        }

        // {ri-N} — random integer from 0 to N (inclusive)
        // Original: re.sub(r'{ri-(\d+)}', lambda m: str(random.randint(0, int(m.group(1)))), cmd)
        {
                size_t Pos;
                while((Pos = Result.find("{ri-")) != std::string::npos)
                {
                        size_t End = Result.find('}', Pos);
                        if(End != std::string::npos)
                        {
                                std::string NumStr = Result.substr(Pos + 4, End - Pos - 4);
                                int MaxVal = atoi(NumStr.c_str());
                                if(MaxVal < 1) MaxVal = 1;
                                int Val = rand() % (MaxVal + 1);
                                Result.replace(Pos, End - Pos + 1, std::to_string(Val));
                        }
                        else
                        {
                                break;
                        }
                }
        }

        // {c} — random CJK character U+4E00–U+9FFF
        // Original: chr(random.randint(0x4E00, 0x9FFF))
        {
                size_t Pos;
                while((Pos = Result.find("{c}")) != std::string::npos)
                {
                        int Codepoint = 0x4E00 + (rand() % (0x9FFF - 0x4E00 + 1));
                        char Utf8[5];
                        Utf8[0] = (char)(0xE0 | ((Codepoint >> 12) & 0x0F));
                        Utf8[1] = (char)(0x80 | ((Codepoint >> 6) & 0x3F));
                        Utf8[2] = (char)(0x80 | (Codepoint & 0x3F));
                        Utf8[3] = '\0';
                        Result.replace(Pos, 3, Utf8);
                }
        }

        return Result;
}

void CBotNet::ConSendDummy(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        const char *pIds = pResult->GetString(0);
        const char *pCmd = pResult->GetString(1);

        if(!pCmd[0])
                return;

        // Parse dummy IDs
        bool aTargets[MAX_DUMMIES] = {};
        bool AllDummies = false;

        if(str_comp(pIds, "-1") == 0)
        {
                AllDummies = true;
        }
        else
        {
                // Parse comma-separated IDs: "0,1,2,3"
                char aBuf[64];
                str_copy(aBuf, pIds, sizeof(aBuf));
                char *pTok = strtok(aBuf, ",");
                while(pTok)
                {
                        int Id = atoi(pTok);
                        if(Id >= 0 && Id < MAX_DUMMIES)
                                aTargets[Id] = true;
                        pTok = strtok(nullptr, ",");
                }
        }

        // Save current dummy
        int SavedDummy = g_Config.m_ClDummy;

        // Execute command on each target dummy
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                if(!AllDummies && !aTargets[D])
                        continue;
                // Skip disconnected dummies (0=main, always connected if online)
                if(D != 0 && !pSelf->Client()->DummyConnected(D))
                        continue;

                // Replace placeholders per-dummy
                std::string Cmd = ReplacePlaceholders(pCmd, D, pSelf->GameClient());

                g_Config.m_ClDummy = D;
                pSelf->Console()->ExecuteLine(Cmd.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
        }

        // Restore original dummy
        g_Config.m_ClDummy = SavedDummy;
}

// =========================================================
// KINODYNAMIC A* — CONSOLE COMMANDS
// =========================================================

void CBotNet::ConKinodynamic(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinodynamicEnabled = pResult->GetInteger(0) != 0;
        if(!p->m_KinodynamicEnabled)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_KinoCache.Reset();
                        p->ResetDummyInputs(D);
                }
        }
}

void CBotNet::ConKinoCandidates(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 100 && v <= 2000)
                p->m_KinoCandidates = v;
}

void CBotNet::ConKinoTicks(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 3 && v <= 20)
                p->m_KinoTicks = v;
}

void CBotNet::ConKinoHookAngles(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 4 && v <= 32)
                p->m_KinoHookAngles = v;
}

void CBotNet::ConKinoCacheTicks(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 1 && v <= 128)
                p->m_KinoCacheTicks = v;
}

void CBotNet::ConKinoShowPath(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinoShowPath = pResult->GetInteger(0) != 0;
}

void CBotNet::ConKinoShowField(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinoShowField = pResult->GetInteger(0) != 0;
}

void CBotNet::ConKinoAggressive(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_KinoAggressive = pResult->GetInteger(0) != 0;
}

// =========================================================
// PATHFINDER TAB (Kinetix→Pathfinder) — chunk-based mini TAS maker
// =========================================================
//
// State machine:  IDLE --[user clicks "pathfinding"]--> RUNNING
//                      --[chunk reaches finish / died / max-chunks]--> FINISHED
//                      --[user clicks "finish"]--> IDLE (path disappears)
//                 RUNNING --[user clicks "stop"]--> IDLE (cancel)
//
// Per OnUpdate (RUNNING): one chunk = generate candidates (v1.42 motion
// primitives: 6 BASE + HOOK_FIRE × hookAngles + 3 HOOK_HOLD + HOOK_RELEASE),
// simulate each in a cloned world, score by enabled Score+Fine methods,
// pick the best, append trajectory to m_PfVPath, advance sim state.
//
// RenderPathfinderPath draws m_PfVPath using the shared m_ClTrajectoryColor/
// Alpha/LineSize settings (same as Advanced→Trajectory).
//
// SIMULATION-ONLY: does NOT write inputs to any dummy.

// --- console command handlers ---
void CBotNet::ConPfLive(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int s = pResult->GetInteger(0);
        if(s < 0 || s > 2)
                return;
        if(s == PF_STATE_RUNNING && p->m_PfState != PF_STATE_RUNNING)
                p->PfResetRun();
        p->m_PfState = s;
        if(s == PF_STATE_IDLE)
                p->m_PfVPath.clear();
}

void CBotNet::ConPfAlgorithm(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 0 && v < PF_TAB_ALGORITHM_COUNT)
                g_Config.m_ClPfAlgorithm = v;
}

void CBotNet::ConPfChunkSize(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= CConfig::PF_CHUNK_SIZE_MIN && v <= PF_TAB_MAX_CHUNK_TICKS)
                g_Config.m_ClPfChunkSize = v;
}

void CBotNet::ConPfCandidates(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 50 && v <= 2000)
                g_Config.m_ClPfCandidates = v;
}

void CBotNet::ConPfHookAngles(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 4 && v <= 32)
                g_Config.m_ClPfHookAngles = v;
}

void CBotNet::ConPfHorizon(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 1 && v <= PF_TAB_MAX_CHUNK_TICKS)
                g_Config.m_ClPfHorizon = v;
}

// Reset run: anchor to active player's predicted core, build flow field.
void CBotNet::PfResetRun()
{
        m_PfVPath.clear();
        m_PfChunkCount = 0;
        m_PfTickCounter = 0;

        CGameClient *pGame = GameClient();
        if(!pGame || !pGame->m_Snap.m_pLocalInfo)
        {
                dbg_msg("pathfinder", "PfResetRun: no game or no local info");
                return;
        }

        // Make sure the map grid is loaded BEFORE building the flow field
        // (PfComputeFlowField needs m_pMapGrid / m_pFrontGrid).
        if(!m_MapGridLoaded)
                LoadMapGrid();
        if(!m_MapGridLoaded)
        {
                dbg_msg("pathfinder", "PfResetRun: map grid failed to load");
                return;
        }

        int LocalID = pGame->m_Snap.m_LocalClientId;
        if(LocalID < 0)
        {
                dbg_msg("pathfinder", "PfResetRun: no local client id");
                return;
        }
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalID);
        if(!pChar)
        {
                dbg_msg("pathfinder", "PfResetRun: no predicted character");
                return;
        }

        const CCharacterCore &Core = pChar->GetCore();
        m_PfStartPos = Core.m_Pos;
        m_PfCurPos = Core.m_Pos;
        m_PfCurVel = Core.m_Vel;
        m_PfCurHookState = Core.m_HookState;
        m_PfCurHookPos = Core.m_HookPos;
        m_PfCurHookDir = Core.m_HookDir;
        m_PfCurHookTick = Core.m_HookTick;
        m_PfCurFreezeTime = pChar->m_FreezeTime;
        m_PfCurJumped = Core.m_Jumped;
        m_PfTotalFreezeTicks = 0;
        m_PfBacktrackIdx = 0;
        for(int i = 0; i < PF_BACKTRACK_DEPTH; i++)
                m_PfBacktrack[i].Reset();

        // Reset State-Lattice A* state
        m_PfANodes.clear();
        m_PfAOpen.clear();
        m_PfABestG.clear();
        m_PfAGoalIdx = -1;
        m_PfAStarted = false;
        m_PfAPathReady = false;
        m_PfAExpandCount = 0;
        m_PfFullInputs.clear();
        m_PfFullInputsIdx = 0;

        m_PfVPath.push_back(Core.m_Pos);

        // Force flow-field rebuild (clear cached finish tiles so PfComputeFlowField
        // re-scans the map).
        if(m_PfFlowField)
        {
                delete[] m_PfFlowField;
                m_PfFlowField = nullptr;
        }
        m_PfFinishTiles.clear();
        PfComputeFlowField();

        dbg_msg("pathfinder", "PfResetRun: start=(%.0f,%.0f) finish_tiles=%d flowfield=%s",
                Core.m_Pos.x, Core.m_Pos.y, (int)m_PfFinishTiles.size(),
                m_PfFlowField ? "ok" : "null");
}

// Multi-source BFS: distance field to the NEAREST finish tile.
void CBotNet::PfComputeFlowField()
{
        if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
                return;

        int Size = m_MapWidth * m_MapHeight;
        if(!m_PfFlowField || (int)m_PfFinishTiles.empty())
        {
                if(m_PfFlowField)
                        delete[] m_PfFlowField;
                m_PfFlowField = new float[Size];
                m_PfFinishTiles.clear();
                for(int ty = 0; ty < m_MapHeight; ty++)
                {
                        for(int tx = 0; tx < m_MapWidth; tx++)
                        {
                                int idx = ty * m_MapWidth + tx;
                                if(m_pMapGrid[idx] == TILE_FINISH || m_pFrontGrid[idx] == TILE_FINISH)
                                        m_PfFinishTiles.push_back(vec2(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f));
                        }
                }
        }

        // === Phase 1: BFS from finish, STRICTLY skip freeze tiles. ===
        // Freeze tiles stay 1e18 (unreachable). If the bot's start position is
        // reachable in Phase 1, we're done — path exists without freeze.
        // Otherwise, Phase 2 takes over with a full reset.
        for(int i = 0; i < Size; i++)
                m_PfFlowField[i] = 1e18f;

        if(m_PfFinishTiles.empty())
                return;

        const float SQRT2 = 1.4142135623730951f;
        const int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        const int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

        auto fnIsFreeze = [&](int tx, int ty) -> bool {
                int idx = ty * m_MapWidth + tx;
                return m_pMapGrid[idx] == TILE_FREEZE || m_pMapGrid[idx] == TILE_DFREEZE ||
                       m_pFrontGrid[idx] == TILE_FREEZE || m_pFrontGrid[idx] == TILE_DFREEZE;
        };

        std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                int tx = (int)(fp.x / 32.0f);
                int ty = (int)(fp.y / 32.0f);
                if(!IsTileWalkable(tx, ty))
                        continue;
                m_PfFlowField[ty * m_MapWidth + tx] = 0.0f;
                open.push({0.0f, 0.0f, ty, tx});
        }

        while(!open.empty())
        {
                PfNode cur = open.top();
                open.pop();
                int cIdx = cur.r * m_MapWidth + cur.c;
                if(cur.g > m_PfFlowField[cIdx])
                        continue;

                for(int d = 0; d < 8; d++)
                {
                        int nr = cur.r + dr[d];
                        int nc = cur.c + dc[d];
                        if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
                                continue;
                        if(!IsTileWalkable(nc, nr))
                                continue;
                        if(fnIsFreeze(nc, nr))
                                continue; // Phase 1: strictly skip freeze
                        if(abs(dr[d]) + abs(dc[d]) == 2)
                        {
                                if(!IsTileWalkable(cur.c, cur.r + dr[d]) || !IsTileWalkable(cur.c + dc[d], cur.r))
                                        continue;
                        }
                        float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 : 1.0f;
                        float newG = cur.g + moveCost;
                        int nIdx = nr * m_MapWidth + nc;
                        if(newG < m_PfFlowField[nIdx])
                        {
                                m_PfFlowField[nIdx] = newG;
                                open.push({newG, newG, nr, nc});
                        }
                }
        }

        // === Phase 1 reachability check: did Phase 1 reach the bot's start? ===
        // m_PfStartPos is set in PfResetRun (bot pos at run start). If it's still
        // (0,0) or out of map, skip Phase 2 (no start to check).
        int startTX = (int)(m_PfStartPos.x / 32.0f);
        int startTY = (int)(m_PfStartPos.y / 32.0f);
        bool phase1ReachedStart = false;
        if(startTX >= 0 && startTY >= 0 && startTX < m_MapWidth && startTY < m_MapHeight &&
           (m_PfStartPos.x != 0.0f || m_PfStartPos.y != 0.0f))
        {
                phase1ReachedStart = m_PfFlowField[startTY * m_MapWidth + startTX] < 1e17f;
        }

        // === Phase 2: full reset + BFS with freeze passable (cost 1). ===
        // Only if Phase 1 did NOT reach the bot's start (path requires freeze).
        if(!phase1ReachedStart)
        {
                for(int i = 0; i < Size; i++)
                        m_PfFlowField[i] = 1e18f;

                std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open2;
                for(const vec2 &fp : m_PfFinishTiles)
                {
                        int tx = (int)(fp.x / 32.0f);
                        int ty = (int)(fp.y / 32.0f);
                        if(!IsTileWalkable(tx, ty))
                                continue;
                        m_PfFlowField[ty * m_MapWidth + tx] = 0.0f;
                        open2.push({0.0f, 0.0f, ty, tx});
                }

                while(!open2.empty())
                {
                        PfNode cur = open2.top();
                        open2.pop();
                        int cIdx = cur.r * m_MapWidth + cur.c;
                        if(cur.g > m_PfFlowField[cIdx])
                                continue;

                        for(int d = 0; d < 8; d++)
                        {
                                int nr = cur.r + dr[d];
                                int nc = cur.c + dc[d];
                                if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
                                        continue;
                                if(!IsTileWalkable(nc, nr))
                                        continue;
                                // Phase 2: freeze passable at cost 1 (same as normal tiles).
                                if(abs(dr[d]) + abs(dc[d]) == 2)
                                {
                                        if(!IsTileWalkable(cur.c, cur.r + dr[d]) || !IsTileWalkable(cur.c + dc[d], cur.r))
                                                continue;
                                }
                                float moveCost = (abs(dr[d]) + abs(dc[d]) == 2) ? SQRT2 : 1.0f;
                                float newG = cur.g + moveCost;
                                int nIdx = nr * m_MapWidth + nc;
                                if(newG < m_PfFlowField[nIdx])
                                {
                                        m_PfFlowField[nIdx] = newG;
                                        open2.push({newG, newG, nr, nc});
                                }
                        }
                }
        }
}

// Score a candidate chunk.  Higher = better.
// Simplified: flowProgress + finishBonus - deathPenalty - freezePenalty - stallPenalty.
float CBotNet::PfScoreChunk(const vec2 &startPos, const vec2 &endPos, bool reachedFinish, bool died,
                            int freezeTicks) const
{
        float score = 0.0f;
        const float INF = 1e17f; // unreachable threshold (m_PfFlowField uses 1e18)

        // Score method: flow-field alignment (how well the chunk's movement
        // direction matches the flow-field direction toward finish).
        // Gated by ClPfScoreFlow. Uses cosine of angle between movement vector
        // and flow-field gradient at start tile: +1 = along flow (toward finish),
        // -1 = against flow (away from finish), 0 = perpendicular.
        if(g_Config.m_ClPfScoreFlow && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                vec2 move = endPos - startPos;
                float moveLen = sqrtf(move.x * move.x + move.y * move.y);
                if(moveLen > 0.001f)
                {
                        int sx = pf_clamp((int)(startPos.x / 32.0f), 0, m_MapWidth - 1);
                        int sy = pf_clamp((int)(startPos.y / 32.0f), 0, m_MapHeight - 1);
                        // Flow direction at start tile = gradient of m_PfFlowField
                        // (points toward finish, i.e. toward smaller distance).
                        float fd = m_PfFlowField[sy * m_MapWidth + sx];
                        if(fd < INF)
                        {
                                float fdx_m = (sx > 0) ? m_PfFlowField[sy * m_MapWidth + (sx - 1)] : fd;
                                float fdx_p = (sx < m_MapWidth - 1) ? m_PfFlowField[sy * m_MapWidth + (sx + 1)] : fd;
                                float fdy_m = (sy > 0) ? m_PfFlowField[(sy - 1) * m_MapWidth + sx] : fd;
                                float fdy_p = (sy < m_MapHeight - 1) ? m_PfFlowField[(sy + 1) * m_MapWidth + sx] : fd;
                                // Unreachable neighbors → treat as equal (no spurious gradient).
                                if(fdx_m >= INF) fdx_m = fd;
                                if(fdx_p >= INF) fdx_p = fd;
                                if(fdy_m >= INF) fdy_m = fd;
                                if(fdy_p >= INF) fdy_p = fd;
                                // Gradient: negative of distance increase direction.
                                // gx = fdx_m - fdx_p (points toward smaller distance).
                                float gx = fdx_m - fdx_p;
                                float gy = fdy_m - fdy_p;
                                float gLen = sqrtf(gx * gx + gy * gy);
                                if(gLen > 0.001f)
                                {
                                        // Cosine of angle between move and flow direction.
                                        float cosA = (move.x * gx + move.y * gy) / (moveLen * gLen);
                                        // cosA in [-1, +1]. Scale to score weight.
                                        score += cosA * 10.0f;
                                }
                        }
                }
        }

        // Score method: distance reduction (BFS flow-field distance to finish).
        // Gated by ClPfScoreDist. Positive when chunk moved us closer to finish
        // in the BFS distance field. Uses flow field (not euclidean) so walls
        // are accounted for — same algorithm as the old always-on flow progress.
        if(g_Config.m_ClPfScoreDist && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                int sx = pf_clamp((int)(startPos.x / 32.0f), 0, m_MapWidth - 1);
                int sy = pf_clamp((int)(startPos.y / 32.0f), 0, m_MapHeight - 1);
                int ex = pf_clamp((int)(endPos.x / 32.0f), 0, m_MapWidth - 1);
                int ey = pf_clamp((int)(endPos.y / 32.0f), 0, m_MapHeight - 1);
                float dStart = m_PfFlowField[sy * m_MapWidth + sx];
                float dEnd = m_PfFlowField[ey * m_MapWidth + ex];
                if(dStart < INF && dEnd < INF)
                        score += (dStart - dEnd) * 10.0f;
                else if(dEnd < INF && dStart >= INF)
                        score += 50.0f; // escaped an unreachable tile
                else if(dEnd >= INF && dStart < INF)
                        score -= 50.0f; // moved INTO an unreachable tile
        }

        // Finish bonus
        if(reachedFinish)
                score += 1000000.0f;

        // Death penalty
        if(died)
                score -= 10000.0f;

        // Dynamic freeze penalty (increases over time to discourage freeze routes)
        if(g_Config.m_ClPfFineFreeze)
                score -= freezeTicks * (1.0f + m_PfTotalFreezeTicks / 64.0f);

        // Stall penalty
        if(distance(endPos, startPos) < 2.0f)
                score -= 500.0f;

        return score;
}

// Per-update chunk step.
void CBotNet::UpdatePathfinder()
{
        if(m_PfState != PF_STATE_RUNNING)
                return;
        if(!m_MapGridLoaded)
                LoadMapGrid();
        if(!m_MapGridLoaded)
        {
                m_PfState = PF_STATE_FINISHED;
                return;
        }

        if(m_PfFlowField == nullptr || m_PfFinishTiles.empty())
                PfComputeFlowField();
        if(m_PfFinishTiles.empty())
        {
                m_PfState = PF_STATE_FINISHED;
                return;
        }

        // ── Playback phase: A* found a path, emit ChunkSize ticks per call ──
        if(m_PfAPathReady)
        {
                m_PfTickCounter++;
                int perf = pf_clamp(g_Config.m_ClPfPerf, 1, 100);
                int tickInterval = 6000 / perf;
                if(tickInterval < 1)
                        tickInterval = 1;
                if(m_PfTickCounter < tickInterval)
                        return;
                m_PfTickCounter = 0;

                if(m_PfFullInputsIdx >= m_PfFullInputs.size())
                {
                        m_PfState = PF_STATE_FINISHED;
                        return;
                }
                int ChunkSize = pf_clamp(g_Config.m_ClPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
                size_t endIdx = m_PfFullInputsIdx + (size_t)ChunkSize;
                if(endIdx > m_PfFullInputs.size())
                        endIdx = m_PfFullInputs.size();
                m_PfFullInputsIdx = endIdx;
                m_PfChunkCount++;
                if(m_PfFullInputsIdx >= m_PfFullInputs.size())
                        m_PfState = PF_STATE_FINISHED;
                return;
        }

        // ── A* phase ──
        // Perf throttle: 6000/perf frames between expansions (same as v1.55)
        m_PfTickCounter++;
        int perf = pf_clamp(g_Config.m_ClPfPerf, 1, 100);
        int tickInterval = 6000 / perf;
        if(tickInterval < 1)
                tickInterval = 1;
        if(m_PfTickCounter < tickInterval)
                return;
        m_PfTickCounter = 0;

        if(!m_PfAStarted)
        {
                if(!PfAStarInit())
                {
                        m_PfState = PF_STATE_FINISHED;
                        return;
                }
                m_PfAStarted = true;
                PfAStarUpdatePreview();
                return;
        }

        int result = PfAStarStep();
        if(result == 1)
        {
                PfAStarReconstruct();
                m_PfAPathReady = true;
                m_PfFullInputsIdx = 0;
                dbg_msg("pathfinder", "A* path found: %zu ticks, %zu nodes, %d expansions",
                        m_PfFullInputs.size(), m_PfANodes.size(), m_PfAExpandCount);
                return;
        }
        else if(result < 0)
        {
                dbg_msg("pathfinder", "A* failed: %d expansions, %zu nodes",
                        m_PfAExpandCount, m_PfANodes.size());
                m_PfState = PF_STATE_FINISHED;
                return;
        }
        PfAStarUpdatePreview();
}


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

// Spawn the sim bot at the given state in a cloned world.  The bot keeps its
// real CCharacter identity so DDNet freeze/finish/death logic runs unmodified
// (via OnPredictedInput -> DDNet CCharacter::Tick).
static CCharacter *PfSpawnSimBot(CGameWorld *pWorld, int PredictedCharId, const vec2 &pos,
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
static void PfSimulateChunk(CGameWorld *pWorld, int PredictedCharId, CCharacter *pBot,
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

// Try an alternative candidate from the backtrack buffer.
// Returns true if a candidate was applied, false if the buffer is exhausted.
bool CBotNet::PfBacktrack()
{
        while(m_PfBacktrackIdx > 0)
        {
                m_PfBacktrackIdx--;
                PfBacktrackEntry &entry = m_PfBacktrack[m_PfBacktrackIdx];

                // Try next candidate (usedCandidateIdx + 1)
                int nextIdx = entry.usedCandidateIdx + 1;
                if(nextIdx >= entry.numCandidates || nextIdx >= (int)entry.candidates.size())
                        continue; // no more candidates at this level, pop further

                // Restore anchor state
                m_PfCurPos = entry.pos;
                m_PfCurVel = entry.vel;
                m_PfCurHookPos = entry.hookPos;
                m_PfCurHookDir = entry.hookDir;
                m_PfCurHookState = entry.hookState;
                m_PfCurHookTick = entry.hookTick;
                m_PfCurJumped = entry.jumped;
                m_PfCurFreezeTime = entry.freezeTime;
                m_PfTotalFreezeTicks = entry.totalFreezeTicks;

                // Truncate trajectory and restore chunk count to the active
                // trajectory length (active chunks, not total attempts — otherwise
                // backtracking burns the budget on retries).
                m_PfVPath.resize(entry.vPathSize);
                m_PfChunkCount = entry.chunkCount;

                // Apply the alternative candidate
                PfBacktrackEntry::Candidate &cand = entry.candidates[nextIdx];
                for(size_t i = 1; i < cand.traj.size(); i++)
                        m_PfVPath.push_back(cand.traj[i]);

                m_PfCurPos = cand.endPos;
                m_PfCurVel = cand.endVel;
                m_PfCurHookPos = cand.endHookPos;
                m_PfCurHookDir = cand.endHookDir;
                m_PfCurHookState = cand.endHookState;
                m_PfCurHookTick = cand.endHookTick;
                m_PfCurJumped = cand.endJumped;
                m_PfCurFreezeTime = cand.endFreezeTime;
                m_PfTotalFreezeTicks += cand.freezeTicks;

                entry.usedCandidateIdx = nextIdx;
                m_PfBacktrackIdx++; // push back (we're using this entry now)

                dbg_msg("pathfinder", "backtrack: using candidate %d at depth %d", nextIdx, m_PfBacktrackIdx - 1);
                return true;
        }

        return false; // backtrack exhausted
}

// =========================================================
// State-Lattice A* (v1.56)
// =========================================================

static uint64_t PfAStarKey(const CCharacterCore &core, int freezeTime, bool grounded)
{
        int tx = (int)(core.m_Pos.x / 32.0f);
        int ty = (int)(core.m_Pos.y / 32.0f);
        int vbx = pf_clamp((int)(core.m_Vel.x / 5.0f) + 3, 0, 6);
        int vby = pf_clamp((int)(core.m_Vel.y / 5.0f) + 3, 0, 6);
        int frozen = (freezeTime > 0) ? 1 : 0;
        uint64_t k = 0;
        k |= (uint64_t)(uint16_t)(int16_t)tx;
        k |= (uint64_t)(uint16_t)(int16_t)ty << 16;
        k |= (uint64_t)(uint8_t)vbx << 32;
        k |= (uint64_t)(uint8_t)vby << 35;
        k |= (uint64_t)(grounded ? 1 : 0) << 38;
        k |= (uint64_t)(uint8_t)(core.m_HookState + 8) << 39;
        k |= (uint64_t)(frozen & 1) << 43;
        k |= (uint64_t)(uint8_t)(core.m_Jumped & 0xF) << 44;
        return k;
}

static void PfAStarHeapPush(std::vector<int> &heap, const std::vector<CBotNet::AStarNode> &nodes, int idx)
{
        heap.push_back(idx);
        int i = (int)heap.size() - 1;
        while(i > 0)
        {
                int parent = (i - 1) / 2;
                if(nodes[heap[parent]].F <= nodes[heap[i]].F)
                        break;
                std::swap(heap[parent], heap[i]);
                i = parent;
        }
}

static int PfAStarHeapPop(std::vector<int> &heap, const std::vector<CBotNet::AStarNode> &nodes)
{
        int top = heap[0];
        heap[0] = heap.back();
        heap.pop_back();
        int n = (int)heap.size();
        int i = 0;
        while(true)
        {
                int l = 2*i+1, r = 2*i+2, s = i;
                if(l < n && nodes[heap[l]].F < nodes[heap[s]].F) s = l;
                if(r < n && nodes[heap[r]].F < nodes[heap[s]].F) s = r;
                if(s == i) break;
                std::swap(heap[s], heap[i]);
                i = s;
        }
        return top;
}

float CBotNet::PfAStarHeuristic(const vec2 &pos, const vec2 &endPos)
{
        // Heuristic is gated by the same Score Method checkboxes as PfScoreChunk:
        //   ClPfScoreFlow → flow-field distance (BFS to finish)
        //   ClPfScoreDist → euclidean distance to finish
        //   Both on        → min(flow, euclidean) — admissible
        //   Both off       → 0 (Dijkstra, blind search)
        bool flowOn = g_Config.m_ClPfScoreFlow != 0;
        bool distOn = g_Config.m_ClPfScoreDist != 0;

        if(!flowOn && !distOn)
                return 0.0f; // Dijkstra

        float hFlow = 1e18f;
        float hDist = 1e18f;

        if(flowOn && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                int tx = pf_clamp((int)(pos.x / 32.0f), 0, m_MapWidth - 1);
                int ty = pf_clamp((int)(pos.y / 32.0f), 0, m_MapHeight - 1);
                float fd = m_PfFlowField[ty * m_MapWidth + tx];
                if(fd < 1e17f)
                        hFlow = fd * 2.0f;
        }

        if(distOn)
                hDist = distance(pos, endPos);

        // If only one method is on, return its value (the other stays 1e18 → loses).
        // If both on, return min (admissible).
        float h = hFlow < hDist ? hFlow : hDist;
        if(h >= 1e17f)
                return 0.0f; // no usable estimate → don't mislead A* with huge value
        return h;
}

// Setup clean FutureWorld (local variable passed by caller — same as v1.55 PfGenerateChunk)
static void PfAStarSetupWorld(CGameWorld *pWorld, CGameClient *pGame, int LocalID)
{
        pWorld->CopyWorld(&pGame->m_PredictedWorld);
        for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
        {
                if(Type == CGameWorld::ENTTYPE_CHARACTER)
                        continue;
                std::vector<CEntity *> vRemove;
                for(CEntity *pEnt = pWorld->FindLast(Type); pEnt; pEnt = pEnt->TypePrev())
                        vRemove.push_back(pEnt);
                for(CEntity *pEnt : vRemove)
                        pWorld->RemoveEntity(pEnt);
        }
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalID)
                        continue;
                if(CCharacter *pChar = pWorld->GetCharacterById(i))
                        pWorld->RemoveEntity(pChar);
        }
}

bool CBotNet::PfAStarInit()
{
        CGameClient *pGame = GameClient();
        if(!pGame || !pGame->m_Snap.m_pLocalInfo)
                return false;
        int LocalID = pGame->m_Snap.m_LocalClientId;
        if(LocalID < 0)
                return false;
        if(!pGame->m_PredictedWorld.GetCharacterById(LocalID))
                return false;
        if(m_PfFinishTiles.empty())
                return false;

        m_PfANodes.clear();
        m_PfAOpen.clear();
        m_PfABestG.clear();
        m_PfAGoalIdx = -1;
        m_PfAExpandCount = 0;

        vec2 endPos = m_PfFinishTiles[0];
        float bestDist = 1e18f;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                float d = distance(fp, m_PfCurPos);
                if(d < bestDist) { bestDist = d; endPos = fp; }
        }
        dbg_msg("pathfinder", "A* init: start=(%.0f,%.0f) goal=(%.0f,%.0f) dist=%.0f",
                m_PfCurPos.x, m_PfCurPos.y, endPos.x, endPos.y, bestDist);

        AStarNode root;
        root.Core = pGame->m_PredictedWorld.GetCharacterById(LocalID)->GetCore();
        root.Core.m_Pos = m_PfCurPos;
        root.Core.m_Vel = m_PfCurVel;
        root.Core.m_HookState = m_PfCurHookState;
        root.Core.m_HookPos = m_PfCurHookPos;
        root.Core.m_HookDir = m_PfCurHookDir;
        root.Core.m_HookTick = m_PfCurHookTick;
        root.Core.m_Jumped = m_PfCurJumped;
        root.Pos = m_PfCurPos;
        root.FreezeTime = m_PfCurFreezeTime;
        CCollision *pColl = pGame->Collision();
        root.Grounded = pColl ? pColl->IsOnGround(root.Pos, CCharacterCore::PhysicalSize()) : false;
        root.G = 0.0f;
        root.H = PfAStarHeuristic(m_PfCurPos, endPos);
        root.F = root.G + (float)pf_clamp(g_Config.m_ClPfAStarWeight, 1, 20) * root.H;
        root.ParentIdx = -1;
        root.PrimIdx = -1;
        root.PrimTicks = 0;
        root.TickOffset = 0;
        m_PfANodes.push_back(root);
        m_PfABestG[PfAStarKey(root.Core, root.FreezeTime, root.Grounded)] = 0.0f;
        m_PfAOpen.push_back(0);

        if(fabsf(root.Pos.x - endPos.x) < 32.0f && fabsf(root.Pos.y - endPos.y) < 32.0f)
        {
                m_PfAGoalIdx = 0;
                m_PfAPathReady = true;
                m_PfFullInputs.clear();
                m_PfFullInputsIdx = 0;
                return true;
        }
        return true;
}

int CBotNet::PfAStarStep()
{
        if(m_PfAOpen.empty())
                return -1;

        int maxNodes = pf_clamp(g_Config.m_ClPfAStarNodeBudget, 1000, 1000000);
        int W = pf_clamp(g_Config.m_ClPfAStarWeight, 1, 20);
        const int maxTicks = 4000;
        const float hookLength = 380.0f;

        if((int)m_PfANodes.size() > maxNodes)
                return -1;

        m_PfAExpandCount++;
        if(m_PfAExpandCount % 1000 == 0)
                dbg_msg("pathfinder", "A* progress: %d expansions, %zu nodes, open=%zu",
                        m_PfAExpandCount, m_PfANodes.size(), m_PfAOpen.size());

        vec2 endPos = m_PfFinishTiles[0];
        float bestDist = 1e18f;
        for(const vec2 &fp : m_PfFinishTiles)
        {
                float d = distance(fp, m_PfCurPos);
                if(d < bestDist) { bestDist = d; endPos = fp; }
        }

        int curIdx = PfAStarHeapPop(m_PfAOpen, m_PfANodes);
        AStarNode cur = m_PfANodes[curIdx];

        uint64_t curKey = PfAStarKey(cur.Core, cur.FreezeTime, cur.Grounded);
        auto itCur = m_PfABestG.find(curKey);
        if(itCur == m_PfABestG.end() || itCur->second < cur.G - 0.001f)
                return 0;

        if(fabsf(cur.Pos.x - endPos.x) < 32.0f && fabsf(cur.Pos.y - endPos.y) < 32.0f)
        {
                m_PfAGoalIdx = curIdx;
                return 1;
        }

        if(cur.G >= (float)maxTicks)
                return 0;

        CGameClient *pGame = GameClient();
        if(!pGame)
                return -1;
        int LocalID = pGame->m_Snap.m_LocalClientId;

        CGameWorld FutureWorld;
        PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
        CCollision *pColl = FutureWorld.Collision();
        if(!pColl)
                return 0;

        int RootStartTick = FutureWorld.GameTick();
        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
        CCharacter *pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                          cur.Core.m_HookState, cur.Core.m_HookPos,
                                          cur.Core.m_HookDir, cur.Core.m_HookTick,
                                          cur.FreezeTime, cur.Core.m_Jumped);
        if(!pBot)
                return 0;
        // Restore m_JumpedTotal (PfSpawnSimBot copies from predicted world, not from node)
        {
                CCharacterCore c = pBot->GetCore();
                c.m_JumpedTotal = cur.Core.m_JumpedTotal;
                pBot->SetCore(c);
        }

        // FREEZE_SKIP
        if(pBot->m_FreezeTime > 0)
        {
                int ft = pBot->m_FreezeTime;
                std::vector<CNetObj_PlayerInput> emptyInputs;
                emptyInputs.resize(ft);
                for(int t = 0; t < ft; t++)
                {
                        mem_zero(&emptyInputs[t], sizeof(CNetObj_PlayerInput));
                        emptyInputs[t].m_TargetY = -1;
                }
                PfChunkResult res;
                PfSimulateChunk(&FutureWorld, LocalID, pBot, emptyInputs, RootStartTick + cur.TickOffset, res);
                if(res.died && !res.reachedFinish)
                        return 0;

                CCharacterCore endCore = cur.Core;
                endCore.m_Pos = res.endPos;
                endCore.m_Vel = res.endVel;
                endCore.m_HookState = res.endHookState;
                endCore.m_HookPos = res.endHookPos;
                endCore.m_HookDir = res.endHookDir;
                endCore.m_HookTick = res.endHookTick;
                endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                float newG = cur.G + (float)ft;
                auto it = m_PfABestG.find(newKey);
                if(it != m_PfABestG.end() && it->second <= newG)
                        return 0;
                m_PfABestG[newKey] = newG;

                AStarNode child;
                child.Core = endCore;
                child.Pos = res.endPos;
                child.FreezeTime = res.endFreezeTime;
                child.Grounded = endGrounded;
                child.G = newG;
                child.H = PfAStarHeuristic(child.Pos, endPos);
                child.F = child.G + (float)W * child.H;
                child.ParentIdx = curIdx;
                child.PrimIdx = 99;
                child.PrimTicks = ft;
                child.TickOffset = cur.TickOffset + ft;
                child.Traj = res.traj;
                        child.HookSegs = res.hookSegs;
                m_PfANodes.push_back(child);
                PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                return 0;
        }

        // === Parameter sweep (same as v1.55 PfGenerateChunk) ===
        // direction × jumpTick × hook × hookAngle
        int ChunkSize = pf_clamp(g_Config.m_ClPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
        int HookAngles = pf_clamp(g_Config.m_ClPfHookAngles, 4, 32);
        static const int aDirs[] = {-1, 0, 1};

        // Precompute hook raycast (same as v1.55)
        struct HookRayResult { float angle; bool hasTarget; };
        HookRayResult aHookRays[32];
        if(cur.Core.m_HookState == HOOK_IDLE)
        {
                for(int i = 0; i < HookAngles; i++)
                {
                        float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
                        float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
                        aHookRays[i].angle = angle;
                        vec2 endPos = cur.Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
                        vec2 hitPos;
                        int hit = pColl->IntersectLineTeleHook(cur.Pos, endPos, &hitPos, nullptr);
                        if(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH))
                                aHookRays[i].hasTarget = true;
                        else
                                aHookRays[i].hasTarget = false;
                }
        }

        bool hookIdle = (cur.Core.m_HookState == HOOK_IDLE);
        bool hookActive = (cur.Core.m_HookState == HOOK_FLYING || cur.Core.m_HookState == HOOK_GRABBED);

        // === Advanced search (v1.56.14): per-tick input combinations ===
        // Each tick in the chunk gets its OWN input (dir/jump/angle/hook), not one
        // input for the whole chunk. A flat counter over (dir,jump,angle,hook)
        // advances across ticks AND across candidates without restart, so every
        // tick of every candidate gets a unique combination.
        // One candidate = ChunkSize consecutive picks from the counter.
        if(g_Config.m_ClPfAdvancedSearch)
        {
                // Push helper: simulate one inSeq, push bestG + A* open heap.
                auto fnPushAdvanced = [&](std::vector<CNetObj_PlayerInput> &inSeq) {
                        if(!FutureWorld.GetCharacterById(LocalID))
                                PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                             cur.Core.m_HookState, cur.Core.m_HookPos,
                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                             cur.FreezeTime, cur.Core.m_Jumped);
                        if(!pBot)
                                return;
                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                        PfChunkResult res;
                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                        if(res.died && !res.reachedFinish)
                                return;

                        CCharacterCore endCore = cur.Core;
                        endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                        endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                        endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                        endCore.m_Jumped = res.endJumped;
                        endCore.m_JumpedTotal = res.endJumpedTotal;
                        bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                        uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                        float newG = cur.G + (float)ChunkSize;
                        auto it = m_PfABestG.find(newKey);
                        if(it != m_PfABestG.end() && it->second <= newG)
                                return;
                        m_PfABestG[newKey] = newG;

                        AStarNode child;
                        child.Core = endCore; child.Pos = res.endPos;
                        child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                        child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                        child.F = child.G + (float)W * child.H;
                        child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                        child.TickOffset = cur.TickOffset + ChunkSize;
                        child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                        m_PfANodes.push_back(child);
                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                };

                auto fnAimFor = [&](int angleIdx) -> vec2 {
                        float a = aHookRays[angleIdx].angle;
                        return vec2(cosf(a), sinf(a)) * 256.0f;
                };
                auto fnAimHooked = [&]() -> vec2 { return cur.Core.m_HookPos - cur.Pos; };

                const int nDir = 3;
                const int nJump = 2;
                const int nAngle = HookAngles;
                const int nHook = 2;
                const long totalCombos = (long)nDir * nJump * nAngle * nHook; // = 12 * HookAngles
                // Number of candidates: bounded by ClPfBacktrackCandidates, by available
                // combos (ceil(totalCombos / ChunkSize)), and by A* node budget.
                const int maxNodes = pf_clamp(g_Config.m_ClPfAStarNodeBudget, 1000, 1000000);
                const int dynAdvMax = (int)((totalCombos + ChunkSize - 1) / ChunkSize); // ceil(combos/CS)
                int numCandidates = pf_clamp(g_Config.m_ClPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynAdvMax);
                if(numCandidates > maxNodes) numCandidates = maxNodes;
                if(numCandidates < 1) numCandidates = 1;

                for(int cand = 0; cand < numCandidates; cand++)
                {
                        std::vector<CNetObj_PlayerInput> inSeq;
                        inSeq.reserve(ChunkSize);
                        // ChunkSize consecutive picks from counter, continuing across candidates.
                        for(int t = 0; t < ChunkSize; t++)
                        {
                                long idx = ((long)cand * ChunkSize + t) % totalCombos;
                                int hookVal = (int)(idx % nHook); idx /= nHook;
                                int angleIdx = (int)(idx % nAngle); idx /= nAngle;
                                int jumpVal = (int)(idx % nJump); idx /= nJump;
                                int dirVal = (int)(idx % nDir);

                                // Skip impossible hook fires (no hook target / not idle nor active).
                                if(hookVal == 1 && !hookIdle && !hookActive)
                                        hookVal = 0; // fall back to no-hook for this tick
                                if(hookVal == 1 && hookIdle && !aHookRays[angleIdx].hasTarget)
                                        hookVal = 0; // ray missed — no hook for this tick

                                CNetObj_PlayerInput inp;
                                mem_zero(&inp, sizeof(inp));
                                inp.m_Direction = aDirs[dirVal];
                                inp.m_Jump = jumpVal;
                                inp.m_Hook = hookVal;
                                vec2 aim;
                                if(hookVal == 1 && hookActive)
                                        aim = fnAimHooked();
                                else if(hookVal == 1 && hookIdle)
                                        aim = fnAimFor(angleIdx);
                                else
                                        aim = vec2((float)aDirs[dirVal] * 256.0f, -256.0f);
                                inp.m_TargetX = (int)aim.x;
                                inp.m_TargetY = (int)aim.y;
                                inSeq.push_back(inp);
                        }
                        fnPushAdvanced(inSeq);
                }

                return 0;
        }

        for(int dirIdx = 0; dirIdx < 3; dirIdx++)
        {
                int dir = aDirs[dirIdx];
                for(int jumpTick = -1; jumpTick < ChunkSize; jumpTick++)
                {
                        for(int hookOn = 0; hookOn <= 1; hookOn++)
                        {
                                if(hookOn == 1 && !hookIdle && !hookActive)
                                        continue;

                                if(hookOn == 1 && hookIdle)
                                {
                                        // Fire new hook: sweep over hook angles (with raycast filter)
                                        for(int angleIdx = 0; angleIdx < HookAngles; angleIdx++)
                                        {
                                                if(!aHookRays[angleIdx].hasTarget)
                                                        continue;
                                                float hookAngle = aHookRays[angleIdx].angle;

                                                // Build inputs (same as v1.55)
                                                std::vector<CNetObj_PlayerInput> inSeq;
                                                inSeq.reserve(ChunkSize);
                                                vec2 aimRel = vec2(cosf(hookAngle), sinf(hookAngle)) * 256.0f;
                                                for(int t = 0; t < ChunkSize; t++)
                                                {
                                                        CNetObj_PlayerInput inp;
                                                        mem_zero(&inp, sizeof(inp));
                                                        inp.m_Direction = dir;
                                                        inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                        inp.m_Hook = 1;
                                                        inp.m_TargetX = (int)aimRel.x;
                                                        inp.m_TargetY = (int)aimRel.y;
                                                        inSeq.push_back(inp);
                                                }

                                                // Re-spawn bot
                                                if(!FutureWorld.GetCharacterById(LocalID))
                                                        PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
                                                FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                                pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                                     cur.Core.m_HookState, cur.Core.m_HookPos,
                                                     cur.Core.m_HookDir, cur.Core.m_HookTick,
                                                     cur.FreezeTime, cur.Core.m_Jumped);
                                                if(!pBot)
                                                        continue;
                                                { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                                                PfChunkResult res;
                                                PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                                                if(res.died && !res.reachedFinish)
                                                        continue;

                                                CCharacterCore endCore = cur.Core;
                                                endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                                                endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                                                endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                                                endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                                                bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                                                uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                                                float newG = cur.G + (float)ChunkSize;
                                                auto it = m_PfABestG.find(newKey);
                                                if(it != m_PfABestG.end() && it->second <= newG)
                                                        continue;
                                                m_PfABestG[newKey] = newG;

                                                AStarNode child;
                                                child.Core = endCore; child.Pos = res.endPos;
                                                child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                                                child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                                                child.F = child.G + (float)W * child.H;
                                                child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                                                child.TickOffset = cur.TickOffset + ChunkSize;
                                                child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                                                m_PfANodes.push_back(child);
                                                PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                                        }
                                }
                                else if(hookOn == 1 && hookActive)
                                {
                                        // Hold existing hook (no angle sweep needed)
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 1;
                                                vec2 aimRel = cur.Core.m_HookPos - cur.Pos;
                                                inp.m_TargetX = (int)aimRel.x;
                                                inp.m_TargetY = (int)aimRel.y;
                                                inSeq.push_back(inp);
                                        }

                                        if(!FutureWorld.GetCharacterById(LocalID))
                                                PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
                                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                             cur.Core.m_HookState, cur.Core.m_HookPos,
                                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                                             cur.FreezeTime, cur.Core.m_Jumped);
                                        if(!pBot)
                                                continue;
                                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        CCharacterCore endCore = cur.Core;
                                        endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                                        endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                                        endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                                        endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                                        bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                                        uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                                        float newG = cur.G + (float)ChunkSize;
                                        auto it = m_PfABestG.find(newKey);
                                        if(it != m_PfABestG.end() && it->second <= newG)
                                                continue;
                                        m_PfABestG[newKey] = newG;

                                        AStarNode child;
                                        child.Core = endCore; child.Pos = res.endPos;
                                        child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                                        child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                                        child.F = child.G + (float)W * child.H;
                                        child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                                        child.TickOffset = cur.TickOffset + ChunkSize;
                                        child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                                        m_PfANodes.push_back(child);
                                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                                }
                                else // hookOn == 0: no hook
                                {
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 0;
                                                inp.m_TargetX = dir * 256;
                                                inp.m_TargetY = -256;
                                                inSeq.push_back(inp);
                                        }

                                        if(!FutureWorld.GetCharacterById(LocalID))
                                                PfAStarSetupWorld(&FutureWorld, pGame, LocalID);
                                        FutureWorld.m_GameTick = RootStartTick + cur.TickOffset;
                                        pBot = PfSpawnSimBot(&FutureWorld, LocalID, cur.Pos, cur.Core.m_Vel,
                                             cur.Core.m_HookState, cur.Core.m_HookPos,
                                             cur.Core.m_HookDir, cur.Core.m_HookTick,
                                             cur.FreezeTime, cur.Core.m_Jumped);
                                        if(!pBot)
                                                continue;
                                        { CCharacterCore c = pBot->GetCore(); c.m_JumpedTotal = cur.Core.m_JumpedTotal; pBot->SetCore(c); }

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, RootStartTick + cur.TickOffset, res);
                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        CCharacterCore endCore = cur.Core;
                                        endCore.m_Pos = res.endPos; endCore.m_Vel = res.endVel;
                                        endCore.m_HookState = res.endHookState; endCore.m_HookPos = res.endHookPos;
                                        endCore.m_HookDir = res.endHookDir; endCore.m_HookTick = res.endHookTick;
                                        endCore.m_Jumped = res.endJumped;
                endCore.m_JumpedTotal = res.endJumpedTotal;
                                        bool endGrounded = pColl->IsOnGround(res.endPos, CCharacterCore::PhysicalSize());

                                        uint64_t newKey = PfAStarKey(endCore, res.endFreezeTime, endGrounded);
                                        float newG = cur.G + (float)ChunkSize;
                                        auto it = m_PfABestG.find(newKey);
                                        if(it != m_PfABestG.end() && it->second <= newG)
                                                continue;
                                        m_PfABestG[newKey] = newG;

                                        AStarNode child;
                                        child.Core = endCore; child.Pos = res.endPos;
                                        child.FreezeTime = res.endFreezeTime; child.Grounded = endGrounded;
                                        child.G = newG; child.H = PfAStarHeuristic(child.Pos, endPos);
                                        child.F = child.G + (float)W * child.H;
                                        child.ParentIdx = curIdx; child.PrimIdx = 0; child.PrimTicks = ChunkSize;
                                        child.TickOffset = cur.TickOffset + ChunkSize;
                                        child.Traj = res.traj; child.HookSegs = res.hookSegs; child.Inputs = inSeq;
                                        m_PfANodes.push_back(child);
                                        PfAStarHeapPush(m_PfAOpen, m_PfANodes, (int)m_PfANodes.size() - 1);
                                }
                        }
                }
        }

        return 0;
}

static void PfAStarBuildInputs(int primI, int pTicks, float aHookX[8], float aHookY[8],
                               std::vector<CNetObj_PlayerInput> &outSeq)
{
        int pDir = 0, pJumpTick = -1;
        bool pHookFire = false, pHookHold = false;
        int pHookAngleIdx = 0;
        bool isFreezeSkip = (primI == 99);
        if(!isFreezeSkip)
        {
                switch(primI)
                {
                        case 0: pDir = -1; break;
                        case 1: pDir = +1; break;
                        case 2: pDir = -1; pJumpTick = 0; break;
                        case 3: pDir = +1; pJumpTick = 0; break;
                        case 4: pDir = 0; pJumpTick = 0; break;
                        case 5: case 6: case 7: case 8:
                        case 9: case 10: case 11: case 12:
                                pHookFire = true; pHookAngleIdx = primI - 5; break;
                        case 13: pHookHold = true; break;
                        default: break;
                }
        }
        outSeq.clear();
        outSeq.reserve(pTicks);
        for(int t = 0; t < pTicks; t++)
        {
                CNetObj_PlayerInput inp;
                mem_zero(&inp, sizeof(inp));
                inp.m_Direction = pDir;
                inp.m_Jump = (t == pJumpTick) ? 1 : 0;
                inp.m_Hook = (pHookFire || pHookHold) ? 1 : 0;
                if(pHookFire)
                {
                        inp.m_TargetX = (int)(aHookX[pHookAngleIdx] * 256.0f);
                        inp.m_TargetY = (int)(aHookY[pHookAngleIdx] * 256.0f);
                }
                else
                {
                        inp.m_TargetX = pDir * 256;
                        inp.m_TargetY = -256;
                }
                outSeq.push_back(inp);
        }
}

void CBotNet::PfAStarResimulatePath(int targetIdx)
{
        if(targetIdx < 0 || targetIdx >= (int)m_PfANodes.size())
                return;

        // Walk parents from target to root
        std::vector<int> vPathIdx;
        for(int idx = targetIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
                vPathIdx.push_back(idx);
        std::reverse(vPathIdx.begin(), vPathIdx.end());

        // Build m_PfVPath from cached Traj in each node (no re-simulation — fast)
        m_PfVPath.clear();
        m_PfVPath.push_back(m_PfANodes[vPathIdx[0]].Pos);
        for(size_t i = 1; i < vPathIdx.size(); i++)
        {
                AStarNode &node = m_PfANodes[vPathIdx[i]];
                for(size_t k = 1; k < node.Traj.size(); k++)
                        m_PfVPath.push_back(node.Traj[k]);
        }
}

void CBotNet::PfAStarUpdatePreview()
{
        if(m_PfANodes.empty())
                return;

        int bestIdx = -1;
        float bestH = 1e18f;
        for(int idx : m_PfAOpen)
        {
                if(idx < 0 || idx >= (int)m_PfANodes.size())
                        continue;
                if(m_PfANodes[idx].H < bestH)
                {
                        bestH = m_PfANodes[idx].H;
                        bestIdx = idx;
                }
        }
        if(bestIdx < 0)
                return;

        PfAStarResimulatePath(bestIdx);
}

void CBotNet::PfAStarReconstruct()
{
        if(m_PfAGoalIdx < 0)
                return;

        PfAStarResimulatePath(m_PfAGoalIdx);

        std::vector<int> vPathIdx;
        for(int idx = m_PfAGoalIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
                vPathIdx.push_back(idx);
        std::reverse(vPathIdx.begin(), vPathIdx.end());

        m_PfFullInputs.clear();
        for(size_t i = 1; i < vPathIdx.size(); i++)
        {
                AStarNode &node = m_PfANodes[vPathIdx[i]];
                for(size_t k = 0; k < node.Inputs.size(); k++)
                        m_PfFullInputs.push_back(node.Inputs[k]);
        }

        m_PfFullInputsIdx = 0;

        if(!vPathIdx.empty())
        {
                AStarNode &last = m_PfANodes[vPathIdx.back()];
                m_PfCurPos = last.Pos;
                m_PfCurVel = last.Core.m_Vel;
                m_PfCurHookState = last.Core.m_HookState;
                m_PfCurHookPos = last.Core.m_HookPos;
                m_PfCurHookDir = last.Core.m_HookDir;
                m_PfCurHookTick = last.Core.m_HookTick;
                m_PfCurJumped = last.Core.m_Jumped;
                m_PfCurFreezeTime = last.FreezeTime;
        }
}

// Render m_PfVPath (copy of RenderPackage with m_PfVPath substituted).
void CBotNet::RenderPathfinderPath()
{
        if(m_PfVPath.empty())
                return;

        // CBotNet is registered in m_vpAll BEFORE m_MapLayersBackground, so the
        // world projection (normally set up by CMapRenderer via MapScreenToInterface)
        // is NOT yet active when OnRender is called.  Without this, m_PfVPath
        // coordinates (world space) would be drawn against the UI/default screen
        // and end up off-screen / invisible.  Set the world projection explicitly
        // here so the trajectory renders in the same space as the map.
        CGameClient *pGame = GameClient();
        if(pGame)
                Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

        Graphics()->TextureClear();

        ColorRGBA BaseColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClTrajectoryColor, true));
        float ConfigAlpha = g_Config.m_ClTrajectoryAlpha / 100.0f;
        int LineSize = g_Config.m_ClTrajectoryLineSize;

        // If only the start point exists (no chunk generated yet, or all chunks
        // failed), render a small marker so the user sees the pathfinder is
        // active but stuck — instead of an invisible empty path.
        if(m_PfVPath.size() == 1)
        {
                Graphics()->QuadsBegin();
                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha);
                vec2 p = m_PfVPath[0];
                float s = 8.0f;
                IGraphics::CFreeformItem Quad(
                        p.x - s, p.y - s, p.x + s, p.y - s,
                        p.x - s, p.y + s, p.x + s, p.y + s);
                Graphics()->QuadsDrawFreeform(&Quad, 1);
                Graphics()->QuadsEnd();
                return;
        }

        // === Show flow field (vector arrows pointing toward finish) ===
        if(g_Config.m_ClPfShowField && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
        {
                int tx0 = pf_clamp((int)(pGame->m_Camera.m_Center.x / 32.0f) - 30, 0, m_MapWidth - 1);
                int ty0 = pf_clamp((int)(pGame->m_Camera.m_Center.y / 32.0f) - 20, 0, m_MapHeight - 1);
                int tx1 = pf_clamp(tx0 + 60, 0, m_MapWidth - 1);
                int ty1 = pf_clamp(ty0 + 40, 0, m_MapHeight - 1);
                Graphics()->LinesBegin();
                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.3f);
                for(int ty = ty0; ty <= ty1; ty++)
                {
                        for(int tx = tx0; tx <= tx1; tx++)
                        {
                                float fd = m_PfFlowField[ty * m_MapWidth + tx];
                                if(fd >= 1e17f || fd == 0.0f)
                                        continue; // unreachable or already at finish

                                // Gradient-based direction (360°, not 8-directional).
                                // Unreachable neighbors (>= 1e17, e.g. walls/freeze) are
                                // treated as equal to the current tile so they don't
                                // create a spurious "away from wall" gradient component.
                                float fdx_m = (tx > 0) ? m_PfFlowField[ty * m_MapWidth + (tx - 1)] : fd;
                                float fdx_p = (tx < m_MapWidth - 1) ? m_PfFlowField[ty * m_MapWidth + (tx + 1)] : fd;
                                float fdy_m = (ty > 0) ? m_PfFlowField[(ty - 1) * m_MapWidth + tx] : fd;
                                float fdy_p = (ty < m_MapHeight - 1) ? m_PfFlowField[(ty + 1) * m_MapWidth + tx] : fd;
                                if(fdx_m >= 1e17f) fdx_m = fd;
                                if(fdx_p >= 1e17f) fdx_p = fd;
                                if(fdy_m >= 1e17f) fdy_m = fd;
                                if(fdy_p >= 1e17f) fdy_p = fd;

                                float gx = fdx_m - fdx_p; // negative = finish is to the left
                                float gy = fdy_m - fdy_p; // negative = finish is above
                                float glen = sqrtf(gx * gx + gy * gy);
                                if(glen < 0.001f)
                                        continue;
                                vec2 dir = vec2(gx / glen, gy / glen);

                                // Arrow centered on tile: from center-dir*6 to center+dir*6
                                float cx = tx * 32.0f + 16.0f;
                                float cy = ty * 32.0f + 16.0f;
                                float half = 6.0f;
                                vec2 start = vec2(cx, cy) - dir * half;
                                vec2 end = vec2(cx, cy) + dir * half;
                                vec2 perp = vec2(-dir.y, dir.x) * 4.0f;
                                vec2 arrowBack = end - dir * 5.0f;

                                // Main line
                                IGraphics::CLineItem Main(start.x, start.y, end.x, end.y);
                                Graphics()->LinesDraw(&Main, 1);
                                // Arrowhead: two small lines
                                IGraphics::CLineItem A1(end.x, end.y, arrowBack.x + perp.x, arrowBack.y + perp.y);
                                Graphics()->LinesDraw(&A1, 1);
                                IGraphics::CLineItem A2(end.x, end.y, arrowBack.x - perp.x, arrowBack.y - perp.y);
                                Graphics()->LinesDraw(&A2, 1);
                        }
                }
                Graphics()->LinesEnd();
        }

        // === Show other branches (semi-transparent, using trajectory settings) ===
        if(g_Config.m_ClPfShowBranches && m_PfANodes.size() > 1)
        {
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
                if(LineSize > 0)
                {
                        std::vector<IGraphics::CFreeformItem> vQuads;
                        for(size_t i = 1; i < m_PfANodes.size(); i++)
                        {
                                AStarNode &node = m_PfANodes[i];
                                if(node.ParentIdx < 0 || node.Traj.size() < 2)
                                        continue;
                                for(size_t k = 1; k < node.Traj.size(); k++)
                                {
                                        vec2 p0 = node.Traj[k - 1];
                                        vec2 p1 = node.Traj[k];
                                        vec2 Dir = normalize(p1 - p0);
                                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
                                        vQuads.emplace_back(
                                                p0.x - Perp.x, p0.y - Perp.y,
                                                p0.x + Perp.x, p0.y + Perp.y,
                                                p1.x - Perp.x, p1.y - Perp.y,
                                                p1.x + Perp.x, p1.y + Perp.y);
                                }
                        }
                        Graphics()->QuadsBegin();
                        for(size_t i = 0; i < vQuads.size(); i++)
                        {
                                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.2f);
                                Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
                        }
                        Graphics()->QuadsEnd();
                }
                else
                {
                        Graphics()->LinesBegin();
                        Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.2f);
                        for(size_t i = 1; i < m_PfANodes.size(); i++)
                        {
                                AStarNode &node = m_PfANodes[i];
                                if(node.ParentIdx < 0 || node.Traj.size() < 2)
                                        continue;
                                for(size_t k = 1; k < node.Traj.size(); k++)
                                {
                                        IGraphics::CLineItem Line(node.Traj[k - 1], node.Traj[k]);
                                        Graphics()->LinesDraw(&Line, 1);
                                }
                        }
                        Graphics()->LinesEnd();
                }
        }

        // === Show hooks (blue lines, using trajectory settings for width) ===
        // Draws ALL hook fire/hold segments along the best path, captured per-tick
        // in PfSimulateChunk (node.HookSegs). This shows multiple hook cycles inside
        // a single chunk (e.g. Advanced search per-tick hook fire), not just the
        // final HOOK_GRABBED state.
        if(g_Config.m_ClPfShowHooks && m_PfANodes.size() > 0)
        {
                // Find best path (same as preview)
                int bestIdx = -1;
                float bestH = 1e18f;
                for(int idx : m_PfAOpen)
                {
                        if(idx < 0 || idx >= (int)m_PfANodes.size())
                                continue;
                        if(m_PfANodes[idx].H < bestH)
                        {
                                bestH = m_PfANodes[idx].H;
                                bestIdx = idx;
                        }
                }
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
                if(LineSize > 0 && bestIdx >= 0)
                {
                        std::vector<IGraphics::CFreeformItem> vQuads;
                        for(int idx = bestIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
                        {
                                AStarNode &node = m_PfANodes[idx];
                                for(const auto &seg : node.HookSegs)
                                {
                                        vec2 p0 = seg.teePos;
                                        vec2 p1 = seg.hookPos;
                                        vec2 d = p1 - p0;
                                        float len = length(d);
                                        if(len < 0.001f)
                                                continue;
                                        vec2 Dir = d / len;
                                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
                                        vQuads.emplace_back(
                                                p0.x - Perp.x, p0.y - Perp.y,
                                                p0.x + Perp.x, p0.y + Perp.y,
                                                p1.x - Perp.x, p1.y - Perp.y,
                                                p1.x + Perp.x, p1.y + Perp.y);
                                }
                        }
                        Graphics()->QuadsBegin();
                        for(size_t i = 0; i < vQuads.size(); i++)
                        {
                                Graphics()->SetColor(0.2f, 0.4f, 1.0f, ConfigAlpha);
                                Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
                        }
                        Graphics()->QuadsEnd();
                }
                else if(bestIdx >= 0)
                {
                        Graphics()->LinesBegin();
                        Graphics()->SetColor(0.2f, 0.4f, 1.0f, ConfigAlpha);
                        for(int idx = bestIdx; idx >= 0; idx = m_PfANodes[idx].ParentIdx)
                        {
                                AStarNode &node = m_PfANodes[idx];
                                for(const auto &seg : node.HookSegs)
                                {
                                        IGraphics::CLineItem Line(seg.teePos, seg.hookPos);
                                        Graphics()->LinesDraw(&Line, 1);
                                }
                        }
                        Graphics()->LinesEnd();
                }
        }

        // === Main path render (with optional speed gradient) ===
        // Speed gradient: full rainbow via HSL hue sweep.
        // Speed range [0, 30] px/tick mapped to hue [0°, 300°]:
        //   0    px/tick → red    (hue 0)
        //   5    px/tick → orange (hue 50)
        //   10   px/tick → yellow (hue 100)
        //   15   px/tick → green  (hue 150)
        //   20   px/tick → cyan   (hue 200)
        //   25   px/tick → blue   (hue 250)
        //   30+  px/tick → purple (hue 300)
        // 30 px/tick covers typical DDNet speeds (run ~10, jump ~12, fall ~17, hook-swing 20-30).
        // S=1, L=0.5 → pure rainbow colors, smooth interpolation across the whole range.
        auto fnSpeedColor = [](float speed) -> ColorRGBA {
                const float MAX_SPEED = 30.0f;
                float t = std::clamp(speed / MAX_SPEED, 0.0f, 1.0f);
                float hue = t * 300.0f; // 0..300 degrees
                return color_cast<ColorRGBA>(ColorHSLA(hue / 360.0f, 1.0f, 0.5f, true));
        };

        if(LineSize > 0)
        {
                float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
                Graphics()->QuadsBegin();
                for(size_t i = 1; i < m_PfVPath.size(); i++)
                {
                        vec2 p0 = m_PfVPath[i - 1];
                        vec2 p1 = m_PfVPath[i];
                        vec2 Dir = normalize(p1 - p0);
                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
                        IGraphics::CFreeformItem Quad(
                                p0.x - Perp.x, p0.y - Perp.y,
                                p0.x + Perp.x, p0.y + Perp.y,
                                p1.x - Perp.x, p1.y - Perp.y,
                                p1.x + Perp.x, p1.y + Perp.y);

                        if(g_Config.m_ClPfShowSpeed)
                        {
                                // Speed at start and end of this segment.
                                float speed0 = (i > 1) ? distance(m_PfVPath[i - 2], m_PfVPath[i - 1]) : distance(p0, p1);
                                float speed1 = distance(p0, p1);
                                ColorRGBA c0 = fnSpeedColor(speed0);
                                ColorRGBA c1 = fnSpeedColor(speed1);
                                // Per-vertex: start vertices get c0, end vertices get c1
                                Graphics()->SetColor4(
                                        ColorRGBA(c0.r, c0.g, c0.b, ConfigAlpha), // TL (start)
                                        ColorRGBA(c0.r, c0.g, c0.b, ConfigAlpha), // TR (start)
                                        ColorRGBA(c1.r, c1.g, c1.b, ConfigAlpha), // BL (end)
                                        ColorRGBA(c1.r, c1.g, c1.b, ConfigAlpha)); // BR (end)
                        }
                        else
                        {
                                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha);
                        }
                        Graphics()->QuadsDrawFreeform(&Quad, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                for(size_t i = 1; i < m_PfVPath.size(); i++)
                {
                        if(g_Config.m_ClPfShowSpeed)
                        {
                                float speed = distance(m_PfVPath[i - 1], m_PfVPath[i]);
                                ColorRGBA c = fnSpeedColor(speed);
                                Graphics()->SetColor(c.r, c.g, c.b, ConfigAlpha);
                        }
                        else
                        {
                                Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha);
                        }
                        IGraphics::CLineItem Line(m_PfVPath[i - 1], m_PfVPath[i]);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

// =========================================================
// STATE-LATTICE A* — MOTION PRIMITIVES
// =========================================================
//
// The planner searches a state lattice where each node is a discrete
// (tile, velocity-bucket, grounded, hooked) state and each edge is a
// motion primitive: a short fixed input sequence (run / jump / hook-swing).
// A* uses the pathfinder distance field (m_pfDist) as the heuristic, so
// search is guided toward the goal instead of blindly sampling.
//
// Why this fixes the "stuck nudging one pixel" problem:
//   The old code scored only the final tile of a single long trajectory and
//   picked the argmin.  A tiny nudge that crosses one tile boundary improved
//   the score by 1, while any real maneuver (jump/hook) needed many ticks to
//   pay off and almost never won.  A* compares f = g + h for *every* neighbor
//   of *every* state, so a primitive that reaches a much closer tile always
//   wins over a one-pixel nudge, and tricks are discovered instead of hoped for.
// =========================================================

// A single motion primitive: a short fixed input sequence representing an
// atomic maneuver (run, jump, hook-swing, wait).
struct SMotionPrimitive
{
        int Ticks; // duration in ticks
        int Direction; // -1, 0, +1 (horizontal input)
        int JumpTick; // tick index to press jump (-1 = never)
        int HookStartTick; // tick index to start hook (-1 = never)
        int HookHoldTicks; // how long to hold the hook
        float HookAngle; // hook aim angle in radians (relative to bot)
        bool RequiresGround; // precondition: bot must be on ground
};

// Build the motion-primitive table.
//
// v1.40 redesign (verified via Python model in /home/z/my-project/verify/):
//   - RUN is 8 ticks (not 4) so each primitive reliably crosses a tile
//     boundary and the tile-based heuristic actually decreases.
//   - HOOK_FIRE angles are restricted to [pi/6, 5pi/6] (30-150 deg) so
//     hooks always aim UPWARD.  Combined with the y-axis sign fix in
//     FindHookGrabPoint, this prevents the "hook grabs the floor ahead"
//     exploit that made v1.39 fire useless hooks on flat ground.
//   - HOOK_FIRE holds the hook for the FULL 10 ticks (not 8+2 release).
//     The 2-tick release in v1.39 ALWAYS detached the hook mid-swing,
//     making sustained swings impossible ("can't hold hook" bug).
//   - New HOOK_HOLD_{LEFT,RIGHT,NEUTRAL} primitives continue an existing
//     swing: precondition m_HookState == HOOK_GRABBED, keep m_Hook == 1,
//     allow directional input so the bot can steer mid-swing.
//   - New HOOK_RELEASE_3 primitive cleanly detaches an active hook.
//
// Primitive classification (used by ComputeKinodynamic for preconditions):
//   - HOOK_FIRE:   hook_start_tick == 0 AND hook_hold_ticks == ticks
//                  (name prefix "HOOK_FIRE" — fires a NEW hook)
//   - HOOK_HOLD:   hook_start_tick == 0 AND hook_hold_ticks == ticks
//                  AND direction != 0 or name starts with "HOOK_HOLD"
//                  Actually we encode a flag via hook_hold_ticks == ticks
//                  and distinguish by a sentinel: HOOK_HOLD uses
//                  HookAngle = -1.0f, HOOK_RELEASE uses HookAngle = -2.0f.
//                  This avoids string comparisons in the hot loop.
static void BuildPrimitiveTable(int hookAngles, SMotionPrimitive *pPrims, int &outCount)
{
        outCount = 0;

        // --- Base movement primitives ---
        // RUN: 8-tick ground run to build / maintain horizontal speed and
        // reliably cross tile boundaries (8 ticks * ~10 px/tick = 80 px = 2.5 tiles)
        pPrims[outCount++] = {8, -1, -1, -1, 0, 0.0f, false}; // RUN_LEFT_8
        pPrims[outCount++] = {8, +1, -1, -1, 0, 0.0f, false}; // RUN_RIGHT_8
        // JUMP: jump + drift in a direction (requires ground)
        pPrims[outCount++] = {8, -1, 0, -1, 0, 0.0f, true}; // JUMP_LEFT_8
        pPrims[outCount++] = {8, +1, 0, -1, 0, 0.0f, true}; // JUMP_RIGHT_8
        pPrims[outCount++] = {8, 0, 0, -1, 0, 0.0f, true}; // JUMP_UP_8
        // STAY: brief wait / brake (no movement input)
        pPrims[outCount++] = {3, 0, -1, -1, 0, 0.0f, false}; // STAY_3

        // --- HOOK_FIRE primitives ---
        // v1.41: angles span the FULL upper hemisphere [0, pi] (with the
        // y-axis sign fix in FindHookGrabPoint, these all point UP or
        // HORIZONTAL — never DOWN).  This includes near-horizontal angles
        // (~0 and ~pi) which are ESSENTIAL for sideways hooking off walls
        // at the same height as the bot — without them the bot can't hook
        // sideways when falling, which was the v1.40 "doesn't hook when it
        // could" bug.
        // Hook is held for the FULL 10 ticks (no 2-tick release) so the
        // swing can persist across consecutive HOOK_FIRE / HOOK_HOLD
        // primitives.  HookAngle > 0.0f is the "fire" sentinel.
        hookAngles = pf_clamp(hookAngles, 4, 32);
        for(int i = 0; i < hookAngles && outCount < LATTICE_MAX_PRIMITIVES; i++)
        {
                float angle = (float)pi * ((float)i + 0.5f) / (float)hookAngles;
                pPrims[outCount++] = {10, 0, -1, 0, 10, angle, false}; // HOOK_FIRE_i (hold full 10)
        }

        // --- HOOK_HOLD primitives ---
        // Continue an existing swing.  Precondition: m_HookState == HOOK_GRABBED.
        // Keep m_Hook == 1 for the full 6 ticks, allow directional input so
        // the bot can steer toward where it wants to land.
        // Sentinel: HookAngle = -1.0f marks HOOK_HOLD variants.
        if(outCount < LATTICE_MAX_PRIMITIVES)
                pPrims[outCount++] = {6, -1, -1, 0, 6, -1.0f, false}; // HOOK_HOLD_LEFT
        if(outCount < LATTICE_MAX_PRIMITIVES)
                pPrims[outCount++] = {6, +1, -1, 0, 6, -1.0f, false}; // HOOK_HOLD_RIGHT
        if(outCount < LATTICE_MAX_PRIMITIVES)
                pPrims[outCount++] = {6, 0, -1, 0, 6, -1.0f, false}; // HOOK_HOLD_NEUTRAL

        // --- HOOK_RELEASE primitive ---
        // Cleanly detach an active hook.  Precondition: m_HookState in
        // {HOOK_FLYING, HOOK_GRABBED}.  Sets m_Hook == 0 for 3 ticks.
        // Sentinel: HookAngle = -2.0f marks HOOK_RELEASE.
        if(outCount < LATTICE_MAX_PRIMITIVES)
                pPrims[outCount++] = {3, 0, -1, -1, 0, -2.0f, false}; // HOOK_RELEASE_3
}

// Classify a primitive by its sentinel HookAngle value.
// (Avoids string comparisons in the A* hot loop.)
static inline bool IsHookFirePrim(const SMotionPrimitive &prim)
{
        // Fire: angle in [pi/6, 5pi/6], hook held for full duration
        return prim.HookStartTick >= 0 && prim.HookHoldTicks == prim.Ticks && prim.HookAngle > 0.0f;
}
static inline bool IsHookHoldPrim(const SMotionPrimitive &prim)
{
        return prim.HookAngle == -1.0f;
}
static inline bool IsHookReleasePrim(const SMotionPrimitive &prim)
{
        return prim.HookAngle == -2.0f;
}

// Build the per-tick input array for a single motion primitive.
//
// v1.40: HOOK_FIRE and HOOK_HOLD now hold m_Hook == 1 for the FULL primitive
// duration (no 2-tick release).  This is intentional — it lets the hook stay
// attached across consecutive primitives so the bot can sustain a swing.
// HOOK_RELEASE sets m_Hook == 0 to detach.  Base primitives (RUN/JUMP/STAY)
// end with m_Jump == 0 and m_Hook == 0 as before.
//
// The full CCharacterCore state (including m_HookState, m_HookPos) is saved
// and restored via SetCore between primitive expansions, so the hook state
// machine carries over correctly between primitives in the A* search.
//
// For hook-fire primitives, pass a non-zero pAimRel to aim at a specific grab
// point (relative to bot).  When pAimRel is null, the primitive's fixed
// HookAngle is used (fallback).  For hook-hold primitives, pAimRel is the
// current hook attachment point (visual only — the state machine controls
// the pull once attached).
static void BuildPrimitiveInputs(const SMotionPrimitive &prim, CNetObj_PlayerInput *pInputs, const vec2 *pAimRel = nullptr)
{
        bool useAimRel = (pAimRel != nullptr);
        for(int t = 0; t < prim.Ticks; t++)
        {
                pInputs[t].m_Direction = prim.Direction;
                pInputs[t].m_Jump = (t == prim.JumpTick) ? 1 : 0;
                bool hookActive = (prim.HookStartTick >= 0 && t >= prim.HookStartTick && t < prim.HookStartTick + prim.HookHoldTicks);
                pInputs[t].m_Hook = hookActive ? 1 : 0;

                if(hookActive)
                {
                        if(useAimRel)
                        {
                                // Aim at the actual grab point discovered by raycast.
                                // Direction is what matters (server normalises), but we
                                // also scale the vector so the visual cursor sits on
                                // the grab point rather than 200px out along the angle.
                                pInputs[t].m_TargetX = (int)pAimRel->x;
                                pInputs[t].m_TargetY = (int)pAimRel->y;
                        }
                        else
                        {
                                // Fallback: fixed-angle aim (legacy).
                                // Screen coords: y DOWN — negate sin so angle pi/2 = UP.
                                float aimDist = 200.0f;
                                pInputs[t].m_TargetX = (int)(cosf(prim.HookAngle) * aimDist);
                                pInputs[t].m_TargetY = (int)(-sinf(prim.HookAngle) * aimDist);
                        }
                }
                else
                {
                        // Aim in movement direction
                        float aimDist = 100.0f;
                        pInputs[t].m_TargetX = (int)((float)prim.Direction * aimDist);
                        pInputs[t].m_TargetY = (prim.JumpTick >= 0 && t <= prim.JumpTick) ? (int)(-aimDist) : 0;
                }

                pInputs[t].m_Fire = 0;
                pInputs[t].m_WantedWeapon = 0;
                pInputs[t].m_PlayerFlags = 0;
                pInputs[t].m_NextWeapon = 0;
                pInputs[t].m_PrevWeapon = 0;
        }
}

// Discrete velocity bucket: -1 (left), 0 (stopped), +1 (right).
static inline int VelDirBucket(float vx)
{
        const float threshold = 1.0f; // ~1 px/tick = nearly stopped
        if(vx < -threshold)
                return -1;
        if(vx > threshold)
                return +1;
        return 0;
}

// Hash a discrete lattice state: (tx, ty, velDir, onGround, hooked).
// 12 bits each for tx/ty (covers maps up to 4096 tiles), rest packed.
static inline uint64_t MakeStateKey(int tx, int ty, int velDir, bool onGround, bool hooked)
{
        uint64_t k = 0;
        k |= (uint64_t)(tx & 0xFFF);
        k |= (uint64_t)(ty & 0xFFF) << 12;
        k |= (uint64_t)(velDir + 1) << 24; // -1,0,+1 → 0,1,2
        k |= (uint64_t)(onGround ? 1 : 0) << 26;
        k |= (uint64_t)(hooked ? 1 : 0) << 27;
        return k;
}

// A* search node.  Stores a full CCharacterCore snapshot so the bot can be
// restored to this exact state when the node is expanded later.
struct SLatticeNode
{
        CCharacterCore Core; // full physics state at node entry
        float G; // cost so far (ticks + freeze penalty)
        float H; // heuristic = m_pfDist at the node's tile
        float F; // G + H * LATTICE_H_SCALE
        int ParentIdx; // index in the node array (-1 = root)
        int PrimIdx; // primitive used from parent (-1 = root)
        uint64_t StateKey;
        bool Closed;
        // Hook aim point (relative to bot at expansion time) used by hook
        // primitives.  Computed by raycasting at expansion time so the hook
        // always aims at a real hookable surface, not a blind fixed angle.
        // AimRelW == 0.0f means "no specific aim — use primitive's angle".
        float AimRelX;
        float AimRelY;
        float AimRelW;
};

// Raycast from `from` in direction `angle` and return true if a hookable
// surface is found within maxDist.  outGrab = world position of the grab point.
// Returns false for: no hit, TILE_NOHOOK (hook would retract, not grab),
// TILE_TELEINHOOK (teleport-hook — too complex to plan around reliably).
static bool FindHookGrabPoint(CCollision *pColl, vec2 from, float angle, float maxDist, vec2 &outGrab)
{
        // Screen coordinates: y increases DOWNWARD.  Angle convention is
        // 0 = +x (right), pi/2 = UP, pi = -x (left).  So we NEGATE sin to
        // convert math-up to screen-up.  Without this negation, angles in
        // [0, pi] aim DOWNWARD and the hook grabs the floor ahead — the
        // root cause of the v1.39 "hook fires at nothing 100 tiles away"
        // and "can't hold hook" bugs.
        vec2 dir = vec2(cosf(angle), -sinf(angle));
        vec2 endPos = from + dir * maxDist;
        vec2 hitPos, hitBefore;
        int hit = pColl->IntersectLineTeleHook(from, endPos, &hitPos, &hitBefore, nullptr);
        if(hit == 0)
                return false; // no hit — hook would fly into the void
        if(hit == TILE_NOHOOK)
                return false; // hook would retract, not grab
        if(hit == TILE_TELEINHOOK)
                return false; // teleport-hook — skip for predictability
        // v1.41: NO 'above bot' filter — it rejected legitimate sideways
        // grabs on walls at the same height as the bot, which made the bot
        // unable to hook sideways when falling (the "doesn't hook when it
        // could" bug).  With the y-axis sign fix above, angles in [0, pi]
        // all point UP or HORIZONTAL — never DOWN — so floor grabs are
        // impossible from a grounded bot (the ray stays at bot-center
        // height, above the floor).
        outGrab = hitPos;
        return true;
}


// =========================================================
// PATHFINDER TAB — PfGenerateChunk (v1.50: FutureWorld + CCharacter)
// =========================================================
// Generate one chunk by simulating REAL CCharacter in a cloned CGameWorld.
// Freeze / finish / death are handled by DDNet's own CCharacter logic — no
// hand-rolled tile checks or custom freeze counters.
//
// Parameter sweep: direction x jumpTick x hook x hookAngle
//   direction   in {-1, 0, +1}
//   jumpTick    in {-1, 0, 1, ..., ChunkSize-1}   (-1 = no jump)
//   hook        in {0, 1}
//   hookAngle   in [-5pi/6, +5pi/6] evenly spaced   (ONLY when hook=1 and HOOK_IDLE)
//
// Top-N candidates (N = ClPfBacktrackCandidates, default 100) are kept for the
// backtracking buffer.
//
// Per candidate: save pBot state (core + pos + m_FreezeTime), simulate via
// PfSimulateChunk, restore.  This is the v1.48 (5c28aac) fix — without restoring
// m_FreezeTime, freeze state leaks across candidates.

bool CBotNet::PfGenerateChunk(std::vector<CNetObj_PlayerInput> &outInputs, std::vector<vec2> &outTraj,
                              bool &outReachedFinish, bool &outDied, int &outFreezeTicks)
{
        CGameClient *pGame = GameClient();
        if(!pGame)
                return false;

        int LocalID = pGame->m_Snap.m_LocalClientId;
        if(LocalID < 0)
                return false;

        if(!pGame->m_PredictedWorld.GetCharacterById(LocalID))
                return false;

        int ChunkSize = pf_clamp(g_Config.m_ClPfChunkSize, CConfig::PF_CHUNK_SIZE_MIN, PF_TAB_MAX_CHUNK_TICKS);
        int HookAngles = pf_clamp(g_Config.m_ClPfHookAngles, 4, 32);

        // Clean planning world (bot + collision only).  Other entities are
        // removed so they don't interfere with the prediction.
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(&pGame->m_PredictedWorld);
        for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
        {
                if(Type == CGameWorld::ENTTYPE_CHARACTER)
                        continue;
                std::vector<CEntity *> vRemove;
                for(CEntity *pEnt = FutureWorld.FindLast(Type); pEnt; pEnt = pEnt->TypePrev())
                        vRemove.push_back(pEnt);
                for(CEntity *pEnt : vRemove)
                        FutureWorld.RemoveEntity(pEnt);
        }
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalID)
                        continue;
                if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                        FutureWorld.RemoveEntity(pChar);
        }

        CCharacter *pBot = PfSpawnSimBot(&FutureWorld, LocalID, m_PfCurPos, m_PfCurVel,
                                         m_PfCurHookState, m_PfCurHookPos, m_PfCurHookDir,
                                         m_PfCurHookTick, m_PfCurFreezeTime, m_PfCurJumped);
        if(!pBot)
                return false;

        int StartTick = FutureWorld.GameTick();
        CCollision *pColl = FutureWorld.Collision();
        if(!pColl)
                return false;

        // ── Freeze-skip (v1.55): if the bot is frozen, fast-forward through
        // all freeze ticks as ONE chunk with empty input. This avoids wasting
        // multiple chunks on freeze stall. The frozen bot still experiences
        // gravity/friction (real DDNet behavior via CCharacter), so it
        // falls/slides correctly during the skip.
        // Discovered via Python benchmark: maps with freeze (e.g. map 9 with
        // 3 freeze walls = 450 ticks) are unsolvable without this macro-step.
        if(pBot->m_FreezeTime > 0)
        {
                int freezeTicks = pBot->m_FreezeTime;
                std::vector<CNetObj_PlayerInput> emptyInputs;
                emptyInputs.reserve(freezeTicks);
                for(int t = 0; t < freezeTicks; t++)
                {
                        CNetObj_PlayerInput inp;
                        mem_zero(&inp, sizeof(inp));
                        inp.m_TargetY = -1; // aim up (avoid (0,0) target)
                        emptyInputs.push_back(inp);
                }
                PfChunkResult res;
                PfSimulateChunk(&FutureWorld, LocalID, pBot, emptyInputs, StartTick, res);
                if(res.died && !res.reachedFinish)
                        return false;
                // Return this as the chunk
                outInputs = res.inputs;
                outTraj = res.traj;
                outReachedFinish = res.reachedFinish;
                outDied = res.died;
                outFreezeTicks = res.freezeTicks;
                // Update sim state from end of freeze
                m_PfCurPos = res.endPos;
                m_PfCurVel = res.endVel;
                m_PfCurHookPos = res.endHookPos;
                m_PfCurHookDir = res.endHookDir;
                m_PfCurHookState = res.endHookState;
                m_PfCurHookTick = res.endHookTick;
                m_PfCurJumped = res.endJumped;
                m_PfCurFreezeTime = res.endFreezeTime;
                return true;
        }

        float hookLength = pBot->GetCore().m_Tuning.m_HookLength;
        if(!(hookLength > 1.0f) || !(hookLength < 10000.0f))
                hookLength = 900.0f;

        // Hook ray precompute: same angles as before, filtered by raycast.
        struct HookRayResult {
                float angle;
                bool hasTarget;
        };
        HookRayResult aHookRays[32];
        if(pBot->GetCore().m_HookState == HOOK_IDLE)
        {
                for(int i = 0; i < HookAngles; i++)
                {
                        // Evenly spaced from -5pi/6 to +5pi/6
                        float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
                        float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
                        aHookRays[i].angle = angle;
                        vec2 endPos = pBot->m_Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
                        vec2 hitPos;
                        int hit = pColl->IntersectLineTeleHook(pBot->m_Pos, endPos, &hitPos, nullptr);
                        if(hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH))
                                aHookRays[i].hasTarget = true;
                        else
                                aHookRays[i].hasTarget = false;
                }
        }

        static const int aDirs[] = {-1, 0, 1};

        // Track top-3 candidates for backtrack buffer.
        struct CandidateResult {
                std::vector<CNetObj_PlayerInput> inputs;
                std::vector<vec2> traj;
                float score;
                bool reachedFinish;
                bool died;
                int freezeTicks;
                int endFreezeTime;
                vec2 endPos, endVel, endHookPos, endHookDir;
                int endHookState, endHookTick, endJumped;
        };
        // Top-N candidates (N = ClPfBacktrackCandidates, dynamic). Using a vector
        // keeps memory bounded — only N slots per chunk, freed on reset.
        // Max for current params = 3 * (ChunkSize+1) * (1+HookAngles) (see PF_CANDIDATES_MAX).
        const int dynMax = 3 * (ChunkSize + 1) * (1 + HookAngles);
        const int N = pf_clamp(g_Config.m_ClPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynMax);
        std::vector<CandidateResult> topCandidates(N);
        for(int i = 0; i < N; i++)
                topCandidates[i].score = -1e18f;

        // === Advanced search (v1.56.14): per-tick input combinations ===
        // Same flat-counter mechanism as in PfAStarStep — each tick of every
        // candidate gets a unique (dir,jump,angle,hook) combination, the counter
        // keeps advancing across ticks and candidates without restart.
        if(g_Config.m_ClPfAdvancedSearch)
        {
                bool hookIdleAdv = (pBot->GetCore().m_HookState == HOOK_IDLE);
                bool hookActiveAdv = (pBot->GetCore().m_HookState == HOOK_FLYING || pBot->GetCore().m_HookState == HOOK_GRABBED);

                auto fnAimForAdv = [&](int angleIdx) -> vec2 {
                        float a = aHookRays[angleIdx].angle;
                        return vec2(cosf(a), sinf(a)) * 256.0f;
                };
                auto fnAimHookedAdv = [&]() -> vec2 { return pBot->GetCore().m_HookPos - pBot->m_Pos; };

                const int nDir = 3;
                const int nJump = 2;
                const int nAngle = HookAngles;
                const int nHook = 2;
                const long totalCombos = (long)nDir * nJump * nAngle * nHook; // = 12 * HookAngles
                const int dynAdvMax = (int)((totalCombos + ChunkSize - 1) / ChunkSize);
                int numCandidates = pf_clamp(g_Config.m_ClPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynAdvMax);
                if(numCandidates < 1) numCandidates = 1;

                for(int cand = 0; cand < numCandidates; cand++)
                {
                        std::vector<CNetObj_PlayerInput> inSeq;
                        inSeq.reserve(ChunkSize);
                        for(int t = 0; t < ChunkSize; t++)
                        {
                                long idx = ((long)cand * ChunkSize + t) % totalCombos;
                                int hookVal = (int)(idx % nHook); idx /= nHook;
                                int angleIdx = (int)(idx % nAngle); idx /= nAngle;
                                int jumpVal = (int)(idx % nJump); idx /= nJump;
                                int dirVal = (int)(idx % nDir);

                                if(hookVal == 1 && !hookIdleAdv && !hookActiveAdv)
                                        hookVal = 0;
                                if(hookVal == 1 && hookIdleAdv && !aHookRays[angleIdx].hasTarget)
                                        hookVal = 0;

                                CNetObj_PlayerInput inp;
                                mem_zero(&inp, sizeof(inp));
                                inp.m_Direction = aDirs[dirVal];
                                inp.m_Jump = jumpVal;
                                inp.m_Hook = hookVal;
                                vec2 aim;
                                if(hookVal == 1 && hookActiveAdv)
                                        aim = fnAimHookedAdv();
                                else if(hookVal == 1 && hookIdleAdv)
                                        aim = fnAimForAdv(angleIdx);
                                else
                                        aim = vec2((float)aDirs[dirVal] * 256.0f, -256.0f);
                                inp.m_TargetX = (int)aim.x;
                                inp.m_TargetY = (int)aim.y;
                                inSeq.push_back(inp);
                        }

                        // Save bot state, simulate, restore (same pattern as main sweep).
                        CCharacterCore savedCore = pBot->GetCore();
                        vec2 savedPos = pBot->m_Pos;
                        int savedFreeze = pBot->m_FreezeTime;

                        PfChunkResult res;
                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                        pBot->SetCore(savedCore);
                        pBot->m_Pos = savedPos;
                        pBot->m_FreezeTime = savedFreeze;

                        if(res.died && !res.reachedFinish)
                                continue;

                        float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                        for(int k = 0; k < N; k++)
                        {
                                if(score > topCandidates[k].score)
                                {
                                        for(int m = N - 1; m > k; m--)
                                                topCandidates[m] = topCandidates[m - 1];
                                        topCandidates[k].inputs = res.inputs;
                                        topCandidates[k].traj = res.traj;
                                        topCandidates[k].score = score;
                                        topCandidates[k].reachedFinish = res.reachedFinish;
                                        topCandidates[k].died = res.died;
                                        topCandidates[k].freezeTicks = res.freezeTicks;
                                        topCandidates[k].endPos = res.endPos;
                                        topCandidates[k].endVel = res.endVel;
                                        topCandidates[k].endHookPos = res.endHookPos;
                                        topCandidates[k].endHookDir = res.endHookDir;
                                        topCandidates[k].endHookState = res.endHookState;
                                        topCandidates[k].endHookTick = res.endHookTick;
                                        topCandidates[k].endJumped = res.endJumped;
                                        topCandidates[k].endFreezeTime = res.endFreezeTime;
                                        break;
                                }
                        }
                }
                // Fall through to the existing topCandidates consumer code below.
        }
        else
        // Parameter sweep: direction x jumpTick x hook x hookAngle
        for(int dirIdx = 0; dirIdx < 3; dirIdx++)
        {
                int dir = aDirs[dirIdx];
                for(int jumpTick = -1; jumpTick < ChunkSize; jumpTick++)
                {
                        for(int hookOn = 0; hookOn <= 1; hookOn++)
                        {
                                bool hookIdle = (pBot->GetCore().m_HookState == HOOK_IDLE);
                                bool hookActive = (pBot->GetCore().m_HookState == HOOK_FLYING || pBot->GetCore().m_HookState == HOOK_GRABBED);
                                if(hookOn == 1 && !hookIdle && !hookActive)
                                        continue;

                                if(hookOn == 1 && hookIdle)
                                {
                                        // Fire new hook: sweep over hook angles (with raycast filter).
                                        for(int angleIdx = 0; angleIdx < HookAngles; angleIdx++)
                                        {
                                                if(!aHookRays[angleIdx].hasTarget)
                                                        continue;
                                                float hookAngle = aHookRays[angleIdx].angle;

                                                // Build the per-tick input for this candidate.
                                                std::vector<CNetObj_PlayerInput> inSeq;
                                                inSeq.reserve(ChunkSize);
                                                vec2 aimRel = vec2(cosf(hookAngle), sinf(hookAngle)) * 256.0f;
                                                for(int t = 0; t < ChunkSize; t++)
                                                {
                                                        CNetObj_PlayerInput inp;
                                                        mem_zero(&inp, sizeof(inp));
                                                        inp.m_Direction = dir;
                                                        inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                        inp.m_Hook = 1;
                                                        inp.m_TargetX = (int)aimRel.x;
                                                        inp.m_TargetY = (int)aimRel.y;
                                                        inp.m_Fire = 0;
                                                        inp.m_WantedWeapon = 0;
                                                        inp.m_PlayerFlags = 0;
                                                        inp.m_NextWeapon = 0;
                                                        inp.m_PrevWeapon = 0;
                                                        inSeq.push_back(inp);
                                                }

                                                // v1.48 fix: save full bot state (core + pos + m_FreezeTime)
                                                // before simulating, restore after — otherwise freeze state
                                                // leaks across candidates.
                                                CCharacterCore savedCore = pBot->GetCore();
                                                vec2 savedPos = pBot->m_Pos;
                                                int savedFreeze = pBot->m_FreezeTime;

                                                PfChunkResult res;
                                                PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                                                pBot->SetCore(savedCore);
                                                pBot->m_Pos = savedPos;
                                                pBot->m_FreezeTime = savedFreeze;

                                                if(res.died && !res.reachedFinish)
                                                        continue;

                                                float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                                                for(int k = 0; k < N; k++)
                                                {
                                                        if(score > topCandidates[k].score)
                                                        {
                                                                for(int m = N - 1; m > k; m--)
                                                                        topCandidates[m] = topCandidates[m - 1];
                                                                topCandidates[k].inputs = res.inputs;
                                                                topCandidates[k].traj = res.traj;
                                                                topCandidates[k].score = score;
                                                                topCandidates[k].reachedFinish = res.reachedFinish;
                                                                topCandidates[k].died = res.died;
                                                                topCandidates[k].freezeTicks = res.freezeTicks;
                                                                topCandidates[k].endPos = res.endPos;
                                                                topCandidates[k].endVel = res.endVel;
                                                                topCandidates[k].endHookPos = res.endHookPos;
                                                                topCandidates[k].endHookDir = res.endHookDir;
                                                                topCandidates[k].endHookState = res.endHookState;
                                                                topCandidates[k].endHookTick = res.endHookTick;
                                                                topCandidates[k].endJumped = res.endJumped;
                                                                topCandidates[k].endFreezeTime = res.endFreezeTime;
                                                                break;
                                                        }
                                                }
                                        }
                                }
                                else if(hookOn == 1 && hookActive)
                                {
                                        // Hold existing hook (no angle sweep needed).
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 1;
                                                vec2 aimRel = pBot->GetCore().m_HookPos - pBot->m_Pos;
                                                inp.m_TargetX = (int)aimRel.x;
                                                inp.m_TargetY = (int)aimRel.y;
                                                inp.m_Fire = 0;
                                                inp.m_WantedWeapon = 0;
                                                inp.m_PlayerFlags = 0;
                                                inp.m_NextWeapon = 0;
                                                inp.m_PrevWeapon = 0;
                                                inSeq.push_back(inp);
                                        }

                                        CCharacterCore savedCore = pBot->GetCore();
                                        vec2 savedPos = pBot->m_Pos;
                                        int savedFreeze = pBot->m_FreezeTime;

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                                        pBot->SetCore(savedCore);
                                        pBot->m_Pos = savedPos;
                                        pBot->m_FreezeTime = savedFreeze;

                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                                        for(int k = 0; k < N; k++)
                                        {
                                                if(score > topCandidates[k].score)
                                                {
                                                        for(int m = N - 1; m > k; m--)
                                                                topCandidates[m] = topCandidates[m - 1];
                                                        topCandidates[k].inputs = res.inputs;
                                                        topCandidates[k].traj = res.traj;
                                                        topCandidates[k].score = score;
                                                        topCandidates[k].reachedFinish = res.reachedFinish;
                                                        topCandidates[k].died = res.died;
                                                        topCandidates[k].freezeTicks = res.freezeTicks;
                                                        topCandidates[k].endPos = res.endPos;
                                                        topCandidates[k].endVel = res.endVel;
                                                        topCandidates[k].endHookPos = res.endHookPos;
                                                        topCandidates[k].endHookDir = res.endHookDir;
                                                        topCandidates[k].endHookState = res.endHookState;
                                                        topCandidates[k].endHookTick = res.endHookTick;
                                                        topCandidates[k].endJumped = res.endJumped;
                                                        topCandidates[k].endFreezeTime = res.endFreezeTime;
                                                        break;
                                                }
                                        }
                                }
                                else // hookOn == 0: no hook
                                {
                                        std::vector<CNetObj_PlayerInput> inSeq;
                                        inSeq.reserve(ChunkSize);
                                        for(int t = 0; t < ChunkSize; t++)
                                        {
                                                CNetObj_PlayerInput inp;
                                                mem_zero(&inp, sizeof(inp));
                                                inp.m_Direction = dir;
                                                inp.m_Jump = (t == jumpTick) ? 1 : 0;
                                                inp.m_Hook = 0;
                                                inp.m_TargetX = dir * 256;
                                                inp.m_TargetY = -256;
                                                inp.m_Fire = 0;
                                                inp.m_WantedWeapon = 0;
                                                inp.m_PlayerFlags = 0;
                                                inp.m_NextWeapon = 0;
                                                inp.m_PrevWeapon = 0;
                                                inSeq.push_back(inp);
                                        }

                                        CCharacterCore savedCore = pBot->GetCore();
                                        vec2 savedPos = pBot->m_Pos;
                                        int savedFreeze = pBot->m_FreezeTime;

                                        PfChunkResult res;
                                        PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res);

                                        pBot->SetCore(savedCore);
                                        pBot->m_Pos = savedPos;
                                        pBot->m_FreezeTime = savedFreeze;

                                        if(res.died && !res.reachedFinish)
                                                continue;

                                        float score = PfScoreChunk(m_PfCurPos, res.endPos, res.reachedFinish, res.died, res.freezeTicks);

                                        for(int k = 0; k < N; k++)
                                        {
                                                if(score > topCandidates[k].score)
                                                {
                                                        for(int m = N - 1; m > k; m--)
                                                                topCandidates[m] = topCandidates[m - 1];
                                                        topCandidates[k].inputs = res.inputs;
                                                        topCandidates[k].traj = res.traj;
                                                        topCandidates[k].score = score;
                                                        topCandidates[k].reachedFinish = res.reachedFinish;
                                                        topCandidates[k].died = res.died;
                                                        topCandidates[k].freezeTicks = res.freezeTicks;
                                                        topCandidates[k].endPos = res.endPos;
                                                        topCandidates[k].endVel = res.endVel;
                                                        topCandidates[k].endHookPos = res.endHookPos;
                                                        topCandidates[k].endHookDir = res.endHookDir;
                                                        topCandidates[k].endHookState = res.endHookState;
                                                        topCandidates[k].endHookTick = res.endHookTick;
                                                        topCandidates[k].endJumped = res.endJumped;
                                                        topCandidates[k].endFreezeTime = res.endFreezeTime;
                                                        break;
                                                }
                                        }
                                }
                        }
                }
        }

        if(topCandidates[0].score <= -1e17f)
                return false; // no valid candidates

        // Save backtrack entry.
        if(m_PfBacktrackIdx < PF_BACKTRACK_DEPTH)
        {
                PfBacktrackEntry &entry = m_PfBacktrack[m_PfBacktrackIdx];
                entry.pos = m_PfCurPos;
                entry.vel = m_PfCurVel;
                entry.hookPos = m_PfCurHookPos;
                entry.hookDir = m_PfCurHookDir;
                entry.hookState = m_PfCurHookState;
                entry.hookTick = m_PfCurHookTick;
                entry.jumped = m_PfCurJumped;
                entry.freezeTime = m_PfCurFreezeTime;
                entry.totalFreezeTicks = m_PfTotalFreezeTicks;
                entry.vPathSize = (int)m_PfVPath.size();
                entry.chunkCount = m_PfChunkCount;
                entry.numCandidates = 0;
                entry.usedCandidateIdx = 0;

                // Reserve exact capacity for this entry's candidates (avoids
                // reallocation as we push_back). Vector is cleared on Reset()
                // so no leak across runs.
                entry.candidates.clear();
                entry.candidates.reserve(N);
                for(int k = 0; k < N; k++)
                {
                        if(topCandidates[k].score <= -1e17f)
                                break;
                        PfBacktrackEntry::Candidate c;
                        c.inputs = topCandidates[k].inputs;
                        c.traj = topCandidates[k].traj;
                        c.score = topCandidates[k].score;
                        c.endPos = topCandidates[k].endPos;
                        c.endVel = topCandidates[k].endVel;
                        c.endHookPos = topCandidates[k].endHookPos;
                        c.endHookDir = topCandidates[k].endHookDir;
                        c.endHookState = topCandidates[k].endHookState;
                        c.endHookTick = topCandidates[k].endHookTick;
                        c.endJumped = topCandidates[k].endJumped;
                        c.endFreezeTime = topCandidates[k].endFreezeTime;
                        c.freezeTicks = topCandidates[k].freezeTicks;
                        entry.candidates.push_back(std::move(c));
                        entry.numCandidates++;
                }
                m_PfBacktrackIdx++;
        }

        // Apply best candidate.
        CandidateResult &best = topCandidates[0];
        outInputs = best.inputs;
        outTraj = best.traj;
        outReachedFinish = best.reachedFinish;
        outDied = best.died;
        outFreezeTicks = best.freezeTicks;

        m_PfCurPos = best.endPos;
        m_PfCurVel = best.endVel;
        m_PfCurHookPos = best.endHookPos;
        m_PfCurHookDir = best.endHookDir;
        m_PfCurHookState = best.endHookState;
        m_PfCurHookTick = best.endHookTick;
        m_PfCurJumped = best.endJumped;
        m_PfCurFreezeTime = best.endFreezeTime;

        return true;
}

void CBotNet::ComputeKinodynamic(int Dummy, const vec2 &MyPos, int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
        // Preconditions: m_pfDist must be computed by ComputePathfinder already
        if(!m_MapGridLoaded || !m_pfDist)
                return;
        if(botTX < 0 || botTY < 0 || botTX >= m_MapWidth || botTY >= m_MapHeight)
                return;

        CGameClient *pGame = GameClient();

        int maxNodes = pf_clamp(m_KinoCandidates, 100, LATTICE_MAX_NODES);
        int maxDepth = pf_clamp(m_KinoTicks, 3, LATTICE_MAX_DEPTH);
        int hookAngles = pf_clamp(m_KinoHookAngles, 4, 32);

        // Score the start position using the pathfinder distance field
        int startIdx = botTY * m_MapWidth + botTX;
        float startDist = m_pfDist[startIdx];
        if(startDist >= 1e17f)
                return; // bot is on an unreachable tile

        // Find the dummy character ID in the predicted world
        int PredictedCharId = (Dummy == g_Config.m_ClDummy) ? pGame->m_Snap.m_LocalClientId : Dummy;
        if(!pGame->m_PredictedWorld.GetCharacterById(PredictedCharId))
                return;

        // === Build the motion-primitive table ===
        SMotionPrimitive aPrims[LATTICE_MAX_PRIMITIVES];
        int numPrims = 0;
        BuildPrimitiveTable(hookAngles, aPrims, numPrims);

        // === Set up a clean planning world (bot + collision only) ===
        // Removing all non-character entities makes the world effectively
        // stateless: only the bot changes between simulations, so save/restore
        // of the bot's CCharacterCore is sufficient for consistent A* expansion.
        // (One CopyWorld instead of one-per-candidate like the old code.)
        CGameWorld FutureWorld;
        FutureWorld.CopyWorld(&pGame->m_PredictedWorld);

        for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
        {
                if(Type == CGameWorld::ENTTYPE_CHARACTER)
                        continue;
                std::vector<CEntity *> vRemove;
                for(CEntity *pEnt = FutureWorld.FindLast(Type); pEnt; pEnt = pEnt->TypePrev())
                        vRemove.push_back(pEnt);
                for(CEntity *pEnt : vRemove)
                        FutureWorld.RemoveEntity(pEnt);
        }
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == PredictedCharId)
                        continue;
                if(CCharacter *pChar = FutureWorld.GetCharacterById(i))
                        FutureWorld.RemoveEntity(pChar);
        }

        CCharacter *pBot = FutureWorld.GetCharacterById(PredictedCharId);
        if(!pBot)
                return;

        int StartTick = FutureWorld.GameTick();

        // === A* search ===
        std::vector<SLatticeNode> vNodes;
        vNodes.reserve(maxNodes + numPrims + 16);
        std::unordered_map<uint64_t, float> bestG;

        // Root node
        {
                SLatticeNode root;
                root.Core = pBot->GetCore();
                root.G = 0.0f;
                root.H = startDist;
                root.F = root.H * LATTICE_H_SCALE;
                root.ParentIdx = -1;
                root.PrimIdx = -1;
                int velDir = VelDirBucket(root.Core.m_Vel.x);
                bool onGround = pBot->IsGrounded();
                bool hooked = (root.Core.m_HookState == HOOK_GRABBED);
                root.StateKey = MakeStateKey(botTX, botTY, velDir, onGround, hooked);
                root.Closed = false;
                root.AimRelX = 0.0f;
                root.AimRelY = 0.0f;
                root.AimRelW = 0.0f;
                vNodes.push_back(root);
                bestG[root.StateKey] = 0.0f;
        }

        // Hook length for raycasting.  Read from the bot's tuning so custom
        // tuning zones are respected.  Clamp to a sane range so a broken
        // tuning (e.g. 0 or 1e9) can't paralyse or explode the search.
        float hookLength = pBot->GetCore().m_Tuning.m_HookLength;
        if(!(hookLength > 1.0f) || !(hookLength < 10000.0f))
                hookLength = 900.0f; // DDNet default
        CCollision *pColl = FutureWorld.Collision();
        if(!pColl)
                return; // no collision — can't raycast, can't plan safely

        auto fnCmp = [&vNodes](int a, int b) { return vNodes[a].F > vNodes[b].F; };
        std::priority_queue<int, std::vector<int>, decltype(fnCmp)> open(fnCmp);
        open.push(0);

        int goalNodeIdx = -1;

        while(!open.empty() && (int)vNodes.size() < maxNodes)
        {
                int curIdx = open.top();
                open.pop();
                if(vNodes[curIdx].Closed)
                        continue;
                vNodes[curIdx].Closed = true;

                // Goal test: reached (or is at) the target tile
                if(vNodes[curIdx].H <= 0.5f)
                {
                        goalNodeIdx = curIdx;
                        break;
                }

                // Depth limit: count ancestors
                int depth = 0;
                for(int p = vNodes[curIdx].ParentIdx; p >= 0; p = vNodes[p].ParentIdx)
                        depth++;
                if(depth >= maxDepth)
                        continue;

                // Restore the bot to this node's exact state
                pBot->SetCore(vNodes[curIdx].Core);
                pBot->m_Pos = vNodes[curIdx].Core.m_Pos;

                // Expand: try every motion primitive
                for(int primI = 0; primI < numPrims; primI++)
                {
                        const SMotionPrimitive &prim = aPrims[primI];

                        // Precondition: ground-only primitives (jump)
                        if(prim.RequiresGround && !pBot->IsGrounded())
                                continue;

                        // Classify the primitive to apply the right preconditions.
                        // v1.40: HOOK_FIRE / HOOK_HOLD / HOOK_RELEASE are distinguished
                        // by sentinel HookAngle values (see BuildPrimitiveTable).
                        const bool isHookFire = IsHookFirePrim(prim);
                        const bool isHookHold = IsHookHoldPrim(prim);
                        const bool isHookRelease = IsHookReleasePrim(prim);

                        // Snapshot the parent state so we can restore between primitives
                        CCharacterCore savedCore = pBot->GetCore();
                        vec2 savedPos = pBot->m_Pos;

                        // --- Hook primitive preconditions ---
                        // HOOK_FIRE: only fire when the hook state machine is IDLE.
                        //   Raycast to find a REAL hookable surface ABOVE the bot;
                        //   skip if nothing hookable is in range.  This prevents
                        //   firing into the void and the v1.39 floor-grab exploit.
                        // HOOK_HOLD: only when m_HookState == HOOK_GRABBED (swing
                        //   continuation).  Aim is irrelevant once attached, but
                        //   we pass the current hook pos for visual consistency.
                        // HOOK_RELEASE: only when a hook is active (FLYING/GRABBED).
                        vec2 aimRel(0.0f, 0.0f);
                        bool haveAim = false;
                        if(isHookFire)
                        {
                                if(savedCore.m_HookState != HOOK_IDLE)
                                        continue;
                                vec2 grabPos;
                                if(!FindHookGrabPoint(pColl, savedPos, prim.HookAngle, hookLength, grabPos))
                                        continue; // no hookable surface above bot in range
                                aimRel = grabPos - savedPos;
                                haveAim = true;
                        }
                        else if(isHookHold)
                        {
                                if(savedCore.m_HookState != HOOK_GRABBED)
                                        continue;
                                // Aim at current hook attachment point so the
                                // cursor stays on the hook (visual only — the
                                // state machine controls the pull once attached).
                                aimRel = savedCore.m_HookPos - savedPos;
                                haveAim = true;
                        }
                        else if(isHookRelease)
                        {
                                if(savedCore.m_HookState != HOOK_FLYING && savedCore.m_HookState != HOOK_GRABBED)
                                        continue;
                        }

                        // Build the primitive's input array
                        CNetObj_PlayerInput aPrimInputs[LATTICE_PRIM_MAX_TICKS];
                        BuildPrimitiveInputs(prim, aPrimInputs, haveAim ? &aimRel : nullptr);

                        // Simulate the primitive
                        bool died = false;
                        int freezeCount = 0;
                        for(int t = 0; t < prim.Ticks; t++)
                        {
                                int Tick = StartTick + t + 1;
                                pBot->OnDirectInput(&aPrimInputs[t]);
                                FutureWorld.m_GameTick = Tick;
                                pBot->OnPredictedInput(&aPrimInputs[t]);
                                FutureWorld.Tick();
                                if(!FutureWorld.GetCharacterById(PredictedCharId))
                                {
                                        died = true;
                                        break;
                                }
                                vec2 p = pBot->m_Pos;
                                int ptx = pf_clamp((int)(p.x / 32.0f), 0, m_MapWidth - 1);
                                int pty = pf_clamp((int)(p.y / 32.0f), 0, m_MapHeight - 1);
                                if(IsTileFreeze(ptx, pty))
                                        freezeCount++;
                        }

                        // Capture child state BEFORE restoring (IsGrounded / m_FreezeTime
                        // reflect the post-simulation world)
                        bool childEndedFrozen = (!died && pBot->m_FreezeTime > 0);
                        CCharacterCore childCore;
                        vec2 childPos(0, 0);
                        bool childOnGround = false;
                        if(!died)
                        {
                                childCore = pBot->GetCore();
                                childPos = pBot->m_Pos;
                                childOnGround = pBot->IsGrounded();
                        }

                        // Restore the bot to the parent state for the next primitive
                        pBot->SetCore(savedCore);
                        pBot->m_Pos = savedPos;

                        if(died)
                                continue;
                        // Ending frozen = bot cannot move further from this state
                        if(childEndedFrozen)
                                continue;

                        // Discrete child state
                        int newTX = pf_clamp((int)(childPos.x / 32.0f), 0, m_MapWidth - 1);
                        int newTY = pf_clamp((int)(childPos.y / 32.0f), 0, m_MapHeight - 1);
                        int newIdx = newTY * m_MapWidth + newTX;
                        float newH = m_pfDist[newIdx];
                        if(newH >= 1e17f)
                                continue; // landed on an unreachable tile

                        int newVelDir = VelDirBucket(childCore.m_Vel.x);
                        bool newHooked = (childCore.m_HookState == HOOK_GRABBED);
                        uint64_t newKey = MakeStateKey(newTX, newTY, newVelDir, childOnGround, newHooked);

                        // Cost = ticks + freeze penalty + retract penalty.
                        // v1.40: add a 3-tick penalty for ending a primitive in
                        // HOOK_RETRACTED — this state means the hook was just
                        // released and is retracting, so the bot has lost momentum
                        // and cannot immediately re-hook.  Discourages wasted hooks.
                        float primCost = (float)prim.Ticks + (float)freezeCount * LATTICE_FREEZE_PENALTY;
                        if(childCore.m_HookState == HOOK_RETRACTED || childCore.m_HookState == HOOK_RETRACT_START)
                                primCost += 3.0f;
                        float newG = vNodes[curIdx].G + primCost;

                        // Skip if we already reached this state at a lower-or-equal cost
                        auto it = bestG.find(newKey);
                        if(it != bestG.end() && it->second <= newG)
                                continue;
                        bestG[newKey] = newG;

                        float newF = newG + newH * LATTICE_H_SCALE;

                        // Create the child node
                        SLatticeNode child;
                        child.Core = childCore;
                        child.G = newG;
                        child.H = newH;
                        child.F = newF;
                        child.ParentIdx = curIdx;
                        child.PrimIdx = primI;
                        child.StateKey = newKey;
                        child.Closed = false;
                        // Preserve the aim point used at expansion time so the
                        // plan-replay step can reproduce the exact same inputs.
                        child.AimRelX = haveAim ? aimRel.x : 0.0f;
                        child.AimRelY = haveAim ? aimRel.y : 0.0f;
                        child.AimRelW = haveAim ? 1.0f : 0.0f;
                        vNodes.push_back(child);
                        open.push((int)vNodes.size() - 1);
                }
        }

        // === Pick the result node ===
        // If the goal was reached, use it.  Otherwise pick the node with the
        // lowest H (closest to goal) as a best-effort partial plan.
        int resultNodeIdx;
        if(goalNodeIdx >= 0)
        {
                resultNodeIdx = goalNodeIdx;
        }
        else
        {
                // Find the non-root node with the lowest H (closest to goal
                // by the pathfinder distance-field gradient).  This is the
                // "best partial plan" — the input sequence that ended on the
                // tile whose distance-to-goal is smallest.
                float rootH = vNodes[0].H;
                float bestH = 1e17f;
                int bestIdx = -1;
                for(int i = 1; i < (int)vNodes.size(); i++)
                {
                        if(vNodes[i].H < bestH)
                        {
                                bestH = vNodes[i].H;
                                bestIdx = i;
                        }
                }

                if(bestIdx < 0)
                {
                        // No children were ever expanded — nothing to apply.
                        State.m_KinoCache.Reset();
                        return;
                }

                if(bestH < rootH)
                {
                        // Normal partial plan: at least one primitive moved
                        // the bot closer to the goal.
                        resultNodeIdx = bestIdx;
                }
                else if(m_KinoAggressive)
                {
                        // Aggressive mode: no primitive improved on the start,
                        // but the user wants us to force-apply the best partial
                        // plan anyway.  This keeps the bot on the kinodynamic
                        // planner (which knows how to fire hooks and chain
                        // maneuvers) instead of bailing out to the legacy flow
                        // pathfinder, whose only response to "goal is up" is
                        // to spam jump — which does nothing in mid-air and
                        // never fires the hook.  The bot may temporarily move
                        // away from the goal, but the next recompute starts
                        // from the new state and may find a real path.
                        resultNodeIdx = bestIdx;
                }
                else
                {
                        // Normal mode and no primitive made progress — bail
                        // out and let the normal pathfinder handle movement.
                        State.m_KinoCache.Reset();
                        return;
                }
        }

        // === Reconstruct the primitive path (root → result) ===
        // Walk parents to get the sequence of child node indices (each child
        // stores which primitive was used to reach it AND the aim point that
        // was raycast at expansion time).  We need the child indices (not just
        // primIdx) so we can replay with the exact same aim point.
        std::vector<int> vChildNodeIdx;
        for(int idx = resultNodeIdx; idx > 0; idx = vNodes[idx].ParentIdx)
                vChildNodeIdx.push_back(idx);
        std::reverse(vChildNodeIdx.begin(), vChildNodeIdx.end());

        if(vChildNodeIdx.empty())
        {
                State.m_KinoCache.Reset();
                return;
        }

        // === Flatten the primitive sequence into a single input array ===
        CNetObj_PlayerInput aPlanInputs[KINO_MAX_TICKS];
        int planTicks = 0;
        for(int childIdx : vChildNodeIdx)
        {
                const SLatticeNode &child = vNodes[childIdx];
                const SMotionPrimitive &prim = aPrims[child.PrimIdx];

                CNetObj_PlayerInput aPrimInputs[LATTICE_PRIM_MAX_TICKS];
                vec2 aimRel(child.AimRelX, child.AimRelY);
                BuildPrimitiveInputs(prim, aPrimInputs, (child.AimRelW > 0.5f) ? &aimRel : nullptr);
                for(int t = 0; t < prim.Ticks; t++)
                {
                        if(planTicks >= KINO_MAX_TICKS)
                                break;
                        aPlanInputs[planTicks++] = aPrimInputs[t];
                }
                if(planTicks >= KINO_MAX_TICKS)
                        break;
        }

        if(planTicks == 0)
        {
                State.m_KinoCache.Reset();
                return;
        }

        // === Re-simulate the full plan from the root to collect dense vPath
        // for rendering (and to validate the concatenated plan). ===
        pBot->SetCore(vNodes[0].Core);
        pBot->m_Pos = vNodes[0].Core.m_Pos;
        std::vector<vec2> vPath;
        vPath.reserve(planTicks + 1);
        vPath.push_back(pBot->m_Pos);
        for(int t = 0; t < planTicks; t++)
        {
                int Tick = StartTick + t + 1;
                pBot->OnDirectInput(&aPlanInputs[t]);
                FutureWorld.m_GameTick = Tick;
                pBot->OnPredictedInput(&aPlanInputs[t]);
                FutureWorld.Tick();
                if(!FutureWorld.GetCharacterById(PredictedCharId))
                        break;
                vPath.push_back(pBot->m_Pos);
        }

        // === Store the plan in the cache ===
        CKinodynamicCache &cache = State.m_KinoCache;
        cache.Reset();
        memcpy(cache.aInputs, aPlanInputs, sizeof(CNetObj_PlayerInput) * planTicks);
        cache.NumTicks = planTicks;
        cache.CurrentTick = 0;
        cache.ComputeTick = pGame->Client()->GameTick(g_Config.m_ClDummy);
        cache.TargetTX = targetTX;
        cache.TargetTY = targetTY;
        cache.BotPos = MyPos;
        cache.vPath = vPath;
}

void CBotNet::ApplyKinodynamicCache(int Dummy, CNetObj_PlayerInput *pInput, CBotNetDummy &State)
{
        CKinodynamicCache &cache = State.m_KinoCache;
        if(!cache.IsValid())
                return;

        CGameClient *pGame = GameClient();

        if(cache.CurrentTick < cache.NumTicks)
        {
                CNetObj_PlayerInput &src = cache.aInputs[cache.CurrentTick];

                // Apply direction
                if(Dummy == g_Config.m_ClDummy)
                {
                        pGame->m_Controls.m_aInputDirectionLeft[Dummy] = (src.m_Direction < 0) ? 1 : 0;
                        pGame->m_Controls.m_aInputDirectionRight[Dummy] = (src.m_Direction > 0) ? 1 : 0;
                }
                else
                {
                        pInput->m_Direction = src.m_Direction;
                }

                // Apply jump
                pInput->m_Jump = src.m_Jump;

                // Apply hook and aim
                pInput->m_Hook = src.m_Hook;
                if(src.m_Hook || src.m_TargetX != 0 || src.m_TargetY != 0)
                {
                        SetMousePos(pGame, Dummy, vec2((float)src.m_TargetX, (float)src.m_TargetY));
                }

                pInput->m_PlayerFlags |= 1;
                cache.CurrentTick++;
        }
        else
        {
                // Cache exhausted, mark for recomputation
                // IMPORTANT: Clear hook/jump to prevent them leaking and getting stuck
                pInput->m_Hook = 0;
                pInput->m_Jump = 0;
                cache.Reset();
        }
}

bool CBotNet::IsKinodynamicCacheValid(int targetTX, int targetTY, const vec2 &MyPos) const
{
        // This is called on the State's cache - but since it's per-dummy,
        // the caller should check State.m_KinoCache directly
        return false; // placeholder, actual logic is inline in ProcessDummy
}

// =========================================================
// KINODYNAMIC A* — RENDERING
// =========================================================

void CBotNet::RenderPackage(int Dummy)
{
        if(!m_KinodynamicEnabled)
                return;
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;

        const CKinodynamicCache &cache = m_aDummies[Dummy].m_KinoCache;
        if(!cache.IsValid() || cache.vPath.size() < 2)
                return;

        // Render kinodynamic predicted path — use fixed alpha, independent of trajectory settings
        Graphics()->TextureClear();

        const ColorRGBA KinoColor = ColorRGBA(0.2f, 1.0f, 0.4f, 1.0f);
        const float PathAlpha = 0.8f;
        const float HalfWidth = 1.5f;

        {
                std::vector<IGraphics::CFreeformItem> vQuads;
                vQuads.reserve(cache.vPath.size() - 1);

                for(size_t i = 1; i < cache.vPath.size(); i++)
                {
                        vec2 p0 = cache.vPath[i - 1];
                        vec2 p1 = cache.vPath[i];
                        vec2 Dir = normalize(p1 - p0);
                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;

                        vQuads.emplace_back(
                                p0.x - Perp.x, p0.y - Perp.y,
                                p0.x + Perp.x, p0.y + Perp.y,
                                p1.x - Perp.x, p1.y - Perp.y,
                                p1.x + Perp.x, p1.y + Perp.y);
                }

                Graphics()->QuadsBegin();
                for(size_t i = 0; i < vQuads.size(); i++)
                {
                        float t = (float)(i + 1) / (float)cache.vPath.size();
                        float Alpha = PathAlpha * (1.0f - t * 0.7f);
                        Graphics()->SetColor(KinoColor.r, KinoColor.g, KinoColor.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
                }
                Graphics()->QuadsEnd();
        }
}

void CBotNet::RenderVectorField()
{
        if(!m_KinodynamicEnabled || !m_PathfinderEnabled || !m_MapGridLoaded)
                return;
        if(!m_pfDist)
                return;

        CGameClient *pGame = GameClient();

        // Get camera position for viewport culling
        float CamX = pGame->m_Camera.m_Center.x;
        float CamY = pGame->m_Camera.m_Center.y;
        float CamW = pGame->m_Camera.m_Zoom * Graphics()->ScreenWidth() / 2.0f;
        float CamH = pGame->m_Camera.m_Zoom * Graphics()->ScreenHeight() / 2.0f;

        int minTX = pf_max(0, (int)((CamX - CamW) / 32.0f) - 1);
        int maxTX = pf_min(m_MapWidth - 1, (int)((CamX + CamW) / 32.0f) + 1);
        int minTY = pf_max(0, (int)((CamY - CamH) / 32.0f) - 1);
        int maxTY = pf_min(m_MapHeight - 1, (int)((CamY + CamH) / 32.0f) + 1);

        // Sample every 2 tiles for performance
        int step = 2;

        Graphics()->TextureClear();
        Graphics()->LinesBegin();

        for(int ty = minTY; ty <= maxTY; ty += step)
        {
                for(int tx = minTX; tx <= maxTX; tx += step)
                {
                        int idx = ty * m_MapWidth + tx;
                        if(m_pfDist[idx] >= 1e17f)
                                continue;
                        if(!IsTileWalkable(tx, ty))
                                continue;

                        float currentD = m_pfDist[idx];

                        // Find the neighbor with the smallest distance (gradient direction)
                        float bestDx = 0, bestDy = 0;
                        float bestDist = currentD;
                        int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
                        int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

                        for(int d = 0; d < 8; d++)
                        {
                                int nr = ty + dr[d];
                                int nc = tx + dc[d];
                                if(nr < 0 || nc < 0 || nr >= m_MapHeight || nc >= m_MapWidth)
                                        continue;
                                int nIdx = nr * m_MapWidth + nc;
                                if(m_pfDist[nIdx] < bestDist)
                                {
                                        bestDist = m_pfDist[nIdx];
                                        bestDx = (float)(nc - tx);
                                        bestDy = (float)(nr - ty);
                                }
                        }

                        if(bestDx == 0 && bestDy == 0)
                                continue;

                        float len = sqrtf(bestDx * bestDx + bestDy * bestDy);
                        if(len < 0.001f)
                                continue;

                        bestDx /= len;
                        bestDy /= len;

                        float cx = tx * 32.0f + 16.0f;
                        float cy = ty * 32.0f + 16.0f;
                        float arrowLen = 12.0f;

                        vec2 start(cx, cy);
                        vec2 end(cx + bestDx * arrowLen, cy + bestDy * arrowLen);

                        // Color based on distance: close to target = green, far = yellow
                        float maxVisDist = 100.0f;
                        float ratio = pf_min(currentD / maxVisDist, 1.0f);
                        Graphics()->SetColor(1.0f, 1.0f - ratio * 0.5f, 0.0f, 0.4f);
                        IGraphics::CLineItem Line(start, end);
                        Graphics()->LinesDraw(&Line, 1);
                }
        }

        Graphics()->LinesEnd();
}
