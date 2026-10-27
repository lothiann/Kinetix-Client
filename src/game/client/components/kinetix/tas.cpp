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
#include <game/client/components/mapimages.h>
#include <game/client/prediction/entities/door.h>
#include <game/client/prediction/entities/dragger.h>
#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/pickup.h>
#include <game/client/prediction/entities/plasma.h>
#include <game/client/prediction/entities/projectile.h>
#include <game/client/render.h>

#include <algorithm>
#include <utility>
#include <cstdlib>
#include <cmath>
#include <cstdio>
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
	m_vSpawnerRecords.clear();
	m_Spawner = STasSpawnerSettings{};
	m_SpawnerHasPrev = false;
	m_SpawnerPrevPos = vec2(0.0f, 0.0f);
	m_SpawnerLoop = false;
	m_SpawnerLoopMs = 0.0f;
	m_vPgChat.clear();
	m_vPgRcon.clear();
	m_PgMaster = false;
	m_PgVirtualRcon = false;
	m_PgVirtualChat = false;
	m_PgPractice = false;
	m_PgInvincible = false;
	m_PgSuper = false;
	m_PgPaused = false;
	m_PgSpec = false;
	m_PgTuningInit = false;
	m_PgTuningDirty = false;
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

// v6.2: shadows the engine's "rcon" command. While the playground's Virtual
// RCON is on, rcon lines run against the playground instead of the server (no
// login needed); otherwise the call is passed straight through, so the real
// rcon keeps working exactly as before.
static void ConTasRcon(IConsole::IResult *pResult, void *pUserData)
{
	CTas *pThis = (CTas *)pUserData;
	const char *pLine = pResult->GetString(0);
	if(pThis->IsJoined() && pThis->PgMaster() && pThis->PgVirtualRcon())
	{
		pThis->PgRconEcho(pLine);
		pThis->PgRconExecute(pLine);
		return;
	}
	pThis->SendRealRcon(pLine);
}

void CTas::OnConsoleInit()
{
	Console()->Register("kx_tas_forward", "", CFGFLAG_CLIENT, ConTasForward, this, "TAS: step one tick forward");
	Console()->Register("kx_tas_rewind", "", CFGFLAG_CLIENT, ConTasRewind, this, "TAS: step one tick backward");
	Console()->Register("rcon", "r[rcon-command]", CFGFLAG_CLIENT, ConTasRcon, this, "Send specified command to rcon");
}

void CTas::SendRealRcon(const char *pLine)
{
	Client()->Rcon(pLine);
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

	// /spec (PAUSE_SPEC) inside the sandbox: engage the engine's own spectator
	// mode — freeview camera (CAMTYPE_SPEC) and the spec UI — by overriding the
	// snap SpecInfo. The real server is never touched; OnNewSnapshot restores
	// the real values, so the override is reapplied every tick.
	if(m_PgSpec)
	{
		auto &Spec = GameClient()->m_Snap.m_SpecInfo;
		Spec.m_Active = true;
		Spec.m_SpectatorId = SPEC_FREEVIEW;
		Spec.m_UsePosition = false;
	}

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
		// A new map means a new tuning list: re-seed on the next join.
		m_PgTuningInit = false;
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
	// Raw tick delta of this frame (before TPS scaling) — used by the Spawner
	// loop timer, which runs in real milliseconds.
	const float RawPredDelta = m_LastPredClock < 0.0f ? 0.0f : std::max(0.0f, PredClock - m_LastPredClock);
	if(m_LastPredClock < 0.0f)
		m_LastPredClock = PredClock;
	float ClockDelta = (PredClock - m_LastPredClock) * m_Tps / (float)Client()->GameTickSpeed();
	m_LastPredClock = PredClock;
	if(ClockDelta < 0.0f)
		ClockDelta = 0.0f;
	if(ClockDelta > 10.0f)
		ClockDelta = 10.0f;

	// Spawner loop: real-time timer (runs even while the TAS is paused).
	if(m_SpawnerLoop)
	{
		m_SpawnerLoopMs += RawPredDelta * (1000.0f / (float)Client()->GameTickSpeed());
		const float Delay = (float)std::clamp(m_Spawner.m_LoopDelay, 20, 10000);
		int Guard = 0;
		while(m_SpawnerLoopMs >= Delay && Guard++ < 4)
		{
			m_SpawnerLoopMs -= Delay;
			if(!SpawnerSpawn())
				break;
		}
	}

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

// Hammer hit for the playground: copy of CEffects::HammerHit with the
// lifetime divided by the TPS factor, so the burst expands and fades at the
// same slowed-down pace the world itself runs at.
static void SpawnPlaygroundHammerHit(CGameClient *pGame, vec2 Pos, float Scale)
{
	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_HIT01;
	p.m_Pos = Pos;
	p.m_LifeSpan = 0.3f / Scale;
	p.m_StartSize = 120.0f;
	p.m_EndSize = 0.0f;
	p.m_Rot = random_angle();
	p.m_Color.a = 1.0f;
	p.m_StartAlpha = 1.0f;
	pGame->m_Particles.Add(CParticles::GROUP_EXPLOSIONS, &p);
	if(g_Config.m_SndGame)
		pGame->m_Sounds.PlayAt(CSounds::CHN_WORLD, SOUND_HAMMER_HIT, 1.0f, Pos);
}

void CTas::OnRender()
{
	RenderVisuals();

	// v6.2: CCollision::SetCollisionAt writes into the map's own game layer
	// data, but the map renderer keeps pre-built GPU buffers — rebuild them so
	// spawned/removed tiles appear exactly like normal map tiles (including
	// with cl_overlay_entities = 100).
	if(m_TileVisualDirty)
	{
		m_TileVisualDirty = false;
		if(GameClient()->Layers() && GameClient()->Layers()->GameLayer())
		{
			GameClient()->m_MapLayersBackground.OnMapLoad();
			GameClient()->m_MapLayersForeground.OnMapLoad();
		}
	}

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
				SpawnPlaygroundHammerHit(pGame, EventIt->m_Pos, m_Tps / 50.0f);
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

	// Speedup arrows for maps without a speedup layer: the native renderer has
	// no layer to draw them from, so draw the arrow here (the physics still
	// works through the collision fallback layer).
	for(const SSpawnedTile &T : m_vSpawnedTiles)
	{
		if(!T.m_SpeedupSet || !T.m_SpeedupSelfDraw)
			continue;
		int Type = 0, Force = 0, MaxSpeed = 0, Angle = 0;
		m_Playground.Collision()->GetSpeedupTileAt(T.m_Pos.x + 16.0f, T.m_Pos.y + 16.0f, &Type, &Force, &MaxSpeed, &Angle);
		if(!Force && !MaxSpeed)
			continue;
		// Exactly what CRenderMap::RenderSpeedupOverlay draws for native tiles:
		// the speedup arrow sprite from the game data, rotated by the angle.
		Graphics()->TextureClear();
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_SPEEDUP_ARROW].m_Id);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		Graphics()->SelectSprite(SPRITE_SPEEDUP_ARROW);
		Graphics()->QuadsSetRotation(Angle * (pi / 180.0f));
		Graphics()->DrawSprite(T.m_Pos.x + 16.0f, T.m_Pos.y + 16.0f, 35.0f);
		Graphics()->QuadsEnd();
	}

	// Tele tiles for maps without a tele layer: draw with the NATIVE entity
	// texture using the exact CRenderMap::RenderTelemap atlas math (same
	// sprites, same top-left anchored quads → same alignment).
	{
		const bool TexArrays = Graphics()->HasTextureArraysSupport();
		const CScreenRect ScreenRect = Graphics()->GetScreen();
		const float TilePixelSize = 1024 / 32.0f;
		const float FinalTileSize = 32.0f / ScreenRect.Width() * Graphics()->ScreenWidth();
		const float FinalTilesetScale = FinalTileSize / TilePixelSize;
		const float TexSize = 1024.0f;
		const float Frac = (1.25f / TexSize) * (1 / FinalTilesetScale);
		const float Nudge = (0.5f / TexSize) * (1 / FinalTilesetScale);
		Graphics()->TextureSet(pGame->m_MapImages.GetEntities(MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH));
		if(TexArrays)
			Graphics()->QuadsTex3DBegin();
		else
			Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		for(const SSpawnedTile &T : m_vSpawnedTiles)
		{
			if(!T.m_TeleSet || !T.m_TeleSelfDraw || !T.m_Tile)
				continue;
			const int Index = T.m_Tile;
			const int tx = Index % 16;
			const int ty = Index / 16;
			const int Px0 = tx * (1024 / 16);
			const int Py0 = ty * (1024 / 16);
			const int Px1 = Px0 + (1024 / 16) - 1;
			const int Py1 = Py0 + (1024 / 16) - 1;
			float x0 = Nudge + Px0 / TexSize + Frac;
			float y0 = Nudge + Py0 / TexSize + Frac;
			float x1 = Nudge + Px1 / TexSize - Frac;
			float y1 = Nudge + Py0 / TexSize + Frac;
			float x2 = Nudge + Px1 / TexSize - Frac;
			float y2 = Nudge + Py1 / TexSize - Frac;
			float x3 = Nudge + Px0 / TexSize + Frac;
			float y3 = Nudge + Py1 / TexSize - Frac;
			if(TexArrays)
			{
				x0 = 0;
				y0 = 0;
				x1 = x0 + 1;
				y1 = y0;
				x2 = x0 + 1;
				y2 = y0 + 1;
				x3 = x0;
				y3 = y0 + 1;
			}
			const IGraphics::CQuadItem QuadItem(T.m_Pos.x, T.m_Pos.y, 32.0f, 32.0f);
			if(TexArrays)
			{
				Graphics()->QuadsSetSubsetFree(x0, y0, x1, y1, x2, y2, x3, y3, Index);
				Graphics()->QuadsTex3DDrawTL(&QuadItem, 1);
			}
			else
			{
				Graphics()->QuadsSetSubsetFree(x0, y0, x1, y1, x2, y2, x3, y3);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
			}
		}
		if(TexArrays)
			Graphics()->QuadsTex3DEnd();
		else
			Graphics()->QuadsEnd();

		// Number overlay on top, centered like the native tele overlay.
		for(const SSpawnedTile &T : m_vSpawnedTiles)
		{
			if(!T.m_TeleSet || !T.m_TeleSelfDraw || !T.m_Number)
				continue;
			char aBuf[16];
			str_format(aBuf, sizeof(aBuf), "%d", T.m_Number);
			const float Width = TextRender()->TextWidth(10.0f, aBuf, -1);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			TextRender()->Text(T.m_Pos.x + 16.0f - Width / 2.0f, T.m_Pos.y + 16.0f - 5.0f, 10.0f, aBuf, -1.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}

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
			{
				// Native rate (4*pi rad/s like CItems::RenderProjectile), but
				// phased by this projectile's own age instead of a global
				// timer — grenades spawned at different ticks spin out of sync.
				Graphics()->QuadsSetRotation(Ct * pi * 2.0f * 2.0f + (float)pEnt->GetId() * 0.1f);
			}
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

	// The real player renderer widens the culling rect by a tee-sized border so
	// tees do not pop out while only their weapon/hook is still on screen.
	CScreenRect ScreenRect = Graphics()->GetScreen();
	ScreenRect.Expand(100.0f);
	// Same layering as the regular player renderer: every hook chain first, then
	// every tee. The local hook is drawn here, the spawned hooks + tiles + tees
	// inside RenderSpawned (called between the two), so a hook from any tee can
	// never cover another tee.
	// v6.2: hook coll preview first — exactly like the real client, it sits
	// UNDER every tee and hook chain (previously it was drawn last and covered
	// the tee).
	RenderPlaygroundHookColl();
	if(m_PgSpec)
	{
		// /spec (PAUSE_SPEC): the engine renders a paused player as the
		// SPECCHAR ghost — the white spectator skin, idle animation and blink
		// emote, no hook (CPlayers::OnRender's spectating-players loop). Using
		// the tee's own skin made the ghost look like a normal standing tee.
		const CTeeRenderInfo &SpecInfo = GameClient()->m_Players.SpectatorTeeRenderInfo()->TeeRenderInfo();
		RenderTools()->RenderTee(CAnimState::GetIdle(), &SpecInfo, EMOTE_BLINK, vec2(1.0f, 0.0f), PlaygroundTeeRenderPos(), 1.0f);
		RenderSpawned();
	}
	else
	{
		if(Cur.m_HookState > 0)
			pGame->m_Players.RenderHook(ScreenRect, &Prev, &Cur, &Info, -1, IntraAlpha);
		RenderSpawned();
		pGame->m_Players.RenderPlayer(ScreenRect, &Prev, &Cur, &Info, -1, IntraAlpha);
	}

	// The name plate component is disabled while the playground is active, so the
	// fake tee's own name plate is rendered through it manually (same settings
	// as every other tee: name size, clan, id, direction, strong/weak).
	{
		const CGameClient::CClientData &LocalClient = pGame->m_aClients[m_LocalId];
		int LocalStrongWeakId = 0;
		if(m_LocalId >= 0 && m_LocalId < MAX_CLIENTS)
		{
			const CGameClient::CSnapState::CCharacterInfo &LocalChar = pGame->m_Snap.m_aCharacters[m_LocalId];
			if(LocalChar.m_HasExtendedData)
				LocalStrongWeakId = LocalChar.m_ExtendedData.m_StrongWeakId;
		}
		const bool ShowDir = g_Config.m_ClShowDirection == 2 || g_Config.m_ClShowDirection == 3;
		pGame->m_NamePlates.RenderNamePlateGameCustom(PlaygroundTeeRenderPos(), m_LocalId,
			LocalClient.m_aName, LocalClient.m_aClan, m_LocalId, true, LocalStrongWeakId, false,
			ShowDir, m_LastInput.m_Direction == -1, m_LastInput.m_Jump == 1, m_LastInput.m_Direction == 1,
			1.0f);
	}
}

void CTas::RenderFreezeBar()
{
	if(!m_Joined || !g_Config.m_ClShowFreezeBars)
		return;

	auto DrawBar = [&](CCharacter *pChar, const vec2 &BarPos) {
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
		GameClient()->m_FreezeBars.RenderFreezeBarPos(BarPos.x - 32.0f, BarPos.y + 32.0f, 64.0f, 16.0f, FreezeProgress, BarAlpha);
	};

	// Local playground tee.
	DrawBar(m_Playground.GetCharacterById(m_LocalId), PlaygroundTeeRenderPos());

	// v6.3: every spawned tee too — they freeze on freeze tiles / freeze
	// weapons, and previously only the local tee got a bar.
	const float Intra = m_Paused ? 0.0f : RenderIntraAlpha();
	for(const SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(E.m_Entity != TAS_SPAWN_ENT_TEE || !E.m_pEnt)
			continue;
		const vec2 Pos = E.m_Inserted
			? mix(E.m_RenderPrevPos, E.m_RenderCurPos, Intra)
			: E.m_Pos;
		DrawBar(static_cast<CCharacter *>(E.m_pEnt), Pos);
	}
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

// ---------------------------------------------------------------------------
// Spawner (TAS playground only)
// ---------------------------------------------------------------------------

struct STasSpawnerTileDef
{
	int m_Id;
	const char *m_pName;
};

static const STasSpawnerTileDef g_aTasSpawnerTiles[] = {
	{TILE_AIR, "Air"},
	{TILE_SOLID, "Solid"},
	{TILE_DEATH, "Death"},
	{TILE_NOHOOK, "Unhookable"},
	{TILE_NOLASER, "NoLaser"},
	{TILE_THROUGH_CUT, "ThroughCut"},
	{TILE_THROUGH, "Through"},
	{TILE_JUMP, "Jump"},
	{TILE_FREEZE, "Freeze"},
	{TILE_UNFREEZE, "Unfreeze"},
	{TILE_DFREEZE, "DeepFreeze"},
	{TILE_DUNFREEZE, "DeepUnfreeze"},
	{TILE_WALLJUMP, "WallJump"},
	{TILE_REFILL_JUMPS, "RefillJumps"},
	{TILE_START, "Start"},
	{TILE_FINISH, "Finish"},
	{TILE_STOP, "Stop"},
	{TILE_STOPS, "StopS"},
	{TILE_STOPA, "StopA"},
	{TILE_CP, "Checkpoint"},
	{TILE_CP_F, "CheckpointFinish"},
	{TILE_THROUGH_ALL, "ThroughAll"},
	{TILE_THROUGH_DIR, "ThroughDir"},
	{TILE_TUNE, "Tune"},
	{TILE_OLDLASER, "OldLaser"},
	{TILE_NPC, "NPC"},
	{TILE_EHOOK, "EHook"},
	{TILE_NOHIT, "NoHit"},
	{TILE_NPH, "NPH"},
	{TILE_UNLOCK_TEAM, "UnlockTeam"},
	{TILE_ADD_TIME, "AddTime"},
	{TILE_SUBTRACT_TIME, "SubtractTime"},
	{TILE_LFREEZE, "LaserFreeze"},
	{TILE_LUNFREEZE, "LaserUnfreeze"},
	// Special-layer tiles (v6.3): written into their own map layers, not the
	// game layer — that is what makes them actually work client-side.
	{TILE_SPEED_BOOST, "Speedup"},
	{TILE_TELEIN, "TeleIn"},
	{TILE_TELEINEVIL, "TeleIn Evil"},
	{TILE_TELEOUT, "TeleOut"},
	{TILE_TELECHECKIN, "TeleCheckIn"},
};

static const char *const g_apTasSpawnerEntities[] = {
	"Tee", "Projectile", "Laser", "Pickup", "Door", "Dragger", "Plasma",
};

static_assert((int)(sizeof(g_aTasSpawnerTiles) / sizeof(g_aTasSpawnerTiles[0])) == TAS_SPAWNER_TILE_COUNT, "spawner tile table size mismatch");
static_assert((int)(sizeof(g_apTasSpawnerEntities) / sizeof(g_apTasSpawnerEntities[0])) == TAS_SPAWN_ENT_COUNT, "spawner entity table size mismatch");

const char *CTas::SpawnerEntityName(int Index)
{
	if(Index < 0 || Index >= TAS_SPAWN_ENT_COUNT)
		return "";
	return g_apTasSpawnerEntities[Index];
}

const char *CTas::SpawnerTileName(int Index)
{
	if(Index < 0 || Index >= TAS_SPAWNER_TILE_COUNT)
		return "";
	return g_aTasSpawnerTiles[Index].m_pName;
}

int CTas::SpawnerTileId(int Index)
{
	if(Index < 0 || Index >= TAS_SPAWNER_TILE_COUNT)
		return 0;
	return g_aTasSpawnerTiles[Index].m_Id;
}

static ColorRGBA TasSpawnerHexColor(const char *pHex)
{
	unsigned Hex = 0xffffff;
	if(pHex && pHex[0])
		sscanf(pHex, "%6x", &Hex);
	return ColorRGBA((int)((Hex >> 16) & 0xff), (int)((Hex >> 8) & 0xff), (int)(Hex & 0xff), 255);
}

// Direction presets for non-tee entities (index = m_Dir, screen coords: +y down).
static const vec2 g_aTasSpawnerDirs[8] = {
	vec2(1.0f, 0.0f), vec2(1.0f, -1.0f), vec2(0.0f, -1.0f), vec2(-1.0f, -1.0f),
	vec2(-1.0f, 0.0f), vec2(-1.0f, 1.0f), vec2(0.0f, 1.0f), vec2(1.0f, 1.0f),
};

static vec2 TasSpawnerDir(int Dir)
{
	return normalize(g_aTasSpawnerDirs[std::clamp(Dir, 0, 7)]);
}

// Render info of a spawned tee: honors the custom skin name (loaded through
// the regular skin pipeline, community skins included) and the custom body /
// feet colors; falls back to the local player's look.
CTeeRenderInfo CTas::SpawnerTeeRenderInfo(const STasSpawnerSettings &S)
{
	CGameClient *pGame = GameClient();
	const char *pSkin = S.m_aSkin[0] ? S.m_aSkin : pGame->m_aClients[m_LocalId].m_aSkinName;
	char aKey[192];
	str_format(aKey, sizeof(aKey), "%s|%s|%s", pSkin, S.m_aColorBody, S.m_aColorFeet);
	auto It = m_SpawnerSkinCache.find(aKey);
	if(It == m_SpawnerSkinCache.end())
	{
		CTeeRenderInfo Info;
		Info.m_Size = 64.0f;
		CSkinDescriptor Desc;
		Desc.m_Flags |= CSkinDescriptor::FLAG_SIX;
		str_copy(Desc.m_aSkinName, pSkin, sizeof(Desc.m_aSkinName));
		auto pInfo = pGame->CreateManagedTeeRenderInfo(Info, Desc);
		pInfo->TeeRenderInfo().m_Size = 64.0f;
		const bool CustomBody = S.m_aColorBody[0] != 0;
		const bool CustomFeet = S.m_aColorFeet[0] != 0;
		if(CustomBody || CustomFeet)
		{
			pInfo->TeeRenderInfo().m_CustomColoredSkin = true;
			pInfo->TeeRenderInfo().m_ColorBody = CustomBody ? TasSpawnerHexColor(S.m_aColorBody) : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
			pInfo->TeeRenderInfo().m_ColorFeet = CustomFeet ? TasSpawnerHexColor(S.m_aColorFeet) : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
		}
		It = m_SpawnerSkinCache.emplace(aKey, std::move(pInfo)).first;
	}
	CTeeRenderInfo Out = It->second->TeeRenderInfo();
	Out.m_Size = 64.0f;
	return Out;
}

vec2 CTas::SpawnerAimPos()
{
	// "Cursor" is the AIM point (the crosshair), not the OS pointer.
	// CControls::m_aTargetPos is stored in unzoomed camera space: the HUD
	// draws the crosshair with MapScreenToWorld(..., Zoom = 1). The world
	// point under it is therefore cam + (target - cam) * zoom — exactly the
	// conversion the Pathfinder tile editor uses (pathfinder_tab.cpp).
	// A real spectator is the exception: there the client draws the spec
	// cursor from CCursorInfo::WorldTarget(), already a world point.
	CGameClient *pGame = GameClient();
	if(pGame->m_Snap.m_SpecInfo.m_Active && pGame->m_CursorInfo.IsAvailable())
		return pGame->m_CursorInfo.WorldTarget();
	const vec2 Cam = pGame->m_Camera.m_Center;
	const float Zoom = pGame->m_Camera.m_Zoom;
	return Cam + (pGame->m_Controls.m_aTargetPos[g_Config.m_ClDummy] - Cam) * Zoom;
}

vec2 CTas::SpawnerTargetPos()
{
	CGameClient *pGame = GameClient();
	vec2 Pos(0.0f, 0.0f);
	if(m_Spawner.m_CoordsMode == TAS_SPAWN_COORD_COORDS)
		Pos = vec2(m_Spawner.m_X, m_Spawner.m_Y);
	else if(m_Spawner.m_CoordsMode == TAS_SPAWN_COORD_PREVIOUS)
		Pos = m_SpawnerPrevPos;
	else
		Pos = SpawnerAimPos();
	if(m_Spawner.m_Snap)
		Pos = vec2(floorf(Pos.x / 32.0f) * 32.0f, floorf(Pos.y / 32.0f) * 32.0f);
	return Pos;
}

bool CTas::SpawnerSpawn()
{
	if(!m_Joined)
		return false;

	vec2 Pos = SpawnerTargetPos();
	// Entities are positioned by their centre; the snapped value is the tile
	// origin, so move to the tile centre.
	if(m_Spawner.m_Mode == TAS_SPAWN_MODE_ENTITY && m_Spawner.m_Snap)
		Pos += vec2(16.0f, 16.0f);

	const bool Ok = SpawnerSpawnAt(m_Spawner, Pos, m_PlaygroundTick, true);
	if(Ok)
	{
		m_SpawnerHasPrev = true;
		m_SpawnerPrevPos = Pos;
	}
	return Ok;
}

bool CTas::SpawnerKill()
{
	if(!m_Joined)
		return false;

	const vec2 Pos = SpawnerTargetPos();
	bool Removed = false;

	if(m_Spawner.m_Mode == TAS_SPAWN_MODE_ENTITY)
	{
		for(auto It = m_vSpawnedEnts.begin(); It != m_vSpawnedEnts.end();)
		{
			const vec2 EPos = It->m_pEnt ? It->m_pEnt->GetPos() : It->m_Pos;
			if(distance(EPos, Pos) <= 32.0f)
			{
				// Respawn-style burst where the tee is removed.
				if(It->m_Entity == TAS_SPAWN_ENT_TEE)
					GameClient()->m_Effects.PlayerSpawn(EPos, 1.0f, 1.0f);
				if(It->m_Inserted)
					m_Playground.RemoveEntity(It->m_pEnt);
				delete It->m_pEnt;
				It = m_vSpawnedEnts.erase(It);
				Removed = true;
			}
			else
				++It;
		}
		for(auto It = m_vSpawnerRecords.begin(); It != m_vSpawnerRecords.end();)
		{
			if(It->m_Mode == TAS_SPAWN_MODE_ENTITY && distance(It->m_Pos, Pos) <= 32.0f)
				It = m_vSpawnerRecords.erase(It);
			else
				++It;
		}
	}
	else
	{
		CCollision *pColl = m_Playground.Collision();
		if(!pColl)
			return false;
		const int Index = pColl->GetPureMapIndex(Pos.x + 16.0f, Pos.y + 16.0f);
		for(int i = (int)m_vSpawnedTiles.size() - 1; i >= 0; --i)
		{
			const SSpawnedTile &T = m_vSpawnedTiles[i];
			if(T.m_Index != Index)
				continue;
			if(T.m_Physics)
			{
				pColl->SetCollisionAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTile);
				if(T.m_TuneSet)
					pColl->RestoreTuneTileAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTuneType, T.m_OldTuneNumber);
				if(T.m_TeleSet)
					pColl->RestoreTeleTileAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTeleType, T.m_OldTeleNumber);
				if(T.m_SpeedupSet)
					pColl->RestoreSpeedupAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldSpeedupType, T.m_OldSpeedupForce, T.m_OldSpeedupMaxSpeed, T.m_OldSpeedupAngle);
			}
				m_vSpawnedTiles.erase(m_vSpawnedTiles.begin() + i);
				Removed = true;
				m_TileVisualDirty = true;
			}
		for(auto It = m_vSpawnerRecords.begin(); It != m_vSpawnerRecords.end();)
		{
			if(It->m_Mode == TAS_SPAWN_MODE_TILE && (int)It->m_Pos.x == (int)Pos.x && (int)It->m_Pos.y == (int)Pos.y)
				It = m_vSpawnerRecords.erase(It);
			else
				++It;
		}
	}
	return Removed;
}

