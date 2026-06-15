#ifndef GAME_CLIENT_COMPONENTS_CODE_EXEC_H
#define GAME_CLIENT_COMPONENTS_CODE_EXEC_H

#include <game/client/component.h>
#include <engine/console.h>

namespace sol { class state; }

// =========================================================
// CCodeExec — Lua scripting engine via sol2
// Provides Code Execution functionality in ZMajor
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
        char m_aCodeBuffer[8192];

        // Public so LuaAbortHook (static function) can read it
        volatile bool m_AbortRequested;

private:
        EExecState m_State;
        char m_aLastError[1024];

        // Lua state — allocated on heap due to size
        sol::state *m_pLua;

        // Register all bindings (game state, chat, input, etc.)
        void RegisterBindings();

        // Console commands
        static void ConExec(IConsole::IResult *pResult, void *pUserData);
        static void ConStop(IConsole::IResult *pResult, void *pUserData);
};

#endif
