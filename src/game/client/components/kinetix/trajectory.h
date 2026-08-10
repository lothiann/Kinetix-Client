// (c) Kinetix. Trajectory prediction component — v1.56.178.
//
// Replaces the old single-toggle RenderTrajectory in players.cpp with a
// per-type prediction system. Five trajectory types (Tee/Pistol/Shotgun/
// Grenade/Laser) each have independent Show / Prediction Ticks / Alpha
// Gradient / Simulate Players / Show for other players / Show for current
// settings. The ClickGUI "Trajectory" expandable exposes a Type dropdown
// that selects which type's settings are being edited (same pattern as
// AimBot's per-weapon dropdown).
//
// v1.56.183: MASTER TOGGLE — g_Config.m_KxShowTrajectory is checked at the top
// of OnRender (like g_Config.m_KxAimBot in aimbot.cpp). When OFF, NOTHING
// renders. Per-type m_Show is independent (like AimBot per-weapon m_Enabled).
// The ClickGUI parent "Trajectory" toggle and the legacy bind
// "toggle kx_show_trajectory 1 0" both control this master toggle.
//
// Types:
//   0 = Tee      — predict tee movement N ticks forward (existing behaviour)
//   1 = Pistol   — spawn CProjectile(WEAPON_GUN) in cloned world, simulate N ticks
//   2 = Shotgun  — spawn CProjectile(WEAPON_SHOTGUN) x5 (vanilla) or CLaser (DDRace), simulate
//   3 = Grenade  — spawn CProjectile(WEAPON_GRENADE), simulate N ticks (real arc + bounces)
//   4 = Laser    — spawn CLaser(WEAPON_LASER), simulate N ticks (real bounces off walls)
//
// "Show for current" (Pistol/Shotgun/Grenade/Laser only) iterates the
// PREDICTED world's existing CProjectile / CLaser entities and draws
// their remaining trajectory. v1.56.183: ShowForOtherPlayers now works
// for show-for-current — tracks ALL players' in-flight projectiles/lasers.
// SimulatePlayers also applies to show-for-current (keeps other characters
// for hit detection when ON).
// v1.56.184: ShowForOtherPlayers ALSO enables the PREVIEW trajectory for
// other players. OnRender iterates all active players and calls
// RenderWeaponPredict(theirWeapon, theirClientId) when their weapon type's
// Show+ShowForOtherPlayers are both ON. Each player gets their own cloned
// world + aim direction (m_TargetX/Y from snapshots for non-local players).
//
// Settings are runtime-only (NOT in config) — same as AimBot per-weapon
// settings. The legacy m_KxTrajectoryTicks config var stays two-way synced
// with m_aTypes[0] (Tee) m_PredictionTicks for backward compat with binds.

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_TRAJECTORY_H
#define GAME_CLIENT_COMPONENTS_KINETIX_TRAJECTORY_H

#include <base/vmath.h>
#include <game/client/component.h>

#include <vector> // std::vector in DrawPolyline signature

class CTrajectory : public CComponent
{
public:
        // Trajectory type indices. Must match g_TrajTypeOpts in clickgui.cpp.
        static constexpr int TRAJ_TEE = 0;
        static constexpr int TRAJ_PISTOL = 1;
        static constexpr int TRAJ_SHOTGUN = 2;
        static constexpr int TRAJ_GRENADE = 3;
        static constexpr int TRAJ_LASER = 4;
        static constexpr int NUM_TRAJ_TYPES = 5;

        struct STypeSettings
        {
                bool m_Show = false;
                int m_PredictionTicks = 10;
                bool m_AlphaGradient = true; // ON = 1-t*0.7 (existing fade), OFF = uniform alpha
                bool m_SimulatePlayers = false; // ON = keep other players in sim, OFF = remove them
                bool m_ShowForOtherPlayers = false; // predict for other players too (Tee type)
                bool m_ShowForCurrent = false; // draw existing projectiles (Pistol/Shotgun/Grenade/Laser)
        };

        STypeSettings m_aTypes[NUM_TRAJ_TYPES];
        int m_SelectedType = 0;

        int Sizeof() const override { return sizeof(*this); }
        void OnRender() override;

private:
        // Per-type renderers. Each reads its STypeSettings and draws if m_Show.
        void RenderTee();
        // v1.56.179: unified prediction-based weapon trajectory renderer.
        // Spawns a real CProjectile/CLaser in a cloned CGameWorld and simulates
        // N ticks, collecting positions each tick. Gives the REAL trajectory
        // (wall collisions, bounces, explosions) — not a hand-rolled approximation.
        // WeaponType is WEAPON_GUN / WEAPON_SHOTGUN / WEAPON_GRENADE / WEAPON_LASER.
        // v1.56.184: ClientId selects which player to predict for. When == LocalClientId,
        // uses m_Controls.m_aMousePos for aim (live mouse). Otherwise uses the player's
        // prediction character m_TargetX/m_TargetY (from snapshots). OnRender calls this
        // once for the local player + once per other player whose weapon type has
        // Show+ShowForOtherPlayers enabled.
        void RenderWeaponPredict(int WeaponType, int ClientId);

        // "Show for current" helpers — iterate prediction-world entities.
        void RenderCurrentProjectiles(int WeaponType);
        void RenderCurrentLasers();

        // Shared line drawing helper — draws a polyline with optional alpha gradient.
        // vPoints must have >= 2 entries. BaseColor/Alpha/Size come from
        // KX_LINE_TRAJECTORY config (shared line rendering settings).
        void DrawPolyline(const std::vector<vec2> &vPoints, bool AlphaGradient);

        // Sync legacy m_KxShowTrajectory / m_KxTrajectoryTicks <-> m_aTypes[TRAJ_TEE].
        // Called at the top of OnRender so binds like "toggle kx_show_trajectory 1 0"
        // still work and reflect into the Tee type settings.
        void SyncLegacyConfig();
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_TRAJECTORY_H