void CTas::SpawnerToggleLoop()
{
	if(!m_Joined)
		return;
	m_SpawnerLoop = !m_SpawnerLoop;
	m_SpawnerLoopMs = 0.0f;
}

// ---------------------------------------------------------------------------
// Playground settings (v6.2): Virtual RCON / Virtual Chat / Practice
// ---------------------------------------------------------------------------

bool CTas::PlaygroundDirty() const
{
	return !m_vSpawnedEnts.empty() || !m_vSpawnedTiles.empty() ||
		!m_vSpawnerRecords.empty() || m_SpawnerLoop || !m_vPgChat.empty() ||
		!m_vPgRcon.empty() || m_PgInvincible || m_PgSuper || m_PgTuningDirty ||
		!m_vPlaygroundInputs.empty() || m_PgPaused || m_PgSpec;
}

void CTas::PlaygroundReset()
{
	// Absolutely the whole playground: spawned entities and tiles, spawner
	// records/settings/loop, chat + rcon buffers, practice cheats, private
	// tuning, virtual switches, the rewind/record timeline and the world
	// itself (leave + rejoin gives a pristine tee).
	SpawnerClearLive();
	m_vSpawnerRecords.clear();
	m_SpawnerHasPrev = false;
	m_SpawnerPrevPos = vec2(0.0f, 0.0f);
	m_SpawnerLoop = false;
	m_SpawnerLoopMs = 0.0f;
	m_Spawner = STasSpawnerSettings{};
	m_vPgChat.clear();
	m_vPgRcon.clear();
	m_PgInvincible = false;
	m_PgSuper = false;
	m_PgPaused = false;
	m_PgSpec = false;
	m_PgTuningDirty = false;
	m_PgTuningInit = false;
	m_PgVirtualRcon = false;
	m_PgVirtualChat = false;
	m_PgPractice = false;
	m_PgMaster = false;
	GameClient()->m_Chat.TasSetVirtual(false);
	// The record/play take is a SEPARATE buffer — Reset Record owns it. Reset
	// Playground only wipes the world (including its rewind history below).
	m_PgResetSerial++;
	m_vPlaygroundHistory.clear();
	m_vPlaygroundInputs.clear();
	if(m_Joined)
	{
		LeavePlayground();
		JoinPlayground();
	}
}

