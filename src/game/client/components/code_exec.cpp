#include <game/client/components/code_exec.h>
#include <game/client/components/chat.h>

#include <game/client/gameclient.h>
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
        m_CursorPos(0),
        m_SelectionStart(0),
        m_SelectionEnd(0),
        m_ScrollY(0.0f),
        m_ScrollYChange(0.0f)
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
