// (c) Kinetix. Per-dummy profile + proxy storage implementation (Dummy
// Manager) — see dummy_data.h for the design notes.
#include <game/client/components/kinetix/dummies/dummy_data.h>

#include <base/dbg.h>
#include <base/io.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client.h>
#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

// Config-file formatting helpers (plain char constants — no escape sequences).
static const char DUMMY_CFG_QUOTE = '"';
static const char DUMMY_CFG_NL = (char)10;

// Registered runtime command fields.
enum EDummyField
{
	FIELD_NAME = 0,
	FIELD_CLAN,
	FIELD_COUNTRY,
	FIELD_SKIN,
	FIELD_USE_CUSTOM_COLOR,
	FIELD_COLOR_BODY,
	FIELD_COLOR_FEET,
	FIELD_DEFAULT_EYES,
	FIELD_PROXY,
	FIELD_PROXY_ON,
};

struct SDummyFieldDef
{
	const char *m_pFormat; // command name printf format
	int m_Field;
	bool m_Str; // true = string value, false = integer value
	const char *m_pHelpFormat;
};

static const SDummyFieldDef s_aProfileFields[] = {
	{"dummy%d_name", FIELD_NAME, true, "Dummy %d profile name (empty = shared dummy_name)"},
	{"dummy%d_clan", FIELD_CLAN, true, "Dummy %d profile clan (empty = shared dummy_clan)"},
	{"dummy%d_country", FIELD_COUNTRY, false, "Dummy %d profile country (-1 = shared dummy_country)"},
	{"dummy%d_skin", FIELD_SKIN, true, "Dummy %d profile skin (empty = shared dummy_skin)"},
	{"dummy%d_use_custom_color", FIELD_USE_CUSTOM_COLOR, false, "Dummy %d custom colors toggle (0/1, -1 = inherit dummy_use_custom_color)"},
	{"dummy%d_color_body", FIELD_COLOR_BODY, false, "Dummy %d body color (-1 = inherit dummy_color_body)"},
	{"dummy%d_color_feet", FIELD_COLOR_FEET, false, "Dummy %d feet color (-1 = inherit dummy_color_feet)"},
	{"dummy%d_default_eyes", FIELD_DEFAULT_EYES, false, "Dummy %d default eyes (-1 = inherit dummy_default_eyes)"},
};

static const SDummyFieldDef s_aProxyFields[] = {
	{"dummy%d_proxy", FIELD_PROXY, true, "Dummy %d SOCKS5 proxy (empty = none, format user:pass@host:port)"},
	{"dummy%d_proxy_on", FIELD_PROXY_ON, false, "Dummy %d proxy toggle (0/1)"},
};

static constexpr int NUM_PROFILE_FIELDS = (int)(sizeof(s_aProfileFields) / sizeof(s_aProfileFields[0]));
static constexpr int NUM_PROXY_FIELDS = (int)(sizeof(s_aProxyFields) / sizeof(s_aProxyFields[0]));

struct SDummyCmdCtx
{
	CDummyData *m_pSelf;
	int m_Slot;
	int m_Field;
};

static SDummyCmdCtx s_aDummyCmdCtx[MAX_DUMMIES * 12];
static int s_NumDummyCmdCtx = 0;

static void Con_DummyStr(IConsole::IResult *pResult, void *pUserData)
{
	SDummyCmdCtx *pCtx = (SDummyCmdCtx *)pUserData;
	SDummyProfile &P = pCtx->m_pSelf->m_aProfiles[pCtx->m_Slot];
	const char *pValue = pResult->GetString(0);
	switch(pCtx->m_Field)
	{
	case FIELD_NAME: str_copy(P.m_aName, pValue, sizeof(P.m_aName)); break;
	case FIELD_CLAN: str_copy(P.m_aClan, pValue, sizeof(P.m_aClan)); break;
	case FIELD_SKIN: str_copy(P.m_aSkin, pValue, sizeof(P.m_aSkin)); break;
	case FIELD_PROXY: str_copy(P.m_aProxy, pValue, sizeof(P.m_aProxy)); break;
	default: return;
	}
	if(!pCtx->m_pSelf->m_Loading)
		pCtx->m_pSelf->Save();
}

