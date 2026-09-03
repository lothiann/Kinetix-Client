#ifndef GAME_CLIENT_COMPONENTS_CODE_EXEC_H
#define GAME_CLIENT_COMPONENTS_CODE_EXEC_H

#include <game/client/component.h>
#include <engine/console.h>

#include <atomic>
#include <string>

struct lua_Debug;
struct lua_State;

namespace sol { class state; }

// =========================================================
// CCodeExec — Lua scripting engine via sol2
// Provides Code Execution functionality in Kinetix
// =========================================================
class CCodeExec : public CComponent
{
public:
        // Script execution state
        enum EExecState
        {
                STATE_IDLE = 0,    // No script running
                STATE_RUNNING,     // Script is executing
                STATE_ERROR,       // Script ended with error
        };

        // Syntax highlighting segment types
        enum EHighlightType
        {
                HIGHLIGHT_DEFAULT = 0,
                HIGHLIGHT_KEYWORD,
                HIGHLIGHT_STRING,
                HIGHLIGHT_COMMENT,
                HIGHLIGHT_NUMBER,
        };

        // A colored segment of a line for syntax highlighting
        struct SHighlightSegment
        {
                int m_Start;   // byte offset within the line text
                int m_Length;
                EHighlightType m_Type;
        };

        // Maximum highlight segments per line
        static const int MAX_HIGHLIGHT_SEGMENTS = 128;

        CCodeExec();
        ~CCodeExec();

        int Sizeof() const override { return sizeof(*this); }
        void OnConsoleInit() override;
        void OnInit() override;
        void OnShutdown() override;
        void OnRender() override;
        void OnReset() override;

	// Execute a Lua script string
	bool Execute(const char *pCode, const char *pChunkName = "script");

	// Stop the currently running script (abandons the coroutine)
	void Stop();

        // Get current execution state
        EExecState GetState() const { return (EExecState)m_State.load(); }

        // Get last error message
        const char *GetLastError() const;

	// The Lua source code buffer (edited in UI)
	char m_aCodeBuffer[65536];

	// --- Multiline editor state ---
        bool m_EditorActive;           // true when the code editor has focus
        bool m_MouseSelecting;         // true while mouse drag selects text
        int m_CursorPos;               // byte offset of cursor in m_aCodeBuffer
        int m_SelectionStart;          // byte offset of selection start
        int m_SelectionEnd;            // byte offset of selection end
        float m_ScrollY;               // vertical scroll offset in pixels
        float m_ScrollYChange;         // smooth scroll velocity

        // --- Syntax highlighting state ---
        int m_HighlightParseState;     // 0=normal, 1=block comment --[[ ... ]]

        // Editor helpers
        int GetLineCount() const;
        int GetLineStart(int Line) const;
        int GetLineFromOffset(int Offset) const;
        void EnsureCursorVisible(float LineHeight, float ViewHeight);

        // Syntax highlighting: get colored segments for one line
        // Updates m_HighlightParseState across calls for multi-line constructs
        void GetHighlightSegments(const char *pLine, SHighlightSegment *pSegments, int &SegmentCount);

        // Get the length of a logical line (excluding trailing \n)
        int GetLineLength(int Line) const;

private:
	// Lua state — allocated on heap due to size
	sol::state *m_pLua;

	// Registry reference to the running coroutine thread (-1 = none)
	int m_CoroutineRef;

	std::atomic<int> m_State;
	std::atomic<bool> m_StopRequested;
	bool m_WasRunning;
        int m_InstructionCounter;
        lua_State *m_pCoroutine;
        bool m_InResume;
        char m_aLastError[1024];

        // Register all bindings (game state, chat, input, etc.)
        void RegisterBindings();

        // Register raw C++ class access (direct pointers, usertypes)
        void RegisterRawAPI();

        static const int HOOK_INSTRUCTION_INTERVAL = 1000;
        static const int RESUME_INSTRUCTION_BUDGET = 200000;

        // Instruction-count hook: aborts the script when stop was requested
        static void LuaStepHook(lua_State *L, lua_Debug *ar);

        // Console commands
        static void ConExec(IConsole::IResult *pResult, void *pUserData);
        static void ConStop(IConsole::IResult *pResult, void *pUserData);
};

#endif
