#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

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
        m_aLastMapName[0] = '\0';

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
        if(m_PfScoreField) delete[] m_PfScoreField;
}

// =========================================================
// ON CONSOLE INIT
// =========================================================

void CBotNet::OnConsoleInit()
{
        Console()->Register("kx_random_aim", "i[on] ?i[ms]", CFGFLAG_CLIENT, ConRandomAim, this, "Random Aim");
        Console()->Register("kx_copy_moves", "i[target_id]", CFGFLAG_CLIENT, ConCopyMoves, this, "Copy Moves");
        Console()->Register("kx_attack", "i[on]", CFGFLAG_CLIENT, ConAttackEnable, this, "Attack Mode");
        Console()->Register("kx_main", "i[id]", CFGFLAG_CLIENT, ConSetMain, this, "Main ID");
        Console()->Register("kx_targets", "s[ids]", CFGFLAG_CLIENT, ConSetTargets, this, "Targets");
        Console()->Register("kx_bots", "s[ids]", CFGFLAG_CLIENT, ConSetBots, this, "Bots (allies)");
        Console()->Register("kx_target_all", "i[on]", CFGFLAG_CLIENT, ConSetTargetAll, this, "Attack all (targets become blacklist)");
        Console()->Register("kx_atk_set", "iiiiiiiiiiiiiiii", CFGFLAG_CLIENT, ConAttackSettings, this, "Settings (16 params: aim,fire,hook,move,stand,rescue,rescueAll,smartDetect,smartRescue,killFrz,atkMain,hammer,simPlayers,avoidFreeze,pfHook,kinodynamic)");
        Console()->Register("kx_atk_dists", "fffffff", CFGFLAG_CLIENT, ConAttackDists, this, "Radii (fire, hook, rescue, target, main, stand, main stand)");
        Console()->Register("kx_atk_hook_delay", "i[ms]", CFGFLAG_CLIENT, ConAttackHookDelay, this, "Hook Delay");
        Console()->Register("kx_client_delay", "i[ms]", CFGFLAG_CLIENT, ConClientDelay, this, "Client delay in ms (0 to disable)");
        Console()->Register("kx_stand_on_x", "i[on]", CFGFLAG_CLIENT, ConStandOnX, this, "Stand only on X axis");
        Console()->Register("kx_rescue_ids", "s[ids]", CFGFLAG_CLIENT, ConRescueIds, this, "Rescue/Unrescue IDs");
        Console()->Register("kx_atk_pathfinder", "i[on]", CFGFLAG_CLIENT, ConPathfinder, this, "Enable A* pathfinder movement");
        Console()->Register("kx_atk_pathfinder_rays", "i[rays]", CFGFLAG_CLIENT, ConPathfinderRays, this, "Number of raycast rays (12-90)");
        Console()->Register("kx_atk_pathfinder_rays_dist", "i[tiles]", CFGFLAG_CLIENT, ConPathfinderRaysDist, this, "Raycast view radius (1-128 tiles)");
        Console()->Register("kx_atk_pathfinder_snap", "i[on]", CFGFLAG_CLIENT, ConPathfinderSnap, this, "Snap to tile center when flow is vertical");
        Console()->Register("kx_atk_pathfinder_sps", "i[method]", CFGFLAG_CLIENT, ConPathfinderSps, this, "Simulate players method: 0=walls, 1=score(push)");
        Console()->Register("kx_pathfinder_go", "i[on] ?i[x] ?i[y]", CFGFLAG_CLIENT, ConPathfinderGo, this, "Move to position: 0=disable, 1 x y=enable and set target");
        Console()->Register("kx_macro_load", "s[path]", CFGFLAG_CLIENT, ConMacroLoad, this, "Load macro from file");
        Console()->Register("kx_macro_play", "i[on]", CFGFLAG_CLIENT, ConMacroPlay, this, "Play loaded macro (1=start, 0=stop)");
        Console()->Register("kx_macro_record", "i[on]", CFGFLAG_CLIENT, ConMacroRecord, this, "Record macro (1=start, 0=stop)");
        Console()->Register("kx_macro_save", "s[path]", CFGFLAG_CLIENT, ConMacroSave, this, "Save recorded macro to file");
        Console()->Register("kx_macro_capture", "i[id]", CFGFLAG_CLIENT, ConMacroCapture, this, "Set capture ID for macro recording");

        // Individual toggle commands
        Console()->Register("kx_stand", "i[on]", CFGFLAG_CLIENT, ConStand, this, "Stand when close");
        Console()->Register("kx_autoaim", "i[on]", CFGFLAG_CLIENT, ConAutoAim, this, "Auto aim at target");
        Console()->Register("kx_autofire", "i[on]", CFGFLAG_CLIENT, ConAutoFire, this, "Auto fire at target");
        Console()->Register("kx_autohook", "i[on]", CFGFLAG_CLIENT, ConAutoHook, this, "Auto hook target");
        Console()->Register("kx_move", "i[on]", CFGFLAG_CLIENT, ConMove, this, "Move towards target");
        Console()->Register("kx_rescue", "i[on]", CFGFLAG_CLIENT, ConRescue, this, "Rescue frozen allies");
        Console()->Register("kx_kill_frz", "i[on]", CFGFLAG_CLIENT, ConKillFrz, this, "Kill on freeze");
        Console()->Register("kx_atk_main", "i[on]", CFGFLAG_CLIENT, ConAtkMain, this, "Attack main target");
        Console()->Register("kx_auto_main", "i[on]", CFGFLAG_CLIENT, ConAutoMain, this, "Auto-set main to active dummy");
        Console()->Register("kx_hammer", "i[on]", CFGFLAG_CLIENT, ConHammer, this, "Auto hammer");
        Console()->Register("kx_smart_detect", "i[on]", CFGFLAG_CLIENT, ConSmartDetect, this, "Smart detect for rescue");
        Console()->Register("kx_smart_rescue", "i[on]", CFGFLAG_CLIENT, ConSmartRescue, this, "Smart rescue pathfinder");
        Console()->Register("kx_avoid_freeze", "i[on]", CFGFLAG_CLIENT, ConAvoidFreeze, this, "Avoid freeze tiles");
        Console()->Register("kx_pf_hook", "i[on]", CFGFLAG_CLIENT, ConPfHook, this, "Pathfinder hook to walls");
        Console()->Register("kx_kinodynamic", "i[on]", CFGFLAG_CLIENT, ConKinodynamic, this, "Enable Kinodynamic A* movement");
        Console()->Register("kx_kino_candidates", "i[count]", CFGFLAG_CLIENT, ConKinoCandidates, this, "Kinodynamic: A* node budget (100-2000)");
        Console()->Register("kx_kino_ticks", "i[depth]", CFGFLAG_CLIENT, ConKinoTicks, this, "Kinodynamic: max search depth in primitives (3-12)");
        Console()->Register("kx_kino_hook_angles", "i[angles]", CFGFLAG_CLIENT, ConKinoHookAngles, this, "Kinodynamic: number of hook angles (4-32)");
        Console()->Register("kx_kino_cache_ticks", "i[ticks]", CFGFLAG_CLIENT, ConKinoCacheTicks, this, "Kinodynamic: cache duration in ticks (1-128, 128=full plan)");
        Console()->Register("kx_kino_show_path", "i[on]", CFGFLAG_CLIENT, ConKinoShowPath, this, "Kinodynamic: show predicted path");
        Console()->Register("kx_kino_show_field", "i[on]", CFGFLAG_CLIENT, ConKinoShowField, this, "Kinodynamic: show vector field");
        Console()->Register("kx_kino_aggressive", "i[on]", CFGFLAG_CLIENT, ConKinoAggressive, this, "Kinodynamic: force-apply best partial plan even when no primitive improved on start");
        Console()->Register("kx_send", "s[dummyids] r[command]", CFGFLAG_CLIENT, ConSendDummy, this, "Execute command on dummies (-1=all, 0,1,2=specific)");

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker
        Console()->Register("kx_pf_live", "i[state]", CFGFLAG_CLIENT, ConPfLive, this, "Pathfinder tab: 0=idle, 1=running, 2=finished");
        Console()->Register("kx_pf_algorithm", "i[algo]", CFGFLAG_CLIENT, ConPfAlgorithm, this, "Pathfinder tab: algorithm index 0-14");
        Console()->Register("kx_pf_chunk_size", "i[ticks]", CFGFLAG_CLIENT, ConPfChunkSize, this, "Pathfinder tab: ticks per chunk (1-50)");
        Console()->Register("kx_pf_candidates", "i[n]", CFGFLAG_CLIENT, ConPfCandidates, this, "Pathfinder tab: max search nodes (50-2000)");
        Console()->Register("kx_pf_hook_angles", "i[angles]", CFGFLAG_CLIENT, ConPfHookAngles, this, "Pathfinder tab: hook fan-raycast rays (4-32)");
        Console()->Register("kx_pf_horizon", "i[ticks]", CFGFLAG_CLIENT, ConPfHorizon, this, "Pathfinder tab: ticks applied per chunk (1-50)");
        Console()->Register("kx_pf_play", "i[on]", CFGFLAG_CLIENT, ConPfGo, this, "Pathfinder: 1=replay path on real player (needs FINISHED state), 0=stop");
}

