// (c) Kinetix. TAS component implementation — see tas.h for the design.

#include <game/client/components/kinetix/tas.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <base/dbg.h>
#include <base/mem.h>

#include <engine/client.h>
#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/collision.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/laser_data.h>
#include <game/client/pickup_data.h>
#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/pickup.h>
#include <game/client/prediction/entities/projectile.h>
#include <game/client/render.h>

#include <algorithm>
#include <utility>
#include <cstdlib>
#include <string.h>

#include <zlib.h>

#include <base/fs.h>
#include <base/io.h>
#include <base/time.h>

#include <engine/storage.h>

void CTas::OnReset()
{
	LeavePlayground();
	m_vRecord.clear();
	m_vSaved.clear();
	m_Tps = 50.0f;
	m_Paused = false;
	m_Mode = 0;
	m_RealPlaying = false;
	m_RealIdx = 0;
	m_RecordStart = CTasState{};
	m_SavedStart = CTasState{};
	m_TakeFromFile = false;
	UnpickFile();
	g_Config.m_KxTasRecord = 0;
	g_Config.m_KxTasPause = 0;
	m_CvarRecordShadow = 0;
	m_CvarPauseShadow = 0;
}

static void ConTasForward(IConsole::IResult *pResult, void *pUserData)
{
	((CTas *)pUserData)->StepForward();
}

static void ConTasRewind(IConsole::IResult *pResult, void *pUserData)
{
	((CTas *)pUserData)->StepRewind();
}

void CTas::OnConsoleInit()
{
	Console()->Register("kx_tas_forward", "", CFGFLAG_CLIENT, ConTasForward, this, "TAS: step one tick forward");
	Console()->Register("kx_tas_rewind", "", CFGFLAG_CLIENT, ConTasRewind, this, "TAS: step one tick backward");
}

void CTas::OnUpdate()
{
	if(g_Config.m_KxTasRecord != m_CvarRecordShadow)
	{
		m_CvarRecordShadow = g_Config.m_KxTasRecord;
		if((m_CvarRecordShadow != 0) != m_Recording)
			ToggleRecord();
	}
	if(g_Config.m_KxTasPause != m_CvarPauseShadow)
	{
		m_CvarPauseShadow = g_Config.m_KxTasPause;
		SetPaused(m_CvarPauseShadow != 0);
	}
	g_Config.m_KxTasRecord = m_Recording ? 1 : 0;
	m_CvarRecordShadow = g_Config.m_KxTasRecord;
	g_Config.m_KxTasPause = m_Paused ? 1 : 0;
	m_CvarPauseShadow = g_Config.m_KxTasPause;
	if(!m_Joined)
		return;

	const int ClientState = Client()->State();
	if(ClientState != IClient::STATE_ONLINE && ClientState != IClient::STATE_DEMOPLAYBACK)
	{
		LeavePlayground();
		return;
	}

	// Map changed underneath us — the playground's collision pointer would be
	// stale, so drop the playground (the recordings stay in memory).
	if(GameClient()->m_PredictedWorld.Collision() != m_Playground.Collision())
	{
		LeavePlayground();
		return;
	}

	// Step on the client's SMOOTH prediction clock, every frame: the clock
	// is read as a float (PredGameTick + PredIntraGameTick fraction), scaled
	// by TPS and accumulated per frame — a step happens in the very same
	// pass where the fraction crosses 1, so the interpolation fraction
	// (m_TickAccum) never clamps at 1 and the tee never freezes between
	// steps. (Driving the steps off the integer PredGameTick made the pair
	// swap lag behind the alpha crossing: the tee stalled for a few frames,
	// then jumped — visible as slight jitter.)
	const float PredClock = (float)Client()->PredGameTick(g_Config.m_ClDummy) +
		Client()->PredIntraGameTick(g_Config.m_ClDummy);
	if(m_LastPredClock < 0.0f)
		m_LastPredClock = PredClock;
	float ClockDelta = (PredClock - m_LastPredClock) * m_Tps / (float)Client()->GameTickSpeed();
	m_LastPredClock = PredClock;
	if(ClockDelta < 0.0f)
		ClockDelta = 0.0f;
	if(ClockDelta > 10.0f)
		ClockDelta = 10.0f;

	if(m_Paused)
	{
		m_TickAccum = 0.0f;
		return;
	}

	m_TickAccum += ClockDelta;
	if(m_TickAccum > 12.0f)
		m_TickAccum = 12.0f;

	int Steps = 0;
	while(m_TickAccum >= 1.0f && Steps < 8)
	{
		m_TickAccum -= 1.0f;
		AdvanceOne();
		Steps++;
		if(!m_Joined)
			break;
	}
}

