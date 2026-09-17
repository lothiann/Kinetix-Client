#include <game/client/components/kinetix/aimbot.h>

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>
#include <stdlib.h>
#include <base/vmath.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <game/client/gameclient.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <game/client/components/kinetix/kinetix_lines.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/tuning.h>
#include <generated/protocol.h>

#include <algorithm>
#include <cmath>
#include <vector>

void CAimBot::OnConsoleInit()
{
	// v1.56.171 BUG9: kx_aimbot and kx_triggerbot are now MACRO_CONFIG_INT cvars
	// (KxAimBot, KxTriggerBot). `toggle kx_aimbot 1 0` works in binds.
}

// v1.56.255: per-weapon cvar tables — one entry per weapon, same order as
// m_AimBotWeapons (hammer, pistol, shotgun, grenade, laser, ninja, hook) and
// m_TriggerBotWeapons (same minus hook). Used by SyncSavedSettings().
struct KxAbCvars
{
	int *pEnabled;
	int *pFov;
	int *pRadius;
	int *pRaycastShow;
	int *pRules;
	int *pUseAngle;
	int *pPredict;
	int *pJitter;
	int *pAimMode;
	int *pTeam;
	int *pFriend;
	int *pDummy;
	int *pClanmate;
	int *pTargetsFilter;
	char *pTargetsIds;
	int *pFreeze;
	int *pFngFreeze;
	int *pAutoSwitch;
	int *pPriority;
	int *pSilent;
	int *pSmoothSpeed;
	int *pSmoothCenterSnap;
};

static const KxAbCvars s_aAbCvars[] = {
	{&g_Config.m_KxAbHammerEnabled, &g_Config.m_KxAbHammerFov, &g_Config.m_KxAbHammerRadius,
		&g_Config.m_KxAbHammerRaycastShow, &g_Config.m_KxAbHammerRules, &g_Config.m_KxAbHammerUseAngle,
		&g_Config.m_KxAbHammerPredict, &g_Config.m_KxAbHammerJitter, &g_Config.m_KxAbHammerAimMode,
		&g_Config.m_KxAbHammerTeam, &g_Config.m_KxAbHammerFriend, &g_Config.m_KxAbHammerDummy,
		&g_Config.m_KxAbHammerClanmate, &g_Config.m_KxAbHammerTargetsFilter, g_Config.m_KxAbHammerTargetsIds,
		&g_Config.m_KxAbHammerFreeze, &g_Config.m_KxAbHammerFngFreeze, &g_Config.m_KxAbHammerAutoSwitch, &g_Config.m_KxAbHammerPriority,
		&g_Config.m_KxAbHammerSilent, &g_Config.m_KxAbHammerSmoothSpeed,
		&g_Config.m_KxAbHammerSmoothCenterSnap},
	{&g_Config.m_KxAbPistolEnabled, &g_Config.m_KxAbPistolFov, &g_Config.m_KxAbPistolRadius,
		&g_Config.m_KxAbPistolRaycastShow, &g_Config.m_KxAbPistolRules, &g_Config.m_KxAbPistolUseAngle,
		&g_Config.m_KxAbPistolPredict, &g_Config.m_KxAbPistolJitter, &g_Config.m_KxAbPistolAimMode,
		&g_Config.m_KxAbPistolTeam, &g_Config.m_KxAbPistolFriend, &g_Config.m_KxAbPistolDummy,
		&g_Config.m_KxAbPistolClanmate, &g_Config.m_KxAbPistolTargetsFilter, g_Config.m_KxAbPistolTargetsIds,
		&g_Config.m_KxAbPistolFreeze, &g_Config.m_KxAbPistolFngFreeze, &g_Config.m_KxAbPistolAutoSwitch, &g_Config.m_KxAbPistolPriority,
		&g_Config.m_KxAbPistolSilent, &g_Config.m_KxAbPistolSmoothSpeed,
		&g_Config.m_KxAbPistolSmoothCenterSnap},
	{&g_Config.m_KxAbShotgunEnabled, &g_Config.m_KxAbShotgunFov, &g_Config.m_KxAbShotgunRadius,
		&g_Config.m_KxAbShotgunRaycastShow, &g_Config.m_KxAbShotgunRules, &g_Config.m_KxAbShotgunUseAngle,
		&g_Config.m_KxAbShotgunPredict, &g_Config.m_KxAbShotgunJitter, &g_Config.m_KxAbShotgunAimMode,
		&g_Config.m_KxAbShotgunTeam, &g_Config.m_KxAbShotgunFriend, &g_Config.m_KxAbShotgunDummy,
		&g_Config.m_KxAbShotgunClanmate, &g_Config.m_KxAbShotgunTargetsFilter, g_Config.m_KxAbShotgunTargetsIds,
		&g_Config.m_KxAbShotgunFreeze, &g_Config.m_KxAbShotgunFngFreeze, &g_Config.m_KxAbShotgunAutoSwitch, &g_Config.m_KxAbShotgunPriority,
		&g_Config.m_KxAbShotgunSilent, &g_Config.m_KxAbShotgunSmoothSpeed,
		&g_Config.m_KxAbShotgunSmoothCenterSnap},
	{&g_Config.m_KxAbGrenadeEnabled, &g_Config.m_KxAbGrenadeFov, &g_Config.m_KxAbGrenadeRadius,
		&g_Config.m_KxAbGrenadeRaycastShow, &g_Config.m_KxAbGrenadeRules, &g_Config.m_KxAbGrenadeUseAngle,
		&g_Config.m_KxAbGrenadePredict, &g_Config.m_KxAbGrenadeJitter, &g_Config.m_KxAbGrenadeAimMode,
		&g_Config.m_KxAbGrenadeTeam, &g_Config.m_KxAbGrenadeFriend, &g_Config.m_KxAbGrenadeDummy,
		&g_Config.m_KxAbGrenadeClanmate, &g_Config.m_KxAbGrenadeTargetsFilter, g_Config.m_KxAbGrenadeTargetsIds,
		&g_Config.m_KxAbGrenadeFreeze, &g_Config.m_KxAbGrenadeFngFreeze, &g_Config.m_KxAbGrenadeAutoSwitch, &g_Config.m_KxAbGrenadePriority,
		&g_Config.m_KxAbGrenadeSilent, &g_Config.m_KxAbGrenadeSmoothSpeed,
		&g_Config.m_KxAbGrenadeSmoothCenterSnap},
	{&g_Config.m_KxAbLaserEnabled, &g_Config.m_KxAbLaserFov, &g_Config.m_KxAbLaserRadius,
		&g_Config.m_KxAbLaserRaycastShow, &g_Config.m_KxAbLaserRules, &g_Config.m_KxAbLaserUseAngle,
		&g_Config.m_KxAbLaserPredict, &g_Config.m_KxAbLaserJitter, &g_Config.m_KxAbLaserAimMode,
		&g_Config.m_KxAbLaserTeam, &g_Config.m_KxAbLaserFriend, &g_Config.m_KxAbLaserDummy,
		&g_Config.m_KxAbLaserClanmate, &g_Config.m_KxAbLaserTargetsFilter, g_Config.m_KxAbLaserTargetsIds,
		&g_Config.m_KxAbLaserFreeze, &g_Config.m_KxAbLaserFngFreeze, &g_Config.m_KxAbLaserAutoSwitch, &g_Config.m_KxAbLaserPriority,
		&g_Config.m_KxAbLaserSilent, &g_Config.m_KxAbLaserSmoothSpeed,
		&g_Config.m_KxAbLaserSmoothCenterSnap},
	{&g_Config.m_KxAbNinjaEnabled, &g_Config.m_KxAbNinjaFov, &g_Config.m_KxAbNinjaRadius,
		&g_Config.m_KxAbNinjaRaycastShow, &g_Config.m_KxAbNinjaRules, &g_Config.m_KxAbNinjaUseAngle,
		&g_Config.m_KxAbNinjaPredict, &g_Config.m_KxAbNinjaJitter, &g_Config.m_KxAbNinjaAimMode,
		&g_Config.m_KxAbNinjaTeam, &g_Config.m_KxAbNinjaFriend, &g_Config.m_KxAbNinjaDummy,
		&g_Config.m_KxAbNinjaClanmate, &g_Config.m_KxAbNinjaTargetsFilter, g_Config.m_KxAbNinjaTargetsIds,
		&g_Config.m_KxAbNinjaFreeze, &g_Config.m_KxAbNinjaFngFreeze, &g_Config.m_KxAbNinjaAutoSwitch, &g_Config.m_KxAbNinjaPriority,
		&g_Config.m_KxAbNinjaSilent, &g_Config.m_KxAbNinjaSmoothSpeed,
		&g_Config.m_KxAbNinjaSmoothCenterSnap},
	{&g_Config.m_KxAbHookEnabled, &g_Config.m_KxAbHookFov, &g_Config.m_KxAbHookRadius,
		&g_Config.m_KxAbHookRaycastShow, &g_Config.m_KxAbHookRules, &g_Config.m_KxAbHookUseAngle,
		&g_Config.m_KxAbHookPredict, &g_Config.m_KxAbHookJitter, &g_Config.m_KxAbHookAimMode,
		&g_Config.m_KxAbHookTeam, &g_Config.m_KxAbHookFriend, &g_Config.m_KxAbHookDummy,
		&g_Config.m_KxAbHookClanmate, &g_Config.m_KxAbHookTargetsFilter, g_Config.m_KxAbHookTargetsIds,
		&g_Config.m_KxAbHookFreeze, &g_Config.m_KxAbHookFngFreeze, &g_Config.m_KxAbHookAutoSwitch, &g_Config.m_KxAbHookPriority,
		&g_Config.m_KxAbHookSilent, &g_Config.m_KxAbHookSmoothSpeed,
		&g_Config.m_KxAbHookSmoothCenterSnap},
};

