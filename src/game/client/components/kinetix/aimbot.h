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
        // v1.56.292: 7 weapons for TriggerBot: hammer..ninja + Hook. The Hook
        // entry is not a held weapon — it is the Hook trigger channel (own
        // settings + Entity Mode). The per-weapon Trigger fire/hook dropdown is
        // gone: weapons 0..5 always fire, index 6 always hooks.
        static constexpr int NUM_WEAPONS_TRIGGER = 7;
        // v1.56.292: per-entity settings — entity modes 1..5 (Hookable,
        // Unhookable, Freeze, Death, Others) each own a settings block in both
        // bots; entity mode 0 (Tee / players) keeps the Hook weapon block.
        static constexpr int NUM_HOOK_ENTITIES = 5;

        struct SWeaponSettings
        {
                bool m_Enabled = false;
                int m_TriggerMode = 0; // 0=one_tick 1=hold 2=every_tick (TriggerBot only)
                float m_Fov = 90.0f;
                float m_Radius = 300.0f;
                bool m_UseRaycastShow = false; // show: render FOV cone with wall collision
                int m_Rules = 1; // 0=none 1=insert_line 2=insert_line+raycast
                bool m_UseAngle = false; // AB: aim at raycast hit point instead of target center
                bool m_Predict = true; // AB: predict target position based on projectile time-of-flight (iterative)
                // v1.56.248: AB Jitter — randomize the aim point within the player
                // hitbox (radius 28). 0 = off; 1-100 = % of the hitbox radius used for
                // the random offset (normal mode) / random distance along a random
                // RAY THAT HIT the player (raycast-angle mode).
                float m_Jitter = 0.0f;
		// v1.56.262: Silent became a separate toggle; Mode is 0=edge 1=trigger
		// 2=always 3=smooth (AimBot only, TriggerBot ignores it).
		int m_AimMode = 0;
		bool m_Silent = true; // aim changes don't move the visible crosshair
		float m_SmoothSpeed = 3.0f; // Smooth mode: max aim travel per tick, px
		bool m_SmoothCenterSnap = true; // Smooth mode: rotate around the player center
                int m_TeamFilter = 0; // 0=both 1=war 2=my
                int m_FriendFilter = 0; // 0=both 1=ignore 2=only
                int m_DummyFilter = 0; // 0=both 1=ignore 2=only
                int m_ClanmateFilter = 0; // 0=both 1=ignore 2=only
                int m_TargetsFilter = 0; // 0=both 1=only 2=ignore
                char m_aTargetsFilter[64] = "";
                int m_FreezeFilter = 0; // 0=both 1=freeze 2=no_freeze
                bool m_FngFreeze = false; // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze)
                bool m_AutoSwitchWeapon = false; // v1.56.276: Auto Switch Weapon — switch to this weapon when it has a target in its zone and the held one doesn't
                bool m_UseSilentChannelAB = false; // v1.56.279: TriggerBot — while the AimBot's silent aim is active, this weapon's zone follows the silent direction
                bool m_OffWhileFreeze = false; // this weapon's bot is paused while we are frozen (ninja counts as frozen when this weapon's FNG Freeze is on)
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

	// v1.56.262: Edge mode — restores the pre-snap crosshair position on the
	// tick AFTER the snap (1-tick visible aim). Silent Smooth glides its own
	// server-side offset independently of the visible crosshair.
	bool m_AimEdgeRestore = false;
	vec2 m_AimEdgeRestorePos = vec2(0, 0);
	vec2 m_SilentSmoothPos = vec2(0, 0);
	bool m_SilentSmoothInit = false;
	// v1.56.276: Smooth Predict — per-tick aim published by ApplyAim (Smooth
	// mode) for the render-side interpolation in controls.cpp OnRender.
	vec2 m_SmoothPredictPrev = vec2(0, 0);
	vec2 m_SmoothPredictCurr = vec2(0, 0);
	int m_SmoothPredictTick = -1;
	// (fake aim releases it only on a real shot/hook; other sources keep
	// old priority).

        SWeaponSettings m_AimBotWeapons[NUM_WEAPONS_AIM];
        SWeaponSettings m_TriggerBotWeapons[NUM_WEAPONS_TRIGGER];
        // v1.56.292: per-entity Hook settings — index 0 = Hookable ... 4 =
        // Others (entity modes 1..5). Entity mode 0 (Tee, players) keeps the
        // Hook weapon block above (m_AimBotWeapons[6] / m_TriggerBotWeapons[6]).
        SWeaponSettings m_AimBotHookEnt[NUM_HOOK_ENTITIES];
        SWeaponSettings m_TriggerBotHookEnt[NUM_HOOK_ENTITIES];

        int m_AimBotSelectedWeapon = 0;
        int m_TriggerBotSelectedWeapon = 0;
        // v1.56.292: selected Hook entity (0=Tee 1=Hookable 2=Unhookable
        // 3=Freeze 4=Death 5=Others) — a GUI selection like the Weapon
        // dropdown (not saved). v1.56.294: it only picks the per-entity block
        // that is being EDITED — all enabled Hook blocks are active channels.
        int m_AimBotSelectedEntity = 0;
        int m_TriggerBotSelectedEntity = 0;

        // v1.56.292: settings block of Hook entity mode Entity (0 -> the Hook
        // weapon block). Entity is clamped to 0..5.
        SWeaponSettings &AimBotHookSettings(int Entity);
        const SWeaponSettings &AimBotHookSettings(int Entity) const;
        SWeaponSettings &TriggerBotHookSettings(int Entity);
        const SWeaponSettings &TriggerBotHookSettings(int Entity) const;

        float GetWeaponRadius(int Weapon) const;
        // v1.56.297: real reach for hooking TILES — the last FULL hook step.
        // The engine walks the hook in hook_fire_speed steps from the launch
        // offset PhysicalSize()*1.5 and clamps the last step to hook_length,
        // setting HOOK_RETRACT_START *before* the tile check (gamecore.cpp), so
        // a tile is never grabbed on that clamped tip — only on a full step:
        //   42 + floor((380 - 42) / 80) * 80 = 362
        //   (42 = PhysicalSize()*1.5, 380 = hook_length, 80 = hook_fire_speed)
        // That is the red part of the hook-collision line; the yellow tip up to
        // 380 is players-only (they are grabbed on the clamped step as well).
        static constexpr float HOOK_TILE_REACH = 362.0f;

private:
        bool m_PrevFire = false;
        bool m_PrevHook = false;
        // v1.56.291: set by FindTarget — true when the last call resolved to a
        // map entity (block) target instead of a player. Callers must not index
        // m_aClients[...] with the target id in that case.
        bool m_FoundEntityTarget = false;
        // v1.56.294: per-channel TriggerBot state — 0 = Fire (held weapon
        // 0..5), 1..6 = the Hook channels (0 = Tee players, 1..5 = the
        // per-entity blocks; the Hook runs independently of the held weapon,
        // so every channel needs its own edge/latency state).
        struct STriggerChannelState
        {
                bool m_WasActive = false; // edge detection for one_tick mode
                int m_ReleaseTick = -1; // one_tick: tick on which to release trigger (-1 = none)
                int m_LatencyCounter = 0; // random latency countdown
        };
        STriggerChannelState m_TriggerBotFireState;
        STriggerChannelState m_TriggerBotHookStates[NUM_HOOK_ENTITIES + 1];

        void Update();
        void UpdateAimBot();
        void UpdateTriggerBot();
        // v1.56.292: one TriggerBot channel (fire or hook) — all per-weapon
        // state lives in State so the two channels don't interfere.
        void UpdateTriggerBotChannel(const SWeaponSettings &Settings, int Weapon, bool HookChannel, int LocalId, const vec2 &MyPos, vec2 AimDir, vec2 AimPos, int HeldWeapon, bool WeFrozen, int EntityMode, STriggerChannelState &State);
        // EntityMode: 0 = players (normal player targeting), 1..5 = the Hook
        // block category (same categories as Custom colors -> Entities).
        int FindTarget(const SWeaponSettings &Settings, int Weapon, int LocalId, const vec2 &MyPos, const vec2 &AimDir, const vec2 &AimPos, vec2 &outAimPoint, int EntityMode = 0);
        bool IsVisible(const vec2 &From, const vec2 &To);
        // v1.56.263: InsertCharacter — true when another player's hitbox
        // intersects the segment (hooks/projectiles/lasers stop on the first
        // player touched, so a target behind a player is unreachable).
        bool LineBlockedByCharacter(int LocalId, int TargetId, const vec2 &From, const vec2 &To) const;
        // v1.56.263: show-raycast endpoint — clips the ray on walls AND players
        // (used by FOV/radius "Use raycast (Show)" rendering). v1.56.292:
        // EntityMode != 0 additionally stops the ray at the first tile of the
        // selected Hook entity (also non-solid: freeze/death), so the shown
        // raycast matches the block being targeted.
        vec2 RayShowEndPoint(const vec2 &From, const vec2 &To, int EntityMode = 0) const;
        // v1.56.294: true when the CURRENT aim already sends the hook onto a
        // tile of an ENABLED Hook entity category (before any other hook
        // blocker) — the aim must not be moved then. On the block = the user
        // pointed it there; Priority only applies while the aim is elsewhere.
        bool HookAimAlreadyOnEntity(const vec2 &MyPos, const vec2 &AimDir) const;
        bool PassesFilters(int ClientId, int LocalId, const SWeaponSettings &Settings);
        void ApplyAim(int ClientId, const vec2 &MyPos, const SWeaponSettings &Settings, const vec2 &AimPoint);
        void FireWeapon();
        void DoHook();
        // v1.56.292: EntityMode != 0 makes the show-raycast stop on a tile of
        // that entity category (mirrors the targeting of that channel).
        void RenderFov(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha, int EntityMode = 0);
	// v1.56.291: block ("entity") targeting used when EntityMode != 0.
	// Scans map tiles within FOV/radius for the selected category and returns a
	// non-negative sentinel (0) with outAimPoint = the point where the hook ray
	// meets the tile (the raycast angle; Jitter applied when set), or -1.
	int FindEntityTarget(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, const vec2 &AimPos, vec2 &outAimPoint, int EntityMode);
	void RenderRadius(const SWeaponSettings &Settings, const vec2 &MyPos, const vec2 &AimDir, float Alpha, int EntityMode = 0);

	// v1.56.255: per-weapon settings persistence (cvars <-> structs, pull once
	// at startup / push every tick). See aimbot.cpp.
	void SyncSavedSettings();
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
