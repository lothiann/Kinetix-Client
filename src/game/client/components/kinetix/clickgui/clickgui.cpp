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
// v1.56.83: AimBot/TriggerBot dropdown options (unique prefix AB/TB to avoid collisions).
static const char *const g_ABWeaponOpts[] = {"Hammer", "Pistol", "Shotgun", "Grenade", "Laser", "Ninja", "Hook"};
static const char *const g_ABWeaponOptsNoHook[] = {"Hammer", "Pistol", "Shotgun", "Grenade", "Laser", "Ninja"};
static const char *const g_ABTriggerOpts[] = {"Fire", "Hook"};
static const char *const g_ABTriggerModeOpts[] = {"One tick", "Hold", "Every tick"};
static const char *const g_ABAimModeOpts[] = {"Silent", "Trigger", "Always"};
static const char *const g_ABTeamOpts[] = {"Both", "War", "My"};
static const char *const g_ABFriendOpts[] = {"Both", "Ignore"};
static const char *const g_ABDummyOpts[] = {"Both", "Ignore"};
static const char *const g_ABFreezeOpts[] = {"Both", "Freeze", "No freeze"};
static const char *const g_ABPriorityOpts[] = {"Nearest FOV", "Nearest Aim", "Nearest to me"};
// v1.56.83: AimBot expandable — 15 children.
static SRow g_AimBotChildren[] = {
        {ERowType::Dropdown, "AB Weapon", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABWeaponOpts, 7, 0, nullptr, 0},
        {ERowType::Toggle, "AB Enable", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "AB FOV", false, false, 1.0f, 360.0f, 1.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "AB Radius", false, false, 1.0f, 2000.0f, 1.0f, 300.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show FOV", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show Radius", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Use raycast (show)", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "AB Raycast (aim)", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "AB Angles", false, false, 1.0f, 64.0f, 1.0f, 16.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Aim Mode", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABAimModeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Team", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTeamOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Friend", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 2, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Dummy", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 2, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Freeze", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Priority", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABPriorityOpts, 3, 0, nullptr, 0},
};
// v1.56.86: TriggerBot expandable — 17 children (no Hook, has Trigger + Trigger mode + Latency).
static SRow g_TriggerBotChildren[] = {
        {ERowType::Dropdown, "TB Weapon", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABWeaponOptsNoHook, 6, 0, nullptr, 0},
        {ERowType::Toggle, "TB Enable", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Trigger", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTriggerOpts, 2, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Trigger mode", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTriggerModeOpts, 3, 0, nullptr, 0},
        {ERowType::Slider, "TB FOV", false, false, 1.0f, 360.0f, 1.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "TB Radius", false, false, 1.0f, 2000.0f, 1.0f, 300.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Show FOV", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Show Radius", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Use raycast (show)", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Raycast (trigger)", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "TB Angles", false, false, 1.0f, 64.0f, 1.0f, 16.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "TB Latency", false, false, 0.0f, 10.0f, 1.0f, 0.0f, " ticks", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Team", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTeamOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Friend", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 2, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Dummy", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 2, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Freeze", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Priority", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABPriorityOpts, 3, 0, nullptr, 0},
};
static SRow g_FunctionsRows[] = {
        {ERowType::Toggle, "Block", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Random Aim", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // v1.56.83: AimBot + TriggerBot expandables
        {ERowType::Expandable, "AimBot", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AimBotChildren, 15},
        {ERowType::Expandable, "TriggerBot", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TriggerBotChildren, 17},
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
// v1.56.89: "Trajectory" expandable: toggle + Prediction Ticks slider.
static SRow g_TrajectoryChildren[] = {
        {ERowType::Slider, "Prediction Ticks", false, false, 1.0f, 50.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_AdvancedRows[] = {
        {ERowType::Expandable, "Trajectory", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TrajectoryChildren, 1},
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
// v1.56.77: "Rainbow color" expandable: toggle + speed slider.
static SRow g_RainbowChildren[] = {
        {ERowType::Slider, "Speed", false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.89: "Line rendering" visual expandable (cannot toggle, only expand).
// Shared line render settings used by trajectory, aimbot FOV/radius, pathfinder path, laser unfreeze.
static SRow g_LineRenderingChildren[] = {
        {ERowType::Slider, "Line Margin", false, false, 0.0f, 20.0f, 1.0f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Line color", false, false, 0, 0, 0, 0, nullptr, "ffbf00", "hex...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Line opacity", false, false, 0.0f, 100.0f, 1.0f, 80.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_SettingsRows[] = {
        {ERowType::Input, "ClickGUI Color", false, false, 0, 0, 0, 0, nullptr, "9d4edd", "hex...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Rainbow color", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_RainbowChildren, 1},
        {ERowType::Expandable, "Line rendering", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_LineRenderingChildren, 3},
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
                        {
                                const bool wasInLayout = (row.m_AnimExpand > 0.0f);
                                lerpToward(row.m_AnimExpand, row.expanded ? 1.0f : 0.0f, expandSpeed);
                                // v1.56.72: invalidate Yoga cache when row crosses the
                                // 0 boundary (children added/removed from layout).
                                if(wasInLayout != (row.m_AnimExpand > 0.0f))
                                        m_aLayoutCacheDirty[p] = true;
                        }

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
                m_aPanelScroll[i] = 0.0f;
                m_aPanelScrollStored[i] = 0.0f;
                m_aZOrder[i] = i;
                // v1.56.72: free + invalidate Yoga layout cache on reset.
                if(m_aLayoutCache[i].pRoot)
                        YGNodeFreeRecursive(m_aLayoutCache[i].pRoot);
                m_aLayoutCache[i].pRoot = nullptr;
                m_aLayoutCache[i].rowCount = 0;
                m_aLayoutCache[i].signature = 0;
                m_aLayoutCacheDirty[i] = true;
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
                // (So if c_attack / c_random_aim / kx_show_trajectory were enabled
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
                                        row.on = g_Config.m_KxShowTrajectory != 0;
                                // Advanced → Laser unfreeze expandable is master toggle.
                                else if(str_comp(row.pName, "Laser unfreeze") == 0)
                                        row.on = g_Config.m_KxLaserUnfreeze != 0;
                                else if(str_comp(row.pName, "Fake Aim") == 0)
                                        row.on = g_Config.m_KxFakeAim != 0;
                                // Functions panel — Fly/Copy Moves sub-section
                                else if(str_comp(row.pName, "Preserve input") == 0)
                                        row.on = g_Config.m_KxDummyHammerKeepInputs != 0;
                                else if(str_comp(row.pName, "Custom latency") == 0)
                                        row.on = g_Config.m_KxDummyHammerCustomLatency != 0;
                                else if(str_comp(row.pName, "Auto fly") == 0)
                                        row.on = g_Config.m_KxDummyHammerAuto != 0;
                                else if(str_comp(row.pName, "Mirror direction") == 0)
                                        row.on = g_Config.m_KxDummyCopyMirrorDir != 0;
                                else if(str_comp(row.pName, "Aim mode") == 0)
                                        row.valueIdx = g_Config.m_KxDummyCopyAimMode;
                                else if(str_comp(row.pName, "Mirror aim X") == 0)
                                        row.on = g_Config.m_KxDummyCopyMirrorAimX != 0;
                                else if(str_comp(row.pName, "Mirror aim Y") == 0)
                                        row.on = g_Config.m_KxDummyCopyMirrorAimY != 0;
                                else if(str_comp(row.pName, "Smart aim") == 0)
                                        row.on = g_Config.m_KxDummyCopySmartAim != 0;
                                // Functions → Copy Moves Filter expandable is master toggle.
                                else if(str_comp(row.pName, "Copy Moves Filter") == 0)
                                        row.on = g_Config.m_KxDummyCopyMovesFilter != 0;
                                // Pathfinder top-level sliders/toggles
                                else if(str_comp(row.pName, "Chunk size") == 0)
                                        row.value = (float)g_Config.m_KxPfChunkSize;
                                else if(str_comp(row.pName, "Hook angles") == 0)
                                        row.value = (float)g_Config.m_KxPfHookAngles;
                                else if(str_comp(row.pName, "Candidates") == 0)
                                        row.value = (float)g_Config.m_KxPfCandidates;
                                else if(str_comp(row.pName, "Performance") == 0)
                                        row.value = (float)g_Config.m_KxPfPerf;
                                else if(str_comp(row.pName, "Advanced search") == 0)
                                        row.on = g_Config.m_KxPfAdvancedSearch != 0;
                                // Settings → IRC/Spoofer top-level expandables are master toggles.
                                else if(str_comp(row.pName, "IRC") == 0)
                                        row.on = g_Config.m_KxIrcEnabled != 0;
                                else if(str_comp(row.pName, "Spoofer") == 0)
                                        row.on = (g_Config.m_KxVersionSpoof != 0 || g_Config.m_KxSpoofClientStr != 0);
                                // Sync children of expandable rows
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                        if(str_comp(child.pName, "Latency (ms)") == 0)
                                                child.value = (float)g_Config.m_KxDummyHammerLatencyMs;
                                        else if(str_comp(child.pName, "Filter: Jump") == 0)
                                                child.on = g_Config.m_KxDummyCopyMovesFilterJump != 0;
                                        else if(str_comp(child.pName, "Filter: Direction") == 0)
                                                child.on = g_Config.m_KxDummyCopyMovesFilterDirection != 0;
                                        else if(str_comp(child.pName, "Filter: Hook") == 0)
                                                child.on = g_Config.m_KxDummyCopyMovesFilterHook != 0;
                                        else if(str_comp(child.pName, "Filter: Aim") == 0)
                                                child.on = g_Config.m_KxDummyCopyMovesFilterAim != 0;
                                        else if(str_comp(child.pName, "Filter: Fire") == 0)
                                                child.on = g_Config.m_KxDummyCopyMovesFilterFire != 0;
                                        // Settings → IRC children
                                        else if(str_comp(child.pName, "Reveal join") == 0)
                                                child.on = g_Config.m_KxIrcRevealJoin != 0;
                                        else if(str_comp(child.pName, "Nameplate tag") == 0)
                                                child.on = g_Config.m_KxIrcNameplateTag != 0;
                                        // Settings → Line rendering children
                                        else if(str_comp(child.pName, "Line Margin") == 0)
                                                child.value = (float)g_Config.m_KxLineRenderingSize;
                                        else if(str_comp(child.pName, "Line color") == 0 && str_comp(row.pName, "Line rendering") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r)
                                                {
                                                        char hex[8];
                                                        str_format(hex, sizeof(hex), "%06x", g_Config.m_KxLineRenderingColor & 0xffffff);
                                                        str_copy(child.m_aInputBuf, hex, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Line opacity") == 0)
                                                child.value = (float)g_Config.m_KxLineRenderingAlpha;
                                        // Advanced → Trajectory children
                                        else if(str_comp(child.pName, "Prediction Ticks") == 0)
                                                child.value = (float)g_Config.m_KxTrajectoryTicks;
                                        // Advanced → Laser unfreeze children
                                        else if(str_comp(child.pName, "Auto laser") == 0)
                                                child.on = g_Config.m_KxLaserUnfreezeAuto != 0;
                                        else if(str_comp(child.pName, "FOV") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeFov;
                                        else if(str_comp(child.pName, "Angles") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeAngles;
                                        else if(str_comp(child.pName, "Ticks") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeTicks;
                                        else if(str_comp(child.pName, "Trigger ticks") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeTriggerTicks;
                                        else if(str_comp(child.pName, "Silent") == 0)
                                                child.on = g_Config.m_KxLaserUnfreezeSilent != 0;
                                        else if(str_comp(child.pName, "Show attempt") == 0)
                                                child.on = g_Config.m_KxLaserUnfreezeShowAttempt != 0;
                                        // Fake Aim children
                                        else if(str_comp(child.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.valueIdx = g_Config.m_KxFakeAimMode;
                                        else if(str_comp(child.pName, "Speed") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.value = (float)g_Config.m_KxFakeAimSpeed;
                                        else if(str_comp(child.pName, "Show for me") == 0)
                                                child.on = g_Config.m_KxFakeAimShowForMe != 0;
                                        // Settings → Spoofer children
                                        else if(str_comp(child.pName, "Version ID") == 0)
                                                child.value = (float)g_Config.m_KxVersionSpoofId;
                                        else if(str_comp(child.pName, "Client preset") == 0)
                                                child.valueIdx = g_Config.m_KxSpoofClientPreset;
                                        // Pathfinder → Score method children
                                        else if(str_comp(child.pName, "Distance reduction") == 0)
                                                child.on = g_Config.m_KxPfScoreDist != 0;
                                        else if(str_comp(child.pName, "Flow-field alignment") == 0)
                                                child.on = g_Config.m_KxPfScoreFlow != 0;
                                        // Pathfinder → Penalties children
                                        else if(str_comp(child.pName, "Freeze penalty") == 0)
                                                child.on = g_Config.m_KxPfFineFreeze != 0;
                                        else if(str_comp(child.pName, "Hook retract penalty") == 0)
                                                child.on = g_Config.m_KxPfFineRetract != 0;
                                        else if(str_comp(child.pName, "Death penalty") == 0)
                                                child.on = g_Config.m_KxPfFineDeath != 0;
                                        else if(str_comp(child.pName, "Wall-hit penalty") == 0)
                                                child.on = g_Config.m_KxPfFineWall != 0;
                                        else if(str_comp(child.pName, "Air-time penalty") == 0)
                                                child.on = g_Config.m_KxPfFineAir != 0;
                                        else if(str_comp(child.pName, "Hook-fire penalty") == 0)
                                                child.on = g_Config.m_KxPfFineHook != 0;
                                        else if(str_comp(child.pName, "Stuck penalty") == 0)
                                                child.on = g_Config.m_KxPfFineStuck != 0;
                                        // Pathfinder → Visuals children
                                        else if(str_comp(child.pName, "Show field") == 0)
                                                child.on = g_Config.m_KxPfShowField != 0;
                                        else if(str_comp(child.pName, "Show hooks") == 0)
                                                child.on = g_Config.m_KxPfShowHooks != 0;
                                        else if(str_comp(child.pName, "Show branches") == 0)
                                                child.on = g_Config.m_KxPfShowBranches != 0;
                                        else if(str_comp(child.pName, "Show speed") == 0)
                                                child.on = g_Config.m_KxPfShowSpeed != 0;
                                }
                        }
                }
        }
        // v1.56.83: per-frame sync of AimBot/TriggerBot UI from real state.
        // Runs every frame while open (not just on open) so switching the Weapon dropdown
        // immediately updates all per-weapon fields (Enable/FOV/Radius/filters/etc.).
        if(isOpen)
        {
                CAimBot &aimbot = GameClient()->m_AimBot;
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(row.type != ERowType::Expandable)
                                continue;
                                if(str_comp(row.pName, "AimBot") == 0)
                                {
                                        row.on = aimbot.m_AimBotEnabled;
                                        int w = aimbot.m_AimBotSelectedWeapon;
                                        if(w < 0 || w >= CAimBot::NUM_WEAPONS_AIM)
                                                w = 0;
                                        const auto &s = aimbot.m_AimBotWeapons[w];
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "AB Weapon") == 0)
                                                        ch.valueIdx = w;
                                                else if(str_comp(ch.pName, "AB Enable") == 0)
                                                        ch.on = s.m_Enabled;
                                                else if(str_comp(ch.pName, "AB FOV") == 0)
                                                        ch.value = s.m_Fov;
                                                else if(str_comp(ch.pName, "AB Radius") == 0)
                                                {
                                                        ch.max = aimbot.GetWeaponRadius(w);
                                                        ch.value = s.m_Radius;
                                                }
                                                else if(str_comp(ch.pName, "Show FOV") == 0)
                                                        ch.on = aimbot.m_AimBotShowFov;
                                                else if(str_comp(ch.pName, "Show Radius") == 0)
                                                        ch.on = aimbot.m_AimBotShowRadius;
                                                else if(str_comp(ch.pName, "Use raycast (show)") == 0)
                                                        ch.on = aimbot.m_AimBotUseRaycastShow;
                                                else if(str_comp(ch.pName, "AB Raycast (aim)") == 0)
                                                        ch.on = s.m_UseRaycast;
                                                else if(str_comp(ch.pName, "AB Angles") == 0)
                                                        ch.value = (float)s.m_RaycastAngles;
                                                else if(str_comp(ch.pName, "AB Aim Mode") == 0)
                                                        ch.valueIdx = s.m_AimMode;
                                                else if(str_comp(ch.pName, "AB Team") == 0)
                                                        ch.valueIdx = s.m_TeamFilter;
                                                else if(str_comp(ch.pName, "AB Friend") == 0)
                                                        ch.valueIdx = s.m_FriendFilter;
                                                else if(str_comp(ch.pName, "AB Dummy") == 0)
                                                        ch.valueIdx = s.m_DummyFilter;
                                                else if(str_comp(ch.pName, "AB Freeze") == 0)
                                                        ch.valueIdx = s.m_FreezeFilter;
                                                else if(str_comp(ch.pName, "AB Priority") == 0)
                                                        ch.valueIdx = s.m_Priority;
                                        }
                                }
                                else if(str_comp(row.pName, "TriggerBot") == 0)
                                {
                                        row.on = aimbot.m_TriggerBotEnabled;
                                        int w = aimbot.m_TriggerBotSelectedWeapon;
                                        if(w < 0 || w >= CAimBot::NUM_WEAPONS_TRIGGER)
                                                w = 0;
                                        const auto &s = aimbot.m_TriggerBotWeapons[w];
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "TB Weapon") == 0)
                                                        ch.valueIdx = w;
                                                else if(str_comp(ch.pName, "TB Enable") == 0)
                                                        ch.on = s.m_Enabled;
                                                else if(str_comp(ch.pName, "TB Trigger") == 0)
                                                        ch.valueIdx = s.m_Trigger;
                                                else if(str_comp(ch.pName, "TB Trigger mode") == 0)
                                                        ch.valueIdx = s.m_TriggerMode;
                                                else if(str_comp(ch.pName, "TB FOV") == 0)
                                                        ch.value = s.m_Fov;
                                                else if(str_comp(ch.pName, "TB Radius") == 0)
                                                {
                                                        ch.max = aimbot.GetWeaponRadius(w);
                                                        ch.value = s.m_Radius;
                                                }
                                                else if(str_comp(ch.pName, "TB Show FOV") == 0)
                                                        ch.on = aimbot.m_TriggerBotShowFov;
                                                else if(str_comp(ch.pName, "TB Show Radius") == 0)
                                                        ch.on = aimbot.m_TriggerBotShowRadius;
                                                else if(str_comp(ch.pName, "TB Use raycast (show)") == 0)
                                                        ch.on = aimbot.m_TriggerBotUseRaycastShow;
                                                else if(str_comp(ch.pName, "TB Raycast (trigger)") == 0)
                                                        ch.on = s.m_UseRaycast;
                                                else if(str_comp(ch.pName, "TB Angles") == 0)
                                                        ch.value = (float)s.m_RaycastAngles;
                                                else if(str_comp(ch.pName, "TB Latency") == 0)
                                                        ch.value = (float)s.m_RandomLatency;
                                                else if(str_comp(ch.pName, "TB Team") == 0)
                                                        ch.valueIdx = s.m_TeamFilter;
                                                else if(str_comp(ch.pName, "TB Friend") == 0)
                                                        ch.valueIdx = s.m_FriendFilter;
                                                else if(str_comp(ch.pName, "TB Dummy") == 0)
                                                        ch.valueIdx = s.m_DummyFilter;
                                                else if(str_comp(ch.pName, "TB Freeze") == 0)
                                                        ch.valueIdx = s.m_FreezeFilter;
                                                else if(str_comp(ch.pName, "TB Priority") == 0)
                                                        ch.valueIdx = s.m_Priority;
                                        }
                                }
                        }
                }
        }
        // v1.56.77: sync ClickGUI Color input + Rainbow state from m_AccentColorRGBA.
        for(int p = 0; p < NUM_PANELS; ++p)
        {
                for(int r = 0; r < g_Panels[p].rowCount; ++r)
                {
                        SRow &row = g_Panels[p].pRows[r];
                        if(str_comp(row.pName, "ClickGUI Color") == 0 && row.type == ERowType::Input)
                        {
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r)
                                {
                                        char hex[8];
                                        str_format(hex, sizeof(hex), "%06x", m_AccentColorRGBA & 0xffffff);
                                        str_copy(row.m_aInputBuf, hex, sizeof(row.m_aInputBuf));
                                        row.m_InputInitialized = true;
                                }
                        }
                        else if(str_comp(row.pName, "Rainbow color") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = m_RainbowEnabled;
                                // v1.56.78: do NOT force row.expanded — let the user
                                // expand/collapse independently of the toggle state,
                                // like every other expandable.
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        if(str_comp(child.pName, "Speed") == 0)
                                                child.value = m_RainbowSpeed;
                                }
                        }
                }
        }
        // v1.56.76: capture the open→closed transition BEFORE updating m_WasOpen
        // so we can hide the cursor instantly when ClickGUI starts closing.
        const bool wasOpenThisFrame = m_WasOpen;
        m_WasOpen = isOpen;

        // v1.56.76: hide the Windows cursor INSTANTLY when cl_clickgui goes 1→0
        // (transition open→closed), not after the close animation finishes. The
        // close animation still plays (m_AnimProgress keeps decreasing toward 0),
        // only the cursor disappears immediately. Symmetric with opening, where
        // MouseModeAbsolute is called instantly on open.
        if(!isOpen && wasOpenThisFrame && !GameClient()->m_GameConsole.IsActive())
                Input()->MouseModeRelative();

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

        // v1.56.77: rainbow color rotation. When enabled, overwrite m_AccentColorRGBA
        // each frame with a hue-rotating color. HSV→RGB conversion inline.
        if(m_RainbowEnabled)
        {
                m_RainbowHue += dt * m_RainbowSpeed * 60.0f; // 60 deg/sec at speed=1
                if(m_RainbowHue >= 360.0f)
                        m_RainbowHue -= 360.0f;
                // HSV(hue, 1.0, 1.0) → RGB
                const float h = m_RainbowHue / 60.0f;
                const float c = 1.0f; // chroma (v=1, s=1)
                const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
                float r = 0, g = 0, b = 0;
                if(h < 1) { r = c; g = x; b = 0; }
                else if(h < 2) { r = x; g = c; b = 0; }
                else if(h < 3) { r = 0; g = c; b = x; }
                else if(h < 4) { r = 0; g = x; b = c; }
                else if(h < 5) { r = x; g = 0; b = c; }
                else { r = c; g = 0; b = x; }
                const unsigned ur = (unsigned)(r * 255.0f);
                const unsigned ug = (unsigned)(g * 255.0f);
                const unsigned ub = (unsigned)(b * 255.0f);
                m_AccentColorRGBA = 0xff000000u | (ur << 16) | (ug << 8) | ub;
        }

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
                                g_Config.m_KxDummyHammerLatencyMs = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Version ID") == 0)
                                g_Config.m_KxVersionSpoofId = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "FOV") == 0)
                                g_Config.m_KxLaserUnfreezeFov = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Angles") == 0)
                                g_Config.m_KxLaserUnfreezeAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks") == 0)
                                g_Config.m_KxLaserUnfreezeTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger ticks") == 0)
                                g_Config.m_KxLaserUnfreezeTriggerTicks = (int)pTarget->value;
                        // Settings → Line rendering children
                        else if(str_comp(pTarget->pName, "Line Margin") == 0)
                                g_Config.m_KxLineRenderingSize = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Line opacity") == 0)
                                g_Config.m_KxLineRenderingAlpha = (int)pTarget->value;
                        // Advanced → Trajectory children
                        else if(str_comp(pTarget->pName, "Prediction Ticks") == 0)
                                g_Config.m_KxTrajectoryTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Speed") == 0)
                        {
                                // v1.56.77: "Speed" is used by both Fake Aim and Rainbow.
                                // Disambiguate by checking which panel the slider is in.
                                bool isRainbow = false;
                                if(m_DragSliderChild >= 0 && m_DragSliderPanel >= 0)
                                {
                                        SPanel &dragPanel = g_Panels[m_DragSliderPanel];
                                        if(m_DragSliderRow >= 0 && m_DragSliderRow < dragPanel.rowCount)
                                        {
                                                SRow &parentRow = dragPanel.pRows[m_DragSliderRow];
                                                if(str_comp(parentRow.pName, "Rainbow color") == 0)
                                                        isRainbow = true;
                                        }
                                }
                                if(isRainbow)
                                        m_RainbowSpeed = pTarget->value;
                                else
                                        g_Config.m_KxFakeAimSpeed = (int)pTarget->value;
                        }
                        else if(str_comp(pTarget->pName, "Chunk size") == 0)
                                g_Config.m_KxPfChunkSize = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Hook angles") == 0)
                                g_Config.m_KxPfHookAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Candidates") == 0)
                                g_Config.m_KxPfCandidates = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Performance") == 0)
                                g_Config.m_KxPfPerf = (int)pTarget->value;
                        // v1.56.83: AimBot/TriggerBot per-weapon sliders (disambiguate by parent expandable)
                        else if(m_DragSliderChild >= 0 && m_DragSliderPanel >= 0)
                        {
                                SPanel &dragPanel = g_Panels[m_DragSliderPanel];
                                if(m_DragSliderRow >= 0 && m_DragSliderRow < dragPanel.rowCount)
                                {
                                        SRow &parentRow = dragPanel.pRows[m_DragSliderRow];
                                        CAimBot &ab = GameClient()->m_AimBot;
                                        if(str_comp(parentRow.pName, "AimBot") == 0)
                                        {
                                                int w = ab.m_AimBotSelectedWeapon;
                                                if(w >= 0 && w < CAimBot::NUM_WEAPONS_AIM)
                                                {
                                                    auto &s = ab.m_AimBotWeapons[w];
                                                    if(str_comp(pTarget->pName, "AB FOV") == 0)
                                                        s.m_Fov = pTarget->value;
                                                    else if(str_comp(pTarget->pName, "AB Radius") == 0)
                                                        s.m_Radius = pTarget->value;
                                                    else if(str_comp(pTarget->pName, "AB Angles") == 0)
                                                        s.m_RaycastAngles = (int)pTarget->value;
                                                }
                                        }
                                        else if(str_comp(parentRow.pName, "TriggerBot") == 0)
                                        {
                                                int w = ab.m_TriggerBotSelectedWeapon;
                                                if(w >= 0 && w < CAimBot::NUM_WEAPONS_TRIGGER)
                                                {
                                                    auto &s = ab.m_TriggerBotWeapons[w];
                                                    if(str_comp(pTarget->pName, "TB FOV") == 0)
                                                        s.m_Fov = pTarget->value;
                                                    else if(str_comp(pTarget->pName, "TB Radius") == 0)
                                                        s.m_Radius = pTarget->value;
                                                    else if(str_comp(pTarget->pName, "TB Angles") == 0)
                                                        s.m_RaycastAngles = (int)pTarget->value;
                                                    else if(str_comp(pTarget->pName, "TB Latency") == 0)
                                                        s.m_RandomLatency = (int)pTarget->value;
                                                }
                                        }
                                }
                        }
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

        // Mouse wheel — scroll the panel under the cursor.
        if(Event.m_Flags & IInput::FLAG_PRESS &&
                (Event.m_Key == KEY_MOUSE_WHEEL_UP || Event.m_Key == KEY_MOUSE_WHEEL_DOWN))
        {
                const vec2 mp = m_MousePos;
                // Iterate panels top-to-bottom (high z first).
                int order[NUM_PANELS];
                for(int i = 0; i < NUM_PANELS; ++i)
                        order[i] = i;
                for(int a = 0; a < NUM_PANELS; ++a)
                        for(int b = a + 1; b < NUM_PANELS; ++b)
                                if(m_aZOrder[order[b]] > m_aZOrder[order[a]])
                                {
                                        int tmp = order[a];
                                        order[a] = order[b];
                                        order[b] = tmp;
                                }
                for(int idx = 0; idx < NUM_PANELS; ++idx)
                {
                        const int i = order[idx];
                        if(!m_aPanelPosValid[i])
                                continue;
                        const vec2 p = m_aPanelPos[i];
                        const float bodyTop = p.y + HEADER_HEIGHT;
                        const float bodyBottom = p.y + HEADER_HEIGHT + MAX_BODY_HEIGHT + 50.0f;
                        if(mp.x >= p.x && mp.x < p.x + PANEL_WIDTH && mp.y >= bodyTop && mp.y < bodyBottom)
                        {
                                const float dir = (Event.m_Key == KEY_MOUSE_WHEEL_UP) ? -SCROLL_STEP : SCROLL_STEP;
                                // Write to stored so the value survives panel collapse/expand.
                                m_aPanelScrollStored[i] += dir;
                                if(m_aPanelScrollStored[i] < 0.0f)
                                        m_aPanelScrollStored[i] = 0.0f;
                                return true;
                        }
                }
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
                        // v1.56.77: parse HEX color for "ClickGUI Color" input.
                        if(str_comp(pRow->pName, "ClickGUI Color") == 0)
                        {
                                unsigned hex = 0;
                                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                                {
                                        m_AccentColorRGBA = 0xff000000u | (hex & 0xffffff);
                                        m_RainbowEnabled = false; // manual color disables rainbow
                                        // v1.56.78: color already set from HEX above, update saved too.
                                        m_SavedAccentColorRGBA = m_AccentColorRGBA;
                                }
                        }
                        // v1.56.89: parse HEX color for "Line color" input (Line rendering expandable).
                        else if(str_comp(pRow->pName, "Line color") == 0)
                        {
                                unsigned hex = 0;
                                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                                        g_Config.m_KxLineRenderingColor = hex & 0xffffff;
                        }
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
                                         str_comp(pTarget->pName, "Visuals") == 0 ||
                                         str_comp(pTarget->pName, "Line rendering") == 0);
                                // Click on check area or name area → toggle on/off (or expand for visual).
                                const float toggleEndX = rx + rw - (pTarget->type == ERowType::Expandable ? ARROW_W + 4.0f : (pTarget->type == ERowType::ToggleDropdown ? DROPDOWN_W + 4.0f : 0.0f));
                                if(mousePos.x < toggleEndX)
                                {
                                        if(isVisualExpandable)
                                        {
                                                // Visual expandable: only expand/collapse, no toggle.
                                                pTarget->expanded = !pTarget->expanded;
                                                m_aLayoutCacheDirty[i] = true; // v1.56.72: invalidate Yoga cache
                                                return;
                                        }
                                        pTarget->on = !pTarget->on;
                                        // Wire reference toggles to real subsystems.
                                        if(str_comp(pTarget->pName, "Block") == 0)
                                                GameClient()->m_BotNet.m_AttackEnabled = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Random Aim") == 0)
                                                GameClient()->m_BotNet.m_RandomAim = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Trajectory") == 0)
                                                g_Config.m_KxShowTrajectory = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Laser unfreeze") == 0)
                                                g_Config.m_KxLaserUnfreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Fake Aim") == 0)
                                                g_Config.m_KxFakeAim = pTarget->on ? 1 : 0;
                                        // Functions panel — Fly/Copy Moves sub-section
                                        else if(str_comp(pTarget->pName, "Preserve input") == 0)
                                                g_Config.m_KxDummyHammerKeepInputs = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom latency") == 0)
                                                g_Config.m_KxDummyHammerCustomLatency = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Auto fly") == 0)
                                                g_Config.m_KxDummyHammerAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror direction") == 0)
                                                g_Config.m_KxDummyCopyMirrorDir = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror aim X") == 0)
                                                g_Config.m_KxDummyCopyMirrorAimX = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror aim Y") == 0)
                                                g_Config.m_KxDummyCopyMirrorAimY = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Smart aim") == 0)
                                                g_Config.m_KxDummyCopySmartAim = pTarget->on ? 1 : 0;
                                        // v1.56.83: AimBot/TriggerBot expandable master toggles
                                        else if(str_comp(pTarget->pName, "AimBot") == 0)
                                                GameClient()->m_AimBot.m_AimBotEnabled = pTarget->on;
                                        else if(str_comp(pTarget->pName, "TriggerBot") == 0)
                                                GameClient()->m_AimBot.m_TriggerBotEnabled = pTarget->on;
                                        // v1.56.83: AimBot per-weapon child toggles (parent = "AimBot")
                                        else if(str_comp(row.pName, "AimBot") == 0)
                                        {
                                                CAimBot &ab = GameClient()->m_AimBot;
                                                int w = ab.m_AimBotSelectedWeapon;
                                                if(w >= 0 && w < CAimBot::NUM_WEAPONS_AIM)
                                                {
                                                        auto &s = ab.m_AimBotWeapons[w];
                                                        if(str_comp(pTarget->pName, "AB Enable") == 0)
                                                            s.m_Enabled = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Show FOV") == 0)
                                                            ab.m_AimBotShowFov = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Show Radius") == 0)
                                                            ab.m_AimBotShowRadius = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Use raycast (show)") == 0)
                                                            ab.m_AimBotUseRaycastShow = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "AB Raycast (aim)") == 0)
                                                            s.m_UseRaycast = pTarget->on;
                                                }
                                        }
                                        // v1.56.83: TriggerBot per-weapon child toggles (parent = "TriggerBot")
                                        else if(str_comp(row.pName, "TriggerBot") == 0)
                                        {
                                                CAimBot &tb = GameClient()->m_AimBot;
                                                int w = tb.m_TriggerBotSelectedWeapon;
                                                if(w >= 0 && w < CAimBot::NUM_WEAPONS_TRIGGER)
                                                {
                                                        auto &s = tb.m_TriggerBotWeapons[w];
                                                        if(str_comp(pTarget->pName, "TB Enable") == 0)
                                                            s.m_Enabled = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "TB Show FOV") == 0)
                                                            tb.m_TriggerBotShowFov = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "TB Show Radius") == 0)
                                                            tb.m_TriggerBotShowRadius = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "TB Use raycast (show)") == 0)
                                                            tb.m_TriggerBotUseRaycastShow = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "TB Raycast (trigger)") == 0)
                                                            s.m_UseRaycast = pTarget->on;
                                                }
                                        }
                                        // Copy Moves Filter expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Copy Moves Filter") == 0)
                                                g_Config.m_KxDummyCopyMovesFilter = pTarget->on ? 1 : 0;
                                        // Copy Moves Filter sub-toggles
                                        else if(str_comp(pTarget->pName, "Filter: Jump") == 0)
                                                g_Config.m_KxDummyCopyMovesFilterJump = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Direction") == 0)
                                                g_Config.m_KxDummyCopyMovesFilterDirection = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Hook") == 0)
                                                g_Config.m_KxDummyCopyMovesFilterHook = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Aim") == 0)
                                                g_Config.m_KxDummyCopyMovesFilterAim = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Filter: Fire") == 0)
                                                g_Config.m_KxDummyCopyMovesFilterFire = pTarget->on ? 1 : 0;
                                        // Settings → IRC children
                                        else if(str_comp(pTarget->pName, "Reveal join") == 0)
                                                g_Config.m_KxIrcRevealJoin = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Nameplate tag") == 0)
                                                g_Config.m_KxIrcNameplateTag = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze children
                                        else if(str_comp(pTarget->pName, "Auto laser") == 0)
                                                g_Config.m_KxLaserUnfreezeAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Silent") == 0)
                                                g_Config.m_KxLaserUnfreezeSilent = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show attempt") == 0)
                                                g_Config.m_KxLaserUnfreezeShowAttempt = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show for me") == 0)
                                                g_Config.m_KxFakeAimShowForMe = pTarget->on ? 1 : 0;
                                        // Pathfinder → Score method children
                                        else if(str_comp(pTarget->pName, "Distance reduction") == 0)
                                                g_Config.m_KxPfScoreDist = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Flow-field alignment") == 0)
                                                g_Config.m_KxPfScoreFlow = pTarget->on ? 1 : 0;
                                        // Pathfinder → Penalties children
                                        else if(str_comp(pTarget->pName, "Freeze penalty") == 0)
                                                g_Config.m_KxPfFineFreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Hook retract penalty") == 0)
                                                g_Config.m_KxPfFineRetract = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Death penalty") == 0)
                                                g_Config.m_KxPfFineDeath = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Wall-hit penalty") == 0)
                                                g_Config.m_KxPfFineWall = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Air-time penalty") == 0)
                                                g_Config.m_KxPfFineAir = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Hook-fire penalty") == 0)
                                                g_Config.m_KxPfFineHook = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Stuck penalty") == 0)
                                                g_Config.m_KxPfFineStuck = pTarget->on ? 1 : 0;
                                        // Pathfinder → Visuals children
                                        else if(str_comp(pTarget->pName, "Show field") == 0)
                                                g_Config.m_KxPfShowField = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show hooks") == 0)
                                                g_Config.m_KxPfShowHooks = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show branches") == 0)
                                                g_Config.m_KxPfShowBranches = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show speed") == 0)
                                                g_Config.m_KxPfShowSpeed = pTarget->on ? 1 : 0;
                                        // Pathfinder top-level
                                        else if(str_comp(pTarget->pName, "Advanced search") == 0)
                                                g_Config.m_KxPfAdvancedSearch = pTarget->on ? 1 : 0;
                                        // Settings → IRC/Spoofer expandables are master toggles.
                                        else if(str_comp(pTarget->pName, "IRC") == 0)
                                                g_Config.m_KxIrcEnabled = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Spoofer") == 0)
                                        {
                                                g_Config.m_KxVersionSpoof = pTarget->on ? 1 : 0;
                                                g_Config.m_KxSpoofClientStr = pTarget->on ? 1 : 0;
                                        }
                                        else if(str_comp(pTarget->pName, "Rainbow color") == 0)
                                        {
                                                // v1.56.78: save color before enabling rainbow,
                                                // restore it when disabling.
                                                if(pTarget->on && !m_RainbowEnabled)
                                                        m_SavedAccentColorRGBA = m_AccentColorRGBA;
                                                else if(!pTarget->on && m_RainbowEnabled)
                                                        m_AccentColorRGBA = m_SavedAccentColorRGBA;
                                                m_RainbowEnabled = pTarget->on;
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
                                        m_aLayoutCacheDirty[i] = true; // v1.56.72: invalidate Yoga cache
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
                                        g_Config.m_KxDummyHammerLatencyMs = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Version ID") == 0)
                                        g_Config.m_KxVersionSpoofId = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "FOV") == 0)
                                        g_Config.m_KxLaserUnfreezeFov = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Angles") == 0)
                                        g_Config.m_KxLaserUnfreezeAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks") == 0)
                                        g_Config.m_KxLaserUnfreezeTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger ticks") == 0)
                                        g_Config.m_KxLaserUnfreezeTriggerTicks = (int)pTarget->value;
                                // Settings → Line rendering children
                                else if(str_comp(pTarget->pName, "Line Margin") == 0)
                                        g_Config.m_KxLineRenderingSize = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Line opacity") == 0)
                                        g_Config.m_KxLineRenderingAlpha = (int)pTarget->value;
                                // Advanced → Trajectory children
                                else if(str_comp(pTarget->pName, "Prediction Ticks") == 0)
                                        g_Config.m_KxTrajectoryTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Chunk size") == 0)
                                        g_Config.m_KxPfChunkSize = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Hook angles") == 0)
                                        g_Config.m_KxPfHookAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Candidates") == 0)
                                        g_Config.m_KxPfCandidates = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Performance") == 0)
                                        g_Config.m_KxPfPerf = (int)pTarget->value;
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
        float cursorY = ppos.y + HEADER_HEIGHT + BODY_PADDING - m_aPanelScroll[m_DropdownPanel];
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
                                                g_Config.m_KxDummyCopyAimMode = o;
                                        else if(str_comp(target.pName, "Client preset") == 0)
                                                g_Config.m_KxSpoofClientPreset = o;
                                        else if(str_comp(target.pName, "Mode") == 0)
                                                g_Config.m_KxFakeAimMode = o;
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
                                                                g_Config.m_KxDummyCopyAimMode = o;
                                                        else if(str_comp(target.pName, "Client preset") == 0)
                                                                g_Config.m_KxSpoofClientPreset = o;
                                                        else if(str_comp(target.pName, "Mode") == 0)
                                                                g_Config.m_KxFakeAimMode = o;
                                                        // v1.56.83: AimBot per-weapon dropdowns (parent = "AimBot")
                                                        else if(str_comp(row.pName, "AimBot") == 0)
                                                        {
                                                                CAimBot &ab = GameClient()->m_AimBot;
                                                                if(str_comp(target.pName, "AB Weapon") == 0)
                                                                        ab.m_AimBotSelectedWeapon = o;
                                                                else
                                                                {
                                                                        int w = ab.m_AimBotSelectedWeapon;
                                                                        if(w >= 0 && w < CAimBot::NUM_WEAPONS_AIM)
                                                                        {
                                                                                auto &s = ab.m_AimBotWeapons[w];
                                                                                if(str_comp(target.pName, "AB Aim Mode") == 0)
                                                                                        s.m_AimMode = o;
                                                                                else if(str_comp(target.pName, "AB Team") == 0)
                                                                                        s.m_TeamFilter = o;
                                                                                else if(str_comp(target.pName, "AB Friend") == 0)
                                                                                        s.m_FriendFilter = o;
                                                                                else if(str_comp(target.pName, "AB Dummy") == 0)
                                                                                        s.m_DummyFilter = o;
                                                                                else if(str_comp(target.pName, "AB Freeze") == 0)
                                                                                        s.m_FreezeFilter = o;
                                                                                else if(str_comp(target.pName, "AB Priority") == 0)
                                                                                        s.m_Priority = o;
                                                                        }
                                                                }
                                                        }
                                                        // v1.56.83: TriggerBot per-weapon dropdowns (parent = "TriggerBot")
                                                        else if(str_comp(row.pName, "TriggerBot") == 0)
                                                        {
                                                                CAimBot &tb = GameClient()->m_AimBot;
                                                                if(str_comp(target.pName, "TB Weapon") == 0)
                                                                        tb.m_TriggerBotSelectedWeapon = o;
                                                                else
                                                                {
                                                                        int w = tb.m_TriggerBotSelectedWeapon;
                                                                        if(w >= 0 && w < CAimBot::NUM_WEAPONS_TRIGGER)
                                                                        {
                                                                                auto &s = tb.m_TriggerBotWeapons[w];
                                                                                if(str_comp(target.pName, "TB Trigger") == 0)
                                                                                        s.m_Trigger = o;
                                                                                else if(str_comp(target.pName, "TB Trigger mode") == 0)
                                                                                        s.m_TriggerMode = o;
                                                                                else if(str_comp(target.pName, "TB Team") == 0)
                                                                                        s.m_TeamFilter = o;
                                                                                else if(str_comp(target.pName, "TB Friend") == 0)
                                                                                        s.m_FriendFilter = o;
                                                                                else if(str_comp(target.pName, "TB Dummy") == 0)
                                                                                        s.m_DummyFilter = o;
                                                                                else if(str_comp(target.pName, "TB Freeze") == 0)
                                                                                        s.m_FreezeFilter = o;
                                                                                else if(str_comp(target.pName, "TB Priority") == 0)
                                                                                        s.m_Priority = o;
                                                                        }
                                                                }
                                                        }
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
        float cursorY = ppos.y + HEADER_HEIGHT + BODY_PADDING - m_aPanelScroll[m_DropdownPanel];
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
                                                ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f));
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
                                                                ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f));
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
                // v1.56.77: parse HEX color for "ClickGUI Color" input.
                if(str_comp(pRow->pName, "ClickGUI Color") == 0)
                {
                        unsigned hex = 0;
                        if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                        {
                                m_AccentColorRGBA = 0xff000000u | (hex & 0xffffff);
                                m_RainbowEnabled = false;
                                m_SavedAccentColorRGBA = m_AccentColorRGBA; // v1.56.78
                        }
                }
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
        // Applies the per-panel scroll offset so hit-testing matches the
        // shifted row positions in the render loop.
        const vec2 ppos = m_aPanelPosValid[panelIdx] ? m_aPanelPos[panelIdx] : PanelHomePos(panelIdx);
        const float bodyX = ppos.x + BODY_PADDING;
        float cursorY = ppos.y + HEADER_HEIGHT + BODY_PADDING - m_aPanelScroll[panelIdx];

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
        // v1.56.72: optimized — Yoga layout cached per panel, rebuilt only when
        // the row membership signature changes (expandable opened/closed).
        // Previously: YGNodeNew + YGNodeCalculateLayout every frame (240/sec).
        // Now: only on actual layout changes.
        const bool isFunctionsPanel = (str_comp(panel.pTitle, "Functions") == 0);

        // Build the layout signature: hash of (row index, child included?) bits.
        // Children are included when m_AnimExpand > 0 (covers expanded + animating).
        unsigned sig = 0;
        for(int r = 0; r < panel.rowCount; ++r)
        {
                const SRow &row = panel.pRows[r];
                if(isFunctionsPanel && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
                sig = sig * 31u + (unsigned)r + 1u;
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                sig = sig * 31u + (unsigned)(c + 100);
                        }
                }
        }

        SLayoutCache &cache = m_aLayoutCache[panelIdx];
        const bool needRebuild = m_aLayoutCacheDirty[panelIdx] || cache.signature != sig;

        YGNodeRef bodyRoot = nullptr;
        YGNodeRef rowNodes[64];
        int rowCount = 0;
        float bodyHeightFull = 0.0f;

        if(collapseAnim < 1.0f || !collapsed)
        {
                if(needRebuild)
                {
                        // Free old cached tree.
                        if(cache.pRoot)
                                YGNodeFreeRecursive(cache.pRoot);
                        cache.pRoot = YGNodeNew();
                        cache.rowCount = 0;
                        bodyRoot = cache.pRoot;
                        YGNodeStyleSetWidth(bodyRoot, PANEL_WIDTH);
                        YGNodeStyleSetPadding(bodyRoot, YGEdgeAll, BODY_PADDING);
                        YGNodeStyleSetFlexDirection(bodyRoot, YGFlexDirectionColumn);
                        YGNodeStyleSetGap(bodyRoot, YGGutterAll, ROW_GAP);
                        YGNodeStyleSetOverflow(bodyRoot, YGOverflowHidden);

                        constexpr int MAX_ROWS = 256;
                        auto addRowNode = [&](const SRow &row, bool isChild) {
                                if(cache.rowCount >= MAX_ROWS)
                                        return;
                                YGNodeRef n = YGNodeNew();
                                YGNodeStyleSetHeight(n, RowHeight(row.type));
                                if(isChild)
                                        YGNodeStyleSetMargin(n, YGEdgeLeft, SUB_LIST_INDENT);
                                YGNodeInsertChild(bodyRoot, n, (size_t)cache.rowCount);
                                cache.aRowNodes[cache.rowCount++] = n;
                        };

                        for(int r = 0; r < panel.rowCount; ++r)
                        {
                                SRow &row = panel.pRows[r];
                                if(isFunctionsPanel && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                                        continue;
                                addRowNode(row, false);
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
                        cache.bodyHeightFull = YGNodeLayoutGetHeight(bodyRoot);
                        cache.signature = sig;
                        m_aLayoutCacheDirty[panelIdx] = false;
                }
                else
                {
                        bodyRoot = cache.pRoot;
                }
                // Copy cached data to local vars for the render code below.
                rowCount = cache.rowCount;
                bodyHeightFull = cache.bodyHeightFull;
                for(int i = 0; i < rowCount; ++i)
                        rowNodes[i] = cache.aRowNodes[i];
        }

        // v1.56.61: scale body height by the EASED visibility (panel open/close).
        const float visibilityRaw = 1.0f - collapseAnim;
        const float visibilityTarget = collapsed ? 0.0f : 1.0f;
        const float easedVisibility = EaseBothDirections(visibilityRaw, visibilityTarget);

        // v1.56.72: single pass — compute expandProgress + childrenBlockH for each
        // expandable ONCE, store in a small map keyed by row index, reuse in the
        // render loop (avoids double EaseBothDirections + double block-height sum).
        struct ExpandAnimInfo
        {
                float expandProgress;
                float childrenBlockH;
                float hiddenH; // childrenBlockH * (1 - expandProgress)
        };
        ExpandAnimInfo expandInfo[16]; // max 16 expandables per panel
        int expandInfoCount = 0;
        // Map row index → expandInfo slot (for O(1) lookup in render loop).
        // Simple linear array indexed by row r; -1 = not an animating expandable.
        int rowToExpandSlot[64];
        for(int i = 0; i < 64; ++i)
                rowToExpandSlot[i] = -1;

        float hiddenChildrenHeight = 0.0f;
        for(int r = 0; r < panel.rowCount; ++r)
        {
                SRow &row = panel.pRows[r];
                if(isFunctionsPanel && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f && row.m_AnimExpand < 1.0f)
                {
                        const float expandProgress = EaseBothDirections(row.m_AnimExpand, row.expanded ? 1.0f : 0.0f);
                        float childrenBlockH = 0.0f;
                        int visibleCount = 0;
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                childrenBlockH += RowHeight(row.pChildren[c].type);
                                visibleCount++;
                        }
                        if(visibleCount > 1)
                                childrenBlockH += ROW_GAP * (visibleCount - 1);
                        if(visibleCount > 0)
                                childrenBlockH += ROW_GAP;
                        const float hiddenH = childrenBlockH * (1.0f - expandProgress);
                        hiddenChildrenHeight += hiddenH;

                        if(expandInfoCount < 16 && r < 64)
                        {
                                rowToExpandSlot[r] = expandInfoCount;
                                expandInfo[expandInfoCount++] = {expandProgress, childrenBlockH, hiddenH};
                        }
                }
        }

        // v1.56.72: single pass — compute expandProgress + childrenBlockH for each
        // expandable ONCE, store in a small map keyed by row index, reuse in the
        // render loop (avoids double EaseBothDirections + double block-height sum).
        const float contentHeightFull = bodyHeightFull - hiddenChildrenHeight;
        const bool scrollable = contentHeightFull > MAX_BODY_HEIGHT;

        // Scale body height by the EASED visibility (panel open/close).
        // For scrollable panels, animate the VISIBLE height (MAX → 0) instead of
        // the full content height — otherwise the visible box stays at MAX for the
        // first half of the collapse animation, then snaps shut abruptly.
        const float animBase = scrollable ? MAX_BODY_HEIGHT : contentHeightFull;
        const float bodyHeight = animBase * easedVisibility;

        // Cap the visible body height at MAX_BODY_HEIGHT. The existing ClipEnable
        // call clips rows outside this rect, so scrolled-away rows are GPU-clipped.
        const float visibleBodyHeight = scrollable ? std::min(bodyHeight, MAX_BODY_HEIGHT) : bodyHeight;

        // Clamp the per-panel scroll offset. 'scrollable' is based on the full
        // content height (not the animated bodyHeight), so it stays stable across
        // the whole collapse/expand animation — the scroll state isn't reset
        // mid-animation. When not scrollable, stored is left untouched so the
        // user's last scroll position survives until content comes back.
        const float maxScroll = scrollable ? (contentHeightFull - MAX_BODY_HEIGHT) : 0.0f;
        if(scrollable)
                m_aPanelScroll[panelIdx] = m_aPanelScrollStored[panelIdx];
        float &scrollRef = m_aPanelScroll[panelIdx];
        if(scrollRef < 0.0f)
                scrollRef = 0.0f;
        if(scrollRef > maxScroll)
                scrollRef = maxScroll;
        if(scrollable)
                m_aPanelScrollStored[panelIdx] = scrollRef;
        const float scrollOffset = scrollable ? scrollRef : 0.0f;

        // ---- Step 2: panel geometry ----
        const float panelH = HEADER_HEIGHT + visibleBodyHeight;
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
                ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha));

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
                DrawRoundedRect(bodyX, bodyY, bodyW, visibleBodyHeight,
                        0.0f, IGraphics::CORNER_NONE,
                        ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(alpha * 0.5f));

                Graphics()->ClipEnable((int)bodyX, (int)bodyY, (int)bodyW, (int)bodyH_safe(visibleBodyHeight));

        // ---- Step 5: draw each row using its Yoga-computed rectangle ----
        // v1.56.71: rows below an animating expandable shift UP by that
        // expandable's hidden children height. We accumulate `rowYOffset`
        // (negative = up) as we walk down. This matches bodyHeight (which
        // subtracts hiddenChildrenHeight) so rows, panel border, and the single
        // body clip all stay in sync. Children are in Yoga (rowNodes) and render
        // at their Yoga positions — no separate clip, no sub-panel.
        // rowYOffset starts at -scrollOffset so the content shifts up when the
        // user scrolls down. Rows above the visible window are clipped by the
        // body ClipEnable above.
        float rowYOffset = -scrollOffset;
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
                const float ry = bodyY + ryLocal + rowYOffset;
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
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x93" /* ✓ */,
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 18.0f, row.pName, textCol);
                        break;
                }
                case ERowType::ToggleDropdown:
                {
                        // [✓ or empty] [name] [select box]
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x93",
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
                        Graphics()->SetColor(ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha));
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
                                editingThis ? ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
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
                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f));
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
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x93",
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
                        ColorRGBA btnCol = ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha); // default purple
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

                // v1.56.74: render expanded children — CLIP the children block to
                // (childrenBlockH * expandProgress) so children are CROPPED from the
                // bottom (like the panel close animation), NOT shifted up. Lower rows
                // still shift up via rowYOffset (so they don't leave a gap), but the
                // children themselves stay at their Yoga positions and get clipped.
                // This gives the smooth "crop" feel instead of "slide up + disappear".
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                {
                        // Compute the clip rect for this expandable's children block.
                        float childrenClipH = 0.0f;
                        if(r < 64 && rowToExpandSlot[r] >= 0)
                        {
                                const ExpandAnimInfo &info = expandInfo[rowToExpandSlot[r]];
                                childrenClipH = info.childrenBlockH * info.expandProgress;
                        }
                        else if(row.m_AnimExpand >= 1.0f)
                        {
                                // Fully expanded — no clip, full block visible.
                                childrenClipH = 99999.0f; // effectively no clip
                        }

                        const float childrenTopRaw = ry + rh;
                        const bool useChildClip = (childrenClipH < 99999.0f && childrenClipH > 0.5f);

                        // Intersect the children clip rect with the visible body rect
                        // so children don't paint outside the scroll window when the
                        // panel is scrollable. Visible body Y range: [bodyY, bodyY + visibleBodyHeight].
                        const float visBodyBottom = bodyY + visibleBodyHeight;
                        float childClipY = childrenTopRaw;
                        float childClipH = childrenClipH;
                        if(useChildClip)
                        {
                                if(childClipY < bodyY)
                                {
                                        const float cut = bodyY - childClipY;
                                        childClipY = bodyY;
                                        childClipH -= cut;
                                }
                                if(childClipY + childClipH > visBodyBottom)
                                        childClipH = visBodyBottom - childClipY;
                                if(childClipH < 0.5f)
                                        childClipH = 0.0f; // fully outside — skip clip+render
                        }

                        // v1.56.75: if the expandable is animating but the clip height
                        // is ~0 (end of collapse), skip rendering children entirely.
                        // Otherwise they'd render for one frame without a clip and
                        // flicker before m_AnimExpand reaches 0 and removes them from
                        // the layout.
                        if(row.m_AnimExpand < 1.0f && !useChildClip)
                        {
                                // Still need to advance rowIdx past these children so
                                // subsequent rows read the correct Yoga nodes.
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                                continue;
                                        ++rowIdx;
                                }
                                // Update rowYOffset for lower rows (children are hidden).
                                if(r < 64 && rowToExpandSlot[r] >= 0)
                                {
                                        const ExpandAnimInfo &info = expandInfo[rowToExpandSlot[r]];
                                        rowYOffset -= info.hiddenH;
                                }
                                continue;
                        }

                        // Apply the intersected children clip rect. When fully expanded
                        // (childrenClipH = 99999), useChildClip is false and we rely on
                        // the body clip. For animating expand/collapse, childClipH is the
                        // visible portion of the children block (intersected with the body).
                        if(useChildClip && childClipH > 0.5f)
                                Graphics()->ClipEnable((int)bodyX, (int)childClipY, (int)bodyW, (int)bodyH_safe(childClipH));

                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                const YGNodeRef cnode = rowNodes[rowIdx];
                                const float crx = bodyX + YGNodeLayoutGetLeft(cnode);
                                const float cry = bodyY + YGNodeLayoutGetTop(cnode) + rowYOffset;
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
                                        DrawRoundedRect(crx + 1.0f, cry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                        if(child.on)
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x93",
                                                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
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
                                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(alpha));
                                        DrawText(vx + 6.0f, cry + 2.0f, 17.0f, vbuf, cText);
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
                                        Graphics()->SetColor(ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha));
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
                                        if(!child.m_InputInitialized)
                                        {
                                                if(child.pInputValue)
                                                        str_copy(child.m_aInputBuf, child.pInputValue, sizeof(child.m_aInputBuf));
                                                child.m_InputInitialized = true;
                                        }
                                        const bool editingChild = (m_InputEditing && m_InputPanel == panelIdx && m_InputRow == r && m_InputChild == c);
                                        DrawRoundedRect(ix, iy, iw, ih, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                                        DrawRoundedRect(ix - 1.0f, iy - 1.0f, iw + 2.0f, ih + 2.0f, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                editingChild ? ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
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
                                                        : ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha);
                                        }
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
                                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f));
                                        }
                                        if(pT)
                                                DrawText(ix + 4.0f, iy + 3.0f, 17.0f, pT, ic);
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
                                        if(child.type == ERowType::ToggleDropdown)
                                        {
                                                DrawRoundedRect(crx + 1.0f, cry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                                        IGraphics::CORNER_ALL,
                                                        ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                                if(child.on)
                                                        DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x93",
                                                                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                        }
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        const float ddx = crx + crw - DROPDOWN_W - 2.0f;
                                        const float ddy = cry + (crh - DROPDOWN_H) * 0.5f;
                                        DrawRoundedRect(ddx, ddy, DROPDOWN_W, DROPDOWN_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                                        const char *pV = (child.optionCount > 0 && child.valueIdx >= 0 && child.valueIdx < child.optionCount)
                                                ? child.pOptions[child.valueIdx]
                                                : "";
                                        DrawText(ddx + 4.0f, ddy + 3.0f, 17.0f, pV, cText);
                                        break;
                                }
                                case ERowType::Expandable:
                                        // Nested expandables not in the data model; render as a plain toggle.
                                        DrawRoundedRect(crx + 1.0f, cry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                        if(child.on)
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x93",
                                                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        break;
                                }
                        }

                        // Restore the body clip for subsequent rows.
                        if(useChildClip)
                                Graphics()->ClipEnable((int)bodyX, (int)bodyY, (int)bodyW, (int)bodyH_safe(visibleBodyHeight));

                        // v1.56.72: use precomputed expandInfo (avoids re-calling
                        // EaseBothDirections and re-summing childrenBlockH here).
                        // rowToExpandSlot[r] gives the slot in expandInfo, or -1
                        // if this expandable is fully expanded (no hidden height).
                        if(r < 64 && rowToExpandSlot[r] >= 0)
                        {
                                const ExpandAnimInfo &info = expandInfo[rowToExpandSlot[r]];
                                rowYOffset -= info.hiddenH;
                        }
                }
        }

                Graphics()->ClipDisable();
        } // end if(!collapsed)

        // v1.56.72: do NOT free bodyRoot here — the Yoga tree is now cached in
        // m_aLayoutCache[panelIdx].pRoot and reused across frames. It is freed
        // only when the cache is rebuilt (see YGNodeFreeRecursive above) or on
        // component destruction. Previously this freed the cached tree every
        // frame, causing a use-after-free crash on the next frame.
}
