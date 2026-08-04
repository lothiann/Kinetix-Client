#include "irc.h"
#include <game/client/gameclient.h>
#include <engine/shared/config.h>
#include <game/client/components/chat.h>
#include <base/str.h>
#include <base/time.h>
#include <algorithm>

// Local client name to identify which configuration sequence to send
const char *LOCAL_CLIENT_NAME = "DMC";

// Client configuration specifications
struct CClientSpec
{
	const char *m_pName;
	int m_aSequence[4];
};

const CClientSpec CLIENT_SPECS[] = {
	{"CFF", {10, 7, 10, 7}}, // Devil tee, Ghost, Devil tee, Ghost
	{"DMC", {12, 1, 12, 1}}  // Zzz, Exclamation, Zzz, Exclamation
};

// Define the 16 Emotes map matching the protocol designer
const std::string EMOTES[] = {
	"Oop", "Exclamation", "Hearts", "Drop", "Dot-dot-dot", "Music", "Sorry", "Ghost",
	"Sushi", "Splatter", "Devil tee", "Omg", "Zzz", "Wtf", "Eyes", "Questionmark"
};

// Table 1 (Level 1): 32 most frequent characters (coded with 0 prefix + 5-bit index)
const std::string TABLE_1 = " оаеинтвсрлкмдпуяьгзбчйхetaoinsr";

// Table 2 (Level 2): 256 additional characters (coded with 10 prefix + 8-bit index)
const std::string TABLE_2 =
	"АБВГДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ"
	"єіїґэыёъ"
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"bcdfghjklmpquvwxyz"
	"0123456789"
	".,!?-+=_@#$%^&*()[]{}<>;:/\\|'\"\n\t:";

// SYNCHRONIZED STATIC SHANNON DICTIONARY (256 tokens)
// Level 0: coded with 0 prefix + 8-bit index = 9 bits per token
const std::string DICTIONARY[256] = {
	"th", "the", "he", "при", "ha", "re", "er", "in", "or", "int", "рив", "an", "ра", "ea", "nt", "пр", "ого", "as", "at", "en", "le", "ma", "ou", "вс", "ри", "ch", "es", "ic", "la", "lo", "om", "oo", "te", "to", "го", "ив", "то", "ank", "ans", "ase", "cla", "eas", "eed", "eez", "ent", "ere", "ess", "eze", "fre", "hat", "hen", "ien", "ill", "lan", "mat", "mor", "oin", "ook", "ore", "pee", "ple", "poi", "ran", "ree", "ser", "spe", "sto", "tha", "wor", "you", "вет", "все", "всі", "від", "год", "гра", "доб", "ебе", "ере", "иве", "обр", "одн", "ому", "рав", "ств", "сьо", "тву", "ьог", "ar", "ee", "fr", "is", "ll", "me", "pe", "pl", "ra", "ro", "se", "st", "ve", "wo", "ав", "ам", "на", "но", "ре", "ст", "та", "ти", "ad", "am", "cl", "co", "de", "ed", "ez", "hi", "ho", "il", "nd", "ol", "on", "po", "so", "sp", "un", "wi", "ал", "бе", "бу", "ва", "ве", "ві", "де", "ди", "до", "еб", "ен", "ет", "ка", "ми", "му", "не", "об", "ог", "ом", "се", "ую", "чу", "як", "ід", "abl", "abo", "ace", "ach", "ade", "adm", "aha", "aid", "ail", "ake", "alt", "ame", "amm", "and", "any", "ard", "are", "arm", "asy", "atc", "ate", "ath", "ave", "awn", "ble", "blo", "bou", "bro", "cha", "che", "ckp", "cli", "con", "coo", "cor", "cus", "ddn", "dea", "dis", "dmi", "dne", "dra", "dru", "dum", "eac", "eal", "eam", "eat", "eck", "ect", "edr", "eir", "ele", "ell", "ena", "end", "eop", "epi", "epo", "erf", "erv", "est", "fai", "fec", "fla", "fly", "for", "fri", "fro", "ful", "gam", "ggw", "glh", "gre", "gun", "guy", "gwp", "hah", "ham", "han", "har", "has", "hav", "hea", "hec", "hei", "hel", "hem", "her", "hey", "hic", "him", "his", "hoo", "hot", "how", "ice", "ich", "ike", "ing", "inj", "ion", "ist", "ith"
};

