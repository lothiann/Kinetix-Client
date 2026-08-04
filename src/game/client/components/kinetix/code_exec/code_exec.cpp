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
#include <game/client/components/tooltips.h>
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
#include <base/str.h>
#include <engine/console.h>
#include <engine/client/client.h>
#include <base/vmath.h>

#include <lua.hpp>
#include <sol.hpp>

#include <cstdio>
#include <cstring>

static void LuaAbortHook(lua_State *L, lua_Debug *ar)
{
        (void)ar;
        sol::state_view lua(L);
        CCodeExec *pSelf = lua["__code_exec_ptr"].get_or<CCodeExec*>(nullptr);
        if(pSelf && pSelf->m_AbortRequested)
                luaL_error(L, "Script aborted by user");
}

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

void CCodeExec::OnConsoleInit()
{
        Console()->Register("kx_exec", "s[code]", CFGFLAG_CLIENT, ConExec, this, "Execute Lua code");
        Console()->Register("kx_exec_stop", "", CFGFLAG_CLIENT, ConStop, this, "Stop running Lua script");
}

void CCodeExec::OnInit()
{
        m_pLua = new sol::state();
        m_pLua->open_libraries(
                sol::lib::base, sol::lib::string, sol::lib::math,
                sol::lib::table, sol::lib::coroutine, sol::lib::utf8,
                sol::lib::package, sol::lib::os, sol::lib::io, sol::lib::debug
        );

        // print() — outputs to in-game chat
        m_pLua->set_function("print", [this](sol::variadic_args va) {
                std::string result;
                sol::state_view sv(m_pLua->lua_state());
                sol::function tostring_fn = sv["tostring"];
                for(auto it = va.begin(); it != va.end(); ++it)
                {
                        if(it != va.begin()) result += "\t";
                        auto obj = *it;
                        if(tostring_fn.valid())
                        {
                                std::string s = tostring_fn(obj);
                                result += s;
                        }
                        else
                        {
                                sol::optional<std::string> str = obj.get<sol::optional<std::string>>();
                                if(str) result += *str;
                                else result += sol::type_name(m_pLua->lua_state(), obj.get_type());
                        }
                }
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, result.c_str());
        });

        RegisterBindings();
        RegisterRawAPI();
}

void CCodeExec::OnShutdown()
{
        Stop();
        if(m_pLua) { delete m_pLua; m_pLua = nullptr; }
}

void CCodeExec::OnUpdate() {}

void CCodeExec::OnReset() { Stop(); }

const char *CCodeExec::GetLastError() const { return m_aLastError; }

bool CCodeExec::Execute(const char *pCode)
{
        if(!m_pLua || !pCode) return false;
        Stop();
        m_AbortRequested = false;
        m_aLastError[0] = '\0';
        m_State = STATE_RUNNING;
        (*m_pLua)["__code_exec_ptr"] = this;
        lua_sethook(m_pLua->lua_state(), LuaAbortHook, LUA_MASKCOUNT, 1000);

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

        sol::load_result load = m_pLua->load(pSrc);
        if(!load.valid())
        {
                sol::optional<sol::error> maybe_err = load;
                const char *pErrMsg = maybe_err ? maybe_err->what() : "Unknown load error";
                str_copy(m_aLastError, pErrMsg, sizeof(m_aLastError));
                char aBuf[1024]; str_format(aBuf, sizeof(aBuf), "Lua load error: %s", pErrMsg);
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, aBuf);
                m_State = STATE_ERROR;
                lua_sethook(m_pLua->lua_state(), nullptr, 0, 0);
                return false;
        }

        sol::protected_function_result result = load();
        if(!result.valid())
        {
                sol::optional<sol::error> maybe_err = result;
                const char *pErrMsg = maybe_err ? maybe_err->what() : "Unknown execution error";
                str_copy(m_aLastError, pErrMsg, sizeof(m_aLastError));
                char aBuf[1024]; str_format(aBuf, sizeof(aBuf), "Lua error: %s", pErrMsg);
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, aBuf);
                m_State = STATE_ERROR;
                lua_sethook(m_pLua->lua_state(), nullptr, 0, 0);
                return false;
        }

        lua_sethook(m_pLua->lua_state(), nullptr, 0, 0);
        m_State = STATE_IDLE;
        return true;
}

void CCodeExec::Stop()
{
        if(m_State == STATE_RUNNING) m_AbortRequested = true;
        m_State = STATE_IDLE;
}