struct KxTbCvars
{
	int *pEnabled;
	int *pTrigger;
	int *pTriggerMode;
	int *pFov;
	int *pRadius;
	int *pRaycastShow;
	int *pRules;
	int *pTeam;
	int *pFriend;
	int *pDummy;
	int *pClanmate;
	int *pTargetsFilter;
	char *pTargetsIds;
	int *pFreeze;
	int *pFngFreeze;
	int *pAutoSwitch;
	int *pPriority;
	int *pLatency;
};

static const KxTbCvars s_aTbCvars[] = {
	{&g_Config.m_KxTbHammerEnabled, &g_Config.m_KxTbHammerTrigger, &g_Config.m_KxTbHammerTriggerMode,
		&g_Config.m_KxTbHammerFov, &g_Config.m_KxTbHammerRadius,
		&g_Config.m_KxTbHammerRaycastShow, &g_Config.m_KxTbHammerRules,
		&g_Config.m_KxTbHammerTeam, &g_Config.m_KxTbHammerFriend, &g_Config.m_KxTbHammerDummy,
		&g_Config.m_KxTbHammerClanmate, &g_Config.m_KxTbHammerTargetsFilter, g_Config.m_KxTbHammerTargetsIds,
		&g_Config.m_KxTbHammerFreeze, &g_Config.m_KxTbHammerFngFreeze, &g_Config.m_KxTbHammerAutoSwitch, &g_Config.m_KxTbHammerPriority, &g_Config.m_KxTbHammerLatency},
	{&g_Config.m_KxTbPistolEnabled, &g_Config.m_KxTbPistolTrigger, &g_Config.m_KxTbPistolTriggerMode,
		&g_Config.m_KxTbPistolFov, &g_Config.m_KxTbPistolRadius,
		&g_Config.m_KxTbPistolRaycastShow, &g_Config.m_KxTbPistolRules,
		&g_Config.m_KxTbPistolTeam, &g_Config.m_KxTbPistolFriend, &g_Config.m_KxTbPistolDummy,
		&g_Config.m_KxTbPistolClanmate, &g_Config.m_KxTbPistolTargetsFilter, g_Config.m_KxTbPistolTargetsIds,
		&g_Config.m_KxTbPistolFreeze, &g_Config.m_KxTbPistolFngFreeze, &g_Config.m_KxTbPistolAutoSwitch, &g_Config.m_KxTbPistolPriority, &g_Config.m_KxTbPistolLatency},
	{&g_Config.m_KxTbShotgunEnabled, &g_Config.m_KxTbShotgunTrigger, &g_Config.m_KxTbShotgunTriggerMode,
		&g_Config.m_KxTbShotgunFov, &g_Config.m_KxTbShotgunRadius,
		&g_Config.m_KxTbShotgunRaycastShow, &g_Config.m_KxTbShotgunRules,
		&g_Config.m_KxTbShotgunTeam, &g_Config.m_KxTbShotgunFriend, &g_Config.m_KxTbShotgunDummy,
		&g_Config.m_KxTbShotgunClanmate, &g_Config.m_KxTbShotgunTargetsFilter, g_Config.m_KxTbShotgunTargetsIds,
		&g_Config.m_KxTbShotgunFreeze, &g_Config.m_KxTbShotgunFngFreeze, &g_Config.m_KxTbShotgunAutoSwitch, &g_Config.m_KxTbShotgunPriority, &g_Config.m_KxTbShotgunLatency},
	{&g_Config.m_KxTbGrenadeEnabled, &g_Config.m_KxTbGrenadeTrigger, &g_Config.m_KxTbGrenadeTriggerMode,
		&g_Config.m_KxTbGrenadeFov, &g_Config.m_KxTbGrenadeRadius,
		&g_Config.m_KxTbGrenadeRaycastShow, &g_Config.m_KxTbGrenadeRules,
		&g_Config.m_KxTbGrenadeTeam, &g_Config.m_KxTbGrenadeFriend, &g_Config.m_KxTbGrenadeDummy,
		&g_Config.m_KxTbGrenadeClanmate, &g_Config.m_KxTbGrenadeTargetsFilter, g_Config.m_KxTbGrenadeTargetsIds,
		&g_Config.m_KxTbGrenadeFreeze, &g_Config.m_KxTbGrenadeFngFreeze, &g_Config.m_KxTbGrenadeAutoSwitch, &g_Config.m_KxTbGrenadePriority, &g_Config.m_KxTbGrenadeLatency},
	{&g_Config.m_KxTbLaserEnabled, &g_Config.m_KxTbLaserTrigger, &g_Config.m_KxTbLaserTriggerMode,
		&g_Config.m_KxTbLaserFov, &g_Config.m_KxTbLaserRadius,
		&g_Config.m_KxTbLaserRaycastShow, &g_Config.m_KxTbLaserRules,
		&g_Config.m_KxTbLaserTeam, &g_Config.m_KxTbLaserFriend, &g_Config.m_KxTbLaserDummy,
		&g_Config.m_KxTbLaserClanmate, &g_Config.m_KxTbLaserTargetsFilter, g_Config.m_KxTbLaserTargetsIds,
		&g_Config.m_KxTbLaserFreeze, &g_Config.m_KxTbLaserFngFreeze, &g_Config.m_KxTbLaserAutoSwitch, &g_Config.m_KxTbLaserPriority, &g_Config.m_KxTbLaserLatency},
	{&g_Config.m_KxTbNinjaEnabled, &g_Config.m_KxTbNinjaTrigger, &g_Config.m_KxTbNinjaTriggerMode,
		&g_Config.m_KxTbNinjaFov, &g_Config.m_KxTbNinjaRadius,
		&g_Config.m_KxTbNinjaRaycastShow, &g_Config.m_KxTbNinjaRules,
		&g_Config.m_KxTbNinjaTeam, &g_Config.m_KxTbNinjaFriend, &g_Config.m_KxTbNinjaDummy,
		&g_Config.m_KxTbNinjaClanmate, &g_Config.m_KxTbNinjaTargetsFilter, g_Config.m_KxTbNinjaTargetsIds,
		&g_Config.m_KxTbNinjaFreeze, &g_Config.m_KxTbNinjaFngFreeze, &g_Config.m_KxTbNinjaAutoSwitch, &g_Config.m_KxTbNinjaPriority, &g_Config.m_KxTbNinjaLatency},
};