void CTas::ResetRecord()
{
	// Stops any playback/recording and drops the take + the loaded file. The
	// playground world (tee, spawner, tiles, chat) is left untouched.
	m_Recording = false;
	m_Playing = false;
	m_RealPlaying = false;
	m_Desynced = false;
	m_PlayIdx = 0;
	m_RealIdx = 0;
	m_TakeFromFile = false;
	m_vRecord.clear();
	m_vSaved.clear();
	UnpickFile();
}

// All four setters are called every frame by the ClickGUI sync, so they must
// be idempotent: acting on an unchanged value replayed the rcon backlog over
// and over (the "tens of thousands of lines" flood).
void CTas::SetPgMaster(bool On)
{
	if(m_PgMaster == On)
		return;
	m_PgMaster = On;
	if(!On)
	{
		// Master off: every virtual subsystem steps aside (their individual
		// toggles and buffers are kept, so turning it back on restores them).
		GameClient()->m_Chat.TasSetVirtual(false);
	}
	else if(m_Joined && m_PgVirtualChat)
		GameClient()->m_Chat.TasSetVirtual(true);
}

void CTas::SetPgVirtualChat(bool On)
{
	if(m_PgVirtualChat == On)
		return;
	m_PgVirtualChat = On;
	if(m_Joined && m_PgMaster && On)
		GameClient()->m_Chat.TasSetVirtual(true);
	else
		GameClient()->m_Chat.TasSetVirtual(false);
}

void CTas::SetPgPractice(bool On)
{
	// Practice only gates the virtual commands; nothing is swapped or reset
	// here, so the setting can be flipped freely.
	m_PgPractice = On;
}

void CTas::PgChatAddLine(int ClientId, int Team, const char *pName, const char *pText)
{
	SPgChatLine Line;
	Line.m_ClientId = ClientId;
	Line.m_Team = Team;
	str_copy(Line.m_aName, pName ? pName : "", sizeof(Line.m_aName));
	str_copy(Line.m_aText, pText ? pText : "", sizeof(Line.m_aText));
	m_vPgChat.push_back(Line);
	constexpr int MAX_PG_CHAT = 256;
	if((int)m_vPgChat.size() > MAX_PG_CHAT)
		m_vPgChat.erase(m_vPgChat.begin(), m_vPgChat.begin() + ((int)m_vPgChat.size() - MAX_PG_CHAT));
}

void CTas::PgTeleCursor()
{
	if(!m_Joined)
		return;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return;
	// /tc always teleports to the crosshair, regardless of the spawner's
	// Coordinates Mode or the Snap toggle.
	const vec2 Pos = SpawnerAimPos();
	CCharacterCore Core = pChar->GetCore();
	Core.m_Pos = Pos;
	Core.m_Vel = vec2(0.0f, 0.0f);
	Core.m_HookState = HOOK_IDLE;
	Core.m_HookPos = Pos;
	Core.m_HookDir = vec2(1.0f, 0.0f);
	pChar->SetCore(Core);
	pChar->m_Pos = Pos;
	pChar->m_PrevPos = Pos;
	pChar->m_PrevPrevPos = Pos;
	RebaseCounters();
}

// Weapon name -> weapon id (playground commands).
static int PgWeaponIdFromName(const char *pName)
{
	if(str_comp_nocase(pName, "hammer") == 0)
		return WEAPON_HAMMER;
	if(str_comp_nocase(pName, "gun") == 0 || str_comp_nocase(pName, "pistol") == 0)
		return WEAPON_GUN;
	if(str_comp_nocase(pName, "shotgun") == 0)
		return WEAPON_SHOTGUN;
	if(str_comp_nocase(pName, "grenade") == 0)
		return WEAPON_GRENADE;
	if(str_comp_nocase(pName, "laser") == 0 || str_comp_nocase(pName, "rifle") == 0)
		return WEAPON_LASER;
	if(str_comp_nocase(pName, "ninja") == 0)
		return WEAPON_NINJA;
	return -1;
}

void CTas::PgGiveWeapon(int Weapon)
{
	if(!m_Joined || Weapon < 0 || Weapon >= NUM_WEAPONS)
		return;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return;
	pChar->GiveWeapon(Weapon, false);
	pChar->SetActiveWeapon(Weapon);
}

// /kill — respawn the tee at the join position. The Spawner world is kept.
void CTas::PgRespawn()
{
	if(!m_Joined)
		return;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return;
	vec2 Pos = pChar->Core()->m_Pos;
	if(!m_vPlaygroundHistory.empty())
		Pos = m_vPlaygroundHistory.front().Core.m_Pos;
	CCharacterCore Core = pChar->GetCore();
	Core.m_Pos = Pos;
	Core.m_Vel = vec2(0.0f, 0.0f);
	Core.m_HookState = HOOK_IDLE;
	Core.m_HookPos = Pos;
	Core.m_DeepFrozen = false;
	Core.m_LiveFrozen = false;
	pChar->SetCore(Core);
	pChar->m_Pos = Pos;
	pChar->m_PrevPos = Pos;
	pChar->m_PrevPrevPos = Pos;
	pChar->m_FreezeTime = 0;
	RebaseCounters();
}

void CTas::PgRconPrint(const char *pText)
{
	if(!pText || !pText[0])
		return;
	m_vPgRcon.push_back(pText);
	constexpr int MAX_PG_RCON = 400;
	if((int)m_vPgRcon.size() > MAX_PG_RCON)
		m_vPgRcon.erase(m_vPgRcon.begin(), m_vPgRcon.begin() + ((int)m_vPgRcon.size() - MAX_PG_RCON));
	GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, pText);
}

void CTas::PgRconLogCommand(const char *pLine)
{
	char aBuf[768];
	str_format(aBuf, sizeof(aBuf), "> %s", pLine ? pLine : "");
	m_vPgRcon.push_back(aBuf);
	constexpr int MAX_PG_RCON = 400;
	if((int)m_vPgRcon.size() > MAX_PG_RCON)
		m_vPgRcon.erase(m_vPgRcon.begin(), m_vPgRcon.begin() + ((int)m_vPgRcon.size() - MAX_PG_RCON));
}

void CTas::PgRconEcho(const char *pLine)
{
	char aBuf[768];
	str_format(aBuf, sizeof(aBuf), "> %s", pLine ? pLine : "");
	PgRconLogCommand(pLine);
	GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, aBuf);
}