// =============================================================
// RegisterBindings — vec2 + constants only
// =============================================================
void CCodeExec::RegisterBindings()
{
        if(!m_pLua) return;
        auto &lua = *m_pLua;

        // ===== vec2 with full operator support =====
        lua.new_usertype<vec2>("vec2",
                sol::constructors<vec2(), vec2(float, float)>(),
                "x", &vec2::x,
                "y", &vec2::y,
                sol::meta_function::to_string, [](vec2 &v) -> std::string {
                        char buf[64]; str_format(buf, sizeof(buf), "(%.2f, %.2f)", v.x, v.y);
                        return std::string(buf);
                },
                sol::meta_function::addition, [](vec2 &a, vec2 &b) -> vec2 { return a + b; },
                sol::meta_function::subtraction, [](vec2 &a, vec2 &b) -> vec2 { return a - b; },
                sol::meta_function::multiplication, sol::overload(
                        [](vec2 &v, float s) -> vec2 { return v * s; },
                        [](float s, vec2 &v) -> vec2 { return v * s; }
                ),
                sol::meta_function::division, [](vec2 &v, float s) -> vec2 { return v / s; },
                sol::meta_function::unary_minus, [](vec2 &v) -> vec2 { return -v; },
                "length", [](vec2 &v) -> float { return length(v); },
                "normalize", [](vec2 &v) -> vec2 { return normalize(v); },
                "dot", [](vec2 &a, vec2 &b) -> float { return dot(a, b); },
                "distance", [](vec2 &a, vec2 &b) -> float { return distance(a, b); },
                "angle", [](vec2 &v) -> float { return angle(v); }
        );

        // ===== Global constants =====
        auto CONST = lua["CONST"].get_or_create<sol::table>();
        CONST["SERVER_TICK_SPEED"] = SERVER_TICK_SPEED;
        CONST["MAX_CLIENTS"] = MAX_CLIENTS;
        CONST["NUM_WEAPONS"] = NUM_WEAPONS;
        CONST["MAX_DUMMIES"] = MAX_DUMMIES;
        CONST["HOOK_RETRACTED"] = HOOK_RETRACTED;
        CONST["HOOK_IDLE"] = HOOK_IDLE;
        CONST["HOOK_RETRACT_START"] = HOOK_RETRACT_START;
        CONST["HOOK_RETRACT_END"] = HOOK_RETRACT_END;
        CONST["HOOK_FLYING"] = HOOK_FLYING;
        CONST["HOOK_GRABBED"] = HOOK_GRABBED;
        CONST["COREEVENT_GROUND_JUMP"] = COREEVENT_GROUND_JUMP;
        CONST["COREEVENT_AIR_JUMP"] = COREEVENT_AIR_JUMP;
        CONST["COREEVENT_HOOK_LAUNCH"] = COREEVENT_HOOK_LAUNCH;
        CONST["COREEVENT_HOOK_ATTACH_PLAYER"] = COREEVENT_HOOK_ATTACH_PLAYER;
        CONST["COREEVENT_HOOK_ATTACH_GROUND"] = COREEVENT_HOOK_ATTACH_GROUND;
        CONST["COREEVENT_HOOK_HIT_NOHOOK"] = COREEVENT_HOOK_HIT_NOHOOK;
        CONST["COREEVENT_HOOK_RETRACT"] = COREEVENT_HOOK_RETRACT;
        CONST["TEAM_SPECTATORS"] = TEAM_SPECTATORS;
        CONST["TEAM_RED"] = TEAM_RED;
        CONST["TEAM_BLUE"] = TEAM_BLUE;
        CONST["WEAPON_HAMMER"] = WEAPON_HAMMER;
        CONST["WEAPON_GUN"] = WEAPON_GUN;
        CONST["WEAPON_SHOTGUN"] = WEAPON_SHOTGUN;
        CONST["WEAPON_GRENADE"] = WEAPON_GRENADE;
        CONST["WEAPON_LASER"] = WEAPON_LASER;
        CONST["WEAPON_NINJA"] = WEAPON_NINJA;
        CONST["SPEC_FREEVIEW"] = SPEC_FREEVIEW;
        CONST["SPEC_FOLLOW"] = SPEC_FOLLOW;
        CONST["FLAG_MISSING"] = FLAG_MISSING;
        CONST["FLAG_ATSTAND"] = FLAG_ATSTAND;
        CONST["FLAG_TAKEN"] = FLAG_TAKEN;
        CONST["SHOW_OTHERS_NOT_SET"] = SHOW_OTHERS_NOT_SET;
        CONST["SHOW_OTHERS_OFF"] = SHOW_OTHERS_OFF;
        CONST["SHOW_OTHERS_ON"] = SHOW_OTHERS_ON;
        CONST["SHOW_OTHERS_ONLY_TEAM"] = SHOW_OTHERS_ONLY_TEAM;
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