// v1.56.255: per-weapon settings persistence. Pulls the saved cvars into the
// weapon structs once at startup (settings.cfg is loaded before the first
// tick), then pushes the structs back to g_Config every tick afterwards, so
// ClickGUI edits are saved to settings.cfg like any other cvar.
void CAimBot::SyncSavedSettings()
{
	static bool s_Init = false;
	if(!s_Init)
	{
		s_Init = true;
		for(int w = 0; w < NUM_WEAPONS_AIM; w++)
		{
			SWeaponSettings &s = m_AimBotWeapons[w];
			const KxAbCvars &c = s_aAbCvars[w];
			s.m_Enabled = *c.pEnabled != 0;
			s.m_Fov = (float)*c.pFov;
			s.m_Radius = (float)*c.pRadius;
			s.m_UseRaycastShow = *c.pRaycastShow != 0;
			s.m_Rules = *c.pRules;
			s.m_UseAngle = *c.pUseAngle != 0;
			s.m_Predict = *c.pPredict != 0;
			s.m_Jitter = (float)*c.pJitter;
			s.m_AimMode = *c.pAimMode;
			s.m_Silent = *c.pSilent != 0;
			s.m_SmoothSpeed = *c.pSmoothSpeed / 10.0f;
			s.m_SmoothCenterSnap = *c.pSmoothCenterSnap != 0;
			s.m_TeamFilter = *c.pTeam;
			s.m_FriendFilter = *c.pFriend;
			s.m_DummyFilter = *c.pDummy;
			s.m_ClanmateFilter = *c.pClanmate;
			s.m_TargetsFilter = *c.pTargetsFilter;
			str_copy(s.m_aTargetsFilter, c.pTargetsIds, sizeof(s.m_aTargetsFilter));
			s.m_FreezeFilter = *c.pFreeze;
			s.m_FngFreeze = *c.pFngFreeze != 0;
			s.m_AutoSwitchWeapon = *c.pAutoSwitch != 0;
			s.m_Priority = *c.pPriority;
		}
		for(int w = 0; w < NUM_WEAPONS_TRIGGER; w++)
		{
			SWeaponSettings &s = m_TriggerBotWeapons[w];
			const KxTbCvars &c = s_aTbCvars[w];
			s.m_Enabled = *c.pEnabled != 0;
			s.m_Trigger = *c.pTrigger;
			s.m_TriggerMode = *c.pTriggerMode;
			s.m_Fov = (float)*c.pFov;
			s.m_Radius = (float)*c.pRadius;
			s.m_UseRaycastShow = *c.pRaycastShow != 0;
			s.m_Rules = *c.pRules;
			s.m_TeamFilter = *c.pTeam;
			s.m_FriendFilter = *c.pFriend;
			s.m_DummyFilter = *c.pDummy;
			s.m_ClanmateFilter = *c.pClanmate;
			s.m_TargetsFilter = *c.pTargetsFilter;
			str_copy(s.m_aTargetsFilter, c.pTargetsIds, sizeof(s.m_aTargetsFilter));
			s.m_FreezeFilter = *c.pFreeze;
			s.m_FngFreeze = *c.pFngFreeze != 0;
			s.m_AutoSwitchWeapon = *c.pAutoSwitch != 0;
			s.m_Priority = *c.pPriority;
			s.m_RandomLatency = *c.pLatency;
		}
		m_AimBotShowFov = g_Config.m_KxAimBotShowFov != 0;
		m_AimBotShowRadius = g_Config.m_KxAimBotShowRadius != 0;
		m_AimBotUseRaycastShow = g_Config.m_KxAimBotRaycastShow != 0;
		m_TriggerBotShowFov = g_Config.m_KxTriggerBotShowFov != 0;
		m_TriggerBotShowRadius = g_Config.m_KxTriggerBotShowRadius != 0;
		m_TriggerBotUseRaycastShow = g_Config.m_KxTriggerBotRaycastShow != 0;
		return;
	}
	for(int w = 0; w < NUM_WEAPONS_AIM; w++)
	{
		const SWeaponSettings &s = m_AimBotWeapons[w];
		const KxAbCvars &c = s_aAbCvars[w];
		*c.pEnabled = s.m_Enabled ? 1 : 0;
		*c.pFov = (int)(s.m_Fov + 0.5f);
		*c.pRadius = (int)(s.m_Radius + 0.5f);
		*c.pRaycastShow = s.m_UseRaycastShow ? 1 : 0;
		*c.pRules = s.m_Rules;
		*c.pUseAngle = s.m_UseAngle ? 1 : 0;
		*c.pPredict = s.m_Predict ? 1 : 0;
		*c.pJitter = (int)(s.m_Jitter + 0.5f);
		*c.pAimMode = s.m_AimMode;
		*c.pSilent = s.m_Silent ? 1 : 0;
		*c.pSmoothSpeed = (int)(s.m_SmoothSpeed * 10.0f + 0.5f);
		*c.pSmoothCenterSnap = s.m_SmoothCenterSnap ? 1 : 0;
		*c.pTeam = s.m_TeamFilter;
		*c.pFriend = s.m_FriendFilter;
		*c.pDummy = s.m_DummyFilter;
		*c.pClanmate = s.m_ClanmateFilter;
		*c.pTargetsFilter = s.m_TargetsFilter;
		str_copy(c.pTargetsIds, s.m_aTargetsFilter, 64);
		*c.pFreeze = s.m_FreezeFilter;
		*c.pFngFreeze = s.m_FngFreeze ? 1 : 0;
		*c.pAutoSwitch = s.m_AutoSwitchWeapon ? 1 : 0;
		*c.pPriority = s.m_Priority;
	}
	for(int w = 0; w < NUM_WEAPONS_TRIGGER; w++)
	{
		const SWeaponSettings &s = m_TriggerBotWeapons[w];
		const KxTbCvars &c = s_aTbCvars[w];
		*c.pEnabled = s.m_Enabled ? 1 : 0;
		*c.pTrigger = s.m_Trigger;
		*c.pTriggerMode = s.m_TriggerMode;
		*c.pFov = (int)(s.m_Fov + 0.5f);
		*c.pRadius = (int)(s.m_Radius + 0.5f);
		*c.pRaycastShow = s.m_UseRaycastShow ? 1 : 0;
		*c.pRules = s.m_Rules;
		*c.pTeam = s.m_TeamFilter;
		*c.pFriend = s.m_FriendFilter;
		*c.pDummy = s.m_DummyFilter;
		*c.pClanmate = s.m_ClanmateFilter;
		*c.pTargetsFilter = s.m_TargetsFilter;
		str_copy(c.pTargetsIds, s.m_aTargetsFilter, 64);
		*c.pFreeze = s.m_FreezeFilter;
		*c.pFngFreeze = s.m_FngFreeze ? 1 : 0;
		*c.pAutoSwitch = s.m_AutoSwitchWeapon ? 1 : 0;
		*c.pPriority = s.m_Priority;
		*c.pLatency = s.m_RandomLatency;
	}
	g_Config.m_KxAimBotShowFov = m_AimBotShowFov ? 1 : 0;
	g_Config.m_KxAimBotShowRadius = m_AimBotShowRadius ? 1 : 0;
	g_Config.m_KxAimBotRaycastShow = m_AimBotUseRaycastShow ? 1 : 0;
	g_Config.m_KxTriggerBotShowFov = m_TriggerBotShowFov ? 1 : 0;
	g_Config.m_KxTriggerBotShowRadius = m_TriggerBotShowRadius ? 1 : 0;
	g_Config.m_KxTriggerBotRaycastShow = m_TriggerBotUseRaycastShow ? 1 : 0;
}

void CAimBot::OnReset()
{
        g_Config.m_KxAimBot = false;
        g_Config.m_KxTriggerBot = false;
        m_LatencyCounter = 0;
        m_PrevFire = false;
        m_PrevHook = false;
        // v1.56.262: drop pending Edge snap restore + Smooth silent glide state.
        m_AimEdgeRestore = false;
        m_SilentSmoothInit = false;
}

float CAimBot::GetWeaponRadius(int Weapon) const
{
        CGameClient *pGame = GameClient();
        const CTuningParams *pT = pGame->m_PredictedWorld.GlobalTuning();
        if(!pT)
                return 300.0f;
        switch(Weapon)
        {
        case WEAPON_HAMMER: return 28.0f * 2.0f;
        case WEAPON_GUN: return pT->m_GunSpeed * pT->m_GunLifetime;
        case WEAPON_SHOTGUN: return pT->m_LaserReach; // NOT ShotgunSpeed*Lifetime
        case WEAPON_GRENADE: return pT->m_GrenadeSpeed * pT->m_GrenadeLifetime;
        case WEAPON_LASER: return pT->m_LaserReach;
        case WEAPON_NINJA: return 280.0f;
        case 6: return pT->m_HookLength; // hook
        default: return 300.0f;
        }
}

void CAimBot::OnUpdate()
{
	// AimBot/TriggerBot logic runs in OnUpdate (BEFORE SnapInput) so silent aim
	// via m_LaserUnfreezeAimActive is applied to m_TargetX/Y in SnapInput.
	// v1.56.255: per-weapon settings persistence (pull once / push every tick).
	SyncSavedSettings();
	if(g_Config.m_KxAimBot)
                UpdateAimBot();
        if(g_Config.m_KxTriggerBot)
                UpdateTriggerBot();
}

void CAimBot::OnRender()
{
        // Render FOV/Radius circles for current weapon (visual only, no logic).
        // Use m_aClients[LocalId].m_RenderPos — the SAME interpolated render position
        // CPlayers::OnRender uses to draw the tee (see players.cpp:299). This keeps the
        // circles glued to the visible tee during movement (no jerk from prediction lag).
        CGameClient *pGame = GameClient();
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0 || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
                return;

        vec2 MyPos = pGame->m_aClients[LocalId].m_RenderPos;
        vec2 LocalAim = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(LocalAim);
        if(length(AimDir) < 0.001f)
                AimDir = vec2(1, 0);

        // Active weapon from predicted world (snapshot doesn't expose it directly).
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        int weapon = pChar ? pChar->GetActiveWeapon() : -1;
        Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

        float alpha = KxLineAlpha(KX_LINE_AIMBOT); // v1.56.108: per-component

        if(g_Config.m_KxAimBot)
        {
                // Active weapon (0-5)
                if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
                {
                        if(m_AimBotShowFov)
                                RenderFov(m_AimBotWeapons[weapon], MyPos, AimDir, alpha);
                        if(m_AimBotShowRadius)
                                RenderRadius(m_AimBotWeapons[weapon], MyPos, AimDir, alpha);
                }
                // Hook (6) — v1.56.248: show FOV/radius ALWAYS when the hook weapon is
                // enabled (previously gated on hook held / AimMode=always).
                if(m_AimBotWeapons[6].m_Enabled)
                {
                        if(m_AimBotShowFov)
                                RenderFov(m_AimBotWeapons[6], MyPos, AimDir, alpha);
                        if(m_AimBotShowRadius)
                                RenderRadius(m_AimBotWeapons[6], MyPos, AimDir, alpha);
                }
        }
        if(g_Config.m_KxTriggerBot && weapon >= 0 && weapon < NUM_WEAPONS_TRIGGER && m_TriggerBotWeapons[weapon].m_Enabled)
        {
                if(m_TriggerBotShowFov)
                        RenderFov(m_TriggerBotWeapons[weapon], MyPos, AimDir, alpha * 0.7f);
                if(m_TriggerBotShowRadius)
                        RenderRadius(m_TriggerBotWeapons[weapon], MyPos, AimDir, alpha * 0.7f);
        }

}

