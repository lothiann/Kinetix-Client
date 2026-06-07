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

// Table 1 (Level 1): 32 most frequent characters (coded with 6 bits: 0 prefix + 5-bit index)
const std::string TABLE_1 = " оаеинтвсрлкмдпуяьгзбчйхetaoinsr";

// Table 2 (Level 2): 128 additional characters (coded with 10 bits: 10 prefix + 8-bit index)
const std::string TABLE_2 =
        "АБВГДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ"
        "єіїґэыёъ"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "bcdfghjklmpquvwxyz"
        "0123456789"
        ".,!?-+=_@#$%^&*()[]{}<>;:/\\|'\"\n\t:";

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
        m_NextEmoteSendTick = 0;
        m_LastRevealJoin = false;
        OnReset();
}

void CIRC::OnInit()
{
        OnReset();
}

void CIRC::OnReset()
{
        m_vSendQueue.clear();
        m_NextEmoteSendTick = 0;
        m_LastRevealJoin = g_Config.m_ClIrcEnabled != 0;
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
        Console()->Register("c_irc_send", "r[text]", CFGFLAG_CLIENT, ConSendIRC, this, "Send an encoded emoji message");
}

void CIRC::ConSendIRC(IConsole::IResult *pResult, void *pUserData)
{
        CIRC *pSelf = (CIRC *)pUserData;
        pSelf->SendIRC(pResult->GetString(0));
}

void CIRC::SendIRC(const char *pText)
{
        if(Client()->State() != IClient::STATE_ONLINE)
                return;

        std::vector<int> Encoded = EncodeStringToEmotes(pText);
        for(int Emote : Encoded)
        {
                m_vSendQueue.push_back(Emote);
        }
}

void CIRC::QueueHandshake()
{
        m_vSendQueue.clear();
        for(const auto &Spec : CLIENT_SPECS)
        {
                if(str_comp(Spec.m_pName, LOCAL_CLIENT_NAME) == 0)
                {
                        for(int i = 0; i < 4; ++i)
                        {
                                m_vSendQueue.push_back(Spec.m_aSequence[i]);
                        }
                        break;
                }
        }
        // Fallback if local client name not found in specs
        if(m_vSendQueue.empty())
        {
                m_vSendQueue.push_back(12);
                m_vSendQueue.push_back(1);
                m_vSendQueue.push_back(12);
                m_vSendQueue.push_back(1);
        }
}

