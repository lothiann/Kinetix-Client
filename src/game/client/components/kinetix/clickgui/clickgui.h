// (c) Kinetix. ClickGUI overlay component.
// v1.56.34 (UI-only): renders 3 column panels (Combat/Movement/Misc)
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

        // Animation timing. ANIM_DURATION matches the HTML transition:
        // `transition: left/top 0.55s cubic-bezier(0.16, 1, 0.3, 1)`.
        static constexpr float ANIM_DURATION = 0.55f;

        // Distance each panel slides in from offscreen (px in screen space).
        static constexpr float SLIDE_DISTANCE = 420.0f;

        // Panel geometry (matches clickgui.html .panel/.panel-header/.panel-body).
        static constexpr float PANEL_WIDTH = 200.0f;
        static constexpr float HEADER_HEIGHT = 26.0f;
        static constexpr float BODY_PADDING = 4.0f;
        static constexpr float ROW_GAP = 2.0f;

        // Per-panel fixed "home" position (top-left corner, screen-space px).
        // Matches the inline style="left/top" in the HTML.
        static vec2 PanelHomePos(int panelIdx);

        // Unit vector each panel slides IN from during the open animation.
        // Each panel uses a distinct direction so they fly in from different sides
        // (echoes the HTML's random offscreen scatter on TAB-hide).
        static vec2 PanelSlideDir(int panelIdx);

        // Easing approximating cubic-bezier(0.16, 1, 0.3, 1) (easeOutExpo).
        static float EaseOutExpo(float t);

        // Render one panel at the given screen position with the given alpha.
        void RenderPanel(int panelIdx, vec2 pos, float alpha);

        // Low-level draw helpers (all screen-space px).
        void DrawRoundedRect(float x, float y, float w, float h, float radius, int corners, ColorRGBA color);
        void DrawText(float x, float y, float size, const char *pText, ColorRGBA color, int align = 0);
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H