bool CAimBot::PassesFilters(int ClientId, int LocalId, const SWeaponSettings &Settings)
{
        CGameClient *pGame = GameClient();
        if(!pGame->m_aClients[ClientId].m_Active)
                return false;
        if(!pGame->m_Snap.m_aCharacters[ClientId].m_Active)
                return false;
        // Team filter: 0=both 1=my 2=war 3=blue 4=red (v1.56.275).
        // "Team" = REAL team (red/blue); My/War compare real team with ours.
        const int MyRealTeam = pGame->m_aClients[LocalId].m_Team;
        if(Settings.m_TeamFilter == 1)
        {
                if(pGame->m_aClients[ClientId].m_Team != MyRealTeam)
                        return false;
        }
        else if(Settings.m_TeamFilter == 2)
        {
                if(pGame->m_aClients[ClientId].m_Team == MyRealTeam)
                        return false;
        }
        else if(Settings.m_TeamFilter == 3)
        {
                if(pGame->m_aClients[ClientId].m_Team != TEAM_BLUE)
                        return false;
        }
        else if(Settings.m_TeamFilter == 4)
        {
                if(pGame->m_aClients[ClientId].m_Team != TEAM_RED)
                        return false;
        }
        // Friend filter
        if(Settings.m_FriendFilter == 1 && pGame->m_aClients[ClientId].m_Friend)
                return false;
        if(Settings.m_FriendFilter == 2 && !pGame->m_aClients[ClientId].m_Friend)
                return false;
        // Dummy filter
        bool isDummy = false;
        for(int d = 0; d < MAX_DUMMIES; d++)
        {
                if(pGame->m_aLocalIds[d] == ClientId)
                {
                        isDummy = true;
                        break;
                }
        }
        if(Settings.m_DummyFilter == 1 && isDummy)
                return false;
        if(Settings.m_DummyFilter == 2 && !isDummy)
                return false;
        bool isClanmate = str_comp(pGame->m_aClients[ClientId].m_aClan, pGame->m_aClients[LocalId].m_aClan) == 0;
        if(Settings.m_ClanmateFilter == 1 && isClanmate)
                return false;
        if(Settings.m_ClanmateFilter == 2 && !isClanmate)
                return false;
        if(Settings.m_TargetsFilter != 0)
        {
                bool found = false;
                bool anyValid = false;
                const char *pIds = Settings.m_aTargetsFilter;
                while(*pIds)
                {
                        while(*pIds && (*pIds == ' ' || *pIds == ',' || *pIds == ';'))
                                pIds++;
                        if(!*pIds)
                                break;
                        if(*pIds >= '0' && *pIds <= '9')
                        {
                                const int id = atoi(pIds);
                                if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
                                {
                                        anyValid = true;
                                        if(id == ClientId)
                                                found = true;
                                }
                        }
                        while(*pIds && *pIds != ' ' && *pIds != ',' && *pIds != ';')
                                pIds++;
                }
                if(Settings.m_TargetsFilter == 1 && anyValid && !found)
                        return false;
                if(Settings.m_TargetsFilter == 2 && anyValid && found)
                        return false;
        }
        // Freeze filter (LiveFrozen covers servers like FNG that hide the
        // unfreeze tick — same check as the freeze flakes rendering)
        bool isFrozen = pGame->m_aClients[ClientId].m_FreezeEnd != 0 ||
                pGame->m_aClients[ClientId].m_LiveFrozen;
        // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
        if(Settings.m_FngFreeze && pGame->m_Snap.m_aCharacters[ClientId].m_Cur.m_Weapon == WEAPON_NINJA)
                isFrozen = true;
        if(Settings.m_FreezeFilter == 1 && !isFrozen)
                return false;
        if(Settings.m_FreezeFilter == 2 && isFrozen)
                return false;
        return true;
}

