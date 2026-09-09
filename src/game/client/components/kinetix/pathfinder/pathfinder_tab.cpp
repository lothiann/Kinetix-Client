#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <engine/keys.h>
#include <algorithm>
#include <cmath>

// =========================================================
// PATHFINDER TAB — state machine, flow fields, scoring,
// playback, tile editor and rendering.
// =========================================================

// Release ALL A* search buffers and stop the worker. Safe to call from
// any state — it never touches the player or the map, only search data.
// Without this the heavy buffers (nodes with per-node Traj/Inputs vectors,
// RHEA population) stayed alive across idle periods and corrupted vector
// headers led to crashes on restart (v1.56.165 BUG4).
void CBotNet::PfClearSearchState()
{
	PfThreadStop();
	if(m_PfBaseWorld)
	{
		delete m_PfBaseWorld;
		m_PfBaseWorld = nullptr;
	}
	if(m_PfSimWorld)
	{
		delete m_PfSimWorld;
		m_PfSimWorld = nullptr;
	}
	m_PfLocalID = -1;
	m_PfThreadResult.store(-1);
	std::vector<AStarNode>().swap(m_PfANodes);
	std::vector<int>().swap(m_PfAOpen);
	std::unordered_map<uint64_t, float>().swap(m_PfABestG);
	m_PfAGoalIdx = -1;
	m_PfAStarted = false;
	m_PfAPathReady = false;
	m_PfAExpandCount = 0;
	m_PfFullInputs.clear();
	m_PfFullInputsIdx = 0;
	m_PfUiBestDone = 0;
	m_PfUiBestTotal = 0;
}

// Start a fresh search from the active player's predicted state.
void CBotNet::PfResetRun()
{
	m_PfVPath.clear();
	m_PfVHookSegs.clear();
	m_PfChunkCount = 0;
	m_PfTickCounter = 0;

	CGameClient *pGame = GameClient();
	if(!pGame || !pGame->m_Snap.m_pLocalInfo)
	{
		dbg_msg("pathfinder", "PfResetRun: no game or no local info");
		return;
	}

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

	PfClearSearchState();

	m_PfVPath.push_back(Core.m_Pos);

	// Force a flow-field rebuild with the currently active finish sources.
	if(m_PfFlowField)
	{
		delete[] m_PfFlowField;
		m_PfFlowField = nullptr;
	}
	m_PfFinishTiles.clear();
	PfComputeFlowField(m_PfStartPos);

	const std::vector<vec2> &vFinishes = PfActiveFinishTiles();
	if(vFinishes.empty())
	{
		dbg_msg("pathfinder", "PfResetRun: no finish tiles (paint custom finish tiles or play a map with finish)");
		return;
	}

	const char *pFlowStatus = "unavailable";
	if(m_PfFlowField)
	{
		int sTX = pf_clamp((int)(m_PfStartPos.x / 32.0f), 0, m_MapWidth - 1);
		int sTY = pf_clamp((int)(m_PfStartPos.y / 32.0f), 0, m_MapHeight - 1);
		float d = m_PfFlowField[sTY * m_MapWidth + sTX];
		pFlowStatus = (d < 1e17f) ? "reachable" : "grid-unreachable (A* uses air-distance heuristic)";
	}

	dbg_msg("pathfinder", "PfResetRun: start=(%.0f,%.0f) finishes=%d flow=%s",
		Core.m_Pos.x, Core.m_Pos.y, (int)vFinishes.size(), pFlowStatus);

	// Snapshot the predicted world for the worker thread. The worker never
	// touches GameClient() — it only reads from this snapshot.
	if(m_PfBaseWorld)
		delete m_PfBaseWorld;
	m_PfBaseWorld = new CGameWorld();
	m_PfBaseWorld->CopyWorld(&pGame->m_PredictedWorld);
	m_PfLocalID = LocalID;

	PfThreadStart();
	m_PfAStarted = true;
}

// kx_pf_paste: copy the found path into the TAS Playground as the current run.
void CBotNet::PfPasteToTas()
{
	if(!m_PfAPathReady)
	{
		dbg_msg("pathfinder", "kx_pf_paste: path not ready (run pathfinder first, or press Stop after it finds a path)");
		return;
	}
	if(m_PfFullInputs.empty())
	{
		dbg_msg("pathfinder", "kx_pf_paste: m_PfFullInputs is empty");
		return;
	}
	std::vector<std::pair<vec2, vec2>> vSegs;
	for(const PfHookSeg &Seg : m_PfVHookSegs)
		vSegs.emplace_back(Seg.teePos, Seg.hookPos);
	GameClient()->m_Tas.PasteRun(m_PfFullInputs, m_PfVPath, vSegs);
}

// =========================================================
// FLOW FIELD + SCORE FIELD
//
// Two fields, each filled by the same two-phase BFS:
//   Phase 1: freeze tiles impassable — freeze-free routes win when they
//            exist (freeze tiles stay 1e18 = unreachable).
//   Phase 2: only if Phase 1 did not reach the run start — freeze becomes
//            passable at normal cost (the only route crosses freeze).
// Forbidden zones are always impassable (via the walk predicates).
// =========================================================

namespace
{
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
} // namespace

