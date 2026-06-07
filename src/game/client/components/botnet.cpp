#include "botnet.h"

#include <game/client/components/bot_control.h>


#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/dbg.h>

#include <engine/client.h>
#include <engine/map.h>

#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/mapitems.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <queue>
#include <sstream>

// --- Local helpers (DDNet max/min/clamp may not be in scope) ---
static inline int pf_max(int a, int b) { return a > b ? a : b; }
static inline int pf_min(int a, int b) { return a < b ? a : b; }
static inline int pf_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float pf_maxf(float a, float b) { return a > b ? a : b; }
static inline float pf_minf(float a, float b) { return a < b ? a : b; }

// --- Pathfinder heap node ---
struct PfNode
{
        float f; // g + h
        float g; // cost from start
        int r, c; // tile coords
        bool operator>(const PfNode &o) const { return f > o.f; }
};

CBotNet::CBotNet()
{
        m_RandomAim = m_CopyMoves = m_AttackEnabled = false;
        m_MainID = -1;
        m_RandomAimInterval = 100;
        m_NextRandomAimTick = 0;
        m_LastTargetAttackTick = -1;
        m_AllTarget = false;
        m_AutoAim = m_AutoFire = m_MoveEnabled = true;
        m_AutoHook = m_StandEnabled = m_RescueFrozen = m_RescueAll = m_KillOnFreeze = m_AttackMain = m_AutoHammer = m_SmartDetect = m_SmartRescue = m_AvoidFreeze = false;
        m_FireDist = 80.0f;
        m_HookDist = 400.0f;
        m_RescueRadius = 500.0f;
        m_TargetDist = 300.0f;
        m_MainDist = INFINITY;
        m_StandDist = 64.0f;
        m_HookDelay = 1000;
        m_HookTickTimer = 0;
        m_JumpTicks = 0;
        m_ClientDelay = 0;
        m_StandOnXOnly = false;
        for(int i = 0; i < 128; i++)
        {
                m_TargetList[i] = false;
                m_BotsList[i] = false;
                m_RescueList[i] = false;
        }

        m_MacroRecording = false;
        m_MacroPlaying = false;
        m_MacroCaptureID = -1;
        m_MacroPlayIndex = 0;
        m_MacroSleepTicks = 0;
        m_LastMacroRecordTick = 0;
        m_LastRecordedDir = 0;
        m_LastRecordedJump = 0;
        m_LastRecordedHook = 0;
        m_LastRecordedFire = 0;
        m_LastRecordedAimX = 0;
        m_LastRecordedAimY = 0;
        m_LastRecordedWeapon = -1;

        // Pathfinder init
        m_PathfinderEnabled = false;
        m_PfSimulatePlayers = false;
        m_PfSimulateMethod = 0; // 0 = walls, 1 = score
        m_PfSimulateScore = 25.0f;
        m_PfRays = 24;
        m_PfViewRadius = 6;
        m_PfSnap = false;
        m_PfHookEnabled = false;
        m_PfHookTile = vec2(0, 0);
        m_MapWidth = 0;
        m_MapHeight = 0;
        m_pMapGrid = nullptr;
        m_pFrontGrid = nullptr;
        m_PfPlayerPenalty = nullptr;
        m_MapGridLoaded = false;
        m_pfDist = nullptr;
        m_pfVisited = nullptr;
        m_FlowDir = vec2(0, 0);
        m_FlowTargetTX = -1;
        m_FlowTargetTY = -1;
        m_LastTargetTX = -1;
        m_LastTargetTY = -1;
        m_LastBotTX = -1;
        m_LastBotTY = -1;
        m_PathFound = false;
        m_LastPos = vec2(0, 0);
        m_StuckTicks = 0;
        m_PathfinderGoActive = false;
        m_PathfinderGoPos = vec2(0, 0);
        m_TargetDummy = 1; // default: control the inactive dummy
}

CBotNet::~CBotNet()
{
        if(m_pMapGrid) delete[] m_pMapGrid;
        if(m_pFrontGrid) delete[] m_pFrontGrid;
        if(m_pfDist) delete[] m_pfDist;
        if(m_pfVisited) delete[] m_pfVisited;
        if(m_PfPlayerPenalty) delete[] m_PfPlayerPenalty;
}

