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
#include <game/client/tooltips.h>
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
#include <base/detect.h>
#include <base/str.h>
#include <engine/console.h>
#include <engine/keys.h>
#include <engine/client/client.h>
#include <base/vmath.h>
#include <engine/http.h>
#include <engine/client/keyboard.h>

#include <lua.hpp>
#include <LuaBridge/LuaBridge.h>
#include <LuaBridge/Vector.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <tuple>

#if defined(CONF_FAMILY_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

// =============================================================
// Code Execution — Lua API (обзор)
//
//   print(...)        — вывод в игровой чат
//   vec2(x, y)        — вектор;  CONST.* — константы движка
//   gc.*              — CGameClient (root):
//                       gc.m_aClients[i].m_aName, gc.m_aLocalIds[d],
//                       gc.m_Snap, gc.m_GameWorld, gc.m_PredictedWorld,
//                       gc.m_Controls, gc.m_Camera, gc.m_BotNet, ...
//   cl.*              — IClient: Connect/Disconnect, DummyConnect(d),
//                       DummyConnected(d), PlayerName, GameTick, LocalTime,
//                       Rcon/RconAuth, DummyName, ...
//   config            — конфиг:
//                       config.get(name) / config.set(name, value) — ЛЮБАЯ
//                       переменная по имени (динамически, через реестр);
//                       config.m_<Var> — легаси-доступ к отдельным полям
//   console.execute(line), console.print(s), console.execute_file(f)
//
//   net.get(url[, timeout])                     -> job
//   net.post(url, body[, timeout])              -> job
//   net.post_json(url, json[, hdrs][, timeout]) -> job
//       job:done() job:status() job:result() job:code() job:abort()
//
//   io.popen(cmd)     — процесс БЕЗ окна консоли -> job:
//                       job:done() job:output() job:exitcode()
//                       job:read("*a") job:close()   (блокируют, как popen)
//   os.execute(cmd)   — синхронно, без окна; возвращает ok, "exit", code
//
//   gfx.* sound.* input.* text.* — рендер, звук, ввод, текст
//
// Binding style note: LuaBridge3 registers each member with its own small
// statement. sol2 grew quadratically with the argument count of every
// new_usertype call (the 120-member probe compiled 43s vs 5s here), which is
// what made this translation unit take minutes on MSVC.
// =============================================================

// LuaBridge3's addPropertyReadWrite requires a member pointer declared in T
// itself, but many DDNet structs keep their fields in versioned/policy bases
// (CMapItemGroup_v1, color4_base, snapshot cores). Register through a member
// pointer of any declaring class with the same get/set semantics.
template <class T, class D, class Field>
static void AddField(luabridge::Namespace::Class<T> &ns, const char *pName, Field D::*pMember)
{
        ns.addProperty(pName,
                [pMember](T &o) -> Field & { return o.*pMember; },
                [pMember](T &o, const Field &v) { o.*pMember = v; });
}

// Builds the OS command line for the shell selected in Execution Settings
// (kx_exec_shell). Falls back to CMD when the configured shell is missing.
static std::string ExecShellCommandLine(const std::string &Cmd)
{
#if defined(CONF_FAMILY_WINDOWS)
        const int Shell = g_Config.m_KxExecShell;
        const char *pExe = CCodeExec::ShellPath(Shell);
        if(Shell == CCodeExec::SHELL_POWERSHELL)
                return std::string((pExe && pExe[0]) ? pExe : "powershell.exe") + " -NoProfile -Command \"" + Cmd + "\"";
        if(Shell == CCodeExec::SHELL_CMD || !CCodeExec::ShellAvailable(Shell))
                return "cmd.exe /c " + Cmd;
        return std::string((pExe && pExe[0]) ? pExe : "bash.exe") + " -c \"" + Cmd + "\"";
#else
        return Cmd;
#endif
}

