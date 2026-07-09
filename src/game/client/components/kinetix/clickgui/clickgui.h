// (c) Kinetix. ClickGUI overlay component.
// v1.56.37: UI-only (no UX) — renders 3 column panels (Combat/Movement/Misc)
// using Yoga for layout and IGraphics for rendering. Open/close animation
// driven by the `cl_clickgui` config var (typically bound to a key, e.g.
//   bind tab "toggle cl_clickgui 1 0"
// ). No UX yet (no drag, no toggling, no input handling) — purely visual.
//
// Layout pipeline:
//   data model (static, mirrors upload/clickgui.html `data` object)
//     -> Yoga tree built per panel each frame
//       -> YGNodeCalculateLayout
//         -> rectangles read back and drawn via IGraphics.

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H
#define GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H

#include <base/color.h>
#include <base/vmath.h>

#include <game/client/component.h>

class CClickGui : public CComponent
{
public:
	CClickGui() = default;
	~CClickGui() override = default;

	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;

private:
	// Animation state: 0.0 = fully closed, 1.0 = fully open.
	float m_AnimProgress = 0.0f;

	// Per-panel offscreen target (randomized each time ClickGUI closes).
	// When m_AnimProgress < 1, panels interpolate from home -> offscreen.
	// When m_AnimProgress > 0 (opening), panels interpolate from offscreen -> home.
	vec2 m_aOffscreenPos[3] = {vec2(0, 0), vec2(0, 0), vec2(0, 0)};
	bool m_aOffscreenValid[3] = {false, false, false};

	// Track cl_clickgui state to detect transitions (for picking fresh offscreen spots).
	bool m_WasOpen = false;

	// Animation timing. ANIM_DURATION matches the HTML transition:
	// `transition: left/top 0.55s cubic-bezier(0.16, 1, 0.3, 1)`.
	static constexpr float ANIM_DURATION = 0.55f;

	// Panel geometry — scaled up 1.25x from v1.56.34 for readability.
	static constexpr float PANEL_WIDTH = 250.0f;
	static constexpr float HEADER_HEIGHT = 32.0f;
	static constexpr float BODY_PADDING = 5.0f;
	static constexpr float ROW_GAP = 3.0f;

	// Per-panel fixed "home" position (top-left corner, screen-space px).
	// Matches the inline style="left/top" in the HTML, scaled up.
	static vec2 PanelHomePos(int panelIdx);

	// Cubic-bezier easing exactly matching CSS cubic-bezier(0.16, 1, 0.3, 1).
	// Uses Newton-Raphson to solve the bezier parametric equation for a given t.
	static float CubicBezierEase(float t);

	// Pick a random offscreen position for a panel (one of 4 sides: L/R/T/B),
	// ensuring the panel is fully outside the visible screen area plus margin.
	vec2 PickRandomOffscreenPos(int panelIdx, float screenW, float screenH) const;

	// Render one panel at the given screen position with the given alpha.
	void RenderPanel(int panelIdx, vec2 pos, float alpha);

	// Low-level draw helpers (all screen-space px).
	void DrawRoundedRect(float x, float y, float w, float h, float radius, int corners, ColorRGBA color);
	void DrawText(float x, float y, float size, const char *pText, ColorRGBA color, int align = 0);
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H