void CBotNet::PfComputeFlowField(const vec2 &StartPos)
{
	if(!m_MapGridLoaded || m_MapWidth <= 0 || m_MapHeight <= 0)
		return;

	int Size = m_MapWidth * m_MapHeight;
	if(!m_PfFlowField || m_PfFinishTiles.empty())
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

	const std::vector<vec2> &vSources = PfActiveFinishTiles();
	if(vSources.empty())
	{
		for(int i = 0; i < Size; i++)
			m_PfFlowField[i] = 1e18f;
		return;
	}

	auto fnTileWalk = [this](int tx, int ty) -> bool { return IsTileWalkable(tx, ty); };
	auto fnTileFreeze = [this](int tx, int ty) -> bool {
		if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
			return false;
		int idx = ty * m_MapWidth + tx;
		return m_pMapGrid[idx] == TILE_FREEZE || m_pMapGrid[idx] == TILE_DFREEZE ||
		       m_pFrontGrid[idx] == TILE_FREEZE || m_pFrontGrid[idx] == TILE_DFREEZE;
	};

	std::vector<std::pair<int, int>> vSrcTiles;
	for(const vec2 &fp : vSources)
	{
		int tx = (int)(fp.x / 32.0f);
		int ty = (int)(fp.y / 32.0f);
		if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
			continue;
		vSrcTiles.emplace_back(tx, ty);
	}

	// Freedom Path: tiles far from any non-air tile (walls, freeze,
	// teleports, painted forbidden zones) are cheaper to traverse, so
	// the flow field prefers open space. Ratio is stored x10 in config;
	// the per-tile speed is squared so the modifier has real teeth.
	const float freedomRatio = (g_Config.m_KxPfFreedom != 0) ? (float)g_Config.m_KxPfFreedomRatio / 10.0f : 0.0f;
	std::vector<float> vHop;
	if(freedomRatio > 0.0f)
	{
		std::vector<int> vClear(Size, -1);
		std::vector<std::pair<int, int>> vBfs;
		vBfs.reserve(Size);
		for(int ty = 0; ty < m_MapHeight; ty++)
		{
			for(int tx = 0; tx < m_MapWidth; tx++)
			{
				int idx = ty * m_MapWidth + tx;
				if(m_pMapGrid[idx] != TILE_AIR || m_pFrontGrid[idx] != TILE_AIR ||
					(m_pForbiddenGrid && m_pForbiddenGrid[idx]))
				{
					vClear[idx] = 0;
					vBfs.push_back(std::make_pair(tx, ty));
				}
			}
		}
		static const int s_hopDr[] = {-1, 1, 0, 0};
		static const int s_hopDc[] = {0, 0, -1, 1};
		for(size_t qi = 0; qi < vBfs.size(); qi++)
		{
			int x = vBfs[qi].first, y = vBfs[qi].second;
			int d = vClear[y * m_MapWidth + x];
			for(int k = 0; k < 4; k++)
			{
				int nx = x + s_hopDc[k], ny = y + s_hopDr[k];
				if(nx < 0 || ny < 0 || nx >= m_MapWidth || ny >= m_MapHeight)
					continue;
				int nIdx = ny * m_MapWidth + nx;
				if(vClear[nIdx] >= 0)
					continue;
				vClear[nIdx] = d + 1;
				vBfs.push_back(std::make_pair(nx, ny));
			}
		}
		vHop.resize(Size);
		for(int i = 0; i < Size; i++)
		{
			float speed = 1.0f + freedomRatio * (float)vClear[i];
			vHop[i] = 1.0f / (speed * speed);
		}
	}
	const float *pHop = vHop.empty() ? nullptr : vHop.data();

	PfBfsFill(m_PfFlowField, m_MapWidth, m_MapHeight, vSrcTiles, fnTileWalk, fnTileFreeze, false, pHop);

	bool phase1ReachedStart = false;
	{
		int startTX = (int)(StartPos.x / 32.0f);
		int startTY = (int)(StartPos.y / 32.0f);
		if(startTX >= 0 && startTY >= 0 && startTX < m_MapWidth && startTY < m_MapHeight &&
			(StartPos.x != 0.0f || StartPos.y != 0.0f))
			phase1ReachedStart = m_PfFlowField[startTY * m_MapWidth + startTX] < 1e17f;
	}
	if(!phase1ReachedStart && g_Config.m_KxPfFreezeSupport != 0)
		PfBfsFill(m_PfFlowField, m_MapWidth, m_MapHeight, vSrcTiles, fnTileWalk, fnTileFreeze, true, pHop);

	// Score field: 4x resolution (8px cells) for sub-tile distance scoring.
	m_PfScoreFieldW = m_MapWidth * 4;
	m_PfScoreFieldH = m_MapHeight * 4;
	int scoreSize = m_PfScoreFieldW * m_PfScoreFieldH;
	if(m_PfScoreField)
		delete[] m_PfScoreField;
	m_PfScoreField = new float[scoreSize];

	auto fnSubWalk = [this](int scx, int scy) -> bool {
		int tx = scx / 4;
		int ty = scy / 4;
		if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
			return false;
		int idx = ty * m_MapWidth + tx;
		if(m_pForbiddenGrid && m_pForbiddenGrid[idx])
			return false;
		unsigned char t = m_pMapGrid[idx];
		unsigned char ft = m_pFrontGrid[idx];
		return t != TILE_SOLID && t != TILE_DEATH && ft != TILE_DEATH;
	};
	auto fnSubFreeze = [this](int scx, int scy) -> bool {
		int tx = scx / 4;
		int ty = scy / 4;
		if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
			return false;
		int idx = ty * m_MapWidth + tx;
		return m_pMapGrid[idx] == TILE_FREEZE || m_pMapGrid[idx] == TILE_DFREEZE ||
		       m_pFrontGrid[idx] == TILE_FREEZE || m_pFrontGrid[idx] == TILE_DFREEZE;
	};

	std::vector<std::pair<int, int>> vSrcSub;
	for(const vec2 &fp : vSources)
	{
		int scx = (int)(fp.x / 8.0f);
		int scy = (int)(fp.y / 8.0f);
		if(scx < 0 || scy < 0 || scx >= m_PfScoreFieldW || scy >= m_PfScoreFieldH)
			continue;
		vSrcSub.emplace_back(scx, scy);
	}

	PfBfsFill(m_PfScoreField, m_PfScoreFieldW, m_PfScoreFieldH, vSrcSub, fnSubWalk, fnSubFreeze, false);

	bool scorePhase1ReachedStart = false;
	{
		int startSCX = (int)(StartPos.x / 8.0f);
		int startSCY = (int)(StartPos.y / 8.0f);
		if(startSCX >= 0 && startSCY >= 0 && startSCX < m_PfScoreFieldW && startSCY < m_PfScoreFieldH &&
			(StartPos.x != 0.0f || StartPos.y != 0.0f))
			scorePhase1ReachedStart = m_PfScoreField[startSCY * m_PfScoreFieldW + startSCX] < 1e17f;
	}
	if(!scorePhase1ReachedStart && g_Config.m_KxPfFreezeSupport != 0)
		PfBfsFill(m_PfScoreField, m_PfScoreFieldW, m_PfScoreFieldH, vSrcSub, fnSubWalk, fnSubFreeze, true);
}

#include <chrono>