void CTas::OnRender()
{
	RenderVisuals();

	if(!m_Joined)
		return;

	CGameClient *pGame = GameClient();

	// Play the playground's predicted events (explosions, sounds) — the
	// client's own handler only plays the m_PredictedWorld ones.
	auto EventIt = m_Playground.m_PredictedEvents.begin();
	while(EventIt != m_Playground.m_PredictedEvents.end())
	{
		if(!EventIt->m_Handled && EventIt->m_Tick <= m_PlaygroundTick)
		{
			if(EventIt->m_EventId == NETEVENTTYPE_SOUNDWORLD)
			{
				if(pGame->m_GameInfo.m_RaceSounds && ((EventIt->m_ExtraInfo == SOUND_GUN_FIRE && !g_Config.m_SndGun) || (EventIt->m_ExtraInfo == SOUND_PLAYER_PAIN_LONG && !g_Config.m_SndLongPain)))
				{
					EventIt = m_Playground.m_PredictedEvents.erase(EventIt);
					continue;
				}
				pGame->m_Sounds.PlayAt(CSounds::CHN_WORLD, EventIt->m_ExtraInfo, 1.0f, EventIt->m_Pos);
			}
			else if(EventIt->m_EventId == NETEVENTTYPE_EXPLOSION)
				pGame->m_Effects.Explosion(EventIt->m_Pos, 1.0f);
			else if(EventIt->m_EventId == NETEVENTTYPE_HAMMERHIT)
				pGame->m_Effects.HammerHit(EventIt->m_Pos, 1.0f, 1.0f);
			else if(EventIt->m_EventId == NETEVENTTYPE_DAMAGEIND)
				pGame->m_Effects.DamageIndicator(EventIt->m_Pos, direction(EventIt->m_ExtraInfo / 256.0f), 1.0f);
			EventIt->m_Handled = true;
			++EventIt;
		}
		else if(m_PlaygroundTick - EventIt->m_Tick > 3 * Client()->GameTickSpeed())
			EventIt = m_Playground.m_PredictedEvents.erase(EventIt);
		else
			++EventIt;
	}

	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return;

	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

	const CCharacterCore *pCore = pChar->Core();

	// Intra-tick interpolation: at TPS < 50 the world steps discretely, so
	// render positions are mixed between the last two ticks for smooth motion.
	const CCharacterCore *pPrevCore = nullptr;
	float IntraAlpha = 0.0f;
	if(!m_Paused && (int)m_vPlaygroundHistory.size() >= 2 && !m_vPlaygroundInputs.empty())
	{
		pPrevCore = &m_vPlaygroundHistory[m_vPlaygroundHistory.size() - 2].Core;
		IntraAlpha = RenderIntraAlpha();
	}

	// Playground projectiles — positioned by playground tick time, not the real client
	// tick (CItems::RenderProjectile uses real snap timing, wrong for us).
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
	{
		CProjectileData Data = static_cast<CProjectile *>(pEnt)->GetData();
		const int CurWeapon = std::clamp(Data.m_Type, 0, NUM_WEAPONS - 1);
		const CTuningParams *pTuning = pGame->GetTuning(Data.m_TuneZone);
		float Curvature = 0.0f;
		float Speed = 0.0f;
		if(CurWeapon == WEAPON_GRENADE)
		{
			Curvature = pTuning->m_GrenadeCurvature;
			Speed = pTuning->m_GrenadeSpeed;
		}
		else if(CurWeapon == WEAPON_SHOTGUN)
		{
			Curvature = pTuning->m_ShotgunCurvature;
			Speed = pTuning->m_ShotgunSpeed;
		}
		else if(CurWeapon == WEAPON_GUN)
		{
			Curvature = pTuning->m_GunCurvature;
			Speed = pTuning->m_GunSpeed;
		}

		const float Ct = ((float)(m_PlaygroundTick - Data.m_StartTick) - (1.0f - IntraAlpha)) / (float)m_Playground.GameTickSpeed();
		if(Ct < 0.0f)
			continue;
		const vec2 Pos = CalcPos(Data.m_StartPos, Data.m_StartVel, Curvature, Speed, Ct);
		const vec2 PrevPos = CalcPos(Data.m_StartPos, Data.m_StartVel, Curvature, Speed, Ct - 0.001f);
		const vec2 Vel = Pos - PrevPos;

		if(pGame->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon].IsValid())
		{
			Graphics()->TextureSet(pGame->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon]);
			Graphics()->QuadsBegin();
			if(CurWeapon == WEAPON_GRENADE)
				Graphics()->QuadsSetRotation(((float)m_PlaygroundTick - 1.0f + IntraAlpha) * 0.35f);
			else if(length(Vel) > 0.00001f)
				Graphics()->QuadsSetRotation(angle(Vel));
			else
				Graphics()->QuadsSetRotation(angle(Data.m_StartVel));
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			IGraphics::CQuadItem Quad(Pos.x, Pos.y, 32.0f, 32.0f);
			Graphics()->QuadsDraw(&Quad, 1);
			Graphics()->QuadsEnd();
		}
	}

	// Playground laser beams — rifle shots and the DDRace shotgun are both CLaser
	// entities; CItems::RenderLaser(CLaserData*) times off the real client
	// tick, so draw through the positional overload with the fake-tick age.
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
	{
		const CLaserData Data = static_cast<CLaser *>(pEnt)->GetData();
		const int LaserType = std::clamp(Data.m_Type, -1, NUM_LASERTYPES - 1);
		int ColorOut, ColorIn;
		if(LaserType == LASERTYPE_SHOTGUN)
		{
			ColorOut = g_Config.m_ClLaserShotgunOutlineColor;
			ColorIn = g_Config.m_ClLaserShotgunInnerColor;
		}
		else
		{
			ColorOut = g_Config.m_ClLaserRifleOutlineColor;
			ColorIn = g_Config.m_ClLaserRifleInnerColor;
		}
		const float Ticks = (float)(m_PlaygroundTick - Data.m_StartTick) + IntraAlpha;
		pGame->m_Items.RenderLaser(Data.m_From, Data.m_To,
			color_cast<ColorRGBA>(ColorHSLA(ColorOut).WithAlpha(1.0f)),
			color_cast<ColorRGBA>(ColorHSLA(ColorIn).WithAlpha(1.0f)),
			Ticks, (float)m_PlaygroundTick, LaserType);
	}

	for(CPickup *pPickup = (CPickup *)m_Playground.FindFirst(CGameWorld::ENTTYPE_PICKUP); pPickup; pPickup = (CPickup *)pPickup->TypeNext())
	{
		CNetObj_Pickup Data;
		pPickup->FillInfo(&Data);
		pGame->m_Items.RenderPickup(&Data, &Data, true, pPickup->Flags());
	}

	// Playground tee — drawn through the standard player renderer (weapon in hand,
	// hook chain/head and hand) like the race ghost does. ClientId -1 makes
	// the renderer take the positions from the fabricated snapshot below
	// instead of the m_aClients render data.
	CTeeRenderInfo Info = pGame->m_aClients[m_LocalId].m_RenderInfo;
	Info.m_Size = 64.0f;
	if(pChar->m_FreezeTime > 0)
		Info.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;

	CNetObj_Character Cur;
	mem_zero(&Cur, sizeof(Cur));
	Cur.m_X = (int)pCore->m_Pos.x;
	Cur.m_Y = (int)pCore->m_Pos.y;
	Cur.m_VelX = (int)(pCore->m_Vel.x * 256.0f);
	Cur.m_VelY = (int)(pCore->m_Vel.y * 256.0f);
	Cur.m_Direction = m_LastInput.m_Direction;
	// Aim: during playback the TAS Fake Aim masks the rendered angle (Real =
	// the recorded aim, the other modes are generated); otherwise the live
	// crosshair is used. Snapshots encode m_Angle = atan2(target) * 256.
	vec2 AimPoint(0.0f, 0.0f);
	if(!FakeAimOffset(&AimPoint))
		AimPoint = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	Cur.m_Angle = (int)(atan2f(AimPoint.y, AimPoint.x) * 256.0f);
	Cur.m_Weapon = std::clamp(pCore->m_ActiveWeapon, 0, NUM_WEAPONS - 1);
	Cur.m_HookState = pCore->m_HookState;
	Cur.m_HookX = (int)pCore->m_HookPos.x;
	Cur.m_HookY = (int)pCore->m_HookPos.y;
	Cur.m_HookDx = (int)(pCore->m_HookDir.x * 256.0f);
	Cur.m_HookDy = (int)(pCore->m_HookDir.y * 256.0f);
	Cur.m_HookedPlayer = -1;
	// Weapon kick: encoded as the raw fake-tick attack age; players.cpp
	// interpolates it with the TAS intra for the fake tee (ClientId -1).
	Cur.m_AttackTick = m_PlaygroundTick - pChar->GetAttackTick() - 1;
	Cur.m_Emote = EMOTE_NORMAL;
	Cur.m_Jumped = 1;
	CNetObj_Character Prev = Cur;
	if(pPrevCore)
	{
		Prev.m_X = (int)pPrevCore->m_Pos.x;
		Prev.m_Y = (int)pPrevCore->m_Pos.y;
		Prev.m_VelX = (int)(pPrevCore->m_Vel.x * 256.0f);
		Prev.m_VelY = (int)(pPrevCore->m_Vel.y * 256.0f);
		Prev.m_HookState = pPrevCore->m_HookState;
		Prev.m_HookX = (int)pPrevCore->m_HookPos.x;
		Prev.m_HookY = (int)pPrevCore->m_HookPos.y;
		Prev.m_HookDx = (int)(pPrevCore->m_HookDir.x * 256.0f);
		Prev.m_HookDy = (int)(pPrevCore->m_HookDir.y * 256.0f);
		Prev.m_AttackTick = Cur.m_AttackTick - 1;
	}

	if(Cur.m_HookState > 0)
		pGame->m_Players.RenderHook(&Prev, &Cur, &Info, -1, IntraAlpha);
	pGame->m_Players.RenderPlayer(&Prev, &Cur, &Info, -1, IntraAlpha);
}

void CTas::RenderFreezeBar()
{
	if(!m_Joined || !g_Config.m_ClShowFreezeBars)
		return;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar || pChar->m_FreezeTime <= 0)
		return;
	const CCharacterCore *pCore = pChar->Core();
	const int FreezeStart = pCore->m_FreezeStart;
	const int FreezeEnd = pCore->m_FreezeEnd;
	if(FreezeStart <= 0 || FreezeEnd <= FreezeStart)
		return;
	const int Max = FreezeEnd - FreezeStart;
	const float FreezeProgress = std::clamp(Max - (m_PlaygroundTick - FreezeStart), 0, Max) / (float)Max;
	float BarAlpha = 1.0f;
	if(pCore->m_IsInFreeze)
		BarAlpha *= g_Config.m_ClFreezeBarsAlphaInsideFreeze / 100.0f;
	if(BarAlpha <= 0.0f)
		return;
	const vec2 BarPos = PlaygroundTeeRenderPos();
	GameClient()->m_FreezeBars.RenderFreezeBarPos(BarPos.x - 32.0f, BarPos.y + 32.0f, 64.0f, 16.0f, FreezeProgress, BarAlpha);
}