// =========================================================
// ENGINE LIFECYCLE (CComponent virtuals)
// =========================================================

// OnReset: called by CGameClient::OnConnected() right after OnMapLoad()
// (gameclient.cpp:810-811), and on full client reset. Disables all active
// botnet modes and resets every dummy's state machine + input buffer.
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

// OnMapLoad (v1.56.164): called by CGameClient::OnConnected() after
// m_Layers.Init() + m_Collision.Init() on EVERY map load (map vote,
// reconnect, demo start). The engine has already swapped in the new map —
// our cached pathfinder map data (grid, flow field, BFS score field,
// finish tiles, A* search state, trajectory) is now stale and must be
// invalidated.
//
// Previously pathfinder only reloaded the grid via MaybeReloadMapGrid()
// (called from dummies/attack) which compared GameLayer W/H. That misses:
//   - same-size map swaps (vote to a different map with identical W*H)
//   - PathfinderGo/Attack branches that don't call MaybeReloadMapGrid
// And even when LoadMapGrid() DID run, it left m_PfFlowField alive
// (fixed in LoadMapGrid itself — see map_grid.cpp).
//
// Here we just flag the grid as unloaded; the next UpdatePathfinder() /
// MaybeReloadMapGrid() / PfResetRun() call will lazy-reload via the
// existing `if(!m_MapGridLoaded) LoadMapGrid();` guards. We also stop
// any in-flight search (its nodes reference old tile coords) and clear
// playback so kx_pf_play can't replay a stale path.
void CBotNet::OnMapLoad()
{
        // Force lazy grid reload on next access. LoadMapGrid() frees the old
        // buffers (including m_PfFlowField/m_PfScoreField — see map_grid.cpp)
        // and re-reads tile data from the freshly-loaded IMap.
        m_MapGridLoaded = false;

        // Abort any in-flight A* search — its nodes reference old tile coords.
        // PfClearSearchState (v1.56.165) releases all A*/RHEA buffers +
        // playback state in one shot (also fixes BUG4 — heavy buffers were
        // previously left alive across the idle period, leading to crash on
        // restart when their vector headers got corrupted by sibling OOB writes).
        m_PfState = PF_STATE_IDLE;
        PfClearSearchState();
        m_PfVPath.clear();

        // m_PfFlowField / m_PfScoreField / m_PfFinishTiles are freed inside
        // LoadMapGrid() on next call; no need to touch them here (and doing
        // so would risk a double-free if LoadMapGrid hasn't run yet).
}

