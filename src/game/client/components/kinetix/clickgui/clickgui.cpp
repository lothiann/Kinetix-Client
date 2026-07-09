// (c) Kinetix. ClickGUI overlay component — see clickgui.h for overview.
//
// v1.56.34 (UI-only): renders 3 column panels with open/close animation.
// No UX yet (no drag/click/input handling). All visuals are static; row
// states reflect the hardcoded defaults from upload/clickgui.html.
//
// Layout: Yoga (flexbox) computes the body's row stack; we then draw each
// row via IGraphics (rounded rects, gradient fills) and ITextRender (text).

#include <game/client/components/kinetix/clickgui/clickgui.h>

#include <yoga/Yoga.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/color.h>
#include <base/str.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

namespace {
// Coerce a body height to int for ClipEnable without truncation surprises.
inline int bodyH_safe(float h)
{
        return (int)(h + 0.5f);
}
} // namespace

// ============================================================================
// Data model — mirrors the `data` object in upload/clickgui.html exactly.
// All values are static; v1.56.34 has no UX so they never change at runtime.
// ============================================================================

namespace {
enum class ERowType
{
        Toggle,
        ToggleDropdown,
        Slider,
        Input,
        Dropdown,
        Expandable,
};

struct SRow
{
        ERowType type;
        const char *pName;
        bool on; // toggle / toggle-dropdown / expandable
        bool expanded; // expandable only
        float min, max, step, value; // slider only
        const char *pSuffix; // slider only
        const char *pInputValue; // input only
        const char *pPlaceholder; // input only
        const char *const *pOptions; // dropdown / toggle-dropdown
        int optionCount; // dropdown / toggle-dropdown
        int valueIdx; // dropdown / toggle-dropdown
        const SRow *pChildren; // expandable only
        int childCount; // expandable only
};

// ---------- Combat panel (1) ----------
static const char *const g_AimbotOpts[] = {"Head", "Body", "Legs"};
static const char *const g_TracersOpts[] = {"Top", "Center", "Bottom"};
static const char *const g_AutoArmorOpts[] = {"Off", "Weak", "Strong"};
static const SRow g_EspChildren[] = {
        {ERowType::Toggle, "Box", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Outline", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Health Bar", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Name Tags", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Tracers", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const SRow g_CritChildren[] = {
        {ERowType::Toggle, "Air Criticals", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "No Cooldown", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Delay", false, false, 0.0f, 1.0f, 0.05f, 0.25f, "s", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const SRow g_CombatRows[] = {
        {ERowType::Toggle, "KillAura", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "AutoClicker", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::ToggleDropdown, "Aimbot", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AimbotOpts, 3, 0, nullptr, 0},
        {ERowType::Expandable, "ESP Settings", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_EspChildren, 5},
        {ERowType::ToggleDropdown, "Tracers", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TracersOpts, 3, 1, nullptr, 0},
        {ERowType::Toggle, "Reach", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Criticals", true, true, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CritChildren, 3},
        {ERowType::ToggleDropdown, "AutoArmor", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AutoArmorOpts, 3, 0, nullptr, 0},
};

// ---------- Movement panel (2) ----------
static const SRow g_MvTweaksChildren[] = {
        {ERowType::Toggle, "NoFall", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Jesus", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "AutoJump", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Sneak", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const SRow g_VelocityChildren[] = {
        {ERowType::Slider, "Horizontal", false, false, 0.0f, 100.0f, 1.0f, 80.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Vertical", false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const SRow g_MovementRows[] = {
        {ERowType::Toggle, "Fly", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Speed", false, false, 0.0f, 10.0f, 0.1f, 2.5f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Jump", false, false, 0.0f, 5.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Sprint", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Movement Tweaks", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_MvTweaksChildren, 4},
        {ERowType::Slider, "Step Height", false, false, 0.0f, 3.0f, 0.25f, 1.0f, "b", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Reach", false, false, 3.0f, 6.0f, 0.05f, 4.5f, "b", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Velocity", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_VelocityChildren, 2},
        {ERowType::Slider, "Timer", false, false, 0.1f, 2.0f, 0.05f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

// ---------- Misc panel (3) ----------
static const char *const g_LangOpts[] = {"EN", "RU", "DE", "JP"};
static const char *const g_ThemeOpts[] = {"Dark", "Light", "Purple", "Neon"};
static const char *const g_AutoSaveOpts[] = {"Off", "5s", "30s", "5m"};
static const SRow g_KeybindsChildren[] = {
        {ERowType::Input, "Open Menu", false, false, 0, 0, 0, 0, nullptr, "RShift", "key...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Toggle Fly", false, false, 0, 0, 0, 0, nullptr, "F", "key...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Toggle Speed", false, false, 0, 0, 0, 0, nullptr, "V", "key...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Toggle ESP", false, false, 0, 0, 0, 0, nullptr, "X", "key...", nullptr, 0, 0, nullptr, 0},
};
static const SRow g_MiscRows[] = {
        {ERowType::Input, "Tag", false, false, 0, 0, 0, 0, nullptr, "[Z]", "tag...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Say Text", false, false, 0, 0, 0, 0, nullptr, "gg", "auto-say", nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Language", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_LangOpts, 4, 1, nullptr, 0},
        {ERowType::Dropdown, "Theme", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ThemeOpts, 4, 2, nullptr, 0},
        {ERowType::Input, "Color (hex)", false, false, 0, 0, 0, 0, nullptr, "#8a2be2", "#rrggbb", nullptr, 0, 0, nullptr, 0},
        {ERowType::ToggleDropdown, "AutoSave", true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AutoSaveOpts, 4, 2, nullptr, 0},
        {ERowType::Expandable, "Keybinds", false, true, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_KeybindsChildren, 4},
        {ERowType::Input, "Webhook URL", false, false, 0, 0, 0, 0, nullptr, "", "https://...", nullptr, 0, 0, nullptr, 0},
};

struct SPanel
{
        const char *pTitle;
        const SRow *pRows;
        int rowCount;
};

static const SPanel g_Panels[3] = {
        {"Combat", g_CombatRows, sizeof(g_CombatRows) / sizeof(g_CombatRows[0])},
        {"Movement", g_MovementRows, sizeof(g_MovementRows) / sizeof(g_MovementRows[0])},
        {"Misc", g_MiscRows, sizeof(g_MiscRows) / sizeof(g_MiscRows[0])},
};

// ============================================================================
// Constants — colors and geometry from clickgui.html (box-shadow/blur dropped
// per the spec; everything else matches the HTML aesthetic).
// ============================================================================

// v1.56.38: FIXED hex color format. ColorRGBA(unsigned col, bool alpha=true)
// decodes as 0xAARRGGBB (alpha in high byte), NOT 0xRRGGBBAA. Previous versions
// had R and B swapped and alpha in wrong position — header looked blue/cyan
// instead of purple. Now all colors use correct 0xAARRGGBB format.
static constexpr unsigned COL_PANEL_BG = 0xff161622;
static constexpr unsigned COL_PANEL_BORDER = 0xff2a2a3e;
static constexpr unsigned COL_HEADER_BG = 0xff9d4edd; // vivid purple (A=ff,R=9d,G=4e,B=dd)
static constexpr unsigned COL_HEADER_TEXT = 0xffffffff;
static constexpr unsigned COL_ARROW = 0xffdddddd;
static constexpr unsigned COL_ROW_TEXT = 0xffd8d8e8;
static constexpr unsigned COL_ROW_TEXT_ON = 0xffffffff;
static constexpr unsigned COL_CHECK_ON = 0xff00ff00; // green check
static constexpr unsigned COL_EXPAND_ARROW = 0xff8a8aa8;
static constexpr unsigned COL_SUB_LIST_BG = 0x2e000000; // 18% alpha, black
static constexpr unsigned COL_SUB_LIST_BORDER = 0x4d9d4edd; // 30% alpha purple
static constexpr unsigned COL_SLIDER_TRACK = 0xff2a2a3e;
static constexpr unsigned COL_SLIDER_THUMB = 0xff9d4edd;
static constexpr unsigned COL_SLIDER_THUMB_BORDER = 0xffb8b8e8;
static constexpr unsigned COL_INPUT_BG = 0xff0d0d18;
static constexpr unsigned COL_INPUT_BORDER = 0xff2a2a3e;
static constexpr unsigned COL_VALUE_BG = 0x4d000000; // 30% alpha, black
static constexpr unsigned COL_PLACEHOLDER = 0xff8a8aa8;

// Row heights (px) — scaled up 1.25x in v1.56.37.
static constexpr float ROW_H_TOGGLE = 25.0f;
static constexpr float ROW_H_SLIDER = 40.0f;
static constexpr float ROW_H_INPUT = 28.0f;
static constexpr float ROW_H_EXPANDABLE = 25.0f;

// Row internal layout (px) — scaled up 1.25x.
static constexpr float CHECK_W = 15.0f;
static constexpr float ARROW_W = 15.0f;
static constexpr float DROPDOWN_W = 100.0f;
static constexpr float DROPDOWN_H = 20.0f;
static constexpr float SLIDER_TRACK_H = 5.0f;
static constexpr float SLIDER_THUMB_R = 6.5f;
static constexpr float SUB_LIST_INDENT = 18.0f;

// Offscreen margin: how far outside the visible screen panels fly when hidden.
static constexpr float OFFSCREEN_MARGIN = 100.0f;

static float RowHeight(ERowType t)
{
        switch(t)
        {
        case ERowType::Toggle:
        case ERowType::ToggleDropdown:
        case ERowType::Dropdown:
                return ROW_H_TOGGLE;
        case ERowType::Slider:
                return ROW_H_SLIDER;
        case ERowType::Input:
                return ROW_H_INPUT;
        case ERowType::Expandable:
                return ROW_H_EXPANDABLE;
        }
        return ROW_H_TOGGLE;
}

} // anonymous namespace

// ============================================================================
// CClickGui — static helpers
// ============================================================================

vec2 CClickGui::PanelHomePos(int panelIdx)
{
        // Matches the inline style="left/top" in clickgui.html, scaled up 1.25x.
        switch(panelIdx)
        {
        case 0: return vec2(50.0f, 75.0f);
        case 1: return vec2(325.0f, 75.0f);
        case 2: return vec2(600.0f, 75.0f);
        default: return vec2(50.0f, 75.0f);
        }
}

// Cubic-bezier easing matching CSS cubic-bezier(0.16, 1, 0.3, 1) exactly.
// P0=(0,0), P1=(0.16,1), P2=(0.3,1), P3=(1,1). Solved with Newton-Raphson.
// This is the EXACT curve used by the HTML transition.
float CClickGui::CubicBezierEase(float t)
{
        if(t <= 0.0f)
                return 0.0f;
        if(t >= 1.0f)
                return 1.0f;

        // Bezier control points (x coordinates of P1, P2; P0.x=0, P3.x=1)
        const float x1 = 0.16f, x2 = 0.3f;
        // y coordinates of P1, P2; P0.y=0, P3.y=1
        const float y1 = 1.0f, y2 = 1.0f;

        // Solve for parameter s such that bezierX(s) = t, using Newton-Raphson.
        // bezierX(s) = 3(1-s)^2 * s * x1 + 3(1-s) * s^2 * x2 + s^3
        // bezierY(s) = 3(1-s)^2 * s * y1 + 3(1-s) * s^2 * y2 + s^3
        float s = t; // initial guess
        for(int i = 0; i < 8; ++i)
        {
                const float oneMinusS = 1.0f - s;
                const float x = 3.0f * oneMinusS * oneMinusS * s * x1
                             + 3.0f * oneMinusS * s * s * x2
                             + s * s * s;
                // derivative of bezierX w.r.t. s
                const float dx = 3.0f * oneMinusS * oneMinusS * (x1 - 0.0f)
                               + 6.0f * oneMinusS * s * (x2 - x1)
                               + 3.0f * s * s * (1.0f - x2);
                if(dx < 1e-6f)
                        break;
                const float delta = (x - t) / dx;
                s -= delta;
                if(s < 0.0f)
                        s = 0.0f;
                else if(s > 1.0f)
                        s = 1.0f;
                if(std::abs(delta) < 1e-5f)
                        break;
        }

        // Evaluate bezierY at the solved s.
        const float oneMinusS = 1.0f - s;
        return 3.0f * oneMinusS * oneMinusS * s * y1
             + 3.0f * oneMinusS * s * s * y2
             + s * s * s;
}

vec2 CClickGui::PickRandomOffscreenPos(int panelIdx, float screenW, float screenH) const
{
        // Pick one of 4 sides; place panel fully outside the visible screen plus
        // OFFSCREEN_MARGIN. Matches the HTML's pickRandomOffscreenPos() behavior.
        const float pw = PANEL_WIDTH;
        const float ph = HEADER_HEIGHT + 300.0f; // approximate panel height for offscreen calc

        const int side = (int)(std::rand() % 4);
        float x, y;
        if(side == 0) // left
        {
                x = -pw - OFFSCREEN_MARGIN - (float)(std::rand() % 200);
                y = (float)(std::rand() % (int)std::max(screenH - ph, 100.0f));
        }
        else if(side == 1) // right
        {
                x = screenW + OFFSCREEN_MARGIN + (float)(std::rand() % 200);
                y = (float)(std::rand() % (int)std::max(screenH - ph, 100.0f));
        }
        else if(side == 2) // top
        {
                x = (float)(std::rand() % (int)std::max(screenW - pw, 100.0f));
                y = -ph - OFFSCREEN_MARGIN - (float)(std::rand() % 200);
        }
        else // bottom
        {
                x = (float)(std::rand() % (int)std::max(screenW - pw, 100.0f));
                y = screenH + OFFSCREEN_MARGIN + (float)(std::rand() % 200);
        }
        return vec2(x, y);
}

// ============================================================================
// CClickGui — draw helpers
// ============================================================================

void CClickGui::DrawRoundedRect(float x, float y, float w, float h, float radius, int corners, ColorRGBA color)
{
        // DrawRect handles Begin/End internally and supports rounded corners via
        // the CORNER_* bitmask (matches CSS border-radius per-corner semantics).
        Graphics()->DrawRect(x, y, w, h, color, corners, radius);
}

void CClickGui::DrawText(float x, float y, float size, const char *pText, ColorRGBA color, int align)
{
        // CTextCursor has no m_Align field; center/right alignment is done by
        // pre-computing the text width and offsetting x manually.
        float actualX = x;
        if(align & TEXTALIGN_CENTER)
        {
                const float w = TextRender()->TextWidth(size, pText);
                actualX = x - w * 0.5f;
        }
        else if(align & TEXTALIGN_RIGHT)
        {
                const float w = TextRender()->TextWidth(size, pText);
                actualX = x - w;
        }
        CTextCursor cursor;
        cursor.SetPosition(vec2(actualX, y));
        cursor.m_FontSize = size;
        TextRender()->TextColor(color);
        TextRender()->TextEx(&cursor, pText, -1);
}

// ============================================================================
// CClickGui — component lifecycle
// ============================================================================

void CClickGui::OnReset()
{
        m_AnimProgress = 0.0f;
        m_WasOpen = false;
        for(int i = 0; i < 3; ++i)
                m_aOffscreenValid[i] = false;
}

void CClickGui::OnRender()
{
        const bool isOpen = g_Config.m_ClClickGui != 0;

        // Detect transition 0->1 (opening): pick fresh random offscreen positions
        // for each panel (matches HTML's "generate fresh random offscreen targets
        // each time we hide" — we do it on open so panels fly IN from those spots).
        if(isOpen && !m_WasOpen)
        {
                const float sw = (float)Graphics()->ScreenWidth();
                const float sh = (float)Graphics()->ScreenHeight();
                for(int i = 0; i < 3; ++i)
                {
                        m_aOffscreenPos[i] = PickRandomOffscreenPos(i, sw, sh);
                        m_aOffscreenValid[i] = true;
                }
                // Show Windows cursor while ClickGUI is open.
                Input()->MouseModeAbsolute();
        }
        m_WasOpen = isOpen;

        // Drive animation toward the current cl_clickgui target (0 or 1).
        const float target = isOpen ? 1.0f : 0.0f;
        const float dt = Client()->RenderFrameTime();
        const float speed = (ANIM_DURATION > 0.0f) ? (1.0f / ANIM_DURATION) : 1.0f;
        if(m_AnimProgress < target)
        {
                m_AnimProgress += dt * speed;
                if(m_AnimProgress > target)
                        m_AnimProgress = target;
        }
        else if(m_AnimProgress > target)
        {
                m_AnimProgress -= dt * speed;
                if(m_AnimProgress < target)
                        m_AnimProgress = target;
        }

        // When fully closed, restore relative mouse mode (hide cursor).
        if(m_AnimProgress <= 0.0f && !isOpen)
        {
                Input()->MouseModeRelative();
                return;
        }

        // While open (or animating), force absolute mouse mode EVERY FRAME.
        // Like the F1/F2 console does — window focus events etc. can switch back
        // to relative mode, so we must re-assert every frame to keep cursor visible.
        if(isOpen)
                Input()->MouseModeAbsolute();

        const float eased = CubicBezierEase(m_AnimProgress);
        const float alpha = eased; // fade in/out

        // Set up a pixel-space projection (0,0) = top-left, (W,H) = bottom-right.
        const float screenW = (float)Graphics()->ScreenWidth();
        const float screenH = (float)Graphics()->ScreenHeight();
        Graphics()->MapScreen(0.0f, 0.0f, screenW, screenH);

        // CRITICAL: clear any bound texture so DrawRect draws solid colored quads,
        // not textured ones (which would tint our colors unpredictably). This was
        // the cause of v1.56.34's "blue instead of purple" — a sprite texture from
        // a previous component was still bound.
        Graphics()->TextureClear();

        // Render each panel at its animated position (lerp offscreen -> home).
        for(int i = 0; i < 3; ++i)
        {
                const vec2 home = PanelHomePos(i);
                vec2 offscreen;
                if(m_aOffscreenValid[i])
                        offscreen = m_aOffscreenPos[i];
                else
                {
                        offscreen = PickRandomOffscreenPos(i, screenW, screenH);
                        m_aOffscreenPos[i] = offscreen;
                        m_aOffscreenValid[i] = true;
                }
                // position = lerp(offscreen, home, eased)
                const vec2 pos = vec2(
                        offscreen.x + (home.x - offscreen.x) * eased,
                        offscreen.y + (home.y - offscreen.y) * eased);
                RenderPanel(i, pos, alpha);
        }

        // Reset text color to white so we don't leak our color into other components
        // (this was the cause of v1.56.34's "console text colored in clickgui color").
        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ============================================================================
// CClickGui — RenderPanel (Yoga layout + IGraphics draw)
// ============================================================================

void CClickGui::RenderPanel(int panelIdx, vec2 pos, float alpha)
{
        const SPanel &panel = g_Panels[panelIdx];

        // ---- Step 1: build a Yoga tree for the body and compute its height ----
        // Root body node: width = PANEL_WIDTH, padding = BODY_PADDING, flex-column,
        // gap = ROW_GAP. We add one YGNode child per row (including expandable's
        // children when expanded, indented via margin-left).
        YGNodeRef bodyRoot = YGNodeNew();
        YGNodeStyleSetWidth(bodyRoot, PANEL_WIDTH);
        YGNodeStyleSetPadding(bodyRoot, YGEdgeAll, BODY_PADDING);
        YGNodeStyleSetFlexDirection(bodyRoot, YGFlexDirectionColumn);
        YGNodeStyleSetGap(bodyRoot, YGGutterAll, ROW_GAP);
        YGNodeStyleSetOverflow(bodyRoot, YGOverflowHidden);

        // Allocate row nodes; one per rendered row (top-level + expanded children).
        // Use a simple fixed-size array since each panel has a bounded row count.
        constexpr int MAX_ROWS = 32;
        YGNodeRef rowNodes[MAX_ROWS];
        int rowCount = 0;

        auto addRowNode = [&](const SRow &row, bool isChild) {
                if(rowCount >= MAX_ROWS)
                        return;
                YGNodeRef n = YGNodeNew();
                YGNodeStyleSetHeight(n, RowHeight(row.type));
                if(isChild)
                        YGNodeStyleSetMargin(n, YGEdgeLeft, SUB_LIST_INDENT);
                YGNodeInsertChild(bodyRoot, n, (size_t)rowCount);
                rowNodes[rowCount++] = n;
        };

        for(int r = 0; r < panel.rowCount; ++r)
        {
                const SRow &row = panel.pRows[r];
                addRowNode(row, false);
                if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                                addRowNode(row.pChildren[c], true);
                }
        }

        // Calculate layout: width = PANEL_WIDTH, height = auto (undefined).
        YGNodeCalculateLayout(bodyRoot, PANEL_WIDTH, YGUndefined, YGDirectionLTR);
        const float bodyHeight = YGNodeLayoutGetHeight(bodyRoot);

        // ---- Step 2: panel geometry ----
        const float panelH = HEADER_HEIGHT + bodyHeight;
        const float px = pos.x;
        const float py = pos.y;

        Graphics()->BlendNormal();

        // Panel border (1px-ish outline via slightly larger dark rect behind).
        DrawRoundedRect(px - 1.0f, py - 1.0f, PANEL_WIDTH + 2.0f, panelH + 2.0f,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(COL_PANEL_BORDER, true).WithAlpha(alpha));

        // Panel background.
        DrawRoundedRect(px, py, PANEL_WIDTH, panelH,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(COL_PANEL_BG, true).WithAlpha(alpha));

        // ---- Step 3: header ----
        // Header is the top HEADER_HEIGHT px; rounded only on top corners so it
        // blends with the body below.
        DrawRoundedRect(px, py, PANEL_WIDTH, HEADER_HEIGHT,
                5.0f, IGraphics::CORNER_T,
                ColorRGBA(COL_HEADER_BG, true).WithAlpha(alpha));

        // ▼ collapse arrow (left side of header).
        DrawText(px + 10.0f, py + 9.0f, 15.0f, "\xe2\x96\xbc" /* ▼ */,
                ColorRGBA(COL_ARROW, true).WithAlpha(alpha));

        // Title centered.
        {
                const float titleSize = 15.0f;
                DrawText(px + PANEL_WIDTH * 0.5f, py + 9.0f, titleSize,
                        panel.pTitle, ColorRGBA(COL_HEADER_TEXT, true).WithAlpha(alpha),
                        TEXTALIGN_CENTER);
        }

        // ---- Step 4: body (clip to body rect so overflowing rows are hidden) ----
        const float bodyX = px;
        const float bodyY = py + HEADER_HEIGHT;
        const float bodyW = PANEL_WIDTH;

        // Subtle sub-list backdrop tint (the HTML has rgba(0,0,0,0.18) under
        // expandable children; we tint the whole body faintly for visual depth).
        DrawRoundedRect(bodyX, bodyY, bodyW, bodyHeight,
                0.0f, IGraphics::CORNER_NONE,
                ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(alpha * 0.5f));

        Graphics()->ClipEnable((int)bodyX, (int)bodyY, (int)bodyW, (int)bodyH_safe(bodyHeight));

        // ---- Step 5: draw each row using its Yoga-computed rectangle ----
        int rowIdx = 0;
        for(int r = 0; r < panel.rowCount; ++r)
        {
                const SRow &row = panel.pRows[r];
                const YGNodeRef node = rowNodes[rowIdx];
                const float rxLocal = YGNodeLayoutGetLeft(node);
                const float ryLocal = YGNodeLayoutGetTop(node);
                const float rw = YGNodeLayoutGetWidth(node);
                const float rh = YGNodeLayoutGetHeight(node);
                const float rx = bodyX + rxLocal;
                const float ry = bodyY + ryLocal;
                ++rowIdx;

                // Common text color: brighter when "on".
                const ColorRGBA rowTextCol = row.on
                        ? ColorRGBA(COL_ROW_TEXT_ON, true)
                        : ColorRGBA(COL_ROW_TEXT, true);
                const ColorRGBA textCol = rowTextCol.WithAlpha(alpha);

                switch(row.type)
                {
                case ERowType::Toggle:
                {
                        // [✓ or empty] [name]
                        if(row.on)
                                DrawText(rx + 1.0f, ry + 5.0f, 15.0f, "\xe2\x9c\x93" /* ✓ */,
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 15.0f, row.pName, textCol);
                        break;
                }
                case ERowType::ToggleDropdown:
                {
                        // [✓ or empty] [name] [select box]
                        if(row.on)
                                DrawText(rx + 1.0f, ry + 5.0f, 15.0f, "\xe2\x9c\x93",
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 15.0f, row.pName, textCol);

                        // Dropdown box on the right.
                        const float ddX = rx + rw - DROPDOWN_W - 2.0f;
                        const float ddY = ry + (rh - DROPDOWN_H) * 0.5f;
                        DrawRoundedRect(ddX, ddY, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        const char *pVal = (row.optionCount > 0 && row.valueIdx >= 0 && row.valueIdx < row.optionCount)
                                ? row.pOptions[row.valueIdx]
                                : "";
                        DrawText(ddX + 4.0f, ddY + 4.0f, 14.0f, pVal, textCol);
                        break;
                }
                case ERowType::Slider:
                {
                        // Top: [name]              [value]
                        // Bottom: [track with thumb at value%]
                        DrawText(rx + 2.0f, ry + 4.0f, 14.0f, row.pName, textCol);

                        // Value chip.
                        char valBuf[32];
                        if(row.pSuffix)
                                str_format(valBuf, sizeof(valBuf), "%.2f%s", row.value, row.pSuffix);
                        else
                                str_format(valBuf, sizeof(valBuf), "%.2f", row.value);
                        const float valW = TextRender()->TextWidth(14.0f, valBuf) + 10.0f;
                        const float valX = rx + rw - valW - 2.0f;
                        const float valY = ry + 1.0f;
                        DrawRoundedRect(valX, valY, valW, 18.0f, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(alpha));
                        DrawText(valX + 6.0f, valY + 3.0f, 14.0f, valBuf, textCol);

                        // Track.
                        const float trackX = rx + 2.0f;
                        const float trackY = ry + rh - SLIDER_TRACK_H - 4.0f;
                        const float trackW = rw - 4.0f;
                        DrawRoundedRect(trackX, trackY, trackW, SLIDER_TRACK_H, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_SLIDER_TRACK, true).WithAlpha(alpha));

                        // Thumb.
                        float pct = 0.0f;
                        if(row.max > row.min)
                                pct = (row.value - row.min) / (row.max - row.min);
                        pct = std::clamp(pct, 0.0f, 1.0f);
                        const float thumbX = trackX + trackW * pct;
                        const float thumbY = trackY + SLIDER_TRACK_H * 0.5f;
                        Graphics()->TextureClear();
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(ColorRGBA(COL_SLIDER_THUMB, true).WithAlpha(alpha));
                        Graphics()->DrawCircle(thumbX, thumbY, SLIDER_THUMB_R, 12);
                        Graphics()->QuadsEnd();
                        break;
                }
                case ERowType::Input:
                {
                        // [name (compact)] [text input box]
                        const float nameW = std::min(88.0f, TextRender()->TextWidth(14.0f, row.pName) + 4.0f);
                        DrawText(rx + 2.0f, ry + 5.0f, 14.0f, row.pName, textCol);
                        const float inX = rx + nameW + 2.0f;
                        const float inY = ry + 2.0f;
                        const float inW = rw - nameW - 4.0f;
                        const float inH = rh - 4.0f;
                        DrawRoundedRect(inX, inY, inW, inH, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        DrawRoundedRect(inX - 1.0f, inY - 1.0f, inW + 2.0f, inH + 2.0f, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
                        const char *pText = (row.pInputValue && row.pInputValue[0]) ? row.pInputValue : row.pPlaceholder;
                        if(pText)
                        {
                                const ColorRGBA inCol = (row.pInputValue && row.pInputValue[0])
                                        ? textCol
                                        : ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha);
                                DrawText(inX + 4.0f, inY + 4.0f, 14.0f, pText, inCol);
                        }
                        break;
                }
                case ERowType::Dropdown:
                {
                        // [name] [select box]
                        DrawText(rx + 2.0f, ry + 5.0f, 15.0f, row.pName, textCol);
                        const float ddX = rx + rw - DROPDOWN_W - 2.0f;
                        const float ddY = ry + (rh - DROPDOWN_H) * 0.5f;
                        DrawRoundedRect(ddX, ddY, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        const char *pVal = (row.optionCount > 0 && row.valueIdx >= 0 && row.valueIdx < row.optionCount)
                                ? row.pOptions[row.valueIdx]
                                : "";
                        DrawText(ddX + 4.0f, ddY + 4.0f, 14.0f, pVal, textCol);
                        break;
                }
                case ERowType::Expandable:
                {
                        // [✓ or empty] [name] [▶ or ▼]
                        if(row.on)
                                DrawText(rx + 1.0f, ry + 5.0f, 15.0f, "\xe2\x9c\x93",
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 15.0f, row.pName,
                                row.on ? ColorRGBA(COL_ROW_TEXT_ON, true).WithAlpha(alpha) : textCol);
                        // Arrow on the right (▶ collapsed, ▼ expanded).
                        const char *pArrow = row.expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6";
                        const ColorRGBA arrowCol = row.expanded
                                ? ColorRGBA(0xffb8b8e8, true).WithAlpha(alpha)
                                : ColorRGBA(COL_EXPAND_ARROW, true).WithAlpha(alpha);
                        DrawText(rx + rw - ARROW_W - 2.0f, ry + 5.0f, 12.0f, pArrow, arrowCol);
                        break;
                }
                }

                // Render expanded children (already added as rowNodes after the parent).
                if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                const SRow &child = row.pChildren[c];
                                const YGNodeRef cnode = rowNodes[rowIdx];
                                const float crx = bodyX + YGNodeLayoutGetLeft(cnode);
                                const float cry = bodyY + YGNodeLayoutGetTop(cnode);
                                const float crw = YGNodeLayoutGetWidth(cnode);
                                const float crh = YGNodeLayoutGetHeight(cnode);
                                ++rowIdx;

                                const ColorRGBA cText = child.on
                                        ? ColorRGBA(COL_ROW_TEXT_ON, true).WithAlpha(alpha)
                                        : ColorRGBA(0xffb0b0d0, true).WithAlpha(alpha);

                                // Faint sub-list background band under each child.
                                DrawRoundedRect(crx - 2.0f, cry - 1.0f, crw + 4.0f, crh + 1.0f,
                                        0.0f, IGraphics::CORNER_NONE,
                                        ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(alpha * 0.6f));

                                switch(child.type)
                                {
                                case ERowType::Toggle:
                                        if(child.on)
                                                DrawText(crx + 1.0f, cry + 5.0f, 14.0f, "\xe2\x9c\x93",
                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 14.0f, child.pName, cText);
                                        break;
                                case ERowType::Slider:
                                {
                                        DrawText(crx + 2.0f, cry + 5.0f, 14.0f, child.pName, cText);
                                        char vbuf[32];
                                        if(child.pSuffix)
                                                str_format(vbuf, sizeof(vbuf), "%.2f%s", child.value, child.pSuffix);
                                        else
                                                str_format(vbuf, sizeof(vbuf), "%.2f", child.value);
                                        const float vw = TextRender()->TextWidth(14.0f, vbuf) + 10.0f;
                                        const float vx = crx + crw - vw - 2.0f;
                                        DrawRoundedRect(vx, cry + 1.0f, vw, 18.0f, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(alpha));
                                        DrawText(vx + 6.0f, cry + 2.0f, 14.0f, vbuf, cText);
                                        const float trx = crx + 2.0f;
                                        const float trY = cry + crh - SLIDER_TRACK_H - 4.0f;
                                        const float trW = crw - 4.0f;
                                        DrawRoundedRect(trx, trY, trW, SLIDER_TRACK_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_SLIDER_TRACK, true).WithAlpha(alpha));
                                        float cpct = 0.0f;
                                        if(child.max > child.min)
                                                cpct = (child.value - child.min) / (child.max - child.min);
                                        cpct = std::clamp(cpct, 0.0f, 1.0f);
                                        const float thX = trx + trW * cpct;
                                        const float thY = trY + SLIDER_TRACK_H * 0.5f;
                                        Graphics()->TextureClear();
                                        Graphics()->QuadsBegin();
                                        Graphics()->SetColor(ColorRGBA(COL_SLIDER_THUMB, true).WithAlpha(alpha));
                                        Graphics()->DrawCircle(thX, thY, SLIDER_THUMB_R, 12);
                                        Graphics()->QuadsEnd();
                                        break;
                                }
                                case ERowType::Input:
                                {
                                        const float nw = std::min(88.0f, TextRender()->TextWidth(14.0f, child.pName) + 4.0f);
                                        DrawText(crx + 2.0f, cry + 5.0f, 14.0f, child.pName, cText);
                                        const float ix = crx + nw + 2.0f;
                                        const float iy = cry + 2.0f;
                                        const float iw = crw - nw - 4.0f;
                                        const float ih = crh - 4.0f;
                                        DrawRoundedRect(ix, iy, iw, ih, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                                        const char *pT = (child.pInputValue && child.pInputValue[0]) ? child.pInputValue : child.pPlaceholder;
                                        if(pT)
                                        {
                                                const ColorRGBA ic = (child.pInputValue && child.pInputValue[0])
                                                        ? cText
                                                        : ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha);
                                                DrawText(ix + 4.0f, iy + 3.0f, 14.0f, pT, ic);
                                        }
                                        break;
                                }
                                case ERowType::Dropdown:
                                case ERowType::ToggleDropdown:
                                {
                                        if(child.type == ERowType::ToggleDropdown && child.on)
                                                DrawText(crx + 1.0f, cry + 5.0f, 14.0f, "\xe2\x9c\x93",
                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 14.0f, child.pName, cText);
                                        const float ddx = crx + crw - DROPDOWN_W - 2.0f;
                                        const float ddy = cry + (crh - DROPDOWN_H) * 0.5f;
                                        DrawRoundedRect(ddx, ddy, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                                        const char *pV = (child.optionCount > 0 && child.valueIdx >= 0 && child.valueIdx < child.optionCount)
                                                ? child.pOptions[child.valueIdx]
                                                : "";
                                        DrawText(ddx + 4.0f, ddy + 3.0f, 14.0f, pV, cText);
                                        break;
                                }
                                case ERowType::Expandable:
                                        // Nested expandables not in the data model; render as a plain toggle.
                                        if(child.on)
                                                DrawText(crx + 1.0f, cry + 5.0f, 14.0f, "\xe2\x9c\x93",
                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 14.0f, child.pName, cText);
                                        break;
                                }
                        }
                }
        }

        Graphics()->ClipDisable();

        // Free the Yoga tree (root + children are freed by YGNodeFreeRecursive).
        YGNodeFreeRecursive(bodyRoot);
}
