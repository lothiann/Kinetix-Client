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
	m_PfLocalID = -1;
	m_PfThreadResult.store(-1);
	m_PfANodes.clear();
	m_PfAOpen.clear();
	m_PfABestG.clear();
	m_PfAGoalIdx = -1;
	m_PfAStarted = false;
	m_PfAPathReady = false;
	m_PfAExpandCount = 0;
	m_PfRHAInitialized = false;
	m_PfRHAPopulation.clear();
	m_PfFullInputs.clear();
	m_PfFullInputsIdx = 0;
	m_PfGoActive = false;
	m_PfGoIdx = 0;
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
	PfComputeFlowField();

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

// kx_pf_play: apply the next path tick to the real player input.
// Called from CControls::SnapInput each tick while m_PfGoActive is true.
// Aim (TargetX/Y) is only applied on the rising edge of m_Hook so Fake Aim
// can coexist with PfGo.
bool CBotNet::ApplyPfGoInput(CNetObj_PlayerInput *pInput)
{
	m_PfGoAimedThisTick = false;
	if(!m_PfGoActive || !pInput)
		return false;
	if(m_PfGoIdx >= m_PfFullInputs.size())
	{
		m_PfGoActive = false;
		dbg_msg("pathfinder", "kx_pf_play: playback finished (%d ticks)", (int)m_PfFullInputs.size());
		return false;
	}
	const CNetObj_PlayerInput &In = m_PfFullInputs[m_PfGoIdx];
	pInput->m_Direction = In.m_Direction;
	pInput->m_Jump = In.m_Jump;
	pInput->m_Hook = In.m_Hook;
	if(In.m_Hook != 0 && m_PfGoPrevHook == 0)
	{
		pInput->m_TargetX = In.m_TargetX;
		pInput->m_TargetY = In.m_TargetY;
		m_PfGoAimOffset = vec2((float)In.m_TargetX, (float)In.m_TargetY);
		m_PfGoAimedThisTick = true;
	}
	m_PfGoPrevHook = In.m_Hook;
	m_PfGoIdx++;
	return true;
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
		TWalk fnWalk, TFreeze fnFreeze, bool freezePassable)
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
			float txm = (x > 0 && state[y * FW + (x - 1)] == 2) ? pField[y * FW + (x - 1)] : 1e18f;
			float txp = (x < FW - 1 && state[y * FW + (x + 1)] == 2) ? pField[y * FW + (x + 1)] : 1e18f;
			float tym = (y > 0 && state[(y - 1) * FW + x] == 2) ? pField[(y - 1) * FW + x] : 1e18f;
			float typ = (y < FH - 1 && state[(y + 1) * FW + x] == 2) ? pField[(y + 1) * FW + x] : 1e18f;
			float a = std::min(txm, txp);
			float b = std::min(tym, typ);
			if(a >= 1e17f && b >= 1e17f) return 1e18f;
			if(a >= 1e17f) return b + 1.0f;
			if(b >= 1e17f) return a + 1.0f;
			float diff = a - b;
			if(diff < 0.0f) diff = -diff;
			if(diff >= 1.0f) return std::min(a, b) + 1.0f;
			return (a + b + std::sqrt(2.0f - diff * diff)) * 0.5f;
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

void CBotNet::PfComputeFlowField()
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

	PfBfsFill(m_PfFlowField, m_MapWidth, m_MapHeight, vSrcTiles, fnTileWalk, fnTileFreeze, false);

	bool phase1ReachedStart = false;
	{
		int startTX = (int)(m_PfStartPos.x / 32.0f);
		int startTY = (int)(m_PfStartPos.y / 32.0f);
		if(startTX >= 0 && startTY >= 0 && startTX < m_MapWidth && startTY < m_MapHeight &&
			(m_PfStartPos.x != 0.0f || m_PfStartPos.y != 0.0f))
			phase1ReachedStart = m_PfFlowField[startTY * m_MapWidth + startTX] < 1e17f;
	}
	if(!phase1ReachedStart)
		PfBfsFill(m_PfFlowField, m_MapWidth, m_MapHeight, vSrcTiles, fnTileWalk, fnTileFreeze, true);

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
		int startSCX = (int)(m_PfStartPos.x / 8.0f);
		int startSCY = (int)(m_PfStartPos.y / 8.0f);
		if(startSCX >= 0 && startSCY >= 0 && startSCX < m_PfScoreFieldW && startSCY < m_PfScoreFieldH &&
			(m_PfStartPos.x != 0.0f || m_PfStartPos.y != 0.0f))
			scorePhase1ReachedStart = m_PfScoreField[startSCY * m_PfScoreFieldW + startSCX] < 1e17f;
	}
	if(!scorePhase1ReachedStart)
		PfBfsFill(m_PfScoreField, m_PfScoreFieldW, m_PfScoreFieldH, vSrcSub, fnSubWalk, fnSubFreeze, true);
}

