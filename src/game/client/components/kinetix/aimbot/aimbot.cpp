#include <game/client/components/kinetix/aimbot/aimbot.h>

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>
#include <base/vmath.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <game/client/gameclient.h>
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
	Console()->Register("kx_aimbot", "i[0/1]", CFGFLAG_CLIENT, ConAimBot, this, "AimBot: 1=on 0=off");
	Console()->Register("kx_triggerbot", "i[0/1]", CFGFLAG_CLIENT, ConTriggerBot, this, "TriggerBot: 1=on 0=off");
}

void CAimBot::OnReset()
{
	m_AimBotEnabled = false;
	m_TriggerBotEnabled = false;
	m_LatencyCounter = 0;
	m_PrevFire = false;
	m_PrevHook = false;
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

void CAimBot::OnRender()
{
	Update();
}

void CAimBot::Update()
{
	if(m_AimBotEnabled)
		UpdateAimBot();
	if(m_TriggerBotEnabled)
		UpdateTriggerBot();

	// Render FOV/Radius circles for current weapon
	CGameClient *pGame = GameClient();
	int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return;
	CCharacter *pChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pChar)
		return;

	vec2 MyPos = pChar->Core()->m_Pos;
	vec2 LocalAim = pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	vec2 AimDir = normalize(LocalAim);
	if(length(AimDir) < 0.001f)
		AimDir = vec2(1, 0);

	int weapon = pChar->GetActiveWeapon();
	bool hookHeld = pGame->m_Controls.m_aInputData[g_Config.m_ClDummy].m_Hook != 0;
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

	float alpha = (float)g_Config.m_KxTrajectoryAlpha / 100.0f;

	if(m_AimBotEnabled)
	{
		// Active weapon (0-5)
		if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
		{
			if(m_AimBotShowFov)
				RenderFov(m_AimBotWeapons[weapon], MyPos, AimDir, alpha);
			if(m_AimBotShowRadius)
				RenderRadius(m_AimBotWeapons[weapon], MyPos, AimDir, alpha);
		}
		// Hook (6) — only show if hook held or AimMode=always
		if(m_AimBotWeapons[6].m_Enabled && (hookHeld || m_AimBotWeapons[6].m_AimMode == 2))
		{
			if(m_AimBotShowFov)
				RenderFov(m_AimBotWeapons[6], MyPos, AimDir, alpha);
			if(m_AimBotShowRadius)
				RenderRadius(m_AimBotWeapons[6], MyPos, AimDir, alpha);
		}
	}
	if(m_TriggerBotEnabled && weapon >= 0 && weapon < NUM_WEAPONS_TRIGGER && m_TriggerBotWeapons[weapon].m_Enabled)
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
	// Team filter
	if(Settings.m_TeamFilter == 1)
	{
		if(pGame->m_Teams.SameTeam(LocalId, ClientId))
			return false;
	}
	else if(Settings.m_TeamFilter == 2)
	{
		if(!pGame->m_Teams.SameTeam(LocalId, ClientId))
			return false;
	}
	// Friend filter
	if(Settings.m_FriendFilter == 1 && pGame->m_aClients[ClientId].m_Friend)
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
	// Freeze filter
	bool isFrozen = pGame->m_aClients[ClientId].m_FreezeEnd != 0;
	if(Settings.m_FreezeFilter == 1 && !isFrozen)
		return false;
	if(Settings.m_FreezeFilter == 2 && isFrozen)
		return false;
	return true;
}

