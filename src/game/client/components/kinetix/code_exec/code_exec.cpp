#include <game/client/components/kinetix/code_exec/code_exec.h>
#include <game/client/components/chat.h>
#include <game/client/components/camera.h>
#include <game/client/components/controls.h>
#include <game/client/components/effects.h>
#include <game/client/components/flow.h>
#include <game/client/components/emoticon.h>
#include <game/client/components/motd.h>
#include <game/client/components/broadcast.h>
#include <game/client/components/voting.h>
#include <game/client/components/spectator.h>
#include <game/client/components/ghost.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/bot_control.h>
#include <game/client/components/kinetix/irc.h>
#include <game/client/components/skins.h>
#include <game/client/components/particles.h>
#include <game/client/components/sounds.h>
#include <game/client/components/damageind.h>
#include <game/client/components/mapimages.h>
#include <game/client/components/mapsounds.h>
#include <game/client/components/menus.h>
#include <game/client/components/binds.h>
#include <game/client/components/console.h>
#include <game/client/components/infomessages.h>
#include <game/client/components/countryflags.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/statboard.h>
#include <game/client/tooltips.h>
#include <game/client/components/debughud.h>
#include <game/client/components/hud.h>
#include <game/client/components/important_alert.h>
#include <game/client/components/race_demo.h>
#include <game/client/components/local_server.h>
#include <game/client/components/nameplates.h>
#include <game/client/components/freezebars.h>
#include <game/client/components/items.h>

#include <game/client/gameclient.h>
#include <game/gamecore.h>
#include <game/collision.h>
#include <game/layers.h>
#include <game/teamscore.h>
#include <game/client/prediction/gameworld.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/projectile.h>
#include <game/client/prediction/entities/pickup.h>
#include <game/client/prediction/entities/door.h>
#include <game/client/prediction/entities/dragger.h>
#include <game/client/prediction/entities/plasma.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <base/fs.h>
#include <base/str.h>
#include <base/io.h>

#if defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
#endif
#include <base/time.h>
#include <engine/console.h>
#include <engine/client/client.h>
#include <base/vmath.h>

#include <lua.hpp>
#include <LuaBridge/LuaBridge.h>

#include <engine/http.h>
#include <engine/shared/json.h>

#include <chrono>
#include <cstdio>
#include <cstring>

static CCodeExec *s_pHookInstance = nullptr;

void CCodeExec::LuaStepHook(lua_State *L, lua_Debug *ar)
{
        (void)ar;
        if(!s_pHookInstance)
                return;
        if(s_pHookInstance->m_StopRequested.load(std::memory_order_relaxed))
                luaL_error(L, "script stopped");
        // kx_exec_debug: log every function call/return of the script.
        if(g_Config.m_KxExecSettings && g_Config.m_KxExecDebug && (ar->event == LUA_HOOKCALL || ar->event == LUA_HOOKTAILCALL || ar->event == LUA_HOOKRET))
        {
                // lua_getinfo reads ar->i_ci itself — pass the hook's own ar
                // (already filled by Lua). A fresh lua_Debug has garbage i_ci
                // and crashes inside lua_getinfo.
                if(lua_getinfo(L, "nS", ar))
                {
                        char aBuf[512];
                        str_format(aBuf, sizeof(aBuf), "%s %s [%s:%d]",
                                ar->event == LUA_HOOKRET ? "return" : "call",
                                ar->name ? ar->name : "?",
                                ar->short_src[0] ? ar->short_src : "?",
                                ar->linedefined);
                        s_pHookInstance->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua_debug", aBuf);
                }
        }
        // Yield only on count hooks. Call/return hooks fire at C-function
        // entries and metamethod dispatches (luaD_callnoyield), where yielding
        // is illegal: lua_yield turns into "attempt to yield across a C-call
        // boundary" and the script's own pcalls record it as a test failure.
        if(ar->event == LUA_HOOKCOUNT)
        {
                // Mark the wall-time exclusion for SoFE (LuaCT): the test
                // running across this yield includes the suspended gap.
                s_pHookInstance->m_YieldedSinceMark = true;
                s_pHookInstance->m_InstructionCounter += HOOK_INSTRUCTION_INTERVAL;
                if(s_pHookInstance->m_InstructionCounter >= RESUME_INSTRUCTION_BUDGET)
                {
                        s_pHookInstance->m_InstructionCounter = 0;
                        lua_yield(L, 0);
                }
        }
}

// print() — outputs to in-game chat. Raw lua_CFunction: variadic without
// any per-argument template machinery.
int CCodeExec::LuaPrint(lua_State *L)
{
        CCodeExec *pSelf = (CCodeExec *)lua_touserdata(L, lua_upvalueindex(1));
        std::string Result;
        const int NumArgs = lua_gettop(L);
        for(int i = 1; i <= NumArgs; i++)
        {
                if(i > 1)
                        Result += "\t";
                size_t Len = 0;
                const char *pStr = luaL_tolstring(L, i, &Len);
                if(pStr)
                        Result.append(pStr, Len);
                else
                        Result += lua_typename(L, lua_type(L, i));
                lua_pop(L, 1);
        }
        pSelf->GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, Result.c_str());
        // v6.13: ArrayAgent tool capture — duplicate the output for the tool
        // result.
        if(pSelf->m_ToolCapture)
                pSelf->m_vToolOutput.push_back(Result);
        return 0;
}

CCodeExec::CCodeExec() :
        m_pLua(nullptr),
        m_CoroutineRef(-1),
        m_State(STATE_IDLE),
        m_StopRequested(false),
        m_WasRunning(false),
        m_InstructionCounter(0),
        m_pCoroutine(nullptr),
        m_InResume(false),
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
        Stop();
        if(m_pLua)
        {
                lua_close(m_pLua);
                m_pLua = nullptr;
        }
}

