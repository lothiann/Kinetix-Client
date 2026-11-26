#include "irc.h"
#include <game/client/gameclient.h>
#include <engine/shared/config.h>
#include <game/client/components/chat.h>
#include <game/collision.h>
#include <game/mapitems.h>
#include <base/math.h>
#include <base/str.h>
#include <base/time.h>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

// Local client name to identify which configuration sequence to send
const char *LOCAL_CLIENT_NAME = "DMC";

// ============================================================================
// IRC v2 — zero-width steganography
// ============================================================================
namespace
{
	// Invisible code points used by the payload, four bits per character. All of
	// them exist in DDNet's default font (DejaVuSans) and render as nothing.
	// Plane 14 tag characters were considered (they are not Unicode whitespace,
	// so DDNet's trimming cannot eat them) but the font has no glyphs for them,
	// so they would show up as replacement boxes in every client.
	const char *const ZW[16] = {
		"\xE2\x80\x8B", // U+200B ZERO WIDTH SPACE
		"\xE2\x80\x8C", // U+200C ZERO WIDTH NON-JOINER
		"\xE2\x80\x8D", // U+200D ZERO WIDTH JOINER
		"\xE2\x81\xA0", // U+2060 WORD JOINER
		"\xEF\xBB\xBF", // U+FEFF ZERO WIDTH NO-BREAK SPACE
		"\xE2\x80\x8E", // U+200E LEFT-TO-RIGHT MARK
		"\xE2\x80\x8F", // U+200F RIGHT-TO-LEFT MARK
		"\xE2\x81\xA1", // U+2061 FUNCTION APPLICATION
		"\xE2\x81\xA2", // U+2062 INVISIBLE TIMES
		"\xE2\x81\xA3", // U+2063 INVISIBLE SEPARATOR
		"\xE2\x81\xA4", // U+2064 INVISIBLE PLUS
		"\xE2\x80\xAA", // U+202A LEFT-TO-RIGHT EMBEDDING
		"\xE2\x80\xAB", // U+202B RIGHT-TO-LEFT EMBEDDING
		"\xE2\x80\xAC", // U+202C POP DIRECTIONAL FORMATTING
		"\xE2\x80\xAD", // U+202D LEFT-TO-RIGHT OVERRIDE
		"\xE2\x80\xAE", // U+202E RIGHT-TO-LEFT OVERRIDE
	};
	constexpr int ZW_BITS = 4;
	constexpr int ZW_BYTES = 3;

	// Identity signature: an adjacent pair of symbols, order-sensitive. Its
	// position does not matter (the server may prepend "(1)" or cut the tail);
	// ordinary text never contains zero-width characters, so false positives are
	// practically impossible.
	constexpr int IDENT_SYM_A = 0; // U+200B
	constexpr int IDENT_SYM_B = 9; // U+2063
	constexpr int IDENT_BYTES = ZW_BYTES * 2;

	// Private salts for the pseudo-random placement (not a security boundary).
	constexpr uint64_t SALT_NAME = 0x4B584E414D455A31ULL; // "KXNAMEZ1"
	constexpr uint64_t SALT_CLAN = 0x4B58434C414E5A31ULL; // "KXCLANZ1"
	constexpr uint64_t SALT_CHAT = 0x4B58434841545A31ULL; // "KXCHATZ1"

	// Channel magic values (bit width in the second field).
	constexpr uint32_t MAGIC_TEXT = 0xB5; // 12 bits, zero-width / emote
	constexpr int MAGIC_TEXT_BITS = 12;
	constexpr uint32_t MAGIC_AIM = 0xB585; // 16 bits, aim channel
	constexpr int MAGIC_AIM_BITS = 16;

	int Utf8Len(unsigned char c)
	{
		if((c & 0x80) == 0)
			return 1;
		if((c & 0xE0) == 0xC0)
			return 2;
		if((c & 0xF0) == 0xE0)
			return 3;
		if((c & 0xF8) == 0xF0)
			return 4;
		return 1;
	}

	// Returns the zero-width symbol index at p (0..15), or -1.
	int ZwSymbolAt(const char *p, int Remaining)
	{
		if(Remaining < ZW_BYTES)
			return -1;
		for(int i = 0; i < 16; ++i)
		{
			if(memcmp(p, ZW[i], ZW_BYTES) == 0)
				return i;
		}
		return -1;
	}

	struct SZwEntry
	{
		int m_Symbol;
		int m_CharPos; // number of visible characters before this marker
	};

	int CountUtf8Chars(const std::string &Text)
	{
		int Count = 0;
		for(size_t i = 0; i < Text.size();)
		{
			int Len = Utf8Len((unsigned char)Text[i]);
			if(i + Len > (int)Text.size())
				Len = (int)(Text.size() - i);
			i += Len;
			Count++;
		}
		return Count;
	}

	uint64_t Fnv1a(const std::string &Text, uint64_t Salt)
	{
		uint64_t Hash = 1469598103934665603ULL ^ Salt;
		for(unsigned char c : Text)
		{
			Hash ^= c;
			Hash *= 1099511628211ULL;
		}
		return Hash;
	}

	// Pseudo-random insertion point: a character index in [1, n-1] derived from
	// the hash of the visible text, i.e. somewhere *between* characters.
	// Returns -1 when the text is too short.
	int RandomCharPos(const std::string &Visible, uint64_t Salt)
	{
		const int n = CountUtf8Chars(Visible);
		if(n < 2)
			return -1;
		const uint64_t Seed = Fnv1a(Visible, Salt);
		return 1 + (int)(Seed % (uint64_t)(n - 1));
	}

	// Byte offset of character index CharPos inside Visible.
	int ByteOffsetOfChar(const std::string &Visible, int CharPos)
	{
		int Char = 0;
		for(size_t i = 0; i < Visible.size();)
		{
			if(Char == CharPos)
				return (int)i;
			int Len = Utf8Len((unsigned char)Visible[i]);
			if(i + Len > Visible.size())
				Len = (int)(Visible.size() - i);
			i += Len;
			Char++;
		}
		return (int)Visible.size();
	}

	// Splits pText into the visible text (zero-width payload removed) and the
	// list of markers with their visible-char positions.
	void SplitText(const char *pText, std::string &Visible, std::vector<SZwEntry> &vZw)
	{
		Visible.clear();
		vZw.clear();
		int CharPos = 0;
		for(int i = 0; pText[i] != '\0';)
		{
			const int Remain = (int)strlen(pText + i);
			const int Sym = ZwSymbolAt(pText + i, Remain);
			if(Sym >= 0)
			{
				SZwEntry Entry;
				Entry.m_Symbol = Sym;
				Entry.m_CharPos = CharPos;
				vZw.push_back(Entry);
				i += ZW_BYTES;
				continue;
			}
			int Len = Utf8Len((unsigned char)pText[i]);
			if(i + Len > Remain)
				Len = Remain;
			Visible.append(pText + i, Len);
			i += Len;
			CharPos++;
		}
	}
}

void KxIrcStego::VisibleText(const char *pText, char *pOut, int OutSize)
{
	if(!pText || !pOut || OutSize <= 0)
		return;
	std::string Visible;
	std::vector<SZwEntry> vZw;
	SplitText(pText, Visible, vZw);
	str_copy(pOut, Visible.c_str(), OutSize);
}

bool KxIrcStego::HasIdentity(const char *pText, bool Clan, char *pVisible, int VisibleSize)
{
	if(!pText || pText[0] == '\0')
		return false;
	std::string Visible;
	std::vector<SZwEntry> vZw;
	SplitText(pText, Visible, vZw);
	if(pVisible && VisibleSize > 0)
		str_copy(pVisible, Visible.c_str(), VisibleSize);
	// The signature is an adjacent pair of symbols; its position does not matter
	// (the server may prepend "(1)" or cut the tail).
	for(size_t i = 0; i + 1 < vZw.size(); ++i)
	{
		if(vZw[i].m_Symbol == IDENT_SYM_A && vZw[i + 1].m_Symbol == IDENT_SYM_B)
			return true;
	}
	// Fallback for tight name/clan budgets: a single marker. Ordinary players
	// never carry zero-width characters in nick/clan, so this stays unambiguous.
	return !vZw.empty();
}

bool KxIrcStego::BuildIdentity(const char *pText, bool Clan, int MaxBytes, char *pOut, int OutSize)
{
	if(!pText || !pOut || OutSize <= 0)
		return false;
	std::string Visible;
	std::vector<SZwEntry> vZw;
	SplitText(pText, Visible, vZw);
	if(Visible.empty())
		return false;
	// Rebuild from the clean visible text; drop any old watermark. Prefer the
	// two-symbol signature, fall back to a single marker when the field is
	// tight. MaxBytes is the usable *content* budget: nick 13 bytes, clan 9
	// bytes — StrToInts() stores 4/3 ints and zeroes the last three bytes, so
	// the NUL does not need a byte of that budget.
	const bool PairFits = (int)(Visible.size() + IDENT_BYTES) <= MaxBytes && (int)(Visible.size() + IDENT_BYTES + 1) <= OutSize;
	const bool SingleFits = (int)(Visible.size() + ZW_BYTES) <= MaxBytes && (int)(Visible.size() + ZW_BYTES + 1) <= OutSize;
	if(!PairFits && !SingleFits)
		return false;
	// Pseudo-random position between characters.
	const int CharPos = RandomCharPos(Visible, Clan ? SALT_CLAN : SALT_NAME);
	if(CharPos < 0)
	{
		str_copy(pOut, Visible.c_str(), OutSize);
		return false;
	}
	const int InsertAt = ByteOffsetOfChar(Visible, CharPos);

	std::string Out;
	Out.append(Visible, 0, InsertAt);
	Out.append(ZW[IDENT_SYM_A], ZW_BYTES);
	if(PairFits)
		Out.append(ZW[IDENT_SYM_B], ZW_BYTES);
	Out.append(Visible, InsertAt, std::string::npos);
	str_copy(pOut, Out.c_str(), OutSize);
	return true;
}

namespace
{
	// ---- bit helpers ----
	void PushBits(std::vector<unsigned char> &vBits, uint32_t Value, int Count)
	{
		for(int i = Count - 1; i >= 0; --i)
			vBits.push_back((Value >> i) & 1);
	}

	bool ReadBits(const std::vector<unsigned char> &vBits, size_t &Pos, int Count, uint32_t &Out)
	{
		if(Pos + (size_t)Count > vBits.size())
			return false;
		uint32_t r = 0;
		for(int i = 0; i < Count; ++i)
			r = (r << 1) | (vBits[Pos++] & 1);
		Out = r;
		return true;
	}

	unsigned char Crc8(const unsigned char *pData, int Len)
	{
		unsigned Crc = 0;
		for(int i = 0; i < Len; ++i)
		{
			Crc ^= pData[i];
			for(int b = 0; b < 8; ++b)
				Crc = (Crc & 0x80) ? ((Crc << 1) ^ 0x07) : (Crc << 1);
		}
		return (unsigned char)(Crc & 0xFF);
	}

	// v6.21: frame layout lives in irc.h (SKxIrcFrame) so the component can pass
	// a decoded frame around.
	using SFrame = SKxIrcFrame;

	// v6.21: a text frame carries the dictionary-compressed text (4-bit nibbles,
	// the same stream the v1 emote encoder produces) instead of raw UTF-8 bytes.
	// Raw bytes needed 8 bits per byte, i.e. 16 bits per Cyrillic character, so
	// anything longer than ~19 Russian characters never fit a chat line. The
	// compressed stream is 4-8x smaller and is additionally split into
	// fragments (one per chat line) when it still does not fit.
	//   magic | 1 (text) | frag (1) | [total (4) | index (4)] | nibbles (12) | 4*nib | crc (8)
	constexpr int NIB_COUNT_BITS = 12;
	constexpr int KX_V2_MAX_FRAG_NIBBLES = 40;
	constexpr int KX_V2_MAX_FRAGMENTS = 15; // 4-bit total field

	// A zero-width symbol carries 4 bits, so a frame stream must be padded to a
	// multiple of 4 bits — otherwise BitsToZw drops the trailing bits and the
	// frame (its CRC tail included) arrives truncated.
	void PadBits(std::vector<unsigned char> &vBits)
	{
		while(vBits.size() % ZW_BITS != 0)
			vBits.push_back(0);
	}

