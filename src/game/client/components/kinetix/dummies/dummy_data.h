// (c) Kinetix. Per-dummy profile + proxy storage (Dummy Manager).
//
// v1.56.282: deliberately NOT part of g_Config / config_variables.h — the
// dummy profile and proxy settings are exposed as RUNTIME console commands
// ("dummy1_name", "dummy5_proxy", "kx_dummy_limit", ...) registered here and
// persisted to "kinetix_dummies.cfg" in the save folder (next to
// settings.cfg). Changing MAX_DUMMIES requires no config header edits.
//
// Fallback chain everywhere: per-dummy value -> shared dummy_* config ->
// the vanilla behavior the caller would have used.
#ifndef GAME_CLIENT_COMPONENTS_KINETIX_DUMMIES_DUMMY_DATA_H
#define GAME_CLIENT_COMPONENTS_KINETIX_DUMMIES_DUMMY_DATA_H

#include <engine/client/enums.h>

class IClient;
class IConsole;
class IStorage;

// Per-dummy profile + proxy settings. Index 0 = main: only the proxy fields
// are used there (main's profile is the vanilla player_* config).
struct SDummyProfile
{
	char m_aName[16];
	char m_aClan[12];
	int m_Country; // -1 = inherit shared dummy_country
	char m_aSkin[24];
	int m_UseCustomColor; // -1 = inherit shared dummy_use_custom_color
	unsigned m_ColorBody; // 0xFFFFFFFF = inherit shared dummy_color_body
	unsigned m_ColorFeet; // 0xFFFFFFFF = inherit shared dummy_color_feet
	int m_DefaultEyes; // -1 = inherit shared dummy_default_eyes
	char m_aProxy[256]; // "" = none, otherwise "user:pass@host:port"
	int m_ProxyOn; // proxy toggle for this slot (0/1)
};

class CDummyData
{
public:
	void Init(IConsole *pConsole, IClient *pClient, IStorage *pStorage);
	void Save();

	// Push the proxy settings into the engine (IClient).
	void ApplyToEngine();

	SDummyProfile m_aProfiles[MAX_DUMMIES];

	// Set while ExecuteFile() replays the save file so per-command saves are
	// suppressed (one Save() at the end instead).
	bool m_Loading = false;

	// Fallback-aware getters used by the connect/info paths.
	const char *ProfileName(int Dummy) const;
	const char *ProfileClan(int Dummy) const;
	int ProfileCountry(int Dummy) const;
	const char *ProfileSkin(int Dummy) const;
	int ProfileUseCustomColor(int Dummy) const;
	unsigned ProfileColorBody(int Dummy) const;
	unsigned ProfileColorFeet(int Dummy) const;

	// Public so the static console callbacks can reach them.
	IClient *m_pClient = nullptr;
	IConsole *m_pConsole = nullptr;
	IStorage *m_pStorage = nullptr;

private:
	void ResetDefaults();
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_DUMMIES_DUMMY_DATA_H