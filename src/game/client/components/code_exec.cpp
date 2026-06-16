#include <game/client/components/code_exec.h>
#include <game/client/components/chat.h>

#include <game/client/gameclient.h>
#include <game/gamecore.h>
#include <game/collision.h>
#include <engine/shared/config.h>
#include <base/str.h>
#include <engine/console.h>
#include <engine/client/client.h>
#include <base/vmath.h>

// Lua headers — MUST use lua.hpp (wraps in extern "C") so that
// C++ code uses C linkage when calling Lua API functions.
// Without extern "C", MSVC will mangle the names and LNK2019.
#include <lua.hpp>

// sol2
#include <sol.hpp>

#include <cstdio>
#include <cstring>

// =========================================================
// Lua abort hook — static function, not in class header
// to avoid exposing lua_Debug in the header
// =========================================================
static void LuaAbortHook(lua_State *L, lua_Debug *ar)
{
        (void)ar; // unused
        // Retrieve the CCodeExec instance from Lua registry
        sol::state_view lua(L);
        CCodeExec *pSelf = lua["__code_exec_ptr"].get_or<CCodeExec*>(nullptr);
        if(pSelf && pSelf->m_AbortRequested)
        {
                luaL_error(L, "Script aborted by user");
        }
}

// =========================================================
// Construction / Destruction
// =========================================================
CCodeExec::CCodeExec() :
        m_State(STATE_IDLE),
        m_AbortRequested(false),
        m_pLua(nullptr),
        m_EditorActive(false),
        m_MouseSelecting(false),
        m_CursorPos(0),
        m_SelectionStart(0),
        m_SelectionEnd(0),
        m_ScrollY(0.0f),
        m_ScrollYChange(0.0f),
        m_HighlightParseState(0)
{
        mem_zero(m_aLastError, sizeof(m_aLastError));
        mem_zero(m_aCodeBuffer, sizeof(m_aCodeBuffer));
}

CCodeExec::~CCodeExec()
{
        if(m_pLua)
        {
                delete m_pLua;
                m_pLua = nullptr;
        }
}

// =========================================================
// Component lifecycle
// =========================================================
void CCodeExec::OnConsoleInit()
{
        Console()->Register("c_exec", "s[code]", CFGFLAG_CLIENT, ConExec, this, "Execute Lua code");
        Console()->Register("c_exec_stop", "", CFGFLAG_CLIENT, ConStop, this, "Stop running Lua script");
}

void CCodeExec::OnInit()
{
        // Create Lua state
        m_pLua = new sol::state();

        // Open standard Lua libraries
        m_pLua->open_libraries(
                sol::lib::base,
                sol::lib::string,
                sol::lib::math,
                sol::lib::table,
                sol::lib::coroutine,
                sol::lib::utf8,
                sol::lib::package,
                sol::lib::os,
                sol::lib::io,
                sol::lib::debug
        );

        // Override print to output in chat as chat/lua
        m_pLua->set_function("print", [this](sol::variadic_args va) {
                std::string result;
                for(auto it = va.begin(); it != va.end(); ++it)
                {
                        if(it != va.begin())
                                result += "\t";
                        auto obj = *it;
                        sol::optional<std::string> str = obj.get<sol::optional<std::string>>();
                        if(str)
                                result += *str;
                        else
                        {
                                sol::optional<int> num = obj.get<sol::optional<int>>();
                                if(num)
                                        result += std::to_string(*num);
                                else
                                        result += sol::type_name(m_pLua->lua_state(), obj.get_type());
                        }
                }

                // Output to chat as chat/lua (not chat/client like echo)
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, result.c_str());
        });

        // Register all game bindings
        RegisterBindings();

        // Register raw C++ class access
        RegisterRawAPI();
}

void CCodeExec::OnShutdown()
{
        Stop();
        if(m_pLua)
        {
                delete m_pLua;
                m_pLua = nullptr;
        }
}

void CCodeExec::OnUpdate()
{
        // Nothing to do per-tick currently.
}

void CCodeExec::OnReset()
{
        Stop();

}

// =========================================================
// Getters (implementation in .cpp, not inline in .h)
// =========================================================
const char *CCodeExec::GetLastError() const
{
        return m_aLastError;
}


// =========================================================
// Script Execution
// =========================================================
bool CCodeExec::Execute(const char *pCode)
{
        if(!m_pLua || !pCode)
                return false;

        // Stop any previous execution first
        Stop();

        m_AbortRequested = false;
        m_aLastError[0] = '\0';
        m_State = STATE_RUNNING;

        // Store self pointer in Lua registry so the abort hook can access it
        (*m_pLua)["__code_exec_ptr"] = this;

        // Install debug hook — called every 1000 VM instructions
        // This allows us to abort infinite loops without freezing the client
        lua_sethook(m_pLua->lua_state(), LuaAbortHook, LUA_MASKCOUNT, 1000);

        sol::load_result load = m_pLua->load(pCode);
        if(!load.valid())
        {
                sol::optional<sol::error> maybe_err = load;
                const char *pErrMsg = "Unknown Lua load error";
                if(maybe_err)
                        pErrMsg = maybe_err->what();
                str_copy(m_aLastError, pErrMsg, sizeof(m_aLastError));

                // Output error to chat as chat/lua
                char aErrBuf[1024];
                str_format(aErrBuf, sizeof(aErrBuf), "Lua load error: %s", pErrMsg);
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, aErrBuf);

                m_State = STATE_ERROR;
                lua_sethook(m_pLua->lua_state(), nullptr, 0, 0);
                return false;
        }

        sol::protected_function_result result = load();
        if(!result.valid())
        {
                sol::optional<sol::error> maybe_err = result;
                const char *pErrMsg = "Unknown Lua execution error";
                if(maybe_err)
                        pErrMsg = maybe_err->what();
                str_copy(m_aLastError, pErrMsg, sizeof(m_aLastError));

                // Output error to chat as chat/lua
                char aErrBuf[1024];
                str_format(aErrBuf, sizeof(aErrBuf), "Lua error: %s", pErrMsg);
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, aErrBuf);

                m_State = STATE_ERROR;
                lua_sethook(m_pLua->lua_state(), nullptr, 0, 0);
                return false;
        }

        // Script completed successfully — remove hook
        lua_sethook(m_pLua->lua_state(), nullptr, 0, 0);
        m_State = STATE_IDLE;
        return true;
}

void CCodeExec::Stop()
{
        if(m_State == STATE_RUNNING)
        {
                m_AbortRequested = true;
        }
        m_State = STATE_IDLE;
}