	// Two nibbles per byte, the high nibble first (mirrors the bit order).
	std::vector<unsigned char> PackNibbles(const std::vector<int> &vNibbles)
	{
		std::vector<unsigned char> vBytes((vNibbles.size() + 1) / 2, 0);
		for(size_t i = 0; i < vNibbles.size(); ++i)
		{
			if(i % 2 == 0)
				vBytes[i / 2] = (unsigned char)((vNibbles[i] & 0xF) << 4);
			else
				vBytes[i / 2] |= (unsigned char)(vNibbles[i] & 0xF);
		}
		return vBytes;
	}

	unsigned char FrameCrc(bool Frag, int Index, int Total, const std::vector<int> &vNibbles)
	{
		const std::vector<unsigned char> vPayload = PackNibbles(vNibbles);
		unsigned char aHeader[4] = {(unsigned char)(Frag ? 1 : 0), (unsigned char)Index, (unsigned char)Total, (unsigned char)(vNibbles.size() & 0xFF)};
		std::vector<unsigned char> vAll(aHeader, aHeader + 4);
		vAll.insert(vAll.end(), vPayload.begin(), vPayload.end());
		return Crc8(vAll.data(), (int)vAll.size());
	}

	void BuildIdentityFrame(std::vector<unsigned char> &vBits, uint32_t Magic, int MagicBits)
	{
		PushBits(vBits, Magic, MagicBits);
		PushBits(vBits, 0, 1); // identity flag
		unsigned char Payload = 0;
		PushBits(vBits, Crc8(&Payload, 1), 8);
		PadBits(vBits);
	}

	void BuildTextFrame(std::vector<unsigned char> &vBits, uint32_t Magic, int MagicBits,
		const std::vector<int> &vNibbles, int Index, int Total)
	{
		const bool Frag = Total > 1;
		PushBits(vBits, Magic, MagicBits);
		PushBits(vBits, 1, 1); // text flag
		PushBits(vBits, Frag ? 1 : 0, 1);
		if(Frag)
		{
			PushBits(vBits, (uint32_t)Total, 4);
			PushBits(vBits, (uint32_t)Index, 4);
		}
		PushBits(vBits, (uint32_t)vNibbles.size(), NIB_COUNT_BITS);
		for(int Nib : vNibbles)
			PushBits(vBits, (uint32_t)(Nib & 0xF), 4);
		PushBits(vBits, FrameCrc(Frag, Index, Total, vNibbles), 8);
		PadBits(vBits);
	}

	// Splits the compressed stream into frames small enough to ride on one chat
	// line (or one whisper) each.
	void BuildTextFrames(std::vector<std::vector<unsigned char>> &vOut, uint32_t Magic, int MagicBits,
		const std::vector<int> &vNibbles, int MaxNibbles)
	{
		if(vNibbles.empty())
			return;
		if(MaxNibbles < 1)
			MaxNibbles = 1;
		int Total = (int)((vNibbles.size() + MaxNibbles - 1) / MaxNibbles);
		if(Total > KX_V2_MAX_FRAGMENTS)
			Total = KX_V2_MAX_FRAGMENTS;
		if(Total < 1)
			Total = 1;
		const int Chunk = (int)((vNibbles.size() + Total - 1) / Total);
		Total = (int)((vNibbles.size() + Chunk - 1) / Chunk);
		for(size_t i = 0, Idx = 0; i < vNibbles.size(); i += Chunk, ++Idx)
		{
			const size_t End = std::min(i + (size_t)Chunk, vNibbles.size());
			std::vector<int> vPart(vNibbles.begin() + i, vNibbles.begin() + End);
			std::vector<unsigned char> vBits;
			BuildTextFrame(vBits, Magic, MagicBits, vPart, (int)Idx, Total);
			vOut.push_back(vBits);
		}
	}

	void DecodeFrames(const std::vector<unsigned char> &vBits, uint32_t Magic, int MagicBits, std::vector<SFrame> &vOut, size_t *pEnd = nullptr)
	{
		size_t Pos = 0;
		size_t LastEnd = 0;
		while(Pos + MagicBits + 1 + 8 <= vBits.size())
		{
			size_t P = Pos;
			uint32_t M = 0;
			if(!ReadBits(vBits, P, MagicBits, M))
				break;
			if(M != Magic)
			{
				Pos++;
				continue;
			}
			uint32_t Type = 0;
			if(!ReadBits(vBits, P, 1, Type))
			{
				Pos++;
				continue;
			}
			if(Type == 0)
			{
				uint32_t Crc = 0;
				if(!ReadBits(vBits, P, 8, Crc))
				{
					Pos++;
					continue;
				}
				unsigned char Payload = 0;
				if(Crc8(&Payload, 1) == Crc)
				{
					SFrame Frame;
					Frame.m_Identity = true;
					Frame.m_Frag = false;
					Frame.m_Index = 0;
					Frame.m_Total = 1;
					vOut.push_back(Frame);
					LastEnd = P;
					Pos = P;
					continue;
				}
				Pos++;
				continue;
			}
			uint32_t Frag = 0;
			if(!ReadBits(vBits, P, 1, Frag))
			{
				Pos++;
				continue;
			}
			uint32_t Total = 1;
			uint32_t Index = 0;
			if(Frag && (!ReadBits(vBits, P, 4, Total) || !ReadBits(vBits, P, 4, Index)))
			{
				Pos++;
				continue;
			}
			uint32_t NibCount = 0;
			if(!ReadBits(vBits, P, NIB_COUNT_BITS, NibCount) || NibCount == 0 || NibCount > 4095)
			{
				Pos++;
				continue;
			}
			if(P + (size_t)NibCount * 4 + 8 > vBits.size())
			{
				Pos++;
				continue;
			}
			std::vector<int> vNibbles;
			vNibbles.reserve(NibCount);
			for(uint32_t i = 0; i < NibCount; ++i)
			{
				uint32_t N = 0;
				ReadBits(vBits, P, 4, N);
				vNibbles.push_back((int)(N & 0xF));
			}
			uint32_t Crc = 0;
			ReadBits(vBits, P, 8, Crc);
			if(FrameCrc(Frag != 0, (int)Index, (int)Total, vNibbles) == Crc)
			{
				SFrame Frame;
				Frame.m_Identity = false;
				Frame.m_Frag = Frag != 0;
				Frame.m_Index = (int)Index;
				Frame.m_Total = (int)Total;
				Frame.m_vNibbles = vNibbles;
				vOut.push_back(Frame);
				LastEnd = P;
				Pos = P;
				continue;
			}
			Pos++;
		}
		if(pEnd)
			*pEnd = LastEnd;
	}

	// Converts a bit stream to zero-width characters (4 bits each).
	std::string BitsToZw(const std::vector<unsigned char> &vBits, int MaxBytes)
	{
		std::string Out;
		int Budget = MaxBytes;
		for(size_t i = 0; i + ZW_BITS <= vBits.size(); i += ZW_BITS)
		{
			if(Budget < ZW_BYTES)
				break;
			int Sym = 0;
			for(int b = 0; b < ZW_BITS; ++b)
				Sym = (Sym << 1) | (vBits[i + b] & 1);
			Out.append(ZW[Sym & 0xF], ZW_BYTES);
			Budget -= ZW_BYTES;
		}
		return Out;
	}

	// Reads zero-width characters from pText into a bit stream.
	std::vector<unsigned char> ZwToBits(const char *pText)
	{
		std::vector<unsigned char> vBits;
		std::string Visible;
		std::vector<SZwEntry> vZw;
		SplitText(pText, Visible, vZw);
		for(const SZwEntry &Entry : vZw)
		{
			for(int b = ZW_BITS - 1; b >= 0; --b)
				vBits.push_back((Entry.m_Symbol >> b) & 1);
		}
		return vBits;
	}

	// Aim channel: 8 bits per tick, encoded as a direction (bucket centre).
	void BucketToAim(int Bucket, int &TargetX, int &TargetY)
	{
		const float Angle = (Bucket + 0.5f) * 2.0f * pi / 256.0f;
		TargetX = (int)(cosf(Angle) * 1000.0f);
		TargetY = (int)(sinf(Angle) * 1000.0f);
	}

	int AimToBucket(int Angle)
	{
		float a = Angle / 256.0f;
		if(a > pi)
			a -= 2.0f * pi;
		if(a < 0.0f)
			a += 2.0f * pi;
		int Bucket = (int)(a / (2.0f * pi) * 256.0f);
		return Bucket & 0xFF;
	}
}

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

// SYNCHRONIZED STATIC SHANNON DICTIONARY (256 tokens) — the IRC v1 wire format.
// Do not change the contents: v1 clients must keep decoding each other.
// Level 0: coded with 0 prefix + 8-bit index = 9 bits per token
const std::string DICTIONARY[256] = {
	"th", "the", "he", "при", "ha", "re", "er", "in", "or", "int", "рив", "an", "ра", "ea", "nt", "пр", "ого", "as", "at", "en", "le", "ma", "ou", "вс", "ри", "ch", "es", "ic", "la", "lo", "om", "oo", "te", "to", "го", "ив", "то", "ank", "ans", "ase", "cla", "eas", "eed", "eez", "ent", "ere", "ess", "eze", "fre", "hat", "hen", "ien", "ill", "lan", "mat", "mor", "oin", "ook", "ore", "pee", "ple", "poi", "ran", "ree", "ser", "spe", "sto", "tha", "wor", "you", "вет", "все", "всі", "від", "год", "гра", "доб", "ебе", "ере", "иве", "обр", "одн", "ому", "рав", "ств", "сьо", "тву", "ьог", "ar", "ee", "fr", "is", "ll", "me", "pe", "pl", "ra", "ro", "se", "st", "ve", "wo", "ав", "ам", "на", "но", "ре", "ст", "та", "ти", "ad", "am", "cl", "co", "de", "ed", "ez", "hi", "ho", "il", "nd", "ol", "on", "po", "so", "sp", "un", "wi", "ал", "бе", "бу", "ва", "ве", "ві", "де", "ди", "до", "еб", "ен", "ет", "ка", "ми", "му", "не", "об", "ог", "ом", "се", "ую", "чу", "як", "ід", "abl", "abo", "ace", "ach", "ade", "adm", "aha", "aid", "ail", "ake", "alt", "ame", "amm", "and", "any", "ard", "are", "arm", "asy", "atc", "ate", "ath", "ave", "awn", "ble", "blo", "bou", "bro", "cha", "che", "ckp", "cli", "con", "coo", "cor", "cus", "ddn", "dea", "dis", "dmi", "dne", "dra", "dru", "dum", "eac", "eal", "eam", "eat", "eck", "ect", "edr", "eir", "ele", "ell", "ena", "end", "eop", "epi", "epo", "erf", "erv", "est", "fai", "fec", "fla", "fly", "for", "fri", "fro", "ful", "gam", "ggw", "glh", "gre", "gun", "guy", "gwp", "hah", "ham", "han", "har", "has", "hav", "hea", "hec", "hei", "hel", "hem", "her", "hey", "hic", "him", "his", "hoo", "hot", "how", "ice", "ich", "ike", "ing", "inj", "ion", "ist", "ith"
};

// v6.21: v2-only token dictionary rebuilt around frequent whole words — Russian
// chat words first, then English / client terms, then productive Russian n-grams
// for the words that are not listed. The encoder always takes the longest match,
// so a whole word costs 9 bits instead of ~7 bits per character. The bit layout
// is the same as v1; only the token table differs, and v2 frames never carry v1
// data, so v1 compatibility is untouched.
const std::string DICTIONARY_V2[256] = {
	"это", "что", "как", "так", "все", "для", "нет", "да", "не", "ты", "я", "он", "она", "они", "мы", "вы", "мне", "тебя", "меня", "его", "её", "их", "нас", "вас", "был", "была", "было",
	"были", "быть", "есть", "уже", "ещё", "еще", "или", "но", "на", "по", "за", "из", "от", "до", "при", "про", "без", "под", "над", "через", "после", "перед", "между", "когда", "где", "кто",
	"почему", "потому", "чтобы", "если", "тоже", "также", "очень", "можно", "нужно", "надо", "будет", "буду", "можешь", "может", "сообщение", "сообщения", "отправлено", "отправил",
	"отправить", "получил", "получить", "привет", "пока", "спасибо", "пожалуйста", "извини", "хорошо", "плохо", "нормально", "давай", "сейчас", "потом", "тут", "там", "здесь", "вообще",
	"просто", "точно", "ясно", "понял", "понятно", "короче", "типа", "ладно", "окей", "себе", "себя", "свой", "свои", "этот", "эта", "эти", "тот", "та", "те", "весь", "вся", "всё", "всех",
	"всем", "него", "неё", "них", "нам", "вам", "тебе", "думаю", "знаю", "будем", "the", "and", "you", "that", "for", "with", "what", "this", "have", "not", "are", "was", "were", "will",
	"your", "from", "they", "there", "their", "hello", "hi", "hey", "sorry", "thanks", "please", "okay", "yes", "good", "bad", "nice", "kinetix", "Kinetix", "ddnet", "DDNet", "dummy",
	"dummies", "hook", "tee", "map", "team", "server", "client", "friend", "test", "ост", "ени", "ние", "ств", "ость", "ать", "ить", "еть", "ять", "ой", "ая", "ый", "ие", "ых", "ом", "ам",
	"ем", "ет", "ит", "ат", "ут", "ят", "ил", "ал", "ел", "ол", "ул", "ан", "ин", "ен", "ор", "ар", "ер", "ир", "ур", "ка", "ко", "ку", "ки", "ке", "го", "гу", "ду", "ди", "де", "ла", "ло",
	"лу", "ли", "ле", "ма", "мо", "му", "ми", "ме", "ну", "ни", "па", "пу", "пи", "пе", "ра", "ро", "ру", "ри", "ре", "са", "со", "су", "си", "се", "то", "ту", "ти", "ва", "ве", "ви", "во",
	"зо", "ча", "че", "чи", "ша", "ше", "ши", "ще", "ьно", "тья", "ье", "ья"
};