void CCodeExec::OnConsoleInit()
{
        Console()->Register("kx_exec", "s[code]", CFGFLAG_CLIENT, ConExec, this, "Execute Lua code");
        Console()->Register("kx_exec_stop", "", CFGFLAG_CLIENT, ConStop, this, "Stop running Lua script");
        Console()->Register("kx_luact", "", CFGFLAG_CLIENT, ConLuaCT, this, "Start/stop/clear the Lua Compatibility Test");
}

void CCodeExec::OnInit()
{
        s_pHookInstance = this;
        m_pLua = luaL_newstate();
        // The sol2 version opened exactly the standard library set; luaL_openlibs
        // opens the same set for Lua 5.4 (base, package, coroutine, table, io,
        // os, string, utf8, math, debug).
        luaL_openlibs(m_pLua);

        lua_pushlightuserdata(m_pLua, this);
        lua_pushcclosure(m_pLua, LuaPrint, 1);
        lua_setglobal(m_pLua, "print");

        // The stop/yield protection owns the coroutine's debug hook. Scripts
        // must not replace it, so debug.sethook is disabled entirely.
        lua_getglobal(m_pLua, "debug");
        if(lua_istable(m_pLua, -1))
        {
                lua_pushcfunction(m_pLua, [](lua_State *L) -> int {
                        return luaL_error(L, "debug.sethook is not allowed");
                });
                lua_setfield(m_pLua, -2, "sethook");
        }
        lua_pop(m_pLua, 1);

        DetectShells();

        try
        {
                RegisterBindings();
                RegisterRawAPI();
        }
        catch(const std::exception &e)
        {
                char aBuf[512];
                str_format(aBuf, sizeof(aBuf), "binding registration failed: %s", e.what());
                Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "code_exec", aBuf);
        }
}

void CCodeExec::OnShutdown()
{
        Stop();
        if(m_pLua) { lua_close(m_pLua); m_pLua = nullptr; }
}

void CCodeExec::OnRender()
{
        if(m_State.load() != STATE_RUNNING || !m_pCoroutine)
                return;
        const CScreenRect SaveScreen = Graphics()->GetScreen();
        const ColorRGBA SaveTextColor = TextRender()->GetTextColor();
        const ColorRGBA SaveTextOutlineColor = TextRender()->GetTextOutlineColor();
        m_InstructionCounter = 0;
        m_InResume = true;
        int NumResults = 0;
        const int Status = lua_resume(m_pCoroutine, nullptr, 0, &NumResults);
        m_InResume = false;
        TextRender()->TextColor(SaveTextColor);
        TextRender()->TextOutlineColor(SaveTextOutlineColor);
        Graphics()->MapScreen(SaveScreen);
        if(Status == LUA_OK || Status == LUA_YIELD)
                lua_pop(m_pCoroutine, NumResults);
        if(Status != LUA_YIELD)
        {
                if(Status != LUA_OK && !m_StopRequested.load())
                {
                        const char *pErrMsg = lua_tostring(m_pCoroutine, -1) ? lua_tostring(m_pCoroutine, -1) : "Unknown execution error";
                        str_copy(m_aLastError, pErrMsg, sizeof(m_aLastError));
                }
                if(m_CoroutineRef != -1)
                {
                        luaL_unref(m_pLua, LUA_REGISTRYINDEX, m_CoroutineRef);
                        m_CoroutineRef = -1;
                }
                m_pCoroutine = nullptr;
                m_State = (Status == LUA_OK || m_StopRequested.load()) ? STATE_IDLE : STATE_ERROR;
                if(m_State.load() == STATE_ERROR)
                {
                        char aBuf[1024];
                        str_format(aBuf, sizeof(aBuf), "Lua error: %s", m_aLastError);
                        GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, aBuf);
                        // v6.13: ArrayAgent tool capture — the error is part of
                        // the tool result.
                        if(m_ToolCapture)
                                m_vToolOutput.push_back(aBuf);
                }
                m_WasRunning = false;
                if(m_LuaCTWasRunning)
                {
                        m_LuaCTWasRunning = false;
                        m_LuaCTState = LUACT_RESULTS;
                }
        }
}

void CCodeExec::OnReset() { Stop(); }

// v6.13: ArrayAgent tool capture — print() output and errors are collected
// into m_vToolOutput while the capture is active.
void CCodeExec::ToolCaptureStart()
{
	m_ToolCapture = true;
	m_vToolOutput.clear();
}

std::vector<std::string> CCodeExec::ToolCaptureFinish()
{
	m_ToolCapture = false;
	std::vector<std::string> vOut = std::move(m_vToolOutput);
	m_vToolOutput.clear();
	return vOut;
}

std::vector<std::string> CCodeExec::ToolCapturePeek() const
{
	return m_vToolOutput;
}

const char *CCodeExec::GetLastError() const { return m_aLastError; }

bool CCodeExec::Execute(const char *pCode, const char *pChunkName)
{
        if(!m_pLua || !pCode || m_InResume) return false;
        Stop();
        m_aLastError[0] = '\0';

        char aChunk[128];
        str_format(aChunk, sizeof(aChunk), "=%s", pChunkName);

        lua_State *L = m_pLua;

        // Sanitize the source start. If the editor buffer got a stray UTF-8
        // BOM (EF BB BF) or a leading continuation byte (10xxxxxx) at offset 0
        // (which can happen after certain paste/backspace edge cases), Lua's
        // load() fails with "unexpected symbol near '<0xAE>'". Skip leading
        // BOM and any stray continuation bytes so the script still runs.
        const char *pSrc = pCode;
        // skip UTF-8 BOM
        if((unsigned char)pSrc[0] == 0xEF && (unsigned char)pSrc[1] == 0xBB && (unsigned char)pSrc[2] == 0xBF)
                pSrc += 3;
        // skip leading continuation bytes (10xxxxxx) that have no lead byte
        while((unsigned char)pSrc[0] >= 0x80 && (unsigned char)pSrc[0] <= 0xBF)
                pSrc++;

        if(luaL_loadbuffer(L, pSrc, str_length(pSrc), aChunk) != LUA_OK)
        {
                const char *pErrMsg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Unknown load error";
                str_copy(m_aLastError, pErrMsg, sizeof(m_aLastError));
                char aBuf[1024]; str_format(aBuf, sizeof(aBuf), "Lua load error: %s", pErrMsg);
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, aBuf);
                // v6.13: ArrayAgent tool capture — the load error is part of
                // the tool result.
                if(m_ToolCapture)
                        m_vToolOutput.push_back(aBuf);
                lua_pop(L, 1);
                m_State = STATE_ERROR;
                return false;
        }

        lua_newthread(L);
        m_pCoroutine = lua_tothread(L, -1);
        m_CoroutineRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_xmove(L, m_pCoroutine, 1);
        lua_sethook(m_pCoroutine, LuaStepHook,
                (g_Config.m_KxExecSettings && g_Config.m_KxExecDebug) ? (LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET) : LUA_MASKCOUNT,
                HOOK_INSTRUCTION_INTERVAL);
        m_InstructionCounter = 0;

        m_StopRequested = false;
        m_WasRunning = true;
        m_State = STATE_RUNNING;
        return true;
}