// Helper class to write individual bits into 4-bit emotes (nibbles)
class EmoteBitWriter {
	std::vector<int>& m_Output;
	int m_CurrentValue = 0;
	int m_BitCount = 0;
public:
	EmoteBitWriter(std::vector<int>& out) : m_Output(out) {}

	void WriteBit(int bit) {
		m_CurrentValue = (m_CurrentValue << 1) | (bit & 1);
		m_BitCount++;
		if(m_BitCount == 4) {
			m_Output.push_back(m_CurrentValue);
			m_CurrentValue = 0;
			m_BitCount = 0;
		}
	}

	void WriteBits(uint32_t value, int count) {
		for(int i = count - 1; i >= 0; --i) {
			WriteBit((value >> i) & 1);
		}
	}

	void Flush() {
		if(m_BitCount > 0) {
			m_CurrentValue <<= (4 - m_BitCount);
			m_Output.push_back(m_CurrentValue);
			m_CurrentValue = 0;
			m_BitCount = 0;
		}
	}
};

// Helper class to read individual bits from 4-bit emotes (nibbles)
class EmoteBitReader {
	const std::vector<int>& m_Input;
	size_t m_Index = 0;
	int m_CurrentValue = 0;
	int m_BitCount = 0;
public:
	EmoteBitReader(const std::vector<int>& in) : m_Input(in) {}

	int ReadBit() {
		if(m_BitCount == 0) {
			if(m_Index >= m_Input.size()) return -1;
			m_CurrentValue = m_Input[m_Index++];
			m_BitCount = 4;
		}
		int bit = (m_CurrentValue >> (m_BitCount - 1)) & 1;
		m_BitCount--;
		return bit;
	}

	int ReadBits(int count) {
		int val = 0;
		for(int i = 0; i < count; ++i) {
			int bit = ReadBit();
			if(bit == -1) return -1;
			val = (val << 1) | bit;
		}
		return val;
	}
};

CIRC::CIRC()
{
	m_LastRevealJoin = false;
	OnReset();
}

void CIRC::OnInit()
{
	OnReset();
}

void CIRC::OnReset()
{
	for(int d = 0; d < MAX_DUMMIES; ++d)
	{
		m_aSendQueue[d].clear();
		m_aNextEmoteSendTick[d] = 0;
		m_aHandshakeSent[d] = false;
	}
	m_LastRevealJoin = g_Config.m_KxIrcEnabled != 0;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		m_aEmoteHistory[i].clear();
		m_aDetectedClient[i][0] = '\0';
		m_aMsgBuffer[i].clear();
		m_aLastEmoteTime[i] = 0;
	}
}

void CIRC::OnConsoleInit()
{
	Console()->Register("kx_irc_send", "r[text]", CFGFLAG_CLIENT, ConSendIRC, this, "Send an encoded emoji message");
}

void CIRC::ConSendIRC(IConsole::IResult *pResult, void *pUserData)
{
	CIRC *pSelf = (CIRC *)pUserData;
	pSelf->SendIRC(pResult->GetString(0));
}

bool CIRC::IsOwnClient(int ClientId) const
{
	for(int d = 0; d < MAX_DUMMIES; ++d)
	{
		if(GameClient()->m_aLocalIds[d] == ClientId)
			return true;
	}
	return false;
}

void CIRC::SendIRC(const char *pText)
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	// Queue the message on the active dummy
	std::vector<int> Encoded = EncodeStringToEmotes(pText);
	int ActiveDummy = g_Config.m_ClDummy;
	for(int Emote : Encoded)
	{
		m_aSendQueue[ActiveDummy].push_back(Emote);
	}
}

void CIRC::QueueHandshake(int Dummy)
{
	// Skip already-sent dummies or disconnected dummies
	if(Dummy != 0 && !Client()->DummyConnected(Dummy))
		return;
	if(m_aHandshakeSent[Dummy])
		return;

	m_aSendQueue[Dummy].clear();
	for(const auto &Spec : CLIENT_SPECS)
	{
		if(str_comp(Spec.m_pName, LOCAL_CLIENT_NAME) == 0)
		{
			for(int i = 0; i < 4; ++i)
			{
				m_aSendQueue[Dummy].push_back(Spec.m_aSequence[i]);
			}
			break;
		}
	}
	// Fallback if local client name not found in specs
	if(m_aSendQueue[Dummy].empty())
	{
		m_aSendQueue[Dummy].push_back(12);
		m_aSendQueue[Dummy].push_back(1);
		m_aSendQueue[Dummy].push_back(12);
		m_aSendQueue[Dummy].push_back(1);
	}
	m_aHandshakeSent[Dummy] = true;
}

