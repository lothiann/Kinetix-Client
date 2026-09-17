// (c) Kinetix. ESP component — draws lines from point A to all players matching
// the configured filters. See README.md for the folder-structure convention.
//
// Point A (the origin of all ESP lines) depends on Mode:
//   Active player     → interpolated render position of the active dummy.
//   Screen coordinates→ a fixed screen pixel, converted to world space. Useful
//                       during spectate or when the camera isn't on the dummy.
//
// Target players are filtered by the same 4 filters as AimBot (team/friend/
// dummy/freeze). The active dummy itself is never a target (no self-line).
//
// Lines use the shared Line rendering settings (color/opacity/size/layer).
// When Layer > 0, segments are deferred to CKinetixLines (drawn on top of
// foreground blocks); otherwise drawn here (behind fg blocks).

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_ESP_H
#define GAME_CLIENT_COMPONENTS_KINETIX_ESP_H

#include <base/color.h>
#include <base/vmath.h>
#include <game/client/component.h>

class CEsp : public CComponent
{
public:
	CEsp() = default;
	~CEsp() override = default;

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnRender() override;

private:
	// Same filter logic as CAimBot::PassesFilters (team/friend/dummy/clanmate/
	// targets/freeze). Returns true if ClientId should be an ESP target.
	bool PassesFilters(int ClientId, int LocalId) const;

	// v1.56.257: Hitboxes reuse the same filter set with their own cvar values.
	bool PassesHitboxesFilters(int ClientId, int LocalId) const;

	// v1.56.257: shared filter body — ESP and Hitboxes pass their own config
	// filter values so both use the exact same logic.
	bool PassesFiltersImpl(int ClientId, int LocalId, int teamFilter, int ddraceTeamFilter, int ddraceTeamId, int friendFilter, int dummyFilter, int clanmateFilter, int targetsFilterMode, const char *pTargetsIds, int freezeFilter) const;

	// v1.56.113: Style-aware line emission (Line/Arrow/Dotted/DottedArrow).
	void EmitLine(const vec2 &p0, const vec2 &p1, int playerIndex) const;

	// Draw a single solid segment (immediate mode, not deferred).
	void DrawSegment(const vec2 &p0, const vec2 &p1, ColorRGBA col, float halfWidth) const;

	// Draw arrowhead at p1 (2 short lines forming a V pointing toward p1).
	void DrawArrowhead(const vec2 &p0, const vec2 &p1, ColorRGBA col, float halfWidth) const;

	// v1.56.254: square box outline around a target (Box Style:
	// 0=Box solid 1=Dotted box, dashes animated with Box Speed).
	void EmitBox(const vec2 &center, int playerIndex) const;

	// v1.56.256: Hitboxes component — 28x28 tile-collision square and the round
	// trigger hitbox (radius 28, lasers/shotgun/hook) on the active dummy; with
	// Show for others, repeated for other players (optional ESP-like filters).
	// Style: 0=Filled (always immediate) 1=Box 2=Dotted Box (animated outline).
	void EmitHitboxes(int LocalId) const;
	void EmitRectShape(const vec2 &center, float halfW, float halfH, int style, int speed, int playerIndex) const;
	void EmitCircleShape(const vec2 &center, float radius, int style, int speed, int playerIndex) const;
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_ESP_H