int CAimBot::FindTarget(const SWeaponSettings &Settings, int LocalId, const vec2 &MyPos, const vec2 &AimDir)
{
	CGameClient *pGame = GameClient();
	float myAngle = atan2f(AimDir.y, AimDir.x);
	float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
	int bestTarget = -1;
	float bestScore = 1e18f;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId)
			continue;
		if(!PassesFilters(i, LocalId, Settings))
			continue;
		vec2 targetPos = vec2((float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[i].m_Cur.m_Y);
		vec2 toTarget = targetPos - MyPos;
		float dist = length(toTarget);
		if(dist > Settings.m_Radius || dist < 1.0f)
			continue;
		vec2 dir = normalize(toTarget);
		float targetAngle = atan2f(dir.y, dir.x);
		float angleDiff = fabsf(atan2f(sinf(myAngle - targetAngle), cosf(myAngle - targetAngle)));
		if(angleDiff > halfFovRad)
			continue;
		if(Settings.m_UseRaycast && !IsVisible(MyPos, targetPos))
			continue;
		float score;
		if(Settings.m_Priority == 0)
			score = angleDiff; // nearest FOV
		else if(Settings.m_Priority == 1)
			score = angleDiff * 100.0f + dist * 0.01f; // nearest aim
		else
			score = dist; // nearest to me
		if(score < bestScore)
		{
			bestScore = score;
			bestTarget = i;
		}
	}
	return bestTarget;
}

bool CAimBot::IsVisible(const vec2 &From, const vec2 &To)
{
	return GameClient()->Collision()->IntersectLine(From, To, nullptr, nullptr) == 0;
}

void CAimBot::ApplyAim(int ClientId, const vec2 &MyPos, const SWeaponSettings &Settings)
{
	CGameClient *pGame = GameClient();
	vec2 targetPos = vec2((float)pGame->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, (float)pGame->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
	vec2 aimOffset = targetPos - MyPos;
	// Move the visible crosshair directly (silent mode would route through LaserUnfreeze
	// channel, but for simplicity we always move m_aMousePos — visible to the player).
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
	vec2 AimDir = normalize(pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy]);
	if(length(AimDir) < 0.001f)
		AimDir = vec2(1, 0);

	// AimMode==1 (trigger) on weapons 0-5: predict whether the weapon will fire THIS tick
	// via 1-tick CopyWorld prediction. This catches every real shot (including auto-fire
	// during reload), not just the rising edge of m_Fire. Same technique as fake aim
	// (controls.cpp). Hook (weapon 6) has no reload cadence, so edge detection is enough.
	bool willFire = false;
	if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1
		&& m_AimBotWeapons[weapon].m_Enabled
		&& m_AimBotWeapons[weapon].m_AimMode == 1)
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

	// Active weapon (0-5): triggered by fire
	if(weapon >= 0 && weapon < NUM_WEAPONS_AIM - 1 && m_AimBotWeapons[weapon].m_Enabled)
	{
		const SWeaponSettings &s = m_AimBotWeapons[weapon];
		// AimMode: 0=silent(edge on m_Fire), 1=trigger(willFire via CopyWorld), 2=always
		bool trigger;
		if(s.m_AimMode == 2)
			trigger = true;
		else if(s.m_AimMode == 1)
			trigger = willFire;
		else
			trigger = (fire && !m_PrevFire);
		if(trigger)
		{
			int t = FindTarget(s, LocalId, MyPos, AimDir);
			if(t >= 0)
				ApplyAim(t, MyPos, s);
		}
	}
	// Hook (6): triggered by hook edge (hook has no reload cadence, edge is sufficient)
	if(m_AimBotWeapons[6].m_Enabled)
	{
		const SWeaponSettings &s = m_AimBotWeapons[6];
		// AimMode 0 (silent) and 1 (trigger) both use edge on m_Hook; 2 = always
		bool trigger = (s.m_AimMode == 2) ? true : (hook && !m_PrevHook);
		if(trigger)
		{
			int t = FindTarget(s, LocalId, MyPos, AimDir);
			if(t >= 0)
				ApplyAim(t, MyPos, s);
		}
	}
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
	if(weapon < 0 || weapon >= NUM_WEAPONS_TRIGGER)
		return;
	if(!m_TriggerBotWeapons[weapon].m_Enabled)
		return;

	const SWeaponSettings &s = m_TriggerBotWeapons[weapon];
	vec2 MyPos = pChar->Core()->m_Pos;
	vec2 AimDir = normalize(pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy]);
	if(length(AimDir) < 0.001f)
		AimDir = vec2(1, 0);

	int target = FindTarget(s, LocalId, MyPos, AimDir);
	if(target >= 0)
	{
		// TriggerBot does NOT aim — only fires/hooks
		if(s.m_RandomLatency > 0)
		{
			if(m_LatencyCounter <= 0)
				m_LatencyCounter = (rand() % (s.m_RandomLatency + 1));
			if(m_LatencyCounter > 0)
			{
				m_LatencyCounter--;
				return;
			}
		}
		if(s.m_Trigger == 0)
			FireWeapon();
		else
			DoHook();
	}
	else
	{
		m_LatencyCounter = 0;
	}
}