void CTas::RenderVisuals()
{
	const std::vector<CTasTick> &vTicks = m_Recording ? m_vRecord : m_vSaved;
	const int TickCount = (int)vTicks.size();
	if(TickCount == 0)
		return;

	CGameClient *pGame = GameClient();
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
	Graphics()->TextureClear();

	const int LineSize = KxLineSize(KX_LINE_TAS);
	const float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
	const float Alpha = KxLineAlpha(KX_LINE_TAS);

	auto fnSpeedColor = [](float speed) -> ColorRGBA {
		const float MAX_SPEED = 30.0f;
		float t = std::clamp(speed / MAX_SPEED, 0.0f, 1.0f);
		float hue = t * 300.0f;
		return color_cast<ColorRGBA>(ColorHSLA(hue / 360.0f, 1.0f, 0.5f, true));
	};

	if(g_Config.m_KxTasShowPath)
	{
		if(LineSize > 0)
		{
			Graphics()->QuadsBegin();
			for(int i = 1; i < TickCount; i++)
			{
				const vec2 p0 = vTicks[i - 1].Core.m_Pos;
				const vec2 p1 = vTicks[i].Core.m_Pos;
				vec2 Dir = p1 - p0;
				const float Len = length(Dir);
				if(Len < 0.001f)
					continue;
				Dir /= Len;
				const vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;
				IGraphics::CFreeformItem Quad(
					p0.x - Perp.x, p0.y - Perp.y,
					p0.x + Perp.x, p0.y + Perp.y,
					p1.x - Perp.x, p1.y - Perp.y,
					p1.x + Perp.x, p1.y + Perp.y);
				if(g_Config.m_KxTasShowSpeed)
				{
					float speed0 = (i > 1) ? distance(vTicks[i - 2].Core.m_Pos, vTicks[i - 1].Core.m_Pos) : distance(p0, p1);
					float speed1 = distance(p0, p1);
					ColorRGBA c0 = fnSpeedColor(speed0);
					ColorRGBA c1 = fnSpeedColor(speed1);
					Graphics()->SetColor4(
						ColorRGBA(c0.r, c0.g, c0.b, Alpha),
						ColorRGBA(c0.r, c0.g, c0.b, Alpha),
						ColorRGBA(c1.r, c1.g, c1.b, Alpha),
						ColorRGBA(c1.r, c1.g, c1.b, Alpha));
				}
				else
				{
					ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_TAS, i - 1), true);
					Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
				}
				Graphics()->QuadsDrawFreeform(&Quad, 1);
			}
			Graphics()->QuadsEnd();
		}
		else
		{
			Graphics()->LinesBegin();
			for(int i = 1; i < TickCount; i++)
			{
				if(g_Config.m_KxTasShowSpeed)
				{
					float speed = distance(vTicks[i - 1].Core.m_Pos, vTicks[i].Core.m_Pos);
					ColorRGBA c = fnSpeedColor(speed);
					Graphics()->SetColor(c.r, c.g, c.b, Alpha);
				}
				else
				{
					ColorRGBA segCol = ColorRGBA(KxLineColorAt(KX_LINE_TAS, i - 1), true);
					Graphics()->SetColor(segCol.r, segCol.g, segCol.b, Alpha);
				}
				IGraphics::CLineItem Line(vTicks[i - 1].Core.m_Pos, vTicks[i].Core.m_Pos);
				Graphics()->LinesDraw(&Line, 1);
			}
			Graphics()->LinesEnd();
		}
	}

	if(g_Config.m_KxTasShowHooks)
	{
		const float HookHalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
		if(LineSize > 0)
		{
			std::vector<IGraphics::CFreeformItem> vQuads;
			for(int i = 1; i < TickCount; i++)
			{
				if(vTicks[i].Core.m_HookState != HOOK_GRABBED || vTicks[i - 1].Core.m_HookState == HOOK_GRABBED)
					continue;
				const vec2 d = vTicks[i].Core.m_HookPos - vTicks[i].Core.m_Pos;
				const float len = length(d);
				if(len < 0.001f)
					continue;
				const vec2 Dir = d / len;
				const vec2 Perp = vec2(Dir.y, -Dir.x) * HookHalfWidth;
				vQuads.emplace_back(
					vTicks[i].Core.m_Pos.x - Perp.x, vTicks[i].Core.m_Pos.y - Perp.y,
					vTicks[i].Core.m_Pos.x + Perp.x, vTicks[i].Core.m_Pos.y + Perp.y,
					vTicks[i].Core.m_HookPos.x - Perp.x, vTicks[i].Core.m_HookPos.y - Perp.y,
					vTicks[i].Core.m_HookPos.x + Perp.x, vTicks[i].Core.m_HookPos.y + Perp.y);
			}
			Graphics()->QuadsBegin();
			Graphics()->SetColor(0.2f, 0.4f, 1.0f, Alpha);
			for(size_t i = 0; i < vQuads.size(); i++)
				Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
			Graphics()->QuadsEnd();
		}
		else
		{
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.2f, 0.4f, 1.0f, Alpha);
			for(int i = 1; i < TickCount; i++)
			{
				if(vTicks[i].Core.m_HookState != HOOK_GRABBED || vTicks[i - 1].Core.m_HookState == HOOK_GRABBED)
					continue;
				IGraphics::CLineItem Line(vTicks[i].Core.m_Pos, vTicks[i].Core.m_HookPos);
				Graphics()->LinesDraw(&Line, 1);
			}
			Graphics()->LinesEnd();
		}
	}


}

void CTas::JoinLeavePlayground()
{
	if(m_Joined)
		LeavePlayground();
	else
		JoinPlayground();
}

void CTas::JoinPlayground()
{
	CGameClient *pGame = GameClient();
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0 || !pGame->m_PredictedWorld.GetCharacterById(LocalId))
		return;

	m_Playground.CopyWorld(&pGame->m_PredictedWorld);
	m_Playground.m_WorldConfig.m_PredictWeapons = true;
	m_Playground.m_WorldConfig.m_PredictTeleports = true;
	// Copied predicted events are already owned/played by the real predicted
	// world's handler — clear them so the playground does not replay them.
	m_Playground.m_PredictedEvents.clear();

	// The playground contains ONLY the local tee: every other character and
	// every other entity (projectiles, lasers, pickups, ...) is removed.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId)
			continue;
		CCharacter *pChar = m_Playground.GetCharacterById(i);
		if(pChar)
		{
			m_Playground.RemoveCharacter(pChar);
			m_Playground.RemoveEntity(pChar);
			delete pChar;
		}
	}
	for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
	{
		if(Type == CGameWorld::ENTTYPE_CHARACTER)
			continue;
		CEntity *pEnt = m_Playground.FindFirst(Type);
		while(pEnt)
		{
			CEntity *pNext = pEnt->TypeNext();
			m_Playground.RemoveEntity(pEnt);
			delete pEnt;
			pEnt = pNext;
		}
	}

	m_LocalId = LocalId;
	m_PlaygroundTick = m_Playground.m_GameTick;
	m_Joined = true;
	m_RealPlaying = false;
	m_RealIdx = 0;
	m_Playing = false;
	m_Recording = false;
	m_Desynced = false;
	m_PlayIdx = 0;
	m_TickAccum = 0.0f;
	m_LastPredClock = -1.0f;
	m_LastInput = CNetObj_PlayerInput{};
	m_AutoRfWasFrozen = m_AutoRfWasTele = m_AutoRfWasDead = false;
	m_RecordStart = CTasState{};
	// m_SavedStart survives leave/rejoin: it is the state Play restores in
	// the playground — clearing it here made a replay after a re-join start
	// from (0, 0).
	m_vPlaygroundHistory.clear();
	m_vPlaygroundInputs.clear();
	m_vPlaygroundHistory.push_back(CaptureState());
	RebaseCounters();
}

void CTas::LeavePlayground()
{
	m_Joined = false;
	m_Playing = false;
	m_Recording = false;
	m_Desynced = false;
	m_PlayIdx = 0;
	m_TickAccum = 0.0f;
	m_LastPredClock = -1.0f;
	m_LocalId = -1;
	m_PlaygroundTick = 0;
	m_vPlaygroundHistory.clear();
	m_vPlaygroundInputs.clear();
	// Unhook from the predicted world's copy chain before clearing.
	if(m_Playground.m_pParent && m_Playground.m_pParent->m_pChild == &m_Playground)
		m_Playground.m_pParent->m_pChild = nullptr;
	m_Playground.m_pParent = nullptr;
	m_Playground.Clear();
}

void CTas::TogglePlay()
{
	// Inside the playground: play the take back in the sandbox (at the
	// current TPS, works with Pause/Forward/Rewind) so the user can watch
		// it. Outside: execute the take on the REAL player — see
		// ApplyTasRealInput.
	if(m_Joined)
	{
		if(m_Playing)
		{
			// Stop: keep the playback position, do not delete anything.
			m_Playing = false;
			RebaseCounters();
			return;
		}
		// Play while the recording is still armed: finalize it first, so
		// the fresh take is playable without pressing Record Stop manually.
		if(m_Recording)
		{
			m_Recording = false;
			m_vSaved = m_vRecord;
			m_SavedStart = m_RecordStart;
		}
		if(m_vSaved.empty())
			return;
		m_Playing = true;
		m_Desynced = false;
		// Restart from the state the recording started at, so the recorded
		// inputs replay from exactly the state they were recorded in (the
		// fake history restarts; the saved recording stays intact).
		if(m_TakeFromFile)
			m_SavedStart = CaptureState();
		RestoreState(m_SavedStart);
		m_vPlaygroundHistory.clear();
		m_vPlaygroundHistory.push_back(m_SavedStart);
		m_vPlaygroundInputs.clear();
		m_PlayIdx = 0;
		m_TickAccum = 0.0f;
		return;
	}

	// Real-world execution: the recorded inputs are fed through SnapInput
	// every tick until the take ends.
	if(m_RealPlaying)
	{
		m_RealPlaying = false;
		dbg_msg("tas", "real replay stopped at %d/%d", m_RealIdx, (int)m_vSaved.size());
		return;
	}
	if(m_Recording)
	{
		m_Recording = false;
		m_vSaved = m_vRecord;
		m_SavedStart = m_RecordStart;
	}
	if(m_vSaved.empty())
		return;
	m_RealPlaying = true;
	m_RealIdx = 0;
	m_RealFire = 0;
	dbg_msg("tas", "real replay started (%d ticks)", (int)m_vSaved.size());
}