void CIRC::SendEmoteOnDummy(int Dummy, int Emoticon)
{
	if(Dummy == g_Config.m_ClDummy)
	{
		// Active dummy — use the standard emote path
		GameClient()->m_Emoticon.Emote(Emoticon);
	}
	else
	{
		// Non-active dummy — send directly on its connection
		CMsgPacker Msg(NETMSGTYPE_CL_EMOTICON, false);
		Msg.AddInt(Emoticon);
		int Conn = IClient::CONN_DUMMY_START + Dummy - 1;
		Client()->SendMsg(Conn, &Msg, MSGFLAG_VITAL);
	}
}

const char *CIRC::GetClientPrefix(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return nullptr;

	// Check all local dummies, not just the active one
	if(IsOwnClient(ClientId))
	{
		return g_Config.m_KxIrcRevealJoin ? LOCAL_CLIENT_NAME : nullptr;
	}

	if(m_aDetectedClient[ClientId][0] != '\0')
		return m_aDetectedClient[ClientId];

	return nullptr;
}

void CIRC::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	// Handle reveal join toggle / sending handshake
	if(g_Config.m_KxIrcEnabled && g_Config.m_KxIrcRevealJoin)
	{
		if(!m_LastRevealJoin)
		{
			// Queue handshake for all connected dummies
			for(int d = 0; d < MAX_DUMMIES; ++d)
			{
				QueueHandshake(d);
				if(m_aHandshakeSent[d])
				{
					// Stagger initial send: 50 ticks for main, +20 per additional dummy
					m_aNextEmoteSendTick[d] = Client()->GameTick(0) + 50 + d * 20;
				}
			}
			m_LastRevealJoin = true;
		}

		// Check for newly connected dummies that still need handshake
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			if(d != 0 && !Client()->DummyConnected(d))
				continue;
			if(!m_aHandshakeSent[d])
			{
				QueueHandshake(d);
				m_aNextEmoteSendTick[d] = Client()->GameTick(0) + 50;
			}
		}

		// Process send queues for all dummies independently
		int CurTick = Client()->GameTick(0);
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			if(d != 0 && !Client()->DummyConnected(d))
				continue;

			if(!m_aSendQueue[d].empty() && CurTick >= m_aNextEmoteSendTick[d])
			{
				int Emote = m_aSendQueue[d].front();
				m_aSendQueue[d].erase(m_aSendQueue[d].begin());
				SendEmoteOnDummy(d, Emote);
				// Delay between emotes per dummy (0.6s / 30 ticks)
				m_aNextEmoteSendTick[d] = CurTick + 30;
			}
		}
	}
	else
	{
		m_LastRevealJoin = g_Config.m_KxIrcRevealJoin != 0;
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			m_aSendQueue[d].clear();
			m_aHandshakeSent[d] = false;
		}
	}

	// Process message buffers for timeout decoding
	if(g_Config.m_KxIrcEnabled)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!m_aMsgBuffer[i].empty() && time_get() > m_aLastEmoteTime[i] + time_freq() * 3)
			{
				bool IsHandshake = false;
				for(const auto &Spec : CLIENT_SPECS)
				{
					if(m_aMsgBuffer[i].size() == 4 &&
						m_aMsgBuffer[i][0] == Spec.m_aSequence[0] &&
						m_aMsgBuffer[i][1] == Spec.m_aSequence[1] &&
						m_aMsgBuffer[i][2] == Spec.m_aSequence[2] &&
						m_aMsgBuffer[i][3] == Spec.m_aSequence[3])
					{
						IsHandshake = true;
						break;
					}
				}

				if(!IsHandshake && m_aMsgBuffer[i].size() >= 2)
				{
					std::string Decoded = DecodeEmotesToString(m_aMsgBuffer[i]);
					if(!Decoded.empty())
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "[IRC] %s: %s", GameClient()->m_aClients[i].m_aName, Decoded.c_str());
						GameClient()->m_Chat.Echo(aBuf);
					}
				}
				m_aMsgBuffer[i].clear();
			}
		}
	}
}

