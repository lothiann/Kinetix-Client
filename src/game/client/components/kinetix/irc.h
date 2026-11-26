#ifndef GAME_CLIENT_COMPONENTS_IRC_H
#define GAME_CLIENT_COMPONENTS_IRC_H

#include <game/client/component.h>
#include <engine/shared/protocol.h>
#include <engine/console.h>
#include <engine/client/enums.h>
#include <engine/graphics.h>
#include <vector>
#include <string>

// v6.19: IRC v2 identity watermark helpers. A Kinetix user is recognised by a
// pair of zero-width characters embedded in their nick/clan at a position
// derived from the visible text with a private salt. Ordinary players never
// carry zero-width characters, so false positives are practically impossible.
namespace KxIrcStego
{
	// Extracts the visible text (drops the zero-width payload) of pText.
	void VisibleText(const char *pText, char *pOut, int OutSize);
	// True when pText carries a valid Kinetix identity watermark. Also strips the
	// watermark into pVisible (when not null).
	bool HasIdentity(const char *pText, bool Clan, char *pVisible = nullptr, int VisibleSize = 0);
	// Builds the watermarked text. Returns false when it does not fit MaxBytes.
	bool BuildIdentity(const char *pText, bool Clan, int MaxBytes, char *pOut, int OutSize);
}

// v6.21: one decoded IRC v2 frame. A long message arrives as several text
// frames (fragments) carrying 4-bit text nibbles of the compressed stream.
struct SKxIrcFrame
{
	bool m_Identity;
	bool m_Frag;
	int m_Index;
	int m_Total;
	std::vector<int> m_vNibbles;
};

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

	// v6.19: outgoing StartInfo/ChangeInfo name/clan with the identity watermark
	// applied. Returns pBuf, or pVisible when v2 is off / nothing fits.
	const char *EffectiveName(int Dummy, char *pBuf, int BufSize) const;
	const char *EffectiveClan(int Dummy, char *pBuf, int BufSize) const;

	// v6.19: called by CChat before a user line is sent. May append a hidden
	// zero-width payload (identity and/or a queued IRC message) and return pOut.
	const char *EncodeChatLine(const char *pLine, char *pOut, int OutSize, int Dummy);

	// v6.19: true when the client is a detected Kinetix user (any channel).
	bool IsKinetixClient(int ClientId) const;

	// v6.19: Kinetix server-browser indicator icon.
	IGraphics::CTextureHandle LogoTexture() const { return m_LogoTexture; }

	// v6.20: draws the Kinetix icon centered at (CenterX, CenterY). Used by the
	// server browser, the scoreboard, the chat lines and the nameplates so the
	// indicator looks identical everywhere.
	void RenderLogo(float CenterX, float CenterY, float Size, float Alpha = 1.0f) const;

	// v6.20: extracts the IRC messages embedded in a chat line as a gray suffix
	// (" (msg1) (msg2)"). Returns pOut (empty when the line carries no IRC
	// frame). The visible text of the line itself is never returned — it was
	// already delivered as a normal chat message.
	const char *DecodeChatSuffix(const char *pMessage, char *pOut, int OutSize);
	bool V2ProcessChat2(int ClientId, int Team, char *pMessage, int MessageSize, char *pSuffix, int SuffixSize);

	// v6.20: true when this dummy's tee is within 50 tiles of a finish tile.
	// While that is the case the identity watermark is dropped so the finish is
	// recorded with the player's real nickname.
	bool NearFinish(int Dummy) const;

	// v6.19: aim channel hook — called by CControls right before the input is
	// sent. Overrides the target direction while an IRC v2 payload is pending.
	void ApplyAimOverride(int Dummy, int *pTargetX, int *pTargetY);

	// v6.20: true while a whole aim byte is still queued. CControls forces an
	// input send every tick in that case so the receiver can sample the stream.
	bool HasPendingAim(int Dummy) const;

	std::vector<int> EncodeStringToEmotes(const std::string& text, const std::string *pDictionary = nullptr);
	std::string DecodeEmotesToString(const std::vector<int>& emotes, const std::string *pDictionary = nullptr);
	int GetUtf8CharIndex(const std::string &table, const std::string &utf8Char);
	std::string GetUtf8CharFromTable(const std::string& table, size_t index);