int CAimBot::FindTarget(const SWeaponSettings &Settings, int Weapon, int LocalId, const vec2 &MyPos, const vec2 &AimDir, const vec2 &AimPos, vec2 &outAimPoint)
{
        CGameClient *pGame = GameClient();
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        // v1.56.263: keep the sampling/jitter circle strictly INSIDE the real
        // hitbox. Projectiles/lasers hit only when their path passes strictly
        // less than the target's ProximityRadius (= CCharacterCore::PhysicalSize()
        // = 28) from its center, so the old "+2" offset put edge samples and
        // jitter aims ~1 unit OUTSIDE the hitbox and they missed (most visible
        // with lasers). hbR below then lands 1 unit inside the edge, safe
        // against the strict '<' check and float rounding — for ALL weapons.
        constexpr float HB_HITBOX = CCharacterCore::PhysicalSize();

        int bestTarget = -1;
        float bestScore = 1e18f;
        vec2 bestAimPoint = vec2(0, 0);

        // v1.56.196: Real Predict — full physics simulation (CopyWorld + Tick).
        // One CopyWorld per FindTarget call, pre-roll MAX_PREDICT_TICKS ticks
        // ahead, recording each client's position after each tick into a table.
        // Then for each target we walk the table forward one tick at a time:
        //   tick t → dist(MyPos, pos[t]) → flightTime = dist/speed (or dist*50/speed)
        //   if flightTime <= t → projectile caught up between (t-1) and t.
        //     Interpolate the exact sub-tick moment via linear interp of flightTime:
        //       u = (prevFlightTime - (t-1)) / (1 - (flightTime - prevFlightTime))
        //     aim at mix(pos[t-1], pos[t], u).
        // Gravity, friction, wall collisions, and hook physics all apply during
        // the pre-roll — prediction matches real server state much better than
        // linear extrapolation on 10+ tick horizons.
        //
        // Stack usage: positions[64][21] of vec2 = ~10.7 KB. Acceptable.
        CGameWorld *pPredWorld = nullptr;
        vec2 predPositions[MAX_CLIENTS][MAX_PREDICT_TICKS + 1];
        bool predValid[MAX_CLIENTS] = {};
        float projSpeed = 0.0f;
        bool projSpeedPerTick = false; // false = units/sec (gun/grenade), true = units/tick (hook)
        bool predictActive = false;

        if(Settings.m_Predict)
        {
                const CTuningParams *pT = pGame->m_PredictedWorld.GlobalTuning();
                if(pT)
                {
                        switch(Weapon)
                        {
                        // Gun/Grenade speeds are in units/SECOND (used by CalcPos where Time is in seconds).
                        // flightTicks = ceil(dist * TICKS_PER_SEC / speed)
                        case WEAPON_GUN:     projSpeed = pT->m_GunSpeed;     projSpeedPerTick = false; break;
                        case WEAPON_GRENADE: projSpeed = pT->m_GrenadeSpeed; projSpeedPerTick = false; break;
                        // Hook speed is in units/TICK (m_HookPos += HookDir * HookFireSpeed per Tick()).
                        // flightTicks = ceil(dist / speed)
                        case 6:              projSpeed = pT->m_HookFireSpeed; projSpeedPerTick = true; break;
                        default:             break; // instant weapons (hammer/shotgun/laser/ninja) — no predict
                        }
                        if(projSpeed > 1.0f)
                                predictActive = true;
                }
        }

        if(predictActive)
        {
                pPredWorld = new CGameWorld;
                pPredWorld->CopyWorld(&pGame->m_PredictedWorld);

                // Record each client's CURRENT position as tick 0.
                for(int c = 0; c < MAX_CLIENTS; c++)
                {
                        if(CCharacter *pChar = pPredWorld->GetCharacterById(c))
                        {
                                predPositions[c][0] = pChar->Core()->m_Pos;
                                predValid[c] = true;
                        }
                }

                // Pre-roll MAX_PREDICT_TICKS ticks. Each character gets its last known
                // input fed back to it (preserves movement direction, jump state, hook
                // state). This is the same pattern laser_unfreeze uses for its trigger
                // prediction — proven to match real server-side state closely.
                //
                // Per-tick ordering (matches laser_unfreeze exactly):
                //   1. OnDirectInput on all chars (still on previous tick)
                //   2. Advance m_GameTick to the new tick
                //   3. OnPredictedInput on all chars (now on the new tick)
                //   4. World Tick() — applies physics for the new tick
                const int startTick = pPredWorld->GameTick();
                for(int t = 1; t <= MAX_PREDICT_TICKS; t++)
                {
                        // 1. OnDirectInput for all characters (previous tick)
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                CCharacter *pChar = pPredWorld->GetCharacterById(c);
                                if(!pChar)
                                        continue;
                                CNetObj_PlayerInput inp = pChar->Core()->m_Input;
                                pChar->OnDirectInput(&inp);
                        }
                        // 2. Advance tick
                        pPredWorld->m_GameTick = startTick + t;
                        // 3. OnPredictedInput for all characters (new tick)
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                CCharacter *pChar = pPredWorld->GetCharacterById(c);
                                if(!pChar)
                                        continue;
                                CNetObj_PlayerInput inp = pChar->Core()->m_Input;
                                pChar->OnPredictedInput(&inp);
                        }
                        // 4. World Tick — applies physics for this tick
                        pPredWorld->Tick();

                        // Record positions after this tick
                        for(int c = 0; c < MAX_CLIENTS; c++)
                        {
                                if(CCharacter *pChar = pPredWorld->GetCharacterById(c))
                                        predPositions[c][t] = pChar->Core()->m_Pos;
                                else
                                        predValid[c] = false; // character disappeared during sim (death/leave)
                        }
                }
        }

        // v1.56.263: predict-aware jitter re-timing, shared by BOTH jitter modes.
        //
        // The projectile is a STRAIGHT shot to a fixed aim point Cand: it arrives
        // at time T(Cand) = |Cand - MyPos| / speed. For a guaranteed hit the aim
        // must satisfy the fixed point  Cand = pos(T(Cand)) + JOff  — the
        // projectile then passes exactly |JOff| (<= hbR) away from the target
        // center at the moment of arrival. Solved by fixed-point iteration over
        // the pre-rolled position table (sub-tick interpolated):
        //   Cand_{k+1} = pos(|Cand_k - MyPos| / speed) + JOff
        // The old v1.56.249 chase-walk modeled the projectile as RE-AIMING at the
        // moving jittered point every tick — only an approximation of the ballistic
        // straight shot — and the raycast-angle jitter mode had NO timing pass at
        // all, so both could miss a moving target by 1-3 ticks of travel.
        auto PredJitAim = [&](int TargetIdx, const vec2 &SeedCenter, const vec2 &JOff) -> vec2 {
                vec2 Cand = SeedCenter + JOff;
                for(int Iter = 0; Iter < 3; Iter++)
                {
                        float T = length(Cand - MyPos);
                        if(projSpeedPerTick)
                                T = T / projSpeed;
                        else
                                T = T * (float)SERVER_TICK_SPEED / projSpeed;
                        if(T < 0.0f)
                                T = 0.0f;
                        if(T > (float)MAX_PREDICT_TICKS)
                                T = (float)MAX_PREDICT_TICKS;
                        const int t = (int)T;
                        const int tNext = (t < MAX_PREDICT_TICKS) ? t + 1 : t;
                        const float u = T - (float)t;
                        const vec2 NewCand = mix(predPositions[TargetIdx][t], predPositions[TargetIdx][tNext], u) + JOff;
                        if(length(NewCand - Cand) < 0.5f)
                        {
                                Cand = NewCand;
                                break;
                        }
                        Cand = NewCand;
                }
                return Cand;
        };

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                if(!PassesFilters(i, LocalId, Settings))
                        continue;
                vec2 prevPos = vec2((float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_Y);
                vec2 curPos = vec2((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                float intra = pGame->Client()->IntraGameTick(g_Config.m_ClDummy);
                vec2 targetCenter = mix(prevPos, curPos, intra);
                // v1.56.194: Real Predict — replace targetCenter with the predicted
                // position where the projectile will intersect the target. Walks the
                // pre-rolled position table forward one tick at a time.
                if(predictActive && predValid[i])
                {
                        // v1.56.196: Sub-tick interpolation. flightTime is now a FLOAT
                        // (not ceil'd to int). Walk forward through the pre-rolled position
                        // table. Find the first tick t where flightTime <= t — that's where
                        // the projectile catches up with the target. The real intersection
                        // happened BETWEEN tick (t-1) and tick t (sub-tick moment).
                        //
                        // Linear interp of flightTime between (t-1) and t:
                        //   flightTime(u) = prevFlightTime + (flightTime - prevFlightTime) * u
                        //   where u in [0,1] is the fraction from tick (t-1) to tick t
                        // Hook catches up when flightTime(u) == (t-1) + u:
                        //   prevFlightTime + (flightTime - prevFlightTime) * u = (t-1) + u
                        //   => u = (prevFlightTime - (t-1)) / (1 - (flightTime - prevFlightTime))
                        //
                        // Then aim at mix(pos[t-1], pos[t], u) — the target's position at
                        // the exact sub-tick moment of intersection.
                        vec2 predictedPos = predPositions[i][0]; // fallback
                        bool foundPrediction = false;
                        float prevFlightTime = -1.0f;

                        for(int t = 0; t <= MAX_PREDICT_TICKS; t++)
                        {
                                float dist = length(predPositions[i][t] - MyPos);
                                if(dist < 1.0f)
                                {
                                        // Target literally on top of us — aim at current position.
                                        predictedPos = predPositions[i][t];
                                        foundPrediction = true;
                                        break;
                                }

                                // v1.56.195: Hook speed is units/TICK, gun/grenade are units/SECOND.
                                float flightTime;
                                if(projSpeedPerTick)
                                        flightTime = dist / projSpeed;
                                else
                                        flightTime = dist * (float)SERVER_TICK_SPEED / projSpeed;

                                if(flightTime <= (float)t)
                                {
                                        // Projectile caught up between tick (t-1) and tick t (or at t).
                                        if(t == 0 || prevFlightTime < 0.0f)
                                        {
                                                // Caught up on the very first tick — no previous to interp.
                                                predictedPos = predPositions[i][t];
                                        }
                                        else
                                        {
                                                float prevTick = (float)(t - 1);
                                                float numerator = prevFlightTime - prevTick;
                                                float denominator = 1.0f - (flightTime - prevFlightTime);
                                                float u;
                                                if(denominator > 0.001f)
                                                        u = numerator / denominator;
                                                else
                                                        u = 0.5f; // fallback (denominator<=0 shouldn't happen here)
                                                if(u < 0.0f) u = 0.0f;
                                                if(u > 1.0f) u = 1.0f;
                                                predictedPos = mix(predPositions[i][t - 1], predPositions[i][t], u);
                                        }
                                        foundPrediction = true;
                                        break;
                                }

                                prevFlightTime = flightTime;
                        }

                        if(!foundPrediction)
                        {
                                // Never caught up within MAX_PREDICT_TICKS — use last available position.
                                predictedPos = predPositions[i][MAX_PREDICT_TICKS];
                        }
                        targetCenter = predictedPos;
                }

                // v1.56.263: InsertCharacter — a player standing on the line
                // physically stops hooks/hammer/projectiles/lasers (they hook/hit
                // the first player touched), so a target behind another player is
                // unreachable and must not be targeted. Applies to Rules 1+2
                // (wherever the wall insert line applies); Rules = None keeps
                // ignoring everything, as before.
                const bool charBlocked = Settings.m_Rules != 0 &&
                        LineBlockedByCharacter(LocalId, i, MyPos, targetCenter);

                const float hbR = HB_HITBOX - 1.0f;
                const float hbDistToMe = distance(MyPos, targetCenter);
                const float hbToMeAngle = atan2f(MyPos.y - targetCenter.y, MyPos.x - targetCenter.x);
                float hbArc = pi;
                if(hbDistToMe > hbR)
                        hbArc = acosf(hbR / hbDistToMe);
                const int hbSamples = std::max(1, (int)(hbR * 4.0f * hbArc));
                constexpr int HB_MAX_SAMPLES = 384;
                vec2 hbPoints[HB_MAX_SAMPLES];
                for(int k = 0; k < hbSamples; k++)
                {
                        const float a = hbToMeAngle - hbArc + ((k + 0.5f) * 2.0f * hbArc) / (float)hbSamples;
                        hbPoints[k] = targetCenter + vec2(cosf(a), sinf(a)) * hbR;
                }
                const bool hbCenterVisible = (Settings.m_Rules != 1 || IsVisible(MyPos, targetCenter)) && !charBlocked;
                float bestSampleScore = 1e18f;
                bool anyPass = false;
                vec2 sampleAimPoint = targetCenter;
                // v1.56.248: Jitter (raycast-angle mode) — collect ray sample points
                // whose rays HIT the player (visible); a random one is chosen later.
                vec2 aJitHits[HB_MAX_SAMPLES];
                int NumJitHits = 0;
                for(int s = 0; s < hbSamples; s++)
                {
                        vec2 sp = hbPoints[s];
                        vec2 toSample = sp - MyPos;
                        float dist = length(toSample);
                        if(dist > Settings.m_Radius || dist < 1.0f)
                                continue;
                        vec2 dir = normalize(toSample);
                        float sampleAngle = atan2f(dir.y, dir.x);
                        float angleDiff = fabsf(atan2f(sinf(myAngle - sampleAngle), cosf(myAngle - sampleAngle)));
                        if(angleDiff > halfFovRad)
                                continue;
                        bool visible = false;
                        vec2 rayHitPoint = sp;
                        if(Settings.m_Rules == 0)
                        {
                                visible = true;
                        }
                        else if(Settings.m_Rules == 1)
                        {
                                visible = hbCenterVisible;
                                rayHitPoint = targetCenter;
                        }
                        else if(Settings.m_Rules == 2)
                        {
                                visible = IsVisible(MyPos, sp) &&
                                        !LineBlockedByCharacter(LocalId, i, MyPos, sp);
                        }
                        if(visible)
                        {
                                // v1.56.248: Jitter — remember this hitting ray (raycast-angle mode).
                                if(Settings.m_Jitter > 0.0f && Settings.m_UseAngle && Settings.m_Rules == 2 && NumJitHits < HB_MAX_SAMPLES)
                                        aJitHits[NumJitHits++] = sp;
                                float score;
                                if(Settings.m_Priority == 0)
                                        score = angleDiff;
                                else if(Settings.m_Priority == 1)
                                        score = length(AimPos - sp);
                                else
                                        score = dist;
                                if(score < bestSampleScore)
                                {
                                        bestSampleScore = score;
                                        anyPass = true;
                                        sampleAimPoint = Settings.m_UseAngle ? rayHitPoint : targetCenter;
                                }
                        }
                }
                if(anyPass && bestSampleScore < bestScore)
                {
                        bestScore = bestSampleScore;
                        bestTarget = i;
                        bestAimPoint = sampleAimPoint;
                        // v1.56.248: Jitter — randomize the aim point inside the player
                        // hitbox. Jitter scales how much of the hitbox radius is used.
                        if(Settings.m_Jitter > 0.0f)
                        {
                                const float JitScale = Settings.m_Jitter / 100.0f;
                                if(Settings.m_UseAngle && Settings.m_Rules == 2)
                                {
                                        // Raycast-angle mode: random angle among the rays that
                                        // HIT the player, plus a random distance along that ray
                                        // from its closest point to the target center — bounded
                                        // by the hitbox chord so the aim always stays on the tee.
                                        // v1.56.263: with Predict on the chosen ray point is re-timed
                                        // through PredJitAim — it sits up to hbR away from the center,
                                        // which shifts the projectile's arrival by 1-3 ticks; without
                                        // the re-timing the target has already moved on and the shot
                                        // misses a fast-moving target.
                                        if(NumJitHits > 0)
                                        {
                                                const vec2 HitSp = aJitHits[rand() % NumJitHits];
                                                vec2 RayDir = HitSp - MyPos;
                                                const float HitDist = length(RayDir);
                                                if(HitDist > 0.5f)
                                                {
                                                        RayDir = RayDir / HitDist;
                                                        const vec2 ToCenter = targetCenter - MyPos;
                                                        const float Proj = dot(ToCenter, RayDir);
                                                        const float HalfChordSq = hbR * hbR - (length_squared(ToCenter) - Proj * Proj);
                                                        const float HalfChord = HalfChordSq > 0.0f ? sqrtf(HalfChordSq) : 0.0f;
                                                        const float Off = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * HalfChord * JitScale;
                                                        const vec2 RayAim = MyPos + RayDir * (Proj + Off);
                                                        if(predictActive && predValid[i])
                                                        {
                                                                const vec2 JOff = RayAim - targetCenter;
                                                                bestAimPoint = PredJitAim(i, targetCenter, JOff);
                                                        }
                                                        else
                                                                bestAimPoint = RayAim;
                                                }
                                        }
                                }
                                else
                                {
                                        // Normal mode — random offset inside the hitbox with visibility
                                        // retries (the candidate must be visible for Rules != 0, otherwise
                                        // the shot would fly into a wall). If no candidate passes, the
                                        // exact (unjittered) aim is kept.
                                        // v1.56.263: with Predict on the offset is re-timed through
                                        // PredJitAim so the projectile arrives when the target is exactly
                                        // JOff away from its center — an in-hitbox pass instead of the old
                                        // chase approximation that lagged 1-3 ticks behind a fast target.
                                        const vec2 BaseAim = bestAimPoint;
                                        for(int Attempt = 0; Attempt < 4; Attempt++)
                                        {
                                                const float JAng = ((float)rand() / (float)RAND_MAX) * pi * 2.0f;
                                                const float JMag = ((float)rand() / (float)RAND_MAX) * hbR * JitScale;
                                                const vec2 JOff = vec2(cosf(JAng), sinf(JAng)) * JMag;
                                                vec2 Cand = BaseAim + JOff;
                                                if(predictActive && predValid[i])
                                                        Cand = PredJitAim(i, targetCenter, JOff);
                                                if(Settings.m_Rules == 0 || (IsVisible(MyPos, Cand) &&
                                                        !LineBlockedByCharacter(LocalId, i, MyPos, Cand)))
                                                {
                                                        bestAimPoint = Cand;
                                                        break;
                                                }
                                                if(Attempt == 3)
                                                        bestAimPoint = BaseAim;
                                        }
                                }
                        }
                }
        }
        // v1.56.194: Free the predicted world before returning. CopyWorld allocated
        // characters with `new`, so `delete` is correct here (RemoveEntity only unlinks
        // — it does NOT free). See laser_unfreeze Phase 1 comment for the same pattern.
        if(pPredWorld)
                delete pPredWorld;

        outAimPoint = bestAimPoint;
        return bestTarget;
}

bool CAimBot::IsVisible(const vec2 &From, const vec2 &To)
{
        return GameClient()->Collision()->IntersectLineTeleHook(From, To, nullptr, nullptr) == 0;
}

// v1.56.263: InsertCharacter — true when another player's hitbox intersects
// the From→To segment. Hook heads and projectiles physically stop on the
// first player they touch (grab/hit radius = ProximityRadius = PhysicalSize
// = 28), so a target standing behind another player is unreachable.
// Blocker positions use the same interpolated snapshot coords as targets.
bool CAimBot::LineBlockedByCharacter(int LocalId, int TargetId, const vec2 &From, const vec2 &To) const
{
        CGameClient *pGame = GameClient();
        const float BlockRSq = CCharacterCore::PhysicalSize() * CCharacterCore::PhysicalSize();
        const vec2 Dir = To - From;
        const float LenSq = length_squared(Dir);
        const float intra = pGame->Client()->IntraGameTick(g_Config.m_ClDummy);
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId || i == TargetId)
                        continue;
                if(!pGame->m_aClients[i].m_Active || !pGame->m_Snap.m_aCharacters[i].m_Active)
                        continue;
                const vec2 prevPos((float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_Y);
                const vec2 curPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                const vec2 BlockerPos = mix(prevPos, curPos, intra);
                // Closest point of the From→To segment to the blocker's center.
                float t = 0.0f;
                if(LenSq > 0.0001f)
                        t = std::clamp(dot(BlockerPos - From, Dir) / LenSq, 0.0f, 1.0f);
                const vec2 Closest = From + Dir * t;
                if(length_squared(Closest - BlockerPos) < BlockRSq)
                        return true;
        }
        return false;
}

