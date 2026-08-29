#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <utility>
#include <vector>

// =========================================================
// TUNNEL GENERATOR (Pathfinder tab — "Generate tunnel")
//
// Gradient descent over the FMM flow field gives the route
// start->finish; free space = BFS dilation of the route by
// Tunnel size; then a multi-source spread floods everything
// else (blocked only by solid tiles) into forbidden zones.
// =========================================================

namespace
{
	const int PfTunDr[] = {-1, 1, 0, 0};
	const int PfTunDc[] = {0, 0, -1, 1};
} // namespace

void CBotNet::PfGenerateTunnel()
{
	CGameClient *pGame = GameClient();
	if(!pGame)
		return;
	if(!m_MapGridLoaded)
		LoadMapGrid();
	if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0 || !m_pMapGrid || !m_pFrontGrid || !m_pForbiddenGrid)
	{
		dbg_msg("pathfinder", "tunnel: map grid not loaded");
		return;
	}

	int tunnelSize = g_Config.m_KxPfTunnelSize;
	if(tunnelSize < 0)
		tunnelSize = 0;
	if(tunnelSize > 10)
		tunnelSize = 10;

	const int Size = m_MapWidth * m_MapHeight;

	// Blocked = solid only: the spread/corridor crosses freeze,
	// teleport, death and nohook tiles.
	auto fnBlocked = [&](int tx, int ty) -> bool {
		if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
			return true;
		int idx = ty * m_MapWidth + tx;
		return m_pMapGrid[idx] == TILE_SOLID || m_pFrontGrid[idx] == TILE_SOLID;
	};

	// Start tile = active player position (fallback: last run start).
	int startTX = -1, startTY = -1;
	int LocalID = pGame->m_Snap.m_LocalClientId;
	if(LocalID >= 0)
	{
		CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalID);
		if(pChar)
		{
			startTX = (int)(pChar->GetCore().m_Pos.x / 32.0f);
			startTY = (int)(pChar->GetCore().m_Pos.y / 32.0f);
		}
	}
	if(startTX < 0 || startTY < 0 || startTX >= m_MapWidth || startTY >= m_MapHeight)
	{
		startTX = (int)(m_PfStartPos.x / 32.0f);
		startTY = (int)(m_PfStartPos.y / 32.0f);
	}
	if(startTX < 0 || startTY < 0 || startTX >= m_MapWidth || startTY >= m_MapHeight || fnBlocked(startTX, startTY))
	{
		dbg_msg("pathfinder", "tunnel: no valid start position (stand somewhere walkable)");
		return;
	}

	// Force a fresh flow field for the currently active finish sources
	// (same as PfResetRun does). Map finishes are only collected inside
	// PfComputeFlowField, so without this the tunnel demanded custom
	// finish tiles on a freshly loaded map.
	PfEditorBeginChange();

	if(m_PfFlowField)
	{
		delete[] m_PfFlowField;
		m_PfFlowField = nullptr;
	}
	m_PfFinishTiles.clear();
	PfComputeFlowField(vec2(startTX * 32.0f + 16.0f, startTY * 32.0f + 16.0f));
	if(!m_PfFlowField)
	{
		dbg_msg("pathfinder", "tunnel: flow field unavailable");
		PfEditorEndChange();
		return;
	}

	// Finish tiles (map + custom, active set).
	std::vector<std::pair<int, int>> vFin;
	for(const vec2 &fp : PfActiveFinishTiles())
	{
		int tx = (int)(fp.x / 32.0f);
		int ty = (int)(fp.y / 32.0f);
		if(tx >= 0 && ty >= 0 && tx < m_MapWidth && ty < m_MapHeight && !fnBlocked(tx, ty))
			vFin.push_back(std::make_pair(tx, ty));
	}
	if(vFin.empty())
	{
		dbg_msg("pathfinder", "tunnel: no finish tiles (paint custom finish tiles or play a map with finish)");
		PfEditorEndChange();
		return;
	}

	std::vector<unsigned char> corridor(Size, 0);

	if(m_PfFlowField[startTY * m_MapWidth + startTX] >= 1e17f)
	{
		dbg_msg("pathfinder", "tunnel: flow field does not reach the start (blocked by forbidden zones?)");
		PfEditorEndChange();
		return;
	}

	// Gradient descent start -> finish over the FMM field.
	int cx = startTX, cy = startTY;
	corridor[cy * m_MapWidth + cx] = 1;
	for(int step = 0; step < Size; step++)
	{
		float cur = m_PfFlowField[cy * m_MapWidth + cx];
		if(cur <= 0.0f)
			break;
		int bx = -1, by = -1;
		float bd = cur;
		for(int k = 0; k < 4; k++)
		{
			int nx = cx + PfTunDc[k], ny = cy + PfTunDr[k];
			if(nx < 0 || ny < 0 || nx >= m_MapWidth || ny >= m_MapHeight)
				continue;
			float nd = m_PfFlowField[ny * m_MapWidth + nx];
			if(nd < bd)
			{
				bd = nd;
				bx = nx;
				by = ny;
			}
		}
		if(bx < 0)
			break;
		cx = bx;
		cy = by;
		corridor[cy * m_MapWidth + cx] = 1;
	}
	if(m_PfFlowField[cy * m_MapWidth + cx] > 0.0f)
		dbg_msg("pathfinder", "tunnel: descent did not reach a finish (best %f)", m_PfFlowField[cy * m_MapWidth + cx]);

	// Free space = BFS dilation of the path by tunnelSize
	// (0 = the path line itself stays the only free tiles).
	std::vector<int> depth(Size, -1);
	std::vector<std::pair<int, int>> queue;
	queue.reserve(Size);
	for(int i = 0; i < Size; i++)
	{
		if(corridor[i])
		{
			depth[i] = 0;
			queue.push_back(std::make_pair(i % m_MapWidth, i / m_MapWidth));
		}
	}
	for(size_t qi = 0; qi < queue.size(); qi++)
	{
		int x = queue[qi].first, y = queue[qi].second;
		int d = depth[y * m_MapWidth + x];
		if(d >= tunnelSize)
			continue;
		for(int k = 0; k < 4; k++)
		{
			int nx = x + PfTunDc[k], ny = y + PfTunDr[k];
			if(nx < 0 || ny < 0 || nx >= m_MapWidth || ny >= m_MapHeight)
				continue;
			int nIdx = ny * m_MapWidth + nx;
			if(depth[nIdx] >= 0 || fnBlocked(nx, ny))
				continue;
			depth[nIdx] = d + 1;
			corridor[nIdx] = 1;
			queue.push_back(std::make_pair(nx, ny));
		}
	}

	// Spread from the corridor border through everything except solid
	// tiles and the corridor itself; everything reached is forbidden.
	std::vector<unsigned char> forbid(Size, 0);
	std::vector<unsigned char> visited(Size, 0);
	queue.clear();
	for(int y = 0; y < m_MapHeight; y++)
	{
		for(int x = 0; x < m_MapWidth; x++)
		{
			int idx = y * m_MapWidth + x;
			if(corridor[idx] || fnBlocked(x, y))
				continue;
			bool border = false;
			for(int k = 0; k < 4 && !border; k++)
			{
				int nx = x + PfTunDc[k], ny = y + PfTunDr[k];
				if(nx < 0 || ny < 0 || nx >= m_MapWidth || ny >= m_MapHeight)
					continue;
				if(corridor[ny * m_MapWidth + nx])
					border = true;
			}
			if(border)
			{
				visited[idx] = 1;
				forbid[idx] = 1;
				queue.push_back(std::make_pair(x, y));
			}
		}
	}
	for(size_t qi = 0; qi < queue.size(); qi++)
	{
		int x = queue[qi].first, y = queue[qi].second;
		for(int k = 0; k < 4; k++)
		{
			int nx = x + PfTunDc[k], ny = y + PfTunDr[k];
			if(nx < 0 || ny < 0 || nx >= m_MapWidth || ny >= m_MapHeight)
				continue;
			int nIdx = ny * m_MapWidth + nx;
			if(visited[nIdx] || corridor[nIdx] || fnBlocked(nx, ny))
				continue;
			visited[nIdx] = 1;
			forbid[nIdx] = 1;
			queue.push_back(std::make_pair(nx, ny));
		}
	}

	// Commit (overlay on top of the existing forbidden zones).
	int added = 0;
	for(int i = 0; i < Size; i++)
	{
		if(forbid[i] && !m_pForbiddenGrid[i])
		{
			int tx = i % m_MapWidth, ty = i / m_MapWidth;
			m_pForbiddenGrid[i] = 1;
			m_PfForbiddenTiles.push_back(vec2(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f));
			added++;
		}
	}
	dbg_msg("pathfinder", "tunnel: +%d forbidden tiles (size %d)", added, tunnelSize);
	PfEditorEndChange();
}