// Shared playground command core. Used by the practice chat (/command) and the
// virtual rcon (command without the slash). Cheat commands require Practice,
// exactly like on a DDNet server; tune/tune_reset work regardless.
bool CTas::PgRunCommand(const char *pCmdLine, std::vector<std::string> &vOut)
{
	vOut.clear();
	if(!pCmdLine || !pCmdLine[0])
		return false;

	char aTok[6][128];
	int TokCount = 0;
	{
		const char *p = pCmdLine;
		while(*p && TokCount < 6)
		{
			while(*p == ' ')
				++p;
			if(!*p)
				break;
			int n = 0;
			while(*p && *p != ' ' && n < (int)sizeof(aTok[0]) - 1)
				aTok[TokCount][n++] = *p++;
			aTok[TokCount][n] = '\0';
			++TokCount;
		}
	}
	if(TokCount == 0)
		return false;
	char aCmd[64];
	char aArg1[128] = "";
	char aArg2[128] = "";
	char aArg3[128] = "";
	str_copy(aCmd, aTok[0], sizeof(aCmd));
	if(TokCount > 1)
		str_copy(aArg1, aTok[1], sizeof(aArg1));
	if(TokCount > 2)
		str_copy(aArg2, aTok[2], sizeof(aArg2));
	if(TokCount > 3)
		str_copy(aArg3, aTok[3], sizeof(aArg3));

	auto Out = [&](const char *pText) {
		vOut.push_back(pText);
	};

	if(str_comp_nocase(aCmd, "help") == 0)
	{
		Out("Practice: super/unsuper, invincible [0|1], infinite_jump, uninfinite_jump, endless_hook, unendless_hook, deep/undeep, livefreeze/unlivefreeze, freeze/unfreeze, addweapon/removeweapon <id>, weapons, unweapons, shotgun, grenade, laser/rifle, ninja/unninja, setjumps <n>, solo/unsolo, left/right/up/down [n], move/move_raw/tpxy <x> <y>, tp/teleport/tc, rescue, kill, pause/spec");
		Out("Tuning: tune <name> [value], tunes, tune_reset [name], tune_zone <zone> <name> [value], tune_zone_dump <zone>, tune_zone_reset [zone], toggle_tune <name> <v1> <v2>, tps <n>, pause_game");
		return true;
	}
	if(str_comp_nocase(aCmd, "clear") == 0)
	{
		PgChatClear();
		Out("Playground chat cleared");
		return true;
	}
	if(!m_Joined)
	{
		Out("Playground: not joined");
		return true;
	}

	// ---- tuning family, mirroring the server commands ----
	if(str_comp_nocase(aCmd, "tune") == 0)
	{
		CTuningParams *pList = m_Playground.TuningList();
		if(!pList)
		{
			Out("tune: playground not initialised");
			return true;
		}
		if(!aArg1[0])
		{
			Out("usage: tune <name> [value]");
			return true;
		}
		char aBuf[256];
		if(!aArg2[0])
		{
			float Val = 0.0f;
			if(!pList->Get(aArg1, &Val))
				str_format(aBuf, sizeof(aBuf), "tune: unknown parameter '%s'", aArg1);
			else
				str_format(aBuf, sizeof(aBuf), "tune: %s = %.4f", aArg1, (double)Val);
			Out(aBuf);
			return true;
		}
		if(!pList->Set(aArg1, (float)atof(aArg2)))
			str_format(aBuf, sizeof(aBuf), "tune: unknown parameter '%s'", aArg1);
		else
		{
			m_PgTuningDirty = true;
			str_format(aBuf, sizeof(aBuf), "tune: %s = %.4f", aArg1, (double)atof(aArg2));
		}
		Out(aBuf);
		return true;
	}
	if(str_comp_nocase(aCmd, "tunes") == 0)
	{
		CTuningParams *pList = m_Playground.TuningList();
		if(!pList)
		{
			Out("tune: playground not initialised");
			return true;
		}
		for(int i = 0; i < CTuningParams::Num(); ++i)
		{
			float Val = 0.0f;
			pList->Get(i, &Val);
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "%s = %.4f", CTuningParams::Name(i), (double)Val);
			Out(aBuf);
		}
		return true;
	}
	if(str_comp_nocase(aCmd, "tune_reset") == 0)
	{
		if(aArg1[0])
		{
			CTuningParams *pList = m_Playground.TuningList();
			for(int i = 0; pList && i < CTuningParams::Num(); ++i)
			{
				if(str_comp_nocase(CTuningParams::Name(i), aArg1) != 0)
					continue;
				float Val = 0.0f;
				GameClient()->GetTuning(0)->Get(i, &Val);
				pList->Set(i, Val);
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "tune_reset: %s = %.4f", aArg1, (double)Val);
				Out(aBuf);
				return true;
			}
			Out("tune_reset: unknown parameter");
			return true;
		}
		for(int i = 0; i < TuneZone::NUM; ++i)
			m_aPgTuning[i] = *GameClient()->GetTuning(i);
		m_PgTuningDirty = false;
		Out("tune_reset: playground tuning restored");
		return true;
	}
	if(str_comp_nocase(aCmd, "tune_zone") == 0)
	{
		const int Zone = atoi(aArg1);
		char aBuf[256];
		if(Zone < 0 || Zone >= TuneZone::NUM || !aArg2[0])
		{
			Out("usage: tune_zone <zone> <name> [value]");
			return true;
		}
		if(!aArg3[0])
		{
			float Val = 0.0f;
			if(!m_aPgTuning[Zone].Get(aArg2, &Val))
				str_format(aBuf, sizeof(aBuf), "tune_zone: unknown parameter '%s'", aArg2);
			else
				str_format(aBuf, sizeof(aBuf), "tune_zone %d: %s = %.4f", Zone, aArg2, (double)Val);
			Out(aBuf);
			return true;
		}
		if(!m_aPgTuning[Zone].Set(aArg2, (float)atof(aArg3)))
			str_format(aBuf, sizeof(aBuf), "tune_zone: unknown parameter '%s'", aArg2);
		else
		{
			m_PgTuningDirty = true;
			str_format(aBuf, sizeof(aBuf), "tune_zone %d: %s = %.4f", Zone, aArg2, (double)atof(aArg3));
		}
		Out(aBuf);
		return true;
	}
	if(str_comp_nocase(aCmd, "tune_zone_dump") == 0)
	{
		const int Zone = atoi(aArg1);
		if(Zone < 0 || Zone >= TuneZone::NUM)
		{
			Out("usage: tune_zone_dump <zone>");
			return true;
		}
		for(int i = 0; i < CTuningParams::Num(); ++i)
		{
			float Val = 0.0f;
			m_aPgTuning[Zone].Get(i, &Val);
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "%s = %.4f", CTuningParams::Name(i), (double)Val);
			Out(aBuf);
		}
		return true;
	}
	if(str_comp_nocase(aCmd, "tune_zone_reset") == 0)
	{
		if(aArg1[0])
		{
			const int Zone = atoi(aArg1);
			if(Zone < 0 || Zone >= TuneZone::NUM)
			{
				Out("usage: tune_zone_reset [zone]");
				return true;
			}
			m_aPgTuning[Zone] = *GameClient()->GetTuning(Zone);
			Out("tune_zone_reset: zone restored");
			return true;
		}
		for(int i = 0; i < TuneZone::NUM; ++i)
			m_aPgTuning[i] = *GameClient()->GetTuning(i);
		m_PgTuningDirty = false;
		Out("tune_zone_reset: all zones restored");
		return true;
	}
	if(str_comp_nocase(aCmd, "toggle_tune") == 0)
	{
		CTuningParams *pList = m_Playground.TuningList();
		char aBuf[256];
		if(!pList || !aArg1[0] || !aArg2[0] || !aArg3[0])
		{
			Out("usage: toggle_tune <name> <value1> <value2>");
			return true;
		}
		float Cur = 0.0f;
		if(!pList->Get(aArg1, &Cur))
		{
			str_format(aBuf, sizeof(aBuf), "toggle_tune: unknown parameter '%s'", aArg1);
			Out(aBuf);
			return true;
		}
		const float V1 = (float)atof(aArg2);
		const float V2 = (float)atof(aArg3);
		const float NewVal = (fabsf(Cur - V1) < 0.0001f) ? V2 : V1;
		pList->Set(aArg1, NewVal);
		m_PgTuningDirty = true;
		str_format(aBuf, sizeof(aBuf), "toggle_tune: %s = %.4f", aArg1, (double)NewVal);
		Out(aBuf);
		return true;
	}
	if(str_comp_nocase(aCmd, "practice") == 0)
	{
		const bool On = aArg1[0] ? (atoi(aArg1) != 0) : true;
		SetPgPractice(On);
		Out(On ? "Practice enabled" : "Practice disabled");
		return true;
	}
	if(str_comp_nocase(aCmd, "unpractice") == 0)
	{
		SetPgPractice(false);
		Out("Practice disabled");
		return true;
	}
	// Engine: /pause and /spec are ordinary chat commands, not practice cheats.
	// The playground has no separate spectator, so /spec falls back to /pause
	// exactly as documented by the engine.
	if(str_comp_nocase(aCmd, "pause") == 0)
	{
		// Engine PAUSE_PAUSED: the tee stands still, the world keeps ticking.
		m_PgPaused = !m_PgPaused;
		if(m_PgPaused)
			m_PgSpec = false;
		Out(m_PgPaused ? "Paused" : "Unpaused");
		return true;
	}
	if(str_comp_nocase(aCmd, "spec") == 0)
	{
		// Engine PAUSE_SPEC: the tee stays paused in the world and the camera
		// detaches to the spectator view.
		m_PgSpec = !m_PgSpec;
		if(m_PgSpec)
			m_PgPaused = false;
		Out(m_PgSpec ? "Spectating" : "Resumed");
		return true;
	}
	if(str_comp_nocase(aCmd, "pause_game") == 0)
	{
		SetPaused(!Paused());
		Out(Paused() ? "Game paused" : "Game unpaused");
		return true;
	}
	if(str_comp_nocase(aCmd, "tps") == 0)
	{
		const float Tps = (float)atof(aArg1);
		if(Tps < 1.0f || Tps > 50.0f)
		{
			Out("usage: tps <1..50>");
			return true;
		}
		SetTps(Tps);
		Out("TPS set");
		return true;
	}

	// Everything below is a cheat command: it needs Practice.
	if(!m_PgPractice)
	{
		Out("Practice is disabled (enable Practice in Playground Settings)");
		return true;
	}

	if(str_comp_nocase(aCmd, "tc") == 0 || str_comp_nocase(aCmd, "telecursor") == 0)
	{
		PgTeleCursor();
		Out("Teleported to cursor");
		return true;
	}
	if(str_comp_nocase(aCmd, "tp") == 0 || str_comp_nocase(aCmd, "tele") == 0 || str_comp_nocase(aCmd, "teleport") == 0)
	{
		if(!aArg1[0] || !aArg2[0])
		{
			Out("usage: tp <x> <y>");
			return true;
		}
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const vec2 Pos((float)atof(aArg1), (float)atof(aArg2));
		CCharacterCore Core = pChar->GetCore();
		Core.m_Pos = Pos;
		Core.m_Vel = vec2(0.0f, 0.0f);
		Core.m_HookState = HOOK_IDLE;
		Core.m_HookPos = Pos;
		pChar->SetCore(Core);
		pChar->m_Pos = Pos;
		pChar->m_PrevPos = Pos;
		pChar->m_PrevPrevPos = Pos;
		RebaseCounters();
		Out("Teleported");
		return true;
	}
	if(str_comp_nocase(aCmd, "kill") == 0 || str_comp_nocase(aCmd, "respawn") == 0 || str_comp_nocase(aCmd, "rescue") == 0)
	{
		PgRespawn();
		Out("Respawned");
		return true;
	}
	if(str_comp_nocase(aCmd, "freeze") == 0 || str_comp_nocase(aCmd, "unfreeze") == 0 || str_comp_nocase(aCmd, "deep") == 0 || str_comp_nocase(aCmd, "undeep") == 0 || str_comp_nocase(aCmd, "livefreeze") == 0 || str_comp_nocase(aCmd, "unlivefreeze") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		if(str_comp_nocase(aCmd, "deep") == 0 || str_comp_nocase(aCmd, "undeep") == 0)
		{
			const bool On = str_comp_nocase(aCmd, "deep") == 0;
			CCharacterCore Core = pChar->GetCore();
			Core.m_DeepFrozen = On;
			pChar->SetCore(Core);
			Out(On ? "Deep frozen" : "Not deep frozen");
		}
		else if(str_comp_nocase(aCmd, "livefreeze") == 0 || str_comp_nocase(aCmd, "unlivefreeze") == 0)
		{
			const bool On = str_comp_nocase(aCmd, "livefreeze") == 0;
			CCharacterCore Core = pChar->GetCore();
			Core.m_LiveFrozen = On;
			pChar->SetCore(Core);
			Out(On ? "Live frozen" : "Not live frozen");
		}
		else if(str_comp_nocase(aCmd, "freeze") == 0)
		{
			pChar->Freeze();
			Out("Frozen");
		}
		else
		{
			pChar->Unfreeze();
			Out("Unfrozen");
		}
		return true;
	}
	// v6.3: engine-faithful practice flags. They live in CCharacterCore and the
	// client prediction already honours them (freeze/death/teleport immunity,
	// endless hook, deep/live freeze, ...).
	if(str_comp_nocase(aCmd, "super") == 0 || str_comp_nocase(aCmd, "unsuper") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const bool On = str_comp_nocase(aCmd, "super") == 0;
		CCharacterCore Core = pChar->GetCore();
		Core.m_Super = On;
		if(On && g_Config.m_SvEndlessSuperHook)
			Core.m_EndlessHook = true;
		pChar->SetCore(Core);
		m_PgSuper = On;
		Out(On ? "Super enabled" : "Super disabled");
		return true;
	}
	if(str_comp_nocase(aCmd, "invincible") == 0 || str_comp_nocase(aCmd, "inv") == 0 || str_comp_nocase(aCmd, "god") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const bool On = aArg1[0] ? (atoi(aArg1) != 0) : !pChar->GetCore().m_Invincible;
		CCharacterCore Core = pChar->GetCore();
		Core.m_Invincible = On;
		pChar->SetCore(Core);
		m_PgInvincible = On;
		Out(On ? "Invincible enabled" : "Invincible disabled");
		return true;
	}
	if(str_comp_nocase(aCmd, "infinite_jump") == 0 || str_comp_nocase(aCmd, "uninfinite_jump") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const bool On = str_comp_nocase(aCmd, "infinite_jump") == 0;
		CCharacterCore Core = pChar->GetCore();
		Core.m_EndlessJump = On;
		pChar->SetCore(Core);
		Out(On ? "Infinite jump enabled" : "Infinite jump disabled");
		return true;
	}
	if(str_comp_nocase(aCmd, "endless_hook") == 0 || str_comp_nocase(aCmd, "unendless_hook") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const bool On = str_comp_nocase(aCmd, "endless_hook") == 0;
		CCharacterCore Core = pChar->GetCore();
		Core.m_EndlessHook = On;
		pChar->SetCore(Core);
		Out(On ? "Endless hook enabled" : "Endless hook disabled");
		return true;
	}
	if(str_comp_nocase(aCmd, "giveweapons") == 0 || str_comp_nocase(aCmd, "giveweapon") == 0 || str_comp_nocase(aCmd, "weapons") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		char aRest[512];
		str_copy(aRest, pCmdLine, sizeof(aRest));
		char *pTok = aRest;
		while(*pTok && *pTok != ' ')
			pTok++;
		int Given = 0;
		if(!*pTok)
		{
			for(int w = 0; w < NUM_WEAPONS; ++w)
			{
				pChar->GiveWeapon(w, false);
				Given++;
			}
		}
		else
		{
			while(*pTok)
			{
				while(*pTok == ' ')
					pTok++;
				if(!*pTok)
					break;
				char aName[64];
				int n = 0;
				while(*pTok && *pTok != ' ' && n < (int)sizeof(aName) - 1)
					aName[n++] = *pTok++;
				aName[n] = '\0';
				const int w = PgWeaponIdFromName(aName);
				if(w >= 0)
				{
					pChar->GiveWeapon(w, false);
					Given++;
				}
			}
		}
		if(Given <= 0)
		{
			Out("usage: giveweapons [hammer|gun|shotgun|grenade|laser|ninja]...");
			return true;
		}
		Out("Weapons given");
		return true;
	}
	if(str_comp_nocase(aCmd, "unweapons") == 0 || str_comp_nocase(aCmd, "removeweapons") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		CCharacterCore Core = pChar->GetCore();
		for(int w = 0; w < NUM_WEAPONS; ++w)
			Core.m_aWeapons[w].m_Ammo = 0;
		Core.m_ActiveWeapon = WEAPON_HAMMER;
		pChar->SetCore(Core);
		pChar->SetActiveWeapon(WEAPON_HAMMER);
		Out("Weapons removed");
		return true;
	}
	if(str_comp_nocase(aCmd, "weapon") == 0 || str_comp_nocase(aCmd, "setweapon") == 0)
	{
		const int w = PgWeaponIdFromName(aArg1);
		if(w < 0)
		{
			Out("usage: weapon <hammer|gun|shotgun|grenade|laser|ninja>");
			return true;
		}
		PgGiveWeapon(w);
		Out("Weapon set");
		return true;
	}
	const int SingleWeapon = PgWeaponIdFromName(aCmd);
	if(SingleWeapon >= 0)
	{
		PgGiveWeapon(SingleWeapon);
		Out("Weapon given");
		return true;
	}
	if(str_comp_nocase(aCmd, "addweapon") == 0 || str_comp_nocase(aCmd, "removeweapon") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const int Id = atoi(aArg1);
		if(Id < 0 || Id >= NUM_WEAPONS)
		{
			Out("usage: addweapon|removeweapon <0..5>");
			return true;
		}
		if(str_comp_nocase(aCmd, "addweapon") == 0)
		{
			pChar->GiveWeapon(Id, false);
			Out("Weapon added");
		}
		else
		{
			pChar->SetWeaponGot(Id, false);
			pChar->SetWeaponAmmo(Id, 0);
			if(pChar->GetActiveWeapon() == Id)
				pChar->SetActiveWeapon(WEAPON_HAMMER);
			Out("Weapon removed");
		}
		return true;
	}
	if(str_comp_nocase(aCmd, "unshotgun") == 0 || str_comp_nocase(aCmd, "ungrenade") == 0 || str_comp_nocase(aCmd, "unlaser") == 0 || str_comp_nocase(aCmd, "unrifle") == 0 || str_comp_nocase(aCmd, "unninja") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		int w = WEAPON_NINJA;
		if(str_comp_nocase(aCmd, "unshotgun") == 0)
			w = WEAPON_SHOTGUN;
		else if(str_comp_nocase(aCmd, "ungrenade") == 0)
			w = WEAPON_GRENADE;
		else if(str_comp_nocase(aCmd, "unlaser") == 0 || str_comp_nocase(aCmd, "unrifle") == 0)
			w = WEAPON_LASER;
		if(w == WEAPON_NINJA)
			pChar->RemoveNinja();
		else
		{
			pChar->SetWeaponGot(w, false);
			pChar->SetWeaponAmmo(w, 0);
		}
		Out("Weapon removed");
		return true;
	}
	if(str_comp_nocase(aCmd, "setjumps") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		CCharacterCore Core = pChar->GetCore();
		Core.m_Jumps = std::clamp(atoi(aArg1), 0, 255);
		pChar->SetCore(Core);
		Out("Jumps set");
		return true;
	}
	if(str_comp_nocase(aCmd, "solo") == 0 || str_comp_nocase(aCmd, "unsolo") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		pChar->SetSolo(str_comp_nocase(aCmd, "solo") == 0);
		Out("Solo toggled");
		return true;
	}
	if(str_comp_nocase(aCmd, "left") == 0 || str_comp_nocase(aCmd, "right") == 0 || str_comp_nocase(aCmd, "up") == 0 || str_comp_nocase(aCmd, "down") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		const int Tiles = aArg1[0] ? std::max(1, atoi(aArg1)) : 1;
		vec2 Pos = pChar->Core()->m_Pos;
		if(str_comp_nocase(aCmd, "left") == 0)
			Pos.x -= 32.0f * Tiles;
		else if(str_comp_nocase(aCmd, "right") == 0)
			Pos.x += 32.0f * Tiles;
		else if(str_comp_nocase(aCmd, "up") == 0)
			Pos.y -= 32.0f * Tiles;
		else
			Pos.y += 32.0f * Tiles;
		CCharacterCore Core = pChar->GetCore();
		Core.m_Pos = Pos;
		Core.m_Vel = vec2(0.0f, 0.0f);
		pChar->SetCore(Core);
		pChar->m_Pos = Pos;
		pChar->m_PrevPos = Pos;
		pChar->m_PrevPrevPos = Pos;
		RebaseCounters();
		Out("Moved");
		return true;
	}
	if(str_comp_nocase(aCmd, "move") == 0 || str_comp_nocase(aCmd, "move_raw") == 0 || str_comp_nocase(aCmd, "tpxy") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		if(!aArg1[0] || !aArg2[0])
		{
			Out("usage: move|move_raw|tpxy <x> <y>");
			return true;
		}
		vec2 Pos((float)atof(aArg1), (float)atof(aArg2));
		if(str_comp_nocase(aCmd, "move") == 0)
			Pos = vec2(Pos.x * 32.0f + 16.0f, Pos.y * 32.0f + 16.0f);
		CCharacterCore Core = pChar->GetCore();
		Core.m_Pos = Pos;
		Core.m_Vel = vec2(0.0f, 0.0f);
		Core.m_HookState = HOOK_IDLE;
		Core.m_HookPos = Pos;
		pChar->SetCore(Core);
		pChar->m_Pos = Pos;
		pChar->m_PrevPos = Pos;
		pChar->m_PrevPrevPos = Pos;
		RebaseCounters();
		Out("Teleported");
		return true;
	}
	if(str_comp_nocase(aCmd, "rescue") == 0 || str_comp_nocase(aCmd, "r") == 0)
	{
		CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
		if(!pChar)
		{
			Out("Playground: no tee");
			return true;
		}
		pChar->Unfreeze();
		Out("Rescued");
		return true;
	}
	if(str_comp_nocase(aCmd, "say") == 0 || str_comp_nocase(aCmd, "broadcast") == 0)
	{
		const char *pMsg = pCmdLine;
		while(*pMsg && *pMsg != ' ')
			pMsg++;
		while(*pMsg == ' ')
			pMsg++;
		PgChatAddLine(CChat::SERVER_MSG, 0, "", pMsg);
		Out("Said");
		return true;
	}
	Out("Unknown command (/help for the list)");
	return true;
}

bool CTas::PgChatHandleCommand(const char *pLine, char *pOut, int OutSize)
{
	if(pOut && OutSize > 0)
		pOut[0] = '\0';
	if(!pLine || pLine[0] != '/')
		return false;
	std::vector<std::string> vOut;
	if(!PgRunCommand(pLine + 1, vOut))
		return false;
	if(pOut && OutSize > 0)
	{
		std::string Joined;
		for(const std::string &Line : vOut)
		{
			if(!Joined.empty())
				Joined += " | ";
			Joined += Line;
		}
		str_copy(pOut, Joined.c_str(), OutSize);
	}
	return true;
}