void CBotNet::OnConsoleInit()
{
        
        Console()->Register("c_random_aim", "i[on] ?i[ms]", CFGFLAG_CLIENT, ConRandomAim, this, "Random Aim");
        Console()->Register("c_copy_moves", "i[target_id]", CFGFLAG_CLIENT, ConCopyMoves, this, "Copy Moves");
        Console()->Register("c_attack", "i[on]", CFGFLAG_CLIENT, ConAttackEnable, this, "Attack Mode");
        Console()->Register("c_main", "i[id]", CFGFLAG_CLIENT, ConSetMain, this, "Main ID");
        Console()->Register("c_targets", "s[ids]", CFGFLAG_CLIENT, ConSetTargets, this, "Targets");
        Console()->Register("c_bots", "s[ids]", CFGFLAG_CLIENT, ConSetBots, this, "Bots (allies)");
        Console()->Register("c_target_all", "i[on]", CFGFLAG_CLIENT, ConSetTargetAll, this, "Attack all (targets become blacklist)");
        Console()->Register("c_atk_set", "iiiiiiiiiiiiiii", CFGFLAG_CLIENT, ConAttackSettings, this, "Settings (15 params: aim,fire,hook,move,stand,rescue,rescueAll,smartDetect,smartRescue,killFrz,atkMain,hammer,simPlayers,avoidFreeze,pfHook)");
        Console()->Register("c_atk_dists", "ffffff", CFGFLAG_CLIENT, ConAttackDists, this, "Radii (fire, hook, rescue, target, main, stand)");
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
        Console()->Register("c_target_dummy", "i[dummy]", CFGFLAG_CLIENT, ConTargetDummy, this, "Set target dummy index (0=main, 1=first dummy, etc.)");
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

        // Применяем штраф за игроков (работает и для стен, и для пуша)
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

void CBotNet::ComputePathfinder(int botTX, int botTY, int targetTX, int targetTY)
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
                m_PathFound = false;
                m_FlowDir = vec2(0, 0);
                m_PfHookTile = vec2(0, 0);
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

        m_PathFound = m_pfVisited[botTY * m_MapWidth + botTX];
        m_LastTargetTX = targetTX;
        m_LastTargetTY = targetTY;
        m_LastBotTX = botTX;
        m_LastBotTY = botTY;

        if(m_PathFound)
        {
                ComputeFlowForTile(botTY, botTX);
        }
        else
        {
                m_FlowDir = vec2(0, 0);
                m_PfHookTile = vec2(0, 0);
        }
}

// =========================================================
// PATHFINDER RESCUE — вариант для спасения: избегаем фриз
// Ищем лучший тайл в HOOK радиусе от цели, который не во фризе
// =========================================================
void CBotNet::ComputePathfinderRescue(int botTX, int botTY, int targetTX, int targetTY)
{
        if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
                return;

        targetTX = pf_clamp(targetTX, 0, m_MapWidth - 1);
        targetTY = pf_clamp(targetTY, 0, m_MapHeight - 1);
        botTX = pf_clamp(botTX, 0, m_MapWidth - 1);
        botTY = pf_clamp(botTY, 0, m_MapHeight - 1);

        if(!IsTileWalkable(botTX, botTY))
        {
                m_PathFound = false;
                m_FlowDir = vec2(0, 0);
                return;
        }

        int hookRadiusTiles = (int)(m_HookDist / 32.0f);
        if(hookRadiusTiles < 1)
                hookRadiusTiles = 1;

        // Рейкастинг от цели — N лучей
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
                m_PathFound = false;
                m_FlowDir = vec2(0, 0);
                return;
        }

        // Сортируем по дистанции (ближайшие первыми)
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

        // Перебираем пачками по дистанции
        int startIdx = 0;
        while(startIdx < numSafe)
        {
                int curDist = safeTiles[startIdx].dist;

                // Сбрасываем массивы
                for(int i = 0; i < Size; i++)
                {
                        m_pfDist[i] = 1e18f;
                        m_pfVisited[i] = false;
                }

                std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open;

                // Добавляем ВСЕ тайлы с одинаковой дистанцией пачкой
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

                // A*
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

                // Путь найден?
                if(m_pfVisited[botTY * m_MapWidth + botTX])
                {
                        m_PathFound = true;
                        m_LastTargetTX = targetTX;
                        m_LastTargetTY = targetTY;
                        m_LastBotTX = botTX;
                        m_LastBotTY = botTY;
                        ComputeFlowForTile(botTY, botTX);
                        return;
                }

                // Не нашли — пробуем следующую пачку
                startIdx = endIdx;
        }

        // Ни до одного безопасного тайла нет пути
        m_PathFound = false;
        m_FlowDir = vec2(0, 0);
}