// =========================================================
// Lua Bindings — expose DDNet game state to scripts
// =========================================================
void CCodeExec::RegisterBindings()
{
        if(!m_pLua)
                return;

        auto &lua = *m_pLua;

        // ----- vec2 type -----
        lua.new_usertype<vec2>("vec2",
                sol::constructors<vec2(), vec2(float, float)>(),
                "x", &vec2::x,
                "y", &vec2::y
        );

        // ----- Game state namespace -----
        auto game = lua["game"].get_or_create<sol::table>();

        // game.local_pos() — local player position
        game.set_function("local_pos", [this]() -> vec2 {
                return GameClient()->m_LocalCharacterPos;
        });

        // game.local_vel() — local player velocity
        game.set_function("local_vel", [this]() -> vec2 {
                auto &pc = GameClient()->m_PredictedChar;
                return vec2(pc.m_Vel.x, pc.m_Vel.y);
        });

        // game.tick() — current game tick (CONN_MAIN = 0)
        game.set_function("tick", [this]() -> int {
                return Client()->GameTick(0);
        });

        // game.ticks_per_sec() — tick rate
        game.set_function("ticks_per_sec", [this]() -> int {
                return Client()->GameTickSpeed();
        });

        // game.local_id() — local client ID
        game.set_function("local_id", [this]() -> int {
                return GameClient()->m_Snap.m_LocalClientId;
        });

        // game.num_players() — number of players
        game.set_function("num_players", [this]() -> int {
                return GameClient()->m_Snap.m_NumPlayers;
        });

        // game.player_pos(id) — get player position by ID
        game.set_function("player_pos", [this](int id) -> sol::object {
                if(id < 0 || id >= MAX_CLIENTS || !GameClient()->m_Snap.m_apPlayerInfos[id])
                        return sol::nil;
                auto &info = GameClient()->m_Snap.m_aCharacters[id];
                if(!info.m_Active)
                        return sol::nil;
                return sol::make_object(*m_pLua, vec2(
                        (float)info.m_Cur.m_X / 100.0f,
                        (float)info.m_Cur.m_Y / 100.0f
                ));
        });

        // game.player_vel(id) — get player velocity by ID
        game.set_function("player_vel", [this](int id) -> sol::object {
                if(id < 0 || id >= MAX_CLIENTS || !GameClient()->m_Snap.m_apPlayerInfos[id])
                        return sol::nil;
                auto &info = GameClient()->m_Snap.m_aCharacters[id];
                if(!info.m_Active)
                        return sol::nil;
                return sol::make_object(*m_pLua, vec2(
                        (float)info.m_Cur.m_VelX / 100.0f,
                        (float)info.m_Cur.m_VelY / 100.0f
                ));
        });

        // game.player_name(id) — get player name
        game.set_function("player_name", [this](int id) -> sol::object {
                if(id < 0 || id >= MAX_CLIENTS || !GameClient()->m_Snap.m_apPlayerInfos[id])
                        return sol::nil;
                const char *pName = GameClient()->m_aClients[id].m_aName;
                if(!pName || pName[0] == '\0')
                        return sol::nil;
                return sol::make_object(*m_pLua, std::string(pName));
        });

        // game.player_alive(id) — is player alive
        game.set_function("player_alive", [this](int id) -> bool {
                if(id < 0 || id >= MAX_CLIENTS)
                        return false;
                return GameClient()->m_Snap.m_aCharacters[id].m_Active;
        });

        // ----- Chat namespace -----
        auto chat = lua["chat"].get_or_create<sol::table>();

        // chat.send(msg) — send chat message
        chat.set_function("send", [this](const std::string &msg) {
                if(msg.empty())
                        return;
                CNetMsg_Cl_Say Msg;
                Msg.m_Team = 0;
                Msg.m_pMessage = msg.c_str();
                Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
        });

        // chat.send_team(msg) — send team chat message
        chat.set_function("send_team", [this](const std::string &msg) {
                if(msg.empty())
                        return;
                CNetMsg_Cl_Say Msg;
                Msg.m_Team = 1;
                Msg.m_pMessage = msg.c_str();
                Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
        });

        // ----- Input namespace -----
        auto input = lua["input"].get_or_create<sol::table>();

        // input.set_direction(dir) — set movement direction (-1, 0, 1)
        input.set_function("set_direction", [this](int dir) {
                int Dummy = g_Config.m_ClDummy;
                GameClient()->m_Controls.m_aInputData[Dummy].m_Direction = dir;
        });

        // input.set_jump(jump) — set jump state
        input.set_function("set_jump", [this](int jump) {
                int Dummy = g_Config.m_ClDummy;
                GameClient()->m_Controls.m_aInputData[Dummy].m_Jump = jump;
        });

        // input.set_hook(hook) — set hook state
        input.set_function("set_hook", [this](int hook) {
                int Dummy = g_Config.m_ClDummy;
                GameClient()->m_Controls.m_aInputData[Dummy].m_Hook = hook;
        });

        // input.set_fire(fire) — set fire state
        input.set_function("set_fire", [this](int fire) {
                int Dummy = g_Config.m_ClDummy;
                GameClient()->m_Controls.m_aInputData[Dummy].m_Fire = fire;
        });

        // input.set_aim(x, y) — set aim target
        input.set_function("set_aim", [this](int x, int y) {
                int Dummy = g_Config.m_ClDummy;
                GameClient()->m_Controls.m_aInputData[Dummy].m_TargetX = x;
                GameClient()->m_Controls.m_aInputData[Dummy].m_TargetY = y;
        });

        // input.set_dummy_direction(dummy, dir)
        input.set_function("set_dummy_direction", [this](int dummy, int dir) {
                if(dummy >= 0 && dummy < MAX_DUMMIES)
                        GameClient()->m_Controls.m_aInputData[dummy].m_Direction = dir;
        });

        // input.set_dummy_jump(dummy, jump)
        input.set_function("set_dummy_jump", [this](int dummy, int jump) {
                if(dummy >= 0 && dummy < MAX_DUMMIES)
                        GameClient()->m_Controls.m_aInputData[dummy].m_Jump = jump;
        });

        // input.set_dummy_hook(dummy, hook)
        input.set_function("set_dummy_hook", [this](int dummy, int hook) {
                if(dummy >= 0 && dummy < MAX_DUMMIES)
                        GameClient()->m_Controls.m_aInputData[dummy].m_Hook = hook;
        });

        // input.set_dummy_aim(dummy, x, y)
        input.set_function("set_dummy_aim", [this](int dummy, int x, int y) {
                if(dummy >= 0 && dummy < MAX_DUMMIES)
                {
                        GameClient()->m_Controls.m_aInputData[dummy].m_TargetX = x;
                        GameClient()->m_Controls.m_aInputData[dummy].m_TargetY = y;
                }
        });

        // ----- BotNet namespace -----
        auto botnet = lua["botnet"].get_or_create<sol::table>();

        // botnet.attack_enabled — read/write
        botnet.set_function("get_attack_enabled", [this]() -> bool {
                return GameClient()->m_BotNet.m_AttackEnabled;
        });
        botnet.set_function("set_attack_enabled", [this](bool v) {
                GameClient()->m_BotNet.m_AttackEnabled = v;
        });

        // botnet.move_enabled
        botnet.set_function("get_move_enabled", [this]() -> bool {
                return GameClient()->m_BotNet.m_MoveEnabled;
        });
        botnet.set_function("set_move_enabled", [this](bool v) {
                GameClient()->m_BotNet.m_MoveEnabled = v;
        });

        // botnet.pathfinder_enabled
        botnet.set_function("get_pathfinder_enabled", [this]() -> bool {
                return GameClient()->m_BotNet.m_PathfinderEnabled;
        });
        botnet.set_function("set_pathfinder_enabled", [this](bool v) {
                GameClient()->m_BotNet.m_PathfinderEnabled = v;
        });

        // botnet.kinodynamic_enabled
        botnet.set_function("get_kinodynamic_enabled", [this]() -> bool {
                return GameClient()->m_BotNet.m_KinodynamicEnabled;
        });
        botnet.set_function("set_kinodynamic_enabled", [this](bool v) {
                GameClient()->m_BotNet.m_KinodynamicEnabled = v;
        });

        // ----- Console namespace -----
        auto con = lua["console"].get_or_create<sol::table>();

        // console.log(msg) — print to game console
        con.set_function("log", [this](const std::string &msg) {
                Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", msg.c_str());
        });

        // console.exec(cmd) — execute a console command
        // ExecuteLine requires (pStr, ClientId) — use -1 for "no client"
        con.set_function("exec", [this](const std::string &cmd) {
                Console()->ExecuteLine(cmd.c_str(), -1);
        });

        // ----- Map namespace -----
        auto map = lua["map"].get_or_create<sol::table>();

        // map.width() / map.height() — via CCollision
        map.set_function("width", [this]() -> int {
                auto *pCol = GameClient()->Collision();
                return pCol ? pCol->GetWidth() : 0;
        });
        map.set_function("height", [this]() -> int {
                auto *pCol = GameClient()->Collision();
                return pCol ? pCol->GetHeight() : 0;
        });

        // map.is_tile_solid(tx, ty) — check if a tile is solid
        map.set_function("is_tile_solid", [this](int tx, int ty) -> bool {
                auto *pCol = GameClient()->Collision();
                if(!pCol)
                        return false;
                return pCol->IsSolid(tx * 32 + 16, ty * 32 + 16);
        });

        // map.is_tile_freeze(tx, ty) — check if a tile is freeze
        // GetTileIndex takes a linear index, use GetPureMapIndex to convert
        map.set_function("is_tile_freeze", [this](int tx, int ty) -> bool {
                auto *pCol = GameClient()->Collision();
                if(!pCol)
                        return false;
                int Index = pCol->GetPureMapIndex(tx * 32.0f, ty * 32.0f);
                int Tile = pCol->GetTileIndex(Index);
                // Freeze tiles in DDNet are tile 9 (FREEZE)
                return (Tile == 9);
        });

        // ----- Config namespace (read/write access to g_Config) -----
        auto config = lua["config"].get_or_create<sol::table>();

        // config.set(name, value) — set config variable via console
        config.set_function("set", [this](const std::string &name, const std::string &value) {
                char aCmd[512];
                str_format(aCmd, sizeof(aCmd), "%s %s", name.c_str(), value.c_str());
                Console()->ExecuteLine(aCmd, -1);
        });

        // config.get(name) — get config variable info as string
        // Use IConsole::ICommandInfo (not CCommandInfo)
        config.set_function("get", [this](const std::string &name) -> std::string {
                const IConsole::ICommandInfo *pInfo = Console()->GetCommandInfo(name.c_str(), CFGFLAG_CLIENT, false);
                if(!pInfo)
                        pInfo = Console()->GetCommandInfo(name.c_str(), CFGFLAG_SERVER, false);
                if(pInfo)
                {
                        // Return name + params + help
                        std::string result;
                        if(pInfo->Params())
                                result = std::string(pInfo->Params());
                        return result;
                }
                return "(unknown)";
        });

        // ----- Emote namespace -----
        auto emote = lua["emote"].get_or_create<sol::table>();

        // emote.set(type) — set player emote
        emote.set_function("set", [this](int type) {
                CNetMsg_Cl_Emoticon Msg;
                Msg.m_Emoticon = type;
                Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
        });

        // ----- Kill command -----
        lua.set_function("kill", [this]() {
                Console()->ExecuteLine("kill", -1);
        });

        // ----- Math helpers -----
        auto math2d = lua["math2d"].get_or_create<sol::table>();

        math2d.set_function("distance", [](vec2 a, vec2 b) -> float {
                return distance(a, b);
        });
        math2d.set_function("normalize", [](vec2 v) -> vec2 {
                return normalize(v);
        });
        math2d.set_function("length", [](vec2 v) -> float {
                return length(v);
        });
        math2d.set_function("dot", [](vec2 a, vec2 b) -> float {
                return dot(a, b);
        });

        // ----- Info string -----
        lua.set_function("api_info", []() -> std::string {
                return R"(
ZMajor Code Execution API:
  game.local_pos() -> vec2
  game.local_vel() -> vec2
  game.tick() -> int
  game.ticks_per_sec() -> int
  game.local_id() -> int
  game.num_players() -> int
  game.player_pos(id) -> vec2|nil
  game.player_vel(id) -> vec2|nil
  game.player_name(id) -> string|nil
  game.player_alive(id) -> bool

  chat.send(msg)
  chat.send_team(msg)

  input.set_direction(dir)       (-1, 0, 1)
  input.set_jump(jump)           (0 or 1)
  input.set_hook(hook)           (0 or 1)
  input.set_fire(fire)           (0 or 1)
  input.set_aim(x, y)
  input.set_dummy_direction(d, dir)
  input.set_dummy_jump(d, jump)
  input.set_dummy_hook(d, hook)
  input.set_dummy_aim(d, x, y)

  botnet.get_attack_enabled() / set_attack_enabled(v)
  botnet.get_move_enabled() / set_move_enabled(v)
  botnet.get_pathfinder_enabled() / set_pathfinder_enabled(v)
  botnet.get_kinodynamic_enabled() / set_kinodynamic_enabled(v)

  console.log(msg)
  console.exec(cmd)

  map.width() / height()
  map.is_tile_solid(tx, ty)
  map.is_tile_freeze(tx, ty)

  config.get(name) -> string
  config.set(name, value)

  emote.set(type)
  kill()

  math2d.distance(a, b)
  math2d.normalize(v)
  math2d.length(v)
  math2d.dot(a, b)
)";
        });
}