void CCodeExec::Stop()
{
        if(m_InResume)
                return;
        m_StopRequested = true;
        m_WasRunning = false;
        m_pCoroutine = nullptr;
        m_State = STATE_IDLE;
        if(m_CoroutineRef != -1)
        {
                if(m_pLua)
                        luaL_unref(m_pLua, LUA_REGISTRYINDEX, m_CoroutineRef);
                m_CoroutineRef = -1;
        }
}

// =============================================================
// Shell detection — locate shells used for OS commands
// =============================================================
char CCodeExec::s_aaShellPaths[CCodeExec::NUM_SHELLS][300] = {{""}};
bool CCodeExec::s_aShellFound[CCodeExec::NUM_SHELLS] = {false};

#if defined(CONF_FAMILY_WINDOWS)
static bool ExecFileExists(const char *pPath)
{
        return pPath[0] && fs_is_file(pPath);
}

// Scans %PATH% for pExe; skips Windows system dirs so WSL's bash.exe is not
// mistaken for Git Bash / MSYS.
static bool ExecPathFind(const char *pExe, char *pOut, int OutSize)
{
        static char aPath[32768];
        if(!GetEnvironmentVariableA("PATH", aPath, sizeof(aPath)) || !aPath[0])
                return false;
        char *pCtx = nullptr;
        for(char *pTok = strtok_s(aPath, ";", &pCtx); pTok; pTok = strtok_s(nullptr, ";", &pCtx))
        {
                if((strstr(pTok, "System32") || strstr(pTok, "WindowsApps")) &&
                        (str_comp(pExe, "bash.exe") == 0 || str_comp(pExe, "sh.exe") == 0))
                        continue;
                char aCand[512];
                str_format(aCand, sizeof(aCand), "%s\\%s", pTok, pExe);
                if(ExecFileExists(aCand))
                {
                        str_copy(pOut, aCand, OutSize);
                        return true;
                }
        }
        return false;
}
#endif

void CCodeExec::DetectShells()
{
#if defined(CONF_FAMILY_WINDOWS)
        const char *const apPsPaths[] = {
                "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
        };
        const char *const apCmdPaths[] = {
                "C:\\Windows\\System32\\cmd.exe",
        };
        const char *const apBashPaths[] = {
                "C:\\Program Files\\Git\\bin\\bash.exe",
                "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
        };
        const char *const apMsysPaths[] = {
                "C:\\msys64\\usr\\bin\\sh.exe",
                "C:\\msys32\\usr\\bin\\sh.exe",
                "C:\\msys64\\usr\\bin\\bash.exe",
        };
        for(const char *p : apPsPaths)
                if(ExecFileExists(p)) { str_copy(s_aaShellPaths[SHELL_POWERSHELL], p, sizeof(s_aaShellPaths[0])); s_aShellFound[SHELL_POWERSHELL] = true; break; }
        if(!s_aShellFound[SHELL_POWERSHELL] && ExecPathFind("powershell.exe", s_aaShellPaths[SHELL_POWERSHELL], sizeof(s_aaShellPaths[0])))
                s_aShellFound[SHELL_POWERSHELL] = true;
        for(const char *p : apCmdPaths)
                if(ExecFileExists(p)) { str_copy(s_aaShellPaths[SHELL_CMD], p, sizeof(s_aaShellPaths[0])); s_aShellFound[SHELL_CMD] = true; break; }
        for(const char *p : apBashPaths)
                if(ExecFileExists(p)) { str_copy(s_aaShellPaths[SHELL_GITBASH], p, sizeof(s_aaShellPaths[0])); s_aShellFound[SHELL_GITBASH] = true; break; }
        if(!s_aShellFound[SHELL_GITBASH] && ExecPathFind("bash.exe", s_aaShellPaths[SHELL_GITBASH], sizeof(s_aaShellPaths[0])))
                s_aShellFound[SHELL_GITBASH] = true;
        for(const char *p : apMsysPaths)
                if(ExecFileExists(p)) { str_copy(s_aaShellPaths[SHELL_MSYS], p, sizeof(s_aaShellPaths[0])); s_aShellFound[SHELL_MSYS] = true; break; }
        if(!s_aShellFound[SHELL_MSYS] && ExecPathFind("sh.exe", s_aaShellPaths[SHELL_MSYS], sizeof(s_aaShellPaths[0])))
                s_aShellFound[SHELL_MSYS] = true;
#else
        str_copy(s_aaShellPaths[SHELL_CMD], "/bin/sh", sizeof(s_aaShellPaths[0]));
        s_aShellFound[SHELL_CMD] = fs_is_file("/bin/sh");
#endif
}

