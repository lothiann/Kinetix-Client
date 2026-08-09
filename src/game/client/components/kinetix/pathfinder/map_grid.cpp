#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

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

        // v1.56.164: invalidate dependent BFS caches. Previously these survived
        // a grid reload — m_PfFlowField kept its OLD buffer (allocated for the
        // previous map's W*H) while m_MapWidth/m_MapHeight updated to the new
        // map. PfComputeFlowField's lazy guard (if(!m_PfFlowField || ...))
        // then SKIPPED reallocation, and the subsequent
        //   for(int i=0; i<Size; i++) m_PfFlowField[i] = 1e18f;
        // wrote Size=new_W*new_H floats into a buffer of old_W*old_H →
        // HEAP OVERFLOW if the new map is bigger. Even at equal size, the
        // BFS sources (m_PfFinishTiles) were stale → flow field pointed
        // toward the old map's finishes. Clearing here forces a clean
        // recomputation in PfComputeFlowField on next access.
        if(m_PfFlowField)
        {
                delete[] m_PfFlowField;
                m_PfFlowField = nullptr;
        }
        if(m_PfScoreField)
        {
                delete[] m_PfScoreField;
                m_PfScoreField = nullptr;
        }
        m_PfScoreFieldW = 0;
        m_PfScoreFieldH = 0;
        m_PfFinishTiles.clear();

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

        if(g_Config.m_KxPfSimulatePlayers && m_PfPlayerPenalty)
        {
                cost += m_PfPlayerPenalty[idx];
        }

        return cost;
}

bool CBotNet::PfAStarSearch(int botTX, int botTY,
        const std::vector<std::pair<int, int>> &sources, bool skipFreeze)
{
        // Reset the working buffers.
        int Size = m_MapWidth * m_MapHeight;
        for(int i = 0; i < Size; i++)
        {
                m_pfDist[i] = 1e18f;
                m_pfVisited[i] = false;
        }

        const float SQRT2 = 1.4142135623730951f;
        // 8-neighbour offsets: 4 orthogonal + 4 diagonal.
        static const int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        static const int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

        // Octile-distance heuristic toward the bot tile (admissible).
        auto heuristic = [&](int r, int c) -> float {
                int dy = abs(r - botTY);
                int dx = abs(c - botTX);
                return pf_maxf((float)dy, (float)dx) + (SQRT2 - 1.0f) * pf_minf((float)dy, (float)dx);
        };

        // Multi-source seed: every source tile starts at g=0.
        std::priority_queue<PfNode, std::vector<PfNode>, std::greater<PfNode>> open;
        for(const auto &src : sources)
        {
                int idx = src.second * m_MapWidth + src.first;
                m_pfDist[idx] = 0.0f;
                open.push({heuristic(src.second, src.first), 0.0f, src.second, src.first});
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

                        // Rescue variant: freeze tiles are walls (skip entirely).
                        if(skipFreeze && IsTileFreeze(nc, nr))
                                continue;

                        float tileCost = GetTileCost(nc, nr);
                        if(tileCost < 0)
                                continue;

                        // Diagonal move: prevent corner-cutting through two solids.
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

        return m_pfVisited[botTY * m_MapWidth + botTX];
}