void CIRC::OnMessage(int MsgId, void *pRawMsg)
{
	if(!g_Config.m_KxIrcEnabled)
		return;

	if(MsgId == NETMSGTYPE_SV_EMOTICON)
	{
		CNetMsg_Sv_Emoticon *pMsg = (CNetMsg_Sv_Emoticon *)pRawMsg;
		int ClientId = pMsg->m_ClientId;
		int Emote = pMsg->m_Emoticon;

		if(ClientId >= 0 && ClientId < MAX_CLIENTS)
		{
			// Skip emotes from our own dummies (avoid self-echo)
			if(IsOwnClient(ClientId))
				return;

			// Emote history for signature detection
			m_aEmoteHistory[ClientId].push_back(Emote);
			if(m_aEmoteHistory[ClientId].size() > 4)
			{
				m_aEmoteHistory[ClientId].erase(m_aEmoteHistory[ClientId].begin());
			}

			if(m_aEmoteHistory[ClientId].size() == 4)
			{
				for(const auto &Spec : CLIENT_SPECS)
				{
					if(m_aEmoteHistory[ClientId][0] == Spec.m_aSequence[0] &&
						m_aEmoteHistory[ClientId][1] == Spec.m_aSequence[1] &&
						m_aEmoteHistory[ClientId][2] == Spec.m_aSequence[2] &&
						m_aEmoteHistory[ClientId][3] == Spec.m_aSequence[3])
					{
						str_copy(m_aDetectedClient[ClientId], Spec.m_pName, sizeof(m_aDetectedClient[ClientId]));
						break;
					}
				}
			}

			// Add to message buffer for decoding
			m_aMsgBuffer[ClientId].push_back(Emote);
			m_aLastEmoteTime[ClientId] = time_get();
		}
	}
}

void CIRC::OnStateChange(int NewState, int OldState)
{
	if(NewState == IClient::STATE_ONLINE)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			m_aEmoteHistory[i].clear();
			m_aDetectedClient[i][0] = '\0';
			m_aMsgBuffer[i].clear();
			m_aLastEmoteTime[i] = 0;
		}

		// Reset per-dummy state
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			m_aSendQueue[d].clear();
			m_aHandshakeSent[d] = false;
			m_aNextEmoteSendTick[d] = 0;
		}

		if(g_Config.m_KxIrcEnabled && g_Config.m_KxIrcRevealJoin)
		{
			// Queue handshake for all connected dummies
			for(int d = 0; d < MAX_DUMMIES; ++d)
			{
				QueueHandshake(d);
				if(m_aHandshakeSent[d])
				{
					m_aNextEmoteSendTick[d] = Client()->GameTick(0) + 50 + d * 20;
				}
			}
		}
	}
	else if(NewState == IClient::STATE_OFFLINE)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			m_aEmoteHistory[i].clear();
			m_aDetectedClient[i][0] = '\0';
			m_aMsgBuffer[i].clear();
			m_aLastEmoteTime[i] = 0;
		}
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			m_aSendQueue[d].clear();
			m_aHandshakeSent[d] = false;
			m_aNextEmoteSendTick[d] = 0;
		}
	}
}

int CIRC::GetUtf8CharIndex(const std::string &table, const std::string &utf8Char)
{
	size_t charBytePos = 0;
	int currentSymbolIndex = 0;
	while(charBytePos < table.size())
	{
		unsigned char c = table[charBytePos];
		int len = 1;
		if((c & 0x80) == 0) len = 1;
		else if((c & 0xE0) == 0xC0) len = 2;
		else if((c & 0xF0) == 0xE0) len = 3;
		else if((c & 0xF8) == 0xF0) len = 4;

		if(table.substr(charBytePos, len) == utf8Char)
		{
			return currentSymbolIndex;
		}

		charBytePos += len;
		currentSymbolIndex++;
	}
	return -1;
}

std::string CIRC::GetUtf8CharFromTable(const std::string& table, size_t index)
{
	size_t bytePos = 0;
	size_t symbolIdx = 0;
	while(bytePos < table.size() && symbolIdx < index)
	{
		unsigned char c = table[bytePos];
		int len = 1;
		if((c & 0x80) == 0) len = 1;
		else if((c & 0xE0) == 0xC0) len = 2;
		else if((c & 0xF0) == 0xE0) len = 3;
		else if((c & 0xF8) == 0xF0) len = 4;
		bytePos += len;
		symbolIdx++;
	}
	if(bytePos < table.size())
	{
		unsigned char c = table[bytePos];
		int len = 1;
		if((c & 0x80) == 0) len = 1;
		else if((c & 0xE0) == 0xC0) len = 2;
		else if((c & 0xF0) == 0xE0) len = 3;
		else if((c & 0xF8) == 0xF0) len = 4;
		return table.substr(bytePos, len);
	}
	return "";
}