void CTas::PasteRun(const std::vector<CNetObj_PlayerInput> &vInputs, const std::vector<vec2> &vPath, const std::vector<std::pair<vec2, vec2>> &vHookSegs)
{
	if(vInputs.empty())
		return;
	if(m_Playing)
	{
		m_Playing = false;
		RebaseCounters();
	}
	if(m_Recording)
		m_Recording = false;
	m_SavedStart = CTasState{};
	m_vSaved.clear();
	for(size_t i = 0; i < vInputs.size(); i++)
	{
		CTasTick Tick;
		Tick.Tick = (int)i + 1;
		Tick.Input = vInputs[i];
		if(i + 1 < vPath.size())
			Tick.Core.m_Pos = vPath[i + 1];
		m_vSaved.push_back(std::move(Tick));
	}
	size_t Cursor = 0;
	for(const auto &Seg : vHookSegs)
	{
		while(Cursor < m_vSaved.size() && m_vSaved[Cursor].Core.m_Pos != Seg.first)
			Cursor++;
		if(Cursor >= m_vSaved.size())
			break;
		m_vSaved[Cursor].Core.m_HookState = HOOK_GRABBED;
		m_vSaved[Cursor].Core.m_HookPos = Seg.second;
		Cursor++;
	}
	m_TakeFromFile = true;
	m_PlayIdx = 0;
	m_Desynced = false;
	dbg_msg("tas", "pathfinder run pasted (%d ticks)", (int)m_vSaved.size());
}

bool CTas::HasRun(const std::vector<CNetObj_PlayerInput> &vInputs) const
{
	if(vInputs.empty() || vInputs.size() != m_vSaved.size())
		return false;
	for(size_t i = 0; i < vInputs.size(); i++)
	{
		if(mem_comp(&vInputs[i], &m_vSaved[i].Input, sizeof(CNetObj_PlayerInput)) != 0)
			return false;
	}
	return true;
}

void CTas::ToggleRecord()
{
	// Record outside the playground: re-enter it first.
	if(!m_Joined)
	{
		JoinPlayground();
		if(!m_Joined)
			return;
	}
	if(m_Recording)
	{
		// Stop: copy the record memory into the separate saved memory.
		m_Recording = false;
		m_vSaved = m_vRecord;
		m_SavedStart = m_RecordStart;
		return;
	}
	// Start: the record memory is cleared, the playground memory is not.
	m_Recording = true;
	m_Playing = false;
	m_Desynced = false;
	m_vRecord.clear();
	m_RecordStart = CaptureState();
	m_TakeFromFile = false;
}

void CTas::StepForward()
{
	if(!m_Joined || IsDead())
		return;
	if(m_Playing)
	{
		// Frame-stepping during playback: replay the next recorded input.
		if(m_PlayIdx >= (int)m_vSaved.size())
		{
			m_Playing = false;
			RebaseCounters();
			return;
		}
		const CTasTick &T = m_vSaved[m_PlayIdx];
		DoStep(T.Input);
		if(T.Core.m_Pos != vec2(0, 0) && !m_Desynced && !m_vPlaygroundHistory.empty() &&
			distance(m_vPlaygroundHistory.back().Core.m_Pos, T.Core.m_Pos) > 8.0f)
			m_Desynced = true;
		m_PlayIdx++;
	}
	else
	{
		// Live / recording: advance one tick with the current held input.
		const vec2 PosBefore = m_Playground.GetCharacterById(m_LocalId)->Core()->m_Pos;
		DoStep(LiveInput());
		DoAutoTiles(PosBefore);
	}
}

void CTas::StepRewind()
{
	if(!m_Joined || m_vPlaygroundInputs.empty())
		return;
	m_vPlaygroundInputs.pop_back();
	RestoreState(m_vPlaygroundHistory[m_vPlaygroundHistory.size() - 2]);
	m_vPlaygroundHistory.pop_back();
	if(m_Playing && m_PlayIdx > 0)
		m_PlayIdx--;
	// While recording, the rewound tick is dropped from the record buffer
	// too (frame-by-frame recording fixes mistakes by rewinding).
	if(m_Recording && !m_vRecord.empty())
		m_vRecord.pop_back();
	RebaseCounters();
}

bool CTas::IsDead()
{
	return !m_Joined || m_Playground.GetCharacterById(m_LocalId) == nullptr;
}

vec2 CTas::PlaygroundTeePos()
{
	const CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	// Core()->m_Pos, not m_Pos: m_Pos only updates on Tick() and is stale
	// right after RestoreState (rewind), which made the camera lag behind.
	return pChar ? pChar->Core()->m_Pos : vec2(0.0f, 0.0f);
}

int CTas::PlaygroundWeapon()
{
	const CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	return pChar ? pChar->Core()->m_ActiveWeapon : 0;
}

vec2 CTas::PlaygroundTeeRenderPos()
{
	const CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return vec2(0.0f, 0.0f);
	const vec2 Cur = pChar->Core()->m_Pos;
	// Exactly the current tick while paused / frame-stepping; between the last
	// two ticks otherwise, so low TPS values glide instead of teleporting.
	if(m_Paused || (int)m_vPlaygroundHistory.size() < 2 || m_vPlaygroundInputs.empty())
		return Cur;
	const CCharacterCore &Prev = m_vPlaygroundHistory[m_vPlaygroundHistory.size() - 2].Core;
	const float Alpha = RenderIntraAlpha();
	return mix(Prev.m_Pos, Cur, Alpha);
}

// HUD data from the fake tee: the full core (jumps, weapons, capabilities)
// and a snapshot-style character (active weapon, ammo, position for the
// ground check) so the HUD renders the playground state while joined.
bool CTas::PlaygroundHudState(CCharacterCore *pCore, CNetObj_Character *pPlayer)
{
	if(!m_Joined || !pCore || !pPlayer)
		return false;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return false;
	*pCore = pChar->GetCore();
	mem_zero(pPlayer, sizeof(*pPlayer));
	const int Weapon = std::clamp(pCore->m_ActiveWeapon, 0, NUM_WEAPONS - 1);
	pPlayer->m_Weapon = Weapon;
	pPlayer->m_X = (int)pCore->m_Pos.x;
	pPlayer->m_Y = (int)pCore->m_Pos.y;
	pPlayer->m_Jumped = pCore->m_Jumped;
	int Ammo = pCore->m_aWeapons[Weapon].m_Ammo;
	if(Ammo < 0)
		Ammo = 10; // infinite
	pPlayer->m_AmmoCount = std::clamp(Ammo, 0, 10);
	return true;
}

float CTas::RenderIntraAlpha() const
{
	// Progress toward the next fake tick: the un-stepped fraction of the
	// frame-time accumulator. OnUpdate performs a step in the very same
	// pass where the fraction crosses 1, so this never clamps — the
	// interpolation pair swaps smoothly, with no frozen frames in between.
	return std::clamp(m_TickAccum, 0.0f, 1.0f);
}

bool CTas::FutureSavedInput(int Idx, CNetObj_PlayerInput *pOut) const
{
	if(Idx < 0 || Idx >= (int)m_vSaved.size())
		return false;
	*pOut = m_vSaved[Idx].Input;
	return true;
}

CNetObj_PlayerInput CTas::LiveInput() const
{
	CNetObj_PlayerInput Input = GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
	// Rewind rebasing: the global counters keep every click undone by a
	// rewind — subtract them so only NEW clicks press in the fake timeline.
	Input.m_Fire -= m_FireRebase;
	Input.m_NextWeapon -= m_NextWeaponRebase;
	Input.m_PrevWeapon -= m_PrevWeaponRebase;
	return Input;
}

// Pin the live counters to the last recorded input; LiveInput subtracts the
// difference, so undone clicks neither fire during re-recording nor land in
// the recording (the ghost shot after rewind). Fire also needs an even
// rebase: its low bit is the pressed state, and an odd rebase inverts it —
// after a rewind with the button held the tee would never see the release
// and keep firing.
void CTas::RebaseCounters()
{
	if(m_Playing || m_vPlaygroundInputs.empty())
	{
		m_FireRebase = 0;
		m_NextWeaponRebase = 0;
		m_PrevWeaponRebase = 0;
		return;
	}
	const CNetObj_PlayerInput &Last = m_vPlaygroundInputs.back();
	const CNetObj_PlayerInput &Cur = GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
	m_FireRebase = Cur.m_Fire - Last.m_Fire;
	if(m_FireRebase & 1)
		m_FireRebase++;
	m_NextWeaponRebase = Cur.m_NextWeapon - Last.m_NextWeapon;
	m_PrevWeaponRebase = Cur.m_PrevWeapon - Last.m_PrevWeapon;
}

// Auto Rewind/Forward settings live in config vars (persisted, console-
// settable); the per-tile values are selected by m_AutoRfTile for the UI.
void CTas::SetAutoRfEnabled(bool On) { g_Config.m_KxTasAutoRf = On ? 1 : 0; }
bool CTas::AutoRfEnabled() const { return g_Config.m_KxTasAutoRf != 0; }

void CTas::SetAutoRfMode(int Mode)
{
	if(m_AutoRfTile == TAS_AUTOTILE_FREEZE)
		g_Config.m_KxTasAutoRfFMode = Mode != 0;
	else if(m_AutoRfTile == TAS_AUTOTILE_TELEPORT)
		g_Config.m_KxTasAutoRfTMode = Mode != 0;
	else
		g_Config.m_KxTasAutoRfDMode = Mode != 0;
}

int CTas::AutoRfMode() const
{
	return AutoRfModeFor(m_AutoRfTile);
}

bool CTas::AutoRfForward() const
{
	return AutoRfMode() != 0;
}

