// (c) Kinetix. TAS component implementation — see tas.h for the design.

#include <game/client/components/kinetix/tas.h>
#include <game/client/components/kinetix/kinetix_internal.h>

#include <base/dbg.h>
#include <base/io.h>
#include <base/mem.h>

#include <engine/client.h>
#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>
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
#include <array>
#include <climits>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
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

void CTasState::CompressHistoryPayload()
{
	vProjectiles.Compress();
	vLasers.Compress();
	vPickups.Compress();
	vCharacterOrder.Compress();
}

size_t CTasState::PayloadMemoryBytes() const
{
	return vProjectiles.MemoryBytes() + vLasers.MemoryBytes() + vPickups.MemoryBytes() +
		vCharacterOrder.MemoryBytes() + vSpawnedEnts.capacity() * sizeof(SSpawnedEntSnapshot) +
		vDummies.capacity() * sizeof(STasDummyState);
}

size_t CTasState::PayloadUncompressedBytes() const
{
	return vProjectiles.UncompressedBytes() + vLasers.UncompressedBytes() + vPickups.UncompressedBytes() +
		vCharacterOrder.UncompressedBytes() + vSpawnedEnts.size() * sizeof(SSpawnedEntSnapshot) +
		vDummies.size() * sizeof(STasDummyState);
}

void CTas::OnReset()
{
	LeavePlayground();
	std::vector<CTasTick>().swap(m_vRecord);
	std::vector<CTasTick>().swap(m_vSaved);
	m_UsedFlags = 0;
	mem_zero(m_aFinishTicks, sizeof(m_aFinishTicks));
	m_aRecordCreated[0] = '\0';
	m_Tps = 50.0f;
	m_Paused = false;
	m_Mode = 0;
	CancelPreparation();
	m_RealPlaying = false;
	m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
	m_RecordStart = CTasState{};
	m_SavedStart = CTasState{};
	m_HasTakePgStart = false;
	m_vTakeSpawner.clear();
	m_vTakeSpawnerEvents.clear();
	m_vTakeTimelineEvents.clear();
	m_TasTimelineNextOrder = 0;
	m_vTakeChat.clear();
	m_vTakeRcon.clear();
	m_PgAutoFireCounter = 0;
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

int CTas::TakeDummyCount() const
{
	bool aSlots[MAX_DUMMIES] = {};
	aSlots[0] = true; // Main is always included in the displayed count.
	const std::vector<CTasTick> &vTake = m_Recording ? m_vRecord : m_vSaved;
	for(const CTasTick &Tick : vTake)
		for(const STasDummyFrame &Frame : Tick.vDummyInputs)
			if(Frame.m_Slot > 0 && Frame.m_Slot < MAX_DUMMIES)
				aSlots[Frame.m_Slot] = true;
	if(m_TakeFromFile)
		for(const STasStartLoadout &Start : m_vFileStartLoadouts)
			if(Start.m_Slot > 0 && Start.m_Slot < MAX_DUMMIES)
				aSlots[Start.m_Slot] = true;
	int Count = 0;
	for(bool Slot : aSlots)
		Count += Slot ? 1 : 0;
	return Count;
}

int CTas::TakeFinishTicks() const
{
	if(m_TakeFromFile)
		return m_PickedFinishTicks;
	int Best = INT_MAX;
	for(int Ticks : m_aFinishTicks)
		if(Ticks > 0)
			Best = std::min(Best, Ticks);
	return Best == INT_MAX ? -1 : Best;
}

size_t CTas::PlaygroundMemoryBytes(bool Uncompressed) const
{
	size_t Bytes = m_vPlaygroundHistory.capacity() * sizeof(CTasState) +
		m_vPlaygroundInputs.capacity() * sizeof(CNetObj_PlayerInput);
	for(const CTasState &State : m_vPlaygroundHistory)
		Bytes += Uncompressed ? State.PayloadUncompressedBytes() : State.PayloadMemoryBytes();
	return Bytes;
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
	// Real-world preparation runs OUTSIDE the playground and must be ticked
	// before the playground-only early return. While preparing, TAS does not
	// own the inputs yet, so the botnet dummy loop can walk the dummies.
	UpdatePreparation();
	if(!m_RealPreparing && m_RealPlaying && !m_vRealPrepTargets.empty())
		RefreshPreparationWarning();
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
			GameClient()->m_MapLayersBackground.RefreshLayers();
			GameClient()->m_MapLayersForeground.RefreshLayers();
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
	const auto &MainInput = pChar->GetLatestInput();
	vec2 AimPoint(MainInput.m_TargetX, MainInput.m_TargetY);
	if(!FakeAimOffset(&AimPoint) && m_PgActiveDummy == 0 && !m_Playing)
		AimPoint = pGame->m_Controls.m_aMousePos[0];
	CNetObj_Character Prev;
	BuildPlaygroundRenderState(pChar, pPrevCore, AimPoint, Prev, Cur);

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

}

void CTas::RenderPlaygroundNamePlates()
{
	CGameClient *pGame = GameClient();
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
	auto *pActive = PlaygroundCharacter(m_PgActiveDummy);
	int aRanks[MAX_CLIENTS]{};
	int Rank = 0;
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_CHARACTER); pEnt; pEnt = pEnt->TypeNext())
		if(in_range(pEnt->GetId(), MAX_CLIENTS - 1))
			aRanks[pEnt->GetId()] = Rank++;
	const int ActiveRank = pActive ? aRanks[pActive->GetCid()] : 0;
	for(int D = 0; D < m_PgDummyLimit; ++D)
	{
		CCharacter *pTee = PlaygroundCharacter(D);
		if(!pTee)
			continue;
		const bool Own = D == m_PgActiveDummy;
		const char *pName = D == 0 ? pGame->m_aClients[m_LocalId].m_aName : pGame->m_BotNet.m_Dummy.ProfileName(D);
		const char *pClan = D == 0 ? pGame->m_aClients[m_LocalId].m_aClan : pGame->m_BotNet.m_Dummy.ProfileClan(D);
		const auto &Input = pTee->GetLatestInput();
		const int TeeRank = aRanks[pTee->GetCid()];
		const int Show = g_Config.m_ClShowDirection;
		const bool ShowDirection = Show == 2 || (Show == 1 && !Own) || (Show == 3 && Own);
		pGame->m_NamePlates.RenderNamePlateGameCustom(PlaygroundSlotRenderPos(D), D, pName, pClan,
			D, Own, TeeRank, ActiveRank > TeeRank, ShowDirection,
			Input.m_Direction < 0, Input.m_Jump != 0, Input.m_Direction > 0, 1.0f);
	}
	int Slot = MAX_DUMMIES;
	for(const SSpawnedEnt &E : m_vSpawnedEnts)
	{
		if(E.m_Entity != TAS_SPAWN_ENT_TEE || !E.m_pEnt)
			continue;
		const vec2 Pos = E.m_Inserted ? mix(E.m_RenderPrevPos, E.m_RenderCurPos, m_Paused ? 1.0f : RenderIntraAlpha()) : E.m_Pos;
		const int Id = E.m_pEnt->GetId();
		const int TeeRank = in_range(Id, MAX_CLIENTS - 1) ? aRanks[Id] : 0;
		pGame->m_NamePlates.RenderNamePlateGameCustom(Pos, Slot++, E.m_Settings.m_aName, E.m_Settings.m_aClan,
			E.m_Settings.m_Id, false, TeeRank, ActiveRank > TeeRank, false, false, false, false, 1.0f);
	}
}

// Spawned tele/speedup tiles for maps without those layers. Called from
// CKinetixLines::OnRender — i.e. AFTER m_MapLayersForeground — so the tiles
// stack like the native entity layers: ABOVE players and items.
void CTas::RenderSpawnedTilesOverlay()
{
	if(!m_Joined)
		return;
	CGameClient *pGame = GameClient();

	// Speedup arrows: exactly what CRenderMap::RenderSpeedupOverlay draws for
	// native tiles — the speedup arrow sprite from the game data, rotated.
	for(const SSpawnedTile &T : m_vSpawnedTiles)
	{
		if(!T.m_SpeedupSet || !T.m_SpeedupSelfDraw)
			continue;
		int Type = 0, Force = 0, MaxSpeed = 0, Angle = 0;
		m_Playground.Collision()->GetSpeedupTileAt(T.m_Pos.x + 16.0f, T.m_Pos.y + 16.0f, &Type, &Force, &MaxSpeed, &Angle);
		if(!Force && !MaxSpeed)
			continue;
		Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
		Graphics()->TextureClear();
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_SPEEDUP_ARROW].m_Id);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		Graphics()->SelectSprite(SPRITE_SPEEDUP_ARROW);
		Graphics()->QuadsSetRotation(Angle * (pi / 180.0f));
		Graphics()->DrawSprite(T.m_Pos.x + 16.0f, T.m_Pos.y + 16.0f, 35.0f);
		Graphics()->QuadsEnd();
	}

	// Tele tiles: draw with the NATIVE entity texture using the exact
	// CRenderMap::RenderTelemap atlas math (same sprites, same top-left
	// anchored quads → same alignment).
	{
		const bool TexArrays = Graphics()->HasTextureArraysSupport();
		const CScreenRect ScreenRect = Graphics()->GetScreen();
		const float TilePixelSize = 1024 / 32.0f;
		const float FinalTileSize = 32.0f / ScreenRect.Width() * Graphics()->ScreenWidth();
		const float FinalTilesetScale = FinalTileSize / TilePixelSize;
		const float TexSize = 1024.0f;
		const float Frac = (1.25f / TexSize) * (1 / FinalTilesetScale);
		const float Nudge = (0.5f / TexSize) * (1 / FinalTilesetScale);
		Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
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
	if(TickCount == 0 && m_vRealPrepPaths.empty())
		return;

	CGameClient *pGame = GameClient();
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);
	Graphics()->TextureClear();
	CScreenRect Visible = Graphics()->GetScreen();
	Visible.Expand(64.0f);
	const auto SegmentVisible = [&Visible](const vec2 &From, const vec2 &To) {
		return !(std::max(From.x, To.x) < Visible.m_TopLeft.x ||
			std::min(From.x, To.x) > Visible.m_BottomRight.x ||
			std::max(From.y, To.y) < Visible.m_TopLeft.y ||
			std::min(From.y, To.y) > Visible.m_BottomRight.y);
	};

	const int LineSize = KxLineSize(KX_LINE_TAS);
	const float HalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
	const float Alpha = KxLineAlpha(KX_LINE_TAS);

	// v6.18: while real dummies are being staged, draw the exact BFS routes
	// with the same TAS Visuals toggles used for the saved replay.
	if(!m_vRealPrepPaths.empty() && (g_Config.m_KxTasShowPath || g_Config.m_KxTasShowHooks))
	{
		Graphics()->LinesBegin();
		for(const std::vector<vec2> &Path : m_vRealPrepPaths)
			for(size_t i = 1; i < Path.size(); ++i)
			{
				if(!g_Config.m_KxTasShowPath)
					break;
				const ColorRGBA Col = ColorRGBA(KxLineColorAt(KX_LINE_TAS, (int)i), true).WithAlpha(Alpha);
				Graphics()->SetColor(Col.r, Col.g, Col.b, Col.a);
				const IGraphics::CLineItem Line(Path[i - 1], Path[i]);
				Graphics()->LinesDraw(&Line, 1);
			}
		Graphics()->LinesEnd();
		if(g_Config.m_KxTasShowHooks)
		{
			Graphics()->LinesBegin();
			for(const STasPrepDummy &Target : m_vRealPrepTargets)
				for(const std::pair<vec2, vec2> &Hook : Target.m_Hooks)
				{
					const ColorRGBA Col = ColorRGBA(KxLineColorAt(KX_LINE_TAS, 0), true).WithAlpha(Alpha * 0.6f);
					Graphics()->SetColor(Col.r, Col.g, Col.b, Col.a);
					const IGraphics::CLineItem Line(Hook.first, Hook.second);
					Graphics()->LinesDraw(&Line, 1);
				}
			Graphics()->LinesEnd();
		}
	}

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
				if(!SegmentVisible(p0, p1))
					continue;
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
				if(!SegmentVisible(vTicks[i - 1].Core.m_Pos, vTicks[i].Core.m_Pos))
					continue;
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

	if(g_Config.m_KxTasShowPath)
	{
		vec2 aPrev[MAX_DUMMIES]{};
		int aLastTick[MAX_DUMMIES];
		std::fill(std::begin(aLastTick), std::end(aLastTick), -2);
		if(LineSize > 0)
			Graphics()->QuadsBegin();
		else
			Graphics()->LinesBegin();
		for(int i = 0; i < TickCount; ++i)
		{
			for(const STasDummyFrame &Frame : vTicks[i].vDummyInputs)
			{
				const int Slot = Frame.m_Slot;
				const vec2 From = aPrev[Slot];
				const vec2 Delta = Frame.m_Pos - From;
				const float Speed = length(Delta);
				if(aLastTick[Slot] == i - 1 && Speed > 0.001f && SegmentVisible(From, Frame.m_Pos))
				{
					const ColorRGBA Color = g_Config.m_KxTasShowSpeed ? fnSpeedColor(Speed) :
						ColorRGBA(KxLineColorAt(KX_LINE_TAS, i - 1), true);
					Graphics()->SetColor(Color.WithAlpha(Alpha));
					if(LineSize > 0)
					{
						const vec2 Perp = vec2(Delta.y, -Delta.x) * (HalfWidth / Speed);
						IGraphics::CFreeformItem Quad(
							From.x - Perp.x, From.y - Perp.y, From.x + Perp.x, From.y + Perp.y,
							Frame.m_Pos.x - Perp.x, Frame.m_Pos.y - Perp.y, Frame.m_Pos.x + Perp.x, Frame.m_Pos.y + Perp.y);
						Graphics()->QuadsDrawFreeform(&Quad, 1);
					}
					else
					{
						IGraphics::CLineItem Line(From, Frame.m_Pos);
						Graphics()->LinesDraw(&Line, 1);
					}
				}
				aPrev[Slot] = Frame.m_Pos;
				aLastTick[Slot] = i;
			}
		}
		if(LineSize > 0)
			Graphics()->QuadsEnd();
		else
			Graphics()->LinesEnd();
	}

	if(g_Config.m_KxTasShowHooks)
	{
		const float HookHalfWidth = 0.5f + (float)(LineSize - 1) * 0.25f;
		const auto HookAt = [&](int Tick, int Slot) -> const STasDummyHookFrame * {
			for(const STasDummyHookFrame &Hook : vTicks[Tick].vDummyHooks)
				if(Hook.m_Slot == Slot)
					return &Hook;
			return nullptr;
		};
		const auto DummyPosAt = [&](int Tick, int Slot) -> const vec2 * {
			for(const STasDummyFrame &Frame : vTicks[Tick].vDummyInputs)
				if(Frame.m_Slot == Slot)
					return &Frame.m_Pos;
			return nullptr;
		};
		if(LineSize > 0)
		{
			std::vector<IGraphics::CFreeformItem> vQuads;
			for(int i = 1; i < TickCount; i++)
			{
				if(vTicks[i].Core.m_HookState != HOOK_GRABBED || vTicks[i - 1].Core.m_HookState == HOOK_GRABBED)
					continue;
				if(!SegmentVisible(vTicks[i].Core.m_Pos, vTicks[i].Core.m_HookPos))
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
			for(int i = 1; i < TickCount; ++i)
				for(const STasDummyHookFrame &Hook : vTicks[i].vDummyHooks)
				{
					const STasDummyHookFrame *pPrevHook = HookAt(i - 1, Hook.m_Slot);
					const vec2 *pPos = DummyPosAt(i, Hook.m_Slot);
					if(Hook.m_HookState != HOOK_GRABBED || !pPrevHook || pPrevHook->m_HookState == HOOK_GRABBED || !pPos)
						continue;
					if(!SegmentVisible(*pPos, Hook.m_HookPos))
						continue;
					const vec2 Delta = Hook.m_HookPos - *pPos;
					const float Len = length(Delta);
					if(Len < 0.001f)
						continue;
					const vec2 Perp = vec2(Delta.y, -Delta.x) * (HookHalfWidth / Len);
					vQuads.emplace_back(
						pPos->x - Perp.x, pPos->y - Perp.y, pPos->x + Perp.x, pPos->y + Perp.y,
						Hook.m_HookPos.x - Perp.x, Hook.m_HookPos.y - Perp.y,
						Hook.m_HookPos.x + Perp.x, Hook.m_HookPos.y + Perp.y);
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
				if(!SegmentVisible(vTicks[i].Core.m_Pos, vTicks[i].Core.m_HookPos))
					continue;
				IGraphics::CLineItem Line(vTicks[i].Core.m_Pos, vTicks[i].Core.m_HookPos);
				Graphics()->LinesDraw(&Line, 1);
			}
			for(int i = 1; i < TickCount; ++i)
				for(const STasDummyHookFrame &Hook : vTicks[i].vDummyHooks)
				{
					const STasDummyHookFrame *pPrevHook = HookAt(i - 1, Hook.m_Slot);
					const vec2 *pPos = DummyPosAt(i, Hook.m_Slot);
					if(Hook.m_HookState != HOOK_GRABBED || !pPrevHook || pPrevHook->m_HookState == HOOK_GRABBED || !pPos)
						continue;
					if(!SegmentVisible(*pPos, Hook.m_HookPos))
						continue;
					IGraphics::CLineItem Line(*pPos, Hook.m_HookPos);
					Graphics()->LinesDraw(&Line, 1);
				}
			Graphics()->LinesEnd();
		}
	}

	if(g_Config.m_KxTasDrawStartEndPos)
	{
		const vec2 aPos[] = {vTicks.front().Core.m_Pos, vTicks.back().Core.m_Pos};
		const ColorRGBA aColor[] = {ColorRGBA(0.2f, 1.0f, 0.2f, 1.0f), ColorRGBA(1.0f, 0.2f, 0.2f, 1.0f)};
		Graphics()->LinesBegin();
		for(int i = 0; i < 2; i++)
		{
			if(aPos[i] == vec2(0, 0))
				continue;
			Graphics()->SetColor(aColor[i]);
			IGraphics::CLineItem aBox[] = {
				IGraphics::CLineItem(aPos[i] + vec2(-16, -16), aPos[i] + vec2(16, -16)),
				IGraphics::CLineItem(aPos[i] + vec2(16, -16), aPos[i] + vec2(16, 16)),
				IGraphics::CLineItem(aPos[i] + vec2(16, 16), aPos[i] + vec2(-16, 16)),
				IGraphics::CLineItem(aPos[i] + vec2(-16, 16), aPos[i] + vec2(-16, -16)),
			};
			Graphics()->LinesDraw(aBox, 4);
		}
		Graphics()->LinesEnd();
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
	{TILE_EHOOK_ENABLE, "EndlessHookEnable"},
	{TILE_EHOOK_DISABLE, "EndlessHookDisable"},
	{TILE_HIT_ENABLE, "HitEnable"},
	{TILE_HIT_DISABLE, "HitDisable"},
	{TILE_SOLO_ENABLE, "SoloEnable"},
	{TILE_SOLO_DISABLE, "SoloDisable"},
	{TILE_NPC_DISABLE, "PlayerCollisionDisable"},
	{TILE_NPC_ENABLE, "PlayerCollisionEnable"},
	{TILE_UNLIMITED_JUMPS_DISABLE, "EndlessJumpDisable"},
	{TILE_UNLIMITED_JUMPS_ENABLE, "EndlessJumpEnable"},
	{TILE_JETPACK_DISABLE, "JetpackDisable"},
	{TILE_JETPACK_ENABLE, "JetpackEnable"},
	{TILE_NPH_DISABLE, "PlayerHookDisable"},
	{TILE_NPH_ENABLE, "PlayerHookEnable"},
	{TILE_TELE_GUN_ENABLE, "TeleGunEnable"},
	{TILE_TELE_GUN_DISABLE, "TeleGunDisable"},
	{TILE_ALLOW_TELE_GUN, "AllowTeleGun"},
	{TILE_ALLOW_BLUE_TELE_GUN, "AllowBlueTeleGun"},
	{TILE_TELE_GRENADE_ENABLE, "TeleGrenadeEnable"},
	{TILE_TELE_GRENADE_DISABLE, "TeleGrenadeDisable"},
	{TILE_TELE_LASER_ENABLE, "TeleLaserEnable"},
	{TILE_TELE_LASER_DISABLE, "TeleLaserDisable"},
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

CTeeRenderInfo CTas::PgDummyTeeRenderInfo(int D)
{
	CGameClient *pGame = GameClient();
	const SDummyProfile &P = pGame->m_BotNet.m_Dummy.m_aProfiles[D];
	STasSpawnerSettings S;
	str_copy(S.m_aName, pGame->m_BotNet.m_Dummy.ProfileName(D), sizeof(S.m_aName));
	str_copy(S.m_aClan, pGame->m_BotNet.m_Dummy.ProfileClan(D), sizeof(S.m_aClan));
	str_copy(S.m_aSkin, pGame->m_BotNet.m_Dummy.ProfileSkin(D), sizeof(S.m_aSkin));
	S.m_Id = D;

	// Use the effective custom-color flags; format the profile color as Spawner
	// does. Missing profile values inherit the shared dummy_color_* cvars.
	const int UseCustom = pGame->m_BotNet.m_Dummy.ProfileUseCustomColor(D);
	if(UseCustom == 1 || (UseCustom == 0 && P.m_UseCustomColor == 1))
	{
		char aBody[8], aFeet[8];
		if(P.m_ColorBody != 0xFFFFFFFF)
			str_format(aBody, sizeof(aBody), "#%06x", P.m_ColorBody & 0xFFFFFF);
		else
			str_format(aBody, sizeof(aBody), "#%06x", g_Config.m_ClDummyColorBody & 0xFFFFFF);
		if(P.m_ColorFeet != 0xFFFFFFFF)
			str_format(aFeet, sizeof(aFeet), "#%06x", P.m_ColorFeet & 0xFFFFFF);
		else
			str_format(aFeet, sizeof(aFeet), "#%06x", g_Config.m_ClDummyColorFeet & 0xFFFFFF);
		str_copy(S.m_aColorBody, aBody, sizeof(S.m_aColorBody));
		str_copy(S.m_aColorFeet, aFeet, sizeof(S.m_aColorFeet));
	}
	return SpawnerTeeRenderInfo(S);
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
	if(m_Recording)
	{
		STasSpawnerEvent Event;
		Event.m_Type = 1;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_Record.m_Tick = m_PlaygroundTick;
		Event.m_Record.m_Mode = m_Spawner.m_Mode;
		Event.m_Record.m_Entity = m_Spawner.m_Entity;
		Event.m_Record.m_Tile = m_Spawner.m_Tile;
		Event.m_Record.m_Pos = Pos;
		Event.m_Record.m_Settings = m_Spawner;
		m_vTakeSpawnerEvents.push_back(Event);
	}
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
	if(Removed && m_Recording)
		m_UsedFlags |= TAS_USED_SPAWNER;
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
	CancelPreparation();
	m_RealPlaying = false;
	m_Desynced = false;
	m_PlayIdx = 0;
	m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
	m_TakeFromFile = false;
	std::vector<CTasTick>().swap(m_vRecord);
	std::vector<CTasTick>().swap(m_vSaved);
	m_RecordStart = CTasState{};
	m_SavedStart = CTasState{};
	m_HasTakePgStart = false;
	m_vTakeSpawner.clear();
	m_vTakeSpawnerEvents.clear();
	m_vTakeTimelineEvents.clear();
	m_TasTimelineNextOrder = 0;
	m_vTakeChat.clear();
	m_vTakeRcon.clear();
	m_UsedFlags = 0;
	mem_zero(m_aFinishTicks, sizeof(m_aFinishTicks));
	m_aRecordCreated[0] = '\0';
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
	if(m_PgPractice == On)
		return;
	m_PgPractice = On;
	if(m_Recording)
		m_UsedFlags |= TAS_USED_PRACTICE;
	if(m_Recording && !m_ExecutingPgCommand && !m_ApplyingTakeTimelineEvent)
	{
		STasTimelineEvent Event;
		Event.m_Type = TAS_TIMELINE_EVENT_PRACTICE;
		Event.m_Tick = m_PlaygroundTick;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_ActiveDummy = m_PgActiveDummy;
		Event.m_Value = On;
		m_vTakeTimelineEvents.push_back(Event);
	}
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

void CTas::RecordPgChatLine(int ClientId, int Team, const char *pText)
{
	if(!m_Recording || m_ApplyingTakeTimelineEvent)
		return;
	STasTimelineEvent Event;
	Event.m_Type = TAS_TIMELINE_EVENT_CHAT_LINE;
	Event.m_Tick = m_PlaygroundTick;
	Event.m_Order = m_TasTimelineNextOrder++;
	Event.m_ActiveDummy = m_PgActiveDummy;
	Event.m_Team = Team;
	Event.m_ClientId = ClientId;
	str_copy(Event.m_aText, pText ? pText : "", sizeof(Event.m_aText));
	m_vTakeTimelineEvents.push_back(Event);
}

void CTas::RecordTakeTuningChanges(const CTuningParams *pBefore, bool WasDirty)
{
	if(!m_Recording || m_ApplyingTakeTimelineEvent || !pBefore)
		return;
	const int NumParams = CTuningParams::Num();
	if(NumParams > TAS_TIMELINE_MAX_TUNING_PARAMS)
		return;
	bool Changed = false;
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
	{
		bool ZoneChanged = false;
		for(int i = 0; i < NumParams; ++i)
			ZoneChanged |= pBefore[Zone].NetworkArray()[i] != m_aPgTuning[Zone].NetworkArray()[i];
		if(!ZoneChanged)
			continue;
		STasTimelineEvent Event;
		Event.m_Type = TAS_TIMELINE_EVENT_TUNING;
		Event.m_Tick = m_PlaygroundTick;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_ActiveDummy = m_PgActiveDummy;
		Event.m_Value = m_PgTuningDirty;
		Event.m_TuningZone = Zone;
		Event.m_TuningCount = NumParams;
		for(int i = 0; i < NumParams; ++i)
			Event.m_aTuningValues[i] = m_aPgTuning[Zone].NetworkArray()[i];
		m_vTakeTimelineEvents.push_back(Event);
		Changed = true;
	}
	if(!Changed && WasDirty != m_PgTuningDirty)
	{
		STasTimelineEvent Event;
		Event.m_Type = TAS_TIMELINE_EVENT_TUNING;
		Event.m_Tick = m_PlaygroundTick;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_ActiveDummy = m_PgActiveDummy;
		Event.m_Value = m_PgTuningDirty;
		m_vTakeTimelineEvents.push_back(Event);
	}
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
	const vec2 Pos = m_UseTakeTimelineCursor ? m_TakeTimelineCursor : SpawnerAimPos();
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
	CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
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
	if(!m_RebuildingTakeTimelineState)
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
	if(!m_RebuildingTakeTimelineState)
		GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, aBuf);
}

static bool KxTasCommandUsesCursor(const char *pCmdLine)
{
	if(!pCmdLine)
		return false;
	while(*pCmdLine == ' ')
		++pCmdLine;
	char aCommand[64];
	int Length = 0;
	while(*pCmdLine && *pCmdLine != ' ' && Length < (int)sizeof(aCommand) - 1)
		aCommand[Length++] = *pCmdLine++;
	aCommand[Length] = '\0';
	return str_comp_nocase(aCommand, "tc") == 0 || str_comp_nocase(aCommand, "telecursor") == 0;
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
	if(m_Recording)
		m_UsedFlags |= TAS_USED_PRACTICE;

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
		CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
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
		CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
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

bool CTas::PgChatHandleCommand(const char *pLine, char *pOut, int OutSize, int Team)
{
	if(pOut && OutSize > 0)
		pOut[0] = '\0';
	if(!pLine || pLine[0] != '/')
		return false;
	const bool RecordEvent = m_Recording && !m_ApplyingTakeTimelineEvent;
	size_t CommandEventIndex = 0;
	std::vector<CTuningParams> vTuningBefore;
	const bool WasTuningDirty = m_PgTuningDirty;
	if(RecordEvent)
	{
		STasTimelineEvent Event;
		Event.m_Type = TAS_TIMELINE_EVENT_CHAT_COMMAND;
		Event.m_Tick = m_PlaygroundTick;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_ActiveDummy = m_PgActiveDummy;
		Event.m_Team = Team;
		Event.m_HasCursor = KxTasCommandUsesCursor(pLine + 1);
		if(Event.m_HasCursor)
			Event.m_Cursor = SpawnerAimPos();
		str_copy(Event.m_aText, pLine + 1, sizeof(Event.m_aText));
		CommandEventIndex = m_vTakeTimelineEvents.size();
		m_vTakeTimelineEvents.push_back(Event);
		vTuningBefore.resize(TuneZone::NUM);
		for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
			vTuningBefore[Zone] = m_aPgTuning[Zone];
	}
	std::vector<std::string> vOut;
	const bool WasExecuting = m_ExecutingPgCommand;
	m_ExecutingPgCommand = true;
	const bool Handled = PgRunCommand(pLine + 1, vOut);
	m_ExecutingPgCommand = WasExecuting;
	if(!Handled)
	{
		if(RecordEvent)
			m_vTakeTimelineEvents.pop_back();
		return false;
	}
	if(RecordEvent)
	{
		m_vTakeTimelineEvents[CommandEventIndex].m_Value = (m_UsedFlags & TAS_USED_PRACTICE) != 0;
		RecordTakeTuningChanges(vTuningBefore.data(), WasTuningDirty);
	}
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
		if(pOut[0])
			PgChatAddLine(CChat::CLIENT_MSG, Team, "", pOut);
	}
	return true;
}

void CTas::PgRconExecute(const char *pLine)
{
	if(!pLine || !pLine[0])
		return;
	const bool RecordEvent = m_Recording && !m_ApplyingTakeTimelineEvent;
	size_t CommandEventIndex = 0;
	std::vector<CTuningParams> vTuningBefore;
	const bool WasTuningDirty = m_PgTuningDirty;
	if(RecordEvent)
	{
		STasTimelineEvent Event;
		Event.m_Type = TAS_TIMELINE_EVENT_RCON_COMMAND;
		Event.m_Tick = m_PlaygroundTick;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_ActiveDummy = m_PgActiveDummy;
		Event.m_HasCursor = KxTasCommandUsesCursor(pLine);
		if(Event.m_HasCursor)
			Event.m_Cursor = SpawnerAimPos();
		str_copy(Event.m_aText, pLine, sizeof(Event.m_aText));
		CommandEventIndex = m_vTakeTimelineEvents.size();
		m_vTakeTimelineEvents.push_back(Event);
		vTuningBefore.resize(TuneZone::NUM);
		for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
			vTuningBefore[Zone] = m_aPgTuning[Zone];
	}
	if(m_Recording)
		m_UsedFlags |= TAS_USED_RCON;
	std::vector<std::string> vOut;
	const bool WasExecuting = m_ExecutingPgCommand;
	m_ExecutingPgCommand = true;
	PgRunCommand(pLine, vOut);
	m_ExecutingPgCommand = WasExecuting;
	if(RecordEvent)
	{
		m_vTakeTimelineEvents[CommandEventIndex].m_Value = (m_UsedFlags & TAS_USED_PRACTICE) != 0;
		RecordTakeTuningChanges(vTuningBefore.data(), WasTuningDirty);
	}
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
	const int ActionTick = Tick;
	// "Timeline Tick" (tee only): pretend the tee already existed at that tick.
	// It is stored as the spawn tick, so playback/rewind ages from it instead
	// of from the moment it was placed.
	const int Timeline = Settings.m_EntityTimelineTick;
	if(Timeline > 0)
		Tick = Timeline;

	const bool Ok = Settings.m_Mode == TAS_SPAWN_MODE_TILE ?
		SpawnerSpawnTile(Settings, Pos, Tick, Record) :
		SpawnerSpawnEntity(Settings, Pos, Tick, Record);
	if(Ok && Record && m_Recording)
	{
		m_UsedFlags |= TAS_USED_SPAWNER;
		STasSpawnerEvent Event;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_Record.m_Tick = ActionTick;
		Event.m_Record.m_Mode = Settings.m_Mode;
		Event.m_Record.m_Entity = Settings.m_Entity;
		Event.m_Record.m_Tile = Settings.m_Tile;
		Event.m_Record.m_Physics = Settings.m_Mode == TAS_SPAWN_MODE_TILE ? Settings.m_TilePhysics : Settings.m_EntityPhysics;
		Event.m_Record.m_Pos = Pos;
		Event.m_Record.m_Settings = Settings;
		m_vTakeSpawnerEvents.push_back(Event);
	}
	return Ok;
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

	// Restore every affected layer before replacing an existing tile.
	for(auto It = m_vSpawnedTiles.begin(); It != m_vSpawnedTiles.end(); ++It)
	{
		const SSpawnedTile &T = *It;
		if(T.m_Index != Index)
			continue;
		if(T.m_Physics)
		{
			const float X = TilePos.x + 16.0f, Y = TilePos.y + 16.0f;
			pColl->SetCollisionAt(X, Y, T.m_OldTile);
			if(T.m_TuneSet)
				pColl->RestoreTuneTileAt(X, Y, T.m_OldTuneType, T.m_OldTuneNumber);
			if(T.m_TeleSet)
				pColl->RestoreTeleTileAt(X, Y, T.m_OldTeleType, T.m_OldTeleNumber);
			if(T.m_SpeedupSet)
				pColl->RestoreSpeedupAt(X, Y, T.m_OldSpeedupType, T.m_OldSpeedupForce, T.m_OldSpeedupMaxSpeed, T.m_OldSpeedupAngle);
		}
		m_vSpawnedTiles.erase(It);
		break;
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

void CTas::SpawnerRestoreTakeTiles(int Tick)
{
	CCollision *pColl = m_Playground.Collision();
	if(!pColl || !pColl->GameLayer())
		return;
	for(auto It = m_vSpawnedTiles.rbegin(); It != m_vSpawnedTiles.rend(); ++It)
	{
		const SSpawnedTile &T = *It;
		if(!T.m_Physics || T.m_Index < 0 || T.m_Index >= pColl->GetWidth() * pColl->GetHeight())
			continue;
		const float X = T.m_Pos.x + 16.0f, Y = T.m_Pos.y + 16.0f;
		pColl->SetCollisionAt(X, Y, T.m_OldTile);
		if(T.m_TuneSet)
			pColl->RestoreTuneTileAt(X, Y, T.m_OldTuneType, T.m_OldTuneNumber);
		if(T.m_TeleSet)
			pColl->RestoreTeleTileAt(X, Y, T.m_OldTeleType, T.m_OldTeleNumber);
		if(T.m_SpeedupSet)
			pColl->RestoreSpeedupAt(X, Y, T.m_OldSpeedupType, T.m_OldSpeedupForce, T.m_OldSpeedupMaxSpeed, T.m_OldSpeedupAngle);
	}
	m_vSpawnedTiles.clear();
	m_TileVisualDirty = true;
	for(const STasSpawnerRecord &Record : m_vTakeSpawner)
		if(Record.m_Mode == TAS_SPAWN_MODE_TILE)
			SpawnerSpawnAt(Record.m_Settings, Record.m_Pos, Record.m_Tick, false);
	for(const STasSpawnerEvent &Event : m_vTakeSpawnerEvents)
		if(Event.m_Record.m_Tick < Tick && Event.m_Record.m_Mode == TAS_SPAWN_MODE_TILE)
			ApplyTakeSpawnerEvent(Event);
}

void CTas::ApplyTakeSpawnerEvent(const STasSpawnerEvent &Event)
{
	const STasSpawnerSettings SavedSettings = m_Spawner;
	const bool WasRecording = m_Recording;
	const std::vector<STasSpawnerRecord> SavedRecords = m_vSpawnerRecords;
	const bool SavedHasPrev = m_SpawnerHasPrev;
	const vec2 SavedPrevPos = m_SpawnerPrevPos;
	const bool SavedLoop = m_SpawnerLoop;
	const float SavedLoopMs = m_SpawnerLoopMs;
	m_Recording = false;
	const STasSpawnerRecord &Record = Event.m_Record;
	if(Event.m_Type == 0)
		SpawnerSpawnAt(Record.m_Settings, Record.m_Pos, Record.m_Tick, false);
	else
	{
		m_Spawner = Record.m_Settings;
		m_Spawner.m_CoordsMode = TAS_SPAWN_COORD_COORDS;
		m_Spawner.m_X = Record.m_Pos.x;
		m_Spawner.m_Y = Record.m_Pos.y;
		m_Spawner.m_Snap = false;
		if(Event.m_Type == 1)
			SpawnerKill();
		else if(Event.m_Type == 2)
			SpawnerKillAll();
	}
	m_vSpawnerRecords = SavedRecords;
	m_Spawner = SavedSettings;
	m_Recording = WasRecording;
	m_SpawnerHasPrev = SavedHasPrev;
	m_SpawnerPrevPos = SavedPrevPos;
	m_SpawnerLoop = SavedLoop;
	m_SpawnerLoopMs = SavedLoopMs;
}

void CTas::ApplyTakeTimelineEvent(const STasTimelineEvent &Event, bool Rebuilding)
{
	const bool WasApplying = m_ApplyingTakeTimelineEvent;
	const bool WasRebuilding = m_RebuildingTakeTimelineState;
	const bool WasUseCursor = m_UseTakeTimelineCursor;
	const vec2 WasCursor = m_TakeTimelineCursor;
	const int WasActiveDummy = m_PgActiveDummy;
	m_ApplyingTakeTimelineEvent = true;
	m_RebuildingTakeTimelineState = Rebuilding;
	m_PgActiveDummy = std::clamp(Event.m_ActiveDummy, 0, MAX_DUMMIES - 1);
	m_UseTakeTimelineCursor = Event.m_HasCursor;
	m_TakeTimelineCursor = Event.m_Cursor;

	switch(Event.m_Type)
	{
	case TAS_TIMELINE_EVENT_CHAT_LINE:
		PgChatAddLine(Event.m_ClientId, Event.m_Team, "", Event.m_aText);
		if(!Rebuilding)
			GameClient()->m_Chat.TasAddVirtualLine(Event.m_ClientId, Event.m_Team, Event.m_aText);
		break;
	case TAS_TIMELINE_EVENT_CHAT_COMMAND:
	{
		char aCommand[sizeof(Event.m_aText) + 2];
		str_format(aCommand, sizeof(aCommand), "/%s", Event.m_aText);
		char aOutput[1024];
		if(PgChatHandleCommand(aCommand, aOutput, sizeof(aOutput), Event.m_Team) && aOutput[0] && !Rebuilding)
			GameClient()->m_Chat.TasAddVirtualLine(CChat::CLIENT_MSG, Event.m_Team, aOutput);
		break;
	}
	case TAS_TIMELINE_EVENT_RCON_COMMAND:
		PgRconEcho(Event.m_aText);
		PgRconExecute(Event.m_aText);
		break;
	case TAS_TIMELINE_EVENT_PRACTICE:
		SetPgPractice(Event.m_Value);
		break;
	case TAS_TIMELINE_EVENT_TUNING:
		if(Event.m_TuningCount > 0 && Event.m_TuningCount <= TAS_TIMELINE_MAX_TUNING_PARAMS &&
			in_range(Event.m_TuningZone, TuneZone::NUM - 1) &&
			Event.m_TuningCount == CTuningParams::Num())
			for(int i = 0; i < Event.m_TuningCount; ++i)
				m_aPgTuning[Event.m_TuningZone].NetworkArray()[i] = Event.m_aTuningValues[i];
		m_PgTuningDirty = Event.m_Value;
		break;
	}

	m_PgActiveDummy = WasActiveDummy;
	m_UseTakeTimelineCursor = WasUseCursor;
	m_TakeTimelineCursor = WasCursor;
	m_RebuildingTakeTimelineState = WasRebuilding;
	m_ApplyingTakeTimelineEvent = WasApplying;
}

void CTas::ApplyTakeTimelineEvents(int Tick)
{
	const auto SpawnerBegin = std::lower_bound(m_vTakeSpawnerEvents.begin(), m_vTakeSpawnerEvents.end(), Tick,
		[](const STasSpawnerEvent &Event, int Value) { return Event.m_Record.m_Tick < Value; });
	const auto TimelineBegin = std::lower_bound(m_vTakeTimelineEvents.begin(), m_vTakeTimelineEvents.end(), Tick,
		[](const STasTimelineEvent &Event, int Value) { return Event.m_Tick < Value; });
	auto SpawnerIt = SpawnerBegin;
	auto TimelineIt = TimelineBegin;
	while((SpawnerIt != m_vTakeSpawnerEvents.end() && SpawnerIt->m_Record.m_Tick == Tick) ||
		(TimelineIt != m_vTakeTimelineEvents.end() && TimelineIt->m_Tick == Tick))
	{
		if(TimelineIt == m_vTakeTimelineEvents.end() || TimelineIt->m_Tick != Tick ||
			(SpawnerIt != m_vTakeSpawnerEvents.end() && SpawnerIt->m_Record.m_Tick == Tick && SpawnerIt->m_Order < TimelineIt->m_Order))
		{
			ApplyTakeSpawnerEvent(*SpawnerIt++);
		}
		else
			ApplyTakeTimelineEvent(*TimelineIt++, false);
	}
}

void CTas::RebuildTakeTimelineState(int Tick)
{
	if(!m_HasTakePgStart)
		return;
	m_PgPractice = m_TakePgPractice;
	m_PgInvincible = m_TakePgInvincible;
	m_PgSuper = m_TakePgSuper;
	m_PgPaused = m_TakePgPaused;
	m_PgSpec = m_TakePgSpec;
	m_PgTuningDirty = m_TakePgTuningDirty;
	m_PgTuningInit = true;
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
		m_aPgTuning[Zone] = m_aTakePgTuning[Zone];
	m_vPgChat = m_vTakeChat;
	m_vPgRcon = m_vTakeRcon;

	const bool WasRebuilding = m_RebuildingTakeTimelineState;
	m_RebuildingTakeTimelineState = true;
	const auto End = std::lower_bound(m_vTakeTimelineEvents.begin(), m_vTakeTimelineEvents.end(), Tick,
		[](const STasTimelineEvent &Event, int Value) { return Event.m_Tick < Value; });
	for(auto It = m_vTakeTimelineEvents.begin(); It != End; ++It)
		ApplyTakeTimelineEvent(*It, true);
	m_RebuildingTakeTimelineState = WasRebuilding;

	GameClient()->m_Chat.TasRefreshVirtualLines();
	if(m_PgVirtualRcon && m_Joined)
	{
		GameClient()->m_GameConsole.ClearRemoteBacklog();
		const int Start = std::max(0, (int)m_vPgRcon.size() - 200);
		for(int i = Start; i < (int)m_vPgRcon.size(); ++i)
			GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, m_vPgRcon[i].c_str());
	}
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

	if(m_Recording)
	{
		STasSpawnerEvent Event;
		Event.m_Type = 2;
		Event.m_Order = m_TasTimelineNextOrder++;
		Event.m_Record.m_Tick = m_PlaygroundTick;
		Event.m_Record.m_Mode = m_Spawner.m_Mode;
		Event.m_Record.m_Entity = m_Spawner.m_Entity;
		Event.m_Record.m_Tile = m_Spawner.m_Tile;
		Event.m_Record.m_Settings = m_Spawner;
		m_vTakeSpawnerEvents.push_back(Event);
	}
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

void CTas::BuildPlaygroundRenderState(CCharacter *pChar, const CCharacterCore *pPrevCore, vec2 Aim, CNetObj_Character &Prev, CNetObj_Character &Cur)
{
	const CCharacterCore *pCore = pChar->Core();
	Cur = {};
	pCore->Write(&Cur);
	Cur.m_Angle = static_cast<int>(angle(Aim) * 256.0f);
	Cur.m_Weapon = std::clamp(pCore->m_ActiveWeapon, 0, NUM_WEAPONS - 1);
	Cur.m_AttackTick = m_PlaygroundTick - pChar->GetAttackTick() - 1;
	Cur.m_Emote = EMOTE_NORMAL;
	Prev = Cur;
	if(pPrevCore)
	{
		pPrevCore->Write(&Prev);
		Prev.m_AttackTick--;
		// Low TPS: the previous core carries the angle of its own tick while
		// Cur uses the CURRENT mouse/input aim. Interpolating between the two
		// makes the eyes visibly swing from the old tick towards the new aim.
		// Keep the aim angle in sync so only the position is interpolated.
		Prev.m_Angle = Cur.m_Angle;
	}
	else if(!m_Paused && pChar->GetCid() != m_LocalId)
	{
		Prev.m_X = round_to_int(pChar->m_PrevPos.x);
		Prev.m_Y = round_to_int(pChar->m_PrevPos.y);
	}
	// Fake player rendering must never resolve the hook through the server snapshot.
	const int HookedId = pCore->HookedPlayer();
	if(const CCharacter *pTarget = m_Playground.GetCharacterById(HookedId))
	{
		const vec2 Pos = pTarget->Core()->m_Pos;
		const vec2 PrevPos = m_Paused ? Pos : pTarget->m_PrevPos;
		Cur.m_HookX = round_to_int(Pos.x);
		Cur.m_HookY = round_to_int(Pos.y);
		Prev.m_HookX = round_to_int(PrevPos.x);
		Prev.m_HookY = round_to_int(PrevPos.y);
	}
	Cur.m_HookedPlayer = Prev.m_HookedPlayer = -1;
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

	const float Intra = m_Paused ? 1.0f : RenderIntraAlpha();

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

	// Playground-local dummies — appended to the same hook/tee/nameplate passes
	// so they visibly exist in the sandbox.
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		if(!m_aPgDummyConnected[D] || !m_apPgDummies[D])
			continue;
		CCharacter *pDummy = m_apPgDummies[D];
		STeeDraw Draw = {};
		Draw.pName = GameClient()->m_BotNet.m_Dummy.ProfileName(D);
		Draw.pClan = GameClient()->m_BotNet.m_Dummy.ProfileClan(D);
		Draw.Id = D;
		const CCharacterCore *pPrevCore = nullptr;
		if(!m_Paused && m_vPlaygroundHistory.size() >= 2)
		{
			for(const STasDummyState &Dummy : (m_vPlaygroundHistory.end() - 2)->vDummies)
				if(Dummy.m_Slot == D && Dummy.m_Id == pDummy->GetCid())
				{
					pPrevCore = &Dummy.m_Core;
					break;
				}
		}
		const auto &Input = pDummy->GetLatestInput();
		vec2 Aim(Input.m_TargetX, Input.m_TargetY);
		if(D == m_PgActiveDummy && !m_Playing)
			Aim = pGame->m_Controls.m_aMousePos[0];
		BuildPlaygroundRenderState(pDummy, pPrevCore, Aim, Draw.Prev, Draw.Cur);
		Draw.Info = PgDummyTeeRenderInfo(D);
		if(pDummy->m_FreezeTime > 0)
			Draw.Info.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
		vTeeDraw.push_back(Draw);
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
	CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
	if(!pChar || !m_Playground.Collision())
		return;
	const vec2 AimInput = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	if(length(AimInput) < 0.001f)
		return;
	const vec2 Aim = normalize(AimInput);
	const vec2 AimPoint = SpawnerTargetPos();
	const bool AlwaysOther = g_Config.m_ClShowHookCollOther == 2;
	const float Alpha = std::clamp(g_Config.m_ClHookCollAlpha / 100.0f, 0.0f, 1.0f);
	if(Alpha <= 0.0f)
		return;

	// Trace against the private playground world, not the server snapshot.
	// Map collision clips the ray; hookable tees clip it sooner and use the
	// dedicated tee-collision color (yellow by default).
	auto DrawPreview = [&](CCharacter *pSource, vec2 Pos, vec2 Direction) {
		if(!pSource || pSource->Core()->m_HookState != HOOK_IDLE ||
			pSource->Core()->m_HookHitDisabled || !pSource->Core()->m_Tuning.m_PlayerHooking)
			return;
		if(length(Direction) < 0.001f)
			return;
		Direction = normalize(Direction);
		const CCharacterCore *pSourceCore = pSource->Core();
		const CTuningParams *pTuning = m_Playground.GetTuning(pSource->GetOverriddenTuneZone());
		const float HookLength = pTuning ? pTuning->m_HookLength : 380.0f;
		const float StartDistance = CCharacterCore::PhysicalSize() * 1.5f;
		const vec2 Start = Pos + Direction * StartDistance;
		const vec2 End = Start + Direction * HookLength;
		vec2 MapHitPos = End;
		int Tele = 0;
		const int MapHit = m_Playground.Collision()->IntersectLineTeleHook(Start, End, &MapHitPos, nullptr, &Tele);
		vec2 Stop = MapHit ? MapHitPos : End;
		int ColorVal = MapHit && MapHit != TILE_NOHOOK ?
			g_Config.m_ClHookCollColorHookableColl : g_Config.m_ClHookCollColorNoColl;

		CCharacter *pHitChar = nullptr;
		float BestAlong = HookLength;
		for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_CHARACTER); pEnt; pEnt = pEnt->TypeNext())
		{
			CCharacter *pTarget = static_cast<CCharacter *>(pEnt);
			if(pTarget == pSource)
				continue;
			const CCharacterCore *pTargetCore = pTarget->Core();
			const int SourceId = pSource->GetCid();
			const int TargetId = pTarget->GetCid();
			const bool InTeamRange = SourceId >= 0 && SourceId < MAX_CLIENTS &&
				TargetId >= 0 && TargetId < MAX_CLIENTS;
			if(!(pSourceCore->m_Super || pTargetCore->m_Super) &&
				((InTeamRange && !m_Playground.Teams()->CanCollide(TargetId, SourceId)) ||
				 pTargetCore->m_Solo || pSourceCore->m_Solo))
				continue;

			vec2 Closest;
			if(!closest_point_on_line(Start, Stop, pTargetCore->m_Pos, Closest))
				continue;
			const float Along = dot(Closest - Start, Direction);
			const float Radius = CCharacterCore::PhysicalSize() + 2.0f;
			if(Along < 0.0f || Along >= BestAlong || distance(Closest, pTargetCore->m_Pos) >= Radius)
				continue;
			BestAlong = Along;
			pHitChar = pTarget;
		}

		if(pHitChar)
		{
			const float Radius = CCharacterCore::PhysicalSize() + 2.0f;
			Stop = pHitChar->Core()->m_Pos - Direction * Radius;
			ColorVal = g_Config.m_ClHookCollColorTeeColl;
		}
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(ColorVal)).WithAlpha(Alpha));
		IGraphics::CLineItem Line(Pos, Stop);
		Graphics()->LinesDraw(&Line, 1);
		Graphics()->LinesEnd();
	};

	if(pChar->Core()->m_HookState != HOOK_IDLE)
		return;
	const vec2 Pos = PlaygroundActiveTeeRenderPos();
	DrawPreview(pChar, Pos, Aim);

	if(g_Config.m_ClShowHookCollOther <= 0)
		return;
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_CHARACTER); pEnt; pEnt = pEnt->TypeNext())
	{
		CCharacter *pOther = static_cast<CCharacter *>(pEnt);
		if(pOther == pChar)
			continue;
		if(!AlwaysOther && distance(pOther->Core()->m_Pos, AimPoint) > CCharacterCore::PhysicalSize())
			continue;
		DrawPreview(pOther, pOther->Core()->m_Pos, Aim);
	}
}