namespace
{
	unsigned Chat2Crc32(const unsigned char *pData, size_t Size)
	{
		return (unsigned)::crc32(0, pData, (uInt)Size);
	}

	void Chat2PutBe32(std::vector<unsigned char> &vOut, unsigned Value)
	{
		vOut.push_back((Value >> 24) & 0xff);
		vOut.push_back((Value >> 16) & 0xff);
		vOut.push_back((Value >> 8) & 0xff);
		vOut.push_back(Value & 0xff);
	}

	unsigned Chat2ReadBe32(const unsigned char *pData)
	{
		return ((unsigned)pData[0] << 24) | ((unsigned)pData[1] << 16) | ((unsigned)pData[2] << 8) | pData[3];
	}

	bool Chat2Deflate(const std::string &Text, std::vector<unsigned char> &vOut)
	{
		z_stream Stream{};
		if(deflateInit2(&Stream, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			return false;
		vOut.resize(256);
		Stream.next_in = (Bytef *)Text.data();
		Stream.avail_in = (uInt)Text.size();
		Stream.next_out = vOut.data();
		Stream.avail_out = (uInt)vOut.size();
		const int Result = deflate(&Stream, Z_FINISH);
		const bool Ok = Result == Z_STREAM_END && Stream.avail_in == 0 && Stream.total_out <= 255;
		if(Ok)
			vOut.resize(Stream.total_out);
		else
			vOut.clear();
		deflateEnd(&Stream);
		return Ok;
	}

	const std::vector<int> &Chat2UnicodeAlphabet()
	{
		static const std::vector<int> s_vAlphabet = [] {
			std::vector<int> v;
			v.reserve(0x110000);
			for(int C = 0; C <= 0x10ffff; ++C)
			{
				const bool SpaceOrInvisible = C <= 0x20 || (C >= 0x7f && C <= 0xa0) ||
					(C >= 0xfdd0 && C <= 0xfdef) || (C & 0xffff) == 0xfffe || (C & 0xffff) == 0xffff ||
					C == 0x00ad || C == 0x034f || C == 0x061c ||
					(C >= 0x115f && C <= 0x1160) || (C >= 0x17b4 && C <= 0x17b5) ||
					(C >= 0x1680 && C <= 0x1680) || (C >= 0x180b && C <= 0x180f) ||
					(C >= 0x0300 && C <= 0x036f) || (C >= 0x0483 && C <= 0x0489) ||
					(C >= 0x0591 && C <= 0x05bd) || C == 0x05bf || (C >= 0x05c1 && C <= 0x05c2) ||
					(C >= 0x0610 && C <= 0x061a) || (C >= 0x064b && C <= 0x065f) ||
					(C >= 0x0670 && C <= 0x0670) || (C >= 0x06d6 && C <= 0x06ed) ||
					(C >= 0x0900 && C <= 0x0903) || (C >= 0x093a && C <= 0x094f) ||
					(C >= 0x1ab0 && C <= 0x1aff) || (C >= 0x1dc0 && C <= 0x1dff) ||
					(C >= 0x20d0 && C <= 0x20ff) || (C >= 0xfe20 && C <= 0xfe2f) ||
					(C >= 0x2000 && C <= 0x200f) || (C >= 0x2028 && C <= 0x202f) ||
					(C >= 0x205f && C <= 0x206f) || C == 0x2800 || C == 0x3000 || C == 0x3164 ||
					(C >= 0xfe00 && C <= 0xfe0f) || C == 0xfeff || C == 0xffa0 ||
					(C >= 0xfff9 && C <= 0xfffb) || (C >= 0xd800 && C <= 0xdfff) ||
					(C >= 0x1bca0 && C <= 0x1bca3) || (C >= 0x1d173 && C <= 0x1d17a) ||
					(C >= 0xe0000 && C <= 0xe0fff);
				if(!SpaceOrInvisible)
					v.push_back(C);
			}
			return v;
		}();
		return s_vAlphabet;
	}

	void Chat2AppendUtf8(std::string &Out, int Codepoint)
	{
		char aUtf8[4];
		const int Length = str_utf8_encode(aUtf8, Codepoint);
		Out.append(aUtf8, Length);
	}

	bool Chat2EncodeUnicode(const std::vector<unsigned char> &vData, std::string &Out)
	{
		if(vData.empty() || vData.size() > 255)
			return false;
		const std::vector<int> &vAlphabet = Chat2UnicodeAlphabet();
		const uint64_t Base = vAlphabet.size();
		std::vector<unsigned char> vPadded;
		vPadded.reserve(vData.size() + 4);
		vPadded.push_back((unsigned char)vData.size());
		vPadded.insert(vPadded.end(), vData.begin(), vData.end());
		while(vPadded.size() % 4)
			vPadded.push_back(0);
		Out.clear();
		for(size_t i = 0; i < vPadded.size(); i += 4)
		{
			uint64_t Value = ((uint64_t)vPadded[i] << 24) | ((uint64_t)vPadded[i + 1] << 16) |
				((uint64_t)vPadded[i + 2] << 8) | vPadded[i + 3];
			if(Value >= Base * Base)
				return false;
			Chat2AppendUtf8(Out, vAlphabet[Value / Base]);
			Chat2AppendUtf8(Out, vAlphabet[Value % Base]);
		}
		return true;
	}

	bool Chat2DecodeUnicode(const std::string &Text, std::vector<unsigned char> &vData)
	{
		if(Text.empty() || Text.size() > 2048 || !str_utf8_check(Text.c_str()))
			return false;
		const std::vector<int> &vAlphabet = Chat2UnicodeAlphabet();
		const uint64_t Base = vAlphabet.size();
		std::vector<unsigned char> vDecoded;
		size_t Pos = 0;
		while(Pos < Text.size())
		{
			const char *pHigh = Text.c_str() + Pos;
			const int High = str_utf8_decode(&pHigh);
			if(High < 0 || pHigh == Text.c_str() + Pos)
				return false;
			Pos = pHigh - Text.c_str();
			const char *pLow = Text.c_str() + Pos;
			const int Low = str_utf8_decode(&pLow);
			if(Low < 0 || pLow == Text.c_str() + Pos)
				return false;
			Pos = pLow - Text.c_str();
			auto HighIt = std::lower_bound(vAlphabet.begin(), vAlphabet.end(), High);
			auto LowIt = std::lower_bound(vAlphabet.begin(), vAlphabet.end(), Low);
			if(HighIt == vAlphabet.end() || *HighIt != High || LowIt == vAlphabet.end() || *LowIt != Low)
				return false;
			const uint64_t Value = (uint64_t)(HighIt - vAlphabet.begin()) * Base + (LowIt - vAlphabet.begin());
			if(Value > 0xffffffffULL)
				return false;
			vDecoded.push_back((Value >> 24) & 0xff);
			vDecoded.push_back((Value >> 16) & 0xff);
			vDecoded.push_back((Value >> 8) & 0xff);
			vDecoded.push_back(Value & 0xff);
		}
		if(vDecoded.empty() || vDecoded[0] == 0 || vDecoded[0] > vDecoded.size() - 1)
			return false;
		const size_t Length = vDecoded[0];
		for(size_t i = Length + 1; i < vDecoded.size(); ++i)
			if(vDecoded[i] != 0)
				return false;
		vData.assign(vDecoded.begin() + 1, vDecoded.begin() + 1 + Length);
		return true;
	}

	const unsigned char CHAT2_ZW[][3] = {
		{0xcd, 0x8f, 0}, // U+034F
		{0xe2, 0x81, 0xaa}, {0xe2, 0x81, 0xab}, {0xe2, 0x81, 0xac},
		{0xe2, 0x81, 0xad}, {0xe2, 0x81, 0xae}, {0xe2, 0x81, 0xaf},
		{0xef, 0xb8, 0x80}, {0xef, 0xb8, 0x81}, {0xef, 0xb8, 0x82},
		{0xef, 0xb8, 0x83}, {0xef, 0xb8, 0x84}, {0xef, 0xb8, 0x85},
		{0xef, 0xb8, 0x86}, {0xef, 0xb8, 0x87}, {0xef, 0xb8, 0x88},
		{0xef, 0xb8, 0x89}, {0xef, 0xb8, 0x8a}, {0xef, 0xb8, 0x8b},
		{0xef, 0xb8, 0x8c}, {0xef, 0xb8, 0x8d}, {0xef, 0xb8, 0x8e},
		{0xef, 0xb8, 0x8f}, {0xef, 0xbf, 0xba},
	};

	constexpr unsigned char CHAT2_HOMOGLYPH_MAGIC = 0xc2;
	// Hybrid block: magic, frame byte count, carrier bit count (BE16), then
	// the remaining frame bits packed MSB-first. Carrier bits occupy existing
	// Cyrillic lookalikes and decode back to their original codepoints.
	struct SChat2Homoglyph
	{
		int m_Source;
		int m_Alternate;
	};
	constexpr SChat2Homoglyph CHAT2_HOMOGLYPHS[] = {
		{0x0410, 0x0391}, {0x0412, 0x0392}, {0x0415, 0x0395}, {0x041a, 0x039a},
		{0x041c, 0x039c}, {0x041d, 0x0397}, {0x041e, 0x039f}, {0x0420, 0x03a1},
		{0x0422, 0x03a4}, {0x0425, 0x03a7}, {0x0430, 0x03b1}, {0x0435, 0x03b5},
		{0x043e, 0x03bf}, {0x0440, 0x03c1}, {0x0441, 0x03f2}, {0x0445, 0x03c7},
	};

	int Chat2HomoglyphSource(int Codepoint)
	{
		for(size_t i = 0; i < sizeof(CHAT2_HOMOGLYPHS) / sizeof(CHAT2_HOMOGLYPHS[0]); ++i)
			if(CHAT2_HOMOGLYPHS[i].m_Source == Codepoint)
				return (int)i;
		return -1;
	}

	int Chat2HomoglyphAlternate(int Codepoint)
	{
		for(size_t i = 0; i < sizeof(CHAT2_HOMOGLYPHS) / sizeof(CHAT2_HOMOGLYPHS[0]); ++i)
			if(CHAT2_HOMOGLYPHS[i].m_Alternate == Codepoint)
				return (int)i;
		return -1;
	}

	bool Chat2PackInvisible(const std::vector<unsigned char> &vData, std::string &Out)
	{
		if(vData.empty() || vData.size() > 255)
			return false;
		Out = "\xef\xbf\xb9";
		for(unsigned char Byte : vData)
		{
			const int aDigits[2] = {Byte / 24, Byte % 24};
			for(int Digit : aDigits)
			{
				const int Length = Digit == 0 ? 2 : 3;
				Out.append((const char *)CHAT2_ZW[Digit], Length);
			}
		}
		Out += "\xef\xbf\xbb";
		return true;
	}

	bool Chat2UnpackInvisible(const std::string &Text, std::vector<unsigned char> &vData)
	{
		if(Text.size() < 9 || Text.compare(0, 3, "\xef\xbf\xb9") != 0 || Text.compare(Text.size() - 3, 3, "\xef\xbf\xbb") != 0)
			return false;
		vData.clear();
		size_t Pos = 3;
		while(Pos < Text.size() - 3)
		{
			int aDigits[2] = {-1, -1};
			for(int d = 0; d < 2; ++d)
			{
				for(int i = 0; i < 24; ++i)
				{
					const int Length = i == 0 ? 2 : 3;
					if(Pos + Length <= Text.size() - 3 && memcmp(Text.data() + Pos, CHAT2_ZW[i], Length) == 0)
					{
						aDigits[d] = i;
						Pos += Length;
						break;
					}
				}
				if(aDigits[d] < 0)
					return false;
			}
			const int Value = aDigits[0] * 24 + aDigits[1];
			if(Value > 255)
				return false;
			vData.push_back((unsigned char)Value);
		}
		return !vData.empty();
	}

	int Chat2SafeInsertPosition(const std::string &Text);

	bool Chat2PackHomoglyphHybrid(const std::vector<unsigned char> &vFrame, const std::string &Visible, std::string &Out)
	{
		if(vFrame.empty() || vFrame.size() > 255 || !str_utf8_check(Visible.c_str()))
			return false;

		const unsigned TotalBits = (unsigned)vFrame.size() * 8;
		unsigned CarrierBits = 0;
		for(size_t Pos = 0; Pos < Visible.size();)
		{
			const char *pChar = Visible.c_str() + Pos;
			const int Codepoint = str_utf8_decode(&pChar);
			if(Codepoint < 0 || pChar <= Visible.c_str() + Pos)
				return false;
			// These codepoints mark 1-bits on receive; reject source text that
			// already contains one so carrier positions remain unambiguous.
			if(Chat2HomoglyphAlternate(Codepoint) >= 0)
				return false;
			if(Chat2HomoglyphSource(Codepoint) >= 0 && CarrierBits < TotalBits)
				++CarrierBits;
			Pos = pChar - Visible.c_str();
		}
		if(CarrierBits == 0)
			return false;

		const unsigned RemainingBits = TotalBits - CarrierBits;
		std::vector<unsigned char> vHybrid = {
			CHAT2_HOMOGLYPH_MAGIC,
			(unsigned char)vFrame.size(),
			(unsigned char)(CarrierBits >> 8),
			(unsigned char)CarrierBits,
		};
		std::vector<unsigned char> vTail((RemainingBits + 7) / 8, 0);
		for(unsigned i = 0; i < RemainingBits; ++i)
		{
			const unsigned SourceBit = CarrierBits + i;
			const unsigned Bit = (vFrame[SourceBit / 8] >> (7 - SourceBit % 8)) & 1;
			vTail[i / 8] |= (unsigned char)(Bit << (7 - i % 8));
		}
		vHybrid.insert(vHybrid.end(), vTail.begin(), vTail.end());
		std::string Hidden;
		if(!Chat2PackInvisible(vHybrid, Hidden))
			return false;

		std::string Transformed;
		unsigned CarrierIndex = 0;
		for(size_t Pos = 0; Pos < Visible.size();)
		{
			const char *pChar = Visible.c_str() + Pos;
			const int Codepoint = str_utf8_decode(&pChar);
			if(Codepoint < 0 || pChar <= Visible.c_str() + Pos)
				return false;
			const size_t ByteLength = pChar - (Visible.c_str() + Pos);
			const int SourceIndex = Chat2HomoglyphSource(Codepoint);
			if(SourceIndex >= 0 && CarrierIndex < CarrierBits)
			{
				const unsigned Bit = (vFrame[CarrierIndex / 8] >> (7 - CarrierIndex % 8)) & 1;
				++CarrierIndex;
				Chat2AppendUtf8(Transformed, Bit ? CHAT2_HOMOGLYPHS[SourceIndex].m_Alternate : Codepoint);
			}
			else
				Transformed.append(Visible, Pos, ByteLength);
			Pos += ByteLength;
		}

		const int InsertAt = Chat2SafeInsertPosition(Transformed);
		if(InsertAt < 0)
			return false;
		Out.assign(Transformed, 0, InsertAt);
		Out += Hidden;
		Out.append(Transformed, InsertAt, std::string::npos);
		return true;
	}

	bool Chat2UnpackHomoglyphHybrid(const std::vector<unsigned char> &vHybrid, const std::string &Text, size_t HiddenBegin, size_t HiddenEnd, std::vector<unsigned char> &vFrame, std::string &RestoredText)
	{
		if(vHybrid.size() < 4 || vHybrid[0] != CHAT2_HOMOGLYPH_MAGIC || vHybrid[1] == 0 ||
			HiddenBegin > HiddenEnd || HiddenEnd > Text.size())
			return false;
		const unsigned FrameBytes = vHybrid[1];
		const unsigned TotalBits = FrameBytes * 8;
		const unsigned CarrierBits = ((unsigned)vHybrid[2] << 8) | vHybrid[3];
		if(CarrierBits == 0 || CarrierBits > TotalBits)
			return false;
		const unsigned RemainingBits = TotalBits - CarrierBits;
		const size_t TailBytes = (RemainingBits + 7) / 8;
		if(vHybrid.size() != 4 + TailBytes)
			return false;
		if(RemainingBits % 8 && TailBytes &&
			(vHybrid.back() & ((1u << (8 - RemainingBits % 8)) - 1)) != 0)
			return false;

		std::string Body = Text;
		Body.erase(HiddenBegin, HiddenEnd - HiddenBegin);
		std::vector<unsigned char> vBits;
		vBits.reserve(TotalBits);
		RestoredText.clear();
		unsigned SeenCarriers = 0;
		for(size_t Pos = 0; Pos < Body.size();)
		{
			const char *pChar = Body.c_str() + Pos;
			const int Codepoint = str_utf8_decode(&pChar);
			if(Codepoint < 0 || pChar <= Body.c_str() + Pos)
				return false;
			const size_t ByteLength = pChar - (Body.c_str() + Pos);
			const int SourceIndex = Chat2HomoglyphSource(Codepoint);
			const int AlternateIndex = Chat2HomoglyphAlternate(Codepoint);
			if(SeenCarriers < CarrierBits && (SourceIndex >= 0 || AlternateIndex >= 0))
			{
				const int PairIndex = SourceIndex >= 0 ? SourceIndex : AlternateIndex;
				vBits.push_back(AlternateIndex >= 0 ? 1 : 0);
				++SeenCarriers;
				Chat2AppendUtf8(RestoredText, CHAT2_HOMOGLYPHS[PairIndex].m_Source);
			}
			else
				RestoredText.append(Body, Pos, ByteLength);
			Pos += ByteLength;
		}
		if(SeenCarriers != CarrierBits)
			return false;
		for(unsigned i = 0; i < RemainingBits; ++i)
			vBits.push_back((vHybrid[4 + i / 8] >> (7 - i % 8)) & 1);
		vFrame.assign(FrameBytes, 0);
		for(unsigned i = 0; i < TotalBits; ++i)
			vFrame[i / 8] |= (unsigned char)(vBits[i] << (7 - i % 8));
		return true;
	}

	int Chat2SafeInsertPosition(const std::string &Text)
	{
		// Prefer inserting at an existing word boundary. There must be visible,
		// non-whitespace text on both sides so 0.7's two-sided trimming cannot
		// remove the payload.
		std::vector<std::pair<size_t, int>> vCodepoints;
		for(size_t Pos = 0; Pos < Text.size();)
		{
			const char *pChar = Text.c_str() + Pos;
			const int Codepoint = str_utf8_decode(&pChar);
			if(Codepoint < 0 || pChar <= Text.c_str() + Pos)
				return -1;
			vCodepoints.emplace_back(Pos, Codepoint);
			Pos = pChar - Text.c_str();
		}
		auto IsWord = [](int Codepoint) {
			return !str_utf8_isspace(Codepoint);
		};
		for(size_t i = 1; i < vCodepoints.size(); ++i)
		{
			const int Previous = vCodepoints[i - 1].second;
			const int Current = vCodepoints[i].second;
			if(IsWord(Previous) && IsWord(Current))
				return (int)vCodepoints[i].first;
		}
		return -1;
	}

	bool Chat2EncodeNumSeq(const std::string &Text, std::vector<unsigned char> &vOut)
	{
		std::vector<int> vValues;
		size_t Pos = 0;
		while(Pos < Text.size())
		{
			const size_t Start = Pos;
			bool Negative = false;
			if(Text[Pos] == '-')
			{
				Negative = true;
				++Pos;
			}
			if(Pos >= Text.size() || Text[Pos] < '0' || Text[Pos] > '9' ||
				(Text[Pos] == '0' && Pos + 1 < Text.size() && Text[Pos + 1] >= '0' && Text[Pos + 1] <= '9'))
				return false;
			int64_t Value = 0;
			while(Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9')
			{
				Value = Value * 10 + Text[Pos++] - '0';
				if(Value > (int64_t)INT32_MAX + (Negative ? 1 : 0))
					return false;
			}
			if(Negative)
				Value = -Value;
			if(Negative && Value == 0)
				return false;
			vValues.push_back((int)Value);
			if(Pos == Text.size())
				break;
			if(Text[Pos++] != ' ' || Pos == Text.size() || Text[Pos] == ' ')
				return false;
			if(Start == Pos)
				return false;
		}
		if(vValues.size() < 3 || vValues.size() > 255)
			return false;
		const int64_t Step = (int64_t)vValues[1] - vValues[0];
		for(size_t i = 2; i < vValues.size(); ++i)
			if((int64_t)vValues[i] - vValues[i - 1] != Step)
				return false;
		auto Put = [&vOut](uint32_t Value) {
			do
			{
				unsigned char Byte = Value & 0x7f;
				Value >>= 7;
				if(Value)
					Byte |= 0x80;
				vOut.push_back(Byte);
			} while(Value);
		};
		auto ZigZag = [](int64_t Value) -> uint32_t {
			return Value < 0 ? (uint32_t)(-Value * 2 - 1) : (uint32_t)(Value * 2);
		};
		vOut.clear();
		vOut.push_back((unsigned char)vValues.size());
		Put(ZigZag(vValues[0]));
		if(Step != 1)
			Put(ZigZag(Step));
		return vOut.size() < Text.size();
	}

	bool Chat2DecodeData(int Codec, const std::vector<unsigned char> &vData, std::string &Text)
	{
		Text.clear();
		if(vData.empty() || vData.size() > 255)
			return false;
		if(Codec == 0)
			Text.assign(vData.begin(), vData.end());
		else if(Codec == 1)
		{
			z_stream Stream{};
			if(inflateInit2(&Stream, -15) != Z_OK)
				return false;
			Text.resize(256);
			Stream.next_in = (Bytef *)vData.data();
			Stream.avail_in = (uInt)vData.size();
			Stream.next_out = (Bytef *)Text.data();
			Stream.avail_out = (uInt)Text.size();
			const int Result = inflate(&Stream, Z_FINISH);
			const bool Ok = Result == Z_STREAM_END && Stream.avail_in == 0 && Stream.total_out > 0 && Stream.total_out <= 255;
			if(Ok)
				Text.resize(Stream.total_out);
			else
				Text.clear();
			inflateEnd(&Stream);
			if(!Ok)
				return false;
		}
		else if(Codec == 4)
		{
			static constexpr char s_aDigits[] = "0123456789 +-.,"; // 15 symbols
			for(size_t i = 0; i < vData.size(); ++i)
			{
				const int Hi = vData[i] >> 4;
				const int Lo = vData[i] & 0xf;
				if(Hi >= 15 || (Lo >= 15 && i + 1 != vData.size()))
					return false;
				Text.push_back(s_aDigits[Hi]);
				if(Lo < 15)
					Text.push_back(s_aDigits[Lo]);
			}
		}
		else
			return false;

		if(Text.empty() || Text.size() > 255 || !str_utf8_check(Text.c_str()))
			return false;
		for(unsigned char c : Text)
			if(c < 0x20 || c == 0x7f)
				return false;
		return true;
	}
}

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
	m_LogoTexture = Graphics()->LoadTexture("kinetix_icon.png", IStorage::TYPE_ALL);
	OnReset();
}

void CIRC::OnReset()
{
	for(int d = 0; d < MAX_DUMMIES; ++d)
	{
		m_aSendQueue[d].clear();
		m_aNextEmoteSendTick[d] = 0;
		m_aHandshakeSent[d] = false;
		for(int c = 0; c < 3; ++c)
			m_aV2Bits[d][c].clear();
		m_aV2EmoteNextTick[d] = 0;
		m_aV2AimNextTick[d] = 0;
		m_aV2BeaconTick[d] = 0;
		m_aV2AimTargetX[d] = 0;
		m_aV2AimTargetY[d] = 0;
		m_aV2AimActive[d] = false;
		m_aV2ChatQueue[d].clear();
		m_aV2Chat2Queue[d].clear();
	}
	m_LastRevealJoin = g_Config.m_KxIrcEnabled != 0;
	m_V2SuppressEncode = false;
	m_vFinishTiles.clear();
	m_FinishTilesBuilt = false;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		m_aEmoteHistory[i].clear();
		m_aDetectedClient[i][0] = '\0';
		m_aMsgBuffer[i].clear();
		m_aLastEmoteTime[i] = 0;
		m_aV2RxEmote[i].clear();
		m_aV2RxAim[i].clear();
		m_aV2RxEmoteTime[i] = 0;
		m_aV2RxAimBucket[i] = -1;
		m_aV2RxAimTick[i] = -1;
		m_aV2Detected[i] = false;
		m_aV2Asm[i].m_Active = false;
		m_aV2Asm[i].m_Total = 0;
		m_aV2Asm[i].m_Next = 0;
		m_aV2Asm[i].m_LastTime = 0;
		m_aV2Asm[i].m_vNibbles.clear();
		m_aV2Chat2Asm[i] = SV2Chat2Assembly{};
	}
}

void CIRC::RenderLogo(float CenterX, float CenterY, float Size, float Alpha) const
{
	if(!m_LogoTexture.IsValid() || Size <= 0.0f)
		return;
	Graphics()->BlendNormal();
	Graphics()->TextureSet(m_LogoTexture);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
	IGraphics::CQuadItem Quad(CenterX - Size / 2.0f, CenterY - Size / 2.0f, Size, Size);
	Graphics()->QuadsDrawTL(&Quad, 1);
	Graphics()->QuadsEnd();
	Graphics()->TextureClear();
}

const char *CIRC::DecodeChatSuffix(const char *pMessage, char *pOut, int OutSize)
{
	if(!pOut || OutSize <= 0)
		return pOut;
	pOut[0] = '\0';
	if(!pMessage || !pMessage[0])
		return pOut;

	const std::vector<unsigned char> vBits = ZwToBits(pMessage);
	if(vBits.empty())
		return pOut;
	std::vector<SFrame> vFrames;
	DecodeFrames(vBits, MAGIC_TEXT, MAGIC_TEXT_BITS, vFrames, nullptr);
	if(vFrames.empty())
		return pOut;

	char aVisible[MAX_CHAT_LENGTH];
	KxIrcStego::VisibleText(pMessage, aVisible, sizeof(aVisible));
	std::string Suffix;
	for(const SFrame &Frame : vFrames)
	{
		// v6.21: fragments are reassembled and echoed separately; only complete
		// single-frame messages belong to this line.
		if(Frame.m_Identity || Frame.m_Frag || Frame.m_vNibbles.empty())
			continue;
		const std::string Text = DecodeEmotesToString(Frame.m_vNibbles, DICTIONARY_V2);
		if(Text.empty() || str_comp(Text.c_str(), aVisible) == 0)
			continue;
		Suffix += " (";
		Suffix += Text;
		Suffix += ')';
	}
	str_copy(pOut, Suffix.c_str(), OutSize);
	return pOut;
}

void CIRC::BuildFinishTiles()
{
	m_vFinishTiles.clear();
	const CCollision *pCollision = GameClient()->Collision();
	if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0 || !pCollision->GameLayer())
	{
		m_FinishTilesBuilt = true;
		return;
	}
	for(int y = 0; y < pCollision->GetHeight(); ++y)
	{
		for(int x = 0; x < pCollision->GetWidth(); ++x)
		{
			const int Index = y * pCollision->GetWidth() + x;
			if(pCollision->GameLayer()[Index].m_Index == TILE_FINISH ||
				(pCollision->FrontLayer() && pCollision->FrontLayer()[Index].m_Index == TILE_FINISH))
				m_vFinishTiles.emplace_back(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
		}
	}
	m_FinishTilesBuilt = true;
}

bool CIRC::NearFinish(int Dummy) const
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES || Client()->State() != IClient::STATE_ONLINE)
		return false;
	CIRC *pSelf = const_cast<CIRC *>(this);
	if(!m_FinishTilesBuilt)
		pSelf->BuildFinishTiles();
	if(m_vFinishTiles.empty())
		return false;
	const int ClientId = GameClient()->m_aLocalIds[Dummy];
	if(ClientId < 0)
		return false;
	const CNetObj_Character *pCharacter = (const CNetObj_Character *)Client()->SnapFindItemConn(
		Dummy, IClient::SNAP_CURRENT, NETOBJTYPE_CHARACTER, ClientId);
	if(!pCharacter)
		return false;
	const vec2 Pos((float)pCharacter->m_X, (float)pCharacter->m_Y);
	constexpr float Radius = 50.0f * 32.0f;
	for(const vec2 &Finish : m_vFinishTiles)
		if(distance(Pos, Finish) <= Radius)
			return true;
	return false;
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
	if(g_Config.m_KxIrcVersion == 1)
	{
		GameClient()->m_Chat.AddHistoryEntry(pText, -1);
		V2QueueMessage(g_Config.m_ClDummy, pText);
		char aBuf[300];
		str_format(aBuf, sizeof(aBuf), "[IRC] -> %s", pText);
		GameClient()->m_Chat.Echo(aBuf);
		return;
	}

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

	if(g_Config.m_KxIrcVersion == 1)
		return m_aV2Detected[ClientId] ? "Kinetix" : nullptr;

	// Check all local dummies, not just the active one
	if(IsOwnClient(ClientId))
	{
		return g_Config.m_KxIrcRevealJoin ? LOCAL_CLIENT_NAME : nullptr;
	}

	if(m_aDetectedClient[ClientId][0] != '\0')
		return m_aDetectedClient[ClientId];

	return nullptr;
}