// Chunk candidate score. Weights are unchanged from the legacy tuner —
// the score only RANKS candidates inside RHEA/beam/sweep, it never enters
// the A* node cost G.
float CBotNet::PfScoreChunk(const vec2 &startPos, const vec2 &endPos, bool reachedFinish, bool died,
	int freezeTicks) const
{
	float score = 0.0f;
	const float INF = 1e17f;

	// Flow-field alignment: cosine between the movement vector and the
	// distance gradient (+1 = toward finish, -1 = away).
	if(PfEffortScoreFlow() && m_PfFlowField && m_MapWidth > 0 && m_MapHeight > 0)
	{
		vec2 move = endPos - startPos;
		float moveLen = sqrtf(move.x * move.x + move.y * move.y);
		if(moveLen > 0.001f)
		{
			int sx = pf_clamp((int)(startPos.x / 32.0f), 0, m_MapWidth - 1);
			int sy = pf_clamp((int)(startPos.y / 32.0f), 0, m_MapHeight - 1);
			float fd = m_PfFlowField[sy * m_MapWidth + sx];
			if(fd < INF)
			{
				float fdx_m = (sx > 0) ? m_PfFlowField[sy * m_MapWidth + (sx - 1)] : fd;
				float fdx_p = (sx < m_MapWidth - 1) ? m_PfFlowField[sy * m_MapWidth + (sx + 1)] : fd;
				float fdy_m = (sy > 0) ? m_PfFlowField[(sy - 1) * m_MapWidth + sx] : fd;
				float fdy_p = (sy < m_MapHeight - 1) ? m_PfFlowField[(sy + 1) * m_MapWidth + sx] : fd;
				if(fdx_m >= INF) fdx_m = fd;
				if(fdx_p >= INF) fdx_p = fd;
				if(fdy_m >= INF) fdy_m = fd;
				if(fdy_p >= INF) fdy_p = fd;
				float gx = fdx_m - fdx_p;
				float gy = fdy_m - fdy_p;
				float gLen = sqrtf(gx * gx + gy * gy);
				if(gLen > 0.001f)
				{
					float cosA = (move.x * gx + move.y * gy) / (moveLen * gLen);
					score += cosA * 10.0f;
				}
			}
		}
	}

	// Distance reduction on the 4x-resolution score field (sub-tile precision).
	if(PfEffortScoreDist() && m_PfScoreField && m_PfScoreFieldW > 0 && m_PfScoreFieldH > 0)
	{
		int sx = pf_clamp((int)(startPos.x / 8.0f), 0, m_PfScoreFieldW - 1);
		int sy = pf_clamp((int)(startPos.y / 8.0f), 0, m_PfScoreFieldH - 1);
		int ex = pf_clamp((int)(endPos.x / 8.0f), 0, m_PfScoreFieldW - 1);
		int ey = pf_clamp((int)(endPos.y / 8.0f), 0, m_PfScoreFieldH - 1);
		float dStart = m_PfScoreField[sy * m_PfScoreFieldW + sx];
		float dEnd = m_PfScoreField[ey * m_PfScoreFieldW + ex];
		if(dStart < INF && dEnd < INF)
			score += (dStart - dEnd) * 10.0f;
		else if(dEnd < INF && dStart >= INF)
			score += 50.0f; // escaped an unreachable cell
		else if(dEnd >= INF && dStart < INF)
			score -= 50.0f; // moved INTO an unreachable cell
	}

	if(reachedFinish)
		score += 1000000.0f;

	if(died && PfEffortFineDeath())
		score -= 10000.0f;

	if(PfEffortFineFreeze())
		score -= freezeTicks * (10.0f + m_PfTotalFreezeTicks / 8.0f);

	if(distance(endPos, startPos) < 2.0f)
		score -= 500.0f;

	return score;
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
		PfComputeFlowField();
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
		int ChunkSize = PfEffortChunkTicks();
		size_t endIdx = m_PfFullInputsIdx + (size_t)ChunkSize;
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
		for(size_t i = 0; i < m_PfANodes.size(); i++)
		{
			if(m_PfANodes[i].ParentIdx < 0)
				continue;
			if(m_PfANodes[i].H < bestH)
			{
				bestH = m_PfANodes[i].H;
				bestIdx = (int)i;
			}
		}
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

// Legacy greedy single-chunk generator (API compatibility — the tab runs
// the threaded physics A* now). Sweeps direction x jumpTick x hook and
// keeps the top-N candidates by PfScoreChunk.
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

	int ChunkSize = PfEffortChunkTicks();
	int HookAngles = PfEffortHookAngles();

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

	// Freeze fast-forward: one chunk of empty input through all freeze ticks.
	if(pBot->m_FreezeTime > 0)
	{
		int freezeTicks = pBot->m_FreezeTime;
		std::vector<CNetObj_PlayerInput> emptyInputs;
		emptyInputs.reserve(freezeTicks);
		for(int t = 0; t < freezeTicks; t++)
		{
			CNetObj_PlayerInput inp;
			mem_zero(&inp, sizeof(inp));
			inp.m_TargetY = -1;
			emptyInputs.push_back(inp);
		}
		PfChunkResult res;
		PfSimulateChunk(&FutureWorld, LocalID, pBot, emptyInputs, StartTick, res, m_pForbiddenGrid, PfCustomFinishSimGrid());
		if(res.died && !res.reachedFinish)
			return false;
		outInputs = res.inputs;
		outTraj = res.traj;
		outReachedFinish = res.reachedFinish;
		outDied = res.died;
		outFreezeTicks = res.freezeTicks;
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

	struct HookRayResult
	{
		float angle;
		bool hasTarget;
	};
	HookRayResult aHookRays[32];
	bool hookIdle = (pBot->GetCore().m_HookState == HOOK_IDLE);
	bool hookActive = (pBot->GetCore().m_HookState == HOOK_FLYING || pBot->GetCore().m_HookState == HOOK_GRABBED);
	if(hookIdle)
	{
		for(int i = 0; i < HookAngles; i++)
		{
			float t = (float)i / (float)(HookAngles - 1 < 1 ? 1 : HookAngles - 1);
			float angle = (-5.0f / 6.0f) * pi + t * (10.0f / 6.0f) * pi;
			aHookRays[i].angle = angle;
			vec2 endPos = pBot->m_Pos + vec2(cosf(angle), sinf(angle)) * hookLength;
			vec2 hitPos;
			int hit = pColl->IntersectLineTeleHook(pBot->m_Pos, endPos, &hitPos, nullptr);
			aHookRays[i].hasTarget = (hit == TILE_SOLID || hit == TILE_TELEINHOOK || (hit > 0 && hit != TILE_NOHOOK && hit != TILE_DEATH));
		}
	}

	static const int aDirs[] = {-1, 0, 1};

	struct CandidateResult
	{
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
	const int dynMax = 3 * (ChunkSize + 1) * (1 + HookAngles);
	const int N = pf_clamp(g_Config.m_KxPfBacktrackCandidates, CConfig::PF_CANDIDATES_MIN, dynMax);
	std::vector<CandidateResult> topCandidates(N);
	for(int i = 0; i < N; i++)
		topCandidates[i].score = -1e18f;

	auto fnSimulateCandidate = [&](const std::vector<CNetObj_PlayerInput> &inSeq) {
		CCharacterCore savedCore = pBot->GetCore();
		vec2 savedPos = pBot->m_Pos;
		int savedFreeze = pBot->m_FreezeTime;

		PfChunkResult res;
		PfSimulateChunk(&FutureWorld, LocalID, pBot, inSeq, StartTick, res, m_pForbiddenGrid, PfCustomFinishSimGrid());

		pBot->SetCore(savedCore);
		pBot->m_Pos = savedPos;
		pBot->m_FreezeTime = savedFreeze;

		if(res.died && !res.reachedFinish)
			return;

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
	};

	auto fnBuildSeq = [&](int dir, int jumpTick, int hookOn, int angleIdx) {
		std::vector<CNetObj_PlayerInput> inSeq;
		inSeq.reserve(ChunkSize);
		vec2 aimRel = vec2(0.0f, -256.0f);
		if(hookOn == 1 && angleIdx >= 0)
			aimRel = vec2(cosf(aHookRays[angleIdx].angle), sinf(aHookRays[angleIdx].angle)) * 256.0f;
		for(int t = 0; t < ChunkSize; t++)
		{
			CNetObj_PlayerInput inp;
			mem_zero(&inp, sizeof(inp));
			inp.m_Direction = dir;
			inp.m_Jump = (t == jumpTick) ? 1 : 0;
			inp.m_Hook = (hookOn == 1) ? 1 : 0;
			if(hookOn == 1 && angleIdx < 0)
			{
				vec2 aim = pBot->GetCore().m_HookPos - pBot->m_Pos;
				inp.m_TargetX = (int)aim.x;
				inp.m_TargetY = (int)aim.y;
			}
			else if(hookOn == 1)
			{
				inp.m_TargetX = (int)aimRel.x;
				inp.m_TargetY = (int)aimRel.y;
			}
			else
			{
				inp.m_TargetX = dir * 256;
				inp.m_TargetY = -256;
			}
			inSeq.push_back(inp);
		}
		return inSeq;
	};

	for(int dirIdx = 0; dirIdx < 3; dirIdx++)
	{
		int dir = aDirs[dirIdx];
		for(int jumpTick = -1; jumpTick < ChunkSize; jumpTick++)
		{
			fnSimulateCandidate(fnBuildSeq(dir, jumpTick, 0, -1));
			if(hookActive)
				fnSimulateCandidate(fnBuildSeq(dir, jumpTick, 1, -1));
			if(hookIdle)
			{
				for(int angleIdx = 0; angleIdx < HookAngles; angleIdx++)
				{
					if(!aHookRays[angleIdx].hasTarget)
						continue;
					fnSimulateCandidate(fnBuildSeq(dir, jumpTick, 1, angleIdx));
				}
			}
		}
	}

	if(topCandidates[0].score <= -1e17f)
		return false; // no valid candidates

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
		return;
	if(g_Config.m_ClClickGui != 0)
		return;
	CGameClient *pGame = GameClient();
	if(!pGame || pGame->m_Menus.IsActive())
		return;
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
	bool lmb = Input()->KeyIsPressed(KEY_MOUSE_1);
	bool rmb = Input()->KeyIsPressed(KEY_MOUSE_2);
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
	std::vector<vec2> vPath;
	std::vector<PfHookSeg> vHookSegs;
	{
		std::lock_guard<std::mutex> lock(m_PfDataMutex);
		vPath = m_PfVPath;
		vHookSegs = m_PfVHookSegs;
	}
	if(vPath.empty())
		return;

	CGameClient *pGame = GameClient();
	if(!pGame)
		return;

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
		std::vector<std::pair<vec2, vec2>> vBranchSegs;
		{
			std::lock_guard<std::mutex> lock(m_PfDataMutex);
			for(size_t i = 1; i < m_PfANodes.size(); i++)
			{
				AStarNode &node = m_PfANodes[i];
				if(node.ParentIdx < 0 || node.Traj.size() < 2)
					continue;
				for(size_t k = 1; k < node.Traj.size(); k++)
					vBranchSegs.emplace_back(node.Traj[k - 1], node.Traj[k]);
			}
		}
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
	if(g_Config.m_KxPfShowHooks && !vHookSegs.empty())
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

	if(LineSize > 0)
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
	else
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
}
