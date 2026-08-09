#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// =========================================================
// CONSTRUCTOR / DESTRUCTOR
// =========================================================

CBotNet::CBotNet()
{
        // v1.56.171 BUG9: all kx_ on/off + numeric settings are now MACRO_CONFIG_INT
        // cvars (defaults in config_variables.h). Constructor no longer initializes them.
        for(int i = 0; i < 128; i++)
        {
                m_TargetList[i] = false;
                m_BotsList[i] = false;
                m_RescueList[i] = false;
        }

        // String buffers for UI input fields
        m_aTargetIDsStr[0] = '\0';
        m_aBotsIDsStr[0] = '\0';
        m_aRescueIDsStr[0] = '\0';

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
        // v1.56.171 BUG9: most kx_ commands converted to MACRO_CONFIG_INT cvars.
        // Only complex/temporal commands remain registered here. The cvar-converted
        // commands (kx_attack, kx_aimbot, kx_autoaim, etc.) are now accessible via
        // `toggle kx_attack 1 0` in binds and via direct `kx_attack 1` in console.
        Console()->Register("kx_targets", "s[ids]", CFGFLAG_CLIENT, ConSetTargets, this, "Targets");
        Console()->Register("kx_bots", "s[ids]", CFGFLAG_CLIENT, ConSetBots, this, "Bots (allies)");
        Console()->Register("kx_atk_set", "iiiiiiiiiiiiiiii", CFGFLAG_CLIENT, ConAttackSettings, this, "Settings (16 params: aim,fire,hook,move,stand,rescue,rescueAll,smartDetect,smartRescue,killFrz,atkMain,hammer,simPlayers,avoidFreeze,pfHook,kinodynamic)");
        Console()->Register("kx_atk_dists", "fffffff", CFGFLAG_CLIENT, ConAttackDists, this, "Radii (fire, hook, rescue, target, main, stand, main stand)");
        Console()->Register("kx_rescue_ids", "s[ids]", CFGFLAG_CLIENT, ConRescueIds, this, "Rescue/Unrescue IDs");
        Console()->Register("kx_pathfinder_go", "i[on] ?i[x] ?i[y]", CFGFLAG_CLIENT, ConPathfinderGo, this, "Move to position: 0=disable, 1 x y=enable and set target");
        Console()->Register("kx_macro_load", "s[path]", CFGFLAG_CLIENT, ConMacroLoad, this, "Load macro from file");
        Console()->Register("kx_macro_play", "i[on]", CFGFLAG_CLIENT, ConMacroPlay, this, "Play loaded macro (1=start, 0=stop)");
        Console()->Register("kx_macro_record", "i[on]", CFGFLAG_CLIENT, ConMacroRecord, this, "Record macro (1=start, 0=stop)");
        Console()->Register("kx_macro_save", "s[path]", CFGFLAG_CLIENT, ConMacroSave, this, "Save recorded macro to file");
        Console()->Register("kx_macro_capture", "i[id]", CFGFLAG_CLIENT, ConMacroCapture, this, "Set capture ID for macro recording");
        Console()->Register("kx_send", "s[dummyids] r[command]", CFGFLAG_CLIENT, ConSendDummy, this, "Execute command on dummies (-1=all, 0,1,2=specific)");

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker
        Console()->Register("kx_pf_live", "i[state]", CFGFLAG_CLIENT, ConPfLive, this, "Pathfinder tab: 0=idle, 1=running, 2=finished");
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
        g_Config.m_KxAttack = false;
        g_Config.m_KxCopyMoves = false;
        g_Config.m_KxRandomAim = false;
        g_Config.m_KxKinodynamic = false;
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

        // v1.56.171 BUG9: cvar change-detection. Since kx_ commands are now cvars,
        // there's no Con-handler to run side effects. We poll the cvar each tick
        // and run the side effects (ResetDummyInputs, KinoCache.Reset, etc.) when
        // a value transitions. This replaces the old ConStand/ConAutoAim/... handlers.
        #define KX_CVAR_FALL(prev, cvar) do { \
                int cur = g_Config.cvar; \
                if(prev != cur) { \
                        if(prev && !cur) { for(int D = 0; D < MAX_DUMMIES; D++) ResetDummyInputs(D); } \
                        prev = cur; \
                } } while(0)
        KX_CVAR_FALL(m_PrevKxAttack, m_KxAttack);
        KX_CVAR_FALL(m_PrevKxStand, m_KxStand);
        KX_CVAR_FALL(m_PrevKxAutoAim, m_KxAutoAim);
        KX_CVAR_FALL(m_PrevKxAutoFire, m_KxAutoFire);
        KX_CVAR_FALL(m_PrevKxAutoHook, m_KxAutoHook);
        KX_CVAR_FALL(m_PrevKxMove, m_KxMove);
        KX_CVAR_FALL(m_PrevKxRescue, m_KxRescue);
        KX_CVAR_FALL(m_PrevKxKillFrz, m_KxKillFrz);
        KX_CVAR_FALL(m_PrevKxAtkMain, m_KxAtkMain);
        KX_CVAR_FALL(m_PrevKxHammer, m_KxHammer);
        KX_CVAR_FALL(m_PrevKxSmartDetect, m_KxSmartDetect);
        KX_CVAR_FALL(m_PrevKxSmartRescue, m_KxSmartRescue);
        KX_CVAR_FALL(m_PrevKxAvoidFreeze, m_KxAvoidFreeze);
        KX_CVAR_FALL(m_PrevKxPfHook, m_KxPfHook);
        KX_CVAR_FALL(m_PrevKxCopyMoves, m_KxCopyMoves);
        // Kinodynamic: also reset KinoCache on transition.
        if(m_PrevKxKinodynamic != g_Config.m_KxKinodynamic)
        {
                if(m_PrevKxKinodynamic && !g_Config.m_KxKinodynamic)
                {
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                m_aDummies[D].m_KinoCache.Reset();
                                ResetDummyInputs(D);
                        }
                }
                m_PrevKxKinodynamic = g_Config.m_KxKinodynamic;
        }
        // Pathfinder (kx_atk_pathfinder): reset target cache on any change.
        if(m_PrevKxAtkPathfinder != g_Config.m_KxAtkPathfinder)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        m_aDummies[D].m_LastTargetTX = -1;
                        m_aDummies[D].m_LastTargetTY = -1;
                        m_aDummies[D].m_PathFound = false;
                }
                m_PrevKxAtkPathfinder = g_Config.m_KxAtkPathfinder;
        }
        // Simulate players: reset target cache on any change.
        if(m_PrevKxPfSimulatePlayers != g_Config.m_KxPfSimulatePlayers)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        m_aDummies[D].m_LastTargetTX = -1;
                        m_aDummies[D].m_LastTargetTY = -1;
                        m_aDummies[D].m_PathFound = false;
                }
                m_PrevKxPfSimulatePlayers = g_Config.m_KxPfSimulatePlayers;
        }
        // Hook delay: reset hook tick timer on any change.
        if(m_PrevKxAtkHookDelay != g_Config.m_KxAtkHookDelay)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        m_aDummies[D].m_HookTickTimer = 0;
                m_PrevKxAtkHookDelay = g_Config.m_KxAtkHookDelay;
        }
        // PfHook: reset PfHookTile on any change.
        if(m_PrevKxPfHook != g_Config.m_KxPfHook)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        m_aDummies[D].m_PfHookTile = vec2(0, 0);
                m_PrevKxPfHook = g_Config.m_KxPfHook;
        }
        #undef KX_CVAR_FALL

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
        if(g_Config.m_KxAttack)
        {
                for(int d = 0; d < MAX_DUMMIES; d++)
                {
                        int id = pGame->m_aLocalIds[d];
                        if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
                                m_BotsList[id] = true;
                }
        }

        // Auto main: automatically set main ID to the currently active dummy
        if(g_Config.m_KxAutoMain && g_Config.m_KxAttack)
        {
                int ActiveID = pGame->m_aLocalIds[g_Config.m_ClDummy];
                if(ActiveID >= 0 && g_Config.m_KxMain != ActiveID)
                        g_Config.m_KxMain = ActiveID;
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
        bool anyBotnetFeature = g_Config.m_KxAttack || g_Config.m_KxCopyMoves || g_Config.m_KxRandomAim;
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
        // of g_Config.m_KxKinodynamic so the tab works even when kinodynamic A* is off.
        if(m_PfState != PF_STATE_IDLE)
                RenderPathfinderPath();

        // Laser unfreeze: render successful laser path (Show attempt)
        RenderLaserUnfreezePath();

        // v1.56.159: Fly Ride — render red tile at anchor (debug visual).
        RenderFlyRideAnchor();

        if(!g_Config.m_KxKinodynamic)
                return;

        // Render kinodynamic package trajectory and vector field
        if(g_Config.m_KxKinoShowField)
                RenderVectorField();
        if(g_Config.m_KxKinoShowPath)
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
