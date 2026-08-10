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
// Types:
//   0 = Tee      — predict tee movement N ticks forward (existing behaviour)
//   1 = Pistol   — raycast from player along aim dir to wall
//   2 = Shotgun  — 5 raycasts in a fan (shotgun spread)
//   3 = Grenade  — simulate projectile arc (gravity + bounces) for N ticks
//   4 = Laser    — raycast with bounces off walls until energy runs out
//
// "Show for current" (Pistol/Shotgun/Grenade/Laser only) iterates the
// PREDICTED world's existing CProjectile / CLaser entities and draws
// their remaining trajectory.
//
// Settings are runtime-only (NOT in config) — same as AimBot per-weapon
// settings. The legacy m_KxShowTrajectory / m_KxTrajectoryTicks config
// vars stay for backward-compat with existing bind commands and are
// synced to/from m_aTypes[0] (Tee) each frame.

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
        void RenderPistol();
        void RenderShotgun();
        void RenderGrenade();
        void RenderLaser();

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
