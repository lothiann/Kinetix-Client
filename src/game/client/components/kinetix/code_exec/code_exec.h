#ifndef GAME_CLIENT_COMPONENTS_CODE_EXEC_H
#define GAME_CLIENT_COMPONENTS_CODE_EXEC_H

#include <game/client/component.h>
#include <engine/console.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct lua_Debug;
struct lua_State;

// Version of the Code Execution binding API. Scripts declare which version
// they were written for with a "-- CEV: <n>" metadata header line; the client
// warns when a script targets a newer version. v1 = sol2 bindings era,
// v2 = LuaBridge3 bindings era (current).
#define KX_CE_VERSION 2

// =========================================================
// CCodeExec — Lua scripting engine via LuaBridge3
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

	// --- v6.13: ArrayAgent tool capture ---
	// While active, print() output and Lua errors are ALSO appended to an
	// internal list (in addition to the usual chat line) so the agent can
	// return them as the tool result.
	void ToolCaptureStart();
	// Ends capture and returns the collected output lines.
	std::vector<std::string> ToolCaptureFinish();
	// Returns a copy of the output collected so far; the capture keeps running
	// (used by the agent's background-job polling).
	std::vector<std::string> ToolCapturePeek() const;
	// True while a tool capture is active (the agent loop must not run two
	// scripts at once).
	bool ToolCaptureActive() const { return m_ToolCapture; }

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

        // --- LuaCT (Lua Compatibility Test) ---
        struct SLuaCTResult
        {
                std::string m_Name;
                bool m_Ok;
                std::string m_Error;
                // SoFE: wall-clock seconds the test took (pcall + binding
                // access). m_Timed is false when the coroutine was suspended
                // by the instruction-budget yield mid-test — the wall time
                // then includes the suspended gap and is excluded from the
                // min/avg/max statistics.
                double m_Time = 0.0;
                bool m_Timed = false;
        };
        enum ELuaCTState
        {
                LUACT_IDLE = 0,    // no results yet
                LUACT_RUNNING,     // test is running
                LUACT_RESULTS,     // finished, results available
        };
        int LuaCTState() const { return m_LuaCTState; }
        const std::vector<SLuaCTResult> &LuaCTResults() const { return m_vLuaCTResults; }
        void LuaCTStart();
        void LuaCTStop();
        void LuaCTClear();

        // --- GitHub script repository (storage/scripts in the client repo) ---
        struct SGitHubScript
        {
                std::string m_File;         // file name inside storage/scripts
                std::string m_Name;
                std::string m_Version;
                int m_CeVersion = 0;        // -- CEV: n header value
                std::string m_Author;
                std::string m_Description;
        };
        enum EGitHubState
        {
                GITHUB_IDLE = 0,   // never updated (or cleared)
                GITHUB_UPDATING,   // scripts.json download in flight
                GITHUB_READY,      // list parsed, scripts available
                GITHUB_ERROR,      // download or parse failed
        };
        enum EGitHubLoadState
        {
                GITHUB_LOAD_IDLE = 0,
                GITHUB_LOAD_LOADING, // script download in flight
                GITHUB_LOAD_DONE,    // file written to Kinetix/Scripts/<file>
                GITHUB_LOAD_ERROR,
        };
        EGitHubState GitHubState() const { return (EGitHubState)m_GitHubState; }
        const char *GitHubError() const { return m_aGitHubError; }
        const std::string &GitHubListUpdated() const { return m_GitHubListUpdated; }
        const std::string &GitHubLastSupportedVersion() const { return m_GitHubLastSupportedVersion; }
        const std::vector<SGitHubScript> &GitHubScripts() const { return m_vGitHubScripts; }
        // Start downloading storage/scripts/scripts.json from the repo.
        void GitHubUpdateList();
        // Start downloading scripts[Idx]. Select=true: the script stays in
        // memory (GitHubLoadContent) and is loaded into the execution buffer;
        // Select=false (Download): the file is written to Kinetix/Scripts.
        void GitHubLoadScript(int Idx, bool Select);
        EGitHubLoadState GitHubLoadState() const { return (EGitHubLoadState)m_GitHubLoadState; }
        const char *GitHubLoadFile() const { return m_aGitHubLoadFile; }
        bool GitHubLoadSelect() const { return m_GitHubLoadSelect; }
        // In-memory script content of a finished Load (valid until
        // GitHubClearLoadDone).
        const std::string &GitHubLoadContent() const { return m_GitHubLoadContent; }
        const char *GitHubLoadError() const { return m_aGitHubLoadError; }
        // Consume the "done" state after the caller picked up the file.
        void GitHubClearLoadDone();
        // Poll running HTTP jobs and finish finished ones. Called every frame.
        void GitHubUpdateJobs();

        // --- Sandbox ---
        // Absolute path of the sandbox root ("<userdir>/Kinetix/Sandbox",
        // created on demand) with forward slashes.
        const char *SandboxRoot();
        // Resolves pPath against the sandbox root when the sandbox is enabled:
        // relative paths are joined onto the root; absolute paths must be inside
        // it. Returns false (and leaves pOut empty) when the path is denied.
        // When the sandbox is disabled the path is passed through unchanged.
        bool SandboxResolve(const char *pPath, char *pOut, int OutSize);

        // --- Shell detection (for OS commands) ---
        enum
        {
                SHELL_POWERSHELL = 0,
                SHELL_CMD,
                SHELL_GITBASH,
                SHELL_MSYS,
                NUM_SHELLS,
        };
        static void DetectShells();
        static bool ShellAvailable(int Id) { return Id >= 0 && Id < NUM_SHELLS && s_aShellFound[Id]; }
        static const char *ShellPath(int Id) { return (Id >= 0 && Id < NUM_SHELLS) ? s_aaShellPaths[Id] : ""; }