void CTas::SetAutoRfTicks(int Ticks)
{
	const int T = std::clamp(Ticks, 0, 100);
	if(m_AutoRfTile == TAS_AUTOTILE_FREEZE)
		g_Config.m_KxTasAutoRfFTicks = T;
	else if(m_AutoRfTile == TAS_AUTOTILE_TELEPORT)
		g_Config.m_KxTasAutoRfTTicks = T;
	else
		g_Config.m_KxTasAutoRfDTicks = T;
}

int CTas::AutoRfTicks() const
{
	return AutoRfTicksFor(m_AutoRfTile);
}

int CTas::AutoRfModeFor(int Tile) const
{
	if(Tile == TAS_AUTOTILE_FREEZE)
		return g_Config.m_KxTasAutoRfFMode;
	if(Tile == TAS_AUTOTILE_TELEPORT)
		return g_Config.m_KxTasAutoRfTMode;
	return g_Config.m_KxTasAutoRfDMode;
}

int CTas::AutoRfTicksFor(int Tile) const
{
	if(Tile == TAS_AUTOTILE_FREEZE)
		return g_Config.m_KxTasAutoRfFTicks;
	if(Tile == TAS_AUTOTILE_TELEPORT)
		return g_Config.m_KxTasAutoRfTTicks;
	return g_Config.m_KxTasAutoRfDTicks;
}

// Auto Rewind/Forward: when the recording hits a configured tile, jump
// back/ahead by the configured tick count (edge-triggered per episode).
void CTas::DoAutoTiles(const vec2 &PosBefore)
{
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	const bool Enabled = AutoRfEnabled() && !m_Playing;
	if(!pChar)
	{
		// Death tile / void: the tick destroyed the character.
		if(Enabled && !m_AutoRfWasDead)
			TriggerAutoTile(TAS_AUTOTILE_DEATH);
		m_AutoRfWasDead = true;
		m_AutoRfWasFrozen = false;
		m_AutoRfWasTele = false;
		return;
	}
	CCollision *pCol = pChar->Collision();
	const vec2 PosAfter = pChar->Core()->m_Pos;
	// Teleport hit: either the swept segment crosses a tele tile or the
	// position jumped (long tele jumps exceed the sweep's tile budget).
	bool Tele = distance(PosAfter, PosBefore) > 90.0f;
	const std::vector<int> vIndices = pCol->GetMapIndices(PosBefore, PosAfter);
	if(!vIndices.empty())
	{
		for(int Index : vIndices)
			if(pCol->IsTeleport(Index) || pCol->IsEvilTeleport(Index))
			{
				Tele = true;
				break;
			}
	}
	else
	{
		const int Idx = pCol->GetPureMapIndex(PosBefore);
		if(Idx >= 0 && (pCol->IsTeleport(Idx) || pCol->IsEvilTeleport(Idx)))
			Tele = true;
	}
	const bool Frozen = pChar->m_FreezeTime > 0;
	if(Enabled)
	{
		if(Frozen && !m_AutoRfWasFrozen)
			TriggerAutoTile(TAS_AUTOTILE_FREEZE);
		else if(Tele && !m_AutoRfWasTele)
			TriggerAutoTile(TAS_AUTOTILE_TELEPORT);
	}
	RefreshAutoTileFlags();
}

void CTas::TriggerAutoTile(int Tile)
{
	const int Ticks = AutoRfTicksFor(Tile);
	if(Ticks <= 0)
		return;
	if(AutoRfModeFor(Tile) == 0)
	{
		for(int i = 0; i < Ticks && !m_vPlaygroundInputs.empty(); ++i)
			StepRewind();
	}
	else
	{
		// Forward: burn ticks with the last used input (also recorded).
		for(int i = 0; i < Ticks && !IsDead(); ++i)
			DoStep(m_LastInput);
	}
	RefreshAutoTileFlags();
}

void CTas::RefreshAutoTileFlags()
{
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	m_AutoRfWasDead = !pChar;
	m_AutoRfWasFrozen = pChar && pChar->m_FreezeTime > 0;
	// A tee never rests on a tele-in tile (it is moved away the same tick),
	// so the tele episode ends with the teleport itself.
	m_AutoRfWasTele = false;
}

// Real-world execution: apply the next saved input to the real player.
// Called from CControls::SnapInput each tick while m_RealPlaying is true.
// Movement/jump/hook/aim/weapon are copied verbatim from
// the recording (aim offsets are relative to the tee, so the take must be
// started from the recorded start position). Fire is replayed through a
// counter rebased onto the live one: the deltas between the recorded ticks
// are applied to the player's own counter, preserving every press/release
// parity edge without clashing with the current value.
bool CTas::ApplyTasRealInput(CNetObj_PlayerInput *pInput)
{
	if(!m_RealPlaying || !pInput)
		return false;
	if(m_RealIdx >= (int)m_vSaved.size())
	{
		m_RealPlaying = false;
		dbg_msg("tas", "real replay finished (%d ticks)", (int)m_vSaved.size());
		return false;
	}
	const CTasTick &T = m_vSaved[m_RealIdx];
	if(m_RealIdx == 0)
		m_RealFire = pInput->m_Fire;
	else
		m_RealFire += T.Input.m_Fire - m_vSaved[m_RealIdx - 1].Input.m_Fire;
	pInput->m_Direction = T.Input.m_Direction;
	pInput->m_Jump = T.Input.m_Jump;
	pInput->m_Hook = T.Input.m_Hook;
	pInput->m_TargetX = T.Input.m_TargetX;
	pInput->m_TargetY = T.Input.m_TargetY;
	pInput->m_WantedWeapon = T.Input.m_WantedWeapon;
	pInput->m_Fire = m_RealFire;
	m_RealIdx++;
	return true;
}

// TAS Fake Aim: rendered aim for the current playback position. Never
// touches the physics input — this only masks what is drawn. The anchor
// events for Robot/Smooth are the hook presses and shots of the recorded
// stream (fire counter parity flip / hook rising edge).
static bool TasTickEvent(const CNetObj_PlayerInput &Cur, const CNetObj_PlayerInput &Prev)
{
	return (Cur.m_Fire & 1) != (Prev.m_Fire & 1) || (Cur.m_Hook != 0 && Prev.m_Hook == 0);
}

bool CTas::FakeAimOffset(vec2 *pOffset)
{
	if(!FakeAimEnabled() || m_vSaved.empty() || !pOffset)
		return false;
	int Idx = -1;
	if(m_Playing)
		Idx = m_PlayIdx;
	else if(m_RealPlaying)
		Idx = m_RealIdx;
	if(Idx < 0)
		return false;
	const int Last = (int)m_vSaved.size() - 1;
	if(Idx > Last)
		Idx = Last;

	const vec2 Recorded((float)m_vSaved[Idx].Input.m_TargetX, (float)m_vSaved[Idx].Input.m_TargetY);
	int speed = g_Config.m_KxFakeAimSpeed;
	if(speed < 1)
		speed = 1;
	if(speed > 100)
		speed = 100;
	constexpr float AIM_DIST = 200.0f;

	vec2 Offset = Recorded;
	if(FakeAimMode() == 1 || FakeAimMode() == 3)
	{
		// Smooth / Robot: anchor points are the recorded hook/shot aims.
		int Anchor = 0;
		for(int i = 1; i <= Idx; ++i)
			if(TasTickEvent(m_vSaved[i].Input, m_vSaved[i - 1].Input))
				Anchor = i;
		const vec2 P0((float)m_vSaved[Anchor].Input.m_TargetX, (float)m_vSaved[Anchor].Input.m_TargetY);
		if(FakeAimMode() == 3)
		{
			// Robot: hold the last anchor until the next one.
			Offset = P0;
		}
		else
		{
			// Smooth: glide from the last anchor to the next one by ANGLE
			// (uniform rotation along the shortest arc), still arriving
			// exactly on the hook/shot tick and its anchor coordinates.
			int Next = -1;
			for(int i = Idx + 1; i <= Last; ++i)
				if(TasTickEvent(m_vSaved[i].Input, m_vSaved[i - 1].Input))
				{
					Next = i;
					break;
				}
			if(Next < 0)
				Offset = P0;
			else
			{
				const vec2 P1((float)m_vSaved[Next].Input.m_TargetX, (float)m_vSaved[Next].Input.m_TargetY);
				const float Alpha = (Next > Anchor) ? (float)(Idx - Anchor) / (float)(Next - Anchor) : 1.0f;
				// Polar glide: rotate the direction along the shortest arc and
				// scale the length — the crosshair sweeps uniformly around the
				// tee and both anchors still land exactly on their coordinates.
				const float A0 = atan2f(P0.y, P0.x);
				const float A1 = atan2f(P1.y, P1.x);
				const float Delta = atan2f(sinf(A1 - A0), cosf(A1 - A0));
				const float Len = mix(length(P0), length(P1), Alpha);
				Offset = vec2(cosf(A0 + Delta * Alpha), sinf(A0 + Delta * Alpha)) * Len;
			}
		}
	}
	else if(FakeAimMode() == 2)
	{
		// Random: new direction every speed recorded ticks.
		float Angle = sinf((float)(Idx / speed) * 12.9898f) * 43758.5473f;
		Angle -= floorf(Angle);
		Angle *= 2.0f * pi;
		Offset = vec2(cosf(Angle), sinf(Angle)) * AIM_DIST;
	}
	else if(FakeAimMode() == 4)
	{
		// Spin: constant rotation, same rate as the original Fake Aim.
		const float Angle = (float)Idx * (speed / 100.0f) * 0.5f;
		Offset = vec2(cosf(Angle), sinf(Angle)) * AIM_DIST;
	}
	else if(FakeAimMode() == 5)
	{
		// Lag: hold the recorded aim, refresh it every speed ticks.
		const int Lag = (Idx / speed) * speed;
		Offset = vec2((float)m_vSaved[Lag].Input.m_TargetX, (float)m_vSaved[Lag].Input.m_TargetY);
	}
	// Mode 0 (Real): keep the recorded aim as-is.

	*pOffset = Offset;
	return true;
}

