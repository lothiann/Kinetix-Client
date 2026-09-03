#include <game/client/components/kinetix/code_exec/code_exec.h>
#include <game/client/components/chat.h>
#include <game/client/components/camera.h>
#include <game/client/components/controls.h>
#include <game/client/components/effects.h>
#include <game/client/components/flow.h>
#include <game/client/components/emoticon.h>
#include <game/client/components/motd.h>
#include <game/client/components/broadcast.h>
#include <game/client/components/voting.h>
#include <game/client/components/spectator.h>
#include <game/client/components/ghost.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/bot_control.h>
#include <game/client/components/kinetix/irc.h>
#include <game/client/components/skins.h>
#include <game/client/components/particles.h>
#include <game/client/components/sounds.h>
#include <game/client/components/damageind.h>
#include <game/client/components/mapimages.h>
#include <game/client/components/mapsounds.h>
#include <game/client/components/menus.h>
#include <game/client/components/binds.h>
#include <game/client/components/console.h>
#include <game/client/components/infomessages.h>
#include <game/client/components/countryflags.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/statboard.h>
#include <game/client/components/tooltips.h>
#include <game/client/components/debughud.h>
#include <game/client/components/hud.h>
#include <game/client/components/important_alert.h>
#include <game/client/components/race_demo.h>
#include <game/client/components/local_server.h>
#include <game/client/components/nameplates.h>
#include <game/client/components/freezebars.h>
#include <game/client/components/items.h>

#include <game/client/gameclient.h>
#include <game/gamecore.h>
#include <game/collision.h>
#include <game/layers.h>
#include <game/mapitems.h>
#include <game/teamscore.h>
#include <game/client/prediction/gameworld.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/projectile.h>
#include <game/client/prediction/entities/pickup.h>
#include <game/client/prediction/entities/door.h>
#include <game/client/prediction/entities/dragger.h>
#include <game/client/prediction/entities/plasma.h>
#include <engine/shared/config.h>
#include <base/str.h>
#include <engine/console.h>
#include <engine/keys.h>
#include <engine/client/client.h>
#include <base/vmath.h>

#include <lua.hpp>
#include <sol.hpp>

#include <cstdio>
#include <cstring>