// =========================================================
// Raw C++ Class Access — expose DDNet internals directly to Lua
// =========================================================
void CCodeExec::RegisterRawAPI()
{
        if(!m_pLua)
                return;

        auto &lua = *m_pLua;

        // ===== CCharacterCore usertype =====
        // Core character physics state — position, velocity, hook, weapons, etc.
        lua.new_usertype<CCharacterCore>("CCharacterCore",
                "pos", &CCharacterCore::m_Pos,
                "vel", &CCharacterCore::m_Vel,
                "hook_pos", &CCharacterCore::m_HookPos,
                "hook_dir", &CCharacterCore::m_HookDir,
                "hook_tick", &CCharacterCore::m_HookTick,
                "hook_state", &CCharacterCore::m_HookState,
                "hooked_player", sol::property(&CCharacterCore::HookedPlayer, &CCharacterCore::SetHookedPlayer),
                "active_weapon", &CCharacterCore::m_ActiveWeapon,
                "jumped", &CCharacterCore::m_Jumped,
                "jumped_total", &CCharacterCore::m_JumpedTotal,
                "jumps", &CCharacterCore::m_Jumps,
                "direction", &CCharacterCore::m_Direction,
                "angle", &CCharacterCore::m_Angle,
                "triggered_events", &CCharacterCore::m_TriggeredEvents,
                "id", &CCharacterCore::m_Id,
                // DDRace flags
                "solo", &CCharacterCore::m_Solo,
                "jetpack", &CCharacterCore::m_Jetpack,
                "collision_disabled", &CCharacterCore::m_CollisionDisabled,
                "endless_hook", &CCharacterCore::m_EndlessHook,
                "endless_jump", &CCharacterCore::m_EndlessJump,
                "super", &CCharacterCore::m_Super,
                "invincible", &CCharacterCore::m_Invincible,
                "deep_frozen", &CCharacterCore::m_DeepFrozen,
                "live_frozen", &CCharacterCore::m_LiveFrozen,
                "is_in_freeze", &CCharacterCore::m_IsInFreeze,
                "freeze_start", &CCharacterCore::m_FreezeStart,
                "freeze_end", &CCharacterCore::m_FreezeEnd,
                "has_telegun_gun", &CCharacterCore::m_HasTelegunGun,
                "has_telegun_grenade", &CCharacterCore::m_HasTelegunGrenade,
                "has_telegun_laser", &CCharacterCore::m_HasTelegunLaser,
                "hammer_hit_disabled", &CCharacterCore::m_HammerHitDisabled,
                "grenade_hit_disabled", &CCharacterCore::m_GrenadeHitDisabled,
                "laser_hit_disabled", &CCharacterCore::m_LaserHitDisabled,
                "shotgun_hit_disabled", &CCharacterCore::m_ShotgunHitDisabled,
                "hook_hit_disabled", &CCharacterCore::m_HookHitDisabled,
                // Input sub-object
                "input", &CCharacterCore::m_Input,
                // Ninja sub-object
                "ninja_dir", [](CCharacterCore &c) -> vec2 { return c.m_Ninja.m_ActivationDir; },
                "ninja_activation_tick", [](CCharacterCore &c) -> int { return c.m_Ninja.m_ActivationTick; },
                "ninja_move_time", [](CCharacterCore &c) -> int { return c.m_Ninja.m_CurrentMoveTime; }
        );

        // ===== CNetObj_PlayerInput usertype =====
        lua.new_usertype<CNetObj_PlayerInput>("CPlayerInput",
                "direction", &CNetObj_PlayerInput::m_Direction,
                "target_x", &CNetObj_PlayerInput::m_TargetX,
                "target_y", &CNetObj_PlayerInput::m_TargetY,
                "jump", &CNetObj_PlayerInput::m_Jump,
                "fire", &CNetObj_PlayerInput::m_Fire,
                "hook", &CNetObj_PlayerInput::m_Hook,
                "player_flags", &CNetObj_PlayerInput::m_PlayerFlags,
                "wanted_weapon", &CNetObj_PlayerInput::m_WantedWeapon,
                "next_weapon", &CNetObj_PlayerInput::m_NextWeapon,
                "prev_weapon", &CNetObj_PlayerInput::m_PrevWeapon
        );

        // ===== CCollision usertype =====
        lua.new_usertype<CCollision>("CCollision",
                "width", &CCollision::GetWidth,
                "height", &CCollision::GetHeight,
                "check_point", sol::overload(
                        [](CCollision &c, float x, float y) -> bool { return c.CheckPoint(x, y); },
                        [](CCollision &c, vec2 pos) -> bool { return c.CheckPoint(pos); }
                ),
                "is_solid", [](CCollision &c, int x, int y) -> int { return c.IsSolid(x, y); },
                "get_tile", &CCollision::GetTile,
                "get_front_tile", &CCollision::GetFrontTile,
                "get_tile_index", &CCollision::GetTileIndex,
                "get_front_tile_index", &CCollision::GetFrontTileIndex,
                "get_collision_at", [](CCollision &c, float x, float y) -> int { return c.GetCollisionAt(x, y); },
                "get_front_collision_at", [](CCollision &c, float x, float y) -> int { return c.GetFrontCollisionAt(x, y); },
                "test_box", &CCollision::TestBox,
                "is_on_ground", &CCollision::IsOnGround,
                "intersect_line", [](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before;
                        int res = c.IntersectLine(p0, p1, &col, &before);
                        auto t = lua.create_table();
                        t["hit"] = res;
                        t["collision"] = col;
                        t["before_collision"] = before;
                        return t;
                },
                "intersect_no_laser", [](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before;
                        int res = c.IntersectNoLaser(p0, p1, &col, &before);
                        auto t = lua.create_table();
                        t["hit"] = res;
                        t["collision"] = col;
                        t["before_collision"] = before;
                        return t;
                },
                "get_pure_map_index", sol::overload(
                        [](CCollision &c, float x, float y) -> int { return c.GetPureMapIndex(x, y); },
                        [](CCollision &c, vec2 pos) -> int { return c.GetPureMapIndex(pos); }
                ),
                "is_teleport", &CCollision::IsTeleport,
                "is_evil_teleport", &CCollision::IsEvilTeleport,
                "is_speedup", &CCollision::IsSpeedup,
                "is_tune", &CCollision::IsTune,
                "get_switch_type", &CCollision::GetSwitchType,
                "get_switch_number", &CCollision::GetSwitchNumber,
                "get_switch_delay", &CCollision::GetSwitchDelay,
                "is_time_checkpoint", &CCollision::IsTimeCheckpoint,
                "is_no_laser", [](CCollision &c, int x, int y) -> int { return c.IsNoLaser(x, y); },
                "set_collision_at", &CCollision::SetCollisionAt
        );

        // ===== CChat usertype =====
        lua.new_usertype<CChat>("CChat",
                "add_line", [this](CChat &chat, const std::string &name, const std::string &msg, int team) {
                        // Fake a chat message locally — doesn't send to server
                        // We use AddLine with a special approach: set name then add
                        int ClientId = CChat::CLIENT_MSG;
                        // AddLine expects ClientId, Team, pLine
                        // For fake messages we use a trick: temporarily construct a line
                        // The simplest way is to use AddLine with a custom client ID
                        // But AddLine reads m_aClients[ClientId].m_aName
                        // So we directly add a line to the chat history
                        chat.AddLine(CChat::LUA_MSG, team, msg.c_str());
                },
                "echo", [](CChat &chat, const std::string &msg) {
                        chat.Echo(msg.c_str());
                },
                "send", [this](CChat &chat, int team, const std::string &msg) {
                        chat.SendChat(team, msg.c_str());
                },
                "send_queued", [](CChat &chat, const std::string &msg) {
                        chat.SendChatQueued(msg.c_str());
                },
                "enable_mode", [](CChat &chat, int team) {
                        chat.EnableMode(team);
                },
                "disable_mode", [](CChat &chat) {
                        chat.DisableMode();
                },
                "is_active", [](CChat &chat) -> bool {
                        return chat.IsActive();
                },
                "clear", [](CChat &chat) {
                        chat.ClearLines();
                }
        );

        // ===== CGameClient — the big one =====
        // We expose it as a global `gc` with property access to components
        lua.new_usertype<CGameClient>("CGameClient",
                // Component access
                "chat", sol::property([this](CGameClient &gc) -> CChat& { return gc.m_Chat; }),
                "controls", sol::property([this](CGameClient &gc) -> CControls& { return gc.m_Controls; }),
                "botnet", sol::property([this](CGameClient &gc) -> CBotNet& { return gc.m_BotNet; }),
                "collision", sol::property([this](CGameClient &gc) -> CCollision* { return gc.Collision(); }),
                // Game state
                "local_pos", sol::property([this](CGameClient &gc) -> vec2 { return gc.m_LocalCharacterPos; }),
                "predicted", sol::property([this](CGameClient &gc) -> CCharacterCore& { return gc.m_PredictedChar; }),
                "predicted_prev", sol::property([this](CGameClient &gc) -> CCharacterCore& { return gc.m_PredictedPrevChar; }),
                "local_id", sol::property([this](CGameClient &gc) -> int { return gc.m_Snap.m_LocalClientId; }),
                "num_players", sol::property([this](CGameClient &gc) -> int { return gc.m_Snap.m_NumPlayers; }),
                "new_tick", sol::property([this](CGameClient &gc) -> bool { return gc.m_NewTick; }),
                "server_mode", sol::property([this](CGameClient &gc) -> int { return gc.m_ServerMode; }),
                // Methods
                "send_kill", [](CGameClient &gc) { gc.SendKill(); },
                "send_switch_team", [](CGameClient &gc, int team) { gc.SendSwitchTeam(team); },
                "echo", [](CGameClient &gc, const std::string &msg) { gc.Echo(msg.c_str()); },
                "is_team_play", [](CGameClient &gc) -> bool { return gc.IsTeamPlay(); },
                "is_world_paused", [](CGameClient &gc) -> bool { return gc.IsWorldPaused(); },
                "is_local_char_super", [](CGameClient &gc) -> bool { return gc.IsLocalCharSuper(); },
                // Access client data by ID
                "client", [this](CGameClient &gc, int id) -> sol::object {
                        if(id < 0 || id >= MAX_CLIENTS)
                                return sol::nil;
                        return sol::make_object(lua, &gc.m_aClients[id]);
                }
        );

        // ===== CClientData (CGameClient::CClientData) usertype =====
        lua.new_usertype<CGameClient::CClientData>("CClientData",
                "name", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aName); },
                "clan", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aClan); },
                "country", &CGameClient::CClientData::m_Country,
                "skin", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aSkinName); },
                "team", &CGameClient::CClientData::m_Team,
                "active", &CGameClient::CClientData::m_Active,
                "friend_", &CGameClient::CClientData::m_Friend,
                "foe", &CGameClient::CClientData::m_Foe,
                "afk", &CGameClient::CClientData::m_Afk,
                "paused", &CGameClient::CClientData::m_Paused,
                "spec", &CGameClient::CClientData::m_Spec,
                "solo", &CGameClient::CClientData::m_Solo,
                "jetpack", &CGameClient::CClientData::m_Jetpack,
                "super", &CGameClient::CClientData::m_Super,
                "invincible", &CGameClient::CClientData::m_Invincible,
                "deep_frozen", &CGameClient::CClientData::m_DeepFrozen,
                "live_frozen", &CGameClient::CClientData::m_LiveFrozen,
                "collision_disabled", &CGameClient::CClientData::m_CollisionDisabled,
                "endless_hook", &CGameClient::CClientData::m_EndlessHook,
                "endless_jump", &CGameClient::CClientData::m_EndlessJump,
                "hammer_hit_disabled", &CGameClient::CClientData::m_HammerHitDisabled,
                "grenade_hit_disabled", &CGameClient::CClientData::m_GrenadeHitDisabled,
                "laser_hit_disabled", &CGameClient::CClientData::m_LaserHitDisabled,
                "shotgun_hit_disabled", &CGameClient::CClientData::m_ShotgunHitDisabled,
                "hook_hit_disabled", &CGameClient::CClientData::m_HookHitDisabled,
                "has_telegun_gun", &CGameClient::CClientData::m_HasTelegunGun,
                "has_telegun_grenade", &CGameClient::CClientData::m_HasTelegunGrenade,
                "has_telegun_laser", &CGameClient::CClientData::m_HasTelegunLaser,
                "freeze_end", &CGameClient::CClientData::m_FreezeEnd,
                "auth_level", &CGameClient::CClientData::m_AuthLevel,
                "use_custom_color", &CGameClient::CClientData::m_UseCustomColor,
                "color_body", &CGameClient::CClientData::m_ColorBody,
                "color_feet", &CGameClient::CClientData::m_ColorFeet,
                "angle", &CGameClient::CClientData::m_Angle,
                "emoticon", &CGameClient::CClientData::m_Emoticon,
                "id", &CGameClient::CClientData::ClientId,
                "predicted", sol::property([](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_Predicted; }),
                "chat_ignore", &CGameClient::CClientData::m_ChatIgnore,
                "finish_time_seconds", &CGameClient::CClientData::m_FinishTimeSeconds,
                "finish_time_millis", &CGameClient::CClientData::m_FinishTimeMillis
        );

        // ===== CBotNet usertype =====
        lua.new_usertype<CBotNet>("CBotNet",
                "attack_enabled", &CBotNet::m_AttackEnabled,
                "auto_aim", &CBotNet::m_AutoAim,
                "auto_fire", &CBotNet::m_AutoFire,
                "auto_hook", &CBotNet::m_AutoHook,
                "move_enabled", &CBotNet::m_MoveEnabled,
                "stand_enabled", &CBotNet::m_StandEnabled,
                "rescue_frozen", &CBotNet::m_RescueFrozen,
                "rescue_all", &CBotNet::m_RescueAll,
                "kill_on_freeze", &CBotNet::m_KillOnFreeze,
                "attack_main", &CBotNet::m_AttackMain,
                "auto_main", &CBotNet::m_AutoMain,
                "auto_hammer", &CBotNet::m_AutoHammer,
                "smart_detect", &CBotNet::m_SmartDetect,
                "smart_rescue", &CBotNet::m_SmartRescue,
                "avoid_freeze", &CBotNet::m_AvoidFreeze,
                "main_id", &CBotNet::m_MainID,
                "all_target", &CBotNet::m_AllTarget,
                "fire_dist", &CBotNet::m_FireDist,
                "hook_dist", &CBotNet::m_HookDist,
                "rescue_radius", &CBotNet::m_RescueRadius,
                "target_dist", &CBotNet::m_TargetDist,
                "main_dist", &CBotNet::m_MainDist,
                "stand_dist", &CBotNet::m_StandDist,
                "laser_rescue", &CBotNet::m_LaserRescue,
                "laser_rescue_dist", &CBotNet::m_LaserRescueDist,
                "avoid_freeze_radius", &CBotNet::m_AvoidFreezeRadius,
                "hook_delay", &CBotNet::m_HookDelay,
                "copy_moves", &CBotNet::m_CopyMoves,
                "copy_target_id", &CBotNet::m_CopyTargetID,
                "random_aim", &CBotNet::m_RandomAim,
                "random_aim_interval", &CBotNet::m_RandomAimInterval,
                "pathfinder_enabled", &CBotNet::m_PathfinderEnabled,
                "pf_simulate_players", &CBotNet::m_PfSimulatePlayers,
                "pf_rays", &CBotNet::m_PfRays,
                "pf_view_radius", &CBotNet::m_PfViewRadius,
                "pf_hook_enabled", &CBotNet::m_PfHookEnabled,
                "kinodynamic_enabled", &CBotNet::m_KinodynamicEnabled,
                "kino_candidates", &CBotNet::m_KinoCandidates,
                "kino_ticks", &CBotNet::m_KinoTicks,
                "kino_hook_angles", &CBotNet::m_KinoHookAngles,
                "kino_cache_ticks", &CBotNet::m_KinoCacheTicks,
                "kino_show_path", &CBotNet::m_KinoShowPath,
                "kino_show_field", &CBotNet::m_KinoShowField
        );

        // ===== CControls usertype =====
        lua.new_usertype<CControls>("CControls",
                "input", [](CControls &c, int dummy) -> CNetObj_PlayerInput& { return c.m_aInputData[dummy]; },
                "last_input", [](CControls &c, int dummy) -> CNetObj_PlayerInput& { return c.m_aLastData[dummy]; },
                "mouse_pos", [](CControls &c, int dummy) -> vec2 { return c.m_aMousePos[dummy]; },
                "target_pos", [](CControls &c, int dummy) -> vec2 { return c.m_aTargetPos[dummy]; },
                "reset_input", [](CControls &c, int dummy) { c.ResetInput(dummy); }
        );

        // ===== Set global objects =====
        // gc = CGameClient reference
        lua["gc"] = std::ref(*GameClient());

        // cl = CClient pointer (through IClient interface)
        // We expose the most useful CClient methods through a table
        auto cl = lua["cl"].get_or_create<sol::table>();
        cl.set_function("connect", [this](const std::string &addr) {
                Client()->Connect(addr.c_str());
        });
        cl.set_function("disconnect", [this]() {
                Client()->Disconnect();
        });
        cl.set_function("enter_game", [this]() {
                Client()->EnterGame(0);
        });
        cl.set_function("rcon_auth", [this](const std::string &user, const std::string &pass) {
                Client()->RconAuth(user.c_str(), pass.c_str(), 0);
        });
        cl.set_function("rcon", [this](const std::string &cmd) {
                Client()->Rcon(cmd.c_str());
        });
        cl.set_function("rcon_authed", [this]() -> bool {
                return Client()->RconAuthed();
        });
        cl.set_function("game_tick", [this]() -> int {
                return Client()->GameTick(0);
        });
        cl.set_function("game_tick_speed", [this]() -> int {
                return Client()->GameTickSpeed();
        });
        cl.set_function("connection_problems", [this]() -> bool {
                return Client()->ConnectionProblems();
        });
        cl.set_function("dummy_connect", [this](int d) {
                Client()->DummyConnect(d);
        });
        cl.set_function("dummy_disconnect", [this](int d) {
                Client()->DummyDisconnect(d, "Lua script");
        });
        cl.set_function("dummy_connected", [this](int d) -> bool {
                return Client()->DummyConnected(d);
        });
        cl.set_function("dummy_count", [this]() -> int {
                return Client()->DummyCount();
        });
        cl.set_function("quit", [this]() {
                Client()->Quit();
        });
        cl.set_function("restart", [this]() {
                Client()->Restart();
        });
        cl.set_function("player_name", [this]() -> std::string {
                return std::string(Client()->PlayerName());
        });

        // ===== Additional helpers =====

        // chat.fake(name, msg, team) — display a fake chat message locally
        auto chatNs = lua["chat"].get_or_create<sol::table>();
        chatNs.set_function("fake", [this](const std::string &name, const std::string &msg, sol::optional<int> team) {
                // Directly add a line to chat as a server message with the name prepended
                char aBuf[512];
                str_format(aBuf, sizeof(aBuf), "%s: %s", name.c_str(), msg.c_str());
                int Team = team.value_or(0);
                GameClient()->m_Chat.AddLine(CChat::CLIENT_MSG, Team, aBuf);
        });

        // game.client_info(id) — full client info as table
        auto gameNs = lua["game"].get_or_create<sol::table>();
        gameNs.set_function("client_info", [this](int id) -> sol::object {
                if(id < 0 || id >= MAX_CLIENTS || !GameClient()->m_Snap.m_apPlayerInfos[id])
                        return sol::nil;
                auto &c = GameClient()->m_aClients[id];
                auto t = lua.create_table();
                t["name"] = std::string(c.m_aName);
                t["clan"] = std::string(c.m_aClan);
                t["country"] = c.m_Country;
                t["skin"] = std::string(c.m_aSkinName);
                t["team"] = c.m_Team;
                t["active"] = c.m_Active;
                t["friend"] = c.m_Friend;
                t["foe"] = c.m_Foe;
                t["afk"] = c.m_Afk;
                t["paused"] = c.m_Paused;
                t["spec"] = c.m_Spec;
                t["solo"] = c.m_Solo;
                t["jetpack"] = c.m_Jetpack;
                t["super"] = c.m_Super;
                t["invincible"] = c.m_Invincible;
                t["deep_frozen"] = c.m_DeepFrozen;
                t["live_frozen"] = c.m_LiveFrozen;
                t["collision_disabled"] = c.m_CollisionDisabled;
                t["endless_hook"] = c.m_EndlessHook;
                t["endless_jump"] = c.m_EndlessJump;
                t["auth_level"] = c.m_AuthLevel;
                t["freeze_end"] = c.m_FreezeEnd;
                t["has_telegun_gun"] = c.m_HasTelegunGun;
                t["has_telegun_grenade"] = c.m_HasTelegunGrenade;
                t["has_telegun_laser"] = c.m_HasTelegunLaser;
                return t;
        });

        // game.char_info(id) — character snapshot data as table
        gameNs.set_function("char_info", [this](int id) -> sol::object {
                if(id < 0 || id >= MAX_CLIENTS)
                        return sol::nil;
                auto &info = GameClient()->m_Snap.m_aCharacters[id];
                if(!info.m_Active)
                        return sol::nil;
                auto t = lua.create_table();
                t["x"] = (float)info.m_Cur.m_X / 100.0f;
                t["y"] = (float)info.m_Cur.m_Y / 100.0f;
                t["vel_x"] = (float)info.m_Cur.m_VelX / 100.0f;
                t["vel_y"] = (float)info.m_Cur.m_VelY / 100.0f;
                t["angle"] = info.m_Cur.m_Angle;
                t["direction"] = info.m_Cur.m_Direction;
                t["weapon"] = info.m_Cur.m_Weapon;
                t["ammo_count"] = info.m_Cur.m_AmmoCount;
                t["health"] = info.m_Cur.m_Health;
                t["armor"] = info.m_Cur.m_Armor;
                t["player_flags"] = info.m_Cur.m_PlayerFlags;
                t["hooked_player"] = info.m_Cur.m_HookedPlayer;
                t["hook_state"] = info.m_Cur.m_HookState;
                t["hook_tick"] = info.m_Cur.m_HookTick;
                t["attack_tick"] = info.m_Cur.m_AttackTick;
                t["jumped_total"] = info.m_Cur.m_JumpedTotal;
                // DDNet extended
                if(info.m_HasExtendedData)
                {
                        t["dd_flags"] = info.m_ExtendedData.m_Flags;
                        t["dd_freeze_end"] = info.m_ExtendedData.m_FreezeEnd;
                        t["dd_jumps"] = info.m_ExtendedData.m_Jumps;
                        t["dd_strong_weak_id"] = info.m_ExtendedData.m_StrongWeakId;
                        t["dd_target_x"] = info.m_ExtendedData.m_TargetX;
                        t["dd_target_y"] = info.m_ExtendedData.m_TargetY;
                }
                return t;
        });

        // game.all_players() — iterate all active players
        gameNs.set_function("all_players", [this]() -> sol::object {
                auto t = lua.create_table();
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(!GameClient()->m_Snap.m_apPlayerInfos[i])
                                continue;
                        auto p = lua.create_table();
                        p["id"] = i;
                        p["name"] = std::string(GameClient()->m_aClients[i].m_aName);
                        p["clan"] = std::string(GameClient()->m_aClients[i].m_aClan);
                        p["team"] = GameClient()->m_aClients[i].m_Team;
                        p["alive"] = GameClient()->m_Snap.m_aCharacters[i].m_Active;
                        p["afk"] = GameClient()->m_aClients[i].m_Afk;
                        p["solo"] = GameClient()->m_aClients[i].m_Solo;
                        p["super"] = GameClient()->m_aClients[i].m_Super;
                        if(GameClient()->m_Snap.m_aCharacters[i].m_Active)
                        {
                                p["x"] = (float)GameClient()->m_Snap.m_aCharacters[i].m_Cur.m_X / 100.0f;
                                p["y"] = (float)GameClient()->m_Snap.m_aCharacters[i].m_Cur.m_Y / 100.0f;
                        }
                        t[i + 1] = p; // Lua 1-indexed
                }
                return t;
        });

        // ===== Updated API info =====
        lua.set_function("raw_api_info", []() -> std::string {
                return R"(
ZMajor Raw C++ API:
  gc : CGameClient — direct access to game client
    gc.chat           -> CChat
    gc.controls       -> CControls
    gc.botnet         -> CBotNet
    gc.collision      -> CCollision
    gc.local_pos      -> vec2
    gc.predicted      -> CCharacterCore
    gc.predicted_prev -> CCharacterCore
    gc.local_id       -> int
    gc.num_players    -> int
    gc.new_tick       -> bool
    gc.server_mode    -> int
    gc:send_kill()
    gc:send_switch_team(team)
    gc:echo(msg)
    gc:is_team_play() -> bool
    gc:is_world_paused() -> bool
    gc:is_local_char_super() -> bool
    gc:client(id) -> CClientData

  CClientData:
    .name .clan .country .skin .team .active
    .friend .foe .afk .paused .spec
    .solo .jetpack .super .invincible
    .deep_frozen .live_frozen .freeze_end
    .collision_disabled .endless_hook .endless_jump
    .hammer/grenade/laser/shotgun/hook_hit_disabled
    .has_telegun_gun/grenade/laser
    .auth_level .angle .emoticon
    .predicted -> CCharacterCore

  CCharacterCore:
    .pos .vel .hook_pos .hook_dir
    .hook_tick .hook_state .hooked_player
    .active_weapon .direction .angle
    .jumped .jumped_total .jumps
    .solo .jetpack .super .invincible
    .deep_frozen .live_frozen .is_in_freeze
    .freeze_start .freeze_end
    .endless_hook .endless_jump .collision_disabled
    .input -> CPlayerInput

  CPlayerInput:
    .direction .target_x .target_y
    .jump .fire .hook .player_flags
    .wanted_weapon .next_weapon .prev_weapon

  CCollision:
    :width() :height() :check_point(x,y)
    :is_solid(x,y) :get_tile(x,y) :get_front_tile(x,y)
    :intersect_line(p0, p1) -> table
    :is_teleport(idx) :is_speedup(idx) :is_tune(idx)

  CChat:
    :add_line(name, msg, team) :echo(msg)
    :send(team, msg) :send_queued(msg)
    :enable_mode(team) :disable_mode()
    :is_active() :clear()

  CControls:
    :input(dummy) -> CPlayerInput
    :mouse_pos(dummy) -> vec2
    :target_pos(dummy) -> vec2
    :reset_input(dummy)

  CBotNet:
    .attack_enabled .auto_aim .auto_fire .auto_hook
    .move_enabled .stand_enabled .rescue_frozen
    .kill_on_freeze .avoid_freeze .main_id
    .fire_dist .hook_dist .rescue_radius
    .pathfinder_enabled .kinodynamic_enabled
    (and many more — see raw_api_info())

  cl: connect/disconnect/enter_game
     rcon_auth/rcon/rcon_authed
     dummy_connect/dummy_disconnect/dummy_connected
     game_tick/game_tick_speed
     connection_problems/quit/restart

  chat.fake(name, msg, team)
  game.client_info(id) -> table
  game.char_info(id) -> table
  game.all_players() -> table
)";
        });
}