CTasState CTas::CaptureState()
{
	CTasState State;
	State.Tick = m_PlaygroundTick;
	if(CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId))
	{
		State.Core = pChar->GetCore();
		State.FreezeTime = pChar->m_FreezeTime;
		State.PrevPos = pChar->m_PrevPos;
		State.PrevPrevPos = pChar->m_PrevPrevPos;
		State.ReloadTimer = pChar->GetReloadTimer();
		State.AttackTick = pChar->GetAttackTick();
		State.LatestInput = pChar->GetLatestInput();
	}
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
		State.vProjectiles.push_back(static_cast<CProjectile *>(pEnt)->GetData());
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
	{
		CLaser *pLaser = static_cast<CLaser *>(pEnt);
		CLaserState Laser;
		Laser.Data = pLaser->GetData();
		Laser.Dir = pLaser->GetDir();
		Laser.PrevPos = pLaser->GetPrevPos();
		Laser.Energy = pLaser->GetEnergy();
		Laser.Bounces = pLaser->GetBounces();
		Laser.Owner = pLaser->GetOwner();
		Laser.ZeroEnergyBounce = pLaser->GetZeroEnergyBounceInLastTick();
		State.vLasers.push_back(Laser);
	}
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PICKUP); pEnt; pEnt = pEnt->TypeNext())
	{
		CPickup *pPickup = static_cast<CPickup *>(pEnt);
		CPickupState Pickup;
		Pickup.Pos = pPickup->m_Pos;
		Pickup.Type = pPickup->Type();
		Pickup.Subtype = pPickup->Subtype();
		Pickup.Flags = pPickup->Flags();
		Pickup.SwitchNumber = pPickup->m_Number;
		State.vPickups.push_back(Pickup);
	}
	return State;
}

void CTas::RestoreState(const CTasState &State)
{
	m_PlaygroundTick = State.Tick;
	m_Playground.m_GameTick = State.Tick;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
	{
		// The tee was destroyed by a death tile — resurrect it from the
		// snapshot (the prediction world deletes dead characters).
		CNetObj_Character CharData;
		mem_zero(&CharData, sizeof(CharData));
		CharData.m_X = (int)State.Core.m_Pos.x;
		CharData.m_Y = (int)State.Core.m_Pos.y;
		CharData.m_VelX = (int)(State.Core.m_Vel.x * 256.0f);
		CharData.m_VelY = (int)(State.Core.m_Vel.y * 256.0f);
		CharData.m_HookState = State.Core.m_HookState;
		CharData.m_HookX = (int)State.Core.m_HookPos.x;
		CharData.m_HookY = (int)State.Core.m_HookPos.y;
		CharData.m_HookDx = (int)(State.Core.m_HookDir.x * 256.0f);
		CharData.m_HookDy = (int)(State.Core.m_HookDir.y * 256.0f);
		CharData.m_Weapon = State.Core.m_ActiveWeapon;
		pChar = new CCharacter(&m_Playground, m_LocalId, &CharData);
		m_Playground.InsertEntity(pChar);
	}
	pChar->SetCore(State.Core);
	pChar->SetCoreWorld(&m_Playground);
	pChar->m_Pos = State.Core.m_Pos;
	pChar->m_FreezeTime = State.FreezeTime;
	pChar->m_PrevPos = State.PrevPos;
	pChar->m_PrevPrevPos = State.PrevPrevPos;
	pChar->SetReloadTimer(State.ReloadTimer);
	pChar->SetAttackTick(State.AttackTick);
	pChar->SetLatestInput(State.LatestInput);

	// Rebuild all projectiles from the snapshot (CProjectile self-inserts).
	CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
	while(pEnt)
	{
		CEntity *pNext = pEnt->TypeNext();
		m_Playground.RemoveEntity(pEnt);
		delete pEnt;
		pEnt = pNext;
	}
	int ProjId = 0;
	for(const CProjectileData &Data : State.vProjectiles)
	{
		CProjectile *pProj = new CProjectile(&m_Playground, ProjId++, &Data);
		m_Playground.InsertEntity(pProj);
	}
	pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PICKUP);
	while(pEnt)
	{
		CEntity *pNext = pEnt->TypeNext();
		m_Playground.RemoveEntity(pEnt);
		delete pEnt;
		pEnt = pNext;
	}
	int PickupId = 0;
	for(const CPickupState &Pickup : State.vPickups)
	{
		CPickupData Data;
		Data.m_Pos = Pickup.Pos;
		Data.m_Type = Pickup.Type;
		Data.m_Subtype = Pickup.Subtype;
		Data.m_Flags = Pickup.Flags;
		Data.m_SwitchNumber = Pickup.SwitchNumber;
		CPickup *pPickup = new CPickup(&m_Playground, PickupId++, &Data);
		m_Playground.InsertEntity(pPickup);
	}

	// Weapon beams are rebuilt from the snapshot (their live state is part of
	// the rewind); predicted events beyond the rewound tick are dropped the
	// same way.
	pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_LASER);
	while(pEnt)
	{
		CEntity *pNext = pEnt->TypeNext();
		m_Playground.RemoveEntity(pEnt);
		delete pEnt;
		pEnt = pNext;
	}
	int LaserId = 0;
	for(const CLaserState &Laser : State.vLasers)
	{
		CLaserData Data = Laser.Data;
		CLaser *pLaser = new CLaser(&m_Playground, LaserId++, &Data);
		m_Playground.InsertEntity(pLaser);
		pLaser->Restore(Laser.Data, Laser.Dir, Laser.PrevPos, Laser.Energy, Laser.Bounces, Laser.Owner, Laser.ZeroEnergyBounce);
	}
	for(size_t i = 0; i < m_Playground.m_PredictedEvents.size();)
	{
		if(m_Playground.m_PredictedEvents[i].m_Tick > State.Tick)
			m_Playground.m_PredictedEvents.erase(m_Playground.m_PredictedEvents.begin() + i);
		else
			i++;
	}
}

static void SpawnPlaygroundAirJump(CGameClient *pGame, vec2 Pos, float Scale)
{
	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_AIRJUMP;
	p.m_Pos = Pos + vec2(-6.0f, 16.0f);
	p.m_Vel = vec2(0.0f, -200.0f) * Scale;
	p.m_LifeSpan = 0.5f / Scale;
	p.m_StartSize = 48.0f;
	p.m_EndSize = 0.0f;
	p.m_Rot = random_angle();
	p.m_Rotspeed = pi * 2.0f * Scale;
	p.m_Gravity = 500.0f * Scale * Scale;
	p.m_Friction = 0.7f * Scale;
	p.m_FlowAffected = 0.0f;
	p.m_Color.a = 1.0f;
	p.m_StartAlpha = 1.0f;
	pGame->m_Particles.Add(CParticles::GROUP_GENERAL, &p);

	p.m_Pos = Pos + vec2(6.0f, 16.0f);
	pGame->m_Particles.Add(CParticles::GROUP_GENERAL, &p);

	if(g_Config.m_SndGame)
		pGame->m_Sounds.PlayAt(CSounds::CHN_WORLD, SOUND_PLAYER_AIRJUMP, 1.0f, Pos);
}

void CTas::StepWorld(const CNetObj_PlayerInput &Input)
{
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return;
	m_LastInput = Input;
	// Same tick recipe as the kinodynamic simulator:
	// OnDirectInput -> m_GameTick -> OnPredictedInput -> Tick().
	pChar->OnDirectInput(&Input);
	m_PlaygroundTick++;
	m_Playground.m_GameTick = m_PlaygroundTick;
	pChar->OnPredictedInput(&Input);
	m_Playground.Tick();
	// Double-jump sprite + sound: same pump as the real world, which fires
	// CEffects::AirJump from the core's triggered events after each tick.
	if(pChar->Core()->m_TriggeredEvents & COREEVENT_AIR_JUMP)
		SpawnPlaygroundAirJump(GameClient(), pChar->Core()->m_Pos, m_Tps / 50.0f);
}