// =============================================================
// RegisterRawAPI — ALL DDNet classes, ALL public members/methods
// Direct C++ access through gc (CGameClient) — no wrappers
// =============================================================
void CCodeExec::RegisterRawAPI()
{
        if(!m_pLua) return;
        auto &lua = *m_pLua;

        // ===================== NETWORK OBJECTS =====================

        lua.new_usertype<CNetObj_PlayerInput>("CNetObj_PlayerInput",
                "m_Direction", &CNetObj_PlayerInput::m_Direction,
                "m_TargetX", &CNetObj_PlayerInput::m_TargetX,
                "m_TargetY", &CNetObj_PlayerInput::m_TargetY,
                "m_Jump", &CNetObj_PlayerInput::m_Jump,
                "m_Fire", &CNetObj_PlayerInput::m_Fire,
                "m_Hook", &CNetObj_PlayerInput::m_Hook,
                "m_PlayerFlags", &CNetObj_PlayerInput::m_PlayerFlags,
                "m_WantedWeapon", &CNetObj_PlayerInput::m_WantedWeapon,
                "m_NextWeapon", &CNetObj_PlayerInput::m_NextWeapon,
                "m_PrevWeapon", &CNetObj_PlayerInput::m_PrevWeapon
        );

        lua.new_usertype<CNetObj_Character>("CNetObj_Character",
                "m_Tick", &CNetObj_Character::m_Tick,
                "m_X", &CNetObj_Character::m_X,
                "m_Y", &CNetObj_Character::m_Y,
                "m_VelX", &CNetObj_Character::m_VelX,
                "m_VelY", &CNetObj_Character::m_VelY,
                "m_Angle", &CNetObj_Character::m_Angle,
                "m_Direction", &CNetObj_Character::m_Direction,
                "m_Jumped", &CNetObj_Character::m_Jumped,
                "m_HookedPlayer", &CNetObj_Character::m_HookedPlayer,
                "m_HookState", &CNetObj_Character::m_HookState,
                "m_HookTick", &CNetObj_Character::m_HookTick,
                "m_HookX", &CNetObj_Character::m_HookX,
                "m_HookY", &CNetObj_Character::m_HookY,
                "m_HookDx", &CNetObj_Character::m_HookDx,
                "m_HookDy", &CNetObj_Character::m_HookDy,
                "m_PlayerFlags", &CNetObj_Character::m_PlayerFlags,
                "m_Health", &CNetObj_Character::m_Health,
                "m_Armor", &CNetObj_Character::m_Armor,
                "m_AmmoCount", &CNetObj_Character::m_AmmoCount,
                "m_Weapon", &CNetObj_Character::m_Weapon,
                "m_Emote", &CNetObj_Character::m_Emote,
                "m_AttackTick", &CNetObj_Character::m_AttackTick
        );

        lua.new_usertype<CNetObj_PlayerInfo>("CNetObj_PlayerInfo",
                "m_Local", &CNetObj_PlayerInfo::m_Local,
                "m_ClientId", &CNetObj_PlayerInfo::m_ClientId,
                "m_Team", &CNetObj_PlayerInfo::m_Team,
                "m_Score", &CNetObj_PlayerInfo::m_Score,
                "m_Latency", &CNetObj_PlayerInfo::m_Latency
        );

        lua.new_usertype<CNetObj_GameInfo>("CNetObj_GameInfo",
                "m_GameFlags", &CNetObj_GameInfo::m_GameFlags,
                "m_GameStateFlags", &CNetObj_GameInfo::m_GameStateFlags,
                "m_RoundStartTick", &CNetObj_GameInfo::m_RoundStartTick,
                "m_WarmupTimer", &CNetObj_GameInfo::m_WarmupTimer,
                "m_ScoreLimit", &CNetObj_GameInfo::m_ScoreLimit,
                "m_TimeLimit", &CNetObj_GameInfo::m_TimeLimit,
                "m_RoundNum", &CNetObj_GameInfo::m_RoundNum,
                "m_RoundCurrent", &CNetObj_GameInfo::m_RoundCurrent
        );

        lua.new_usertype<CNetObj_GameData>("CNetObj_GameData",
                "m_TeamscoreRed", &CNetObj_GameData::m_TeamscoreRed,
                "m_TeamscoreBlue", &CNetObj_GameData::m_TeamscoreBlue,
                "m_FlagCarrierRed", &CNetObj_GameData::m_FlagCarrierRed,
                "m_FlagCarrierBlue", &CNetObj_GameData::m_FlagCarrierBlue
        );

        lua.new_usertype<CNetObj_Flag>("CNetObj_Flag",
                "m_X", &CNetObj_Flag::m_X,
                "m_Y", &CNetObj_Flag::m_Y,
                "m_Team", &CNetObj_Flag::m_Team
        );

        lua.new_usertype<CNetObj_SpectatorInfo>("CNetObj_SpectatorInfo",
                "m_SpectatorId", &CNetObj_SpectatorInfo::m_SpectatorId,
                "m_X", &CNetObj_SpectatorInfo::m_X,
                "m_Y", &CNetObj_SpectatorInfo::m_Y
        );

        lua.new_usertype<CNetObj_DDNetCharacter>("CNetObj_DDNetCharacter",
                "m_Flags", &CNetObj_DDNetCharacter::m_Flags,
                "m_FreezeEnd", &CNetObj_DDNetCharacter::m_FreezeEnd,
                "m_Jumps", &CNetObj_DDNetCharacter::m_Jumps,
                "m_TeleCheckpoint", &CNetObj_DDNetCharacter::m_TeleCheckpoint,
                "m_StrongWeakId", &CNetObj_DDNetCharacter::m_StrongWeakId,
                "m_JumpedTotal", &CNetObj_DDNetCharacter::m_JumpedTotal,
                "m_NinjaActivationTick", &CNetObj_DDNetCharacter::m_NinjaActivationTick,
                "m_FreezeStart", &CNetObj_DDNetCharacter::m_FreezeStart,
                "m_TargetX", &CNetObj_DDNetCharacter::m_TargetX,
                "m_TargetY", &CNetObj_DDNetCharacter::m_TargetY
        );

        // ===================== CTuningParams =====================
        lua.new_usertype<CTuningParams>("CTuningParams",
                "m_GroundControlSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_GroundControlSpeed; }, [](CTuningParams &t, float v) { t.m_GroundControlSpeed = v; }),
                "m_GroundControlAccel", sol::property([](CTuningParams &t) -> float { return (float)t.m_GroundControlAccel; }, [](CTuningParams &t, float v) { t.m_GroundControlAccel = v; }),
                "m_GroundFriction", sol::property([](CTuningParams &t) -> float { return (float)t.m_GroundFriction; }, [](CTuningParams &t, float v) { t.m_GroundFriction = v; }),
                "m_GroundJumpImpulse", sol::property([](CTuningParams &t) -> float { return (float)t.m_GroundJumpImpulse; }, [](CTuningParams &t, float v) { t.m_GroundJumpImpulse = v; }),
                "m_AirJumpImpulse", sol::property([](CTuningParams &t) -> float { return (float)t.m_AirJumpImpulse; }, [](CTuningParams &t, float v) { t.m_AirJumpImpulse = v; }),
                "m_AirControlSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_AirControlSpeed; }, [](CTuningParams &t, float v) { t.m_AirControlSpeed = v; }),
                "m_AirControlAccel", sol::property([](CTuningParams &t) -> float { return (float)t.m_AirControlAccel; }, [](CTuningParams &t, float v) { t.m_AirControlAccel = v; }),
                "m_AirFriction", sol::property([](CTuningParams &t) -> float { return (float)t.m_AirFriction; }, [](CTuningParams &t, float v) { t.m_AirFriction = v; }),
                "m_HookLength", sol::property([](CTuningParams &t) -> float { return (float)t.m_HookLength; }, [](CTuningParams &t, float v) { t.m_HookLength = v; }),
                "m_HookFireSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_HookFireSpeed; }, [](CTuningParams &t, float v) { t.m_HookFireSpeed = v; }),
                "m_HookDragAccel", sol::property([](CTuningParams &t) -> float { return (float)t.m_HookDragAccel; }, [](CTuningParams &t, float v) { t.m_HookDragAccel = v; }),
                "m_HookDragSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_HookDragSpeed; }, [](CTuningParams &t, float v) { t.m_HookDragSpeed = v; }),
                "m_Gravity", sol::property([](CTuningParams &t) -> float { return (float)t.m_Gravity; }, [](CTuningParams &t, float v) { t.m_Gravity = v; }),
                "m_VelrampStart", sol::property([](CTuningParams &t) -> float { return (float)t.m_VelrampStart; }, [](CTuningParams &t, float v) { t.m_VelrampStart = v; }),
                "m_VelrampRange", sol::property([](CTuningParams &t) -> float { return (float)t.m_VelrampRange; }, [](CTuningParams &t, float v) { t.m_VelrampRange = v; }),
                "m_VelrampCurvature", sol::property([](CTuningParams &t) -> float { return (float)t.m_VelrampCurvature; }, [](CTuningParams &t, float v) { t.m_VelrampCurvature = v; }),
                "m_GunCurvature", sol::property([](CTuningParams &t) -> float { return (float)t.m_GunCurvature; }, [](CTuningParams &t, float v) { t.m_GunCurvature = v; }),
                "m_GunSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_GunSpeed; }, [](CTuningParams &t, float v) { t.m_GunSpeed = v; }),
                "m_GunLifetime", sol::property([](CTuningParams &t) -> float { return (float)t.m_GunLifetime; }, [](CTuningParams &t, float v) { t.m_GunLifetime = v; }),
                "m_ShotgunCurvature", sol::property([](CTuningParams &t) -> float { return (float)t.m_ShotgunCurvature; }, [](CTuningParams &t, float v) { t.m_ShotgunCurvature = v; }),
                "m_ShotgunSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_ShotgunSpeed; }, [](CTuningParams &t, float v) { t.m_ShotgunSpeed = v; }),
                "m_ShotgunSpeeddiff", sol::property([](CTuningParams &t) -> float { return (float)t.m_ShotgunSpeeddiff; }, [](CTuningParams &t, float v) { t.m_ShotgunSpeeddiff = v; }),
                "m_ShotgunLifetime", sol::property([](CTuningParams &t) -> float { return (float)t.m_ShotgunLifetime; }, [](CTuningParams &t, float v) { t.m_ShotgunLifetime = v; }),
                "m_GrenadeCurvature", sol::property([](CTuningParams &t) -> float { return (float)t.m_GrenadeCurvature; }, [](CTuningParams &t, float v) { t.m_GrenadeCurvature = v; }),
                "m_GrenadeSpeed", sol::property([](CTuningParams &t) -> float { return (float)t.m_GrenadeSpeed; }, [](CTuningParams &t, float v) { t.m_GrenadeSpeed = v; }),
                "m_GrenadeLifetime", sol::property([](CTuningParams &t) -> float { return (float)t.m_GrenadeLifetime; }, [](CTuningParams &t, float v) { t.m_GrenadeLifetime = v; }),
                "m_LaserReach", sol::property([](CTuningParams &t) -> float { return (float)t.m_LaserReach; }, [](CTuningParams &t, float v) { t.m_LaserReach = v; }),
                "m_LaserBounceDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_LaserBounceDelay; }, [](CTuningParams &t, float v) { t.m_LaserBounceDelay = v; }),
                "m_LaserBounceNum", sol::property([](CTuningParams &t) -> float { return (float)t.m_LaserBounceNum; }, [](CTuningParams &t, float v) { t.m_LaserBounceNum = v; }),
                "m_LaserBounceCost", sol::property([](CTuningParams &t) -> float { return (float)t.m_LaserBounceCost; }, [](CTuningParams &t, float v) { t.m_LaserBounceCost = v; }),
                "m_LaserDamage", sol::property([](CTuningParams &t) -> float { return (float)t.m_LaserDamage; }, [](CTuningParams &t, float v) { t.m_LaserDamage = v; }),
                "m_PlayerCollision", sol::property([](CTuningParams &t) -> float { return (float)t.m_PlayerCollision; }, [](CTuningParams &t, float v) { t.m_PlayerCollision = v; }),
                "m_PlayerHooking", sol::property([](CTuningParams &t) -> float { return (float)t.m_PlayerHooking; }, [](CTuningParams &t, float v) { t.m_PlayerHooking = v; }),
                "m_JetpackStrength", sol::property([](CTuningParams &t) -> float { return (float)t.m_JetpackStrength; }, [](CTuningParams &t, float v) { t.m_JetpackStrength = v; }),
                "m_ShotgunStrength", sol::property([](CTuningParams &t) -> float { return (float)t.m_ShotgunStrength; }, [](CTuningParams &t, float v) { t.m_ShotgunStrength = v; }),
                "m_ExplosionStrength", sol::property([](CTuningParams &t) -> float { return (float)t.m_ExplosionStrength; }, [](CTuningParams &t, float v) { t.m_ExplosionStrength = v; }),
                "m_HammerStrength", sol::property([](CTuningParams &t) -> float { return (float)t.m_HammerStrength; }, [](CTuningParams &t, float v) { t.m_HammerStrength = v; }),
                "m_HookDuration", sol::property([](CTuningParams &t) -> float { return (float)t.m_HookDuration; }, [](CTuningParams &t, float v) { t.m_HookDuration = v; }),
                "m_HammerFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_HammerFireDelay; }, [](CTuningParams &t, float v) { t.m_HammerFireDelay = v; }),
                "m_GunFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_GunFireDelay; }, [](CTuningParams &t, float v) { t.m_GunFireDelay = v; }),
                "m_ShotgunFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_ShotgunFireDelay; }, [](CTuningParams &t, float v) { t.m_ShotgunFireDelay = v; }),
                "m_GrenadeFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_GrenadeFireDelay; }, [](CTuningParams &t, float v) { t.m_GrenadeFireDelay = v; }),
                "m_LaserFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_LaserFireDelay; }, [](CTuningParams &t, float v) { t.m_LaserFireDelay = v; }),
                "m_NinjaFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_NinjaFireDelay; }, [](CTuningParams &t, float v) { t.m_NinjaFireDelay = v; }),
                "m_HammerHitFireDelay", sol::property([](CTuningParams &t) -> float { return (float)t.m_HammerHitFireDelay; }, [](CTuningParams &t, float v) { t.m_HammerHitFireDelay = v; }),
                "m_GroundElasticityX", sol::property([](CTuningParams &t) -> float { return (float)t.m_GroundElasticityX; }, [](CTuningParams &t, float v) { t.m_GroundElasticityX = v; }),
                "m_GroundElasticityY", sol::property([](CTuningParams &t) -> float { return (float)t.m_GroundElasticityY; }, [](CTuningParams &t, float v) { t.m_GroundElasticityY = v; }),
                "Set", sol::overload(
                        [](CTuningParams &t, int i, float v) -> bool { return t.Set(i, v); },
                        [](CTuningParams &t, const std::string &n, float v) -> bool { return t.Set(n.c_str(), v); }
                ),
                "Get", sol::overload(
                        [](CTuningParams &t, int i) -> float { float v; t.Get(i, &v); return v; },
                        [](CTuningParams &t, const std::string &n) -> float { float v; t.Get(n.c_str(), &v); return v; }
                ),
                "Name", [](CTuningParams &, int i) -> std::string { return std::string(CTuningParams::Name(i)); },
                "Num", []() -> int { return CTuningParams::Num(); },
                "GetWeaponFireDelay", [](CTuningParams &t, int w) -> float { return t.GetWeaponFireDelay(w); }
        );

        // ===================== CTeamsCore =====================
        lua.new_usertype<CTeamsCore>("CTeamsCore",
                "m_IsDDRace16", &CTeamsCore::m_IsDDRace16,
                "SameTeam", &CTeamsCore::SameTeam,
                "CanKeepHook", &CTeamsCore::CanKeepHook,
                "CanCollide", &CTeamsCore::CanCollide,
                "Team", sol::overload(
                        [](CTeamsCore &t, int id) -> int { return t.Team(id); },
                        [](CTeamsCore &t, int id, int team) { t.Team(id, team); }
                ),
                "Reset", [](CTeamsCore &t) { t.Reset(); },
                "SetSolo", &CTeamsCore::SetSolo,
                "GetSolo", &CTeamsCore::GetSolo
        );

        // ===================== SSwitchers =====================
        lua.new_usertype<SSwitchers>("SSwitchers",
                "m_Initial", &SSwitchers::m_Initial,
                "m_aStatus", sol::property([this, &lua](SSwitchers &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 64; i++) t[i] = s.m_aStatus[i];
                        return t;
                }),
                "m_aEndTick", sol::property([this, &lua](SSwitchers &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 64; i++) t[i] = s.m_aEndTick[i];
                        return t;
                }),
                "m_aType", sol::property([this, &lua](SSwitchers &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 64; i++) t[i] = s.m_aType[i];
                        return t;
                }),
                "m_aLastUpdateTick", sol::property([this, &lua](SSwitchers &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 64; i++) t[i] = s.m_aLastUpdateTick[i];
                        return t;
                })
        );

        // ===================== CWorldCore =====================
        lua.new_usertype<CWorldCore>("CWorldCore",
                "m_apCharacters", sol::property([this, &lua](CWorldCore &w) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++)
                                if(w.m_apCharacters[i])
                                        t[i] = std::ref(*w.m_apCharacters[i]);
                        return t;
                }),
                "m_vSwitchers", sol::property([](CWorldCore &w) -> std::vector<SSwitchers>& { return w.m_vSwitchers; }),
                "RandomOr0", &CWorldCore::RandomOr0,
                "InitSwitchers", &CWorldCore::InitSwitchers
        );

        // ===================== CCharacterCore =====================
        lua.new_usertype<CCharacterCore>("CCharacterCore",
                "m_Pos", sol::property([](CCharacterCore &c) -> vec2& { return c.m_Pos; }),
                "m_Vel", sol::property([](CCharacterCore &c) -> vec2& { return c.m_Vel; }),
                "m_HookPos", sol::property([](CCharacterCore &c) -> vec2& { return c.m_HookPos; }),
                "m_HookDir", sol::property([](CCharacterCore &c) -> vec2& { return c.m_HookDir; }),
                "m_HookTeleBase", sol::property([](CCharacterCore &c) -> vec2& { return c.m_HookTeleBase; }),
                "m_HookTick", &CCharacterCore::m_HookTick,
                "m_HookState", &CCharacterCore::m_HookState,
                "m_HookedPlayer", sol::property(&CCharacterCore::HookedPlayer, &CCharacterCore::SetHookedPlayer),
                "m_ActiveWeapon", &CCharacterCore::m_ActiveWeapon,
                "m_Jumped", &CCharacterCore::m_Jumped,
                "m_JumpedTotal", &CCharacterCore::m_JumpedTotal,
                "m_Jumps", &CCharacterCore::m_Jumps,
                "m_Direction", &CCharacterCore::m_Direction,
                "m_Angle", &CCharacterCore::m_Angle,
                "m_Input", sol::property([](CCharacterCore &c) -> CNetObj_PlayerInput& { return c.m_Input; }),
                "m_TriggeredEvents", &CCharacterCore::m_TriggeredEvents,
                "m_Id", &CCharacterCore::m_Id,
                "m_NewHook", &CCharacterCore::m_NewHook,
                "m_Reset", &CCharacterCore::m_Reset,
                "m_Colliding", &CCharacterCore::m_Colliding,
                "m_LeftWall", &CCharacterCore::m_LeftWall,
                "m_Solo", &CCharacterCore::m_Solo,
                "m_Jetpack", &CCharacterCore::m_Jetpack,
                "m_CollisionDisabled", &CCharacterCore::m_CollisionDisabled,
                "m_EndlessHook", &CCharacterCore::m_EndlessHook,
                "m_EndlessJump", &CCharacterCore::m_EndlessJump,
                "m_HammerHitDisabled", &CCharacterCore::m_HammerHitDisabled,
                "m_GrenadeHitDisabled", &CCharacterCore::m_GrenadeHitDisabled,
                "m_LaserHitDisabled", &CCharacterCore::m_LaserHitDisabled,
                "m_ShotgunHitDisabled", &CCharacterCore::m_ShotgunHitDisabled,
                "m_HookHitDisabled", &CCharacterCore::m_HookHitDisabled,
                "m_Super", &CCharacterCore::m_Super,
                "m_Invincible", &CCharacterCore::m_Invincible,
                "m_HasTelegunGun", &CCharacterCore::m_HasTelegunGun,
                "m_HasTelegunGrenade", &CCharacterCore::m_HasTelegunGrenade,
                "m_HasTelegunLaser", &CCharacterCore::m_HasTelegunLaser,
                "m_FreezeStart", &CCharacterCore::m_FreezeStart,
                "m_FreezeEnd", &CCharacterCore::m_FreezeEnd,
                "m_IsInFreeze", &CCharacterCore::m_IsInFreeze,
                "m_DeepFrozen", &CCharacterCore::m_DeepFrozen,
                "m_LiveFrozen", &CCharacterCore::m_LiveFrozen,
                "m_Tuning", sol::property([](CCharacterCore &c) -> CTuningParams& { return c.m_Tuning; }),
                "m_AttachedPlayers", sol::property([this, &lua](CCharacterCore &c) -> sol::table {
                        auto t = lua.create_table();
                        int idx = 1;
                        for(int pid : c.m_AttachedPlayers) t[idx++] = pid;
                        return t;
                }),
                "m_aWeapons", sol::property([this, &lua](CCharacterCore &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < NUM_WEAPONS; i++)
                        {
                                auto w = lua.create_table();
                                w["m_Ammo"] = c.m_aWeapons[i].m_Ammo;
                                w["m_AmmoRegenStart"] = c.m_aWeapons[i].m_AmmoRegenStart;
                                w["m_Ammocost"] = c.m_aWeapons[i].m_Ammocost;
                                w["m_Got"] = c.m_aWeapons[i].m_Got;
                                t[i] = w;
                        }
                        return t;
                }),
                "m_Ninja_ActivationDir", sol::property([](CCharacterCore &c) -> vec2 { return c.m_Ninja.m_ActivationDir; }),
                "m_Ninja_ActivationTick", sol::property([](CCharacterCore &c) -> int { return c.m_Ninja.m_ActivationTick; }),
                "m_Ninja_CurrentMoveTime", sol::property([](CCharacterCore &c) -> int { return c.m_Ninja.m_CurrentMoveTime; }),
                "m_Ninja_OldVelAmount", sol::property([](CCharacterCore &c) -> int { return c.m_Ninja.m_OldVelAmount; }),
                "PhysicalSize", &CCharacterCore::PhysicalSize,
                "PhysicalSizeVec2", &CCharacterCore::PhysicalSizeVec2,
                "Reset", [](CCharacterCore &c) { c.Reset(); },
                "Tick", [](CCharacterCore &c, bool useInput, sol::optional<bool> deferred) { c.Tick(useInput, deferred.value_or(true)); },
                "TickDeferred", [](CCharacterCore &c) { c.TickDeferred(); },
                "Move", [](CCharacterCore &c) { c.Move(); },
                "Quantize", [](CCharacterCore &c) { c.Quantize(); },
                "Read", [](CCharacterCore &c, CNetObj_CharacterCore &obj) { c.Read(&obj); },
                "Write", [](CCharacterCore &c, CNetObj_CharacterCore &obj) { c.Write(&obj); },
                "ReadDDNet", [](CCharacterCore &c, CNetObj_DDNetCharacter &obj) { c.ReadDDNet(&obj); },
                "Collision", [](CCharacterCore &c) -> CCollision* { return c.Collision(); },
                "SetTeamsCore", [](CCharacterCore &c, CTeamsCore &t) { c.SetTeamsCore(&t); },
                "Clone", [](CCharacterCore &c) -> CCharacterCore { return c; },
                "Init", [](CCharacterCore &c, CWorldCore &world, CCollision &col, CTeamsCore *teams) { c.Init(&world, &col, teams); },
                "SetCoreWorld", [](CCharacterCore &c, CWorldCore &world, CCollision &col, CTeamsCore *teams) { c.SetCoreWorld(&world, &col, teams); }
        );

        // ===================== CClientStats =====================
        lua.new_usertype<CGameClient::CClientStats>("CClientStats",
                "m_Frags", &CGameClient::CClientStats::m_Frags,
                "m_Deaths", &CGameClient::CClientStats::m_Deaths,
                "m_Suicides", &CGameClient::CClientStats::m_Suicides,
                "m_BestSpree", &CGameClient::CClientStats::m_BestSpree,
                "m_CurrentSpree", &CGameClient::CClientStats::m_CurrentSpree,
                "m_FlagGrabs", &CGameClient::CClientStats::m_FlagGrabs,
                "m_FlagCaptures", &CGameClient::CClientStats::m_FlagCaptures,
                "m_aFragsWith", sol::property([this, &lua](CGameClient::CClientStats &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < NUM_WEAPONS; i++) t[i] = s.m_aFragsWith[i];
                        return t;
                }),
                "m_aDeathsFrom", sol::property([this, &lua](CGameClient::CClientStats &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < NUM_WEAPONS; i++) t[i] = s.m_aDeathsFrom[i];
                        return t;
                }),
                "Reset", [](CGameClient::CClientStats &s) { s.Reset(); },
                "IsActive", [](CGameClient::CClientStats &s) -> bool { return s.IsActive(); },
                "JoinGame", [](CGameClient::CClientStats &s, int tick) { s.JoinGame(tick); },
                "JoinSpec", [](CGameClient::CClientStats &s, int tick) { s.JoinSpec(tick); },
                "GetIngameTicks", [](CGameClient::CClientStats &s, int tick) -> int { return s.GetIngameTicks(tick); },
                "GetFPM", [](CGameClient::CClientStats &s, int tick, int speed) -> float { return s.GetFPM(tick, speed); }
        );

        // ===================== CGameInfo =====================
        lua.new_usertype<CGameInfo>("CGameInfo",
                "m_FlagStartsRace", &CGameInfo::m_FlagStartsRace,
                "m_TimeScore", &CGameInfo::m_TimeScore,
                "m_UnlimitedAmmo", &CGameInfo::m_UnlimitedAmmo,
                "m_DDRaceRecordMessage", &CGameInfo::m_DDRaceRecordMessage,
                "m_RaceRecordMessage", &CGameInfo::m_RaceRecordMessage,
                "m_RaceSounds", &CGameInfo::m_RaceSounds,
                "m_AllowEyeWheel", &CGameInfo::m_AllowEyeWheel,
                "m_AllowHookColl", &CGameInfo::m_AllowHookColl,
                "m_AllowZoom", &CGameInfo::m_AllowZoom,
                "m_BugDDRaceGhost", &CGameInfo::m_BugDDRaceGhost,
                "m_BugDDRaceInput", &CGameInfo::m_BugDDRaceInput,
                "m_BugFNGLaserRange", &CGameInfo::m_BugFNGLaserRange,
                "m_BugVanillaBounce", &CGameInfo::m_BugVanillaBounce,
                "m_PredictFNG", &CGameInfo::m_PredictFNG,
                "m_PredictDDRace", &CGameInfo::m_PredictDDRace,
                "m_PredictDDRaceTiles", &CGameInfo::m_PredictDDRaceTiles,
                "m_PredictVanilla", &CGameInfo::m_PredictVanilla,
                "m_EntitiesDDNet", &CGameInfo::m_EntitiesDDNet,
                "m_EntitiesDDRace", &CGameInfo::m_EntitiesDDRace,
                "m_EntitiesRace", &CGameInfo::m_EntitiesRace,
                "m_EntitiesFNG", &CGameInfo::m_EntitiesFNG,
                "m_EntitiesVanilla", &CGameInfo::m_EntitiesVanilla,
                "m_EntitiesBW", &CGameInfo::m_EntitiesBW,
                "m_EntitiesFDDrace", &CGameInfo::m_EntitiesFDDrace,
                "m_Race", &CGameInfo::m_Race,
                "m_Pvp", &CGameInfo::m_Pvp,
                "m_DontMaskEntities", &CGameInfo::m_DontMaskEntities,
                "m_AllowXSkins", &CGameInfo::m_AllowXSkins,
                "m_HudHealthArmor", &CGameInfo::m_HudHealthArmor,
                "m_HudAmmo", &CGameInfo::m_HudAmmo,
                "m_HudDDRace", &CGameInfo::m_HudDDRace,
                "m_NoWeakHookAndBounce", &CGameInfo::m_NoWeakHookAndBounce,
                "m_NoSkinChangeForFrozen", &CGameInfo::m_NoSkinChangeForFrozen,
                "m_DDRaceTeam", &CGameInfo::m_DDRaceTeam,
                "m_PredictEvents", &CGameInfo::m_PredictEvents
        );

        // ===================== CSnapState =====================
        lua.new_usertype<CGameClient::CSnapState::CSpectateInfo>("CSpecInfo",
                "m_Active", &CGameClient::CSnapState::CSpectateInfo::m_Active,
                "m_SpectatorId", &CGameClient::CSnapState::CSpectateInfo::m_SpectatorId,
                "m_UsePosition", &CGameClient::CSnapState::CSpectateInfo::m_UsePosition,
                "m_Position", sol::property([](CGameClient::CSnapState::CSpectateInfo &s) -> vec2& { return s.m_Position; }),
                "m_HasCameraInfo", &CGameClient::CSnapState::CSpectateInfo::m_HasCameraInfo,
                "m_Zoom", &CGameClient::CSnapState::CSpectateInfo::m_Zoom,
                "m_Deadzone", &CGameClient::CSnapState::CSpectateInfo::m_Deadzone,
                "m_FollowFactor", &CGameClient::CSnapState::CSpectateInfo::m_FollowFactor
        );

        lua.new_usertype<CGameClient::CSnapState>("CSnapState",
                "m_LocalClientId", &CGameClient::CSnapState::m_LocalClientId,
                "m_NumPlayers", &CGameClient::CSnapState::m_NumPlayers,
                "m_NumFlags", &CGameClient::CSnapState::m_NumFlags,
                "m_HighestClientId", &CGameClient::CSnapState::m_HighestClientId,
                "m_aTeamSize", sol::property([this, &lua](CGameClient::CSnapState &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 2; i++) t[i] = s.m_aTeamSize[i];
                        return t;
                }),
                "m_SpecInfo", sol::property([](CGameClient::CSnapState &s) -> CGameClient::CSnapState::CSpectateInfo& { return s.m_SpecInfo; }),
                "m_aCharacters", sol::property([this, &lua](CGameClient::CSnapState &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++)
                        {
                                auto ct = lua.create_table();
                                ct["m_Active"] = s.m_aCharacters[i].m_Active;
                                ct["m_Cur"] = sol::make_object(lua, std::ref(s.m_aCharacters[i].m_Cur));
                                ct["m_Prev"] = sol::make_object(lua, std::ref(s.m_aCharacters[i].m_Prev));
                                ct["m_HasExtendedData"] = s.m_aCharacters[i].m_HasExtendedData;
                                ct["m_HasExtendedDisplayInfo"] = s.m_aCharacters[i].m_HasExtendedDisplayInfo;
                                t[i] = ct;
                        }
                        return t;
                }),
                "m_apPlayerInfos", sol::property([this, &lua](CGameClient::CSnapState &s) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++)
                        {
                                if(s.m_apPlayerInfos[i])
                                {
                                        auto pi = lua.create_table();
                                        pi["m_Local"] = s.m_apPlayerInfos[i]->m_Local;
                                        pi["m_ClientId"] = s.m_apPlayerInfos[i]->m_ClientId;
                                        pi["m_Team"] = s.m_apPlayerInfos[i]->m_Team;
                                        pi["m_Score"] = s.m_apPlayerInfos[i]->m_Score;
                                        pi["m_Latency"] = s.m_apPlayerInfos[i]->m_Latency;
                                        t[i] = pi;
                                }
                        }
                        return t;
                }),
                "m_pGameInfoObj", sol::property([](CGameClient::CSnapState &s) -> CNetObj_GameInfo* { return (CNetObj_GameInfo*)s.m_pGameInfoObj; }),
                "m_pGameDataObj", sol::property([](CGameClient::CSnapState &s) -> CNetObj_GameData* { return (CNetObj_GameData*)s.m_pGameDataObj; })
        );

        // ===================== CClientData =====================
        lua.new_usertype<CGameClient::CClientData>("CClientData",
                "m_aName", sol::property([](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aName); }),
                "m_aClan", sol::property([](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aClan); }),
                "m_Country", &CGameClient::CClientData::m_Country,
                "m_aSkinName", sol::property([](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aSkinName); }),
                "m_Team", &CGameClient::CClientData::m_Team,
                "m_Active", &CGameClient::CClientData::m_Active,
                "m_Friend", &CGameClient::CClientData::m_Friend,
                "m_Foe", &CGameClient::CClientData::m_Foe,
                "m_Afk", &CGameClient::CClientData::m_Afk,
                "m_Paused", &CGameClient::CClientData::m_Paused,
                "m_Spec", &CGameClient::CClientData::m_Spec,
                "m_Solo", &CGameClient::CClientData::m_Solo,
                "m_Jetpack", &CGameClient::CClientData::m_Jetpack,
                "m_Super", &CGameClient::CClientData::m_Super,
                "m_Invincible", &CGameClient::CClientData::m_Invincible,
                "m_DeepFrozen", &CGameClient::CClientData::m_DeepFrozen,
                "m_LiveFrozen", &CGameClient::CClientData::m_LiveFrozen,
                "m_CollisionDisabled", &CGameClient::CClientData::m_CollisionDisabled,
                "m_EndlessHook", &CGameClient::CClientData::m_EndlessHook,
                "m_EndlessJump", &CGameClient::CClientData::m_EndlessJump,
                "m_HammerHitDisabled", &CGameClient::CClientData::m_HammerHitDisabled,
                "m_GrenadeHitDisabled", &CGameClient::CClientData::m_GrenadeHitDisabled,
                "m_LaserHitDisabled", &CGameClient::CClientData::m_LaserHitDisabled,
                "m_ShotgunHitDisabled", &CGameClient::CClientData::m_ShotgunHitDisabled,
                "m_HookHitDisabled", &CGameClient::CClientData::m_HookHitDisabled,
                "m_HasTelegunGun", &CGameClient::CClientData::m_HasTelegunGun,
                "m_HasTelegunGrenade", &CGameClient::CClientData::m_HasTelegunGrenade,
                "m_HasTelegunLaser", &CGameClient::CClientData::m_HasTelegunLaser,
                "m_FreezeEnd", &CGameClient::CClientData::m_FreezeEnd,
                "m_AuthLevel", &CGameClient::CClientData::m_AuthLevel,
                "m_UseCustomColor", &CGameClient::CClientData::m_UseCustomColor,
                "m_ColorBody", &CGameClient::CClientData::m_ColorBody,
                "m_ColorFeet", &CGameClient::CClientData::m_ColorFeet,
                "m_Angle", &CGameClient::CClientData::m_Angle,
                "m_Emoticon", &CGameClient::CClientData::m_Emoticon,
                "m_EmoticonStartFraction", &CGameClient::CClientData::m_EmoticonStartFraction,
                "m_EmoticonStartTick", &CGameClient::CClientData::m_EmoticonStartTick,
                "m_ChatIgnore", &CGameClient::CClientData::m_ChatIgnore,
                "m_EmoticonIgnore", &CGameClient::CClientData::m_EmoticonIgnore,
                "m_FinishTimeSeconds", &CGameClient::CClientData::m_FinishTimeSeconds,
                "m_FinishTimeMillis", &CGameClient::CClientData::m_FinishTimeMillis,
                "m_IsPredicted", &CGameClient::CClientData::m_IsPredicted,
                "m_IsPredictedLocal", &CGameClient::CClientData::m_IsPredictedLocal,
                "m_SpecCharPresent", &CGameClient::CClientData::m_SpecCharPresent,
                "m_SpecChar", sol::property([](CGameClient::CClientData &d) -> vec2& { return d.m_SpecChar; }),
                "m_RenderPos", sol::property([](CGameClient::CClientData &d) -> vec2& { return d.m_RenderPos; }),
                "m_Predicted", sol::property([](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_Predicted; }),
                "m_PrevPredicted", sol::property([](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_PrevPredicted; }),
                "m_RenderCur", &CGameClient::CClientData::m_RenderCur,
                "m_RenderPrev", &CGameClient::CClientData::m_RenderPrev,
                "m_aSwitchStates", sol::property([this, &lua](CGameClient::CClientData &d) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 256; i++) t[i] = d.m_aSwitchStates[i];
                        return t;
                }),
                "ClientId", &CGameClient::CClientData::ClientId,
                "Reset", [](CGameClient::CClientData &d) { d.Reset(); },
                "UpdateRenderInfo", [](CGameClient::CClientData &d) { d.UpdateRenderInfo(); },
                "UpdateSkinInfo", [](CGameClient::CClientData &d) { d.UpdateSkinInfo(); }
        );

        // ===================== CCollision =====================
        lua.new_usertype<CCollision>("CCollision",
                "m_HighestSwitchNumber", &CCollision::m_HighestSwitchNumber,
                "Init", [](CCollision &c, CLayers &l) { c.Init(&l); },
                "Unload", &CCollision::Unload,
                "CheckPoint", sol::overload(
                        [](CCollision &c, float x, float y) -> bool { return c.CheckPoint(x, y); },
                        [](CCollision &c, vec2 p) -> bool { return c.CheckPoint(p); }
                ),
                "GetCollisionAt", [](CCollision &c, float x, float y) -> int { return c.GetCollisionAt(x, y); },
                "GetWidth", &CCollision::GetWidth,
                "GetHeight", &CCollision::GetHeight,
                "IntersectLine", [this, &lua](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before; int res = c.IntersectLine(p0, p1, &col, &before);
                        auto t = lua.create_table(); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                },
                "IntersectLineTeleWeapon", [this, &lua](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before; int tele; int res = c.IntersectLineTeleWeapon(p0, p1, &col, &before, &tele);
                        auto t = lua.create_table(); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before; t["TeleNr"] = tele;
                        return t;
                },
                "IntersectLineTeleHook", [this, &lua](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before; int tele; int res = c.IntersectLineTeleHook(p0, p1, &col, &before, &tele);
                        auto t = lua.create_table(); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before; t["TeleNr"] = tele;
                        return t;
                },
                "MovePoint", [this, &lua](CCollision &c, vec2 pos, vec2 vel, float elasticity) -> sol::object {
                        vec2 p = pos, v = vel; int bounces; c.MovePoint(&p, &v, elasticity, &bounces);
                        auto t = lua.create_table(); t["Pos"] = p; t["Vel"] = v; t["Bounces"] = bounces;
                        return t;
                },
                "MoveBox", [this, &lua](CCollision &c, vec2 pos, vec2 vel, vec2 size, vec2 elasticity) -> sol::object {
                        vec2 p = pos, v = vel; bool grounded; c.MoveBox(&p, &v, size, elasticity, &grounded);
                        auto t = lua.create_table(); t["Pos"] = p; t["Vel"] = v; t["Grounded"] = grounded;
                        return t;
                },
                "TestBox", &CCollision::TestBox,
                "IsOnGround", &CCollision::IsOnGround,
                "GetFrontCollisionAt", [](CCollision &c, float x, float y) -> int { return c.GetFrontCollisionAt(x, y); },
                "IntersectNoLaser", [this, &lua](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before; int res = c.IntersectNoLaser(p0, p1, &col, &before);
                        auto t = lua.create_table(); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                },
                "GetPureMapIndex", sol::overload(
                        [](CCollision &c, float x, float y) -> int { return c.GetPureMapIndex(x, y); },
                        [](CCollision &c, vec2 p) -> int { return c.GetPureMapIndex(p); }
                ),
                "GetMapIndex", [](CCollision &c, vec2 p) -> int { return c.GetMapIndex(p); },
                "GetMapIndices", [this, &lua](CCollision &c, vec2 prev, vec2 pos, sol::optional<unsigned> maxIdx) -> sol::object {
                        auto indices = c.GetMapIndices(prev, pos, maxIdx.value_or(0));
                        auto t = lua.create_table();
                        for(size_t i = 0; i < indices.size(); i++) t[i+1] = indices[i];
                        return t;
                },
                "IsSolid", [](CCollision &c, int x, int y) -> int { return c.IsSolid(x, y); },
                "GetTile", &CCollision::GetTile,
                "GetFrontTile", &CCollision::GetFrontTile,
                "GetTileIndex", &CCollision::GetTileIndex,
                "GetFrontTileIndex", &CCollision::GetFrontTileIndex,
                "GetTileFlags", &CCollision::GetTileFlags,
                "GetFrontTileFlags", &CCollision::GetFrontTileFlags,
                "Entity", &CCollision::Entity,
                "IsTeleport", [](CCollision &c, int idx) -> int { return c.IsTeleport(idx); },
                "IsEvilTeleport", &CCollision::IsEvilTeleport,
                "IsSpeedup", [](CCollision &c, int idx) -> bool { return c.IsSpeedup(idx); },
                "IsTune", [](CCollision &c, int idx) -> int { return c.IsTune(idx); },
                "GetSpeedup", [this, &lua](CCollision &c, int idx) -> sol::object {
                        vec2 dir; int force, maxSpeed, type; c.GetSpeedup(idx, &dir, &force, &maxSpeed, &type);
                        auto t = lua.create_table(); t["Dir"] = dir; t["Force"] = force; t["MaxSpeed"] = maxSpeed; t["Type"] = type;
                        return t;
                },
                "GetSwitchType", &CCollision::GetSwitchType,
                "GetSwitchNumber", &CCollision::GetSwitchNumber,
                "GetSwitchDelay", &CCollision::GetSwitchDelay,
                "IsWallJump", &CCollision::IsWallJump,
                "IsTimeCheckpoint", &CCollision::IsTimeCheckpoint,
                "IsFrontTimeCheckpoint", &CCollision::IsFrontTimeCheckpoint,
                "TileExists", &CCollision::TileExists,
                "TileExistsNext", &CCollision::TileExistsNext,
                "GetPos", &CCollision::GetPos,
                "IsNoLaser", [](CCollision &c, int x, int y) -> int { return c.IsNoLaser(x, y); },
                "Layers", [](CCollision &c) -> const CLayers* { return c.Layers(); },
                "GetIndex", sol::overload(
                        [](CCollision &c, int x, int y) -> int { return c.GetIndex(x, y); },
                        [](CCollision &c, vec2 prev, vec2 pos) -> int { return c.GetIndex(prev, pos); }
                ),
                "GetFrontIndex", [](CCollision &c, int x, int y) -> int { return c.GetFrontIndex(x, y); },
                "GetMoveRestrictions", [](CCollision &c, vec2 pos, sol::optional<float> dist) -> int { return c.GetMoveRestrictions(pos, dist.value_or(18.0f)); },
                "IsCheckTeleport", &CCollision::IsCheckTeleport,
                "IsCheckEvilTeleport", &CCollision::IsCheckEvilTeleport,
                "IsTeleportWeapon", &CCollision::IsTeleportWeapon,
                "IsTeleportHook", &CCollision::IsTeleportHook,
                "IsTeleCheckpoint", &CCollision::IsTeleCheckpoint,
                "IsThrough", [](CCollision &c, int x, int y, int ox, int oy, vec2 p0, vec2 p1) -> bool { return c.IsThrough(x, y, ox, oy, p0, p1); },
                "IsHookBlocker", [](CCollision &c, int x, int y, vec2 p0, vec2 p1) -> bool { return c.IsHookBlocker(x, y, p0, p1); },
                "IsFrontNoLaser", [](CCollision &c, int x, int y) -> int { return c.IsFrontNoLaser(x, y); },
                "MoverSpeed", [this, &lua](CCollision &c, int x, int y) -> sol::object {
                        vec2 speed; int res = c.MoverSpeed(x, y, &speed);
                        auto t = lua.create_table(); t["hit"] = res; t["Speed"] = speed;
                        return t;
                },
                "SetCollisionAt", [](CCollision &c, float x, float y, int idx) { c.SetCollisionAt(x, y, idx); },
                "SetDoorCollisionAt", [](CCollision &c, float x, float y, int type, int flags, int number) { c.SetDoorCollisionAt(x, y, (unsigned char)type, (unsigned char)flags, (unsigned char)number); },
                "IntersectNoLaserNoWalls", [this, &lua](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before; int res = c.IntersectNoLaserNoWalls(p0, p1, &col, &before);
                        auto t = lua.create_table(); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                },
                "IntersectAir", [this, &lua](CCollision &c, vec2 p0, vec2 p1) -> sol::object {
                        vec2 col, before; int res = c.IntersectAir(p0, p1, &col, &before);
                        auto t = lua.create_table(); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                },
                "TeleOuts", [this, &lua](CCollision &c, int number) -> sol::table {
                        auto t = lua.create_table();
                        auto &v = c.TeleOuts(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                },
                "TeleIns", [this, &lua](CCollision &c, int number) -> sol::table {
                        auto t = lua.create_table();
                        auto &v = c.TeleIns(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                },
                "TeleCheckOuts", [this, &lua](CCollision &c, int number) -> sol::table {
                        auto t = lua.create_table();
                        auto &v = c.TeleCheckOuts(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                },
                "TeleOthers", [this, &lua](CCollision &c, int number) -> sol::table {
                        auto t = lua.create_table();
                        auto &v = c.TeleOthers(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                },
                "TeleAllSize", [](CCollision &c, int number) -> int { return (int)c.TeleAllSize(number); },
                "TeleAllGet", [](CCollision &c, int number, int offset) -> vec2 { return c.TeleAllGet(number, (size_t)offset); }
        );

        // ===================== ALL COMPONENT CLASSES =====================

        // CCamera
        lua.new_usertype<CCamera>("CCamera",
                "m_Center", sol::property([](CCamera &cam) -> vec2& { return cam.m_Center; }),
                "m_ZoomSet", &CCamera::m_ZoomSet,
                "m_Zooming", &CCamera::m_Zooming,
                "m_Zoom", &CCamera::m_Zoom,
                "m_ZoomSmoothingTarget", &CCamera::m_ZoomSmoothingTarget,
                "m_AutoSpecCameraZooming", &CCamera::m_AutoSpecCameraZooming,
                "m_AutoSpecCamera", &CCamera::m_AutoSpecCamera,
                "m_UserZoomTarget", &CCamera::m_UserZoomTarget,
                "m_DyncamTargetCameraOffset", sol::property([](CCamera &cam) -> vec2& { return cam.m_DyncamTargetCameraOffset; }),
                "SetZoom", [](CCamera &cam, float target, int smooth, bool isUser) { cam.SetZoom(target, smooth, isUser); },
                "ZoomAllowed", [](CCamera &cam) -> bool { return cam.ZoomAllowed(); },
                "Deadzone", [](CCamera &cam) -> int { return cam.Deadzone(); },
                "FollowFactor", [](CCamera &cam) -> int { return cam.FollowFactor(); },
                "CamType", [](CCamera &cam) -> int { return cam.CamType(); },
                "ZoomStepsToValue", &CCamera::ZoomStepsToValue,
                "SpectatingPlayer", [](CCamera &cam) -> bool { return cam.SpectatingPlayer(); },
                "ResetAutoSpecCamera", [](CCamera &cam) { cam.ResetAutoSpecCamera(); }
        );

        // CChat
        lua.new_usertype<CChat>("CChat",
                "IsActive", [](CChat &c) -> bool { return c.IsActive(); },
                "AddLine", [](CChat &c, int cid, int team, const std::string &msg) { c.AddLine(cid, team, msg.c_str()); },
                "Echo", [](CChat &c, const std::string &msg) { c.Echo(msg.c_str()); },
                "SendChat", [](CChat &c, int team, const std::string &msg) { c.SendChat(team, msg.c_str()); },
                "SendChatQueued", [](CChat &c, const std::string &msg) { c.SendChatQueued(msg.c_str()); },
                "EnableMode", [](CChat &c, int team) { c.EnableMode(team); },
                "DisableMode", [](CChat &c) { c.DisableMode(); },
                "ClearLines", [](CChat &c) { c.ClearLines(); },
                "RegisterCommand", [](CChat &c, const std::string &name, const std::string &params, const std::string &help) { c.RegisterCommand(name.c_str(), params.c_str(), help.c_str()); },
                "UnregisterCommand", [](CChat &c, const std::string &name) { c.UnregisterCommand(name.c_str()); },
                "FontSize", [](CChat &c) -> float { return c.FontSize(); }
        );

        // CControls
        lua.new_usertype<CControls>("CControls",
                "m_aInputData", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = std::ref(c.m_aInputData[i]);
                        return t;
                }),
                "m_aLastData", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = std::ref(c.m_aLastData[i]);
                        return t;
                }),
                "m_aMousePos", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aMousePos[i];
                        return t;
                }),
                "m_aTargetPos", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aTargetPos[i];
                        return t;
                }),
                "m_aAmmoCount", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < NUM_WEAPONS; i++) t[i] = c.m_aAmmoCount[i];
                        return t;
                }),
                "m_LastSendTime", &CControls::m_LastSendTime,
                "m_aInputDirectionLeft", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aInputDirectionLeft[i];
                        return t;
                }),
                "m_aInputDirectionRight", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aInputDirectionRight[i];
                        return t;
                }),
                "m_aShowHookColl", sol::property([this, &lua](CControls &c) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aShowHookColl[i];
                        return t;
                }),
                "ResetInput", [](CControls &c, int d) { c.ResetInput(d); },
                "ClampMousePos", [](CControls &c) { c.ClampMousePos(); },
                "GetMinMouseDistance", [](CControls &c) -> float { return c.GetMinMouseDistance(); },
                "GetMaxMouseDistance", [](CControls &c) -> float { return c.GetMaxMouseDistance(); }
        );

        // CEffects
        lua.new_usertype<CEffects>("CEffects",
                "BulletTrail", [](CEffects &e, vec2 p, float a, float t) { e.BulletTrail(p, a, t); },
                "SmokeTrail", [](CEffects &e, vec2 p, vec2 v, float a, float t) { e.SmokeTrail(p, v, a, t); },
                "SkidTrail", [](CEffects &e, vec2 p, vec2 v, int d, float a, float vol) { e.SkidTrail(p, v, d, a, vol); },
                "Explosion", [](CEffects &e, vec2 p, float a) { e.Explosion(p, a); },
                "HammerHit", [](CEffects &e, vec2 p, float a, float vol) { e.HammerHit(p, a, vol); },
                "AirJump", [](CEffects &e, vec2 p, float a, float vol) { e.AirJump(p, a, vol); },
                "DamageIndicator", [](CEffects &e, vec2 p, vec2 d, float a) { e.DamageIndicator(p, d, a); },
                "PlayerSpawn", [](CEffects &e, vec2 p, float a, float vol) { e.PlayerSpawn(p, a, vol); },
                "PlayerDeath", [](CEffects &e, vec2 p, int cid, float a) { e.PlayerDeath(p, cid, a); },
                "PowerupShine", [](CEffects &e, vec2 p, vec2 s, float a) { e.PowerupShine(p, s, a); },
                "FreezingFlakes", [](CEffects &e, vec2 p, vec2 s, float a) { e.FreezingFlakes(p, s, a); },
                "SparkleTrail", [](CEffects &e, vec2 p, float a) { e.SparkleTrail(p, a); },
                "Confetti", [](CEffects &e, vec2 p, float a) { e.Confetti(p, a); }
        );

        // CFlow
        lua.new_usertype<CFlow>("CFlow",
                "Get", [](CFlow &f, vec2 p) -> vec2 { return f.Get(p); },
                "Add", [](CFlow &f, vec2 p, vec2 v, float s) { f.Add(p, v, s); },
                "Update", [](CFlow &f) { f.Update(); }
        );

        // CVoting
        lua.new_usertype<CVoting>("CVoting",
                "CallvoteSpectate", [](CVoting &v, int cid, const std::string &r, bool f) { v.CallvoteSpectate(cid, r.c_str(), f); },
                "CallvoteKick", [](CVoting &v, int cid, const std::string &r, bool f) { v.CallvoteKick(cid, r.c_str(), f); },
                "Vote", [](CVoting &v, int val) { v.Vote(val); },
                "SecondsLeft", [](CVoting &v) -> int { return v.SecondsLeft(); },
                "IsVoting", [](CVoting &v) -> bool { return v.IsVoting(); },
                "TakenChoice", [](CVoting &v) -> int { return v.TakenChoice(); },
                "VoteDescription", [](CVoting &v) -> std::string { return std::string(v.VoteDescription()); },
                "VoteReason", [](CVoting &v) -> std::string { return std::string(v.VoteReason()); },
                "IsReceivingOptions", [](CVoting &v) -> bool { return v.IsReceivingOptions(); },
                "NumOptions", [](CVoting &v) -> int { return v.NumOptions(); },
                "CallvoteOption", [](CVoting &v, int OptionId, const std::string &Reason, sol::optional<bool> Force) { v.CallvoteOption(OptionId, Reason.c_str(), Force.value_or(false)); },
                "RemovevoteOption", [](CVoting &v, int OptionId) { v.RemovevoteOption(OptionId); },
                "AddvoteOption", [](CVoting &v, const std::string &Description, const std::string &Command) { v.AddvoteOption(Description.c_str(), Command.c_str()); }
        );

        // CSpectator
        lua.new_usertype<CSpectator>("CSpectator",
                "Spectate", [](CSpectator &s, int id) { s.Spectate(id); },
                "SpectateClosest", [](CSpectator &s) { s.SpectateClosest(); },
                "IsActive", [](CSpectator &s) -> bool { return s.IsActive(); }
        );

        // CEmoticon
        lua.new_usertype<CEmoticon>("CEmoticon",
                "Emote", [](CEmoticon &e, int t) { e.Emote(t); },
                "EyeEmote", [](CEmoticon &e, int t) { e.EyeEmote(t); },
                "IsActive", [](CEmoticon &e) -> bool { return e.IsActive(); }
        );

        // CMotd
        lua.new_usertype<CMotd>("CMotd",
                "ServerMotd", [](CMotd &m) -> std::string { return std::string(m.ServerMotd()); },
                "ServerMotdUpdateTime", [](CMotd &m) -> int64_t { return m.ServerMotdUpdateTime(); },
                "Clear", [](CMotd &m) { m.Clear(); },
                "IsActive", [](CMotd &m) -> bool { return m.IsActive(); }
        );

        // CBroadcast
        lua.new_usertype<CBroadcast>("CBroadcast",
                "DoBroadcast", [](CBroadcast &b, const std::string &s) { b.DoBroadcast(s.c_str()); }
        );

        // CGhost
        lua.new_usertype<CGhost>("CGhost",
                "m_AllowRestart", &CGhost::m_AllowRestart,
                "FreeSlots", [](CGhost &g) -> int { return g.FreeSlots(); },
                "Load", [](CGhost &g, const std::string &f) { g.Load(f.c_str()); },
                "Unload", [](CGhost &g, int s) { g.Unload(s); },
                "UnloadAll", [](CGhost &g) { g.UnloadAll(); },
                "GetGhostDir", [](CGhost &g) -> std::string { return std::string(g.GetGhostDir()); }
        );

        // CBotControl
        lua.new_usertype<CBotControl>("CBotControl",
                "ActionInput", [](CBotControl &b, const std::string &a, int ms, int d) { b.ActionInput(a.c_str(), ms, d); },
                "ActionAim", [](CBotControl &b, int dx, int dy, int d) { b.ActionAim(dx, dy, d); },
                "ActionOverrideAim", [](CBotControl &b, int x, int y, int d) { b.ActionOverrideAim(x, y, d); },
                "ActionStop", [](CBotControl &b, int d) { b.ActionStop(d); },
                "IsBotActiveJump", [](CBotControl &b, int d) -> bool { return b.IsBotActiveJump(d); },
                "IsBotActiveHook", [](CBotControl &b, int d) -> bool { return b.IsBotActiveHook(d); },
                "IsBotActiveFire", [](CBotControl &b, int d) -> bool { return b.IsBotActiveFire(d); },
                "IsBotActiveMove", [](CBotControl &b, int d) -> bool { return b.IsBotActiveMove(d); },
                "IsBotActiveAim", [](CBotControl &b, int d) -> bool { return b.IsBotActiveAim(d); },
                "IsBotActiveAny", [](CBotControl &b, int d) -> bool { return b.IsBotActiveAny(d); }
        );

        // CBotNet
        // v1.56.171 BUG9: kx_ cvar bindings removed — they are now MACRO_CONFIG_INT
        // globals (g_Config.m_KxAttack etc.), not CBotNet members. Lua scripts access
        // them via the global g_Config usertype instead.
        lua.new_usertype<CBotNet>("CBotNet",
                "m_MapWidth", &CBotNet::m_MapWidth,
                "m_MapHeight", &CBotNet::m_MapHeight,
                "m_MapGridLoaded", &CBotNet::m_MapGridLoaded,
                "m_aLastMapName", sol::property([](CBotNet &b) -> std::string { return std::string(b.m_aLastMapName); }),
                "IsDummyActive", [](CBotNet &b, int d) -> bool { return b.IsDummyActive(d); },
                "ResetDummyInputs", [](CBotNet &b, int d) { b.ResetDummyInputs(d); },
                "IsTileWalkable", [](CBotNet &b, int tx, int ty) -> bool { return b.IsTileWalkable(tx, ty); },
                "IsTileFreeze", [](CBotNet &b, int tx, int ty) -> bool { return b.IsTileFreeze(tx, ty); },
                "ProcessDummy", [](CBotNet &b, int d) { b.ProcessDummy(d); }
        );

        // CIRC
        lua.new_usertype<CIRC>("CIRC",
                "SendIRC", [](CIRC &i, const std::string &s) { i.SendIRC(s.c_str()); },
                "GetClientPrefix", [](CIRC &i, int id) -> std::string { return std::string(i.GetClientPrefix(id)); }
        );

        // CParticles
        lua.new_usertype<CParticles>("CParticles",
                "Add", [](CParticles &p, int group, sol::table part, float timePassed) { /* simplified */ }
        );

        // CDamageInd
        lua.new_usertype<CDamageInd>("CDamageInd",
                "Create", [](CDamageInd &d, vec2 pos, vec2 dir, float alpha) { d.Create(pos, dir, alpha); }
        );

        // CSounds
        lua.new_usertype<CSounds>("CSounds",
                "Play", [](CSounds &s, int ch, int sid, float vol) { s.Play(ch, sid, vol); },
                "PlayAt", [](CSounds &s, int ch, int sid, float vol, vec2 pos) { s.PlayAt(ch, sid, vol, pos); },
                "Stop", [](CSounds &s, int sid) { s.Stop(sid); },
                "IsPlaying", [](CSounds &s, int sid) -> bool { return s.IsPlaying(sid); },
                "ClearQueue", [](CSounds &s) { s.ClearQueue(); },
                "Enqueue", [](CSounds &s, int Channel, int SetId) { s.Enqueue(Channel, SetId); },
                "PlayAndRecord", [](CSounds &s, int Channel, int SetId, float Volume, vec2 Position) { s.PlayAndRecord(Channel, SetId, Volume, Position); }
        );

        // CMenus
        lua.new_usertype<CMenus>("CMenus",
                "m_Dummy", &CMenus::m_Dummy,
                "IsInit", [](CMenus &m) -> bool { return m.IsInit(); },
                "IsActive", [](CMenus &m) -> bool { return m.IsActive(); },
                "SetActive", [](CMenus &m, bool a) { m.SetActive(a); },
                "SetMenuPage", [](CMenus &m, int p) { m.SetMenuPage(p); }
        );

        // CBinds
        lua.new_usertype<CBinds>("CBinds",
                "m_MouseOnAction", &CBinds::m_MouseOnAction,
                "Bind", [](CBinds &b, int key, const std::string &str, bool free, int mod) { b.Bind(key, str.c_str(), free, mod); },
                "Get", [](CBinds &b, int key, int mod) -> std::string { return std::string(b.Get(key, mod)); },
                "SetDefaults", [](CBinds &b) { b.SetDefaults(); },
                "UnbindAll", [](CBinds &b) { b.UnbindAll(); }
        );

        // CGameConsole
        lua.new_usertype<CGameConsole>("CGameConsole",
                "PrintLine", [](CGameConsole &c, int type, const std::string &line) { c.PrintLine(type, line.c_str()); },
                "Toggle", [](CGameConsole &c, int type) { c.Toggle(type); },
                "IsActive", [](CGameConsole &c) -> bool { return c.IsActive(); }
        );

        // CHud
        lua.new_usertype<CHud>("CHud",
                "RenderNinjaBarPos", [](CHud &h, float x, float y, float w, float ht, float progress, float alpha) { h.RenderNinjaBarPos(x, y, w, ht, progress, alpha); }
        );

        // CRaceDemo
        lua.new_usertype<CRaceDemo>("CRaceDemo",
                "m_AllowRestart", &CRaceDemo::m_AllowRestart
        );

        // CLocalServer
        lua.new_usertype<CLocalServer>("CLocalServer",
                "IsServerRunning", [](CLocalServer &l) -> bool { return l.IsServerRunning(); },
                "KillServer", [](CLocalServer &l) { l.KillServer(); }
        );

        // CImportantAlert
        lua.new_usertype<CImportantAlert>("CImportantAlert",
                "IsActive", [](CImportantAlert &a) -> bool { return a.IsActive(); }
        );

        // CScoreboard
        lua.new_usertype<CScoreboard>("CScoreboard",
                "IsActive", [](CScoreboard &s) -> bool { return s.IsActive(); }
        );

        // CStatboard
        lua.new_usertype<CStatboard>("CStatboard",
                "IsActive", [](CStatboard &s) -> bool { return s.IsActive(); }
        );

        // ===================== Map structures =====================
        lua.new_usertype<CTile>("CTile",
                "m_Index", &CTile::m_Index,
                "m_Flags", &CTile::m_Flags,
                "m_Skip", &CTile::m_Skip
        );

        lua.new_usertype<CMapItemLayer>("CMapItemLayer",
                "m_Version", &CMapItemLayer::m_Version,
                "m_Type", &CMapItemLayer::m_Type,
                "m_Flags", &CMapItemLayer::m_Flags
        );

        lua.new_usertype<CMapItemGroup>("CMapItemGroup",
                "m_Version", &CMapItemGroup::m_Version,
                "m_OffsetX", &CMapItemGroup::m_OffsetX,
                "m_OffsetY", &CMapItemGroup::m_OffsetY,
                "m_ParallaxX", &CMapItemGroup::m_ParallaxX,
                "m_ParallaxY", &CMapItemGroup::m_ParallaxY,
                "m_StartLayer", &CMapItemGroup::m_StartLayer,
                "m_NumLayers", &CMapItemGroup::m_NumLayers,
                "m_UseClipping", &CMapItemGroup::m_UseClipping,
                "m_ClipX", &CMapItemGroup::m_ClipX,
                "m_ClipY", &CMapItemGroup::m_ClipY,
                "m_ClipW", &CMapItemGroup::m_ClipW,
                "m_ClipH", &CMapItemGroup::m_ClipH
        );

        lua.new_usertype<CMapItemLayerTilemap>("CMapItemLayerTilemap",
                "m_Layer", &CMapItemLayerTilemap::m_Layer,
                "m_Version", &CMapItemLayerTilemap::m_Version,
                "m_Width", &CMapItemLayerTilemap::m_Width,
                "m_Height", &CMapItemLayerTilemap::m_Height,
                "m_Flags", &CMapItemLayerTilemap::m_Flags,
                "m_Image", &CMapItemLayerTilemap::m_Image,
                "m_Data", &CMapItemLayerTilemap::m_Data,
                "m_Tele", &CMapItemLayerTilemap::m_Tele,
                "m_Speedup", &CMapItemLayerTilemap::m_Speedup,
                "m_Front", &CMapItemLayerTilemap::m_Front,
                "m_Switch", &CMapItemLayerTilemap::m_Switch,
                "m_Tune", &CMapItemLayerTilemap::m_Tune
        );

        lua.new_usertype<CLayers>("CLayers",
                "NumGroups", &CLayers::NumGroups,
                "NumLayers", &CLayers::NumLayers,
                "GameGroup", [](CLayers &l) -> CMapItemGroup* { return l.GameGroup(); },
                "GameLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.GameLayer(); },
                "GetGroup", [](CLayers &l, int Index) -> CMapItemGroup* { return l.GetGroup(Index); },
                "GetLayer", [](CLayers &l, int Index) -> CMapItemLayer* { return l.GetLayer(Index); },
                "TeleLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.TeleLayer(); },
                "SpeedupLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.SpeedupLayer(); },
                "FrontLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.FrontLayer(); },
                "SwitchLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.SwitchLayer(); },
                "TuneLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.TuneLayer(); }
        );

        // ===================== Prediction entities =====================
        lua.new_usertype<CEntity>("CEntity",
                "GetId", &CEntity::GetId,
                "GetPos", [](CEntity &e) -> vec2 { return e.GetPos(); },
                "GetProximityRadius", &CEntity::GetProximityRadius,
                "m_Pos", sol::property([](CEntity &e) -> vec2& { return e.m_Pos; }),
                "m_ProximityRadius", &CEntity::m_ProximityRadius,
                "m_Number", &CEntity::m_Number,
                "m_Layer", &CEntity::m_Layer,
                "m_SnapTicks", &CEntity::m_SnapTicks,
                "m_DestroyTick", &CEntity::m_DestroyTick,
                "GameWorld", [](CEntity &e) -> CGameWorld* { return e.GameWorld(); },
                "Collision", [](CEntity &e) -> CCollision* { return e.Collision(); },
                "GetTuning", [](CEntity &e, int i) -> CTuningParams* { return e.GetTuning(i); },
                "TypeNext", [](CEntity &e) -> CEntity* { return e.TypeNext(); },
                "NextEntity", [](CEntity &e) -> CEntity* { return e.NextEntity(); },
                "CanCollide", &CEntity::CanCollide,
                "Keep", [](CEntity &e) { e.Keep(); },
                "Tick", [](CEntity &e) { e.Tick(); }
        );

        lua.new_usertype<CCharacter>("CCharacter",
                sol::base_classes, sol::bases<CEntity>(),
                "Core", [](CCharacter &c) -> const CCharacterCore* { return c.Core(); }
        );

        lua.new_usertype<CProjectile>("CProjectile",
                sol::base_classes, sol::bases<CEntity>(),
                "GetDirection", [](CProjectile &p) -> vec2 { return p.GetDirection(); },
                "GetOwner", [](CProjectile &p) -> int { return p.GetOwner(); },
                "GetStartTick", [](CProjectile &p) -> int { return p.GetStartTick(); },
                "GetType", [](CProjectile &p) -> int { return p.GetType(); }
        );

        lua.new_usertype<CLaser>("CLaser",
                sol::base_classes, sol::bases<CEntity>(),
                "GetOwner", [](CLaser &l) -> int { return l.GetOwner(); }
        );

        lua.new_usertype<CPickup>("CPickup",
                sol::base_classes, sol::bases<CEntity>(),
                "Type", &CPickup::Type
        );

        // ===================== CSkins =====================
        lua.new_usertype<CSkins>("CSkins",
                "IsFavorite", [](CSkins &s, const std::string &Name) -> bool { return s.IsFavorite(Name.c_str()); },
                "AddFavorite", [](CSkins &s, const std::string &Name) { s.AddFavorite(Name.c_str()); },
                "RemoveFavorite", [](CSkins &s, const std::string &Name) { s.RemoveFavorite(Name.c_str()); },
                "RandomizeSkin", [](CSkins &s, int Dummy) { s.RandomizeSkin(Dummy); },
                "SkinPrefix", [](CSkins &s) -> std::string { return std::string(s.SkinPrefix()); },
                "IsSpecialSkin", [](CSkins &s, const std::string &Name) -> bool { return CSkins::IsSpecialSkin(Name.c_str()); }
        );

        // ===================== CCodeExec =====================
        lua.new_usertype<CCodeExec>("CCodeExec",
                "STATE_IDLE", sol::var(CCodeExec::STATE_IDLE),
                "STATE_RUNNING", sol::var(CCodeExec::STATE_RUNNING),
                "STATE_ERROR", sol::var(CCodeExec::STATE_ERROR),
                "Execute", [](CCodeExec &e, const std::string &Code, sol::optional<std::string> Chunk) -> bool { return e.Execute(Code.c_str(), Chunk.value_or("script").c_str()); },
                "Stop", [](CCodeExec &e) { e.Stop(); },
                "GetState", [](CCodeExec &e) -> int { return (int)e.GetState(); },
                "GetLastError", [](CCodeExec &e) -> std::string { return std::string(e.GetLastError()); },
                "GetLineCount", [](CCodeExec &e) -> int { return e.GetLineCount(); },
                "m_aCodeBuffer", sol::property([](CCodeExec &e) -> std::string { return std::string(e.m_aCodeBuffer); }),
                "SetCodeBuffer", [](CCodeExec &e, const std::string &Code) { str_copy(e.m_aCodeBuffer, Code.c_str(), sizeof(e.m_aCodeBuffer)); },
                "m_EditorActive", &CCodeExec::m_EditorActive,
                "m_CursorPos", &CCodeExec::m_CursorPos,
                "m_SelectionStart", &CCodeExec::m_SelectionStart,
                "m_SelectionEnd", &CCodeExec::m_SelectionEnd,
                "m_ScrollY", &CCodeExec::m_ScrollY
        );

        // ===================== ColorRGBA =====================
        lua.new_usertype<ColorRGBA>("ColorRGBA",
                "r", &ColorRGBA::r,
                "g", &ColorRGBA::g,
                "b", &ColorRGBA::b,
                "a", &ColorRGBA::a
        );

        // ===================== CGameWorld (prediction) =====================
        lua.new_usertype<CGameWorld>("CGameWorld",
                "m_Core", sol::property([](CGameWorld &w) -> CWorldCore& { return w.m_Core; }),
                "m_Teams", sol::property([](CGameWorld &w) -> CTeamsCore& { return w.m_Teams; }),
                "m_GameTick", &CGameWorld::m_GameTick,
                "m_WorldConfig", sol::property([this, &lua](CGameWorld &w) -> sol::table {
                        auto t = lua.create_table();
                        t["m_IsDDRace"] = w.m_WorldConfig.m_IsDDRace;
                        t["m_IsVanilla"] = w.m_WorldConfig.m_IsVanilla;
                        t["m_IsFNG"] = w.m_WorldConfig.m_IsFNG;
                        t["m_InfiniteAmmo"] = w.m_WorldConfig.m_InfiniteAmmo;
                        t["m_PredictTiles"] = w.m_WorldConfig.m_PredictTiles;
                        t["m_PredictFreeze"] = w.m_WorldConfig.m_PredictFreeze;
                        t["m_PredictWeapons"] = w.m_WorldConfig.m_PredictWeapons;
                        t["m_PredictDDRace"] = w.m_WorldConfig.m_PredictDDRace;
                        t["m_IsSolo"] = w.m_WorldConfig.m_IsSolo;
                        t["m_UseTuneZones"] = w.m_WorldConfig.m_UseTuneZones;
                        t["m_BugDDRaceInput"] = w.m_WorldConfig.m_BugDDRaceInput;
                        t["m_NoWeakHookAndBounce"] = w.m_WorldConfig.m_NoWeakHookAndBounce;
                        t["m_PredictEvents"] = w.m_WorldConfig.m_PredictEvents;
                        return t;
                }),
                "GameTick", [](CGameWorld &w) -> int { return w.GameTick(); },
                "GameTickSpeed", [](CGameWorld &w) -> int { return w.GameTickSpeed(); },
                "Collision", sol::overload(
                        [](CGameWorld &w) -> CCollision* { return w.Collision(); },
                        [](CGameWorld &w) -> const CCollision* { return w.Collision(); }
                ),
                "Teams", [](CGameWorld &w) -> CTeamsCore* { return w.Teams(); },
                "Switchers", [](CGameWorld &w) -> std::vector<SSwitchers>& { return w.Switchers(); },
                "FindFirst", [](CGameWorld &w, int type) -> CEntity* { return w.FindFirst(type); },
                "GetEntity", [](CGameWorld &w, int id, int type) -> CEntity* { return w.GetEntity(id, type); },
                "GetCharacterById", [](CGameWorld &w, int id) -> CCharacter* { return w.GetCharacterById(id); },
                "Tick", [](CGameWorld &w) { w.Tick(); },
                "Clear", [](CGameWorld &w) { w.Clear(); },
                "IsLocalTeam", [](CGameWorld &w, int id) -> bool { return w.IsLocalTeam(id); },
                "GlobalTuning", sol::overload(
                        [](CGameWorld &w) -> CTuningParams* { return w.GlobalTuning(); },
                        [](CGameWorld &w) -> const CTuningParams* { return w.GlobalTuning(); }
                ),
                "GetTuning", sol::overload(
                        [](CGameWorld &w, int i) -> CTuningParams* { return w.GetTuning(i); },
                        [](CGameWorld &w, int i) -> const CTuningParams* { return w.GetTuning(i); }
                ),
                "TuningList", sol::overload(
                        [](CGameWorld &w) -> CTuningParams* { return w.TuningList(); },
                        [](CGameWorld &w) -> const CTuningParams* { return w.TuningList(); }
                ),
                "FindEntities", [this, &lua](CGameWorld &w, vec2 pos, float radius, int type, int max) -> sol::table {
                        std::vector<CEntity*> ents(max);
                        int num = w.FindEntities(pos, radius, ents.data(), max, type);
                        auto t = lua.create_table();
                        for(int i = 0; i < num; i++) t[i] = ents[i];
                        return t;
                }
        );

        // ===================== CGameClient (ROOT) =====================
        lua["gc"] = std::ref(*GameClient());

        lua.new_usertype<CGameClient>("CGameClient",
                // Predicted chars
                "m_PredictedChar", sol::property([](CGameClient &gc) -> CCharacterCore& { return gc.m_PredictedChar; }),
                "m_PredictedPrevChar", sol::property([](CGameClient &gc) -> CCharacterCore& { return gc.m_PredictedPrevChar; }),
                "m_LocalCharacterPos", sol::property([](CGameClient &gc) -> vec2& { return gc.m_LocalCharacterPos; }),
                // State
                "m_NewTick", &CGameClient::m_NewTick,
                "m_NewPredictedTick", &CGameClient::m_NewPredictedTick,
                "m_SuppressEvents", &CGameClient::m_SuppressEvents,
                "m_ServerMode", &CGameClient::m_ServerMode,
                "m_DemoSpecId", &CGameClient::m_DemoSpecId,
                "m_ReceivedDDNetPlayer", &CGameClient::m_ReceivedDDNetPlayer,
                "m_ReceivedDDNetPlayerFinishTimes", &CGameClient::m_ReceivedDDNetPlayerFinishTimes,
                "m_ReceivedDDNetPlayerFinishTimesMillis", &CGameClient::m_ReceivedDDNetPlayerFinishTimesMillis,
                // MultiView
                "m_MultiViewTeam", &CGameClient::m_MultiViewTeam,
                "m_MultiViewPersonalZoom", &CGameClient::m_MultiViewPersonalZoom,
                "m_MultiViewActivated", &CGameClient::m_MultiViewActivated,
                "m_MultiViewShowHud", &CGameClient::m_MultiViewShowHud,
                "m_aMultiViewId", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = gc.m_aMultiViewId[i];
                        return t;
                }),
                // Map time
                "m_MapBestTimeSeconds", &CGameClient::m_MapBestTimeSeconds,
                "m_MapBestTimeMillis", &CGameClient::m_MapBestTimeMillis,
                "m_aMapDescription", sol::property([](CGameClient &gc) -> std::string { return std::string(gc.m_aMapDescription); }),
                // Snap
                "m_Snap", sol::property([](CGameClient &gc) -> CGameClient::CSnapState& { return gc.m_Snap; }),
                // Clients
                "m_aClients", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = std::ref(gc.m_aClients[i]);
                        return t;
                }),
                // Stats
                "m_aStats", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = std::ref(gc.m_aStats[i]);
                        return t;
                }),
                // Game info
                "m_GameInfo", sol::property([](CGameClient &gc) -> CGameInfo& { return gc.m_GameInfo; }),
                // Flag drop ticks
                "m_aFlagDropTick", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < 2; i++) t[i] = gc.m_aFlagDropTick[i];
                        return t;
                }),
                // Tuning
                "m_aTuning", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = std::ref(gc.m_aTuning[i]);
                        return t;
                }),
                "m_aLocalTuneZone", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = gc.m_aLocalTuneZone[i];
                        return t;
                }),
                // Input
                "m_aDummyInput", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = std::ref(gc.m_aDummyInput[i]);
                        return t;
                }),
                "m_aHammerInput", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = std::ref(gc.m_aHammerInput[i]);
                        return t;
                }),
                "m_aDummyFire", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = gc.m_aDummyFire[i];
                        return t;
                }),
                "m_aLocalIds", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = gc.m_aLocalIds[i];
                        return t;
                }),
                // Teams
                "m_Teams", sol::property([](CGameClient &gc) -> CTeamsCore& { return gc.m_Teams; }),
                // GameWorlds
                "m_GameWorld", sol::property([](CGameClient &gc) -> CGameWorld& { return gc.m_GameWorld; }),
                "m_PredictedWorld", sol::property([](CGameClient &gc) -> CGameWorld& { return gc.m_PredictedWorld; }),
                "m_PrevPredictedWorld", sol::property([](CGameClient &gc) -> CGameWorld& { return gc.m_PrevPredictedWorld; }),
                // RenderTools / RenderMap
                "m_RenderTools", sol::property([](CGameClient &gc) -> CRenderTools& { return gc.m_RenderTools; }),
                "m_RenderMap", sol::property([](CGameClient &gc) -> CRenderMap& { return gc.m_RenderMap; }),
                // Language
                "m_LanguageChanged", &CGameClient::m_LanguageChanged,
                // Components — ALL of them as references
                "m_Camera", sol::property([](CGameClient &gc) -> CCamera& { return gc.m_Camera; }),
                "m_Chat", sol::property([](CGameClient &gc) -> CChat& { return gc.m_Chat; }),
                "m_Controls", sol::property([](CGameClient &gc) -> CControls& { return gc.m_Controls; }),
                "m_Effects", sol::property([](CGameClient &gc) -> CEffects& { return gc.m_Effects; }),
                "m_Flow", sol::property([](CGameClient &gc) -> CFlow& { return gc.m_Flow; }),
                "m_Voting", sol::property([](CGameClient &gc) -> CVoting& { return gc.m_Voting; }),
                "m_Spectator", sol::property([](CGameClient &gc) -> CSpectator& { return gc.m_Spectator; }),
                "m_Emoticon", sol::property([](CGameClient &gc) -> CEmoticon& { return gc.m_Emoticon; }),
                "m_Motd", sol::property([](CGameClient &gc) -> CMotd& { return gc.m_Motd; }),
                "m_Broadcast", sol::property([](CGameClient &gc) -> CBroadcast& { return gc.m_Broadcast; }),
                "m_Ghost", sol::property([](CGameClient &gc) -> CGhost& { return gc.m_Ghost; }),
                "m_BotControl", sol::property([](CGameClient &gc) -> CBotControl& { return gc.m_BotControl; }),
                "m_BotNet", sol::property([](CGameClient &gc) -> CBotNet& { return gc.m_BotNet; }),
                "m_IRC", sol::property([](CGameClient &gc) -> CIRC& { return gc.m_IRC; }),
                "m_Particles", sol::property([](CGameClient &gc) -> CParticles& { return gc.m_Particles; }),
                "m_Sounds", sol::property([](CGameClient &gc) -> CSounds& { return gc.m_Sounds; }),
                "m_DamageInd", sol::property([](CGameClient &gc) -> CDamageInd& { return gc.m_DamageInd; }),
                "m_Menus", sol::property([](CGameClient &gc) -> CMenus& { return gc.m_Menus; }),
                "m_Binds", sol::property([](CGameClient &gc) -> CBinds& { return gc.m_Binds; }),
                "m_GameConsole", sol::property([](CGameClient &gc) -> CGameConsole& { return gc.m_GameConsole; }),
                "m_Hud", sol::property([](CGameClient &gc) -> CHud& { return gc.m_Hud; }),
                "m_RaceDemo", sol::property([](CGameClient &gc) -> CRaceDemo& { return gc.m_RaceDemo; }),
                "m_LocalServer", sol::property([](CGameClient &gc) -> CLocalServer& { return gc.m_LocalServer; }),
                "m_ImportantAlert", sol::property([](CGameClient &gc) -> CImportantAlert& { return gc.m_ImportantAlert; }),
                "m_Scoreboard", sol::property([](CGameClient &gc) -> CScoreboard& { return gc.m_Scoreboard; }),
                "m_Statboard", sol::property([](CGameClient &gc) -> CStatboard& { return gc.m_Statboard; }),
                // Methods
                "SendKill", [](CGameClient &gc) { gc.SendKill(); },
                "SendSwitchTeam", [](CGameClient &gc, int team) { gc.SendSwitchTeam(team); },
                "Echo", [](CGameClient &gc, const std::string &msg) { gc.Echo(msg.c_str()); },
                "IsTeamPlay", [](CGameClient &gc) -> bool { return gc.IsTeamPlay(); },
                "IsWorldPaused", [](CGameClient &gc) -> bool { return gc.IsWorldPaused(); },
                "IsLocalCharSuper", [](CGameClient &gc) -> bool { return gc.IsLocalCharSuper(); },
                "CurrentRaceTime", [](CGameClient &gc) -> int { return gc.CurrentRaceTime(); },
                "LastRaceTick", [](CGameClient &gc) -> int { return gc.LastRaceTick(); },
                "AntiPingPlayers", [](CGameClient &gc) -> bool { return gc.AntiPingPlayers(); },
                "AntiPingGrenade", [](CGameClient &gc) -> bool { return gc.AntiPingGrenade(); },
                "AntiPingWeapons", [](CGameClient &gc) -> bool { return gc.AntiPingWeapons(); },
                "AntiPingGunfire", [](CGameClient &gc) -> bool { return gc.AntiPingGunfire(); },
                "IsOtherTeam", [](CGameClient &gc, int id) -> bool { return gc.IsOtherTeam(id); },
                "SwitchStateTeam", [](CGameClient &gc) -> int { return gc.SwitchStateTeam(); },
                "IntersectCharacter", [this, &lua](CGameClient &gc, vec2 hookPos, vec2 newPos, int ownId) -> sol::object {
                        vec2 newPos2, playerPos;
                        int id = gc.IntersectCharacter(hookPos, newPos, newPos2, ownId, &playerPos);
                        auto t = lua.create_table();
                        t["ClientId"] = id;
                        t["NewPos2"] = newPos2;
                        t["PlayerPosition"] = playerPos;
                        return t;
                },
                "DummyResetInput", [](CGameClient &gc) { gc.DummyResetInput(); },
                "OnReset", [](CGameClient &gc) { gc.OnReset(); },
                "Version", [](CGameClient &gc) -> const char* { return gc.Version(); },
                "DDNetVersion", [](CGameClient &gc) -> int { return gc.DDNetVersion(); },
                "Collision", sol::overload(
                        [](CGameClient &gc) -> CCollision* { return gc.Collision(); },
                        [](CGameClient &gc) -> const CCollision* { return gc.Collision(); }
                ),
                "Switchers", [](CGameClient &gc) -> std::vector<SSwitchers>& { return gc.Switchers(); },
                "PredSwitchers", [](CGameClient &gc) -> std::vector<SSwitchers>& { return gc.PredSwitchers(); },
                "GetTuning", [](CGameClient &gc, int i) -> const CTuningParams* { return gc.GetTuning(i); },
                "GetDDTeamColor", [](CGameClient &gc, int team, float light) -> ColorRGBA { return gc.GetDDTeamColor(team, light); },
                "Layers", [](CGameClient &gc) -> CLayers* { return gc.Layers(); },
                "Config", [](CGameClient &gc) -> CConfig* { return gc.Config(); },
                "GetItemName", [](CGameClient &gc, int Type) -> std::string { const char *p = gc.GetItemName(Type); return p ? std::string(p) : std::string(); },
                "NetVersion", [](CGameClient &gc) -> std::string { return std::string(gc.NetVersion()); },
                "DDNetVersionStr", [](CGameClient &gc) -> std::string { return std::string(gc.DDNetVersionStr()); },
                "ClientVersion7", [](CGameClient &gc) -> int { return gc.ClientVersion7(); },
                "IsDemoPlaybackPaused", [](CGameClient &gc) -> bool { return gc.IsDemoPlaybackPaused(); },
                "GetAnimationPlaybackSpeed", [](CGameClient &gc) -> float { return gc.GetAnimationPlaybackSpeed(); },
                "Predict", [](CGameClient &gc) -> bool { return gc.Predict(); },
                "PredictDummy", [](CGameClient &gc) -> bool { return gc.PredictDummy(); },
                "ComponentCount", [](CGameClient &gc) -> int { return (int)gc.ComponentCount(); },
                "SendStartInfo7", [](CGameClient &gc, int Dummy) { gc.SendStartInfo7(Dummy); },
                "SendSkinChange7", [](CGameClient &gc, int Dummy) { gc.SendSkinChange7(Dummy); },
                "SendReadyChange7", [](CGameClient &gc) { gc.SendReadyChange7(); },
                "GotWantedSkin7", [](CGameClient &gc, int Dummy) -> bool { return gc.GotWantedSkin7(Dummy); },
                "LoadGameSkin", [](CGameClient &gc, const std::string &Path, sol::optional<bool> AsDir) { gc.LoadGameSkin(Path.c_str(), AsDir.value_or(false)); },
                "LoadEmoticonsSkin", [](CGameClient &gc, const std::string &Path, sol::optional<bool> AsDir) { gc.LoadEmoticonsSkin(Path.c_str(), AsDir.value_or(false)); },
                "LoadParticlesSkin", [](CGameClient &gc, const std::string &Path, sol::optional<bool> AsDir) { gc.LoadParticlesSkin(Path.c_str(), AsDir.value_or(false)); },
                "LoadHudSkin", [](CGameClient &gc, const std::string &Path, sol::optional<bool> AsDir) { gc.LoadHudSkin(Path.c_str(), AsDir.value_or(false)); },
                "LoadExtrasSkin", [](CGameClient &gc, const std::string &Path, sol::optional<bool> AsDir) { gc.LoadExtrasSkin(Path.c_str(), AsDir.value_or(false)); },
                "ResetMultiView", [](CGameClient &gc) { gc.ResetMultiView(); },
                "FindFirstMultiViewId", [](CGameClient &gc) -> int { return gc.FindFirstMultiViewId(); },
                "m_InfoMessages", sol::property([](CGameClient &gc) -> CInfoMessages& { return gc.m_InfoMessages; }),
                "m_Censor", sol::property([](CGameClient &gc) -> CCensor& { return gc.m_Censor; }),
                "m_CodeExec", sol::property([](CGameClient &gc) -> CCodeExec& { return gc.m_CodeExec; }),
                "m_ClickGui", sol::property([](CGameClient &gc) -> CClickGui& { return gc.m_ClickGui; }),
                "m_AimBot", sol::property([](CGameClient &gc) -> CAimBot& { return gc.m_AimBot; }),
                "m_BasicAvoidFreeze", sol::property([](CGameClient &gc) -> CBasicAvoidFreeze& { return gc.m_BasicAvoidFreeze; }),
                "m_Esp", sol::property([](CGameClient &gc) -> CEsp& { return gc.m_Esp; }),
                "m_Trajectory", sol::property([](CGameClient &gc) -> CTrajectory& { return gc.m_Trajectory; }),
                "m_KinetixLines", sol::property([](CGameClient &gc) -> CKinetixLines& { return gc.m_KinetixLines; }),
                "m_KeyBinder", sol::property([](CGameClient &gc) -> CKeyBinder& { return gc.m_KeyBinder; }),
                "m_Skins", sol::property([](CGameClient &gc) -> CSkins& { return gc.m_Skins; }),
                "m_Skins7", sol::property([](CGameClient &gc) -> CSkins7& { return gc.m_Skins7; }),
                "m_CountryFlags", sol::property([](CGameClient &gc) -> CCountryFlags& { return gc.m_CountryFlags; }),
                "m_DebugHud", sol::property([](CGameClient &gc) -> CDebugHud& { return gc.m_DebugHud; }),
                "m_TouchControls", sol::property([](CGameClient &gc) -> CTouchControls& { return gc.m_TouchControls; }),
                "m_Players", sol::property([](CGameClient &gc) -> CPlayers& { return gc.m_Players; }),
                "m_NamePlates", sol::property([](CGameClient &gc) -> CNamePlates& { return gc.m_NamePlates; }),
                "m_FreezeBars", sol::property([](CGameClient &gc) -> CFreezeBars& { return gc.m_FreezeBars; }),
                "m_Items", sol::property([](CGameClient &gc) -> CItems& { return gc.m_Items; }),
                "m_MapImages", sol::property([](CGameClient &gc) -> CMapImages& { return gc.m_MapImages; }),
                "m_MapLayersBackground", sol::property([](CGameClient &gc) -> CMapLayers& { return gc.m_MapLayersBackground; }),
                "m_MapLayersForeground", sol::property([](CGameClient &gc) -> CMapLayers& { return gc.m_MapLayersForeground; }),
                "m_Background", sol::property([](CGameClient &gc) -> CBackground& { return gc.m_Background; }),
                "m_MenuBackground", sol::property([](CGameClient &gc) -> CMenuBackground& { return gc.m_MenuBackground; }),
                "m_MapSounds", sol::property([](CGameClient &gc) -> CMapSounds& { return gc.m_MapSounds; }),
                "m_Tooltips", sol::property([](CGameClient &gc) -> CTooltips& { return gc.m_Tooltips; }),
                "m_aReceivedTuning", sol::property([this, &lua](CGameClient &gc) -> sol::table {
                        auto t = lua.create_table();
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = std::ref(gc.m_aReceivedTuning[i]);
                        return t;
                })
        );

        // ===================== IClient namespace (cl) =====================
        auto cl = lua["cl"].get_or_create<sol::table>();
        cl.set_function("Connect", [this](const std::string &addr) { Client()->Connect(addr.c_str()); });
        cl.set_function("Disconnect", [this]() { Client()->Disconnect(); });
        cl.set_function("EnterGame", [this]() { Client()->EnterGame(0); });
        cl.set_function("RconAuth", [this](const std::string &user, const std::string &pass) { Client()->RconAuth(user.c_str(), pass.c_str(), 0); });
        cl.set_function("Rcon", [this](const std::string &cmd) { Client()->Rcon(cmd.c_str()); });
        cl.set_function("RconAuthed", [this]() -> bool { return Client()->RconAuthed(); });
        cl.set_function("GameTick", [this]() -> int { return Client()->GameTick(0); });
        cl.set_function("PrevGameTick", [this]() -> int { return Client()->PrevGameTick(0); });
        cl.set_function("GameTickSpeed", [this]() -> int { return Client()->GameTickSpeed(); });
        cl.set_function("LocalTime", [this]() -> float { return Client()->LocalTime(); });
        cl.set_function("RenderFrameTime", [this]() -> float { return Client()->RenderFrameTime(); });
        cl.set_function("ConnectionProblems", [this]() -> bool { return Client()->ConnectionProblems(); });
        cl.set_function("DummyConnect", [this](int d) { Client()->DummyConnect(d); });
        cl.set_function("DummyDisconnect", [this](int d) { Client()->DummyDisconnect(d, "Lua"); });
        cl.set_function("DummyConnected", [this](int d) -> bool { return Client()->DummyConnected(d); });
        cl.set_function("DummyCount", [this]() -> int { return Client()->DummyCount(); });
        cl.set_function("Quit", [this]() { Client()->Quit(); });
        cl.set_function("Restart", [this]() { Client()->Restart(); });
        cl.set_function("PlayerName", [this]() -> std::string { return std::string(Client()->PlayerName()); });
        cl.set_function("State", [this]() -> int { return (int)Client()->State(); });
        cl.set_function("MapDownloadName", [this]() -> std::string { return std::string(Client()->MapDownloadName()); });
        cl.set_function("MapDownloadAmount", [this]() -> int { return Client()->MapDownloadAmount(); });
        cl.set_function("MapDownloadTotalsize", [this]() -> int { return Client()->MapDownloadTotalsize(); });
        cl.set_function("PredGameTick", [this]() -> int { return Client()->PredGameTick(0); });
        cl.set_function("IntraGameTick", [this]() -> float { return Client()->IntraGameTick(0); });
        cl.set_function("GameTickTime", [this]() -> float { return Client()->GameTickTime(0); });
        cl.set_function("GlobalTime", [this]() -> float { return Client()->GlobalTime(); });
        cl.set_function("DummyAllowed", [this]() -> bool { return Client()->DummyAllowed(); });
        cl.set_function("AnyDummyConnected", [this]() -> bool { return Client()->AnyDummyConnected(); });
        cl.set_function("LastDummy", [this]() -> int { return Client()->LastDummy(); });
        cl.set_function("DummyName", [this]() -> std::string { return std::string(Client()->DummyName()); });
        cl.set_function("ErrorString", [this]() -> std::string { return std::string(Client()->ErrorString()); });
        cl.set_function("News", [this]() -> std::string { return std::string(Client()->News()); });
        cl.set_function("Points", [this]() -> int { return Client()->Points(); });
        cl.set_function("ConnectAddress", [this]() -> std::string { return std::string(Client()->ConnectAddressString()); });
        cl.set_function("PredictionTime", [this]() -> int { return Client()->GetPredictionTime(); });
        cl.set_function("AutoScreenshot", [this]() { Client()->AutoScreenshot_Start(); });
        cl.set_function("Notify", [this](const std::string &Title, const std::string &Msg) { Client()->Notify(Title.c_str(), Msg.c_str()); });
        cl.set_function("IsSixup", [this]() -> bool { return Client()->IsSixup(); });

        lua["config"] = &g_Config;
        lua.new_usertype<CConfig>("CConfig",
                "m_ClDummy", &CConfig::m_ClDummy,
                "m_ClDummyControl", &CConfig::m_ClDummyControl,
                "m_ClDummyCopyMoves", &CConfig::m_ClDummyCopyMoves,
                "m_ClDummyHook", &CConfig::m_ClDummyHook,
                "m_ClDummyJump", &CConfig::m_ClDummyJump,
                "m_ClDummyFire", &CConfig::m_ClDummyFire,
                "m_ClDummyResetOnSwitch", &CConfig::m_ClDummyResetOnSwitch,
                "m_ClDummyRestoreWeapon", &CConfig::m_ClDummyRestoreWeapon,
                "m_KxAttack", &CConfig::m_KxAttack,
                "m_KxCopyMoves", &CConfig::m_KxCopyMoves,
                "m_KxRandomAim", &CConfig::m_KxRandomAim,
                "m_KxAtkPathfinder", &CConfig::m_KxAtkPathfinder,
                "m_KxFlyRide", &CConfig::m_KxFlyRide,
                "m_KxDummyAim", &CConfig::m_KxDummyAim,
                "m_KxDummyDirection", &CConfig::m_KxDummyDirection,
                "m_KxDummyCopyMovesFilter", &CConfig::m_KxDummyCopyMovesFilter
        );

        auto con = lua["console"].get_or_create<sol::table>();
        con.set_function("execute", [this](const std::string &Line) {
                Console()->ExecuteLine(Line.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
        });
        con.set_function("print", [this](const std::string &Str) {
                Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", Str.c_str());
        });
        con.set_function("execute_file", [this](const std::string &File) -> bool {
                return Console()->ExecuteFile(File.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
        });

        auto gfx = lua["gfx"].get_or_create<sol::table>();
        gfx.set_function("width", [this]() -> float { return (float)Graphics()->ScreenWidth(); });
        gfx.set_function("height", [this]() -> float { return (float)Graphics()->ScreenHeight(); });
        gfx.set_function("aspect", [this]() -> float { return Graphics()->ScreenAspect(); });
        gfx.set_function("window_width", [this]() -> float { return (float)Graphics()->WindowWidth(); });
        gfx.set_function("window_height", [this]() -> float { return (float)Graphics()->WindowHeight(); });
        gfx.set_function("map_screen", [this](float X0, float Y0, float X1, float Y1) {
                Graphics()->MapScreen(X0, Y0, X1, Y1);
        });
        gfx.set_function("map_screen_to_world", [this, &lua](float CenterX, float CenterY, sol::optional<float> Zoom) -> sol::table {
                float aPoints[4];
                Graphics()->MapScreenToWorld(CenterX, CenterY, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), Zoom.value_or(1.0f), aPoints);
                Graphics()->MapScreen(aPoints[0], aPoints[1], aPoints[2], aPoints[3]);
                auto t = lua.create_table();
                t[1] = aPoints[0];
                t[2] = aPoints[1];
                t[3] = aPoints[2];
                t[4] = aPoints[3];
                return t;
        });
        gfx.set_function("draw_rect", [this](float X, float Y, float W, float H, float R, float G, float B, float A, sol::optional<int> Corners, sol::optional<float> Rounding) {
                Graphics()->DrawRect(X, Y, W, H, ColorRGBA(R, G, B, A), Corners.value_or(IGraphics::CORNER_NONE), Rounding.value_or(0.0f));
        });
        gfx.set_function("draw_circle", [this](float CX, float CY, float R, sol::optional<int> Segments) {
                Graphics()->DrawCircle(CX, CY, R, Segments.value_or(32));
        });
        gfx.set_function("draw_line", [this](float X0, float Y0, float X1, float Y1, float R, float G, float B, float A) {
                Graphics()->SetColor(R, G, B, A);
                Graphics()->LinesBegin();
                IGraphics::CLineItem Line;
                Line.m_X0 = X0;
                Line.m_Y0 = Y0;
                Line.m_X1 = X1;
                Line.m_Y1 = Y1;
                Graphics()->LinesDraw(&Line, 1);
                Graphics()->LinesEnd();
        });
        gfx.set_function("set_color", [this](float R, float G, float B, float A) {
                Graphics()->SetColor(R, G, B, A);
        });
        gfx.set_function("blend_normal", [this]() { Graphics()->BlendNormal(); });
        gfx.set_function("blend_additive", [this]() { Graphics()->BlendAdditive(); });
        gfx.set_function("map_screen_to_interface", [this](float CX, float CY, sol::optional<float> Zoom) {
                Graphics()->MapScreenToInterface(CX, CY, Zoom.value_or(1.0f));
        });
        gfx.set_function("texture_clear", [this]() { Graphics()->TextureClear(); });

        auto snd = lua["sound"].get_or_create<sol::table>();
        snd["CHN_GUI"] = CSounds::CHN_GUI;
        snd["CHN_MUSIC"] = CSounds::CHN_MUSIC;
        snd["CHN_WORLD"] = CSounds::CHN_WORLD;
        snd["CHN_GLOBAL"] = CSounds::CHN_GLOBAL;
        snd.set_function("play", [this](int SampleId, float Vol) {
                Sound()->Play(CSounds::CHN_GLOBAL, SampleId, 0, Vol);
        });
        snd.set_function("play_at", [this](int SampleId, float Vol, float X, float Y) {
                Sound()->PlayAt(CSounds::CHN_WORLD, SampleId, 0, Vol, vec2(X, Y));
        });
        snd.set_function("stop", [this](int SampleId) {
                Sound()->Stop(SampleId);
        });
        snd.set_function("stop_all", [this]() {
                Sound()->StopAll();
        });
        snd.set_function("enabled", [this]() -> bool { return Sound()->IsSoundEnabled(); });
        snd.set_function("load", [this](const std::string &Name) -> int {
                if(Name.size() >= 5 && Name.compare(Name.size() - 5, 5, ".opus") == 0)
                        return Sound()->LoadOpus(Name.c_str());
                return Sound()->LoadWV(Name.c_str());
        });
        snd.set_function("unload_sample", [this](int SampleId) { Sound()->UnloadSample(SampleId); });
        snd.set_function("pause", [this](int SampleId) { Sound()->Pause(SampleId); });
        snd.set_function("sample_time", [this](int SampleId) -> float { return Sound()->GetSampleCurrentTime(SampleId); });
        snd.set_function("sample_total_time", [this](int SampleId) -> float { return Sound()->GetSampleTotalTime(SampleId); });

        auto inp = lua["input"].get_or_create<sol::table>();
        inp["KEY_W"] = KEY_W;
        inp["KEY_A"] = KEY_A;
        inp["KEY_S"] = KEY_S;
        inp["KEY_D"] = KEY_D;
        inp["KEY_SPACE"] = KEY_SPACE;
        inp["KEY_RETURN"] = KEY_RETURN;
        inp["KEY_ESCAPE"] = KEY_ESCAPE;
        inp["KEY_TAB"] = KEY_TAB;
        inp["KEY_LSHIFT"] = KEY_LSHIFT;
        inp["KEY_RSHIFT"] = KEY_RSHIFT;
        inp["KEY_LCTRL"] = KEY_LCTRL;
        inp["KEY_RCTRL"] = KEY_RCTRL;
        inp.set_function("pressed", [this](int Key) -> bool { return Input()->KeyIsPressed(Key); });
        inp["KEY_UP"] = KEY_UP;
        inp["KEY_DOWN"] = KEY_DOWN;
        inp["KEY_LEFT"] = KEY_LEFT;
        inp["KEY_RIGHT"] = KEY_RIGHT;
        inp["KEY_DELETE"] = KEY_DELETE;
        inp["KEY_BACKSPACE"] = KEY_BACKSPACE;
        inp["KEY_INSERT"] = KEY_INSERT;
        inp["KEY_HOME"] = KEY_HOME;
        inp["KEY_END"] = KEY_END;
        inp["KEY_PAGEUP"] = KEY_PAGEUP;
        inp["KEY_PAGEDOWN"] = KEY_PAGEDOWN;
        inp["KEY_F1"] = KEY_F1;
        inp["KEY_F2"] = KEY_F2;
        inp["KEY_F3"] = KEY_F3;
        inp["KEY_F4"] = KEY_F4;
        inp["KEY_F5"] = KEY_F5;
        inp["KEY_F6"] = KEY_F6;
        inp["KEY_F7"] = KEY_F7;
        inp["KEY_F8"] = KEY_F8;
        inp["KEY_F9"] = KEY_F9;
        inp["KEY_F10"] = KEY_F10;
        inp["KEY_F11"] = KEY_F11;
        inp["KEY_F12"] = KEY_F12;
        inp["KEY_MOUSE_1"] = KEY_MOUSE_1;
        inp["KEY_MOUSE_2"] = KEY_MOUSE_2;
        inp["KEY_MOUSE_3"] = KEY_MOUSE_3;
        inp["KEY_MOUSE_WHEEL_UP"] = KEY_MOUSE_WHEEL_UP;
        inp["KEY_MOUSE_WHEEL_DOWN"] = KEY_MOUSE_WHEEL_DOWN;
        inp.set_function("pressed_this_frame", [this](int Key) -> bool { return Input()->KeyPress(Key); });
        inp.set_function("key_name", [this](int Key) -> std::string { return std::string(Input()->KeyName(Key)); });
        inp.set_function("find_key", [this](const std::string &Name) -> int { return Input()->FindKeyByName(Name.c_str()); });
        inp.set_function("shift_pressed", [this]() -> bool { return Input()->ShiftIsPressed(); });
        inp.set_function("alt_pressed", [this]() -> bool { return Input()->AltIsPressed(); });
        inp.set_function("modifier_pressed", [this]() -> bool { return Input()->ModifierIsPressed(); });
        inp.set_function("mouse_pressed", [this](sol::optional<int> Index) -> bool { return Input()->NativeMousePressed(Index.value_or(0)); });
        inp.set_function("mouse_pos", [this]() -> vec2 { return Input()->NativeMousePos(); });
        inp.set_function("clipboard_get", [this]() -> std::string { return Input()->GetClipboardText(); });
        inp.set_function("clipboard_set", [this](const std::string &Text) { Input()->SetClipboardText(Text.c_str()); });

        auto text = lua["text"].get_or_create<sol::table>();
        text.set_function("draw", [this](float X, float Y, float Size, const std::string &Str) {
                TextRender()->Text(X, Y, Size, Str.c_str(), -1.0f);
        });
        text.set_function("width", [this](float Size, const std::string &Str) -> float {
                return TextRender()->TextWidth(Size, Str.c_str());
        });
        text.set_function("color", [this](float R, float G, float B, float A) {
                TextRender()->TextColor(R, G, B, A);
        });
        text.set_function("outline_color", [this](float R, float G, float B, float A) {
                TextRender()->TextOutlineColor(R, G, B, A);
        });
        text.set_function("bounding_box", [this, &lua](float Size, const std::string &Str) -> sol::table {
                STextBoundingBox Box = TextRender()->TextBoundingBox(Size, Str.c_str());
                auto t = lua.create_table();
t["width"] = Box.m_W;
				t["height"] = Box.m_H;
                return t;
        });
}