bool CIRC::IsKinetixClient(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	return m_aV2Detected[ClientId] || m_aDetectedClient[ClientId][0] != '\0';
}

void CIRC::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	// The desired IRC watermark changes when IRC is toggled or a tee enters
	// the finish area. Re-arm the regular info check when the snapshot differs.
	if(!GameClient()->Client()->IsSixup())
	{
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			if((d != 0 && !Client()->DummyConnected(d)) || GameClient()->m_aLocalIds[d] < 0)
				continue;
			char aName[MAX_NAME_LENGTH];
			char aClan[MAX_CLAN_LENGTH];
			const char *pName = EffectiveName(d, aName, sizeof(aName));
			const char *pClan = EffectiveClan(d, aClan, sizeof(aClan));
			const auto &Info = GameClient()->m_aClients[GameClient()->m_aLocalIds[d]];
			if(str_comp(Info.m_aName, pName) || str_comp(Info.m_aClan, pClan))
				GameClient()->RequestInfoUpdate(d);
		}
	}

	if(g_Config.m_KxIrcVersion == 1)
	{
		if(g_Config.m_KxIrcEnabled)
			UpdateV2();
		else
		{
			for(int d = 0; d < MAX_DUMMIES; ++d)
			{
				for(int c = 0; c < 3; ++c)
					m_aV2Bits[d][c].clear();
				m_aV2ChatQueue[d].clear();
				m_aV2Chat2Queue[d].clear();
				m_aV2AimActive[d] = false;
			}
		}
		return;
	}

	for(int d = 0; d < MAX_DUMMIES; ++d)
		m_aV2Chat2Queue[d].clear();

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

	if(g_Config.m_KxIrcVersion == 1)
	{
		if(MsgId == NETMSGTYPE_SV_EMOTICON)
		{
			CNetMsg_Sv_Emoticon *pMsg = (CNetMsg_Sv_Emoticon *)pRawMsg;
			if(pMsg->m_ClientId >= 0 && pMsg->m_ClientId < MAX_CLIENTS && !IsOwnClient(pMsg->m_ClientId))
				OnV2Emote(pMsg->m_ClientId, pMsg->m_Emoticon);
		}
		else if(MsgId == NETMSGTYPE_SV_CHAT)
		{
			CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
			if(pMsg->m_ClientId >= 0 && pMsg->m_ClientId < MAX_CLIENTS && !IsOwnClient(pMsg->m_ClientId))
				OnV2Chat(pMsg->m_ClientId, pMsg->m_pMessage);
		}
		return;
	}

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
		// v6.20: the map may have changed since the tiles were cached.
		m_vFinishTiles.clear();
		m_FinishTilesBuilt = false;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			m_aEmoteHistory[i].clear();
			m_aDetectedClient[i][0] = '\0';
			m_aMsgBuffer[i].clear();
			m_aLastEmoteTime[i] = 0;
			m_aV2RxEmote[i].clear();
			m_aV2RxAim[i].clear();
			m_aV2RxEmoteTime[i] = 0;
			m_aV2RxAimBucket[i] = -1;
			m_aV2RxAimTick[i] = -1;
			m_aV2Detected[i] = false;
			m_aV2Asm[i].m_Active = false;
			m_aV2Asm[i].m_vNibbles.clear();
			m_aV2Chat2Asm[i] = SV2Chat2Assembly{};
		}

		// Reset per-dummy state
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			m_aSendQueue[d].clear();
			m_aHandshakeSent[d] = false;
			m_aNextEmoteSendTick[d] = 0;
			for(int c = 0; c < 3; ++c)
				m_aV2Bits[d][c].clear();
			m_aV2EmoteNextTick[d] = 0;
			m_aV2AimNextTick[d] = 0;
			m_aV2BeaconTick[d] = 0;
			m_aV2AimTargetX[d] = 0;
			m_aV2AimTargetY[d] = 0;
			m_aV2AimActive[d] = false;
			m_aV2ChatQueue[d].clear();
			m_aV2Chat2Queue[d].clear();
		}

		// v6.19: announce ourselves right after joining.
		if(g_Config.m_KxIrcEnabled && g_Config.m_KxIrcVersion == 1 && g_Config.m_KxIrcRevealMethod != 0)
		{
			for(int d = 0; d < MAX_DUMMIES; ++d)
				V2QueueIdentity(d);
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
			m_aV2RxEmote[i].clear();
			m_aV2RxAim[i].clear();
			m_aV2RxEmoteTime[i] = 0;
			m_aV2RxAimBucket[i] = -1;
			m_aV2RxAimTick[i] = -1;
			m_aV2Detected[i] = false;
			m_aV2Asm[i].m_Active = false;
			m_aV2Asm[i].m_vNibbles.clear();
			m_aV2Chat2Asm[i] = SV2Chat2Assembly{};
		}
		for(int d = 0; d < MAX_DUMMIES; ++d)
		{
			m_aSendQueue[d].clear();
			m_aHandshakeSent[d] = false;
			m_aNextEmoteSendTick[d] = 0;
			for(int c = 0; c < 3; ++c)
				m_aV2Bits[d][c].clear();
			m_aV2EmoteNextTick[d] = 0;
			m_aV2AimNextTick[d] = 0;
			m_aV2BeaconTick[d] = 0;
			m_aV2AimTargetX[d] = 0;
			m_aV2AimTargetY[d] = 0;
			m_aV2AimActive[d] = false;
			m_aV2ChatQueue[d].clear();
			m_aV2Chat2Queue[d].clear();
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
std::vector<int> CIRC::EncodeStringToEmotes(const std::string& text, const std::string *pDictionary)
{
	const std::string *pDict = pDictionary ? pDictionary : DICTIONARY;
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
			const std::string& token = pDict[idx];
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
std::string CIRC::DecodeEmotesToString(const std::vector<int>& emotes, const std::string *pDictionary)
{
	const std::string *pDict = pDictionary ? pDictionary : DICTIONARY;
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
			result += pDict[idx];
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

// ============================================================================
// IRC v2 — send / receive plumbing
// ============================================================================

static const int KX_V2_CH_EMOTE = 1;
static const int KX_V2_CH_AIM = 2;

void CIRC::V2Mark(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	m_aV2Detected[ClientId] = true;
	if(m_aDetectedClient[ClientId][0] == '\0')
		str_copy(m_aDetectedClient[ClientId], "Kinetix", sizeof(m_aDetectedClient[ClientId]));
}

void CIRC::V2QueueBits(int Dummy, int Channel, const std::vector<unsigned char> &vBits)
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES || Channel < 0 || Channel > 2)
		return;
	std::vector<unsigned char> &Queue = m_aV2Bits[Dummy][Channel];
	Queue.insert(Queue.end(), vBits.begin(), vBits.end());
	if(Queue.size() > 8192)
		Queue.erase(Queue.begin(), Queue.begin() + (Queue.size() - 8192));
}

void CIRC::V2QueueMessage(int Dummy, const char *pText)
{
	if(!pText || pText[0] == '\0')
		return;
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return;
	const int Method = g_Config.m_KxIrcEncodeMethod;
	if(Method == 0)
		return;

	if(Method == 1)
	{
		std::vector<SV2Chat2Outbound> &Queue = m_aV2Chat2Queue[Dummy];
		if(Queue.size() >= 16)
			return;
		SV2Chat2Outbound Message;
		Message.m_Text = pText;
		Queue.push_back(Message);
		return;
	}

	// v6.21: compress once, then split into as many frames as the channel needs.
	const std::vector<int> vNibbles = EncodeStringToEmotes(pText, DICTIONARY_V2);
	if(vNibbles.empty())
		return;
	std::vector<std::vector<unsigned char>> vFrames;
	const uint32_t Magic = Method == 4 ? MAGIC_AIM : MAGIC_TEXT;
	const int MagicBits = Method == 4 ? MAGIC_AIM_BITS : MAGIC_TEXT_BITS;
	BuildTextFrames(vFrames, Magic, MagicBits, vNibbles, KX_V2_MAX_FRAG_NIBBLES);

	switch(Method)
	{
	case 1: // Chat (invisible payload inside a chat line)
		for(const auto &Frame : vFrames)
			V2SendChatPayload(Dummy, Frame);
		break;
	case 2: // /w Chat
		if(vFrames.size() == 1)
			V2WhisperPayload(Dummy, vFrames.front());
		else
			// Too long for one whisper line: the chat channel is decoded by every
			// client as well, so use it instead of dropping the message.
			for(const auto &Frame : vFrames)
				V2SendChatPayload(Dummy, Frame);
		break;
	case 3: // Emote
		for(const auto &Frame : vFrames)
			V2QueueBits(Dummy, KX_V2_CH_EMOTE, Frame);
		break;
	case 4: // Inputs (aim)
		for(const auto &Frame : vFrames)
			V2QueueBits(Dummy, KX_V2_CH_AIM, Frame);
		break;
	default:
		break;
	}
}

void CIRC::V2QueueIdentity(int Dummy)
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return;
	std::vector<unsigned char> vBits;
	switch(g_Config.m_KxIrcRevealMethod)
	{
	case 1: // Nickname/Clan — handled by EffectiveName/EffectiveClan
		break;
	case 2: // Chat
		BuildIdentityFrame(vBits, MAGIC_TEXT, MAGIC_TEXT_BITS);
		V2SendChatPayload(Dummy, vBits);
		break;
	case 3: // /w Chat
		BuildIdentityFrame(vBits, MAGIC_TEXT, MAGIC_TEXT_BITS);
		V2WhisperPayload(Dummy, vBits);
		break;
	case 4: // Emote
		BuildIdentityFrame(vBits, MAGIC_TEXT, MAGIC_TEXT_BITS);
		V2QueueBits(Dummy, KX_V2_CH_EMOTE, vBits);
		break;
	case 5: // Inputs
		BuildIdentityFrame(vBits, MAGIC_AIM, MAGIC_AIM_BITS);
		V2QueueBits(Dummy, KX_V2_CH_AIM, vBits);
		break;
	default:
		break;
	}
}

