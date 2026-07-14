// (c) Kinetix. ClickGUI overlay component — see clickgui.h for overview.
//
// v1.56.34 (UI-only): renders 3 column panels with open/close animation.
// No UX yet (no drag/click/input handling). All visuals are static; row
// states reflect the hardcoded defaults from upload/clickgui.html.
//
// Layout: Yoga (flexbox) computes the body's row stack; we then draw each
// row via IGraphics (rounded rects, gradient fills) and ITextRender (text).

#include <game/client/components/kinetix/clickgui/clickgui.h>
#include <game/client/components/kinetix/kinetix.h>

#include <yoga/Yoga.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/color.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/components/console.h>
#include <game/client/gameclient.h>

namespace {
// Total number of ClickGUI panels. Must match CClickGui::NUM_PANELS in .h.
constexpr int NUM_PANELS = 4;

// Coerce a body height to int for ClipEnable without truncation surprises.
inline int bodyH_safe(float h)
{
        return (int)(h + 0.5f);
}
} // namespace

// ============================================================================
// Data model — mirrors the `data` object in upload/clickgui.html exactly.
// v1.56.39: mutable (UX can toggle/expand/drag). All row state is reference-
// only — clicking a toggle doesn't do anything real yet, it just flips the
// bool. Real hooks will be wired in a future iteration.
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
        Button,
};

struct SRow
{
        ERowType type;
        const char *pName;
        bool on; // toggle / toggle-dropdown / expandable
        bool expanded; // expandable only
        float min, max, step, value; // slider only
        const char *pSuffix; // slider only
        const char *pInputValue; // input only (default value, const)
        const char *pPlaceholder; // input only
        const char *const *pOptions; // dropdown / toggle-dropdown
        int optionCount; // dropdown / toggle-dropdown
        int valueIdx; // dropdown / toggle-dropdown
        SRow *pChildren; // expandable only (non-const for child mutation)
        int childCount; // expandable only
        // v1.56.43: mutable input buffer (so edited text persists).
        // Lazy-initialized from pInputValue on first render.
        char m_aInputBuf[256] = {};
        bool m_InputInitialized = false;
        // v1.56.60: animation state (mutable, not serialised).
        // m_AnimExpand: 0..1 progress of the expand/collapse slide animation.
        //               lerps toward (row.expanded ? 1.0 : 0.0) each frame.
        //               0 = children fully hidden (collapsed), 1 = fully shown.
        float m_AnimExpand = 0.0f;
};