// v1.56.263: "Use raycast (Show)" endpoint — the drawn ray stops on walls
// AND on players, mirroring the targeting raycast physics (a hook/hit lands
// on the first tee in the way, so drawing the ray through it is misleading).
vec2 CAimBot::RayShowEndPoint(const vec2 &From, const vec2 &To) const
{
        vec2 End = To;
        vec2 WallHit;
        if(GameClient()->Collision()->IntersectLineTeleHook(From, To, &WallHit, nullptr) > 0)
                End = WallHit;

        CGameClient *pGame = GameClient();
        const int LocalId = pGame->m_Snap.m_LocalClientId;
        const float BlockRSq = CCharacterCore::PhysicalSize() * CCharacterCore::PhysicalSize();
        const vec2 Dir = End - From;
        const float LenSq = length_squared(Dir);
        const float intra = pGame->Client()->IntraGameTick(g_Config.m_ClDummy);

        float bestT = -1.0f;
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                if(!pGame->m_aClients[i].m_Active || !pGame->m_Snap.m_aCharacters[i].m_Active)
                        continue;
                const vec2 prevPos((float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Prev.m_Y);
                const vec2 curPos((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
                const vec2 BlockerPos = mix(prevPos, curPos, intra);
                float t = 0.0f;
                if(LenSq > 0.0001f)
                        t = std::clamp(dot(BlockerPos - From, Dir) / LenSq, 0.0f, 1.0f);
                if(bestT >= 0.0f && t >= bestT)
                        continue;
                const vec2 Closest = From + Dir * t;
                if(length_squared(Closest - BlockerPos) < BlockRSq)
                        bestT = t;
        }
        if(bestT >= 0.0f)
                return From + Dir * bestT;
        return End;
}

void CAimBot::ApplyAim(int ClientId, const vec2 &MyPos, const SWeaponSettings &Settings, const vec2 &AimPoint)
{
        CGameClient *pGame = GameClient();
        vec2 aimOffset = AimPoint - MyPos;

        // v1.56.262: Smooth mode — glide from the CURRENT aim coordinates toward
        // the target (reference: TAS Smooth Fake Aim). Smooth Speed = px per tick.
        // Center Snap rotates the aim AROUND the player center (m_aMousePos is an
        // offset from the tee, so a polar rotation around (0,0) orbits the tee);
        // without it the aim simply travels straight to the target.
        if(Settings.m_AimMode == 3)
        {
                float speed = Settings.m_SmoothSpeed;
                if(speed < 0.1f)
                        speed = 0.1f;
                // v1.56.267: glide exactly N px per game tick (50 Hz). UpdateAimBot
                // runs per render frame, so the step is applied only on the first
                // call of each game tick. The silent channel below is re-asserted
                // on EVERY call: sends are time-based and can land on a repeat
                // call within the same tick — skipping the channel there dropped
                // the silent aim and sent the REAL aim for 1 tick (visible with
                // Show Server-Side Aim).
                static int s_SmoothLastTick = -1;
                const int CurTick = Client()->GameTick(g_Config.m_ClDummy);
                const bool NewTick = (s_SmoothLastTick != CurTick);
                s_SmoothLastTick = CurTick;
                vec2 cur;
                if(Settings.m_Silent)
                {
                        // Silent Smooth: glide the server-side aim independently — it
                        // starts from the visible crosshair ("from current aim coords").
                        if(!m_SilentSmoothInit)
                        {
                                m_SilentSmoothPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
                                m_SilentSmoothInit = true;
                        }
                        cur = m_SilentSmoothPos;
                }
                else
                        cur = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];

                if(NewTick)
                {
                        m_SmoothPredictPrev = cur;
                        const float dist = length(aimOffset - cur);
                        if(dist <= speed)
                                cur = aimOffset;
                        else if(Settings.m_SmoothCenterSnap && length(cur) > 0.5f && length(aimOffset) > 0.5f)
                        {
                                // Polar glide: rotate along the shortest arc (arc step ~= speed
                                // px) and move the radius toward the target radius by speed px.
                                const float a0 = atan2f(cur.y, cur.x);
                                const float a1 = atan2f(aimOffset.y, aimOffset.x);
                                float dA = atan2f(sinf(a1 - a0), cosf(a1 - a0));
                                const float maxRot = speed / std::max(length(cur), 1.0f);
                                dA = std::clamp(dA, -maxRot, maxRot);
                                float len = length(cur);
                                const float maxLen = speed;
                                len += std::clamp(length(aimOffset) - len, -maxLen, maxLen);
                                cur = vec2(cosf(a0 + dA), sinf(a0 + dA)) * len;
                        }
                        else
                                cur = cur + normalize(aimOffset - cur) * speed;

                        // v1.56.276: Smooth Predict — publish prev/cur for the
                        // render-side interpolation (controls.cpp OnRender).
                        m_SmoothPredictCurr = cur;
                        m_SmoothPredictTick = CurTick;

                        if(Settings.m_Silent)
                                m_SilentSmoothPos = cur;
                        else
                                pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy] = cur;
                }

                if(Settings.m_Silent)
                {
                        pGame->m_Controls.m_LaserUnfreezeAimActive = true;
                        pGame->m_Controls.m_LaserUnfreezeAimOffset = cur;
                }
                return;
        }

        m_SilentSmoothInit = false;

        if(Settings.m_Silent)
        {
                // Silent: do NOT change m_aMousePos (visible crosshair stays where the
                // player aims). Set the silent aim override flag — SnapInput
                // (controls.cpp) will use this offset for m_TargetX/Y right before
                // sending to server. Same channel as Laser Unfreeze silent.
                pGame->m_Controls.m_LaserUnfreezeAimActive = true;
                pGame->m_Controls.m_LaserUnfreezeAimOffset = aimOffset;
                return;
        }

        if(Settings.m_AimMode == 0) // Edge — 1-tick snap, restored on the next tick
        {
                if(!m_AimEdgeRestore)
                {
                        m_AimEdgeRestore = true;
                        m_AimEdgeRestorePos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
                }
                pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy] = aimOffset;
                return;
        }

        // Trigger (1) or Always (2) — move visible crosshair permanently
        // TODO: remove ghost when show for me — visible crosshair moves to
        // aimbot aim, but for a few frames the player's real angle flickers
        // through. Same issue as fake aim showForMe before v1.56.135 sync.
        pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy] = aimOffset;
}

