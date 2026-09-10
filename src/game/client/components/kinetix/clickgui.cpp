// (c) Kinetix. ClickGUI overlay component — see clickgui.h for overview.
//
// v1.56.34 (UI-only): renders 3 column panels with open/close animation.
// No UX yet (no drag/click/input handling). All visuals are static; row
// states reflect the hardcoded defaults from upload/clickgui.html.
//
// Layout: Yoga (flexbox) computes the body's row stack; we then draw each
// row via IGraphics (rounded rects, gradient fills) and ITextRender (text).

#include <game/client/components/kinetix/clickgui.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/chat.h>
#include <game/client/components/kinetix/code_exec/code_exec.h>
#include <game/client/components/kinetix/code_exec/file_dialog.h>

#include <yoga/Yoga.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/color.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/components/console.h>
#include <game/client/gameclient.h>
#include <game/version.h> // v1.56.177: GAME_NAME, GAME_RELEASE_VERSION, DDNET_VERSION_NUMBER, GIT_SHORTREV_HASH (used by Info panel)

namespace {
// Total number of ClickGUI panels. Must match CClickGui::NUM_PANELS in .h.
// v1.56.177: was stale at 5 — caused "too many initializers" on g_Panels[6].
constexpr int NUM_PANELS = 8;
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
        DoubleButton, // v1.56.90: two buttons in one row (Dummies Connect+Switch)
        Label, // v1.56.176: non-interactive text row (Info panel: version strings, FPS)
};

// v1.56.176: Kinetix client version string (shown in Info panel).
// Bumped manually per release. Matches the commit-message version scheme.
#define KINETIX_VERSION "v5 beta"