void CBotNet::ComputeFlowForTile(int r, int c)
{
        m_PfHookTile = vec2(0, 0); // Сброс хук-цели

        if(!m_MapGridLoaded)
                return;
        if(r < 0 || c < 0 || r >= m_MapHeight || c >= m_MapWidth)
                return;

        int idx = r * m_MapWidth + c;
        if(m_pfDist[idx] >= 1e17f)
        {
                m_FlowDir = vec2(0, 0);
                return;
        }

        float currentD = m_pfDist[idx];
        float bestDist = currentD;
        float bestDX = 0, bestDY = 0;

        // === Шаг 1: Лучи A* — ищем направление к цели с учётом штрафов ===
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

                        // Симуляция игроков: стены ломают луч, пуш добавляет штраф
                        if(m_PfSimulatePlayers && m_PfPlayerPenalty && m_PfPlayerPenalty[fIdx] > 0)
                        {
                                if(m_PfSimulateMethod == 0)
                                        break;
                                else
                                        rayFreezePenalty += m_PfPlayerPenalty[fIdx];
                        }

                        // Штраф за freeze-тайлы вдоль луча
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

        // === Шаг 2: Freeze repulsion — отталкивание от ближайших freeze-тайлов ===
        // Если рядом есть freeze, добавляем направление «от freeze» как кандидата.
        // repelLen отражает количество/близость freeze: чем больше freeze вокруг —
        // тем сильнее отталкивание, тем вероятнее что оно победит обычные лучи.
        if(m_AvoidFreeze)
        {
                vec2 repel = ComputeFreezeRepel(c, r); // c = botTX, r = botTY
                float repelLen = sqrtf(repel.x * repel.x + repel.y * repel.y);
                if(repelLen > 0.001f)
                {
                        // Нормализуем
                        float normX = repel.x / repelLen;
                        float normY = repel.y / repelLen;

                        // «Виртуальное расстояние» — чем сильнее repel, тем «ближе» это направление.
                        // repelWeight доминирует когда рядом много freeze (repelLen большой).
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

        // === Шаг 3: Pathfinder Hook — поиск лучшего хукабельного блока ===
        // Ищем TILE_SOLID в направлении движения (погрешность 0.3), ближайший, без игроков на пути.
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

                                // Критерий 1: луч в направлении движения (погрешность 0.3)
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
                                                break; // игрок на пути — хук невозможен

                                        unsigned char tile = m_pMapGrid[hIdx];

                                        if(tile == TILE_SOLID)
                                        {
                                                if(step < bestStep)
                                                {
                                                        bestStep = step;
                                                        m_PfHookTile = vec2(tc * 32.0f + 16.0f, tr * 32.0f + 16.0f);
                                                }
                                                break;
                                        }

                                        if(!IsTileWalkable(tc, tr))
                                                break; // другой непроходимый тайл (nohook, death)
                                }
                        }
                }
        }

        float len = sqrtf(bestDX * bestDX + bestDY * bestDY);
        if(len > 0.001f)
        {
                m_FlowDir = vec2(bestDX / len, bestDY / len);
        }
        else
        {
                m_FlowDir = vec2(0, 0);
        }
}

vec2 CBotNet::ComputeFreezeRepel(int botTX, int botTY)
{
        // Пускаем m_PfRays лучей от бота, ищем freeze-тайлы.
        // Каждый найденный freeze добавляет вектор отталкивания ОТ freeze К боту.
        // Сила обратно пропорциональна расстоянию: repel += (-dir) / dist^2
        vec2 repel = vec2(0, 0);
        const int scanRadius = 2; // тайлы от бота

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

                                // repel += (направление от freeze к боту) / dist^2
                                repel.x += -dx / distSq;
                                repel.y += -dy / distSq;
                        }
                }
        }

        return repel;
}

// ApplyFreezeAvoidance удалена — логика встроена в ComputeFlowForTile.
// Freeze repulsion теперь участвует в конкурсе направлений наравне с лучами A*,
// что устраняет конфликт двух механизмов и гарантирует работу даже при stand.

void CBotNet::UpdatePlayerPenalty(int botTX, int botTY, int excludeTX, int excludeTY, int LocalID)
{
        if(!m_PfPlayerPenalty)
                return;

        mem_zero(m_PfPlayerPenalty, m_MapWidth * m_MapHeight * sizeof(float));

        // Заполняем штраф за игроков:
        // - Если SPS включён: используется и для A*, и для хука
        // - Если SPS выключён: только для хука (игроки = блок для хук-рейкаста)
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
                // Для хука: не исключаем никого — даже таргет/мейна нужно считать блоком,
                // иначе рейкаст найдёт солид за игроком, а реальный хук цепляет игрока.
                // Для A* (SPS): исключаем как раньше — таргет не стена для пути.
                int idx = pty * m_MapWidth + ptx;
                if(m_pMapGrid[idx] == TILE_SOLID || m_pMapGrid[idx] == TILE_DEATH)
                        continue;

                bool isTarget = (ptx == excludeTX && pty == excludeTY);

                // A*: таргет исключается (не стена для пути)
                if(m_PfSimulatePlayers && !isTarget)
                {
                        if(m_PfSimulateMethod == 1)
                                m_PfPlayerPenalty[idx] += m_PfSimulateScore;
                        else
                                m_PfPlayerPenalty[idx] += PF_PLAYER_COST;
                }

                // Хук: ВСЕ игроки = блок, включая таргет
                if(m_PfHookEnabled)
                        m_PfPlayerPenalty[idx] += 1.0f;
        }
}