void CTas::PgRconExecute(const char *pLine)
{
	if(!pLine || !pLine[0])
		return;
	std::vector<std::string> vOut;
	PgRunCommand(pLine, vOut);
	for(const std::string &Line : vOut)
		PgRconPrint(Line.c_str());
}

void CTas::SetPgVirtualRcon(bool On)
{
	if(m_PgVirtualRcon == On)
		return;
	m_PgVirtualRcon = On;
	if(On && m_Joined)
	{
		// One-shot replay of the recent session when the virtual rcon is
		// switched on (never per frame — that flooded the console).
		GameClient()->m_GameConsole.ClearRemoteBacklog();
		const int Start = std::max(0, (int)m_vPgRcon.size() - 200);
		for(int i = Start; i < (int)m_vPgRcon.size(); ++i)
			GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, m_vPgRcon[i].c_str());
	}
}

bool CTas::SpawnerSpawnAt(const STasSpawnerSettings &Settings, vec2 Pos, int Tick, bool Record)
{
	// "Timeline Tick" (tee only): pretend the tee already existed at that tick.
	// It is stored as the spawn tick, so playback/rewind ages from it instead
	// of from the moment it was placed.
	const int Timeline = Settings.m_EntityTimelineTick;
	if(Timeline > 0)
		Tick = Timeline;

	if(Settings.m_Mode == TAS_SPAWN_MODE_TILE)
		return SpawnerSpawnTile(Settings, Pos, Tick, Record);
	return SpawnerSpawnEntity(Settings, Pos, Tick, Record);
}

bool CTas::SpawnerSpawnEntity(const STasSpawnerSettings &S, vec2 Pos, int Tick, bool Record)
{
	SSpawnedEnt Spawn;
	Spawn.m_Entity = S.m_Entity;
	Spawn.m_Tick = Tick;
	Spawn.m_Pos = Pos;
	Spawn.m_RenderPrevPos = Pos;
	Spawn.m_RenderCurPos = Pos;
	Spawn.m_Settings = S;

	CEntity *pEnt = nullptr;
	bool Insert = true;
	// Some entities self-insert in their constructor (CProjectile, CLaser) —
	// inserting them again corrupts the world's linked lists and crashes the
	// client a few ticks later, so they are only removed from the world again
	// when physics is off.
	bool SelfInserted = false;

	switch(S.m_Entity)
	{
	case TAS_SPAWN_ENT_TEE:
	{
		// Every spawned tee needs its OWN free client slot: sharing an id makes
		// the world register both under the same m_apCharacters entry, which
		// corrupts hooking (a hook of one tee is resolved against another) and
		// player collision. Refuse to spawn when the slots are exhausted.
			const int StartId = std::clamp(S.m_Id, 0, MAX_CLIENTS - 1);
			int Id = -1;
			// Physics-off tees never enter the world, so they do not consume a
			// client slot (unlimited visual tees).
			if(!S.m_EntityPhysics)
				Id = 0;
			for(int i = 0; Id < 0 && i < MAX_CLIENTS; ++i)
		{
			const int Candidate = (StartId + i) % MAX_CLIENTS;
			if(Candidate == m_LocalId || m_Playground.GetCharacterById(Candidate))
				continue;
			// Physics-off tees are not world entities, so they are not registered
			// in the world either — check our own list as well.
			bool Used = false;
			for(const SSpawnedEnt &E : m_vSpawnedEnts)
				if(E.m_Entity == TAS_SPAWN_ENT_TEE && E.m_pEnt && static_cast<CCharacter *>(E.m_pEnt)->GetCid() == Candidate)
				{
					Used = true;
					break;
				}
			if(Used)
				continue;
			Id = Candidate;
			break;
		}
		// No free client slot: refuse. Reusing ids round-robin created
		// "phantom" tees — the second InsertEntity overwrote the id →
		// character slot, so the earlier tee stayed in the entity list
		// (hookable/hammerable) but vanished from the player-collision map.
		if(Id < 0)
			return false;

		CNetObj_Character Char;
		mem_zero(&Char, sizeof(Char));
		Char.m_X = (int)Pos.x;
		Char.m_Y = (int)Pos.y;
		Char.m_VelX = 0;
		Char.m_VelY = 0;
		Char.m_Angle = 0;
		Char.m_Weapon = std::clamp(S.m_WeaponId, 0, NUM_WEAPONS - 1);
		Char.m_HookState = HOOK_IDLE;
		Char.m_HookX = (int)Pos.x;
		Char.m_HookY = (int)Pos.y;
		Char.m_Emote = EMOTE_NORMAL;
		Char.m_AttackTick = 0;
		CNetObj_DDNetCharacter Ext;
		mem_zero(&Ext, sizeof(Ext));
		// Hook strength id: Strong keeps the slider value, Weak flips it so the
		// tee sorts as the weak end of the chain.
		const int Strength = std::clamp(S.m_HookStrength, 0, 100);
		Ext.m_StrongWeakId = S.m_HookStrengthMode == 1 ? 100 - Strength : Strength;

		CCharacter *pChar = new CCharacter(&m_Playground, Id, &Char, &Ext);
		// Weapon loadout: -1 = bare tee (no GiveWeapon call at all), otherwise
		// the tee owns exactly the selected weapon.
		const int LoadoutWeapon = S.m_WeaponId;
		if(LoadoutWeapon >= 0 && LoadoutWeapon < NUM_WEAPONS)
		{
			pChar->GiveWeapon(LoadoutWeapon, false);
			pChar->SetActiveWeapon(LoadoutWeapon);
		}
		pChar->m_Pos = Pos;
		pChar->m_PrevPos = Pos + vec2(S.m_PrevPosX, S.m_PrevPosY);
		pChar->m_PrevPrevPos = pChar->m_PrevPos;
		// The core has no previous position: a PrevPos offset is expressed as the
		// implied velocity of one tick (that is what actually flings the tee).
		if(S.m_PrevPosX != 0.0f || S.m_PrevPosY != 0.0f)
			pChar->SetVelocity(vec2(S.m_PrevPosX, S.m_PrevPosY) * (float)m_Playground.GameTickSpeed());
		pChar->m_IsLocal = false;
		pEnt = pChar;
		// Physics off: keep the tee out of the world entirely so it hangs in
		// the air with no collision and no ticking.
		Insert = S.m_EntityPhysics;
		break;
	}
	case TAS_SPAWN_ENT_PROJECTILE:
	{
		const int Type = std::clamp(S.m_WeaponId, 0, NUM_WEAPONS - 1);
		// v6.2: direction preset + extra velocity + lifespan from the panel
		// (-1 = never expires, like a frozen dummy projectile).
		// v6.3: freeze / explosive / sound toggles.
		const vec2 Dir = normalize(TasSpawnerDir(S.m_Dir) + vec2(S.m_VelX, S.m_VelY) * 0.01f);
		const int Span = S.m_LifeSpan;
		pEnt = new CProjectile(&m_Playground, Type, -1, Pos, Dir, Span, S.m_ProjectileFreeze,
			Type == WEAPON_GRENADE || S.m_ProjectileExplosive, S.m_ProjectileSound ? SOUND_GUN_FIRE : -1);
		SelfInserted = true;
		break;
	}
	case TAS_SPAWN_ENT_LASER:
	{
		const float Energy = std::clamp((float)S.m_LaserReach, 32.0f, 10000.0f);
		const int LaserType = S.m_LaserType == 1 ? LASERTYPE_SHOTGUN : LASERTYPE_RIFLE;
		pEnt = new CLaser(&m_Playground, Pos, TasSpawnerDir(S.m_Dir), Energy, -1, LaserType);
		SelfInserted = true;
		break;
	}
	case TAS_SPAWN_ENT_PICKUP:
	{
		CPickupData Data;
		Data.m_Pos = Pos;
		switch(std::clamp(S.m_PickupType, 0, 3))
		{
		case 1: Data.m_Type = POWERUP_ARMOR; break;
		case 2: Data.m_Type = POWERUP_WEAPON; break;
		case 3: Data.m_Type = POWERUP_NINJA; break;
		default: Data.m_Type = POWERUP_HEALTH; break;
		}
		Data.m_Subtype = std::clamp(S.m_Subtype, 0, 255);
		Data.m_SwitchNumber = std::clamp(S.m_SwitchNumber, 0, 255);
		Data.m_Flags = 0;
		pEnt = new CPickup(&m_Playground, -1, &Data);
		break;
	}
	case TAS_SPAWN_ENT_DOOR:
	case TAS_SPAWN_ENT_DRAGGER:
	case TAS_SPAWN_ENT_PLASMA:
	{
		// Only the door entity uses the length (From→To); draggers and plasma
		// compute their reach themselves.
		const bool UsesLength = S.m_Entity == TAS_SPAWN_ENT_DOOR;
		CLaserData Data;
		Data.m_From = Pos;
		Data.m_To = Pos + (UsesLength ? TasSpawnerDir(S.m_Dir) * (float)std::clamp(S.m_LaserLength, 16, 4096) : TasSpawnerDir(S.m_Dir) * 32.0f);
		Data.m_StartTick = Tick;
		Data.m_ExtraInfo = true;
		Data.m_Owner = -1;
		Data.m_SwitchNumber = std::clamp(S.m_SwitchNumber, 0, 255);
		Data.m_Subtype = std::clamp(S.m_Subtype, 0, 255);
		Data.m_Predict = false;
		Data.m_TuneZone = 0;
		if(S.m_Entity == TAS_SPAWN_ENT_DOOR)
		{
			Data.m_Type = LASERTYPE_DOOR;
			pEnt = new CDoor(&m_Playground, -1, &Data);
		}
		else if(S.m_Entity == TAS_SPAWN_ENT_DRAGGER)
		{
			Data.m_Type = LASERTYPE_DRAGGER;
			Data.m_Subtype = (int)std::clamp(S.m_DraggerStrength, 0, 255); // pull strength
			pEnt = new CDragger(&m_Playground, -1, &Data);
		}
		else
		{
			Data.m_Type = LASERTYPE_PLASMA;
			Data.m_Subtype = std::clamp(S.m_Subtype, 0, 255); // bit flags: 1 = explosive, 2 = freeze
			pEnt = new CPlasma(&m_Playground, -1, &Data);
		}
		Spawn.m_LaserData = Data;
		break;
	}
	default:
		break;
	}

	if(!pEnt)
		return false;

	Spawn.m_pEnt = pEnt;
	if(SelfInserted)
	{
		// Already in the world (self-inserting ctor). Physics off = pull it
		// back out so it does not tick / collide; the renderer draws it from
		// the SSpawnedEnt copy.
		if(S.m_EntityPhysics)
		{
			Spawn.m_Inserted = true;
		}
		else
		{
			m_Playground.RemoveEntity(pEnt);
			Spawn.m_Inserted = false;
		}
	}
	else
	{
		Spawn.m_Inserted = Insert;
		if(Insert)
		{
			m_Playground.InsertEntity(pEnt);
			// v6.2: a newly inserted character can take over an m_apCharacters
			// slot previously held by a round-robin duplicate; resync the id →
			// character map the same way CGameWorld::Tick does, otherwise the
			// tee is "phantom" (no hook/collision partner resolution) until the
			// next world tick.
			m_Playground.RefreshCharacterIds();
		}
	}
	m_vSpawnedEnts.push_back(Spawn);

	if(Record)
	{
		STasSpawnerRecord Rec;
		Rec.m_Tick = Tick;
		Rec.m_Mode = S.m_Mode;
		Rec.m_Entity = S.m_Entity;
		Rec.m_Tile = S.m_Tile;
		Rec.m_Physics = S.m_EntityPhysics;
		Rec.m_Pos = Pos;
		Rec.m_Settings = S;
		m_vSpawnerRecords.push_back(Rec);
	}
	return true;
}