void CAimBot::FireWeapon()
{
        CNetObj_PlayerInput *pInput = &GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
        if((pInput->m_Fire & 1) == 0)
                pInput->m_Fire = (pInput->m_Fire + 1) | 1;
}

void CAimBot::DoHook()
{
        GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy].m_Hook = 1;
}

void CAimBot::UpdateAimBot()
{
        CGameClient *pGame = GameClient();
        // v1.56.262: Edge mode — restore the crosshair snapped last tick FIRST,
        // before any early return, so a pending 1-tick snap can never stick the aim.
        if(m_AimEdgeRestore)
        {
                m_AimEdgeRestore = false;
                pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy] = m_AimEdgeRestorePos;
        }
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0)
                return;
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pChar)
                return;

        int weapon = pChar->GetActiveWeapon();
        const CNetObj_PlayerInput &Input = pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
        bool fire = (Input.m_Fire & 1) != 0;
        bool hook = Input.m_Hook != 0;
        vec2 MyPos = pChar->Core()->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos);
        if(length(AimDir) < 0.001f)
                AimDir = vec2(1, 0);

        // v1.56.276: Auto Switch Weapon — if the held weapon's aimbot has no
        // target in its zone, switch to the nearest enabled weapon with Auto
        // Switch on that does have one (hook can't be switched to, so only
        // weapons 0-5 are candidates).
        {
                bool curHasTarget = false;
                if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
                {
                        vec2 curPt;
                        curHasTarget = FindTarget(m_AimBotWeapons[weapon], weapon, LocalId, MyPos, AimDir, AimPos, curPt) >= 0;
                }
                if(!curHasTarget)
                {
                        int bestWeapon = -1;
                        float bestDistSq = 0.0f;
                        for(int w = 0; w < NUM_WEAPONS_AIM - 1; w++)
                        {
                                if(!m_AimBotWeapons[w].m_Enabled || !m_AimBotWeapons[w].m_AutoSwitchWeapon)
                                        continue;
                                vec2 pt;
                                const int t = FindTarget(m_AimBotWeapons[w], w, LocalId, MyPos, AimDir, AimPos, pt);
                                if(t < 0)
                                        continue;
                                const float distSq = length_squared(pGame->m_aClients[t].m_RenderPos - MyPos);
                                if(bestWeapon < 0 || distSq < bestDistSq)
                                {
                                        bestWeapon = w;
                                        bestDistSq = distSq;
                                }
                        }
                        if(bestWeapon >= 0)
                                pGame->m_Controls.m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = bestWeapon + 1;
                }
        }

        // Edge (0) and Trigger (1) both trigger on willFire — 1-tick CopyWorld
        // prediction (same technique as fake aim in controls.cpp). This catches
        // every real shot (including auto-fire during reload), not just the rising
        // edge of m_Fire. Edge snaps the crosshair for exactly 1 tick (ApplyAim
        // schedules the restore), Trigger moves it permanently. Always (2) and
        // Smooth (3) run every tick; Smooth glides the crosshair toward the target.
        // Silent (separate toggle) routes the aim through the invisible silent
        // channel instead of the visible crosshair — orthogonal to the Mode.
        // Hook (weapon 6) has no reload cadence, so Edge/Trigger use edge on m_Hook.
        bool willFire = false;
        bool needWillFire = weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1
                && m_AimBotWeapons[weapon].m_Enabled
                && (m_AimBotWeapons[weapon].m_AimMode == 0 || m_AimBotWeapons[weapon].m_AimMode == 1);
        if(needWillFire)
        {
                int tickBefore = pChar->GetAttackTick();
                CGameWorld FutureWorld;
                FutureWorld.CopyWorld(&pGame->m_PredictedWorld);
                if(CCharacter *pFuture = FutureWorld.GetCharacterById(LocalId))
                {
                        CNetObj_PlayerInput FutureInput = Input;
                        pFuture->OnDirectInput(&FutureInput);
                        FutureWorld.m_GameTick = FutureWorld.GameTick() + 1;
                        pFuture->OnPredictedInput(&FutureInput);
                        FutureWorld.Tick();
                        if(CCharacter *pAfter = FutureWorld.GetCharacterById(LocalId))
                        {
                                if(pAfter->GetAttackTick() != tickBefore)
                                        willFire = true;
                        }
                }
        }

        bool SmoothSilentApplied = false;

        // Active weapon (0-5)
        if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
        {
                const SWeaponSettings &s = m_AimBotWeapons[weapon];
                // Mode: Edge/Trigger use willFire; Always/Smooth run every tick.
                bool trigger = (s.m_AimMode >= 2) ? true : willFire;
                if(trigger)
                {
                        vec2 aimPt; int t = FindTarget(s, weapon, LocalId, MyPos, AimDir, AimPos, aimPt);
                        if(t >= 0)
                        {
                                ApplyAim(t, MyPos, s, aimPt);
                                if(s.m_AimMode == 3 && s.m_Silent)
                                        SmoothSilentApplied = true;
                        }
                }
        }
        // Hook (6): Edge/Trigger use hook edge (hook has no reload cadence); Always/Smooth = every tick
        if(m_AimBotWeapons[6].m_Enabled)
        {
                const SWeaponSettings &s = m_AimBotWeapons[6];
                bool trigger = (s.m_AimMode >= 2) ? true : (hook && !m_PrevHook);
                if(trigger)
                {
                        vec2 aimPt; int t = FindTarget(s, 6, LocalId, MyPos, AimDir, AimPos, aimPt);
                        if(t >= 0)
                        {
                                ApplyAim(t, MyPos, s, aimPt);
                                if(s.m_AimMode == 3 && s.m_Silent)
                                        SmoothSilentApplied = true;
                        }
                }
        }
        // v1.56.266: silent Smooth glides a VIRTUAL aim — restart it from the aim
        // actually sent to the server whenever the glide is interrupted (target
        // left FOV, weapon disabled, no trigger), so re-acquisition starts from
        // the current real aim instead of a stale virtual position.
        if(!SmoothSilentApplied)
                m_SilentSmoothInit = false;
        m_PrevFire = fire;
        m_PrevHook = hook;
}