// =============================================================
// LuaCT (Lua Compatibility Test)
// =============================================================
int CCodeExec::LuaCTRecord(lua_State *L)
{
        CCodeExec *pSelf = (CCodeExec *)lua_touserdata(L, lua_upvalueindex(1));
        SLuaCTResult Result;
        Result.m_Name = lua_isstring(L, 1) ? lua_tostring(L, 1) : "?";
        Result.m_Ok = lua_toboolean(L, 2) != 0;
        if(lua_isstring(L, 3) && !Result.m_Ok)
                Result.m_Error = lua_tostring(L, 3);
        // Optional 4th argument: wall-clock seconds of the test (SoFE).
        if(lua_isnumber(L, 4))
        {
                Result.m_Time = lua_tonumber(L, 4);
                Result.m_Timed = !pSelf->m_YieldedSinceMark;
        }
        pSelf->m_YieldedSinceMark = false;
        pSelf->m_vLuaCTResults.push_back(std::move(Result));
        return 0;
}

// kx_now() — high-resolution wall clock in seconds (steady_clock), used by
// the LuaCT chunk for the SoFE (speed of function execution) measurements.
static int LuaNow(lua_State *L)
{
        const auto Now = std::chrono::steady_clock::now().time_since_epoch();
        lua_pushnumber(L, std::chrono::duration<double>(Now).count());
        return 1;
}

// The LuaCT chunk. Structural checks only: existence/access of every
// registered global, class, namespace entry, and all members enumerated from
// the metatables of the two live roots (gc, config). No state-dependent or
// side-effecting calls. Results go through kx_record(name, ok[, err]).
static const char *const s_pLuaCTCode = R"LUA(
local function T(name, fn)
	local t0 = kx_now()
	local ok, v = pcall(fn)
	local dt = kx_now() - t0
	local msg = ""
        if not ok then
                msg = tostring(v)
        elseif v == nil then
                if name:find("m_pGameInfoObj", 1, true) or name:find("m_pGameDataObj", 1, true) then
                        ok = true
                        msg = ""
                else
                        msg = "= nil"
                end
        end
	kx_record(name, ok, msg, dt)
end
local function TestObject(path, obj)
        local mt = debug.getmetatable(obj)
        if type(mt) ~= "table" then return end
        local names = {}
        for k, v in pairs(mt) do
                if type(k) == "string" and not k:match("^__") then
                        names[k] = true
                        T(path .. "." .. k, function() return obj[k] end)
                elseif type(k) == "userdata" and type(v) == "table" then
                        for n in pairs(v) do
                                if type(n) == "string" and not n:match("^__") and not names[n] then
                                        names[n] = true
                                        T(path .. "." .. n, function() return obj[n] end)
                                end
                        end
                end
        end
        for n in pairs(names) do
                local ok, v = pcall(function() return obj[n] end)
                if ok and type(v) == "userdata" then
                        TestObject(path .. "." .. n, v)
                end
        end
end
T("CONST", function() return CONST end)
if type(CONST) == "table" then
        for k, v in pairs(CONST) do
                T("CONST." .. k, function() return v end)
        end
end
T("vec2", function() return vec2 end)
local okv, v = pcall(vec2, 1, 2)
if okv and v then
        T("vec2.x", function() return v.x == 1 and v end)
        T("vec2.y", function() return v.y == 2 and v end)
        T("vec2.__add", function() return v + vec2(1, 1) end)
        T("vec2.__sub", function() return v - vec2(1, 1) end)
        T("vec2.__mul", function() return v * 2 end)
        T("vec2.__div", function() return v / 2 end)
        T("vec2.__unm", function() return -v end)
        T("vec2.length", function() return v.length end)
        T("vec2.normalize", function() return v.normalize end)
        T("vec2.dot", function() return v.dot end)
        T("vec2.distance", function() return v.distance end)
        T("vec2.angle", function() return v.angle end)
end
for _, name in ipairs({"cl", "net", "console", "gfx", "sound", "input", "text"}) do
        T(name, function() return _G[name] end)
        local t = _G[name]
        if type(t) == "table" then
                for k, v in pairs(t) do
                        if type(k) == "string" and not k:match("^__") then
                                T(name .. "." .. k, function() return t[k] end)
                        elseif type(k) == "userdata" and type(v) == "table" then
                                for n in pairs(v) do
                                        if type(n) == "string" and not n:match("^__") then
                                                T(name .. "." .. n, function() return t[n] end)
                                        end
                                end
                        end
                end
        end
end
local classes = {
        "CNetObj_PlayerInput", "CNetObj_CharacterCore", "CNetObj_Character", "CNetObj_PlayerInfo",
        "CNetObj_GameInfo", "CNetObj_GameData", "CNetObj_Flag", "CNetObj_SpectatorInfo",
        "CNetObj_DDNetCharacter", "CTuningParams", "CTeamsCore", "SSwitchers", "CWorldCore",
        "CCharacterCore", "CClientStats", "CGameInfo", "CSpecInfo", "CSnapState", "CClientData",
        "CCollision", "CCamera", "CChat", "CControls", "CEffects", "CFlow", "CVoting",
        "CSpectator", "CEmoticon", "CMotd", "CBroadcast", "CGhost", "CBotControl", "CBotNet",
        "CIRC", "CParticles", "CDamageInd", "CSounds", "CMenus", "CBinds", "CGameConsole",
        "CHud", "CRaceDemo", "CLocalServer", "CImportantAlert", "CScoreboard", "CStatboard",
        "CTile", "CMapItemLayer", "CMapItemGroup", "CMapItemLayerTilemap", "CLayers",
        "CEntity", "CCharacter", "CProjectile", "CLaser", "CPickup", "CSkins", "CCodeExec",
        "ColorRGBA", "CGameWorld", "CGameClient", "CPlayers", "CNamePlates", "CFreezeBars",
        "CItems", "CMapImages", "CMapSounds", "CBackground", "CMenuBackground", "CMapLayers",
        "CDebugHud", "CTouchControls", "CCountryFlags", "CSkins7", "CRenderTools", "CRenderMap",
        "CInfoMessages", "CCensor", "CTooltips", "CAimBot", "CAvoidFreeze", "CEsp",
        "CTrajectory", "CClickGui", "CKinetixLines", "CKeyBinder", "NetJob", "SPopenJob",
}
for _, c in ipairs(classes) do
        T(c, function() return _G[c] end)