struct SRow
{
        ERowType type;
        const char *pName;
        const char *pName2; // v1.56.90: DoubleButton second button label
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
        {ERowType::Slider, "Latency (ms)", nullptr, false, false, 50.0f, 2000.0f, 10.0f, 500.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.186: "Copy Moves Latency" expandable — staggered input replay delay.
// When >0, inactive dummies replay active player's input from N ms ago, where
// N = latency_ms * dummy_index (D1: 1x, D2: 2x, D3: 3x...). 0 = live copy.
static SRow g_CopyMovesLatencyChildren[] = {
        {ERowType::Slider, "Latency (ms)", nullptr, false, false, 50.0f, 5000.0f, 10.0f, 500.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.189: "Auto Fly" expandable — configurable auto-hammer for hammerfly.
// Children: Max Distance slider (5-63, default 50, real hammer max reach = 63
// = ProjStartPos offset 21 + FindEntities radius 14 + target ProximityRadius 28),
// Predict toggle (next-tick position prediction).
static SRow g_AutoFlyChildren[] = {
        {ERowType::Slider, "Max Distance", nullptr, false, false, 5.0f, 63.0f, 1.0f, 50.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Predict", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const char *const g_TripleFlyDummyModeOpts[] = {"Closest to me", "ID"};
static const char *const g_TripleFlyTargetModeOpts[] = {"Closest to me", "Closest to Aim", "IDs"};
static SRow g_TripleFlyChildren[] = {
        {ERowType::Dropdown, "Dummy mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TripleFlyDummyModeOpts, 2, 0, nullptr, 0},
        {ERowType::Input, "Dummy ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "1", "id...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Target mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TripleFlyTargetModeOpts, 3, 0, nullptr, 0},
        {ERowType::Input, "Target IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Trigger radius", nullptr, false, false, 32.0f, 380.0f, 1.0f, 380.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Radius", nullptr, false, false, 32.0f, 256.0f, 1.0f, 128.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Copy Moves Filter" expandable: 6 sub-toggles.
static SRow g_CopyMovesFilterChildren[] = {
        {ERowType::Toggle, "Filter: Jump", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Hook", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Fire", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Filter: Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// Aim mode dropdown options.
static const char *const g_AimModeOpts[] = {"Smart", "Mirror"};
// v1.56.83: AimBot/TriggerBot dropdown options (unique prefix AB/TB to avoid collisions).
static const char *const g_ABWeaponOpts[] = {"Hammer", "Pistol", "Shotgun", "Grenade", "Laser", "Ninja", "Hook"};
static const char *const g_ABWeaponOptsNoHook[] = {"Hammer", "Pistol", "Shotgun", "Grenade", "Laser", "Ninja"};
static const char *const g_ABTriggerOpts[] = {"Fire", "Hook"};
static const char *const g_ABTriggerModeOpts[] = {"One tick", "Hold", "Every tick"};
static const char *const g_ABAimModeOpts[] = {"Silent", "Trigger", "Always"};
static const char *const g_ABRulesOpts[] = {"None", "Insert line", "Raycast"};
static const char *const g_ABTeamOpts[] = {"Both", "War", "My"};
static const char *const g_ABFriendOpts[] = {"Both", "Ignore", "Only"};
static const char *const g_ABDummyOpts[] = {"Both", "Ignore", "Only"};
static const char *const g_ABTargetsFilterOpts[] = {"Both", "Only", "Ignore"};
static const char *const g_ABFreezeOpts[] = {"Both", "Freeze", "No freeze"};
static const char *const g_ABPriorityOpts[] = {"Nearest FOV", "Nearest Aim", "Nearest to me"};
// v1.56.83: AimBot expandable — 15 children.
static SRow g_AimBotChildren[] = {
        {ERowType::Dropdown, "AB Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABWeaponOpts, 7, 0, nullptr, 0},
        {ERowType::Toggle, "AB Enable", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "AB FOV", nullptr, false, false, 1.0f, 360.0f, 1.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "AB Radius", nullptr, false, false, 1.0f, 2000.0f, 1.0f, 300.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show FOV", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show Radius", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Use raycast (Show)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Rules", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABRulesOpts, 3, 0, nullptr, 0},
        {ERowType::Toggle, "Use raycast angle (AB)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // v1.56.192: AimBot Predict — iterative time-of-flight prediction (NOT in TriggerBot).
        {ERowType::Toggle, "Predict", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Aim Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABAimModeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTeamOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "Clanmate filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "AB Priority", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABPriorityOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "Targets filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0},
};
// v1.56.86: TriggerBot expandable — 17 children (no Hook, has Trigger + Trigger mode + Latency).
static SRow g_TriggerBotChildren[] = {
        {ERowType::Dropdown, "TB Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABWeaponOptsNoHook, 6, 0, nullptr, 0},
        {ERowType::Toggle, "TB Enable", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Trigger", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTriggerOpts, 2, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Trigger mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTriggerModeOpts, 3, 0, nullptr, 0},
        {ERowType::Slider, "TB FOV", nullptr, false, false, 1.0f, 360.0f, 1.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "TB Radius", nullptr, false, false, 1.0f, 2000.0f, 1.0f, 300.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Show FOV", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Show Radius", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "TB Use raycast (Show)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Rules", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABRulesOpts, 3, 0, nullptr, 0},
        {ERowType::Slider, "TB Latency", nullptr, false, false, 0.0f, 10.0f, 1.0f, 0.0f, " ticks", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTeamOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "Clanmate filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "TB Priority", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABPriorityOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "Targets filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0},
};
// v1.56.90: Dummies expandable — dummy connect/disconnect/switch buttons.
// MAX_DUMMIES=8 (D1..D7, index 0 = main). Layout: Connect/Disconnect button +
// Switch button per dummy. Plus top "Connect Dummy 1" (first connect) and
// "Switch to Main Player" (only visible when not on main).
static SRow g_DummiesChildren[] = {
        {ERowType::Button, "Connect Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, "Switch to Main", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D1 Connect", "D1 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D2 Connect", "D2 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D3 Connect", "D3 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D4 Connect", "D4 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D5 Connect", "D5 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D6 Connect", "D6 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "D7 Connect", "D7 Switch", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.90: Block expandable — all attack settings (37 children).
static SRow g_BlockChildren[] = {
        {ERowType::Toggle, "Attack Main", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Auto Main", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Main ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Target IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Rescue IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "All Target", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Auto Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Auto Fire", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Auto Hook", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Auto Hammer", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Move Enabled", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Stand Enabled", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Stand On X Only", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Smart Detect", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Smart Rescue", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Rescue Frozen", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Rescue All", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Kill On Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Avoid Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Avoid Freeze Radius", nullptr, false, false, 0, 0, 0, 0, nullptr, "3", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Laser Rescue", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Fire Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "200", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Hook Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "200", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Rescue Radius", nullptr, false, false, 0, 0, 0, 0, nullptr, "300", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Target Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "500", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Main Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "500", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Stand Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "100", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Main Stand Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "100", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Laser Rescue Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "500", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Pathfinder Enabled", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Simulate Players", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "SPS (Push)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Simulate Score", nullptr, false, false, 0, 0, 0, 0, nullptr, "50", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Rays", nullptr, false, false, 0, 0, 0, 0, nullptr, "24", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "32", "value...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Fix Snap", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Hook Enabled", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_FunctionsRows[] = {
        {ERowType::Expandable, "Dummies", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_DummiesChildren, 9},
        {ERowType::Expandable, "Block", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BlockChildren, 37},
        {ERowType::Toggle, "Random Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // v1.56.83: AimBot + TriggerBot expandables
        {ERowType::Expandable, "AimBot", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AimBotChildren, 19},
        {ERowType::Expandable, "TriggerBot", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TriggerBotChildren, 19},
        // Fly sub-section
        {ERowType::Toggle, "Preserve input", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Custom latency", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CustomLatencyChildren, 1},
        // v1.56.189: Auto fly is now an expandable with Max Distance + Predict children.
        {ERowType::Expandable, "Auto fly", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AutoFlyChildren, 2},
        {ERowType::Expandable, "Auto Triple Fly", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TripleFlyChildren, 6},
        // Copy Moves sub-section
        {ERowType::Expandable, "Copy Moves Filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CopyMovesFilterChildren, 6},
        {ERowType::Toggle, "Mirror direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Aim mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AimModeOpts, 2, 0, nullptr, 0},
        // Conditional aim options (visibility based on Aim mode)
        {ERowType::Toggle, "Mirror aim X", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Mirror aim Y", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Smart aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // v1.56.151: Fly Ride — pilot hooks/hammers nearest dummy, WASD moves anchor.
        {ERowType::Toggle, "Fly Ride", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        // v1.56.186: Copy Moves Latency — staggered input replay delay for inactive dummies.
        {ERowType::Expandable, "Copy Moves Latency", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CopyMovesLatencyChildren, 1},
};

// ---------- Advanced panel (5) ----------
// "Laser unfreeze" expandable: predictive laser self-unfreeze.
static SRow g_LaserUnfreezeChildren[] = {
        {ERowType::Toggle, "Auto laser", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "FOV", nullptr, false, false, 5.0f, 360.0f, 5.0f, 90.0f, "°", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Angles", nullptr, false, false, 1.0f, 144.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Ticks", nullptr, false, false, 1.0f, 50.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Trigger ticks", nullptr, false, false, 1.0f, 5.0f, 1.0f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Silent", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show attempt", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Fake Aim" expandable.
static const char *const g_FakeAimModeOpts[] = {"Random", "Robot", "Spin", "Lag"};
static SRow g_FakeAimChildren[] = {
        {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_FakeAimModeOpts, 4, 0, nullptr, 0},
        {ERowType::Slider, "Speed", nullptr, false, false, 1.0f, 100.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show for me", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.178: "Trajectory" expandable — per-type settings (Tee/Pistol/Shotgun/Grenade/Laser).
// Type dropdown selects which type's settings are being edited (same pattern as
// AimBot per-weapon dropdown). All toggles/sliders read/write the selected type's
// STypeSettings in CTrajectory. "Show for current" is hidden when Type=Tee (no
// projectiles for the tee type — it predicts tee movement only).
static const char *const g_TrajTypeOpts[] = {"Tee", "Pistol", "Shotgun", "Grenade", "Laser"};
static SRow g_TrajectoryChildren[] = {
        {ERowType::Dropdown, "Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TrajTypeOpts, 5, 0, nullptr, 0},
        {ERowType::Toggle, "Show", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Prediction Ticks", nullptr, false, false, 1.0f, 50.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Alpha Gradient", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Simulate Players", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show for other players", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show for current", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.104: "ESP" expandable — lines from point A to all players matching filters.
// Filter option arrays reuse the AimBot ones (identical option strings).
static const char *const g_EspModeOpts[] = {"Active player", "Screen coordinates"};
static const char *const g_EspStyleOpts[] = {"Line", "Arrow", "Dotted line", "Dotted Arrow"};
static SRow g_EspChildren[] = {
        {ERowType::Dropdown, "ESP Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EspStyleOpts, 4, 0, nullptr, 0},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTeamOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EspModeOpts, 2, 0, nullptr, 0},
        {ERowType::Slider, "ESP X", nullptr, false, false, 0.0f, 7680.0f, 1.0f, 0.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "ESP Y", nullptr, false, false, 0.0f, 4320.0f, 1.0f, 0.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "ESP Targets filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0},
};
// v1.56.121: "Basic Avoid Freeze" expandable.
static SRow g_BasicAvoidFreezeChildren[] = {
        {ERowType::Toggle, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Teleport", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Death", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Jump", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Hook", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "FOV", nullptr, false, false, 5.0f, 360.0f, 5.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Angles", nullptr, false, false, 1.0f, 144.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Silent", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Ticks", nullptr, false, false, 1.0f, 20.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Trigger ticks", nullptr, false, false, 1.0f, 20.0f, 1.0f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_AdvancedRows[] = {
        {ERowType::Expandable, "Trajectory", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TrajectoryChildren, 7},
        {ERowType::Expandable, "Laser unfreeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_LaserUnfreezeChildren, 7},
        {ERowType::Expandable, "Fake Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_FakeAimChildren, 3},
        {ERowType::Expandable, "ESP", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_EspChildren, 12},
        {ERowType::Expandable, "Basic Avoid Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BasicAvoidFreezeChildren, 12},
};

// ---------- Settings panel (6) ----------
// "IRC" expandable: 2 sub-toggles.
static SRow g_IrcChildren[] = {
        {ERowType::Toggle, "Reveal join", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Nameplate tag", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// "Spoofer" expandable: slider + dropdown + input.
static const char *const g_ClientPresetOpts[] = {"DDNet", "TClient", "BestClient", "Custom"};
static SRow g_SpooferChildren[] = {
        {ERowType::Slider, "Version ID", nullptr, false, false, 10000.0f, 21000.0f, 1.0f, 20000.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Client preset", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ClientPresetOpts, 4, 0, nullptr, 0},
        {ERowType::Input, "Custom client name", nullptr, false, false, 0, 0, 0, 0, nullptr, "MyClient", "name...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Client version", nullptr, false, false, 0, 0, 0, 0, nullptr, "20.0", "version...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Git hash", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Git hash value", nullptr, false, false, 0, 0, 0, 0, nullptr, "abc1234", "hash...", nullptr, 0, 0, nullptr, 0},
        // v1.56.174: BestClient extra NETMSGs toggle (visible only when preset=BestClient, see SpooferChildVisible).
        {ERowType::Toggle, "Extra NETMSGs", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.77: "Rainbow color" expandable: toggle + speed slider.
static SRow g_RainbowChildren[] = {
        {ERowType::Slider, "Speed", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.89: "Line rendering" visual expandable (cannot toggle, only expand).
// Shared line render settings used by trajectory, aimbot FOV/radius, pathfinder path, laser unfreeze.
// v1.56.108: Line rendering per-component. Component dropdown selects which
// component's Size/Color/Opacity is being edited. Layer stays global.
// v1.56.210: Added Gradient toggle + Step slider per-component. Both visible
// only when Rainbow is ON (gradient has no effect without rainbow). Speed
// slider min lowered from 0.1 to 0 (frozen rainbow is now possible).
static const char *const g_LineComponentOpts[] = {"AimBot", "TriggerBot", "Trajectory", "Laser Unfreeze", "Pathfinder", "ESP", "TAS"};
static SRow g_LineRenderingChildren[] = {
        {ERowType::Dropdown, "Component", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_LineComponentOpts, 7, 0, nullptr, 0},
        {ERowType::Input, "Line color", nullptr, false, false, 0, 0, 0, 0, nullptr, "ffbf00", "hex...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Line size", nullptr, false, false, 0.0f, 20.0f, 1.0f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Line opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 80.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Rainbow", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.0f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Gradient", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Step", nullptr, false, false, 1.0f, 180.0f, 1.0f, 15.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Layer", nullptr, false, false, 0.0f, 10.0f, 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static SRow g_SettingsRows[] = {
        {ERowType::Input, "Main Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "9d4edd", "hex...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Rainbow color", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_RainbowChildren, 1},
        {ERowType::Input, "Panel Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "161622", "hex...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "BG blackout", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "ClickGUI Scale", nullptr, false, false, 0.1f, 2.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Line rendering", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_LineRenderingChildren, 9},
        {ERowType::Expandable, "IRC", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_IrcChildren, 2},
        {ERowType::Expandable, "Spoofer", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SpooferChildren, 7},
};

// ---------- Pathfinder panel (7) ----------
// "Visuals" visual expandable.
static SRow g_VisualsChildren[] = {
        {ERowType::Toggle, "Show field", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show hooks", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show branches", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show speed", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const char *const g_PfEffortOpts[] = {"Very Low", "Low", "Medium", "High", "Ultra", "Max", "Custom"};
static const char *const g_PfAlgorithmOpts[] = {"BFS", "Beam", "RHEA", "BMCTS"};
static SRow g_PfFreedomPathChildren[] = {
        {ERowType::Slider, "Ratio", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static char g_PfDyn_Progress[96];
static char g_PfDyn_Eta[64];
static char g_PfDyn_Time[96];
static bool g_PfShowRunLabels = false;
static bool g_PfShowStatLabels = false;

static SRow g_PathfinderRows[] = {
        {ERowType::Dropdown, "Effort", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_PfEffortOpts, 7, 0, nullptr, 0},
        {ERowType::Dropdown, "Algorithm", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_PfAlgorithmOpts, 4, 0, nullptr, 0},
        {ERowType::Expandable, "Freedom Path", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_PfFreedomPathChildren, 1},
        {ERowType::Slider, "Hook angles", nullptr, false, false, 4.0f, 32.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "A* Weight", nullptr, false, false, 1.0f, 20.0f, 0.01f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Evolutions", nullptr, false, false, 1.0f, 100.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Beam width", nullptr, false, false, 8.0f, 4096.0f, 1.0f, 128.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Performance", nullptr, false, false, 1.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Freeze Support (Beta)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Visuals", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_VisualsChildren, 4},
        // v1.56.150: DoubleButton — Pathfinding (left) + Play (right, visible only when FINISHED).
        {ERowType::DoubleButton, "Pathfinding", "Paste", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "Editor Forbidden Tiles", "Clear Forbidden Tiles", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Tunnel size", nullptr, false, false, 0.0f, 10.0f, 1.0f, 2.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, "Generate tunnel", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::DoubleButton, "Editor Finish Tiles", "Clear Finish Tiles", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_PfDyn_Progress, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_PfDyn_Eta, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_PfDyn_Time, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

struct SPanel
{
        const char *pTitle;
        SRow *pRows;
        int rowCount;
};

// v1.56.89: Binds panel — input fields for binding keys to toggle commands.
// On input commit (focus lost), executes: bind <input_value> toggle <cmd> 1 0
static SRow g_BindsTasChildren[] = {
        {ERowType::Input, "Record", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Pause", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Rewind", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Forward", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
};

static SRow g_BindsEdgeChildren[] = {
        {ERowType::Input, "Buttons", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "a,b,c...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "Edge in ms", nullptr, false, false, 1.0f, 1000.0f, 10.0f, 100.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
static const char *const g_BindsModeOpts[] = {"Merge", "Override"};
static const char *const g_SdsModeOpts[] = {"Loop all", "Order targets"};
static SRow g_BindsSdsChildren[] = {
        {ERowType::Input, "Button", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_SdsModeOpts, 2, 0, nullptr, 0},
        {ERowType::Input, "Dummy IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "0,1,3,5...", nullptr, 0, 0, nullptr, 0},
};

static SRow g_BindsRows[] = {
        {ERowType::Dropdown, "Bind Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_BindsModeOpts, 2, 0, nullptr, 0},
        {ERowType::Input, "ClickGUI", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "AimBot", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "TriggerBot", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Fake Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Laser Unfreeze", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Copy Moves Filter", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Copy Moves Latency", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Trajectory", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "ESP", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Basic Avoid Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Block", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Input, "Auto Triple Fly", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "TAS", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BindsTasChildren, 4},
        {ERowType::Expandable, "Edge bind", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BindsEdgeChildren, 2},
        {ERowType::Expandable, "Smart dummy switch", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BindsSdsChildren, 3},
};

// v1.56.176: Info panel — client identity + version + FPS + social links.
// Content is dynamic: depends on Show spoofed toggle and live FPS.
// We keep static SRow array with pName pointing into g_InfoDynamicBuffers,
// which are rewritten each frame by UpdateInfoRows().
//
// Layout (top to bottom):
//   [toggle] Show spoofed
//   Client Name:    <real or spoofed>
//   Client Version: <real "X.Y" or spoofed "X.Y / id">   (real includes DDNet id)
//   Client Hash:    <GIT_SHORTREV_HASH or spoofed hash>
//   Kinetix Version: v2 beta
//   AVG FPS: <n>
//   [button] Telegram
//   [button] TikTok
//
// Real strings come from version.h: GAME_NAME, GAME_RELEASE_VERSION, GIT_SHORTREV_HASH,
// DDNET_VERSION_NUMBER. Spoofed strings come from config: m_KxSpoofClientName,
// m_KxSpoofClientVersion, m_KxSpoofGitHash, m_KxVersionSpoofId.
// "Show spoofed" toggles between showing real (off) vs spoofed (on) identity.
static char g_InfoDyn_Name[64];
static char g_InfoDyn_Version[96];
static char g_InfoDyn_Hash[64];
static char g_InfoDyn_Fps[32];

// Index of the "Show spoofed" toggle in g_InfoRows (so UpdateInfoRows can read it).
static constexpr int INFO_ROW_SHOWSPOOFED = 0;
static constexpr int INFO_ROW_NAME = 1;
static constexpr int INFO_ROW_VERSION = 2;
static constexpr int INFO_ROW_HASH = 3;
static constexpr int INFO_ROW_KINETIX = 4;
static constexpr int INFO_ROW_FPS = 5;
static constexpr int INFO_ROW_TELEGRAM = 6;
static constexpr int INFO_ROW_TIKTOK = 7;

static SRow g_InfoRows[] = {
        {ERowType::Toggle, "Show spoofed", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Name, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Version, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Hash, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, "Kinetix Version: " KINETIX_VERSION, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Fps, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, "Telegram", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, "TikTok", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

static char g_ExecDyn_Path[560];
static char g_ExecDyn_Name[160];
static char g_ExecDyn_Version[96];
static char g_ExecDyn_Author[160];
static char g_ExecDyn_Desc[320];
static char g_ExecDyn_Btn[16] = "Execute";
static char g_ExecScriptPath[1024];
static char g_ExecScriptName[128];
static bool g_ExecScriptViaStorage = false;
static bool g_ExecLoaded = false;
static bool g_ExecHasName = false;
static bool g_ExecHasVersion = false;
static bool g_ExecHasAuthor = false;
static bool g_ExecHasDesc = false;

static SRow g_CodeExecRows[] = {
        {ERowType::Button, "Pick File", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Path, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Name, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Version, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Author, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Desc, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, g_ExecDyn_Btn, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

// ---------- TAS panel (8) ----------
// Dynamic labels are rewritten every frame by UpdateTasRows().
static char g_TasDyn_Action[32] = "Play";
static char g_TasDyn_Join[40] = "Join Playground";
static char g_TasDyn_Status[192] = "";
static char g_TasDyn_SaveCreated[64] = "";
static bool g_TasFileHasBuffer = false;
static bool g_TasFilePicked = false;
        static const char *const g_TasModeOpts[] = {"Play", "Record"};
        // TAS Fake Aim: rendered-aim modes for playback (visible in Play mode).
        static const char *const g_TasFakeAimModeOpts[] = {"Real", "Smooth", "Random", "Robot", "Spin", "Lag"};
        static SRow g_TasFakeAimChildren[] = {
                {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasFakeAimModeOpts, 6, 0, nullptr, 0},
        };
        // Auto Rewind/Forward: per-tile auto actions, Record mode only. The
        // Tile dropdown selects the tile being edited; Mode and the tick
        // slider are per tile (0 ticks = disabled).
        static const char *const g_TasAutoTileOpts[] = {"Freeze", "Teleport", "Death"};
        static const char *const g_TasAutoModeOpts[] = {"Rewind", "Forward"};
        static SRow g_TasAutoRfChildren[] = {
                {ERowType::Dropdown, "Tile", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasAutoTileOpts, 3, 0, nullptr, 0},
                {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasAutoModeOpts, 2, 0, nullptr, 0},
                {ERowType::Slider, "Rewind Ticks", nullptr, false, false, 0.0f, 100.0f, 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        };
        static SRow g_TasVisualsChildren[] = {
                {ERowType::Toggle, "Show Path", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
                {ERowType::Toggle, "Show Hooks", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
                {ERowType::Toggle, "Show Speed", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
                {ERowType::Toggle, "Trajectory", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        };

static SRow g_TasRows[] = {
	{ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasModeOpts, 2, 0, nullptr, 0},
	{ERowType::DoubleButton, "Pick File", "Unpick File", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
	{ERowType::Button, g_TasDyn_Action, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
	{ERowType::Input, "Map", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "map...", nullptr, 0, 0, nullptr, 0},
	{ERowType::Input, "Name", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "name...", nullptr, 0, 0, nullptr, 0},
	{ERowType::Input, "Author", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "author...", nullptr, 0, 0, nullptr, 0},
	{ERowType::Label, g_TasDyn_SaveCreated, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
	{ERowType::Button, "Save", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
	{ERowType::Button, g_TasDyn_Join, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Slider, "TPS", nullptr, false, false, 1.0f, 50.0f, 1.0f, 50.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Pause", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Expandable, "Auto Rewind/Forward", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasAutoRfChildren, 3},
        {ERowType::Expandable, "Fake Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasFakeAimChildren, 1},
        {ERowType::Expandable, "Visuals", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasVisualsChildren, 4},
        {ERowType::Button, "Forward", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, "Rewind", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_TasDyn_Status, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

static SPanel g_Panels[NUM_PANELS] = {
        {"Functions", g_FunctionsRows, sizeof(g_FunctionsRows) / sizeof(g_FunctionsRows[0])},
        {"Advanced", g_AdvancedRows, sizeof(g_AdvancedRows) / sizeof(g_AdvancedRows[0])},
        {"Settings", g_SettingsRows, sizeof(g_SettingsRows) / sizeof(g_SettingsRows[0])},
        {"Pathfinder", g_PathfinderRows, sizeof(g_PathfinderRows) / sizeof(g_PathfinderRows[0])},
        {"Binds", g_BindsRows, sizeof(g_BindsRows) / sizeof(g_BindsRows[0])},
        {"Info", g_InfoRows, sizeof(g_InfoRows) / sizeof(g_InfoRows[0])},
        {"Code Execution (Lua)", g_CodeExecRows, sizeof(g_CodeExecRows) / sizeof(g_CodeExecRows[0])},
        {"TAS", g_TasRows, sizeof(g_TasRows) / sizeof(g_TasRows[0])},
};

// Conditional visibility for Functions panel top-level rows.
// Mirror aim X/Y visible only when Aim mode = Mirror (valueIdx=1).
// Smart aim visible only when Aim mode = Smart (valueIdx=0).
// All other rows always visible.
static bool PfRowVisible(const SRow *pRows, int rowCount, int idx)
{
        if(idx < 0 || idx >= rowCount)
                return false;
        const SRow &row = pRows[idx];
        if(str_comp(row.pName, "A* Weight") == 0 || str_comp(row.pName, "Hook angles") == 0 ||
           str_comp(row.pName, "Freeze Support (Beta)") == 0)
                return g_Config.m_KxPfEffort == 6;
        if(str_comp(row.pName, "Beam width") == 0)
                return g_Config.m_KxPfEffort == 6 && g_Config.m_KxPfAlgorithm == 1;
        if(str_comp(row.pName, "Evolutions") == 0)
                return g_Config.m_KxPfEffort == 6 && g_Config.m_KxPfAlgorithm == 2;
        if(row.pName == g_PfDyn_Progress || row.pName == g_PfDyn_Eta)
                return g_PfShowRunLabels;
        if(row.pName == g_PfDyn_Time)
                return g_PfShowStatLabels;
        return true;
}

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

static bool CodeExecRowVisible(const SRow *pRows, int rowCount, int idx)
{
        if(idx < 0 || idx >= rowCount)
                return false;
        const SRow &row = pRows[idx];
        if(row.pName == g_ExecDyn_Path)
                return g_ExecLoaded;
        if(row.pName == g_ExecDyn_Name)
                return g_ExecHasName;
        if(row.pName == g_ExecDyn_Version)
                return g_ExecHasVersion;
        if(row.pName == g_ExecDyn_Author)
                return g_ExecHasAuthor;
        if(row.pName == g_ExecDyn_Desc)
                return g_ExecHasDesc;
        return true;
}

// TAS panel: the "Fake Aim" expandable is a Play-mode tool — hidden while
// Mode = Record (its children are skipped together with the row).
static bool TasRowVisible(const SRow *pRows, int rowCount, int idx)
{
        if(idx < 0 || idx >= rowCount)
                return false;
        const SRow &row = pRows[idx];
        if(str_comp(row.pName, "Fake Aim") == 0)
        {
                for(int i = 0; i < rowCount; ++i)
                        if(str_comp(pRows[i].pName, "Mode") == 0)
                                return pRows[i].valueIdx == 0;
        }
        if(str_comp(row.pName, "Auto Rewind/Forward") == 0)
        {
                for(int i = 0; i < rowCount; ++i)
                        if(str_comp(pRows[i].pName, "Mode") == 0)
                                return pRows[i].valueIdx == 1;
        }
        if(row.pName == g_TasDyn_SaveCreated)
                return g_TasFileHasBuffer;
        if(str_comp(row.pName, "Map") == 0 || str_comp(row.pName, "Name") == 0 ||
                str_comp(row.pName, "Author") == 0 || str_comp(row.pName, "Save") == 0)
                return g_TasFileHasBuffer;
        return true;
}

static bool TasVisualsChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Trajectory") == 0)
        {
                int Mode = 0;
                for(size_t r = 0; r < sizeof(g_TasRows) / sizeof(g_TasRows[0]); ++r)
                        if(str_comp(g_TasRows[r].pName, "Mode") == 0)
                                Mode = g_TasRows[r].valueIdx;
                return Mode == 0 && g_Config.m_KxShowTrajectory != 0;
        }
        if(str_comp(row.pName, "Show Speed") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                        if(str_comp(pChildren[i].pName, "Show Path") == 0)
                                return pChildren[i].on != 0;
        }
        return true;
}

static bool ExecParseHeaderLine(const char *pLine, int LineLen, const char *pKeyName, char *pValue, int ValueSize)
{
        const int KeyLen = str_length(pKeyName);
        if(LineLen <= KeyLen || str_comp_nocase_num(pLine, pKeyName, KeyLen) != 0 || pLine[KeyLen] != ':')
                return false;
        const char *pVal = pLine + KeyLen + 1;
        int ValLen = LineLen - KeyLen - 1;
        while(ValLen > 0 && (*pVal == ' ' || *pVal == '\t'))
        {
                pVal++;
                ValLen--;
        }
        while(ValLen > 0 && (pVal[ValLen - 1] == ' ' || pVal[ValLen - 1] == '\t'))
                ValLen--;
        if(ValLen <= 0)
                return false;
        if(ValLen > ValueSize - 1)
                ValLen = ValueSize - 1;
        mem_copy(pValue, pVal, ValLen);
        pValue[ValLen] = '\0';
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

// v1.56.92: AimBot — raycast AB fields visible only when Rules=2 (Insert line + Raycast)
static bool AimBotChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        // Find Rules dropdown value
        int rulesIdx = 1; // default Insert line
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Rules") == 0)
                {
                        rulesIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        const bool isRaycastMode = (rulesIdx == 2);
        int targetsIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Targets filter") == 0)
                {
                        targetsIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        // Raycast AB fields hidden when Rules != 2
        if(str_comp(row.pName, "Use raycast angle (AB)") == 0)
                return isRaycastMode;
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
                return targetsIdx != 0;
        return true;
}

// v1.56.92: TriggerBot — same as AimBot
static bool TriggerBotChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        int targetsIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Targets filter") == 0)
                {
                        targetsIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
                return targetsIdx != 0;
        return true;
}

// v1.56.90: Dummies — "Switch to Main" hidden when already on main player.
static bool DummiesChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Switch to Main") == 0)
                return g_Config.m_ClDummy != 0; // visible only when on a dummy
        return true;
}
// v1.56.104: ESP — "ESP X"/"ESP Y" visible only when ESP Mode = 1 (Screen coordinates).
// v1.56.113: "Speed" visible only when ESP Style = 2 or 3 (Dotted line / Dotted Arrow).
static bool EspChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        int modeIdx = 0;
        int styleIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "ESP Mode") == 0)
                        modeIdx = pChildren[i].valueIdx;
                else if(str_comp(pChildren[i].pName, "ESP Style") == 0)
                        styleIdx = pChildren[i].valueIdx;
        }
        if(str_comp(row.pName, "ESP X") == 0 || str_comp(row.pName, "ESP Y") == 0)
                return modeIdx == 1; // Screen coordinates
        if(str_comp(row.pName, "Speed") == 0)
                return styleIdx == 2 || styleIdx == 3; // Dotted line / Dotted Arrow
        int targetsIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "ESP Targets filter") == 0)
                {
                        targetsIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
                return targetsIdx != 0;
        return true;
}
// v1.56.107: Spoofer — "Custom client name" visible only when preset=Custom (3).
// "Git hash value" visible only when "Git hash" toggle is on.
static bool SpooferChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Custom client name") == 0)
        {
                int presetIdx = 0;
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Client preset") == 0)
                        {
                                presetIdx = pChildren[i].valueIdx;
                                break;
                        }
                }
                return presetIdx == 3;
        }
        if(str_comp(row.pName, "Git hash value") == 0)
        {
                bool gitOn = false;
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Git hash") == 0)
                        {
                                gitOn = pChildren[i].on;
                                break;
                        }
                }
                return gitOn;
        }
        // v1.56.174: "Extra NETMSGs" visible only when preset=BestClient (2).
        if(str_comp(row.pName, "Extra NETMSGs") == 0)
        {
                int presetIdx = 0;
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Client preset") == 0)
                        {
                                presetIdx = pChildren[i].valueIdx;
                                break;
                        }
                }
                return presetIdx == 1 || presetIdx == 2;
        }
        return true;
}
// v1.56.204: Line rendering — "Speed" slider visible only when "Rainbow" toggle is on
// for the currently selected component.
// v1.56.210: "Gradient" toggle visible only with Rainbow ON (gradient has no
// effect without rainbow rotation). "Step" slider visible only when BOTH
// Rainbow AND Gradient are ON — it controls the per-segment hue increment.
static bool LineRenderingChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];

        // Step needs BOTH Rainbow ON and Gradient ON.
        if(str_comp(row.pName, "Step") == 0)
        {
                bool rainbowOn = false;
                bool gradientOn = false;
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Rainbow") == 0)
                                rainbowOn = pChildren[i].on;
                        else if(str_comp(pChildren[i].pName, "Gradient") == 0)
                                gradientOn = pChildren[i].on;
                }
                return rainbowOn && gradientOn;
        }

        // Speed and Gradient only need Rainbow ON.
        if(str_comp(row.pName, "Speed") == 0 || str_comp(row.pName, "Gradient") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Rainbow") == 0)
                                return pChildren[i].on;
                }
                return false;
        }
        return true;
}
static bool BindsChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        if(str_comp(pChildren[idx].pName, "Dummy IDs") == 0)
                return g_Config.m_KxSdsMode == 1;
        return true;
}
// v1.56.129: BAF — "Angles" + "Silent" visible only when Aim toggle is on.
static bool BafChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "FOV") == 0 || str_comp(row.pName, "Angles") == 0 || str_comp(row.pName, "Silent") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Aim") == 0)
                                return pChildren[i].on;
                }
                return false;
        }
        return true;
}
// v1.56.178: Trajectory — "Show for current" hidden when Type=Tee (no projectiles
// for the tee type — it predicts tee movement only).
// v1.56.182: "Simulate Players" and "Show for other players" are now visible for
// ALL types (previously hidden when Type!=Tee). Simulate Players controls whether
// other characters are kept in the prediction world — applies to both tee and
// weapon prediction (projectiles/lasers can hit other players when ON).
// v1.56.183: Show for other players now works for ALL types — for Tee it predicts
// other players' tee movement; for weapon types (in show-for-current mode) it
// tracks other players' in-flight projectiles/lasers too.
static bool TrajectoryChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        // Find the Type dropdown's current value.
        int typeIdx = 0; // default Tee
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Type") == 0)
                {
                        typeIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        const bool isTee = (typeIdx == 0);
        if(str_comp(row.pName, "Show for current") == 0)
                return !isTee; // only for weapon types
        // Simulate Players + Show for other players: always visible (v1.56.182)
        return true;
}

static bool TripleFlyChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Dummy ID") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Dummy mode") == 0)
                                return pChildren[i].valueIdx == 1;
                }
        }
        if(str_comp(row.pName, "Target IDs") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Target mode") == 0)
                                return pChildren[i].valueIdx == 2;
                }
        }
        return true;
}
// Constants — colors and geometry from clickgui.html (box-shadow/blur dropped
// per the spec; everything else matches the HTML aesthetic).
// ============================================================================

// v1.56.38: FIXED hex color format. ColorRGBA(unsigned col, bool alpha=true)
// decodes as 0xAARRGGBB (alpha in high byte), NOT 0xRRGGBBAA. Previous versions
// had R and B swapped and alpha in wrong position — header looked blue/cyan
// instead of purple. Now all colors use correct 0xAARRGGBB format.
// COL_PANEL_BG is now replaced by m_PanelBgRGBA (configurable per-instance).
// Kept as fallback constant for any static references.
static constexpr unsigned COL_PANEL_BG_DEFAULT = 0xff161622;
static constexpr unsigned COL_PANEL_BORDER = 0xff2a2a3e;
static constexpr unsigned COL_HEADER_BG = 0xff9d4edd; // vivid purple (A=ff,R=9d,G=4e,B=dd)
static constexpr unsigned COL_HEADER_TEXT = 0xffffffff;
static constexpr unsigned COL_ARROW = 0xffdddddd;
static constexpr unsigned COL_ROW_TEXT = 0xffd8d8e8;
static constexpr unsigned COL_ROW_TEXT_ON = 0xffffffff;
static constexpr unsigned COL_CHECK_ON = 0xffd0d0e0; // light white checkmark
static constexpr unsigned COL_CHECK_SQUARE = 0xff282834; // v1.56.61: rounded square bg under the checkmark (slightly lighter than COL_PANEL_BG=0xff161622)
static constexpr unsigned COL_EXPAND_ARROW = 0xff8a8aa8;
static constexpr unsigned COL_SUB_LIST_BG = 0x2e000000; // 18% alpha, black
static constexpr unsigned COL_SUB_LIST_BORDER = 0x4d9d4edd; // 30% alpha purple
static constexpr unsigned COL_CHILD_STRIPE = 0x80ffffff; // 50% alpha white - vertical stripe left of children bg
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
static constexpr float SLIDER_THUMB_W = 18.0f;
static constexpr float SLIDER_THUMB_H = 10.0f;
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
        case ERowType::DoubleButton:
                return ROW_H_BUTTON;
        case ERowType::Label:
                return ROW_H_TOGGLE; // v1.56.176: Label uses same height as a toggle row
        }
        return ROW_H_TOGGLE;
}

} // anonymous namespace

// ============================================================================
// CClickGui — static helpers
// ============================================================================

float CClickGui::VScreenWidth() const
{
        // Virtual width = physical width / scale, preserving the physical
        // aspect ratio. On 1920x1080 this is 1920; on any 16:9 resolution it
        // stays 1920, so the same number of panels fit the first row.
        return (float)Graphics()->ScreenWidth() / (m_UiScale > 0.0f ? m_UiScale : 1.0f);
}

float CClickGui::VScreenHeight() const
{
        return (float)Graphics()->ScreenHeight() / (m_UiScale > 0.0f ? m_UiScale : 1.0f);
}

void CClickGui::ClipEnableVirtual(float x, float y, float w, float h)
{
        // ClipEnable ignores the MapScreen transform and clamps against the
        // physical ScreenWidth()/ScreenHeight(), so the rect must be converted
        // back to physical pixels (virtual unit * m_UiScale).
        const float s = m_UiScale > 0.0f ? m_UiScale : 1.0f;
        Graphics()->ClipEnable((int)roundf(x * s), (int)roundf(y * s), (int)roundf(w * s), (int)roundf(h * s));
}

vec2 CClickGui::SlotHomePos(int slot) const
{
        // v1.56.212: dynamic relocation of overflow panels.
        // Panels are placed left-to-right in the first row. Any panel that does
        // not fit (plus Info, slot 5) is stacked below the column with the LEAST
        // accumulated height — accounting for both the first-row panel heights
        // and panels already relocated under a column. Info is no longer glued
        // below Binds: it is placed independently like every other overflow
        // panel, so a relocated panel never drags its lower neighbour with it.
        // Layout runs in the virtual 1920x1080 space (DPI-independent).
        const float screenW = VScreenWidth();
        const float startX = 60.0f;
        const float startY = 90.0f;
        const float spacingX = 30.0f; // gap between panels horizontally
        const float spacingY = FLOW_GAP; // gap between rows vertically

        // Phase 1: determine which slots fit in the first row.
        int firstRowCount = 0;
        {
                float x = startX;
                for(int s = 0; s < NUM_PANELS; ++s)
                {
                        if(s > 0 && x + PANEL_WIDTH > screenW - 10.0f)
                                break;
                        firstRowCount++;
                        x += PANEL_WIDTH + spacingX;
                }
        }

        // First-row slots: simple left-to-right layout.
        if(slot < firstRowCount)
        {
                float x = startX + slot * (PANEL_WIDTH + spacingX);
                return vec2(x, startY);
        }

        // Phase 2: overflow slots (firstRowCount..NUM_PANELS-1) stack below the
        // column with the least bottom, accounting for already-stacked panels.
        // Overflow panels are assigned tallest-first: the tallest panel that
        // still needs a column goes onto the shortest column, then the next
        // tallest, and so on. colBottom tracks each column's current bottom;
        // colCount tracks how many overflow panels a column already absorbed
        // (used as tie-breaker so panels spread evenly even while heights are
        // still animating/0).
        float colBottom[NUM_PANELS];
        int colCount[NUM_PANELS];
        for(int c = 0; c < firstRowCount; ++c)
        {
                const int panelIdx = m_aPanelOrder[c];
                colBottom[c] = startY + m_aPanelCurrentHeight[panelIdx];
                colCount[c] = 0;
        }

        bool placed[NUM_PANELS] = {};
        vec2 placedPos[NUM_PANELS];
        for(int n = 0; n < NUM_PANELS - firstRowCount; ++n)
        {
                int bestSlot = -1;
                for(int s = firstRowCount; s < NUM_PANELS; ++s)
                {
                        if(placed[s])
                                continue;
                        if(bestSlot < 0 || m_aPanelCurrentHeight[m_aPanelOrder[s]] > m_aPanelCurrentHeight[m_aPanelOrder[bestSlot]])
                                bestSlot = s;
                }
                if(bestSlot < 0)
                        break;

                // Find column with the least bottom (ties → least stacked).
                int bestCol = 0;
                for(int c = 1; c < firstRowCount; ++c)
                {
                        if(colBottom[c] < colBottom[bestCol] - 0.5f ||
                                (std::abs(colBottom[c] - colBottom[bestCol]) <= 0.5f && colCount[c] < colCount[bestCol]))
                                bestCol = c;
                }

                const int panelIdx = m_aPanelOrder[bestSlot];
                placedPos[bestSlot] = vec2(startX + bestCol * (PANEL_WIDTH + spacingX), colBottom[bestCol] + spacingY);
                placed[bestSlot] = true;
                colBottom[bestCol] += spacingY + m_aPanelCurrentHeight[panelIdx];
                colCount[bestCol]++;
        }

        if(slot >= firstRowCount && slot < NUM_PANELS && placed[slot])
                return placedPos[slot];

        return vec2(startX, startY);
}

vec2 CClickGui::PanelHomePos(int panelIdx) const
{
        // v1.56.175: home position of the slot this panel currently occupies.
        return SlotHomePos(SlotOf(panelIdx));
}

int CClickGui::SlotOf(int panelIdx) const
{
        for(int s = 0; s < NUM_PANELS; ++s)
        {
                if(m_aPanelOrder[s] == panelIdx)
                        return s;
        }
        return 0; // not found — shouldn't happen, default to slot 0
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

        lerpToward(m_DropdownAnim, m_DropdownOpen ? 1.0f : 0.0f, 8.0f);

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

// v1.56.176: refresh Info panel dynamic row text buffers.
// Reads the "Show spoofed" toggle from g_InfoRows[INFO_ROW_SHOWSPOOFED]
// and builds the client identity strings (real vs spoofed) + FPS.
// Real:    GAME_NAME / GAME_RELEASE_VERSION / GIT_SHORTREV_HASH
//          + version id DDNET_VERSION_NUMBER (shown as "X.Y / id")
// Spoofed: g_Config.m_KxSpoofClientName / m_KxSpoofClientVersion /
//          m_KxSpoofGitHash + m_KxVersionSpoofId
void CClickGui::UpdateInfoRows()
{
        // Update rolling average FPS via EMA (smoothing factor 0.05 ~ 1s window at 60fps).
        const float dt = Client()->RenderFrameTime();
        if(dt > 0.0f)
        {
                const float instantFps = 1.0f / dt;
                if(m_AvgFps <= 0.0f)
                        m_AvgFps = instantFps; // first frame: seed
                else
                        m_AvgFps = m_AvgFps + 0.05f * (instantFps - m_AvgFps);
        }

        // Show spoofed toggle: local ClickGUI state (g_InfoRows[0].on).
        const bool showSpoofed = g_InfoRows[INFO_ROW_SHOWSPOOFED].on;

        // --- Client Name ---
        if(showSpoofed)
        {
                const char *pName = g_Config.m_KxSpoofClientName[0] ? g_Config.m_KxSpoofClientName : GAME_NAME;
                str_format(g_InfoDyn_Name, sizeof(g_InfoDyn_Name), "Client Name: %s", pName);
        }
        else
        {
                str_format(g_InfoDyn_Name, sizeof(g_InfoDyn_Name), "Client Name: %s", GAME_NAME);
        }

        // --- Client Version (real includes DDNet id; spoofed includes spoof id) ---
        if(showSpoofed)
        {
                const char *pVer = g_Config.m_KxSpoofClientVersion[0] ? g_Config.m_KxSpoofClientVersion : GAME_RELEASE_VERSION;
                str_format(g_InfoDyn_Version, sizeof(g_InfoDyn_Version),
                        "Client Version: %s / %d", pVer, g_Config.m_KxVersionSpoofId);
        }
        else
        {
                str_format(g_InfoDyn_Version, sizeof(g_InfoDyn_Version),
                        "Client Version: %s / %d", GAME_RELEASE_VERSION, DDNET_VERSION_NUMBER);
        }

        // --- Client Hash ---
        if(showSpoofed)
        {
                const char *pHash = g_Config.m_KxSpoofGitHashEnabled
                        ? (g_Config.m_KxSpoofGitHash[0] ? g_Config.m_KxSpoofGitHash : "0")
                        : (GIT_SHORTREV_HASH ? GIT_SHORTREV_HASH : "none");
                str_format(g_InfoDyn_Hash, sizeof(g_InfoDyn_Hash), "Client Hash: %s", pHash);
        }
        else
        {
                str_format(g_InfoDyn_Hash, sizeof(g_InfoDyn_Hash),
                        "Client Hash: %s", GIT_SHORTREV_HASH ? GIT_SHORTREV_HASH : "none");
        }

        // --- AVG FPS ---
        str_format(g_InfoDyn_Fps, sizeof(g_InfoDyn_Fps), "AVG FPS: %d", (int)(m_AvgFps + 0.5f));
}

void CClickGui::UpdatePathfinderLabels()
{
        CBotNet &BN = GameClient()->m_BotNet;
        bool running = false;
        bool finished = false;
        float pct = 0.0f;
        int doneTiles = 0;
        int totalTiles = 0;
        double elapsedSec = 0.0;
        double etaSec = -1.0;
        double pathTimeSec = 0.0;
        double genTimeSec = 0.0;
        BN.PfGetUiStats(running, finished, pct, doneTiles, totalTiles, elapsedSec, etaSec, pathTimeSec, genTimeSec);
        g_PfShowRunLabels = running;
        g_PfShowStatLabels = finished;
        if(running)
        {
                if(totalTiles > 0)
                        str_format(g_PfDyn_Progress, sizeof(g_PfDyn_Progress),
                                "Progress: ~%d%% / 100%% | %d / ~%d", (int)(pct + 0.5f), doneTiles, totalTiles);
                else if(pct >= 0.5f)
                        str_format(g_PfDyn_Progress, sizeof(g_PfDyn_Progress),
                                "Progress: ~%d%% / 100%%", (int)(pct + 0.5f));
                else
                        str_copy(g_PfDyn_Progress, "Progress: --", sizeof(g_PfDyn_Progress));
                if(etaSec > 0.0)
                        str_format(g_PfDyn_Eta, sizeof(g_PfDyn_Eta), "ETA: ~%.1fs", etaSec);
                else
                        str_copy(g_PfDyn_Eta, "ETA: --", sizeof(g_PfDyn_Eta));
        }
        else if(finished)
        {
                str_format(g_PfDyn_Time, sizeof(g_PfDyn_Time), "Time: %.1fs | GTime: %.1fs", pathTimeSec, genTimeSec);
        }
}

void CClickGui::UpdateCodeExecRows()
{
        CCodeExec &CE = GameClient()->m_CodeExec;
        str_copy(g_ExecDyn_Btn, CE.GetState() == CCodeExec::STATE_RUNNING ? "Stop" : "Execute", sizeof(g_ExecDyn_Btn));
}

void CClickGui::UpdateTasRows()
{
        SPanel *pPanel = nullptr;
        for(int p = 0; p < NUM_PANELS; ++p)
        {
                if(str_comp(g_Panels[p].pTitle, "TAS") == 0)
                {
                        pPanel = &g_Panels[p];
                        break;
                }
        }
        if(!pPanel)
                return;

	// Push UI -> engine (the panel owns TPS / Pause / Mode).
	CTas &Tas = GameClient()->m_Tas;
	// One-time pull of persisted settings into the static rows: without this
	// the zero-initialized rows would push their defaults over the cvars
	// restored from settings.cfg on every startup.
	static bool s_TasRowsInit = false;
	if(!s_TasRowsInit)
	{
		s_TasRowsInit = true;
		for(int r = 0; r < pPanel->rowCount; ++r)
		{
			SRow &Row = pPanel->pRows[r];
			if(str_comp(Row.pName, "Auto Rewind/Forward") == 0)
			{
				Row.on = Tas.AutoRfEnabled();
				Row.expanded = Row.on;
				for(int c = 0; c < Row.childCount; ++c)
				{
					SRow &Child = Row.pChildren[c];
					if(str_comp(Child.pName, "Mode") == 0)
						Child.valueIdx = Tas.AutoRfMode();
					else if(str_comp(Child.pName, "Rewind Ticks") == 0 || str_comp(Child.pName, "Forward Ticks") == 0)
					{
						Child.value = (float)Tas.AutoRfTicks();
						Child.pName = Tas.AutoRfForward() ? "Forward Ticks" : "Rewind Ticks";
					}
				}
			}
			else if(str_comp(Row.pName, "Fake Aim") == 0)
			{
				Row.on = Tas.FakeAimEnabled();
				Row.expanded = Row.on;
				if(Row.pChildren)
					for(int c = 0; c < Row.childCount; ++c)
						if(str_comp(Row.pChildren[c].pName, "Mode") == 0)
							Row.pChildren[c].valueIdx = Tas.FakeAimMode();
			}
			else if(str_comp(Row.pName, "Visuals") == 0)
			{
				if(Row.pChildren)
					for(int c = 0; c < Row.childCount; ++c)
					{
						SRow &Child = Row.pChildren[c];
						if(str_comp(Child.pName, "Show Path") == 0)
							Child.on = g_Config.m_KxTasShowPath != 0;
						else if(str_comp(Child.pName, "Show Hooks") == 0)
							Child.on = g_Config.m_KxTasShowHooks != 0;
						else if(str_comp(Child.pName, "Show Speed") == 0)
							Child.on = g_Config.m_KxTasShowSpeed != 0;
						else if(str_comp(Child.pName, "Trajectory") == 0)
							Child.on = g_Config.m_KxTasShowTrajectory != 0;
					}
			}
		}
	}
	for(int r = 0; r < pPanel->rowCount; ++r)
        {
                SRow &Row = pPanel->pRows[r];
                if(str_comp(Row.pName, "TPS") == 0)
                        Tas.SetTps(Row.value);
				else if(str_comp(Row.pName, "Pause") == 0)
					Row.on = Tas.Paused();
			else if(str_comp(Row.pName, "Mode") == 0)
				Tas.SetMode(Row.valueIdx);
			else if(str_comp(Row.pName, "Fake Aim") == 0)
			{
				Tas.SetFakeAimEnabled(Row.on);
				if(Row.pChildren)
					for(int c = 0; c < Row.childCount; ++c)
						if(str_comp(Row.pChildren[c].pName, "Mode") == 0)
							Tas.SetFakeAimMode(Row.pChildren[c].valueIdx);
			}
			else if(str_comp(Row.pName, "Auto Rewind/Forward") == 0)
			{
				Tas.SetAutoRfEnabled(Row.on);
				if(Row.pChildren)
				{
					const int TileBefore = Tas.AutoRfTile();
					bool TileChanged = false;
					for(int c = 0; c < Row.childCount; ++c)
					{
						SRow &Child = Row.pChildren[c];
						if(str_comp(Child.pName, "Tile") == 0)
						{
							Tas.SetAutoRfTile(Child.valueIdx);
							TileChanged = Tas.AutoRfTile() != TileBefore;
						}
						else if(str_comp(Child.pName, "Mode") == 0)
						{
							if(!TileChanged)
								Tas.SetAutoRfMode(Child.valueIdx);
							Child.valueIdx = Tas.AutoRfMode();
						}
						else if(str_comp(Child.pName, "Rewind Ticks") == 0 || str_comp(Child.pName, "Forward Ticks") == 0)
						{
							if(!TileChanged)
								Tas.SetAutoRfTicks((int)Child.value);
							Child.value = (float)Tas.AutoRfTicks();
							Child.pName = Tas.AutoRfForward() ? "Forward Ticks" : "Rewind Ticks";
						}
					}
				}
			}
			else if(str_comp(Row.pName, "Visuals") == 0)
			{
				if(Row.pChildren)
					for(int c = 0; c < Row.childCount; ++c)
					{
						SRow &Child = Row.pChildren[c];
						if(str_comp(Child.pName, "Show Path") == 0)
							g_Config.m_KxTasShowPath = Child.on ? 1 : 0;
						else if(str_comp(Child.pName, "Show Hooks") == 0)
							g_Config.m_KxTasShowHooks = Child.on ? 1 : 0;
						else if(str_comp(Child.pName, "Show Speed") == 0)
							g_Config.m_KxTasShowSpeed = Child.on ? 1 : 0;
						else if(str_comp(Child.pName, "Trajectory") == 0)
							g_Config.m_KxTasShowTrajectory = Child.on ? 1 : 0;
					}
			}
        }

        // Pull engine -> UI (dynamic button labels + status line).
        if(Tas.Mode() == 1)
                str_copy(g_TasDyn_Action, Tas.IsRecording() ? "Stop" : "Record", sizeof(g_TasDyn_Action));
        else
                str_copy(g_TasDyn_Action, (Tas.IsPlaying() || Tas.IsRealPlaying()) ? "Stop" : "Play", sizeof(g_TasDyn_Action));
        str_copy(g_TasDyn_Join, Tas.IsJoined() ? "Leave Playground" : "Join Playground", sizeof(g_TasDyn_Join));

	const char *pState = "IDLE";
	if(Tas.IsRecording())
		pState = "RECORDING";
	else if(Tas.IsRealPlaying())
		pState = "EXECUTING";
	else if(Tas.IsPlaying())
		pState = "PLAYING";
	else if(Tas.IsJoined())
		pState = "LIVE";
        if(Tas.IsDead())
                pState = "DEAD";
        else if(Tas.Paused() && Tas.IsJoined())
                pState = "PAUSED";

	char aExec[32] = "";
	if(Tas.IsRealPlaying())
		str_format(aExec, sizeof(aExec), " | exec %d/%d", Tas.RealPlayIdx(), Tas.SavedCount());
	str_format(g_TasDyn_Status, sizeof(g_TasDyn_Status),
		"%s | tick %d | hist %d | rec %d | saved %d | play %d%s%s",
		pState, Tas.PlaygroundTick(), Tas.HistoryCount(), Tas.RecordCount(),
		Tas.SavedCount(), Tas.PlayIdx(), aExec, Tas.Desynced() ? " | DESYNC" : "");

	g_TasFileHasBuffer = (Tas.IsRecording() ? Tas.RecordCount() > 0 : Tas.SavedCount() > 0);
	g_TasFilePicked = Tas.FilePicked();
	char aNowCreated[64];
	str_timestamp_format(aNowCreated, sizeof(aNowCreated), "%H:%M | %d.%m.%Y");
	str_format(g_TasDyn_SaveCreated, sizeof(g_TasDyn_SaveCreated), "Created: %s", aNowCreated);
}

void CClickGui::OpenScriptPicker()
{
        if(KxPickFileAsync(KXFILEPICK_SCRIPT))
                return;
#if defined(CONF_FAMILY_WINDOWS)
        char aPath[1024];
        if(KxPickScriptFile(aPath, sizeof(aPath)))
                OnScriptPicked(aPath, false);
#else
        m_FileBrowserOpen = true;
        m_FileBrowserScroll = 0.0f;
        RefreshScriptBrowserList();
#endif
}

static int ScriptBrowserListCb(const char *pName, int IsDir, int DirType, void *pUser)
{
        (void)DirType;
        auto *pFiles = (std::vector<std::string> *)pUser;
        if(IsDir || !pName || pName[0] == '.')
                return 0;
        const int Len = str_length(pName);
        if(Len > 4 && (str_comp_nocase(pName + Len - 4, ".lua") == 0 || str_comp_nocase(pName + Len - 4, ".txt") == 0))
                pFiles->emplace_back(pName);
        return 0;
}

void CClickGui::RefreshScriptBrowserList()
{
        m_vScriptBrowserFiles.clear();
        Storage()->CreateFolder("scripts", IStorage::TYPE_SAVE);
        Storage()->ListDirectory(IStorage::TYPE_SAVE, "scripts", ScriptBrowserListCb, &m_vScriptBrowserFiles);
        std::sort(m_vScriptBrowserFiles.begin(), m_vScriptBrowserFiles.end());
}

void CClickGui::OnScriptPicked(const char *pPath, bool ViaStorage)
{
        g_ExecScriptViaStorage = ViaStorage;
        str_copy(g_ExecScriptPath, pPath, sizeof(g_ExecScriptPath));

        const char *pFileName = pPath;
        for(const char *p = pPath; *p; p++)
        {
                if(*p == '/' || *p == '\\')
                        pFileName = p + 1;
        }
        str_copy(g_ExecScriptName, pFileName, sizeof(g_ExecScriptName));

        g_ExecLoaded = false;
        g_ExecHasName = false;
        g_ExecHasVersion = false;
        g_ExecHasAuthor = false;
        g_ExecHasDesc = false;

        if(str_length(g_ExecScriptPath) > 56)
                str_format(g_ExecDyn_Path, sizeof(g_ExecDyn_Path), "File: ..%s", g_ExecScriptPath + str_length(g_ExecScriptPath) - 54);
        else
                str_format(g_ExecDyn_Path, sizeof(g_ExecDyn_Path), "File: %s", g_ExecScriptPath);

        void *pContent = nullptr;
        unsigned Len = 0;
        const bool Read = Storage()->ReadFile(g_ExecScriptPath, ViaStorage ? IStorage::TYPE_SAVE : IStorage::TYPE_ABSOLUTE, &pContent, &Len);
        if(!Read || (!pContent && Len > 0))
        {
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, "Code Exec: failed to read script file");
                return;
        }

        if(pContent)
        {
                const char *p = (const char *)pContent;
                if(Len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
                        p += 3;
                const char *pEnd = (const char *)pContent + Len;
                int LinesChecked = 0;
                while(p < pEnd && LinesChecked < 32)
                {
                        const char *pLineEnd = p;
                        while(pLineEnd < pEnd && *pLineEnd != '\n' && *pLineEnd != '\r')
                                pLineEnd++;
                        LinesChecked++;

                        const char *pKey = p;
                        int KeyLen = (int)(pLineEnd - pKey);
                        while(KeyLen > 0 && (*pKey == ' ' || *pKey == '\t'))
                        {
                                pKey++;
                                KeyLen--;
                        }
                        if(KeyLen < 2 || pKey[0] != '-' || pKey[1] != '-')
                                break;
                        pKey += 2;
                        KeyLen -= 2;
                        while(KeyLen > 0 && (*pKey == ' ' || *pKey == '\t'))
                        {
                                pKey++;
                                KeyLen--;
                        }

                        char aValue[288];
                        if(ExecParseHeaderLine(pKey, KeyLen, "NAME", aValue, sizeof(aValue)))
                        {
                                str_format(g_ExecDyn_Name, sizeof(g_ExecDyn_Name), "NAME: %s", aValue);
                                g_ExecHasName = true;
                        }
                        else if(ExecParseHeaderLine(pKey, KeyLen, "VERSION", aValue, sizeof(aValue)))
                        {
                                str_format(g_ExecDyn_Version, sizeof(g_ExecDyn_Version), "VERSION: %s", aValue);
                                g_ExecHasVersion = true;
                        }
                        else if(ExecParseHeaderLine(pKey, KeyLen, "AUTHOR", aValue, sizeof(aValue)))
                        {
                                str_format(g_ExecDyn_Author, sizeof(g_ExecDyn_Author), "AUTHOR: %s", aValue);
                                g_ExecHasAuthor = true;
                        }
                        else if(ExecParseHeaderLine(pKey, KeyLen, "DESCRIPTION", aValue, sizeof(aValue)))
                        {
                                str_format(g_ExecDyn_Desc, sizeof(g_ExecDyn_Desc), "DESCRIPTION: %s", aValue);
                                g_ExecHasDesc = true;
                        }

                        p = pLineEnd;
                        while(p < pEnd && (*p == '\n' || *p == '\r'))
                                p++;
                }
                free(pContent);
        }

        g_ExecLoaded = true;
}

void CClickGui::ExecuteSelectedScript()
{
        if(!g_ExecLoaded)
                return;
        void *pContent = nullptr;
        unsigned Len = 0;
        if(!Storage()->ReadFile(g_ExecScriptPath, g_ExecScriptViaStorage ? IStorage::TYPE_SAVE : IStorage::TYPE_ABSOLUTE, &pContent, &Len) || !pContent)
        {
                GameClient()->m_Chat.AddLine(CChat::LUA_MSG, 0, "Code Exec: failed to read script file");
                return;
        }
        static char s_aCode[262144];
        unsigned CopyLen = Len;
        if(CopyLen > sizeof(s_aCode) - 1)
                CopyLen = sizeof(s_aCode) - 1;
        mem_copy(s_aCode, pContent, CopyLen);
        s_aCode[CopyLen] = '\0';
        free(pContent);
        GameClient()->m_CodeExec.Execute(s_aCode, g_ExecScriptName);
}

void CClickGui::RenderFileBrowser(float alpha)
{
        const float screenW = VScreenWidth();
        const float screenH = VScreenHeight();
        const float boxW = 420.0f;
        const float rowH = 30.0f;
        const int fileCount = (int)m_vScriptBrowserFiles.size();
        const int visibleRows = fileCount > 0 ? (fileCount < 8 ? fileCount : 8) : 2;
        const float listH = (float)visibleRows * rowH;
        const float boxH = 52.0f + listH + 4.0f + 34.0f + 8.0f;
        const float boxX = (screenW - boxW) * 0.5f;
        const float boxY = (screenH - boxH) * 0.5f;
        const float listX = boxX + 8.0f;
        const float listY = boxY + 52.0f;
        const float cancelY = listY + listH + 4.0f;

        DrawRoundedRect(0.0f, 0.0f, screenW, screenH, 0.0f, IGraphics::CORNER_NONE,
                ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.5f));
        DrawRoundedRect(boxX - 1.0f, boxY - 1.0f, boxW + 2.0f, boxH + 2.0f, 6.0f, IGraphics::CORNER_ALL,
                ColorRGBA(0xff2a2a3e, true).WithAlpha(alpha));
        DrawRoundedRect(boxX, boxY, boxW, boxH, 6.0f, IGraphics::CORNER_ALL,
                ColorRGBA(0xff0d0d18, true).WithAlpha(alpha));

        DrawText(boxX + 12.0f, boxY + 10.0f, 18.0f, "Select script file",
                ColorRGBA(0xffffffff, true).WithAlpha(alpha));

        char aComplete[1024];
        Storage()->GetCompletePath(IStorage::TYPE_SAVE, "scripts", aComplete, sizeof(aComplete));
        const int CompleteLen = str_length(aComplete);
        if(CompleteLen > 54)
                str_format(aComplete, sizeof(aComplete), "..%s", aComplete + CompleteLen - 52);
        DrawText(boxX + 12.0f, boxY + 32.0f, 12.0f, aComplete,
                ColorRGBA(0xff8888a0, true).WithAlpha(alpha));

        if(fileCount == 0)
        {
                DrawText(listX, listY + 4.0f, 16.0f, "No .lua/.txt files found",
                        ColorRGBA(0xffd8d8e8, true).WithAlpha(alpha));
                DrawText(listX, listY + 4.0f + rowH, 14.0f, "Put your scripts into the folder above",
                        ColorRGBA(0xff8888a0, true).WithAlpha(alpha));
        }
        else
        {
                ClipEnableVirtual(listX, listY, boxW - 16.0f, listH);
                const int firstRow = (int)(m_FileBrowserScroll / rowH);
                for(int i = firstRow; i < fileCount; i++)
                {
                        const float rowY = listY + (float)i * rowH - m_FileBrowserScroll;
                        if(rowY + rowH < listY || rowY > listY + listH)
                                continue;
                        if(PointInRect(m_MousePos, listX, rowY, boxW - 16.0f, rowH))
                                DrawRoundedRect(listX, rowY, boxW - 16.0f, rowH, 2.0f, IGraphics::CORNER_ALL,
                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f));
                        DrawText(listX + 6.0f, rowY + 6.0f, 16.0f, m_vScriptBrowserFiles[i].c_str(),
                                ColorRGBA(0xffd8d8e8, true).WithAlpha(alpha));
                }
                Graphics()->ClipDisable();
        }

        const bool cancelHover = PointInRect(m_MousePos, boxX, cancelY, boxW, 34.0f);
        DrawRoundedRect(boxX, cancelY, boxW, 34.0f, 4.0f, IGraphics::CORNER_ALL,
                ColorRGBA(cancelHover ? m_AccentColorRGBA : 0xff282834u, true).WithAlpha(cancelHover ? alpha * 0.6f : alpha));
        DrawText(boxX + boxW * 0.5f, cancelY + 7.0f, 18.0f, "Cancel",
                ColorRGBA(0xffffffff, true).WithAlpha(alpha), TEXTALIGN_CENTER);
}

void CClickGui::HandleFileBrowserClick(vec2 mousePos)
{
        const float screenW = VScreenWidth();
        const float screenH = VScreenHeight();
        const float boxW = 420.0f;
        const float rowH = 30.0f;
        const int fileCount = (int)m_vScriptBrowserFiles.size();
        const int visibleRows = fileCount > 0 ? (fileCount < 8 ? fileCount : 8) : 2;
        const float listH = (float)visibleRows * rowH;
        const float boxH = 52.0f + listH + 4.0f + 34.0f + 8.0f;
        const float boxX = (screenW - boxW) * 0.5f;
        const float boxY = (screenH - boxH) * 0.5f;
        const float listX = boxX + 8.0f;
        const float listY = boxY + 52.0f;

        if(fileCount > 0 && PointInRect(mousePos, listX, listY, boxW - 16.0f, listH))
        {
                const int hit = (int)((mousePos.y - listY + m_FileBrowserScroll) / rowH);
                if(hit >= 0 && hit < fileCount)
                {
                        char aPath[1024];
                        str_format(aPath, sizeof(aPath), "scripts/%s", m_vScriptBrowserFiles[hit].c_str());
                        m_FileBrowserOpen = false;
                        OnScriptPicked(aPath, true);
                }
                return;
        }
        m_FileBrowserOpen = false;
}

// ============================================================================
// CClickGui — component lifecycle
// ============================================================================

void CClickGui::OnReset()
{
        m_AnimProgress = 0.0f;
        m_WasOpen = false;
        m_FileBrowserOpen = false;
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
                // v1.56.175: reset drag-to-sort order to identity on reset.
                m_aPanelOrder[i] = i;
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

        // v1.56.212: DPI-independent UI scale. m_UiScale = screenH / 1080 maps
        // physical pixels into the virtual 1920x1080 space used by all ClickGUI
        // geometry, so panels keep the same physical size on every resolution
        // as they have at 1920x1080.
        const float physH = (float)Graphics()->ScreenHeight();
        m_UiScale = ((physH > 0.0f) ? (physH / 1080.0f) : 1.0f) * (g_Config.m_KxClickGuiScale / 10.0f);

        // Mouse position in virtual units (physical px / scale).
        // v1.56.218: while a touch finger drives the GUI (Android), keep the
        // position set by OnTouchState — NativeMousePos is stale on touch
        // devices and would reset the cursor to 0,0 every frame.
        if(!m_TouchActive)
                m_MousePos = Input()->NativeMousePos() / m_UiScale;

        int PickKind = -1;
        char aPickPath[1024];
        if(KxPickFilePump(&PickKind, aPickPath, sizeof(aPickPath)) && aPickPath[0])
        {
                if(PickKind == KXFILEPICK_TAS)
                {
                        CTas &Tas = GameClient()->m_Tas;
                        Tas.PickFile(aPickPath);
                        if(Tas.FilePicked())
                        {
                                for(SRow &R : g_TasRows)
                                {
                                        if(str_comp(R.pName, "Map") == 0)
                                        {
                                                str_copy(R.m_aInputBuf, Tas.PickedMap(), sizeof(R.m_aInputBuf));
                                                R.m_InputInitialized = true;
                                        }
                                        else if(str_comp(R.pName, "Name") == 0)
                                        {
                                                str_copy(R.m_aInputBuf, Tas.PickedName(), sizeof(R.m_aInputBuf));
                                                R.m_InputInitialized = true;
                                        }
                                        else if(str_comp(R.pName, "Author") == 0)
                                        {
                                                str_copy(R.m_aInputBuf, Tas.PickedAuthor(), sizeof(R.m_aInputBuf));
                                                R.m_InputInitialized = true;
                                        }
                                }
                        }
                }
                else if(PickKind == KXFILEPICK_SCRIPT)
                {
                        OnScriptPicked(aPickPath, false);
                }
        }

        // Detect transition 0->1 (opening): pick fresh random offscreen positions
        // for each panel and initialize panel positions to home (if first open).
        if(isOpen && !m_WasOpen)
        {
                const float sw = VScreenWidth();
                const float sh = VScreenHeight();
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
                // (So if kx_attack / kx_random_aim / kx_show_trajectory were enabled
                // via console before opening ClickGUI, the checkboxes reflect that.)
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(str_comp(row.pName, "Block") == 0)
                                        row.on = g_Config.m_KxAttack;
                                else if(str_comp(row.pName, "Random Aim") == 0)
                                        row.on = g_Config.m_KxRandomAim;
                                else if(str_comp(row.pName, "Trajectory") == 0)
                                        row.on = g_Config.m_KxShowTrajectory != 0;
                                // Advanced → Laser unfreeze expandable is master toggle.
                                else if(str_comp(row.pName, "Laser unfreeze") == 0)
                                        row.on = g_Config.m_KxLaserUnfreeze != 0;
				else if(str_comp(row.pName, "Fake Aim") == 0 && str_comp(g_Panels[p].pTitle, "Advanced") == 0)
					row.on = g_Config.m_KxFakeAim != 0;
                                else if(str_comp(row.pName, "ESP") == 0)
                                        row.on = g_Config.m_KxEsp != 0;
                                else if(str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                        row.on = g_Config.m_KxBasicAvoidFreeze != 0;
                                // Functions panel — Fly/Copy Moves sub-section
                                else if(str_comp(row.pName, "Preserve input") == 0)
                                        row.on = g_Config.m_KxDummyHammerKeepInputs != 0;
                                else if(str_comp(row.pName, "Custom latency") == 0)
                                        row.on = g_Config.m_KxDummyHammerCustomLatency != 0;
                                else if(str_comp(row.pName, "Auto fly") == 0)
                                        row.on = g_Config.m_KxDummyHammerAuto != 0;
                                else if(str_comp(row.pName, "Auto Triple Fly") == 0)
                                        row.on = g_Config.m_KxTripleFly != 0;
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
                                // v1.56.151: Fly Ride
                                else if(str_comp(row.pName, "Fly Ride") == 0)
                                        row.on = g_Config.m_KxFlyRide != 0;
                                // Functions → Copy Moves Filter expandable is master toggle.
                                else if(str_comp(row.pName, "Copy Moves Filter") == 0)
                                        row.on = g_Config.m_KxDummyCopyMovesFilter != 0;
                                // v1.56.187: Copy Moves Latency expandable is master toggle.
                                else if(str_comp(row.pName, "Copy Moves Latency") == 0)
                                        row.on = g_Config.m_KxCopyMovesLatencyEnabled != 0;
                                // Pathfinder top-level sliders/toggles
                                else if(str_comp(row.pName, "Hook angles") == 0)
                                        row.value = (float)g_Config.m_KxPfHookAngles;
                                else if(str_comp(row.pName, "A* Weight") == 0)
                                        row.value = g_Config.m_KxPfAStarWeight / 100.0f;
                                else if(str_comp(row.pName, "Beam width") == 0)
                                        row.value = (float)g_Config.m_KxPfBeamWidth;
                                else if(str_comp(row.pName, "Evolutions") == 0)
                                        row.value = (float)g_Config.m_KxPfRheaGenerations;
                                else if(str_comp(row.pName, "Performance") == 0)
                                        row.value = (float)g_Config.m_KxPfPerf;
                                else if(str_comp(row.pName, "Effort") == 0)
                                        row.valueIdx = g_Config.m_KxPfEffort;
                                else if(str_comp(row.pName, "Algorithm") == 0)
                                        row.valueIdx = g_Config.m_KxPfAlgorithm;
                                else if(str_comp(row.pName, "Freeze Support (Beta)") == 0)
                                        row.on = g_Config.m_KxPfFreezeSupport != 0;
                                else if(str_comp(row.pName, "Freedom Path") == 0)
                                        row.on = g_Config.m_KxPfFreedom != 0;
                                else if(str_comp(row.pName, "Tunnel size") == 0)
                                        row.value = (float)g_Config.m_KxPfTunnelSize;
                                // Settings → IRC/Spoofer top-level expandables are master toggles.
                                else if(str_comp(row.pName, "IRC") == 0)
                                        row.on = g_Config.m_KxIrcEnabled != 0;
                                else if(str_comp(row.pName, "Spoofer") == 0)
                                        row.on = (g_Config.m_KxVersionSpoof != 0 || g_Config.m_KxSpoofClientStr != 0);
                                // v1.56.199: Binds panel sync moved to every-frame loop (see below).
                                // Sync children of expandable rows
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
        continue;
                                        if(str_comp(child.pName, "Latency (ms)") == 0 && str_comp(row.pName, "Custom latency") == 0)
                                                child.value = (float)g_Config.m_KxDummyHammerLatencyMs;
                                        else if(str_comp(child.pName, "Latency (ms)") == 0 && str_comp(row.pName, "Copy Moves Latency") == 0)
                                                child.value = (float)g_Config.m_KxCopyMovesLatency;
                                        else if(str_comp(child.pName, "Ratio") == 0 && str_comp(row.pName, "Freedom Path") == 0)
                                                child.value = (float)g_Config.m_KxPfFreedomRatio / 10.0f;
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
                                        else if(str_comp(child.pName, "Filter: Weapon") == 0)
                                                child.on = g_Config.m_KxDummyCopyMovesFilterWeapon != 0;
                                        // v1.56.189: Auto Fly children — Max Distance slider + Predict toggle.
                                        else if(str_comp(child.pName, "Max Distance") == 0 && str_comp(row.pName, "Auto fly") == 0)
                                                child.value = (float)g_Config.m_KxDummyHammerAutoMaxDist;
                                        else if(str_comp(child.pName, "Predict") == 0 && str_comp(row.pName, "Auto fly") == 0)
                                                child.on = g_Config.m_KxDummyHammerAutoPredict != 0;
                                        else if(str_comp(child.pName, "Dummy mode") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                                child.valueIdx = g_Config.m_KxTripleFlyDummyMode;
                                        else if(str_comp(child.pName, "Target mode") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                                child.valueIdx = g_Config.m_KxTripleFlyTargetMode;
                                        else if(str_comp(child.pName, "Trigger radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                                child.value = (float)g_Config.m_KxTripleFlyTriggerRadius;
                                        else if(str_comp(child.pName, "Radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                                child.value = (float)g_Config.m_KxTripleFlyRadius;
                                        // Settings → IRC children
                                        else if(str_comp(child.pName, "Reveal join") == 0)
                                                child.on = g_Config.m_KxIrcRevealJoin != 0;
                                        else if(str_comp(child.pName, "Nameplate tag") == 0)
                                                child.on = g_Config.m_KxIrcNameplateTag != 0;
                                        // v1.56.112: Line rendering sync moved to dedicated block below
                                        // (per-component, reads g_Config.m_KxLineComponent every frame).
                                        // v1.56.178: Trajectory children now sync from CTrajectory per-type settings.
                                        // The selected type is read from the Trajectory.Type dropdown value (which
                                        // is set by the dropdown handler below).
                                        else if(str_comp(row.pName, "Trajectory") == 0)
                                        {
                                                CTrajectory &traj = GameClient()->m_Trajectory;
                                                int sel = traj.m_SelectedType;
                                                if(sel < 0 || sel >= CTrajectory::NUM_TRAJ_TYPES)
                                                        sel = 0;
                                                const CTrajectory::STypeSettings &ts = traj.m_aTypes[sel];
                                                if(str_comp(child.pName, "Type") == 0)
                                                        child.valueIdx = sel;
                                                else if(str_comp(child.pName, "Show") == 0)
                                                        child.on = ts.m_Show;
                                                else if(str_comp(child.pName, "Prediction Ticks") == 0)
                                                        child.value = (float)ts.m_PredictionTicks;
                                                else if(str_comp(child.pName, "Alpha Gradient") == 0)
                                                        child.on = ts.m_AlphaGradient;
                                                else if(str_comp(child.pName, "Simulate Players") == 0)
                                                        child.on = ts.m_SimulatePlayers;
                                                else if(str_comp(child.pName, "Show for other players") == 0)
                                                        child.on = ts.m_ShowForOtherPlayers;
                                                else if(str_comp(child.pName, "Show for current") == 0)
                                                        child.on = ts.m_ShowForCurrent;
                                        }
                                        // Advanced → Laser unfreeze children
                                        else if(str_comp(child.pName, "Auto laser") == 0)
                                                child.on = g_Config.m_KxLaserUnfreezeAuto != 0;
                                        else if(str_comp(child.pName, "FOV") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeFov;
                                        else if(str_comp(child.pName, "Angles") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeAngles;
                                        else if(str_comp(child.pName, "Ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeTicks;
                                        else if(str_comp(child.pName, "Trigger ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                                child.value = (float)g_Config.m_KxLaserUnfreezeTriggerTicks;
                                        else if(str_comp(child.pName, "Silent") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                                child.on = g_Config.m_KxLaserUnfreezeSilent != 0;
                                        else if(str_comp(child.pName, "Show attempt") == 0)
                                                child.on = g_Config.m_KxLaserUnfreezeShowAttempt != 0;
                                        // Basic Avoid Freeze children
                                        else if(str_comp(child.pName, "Freeze") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafAvoidFreeze != 0;
                                        else if(str_comp(child.pName, "Teleport") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafAvoidTeleport != 0;
                                        else if(str_comp(child.pName, "Death") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafAvoidDeath != 0;
                                        else if(str_comp(child.pName, "Direction") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafDirection != 0;
                                        else if(str_comp(child.pName, "Jump") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafJump != 0;
                                        else if(str_comp(child.pName, "Hook") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafHook != 0;
                                        else if(str_comp(child.pName, "Aim") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafAim != 0;
                                        else if(str_comp(child.pName, "FOV") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxBafFov;
                                        else if(str_comp(child.pName, "Angles") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxBafAngles;
                                        else if(str_comp(child.pName, "Silent") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxBafSilent != 0;
                                        else if(str_comp(child.pName, "Ticks") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxBafTicks;
                                        else if(str_comp(child.pName, "Trigger ticks") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxBafTriggerTicks;
                                        // Fake Aim children
					else if(str_comp(child.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0 && str_comp(g_Panels[p].pTitle, "Advanced") == 0)
						child.valueIdx = g_Config.m_KxFakeAimMode;
                                        else if(str_comp(child.pName, "Speed") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.value = (float)g_Config.m_KxFakeAimSpeed;
                                        else if(str_comp(child.pName, "Show for me") == 0)
                                                child.on = g_Config.m_KxFakeAimShowForMe != 0;
                                        // ESP children
                                        else if(str_comp(child.pName, "ESP Team") == 0)
                                                child.valueIdx = g_Config.m_KxEspTeamFilter;
                                        else if(str_comp(child.pName, "ESP Friend") == 0)
                                                child.valueIdx = g_Config.m_KxEspFriendFilter;
                                        else if(str_comp(child.pName, "ESP Dummy") == 0)
                                                child.valueIdx = g_Config.m_KxEspDummyFilter;
                                        else if(str_comp(child.pName, "ESP Clanmate") == 0)
                                                child.valueIdx = g_Config.m_KxEspClanmateFilter;
                                        else if(str_comp(child.pName, "ESP Targets filter") == 0)
                                                child.valueIdx = g_Config.m_KxEspTargetsFilterMode;
                                        else if((str_comp(child.pName, "Targets IDs") == 0 || str_comp(child.pName, "Untargets IDs") == 0) && str_comp(row.pName, "ESP") == 0)
                                        {
                                                child.pName = (g_Config.m_KxEspTargetsFilterMode == 2) ? "Untargets IDs" : "Targets IDs";
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxEspTargetsFilter, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "ESP Freeze") == 0)
                                                child.valueIdx = g_Config.m_KxEspFreezeFilter;
                                        else if(str_comp(child.pName, "ESP Mode") == 0)
                                                child.valueIdx = g_Config.m_KxEspMode;
                                        else if(str_comp(child.pName, "ESP Style") == 0)
                                                child.valueIdx = g_Config.m_KxEspStyle;
                                        else if(str_comp(child.pName, "Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.value = (float)g_Config.m_KxEspSpeed;
                                        else if(str_comp(child.pName, "ESP X") == 0)
                                                                                {
                                                                                        child.value = (float)g_Config.m_KxEspScreenX;
                                                                                        child.max = (float)Graphics()->ScreenWidth();
                                                                                }
                                        else if(str_comp(child.pName, "ESP Y") == 0)
                                                                                {
                                                                                        child.value = (float)g_Config.m_KxEspScreenY;
                                                                                        child.max = (float)Graphics()->ScreenHeight();
                                                                                }
                                        // Settings → Spoofer children
                                        else if(str_comp(child.pName, "Version ID") == 0)
                                                child.value = (float)g_Config.m_KxVersionSpoofId;
                                        else if(str_comp(child.pName, "Client preset") == 0)
                                                child.valueIdx = g_Config.m_KxSpoofClientPreset;
                                        else if(str_comp(child.pName, "Custom client name") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxSpoofClientName, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Client version") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxSpoofClientVersion, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Git hash") == 0)
                                                child.on = g_Config.m_KxSpoofGitHashEnabled != 0;
                                        else if(str_comp(child.pName, "Git hash value") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxSpoofGitHash, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        // v1.56.174: BestClient "Extra NETMSGs" checkbox sync.
                                        else if(str_comp(child.pName, "Extra NETMSGs") == 0)
                                                child.on = (g_Config.m_KxSpoofClientPreset == 1 ? g_Config.m_KxSpoofTClientExtraNetmsgs : g_Config.m_KxSpoofBestClientExtraNetmsgs) != 0;
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
                                        row.on = g_Config.m_KxAimBot;
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
                                                else if(str_comp(ch.pName, "Use raycast (Show)") == 0)
                                                        ch.on = s.m_UseRaycastShow;
                                                else if(str_comp(ch.pName, "Rules") == 0)
                                                        ch.valueIdx = s.m_Rules;
                                                else if(str_comp(ch.pName, "Use raycast angle (AB)") == 0)
                                                        ch.on = s.m_UseAngle;
                                                else if(str_comp(ch.pName, "AB Aim Mode") == 0)
                                                        ch.valueIdx = s.m_AimMode;
                                                else if(str_comp(ch.pName, "AB Team") == 0)
                                                        ch.valueIdx = s.m_TeamFilter;
                                                else if(str_comp(ch.pName, "AB Friend") == 0)
                                                        ch.valueIdx = s.m_FriendFilter;
                                                else if(str_comp(ch.pName, "Predict") == 0)
                                                        ch.on = s.m_Predict;
                                                else if(str_comp(ch.pName, "AB Dummy") == 0)
                                                        ch.valueIdx = s.m_DummyFilter;
                                                else if(str_comp(ch.pName, "Clanmate filter") == 0)
                                                        ch.valueIdx = s.m_ClanmateFilter;
                                                else if(str_comp(ch.pName, "AB Freeze") == 0)
                                                        ch.valueIdx = s.m_FreezeFilter;
                                                else if(str_comp(ch.pName, "AB Priority") == 0)
                                                        ch.valueIdx = s.m_Priority;
                                                else if(str_comp(ch.pName, "Targets filter") == 0)
                                                        ch.valueIdx = s.m_TargetsFilter;
                                                else if(str_comp(ch.pName, "Targets IDs") == 0 || str_comp(ch.pName, "Untargets IDs") == 0)
                                                {
                                                        ch.pName = (s.m_TargetsFilter == 2) ? "Untargets IDs" : "Targets IDs";
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, s.m_aTargetsFilter, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                        }
                                }
                                else if(str_comp(row.pName, "TriggerBot") == 0)
                                {
                                        row.on = g_Config.m_KxTriggerBot;
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
                                                else if(str_comp(ch.pName, "TB Use raycast (Show)") == 0)
                                                        ch.on = s.m_UseRaycastShow;
                                                else if(str_comp(ch.pName, "TB Rules") == 0)
                                                        ch.valueIdx = s.m_Rules;
                                                else if(str_comp(ch.pName, "TB Latency") == 0)
                                                        ch.value = (float)s.m_RandomLatency;
                                                else if(str_comp(ch.pName, "TB Team") == 0)
                                                        ch.valueIdx = s.m_TeamFilter;
                                                else if(str_comp(ch.pName, "TB Friend") == 0)
                                                        ch.valueIdx = s.m_FriendFilter;
                                                else if(str_comp(ch.pName, "TB Dummy") == 0)
                                                        ch.valueIdx = s.m_DummyFilter;
                                                else if(str_comp(ch.pName, "Clanmate filter") == 0)
                                                        ch.valueIdx = s.m_ClanmateFilter;
                                                else if(str_comp(ch.pName, "TB Freeze") == 0)
                                                        ch.valueIdx = s.m_FreezeFilter;
                                                else if(str_comp(ch.pName, "TB Priority") == 0)
                                                        ch.valueIdx = s.m_Priority;
                                                else if(str_comp(ch.pName, "Targets filter") == 0)
                                                        ch.valueIdx = s.m_TargetsFilter;
                                                else if(str_comp(ch.pName, "Targets IDs") == 0 || str_comp(ch.pName, "Untargets IDs") == 0)
                                                {
                                                        ch.pName = (s.m_TargetsFilter == 2) ? "Untargets IDs" : "Targets IDs";
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, s.m_aTargetsFilter, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                        }
                                }
                        }
                }
        }
        // v1.56.174: per-frame sync of Spoofer UI from config.
        // Runs every frame while open (not just on open) so switching the
        // "Client preset" dropdown immediately updates name/version/version_id/
        // git_hash/extra-netmsgs fields. Mirrors the AimBot/TriggerBot per-frame
        // sync pattern (see v1.56.83 above).
        if(isOpen)
        {
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(row.type != ERowType::Expandable)
                                continue;
                                if(str_comp(row.pName, "Spoofer") == 0)
                                {
                                        row.on = (g_Config.m_KxVersionSpoof != 0 || g_Config.m_KxSpoofClientStr != 0);
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Version ID") == 0)
                                                        ch.value = (float)g_Config.m_KxVersionSpoofId;
                                                else if(str_comp(ch.pName, "Client preset") == 0)
                                                        ch.valueIdx = g_Config.m_KxSpoofClientPreset;
                                                else if(str_comp(ch.pName, "Custom client name") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, g_Config.m_KxSpoofClientName, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Client version") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, g_Config.m_KxSpoofClientVersion, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Git hash") == 0)
                                                        ch.on = g_Config.m_KxSpoofGitHashEnabled != 0;
                                                else if(str_comp(ch.pName, "Git hash value") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, g_Config.m_KxSpoofGitHash, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Extra NETMSGs") == 0)
                                                        ch.on = (g_Config.m_KxSpoofClientPreset == 1 ? g_Config.m_KxSpoofTClientExtraNetmsgs : g_Config.m_KxSpoofBestClientExtraNetmsgs) != 0;
                                        }
                                }
                        }
                }
        }
        // v1.56.181: per-frame sync of Trajectory UI from CTrajectory state.
        // Runs every frame while open (not just on open) so switching the Type
        // dropdown immediately updates all per-type fields (Show / Prediction
        // Ticks / Alpha Gradient / Simulate Players / Show for other players /
        // Show for current). Mirrors the AimBot per-frame sync pattern (v1.56.83).
        // ROOT CAUSE of bug #5: previously this sync only ran inside the
        // on-open block (if(isOpen && !m_WasOpen)), so changing the Type dropdown
        // at runtime did NOT refresh the child rows — they kept showing the
        // previous type's values until the ClickGUI was closed and reopened.
        if(isOpen)
        {
                CTrajectory &traj = GameClient()->m_Trajectory;
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(row.type != ERowType::Expandable)
                                        continue;
                                if(str_comp(row.pName, "Trajectory") == 0)
                                {
                                        int sel = traj.m_SelectedType;
                                        if(sel < 0 || sel >= CTrajectory::NUM_TRAJ_TYPES)
                                                sel = 0;
                                        const CTrajectory::STypeSettings &ts = traj.m_aTypes[sel];
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Type") == 0)
                                                        ch.valueIdx = sel;
                                                else if(str_comp(ch.pName, "Show") == 0)
                                                        ch.on = ts.m_Show;
                                                else if(str_comp(ch.pName, "Prediction Ticks") == 0)
                                                        ch.value = (float)ts.m_PredictionTicks;
                                                else if(str_comp(ch.pName, "Alpha Gradient") == 0)
                                                        ch.on = ts.m_AlphaGradient;
                                                else if(str_comp(ch.pName, "Simulate Players") == 0)
                                                        ch.on = ts.m_SimulatePlayers;
                                                else if(str_comp(ch.pName, "Show for other players") == 0)
                                                        ch.on = ts.m_ShowForOtherPlayers;
                                                else if(str_comp(ch.pName, "Show for current") == 0)
                                                        ch.on = ts.m_ShowForCurrent;
                                        }
                                }
                        }
                }
        }
        // v1.56.90: per-frame sync of Block + Dummies UI from CBotNet state.
        if(isOpen)
        {
                CBotNet &bn = GameClient()->m_BotNet;
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(row.type != ERowType::Expandable)
                                        continue;
                                if(str_comp(row.pName, "Block") == 0)
                                {
                                        row.on = g_Config.m_KxAttack;
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Attack Main") == 0) ch.on = g_Config.m_KxAtkMain;
                                                else if(str_comp(ch.pName, "Auto Main") == 0) ch.on = g_Config.m_KxAutoMain;
                                                else if(str_comp(ch.pName, "Main ID") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char b[16]; str_format(b, sizeof(b), "%d", g_Config.m_KxMain);
                                                                str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Target IDs") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, bn.m_aTargetIDsStr, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Rescue IDs") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, bn.m_aRescueIDsStr, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "All Target") == 0) ch.on = g_Config.m_KxTargetAll;
                                                else if(str_comp(ch.pName, "Auto Aim") == 0) ch.on = g_Config.m_KxAutoAim;
                                                else if(str_comp(ch.pName, "Auto Fire") == 0) ch.on = g_Config.m_KxAutoFire;
                                                else if(str_comp(ch.pName, "Auto Hook") == 0) ch.on = g_Config.m_KxAutoHook;
                                                else if(str_comp(ch.pName, "Auto Hammer") == 0) ch.on = g_Config.m_KxHammer;
                                                else if(str_comp(ch.pName, "Move Enabled") == 0) ch.on = g_Config.m_KxMove;
                                                else if(str_comp(ch.pName, "Stand Enabled") == 0) ch.on = g_Config.m_KxStand;
                                                else if(str_comp(ch.pName, "Stand On X Only") == 0) ch.on = g_Config.m_KxStandOnX;
                                                else if(str_comp(ch.pName, "Smart Detect") == 0) ch.on = g_Config.m_KxSmartDetect;
                                                else if(str_comp(ch.pName, "Smart Rescue") == 0) ch.on = g_Config.m_KxSmartRescue;
                                                else if(str_comp(ch.pName, "Rescue Frozen") == 0) ch.on = g_Config.m_KxRescue;
                                                else if(str_comp(ch.pName, "Rescue All") == 0) ch.on = g_Config.m_KxRescueAll;
                                                else if(str_comp(ch.pName, "Kill On Freeze") == 0) ch.on = g_Config.m_KxKillFrz;
                                                else if(str_comp(ch.pName, "Avoid Freeze") == 0) ch.on = g_Config.m_KxAvoidFreeze;
                                                else if(str_comp(ch.pName, "Avoid Freeze Radius") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", g_Config.m_KxAvoidFreezeRadius);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Laser Rescue") == 0) ch.on = g_Config.m_KxLaserRescue;
                                                else if(str_comp(ch.pName, "Fire Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxFireDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Hook Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxHookDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Rescue Radius") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxRescueRadius);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Target Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxTargetDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Main Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxMainDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Stand Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxStandDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Main Stand Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxMainStandDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Laser Rescue Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxLaserRescueDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Pathfinder Enabled") == 0) ch.on = g_Config.m_KxAtkPathfinder;
                                                else if(str_comp(ch.pName, "Simulate Players") == 0) ch.on = g_Config.m_KxPfSimulatePlayers;
                                                else if(str_comp(ch.pName, "SPS (Push)") == 0) ch.on = (g_Config.m_KxAtkPathfinderSps != 0);
                                                else if(str_comp(ch.pName, "Simulate Score") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxPfSimulateScore);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Rays") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", g_Config.m_KxAtkPathfinderRays);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", g_Config.m_KxAtkPathfinderRaysDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                else if(str_comp(ch.pName, "Fix Snap") == 0) ch.on = g_Config.m_KxAtkPathfinderSnap;
                                                else if(str_comp(ch.pName, "Hook Enabled") == 0) ch.on = g_Config.m_KxPfHook;
                                        }
                                }
                                else if(str_comp(row.pName, "Auto Triple Fly") == 0)
                                {
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Dummy ID") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char b[16]; str_format(b, sizeof(b), "%d", g_Config.m_KxTripleFlyDummyId);
                                                                str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Target IDs") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, g_Config.m_KxTripleFlyTargetIds, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                        }
                                }
                                // Dummies: hide "Switch to Main" if already on main player.
                                else if(str_comp(row.pName, "Dummies") == 0)
                                {
                                        // Update button labels + visibility dynamically.
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Connect Dummy") == 0)
                                                {
                                                        // Label depends on connection state.
                                                        ch.on = GameClient()->Client()->AnyDummyConnected();
                                                }
                                                else if(str_comp(ch.pName, "Switch to Main") == 0)
                                                {
                                                        // Hide if already on main (g_Config.m_ClDummy == 0).
                                                        ch.on = (g_Config.m_ClDummy != 0);
                                                }
                                                else if(ch.pName && ch.pName[0] == 'D' && ch.pName[1] >= '1' && ch.pName[1] <= '7')
                                                {
                                                        int d = ch.pName[1] - '0';
                                                        if(strstr(ch.pName, "Connect"))
                                                                ch.on = GameClient()->Client()->DummyConnected(d);
                                                        else if(strstr(ch.pName, "Switch"))
                                                                ch.on = (g_Config.m_ClDummy == d);
                                                }
                                                // v1.56.90: DoubleButton (Connect+Switch+Send) — ch.on=connected, ch.value=active, ch.expanded=send
                                                else if(ch.type == ERowType::DoubleButton && ch.pName && ch.pName[0] == 'D')
                                                {
                                                        int d = ch.pName[1] - '0';
                                                        ch.on = GameClient()->Client()->DummyConnected(d);
                                                        ch.value = (g_Config.m_ClDummy == d) ? 1.0f : 0.0f;
                                                        if(d >= 1 && d < 8)
                                                                ch.expanded = m_aDummySendEnabled[d];
                                                }
                                        }
                                }
                        }
                }
        }
        // v1.56.199: sync Main Color, Panel Color, BG blackout, Rainbow state.
        for(int p = 0; p < NUM_PANELS; ++p)
        {
                for(int r = 0; r < g_Panels[p].rowCount; ++r)
                {
                        SRow &row = g_Panels[p].pRows[r];
                        if(str_comp(row.pName, "Main Color") == 0 && row.type == ERowType::Input)
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
                        // v1.56.199: sync Panel Color input from m_PanelBgRGBA.
                        else if(str_comp(row.pName, "Panel Color") == 0 && row.type == ERowType::Input)
                        {
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r)
                                {
                                        char hex[8];
                                        str_format(hex, sizeof(hex), "%06x", m_PanelBgRGBA & 0xffffff);
                                        str_copy(row.m_aInputBuf, hex, sizeof(row.m_aInputBuf));
                                        row.m_InputInitialized = true;
                                }
                        }
                        // v1.56.199: sync BG blackout slider from m_BgBlackout.
                        else if(str_comp(row.pName, "BG blackout") == 0 && row.type == ERowType::Slider)
                                row.value = (float)m_BgBlackout;
                        else if(str_comp(row.pName, "ClickGUI Scale") == 0 && row.type == ERowType::Slider)
                        {
                                if(!(m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == -1))
                                        row.value = g_Config.m_KxClickGuiScale / 10.0f;
                        }
                        else if(p == 4 && row.type == ERowType::Dropdown && str_comp(row.pName, "Bind Mode") == 0)
                                row.valueIdx = g_Config.m_KxBindsMode;
                        // v1.56.199: Binds panel — sync input fields with actual bound keys
                        // every frame (not just on open), so console bind changes appear
                        // immediately while ClickGUI is open.
                        else if(p == 4 && row.type == ERowType::Input)
                        {
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r)
                                {
                                        const char *pCmd = nullptr;
                                        if(str_comp(row.pName, "ClickGUI") == 0) pCmd = "toggle cl_clickgui 1 0";
                                        else if(str_comp(row.pName, "AimBot") == 0) pCmd = "toggle kx_aimbot 1 0";
                                        else if(str_comp(row.pName, "TriggerBot") == 0) pCmd = "toggle kx_triggerbot 1 0";
                                        else if(str_comp(row.pName, "Fake Aim") == 0) pCmd = "toggle kx_fake_aim 1 0";
                                        else if(str_comp(row.pName, "Laser Unfreeze") == 0) pCmd = "toggle kx_laser_unfreeze 1 0";
                                        else if(str_comp(row.pName, "Copy Moves Filter") == 0) pCmd = "toggle kx_dummy_copy_moves_filter 1 0";
                                        else if(str_comp(row.pName, "Copy Moves Latency") == 0) pCmd = "toggle kx_copy_moves_latency_enabled 1 0";
                                        else if(str_comp(row.pName, "Trajectory") == 0) pCmd = "toggle kx_show_trajectory 1 0";
                                        else if(str_comp(row.pName, "ESP") == 0) pCmd = "toggle kx_esp 1 0";
                                        else if(str_comp(row.pName, "Basic Avoid Freeze") == 0) pCmd = "toggle kx_basic_avoid_freeze 1 0";
                                        else if(str_comp(row.pName, "Block") == 0) pCmd = "toggle kx_attack 1 0";
                                        else if(str_comp(row.pName, "Auto Triple Fly") == 0) pCmd = "toggle kx_triple_fly 1 0";
                                        if(pCmd)
                                        {
                                                char aKey[64];
                                                if(g_Config.m_KxBindsMode == 0)
                                                        GameClient()->m_Binds.GetKeySegment(pCmd, aKey, sizeof(aKey));
                                                else
                                                        GameClient()->m_Binds.GetKey(pCmd, aKey, sizeof(aKey));
                                                if(aKey[0])
                                                        str_copy(row.m_aInputBuf, aKey, sizeof(row.m_aInputBuf));
                                                else
                                                        str_copy(row.m_aInputBuf, "", sizeof(row.m_aInputBuf));
                                                row.m_InputInitialized = true;
                                        }
                                }
                        }
                        else if(p == 4 && row.type == ERowType::Expandable && str_comp(row.pName, "TAS") == 0)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const char *pCmd = nullptr;
                                        if(str_comp(child.pName, "Record") == 0) pCmd = "toggle kx_tas_record 1 0";
                                        else if(str_comp(child.pName, "Pause") == 0) pCmd = "toggle kx_tas_pause 1 0";
                                        else if(str_comp(child.pName, "Rewind") == 0) pCmd = "kx_tas_rewind";
                                        else if(str_comp(child.pName, "Forward") == 0) pCmd = "kx_tas_forward";
                                        if(pCmd && (!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c))
                                        {
                                                char aKey[64];
                                                if(g_Config.m_KxBindsMode == 0)
                                                        GameClient()->m_Binds.GetKeySegment(pCmd, aKey, sizeof(aKey));
                                                else
                                                        GameClient()->m_Binds.GetKey(pCmd, aKey, sizeof(aKey));
                                                if(aKey[0])
                                                        str_copy(child.m_aInputBuf, aKey, sizeof(child.m_aInputBuf));
                                                else
                                                        str_copy(child.m_aInputBuf, "", sizeof(child.m_aInputBuf));
                                                child.m_InputInitialized = true;
                                        }
                                }
                        }
                        else if(p == 4 && row.type == ERowType::Expandable && str_comp(row.pName, "Edge bind") == 0)
                        {
                                row.on = g_Config.m_KxEdgeBind != 0;
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        if(str_comp(child.pName, "Buttons") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxEdgeBindButtons, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Edge in ms") == 0)
                                        {
                                                if(!(m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c))
                                                        child.value = (float)g_Config.m_KxEdgeBindMs;
                                        }
                                }
                        }
                        else if(p == 4 && row.type == ERowType::Expandable && str_comp(row.pName, "Smart dummy switch") == 0)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        if(str_comp(child.pName, "Button") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char aKey[64];
                                                        if(g_Config.m_KxBindsMode == 0)
                                                                GameClient()->m_Binds.GetKeySegment("kx_dummy_switch", aKey, sizeof(aKey));
                                                        else
                                                                GameClient()->m_Binds.GetKey("kx_dummy_switch", aKey, sizeof(aKey));
                                                        if(aKey[0])
                                                                str_copy(child.m_aInputBuf, aKey, sizeof(child.m_aInputBuf));
                                                        else
                                                                str_copy(child.m_aInputBuf, "", sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Mode") == 0)
                                                child.valueIdx = g_Config.m_KxSdsMode;
                                        else if(str_comp(child.pName, "Dummy IDs") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxSdsDummyIds, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                }
                        }
                        // v1.56.112: Line rendering dedicated sync (per-component).
                        // Reads g_Config.m_KxLineComponent every frame so changing
                        // the Component dropdown updates color/size/opacity immediately.
                        else if(str_comp(row.pName, "Line rendering") == 0 && row.type == ERowType::Expandable)
                        {
                                int compIdx = g_Config.m_KxLineComponent;
                                if(compIdx < 0 || compIdx >= KX_LINE_COUNT)
                                        compIdx = 0;
                                EKxLineComponent comp = (EKxLineComponent)compIdx;
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        if(str_comp(child.pName, "Component") == 0)
                                                child.valueIdx = compIdx;
                                        else if(str_comp(child.pName, "Line color") == 0)
                                        {
                                                // Update unless this field is being actively edited.
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char hex[8];
                                                        str_format(hex, sizeof(hex), "%06x", KxLineColor(comp) & 0xffffff);
                                                        str_copy(child.m_aInputBuf, hex, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Line size") == 0)
                                                child.value = (float)KxLineSize(comp);
                                        else if(str_comp(child.pName, "Line opacity") == 0)
                                                child.value = (float)(KxLineAlpha(comp) * 100.0f);
                                        // v1.56.201: per-component rainbow sync
                                        else if(str_comp(child.pName, "Rainbow") == 0)
                                        {
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: child.on = g_Config.m_KxLineAimBotRainbow != 0; break;
                                                case KX_LINE_TRIGGERBOT: child.on = g_Config.m_KxLineTriggerBotRainbow != 0; break;
                                                case KX_LINE_TRAJECTORY: child.on = g_Config.m_KxLineTrajectoryRainbow != 0; break;
                                                case KX_LINE_LASER_UNFREEZE: child.on = g_Config.m_KxLineLaserUnfreezeRainbow != 0; break;
                                                case KX_LINE_PATHFINDER: child.on = g_Config.m_KxLinePathfinderRainbow != 0; break;
                                                case KX_LINE_ESP: child.on = g_Config.m_KxLineEspRainbow != 0; break;
                                                case KX_LINE_TAS: child.on = g_Config.m_KxLineTasRainbow != 0; break;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Speed") == 0)
                                        {
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: child.value = (float)g_Config.m_KxLineAimBotRainbowSpeed / 10.0f; break;
                                                case KX_LINE_TRIGGERBOT: child.value = (float)g_Config.m_KxLineTriggerBotRainbowSpeed / 10.0f; break;
                                                case KX_LINE_TRAJECTORY: child.value = (float)g_Config.m_KxLineTrajectoryRainbowSpeed / 10.0f; break;
                                                case KX_LINE_LASER_UNFREEZE: child.value = (float)g_Config.m_KxLineLaserUnfreezeRainbowSpeed / 10.0f; break;
                                                case KX_LINE_PATHFINDER: child.value = (float)g_Config.m_KxLinePathfinderRainbowSpeed / 10.0f; break;
                                                case KX_LINE_ESP: child.value = (float)g_Config.m_KxLineEspRainbowSpeed / 10.0f; break;
                                                case KX_LINE_TAS: child.value = (float)g_Config.m_KxLineTasRainbowSpeed / 10.0f; break;
                                                }
                                        }
                                        // v1.56.210: Gradient toggle per-component
                                        else if(str_comp(child.pName, "Gradient") == 0)
                                        {
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: child.on = g_Config.m_KxLineAimBotGradient != 0; break;
                                                case KX_LINE_TRIGGERBOT: child.on = g_Config.m_KxLineTriggerBotGradient != 0; break;
                                                case KX_LINE_TRAJECTORY: child.on = g_Config.m_KxLineTrajectoryGradient != 0; break;
                                                case KX_LINE_LASER_UNFREEZE: child.on = g_Config.m_KxLineLaserUnfreezeGradient != 0; break;
                                                case KX_LINE_PATHFINDER: child.on = g_Config.m_KxLinePathfinderGradient != 0; break;
                                                case KX_LINE_ESP: child.on = g_Config.m_KxLineEspGradient != 0; break;
                                                case KX_LINE_TAS: child.on = g_Config.m_KxLineTasGradient != 0; break;
                                                }
                                        }
                                        // v1.56.210: Step slider per-component (hue increment per segment)
                                        else if(str_comp(child.pName, "Step") == 0)
                                        {
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: child.value = (float)g_Config.m_KxLineAimBotGradientStep; break;
                                                case KX_LINE_TRIGGERBOT: child.value = (float)g_Config.m_KxLineTriggerBotGradientStep; break;
                                                case KX_LINE_TRAJECTORY: child.value = (float)g_Config.m_KxLineTrajectoryGradientStep; break;
                                                case KX_LINE_LASER_UNFREEZE: child.value = (float)g_Config.m_KxLineLaserUnfreezeGradientStep; break;
                                                case KX_LINE_PATHFINDER: child.value = (float)g_Config.m_KxLinePathfinderGradientStep; break;
                                                case KX_LINE_ESP: child.value = (float)g_Config.m_KxLineEspGradientStep; break;
                                                case KX_LINE_TAS: child.value = (float)g_Config.m_KxLineTasGradientStep; break;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Layer") == 0)
                                                child.value = (float)g_Config.m_KxLineRenderingLayer;
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

        // v1.56.176: refresh Info panel dynamic text (client identity + FPS).
        // Runs every frame so AVG FPS stays live and spoofed settings update.
        UpdateInfoRows();
        UpdatePathfinderLabels();
        UpdateCodeExecRows();
        UpdateTasRows();

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

                // v1.56.175: Drag-to-sort — if the dragged panel's center is over
                // another panel's slot (header rect), swap them in m_aPanelOrder.
                // The swapped-out (non-drag) panel glides to the dragged panel's old
                // slot via the render-loop lerp below. The dragged panel keeps
                // following the mouse (target = mouse - offset) until release.
                const vec2 dragCenter = vec2(
                        m_aPanelTarget[m_DragPanelIdx].x + PANEL_WIDTH * 0.5f,
                        m_aPanelTarget[m_DragPanelIdx].y + HEADER_HEIGHT * 0.5f);
                const int dragSlot = SlotOf(m_DragPanelIdx);
                for(int s = 0; s < NUM_PANELS; ++s)
                {
                        if(s == dragSlot)
                                continue;
                        const vec2 slotHome = SlotHomePos(s);
                        // Use the header rect of the slot as the drop target — predictable
                        // and matches where the user grabs panels.
                        if(PointInRect(dragCenter, slotHome.x, slotHome.y, PANEL_WIDTH, HEADER_HEIGHT))
                        {
                                // Swap: the panel occupying slot s moves to dragSlot, the
                                // dragged panel takes slot s.
                                m_aPanelOrder[dragSlot] = m_aPanelOrder[s];
                                m_aPanelOrder[s] = m_DragPanelIdx;
                                // The swapped-out panel's target is updated in the render loop
                                // (non-drag branch lerps to PanelHomePos = its new slot home).
                                break;
                        }
                }
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
                        if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Custom latency") == 0)
                                g_Config.m_KxDummyHammerLatencyMs = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "BG blackout") == 0)
                                m_BgBlackout = (int)pTarget->value;
                        // v1.56.204: Line rendering per-component Speed slider (display = config/10)
                        else if(str_comp(pTarget->pName, "Speed") == 0 && str_comp(row.pName, "Line rendering") == 0)
                        {
                                int compIdx = g_Config.m_KxLineComponent;
                                EKxLineComponent comp = (EKxLineComponent)compIdx;
                                int rawVal = (int)(pTarget->value * 10.0f + 0.5f);
                                switch(comp)
                                {
                                case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotRainbowSpeed = rawVal; break;
                                case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotRainbowSpeed = rawVal; break;
                                case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryRainbowSpeed = rawVal; break;
                                case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeRainbowSpeed = rawVal; break;
                                case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderRainbowSpeed = rawVal; break;
                                case KX_LINE_ESP: g_Config.m_KxLineEspRainbowSpeed = rawVal; break;
                                case KX_LINE_TAS: g_Config.m_KxLineTasRainbowSpeed = rawVal; break;
                                }
                        }
                        // v1.56.210: Line rendering per-component Step slider (hue increment per segment)
                        else if(str_comp(pTarget->pName, "Step") == 0 && str_comp(row.pName, "Line rendering") == 0)
                        {
                                int compIdx = g_Config.m_KxLineComponent;
                                EKxLineComponent comp = (EKxLineComponent)compIdx;
                                int rawVal = (int)(pTarget->value + 0.5f);
                                switch(comp)
                                {
                                case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotGradientStep = rawVal; break;
                                case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotGradientStep = rawVal; break;
                                case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryGradientStep = rawVal; break;
                                case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeGradientStep = rawVal; break;
                                case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderGradientStep = rawVal; break;
                                case KX_LINE_ESP: g_Config.m_KxLineEspGradientStep = rawVal; break;
                                case KX_LINE_TAS: g_Config.m_KxLineTasGradientStep = rawVal; break;
                                }
                        }
                        else if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Copy Moves Latency") == 0)
                                g_Config.m_KxCopyMovesLatency = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ratio") == 0 && str_comp(row.pName, "Freedom Path") == 0)
                                g_Config.m_KxPfFreedomRatio = (int)(pTarget->value * 10.0f + 0.5f);
                        else if(str_comp(pTarget->pName, "Max Distance") == 0 && str_comp(row.pName, "Auto fly") == 0)
                                g_Config.m_KxDummyHammerAutoMaxDist = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                g_Config.m_KxTripleFlyTriggerRadius = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                g_Config.m_KxTripleFlyRadius = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Edge in ms") == 0 && str_comp(row.pName, "Edge bind") == 0)
                                g_Config.m_KxEdgeBindMs = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Version ID") == 0)
                                g_Config.m_KxVersionSpoofId = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeFov = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeTriggerTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                g_Config.m_KxBafTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger ticks") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                g_Config.m_KxBafTriggerTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                g_Config.m_KxBafAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                g_Config.m_KxBafFov = (int)pTarget->value;
                        // v1.56.108: Line rendering per-component writes
                        else if(str_comp(pTarget->pName, "Line size") == 0 || str_comp(pTarget->pName, "Line opacity") == 0)
                        {
                                // v1.56.111: read selected component from config (persisted)

                                int compIdx = g_Config.m_KxLineComponent;

                                EKxLineComponent comp = (EKxLineComponent)compIdx;
                                if(str_comp(pTarget->pName, "Line size") == 0)
                                        {
                                        switch(comp)
                                        {
                                        case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotSize = (int)pTarget->value; break;
                                        case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotSize = (int)pTarget->value; break;
                                        case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectorySize = (int)pTarget->value; break;
                                        case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeSize = (int)pTarget->value; break;
                                        case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderSize = (int)pTarget->value; break;
                                        case KX_LINE_ESP: g_Config.m_KxLineEspSize = (int)pTarget->value; break;
                                        case KX_LINE_TAS: g_Config.m_KxLineTasSize = (int)pTarget->value; break;
                                        }
                                        }
                                else // Line opacity
                                        {
                                        switch(comp)
                                        {
                                        case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotAlpha = (int)pTarget->value; break;
                                        case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotAlpha = (int)pTarget->value; break;
                                        case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryAlpha = (int)pTarget->value; break;
                                        case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeAlpha = (int)pTarget->value; break;
                                        case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderAlpha = (int)pTarget->value; break;
                                        case KX_LINE_ESP: g_Config.m_KxLineEspAlpha = (int)pTarget->value; break;
                                        case KX_LINE_TAS: g_Config.m_KxLineTasAlpha = (int)pTarget->value; break;
                                        }
                                        }
                        }
                        else if(str_comp(pTarget->pName, "Layer") == 0)
                                g_Config.m_KxLineRenderingLayer = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "ESP X") == 0)
                                g_Config.m_KxEspScreenX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "ESP Y") == 0)
                                g_Config.m_KxEspScreenY = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspSpeed = (int)pTarget->value;
                        // v1.56.178: Trajectory per-type slider — writes to CTrajectory.m_aTypes[sel].
                        else if(str_comp(pTarget->pName, "Prediction Ticks") == 0 && str_comp(row.pName, "Trajectory") == 0)
                        {
                                CTrajectory &traj = GameClient()->m_Trajectory;
                                int sel = traj.m_SelectedType;
                                if(sel >= 0 && sel < CTrajectory::NUM_TRAJ_TYPES)
                                        traj.m_aTypes[sel].m_PredictionTicks = (int)pTarget->value;
                        }
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
                        else if(str_comp(pTarget->pName, "Hook angles") == 0)
                                g_Config.m_KxPfHookAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "A* Weight") == 0)
                                g_Config.m_KxPfAStarWeight = (int)(pTarget->value * 100.0f + 0.5f);
                        else if(str_comp(pTarget->pName, "Beam width") == 0)
                                g_Config.m_KxPfBeamWidth = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Evolutions") == 0)
                                g_Config.m_KxPfRheaGenerations = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Performance") == 0)
                                g_Config.m_KxPfPerf = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Tunnel size") == 0)
                                g_Config.m_KxPfTunnelSize = (int)pTarget->value;
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

        // Set up pixel-space projection. The screen is mapped into the virtual
        // 1920x1080 space (DPI-scaled), so all hardcoded geometry below renders
        // at the same physical size on every resolution.
        const float screenW = VScreenWidth();
        const float screenH = VScreenHeight();
        Graphics()->MapScreen(0.0f, 0.0f, screenW, screenH);

        Graphics()->TextureClear();

        // Background dimming: black overlay, alpha = BG_DIM_ALPHA * eased.
        // Drawn FIRST so panels appear on top of it. Fades in/out smoothly.
        if(eased > 0.0f)
        {
                Graphics()->BlendNormal();
                Graphics()->DrawRect(0.0f, 0.0f, screenW, screenH,
                        ColorRGBA(0.0f, 0.0f, 0.0f, (m_BgBlackout / 100.0f) * eased),
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
        // v1.56.175: BOTH the dragged panel AND non-dragged panels lerp toward
        // their target. For non-dragged panels the target is their slot home
        // (PanelHomePos), so when a drag-to-sort swap moves them to a new slot
        // they glide there. When no swap happened, target == current position,
        // so the lerp is a no-op (no jitter).
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
        {
                // Non-dragged panel: glide toward its slot home (supports drag-to-sort
                // swaps). When already at home this is a no-op.
                m_aPanelTarget[i] = home;
                m_aPanelPos[i] = vec2(
                        m_aPanelPos[i].x + (m_aPanelTarget[i].x - m_aPanelPos[i].x) * dragFactor,
                        m_aPanelPos[i].y + (m_aPanelTarget[i].y - m_aPanelPos[i].y) * dragFactor);
        }
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

        if(m_FileBrowserOpen)
                RenderFileBrowser(alpha);

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

        // Android BACK (translated to KEY_ESCAPE in engine/client/input.cpp) or
        // desktop ESC: close the ClickGUI instead of leaking into the game menu.
        // While an input field is being edited, route through HandleTextInput
        // first so the typed text is committed (same as ESC inside the field).
        if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
        {
                if(m_InputEditing)
                        HandleTextInput(Event);
                m_FileBrowserOpen = false;
                g_Config.m_ClClickGui = 0;
                return true;
        }

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
                const float dir = (Event.m_Key == KEY_MOUSE_WHEEL_UP) ? -SCROLL_STEP : SCROLL_STEP;
                return ScrollPanelAt(m_MousePos, dir);
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

// v1.56.218: Android touch support. Called by the game client each frame with
// the current finger states (normalized 0..1 positions). While ClickGUI is
// open we claim every touch — returning true stops the input stack here, so
// the game's touch controls / camera / aim never see the touches. The primary
// finger acts as a Windows cursor: its position drives m_MousePos.
// v1.56.219: swipe-to-scroll. A fresh press on a plain row is deferred — if
// the finger moves it becomes a scroll gesture instead of an accidental tap.
// Presses on headers (panel drag), sliders, dropdowns and while editing an
// input field fire immediately like on desktop.
bool CClickGui::OnTouchState(const std::vector<IInput::CTouchFingerState> &vTouchFingerStates)
{
        const bool isOpen = g_Config.m_ClClickGui != 0 || m_AnimProgress > 0.0f;
        if(!isOpen)
        {
                m_vPrevTouchFingers.clear();
                m_TouchActive = false;
                m_TouchPendingTap = false;
                m_TouchScrollMode = false;
                m_TouchScrollAccum = 0.0f;
                return false;
        }

        m_TouchActive = !vTouchFingerStates.empty();

        if(!vTouchFingerStates.empty())
        {
                // Primary finger = cursor. Map normalized touch position to
                // ClickGUI virtual units (physical px / m_UiScale), same space
                // as m_MousePos from NativeMousePos on desktop.
                const vec2 ScreenSize = vec2((float)Graphics()->ScreenWidth(), (float)Graphics()->ScreenHeight());
                const vec2 FingerPos = vTouchFingerStates.front().m_Position;
                m_MousePos = vec2(FingerPos.x * ScreenSize.x, FingerPos.y * ScreenSize.y) / (m_UiScale > 0.0f ? m_UiScale : 1.0f);

                // True if the press lands on something that needs an immediate
                // press-down (drag interactions), not a deferred tap.
                const auto HitHeaderOrSlider = [&](vec2 pos) {
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
                                const vec2 ppos = m_aPanelPosValid[i] ? m_aPanelPos[i] : PanelHomePos(i);
                                if(PointInRect(pos, ppos.x, ppos.y, PANEL_WIDTH, HEADER_HEIGHT))
                                        return true;
                                if(m_aPanelCollapsed[i])
                                        continue;
                                int outRow, outChild;
                                float rx, ry, rw, rh;
                                if(HitTestRow(pos, i, outRow, outChild, rx, ry, rw, rh))
                                {
                                        SPanel &panel = g_Panels[i];
                                        SRow &row = panel.pRows[outRow];
                                        SRow *pTarget = (outChild >= 0 && row.pChildren) ? &row.pChildren[outChild] : &row;
                                        if(pTarget->type == ERowType::Slider && pTarget->max > pTarget->min)
                                                return true;
                                }
                        }
                        return false;
                };

                // Finger press (not present last frame).
                const bool WasDown = std::any_of(m_vPrevTouchFingers.begin(), m_vPrevTouchFingers.end(), [&](const IInput::CTouchFingerState &Prev) {
                        return Prev.m_Finger == vTouchFingerStates.front().m_Finger;
                });
                if(!WasDown)
                {
                        m_TouchPressPos = m_MousePos;
                        m_TouchScrollAccum = 0.0f;
                        m_TouchScrollMode = false;
                        if(m_DropdownOpen || m_InputEditing || HitHeaderOrSlider(m_MousePos))
                        {
                                // Drag-style interactions: press fires immediately.
                                m_TouchPendingTap = false;
                                if(m_AnimProgress >= 0.95f)
                                        HandleMouseDown(m_MousePos);
                        }
                        else
                        {
                                // Plain row: defer the click — a swipe becomes a scroll.
                                m_TouchPendingTap = true;
                        }
                }
                else if(m_TouchPendingTap || m_TouchScrollMode)
                {
                        // Swipe: accumulate vertical travel, emit one scroll step
                        // per SCROLL_STEP units (finger up = scroll down).
                        m_TouchScrollAccum += m_MousePos.y - m_TouchPressPos.y;
                        m_TouchPressPos = m_MousePos;
                        while(m_TouchScrollAccum >= SCROLL_STEP)
                        {
                                m_TouchScrollAccum -= SCROLL_STEP;
                                if(ScrollPanelAt(m_MousePos, -SCROLL_STEP))
                                        m_TouchScrollMode = true;
                        }
                        while(m_TouchScrollAccum <= -SCROLL_STEP)
                        {
                                m_TouchScrollAccum += SCROLL_STEP;
                                if(ScrollPanelAt(m_MousePos, SCROLL_STEP))
                                        m_TouchScrollMode = true;
                        }
                        if(m_TouchScrollMode)
                                m_TouchPendingTap = false;
                }
        }

        // Finger release (present last frame, gone now) = mouse button up.
        for(const auto &Prev : m_vPrevTouchFingers)
        {
                const bool StillDown = std::any_of(vTouchFingerStates.begin(), vTouchFingerStates.end(), [&](const IInput::CTouchFingerState &Cur) {
                        return Cur.m_Finger == Prev.m_Finger;
                });
                if(!StillDown && m_AnimProgress >= 0.95f)
                {
                        if(m_TouchPendingTap)
                        {
                                // Deferred tap: fire press + release at the release spot.
                                HandleMouseDown(m_MousePos);
                                HandleMouseUp(m_MousePos);
                        }
                        else
                        {
                                HandleMouseUp(m_MousePos);
                        }
                        m_TouchPendingTap = false;
                        m_TouchScrollMode = false;
                        m_TouchScrollAccum = 0.0f;
                }
        }

        m_vPrevTouchFingers = vTouchFingerStates;
        return true;
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

        if(m_FileBrowserOpen)
        {
                HandleFileBrowserClick(mousePos);
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
                        // v1.56.170 BUG8: apply input value via unified commit handler.
                        // Previously this block had all the if/else if application logic
                        // inline (full coverage), but HandleTextInput's CommitToRow lambda
                        // had only ClickGUI Color + Line color. Pressing Enter on Spoofer/
                        // Block/Binds/Dist fields left them unapplied. Now both paths call
                        // ApplyInputCommit for consistent behavior.
                        ApplyInputCommit(pRow);
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

                        // v1.56.176: Label rows are non-interactive — click passes through
                        // (no action, no toggle). BringToFront already happened above.
                        if(pTarget->type == ERowType::Label)
                                return;

                        // Sub-region hits within the row.
                        if(pTarget->type == ERowType::Toggle || pTarget->type == ERowType::ToggleDropdown || pTarget->type == ERowType::Expandable)
                        {
                                // Visual expandables (Visuals) cannot be
                                // toggled off — only expand/collapse. Click on name = expand toggle.
                                const bool isVisualExpandable = (pTarget->type == ERowType::Expandable) &&
                                        (str_comp(pTarget->pName, "Visuals") == 0 ||
                                         str_comp(pTarget->pName, "Line rendering") == 0 ||
                                         str_comp(pTarget->pName, "TAS") == 0);
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
                                                g_Config.m_KxAttack = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Random Aim") == 0)
                                                g_Config.m_KxRandomAim = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Trajectory") == 0)
                                                g_Config.m_KxShowTrajectory = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Laser unfreeze") == 0)
                                                g_Config.m_KxLaserUnfreeze = pTarget->on ? 1 : 0;
					else if(str_comp(pTarget->pName, "Fake Aim") == 0 && str_comp(g_Panels[i].pTitle, "Advanced") == 0)
						g_Config.m_KxFakeAim = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "ESP") == 0)
                                                g_Config.m_KxEsp = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Pause") == 0 && str_comp(panel.pTitle, "TAS") == 0)
                                                GameClient()->m_Tas.SetPaused(pTarget->on);
                                        else if(str_comp(pTarget->pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBasicAvoidFreeze = pTarget->on ? 1 : 0;
                                        // Functions panel — Fly/Copy Moves sub-section
                                        else if(str_comp(pTarget->pName, "Preserve input") == 0)
                                                g_Config.m_KxDummyHammerKeepInputs = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom latency") == 0)
                                                g_Config.m_KxDummyHammerCustomLatency = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Auto fly") == 0)
                                                g_Config.m_KxDummyHammerAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Auto Triple Fly") == 0)
                                                g_Config.m_KxTripleFly = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Edge bind") == 0)
                                                g_Config.m_KxEdgeBind = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror direction") == 0)
                                                g_Config.m_KxDummyCopyMirrorDir = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror aim X") == 0)
                                                g_Config.m_KxDummyCopyMirrorAimX = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Mirror aim Y") == 0)
                                                g_Config.m_KxDummyCopyMirrorAimY = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Smart aim") == 0)
                                                g_Config.m_KxDummyCopySmartAim = pTarget->on ? 1 : 0;
                                        // v1.56.151: Fly Ride
                                        else if(str_comp(pTarget->pName, "Fly Ride") == 0)
                                                g_Config.m_KxFlyRide = pTarget->on ? 1 : 0;
                                        // v1.56.83: AimBot/TriggerBot expandable master toggles
                                        else if(str_comp(pTarget->pName, "AimBot") == 0)
                                                g_Config.m_KxAimBot = pTarget->on;
                                        else if(str_comp(pTarget->pName, "TriggerBot") == 0)
                                                g_Config.m_KxTriggerBot = pTarget->on;
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
                                                        else if(str_comp(pTarget->pName, "Use raycast (Show)") == 0)
                                                            s.m_UseRaycastShow = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Use raycast angle (AB)") == 0)
                                                            s.m_UseAngle = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Predict") == 0)
                                                            s.m_Predict = pTarget->on;
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
                                                        else if(str_comp(pTarget->pName, "TB Use raycast (Show)") == 0)
                                                            s.m_UseRaycastShow = pTarget->on;
                                                }
                                        }
                                        // v1.56.178: Trajectory per-type child toggles (parent = "Trajectory")
                                        else if(str_comp(row.pName, "Trajectory") == 0 && pTarget != &row)
                                        {
                                                CTrajectory &traj = GameClient()->m_Trajectory;
                                                int sel = traj.m_SelectedType;
                                                if(sel >= 0 && sel < CTrajectory::NUM_TRAJ_TYPES)
                                                {
                                                        auto &ts = traj.m_aTypes[sel];
                                                        if(str_comp(pTarget->pName, "Show") == 0)
                                                                ts.m_Show = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Alpha Gradient") == 0)
                                                                ts.m_AlphaGradient = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Simulate Players") == 0)
                                                                ts.m_SimulatePlayers = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Show for other players") == 0)
                                                                ts.m_ShowForOtherPlayers = pTarget->on;
                                                        else if(str_comp(pTarget->pName, "Show for current") == 0)
                                                                ts.m_ShowForCurrent = pTarget->on;
                                                }
                                        }
                                        // v1.56.90: Block children toggles
                                        else if(str_comp(row.pName, "Block") == 0 && pTarget != &row)
                                        {
                                                CBotNet &bn = GameClient()->m_BotNet;
                                                if(str_comp(pTarget->pName, "Attack Main") == 0) g_Config.m_KxAtkMain = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Auto Main") == 0) g_Config.m_KxAutoMain = pTarget->on;
                                                else if(str_comp(pTarget->pName, "All Target") == 0) g_Config.m_KxTargetAll = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Auto Aim") == 0) g_Config.m_KxAutoAim = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Auto Fire") == 0) g_Config.m_KxAutoFire = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Auto Hook") == 0) g_Config.m_KxAutoHook = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Auto Hammer") == 0) g_Config.m_KxHammer = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Move Enabled") == 0) g_Config.m_KxMove = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Stand Enabled") == 0) g_Config.m_KxStand = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Stand On X Only") == 0) g_Config.m_KxStandOnX = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Smart Detect") == 0) g_Config.m_KxSmartDetect = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Smart Rescue") == 0) g_Config.m_KxSmartRescue = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Rescue Frozen") == 0) g_Config.m_KxRescue = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Rescue All") == 0) g_Config.m_KxRescueAll = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Kill On Freeze") == 0) g_Config.m_KxKillFrz = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Avoid Freeze") == 0) g_Config.m_KxAvoidFreeze = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Laser Rescue") == 0) g_Config.m_KxLaserRescue = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Pathfinder Enabled") == 0) g_Config.m_KxAtkPathfinder = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Simulate Players") == 0) g_Config.m_KxPfSimulatePlayers = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Fix Snap") == 0) g_Config.m_KxAtkPathfinderSnap = pTarget->on;
                                                else if(str_comp(pTarget->pName, "Hook Enabled") == 0) g_Config.m_KxPfHook = pTarget->on;
                                                else if(str_comp(pTarget->pName, "SPS (Push)") == 0) g_Config.m_KxAtkPathfinderSps = pTarget->on ? 1 : 0;
                                        }
                                        // Copy Moves Filter expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Copy Moves Filter") == 0)
                                                g_Config.m_KxDummyCopyMovesFilter = pTarget->on ? 1 : 0;
                                        // v1.56.187: Copy Moves Latency expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Copy Moves Latency") == 0)
                                                g_Config.m_KxCopyMovesLatencyEnabled = pTarget->on ? 1 : 0;
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
                                        else if(str_comp(pTarget->pName, "Filter: Weapon") == 0)
                                                g_Config.m_KxDummyCopyMovesFilterWeapon = pTarget->on ? 1 : 0;
                                        // v1.56.189: Auto Fly Predict toggle.
                                        else if(str_comp(pTarget->pName, "Predict") == 0 && str_comp(row.pName, "Auto fly") == 0)
                                                g_Config.m_KxDummyHammerAutoPredict = pTarget->on ? 1 : 0;
                                        // Settings → IRC children
                                        else if(str_comp(pTarget->pName, "Reveal join") == 0)
                                                g_Config.m_KxIrcRevealJoin = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Nameplate tag") == 0)
                                                g_Config.m_KxIrcNameplateTag = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze children
                                        else if(str_comp(pTarget->pName, "Auto laser") == 0)
                                                g_Config.m_KxLaserUnfreezeAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Silent") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                                g_Config.m_KxLaserUnfreezeSilent = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show attempt") == 0)
                                                g_Config.m_KxLaserUnfreezeShowAttempt = pTarget->on ? 1 : 0;
                                        // Basic Avoid Freeze toggles
                                        else if(str_comp(pTarget->pName, "Freeze") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafAvoidFreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Teleport") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafAvoidTeleport = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Death") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafAvoidDeath = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Direction") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafDirection = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Jump") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafJump = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Hook") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafHook = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Aim") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafAim = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Silent") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                                g_Config.m_KxBafSilent = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show for me") == 0)
                                                g_Config.m_KxFakeAimShowForMe = pTarget->on ? 1 : 0;
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
                                        else if(str_comp(pTarget->pName, "Freeze Support (Beta)") == 0)
                                                g_Config.m_KxPfFreezeSupport = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Freedom Path") == 0)
                                                g_Config.m_KxPfFreedom = pTarget->on ? 1 : 0;
                                        // Settings → IRC/Spoofer expandables are master toggles.
                                        else if(str_comp(pTarget->pName, "IRC") == 0)
                                                g_Config.m_KxIrcEnabled = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Spoofer") == 0)
                                        {
                                                g_Config.m_KxVersionSpoof = pTarget->on ? 1 : 0;
                                                g_Config.m_KxSpoofClientStr = pTarget->on ? 1 : 0;
                                        }
                                        // v1.56.107: Spoofer → Git hash toggle (child)
                                        else if(str_comp(pTarget->pName, "Git hash") == 0 && str_comp(row.pName, "Spoofer") == 0)
                                                g_Config.m_KxSpoofGitHashEnabled = pTarget->on ? 1 : 0;
                                        // v1.56.174: Spoofer → Extra NETMSGs toggle (BestClient preset only)
                                        else if(str_comp(pTarget->pName, "Extra NETMSGs") == 0 && str_comp(row.pName, "Spoofer") == 0)
                                        {
                                                if(g_Config.m_KxSpoofClientPreset == 1)
                                                        g_Config.m_KxSpoofTClientExtraNetmsgs = pTarget->on ? 1 : 0;
                                                else
                                                        g_Config.m_KxSpoofBestClientExtraNetmsgs = pTarget->on ? 1 : 0;
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
                                        // v1.56.201: Line rendering per-component Rainbow toggle
                                        else if(str_comp(pTarget->pName, "Rainbow") == 0 && str_comp(row.pName, "Line rendering") == 0)
                                        {
                                                int compIdx = g_Config.m_KxLineComponent;
                                                EKxLineComponent comp = (EKxLineComponent)compIdx;
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotRainbow = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotRainbow = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryRainbow = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeRainbow = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderRainbow = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_ESP: g_Config.m_KxLineEspRainbow = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_TAS: g_Config.m_KxLineTasRainbow = pTarget->on ? 1 : 0; break;
                                                }
                                        }
                                        // v1.56.210: Line rendering per-component Gradient toggle
                                        else if(str_comp(pTarget->pName, "Gradient") == 0 && str_comp(row.pName, "Line rendering") == 0)
                                        {
                                                int compIdx = g_Config.m_KxLineComponent;
                                                EKxLineComponent comp = (EKxLineComponent)compIdx;
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotGradient = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotGradient = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryGradient = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeGradient = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderGradient = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_ESP: g_Config.m_KxLineEspGradient = pTarget->on ? 1 : 0; break;
                                                case KX_LINE_TAS: g_Config.m_KxLineTasGradient = pTarget->on ? 1 : 0; break;
                                                }
                                        }
                                        return;
                                }
                        }
                        if(pTarget->type == ERowType::Button || pTarget->type == ERowType::DoubleButton)
                        {
                                        // v1.56.150: Pathfinding is now DoubleButton (Pathfinding + Paste).
                                        // Left zone = Pathfinding toggle, right zone = Paste (kx_pf_paste).
                                        // Right zone active only when m_PfState == FINISHED.
                                if(str_comp(pTarget->pName, "Pathfinding") == 0)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        const bool playVisible = (BN.m_PfState == PF_STATE_FINISHED);
                                        const bool specVisible = (BN.m_PfState == PF_STATE_RUNNING);
                                        bool playClicked = false;
                                        bool specClicked = false;
                                        if(pTarget->type == ERowType::DoubleButton && (playVisible || specVisible))
                                        {
                                                const float btnW = (rw - 6.0f) * 0.5f;
                                                const float zone1End = rx + 2.0f + btnW;
                                                if(mousePos.x >= zone1End)
                                                {
                                                        if(playVisible)
                                                                playClicked = true;
                                                        else
                                                                specClicked = true;
                                                }
                                        }

                                        if(playClicked)
                                        {
                                                BN.PfPasteToTas();
                                        }
                                        else if(specClicked)
                                        {
                                                if(BN.m_PfSpecFollow)
                                                {
                                                        BN.PfSpectateStop();
                                                        dbg_msg("pathfinder", "kx_pf_spectate 0: grenade follow disabled");
                                                }
                                                else
                                                {
                                                        BN.PfSpectateStart();
                                                        dbg_msg("pathfinder", "kx_pf_spectate 1: grenade follow enabled");
                                                }
                                        }
                                        else
                                        {
                                                // Pathfinding state toggle (left zone, or full width when Play hidden).
                                                if(BN.m_PfState == PF_STATE_IDLE)
                                                {
                                                        BN.m_PfState = PF_STATE_RUNNING;
                                                        BN.PfResetRun();
                                                }
                                                else if(BN.m_PfState == PF_STATE_RUNNING)
                                                {
                                                        BN.m_PfState = PF_STATE_FINISHED;
                                                                                BN.PfThreadStop();
                                                        // Stop pressed: if A* hasn't reached goal yet, try to
                                                        // reconstruct best partial path so kx_pf_paste works.
                                                        if(!BN.m_PfAPathReady && !BN.m_PfAOpen.empty())
                                                        {
                                                                // Find best open node (lowest H = closest to goal)
                                                                int bestIdx = -1;
                                                                float bestH = 1e18f;
                                                                for(int idx : BN.m_PfAOpen)
                                                                {
                                                                        if(idx < 0 || idx >= (int)BN.m_PfANodes.size())
                                                                                continue;
                                                                        if(BN.m_PfANodes[idx].H < bestH)
                                                                        {
                                                                                bestH = BN.m_PfANodes[idx].H;
                                                                                bestIdx = idx;
                                                                        }
                                                                }
                                                                if(bestIdx >= 0)
                                                                {
                                                                        BN.m_PfAGoalIdx = bestIdx;
                                                                        BN.PfAStarReconstruct();
                                                                        BN.m_PfAPathReady = true;
                                                                        BN.m_PfFullInputsIdx = 0;
                                                                }
                                                        }
                                                }
                                                else // PF_STATE_FINISHED
                                                {
                                                        BN.m_PfState = PF_STATE_IDLE;
														{
															std::lock_guard<std::mutex> lock(BN.m_PfDataMutex);
															BN.m_PfVPath.clear();
														}
                                                        // BUG4 v1.56.165: release heavy A* search buffers
                                                        // (m_PfANodes, m_PfFullInputs, ...)
                                                        // before going idle. They were left alive across the
                                                        // idle period before, and any heap corruption from the
                                                        // long Advanced Search sim landed inside their vector
                                                        // control blocks → crash on next PfResetRun.
                                                        BN.PfClearSearchState();
                                                }
                                        }
                                }
                                else if(str_comp(pTarget->pName, "Editor Forbidden Tiles") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(!BN.m_PfForbiddenTiles.empty() && mousePos.x >= rx + 2.0f + btnW)
                                                BN.PfClearForbiddenTiles();
                                        else
                                        {
                                                BN.m_PfEditorMode = (BN.m_PfEditorMode == 1) ? 0 : 1;
                                                if(BN.m_PfEditorMode != 0 && !BN.m_MapGridLoaded)
                                                        BN.LoadMapGrid();
                                        }
                                }
                                else if(str_comp(pTarget->pName, "Editor Finish Tiles") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(!BN.m_PfCustomFinishTiles.empty() && mousePos.x >= rx + 2.0f + btnW)
                                                BN.PfClearCustomFinishTiles();
                                        else
                                        {
                                                BN.m_PfEditorMode = (BN.m_PfEditorMode == 2) ? 0 : 2;
                                                if(BN.m_PfEditorMode != 0 && !BN.m_MapGridLoaded)
                                                        BN.LoadMapGrid();
                                        }
                                }
                                else if(str_comp(pTarget->pName, "Generate tunnel") == 0 && pTarget->type == ERowType::Button)
                                {
                                        GameClient()->m_BotNet.PfGenerateTunnel();
                                }
                                // TAS panel buttons.
                                else if(pTarget->pName == g_TasDyn_Action)
                                {
                                        CTas &Tas = GameClient()->m_Tas;
                                        if(Tas.Mode() == 1)
                                                Tas.ToggleRecord();
                                        else
                                                Tas.TogglePlay();
                                }
                                else if(pTarget->pName == g_TasDyn_Join)
                                {
                                        GameClient()->m_Tas.JoinLeavePlayground();
                                }
                                else if(str_comp(pTarget->pName, "Forward") == 0 && pTarget->type == ERowType::Button)
                                {
                                        GameClient()->m_Tas.StepForward();
                                }
                                else if(str_comp(pTarget->pName, "Rewind") == 0 && pTarget->type == ERowType::Button)
                                {
                                        GameClient()->m_Tas.StepRewind();
                                }
                                else if(str_comp(pTarget->pName, "Pick File") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        CTas &Tas = GameClient()->m_Tas;
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(g_TasFilePicked && mousePos.x >= rx + 2.0f + btnW)
                                        {
                                                Tas.UnpickFile();
                                        }
                                        else
                                        {
                                                if(KxPickFileAsync(KXFILEPICK_TAS))
                                                        return;
                                                char aTasPath[1024];
                                                if(KxPickTasFile(aTasPath, sizeof(aTasPath)))
                                                {
                                                        Tas.PickFile(aTasPath);
                                                        if(Tas.FilePicked())
                                                        {
                                                                for(SRow &R : g_TasRows)
                                                                {
                                                                        if(str_comp(R.pName, "Map") == 0)
                                                                        {
                                                                                str_copy(R.m_aInputBuf, Tas.PickedMap(), sizeof(R.m_aInputBuf));
                                                                                R.m_InputInitialized = true;
                                                                        }
                                                                        else if(str_comp(R.pName, "Name") == 0)
                                                                        {
                                                                                str_copy(R.m_aInputBuf, Tas.PickedName(), sizeof(R.m_aInputBuf));
                                                                                R.m_InputInitialized = true;
                                                                        }
                                                                        else if(str_comp(R.pName, "Author") == 0)
                                                                        {
                                                                                str_copy(R.m_aInputBuf, Tas.PickedAuthor(), sizeof(R.m_aInputBuf));
                                                                                R.m_InputInitialized = true;
                                                                        }
                                                                }
                                                        }
                                                }
                                                return;
                                        }
                                }
                                else if(str_comp(pTarget->pName, "Save") == 0 && pTarget->type == ERowType::Button)
                                {
                                        const char *pMap = "";
                                        const char *pName = "";
                                        const char *pAuthor = "";
                                        for(SRow &R : g_TasRows)
                                        {
                                                if(str_comp(R.pName, "Map") == 0)
                                                        pMap = R.m_aInputBuf;
                                                else if(str_comp(R.pName, "Name") == 0)
                                                        pName = R.m_aInputBuf;
                                                else if(str_comp(R.pName, "Author") == 0)
                                                        pAuthor = R.m_aInputBuf;
                                        }
                                        GameClient()->m_Tas.SaveTake(pMap, pName, pAuthor);
                                }
                                else if(str_comp(pTarget->pName, "Pick File") == 0 && pTarget->type == ERowType::Button)
                                {
                                        OpenScriptPicker();
                                        return;
                                }
                                else if(pTarget->pName == g_ExecDyn_Btn)
                                {
                                        CCodeExec &CE = GameClient()->m_CodeExec;
                                        if(CE.GetState() == CCodeExec::STATE_RUNNING)
                                                CE.Stop();
                                        else
                                                ExecuteSelectedScript();
                                        return;
                                }
                                // v1.56.90: Dummies Button (Connect Dummy, Switch to Main)
                                else if(str_comp(row.pName, "Dummies") == 0 && pTarget->type == ERowType::Button)
                                {
                                        if(str_comp(pTarget->pName, "Connect Dummy") == 0)
                                        {
                                                if(GameClient()->Client()->AnyDummyConnected())
                                                {
                                                        for(int D = 1; D < MAX_DUMMIES; D++)
                                                                if(GameClient()->Client()->DummyConnected(D))
                                                                        GameClient()->Client()->DummyDisconnect(D, nullptr);
                                                }
                                                else
                                                        GameClient()->Client()->DummyConnect(1);
                                        }
                                        else if(str_comp(pTarget->pName, "Switch to Main") == 0)
                                                g_Config.m_ClDummy = 0;
                                }
                                // v1.56.90: Dummies DoubleButton (Connect+Switch+Send)
                                else if(str_comp(row.pName, "Dummies") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        int d = pTarget->pName[1] - '0';
                                        if(!GameClient()->Client()->DummyConnected(d))
                                        {
                                                // Not connected — any click = Connect.
                                                GameClient()->Client()->DummyConnect(d);
                                        }
                                        else
                                        {
                                                // Connected — 3 zones: Disconnect | Switch | Send toggle (square checkbox)
                                                const float checkSide = rh - 4.0f;
                                                const float btnW = (rw - 8.0f - checkSide - 4.0f) * 0.5f;
                                                const float zone1End = rx + 2.0f + btnW;
                                                const float zone2End = zone1End + 2.0f + btnW;
                                                if(mousePos.x < zone1End)
                                                {
                                                        // Left = Disconnect
                                                        GameClient()->Client()->DummyDisconnect(d, nullptr);
                                                }
                                                else if(mousePos.x < zone2End)
                                                {
                                                        // Middle = Switch
                                                        if(g_Config.m_ClDummy != d)
                                                                g_Config.m_ClDummy = d;
                                                }
                                                else
                                                {
                                                        // Right = Send toggle
                                                        pTarget->expanded = !pTarget->expanded;
                                                        if(d >= 1 && d < 8)
                                                                m_aDummySendEnabled[d] = pTarget->expanded;
                                                }
                                        }
                                }
                                // v1.56.176: Info panel social buttons — open URL in browser.
                                else if(str_comp(row.pName, "Telegram") == 0)
                                {
                                        Client()->ViewLink("https://t.me/KinetixClient");
                                        return;
                                }
                                else if(str_comp(row.pName, "TikTok") == 0)
                                {
                                        Client()->ViewLink("https://tiktok.com/@KinetixClient");
                                        return;
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
                                if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Custom latency") == 0)
                                        g_Config.m_KxDummyHammerLatencyMs = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Copy Moves Latency") == 0)
                                        g_Config.m_KxCopyMovesLatency = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ratio") == 0 && str_comp(row.pName, "Freedom Path") == 0)
                                        g_Config.m_KxPfFreedomRatio = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Max Distance") == 0 && str_comp(row.pName, "Auto fly") == 0)
                                        g_Config.m_KxDummyHammerAutoMaxDist = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                        g_Config.m_KxTripleFlyTriggerRadius = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                        g_Config.m_KxTripleFlyRadius = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Edge in ms") == 0 && str_comp(row.pName, "Edge bind") == 0)
                                        g_Config.m_KxEdgeBindMs = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Version ID") == 0)
                                        g_Config.m_KxVersionSpoofId = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeFov = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeTriggerTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                        g_Config.m_KxBafTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger ticks") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                        g_Config.m_KxBafTriggerTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                        g_Config.m_KxBafAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Basic Avoid Freeze") == 0)
                                        g_Config.m_KxBafFov = (int)pTarget->value;
                                // Settings → Line rendering children
                                // v1.56.108: Line rendering per-component writes
                                else if(str_comp(pTarget->pName, "Line size") == 0 || str_comp(pTarget->pName, "Line opacity") == 0)
                                {
                                        // v1.56.111: read selected component from config (persisted)

                                        int compIdx = g_Config.m_KxLineComponent;

                                        EKxLineComponent comp = (EKxLineComponent)compIdx;
                                        if(str_comp(pTarget->pName, "Line size") == 0)
                                                {
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotSize = (int)pTarget->value; break;
                                                case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotSize = (int)pTarget->value; break;
                                                case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectorySize = (int)pTarget->value; break;
                                                case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeSize = (int)pTarget->value; break;
                                                case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderSize = (int)pTarget->value; break;
                                                case KX_LINE_ESP: g_Config.m_KxLineEspSize = (int)pTarget->value; break;
                                                case KX_LINE_TAS: g_Config.m_KxLineTasSize = (int)pTarget->value; break;
                                                }
                                                }
                                        else // Line opacity
                                                {
                                                switch(comp)
                                                {
                                                case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotAlpha = (int)pTarget->value; break;
                                                case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotAlpha = (int)pTarget->value; break;
                                                case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryAlpha = (int)pTarget->value; break;
                                                case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeAlpha = (int)pTarget->value; break;
                                                case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderAlpha = (int)pTarget->value; break;
                                                case KX_LINE_ESP: g_Config.m_KxLineEspAlpha = (int)pTarget->value; break;
                                                case KX_LINE_TAS: g_Config.m_KxLineTasAlpha = (int)pTarget->value; break;
                                                }
                                                }
                                }
                                else if(str_comp(pTarget->pName, "Layer") == 0)
                                        g_Config.m_KxLineRenderingLayer = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "ESP X") == 0)
                                        g_Config.m_KxEspScreenX = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "ESP Y") == 0)
                                        g_Config.m_KxEspScreenY = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                        g_Config.m_KxEspSpeed = (int)pTarget->value;
                                // v1.56.178: Trajectory per-type slider (drag path) — writes to CTrajectory.m_aTypes[sel].
                                else if(str_comp(pTarget->pName, "Prediction Ticks") == 0 && str_comp(row.pName, "Trajectory") == 0)
                                {
                                        CTrajectory &traj = GameClient()->m_Trajectory;
                                        int sel = traj.m_SelectedType;
                                        if(sel >= 0 && sel < CTrajectory::NUM_TRAJ_TYPES)
                                                traj.m_aTypes[sel].m_PredictionTicks = (int)pTarget->value;
                                }
                                else if(str_comp(pTarget->pName, "Hook angles") == 0)
                                        g_Config.m_KxPfHookAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "A* Weight") == 0)
                                        g_Config.m_KxPfAStarWeight = (int)(pTarget->value * 100.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Beam width") == 0)
                                        g_Config.m_KxPfBeamWidth = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Evolutions") == 0)
                                        g_Config.m_KxPfRheaGenerations = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Performance") == 0)
                                        g_Config.m_KxPfPerf = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Tunnel size") == 0)
                                        g_Config.m_KxPfTunnelSize = (int)pTarget->value;
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
        if(m_DragSliderPanel >= 0)
        {
                SPanel &panel = g_Panels[m_DragSliderPanel];
                if(m_DragSliderRow >= 0 && m_DragSliderRow < panel.rowCount)
                {
                        SRow &row = panel.pRows[m_DragSliderRow];
                        SRow *pTarget = (m_DragSliderChild >= 0 && row.pChildren) ? &row.pChildren[m_DragSliderChild] : &row;
                        if(str_comp(pTarget->pName, "ClickGUI Scale") == 0)
                                g_Config.m_KxClickGuiScale = (int)(pTarget->value * 10.0f + 0.5f);
                }
        }
        m_Dragging = false;
        m_DragPanelIdx = -1;
        m_DragSliderPanel = -1;
        m_DragSliderRow = -1;
        m_DragSliderChild = -1;
}

bool CClickGui::ScrollPanelAt(vec2 mousePos, float dir)
{
        if(m_FileBrowserOpen)
        {
                const int fileCount = (int)m_vScriptBrowserFiles.size();
                const int visibleRows = fileCount > 0 ? (fileCount < 8 ? fileCount : 8) : 2;
                const float maxScroll = (float)fileCount * 30.0f - (float)visibleRows * 30.0f;
                m_FileBrowserScroll += dir;
                if(m_FileBrowserScroll < 0.0f)
                        m_FileBrowserScroll = 0.0f;
                if(m_FileBrowserScroll > maxScroll)
                        m_FileBrowserScroll = maxScroll;
                return true;
        }
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
                if(mousePos.x >= p.x && mousePos.x < p.x + PANEL_WIDTH && mousePos.y >= bodyTop && mousePos.y < bodyBottom)
                {
                        // Write to stored so the value survives panel collapse/expand.
                        m_aPanelScrollStored[i] += dir;
                        if(m_aPanelScrollStored[i] < 0.0f)
                                m_aPanelScrollStored[i] = 0.0f;
                        return true;
                }
        }
        return false;
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
			continue;
		if(str_comp(panel.pTitle, "Pathfinder") == 0 && !PfRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(str_comp(panel.pTitle, "TAS") == 0 && !TasRowVisible(panel.pRows, panel.rowCount, r))
			continue;
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
                                        else if(str_comp(target.pName, "Dummy mode") == 0)
                                                g_Config.m_KxTripleFlyDummyMode = o;
                                        else if(str_comp(target.pName, "Target mode") == 0)
                                                g_Config.m_KxTripleFlyTargetMode = o;
                                        // v1.56.111: Line rendering Component dropdown
                                        else if(str_comp(target.pName, "Component") == 0)
                                                g_Config.m_KxLineComponent = o;
                                        else if(str_comp(target.pName, "Bind Mode") == 0)
                                                g_Config.m_KxBindsMode = o;
                                        else if(str_comp(target.pName, "Client preset") == 0)
                                        {
                                                g_Config.m_KxSpoofClientPreset = o;
                                                // v1.56.107: auto-fill name+version+id from preset (like menus_settings)
                                                if(o != 3) // not Custom
                                                {
                                                        if(o == 0)
                                                        {
                                                                str_copy(g_Config.m_KxSpoofClientName, "DDNet", sizeof(g_Config.m_KxSpoofClientName));
                                                                str_copy(g_Config.m_KxSpoofClientVersion, "19.8.2", sizeof(g_Config.m_KxSpoofClientVersion));
                                                                g_Config.m_KxVersionSpoofId = 19082;
                                                        }
                                                        else if(o == 1)
                                                        {
                                                                str_copy(g_Config.m_KxSpoofClientName, "TClient", sizeof(g_Config.m_KxSpoofClientName));
                                                                str_copy(g_Config.m_KxSpoofClientVersion, "10.9.0", sizeof(g_Config.m_KxSpoofClientVersion));
                                                                g_Config.m_KxVersionSpoofId = 20010;
                                                                str_copy(g_Config.m_KxSpoofGitHash, "2e2aedc2e7b463a10b324a0f69d3fdb57", sizeof(g_Config.m_KxSpoofGitHash));
                                                                g_Config.m_KxSpoofGitHashEnabled = 1;
                                                        }
                                                        else if(o == 2)
                                                        {
                                                                str_copy(g_Config.m_KxSpoofClientName, "BestClient", sizeof(g_Config.m_KxSpoofClientName));
                                                                str_copy(g_Config.m_KxSpoofClientVersion, "2.1.1 stable-beta", sizeof(g_Config.m_KxSpoofClientVersion));
                                                                g_Config.m_KxVersionSpoofId = 19080;
                                                                // v1.56.174: BestClient uses --short=32 git hash format (see
                                                                // BestClient/scripts/git_revision.py). Auto-fill official
                                                                // BestProjectTeam/BestClient main HEAD so user doesn't have to guess.
                                                                str_copy(g_Config.m_KxSpoofGitHash, "0df5194057d68a7708b282c5c8245308", sizeof(g_Config.m_KxSpoofGitHash));
                                                                g_Config.m_KxSpoofGitHashEnabled = 1;
                                                        }
                                                }
                                        }
					else if(str_comp(target.pName, "Mode") == 0 && str_comp(panel.pTitle, "Advanced") == 0)
						g_Config.m_KxFakeAimMode = o;
                                        // ESP dropdowns
                                        else if(str_comp(target.pName, "ESP Team") == 0)
                                                g_Config.m_KxEspTeamFilter = o;
                                        else if(str_comp(target.pName, "ESP Friend") == 0)
                                                g_Config.m_KxEspFriendFilter = o;
                                        else if(str_comp(target.pName, "ESP Dummy") == 0)
                                                g_Config.m_KxEspDummyFilter = o;
                                        else if(str_comp(target.pName, "ESP Clanmate") == 0)
                                                g_Config.m_KxEspClanmateFilter = o;
                                        else if(str_comp(target.pName, "ESP Freeze") == 0)
                                                g_Config.m_KxEspFreezeFilter = o;
                                        else if(str_comp(target.pName, "ESP Targets filter") == 0)
                                                g_Config.m_KxEspTargetsFilterMode = o;
                                        else if(str_comp(target.pName, "ESP Mode") == 0)
                                                g_Config.m_KxEspMode = o;
                                        else if(str_comp(target.pName, "ESP Style") == 0)
                                                g_Config.m_KxEspStyle = o;
                                        else if(str_comp(target.pName, "Algorithm") == 0)
                                                g_Config.m_KxPfAlgorithm = o;
                                        else if(str_comp(target.pName, "Effort") == 0)
                                                g_Config.m_KxPfEffort = o;
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
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
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
                                                        else if(str_comp(target.pName, "Dummy mode") == 0)
                                                                g_Config.m_KxTripleFlyDummyMode = o;
                                                        else if(str_comp(target.pName, "Target mode") == 0)
                                                                g_Config.m_KxTripleFlyTargetMode = o;
                                                        // v1.56.111: Line rendering Component dropdown
                                                        else if(str_comp(target.pName, "Component") == 0)
                                                                g_Config.m_KxLineComponent = o;
                                                        else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsMode = o;
                                                        else if(str_comp(target.pName, "Client preset") == 0)
                                                        {
                                                                g_Config.m_KxSpoofClientPreset = o;
                                                                // v1.56.107: auto-fill name+version+id from preset (like menus_settings)
                                                                if(o != 3) // not Custom
                                                                {
                                                                        if(o == 0)
                                                                        {
                                                                                str_copy(g_Config.m_KxSpoofClientName, "DDNet", sizeof(g_Config.m_KxSpoofClientName));
                                                                                str_copy(g_Config.m_KxSpoofClientVersion, "19.8.2", sizeof(g_Config.m_KxSpoofClientVersion));
                                                                                g_Config.m_KxVersionSpoofId = 19082;
                                                                        }
                                                                        else if(o == 1)
                                                                        {
                                                                                str_copy(g_Config.m_KxSpoofClientName, "TClient", sizeof(g_Config.m_KxSpoofClientName));
                                                                                str_copy(g_Config.m_KxSpoofClientVersion, "10.9.0", sizeof(g_Config.m_KxSpoofClientVersion));
                                                                                g_Config.m_KxVersionSpoofId = 20010;
                                                                                str_copy(g_Config.m_KxSpoofGitHash, "2e2aedc2e7b463a10b324a0f69d3fdb57", sizeof(g_Config.m_KxSpoofGitHash));
                                                                                g_Config.m_KxSpoofGitHashEnabled = 1;
                                                                        }
                                                                        else if(o == 2)
                                                                        {
                                                                                str_copy(g_Config.m_KxSpoofClientName, "BestClient", sizeof(g_Config.m_KxSpoofClientName));
                                                                                str_copy(g_Config.m_KxSpoofClientVersion, "2.1.1 stable-beta", sizeof(g_Config.m_KxSpoofClientVersion));
                                                                                g_Config.m_KxVersionSpoofId = 19080;
                                                                                // v1.56.174: BestClient uses --short=32 git hash format (see
                                                                                // BestClient/scripts/git_revision.py). Auto-fill official
                                                                                // BestProjectTeam/BestClient main HEAD so user doesn't have to guess.
                                                                                str_copy(g_Config.m_KxSpoofGitHash, "0df5194057d68a7708b282c5c8245308", sizeof(g_Config.m_KxSpoofGitHash));
                                                                                g_Config.m_KxSpoofGitHashEnabled = 1;
                                                                        }
                                                                }
                                                        }
							else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0 && str_comp(panel.pTitle, "Advanced") == 0)
								g_Config.m_KxFakeAimMode = o;
                                                        // ESP dropdowns
                                                        else if(str_comp(target.pName, "ESP Team") == 0)
                                                                g_Config.m_KxEspTeamFilter = o;
                                                        else if(str_comp(target.pName, "ESP Friend") == 0)
                                                                g_Config.m_KxEspFriendFilter = o;
                                                        else if(str_comp(target.pName, "ESP Dummy") == 0)
                                                                g_Config.m_KxEspDummyFilter = o;
                                                        else if(str_comp(target.pName, "ESP Clanmate") == 0)
                                                                g_Config.m_KxEspClanmateFilter = o;
                                                        else if(str_comp(target.pName, "ESP Freeze") == 0)
                                                                g_Config.m_KxEspFreezeFilter = o;
                                                        else if(str_comp(target.pName, "ESP Targets filter") == 0)
                                                                g_Config.m_KxEspTargetsFilterMode = o;
                                                        else if(str_comp(target.pName, "ESP Mode") == 0)
                                                                g_Config.m_KxEspMode = o;
                                                        else if(str_comp(target.pName, "ESP Style") == 0)
                                                                g_Config.m_KxEspStyle = o;
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
                                                                                else if(str_comp(target.pName, "Rules") == 0)
                                                                                        s.m_Rules = o;
                                                                                else if(str_comp(target.pName, "AB Team") == 0)
                                                                                        s.m_TeamFilter = o;
                                                                                else if(str_comp(target.pName, "AB Friend") == 0)
                                                                                        s.m_FriendFilter = o;
                                                                                else if(str_comp(target.pName, "AB Dummy") == 0)
                                                                                        s.m_DummyFilter = o;
                                                                                else if(str_comp(target.pName, "Clanmate filter") == 0)
                                                                                        s.m_ClanmateFilter = o;
                                                                                else if(str_comp(target.pName, "AB Freeze") == 0)
                                                                                        s.m_FreezeFilter = o;
                                                                                else if(str_comp(target.pName, "AB Priority") == 0)
                                                                                        s.m_Priority = o;
                                                                                else if(str_comp(target.pName, "Targets filter") == 0)
                                                                                        s.m_TargetsFilter = o;
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
                                                                                else if(str_comp(target.pName, "TB Rules") == 0)
                                                                                        s.m_Rules = o;
                                                                                else if(str_comp(target.pName, "TB Team") == 0)
                                                                                        s.m_TeamFilter = o;
                                                                                else if(str_comp(target.pName, "TB Friend") == 0)
                                                                                        s.m_FriendFilter = o;
                                                                                else if(str_comp(target.pName, "TB Dummy") == 0)
                                                                                        s.m_DummyFilter = o;
                                                                                else if(str_comp(target.pName, "Clanmate filter") == 0)
                                                                                        s.m_ClanmateFilter = o;
                                                                                else if(str_comp(target.pName, "TB Freeze") == 0)
                                                                                        s.m_FreezeFilter = o;
                                                                                else if(str_comp(target.pName, "TB Priority") == 0)
                                                                                        s.m_Priority = o;
                                                                                else if(str_comp(target.pName, "Targets filter") == 0)
                                                                                        s.m_TargetsFilter = o;
                                                                        }
                                                                }
                                                        }
                                                        // v1.56.178: Trajectory Type dropdown — selects which type's settings are edited.
                                                        else if(str_comp(row.pName, "Trajectory") == 0)
                                                        {
                                                                CTrajectory &traj = GameClient()->m_Trajectory;
                                                                if(str_comp(target.pName, "Type") == 0)
                                                                {
                                                                        if(o >= 0 && o < CTrajectory::NUM_TRAJ_TYPES)
                                                                                traj.m_SelectedType = o;
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
        if((!m_DropdownOpen && m_DropdownAnim <= 0.0f) || m_DropdownPanel < 0 || m_DropdownRow < 0)
                return;
        const float animT = std::clamp(m_DropdownAnim, 0.0f, 1.0f);
        const float openT = 1.0f - (1.0f - animT) * (1.0f - animT) * (1.0f - animT);
        auto RenderList = [&](SRow &target, float ddX, float ddY) {
                const float itemStep = DROPDOWN_H + 2.0f;
                const float popupH = target.optionCount * itemStep + 4.0f;
                const float popY = ddY + DROPDOWN_H + 2.0f;
                const float bgH = popupH * openT;
                DrawRoundedRect(ddX + 4.0f, popY + 4.0f, DROPDOWN_W, bgH, 4.0f,
                        IGraphics::CORNER_ALL,
                        ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.15f * openT));
                DrawRoundedRect(ddX - 1.0f, popY - 1.0f, DROPDOWN_W + 2.0f, bgH + 2.0f, 4.0f,
                        IGraphics::CORNER_ALL,
                        ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * openT));
                DrawRoundedRect(ddX, popY, DROPDOWN_W, bgH, 4.0f,
                        IGraphics::CORNER_ALL,
                        ColorRGBA(0xff0d0d18, true).WithAlpha(alpha * openT));
                for(int o = 0; o < target.optionCount; ++o)
                {
                        const float itemY = popY + 2.0f + o * itemStep;
                        const float reveal = (popY + bgH - itemY) / DROPDOWN_H;
                        if(reveal <= 0.0f)
                                continue;
                        const float ia = std::clamp(reveal, 0.0f, 1.0f) * openT;
                        const bool hovered = PointInRect(m_MousePos, ddX, itemY, DROPDOWN_W, DROPDOWN_H);
                        if(hovered)
                        {
                                DrawRoundedRect(ddX + 2.0f, itemY, DROPDOWN_W - 4.0f, DROPDOWN_H, 3.0f,
                                        IGraphics::CORNER_ALL,
                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.4f * ia));
                        }
                        if(o == target.valueIdx)
                        {
                                DrawRoundedRect(ddX + 3.0f, itemY + 6.0f, 2.5f, DROPDOWN_H - 12.0f, 1.0f,
                                        IGraphics::CORNER_ALL,
                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * ia));
                        }
                        const ColorRGBA optCol = (o == target.valueIdx)
                                ? ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * ia)
                                : ColorRGBA(0xffd8d8e8, true).WithAlpha(alpha * ia);
                        DrawText(ddX + 10.0f, itemY + 5.0f, 14.0f, target.pOptions[o], optCol);
                }
        };
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
                        continue;
		if(str_comp(panel.pTitle, "Pathfinder") == 0 && !PfRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(str_comp(panel.pTitle, "TAS") == 0 && !TasRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		const float rh = RowHeight(row.type);
                if(r == m_DropdownRow && m_DropdownChild < 0)
                {
                        const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                        const float ddX = bodyX + rw - DROPDOWN_W - 2.0f;
                        const float ddY = cursorY + (rh - DROPDOWN_H) * 0.5f;
                        RenderList(row, ddX, ddY);
                        return;
                }
                cursorY += rh + ROW_GAP;
                if(row.type == ERowType::Expandable && row.expanded && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
        continue;
                                const float crh = RowHeight(child.type);
                                if(r == m_DropdownRow && c == m_DropdownChild)
                                {
                                        const float rw = PANEL_WIDTH - BODY_PADDING * 2;
                                        const float crw = rw - SUB_LIST_INDENT;
                                        const float ddX = bodyX + SUB_LIST_INDENT + crw - DROPDOWN_W - 2.0f;
                                        const float ddY = cursorY + (crh - DROPDOWN_H) * 0.5f;
                                        RenderList(child, ddX, ddY);
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
                // v1.56.170 BUG8: delegate to unified commit handler so Enter/Escape
                // applies the SAME field handlers as mouse-click focus loss. Before this,
                // CommitToRow only handled ClickGUI Color + Line color — all other input
                // fields (Spoofer, Block, Binds, Dists, etc.) were silently dropped on Enter.
                ApplyInputCommit(pRow);
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
                        continue;
		if(str_comp(g_Panels[panelIdx].pTitle, "Pathfinder") == 0 && !PfRowVisible(g_Panels[panelIdx].pRows, g_Panels[panelIdx].rowCount, r))
			continue;
		if(str_comp(g_Panels[panelIdx].pTitle, "Code Execution (Lua)") == 0 && !CodeExecRowVisible(g_Panels[panelIdx].pRows, g_Panels[panelIdx].rowCount, r))
			continue;
		if(str_comp(g_Panels[panelIdx].pTitle, "TAS") == 0 && !TasRowVisible(g_Panels[panelIdx].pRows, g_Panels[panelIdx].rowCount, r))
			continue;
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
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
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

// v1.56.170 BUG8: unified input commit handler. Applies pRow->m_aInputBuf to the
// corresponding config/botnet field. Called from BOTH:
//   - HandleMouseDown (focus loss / click elsewhere) — was the only path applying
//     Spoofer/Block/Binds/Dist fields before this fix.
//   - HandleTextInput CommitToRow lambda (Enter/Escape) — was only applying
//     ClickGUI Color + Line color before this fix.
// Consolidating here guarantees both paths apply the SAME set of fields.
// pRow is void* because SRow is in an anonymous namespace (not visible in .h).
void CClickGui::ApplyInputCommit(void *pRowVoid)
{
        SRow *pRow = (SRow *)pRowVoid;
        if(!pRow)
                return;

        // Main Color (hex, e.g. "ff8800"). Was "ClickGUI Color" before v1.56.199.
        if(str_comp(pRow->pName, "Main Color") == 0)
        {
                unsigned hex = 0;
                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                {
                        m_AccentColorRGBA = 0xff000000u | (hex & 0xffffff);
                        m_RainbowEnabled = false; // manual color disables rainbow
                        m_SavedAccentColorRGBA = m_AccentColorRGBA;
                }
        }
        // Panel Color (hex, e.g. "161622").
        else if(str_comp(pRow->pName, "Panel Color") == 0)
        {
                unsigned hex = 0;
                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                        m_PanelBgRGBA = 0xff000000u | (hex & 0xffffff);
        }
        // Line color (per-component via m_KxLineComponent).
        else if(str_comp(pRow->pName, "Line color") == 0)
        {
                unsigned hex = 0;
                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                {
                        int compIdx = g_Config.m_KxLineComponent;
                        EKxLineComponent comp = (EKxLineComponent)compIdx;
                        switch(comp)
                        {
                        case KX_LINE_AIMBOT: g_Config.m_KxLineAimBotColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_TRIGGERBOT: g_Config.m_KxLineTriggerBotColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_TRAJECTORY: g_Config.m_KxLineTrajectoryColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_LASER_UNFREEZE: g_Config.m_KxLineLaserUnfreezeColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_PATHFINDER: g_Config.m_KxLinePathfinderColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_ESP: g_Config.m_KxLineEspColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_TAS: g_Config.m_KxLineTasColor = 0xff000000u | (hex & 0xffffff); break;
                        }
                }
        }
        // Spoofer string inputs.
        else if(str_comp(pRow->pName, "Custom client name") == 0)
                str_copy(g_Config.m_KxSpoofClientName, pRow->m_aInputBuf, sizeof(g_Config.m_KxSpoofClientName));
        else if(str_comp(pRow->pName, "Client version") == 0)
                str_copy(g_Config.m_KxSpoofClientVersion, pRow->m_aInputBuf, sizeof(g_Config.m_KxSpoofClientVersion));
        else if(str_comp(pRow->pName, "Git hash value") == 0)
                str_copy(g_Config.m_KxSpoofGitHash, pRow->m_aInputBuf, sizeof(g_Config.m_KxSpoofGitHash));
        else if((str_comp(pRow->pName, "Targets IDs") == 0 || str_comp(pRow->pName, "Untargets IDs") == 0) && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount)
        {
                const char *pOwner = g_Panels[m_InputPanel].pRows[m_InputRow].pName;
                CAimBot &ab = GameClient()->m_AimBot;
                if(str_comp(pOwner, "AimBot") == 0)
                {
                        int w = ab.m_AimBotSelectedWeapon;
                        if(w >= 0 && w < CAimBot::NUM_WEAPONS_AIM)
                                str_copy(ab.m_AimBotWeapons[w].m_aTargetsFilter, pRow->m_aInputBuf, sizeof(ab.m_AimBotWeapons[w].m_aTargetsFilter));
                }
                else if(str_comp(pOwner, "TriggerBot") == 0)
                {
                        int w = ab.m_TriggerBotSelectedWeapon;
                        if(w >= 0 && w < CAimBot::NUM_WEAPONS_TRIGGER)
                                str_copy(ab.m_TriggerBotWeapons[w].m_aTargetsFilter, pRow->m_aInputBuf, sizeof(ab.m_TriggerBotWeapons[w].m_aTargetsFilter));
                }
                else if(str_comp(pOwner, "ESP") == 0)
                        str_copy(g_Config.m_KxEspTargetsFilter, pRow->m_aInputBuf, sizeof(g_Config.m_KxEspTargetsFilter));
        }
        // Block children: string IDs.
        else if(str_comp(pRow->pName, "Main ID") == 0)
                g_Config.m_KxMain = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Dummy ID") == 0)
                g_Config.m_KxTripleFlyDummyId = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Target IDs") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount && str_comp(g_Panels[m_InputPanel].pRows[m_InputRow].pName, "Auto Triple Fly") == 0)
                str_copy(g_Config.m_KxTripleFlyTargetIds, pRow->m_aInputBuf, sizeof(g_Config.m_KxTripleFlyTargetIds));
        else if(str_comp(pRow->pName, "Target IDs") == 0)
                str_copy(GameClient()->m_BotNet.m_aTargetIDsStr, pRow->m_aInputBuf, sizeof(GameClient()->m_BotNet.m_aTargetIDsStr));
        else if(str_comp(pRow->pName, "Rescue IDs") == 0)
                str_copy(GameClient()->m_BotNet.m_aRescueIDsStr, pRow->m_aInputBuf, sizeof(GameClient()->m_BotNet.m_aRescueIDsStr));
        // Block children: numeric distances.
        else if(str_comp(pRow->pName, "Avoid Freeze Radius") == 0) g_Config.m_KxAvoidFreezeRadius = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Fire Dist") == 0) g_Config.m_KxFireDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Hook Dist") == 0) g_Config.m_KxHookDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Rescue Radius") == 0) g_Config.m_KxRescueRadius = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Target Dist") == 0) g_Config.m_KxTargetDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Main Dist") == 0) g_Config.m_KxMainDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Stand Dist") == 0) g_Config.m_KxStandDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Main Stand Dist") == 0) g_Config.m_KxMainStandDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Laser Rescue Dist") == 0) g_Config.m_KxLaserRescueDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Simulate Score") == 0) g_Config.m_KxPfSimulateScore = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Rays") == 0) g_Config.m_KxAtkPathfinderRays = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Dist") == 0) g_Config.m_KxAtkPathfinderRaysDist = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Buttons") == 0) str_copy(g_Config.m_KxEdgeBindButtons, pRow->m_aInputBuf, sizeof(g_Config.m_KxEdgeBindButtons));
        else if(str_comp(pRow->pName, "Dummy IDs") == 0) str_copy(g_Config.m_KxSdsDummyIds, pRow->m_aInputBuf, sizeof(g_Config.m_KxSdsDummyIds));
        // v1.56.171 BUG9: Binds panel — unified bind/unbind logic.
        // For each bind row: look up the command string, find the currently bound key
        // (if any), unbind it, then bind the new key (if non-empty). When the field is
        // emptied, only the unbind runs. All kx_ targets are now cvars, so
        // `toggle kx_X 1 0` works (no more "Invalid command: 'kx_attack'").
        else if(m_InputPanel == 4 && pRow->type == ERowType::Input)
        {
                const char *pCmd = nullptr;
                if(str_comp(pRow->pName, "ClickGUI") == 0) pCmd = "toggle cl_clickgui 1 0";
                else if(str_comp(pRow->pName, "AimBot") == 0) pCmd = "toggle kx_aimbot 1 0";
                else if(str_comp(pRow->pName, "TriggerBot") == 0) pCmd = "toggle kx_triggerbot 1 0";
                else if(str_comp(pRow->pName, "Fake Aim") == 0) pCmd = "toggle kx_fake_aim 1 0";
                else if(str_comp(pRow->pName, "Laser Unfreeze") == 0) pCmd = "toggle kx_laser_unfreeze 1 0";
                else if(str_comp(pRow->pName, "Copy Moves Filter") == 0) pCmd = "toggle kx_dummy_copy_moves_filter 1 0";
                else if(str_comp(pRow->pName, "Copy Moves Latency") == 0) pCmd = "toggle kx_copy_moves_latency_enabled 1 0";
                else if(str_comp(pRow->pName, "Trajectory") == 0) pCmd = "toggle kx_show_trajectory 1 0";
                else if(str_comp(pRow->pName, "ESP") == 0) pCmd = "toggle kx_esp 1 0";
                else if(str_comp(pRow->pName, "Basic Avoid Freeze") == 0) pCmd = "toggle kx_basic_avoid_freeze 1 0";
                else if(str_comp(pRow->pName, "Block") == 0) pCmd = "toggle kx_attack 1 0";
                else if(str_comp(pRow->pName, "Auto Triple Fly") == 0) pCmd = "toggle kx_triple_fly 1 0";
                else if(str_comp(pRow->pName, "Record") == 0) pCmd = "toggle kx_tas_record 1 0";
                else if(str_comp(pRow->pName, "Pause") == 0) pCmd = "toggle kx_tas_pause 1 0";
                else if(str_comp(pRow->pName, "Rewind") == 0) pCmd = "kx_tas_rewind";
                                else if(str_comp(pRow->pName, "Forward") == 0) pCmd = "kx_tas_forward";
                                else if(str_comp(pRow->pName, "Button") == 0) pCmd = "kx_dummy_switch";
                if(pCmd)
                {
                        if(g_Config.m_KxBindsMode == 0)
                        {
                                GameClient()->m_Binds.RemoveBindSegment(pCmd);
                                if(pRow->m_aInputBuf[0])
                                        GameClient()->m_Binds.AddBindSegment(pRow->m_aInputBuf, pCmd);
                        }
                        else
                        {
                                // Find the currently bound key for this command (if any).
                                char aOldKey[64];
                                GameClient()->m_Binds.GetKey(pCmd, aOldKey, sizeof(aOldKey));
                                // If the old key differs from the new one, unbind the old key.
                                if(aOldKey[0] && str_comp(aOldKey, pRow->m_aInputBuf) != 0)
                                {
                                        char ub[80];
                                        str_format(ub, sizeof(ub), "unbind \"%s\"", aOldKey);
                                        Console()->ExecuteLine(ub, -1, false);
                                }
                                // If the new key is non-empty, bind it.
                                if(pRow->m_aInputBuf[0])
                                {
                                        char cmd[256];
                                        str_format(cmd, sizeof(cmd), "bind \"%s\" %s", pRow->m_aInputBuf, pCmd);
                                        Console()->ExecuteLine(cmd, -1, false);
                                }
                        }
                }
        }
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
        const bool isPathfinderPanel = (str_comp(panel.pTitle, "Pathfinder") == 0);
        const bool isCodeExecPanel = (str_comp(panel.pTitle, "Code Execution (Lua)") == 0);

        // Build the layout signature: hash of (row index, child included?) bits.
        // Children are included when m_AnimExpand > 0 (covers expanded + animating).
        unsigned sig = 0;
        for(int r = 0; r < panel.rowCount; ++r)
        {
                const SRow &row = panel.pRows[r];
                if(isFunctionsPanel && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
		if(isPathfinderPanel && !PfRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(str_comp(panel.pTitle, "TAS") == 0 && !TasRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(isCodeExecPanel)
		{
			if(!CodeExecRowVisible(panel.pRows, panel.rowCount, r))
				continue;
			sig = sig * 31u + 7u;
                }
                sig = sig * 31u + (unsigned)r + 1u;
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
        continue;
                                sig = sig * 31u + (unsigned)(c + 100);
                        }
                }
        }

        SLayoutCache &cache = m_aLayoutCache[panelIdx];
        const bool needRebuild = m_aLayoutCacheDirty[panelIdx] || cache.signature != sig;

        YGNodeRef bodyRoot = nullptr;
        YGNodeRef rowNodes[256];
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
		if(isPathfinderPanel && !PfRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(isCodeExecPanel && !CodeExecRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(str_comp(panel.pTitle, "TAS") == 0 && !TasRowVisible(panel.pRows, panel.rowCount, r))
			continue;
				if(str_comp(panel.pTitle, "TAS") == 0 && !TasRowVisible(panel.pRows, panel.rowCount, r))
					continue;
                                addRowNode(row, false);
                                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                                {
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                                        continue;
                                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                                        continue;
                                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
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
        int rowToExpandSlot[256];
        for(int i = 0; i < 256; ++i)
                rowToExpandSlot[i] = -1;

        float hiddenChildrenHeight = 0.0f;
        for(int r = 0; r < panel.rowCount; ++r)
        {
                SRow &row = panel.pRows[r];
                if(isFunctionsPanel && !FunctionsRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
                if(isPathfinderPanel && !PfRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
                if(isCodeExecPanel && !CodeExecRowVisible(panel.pRows, panel.rowCount, r))
                        continue;
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f && row.m_AnimExpand < 1.0f)
                {
                        const float expandProgress = EaseBothDirections(row.m_AnimExpand, row.expanded ? 1.0f : 0.0f);
                        float childrenBlockH = 0.0f;
                        int visibleCount = 0;
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
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

                        if(expandInfoCount < 16 && r < 256)
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

        // v1.56.176: record this panel's current rendered height so SlotHomePos(5)
        // (Info panel) can stack below the panel in slot 4. Updated every frame.
        // Height = header + visible body. 1-frame lag is invisible at 60fps.
        m_aPanelCurrentHeight[panelIdx] = HEADER_HEIGHT + visibleBodyHeight;

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

        DrawRoundedRect(px + 4.0f, py + 4.0f, PANEL_WIDTH, panelH,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.15f));

        // Panel border (1px-ish outline via slightly larger dark rect behind).
        DrawRoundedRect(px - 1.0f, py - 1.0f, PANEL_WIDTH + 2.0f, panelH + 2.0f,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(COL_PANEL_BORDER, true).WithAlpha(alpha));

        // Panel background.
        DrawRoundedRect(px, py, PANEL_WIDTH, panelH,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(m_PanelBgRGBA, true).WithAlpha(alpha));

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

                ClipEnableVirtual(bodyX, bodyY, bodyW, visibleBodyHeight);

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
		if(str_comp(panel.pTitle, "Pathfinder") == 0 && !PfRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(str_comp(panel.pTitle, "Code Execution (Lua)") == 0 && !CodeExecRowVisible(panel.pRows, panel.rowCount, r))
			continue;
		if(str_comp(panel.pTitle, "TAS") == 0 && !TasRowVisible(panel.pRows, panel.rowCount, r))
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
                        // [✕ or empty] [name]
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x95" /* ✕ */,
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 18.0f, row.pName, textCol);
                        break;
                }
                case ERowType::ToggleDropdown:
                {
                        // [✕ or empty] [name] [select box]
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x95",
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
                        DrawRoundedRect(thumbX - SLIDER_THUMB_W * 0.5f, thumbY - SLIDER_THUMB_H * 0.5f,
                                SLIDER_THUMB_W, SLIDER_THUMB_H, 3.0f, IGraphics::CORNER_ALL,
                                ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha));
                        break;
                }
                case ERowType::Input:
                {
                        // [name (compact)] [text input box]
                        const float nameW = std::min(88.0f, TextRender()->TextWidth(17.0f, row.pName) + 4.0f);
                        DrawText(rx + 2.0f, ry + 5.0f, 17.0f, row.pName, textCol);
                        const float inW = rw / 2.0f; // v1.56.89: input field always half row width
                        const float inX = rx + rw - inW - 2.0f; // v1.56.89: align to right edge of panel (like dropdown)
                        const float inY = ry + 2.0f;
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
                        // [✕ or empty] [name] [▶ or ▼]
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x95",
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
                        if(row.pName == g_ExecDyn_Btn)
                        {
                                CCodeExec &CE = GameClient()->m_CodeExec;
                                if(CE.GetState() == CCodeExec::STATE_RUNNING)
                                {
                                        btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha);
                                        pLabel = "Stop";
                                }
                                else
                                {
                                        btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                        pLabel = "Execute";
                                }
                        }
                        if(row.pName == g_TasDyn_Action)
                        {
                                if(GameClient()->m_Tas.IsPlaying() || GameClient()->m_Tas.IsRealPlaying() || GameClient()->m_Tas.IsRecording())
                                        btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha); // red
                                else
                                        btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green
                        }
                        else if(row.pName == g_TasDyn_Join)
                        {
                                if(GameClient()->m_Tas.IsJoined())
                                        btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha); // red (Leave)
                                else
                                        btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green (Join)
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
                case ERowType::DoubleButton:
                {
                        // v1.56.150: Pathfinder DoubleButton — Pathfinding (left) + Paste (right).
                        // Paste visible only when m_PfState == FINISHED (path ready).
                        // When Paste hidden: Pathfinding takes full row width.
                        if(str_comp(row.pName, "Pathfinding") == 0)
                        {
                                CBotNet &BN = GameClient()->m_BotNet;
                                // Left button colors (Pathfinding state).
                                ColorRGBA leftCol = ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha);
                                const char *pLeftLabel = "Pathfinding";
                                if(BN.m_PfState == PF_STATE_IDLE)
                                {
                                        leftCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green
                                        pLeftLabel = "Pathfinding";
                                }
                                else if(BN.m_PfState == PF_STATE_RUNNING)
                                {
                                        leftCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha); // red
                                        pLeftLabel = "Stop";
                                }
                                else // FINISHED
                                {
                                        leftCol = ColorRGBA(0xffe69528, true).WithAlpha(alpha); // orange
                                        pLeftLabel = "Finish";
                                }

                                const bool playVisible = (BN.m_PfState == PF_STATE_FINISHED);
                                const bool specVisible = (BN.m_PfState == PF_STATE_RUNNING);
                                if(!playVisible)
                                {
                                        if(specVisible)
                                        {
                                                const float btnW = (rw - 6.0f) * 0.5f;
                                                DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, leftCol);
                                                const float lw = TextRender()->TextWidth(14.0f, pLeftLabel);
                                                DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 14.0f, pLeftLabel,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                const ColorRGBA specCol = ColorRGBA(0xff3d7ac4, true).WithAlpha(alpha);
                                                const char *pSpecLabel = BN.m_PfSpecFollow ? "Unspectate" : "Spectate";
                                                const float rx2 = rx + 2.0f + btnW + 2.0f;
                                                DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, specCol);
                                                const float rw2 = TextRender()->TextWidth(14.0f, pSpecLabel);
                                                DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 14.0f, pSpecLabel,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        }
                                        else
                                        {
                                                // Play hidden — Pathfinding full width.
                                                DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, leftCol);
                                                const float lw = TextRender()->TextWidth(16.0f, pLeftLabel);
                                                DrawText(rx + (rw - lw) * 0.5f, ry + 8.0f, 16.0f, pLeftLabel,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        }
                                }
                                else
                                {
                                        // Split: left (Pathfinding) + right (Play/Stop).
                                        const float btnW = (rw - 6.0f) * 0.5f; // 2px padding each side + 2px gap
                                        // Left button.
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(14.0f, pLeftLabel);
                                        DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 14.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        // Right button (Paste).
                                        const char *pRightLabel = "Paste";
                                        ColorRGBA rightCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green
                                        const float rx2 = rx + 2.0f + btnW + 2.0f;
                                        DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, rightCol);
                                        const float rw2 = TextRender()->TextWidth(14.0f, pRightLabel);
                                        DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 14.0f, pRightLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                        }
                        else if(str_comp(row.pName, "Editor Forbidden Tiles") == 0 || str_comp(row.pName, "Editor Finish Tiles") == 0)
                        {
                                CBotNet &BN = GameClient()->m_BotNet;
                                const bool isForbidden = (str_comp(row.pName, "Editor Forbidden Tiles") == 0);
                                const bool editorActive = isForbidden ? (BN.m_PfEditorMode == 1) : (BN.m_PfEditorMode == 2);
                                const bool clearVisible = isForbidden ? !BN.m_PfForbiddenTiles.empty() : !BN.m_PfCustomFinishTiles.empty();
                                ColorRGBA leftCol = editorActive
                                        ? ColorRGBA(0xffc43d3d, true).WithAlpha(alpha)
                                        : ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                const char *pLeftLabel = editorActive ? "Exit Editor" : row.pName;
                                const char *pClearLabel = isForbidden ? "Clear Forbidden Tiles" : "Clear Finish Tiles";
                                if(!clearVisible)
                                {
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(13.0f, pLeftLabel);
                                        DrawText(rx + (rw - lw) * 0.5f, ry + 8.0f, 13.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                                else
                                {
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(11.0f, pLeftLabel);
                                        DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 11.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        const float rx2 = rx + 2.0f + btnW + 2.0f;
                                        DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, ColorRGBA(0xffe69528, true).WithAlpha(alpha));
                                        const float rw2 = TextRender()->TextWidth(11.0f, pClearLabel);
                                        DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 11.0f, pClearLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                        }
                        }
                        else if(str_comp(row.pName, "Pick File") == 0)
                        {
                                const ColorRGBA leftCol = ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha);
                                if(!g_TasFilePicked)
                                {
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(16.0f, "Pick File");
                                        DrawText(rx + (rw - lw) * 0.5f, ry + 8.0f, 16.0f, "Pick File",
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                                else
                                {
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(14.0f, "Pick File");
                                        DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 14.0f, "Pick File",
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        const float rx2 = rx + 2.0f + btnW + 2.0f;
                                        DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, ColorRGBA(0xffc43d3d, true).WithAlpha(alpha));
                                        const float rw2 = TextRender()->TextWidth(14.0f, "Unpick File");
                                        DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 14.0f, "Unpick File",
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                        }
                        break;
                }
                case ERowType::Label:
                {
                        // v1.56.176: Label — non-interactive text row (Info panel).
                        // Just draw the text left-aligned, no background, no checkbox.
                        // Slightly dimmer than toggle row text to read as "info, not clickable".
                        DrawText(rx + 4.0f, ry + 5.0f, 16.0f, row.pName,
                                ColorRGBA(0xffb8b8c8, true).WithAlpha(alpha));
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
                        if(r < 256 && rowToExpandSlot[r] >= 0)
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
                                        if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                                continue;
                                        if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                                continue;
                                        if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                                continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
        continue;
                                        ++rowIdx;
                                }
                                // Update rowYOffset for lower rows (children are hidden).
                                if(r < 256 && rowToExpandSlot[r] >= 0)
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
                                ClipEnableVirtual(bodyX, childClipY, bodyW, childClipH);

                        // v1.56.118: merged children background + left stripe.
                        float childBlockTop = -1.0f, childBlockBottom = -1.0f;
                        int preIdx = rowIdx;
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) continue;
                                const YGNodeRef pn = rowNodes[preIdx];
                                const float py2 = bodyY + YGNodeLayoutGetTop(pn) + rowYOffset;
                                const float ph2 = YGNodeLayoutGetHeight(pn);
                                if(childBlockTop < 0.0f) childBlockTop = py2;
                                childBlockBottom = py2 + ph2;
                                ++preIdx;
                        }
                        if(childBlockTop >= 0.0f && childBlockBottom > childBlockTop)
                        {
                                const float cbX = bodyX + SUB_LIST_INDENT - 2.0f;
                                const float cbW = bodyW - SUB_LIST_INDENT + 4.0f;
                                const float cbH = childBlockBottom - childBlockTop;
                                DrawRoundedRect(cbX, childBlockTop, cbW, cbH, 0.0f, IGraphics::CORNER_NONE,
                                        ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(alpha * 0.6f));
                                const float stripeX = rx + 1.0f + CHECK_W * 0.5f - 1.0f;
                                const float stripeY = childBlockTop;
                                const float stripeH = childBlockBottom - childBlockTop;
                                if(stripeH > 0.5f)
                                        DrawRoundedRect(stripeX, stripeY, 2.0f, stripeH, 0.0f, IGraphics::CORNER_NONE,
                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha));
                        }

                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(str_comp(row.pName, "Visuals") == 0 && !TasVisualsChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                                        if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
                                                        if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c))
                                                                continue;
if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "ESP") == 0 && !EspChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Basic Avoid Freeze") == 0 && !BafChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c))
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

                                // v1.56.118: per-child bg removed (merged block drawn above).
                                switch(child.type)
                                {
                                case ERowType::Toggle:
                                        DrawRoundedRect(crx + 1.0f, cry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                        if(child.on)
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x95",
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
                                        DrawRoundedRect(thX - SLIDER_THUMB_W * 0.5f, thY - SLIDER_THUMB_H * 0.5f,
                                                SLIDER_THUMB_W, SLIDER_THUMB_H, 3.0f, IGraphics::CORNER_ALL,
                                                ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha));
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
                                                        DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x95",
                                                                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                        }
                                        if(child.type == ERowType::ToggleDropdown)
                                                DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        else
                                                DrawText(crx + 2.0f, cry + 5.0f, 17.0f, child.pName, cText);
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
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x95",
                                                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        break;
                                case ERowType::Button:
                                {
                                        // v1.56.90: Button child — colored rect with centered label (Dummies).
                                        ColorRGBA btnCol = ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha);
                                        const char *pLabel = child.pName;
                                        // v1.56.90: "Connect Dummy" label changes based on connection state.
                                        if(str_comp(child.pName, "Connect Dummy") == 0)
                                        {
                                                if(GameClient()->Client()->AnyDummyConnecting())
                                                        pLabel = "Connecting...";
                                                else if(GameClient()->Client()->AnyDummyConnected())
                                                {
                                                        pLabel = "Disconnect All";
                                                        btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green
                                                }
                                        }
                                        // Connected/active state → green tint.
                                        if(child.on)
                                                btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                        DrawRoundedRect(crx + 2.0f, cry + 2.0f, crw - 4.0f, crh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, btnCol);
                                        const float labelW = TextRender()->TextWidth(15.0f, pLabel);
                                        DrawText(crx + (crw - labelW) * 0.5f, cry + 7.0f, 15.0f, pLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        break;
                                }
                                case ERowType::DoubleButton:
                                {
                                        // v1.56.90: DoubleButton child — Connect + Switch + Send toggle (Dummies).
                                        // If dummy NOT connected: only Connect button (full row width).
                                        // If dummy connected: Connect (left) + Switch (middle) + Send checkbox (right).
                                        // v1.56.197: left button label is dynamic — "D# Connect" (idle),
                                        // "D# Connecting..." (connecting), "D# Disconnect" (connected).
                                        ColorRGBA leftCol = ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha);
                                        if(child.on) // child.on = connected state
                                                leftCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                        // v1.56.197: build dynamic left-button label based on connection state.
                                        // pName is const (static "D# Connect"), so render from a local buffer
                                        // when the state requires a different word.
                                        const char *pLeftLabel = child.pName;
                                        char aLeftLabel[32];
                                        if(child.pName && child.pName[0] == 'D' && child.pName[1] >= '1' && child.pName[1] <= '7')
                                        {
                                                int d = child.pName[1] - '0';
                                                if(child.on)
                                                        str_format(aLeftLabel, sizeof(aLeftLabel), "D%d Disconnect", d);
                                                else if(GameClient()->Client()->DummyConnecting(d))
                                                        str_format(aLeftLabel, sizeof(aLeftLabel), "D%d Connecting...", d);
                                                else
                                                        str_format(aLeftLabel, sizeof(aLeftLabel), "D%d Connect", d);
                                                pLeftLabel = aLeftLabel;
                                        }
                                        if(!child.on)
                                        {
                                                // Not connected — full-width Connect button.
                                                DrawRoundedRect(crx + 2.0f, cry + 2.0f, crw - 4.0f, crh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, leftCol);
                                                const float lw1 = TextRender()->TextWidth(15.0f, pLeftLabel);
                                                DrawText(crx + (crw - lw1) * 0.5f, cry + 7.0f, 15.0f, pLeftLabel,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        }
                                        else
                                        {
                                                // Connected — Connect (left) + Switch (middle) + Send checkbox (right, square).
                                                // Checkbox is square: side = crh-4 (same as button height).
                                                const float checkSide = crh - 4.0f; // square checkbox side
                                                const float btnW = (crw - 8.0f - checkSide - 4.0f) * 0.5f; // 2 buttons split remaining
                                                // Left button (Connect/Disconnect)
                                                DrawRoundedRect(crx + 2.0f, cry + 2.0f, btnW, crh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, leftCol);
                                                const float lw1 = TextRender()->TextWidth(13.0f, pLeftLabel);
                                                DrawText(crx + 2.0f + (btnW - lw1) * 0.5f, cry + 8.0f, 13.0f, pLeftLabel,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                // Middle button (Switch) — green if active.
                                                ColorRGBA rightCol = ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha);
                                                if(child.value > 0.5f)
                                                        rightCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                                const float rx2 = crx + 2.0f + btnW + 2.0f;
                                                DrawRoundedRect(rx2, cry + 2.0f, btnW, crh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, rightCol);
                                                const float lw2 = TextRender()->TextWidth(13.0f, child.pName2);
                                                DrawText(rx2 + (btnW - lw2) * 0.5f, cry + 8.0f, 13.0f, child.pName2,
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                // Right checkbox (Send) — square, same height as buttons.
                                                const float tx = crx + crw - checkSide - 2.0f;
                                                DrawRoundedRect(tx, cry + 2.0f, checkSide, checkSide, 4.0f,
                                                        IGraphics::CORNER_ALL,
                                                        ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                                if(child.expanded) // child.expanded = send enabled
                                                        DrawText(tx + checkSide * 0.5f, cry + 2.0f - 2.0f, checkSide, "\xe2\x9c\x95",
                                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                        }
                                        break;
                                }
                                }
                        }

                        // Restore the body clip for subsequent rows.
                        if(useChildClip)
                                ClipEnableVirtual(bodyX, bodyY, bodyW, visibleBodyHeight);

                        // v1.56.72: use precomputed expandInfo (avoids re-calling
                        // EaseBothDirections and re-summing childrenBlockH here).
                        // rowToExpandSlot[r] gives the slot in expandInfo, or -1
                        // if this expandable is fully expanded (no hidden height).
                        if(r < 256 && rowToExpandSlot[r] >= 0)
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