void CTas::JoinLeavePlayground()
{
	if(m_Joined)
		LeavePlayground();
	else
		JoinPlayground();
}

void CTas::SetPgDummyLimit(int Limit)
{
	m_PgDummyLimit = std::clamp(Limit, 1, (int)MAX_DUMMIES);
	for(int D = m_PgDummyLimit; D < MAX_DUMMIES; ++D)
		m_aPgDummyConnected[D] = false;
	if(m_PgActiveDummy >= m_PgDummyLimit || (m_PgActiveDummy > 0 && !m_aPgDummyConnected[m_PgActiveDummy]))
		SetPgActiveDummy(0);
	if(m_Joined)
		SyncPgDummies();
}

void CTas::SetPgActiveDummy(int D)
{
	if(D < 0 || D >= m_PgDummyLimit)
		return;
	if(D > 0 && !m_aPgDummyConnected[D])
		return;
	if(D == m_PgActiveDummy)
		return;
	const int Previous = m_PgActiveDummy;
	if(m_Joined && g_Config.m_ClDummyResetOnSwitch == 1)
		ResetPgSlotInput(Previous);
	else if(m_Joined)
	{
		// DDNet keeps the old slot's key state when switching: key-up events
		// after the switch belong to the new active slot. Snapshot that state
		// now, rather than waiting for a simulation tick (TAS may be paused).
		const CCharacter *pPrevious = PlaygroundCharacter(Previous);
		if(pPrevious)
		{
			CControls &Controls = GameClient()->m_Controls;
			CNetObj_PlayerInput Pending = Controls.m_aInputData[0];
			Pending.m_TargetX = round_to_int(Controls.m_aMousePos[0].x);
			Pending.m_TargetY = round_to_int(Controls.m_aMousePos[0].y);
			Pending.m_Direction = Controls.m_aInputDirectionRight[0] - Controls.m_aInputDirectionLeft[0];
			Pending.m_PlayerFlags = PLAYERFLAG_PLAYING;
			const CNetObj_PlayerInput &Prev = pPrevious->GetLatestInput();
			Pending.m_Fire = (Pending.m_Fire & 1) ? (Prev.m_Fire | 1) : ((Prev.m_Fire + 1) & ~1);
			Pending.m_Fire &= INPUT_STATE_MASK;
			Pending.m_NextWeapon = (Prev.m_NextWeapon + Pending.m_NextWeapon - m_PgLastKeyboardInput.m_NextWeapon) & INPUT_STATE_MASK;
			Pending.m_PrevWeapon = (Prev.m_PrevWeapon + Pending.m_PrevWeapon - m_PgLastKeyboardInput.m_PrevWeapon) & INPUT_STATE_MASK;
			m_aPgPendingSwitchInput[Previous] = Pending;
			m_aPgPendingSwitchInputActive[Previous] = true;
			m_aPgDummyInputs[Previous] = Pending;
		}
	}
	// The newly selected tee starts with released controls, as its key-up
	// events now belong to this slot. Preserve its aim and weapon selection.
	if(m_Joined)
		ResetPgSlotInput(D);
	m_aPgPendingSwitchInputActive[D] = false;
	m_PgActiveDummy = D;
	m_PgLastKeyboardInput = GameClient()->m_Controls.m_aInputData[0];
	// Do NOT touch cl_dummy: the real connection (and any external dummy) keeps
	// its state. The keyboard always lives in slot 0; StepWorld routes it to
	// either Main or the selected sandbox dummy, never both.
}

void CTas::ResetPgSlotInput(int Slot)
{
	CCharacter *pChar = PlaygroundCharacter(Slot);
	if(!pChar)
		return;
	CNetObj_PlayerInput Input = pChar->GetLatestInput();
	Input.m_Direction = 0;
	Input.m_Jump = 0;
	Input.m_Hook = 0;
	if(Input.m_Fire & 1)
		Input.m_Fire++;
	Input.m_Fire &= INPUT_STATE_MASK;
	pChar->SetLatestInput(Input);
	m_aPgDummyInputs[Slot] = Input;
}

void CTas::ResetPgDummyInputs()
{
	if(!m_Joined)
		return;
	for(int Slot = 0; Slot < m_PgDummyLimit; ++Slot)
		if(Slot != m_PgActiveDummy)
			ResetPgSlotInput(Slot);
}

CCharacter *CTas::PlaygroundCharacter(int Slot)
{
	if(Slot == 0)
		return m_Playground.GetCharacterById(m_LocalId);
	if(Slot < 1 || Slot >= MAX_DUMMIES || !m_aPgDummyConnected[Slot])
		return nullptr;
	return m_apPgDummies[Slot];
}

const CCharacter *CTas::PlaygroundCharacter(int Slot) const
{
	return const_cast<CTas *>(this)->PlaygroundCharacter(Slot);
}

int CTas::PlaygroundActiveId() const
{
	if(!PlaygroundCharacter(m_PgActiveDummy))
		return -1;
	return m_PgActiveDummy == 0 ? m_LocalId : m_aPgDummyIds[m_PgActiveDummy];
}

vec2 CTas::PlaygroundSlotPos(int Slot) const
{
	const CCharacter *pChar = PlaygroundCharacter(Slot);
	return pChar ? pChar->Core()->m_Pos : vec2(0.0f, 0.0f);
}

vec2 CTas::PlaygroundActiveTeeRenderPos()
{
	return PlaygroundSlotRenderPos(m_PgActiveDummy);
}

vec2 CTas::PlaygroundSlotRenderPos(int Slot)
{
	if(Slot == 0)
		return PlaygroundTeeRenderPos();
	CCharacter *pChar = PlaygroundCharacter(Slot);
	if(!pChar)
		return vec2(0.0f, 0.0f);
	const float Alpha = m_Paused ? 1.0f : RenderIntraAlpha();
	vec2 Prev = pChar->m_PrevPos;
	if(!m_Paused && m_vPlaygroundHistory.size() >= 2)
		for(const STasDummyState &Dummy : (m_vPlaygroundHistory.end() - 2)->vDummies)
			if(Dummy.m_Slot == Slot && Dummy.m_Id == pChar->GetCid())
			{
				Prev = Dummy.m_Core.m_Pos;
				break;
			}
	return mix(Prev, pChar->Core()->m_Pos, Alpha);
}

int CTas::PlaygroundActiveWeapon() const
{
	const CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
	return pChar ? pChar->Core()->m_ActiveWeapon : 0;
}

void CTas::TogglePgDummy(int D)
{
	if(D < 1 || D >= m_PgDummyLimit)
		return;
	m_aPgDummyConnected[D] = !m_aPgDummyConnected[D];
	if(!m_aPgDummyConnected[D] && m_PgActiveDummy == D)
		SetPgActiveDummy(0);
	if(m_Joined)
		SyncPgDummies();
}

void CTas::DestroyPgDummies()
{
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		CCharacter *pChar = m_apPgDummies[D];
		m_apPgDummies[D] = nullptr;
		m_aPgDummyIds[D] = 0;
		if(!pChar)
			continue;
		m_Playground.RemoveCharacter(pChar);
		m_Playground.RemoveEntity(pChar);
		delete pChar;
	}
}