end
T("gc", function() return gc end)
if gc then TestObject("gc", gc) end
T("config", function() return config end)
if config then TestObject("config", config) end
T("print", function() return type(print) == "function" and print end)
T("os.execute", function() return type(os.execute) == "function" and os.execute end)
T("io.popen", function() return type(io.popen) == "function" and io.popen end)
)LUA";

void CCodeExec::LuaCTStart()
{
        if(!m_pLua)
                return;
        Stop();
        m_vLuaCTResults.clear();
        lua_pushlightuserdata(m_pLua, this);
        lua_pushcclosure(m_pLua, LuaCTRecord, 1);
        lua_setglobal(m_pLua, "kx_record");
        lua_register(m_pLua, "kx_now", LuaNow);
        m_YieldedSinceMark = false;
        if(!Execute(s_pLuaCTCode, "LuaCT"))
        {
                m_LuaCTState = LUACT_IDLE;
                return;
        }
        m_LuaCTState = LUACT_RUNNING;
        m_LuaCTWasRunning = true;
}

void CCodeExec::LuaCTStop()
{
        if(m_LuaCTState == LUACT_RUNNING)
                Stop();
}

void CCodeExec::LuaCTClear()
{
        m_vLuaCTResults.clear();
        m_LuaCTState = LUACT_IDLE;
        m_LuaCTWasRunning = false;
}

// =============================================================
// GitHub script repository — storage/scripts in the client repo
// =============================================================
// The repo carries storage/scripts/scripts.json with metadata of every
// script; the client downloads ONLY the metadata list (so GitHub's rate
// limit is never hit by bulk fetching) and downloads an individual .lua
// file when the user presses its Load button.
static const char *const s_pGitHubRawBase =
        "https://raw.githubusercontent.com/lothiann/Kinetix-Client/master/storage/scripts/";

static std::shared_ptr<IHttpRequest> GitHubRequest(const char *pUrl)
{
        std::shared_ptr<IHttpRequest> pReq = HttpGet(pUrl);
        pReq->WriteToMemory();
        pReq->LogProgress(HTTPLOG::FAILURE);
        pReq->FailOnErrorStatus(false);
        pReq->MaxResponseSize(16 * 1024 * 1024);
        pReq->Timeout(CTimeout{4000, 20000, 1, 30});
        return pReq;
}

void CCodeExec::GitHubUpdateList()
{
        if(m_GitHubState == GITHUB_UPDATING)
                return;
        m_aGitHubError[0] = '\0';
        m_pGitHubListJob = GitHubRequest((std::string(s_pGitHubRawBase) + "scripts.json").c_str());
        Http()->Run(m_pGitHubListJob);
        m_GitHubState = GITHUB_UPDATING;
}

void CCodeExec::GitHubLoadScript(int Idx, bool Select)
{
        if(m_GitHubLoadState == GITHUB_LOAD_LOADING || Idx < 0 || Idx >= (int)m_vGitHubScripts.size())
                return;
        m_aGitHubLoadError[0] = '\0';
        m_GitHubLoadSelect = Select;
        m_GitHubLoadTarget = m_vGitHubScripts[Idx].m_File;
        const std::string Url = std::string(s_pGitHubRawBase) + m_GitHubLoadTarget;
        m_pGitHubLoadJob = GitHubRequest(Url.c_str());
        Http()->Run(m_pGitHubLoadJob);
        m_GitHubLoadState = GITHUB_LOAD_LOADING;
}

void CCodeExec::GitHubClearLoadDone()
{
        m_GitHubLoadState = GITHUB_LOAD_IDLE;
        m_aGitHubLoadFile[0] = '\0';
        m_GitHubLoadContent.clear();
}

void CCodeExec::GitHubUpdateJobs()
{
        if(m_pGitHubListJob && m_pGitHubListJob->Done())
        {
                std::shared_ptr<IHttpRequest> pReq = m_pGitHubListJob;
                m_pGitHubListJob = nullptr;
                GitHubFinishList(pReq.get());
        }
        if(m_pGitHubLoadJob && m_pGitHubLoadJob->Done())
        {
                std::shared_ptr<IHttpRequest> pReq = m_pGitHubLoadJob;
                m_pGitHubLoadJob = nullptr;
                GitHubFinishLoad(pReq.get());
        }
}

void CCodeExec::GitHubFinishList(IHttpRequest *pReq)
{
        if(pReq->State() != EHttpState::DONE || pReq->StatusCode() != 200)
        {
                m_GitHubState = GITHUB_ERROR;
                str_format(m_aGitHubError, sizeof(m_aGitHubError), "download failed (HTTP %d)", pReq->StatusCode());
                return;
        }
        unsigned char *pData = nullptr;
        size_t Len = 0;
        pReq->Result(&pData, &Len);
        if(!pData || Len == 0)
        {
                m_GitHubState = GITHUB_ERROR;
                str_copy(m_aGitHubError, "empty response", sizeof(m_aGitHubError));
                return;
        }
        json_value *pRoot = JsonParse((const json_char *)pData, Len);
        if(!pRoot || pRoot->type != json_object)
        {
                json_value_free(pRoot);
                m_GitHubState = GITHUB_ERROR;
                str_copy(m_aGitHubError, "invalid scripts.json", sizeof(m_aGitHubError));
                return;
        }
        m_vGitHubScripts.clear();
        m_GitHubListUpdated.clear();
        m_GitHubLastSupportedVersion.clear();
        if(const json_value *pField = json_object_get(pRoot, "list_updated"))
                m_GitHubListUpdated = json_string_get(pField);
        if(const json_value *pField = json_object_get(pRoot, "last_supported_version"))
                m_GitHubLastSupportedVersion = json_string_get(pField);
        const json_value *pScripts = json_object_get(pRoot, "scripts");
        if(pScripts && pScripts->type == json_array)
        {
                for(int i = 0; i < json_array_length(pScripts); i++)
                {
                        const json_value *pObj = json_array_get(pScripts, i);
                        if(!pObj || pObj->type != json_object)
                                continue;
                        SGitHubScript Script;
                auto Field = [&](const char *pName) -> const char * {
                        const json_value *pValue = json_object_get(pObj, pName);
                        // Null-safe: a missing / non-string key must never crash
                        // the client (std::string::operator=(nullptr) does).
                        return pValue && pValue->type == json_string ? json_string_get(pValue) : "";
                        };
                        Script.m_File = Field("file");
                        Script.m_Name = Field("name");
                        Script.m_Version = Field("version");
                        Script.m_Author = Field("author");
                        Script.m_Description = Field("description");
                        if(const json_value *pValue = json_object_get(pObj, "ce_version"))
                                Script.m_CeVersion = json_int_get(pValue);
                        if(!Script.m_File.empty())
                                m_vGitHubScripts.push_back(std::move(Script));
                }
        }
        json_value_free(pRoot);
        m_GitHubState = GITHUB_READY;
}