// ENCODING — with Level 0 dictionary compression
std::vector<int> CIRC::EncodeStringToEmotes(const std::string& text)
{
	std::vector<int> result;
	EmoteBitWriter writer(result);

	for(size_t i = 0; i < text.size(); )
	{
		bool matchFound = false;

		// Search for the longest matching chunk in DICTIONARY (Level 0)
		int bestIndex = -1;
		size_t bestLen = 0;
		for(int idx = 0; idx < 256; ++idx)
		{
			const std::string& token = DICTIONARY[idx];
			if(token.empty()) continue;
			if(text.compare(i, token.length(), token) == 0)
			{
				if(token.length() > bestLen)
				{
					bestLen = token.length();
					bestIndex = idx;
				}
			}
		}

		if(bestIndex != -1)
		{
			writer.WriteBit(0); // Level 0
			writer.WriteBits(bestIndex, 8);
			i += bestLen;
			continue;
		}

		// If no dictionary match, encode one UTF-8 character
		unsigned char c = text[i];
		int charLen = 1;
		if((c & 0x80) == 0) charLen = 1;
		else if((c & 0xE0) == 0xC0) charLen = 2;
		else if((c & 0xF0) == 0xE0) charLen = 3;
		else if((c & 0xF8) == 0xF0) charLen = 4;

		if(i + charLen > text.size()) charLen = text.size() - i;
		std::string utf8Char = text.substr(i, charLen);
		i += charLen;

		// Search in Table 1
		int pos1 = GetUtf8CharIndex(TABLE_1, utf8Char);
		if(pos1 != -1)
		{
			writer.WriteBit(1);
			writer.WriteBit(0); // Level 1
			writer.WriteBits(pos1, 5);
			continue;
		}

		// Search in Table 2
		int pos2 = GetUtf8CharIndex(TABLE_2, utf8Char);
		if(pos2 != -1)
		{
			writer.WriteBit(1);
			writer.WriteBit(1);
			writer.WriteBit(0); // Level 2
			writer.WriteBits(pos2, 8);
			continue;
		}

		// Level 3 Fallback (raw UTF-8 byte stream)
		writer.WriteBit(1);
		writer.WriteBit(1);
		writer.WriteBit(1);
		writer.WriteBits(charLen, 3); // Byte size (1-4)
		for(char byte : utf8Char)
		{
			writer.WriteBits(static_cast<unsigned char>(byte), 8);
		}
	}

	writer.Flush();
	return result;
}

// DECODING — with Level 0 dictionary compression
std::string CIRC::DecodeEmotesToString(const std::vector<int>& emotes)
{
	std::string result = "";
	EmoteBitReader reader(emotes);

	while(true)
	{
		int firstBit = reader.ReadBit();
		if(firstBit == -1) break;

		if(firstBit == 0)
		{
			// Level 0: Dictionary token
			int idx = reader.ReadBits(8);
			if(idx == -1 || idx < 0 || idx >= 256) break;
			result += DICTIONARY[idx];
		}
		else
		{
			int secondBit = reader.ReadBit();
			if(secondBit == -1) break;

			if(secondBit == 0)
			{
				// Level 1
				int idx = reader.ReadBits(5);
				if(idx == -1) break;
				result += GetUtf8CharFromTable(TABLE_1, idx);
			}
			else
			{
				int thirdBit = reader.ReadBit();
				if(thirdBit == -1) break;

				if(thirdBit == 0)
				{
					// Level 2
					int idx = reader.ReadBits(8);
					if(idx == -1) break;
					result += GetUtf8CharFromTable(TABLE_2, idx);
				}
				else
				{
					// Level 3: Raw UTF-8 bytes
					int byteLen = reader.ReadBits(3);
					if(byteLen == -1) break;
					std::string utf8Char = "";
					for(int b = 0; b < byteLen; ++b)
					{
						int val = reader.ReadBits(8);
						if(val == -1) break;
						utf8Char += static_cast<char>(val);
					}
					result += utf8Char;
				}
			}
		}
	}
	return result;
}