// Projectile trail particles for the playground: a copy of the client's
// SmokeTrail/BulletTrail with every time-dependent property scaled by the
// TPS factor (velocity and spread x s, gravity x s^2, friction x s, lifetime
// / s) and spawned once per fake tick, so the trails slow down together with
// the world exactly like a 50 TPS recording played slower.
static void SpawnPlaygroundTrail(CGameClient *pGame, const CProjectileData &Data, int PlaygroundTick, float TickSpeed, float Scale)
{
	float Curvature = 0.0f;
	float Speed = 0.0f;
	if(Data.m_Type == WEAPON_GRENADE)
	{
		Curvature = pGame->GetTuning(Data.m_TuneZone)->m_GrenadeCurvature;
		Speed = pGame->GetTuning(Data.m_TuneZone)->m_GrenadeSpeed;
	}
	else if(Data.m_Type == WEAPON_GUN)
	{
		Curvature = pGame->GetTuning(Data.m_TuneZone)->m_GunCurvature;
		Speed = pGame->GetTuning(Data.m_TuneZone)->m_GunSpeed;
	}
	else
		return;
	const float T = (float)(PlaygroundTick - Data.m_StartTick) / TickSpeed;
	if(T < 0.0f)
		return;
	const vec2 Pos = CalcPos(Data.m_StartPos, Data.m_StartVel, Curvature, Speed, T);
	const vec2 PosPrev = CalcPos(Data.m_StartPos, Data.m_StartVel, Curvature, Speed, T - 0.001f);
	const vec2 Vel = Pos - PosPrev;

	CParticle p;
	p.SetDefault();
	p.m_Pos = Pos;
	if(Data.m_Type == WEAPON_GRENADE)
	{
		p.m_Spr = SPRITE_PART_SMOKE;
		p.m_Vel = Vel * -1.0f * Scale + random_direction() * 50.0f * Scale;
		p.m_LifeSpan = random_float(0.5f, 1.0f) / Scale;
		p.m_StartSize = random_float(12.0f, 20.0f);
		p.m_EndSize = 0.0f;
		p.m_Friction = 0.7f * Scale;
		p.m_Gravity = random_float(-500.0f) * Scale * Scale;
		p.m_Color.a = 1.0f;
		p.m_StartAlpha = 1.0f;
	}
	else
	{
		p.m_Spr = SPRITE_PART_BALL;
		p.m_LifeSpan = random_float(0.25f, 0.5f) / Scale;
		p.m_StartSize = 8.0f;
		p.m_EndSize = 0.0f;
		p.m_Friction = 0.7f * Scale;
	}
	pGame->m_Particles.Add(CParticles::GROUP_PROJECTILE_TRAIL, &p);
}

void CTas::DoStep(const CNetObj_PlayerInput &Input)
{
	if(!m_Joined)
		return;
	if(!m_Playground.GetCharacterById(m_LocalId))
		return;
	StepWorld(Input);

	// Projectile trails: one particle per fake tick, fully time-scaled.
	const float TrailScale = m_Tps / (float)Client()->GameTickSpeed();
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
		SpawnPlaygroundTrail(GameClient(), static_cast<CProjectile *>(pEnt)->GetData(), m_PlaygroundTick, (float)m_Playground.GameTickSpeed(), TrailScale);
	m_vPlaygroundInputs.push_back(Input);
	m_vPlaygroundHistory.push_back(CaptureState());
	TrimHistory();
	if(m_Recording)
	{
		CTasTick Tick;
		Tick.Tick = m_PlaygroundTick;
		Tick.Input = Input;
		const CTasState &S = m_vPlaygroundHistory.back();
		Tick.Core = S.Core;
		Tick.FreezeTime = S.FreezeTime;
		Tick.vProjectiles = S.vProjectiles;
		m_vRecord.push_back(std::move(Tick));
	}
}

void CTas::AdvanceOne()
{
	if(IsDead())
		return;
	if(m_Playing)
	{
		if(m_PlayIdx >= (int)m_vSaved.size())
		{
			m_Playing = false; // playback finished
			RebaseCounters();
			return;
		}
		const CTasTick &T = m_vSaved[m_PlayIdx];
		DoStep(T.Input);
		if(T.Core.m_Pos != vec2(0, 0) && !m_Desynced && !m_vPlaygroundHistory.empty() &&
			distance(m_vPlaygroundHistory.back().Core.m_Pos, T.Core.m_Pos) > 8.0f)
			m_Desynced = true;
		m_PlayIdx++;
	}
	else
	{
		const vec2 PosBefore = m_Playground.GetCharacterById(m_LocalId)->Core()->m_Pos;
		DoStep(LiveInput());
		DoAutoTiles(PosBefore);
	}
}

void CTas::TrimHistory()
{
	if((int)m_vPlaygroundHistory.size() <= MAX_HISTORY + 1)
		return;
	// Keep history[0] (the join state — Play restarts from it), drop a chunk
	// of the oldest steps so rewind depth stays bounded.
	m_vPlaygroundHistory.erase(m_vPlaygroundHistory.begin() + 1, m_vPlaygroundHistory.begin() + 1 + TRIM_CHUNK);
	if((int)m_vPlaygroundInputs.size() > TRIM_CHUNK)
		m_vPlaygroundInputs.erase(m_vPlaygroundInputs.begin(), m_vPlaygroundInputs.begin() + TRIM_CHUNK);
	else
		m_vPlaygroundInputs.clear();
}

static const char g_aKxTasFormat[] = "kinetix1";

static bool KxTasParseHeaderLine(const char *pLine, int LineLen, const char *pKey, char *pValue, int ValueSize)
{
	const int KeyLen = str_length(pKey);
	if(LineLen < KeyLen + 5 || pLine[0] != '"')
		return false;
	if(strncmp(pLine + 1, pKey, KeyLen) != 0 || pLine[KeyLen + 1] != '"')
		return false;
	const char *pVal = pLine + KeyLen + 2;
	if(pVal[0] != ':' || pVal[1] != ' ' || pVal[2] != '"')
		return false;
	const char *pStart = pVal + 3;
	const char *pEnd = (const char *)memchr(pStart, '"', LineLen - (int)(pStart - pLine));
	if(!pEnd)
		return false;
	int Len = (int)(pEnd - pStart);
	if(Len > ValueSize - 1)
		Len = ValueSize - 1;
	mem_copy(pValue, pStart, Len);
	pValue[Len] = '\0';
	return true;
}

static bool KxTasReadHeader(const char *pText, int TextLen, const char *pKey, char *pValue, int ValueSize)
{
	const char *pCursor = pText;
	const char *pEnd = pText + TextLen;
	pValue[0] = '\0';
	while(pCursor < pEnd)
	{
		const char *pLineEnd = (const char *)memchr(pCursor, '\n', pEnd - pCursor);
		const int LineLen = pLineEnd ? (int)(pLineEnd - pCursor) : (int)(pEnd - pCursor);
		if(KxTasParseHeaderLine(pCursor, LineLen, pKey, pValue, ValueSize))
			return true;
		if(!pLineEnd)
			break;
		pCursor = pLineEnd + 1;
	}
	return false;
}

void CTas::PickFile(const char *pPath)
{
	if(m_Recording || !pPath || !pPath[0])
		return;
	LoadTakeFile(pPath);
}

void CTas::UnpickFile()
{
	m_FilePicked = false;
	m_aPickedPath[0] = '\0';
	m_aPickedMap[0] = '\0';
	m_aPickedName[0] = '\0';
	m_aPickedAuthor[0] = '\0';
	m_aPickedCreated[0] = '\0';
}