void CCodeExec::GitHubFinishLoad(IHttpRequest *pReq)
{
        if(pReq->State() != EHttpState::DONE || pReq->StatusCode() != 200)
        {
                m_GitHubLoadState = GITHUB_LOAD_ERROR;
                str_format(m_aGitHubLoadError, sizeof(m_aGitHubLoadError), "download failed (HTTP %d)", pReq->StatusCode());
                return;
        }
        unsigned char *pData = nullptr;
        size_t Len = 0;
        pReq->Result(&pData, &Len);
        if(!pData || Len == 0)
        {
                m_GitHubLoadState = GITHUB_LOAD_ERROR;
                str_copy(m_aGitHubLoadError, "empty response", sizeof(m_aGitHubLoadError));
                return;
        }
        // v6.12: Load keeps the script in memory only — the ClickGUI loads it
        // into the execution buffer without any disk write or Path line.
        // Download does the disk write (Kinetix/Scripts).
        str_copy(m_aGitHubLoadFile, m_GitHubLoadTarget.c_str(), sizeof(m_aGitHubLoadFile));
        if(m_GitHubLoadSelect)
        {
                m_GitHubLoadContent.assign((const char *)pData, Len);
                m_GitHubLoadState = GITHUB_LOAD_DONE;
                return;
        }
        Storage()->CreateFolder("Kinetix", IStorage::TYPE_SAVE);
        Storage()->CreateFolder("Kinetix/Scripts", IStorage::TYPE_SAVE);
        char aPath[IO_MAX_PATH_LENGTH];
        str_format(aPath, sizeof(aPath), "Kinetix/Scripts/%s", m_GitHubLoadTarget.c_str());
        char aComplete[IO_MAX_PATH_LENGTH];
        Storage()->GetCompletePath(IStorage::TYPE_SAVE, aPath, aComplete, sizeof(aComplete));
        IOHANDLE File = io_open(aComplete, IOFLAG_WRITE);
        if(!File)
        {
                m_GitHubLoadState = GITHUB_LOAD_ERROR;
                str_copy(m_aGitHubLoadError, "failed to write the script file", sizeof(m_aGitHubLoadError));
                return;
        }
        io_write(File, pData, Len);
        io_close(File);
        m_GitHubLoadState = GITHUB_LOAD_DONE;
}

// =============================================================
// Sandbox — restrict script file access to <userdir>/Kinetix/Sandbox
// =============================================================
const char *CCodeExec::SandboxRoot()
{
        if(m_aSandboxRoot[0] == '\0')
        {
                char aBuf[512];
                Storage()->GetCompletePath(IStorage::TYPE_SAVE, "Kinetix/Sandbox", aBuf, sizeof(aBuf));
                Storage()->CreateFolder("Kinetix", IStorage::TYPE_SAVE);
                Storage()->CreateFolder("Kinetix/Sandbox", IStorage::TYPE_SAVE);
                for(char *p = aBuf; *p; p++)
                        if(*p == '\\')
                                *p = '/';
                str_copy(m_aSandboxRoot, aBuf, sizeof(m_aSandboxRoot));
        }
        return m_aSandboxRoot;
}

bool CCodeExec::SandboxResolve(const char *pPath, char *pOut, int OutSize)
{
        pOut[0] = '\0';
        if(!pPath || !pPath[0])
                return false;
        if(!g_Config.m_KxExecSettings || !g_Config.m_KxExecSandbox)
        {
                str_copy(pOut, pPath, OutSize);
                return true;
        }
        // Any ".." component escapes the sandbox — deny outright.
        if(str_find(pPath, ".."))
                return false;
        char aNorm[512];
        str_copy(aNorm, pPath, sizeof(aNorm));
        for(char *p = aNorm; *p; p++)
                if(*p == '\\')
                        *p = '/';
        const char *pRoot = SandboxRoot();
        const int RootLen = str_length(pRoot);
        if(aNorm[0] == '/' || (aNorm[0] && aNorm[1] == ':'))
        {
                // Absolute path: must be inside the sandbox root.
                if(str_comp_nocase_num(aNorm, pRoot, RootLen) != 0)
                        return false;
                if(aNorm[RootLen] != '\0' && aNorm[RootLen] != '/')
                        return false;
                str_copy(pOut, aNorm, OutSize);
        }
        else
        {
                // Relative path: resolved against the sandbox root.
                const char *pRel = aNorm;
                while(pRel[0] == '.' && pRel[1] == '/')
                        pRel += 2;
                str_format(pOut, OutSize, "%s/%s", pRoot, pRel);
        }
        return true;
}

