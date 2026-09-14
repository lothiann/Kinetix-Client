#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// =========================================================
// MAP GRID — tile snapshot of the loaded map + tile queries
// + the grid A* used by the bot flow pathfinder.
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
	if(m_pfFreezeDist)
		delete[] m_pfFreezeDist;
	if(m_pForbiddenGrid)
		delete[] m_pForbiddenGrid;
	if(m_pCustomFinishGrid)
		delete[] m_pCustomFinishGrid;

	// Invalidate every dependent cache — the old buffers were sized for the
	// previous map. Keeping them alive caused heap overflows and stale BFS
	// sources after a map vote (see v1.56.164).
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
	m_PfForbiddenTiles.clear();
	m_PfCustomFinishTiles.clear();

	m_MapWidth = Width;
	m_MapHeight = Height;
	int Size = Width * Height;

	m_pMapGrid = new unsigned char[Size];
	m_pFrontGrid = new unsigned char[Size];
	m_pfDist = new float[Size];
	m_pfFreezeDist = new unsigned char[Size];
	m_pForbiddenGrid = new unsigned char[Size];
	m_pCustomFinishGrid = new unsigned char[Size];

	mem_zero(m_pMapGrid, Size);
	mem_zero(m_pFrontGrid, Size);
	mem_zero(m_pForbiddenGrid, Size);
	mem_zero(m_pCustomFinishGrid, Size);

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
		m_pfFreezeDist[i] = 255;
	}

	// Route distance to every freeze pool — once per map load; the
	// survival retreat reads it.
	ComputeFreezeDistField();

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
	if(m_pForbiddenGrid && m_pForbiddenGrid[idx])
		return false;
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

// =========================================================
// FREEZE ROUTE DISTANCE - multi-source BFS from every freeze
// tile through walkable tiles only, so the distance is a real
// route (walls are never crossed).  255 = no freeze in reach.
// Feeds the A* danger cost and the survival retreat.
// =========================================================

void CBotNet::ComputeFreezeDistField()
{
	int Size = m_MapWidth * m_MapHeight;
	for(int i = 0; i < Size; i++)
		m_pfFreezeDist[i] = 255;

	static const int dr[] = {-1, 1, 0, 0};
	static const int dc[] = {0, 0, -1, 1};
	std::vector<int> q;
	q.reserve(Size);
	int head = 0;
	for(int i = 0; i < Size; i++)
	{
		if(IsTileFreeze(i % m_MapWidth, i / m_MapWidth))
		{
			m_pfFreezeDist[i] = 0;
			q.push_back(i);
		}
	}

	while(head < (int)q.size())
	{
		int idx = q[head++];
		int r = idx / m_MapWidth, c = idx % m_MapWidth;
		int nd = m_pfFreezeDist[idx] + 1;
		if(nd >= 255)
			continue;
		for(int d = 0; d < 4; d++)
		{
			int nr = r + dr[d], nc = c + dc[d];
			if(nc < 0 || nr < 0 || nc >= m_MapWidth || nr >= m_MapHeight)
				continue;
			int nIdx = nr * m_MapWidth + nc;
			if(m_pfFreezeDist[nIdx] <= nd)
				continue;
			if(!IsTileWalkable(nc, nr))
				continue;
			m_pfFreezeDist[nIdx] = nd;
			q.push_back(nIdx);
		}
	}
}