const char *CIRC::GetClientPrefix(int ClientId) const
{
        if(ClientId < 0 || ClientId >= MAX_CLIENTS)
                return nullptr;

        // Local player check
        if(ClientId == GameClient()->m_Snap.m_LocalClientId)
        {
                return g_Config.m_ClIrcRevealJoin ? LOCAL_CLIENT_NAME : nullptr;
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
        if(g_Config.m_ClIrcEnabled && g_Config.m_ClIrcRevealJoin)
        {
                if(!m_LastRevealJoin)
                {
                        QueueHandshake();
                        m_LastRevealJoin = true;
                }

                if(!m_vSendQueue.empty())
                {
                        if(Client()->GameTick(g_Config.m_ClDummy) >= m_NextEmoteSendTick)
                        {
                                int Emote = m_vSendQueue.front();
                                m_vSendQueue.erase(m_vSendQueue.begin());
                                GameClient()->m_Emoticon.Emote(Emote);
                                // Delay between sending emotes to prevent server anti-spam (0.6s / 30 ticks is safe)
                                m_NextEmoteSendTick = Client()->GameTick(g_Config.m_ClDummy) + 30;
                        }
                }
        }
        else
        {
                m_LastRevealJoin = g_Config.m_ClIrcRevealJoin != 0;
                m_vSendQueue.clear();
        }

        // Process message buffers for timeout decoding
        if(g_Config.m_ClIrcEnabled)
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
        if(!g_Config.m_ClIrcEnabled)
                return;

        if(MsgId == NETMSGTYPE_SV_EMOTICON)
        {
                CNetMsg_Sv_Emoticon *pMsg = (CNetMsg_Sv_Emoticon *)pRawMsg;
                int ClientId = pMsg->m_ClientId;
                int Emote = pMsg->m_Emoticon;

                if(ClientId >= 0 && ClientId < MAX_CLIENTS)
                {
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

                if(g_Config.m_ClIrcEnabled && g_Config.m_ClIrcRevealJoin)
                {
                        QueueHandshake();
                        // Delay initial send on join
                        m_NextEmoteSendTick = Client()->GameTick(g_Config.m_ClDummy) + 50;
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
                m_vSendQueue.clear();
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

std::vector<int> CIRC::EncodeStringToEmotes(const std::string& text)
{
        std::vector<int> result;
        EmoteBitWriter writer(result);

        for(size_t i = 0; i < text.size(); )
        {
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
                        writer.WriteBit(0); // Level 1 Prefix
                        writer.WriteBits(pos1, 5);
                        continue;
                }

                // Search in Table 2
                int pos2 = GetUtf8CharIndex(TABLE_2, utf8Char);
                if(pos2 != -1)
                {
                        writer.WriteBit(1); // Level 2-3 Prefix
                        writer.WriteBit(0); // Level 2 Prefix
                        writer.WriteBits(pos2, 8);
                        continue;
                }

                // Level 3 Fallback (raw UTF-8 byte stream)
                writer.WriteBit(1); // Level 2-3 Prefix
                writer.WriteBit(1); // Level 3 Prefix
                writer.WriteBits(charLen, 3); // Byte size (1-4)
                for(char byte : utf8Char)
                {
                        writer.WriteBits(static_cast<unsigned char>(byte), 8);
                }
        }

        writer.Flush();
        return result;
}

std::string CIRC::DecodeEmotesToString(const std::vector<int>& emotes)
{
        std::string result = "";
        EmoteBitReader reader(emotes);

        while(true)
        {
                int levelBit = reader.ReadBit();
                if(levelBit == -1) break;

                if(levelBit == 0)
                {
                        // Level 1
                        int idx = reader.ReadBits(5);
                        if(idx == -1) break;

                        size_t charBytePos = 0;
                        int currentSymbolIndex = 0;
                        while(charBytePos < TABLE_1.size() && currentSymbolIndex < idx)
                        {
                                unsigned char c = TABLE_1[charBytePos];
                                int len = 1;
                                if((c & 0x80) == 0) len = 1;
                                else if((c & 0xE0) == 0xC0) len = 2;
                                else if((c & 0xF0) == 0xE0) len = 3;
                                else if((c & 0xF8) == 0xF0) len = 4;
                                charBytePos += len;
                                currentSymbolIndex++;
                        }
                        if(charBytePos < TABLE_1.size())
                        {
                                unsigned char c = TABLE_1[charBytePos];
                                int len = 1;
                                if((c & 0x80) == 0) len = 1;
                                else if((c & 0xE0) == 0xC0) len = 2;
                                else if((c & 0xF0) == 0xE0) len = 3;
                                else if((c & 0xF8) == 0xF0) len = 4;
                                result += TABLE_1.substr(charBytePos, len);
                        }
                }
                else
                {
                        int secondBit = reader.ReadBit();
                        if(secondBit == -1) break;

                        if(secondBit == 0)
                        {
                                // Level 2
                                int idx = reader.ReadBits(8);
                                if(idx == -1) break;

                                size_t charBytePos = 0;
                                int currentSymbolIndex = 0;
                                while(charBytePos < TABLE_2.size() && currentSymbolIndex < idx)
                                {
                                        unsigned char c = TABLE_2[charBytePos];
                                        int len = 1;
                                        if((c & 0x80) == 0) len = 1;
                                        else if((c & 0xE0) == 0xC0) len = 2;
                                        else if((c & 0xF0) == 0xE0) len = 3;
                                        else if((c & 0xF8) == 0xF0) len = 4;
                                        charBytePos += len;
                                        currentSymbolIndex++;
                                }
                                if(charBytePos < TABLE_2.size())
                                {
                                        unsigned char c = TABLE_2[charBytePos];
                                        int len = 1;
                                        if((c & 0x80) == 0) len = 1;
                                        else if((c & 0xE0) == 0xC0) len = 2;
                                        else if((c & 0xF0) == 0xE0) len = 3;
                                        else if((c & 0xF8) == 0xF0) len = 4;
                                        result += TABLE_2.substr(charBytePos, len);
                                }
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
        return result;
}
