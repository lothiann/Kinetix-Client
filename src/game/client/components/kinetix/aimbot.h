#ifndef GAME_CLIENT_COMPONENTS_KINETIX_AIMBOT_AIMBOT_H
#define GAME_CLIENT_COMPONENTS_KINETIX_AIMBOT_AIMBOT_H

#include <base/vmath.h>
#include <engine/console.h>
#include <game/client/component.h>
#include <game/gamecore.h>

class CAimBot : public CComponent
{
public:
        CAimBot() = default;
        ~CAimBot() override = default;

        int Sizeof() const override { return sizeof(*this); }
        void OnConsoleInit() override;
        void OnReset() override;
        void OnUpdate() override;
        void OnRender() override;

        // 7 weapons for AimBot: hammer, pistol, shotgun, grenade, laser, ninja, hook
        static constexpr int NUM_WEAPONS_AIM = 7;
        // 6 weapons for TriggerBot: no hook (triggerbot does not aim, hook makes no sense)
        static constexpr int NUM_WEAPONS_TRIGGER = 6;

        struct SWeaponSettings
        {
                bool m_Enabled = false;
                int m_Trigger = 0; // 0=fire 1=hook (TriggerBot only)
                int m_TriggerMode = 0; // 0=one_tick 1=hold 2=every_tick (TriggerBot only)
                float m_Fov = 90.0f;
                float m_Radius = 300.0f;
                bool m_UseRaycastShow = false; // show: render FOV cone with wall collision
                int m_RaycastAnglesShow = 16; // show: number of raycast angles
                int m_Rules = 1; // 0=none 1=insert_line 2=insert_line+raycast
                bool m_UseRaycastAB = false; // AB: use raycast for target detection
                bool m_ShowRaycastLinesAB = false; // AB: debug render raycast lines
                int m_RaycastAnglesAB = 16; // AB: number of raycast angles
                bool m_UseAngle = false; // AB: aim at raycast hit point instead of target center
                bool m_ShowSuccess = false; // AB: show rays that hit player hitbox in success color
                unsigned m_SuccessColor = 0xffff0000; // AB: success color (0xAARRGGBB, default red)
                bool m_Predict = false; // AB: predict target position based on projectile time-of-flight (iterative)
                int m_AimMode = 0; // 0=silent 1=trigger 2=always (AimBot only)
                int m_TeamFilter = 0; // 0=both 1=war 2=my
                int m_FriendFilter = 0; // 0=both 1=ignore
                int m_DummyFilter = 0; // 0=both 1=ignore
                int m_FreezeFilter = 0; // 0=both 1=freeze 2=no_freeze
                int m_Priority = 0; // 0=nearest_fov 1=nearest_aim 2=nearest_to_me
                int m_RandomLatency = 0; // TriggerBot only (ticks)
        };

        // v1.56.171 BUG9: m_AimBotEnabled/m_TriggerBotEnabled removed — now cvars
        // (g_Config.m_KxAimBot, g_Config.m_KxTriggerBot).
        bool m_AimBotShowFov = false;
        bool m_AimBotShowRadius = false;
        bool m_AimBotUseRaycastShow = false;
        bool m_TriggerBotShowFov = false;
        bool m_TriggerBotShowRadius = false;
        bool m_TriggerBotUseRaycastShow = false;

        SWeaponSettings m_AimBotWeapons[NUM_WEAPONS_AIM];
        SWeaponSettings m_TriggerBotWeapons[NUM_WEAPONS_TRIGGER];

        int m_AimBotSelectedWeapon = 0;
        int m_TriggerBotSelectedWeapon = 0;

        float GetWeaponRadius(int Weapon) const;

private:
        int m_LatencyCounter = 0;
        bool m_PrevFire = false;
        bool m_PrevHook = false;
        bool m_TriggerBotWasActive = false; // edge detection for TriggerBot one_tick mode
        int m_TriggerBotReleaseTick = -1; // one_tick: tick on which to release trigger (-1 = none)

        // v1.56.92: Cached raycast rays for debug rendering (filled in FindTarget, drawn in OnRender)
        struct SRayHit { vec2 end; float dist; };
        std::vector<SRayHit> m_DebugRayHits;
        bool m_DebugShowRays = false;

        void Update();
        void UpdateAimBot();
        void UpdateTriggerBot();
        int FindTarget(const SWeaponSettings &Settings, int Weapon, int LocalId, const vec2 &MyPos, const vec2 &AimDir, const vec2 &AimPos, vec2 &outAimPoint);
        bool IsVisible(const vec2 &From, const vec2 &To);
        bool PassesFilters(int ClientId, int LocalId, const SWeaponSettings &Settings);
        void ApplyAim(int ClientId, const vec2 &MyPos, const SWeaponSettings &Settings, const vec2 &AimPoint);
        void FireWeapon();
        void DoHook();
        void RenderFov(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha);
        void RenderRadius(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha);
        // v1.56.196: Real Predict — uses full physics simulation (CopyWorld + Tick).
        // Pre-rolls MAX_PREDICT_TICKS ticks ahead ONCE per FindTarget call,
        // recording each client's position after each tick. Then walks the table
        // forward: at each tick t, computes projectile flight time (FLOAT, not
        // ceil'd) to the target's position at tick t. When flightTime <= t,
        // the projectile caught up between (t-1) and t — interpolate the exact
        // sub-tick moment via linear interp of flightTime and aim at
        // mix(pos[t-1], pos[t], u). Gravity, friction, wall collisions, and
        // hook physics all apply during the pre-roll.
        static constexpr int MAX_PREDICT_TICKS = 20;

        static void ConAimBot(IConsole::IResult *pResult, void *pUserData);
        static void ConTriggerBot(IConsole::IResult *pResult, void *pUserData);
};

#endif