bool CTas::SpawnerSpawnTile(const STasSpawnerSettings &S, vec2 Pos, int Tick, bool Record)
{
	CCollision *pColl = m_Playground.Collision();
	if(!pColl || !pColl->GameLayer())
		return false;

	const vec2 TilePos(floorf(Pos.x / 32.0f) * 32.0f, floorf(Pos.y / 32.0f) * 32.0f);
	const int Index = pColl->GetPureMapIndex(TilePos.x + 16.0f, TilePos.y + 16.0f);
	// Only require the index to be inside the game layer: TileExists() is about
	// freeze/tele/laser tiles, so using it here refused to place a tile on air.
	if(Index < 0 || Index >= pColl->GetWidth() * pColl->GetHeight())
		return false;

	const int TileId = std::clamp(S.m_Tile, 0, 255);
	// Layer dispatch: tele tiles live in the tele layer, the speedup tile in
	// the speedup layer, everything else in the game layer. A spawned
	// TILE_TUNE additionally rewrites the tune layer so the prediction
	// switches to the configured zone.
	const bool IsTeleTile = TileId == TILE_TELEIN || TileId == TILE_TELEINEVIL || TileId == TILE_TELEOUT || TileId == TILE_TELECHECKIN;
	const bool IsSpeedupTile = TileId == TILE_SPEED_BOOST;
	const bool TileUsesNumber = TileId == TILE_TUNE || IsTeleTile;
	const int TileNumber = std::clamp(S.m_TileNumber, 0, 255);

	// Already spawned here: retarget instead of stacking records.
	for(SSpawnedTile &T : m_vSpawnedTiles)
	{
		if(T.m_Index != Index)
			continue;
		T.m_Tile = TileId;
		T.m_Number = TileUsesNumber ? TileNumber : 0;
		T.m_Physics = S.m_TilePhysics;
		if(T.m_Physics)
		{
			if(IsTeleTile)
				pColl->SetTeleTileAt(TilePos.x + 16.0f, TilePos.y + 16.0f, TileId, T.m_Number);
			else if(IsSpeedupTile)
				pColl->SetSpeedupAt(TilePos.x + 16.0f, TilePos.y + 16.0f, S.m_SpeedupForce, S.m_SpeedupMaxSpeed, S.m_SpeedupAngle);
			else
			{
				pColl->SetCollisionAt(TilePos.x + 16.0f, TilePos.y + 16.0f, TileId);
				if(TileId == TILE_TUNE)
				{
					T.m_TuneSet = true;
					T.m_OldTuneType = 0;
					T.m_OldTuneNumber = 0;
					pColl->SetTuneZoneAt(TilePos.x + 16.0f, TilePos.y + 16.0f, T.m_Number);
				}
			}
		}
		else
			pColl->SetCollisionAt(TilePos.x + 16.0f, TilePos.y + 16.0f, T.m_OldTile);
		m_TileVisualDirty = true;
		return true;
	}

	const int OldTile = pColl->GameLayer()[Index].m_Index;
	SSpawnedTile T;
	T.m_Index = Index;
	T.m_OldTile = OldTile;
	T.m_Tile = TileId;
	T.m_Number = TileUsesNumber ? TileNumber : 0;
	T.m_Physics = S.m_TilePhysics;
	T.m_Tick = Tick;
	T.m_Pos = TilePos;
	if(S.m_TilePhysics)
	{
		if(IsTeleTile)
		{
			T.m_TeleSet = true;
			// Without a native tele layer the map renderer has nothing to
			// draw — the TAS renderer draws the tile itself.
			T.m_TeleSelfDraw = !(GameClient()->Layers() && GameClient()->Layers()->TeleLayer());
			T.m_OldTeleType = pColl->GetTeleTileTypeAt(TilePos.x + 16.0f, TilePos.y + 16.0f);
			T.m_OldTeleNumber = 0;
			pColl->SetTeleTileAt(TilePos.x + 16.0f, TilePos.y + 16.0f, TileId, T.m_Number);
		}
		else if(IsSpeedupTile)
		{
			T.m_SpeedupSet = true;
			// Without a native speedup layer the map renderer has nothing to
			// draw — the TAS renderer draws the arrow itself.
			T.m_SpeedupSelfDraw = !(GameClient()->Layers() && GameClient()->Layers()->SpeedupLayer());
			pColl->GetSpeedupTileAt(TilePos.x + 16.0f, TilePos.y + 16.0f, &T.m_OldSpeedupType, &T.m_OldSpeedupForce, &T.m_OldSpeedupMaxSpeed, &T.m_OldSpeedupAngle);
			pColl->SetSpeedupAt(TilePos.x + 16.0f, TilePos.y + 16.0f, S.m_SpeedupForce, S.m_SpeedupMaxSpeed, S.m_SpeedupAngle);
		}
		else
		{
			pColl->SetCollisionAt(TilePos.x + 16.0f, TilePos.y + 16.0f, TileId);
			// Remember the previous tune-layer tile to restore it on removal.
			if(TileId == TILE_TUNE)
			{
				T.m_TuneSet = true;
				const int OldZone = pColl->GetTuneZoneAt(TilePos.x + 16.0f, TilePos.y + 16.0f);
				if(OldZone > 0)
				{
					T.m_OldTuneType = TILE_TUNE + ENTITY_OFFSET;
					T.m_OldTuneNumber = OldZone;
				}
				pColl->SetTuneZoneAt(TilePos.x + 16.0f, TilePos.y + 16.0f, T.m_Number);
			}
		}
	}
	m_vSpawnedTiles.push_back(T);
	m_TileVisualDirty = true;

	if(Record)
	{
		STasSpawnerRecord Rec;
		Rec.m_Tick = Tick;
		Rec.m_Mode = S.m_Mode;
		Rec.m_Entity = S.m_Entity;
		Rec.m_Tile = S.m_Tile;
		Rec.m_Physics = S.m_TilePhysics;
		Rec.m_Pos = TilePos;
		Rec.m_Settings = S;
		m_vSpawnerRecords.push_back(Rec);
	}
	return true;
}

void CTas::SpawnerClearLive()
{
	for(SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(!E.m_pEnt)
			continue;
		if(E.m_Inserted)
			m_Playground.RemoveEntity(E.m_pEnt);
		delete E.m_pEnt;
	}
	m_vSpawnedEnts.clear();

	CCollision *pColl = m_Playground.Collision();
	if(pColl && pColl->GameLayer())
	{
		for(int i = (int)m_vSpawnedTiles.size() - 1; i >= 0; --i)
		{
			const SSpawnedTile &T = m_vSpawnedTiles[i];
			if(!T.m_Physics || T.m_Index < 0)
				continue;
			// Do not write into a different (possibly smaller) map's layer after
			// a map change.
			if(T.m_Index >= pColl->GetWidth() * pColl->GetHeight())
				continue;
			if(pColl->GameLayer()[T.m_Index].m_Index != T.m_Tile)
				continue;
			pColl->SetCollisionAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTile);
			if(T.m_TuneSet)
				pColl->RestoreTuneTileAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTuneType, T.m_OldTuneNumber);
			if(T.m_TeleSet)
				pColl->RestoreTeleTileAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTeleType, T.m_OldTeleNumber);
			if(T.m_SpeedupSet)
				pColl->RestoreSpeedupAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldSpeedupType, T.m_OldSpeedupForce, T.m_OldSpeedupMaxSpeed, T.m_OldSpeedupAngle);
		}
	}
		m_vSpawnedTiles.clear();
		m_TileVisualDirty = true;
		m_SpawnerLoop = false;
	m_SpawnerLoopMs = 0.0f;
	m_SpawnerRenderTick = -1;
}

void CTas::SpawnerKillAll()
{
	if(!m_Joined)
		return;

	// "Kill All" only clears the selected GROUP: the entity type chosen in the
	// panel (entity mode) or the chosen tile id (tile mode).
	if(m_Spawner.m_Mode == TAS_SPAWN_MODE_TILE)
	{
		CCollision *pColl = m_Playground.Collision();
		if(pColl)
		{
			for(const SSpawnedTile &T : m_vSpawnedTiles)
			{
				if(T.m_Tile != m_Spawner.m_Tile)
					continue;
				if(T.m_Index < 0 || T.m_Index >= pColl->GetWidth() * pColl->GetHeight())
					continue;
				if(T.m_Physics)
				{
					pColl->SetCollisionAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTile);
					if(T.m_TuneSet)
						pColl->RestoreTuneTileAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTuneType, T.m_OldTuneNumber);
					if(T.m_TeleSet)
						pColl->RestoreTeleTileAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldTeleType, T.m_OldTeleNumber);
					if(T.m_SpeedupSet)
						pColl->RestoreSpeedupAt((float)T.m_Pos.x + 16.0f, (float)T.m_Pos.y + 16.0f, T.m_OldSpeedupType, T.m_OldSpeedupForce, T.m_OldSpeedupMaxSpeed, T.m_OldSpeedupAngle);
				}
			}
		}
		m_vSpawnedTiles.erase(
			std::remove_if(m_vSpawnedTiles.begin(), m_vSpawnedTiles.end(),
				[this](const SSpawnedTile &T) { return T.m_Tile == m_Spawner.m_Tile; }),
			m_vSpawnedTiles.end());
		for(auto It = m_vSpawnerRecords.begin(); It != m_vSpawnerRecords.end();)
		{
			if(It->m_Mode == TAS_SPAWN_MODE_TILE && It->m_Tile == m_Spawner.m_Tile)
				It = m_vSpawnerRecords.erase(It);
			else
				++It;
		}
		m_TileVisualDirty = true;
	}
	else
	{
		for(SSpawnedEnt &E : m_vSpawnedEnts)
		{
			if(E.m_Entity != m_Spawner.m_Entity)
				continue;
			if(!E.m_pEnt)
				continue;
			// Respawn-style burst where the tee is removed.
			GameClient()->m_Effects.PlayerSpawn(E.m_pEnt->GetPos(), 1.0f, 1.0f);
			if(E.m_Inserted)
				m_Playground.RemoveEntity(E.m_pEnt);
			delete E.m_pEnt;
			E.m_pEnt = nullptr;
		}
		m_vSpawnedEnts.erase(
			std::remove_if(m_vSpawnedEnts.begin(), m_vSpawnedEnts.end(),
				[this](const SSpawnedEnt &E) { return E.m_Entity == m_Spawner.m_Entity; }),
			m_vSpawnedEnts.end());
		for(auto It = m_vSpawnerRecords.begin(); It != m_vSpawnerRecords.end();)
		{
			if(It->m_Mode == TAS_SPAWN_MODE_ENTITY && It->m_Entity == m_Spawner.m_Entity)
				It = m_vSpawnerRecords.erase(It);
			else
				++It;
		}
		// The previous-spawn anchor survives only if a record of the selected
		// group still exists; otherwise the anchor belongs to a dead spawn.
		m_SpawnerHasPrev = false;
		for(const STasSpawnerRecord &Rec : m_vSpawnerRecords)
		{
			if(Rec.m_Mode == TAS_SPAWN_MODE_ENTITY && Rec.m_Entity == m_Spawner.m_Entity)
			{
				m_SpawnerHasPrev = true;
				m_SpawnerPrevPos = Rec.m_Pos;
				break;
			}
		}
	}
	m_SpawnerRenderTick = -1;
}

bool CTas::SpawnerOwnsEntity(const CEntity *pEnt) const
{
	if(!pEnt)
		return false;
	for(const SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(E.m_pEnt == pEnt)
			return true;
	}
	return false;
}

void CTas::SpawnerDetachLive()
{
	// The playground world is about to be cleared (it deletes its entities), so
	// only forget the pointers and remember where each object ended up. Nothing
	// else is reset: records, settings, previous-spawn position, loop and spawned
	// tiles all stay.
	for(SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(E.m_pEnt && E.m_Inserted)
			E.m_Pos = E.m_pEnt->GetPos();
		E.m_pEnt = nullptr;
		E.m_Inserted = false;
		E.m_RenderPrevPos = E.m_RenderCurPos = E.m_Pos;
	}
	m_SpawnerRenderTick = -1;
}

void CTas::SpawnerReattachLive()
{
	// Re-create every spawned entity in the fresh playground world at the
	// position it had when the playground was left.
	for(size_t i = 0; i < m_vSpawnedEnts.size(); ++i)
	{
		const SSpawnedEnt Copy = m_vSpawnedEnts[i];
		if(Copy.m_pEnt || Copy.m_Entity != TAS_SPAWN_ENT_TEE)
			continue;
		const size_t Before = m_vSpawnedEnts.size();
		SpawnerSpawnAt(Copy.m_Settings, Copy.m_Pos, Copy.m_Tick, false);
		if(m_vSpawnedEnts.size() == Before + 1)
		{
			const SSpawnedEnt &New = m_vSpawnedEnts.back();
			SSpawnedEnt &Slot = m_vSpawnedEnts[i];
			Slot.m_pEnt = New.m_pEnt;
			Slot.m_Inserted = New.m_Inserted;
			Slot.m_Pos = New.m_Pos;
			Slot.m_RenderPrevPos = Slot.m_RenderCurPos = New.m_Pos;
			m_vSpawnedEnts.pop_back();
		}
	}
	// Tiles live in the map collision layer; re-apply them in case the layer was
	// rebuilt (map change) while the playground was closed.
	CCollision *pColl = m_Playground.Collision();
	if(pColl && pColl->GameLayer())
	{
		for(SSpawnedTile &T : m_vSpawnedTiles)
		{
			const int Index = pColl->GetPureMapIndex(T.m_Pos.x + 16.0f, T.m_Pos.y + 16.0f);
			if(Index < 0 || Index >= pColl->GetWidth() * pColl->GetHeight())
				continue;
			T.m_Index = Index;
			if(!T.m_Physics)
				continue;
			if(pColl->GameLayer()[Index].m_Index != T.m_Tile)
				T.m_OldTile = pColl->GameLayer()[Index].m_Index;
				pColl->SetCollisionAt(T.m_Pos.x + 16.0f, T.m_Pos.y + 16.0f, T.m_Tile);
			}
		}
		m_TileVisualDirty = true;
		m_SpawnerRenderTick = -1;
	}

void CTas::SpawnerPruneRemoved()
{
	if(m_vSpawnedEnts.empty())
		return;
	// Collect the entities that still exist in the playground.
	std::vector<CEntity *> vLive;
	for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; ++Type)
		for(CEntity *pEnt = m_Playground.FindFirst(Type); pEnt; pEnt = pEnt->TypeNext())
			vLive.push_back(pEnt);
	for(auto It = m_vSpawnedEnts.begin(); It != m_vSpawnedEnts.end();)
	{
		if(It->m_Inserted && std::find(vLive.begin(), vLive.end(), It->m_pEnt) == vLive.end())
			It = m_vSpawnedEnts.erase(It);
		else
			++It;
	}
}

void CTas::SpawnerApplyRecords()
{
	for(const STasSpawnerRecord &R : m_vSpawnerRecords)
		SpawnerSpawnAt(R.m_Settings, R.m_Pos, R.m_Tick, false);
}