private:
	static void ConSendIRC(IConsole::IResult *pResult, void *pUserData);
	void QueueHandshake(int Dummy);
	void SendEmoteOnDummy(int Dummy, int Emoticon);
	bool IsOwnClient(int ClientId) const;

	// v6.19: where the identity watermark goes for this dummy. Priority:
	// clan -> nick -> chat (the chat payload is the last resort when neither
	// the 9-byte clan nor the 13-byte nick budget can hold a marker).
	enum class EIdentityTarget
	{
		NONE,
		CLAN,
		NAME,
		CHAT,
	};
	EIdentityTarget IdentityTarget(int Dummy) const;

	// ---- v1 (legacy) ----
	std::vector<int> m_aSendQueue[MAX_DUMMIES];
	int m_aNextEmoteSendTick[MAX_DUMMIES];
	bool m_aHandshakeSent[MAX_DUMMIES];
	bool m_LastRevealJoin;

	std::vector<int> m_aEmoteHistory[MAX_CLIENTS];
	char m_aDetectedClient[MAX_CLIENTS][16];

	std::vector<int> m_aMsgBuffer[MAX_CLIENTS];
	int64_t m_aLastEmoteTime[MAX_CLIENTS];

	// ---- v2 ----
	void UpdateV2();
	void OnV2Chat(int ClientId, const char *pMessage);
	void OnV2Emote(int ClientId, int Emoticon);
	void V2QueueBits(int Dummy, int Channel, const std::vector<unsigned char> &vBits);
	void V2QueueMessage(int Dummy, const char *pText);
	void V2QueueIdentity(int Dummy);
	void V2PumpChannel(int Dummy, int Channel);
	void V2SendChatPayload(int Dummy, const std::vector<unsigned char> &vBits);
	void V2WhisperPayload(int Dummy, const std::vector<unsigned char> &vBits);
	void V2TryDecode(int ClientId, int Channel);
	void V2Mark(int ClientId);
	void V2ScanIdentities();
	void BuildFinishTiles();

	struct SV2Chat2Outbound
	{
		std::string m_Text;
		std::vector<unsigned char> m_vData;
		int m_Codec = -1;
		int m_Offset = 0;
		unsigned m_MessageCrc = 0;
	};
	struct SV2Chat2Assembly
	{
		bool m_Active = false;
		int m_Team = 0;
		int m_Codec = -1;
		int m_Total = 0;
		int m_Next = 0;
		int m_Part = 0;
		unsigned m_MessageCrc = 0;
		int64_t m_LastTime = 0;
		std::vector<unsigned char> m_vData;
	};

	// v6.21: a message longer than one chat line / whisper is split into
	// fragments and reassembled here. EchoSingle controls whether a complete
	// single-fragment message is echoed: the chat channel renders it as a gray
	// suffix on the original line instead, so it must not be echoed twice.
	struct SV2Assembly
	{
		bool m_Active;
		int m_Total;
		int m_Next;
		int64_t m_LastTime;
		std::vector<int> m_vNibbles;
	};
	void V2DeliverFrame(int ClientId, const SKxIrcFrame &Frame, bool EchoSingle);
	void V2EmitText(int ClientId, const std::vector<int> &vNibbles);

	IGraphics::CTextureHandle m_LogoTexture;
	// v6.20: cached finish tile centers (rebuilt on join) for the "finish with
	// the real nickname" rule.
	std::vector<vec2> m_vFinishTiles;
	bool m_FinishTilesBuilt;
	// Pending bit queues per dummy (3 channels: 0 zero-width/chat, 1 emote, 2 aim).
	std::vector<unsigned char> m_aV2Bits[MAX_DUMMIES][3];
	int m_aV2EmoteNextTick[MAX_DUMMIES];
	int m_aV2AimNextTick[MAX_DUMMIES];
	int m_aV2BeaconTick[MAX_DUMMIES];
	// Current aim override (8 bits per tick).
	int m_aV2AimTargetX[MAX_DUMMIES];
	int m_aV2AimTargetY[MAX_DUMMIES];
	bool m_aV2AimActive[MAX_DUMMIES];
	// Frames waiting to ride on the next user chat line (chat channel only).
	// Each entry is a complete bit stream of one frame; a line carries as many
	// as fit its byte budget, the rest wait for the next line.
	std::vector<std::vector<unsigned char>> m_aV2ChatQueue[MAX_DUMMIES];
	std::vector<SV2Chat2Outbound> m_aV2Chat2Queue[MAX_DUMMIES];
	SV2Chat2Assembly m_aV2Chat2Asm[MAX_CLIENTS];
	// Guards EncodeChatLine against re-encoding our own generated payload lines.
	bool m_V2SuppressEncode;
	// Receive bit buffers.
	std::vector<unsigned char> m_aV2RxEmote[MAX_CLIENTS];
	std::vector<unsigned char> m_aV2RxAim[MAX_CLIENTS];
	int64_t m_aV2RxEmoteTime[MAX_CLIENTS];
	// v6.20: sample one aim bucket per received game tick.
	int m_aV2RxAimBucket[MAX_CLIENTS];
	int m_aV2RxAimTick[MAX_CLIENTS];
	// v2 detection set (parallel to m_aDetectedClient, set by any channel).
	bool m_aV2Detected[MAX_CLIENTS];
	// v6.21: fragment reassembly state per sender.
	SV2Assembly m_aV2Asm[MAX_CLIENTS];
};

#endif