static double PfNowSec()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Per-update step of the state machine. The search itself runs on the
// worker thread; this polls the result and drives the playback cursor.
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

	if(m_PfFlowField == nullptr || (m_PfFinishTiles.empty() && m_PfCustomFinishTiles.empty()))
		PfComputeFlowField(m_PfStartPos);
	if(m_PfFinishTiles.empty() && m_PfCustomFinishTiles.empty())
	{
		dbg_msg("pathfinder", "no finish tiles — stopping");
		m_PfState = PF_STATE_FINISHED;
		return;
	}

	// Playback phase: path found, advance the visual chunk cursor.
	if(m_PfAPathReady)
	{
		m_PfTickCounter++;
		int perf = pf_clamp(g_Config.m_KxPfPerf, 1, 100);
		int tickInterval = (perf >= 100) ? 1 : 60 * (100 - perf) + 1;
		if(m_PfTickCounter < tickInterval)
			return;
		m_PfTickCounter = 0;

		if(m_PfFullInputsIdx >= m_PfFullInputs.size())
		{
			m_PfState = PF_STATE_FINISHED;
			return;
		}
		size_t endIdx = m_PfFullInputsIdx + 10;
		if(endIdx > m_PfFullInputs.size())
			endIdx = m_PfFullInputs.size();
		m_PfFullInputsIdx = endIdx;
		m_PfChunkCount++;
		if(m_PfFullInputsIdx >= m_PfFullInputs.size())
			m_PfState = PF_STATE_FINISHED;
		return;
	}

	// Search phase: poll the worker result.
	int result = m_PfThreadResult.load();
	if(result == 0)
		return; // still searching

	if(result == 1 && !PfEffortFreezeSupport()
		&& m_PfAGoalIdx >= 0 && m_PfAGoalIdx < (int)m_PfANodes.size()
		&& m_PfANodes[m_PfAGoalIdx].FreezeChainTicks > 0)
	{
		dbg_msg("pathfinder", "result rejected: goal node carries %d freeze ticks", m_PfANodes[m_PfAGoalIdx].FreezeChainTicks);
		result = -1;
	}

	if(result == 1
		&& m_PfAGoalIdx >= 0 && m_PfAGoalIdx < (int)m_PfANodes.size()
		&& m_PfANodes[m_PfAGoalIdx].ForbiddenChainTicks > 0)
	{
		dbg_msg("pathfinder", "result rejected: goal node carries %d forbidden ticks", m_PfANodes[m_PfAGoalIdx].ForbiddenChainTicks);
		result = -1;
	}

	if(result == 1)
	{
		PfAStarReconstruct();
		m_PfAPathReady = true;
		m_PfFullInputsIdx = 0;
		dbg_msg("pathfinder", "path found: %d ticks, %zu nodes, %d expansions",
			(int)m_PfFullInputs.size(), m_PfANodes.size(), m_PfAExpandCount);
		return;
	}

	// result == -1: no complete path. Reconstruct the best partial branch
	// (lowest H = closest to a finish) so Play still works and the user sees
	// how far the search got, instead of a dead "Finish" button.
	{
		int bestIdx = -1;
		float bestH = 1e18f;
		int bestCleanIdx = -1;
		float bestCleanH = 1e18f;
		bool allowFrozen = PfEffortFreezeSupport();
		for(size_t i = 0; i < m_PfANodes.size(); i++)
		{
			if(m_PfANodes[i].ParentIdx < 0)
				continue;
			if(m_PfANodes[i].H < bestH)
			{
				bestH = m_PfANodes[i].H;
				bestIdx = (int)i;
			}
			if(!allowFrozen && m_PfANodes[i].FreezeChainTicks <= 0 && m_PfANodes[i].ForbiddenChainTicks <= 0 && m_PfANodes[i].H < bestCleanH)
			{
				bestCleanH = m_PfANodes[i].H;
				bestCleanIdx = (int)i;
			}
		}
		if(bestCleanIdx >= 0)
			bestIdx = bestCleanIdx;
		else if(!allowFrozen)
			bestIdx = -1;
		if(bestIdx >= 0 && m_PfANodes[bestIdx].ForbiddenChainTicks > 0)
			bestIdx = -1;
		if(bestIdx >= 0)
		{
			m_PfAGoalIdx = bestIdx;
			PfAStarReconstruct();
			if(!m_PfFullInputs.empty())
			{
				m_PfAPathReady = true;
				m_PfFullInputsIdx = 0;
				dbg_msg("pathfinder", "no complete path — showing best partial (%d ticks, %zu nodes, %d expansions)",
					(int)m_PfFullInputs.size(), m_PfANodes.size(), m_PfAExpandCount);
			}
			else
				dbg_msg("pathfinder", "no path found (%d expansions, %zu nodes)", m_PfAExpandCount, m_PfANodes.size());
		}
		else
			dbg_msg("pathfinder", "no path found (%d expansions, %zu nodes)", m_PfAExpandCount, m_PfANodes.size());
	}
	m_PfState = PF_STATE_FINISHED;
}

