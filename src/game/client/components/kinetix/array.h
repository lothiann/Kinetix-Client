// (c) Kinetix. Array* HUD translation unit (array.cpp): the ArrayList /
// ArrayInfo / ArrayPlayer renderers (moved out of clickgui.cpp) and the
// ArrayAgent AI-chat HUD with its CAgent component.
//
// CAgent owns the chat state machine: an OpenAI-compatible HTTP endpoint
// (streamed via IHttpRequest::StreamTake SSE parsing), two tools
// (code_execution — runs Lua through CCodeExec, documentation — binding
// reference sections) and the in-memory conversation.
#pragma once

#include <game/client/component.h>
#include <engine/http.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CAgent : public CComponent
{
public:
	enum
	{
		// EAgentRole
		ROLE_USER = 0,
		ROLE_ASSISTANT,
		ROLE_TOOL,
		ROLE_ERROR,
	};

	struct SAgentMessage
	{
		int m_Role = ROLE_USER;
		std::string m_Text;
		std::string m_Reasoning; // reasoning model stream (delta.reasoning_content)
		int m_Tokens = 0;    // token usage (assistant meta line)
		int m_WorkSec = 0;   // generation wall time (assistant meta line)
		bool m_Streaming = false; // assistant text still arriving
		time_t m_Time = 0;
		// Tool-call display ([ Code Execution > ] block).
		std::string m_ToolCallId; // links the display line to its result
		std::string m_ToolName;   // raw tool name (code_execution / documentation)
		std::string m_ToolArgs;   // raw JSON arguments
		std::string m_ToolResult; // result text ("" while running)
		bool m_Expanded = false;  // tool block open
		bool m_ReasonOpen = false; // reasoning block open (Reasoning = Show)
		// Animated expand progress (0..1, eased in the renderer each frame).
		float m_ExpandAnim = 0.0f;
		float m_ReasonAnim = 0.0f;
	};

	CAgent() :
		m_State(AGENT_IDLE), m_RoundTrips(0), m_EffortIdx(0), m_ModelIdx(0), m_BusyMeta(false)
	{
		mem_zero(m_aError, sizeof(m_aError));
	}

	void OnInit() override;
	void OnRender() override;
	void OnReset() override;
	int Sizeof() const override { return sizeof(*this); }

	enum EAgentState
	{
		AGENT_IDLE = 0,
		AGENT_REQUEST,  // HTTP request in flight (streaming)
		AGENT_TOOL_RUN, // a tool Lua script is executing
	};

	bool IsBusy() const { return m_State != AGENT_IDLE; }
	const char *GetError() const { return m_aError; }

	// Chat API (called by the ClickGUI HUD).
	void Send(const char *pText);
	void StopGeneration();
	void ClearChat() { m_vMessages.clear(); }
	// Model list parsed from kx_arrayagent_models (comma separated).
	const std::vector<std::string> &Models() { return m_vModels; }
	void RefreshModels();
	// v6.14: read-only path helpers for the HUD renderer. The tool widgets
	// show resolved paths and read files to build the edit diff view.
	std::string ToolFilesRoot() const { return AgentFilesRoot(); }
	std::string ToolResolvePath(const std::string &Path) const { return AgentResolvePath(Path); }

	// v6.14: sessions. One JSON file per session in Kinetix/Sessions.
	struct SSessionInfo
	{
		std::string m_Id;
		std::string m_Title;
		time_t m_Time = 0;
		bool m_Temp = false;
	};
	bool SessionListVisible() const { return m_SessionListVisible; }
	const std::vector<SSessionInfo> &Sessions() const { return m_vSessions; }
	void RefreshSessions();
	void NewSession();
	void OpenSession(const std::string &Id);
	void ExitToSessionList();
	std::string CurrentSessionTitle() const { return m_CurrentSessionTitle; }
	bool CurrentSessionIsTemp() const { return m_CurrentSessionTemp; }
	time_t CurrentSessionTime() const { return m_CurrentSessionTime; }
	bool HasMessages() const { return !m_vMessages.empty(); }
	int m_ModelIdx;
	int m_EffortIdx; // 0=Default, 1=Low, 2=High, 3=Max

	std::vector<SAgentMessage> m_vMessages;
	// The chat input text (edited by the ClickGUI HUD input field).
	char m_aInput[1024] = "";
	// TODO(v6.14): persist the conversation to Kinetix/Agent/chat.json and
	// restore it on start — the history is currently in-memory only.

private:
	void StartRequest();
	void ConsumeStream();
	void ProcessChunk(const char *pJson);
	void FinishRequest(EHttpState State);
	void RunTool();
	void AppendToolResult(const std::string &CallId, const std::string &Name, const std::string &Result);
	static std::string DocsLookup(const std::string &Query);

	int m_State; // EAgentState
	int m_RoundTrips; // tool-loop iteration guard
	std::shared_ptr<IHttpRequest> m_pRequest;
	std::string m_SseBuf;      // SSE bytes awaiting a complete line
	bool m_SseDone = false;    // [DONE] seen
	bool m_AbortRequested = false; // error chunk seen
	std::string m_PendingText; // streamed assistant content of the current turn
	std::string m_PendingReasoning; // streamed reasoning_content of the current turn
	std::string m_FinishReason;
	int m_UsageTokens = -1;
	// OpenAI tool_calls accumulated from the stream deltas.
	struct SToolCall
	{
		std::string m_Id;
		std::string m_Name;
		std::string m_Args;
	};
		std::vector<SToolCall> m_vToolCalls;
		std::vector<SToolCall> m_vPendingToolCalls;
		SToolCall m_ActiveTool;

		// --- v6.14: external tools (shell / files / console / jobs) ---
		// What the current tool call waits for. Foreground Lua runs over frames
		// inside CCodeExec; the shell runs on a worker thread.
		enum
		{
			TOOLRUN_NONE = 0,
			TOOLRUN_LUA,
			TOOLRUN_SHELL,
		};
		int m_ToolRun = TOOLRUN_NONE;

		// One shell command running on a worker thread (Win32 CreateProcess /
		// popen elsewhere). The thread owns the process and collects output.
		struct SShellJob
		{
			std::thread m_Thread;
			std::thread m_OutThread;
			std::thread m_ErrThread;
			std::mutex m_Mutex;
			std::string m_Out;
			std::string m_Err;
			int m_ExitCode = -1;
			bool m_TimedOut = false;
			std::atomic<bool> m_Done{false};
			std::atomic<bool> m_Kill{false};
			void *m_pProcess = nullptr; // Win32 HANDLE
		};
		std::shared_ptr<SShellJob> m_pShellJob;

		// Background Lua jobs (code_execution { background: true }).
		struct SBgJob
		{
			int m_Id = 0;
			bool m_Finished = false;
			bool m_Killed = false;
			std::string m_Output;
		};
		std::vector<SBgJob> m_vBgJobs;
		int m_ActiveBgJob = -1; // job id currently running in CodeExec, -1 = none
		int m_NextBgJobId = 1;

		// --- v6.14: session persistence ---
		std::string SessionsDir() const;
		void SaveCurrentSession();
		bool LoadSessionFile(const std::string &Id);
		std::string BuildSessionJson() const;
		std::string SessionTitle() const;
		bool m_SessionListVisible = true;
		std::string m_CurrentSessionId;
		std::string m_CurrentSessionTitle;
		bool m_CurrentSessionTemp = true;
		time_t m_CurrentSessionTime = 0;
		std::vector<SSessionInfo> m_vSessions;

		void RunShellTool(const SToolCall &Call);
		void RunFilesTool(const SToolCall &Call);
		void RunConsoleTool(const SToolCall &Call);
		void RunJobOutputTool(const SToolCall &Call);
		void RunJobKillTool(const SToolCall &Call);
		void PollShellJob();
		void PollBgJob();
		std::string BgJobResult(int Id, bool Kill);
		std::string AgentFilesRoot() const;
		std::string AgentResolvePath(const std::string &Path) const;
		static void ShellThreadProc(std::shared_ptr<SShellJob> pJob, std::string Shell, std::string Command, std::string Cwd, int TimeoutSec);

		// Raw API transcript (one JSON object string per completed turn).
	std::vector<std::string> m_vApiMessages;
	std::vector<std::string> m_vModels;
	char m_aError[512];
	bool m_BusyMeta; // tokens/worksec valid for the streaming message
	std::chrono::steady_clock::time_point m_RequestStart;
};

// The Agent chat state is rendered/interacted with by CClickGui
// (RenderArrayAgent + input handling in clickgui.cpp) reading CAgent.