// =========================================================
// Multiline editor helpers
// =========================================================
int CCodeExec::GetLineCount() const
{
        int Count = 1;
        for(int i = 0; m_aCodeBuffer[i]; i++)
        {
                if(m_aCodeBuffer[i] == '\n')
                        Count++;
        }
        return Count;
}

int CCodeExec::GetLineStart(int Line) const
{
        if(Line <= 0)
                return 0;
        int CurLine = 0;
        for(int i = 0; m_aCodeBuffer[i]; i++)
        {
                if(m_aCodeBuffer[i] == '\n')
                {
                        CurLine++;
                        if(CurLine == Line)
                                return i + 1;
                }
        }
        return str_length(m_aCodeBuffer);
}

int CCodeExec::GetLineFromOffset(int Offset) const
{
        int Line = 0;
        for(int i = 0; i < Offset && m_aCodeBuffer[i]; i++)
        {
                if(m_aCodeBuffer[i] == '\n')
                        Line++;
        }
        return Line;
}

void CCodeExec::EnsureCursorVisible(float LineHeight, float ViewHeight)
{
        int CursorLine = GetLineFromOffset(m_CursorPos);
        float CursorY = CursorLine * LineHeight;
        if(CursorY < m_ScrollY)
                m_ScrollYChange = CursorY - m_ScrollY;
        else if(CursorY + LineHeight > m_ScrollY + ViewHeight)
                m_ScrollYChange = (CursorY + LineHeight) - (m_ScrollY + ViewHeight);
}