void CBotNet::GetMovementFromFlow(bool &outLeft, bool &outRight, bool &outJump)
{
        outLeft = false;
        outRight = false;
        outJump = false;

        if(m_FlowDir.x == 0.0f && m_FlowDir.y == 0.0f)
                return;

        float angle = atan2f(m_FlowDir.y, m_FlowDir.x);
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
// INPUT HELPERS — route to correct buffer for active/inactive dummy
// =========================================================

CNetObj_PlayerInput &CBotNet::InputData()
{
        CGameClient *pGame = GameClient();
        int D = m_TargetDummy;
        if(D == g_Config.m_ClDummy)
                return pGame->m_Controls.m_aInputData[D];
        else
                return pGame->m_aDummyInput[D];
}

int &CBotNet::InputDirLeft()
{
        CGameClient *pGame = GameClient();
        int D = m_TargetDummy;
        if(D == g_Config.m_ClDummy)
                return pGame->m_Controls.m_aInputDirectionLeft[D];
        else
        {
                // For inactive dummies, direction is stored in m_aDummyInput[D].m_Direction
                // We use a static temp and sync on write — see SetDirection() below
                static int s_DirLeft = 0;
                return s_DirLeft;
        }
}

int &CBotNet::InputDirRight()
{
        static int s_DirRight = 0;
        return s_DirRight;
}

vec2 &CBotNet::InputMousePos()
{
        CGameClient *pGame = GameClient();
        int D = m_TargetDummy;
        return pGame->m_Controls.m_aMousePos[D];
}

// Helper to set direction for the target dummy
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

// Helper to set mouse pos for the target dummy
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
// ON TICK
// =========================================================

void CBotNet::OnUpdate()
{
                if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        CGameClient *pGame = GameClient();
        if(!pGame->m_Snap.m_pLocalInfo)
                return;

        int Dummy = m_TargetDummy;
        int LocalID = pGame->m_aLocalIds[Dummy];
        if(LocalID < 0 || LocalID >= 64)
                return;
        if(Dummy != 0 && !Client()->DummyConnected(Dummy))
                return;
        if(!pGame->m_aClients[LocalID].m_Active)
                return;

        auto &Controls = pGame->m_Controls;
        CNetObj_PlayerInput *pInput = (Dummy == g_Config.m_ClDummy) ? &Controls.m_aInputData[Dummy] : &pGame->m_aDummyInput[Dummy];
        int64_t CurTick = Client()->GameTick(0);

        if(m_MacroPlaying)
        {
                if(m_MacroSleepTicks > 0)
                {
                        int64_t Now = Client()->GameTick(0);
                        if(Now >= m_MacroSleepUntilTick)
                                m_MacroSleepTicks = 0;
                        else
                                return;
                }

                if(m_MacroPlayIndex < (int)m_MacroPlayLines.size())
                {
                        const std::string &line = m_MacroPlayLines[m_MacroPlayIndex];
                        m_MacroPlayIndex++;

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
                                        m_MacroSleepTicks = 1;
                                        m_MacroSleepUntilTick = Client()->GameTick(0) + (int64_t)ms * TickSpeed / 1000;
                                }
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
                        m_MacroPlaying = false;
                        SetDirection(pGame, Dummy, false, false);
                        pInput->m_Jump = 0;
                        pInput->m_Hook = 0;
                        pInput->m_Fire = 0;
                        GameClient()->m_BotControl.ActionStop(Dummy);
                        dbg_msg("botnet_macro", "Playback finished.");
                }

                pInput->m_PlayerFlags |= 1;
                if(Dummy != g_Config.m_ClDummy)
                        Controls.m_aInputData[Dummy] = *pInput;
                return;
        }

        if(m_MacroRecording)
        {
                if(m_MacroRecordBuffer.size() > 100000)
                {
                        dbg_msg("botnet_macro", "EMERGENCY STOP: Buffer exceeded 100,000 lines! Stopping recording to prevent crash.");
                        m_MacroRecording = false;
                        return;
                }

                int captureID = (m_MacroCaptureID >= 0 && m_MacroCaptureID < 128) ? m_MacroCaptureID : LocalID;

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
                                return;
                        }
                }

                bool changed = false;
                if(curDir != m_LastRecordedDir)
                        changed = true;
                if(curJump != m_LastRecordedJump)
                        changed = true;
                if(curHook != m_LastRecordedHook)
                        changed = true;
                if(curAimX != m_LastRecordedAimX || curAimY != m_LastRecordedAimY)
                        changed = true;
                if(curFire != m_LastRecordedFire)
                        changed = true;
                if(curWeapon != m_LastRecordedWeapon)
                        changed = true;

                if(changed)
                {
                        int64_t deltaTick = CurTick - m_LastMacroRecordTick;
                        int TickSpeed = Client()->GameTickSpeed();
                        if(TickSpeed <= 0)
                                TickSpeed = 50;
                        int deltaMs = (int)(deltaTick * 1000 / TickSpeed);

                        if(deltaMs > 1 && !m_MacroRecordBuffer.empty())
                        {
                                std::string sleepLine = "sleep " + std::to_string(deltaMs);
                                m_MacroRecordBuffer.push_back(sleepLine);
                        }

                        if(curDir != m_LastRecordedDir)
                        {
                                if(curDir == -1)
                                        m_MacroRecordBuffer.push_back("input left 1");
                                else if(curDir == 1)
                                        m_MacroRecordBuffer.push_back("input right 1");
                                else
                                        m_MacroRecordBuffer.push_back("input left 0");
                        }
                        if(curJump != m_LastRecordedJump)
                        {
                                if(curJump)
                                        m_MacroRecordBuffer.push_back("input jump 1");
                                else
                                        m_MacroRecordBuffer.push_back("input jump 0");
                        }
                        if(curHook != m_LastRecordedHook)
                        {
                                if(curHook)
                                        m_MacroRecordBuffer.push_back("input hook 1");
                                else
                                        m_MacroRecordBuffer.push_back("input hook 0");
                        }
                        if(curAimX != m_LastRecordedAimX || curAimY != m_LastRecordedAimY)
                        {
                                std::string aimLine = "aim " + std::to_string(curAimX) + " " + std::to_string(curAimY);
                                m_MacroRecordBuffer.push_back(aimLine);
                        }
                        if(curFire != m_LastRecordedFire)
                        {
                                m_MacroRecordBuffer.push_back("input fire");
                        }
                        if(curWeapon != m_LastRecordedWeapon)
                        {
                                std::string wpnLine = "input weapon " + std::to_string(curWeapon + 1);
                                m_MacroRecordBuffer.push_back(wpnLine);
                        }

                        m_LastRecordedDir = curDir;
                        m_LastRecordedJump = curJump;
                        m_LastRecordedHook = curHook;
                        m_LastRecordedFire = curFire;
                        m_LastRecordedAimX = curAimX;
                        m_LastRecordedAimY = curAimY;
                        m_LastRecordedWeapon = curWeapon;
                        m_LastMacroRecordTick = CurTick;
                }
                return;
        }

        if(m_ClientDelay > 0)
        {
                static int64_t s_LastActionTick = 0;
                int64_t Now = Client()->GameTick(0);
                int64_t TickDelay = ((int64_t)m_ClientDelay * Client()->GameTickSpeed()) / 1000;
                if(TickDelay < 1)
                        TickDelay = 1;
                if(Now - s_LastActionTick < TickDelay)
                        return;
                s_LastActionTick = Now;
        }

        if(m_KillOnFreeze && pGame->m_aClients[LocalID].m_FreezeEnd > 0)
        {
                Console()->ExecuteLine("kill", -1, -1);
        }

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
                        if(Dummy != g_Config.m_ClDummy)
                                Controls.m_aInputData[Dummy] = *pInput;
                        return;
                }
        }

        // В OnTick(), после макросов, до атаки
        if(m_PathfinderGoActive)
        {
                vec2 MyPos((float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_X,
                        (float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Y);

                int botTX = (int)(MyPos.x / 32.0f);
                int botTY = (int)(MyPos.y / 32.0f);
                int targetTX = (int)(m_PathfinderGoPos.x / 32.0f);
                int targetTY = (int)(m_PathfinderGoPos.y / 32.0f);

                // Дошли?
                if(botTX == targetTX && botTY == targetTY)
                {
                        m_PathfinderGoActive = false;
                        SetDirection(pGame, Dummy, false, false);
                        pInput->m_Jump = 0;
                        if(Dummy != g_Config.m_ClDummy)
                                Controls.m_aInputData[Dummy] = *pInput;

                        dbg_msg("botnet", "Reached destination");
                        return;
                }

                // Тупо копируем логику из атаки
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
                                if(targetTX != m_LastTargetTX || targetTY != m_LastTargetTY)
                                        needRecalc = true;
                                if(!m_PathFound)
                                        needRecalc = true;
                                if(m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                {
                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                needRecalc = true;
                                }

                                if(needRecalc && IsTileWalkable(targetTX, targetTY))
                                        ComputePathfinder(botTX, botTY, targetTX, targetTY);

                                if(m_PathFound && (botTX != m_LastBotTX || botTY != m_LastBotTY))
                                {
                                        ComputeFlowForTile(botTY, botTX);
                                        m_LastBotTX = botTX;
                                        m_LastBotTY = botTY;
                                }

                                bool left = false, right = false, jump = false;
                                GetMovementFromFlow(left, right, jump);

                                SetDirection(pGame, Dummy, left, right);
                                pInput->m_Jump = jump ? 1 : 0;
                                pInput->m_PlayerFlags |= 1;
                        }
                }
                if(Dummy != g_Config.m_ClDummy)
                        Controls.m_aInputData[Dummy] = *pInput;
                return;
        }

        if(m_AttackEnabled)
        {
                int TargetID = -1;
                float MinDist = 1000000.0f;
                vec2 MyPos((float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[LocalID].m_Cur.m_Y);
                bool TargetIsMain = false;
                bool TargetIsRescue = false; // <-- NEW: помечаем что это rescue-цель

                // ===== СПАСЕНИЕ: Rescue Frozen + Smart Rescue =====
                bool usingSmartRescue = false;
                if(m_RescueFrozen)
                {
                        // Собираем всех замёрзших кого можно спасти
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

                        // Сортируем по дистанции (ближайшие первыми)
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

                        // Пробуем каждого, пока не найдём до кого можно дойти
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

                                        ComputePathfinderRescue(botTX, botTY, resTX, resTY);

                                        if(m_PathFound)
                                        {
                                                usingSmartRescue = true;
                                                break;
                                        }
                                }
                                else
                                {
                                        // Без smart rescue — просто берём ближайшего
                                        break;
                                }

                                // Путь не найден к этому игроку, пробуем следующего
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

                                                // === ОБНОВЛЕНИЕ СИМУЛЯЦИИ ИГРОКОВ ===
                                                UpdatePlayerPenalty(botTX, botTY, targetTX, targetTY, LocalID);

                                                bool needRecalc = false;
                                                if(targetTX != m_LastTargetTX || targetTY != m_LastTargetTY)
                                                        needRecalc = true;
                                                if(!m_PathFound && (m_LastTargetTX == -1 || targetTX != m_LastTargetTX || targetTY != m_LastTargetTY))
                                                        needRecalc = true;
                                                if(m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                                {
                                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                                needRecalc = true;
                                                }
                                                if(m_PfSimulatePlayers)
                                                        needRecalc = true;

                                                if(needRecalc && IsTileWalkable(targetTX, targetTY))
                                                        ComputePathfinder(botTX, botTY, targetTX, targetTY);

                                                if(m_PathFound && (botTX != m_LastBotTX || botTY != m_LastBotTY))
                                                {
                                                        ComputeFlowForTile(botTY, botTX);
                                                        m_LastBotTX = botTX;
                                                        m_LastBotTY = botTY;
                                                }

                                                GetMovementFromFlow(left, right, jump);

                                                if(m_PfSnap && m_PathFound && !left && !right)
                                                {
                                                        int snapTX = (int)roundf(MyPos.x / 32.0f);
                                                        float tileCenterX = snapTX * 32.0f + 16.0f;
                                                        float offsetX = MyPos.x - tileCenterX;

                                                        if(offsetX > 0)
                                                                left = true;
                                                        else if(offsetX < 0)
                                                                right = true;
                                                }

                                                // Stand: стоим на месте, но если рядом freeze — отходим
                                                // (avoid freeze уже внутри flow, но stand его обнуляет — поэтому проверяем)
                                                bool nearFreeze = false;
                                                if(m_AvoidFreeze && m_MapGridLoaded)
                                                {
                                                        vec2 repel = ComputeFreezeRepel(botTX, botTY);
                                                        nearFreeze = (repel.x * repel.x + repel.y * repel.y) > 0.0001f;
                                                }
                                                if(m_StandEnabled && !nearFreeze &&
                                                        ((!m_StandOnXOnly && Dist < m_StandDist) ||
                                                                (m_StandOnXOnly && absolute(dx) < m_StandDist)))
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
                                        // Градиент уже построен в ComputePathfinderRescue выше
                                        // Просто обновляем flow для текущей позиции бота
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

                                                // Проверяем нужно ли пересчитать
                                                int resTX = (int)((float)TChar.m_X / 32.0f);
                                                int resTY = (int)((float)TChar.m_Y / 32.0f);

                                                bool needRecalc = false;
                                                if(resTX != m_LastTargetTX || resTY != m_LastTargetTY)
                                                        needRecalc = true;
                                                if(!m_PathFound)
                                                        needRecalc = true;
                                                if(m_PathFound && botTX >= 0 && botTY >= 0 && botTX < m_MapWidth && botTY < m_MapHeight)
                                                {
                                                        if(m_pfDist[botTY * m_MapWidth + botTX] >= 1e17f)
                                                                needRecalc = true;
                                                }
                                                if(m_PfSimulatePlayers)
                                                        needRecalc = true;

                                                UpdatePlayerPenalty(botTX, botTY, -1, -1, LocalID);

                                                if(needRecalc)
                                                        ComputePathfinderRescue(botTX, botTY, resTX, resTY);

                                                if(m_PathFound && (botTX != m_LastBotTX || botTY != m_LastBotTY))
                                                {
                                                        ComputeFlowForTile(botTY, botTX);
                                                        m_LastBotTX = botTX;
                                                        m_LastBotTY = botTY;
                                                }

                                                GetMovementFromFlow(left, right, jump);

                                                // Stand: стоим на месте, но если рядом freeze — отходим
                                                bool nearFreeze = false;
                                                if(m_AvoidFreeze && m_MapGridLoaded)
                                                {
                                                        vec2 repel = ComputeFreezeRepel(botTX, botTY);
                                                        nearFreeze = (repel.x * repel.x + repel.y * repel.y) > 0.0001f;
                                                }
                                                if(m_StandEnabled && !nearFreeze &&
                                                        ((!m_StandOnXOnly && Dist < m_StandDist) ||
                                                                (m_StandOnXOnly && absolute(dx) < m_StandDist)))
                                                {
                                                        left = false;
                                                        right = false;
                                                        standActive = true;
                                                }

                                                if(m_PfSnap && m_PathFound && !left && !right)
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
                                        if(!m_StandEnabled || (!m_StandOnXOnly && Dist >= m_StandDist) || (m_StandOnXOnly && absolute(dx) >= m_StandDist))
                                        {
                                                left = (dx < -20.0f);
                                                right = (dx > 20.0f);
                                        }
                                        else
                                        {
                                                standActive = true;
                                        }

                                        // Avoid freeze уже учтён внутри ComputeFlowForTile.

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
                                                        if(m_JumpTicks == 0)
                                                        {
                                                                pInput->m_Jump = 1;
                                                                m_JumpTicks = 1;
                                                        }
                                                        else
                                                        {
                                                                pInput->m_Jump = 0;
                                                                m_JumpTicks = 0;
                                                        }
                                                }
                                                else
                                                {
                                                        pInput->m_Jump = 0;
                                                        m_JumpTicks = 0;
                                                }
                                        }

                                        goto after_movement;
                                }

                                SetDirection(pGame, Dummy, left, right);

                                if(jump)
                                {
                                        if(m_JumpTicks == 0)
                                        {
                                                pInput->m_Jump = 1;
                                                m_JumpTicks = 1;
                                        }
                                        else
                                        {
                                                pInput->m_Jump = 0;
                                                m_JumpTicks = 0;
                                        }
                                }
                                else
                                {
                                        pInput->m_Jump = 0;
                                        m_JumpTicks = 0;
                                }
                        }
                after_movement:

                        bool CanShoot = !TargetIsMain || m_AttackMain;
                        if(m_AutoFire && Dist < m_FireDist && CanShoot)
                                pInput->m_Fire++;
                        if(m_AutoHammer && pInput->m_Fire > 0)
                                pInput->m_WantedWeapon = 1;

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
                                        m_HookTickTimer++;
                                        if(m_HookTickTimer >= TicksCycle)
                                                m_HookTickTimer = 0;
                                        pInput->m_Hook = (m_HookTickTimer < TicksCycle - 2);
                                }
                                else
                                {
                                        pInput->m_Hook = 0;
                                        m_HookTickTimer = 0;
                                }
                        }
                        else
                        {
                                pInput->m_Hook = 0;
                                m_HookTickTimer = 0;
                        }

            // === Pathfinder Hook: aim at solid block and hook ===
                        // Сбрасываем таймер, если pathfinder хук выключен, stand активен,
                        // обычный AutoHook уже хукает цель, или точки хука нет.
                        if(!m_PfHookEnabled || standActive || pInput->m_Hook != 0 ||
                                (m_PfHookTile.x == 0 && m_PfHookTile.y == 0))
                        {
                                m_HookTickTimer = 0;
                        }
                        else
                        {
                                int TicksCycle = (50 * m_HookDelay / 1000);
                                if(TicksCycle < 2)
                                        TicksCycle = 2;

                                // Направляем аим и нажимаем хук
                                float hDx = m_PfHookTile.x - MyPos.x;
                                float hDy = m_PfHookTile.y - MyPos.y;
                                float hDist = sqrtf(hDx * hDx + hDy * hDy);
                                if(hDist > 0.001f && hDist < m_HookDist)
                                {
                                        SetMousePos(pGame, Dummy, vec2(hDx, hDy));
                                        pInput->m_Hook = 1;
                                }

                                // Если хук нажат (бот зацепился), считаем тики до сброса
                                if(pInput->m_Hook != 0)
                                {
                                        m_HookTickTimer++;
                                        if(m_HookTickTimer >= TicksCycle)
                                        {
                                                pInput->m_Hook = 0; // Сброс хука каждые HOOK_DELAY
                                                m_HookTickTimer = 0;
                                        }
                                }
                                else
                                {
                                        m_HookTickTimer = 0; // Если ещё не зацепились, таймер не тикает
                                }
                        }

                        pInput->m_PlayerFlags |= 1;
                }
                else
                {
                        SetDirection(pGame, Dummy, false, false);
                        pInput->m_Hook = 0;
                        m_HookTickTimer = 0;
                }
        }

        if(m_RandomAim && !m_AttackEnabled && !m_CopyMoves)
        {
                if(CurTick >= m_NextRandomAimTick)
                {
                        GameClient()->m_BotControl.ActionOverrideAim((rand() % 2001) - 1000, (rand() % 2001) - 1000, Dummy);
                        int TS = Client()->GameTickSpeed();
                        m_NextRandomAimTick = CurTick + (int64_t)(TS ? TS : 50) * m_RandomAimInterval / 1000;
                }
        }

        if(Dummy != g_Config.m_ClDummy)
                Controls.m_aInputData[Dummy] = *pInput;
}