void CBotNet::PfGetUiStats(bool &outRunning, bool &outFinished, float &outPercent,
	int &outDoneTiles, int &outTotalTiles, double &outElapsedSec, double &outEtaSec,
	double &outPathTimeSec, double &outGenTimeSec)
{
	outRunning = (m_PfState == PF_STATE_RUNNING);
	outFinished = (m_PfState == PF_STATE_FINISHED);
	outPercent = 0.0f;
	outDoneTiles = 0;
	outTotalTiles = 0;
	outElapsedSec = 0.0;
	outEtaSec = -1.0;
	outPathTimeSec = 0.0;
	outGenTimeSec = 0.0;

	if(outRunning && m_PfGenLastState != PF_STATE_RUNNING)
	{
		m_PfGenStartTime = PfNowSec();
		m_PfGenEndTime = 0.0;
	}
	if(m_PfGenLastState == PF_STATE_RUNNING && m_PfState == PF_STATE_FINISHED && m_PfGenEndTime <= 0.0)
		m_PfGenEndTime = PfNowSec();
	m_PfGenLastState = m_PfState;

	if(outRunning)
	{
		outElapsedSec = (m_PfGenStartTime > 0.0) ? PfNowSec() - m_PfGenStartTime : 0.0;
		if(outElapsedSec < 0.0)
			outElapsedSec = 0.0;

		int flowTotal = 0;
		int flowDone = 0;
		bool flowValid = false;
		bool probeLive = false;
		vec2 probePos = m_PfCurPos;
		{
			std::lock_guard<std::mutex> lk(m_PfDataMutex);
			if(m_PfAPathReady)
			{
				size_t i = m_PfFullInputsIdx;
				if(i > 0)
					i--;
				if(i < m_PfVPath.size())
				{
					probePos = m_PfVPath[i];
					probeLive = true;
				}
			}
			else if(m_MapGridLoaded && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
			{
				for(size_t i = m_PfVPath.size(); i-- > 0;)
				{
					vec2 p = m_PfVPath[i];
					int fx = pf_clamp((int)(p.x / 32.0f), 0, m_MapWidth - 1);
					int fy = pf_clamp((int)(p.y / 32.0f), 0, m_MapHeight - 1);
					if(m_PfFlowField[fy * m_MapWidth + fx] < 1e17f)
					{
						probePos = p;
						probeLive = true;
						break;
					}
				}
			}
		}
		if(m_MapGridLoaded && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
		{
			auto fnDist = [&](const vec2 &pos) -> float {
				int tx = pf_clamp((int)(pos.x / 32.0f), 0, m_MapWidth - 1);
				int ty = pf_clamp((int)(pos.y / 32.0f), 0, m_MapHeight - 1);
				return m_PfFlowField[ty * m_MapWidth + tx];
			};
			float dStart = fnDist(m_PfStartPos);
			float dCur = fnDist(probePos);
			if(probeLive && dStart < 1e17f && dStart > 0.001f && dCur < 1e17f)
			{
				float fDone = dStart - dCur;
				if(fDone < 0.0f)
					fDone = 0.0f;
				if(fDone > dStart)
					fDone = dStart;
				flowTotal = (int)(dStart + 0.5f);
				flowDone = (int)(fDone + 0.5f);
				m_PfUiBestTotal = flowTotal;
				m_PfUiBestDone = flowDone;
				flowValid = true;
			}
		}

		if(flowValid)
		{
			outTotalTiles = flowTotal;
			outDoneTiles = flowDone;
			if(flowTotal > 0)
			{
				outPercent = 100.0f * (float)flowDone / (float)flowTotal;
				if(outPercent > 100.0f)
					outPercent = 100.0f;
			}
		}
		else if(m_PfUiBestTotal > 0)
		{
			outTotalTiles = m_PfUiBestTotal;
			outDoneTiles = m_PfUiBestDone;
			outPercent = 100.0f * (float)m_PfUiBestDone / (float)m_PfUiBestTotal;
			if(outPercent > 100.0f)
				outPercent = 100.0f;
		}
		else
		{
			float hRoot = m_PfAHRoot;
			float hBest = m_PfABestH.load(std::memory_order_relaxed);
			if(hRoot > 0.001f && hRoot < 1e17f && hBest < hRoot)
			{
				float frac = (hRoot - hBest) / hRoot;
				if(frac < 0.0f)
					frac = 0.0f;
				outPercent = 100.0f * frac;
				if(outPercent > 95.0f)
					outPercent = 95.0f;
				outTotalTiles = flowTotal;
				outDoneTiles = (int)((float)flowTotal * frac + 0.5f);
			}
		}

		if(outPercent >= 0.5f && outElapsedSec > 0.05)
			outEtaSec = outElapsedSec * (100.0f - outPercent) / outPercent;
	}

	if(outFinished)
	{
		outPathTimeSec = (double)m_PfFullInputs.size() / 50.0;
		if(m_PfGenStartTime > 0.0)
		{
			double endTime = (m_PfGenEndTime > m_PfGenStartTime) ? m_PfGenEndTime : PfNowSec();
			outGenTimeSec = endTime - m_PfGenStartTime;
			if(outGenTimeSec < 0.0)
				outGenTimeSec = 0.0;
		}
	}
}

// Try an alternative candidate from the backtrack buffer (legacy chunk loop).
bool CBotNet::PfBacktrack()
{
	while(m_PfBacktrackIdx > 0)
	{
		m_PfBacktrackIdx--;
		PfBacktrackEntry &entry = m_PfBacktrack[m_PfBacktrackIdx];

		int nextIdx = entry.usedCandidateIdx + 1;
		if(nextIdx >= entry.numCandidates || nextIdx >= (int)entry.candidates.size())
			continue; // no more candidates at this level, pop further

		m_PfCurPos = entry.pos;
		m_PfCurVel = entry.vel;
		m_PfCurHookPos = entry.hookPos;
		m_PfCurHookDir = entry.hookDir;
		m_PfCurHookState = entry.hookState;
		m_PfCurHookTick = entry.hookTick;
		m_PfCurJumped = entry.jumped;
		m_PfCurFreezeTime = entry.freezeTime;
		m_PfTotalFreezeTicks = entry.totalFreezeTicks;

		m_PfVPath.resize(entry.vPathSize);
		m_PfChunkCount = entry.chunkCount;

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
		m_PfBacktrackIdx++;

		dbg_msg("pathfinder", "backtrack: using candidate %d at depth %d", nextIdx, m_PfBacktrackIdx - 1);
		return true;
	}

	return false;
}

// =========================================================
// TILE EDITOR — forbidden zones + custom finish tiles
// =========================================================

bool CBotNet::PfTileCanBeCustomFinish(int tx, int ty)
{
	if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
		return false;
	int idx = ty * m_MapWidth + tx;
	auto fnOk = [](unsigned char t) {
		return t == TILE_AIR || t == TILE_FINISH || t == TILE_FREEZE || t == TILE_DFREEZE ||
			t == TILE_LFREEZE || t == TILE_UNFREEZE || t == TILE_DUNFREEZE || t == TILE_LUNFREEZE;
	};
	return fnOk(m_pMapGrid[idx]) && fnOk(m_pFrontGrid[idx]);
}

void CBotNet::PfEditorBeginChange()
{
	if(m_PfState == PF_STATE_RUNNING)
		PfThreadStop();
}

void CBotNet::PfEditorEndChange()
{
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
	if(m_PfState == PF_STATE_RUNNING)
		PfResetRun();
}

void CBotNet::PfAddForbiddenTile(int tx, int ty)
{
	if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight || !m_pForbiddenGrid)
		return;
	int idx = ty * m_MapWidth + tx;
	if(m_pForbiddenGrid[idx])
		return;
	PfEditorBeginChange();
	if(m_pCustomFinishGrid && m_pCustomFinishGrid[idx])
	{
		m_pCustomFinishGrid[idx] = 0;
		vec2 c(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f);
		for(size_t i = 0; i < m_PfCustomFinishTiles.size(); i++)
		{
			if(m_PfCustomFinishTiles[i].x == c.x && m_PfCustomFinishTiles[i].y == c.y)
			{
				m_PfCustomFinishTiles.erase(m_PfCustomFinishTiles.begin() + i);
				break;
			}
		}
	}
	m_pForbiddenGrid[idx] = 1;
	m_PfForbiddenTiles.push_back(vec2(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f));
	PfEditorEndChange();
}

void CBotNet::PfRemoveForbiddenTile(int tx, int ty)
{
	if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight || !m_pForbiddenGrid)
		return;
	int idx = ty * m_MapWidth + tx;
	if(!m_pForbiddenGrid[idx])
		return;
	PfEditorBeginChange();
	m_pForbiddenGrid[idx] = 0;
	vec2 c(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f);
	for(size_t i = 0; i < m_PfForbiddenTiles.size(); i++)
	{
		if(m_PfForbiddenTiles[i].x == c.x && m_PfForbiddenTiles[i].y == c.y)
		{
			m_PfForbiddenTiles.erase(m_PfForbiddenTiles.begin() + i);
			break;
		}
	}
	PfEditorEndChange();
}

