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
	// Same filter logic as CAimBot::PassesFilters (team/friend/dummy/freeze).
	// Returns true if ClientId should be an ESP target.
	bool PassesFilters(int ClientId, int LocalId) const;

	// Draw (or enqueue, when Layer > 0) a single line p0→p1.
	void EmitLine(const vec2 &p0, const vec2 &p1) const;
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_ESP_H