// =============================================================
// RegisterBindings — vec2 + constants only
// =============================================================
void CCodeExec::RegisterBindings()
{
        if(!m_pLua) return;
        lua_State *L = m_pLua;

        // ===== vec2 with full operator support =====
        luabridge::getGlobalNamespace(L)
                .beginClass<vec2>("vec2")
                        .addConstructor<vec2(), vec2(float, float)>()
                        .addPropertyReadWrite("x", &vec2::x)
                        .addPropertyReadWrite("y", &vec2::y)
                        .addFunction("__tostring", [](vec2 &v) -> std::string {
                                char buf[64]; str_format(buf, sizeof(buf), "(%.2f, %.2f)", v.x, v.y);
                                return std::string(buf);
                        })
                        .addFunction("__add", [](vec2 &a, vec2 &b) -> vec2 { return a + b; })
                        .addFunction("__sub", [](vec2 &a, vec2 &b) -> vec2 { return a - b; })
                        .addFunction("__mul", [](vec2 &v, float s) -> vec2 { return v * s; })
                        .addFunction("__div", [](vec2 &v, float s) -> vec2 { return v / s; })
                        .addFunction("__unm", [](vec2 &v) -> vec2 { return -v; })
                        .addFunction("length", [](vec2 &v) -> float { return length(v); })
                        .addFunction("normalize", [](vec2 &v) -> vec2 { return normalize(v); })
                        .addFunction("dot", [](vec2 &a, vec2 &b) -> float { return dot(a, b); })
                        .addFunction("distance", [](vec2 &a, vec2 &b) -> float { return distance(a, b); })
                        .addFunction("angle", [](vec2 &v) -> float { return angle(v); })
                .endClass();

        luabridge::getGlobalNamespace(L)
                .addFunction("vec2", []() -> vec2 { return vec2(); })
                .addFunction("vec2", [](float x, float y) -> vec2 { return vec2(x, y); });

        // ===== Global constants =====
        // NOTE: cast to int — many of these are anonymous enums, and LuaBridge3
        // pushes an unregistered enum as userdata (ClassNotRegistered), not as a number.
        // Named LuaConst: windows.h (winnt.h) defines `CONST` as a macro.
        luabridge::LuaRef LuaConst = luabridge::newTable(L);
        LuaConst["SERVER_TICK_SPEED"] = (int)SERVER_TICK_SPEED;
        LuaConst["MAX_CLIENTS"] = (int)MAX_CLIENTS;
        LuaConst["NUM_WEAPONS"] = (int)NUM_WEAPONS;
        LuaConst["MAX_DUMMIES"] = (int)MAX_DUMMIES;
        LuaConst["HOOK_RETRACTED"] = (int)HOOK_RETRACTED;
        LuaConst["HOOK_IDLE"] = (int)HOOK_IDLE;
        LuaConst["HOOK_RETRACT_START"] = (int)HOOK_RETRACT_START;
        LuaConst["HOOK_RETRACT_END"] = (int)HOOK_RETRACT_END;
        LuaConst["HOOK_FLYING"] = (int)HOOK_FLYING;
        LuaConst["HOOK_GRABBED"] = (int)HOOK_GRABBED;
        LuaConst["COREEVENT_GROUND_JUMP"] = (int)COREEVENT_GROUND_JUMP;
        LuaConst["COREEVENT_AIR_JUMP"] = (int)COREEVENT_AIR_JUMP;
        LuaConst["COREEVENT_HOOK_LAUNCH"] = (int)COREEVENT_HOOK_LAUNCH;
        LuaConst["COREEVENT_HOOK_ATTACH_PLAYER"] = (int)COREEVENT_HOOK_ATTACH_PLAYER;
        LuaConst["COREEVENT_HOOK_ATTACH_GROUND"] = (int)COREEVENT_HOOK_ATTACH_GROUND;
        LuaConst["COREEVENT_HOOK_HIT_NOHOOK"] = (int)COREEVENT_HOOK_HIT_NOHOOK;
        LuaConst["COREEVENT_HOOK_RETRACT"] = (int)COREEVENT_HOOK_RETRACT;
        LuaConst["TEAM_SPECTATORS"] = (int)TEAM_SPECTATORS;
        LuaConst["TEAM_RED"] = (int)TEAM_RED;
        LuaConst["TEAM_BLUE"] = (int)TEAM_BLUE;
        LuaConst["WEAPON_HAMMER"] = (int)WEAPON_HAMMER;
        LuaConst["WEAPON_GUN"] = (int)WEAPON_GUN;
        LuaConst["WEAPON_SHOTGUN"] = (int)WEAPON_SHOTGUN;
        LuaConst["WEAPON_GRENADE"] = (int)WEAPON_GRENADE;
        LuaConst["WEAPON_LASER"] = (int)WEAPON_LASER;
        LuaConst["WEAPON_NINJA"] = (int)WEAPON_NINJA;
        LuaConst["SPEC_FREEVIEW"] = (int)SPEC_FREEVIEW;
        LuaConst["SPEC_FOLLOW"] = (int)SPEC_FOLLOW;
        LuaConst["FLAG_MISSING"] = (int)FLAG_MISSING;
        LuaConst["FLAG_ATSTAND"] = (int)FLAG_ATSTAND;
        LuaConst["FLAG_TAKEN"] = (int)FLAG_TAKEN;
        LuaConst["SHOW_OTHERS_NOT_SET"] = (int)SHOW_OTHERS_NOT_SET;
        LuaConst["SHOW_OTHERS_OFF"] = (int)SHOW_OTHERS_OFF;
        LuaConst["SHOW_OTHERS_ON"] = (int)SHOW_OTHERS_ON;
        LuaConst["SHOW_OTHERS_ONLY_TEAM"] = (int)SHOW_OTHERS_ONLY_TEAM;
        luabridge::setGlobal(L, LuaConst, "CONST");
}


// =============================================================
// Editor helpers
// =============================================================
int CCodeExec::GetLineCount() const
{
        int Count = 1;
        for(int i = 0; m_aCodeBuffer[i]; i++)
                if(m_aCodeBuffer[i] == '\n') Count++;
        return Count;
}