void CBotNet::PfAddCustomFinishTile(int tx, int ty)
{
	if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight || !m_pCustomFinishGrid)
		return;
	int idx = ty * m_MapWidth + tx;
	if(m_pCustomFinishGrid[idx])
		return;
	PfEditorBeginChange();
	if(m_pForbiddenGrid && m_pForbiddenGrid[idx])
	{
		m_pForbiddenGrid[idx] = 0;
		vec2 c(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f);
		for(size_t i = 0; i < m_PfForbiddenTiles.size(); i++)
		{
			if(m_PfForbiddenTiles[i].x == c.x && m_PfForbiddenTiles[i].y == c.y)
			{
				m_PfForbiddenTiles.erase(m_PfForbiddenTiles.begin() + i);
				break;
			}
		}
	}
	m_pCustomFinishGrid[idx] = 1;
	m_PfCustomFinishTiles.push_back(vec2(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f));
	PfEditorEndChange();
}

void CBotNet::PfRemoveCustomFinishTile(int tx, int ty)
{
	if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight || !m_pCustomFinishGrid)
		return;
	int idx = ty * m_MapWidth + tx;
	if(!m_pCustomFinishGrid[idx])
		return;
	PfEditorBeginChange();
	m_pCustomFinishGrid[idx] = 0;
	vec2 c(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f);
	for(size_t i = 0; i < m_PfCustomFinishTiles.size(); i++)
	{
		if(m_PfCustomFinishTiles[i].x == c.x && m_PfCustomFinishTiles[i].y == c.y)
		{
			m_PfCustomFinishTiles.erase(m_PfCustomFinishTiles.begin() + i);
			break;
		}
	}
	PfEditorEndChange();
}

void CBotNet::PfClearForbiddenTiles()
{
	if(m_PfForbiddenTiles.empty() || !m_pForbiddenGrid)
		return;
	PfEditorBeginChange();
	mem_zero(m_pForbiddenGrid, (size_t)m_MapWidth * m_MapHeight);
	m_PfForbiddenTiles.clear();
	PfEditorEndChange();
}

void CBotNet::PfClearCustomFinishTiles()
{
	if(m_PfCustomFinishTiles.empty() || !m_pCustomFinishGrid)
		return;
	PfEditorBeginChange();
	mem_zero(m_pCustomFinishGrid, (size_t)m_MapWidth * m_MapHeight);
	m_PfCustomFinishTiles.clear();
	PfEditorEndChange();
}

void CBotNet::UpdatePfTileEditor()
{
	if(m_PfEditorMode == 0)
	{
		m_PfEditCleaned = false;
		return;
	}
	CGameClient *pGame = GameClient();
	if(pGame && !m_PfEditCleaned)
	{
		CNetObj_PlayerInput &In = pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
		In.m_Fire &= ~1;
		In.m_Hook = 0;
		m_PfEditCleaned = true;
	}
	if(g_Config.m_ClClickGui != 0)
		return;
	if(!pGame || pGame->m_Menus.IsActive())
		return;
	bool lmb = Input()->KeyIsPressed(KEY_MOUSE_1) || m_PfEditFireHeld;
	bool rmb = Input()->KeyIsPressed(KEY_MOUSE_2) || m_PfEditHookHeld;
	if(!m_MapGridLoaded)
		LoadMapGrid();
	if(!m_MapGridLoaded || !m_pForbiddenGrid || !m_pCustomFinishGrid)
		return;
	vec2 cam = pGame->m_Camera.m_Center;
	vec2 aim = cam + (pGame->m_Controls.m_aTargetPos[g_Config.m_ClDummy] - cam) * pGame->m_Camera.m_Zoom;
	int tx = (int)floorf(aim.x / 32.0f);
	int ty = (int)floorf(aim.y / 32.0f);
	if(tx < 0 || ty < 0 || tx >= m_MapWidth || ty >= m_MapHeight)
		return;
	if(m_PfEditorMode == 1)
	{
		if(lmb && !rmb)
			PfAddForbiddenTile(tx, ty);
		else if(rmb && !lmb)
			PfRemoveForbiddenTile(tx, ty);
	}
	else if(m_PfEditorMode == 2)
	{
		if(rmb && !lmb)
			PfRemoveCustomFinishTile(tx, ty);
		else if(lmb && !rmb && PfTileCanBeCustomFinish(tx, ty))
			PfAddCustomFinishTile(tx, ty);
	}
}