void CBotNet::OnUpdate()
{
        if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        // Pathfinder tab: run one chunk per update if RUNNING.  Placed before the
        // dummy loop so it never interferes with the active dummy's inputs (the
        // tab only simulates + renders; it does NOT write inputs to any dummy).
        UpdatePathfinder();

        // Laser unfreeze: predict freeze and auto-fire laser to unfreeze self.
        // Runs on the active player's input (not dummies).
        UpdateLaserUnfreeze();

        // v1.56.151: Fly Ride — pilot hooks/hammers nearest dummy, WASD moves anchor.
        UpdateFlyRide();

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

        // ── DDNet-compatibility gate ──────────────────────────────────────────
        // DDNet has NO ProcessDummy. Its dummy input pipeline is:
        //   1. controls.cpp SnapInput  (copy_moves / dummy_control for inactive D)
        //   2. gameclient.cpp OnSnapInput  (hammer branch for inactive D)
        //   3. gameclient.cpp OnDummySwap  (fire counter transfer on cl_dummy swap)
        //
        // Kinetix adds ProcessDummy for its OWN botnet features (attack, copy mirror,
        // pathfinder, macro, random aim). When NONE of these are active, running
        // ProcessDummy only causes damage: HandleNoFeatureIdle → ResetAndCommitInput
        // clobbers m_aDummyInput[D].m_Direction/Jump/Hook every frame, which breaks
        // the OnSnapInput skip-release check and delays hammer release sends.
        //
        // Solution: skip the entire ProcessDummy loop when no botnet features are
        // active. This makes the default dummy input pipeline byte-identical to DDNet.
        bool anyBotnetFeature = m_AttackEnabled || m_CopyMoves || m_RandomAim;
        if(!anyBotnetFeature)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(D == g_Config.m_ClDummy)
                                continue;
                        if(D != 0 && !Client()->DummyConnected(D))
                                continue;
                        if(m_aDummies[D].m_MacroPlaying || m_aDummies[D].m_PathfinderGoActive)
                        {
                                anyBotnetFeature = true;
                                break;
                        }
                }
        }
        if(!anyBotnetFeature)
                return;

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

// =========================================================
// PROCESSDUMMY SUB-STEPS (v1.56.32 refactor)
// Each handler corresponds to one early-return block that used to live
// inline in ProcessDummy.  Returns true when it handled the tick (and
// ProcessDummy must return immediately); false = fall through to the
// next stage.  Logic is identical to the inlined originals.
// =========================================================

void CBotNet::OnRender()
{
        // Pathfinder tab: render the accumulated trajectory whenever the state
        // machine has produced a path (RUNNING or FINISHED).  This is INDEPENDENT
        // of m_KinodynamicEnabled so the tab works even when kinodynamic A* is off.
        if(m_PfState != PF_STATE_IDLE)
                RenderPathfinderPath();

        // Laser unfreeze: render successful laser path (Show attempt)
        RenderLaserUnfreezePath();

        // v1.56.159: Fly Ride — render red tile at anchor (debug visual).
        RenderFlyRideAnchor();

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