void CBotNet::OnReset()
{
        m_AttackEnabled = false;
        m_CopyMoves = false;
        m_RandomAim = false;
        m_MacroPlaying = false;
        m_MacroRecording = false;
        m_PathfinderGoActive = false;
        m_HookTickTimer = 0;
        m_JumpTicks = 0;
        m_PathFound = false;
        m_FlowDir = vec2(0, 0);
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

        p->m_MacroPlayLines.clear();
        std::string line;
        while(std::getline(file, line))
        {
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if(!line.empty())
                        p->m_MacroPlayLines.push_back(line);
        }
        file.close();

        dbg_msg("botnet_macro", "Loaded %d macro lines", (int)p->m_MacroPlayLines.size());
        if(p->Console())
        {
                char aBuf[64];
                str_format(aBuf, sizeof(aBuf), "Loaded %d macro lines", (int)p->m_MacroPlayLines.size());
                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", aBuf);
        }
}

void CBotNet::ConMacroPlay(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "ConMacroPlay called: %d", on);
        if(on)
        {
                if(p->m_MacroPlayLines.empty())
                {
                        dbg_msg("botnet_macro", "No macro loaded to play.");
                        if(p->Console())
                                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "No macro loaded");
                        return;
                }
                p->m_MacroPlaying = true;
                p->m_MacroPlayIndex = 0;
                p->m_MacroSleepTicks = 0;
                dbg_msg("botnet_macro", "Playback started.");
        }
        else
        {
                p->m_MacroPlaying = false;
                if(p->GameClient())
                {
                        int Dummy = p->m_TargetDummy;
                        CGameClient *pGame = p->GameClient();
                        SetDirection(pGame, Dummy, false, false);
                        CNetObj_PlayerInput *pInput = (Dummy == g_Config.m_ClDummy) ? &pGame->m_Controls.m_aInputData[Dummy] : &pGame->m_aDummyInput[Dummy];
                        pInput->m_Jump = 0;
                        pInput->m_Hook = 0;
                        pInput->m_Fire = 0;
                        if(Dummy != g_Config.m_ClDummy)
                                pGame->m_Controls.m_aInputData[Dummy] = *pInput;
                }
                p->GameClient()->m_BotControl.ActionStop(p->m_TargetDummy);
                dbg_msg("botnet_macro", "Playback stopped manually.");
        }
}