static void Con_DummyInt(IConsole::IResult *pResult, void *pUserData)
{
	SDummyCmdCtx *pCtx = (SDummyCmdCtx *)pUserData;
	SDummyProfile &P = pCtx->m_pSelf->m_aProfiles[pCtx->m_Slot];
	const int Value = pResult->GetInteger(0);
	switch(pCtx->m_Field)
	{
	case FIELD_COUNTRY: P.m_Country = Value; break;
	case FIELD_USE_CUSTOM_COLOR: P.m_UseCustomColor = Value; break;
	case FIELD_COLOR_BODY: P.m_ColorBody = (unsigned)Value; break;
	case FIELD_COLOR_FEET: P.m_ColorFeet = (unsigned)Value; break;
	case FIELD_DEFAULT_EYES: P.m_DefaultEyes = Value; break;
	case FIELD_PROXY_ON: P.m_ProxyOn = Value; break;
	default: return;
	}
	if(!pCtx->m_pSelf->m_Loading)
		pCtx->m_pSelf->Save();
}

static void Con_DummyLimit(IConsole::IResult *pResult, void *pUserData)
{
	CDummyData *pSelf = (CDummyData *)pUserData;
	if(!pSelf->m_pClient)
		return;
	if(pResult->NumArguments() > 0)
		pSelf->m_pClient->SetDummyLimit(pResult->GetInteger(0));
	else
		dbg_msg("dummy", "kx_dummy_limit=%d", pSelf->m_pClient->DummyLimit());
}

static void RegisterDummyFields(CDummyData *pSelf, IConsole *pConsole, const SDummyFieldDef *pDefs, int NumDefs, int Slot)
{
	for(int F = 0; F < NumDefs; F++)
	{
		char aCmd[64];
		char aHelp[192];
		str_format(aCmd, sizeof(aCmd), pDefs[F].m_pFormat, Slot);
		str_format(aHelp, sizeof(aHelp), pDefs[F].m_pHelpFormat, Slot);
		if(s_NumDummyCmdCtx >= (int)(MAX_DUMMIES * 12))
			return;
		SDummyCmdCtx &Ctx = s_aDummyCmdCtx[s_NumDummyCmdCtx++];
		Ctx.m_pSelf = pSelf;
		Ctx.m_Slot = Slot;
		Ctx.m_Field = pDefs[F].m_Field;
		if(pDefs[F].m_Str)
			pConsole->Register(aCmd, "s[value]", CFGFLAG_CLIENT, Con_DummyStr, &Ctx, aHelp);
		else
			pConsole->Register(aCmd, "i[value]", CFGFLAG_CLIENT, Con_DummyInt, &Ctx, aHelp);
	}
}

void CDummyData::ResetDefaults()
{
	mem_zero(m_aProfiles, sizeof(m_aProfiles));
	for(int D = 0; D < MAX_DUMMIES; D++)
	{
		m_aProfiles[D].m_Country = -1;
		m_aProfiles[D].m_UseCustomColor = -1;
		m_aProfiles[D].m_ColorBody = 0xFFFFFFFF;
		m_aProfiles[D].m_ColorFeet = 0xFFFFFFFF;
		m_aProfiles[D].m_DefaultEyes = -1;
	}
}

void CDummyData::Init(IConsole *pConsole, IClient *pClient, IStorage *pStorage)
{
	m_pConsole = pConsole;
	m_pClient = pClient;
	m_pStorage = pStorage;

	ResetDefaults();

	// Per-dummy profile commands (main profile is the vanilla player_* config,
	// so dummy0_* profile fields are not registered).
	for(int D = 1; D < MAX_DUMMIES; D++)
		RegisterDummyFields(this, m_pConsole, s_aProfileFields, NUM_PROFILE_FIELDS, D);
	// Proxy commands (dummy0_proxy = main connection proxy).
	for(int D = 0; D < MAX_DUMMIES; D++)
		RegisterDummyFields(this, m_pConsole, s_aProxyFields, NUM_PROXY_FIELDS, D);
	m_pConsole->Register("kx_dummy_limit", "i[limit]", CFGFLAG_CLIENT, Con_DummyLimit, this,
		"Dummy limit incl. main (1..128): ClickGUI Dummies slider; respected by dummy_connect/ClickGUI/Reconnect");

	// Load the persisted state (if any) and push it into the engine.
	if(m_pStorage->FileExists("kinetix_dummies.cfg", IStorage::TYPE_SAVE) ||
		m_pStorage->FileExists("kinetix_dummies.cfg", IStorage::TYPE_ALL))
	{
		m_Loading = true;
		m_pConsole->ExecuteFile("kinetix_dummies.cfg", false);
		m_Loading = false;
		Save();
	}
	ApplyToEngine();
}