// ---------- Functions panel (4) ----------
// "Custom latency" expandable: 1 slider child.
static SRow g_CustomLatencyChildren[] = {
        {ERowType::Slider, "Latency (ms)", false, false, 50.0f, 2000.0f, 10.0f, 500.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Copy Moves Filter" expandable: 5 sub-toggles.
static SRow g_CopyMovesFilterChildren[] = {
        {ERowType::Toggle, "Filter: Jump", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Direction", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Hook", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Aim", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Fire", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// Aim mode dropdown options.
static const char *const g_AimModeOpts[] = {"Smart", "Mirror"};
static SRow g_FunctionsRows[] = {
        {ERowType::Toggle, "Block", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Random Aim", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // Fly sub-section
        {ERowType::Toggle, "Preserve input", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Custom latency", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CustomLatencyChildren, 1},
        {ERowType::Toggle, "Auto fly", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // Copy Moves sub-section
        {ERowType::Expandable, "Copy Moves Filter", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CopyMovesFilterChildren, 5},
        {ERowType::Toggle, "Mirror direction", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Aim mode", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AimModeOpts, 2, 0, nullptr, 0},
        // Conditional aim options (visibility based on Aim mode)
        {ERowType::Toggle, "Mirror aim X", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Mirror aim Y", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Smart aim", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

// ---------- Advanced panel (5) ----------
// "Laser unfreeze" expandable: predictive laser self-unfreeze.
static SRow g_LaserUnfreezeChildren[] = {
        {ERowType::Toggle, "Auto laser", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "FOV", false, false, 5.0f, 360.0f, 5.0f, 90.0f, "°", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Angles", false, false, 1.0f, 144.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Ticks", false, false, 1.0f, 50.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Trigger ticks", false, false, 1.0f, 5.0f, 1.0f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Silent", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show attempt", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Fake Aim" expandable.
static const char *const g_FakeAimModeOpts[] = {"Random", "Robot", "Spin", "Lag"};
static SRow g_FakeAimChildren[] = {
        {ERowType::Dropdown, "Mode", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_FakeAimModeOpts, 4, 0, nullptr, 0},
        {ERowType::Slider, "Speed", false, false, 1.0f, 100.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show for me", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_AdvancedRows[] = {
        {ERowType::Toggle, "Trajectory", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Laser unfreeze", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_LaserUnfreezeChildren, 7},
        {ERowType::Expandable, "Fake Aim", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_FakeAimChildren, 3},
};

// ---------- Settings panel (6) ----------
// "IRC" expandable: 2 sub-toggles.
static SRow g_IrcChildren[] = {
        {ERowType::Toggle, "Reveal join", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Nameplate tag", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Spoofer" expandable: slider + dropdown + input.
static const char *const g_ClientPresetOpts[] = {"DDNet", "TClient", "BestClient", "Custom"};
static SRow g_SpooferChildren[] = {
        {ERowType::Slider, "Version ID", false, false, 10000.0f, 21000.0f, 1.0f, 20000.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Client preset", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ClientPresetOpts, 4, 0, nullptr, 0},
        {ERowType::Input, "Client version", false, false, 0, 0, 0, 0, nullptr, "10.8.7", "version...", nullptr, 0, 0, nullptr, 0},
};
static SRow g_SettingsRows[] = {
        {ERowType::Expandable, "IRC", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_IrcChildren, 2},
        {ERowType::Expandable, "Spoofer", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SpooferChildren, 3},
};

// ---------- Pathfinder panel (7) ----------
// "Score method" visual expandable (cannot toggle off, only expand).
static SRow g_ScoreMethodChildren[] = {
        {ERowType::Toggle, "Distance reduction", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Flow-field alignment", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Penalties" visual expandable.
static SRow g_PenaltiesChildren[] = {
        {ERowType::Toggle, "Freeze penalty", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Hook retract penalty", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Death penalty", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Wall-hit penalty", false, false, 0, 0, 0,0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Air-time penalty", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Hook-fire penalty", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Stuck penalty", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Visuals" visual expandable.
static SRow g_VisualsChildren[] = {
        {ERowType::Toggle, "Show field", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show hooks", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show branches", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show speed", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_PathfinderRows[] = {
        {ERowType::Slider, "Chunk size", false, false, 1.0f, 50.0f, 1.0f, 20.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Hook angles", false, false, 4.0f, 32.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Candidates", false, false, 50.0f, 2000.0f, 10.0f, 200.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Performance", false, false, 1.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Advanced search", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Score method", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_ScoreMethodChildren, 2},
        {ERowType::Expandable, "Penalties", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_PenaltiesChildren, 7},
        {ERowType::Expandable, "Visuals", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_VisualsChildren, 4},
        {ERowType::Button, "Pathfinding", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

struct SPanel
{
        const char *pTitle;
        SRow *pRows;
        int rowCount;
};

static SPanel g_Panels[NUM_PANELS] = {
        {"Functions", g_FunctionsRows, sizeof(g_FunctionsRows) / sizeof(g_FunctionsRows[0])},
        {"Advanced", g_AdvancedRows, sizeof(g_AdvancedRows) / sizeof(g_AdvancedRows[0])},
        {"Settings", g_SettingsRows, sizeof(g_SettingsRows) / sizeof(g_SettingsRows[0])},
        {"Pathfinder", g_PathfinderRows, sizeof(g_PathfinderRows) / sizeof(g_PathfinderRows[0])},
};

// Conditional visibility for Functions panel top-level rows.
// Mirror aim X/Y visible only when Aim mode = Mirror (valueIdx=1).
// Smart aim visible only when Aim mode = Smart (valueIdx=0).
// All other rows always visible.
static bool FunctionsRowVisible(const SRow *pRows, int rowCount, int idx)
{
        if(idx < 0 || idx >= rowCount)
                return false;
        const SRow &row = pRows[idx];
        // Find Aim mode dropdown in the same array.
        int aimModeIdx = 0; // default Smart
        for(int i = 0; i < rowCount; ++i)
        {
                if(str_comp(pRows[i].pName, "Aim mode") == 0)
                {
                        aimModeIdx = pRows[i].valueIdx;
                        break;
                }
        }
        const bool isMirror = (aimModeIdx == 1);
        if(str_comp(row.pName, "Mirror aim X") == 0 || str_comp(row.pName, "Mirror aim Y") == 0)
                return isMirror;
        if(str_comp(row.pName, "Smart aim") == 0)
                return !isMirror;
        return true;
}

// Fake Aim: Speed hidden when Robot mode (modeIdx == 1)
static bool FakeAimChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        int modeIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Mode") == 0)
                {
                        modeIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        if(str_comp(row.pName, "Speed") == 0)
                return modeIdx != 1;
        return true;
}

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
static constexpr unsigned COL_CHECK_SQUARE = 0xff282834; // v1.56.61: rounded square bg under the checkmark (slightly lighter than COL_PANEL_BG=0xff161622)
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
static constexpr float ROW_H_TOGGLE = 30.0f;
static constexpr float ROW_H_SLIDER = 48.0f;
static constexpr float ROW_H_INPUT = 34.0f;
static constexpr float ROW_H_EXPANDABLE = 30.0f;
static constexpr float ROW_H_BUTTON = 36.0f;

// Row internal layout (px) — scaled up 1.25x.
static constexpr float CHECK_W = 22.0f; // v1.56.61: enlarged from 18 for better visibility
static constexpr float ARROW_W = 18.0f;
static constexpr float DROPDOWN_W = 120.0f;
static constexpr float DROPDOWN_H = 24.0f;
static constexpr float SLIDER_TRACK_H = 6.0f;
static constexpr float SLIDER_THUMB_R = 8.0f;
static constexpr float SUB_LIST_INDENT = 22.0f;

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
        case ERowType::Button:
                return ROW_H_BUTTON;
        }
        return ROW_H_TOGGLE;
}

} // anonymous namespace

// ============================================================================
// CClickGui — static helpers
// ============================================================================

vec2 CClickGui::PanelHomePos(int panelIdx)
{
        // 4 panels: Functions, Advanced, Settings, Pathfinder.
        // Spaced horizontally at the top, 330px apart (300px panel + 30px gap).
        switch(panelIdx)
        {
        case 0: return vec2(60.0f, 90.0f); // Functions
        case 1: return vec2(390.0f, 90.0f); // Advanced
        case 2: return vec2(720.0f, 90.0f); // Settings
        case 3: return vec2(1050.0f, 90.0f); // Pathfinder
        default: return vec2(60.0f, 90.0f);
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

// v1.56.61: Direction-aware finish-easing. See header doc.
// Returns an eased value in [0,1] with finish-easing (fast-start, slow-end)
// regardless of animation direction. The `target` parameter selects which
// mirror of the curve to use so both opening and closing feel symmetric.
float CClickGui::EaseBothDirections(float rawProgress, float target)
{
        const float t = std::clamp(rawProgress, 0.0f, 1.0f);
        if(target > 0.5f)
                return CubicBezierEase(t); // opening: fast-start, slow-end
        return 1.0f - CubicBezierEase(1.0f - t); // closing: mirrored → fast-start vanish, slow-end
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

// v1.56.60: UpdateAnimations — advance per-row (m_AnimExpand) and
// per-panel (m_aPanelCollapseAnim) progress values toward their targets using
// frame-rate-independent exponential approach. Easing is applied at draw-time
// via EaseBothDirections(progress, target) so the motion has finish-easing
// (fast start, slow end) in both directions.
//
// Target rules:
//   m_AnimExpand → (row.expanded? 1.0 : 0.0)   for every Expandable row (children slide)
//   m_aPanelCollapseAnim[i] → (m_aPanelCollapsed[i] ? 1.0 : 0.0)
//
// Speed: 1 / DURATION per second. We use a clamped linear step (not exponential)
// so the animation reaches exactly 0.0 or 1.0 in bounded time, matching the
// legacy m_AnimProgress style above.
void CClickGui::UpdateAnimations(float dt)
{
        const float expandSpeed = (EXPAND_ANIM_DURATION > 0.0f) ? (1.0f / EXPAND_ANIM_DURATION) : 1.0f;

        auto lerpToward = [dt](float &val, float target, float speed) {
                if(val < target)
                {
                        val += dt * speed;
                        if(val > target)
                                val = target;
                }
                else if(val > target)
                {
                        val -= dt * speed;
                        if(val < target)
                                val = target;
                }
        };

        // Per-panel collapse animation.
        for(int i = 0; i < NUM_PANELS; ++i)
        {
                const float target = m_aPanelCollapsed[i] ? 1.0f : 0.0f;
                lerpToward(m_aPanelCollapseAnim[i], target, expandSpeed);
        }

        // Per-row expand animations across all panels.
        for(int p = 0; p < NUM_PANELS; ++p)
        {
                SPanel &panel = g_Panels[p];
                for(int r = 0; r < panel.rowCount; ++r)
                {
                        SRow &row = panel.pRows[r];
                        if(row.type == ERowType::Expandable)
                                lerpToward(row.m_AnimExpand, row.expanded ? 1.0f : 0.0f, expandSpeed);

                        // Children of expandable rows may themselves be expandable.
                        if(row.pChildren)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        if(child.type == ERowType::Expandable)
                                                lerpToward(child.m_AnimExpand, child.expanded ? 1.0f : 0.0f, expandSpeed);
                                }
                        }
                }
        }
}

// ============================================================================
// CClickGui — component lifecycle
// ============================================================================

void CClickGui::OnReset()
{
        m_AnimProgress = 0.0f;
        m_WasOpen = false;
        m_Dragging = false;
        m_DragPanelIdx = -1;
        m_DragSliderPanel = -1;
        m_DragSliderRow = -1;
        m_DragSliderChild = -1;
        m_DropdownOpen = false;
        m_InputEditing = false;
        Input()->StopTextInput();
        for(int i = 0; i < NUM_PANELS; ++i)
        {
                m_aOffscreenValid[i] = false;
                m_aPanelPosValid[i] = false;
                m_aPanelCollapsed[i] = false;
                m_aZOrder[i] = i;
        }
}

void CClickGui::OnRender()
{
        const bool isOpen = g_Config.m_ClClickGui != 0;

        // Update mouse position from native SDL cursor (screen-space px).
        m_MousePos = Input()->NativeMousePos();

        // Detect transition 0->1 (opening): pick fresh random offscreen positions
        // for each panel and initialize panel positions to home (if first open).
        if(isOpen && !m_WasOpen)
        {
                const float sw = (float)Graphics()->ScreenWidth();
                const float sh = (float)Graphics()->ScreenHeight();
                for(int i = 0; i < NUM_PANELS; ++i)
                {
                        m_aOffscreenPos[i] = PickRandomOffscreenPos(i, sw, sh);
                        m_aOffscreenValid[i] = true;
                        if(!m_aPanelPosValid[i])
                        {
                                m_aPanelPos[i] = PanelHomePos(i);
                                m_aPanelTarget[i] = PanelHomePos(i);
                                m_aPanelPosValid[i] = true;
                        }
                }
                // Sync reference toggles with real subsystem state on open.
                // (So if c_attack / c_random_aim / cl_show_trajectory were enabled
                // via console before opening ClickGUI, the checkboxes reflect that.)
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(str_comp(row.pName, "Block") == 0)
                                        row.on = GameClient()->m_BotNet.m_AttackEnabled;
                                else if(str_comp(row.pName, "Random Aim") == 0)
                                        row.on = GameClient()->m_BotNet.m_RandomAim;
                                else if(str_comp(row.pName, "Trajectory") == 0)
                                        row.on = g_Config.m_ClShowTrajectory != 0;
                                // Advanced → Laser unfreeze expandable is master toggle.
                                else if(str_comp(row.pName, "Laser unfreeze") == 0)
                                        row.on = g_Config.m_ClLaserUnfreeze != 0;
                                else if(str_comp(row.pName, "Fake Aim") == 0)
                                        row.on = g_Config.m_ClFakeAim != 0;
                                // Functions panel — Fly/Copy Moves sub-section
                                else if(str_comp(row.pName, "Preserve input") == 0)
                                        row.on = g_Config.m_ClDummyHammerKeepInputs != 0;
                                else if(str_comp(row.pName, "Custom latency") == 0)
                                        row.on = g_Config.m_ClDummyHammerCustomLatency != 0;
                                else if(str_comp(row.pName, "Auto fly") == 0)
                                        row.on = g_Config.m_ClDummyHammerAuto != 0;
                                else if(str_comp(row.pName, "Mirror direction") == 0)
                                        row.on = g_Config.m_ClDummyCopyMirrorDir != 0;
                                else if(str_comp(row.pName, "Aim mode") == 0)
                                        row.valueIdx = g_Config.m_ClDummyCopyAimMode;
                                else if(str_comp(row.pName, "Mirror aim X") == 0)
                                        row.on = g_Config.m_ClDummyCopyMirrorAimX != 0;
                                else if(str_comp(row.pName, "Mirror aim Y") == 0)
                                        row.on = g_Config.m_ClDummyCopyMirrorAimY != 0;
                                else if(str_comp(row.pName, "Smart aim") == 0)
                                        row.on = g_Config.m_ClDummyCopySmartAim != 0;
                                // Functions → Copy Moves Filter expandable is master toggle.
                                else if(str_comp(row.pName, "Copy Moves Filter") == 0)
                                        row.on = g_Config.m_ClDummyCopyMovesFilter != 0;
                                // Pathfinder top-level sliders/toggles
                                else if(str_comp(row.pName, "Chunk size") == 0)
                                        row.value = (float)g_Config.m_ClPfChunkSize;
                                else if(str_comp(row.pName, "Hook angles") == 0)
                                        row.value = (float)g_Config.m_ClPfHookAngles;
                                else if(str_comp(row.pName, "Candidates") == 0)
                                        row.value = (float)g_Config.m_ClPfCandidates;
                                else if(str_comp(row.pName, "Performance") == 0)
                                        row.value = (float)g_Config.m_ClPfPerf;
                                else if(str_comp(row.pName, "Advanced search") == 0)
                                        row.on = g_Config.m_ClPfAdvancedSearch != 0;
                                // Settings → IRC/Spoofer top-level expandables are master toggles.
                                else if(str_comp(row.pName, "IRC") == 0)
                                        row.on = g_Config.m_ClIrcEnabled != 0;
                                else if(str_comp(row.pName, "Spoofer") == 0)
                                        row.on = (g_Config.m_ClVersionSpoof != 0 || g_Config.m_ClSpoofClientStr != 0);
                                // Sync children of expandable rows
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                        if(str_comp(child.pName, "Latency (ms)") == 0)
                                                child.value = (float)g_Config.m_ClDummyHammerLatencyMs;
                                        else if(str_comp(child.pName, "Filter: Jump") == 0)
                                                child.on = g_Config.m_ClDummyCopyMovesFilterJump != 0;
                                        else if(str_comp(child.pName, "Filter: Direction") == 0)
                                                child.on = g_Config.m_ClDummyCopyMovesFilterDirection != 0;
                                        else if(str_comp(child.pName, "Filter: Hook") == 0)
                                                child.on = g_Config.m_ClDummyCopyMovesFilterHook != 0;
                                        else if(str_comp(child.pName, "Filter: Aim") == 0)
                                                child.on = g_Config.m_ClDummyCopyMovesFilterAim != 0;
                                        else if(str_comp(child.pName, "Filter: Fire") == 0)
                                                child.on = g_Config.m_ClDummyCopyMovesFilterFire != 0;
                                        // Settings → IRC children
                                        else if(str_comp(child.pName, "Reveal join") == 0)
                                                child.on = g_Config.m_ClIrcRevealJoin != 0;
                                        else if(str_comp(child.pName, "Nameplate tag") == 0)
                                                child.on = g_Config.m_ClIrcNameplateTag != 0;
                                        // Advanced → Laser unfreeze children
                                        else if(str_comp(child.pName, "Auto laser") == 0)
                                                child.on = g_Config.m_ClLaserUnfreezeAuto != 0;
                                        else if(str_comp(child.pName, "FOV") == 0)
                                                child.value = (float)g_Config.m_ClLaserUnfreezeFov;
                                        else if(str_comp(child.pName, "Angles") == 0)
                                                child.value = (float)g_Config.m_ClLaserUnfreezeAngles;
                                        else if(str_comp(child.pName, "Ticks") == 0)
                                                child.value = (float)g_Config.m_ClLaserUnfreezeTicks;
                                        else if(str_comp(child.pName, "Trigger ticks") == 0)
                                                child.value = (float)g_Config.m_ClLaserUnfreezeTriggerTicks;
                                        else if(str_comp(child.pName, "Silent") == 0)
                                                child.on = g_Config.m_ClLaserUnfreezeSilent != 0;
                                        else if(str_comp(child.pName, "Show attempt") == 0)
                                                child.on = g_Config.m_ClLaserUnfreezeShowAttempt != 0;
                                        // Fake Aim children
                                        else if(str_comp(child.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.valueIdx = g_Config.m_ClFakeAimMode;
                                        else if(str_comp(child.pName, "Speed") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.value = (float)g_Config.m_ClFakeAimSpeed;
                                        else if(str_comp(child.pName, "Show for me") == 0)
                                                child.on = g_Config.m_ClFakeAimShowForMe != 0;
                                        // Settings → Spoofer children
                                        else if(str_comp(child.pName, "Version ID") == 0)
                                                child.value = (float)g_Config.m_ClVersionSpoofId;
                                        else if(str_comp(child.pName, "Client preset") == 0)
                                                child.valueIdx = g_Config.m_ClSpoofClientPreset;
                                        // Pathfinder → Score method children
                                        else if(str_comp(child.pName, "Distance reduction") == 0)
                                                child.on = g_Config.m_ClPfScoreDist != 0;
                                        else if(str_comp(child.pName, "Flow-field alignment") == 0)
                                                child.on = g_Config.m_ClPfScoreFlow != 0;
                                        // Pathfinder → Penalties children
                                        else if(str_comp(child.pName, "Freeze penalty") == 0)
                                                child.on = g_Config.m_ClPfFineFreeze != 0;
                                        else if(str_comp(child.pName, "Hook retract penalty") == 0)
                                                child.on = g_Config.m_ClPfFineRetract != 0;
                                        else if(str_comp(child.pName, "Death penalty") == 0)
                                                child.on = g_Config.m_ClPfFineDeath != 0;
                                        else if(str_comp(child.pName, "Wall-hit penalty") == 0)
                                                child.on = g_Config.m_ClPfFineWall != 0;
                                        else if(str_comp(child.pName, "Air-time penalty") == 0)
                                                child.on = g_Config.m_ClPfFineAir != 0;
                                        else if(str_comp(child.pName, "Hook-fire penalty") == 0)
                                                child.on = g_Config.m_ClPfFineHook != 0;
                                        else if(str_comp(child.pName, "Stuck penalty") == 0)
                                                child.on = g_Config.m_ClPfFineStuck != 0;
                                        // Pathfinder → Visuals children
                                        else if(str_comp(child.pName, "Show field") == 0)
                                                child.on = g_Config.m_ClPfShowField != 0;
                                        else if(str_comp(child.pName, "Show hooks") == 0)
                                                child.on = g_Config.m_ClPfShowHooks != 0;
                                        else if(str_comp(child.pName, "Show branches") == 0)
                                                child.on = g_Config.m_ClPfShowBranches != 0;
                                        else if(str_comp(child.pName, "Show speed") == 0)
                                                child.on = g_Config.m_ClPfShowSpeed != 0;
                                }
                        }
                }
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

        // v1.56.60: advance per-row expand/collapse animations and per-panel
        // collapse animations. Runs every frame regardless of open state so
        // the next open shows consistent mid-animation values.
        UpdateAnimations(dt);

        // When fully closed, restore relative mouse mode (hide cursor) and bail.
        // Don't touch mouse mode if game console (F1/F2) is open — it manages
        // mouse mode itself and we'd conflict with it.
        if(m_AnimProgress <= 0.0f && !isOpen)
        {
                if(!GameClient()->m_GameConsole.IsActive())
                        Input()->MouseModeRelative();
                // Also stop text input if we were editing when ClickGUI closed.
                if(m_InputEditing)
                {
                        m_InputEditing = false;
                        Input()->StopTextInput();
                }
                return;
        }

        // While open (or animating), force absolute mouse mode EVERY FRAME.
        // Skip if game console (F1/F2) is open — it manages mouse mode itself.
        if(isOpen && !GameClient()->m_GameConsole.IsActive())
                Input()->MouseModeAbsolute();

        // If dragging a panel, set its target position to follow the mouse.
        // The panel lerps toward this target (smooth drag) in the render loop below.
        if(m_Dragging && m_DragPanelIdx >= 0)
        {
                m_aPanelTarget[m_DragPanelIdx] = m_MousePos - m_DragOffset;
        }

        // If dragging a slider, update its value from mouse X position.
        if(m_DragSliderPanel >= 0 && m_DragSliderRow >= 0)
        {
                SPanel &panel = g_Panels[m_DragSliderPanel];
                SRow &row = panel.pRows[m_DragSliderRow];
                SRow *pTarget = (m_DragSliderChild >= 0 && row.pChildren) ? &row.pChildren[m_DragSliderChild] : &row;
                if(pTarget->type == ERowType::Slider && pTarget->max > pTarget->min)
                {
                        // Track geometry MUST match RenderPanel: 2px margin each side.
                        // Child sliders are indented by SUB_LIST_INDENT, so their track
                        // is narrower — must account for that or the thumb position
                        // won't match the mouse.
                        const vec2 ppos = m_aPanelPos[m_DragSliderPanel];
                        const float indent = (m_DragSliderChild >= 0) ? SUB_LIST_INDENT : 0.0f;
                        const float trackX = ppos.x + BODY_PADDING + indent + 2.0f;
                        const float trackW = PANEL_WIDTH - BODY_PADDING * 2 - indent - 4.0f;
                        float pct = (m_MousePos.x - trackX) / trackW;
                        pct = std::clamp(pct, 0.0f, 1.0f);
                        {
                                float raw = pTarget->min + pct * (pTarget->max - pTarget->min);
                                if(pTarget->step >= 1.0f)
                                        pTarget->value = pTarget->min + roundf((raw - pTarget->min) / pTarget->step) * pTarget->step;
                                else
                                        pTarget->value = raw;
                        }
                        if(str_comp(pTarget->pName, "Latency (ms)") == 0)
                                g_Config.m_ClDummyHammerLatencyMs = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Version ID") == 0)
                                g_Config.m_ClVersionSpoofId = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "FOV") == 0)
                                g_Config.m_ClLaserUnfreezeFov = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Angles") == 0)
                                g_Config.m_ClLaserUnfreezeAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks") == 0)
                                g_Config.m_ClLaserUnfreezeTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger ticks") == 0)
                                g_Config.m_ClLaserUnfreezeTriggerTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Speed") == 0)
                                g_Config.m_ClFakeAimSpeed = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Chunk size") == 0)
                                g_Config.m_ClPfChunkSize = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Hook angles") == 0)
                                g_Config.m_ClPfHookAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Candidates") == 0)
                                g_Config.m_ClPfCandidates = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Performance") == 0)
                                g_Config.m_ClPfPerf = (int)pTarget->value;
                }
        }

        // Easing: open uses CubicBezierEase(progress) which is fast-start, slow-end.
        // Close should mirror this so the easing is at the END of the animation
        // (slow-start, fast-end) — i.e. panels decelerate as they vanish, matching
        // the HTML's symmetric transition. We invert the curve for close direction.
        float eased;
        if(isOpen)
                eased = CubicBezierEase(m_AnimProgress); // 0→1, fast-start slow-end
        else
                eased = 1.0f - CubicBezierEase(1.0f - m_AnimProgress); // 1→0, slow-start fast-end
        const float alpha = eased;

        // Set up pixel-space projection.
        const float screenW = (float)Graphics()->ScreenWidth();
        const float screenH = (float)Graphics()->ScreenHeight();
        Graphics()->MapScreen(0.0f, 0.0f, screenW, screenH);

        Graphics()->TextureClear();

        // Background dimming: black overlay, alpha = BG_DIM_ALPHA * eased.
        // Drawn FIRST so panels appear on top of it. Fades in/out smoothly.
        if(eased > 0.0f)
        {
                Graphics()->BlendNormal();
                Graphics()->DrawRect(0.0f, 0.0f, screenW, screenH,
                        ColorRGBA(0.0f, 0.0f, 0.0f, BG_DIM_ALPHA * eased),
                        IGraphics::CORNER_NONE, 0.0f);
        }

        // Render panels in z-order (low z first = painted behind, high z on top).
        // Build a list of panel indices sorted by z-order ascending.
        int renderOrder[NUM_PANELS];
        for(int i = 0; i < NUM_PANELS; ++i)
                renderOrder[i] = i;
        for(int a = 0; a < NUM_PANELS; ++a)
                for(int b = a + 1; b < NUM_PANELS; ++b)
                        if(m_aZOrder[renderOrder[b]] < m_aZOrder[renderOrder[a]])
                        {
                                int tmp = renderOrder[a];
                                renderOrder[a] = renderOrder[b];
                                renderOrder[b] = tmp;
                        }

        for(int idx = 0; idx < NUM_PANELS; ++idx)
        {
                const int i = renderOrder[idx];
                const vec2 home = PanelHomePos(i);
                // Initialize target on first valid frame.
                if(!m_aPanelPosValid[i])
                {
                        m_aPanelPos[i] = home;
                        m_aPanelTarget[i] = home;
                        m_aPanelPosValid[i] = true;
                }
        // Smooth drag: lerp current position toward target, frame-rate independent.
        // Uses exponential approach: factor = 1 - exp(-k * dt). At high FPS the
        // per-frame step is small; at low FPS it's large — total speed is constant.
        // k = 25 means ~63% of remaining distance covered in 1/25s (40ms).
        // Only the dragged panel lerps; others keep target synced (no glide).
        const float dtDrag = Client()->RenderFrameTime();
        const float dragK = 25.0f;
        const float dragFactor = 1.0f - std::exp(-dragK * dtDrag);
        if(m_Dragging && m_DragPanelIdx == i)
        {
                m_aPanelPos[i] = vec2(
                        m_aPanelPos[i].x + (m_aPanelTarget[i].x - m_aPanelPos[i].x) * dragFactor,
                        m_aPanelPos[i].y + (m_aPanelTarget[i].y - m_aPanelPos[i].y) * dragFactor);
        }
        else
                m_aPanelTarget[i] = m_aPanelPos[i];
        const vec2 current = m_aPanelPos[i];
                vec2 offscreen;
                if(m_aOffscreenValid[i])
                        offscreen = m_aOffscreenPos[i];
                else
                {
                        offscreen = PickRandomOffscreenPos(i, screenW, screenH);
                        m_aOffscreenPos[i] = offscreen;
                        m_aOffscreenValid[i] = true;
                }
                // Position = lerp(offscreen, current, eased).
                // When closed (eased=0), panel is fully offscreen; when open (eased=1),
                // panel is at its dragged/home position.
                const vec2 pos = vec2(
                        offscreen.x + (current.x - offscreen.x) * eased,
                        offscreen.y + (current.y - offscreen.y) * eased);
                RenderPanel(i, pos, alpha);
        }

        // Render dropdown popup ON TOP of all panels (if open).
        RenderDropdownPopup(alpha);

        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ============================================================================
// CClickGui — UX (input handling)
// ============================================================================

bool CClickGui::OnInput(const IInput::CEvent &Event)
{
        if(g_Config.m_ClClickGui == 0 && m_AnimProgress <= 0.0f)
                return false;

        // Only handle events when ClickGUI is fully open (avoid clicks during anim).
        if(m_AnimProgress < 0.95f)
                return false;

        // Mouse clicks ALWAYS go through HandleMouseDown/Up — consumed by ClickGUI.
        if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_MOUSE_1)
        {
                HandleMouseDown(m_MousePos);
                return true;
        }
        if(Event.m_Flags & IInput::FLAG_RELEASE && Event.m_Key == KEY_MOUSE_1)
        {
                HandleMouseUp(m_MousePos);
                return true;
        }

        // If editing an input field, route keyboard events to text editing.
        // (Mouse clicks already handled above — they can commit/switch input focus.)
        if(m_InputEditing && (Event.m_Flags & (IInput::FLAG_PRESS | IInput::FLAG_TEXT)))
        {
                HandleTextInput(Event);
                return true;
        }

        // Keyboard input when NOT editing an input field: let it fall through to
        // the game so binds (e.g. tab to toggle cl_clickgui, movement keys) work.
        // Only mouse is consumed by ClickGUI; keyboard is free for the game.
        return false;
}

bool CClickGui::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
        if(g_Config.m_ClClickGui != 0)
        {
                // NativeMousePos is updated by SDL; we just need to consume the event.
                return true;
        }
        return false;
}

void CClickGui::BringToFront(int panelIdx)
{
        const int oldZ = m_aZOrder[panelIdx];
        if(oldZ == NUM_PANELS - 1)
                return; // already on top
        // Shift panels with z > oldZ down by 1, set this panel to top.
        const int topZ = NUM_PANELS - 1;
        for(int i = 0; i < NUM_PANELS; ++i)
        {
                if(i == panelIdx)
                        m_aZOrder[i] = topZ;
                else if(m_aZOrder[i] > oldZ)
                        m_aZOrder[i] -= 1;
        }
}

bool CClickGui::PointInRect(vec2 p, float x, float y, float w, float h) const
{
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
}

void CClickGui::HandleMouseDown(vec2 mousePos)
{
        // If a dropdown popup is open, check it FIRST (it's on top of everything).
        if(m_DropdownOpen)
        {
                if(HandleDropdownClick(mousePos))
                        return;
                // Click outside dropdown → close it (and don't fall through to row
                // beneath, matching HTML select behavior).
                m_DropdownOpen = false;
                return;
        }

        // If editing an input field, commit the edit buffer back to the row before
        // processing the new click (so the typed text is saved).
        if(m_InputEditing)
        {
                SRow *pRow = (SRow *)GetEditingInputRowPtr();
                if(pRow)
                {
                        if(!pRow->m_InputInitialized)
                        {
                                if(pRow->pInputValue)
                                        str_copy(pRow->m_aInputBuf, pRow->pInputValue, sizeof(pRow->m_aInputBuf));
                                pRow->m_InputInitialized = true;
                        }
                        mem_copy(pRow->m_aInputBuf, m_InputBuf, m_InputLen + 1);
                }
                m_InputEditing = false; Input()->StopTextInput();
        }

        // Iterate panels top-to-bottom (high z first) to find the topmost hit.
        int clickOrder[NUM_PANELS];
        for(int i = 0; i < NUM_PANELS; ++i)
                clickOrder[i] = i;
        for(int a = 0; a < NUM_PANELS; ++a)
                for(int b = a + 1; b < NUM_PANELS; ++b)
                        if(m_aZOrder[clickOrder[b]] > m_aZOrder[clickOrder[a]])
                        {
                                int tmp = clickOrder[a];
                                clickOrder[a] = clickOrder[b];
                                clickOrder[b] = tmp;
                        }

        for(int idx = 0; idx < NUM_PANELS; ++idx)
        {
                const int i = clickOrder[idx];
                const vec2 ppos = m_aPanelPosValid[i] ? m_aPanelPos[i] : PanelHomePos(i);

                // Check header hit (drag + collapse arrow).
                if(PointInRect(mousePos, ppos.x, ppos.y, PANEL_WIDTH, HEADER_HEIGHT))
                {
                        BringToFront(i);

                        // Collapse arrow ▼ on left side of header (first ~24px).
                        if(PointInRect(mousePos, ppos.x + 4.0f, ppos.y + 4.0f, 24.0f, HEADER_HEIGHT - 8.0f))
                        {
                                m_aPanelCollapsed[i] = !m_aPanelCollapsed[i];
                                return;
                        }

                        // Otherwise start dragging.
                        m_Dragging = true;
                        m_DragPanelIdx = i;
                        m_DragOffset = mousePos - ppos;
                        // Target is set in OnRender render loop; just mark dragging.
                        return;
                }

                // If panel is collapsed, body doesn't exist — skip row hit-test.
                if(m_aPanelCollapsed[i])
                        continue;

                // Check row hits in the body.
                int outRow, outChild;
                float rx, ry, rw, rh;
                if(HitTestRow(mousePos, i, outRow, outChild, rx, ry, rw, rh))
                {
                        BringToFront(i);
                        SPanel &panel = g_Panels[i];
                        SRow &row = panel.pRows[outRow];
                        SRow *pTarget = (outChild >= 0 && row.pChildren) ? &row.pChildren[outChild] : &row;

                        // Sub-region hits within the row.
                        if(pTarget->type == ERowType::Toggle || pTarget->type == ERowType::ToggleDropdown || pTarget->type == ERowType::Expandable)
                        {
                                // Visual expandables (Score method, Penalties, Visuals) cannot be
                                // toggled off — only expand/collapse. Click on name = expand toggle.
                                const bool isVisualExpandable = (pTarget->type == ERowType::Expandable) &&
                                        (str_comp(pTarget->pName, "Score method") == 0 ||
                                         str_comp(pTarget->pName, "Penalties") == 0 ||
                                         str_comp(pTarget->pName, "Visuals") == 0);
                                // Click on check area or name area → toggle on/off (or expand for visual).
                                const float toggleEndX = rx + rw - (pTarget->type == ERowType::Expandable ? ARROW_W + 4.0f : (pTarget->type == ERowType::ToggleDropdown ? DROPDOWN_W + 4.0f : 0.0f));
                                if(mousePos.x < toggleEndX)
                                {
                                        if(isVisualExpandable)
                                        {
                                                // Visual expandable: only expand/collapse, no toggle.
                                                pTarget->expanded = !pTarget->expanded;
                                                return;
                                        }
                                        pTarget->on = !pTarget->on;
                                        // Wire reference toggles to real subsystems.
                                        if(str_comp(pTarget->pName, "Block") == 0)
                                                GameClient()->m_BotNet.m_AttackEnabled = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Random Aim") == 0)
                                                GameClient()->m_BotNet.m_RandomAim = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Trajectory") == 0)
                                                g_Config.m_ClShowTrajectory = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Laser unfreeze") == 0)
                                                g_Config.m_ClLaserUnfreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Fake Aim") == 0)
                                                g_Config.m_ClFakeAim = pTarget->on ? 1 : 0;
                                        // Functions panel — Fly/Copy Moves sub-section
                                        else if(str_comp(pTarget->pName, "Preserve input") == 0)
                                                g_Config.m_ClDummyHammerKeepInputs = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom latency") == 0)
                                                g_Config.m_ClDummyHammerCustomLatency = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Auto fly") == 0)
                                                g_Config.m_ClDummyHammerAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror direction") == 0)
                                                g_Config.m_ClDummyCopyMirrorDir = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror aim X") == 0)
                                                g_Config.m_ClDummyCopyMirrorAimX = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror aim Y") == 0)
                                                g_Config.m_ClDummyCopyMirrorAimY = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Smart aim") == 0)
                                                g_Config.m_ClDummyCopySmartAim = pTarget->on ? 1 : 0;
                                        // Copy Moves Filter expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Copy Moves Filter") == 0)
                                                g_Config.m_ClDummyCopyMovesFilter = pTarget->on ? 1 : 0;
                                        // Copy Moves Filter sub-toggles
                                        else if(str_comp(pTarget->pName, "Filter: Jump") == 0)
                                                g_Config.m_ClDummyCopyMovesFilterJump = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Direction") == 0)
                                                g_Config.m_ClDummyCopyMovesFilterDirection = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Hook") == 0)
                                                g_Config.m_ClDummyCopyMovesFilterHook = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Aim") == 0)
                                                g_Config.m_ClDummyCopyMovesFilterAim = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Fire") == 0)
                                                g_Config.m_ClDummyCopyMovesFilterFire = pTarget->on ? 1 : 0;
                                        // Settings → IRC children
                                        else if(str_comp(pTarget->pName, "Reveal join") == 0)
                                                g_Config.m_ClIrcRevealJoin = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Nameplate tag") == 0)
                                                g_Config.m_ClIrcNameplateTag = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze children
                                        else if(str_comp(pTarget->pName, "Auto laser") == 0)
                                                g_Config.m_ClLaserUnfreezeAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Silent") == 0)
                                                g_Config.m_ClLaserUnfreezeSilent = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show attempt") == 0)
                                                g_Config.m_ClLaserUnfreezeShowAttempt = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show for me") == 0)
                                                g_Config.m_ClFakeAimShowForMe = pTarget->on ? 1 : 0;
                                        // Pathfinder → Score method children
                                        else if(str_comp(pTarget->pName, "Distance reduction") == 0)
                                                g_Config.m_ClPfScoreDist = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Flow-field alignment") == 0)
                                                g_Config.m_ClPfScoreFlow = pTarget->on ? 1 : 0;
                                        // Pathfinder → Penalties children
                                        else if(str_comp(pTarget->pName, "Freeze penalty") == 0)
                                                g_Config.m_ClPfFineFreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Hook retract penalty") == 0)
                                                g_Config.m_ClPfFineRetract = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Death penalty") == 0)
                                                g_Config.m_ClPfFineDeath = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Wall-hit penalty") == 0)
                                                g_Config.m_ClPfFineWall = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Air-time penalty") == 0)
                                                g_Config.m_ClPfFineAir = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Hook-fire penalty") == 0)
                                                g_Config.m_ClPfFineHook = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Stuck penalty") == 0)
                                                g_Config.m_ClPfFineStuck = pTarget->on ? 1 : 0;
                                        // Pathfinder → Visuals children
                                        else if(str_comp(pTarget->pName, "Show field") == 0)
                                                g_Config.m_ClPfShowField = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show hooks") == 0)
                                                g_Config.m_ClPfShowHooks = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show branches") == 0)
                                                g_Config.m_ClPfShowBranches = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show speed") == 0)
                                                g_Config.m_ClPfShowSpeed = pTarget->on ? 1 : 0;
                                        // Pathfinder top-level
                                        else if(str_comp(pTarget->pName, "Advanced search") == 0)
                                                g_Config.m_ClPfAdvancedSearch = pTarget->on ? 1 : 0;
                                        // Settings → IRC/Spoofer expandables are master toggles.
                                        else if(str_comp(pTarget->pName, "IRC") == 0)
                                                g_Config.m_ClIrcEnabled = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Spoofer") == 0)
                                        {
                                                g_Config.m_ClVersionSpoof = pTarget->on ? 1 : 0;
                                                g_Config.m_ClSpoofClientStr = pTarget->on ? 1 : 0;
                                        }
                                        return;
                                }
                        }
                        if(pTarget->type == ERowType::Button)
                        {
                                // Button click — dispatch by name.
                                if(str_comp(pTarget->pName, "Pathfinding") == 0)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        if(BN.m_PfState == PF_STATE_IDLE)
                                        {
                                                BN.m_PfState = PF_STATE_RUNNING;
                                                BN.PfResetRun();
                                        }
                                        else if(BN.m_PfState == PF_STATE_RUNNING)
                                        {
                                                BN.m_PfState = PF_STATE_FINISHED;
                                        }
                                        else // PF_STATE_FINISHED
                                        {
                                                BN.m_PfState = PF_STATE_IDLE;
                                                BN.m_PfVPath.clear();
                                        }
                                }
                                return;
                        }
                        if(pTarget->type == ERowType::Expandable)
                        {
                                // Click on ▶/▼ area → toggle expand.
                                if(mousePos.x >= rx + rw - ARROW_W - 4.0f)
                                {
                                        pTarget->expanded = !pTarget->expanded;
                                        return;
                                }
                        }
                        if(pTarget->type == ERowType::Slider)
                        {
                                // Start slider drag.
                                m_DragSliderPanel = i;
                                m_DragSliderRow = outRow;
                                m_DragSliderChild = outChild;
                                const float trackX = rx + 2.0f;
                                const float trackW = rw - 4.0f;
                                float pct = (mousePos.x - trackX) / trackW;
                                pct = std::clamp(pct, 0.0f, 1.0f);
                                {
                                        float raw = pTarget->min + pct * (pTarget->max - pTarget->min);
                                        if(pTarget->step >= 1.0f)
                                                pTarget->value = pTarget->min + roundf((raw - pTarget->min) / pTarget->step) * pTarget->step;
                                        else
                                                pTarget->value = raw;
                                }
                                if(str_comp(pTarget->pName, "Latency (ms)") == 0)
                                        g_Config.m_ClDummyHammerLatencyMs = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Version ID") == 0)
                                        g_Config.m_ClVersionSpoofId = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "FOV") == 0)
                                        g_Config.m_ClLaserUnfreezeFov = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Angles") == 0)
                                        g_Config.m_ClLaserUnfreezeAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks") == 0)
                                        g_Config.m_ClLaserUnfreezeTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger ticks") == 0)
                                        g_Config.m_ClLaserUnfreezeTriggerTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Chunk size") == 0)
                                        g_Config.m_ClPfChunkSize = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Hook angles") == 0)
                                        g_Config.m_ClPfHookAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Candidates") == 0)
                                        g_Config.m_ClPfCandidates = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Performance") == 0)
                                        g_Config.m_ClPfPerf = (int)pTarget->value;
                                return;
                        }
                        if(pTarget->type == ERowType::Dropdown || pTarget->type == ERowType::ToggleDropdown)
                        {
                                // Click on the dropdown box (right side) → open popup.
                                const float ddX = rx + rw - DROPDOWN_W - 2.0f;
                                const float ddY = ry + (rh - DROPDOWN_H) * 0.5f;
                                if(PointInRect(mousePos, ddX, ddY, DROPDOWN_W, DROPDOWN_H))
                                {
                                        m_DropdownOpen = true;
                                        m_DropdownPanel = i;
                                        m_DropdownRow = outRow;
                                        m_DropdownChild = outChild;
                                        return;
                                }
                                // For ToggleDropdown, click on name still toggles (handled above).
                                return;
                        }
                        if(pTarget->type == ERowType::Input)
                        {
                                // Click on the input box → start editing.
                                const float nameW = std::min(88.0f, TextRender()->TextWidth(17.0f, pTarget->pName) + 5.0f);
                                const float inX = rx + nameW + 2.0f;
                                const float inY = ry + 2.0f;
                                const float inW = rw - nameW - 4.0f;
                                const float inH = rh - 4.0f;
                                if(PointInRect(mousePos, inX, inY, inW, inH))
                                {
                                        // Lazy-init the row's mutable buffer from pInputValue.
                                        if(!pTarget->m_InputInitialized)
                                        {
                                                if(pTarget->pInputValue)
                                                        str_copy(pTarget->m_aInputBuf, pTarget->pInputValue, sizeof(pTarget->m_aInputBuf));
                                                pTarget->m_InputInitialized = true;
                                        }
                                        // Start editing. Copy row buffer into edit buffer.
                                        m_InputEditing = true;
                                        m_InputPanel = i;
                                        m_InputRow = outRow;
                                        m_InputChild = outChild;
                                        m_InputLen = (int)str_length(pTarget->m_aInputBuf);
                                        if(m_InputLen >= INPUT_BUF_SIZE)
                                                m_InputLen = INPUT_BUF_SIZE - 1;
                                        mem_copy(m_InputBuf, pTarget->m_aInputBuf, m_InputLen);
                                        m_InputBuf[m_InputLen] = 0;
                                        // Position cursor at click location (approximate: nearest char).
                                        const float textX = inX + 4.0f;
                                        const float clickOffset = mousePos.x - textX;
                                        m_InputCursor = 0;
                                        float accumW = 0.0f;
                                        for(int ci = 0; ci < m_InputLen; ++ci)
                                        {
                                                char tmp[2] = {m_InputBuf[ci], 0};
                                                float cw = TextRender()->TextWidth(17.0f, tmp);
                                                if(accumW + cw * 0.5f > clickOffset)
                                                {
                                                        m_InputCursor = ci;
                                                        break;
                                                }
                                                accumW += cw;
                                                m_InputCursor = ci + 1;
                                        }
                                        m_InputSelStart = -1;
                                        m_InputSelEnd = -1;
                                        // CRITICAL: enable SDL text input so FLAG_TEXT events arrive.
                                        Input()->StartTextInput();
                                        return;
                                }
                                return;
                        }
                        return;
                }
        }
}

void CClickGui::HandleMouseUp(vec2 mousePos)
{
        m_Dragging = false;
        m_DragPanelIdx = -1;
        m_DragSliderPanel = -1;
        m_DragSliderRow = -1;
        m_DragSliderChild = -1;
}

void *CClickGui::GetEditingInputRowPtr()
{
        if(!m_InputEditing || m_InputPanel < 0 || m_InputRow < 0)
                return nullptr;
        SPanel &panel = g_Panels[m_InputPanel];
        if(m_InputRow >= panel.rowCount)
                return nullptr;
        SRow &row = panel.pRows[m_InputRow];
        if(m_InputChild >= 0 && row.pChildren && m_InputChild < row.childCount)
                return &row.pChildren[m_InputChild];
        return &row;
}

bool CClickGui::HandleDropdownClick(vec2 mousePos)
{
        // Find the dropdown box position (same as where we render it).
        if(m_DropdownPanel < 0 || m_DropdownRow < 0)
                return false;
        const vec2 ppos = m_aPanelPosValid[m_DropdownPanel] ? m_aPanelPos[m_DropdownPanel] : PanelHomePos(m_DropdownPanel);
        const float bodyX = ppos.x + BODY_PADDING;
        float cursorY = ppos.y + HEADER_HEIGHT + BODY_PADDING;
        SPanel &panel = g_Panels[m_DropdownPanel];
        for(int r = 0; r < panel.rowCount; ++r)
        {
                SRow &row = panel.pRows[r];
                // Conditional visibility: skip hidden rows in Functions panel.
                if(str_comp(panel.pTitle, "Functions") == 0 && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                {
                        cursorY += RowHeight(row.type) + ROW_GAP;
                        if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                                for(int c = 0; c < row.childCount; ++c)
                                        cursorY += RowHeight(row.pChildren[c].type) + ROW_GAP;
                        continue;
                }
                const float rh = RowHeight(row.type);
                if(r == m_DropdownRow && m_DropdownChild < 0)
                {
                        // Found the row. Dropdown box is at right side.
                        const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                        const float ddX = bodyX + rw - DROPDOWN_W - 2.0f;
                        const float ddY = cursorY + (rh - DROPDOWN_H) * 0.5f;
                        // Popup is a vertical list below the dropdown box.
                        SRow &target = row;
                        const float itemH = DROPDOWN_H + 2.0f;
                        const float popupW = DROPDOWN_W;
                        for(int o = 0; o < target.optionCount; ++o)
                        {
                                const float itemY = ddY + DROPDOWN_H + 2.0f + o * itemH;
                                if(PointInRect(mousePos, ddX, itemY, popupW, itemH - 2.0f))
                                {
                                        target.valueIdx = o;
                                        if(str_comp(target.pName, "Aim mode") == 0)
                                                g_Config.m_ClDummyCopyAimMode = o;
                                        else if(str_comp(target.pName, "Client preset") == 0)
                                                g_Config.m_ClSpoofClientPreset = o;
                                        else if(str_comp(target.pName, "Mode") == 0)
                                                g_Config.m_ClFakeAimMode = o;
                                        m_DropdownOpen = false;
                                        return true;
                                }
                        }
                        return false;
                }
                cursorY += rh + ROW_GAP;
                if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                const float crh = RowHeight(child.type);
                                if(r == m_DropdownRow && c == m_DropdownChild)
                                {
                                        const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                                        const float crw = rw - SUB_LIST_INDENT;
                                        const float ddX = bodyX + SUB_LIST_INDENT + crw - DROPDOWN_W - 2.0f;
                                        const float ddY = cursorY + (crh - DROPDOWN_H) * 0.5f;
                                        SRow &target = child;
                                        const float itemH = DROPDOWN_H + 2.0f;
                                        for(int o = 0; o < target.optionCount; ++o)
                                        {
                                                const float itemY = ddY + DROPDOWN_H + 2.0f + o * itemH;
                                                if(PointInRect(mousePos, ddX, itemY, DROPDOWN_W, itemH - 2.0f))
                                                {
                                                        target.valueIdx = o;
                                                        if(str_comp(target.pName, "Aim mode") == 0)
                                                                g_Config.m_ClDummyCopyAimMode = o;
                                                        else if(str_comp(target.pName, "Client preset") == 0)
                                                                g_Config.m_ClSpoofClientPreset = o;
                                                        else if(str_comp(target.pName, "Mode") == 0)
                                                                g_Config.m_ClFakeAimMode = o;
                                                        m_DropdownOpen = false;
                                                        return true;
                                                }
                                        }
                                        return false;
                                }
                                cursorY += crh + ROW_GAP;
                        }
                }
        }
        return false;
}

void CClickGui::RenderDropdownPopup(float alpha)
{
        if(!m_DropdownOpen || m_DropdownPanel < 0 || m_DropdownRow < 0)
                return;
        // Locate the dropdown box position (mirror HandleDropdownClick traversal).
        const vec2 ppos = m_aPanelPosValid[m_DropdownPanel] ? m_aPanelPos[m_DropdownPanel] : PanelHomePos(m_DropdownPanel);
        const float bodyX = ppos.x + BODY_PADDING;
        float cursorY = ppos.y + HEADER_HEIGHT + BODY_PADDING;
        SPanel &panel = g_Panels[m_DropdownPanel];
        for(int r = 0; r < panel.rowCount; ++r)
        {
                SRow &row = panel.pRows[r];
                // Conditional visibility: skip hidden rows in Functions panel.
                if(str_comp(panel.pTitle, "Functions") == 0 && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                {
                        cursorY += RowHeight(row.type) + ROW_GAP;
                        if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                                for(int c = 0; c < row.childCount; ++c)
                                        cursorY += RowHeight(row.pChildren[c].type) + ROW_GAP;
                        continue;
                }
                const float rh = RowHeight(row.type);
                if(r == m_DropdownRow && m_DropdownChild < 0)
                {
                        const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                        const float ddX = bodyX + rw - DROPDOWN_W - 2.0f;
                        const float ddY = cursorY + (rh - DROPDOWN_H) * 0.5f;
                        SRow &target = row;
                        // Popup background.
                        const float popupH = target.optionCount * (DROPDOWN_H + 2.0f) + 4.0f;
                        DrawRoundedRect(ddX, ddY + DROPDOWN_H + 2.0f, DROPDOWN_W, popupH,
                                3.0f, IGraphics::CORNER_ALL,
                                ColorRGBA(0xff0d0d18, true).WithAlpha(alpha));
                        // Option rows.
                        for(int o = 0; o < target.optionCount; ++o)
                        {
                                const float itemY = ddY + DROPDOWN_H + 4.0f + o * (DROPDOWN_H + 2.0f);
                                const bool hovered = PointInRect(m_MousePos, ddX, itemY, DROPDOWN_W, DROPDOWN_H);
                                if(hovered)
                                {
                                        DrawRoundedRect(ddX, itemY, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(0xff9d4edd, true).WithAlpha(alpha * 0.5f));
                                }
                                const ColorRGBA optCol = (o == target.valueIdx)
                                        ? ColorRGBA(0xffff9d4e, true).WithAlpha(alpha) // highlight selected
                                        : ColorRGBA(0xffd8d8e8, true).WithAlpha(alpha);
                                DrawText(ddX + 5.0f, itemY + 4.0f, 14.0f, target.pOptions[o], optCol);
                        }
                        return;
                }
                cursorY += rh + ROW_GAP;
                if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                const float crh = RowHeight(child.type);
                                if(r == m_DropdownRow && c == m_DropdownChild)
                                {
                                        const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                                        const float crw = rw - SUB_LIST_INDENT;
                                        const float ddX = bodyX + SUB_LIST_INDENT + crw - DROPDOWN_W - 2.0f;
                                        const float ddY = cursorY + (crh - DROPDOWN_H) * 0.5f;
                                        SRow &target = child;
                                        const float popupH = target.optionCount * (DROPDOWN_H + 2.0f) + 4.0f;
                                        DrawRoundedRect(ddX, ddY + DROPDOWN_H + 2.0f, DROPDOWN_W, popupH,
                                                3.0f, IGraphics::CORNER_ALL,
                                                ColorRGBA(0xff0d0d18, true).WithAlpha(alpha));
                                        for(int o = 0; o < target.optionCount; ++o)
                                        {
                                                const float itemY = ddY + DROPDOWN_H + 4.0f + o * (DROPDOWN_H + 2.0f);
                                                const bool hovered = PointInRect(m_MousePos, ddX, itemY, DROPDOWN_W, DROPDOWN_H);
                                                if(hovered)
                                                {
                                                        DrawRoundedRect(ddX, itemY, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                                                IGraphics::CORNER_ALL,
                                                                ColorRGBA(0xff9d4edd, true).WithAlpha(alpha * 0.5f));
                                                }
                                                const ColorRGBA optCol = (o == target.valueIdx)
                                                        ? ColorRGBA(0xffff9d4e, true).WithAlpha(alpha)
                                                        : ColorRGBA(0xffd8d8e8, true).WithAlpha(alpha);
                                                DrawText(ddX + 5.0f, itemY + 4.0f, 14.0f, target.pOptions[o], optCol);
                                        }
                                        return;
                                }
                                cursorY += crh + ROW_GAP;
                        }
                }
        }
}

void CClickGui::HandleTextInput(const IInput::CEvent &Event)
{
        SRow *pRow = (SRow *)GetEditingInputRowPtr();
        if(!pRow || pRow->type != ERowType::Input)
        {
                m_InputEditing = false; Input()->StopTextInput();
                return;
        }

        // Helper: commit edit buffer back to the row's mutable buffer.
        auto &&CommitToRow = [&]() {
                // Lazy-init if needed (defensive).
                if(!pRow->m_InputInitialized)
                {
                        if(pRow->pInputValue)
                                str_copy(pRow->m_aInputBuf, pRow->pInputValue, sizeof(pRow->m_aInputBuf));
                        pRow->m_InputInitialized = true;
                }
                mem_copy(pRow->m_aInputBuf, m_InputBuf, m_InputLen + 1);
        };

        // Helper: delete the current selection (if any). Returns true if deleted.
        auto &&DeleteSelection = [&]() {
                if(m_InputSelStart < 0 || m_InputSelEnd < 0 || m_InputSelStart >= m_InputSelEnd)
                        return false;
                const int delCount = m_InputSelEnd - m_InputSelStart;
                for(int i = m_InputSelStart; i + delCount < m_InputLen; ++i)
                        m_InputBuf[i] = m_InputBuf[i + delCount];
                m_InputLen -= delCount;
                m_InputBuf[m_InputLen] = 0;
                m_InputCursor = m_InputSelStart;
                m_InputSelStart = -1;
                m_InputSelEnd = -1;
                return true;
        };

        const bool shiftPressed = Input()->ShiftIsPressed();

        // Escape / Enter → commit and stop editing.
        if(Event.m_Flags & IInput::FLAG_PRESS)
        {
                if(Event.m_Key == KEY_ESCAPE)
                {
                        CommitToRow();
                        m_InputEditing = false; Input()->StopTextInput();
                        return;
                }
                if(Event.m_Key == KEY_RETURN)
                {
                        CommitToRow();
                        m_InputEditing = false; Input()->StopTextInput();
                        return;
                }
                if(Event.m_Key == KEY_BACKSPACE)
                {
                        if(DeleteSelection())
                                ;
                        else if(m_InputCursor > 0)
                        {
                                for(int i = m_InputCursor - 1; i < m_InputLen; ++i)
                                        m_InputBuf[i] = m_InputBuf[i + 1];
                                m_InputLen--;
                                m_InputBuf[m_InputLen] = 0;
                                m_InputCursor--;
                        }
                        return;
                }
                if(Event.m_Key == KEY_DELETE)
                {
                        if(DeleteSelection())
                                ;
                        else if(m_InputCursor < m_InputLen)
                        {
                                for(int i = m_InputCursor; i < m_InputLen; ++i)
                                        m_InputBuf[i] = m_InputBuf[i + 1];
                                m_InputLen--;
                                m_InputBuf[m_InputLen] = 0;
                        }
                        return;
                }
                if(Event.m_Key == KEY_LEFT)
                {
                        if(m_InputCursor > 0)
                        {
                                m_InputCursor--;
                                if(!shiftPressed)
                                {
                                        m_InputSelStart = -1;
                                        m_InputSelEnd = -1;
                                }
                                else
                                {
                                        // Extend selection.
                                        if(m_InputSelStart < 0)
                                        {
                                                m_InputSelStart = m_InputCursor + 1;
                                                m_InputSelEnd = m_InputCursor + 1;
                                        }
                                        m_InputSelStart = m_InputCursor;
                                }
                        }
                        return;
                }
                if(Event.m_Key == KEY_RIGHT)
                {
                        if(m_InputCursor < m_InputLen)
                        {
                                m_InputCursor++;
                                if(!shiftPressed)
                                {
                                        m_InputSelStart = -1;
                                        m_InputSelEnd = -1;
                                }
                                else
                                {
                                        if(m_InputSelEnd < 0)
                                        {
                                                m_InputSelStart = m_InputCursor - 1;
                                                m_InputSelEnd = m_InputCursor - 1;
                                        }
                                        m_InputSelEnd = m_InputCursor;
                                }
                        }
                        return;
                }
                if(Event.m_Key == KEY_HOME)
                {
                        m_InputCursor = 0;
                        if(!shiftPressed)
                        {
                                m_InputSelStart = -1;
                                m_InputSelEnd = -1;
                        }
                        return;
                }
                if(Event.m_Key == KEY_END)
                {
                        m_InputCursor = m_InputLen;
                        if(!shiftPressed)
                        {
                                m_InputSelStart = -1;
                                m_InputSelEnd = -1;
                        }
                        return;
                }
        }

        // Text input (FLAG_TEXT): insert at cursor, replacing selection.
        if(Event.m_Flags & IInput::FLAG_TEXT)
        {
                const char *pText = Event.m_aText;
                if(pText && pText[0])
                {
                        // Delete existing selection first.
                        DeleteSelection();
                        int textLen = (int)str_length(pText);
                        if(m_InputLen + textLen < INPUT_BUF_SIZE - 1)
                        {
                                // Shift chars after cursor right.
                                for(int i = m_InputLen; i >= m_InputCursor; --i)
                                        m_InputBuf[i + textLen] = m_InputBuf[i];
                                mem_copy(m_InputBuf + m_InputCursor, pText, textLen);
                                m_InputLen += textLen;
                                m_InputBuf[m_InputLen] = 0;
                                m_InputCursor += textLen;
                        }
                }
        }
}

bool CClickGui::HitTestRow(vec2 mousePos, int panelIdx, int &outRow, int &outChild, float &outRowX, float &outRowY, float &outRowW, float &outRowH)
{
        // Walk all rows (incl. expanded children) and check rect hits.
        // Uses the same geometry constants as RenderPanel for consistency.
        const vec2 ppos = m_aPanelPosValid[panelIdx] ? m_aPanelPos[panelIdx] : PanelHomePos(panelIdx);
        const float bodyX = ppos.x + BODY_PADDING;
        float cursorY = ppos.y + HEADER_HEIGHT + BODY_PADDING;

        for(int r = 0; r < g_Panels[panelIdx].rowCount; ++r)
        {
                SRow &row = g_Panels[panelIdx].pRows[r];
                // Conditional visibility: skip hidden rows in Functions panel.
                if(str_comp(g_Panels[panelIdx].pTitle, "Functions") == 0 && !FunctionsRowVisible(g_Panels[panelIdx].pRows, g_Panels[panelIdx].rowCount, r))
                {
                        cursorY += RowHeight(row.type) + ROW_GAP;
                        if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                                for(int c = 0; c < row.childCount; ++c)
                                        cursorY += RowHeight(row.pChildren[c].type) + ROW_GAP;
                        continue;
                }
                const float rh = RowHeight(row.type);
                const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                if(PointInRect(mousePos, bodyX, cursorY, rw, rh))
                {
                        outRow = r;
                        outChild = -1;
                        outRowX = bodyX;
                        outRowY = cursorY;
                        outRowW = rw;
                        outRowH = rh;
                        return true;
                }
                cursorY += rh + ROW_GAP;
                if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                const float crh = RowHeight(child.type);
                                const float crw = rw - SUB_LIST_INDENT;
                                if(PointInRect(mousePos, bodyX + SUB_LIST_INDENT, cursorY, crw, crh))
                                {
                                        outRow = r;
                                        outChild = c;
                                        outRowX = bodyX + SUB_LIST_INDENT;
                                        outRowY = cursorY;
                                        outRowW = crw;
                                        outRowH = crh;
                                        return true;
                                }
                                cursorY += crh + ROW_GAP;
                        }
                }
        }
        return false;
}

// ============================================================================
// CClickGui — RenderPanel (Yoga layout + IGraphics draw)
// ============================================================================

void CClickGui::RenderPanel(int panelIdx, vec2 pos, float alpha)
{
        const SPanel &panel = g_Panels[panelIdx];
        // v1.56.60: collapse is now animated — use the progress value instead of
        // the boolean. collapseAnim = 0 → fully expanded, 1 → fully collapsed.
        const float collapseAnim = m_aPanelCollapseAnim[panelIdx];
        const bool collapsed = m_aPanelCollapsed[panelIdx];

        // ---- Step 1: build a Yoga tree for the body and compute its FULL height ----
        // (Skipped only when fully collapsed AND animation finished — body height = 0.)
        // We always use the full expanded height from Yoga and then scale it by
        // (1 - collapseAnim) so the panel shrinks smoothly during collapse.
        YGNodeRef bodyRoot = nullptr;
        YGNodeRef rowNodes[64];
        int rowCount = 0;
        float bodyHeightFull = 0.0f;

        if(collapseAnim < 1.0f || !collapsed)
        {
                bodyRoot = YGNodeNew();
                YGNodeStyleSetWidth(bodyRoot, PANEL_WIDTH);
                YGNodeStyleSetPadding(bodyRoot, YGEdgeAll, BODY_PADDING);
                YGNodeStyleSetFlexDirection(bodyRoot, YGFlexDirectionColumn);
                YGNodeStyleSetGap(bodyRoot, YGGutterAll, ROW_GAP);
                YGNodeStyleSetOverflow(bodyRoot, YGOverflowHidden);

                constexpr int MAX_ROWS = 32;
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
                        SRow &row = panel.pRows[r];
                        // Conditional visibility: skip hidden rows in Functions panel.
                        if(str_comp(panel.pTitle, "Functions") == 0 && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                                continue;
                        addRowNode(row, false);
                        // v1.56.60: include children in layout while expand animation
                        // is playing (m_AnimExpand > 0) even if just collapsed, so the
                        // Yoga tree keeps the full height during the slide-out.
                        if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                                continue;
                                        addRowNode(row.pChildren[c], true);
                                }
                        }
                }

                YGNodeCalculateLayout(bodyRoot, PANEL_WIDTH, YGUndefined, YGDirectionLTR);
                bodyHeightFull = YGNodeLayoutGetHeight(bodyRoot);
        }

        // v1.56.61: scale body height by the EASED visibility (not raw collapseAnim)
        // so the panel slide has finish-easing in both expand and collapse directions.
        // visibility raw = 1 - collapseAnim (0 = hidden, 1 = shown).
        // target = !collapsed ? 1 : 0 (1 = expanding toward visible, 0 = collapsing toward hidden).
        // EaseBothDirections applies finish-easing (fast-start, slow-end) regardless of direction.
        const float visibilityRaw = 1.0f - collapseAnim;
        const float visibilityTarget = collapsed ? 0.0f : 1.0f;
        const float easedVisibility = EaseBothDirections(visibilityRaw, visibilityTarget);
        const float bodyHeight = bodyHeightFull * easedVisibility;

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

        // ▼/▶ collapse arrow (left side of header) — ▼ when expanded, ▶ when collapsed.
        {
                const char *pArrowStr = collapsed ? "\xe2\x96\xb6" /* ▶ */ : "\xe2\x96\xbc" /* ▼ */;
                DrawText(px + 10.0f, py + 9.0f, 18.0f, pArrowStr,
                        ColorRGBA(COL_ARROW, true).WithAlpha(alpha));
        }

        // Title centered.
        {
                const float titleSize = 18.0f;
                DrawText(px + PANEL_WIDTH * 0.5f, py + 9.0f, titleSize,
                        panel.pTitle, ColorRGBA(COL_HEADER_TEXT, true).WithAlpha(alpha),
                        TEXTALIGN_CENTER);
        }

        // ---- Step 4: body (drawn while bodyHeight > 0, i.e. during expand/collapse anim) ----
        // v1.56.60: was `if(!collapsed)` (binary). Now uses the animated bodyHeight so
        // the body stays visible — and clipped — while the collapse slide plays out.
        if(bodyHeight > 0.5f)
        {
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
                SRow &row = panel.pRows[r];
                // Conditional visibility: skip hidden rows in Functions panel.
                if(str_comp(panel.pTitle, "Functions") == 0 && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
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
                        DrawRoundedRect(rx + 1.0f, ry + 5.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 5.0f + 3.0f, 18.0f, "\xe2\x9c\x93" /* ✓ */,
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 18.0f, row.pName, textCol);
                        break;
                }
                case ERowType::ToggleDropdown:
                {
                        // [✓ or empty] [name] [select box]
                        DrawRoundedRect(rx + 1.0f, ry + 5.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 5.0f + 3.0f, 18.0f, "\xe2\x9c\x93",
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 18.0f, row.pName, textCol);

                        // Dropdown box on the right.
                        const float ddX = rx + rw - DROPDOWN_W - 2.0f;
                        const float ddY = ry + (rh - DROPDOWN_H) * 0.5f;
                        DrawRoundedRect(ddX, ddY, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        const char *pVal = (row.optionCount > 0 && row.valueIdx >= 0 && row.valueIdx < row.optionCount)
                                ? row.pOptions[row.valueIdx]
                                : "";
                        DrawText(ddX + 4.0f, ddY + 4.0f, 17.0f, pVal, textCol);
                        break;
                }
                case ERowType::Slider:
                {
                        // Top: [name]              [value]
                        // Bottom: [track with thumb at value%]
                        DrawText(rx + 2.0f, ry + 4.0f, 17.0f, row.pName, textCol);

                        // Value chip.
                        char valBuf[32];
                        // Use integer format when step >= 1.0 (no fractional values needed).
                        if(row.step >= 1.0f)
                        {
                                if(row.pSuffix)
                                        str_format(valBuf, sizeof(valBuf), "%d%s", (int)row.value, row.pSuffix);
                                else
                                        str_format(valBuf, sizeof(valBuf), "%d", (int)row.value);
                        }
                        else
                        {
                                if(row.pSuffix)
                                        str_format(valBuf, sizeof(valBuf), "%.2f%s", row.value, row.pSuffix);
                                else
                                        str_format(valBuf, sizeof(valBuf), "%.2f", row.value);
                        }
                        const float valW = TextRender()->TextWidth(17.0f, valBuf) + 10.0f;
                        const float valX = rx + rw - valW - 2.0f;
                        const float valY = ry + 1.0f;
                        DrawRoundedRect(valX, valY, valW, 18.0f, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(alpha));
                        DrawText(valX + 6.0f, valY + 3.0f, 17.0f, valBuf, textCol);

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
                        const float nameW = std::min(88.0f, TextRender()->TextWidth(17.0f, row.pName) + 4.0f);
                        DrawText(rx + 2.0f, ry + 5.0f, 17.0f, row.pName, textCol);
                        const float inX = rx + nameW + 2.0f;
                        const float inY = ry + 2.0f;
                        const float inW = rw - nameW - 4.0f;
                        const float inH = rh - 4.0f;
                        // Highlight border if this input is being edited.
                        const bool editingThis = (m_InputEditing && m_InputPanel == panelIdx && m_InputRow == r && m_InputChild < 0);
                        DrawRoundedRect(inX, inY, inW, inH, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        DrawRoundedRect(inX - 1.0f, inY - 1.0f, inW + 2.0f, inH + 2.0f, 2.0f,
                                IGraphics::CORNER_ALL,
                                editingThis ? ColorRGBA(0xff9d4edd, true).WithAlpha(alpha) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
                        // Lazy-init the row's mutable buffer.
                        if(!row.m_InputInitialized)
                        {
                                if(row.pInputValue)
                                        str_copy(row.m_aInputBuf, row.pInputValue, sizeof(row.m_aInputBuf));
                                row.m_InputInitialized = true;
                        }
                        // Show edit buffer if editing, else stored mutable buffer / placeholder.
                        const char *pText;
                        ColorRGBA inCol;
                        if(editingThis)
                        {
                                pText = m_InputBuf;
                                inCol = textCol;
                        }
                        else
                        {
                                pText = (row.m_aInputBuf[0]) ? row.m_aInputBuf : row.pPlaceholder;
                                inCol = (row.m_aInputBuf[0])
                                        ? textCol
                                        : ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha);
                        }
                        // Draw selection highlight (if editing and selection active).
                        if(editingThis && m_InputSelStart >= 0 && m_InputSelEnd > m_InputSelStart)
                        {
                                // Measure width up to selStart and selEnd.
                                char tmp[256];
                                int n = std::min(m_InputSelStart, (int)sizeof(tmp) - 1);
                                mem_copy(tmp, m_InputBuf, n); tmp[n] = 0;
                                float selStartX = inX + 4.0f + TextRender()->TextWidth(17.0f, tmp);
                                n = std::min(m_InputSelEnd, (int)sizeof(tmp) - 1);
                                mem_copy(tmp, m_InputBuf, n); tmp[n] = 0;
                                float selEndX = inX + 4.0f + TextRender()->TextWidth(17.0f, tmp);
                                DrawRoundedRect(selStartX, inY + 3.0f, selEndX - selStartX, inH - 6.0f, 1.0f,
                                        IGraphics::CORNER_NONE,
                                        ColorRGBA(0xff9d4edd, true).WithAlpha(alpha * 0.5f));
                        }
                        if(pText)
                                DrawText(inX + 4.0f, inY + 4.0f, 17.0f, pText, inCol);
                        // Cursor at m_InputCursor position while editing.
                        if(editingThis)
                        {
                                char tmp[256];
                                int n = std::min(m_InputCursor, (int)sizeof(tmp) - 1);
                                mem_copy(tmp, m_InputBuf, n); tmp[n] = 0;
                                const float cursorX = inX + 4.0f + TextRender()->TextWidth(17.0f, tmp);
                                DrawRoundedRect(cursorX, inY + 4.0f, 1.5f, inH - 8.0f, 0.0f,
                                        IGraphics::CORNER_NONE,
                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha * (0.5f + 0.5f * std::sin(LocalTime() * 5.0f))));
                        }
                        break;
                }
                case ERowType::Dropdown:
                {
                        // [name] [select box]
                        DrawText(rx + 2.0f, ry + 5.0f, 18.0f, row.pName, textCol);
                        const float ddX = rx + rw - DROPDOWN_W - 2.0f;
                        const float ddY = ry + (rh - DROPDOWN_H) * 0.5f;
                        DrawRoundedRect(ddX, ddY, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        const char *pVal = (row.optionCount > 0 && row.valueIdx >= 0 && row.valueIdx < row.optionCount)
                                ? row.pOptions[row.valueIdx]
                                : "";
                        DrawText(ddX + 4.0f, ddY + 4.0f, 17.0f, pVal, textCol);
                        break;
                }
                case ERowType::Expandable:
                {
                        // [✓ or empty] [name] [▶ or ▼]
                        DrawRoundedRect(rx + 1.0f, ry + 5.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 5.0f + 3.0f, 18.0f, "\xe2\x9c\x93",
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 18.0f, row.pName,
                                row.on ? ColorRGBA(COL_ROW_TEXT_ON, true).WithAlpha(alpha) : textCol);
                        // Arrow on the right (▶ collapsed, ▼ expanded).
                        const char *pArrow = row.expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6";
                        const ColorRGBA arrowCol = row.expanded
                                ? ColorRGBA(0xffb8b8e8, true).WithAlpha(alpha)
                                : ColorRGBA(COL_EXPAND_ARROW, true).WithAlpha(alpha);
                        DrawText(rx + rw - ARROW_W - 2.0f, ry + 5.0f, 14.0f, pArrow, arrowCol);
                        break;
                }
                case ERowType::Button:
                {
                        // Button — colored rect with centered label.
                        // Pathfinding button: green (IDLE) / red (RUNNING) / orange (FINISHED).
                        ColorRGBA btnCol = ColorRGBA(0xff9d4edd, true).WithAlpha(alpha); // default purple
                        const char *pLabel = row.pName;
                        if(str_comp(row.pName, "Pathfinding") == 0)
                        {
                                CBotNet &BN = GameClient()->m_BotNet;
                                if(BN.m_PfState == PF_STATE_IDLE)
                                {
                                        btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green
                                        pLabel = "Pathfinding";
                                }
                                else if(BN.m_PfState == PF_STATE_RUNNING)
                                {
                                        btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha); // red
                                        pLabel = "Stop";
                                }
                                else // FINISHED
                                {
                                        btnCol = ColorRGBA(0xffe69528, true).WithAlpha(alpha); // orange
                                        pLabel = "Finish";
                                }
                        }
                        // Button background (full row width, centered).
                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                IGraphics::CORNER_ALL, btnCol);
                        // Centered label.
                        const float labelW = TextRender()->TextWidth(16.0f, pLabel);
                        DrawText(rx + (rw - labelW) * 0.5f, ry + 8.0f, 16.0f, pLabel,
                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                        break;
                }
                }

                // Render expanded children (already added as rowNodes after the parent).
                // v1.56.60: render while m_AnimExpand > 0 (covers both expand and
                // collapse animations). Children slide up/down + fade with CubicBezierEase.
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                {
                        // Eased expand progress (0 = fully collapsed, 1 = fully expanded).
                        // v1.56.61: EaseBothDirections for finish-easing in both expand and collapse.
                        const float expandProgress = EaseBothDirections(row.m_AnimExpand, row.expanded ? 1.0f : 0.0f);
                        // Slide offset: children enter from above (negative Y) when expanding,
                        // exit upward when collapsing. Magnitude = a fraction of one row height
                        // so the motion is visible but never overshoots the parent row.
                        const float slideOffset = -(1.0f - expandProgress) * (rh + ROW_GAP);
                        // Child alpha fades in/out with the same progress.
                        const float childAlpha = alpha * expandProgress;

                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                const YGNodeRef cnode = rowNodes[rowIdx];
                                const float crx = bodyX + YGNodeLayoutGetLeft(cnode);
                                const float cry = bodyY + YGNodeLayoutGetTop(cnode) + slideOffset;
                                const float crw = YGNodeLayoutGetWidth(cnode);
                                const float crh = YGNodeLayoutGetHeight(cnode);
                                ++rowIdx;

                                const ColorRGBA cText = child.on
                                        ? ColorRGBA(COL_ROW_TEXT_ON, true).WithAlpha(childAlpha)
                                        : ColorRGBA(0xffb0b0d0, true).WithAlpha(childAlpha);

                                // Faint sub-list background band under each child.
                                DrawRoundedRect(crx - 2.0f, cry - 1.0f, crw + 4.0f, crh + 1.0f,
                                        0.0f, IGraphics::CORNER_NONE,
                                        ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(childAlpha * 0.6f));

                                switch(child.type)
                                {
                                case ERowType::Toggle:
                                        DrawRoundedRect(crx + 1.0f, cry + 5.0f, CHECK_W, CHECK_W, 3.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(childAlpha));
                                        if(child.on)
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 5.0f + 3.0f, 17.0f, "\xe2\x9c\x93",
                                                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(childAlpha), TEXTALIGN_CENTER);
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        break;
                                case ERowType::Slider:
                                {
                                        DrawText(crx + 2.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        char vbuf[32];
                                        if(child.step >= 1.0f)
                                        {
                                                if(child.pSuffix)
                                                        str_format(vbuf, sizeof(vbuf), "%d%s", (int)child.value, child.pSuffix);
                                                else
                                                        str_format(vbuf, sizeof(vbuf), "%d", (int)child.value);
                                        }
                                        else
                                        {
                                                if(child.pSuffix)
                                                        str_format(vbuf, sizeof(vbuf), "%.2f%s", child.value, child.pSuffix);
                                                else
                                                        str_format(vbuf, sizeof(vbuf), "%.2f", child.value);
                                        }
                                        const float vw = TextRender()->TextWidth(17.0f, vbuf) + 10.0f;
                                        const float vx = crx + crw - vw - 2.0f;
                                        DrawRoundedRect(vx, cry + 1.0f, vw, 18.0f, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(childAlpha));
                                        DrawText(vx + 6.0f, cry + 2.0f, 17.0f, vbuf, cText);
                                        const float trx = crx + 2.0f;
                                        const float trY = cry + crh - SLIDER_TRACK_H - 4.0f;
                                        const float trW = crw - 4.0f;
                                        DrawRoundedRect(trx, trY, trW, SLIDER_TRACK_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_SLIDER_TRACK, true).WithAlpha(childAlpha));
                                        float cpct = 0.0f;
                                        if(child.max > child.min)
                                                cpct = (child.value - child.min) / (child.max - child.min);
                                        cpct = std::clamp(cpct, 0.0f, 1.0f);
                                        const float thX = trx + trW * cpct;
                                        const float thY = trY + SLIDER_TRACK_H * 0.5f;
                                        Graphics()->TextureClear();
                                        Graphics()->QuadsBegin();
                                        Graphics()->SetColor(ColorRGBA(COL_SLIDER_THUMB, true).WithAlpha(childAlpha));
                                        Graphics()->DrawCircle(thX, thY, SLIDER_THUMB_R, 12);
                                        Graphics()->QuadsEnd();
                                        break;
                                }
                                case ERowType::Input:
                                {
                                        const float nw = std::min(88.0f, TextRender()->TextWidth(17.0f, child.pName) + 4.0f);
                                        DrawText(crx + 2.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        const float ix = crx + nw + 2.0f;
                                        const float iy = cry + 2.0f;
                                        const float iw = crw - nw - 4.0f;
                                        const float ih = crh - 4.0f;
                                        // Lazy-init child's mutable buffer.
                                        if(!child.m_InputInitialized)
                                        {
                                                if(child.pInputValue)
                                                        str_copy(child.m_aInputBuf, child.pInputValue, sizeof(child.m_aInputBuf));
                                                child.m_InputInitialized = true;
                                        }
                                        const bool editingChild = (m_InputEditing && m_InputPanel == panelIdx && m_InputRow == r && m_InputChild == c);
                                        DrawRoundedRect(ix, iy, iw, ih, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(childAlpha));
                                        DrawRoundedRect(ix - 1.0f, iy - 1.0f, iw + 2.0f, ih + 2.0f, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                editingChild ? ColorRGBA(0xff9d4edd, true).WithAlpha(childAlpha) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
                                        const char *pT;
                                        ColorRGBA ic;
                                        if(editingChild)
                                        {
                                                pT = m_InputBuf;
                                                ic = cText;
                                        }
                                        else
                                        {
                                                pT = (child.m_aInputBuf[0]) ? child.m_aInputBuf : child.pPlaceholder;
                                                ic = (child.m_aInputBuf[0])
                                                        ? cText
                                                        : ColorRGBA(0xff8a8aa8, true).WithAlpha(childAlpha);
                                        }
                                        // Selection highlight.
                                        if(editingChild && m_InputSelStart >= 0 && m_InputSelEnd > m_InputSelStart)
                                        {
                                                char tmp[256];
                                                int n = std::min(m_InputSelStart, (int)sizeof(tmp) - 1);
                                                mem_copy(tmp, m_InputBuf, n); tmp[n] = 0;
                                                float selStartX = ix + 4.0f + TextRender()->TextWidth(17.0f, tmp);
                                                n = std::min(m_InputSelEnd, (int)sizeof(tmp) - 1);
                                                mem_copy(tmp, m_InputBuf, n); tmp[n] = 0;
                                                float selEndX = ix + 4.0f + TextRender()->TextWidth(17.0f, tmp);
                                                DrawRoundedRect(selStartX, iy + 3.0f, selEndX - selStartX, ih - 6.0f, 1.0f,
                                                        IGraphics::CORNER_NONE,
                                                        ColorRGBA(0xff9d4edd, true).WithAlpha(alpha * 0.5f));
                                        }
                                        if(pT)
                                                DrawText(ix + 4.0f, iy + 3.0f, 17.0f, pT, ic);
                                        // Cursor.
                                        if(editingChild)
                                        {
                                                char tmp[256];
                                                int n = std::min(m_InputCursor, (int)sizeof(tmp) - 1);
                                                mem_copy(tmp, m_InputBuf, n); tmp[n] = 0;
                                                const float cursorX = ix + 4.0f + TextRender()->TextWidth(17.0f, tmp);
                                                DrawRoundedRect(cursorX, iy + 3.0f, 1.5f, ih - 6.0f, 0.0f,
                                                        IGraphics::CORNER_NONE,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha * (0.5f + 0.5f * std::sin(LocalTime() * 5.0f))));
                                        }
                                        break;
                                }
                                case ERowType::Dropdown:
                                case ERowType::ToggleDropdown:
                                {
                                        // v1.56.60: vector checkbox with draw animation (only ToggleDropdown has a check).
                                        if(child.type == ERowType::ToggleDropdown)
                                                DrawRoundedRect(crx + 1.0f, cry + 5.0f, CHECK_W, CHECK_W, 3.0f,
                                                        IGraphics::CORNER_ALL,
                                                        ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(childAlpha));
                                                if(child.on)
                                                        DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 5.0f + 3.0f, 17.0f, "\xe2\x9c\x93",
                                                                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(childAlpha), TEXTALIGN_CENTER);
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        const float ddx = crx + crw - DROPDOWN_W - 2.0f;
                                        const float ddy = cry + (crh - DROPDOWN_H) * 0.5f;
                                        DrawRoundedRect(ddx, ddy, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(childAlpha));
                                        const char *pV = (child.optionCount > 0 && child.valueIdx >= 0 && child.valueIdx < child.optionCount)
                                                ? child.pOptions[child.valueIdx]
                                                : "";
                                        DrawText(ddx + 4.0f, ddy + 3.0f, 17.0f, pV, cText);
                                        break;
                                }
                                case ERowType::Expandable:
                                        // Nested expandables not in the data model; render as a plain toggle.
                                        DrawRoundedRect(crx + 1.0f, cry + 5.0f, CHECK_W, CHECK_W, 3.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(childAlpha));
                                        if(child.on)
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 5.0f + 3.0f, 17.0f, "\xe2\x9c\x93",
                                                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(childAlpha), TEXTALIGN_CENTER);
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        break;
                                }
                        }
                }
        }

                Graphics()->ClipDisable();
        } // end if(!collapsed)

        // Free the Yoga tree (root + children are freed by YGNodeFreeRecursive).
        // Safe to call with nullptr (collapsed case) — YGNodeFreeRecursive handles it.
        if(bodyRoot)
                YGNodeFreeRecursive(bodyRoot);
}
