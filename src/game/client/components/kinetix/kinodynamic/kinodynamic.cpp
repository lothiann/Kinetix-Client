#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

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
// Top-N candidates (N = KxPfBacktrackCandidates, default 100) are kept for the
// backtracking buffer.
//
// Per candidate: save pBot state (core + pos + m_FreezeTime), simulate via
// PfSimulateChunk, restore.  This is the v1.48 (5c28aac) fix — without restoring
// m_FreezeTime, freeze state leaks across candidates.

void CBotNet::ComputeKinodynamic(int Dummy, const vec2 &MyPos, int botTX, int botTY, int targetTX, int targetTY, CBotNetDummy &State)
{
        // Preconditions: m_pfDist must be computed by ComputePathfinder already
        if(!m_MapGridLoaded || !m_pfDist)
                return;
        if(botTX < 0 || botTY < 0 || botTX >= m_MapWidth || botTY >= m_MapHeight)
                return;

        CGameClient *pGame = GameClient();

        int maxNodes = pf_clamp(g_Config.m_KxKinoCandidates, 100, LATTICE_MAX_NODES);
        int maxDepth = pf_clamp(g_Config.m_KxKinoTicks, 3, LATTICE_MAX_DEPTH);
        int hookAngles = pf_clamp(g_Config.m_KxKinoHookAngles, 4, 32);

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
                else if(g_Config.m_KxKinoAggressive)
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