void CBotNet::ConMacroRecord(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "ConMacroRecord called with on=%d", on);

        if(on)
        {
                dbg_msg("botnet_macro", "Starting macro recording...");
                p->m_MacroRecording = true;
                p->m_MacroRecordBuffer.clear();
                dbg_msg("botnet_macro", "Buffer cleared.");

                p->m_LastMacroRecordTick = p->Client() ? p->Client()->GameTick(0) : 0;
                p->m_LastRecordedDir = 0;
                p->m_LastRecordedJump = 0;
                p->m_LastRecordedHook = 0;
                p->m_LastRecordedFire = 0;
                p->m_LastRecordedAimX = 0;
                p->m_LastRecordedAimY = 0;
                p->m_LastRecordedWeapon = -1;

                dbg_msg("botnet_macro", "Macro recording started successfully. Start tick: %lld", (long long)p->m_LastMacroRecordTick);
        }
        else
        {
                dbg_msg("botnet_macro", "Stopping macro recording...");
                p->m_MacroRecording = false;
                dbg_msg("botnet_macro", "Macro recording stopped. Total lines recorded in buffer: %d", (int)p->m_MacroRecordBuffer.size());
        }
}

void CBotNet::ConMacroSave(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        const char *path = pResult->GetString(0);
        dbg_msg("botnet_macro", "ConMacroSave called with path: %s", path ? path : "NULL");
        if(!path || !path[0])
                return;

        std::ofstream file(path);
        if(!file.is_open())
        {
                dbg_msg("botnet_macro", "Failed to open file for writing: %s", path);
                if(p->Console())
                        p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Failed to open file for writing");
                return;
        }

        dbg_msg("botnet_macro", "Writing %d lines to file...", (int)p->m_MacroRecordBuffer.size());
        for(const std::string &line : p->m_MacroRecordBuffer)
                file << line << "\n";
        file.close();

        dbg_msg("botnet_macro", "File saved successfully.");
        if(p->Console())
                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Macro saved");
}