void CBotNet::RenderPfTileEditor()
{
	if(!m_MapGridLoaded)
		return;
	if(m_PfEditorMode == 0 && m_PfForbiddenTiles.empty() && m_PfCustomFinishTiles.empty())
		return;
	CGameClient *pGame = GameClient();
	if(!pGame)
		return;
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
	float alpha = KxLineAlpha(KX_LINE_PATHFINDER);
	int lineSize = KxLineSize(KX_LINE_PATHFINDER);
	float halfWidth = 0.5f + (float)(lineSize - 1) * 0.25f;
	vec2 cam = pGame->m_Camera.m_Center;
	float zoom = pGame->m_Camera.m_Zoom;
	float viewHalfW = zoom * (float)Graphics()->ScreenWidth() * 0.5f + 64.0f;
	float viewHalfH = zoom * (float)Graphics()->ScreenHeight() * 0.5f + 64.0f;
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0.85f, 0.15f, 0.15f, alpha * 0.5f);
	for(const vec2 &t : m_PfForbiddenTiles)
	{
		if(t.x < cam.x - viewHalfW || t.x > cam.x + viewHalfW || t.y < cam.y - viewHalfH || t.y > cam.y + viewHalfH)
			continue;
		IGraphics::CFreeformItem Quad(
			t.x - 16.0f, t.y - 16.0f, t.x + 16.0f, t.y - 16.0f,
			t.x - 16.0f, t.y + 16.0f, t.x + 16.0f, t.y + 16.0f);
		Graphics()->QuadsDrawFreeform(&Quad, 1);
	}
	Graphics()->SetColor(0.20f, 0.85f, 0.30f, alpha * 0.5f);
	for(const vec2 &t : m_PfCustomFinishTiles)
	{
		if(t.x < cam.x - viewHalfW || t.x > cam.x + viewHalfW || t.y < cam.y - viewHalfH || t.y > cam.y + viewHalfH)
			continue;
		IGraphics::CFreeformItem Quad(
			t.x - 16.0f, t.y - 16.0f, t.x + 16.0f, t.y - 16.0f,
			t.x - 16.0f, t.y + 16.0f, t.x + 16.0f, t.y + 16.0f);
		Graphics()->QuadsDrawFreeform(&Quad, 1);
	}
	Graphics()->QuadsEnd();
	if(m_PfEditorMode != 0)
	{
		vec2 aim = cam + (pGame->m_Controls.m_aTargetPos[g_Config.m_ClDummy] - cam) * zoom;
		int tx = (int)floorf(aim.x / 32.0f);
		int ty = (int)floorf(aim.y / 32.0f);
		if(tx >= 0 && ty >= 0 && tx < m_MapWidth && ty < m_MapHeight &&
			(m_PfEditorMode == 1 || PfTileCanBeCustomFinish(tx, ty)))
		{
			float x0 = tx * 32.0f;
			float y0 = ty * 32.0f;
			float x1 = x0 + 32.0f;
			float y1 = y0 + 32.0f;
			float r = (m_PfEditorMode == 1) ? 0.95f : 0.25f;
			float g = (m_PfEditorMode == 1) ? 0.20f : 0.95f;
			float b = (m_PfEditorMode == 1) ? 0.20f : 0.35f;
			if(lineSize > 0)
			{
				Graphics()->QuadsBegin();
				Graphics()->SetColor(r, g, b, alpha);
				IGraphics::CFreeformItem E0(x0 - halfWidth, y0 - halfWidth, x1 + halfWidth, y0 - halfWidth, x0 - halfWidth, y0 + halfWidth, x1 + halfWidth, y0 + halfWidth);
				Graphics()->QuadsDrawFreeform(&E0, 1);
				IGraphics::CFreeformItem E1(x0 - halfWidth, y1 - halfWidth, x1 + halfWidth, y1 - halfWidth, x0 - halfWidth, y1 + halfWidth, x1 + halfWidth, y1 + halfWidth);
				Graphics()->QuadsDrawFreeform(&E1, 1);
				IGraphics::CFreeformItem E2(x0 - halfWidth, y0 - halfWidth, x0 + halfWidth, y0 - halfWidth, x0 - halfWidth, y1 + halfWidth, x0 + halfWidth, y1 + halfWidth);
				Graphics()->QuadsDrawFreeform(&E2, 1);
				IGraphics::CFreeformItem E3(x1 - halfWidth, y0 - halfWidth, x1 + halfWidth, y0 - halfWidth, x1 - halfWidth, y1 + halfWidth, x1 + halfWidth, y1 + halfWidth);
				Graphics()->QuadsDrawFreeform(&E3, 1);
				Graphics()->QuadsEnd();
			}
			else
			{
				Graphics()->LinesBegin();
				Graphics()->SetColor(r, g, b, alpha);
				IGraphics::CLineItem L0(x0, y0, x1, y0);
				IGraphics::CLineItem L1(x1, y0, x1, y1);
				IGraphics::CLineItem L2(x1, y1, x0, y1);
				IGraphics::CLineItem L3(x0, y1, x0, y0);
				Graphics()->LinesDraw(&L0, 1);
				Graphics()->LinesDraw(&L1, 1);
				Graphics()->LinesDraw(&L2, 1);
				Graphics()->LinesDraw(&L3, 1);
				Graphics()->LinesEnd();
			}
		}
	}
}

// =========================================================
// PATH RENDERING
// =========================================================

