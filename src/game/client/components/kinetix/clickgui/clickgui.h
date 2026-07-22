// (c) Kinetix. ClickGUI overlay component.
// v1.56.39: Full UX (drag, toggle, slider, expand/collapse) + 30% background
// dimming with smooth animation. Row states are reference-only (no real
// behavior wired to game systems) — will be replaced with real hooks later.
//
// Layout pipeline:
//   data model (mirrors upload/clickgui.html `data` object, mutable)
//     -> Yoga tree built per panel each frame
//       -> YGNodeCalculateLayout
//         -> rectangles read back and drawn via IGraphics.
//
// UX:
//   - Drag panel by header (left mouse). Z-order: dragged panel on top.
//   - Click ✓ / name → toggle on/off.
//   - Click ▶/▼ → expand/collapse children (expandable rows).
//   - Click ▼ in header → collapse column body.
//   - Drag slider thumb → change value.
//   - Dropdown / Input: visual only this iteration (rendered, not interactive).
//
// Animation:
//   - Open/close: panels fly in from random offscreen positions,
//     cubic-bezier(0.16, 1, 0.3, 1) easing, 0.55s.
//   - Background dimming: 0% → 30% black overlay, same easing curve.

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H
#define GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H

#include <base/color.h>
#include <base/vmath.h>

#include <yoga/Yoga.h>

#include <game/client/component.h>

class CClickGui : public CComponent
{
public:
        CClickGui() = default;
        ~CClickGui() override = default;

        int Sizeof() const override { return sizeof(*this); }

        void OnReset() override;
        void OnRender() override;
        bool OnInput(const IInput::CEvent &Event) override;
        bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;

        static constexpr int NUM_PANELS = 4;

private:
        // Animation state: 0.0 = fully closed, 1.0 = fully open.
        float m_AnimProgress = 0.0f;

        // v1.56.77: dynamic ClickGUI accent color (replaces hardcoded 0xff9d4edd).
        // Mutable so the user can change it via the HEX input in Settings.
        // m_AccentColorRGBA = packed 0xAARRGGBB. Default = vivid purple.
        unsigned m_AccentColorRGBA = 0xff9d4edd;
        // Rainbow mode: when enabled, m_AccentColorRGBA is overwritten each frame
        // with a hue-rotating color. m_RainbowHue = 0..360 degrees.
        bool m_RainbowEnabled = false;
        float m_RainbowHue = 0.0f;
        float m_RainbowSpeed = 1.0f; // hue degrees per second
        // v1.56.78: saved accent color before rainbow was enabled. Restored when
        // rainbow is disabled so the manual HEX color comes back.
        unsigned m_SavedAccentColorRGBA = 0xff9d4edd;

        // Per-panel offscreen target (randomized each time ClickGUI closes).
        vec2 m_aOffscreenPos[NUM_PANELS] = {};
        bool m_aOffscreenValid[NUM_PANELS] = {};

        // Track cl_clickgui state to detect transitions (for picking fresh offscreen spots).
        bool m_WasOpen = false;

        // Per-panel current position (draggable). Initialized to home on first open.
        vec2 m_aPanelPos[NUM_PANELS] = {};
        bool m_aPanelPosValid[NUM_PANELS] = {};

        // Per-panel collapsed state (body hidden, only header shown).
        bool m_aPanelCollapsed[NUM_PANELS] = {};

        // Z-order: which panel is on top (0..2, higher = on top). Dragged panel
        // gets bumped to top. We render in low->high order so high paints last.
        int m_aZOrder[NUM_PANELS] = {};

        // Drag state.
        bool m_Dragging = false;
        int m_DragPanelIdx = -1; // which panel is being dragged
        vec2 m_DragOffset = vec2(0, 0); // mouse - panel-topleft at drag start

        // Slider drag state.
        int m_DragSliderPanel = -1;
        int m_DragSliderRow = -1;
        int m_DragSliderChild = -1; // -1 = top-level row, >=0 = child index within expandable

        // Dropdown popup state: when open, a list of options is rendered below the
        // dropdown box. Only one dropdown can be open at a time.
        bool m_DropdownOpen = false;
        int m_DropdownPanel = -1;
        int m_DropdownRow = -1;
        int m_DropdownChild = -1;

        // Input field editing state: when active, keystrokes go into the buffer.
        bool m_InputEditing = false;
        int m_InputPanel = -1;
        int m_InputRow = -1;
        int m_InputChild = -1;
        // Edit buffer (static so it survives across frames; one field at a time).
        static constexpr int INPUT_BUF_SIZE = 256;
        char m_InputBuf[INPUT_BUF_SIZE] = {};
        int m_InputLen = 0;
        int m_InputCursor = 0; // cursor position (0..m_InputLen)
        int m_InputSelStart = -1; // selection start (-1 = no selection)
        int m_InputSelEnd = -1; // selection end

        // Smooth drag: target position lerps to mouse, panel position lerps to target.
        vec2 m_aPanelTarget[NUM_PANELS] = {};

        // Mouse position in screen-space px (updated each frame from NativeMousePos).
        vec2 m_MousePos = vec2(0, 0);

        // Animation timing. ANIM_DURATION matches the HTML transition:
        // `transition: left/top 0.55s cubic-bezier(0.16, 1, 0.3, 1)`.
        static constexpr float ANIM_DURATION = 0.55f;
        // v1.56.60: per-element animation duration (seconds).
        // EXPAND_ANIM_DURATION — expandable children slide + panel collapse slide.
        // Uses EaseBothDirections for finish-easing in both expand and collapse.
        static constexpr float EXPAND_ANIM_DURATION = 0.28f;