bool CTas::WriteTakeFile(const char *pPath, const char *pMap, const char *pName, const char *pAuthor, const char *pCreated)
{
	const std::vector<CTasTick> &vTake = m_Recording ? m_vRecord : m_vSaved;
	if(vTake.empty())
		return false;

	char aText[2048];
	str_format(aText, sizeof(aText),
		"\"Format\": \"%s\",\n"
		"\"Map\": \"%s\",\n"
		"\"Name\": \"%s\",\n"
		"\"Author\": \"%s\",\n"
		"\"Created\": \"%s\",\n"
		"\"Ticks\": \"%d\",\n"
		"\"Tps\": \"%.1f\"\n",
		g_aKxTasFormat, pMap, pName, pAuthor, pCreated,
		(int)vTake.size(), (double)m_Tps);
	const int TextLen = str_length(aText) + 1;

	const int InputSize = (int)sizeof(CNetObj_PlayerInput);
	const int CoreSize = 6 * (int)sizeof(float) + 3 * (int)sizeof(int);
	const int TickSize = InputSize + CoreSize;
	const int BinSize = 16 + (int)vTake.size() * TickSize;
	const int UncompSize = 4 + TextLen + BinSize;

	std::vector<unsigned char> vUncomp(UncompSize);
	unsigned char *pOut = vUncomp.data();
	mem_copy(pOut, &BinSize, sizeof(int));
	pOut += 4;
	mem_copy(pOut, aText, TextLen);
	pOut += TextLen;
	const int Version = 1;
	const int TickCount = (int)vTake.size();
	const int StartTick = vTake.front().Tick;
	const float FileTps = m_Tps;
	mem_copy(pOut, &Version, sizeof(int));
	mem_copy(pOut + 4, &TickCount, sizeof(int));
	mem_copy(pOut + 8, &StartTick, sizeof(int));
	mem_copy(pOut + 12, &FileTps, sizeof(float));
	pOut += 16;
	for(const CTasTick &Tick : vTake)
	{
		mem_copy(pOut, &Tick.Input, InputSize);
		float *pFloats = (float *)(pOut + InputSize);
		pFloats[0] = Tick.Core.m_Pos.x;
		pFloats[1] = Tick.Core.m_Pos.y;
		pFloats[2] = Tick.Core.m_Vel.x;
		pFloats[3] = Tick.Core.m_Vel.y;
		pFloats[4] = Tick.Core.m_HookPos.x;
		pFloats[5] = Tick.Core.m_HookPos.y;
		int *pInts = (int *)(pOut + InputSize + 6 * (int)sizeof(float));
		pInts[0] = Tick.Core.m_HookState;
		pInts[1] = Tick.Core.m_HookTick;
		pInts[2] = Tick.Core.m_ActiveWeapon;
		pOut += TickSize;
	}

	uLongf CompLen = compressBound((uLong)UncompSize);
	std::vector<unsigned char> vComp(CompLen);
	if(compress2(vComp.data(), &CompLen, vUncomp.data(), (uLong)UncompSize, Z_BEST_COMPRESSION) != Z_OK)
		return false;

	const char aMagic[4] = {'K', 'X', 'T', '1'};
	const int CompSize = (int)CompLen;
	IOHANDLE File = Storage()->OpenFile(pPath, IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
	if(!File)
		return false;
	bool Ok = true;
	Ok = Ok && io_write(File, aMagic, sizeof(aMagic)) == (unsigned)sizeof(aMagic);
	Ok = Ok && io_write(File, &UncompSize, sizeof(int)) == (unsigned)sizeof(int);
	Ok = Ok && io_write(File, &CompSize, sizeof(int)) == (unsigned)sizeof(int);
	Ok = Ok && io_write(File, vComp.data(), CompSize) == (unsigned)CompSize;
	io_close(File);
	return Ok;
}

bool CTas::LoadTakeFile(const char *pPath)
{
	void *pRawData = nullptr;
	unsigned RawSize = 0;
	if(!Storage()->ReadFile(pPath, IStorage::TYPE_ABSOLUTE, &pRawData, &RawSize))
		return false;

	bool Ok = false;
	do
	{
		const unsigned char *pData = (const unsigned char *)pRawData;
		if(RawSize < 16 || pData[0] != 'K' || pData[1] != 'X' || pData[2] != 'T' || pData[3] != '1')
			break;
		int UncompSize = 0;
		int CompSize = 0;
		mem_copy(&UncompSize, pData + 4, sizeof(int));
		mem_copy(&CompSize, pData + 8, sizeof(int));
		if(UncompSize <= 0 || UncompSize > 512 * 1024 * 1024 || CompSize <= 0 || 12 + CompSize > (int)RawSize)
			break;
		std::vector<unsigned char> vUncomp(UncompSize);
		uLongf DestLen = UncompSize;
		if(uncompress(vUncomp.data(), &DestLen, pData + 12, (uLong)CompSize) != Z_OK || (int)DestLen != UncompSize)
			break;
		int BinSize = 0;
		mem_copy(&BinSize, vUncomp.data(), sizeof(int));
		if(BinSize < 16 || 4 + BinSize > UncompSize)
			break;
		const int TextLen = UncompSize - 4 - BinSize;
		if(TextLen <= 0)
			break;
		const char *pText = (const char *)vUncomp.data() + 4;
		char aMap[128] = {0};
		char aName[128] = {0};
		char aAuthor[128] = {0};
		char aCreated[64] = {0};
		KxTasReadHeader(pText, TextLen - 1, "Map", aMap, sizeof(aMap));
		KxTasReadHeader(pText, TextLen - 1, "Name", aName, sizeof(aName));
		KxTasReadHeader(pText, TextLen - 1, "Author", aAuthor, sizeof(aAuthor));
		KxTasReadHeader(pText, TextLen - 1, "Created", aCreated, sizeof(aCreated));

		const unsigned char *pBin = vUncomp.data() + 4 + TextLen;
		int Version = 0;
		int TickCount = 0;
		int StartTick = 0;
		float FileTps = 0.0f;
		mem_copy(&Version, pBin, sizeof(int));
		mem_copy(&TickCount, pBin + 4, sizeof(int));
		mem_copy(&StartTick, pBin + 8, sizeof(int));
		mem_copy(&FileTps, pBin + 12, sizeof(float));
		const int InputSize = (int)sizeof(CNetObj_PlayerInput);
		const int CoreSize = 6 * (int)sizeof(float) + 3 * (int)sizeof(int);
		const int TickSize = InputSize + CoreSize;
		if(Version != 1 || TickCount < 0 || TickCount > (BinSize - 16) / TickSize)
			break;

		std::vector<CTasTick> vNew;
		vNew.resize(TickCount);
		const unsigned char *pTick = pBin + 16;
		for(int i = 0; i < TickCount; i++, pTick += TickSize)
		{
			CTasTick &Tick = vNew[i];
			Tick.Tick = StartTick + i;
			mem_copy(&Tick.Input, pTick, InputSize);
			const float *pFloats = (const float *)(pTick + InputSize);
			Tick.Core.m_Pos.x = pFloats[0];
			Tick.Core.m_Pos.y = pFloats[1];
			Tick.Core.m_Vel.x = pFloats[2];
			Tick.Core.m_Vel.y = pFloats[3];
			Tick.Core.m_HookPos.x = pFloats[4];
			Tick.Core.m_HookPos.y = pFloats[5];
			const int *pInts = (const int *)(pTick + InputSize + 6 * (int)sizeof(float));
			Tick.Core.m_HookState = pInts[0];
			Tick.Core.m_HookTick = pInts[1];
			Tick.Core.m_ActiveWeapon = pInts[2];
		}

		m_vSaved = std::move(vNew);
		str_copy(m_aPickedPath, pPath, sizeof(m_aPickedPath));
		str_copy(m_aPickedMap, aMap, sizeof(m_aPickedMap));
		str_copy(m_aPickedName, aName, sizeof(m_aPickedName));
		str_copy(m_aPickedAuthor, aAuthor, sizeof(m_aPickedAuthor));
		str_copy(m_aPickedCreated, aCreated, sizeof(m_aPickedCreated));
		m_FilePicked = true;
		m_Playing = false;
		m_RealPlaying = false;
		m_Paused = false;
		m_PlayIdx = 0;
		m_RealIdx = 0;
		if(FileTps >= 1.0f && FileTps <= 50.0f)
			m_Tps = FileTps;
		m_TakeFromFile = true;
		Ok = true;
	} while(false);

	free(pRawData);
	return Ok;
}

void CTas::SaveTake(const char *pMap, const char *pName, const char *pAuthor)
{
	const std::vector<CTasTick> &vTake = m_Recording ? m_vRecord : m_vSaved;
	if(vTake.empty())
		return;

	const char *pMapClean = (pMap && pMap[0]) ? pMap : "unnamed";
	char aMapSafe[128];
	str_copy(aMapSafe, pMapClean, sizeof(aMapSafe));
	for(int i = 0; aMapSafe[i]; i++)
	{
		switch(aMapSafe[i])
		{
		case '<': case '>': case ':': case '"': case '/':
		case '\\': case '|': case '?': case '*':
			aMapSafe[i] = '_';
			break;
		default:
			break;
		}
	}

	char aPath[512];
	char aCreated[64];
	if(m_FilePicked)
	{
		str_copy(aPath, m_aPickedPath, sizeof(aPath));
		str_copy(aCreated, m_aPickedCreated, sizeof(aCreated));
	}
	else
	{
		char aStamp[64];
		str_timestamp_format(aStamp, sizeof(aStamp), "%H-%M_%d-%m-%Y");
		char aRel[512];
		str_format(aRel, sizeof(aRel), "data/tas/%s_%s.tas", aMapSafe, aStamp);
		Storage()->GetBinaryPathAbsolute(aRel, aPath, sizeof(aPath));
		char aDir[512];
		Storage()->GetBinaryPathAbsolute("data/tas", aDir, sizeof(aDir));
		fs_makedir(aDir);
		str_timestamp_format(aCreated, sizeof(aCreated), "%H:%M | %d.%m.%Y");
	}

	if(!WriteTakeFile(aPath, pMap ? pMap : "", pName ? pName : "", pAuthor ? pAuthor : "", aCreated))
		return;

	if(m_FilePicked)
	{
		str_copy(m_aPickedMap, pMap ? pMap : "", sizeof(m_aPickedMap));
		str_copy(m_aPickedName, pName ? pName : "", sizeof(m_aPickedName));
		str_copy(m_aPickedAuthor, pAuthor ? pAuthor : "", sizeof(m_aPickedAuthor));
	}
}