// =============================================================
// RegisterRawAPI — ALL DDNet classes, ALL public members/methods
// Direct C++ access through gc (CGameClient) — no wrappers
// =============================================================
void CCodeExec::RegisterRawAPI()
{
        if(!m_pLua) return;
        lua_State *L = m_pLua;
        auto Stage = [](const char *) {};

        Stage("netobj");
        // ===================== NETWORK OBJECTS =====================

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_PlayerInput>("CNetObj_PlayerInput");
                ns.addPropertyReadWrite("m_Direction", &CNetObj_PlayerInput::m_Direction);
                ns.addPropertyReadWrite("m_TargetX", &CNetObj_PlayerInput::m_TargetX);
                ns.addPropertyReadWrite("m_TargetY", &CNetObj_PlayerInput::m_TargetY);
                ns.addPropertyReadWrite("m_Jump", &CNetObj_PlayerInput::m_Jump);
                ns.addPropertyReadWrite("m_Fire", &CNetObj_PlayerInput::m_Fire);
                ns.addPropertyReadWrite("m_Hook", &CNetObj_PlayerInput::m_Hook);
                ns.addPropertyReadWrite("m_PlayerFlags", &CNetObj_PlayerInput::m_PlayerFlags);
                ns.addPropertyReadWrite("m_WantedWeapon", &CNetObj_PlayerInput::m_WantedWeapon);
                ns.addPropertyReadWrite("m_NextWeapon", &CNetObj_PlayerInput::m_NextWeapon);
                ns.addPropertyReadWrite("m_PrevWeapon", &CNetObj_PlayerInput::m_PrevWeapon);
                ns.endClass();
        }

        // CNetObj_Character derives from CNetObj_CharacterCore: register the
        // base class so inherited members resolve and the type converts when
        // passed to functions taking CNetObj_CharacterCore.
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_CharacterCore>("CNetObj_CharacterCore");
                ns.addPropertyReadWrite("m_Tick", &CNetObj_CharacterCore::m_Tick);
                ns.addPropertyReadWrite("m_X", &CNetObj_CharacterCore::m_X);
                ns.addPropertyReadWrite("m_Y", &CNetObj_CharacterCore::m_Y);
                ns.addPropertyReadWrite("m_VelX", &CNetObj_CharacterCore::m_VelX);
                ns.addPropertyReadWrite("m_VelY", &CNetObj_CharacterCore::m_VelY);
                ns.addPropertyReadWrite("m_Angle", &CNetObj_CharacterCore::m_Angle);
                ns.addPropertyReadWrite("m_Direction", &CNetObj_CharacterCore::m_Direction);
                ns.addPropertyReadWrite("m_Jumped", &CNetObj_CharacterCore::m_Jumped);
                ns.addPropertyReadWrite("m_HookedPlayer", &CNetObj_CharacterCore::m_HookedPlayer);
                ns.addPropertyReadWrite("m_HookState", &CNetObj_CharacterCore::m_HookState);
                ns.addPropertyReadWrite("m_HookTick", &CNetObj_CharacterCore::m_HookTick);
                ns.addPropertyReadWrite("m_HookX", &CNetObj_CharacterCore::m_HookX);
                ns.addPropertyReadWrite("m_HookY", &CNetObj_CharacterCore::m_HookY);
                ns.addPropertyReadWrite("m_HookDx", &CNetObj_CharacterCore::m_HookDx);
                ns.addPropertyReadWrite("m_HookDy", &CNetObj_CharacterCore::m_HookDy);
                ns.endClass();
        }
        {
                auto ns = luabridge::getGlobalNamespace(L).deriveClass<CNetObj_Character, CNetObj_CharacterCore>("CNetObj_Character");
                ns.addPropertyReadWrite("m_PlayerFlags", &CNetObj_Character::m_PlayerFlags);
                ns.addPropertyReadWrite("m_Health", &CNetObj_Character::m_Health);
                ns.addPropertyReadWrite("m_Armor", &CNetObj_Character::m_Armor);
                ns.addPropertyReadWrite("m_AmmoCount", &CNetObj_Character::m_AmmoCount);
                ns.addPropertyReadWrite("m_Weapon", &CNetObj_Character::m_Weapon);
                ns.addPropertyReadWrite("m_Emote", &CNetObj_Character::m_Emote);
                ns.addPropertyReadWrite("m_AttackTick", &CNetObj_Character::m_AttackTick);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_PlayerInfo>("CNetObj_PlayerInfo");
                ns.addPropertyReadWrite("m_Local", &CNetObj_PlayerInfo::m_Local);
                ns.addPropertyReadWrite("m_ClientId", &CNetObj_PlayerInfo::m_ClientId);
                ns.addPropertyReadWrite("m_Team", &CNetObj_PlayerInfo::m_Team);
                ns.addPropertyReadWrite("m_Score", &CNetObj_PlayerInfo::m_Score);
                ns.addPropertyReadWrite("m_Latency", &CNetObj_PlayerInfo::m_Latency);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_GameInfo>("CNetObj_GameInfo");
                ns.addPropertyReadWrite("m_GameFlags", &CNetObj_GameInfo::m_GameFlags);
                ns.addPropertyReadWrite("m_GameStateFlags", &CNetObj_GameInfo::m_GameStateFlags);
                ns.addPropertyReadWrite("m_RoundStartTick", &CNetObj_GameInfo::m_RoundStartTick);
                ns.addPropertyReadWrite("m_WarmupTimer", &CNetObj_GameInfo::m_WarmupTimer);
                ns.addPropertyReadWrite("m_ScoreLimit", &CNetObj_GameInfo::m_ScoreLimit);
                ns.addPropertyReadWrite("m_TimeLimit", &CNetObj_GameInfo::m_TimeLimit);
                ns.addPropertyReadWrite("m_RoundNum", &CNetObj_GameInfo::m_RoundNum);
                ns.addPropertyReadWrite("m_RoundCurrent", &CNetObj_GameInfo::m_RoundCurrent);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_GameData>("CNetObj_GameData");
                ns.addPropertyReadWrite("m_TeamscoreRed", &CNetObj_GameData::m_TeamscoreRed);
                ns.addPropertyReadWrite("m_TeamscoreBlue", &CNetObj_GameData::m_TeamscoreBlue);
                ns.addPropertyReadWrite("m_FlagCarrierRed", &CNetObj_GameData::m_FlagCarrierRed);
                ns.addPropertyReadWrite("m_FlagCarrierBlue", &CNetObj_GameData::m_FlagCarrierBlue);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_Flag>("CNetObj_Flag");
                ns.addPropertyReadWrite("m_X", &CNetObj_Flag::m_X);
                ns.addPropertyReadWrite("m_Y", &CNetObj_Flag::m_Y);
                ns.addPropertyReadWrite("m_Team", &CNetObj_Flag::m_Team);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_SpectatorInfo>("CNetObj_SpectatorInfo");
                ns.addPropertyReadWrite("m_SpectatorId", &CNetObj_SpectatorInfo::m_SpectatorId);
                ns.addPropertyReadWrite("m_X", &CNetObj_SpectatorInfo::m_X);
                ns.addPropertyReadWrite("m_Y", &CNetObj_SpectatorInfo::m_Y);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CNetObj_DDNetCharacter>("CNetObj_DDNetCharacter");
                ns.addPropertyReadWrite("m_Flags", &CNetObj_DDNetCharacter::m_Flags);
                ns.addPropertyReadWrite("m_FreezeEnd", &CNetObj_DDNetCharacter::m_FreezeEnd);
                ns.addPropertyReadWrite("m_Jumps", &CNetObj_DDNetCharacter::m_Jumps);
                ns.addPropertyReadWrite("m_TeleCheckpoint", &CNetObj_DDNetCharacter::m_TeleCheckpoint);
                ns.addPropertyReadWrite("m_StrongWeakId", &CNetObj_DDNetCharacter::m_StrongWeakId);
                ns.addPropertyReadWrite("m_JumpedTotal", &CNetObj_DDNetCharacter::m_JumpedTotal);
                ns.addPropertyReadWrite("m_NinjaActivationTick", &CNetObj_DDNetCharacter::m_NinjaActivationTick);
                ns.addPropertyReadWrite("m_FreezeStart", &CNetObj_DDNetCharacter::m_FreezeStart);
                ns.addPropertyReadWrite("m_TargetX", &CNetObj_DDNetCharacter::m_TargetX);
                ns.addPropertyReadWrite("m_TargetY", &CNetObj_DDNetCharacter::m_TargetY);
                ns.endClass();
        }

        Stage("tuning");
        // ===================== CTuningParams =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CTuningParams>("CTuningParams");
                ns.addProperty("m_GroundControlSpeed", [](CTuningParams &t) -> float { return (float)t.m_GroundControlSpeed; }, [](CTuningParams &t, float v) { t.m_GroundControlSpeed = v; });
                ns.addProperty("m_GroundControlAccel", [](CTuningParams &t) -> float { return (float)t.m_GroundControlAccel; }, [](CTuningParams &t, float v) { t.m_GroundControlAccel = v; });
                ns.addProperty("m_GroundFriction", [](CTuningParams &t) -> float { return (float)t.m_GroundFriction; }, [](CTuningParams &t, float v) { t.m_GroundFriction = v; });
                ns.addProperty("m_GroundJumpImpulse", [](CTuningParams &t) -> float { return (float)t.m_GroundJumpImpulse; }, [](CTuningParams &t, float v) { t.m_GroundJumpImpulse = v; });
                ns.addProperty("m_AirJumpImpulse", [](CTuningParams &t) -> float { return (float)t.m_AirJumpImpulse; }, [](CTuningParams &t, float v) { t.m_AirJumpImpulse = v; });
                ns.addProperty("m_AirControlSpeed", [](CTuningParams &t) -> float { return (float)t.m_AirControlSpeed; }, [](CTuningParams &t, float v) { t.m_AirControlSpeed = v; });
                ns.addProperty("m_AirControlAccel", [](CTuningParams &t) -> float { return (float)t.m_AirControlAccel; }, [](CTuningParams &t, float v) { t.m_AirControlAccel = v; });
                ns.addProperty("m_AirFriction", [](CTuningParams &t) -> float { return (float)t.m_AirFriction; }, [](CTuningParams &t, float v) { t.m_AirFriction = v; });
                ns.addProperty("m_HookLength", [](CTuningParams &t) -> float { return (float)t.m_HookLength; }, [](CTuningParams &t, float v) { t.m_HookLength = v; });
                ns.addProperty("m_HookFireSpeed", [](CTuningParams &t) -> float { return (float)t.m_HookFireSpeed; }, [](CTuningParams &t, float v) { t.m_HookFireSpeed = v; });
                ns.addProperty("m_HookDragAccel", [](CTuningParams &t) -> float { return (float)t.m_HookDragAccel; }, [](CTuningParams &t, float v) { t.m_HookDragAccel = v; });
                ns.addProperty("m_HookDragSpeed", [](CTuningParams &t) -> float { return (float)t.m_HookDragSpeed; }, [](CTuningParams &t, float v) { t.m_HookDragSpeed = v; });
                ns.addProperty("m_Gravity", [](CTuningParams &t) -> float { return (float)t.m_Gravity; }, [](CTuningParams &t, float v) { t.m_Gravity = v; });
                ns.addProperty("m_VelrampStart", [](CTuningParams &t) -> float { return (float)t.m_VelrampStart; }, [](CTuningParams &t, float v) { t.m_VelrampStart = v; });
                ns.addProperty("m_VelrampRange", [](CTuningParams &t) -> float { return (float)t.m_VelrampRange; }, [](CTuningParams &t, float v) { t.m_VelrampRange = v; });
                ns.addProperty("m_VelrampCurvature", [](CTuningParams &t) -> float { return (float)t.m_VelrampCurvature; }, [](CTuningParams &t, float v) { t.m_VelrampCurvature = v; });
                ns.addProperty("m_GunCurvature", [](CTuningParams &t) -> float { return (float)t.m_GunCurvature; }, [](CTuningParams &t, float v) { t.m_GunCurvature = v; });
                ns.addProperty("m_GunSpeed", [](CTuningParams &t) -> float { return (float)t.m_GunSpeed; }, [](CTuningParams &t, float v) { t.m_GunSpeed = v; });
                ns.addProperty("m_GunLifetime", [](CTuningParams &t) -> float { return (float)t.m_GunLifetime; }, [](CTuningParams &t, float v) { t.m_GunLifetime = v; });
                ns.addProperty("m_ShotgunCurvature", [](CTuningParams &t) -> float { return (float)t.m_ShotgunCurvature; }, [](CTuningParams &t, float v) { t.m_ShotgunCurvature = v; });
                ns.addProperty("m_ShotgunSpeed", [](CTuningParams &t) -> float { return (float)t.m_ShotgunSpeed; }, [](CTuningParams &t, float v) { t.m_ShotgunSpeed = v; });
                ns.addProperty("m_ShotgunSpeeddiff", [](CTuningParams &t) -> float { return (float)t.m_ShotgunSpeeddiff; }, [](CTuningParams &t, float v) { t.m_ShotgunSpeeddiff = v; });
                ns.addProperty("m_ShotgunLifetime", [](CTuningParams &t) -> float { return (float)t.m_ShotgunLifetime; }, [](CTuningParams &t, float v) { t.m_ShotgunLifetime = v; });
                ns.addProperty("m_GrenadeCurvature", [](CTuningParams &t) -> float { return (float)t.m_GrenadeCurvature; }, [](CTuningParams &t, float v) { t.m_GrenadeCurvature = v; });
                ns.addProperty("m_GrenadeSpeed", [](CTuningParams &t) -> float { return (float)t.m_GrenadeSpeed; }, [](CTuningParams &t, float v) { t.m_GrenadeSpeed = v; });
                ns.addProperty("m_GrenadeLifetime", [](CTuningParams &t) -> float { return (float)t.m_GrenadeLifetime; }, [](CTuningParams &t, float v) { t.m_GrenadeLifetime = v; });
                ns.addProperty("m_LaserReach", [](CTuningParams &t) -> float { return (float)t.m_LaserReach; }, [](CTuningParams &t, float v) { t.m_LaserReach = v; });
                ns.addProperty("m_LaserBounceDelay", [](CTuningParams &t) -> float { return (float)t.m_LaserBounceDelay; }, [](CTuningParams &t, float v) { t.m_LaserBounceDelay = v; });
                ns.addProperty("m_LaserBounceNum", [](CTuningParams &t) -> float { return (float)t.m_LaserBounceNum; }, [](CTuningParams &t, float v) { t.m_LaserBounceNum = v; });
                ns.addProperty("m_LaserBounceCost", [](CTuningParams &t) -> float { return (float)t.m_LaserBounceCost; }, [](CTuningParams &t, float v) { t.m_LaserBounceCost = v; });
                ns.addProperty("m_LaserDamage", [](CTuningParams &t) -> float { return (float)t.m_LaserDamage; }, [](CTuningParams &t, float v) { t.m_LaserDamage = v; });
                ns.addProperty("m_PlayerCollision", [](CTuningParams &t) -> float { return (float)t.m_PlayerCollision; }, [](CTuningParams &t, float v) { t.m_PlayerCollision = v; });
                ns.addProperty("m_PlayerHooking", [](CTuningParams &t) -> float { return (float)t.m_PlayerHooking; }, [](CTuningParams &t, float v) { t.m_PlayerHooking = v; });
                ns.addProperty("m_JetpackStrength", [](CTuningParams &t) -> float { return (float)t.m_JetpackStrength; }, [](CTuningParams &t, float v) { t.m_JetpackStrength = v; });
                ns.addProperty("m_ShotgunStrength", [](CTuningParams &t) -> float { return (float)t.m_ShotgunStrength; }, [](CTuningParams &t, float v) { t.m_ShotgunStrength = v; });
                ns.addProperty("m_ExplosionStrength", [](CTuningParams &t) -> float { return (float)t.m_ExplosionStrength; }, [](CTuningParams &t, float v) { t.m_ExplosionStrength = v; });
                ns.addProperty("m_HammerStrength", [](CTuningParams &t) -> float { return (float)t.m_HammerStrength; }, [](CTuningParams &t, float v) { t.m_HammerStrength = v; });
                ns.addProperty("m_HookDuration", [](CTuningParams &t) -> float { return (float)t.m_HookDuration; }, [](CTuningParams &t, float v) { t.m_HookDuration = v; });
                ns.addProperty("m_HammerFireDelay", [](CTuningParams &t) -> float { return (float)t.m_HammerFireDelay; }, [](CTuningParams &t, float v) { t.m_HammerFireDelay = v; });
                ns.addProperty("m_GunFireDelay", [](CTuningParams &t) -> float { return (float)t.m_GunFireDelay; }, [](CTuningParams &t, float v) { t.m_GunFireDelay = v; });
                ns.addProperty("m_ShotgunFireDelay", [](CTuningParams &t) -> float { return (float)t.m_ShotgunFireDelay; }, [](CTuningParams &t, float v) { t.m_ShotgunFireDelay = v; });
                ns.addProperty("m_GrenadeFireDelay", [](CTuningParams &t) -> float { return (float)t.m_GrenadeFireDelay; }, [](CTuningParams &t, float v) { t.m_GrenadeFireDelay = v; });
                ns.addProperty("m_LaserFireDelay", [](CTuningParams &t) -> float { return (float)t.m_LaserFireDelay; }, [](CTuningParams &t, float v) { t.m_LaserFireDelay = v; });
                ns.addProperty("m_NinjaFireDelay", [](CTuningParams &t) -> float { return (float)t.m_NinjaFireDelay; }, [](CTuningParams &t, float v) { t.m_NinjaFireDelay = v; });
                ns.addProperty("m_HammerHitFireDelay", [](CTuningParams &t) -> float { return (float)t.m_HammerHitFireDelay; }, [](CTuningParams &t, float v) { t.m_HammerHitFireDelay = v; });
                ns.addProperty("m_GroundElasticityX", [](CTuningParams &t) -> float { return (float)t.m_GroundElasticityX; }, [](CTuningParams &t, float v) { t.m_GroundElasticityX = v; });
                ns.addProperty("m_GroundElasticityY", [](CTuningParams &t) -> float { return (float)t.m_GroundElasticityY; }, [](CTuningParams &t, float v) { t.m_GroundElasticityY = v; });
                ns.addFunction("Set", [](CTuningParams &t, int i, float v) -> bool { return t.Set(i, v); }, [](CTuningParams &t, const std::string &n, float v) -> bool { return t.Set(n.c_str(), v); });
                ns.addFunction("Get", [](CTuningParams &t, int i) -> float { float v; t.Get(i, &v); return v; }, [](CTuningParams &t, const std::string &n) -> float { float v; t.Get(n.c_str(), &v); return v; });
                ns.addFunction("Name", [](CTuningParams &, int i) -> std::string { return std::string(CTuningParams::Name(i)); });
                ns.addFunction("Num", [](CTuningParams &) -> int { return CTuningParams::Num(); });
                ns.addFunction("GetWeaponFireDelay", [](CTuningParams &t, int w) -> float { return t.GetWeaponFireDelay(w); });
                ns.endClass();
        }

        Stage("teams");
        // ===================== CTeamsCore =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CTeamsCore>("CTeamsCore");
                ns.addPropertyReadWrite("m_NumDDRaceTeams", &CTeamsCore::m_NumDDRaceTeams);
                ns.addFunction("SameTeam", &CTeamsCore::SameTeam);
                ns.addFunction("CanKeepHook", &CTeamsCore::CanKeepHook);
                ns.addFunction("CanCollide", &CTeamsCore::CanCollide);
                ns.addFunction("Team", [](CTeamsCore &t, int id) -> int { return t.Team(id); }, [](CTeamsCore &t, int id, int team) { t.Team(id, team); });
                ns.addFunction("Reset", [](CTeamsCore &t) { t.Reset(); });
                ns.addFunction("SetSolo", &CTeamsCore::SetSolo);
                ns.addFunction("GetSolo", &CTeamsCore::GetSolo);
                ns.endClass();
        }

        Stage("switchers");
        // ===================== SSwitchers =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<SSwitchers>("SSwitchers");
                ns.addPropertyReadWrite("m_Initial", &SSwitchers::m_Initial);
                ns.addProperty("m_aStatus", [L](SSwitchers &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 64; i++) t[i] = s.m_aStatus[i];
                        return t;
                });
                ns.addProperty("m_aEndTick", [L](SSwitchers &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 64; i++) t[i] = s.m_aEndTick[i];
                        return t;
                });
                ns.addProperty("m_aType", [L](SSwitchers &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 64; i++) t[i] = s.m_aType[i];
                        return t;
                });
                ns.addProperty("m_aLastUpdateTick", [L](SSwitchers &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 64; i++) t[i] = s.m_aLastUpdateTick[i];
                        return t;
                });
                ns.endClass();
        }

        Stage("worldcore");
        // ===================== CWorldCore =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CWorldCore>("CWorldCore");
                ns.addProperty("m_apCharacters", [L](CWorldCore &w) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++)
                                if(w.m_apCharacters[i])
                                        t[i] = w.m_apCharacters[i];
                        return t;
                });
                // Live reference table (sol2 returned userdata referencing the
                // vector): writes through the elements must stick.
                ns.addProperty("m_vSwitchers", [L](CWorldCore &w) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = w.m_vSwitchers;
                        for(size_t i = 0; i < v.size(); i++) t[(int)i] = &v[i];
                        return t;
                });
                ns.addFunction("RandomOr0", &CWorldCore::RandomOr0);
                ns.addFunction("InitSwitchers", &CWorldCore::InitSwitchers);
                ns.endClass();
        }

        Stage("charcore");
        // ===================== CCharacterCore =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CCharacterCore>("CCharacterCore");
                ns.addProperty("m_Pos", [](CCharacterCore &c) -> vec2& { return c.m_Pos; });
                ns.addProperty("m_Vel", [](CCharacterCore &c) -> vec2& { return c.m_Vel; });
                ns.addProperty("m_HookPos", [](CCharacterCore &c) -> vec2& { return c.m_HookPos; });
                ns.addProperty("m_HookDir", [](CCharacterCore &c) -> vec2& { return c.m_HookDir; });
                ns.addProperty("m_HookTeleBase", [](CCharacterCore &c) -> vec2& { return c.m_HookTeleBase; });
                ns.addPropertyReadWrite("m_HookTick", &CCharacterCore::m_HookTick);
                ns.addPropertyReadWrite("m_HookState", &CCharacterCore::m_HookState);
                ns.addProperty("m_HookedPlayer", [](CCharacterCore &c) -> int { return c.HookedPlayer(); }, [](CCharacterCore &c, int v) { c.SetHookedPlayer(v); });
                ns.addPropertyReadWrite("m_ActiveWeapon", &CCharacterCore::m_ActiveWeapon);
                ns.addPropertyReadWrite("m_Jumped", &CCharacterCore::m_Jumped);
                ns.addPropertyReadWrite("m_JumpedTotal", &CCharacterCore::m_JumpedTotal);
                ns.addPropertyReadWrite("m_Jumps", &CCharacterCore::m_Jumps);
                ns.addPropertyReadWrite("m_Direction", &CCharacterCore::m_Direction);
                ns.addPropertyReadWrite("m_Angle", &CCharacterCore::m_Angle);
                ns.addProperty("m_Input", [](CCharacterCore &c) -> CNetObj_PlayerInput& { return c.m_Input; });
                ns.addPropertyReadWrite("m_TriggeredEvents", &CCharacterCore::m_TriggeredEvents);
                ns.addPropertyReadWrite("m_Id", &CCharacterCore::m_Id);
                ns.addPropertyReadWrite("m_NewHook", &CCharacterCore::m_NewHook);
                ns.addPropertyReadWrite("m_Reset", &CCharacterCore::m_Reset);
                ns.addPropertyReadWrite("m_Colliding", &CCharacterCore::m_Colliding);
                ns.addPropertyReadWrite("m_LeftWall", &CCharacterCore::m_LeftWall);
                ns.addPropertyReadWrite("m_Solo", &CCharacterCore::m_Solo);
                ns.addPropertyReadWrite("m_Jetpack", &CCharacterCore::m_Jetpack);
                ns.addPropertyReadWrite("m_CollisionDisabled", &CCharacterCore::m_CollisionDisabled);
                ns.addPropertyReadWrite("m_EndlessHook", &CCharacterCore::m_EndlessHook);
                ns.addPropertyReadWrite("m_EndlessJump", &CCharacterCore::m_EndlessJump);
                ns.addPropertyReadWrite("m_HammerHitDisabled", &CCharacterCore::m_HammerHitDisabled);
                ns.addPropertyReadWrite("m_GrenadeHitDisabled", &CCharacterCore::m_GrenadeHitDisabled);
                ns.addPropertyReadWrite("m_LaserHitDisabled", &CCharacterCore::m_LaserHitDisabled);
                ns.addPropertyReadWrite("m_ShotgunHitDisabled", &CCharacterCore::m_ShotgunHitDisabled);
                ns.addPropertyReadWrite("m_HookHitDisabled", &CCharacterCore::m_HookHitDisabled);
                ns.addPropertyReadWrite("m_Super", &CCharacterCore::m_Super);
                ns.addPropertyReadWrite("m_Invincible", &CCharacterCore::m_Invincible);
                ns.addPropertyReadWrite("m_HasTelegunGun", &CCharacterCore::m_HasTelegunGun);
                ns.addPropertyReadWrite("m_HasTelegunGrenade", &CCharacterCore::m_HasTelegunGrenade);
                ns.addPropertyReadWrite("m_HasTelegunLaser", &CCharacterCore::m_HasTelegunLaser);
                ns.addPropertyReadWrite("m_FreezeStart", &CCharacterCore::m_FreezeStart);
                ns.addPropertyReadWrite("m_FreezeEnd", &CCharacterCore::m_FreezeEnd);
                ns.addPropertyReadWrite("m_IsInFreeze", &CCharacterCore::m_IsInFreeze);
                ns.addPropertyReadWrite("m_DeepFrozen", &CCharacterCore::m_DeepFrozen);
                ns.addPropertyReadWrite("m_LiveFrozen", &CCharacterCore::m_LiveFrozen);
                ns.addProperty("m_Tuning", [](CCharacterCore &c) -> CTuningParams& { return c.m_Tuning; });
                ns.addProperty("m_AttachedPlayers", [L](CCharacterCore &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        int idx = 1;
                        for(int pid : c.m_AttachedPlayers) t[idx++] = pid;
                        return t;
                });
                ns.addProperty("m_aWeapons", [L](CCharacterCore &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < NUM_WEAPONS; i++)
                        {
                                auto w = luabridge::newTable(L);
                                w["m_Ammo"] = c.m_aWeapons[i].m_Ammo;
                                w["m_AmmoRegenStart"] = c.m_aWeapons[i].m_AmmoRegenStart;
                                w["m_Ammocost"] = c.m_aWeapons[i].m_Ammocost;
                                w["m_Got"] = c.m_aWeapons[i].m_Got;
                                t[i] = w;
                        }
                        return t;
                });
                ns.addProperty("m_Ninja_ActivationDir", [](CCharacterCore &c) -> vec2 { return c.m_Ninja.m_ActivationDir; });
                ns.addProperty("m_Ninja_ActivationTick", [](CCharacterCore &c) -> int { return c.m_Ninja.m_ActivationTick; });
                ns.addProperty("m_Ninja_CurrentMoveTime", [](CCharacterCore &c) -> int { return c.m_Ninja.m_CurrentMoveTime; });
                ns.addProperty("m_Ninja_OldVelAmount", [](CCharacterCore &c) -> int { return c.m_Ninja.m_OldVelAmount; });
                // Static members: addStaticFunction also serves instance lookups
                // (an extra self argument from a ':' call is simply ignored).
                ns.addStaticFunction("PhysicalSize", &CCharacterCore::PhysicalSize);
                ns.addStaticFunction("PhysicalSizeVec2", &CCharacterCore::PhysicalSizeVec2);
                ns.addFunction("Reset", [](CCharacterCore &c) { c.Reset(); });
                ns.addFunction("Tick", [](CCharacterCore &c, bool useInput, std::optional<bool> deferred) { c.Tick(useInput, deferred.value_or(true)); });
                ns.addFunction("TickDeferred", [](CCharacterCore &c) { c.TickDeferred(); });
                ns.addFunction("Move", [](CCharacterCore &c) { c.Move(); });
                ns.addFunction("Quantize", [](CCharacterCore &c) { c.Quantize(); });
                ns.addFunction("Read", [](CCharacterCore &c, CNetObj_CharacterCore &obj) { c.Read(&obj); });
                ns.addFunction("Write", [](CCharacterCore &c, CNetObj_CharacterCore &obj) { c.Write(&obj); });
                ns.addFunction("ReadDDNet", [](CCharacterCore &c, CNetObj_DDNetCharacter &obj) { c.ReadDDNet(&obj); });
                ns.addFunction("Collision", [](CCharacterCore &c) -> CCollision* { return c.Collision(); });
                ns.addFunction("SetTeamsCore", [](CCharacterCore &c, CTeamsCore &t) { c.SetTeamsCore(&t); });
                ns.addFunction("Clone", [](CCharacterCore &c) -> CCharacterCore { return c; });
                ns.addFunction("Init", [](CCharacterCore &c, CWorldCore &world, CCollision &col, CTeamsCore *teams) { c.Init(&world, &col, teams); });
                ns.addFunction("SetCoreWorld", [](CCharacterCore &c, CWorldCore &world, CCollision &col, CTeamsCore *teams) { c.SetCoreWorld(&world, &col, teams); });
                ns.endClass();
        }

        Stage("clientstats");
        // ===================== CClientStats =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameClient::CClientStats>("CClientStats");
                ns.addPropertyReadWrite("m_Frags", &CGameClient::CClientStats::m_Frags);
                ns.addPropertyReadWrite("m_Deaths", &CGameClient::CClientStats::m_Deaths);
                ns.addPropertyReadWrite("m_Suicides", &CGameClient::CClientStats::m_Suicides);
                ns.addPropertyReadWrite("m_BestSpree", &CGameClient::CClientStats::m_BestSpree);
                ns.addPropertyReadWrite("m_CurrentSpree", &CGameClient::CClientStats::m_CurrentSpree);
                ns.addPropertyReadWrite("m_FlagGrabs", &CGameClient::CClientStats::m_FlagGrabs);
                ns.addPropertyReadWrite("m_FlagCaptures", &CGameClient::CClientStats::m_FlagCaptures);
                ns.addProperty("m_aFragsWith", [L](CGameClient::CClientStats &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < NUM_WEAPONS; i++) t[i] = s.m_aFragsWith[i];
                        return t;
                });
                ns.addProperty("m_aDeathsFrom", [L](CGameClient::CClientStats &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < NUM_WEAPONS; i++) t[i] = s.m_aDeathsFrom[i];
                        return t;
                });
                ns.addFunction("Reset", [](CGameClient::CClientStats &s) { s.Reset(); });
                ns.addFunction("IsActive", [](CGameClient::CClientStats &s) -> bool { return s.IsActive(); });
                ns.addFunction("JoinGame", [](CGameClient::CClientStats &s, int tick) { s.JoinGame(tick); });
                ns.addFunction("JoinSpec", [](CGameClient::CClientStats &s, int tick) { s.JoinSpec(tick); });
                ns.addFunction("GetIngameTicks", [](CGameClient::CClientStats &s, int tick) -> int { return s.GetIngameTicks(tick); });
                ns.addFunction("GetFPM", [](CGameClient::CClientStats &s, int tick, int speed) -> float { return s.GetFPM(tick, speed); });
                ns.endClass();
        }

        Stage("gameinfo");
        // ===================== CGameInfo =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameInfo>("CGameInfo");
                ns.addPropertyReadWrite("m_FlagStartsRace", &CGameInfo::m_FlagStartsRace);
                ns.addPropertyReadWrite("m_TimeScore", &CGameInfo::m_TimeScore);
                ns.addPropertyReadWrite("m_UnlimitedAmmo", &CGameInfo::m_UnlimitedAmmo);
                ns.addPropertyReadWrite("m_DDRaceRecordMessage", &CGameInfo::m_DDRaceRecordMessage);
                ns.addPropertyReadWrite("m_RaceRecordMessage", &CGameInfo::m_RaceRecordMessage);
                ns.addPropertyReadWrite("m_RaceSounds", &CGameInfo::m_RaceSounds);
                ns.addPropertyReadWrite("m_AllowEyeWheel", &CGameInfo::m_AllowEyeWheel);
                ns.addPropertyReadWrite("m_AllowHookColl", &CGameInfo::m_AllowHookColl);
                ns.addPropertyReadWrite("m_AllowZoom", &CGameInfo::m_AllowZoom);
                ns.addPropertyReadWrite("m_BugDDRaceGhost", &CGameInfo::m_BugDDRaceGhost);
                ns.addPropertyReadWrite("m_BugDDRaceInput", &CGameInfo::m_BugDDRaceInput);
                ns.addPropertyReadWrite("m_BugFNGLaserRange", &CGameInfo::m_BugFNGLaserRange);
                ns.addPropertyReadWrite("m_BugVanillaBounce", &CGameInfo::m_BugVanillaBounce);
                ns.addPropertyReadWrite("m_PredictFNG", &CGameInfo::m_PredictFNG);
                ns.addPropertyReadWrite("m_PredictDDRace", &CGameInfo::m_PredictDDRace);
                ns.addPropertyReadWrite("m_PredictDDRaceTiles", &CGameInfo::m_PredictDDRaceTiles);
                ns.addPropertyReadWrite("m_PredictVanilla", &CGameInfo::m_PredictVanilla);
                ns.addPropertyReadWrite("m_EntitiesDDNet", &CGameInfo::m_EntitiesDDNet);
                ns.addPropertyReadWrite("m_EntitiesDDRace", &CGameInfo::m_EntitiesDDRace);
                ns.addPropertyReadWrite("m_EntitiesRace", &CGameInfo::m_EntitiesRace);
                ns.addPropertyReadWrite("m_EntitiesFNG", &CGameInfo::m_EntitiesFNG);
                ns.addPropertyReadWrite("m_EntitiesVanilla", &CGameInfo::m_EntitiesVanilla);
                ns.addPropertyReadWrite("m_EntitiesBW", &CGameInfo::m_EntitiesBW);
                ns.addPropertyReadWrite("m_EntitiesFDDrace", &CGameInfo::m_EntitiesFDDrace);
                ns.addPropertyReadWrite("m_Race", &CGameInfo::m_Race);
                ns.addPropertyReadWrite("m_Pvp", &CGameInfo::m_Pvp);
                ns.addPropertyReadWrite("m_DontMaskEntities", &CGameInfo::m_DontMaskEntities);
                ns.addPropertyReadWrite("m_AllowXSkins", &CGameInfo::m_AllowXSkins);
                ns.addPropertyReadWrite("m_HudHealthArmor", &CGameInfo::m_HudHealthArmor);
                ns.addPropertyReadWrite("m_HudAmmo", &CGameInfo::m_HudAmmo);
                ns.addPropertyReadWrite("m_HudDDRace", &CGameInfo::m_HudDDRace);
                ns.addPropertyReadWrite("m_NoWeakHookAndBounce", &CGameInfo::m_NoWeakHookAndBounce);
                ns.addPropertyReadWrite("m_NoSkinChangeForFrozen", &CGameInfo::m_NoSkinChangeForFrozen);
                ns.addPropertyReadWrite("m_DDRaceTeam", &CGameInfo::m_DDRaceTeam);
                ns.addPropertyReadWrite("m_PredictEvents", &CGameInfo::m_PredictEvents);
                ns.endClass();
        }

        Stage("snap");
        // ===================== CSnapState =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameClient::CSnapState::CSpectateInfo>("CSpecInfo");
                ns.addPropertyReadWrite("m_Active", &CGameClient::CSnapState::CSpectateInfo::m_Active);
                ns.addPropertyReadWrite("m_SpectatorId", &CGameClient::CSnapState::CSpectateInfo::m_SpectatorId);
                ns.addPropertyReadWrite("m_UsePosition", &CGameClient::CSnapState::CSpectateInfo::m_UsePosition);
                ns.addProperty("m_Position", [](CGameClient::CSnapState::CSpectateInfo &s) -> vec2& { return s.m_Position; });
                ns.addPropertyReadWrite("m_HasCameraInfo", &CGameClient::CSnapState::CSpectateInfo::m_HasCameraInfo);
                ns.addPropertyReadWrite("m_Zoom", &CGameClient::CSnapState::CSpectateInfo::m_Zoom);
                ns.addPropertyReadWrite("m_Deadzone", &CGameClient::CSnapState::CSpectateInfo::m_Deadzone);
                ns.addPropertyReadWrite("m_FollowFactor", &CGameClient::CSnapState::CSpectateInfo::m_FollowFactor);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameClient::CSnapState>("CSnapState");
                ns.addPropertyReadWrite("m_LocalClientId", &CGameClient::CSnapState::m_LocalClientId);
                ns.addPropertyReadWrite("m_NumPlayers", &CGameClient::CSnapState::m_NumPlayers);
                ns.addPropertyReadWrite("m_NumFlags", &CGameClient::CSnapState::m_NumFlags);
                ns.addPropertyReadWrite("m_HighestClientId", &CGameClient::CSnapState::m_HighestClientId);
                ns.addProperty("m_aTeamSize", [L](CGameClient::CSnapState &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 2; i++) t[i] = s.m_aTeamSize[i];
                        return t;
                });
                ns.addProperty("m_SpecInfo", [](CGameClient::CSnapState &s) -> CGameClient::CSnapState::CSpectateInfo& { return s.m_SpecInfo; });
                ns.addProperty("m_aCharacters", [L](CGameClient::CSnapState &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++)
                        {
                                auto ct = luabridge::newTable(L);
                                ct["m_Active"] = s.m_aCharacters[i].m_Active;
                                ct["m_Cur"] = &s.m_aCharacters[i].m_Cur;
                                ct["m_Prev"] = &s.m_aCharacters[i].m_Prev;
                                ct["m_HasExtendedData"] = s.m_aCharacters[i].m_HasExtendedData;
                                ct["m_HasExtendedDisplayInfo"] = s.m_aCharacters[i].m_HasExtendedDisplayInfo;
                                t[i] = ct;
                        }
                        return t;
                });
                ns.addProperty("m_apPlayerInfos", [L](CGameClient::CSnapState &s) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++)
                        {
                                if(s.m_apPlayerInfos[i])
                                {
                                        auto pi = luabridge::newTable(L);
                                        pi["m_Local"] = s.m_apPlayerInfos[i]->m_Local;
                                        pi["m_ClientId"] = s.m_apPlayerInfos[i]->m_ClientId;
                                        pi["m_Team"] = s.m_apPlayerInfos[i]->m_Team;
                                        pi["m_Score"] = s.m_apPlayerInfos[i]->m_Score;
                                        pi["m_Latency"] = s.m_apPlayerInfos[i]->m_Latency;
                                        t[i] = pi;
                                }
                        }
                        return t;
                });
                ns.addProperty("m_pGameInfoObj", [](CGameClient::CSnapState &s) -> CNetObj_GameInfo* { return (CNetObj_GameInfo*)s.m_pGameInfoObj; });
                ns.addProperty("m_pGameDataObj", [](CGameClient::CSnapState &s) -> CNetObj_GameData* { return (CNetObj_GameData*)s.m_pGameDataObj; });
                ns.endClass();
        }

        Stage("clientdata");
        // ===================== CClientData =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameClient::CClientData>("CClientData");
                ns.addProperty("m_aName", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aName); });
                ns.addProperty("m_aClan", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aClan); });
                ns.addPropertyReadWrite("m_Country", &CGameClient::CClientData::m_Country);
                ns.addProperty("m_aSkinName", [](CGameClient::CClientData &d) -> std::string { return std::string(d.m_aSkinName); });
                ns.addPropertyReadWrite("m_Team", &CGameClient::CClientData::m_Team);
                ns.addPropertyReadWrite("m_Active", &CGameClient::CClientData::m_Active);
                ns.addPropertyReadWrite("m_Friend", &CGameClient::CClientData::m_Friend);
                ns.addPropertyReadWrite("m_Foe", &CGameClient::CClientData::m_Foe);
                ns.addPropertyReadWrite("m_Afk", &CGameClient::CClientData::m_Afk);
                ns.addPropertyReadWrite("m_Paused", &CGameClient::CClientData::m_Paused);
                ns.addPropertyReadWrite("m_Spec", &CGameClient::CClientData::m_Spec);
                ns.addPropertyReadWrite("m_Solo", &CGameClient::CClientData::m_Solo);
                ns.addPropertyReadWrite("m_Jetpack", &CGameClient::CClientData::m_Jetpack);
                ns.addPropertyReadWrite("m_Super", &CGameClient::CClientData::m_Super);
                ns.addPropertyReadWrite("m_Invincible", &CGameClient::CClientData::m_Invincible);
                ns.addPropertyReadWrite("m_DeepFrozen", &CGameClient::CClientData::m_DeepFrozen);
                ns.addPropertyReadWrite("m_LiveFrozen", &CGameClient::CClientData::m_LiveFrozen);
                ns.addPropertyReadWrite("m_CollisionDisabled", &CGameClient::CClientData::m_CollisionDisabled);
                ns.addPropertyReadWrite("m_EndlessHook", &CGameClient::CClientData::m_EndlessHook);
                ns.addPropertyReadWrite("m_EndlessJump", &CGameClient::CClientData::m_EndlessJump);
                ns.addPropertyReadWrite("m_HammerHitDisabled", &CGameClient::CClientData::m_HammerHitDisabled);
                ns.addPropertyReadWrite("m_GrenadeHitDisabled", &CGameClient::CClientData::m_GrenadeHitDisabled);
                ns.addPropertyReadWrite("m_LaserHitDisabled", &CGameClient::CClientData::m_LaserHitDisabled);
                ns.addPropertyReadWrite("m_ShotgunHitDisabled", &CGameClient::CClientData::m_ShotgunHitDisabled);
                ns.addPropertyReadWrite("m_HookHitDisabled", &CGameClient::CClientData::m_HookHitDisabled);
                ns.addPropertyReadWrite("m_HasTelegunGun", &CGameClient::CClientData::m_HasTelegunGun);
                ns.addPropertyReadWrite("m_HasTelegunGrenade", &CGameClient::CClientData::m_HasTelegunGrenade);
                ns.addPropertyReadWrite("m_HasTelegunLaser", &CGameClient::CClientData::m_HasTelegunLaser);
                ns.addPropertyReadWrite("m_FreezeEnd", &CGameClient::CClientData::m_FreezeEnd);
                ns.addPropertyReadWrite("m_AuthLevel", &CGameClient::CClientData::m_AuthLevel);
                ns.addPropertyReadWrite("m_UseCustomColor", &CGameClient::CClientData::m_UseCustomColor);
                ns.addPropertyReadWrite("m_ColorBody", &CGameClient::CClientData::m_ColorBody);
                ns.addPropertyReadWrite("m_ColorFeet", &CGameClient::CClientData::m_ColorFeet);
                ns.addPropertyReadWrite("m_Angle", &CGameClient::CClientData::m_Angle);
                ns.addPropertyReadWrite("m_Emoticon", &CGameClient::CClientData::m_Emoticon);
                ns.addPropertyReadWrite("m_EmoticonStartFraction", &CGameClient::CClientData::m_EmoticonStartFraction);
                ns.addPropertyReadWrite("m_EmoticonStartTick", &CGameClient::CClientData::m_EmoticonStartTick);
                ns.addPropertyReadWrite("m_ChatIgnore", &CGameClient::CClientData::m_ChatIgnore);
                ns.addPropertyReadWrite("m_EmoticonIgnore", &CGameClient::CClientData::m_EmoticonIgnore);
                ns.addPropertyReadWrite("m_FinishTimeSeconds", &CGameClient::CClientData::m_FinishTimeSeconds);
                ns.addPropertyReadWrite("m_FinishTimeMillis", &CGameClient::CClientData::m_FinishTimeMillis);
                ns.addPropertyReadWrite("m_IsPredicted", &CGameClient::CClientData::m_IsPredicted);
                ns.addPropertyReadWrite("m_IsPredictedLocal", &CGameClient::CClientData::m_IsPredictedLocal);
                ns.addPropertyReadWrite("m_SpecCharPresent", &CGameClient::CClientData::m_SpecCharPresent);
                ns.addProperty("m_SpecChar", [](CGameClient::CClientData &d) -> vec2& { return d.m_SpecChar; });
                ns.addProperty("m_RenderPos", [](CGameClient::CClientData &d) -> vec2& { return d.m_RenderPos; });
                ns.addProperty("m_Predicted", [](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_Predicted; });
                ns.addProperty("m_PrevPredicted", [](CGameClient::CClientData &d) -> CCharacterCore& { return d.m_PrevPredicted; });
                ns.addPropertyReadWrite("m_RenderCur", &CGameClient::CClientData::m_RenderCur);
                ns.addPropertyReadWrite("m_RenderPrev", &CGameClient::CClientData::m_RenderPrev);
                ns.addProperty("m_aSwitchStates", [L](CGameClient::CClientData &d) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 256; i++) t[i] = d.m_aSwitchStates[i];
                        return t;
                });
                ns.addFunction("ClientId", &CGameClient::CClientData::ClientId);
                ns.addFunction("Reset", [](CGameClient::CClientData &d) { d.Reset(); });
                ns.addFunction("UpdateRenderInfo", [](CGameClient::CClientData &d) { d.UpdateRenderInfo(); });
                ns.addFunction("UpdateSkinInfo", [](CGameClient::CClientData &d) { d.UpdateSkinInfo(); });
                ns.endClass();
        }

        Stage("collision");
        // ===================== CCollision =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CCollision>("CCollision");
                ns.addPropertyReadWrite("m_HighestSwitchNumber", &CCollision::m_HighestSwitchNumber);
                ns.addFunction("Init", [](CCollision &c, CLayers &l) { c.Init(&l); });
                ns.addFunction("Unload", &CCollision::Unload);
                ns.addFunction("CheckPoint", [](CCollision &c, float x, float y) -> bool { return c.CheckPoint(x, y); }, [](CCollision &c, vec2 p) -> bool { return c.CheckPoint(p); });
                ns.addFunction("GetCollisionAt", [](CCollision &c, float x, float y) -> int { return c.GetCollisionAt(x, y); });
                ns.addFunction("GetWidth", &CCollision::GetWidth);
                ns.addFunction("GetHeight", &CCollision::GetHeight);
                ns.addFunction("IntersectLine", [L](CCollision &c, vec2 p0, vec2 p1) -> luabridge::LuaRef {
                        vec2 col, before; int res = c.IntersectLine(p0, p1, &col, &before);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                });
                ns.addFunction("IntersectLineTeleWeapon", [L](CCollision &c, vec2 p0, vec2 p1) -> luabridge::LuaRef {
                        vec2 col, before; int tele; int res = c.IntersectLineTeleWeapon(p0, p1, &col, &before, &tele);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before; t["TeleNr"] = tele;
                        return t;
                });
                ns.addFunction("IntersectLineTeleHook", [L](CCollision &c, vec2 p0, vec2 p1) -> luabridge::LuaRef {
                        vec2 col, before; int tele; int res = c.IntersectLineTeleHook(p0, p1, &col, &before, &tele);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before; t["TeleNr"] = tele;
                        return t;
                });
                ns.addFunction("MovePoint", [L](CCollision &c, vec2 pos, vec2 vel, float elasticity) -> luabridge::LuaRef {
                        vec2 p = pos, v = vel; int bounces; c.MovePoint(&p, &v, elasticity, &bounces);
                        auto t = luabridge::newTable(L); t["Pos"] = p; t["Vel"] = v; t["Bounces"] = bounces;
                        return t;
                });
                ns.addFunction("MoveBox", [L](CCollision &c, vec2 pos, vec2 vel, vec2 size, vec2 elasticity) -> luabridge::LuaRef {
                        vec2 p = pos, v = vel; bool grounded; c.MoveBox(&p, &v, size, elasticity, &grounded);
                        auto t = luabridge::newTable(L); t["Pos"] = p; t["Vel"] = v; t["Grounded"] = grounded;
                        return t;
                });
                ns.addFunction("TestBox", &CCollision::TestBox);
                ns.addFunction("IsOnGround", &CCollision::IsOnGround);
                ns.addFunction("GetFrontCollisionAt", [](CCollision &c, float x, float y) -> int { return c.GetFrontCollisionAt(x, y); });
                ns.addFunction("IntersectNoLaser", [L](CCollision &c, vec2 p0, vec2 p1) -> luabridge::LuaRef {
                        vec2 col, before; int res = c.IntersectNoLaser(p0, p1, &col, &before);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                });
                ns.addFunction("GetPureMapIndex", [](CCollision &c, float x, float y) -> int { return c.GetPureMapIndex(x, y); }, [](CCollision &c, vec2 p) -> int { return c.GetPureMapIndex(p); });
                ns.addFunction("GetMapIndex", [](CCollision &c, vec2 p) -> int { return c.GetMapIndex(p); });
                ns.addFunction("GetMapIndices", [L](CCollision &c, vec2 prev, vec2 pos, std::optional<unsigned> maxIdx) -> luabridge::LuaRef {
                        auto indices = c.GetMapIndices(prev, pos, maxIdx.value_or(0));
                        auto t = luabridge::newTable(L);
                        for(size_t i = 0; i < indices.size(); i++) t[i+1] = indices[i];
                        return t;
                });
                ns.addFunction("IsSolid", [](CCollision &c, int x, int y) -> int { return c.IsSolid(x, y); });
                ns.addFunction("GetTile", &CCollision::GetTile);
                ns.addFunction("GetFrontTile", &CCollision::GetFrontTile);
                ns.addFunction("GetTileIndex", &CCollision::GetTileIndex);
                ns.addFunction("GetFrontTileIndex", &CCollision::GetFrontTileIndex);
                ns.addFunction("GetTileFlags", &CCollision::GetTileFlags);
                ns.addFunction("GetFrontTileFlags", &CCollision::GetFrontTileFlags);
                ns.addFunction("Entity", &CCollision::Entity);
                ns.addFunction("IsTeleport", [](CCollision &c, int idx) -> int { return c.IsTeleport(idx); });
                ns.addFunction("IsEvilTeleport", &CCollision::IsEvilTeleport);
                ns.addFunction("IsSpeedup", [](CCollision &c, int idx) -> bool { return c.IsSpeedup(idx); });
                ns.addFunction("IsTune", [](CCollision &c, int idx) -> int { return c.IsTune(idx); });
                ns.addFunction("GetSpeedup", [L](CCollision &c, int idx) -> luabridge::LuaRef {
                        vec2 dir; int force, maxSpeed, type; c.GetSpeedup(idx, &dir, &force, &maxSpeed, &type);
                        auto t = luabridge::newTable(L); t["Dir"] = dir; t["Force"] = force; t["MaxSpeed"] = maxSpeed; t["Type"] = type;
                        return t;
                });
                ns.addFunction("GetSwitchType", &CCollision::GetSwitchType);
                ns.addFunction("GetSwitchNumber", &CCollision::GetSwitchNumber);
                ns.addFunction("GetSwitchDelay", &CCollision::GetSwitchDelay);
                ns.addFunction("IsWallJump", &CCollision::IsWallJump);
                ns.addFunction("IsTimeCheckpoint", &CCollision::IsTimeCheckpoint);
                ns.addFunction("IsFrontTimeCheckpoint", &CCollision::IsFrontTimeCheckpoint);
                ns.addFunction("TileExists", &CCollision::TileExists);
                ns.addFunction("TileExistsNext", &CCollision::TileExistsNext);
                ns.addFunction("GetPos", &CCollision::GetPos);
                ns.addFunction("IsNoLaser", [](CCollision &c, int x, int y) -> int { return c.IsNoLaser(x, y); });
                ns.addFunction("Layers", [](CCollision &c) -> const CLayers* { return c.Layers(); });
                ns.addFunction("GetIndex", [](CCollision &c, int x, int y) -> int { return c.GetIndex(x, y); }, [](CCollision &c, vec2 prev, vec2 pos) -> int { return c.GetIndex(prev, pos); });
                ns.addFunction("GetFrontIndex", [](CCollision &c, int x, int y) -> int { return c.GetFrontIndex(x, y); });
                ns.addFunction("GetMoveRestrictions", [](CCollision &c, vec2 pos, std::optional<float> dist) -> int { return c.GetMoveRestrictions(pos, dist.value_or(18.0f)); });
                ns.addFunction("IsCheckTeleport", &CCollision::IsCheckTeleport);
                ns.addFunction("IsCheckEvilTeleport", &CCollision::IsCheckEvilTeleport);
                ns.addFunction("IsTeleportWeapon", &CCollision::IsTeleportWeapon);
                ns.addFunction("IsTeleportHook", &CCollision::IsTeleportHook);
                ns.addFunction("IsTeleCheckpoint", &CCollision::IsTeleCheckpoint);
                ns.addFunction("IsThrough", [](CCollision &c, int x, int y, int ox, int oy, vec2 p0, vec2 p1) -> bool { return c.IsThrough(x, y, ox, oy, p0, p1); });
                ns.addFunction("IsHookBlocker", [](CCollision &c, int x, int y, vec2 p0, vec2 p1) -> bool { return c.IsHookBlocker(x, y, p0, p1); });
                ns.addFunction("IsFrontNoLaser", [](CCollision &c, int x, int y) -> int { return c.IsFrontNoLaser(x, y); });
                ns.addFunction("MoverSpeed", [L](CCollision &c, int x, int y) -> luabridge::LuaRef {
                        vec2 speed; int res = c.MoverSpeed(x, y, &speed);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Speed"] = speed;
                        return t;
                });
                ns.addFunction("SetCollisionAt", [](CCollision &c, float x, float y, int idx) { c.SetCollisionAt(x, y, idx); });
                ns.addFunction("SetDoorCollisionAt", [](CCollision &c, float x, float y, int type, int flags, int number) { c.SetDoorCollisionAt(x, y, (unsigned char)type, (unsigned char)flags, (unsigned char)number); });
                ns.addFunction("IntersectNoLaserNoWalls", [L](CCollision &c, vec2 p0, vec2 p1) -> luabridge::LuaRef {
                        vec2 col, before; int res = c.IntersectNoLaserNoWalls(p0, p1, &col, &before);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                });
                ns.addFunction("IntersectAir", [L](CCollision &c, vec2 p0, vec2 p1) -> luabridge::LuaRef {
                        vec2 col, before; int res = c.IntersectAir(p0, p1, &col, &before);
                        auto t = luabridge::newTable(L); t["hit"] = res; t["Collision"] = col; t["BeforeCollision"] = before;
                        return t;
                });
                ns.addFunction("TeleOuts", [L](CCollision &c, int number) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = c.TeleOuts(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                });
                ns.addFunction("TeleIns", [L](CCollision &c, int number) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = c.TeleIns(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                });
                ns.addFunction("TeleCheckOuts", [L](CCollision &c, int number) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = c.TeleCheckOuts(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                });
                ns.addFunction("TeleOthers", [L](CCollision &c, int number) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = c.TeleOthers(number);
                        for(size_t i = 0; i < v.size(); i++) t[i+1] = v[i];
                        return t;
                });
                ns.addFunction("TeleAllSize", [](CCollision &c, int number) -> int { return (int)c.TeleAllSize(number); });
                ns.addFunction("TeleAllGet", [](CCollision &c, int number, int offset) -> vec2 { return c.TeleAllGet(number, (size_t)offset); });
                ns.endClass();
        }

        Stage("components");
        // ===================== ALL COMPONENT CLASSES =====================

        // Components reachable as gc.* members but without dedicated bindings:
        // register the (empty) classes so pushing the pointers doesn't throw.
        {
                luabridge::getGlobalNamespace(L).beginClass<CPlayers>("CPlayers").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CNamePlates>("CNamePlates").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CFreezeBars>("CFreezeBars").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CItems>("CItems").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CMapImages>("CMapImages").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CMapSounds>("CMapSounds").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CBackground>("CBackground").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CMenuBackground>("CMenuBackground").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CMapLayers>("CMapLayers").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CDebugHud>("CDebugHud").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CTouchControls>("CTouchControls").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CCountryFlags>("CCountryFlags").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CSkins7>("CSkins7").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CRenderTools>("CRenderTools").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CRenderMap>("CRenderMap").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CInfoMessages>("CInfoMessages").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CCensor>("CCensor").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CTooltips>("CTooltips").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CAimBot>("CAimBot").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CAvoidFreeze>("CAvoidFreeze").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CEsp>("CEsp").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CTrajectory>("CTrajectory").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CClickGui>("CClickGui").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CKinetixLines>("CKinetixLines").endClass();
                luabridge::getGlobalNamespace(L).beginClass<CKeyBinder>("CKeyBinder").endClass();
        }

        // CCamera
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CCamera>("CCamera");
                ns.addProperty("m_Center", [](CCamera &cam) -> vec2& { return cam.m_Center; });
                ns.addPropertyReadWrite("m_ZoomSet", &CCamera::m_ZoomSet);
                ns.addPropertyReadWrite("m_Zooming", &CCamera::m_Zooming);
                ns.addPropertyReadWrite("m_Zoom", &CCamera::m_Zoom);
                ns.addPropertyReadWrite("m_ZoomSmoothingTarget", &CCamera::m_ZoomSmoothingTarget);
                ns.addPropertyReadWrite("m_AutoSpecCameraZooming", &CCamera::m_AutoSpecCameraZooming);
                ns.addPropertyReadWrite("m_AutoSpecCamera", &CCamera::m_AutoSpecCamera);
                ns.addPropertyReadWrite("m_UserZoomTarget", &CCamera::m_UserZoomTarget);
                ns.addProperty("m_DyncamTargetCameraOffset", [](CCamera &cam) -> vec2& { return cam.m_DyncamTargetCameraOffset; });
                ns.addFunction("SetZoom", [](CCamera &cam, float target, int smooth, bool isUser) { cam.SetZoom(target, smooth, isUser); });
                ns.addFunction("ZoomAllowed", [](CCamera &cam) -> bool { return cam.ZoomAllowed(); });
                ns.addFunction("Deadzone", [](CCamera &cam) -> int { return cam.Deadzone(); });
                ns.addFunction("FollowFactor", [](CCamera &cam) -> int { return cam.FollowFactor(); });
                ns.addFunction("CamType", [](CCamera &cam) -> int { return cam.CamType(); });
                ns.addStaticFunction("ZoomStepsToValue", &CCamera::ZoomStepsToValue);
                ns.addFunction("SpectatingPlayer", [](CCamera &cam) -> bool { return cam.SpectatingPlayer(); });
                ns.addFunction("ResetAutoSpecCamera", [](CCamera &cam) { cam.ResetAutoSpecCamera(); });
                ns.endClass();
        }

        // CChat
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CChat>("CChat");
                ns.addFunction("IsActive", [](CChat &c) -> bool { return c.IsActive(); });
                ns.addFunction("AddLine", [](CChat &c, int cid, int team, const std::string &msg) { c.AddLine(cid, team, msg.c_str()); });
                ns.addFunction("Echo", [](CChat &c, const std::string &msg) { c.Echo(msg.c_str()); });
                ns.addFunction("SendChat", [](CChat &c, int team, const std::string &msg) { c.SendChat(team, msg.c_str()); });
                ns.addFunction("SendChatQueued", [](CChat &c, const std::string &msg) { c.SendChatQueued(msg.c_str()); });
                ns.addFunction("EnableMode", [](CChat &c, int team) { c.EnableMode(team); });
                ns.addFunction("DisableMode", [](CChat &c) { c.DisableMode(); });
                ns.addFunction("ClearLines", [](CChat &c) { c.ClearLines(); });
                ns.addFunction("RegisterCommand", [](CChat &c, const std::string &name, const std::string &params, const std::string &help) { c.RegisterCommand(name.c_str(), params.c_str(), help.c_str()); });
                ns.addFunction("UnregisterCommand", [](CChat &c, const std::string &name) { c.UnregisterCommand(name.c_str()); });
                ns.addFunction("FontSize", [](CChat &c) -> float { return c.FontSize(); });
                ns.addFunction("GetRecentLines", [L](CChat &c, std::optional<int> Count) -> luabridge::LuaRef {
                        int Want = Count.value_or(20);
                        if(Want < 1)
                                Want = 1;
                        if(Want > 64)
                                Want = 64;
                        CChat::CChatLineInfo aLines[64];
                        const int Num = c.GetRecentLines(aLines, Want);
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < Num; i++)
                        {
                                auto e = luabridge::newTable(L);
                                e["time"] = aLines[i].m_Time;
                                e["clientid"] = aLines[i].m_ClientId;
                                e["team"] = aLines[i].m_TeamNumber;
                                e["name"] = std::string(aLines[i].m_aName);
                                e["text"] = std::string(aLines[i].m_aText);
                                t[i + 1] = e;
                        }
                        return t;
                });
                ns.addFunction("SendChatFrom", [](CChat &c, int Dummy, int Team, const std::string &Msg) { c.SendChatFrom(Dummy, Team, Msg.c_str()); });
                ns.endClass();
        }

        // CControls
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CControls>("CControls");
                ns.addProperty("m_aInputData", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = &c.m_aInputData[i];
                        return t;
                });
                ns.addProperty("m_aLastData", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = &c.m_aLastData[i];
                        return t;
                });
                ns.addProperty("m_aMousePos", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aMousePos[i];
                        return t;
                });
                ns.addProperty("m_aTargetPos", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aTargetPos[i];
                        return t;
                });
                ns.addProperty("m_aAmmoCount", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < NUM_WEAPONS; i++) t[i] = c.m_aAmmoCount[i];
                        return t;
                });
                ns.addPropertyReadWrite("m_LastSendTime", &CControls::m_LastSendTime);
                ns.addProperty("m_aInputDirectionLeft", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aInputDirectionLeft[i];
                        return t;
                });
                ns.addProperty("m_aInputDirectionRight", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aInputDirectionRight[i];
                        return t;
                });
                ns.addProperty("m_aShowHookColl", [L](CControls &c) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = c.m_aShowHookColl[i];
                        return t;
                });
                ns.addFunction("ResetInput", [](CControls &c, int d) { c.ResetInput(d); });
                ns.addFunction("ClampMousePos", [](CControls &c) { c.ClampMousePos(); });
                ns.addFunction("GetMinMouseDistance", [](CControls &c) -> float { return c.GetMinMouseDistance(); });
                ns.addFunction("GetMaxMouseDistance", [](CControls &c) -> float { return c.GetMaxMouseDistance(); });
                ns.endClass();
        }

        // CEffects
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CEffects>("CEffects");
                ns.addFunction("BulletTrail", [](CEffects &e, vec2 p, float a, float t) { e.BulletTrail(p, a, t); });
                ns.addFunction("SmokeTrail", [](CEffects &e, vec2 p, vec2 v, float a, float t) { e.SmokeTrail(p, v, a, t); });
                ns.addFunction("SkidTrail", [](CEffects &e, vec2 p, vec2 v, int d, float a, float vol) { e.SkidTrail(p, v, d, a, vol); });
                ns.addFunction("Explosion", [](CEffects &e, vec2 p, float a) { e.Explosion(p, a); });
                ns.addFunction("HammerHit", [](CEffects &e, vec2 p, float a, float vol) { e.HammerHit(p, a, vol); });
                ns.addFunction("AirJump", [](CEffects &e, vec2 p, float a, float vol) { e.AirJump(p, a, vol); });
                ns.addFunction("DamageIndicator", [](CEffects &e, vec2 p, vec2 d, float a) { e.DamageIndicator(p, d, a); });
                ns.addFunction("PlayerSpawn", [](CEffects &e, vec2 p, float a, float vol) { e.PlayerSpawn(p, a, vol); });
                ns.addFunction("PlayerDeath", [](CEffects &e, vec2 p, int cid, float a) { e.PlayerDeath(p, cid, a); });
                ns.addFunction("PowerupShine", [](CEffects &e, vec2 p, vec2 s, float a) { e.PowerupShine(p, s, a); });
                ns.addFunction("FreezingFlakes", [](CEffects &e, vec2 p, vec2 s, float a) { e.FreezingFlakes(p, s, a); });
                ns.addFunction("SparkleTrail", [](CEffects &e, vec2 p, float a) { e.SparkleTrail(p, a); });
                ns.addFunction("Confetti", [](CEffects &e, vec2 p, float a) { e.Confetti(p, a); });
                ns.endClass();
        }

        // CFlow
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CFlow>("CFlow");
                ns.addFunction("Get", [](CFlow &f, vec2 p) -> vec2 { return f.Get(p); });
                ns.addFunction("Add", [](CFlow &f, vec2 p, vec2 v, float s) { f.Add(p, v, s); });
                ns.addFunction("Update", [](CFlow &f) { f.Update(); });
                ns.endClass();
        }

        // CVoting
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CVoting>("CVoting");
                ns.addFunction("CallvoteSpectate", [](CVoting &v, int cid, const std::string &r, bool f) { v.CallvoteSpectate(cid, r.c_str(), f); });
                ns.addFunction("CallvoteKick", [](CVoting &v, int cid, const std::string &r, bool f) { v.CallvoteKick(cid, r.c_str(), f); });
                ns.addFunction("Vote", [](CVoting &v, int val) { v.Vote(val); });
                ns.addFunction("SecondsLeft", [](CVoting &v) -> int { return v.SecondsLeft(); });
                ns.addFunction("IsVoting", [](CVoting &v) -> bool { return v.IsVoting(); });
                ns.addFunction("TakenChoice", [](CVoting &v) -> int { return v.TakenChoice(); });
                ns.addFunction("VoteDescription", [](CVoting &v) -> std::string { return std::string(v.VoteDescription()); });
                ns.addFunction("VoteReason", [](CVoting &v) -> std::string { return std::string(v.VoteReason()); });
                ns.addFunction("IsReceivingOptions", [](CVoting &v) -> bool { return v.IsReceivingOptions(); });
                ns.addFunction("NumOptions", [](CVoting &v) -> int { return v.NumOptions(); });
                ns.addFunction("CallvoteOption", [](CVoting &v, int OptionId, const std::string &Reason, std::optional<bool> Force) { v.CallvoteOption(OptionId, Reason.c_str(), Force.value_or(false)); });
                ns.addFunction("RemovevoteOption", [](CVoting &v, int OptionId) { v.RemovevoteOption(OptionId); });
                ns.addFunction("AddvoteOption", [](CVoting &v, const std::string &Description, const std::string &Command) { v.AddvoteOption(Description.c_str(), Command.c_str()); });
                ns.endClass();
        }

        // CSpectator
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CSpectator>("CSpectator");
                ns.addFunction("Spectate", [](CSpectator &s, int id) { s.Spectate(id); });
                ns.addFunction("SpectateClosest", [](CSpectator &s) { s.SpectateClosest(); });
                ns.addFunction("IsActive", [](CSpectator &s) -> bool { return s.IsActive(); });
                ns.endClass();
        }

        // CEmoticon
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CEmoticon>("CEmoticon");
                ns.addFunction("Emote", [](CEmoticon &e, int t) { e.Emote(t); });
                ns.addFunction("EyeEmote", [](CEmoticon &e, int t) { e.EyeEmote(t); });
                ns.addFunction("IsActive", [](CEmoticon &e) -> bool { return e.IsActive(); });
                ns.endClass();
        }

        // CMotd
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CMotd>("CMotd");
                ns.addFunction("ServerMotd", [](CMotd &m) -> std::string { return std::string(m.ServerMotd()); });
                ns.addFunction("ServerMotdUpdateTime", [](CMotd &m) -> int64_t { return m.ServerMotdUpdateTime(); });
                ns.addFunction("Clear", [](CMotd &m) { m.Clear(); });
                ns.addFunction("IsActive", [](CMotd &m) -> bool { return m.IsActive(); });
                ns.endClass();
        }

        // CBroadcast
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CBroadcast>("CBroadcast");
                ns.addFunction("DoBroadcast", [](CBroadcast &b, const std::string &s) { b.DoBroadcast(s.c_str()); });
                ns.endClass();
        }

        // CGhost
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGhost>("CGhost");
                ns.addPropertyReadWrite("m_AllowRestart", &CGhost::m_AllowRestart);
                ns.addFunction("FreeSlots", [](CGhost &g) -> int { return g.FreeSlots(); });
                ns.addFunction("Load", [](CGhost &g, const std::string &f) { g.Load(f.c_str()); });
                ns.addFunction("Unload", [](CGhost &g, int s) { g.Unload(s); });
                ns.addFunction("UnloadAll", [](CGhost &g) { g.UnloadAll(); });
                ns.addFunction("GetGhostDir", [](CGhost &g) -> std::string { return std::string(g.GetGhostDir()); });
                ns.endClass();
        }

        // CBotControl
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CBotControl>("CBotControl");
                ns.addFunction("ActionInput", [](CBotControl &b, const std::string &a, int ms, int d) { b.ActionInput(a.c_str(), ms, d); });
                ns.addFunction("ActionAim", [](CBotControl &b, int dx, int dy, int d) { b.ActionAim(dx, dy, d); });
                ns.addFunction("ActionOverrideAim", [](CBotControl &b, int x, int y, int d) { b.ActionOverrideAim(x, y, d); });
                ns.addFunction("ActionStop", [](CBotControl &b, int d) { b.ActionStop(d); });
                ns.addFunction("IsBotActiveJump", [](CBotControl &b, int d) -> bool { return b.IsBotActiveJump(d); });
                ns.addFunction("IsBotActiveHook", [](CBotControl &b, int d) -> bool { return b.IsBotActiveHook(d); });
                ns.addFunction("IsBotActiveFire", [](CBotControl &b, int d) -> bool { return b.IsBotActiveFire(d); });
                ns.addFunction("IsBotActiveMove", [](CBotControl &b, int d) -> bool { return b.IsBotActiveMove(d); });
                ns.addFunction("IsBotActiveAim", [](CBotControl &b, int d) -> bool { return b.IsBotActiveAim(d); });
                ns.addFunction("IsBotActiveAny", [](CBotControl &b, int d) -> bool { return b.IsBotActiveAny(d); });
                ns.endClass();
        }

        // CBotNet
        // v1.56.171 BUG9: kx_ cvar bindings removed — they are now MACRO_CONFIG_INT
        // globals (g_Config.m_KxHelpers etc.), not CBotNet members. Lua scripts access
        // them via the global g_Config usertype instead.
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CBotNet>("CBotNet");
                ns.addPropertyReadWrite("m_MapWidth", &CBotNet::m_MapWidth);
                ns.addPropertyReadWrite("m_MapHeight", &CBotNet::m_MapHeight);
                ns.addPropertyReadWrite("m_MapGridLoaded", &CBotNet::m_MapGridLoaded);
                ns.addProperty("m_aLastMapName", [](CBotNet &b) -> std::string { return std::string(b.m_aLastMapName); });
                ns.addFunction("IsDummyActive", [](CBotNet &b, int d) -> bool { return b.IsDummyActive(d); });
                ns.addFunction("ResetDummyInputs", [](CBotNet &b, int d) { b.ResetDummyInputs(d); });
                ns.addFunction("IsTileWalkable", [](CBotNet &b, int tx, int ty) -> bool { return b.IsTileWalkable(tx, ty); });
                ns.addFunction("IsTileFreeze", [](CBotNet &b, int tx, int ty) -> bool { return b.IsTileFreeze(tx, ty); });
                ns.addFunction("ProcessDummy", [](CBotNet &b, int d) { b.ProcessDummy(d); });
                ns.endClass();
        }

        // CIRC
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CIRC>("CIRC");
                ns.addFunction("SendIRC", [](CIRC &i, const std::string &s) { i.SendIRC(s.c_str()); });
                ns.addFunction("GetClientPrefix", [](CIRC &i, int id) -> std::string { return std::string(i.GetClientPrefix(id)); });
                ns.endClass();
        }

        // CParticles
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CParticles>("CParticles");
                ns.addFunction("Add", [](CParticles &p, int group, luabridge::LuaRef part, float timePassed) { /* simplified */ (void)p; (void)group; (void)part; (void)timePassed; });
                ns.endClass();
        }

        // CDamageInd
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CDamageInd>("CDamageInd");
                ns.addFunction("Create", [](CDamageInd &d, vec2 pos, vec2 dir, float alpha) { d.Create(pos, dir, alpha); });
                ns.endClass();
        }

        // CSounds
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CSounds>("CSounds");
                ns.addFunction("Play", [](CSounds &s, int ch, int sid, float vol) { s.Play(ch, sid, vol); });
                ns.addFunction("PlayAt", [](CSounds &s, int ch, int sid, float vol, vec2 pos) { s.PlayAt(ch, sid, vol, pos); });
                ns.addFunction("Stop", [](CSounds &s, int sid) { s.Stop(sid); });
                ns.addFunction("IsPlaying", [](CSounds &s, int sid) -> bool { return s.IsPlaying(sid); });
                ns.addFunction("ClearQueue", [](CSounds &s) { s.ClearQueue(); });
                ns.addFunction("Enqueue", [](CSounds &s, int Channel, int SetId) { s.Enqueue(Channel, SetId); });
                ns.addFunction("PlayAndRecord", [](CSounds &s, int Channel, int SetId, float Volume, vec2 Position) { s.PlayAndRecord(Channel, SetId, Volume, Position); });
                ns.endClass();
        }

        // CMenus
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CMenus>("CMenus");
                ns.addPropertyReadWrite("m_Dummy", &CMenus::m_Dummy);
                ns.addFunction("IsInit", [](CMenus &m) -> bool { return m.IsInit(); });
                ns.addFunction("IsActive", [](CMenus &m) -> bool { return m.IsActive(); });
                ns.addFunction("SetActive", [](CMenus &m, bool a) { m.SetActive(a); });
                ns.addFunction("SetMenuPage", [](CMenus &m, int p) { m.SetMenuPage(p); });
                ns.endClass();
        }

        // CBinds
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CBinds>("CBinds");
                // 20.2 moved this flag from CBinds to CControls (per dummy).
                ns.addProperty(
                        "m_MouseOnAction",
                        [this](CBinds &) -> bool { return GameClient()->m_Controls.m_aMouseOnAction[g_Config.m_ClDummy]; },
                        [this](CBinds &, bool Value) { GameClient()->m_Controls.m_aMouseOnAction[g_Config.m_ClDummy] = Value; });
                ns.addFunction("Bind", [](CBinds &b, int key, const std::string &str, bool free, int mod) { b.Bind(key, str.c_str(), free, mod); });
                ns.addFunction("Get", [](CBinds &b, int key, int mod) -> std::string { return std::string(b.Get(key, mod)); });
                ns.addFunction("SetDefaults", [](CBinds &b) { b.SetDefaults(); });
                ns.addFunction("UnbindAll", [](CBinds &b) { b.UnbindAll(); });
                ns.endClass();
        }

        // CGameConsole
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameConsole>("CGameConsole");
                ns.addFunction("PrintLine", [](CGameConsole &c, int type, const std::string &line) { c.PrintLine(type, line.c_str()); });
                ns.addFunction("Toggle", [](CGameConsole &c, int type) { c.Toggle(type); });
                ns.addFunction("IsActive", [](CGameConsole &c) -> bool { return c.IsActive(); });
                ns.endClass();
        }

        // CHud
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CHud>("CHud");
                ns.addFunction("RenderNinjaBarPos", [](CHud &h, float x, float y, float w, float ht, float progress, float alpha) { h.RenderNinjaBarPos(x, y, w, ht, progress, alpha); });
                ns.endClass();
        }

        // CRaceDemo
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CRaceDemo>("CRaceDemo");
                ns.addPropertyReadWrite("m_AllowRestart", &CRaceDemo::m_AllowRestart);
                ns.endClass();
        }

        // CLocalServer
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CLocalServer>("CLocalServer");
                ns.addFunction("IsServerRunning", [](CLocalServer &l) -> bool { return l.IsServerRunning(); });
                ns.addFunction("KillServer", [](CLocalServer &l) { l.KillServer(); });
                ns.endClass();
        }

        // CImportantAlert
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CImportantAlert>("CImportantAlert");
                ns.addFunction("IsActive", [](CImportantAlert &a) -> bool { return a.IsActive(); });
                ns.endClass();
        }

        // CScoreboard
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CScoreboard>("CScoreboard");
                ns.addFunction("IsActive", [](CScoreboard &s) -> bool { return s.IsActive(); });
                ns.endClass();
        }

        // CStatboard
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CStatboard>("CStatboard");
                ns.addFunction("IsActive", [](CStatboard &s) -> bool { return s.IsActive(); });
                ns.endClass();
        }

        Stage("map");
        // ===================== Map structures =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CTile>("CTile");
                ns.addPropertyReadWrite("m_Index", &CTile::m_Index);
                ns.addPropertyReadWrite("m_Flags", &CTile::m_Flags);
                ns.addPropertyReadWrite("m_Skip", &CTile::m_Skip);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CMapItemLayer>("CMapItemLayer");
                ns.addPropertyReadWrite("m_Version", &CMapItemLayer::m_Version);
                ns.addPropertyReadWrite("m_Type", &CMapItemLayer::m_Type);
                ns.addPropertyReadWrite("m_Flags", &CMapItemLayer::m_Flags);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CMapItemGroup>("CMapItemGroup");
                AddField(ns, "m_Version", &CMapItemGroup_v1::m_Version);
                AddField(ns, "m_OffsetX", &CMapItemGroup_v1::m_OffsetX);
                AddField(ns, "m_OffsetY", &CMapItemGroup_v1::m_OffsetY);
                AddField(ns, "m_ParallaxX", &CMapItemGroup_v1::m_ParallaxX);
                AddField(ns, "m_ParallaxY", &CMapItemGroup_v1::m_ParallaxY);
                AddField(ns, "m_StartLayer", &CMapItemGroup_v1::m_StartLayer);
                AddField(ns, "m_NumLayers", &CMapItemGroup_v1::m_NumLayers);
                AddField(ns, "m_UseClipping", &CMapItemGroup::m_UseClipping);
                AddField(ns, "m_ClipX", &CMapItemGroup::m_ClipX);
                AddField(ns, "m_ClipY", &CMapItemGroup::m_ClipY);
                AddField(ns, "m_ClipW", &CMapItemGroup::m_ClipW);
                AddField(ns, "m_ClipH", &CMapItemGroup::m_ClipH);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CMapItemLayerTilemap>("CMapItemLayerTilemap");
                AddField(ns, "m_Layer", &CMapItemLayerTilemap_v2::m_Layer);
                AddField(ns, "m_Version", &CMapItemLayerTilemap_v2::m_Version);
                AddField(ns, "m_Width", &CMapItemLayerTilemap_v2::m_Width);
                AddField(ns, "m_Height", &CMapItemLayerTilemap_v2::m_Height);
                AddField(ns, "m_Flags", &CMapItemLayerTilemap_v2::m_Flags);
                AddField(ns, "m_Image", &CMapItemLayerTilemap_v2::m_Image);
                AddField(ns, "m_Data", &CMapItemLayerTilemap_v2::m_Data);
                AddField(ns, "m_Tele", &CMapItemLayerTilemap::m_Tele);
                AddField(ns, "m_Speedup", &CMapItemLayerTilemap::m_Speedup);
                AddField(ns, "m_Front", &CMapItemLayerTilemap::m_Front);
                AddField(ns, "m_Switch", &CMapItemLayerTilemap::m_Switch);
                AddField(ns, "m_Tune", &CMapItemLayerTilemap::m_Tune);
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CLayers>("CLayers");
                ns.addFunction("NumGroups", &CLayers::NumGroups);
                ns.addFunction("NumLayers", &CLayers::NumLayers);
                ns.addFunction("GameGroup", [](CLayers &l) -> CMapItemGroup* { return l.GameGroup(); });
                ns.addFunction("GameLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.GameLayer(); });
                ns.addFunction("GetGroup", [](CLayers &l, int Index) -> CMapItemGroup* { return l.GetGroup(Index); });
                ns.addFunction("GetLayer", [](CLayers &l, int Index) -> CMapItemLayer* { return l.GetLayer(Index); });
                ns.addFunction("TeleLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.TeleLayer(); });
                ns.addFunction("SpeedupLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.SpeedupLayer(); });
                ns.addFunction("FrontLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.FrontLayer(); });
                ns.addFunction("SwitchLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.SwitchLayer(); });
                ns.addFunction("TuneLayer", [](CLayers &l) -> CMapItemLayerTilemap* { return l.TuneLayer(); });
                ns.endClass();
        }

        Stage("prediction");
        // ===================== Prediction entities =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CEntity>("CEntity");
                ns.addFunction("GetId", &CEntity::GetId);
                ns.addFunction("GetPos", [](CEntity &e) -> vec2 { return e.GetPos(); });
                ns.addPropertyReadWrite("m_ProximityRadius", &CEntity::m_ProximityRadius);
                ns.addProperty("m_Pos", [](CEntity &e) -> vec2& { return e.m_Pos; });
                ns.addPropertyReadWrite("m_Number", &CEntity::m_Number);
                ns.addPropertyReadWrite("m_Layer", &CEntity::m_Layer);
                ns.addPropertyReadWrite("m_SnapTicks", &CEntity::m_SnapTicks);
                ns.addPropertyReadWrite("m_DestroyTick", &CEntity::m_DestroyTick);
                ns.addFunction("GameWorld", [](CEntity &e) -> CGameWorld* { return e.GameWorld(); });
                ns.addFunction("Collision", [](CEntity &e) -> CCollision* { return e.Collision(); });
                ns.addFunction("GetTuning", [](CEntity &e, int i) -> CTuningParams* { return e.GetTuning(i); });
                ns.addFunction("TypeNext", [](CEntity &e) -> CEntity* { return e.TypeNext(); });
                ns.addFunction("NextEntity", [](CEntity &e) -> CEntity* { return e.NextEntity(); });
                ns.addFunction("CanCollide", &CEntity::CanCollide);
                ns.addFunction("Keep", [](CEntity &e) { e.Keep(); });
                ns.addFunction("Tick", [](CEntity &e) { e.Tick(); });
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).deriveClass<CCharacter, CEntity>("CCharacter");
                ns.addFunction("Core", [](CCharacter &c) -> const CCharacterCore* { return c.Core(); });
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).deriveClass<CProjectile, CEntity>("CProjectile");
                ns.addFunction("GetDirection", [](CProjectile &p) -> vec2 { return p.GetDirection(); });
                ns.addFunction("GetOwner", [](CProjectile &p) -> int { return p.GetOwner(); });
                ns.addFunction("GetStartTick", [](CProjectile &p) -> int { return p.GetStartTick(); });
                ns.addFunction("GetType", [](CProjectile &p) -> int { return p.GetType(); });
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).deriveClass<CLaser, CEntity>("CLaser");
                ns.addFunction("GetOwner", [](CLaser &l) -> int { return l.GetOwner(); });
                ns.endClass();
        }

        {
                auto ns = luabridge::getGlobalNamespace(L).deriveClass<CPickup, CEntity>("CPickup");
                ns.addFunction("Type", &CPickup::Type);
                ns.endClass();
        }

        Stage("skins");
        // ===================== CSkins =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CSkins>("CSkins");
                ns.addFunction("IsFavorite", [](CSkins &s, const std::string &Name) -> bool { return s.IsFavorite(Name.c_str()); });
                ns.addFunction("AddFavorite", [](CSkins &s, const std::string &Name) { s.AddFavorite(Name.c_str()); });
                ns.addFunction("RemoveFavorite", [](CSkins &s, const std::string &Name) { s.RemoveFavorite(Name.c_str()); });
                ns.addFunction("RandomizeSkin", [](CSkins &s, int Dummy) { s.RandomizeSkin(Dummy); });
                ns.addFunction("SkinPrefix", [](CSkins &s) -> std::string { return std::string(s.SkinPrefix()); });
                ns.addFunction("IsSpecialSkin", [](CSkins &s, const std::string &Name) -> bool { return CSkins::IsSpecialSkin(Name.c_str()); });
                ns.endClass();
        }

        Stage("codeexec");
        // ===================== CCodeExec =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CCodeExec>("CCodeExec");
                ns.addStaticProperty("STATE_IDLE", []() -> int { return CCodeExec::STATE_IDLE; });
                ns.addStaticProperty("STATE_RUNNING", []() -> int { return CCodeExec::STATE_RUNNING; });
                ns.addStaticProperty("STATE_ERROR", []() -> int { return CCodeExec::STATE_ERROR; });
                ns.addFunction("Execute", [](CCodeExec &e, const std::string &Code, std::optional<std::string> Chunk) -> bool { return e.Execute(Code.c_str(), Chunk.value_or("script").c_str()); });
                ns.addFunction("Stop", [](CCodeExec &e) { e.Stop(); });
                ns.addFunction("GetState", [](CCodeExec &e) -> int { return (int)e.GetState(); });
                ns.addFunction("GetLastError", [](CCodeExec &e) -> std::string { return std::string(e.GetLastError()); });
                ns.addFunction("GetLineCount", [](CCodeExec &e) -> int { return e.GetLineCount(); });
                ns.addProperty("m_aCodeBuffer", [](CCodeExec &e) -> std::string { return std::string(e.m_aCodeBuffer); });
                ns.addFunction("SetCodeBuffer", [](CCodeExec &e, const std::string &Code) { str_copy(e.m_aCodeBuffer, Code.c_str(), sizeof(e.m_aCodeBuffer)); });
                ns.addPropertyReadWrite("m_EditorActive", &CCodeExec::m_EditorActive);
                ns.addPropertyReadWrite("m_CursorPos", &CCodeExec::m_CursorPos);
                ns.addPropertyReadWrite("m_SelectionStart", &CCodeExec::m_SelectionStart);
                ns.addPropertyReadWrite("m_SelectionEnd", &CCodeExec::m_SelectionEnd);
                ns.addPropertyReadWrite("m_ScrollY", &CCodeExec::m_ScrollY);
                ns.endClass();
        }

        Stage("colorrgba");
        // ===================== ColorRGBA =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<ColorRGBA>("ColorRGBA");
                AddField(ns, "r", &ColorRGBA::r);
                AddField(ns, "g", &ColorRGBA::g);
                AddField(ns, "b", &ColorRGBA::b);
                AddField(ns, "a", &ColorRGBA::a);
                ns.endClass();
        }

        Stage("gameworld");
        // ===================== CGameWorld (prediction) =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameWorld>("CGameWorld");
                ns.addProperty("m_Core", [](CGameWorld &w) -> CWorldCore& { return w.m_Core; });
                ns.addProperty("m_Teams", [](CGameWorld &w) -> CTeamsCore& { return w.m_Teams; });
                ns.addPropertyReadWrite("m_GameTick", &CGameWorld::m_GameTick);
                ns.addProperty("m_WorldConfig", [L](CGameWorld &w) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
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
                });
                ns.addFunction("GameTick", [](CGameWorld &w) -> int { return w.GameTick(); });
                ns.addFunction("GameTickSpeed", [](CGameWorld &w) -> int { return w.GameTickSpeed(); });
                ns.addFunction("Collision", [](CGameWorld &w) -> CCollision* { return w.Collision(); });
                ns.addFunction("Teams", [](CGameWorld &w) -> CTeamsCore* { return w.Teams(); });
                // Live reference table (see CWorldCore::m_vSwitchers).
                ns.addFunction("Switchers", [L](CGameWorld &w) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = w.Switchers();
                        for(size_t i = 0; i < v.size(); i++) t[(int)i] = &v[i];
                        return t;
                });
                ns.addFunction("FindFirst", [](CGameWorld &w, int type) -> CEntity* { return w.FindFirst(type); });
                ns.addFunction("GetEntity", [](CGameWorld &w, int id, int type) -> CEntity* { return w.GetEntity(id, type); });
                ns.addFunction("GetCharacterById", [](CGameWorld &w, int id) -> CCharacter* { return w.GetCharacterById(id); });
                ns.addFunction("Tick", [](CGameWorld &w) { w.Tick(); });
                ns.addFunction("Clear", [](CGameWorld &w) { w.Clear(); });
                ns.addFunction("IsLocalTeam", [](CGameWorld &w, int id) -> bool { return w.IsLocalTeam(id); });
                ns.addFunction("GlobalTuning", [](CGameWorld &w) -> CTuningParams* { return w.GlobalTuning(); });
                ns.addFunction("GetTuning", [](CGameWorld &w, int i) -> CTuningParams* { return w.GetTuning(i); });
                ns.addFunction("TuningList", [](CGameWorld &w) -> CTuningParams* { return w.TuningList(); });
                ns.addFunction("FindEntities", [L](CGameWorld &w, vec2 pos, float radius, int type, int max) -> luabridge::LuaRef {
                        std::vector<CEntity*> ents(max);
                        int num = w.FindEntities(pos, radius, ents.data(), max, type);
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < num; i++) t[i] = ents[i];
                        return t;
                });
                ns.endClass();
        }

        Stage("gameclient");
        // ===================== CGameClient (ROOT) =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CGameClient>("CGameClient");
                // Predicted chars
                ns.addProperty("m_PredictedChar", [](CGameClient &gc) -> CCharacterCore& { return gc.m_PredictedChar; });
                ns.addProperty("m_PredictedPrevChar", [](CGameClient &gc) -> CCharacterCore& { return gc.m_PredictedPrevChar; });
                ns.addProperty("m_LocalCharacterPos", [](CGameClient &gc) -> vec2& { return gc.m_LocalCharacterPos; });
                // State
                ns.addPropertyReadWrite("m_NewTick", &CGameClient::m_NewTick);
                ns.addPropertyReadWrite("m_NewPredictedTick", &CGameClient::m_NewPredictedTick);
                ns.addPropertyReadWrite("m_SuppressEvents", &CGameClient::m_SuppressEvents);
                ns.addPropertyReadWrite("m_ServerMode", &CGameClient::m_ServerMode);
                ns.addPropertyReadWrite("m_DemoSpecId", &CGameClient::m_DemoSpecId);
                ns.addPropertyReadWrite("m_ReceivedDDNetPlayer", &CGameClient::m_ReceivedDDNetPlayer);
                ns.addPropertyReadWrite("m_ReceivedDDNetPlayerFinishTimes", &CGameClient::m_ReceivedDDNetPlayerFinishTimes);
                ns.addPropertyReadWrite("m_ReceivedDDNetPlayerFinishTimesMillis", &CGameClient::m_ReceivedDDNetPlayerFinishTimesMillis);
                // MultiView
                ns.addPropertyReadWrite("m_MultiViewTeam", &CGameClient::m_MultiViewTeam);
                ns.addPropertyReadWrite("m_MultiViewPersonalZoom", &CGameClient::m_MultiViewPersonalZoom);
                ns.addPropertyReadWrite("m_MultiViewActivated", &CGameClient::m_MultiViewActivated);
                ns.addPropertyReadWrite("m_MultiViewShowHud", &CGameClient::m_MultiViewShowHud);
                ns.addProperty("m_aMultiViewId", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = gc.m_aMultiViewId[i];
                        return t;
                });
                // Map time
                ns.addPropertyReadWrite("m_MapBestTimeSeconds", &CGameClient::m_MapBestTimeSeconds);
                ns.addPropertyReadWrite("m_MapBestTimeMillis", &CGameClient::m_MapBestTimeMillis);
                ns.addProperty("m_aMapDescription", [](CGameClient &gc) -> std::string { return std::string(gc.m_aMapDescription); });
                // Snap
                ns.addProperty("m_Snap", [](CGameClient &gc) -> CGameClient::CSnapState& { return gc.m_Snap; });
                // Clients
                ns.addProperty("m_aClients", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = &gc.m_aClients[i];
                        return t;
                });
                // Stats
                ns.addProperty("m_aStats", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = &gc.m_aStats[i];
                        return t;
                });
                // Game info
                ns.addProperty("m_GameInfo", [](CGameClient &gc) -> CGameInfo& { return gc.m_GameInfo; });
                // Flag drop ticks
                ns.addProperty("m_aFlagDropTick", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < 2; i++) t[i] = gc.m_aFlagDropTick[i];
                        return t;
                });
                // Tuning
                ns.addProperty("m_aTuning", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = &gc.m_aTuning[i];
                        return t;
                });
                ns.addProperty("m_aLocalTuneZone", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = gc.m_aLocalTuneZone[i];
                        return t;
                });
                // Input
                ns.addProperty("m_aDummyInput", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = &gc.m_aDummyInput[i];
                        return t;
                });
                ns.addProperty("m_aHammerInput", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = &gc.m_aHammerInput[i];
                        return t;
                });
                ns.addProperty("m_aDummyFire", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = gc.m_aDummyFire[i];
                        return t;
                });
                ns.addProperty("m_aLocalIds", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_DUMMIES; i++) t[i] = gc.m_aLocalIds[i];
                        return t;
                });
                // Teams
                ns.addProperty("m_Teams", [](CGameClient &gc) -> CTeamsCore& { return gc.m_Teams; });
                // GameWorlds
                ns.addProperty("m_GameWorld", [](CGameClient &gc) -> CGameWorld& { return gc.m_GameWorld; });
                ns.addProperty("m_PredictedWorld", [](CGameClient &gc) -> CGameWorld& { return gc.m_PredictedWorld; });
                ns.addProperty("m_PrevPredictedWorld", [](CGameClient &gc) -> CGameWorld& { return gc.m_PrevPredictedWorld; });
                // RenderTools / RenderMap
                ns.addProperty("m_RenderTools", [](CGameClient &gc) -> CRenderTools& { return gc.m_RenderTools; });
                ns.addProperty("m_RenderMap", [](CGameClient &gc) -> CRenderMap& { return gc.m_RenderMap; });
                // Language
                ns.addPropertyReadWrite("m_LanguageChanged", &CGameClient::m_LanguageChanged);
                // Components — ALL of them as references
                ns.addProperty("m_Camera", [](CGameClient &gc) -> CCamera& { return gc.m_Camera; });
                ns.addProperty("m_Chat", [](CGameClient &gc) -> CChat& { return gc.m_Chat; });
                ns.addProperty("m_Controls", [](CGameClient &gc) -> CControls& { return gc.m_Controls; });
                ns.addProperty("m_Effects", [](CGameClient &gc) -> CEffects& { return gc.m_Effects; });
                ns.addProperty("m_Flow", [](CGameClient &gc) -> CFlow& { return gc.m_Flow; });
                ns.addProperty("m_Voting", [](CGameClient &gc) -> CVoting& { return gc.m_Voting; });
                ns.addProperty("m_Spectator", [](CGameClient &gc) -> CSpectator& { return gc.m_Spectator; });
                ns.addProperty("m_Emoticon", [](CGameClient &gc) -> CEmoticon& { return gc.m_Emoticon; });
                ns.addProperty("m_Motd", [](CGameClient &gc) -> CMotd& { return gc.m_Motd; });
                ns.addProperty("m_Broadcast", [](CGameClient &gc) -> CBroadcast& { return gc.m_Broadcast; });
                ns.addProperty("m_Ghost", [](CGameClient &gc) -> CGhost& { return gc.m_Ghost; });
                ns.addProperty("m_BotControl", [](CGameClient &gc) -> CBotControl& { return gc.m_BotControl; });
                ns.addProperty("m_BotNet", [](CGameClient &gc) -> CBotNet& { return gc.m_BotNet; });
                ns.addProperty("m_IRC", [](CGameClient &gc) -> CIRC& { return gc.m_IRC; });
                ns.addProperty("m_Particles", [](CGameClient &gc) -> CParticles& { return gc.m_Particles; });
                ns.addProperty("m_Sounds", [](CGameClient &gc) -> CSounds& { return gc.m_Sounds; });
                ns.addProperty("m_DamageInd", [](CGameClient &gc) -> CDamageInd& { return gc.m_DamageInd; });
                ns.addProperty("m_Menus", [](CGameClient &gc) -> CMenus& { return gc.m_Menus; });
                ns.addProperty("m_Binds", [](CGameClient &gc) -> CBinds& { return gc.m_Binds; });
                ns.addProperty("m_GameConsole", [](CGameClient &gc) -> CGameConsole& { return gc.m_GameConsole; });
                ns.addProperty("m_Hud", [](CGameClient &gc) -> CHud& { return gc.m_Hud; });
                ns.addProperty("m_RaceDemo", [](CGameClient &gc) -> CRaceDemo& { return gc.m_RaceDemo; });
                ns.addProperty("m_LocalServer", [](CGameClient &gc) -> CLocalServer& { return gc.m_LocalServer; });
                ns.addProperty("m_ImportantAlert", [](CGameClient &gc) -> CImportantAlert& { return gc.m_ImportantAlert; });
                ns.addProperty("m_Scoreboard", [](CGameClient &gc) -> CScoreboard& { return gc.m_Scoreboard; });
                ns.addProperty("m_Statboard", [](CGameClient &gc) -> CStatboard& { return gc.m_Statboard; });
                // Methods
                ns.addFunction("SendKill", [](CGameClient &gc) { gc.SendKill(); });
                ns.addFunction("SendSwitchTeam", [](CGameClient &gc, int team) { gc.SendSwitchTeam(team); });
                ns.addFunction("Echo", [](CGameClient &gc, const std::string &msg) { gc.Echo(msg.c_str()); });
                ns.addFunction("IsTeamPlay", [](CGameClient &gc) -> bool { return gc.IsTeamPlay(); });
                ns.addFunction("IsWorldPaused", [](CGameClient &gc) -> bool { return gc.IsWorldPaused(); });
                ns.addFunction("IsLocalCharSuper", [](CGameClient &gc) -> bool { return gc.IsLocalCharSuper(); });
                ns.addFunction("CurrentRaceTime", [](CGameClient &gc) -> int { return gc.CurrentRaceTime(); });
                ns.addFunction("LastRaceTick", [](CGameClient &gc) -> int { return gc.LastRaceTick(); });
                ns.addFunction("AntiPingPlayers", [](CGameClient &gc) -> bool { return gc.AntiPingPlayers(); });
                ns.addFunction("AntiPingGrenade", [](CGameClient &gc) -> bool { return gc.AntiPingGrenade(); });
                ns.addFunction("AntiPingWeapons", [](CGameClient &gc) -> bool { return gc.AntiPingWeapons(); });
                ns.addFunction("AntiPingGunfire", [](CGameClient &gc) -> bool { return gc.AntiPingGunfire(); });
                ns.addFunction("IsOtherTeam", [](CGameClient &gc, int id) -> bool { return gc.IsOtherTeam(id); });
                ns.addFunction("SwitchStateTeam", [](CGameClient &gc) -> int { return gc.SwitchStateTeam(); });
                ns.addFunction("IntersectCharacter", [L](CGameClient &gc, vec2 hookPos, vec2 newPos, int ownId) -> luabridge::LuaRef {
                        vec2 newPos2, playerPos;
                        int id = gc.IntersectCharacter(hookPos, newPos, newPos2, ownId, &playerPos);
                        auto t = luabridge::newTable(L);
                        t["ClientId"] = id;
                        t["NewPos2"] = newPos2;
                        t["PlayerPosition"] = playerPos;
                        return t;
                });
                ns.addFunction("DummyResetInput", [](CGameClient &gc) { gc.DummyResetInput(); });
                ns.addFunction("OnReset", [](CGameClient &gc) { gc.OnReset(); });
                ns.addFunction("Version", [](CGameClient &gc) -> const char* { return gc.Version(); });
                ns.addFunction("DDNetVersion", [](CGameClient &gc) -> int { return gc.DDNetVersion(); });
                ns.addFunction("Collision", [](CGameClient &gc) -> CCollision* { return gc.Collision(); });
                // Live reference tables (see CWorldCore::m_vSwitchers).
                ns.addFunction("Switchers", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = gc.Switchers();
                        for(size_t i = 0; i < v.size(); i++) t[(int)i] = &v[i];
                        return t;
                });
                ns.addFunction("PredSwitchers", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        auto &v = gc.PredSwitchers();
                        for(size_t i = 0; i < v.size(); i++) t[(int)i] = &v[i];
                        return t;
                });
                ns.addFunction("GetTuning", [](CGameClient &gc, int i) -> const CTuningParams* { return gc.GetTuning(i); });
                ns.addFunction("GetDDTeamColor", [](CGameClient &gc, int team, float light) -> ColorRGBA { return gc.GetDDTeamColor(team, light); });
                ns.addFunction("Layers", [](CGameClient &gc) -> CLayers* { return gc.Layers(); });
                ns.addFunction("Config", [](CGameClient &gc) -> CConfig* { return gc.Config(); });
                ns.addFunction("GetItemName", [](CGameClient &gc, int Type) -> std::string { const char *p = gc.GetItemName(Type); return p ? std::string(p) : std::string(); });
                ns.addFunction("NetVersion", [](CGameClient &gc) -> std::string { return std::string(gc.NetVersion()); });
                ns.addFunction("DDNetVersionStr", [](CGameClient &gc) -> std::string { return std::string(gc.DDNetVersionStr()); });
                ns.addFunction("ClientVersion7", [](CGameClient &gc) -> int { return gc.ClientVersion7(); });
                ns.addFunction("IsDemoPlaybackPaused", [](CGameClient &gc) -> bool { return gc.IsDemoPlaybackPaused(); });
                ns.addFunction("GetAnimationPlaybackSpeed", [](CGameClient &gc) -> float { return gc.GetAnimationPlaybackSpeed(); });
                ns.addFunction("Predict", [](CGameClient &gc) -> bool { return gc.Predict(); });
                ns.addFunction("PredictDummy", [](CGameClient &gc) -> bool { return gc.PredictDummy(); });
                ns.addFunction("ComponentCount", [](CGameClient &gc) -> int { return (int)gc.ComponentCount(); });
                ns.addFunction("SendStartInfo7", [](CGameClient &gc, int Dummy) { gc.SendStartInfo7(Dummy); });
                ns.addFunction("SendSkinChange7", [](CGameClient &gc, int Dummy) { gc.SendSkinChange7(Dummy); });
                ns.addFunction("SendReadyChange7", [](CGameClient &gc) { gc.SendReadyChange7(); });
                ns.addFunction("GotWantedSkin7", [](CGameClient &gc, int Dummy) -> bool { return gc.GotWantedSkin7(Dummy); });
                ns.addFunction("LoadGameSkin", [](CGameClient &gc, const std::string &Path, std::optional<bool> AsDir) { gc.LoadGameSkin(Path.c_str(), AsDir.value_or(false)); });
                ns.addFunction("LoadEmoticonsSkin", [](CGameClient &gc, const std::string &Path, std::optional<bool> AsDir) { gc.LoadEmoticonsSkin(Path.c_str(), AsDir.value_or(false)); });
                ns.addFunction("LoadParticlesSkin", [](CGameClient &gc, const std::string &Path, std::optional<bool> AsDir) { gc.LoadParticlesSkin(Path.c_str(), AsDir.value_or(false)); });
                ns.addFunction("LoadHudSkin", [](CGameClient &gc, const std::string &Path, std::optional<bool> AsDir) { gc.LoadHudSkin(Path.c_str(), AsDir.value_or(false)); });
                ns.addFunction("LoadExtrasSkin", [](CGameClient &gc, const std::string &Path, std::optional<bool> AsDir) { gc.LoadExtrasSkin(Path.c_str(), AsDir.value_or(false)); });
                ns.addFunction("ResetMultiView", [](CGameClient &gc) { gc.ResetMultiView(); });
                ns.addFunction("FindFirstMultiViewId", [](CGameClient &gc) -> int { return gc.FindFirstMultiViewId(); });
                ns.addProperty("m_InfoMessages", [](CGameClient &gc) -> CInfoMessages& { return gc.m_InfoMessages; });
                ns.addProperty("m_Censor", [](CGameClient &gc) -> CCensor& { return gc.m_Censor; });
                ns.addProperty("m_CodeExec", [](CGameClient &gc) -> CCodeExec& { return gc.m_CodeExec; });
                ns.addProperty("m_ClickGui", [](CGameClient &gc) -> CClickGui& { return gc.m_ClickGui; });
                ns.addProperty("m_AimBot", [](CGameClient &gc) -> CAimBot& { return gc.m_AimBot; });
                ns.addProperty("m_AvoidFreeze", [](CGameClient &gc) -> CAvoidFreeze& { return gc.m_AvoidFreeze; });
                ns.addProperty("m_Esp", [](CGameClient &gc) -> CEsp& { return gc.m_Esp; });
                ns.addProperty("m_Trajectory", [](CGameClient &gc) -> CTrajectory& { return gc.m_Trajectory; });
                ns.addProperty("m_KinetixLines", [](CGameClient &gc) -> CKinetixLines& { return gc.m_KinetixLines; });
                ns.addProperty("m_KeyBinder", [](CGameClient &gc) -> CKeyBinder& { return gc.m_KeyBinder; });
                ns.addProperty("m_Skins", [](CGameClient &gc) -> CSkins& { return gc.m_Skins; });
                ns.addProperty("m_Skins7", [](CGameClient &gc) -> CSkins7& { return gc.m_Skins7; });
                ns.addProperty("m_CountryFlags", [](CGameClient &gc) -> CCountryFlags& { return gc.m_CountryFlags; });
                ns.addProperty("m_DebugHud", [](CGameClient &gc) -> CDebugHud& { return gc.m_DebugHud; });
                ns.addProperty("m_TouchControls", [](CGameClient &gc) -> CTouchControls& { return gc.m_TouchControls; });
                ns.addProperty("m_Players", [](CGameClient &gc) -> CPlayers& { return gc.m_Players; });
                ns.addProperty("m_NamePlates", [](CGameClient &gc) -> CNamePlates& { return gc.m_NamePlates; });
                ns.addProperty("m_FreezeBars", [](CGameClient &gc) -> CFreezeBars& { return gc.m_FreezeBars; });
                ns.addProperty("m_Items", [](CGameClient &gc) -> CItems& { return gc.m_Items; });
                ns.addProperty("m_MapImages", [](CGameClient &gc) -> CMapImages& { return gc.m_MapImages; });
                ns.addProperty("m_MapLayersBackground", [](CGameClient &gc) -> CMapLayers& { return gc.m_MapLayersBackground; });
                ns.addProperty("m_MapLayersForeground", [](CGameClient &gc) -> CMapLayers& { return gc.m_MapLayersForeground; });
                ns.addProperty("m_Background", [](CGameClient &gc) -> CBackground& { return gc.m_Background; });
                ns.addProperty("m_MenuBackground", [](CGameClient &gc) -> CMenuBackground& { return gc.m_MenuBackground; });
                ns.addProperty("m_MapSounds", [](CGameClient &gc) -> CMapSounds& { return gc.m_MapSounds; });
                ns.addProperty("m_Tooltips", [](CGameClient &gc) -> CTooltips& { return *gc.Ui()->Tooltips(); });
                ns.addProperty("m_aReceivedTuning", [L](CGameClient &gc) -> luabridge::LuaRef {
                        auto t = luabridge::newTable(L);
                        for(int i = 0; i < MAX_CLIENTS; i++) t[i] = &gc.m_aReceivedTuning[i];
                        return t;
                });
                ns.endClass();
        }

        // The root CGameClient instance. Registered AFTER the class so the
        // userdata picks up the metatable.
        try
        {
                luabridge::setGlobal(L, GameClient(), "gc");
        }
        catch(const std::exception &e)
        {
                char aBuf[512];
                str_format(aBuf, sizeof(aBuf), "setGlobal gc failed: %s", e.what());
                Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "code_exec", aBuf);
        }

        Stage("cl");
        // ===================== IClient namespace (cl) =====================
        {
                auto cl = luabridge::getGlobalNamespace(L).beginNamespace("cl");
                cl.addFunction("Connect", [this](const std::string &addr) { Client()->Connect(addr.c_str()); });
                cl.addFunction("Disconnect", [this]() { Client()->Disconnect(); });
                cl.addFunction("EnterGame", [this]() { Client()->EnterGame(0); });
                cl.addFunction("RconAuth", [this](const std::string &user, const std::string &pass) { Client()->RconAuth(user.c_str(), pass.c_str(), 0); });
                cl.addFunction("Rcon", [this](const std::string &cmd) { Client()->Rcon(cmd.c_str()); });
                cl.addFunction("RconAuthed", [this]() -> bool { return Client()->RconAuthed(); });
                cl.addFunction("GameTick", [this]() -> int { return Client()->GameTick(0); });
                cl.addFunction("PrevGameTick", [this]() -> int { return Client()->PrevGameTick(0); });
                cl.addFunction("GameTickSpeed", [this]() -> int { return Client()->GameTickSpeed(); });
                cl.addFunction("LocalTime", [this]() -> float { return Client()->LocalTime(); });
                cl.addFunction("RenderFrameTime", [this]() -> float { return Client()->RenderFrameTime(); });
                cl.addFunction("ConnectionProblems", [this]() -> bool { return Client()->ConnectionProblems(); });
                cl.addFunction("DummyConnect", [this](int d) { Client()->DummyConnect(d); });
                cl.addFunction("DummyDisconnect", [this](int d) { Client()->DummyDisconnect(d, "Lua"); });
                cl.addFunction("DummyConnected", [this](int d) -> bool { return Client()->DummyConnected(d); });
                cl.addFunction("DummyCount", [this]() -> int { return Client()->DummyCount(); });
                cl.addFunction("Quit", [this]() { Client()->Quit(); });
                cl.addFunction("Restart", [this]() { Client()->Restart(); });
                cl.addFunction("PlayerName", [this]() -> std::string { return std::string(Client()->PlayerName()); });
                cl.addFunction("State", [this]() -> int { return (int)Client()->State(); });
                cl.addFunction("MapDownloadName", [this]() -> std::string { return std::string(Client()->MapDownloadName()); });
                cl.addFunction("MapDownloadAmount", [this]() -> int { return Client()->MapDownloadAmount(); });
                cl.addFunction("MapDownloadTotalsize", [this]() -> int { return Client()->MapDownloadTotalsize(); });
                cl.addFunction("PredGameTick", [this]() -> int { return Client()->PredGameTick(0); });
                cl.addFunction("IntraGameTick", [this]() -> float { return Client()->IntraGameTick(0); });
                cl.addFunction("GameTickTime", [this]() -> float { return Client()->GameTickTime(0); });
                cl.addFunction("GlobalTime", [this]() -> float { return Client()->GlobalTime(); });
                cl.addFunction("DummyAllowed", [this]() -> bool { return Client()->DummyAllowed(); });
                cl.addFunction("AnyDummyConnected", [this]() -> bool { return Client()->AnyDummyConnected(); });
                cl.addFunction("LastDummy", [this]() -> int { return Client()->LastDummy(); });
                cl.addFunction("DummyName", [this]() -> std::string { return std::string(Client()->DummyName()); });
                cl.addFunction("ErrorString", [this]() -> std::string { return std::string(Client()->ErrorString()); });
                cl.addFunction("News", [this]() -> std::string { return std::string(Client()->News()); });
                cl.addFunction("Points", [this]() -> int { return Client()->Points(); });
                cl.addFunction("ConnectAddress", [this]() -> std::string { return std::string(Client()->ConnectAddressString()); });
                cl.addFunction("PredictionTime", [this]() -> int { return Client()->GetPredictionTime(); });
                cl.addFunction("AutoScreenshot", [this]() { Client()->AutoScreenshot_Start(); });
                cl.addFunction("Notify", [this](const std::string &Title, const std::string &Msg) { Client()->Notify(Title.c_str(), Msg.c_str()); });
                cl.addFunction("IsSixup", [this]() -> bool { return Client()->IsSixup(); });
                cl.endNamespace();
        }

        Stage("net");
        // ===================== Net (net) =====================
        // Async HTTP through the engine's libcurl thread. Non-blocking.
        //   net.get(url[, timeout]) / net.post(url, body[, timeout])
        //   net.post_json(url, json[, headers][, timeout])
        // -> job: job:done() job:status() job:result() job:code() job:abort()
        auto NetPrep = [](const std::shared_ptr<IHttpRequest> &pReq, int TimeoutSec) {
                pReq->WriteToMemory();
                pReq->LogProgress(HTTPLOG::FAILURE);
                pReq->FailOnErrorStatus(false);
                pReq->MaxResponseSize(16 * 1024 * 1024);
                pReq->Timeout(CTimeout{4000, TimeoutSec * 1000, 1, 30});
        };
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<IHttpRequest>("NetJob");
                ns.addFunction("done", [](IHttpRequest &R) -> bool { return R.Done(); });
                ns.addFunction("status", [](IHttpRequest &R) -> std::string {
                        const EHttpState St = R.State();
                        if(St == EHttpState::QUEUED)
                                return "queued";
                        if(St == EHttpState::RUNNING)
                                return "running";
                        if(St == EHttpState::DONE)
                                return "done";
                        if(St == EHttpState::ABORTED)
                                return "aborted";
                        return "error";
                });
                ns.addFunction("result", [](IHttpRequest &R) -> std::string {
                        if(R.State() != EHttpState::DONE)
                                return std::string();
                        unsigned char *pData = nullptr;
                        size_t Len = 0;
                        R.Result(&pData, &Len);
                        return pData ? std::string((const char *)pData, Len) : std::string();
                });
                ns.addFunction("code", [](IHttpRequest &R) -> int {
                        if(R.State() != EHttpState::DONE)
                                return 0;
                        return R.StatusCode();
                });
                ns.addFunction("abort", [](IHttpRequest &R) { R.Abort(); });
                ns.endClass();
        }
        {
                auto net = luabridge::getGlobalNamespace(L).beginNamespace("net");
                net.addFunction("get", [this, NetPrep](const std::string &Url, std::optional<int> TimeoutSec) -> std::shared_ptr<IHttpRequest> {
                        if(!g_Config.m_KxExecSettings || !g_Config.m_KxExecInternet)
                                throw std::runtime_error("net: internet access is disabled (enable 'Access To The Internet' in Execution Settings)");
                        std::shared_ptr<IHttpRequest> pReq = HttpGet(Url.c_str());
                        NetPrep(pReq, TimeoutSec.value_or(30));
                        Http()->Run(pReq);
                        return pReq;
                });
                net.addFunction("post", [this, NetPrep](const std::string &Url, const std::string &Body, std::optional<int> TimeoutSec) -> std::shared_ptr<IHttpRequest> {
                        if(!g_Config.m_KxExecSettings || !g_Config.m_KxExecInternet)
                                throw std::runtime_error("net: internet access is disabled (enable 'Access To The Internet' in Execution Settings)");
                        std::shared_ptr<IHttpRequest> pReq = HttpGet(Url.c_str());
                        NetPrep(pReq, TimeoutSec.value_or(30));
                        pReq->Post((const unsigned char *)Body.data(), Body.size());
                        Http()->Run(pReq);
                        return pReq;
                });
                net.addFunction("post_json", [this, NetPrep, L](const std::string &Url, const std::string &Json, luabridge::LuaRef Headers, std::optional<int> TimeoutSec) -> std::shared_ptr<IHttpRequest> {
                        if(!g_Config.m_KxExecSettings || !g_Config.m_KxExecInternet)
                                throw std::runtime_error("net: internet access is disabled (enable 'Access To The Internet' in Execution Settings)");
                        std::shared_ptr<IHttpRequest> pReq = HttpGet(Url.c_str());
                        NetPrep(pReq, TimeoutSec.value_or(30));
                        pReq->PostJson(Json.c_str());
                        if(Headers.isTable())
                        {
                                for(const auto &kv : luabridge::pairs(Headers))
                                {
                                        const std::string Name = kv.first.cast<std::string>().value();
                                        const std::string Value = kv.second.cast<std::string>().value();
                                        pReq->HeaderString(Name.c_str(), Value.c_str());
                                }
                        }
                        Http()->Run(pReq);
                        return pReq;
                });
                net.endNamespace();
        }

        Stage("io");
        // ============ Safe io.popen / os.execute replacement ============
        // CRT _popen/_system spawn cmd.exe with a new console window in a GUI
        // application and block the render thread. These replacements run the
        // command via CreateProcess with CREATE_NO_WINDOW; io.popen executes
        // on a background thread so scripts can poll it with :done().
        struct SPopenJob
        {
                std::mutex m_Mutex;
                std::condition_variable m_Cv;
                std::string m_Output;
                int m_ExitCode = -1;
                bool m_Done = false;
        };
        auto RunCommandNoWindow = [](const std::string &Cmd, std::string *pOut) -> int {
#if defined(CONF_FAMILY_WINDOWS)
                SECURITY_ATTRIBUTES sa;
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;
                sa.lpSecurityDescriptor = nullptr;
                HANDLE ReadPipe = nullptr;
                HANDLE WritePipe = nullptr;
                HANDLE NullIn = INVALID_HANDLE_VALUE;
                HANDLE Process = nullptr;
                HANDLE ThreadHandle = nullptr;
                int ExitCode = -1;
                do
                {
                        if(!CreatePipe(&ReadPipe, &WritePipe, &sa, 0))
                                break;
                        SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);
                        NullIn = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
                        STARTUPINFOA si;
                        mem_zero(&si, sizeof(si));
                        si.cb = sizeof(si);
                        si.dwFlags = STARTF_USESTDHANDLES;
                        si.hStdInput = NullIn;
                        si.hStdOutput = WritePipe;
                        si.hStdError = WritePipe;
                        PROCESS_INFORMATION pi;
                        mem_zero(&pi, sizeof(pi));
                        std::string MutableCmd = Cmd; // CreateProcessA may modify the buffer
                        if(!CreateProcessA(nullptr, MutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
                                break;
                        Process = pi.hProcess;
                        ThreadHandle = pi.hThread;
                        CloseHandle(WritePipe);
                        WritePipe = nullptr;
                        char aBuf[4096];
                        unsigned long NumRead = 0;
                        for(;;)
                        {
                                if(!ReadFile(ReadPipe, aBuf, sizeof(aBuf), &NumRead, nullptr) || NumRead == 0)
                                        break;
                                if(pOut)
                                        pOut->append(aBuf, (size_t)NumRead);
                        }
                        WaitForSingleObject(Process, INFINITE);
                        unsigned long Exit = (unsigned long)-1;
                        GetExitCodeProcess(Process, &Exit);
                        ExitCode = (int)Exit;
                } while(false);
                if(WritePipe)
                        CloseHandle(WritePipe);
                if(ReadPipe)
                        CloseHandle(ReadPipe);
                if(NullIn != INVALID_HANDLE_VALUE)
                        CloseHandle(NullIn);
                if(ThreadHandle)
                        CloseHandle(ThreadHandle);
                if(Process)
                        CloseHandle(Process);
                return ExitCode;
#else
                // POSIX (Android/Linux/macOS): no console window to hide, so a
                // plain popen is fine. io.popen still runs on a background thread.
                FILE *pPipe = popen(Cmd.c_str(), "r");
                if(!pPipe)
                        return -1;
                char aBuf[4096];
                while(size_t NumRead = fread(aBuf, 1, sizeof(aBuf), pPipe))
                {
                        if(pOut)
                                pOut->append(aBuf, NumRead);
                }
                const int Status = pclose(pPipe);
                if(Status == -1)
                        return -1;
                if(WIFEXITED(Status))
                        return WEXITSTATUS(Status);
                return -1;
#endif
        };

        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<SPopenJob>("SPopenJob");
                ns.addFunction("done", [](SPopenJob &J) -> bool {
                        std::lock_guard<std::mutex> Lock(J.m_Mutex);
                        return J.m_Done;
                });
                ns.addFunction("output", [](SPopenJob &J) -> std::string {
                        std::lock_guard<std::mutex> Lock(J.m_Mutex);
                        return J.m_Output;
                });
                ns.addFunction("exitcode", [](SPopenJob &J) -> int {
                        std::lock_guard<std::mutex> Lock(J.m_Mutex);
                        return J.m_ExitCode;
                });
                // Compatible with the original popen handle: blocks until done.
                ns.addFunction("read", [](SPopenJob &J, std::optional<std::string> Fmt) -> std::string {
                        (void)Fmt;
                        std::unique_lock<std::mutex> Lock(J.m_Mutex);
                        J.m_Cv.wait(Lock, [&J]() { return J.m_Done; });
                        return J.m_Output;
                });
                ns.addFunction("close", [](SPopenJob &J) -> int {
                        std::unique_lock<std::mutex> Lock(J.m_Mutex);
                        J.m_Cv.wait(Lock, [&J]() { return J.m_Done; });
                        return J.m_ExitCode;
                });
                ns.endClass();
        }

        {
                luabridge::LuaRef OsTable = luabridge::getGlobal(L, "os");
                OsTable["execute"] = luabridge::newFunction(L, [RunCommandNoWindow](const std::string &Cmd) -> std::tuple<bool, const char *, int> {
                        if(!g_Config.m_KxExecSettings || g_Config.m_KxExecSandbox)
                                throw std::runtime_error("os.execute is denied while Sandbox is enabled");
                        if(!g_Config.m_KxExecOs)
                                throw std::runtime_error("os.execute is disabled (enable 'OS Commands' in Execution Settings)");
                        std::string Output;
                        const int ExitCode = RunCommandNoWindow(ExecShellCommandLine(Cmd), &Output);
                        if(ExitCode < 0)
                                return {false, "exit", -1};
                        return {true, "exit", ExitCode};
                });
        }

        {
                luabridge::LuaRef IoTable = luabridge::getGlobal(L, "io");
                IoTable["popen"] = luabridge::newFunction(L, [L, RunCommandNoWindow](const std::string &Cmd) -> std::shared_ptr<SPopenJob> {
                        if(!g_Config.m_KxExecSettings || g_Config.m_KxExecSandbox)
                                throw std::runtime_error("io.popen is denied while Sandbox is enabled");
                        if(!g_Config.m_KxExecOs)
                                throw std::runtime_error("io.popen is disabled (enable 'OS Commands' in Execution Settings)");
                        auto Job = std::make_shared<SPopenJob>();
                        std::string ShellCmd = ExecShellCommandLine(Cmd);
                        std::thread([Job, RunCommandNoWindow, ShellCmd]() {
                                std::string Output;
                                const int ExitCode = RunCommandNoWindow(ShellCmd, &Output);
                                std::lock_guard<std::mutex> Lock(Job->m_Mutex);
                                Job->m_Output = std::move(Output);
                                Job->m_ExitCode = ExitCode;
                                Job->m_Done = true;
                                Job->m_Cv.notify_all();
                        }).detach();
                        return Job;
                });
        }

        // ===== Sandbox wrappers (kx_exec_sandbox) =====
        // When the sandbox is on, path-taking stdlib functions resolve their
        // arguments through CCodeExec::SandboxResolve: relative paths are
        // joined onto <userdir>/Kinetix/Sandbox, absolute paths must stay
        // inside it, anything else raises a Lua error.
        {
                auto WrapPathFn = [L, this](const char *pTable, const char *pFn, int NumPathArgs) {
                        if(pTable)
                        {
                                lua_getglobal(L, pTable);
                                if(!lua_istable(L, -1))
                                {
                                        lua_pop(L, 1);
                                        return;
                                }
                        }
                        lua_pushlightuserdata(L, (void *)this); // upvalue 1: this
                        lua_pushinteger(L, NumPathArgs);        // upvalue 2: how many leading args are paths
                        if(pTable)
                                lua_getfield(L, -3, pFn); // upvalue 3: original function
                        else
                                lua_getglobal(L, pFn);
                        lua_pushcclosure(L, [](lua_State *L2) -> int {
                                CCodeExec *pSelf = (CCodeExec *)lua_touserdata(L2, lua_upvalueindex(1));
                                const int NumPathArgs = (int)lua_tointeger(L2, lua_upvalueindex(2));
                                for(int i = 1; i <= NumPathArgs; i++)
                                {
                                        if(lua_isstring(L2, i))
                                        {
                                                char aRes[512];
                                                if(!pSelf->SandboxResolve(lua_tostring(L2, i), aRes, sizeof(aRes)))
                                                        return luaL_error(L2, "sandbox: access to '%s' is denied", lua_tostring(L2, i));
                                                lua_pushstring(L2, aRes);
                                                lua_replace(L2, i);
                                        }
                                }
                                lua_pushvalue(L2, lua_upvalueindex(3)); // original
                                lua_insert(L2, 1);
                                lua_call(L2, lua_gettop(L2) - 1, LUA_MULTRET);
                                return lua_gettop(L2);
                        }, 3);
                        if(pTable)
                        {
                                lua_setfield(L, -2, pFn);
                                lua_pop(L, 1);
                        }
                        else
                                lua_setglobal(L, pFn);
                };
                WrapPathFn("io", "open", 1);
                WrapPathFn("io", "lines", 1);
                WrapPathFn("io", "input", 1);
                WrapPathFn("io", "output", 1);
                WrapPathFn("os", "remove", 1);
                WrapPathFn("os", "rename", 2);
                WrapPathFn(nullptr, "dofile", 1);
                WrapPathFn(nullptr, "loadfile", 1);

                // os.tmpname: inside the sandbox, temp names live in the sandbox root.
                lua_getglobal(L, "os");
                if(lua_istable(L, -1))
                {
                        lua_pushlightuserdata(L, (void *)this); // upvalue 1: this
                        lua_getfield(L, -2, "tmpname");         // upvalue 2: original
                        lua_pushcclosure(L, [](lua_State *L2) -> int {
                                CCodeExec *pSelf = (CCodeExec *)lua_touserdata(L2, lua_upvalueindex(1));
                                if(!g_Config.m_KxExecSettings || !g_Config.m_KxExecSandbox)
                                {
                                        lua_pushvalue(L2, lua_upvalueindex(2));
                                        lua_call(L2, 0, 1);
                                        return 1;
                                }
                                static int TmpCounter = 0;
                                char aBuf[512];
                                str_format(aBuf, sizeof(aBuf), "%s/tmp_%d", pSelf->SandboxRoot(), ++TmpCounter);
                                lua_pushstring(L2, aBuf);
                                return 1;
                        }, 2);
                        lua_setfield(L, -2, "tmpname");
                }
                lua_pop(L, 1);

                // require: while the sandbox is on, search only inside it.
                lua_getglobal(L, "require");              // upvalue 1: original require
                lua_getglobal(L, "package");
                lua_getfield(L, -1, "path");              // upvalue 2: original package.path
                lua_getfield(L, -2, "cpath");             // upvalue 3: original package.cpath
                lua_remove(L, -3);                        // drop the package table
                lua_pushlightuserdata(L, (void *)this);   // upvalue 4: this
                lua_pushcclosure(L, [](lua_State *L2) -> int {
                        CCodeExec *pSelf = (CCodeExec *)lua_touserdata(L2, lua_upvalueindex(4));
                        if(!g_Config.m_KxExecSettings || !g_Config.m_KxExecSandbox)
                        {
                                lua_pushvalue(L2, lua_upvalueindex(1));
                                lua_insert(L2, 1);
                                lua_call(L2, lua_gettop(L2) - 1, 1);
                                return 1;
                        }
                        const char *pRoot = pSelf->SandboxRoot();
                        char aPath[1200];
                        str_format(aPath, sizeof(aPath), "%s/?.lua;%s/?/init.lua", pRoot, pRoot);
                        lua_getglobal(L2, "package");
                        lua_pushstring(L2, aPath);
                        lua_setfield(L2, -2, "path");
                        lua_pushstring(L2, "");
                        lua_setfield(L2, -2, "cpath");
                        lua_pop(L2, 1);
                        lua_pushvalue(L2, lua_upvalueindex(1));
                        lua_insert(L2, 1);
                        lua_call(L2, lua_gettop(L2) - 1, 1);
                        lua_getglobal(L2, "package");
                        lua_pushvalue(L2, lua_upvalueindex(2));
                        lua_setfield(L2, -2, "path");
                        lua_pushvalue(L2, lua_upvalueindex(3));
                        lua_setfield(L2, -2, "cpath");
                        lua_pop(L2, 1);
                        return 1;
                }, 4);
                lua_setglobal(L, "require");
        }

        Stage("config");
        // ===================== CConfig (config) =====================
        {
                auto ns = luabridge::getGlobalNamespace(L).beginClass<CConfig>("CConfig");
                ns.addPropertyReadWrite("m_ClDummy", &CConfig::m_ClDummy);
                ns.addPropertyReadWrite("m_ClDummyControl", &CConfig::m_ClDummyControl);
                ns.addPropertyReadWrite("m_ClDummyCopyMoves", &CConfig::m_ClDummyCopyMoves);
                ns.addPropertyReadWrite("m_ClDummyHook", &CConfig::m_ClDummyHook);
                ns.addPropertyReadWrite("m_ClDummyJump", &CConfig::m_ClDummyJump);
                ns.addPropertyReadWrite("m_ClDummyFire", &CConfig::m_ClDummyFire);
                ns.addPropertyReadWrite("m_ClDummyResetOnSwitch", &CConfig::m_ClDummyResetOnSwitch);
                ns.addPropertyReadWrite("m_ClDummyRestoreWeapon", &CConfig::m_ClDummyRestoreWeapon);
                ns.addPropertyReadWrite("m_KxHelpers", &CConfig::m_KxHelpers);
                ns.addPropertyReadWrite("m_KxCopyMoves", &CConfig::m_KxCopyMoves);
                ns.addPropertyReadWrite("m_KxFlyRide", &CConfig::m_KxFlyRide);
                ns.addPropertyReadWrite("m_KxDummyAim", &CConfig::m_KxDummyAim);
                ns.addPropertyReadWrite("m_KxDummyDirection", &CConfig::m_KxDummyDirection);
                ns.addPropertyReadWrite("m_KxDummyCopyMovesFilter", &CConfig::m_KxDummyCopyMovesFilter);
                // Dynamic access to ANY config variable by script name:
                // config.get("kx_helpers") -> value (nil if unknown)
                // config.set("kx_helpers", 1) -> true on success
                // Self-less functions (called with '.' like in the sol2 version):
                // statics resolve through the instance too.
                ns.addStaticFunction("get", [this, L](const std::string &Name) -> luabridge::LuaRef {
                        struct SFindCtx
                        {
                                const char *pName;
                                const SConfigVariable *pVariable;
                        };
                        SFindCtx Ctx{Name.c_str(), nullptr};
                        if(!ConfigManager())
                                return luabridge::LuaRef(L);
                        ConfigManager()->PossibleConfigVariables(Ctx.pName, -1, [](const SConfigVariable *pVariable, void *pUserData) {
                                SFindCtx *pCtx = (SFindCtx *)pUserData;
                                if(!pCtx->pVariable && str_comp(pVariable->m_pScriptName, pCtx->pName) == 0)
                                        pCtx->pVariable = pVariable;
                        }, &Ctx);
                        if(!Ctx.pVariable)
                                return luabridge::LuaRef(L);
                        if(Ctx.pVariable->m_Type == SConfigVariable::VAR_STRING)
                                return luabridge::LuaRef(L, std::string(static_cast<const SStringConfigVariable *>(Ctx.pVariable)->m_pStr));
                        if(Ctx.pVariable->m_Type == SConfigVariable::VAR_COLOR)
                                return luabridge::LuaRef(L, (int)*static_cast<const SColorConfigVariable *>(Ctx.pVariable)->m_pVariable);
                        return luabridge::LuaRef(L, *static_cast<const SIntConfigVariable *>(Ctx.pVariable)->m_pVariable);
                });
                ns.addStaticFunction("set", [this](const std::string &Name, luabridge::LuaRef Value) -> bool {
                        struct SFindCtx
                        {
                                const char *pName;
                                SConfigVariable *pVariable;
                        };
                        SFindCtx Ctx{Name.c_str(), nullptr};
                        if(!ConfigManager())
                                return false;
                        ConfigManager()->PossibleConfigVariables(Ctx.pName, -1, [](const SConfigVariable *pVariable, void *pUserData) {
                                SFindCtx *pCtx = (SFindCtx *)pUserData;
                                if(!pCtx->pVariable && str_comp(pVariable->m_pScriptName, pCtx->pName) == 0)
                                        pCtx->pVariable = const_cast<SConfigVariable *>(pVariable);
                        }, &Ctx);
                        if(!Ctx.pVariable)
                                return false;
                        if(Ctx.pVariable->m_Type == SConfigVariable::VAR_INT)
                                static_cast<SIntConfigVariable *>(Ctx.pVariable)->SetValue(Value.cast<int>().value());
                        else if(Ctx.pVariable->m_Type == SConfigVariable::VAR_COLOR)
                                static_cast<SColorConfigVariable *>(Ctx.pVariable)->SetValue((unsigned)Value.cast<int>().value());
                        else if(Ctx.pVariable->m_Type == SConfigVariable::VAR_STRING)
                                static_cast<SStringConfigVariable *>(Ctx.pVariable)->SetValue(Value.cast<std::string>().value().c_str());
                        else
                                return false;
                        return true;
                });
                ns.endClass();
        }

        // The live CConfig instance. Registered AFTER the class.
        try
        {
                luabridge::setGlobal(L, &g_Config, "config");
        }
        catch(const std::exception &e)
        {
                char aBuf[512];
                str_format(aBuf, sizeof(aBuf), "setGlobal config failed: %s", e.what());
                Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "code_exec", aBuf);
        }

        Stage("console");
        // ===================== console =====================
        {
                auto con = luabridge::getGlobalNamespace(L).beginNamespace("console");
                con.addFunction("execute", [this](const std::string &Line) {
                        Console()->ExecuteLine(Line.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
                });
                con.addFunction("print", [this](const std::string &Str) {
                        Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", Str.c_str());
                });
                con.addFunction("execute_file", [this](const std::string &File) -> bool {
                        return Console()->ExecuteFile(File.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
                });
                con.endNamespace();
        }

        Stage("gfx");
        // ===================== gfx =====================
        {
                auto gfx = luabridge::getGlobalNamespace(L).beginNamespace("gfx");
                gfx.addFunction("width", [this]() -> float { return (float)Graphics()->ScreenWidth(); });
                gfx.addFunction("height", [this]() -> float { return (float)Graphics()->ScreenHeight(); });
                gfx.addFunction("aspect", [this]() -> float { return Graphics()->ScreenAspect(); });
                gfx.addFunction("window_width", [this]() -> float { return (float)Graphics()->WindowWidth(); });
                gfx.addFunction("window_height", [this]() -> float { return (float)Graphics()->WindowHeight(); });
                gfx.addFunction("map_screen", [this](float X0, float Y0, float X1, float Y1) {
                        Graphics()->MapScreen(CScreenRect(vec2(X0, Y0), vec2(X1, Y1)));
                });
                gfx.addFunction("map_screen_to_world", [this, L](float CenterX, float CenterY, std::optional<float> Zoom) -> luabridge::LuaRef {
                        const CScreenRect ScreenRect = Graphics()->MapScreenToWorld(CenterX, CenterY, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), Zoom.value_or(1.0f));
                        Graphics()->MapScreen(ScreenRect);
                        auto t = luabridge::newTable(L);
                        t[1] = ScreenRect.m_TopLeft.x;
                        t[2] = ScreenRect.m_TopLeft.y;
                        t[3] = ScreenRect.m_BottomRight.x;
                        t[4] = ScreenRect.m_BottomRight.y;
                        return t;
                });
                gfx.addFunction("draw_rect", [this](float X, float Y, float W, float H, float R, float G, float B, float A, std::optional<int> Corners, std::optional<float> Rounding) {
                        Graphics()->DrawRect(X, Y, W, H, ColorRGBA(R, G, B, A), Corners.value_or(IGraphics::CORNER_NONE), Rounding.value_or(0.0f));
                });
                gfx.addFunction("draw_circle", [this](float CX, float CY, float R, std::optional<int> Segments) {
                        Graphics()->DrawCircle(CX, CY, R, Segments.value_or(32));
                });
                gfx.addFunction("draw_line", [this](float X0, float Y0, float X1, float Y1, float R, float G, float B, float A) {
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
                gfx.addFunction("set_color", [this](float R, float G, float B, float A) {
                        Graphics()->SetColor(R, G, B, A);
                });
                gfx.addFunction("blend_normal", [this]() { Graphics()->BlendNormal(); });
                gfx.addFunction("blend_additive", [this]() { Graphics()->BlendAdditive(); });
                gfx.addFunction("map_screen_to_interface", [this](float CX, float CY, std::optional<float> Zoom) {
                        Graphics()->MapScreenToInterface(CX, CY, Zoom.value_or(1.0f));
                });
                gfx.addFunction("texture_clear", [this]() { Graphics()->TextureClear(); });
                gfx.endNamespace();
        }

        Stage("sound");
        // ===================== sound =====================
        {
                auto snd = luabridge::getGlobalNamespace(L).beginNamespace("sound");
                snd.addProperty("CHN_GUI", []() -> int { return CSounds::CHN_GUI; });
                snd.addProperty("CHN_MUSIC", []() -> int { return CSounds::CHN_MUSIC; });
                snd.addProperty("CHN_WORLD", []() -> int { return CSounds::CHN_WORLD; });
                snd.addProperty("CHN_GLOBAL", []() -> int { return CSounds::CHN_GLOBAL; });
                snd.addFunction("play", [this](int SampleId, float Vol) {
                        Sound()->Play(CSounds::CHN_GLOBAL, SampleId, 0, Vol);
                });
                snd.addFunction("play_at", [this](int SampleId, float Vol, float X, float Y) {
                        Sound()->PlayAt(CSounds::CHN_WORLD, SampleId, 0, Vol, vec2(X, Y));
                });
                snd.addFunction("stop", [this](int SampleId) {
                        Sound()->Stop(SampleId);
                });
                snd.addFunction("stop_all", [this]() {
                        Sound()->StopAll();
                });
                snd.addFunction("enabled", [this]() -> bool { return Sound()->IsSoundEnabled(); });
                snd.addFunction("load", [this](const std::string &Name) -> int {
                        if(Name.size() >= 5 && Name.compare(Name.size() - 5, 5, ".opus") == 0)
                                return Sound()->LoadOpus(Name.c_str());
                        return Sound()->LoadWV(Name.c_str());
                });
                snd.addFunction("unload_sample", [this](int SampleId) { Sound()->UnloadSample(SampleId); });
                snd.addFunction("pause", [this](int SampleId) { Sound()->Pause(SampleId); });
                snd.addFunction("sample_time", [this](int SampleId) -> float { return Sound()->GetSampleCurrentTime(SampleId); });
                snd.addFunction("sample_total_time", [this](int SampleId) -> float { return Sound()->GetSampleTotalTime(SampleId); });
                snd.endNamespace();
        }

        Stage("input");
        // ===================== input =====================
        {
                auto inp = luabridge::getGlobalNamespace(L).beginNamespace("input");
                inp.addProperty("KEY_W", []() -> int { return KEY_W; });
                inp.addProperty("KEY_A", []() -> int { return KEY_A; });
                inp.addProperty("KEY_S", []() -> int { return KEY_S; });
                inp.addProperty("KEY_D", []() -> int { return KEY_D; });
                inp.addProperty("KEY_SPACE", []() -> int { return KEY_SPACE; });
                inp.addProperty("KEY_RETURN", []() -> int { return KEY_RETURN; });
                inp.addProperty("KEY_ESCAPE", []() -> int { return KEY_ESCAPE; });
                inp.addProperty("KEY_TAB", []() -> int { return KEY_TAB; });
                inp.addProperty("KEY_LSHIFT", []() -> int { return KEY_LSHIFT; });
                inp.addProperty("KEY_RSHIFT", []() -> int { return KEY_RSHIFT; });
                inp.addProperty("KEY_LCTRL", []() -> int { return KEY_LCTRL; });
                inp.addProperty("KEY_RCTRL", []() -> int { return KEY_RCTRL; });
                inp.addProperty("KEY_UP", []() -> int { return KEY_UP; });
                inp.addProperty("KEY_DOWN", []() -> int { return KEY_DOWN; });
                inp.addProperty("KEY_LEFT", []() -> int { return KEY_LEFT; });
                inp.addProperty("KEY_RIGHT", []() -> int { return KEY_RIGHT; });
                inp.addProperty("KEY_DELETE", []() -> int { return KEY_DELETE; });
                inp.addProperty("KEY_BACKSPACE", []() -> int { return KEY_BACKSPACE; });
                inp.addProperty("KEY_INSERT", []() -> int { return KEY_INSERT; });
                inp.addProperty("KEY_HOME", []() -> int { return KEY_HOME; });
                inp.addProperty("KEY_END", []() -> int { return KEY_END; });
                inp.addProperty("KEY_PAGEUP", []() -> int { return KEY_PAGEUP; });
                inp.addProperty("KEY_PAGEDOWN", []() -> int { return KEY_PAGEDOWN; });
                inp.addProperty("KEY_F1", []() -> int { return KEY_F1; });
                inp.addProperty("KEY_F2", []() -> int { return KEY_F2; });
                inp.addProperty("KEY_F3", []() -> int { return KEY_F3; });
                inp.addProperty("KEY_F4", []() -> int { return KEY_F4; });
                inp.addProperty("KEY_F5", []() -> int { return KEY_F5; });
                inp.addProperty("KEY_F6", []() -> int { return KEY_F6; });
                inp.addProperty("KEY_F7", []() -> int { return KEY_F7; });
                inp.addProperty("KEY_F8", []() -> int { return KEY_F8; });
                inp.addProperty("KEY_F9", []() -> int { return KEY_F9; });
                inp.addProperty("KEY_F10", []() -> int { return KEY_F10; });
                inp.addProperty("KEY_F11", []() -> int { return KEY_F11; });
                inp.addProperty("KEY_F12", []() -> int { return KEY_F12; });
                inp.addProperty("KEY_MOUSE_1", []() -> int { return KEY_MOUSE_1; });
                inp.addProperty("KEY_MOUSE_2", []() -> int { return KEY_MOUSE_2; });
                inp.addProperty("KEY_MOUSE_3", []() -> int { return KEY_MOUSE_3; });
                inp.addProperty("KEY_MOUSE_WHEEL_UP", []() -> int { return KEY_MOUSE_WHEEL_UP; });
                inp.addProperty("KEY_MOUSE_WHEEL_DOWN", []() -> int { return KEY_MOUSE_WHEEL_DOWN; });
                inp.addFunction("pressed", [this](int Key) -> bool { return Input()->KeyIsPressed(Key); });
                inp.addFunction("pressed_this_frame", [this](int Key) -> bool { return Input()->KeyPress(Key); });
                inp.addFunction("key_name", [this](int Key) -> std::string { return std::string(KeyName(Key)); });
                inp.addFunction("find_key", [this](const std::string &Name) -> int { return Input()->FindKeyByName(Name.c_str()); });
                inp.addFunction("shift_pressed", [this]() -> bool { return Input()->ShiftIsPressed(); });
                inp.addFunction("alt_pressed", [this]() -> bool { return Input()->AltIsPressed(); });
                inp.addFunction("modifier_pressed", [this]() -> bool { return Input()->ModifierIsPressed(); });
                inp.addFunction("mouse_pressed", [this](std::optional<int> Index) -> bool { return Input()->NativeMousePressed(Index.value_or(0)); });
                inp.addFunction("mouse_pos", [this]() -> vec2 { return Input()->NativeMousePos(); });
                inp.addFunction("clipboard_get", [this]() -> std::string { return Input()->GetClipboardText(); });
                inp.addFunction("clipboard_set", [this](const std::string &Text) { Input()->SetClipboardText(Text.c_str()); });
                inp.endNamespace();
        }

        Stage("text");
        // ===================== text =====================
        {
                auto text = luabridge::getGlobalNamespace(L).beginNamespace("text");
                text.addFunction("draw", [this](float X, float Y, float Size, const std::string &Str) {
                        TextRender()->Text(X, Y, Size, Str.c_str(), -1.0f);
                });
                text.addFunction("width", [this](float Size, const std::string &Str) -> float {
                        return TextRender()->TextWidth(Size, Str.c_str());
                });
                text.addFunction("color", [this](float R, float G, float B, float A) {
                        TextRender()->TextColor(R, G, B, A);
                });
                text.addFunction("outline_color", [this](float R, float G, float B, float A) {
                        TextRender()->TextOutlineColor(R, G, B, A);
                });
                text.addFunction("bounding_box", [this, L](float Size, const std::string &Str) -> luabridge::LuaRef {
                        STextBoundingBox Box = TextRender()->TextBoundingBox(Size, Str.c_str());
                        auto t = luabridge::newTable(L);
                        t["width"] = Box.m_W;
                        t["height"] = Box.m_H;
                        return t;
                });
                text.endNamespace();
        }
}