void CDummyData::Save()
{
	if(!m_pStorage)
		return;
	IOHANDLE File = m_pStorage->OpenFile("kinetix_dummies.cfg", IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;
	char aBuf[384];
	if(m_pClient)
	{
		str_format(aBuf, sizeof(aBuf), "kx_dummy_limit %d%c", m_pClient->DummyLimit(), DUMMY_CFG_NL);
		io_write(File, aBuf, str_length(aBuf));
	}
	for(int D = 0; D < MAX_DUMMIES; D++)
	{
		const SDummyProfile &P = m_aProfiles[D];
		if(D >= 1)
		{
			if(P.m_aName[0])
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_name %c%s%c%c", D, DUMMY_CFG_QUOTE, P.m_aName, DUMMY_CFG_QUOTE, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_aClan[0])
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_clan %c%s%c%c", D, DUMMY_CFG_QUOTE, P.m_aClan, DUMMY_CFG_QUOTE, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_Country >= 0)
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_country %d%c", D, P.m_Country, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_aSkin[0])
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_skin %c%s%c%c", D, DUMMY_CFG_QUOTE, P.m_aSkin, DUMMY_CFG_QUOTE, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_UseCustomColor >= 0)
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_use_custom_color %d%c", D, P.m_UseCustomColor, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_ColorBody != 0xFFFFFFFF)
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_color_body %u%c", D, P.m_ColorBody, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_ColorFeet != 0xFFFFFFFF)
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_color_feet %u%c", D, P.m_ColorFeet, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
			if(P.m_DefaultEyes >= 0)
			{
				str_format(aBuf, sizeof(aBuf), "dummy%d_default_eyes %d%c", D, P.m_DefaultEyes, DUMMY_CFG_NL);
				io_write(File, aBuf, str_length(aBuf));
			}
		}
		if(P.m_aProxy[0])
		{
			str_format(aBuf, sizeof(aBuf), "dummy%d_proxy %c%s%c%c", D, DUMMY_CFG_QUOTE, P.m_aProxy, DUMMY_CFG_QUOTE, DUMMY_CFG_NL);
			io_write(File, aBuf, str_length(aBuf));
		}
		if(P.m_ProxyOn)
		{
			str_format(aBuf, sizeof(aBuf), "dummy%d_proxy_on %d%c", D, P.m_ProxyOn, DUMMY_CFG_NL);
			io_write(File, aBuf, str_length(aBuf));
		}
	}
	io_close(File);
}

void CDummyData::ApplyToEngine()
{
	if(!m_pClient)
		return;
	for(int D = 0; D < MAX_DUMMIES; D++)
		m_pClient->SetDummyProxy(D, m_aProfiles[D].m_ProxyOn, m_aProfiles[D].m_aProxy);
}

const char *CDummyData::ProfileName(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_aName[0])
		return m_aProfiles[Dummy].m_aName;
	if(g_Config.m_ClDummyName[0])
		return g_Config.m_ClDummyName;
	return m_pClient ? m_pClient->DummyName() : "brainless tee";
}

const char *CDummyData::ProfileClan(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_aClan[0])
		return m_aProfiles[Dummy].m_aClan;
	return g_Config.m_ClDummyClan;
}

int CDummyData::ProfileCountry(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_Country >= 0)
		return m_aProfiles[Dummy].m_Country;
	return g_Config.m_ClDummyCountry;
}

const char *CDummyData::ProfileSkin(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_aSkin[0])
		return m_aProfiles[Dummy].m_aSkin;
	return g_Config.m_ClDummySkin;
}

int CDummyData::ProfileUseCustomColor(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_UseCustomColor >= 0)
		return m_aProfiles[Dummy].m_UseCustomColor;
	return g_Config.m_ClDummyUseCustomColor;
}

unsigned CDummyData::ProfileColorBody(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_ColorBody != 0xFFFFFFFF)
		return m_aProfiles[Dummy].m_ColorBody;
	return g_Config.m_ClDummyColorBody;
}

unsigned CDummyData::ProfileColorFeet(int Dummy) const
{
	if(Dummy >= 1 && Dummy < MAX_DUMMIES && m_aProfiles[Dummy].m_ColorFeet != 0xFFFFFFFF)
		return m_aProfiles[Dummy].m_ColorFeet;
	return g_Config.m_ClDummyColorFeet;
}