int CCodeExec::GetLineLength(int Line) const
{
        int LineOff = GetLineStart(Line);
        int LineEndOff = LineOff;
        while(m_aCodeBuffer[LineEndOff] && m_aCodeBuffer[LineEndOff] != '\n')
                LineEndOff++;
        return LineEndOff - LineOff;
}

// =========================================================
// Lua syntax highlighting
// =========================================================
void CCodeExec::GetHighlightSegments(const char *pLine, SHighlightSegment *pSegments, int &SegmentCount)
{
        SegmentCount = 0;
        int LineLen = str_length(pLine);
        if(LineLen <= 0)
                return;

        // Lua keywords
        static const char *s_apKeywords[] = {
                "and", "break", "do", "else", "elseif", "end",
                "false", "for", "function", "goto", "if", "in",
                "local", "nil", "not", "or", "repeat", "return",
                "then", "true", "until", "while",
                // Common globals
                "print", "pairs", "ipairs", "tostring", "tonumber",
                "type", "require", "pcall", "xpcall", "error",
                "assert", "collectgarbage", "select", "unpack",
                "rawget", "rawset", "setmetatable", "getmetatable",
                nullptr
        };

        auto IsAlpha = [](char c) -> bool { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        auto IsDigit = [](char c) -> bool { return c >= '0' && c <= '9'; };
        auto IsAlphaNum = [&](char c) -> bool { return IsAlpha(c) || IsDigit(c); };

        auto AddSegment = [&](int Start, int Len, EHighlightType Type) {
                if(SegmentCount < MAX_HIGHLIGHT_SEGMENTS && Len > 0)
                {
                        pSegments[SegmentCount].m_Start = Start;
                        pSegments[SegmentCount].m_Length = Len;
                        pSegments[SegmentCount].m_Type = Type;
                        SegmentCount++;
                }
        };

        int i = 0;
        while(i < LineLen)
        {
                // Inside block comment --[[ ... ]]
                if(m_HighlightParseState == 1)
                {
                        int Start = i;
                        while(i < LineLen)
                        {
                                if(i + 1 < LineLen && pLine[i] == ']' && pLine[i + 1] == ']')
                                {
                                        i += 2;
                                        m_HighlightParseState = 0;
                                        break;
                                }
                                i++;
                        }
                        AddSegment(Start, i - Start, HIGHLIGHT_COMMENT);
                        continue;
                }

                // Line comment --
                if(i + 1 < LineLen && pLine[i] == '-' && pLine[i + 1] == '-')
                {
                        // Check for block comment --[[
                        if(i + 3 < LineLen && pLine[i + 2] == '[' && pLine[i + 3] == '[')
                        {
                                int Start = i;
                                i += 4;
                                m_HighlightParseState = 1;
                                // Look for ]] on same line
                                while(i < LineLen)
                                {
                                        if(i + 1 < LineLen && pLine[i] == ']' && pLine[i + 1] == ']')
                                        {
                                                i += 2;
                                                m_HighlightParseState = 0;
                                                break;
                                        }
                                        i++;
                                }
                                AddSegment(Start, i - Start, HIGHLIGHT_COMMENT);
                                continue;
                        }
                        // Regular line comment -- rest of line
                        AddSegment(i, LineLen - i, HIGHLIGHT_COMMENT);
                        i = LineLen;
                        continue;
                }

                // String literals "..." and '...'
                if(pLine[i] == '"' || pLine[i] == '\'')
                {
                        char Quote = pLine[i];
                        int Start = i;
                        i++;
                        while(i < LineLen && pLine[i] != Quote)
                        {
                                if(pLine[i] == '\\' && i + 1 < LineLen)
                                        i++; // skip escaped char
                                i++;
                        }
                        if(i < LineLen)
                                i++; // skip closing quote
                        AddSegment(Start, i - Start, HIGHLIGHT_STRING);
                        continue;
                }

                // Long string [[ ... ]]
                if(i + 1 < LineLen && pLine[i] == '[' && pLine[i + 1] == '[')
                {
                        int Start = i;
                        i += 2;
                        while(i < LineLen)
                        {
                                if(i + 1 < LineLen && pLine[i] == ']' && pLine[i + 1] == ']')
                                {
                                        i += 2;
                                        break;
                                }
                                i++;
                        }
                        AddSegment(Start, i - Start, HIGHLIGHT_STRING);
                        continue;
                }

                // Number
                if(IsDigit(pLine[i]) || (pLine[i] == '.' && i + 1 < LineLen && IsDigit(pLine[i + 1])))
                {
                        int Start = i;
                        // Hex: 0x...
                        if(pLine[i] == '0' && i + 1 < LineLen && (pLine[i + 1] == 'x' || pLine[i + 1] == 'X'))
                        {
                                i += 2;
                                while(i < LineLen && (IsDigit(pLine[i]) || (pLine[i] >= 'a' && pLine[i] <= 'f') || (pLine[i] >= 'A' && pLine[i] <= 'F')))
                                        i++;
                        }
                        else
                        {
                                while(i < LineLen && IsDigit(pLine[i]))
                                        i++;
                                if(i < LineLen && pLine[i] == '.')
                                {
                                        i++;
                                        while(i < LineLen && IsDigit(pLine[i]))
                                                i++;
                                }
                                if(i < LineLen && (pLine[i] == 'e' || pLine[i] == 'E'))
                                {
                                        i++;
                                        if(i < LineLen && (pLine[i] == '+' || pLine[i] == '-'))
                                                i++;
                                        while(i < LineLen && IsDigit(pLine[i]))
                                                i++;
                                }
                        }
                        AddSegment(Start, i - Start, HIGHLIGHT_NUMBER);
                        continue;
                }

                // Identifier or keyword
                if(IsAlpha(pLine[i]))
                {
                        int Start = i;
                        while(i < LineLen && IsAlphaNum(pLine[i]))
                                i++;
                        int WordLen = i - Start;

                        // Check if keyword
                        bool IsKeyword = false;
                        for(int k = 0; s_apKeywords[k]; k++)
                        {
                                int KWLen = str_length(s_apKeywords[k]);
                                if(WordLen == KWLen && str_comp_num(pLine + Start, s_apKeywords[k], WordLen) == 0)
                                {
                                        IsKeyword = true;
                                        break;
                                }
                        }
                        AddSegment(Start, WordLen, IsKeyword ? HIGHLIGHT_KEYWORD : HIGHLIGHT_DEFAULT);
                        continue;
                }

                // Default: single character
                AddSegment(i, 1, HIGHLIGHT_DEFAULT);
                i++;
        }
}

// =========================================================
// Console Commands
// =========================================================
void CCodeExec::ConExec(IConsole::IResult *pResult, void *pUserData)
{
        CCodeExec *pSelf = (CCodeExec *)pUserData;
        const char *pCode = pResult->GetString(0);
        pSelf->Execute(pCode);
}

void CCodeExec::ConStop(IConsole::IResult *pResult, void *pUserData)
{
        CCodeExec *pSelf = (CCodeExec *)pUserData;
        pSelf->Stop();
}
