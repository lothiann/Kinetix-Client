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
// All member names match C++ EXACTLY so Lua code reads like C++ code:
//   gc.m_PredictedChar.m_Vel.x
//   gc.m_aClients[3].m_aName
//   gc.m_BotNet.m_AttackEnabled = true
// =========================================================
void CCodeExec::RegisterRawAPI()
{
        if(!m_pLua)
                return;

        auto &lua = *m_pLua;

        // ===== CCharacterCore — exact C++ member names =====
        lua.new_usertype<CCharacterCore>("CCharacterCore",
                "m_Pos", &CCharacterCore::m_Pos,
                "m_Vel", &CCharacterCore::m_Vel,
                "m_HookPos", &CCharacterCore::m_HookPos,
                "m_HookDir", &CCharacterCore::m_HookDir,
                "m_HookTick", &CCharacterCore::m_HookTick,
                "m_HookState", &CCharacterCore::m_HookState,
                "m_HookedPlayer", sol::property(&CCharacterCore::HookedPlayer, &CCharacterCore::SetHookedPlayer),
                "m_ActiveWeapon", &CCharacterCore::m_ActiveWeapon,
                "m_Jumped", &CCharacterCore::m_Jumped,
                "m_JumpedTotal", &CCharacterCore::m_JumpedTotal,
                "m_Jumps", &CCharacterCore::m_Jumps,
                "m_Direction", &CCharacterCore::m_Direction,
                "m_Angle", &CCharacterCore::m_Angle,
                "m_TriggeredEvents", &CCharacterCore::m_TriggeredEvents,
                "m_Id", &CCharacterCore::m_Id,
                "m_Input", &CCharacterCore::m_Input,
                // DDRace flags
                "m_Solo", &CCharacterCore::m_Solo,
                "m_Jetpack", &CCharacterCore::m_Jetpack,
                "m_CollisionDisabled", &CCharacterCore::m_CollisionDisabled,
                "m_EndlessHook", &CCharacterCore::m_EndlessHook,
                "m_EndlessJump", &CCharacterCore::m_EndlessJump,
                "m_Super", &CCharacterCore::m_Super,
                "m_Invincible", &CCharacterCore::m_Invincible,
                "m_DeepFrozen", &CCharacterCore::m_DeepFrozen,
                "m_LiveFrozen", &CCharacterCore::m_LiveFrozen,
                "m_IsInFreeze", &CCharacterCore::m_IsInFreeze,
                "m_FreezeStart", &CCharacterCore::m_FreezeStart,
                "m_FreezeEnd", &CCharacterCore::m_FreezeEnd,
                "m_HasTelegunGun", &CCharacterCore::m_HasTelegunGun,
                "m_HasTelegunGrenade", &CCharacterCore::m_HasTelegunGrenade,
                "m_HasTelegunLaser", &CCharacterCore::m_HasTelegunLaser,
                "m_HammerHitDisabled", &CCharacterCore::m_HammerHitDisabled,
                "m_GrenadeHitDisabled", &CCharacterCore::m_GrenadeHitDisabled,
                "m_LaserHitDisabled", &CCharacterCore::m_LaserHitDisabled,
                "m_ShotgunHitDisabled", &CCharacterCore::m_ShotgunHitDisabled,
                "m_HookHitDisabled", &CCharacterCore::m_HookHitDisabled,
                // Ninja (nested struct — expose as read-only sub-fields)
                "m_Ninja_ActivationDir", [](CCharacterCore &c) -> vec2 { return c.m_Ninja.m_ActivationDir; },
                "m_Ninja_ActivationTick", [](CCharacterCore &c) -> int { return c.m_Ninja.m_ActivationTick; },
                "m_Ninja_CurrentMoveTime", [](CCharacterCore &c) -> int { return c.m_Ninja.m_CurrentMoveTime; },
                // Methods
                "Reset", [](CCharacterCore &c) { c.Reset(); },
                "Tick", [](CCharacterCore &c, bool useInput) { c.Tick(useInput); },
                "Move", [](CCharacterCore &c) { c.Move(); }
        );

        // ===== CNetObj_PlayerInput — exact C++ member names =====
        lua.new_usertype<CNetObj_PlayerInput>("CNetObj_PlayerInput",
                "m_Direction", &CNetObj_PlayerInput::m_Direction,
                "m_TargetX", &CNetObj_PlayerInput::m_TargetX,
                "m_TargetY", &CNetObj_PlayerInput::m_TargetY,
                "m_Jump", &CNetObj_PlayerInput::m_Jump,
                "m_Fire", &CNetObj_PlayerInput::m_Fire,
                "m_Hook", &CNetObj_PlayerInput::m_Hook,
                "m_PlayerFlags", &CNetObj_PlayerInput::m_PlayerFlags,
                "m_WantedWeapon", &CNetObj_PlayerInput::m_WantedWeapon,
                "m_NextWeapon", &CNetObj_PlayerInput::m_NextWeapon,
                "m_PrevWeapon", &CNetObj_PlayerInput::m_PrevWeapon
        );

        // ===== CNetObj_Character — exact C++ member names =====
        lua.new_usertype<CNetObj_Character>("CNetObj_Character",
                "m_X", &CNetObj_Character::m_X,
                "m_Y", &CNetObj_Character::m_Y,
                "m_VelX", &CNetObj_Character::m_VelX,
                "m_VelY", &CNetObj_Character::m_VelY,
                "m_Angle", &CNetObj_Character::m_Angle,
                "m_Direction", &CNetObj_Character::m_Direction,
                "m_Weapon", &CNetObj_Character::m_Weapon,
                "m_AmmoCount", &CNetObj_Character::m_AmmoCount,
                "m_Health", &CNetObj_Character::m_Health,
                "m_Armor", &CNetObj_Character::m_Armor,
                "m_PlayerFlags", &CNetObj_Character::m_PlayerFlags,
                "m_HookedPlayer", &CNetObj_Character::m_HookedPlayer,
                "m_HookState", &CNetObj_Character::m_HookState,
                "m_HookTick", &CNetObj_Character::m_HookTick,
                "m_AttackTick", &CNetObj_Character::m_AttackTick,
                "m_JumpedTotal", &CNetObj_Character::m_JumpedTotal
        );

        // ===== CClientData — exact C++ member names =====
        lua.new_usertype<CGameClient::CClientData>("CClientData",
                "m_aName", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aName); },
                "m_aClan", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aClan); },
                "m_Country", &CGameClient::CClientData::m_Country,
                "m_aSkinName", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aSkinName); },
                "m_Team", &CGameClient::CClientData::m_Team,
                "m_Active", &CGameClient::CClientData::m_Active,
                "m_Friend", &CGameClient::CClientData::m_Friend,
                "m_Foe", &CGameClient::CClientData::m_Foe,
                "m_Afk", &CGameClient::CClientData::m_Afk,
                "m_Paused", &CGameClient::CClientData::m_Paused,
                "m_Spec", &CGameClient::CClientData::m_Spec,
                "m_Solo", &CGameClient::CClientData::m_Solo,
                "m_Jetpack", &CGameClient::CClientData::m_Jetpack,
                "m_Super", &CGameClient::CClientData::m_Super,
                "m_Invincible", &CGameClient::CClientData::m_Invincible,
                "m_DeepFrozen", &CGameClient::CClientData::m_DeepFrozen,
                "m_LiveFrozen", &CGameClient::CClientData::m_LiveFrozen,
                "m_CollisionDisabled", &CGameClient::CClientData::m_CollisionDisabled,
                "m_EndlessHook", &CGameClient::CClientData::m_EndlessHook,
                "m_EndlessJump", &CGameClient::CClientData::m_EndlessJump,
                "m_HammerHitDisabled", &CGameClient::CClientData::m_HammerHitDisabled,
                "m_GrenadeHitDisabled", &CGameClient::CClientData::m_GrenadeHitDisabled,
                "m_LaserHitDisabled", &CGameClient::CClientData::m_LaserHitDisabled,
                "m_ShotgunHitDisabled", &CGameClient::CClientData::m_ShotgunHitDisabled,
                "m_HookHitDisabled", &CGameClient::CClientData::m_HookHitDisabled,
                "m_HasTelegunGun", &CGameClient::CClientData::m_HasTelegunGun,
                "m_HasTelegunGrenade", &CGameClient::CClientData::m_HasTelegunGrenade,
                "m_HasTelegunLaser", &CGameClient::CClientData::m_HasTelegunLaser,
                "m_FreezeEnd", &CGameClient::CClientData::m_FreezeEnd,
                "m_AuthLevel", &CGameClient::CClientData::m_AuthLevel,
                "m_UseCustomColor", &CGameClient::CClientData::m_UseCustomColor,
                "m_ColorBody", &CGameClient::CClientData::m_ColorBody,
                "m_ColorFeet", &CGameClient::CClientData::m_ColorFeet,
                "m_Angle", &CGameClient::CClientData::m_Angle,
                "m_Emoticon", &CGameClient::CClientData::m_Emoticon,
                "m_ChatIgnore", &CGameClient::CClientData::m_ChatIgnore,
                "m_FinishTimeSeconds", &CGameClient::CClientData::m_FinishTimeSeconds,
                "m_FinishTimeMillis", &CGameClient::CClientData::m_FinishTimeMillis,
                "m_Predicted", sol::property([](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_Predicted; }),
                "m_PrevPredicted", sol::property([](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_PrevPredicted; }),
                // Methods
                "ClientId", &CGameClient::CClientData::ClientId,
                "Reset", [](CGameClient::CClientData &d) { d.Reset(); },
                "UpdateRenderInfo", [](CGameClient::CClientData &d) { d.UpdateRenderInfo(); }
        );

        // ===== CBotNet — exact C++ member names =====
        lua.new_usertype<CBotNet>("CBotNet",
                "m_AttackEnabled", &CBotNet::m_AttackEnabled,
                "m_AutoAim", &CBotNet::m_AutoAim,
                "m_AutoFire", &CBotNet::m_AutoFire,
                "m_AutoHook", &CBotNet::m_AutoHook,
                "m_MoveEnabled", &CBotNet::m_MoveEnabled,
                "m_StandEnabled", &CBotNet::m_StandEnabled,
                "m_RescueFrozen", &CBotNet::m_RescueFrozen,
                "m_RescueAll", &CBotNet::m_RescueAll,
                "m_KillOnFreeze", &CBotNet::m_KillOnFreeze,
                "m_AttackMain", &CBotNet::m_AttackMain,
                "m_AutoMain", &CBotNet::m_AutoMain,
                "m_AutoHammer", &CBotNet::m_AutoHammer,
                "m_SmartDetect", &CBotNet::m_SmartDetect,
                "m_SmartRescue", &CBotNet::m_SmartRescue,
                "m_AvoidFreeze", &CBotNet::m_AvoidFreeze,
                "m_MainID", &CBotNet::m_MainID,
                "m_AllTarget", &CBotNet::m_AllTarget,
                "m_FireDist", &CBotNet::m_FireDist,
                "m_HookDist", &CBotNet::m_HookDist,
                "m_RescueRadius", &CBotNet::m_RescueRadius,
                "m_TargetDist", &CBotNet::m_TargetDist,
                "m_MainDist", &CBotNet::m_MainDist,
                "m_StandDist", &CBotNet::m_StandDist,
                "m_LaserRescue", &CBotNet::m_LaserRescue,
                "m_LaserRescueDist", &CBotNet::m_LaserRescueDist,
                "m_AvoidFreezeRadius", &CBotNet::m_AvoidFreezeRadius,
                "m_HookDelay", &CBotNet::m_HookDelay,
                "m_ClientDelay", &CBotNet::m_ClientDelay,
                "m_CopyMoves", &CBotNet::m_CopyMoves,
                "m_CopyTargetID", &CBotNet::m_CopyTargetID,
                "m_RandomAim", &CBotNet::m_RandomAim,
                "m_RandomAimInterval", &CBotNet::m_RandomAimInterval,
                // Pathfinder
                "m_PathfinderEnabled", &CBotNet::m_PathfinderEnabled,
                "m_PfSimulatePlayers", &CBotNet::m_PfSimulatePlayers,
                "m_PfRays", &CBotNet::m_PfRays,
                "m_PfViewRadius", &CBotNet::m_PfViewRadius,
                "m_PfHookEnabled", &CBotNet::m_PfHookEnabled,
                // Kinodynamic
                "m_KinodynamicEnabled", &CBotNet::m_KinodynamicEnabled,
                "m_KinoCandidates", &CBotNet::m_KinoCandidates,
                "m_KinoTicks", &CBotNet::m_KinoTicks,
                "m_KinoHookAngles", &CBotNet::m_KinoHookAngles,
                "m_KinoCacheTicks", &CBotNet::m_KinoCacheTicks,
                "m_KinoShowPath", &CBotNet::m_KinoShowPath,
                "m_KinoShowField", &CBotNet::m_KinoShowField
        );

        // ===== CChat — exact C++ member names and methods =====
        lua.new_usertype<CChat>("CChat",
                "m_Mode", &CChat::m_Mode,
                "m_Show", &CChat::m_Show,
                "AddLine", [this](CChat &chat, int clientId, int team, const std::string &msg) {
                        chat.AddLine(clientId, team, msg.c_str());
                },
                "Echo", [](CChat &chat, const std::string &msg) {
                        chat.Echo(msg.c_str());
                },
                "SendChat", [this](CChat &chat, int team, const std::string &msg) {
                        chat.SendChat(team, msg.c_str());
                },
                "SendChatQueued", [](CChat &chat, const std::string &msg) {
                        chat.SendChatQueued(msg.c_str());
                },
                "EnableMode", [](CChat &chat, int team) {
                        chat.EnableMode(team);
                },
                "DisableMode", [](CChat &chat) {
                        chat.DisableMode();
                },
                "IsActive", [](CChat &chat) -> bool {
                        return chat.IsActive();
                },
                "ClearLines", [](CChat &chat) {
                        chat.ClearLines();
                }
        );

        // ===== CControls — exact C++ member names =====
        lua.new_usertype<CControls>("CControls",
                "m_aInputData", sol::property([this](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++)
                                t[i] = std::ref(c.m_aInputData[i]);
                        return t;
                }),
                "m_aLastData", sol::property([this](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++)
                                t[i] = std::ref(c.m_aLastData[i]);
                        return t;
                }),
                "m_aMousePos", sol::property([this](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++)
                                t[i] = c.m_aMousePos[i];
                        return t;
                }),
                "m_aTargetPos", sol::property([this](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++)
                                t[i] = c.m_aTargetPos[i];
                        return t;
                }),
                "ResetInput", [](CControls &c, int dummy) { c.ResetInput(dummy); }
        );

        // ===== CCollision — exact C++ method names =====
        lua.new_usertype<CCollision>("CCollision",
                "GetWidth", &CCollision::GetWidth,
                "GetHeight", &CCollision::GetHeight,
                "CheckPoint", sol::overload(
                        [](CCollision &c, float x, float y) -> bool { return c.CheckPoint(x, y); },
                        [](CCollision &c, vec2 pos) -> bool { return c.CheckPoint(pos); }
                ),
                "IsSolid", [](CCollision &c, int x, int y) -> int { return c.IsSolid(x, y); },
                "GetTile", &CCollision::GetTile,
                "GetFrontTile", &CCollision::GetFrontTile,
                "GetTileIndex", &CCollision::GetTileIndex,
                "GetFrontTileIndex", &CCollision::GetFrontTileIndex,
                "GetCollisionAt", [](CCollision &c, float x, float y) -> int { return c.GetCollisionAt(x, y); },
                "GetFrontCollisionAt", [](CCollision &c, float x, float y) -> int { return c.GetFrontCollisionAt(x, y); },
                "TestBox", &CCollision::TestBox,
                "IsOnGround", &CCollision::IsOnGround,
                "IntersectLine", [this](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before;
                        int res = c.IntersectLine(p0, p1, &col, &before);
                        auto t = lua.create_table();
                        t["hit"] = res;
                        t["Collision"] = col;
                        t["BeforeCollision"] = before;
                        return t;
                },
                "IntersectNoLaser", [this](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before;
                        int res = c.IntersectNoLaser(p0, p1, &col, &before);
                        auto t = lua.create_table();
                        t["hit"] = res;
                        t["Collision"] = col;
                        t["BeforeCollision"] = before;
                        return t;
                },
                "GetPureMapIndex", sol::overload(
                        [](CCollision &c, float x, float y) -> int { return c.GetPureMapIndex(x, y); },
                        [](CCollision &c, vec2 pos) -> int { return c.GetPureMapIndex(pos); }
                ),
                "IsTeleport", &CCollision::IsTeleport,
                "IsEvilTeleport", &CCollision::IsEvilTeleport,
                "IsSpeedup", &CCollision::IsSpeedup,
                "IsTune", &CCollision::IsTune,
                "GetSwitchType", &CCollision::GetSwitchType,
                "GetSwitchNumber", &CCollision::GetSwitchNumber,
                "GetSwitchDelay", &CCollision::GetSwitchDelay,
                "IsTimeCheckpoint", &CCollision::IsTimeCheckpoint,
                "IsNoLaser", [](CCollision &c, int x, int y) -> int { return c.IsNoLaser(x, y); },
                "SetCollisionAt", &CCollision::SetCollisionAt
        );

        // ===== CGameClient — THE BIG ONE — exact C++ member names =====
        // This is what makes Lua code look like C++ code.
        // gc.m_PredictedChar.m_Vel.x  ==  GameClient()->m_PredictedChar.m_Vel.x
        // gc.m_aClients[3].m_aName    ==  GameClient()->m_aClients[3].m_aName
        lua.new_usertype<CGameClient>("CGameClient",
                // Direct member variables — SAME NAMES as in gameclient.h
                "m_Chat", &CGameClient::m_Chat,
                "m_Controls", &CGameClient::m_Controls,
                "m_BotNet", &CGameClient::m_BotNet,
                "m_LocalCharacterPos", &CGameClient::m_LocalCharacterPos,
                "m_PredictedChar", &CGameClient::m_PredictedChar,
                "m_PredictedPrevChar", &CGameClient::m_PredictedPrevChar,
                "m_NewTick", &CGameClient::m_NewTick,
                "m_ServerMode", &CGameClient::m_ServerMode,
                "m_Teams", &CGameClient::m_Teams,
                // m_Snap — nested struct, expose key fields
                "m_Snap_m_LocalClientId", sol::property([this](CGameClient &gc) -> int { return gc.m_Snap.m_LocalClientId; }),
                "m_Snap_m_NumPlayers", sol::property([this](CGameClient &gc) -> int { return gc.m_Snap.m_NumPlayers; }),
                "m_Snap_m_HighestClientId", sol::property([this](CGameClient &gc) -> int { return gc.m_Snap.m_HighestClientId; }),
                "m_Snap_m_aCharacters", sol::property([this](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++)
                                if(gc.m_Snap.m_aCharacters[i].m_Active)
                                        t[i] = sol::ref(gc.m_Snap.m_aCharacters[i]);
                        return t;
                }),
                "m_Snap_m_apPlayerInfos", sol::property([this](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++)
                                t[i] = gc.m_Snap.m_apPlayerInfos[i] != nullptr;
                        return t;
                }),
                // m_aClients array — gc.m_aClients[3].m_aName
                "m_aClients", sol::property([this](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++)
                                t[i] = std::ref(gc.m_aClients[i]);
                        return t;
                }),
                // Collision() — method, exposed as property
                "Collision", sol::property([this](CGameClient &gc) -> CCollision* { return gc.Collision(); }),
                // CGameClient methods — SAME NAMES as in gameclient.h
                "SendKill", [](CGameClient &gc) { gc.SendKill(); },
                "SendSwitchTeam", [](CGameClient &gc, int team) { gc.SendSwitchTeam(team); },
                "Echo", [](CGameClient &gc, const std::string &msg) { gc.Echo(msg.c_str()); },
                "IsTeamPlay", [](CGameClient &gc) -> bool { return gc.IsTeamPlay(); },
                "IsWorldPaused", [](CGameClient &gc) -> bool { return gc.IsWorldPaused(); },
                "IsLocalCharSuper", [](CGameClient &gc) -> bool { return gc.IsLocalCharSuper(); },
                "IsOtherTeam", [](CGameClient &gc, int id) -> bool { return gc.IsOtherTeam(id); },
                "CurrentRaceTime", [](CGameClient &gc) -> int { return gc.CurrentRaceTime(); }
        );

        // ===== Set global objects — gc = GameClient() =====
        lua["gc"] = std::ref(*GameClient());

        // ===== CClient methods as 'cl' table =====
        // These are methods (not member variables) so they can't be direct usertypes
        auto cl = lua["cl"].get_or_create<sol::table>();
        cl.set_function("Connect", [this](const std::string &addr) {
                Client()->Connect(addr.c_str());
        });
        cl.set_function("Disconnect", [this]() {
                Client()->Disconnect();
        });
        cl.set_function("EnterGame", [this]() {
                Client()->EnterGame(0);
        });
        cl.set_function("RconAuth", [this](const std::string &user, const std::string &pass) {
                Client()->RconAuth(user.c_str(), pass.c_str(), 0);
        });
        cl.set_function("Rcon", [this](const std::string &cmd) {
                Client()->Rcon(cmd.c_str());
        });
        cl.set_function("RconAuthed", [this]() -> bool {
                return Client()->RconAuthed();
        });
        cl.set_function("GameTick", [this]() -> int {
                return Client()->GameTick(0);
        });
        cl.set_function("GameTickSpeed", [this]() -> int {
                return Client()->GameTickSpeed();
        });
        cl.set_function("ConnectionProblems", [this]() -> bool {
                return Client()->ConnectionProblems();
        });
        cl.set_function("DummyConnect", [this](int d) {
                Client()->DummyConnect(d);
        });
        cl.set_function("DummyDisconnect", [this](int d) {
                Client()->DummyDisconnect(d, "Lua script");
        });
        cl.set_function("DummyConnected", [this](int d) -> bool {
                return Client()->DummyConnected(d);
        });
        cl.set_function("DummyCount", [this]() -> int {
                return Client()->DummyCount();
        });
        cl.set_function("Quit", [this]() {
                Client()->Quit();
        });
        cl.set_function("Restart", [this]() {
                Client()->Restart();
        });
        cl.set_function("PlayerName", [this]() -> std::string {
                return std::string(Client()->PlayerName());
        });
        cl.set_function("SendPackMsgActive", [this](CMsgPacker *pMsg, int flags) {
                Client()->SendMsgActive(pMsg, flags);
        });

        // ===== chat.fake() — still useful convenience =====
        auto chatNs = lua["chat"].get_or_create<sol::table>();
        chatNs.set_function("fake", [this](const std::string &name, const std::string &msg, sol::optional<int> team) {
                char aBuf[512];
                str_format(aBuf, sizeof(aBuf), "%s: %s", name.c_str(), msg.c_str());
                int Team = team.value_or(0);
                GameClient()->m_Chat.AddLine(CChat::CLIENT_MSG, Team, aBuf);
        });

        // ===== API info =====
        lua.set_function("raw_api_info", []() -> std::string {
                return R"(
ZMajor Raw C++ API — ALL NAMES MATCH C++ EXACTLY:

  gc = GameClient() reference (CGameClient&)

  -- Read/write game state EXACTLY like C++ code:
  gc.m_PredictedChar.m_Vel.x
  gc.m_PredictedChar.m_Pos
  gc.m_PredictedChar.m_Input.m_Direction = 1
  gc.m_PredictedChar.m_Input.m_Jump = 1
  gc.m_LocalCharacterPos
  gc.m_aClients[3].m_aName
  gc.m_aClients[3].m_Predicted.m_Vel
  gc.m_aClients[3].m_Solo
  gc.m_aClients[3].m_FreezeEnd
  gc.m_BotNet.m_AttackEnabled = true
  gc.m_BotNet.m_MoveEnabled = false
  gc.m_Chat:AddLine(-2, 0, "hello")
  gc.m_Chat:Echo("message")
  gc.m_Controls.m_aInputData[0].m_Direction = 1
  gc.m_Controls.m_aInputData[0].m_Fire = 1

  gc:SendKill()
  gc:SendSwitchTeam(0)
  gc:Echo("msg")
  gc:Collision():IsSolid(tx, ty)
  gc:Collision():IntersectLine(p0, p1)

  cl.Connect("addr")
  cl.RconAuth("user", "pass")
  cl.Rcon("command")
  cl.DummyConnect(1)

  chat.fake("Name", "Message")

  CCharacterCore:  m_Pos m_Vel m_HookPos m_HookDir m_HookState
    m_HookedPlayer m_ActiveWeapon m_Jumped m_JumpedTotal m_Jumps
    m_Direction m_Angle m_Input m_Id
    m_Solo m_Jetpack m_Super m_Invincible m_DeepFrozen m_LiveFrozen
    m_FreezeStart m_FreezeEnd m_IsInFreeze m_EndlessHook m_EndlessJump
    m_CollisionDisabled m_HasTelegunGun/Grenade/Laser
    m_HammerHitDisabled m_GrenadeHitDisabled etc.

  CNetObj_PlayerInput: m_Direction m_TargetX m_TargetY
    m_Jump m_Fire m_Hook m_PlayerFlags m_WantedWeapon

  CNetObj_Character: m_X m_Y m_VelX m_VelY m_Angle m_Direction
    m_Weapon m_AmmoCount m_Health m_Armor m_PlayerFlags
    m_HookedPlayer m_HookState m_HookTick m_AttackTick

  CClientData: m_aName m_aClan m_Country m_aSkinName m_Team
    m_Active m_Friend m_Foe m_Afk m_Paused m_Spec
    m_Solo m_Jetpack m_Super m_Invincible m_DeepFrozen m_LiveFrozen
    m_FreezeEnd m_AuthLevel m_Predicted m_PrevPredicted
    m_CollisionDisabled m_EndlessHook m_EndlessJump etc.

  CBotNet: m_AttackEnabled m_MoveEnabled m_AutoAim m_AutoFire
    m_RescueFrozen m_KillOnFreeze m_AvoidFreeze m_MainID
    m_PathfinderEnabled m_KinodynamicEnabled etc.

  CChat: m_Mode m_Show AddLine Echo SendChat SendChatQueued
    EnableMode DisableMode IsActive ClearLines

  CControls: m_aInputData m_aLastData m_aMousePos m_aTargetPos
    ResetInput

  CCollision: GetWidth GetHeight CheckPoint IsSolid GetTile
    GetFrontTile IntersectLine IntersectNoLaser
    IsTeleport IsSpeedup IsTune SetCollisionAt etc.
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