void CIRC::V2SendChatPayload(int Dummy, const std::vector<unsigned char> &vBits)
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES || vBits.empty())
		return;
	if(Dummy != 0 && !Client()->DummyConnected(Dummy))
		return;
	// A chat message consisting only of invisible characters is stripped by the
	// server, so the payload has to ride on the next real chat line.
	std::vector<std::vector<unsigned char>> &Queue = m_aV2ChatQueue[Dummy];
	if(Queue.size() >= 16)
		Queue.erase(Queue.begin());
	Queue.push_back(vBits);
}

void CIRC::V2WhisperPayload(int Dummy, const std::vector<unsigned char> &vBits)
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES || vBits.empty())
		return;
	if(Dummy != 0 && !Client()->DummyConnected(Dummy))
		return;
	// The server trims trailing Unicode whitespace from chat messages, and
	// every zero-width symbol counts as whitespace there. A trailing "." keeps
	// the payload from being the trailing whitespace run.
	m_V2SuppressEncode = true;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(IsOwnClient(i) || !IsKinetixClient(i))
			continue;
		const char *pName = GameClient()->m_aClients[i].m_aName;
		if(!pName || pName[0] == '\0')
			continue;
		// v6.21: "/w " + name + " " + payload + "." has to stay inside one chat
		// line. The old fixed 96-byte cap silently truncated longer frames and
		// the receiver dropped them on the CRC check.
		const int Budget = MAX_CHAT_LENGTH - 8 - (int)str_length(pName);
		const std::string Zw = BitsToZw(vBits, Budget);
		if(Zw.empty())
			continue;
		char aLine[512];
		str_format(aLine, sizeof(aLine), "/w %s %s.", pName, Zw.c_str());
		GameClient()->m_Chat.SendChatFrom(Dummy, 0, aLine);
	}
	m_V2SuppressEncode = false;
}