void CTas::RenderSpawned()
{
	if(!m_Joined)
		return;
	CGameClient *pGame = GameClient();
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
	// Same culling border as CPlayers::OnRender (a tee + weapon/hook fits into a
	// 200x200 box), otherwise the spawned tees vanished while still on screen.
	CScreenRect ScreenRect = Graphics()->GetScreen();
	ScreenRect.Expand(100.0f);

	// Spawned tiles live in the map's own game layer data, so the rebuilt map
	// renderer draws them together with the normal tiles (see m_TileVisualDirty
	// in OnRender) — no manual pass is needed here.

	// Refresh the tracked render positions once per playground tick: the world
	// steps discretely, so without this pair the spawned tees jump tick to tick.
	if(m_SpawnerRenderTick != m_PlaygroundTick)
	{
		m_SpawnerRenderTick = m_PlaygroundTick;
		for(SSpawnedEnt &E : m_vSpawnedEnts)
		{
			const vec2 P = (E.m_pEnt && E.m_Inserted) ? E.m_pEnt->GetPos() : E.m_Pos;
			E.m_RenderPrevPos = E.m_RenderCurPos;
			E.m_RenderCurPos = P;
		}
	}

	// Interpolation fraction toward the next tick (0 while paused / stepping).
	const float Intra = m_Paused ? 0.0f : RenderIntraAlpha();

	// Tee render data is collected first: DDNet draws every hook before every
	// player, which is what keeps the hook chain behind all tees. Drawing
	// hook+tee per entity (the old order) put a hook on top of older tees.
	struct STeeDraw
	{
		CNetObj_Character Prev;
		CNetObj_Character Cur;
		CTeeRenderInfo Info;
		const char *pName;
		const char *pClan;
		int Id;
		int StrongWeakId;
		bool Strong;
	};
	std::vector<STeeDraw> vTeeDraw;
	vTeeDraw.reserve(m_vSpawnedEnts.size() + 1);

	// Strong/weak comparison base: the local tee's extended id from the real
	// snapshot (the playground world has no snapshot of its own).
	int LocalStrongWeakId = 0;
	if(m_LocalId >= 0 && m_LocalId < MAX_CLIENTS)
	{
		const CGameClient::CSnapState::CCharacterInfo &LocalChar = pGame->m_Snap.m_aCharacters[m_LocalId];
		if(LocalChar.m_HasExtendedData)
			LocalStrongWeakId = LocalChar.m_ExtendedData.m_StrongWeakId;
	}

	for(const SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(!E.m_pEnt)
			continue;

		if(E.m_Entity == TAS_SPAWN_ENT_TEE)
		{
			CCharacter *pChar = static_cast<CCharacter *>(E.m_pEnt);
			const CCharacterCore *pCore = pChar->Core();

			STeeDraw Draw;
			Draw.pName = E.m_Settings.m_aName;
			Draw.pClan = E.m_Settings.m_aClan;
			Draw.Id = E.m_Settings.m_Id;
			CNetObj_Character &Cur = Draw.Cur;
			CNetObj_Character &Prev = Draw.Prev;
			mem_zero(&Cur, sizeof(Cur));
			mem_zero(&Prev, sizeof(Prev));

			// Interpolated position between the last two playground ticks.
			const vec2 CurPos = E.m_Inserted ? E.m_RenderCurPos : E.m_Pos;
			const vec2 PrevPos = E.m_Inserted ? E.m_RenderPrevPos : E.m_Pos;
			Cur.m_X = (int)CurPos.x;
			Cur.m_Y = (int)CurPos.y;
			Prev.m_X = (int)PrevPos.x;
			Prev.m_Y = (int)PrevPos.y;

			if(E.m_Inserted)
			{
				Cur.m_VelX = (int)(pCore->m_Vel.x * 256.0f);
				Cur.m_VelY = (int)(pCore->m_Vel.y * 256.0f);
				Cur.m_Weapon = std::clamp(pCore->m_ActiveWeapon, 0, NUM_WEAPONS - 1);
				Cur.m_HookState = pCore->m_HookState;
				Cur.m_HookX = (int)pCore->m_HookPos.x;
				Cur.m_HookY = (int)pCore->m_HookPos.y;
				Cur.m_HookDx = (int)(pCore->m_HookDir.x * 256.0f);
				Cur.m_HookDy = (int)(pCore->m_HookDir.y * 256.0f);
				Prev.m_HookState = Cur.m_HookState;
				Prev.m_HookX = Cur.m_HookX;
				Prev.m_HookY = Cur.m_HookY;
				Prev.m_HookDx = Cur.m_HookDx;
				Prev.m_HookDy = Cur.m_HookDy;
			}
			else
			{
				Cur.m_Weapon = std::clamp(E.m_Settings.m_WeaponId, 0, NUM_WEAPONS - 1);
				Cur.m_HookState = HOOK_IDLE;
				Cur.m_HookX = (int)CurPos.x;
				Cur.m_HookY = (int)CurPos.y;
			}
			// Never claim a hooked player: RenderHook would otherwise resolve the
			// hook end through the real snapshot of that client id and the chain
			// would be drawn from a completely different tee.
			Cur.m_HookedPlayer = -1;
			Prev.m_HookedPlayer = -1;
			Cur.m_AttackTick = 0;
			Prev.m_AttackTick = 0;
			Cur.m_Emote = EMOTE_NORMAL;
			Prev.m_Emote = EMOTE_NORMAL;
			Cur.m_Jumped = 1;
			Prev.m_Jumped = 1;
			Prev.m_Weapon = Cur.m_Weapon;

			CTeeRenderInfo &Info = Draw.Info;
			Info = SpawnerTeeRenderInfo(E.m_Settings);
			const bool CustomBody = E.m_Settings.m_aColorBody[0] != 0;
			const bool CustomFeet = E.m_Settings.m_aColorFeet[0] != 0;
			if(CustomBody || CustomFeet)
			{
				Info.m_CustomColoredSkin = true;
				if(CustomBody)
					Info.m_ColorBody = TasSpawnerHexColor(E.m_Settings.m_aColorBody);
				if(CustomFeet)
					Info.m_ColorFeet = TasSpawnerHexColor(E.m_Settings.m_aColorFeet);
			}
			if(E.m_Settings.m_PracticeParticles)
				Info.m_TeeRenderFlags |= TEE_EFFECT_SPARKLE;
			// Freeze is part of the physics: without the frozen skin the spawned
			// tee looked like a normal tee while it could not move at all.
			if(pChar->m_FreezeTime > 0)
				Info.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;

			const int Strength = std::clamp(E.m_Settings.m_HookStrength, 0, 100);
			Draw.StrongWeakId = E.m_Settings.m_HookStrengthMode == 1 ? 100 - Strength : Strength;
			Draw.Strong = LocalStrongWeakId > Draw.StrongWeakId;

			vTeeDraw.push_back(Draw);
		}
		else if(E.m_Entity == TAS_SPAWN_ENT_DOOR || E.m_Entity == TAS_SPAWN_ENT_DRAGGER || E.m_Entity == TAS_SPAWN_ENT_PLASMA)
		{
			// CItems::RenderLaser(const CLaserData*) is private in 20.1.1; use the
			// positional public overload with the per-type colors.
			int ColorOut = g_Config.m_ClLaserRifleOutlineColor;
			int ColorIn = g_Config.m_ClLaserRifleInnerColor;
			if(E.m_LaserData.m_Type == LASERTYPE_DOOR)
			{
				ColorOut = g_Config.m_ClLaserDoorOutlineColor;
				ColorIn = g_Config.m_ClLaserDoorInnerColor;
			}
			else if(E.m_LaserData.m_Type == LASERTYPE_DRAGGER)
			{
				ColorOut = g_Config.m_ClLaserDraggerOutlineColor;
				ColorIn = g_Config.m_ClLaserDraggerInnerColor;
			}
			pGame->m_Items.RenderLaser(E.m_LaserData.m_From, E.m_LaserData.m_To,
				color_cast<ColorRGBA>(ColorHSLA(ColorOut).WithAlpha(1.0f)),
				color_cast<ColorRGBA>(ColorHSLA(ColorIn).WithAlpha(1.0f)),
				1.0f, (float)m_PlaygroundTick, E.m_LaserData.m_Type);
		}
	}

	// Pass 1: all hook chains (under every tee).
	for(const STeeDraw &Draw : vTeeDraw)
	{
		if(Draw.Cur.m_HookState > 0)
			pGame->m_Players.RenderHook(ScreenRect, &Draw.Prev, &Draw.Cur, &Draw.Info, -1, Intra);
	}

	// Pass 2: the tees themselves.
	for(const STeeDraw &Draw : vTeeDraw)
		pGame->m_Players.RenderPlayer(ScreenRect, &Draw.Prev, &Draw.Cur, &Draw.Info, -1, Intra);

	// Pass 3: name plates. CNamePlates is disabled while the TAS playground is
	// active, so the real name plate renderer is called directly with the
	// Spawner data — nicknames, clans, client ids, direction and the
	// strong/weak hook info all obey the "Appearance" settings.
	int Slot = 0;
	for(const STeeDraw &Draw : vTeeDraw)
	{
		const vec2 Pos = mix(vec2(Draw.Prev.m_X, Draw.Prev.m_Y), vec2(Draw.Cur.m_X, Draw.Cur.m_Y), Intra);
		pGame->m_NamePlates.RenderNamePlateGameCustom(Pos, Slot++, Draw.pName, Draw.pClan,
			Draw.Id, false, Draw.StrongWeakId, Draw.Strong,
			false, false, false, false, 1.0f);
	}
}

// v6.2: hook collision preview for the playground tee. CPlayers' renderer
// asserts a real client id, so the fake tee gets its own simple line: from the
// tee along the aim, clipped at the first collision, coloured like the real
// hook coll line.
void CTas::RenderPlaygroundHookColl()
{
	if(!m_Joined || g_Config.m_ClShowHookCollOwn <= 0)
		return;
	// Same trigger as the real local tee: while the aim key is held, or always
	// when the setting is 2. (The previous version showed it unconditionally.)
	const bool AlwaysShow = g_Config.m_ClShowHookCollOwn == 2;
	const bool Held = GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy];
	if(!AlwaysShow && !Held)
		return;
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar || !m_Playground.Collision())
		return;
	const CCharacterCore *pCore = pChar->Core();
	if(pCore->m_HookState != HOOK_IDLE)
		return;
	vec2 Aim = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	if(length(Aim) < 0.001f)
		return;
	Aim = normalize(Aim);
	const CTuningParams *pTuning = m_Playground.GetTuning(0);
	const float HookLength = pTuning ? pTuning->m_HookLength : 380.0f;
	// The tee is rendered interpolated between ticks — draw the line from the
	// same render position, otherwise it shakes against the tee while moving.
	const vec2 Pos = PlaygroundTeeRenderPos();
	const vec2 End = Pos + Aim * HookLength;
	vec2 ColPos(0.0f, 0.0f);
	vec2 NewPos(0.0f, 0.0f);
	const int Hit = m_Playground.Collision()->IntersectLine(Pos, End, &ColPos, &NewPos);
	const vec2 To = Hit ? ColPos : End;
	const int ColorVal = Hit ? g_Config.m_ClHookCollColorHookableColl : g_Config.m_ClHookCollColorNoColl;
	const ColorRGBA Color = color_cast<ColorRGBA>(ColorHSLA(ColorVal));
	Graphics()->TextureClear();
	Graphics()->LinesBegin();
	Graphics()->SetColor(Color.WithAlpha(1.0f));
	IGraphics::CLineItem Line(Pos, To);
	Graphics()->LinesDraw(&Line, 1);
	Graphics()->LinesEnd();

	// Spawned tees: the engine shows other players' hook coll when THEY hold
	// the aim key (PLAYERFLAG_AIM) — a fake tee can never do that, so the
	// playground shows it for the tee the crosshair rests on instead
	// (cl_show_hook_coll_other, 2 = always for every spawned tee).
	if(g_Config.m_ClShowHookCollOther <= 0)
		return;
	const vec2 AimPoint = SpawnerTargetPos();
	for(const SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(E.m_Entity != TAS_SPAWN_ENT_TEE || !E.m_pEnt)
			continue;
		const bool Always = g_Config.m_ClShowHookCollOther == 2;
		if(!Always && distance(E.m_pEnt->GetPos(), AimPoint) > CCharacterCore::PhysicalSize())
			continue;
		const vec2 TeePos = E.m_RenderCurPos;
		const vec2 TeeAim = Aim; // spawned tees aim along the live crosshair
		const vec2 TeeEnd = TeePos + TeeAim * HookLength;
		vec2 TeeCol(0.0f, 0.0f);
		vec2 TeeNew(0.0f, 0.0f);
		const int TeeHit = m_Playground.Collision()->IntersectLine(TeePos, TeeEnd, &TeeCol, &TeeNew);
		const vec2 TeeTo = TeeHit ? TeeCol : TeeEnd;
		const int TeeColorVal = TeeHit ? g_Config.m_ClHookCollColorHookableColl : g_Config.m_ClHookCollColorNoColl;
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(TeeColorVal)).WithAlpha(1.0f));
		IGraphics::CLineItem TeeLine(TeePos, TeeTo);
		Graphics()->LinesDraw(&TeeLine, 1);
		Graphics()->LinesEnd();
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
	// Private tuning list: CopyWorld shares the pointer with the real predicted
	// world, so a virtual-rcon "tune" would otherwise leak into the live game.
	// Seeded once — leaving and re-entering keeps every applied tune; only a
	// map change or Reset re-seeds it.
	if(!m_PgTuningInit)
	{
		for(int i = 0; i < TuneZone::NUM; ++i)
			m_aPgTuning[i] = *pGame->GetTuning(i);
		m_PgTuningInit = true;
	}
	m_Playground.SetTuningList(m_aPgTuning);
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
	// Mirror the real pause/spec state into the sandbox: entering the playground
	// while paused/spectating on the server starts in the same mode there. Only
	// the sandbox is affected — the real world keeps its own state untouched.
	{
		const CGameClient::CClientData &Real = pGame->m_aClients[LocalId];
		m_PgPaused = Real.m_Paused && !Real.m_Spec;
		m_PgSpec = Real.m_Spec;
	}
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
	// Leaving the playground only detached the spawned objects (the world was
	// deleted): bring them back exactly as they were BEFORE capturing the join
	// state, so history[0] already contains them (rewinding to the start must
	// not wipe the Spawner world).
	SpawnerReattachLive();

	m_vPlaygroundHistory.clear();
	m_vPlaygroundInputs.clear();
	m_vPlaygroundHistory.push_back(CaptureState());
	RebaseCounters();

	// Virtual chat: swap the visible chat buffer to the playground one.
	if(m_PgMaster && m_PgVirtualChat)
		GameClient()->m_Chat.TasSetVirtual(true);
}

