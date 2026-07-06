#ifndef GAME_CLIENT_COMPONENTS_CODE_EXEC_H
#define GAME_CLIENT_COMPONENTS_CODE_EXEC_H

#include <game/client/component.h>
#include <engine/console.h>

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
        void OnUpdate() override;
        void OnReset() override;

        // Execute a Lua script string
        bool Execute(const char *pCode);

        // Stop the currently running script (uses lua_sethook to interrupt)
        void Stop();

        // Get current execution state
        EExecState GetState() const { return m_State; }

        // Get last error message
        const char *GetLastError() const;

        // The Lua source code buffer (edited in UI)
        char m_aCodeBuffer[65536];

        // Public so LuaAbortHook (static function) can read it
        volatile bool m_AbortRequested;

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

        EExecState m_State;
        char m_aLastError[1024];

        // Register all bindings (game state, chat, input, etc.)
        void RegisterBindings();

        // Register raw C++ class access (direct pointers, usertypes)
        void RegisterRawAPI();

        // Console commands
        static void ConExec(IConsole::IResult *pResult, void *pUserData);
        static void ConStop(IConsole::IResult *pResult, void *pUserData);
};

#endif