void CIRC::V2PumpChannel(int Dummy, int Channel)
{
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return;
	std::vector<unsigned char> &Queue = m_aV2Bits[Dummy][Channel];

	if(Channel == KX_V2_CH_EMOTE)
	{
		if(Queue.size() < 4)
			return;
		const int CurTick = Client()->GameTick(Dummy);
		if(CurTick < m_aV2EmoteNextTick[Dummy])
			return;
		int Value = 0;
		for(int i = 0; i < 4; ++i)
		{
			Value = (Value << 1) | (Queue.front() & 1);
			Queue.erase(Queue.begin());
		}
		SendEmoteOnDummy(Dummy, Value & 0xF);
		// Conservative spacing: DDNet servers rate-limit emoticons; one per
		// 0.64 s is reliable and never trips the limit.
		m_aV2EmoteNextTick[Dummy] = CurTick + 32;
		return;
	}

	// The aim channel is pumped by ApplyAimOverride(): a byte must be consumed
	// exactly when an input is actually sent, otherwise the sent angle stays
	// unchanged for several ticks and the receiver cannot sample the stream.
}

bool CIRC::HasPendingAim(int Dummy) const
{
	if(g_Config.m_KxIrcVersion != 1 || !g_Config.m_KxIrcEnabled)
		return false;
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return false;
	return m_aV2Bits[Dummy][KX_V2_CH_AIM].size() >= 8;
}

void CIRC::ApplyAimOverride(int Dummy, int *pTargetX, int *pTargetY)
{
	if(g_Config.m_KxIrcVersion != 1 || !g_Config.m_KxIrcEnabled)
		return;
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		return;

	std::vector<unsigned char> &Queue = m_aV2Bits[Dummy][KX_V2_CH_AIM];
	const int CurTick = Client()->GameTick(Dummy);
	if(Queue.size() >= 8 && CurTick != m_aV2AimNextTick[Dummy])
	{
		// One byte per game tick: the receiver samples one angle per tick.
		m_aV2AimNextTick[Dummy] = CurTick;
		int Value = 0;
		for(int i = 0; i < 8; ++i)
		{
			Value = (Value << 1) | (Queue.front() & 1);
			Queue.erase(Queue.begin());
		}
		BucketToAim(Value, m_aV2AimTargetX[Dummy], m_aV2AimTargetY[Dummy]);
		m_aV2AimActive[Dummy] = true;
	}
	if(!m_aV2AimActive[Dummy])
		return;
	*pTargetX = m_aV2AimTargetX[Dummy];
	*pTargetY = m_aV2AimTargetY[Dummy];
	// Deactivate only after this byte was applied to the input; otherwise the
	// last byte of a frame would be consumed and never sent.
	if(Queue.size() < 8)
		m_aV2AimActive[Dummy] = false;
}

void CIRC::V2ScanIdentities()
{
	// Own clients are scanned too: the server echoes the watermarked nick/clan
	// back in the snapshot, so the tag shows up on our own tee/dummy right away
	// instead of only after the first IRC message reveals us.
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		const char *pName = GameClient()->m_aClients[i].m_aName;
		const char *pClan = GameClient()->m_aClients[i].m_aClan;
		if((pName && pName[0] && KxIrcStego::HasIdentity(pName, false)) ||
			(pClan && pClan[0] && KxIrcStego::HasIdentity(pClan, true)))
			V2Mark(i);
	}
}

void CIRC::V2EmitText(int ClientId, const std::vector<int> &vNibbles)
{
	const std::string Text = DecodeEmotesToString(vNibbles, DICTIONARY_V2);
	if(Text.empty())
		return;
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "[IRC] %s: %s", GameClient()->m_aClients[ClientId].m_aName, Text.c_str());
	GameClient()->m_Chat.Echo(aBuf);
}

void CIRC::V2DeliverFrame(int ClientId, const SFrame &Frame, bool EchoSingle)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || Frame.m_Identity)
		return;
	SV2Assembly &Asm = m_aV2Asm[ClientId];
	const int64_t Now = time_get();
	// A stalled assembly (a lost fragment, a reconnect) must not eat the next
	// message of the same sender.
	if(Asm.m_Active && Now > Asm.m_LastTime + time_freq() * 5)
	{
		Asm.m_Active = false;
		Asm.m_vNibbles.clear();
	}

	if(!Frame.m_Frag)
	{
		Asm.m_Active = false;
		Asm.m_vNibbles.clear();
		if(EchoSingle)
			V2EmitText(ClientId, Frame.m_vNibbles);
		return;
	}

	if(Frame.m_Index == 0)
	{
		Asm.m_Active = true;
		Asm.m_Total = Frame.m_Total;
		Asm.m_Next = 1;
		Asm.m_vNibbles = Frame.m_vNibbles;
		Asm.m_LastTime = Now;
		if(Asm.m_Total <= 1)
		{
			Asm.m_Active = false;
			std::vector<int> vAll = Asm.m_vNibbles;
			Asm.m_vNibbles.clear();
			V2EmitText(ClientId, vAll);
		}
		return;
	}

	if(!Asm.m_Active || Frame.m_Index != Asm.m_Next || Frame.m_Total != Asm.m_Total)
	{
		// Out-of-order or lost fragment: drop the partial message instead of
		// gluing unrelated text together.
		Asm.m_Active = false;
		Asm.m_vNibbles.clear();
		return;
	}

	Asm.m_vNibbles.insert(Asm.m_vNibbles.end(), Frame.m_vNibbles.begin(), Frame.m_vNibbles.end());
	Asm.m_Next++;
	Asm.m_LastTime = Now;
	if(Asm.m_Next >= Asm.m_Total)
	{
		Asm.m_Active = false;
		std::vector<int> vAll = Asm.m_vNibbles;
		Asm.m_vNibbles.clear();
		V2EmitText(ClientId, vAll);
	}
}

void CIRC::V2TryDecode(int ClientId, int Channel)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	std::vector<unsigned char> &Buffer = Channel == KX_V2_CH_EMOTE ? m_aV2RxEmote[ClientId] : m_aV2RxAim[ClientId];
	const uint32_t Magic = Channel == KX_V2_CH_EMOTE ? MAGIC_TEXT : MAGIC_AIM;
	const int MagicBits = Channel == KX_V2_CH_EMOTE ? MAGIC_TEXT_BITS : MAGIC_AIM_BITS;

	std::vector<SFrame> vFrames;
	size_t End = 0;
	DecodeFrames(Buffer, Magic, MagicBits, vFrames, &End);
	if(End > 0)
		Buffer.erase(Buffer.begin(), Buffer.begin() + End);
	// Bound the unparsed tail so a stream of noise cannot grow forever.
	if(Buffer.size() > 4096)
		Buffer.erase(Buffer.begin(), Buffer.begin() + (Buffer.size() - 4096));

	if(vFrames.empty())
		return;
	V2Mark(ClientId);
	for(const SFrame &Frame : vFrames)
		V2DeliverFrame(ClientId, Frame, true);
}

void CIRC::OnV2Chat(int ClientId, const char *pMessage)
{
	if(!pMessage || pMessage[0] == '\0')
		return;
	const std::vector<unsigned char> vBits = ZwToBits(pMessage);
	if(vBits.empty())
		return;
	std::vector<SFrame> vFrames;
	DecodeFrames(vBits, MAGIC_TEXT, MAGIC_TEXT_BITS, vFrames, nullptr);
	if(vFrames.empty())
		return;
	V2Mark(ClientId);
	// v6.20: single-frame messages are rendered as a gray suffix at the end of
	// the original chat line by DecodeChatSuffix(), so they are not echoed here.
	// v6.21: fragmented messages span several lines and have nowhere to attach
	// that suffix, so they are echoed once the last fragment arrived.
	for(const SFrame &Frame : vFrames)
		V2DeliverFrame(ClientId, Frame, false);
}