void CBotNet::RenderPathfinderPath()
{
	{
		std::unique_lock<std::mutex> lock(m_PfDataMutex, std::try_to_lock);
		if(lock.owns_lock())
		{
			m_PfVPathRender = m_PfVPath;
			m_PfVHookSegsRender = m_PfVHookSegs;
		}
	}
	std::vector<vec2> &vPath = m_PfVPathRender;
	std::vector<PfHookSeg> &vHookSegs = m_PfVHookSegsRender;
	if(vPath.empty())
		return;

	CGameClient *pGame = GameClient();
	if(!pGame)
		return;

	const bool tasHasRun = GameClient()->m_Tas.HasRun(m_PfFullInputs);

	// CBotNet renders before the map renderer sets up the world projection —
	// set it explicitly so the trajectory lands in world space.
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

	Graphics()->TextureClear();

	ColorRGBA BaseColor = ColorRGBA(KxLineColor(KX_LINE_PATHFINDER), true);
	float ConfigAlpha = KxLineAlpha(KX_LINE_PATHFINDER);
	int LineSize = KxLineSize(KX_LINE_PATHFINDER);

	// Only the start point exists — draw a small marker so the user sees the
	// pathfinder is active but has not produced a chunk yet.
	if(vPath.size() == 1)
	{
		Graphics()->QuadsBegin();
		Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha);
		vec2 p = vPath[0];
		float s = 8.0f;
		IGraphics::CFreeformItem Quad(
			p.x - s, p.y - s, p.x + s, p.y - s,
			p.x - s, p.y + s, p.x + s, p.y + s);
		Graphics()->QuadsDrawFreeform(&Quad, 1);
		Graphics()->QuadsEnd();
		return;
	}

	// Flow field arrows (vector arrows pointing toward the finish).
	if(g_Config.m_KxPfShowField && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
	{
		float halfTilesW = pGame->m_Camera.m_Zoom * (float)Graphics()->ScreenWidth() / 64.0f + 1.0f;
		float halfTilesH = pGame->m_Camera.m_Zoom * (float)Graphics()->ScreenHeight() / 64.0f + 1.0f;
		int camTX = (int)(pGame->m_Camera.m_Center.x / 32.0f);
		int camTY = (int)(pGame->m_Camera.m_Center.y / 32.0f);
		int tx0 = pf_clamp(camTX - (int)halfTilesW, 0, m_MapWidth - 1);
		int ty0 = pf_clamp(camTY - (int)halfTilesH, 0, m_MapHeight - 1);
		int tx1 = pf_clamp(camTX + (int)halfTilesW, 0, m_MapWidth - 1);
		int ty1 = pf_clamp(camTY + (int)halfTilesH, 0, m_MapHeight - 1);
		Graphics()->LinesBegin();
		Graphics()->SetColor(BaseColor.r, BaseColor.g, BaseColor.b, ConfigAlpha * 0.3f);
		for(int ty = ty0; ty <= ty1; ty++)
		{
			for(int tx = tx0; tx <= tx1; tx++)
			{
				float fd = m_PfFlowField[ty * m_MapWidth + tx];
				if(fd >= 1e17f || fd == 0.0f)
					continue;

				// Gradient direction (360°). Unreachable neighbors are treated as
				// equal so walls don't create a spurious away-from-wall component.
				float fdx_m = (tx > 0) ? m_PfFlowField[ty * m_MapWidth + (tx - 1)] : fd;
				float fdx_p = (tx < m_MapWidth - 1) ? m_PfFlowField[ty * m_MapWidth + (tx + 1)] : fd;
				float fdy_m = (ty > 0) ? m_PfFlowField[(ty - 1) * m_MapWidth + tx] : fd;
				float fdy_p = (ty < m_MapHeight - 1) ? m_PfFlowField[(ty + 1) * m_MapWidth + tx] : fd;
				if(fdx_m >= 1e17f) fdx_m = fd;
				if(fdx_p >= 1e17f) fdx_p = fd;
				if(fdy_m >= 1e17f) fdy_m = fd;
				if(fdy_p >= 1e17f) fdy_p = fd;

				float gx = fdx_m - fdx_p;
				float gy = fdy_m - fdy_p;
				float glen = sqrtf(gx * gx + gy * gy);
				if(glen < 0.001f)
					continue;
				vec2 dir = vec2(gx / glen, gy / glen);

				float cx = tx * 32.0f + 16.0f;
				float cy = ty * 32.0f + 16.0f;
				float half = 6.0f;
				vec2 start = vec2(cx, cy) - dir * half;
				vec2 end = vec2(cx, cy) + dir * half;
				vec2 perp = vec2(-dir.y, dir.x) * 4.0f;
				vec2 arrowBack = end - dir * 5.0f;

				IGraphics::CLineItem Main(start.x, start.y, end.x, end.y);
				Graphics()->LinesDraw(&Main, 1);
				IGraphics::CLineItem A1(end.x, end.y, arrowBack.x + perp.x, arrowBack.y + perp.y);
				Graphics()->LinesDraw(&A1, 1);
				IGraphics::CLineItem A2(end.x, end.y, arrowBack.x - perp.x, arrowBack.y - perp.y);
				Graphics()->LinesDraw(&A2, 1);
			}
		}
		Graphics()->LinesEnd();
	}

	// Other branches (semi-transparent). Snapshot the segments under the
	// data mutex — the worker appends to m_PfANodes while the search runs.
	if(g_Config.m_KxPfShowBranches)
	{
		float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
		{
			std::unique_lock<std::mutex> lock(m_PfDataMutex, std::try_to_lock);
			if(lock.owns_lock())
			{
				m_PfVBranchSegsRender.clear();
				for(size_t i = 1; i < m_PfANodes.size(); i++)
				{
					AStarNode &node = m_PfANodes[i];
					if(node.ParentIdx < 0)
						continue;
					const AStarNode &parent = m_PfANodes[node.ParentIdx];
					if(node.PrimIdx == 0)
					{
						for(size_t k = 1; k < node.Traj.size(); k++)
							m_PfVBranchSegsRender.emplace_back(node.Traj[k - 1], node.Traj[k]);
					}
					else
						m_PfVBranchSegsRender.emplace_back(parent.Core.Pos, node.Core.Pos);
				}
			}
		}
		std::vector<std::pair<vec2, vec2>> &vBranchSegs = m_PfVBranchSegsRender;
		if(!vBranchSegs.empty())
		{
			if(LineSize > 0)
			{
				std::vector<IGraphics::CFreeformItem> vQuads;
				for(const auto &seg : vBranchSegs)
				{
					vec2 Dir = normalize(seg.second - seg.first);
					vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
					vQuads.emplace_back(
						seg.first.x - Perp.x, seg.first.y - Perp.y,
						seg.first.x + Perp.x, seg.first.y + Perp.y,
						seg.second.x - Perp.x, seg.second.y - Perp.y,
						seg.second.x + Perp.x, seg.second.y + Perp.y);
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
				for(const auto &seg : vBranchSegs)
				{
					IGraphics::CLineItem Line(seg.first, seg.second);
					Graphics()->LinesDraw(&Line, 1);
				}
				Graphics()->LinesEnd();
			}
		}
	}

	// Hook segments (blue lines) captured per tick along the path.
	if(g_Config.m_KxPfShowHooks && !vHookSegs.empty() && !tasHasRun)
	{
		float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
		if(LineSize > 0)
		{
			std::vector<IGraphics::CFreeformItem> vQuads;
			for(const auto &seg : vHookSegs)
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
			Graphics()->QuadsBegin();
			for(size_t i = 0; i < vQuads.size(); i++)
			{
				Graphics()->SetColor(0.2f, 0.4f, 1.0f, ConfigAlpha);
				Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
			}
			Graphics()->QuadsEnd();
		}
		else
		{
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.2f, 0.4f, 1.0f, ConfigAlpha);
			for(const auto &seg : vHookSegs)
			{
				IGraphics::CLineItem Line(seg.teePos, seg.hookPos);
				Graphics()->LinesDraw(&Line, 1);
			}
			Graphics()->LinesEnd();
		}
	}

	// Main path with optional speed gradient (rainbow hue sweep).
	auto fnSpeedColor = [](float speed) -> ColorRGBA {
		const float MAX_SPEED = 30.0f;
		float t = std::clamp(speed / MAX_SPEED, 0.0f, 1.0f);
		float hue = t * 300.0f;
		return color_cast<ColorRGBA>(ColorHSLA(hue / 360.0f, 1.0f, 0.5f, true));
	};

	if(LineSize > 0 && !tasHasRun)
	{
		float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
		Graphics()->QuadsBegin();
		for(size_t i = 1; i < vPath.size(); i++)
		{
			vec2 p0 = vPath[i - 1];
			vec2 p1 = vPath[i];
			vec2 Dir = normalize(p1 - p0);
			vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
			IGraphics::CFreeformItem Quad(
				p0.x - Perp.x, p0.y - Perp.y,
				p0.x + Perp.x, p0.y + Perp.y,
				p1.x - Perp.x, p1.y - Perp.y,
				p1.x + Perp.x, p1.y + Perp.y);

			if(g_Config.m_KxPfShowSpeed)
			{
				float speed0 = (i > 1) ? distance(vPath[i - 2], vPath[i - 1]) : distance(p0, p1);
				float speed1 = distance(p0, p1);
				ColorRGBA c0 = fnSpeedColor(speed0);
				ColorRGBA c1 = fnSpeedColor(speed1);
				Graphics()->SetColor4(
					ColorRGBA(c0.r, c0.g, c0.b, ConfigAlpha),
					ColorRGBA(c0.r, c0.g, c0.b, ConfigAlpha),
					ColorRGBA(c1.r, c1.g, c1.b, ConfigAlpha),
					ColorRGBA(c1.r, c1.g, c1.b, ConfigAlpha));
			}
			else
			{
				ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_PATHFINDER, (int)i - 1), true);
				Graphics()->SetColor(segCol.r, segCol.g, segCol.b, ConfigAlpha);
			}
			Graphics()->QuadsDrawFreeform(&Quad, 1);
		}
		Graphics()->QuadsEnd();
	}
	else if(!tasHasRun)
	{
		Graphics()->LinesBegin();
		for(size_t i = 1; i < vPath.size(); i++)
		{
			if(g_Config.m_KxPfShowSpeed)
			{
				float speed = distance(vPath[i - 1], vPath[i]);
				ColorRGBA c = fnSpeedColor(speed);
				Graphics()->SetColor(c.r, c.g, c.b, ConfigAlpha);
			}
			else
			{
				ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_PATHFINDER, (int)i - 1), true);
				Graphics()->SetColor(segCol.r, segCol.g, segCol.b, ConfigAlpha);
			}
			IGraphics::CLineItem Line(vPath[i - 1], vPath[i]);
			Graphics()->LinesDraw(&Line, 1);
		}
		Graphics()->LinesEnd();
	}

	if(m_PfState == PF_STATE_RUNNING && vPath.size() >= 2)
	{
		vec2 tip = vPath[vPath.size() - 1];
		vec2 Dir = tip - vPath[vPath.size() - 2];
		float Len = length(Dir);
		if(Len > 0.001f)
		{
			Dir /= Len;
			GameClient()->m_Effects.SmokeTrail(tip, Dir * -10.0f, ConfigAlpha, 0.0f);
			if(GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[WEAPON_GRENADE].IsValid())
			{
				static float s_GrenadeTime = 0.0f;
				static float s_GrenadeLastLocalTime = LocalTime();
				s_GrenadeTime += (LocalTime() - s_GrenadeLastLocalTime) * GameClient()->GetAnimationPlaybackSpeed();
				s_GrenadeLastLocalTime = LocalTime();
				Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[WEAPON_GRENADE]);
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, ConfigAlpha);
				Graphics()->QuadsBegin();
				Graphics()->QuadsSetSubset(0, 0, 1, 1);
				Graphics()->QuadsSetRotation(s_GrenadeTime * pi * 2 * 2);
				IGraphics::CQuadItem Quad(tip.x, tip.y, 32.0f, 32.0f);
				Graphics()->QuadsDraw(&Quad, 1);
				Graphics()->QuadsEnd();
				Graphics()->QuadsSetRotation(0);
			}
		}
	}
}