void CAimBot::RenderFov(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha)
{
	float halfFovRad = (Settings.m_Fov * 0.5f) * (pi / 180.0f);
	float myAngle = atan2f(AimDir.y, AimDir.x);
	float radius = Settings.m_Radius;
	ColorRGBA col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_KxTrajectoryColor, true));
	int LineSize = g_Config.m_KxTrajectoryLineSize;
	float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
	bool useRay = m_AimBotUseRaycastShow || m_TriggerBotUseRaycastShow;

	std::vector<vec2> points;
	points.push_back(MyPos);
	if(useRay)
	{
		int angles = std::max(1, std::min(64, Settings.m_RaycastAngles));
		float leftAngle = myAngle - halfFovRad;
		float rightAngle = myAngle + halfFovRad;
		for(int i = 0; i <= angles; i++)
		{
			float t = (float)i / (float)angles;
			float a = leftAngle + t * (rightAngle - leftAngle);
			vec2 end = MyPos + vec2(cosf(a), sinf(a)) * radius;
			vec2 hitPos;
			if(GameClient()->Collision()->IntersectLine(MyPos, end, &hitPos, nullptr) > 0)
				points.push_back(hitPos);
			else
				points.push_back(end);
		}
	}
	else
	{
		points.push_back(MyPos + vec2(cosf(myAngle - halfFovRad), sinf(myAngle - halfFovRad)) * radius);
		points.push_back(MyPos);
		points.push_back(MyPos + vec2(cosf(myAngle + halfFovRad), sinf(myAngle + halfFovRad)) * radius);
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
			Graphics()->SetColor(col.r, col.g, col.b, Alpha);
			Graphics()->QuadsDrawFreeform(&q, 1);
		}
		Graphics()->QuadsEnd();
	}
	else
	{
		Graphics()->LinesBegin();
		Graphics()->SetColor(col.r, col.g, col.b, Alpha);
		for(size_t i = 0; i + 1 < points.size(); i++)
		{
			IGraphics::CLineItem Line(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
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
	ColorRGBA col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_KxTrajectoryColor, true));
	int LineSize = g_Config.m_KxTrajectoryLineSize;
	float HalfWidth = 0.5f + (float)(LineSize > 0 ? LineSize - 1 : 0) * 0.25f;
	bool useRay = m_AimBotUseRaycastShow || m_TriggerBotUseRaycastShow;
	int segments = 48;

	std::vector<vec2> points;
	for(int i = 0; i <= segments; i++)
	{
		float t = (float)i / (float)segments;
		float a = myAngle - halfFovRad + t * (2.0f * halfFovRad);
		vec2 p = MyPos + vec2(cosf(a), sinf(a)) * radius;
		if(useRay)
		{
			vec2 hit;
			if(GameClient()->Collision()->IntersectLine(MyPos, p, &hit, nullptr) > 0)
				p = hit;
		}
		points.push_back(p);
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
			Graphics()->SetColor(col.r, col.g, col.b, Alpha);
			Graphics()->QuadsDrawFreeform(&q, 1);
		}
		Graphics()->QuadsEnd();
	}
	else
	{
		Graphics()->LinesBegin();
		Graphics()->SetColor(col.r, col.g, col.b, Alpha);
		for(size_t i = 0; i + 1 < points.size(); i++)
		{
			IGraphics::CLineItem Line(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
			Graphics()->LinesDraw(&Line, 1);
		}
		Graphics()->LinesEnd();
	}
}

void CAimBot::ConAimBot(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CAimBot *>(pUserData)->m_AimBotEnabled = pResult->GetInteger(0) != 0;
}

void CAimBot::ConTriggerBot(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CAimBot *>(pUserData)->m_TriggerBotEnabled = pResult->GetInteger(0) != 0;
}