bool CIRC::V2ProcessChat2(int ClientId, int Team, char *pMessage, int MessageSize, char *pSuffix, int SuffixSize)
{
	if(!pMessage || MessageSize <= 0 || !pSuffix || SuffixSize <= 0 || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	pSuffix[0] = '\0';
	std::vector<unsigned char> Wire;
	std::string Visible(pMessage);
	size_t StripBegin = std::string::npos;
	size_t StripEnd = std::string::npos;

	const size_t Separator = Visible.rfind(" | ");
	if(Separator != std::string::npos)
	{
		const std::string Encoded = Visible.substr(Separator + 3);
		if(Chat2DecodeUnicode(Encoded, Wire))
		{
			StripBegin = Separator;
			StripEnd = Visible.size();
		}
	}
	if(Wire.empty())
	{
		const std::string Start = "\xef\xbf\xb9";
		const std::string End = "\xef\xbf\xbb";
		const size_t Begin = Visible.find(Start);
		if(Begin != std::string::npos)
		{
			const size_t Close = Visible.find(End, Begin + Start.size());
			if(Close != std::string::npos)
			{
				StripEnd = Close + End.size();
				if(Chat2UnpackInvisible(Visible.substr(Begin, StripEnd - Begin), Wire))
					StripBegin = Begin;
				else
					StripEnd = std::string::npos;
			}
		}
	}
	if(Wire.empty() || StripBegin == std::string::npos)
		return false;

	if((unsigned char)Wire[0] == CHAT2_HOMOGLYPH_MAGIC)
	{
		std::vector<unsigned char> vFrame;
		std::string RestoredText;
		if(!Chat2UnpackHomoglyphHybrid(Wire, Visible, StripBegin, StripEnd, vFrame, RestoredText))
			return false;
		Wire.swap(vFrame);
		Visible.swap(RestoredText);
		StripBegin = 0;
		StripEnd = 0;
	}

	std::vector<unsigned char> vFrame(Wire);
	if(vFrame.size() < 6 || (vFrame[0] & 0xf0) != 0xa0 || (vFrame[0] & 7) > 4)
		return false;
	const unsigned FrameCrc = Chat2ReadBe32(vFrame.data() + vFrame.size() - 4);
	if(Chat2Crc32(vFrame.data(), vFrame.size() - 4) != FrameCrc)
		return false;

	const int Codec = vFrame[0] & 7;
	std::vector<unsigned char> vCompressed;
	bool Complete = false;
	int PartNumber = 0;
	int PayloadBytesSent = 0;
	int PayloadBytesTotal = 0;
	if((vFrame[0] & 8) == 0)
	{
		vCompressed.assign(vFrame.begin() + 1, vFrame.end() - 4);
		Complete = true;
		PartNumber = 1;
		PayloadBytesSent = PayloadBytesTotal = (int)vCompressed.size();
	}
	else
	{
		if(vFrame.size() < 12)
			return false;
		const unsigned MessageCrc = Chat2ReadBe32(vFrame.data() + 1);
		const int Total = vFrame[5];
		const int Offset = vFrame[6];
		const int ChunkSize = (int)vFrame.size() - 11;
		if(Total <= 0 || ChunkSize <= 0 || Offset + ChunkSize > Total || ChunkSize >= Total)
			return false;

		SV2Chat2Assembly &Asm = m_aV2Chat2Asm[ClientId];
		const int64_t Now = time_get();
		if(Asm.m_Active && Now > Asm.m_LastTime + time_freq() * 60)
			Asm = SV2Chat2Assembly{};
		if(Offset == 0)
		{
			Asm = SV2Chat2Assembly{};
			Asm.m_Active = true;
			Asm.m_Team = Team;
			Asm.m_Codec = Codec;
			Asm.m_Total = Total;
			Asm.m_MessageCrc = MessageCrc;
		}
		if(!Asm.m_Active || Asm.m_Team != Team || Asm.m_Codec != Codec || Asm.m_Total != Total ||
			Asm.m_MessageCrc != MessageCrc || Asm.m_Next != Offset)
		{
			Asm = SV2Chat2Assembly{};
			return false;
		}
		Asm.m_vData.insert(Asm.m_vData.end(), vFrame.begin() + 7, vFrame.end() - 4);
		Asm.m_Next += ChunkSize;
		Asm.m_LastTime = Now;
		++Asm.m_Part;
		PartNumber = Asm.m_Part;
		PayloadBytesSent = Asm.m_Next;
		PayloadBytesTotal = Asm.m_Total;
		char aPartLog[192];
		str_format(aPartLog, sizeof(aPartLog), "Part %d received from %s (%d/%d bytes)", Asm.m_Part,
			GameClient()->m_aClients[ClientId].m_aName, Asm.m_Next, Asm.m_Total);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "IRC", aPartLog);
		if(Asm.m_Next == Total)
		{
			std::vector<unsigned char> vCheck(1, (unsigned char)(0xa0 | Codec));
			vCheck.insert(vCheck.end(), Asm.m_vData.begin(), Asm.m_vData.end());
			if(Chat2Crc32(vCheck.data(), vCheck.size()) != MessageCrc)
			{
				Asm = SV2Chat2Assembly{};
				return false;
			}
			vCompressed.swap(Asm.m_vData);
			Asm = SV2Chat2Assembly{};
			Complete = true;
		}
	}

	std::string Decoded;
	if(Complete)
	{
		if(Codec == 2)
		{
			if(vCompressed.size() < 2 || vCompressed[0] == 0)
				return false;
			std::vector<int> vNibbles;
			for(size_t i = 1; i < vCompressed.size(); ++i)
			{
				vNibbles.push_back(vCompressed[i] >> 4);
				vNibbles.push_back(vCompressed[i] & 0xf);
			}
			Decoded = DecodeEmotesToString(vNibbles, DICTIONARY_V2);
			if(Decoded.size() < vCompressed[0])
				return false;
			Decoded.resize(vCompressed[0]);
		}
		else if(Codec == 3)
		{
			if(vCompressed.size() < 2 || vCompressed[0] < 3)
				return false;
			size_t Pos = 1;
			auto ReadUleb = [&vCompressed, &Pos](uint32_t &Value) {
				Value = 0;
				for(int i = 0; i < 5 && Pos < vCompressed.size(); ++i)
				{
					const unsigned char Byte = vCompressed[Pos++];
					if(i == 4 && (Byte & 0xf0))
						return false;
					Value |= (uint32_t)(Byte & 0x7f) << (7 * i);
					if(!(Byte & 0x80))
						return true;
				}
				return false;
			};
			uint32_t EncodedStart = 0, EncodedStep = 2;
			if(!ReadUleb(EncodedStart) || (Pos < vCompressed.size() && !ReadUleb(EncodedStep)) || Pos != vCompressed.size())
				return false;
			const int64_t Start = (EncodedStart & 1) ? -(int64_t)(EncodedStart / 2) - 1 : EncodedStart / 2;
			const int64_t Step = (EncodedStep & 1) ? -(int64_t)(EncodedStep / 2) - 1 : EncodedStep / 2;
			for(int i = 0; i < vCompressed[0]; ++i)
			{
				const int64_t Value = Start + Step * i;
				if(Value < INT32_MIN || Value > INT32_MAX)
					return false;
				char aNumber[16];
				str_format(aNumber, sizeof(aNumber), "%s%d", i ? " " : "", (int)Value);
				Decoded += aNumber;
			}
		}
		else if(!Chat2DecodeData(Codec, vCompressed, Decoded))
			return false;
		if(Decoded.empty() || Decoded.size() > 255 || !str_utf8_check(Decoded.c_str()))
			return false;
		for(unsigned char c : Decoded)
			if(c < 0x20 || c == 0x7f)
				return false;
		V2Mark(ClientId);
		char aSuccessLog[512];
		str_format(aSuccessLog, sizeof(aSuccessLog), "Message received from %s: %s",
			GameClient()->m_aClients[ClientId].m_aName, Decoded.c_str());
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "IRC", aSuccessLog);
		char aDecodedSuffix[MAX_CHAT_LENGTH * 2];
		str_format(aDecodedSuffix, sizeof(aDecodedSuffix), " (%s)", Decoded.c_str());
		str_copy(pSuffix, aDecodedSuffix, SuffixSize);
	}
	else if(IsOwnClient(ClientId) && PartNumber > 0)
	{
		char aPartSuffix[MAX_CHAT_LENGTH + 4];
		str_format(aPartSuffix, sizeof(aPartSuffix), " (Part %d: %d/%d payload bytes sent, %d remaining)",
			PartNumber, PayloadBytesSent, PayloadBytesTotal, PayloadBytesTotal - PayloadBytesSent);
		str_copy(pSuffix, aPartSuffix, SuffixSize);
	}

	Visible.erase(StripBegin, StripEnd - StripBegin);
	if((int)Visible.size() + 1 > MessageSize)
		return false;
	str_copy(pMessage, Visible.c_str(), MessageSize);
	return true;
}

void CIRC::OnV2Emote(int ClientId, int Emoticon)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	std::vector<unsigned char> &Buffer = m_aV2RxEmote[ClientId];
	// A long pause means the sender stopped; drop the partial frame.
	if(m_aV2RxEmoteTime[ClientId] != 0 && time_get() > m_aV2RxEmoteTime[ClientId] + time_freq() * 5)
		Buffer.clear();
	m_aV2RxEmoteTime[ClientId] = time_get();
	for(int b = 3; b >= 0; --b)
		Buffer.push_back((Emoticon >> b) & 1);
	V2TryDecode(ClientId, KX_V2_CH_EMOTE);
}

void CIRC::UpdateV2()
{
	V2ScanIdentities();

	const int CurTick = Client()->GameTick(0);
	for(int d = 0; d < MAX_DUMMIES; ++d)
	{
		if(d != 0 && !Client()->DummyConnected(d))
		{
			m_aV2Chat2Queue[d].clear();
			continue;
		}

		V2PumpChannel(d, KX_V2_CH_EMOTE);
		V2PumpChannel(d, KX_V2_CH_AIM);

		// Periodic identity beacon for the non-name channels.
		if(g_Config.m_KxIrcRevealMethod >= 2)
		{
			if(m_aV2BeaconTick[d] == 0)
				m_aV2BeaconTick[d] = CurTick + 50 + d * 10;
			else if(CurTick >= m_aV2BeaconTick[d])
			{
				V2QueueIdentity(d);
				m_aV2BeaconTick[d] = CurTick + 50 * 20;
			}
		}
	}

	// Receive the aim channel from other players' character angle.
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(IsOwnClient(i) || !GameClient()->m_Snap.m_apPlayerInfos[i])
			continue;
		const int Tick = Client()->GameTick(0);
		if(Tick == m_aV2RxAimTick[i])
			continue;
		m_aV2RxAimTick[i] = Tick;
		// v6.20: sample the character angle that was actually sent in the
		// server snapshot. m_Predicted is only maintained for the local
		// players, so it stayed constant for everyone else and the Inputs
		// channel never decoded.
		const CNetObj_Character *pCharacter = (const CNetObj_Character *)Client()->SnapFindItemConn(
			0, IClient::SNAP_CURRENT, NETOBJTYPE_CHARACTER, i);
		if(!pCharacter)
			continue;
		const int Bucket = AimToBucket(pCharacter->m_Angle);
		// Do NOT skip repeated buckets: two identical bytes in a row are
		// valid payload, and dropping them shifted the whole bit stream.
		m_aV2RxAimBucket[i] = Bucket;
		std::vector<unsigned char> &Buffer = m_aV2RxAim[i];
		for(int b = 7; b >= 0; --b)
			Buffer.push_back((Bucket >> b) & 1);
		V2TryDecode(i, KX_V2_CH_AIM);
	}
}