private:
	// Lua state — allocated on heap due to size
	lua_State *m_pLua;

	// Registry reference to the running coroutine thread (-1 = none)
	int m_CoroutineRef;

	std::atomic<int> m_State;
	std::atomic<bool> m_StopRequested;
	bool m_WasRunning;
        int m_InstructionCounter;
        lua_State *m_pCoroutine;
        bool m_InResume;
        char m_aLastError[1024];

        // v6.13: ArrayAgent tool capture (see ToolCaptureStart/Finish).
        bool m_ToolCapture = false;
        std::vector<std::string> m_vToolOutput;

        // LuaCT state
        int m_LuaCTState = LUACT_IDLE;
        bool m_LuaCTWasRunning = false;
        std::vector<SLuaCTResult> m_vLuaCTResults;
        // Set by LuaStepHook when the coroutine yields on the instruction
        // budget; the next LuaCTRecord excludes its wall time from SoFE.
        bool m_YieldedSinceMark = false;

        // GitHub script repository state
        int m_GitHubState = GITHUB_IDLE;
        int m_GitHubLoadState = GITHUB_LOAD_IDLE;
        bool m_GitHubLoadSelect = false;
        char m_aGitHubError[256] = "";
        char m_aGitHubLoadError[256] = "";
        char m_aGitHubLoadFile[128] = "";
        std::string m_GitHubLoadContent; // Load: script kept in memory only
        std::string m_GitHubListUpdated;
        std::string m_GitHubLastSupportedVersion;
        std::string m_GitHubLoadTarget;
        std::vector<SGitHubScript> m_vGitHubScripts;
        std::shared_ptr<class IHttpRequest> m_pGitHubListJob;
        std::shared_ptr<class IHttpRequest> m_pGitHubLoadJob;
        // Finish a completed job: parse scripts.json / write the script file.
        void GitHubFinishList(class IHttpRequest *pReq);
        void GitHubFinishLoad(class IHttpRequest *pReq);

        // Sandbox root cache ("" until first use)
        char m_aSandboxRoot[512] = "";

        // Detected shells (filled by DetectShells at startup)
        static char s_aaShellPaths[NUM_SHELLS][300];
        static bool s_aShellFound[NUM_SHELLS];

        // Register all bindings (game state, chat, input, etc.)
        void RegisterBindings();

        // Register raw C++ class access (direct pointers, usertypes)
        void RegisterRawAPI();

        static const int HOOK_INSTRUCTION_INTERVAL = 1000;
        static const int RESUME_INSTRUCTION_BUDGET = 200000;

	// Instruction-count hook: aborts the script when stop was requested.
	// With kx_exec_debug the hook also logs function calls/returns.
	static void LuaStepHook(lua_State *L, lua_Debug *ar);

	// print() — variadic Lua closure forwarding to the in-game chat
	static int LuaPrint(lua_State *L);

	// LuaCT: records one test result (name, ok, [err]) from the test chunk
	static int LuaCTRecord(lua_State *L);

        // Console commands
        static void ConExec(IConsole::IResult *pResult, void *pUserData);
        static void ConStop(IConsole::IResult *pResult, void *pUserData);
        static void ConLuaCT(IConsole::IResult *pResult, void *pUserData);
};

#endif