void CTas::LeavePlayground()
{
	// Virtual chat off first: it copies the playground lines back into the
	// CTas buffer and restores the real chat lines (world state not needed).
	GameClient()->m_Chat.TasSetVirtual(false);
	// Keep everything the Spawner created: only detach it from this world, which
	// is destroyed below. A separate Reset action is the way to actually drop it.
	SpawnerDetachLive();
	m_Joined = false;
	// Playground spec/pause are sandbox-local and end when the sandbox ends —
	// the real world keeps its own state, and re-entering starts unpaused.
	m_PgPaused = false;
	m_PgSpec = false;
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

CNetObj_PlayerInput CTas::LiveInput()
{
	CNetObj_PlayerInput Input = GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
	// Fire is a counter whose low bit is the physical "held" state and whose
	// edges CountInput() turns into presses/releases. Derive it from the last
	// consumed input plus the CURRENT held bit, moving forward by at most one
	// edge per step:
	//   held    -> next odd value  (a press if the last tick was released)
	//   released-> next even value (a release if the last tick was held)
	// The old absolute rebase (Cur - Last) could be odd, which flipped the
	// held bit forever ("not holding = shooting") and made a tap between two
	// paused steps fire on the next step.
	const int LastFire = m_vPlaygroundInputs.empty() ? (Input.m_Fire & ~1) : m_vPlaygroundInputs.back().m_Fire;
	const int FakeFire = (Input.m_Fire & 1) ? (LastFire | 1) : ((LastFire + 1) & ~1);
	Input.m_Fire = FakeFire & INPUT_STATE_MASK;

	// Weapon switch counters have no held state: replay the real edge deltas.
	m_FakeNextWeapon += Input.m_NextWeapon - m_LastLiveNextWeapon;
	m_FakePrevWeapon += Input.m_PrevWeapon - m_LastLivePrevWeapon;
	m_LastLiveNextWeapon = Input.m_NextWeapon;
	m_LastLivePrevWeapon = Input.m_PrevWeapon;
	Input.m_NextWeapon = m_FakeNextWeapon & INPUT_STATE_MASK;
	Input.m_PrevWeapon = m_FakePrevWeapon & INPUT_STATE_MASK;
	return Input;
}

// Re-anchor the weapon counters after a rewind/join: the next LiveInput() must
// start from the last consumed input, so undone switches never replay. Fire
// needs no anchor — it is rebuilt from the physical held bit every step.
void CTas::RebaseCounters()
{
	const CNetObj_PlayerInput &Cur = GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
	m_LastLiveNextWeapon = Cur.m_NextWeapon;
	m_LastLivePrevWeapon = Cur.m_PrevWeapon;
	if(m_Playing || m_vPlaygroundInputs.empty())
	{
		m_FakeNextWeapon = Cur.m_NextWeapon;
		m_FakePrevWeapon = Cur.m_PrevWeapon;
		return;
	}
	const CNetObj_PlayerInput &Last = m_vPlaygroundInputs.back();
	m_FakeNextWeapon = Last.m_NextWeapon;
	m_FakePrevWeapon = Last.m_PrevWeapon;
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
		pChar->SaveFullState(&State.Char);
		State.FreezeTime = pChar->m_FreezeTime;
		State.PrevPos = pChar->m_PrevPos;
		State.PrevPrevPos = pChar->m_PrevPrevPos;
		State.ReloadTimer = pChar->GetReloadTimer();
		State.AttackTick = pChar->GetAttackTick();
		State.LatestInput = pChar->GetLatestInput();
	}
	// v6.3: Spawner-owned entities are part of the rewind too — snapshot each
	// one's exact state so Rewind moves spawned tees, projectiles and beams back.
	for(const SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(!E.m_pEnt)
			continue;
		SSpawnedEntSnapshot Snap;
		Snap.m_Entity = E.m_Entity;
		Snap.m_Tick = E.m_Tick;
		Snap.m_Pos = E.m_Pos;
		Snap.m_Inserted = E.m_Inserted;
		Snap.m_Settings = E.m_Settings;
		if(E.m_Entity == TAS_SPAWN_ENT_TEE)
		{
			CCharacter *pC = static_cast<CCharacter *>(E.m_pEnt);
			Snap.m_Id = pC->GetCid();
			Snap.Core = pC->GetCore();
			Snap.FreezeTime = pC->m_FreezeTime;
			Snap.PrevPos = pC->m_PrevPos;
			Snap.PrevPrevPos = pC->m_PrevPrevPos;
			Snap.ReloadTimer = pC->GetReloadTimer();
			Snap.AttackTick = pC->GetAttackTick();
		}
		else if(E.m_Entity == TAS_SPAWN_ENT_PROJECTILE)
			Snap.ProjData = static_cast<CProjectile *>(E.m_pEnt)->GetData();
		else if(E.m_Entity == TAS_SPAWN_ENT_LASER)
		{
			CLaser *pLaser = static_cast<CLaser *>(E.m_pEnt);
			Snap.Laser.Data = pLaser->GetData();
			Snap.Laser.Dir = pLaser->GetDir();
			Snap.Laser.PrevPos = pLaser->GetPrevPos();
			Snap.Laser.Energy = pLaser->GetEnergy();
			Snap.Laser.Bounces = pLaser->GetBounces();
			Snap.Laser.Owner = pLaser->GetOwner();
			Snap.Laser.ZeroEnergyBounce = pLaser->GetZeroEnergyBounceInLastTick();
		}
		State.vSpawnedEnts.push_back(Snap);
	}
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
	{
		if(SpawnerOwnsEntity(pEnt))
			continue;
		State.vProjectiles.push_back(static_cast<CProjectile *>(pEnt)->GetData());
	}
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
	{
		if(SpawnerOwnsEntity(pEnt))
			continue;
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
		if(SpawnerOwnsEntity(pEnt))
			continue;
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
	// Restore EVERYTHING the tick produced, not just the core: the input queues
	// are what CountInput() inspects for presses, so leaving them at the live
	// values made the tee fire/switch weapons right after a rewind.
	pChar->LoadFullState(State.Char);
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
		if(!SpawnerOwnsEntity(pEnt))
		{
			m_Playground.RemoveEntity(pEnt);
			delete pEnt;
		}
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
		if(!SpawnerOwnsEntity(pEnt))
		{
			m_Playground.RemoveEntity(pEnt);
			delete pEnt;
		}
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
		if(!SpawnerOwnsEntity(pEnt))
		{
			m_Playground.RemoveEntity(pEnt);
			delete pEnt;
		}
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

	// v6.3: Spawner-owned entities are part of the rewind — drop the live ones
	// and rebuild the exact set/state captured for this tick.
	for(SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(!E.m_pEnt)
			continue;
		if(E.m_Inserted)
			m_Playground.RemoveEntity(E.m_pEnt);
		delete E.m_pEnt;
	}
	m_vSpawnedEnts.clear();
	int SpawnerId = 0;
	for(const SSpawnedEntSnapshot &Snap : State.vSpawnedEnts)
	{
		if(Snap.m_Entity == TAS_SPAWN_ENT_PROJECTILE)
		{
			CProjectile *pProj = new CProjectile(&m_Playground, SpawnerId++, &Snap.ProjData);
			m_Playground.InsertEntity(pProj);
			SSpawnedEnt E;
			E.m_Entity = Snap.m_Entity;
			E.m_Tick = Snap.m_Tick;
			E.m_Pos = Snap.m_Pos;
			E.m_Inserted = true;
			E.m_Settings = Snap.m_Settings;
			E.m_RenderPrevPos = E.m_RenderCurPos = Snap.m_Pos;
			E.m_pEnt = pProj;
			m_vSpawnedEnts.push_back(E);
		}
		else if(Snap.m_Entity == TAS_SPAWN_ENT_LASER)
		{
			CLaser *pLaser = new CLaser(&m_Playground, SpawnerId++, &Snap.Laser.Data);
			m_Playground.InsertEntity(pLaser);
			pLaser->Restore(Snap.Laser.Data, Snap.Laser.Dir, Snap.Laser.PrevPos, Snap.Laser.Energy, Snap.Laser.Bounces, Snap.Laser.Owner, Snap.Laser.ZeroEnergyBounce);
			SSpawnedEnt E;
			E.m_Entity = Snap.m_Entity;
			E.m_Tick = Snap.m_Tick;
			E.m_Pos = Snap.m_Pos;
			E.m_Inserted = true;
			E.m_Settings = Snap.m_Settings;
			E.m_RenderPrevPos = E.m_RenderCurPos = Snap.m_Pos;
			E.m_pEnt = pLaser;
			m_vSpawnedEnts.push_back(E);
		}
		else
		{
			const size_t Before = m_vSpawnedEnts.size();
			SpawnerSpawnAt(Snap.m_Settings, Snap.m_Pos, Snap.m_Tick, false);
			if(m_vSpawnedEnts.size() != Before + 1)
				continue;
			SSpawnedEnt &E = m_vSpawnedEnts.back();
			E.m_Inserted = Snap.m_Inserted;
			E.m_RenderPrevPos = E.m_RenderCurPos = Snap.m_Pos;
			if(Snap.m_Entity == TAS_SPAWN_ENT_TEE && E.m_pEnt)
			{
				CCharacter *pC = static_cast<CCharacter *>(E.m_pEnt);
				pC->SetCore(Snap.Core);
				pC->SetCoreWorld(&m_Playground);
				pC->m_Pos = Snap.Core.m_Pos;
				pC->m_FreezeTime = Snap.FreezeTime;
				pC->m_PrevPos = Snap.PrevPos;
				pC->m_PrevPrevPos = Snap.PrevPrevPos;
				pC->SetReloadTimer(Snap.ReloadTimer);
				pC->SetAttackTick(Snap.AttackTick);
			}
		}
	}
	m_SpawnerRenderTick = -1;
	for(size_t i = 0; i < m_Playground.m_PredictedEvents.size();)
	{
		if(m_Playground.m_PredictedEvents[i].m_Tick > State.Tick)
			m_Playground.m_PredictedEvents.erase(m_Playground.m_PredictedEvents.begin() + i);
		else
			i++;
	}

	// Safety net: the rebuild above may still have invalidated a spawner pointer
	// (e.g. a character recreated by a death/teleport), so resync the list.
	SpawnerPruneRemoved();
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
	// /pause and /spec: the player is paused, not the game — the tee stands
	// still while the playground keeps ticking (engine ConTogglePause).
	CNetObj_PlayerInput Effective = Input;
	if(m_PgPaused || m_PgSpec)
	{
		mem_zero(&Effective, sizeof(Effective));
		Effective.m_TargetX = Input.m_TargetX;
		Effective.m_TargetY = Input.m_TargetY;
		Effective.m_WantedWeapon = Input.m_WantedWeapon;
	}
	m_LastInput = Effective;
	// v6.2: /invincible and /super — snapshot the tick so death tiles cannot
	// remove the tee; it is resurrected at the same position right after.
	CTasState PgPreTick;
	if(m_PgInvincible || m_PgSuper)
		PgPreTick = CaptureState();
	// Same tick recipe as the kinodynamic simulator:
	// OnDirectInput -> m_GameTick -> OnPredictedInput -> Tick().
	pChar->OnDirectInput(&Effective);
	m_PlaygroundTick++;
	m_Playground.m_GameTick = m_PlaygroundTick;
	pChar->OnPredictedInput(&Effective);
	m_Playground.Tick();
	// Entities destroyed by the world (death tiles, expired projectiles) are
	// already deleted there — forget the dangling spawner pointers.
	SpawnerPruneRemoved();
	// v6.2: /invincible — a death tile destroyed the tee during the tick;
	// rebuild it from the snapshot taken just before and keep the tick moving.
	if((m_PgInvincible || m_PgSuper) && !m_Playground.GetCharacterById(m_LocalId))
	{
		const int TickAfter = PgPreTick.Tick + 1;
		RestoreState(PgPreTick);
		m_PlaygroundTick = TickAfter;
		m_Playground.m_GameTick = TickAfter;
	}
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
	// Spawn on the position the renderer is showing right now: the render
	// interpolates one tick behind the simulation, so spawning at the raw sim
	// tick position made the smoke lead the projectile by up to half a tile.
	const float T = (float)(PlaygroundTick - 1 - Data.m_StartTick) / TickSpeed;
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
		"\"Version\": \"20\",\n"
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
	// v2.0: Spawner records follow the tick data.
	const int SpawnCount = (int)m_vSpawnerRecords.size();
	const int SpawnSize = (int)sizeof(STasSpawnerRecord);
	// v6.2: playground chat buffer follows the spawner records. It is an
	// optional trailing block (older files simply end after the spawn count),
	// so the .tas version stays unchanged.
	const int ChatCount = (int)m_vPgChat.size();
	const int ChatSize = (int)sizeof(SPgChatLine);
	const int BinSize = 16 + (int)vTake.size() * TickSize + 4 + SpawnCount * SpawnSize +
		4 + ChatCount * ChatSize;
	const int UncompSize = 4 + TextLen + BinSize;

	std::vector<unsigned char> vUncomp(UncompSize);
	unsigned char *pOut = vUncomp.data();
	mem_copy(pOut, &BinSize, sizeof(int));
	pOut += 4;
	mem_copy(pOut, aText, TextLen);
	pOut += TextLen;
	const int Version = 2;
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

	// v2.0 Spawner records (fixed-size POD snapshot per spawn action).
	mem_copy(pOut, &SpawnCount, sizeof(int));
	pOut += 4;
	for(const STasSpawnerRecord &Rec : m_vSpawnerRecords)
	{
		mem_copy(pOut, &Rec, SpawnSize);
		pOut += SpawnSize;
	}

	// v6.2: virtual playground chat (optional trailing block).
	mem_copy(pOut, &ChatCount, sizeof(int));
	pOut += 4;
	for(const SPgChatLine &Line : m_vPgChat)
	{
		mem_copy(pOut, &Line, ChatSize);
		pOut += ChatSize;
	}

	uLongf CompLen = compressBound((uLong)UncompSize);
	std::vector<unsigned char> vComp(CompLen);
	if(compress2(vComp.data(), &CompLen, vUncomp.data(), (uLong)UncompSize, Z_BEST_COMPRESSION) != Z_OK)
		return false;

	const char aMagic[4] = {'K', 'X', 'T', '2'};
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
		if(RawSize < 16 || pData[0] != 'K' || pData[1] != 'X' || pData[2] != 'T' || (pData[3] != '1' && pData[3] != '2'))
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
		if(Version < 1 || Version > 2 || TickCount < 0 || TickCount > (BinSize - 16) / TickSize)
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

			// v2.0: Spawner records.
			std::vector<STasSpawnerRecord> vNewRecords;
			// v6.2: optional trailing playground chat block.
			std::vector<SPgChatLine> vNewChat;
			if(Version >= 2)
			{
				const unsigned char *pSpawn = pBin + 16 + TickCount * TickSize;
				const int SpawnSize = (int)sizeof(STasSpawnerRecord);
				if((int)(pSpawn - pBin) + 4 > BinSize)
					break;
				int SpawnCount = 0;
				mem_copy(&SpawnCount, pSpawn, sizeof(int));
				pSpawn += 4;
				if(SpawnCount < 0 || SpawnCount > 100000 ||
					(int)(pSpawn - pBin) + SpawnCount * SpawnSize > BinSize)
					break;
				vNewRecords.resize(SpawnCount);
				if(SpawnCount > 0)
					mem_copy(vNewRecords.data(), pSpawn, (size_t)SpawnCount * SpawnSize);
				pSpawn += (size_t)SpawnCount * SpawnSize;

				// Files written before v6.2 simply end after the spawn records;
				// the chat block is only read when there are bytes left for it.
				const int ChatSize = (int)sizeof(SPgChatLine);
				if((int)(pSpawn - pBin) + 4 <= BinSize)
				{
					int ChatCount = 0;
					mem_copy(&ChatCount, pSpawn, sizeof(int));
					pSpawn += 4;
					if(ChatCount < 0 || ChatCount > 100000 ||
						(int)(pSpawn - pBin) + ChatCount * ChatSize > BinSize)
						break;
					vNewChat.resize(ChatCount);
					if(ChatCount > 0)
						mem_copy(vNewChat.data(), pSpawn, (size_t)ChatCount * ChatSize);
				}
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

			m_vSpawnerRecords = std::move(vNewRecords);
			m_vPgChat = std::move(vNewChat);
			if(m_Joined)
		{
			SpawnerClearLive();
			SpawnerApplyRecords();
			m_SpawnerHasPrev = !m_vSpawnerRecords.empty();
			if(m_SpawnerHasPrev)
				m_SpawnerPrevPos = m_vSpawnerRecords.back().m_Pos;
		}
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
