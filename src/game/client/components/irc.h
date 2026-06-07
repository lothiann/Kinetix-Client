#ifndef GAME_CLIENT_COMPONENTS_IRC_H
#define GAME_CLIENT_COMPONENTS_IRC_H

#include <game/client/component.h>
#include <engine/shared/protocol.h>
#include <engine/console.h>
#include <vector>
#include <string>

class CIRC : public CComponent
{
public:
	CIRC();

	virtual int Sizeof() const override { return sizeof(*this); }
	virtual void OnInit() override;
	virtual void OnReset() override;
	virtual void OnConsoleInit() override;
	virtual void OnRender() override;
	virtual void OnMessage(int MsgId, void *pRawMsg) override;
	virtual void OnStateChange(int NewState, int OldState) override;

	const char *GetClientPrefix(int ClientId) const;
	void SendIRC(const char *pText);

	std::vector<int> EncodeStringToEmotes(const std::string& text);
	std::string DecodeEmotesToString(const std::vector<int>& emotes);
	int GetUtf8CharIndex(const std::string &table, const std::string &utf8Char);

private:
	static void ConSendIRC(IConsole::IResult *pResult, void *pUserData);
	void QueueHandshake();

	std::vector<int> m_vSendQueue;
	int m_NextEmoteSendTick;
	bool m_LastRevealJoin;

	std::vector<int> m_aEmoteHistory[MAX_CLIENTS];
	char m_aDetectedClient[MAX_CLIENTS][16];

	std::vector<int> m_aMsgBuffer[MAX_CLIENTS];
	int64_t m_aLastEmoteTime[MAX_CLIENTS];
};

#endif