        // v1.56.60: per-panel collapse animation progress (0 = expanded, 1 = collapsed).
        // Lerps toward (m_aPanelCollapsed[i] ? 1.0 : 0.0) each frame. Used to slide
        // the panel body up/down with the same easing as expandable rows.
        float m_aPanelCollapseAnim[NUM_PANELS] = {};

        // v1.56.72: Yoga layout cache. Yoga tree is rebuilt + recalculated every
        // frame in the original code (4 panels × 60fps = 240 layouts/sec) even when
        // nothing changed. We cache the layout per panel and only rebuild when the
        // "row membership" changes — i.e. when an expandable's m_AnimExpand crosses
        // 0 (added/removed from layout). The cache stores: the YGNodeRef root, the
        // per-row YGNodeRef array, the row count, and bodyHeightFull. The cache is
        // invalidated by setting m_aLayoutCacheValid[i] = false whenever a row's
        // expanded/m_AnimExpand>0 state changes between frames.
        struct SLayoutCache
        {
                YGNodeRef pRoot = nullptr;
                YGNodeRef aRowNodes[256] = {};
                int rowCount = 0;
                float bodyHeightFull = 0.0f;
                // Signature: a compact hash of which rows + children are in the layout.
                // We rebuild only when this changes. Uses a simple running hash of
                // (row index, child included?) bits.
                unsigned signature = 0;
        };
        SLayoutCache m_aLayoutCache[NUM_PANELS];
        bool m_aLayoutCacheDirty[NUM_PANELS] = {true, true, true, true};

        // v1.56.60: advance all per-row / per-panel animations toward their targets.
        // Called once per frame from OnRender before any drawing.
        void UpdateAnimations(float dt);

        // Panel geometry — scaled up again 1.2x in v1.56.39 for readability.
        static constexpr float PANEL_WIDTH = 300.0f;
        static constexpr float HEADER_HEIGHT = 38.0f;
        static constexpr float BODY_PADDING = 6.0f;
        static constexpr float ROW_GAP = 4.0f;
        // Maximum visible body height. When content exceeds this, the body becomes
        // scrollable via mouse wheel. Fits ~10 toggle rows (30px each + gaps).
        static constexpr float MAX_BODY_HEIGHT = 600.0f;
        // Scroll step per wheel notch (px) — roughly one toggle row + gap.
        static constexpr float SCROLL_STEP = 34.0f;
        // Per-panel scroll offset (px, 0 = top). 'stored' preserves the user's
        // scroll across panel collapse/expand; 'working' is the clamped value
        // used for rendering (copied from stored each frame when scrollable).
        float m_aPanelScroll[NUM_PANELS] = {};
        float m_aPanelScrollStored[NUM_PANELS] = {};

        // Background dimming: black overlay at this alpha when fully open.
        static constexpr float BG_DIM_ALPHA = 0.30f;

        // Per-panel fixed "home" position (top-left corner, screen-space px).
        static vec2 PanelHomePos(int panelIdx);

        // Cubic-bezier easing exactly matching CSS cubic-bezier(0.16, 1, 0.3, 1).
        static float CubicBezierEase(float t);

        // v1.56.61: Direction-aware finish-easing. Applies CubicBezierEase (fast-start,
        // slow-end) regardless of whether the animation is opening or closing — so
        // BOTH directions feel like "finish-easing" (element lands/settles at the end
        // of its motion).
        //   rawProgress: current animation value (0..1)
        //   target: where it's heading (0.0 = closing/collapsing, 1.0 = opening/expanding)
        // When opening (target=1): returns CubicBezierEase(rawProgress) — fast start, slow end.
        // When closing (target=0): returns 1 - CubicBezierEase(1 - rawProgress) — same curve
        //   mirrored, so the element quickly starts vanishing then slowly finishes vanishing.
        static float EaseBothDirections(float rawProgress, float target);

        // Pick a random offscreen position for a panel (one of 4 sides: L/R/T/B).
        vec2 PickRandomOffscreenPos(int panelIdx, float screenW, float screenH) const;

        // Render one panel at the given screen position with the given alpha.
        void RenderPanel(int panelIdx, vec2 pos, float alpha);

        // Hit-test helpers (screen-space px).
        bool PointInRect(vec2 p, float x, float y, float w, float h) const;

        // Bring a panel to the top of the z-order.
        void BringToFront(int panelIdx);

        // UX event handling (called from OnInput / OnCursorMove).
        void HandleMouseDown(vec2 mousePos);
        void HandleMouseUp(vec2 mousePos);

        // Find which row a click hit. Returns row/child indices via out-params.
        // panelIdx is an input (which panel to test). Returns true if a row was hit.
        bool HitTestRow(vec2 mousePos, int panelIdx, int &outRow, int &outChild, float &outRowX, float &outRowY, float &outRowW, float &outRowH);

        // Dropdown popup rendering + hit testing.
        void RenderDropdownPopup(float alpha);
        bool HandleDropdownClick(vec2 mousePos);

        // Input field text editing. Returns pointer to the row being edited
        // (defined in .cpp where SRow is visible), or nullptr. We use void* to
        // avoid forward-declaring the anonymous-namespace SRow in the header.
        void *GetEditingInputRowPtr();
        void HandleTextInput(const IInput::CEvent &Event);

        // Low-level draw helpers (all screen-space px).
        void DrawRoundedRect(float x, float y, float w, float h, float radius, int corners, ColorRGBA color);
        void DrawText(float x, float y, float size, const char *pText, ColorRGBA color, int align = 0);
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_CLICKGUI_CLICKGUI_H