void CTas::SyncPgDummies()
{
	if(!m_Joined)
		return;
	CCharacter *pLocal = m_Playground.GetCharacterById(m_LocalId);
	if(!pLocal)
		return;
	const vec2 Base = pLocal->Core()->m_Pos;
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		CCharacter *&pSlot = m_apPgDummies[D];
		// v6.18: any slot below the playground limit can be connected; an
		// unconfigured slot simply renders with the shared dummy_* defaults.
		if(!m_aPgDummyConnected[D])
		{
			if(pSlot)
			{
				m_Playground.RemoveCharacter(pSlot);
				m_Playground.ReleaseHooked(pSlot->GetCid());
				m_Playground.RemoveEntity(pSlot);
				delete pSlot;
				pSlot = nullptr;
				m_aPgDummyIds[D] = 0;
			}
			m_aPgDummyConnected[D] = false;
			continue;
		}
		if(pSlot)
			continue;
		// Pick a free sandbox id. The playground starts as a copy of the real
		// world, so names like D1 are not guaranteed to be free.
		int Id = -1;
		for(int Try = 1; Try < MAX_CLIENTS; ++Try)
		{
			const int Candidate = (m_LocalId + D + Try) % MAX_CLIENTS;
			if(Candidate == 0 || Candidate == m_LocalId || m_Playground.GetCharacterById(Candidate))
				continue;
			Id = Candidate;
			break;
		}
		if(Id < 0)
			continue;
		// Find a nearby free spawn. The old D * 8 offset put a 28 px tee deeply
		// inside Main (and other dummies), so the player-collision solver gave
		// both tees a large separating impulse on the first tick.
		vec2 SpawnPos = Base;
		bool FoundSpawn = false;
		const float Spacing = CCharacterCore::PhysicalSize() + 4.0f;
		for(int Ring = 1; Ring <= 8 && !FoundSpawn; ++Ring)
		{
			const vec2 aOffsets[] = {
				vec2(Spacing * Ring, 0.0f),
				vec2(-Spacing * Ring, 0.0f),
				vec2(0.0f, -Spacing * Ring),
				vec2(0.0f, Spacing * Ring),
			};
			for(const vec2 &Offset : aOffsets)
			{
				const vec2 Candidate = Base + Offset;
				if(m_Playground.Collision()->TestBox(Candidate, vec2(CCharacterCore::PhysicalSize(), CCharacterCore::PhysicalSize())))
					continue;
				bool Overlap = false;
				for(int Idx = 0; Idx < MAX_CLIENTS; ++Idx)
				{
					const CCharacter *pOther = m_Playground.GetCharacterById(Idx);
					if(pOther && distance(Candidate, pOther->Core()->m_Pos) < Spacing)
					{
						Overlap = true;
						break;
					}
				}
				if(!Overlap)
				{
					SpawnPos = Candidate;
					FoundSpawn = true;
					break;
				}
			}
		}
		if(!FoundSpawn)
		{
			m_aPgDummyConnected[D] = false;
			continue;
		}

		CNetObj_Character Char;
		mem_zero(&Char, sizeof(Char));
		Char.m_X = (int)SpawnPos.x;
		Char.m_Y = (int)SpawnPos.y;
		Char.m_Angle = 0;
		Char.m_Weapon = WEAPON_GUN;
		Char.m_HookState = HOOK_IDLE;
		Char.m_HookX = Char.m_X;
		Char.m_HookY = Char.m_Y;
		Char.m_HookedPlayer = -1;
		Char.m_Emote = EMOTE_NORMAL;
		Char.m_AttackTick = 0;
		pSlot = new CCharacter(&m_Playground, Id, &Char);
		pSlot->GiveWeapon(WEAPON_HAMMER);
		pSlot->m_Pos = vec2(Char.m_X, Char.m_Y);
		pSlot->m_PrevPos = pSlot->m_Pos;
		// Locally controlled sandbox tees must have the same prediction status
		// as the main playground tee. Leaving this false changes weak-hook /
		// anti-ping paths and produces non-DDNet hook behaviour.
		pSlot->m_IsLocal = true;
		m_Playground.InsertEntity(pSlot);
		m_Playground.Teams()->Team(Id, m_Playground.Teams()->Team(m_LocalId));
		m_Playground.Teams()->SetSolo(Id, m_Playground.Teams()->GetSolo(m_LocalId));
		m_aPgDummyIds[D] = Id;
	}
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
	// The real client may disable DDNet tile prediction (e.g. on vanilla
	// servers). The playground is a local DDNet simulation and must always
	// process state-changing tiles such as Solo, NPC/NPH and endless modes.
	m_Playground.m_WorldConfig.m_PredictTiles = true;
	m_Playground.m_WorldConfig.m_PredictDDRace = true;
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
	m_PgActiveDummy = 0;
	// Playground slots are virtual and must not make the engine select an
	// unconnected network connection. Keep the real client on Main while the
	// sandbox is active and restore the previous real slot on leave.
	m_PgSavedClDummy = g_Config.m_ClDummy;
	g_Config.m_ClDummy = 0;
	// v6.18: the playground limit is a local, user-controlled setting (TAS
	// Dummies slider) — do NOT reset it to the real client limit here, or the
	// slider and the dummy list would jump back on every playground join.
	m_Joined = true;
	// v6.18: create playground-only dummy tees. No client connections are opened.
	SyncPgDummies();
	GameClient()->m_AimBot.ResetPlayground();
	GameClient()->m_BotNet.ResetPlaygroundAled();
	GameClient()->m_Controls.m_SilentAimActive = false;
	GameClient()->m_BotNet.m_PgTripleFlyHooking = false;
	GameClient()->m_BotNet.m_PgTripleFlyDummy = -1;
	m_PgInputHistoryNext = 0;
	m_PgInputHistoryCount = 0;
	// Mirror the real pause/spec state into the sandbox: entering the playground
	// while paused/spectating on the server starts in the same mode there. Only
	// the sandbox is affected — the real world keeps its own state untouched.
	{
		const CGameClient::CClientData &Real = pGame->m_aClients[LocalId];
		m_PgPaused = Real.m_Paused && !Real.m_Spec;
		m_PgSpec = Real.m_Spec;
	}
	CancelPreparation();
	m_RealPlaying = false;
	m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
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
	std::fill(std::begin(m_aPgPendingSwitchInputActive), std::end(m_aPgPendingSwitchInputActive), false);
	m_vPlaygroundHistory.push_back(CaptureState());
	m_aPgDummyInputs[0] = pGame->m_Controls.m_aInputData[0];
	m_PgLastKeyboardInput = pGame->m_Controls.m_aInputData[0];
	RebaseCounters();

	// Virtual chat: swap the visible chat buffer to the playground one.
	if(m_PgMaster && m_PgVirtualChat)
		GameClient()->m_Chat.TasSetVirtual(true);
}

void CTas::LeavePlayground()
{
	const bool WasJoined = m_Joined;
	// Leaving while Record is armed would lose the take — finalize it into the
	// saved buffer first, exactly like pressing Stop Record (the user can still
	// Save to disk afterwards from the File section).
	if(m_Recording && !m_vRecord.empty())
	{
		m_Recording = false;
		m_vSaved = std::move(m_vRecord);
		m_SavedStart = std::move(m_RecordStart);
	}
	// Virtual chat off first: it copies the playground lines back into the
	// CTas buffer and restores the real chat lines (world state not needed).
	GameClient()->m_Chat.TasSetVirtual(false);
	// v6.18: playground dummies live only inside the sandbox — destroy them
	// before the world is cleared. The real client connections are untouched.
	if(WasJoined)
	{
		DestroyPgDummies();
		GameClient()->m_AimBot.ResetPlayground();
		GameClient()->m_BotNet.ResetPlaygroundAled();
		GameClient()->m_BotNet.m_PgTripleFlyHooking = false;
		GameClient()->m_BotNet.m_PgTripleFlyDummy = -1;
		m_PgActiveDummy = 0;
		g_Config.m_ClDummy = (m_PgSavedClDummy > 0 && Client()->DummyConnected(m_PgSavedClDummy)) ? m_PgSavedClDummy : 0;
		m_PgSavedClDummy = 0;
	}
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

// ---------------------------------------------------------------------------
// Real-world playback preparation. The take records where every dummy stood
// when Record started. On a real server those dummies stand somewhere else:
//   Play → yellow "Preparation" → tab Pathfinder (BFS + A*, Very Low) walks
//   every real dummy to its recorded start → red "Stop", replay begins.
// Route generation and the replayed inputs come exclusively from the tab
// Pathfinder (PfComputeFlowField / PfResetRunForDummy / m_PfFullInputs).
// ---------------------------------------------------------------------------

static const int TAS_PREP_TIMEOUT_TICKS = 50 * 15; // 15 seconds at 50 TPS

// Snapshot of everything the TAS preparation borrows from the tab Pathfinder.
// The user's painted forbidden / finish tiles, the BFS fields and the visible
// A* result must all look exactly the same after preparation finishes.
struct STasSavedPfState
{
	int m_State = 0;
	int m_Effort = 2;
	int m_NetworkBuffer = 0;
	int m_MapW = 0;
	int m_MapH = 0;
	std::vector<unsigned char> m_ForbiddenGrid;
	std::vector<unsigned char> m_CustomFinishGrid;
	std::vector<vec2> m_ForbiddenTiles;
	std::vector<vec2> m_CustomFinishTiles;
	std::vector<vec2> m_FinishTiles;
	std::vector<float> m_FlowField;
	std::vector<float> m_ScoreField;
	int m_ScoreW = 0;
	int m_ScoreH = 0;

	vec2 m_StartPos = vec2(0.0f, 0.0f);
	vec2 m_CurPos = vec2(0.0f, 0.0f);
	vec2 m_CurVel = vec2(0.0f, 0.0f);
	int m_CurHookState = 0;
	vec2 m_CurHookPos = vec2(0.0f, 0.0f);
	vec2 m_CurHookDir = vec2(0.0f, 0.0f);
	int m_CurHookTick = 0;
	int m_CurFreezeTime = 0;
	int m_CurJumped = 0;
	int m_TickCounter = 0;
	int m_ChunkCount = 0;
	int m_TotalFreezeTicks = 0;
	int m_BacktrackIdx = 0;
	std::array<PfBacktrackEntry, PF_BACKTRACK_DEPTH> m_Backtrack;
	std::vector<vec2> m_VPath;
	std::vector<PfHookSeg> m_VHookSegs;
	std::vector<vec2> m_VPathRender;
	std::vector<PfHookSeg> m_VHookSegsRender;
	std::vector<std::pair<vec2, vec2>> m_VBranchSegsRender;
	std::vector<CNetObj_PlayerInput> m_FullInputs;
	size_t m_FullInputsIdx = 0;
	int m_LocalId = -1;
	bool m_UseExactGoal = false;
	vec2 m_ExactGoal = vec2(0.0f, 0.0f);

	bool m_APathReady = false;
	int m_AGoalIdx = -1;
	bool m_AStarted = false;
	int m_AExpandCount = 0;
	float m_ABestH = 1e18f;
	float m_AHRoot = 1e18f;
	bool m_AGreedy = false;
	bool m_AGreedyTried = false;
	int m_AChunkCur = 8;
	bool m_ARefined = false;
	std::vector<CNetObj_PlayerInput> m_ARefinedInputs;
	std::vector<vec2> m_ARefinedPath;
	std::vector<PfHookSeg> m_ARefinedSegs;
	CBotNet::PfAState m_ARefinedEnd;
	int m_ARefinedEndFreeze = 0;
	int m_UiBestDone = 0;
	int m_UiBestTotal = 0;
};

void CTas::SaveTabPathfinderState()
{
	CBotNet &Bot = GameClient()->m_BotNet;
	auto p = std::make_shared<STasSavedPfState>();
	STasSavedPfState &S = *p;
	S.m_State = Bot.m_PfState;
	S.m_Effort = g_Config.m_KxPfEffort;
	S.m_NetworkBuffer = g_Config.m_KxNetworkBuffer;
	S.m_MapW = Bot.m_MapWidth;
	S.m_MapH = Bot.m_MapHeight;
	const size_t TileCount = (S.m_MapW > 0 && S.m_MapH > 0) ? (size_t)S.m_MapW * S.m_MapH : 0;
	if(TileCount && Bot.m_pForbiddenGrid)
		S.m_ForbiddenGrid.assign(Bot.m_pForbiddenGrid, Bot.m_pForbiddenGrid + TileCount);
	if(TileCount && Bot.m_pCustomFinishGrid)
		S.m_CustomFinishGrid.assign(Bot.m_pCustomFinishGrid, Bot.m_pCustomFinishGrid + TileCount);
	S.m_ForbiddenTiles = Bot.m_PfForbiddenTiles;
	S.m_CustomFinishTiles = Bot.m_PfCustomFinishTiles;
	S.m_FinishTiles = Bot.m_PfFinishTiles;
	if(Bot.m_PfFlowField && TileCount)
		S.m_FlowField.assign(Bot.m_PfFlowField, Bot.m_PfFlowField + TileCount);
	if(Bot.m_PfScoreField && Bot.m_PfScoreFieldW > 0 && Bot.m_PfScoreFieldH > 0)
	{
		S.m_ScoreW = Bot.m_PfScoreFieldW;
		S.m_ScoreH = Bot.m_PfScoreFieldH;
		S.m_ScoreField.assign(Bot.m_PfScoreField, Bot.m_PfScoreField + (size_t)S.m_ScoreW * S.m_ScoreH);
	}
	S.m_StartPos = Bot.m_PfStartPos;
	S.m_CurPos = Bot.m_PfCurPos;
	S.m_CurVel = Bot.m_PfCurVel;
	S.m_CurHookState = Bot.m_PfCurHookState;
	S.m_CurHookPos = Bot.m_PfCurHookPos;
	S.m_CurHookDir = Bot.m_PfCurHookDir;
	S.m_CurHookTick = Bot.m_PfCurHookTick;
	S.m_CurFreezeTime = Bot.m_PfCurFreezeTime;
	S.m_CurJumped = Bot.m_PfCurJumped;
	S.m_TickCounter = Bot.m_PfTickCounter;
	S.m_ChunkCount = Bot.m_PfChunkCount;
	S.m_TotalFreezeTicks = Bot.m_PfTotalFreezeTicks;
	S.m_BacktrackIdx = Bot.m_PfBacktrackIdx;
	for(int i = 0; i < PF_BACKTRACK_DEPTH; ++i)
		S.m_Backtrack[i] = Bot.m_PfBacktrack[i];
	S.m_VPath = Bot.m_PfVPath;
	S.m_VHookSegs = Bot.m_PfVHookSegs;
	S.m_VPathRender = Bot.m_PfVPathRender;
	S.m_VHookSegsRender = Bot.m_PfVHookSegsRender;
	S.m_VBranchSegsRender = Bot.m_PfVBranchSegsRender;
	S.m_FullInputs = Bot.m_PfFullInputs;
	S.m_FullInputsIdx = Bot.m_PfFullInputsIdx;
	S.m_LocalId = Bot.m_PfLocalID;
	S.m_UseExactGoal = Bot.m_PfUseExactGoal;
	S.m_ExactGoal = Bot.m_PfExactGoal;
	S.m_APathReady = Bot.m_PfAPathReady;
	S.m_AGoalIdx = Bot.m_PfAGoalIdx;
	S.m_AStarted = Bot.m_PfAStarted;
	S.m_AExpandCount = Bot.m_PfAExpandCount;
	S.m_ABestH = Bot.m_PfABestH.load();
	S.m_AHRoot = Bot.m_PfAHRoot;
	S.m_AGreedy = Bot.m_PfAGreedy;
	S.m_AGreedyTried = Bot.m_PfAGreedyTried;
	S.m_AChunkCur = Bot.m_PfAChunkCur;
	S.m_ARefined = Bot.m_PfARefined;
	S.m_ARefinedInputs = Bot.m_PfARefinedInputs;
	S.m_ARefinedPath = Bot.m_PfARefinedPath;
	S.m_ARefinedSegs = Bot.m_PfARefinedSegs;
	S.m_ARefinedEnd = Bot.m_PfARefinedEnd;
	S.m_ARefinedEndFreeze = Bot.m_PfARefinedEndFreeze;
	S.m_UiBestDone = Bot.m_PfUiBestDone;
	S.m_UiBestTotal = Bot.m_PfUiBestTotal;
	m_pSavedPfState = p;
	m_PfBorrowed = true;
}

void CTas::ResetPreparationPaths()
{
	m_vRealPrepPaths.clear();
}

bool CTas::RealDummyPos(int Slot, vec2 *pPos) const
{
	CGameClient *pGame = GameClient();
	if(!pPos || Slot <= 0 || Slot >= MAX_DUMMIES || !Client()->DummyConnected(Slot))
		return false;
	const int Id = pGame->m_aLocalIds[Slot];
	if(Id < 0 || Id >= MAX_CLIENTS || !pGame->m_aClients[Id].m_Active)
		return false;
	*pPos = pGame->m_aClients[Id].m_Predicted.m_Pos;
	return std::isfinite(pPos->x) && std::isfinite(pPos->y);
}

bool CTas::CollectPreparationTargets()
{
	m_vRealPrepTargets.clear();
	m_AutoSyncFailed = false;
	m_PrepRouteFailed = false;
	m_PrepFailedDummy = 0;
	for(int i = 0; i < MAX_DUMMIES; ++i)
		m_aRealToRecordedSlot[i] = i;

	// Recorded dummy start positions (tick 0 of the take).
	bool aSeen[MAX_DUMMIES] = {};
	std::vector<STasPrepDummy> vRecorded;
	const auto AddRecorded = [&](int Slot, vec2 Pos) {
		if(Slot <= 0 || Slot >= MAX_DUMMIES || aSeen[Slot] ||
			!std::isfinite(Pos.x) || !std::isfinite(Pos.y))
			return;
		aSeen[Slot] = true;
		STasPrepDummy T;
		T.m_RecordedSlot = Slot;
		T.m_Target = Pos;
		vRecorded.push_back(T);
	};
	for(const std::pair<int, vec2> &Start : m_vTasRecordedDummyStarts)
		AddRecorded(Start.first, Start.second);
	if(m_TakeFromFile)
		for(const STasStartLoadout &Start : m_vFileStartLoadouts)
			AddRecorded(Start.m_Slot, Start.m_Pos);
	for(const STasDummyState &Dummy : m_SavedStart.vDummies)
		AddRecorded(Dummy.m_Slot, Dummy.m_Core.m_Pos);
	if(vRecorded.empty())
		return false;

	CGameClient *pGame = GameClient();
	const bool bSync = g_Config.m_KxTasAutoSyncDummies != 0;
	std::vector<int> vReal;
	for(int Slot = 1; Slot < MAX_DUMMIES; ++Slot)
	{
		vec2 Pos;
		if(RealDummyPos(Slot, &Pos))
			vReal.push_back(Slot);
	}

	if(!bSync)
	{
		// Identity mapping: recorded slot n is driven into real slot n.
		if((int)vRecorded.size() > (int)vReal.size())
			m_AutoSyncFailed = true;
		for(const STasPrepDummy &T : vRecorded)
		{
			STasPrepDummy Copy = T;
			Copy.m_Slot = T.m_RecordedSlot;
			Copy.m_ClientId = pGame->m_aLocalIds[Copy.m_Slot];
			m_vRealPrepTargets.push_back(Copy);
		}
		return true;
	}

	// Auto Sync: global nearest one-to-one mapping real dummy ↔ recorded dummy.
	// Pairing the globally shortest distances first avoids the trap where a
	// greedy per-slot choice grabs the only near target and sends another dummy
	// across the map. An almost equal second-nearest target is ambiguous, so
	// weak/strong hook dummies cannot be told apart and the warning stays on.
	if(vReal.size() != vRecorded.size())
		m_AutoSyncFailed = true;
	struct SKxPair
	{
		float m_Dist;
		size_t m_Real;
		size_t m_Recorded;
	};
	std::vector<SKxPair> vPairs;
	vPairs.reserve(vReal.size() * vRecorded.size());
	for(size_t i = 0; i < vReal.size(); ++i)
	{
		vec2 RPos;
		RealDummyPos(vReal[i], &RPos);
		float BestD = 1e18f, SecondD = 1e18f;
		for(size_t j = 0; j < vRecorded.size(); ++j)
		{
			const float d = distance(RPos, vRecorded[j].m_Target);
			vPairs.push_back({d, i, j});
			if(d < BestD)
			{
				SecondD = BestD;
				BestD = d;
			}
			else if(d < SecondD)
			{
				SecondD = d;
			}
		}
		if(SecondD < BestD + 16.0f)
			m_AutoSyncFailed = true;
	}
	std::sort(vPairs.begin(), vPairs.end(), [](const SKxPair &A, const SKxPair &B) {
		if(A.m_Dist != B.m_Dist)
			return A.m_Dist < B.m_Dist;
		if(A.m_Real != B.m_Real)
			return A.m_Real < B.m_Real;
		return A.m_Recorded < B.m_Recorded;
	});
	std::vector<bool> aUsedReal(vReal.size(), false);
	std::vector<bool> aUsedRecorded(vRecorded.size(), false);
	for(const SKxPair &Pair : vPairs)
	{
		if(aUsedReal[Pair.m_Real] || aUsedRecorded[Pair.m_Recorded])
			continue;
		aUsedReal[Pair.m_Real] = true;
		aUsedRecorded[Pair.m_Recorded] = true;
		const int RealSlot = vReal[Pair.m_Real];
		const STasPrepDummy &Recorded = vRecorded[Pair.m_Recorded];
		STasPrepDummy T = Recorded;
		T.m_Slot = RealSlot;
		T.m_ClientId = pGame->m_aLocalIds[RealSlot];
		m_aRealToRecordedSlot[RealSlot] = Recorded.m_RecordedSlot;
		m_vRealPrepTargets.push_back(T);
	}
	for(size_t i = 0; i < vReal.size(); ++i)
		if(!aUsedReal[i])
			m_AutoSyncFailed = true;
	for(size_t j = 0; j < vRecorded.size(); ++j)
		if(!aUsedRecorded[j])
		{
			m_AutoSyncFailed = true;
			STasPrepDummy T = vRecorded[j];
			T.m_Slot = T.m_RecordedSlot;
			T.m_ClientId = pGame->m_aLocalIds[T.m_Slot];
			m_vRealPrepTargets.push_back(T);
		}
	return !m_vRealPrepTargets.empty();
}

bool CTas::TryBfsReachability(vec2 From, vec2 To)
{
	CBotNet &Bot = GameClient()->m_BotNet;
	if(!Bot.m_MapGridLoaded)
		Bot.LoadMapGrid();
	if(!Bot.m_MapGridLoaded || Bot.m_MapWidth <= 0 || Bot.m_MapHeight <= 0)
		return false;
	const int W = Bot.m_MapWidth;
	const int H = Bot.m_MapHeight;
	const int FX = std::clamp((int)(From.x / 32.0f), 0, W - 1);
	const int FY = std::clamp((int)(From.y / 32.0f), 0, H - 1);
	const int TX = std::clamp((int)(To.x / 32.0f), 0, W - 1);
	const int TY = std::clamp((int)(To.y / 32.0f), 0, H - 1);

	// Use the tab BFS field with the recorded start tile as its single source.
	// Paint the temporary tile directly into the grid instead of going through
	// the tile editor: PfAddCustomFinishTile() would delete a user's forbidden
	// tile at that spot, and every mutation here must be reverted afterwards.
	const size_t TileCount = (size_t)W * H;
	std::vector<unsigned char> SavedCustom;
	std::vector<unsigned char> SavedForbidden;
	if(Bot.m_pCustomFinishGrid)
		SavedCustom.assign(Bot.m_pCustomFinishGrid, Bot.m_pCustomFinishGrid + TileCount);
	if(Bot.m_pForbiddenGrid)
		SavedForbidden.assign(Bot.m_pForbiddenGrid, Bot.m_pForbiddenGrid + TileCount);
	const std::vector<vec2> SavedCustomTiles = Bot.m_PfCustomFinishTiles;
	const std::vector<vec2> SavedForbiddenTiles = Bot.m_PfForbiddenTiles;
	const std::vector<vec2> SavedFinishTiles = Bot.m_PfFinishTiles;
	std::vector<float> SavedFlow;
	if(Bot.m_PfFlowField)
		SavedFlow.assign(Bot.m_PfFlowField, Bot.m_PfFlowField + TileCount);
	std::vector<float> SavedScore;
	const int SavedScoreW = Bot.m_PfScoreFieldW;
	const int SavedScoreH = Bot.m_PfScoreFieldH;
	if(Bot.m_PfScoreField && SavedScoreW > 0 && SavedScoreH > 0)
		SavedScore.assign(Bot.m_PfScoreField, Bot.m_PfScoreField + (size_t)SavedScoreW * SavedScoreH);

	if(Bot.m_pCustomFinishGrid)
	{
		mem_zero(Bot.m_pCustomFinishGrid, TileCount);
		Bot.m_pCustomFinishGrid[TY * W + TX] = 1;
	}
	Bot.m_PfCustomFinishTiles.clear();
	Bot.m_PfCustomFinishTiles.push_back(vec2(TX * 32.0f + 16.0f, TY * 32.0f + 16.0f));
	delete[] Bot.m_PfFlowField;
	Bot.m_PfFlowField = nullptr;
	delete[] Bot.m_PfScoreField;
	Bot.m_PfScoreField = nullptr;
	Bot.m_PfScoreFieldW = Bot.m_PfScoreFieldH = 0;
	Bot.m_PfFinishTiles.clear();
	Bot.PfComputeFlowField(From);
	const bool bReachable = Bot.m_PfFlowField && Bot.m_PfFlowField[FY * W + FX] < 1e17f;

	delete[] Bot.m_PfFlowField;
	Bot.m_PfFlowField = nullptr;
	if(!SavedFlow.empty())
	{
		Bot.m_PfFlowField = new float[SavedFlow.size()];
		std::copy(SavedFlow.begin(), SavedFlow.end(), Bot.m_PfFlowField);
	}
	delete[] Bot.m_PfScoreField;
	Bot.m_PfScoreField = nullptr;
	Bot.m_PfScoreFieldW = Bot.m_PfScoreFieldH = 0;
	if(!SavedScore.empty())
	{
		Bot.m_PfScoreField = new float[SavedScore.size()];
		std::copy(SavedScore.begin(), SavedScore.end(), Bot.m_PfScoreField);
		Bot.m_PfScoreFieldW = SavedScoreW;
		Bot.m_PfScoreFieldH = SavedScoreH;
	}
	if(Bot.m_pCustomFinishGrid && SavedCustom.size() == TileCount)
		std::copy(SavedCustom.begin(), SavedCustom.end(), Bot.m_pCustomFinishGrid);
	if(Bot.m_pForbiddenGrid && SavedForbidden.size() == TileCount)
		std::copy(SavedForbidden.begin(), SavedForbidden.end(), Bot.m_pForbiddenGrid);
	Bot.m_PfCustomFinishTiles = SavedCustomTiles;
	Bot.m_PfForbiddenTiles = SavedForbiddenTiles;
	Bot.m_PfFinishTiles = SavedFinishTiles;
	return bReachable;
}

bool CTas::RequestPreparationRoute(STasPrepDummy &Target, vec2 From)
{
	CBotNet &Bot = GameClient()->m_BotNet;
	Target.m_RouteRequested = true;
	Target.m_Route = false;

	// 1. BFS (tab field). No reachability → no route at all; the dummy is
	//    skipped and the ClickGUI warning stays visible.
	if(!TryBfsReachability(From, Target.m_Target))
	{
		Target.m_RouteFailed = true;
		m_PrepRouteFailed = true;
		m_PrepFailedDummy = Target.m_Slot;
		dbg_msg("tas", "prep dummy %d: BFS found no route to (%.0f,%.0f)", Target.m_Slot, Target.m_Target.x, Target.m_Target.y);
		return false;
	}

	// 2. Tab A* at Very Low effort, rooted at this real dummy, with a ±1 unit
	//    goal test (PfAtGoal / PfEvalSequence wrappers in the tab solver).
	//    The recorded start must remain a finish source for the heuristic; set
	//    the tile directly so a user's forbidden tile is not deleted. Everything
	//    is reverted by RestoreTabPathfinder() when preparation ends.
	if(Bot.m_pCustomFinishGrid && Bot.m_MapWidth > 0 && Bot.m_MapHeight > 0)
	{
		const int TX = std::clamp((int)(Target.m_Target.x / 32.0f), 0, Bot.m_MapWidth - 1);
		const int TY = std::clamp((int)(Target.m_Target.y / 32.0f), 0, Bot.m_MapHeight - 1);
		const int Idx = TY * Bot.m_MapWidth + TX;
		const vec2 Center(TX * 32.0f + 16.0f, TY * 32.0f + 16.0f);
		if(!Bot.m_pCustomFinishGrid[Idx])
		{
			Bot.m_pCustomFinishGrid[Idx] = 1;
			Bot.m_PfCustomFinishTiles.push_back(Center);
		}
	}
	Bot.m_PfUseExactGoal = true;
	Bot.m_PfExactGoal = Target.m_Target;
	Bot.m_PfState = PF_STATE_RUNNING;
	const bool bStarted = Bot.PfResetRunForDummy(From, Target.m_ClientId);
	Bot.m_PfState = PF_STATE_RUNNING; // PfClearSearchState inside does not touch state
	if(!bStarted)
	{
		Target.m_RouteFailed = true;
		m_PrepRouteFailed = true;
		m_PrepFailedDummy = Target.m_Slot;
		dbg_msg("tas", "prep dummy %d: tab A* failed to start", Target.m_Slot);
		return false;
	}
	return true;
}

void CTas::CollectPreparationRoute(STasPrepDummy &Target)
{
	CBotNet &Bot = GameClient()->m_BotNet;
	if(Bot.m_PfThreadResult.load() != 1 || Bot.m_PfFullInputs.empty())
	{
		Target.m_RouteFailed = true;
		m_PrepRouteFailed = true;
		m_PrepFailedDummy = Target.m_Slot;
		dbg_msg("tas", "prep dummy %d: tab A* produced no route", Target.m_Slot);
		Target.m_Route = false;
		return;
	}
	Target.m_Inputs = Bot.m_PfFullInputs;
	Target.m_InputIdx = 0;
	Target.m_Fire = Target.m_NextWeapon = Target.m_PrevWeapon = 0;
	Target.m_Path = Bot.m_PfVPath;
	Target.m_Hooks.clear();
	for(const PfHookSeg &Seg : Bot.m_PfVHookSegs)
		Target.m_Hooks.push_back({Seg.teePos, Seg.hookPos});
	// The exact goal is the last waypoint (A* itself already aims at ±1 unit).
	if(Target.m_Path.empty() || distance(Target.m_Path.back(), Target.m_Target) > 1.0f)
		Target.m_Path.push_back(Target.m_Target);
	Target.m_Route = true;
	const size_t Index = &Target - m_vRealPrepTargets.data();
	if(Index < m_vRealPrepPaths.size())
		m_vRealPrepPaths[Index] = Target.m_Path;
	dbg_msg("tas", "prep dummy %d: tab route ready (%d ticks)", Target.m_Slot, (int)Target.m_Inputs.size());
}

void CTas::RefreshPreparationWarning()
{
	// v6.18 p.5: the exact warning is shown only when a dummy is ACTUALLY out
	// of position and either Auto Pathfind is off, Auto Sync could not map the
	// dummies unambiguously, or the tab Pathfinder could not build a route.
	bool bWrong = false;
	for(const STasPrepDummy &Target : m_vRealPrepTargets)
	{
		vec2 Pos;
		if(!RealDummyPos(Target.m_Slot, &Pos) || distance(Pos, Target.m_Target) > 1.0f)
		{
			bWrong = true;
			break;
		}
	}
	if(!bWrong)
	{
		m_aPreparationWarning[0] = '\0';
		return;
	}
	const bool bPath = g_Config.m_KxTasAutoPathfindDummies != 0;
	if(!bPath || m_AutoSyncFailed || m_PrepRouteFailed)
		str_copy(m_aPreparationWarning, "Warning: Your dummies are in the wrong positions.", sizeof(m_aPreparationWarning));
	else
		m_aPreparationWarning[0] = '\0';
}

void CTas::RestoreTabPathfinder()
{
	if(!m_PfBorrowed || !m_pSavedPfState)
		return;
	CBotNet &Bot = GameClient()->m_BotNet;
	STasSavedPfState &S = *static_cast<STasSavedPfState *>(m_pSavedPfState.get());
	const bool bSameMap = S.m_MapW == Bot.m_MapWidth && S.m_MapH == Bot.m_MapHeight;
	const size_t TileCount = (S.m_MapW > 0 && S.m_MapH > 0) ? (size_t)S.m_MapW * S.m_MapH : 0;

	Bot.PfThreadStop();
	Bot.PfClearSearchState();
	g_Config.m_KxPfEffort = S.m_Effort;
	g_Config.m_KxNetworkBuffer = S.m_NetworkBuffer;
	Bot.m_PfUseExactGoal = S.m_UseExactGoal;
	Bot.m_PfExactGoal = S.m_ExactGoal;

	if(bSameMap && Bot.m_pForbiddenGrid && S.m_ForbiddenGrid.size() == TileCount)
		std::copy(S.m_ForbiddenGrid.begin(), S.m_ForbiddenGrid.end(), Bot.m_pForbiddenGrid);
	if(bSameMap && Bot.m_pCustomFinishGrid && S.m_CustomFinishGrid.size() == TileCount)
		std::copy(S.m_CustomFinishGrid.begin(), S.m_CustomFinishGrid.end(), Bot.m_pCustomFinishGrid);
	Bot.m_PfForbiddenTiles = S.m_ForbiddenTiles;
	Bot.m_PfCustomFinishTiles = S.m_CustomFinishTiles;
	Bot.m_PfFinishTiles = S.m_FinishTiles;

	delete[] Bot.m_PfFlowField;
	Bot.m_PfFlowField = nullptr;
	if(!S.m_FlowField.empty() && TileCount == S.m_FlowField.size())
	{
		Bot.m_PfFlowField = new float[S.m_FlowField.size()];
		std::copy(S.m_FlowField.begin(), S.m_FlowField.end(), Bot.m_PfFlowField);
	}
	delete[] Bot.m_PfScoreField;
	Bot.m_PfScoreField = nullptr;
	Bot.m_PfScoreFieldW = 0;
	Bot.m_PfScoreFieldH = 0;
	if(!S.m_ScoreField.empty() && S.m_ScoreW > 0 && S.m_ScoreH > 0)
	{
		Bot.m_PfScoreField = new float[S.m_ScoreField.size()];
		std::copy(S.m_ScoreField.begin(), S.m_ScoreField.end(), Bot.m_PfScoreField);
		Bot.m_PfScoreFieldW = S.m_ScoreW;
		Bot.m_PfScoreFieldH = S.m_ScoreH;
	}

	Bot.m_PfStartPos = S.m_StartPos;
	Bot.m_PfCurPos = S.m_CurPos;
	Bot.m_PfCurVel = S.m_CurVel;
	Bot.m_PfCurHookState = S.m_CurHookState;
	Bot.m_PfCurHookPos = S.m_CurHookPos;
	Bot.m_PfCurHookDir = S.m_CurHookDir;
	Bot.m_PfCurHookTick = S.m_CurHookTick;
	Bot.m_PfCurFreezeTime = S.m_CurFreezeTime;
	Bot.m_PfCurJumped = S.m_CurJumped;
	Bot.m_PfTickCounter = S.m_TickCounter;
	Bot.m_PfChunkCount = S.m_ChunkCount;
	Bot.m_PfTotalFreezeTicks = S.m_TotalFreezeTicks;
	Bot.m_PfBacktrackIdx = S.m_BacktrackIdx;
	for(int i = 0; i < PF_BACKTRACK_DEPTH; ++i)
		Bot.m_PfBacktrack[i] = S.m_Backtrack[i];
	Bot.m_PfVPath = S.m_VPath;
	Bot.m_PfVHookSegs = S.m_VHookSegs;
	Bot.m_PfVPathRender = S.m_VPathRender;
	Bot.m_PfVHookSegsRender = S.m_VHookSegsRender;
	Bot.m_PfVBranchSegsRender = S.m_VBranchSegsRender;
	Bot.m_PfFullInputs = S.m_FullInputs;
	Bot.m_PfFullInputsIdx = S.m_FullInputsIdx;
	Bot.m_PfLocalID = S.m_LocalId;
	Bot.m_PfAPathReady = S.m_APathReady;
	Bot.m_PfAGoalIdx = S.m_AGoalIdx;
	Bot.m_PfAStarted = S.m_AStarted;
	Bot.m_PfAExpandCount = S.m_AExpandCount;
	Bot.m_PfABestH.store(S.m_ABestH);
	Bot.m_PfAHRoot = S.m_AHRoot;
	Bot.m_PfAGreedy = S.m_AGreedy;
	Bot.m_PfAGreedyTried = S.m_AGreedyTried;
	Bot.m_PfAChunkCur = S.m_AChunkCur;
	Bot.m_PfARefined = S.m_ARefined;
	Bot.m_PfARefinedInputs = S.m_ARefinedInputs;
	Bot.m_PfARefinedPath = S.m_ARefinedPath;
	Bot.m_PfARefinedSegs = S.m_ARefinedSegs;
	Bot.m_PfARefinedEnd = S.m_ARefinedEnd;
	Bot.m_PfARefinedEndFreeze = S.m_ARefinedEndFreeze;
	Bot.m_PfUiBestDone = S.m_UiBestDone;
	Bot.m_PfUiBestTotal = S.m_UiBestTotal;

	// A search that was running before preparation cannot be resumed from its
	// compressed node store; restart it from the original start so the tab is
	// live again instead of silently showing the last dummy's result.
	if(S.m_State == PF_STATE_RUNNING && S.m_LocalId >= 0)
	{
		Bot.m_PfUseExactGoal = S.m_UseExactGoal;
		Bot.m_PfExactGoal = S.m_ExactGoal;
		Bot.m_PfState = PF_STATE_RUNNING;
		(void)Bot.PfResetRunForDummy(S.m_StartPos, S.m_LocalId);
		Bot.m_PfState = PF_STATE_RUNNING;
	}
	else
	{
		Bot.m_PfState = S.m_State;
	}
	m_pSavedPfState.reset();
	m_PfBorrowed = false;
}

void CTas::CancelPreparation()
{
	RestoreTabPathfinder();
	if(!m_RealPreparing)
		return;
	m_RealPreparing = false;
	m_PrepDriving = false;
	m_PrepRouteIdx = 0;
	m_vRealPrepTargets.clear();
	ResetPreparationPaths();
	m_aPreparationWarning[0] = '\0';
	m_PrepTicks = 0;
}

void CTas::StartRealPlay()
{
	RestoreTabPathfinder();
	m_RealPreparing = false;
	m_PrepDriving = false;
	ResetPreparationPaths();
	// Keep the recorded targets so the warning can be re-evaluated while the
	// replay runs: it must disappear as soon as all dummies are in position.
	RefreshPreparationWarning();
	m_RealPlaying = true;
	m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
	m_RealFire = 0;
	m_RealNextWeapon = 0;
	m_RealPrevWeapon = 0;
	mem_zero(m_aRealDummyFire, sizeof(m_aRealDummyFire));
	mem_zero(m_aRealDummyNextWeapon, sizeof(m_aRealDummyNextWeapon));
	mem_zero(m_aRealDummyPrevWeapon, sizeof(m_aRealDummyPrevWeapon));
	dbg_msg("tas", "real replay started (%d ticks)", (int)m_vSaved.size());
}

void CTas::BeginRealPlayback()
{
	CancelPreparation();
	m_aPreparationWarning[0] = '\0';
	if(!CollectPreparationTargets())
	{
		StartRealPlay();
		return;
	}
	const bool bPath = g_Config.m_KxTasAutoPathfindDummies != 0;
	m_vRealPrepPaths.resize(m_vRealPrepTargets.size());

	bool bNeedsPrep = false;
	for(STasPrepDummy &Target : m_vRealPrepTargets)
	{
		vec2 Pos;
		if(!RealDummyPos(Target.m_Slot, &Pos))
		{
			Target.m_StableTicks = 0;
			Target.m_Done = false;
			bNeedsPrep = true;
		}
		else if(distance(Pos, Target.m_Target) <= 1.0f)
			Target.m_StableTicks++;
		else
		{
			Target.m_StableTicks = 0;
			bNeedsPrep = true;
		}
		if(RealDummyPos(Target.m_Slot, &Pos))
			Target.m_Done = Target.m_StableTicks > 10;
	}
	RefreshPreparationWarning();

	// Without Auto Pathfind the user positions dummies manually: start at once.
	// With Auto Pathfind, wrong positions trigger the yellow Preparation state.
	if(!bPath || !bNeedsPrep)
	{
		StartRealPlay();
		return;
	}

	SaveTabPathfinderState();
	g_Config.m_KxPfEffort = 0; // Very Low — the cheapest tab Pathfinder setting
	// Every staged input must leave on its own tick; a buffered active input
	// would replay the route at the wrong rate and never arrive.
	g_Config.m_KxNetworkBuffer = 0;

	m_RealPreparing = true;
	m_PrepDriving = false;
	m_PrepRouteIdx = 0;
	m_PrepTicks = 0;
	for(STasPrepDummy &Target : m_vRealPrepTargets)
		Target.m_RouteRequested = false;
	dbg_msg("tas", "real playback preparation: %d dummy target(s), tab pathfinder very low", (int)m_vRealPrepTargets.size());
}

void CTas::UpdatePreparation()
{
	if(!m_RealPreparing)
		return;
	++m_PrepTicks;
	CBotNet &Bot = GameClient()->m_BotNet;

	if(!m_PrepDriving)
	{
		// Route-building phase: the tab worker handles ONE dummy at a time —
		// its m_PfFullInputs / m_PfVPath / m_PfVHookSegs are global fields.
		if(m_PrepRouteIdx < (int)m_vRealPrepTargets.size())
		{
			STasPrepDummy &Target = m_vRealPrepTargets[m_PrepRouteIdx];
			if(Target.m_Done)
			{
				++m_PrepRouteIdx;
			}
			else if(!Target.m_RouteRequested)
			{
				vec2 From;
				if(!RealDummyPos(Target.m_Slot, &From) || Target.m_ClientId < 0)
				{
					Target.m_RouteRequested = true;
					Target.m_RouteFailed = true;
					m_PrepRouteFailed = true;
					m_PrepFailedDummy = Target.m_Slot;
					++m_PrepRouteIdx; // start the replay after the other dummies
				}
				else if(!RequestPreparationRoute(Target, From))
				{
					++m_PrepRouteIdx; // BFS failed — no route to build
				}
			}
			else if(Bot.m_PfState == PF_STATE_FINISHED)
			{
				// Result 1 = exact goal; -1 = search ended without one. Both
				// end this route — a failed one is skipped by the driving loop.
				CollectPreparationRoute(Target);
				++m_PrepRouteIdx;
			}
		}
		else
		{
			m_PrepDriving = true;
			for(STasPrepDummy &Target : m_vRealPrepTargets)
			{
				Target.m_InputIdx = 0;
				Target.m_Fire = Target.m_NextWeapon = Target.m_PrevWeapon = 0;
			}
		}
		RefreshPreparationWarning();
		if(m_PrepTicks > TAS_PREP_TIMEOUT_TICKS)
			StartRealPlay();
		return;
	}

	// Driving phase: tab inputs are consumed in ApplyTasPreparationInput; here
	// we only check unit-exact arrival and finish the preparation.
	bool bAllDone = true;
	for(STasPrepDummy &Target : m_vRealPrepTargets)
	{
		// A dummy with no route (BFS/A* failure or already in position) is not
		// waited for — replay starts once the routed dummies arrive.
		if(!Target.m_Route || Target.m_RouteFailed)
		{
			Target.m_Done = true;
			continue;
		}
		vec2 Pos;
		if(!RealDummyPos(Target.m_Slot, &Pos))
		{
			Target.m_StableTicks = 0;
		}
		else if(distance(Pos, Target.m_Target) <= 1.0f)
			Target.m_StableTicks++;
		else
			Target.m_StableTicks = 0;
		Target.m_Done = Target.m_StableTicks > 10;
		if(!Target.m_Done)
			bAllDone = false;
	}
	RefreshPreparationWarning();
	if(bAllDone || m_PrepTicks > TAS_PREP_TIMEOUT_TICKS)
		StartRealPlay();
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
			m_vSaved = std::move(m_vRecord);
			m_SavedStart = std::move(m_RecordStart);
		}
		if(m_vSaved.empty())
			return;
		if(m_HasTakePgStart)
		{
			m_PgPractice = m_TakePgPractice;
			m_PgInvincible = m_TakePgInvincible;
			m_PgSuper = m_TakePgSuper;
			m_PgPaused = m_TakePgPaused;
			m_PgSpec = m_TakePgSpec;
			m_PgTuningDirty = m_TakePgTuningDirty;
			m_PgTuningInit = true;
			for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
				m_aPgTuning[Zone] = m_aTakePgTuning[Zone];
			m_vPgChat = m_vTakeChat;
			m_vPgRcon = m_vTakeRcon;
			GameClient()->m_Chat.TasRefreshVirtualLines();
			if(m_PgVirtualRcon)
			{
				GameClient()->m_GameConsole.ClearRemoteBacklog();
				const int Start = std::max(0, (int)m_vPgRcon.size() - 200);
				for(int i = Start; i < (int)m_vPgRcon.size(); ++i)
					GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, m_vPgRcon[i].c_str());
			}
		}
		m_Playing = true;
		m_Desynced = false;
		// Restart from the state the recording started at, so the recorded
		// inputs replay from exactly the state they were recorded in (the
		// fake history restarts; the saved recording stays intact).
		if(m_TakeFromFile)
		{
			const std::vector<SSpawnedEntSnapshot> vFileSpawned = m_SavedStart.vSpawnedEnts;
			const int FileStartTick = m_SavedStart.Tick;
			if(m_HasTakePgStart)
			{
				SpawnerClearLive();
				m_vSpawnerRecords = m_vTakeSpawner;
				for(const STasSpawnerRecord &Record : m_vSpawnerRecords)
					if(m_PickedFormat != 0 || m_PickedVersion < 3 || Record.m_Mode == TAS_SPAWN_MODE_TILE)
						SpawnerSpawnAt(Record.m_Settings, Record.m_Pos, Record.m_Tick, false);
			}
			if(m_PickedFormat == 1 || m_PickedVersion >= 3)
				std::fill(std::begin(m_aPgDummyConnected), std::end(m_aPgDummyConnected), false);
			for(const STasDummyFrame &Frame : m_vSaved.front().vDummyInputs)
			{
				m_PgDummyLimit = std::max(m_PgDummyLimit, Frame.m_Slot + 1);
				m_aPgDummyConnected[Frame.m_Slot] = true;
			}
			for(const STasStartLoadout &Start : m_vFileStartLoadouts)
				if(Start.m_Slot > 0)
				{
					m_PgDummyLimit = std::max(m_PgDummyLimit, Start.m_Slot + 1);
					m_aPgDummyConnected[Start.m_Slot] = true;
				}
			SyncPgDummies();
			for(const STasStartLoadout &Start : m_vFileStartLoadouts)
				if(CCharacter *pTee = PlaygroundCharacter(Start.m_Slot))
				{
					for(int W = 0; W < NUM_WEAPONS; ++W)
						if(Start.m_WeaponMask >= 0)
							pTee->SetWeaponGot(W, (Start.m_WeaponMask & (1 << W)) != 0);
					pTee->SetWeaponGot(Start.m_Weapon, true);
					pTee->SetActiveWeapon(Start.m_Weapon);
					pTee->SetReloadTimer(0);
					pTee->SetInput(&Start.m_PreviousInput);
					CCharacterCore Core = pTee->GetCore();
					Core.m_Pos = Start.m_Pos;
					Core.m_Vel = Start.m_Vel;
					pTee->SetCore(Core);
					pTee->m_Pos = pTee->m_PrevPos = pTee->m_PrevPrevPos = Start.m_Pos;
				}
			m_SavedStart = CaptureState();
			if(m_PickedFormat == 0 && m_PickedVersion >= 3)
			{
				m_SavedStart.vSpawnedEnts = vFileSpawned;
				m_SavedStart.Tick = FileStartTick;
			}
		}
		if(!m_TakeFromFile)
		{
			SpawnerClearLive();
			for(const STasSpawnerRecord &Record : m_vTakeSpawner)
				if(Record.m_Mode == TAS_SPAWN_MODE_TILE)
					SpawnerSpawnAt(Record.m_Settings, Record.m_Pos, Record.m_Tick, false);
		}
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
	if(m_RealPreparing)
	{
		CancelPreparation();
		dbg_msg("tas", "real playback preparation cancelled");
		return;
	}
	if(m_RealPlaying)
	{
		CancelPreparation();
	m_RealPlaying = false;
		dbg_msg("tas", "real replay stopped at %d/%d", m_RealIdx, (int)m_vSaved.size());
		return;
	}
	if(m_Recording)
	{
		m_Recording = false;
		m_vSaved = std::move(m_vRecord);
		m_SavedStart = std::move(m_RecordStart);
	}
	if(m_vSaved.empty())
		return;
	// Stage the real dummies first (yellow Preparation), then start the replay.
	BeginRealPlayback();
}