void CBotNet::PfSpectateStart()
{
	m_PfSpecFollow = true;
	m_PfSpecWasInSpec = false;
	m_PfSpecCamInit = false;
	m_PfSpecCamPos = vec2(0.0f, 0.0f);
	if(!GameClient()->m_Snap.m_SpecInfo.m_Active)
		GameClient()->m_Chat.SendChat(0, "/pause");
}

void CBotNet::PfSpectateStop()
{
	PfSpectateReset();
	GameClient()->m_Chat.SendChat(0, "/pause");
}

void CBotNet::PfSpectateReset()
{
	m_PfSpecFollow = false;
	m_PfSpecWasInSpec = false;
	m_PfSpecCamInit = false;
}

bool CBotNet::PfSpectateUpdateCamera(vec2 &outCamPos)
{
	if(!m_PfSpecFollow)
		return false;
	if(m_PfState != PF_STATE_RUNNING)
	{
		PfSpectateReset();
		return false;
	}
	CGameClient *pGame = GameClient();
	if(!pGame)
	{
		PfSpectateReset();
		return false;
	}
	const bool InSpec = pGame->m_Snap.m_SpecInfo.m_Active;
	if(InSpec)
		m_PfSpecWasInSpec = true;
	else if(m_PfSpecWasInSpec)
	{
		PfSpectateReset();
		return false;
	}
	if(!InSpec)
		return false;

	vec2 tip;
	bool hasTip = false;
	{
		std::unique_lock<std::mutex> lock(m_PfDataMutex, std::try_to_lock);
		if(lock.owns_lock() && m_PfVPath.size() >= 2)
		{
			tip = m_PfVPath[m_PfVPath.size() - 1];
			hasTip = true;
		}
	}
	if(hasTip)
	{
		if(!m_PfSpecCamInit)
		{
			m_PfSpecCamPos = tip;
			m_PfSpecCamInit = true;
		}
		else
		{
			const vec2 Delta = tip - m_PfSpecCamPos;
			const float dist = length(Delta);
			float dt = Client()->RenderFrameTime();
			dt = std::clamp(dt, 0.001f, 0.05f);
			const float MaxSpeed = 2500.0f;
			const float EaseRate = 3.0f;
			float Step = dist * (1.0f - std::exp(-EaseRate * dt));
			if(Step > MaxSpeed * dt)
				Step = MaxSpeed * dt;
			if(dist <= Step)
				m_PfSpecCamPos = tip;
			else
				m_PfSpecCamPos += Delta * (Step / dist);
		}
	}
	if(!m_PfSpecCamInit)
		return false;
	outCamPos = m_PfSpecCamPos;
	return true;
}