int CCodeExec::GetLineStart(int Line) const
{
        if(Line <= 0) return 0;
        int CurLine = 0;
        for(int i = 0; m_aCodeBuffer[i]; i++)
        {
                if(m_aCodeBuffer[i] == '\n')
                {
                        CurLine++;
                        if(CurLine == Line) return i + 1;
                }
        }
        return str_length(m_aCodeBuffer);
}

int CCodeExec::GetLineFromOffset(int Offset) const
{
        int Line = 0;
        for(int i = 0; i < Offset && m_aCodeBuffer[i]; i++)
                if(m_aCodeBuffer[i] == '\n') Line++;
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

// =============================================================
// Syntax highlighting
// =============================================================
void CCodeExec::GetHighlightSegments(const char *pLine, SHighlightSegment *pSegments, int &SegmentCount)
{
        SegmentCount = 0;
        int LineLen = str_length(pLine);
        if(LineLen <= 0) return;

        static const char *s_apKeywords[] = {
                "and", "break", "do", "else", "elseif", "end",
                "false", "for", "function", "goto", "if", "in",
                "local", "nil", "not", "or", "repeat", "return",
                "then", "true", "until", "while",
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
                if(m_HighlightParseState == 1)
                {
                        int Start = i;
                        while(i < LineLen)
                        {
                                if(i + 1 < LineLen && pLine[i] == ']' && pLine[i + 1] == ']')
                                { i += 2; m_HighlightParseState = 0; break; }
                                i++;
                        }
                        AddSegment(Start, i - Start, HIGHLIGHT_COMMENT);
                        continue;
                }

                if(i + 1 < LineLen && pLine[i] == '-' && pLine[i + 1] == '-')
                {
                        if(i + 3 < LineLen && pLine[i + 2] == '[' && pLine[i + 3] == '[')
                        {
                                int Start = i; i += 4; m_HighlightParseState = 1;
                                while(i < LineLen)
                                {
                                        if(i + 1 < LineLen && pLine[i] == ']' && pLine[i + 1] == ']')
                                        { i += 2; m_HighlightParseState = 0; break; }
                                        i++;
                                }
                                AddSegment(Start, i - Start, HIGHLIGHT_COMMENT);
                                continue;
                        }
                        AddSegment(i, LineLen - i, HIGHLIGHT_COMMENT);
                        i = LineLen; continue;
                }

                if(pLine[i] == '"' || pLine[i] == '\'')
                {
                        char Quote = pLine[i]; int Start = i; i++;
                        while(i < LineLen && pLine[i] != Quote) { if(pLine[i] == '\\' && i + 1 < LineLen) i++; i++; }
                        if(i < LineLen) i++;
                        AddSegment(Start, i - Start, HIGHLIGHT_STRING); continue;
                }

                if(i + 1 < LineLen && pLine[i] == '[' && pLine[i + 1] == '[')
                {
                        int Start = i; i += 2;
                        while(i < LineLen)
                        {
                                if(i + 1 < LineLen && pLine[i] == ']' && pLine[i + 1] == ']') { i += 2; break; }
                                i++;
                        }
                        AddSegment(Start, i - Start, HIGHLIGHT_STRING); continue;
                }

                if(IsDigit(pLine[i]) || (pLine[i] == '.' && i + 1 < LineLen && IsDigit(pLine[i + 1])))
                {
                        int Start = i;
                        if(pLine[i] == '0' && i + 1 < LineLen && (pLine[i + 1] == 'x' || pLine[i + 1] == 'X'))
                        { i += 2; while(i < LineLen && (IsDigit(pLine[i]) || (pLine[i] >= 'a' && pLine[i] <= 'f') || (pLine[i] >= 'A' && pLine[i] <= 'F'))) i++; }
                        else
                        {
                                while(i < LineLen && IsDigit(pLine[i])) i++;
                                if(i < LineLen && pLine[i] == '.') { i++; while(i < LineLen && IsDigit(pLine[i])) i++; }
                                if(i < LineLen && (pLine[i] == 'e' || pLine[i] == 'E')) { i++; if(i < LineLen && (pLine[i] == '+' || pLine[i] == '-')) i++; while(i < LineLen && IsDigit(pLine[i])) i++; }
                        }
                        AddSegment(Start, i - Start, HIGHLIGHT_NUMBER); continue;
                }

                if(IsAlpha(pLine[i]))
                {
                        int Start = i;
                        while(i < LineLen && IsAlphaNum(pLine[i])) i++;
                        int WordLen = i - Start;
                        bool IsKeyword = false;
                        for(int k = 0; s_apKeywords[k]; k++)
                        {
                                int KWLen = str_length(s_apKeywords[k]);
                                if(WordLen == KWLen && str_comp_num(pLine + Start, s_apKeywords[k], WordLen) == 0) { IsKeyword = true; break; }
                        }
                        AddSegment(Start, WordLen, IsKeyword ? HIGHLIGHT_KEYWORD : HIGHLIGHT_DEFAULT); continue;
                }

                AddSegment(i, 1, HIGHLIGHT_DEFAULT); i++;
        }
}

// =============================================================
// Console commands
// =============================================================
void CCodeExec::ConExec(IConsole::IResult *pResult, void *pUserData)
{
        CCodeExec *pSelf = (CCodeExec *)pUserData;
        pSelf->Execute(pResult->GetString(0));
}

void CCodeExec::ConStop(IConsole::IResult *pResult, void *pUserData)
{
        CCodeExec *pSelf = (CCodeExec *)pUserData;
        pSelf->Stop();
}

void CCodeExec::ConLuaCT(IConsole::IResult *pResult, void *pUserData)
{
        CCodeExec *pSelf = (CCodeExec *)pUserData;
        if(pSelf->m_LuaCTState == LUACT_RUNNING)
                pSelf->LuaCTStop();
        else if(pSelf->m_LuaCTState == LUACT_RESULTS)
                pSelf->LuaCTClear();
        else
                pSelf->LuaCTStart();
}