void CTas::PasteRun(const std::vector<CNetObj_PlayerInput> &vInputs, const std::vector<vec2> &vPath, const std::vector<std::pair<vec2, vec2>> &vHookSegs)
{
	if(vInputs.empty())
		return;
	m_HasTakePgStart = false;
	m_vTakeSpawner.clear();
	m_vTakeSpawnerEvents.clear();
	m_vTakeChat.clear();
	m_vTakeRcon.clear();
	if(m_Playing)
	{
		m_Playing = false;
		RebaseCounters();
	}
	if(m_Recording)
		m_Recording = false;
	m_SavedStart = CTasState{};
	m_vFileStartLoadouts.clear();
	m_vTasRecordedDummyStarts.clear();
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
		// Stop: transfer the record allocation instead of keeping a second copy.
		m_Recording = false;
		m_vSaved = std::move(m_vRecord);
		m_SavedStart = std::move(m_RecordStart);
		return;
	}
	// Start: replace both old takes; only the active recording owns tick data.
	// A new recording must not keep the previously picked file as its Save
	// target or inherit its output format and metadata.
	UnpickFile();
	m_vFileStartLoadouts.clear();
	m_vTasRecordedDummyStarts.clear();
	m_vTakeSpawner = m_vSpawnerRecords;
	m_vTakeSpawnerEvents.clear();
	m_vTakeTimelineEvents.clear();
	m_TasTimelineNextOrder = 0;
	m_vTakeChat = m_vPgChat;
	m_vTakeRcon = m_vPgRcon;
	m_TakePgPractice = m_PgPractice;
	m_TakePgInvincible = m_PgInvincible;
	m_TakePgSuper = m_PgSuper;
	m_TakePgPaused = m_PgPaused;
	m_TakePgSpec = m_PgSpec;
	m_TakePgTuningDirty = m_PgTuningDirty;
	m_HasTakePgStart = true;
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
		m_aTakePgTuning[Zone] = m_aPgTuning[Zone];
	m_Recording = true;
	m_Playing = false;
	m_Desynced = false;
	std::vector<CTasTick>().swap(m_vRecord);
	std::vector<CTasTick>().swap(m_vSaved);
	m_SavedStart = CTasState{};
	m_UsedFlags = 0;
	mem_zero(m_aFinishTicks, sizeof(m_aFinishTicks));
	str_timestamp_format(m_aRecordCreated, sizeof(m_aRecordCreated), "%H:%M | %d.%m.%Y");
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
		DoStep(T.Input, &T);
		if(m_PlayIdx + 1 == (int)m_vSaved.size() && (!m_TakeFromFile || (m_PickedFormat == 0 && m_PickedVersion >= 3)))
		{
			ApplyTakeTimelineEvents(m_PlaygroundTick);
			m_vPlaygroundHistory.back() = CaptureState();
		}
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
	if(m_Recording && m_PlaygroundTick <= m_RecordStart.Tick)
		return;
	m_vPlaygroundInputs.pop_back();
	RestoreState(m_vPlaygroundHistory[m_vPlaygroundHistory.size() - 2]);
	GameClient()->m_AvoidFreeze.ResetPlaygroundPlan();
	m_vPlaygroundHistory.pop_back();
	if(m_Playing && m_PlayIdx > 0)
		m_PlayIdx--;
	// While recording, the rewound tick is dropped from the record buffer
	// too (frame-by-frame recording fixes mistakes by rewinding).
	if(m_Recording && !m_vRecord.empty())
		m_vRecord.pop_back();
	if(m_Recording)
	{
		const int StartTick = m_RecordStart.Tick;
		m_vTakeSpawnerEvents.erase(std::remove_if(m_vTakeSpawnerEvents.begin(), m_vTakeSpawnerEvents.end(),
			[this, StartTick](const STasSpawnerEvent &Event) {
				return Event.m_Record.m_Tick >= StartTick && Event.m_Record.m_Tick >= m_PlaygroundTick;
			}), m_vTakeSpawnerEvents.end());
		m_vTakeTimelineEvents.erase(std::remove_if(m_vTakeTimelineEvents.begin(), m_vTakeTimelineEvents.end(),
			[this, StartTick](const STasTimelineEvent &Event) {
				return Event.m_Tick >= StartTick && Event.m_Tick >= m_PlaygroundTick;
			}), m_vTakeTimelineEvents.end());
	}
	if(m_Recording || (m_Playing && (!m_TakeFromFile || (m_PickedFormat == 0 && m_PickedVersion >= 3))))
		SpawnerRestoreTakeTiles(m_PlaygroundTick);
	if(m_Recording || (m_Playing && (!m_TakeFromFile || (m_PickedFormat == 0 && m_PickedVersion >= 3))))
	{
		RebuildTakeTimelineState(m_PlaygroundTick);
		RestoreState(m_vPlaygroundHistory.back());
	}
	RebaseCounters();
}