void CBotNet::ConMacroCapture(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        p->m_MacroCaptureID = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "Capture ID set to: %d", p->m_MacroCaptureID);
}

void CBotNet::ConTargetDummy(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int val = pResult->GetInteger(0);
        if(val < 0) val = 0;
        if(val >= MAX_DUMMIES) val = MAX_DUMMIES - 1;
        p->m_TargetDummy = val;
        dbg_msg("botnet", "Target dummy set to: %d", val);
}

void CBotNet::ConAttackHookDelay(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_HookDelay = pResult->GetInteger(0);
        ((CBotNet *)pUserData)->m_HookTickTimer = 0;
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
                        p->m_LastTargetTX = -1;
                        p->m_LastTargetTY = -1;
                        p->m_PathFound = false;
                }
        }
        p->m_AvoidFreeze = pResult->GetInteger(13) != 0;
        {
                bool newPfHook = pResult->GetInteger(14) != 0;
                if(newPfHook != p->m_PfHookEnabled)
                {
                        p->m_PfHookEnabled = newPfHook;
                        p->m_PfHookTile = vec2(0, 0);
                }
        }
}

void CBotNet::ConAttackEnable(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->m_AttackEnabled = pResult->GetInteger(0) != 0;
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
        pSelf->m_LastTargetAttackTick = -1;
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
        p->m_LastTargetTX = -1;
        p->m_LastTargetTY = -1;
        p->m_PathFound = false;
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
                p->m_PathfinderGoActive = false;
                p->m_LastTargetTX = -1;
                p->m_LastTargetTY = -1;
                p->m_PathFound = false;
                dbg_msg("botnet", "Pathfinder go disabled");
                return;
        }

        if(pResult->NumArguments() >= 3)
        {
                int x = pResult->GetInteger(1);
                int y = pResult->GetInteger(2);
                p->m_PathfinderGoActive = true;
                p->m_PathfinderGoPos = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f); // центр тайла
                dbg_msg("botnet", "Pathfinder go to tile (%d, %d) -> pos (%.0f, %.0f)", x, y, p->m_PathfinderGoPos.x, p->m_PathfinderGoPos.y);
        }
}