void CAimBot::UpdateTriggerBot()
{
        CGameClient *pGame = GameClient();
        int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0)
                return;
        CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pChar)
                return;
        int weapon = pChar->GetActiveWeapon();
        vec2 MyPos = pChar->Core()->m_Pos;
        vec2 AimPos = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
        vec2 AimDir = normalize(AimPos);
        if(length(AimDir) < 0.001f)
                AimDir = vec2(1, 0);

        // v1.56.276: Auto Switch Weapon — if the held weapon's triggerbot has no
        // target in its zone, switch to the nearest enabled weapon with Auto
        // Switch on that does have one. Runs BEFORE the held-weapon gates so it
        // also works while the current weapon's triggerbot is disabled.
        {
                bool curHasTarget = false;
                if(weapon >= 0 && weapon < NUM_WEAPONS_TRIGGER && m_TriggerBotWeapons[weapon].m_Enabled)
                {
                        vec2 curPt;
                        curHasTarget = FindTarget(m_TriggerBotWeapons[weapon], weapon, LocalId, MyPos, AimDir, AimPos, curPt) >= 0;
                }
                if(!curHasTarget)
                {
                        int bestWeapon = -1;
                        float bestDistSq = 0.0f;
                        for(int w = 0; w < NUM_WEAPONS_TRIGGER; w++)
                        {
                                if(!m_TriggerBotWeapons[w].m_Enabled || !m_TriggerBotWeapons[w].m_AutoSwitchWeapon)
                                        continue;
                                vec2 pt;
                                const int t = FindTarget(m_TriggerBotWeapons[w], w, LocalId, MyPos, AimDir, AimPos, pt);
                                if(t < 0)
                                        continue;
                                const float distSq = length_squared(pGame->m_aClients[t].m_RenderPos - MyPos);
                                if(bestWeapon < 0 || distSq < bestDistSq)
                                {
                                        bestWeapon = w;
                                        bestDistSq = distSq;
                                }
                        }
                        if(bestWeapon >= 0)
                                pGame->m_Controls.m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = bestWeapon + 1;
                }
        }

        if(weapon < 0 || weapon >= NUM_WEAPONS_TRIGGER)
                return;
        if(!m_TriggerBotWeapons[weapon].m_Enabled)
                return;

        const SWeaponSettings &s = m_TriggerBotWeapons[weapon];
        vec2 aimPtTb; int target = FindTarget(s, weapon, LocalId, MyPos, AimDir, AimPos, aimPtTb);
        bool targetInZone = (target >= 0);

        // Random latency: delay the trigger reaction by a random number of ticks.
        // Counter counts down while target is in zone; when it hits 0, trigger fires.
        if(targetInZone && s.m_RandomLatency > 0)
        {
                if(m_LatencyCounter <= 0)
                        m_LatencyCounter = (rand() % (s.m_RandomLatency + 1));
                if(m_LatencyCounter > 0)
                {
                        m_LatencyCounter--;
                        // While waiting, treat as "not yet triggered" — but still mark active
                        // so one_tick edge doesn't fire prematurely on the next tick.
                        m_TriggerBotWasActive = true;
                        return;
                }
        }

        CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
        int curTick = Client()->GameTick(g_Config.m_ClDummy);

        // one_tick: release the trigger on the tick AFTER it was pressed (exactly 1 tick hold).
        // Done BEFORE the targetInZone check so it runs regardless of current target state.
        if(m_TriggerBotReleaseTick >= 0 && curTick >= m_TriggerBotReleaseTick)
        {
                if(s.m_Trigger == 0)
                        pInput->m_Fire &= ~1;
                else
                        pInput->m_Hook = 0;
                m_TriggerBotReleaseTick = -1;
        }

        if(targetInZone)
        {
                // TriggerMode:
                // 0=one_tick  — press trigger for exactly 1 tick (only on edge: target just entered zone),
                //               then release on the next tick. Don't press again until target leaves and re-enters.
                // 1=hold      — hold trigger while target in zone, release when it leaves (good for guns)
                // 2=every_tick — press trigger every tick (spam edge) while target in zone (good for hammer)
                if(s.m_TriggerMode == 0) // one_tick
                {
                        if(!m_TriggerBotWasActive) // edge: target just entered zone
                        {
                                if(s.m_Trigger == 0)
                                        FireWeapon();
                                else
                                        DoHook();
                                m_TriggerBotReleaseTick = curTick + 1; // release on next tick
                        }
                }
                else if(s.m_TriggerMode == 1) // hold
                {
                        if(s.m_Trigger == 0)
                                pInput->m_Fire |= 1;
                        else
                                pInput->m_Hook = 1;
                }
                else // every_tick (2) — spam: force a new edge every tick
                {
                        if(s.m_Trigger == 0)
                                pInput->m_Fire = (pInput->m_Fire + 1) | 1; // increment + set bit = new edge
                        else
                                pInput->m_Hook = (pInput->m_Hook == 0) ? 1 : 0; // toggle for hook edge spam
                }
                m_TriggerBotWasActive = true;
        }
        else
        {
                // Target left zone — release for hold/every_tick modes.
                // one_tick already released via m_TriggerBotReleaseTick (no action needed here).
                if(s.m_TriggerMode != 0)
                {
                        // Only release if we were active (don't clobber player's own input).
                        if(m_TriggerBotWasActive)
                        {
                                if(s.m_Trigger == 0)
                                        pInput->m_Fire &= ~1;
                                else
                                        pInput->m_Hook = 0;
                        }
                }
                m_TriggerBotWasActive = false;
                m_LatencyCounter = 0;
        }
}

void CAimBot::RenderFov(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha)
{
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float radius = Settings.m_Radius;
        int LineSize = KxLineSize(KX_LINE_AIMBOT);
        float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
        bool useRay = Settings.m_UseRaycastShow;

        // Build line segments. Each segment is a pair (p0, p1).
        std::vector<std::pair<vec2, vec2>> segments;
        if(useRay)
        {
                // v1.56.92: Show only 2 edge lines (left + right) with wall collision.
                // The arc (radius circle) is drawn by RenderRadius with N raycast points.
                float leftAngle = myAngle - halfFovRad;
                float rightAngle = myAngle + halfFovRad;
                // v1.56.263: show rays stop on walls AND players (same blocking
                // physics as the targeting raycast) — no more rays drawn through tees.
                const vec2 leftEnd = RayShowEndPoint(MyPos, MyPos + vec2(cosf(leftAngle), sinf(leftAngle)) * radius);
                const vec2 rightEnd = RayShowEndPoint(MyPos, MyPos + vec2(cosf(rightAngle), sinf(rightAngle)) * radius);
                segments.push_back({MyPos, leftEnd});
                segments.push_back({MyPos, rightEnd});
        }
        else
        {
                segments.push_back({MyPos, MyPos + vec2(cosf(myAngle - halfFovRad), sinf(myAngle - halfFovRad)) * radius});
                segments.push_back({MyPos, MyPos + vec2(cosf(myAngle + halfFovRad), sinf(myAngle + halfFovRad)) * radius});
        }

        // v1.56.104: Layer > 0 → defer to CKinetixLines (renders on top of fg blocks).
        if(g_Config.m_KxLineRenderingLayer > 0)
        {
                float hw = LineSize > 0 ? HalfWidth : 0.0f;
                for(size_t i = 0; i < segments.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        c.a = Alpha;
                        KinetixEnqueueLine(segments[i].first, segments[i].second, c, hw);
                }
                return;
        }

        Graphics()->TextureClear();
        if(LineSize > 0)
        {
                Graphics()->QuadsBegin();
                for(size_t i = 0; i < segments.size(); i++)
                {
                        vec2 p0 = segments[i].first;
                        vec2 p1 = segments[i].second;
                        vec2 dir = normalize(p1 - p0);
                        if(length(dir) < 0.001f)
                                continue;
                        vec2 perp = vec2(dir.y, -dir.x) * HalfWidth;
                        IGraphics::CFreeformItem q(
                                p0.x - perp.x, p0.y - perp.y, p0.x + perp.x, p0.y + perp.y,
                                p1.x - perp.x, p1.y - perp.y, p1.x + perp.x, p1.y + perp.y);
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&q, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                for(size_t i = 0; i < segments.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        IGraphics::CLineItem Line(segments[i].first.x, segments[i].first.y, segments[i].second.x, segments[i].second.y);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

void CAimBot::RenderRadius(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha)
{
        float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
        float myAngle = atan2f(AimDir.y, AimDir.x);
        float radius = Settings.m_Radius;
        int LineSize = KxLineSize(KX_LINE_AIMBOT);
        float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
        bool useRay = Settings.m_UseRaycastShow;
        int segments = useRay ? 64 : 48;

        std::vector<vec2> points;
        for(int i = 0; i <= segments; i++)
        {
                float t = (float)i / (float)segments;
                float a = myAngle - halfFovRad + t * (2.0f * halfFovRad);
                vec2 p = MyPos + vec2(cosf(a), sinf(a)) * radius;
                if(useRay)
                {
                        // v1.56.263: stop arc rays on players too, not just walls.
                        p = RayShowEndPoint(MyPos, p);
                }
                points.push_back(p);
        }

        // v1.56.104: Layer > 0 → defer to CKinetixLines (renders on top of fg blocks).
        if(g_Config.m_KxLineRenderingLayer > 0)
        {
                float hw = LineSize > 0 ? HalfWidth : 0.0f;
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        c.a = Alpha;
                        KinetixEnqueueLine(points[i], points[i + 1], c, hw);
                }
                return;
        }

        Graphics()->TextureClear();
        if(LineSize > 0)
        {
                Graphics()->QuadsBegin();
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        vec2 p0 = points[i];
                        vec2 p1 = points[i + 1];
                        vec2 dir = normalize(p1 - p0);
                        if(length(dir) < 0.001f)
                                continue;
                        vec2 perp = vec2(dir.y, -dir.x) * HalfWidth;
                        IGraphics::CFreeformItem q(
                                p0.x - perp.x, p0.y - perp.y, p0.x + perp.x, p0.y + perp.y,
                                p1.x - perp.x, p1.y - perp.y, p1.x + perp.x, p1.y + perp.y);
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&q, 1);
                }
                Graphics()->QuadsEnd();
        }
        else
        {
                Graphics()->LinesBegin();
                for(size_t i = 0; i + 1 < points.size(); i++)
                {
                        // v1.56.210: per-segment color when gradient is on.
                        ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_AIMBOT, (int)i), true);
                        Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
                        IGraphics::CLineItem Line(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
                        Graphics()->LinesDraw(&Line, 1);
                }
                Graphics()->LinesEnd();
        }
}

// v1.56.196: ComputePredictedAimPoint removed. Real predict now lives inline
// in FindTarget — one CopyWorld + pre-roll of MAX_PREDICT_TICKS ticks builds a
// positions[MAX_CLIENTS][MAX_PREDICT_TICKS+1] table, then linear tick-scan with
// sub-tick interpolation (linear interp of flightTime between adjacent ticks).

void CAimBot::ConAimBot(IConsole::IResult *pResult, void *pUserData)
{
        (void)pUserData;
        g_Config.m_KxAimBot = pResult->GetInteger(0) != 0;
}

void CAimBot::ConTriggerBot(IConsole::IResult *pResult, void *pUserData)
{
        (void)pUserData;
        g_Config.m_KxTriggerBot = pResult->GetInteger(0) != 0;
}