bool CTas::IsDead()
{
	return !m_Joined || PlaygroundCharacter(m_PgActiveDummy) == nullptr;
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
	CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
	if(!pChar)
		return false;
	*pCore = pChar->GetCore();
	// Keep HUD capabilities in lockstep with the local team state. Solo tiles
	// update both, but the team core is authoritative for collision/hooking.
	pCore->m_Solo = m_Playground.Teams()->GetSolo(pChar->GetCid());
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

bool CTas::PlaygroundSolo() const
{
	const CCharacter *pChar = PlaygroundCharacter(m_PgActiveDummy);
	return m_Joined && pChar && pChar->Core()->m_Solo;
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

bool CTas::FutureSavedDummyInput(int Idx, CNetObj_PlayerInput *pOut) const
{
	if(Idx < 0 || Idx >= (int)m_vSaved.size())
		return false;
	*pOut = m_vSaved[Idx].DummyInput;
	return true;
}

bool CTas::FutureSavedSlotInput(int Idx, int Slot, CNetObj_PlayerInput *pOut) const
{
	if(Slot == 0)
		return FutureSavedInput(Idx, pOut);
	if(Idx < 0 || Idx >= (int)m_vSaved.size())
		return false;
	const CTasTick &Tick = m_vSaved[Idx];
	for(const STasDummyFrame &Frame : Tick.vDummyInputs)
		if(Frame.m_Slot == Slot)
		{
			*pOut = Frame.m_Input;
			return true;
		}
	return Slot == 1 && Tick.vDummyInputs.empty() && FutureSavedDummyInput(Idx, pOut);
}

CNetObj_PlayerInput CTas::PlaygroundKeyboardInput()
{
	const auto &Controls = GameClient()->m_Controls;
	CNetObj_PlayerInput Input = Controls.m_aInputData[0];
	const vec2 Aim = Controls.m_aMousePos[0];
	Input.m_TargetX = round_to_int(Aim.x);
	Input.m_TargetY = round_to_int(Aim.y);
	Input.m_Direction = Controls.m_aInputDirectionRight[0] - Controls.m_aInputDirectionLeft[0];
	Input.m_PlayerFlags = PLAYERFLAG_PLAYING;
	if(GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive() || g_Config.m_ClClickGui)
	{
		Input.m_Direction = Input.m_Jump = Input.m_Hook = Input.m_WantedWeapon = 0;
		Input.m_Fire &= ~1;
	}
	return Input;
}

CNetObj_PlayerInput CTas::LiveInput()
{
	CNetObj_PlayerInput Input = PlaygroundKeyboardInput();
	const bool ControlMain = m_PgActiveDummy == 0;
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
	const int FakeFire = ControlMain && (Input.m_Fire & 1) ? (LastFire | 1) : ((LastFire + 1) & ~1);
	Input.m_Fire = FakeFire & INPUT_STATE_MASK;

	// Weapon switch counters have no held state: replay the real edge deltas.
	if(ControlMain)
	{
		m_FakeNextWeapon += Input.m_NextWeapon - m_LastLiveNextWeapon;
		m_FakePrevWeapon += Input.m_PrevWeapon - m_LastLivePrevWeapon;
	}
	m_LastLiveNextWeapon = Input.m_NextWeapon;
	m_LastLivePrevWeapon = Input.m_PrevWeapon;
	Input.m_NextWeapon = m_FakeNextWeapon & INPUT_STATE_MASK;
	Input.m_PrevWeapon = m_FakePrevWeapon & INPUT_STATE_MASK;
	if(!ControlMain)
	{
		const auto &Previous = m_Playground.GetCharacterById(m_LocalId)->GetLatestInput();
		Input.m_TargetX = Previous.m_TargetX;
		Input.m_TargetY = Previous.m_TargetY;
		Input.m_Direction = 0;
		Input.m_Jump = 0;
		Input.m_Hook = 0;
		Input.m_WantedWeapon = 0;
	}
	return Input;
}

// Re-anchor the weapon counters after a rewind/join: the next LiveInput() must
// start from the last consumed input, so undone switches never replay. Fire
// needs no anchor — it is rebuilt from the physical held bit every step.
void CTas::RebaseCounters()
{
	const CNetObj_PlayerInput &Cur = GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
	m_PgLastKeyboardInput = Cur;
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
		CancelPreparation();
	m_RealPlaying = false;
		dbg_msg("tas", "real replay finished (%d ticks)", (int)m_vSaved.size());
		return false;
	}
	const CTasTick &T = m_vSaved[m_RealIdx];
	if(m_RealIdx == 0)
	{
		m_RealFire = pInput->m_Fire;
		m_RealNextWeapon = pInput->m_NextWeapon;
		m_RealPrevWeapon = pInput->m_PrevWeapon;
	}
	else
	{
		m_RealFire += T.Input.m_Fire - m_vSaved[m_RealIdx - 1].Input.m_Fire;
		m_RealNextWeapon += T.Input.m_NextWeapon - m_vSaved[m_RealIdx - 1].Input.m_NextWeapon;
		m_RealPrevWeapon += T.Input.m_PrevWeapon - m_vSaved[m_RealIdx - 1].Input.m_PrevWeapon;
	}
	pInput->m_Direction = T.Input.m_Direction;
	pInput->m_Jump = T.Input.m_Jump;
	pInput->m_Hook = T.Input.m_Hook;
	pInput->m_TargetX = T.Input.m_TargetX;
	pInput->m_TargetY = T.Input.m_TargetY;
	pInput->m_WantedWeapon = T.Input.m_WantedWeapon;
	pInput->m_Fire = m_RealFire & INPUT_STATE_MASK;
	pInput->m_NextWeapon = m_RealNextWeapon & INPUT_STATE_MASK;
	pInput->m_PrevWeapon = m_RealPrevWeapon & INPUT_STATE_MASK;
	m_RealIdx++;
	return true;
}

bool CTas::ApplyTasPreparationInput(int Conn, CNetObj_PlayerInput *pInput)
{
	if(!m_RealPreparing || !m_PrepDriving || Conn <= 0 || Conn >= MAX_DUMMIES || !pInput)
		return false;
	for(STasPrepDummy &Target : m_vRealPrepTargets)
	{
		if(Target.m_Slot != Conn || !Target.m_Route || Target.m_Inputs.empty())
			continue;
		if(Target.m_InputIdx >= Target.m_Inputs.size())
		{
			CNetObj_PlayerInput In = Target.m_Inputs.back();
			In.m_Direction = In.m_Jump = In.m_Hook = In.m_WantedWeapon = 0;
			if(Target.m_Fire & 1)
				Target.m_Fire++;
			In.m_Fire = Target.m_Fire & INPUT_STATE_MASK;
			In.m_NextWeapon = Target.m_NextWeapon & INPUT_STATE_MASK;
			In.m_PrevWeapon = Target.m_PrevWeapon & INPUT_STATE_MASK;
			In.m_PlayerFlags = PLAYERFLAG_PLAYING;
			*pInput = In;
			return true;
		}
		const size_t Idx = Target.m_InputIdx;
		CNetObj_PlayerInput In = Target.m_Inputs[Idx];
		In.m_PlayerFlags = PLAYERFLAG_PLAYING;
		if(Target.m_InputIdx == 0)
		{
			Target.m_Fire = pInput->m_Fire;
			Target.m_NextWeapon = pInput->m_NextWeapon;
			Target.m_PrevWeapon = pInput->m_PrevWeapon;
		}
		else
		{
			const CNetObj_PlayerInput &Prev = Target.m_Inputs[Target.m_InputIdx - 1];
			Target.m_Fire += In.m_Fire - Prev.m_Fire;
			Target.m_NextWeapon += In.m_NextWeapon - Prev.m_NextWeapon;
			Target.m_PrevWeapon += In.m_PrevWeapon - Prev.m_PrevWeapon;
		}
		In.m_Fire = Target.m_Fire & INPUT_STATE_MASK;
		In.m_NextWeapon = Target.m_NextWeapon & INPUT_STATE_MASK;
		In.m_PrevWeapon = Target.m_PrevWeapon & INPUT_STATE_MASK;
		if(Target.m_InputIdx < Target.m_Inputs.size())
			++Target.m_InputIdx;
		*pInput = In;
		return true;
	}
	return false;
}

bool CTas::ApplyTasRealDummyInput(int Conn, CNetObj_PlayerInput *pInput)
{
	if(!pInput || Conn < 0 || Conn >= MAX_DUMMIES)
		return false;
	// Preparation has its own tab-Pathfinder input stream.
	if(m_RealPreparing)
		return ApplyTasPreparationInput(Conn, pInput);
	if(!m_RealPlaying)
		return false;
	// Auto Sync maps a real connection onto the recorded slot it replaced.
	const int RecordedSlot = Conn > 0 ? m_aRealToRecordedSlot[Conn] : 0;
	int &Idx = m_aRealDummyIdx[Conn];
	if(Idx >= (int)m_vSaved.size())
	{
		// Main is the lifetime owner of the real replay. Without stopping
		// here, its exhausted V3 input stream falls through to SnapInput,
		// whose legacy Main path starts replaying the take from tick zero.
		if(Conn == 0)
		{
			CancelPreparation();
			m_RealPlaying = false;
			dbg_msg("tas", "real replay finished (%d ticks)", (int)m_vSaved.size());
		}
		return false;
	}
	CNetObj_PlayerInput Recorded{};
	if(!FutureSavedSlotInput(Idx, RecordedSlot, &Recorded))
	{
		Idx++;
		return false;
	}
	int &Fire = m_aRealDummyFire[Conn];
	int &Next = m_aRealDummyNextWeapon[Conn];
	int &Prev = m_aRealDummyPrevWeapon[Conn];
	CNetObj_PlayerInput Previous{};
	if(Idx == 0 || !FutureSavedSlotInput(Idx - 1, RecordedSlot, &Previous))
	{
		Fire = pInput->m_Fire;
		Next = pInput->m_NextWeapon;
		Prev = pInput->m_PrevWeapon;
	}
	else
	{
		Fire += Recorded.m_Fire - Previous.m_Fire;
		Next += Recorded.m_NextWeapon - Previous.m_NextWeapon;
		Prev += Recorded.m_PrevWeapon - Previous.m_PrevWeapon;
	}
	*pInput = Recorded;
	pInput->m_Fire = Fire & INPUT_STATE_MASK;
	pInput->m_NextWeapon = Next & INPUT_STATE_MASK;
	pInput->m_PrevWeapon = Prev & INPUT_STATE_MASK;
	Idx++;
	if(Conn == 0)
		m_RealIdx = Idx;
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
		Idx = std::max(0, m_aRealDummyIdx[0] - 1);
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
	State.AutoFireCounter = m_PgAutoFireCounter;
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_CHARACTER); pEnt; pEnt = pEnt->TypeNext())
		State.vCharacterOrder.push_back(pEnt->GetId());
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		if(CCharacter *pDummy = PlaygroundCharacter(D))
		{
			STasDummyState &Dummy = State.vDummies.emplace_back();
			Dummy.m_Slot = D;
			Dummy.m_Id = pDummy->GetCid();
			Dummy.m_Core = pDummy->GetCore();
			pDummy->SaveFullState(&Dummy.m_Character);
		}
	}
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
	mem_copy(State.aFinishTicks, m_aFinishTicks, sizeof(State.aFinishTicks));
	State.UsedFlags = m_UsedFlags;
	State.Tps = m_Tps;
	State.Paused = m_Paused;
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
	m_PgAutoFireCounter = State.AutoFireCounter;
	mem_copy(m_aFinishTicks, State.aFinishTicks, sizeof(m_aFinishTicks));
	m_UsedFlags = State.UsedFlags;
	m_Tps = std::clamp(State.Tps, 1.0f, 50.0f);
	m_Paused = State.Paused;
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
		CharData.m_HookedPlayer = -1;
		CharData.m_HookX = (int)State.Core.m_HookPos.x;
		CharData.m_HookY = (int)State.Core.m_HookPos.y;
		CharData.m_HookDx = (int)(State.Core.m_HookDir.x * 256.0f);
		CharData.m_HookDy = (int)(State.Core.m_HookDir.y * 256.0f);
		CharData.m_Weapon = State.Core.m_ActiveWeapon;
		pChar = new CCharacter(&m_Playground, m_LocalId, &CharData);
		pChar->m_IsLocal = true;
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
	m_aPgDummyInputs[0] = State.LatestInput;

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
	DestroyPgDummies();
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		m_aPgDummyConnected[D] = false;
		m_aPgDummyInputs[D] = {};
	}
	for(const STasDummyState &Dummy : State.vDummies)
	{
		const int D = Dummy.m_Slot;
		if(D >= m_PgDummyLimit)
			continue;
		CNetObj_Character Net{};
		Net.m_X = round_to_int(Dummy.m_Core.m_Pos.x);
		Net.m_Y = round_to_int(Dummy.m_Core.m_Pos.y);
		Net.m_HookState = HOOK_IDLE;
		Net.m_HookedPlayer = -1;
		Net.m_Weapon = std::clamp(Dummy.m_Core.m_ActiveWeapon, 0, NUM_WEAPONS - 1);
		CCharacter *pDummy = new CCharacter(&m_Playground, Dummy.m_Id, &Net);
		pDummy->LoadFullState(Dummy.m_Character);
		pDummy->SetCore(Dummy.m_Core);
		pDummy->m_IsLocal = true;
		m_Playground.Teams()->Team(Dummy.m_Id, m_Playground.Teams()->Team(m_LocalId));
		pDummy->SetSolo(Dummy.m_Core.m_Solo);
		m_Playground.InsertEntity(pDummy, true);
		m_aPgDummyConnected[D] = true;
		m_aPgDummyIds[D] = Dummy.m_Id;
		m_apPgDummies[D] = pDummy;
		m_aPgDummyInputs[D] = pDummy->GetLatestInput();
	}
	if(!PlaygroundCharacter(m_PgActiveDummy))
		SetPgActiveDummy(0);
	int SpawnerId = 0;
	for(const SSpawnedEntSnapshot &Snap : State.vSpawnedEnts)
	{
		if(Snap.m_Entity == TAS_SPAWN_ENT_PROJECTILE)
		{
			CProjectile *pProj = new CProjectile(&m_Playground, SpawnerId++, &Snap.ProjData);
			if(Snap.m_Inserted)
				m_Playground.InsertEntity(pProj);
			SSpawnedEnt E;
			E.m_Entity = Snap.m_Entity;
			E.m_Tick = Snap.m_Tick;
			E.m_Pos = Snap.m_Pos;
			E.m_Inserted = Snap.m_Inserted;
			E.m_Settings = Snap.m_Settings;
			E.m_RenderPrevPos = E.m_RenderCurPos = Snap.m_Pos;
			E.m_pEnt = pProj;
			m_vSpawnedEnts.push_back(E);
		}
		else if(Snap.m_Entity == TAS_SPAWN_ENT_LASER)
		{
			CLaser *pLaser = new CLaser(&m_Playground, SpawnerId++, &Snap.Laser.Data);
			if(Snap.m_Inserted)
				m_Playground.InsertEntity(pLaser);
			pLaser->Restore(Snap.Laser.Data, Snap.Laser.Dir, Snap.Laser.PrevPos, Snap.Laser.Energy, Snap.Laser.Bounces, Snap.Laser.Owner, Snap.Laser.ZeroEnergyBounce);
			SSpawnedEnt E;
			E.m_Entity = Snap.m_Entity;
			E.m_Tick = Snap.m_Tick;
			E.m_Pos = Snap.m_Pos;
			E.m_Inserted = Snap.m_Inserted;
			E.m_Settings = Snap.m_Settings;
			E.m_RenderPrevPos = E.m_RenderCurPos = Snap.m_Pos;
			E.m_pEnt = pLaser;
			m_vSpawnedEnts.push_back(E);
		}
		else
		{
			const size_t Before = m_vSpawnedEnts.size();
			STasSpawnerSettings Settings = Snap.m_Settings;
			if(Snap.m_Entity == TAS_SPAWN_ENT_TEE)
				Settings.m_Id = Snap.m_Id;
			SpawnerSpawnAt(Settings, Snap.m_Pos, Snap.m_Tick, false);
			if(m_vSpawnedEnts.size() != Before + 1)
				continue;
			SSpawnedEnt &E = m_vSpawnedEnts.back();
			E.m_Settings = Snap.m_Settings;
			E.m_Inserted = Snap.m_Inserted;
			E.m_RenderPrevPos = E.m_RenderCurPos = Snap.m_Pos;
			if(Snap.m_Entity == TAS_SPAWN_ENT_TEE && E.m_pEnt)
			{
				CCharacter *pC = static_cast<CCharacter *>(E.m_pEnt);
				if(Snap.m_NetCoreOnly)
				{
					CNetObj_CharacterCore Net{};
					Snap.Core.Write(&Net);
					CCharacterCore Core = pC->GetCore();
					Core.Read(&Net);
					Core.m_ActiveWeapon = Snap.Core.m_ActiveWeapon;
					pC->SetCore(Core);
				}
				else
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
	// The server uses character-list order for weak/strong hook processing.
	// Preserve that order even when a rewind has to recreate a dead tee.
	for(int Id : State.vCharacterOrder)
	{
		if(CCharacter *pTee = m_Playground.GetCharacterById(Id))
		{
			m_Playground.RemoveEntity(pTee);
			m_Playground.InsertEntity(pTee, true);
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

bool CTas::PgDummyHammerCanHit(int D, int TargetSlot, vec2 *pAim)
{
	CCharacter *pDummy = PlaygroundCharacter(D);
	CCharacter *pActive = PlaygroundCharacter(TargetSlot);
	if(!pDummy || !pActive)
		return false;
	// FireWeapon: a live-frozen tee can never fire; a deep-frozen tee cannot
	// fire the hammer unless sv_deepfly is enabled.
	if(pDummy->Core()->m_LiveFrozen)
		return false;
	if(pDummy->Core()->m_DeepFrozen && !g_Config.m_SvDeepfly)
		return false;
	const int DummyId = pDummy->GetCid();
	const int ActiveId = pActive->GetCid();
	if(DummyId == ActiveId)
		return false;
	// A regular freeze has a one-tick window (the hammer hit unfreezes the
	// target in the same input phase). Let the engine answer it: run the same
	// one-tick prediction OnPredict uses on an isolated copy of the sandbox.
	if(pDummy->m_FreezeTime > 0)
	{
		const vec2 AimF = pActive->GetPos() - pDummy->GetPos();
		CGameWorld FutureWorld;
		FutureWorld.CopyWorldIsolated(&m_Playground);
		CCharacter *pDummyF = FutureWorld.GetCharacterById(DummyId);
		CCharacter *pActiveF = FutureWorld.GetCharacterById(ActiveId);
		if(!pDummyF || !pActiveF)
			return false;
		CNetObj_PlayerInput DummyIn = pDummyF->GetLatestInput();
		DummyIn.m_Fire = (DummyIn.m_Fire + 1) | 1;
		DummyIn.m_WantedWeapon = WEAPON_HAMMER + 1;
		DummyIn.m_TargetX = (int)AimF.x;
		DummyIn.m_TargetY = (int)AimF.y;
		CNetObj_PlayerInput ActiveIn = pActiveF->Core()->m_Input;
		const int ReloadBefore = pDummyF->GetReloadTimer();
		if(ActiveId < DummyId)
		{
			pActiveF->OnDirectInput(&ActiveIn);
			pDummyF->OnDirectInput(&DummyIn);
		}
		else
		{
			pDummyF->OnDirectInput(&DummyIn);
			pActiveF->OnDirectInput(&ActiveIn);
		}
		FutureWorld.m_GameTick = FutureWorld.GameTick() + 1;
		FutureWorld.Tick();
		CCharacter *pDummyAfter = FutureWorld.GetCharacterById(DummyId);
		if(!pDummyAfter || pDummyAfter->GetReloadTimer() <= ReloadBefore)
			return false;
		if(pAim)
			*pAim = AimF;
		return true;
	}
	if(pDummy->GetReloadTimer() != 0)
		return false;
	if(!pDummy->GetWeaponGot(WEAPON_HAMMER) || pDummy->GetWeaponAmmo(WEAPON_HAMMER) == 0)
		return false;
	if(pDummy->GetActiveWeapon() != WEAPON_HAMMER)
		return false;
	if(pDummy->HammerHitDisabled())
		return false;
	if(!pDummy->CanCollide(ActiveId))
		return false;
	const vec2 DummyPos = pDummy->GetPos();
	const vec2 ActivePos = pActive->GetPos();
	const vec2 AimF = ActivePos - DummyPos;
	const vec2 AimI((float)(int)AimF.x, (float)(int)AimF.y);
	const float AimLen = length(AimI);
	const vec2 Dir = AimLen > 0.001f ? AimI / AimLen : vec2(0.0f, -1.0f);
	const vec2 ProjStartPos = DummyPos + Dir * pDummy->GetProximityRadius() * 0.75f;
	const float HitRadius = pDummy->GetProximityRadius() * 0.5f + pActive->GetProximityRadius();
	if(distance(ActivePos, ProjStartPos) >= HitRadius)
		return false;
	if(pAim)
		*pAim = AimF;
	return true;
}

void CTas::PgRecordInputHistory(const CNetObj_PlayerInput &Input)
{
	m_aPgInputHistory[m_PgInputHistoryNext] = Input;
	m_PgInputHistoryNext = (m_PgInputHistoryNext + 1) % PG_INPUT_HISTORY;
	m_PgInputHistoryCount = std::min(m_PgInputHistoryCount + 1, PG_INPUT_HISTORY);
}

bool CTas::PgDelayedInput(int DelayTicks, CNetObj_PlayerInput *pOut) const
{
	if(DelayTicks < 0 || DelayTicks >= m_PgInputHistoryCount)
		return false;
	const int Idx = (m_PgInputHistoryNext - 1 - DelayTicks + PG_INPUT_HISTORY * 2) % PG_INPUT_HISTORY;
	*pOut = m_aPgInputHistory[Idx];
	return true;
}

// v6.18: the Dummy-container functions on the sandbox tick. AledBot wins over
// copy/control (same priority as ProcessDummy); hammer fly and triple fly only
// run when no aled plan owns the dummy this tick.
void CTas::PgDummyFunctionsInput(int D, CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &Prev, const CNetObj_PlayerInput &MainEff, const CNetObj_PlayerInput &MainPrev, bool bKeyboardActive)
{
	CBotNet &Bot = GameClient()->m_BotNet;
	const int Id = D == 0 ? m_LocalId : m_aPgDummyIds[D];
	const int TargetSlot = m_PgActiveDummy;
	// Like the real cl_dummy_hammer path, only an inactive dummy acts; the
	// selected playground tee is the target, never a hammer source.
	if(D == TargetSlot)
		return;
	const bool AledHandled = g_Config.m_KxAledEnabled && Bot.ApplyPlaygroundAled(m_Playground, D, Id, Input, Prev);
	if(AledHandled)
		return;
	// A keyboard-controlled dummy must keep its own movement: copy-moves and
	// dummy-control would overwrite the live keys. Hammer / triple fly / aled
	// are applied below regardless of who drives this dummy.
	if(!bKeyboardActive)
	{
	// Copy Moves Latency: inactive dummies replay the active tee's input from
	// N ticks ago (N = latency_ms * D), the same staggering the network path
	// uses. Falls back to the live input while the history is not deep enough.
	const CNetObj_PlayerInput *pMainEff = &MainEff;
	const CNetObj_PlayerInput *pMainPrev = &MainPrev;
	CNetObj_PlayerInput DelayedCur, DelayedPrev;
	const int LatencyMs = (g_Config.m_KxCopyMovesLatencyEnabled != 0) ? g_Config.m_KxCopyMovesLatency : 0;
	if(LatencyMs > 0 && D > 0)
	{
		const int DelayTicks = (LatencyMs * D * Client()->GameTickSpeed()) / 1000;
		if(PgDelayedInput(DelayTicks, &DelayedCur))
			pMainEff = &DelayedCur;
		if(PgDelayedInput(DelayTicks + 1, &DelayedPrev))
			pMainPrev = &DelayedPrev;
	}
	if(g_Config.m_ClDummyCopyMoves && (!g_Config.m_KxSmartCopyMoves || Bot.m_aSmartCopyAllow[D]))
	{
		const bool Filter = g_Config.m_KxDummyCopyMovesFilter != 0;
		const bool TfHooking = Bot.m_PgTripleFlyHooking && Bot.m_PgTripleFlyDummy == D;
		const bool KeepHammerWeapon = g_Config.m_KxFixDummyHammer != 0 &&
			(g_Config.m_ClDummyHammer != 0 || Input.m_WantedWeapon == WEAPON_HAMMER + 1);
		if(!Filter || g_Config.m_KxDummyCopyMovesFilterDirection)
			Input.m_Direction = g_Config.m_KxDummyCopyMirrorDir ? -pMainEff->m_Direction : pMainEff->m_Direction;
		if((!Filter || g_Config.m_KxDummyCopyMovesFilterJump))
			Input.m_Jump = pMainEff->m_Jump;
		if((!Filter || g_Config.m_KxDummyCopyMovesFilterHook) && !TfHooking)
			Input.m_Hook = pMainEff->m_Hook;
		if((!Filter || g_Config.m_KxDummyCopyMovesFilterAim) && !TfHooking)
		{
			const bool WantSmart = g_Config.m_KxDummyCopyAimMode == 0 && g_Config.m_KxDummyCopySmartAim;
			const bool WantMirror = g_Config.m_KxDummyCopyAimMode == 1 && (g_Config.m_KxDummyCopyMirrorAimX || g_Config.m_KxDummyCopyMirrorAimY);
			if(WantSmart)
			{
				// Same world point as Main: sandbox positions replace the
				// zoom-accurate camera conversion of the network path.
				const CCharacter *pMain = PlaygroundCharacter(TargetSlot);
				const CCharacter *pMe = PlaygroundCharacter(D);
				if(pMain && pMe)
				{
					const vec2 WorldAim = pMain->Core()->m_Pos + vec2((float)pMainEff->m_TargetX, (float)pMainEff->m_TargetY);
					const vec2 DummyAim = WorldAim - pMe->Core()->m_Pos;
					Input.m_TargetX = (int)DummyAim.x;
					Input.m_TargetY = (int)DummyAim.y;
				}
			}
			else if(WantMirror)
			{
				Input.m_TargetX = g_Config.m_KxDummyCopyMirrorAimX ? -pMainEff->m_TargetX : pMainEff->m_TargetX;
				Input.m_TargetY = g_Config.m_KxDummyCopyMirrorAimY ? -pMainEff->m_TargetY : pMainEff->m_TargetY;
			}
			else
			{
				Input.m_TargetX = pMainEff->m_TargetX;
				Input.m_TargetY = pMainEff->m_TargetY;
			}
		}
		if((!Filter || g_Config.m_KxDummyCopyMovesFilterWeapon) && !KeepHammerWeapon)
		{
			Input.m_WantedWeapon = pMainEff->m_WantedWeapon;
			Input.m_NextWeapon = (Prev.m_NextWeapon + pMainEff->m_NextWeapon - pMainPrev->m_NextWeapon) & INPUT_STATE_MASK;
			Input.m_PrevWeapon = (Prev.m_PrevWeapon + pMainEff->m_PrevWeapon - pMainPrev->m_PrevWeapon) & INPUT_STATE_MASK;
		}
		if(!g_Config.m_ClDummyControl && (!Filter || g_Config.m_KxDummyCopyMovesFilterFire))
			Input.m_Fire = (Prev.m_Fire + pMainEff->m_Fire - pMainPrev->m_Fire) & INPUT_STATE_MASK;
	}
	if(g_Config.m_ClDummyControl)
	{
		Input.m_Jump = g_Config.m_ClDummyJump;
		Input.m_Hook = g_Config.m_ClDummyHook;
		if(g_Config.m_ClDummyFire)
			Input.m_Fire = (Prev.m_Fire | 1) & INPUT_STATE_MASK;
		if(g_Config.m_KxDummyAim)
		{
			Input.m_TargetX = pMainEff->m_TargetX;
			Input.m_TargetY = pMainEff->m_TargetY;
		}
		if(g_Config.m_KxDummyDirection)
			Input.m_Direction = pMainEff->m_Direction;
	}
	}
	CCharacter *pActive = PlaygroundCharacter(TargetSlot);
	CCharacter *pDummy = PlaygroundCharacter(D);
	if(!pActive || !pDummy)
		return;
	const vec2 ActivePos = pActive->Core()->m_Pos;
	const vec2 MyPos = pDummy->Core()->m_Pos;
	// Dummy hammer / auto fly: the sandbox twin of the OnSnapInput hammer
	// branch. Auto fly only swings when the hit really lands this tick.
	if(g_Config.m_ClDummyHammer && !g_Config.m_ClDummyControl)
	{
		// Re-resolve the active slot every tick. In Playground the keyboard can
		// be switched to any local tee; using the real-world local tee position
		// here makes cl_dummy_hammer keep aiming at Main instead.
		vec2 Aim = ActivePos - MyPos;
		const bool CanHit = PgDummyHammerCanHit(D, TargetSlot, &Aim);
		if(CanHit || !g_Config.m_KxDummyHammerAuto)
		{
			Input.m_WantedWeapon = WEAPON_HAMMER + 1;
			Input.m_TargetX = (int)Aim.x;
			Input.m_TargetY = (int)Aim.y;
			Input.m_Fire = ((Prev.m_Fire + 1) | 1) & INPUT_STATE_MASK;
		}
	}
	// Auto Triple Fly: one acting dummy holds a hook on the target tee so the
	// main can hammer-fly; the hook yields to a landing auto-fly hit.
	if(g_Config.m_KxTripleFly)
	{
		const bool CopyLeavesHookAim = g_Config.m_ClDummyCopyMoves != 0 && g_Config.m_KxDummyCopyMovesFilter != 0;
		const bool HammerLeavesHook = g_Config.m_ClDummyHammer == 0 || g_Config.m_KxDummyHammerAuto != 0;
		const bool Blocked = !HammerLeavesHook || g_Config.m_KxFlyRide != 0 || g_Config.m_KxCopyMoves != 0 || g_Config.m_KxHelpers != 0 ||
			(!CopyLeavesHookAim && g_Config.m_ClDummyCopyMoves != 0) || g_Config.m_ClDummyControl != 0;
		int Acting = -1;
		if(!Blocked)
		{
			if(g_Config.m_KxTripleFlyDummyMode == 0)
			{
				float BestSq = 1e18f;
				for(int T = 0; T < m_PgDummyLimit; ++T)
				{
					if(T == m_PgActiveDummy)
						continue;
					const CCharacter *pT = m_apPgDummies[T];
					if(T == 0)
						pT = PlaygroundCharacter(0);
					if(!pT)
						continue;
					const float dsq = length_squared(pT->Core()->m_Pos - ActivePos);
					if(dsq < BestSq)
					{
						BestSq = dsq;
						Acting = T;
					}
				}
			}
			else if(g_Config.m_KxTripleFlyDummyId == D)
				Acting = D;
		}
		if(Acting == D)
		{
			// Auto Fly coexistence: while the one-tick hammer hit really lands,
			// the hook is released so the press is not competing with it.
			if(g_Config.m_KxDummyHammerAuto && PgDummyHammerCanHit(D, TargetSlot, nullptr))
			{
				Input.m_Hook = 0;
				Bot.m_PgTripleFlyHooking = false;
			}
			else if(!Bot.ApplyPlaygroundTripleFly(m_Playground, D, Id,
				PlaygroundCharacter(TargetSlot) ? PlaygroundCharacter(TargetSlot)->GetCid() : -1, Input))
			{
				Input.m_Hook = 0;
				Bot.m_PgTripleFlyHooking = false;
			}
		}
		else if(Bot.m_PgTripleFlyHooking && Bot.m_PgTripleFlyDummy == D)
		{
			Input.m_Hook = 0;
			Bot.m_PgTripleFlyHooking = false;
		}
	}
}

CNetObj_PlayerInput CTas::StepWorld(const CNetObj_PlayerInput &Input, const CTasTick *pPlayback)
{
	CCharacter *pChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pChar)
		return Input;
	if(pPlayback)
	{
		bool NeedSync = false;
		for(const STasDummyFrame &Frame : pPlayback->vDummyInputs)
			if(!m_apPgDummies[Frame.m_Slot])
			{
				m_PgDummyLimit = std::max(m_PgDummyLimit, Frame.m_Slot + 1);
				m_aPgDummyConnected[Frame.m_Slot] = true;
				NeedSync = true;
			}
		if(NeedSync)
			SyncPgDummies();
	}
	// /pause and /spec: the player is paused, not the game — the tee stands
	// still while the playground keeps ticking (engine ConTogglePause).
	CNetObj_PlayerInput Effective = Input;
	if(!pPlayback && m_PgActiveDummy != 0)
		Effective = pChar->GetLatestInput();
	if(!pPlayback && m_aPgPendingSwitchInputActive[0])
	{
		Effective = m_aPgPendingSwitchInput[0];
		m_aPgPendingSwitchInputActive[0] = false;
	}
	if(!pPlayback && m_PgActiveDummy == 0 && (m_PgPaused || m_PgSpec))
	{
		Effective.m_Direction = Effective.m_Jump = Effective.m_Hook = 0;
		Effective.m_Fire = (pChar->GetLatestInput().m_Fire + 1) & (INPUT_STATE_MASK & ~1);
		Effective.m_NextWeapon = pChar->GetLatestInput().m_NextWeapon;
		Effective.m_PrevWeapon = pChar->GetLatestInput().m_PrevWeapon;
	}
	const CNetObj_PlayerInput PreviousMain = pChar->GetLatestInput();
	if(!pPlayback && m_PgActiveDummy == 0 && PgFunctionsAllowed() && g_Config.m_KxTasBotFunctions)
	{
		CBotNet &Bot = GameClient()->m_BotNet;
		CControls &Controls = GameClient()->m_Controls;
		GameClient()->m_AimBot.ApplyPlayground(m_Playground, m_LocalId, Effective, Controls.m_aMousePos[0]);
		Bot.ApplyMoonwalk(Effective, PreviousMain, Controls.m_aInputDirectionLeft[0] != 0, Controls.m_aInputDirectionRight[0] != 0);
		if(g_Config.m_KxAutoFire && (Effective.m_Fire & 1))
		{
			if(g_Config.m_KxAutoFireDelay <= 0 || m_PgAutoFireCounter >= g_Config.m_KxAutoFireDelay - 1)
			{
				Effective.m_Fire = ((Effective.m_Fire + 1) | 1) & INPUT_STATE_MASK;
				m_PgAutoFireCounter = 0;
			}
			else
				m_PgAutoFireCounter++;
		}
		else
			m_PgAutoFireCounter = 0;
		if(g_Config.m_KxLaserUnfreeze)
			Bot.ApplyPlaygroundLaserUnfreeze(m_Playground, m_LocalId, Effective, Controls.m_aMousePos[0]);
		if(g_Config.m_KxAvoidFreeze)
			GameClient()->m_AvoidFreeze.ApplyPlayground(m_Playground, m_LocalId, Effective, Controls.m_aMousePos[0]);
		if(g_Config.m_KxQuickStop)
			Bot.ApplyQuickStop(Effective, pChar->Core()->m_Vel.x, pChar->m_FreezeTime == 0 && !pChar->Core()->m_DeepFrozen, pChar->IsGrounded());
	}
	m_LastInput = Effective;
	// v6.2: /invincible and /super — snapshot the tick so death tiles cannot
	// remove the tee; it is resurrected at the same position right after.
	CTasState PgPreTick;
	if(m_PgInvincible || m_PgSuper)
		PgPreTick = CaptureState();
	// Same tick recipe as the kinodynamic simulator:
	// OnDirectInput -> m_GameTick -> OnPredictedInput -> Tick().
	// v6.18: playground dummies are local simulated tees. They must take their
	// DIRECT input on the same tick as the main tee: OnDirectInput() runs
	// HandleWeaponSwitch()/FireWeapon(), which read GameWorld()->GameTick(), so
	// feeding them after the increment shifted every fire/reload edge by a tick.
	// Playback takes precedence over live controls. The sandbox never writes
	// to the network dummy inputs.
	CNetObj_PlayerInput ActiveEff = Effective;
	CNetObj_PlayerInput ActivePrev = PreviousMain;
	if(m_PgActiveDummy > 0)
	{
		ActiveEff = PlaygroundKeyboardInput();
		if(const CCharacter *pActive = PlaygroundCharacter(m_PgActiveDummy))
		{
			ActivePrev = pActive->GetLatestInput();
			ActiveEff.m_Fire = (ActiveEff.m_Fire & 1) ? (ActivePrev.m_Fire | 1) :
				((ActivePrev.m_Fire + 1) & ~1);
			ActiveEff.m_Fire &= INPUT_STATE_MASK;
			ActiveEff.m_NextWeapon = (ActivePrev.m_NextWeapon + ActiveEff.m_NextWeapon - m_PgLastKeyboardInput.m_NextWeapon) & INPUT_STATE_MASK;
			ActiveEff.m_PrevWeapon = (ActivePrev.m_PrevWeapon + ActiveEff.m_PrevWeapon - m_PgLastKeyboardInput.m_PrevWeapon) & INPUT_STATE_MASK;
		}
	}
	// Copy Moves Latency follows whichever tee currently owns the keyboard.
	PgRecordInputHistory(ActiveEff);
	if(!pPlayback && m_PgActiveDummy > 0 && PgDummyFunctions())
		PgDummyFunctionsInput(0, Effective, PreviousMain, ActiveEff, ActivePrev);
	// Apply all input transformations before OnDirectInput: weapon selection,
	// firing and other direct-input actions are consumed there, not by
	// OnPredictedInput. This is especially important when Main is an inactive
	// copy-moves target while keyboard control is on another playground tee.
	pChar->OnDirectInput(&Effective);
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		CCharacter *pDummy = m_apPgDummies[D];
		if(!pDummy)
			continue;
		const CNetObj_PlayerInput &Prev = pDummy->GetLatestInput();
		CNetObj_PlayerInput DummyInput = Prev;
		DummyInput.m_PlayerFlags = PLAYERFLAG_PLAYING;
		if(!pPlayback && m_aPgPendingSwitchInputActive[D])
		{
			DummyInput = m_aPgPendingSwitchInput[D];
			m_aPgPendingSwitchInputActive[D] = false;
		}
		if(pPlayback)
		{
			if(pPlayback->vDummyInputs.empty() && D == 1)
				DummyInput = pPlayback->DummyInput;
			for(const STasDummyFrame &Frame : pPlayback->vDummyInputs)
				if(Frame.m_Slot == D)
					DummyInput = Frame.m_Input;
		}
		else if(D == m_PgActiveDummy && !m_PgPaused && !m_PgSpec)
		{
			DummyInput = PlaygroundKeyboardInput();
			DummyInput.m_Fire = (DummyInput.m_Fire & 1) ? (Prev.m_Fire | 1) : ((Prev.m_Fire + 1) & ~1);
			DummyInput.m_Fire &= INPUT_STATE_MASK;
			DummyInput.m_NextWeapon = (Prev.m_NextWeapon + DummyInput.m_NextWeapon - m_PgLastKeyboardInput.m_NextWeapon) & INPUT_STATE_MASK;
			DummyInput.m_PrevWeapon = (Prev.m_PrevWeapon + DummyInput.m_PrevWeapon - m_PgLastKeyboardInput.m_PrevWeapon) & INPUT_STATE_MASK;
		}
		else if(D != m_PgActiveDummy && PgDummyFunctions())
		{
			PgDummyFunctionsInput(D, DummyInput, Prev, ActiveEff, ActivePrev);
		}
		else if(D == m_PgActiveDummy)
		{
			DummyInput.m_Direction = DummyInput.m_Jump = DummyInput.m_Hook = DummyInput.m_WantedWeapon = 0;
			if(DummyInput.m_Fire & 1)
				DummyInput.m_Fire++;
			DummyInput.m_Fire &= INPUT_STATE_MASK;
		}
		m_aPgDummyInputs[D] = DummyInput;
		pDummy->OnDirectInput(&DummyInput);
	}
	m_aPgDummyInputs[0] = Effective;
	m_PgLastKeyboardInput = GameClient()->m_Controls.m_aInputData[0];
	m_PlaygroundTick++;
	m_Playground.m_GameTick = m_PlaygroundTick;
	pChar->OnPredictedInput(&Effective);
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		if(CCharacter *pDummy = m_apPgDummies[D])
			pDummy->OnPredictedInput(&m_aPgDummyInputs[D]);
	}
	m_Playground.Tick();
	// A dummy killed by a death tile is deleted by the world — drop the pointer.
	for(int D = 1; D < MAX_DUMMIES; ++D)
	{
		const int Id = m_aPgDummyIds[D];
		if(Id && m_apPgDummies[D] && m_Playground.GetCharacterById(Id) != m_apPgDummies[D])
		{
			m_Playground.ReleaseHooked(Id);
			m_apPgDummies[D] = nullptr;
			m_aPgDummyIds[D] = 0;
			m_aPgDummyConnected[D] = false;
			if(D == m_PgActiveDummy)
				SetPgActiveDummy(0);
		}
	}
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
	// The tee may have been deleted by a death tile (or replaced by the
	// invincible restore above) — re-fetch instead of using the stale pointer.
	pChar = m_Playground.GetCharacterById(m_LocalId);
	// Double-jump sprite + sound: same pump as the real world, which fires
	// CEffects::AirJump from the core's triggered events after each tick.
	if(pChar && (pChar->Core()->m_TriggeredEvents & COREEVENT_AIR_JUMP))
		SpawnPlaygroundAirJump(GameClient(), pChar->Core()->m_Pos, m_Tps / 50.0f);
	return Effective;
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

// v3 replay weapon storage: an absolute ID in m_WantedWeapon instead of the
// next/prev edge counters, which only make sense relative to the previous
// tick and drift when a take is trimmed, slowed down or replayed out of order.
static void KxTasWeaponOverride(CNetObj_PlayerInput &Input, int ActiveWeapon)
{
	Input.m_WantedWeapon = in_range(ActiveWeapon, NUM_WEAPONS - 1) ? ActiveWeapon + 1 : 0;
	Input.m_NextWeapon = 0;
	Input.m_PrevWeapon = 0;
}

static bool KxTasFinishAt(CCollision *pCollision, vec2 Pos)
{
	if(!pCollision)
		return false;
	const int TileX = (int)floorf(Pos.x / 32.0f);
	const int TileY = (int)floorf(Pos.y / 32.0f);
	if(TileX < 0 || TileX >= pCollision->GetWidth() || TileY < 0 || TileY >= pCollision->GetHeight())
		return false;
	const int Index = TileY * pCollision->GetWidth() + TileX;
	return pCollision->GetTileIndex(Index) == TILE_FINISH ||
		pCollision->GetFrontTileIndex(Index) == TILE_FINISH;
}

static bool KxTasCrossedFinish(CCollision *pCollision, vec2 From, vec2 To)
{
	const vec2 Offsets[] = {
		vec2(0.0f, 0.0f), vec2(-14.0f, -14.0f), vec2(14.0f, -14.0f),
		vec2(-14.0f, 14.0f), vec2(14.0f, 14.0f),
	};
	const int Steps = std::clamp((int)ceilf(distance(From, To) / 8.0f), 1, 4096);
	for(int i = 0; i <= Steps; ++i)
	{
		const vec2 Pos = mix(From, To, (float)i / (float)Steps);
		for(const vec2 &Offset : Offsets)
			if(KxTasFinishAt(pCollision, Pos + Offset))
				return true;
	}
	return false;
}

void CTas::DoStep(const CNetObj_PlayerInput &Input, const CTasTick *pPlayback)
{
	if(!m_Joined)
		return;
	CCharacter *pMainChar = m_Playground.GetCharacterById(m_LocalId);
	if(!pMainChar)
		return;
	if(pPlayback && (!m_TakeFromFile || (m_PickedFormat == 0 && m_PickedVersion >= 3)))
	{
		ApplyTakeTimelineEvents(m_PlaygroundTick);
	}
	const CNetObj_PlayerInput Effective = StepWorld(Input, pPlayback);
	if(m_Recording)
	{
		for(int Slot = 0; Slot < MAX_DUMMIES; ++Slot)
		{
			if(m_aFinishTicks[Slot] > 0)
				continue;
			CCharacter *pTee = PlaygroundCharacter(Slot);
			if(pTee && KxTasCrossedFinish(m_Playground.Collision(), pTee->m_PrevPos, pTee->Core()->m_Pos))
				m_aFinishTicks[Slot] = std::max(1, m_PlaygroundTick - m_RecordStart.Tick);
		}
	}

	// Projectile trails: one particle per fake tick, fully time-scaled.
	const float TrailScale = m_Tps / (float)Client()->GameTickSpeed();
	for(CEntity *pEnt = m_Playground.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
		SpawnPlaygroundTrail(GameClient(), static_cast<CProjectile *>(pEnt)->GetData(), m_PlaygroundTick, (float)m_Playground.GameTickSpeed(), TrailScale);
	m_vPlaygroundInputs.push_back(Effective);
	m_vPlaygroundHistory.push_back(CaptureState());
	TrimHistory();
	if(m_Recording)
	{
		CTasTick Tick;
		Tick.Tick = m_PlaygroundTick;
		Tick.Input = Effective;
		// v3 stores the weapon as an ABSOLUTE override: m_WantedWeapon is the
		// weapon ID + 1 (see CCharacter::HandleWeaponSwitch), while next/prev
		// are only edge counters and drift when a replay is re-timed. The
		// sandbox itself keeps the real counters; only the recorded stream is
		// normalized so playback always selects the weapon seen on this tick.
		KxTasWeaponOverride(Tick.Input, pMainChar->Core()->m_ActiveWeapon);
		for(int D = 1; D < MAX_DUMMIES; ++D)
			if(const CCharacter *pDummy = m_apPgDummies[D])
			{
				CNetObj_PlayerInput DummyRecord = m_aPgDummyInputs[D];
				KxTasWeaponOverride(DummyRecord, pDummy->Core()->m_ActiveWeapon);
				Tick.vDummyInputs.push_back({D, DummyRecord, pDummy->Core()->m_Pos});
				Tick.vDummyHooks.push_back({D, pDummy->Core()->m_HookState, pDummy->Core()->m_HookPos});
			}
		if(m_apPgDummies[1])
		{
			Tick.DummyInput = m_aPgDummyInputs[1];
			KxTasWeaponOverride(Tick.DummyInput, m_apPgDummies[1]->Core()->m_ActiveWeapon);
		}
		const CTasState &S = m_vPlaygroundHistory.back();
		Tick.Core = S.Core;
		Tick.FreezeTime = S.FreezeTime;
		Tick.vProjectiles = S.vProjectiles.Items();
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
		DoStep(T.Input, &T);
		if(m_PlayIdx + 1 == (int)m_vSaved.size() && (!m_TakeFromFile || (m_PickedFormat == 0 && m_PickedVersion >= 3)))
		{
			ApplyTakeTimelineEvents(m_PlaygroundTick);
			m_vPlaygroundHistory.back() = CaptureState();
		}
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
	// Keep the live tail available without decompression for rendering and
	// immediate rewind. Compress one newly-cold snapshot per step.
	static constexpr size_t HOT_TAIL = 128;
	if(m_vPlaygroundHistory.size() > HOT_TAIL)
		m_vPlaygroundHistory[m_vPlaygroundHistory.size() - HOT_TAIL - 1].CompressHistoryPayload();
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
	// v6.18: without a picked file a fresh recording starts in the native
	// format, even if the previously picked take was a KRX replay.
	g_Config.m_KxTasFormat = 0;
	m_aPickedPath[0] = '\0';
}

// v6.18: KRX replay JSON support (OpenClient / eqsit). The wire format is the
// same as OpenKrx: a little-endian uint32 with the uncompressed JSON length,
// followed by zlib-compressed JSON. A plain JSON image is accepted on load for
// the old .json vault files.
static std::string KxTasJsonQuote(const char *pStr)
{
	CJsonStringWriter Writer;
	Writer.WriteStrValue(pStr ? pStr : "");
	std::string Result = Writer.GetOutputString();
	while(!Result.empty() && (Result.back() == '\n' || Result.back() == '\r'))
		Result.pop_back();
	return Result;
}

static const char *const g_aKrxInputKeys[] = {
	"m_Direction", "m_TargetX", "m_TargetY", "m_Jump", "m_Fire",
	"m_Hook", "m_PlayerFlags", "m_WantedWeapon", "m_NextWeapon", "m_PrevWeapon",
};

static int KxKrxInputValues(const CNetObj_PlayerInput &Input, int *pOut)
{
	pOut[0] = Input.m_Direction;
	pOut[1] = Input.m_TargetX;
	pOut[2] = Input.m_TargetY;
	pOut[3] = Input.m_Jump;
	pOut[4] = Input.m_Fire;
	pOut[5] = Input.m_Hook;
	pOut[6] = Input.m_PlayerFlags;
	pOut[7] = Input.m_WantedWeapon;
	pOut[8] = Input.m_NextWeapon;
	pOut[9] = Input.m_PrevWeapon;
	return 10;
}

static CNetObj_PlayerInput KxKrxMakeInput(const int *pValues)
{
	CNetObj_PlayerInput Input{};
	Input.m_Direction = pValues[0];
	Input.m_TargetX = pValues[1];
	Input.m_TargetY = pValues[2];
	Input.m_Jump = pValues[3];
	Input.m_Fire = pValues[4];
	Input.m_Hook = pValues[5];
	Input.m_PlayerFlags = pValues[6];
	Input.m_WantedWeapon = pValues[7];
	Input.m_NextWeapon = pValues[8];
	Input.m_PrevWeapon = pValues[9];
	return Input;
}

static std::string KxKrasFloat(float Value)
{
	std::ostringstream Out;
	Out.imbue(std::locale::classic());
	Out << std::setprecision(std::numeric_limits<float>::max_digits10) << Value;
	return Out.str();
}

static bool KxKrxReadNumber(const json_value *pValue, float *pOut)
{
	if(!pValue || (pValue->type != json_integer && pValue->type != json_double))
		return false;
	const double Value = pValue->type == json_integer ? (double)pValue->u.integer : pValue->u.dbl;
	*pOut = (float)Value;
	return std::isfinite(*pOut);
}

static bool KxKrxReadInt(const json_value *pValue, int *pOut)
{
	if(!pValue || (pValue->type != json_integer && pValue->type != json_double))
		return false;
	const double Value = pValue->type == json_integer ? (double)pValue->u.integer : pValue->u.dbl;
	if(Value < INT_MIN || Value > INT_MAX)
		return false;
	*pOut = (int)Value;
	return true;
}

static const char *KxKrxString(const json_value *pValue, const char *pDefault = "unknown")
{
	return pValue && pValue->type == json_string ? json_string_get(pValue) : pDefault;
}

bool CTas::WriteKrxTakeFile(const char *pPath, const char *pMap, const char *pDescription, const char *pAuthor, const char *pCreated)
{
	const std::vector<CTasTick> &vTake = m_Recording ? m_vRecord : m_vSaved;
	if(vTake.empty())
		return false;

	std::ostringstream Json;
	Json.imbue(std::locale::classic());
	Json << std::setprecision(std::numeric_limits<float>::max_digits10);
	Json << "{\"info\":{\"author\":" << KxTasJsonQuote(pAuthor)
	     << ",\"description\":" << KxTasJsonQuote(pDescription)
	     << ",\"date_modified\":" << KxTasJsonQuote(pCreated)
	     << ",\"map\":" << KxTasJsonQuote(pMap) << "}";
	const auto DummyFrame = [](const CTasTick &Tick) {
		const auto It = std::find_if(Tick.vDummyInputs.begin(), Tick.vDummyInputs.end(), [](const STasDummyFrame &Frame) { return Frame.m_Slot == 1; });
		return It == Tick.vDummyInputs.end() ? nullptr : &*It;
	};
	const auto FirstDummy = std::find_if(vTake.begin(), vTake.end(), [&](const CTasTick &Tick) { return DummyFrame(Tick) != nullptr; });
	const auto WriteFrames = [&](bool Dummy) {
		Json << ",\"" << (Dummy ? "replay_dummy" : "replay_local") << "\":[";
		// KRX recorders prepend 50 neutral frames — keep that convention so
		// these files play identically in OpenClient and in the Kinetix TAS.
		const vec2 StartPos = Dummy ? DummyFrame(*FirstDummy)->m_Pos : vTake.front().Core.m_Pos;
		vec2 LastPos = StartPos;
		for(int i = 0; i < 50; i++)
		{
			if(i)
				Json << ',';
			Json << "{\"input\":{";
			for(int k = 0; k < 10; k++)
				Json << (k ? "," : "") << '"' << g_aKrxInputKeys[k] << "\":0";
			Json << "},\"pos\":{\"x\":" << KxKrasFloat(StartPos.x) << ",\"y\":" << KxKrasFloat(StartPos.y) << "}}";
		}
		for(size_t i = 0; i < vTake.size(); i++)
		{
			const CNetObj_PlayerInput &Input = Dummy ? vTake[i].DummyInput : vTake[i].Input;
			int aValues[10];
			KxKrxInputValues(Input, aValues);
			Json << ',';
			Json << "{\"input\":{";
			for(int k = 0; k < 10; k++)
				Json << (k ? "," : "") << '"' << g_aKrxInputKeys[k] << "\":" << aValues[k];
			const STasDummyFrame *pFrame = Dummy ? DummyFrame(vTake[i]) : nullptr;
			const vec2 Pos = Dummy ? (pFrame ? pFrame->m_Pos : LastPos) : vTake[i].Core.m_Pos;
			LastPos = Pos;
			Json << "},\"pos\":{\"x\":" << KxKrasFloat(Pos.x) << ",\"y\":" << KxKrasFloat(Pos.y) << "}}";
		}
		Json << ']';
	};
	WriteFrames(false);
	if(FirstDummy != vTake.end())
		WriteFrames(true);
	Json << '}';

	const std::string Text = Json.str();
	constexpr size_t MAX_JSON_BYTES = 128 * 1024 * 1024;
	if(Text.empty() || Text.size() > MAX_JSON_BYTES)
		return false;
	uLongf CompLen = compressBound((uLong)Text.size());
	std::vector<unsigned char> vEncoded(CompLen + 4);
	const uint32_t Length = (uint32_t)Text.size();
	for(int i = 0; i < 4; i++)
		vEncoded[i] = (unsigned char)(Length >> (8 * i));
	if(compress2(vEncoded.data() + 4, &CompLen, (const Bytef *)Text.data(), Text.size(), Z_BEST_COMPRESSION) != Z_OK)
		return false;
	vEncoded.resize((size_t)CompLen + 4);

	IOHANDLE File = Storage()->OpenFile(pPath, IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
	if(!File)
		return false;
	const bool Ok = io_write(File, vEncoded.data(), vEncoded.size()) == vEncoded.size();
	io_close(File);
	return Ok;
}

bool CTas::LoadKrxTakeMem(const void *pRawData, unsigned RawSize)
{
	if(m_Recording || !pRawData || !RawSize)
		return false;
	const unsigned char *pData = (const unsigned char *)pRawData;
	constexpr size_t MAX_JSON_BYTES = 128 * 1024 * 1024;
	if(RawSize > MAX_JSON_BYTES)
		return false;

	std::string Text;
	if(pData[0] == '{')
	{
		Text.assign((const char *)pData, RawSize);
	}
	else
	{
		if(RawSize < 5)
			return false;
		uint32_t Length = 0;
		for(int i = 0; i < 4; i++)
			Length |= (uint32_t)pData[i] << (8 * i);
		if(!Length || Length > MAX_JSON_BYTES)
			return false;
		Text.resize(Length);
		uLongf DestLen = Length;
		if(uncompress((Bytef *)Text.data(), &DestLen, pData + 4, (uLong)(RawSize - 4)) != Z_OK || DestLen != Length)
			return false;
	}

	std::unique_ptr<json_value, decltype(&json_value_free)> pRoot(JsonParse(Text.data(), Text.size()), json_value_free);
	if(!pRoot || pRoot->type != json_object)
		return false;
	const json_value *pInfo = json_object_get(pRoot.get(), "info");
	if(!pInfo || pInfo->type != json_object)
		return false;
	char aAuthor[128] = {0};
	char aDescription[256] = {0};
	char aMap[128] = {0};
	char aCreated[64] = {0};
	str_copy(aAuthor, KxKrxString(json_object_get(pInfo, "author")), sizeof(aAuthor));
	str_copy(aDescription, KxKrxString(json_object_get(pInfo, "description"), ""), sizeof(aDescription));
	str_copy(aMap, KxKrxString(json_object_get(pInfo, "map"), ""), sizeof(aMap));
	str_copy(aCreated, KxKrxString(json_object_get(pInfo, "date_modified"), ""), sizeof(aCreated));

	std::vector<CTasTick> vNew;
	bool bKrxDummy = false;
	vec2 KrxDummyStart = vec2(0.0f, 0.0f);
	for(int Slot = 0; Slot < 2; Slot++)
	{
		const json_value *pFrames = json_object_get(pRoot.get(), Slot ? "replay_dummy" : "replay_local");
		if(!pFrames)
		{
			if(Slot == 0)
				return false;
			continue;
		}
		if(pFrames->type != json_array || pFrames->u.array.length > 1000000)
			return false;
		const int Count = json_array_length(pFrames);
		if(Slot == 0)
			vNew.resize(Count);
		else if(Count != (int)vNew.size())
			continue; // tolerate a mismatched dummy stream: keep main replay
		for(int i = 0; i < Count; i++)
		{
			const json_value *pFrame = json_array_get(pFrames, i);
			if(!pFrame || pFrame->type != json_object)
				return false;
			const json_value *pInput = json_object_get(pFrame, "input");
			const json_value *pPos = json_object_get(pFrame, "pos");
			if(!pInput || pInput->type != json_object || !pPos || pPos->type != json_object)
				return false;
			int aValues[10] = {};
			for(int k = 0; k < 10; k++)
				if(!KxKrxReadInt(json_object_get(pInput, g_aKrxInputKeys[k]), &aValues[k]))
					return false;
			vec2 Pos;
			if(!KxKrxReadNumber(json_object_get(pPos, "x"), &Pos.x) || !KxKrxReadNumber(json_object_get(pPos, "y"), &Pos.y))
				return false;
			if(Slot == 0)
			{
				vNew[i].Tick = i;
				vNew[i].Input = KxKrxMakeInput(aValues);
				vNew[i].Core.m_Pos = Pos;
			}
			else
			{
				vNew[i].DummyInput = KxKrxMakeInput(aValues);
				vNew[i].vDummyInputs.push_back({Slot, vNew[i].DummyInput, Pos});
				if(i == 0)
				{
					bKrxDummy = true;
					KrxDummyStart = Pos;
				}
			}
		}
	}
	if(vNew.empty())
		return false;

	m_vSaved = std::move(vNew);
	m_aPickedPath[0] = '\0';
	str_copy(m_aPickedMap, aMap, sizeof(m_aPickedMap));
	str_copy(m_aPickedDescription, aDescription, sizeof(m_aPickedDescription));
	str_copy(m_aPickedAuthor, aAuthor, sizeof(m_aPickedAuthor));
	str_copy(m_aPickedCreated, aCreated, sizeof(m_aPickedCreated));
	m_PickedVersion = 1; // KRX replay JSON
	str_copy(m_aPickedVersionCode, "10");
	m_PickedUsedFlags = 0;
	m_PickedFinishTicks = -1;
	m_vFileStartLoadouts.clear();
	m_vTasRecordedDummyStarts.clear();
	if(bKrxDummy)
		m_vTasRecordedDummyStarts.push_back({1, KrxDummyStart});
	m_PickedFormat = 1;
	g_Config.m_KxTasFormat = 1;
	m_FilePicked = true;
	m_Playing = false;
	CancelPreparation();
	m_RealPlaying = false;
	m_Paused = false;
	m_PlayIdx = 0;
	m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
	m_TakeFromFile = true;
	m_HasTakePgStart = false;
	m_SavedStart = CTasState{};
	m_TakePgPractice = m_TakePgInvincible = m_TakePgSuper = false;
	m_TakePgPaused = m_TakePgSpec = m_TakePgTuningDirty = false;
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
		m_aTakePgTuning[Zone] = CTuningParams{};
	m_vSpawnerRecords.clear();
	m_vPgChat.clear();
	m_vTakeSpawner.clear();
	m_vTakeSpawnerEvents.clear();
	m_vTakeChat.clear();
	m_vTakeRcon.clear();
	m_vTakeTimelineEvents.clear();
	m_TasTimelineNextOrder = 0;
	return true;
}

struct CKxTasCompactWriter
{
	std::vector<unsigned char> m_Data;
	bool m_Ok = true;

	void Bytes(const void *pData, size_t Size)
	{
		if(!m_Ok || Size > 512u * 1024u * 1024u - m_Data.size())
		{
			m_Ok = false;
			return;
		}
		const unsigned char *pBytes = (const unsigned char *)pData;
		m_Data.insert(m_Data.end(), pBytes, pBytes + Size);
	}
	void U8(uint8_t Value) { Bytes(&Value, sizeof(Value)); }
	void U16(uint16_t Value)
	{
		const unsigned char aBytes[2] = {(unsigned char)Value, (unsigned char)(Value >> 8)};
		Bytes(aBytes, sizeof(aBytes));
	}
	void U32(uint32_t Value)
	{
		const unsigned char aBytes[4] = {
			(unsigned char)Value, (unsigned char)(Value >> 8),
			(unsigned char)(Value >> 16), (unsigned char)(Value >> 24)};
		Bytes(aBytes, sizeof(aBytes));
	}
	void Float(float Value)
	{
		if(!std::isfinite(Value))
		{
			m_Ok = false;
			return;
		}
		uint32_t Bits;
		mem_copy(&Bits, &Value, sizeof(Bits));
		U32(Bits);
	}
	void Input(const CNetObj_PlayerInput &Input)
	{
		U32((uint32_t)Input.m_Direction);
		U32((uint32_t)Input.m_TargetX);
		U32((uint32_t)Input.m_TargetY);
		U32((uint32_t)Input.m_Jump);
		U32((uint32_t)Input.m_Fire);
		U32((uint32_t)Input.m_Hook);
		U32((uint32_t)Input.m_PlayerFlags);
		U32((uint32_t)Input.m_WantedWeapon);
		U32((uint32_t)Input.m_NextWeapon);
		U32((uint32_t)Input.m_PrevWeapon);
	}
	void Projectile(const CProjectileData &Data)
	{
		Float(Data.m_StartPos.x);
		Float(Data.m_StartPos.y);
		Float(Data.m_StartVel.x);
		Float(Data.m_StartVel.y);
		U32((uint32_t)Data.m_Type);
		U32((uint32_t)Data.m_StartTick);
		U8(Data.m_ExtraInfo ? 1 : 0);
		U32((uint32_t)Data.m_Owner);
		U8(Data.m_Explosive ? 1 : 0);
		U32((uint32_t)Data.m_Bouncing);
		U8(Data.m_Freeze ? 1 : 0);
		U32((uint32_t)Data.m_SwitchNumber);
		U32((uint32_t)Data.m_TuneZone);
	}
	void StartLoadout(const STasStartLoadout &Start)
	{
		U32((uint32_t)Start.m_Slot);
		U32((uint32_t)Start.m_Weapon);
		U32((uint32_t)Start.m_WeaponMask);
		Input(Start.m_PreviousInput);
		Float(Start.m_Pos.x);
		Float(Start.m_Pos.y);
		Float(Start.m_Vel.x);
		Float(Start.m_Vel.y);
	}
	void String(const char *pText)
	{
		const size_t Length = pText ? (size_t)str_length(pText) : 0;
		if(Length > UINT16_MAX)
		{
			m_Ok = false;
			return;
		}
		U16((uint16_t)Length);
		if(Length)
			Bytes(pText, Length);
	}
	void FixedString(const char *pText, size_t Capacity)
	{
		size_t Length = 0;
		while(pText && Length < Capacity && pText[Length])
			++Length;
		if(Length == Capacity)
		{
			m_Ok = false;
			return;
		}
		U16((uint16_t)Length);
		if(Length)
			Bytes(pText, Length);
	}
	void Settings(const STasSpawnerSettings &S)
	{
		U32((uint32_t)S.m_Mode);
		U32((uint32_t)S.m_Entity);
		FixedString(S.m_aName, sizeof(S.m_aName));
		FixedString(S.m_aClan, sizeof(S.m_aClan));
		U32((uint32_t)S.m_Id);
		FixedString(S.m_aSkin, sizeof(S.m_aSkin));
		FixedString(S.m_aColorBody, sizeof(S.m_aColorBody));
		FixedString(S.m_aColorFeet, sizeof(S.m_aColorFeet));
		U32((uint32_t)S.m_HookStrength);
		U32((uint32_t)S.m_HookStrengthMode);
		U32((uint32_t)S.m_WeaponId);
		U8(S.m_PracticeParticles ? 1 : 0);
		U8(S.m_EntityPhysics ? 1 : 0);
		Float(S.m_PrevPosX);
		Float(S.m_PrevPosY);
		U32((uint32_t)S.m_EntityTimelineTick);
		U32((uint32_t)S.m_Dir);
		Float(S.m_VelX);
		Float(S.m_VelY);
		U32((uint32_t)S.m_LifeSpan);
		U32((uint32_t)S.m_LaserReach);
		U32((uint32_t)S.m_LaserLength);
		U32((uint32_t)S.m_DraggerStrength);
		U8(S.m_ProjectileFreeze ? 1 : 0);
		U8(S.m_ProjectileExplosive ? 1 : 0);
		U8(S.m_ProjectileSound ? 1 : 0);
		U32((uint32_t)S.m_LaserType);
		U32((uint32_t)S.m_PickupType);
		U32((uint32_t)S.m_Subtype);
		U32((uint32_t)S.m_SwitchNumber);
		U32((uint32_t)S.m_SpeedupForce);
		U32((uint32_t)S.m_SpeedupMaxSpeed);
		U32((uint32_t)S.m_SpeedupAngle);
		U32((uint32_t)S.m_Tile);
		U32((uint32_t)S.m_TileNumber);
		U8(S.m_TilePhysics ? 1 : 0);
		U32((uint32_t)S.m_CoordsMode);
		Float(S.m_X);
		Float(S.m_Y);
		U8(S.m_Snap ? 1 : 0);
		U32((uint32_t)S.m_LoopDelay);
	}
	void SpawnerRecord(const STasSpawnerRecord &Record)
	{
		U32((uint32_t)Record.m_Tick);
		U32((uint32_t)Record.m_Mode);
		U32((uint32_t)Record.m_Entity);
		U32((uint32_t)Record.m_Tile);
		U8(Record.m_Physics ? 1 : 0);
		Float(Record.m_Pos.x);
		Float(Record.m_Pos.y);
		Settings(Record.m_Settings);
	}
	void SpawnedSnapshot(const SSpawnedEntSnapshot &S)
	{
		U32((uint32_t)S.m_Entity);
		U32((uint32_t)S.m_Id);
		U32((uint32_t)S.m_Tick);
		Float(S.m_Pos.x); Float(S.m_Pos.y);
		U8(S.m_Inserted ? 1 : 0);
		Settings(S.m_Settings);
		if(S.m_Entity == TAS_SPAWN_ENT_TEE)
		{
			CNetObj_CharacterCore N{};
			S.Core.Write(&N);
			const int aValues[] = {N.m_X, N.m_Y, N.m_VelX, N.m_VelY, N.m_HookState,
				N.m_HookTick, N.m_HookX, N.m_HookY, N.m_HookDx, N.m_HookDy,
				N.m_HookedPlayer, N.m_Jumped, N.m_Direction, N.m_Angle,
				S.Core.m_ActiveWeapon, S.FreezeTime, S.ReloadTimer, S.AttackTick};
			for(int Value : aValues) U32((uint32_t)Value);
			Float(S.PrevPos.x); Float(S.PrevPos.y);
			Float(S.PrevPrevPos.x); Float(S.PrevPrevPos.y);
		}
		else if(S.m_Entity == TAS_SPAWN_ENT_PROJECTILE)
			Projectile(S.ProjData);
		else if(S.m_Entity == TAS_SPAWN_ENT_LASER)
		{
			const CLaserState &L = S.Laser;
			Float(L.Data.m_From.x); Float(L.Data.m_From.y);
			Float(L.Data.m_To.x); Float(L.Data.m_To.y);
			U32((uint32_t)L.Data.m_StartTick); U8(L.Data.m_ExtraInfo ? 1 : 0);
			U32((uint32_t)L.Data.m_Owner); U32((uint32_t)L.Data.m_Type);
			U32((uint32_t)L.Data.m_SwitchNumber); U32((uint32_t)L.Data.m_Subtype);
			U8(L.Data.m_Predict ? 1 : 0); U32((uint32_t)L.Data.m_TuneZone);
			Float(L.Dir.x); Float(L.Dir.y); Float(L.PrevPos.x); Float(L.PrevPos.y);
			Float(L.Energy); U32((uint32_t)L.Bounces); U32((uint32_t)L.Owner);
			U8(L.ZeroEnergyBounce ? 1 : 0);
		}
	}
	void Chat(const CTas::SPgChatLine &Line)
	{
		U32((uint32_t)Line.m_Team);
		U32((uint32_t)Line.m_ClientId);
		FixedString(Line.m_aName, sizeof(Line.m_aName));
		FixedString(Line.m_aText, sizeof(Line.m_aText));
	}
	void TimelineEvent(const STasTimelineEvent &Event)
	{
		U8((uint8_t)Event.m_Type);
		U32((uint32_t)Event.m_Tick);
		U32(Event.m_Order);
		U32((uint32_t)Event.m_ActiveDummy);
		U32((uint32_t)Event.m_Team);
		U32((uint32_t)Event.m_ClientId);
		U8(Event.m_HasCursor ? 1 : 0);
		Float(Event.m_Cursor.x);
		Float(Event.m_Cursor.y);
		U8(Event.m_Value ? 1 : 0);
		FixedString(Event.m_aText, sizeof(Event.m_aText));
		if(Event.m_Type == TAS_TIMELINE_EVENT_TUNING)
		{
			U32((uint32_t)Event.m_TuningZone);
			U32((uint32_t)Event.m_TuningCount);
			for(int i = 0; i < Event.m_TuningCount; ++i)
				U32((uint32_t)Event.m_aTuningValues[i]);
		}
	}
};

struct CKxTasCompactReader
{
	const unsigned char *m_pData;
	size_t m_Size;
	size_t m_Offset = 0;

	bool Bytes(void *pOut, size_t Size)
	{
		if(m_Offset > m_Size || Size > m_Size - m_Offset)
			return false;
		if(Size)
			mem_copy(pOut, m_pData + m_Offset, Size);
		m_Offset += Size;
		return true;
	}
	bool U8(uint8_t *pValue) { return Bytes(pValue, sizeof(*pValue)); }
	bool U16(uint16_t *pValue)
	{
		unsigned char aBytes[2];
		if(!Bytes(aBytes, sizeof(aBytes)))
			return false;
		*pValue = (uint16_t)aBytes[0] | (uint16_t)aBytes[1] << 8;
		return true;
	}
	bool U32(uint32_t *pValue)
	{
		unsigned char aBytes[4];
		if(!Bytes(aBytes, sizeof(aBytes)))
			return false;
		*pValue = (uint32_t)aBytes[0] | (uint32_t)aBytes[1] << 8 |
			(uint32_t)aBytes[2] << 16 | (uint32_t)aBytes[3] << 24;
		return true;
	}
	bool Float(float *pValue)
	{
		uint32_t Bits;
		if(!U32(&Bits))
			return false;
		mem_copy(pValue, &Bits, sizeof(Bits));
		return std::isfinite(*pValue);
	}
	bool Input(CNetObj_PlayerInput *pInput)
	{
		uint32_t aValues[10];
		for(uint32_t &Value : aValues)
			if(!U32(&Value))
				return false;
		pInput->m_Direction = (int32_t)aValues[0];
		pInput->m_TargetX = (int32_t)aValues[1];
		pInput->m_TargetY = (int32_t)aValues[2];
		pInput->m_Jump = (int32_t)aValues[3];
		pInput->m_Fire = (int32_t)aValues[4];
		pInput->m_Hook = (int32_t)aValues[5];
		pInput->m_PlayerFlags = (int32_t)aValues[6];
		pInput->m_WantedWeapon = (int32_t)aValues[7];
		pInput->m_NextWeapon = (int32_t)aValues[8];
		pInput->m_PrevWeapon = (int32_t)aValues[9];
		return true;
	}
	bool Projectile(CProjectileData *pData)
	{
		uint32_t Type, StartTick, Owner, Bouncing, SwitchNumber, TuneZone;
		uint8_t ExtraInfo, Explosive, Freeze;
		if(!Float(&pData->m_StartPos.x) || !Float(&pData->m_StartPos.y) ||
			!Float(&pData->m_StartVel.x) || !Float(&pData->m_StartVel.y) ||
			!U32(&Type) || !U32(&StartTick) || !U8(&ExtraInfo) || ExtraInfo > 1 ||
			!U32(&Owner) || !U8(&Explosive) || Explosive > 1 ||
			!U32(&Bouncing) || !U8(&Freeze) || Freeze > 1 ||
			!U32(&SwitchNumber) || !U32(&TuneZone))
			return false;
		pData->m_Type = (int32_t)Type;
		pData->m_StartTick = (int32_t)StartTick;
		pData->m_ExtraInfo = ExtraInfo != 0;
		pData->m_Owner = (int32_t)Owner;
		pData->m_Explosive = Explosive != 0;
		pData->m_Bouncing = (int32_t)Bouncing;
		pData->m_Freeze = Freeze != 0;
		pData->m_SwitchNumber = (int32_t)SwitchNumber;
		pData->m_TuneZone = (int32_t)TuneZone;
		return true;
	}
	bool StartLoadout(STasStartLoadout *pStart)
	{
		uint32_t Slot, Weapon, WeaponMask;
		if(!U32(&Slot) || !U32(&Weapon) || !U32(&WeaponMask))
			return false;
		pStart->m_Slot = (int32_t)Slot;
		pStart->m_Weapon = (int32_t)Weapon;
		pStart->m_WeaponMask = (int32_t)WeaponMask;
		return Input(&pStart->m_PreviousInput) &&
			Float(&pStart->m_Pos.x) && Float(&pStart->m_Pos.y) &&
			Float(&pStart->m_Vel.x) && Float(&pStart->m_Vel.y);
	}
	bool FixedString(char *pOut, size_t Capacity)
	{
		uint16_t Length;
		if(!U16(&Length) || Length >= Capacity || m_Offset > m_Size ||
			Length > m_Size - m_Offset)
			return false;
		if(Length)
			mem_copy(pOut, m_pData + m_Offset, Length);
		pOut[Length] = '\0';
		m_Offset += Length;
		return true;
	}
	bool Settings(STasSpawnerSettings *pSettings)
	{
		uint32_t Value;
		uint8_t Flag;
		if(!U32(&Value))
			return false;
		pSettings->m_Mode = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pSettings->m_Entity = (int32_t)Value;
		if(!FixedString(pSettings->m_aName, sizeof(pSettings->m_aName)) ||
			!FixedString(pSettings->m_aClan, sizeof(pSettings->m_aClan)) ||
			!U32(&Value))
			return false;
		pSettings->m_Id = (int32_t)Value;
		if(!FixedString(pSettings->m_aSkin, sizeof(pSettings->m_aSkin)) ||
			!FixedString(pSettings->m_aColorBody, sizeof(pSettings->m_aColorBody)) ||
			!FixedString(pSettings->m_aColorFeet, sizeof(pSettings->m_aColorFeet)) ||
			!U32(&Value))
			return false;
		pSettings->m_HookStrength = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pSettings->m_HookStrengthMode = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pSettings->m_WeaponId = (int32_t)Value;
		if(!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_PracticeParticles = Flag != 0;
		if(!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_EntityPhysics = Flag != 0;
		if(!Float(&pSettings->m_PrevPosX) || !Float(&pSettings->m_PrevPosY) ||
			!U32(&Value))
			return false;
		pSettings->m_EntityTimelineTick = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pSettings->m_Dir = (int32_t)Value;
		if(!Float(&pSettings->m_VelX) || !Float(&pSettings->m_VelY) ||
			!U32(&Value))
			return false;
		pSettings->m_LifeSpan = (int32_t)Value;
		const auto ReadInt = [&](int &Field) {
			if(!U32(&Value))
				return false;
			Field = (int32_t)Value;
			return true;
		};
		if(!ReadInt(pSettings->m_LaserReach) || !ReadInt(pSettings->m_LaserLength) ||
			!ReadInt(pSettings->m_DraggerStrength))
			return false;
		if(!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_ProjectileFreeze = Flag != 0;
		if(!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_ProjectileExplosive = Flag != 0;
		if(!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_ProjectileSound = Flag != 0;
		if(!ReadInt(pSettings->m_LaserType) || !ReadInt(pSettings->m_PickupType) ||
			!ReadInt(pSettings->m_Subtype) || !ReadInt(pSettings->m_SwitchNumber) ||
			!ReadInt(pSettings->m_SpeedupForce) || !ReadInt(pSettings->m_SpeedupMaxSpeed) ||
			!ReadInt(pSettings->m_SpeedupAngle) || !ReadInt(pSettings->m_Tile) ||
			!ReadInt(pSettings->m_TileNumber))
			return false;
		if(!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_TilePhysics = Flag != 0;
		if(!U32(&Value))
			return false;
		pSettings->m_CoordsMode = (int32_t)Value;
		if(!Float(&pSettings->m_X) || !Float(&pSettings->m_Y) ||
			!U8(&Flag) || Flag > 1)
			return false;
		pSettings->m_Snap = Flag != 0;
		if(!U32(&Value))
			return false;
		pSettings->m_LoopDelay = (int32_t)Value;
		return true;
	}
	bool SpawnerRecord(STasSpawnerRecord *pRecord)
	{
		uint32_t Value;
		uint8_t Physics;
		if(!U32(&Value))
			return false;
		pRecord->m_Tick = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pRecord->m_Mode = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pRecord->m_Entity = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pRecord->m_Tile = (int32_t)Value;
		if(!U8(&Physics) || Physics > 1)
			return false;
		pRecord->m_Physics = Physics != 0;
		return Float(&pRecord->m_Pos.x) && Float(&pRecord->m_Pos.y) &&
			Settings(&pRecord->m_Settings);
	}
	bool SpawnedSnapshot(SSpawnedEntSnapshot *pS)
	{
		const auto Int = [&](int &Field) {
			uint32_t V;
			if(!U32(&V)) return false;
			Field = (int32_t)V;
			return true;
		};
		const auto Bool = [&](bool &Field) {
			uint8_t V;
			if(!U8(&V) || V > 1) return false;
			Field = V != 0;
			return true;
		};
		if(!Int(pS->m_Entity) || !in_range(pS->m_Entity, TAS_SPAWN_ENT_COUNT - 1) ||
			!Int(pS->m_Id) || !Int(pS->m_Tick) ||
			!Float(&pS->m_Pos.x) || !Float(&pS->m_Pos.y) ||
			!Bool(pS->m_Inserted) || !Settings(&pS->m_Settings) ||
			pS->m_Settings.m_Mode != TAS_SPAWN_MODE_ENTITY || pS->m_Settings.m_Entity != pS->m_Entity)
			return false;
		if(pS->m_Entity == TAS_SPAWN_ENT_TEE)
		{
			CNetObj_CharacterCore N{};
			int *apValues[] = {&N.m_X, &N.m_Y, &N.m_VelX, &N.m_VelY, &N.m_HookState,
				&N.m_HookTick, &N.m_HookX, &N.m_HookY, &N.m_HookDx, &N.m_HookDy,
				&N.m_HookedPlayer, &N.m_Jumped, &N.m_Direction, &N.m_Angle,
				&pS->Core.m_ActiveWeapon, &pS->FreezeTime, &pS->ReloadTimer, &pS->AttackTick};
			for(int *pValue : apValues)
				if(!Int(*pValue)) return false;
			if(!in_range(pS->m_Id, MAX_CLIENTS - 1) ||
				!in_range(pS->Core.m_ActiveWeapon, NUM_WEAPONS - 1) ||
				N.m_HookedPlayer < -1 || N.m_HookedPlayer >= MAX_CLIENTS ||
				!Float(&pS->PrevPos.x) || !Float(&pS->PrevPos.y) ||
				!Float(&pS->PrevPrevPos.x) || !Float(&pS->PrevPrevPos.y))
				return false;
			pS->Core.Read(&N);
			pS->m_NetCoreOnly = true;
		}
		else if(pS->m_Entity == TAS_SPAWN_ENT_PROJECTILE)
			return Projectile(&pS->ProjData);
		else if(pS->m_Entity == TAS_SPAWN_ENT_LASER)
		{
			CLaserState &L = pS->Laser;
			return Float(&L.Data.m_From.x) && Float(&L.Data.m_From.y) &&
				Float(&L.Data.m_To.x) && Float(&L.Data.m_To.y) &&
				Int(L.Data.m_StartTick) && Bool(L.Data.m_ExtraInfo) &&
				Int(L.Data.m_Owner) && Int(L.Data.m_Type) && Int(L.Data.m_SwitchNumber) &&
				Int(L.Data.m_Subtype) && Bool(L.Data.m_Predict) && Int(L.Data.m_TuneZone) &&
				Float(&L.Dir.x) && Float(&L.Dir.y) && Float(&L.PrevPos.x) &&
				Float(&L.PrevPos.y) && Float(&L.Energy) && Int(L.Bounces) &&
				Int(L.Owner) && Bool(L.ZeroEnergyBounce);
		}
		return true;
	}
	bool Chat(CTas::SPgChatLine *pLine)
	{
		uint32_t Value;
		if(!U32(&Value))
			return false;
		pLine->m_Team = (int32_t)Value;
		if(!U32(&Value))
			return false;
		pLine->m_ClientId = (int32_t)Value;
		return FixedString(pLine->m_aName, sizeof(pLine->m_aName)) &&
			FixedString(pLine->m_aText, sizeof(pLine->m_aText));
	}
	bool TimelineEvent(STasTimelineEvent *pEvent)
	{
		uint8_t Type, HasCursor, Value;
		uint32_t Tick, Order, ActiveDummy, Team, ClientId;
		if(!U8(&Type) || !U32(&Tick) || !U32(&Order) ||
			!U32(&ActiveDummy) || !U32(&Team) || !U32(&ClientId) ||
			!U8(&HasCursor) || HasCursor > 1 ||
			!Float(&pEvent->m_Cursor.x) || !Float(&pEvent->m_Cursor.y) ||
			!U8(&Value) || Value > 1 || !FixedString(pEvent->m_aText, sizeof(pEvent->m_aText)))
			return false;
		pEvent->m_TuningZone = -1;
		pEvent->m_TuningCount = 0;
		if(Type == TAS_TIMELINE_EVENT_TUNING)
		{
			uint32_t Zone, Count;
			if(!U32(&Zone) || !U32(&Count) || Count > TAS_TIMELINE_MAX_TUNING_PARAMS)
				return false;
			pEvent->m_TuningZone = (int32_t)Zone;
			pEvent->m_TuningCount = (int32_t)Count;
			for(int i = 0; i < pEvent->m_TuningCount; ++i)
			{
				uint32_t TuningValue;
				if(!U32(&TuningValue))
					return false;
				pEvent->m_aTuningValues[i] = (int32_t)TuningValue;
			}
		}
		pEvent->m_Type = Type;
		pEvent->m_Tick = (int32_t)Tick;
		pEvent->m_Order = Order;
		pEvent->m_ActiveDummy = (int32_t)ActiveDummy;
		pEvent->m_Team = (int32_t)Team;
		pEvent->m_ClientId = (int32_t)ClientId;
		pEvent->m_HasCursor = HasCursor != 0;
		pEvent->m_Value = Value != 0;
		return true;
	}
	bool String(char *pOut, size_t OutSize)
	{
		uint16_t Length;
		if(!U16(&Length) || m_Offset > m_Size || Length > m_Size - m_Offset)
			return false;
		const size_t CopyLength = std::min((size_t)Length, OutSize ? OutSize - 1 : 0);
		if(CopyLength)
			mem_copy(pOut, m_pData + m_Offset, CopyLength);
		if(OutSize)
			pOut[CopyLength] = '\0';
		m_Offset += Length;
		return true;
	}
	bool String(std::string *pOut)
	{
		uint16_t Length;
		if(!U16(&Length) || m_Offset > m_Size || Length > m_Size - m_Offset)
			return false;
		pOut->assign((const char *)m_pData + m_Offset, Length);
		m_Offset += Length;
		return true;
	}
};

bool CTas::WriteTakeFile(const char *pPath, const char *pMap, const char *pDescription, const char *pAuthor, const char *pCreated)
{
	const std::vector<CTasTick> &vTake = m_Recording ? m_vRecord : m_vSaved;
	if(vTake.empty())
		return false;

	const CTasState &Start = m_Recording ? m_RecordStart : m_SavedStart;
	std::vector<STasStartLoadout> vStart = m_TakeFromFile ? m_vFileStartLoadouts : std::vector<STasStartLoadout>{};
	const auto AddLoadout = [&](int Slot, const CCharacterCore &Core, const CNetObj_PlayerInput &PreviousInput) {
		if(!in_range(Core.m_ActiveWeapon, NUM_WEAPONS - 1))
			return;
		int Mask = 0;
		for(int W = 0; W < NUM_WEAPONS; ++W)
			if(Core.m_aWeapons[W].m_Got)
				Mask |= 1 << W;
		CNetObj_PlayerInput StartInput = PreviousInput;
		KxTasWeaponOverride(StartInput, Core.m_ActiveWeapon);
		vStart.push_back({Slot, Core.m_ActiveWeapon, Mask, StartInput, Core.m_Pos, Core.m_Vel});
	};
	if(!m_TakeFromFile)
	{
		AddLoadout(0, Start.Core, Start.LatestInput);
		for(const STasDummyState &Dummy : Start.vDummies)
			AddLoadout(Dummy.m_Slot, Dummy.m_Core, Dummy.m_Character.m_LatestInput);
	}
	if(vTake.size() > 2000000 || m_vTakeSpawner.size() > 100000 || m_vTakeSpawnerEvents.size() > 100000 ||
		m_vTakeTimelineEvents.size() > 100000 ||
		m_vTakeChat.size() > 100000 || m_vTakeRcon.size() > 100000 || vStart.size() > MAX_DUMMIES ||
		Start.vSpawnedEnts.size() > 100000)
		return false;
	if(Start.Tick < 0 || vTake.size() > (size_t)(INT_MAX - Start.Tick))
		return false;
	const int TakeEndTick = Start.Tick + (int)vTake.size();
	unsigned PreviousTimelineOrder = 0;
	int PreviousTimelineTick = Start.Tick;
	std::vector<std::pair<unsigned, int>> vOrderedEvents;
	vOrderedEvents.reserve(m_vTakeSpawnerEvents.size() + m_vTakeTimelineEvents.size());
	int PreviousSpawnerTick = Start.Tick;
	unsigned PreviousSpawnerOrder = 0;
	for(size_t i = 0; i < m_vTakeSpawnerEvents.size(); ++i)
	{
		const STasSpawnerEvent &Event = m_vTakeSpawnerEvents[i];
		if(Event.m_Type < 0 || Event.m_Type > 2 ||
			Event.m_Record.m_Tick < Start.Tick || Event.m_Record.m_Tick > TakeEndTick ||
			Event.m_Order == UINT_MAX ||
			(i && (Event.m_Record.m_Tick < PreviousSpawnerTick || Event.m_Order <= PreviousSpawnerOrder)))
			return false;
		PreviousSpawnerTick = Event.m_Record.m_Tick;
		PreviousSpawnerOrder = Event.m_Order;
		vOrderedEvents.emplace_back(Event.m_Order, Event.m_Record.m_Tick);
	}
	for(size_t i = 0; i < m_vTakeTimelineEvents.size(); ++i)
	{
		const STasTimelineEvent &Event = m_vTakeTimelineEvents[i];
		if(Event.m_Type < TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type > TAS_TIMELINE_EVENT_TUNING ||
			Event.m_Tick < Start.Tick || Event.m_Tick > TakeEndTick ||
			(i && (Event.m_Tick < PreviousTimelineTick || Event.m_Order <= PreviousTimelineOrder)) ||
			Event.m_ActiveDummy < 0 || Event.m_ActiveDummy >= MAX_DUMMIES ||
			(Event.m_Type == TAS_TIMELINE_EVENT_CHAT_LINE &&
				(Event.m_Team < 0 || Event.m_Team > 1 || Event.m_ClientId < CChat::LUA_MSG || Event.m_ClientId >= MAX_CLIENTS)) ||
			((Event.m_Type == TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type == TAS_TIMELINE_EVENT_RCON_COMMAND) &&
				!Event.m_aText[0]) ||
			Event.m_Order == UINT_MAX ||
			(Event.m_Type == TAS_TIMELINE_EVENT_TUNING &&
				(Event.m_TuningCount < 0 ||
					Event.m_TuningCount > TAS_TIMELINE_MAX_TUNING_PARAMS ||
					(Event.m_TuningCount == 0 && Event.m_TuningZone != -1) ||
					(Event.m_TuningCount > 0 && (Event.m_TuningCount != CTuningParams::Num() ||
						!in_range(Event.m_TuningZone, TuneZone::NUM - 1))))) ||
			(Event.m_HasCursor && Event.m_Type != TAS_TIMELINE_EVENT_CHAT_COMMAND &&
				Event.m_Type != TAS_TIMELINE_EVENT_RCON_COMMAND))
			return false;
		PreviousTimelineTick = Event.m_Tick;
		PreviousTimelineOrder = Event.m_Order;
		vOrderedEvents.emplace_back(Event.m_Order, Event.m_Tick);
	}
	std::sort(vOrderedEvents.begin(), vOrderedEvents.end(),
		[](const std::pair<unsigned, int> &A, const std::pair<unsigned, int> &B) { return A.first < B.first; });
	for(size_t i = 1; i < vOrderedEvents.size(); ++i)
		if(vOrderedEvents[i].first == vOrderedEvents[i - 1].first ||
			vOrderedEvents[i].second < vOrderedEvents[i - 1].second)
			return false;

	CKxTasCompactWriter Writer;
	Writer.m_Data.reserve(std::min<size_t>(512u * 1024u * 1024u,
		128 + vTake.size() * (sizeof(CNetObj_PlayerInput) + 40)));
	Writer.U16(NATIVE_FILE_VERSION); // TAS release version code (30 = v3.0).
	Writer.U32((uint32_t)vTake.size());
	Writer.U32((uint32_t)m_vTakeSpawner.size());
	Writer.U32((uint32_t)m_vTakeChat.size());
	Writer.U32((uint32_t)m_vTakeRcon.size());
	Writer.U32((uint32_t)vStart.size());
	Writer.U8(m_TakePgPractice ? 1 : 0);
	Writer.U8(m_TakePgInvincible ? 1 : 0);
	Writer.U8(m_TakePgSuper ? 1 : 0);
	Writer.U8(m_TakePgPaused ? 1 : 0);
	Writer.U8(m_TakePgSpec ? 1 : 0);
	Writer.U8(m_TakePgTuningDirty ? 1 : 0);
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
		for(int i = 0; i < CTuningParams::Num(); ++i)
			Writer.U32((uint32_t)m_aTakePgTuning[Zone].NetworkArray()[i]);
	const int FinishTicks = TakeFinishTicks();
	if(FinishTicks > (int)vTake.size())
		return false;
	Writer.U32(FinishTicks > 0 ? (uint32_t)FinishTicks : UINT32_MAX);
	Writer.String(pMap);
	Writer.String(pDescription);
	Writer.String(pAuthor);
	Writer.String(pCreated);

	for(const CTasTick &Tick : vTake)
	{
		Writer.Input(Tick.Input);
		Writer.Float(Tick.Core.m_Pos.x);
		Writer.Float(Tick.Core.m_Pos.y);
		Writer.Float(Tick.Core.m_Vel.x);
		Writer.Float(Tick.Core.m_Vel.y);
		Writer.Float(Tick.Core.m_HookPos.x);
		Writer.Float(Tick.Core.m_HookPos.y);
		Writer.U32((uint32_t)Tick.Core.m_HookState);
		Writer.U32((uint32_t)Tick.Core.m_HookTick);
		Writer.U32((uint32_t)Tick.Core.m_ActiveWeapon);
		if(Tick.vDummyInputs.size() > MAX_DUMMIES - 1 || Tick.vDummyHooks.size() > MAX_DUMMIES - 1 ||
			Tick.vProjectiles.size() > UINT32_MAX)
			return false;
		Writer.U8((uint8_t)Tick.vDummyInputs.size());
		for(const STasDummyFrame &Frame : Tick.vDummyInputs)
		{
			if(Frame.m_Slot <= 0 || Frame.m_Slot >= MAX_DUMMIES)
				return false;
			Writer.U8((uint8_t)Frame.m_Slot);
			Writer.Input(Frame.m_Input);
			Writer.Float(Frame.m_Pos.x);
			Writer.Float(Frame.m_Pos.y);
		}
		Writer.U8((uint8_t)Tick.vDummyHooks.size());
		for(const STasDummyHookFrame &Hook : Tick.vDummyHooks)
		{
			if(Hook.m_Slot <= 0 || Hook.m_Slot >= MAX_DUMMIES ||
				Hook.m_HookState < INT8_MIN || Hook.m_HookState > INT8_MAX)
				return false;
			Writer.U8((uint8_t)Hook.m_Slot);
			Writer.U8((uint8_t)(int8_t)Hook.m_HookState);
			Writer.Float(Hook.m_HookPos.x);
			Writer.Float(Hook.m_HookPos.y);
		}
		Writer.U32((uint32_t)Tick.vProjectiles.size());
		for(const CProjectileData &Projectile : Tick.vProjectiles)
			Writer.Projectile(Projectile);
	}

	if(!m_vTakeSpawner.empty())
		for(const STasSpawnerRecord &Record : m_vTakeSpawner)
			Writer.SpawnerRecord(Record);
	if(!m_vTakeChat.empty())
		for(const SPgChatLine &Line : m_vTakeChat)
			Writer.Chat(Line);
	for(const std::string &Line : m_vTakeRcon)
		Writer.String(Line.c_str());
	for(const STasStartLoadout &StartLoadout : vStart)
		Writer.StartLoadout(StartLoadout);
	Writer.U32((uint32_t)Start.Tick);
	Writer.U32((uint32_t)Start.vSpawnedEnts.size());
	for(const SSpawnedEntSnapshot &Snapshot : Start.vSpawnedEnts)
		Writer.SpawnedSnapshot(Snapshot);
	Writer.U32((uint32_t)m_vTakeSpawnerEvents.size());
	for(const STasSpawnerEvent &Event : m_vTakeSpawnerEvents)
	{
		Writer.U8((uint8_t)Event.m_Type);
		Writer.U32(Event.m_Order);
		Writer.SpawnerRecord(Event.m_Record);
	}
	Writer.U32((uint32_t)m_vTakeTimelineEvents.size());
	for(const STasTimelineEvent &Event : m_vTakeTimelineEvents)
	{
		if(Event.m_Type < TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type > TAS_TIMELINE_EVENT_TUNING ||
			Event.m_ActiveDummy < 0 || Event.m_ActiveDummy >= MAX_DUMMIES ||
			(Event.m_Type == TAS_TIMELINE_EVENT_CHAT_LINE &&
				(Event.m_Team < 0 || Event.m_Team > 1 || Event.m_ClientId < CChat::LUA_MSG || Event.m_ClientId >= MAX_CLIENTS)) ||
			((Event.m_Type == TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type == TAS_TIMELINE_EVENT_RCON_COMMAND) &&
				!Event.m_aText[0]) ||
			(Event.m_Type == TAS_TIMELINE_EVENT_TUNING &&
				(Event.m_TuningCount < 0 ||
					Event.m_TuningCount > TAS_TIMELINE_MAX_TUNING_PARAMS ||
					(Event.m_TuningCount == 0 && Event.m_TuningZone != -1) ||
					(Event.m_TuningCount > 0 && (Event.m_TuningCount != CTuningParams::Num() ||
						!in_range(Event.m_TuningZone, TuneZone::NUM - 1))))))
			return false;
		Writer.TimelineEvent(Event);
	}
	if(!Writer.m_Ok || Writer.m_Data.empty() || Writer.m_Data.size() > 512u * 1024u * 1024u ||
		Writer.m_Data.size() > (size_t)INT_MAX)
		return false;

	const int UncompSize = (int)Writer.m_Data.size();
	uLongf CompLen = compressBound((uLong)UncompSize);
	if(CompLen > (uLongf)INT_MAX)
		return false;
	std::vector<unsigned char> vComp((size_t)CompLen);
	if(compress2(vComp.data(), &CompLen, Writer.m_Data.data(), (uLong)UncompSize, Z_BEST_COMPRESSION) != Z_OK)
		return false;
	vComp.resize((size_t)CompLen);
	const int CompSize = (int)CompLen;

	const char aMagic[4] = {'K', 'X', 'T', '3'};
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

	// v6.18: accept both TAS formats. Native KXT images are tried first so a
	// KRX file can never be mistaken for the binary format (different magic).
	bool Ok = LoadTakeMem(pRawData, RawSize);
	if(!Ok)
		Ok = LoadKrxTakeMem(pRawData, RawSize);
	free(pRawData);
	if(Ok)
		str_copy(m_aPickedPath, pPath, sizeof(m_aPickedPath));
	return Ok;
}

bool CTas::LoadCompactTake(const unsigned char *pData, size_t Size)
{
	if(!pData || Size < 4)
		return false;
	CKxTasCompactReader Reader{pData, Size};
	uint16_t VersionCode = 0;
	uint32_t TickCount = 0;
	uint32_t SpawnCount = 0;
	uint32_t ChatCount = 0;
	uint32_t RconCount = 0;
	uint32_t StartCount = 0;
	uint32_t FinishValue = UINT32_MAX;
	if(!Reader.U16(&VersionCode) ||
		!Reader.U32(&TickCount) || !Reader.U32(&SpawnCount) ||
		!Reader.U32(&ChatCount) || !Reader.U32(&RconCount) || !Reader.U32(&StartCount) ||
		VersionCode != NATIVE_FILE_VERSION ||
		TickCount == 0 || TickCount > 2000000 || SpawnCount > 100000 ||
		ChatCount > 100000 || RconCount > 100000 || StartCount > MAX_DUMMIES)
		return false;
	uint8_t aPractice[6];
	for(uint8_t &Value : aPractice)
		if(!Reader.U8(&Value) || Value > 1)
			return false;
	std::vector<CTuningParams> vTuning(TuneZone::NUM);
	for(CTuningParams &Tuning : vTuning)
		for(int i = 0; i < CTuningParams::Num(); ++i)
		{
			uint32_t Value;
			if(!Reader.U32(&Value))
				return false;
			Tuning.NetworkArray()[i] = (int32_t)Value;
		}
	if(!Reader.U32(&FinishValue) ||
		(FinishValue != UINT32_MAX && (FinishValue == 0 || FinishValue > TickCount)))
		return false;

	char aMap[128] = {};
	char aDescription[128] = {};
	char aAuthor[128] = {};
	char aCreated[64] = {};
	if(!Reader.String(aMap, sizeof(aMap)) || !Reader.String(aDescription, sizeof(aDescription)) ||
		!Reader.String(aAuthor, sizeof(aAuthor)) || !Reader.String(aCreated, sizeof(aCreated)))
		return false;
	// Each tick has at least the main input/core plus the dummy, hook and
	// projectile counts. Bound the owning CTasTick allocation before resizing.
	static constexpr size_t MIN_TICK_BYTES = 82;
	if(TickCount > (Reader.m_Size - Reader.m_Offset) / MIN_TICK_BYTES)
		return false;

	std::vector<CTasTick> vNew;
	vNew.resize(TickCount);
	for(uint32_t i = 0; i < TickCount; ++i)
	{
		CTasTick &Tick = vNew[i];
		Tick.Tick = (int)i + 1; // Tick indices are implicit in compact V3.
		if(!Reader.Input(&Tick.Input))
			return false;
		float aCoreFloats[6];
		for(float &Value : aCoreFloats)
			if(!Reader.Float(&Value))
				return false;
		uint32_t aCoreInts[3];
		for(uint32_t &Value : aCoreInts)
			if(!Reader.U32(&Value))
				return false;
		Tick.Core.m_Pos = vec2(aCoreFloats[0], aCoreFloats[1]);
		Tick.Core.m_Vel = vec2(aCoreFloats[2], aCoreFloats[3]);
		Tick.Core.m_HookPos = vec2(aCoreFloats[4], aCoreFloats[5]);
		Tick.Core.m_HookState = (int32_t)aCoreInts[0];
		Tick.Core.m_HookTick = (int32_t)aCoreInts[1];
		Tick.Core.m_ActiveWeapon = (int32_t)aCoreInts[2];

		uint8_t DummyCount = 0;
		if(!Reader.U8(&DummyCount) || DummyCount > MAX_DUMMIES - 1)
			return false;
		Tick.vDummyInputs.resize(DummyCount);
		bool aDummySlots[MAX_DUMMIES] = {};
		for(STasDummyFrame &Frame : Tick.vDummyInputs)
		{
			uint8_t Slot = 0;
			if(!Reader.U8(&Slot) || Slot == 0 || Slot >= MAX_DUMMIES || aDummySlots[Slot] ||
				!Reader.Input(&Frame.m_Input))
				return false;
			Frame.m_Slot = Slot;
			aDummySlots[Slot] = true;
			if(!Reader.Float(&Frame.m_Pos.x) || !Reader.Float(&Frame.m_Pos.y))
				return false;
			if(Slot == 1)
				Tick.DummyInput = Frame.m_Input;
		}

		uint8_t HookCount = 0;
		if(!Reader.U8(&HookCount) || HookCount > MAX_DUMMIES - 1)
			return false;
		Tick.vDummyHooks.resize(HookCount);
		bool aHookSlots[MAX_DUMMIES] = {};
		for(STasDummyHookFrame &Hook : Tick.vDummyHooks)
		{
			uint8_t Slot = 0;
			uint8_t State = 0;
			if(!Reader.U8(&Slot) || Slot == 0 || Slot >= MAX_DUMMIES || aHookSlots[Slot] ||
				!Reader.U8(&State))
				return false;
			Hook.m_Slot = Slot;
			Hook.m_HookState = (int8_t)State;
			aHookSlots[Slot] = true;
			if((Hook.m_HookState != HOOK_RETRACTED &&
					(Hook.m_HookState < HOOK_IDLE || Hook.m_HookState > HOOK_GRABBED)) ||
				!Reader.Float(&Hook.m_HookPos.x) || !Reader.Float(&Hook.m_HookPos.y))
				return false;
		}

		uint32_t ProjectileCount = 0;
		static constexpr size_t COMPACT_PROJECTILE_BYTES = 43;
		if(!Reader.U32(&ProjectileCount) || ProjectileCount > 100000 ||
			Reader.m_Offset > Reader.m_Size ||
			ProjectileCount > (Reader.m_Size - Reader.m_Offset) / COMPACT_PROJECTILE_BYTES)
			return false;
		Tick.vProjectiles.resize(ProjectileCount);
		for(CProjectileData &Projectile : Tick.vProjectiles)
			if(!Reader.Projectile(&Projectile))
				return false;
	}

	std::vector<STasSpawnerRecord> vNewRecords(SpawnCount);
	for(STasSpawnerRecord &Record : vNewRecords)
		if(!Reader.SpawnerRecord(&Record))
			return false;
	std::vector<SPgChatLine> vNewChat(ChatCount);
	for(SPgChatLine &Line : vNewChat)
		if(!Reader.Chat(&Line))
			return false;
	std::vector<std::string> vNewRcon(RconCount);
	for(std::string &Line : vNewRcon)
		if(!Reader.String(&Line))
			return false;
	std::vector<STasStartLoadout> vStart(StartCount);
	for(STasStartLoadout &Start : vStart)
		if(!Reader.StartLoadout(&Start))
			return false;
	uint32_t SnapshotTick, SnapshotCount;
	if(!Reader.U32(&SnapshotTick) || SnapshotTick > (uint32_t)INT_MAX - TickCount ||
		!Reader.U32(&SnapshotCount) || SnapshotCount > 100000 ||
		SnapshotCount > (Reader.m_Size - Reader.m_Offset) / 22)
		return false;
	std::vector<SSpawnedEntSnapshot> vSpawned(SnapshotCount);
	for(SSpawnedEntSnapshot &Snapshot : vSpawned)
		if(!Reader.SpawnedSnapshot(&Snapshot))
			return false;
	uint32_t EventCount;
	if(!Reader.U32(&EventCount) || EventCount > 100000)
		return false;
	std::vector<STasSpawnerEvent> vSpawnerEvents(EventCount);
	unsigned PreviousOrder = 0;
	int PreviousTick = (int32_t)SnapshotTick;
	std::vector<std::pair<unsigned, int>> vEventOrder;
	vEventOrder.reserve(EventCount);
	for(size_t i = 0; i < vSpawnerEvents.size(); ++i)
	{
		STasSpawnerEvent &Event = vSpawnerEvents[i];
		uint8_t Type;
		uint32_t Order;
		if(!Reader.U8(&Type) || Type > 2 || !Reader.U32(&Order) || Order == UINT_MAX ||
			!Reader.SpawnerRecord(&Event.m_Record))
			return false;
		Event.m_Type = Type;
		Event.m_Order = Order;
		if(Event.m_Record.m_Tick < PreviousTick ||
			Event.m_Record.m_Tick > (int32_t)SnapshotTick + (int)TickCount ||
			(i && Order <= PreviousOrder) ||
			(Event.m_Record.m_Mode != TAS_SPAWN_MODE_ENTITY && Event.m_Record.m_Mode != TAS_SPAWN_MODE_TILE) ||
			Event.m_Record.m_Settings.m_Mode != Event.m_Record.m_Mode)
			return false;
		PreviousTick = Event.m_Record.m_Tick;
		PreviousOrder = Order;
		vEventOrder.emplace_back(Order, Event.m_Record.m_Tick);
	}
	uint32_t TimelineEventCount;
	if(!Reader.U32(&TimelineEventCount) || TimelineEventCount > 100000)
		return false;
	std::vector<STasTimelineEvent> vTimelineEvents(TimelineEventCount);
	PreviousOrder = 0;
	PreviousTick = (int32_t)SnapshotTick;
	for(size_t i = 0; i < vTimelineEvents.size(); ++i)
	{
		STasTimelineEvent &Event = vTimelineEvents[i];
		if(!Reader.TimelineEvent(&Event) ||
			Event.m_Type < TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type > TAS_TIMELINE_EVENT_TUNING ||
			Event.m_Tick < (int32_t)SnapshotTick ||
			Event.m_Tick > (int32_t)SnapshotTick + (int)TickCount ||
			Event.m_Order == UINT_MAX ||
			(i && (Event.m_Tick < PreviousTick || Event.m_Order <= PreviousOrder)) ||
			!in_range(Event.m_ActiveDummy, MAX_DUMMIES - 1) ||
			(Event.m_Type == TAS_TIMELINE_EVENT_CHAT_LINE &&
				(Event.m_Team < 0 || Event.m_Team > 1 || Event.m_ClientId < CChat::LUA_MSG || Event.m_ClientId >= MAX_CLIENTS)) ||
			((Event.m_Type == TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type == TAS_TIMELINE_EVENT_RCON_COMMAND) &&
				!Event.m_aText[0]) ||
			(Event.m_Type == TAS_TIMELINE_EVENT_TUNING &&
				(Event.m_TuningCount < 0 ||
					Event.m_TuningCount > TAS_TIMELINE_MAX_TUNING_PARAMS ||
					(Event.m_TuningCount == 0 && Event.m_TuningZone != -1) ||
					(Event.m_TuningCount > 0 && (Event.m_TuningCount != CTuningParams::Num() ||
						!in_range(Event.m_TuningZone, TuneZone::NUM - 1))))))
			return false;
		if(Event.m_HasCursor && Event.m_Type != TAS_TIMELINE_EVENT_CHAT_COMMAND &&
			Event.m_Type != TAS_TIMELINE_EVENT_RCON_COMMAND)
			return false;
		PreviousTick = Event.m_Tick;
		PreviousOrder = Event.m_Order;
		vEventOrder.emplace_back(Event.m_Order, Event.m_Tick);
	}
	std::sort(vEventOrder.begin(), vEventOrder.end(),
		[](const std::pair<unsigned, int> &A, const std::pair<unsigned, int> &B) { return A.first < B.first; });
	for(size_t i = 1; i < vEventOrder.size(); ++i)
		if(vEventOrder[i].first == vEventOrder[i - 1].first ||
			vEventOrder[i].second < vEventOrder[i - 1].second)
			return false;
	if(Reader.m_Offset != Reader.m_Size)
		return false;

	bool aStartSlots[MAX_DUMMIES] = {};
	for(const STasStartLoadout &Loadout : vStart)
	{
		const int Slot = Loadout.m_Slot;
		if(!in_range(Slot, MAX_DUMMIES - 1) || aStartSlots[Slot] ||
			!in_range(Loadout.m_Weapon, NUM_WEAPONS - 1) || Loadout.m_WeaponMask < -1 ||
			Loadout.m_WeaponMask >= (1 << NUM_WEAPONS) || !std::isfinite(Loadout.m_Pos.x) ||
			!std::isfinite(Loadout.m_Pos.y) || !std::isfinite(Loadout.m_Vel.x) ||
			!std::isfinite(Loadout.m_Vel.y))
			return false;
		aStartSlots[Slot] = true;
	}

	m_vFileStartLoadouts = std::move(vStart);
	m_vTasRecordedDummyStarts.clear();
	for(const STasStartLoadout &Start : m_vFileStartLoadouts)
		if(Start.m_Slot > 0)
			m_vTasRecordedDummyStarts.push_back({Start.m_Slot, Start.m_Pos});
	m_vSaved = std::move(vNew);
	m_SavedStart = CTasState{};
	m_SavedStart.Tick = (int32_t)SnapshotTick;
	m_SavedStart.vSpawnedEnts = std::move(vSpawned);
	m_aPickedPath[0] = '\0';
	str_copy(m_aPickedMap, aMap, sizeof(m_aPickedMap));
	str_copy(m_aPickedDescription, aDescription, sizeof(m_aPickedDescription));
	str_copy(m_aPickedAuthor, aAuthor, sizeof(m_aPickedAuthor));
	str_copy(m_aPickedCreated, aCreated, sizeof(m_aPickedCreated));
	m_PickedVersion = NATIVE_FILE_FORMAT_VERSION;
	str_format(m_aPickedVersionCode, sizeof(m_aPickedVersionCode), "%d", NATIVE_FILE_VERSION);
	m_PickedFormat = 0;
	const bool HasRconEvent = std::any_of(vTimelineEvents.begin(), vTimelineEvents.end(),
		[](const STasTimelineEvent &Event) { return Event.m_Type == TAS_TIMELINE_EVENT_RCON_COMMAND; });
	const bool HasPracticeEvent = std::any_of(vTimelineEvents.begin(), vTimelineEvents.end(),
	[](const STasTimelineEvent &Event) {
			return Event.m_Type == TAS_TIMELINE_EVENT_PRACTICE ||
				((Event.m_Type == TAS_TIMELINE_EVENT_CHAT_COMMAND || Event.m_Type == TAS_TIMELINE_EVENT_RCON_COMMAND) && Event.m_Value);
		});
	m_PickedUsedFlags = (vNewRecords.empty() && vSpawnerEvents.empty() && m_SavedStart.vSpawnedEnts.empty() ? 0 : TAS_USED_SPAWNER) |
		(vNewRcon.empty() && !HasRconEvent ? 0 : TAS_USED_RCON) |
		(aPractice[0] || aPractice[1] || aPractice[2] || aPractice[3] || aPractice[4] || HasPracticeEvent ? TAS_USED_PRACTICE : 0);
	m_PickedFinishTicks = FinishValue == UINT32_MAX ? -1 : (int)FinishValue;
	g_Config.m_KxTasFormat = 0;
	m_FilePicked = true;
	m_Playing = false;
	CancelPreparation();
	m_RealPlaying = false;
	m_Paused = false;
	m_PlayIdx = 0;
	m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
	m_TakeFromFile = true;
	m_vTakeSpawner = vNewRecords;
	m_vTakeSpawnerEvents = std::move(vSpawnerEvents);
	m_vTakeTimelineEvents = std::move(vTimelineEvents);
	m_TasTimelineNextOrder = 0;
	for(const auto &Event : vEventOrder)
		m_TasTimelineNextOrder = std::max(m_TasTimelineNextOrder, Event.first + 1);
	m_vTakeChat = vNewChat;
	m_vTakeRcon = vNewRcon;
	GameClient()->m_Chat.TasRefreshVirtualLines();
	if(m_PgVirtualRcon && m_Joined)
	{
		GameClient()->m_GameConsole.ClearRemoteBacklog();
		const int Start = std::max(0, (int)m_vPgRcon.size() - 200);
		for(int i = Start; i < (int)m_vPgRcon.size(); ++i)
			GameClient()->m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, m_vPgRcon[i].c_str());
	}
	m_TakePgPractice = aPractice[0] != 0;
	m_TakePgInvincible = aPractice[1] != 0;
	m_TakePgSuper = aPractice[2] != 0;
	m_TakePgPaused = aPractice[3] != 0;
	m_TakePgSpec = aPractice[4] != 0;
	m_TakePgTuningDirty = aPractice[5] != 0;
	m_HasTakePgStart = true;
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
		m_aTakePgTuning[Zone] = vTuning[Zone];
	m_vSpawnerRecords = std::move(vNewRecords);
	m_vPgChat = std::move(vNewChat);
	m_vPgRcon = std::move(vNewRcon);
	if(m_Joined)
	{
		SpawnerClearLive();
		for(const STasSpawnerRecord &Record : m_vSpawnerRecords)
			if(Record.m_Mode == TAS_SPAWN_MODE_TILE)
				SpawnerSpawnAt(Record.m_Settings, Record.m_Pos, Record.m_Tick, false);
		m_SpawnerHasPrev = !m_vSpawnerRecords.empty();
		if(m_SpawnerHasPrev)
			m_SpawnerPrevPos = m_vSpawnerRecords.back().m_Pos;
	}
	return true;
}

// v6.12: parses a .tas image from memory and loads it into the take buffer.
// Used by the GitHub TAS Load — the file is NOT written to disk, so
// m_aPickedPath stays empty and Save writes a fresh file instead of
// overwriting one.
bool CTas::LoadTakeMem(const void *pRawData, unsigned RawSize)
{
	if(m_Recording || !pRawData || !RawSize)
		return false;

	bool Ok = false;
	do
	{
		const unsigned char *pData = (const unsigned char *)pRawData;
		if(RawSize < 16 || pData[0] != 'K' || pData[1] != 'X' || pData[2] != 'T' || pData[3] < '1' || pData[3] > '3')
			break;
		int UncompSize = 0;
		int CompSize = 0;
		mem_copy(&UncompSize, pData + 4, sizeof(int));
		mem_copy(&CompSize, pData + 8, sizeof(int));
		if(UncompSize <= 0 || UncompSize > 512 * 1024 * 1024 || CompSize <= 0 || (unsigned)CompSize > RawSize - 12)
			break;
		std::vector<unsigned char> vUncomp(UncompSize);
		uLongf DestLen = UncompSize;
		if(uncompress(vUncomp.data(), &DestLen, pData + 12, (uLong)CompSize) != Z_OK ||
			(int)DestLen != UncompSize)
			break;
		if(pData[3] == '3')
		{
			Ok = LoadCompactTake(vUncomp.data(), vUncomp.size());
			break;
		}
		int BinSize = 0;
		mem_copy(&BinSize, vUncomp.data(), sizeof(int));
		if(BinSize < 16 || BinSize > UncompSize - 4)
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
		// v6.12: the v2 format renamed the header field "Name" to "Description"
		// (no fallback for v2 files written before the rename); v1 files keep
		// "Name" and the value is displayed as the description.
		KxTasReadHeader(pText, TextLen - 1, Version >= 2 ? "Description" : "Name", aName, sizeof(aName));

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
			std::vector<STasStartLoadout> vStart;
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
					pSpawn += (size_t)ChatCount * ChatSize;
				}
				if(Version >= 3)
				{
					const unsigned char *pEnd = pBin + BinSize;
					const auto ReadList = [&](auto &vItems) {
						if(pEnd - pSpawn < (int)sizeof(int))
							return false;
						int Count;
						mem_copy(&Count, pSpawn, sizeof(Count));
						pSpawn += sizeof(Count);
						const size_t ItemSize = sizeof(vItems.front());
						if(Count < 0 || Count > MAX_DUMMIES || (size_t)Count > (size_t)(pEnd - pSpawn) / ItemSize)
							return false;
						vItems.resize(Count);
						if(Count > 0)
							mem_copy(vItems.data(), pSpawn, Count * ItemSize);
						pSpawn += Count * ItemSize;
						return true;
					};
					if(!ReadList(vStart))
						break;
					bool Valid = true;
					bool aStartSlots[MAX_DUMMIES]{};
					for(const STasStartLoadout &Loadout : vStart)
					{
						const int Slot = Loadout.m_Slot;
						if(!in_range(Slot, MAX_DUMMIES - 1) || aStartSlots[Slot] ||
							!in_range(Loadout.m_Weapon, NUM_WEAPONS - 1) || Loadout.m_WeaponMask < -1 || Loadout.m_WeaponMask >= (1 << NUM_WEAPONS))
						{
							Valid = false;
							break;
						}
						aStartSlots[Slot] = true;
					}
					for(CTasTick &Tick : vNew)
					{
						if(!Valid || !ReadList(Tick.vDummyInputs))
						{
							Valid = false;
							break;
						}
						bool aSlots[MAX_DUMMIES]{};
						for(const STasDummyFrame &Frame : Tick.vDummyInputs)
						{
							const int Slot = Frame.m_Slot;
							if(Slot <= 0 || Slot >= MAX_DUMMIES || aSlots[Slot] || !std::isfinite(Frame.m_Pos.x) || !std::isfinite(Frame.m_Pos.y))
							{
								Valid = false;
								break;
							}
							aSlots[Slot] = true;
							if(Slot == 1)
								Tick.DummyInput = Frame.m_Input;
						}
						if(!Valid || !ReadList(Tick.vDummyHooks))
						{
							Valid = false;
							break;
						}
						bool aHookSlots[MAX_DUMMIES]{};
						for(const STasDummyHookFrame &Hook : Tick.vDummyHooks)
						{
							const int Slot = Hook.m_Slot;
							if(Slot <= 0 || Slot >= MAX_DUMMIES || aHookSlots[Slot] ||
							(Hook.m_HookState != HOOK_RETRACTED && (Hook.m_HookState < HOOK_IDLE || Hook.m_HookState > HOOK_GRABBED)) ||
								!std::isfinite(Hook.m_HookPos.x) || !std::isfinite(Hook.m_HookPos.y))
							{
								Valid = false;
								break;
							}
							aHookSlots[Slot] = true;
						}
					}
					if(!Valid || pSpawn != pEnd)
						break;
				}
			}

		if(Version < 3 && !vNew.empty() && in_range(vNew.front().Core.m_ActiveWeapon, NUM_WEAPONS - 1))
			vStart.push_back({0, vNew.front().Core.m_ActiveWeapon, -1, {}, vNew.front().Core.m_Pos, {}});
		m_vFileStartLoadouts = std::move(vStart);
		for(const STasStartLoadout &Start : m_vFileStartLoadouts)
			if(Start.m_Slot > 0)
				m_vTasRecordedDummyStarts.push_back({Start.m_Slot, Start.m_Pos});
		m_vSaved = std::move(vNew);
		m_aPickedPath[0] = '\0';
		str_copy(m_aPickedMap, aMap, sizeof(m_aPickedMap));
		str_copy(m_aPickedDescription, aName, sizeof(m_aPickedDescription));
		str_copy(m_aPickedAuthor, aAuthor, sizeof(m_aPickedAuthor));
		str_copy(m_aPickedCreated, aCreated, sizeof(m_aPickedCreated));
		m_PickedVersion = Version; // v6.17: .tas binary header version for the File section
		m_PickedUsedFlags = vNewRecords.empty() ? 0 : TAS_USED_SPAWNER;
		m_PickedFinishTicks = -1;
		char aVersionCode[sizeof(m_aPickedVersionCode)] = {};
		KxTasReadHeader(pText, TextLen - 1, "Version", aVersionCode, sizeof(aVersionCode));
		const size_t CodeLength = str_length(aVersionCode);
		if(CodeLength >= 2 && aVersionCode[0] == '0' + Version &&
			strspn(aVersionCode, "0123456789") == CodeLength)
			str_copy(m_aPickedVersionCode, aVersionCode);
		else
			str_format(m_aPickedVersionCode, sizeof(m_aPickedVersionCode), "%d0", Version);
		m_PickedFormat = 0; // v6.18: native Kinetix .tas
		g_Config.m_KxTasFormat = 0;
		m_FilePicked = true;
		m_Playing = false;
		CancelPreparation();
	m_RealPlaying = false;
		m_Paused = false;
		m_PlayIdx = 0;
		m_RealIdx = 0;
	mem_zero(m_aRealDummyIdx, sizeof(m_aRealDummyIdx));
		if(FileTps >= 1.0f && FileTps <= 50.0f)
			m_Tps = FileTps;
		m_TakeFromFile = true;
		m_HasTakePgStart = false;
		m_SavedStart = CTasState{};
		m_TakePgPractice = m_TakePgInvincible = m_TakePgSuper = false;
		m_TakePgPaused = m_TakePgSpec = m_TakePgTuningDirty = false;
		for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
			m_aTakePgTuning[Zone] = CTuningParams{};
		m_vTakeSpawner = vNewRecords;
		m_vTakeSpawnerEvents.clear();
		m_vTakeTimelineEvents.clear();
		m_TasTimelineNextOrder = 0;
		m_vTakeChat = vNewChat;
		m_vTakeRcon.clear();

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

	return Ok;
}

void CTas::SaveTake(const char *pMap, const char *pDescription, const char *pAuthor)
{
	const std::vector<CTasTick> &vTake = m_Recording ? m_vRecord : m_vSaved;
	if(vTake.empty())
		return;
	if(m_FilePicked && m_TakeFromFile && m_PickedFormat == 0 && m_PickedVersion < 3 && g_Config.m_KxTasFormat == 0)
	{
		dbg_msg("tas", "save refused: cannot overwrite a V1/V2 file with V3 data");
		return;
	}

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
	// v6.12: an in-memory GitHub Load sets m_FilePicked with an empty path —
	// treat it as not file-backed so Save writes a fresh file.
	if(m_FilePicked && m_aPickedPath[0])
	{
		str_copy(aPath, m_aPickedPath, sizeof(aPath));
		str_copy(aCreated, m_aPickedCreated, sizeof(aCreated));
	}
	else
	{
		char aStamp[64];
		str_timestamp_format(aStamp, sizeof(aStamp), "%H-%M_%d-%m-%Y");
		char aRel[512];
		// v6.5: TAS files live in the central Kinetix data folder (save dir;
		// on Android that is the app's external files dir), not next to the
		// binary. The folder is created on startup, this is a safety net.
		str_format(aRel, sizeof(aRel), "Kinetix/TAS/%s_%s.tas", aMapSafe, aStamp);
		Storage()->CreateFolder("Kinetix/TAS", IStorage::TYPE_SAVE);
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, aRel, aPath, sizeof(aPath));
		if(m_TakeFromFile && m_aPickedCreated[0])
			str_copy(aCreated, m_aPickedCreated, sizeof(aCreated));
		else if(m_aRecordCreated[0])
			str_copy(aCreated, m_aRecordCreated, sizeof(aCreated));
		else
			str_timestamp_format(aCreated, sizeof(aCreated), "%H:%M | %d.%m.%Y");
	}

	const bool Krx = g_Config.m_KxTasFormat != 0;
	const bool Written = Krx
		? WriteKrxTakeFile(aPath, pMap ? pMap : "", pDescription ? pDescription : "", pAuthor ? pAuthor : "", aCreated)
		: WriteTakeFile(aPath, pMap ? pMap : "", pDescription ? pDescription : "", pAuthor ? pAuthor : "", aCreated);
	if(!Written)
		return;
	// v6.18: the dropdown always determines the written format — even when an
	// existing file was picked in another format.
	m_PickedFormat = Krx ? 1 : 0;

	if(m_FilePicked)
	{
		str_copy(m_aPickedMap, pMap ? pMap : "", sizeof(m_aPickedMap));
		str_copy(m_aPickedDescription, pDescription ? pDescription : "", sizeof(m_aPickedDescription));
		str_copy(m_aPickedAuthor, pAuthor ? pAuthor : "", sizeof(m_aPickedAuthor));
	}
}

// =============================================================
// GitHub TAS repository — storage/tas in the client repo
// =============================================================
// Same scheme as the Code Execution GitHub Scripts: "Update List" downloads
// ONLY storage/tas/tas.json (metadata); the .tas file itself downloads on
// demand into Kinetix/TAS/.
static const char *const s_pGitHubTasRawBase =
	"https://raw.githubusercontent.com/lothiann/Kinetix-Client/master/storage/tas/";

static std::shared_ptr<IHttpRequest> GitHubTasRequest(const char *pUrl)
{
	std::shared_ptr<IHttpRequest> pReq = HttpGet(pUrl);
	pReq->WriteToMemory();
	pReq->LogProgress(HTTPLOG::FAILURE);
	pReq->FailOnErrorStatus(false);
	pReq->MaxResponseSize(512 * 1024 * 1024);
	pReq->Timeout(CTimeout{4000, 120000, 1, 30});
	return pReq;
}

void CTas::GitHubUpdateList()
{
	if(m_GitHubState == GITHUB_UPDATING)
		return;
	m_aGitHubError[0] = '\0';
	m_pGitHubListJob = GitHubTasRequest((std::string(s_pGitHubTasRawBase) + "tas.json").c_str());
	Http()->Run(m_pGitHubListJob);
	m_GitHubState = GITHUB_UPDATING;
}

void CTas::GitHubLoadItem(int Idx, bool Select)
{
	if(m_GitHubLoadState == GITHUB_LOAD_LOADING || Idx < 0 || Idx >= (int)m_vGitHubItems.size())
		return;
	m_aGitHubLoadError[0] = '\0';
	m_GitHubLoadSelect = Select;
	m_GitHubLoadTarget = m_vGitHubItems[Idx].m_File;
	const std::string Url = std::string(s_pGitHubTasRawBase) + m_GitHubLoadTarget;
	m_pGitHubLoadJob = GitHubTasRequest(Url.c_str());
	Http()->Run(m_pGitHubLoadJob);
	m_GitHubLoadState = GITHUB_LOAD_LOADING;
}

void CTas::GitHubClearLoadDone()
{
	m_GitHubLoadState = GITHUB_LOAD_IDLE;
	m_aGitHubLoadFile[0] = '\0';
	m_GitHubLoadContent.clear();
}

void CTas::GitHubUpdateJobs()
{
	if(m_pGitHubListJob && m_pGitHubListJob->Done())
	{
		std::shared_ptr<IHttpRequest> pReq = m_pGitHubListJob;
		m_pGitHubListJob = nullptr;
		GitHubFinishList(pReq.get());
	}
	if(m_pGitHubLoadJob && m_pGitHubLoadJob->Done())
	{
		std::shared_ptr<IHttpRequest> pReq = m_pGitHubLoadJob;
		m_pGitHubLoadJob = nullptr;
		GitHubFinishLoad(pReq.get());
	}
}

void CTas::GitHubFinishList(IHttpRequest *pReq)
{
	if(pReq->State() != EHttpState::DONE || pReq->StatusCode() != 200)
	{
		m_GitHubState = GITHUB_ERROR;
		str_format(m_aGitHubError, sizeof(m_aGitHubError), "download failed (HTTP %d)", pReq->StatusCode());
		return;
	}
	unsigned char *pData = nullptr;
	size_t Len = 0;
	pReq->Result(&pData, &Len);
	if(!pData || Len == 0)
	{
		m_GitHubState = GITHUB_ERROR;
		str_copy(m_aGitHubError, "empty response", sizeof(m_aGitHubError));
		return;
	}
	json_value *pRoot = JsonParse((const json_char *)pData, Len);
	if(!pRoot || pRoot->type != json_object)
	{
		json_value_free(pRoot);
		m_GitHubState = GITHUB_ERROR;
		str_copy(m_aGitHubError, "invalid tas.json", sizeof(m_aGitHubError));
		return;
	}
	m_vGitHubItems.clear();
	m_GitHubListUpdated.clear();
	m_GitHubLastSupportedVersion.clear();
	auto TextField = [](const json_value *pValue) -> std::string {
		if(!pValue)
			return {};
		if(pValue->type == json_string)
			return json_string_get(pValue);
		if(pValue->type == json_integer)
			return std::to_string(json_int_get(pValue));
		return {};
	};
	auto VersionField = [&](const json_value *pValue) -> std::string {
		if(pValue && pValue->type == json_integer)
		{
			const int Version = json_int_get(pValue);
			return "v" + std::to_string(Version / 10) + "." + std::to_string(Version % 10);
		}
		return TextField(pValue);
	};
	if(const json_value *pField = json_object_get(pRoot, "list_updated"))
		m_GitHubListUpdated = TextField(pField);
	if(const json_value *pField = json_object_get(pRoot, "last_supported_version"))
		m_GitHubLastSupportedVersion = VersionField(pField);
	const json_value *pItems = json_object_get(pRoot, "items");
	if(pItems && pItems->type == json_array)
	{
		for(int i = 0; i < json_array_length(pItems); i++)
		{
			const json_value *pObj = json_array_get(pItems, i);
			if(!pObj || pObj->type != json_object)
				continue;
			SGitHubTas Item;
			auto Field = [&](const char *pName) -> const char * {
				const json_value *pValue = json_object_get(pObj, pName);
				// Null-safe: a missing / non-string key must never crash
				// the client (std::string::operator=(nullptr) does).
				return pValue && pValue->type == json_string ? json_string_get(pValue) : "";
			};
			Item.m_File = Field("file");
			Item.m_Map = Field("map");
			Item.m_Description = Field("description");
			Item.m_Author = Field("author");
			Item.m_Version = VersionField(json_object_get(pObj, "version"));
			Item.m_Dummies = TextField(json_object_get(pObj, "dummies"));
			Item.m_Used = TextField(json_object_get(pObj, "used"));
			Item.m_Ticks = TextField(json_object_get(pObj, "ticks"));
			Item.m_FinishTicks = TextField(json_object_get(pObj, "finish_ticks"));
			Item.m_Updated = Field("updated");
			Item.m_Created = Field("created");
			if(!Item.m_File.empty())
				m_vGitHubItems.push_back(std::move(Item));
		}
	}
	json_value_free(pRoot);
	m_GitHubState = GITHUB_READY;
}

void CTas::GitHubFinishLoad(IHttpRequest *pReq)
{
	if(pReq->State() != EHttpState::DONE || pReq->StatusCode() != 200)
	{
		m_GitHubLoadState = GITHUB_LOAD_ERROR;
		str_format(m_aGitHubLoadError, sizeof(m_aGitHubLoadError), "download failed (HTTP %d)", pReq->StatusCode());
		return;
	}
	unsigned char *pData = nullptr;
	size_t Len = 0;
	pReq->Result(&pData, &Len);
	if(!pData || Len == 0)
	{
		m_GitHubLoadState = GITHUB_LOAD_ERROR;
		str_copy(m_aGitHubLoadError, "empty response", sizeof(m_aGitHubLoadError));
		return;
	}
	// v6.12: Load keeps the .tas image in memory only — the ClickGUI parses
	// it into the take buffer via LoadTakeMem without any disk write.
	// Download does the disk write (Kinetix/TAS).
	str_copy(m_aGitHubLoadFile, m_GitHubLoadTarget.c_str(), sizeof(m_aGitHubLoadFile));
	if(m_GitHubLoadSelect)
	{
		m_GitHubLoadContent.assign((const char *)pData, Len);
		m_GitHubLoadState = GITHUB_LOAD_DONE;
		return;
	}
	Storage()->CreateFolder("Kinetix", IStorage::TYPE_SAVE);
	Storage()->CreateFolder("Kinetix/TAS", IStorage::TYPE_SAVE);
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "Kinetix/TAS/%s", m_GitHubLoadTarget.c_str());
	char aComplete[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, aPath, aComplete, sizeof(aComplete));
	IOHANDLE File = io_open(aComplete, IOFLAG_WRITE);
	if(!File)
	{
		m_GitHubLoadState = GITHUB_LOAD_ERROR;
		str_copy(m_aGitHubLoadError, "failed to write the .tas file", sizeof(m_aGitHubLoadError));
		return;
	}
	io_write(File, pData, Len);
	io_close(File);
	m_GitHubLoadState = GITHUB_LOAD_DONE;
}