CIRC::EIdentityTarget CIRC::IdentityTarget(int Dummy) const
{
	if(g_Config.m_KxIrcVersion != 1 || !g_Config.m_KxIrcEnabled || g_Config.m_KxIrcRevealMethod == 0 || NearFinish(Dummy))
		return EIdentityTarget::NONE;

	// Nick/clan travel through CNetObj_ClientInfo as int arrays and StrToInts()
	// zeroes the last three bytes of the last int, so the real budgets are
	// MAX_CLAN_LENGTH - 3 = 9 bytes for the clan and MAX_NAME_LENGTH - 3 = 13
	// bytes for the nick (name + marker + NUL must fit).
	char aBuf[MAX_NAME_LENGTH];

	// 1. Clan first, when the marker still fits.
	const char *pClan = Dummy == 0 ? g_Config.m_PlayerClan : GameClient()->m_BotNet.m_Dummy.ProfileClan(Dummy);
	if(pClan && pClan[0] != '\0' && KxIrcStego::BuildIdentity(pClan, true, MAX_CLAN_LENGTH - 3, aBuf, sizeof(aBuf)))
		return EIdentityTarget::CLAN;

	// 2. Nick, when the clan could not carry it.
	const char *pName = Dummy == 0 ? Client()->PlayerName() : GameClient()->m_BotNet.m_Dummy.ProfileName(Dummy);
	if(pName && pName[0] != '\0' && KxIrcStego::BuildIdentity(pName, false, MAX_NAME_LENGTH - 3, aBuf, sizeof(aBuf)))
		return EIdentityTarget::NAME;

	// 3. Chat payload as the last resort — EncodeChatLine() puts the identity
	// frame into the next outgoing line.
	return EIdentityTarget::CHAT;
}

const char *CIRC::EffectiveName(int Dummy, char *pBuf, int BufSize) const
{
	const char *pVisible = Dummy == 0 ? Client()->PlayerName() : GameClient()->m_BotNet.m_Dummy.ProfileName(Dummy);
	if(!pVisible)
		pVisible = "";
	if(IdentityTarget(Dummy) != EIdentityTarget::NAME ||
		!KxIrcStego::BuildIdentity(pVisible, false, MAX_NAME_LENGTH - 3, pBuf, BufSize))
		str_copy(pBuf, pVisible, BufSize);
	return pBuf;
}

const char *CIRC::EffectiveClan(int Dummy, char *pBuf, int BufSize) const
{
	const char *pVisible = Dummy == 0 ? g_Config.m_PlayerClan : GameClient()->m_BotNet.m_Dummy.ProfileClan(Dummy);
	if(!pVisible)
		pVisible = "";
	if(IdentityTarget(Dummy) != EIdentityTarget::CLAN ||
		!KxIrcStego::BuildIdentity(pVisible, true, MAX_CLAN_LENGTH - 3, pBuf, BufSize))
		str_copy(pBuf, pVisible, BufSize);
	return pBuf;
}

const char *CIRC::EncodeChatLine(const char *pLine, char *pOut, int OutSize, int Dummy)
{
	if(!pLine || OutSize <= 0)
	{
		if(pOut && OutSize > 0)
			pOut[0] = '\0';
		return pOut;
	}
	str_copy(pOut, pLine, OutSize);
	if(g_Config.m_KxIrcVersion != 1 || !g_Config.m_KxIrcEnabled || m_V2SuppressEncode || pLine[0] == '/')
		return pOut;
	if(*str_utf8_skip_whitespaces(pLine) == '\0')
		return pOut;
	if(Dummy < 0 || Dummy >= MAX_DUMMIES)
		Dummy = 0;

	const int Method = g_Config.m_KxIrcEncodeMethod;
	if(Method == 1 && !m_aV2Chat2Queue[Dummy].empty())
	{
		std::vector<SV2Chat2Outbound> &Queue = m_aV2Chat2Queue[Dummy];

		SV2Chat2Outbound &Message = Queue.front();
		if(Message.m_Codec < 0)
		{
			if(Message.m_Text.empty() || Message.m_Text.size() > 255 || !str_utf8_check(Message.m_Text.c_str()))
			{
				Queue.erase(Queue.begin());
				return pOut;
			}
			for(unsigned char c : Message.m_Text)
			{
				if(c < 0x20 || c == 0x7f)
				{
					Queue.erase(Queue.begin());
					return pOut;
				}
			}
			std::vector<std::pair<int, std::vector<unsigned char>>> vCandidates;
			vCandidates.emplace_back(0, std::vector<unsigned char>(Message.m_Text.begin(), Message.m_Text.end()));

			std::vector<unsigned char> vDeflate;
			if(Chat2Deflate(Message.m_Text, vDeflate))
				vCandidates.emplace_back(1, std::move(vDeflate));

			const std::vector<int> vNibbles = EncodeStringToEmotes(Message.m_Text, DICTIONARY_V2);
			if(!vNibbles.empty())
			{
				std::vector<unsigned char> vDict(1, (unsigned char)Message.m_Text.size());
				for(size_t i = 0; i < vNibbles.size(); i += 2)
					vDict.push_back((unsigned char)(((vNibbles[i] & 0xf) << 4) | (i + 1 < vNibbles.size() ? (vNibbles[i + 1] & 0xf) : 0)));
				if(vDict.size() <= 255)
					vCandidates.emplace_back(2, std::move(vDict));
			}

			std::vector<unsigned char> vNumSeq;
			if(Chat2EncodeNumSeq(Message.m_Text, vNumSeq) && vNumSeq.size() <= 255)
				vCandidates.emplace_back(3, std::move(vNumSeq));

			static constexpr char s_aDigits[] = "0123456789 +-.,"; // 15 symbols; 0xF is reserved for padding.
			std::vector<unsigned char> vPackedDigits;
			bool DigitsOnly = !Message.m_Text.empty();
			for(size_t i = 0; i < Message.m_Text.size(); i += 2)
			{
				const char *pHigh = strchr(s_aDigits, Message.m_Text[i]);
				const char *pLow = i + 1 < Message.m_Text.size() ? strchr(s_aDigits, Message.m_Text[i + 1]) : nullptr;
				if(!pHigh || (i + 1 < Message.m_Text.size() && !pLow))
				{
					DigitsOnly = false;
					break;
				}
				vPackedDigits.push_back((unsigned char)(((pHigh - s_aDigits) << 4) | (pLow ? pLow - s_aDigits : 0xf)));
			}
			if(DigitsOnly && vPackedDigits.size() <= 255)
				vCandidates.emplace_back(4, std::move(vPackedDigits));

			if(vCandidates.empty())
			{
				Queue.erase(Queue.begin());
				return pOut;
			}
			auto Best = std::min_element(vCandidates.begin(), vCandidates.end(), [](const auto &A, const auto &B) {
				return A.second.size() < B.second.size() || (A.second.size() == B.second.size() && A.first < B.first);
			});
			Message.m_Codec = Best->first;
			Message.m_vData = std::move(Best->second);
			std::vector<unsigned char> vMessage;
			vMessage.push_back((unsigned char)(0xa0 | Message.m_Codec));
			vMessage.insert(vMessage.end(), Message.m_vData.begin(), Message.m_vData.end());
			Message.m_MessageCrc = Chat2Crc32(vMessage.data(), vMessage.size());
		}

		std::string Visible;
		std::vector<SZwEntry> vOldMarkers;
		SplitText(pLine, Visible, vOldMarkers);
		if(Visible.empty())
			return pOut;
		const int MaxBytes = std::min(OutSize - 1, MAX_CHAT_LENGTH - 1);
		const bool VisiblePayload = g_Config.m_KxIrcVisiblePayload != 0;
		const int InsertAt = Chat2SafeInsertPosition(Visible);
		const int Remaining = (int)Message.m_vData.size() - Message.m_Offset;
		if(Remaining <= 0)
		{
			Queue.erase(Queue.begin());
			return pOut;
		}

		for(int Chunk = Remaining; Chunk > 0; --Chunk)
		{
			const bool Fragment = Message.m_Offset != 0 || Chunk < Remaining;
			std::vector<unsigned char> vFrame;
			if(!Fragment)
			{
				vFrame.push_back((unsigned char)(0xa0 | Message.m_Codec));
				vFrame.insert(vFrame.end(), Message.m_vData.begin(), Message.m_vData.end());
				Chat2PutBe32(vFrame, Chat2Crc32(vFrame.data(), vFrame.size()));
			}
			else
			{
				vFrame.push_back((unsigned char)(0xa8 | Message.m_Codec));
				Chat2PutBe32(vFrame, Message.m_MessageCrc);
				vFrame.push_back((unsigned char)Message.m_vData.size());
				vFrame.push_back((unsigned char)Message.m_Offset);
				vFrame.insert(vFrame.end(), Message.m_vData.begin() + Message.m_Offset, Message.m_vData.begin() + Message.m_Offset + Chunk);
				Chat2PutBe32(vFrame, Chat2Crc32(vFrame.data(), vFrame.size()));
			}

			std::string Payload;
			if(VisiblePayload)
			{
				std::string Encoded;
				if(!Chat2EncodeUnicode(vFrame, Encoded))
					continue;
				Payload = " | " + Encoded;
			}
			else if(!Chat2PackInvisible(vFrame, Payload))
				continue;

			std::string Out;
			if(VisiblePayload)
				Out = Visible + Payload;
			else
			{
				if(InsertAt < 0)
					continue;
				Out.append(Visible, 0, InsertAt);
				Out += Payload;
				Out.append(Visible, InsertAt, std::string::npos);
				std::string Hybrid;
				if(Chat2PackHomoglyphHybrid(vFrame, Visible, Hybrid) &&
					Hybrid.size() < Out.size() && (int)Hybrid.size() + 1 <= OutSize &&
					(int)Hybrid.size() <= MaxBytes && CountUtf8Chars(Hybrid) < MAX_CHAT_LENGTH)
					Out.swap(Hybrid);
			}
			if((int)Out.size() + 1 > OutSize || (int)Out.size() > MaxBytes || CountUtf8Chars(Out) >= MAX_CHAT_LENGTH)
				continue;

			str_copy(pOut, Out.c_str(), OutSize);
			if(Fragment)
			{
				Message.m_Offset += Chunk;
				if(Message.m_Offset >= (int)Message.m_vData.size())
					Queue.erase(Queue.begin());
			}
			else
				Queue.erase(Queue.begin());
			return pOut;
		}
		return pOut;
	}
	m_aV2Chat2Queue[Dummy].clear();

	if(Method != 1)
	{
		// Side channels carry the line themselves; the visible chat line
		// stays clean.
		V2QueueMessage(Dummy, pLine);
		return pOut;
	}

	// Chat channel: hide the frames between characters of the outgoing line.
	std::string Visible;
	std::vector<SZwEntry> vZw;
	SplitText(pLine, Visible, vZw);
	if(Visible.empty())
		return pOut;
	// RandomCharPos needs a gap between two characters and returns -1 for a
	// one-character line ("."). Such a line still has to carry the payload,
	// otherwise a queued IRC message never leaves the queue. The payload goes in
	// front of the character: the server only trims *trailing* whitespace, and
	// every zero-width symbol counts as whitespace.
	const int InsertChar = RandomCharPos(Visible, SALT_CHAT);
	const int InsertAt = InsertChar < 0 ? 0 : ByteOffsetOfChar(Visible, InsertChar);

	const int Budget = MAX_CHAT_LENGTH - 2 - (int)Visible.size(); // bytes left for tags
	const int MaxBits = (Budget / ZW_BYTES) * ZW_BITS;
	if(MaxBits <= 0)
		return pOut;

	std::vector<unsigned char> vBits;
	// Explicit "reveal via chat", or the fallback when neither the clan nor the
	// nick has room for the identity marker.
	if(g_Config.m_KxIrcRevealMethod == 2 || IdentityTarget(Dummy) == EIdentityTarget::CHAT)
		BuildIdentityFrame(vBits, MAGIC_TEXT, MAGIC_TEXT_BITS);

	// 1. Queued IRC frames, oldest first, only while they still fit. The
	// visible line itself is NOT embedded: it already arrives as a normal chat
	// message, and embedding it used to eat the whole byte budget, so queued
	// messages never fit on a typical message.
	std::vector<std::vector<unsigned char>> &Queue = m_aV2ChatQueue[Dummy];
	while(!Queue.empty() && (int)(vBits.size() + Queue.front().size()) <= MaxBits)
	{
		vBits.insert(vBits.end(), Queue.front().begin(), Queue.front().end());
		Queue.erase(Queue.begin());
	}

	if(vBits.empty())
		return pOut;

	const std::string Zw = BitsToZw(vBits, Budget);
	if(Zw.empty())
		return pOut;

	std::string Out;
	Out.append(Visible, 0, InsertAt);
	Out += Zw;
	Out.append(Visible, InsertAt, std::string::npos);
	if((int)Out.size() + 1 > OutSize)
		return pOut;
	str_copy(pOut, Out.c_str(), OutSize);
	return pOut;
}
