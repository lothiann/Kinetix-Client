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

#include <chrono>
#include <ctime>
#include <memory>
#include <string>
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
	bool m_WaitToolExec = false;
	// Raw API transcript (one JSON object string per completed turn).
	std::vector<std::string> m_vApiMessages;
	std::vector<std::string> m_vModels;
	char m_aError[512];
	bool m_BusyMeta; // tokens/worksec valid for the streaming message
	std::chrono::steady_clock::time_point m_RequestStart;
};

// The Agent chat state is rendered/interacted with by CClickGui
// (RenderArrayAgent + input handling in clickgui.cpp) reading CAgent.
