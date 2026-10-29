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
#include <game/client/components/kinetix/media_info.h>

#include <yoga/Yoga.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <string>
#include <utility>

#include <base/color.h>
#include <base/hash.h>
#include <base/io.h>
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

#include <generated/protocol.h>
#include <game/client/animstate.h>
#include <game/client/components/skins.h>
#include <game/client/render.h>

#include <game/client/components/console.h>
#include <game/client/gameclient.h>
#include <game/version.h> // v1.56.177: GAME_NAME, GAME_RELEASE_VERSION, DDNET_VERSION_NUMBER, GIT_SHORTREV_HASH (used by Info panel)

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *hModule, char *lpFilename, unsigned long nSize);
#endif

// v1.56.285: ClickGUI resize cursor feedback (implemented in input.cpp — that
// TU already owns the SDL headers). Shape mirrors SDL_SystemCursor:
// 0=ARROW, 5=SIZENWSE, 7=SIZEWE, 8=SIZENS.
extern "C" void KxSetCursorShape(int Shape);

namespace {
// Total number of ClickGUI panels. Must match CClickGui::NUM_PANELS in .h.
// v1.56.177: was stale at 5 — caused "too many initializers" on g_Panels[6].
constexpr int NUM_PANELS = 11; // v1.56.286: + Search (last panel)
} // namespace

// v1.56.284: live panel geometry. The file-local macros below route every
// existing PANEL_WIDTH/MAX_BODY_HEIGHT call site (layout, render, hit-tests)
// to the config-driven values: KxPanelsWidth/KxPanelsHeight while UI settings
// are enabled, built-in defaults otherwise. Updated each frame in OnRender.
static float g_PanelW = 300.0f;
static float g_PanelMaxBodyH = 600.0f;
// v1.56.285: set when a layout offset config (Panels Start/Indent X/Y) changes —
// forces every panel to glide to its new slot home once, then clears itself.
static bool g_ForceRehome = false;

// v1.56.286: ClickGUI-wide search state (the Search panel). The field writes
// g_SearchQuery live (polled from the edit buffer); SearchUpdateMatches()
// marks matching rows and the Search* helpers hide non-matching rows in
// every layout/render/hit-test pass. Not persisted (ClickGUI-local).
static char g_SearchQuery[128] = {0};
static bool g_SearchCaseSensitive = false;
static bool g_SearchWholeWord = false;
static bool g_SearchRegex = false;
static bool g_SearchFuzzy = false;
// v1.56.286: smoothed body height per panel — the search reflow updates the
// Yoga layout instantly, the panel bottom glides to the new height.
static float g_aSmoothBodyH[NUM_PANELS] = {0.0f};
#define PANEL_WIDTH g_PanelW
#define MAX_BODY_HEIGHT g_PanelMaxBodyH

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
        ToggleInput, // v1.56.282: toggle (left) + text input (right) — Dummy Manager proxy rows
        Dropdown,
        Expandable,
        Button,
        DoubleButton, // v1.56.90: two buttons in one row (Dummies Connect+Switch)
        Label, // v1.56.176: non-interactive text row (Info panel: version strings, FPS)
};

// v1.56.176: Kinetix client version string (shown in Info panel).
// Bumped manually per release. Matches the commit-message version scheme.
#define KINETIX_VERSION "v6"

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
        // v1.56.284: optional ToolTip text — available for EVERY row type
        // (slider, toggle, expandable, visual expandable, button, double
        // button, label, input, dropdown...). nullptr = no tooltip. To give
        // a row a tooltip, append one more string to its initializer.
        const char *pToolTip = nullptr;
        // v1.56.288: optional SECOND tooltip — DoubleButton only. The left half
        // of the row shows pToolTip, the right half shows pToolTip2 (falls back
        // to pToolTip when null). Lets the two buttons describe different things.
        const char *pToolTip2 = nullptr;
        // v6.1: optional override for the slider value chip text. When set
        // (e.g. Spawner Entity/Tile ID), the slider shows this string instead
        // of the numeric value. Ignored by non-slider rows.
        const char *pValueLabel = nullptr;
        // v1.56.43: mutable input buffer (so edited text persists).
        // Lazy-initialized from pInputValue on first render.
        char m_aInputBuf[256] = {};
        bool m_InputInitialized = false;
        // v1.56.60: animation state (mutable, not serialised).
        // m_AnimExpand: 0..1 progress of the expand/collapse slide animation.
        //               lerps toward (row.expanded ? 1.0 : 0.0) each frame.
        //               0 = children fully hidden (collapsed), 1 = fully shown.
	float m_AnimExpand = 0.0f;
	// v6.2: wrapped line count for Label rows (computed in the layout pass).
	int m_LabelLines = 1;
	// v1.56.286: search state (recomputed by SearchUpdateMatches() when the
	// query or a filter toggle changes). m_SearchChildMatch is only set on
	// top-level expandables (any direct child matched).
	bool m_SearchMatch = false;
	bool m_SearchChildMatch = false;
};

// v1.56.284: snaps a slider value to its step (drag + display share this).
// step <= 0 = free value. Result clamped to [min, max].
static float SnapSliderStep(const SRow &Row)
{
        if(Row.step <= 0.0f || Row.max <= Row.min)
                return Row.value;
        float v = Row.min + roundf((Row.value - Row.min) / Row.step) * Row.step;
        if(v < Row.min)
                v = Row.min;
        if(v > Row.max)
                v = Row.max;
        return v;
}

// v1.56.284: decimals to display for a step (0 for step >= 1, up to 3
// for fractional steps like 0.1 / 0.01 / 0.001).
static int SliderStepDecimals(const SRow &Row)
{
        if(Row.step <= 0.0f)
                return 0;
        int d = 0;
        float s = Row.step;
        while(s < 0.999f && d < 3)
        {
                s *= 10.0f;
                ++d;
        }
        return d;
}

// ---------- Functions panel (4) ----------
// "Custom latency" expandable: 1 slider child.
static SRow g_CustomLatencyChildren[] = {
        {ERowType::Slider, "Latency (ms)", nullptr, false, false, 50.0f, 2000.0f, 10.0f, 500.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Adds artificial latency to your own input (ms)."},
};
// v1.56.186: "Copy Moves Latency" expandable — staggered input replay delay.
// When >0, inactive dummies replay active player's input from N ms ago, where
// N = latency_ms * dummy_index (D1: 1x, D2: 2x, D3: 3x...). 0 = live copy.
static SRow g_CopyMovesLatencyChildren[] = {
        {ERowType::Slider, "Latency (ms)", nullptr, false, false, 50.0f, 5000.0f, 10.0f, 500.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Staggered replay delay; effective delay = value × dummy index (0 = live copy)."},
};
static const char *const g_TripleFlyDummyModeOpts[] = {"Closest To Me", "ID"};
static const char *const g_TripleFlyTargetModeOpts[] = {"Closest To Me", "Closest To Aim", "IDs"};
static SRow g_TripleFlyChildren[] = {
        {ERowType::Dropdown, "Dummy mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TripleFlyDummyModeOpts, 2, 0, nullptr, 0,
                "How the anchor dummy is chosen (closest to me or by id)."},
        {ERowType::Input, "Dummy ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "1", "id...", nullptr, 0, 0, nullptr, 0,
                "Anchor dummy id (used when Dummy mode = ID)."},
        {ERowType::Dropdown, "Target mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TripleFlyTargetModeOpts, 3, 0, nullptr, 0,
                "How the fly targets are chosen."},
        {ERowType::Input, "Target IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Target mode = IDs)."},
        {ERowType::Slider, "Trigger radius", nullptr, false, false, 32.0f, 380.0f, 1.0f, 380.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Distance at which the triple-fly starts working."},
        {ERowType::Slider, "Radius", nullptr, false, false, 32.0f, 256.0f, 1.0f, 128.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Search radius (units) for fly targets."},
};
// "Copy Moves Filter" expandable: 6 sub-toggles.
static SRow g_CopyMovesFilterChildren[] = {
        {ERowType::Toggle, "Filter: Jump", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Do not copy jump input from the followed player."},
        {ERowType::Toggle, "Filter: Direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Do not copy movement direction from the followed player."},
        {ERowType::Toggle, "Filter: Hook", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Do not copy hook input from the followed player."},
        {ERowType::Toggle, "Filter: Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Do not copy aim from the followed player."},
        {ERowType::Toggle, "Filter: Fire", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Do not copy fire input from the followed player."},
        {ERowType::Toggle, "Filter: Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Do not copy weapon switching from the followed player."},
};
// Aim mode dropdown options.
static const char *const g_AimModeOpts[] = {"Smart", "Mirror"};
// v1.56.83: AimBot/TriggerBot dropdown options (unique prefix AB/TB to avoid collisions).
static const char *const g_ABWeaponOpts[] = {"Hammer", "Pistol", "Shotgun", "Grenade", "Laser", "Ninja", "Hook"};
static const char *const g_ABTriggerModeOpts[] = {"One tick", "Hold", "Every tick"};
// v1.56.291: Hook "Entity Mode" — which map entity the hook bot targets.
// 0=Tee (players, normal behaviour), then the Custom colors -> Entities
// categories. Used by AimBot and TriggerBot (both only for Weapon = Hook; the
// selection doubles as the edited per-entity settings block, v1.56.292).
static const char *const g_EntityModeOpts[] = {"Tee", "Hookable", "Unhookable", "Freeze", "Death", "Others"};
// v1.56.262: AimBot modes reworked — Silent became a separate toggle, Mode is
// Edge/Trigger/Always/Smooth. While Silent is on the dropdown swaps to
// g_ABModeOptsNoTrigger ("Trigger" hidden) and the index is remapped.
static const char *const g_ABModeOpts[] = {"Edge", "Trigger", "Always", "Smooth"};
static const char *const g_ABModeOptsNoTrigger[] = {"Edge", "Always", "Smooth"};
static const char *const g_ABRulesOpts[] = {"None", "Insert line", "Raycast"};
// v1.56.275: "Team" filter (Both/My/War/Blue/Red) and "DDrace Team" filter
// (Both/My/Other/Target + Target ID field).
static const char *const g_TeamFilterOpts[] = {"Both", "My", "War", "Blue", "Red"};
static const char *const g_DDraceTeamOpts[] = {"Both", "My", "Other", "Target"};
static const char *const g_ABFriendOpts[] = {"Both", "Ignore", "Only"};
static const char *const g_ABDummyOpts[] = {"Both", "Ignore", "Only"};
static const char *const g_ABTargetsFilterOpts[] = {"Both", "Only", "Ignore"};
static const char *const g_ABFreezeOpts[] = {"Both", "Freeze", "No freeze"};
static const char *const g_ABPriorityOpts[] = {"Nearest To FOV", "Nearest To Aim", "Nearest To Me"};
// v1.56.262: "Auto Fire" expandable — auto-fire every Delay ticks while fire is held.
static SRow g_AutoFireChildren[] = {
        {ERowType::Slider, "Delay", nullptr, false, false, 0.0f, 50.0f, 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Auto-fire every N ticks while fire is held (0 = every tick)."},
};
// v1.56.262: "BalanceBot" expandable — X-axis balance against the nearest
// matching player (drives the current dummy only). Filters reuse AimBot/ESP ones.
static const char *const g_BbVerticalOpts[] = {"Below", "Above", "Both"};
static SRow g_BalanceBotChildren[] = {
        {ERowType::Dropdown, "Vertical Filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_BbVerticalOpts, 3, 0, nullptr, 0,
                "Which side (below/above) to balance against the target."},
        {ERowType::Slider, "Radius Filter", nullptr, false, false, 0.0f, 1000.0f, 1.0f, 500.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Search radius (units) for the balance target."},
        {ERowType::Slider, "Stand Radius", nullptr, false, false, 1.0f, 10.0f, 1.0f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Distance (units) at which the bot stops and stands."},
        // v1.56.275: "Team" -> "DDrace Team" + Target ID + new "Team" filter.
        {ERowType::Dropdown, "DDrace Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_DDraceTeamOpts, 4, 0, nullptr, 0,
                "DDrace team filter for the target."},
        {ERowType::Input, "Target ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Explicit target id (used when DDrace Team = Target)."},
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the target."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0,
                "Include or ignore dummies."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        // v1.56.277: Off while freeze — pause the bot while we are frozen.
        {ERowType::Toggle, "Off while freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Pause the bot while you are frozen."},
        // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
        {ERowType::Toggle, "FNG Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Treat ninja as frozen (FNG does not sync freeze)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
};
// v1.56.83: AimBot expandable — 15 children.
static SRow g_AimBotChildren[] = {
        {ERowType::Dropdown, "Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABWeaponOpts, 7, 0, nullptr, 0,
                "Which weapon's AimBot settings are being edited."},
        // v1.56.291: Entity Mode — visible only for Weapon = Hook. Tee = players;
        // the rest target the matching map entities (blocks).
        {ERowType::Dropdown, "Entity Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EntityModeOpts, 6, 0, nullptr, 0,
                "What the Hook aims at: Tee = players; Hookable/Unhookable/Freeze/Death/Others = the matching map entities (blocks)."},
        {ERowType::Toggle, "Enable", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Enable the AimBot for the selected weapon."},
        {ERowType::Slider, "FOV", nullptr, false, false, 1.0f, 360.0f, 1.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Aiming field of view (degrees)."},
        {ERowType::Slider, "Radius", nullptr, false, false, 1.0f, 2000.0f, 1.0f, 300.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Maximum aiming distance (units)."},
        {ERowType::Toggle, "Show FOV", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the FOV cone."},
        {ERowType::Toggle, "Show Radius", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the aim radius circle."},
        // v1.56.276: Auto Switch Weapon — switch to this weapon when it has a target in its zone.
        {ERowType::Toggle, "Auto Switch Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Switch to this weapon when a target enters its zone."},
        {ERowType::Toggle, "Use raycast (Show)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Use a raycast for the displayed aim."},
        {ERowType::Dropdown, "Rules", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABRulesOpts, 3, 0, nullptr, 0,
                "Line-of-sight rules (None / Insert line / Raycast)."},
        {ERowType::Toggle, "Use raycast angle (AB)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Use the raycast angle for the actual aim."},
        // v1.56.192: AimBot Predict — iterative time-of-flight prediction (NOT in TriggerBot).
        {ERowType::Toggle, "Predict", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Predict the target's position by time-of-flight."},
        // v1.56.248: AB Jitter — randomize the aim point within the player hitbox
        // (raycast-angle mode: random RAY THAT HIT the player + random distance).
        {ERowType::Slider, "Jitter", nullptr, false, false, 0.0f, 100.0f, 1.0f, 0.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Randomize the aim point within the target hitbox (%)."},
        // v1.56.262: Silent toggle + Mode dropdown (Edge/Trigger/Always/Smooth).
        {ERowType::Toggle, "Silent", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Aim silently — your own view stays unchanged."},
        {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABModeOpts, 4, 0, nullptr, 0,
                "Aim mode: Edge / Trigger / Always / Smooth."},
        {ERowType::Slider, "Smooth Speed", nullptr, false, false, 0.1f, 50.0f, 0.1f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Smoothing speed for the Smooth mode."},
        {ERowType::Toggle, "Center snap", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Snap exactly to the target center."},
        // v1.56.275: "Team" — Both/My/War/Blue/Red.
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the target."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0,
                "Include or ignore dummies."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        // v1.56.277: Off while freeze — pause the bot while we are frozen.
        {ERowType::Toggle, "Off while freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Pause the bot while you are frozen."},
        // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
        {ERowType::Toggle, "FNG Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Treat ninja as frozen (FNG does not sync freeze)."},
        {ERowType::Dropdown, "Priority", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABPriorityOpts, 3, 0, nullptr, 0,
                "Target priority (nearest FOV / aim / to me)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
};
// v1.56.292: TriggerBot expandable — Weapon has 7 entries now (hook included);
// the Trigger fire/hook dropdown is gone (weapons 0..5 fire, Hook hooks).
static SRow g_TriggerBotChildren[] = {
        {ERowType::Dropdown, "Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABWeaponOpts, 7, 0, nullptr, 0,
                "Which weapon's TriggerBot settings are being edited."},
        {ERowType::Toggle, "Enable", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Enable the TriggerBot for the selected weapon."},
        {ERowType::Dropdown, "Trigger mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTriggerModeOpts, 3, 0, nullptr, 0,
                "When the trigger fires: One tick = a single shot per target entry; Hold = keeps firing while the target stays in the zone (required for FNG); Every tick = re-trigger every tick (best for hammer)."},
        // v1.56.291: Entity Mode — visible only for Weapon = Hook.
        {ERowType::Dropdown, "Entity Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EntityModeOpts, 6, 0, nullptr, 0,
                "What the Hook targets: Tee = players; Hookable/Unhookable/Freeze/Death/Others = the matching map entities (blocks)."},
        {ERowType::Slider, "FOV", nullptr, false, false, 1.0f, 360.0f, 1.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Trigger field of view (degrees)."},
        {ERowType::Slider, "Radius", nullptr, false, false, 1.0f, 2000.0f, 1.0f, 300.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Maximum trigger distance (units)."},
        {ERowType::Toggle, "Show FOV", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the FOV cone."},
        {ERowType::Toggle, "Show Radius", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the trigger radius circle."},
        // v1.56.276: Auto Switch Weapon — switch to this weapon when it has a target in its zone.
        {ERowType::Toggle, "Auto Switch Weapon", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Switch to this weapon when a target enters its zone."},
        {ERowType::Toggle, "Use raycast (Show)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Use a raycast for the displayed aim."},
        {ERowType::Dropdown, "Rules", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABRulesOpts, 3, 0, nullptr, 0,
                "Line-of-sight rules (None / Insert line / Raycast)."},
        {ERowType::Slider, "Latency", nullptr, false, false, 0.0f, 10.0f, 1.0f, 0.0f, " ticks", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Delay the trigger by N ticks."},
        // v1.56.275: "Team" — Both/My/War/Blue/Red.
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the target."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0,
                "Include or ignore dummies."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        // v1.56.277: Off while freeze — pause the bot while we are frozen.
        {ERowType::Toggle, "Off while freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Pause the bot while you are frozen."},
        // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
        {ERowType::Toggle, "FNG Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Treat ninja as frozen (FNG does not sync freeze)."},
        {ERowType::Dropdown, "Priority", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABPriorityOpts, 3, 0, nullptr, 0,
                "Target priority (nearest FOV / aim / to me)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
        // v1.56.279: Use silent channel AB — while the AimBot's silent aim is
        // in action, this weapon's zone (and Show FOV/Radius) follows the
        // silent direction instead of the crosshair.
        {ERowType::Toggle, "Use silent channel AB", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Follow the AimBot's silent direction for this weapon's zone."},
};
// ============================================================================
// v1.56.282: Dummy Manager panel (replaces the old 8-dummy g_DummiesChildren).
// MAX_DUMMIES = 128 (engine/client/enums.h); slot 0 = Main. Rows with per-dummy
// names are runtime-initialized once by InitDummyManagerRows() (str_format for
// up to 127 dummies); g_DummyMgrRows indices are DUMMYMGR_ROW_*.
// ============================================================================

// Dummies expandable: "Connect Dummy" + "Switch to Main" + D%d Connect/Switch.
static SRow g_DummyConnectChildren[2 + MAX_DUMMIES - 1];
// Proxy manager: ToggleInput per dummy (idx 0 = "Main").
static SRow g_DummyProxyChildren[MAX_DUMMIES];
// Dummy Profiles / Dummy Tee (259 rows each): "Edit Mode" dropdown, "Open Main",
// 128 target toggles ("Main" + "Dummy 1..127"), "Open Targets", 127 per-dummy
// "Open Dummy %d" buttons, "Open All".
static SRow g_DummyProfilesChildren[4 + MAX_DUMMIES + (MAX_DUMMIES - 1)];
static SRow g_DummyTeeChildren[4 + MAX_DUMMIES + (MAX_DUMMIES - 1)];
// v1.56.282: masters — InitDummyManagerRows fills these; UpdateDummyManagerRows
// compacts the visible subset into the arrays above (visibility by compaction
// + childCount, so no per-site child-visibility checks are needed).
static SRow g_DummyProfilesMaster[4 + MAX_DUMMIES + (MAX_DUMMIES - 1)];
static SRow g_DummyTeeMaster[4 + MAX_DUMMIES + (MAX_DUMMIES - 1)];

// Name buffers the runtime rows point into.
static char g_aDblConnectNames[MAX_DUMMIES][16]; // "D127 Connect"
static char g_aDblSwitchNames[MAX_DUMMIES][16]; // "D127 Switch"
static char g_aDummyRowNames[MAX_DUMMIES][12]; // "Main" / "Dummy 127"
static char g_aOpenProfNames[MAX_DUMMIES][24]; // "Open Dummy 127 Profile"
static char g_aOpenTeeNames[MAX_DUMMIES][20]; // "Open Dummy 127 Tee"

static const char *const g_DummyEditModeOpts[] = {"Per-Dummy", "All", "Targets"};

// g_DummyMgrRows indices.
enum
{
        DUMMYMGR_ROW_LIMIT = 0, // "Dummies" slider (runtime dummy limit)
        DUMMYMGR_ROW_CONNECT = 1, // "Dummies" expandable
        DUMMYMGR_ROW_PROXY = 2, // "Proxy manager" expandable
        DUMMYMGR_ROW_PROFILES = 3, // "Dummy Profiles" expandable
        DUMMYMGR_ROW_TEE = 4, // "Dummy Tee" expandable
        DUMMYMGR_ROW_APPLY = 5, // "Reconnect (Apply)" button
};

static bool g_DummyRowsInitialized = false;

static void InitDummyManagerRows()
{
        if(g_DummyRowsInitialized)
                return;
        g_DummyRowsInitialized = true;

        int n = 0;
        g_DummyConnectChildren[n++] = {ERowType::Button, "Connect Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Connect the dummy to the server."};
        g_DummyConnectChildren[n++] = {ERowType::Button, "Switch to Main", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Make this dummy the main (anchor)."};
        for(int D = 1; D < MAX_DUMMIES; ++D)
        {
                str_format(g_aDblConnectNames[D], sizeof(g_aDblConnectNames[D]), "D%d Connect", D);
                str_format(g_aDblSwitchNames[D], sizeof(g_aDblSwitchNames[D]), "D%d Switch", D);
                g_DummyConnectChildren[n++] = {ERowType::DoubleButton, g_aDblConnectNames[D], g_aDblSwitchNames[D], false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                        "Connect / disconnect this dummy.", "Switch control to this dummy."};
        }

        for(int D = 0; D < MAX_DUMMIES; ++D)
        {
                if(D == 0)
                        str_copy(g_aDummyRowNames[D], "Main", sizeof(g_aDummyRowNames[D]));
                else
                        str_format(g_aDummyRowNames[D], sizeof(g_aDummyRowNames[D]), "Dummy %d", D);
                g_DummyProxyChildren[D] = {ERowType::ToggleInput, g_aDummyRowNames[D], nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, "u:p@socks5:proxy", nullptr, 0, 0, nullptr, 0,
                        "SOCKS5 proxy for this dummy (user:pass@host:port). Toggle to enable."};
        }

        for(int pass = 0; pass < 2; ++pass)
        {
                SRow *pRows = (pass == 0) ? g_DummyProfilesMaster : g_DummyTeeMaster;
                const bool Tee = pass != 0;
                int m = 0;
                pRows[m++] = {ERowType::Dropdown, "Edit Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_DummyEditModeOpts, 3, 0, nullptr, 0,
                        "Edit scope: Per-Dummy / All / Targets."};
                if(Tee)
                        pRows[m++] = {ERowType::Button, "Open Main Tee", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Open the Main dummy's tee."};
                else
                        pRows[m++] = {ERowType::Button, "Open Main Profile", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Open the Main dummy's profile."};
                for(int D = 0; D < MAX_DUMMIES; ++D)
                        pRows[m++] = {ERowType::Toggle, g_aDummyRowNames[D], nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Include this dummy in the current edit scope."};
                if(Tee)
                        pRows[m++] = {ERowType::Button, "Open Targets Tee", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Open the combined tee for the selected targets."};
                else
                        pRows[m++] = {ERowType::Button, "Open Targets Profile", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Open the combined profile for the selected targets."};
                for(int D = 1; D < MAX_DUMMIES; ++D)
                {
                        if(Tee)
                        {
                                str_format(g_aOpenTeeNames[D], sizeof(g_aOpenTeeNames[D]), "Open Dummy %d Tee", D);
                                pRows[m++] = {ERowType::Button, g_aOpenTeeNames[D], nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                        "Open this dummy's tee."};
                        }
                        else
                        {
                                str_format(g_aOpenProfNames[D], sizeof(g_aOpenProfNames[D]), "Open Dummy %d Profile", D);
                                pRows[m++] = {ERowType::Button, g_aOpenProfNames[D], nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                        "Open this dummy's profile."};
                        }
                }
                if(Tee)
                        pRows[m++] = {ERowType::Button, "Open All Tee", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Open tees for all dummies."};
                else
                        pRows[m++] = {ERowType::Button, "Open All Profile", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                                "Open profiles for all dummies."};
        }

        // v1.56.282: initial copy into the display arrays (UpdateDummyManagerRows
        // re-compacts them every frame).
        mem_copy(g_DummyProfilesChildren, g_DummyProfilesMaster, sizeof(g_DummyProfilesChildren));
        mem_copy(g_DummyTeeChildren, g_DummyTeeMaster, sizeof(g_DummyTeeChildren));
}

// v1.56.282: Dummy Manager top-level rows. Slider "Dummies" = runtime dummy
// limit incl. Main (enforced engine-side via IClient::SetDummyLimit);
// "Reconnect (Apply)" is visible only while proxy changes are staged.
static SRow g_DummyMgrRows[] = {
        {ERowType::Slider, "Dummies", nullptr, false, false, 1.0f, (float)MAX_DUMMIES, 1.0f, 8.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Number of dummies (including Main) to keep connected."},
        {ERowType::Expandable, "Dummies", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_DummyConnectChildren, 2 + MAX_DUMMIES - 1,
                "Connect / disconnect and switch control of individual dummies."},
        {ERowType::Expandable, "Proxy manager", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_DummyProxyChildren, MAX_DUMMIES,
                "Per-dummy SOCKS5 proxies (staged changes apply on Reconnect)."},
        {ERowType::Expandable, "Dummy Profiles", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_DummyProfilesChildren, 4 + MAX_DUMMIES + (MAX_DUMMIES - 1),
                "Open a dummy's saved config profile."},
        {ERowType::Expandable, "Dummy Tee", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_DummyTeeChildren, 4 + MAX_DUMMIES + (MAX_DUMMIES - 1),
                "Open a dummy's tee (skin/config)."},
        {ERowType::Button, "Reconnect (Apply)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Reconnect the dummies to apply staged proxy changes."},
};
// v1.56.258: Block expandable — compact config (was 37 rows). Movement, fire,
// hook and rescue are driven by the Block pathfinder (physical beam search);
// Auto Aim/Fire/Hook/Hammer/Move/Stand are always on now, the rest of the old
// toggles is gone. "All Target"/"All Rescue" turn the ID fields into exclusion
// lists (renamed to "Untarget IDs"/"Unrescue IDs" in sync). "Dists:" is a label.
static SRow g_HelpersChildren[] = {
        {ERowType::Toggle, "Auto Main", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Automatically keep the main dummy under control."},
        {ERowType::Input, "Main ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Dummy id treated as the main (anchor) dummy."},
        {ERowType::Toggle, "All Target", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Target every valid player instead of the Target IDs list."},
        {ERowType::Input, "Target IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when All Target is off)."},
        {ERowType::Toggle, "Rescue", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Rescue frozen teammates (unfreeze / hook them out)."},
        {ERowType::Toggle, "All Rescue", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Rescue every frozen teammate instead of the Rescue IDs list."},
        {ERowType::Input, "Rescue IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Rescue id list (used when All Rescue is off)."},
        {ERowType::Input, "Dummy IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "0,1,2,3,4,5,6,7", "0,1,2...", nullptr, 0, 0, nullptr, 0,
                "Dummy ids the bot is allowed to use."},
        {ERowType::Label, "Dists:", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Distance thresholds for fire / hook / target / rescue."},
        {ERowType::Slider, "Fire Dist", nullptr, false, false, 32.0f, 64.0f, 1.0f, 50.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Distance (units) at which the bot fires at the target."},
        {ERowType::Slider, "Hook Dist", nullptr, false, false, 64.0f, 400.0f, 1.0f, 400.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Distance (units) at which the bot uses the hook."},
        {ERowType::Input, "Main Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "100000", "value...", nullptr, 0, 0, nullptr, 0,
                "Max distance the main dummy may keep from you."},
        {ERowType::Input, "Target Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "300", "value...", nullptr, 0, 0, nullptr, 0,
                "Max distance at which the bot engages a target."},
        {ERowType::Input, "Rescue Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "500", "value...", nullptr, 0, 0, nullptr, 0,
                "Max distance to reach a rescue target."},
        {ERowType::Input, "Stand Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "64", "value...", nullptr, 0, 0, nullptr, 0,
                "Distance at which the bot stops moving."},
        {ERowType::Input, "MStand Dist", nullptr, false, false, 0, 0, 0, 0, nullptr, "128", "value...", nullptr, 0, 0, nullptr, 0,
                "Standing distance for the main dummy."},
};
// ---------- Children shared by the Functions and Visuals panels ----------
// "Laser unfreeze" expandable: predictive laser self-unfreeze.
static SRow g_LaserUnfreezeChildren[] = {
        {ERowType::Toggle, "Auto laser", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Automatically aim the laser to unfreeze yourself."},
        {ERowType::Slider, "FOV", nullptr, false, false, 5.0f, 360.0f, 5.0f, 90.0f, "°", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Laser search field of view (degrees)."},
        {ERowType::Slider, "Angles", nullptr, false, false, 1.0f, 144.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Number of candidate angles to scan."},
        {ERowType::Slider, "Ticks", nullptr, false, false, 1.0f, 50.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Ticks to simulate ahead for the laser prediction."},
        {ERowType::Slider, "Trigger ticks", nullptr, false, false, 1.0f, 5.0f, 1.0f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Ticks the predicted unfreeze must hold before firing."},
        {ERowType::Toggle, "Silent", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Aim the laser silently (your own view stays unchanged)."},
        {ERowType::Toggle, "Show attempt", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the attempted laser prediction."},
};
// "Fake Aim" expandable.
// v1.56.247: Chaos mode (jitters the real aim; Speed = re-roll cadence,
// Jitter = max random offset in units) and Real mode (sends the real aim
// unchanged — laser unfreeze / aimbot keep priority). "Distance from center"
// applies to ALL modes plus the real aim and the laser unfreeze aim channel.
static const char *const g_FakeAimModeOpts[] = {"Random", "Robot", "Spin", "Lag", "Chaos", "Real"};
static SRow g_FakeAimChildren[] = {
        {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_FakeAimModeOpts, 6, 0, nullptr, 0,
                "Fake aim mode: Random / Robot / Spin / Lag / Chaos / Real."},
        {ERowType::Slider, "Speed", nullptr, false, false, 1.0f, 100.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Re-roll cadence (lower = slower switching)."},
        {ERowType::Slider, "Jitter", nullptr, false, false, 1.0f, 100.0f, 1.0f, 20.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Max random offset of the fake aim (units)."},
        {ERowType::Slider, "Distance from center", nullptr, false, false, 0.0f, 500.0f, 5.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Offset the aim from the target center (units)."},
        {ERowType::Toggle, "Show for me", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Render the fake aim locally."},
        // v1.56.262: renders what the server actually sees (visible only with Show for me).
        {ERowType::Toggle, "Show Server-Side Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Render what the server actually sees instead of the local aim."},
        // v1.56.263: between ticks advance the rendered aim at frame rate;
        // Real mode renders exactly like no Fake Aim.
        {ERowType::Toggle, "Predict", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Advance the rendered aim between ticks at frame rate."},
};
// v1.56.178: "Trajectory" expandable — per-type settings (Tee/Pistol/Shotgun/Grenade/Laser).
// Type dropdown selects which type's settings are being edited (same pattern as
// AimBot per-weapon dropdown). All toggles/sliders read/write the selected type's
// STypeSettings in CTrajectory. "Show for current" is hidden when Type=Tee (no
// projectiles for the tee type — it predicts tee movement only).
static const char *const g_TrajTypeOpts[] = {"Tee", "Pistol", "Shotgun", "Grenade", "Laser"};
static SRow g_TrajectoryChildren[] = {
        {ERowType::Dropdown, "Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TrajTypeOpts, 5, 0, nullptr, 0,
                "Which projectile type's settings are being edited."},
        {ERowType::Toggle, "Show", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the trajectory for the selected type."},
        {ERowType::Slider, "Prediction Ticks", nullptr, false, false, 1.0f, 50.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Ticks to simulate the projectile forward."},
        {ERowType::Toggle, "Alpha Gradient", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Fade the trajectory along its length."},
        {ERowType::Toggle, "Simulate Players", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Include player collisions in the simulation."},
        {ERowType::Toggle, "Show for other players", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw predicted trajectories for other players."},
        {ERowType::Toggle, "Show for current", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the trajectory for the currently held projectile."},
};
// v1.56.104: "ESP" expandable — lines from point A to all players matching filters.
// Filter option arrays reuse the AimBot ones (identical option strings).
// v1.56.254: renamed "ESP Style"/"Speed" to "Pointer Style"/"Pointer Speed",
// added "Box Style"/"Box Speed" (square outline around targets), stripped the
// redundant "ESP" prefix from child names (sync/write branches are parent-gated
// to "ESP" so the generic names cannot collide with other panels).
// v1.56.256: "Pointer"/"Box" visibility toggles, "X/Y Box scale" sliders, and the
// new "Hitboxes" expandable right after ESP.
static const char *const g_EspModeOpts[] = {"Active player", "Screen coordinates"};
static const char *const g_EspStyleOpts[] = {"Line", "Arrow", "Dotted line", "Dotted Arrow"};
static const char *const g_EspBoxStyleOpts[] = {"Box", "Dotted box"};
static SRow g_EspChildren[] = {
        // v1.56.256: "Pointer"/"Box" visibility toggles.
        {ERowType::Toggle, "Pointer", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw a pointer line to targets."},
        {ERowType::Dropdown, "Pointer Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EspStyleOpts, 4, 0, nullptr, 0,
                "Pointer line style."},
        {ERowType::Slider, "Pointer Speed", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Pointer animation speed."},
        {ERowType::Toggle, "Box", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw a box around targets."},
        {ERowType::Dropdown, "Box Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EspBoxStyleOpts, 2, 0, nullptr, 0,
                "Box outline style."},
        {ERowType::Slider, "Box Speed", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Box animation speed."},
        // v1.56.256: box size scale (X/Y — box dimensions, not line width).
        {ERowType::Slider, "X Box scale", nullptr, false, false, 10.0f, 1000.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Box width scale (%)."},
        {ERowType::Slider, "Y Box scale", nullptr, false, false, 10.0f, 1000.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Box height scale (%)."},
        // v1.56.275: "Team" -> "DDrace Team" + Target ID + new "Team" filter.
        {ERowType::Dropdown, "DDrace Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_DDraceTeamOpts, 4, 0, nullptr, 0,
                "DDrace team filter for the target."},
        {ERowType::Input, "Target ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Explicit target id (used when DDrace Team = Target)."},
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the target."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0,
                "Include or ignore dummies."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
        {ERowType::Toggle, "FNG Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Treat ninja as frozen (FNG does not sync freeze)."},
        {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_EspModeOpts, 2, 0, nullptr, 0,
                "Pointer anchor: target's active player or fixed screen coordinates."},
        {ERowType::Slider, "X", nullptr, false, false, 0.0f, 7680.0f, 1.0f, 0.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Screen X anchor (Screen coordinates mode)."},
        {ERowType::Slider, "Y", nullptr, false, false, 0.0f, 4320.0f, 1.0f, 0.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Screen Y anchor (Screen coordinates mode)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
};
// v1.56.256: "Hitboxes" expandable — tee tile-collision square (28x28) and the
// round trigger hitbox (radius 28, lasers/shotgun/hook) drawn on players.
// The two "Hitbox Style" dropdowns / "Box Speed" sliders are intentional label
// duplicates — sync/write code disambiguates them by child index.
static const char *const g_HitboxStyleOpts[] = {"Filled", "Box", "Dotted Box"};
static SRow g_HitboxesChildren[] = {
        {ERowType::Toggle, "Tee tile", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the tee tile-collision square (28x28)."},
        {ERowType::Dropdown, "Hitbox Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_HitboxStyleOpts, 3, 0, nullptr, 0,
                "Style of the tee-tile hitbox."},
        {ERowType::Slider, "Box Speed", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Animation speed of the tile hitbox."},
        {ERowType::Toggle, "Tee trigger", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the round trigger hitbox (radius 28)."},
        {ERowType::Dropdown, "Hitbox Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_HitboxStyleOpts, 3, 0, nullptr, 0,
                "Style of the trigger hitbox."},
        {ERowType::Slider, "Box Speed", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Animation speed of the trigger hitbox."},
        {ERowType::Toggle, "Show for others", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Also draw the hitboxes for other players."},
        // v1.56.257: full filter set (same as ESP/AimBot), visible when Show for others.
        // v1.56.275: "Team" -> "DDrace Team" + Target ID + new "Team" filter.
        {ERowType::Dropdown, "DDrace Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_DDraceTeamOpts, 4, 0, nullptr, 0,
                "DDrace team filter for the target."},
        {ERowType::Input, "Target ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Explicit target id (used when DDrace Team = Target)."},
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the target."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Dummy", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABDummyOpts, 3, 0, nullptr, 0,
                "Include or ignore dummies."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        // v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
        {ERowType::Toggle, "FNG Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Treat ninja as frozen (FNG does not sync freeze)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
};
// v1.56.121: "Avoid Freeze" expandable.
static SRow g_AvoidFreezeChildren[] = {
        {ERowType::Toggle, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Override input while frozen to escape the freeze."},
        {ERowType::Toggle, "Teleport", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Emit teleport input to escape the freeze."},
        {ERowType::Toggle, "Death", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Emit death input to escape the freeze."},
        {ERowType::Toggle, "Direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Emit direction input to escape the freeze."},
        {ERowType::Toggle, "Jump", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Emit jump input to escape the freeze."},
        {ERowType::Toggle, "Hook", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Emit hook input to escape the freeze."},
        {ERowType::Toggle, "Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Emit aim input to escape the freeze."},
        {ERowType::Slider, "FOV", nullptr, false, false, 5.0f, 360.0f, 5.0f, 90.0f, "\xc2\xb0", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Field of view for detecting the freeze tile (degrees)."},
        {ERowType::Slider, "Angles", nullptr, false, false, 1.0f, 144.0f, 1.0f, 9.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Number of candidate angles to try."},
        {ERowType::Toggle, "Silent", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Send the escape inputs silently (your own view stays unchanged)."},
        {ERowType::Slider, "Ticks", nullptr, false, false, 1.0f, 20.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Ticks over which the escape input is sent."},
        {ERowType::Slider, "Ticks Before", nullptr, false, false, 1.0f, 10.0f, 1.0f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Ticks before the freeze to pre-empt it."},
};
// v6.7: Smart Copy Moves expandable — header is the master toggle. Children:
// sort/apply dropdowns + top-N slider + ESP-style filters (no Dummy filter —
// the candidates are our own dummies).
static const char *const g_SmartSortOpts[] = {"ID", "Nearest To Me", "Nearest To Aim", "Last Active"};
static const char *const g_SmartApplyOpts[] = {"Trigger", "Always"};
static SRow g_SmartCopyMovesChildren[] = {
        {ERowType::Dropdown, "Sort Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_SmartSortOpts, 4, 0, nullptr, 0,
                "How the top dummies are picked."},
        {ERowType::Dropdown, "Apply Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_SmartApplyOpts, 2, 0, nullptr, 0,
                "Trigger: capture the top on copy moves enable; Always: recompute every tick."},
        {ERowType::Slider, "Apply Dummies", nullptr, false, false, 1.0f, 7.0f, 1.0f, 2.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "How many top dummies receive copy moves (max follows the dummy limit)."},
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the dummy."},
        {ERowType::Dropdown, "DDrace Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_DDraceTeamOpts, 4, 0, nullptr, 0,
                "DDrace team filter for the dummy."},
        {ERowType::Input, "Target ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Explicit DDrace team id (used when DDrace Team = Target)."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
};

// v1.56.260: Functions panel merged with the former Advanced rows (Laser unfreeze,
// Fake Aim, Basic Avoid Freeze). Trajectory/ESP/Hitboxes moved to the Visuals
// panel; Random Aim rows removed (feature deleted).
static SRow g_FunctionsRows[] = {
        // v1.56.282: "Dummies" expandable moved to the Dummy Manager panel.
        {ERowType::Expandable, "Helpers", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_HelpersChildren, 16,
                "Auto bot: aim, fire, hook, hammer and rescue targets."},
        // v1.56.83: AimBot + TriggerBot expandables (AimBot: 23 children since v1.56.262)
        {ERowType::Expandable, "AimBot", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AimBotChildren, 27,
                "Aim assist, configured per weapon."},
        {ERowType::Expandable, "TriggerBot", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TriggerBotChildren, 23,
                "Auto-fire when a target enters the weapon's zone."},
        // v1.56.262: Auto Fire + BalanceBot expandables.
        {ERowType::Expandable, "Auto Fire", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AutoFireChildren, 1,
                "Automatically fire at a fixed tick rate."},
        {ERowType::Expandable, "BalanceBot", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BalanceBotChildren, 14,
                "Balance against a target by standing above/below it."},
        // Fly sub-section
        {ERowType::Toggle, "Preserve input", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Keep your own input while a fly/hammer feature is active."},
        {ERowType::Expandable, "Custom latency", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CustomLatencyChildren, 1,
                "Add artificial input latency."},
        // v6.7: Auto fly is a plain toggle — it only checks whether the next tick
        // can really hit and fires exactly then (no distance/predict settings).
        {ERowType::Toggle, "Auto fly", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Hammer only when the next tick can really hit; aim is set on the hit."},
        {ERowType::Toggle, "Fix Dummy Hammer", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Keep the dummy on the hammer while hammering (fixes Copy Moves switching it to the copied weapon)."},
        {ERowType::Expandable, "Auto Triple Fly", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TripleFlyChildren, 6,
                "Triple-fly anchored to a dummy."},
        // Copy Moves sub-section
        // v6.7: Smart Copy Moves — restrict copy moves to the top-N dummies.
        {ERowType::Expandable, "Smart Copy Moves", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SmartCopyMovesChildren, 11,
                "Apply copy moves only to the top-N dummies (header = master toggle)."},
        {ERowType::Expandable, "Copy Moves Filter", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CopyMovesFilterChildren, 6,
                "Choose which inputs are excluded from copied moves."},
        {ERowType::Toggle, "Mirror direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Mirror the followed player's movement direction."},
        {ERowType::Dropdown, "Aim mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AimModeOpts, 2, 0, nullptr, 0,
                "How the copied aim is applied (Direct / Mirror)."},
        // Conditional aim options (visibility based on Aim mode)
        {ERowType::Toggle, "Mirror aim X", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Mirror the aim X axis of the followed player."},
        {ERowType::Toggle, "Mirror aim Y", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Mirror the aim Y axis of the followed player."},
        {ERowType::Toggle, "Smart aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Smart-remap the copied aim to match your position."},
        // v1.56.151: Fly Ride — pilot hooks/hammers nearest dummy, WASD moves anchor.
        {ERowType::Toggle, "Fly Ride", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "PROTOTYPE / in development — ride the nearest dummy (WASD moves the anchor)."},
        // v1.56.186: Copy Moves Latency — staggered input replay delay for inactive dummies.
        {ERowType::Expandable, "Copy Moves Latency", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CopyMovesLatencyChildren, 1,
                "Staggered input-replay delay for inactive dummies."},
        // v1.56.260: former Advanced rows (Trajectory/ESP/Hitboxes now live in Visuals).
        {ERowType::Expandable, "Laser unfreeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_LaserUnfreezeChildren, 7,
                "Predictive laser self-unfreeze."},
        {ERowType::Expandable, "Fake Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_FakeAimChildren, 7,
                "Spoof the aim the server sees (fake aim)."},
        {ERowType::Expandable, "Avoid Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AvoidFreezeChildren, 12,
                "Auto-escape a freeze by overriding the input."},
};

// ---------- Settings panel (6) ----------
// "IRC" expandable: 2 sub-toggles.
static SRow g_IrcChildren[] = {
        {ERowType::Toggle, "Reveal join", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Announce locally when an IRC friend joins."},
        {ERowType::Toggle, "Nameplate tag", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show an IRC tag on friends' nameplates."},
};
// "Spoofer" expandable: slider + dropdown + input.
static const char *const g_ClientPresetOpts[] = {"DDNet", "TClient", "BestClient", "Custom"};
static SRow g_SpooferChildren[] = {
        {ERowType::Slider, "Version ID", nullptr, false, false, 10000.0f, 21000.0f, 1.0f, 20000.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Spoofed DDNet version id (10000-21000)."},
        {ERowType::Dropdown, "Client preset", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ClientPresetOpts, 4, 0, nullptr, 0,
                "Which client-identity preset to spoof."},
        {ERowType::Input, "Custom client name", nullptr, false, false, 0, 0, 0, 0, nullptr, "MyClient", "name...", nullptr, 0, 0, nullptr, 0,
                "Custom client name (preset = Custom)."},
        {ERowType::Input, "Client version", nullptr, false, false, 0, 0, 0, 0, nullptr, "20.0", "version...", nullptr, 0, 0, nullptr, 0,
                "Spoofed client version string."},
        {ERowType::Toggle, "Git hash", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Spoof the git hash reported to the server."},
        {ERowType::Input, "Git hash value", nullptr, false, false, 0, 0, 0, 0, nullptr, "abc1234", "hash...", nullptr, 0, 0, nullptr, 0,
                "The git hash value to spoof."},
        // v1.56.174: BestClient extra NETMSGs toggle (visible only when preset=BestClient, see SpooferChildVisible).
        {ERowType::Toggle, "Extra NETMSGs", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Send BestClient's extra NETMSGs."},
};
// v1.56.273: "Spoof Zoom" expandable: fake zoom slider (header = master toggle).
static SRow g_SpoofZoomChildren[] = {
        {ERowType::Slider, "Fake Zoom", nullptr, false, false, 1.0f, 50.0f, 0.1f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Spoofed zoom level reported to the server."},
};
// v1.56.273: "Spoof Spectate" expandable: mode + fake fields (header = master toggle).
static const char *const g_SpoofSpectateModeOpts[] = {"Follow Coordinates", "Follow Player"};
static SRow g_SpoofSpectateChildren[] = {
        {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_SpoofSpectateModeOpts, 2, 0, nullptr, 0,
                "Follow fixed coordinates or a player's position."},
        {ERowType::Input, "Fake spectate X", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "x...", nullptr, 0, 0, nullptr, 0,
                "Spoofed spectate X coordinate."},
        {ERowType::Input, "Fake spectate Y", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "y...", nullptr, 0, 0, nullptr, 0,
                "Spoofed spectate Y coordinate."},
        {ERowType::Input, "Fake Follow ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Player id to follow (Follow Player mode)."},
};
// v1.56.283: g_RainbowChildren removed - "Rainbow Main Color" + "Speed" are now
// children of the "ClickGUI Settings" expandable.
// v1.56.89: "Line rendering" visual expandable (cannot toggle, only expand).
// Shared line render settings used by trajectory, aimbot FOV/radius, pathfinder path, laser unfreeze.
// v1.56.108: Line rendering per-component. Component dropdown selects which
// component's Size/Color/Opacity is being edited. Layer stays global.
// v1.56.210: Added Gradient toggle + Step slider per-component. Both visible
// only when Rainbow is ON (gradient has no effect without rainbow). Speed
// slider min lowered from 0.1 to 0 (frozen rainbow is now possible).
static const char *const g_LineComponentOpts[] = {"AimBot", "TriggerBot", "Trajectory", "Laser Unfreeze", "Pathfinder", "ESP", "TAS", "Hitboxes", "RT Pathfinder"};
static SRow g_LineRenderingChildren[] = {
        {ERowType::Dropdown, "Component", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_LineComponentOpts, 9, 0, nullptr, 0,
                "Which component's Size/Color/Opacity is being edited."},
        {ERowType::Input, "Line color", nullptr, false, false, 0, 0, 0, 0, nullptr, "ffbf00", "hex...", nullptr, 0, 0, nullptr, 0,
                "Line color of the selected component (hex RGB)."},
        {ERowType::Slider, "Line size", nullptr, false, false, 0.0f, 20.0f, 1.0f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Line width of the selected component."},
        {ERowType::Slider, "Line opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 80.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Line opacity of the selected component (%)."},
        {ERowType::Toggle, "Rainbow", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Animate the line color with a hue cycle."},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.0f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Rainbow hue rotation speed (0 = frozen)."},
        {ERowType::Toggle, "Gradient", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Fade the line along its length (requires Rainbow)."},
        {ERowType::Slider, "Step", nullptr, false, false, 1.0f, 180.0f, 1.0f, 15.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Gradient step size for the line."},
        {ERowType::Slider, "Layer", nullptr, false, false, 0.0f, 10.0f, 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw layer (z-order) of the line (global)."},
};
// v1.56.283: "ClickGUI Settings" expandable - all UI settings in one place.
// Header = master toggle (KxUiSettingsOn): off = built-in defaults.
static SRow g_ClickGuiSettingsChildren[] = {
        {ERowType::Input, "Main Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "9d4edd", "hex...", nullptr, 0, 0, nullptr, 0, "Accent color of the GUI (hex RGB). Setting a manual color turns the rainbow off."},
        {ERowType::Toggle, "Rainbow Main Color", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Continuously cycle the accent color hue."},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Rainbow hue rotation speed."},
        {ERowType::Slider, "Main Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Opacity of the accent color."},
        {ERowType::Input, "Panel Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "161622", "hex...", nullptr, 0, 0, nullptr, 0, "Background color of the panels (hex RGB)."},
        {ERowType::Slider, "Panel Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Opacity of the panel background."},
        {ERowType::Slider, "BG Blackout", nullptr, false, false, 0.0f, 100.0f, 1.0f, 30.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Darkens the game behind the panels (0-100%)."},
        // v1.56.287: BG Blur — blurs the game behind the panels (FBO-based).
        {ERowType::Toggle, "BG Blur", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Blurs the game behind the ClickGUI panels."},
        {ERowType::Slider, "Blur Radius", nullptr, false, false, 0.0f, 100.0f, 5.0f, 40.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Blur strength of the game behind the panels (visible when BG Blur is on)."},
        {ERowType::Slider, "ClickGUI Scale", nullptr, false, false, 0.1f, 2.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Overall scale of the whole ClickGUI."},
        {ERowType::Slider, "Panels Start X", nullptr, false, false, 0.0f, 1920.0f, 1.0f, 60.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Default X position of the panel grid."},
        {ERowType::Slider, "Panels Start Y", nullptr, false, false, 0.0f, 1080.0f, 1.0f, 90.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Default Y position of the panel grid."},
        {ERowType::Slider, "Panels X-indentation", nullptr, false, false, 0.0f, 200.0f, 1.0f, 30.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Horizontal spacing between the panels in the grid."},
        {ERowType::Slider, "Panels Y-indentation", nullptr, false, false, 0.0f, 200.0f, 1.0f, 12.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Vertical spacing between the panels in the grid."},
        {ERowType::Toggle, "Dynamic scale (Beta)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Dynamic panel sizing: drag the panel edges to resize. Hides the fixed width/height sliders."},
        {ERowType::Slider, "Panels width", nullptr, false, false, 150.0f, 800.0f, 1.0f, 300.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Fixed panel width in pixels (visible when Dynamic scale is off)."},
        {ERowType::Slider, "Panels height", nullptr, false, false, 200.0f, 2000.0f, 1.0f, 600.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Maximum visible body height of a panel (when Dynamic scale is off)."},
        {ERowType::Toggle, "Drag-To-Sort", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Drag a panel header onto another slot to swap their positions."},
        // v6.3: ToolTips master switch + per-part opacity.
        {ERowType::Toggle, "Show ToolTips", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Show the ClickGUI tooltips."},
        {ERowType::Slider, "ToolTips Text Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Opacity of the tooltip text."},
        {ERowType::Slider, "ToolTips Main Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Opacity of the tooltip border."},
        {ERowType::Slider, "ToolTips Panel Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Opacity of the tooltip background."},
};
static SRow g_SettingsRows[] = {
        {ERowType::Expandable, "ClickGUI Settings", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_ClickGuiSettingsChildren, 22, "Master switch: off = built-in default look."},
        {ERowType::Expandable, "Line rendering", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_LineRenderingChildren, 9,
                "Global line-render settings shared by all features."},
        {ERowType::Expandable, "IRC", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_IrcChildren, 2,
                "IRC friend join / nameplate options."},
        {ERowType::Expandable, "Spoofer", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SpooferChildren, 7,
                "Spoof the client identity sent to the server."},
        // v1.56.273: Spoof Zoom / Spoof Spectate (headers are master toggles).
        {ERowType::Expandable, "Spoof Zoom", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SpoofZoomChildren, 1,
                "Spoof the zoom level reported to the server."},
        {ERowType::Expandable, "Spoof Spectate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SpoofSpectateChildren, 4,
                "Spoof spectate coordinates / follow target."},
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
        {ERowType::Slider, "Ratio", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Freedom path ratio (x)."},
};
static char g_PfDyn_Progress[96];
static char g_PfDyn_Eta[64];
static char g_PfDyn_Time[96];
static bool g_PfShowRunLabels = false;
static bool g_PfShowStatLabels = false;

// v1.56.283: "Settings" visual expandable - effort-based knobs in one place.
// kx_pf_hook_angles removed entirely: the hook fan is always 36 rays.
static SRow g_PfSettingsChildren[] = {
        {ERowType::Slider, "A* Weight", nullptr, false, false, 1.0f, 20.0f, 0.01f, 3.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "A* path cost weight (requires Effort = Custom)."},
        {ERowType::Slider, "Evolutions", nullptr, false, false, 1.0f, 100.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "RHEA evolution count (Effort = Custom + Algorithm = RHEA)."},
        {ERowType::Slider, "Beam width", nullptr, false, false, 8.0f, 4096.0f, 1.0f, 128.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Beam search width (Effort = Custom + Algorithm = Beam)."},
        {ERowType::Slider, "Performance", nullptr, false, false, 1.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Pathfinder performance budget (0-100%)."},
        {ERowType::Toggle, "Freeze Support (Beta)", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Let the pathfinder route through freeze tiles."},
};
static SRow g_PathfinderRows[] = {
        {ERowType::Dropdown, "Effort", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_PfEffortOpts, 7, 0, nullptr, 0,
                "Search effort preset (Very Low … Max / Custom)."},
        {ERowType::Dropdown, "Algorithm", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_PfAlgorithmOpts, 4, 0, nullptr, 0,
                "Search algorithm (BFS / Beam / RHEA / BMCTS)."},
        {ERowType::Expandable, "Freedom Path", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_PfFreedomPathChildren, 1,
                "Allow freer paths through the search space."},
        {ERowType::Expandable, "Settings", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_PfSettingsChildren, 5, "Effort-based pathfinder knobs: visible rows depend on Effort/Algorithm."},
        {ERowType::Expandable, "Visuals", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_VisualsChildren, 4,
                "Pathfinder visualization toggles."},
        // v1.56.150: DoubleButton — Pathfinding (left) + Play (right, visible only when FINISHED).
        {ERowType::DoubleButton, "Pathfinding", "Paste", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Compute a path from your tee to the finish.", "Paste a path from the clipboard."},
        {ERowType::DoubleButton, "Editor Forbidden Tiles", "Clear Forbidden Tiles", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Open the forbidden-tiles editor.", "Clear all forbidden tiles."},
        {ERowType::Slider, "Tunnel size", nullptr, false, false, 0.0f, 10.0f, 1.0f, 2.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Size of the generated tunnel."},
        {ERowType::Button, "Generate tunnel", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Generate a tunnel using the tunnel size."},
        {ERowType::DoubleButton, "Editor Finish Tiles", "Clear Finish Tiles", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Open the finish-tiles editor.", "Clear all finish tiles."},
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
// v1.56.283: "Binds" visual expandable - all bind fields in one place, plus
// the former TAS expandable flattened with a "TAS " name prefix.
static SRow g_BindsMainChildren[] = {
        {ERowType::Input, "ClickGUI", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the ClickGUI."},
        {ERowType::Input, "AimBot", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the AimBot on/off."},
        {ERowType::Input, "TriggerBot", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the TriggerBot on/off."},
        {ERowType::Input, "Fake Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle Fake Aim."},
        {ERowType::Input, "Laser Unfreeze", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle laser unfreeze."},
        {ERowType::Input, "Copy Moves Filter", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the copy moves filter."},
        {ERowType::Input, "Copy Moves Latency", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle copy moves latency."},
        {ERowType::Input, "Trajectory", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the trajectory."},
        {ERowType::Input, "ESP", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the ESP."},
        {ERowType::Input, "Avoid Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle avoid freeze."},
        {ERowType::Input, "Helpers", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle the Helpers (Block) bot."},
        {ERowType::Input, "Auto Triple Fly", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle Auto Triple Fly."},
        {ERowType::Input, "Auto Fire", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle Auto Fire."},
        {ERowType::Input, "BalanceBot", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle BalanceBot."},
        {ERowType::Input, "TAS Record", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle TAS recording."},
        {ERowType::Input, "TAS Pause", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to toggle TAS pause."},
        {ERowType::Input, "TAS Rewind", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to trigger a TAS rewind."},
        {ERowType::Input, "TAS Forward", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0, "Bind a key to trigger a TAS forward."},
};

static SRow g_BindsEdgeChildren[] = {
        {ERowType::Input, "Buttons", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "a,b,c...", nullptr, 0, 0, nullptr, 0,
                "Buttons (comma-separated) that trigger the edge bind."},
        {ERowType::Slider, "Edge in ms", nullptr, false, false, 1.0f, 1000.0f, 10.0f, 100.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Edge-bind activation window (ms)."},
};
static const char *const g_BindsModeOpts[] = {"Merge", "Override"};
// v6.7: Smart Dummy Switch — candidate order + Without Repeat + filters.
static const char *const g_SdsOrderOpts[] = {"ID", "Nearest To Me", "Nearest To Aim", "Last Active"};
static SRow g_BindsSdsChildren[] = {
        {ERowType::Input, "Button", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "bind...", nullptr, 0, 0, nullptr, 0,
                "Button that triggers the smart dummy switch."},
        {ERowType::Dropdown, "Order Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_SdsOrderOpts, 4, 0, nullptr, 0,
                "How the next dummy is picked."},
        {ERowType::Toggle, "Without Repeat", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Don't revisit dummies until every candidate was visited once (new round after)."},
        {ERowType::Dropdown, "Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TeamFilterOpts, 5, 0, nullptr, 0,
                "Team filter for the switch candidates."},
        {ERowType::Dropdown, "DDrace Team", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_DDraceTeamOpts, 4, 0, nullptr, 0,
                "DDrace team filter for the switch candidates."},
        {ERowType::Input, "Target ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
                "Explicit DDrace team id (used when DDrace Team = Target)."},
        {ERowType::Dropdown, "Friend", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Friend filter (both / ignore / only)."},
        {ERowType::Dropdown, "Clanmate", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFriendOpts, 3, 0, nullptr, 0,
                "Clanmate filter (both / ignore / only)."},
        {ERowType::Dropdown, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABFreezeOpts, 3, 0, nullptr, 0,
                "Freeze-state filter (both / freeze / no freeze)."},
        {ERowType::Dropdown, "Targets", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ABTargetsFilterOpts, 3, 0, nullptr, 0,
                "Include only / ignore specific target ids."},
        {ERowType::Input, "Targets IDs", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "1,2,3...", nullptr, 0, 0, nullptr, 0,
                "Target id list (used when Targets = Only/Ignore)."},
};

static SRow g_BindsRows[] = {
        {ERowType::Dropdown, "Bind Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_BindsModeOpts, 2, 0, nullptr, 0,
                "Merge with existing binds or override them."},
        {ERowType::Expandable, "Binds", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BindsMainChildren, 18, "Bind keys to toggle features. Commits on Enter or on focus loss."},
        {ERowType::Expandable, "Edge bind", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BindsEdgeChildren, 2,
                "Bind mouse buttons to a toggle with an edge window."},
        {ERowType::Expandable, "Smart dummy switch", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_BindsSdsChildren, 11,
                "Cycle dummies with a bound button."},
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
static char g_InfoDyn_ExeHash[96];

// Index of the "Show spoofed" toggle in g_InfoRows (so UpdateInfoRows can read it).
static constexpr int INFO_ROW_SHOWSPOOFED = 0;
static constexpr int INFO_ROW_NAME = 1;
static constexpr int INFO_ROW_VERSION = 2;
static constexpr int INFO_ROW_HASH = 3;
static constexpr int INFO_ROW_KINETIX = 4;
static constexpr int INFO_ROW_FPS = 5;
static constexpr int INFO_ROW_EXEHASH = 6;
static constexpr int INFO_ROW_TELEGRAM = 7;
static constexpr int INFO_ROW_TIKTOK = 8;

static SRow g_InfoRows[] = {
        {ERowType::Toggle, "Show spoofed", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the spoofed client identity instead of the real one."},
        {ERowType::Label, g_InfoDyn_Name, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Version, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Hash, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, "Kinetix Version: " KINETIX_VERSION, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_Fps, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_InfoDyn_ExeHash, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, "Telegram", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Open the Kinetix Telegram channel."},
        {ERowType::Button, "TikTok", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Open the Kinetix TikTok."},
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
        {ERowType::Button, "Pick File", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Pick a Lua script file to execute."},
        {ERowType::Label, g_ExecDyn_Path, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Name, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Version, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Author, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Label, g_ExecDyn_Desc, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Button, g_ExecDyn_Btn, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Execute the loaded Lua script."},
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
                {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasFakeAimModeOpts, 6, 0, nullptr, 0,
                        "Rendered-aim mode used during TAS playback."},
        };
        // Auto Rewind/Forward: per-tile auto actions, Record mode only. The
        // Tile dropdown selects the tile being edited; Mode and the tick
        // slider are per tile (0 ticks = disabled).
        static const char *const g_TasAutoTileOpts[] = {"Freeze", "Teleport", "Death"};
        static const char *const g_TasAutoModeOpts[] = {"Rewind", "Forward"};
        static SRow g_TasAutoRfChildren[] = {
                {ERowType::Dropdown, "Tile", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasAutoTileOpts, 3, 0, nullptr, 0,
                        "Which tile type's auto action is being edited."},
                {ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasAutoModeOpts, 2, 0, nullptr, 0,
                        "Rewind or forward when the tile is hit."},
                {ERowType::Slider, "Rewind Ticks", nullptr, false, false, 0.0f, 100.0f, 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                        "Ticks to rewind/forward on the tile (0 = disabled)."},
        };
        static SRow g_TasVisualsChildren[] = {
                {ERowType::Toggle, "Show Path", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
                {ERowType::Toggle, "Show Hooks", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
                {ERowType::Toggle, "Show Speed", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
                {ERowType::Toggle, "Trajectory", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                        "Draw the TAS trajectory."},
        };

// ---------- Spawner (child of the TAS panel, .tas v2.0) ----------
static char g_TasDyn_EntityName[48] = "";
static char g_TasDyn_TileName[48] = "";
static char g_TasDyn_Loop[40] = "Loop Spawn";
static const char *const g_TasSpawnerModeOpts[] = {"Entity", "Tile"};
static const char *const g_TasSpawnerHookModeOpts[] = {"Strong", "Weak"};
static const char *const g_TasSpawnerCoordOpts[] = {"Cursor", "Coordinates", "Previous Spawn"};
static const char *const g_TasSpawnerCoordOptsNoPrev[] = {"Cursor", "Coordinates"};
// v6.2: direction presets for the non-tee entities (index = m_Dir).
static const char *const g_TasSpawnerDirOpts[] = {"East", "North-East", "North", "North-West", "West", "South-West", "South", "South-East"};
static const char *const g_TasSpawnerLaserTypeOpts[] = {"Rifle", "Shotgun (Freeze)"};
static const char *const g_TasSpawnerPickupTypeOpts[] = {"Health", "Armor", "Weapon", "Ninja"};

// Indices into g_TasSpawnerChildren (order below must match).
enum
{
	TAS_SPAWN_ROW_MODE = 0,
	TAS_SPAWN_ROW_ENTITY_ID,
	TAS_SPAWN_ROW_NAME,
	TAS_SPAWN_ROW_CLAN,
	TAS_SPAWN_ROW_ID,
	TAS_SPAWN_ROW_SKIN,
	TAS_SPAWN_ROW_CC_TEE,
	TAS_SPAWN_ROW_TEE_COLOR,
	TAS_SPAWN_ROW_CC_LEGS,
	TAS_SPAWN_ROW_LEGS_COLOR,
	TAS_SPAWN_ROW_HOOK_STRENGTH,
	TAS_SPAWN_ROW_HOOK_MODE,
	TAS_SPAWN_ROW_PRACTICE,
	TAS_SPAWN_ROW_WEAPON_ID,
	// v6.3: per-entity parameters.
	TAS_SPAWN_ROW_PROJECTILE_FREEZE,
	TAS_SPAWN_ROW_PROJECTILE_EXPLOSIVE,
	TAS_SPAWN_ROW_PROJECTILE_SOUND,
	TAS_SPAWN_ROW_LASER_TYPE,
	TAS_SPAWN_ROW_PICKUP_TYPE,
	TAS_SPAWN_ROW_SUBTYPE,
	TAS_SPAWN_ROW_SWITCH,
	TAS_SPAWN_ROW_APPLY_PHYSICS,
	TAS_SPAWN_ROW_PREV_X,
	TAS_SPAWN_ROW_PREV_Y,
	TAS_SPAWN_ROW_TIMELINE,
	TAS_SPAWN_ROW_TILE_ID,
	TAS_SPAWN_ROW_TILE_PHYSICS,
	TAS_SPAWN_ROW_TILE_NUMBER,
	// v6.2/6.3: physics & parameters of the non-tee entities (before the
	// placement / action rows so everything spawns-related sits together).
	TAS_SPAWN_ROW_DIR,
	TAS_SPAWN_ROW_VEL_X,
	TAS_SPAWN_ROW_VEL_Y,
	TAS_SPAWN_ROW_LIFESPAN,
	TAS_SPAWN_ROW_LASER_REACH,
	TAS_SPAWN_ROW_LASER_LENGTH,
	TAS_SPAWN_ROW_DRAGGER_STRENGTH,
	TAS_SPAWN_ROW_SPEEDUP_FORCE,
	TAS_SPAWN_ROW_SPEEDUP_MAX_SPEED,
	TAS_SPAWN_ROW_SPEEDUP_ANGLE,
	TAS_SPAWN_ROW_COORD_MODE,
	TAS_SPAWN_ROW_X,
	TAS_SPAWN_ROW_Y,
	TAS_SPAWN_ROW_SNAP,
	TAS_SPAWN_ROW_SPAWN,
	TAS_SPAWN_ROW_KILL,
	TAS_SPAWN_ROW_LOOP_DELAY,
	TAS_SPAWN_ROW_LOOP,
	TAS_SPAWN_ROW_COUNT,
};

static SRow g_TasSpawnerChildren[TAS_SPAWN_ROW_COUNT] = {
	{ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasSpawnerModeOpts, 2, 0, nullptr, 0,
		"Spawn an entity or a tile in the playground."},
	{ERowType::Slider, "Entity ID", nullptr, false, false, 0.0f, (float)(TAS_SPAWN_ENT_COUNT - 1), 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Entity type to spawn (the name is shown instead of the number)."},
	{ERowType::Input, "Name", nullptr, false, false, 0, 0, 0, 0, nullptr, "Spawned", "name...", nullptr, 0, 0, nullptr, 0,
		"Spawned tee name."},
	{ERowType::Input, "Clan", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "clan...", nullptr, 0, 0, nullptr, 0,
		"Spawned tee clan."},
	{ERowType::Input, "ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
		"Client id used by the spawned tee."},
	{ERowType::Input, "Skin ID", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "skin...", nullptr, 0, 0, nullptr, 0,
		"Skin name of the spawned tee."},
	{ERowType::Toggle, "Custom Color Tee", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Use a custom body color for the spawned tee."},
	{ERowType::Input, "Tee Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "hex...", nullptr, 0, 0, nullptr, 0,
		"Spawned tee body color (hex RGB)."},
	{ERowType::Toggle, "Custom Color Legs", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Use a custom feet color for the spawned tee."},
	{ERowType::Input, "Legs Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "hex...", nullptr, 0, 0, nullptr, 0,
		"Spawned tee feet color (hex RGB)."},
	{ERowType::Slider, "Hook Strength", nullptr, false, false, 0.0f, 100.0f, 1.0f, 10.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Strong/weak hook id of the spawned tee (0-100)."},
	{ERowType::Dropdown, "Hook Strength", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasSpawnerHookModeOpts, 2, 1, nullptr, 0,
		"Strong or weak hook for the spawned tee."},
	{ERowType::Toggle, "Practice Particles", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Draw the practice (super) particles around the spawned tee."},
	{ERowType::Slider, "Weapon ID", nullptr, false, false, -1.0f, 5.0f, 1.0f, 0.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Weapon the spawned tee owns and holds (-1 = no weapons at all)."},
	// v6.3: per-entity parameters.
	{ERowType::Toggle, "Freeze", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Projectile freezes tees on hit (freeze bullets)."},
	{ERowType::Toggle, "Explosive", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Projectile explodes on hit (grenades are always explosive)."},
	{ERowType::Toggle, "Sound", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Play the projectile fire sound."},
	{ERowType::Dropdown, "Laser Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasSpawnerLaserTypeOpts, 2, 0, nullptr, 0,
		"Rifle beam or shotgun (freeze) beam."},
	{ERowType::Dropdown, "Pickup Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasSpawnerPickupTypeOpts, 4, 0, nullptr, 0,
		"Type of the spawned pickup."},
	{ERowType::Input, "Subtype", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "id...", nullptr, 0, 0, nullptr, 0,
		"Pickup: weapon id / armor amount. Plasma: 1 = explosive, 2 = freeze, 3 = both."},
	{ERowType::Input, "Switch", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "0-255...", nullptr, 0, 0, nullptr, 0,
		"Switch layer number the entity belongs to (0 = always active; door / dragger / plasma / pickup)."},
	{ERowType::Toggle, "Apply Physics", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Apply physics to the spawned entity (off = hangs in the air, no collision)."},
	{ERowType::Input, "PrevPos X", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "x...", nullptr, 0, 0, nullptr, 0,
		"Tee only: previous-position delta (physics kick on spawn)."},
	{ERowType::Input, "PrevPos Y", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "y...", nullptr, 0, 0, nullptr, 0,
		"Tee only: previous-position delta (physics kick on spawn)."},
	{ERowType::Input, "Timeline Tick", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "tick...", nullptr, 0, 0, nullptr, 0,
		"Tee only: playground tick the tee is treated as spawned at."},
	{ERowType::Slider, "Tile ID", nullptr, false, false, 0.0f, (float)(TAS_SPAWNER_TILE_COUNT - 1), 1.0f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Tile to spawn (the name is shown instead of the number)."},
	{ERowType::Toggle, "Apply Physics", nullptr, true, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Make the spawned tile solid (off = visual only)."},
	{ERowType::Input, "Tile Number", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "0-255...", nullptr, 0, 0, nullptr, 0,
		"Tune tile: the tune zone. Tele tiles: the tele number pairing TeleIn with TeleOut."},
	{ERowType::Dropdown, "Direction", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasSpawnerDirOpts, 8, 0, nullptr, 0,
		"Direction the spawned entity is launched / aimed in."},
	{ERowType::Input, "Vel X", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "x...", nullptr, 0, 0, nullptr, 0,
		"Extra velocity added along the direction (units/tick)."},
	{ERowType::Input, "Vel Y", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "y...", nullptr, 0, 0, nullptr, 0,
		"Extra velocity added along the direction (units/tick)."},
	{ERowType::Input, "Lifespan", nullptr, false, false, 0, 0, 0, 0, nullptr, "-1", "ticks...", nullptr, 0, 0, nullptr, 0,
		"Projectile lifespan in ticks (-1 = never expires)."},
	{ERowType::Input, "Laser Reach", nullptr, false, false, 0, 0, 0, 0, nullptr, "800", "units...", nullptr, 0, 0, nullptr, 0,
		"Laser reach of the spawned rifle beam."},
	{ERowType::Input, "Door Length", nullptr, false, false, 0, 0, 0, 0, nullptr, "64", "units...", nullptr, 0, 0, nullptr, 0,
		"Length of the spawned door (only the door entity uses it)."},
	{ERowType::Input, "Dragger Strength", nullptr, false, false, 0, 0, 0, 0, nullptr, "1", "0-255...", nullptr, 0, 0, nullptr, 0,
		"Pull strength of the spawned dragger."},
	{ERowType::Input, "Speedup Force", nullptr, false, false, 0, 0, 0, 0, nullptr, "50", "0-255...", nullptr, 0, 0, nullptr, 0,
		"Acceleration strength of the spawned speedup tile."},
	{ERowType::Input, "Speedup Max Speed", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "0-255...", nullptr, 0, 0, nullptr, 0,
		"Speed cap of the spawned speedup tile (0 = unlimited)."},
	{ERowType::Input, "Speedup Angle", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "0-359...", nullptr, 0, 0, nullptr, 0,
		"Push direction of the spawned speedup tile in degrees (0 = right, 90 = down)."},
	{ERowType::Dropdown, "Coordinates Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasSpawnerCoordOpts, 3, 0, nullptr, 0,
		"Where to spawn: at the cursor, at absolute X/Y, or at the previous spawn."},
	{ERowType::Input, "X", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "x...", nullptr, 0, 0, nullptr, 0,
		"Absolute world X coordinate."},
	{ERowType::Input, "Y", nullptr, false, false, 0, 0, 0, 0, nullptr, "0", "y...", nullptr, 0, 0, nullptr, 0,
		"Absolute world Y coordinate."},
	{ERowType::Toggle, "Snap Coordinates", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Snap the spawn coordinates to the 32-unit tile grid."},
	{ERowType::Button, "Spawn", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Spawn the configured entity or tile."},
	{ERowType::DoubleButton, "Kill", "Kill All", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Remove the spawned entity or tile at the target coordinates.", "Remove every spawned entity of the selected type (or every spawned tile of the selected tile id)."},
	{ERowType::Slider, "Loop Spawn Delay", nullptr, false, false, 0.0f, 10000.0f, 20.0f, 1000.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Delay between automatic spawns."},
	{ERowType::Button, g_TasDyn_Loop, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Start / stop spawning at the configured delay."},
};

// v6.2: Playground Settings — virtual RCON / virtual chat / practice inside
// the TAS playground. The header checkbox is the master switch of the group.
static char g_TasDyn_RconCmd[256] = "";
static char g_TasDyn_ChatMsg[256] = "";
enum
{
	TAS_PG_ROW_VIRTUAL_RCON = 0,
	TAS_PG_ROW_RCON_COMMAND,
	TAS_PG_ROW_VIRTUAL_CHAT,
	TAS_PG_ROW_CHAT,
	TAS_PG_ROW_PRACTICE,
	TAS_PG_ROW_COUNT,
};
static SRow g_TasPgChildren[TAS_PG_ROW_COUNT] = {
	{ERowType::Toggle, "Virtual RCON", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Run rcon commands (F2 console, the rcon command or the field below) against the playground."},
	{ERowType::Input, "RCON Command", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "command...  (Enter to run)", nullptr, 0, 0, nullptr, 0,
		"Command executed in the virtual rcon on Enter (the field clears afterwards)."},
	{ERowType::Toggle, "Virtual Chat", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Keep chat inside the playground (whispers still go to the real server)."},
	{ERowType::Input, "Chat", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "message...  (Enter to send)", nullptr, 0, 0, nullptr, 0,
		"Message sent to the virtual chat on Enter."},
	{ERowType::Toggle, "Practice", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Enable practice commands (/tc, telecursor) in the virtual chat and rcon."},
};

static SRow g_TasRows[] = {
	{ERowType::Dropdown, "Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_TasModeOpts, 2, 0, nullptr, 0,
		"Play a recorded TAS or record a new one."},
	{ERowType::DoubleButton, "Pick File", "Unpick File", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Pick a TAS file to load.", "Unpick the current TAS file."},
	{ERowType::DoubleButton, g_TasDyn_Action, "Reset Record", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Start / stop the current action (Play or Record).", "Clear the record/saved buffer and unload the picked .tas file."},
	{ERowType::Input, "Map", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "map...", nullptr, 0, 0, nullptr, 0,
		"Map name saved with the TAS."},
	{ERowType::Input, "Name", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "name...", nullptr, 0, 0, nullptr, 0,
		"TAS name."},
	{ERowType::Input, "Author", nullptr, false, false, 0, 0, 0, 0, nullptr, "", "author...", nullptr, 0, 0, nullptr, 0,
		"TAS author."},
	{ERowType::Label, g_TasDyn_SaveCreated, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
	{ERowType::Label, "TAS Version: v2.0", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Version of the .tas file format used by this client."},
	{ERowType::Button, "Save", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Save the recorded TAS to a file."},
	{ERowType::DoubleButton, g_TasDyn_Join, "Reset Playground", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
		"Join / leave the TAS playground.", "Drop every spawned object, tile and chat line in the playground."},
        {ERowType::Slider, "TPS", nullptr, false, false, 1.0f, 50.0f, 1.0f, 50.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Ticks per second used during playback.", "Pause/resume the TAS playback."},
        {ERowType::Expandable, "Auto Rewind/Forward", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasAutoRfChildren, 3,
                "Auto rewind/forward on freeze/teleport/death tiles."},
        {ERowType::Expandable, "Fake Aim", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasFakeAimChildren, 1,
                "Spoof the rendered aim during playback."},
        {ERowType::Expandable, "Visuals", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasVisualsChildren, 4,
                "TAS visualization toggles."},
        {ERowType::Expandable, "Playground Settings", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasPgChildren, TAS_PG_ROW_COUNT,
                "Virtual RCON, virtual chat and practice inside the playground."},
        {ERowType::Expandable, "Spawner", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TasSpawnerChildren, TAS_SPAWN_ROW_COUNT,
                "Spawn entities and tiles in the TAS playground."},
        {ERowType::DoubleButton, "Rewind", "Forward", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Step the TAS back one tick.", "Step the TAS forward one tick."},
	{ERowType::Label, g_TasDyn_Status, nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};

// ---------- Real-Time Pathfinder panel (9, v1.56.280) ----------
// Single-branch real-time planner on a worker thread, reproduced on the live
// player. Reconstruct re-anchors the plan from the live position when the
// deviation from the planned path exceeds the trigger (Anti-TAS chaos).
// The tile editors are SEPARATE from the Pathfinder tab (own mode/tiles/grids).
static SRow g_RtVisualsChildren[] = {
        {ERowType::Toggle, "Show Path", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show Hooks", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show Field", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
        {ERowType::Toggle, "Show Previous Path", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Draw the previous planned path as well."},
        // Visible only when Show Path or Show Previous Path is on (TasVisualsChildVisible).
        {ERowType::Toggle, "Show Speed", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0},
};
// v1.56.281: "Reconstruct" expandable — the header checkbox is the master
// toggle (kx_rt_pf_reconstruct); the trigger slider lives inside it.
static SRow g_RtReconstructChildren[] = {
        {ERowType::Slider, "Reconstruct Trigger", nullptr, false, false, 1.0f, 32.0f, 1.0f, 8.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Deviation from the plan (tiles) that triggers a re-anchor."},
};
// v1.56.281: "Future Limit" expandable — the header checkbox is the master
// toggle (kx_rt_pf_future_limit); the Future Ticks slider lives inside it.
static SRow g_RtFutureLimitChildren[] = {
        {ERowType::Slider, "Future Ticks", nullptr, false, false, 1.0f, 150.0f, 1.0f, 50.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "How far ahead the plan is allowed to extend (ticks)."},
};
static SRow g_RtPathfinderRows[] = {
        {ERowType::Expandable, "Reconstruct", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_RtReconstructChildren, 1,
                "Re-anchor the plan from the live position on deviation."},
        {ERowType::Expandable, "Future Limit", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_RtFutureLimitChildren, 1,
                "Limit how far ahead the plan may extend."},
        {ERowType::Expandable, "Visuals", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_RtVisualsChildren, 5,
                "RT pathfinder visualization toggles."},
        // Label toggles Pathfinding (green) / Stop (red) via m_RtPfState (no Finish state).
        {ERowType::Button, "RT Pathfinding", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Start / stop the real-time pathfinder."},
        {ERowType::DoubleButton, "RT Editor Forbidden Tiles", "Clear Forbidden Tiles", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Open the RT forbidden-tiles editor.", "Clear all RT forbidden tiles."},
        {ERowType::DoubleButton, "RT Editor Finish Tiles", "Clear Finish Tiles", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Open the RT finish-tiles editor.", "Clear all RT finish tiles."},
};

// ---------- Visuals panel (v1.56.260) ----------
// v1.56.275: rainbow expandables — Color (hex) + Opacity (0-100%) + Rainbow
// anim toggle + Speed (visible only when Rainbow is on). Header checkbox =
// master (off = original look). Anim=1 -> hue rotation at Speed; Anim=0 ->
// static Color with Opacity. Old per-entity-category children removed.
// v1.56.283: "Custom colors" - one combo expandable. The Sprite dropdown
// selects which sprite's settings are edited; the Entity dropdown (visible
// only for Entities) selects the entity category. Header = master toggle.
static const char *const g_CustomColorSpriteOpts[] = {"Tee", "Legs", "Entities", "Guns", "Hook", "HUD", "Particles", "Emotions"};
static const char *const g_CustomColorEntityOpts[] = {"Hookable", "Unhookable", "Freeze", "Death", "Others"};
static SRow g_CustomColorsChildren[] = {
        {ERowType::Dropdown, "Sprite", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_CustomColorSpriteOpts, 8, 0, nullptr, 0, "Select which sprite group you are configuring."},
        {ERowType::Dropdown, "Entity", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_CustomColorEntityOpts, 5, 0, nullptr, 0, "Entity category to configure (only for Sprite = Entities)."},
        {ERowType::Toggle, "Enable", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Enable the custom color for the selected sprite."},
        {ERowType::Input, "Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "ffffff", "hex...", nullptr, 0, 0, nullptr, 0, "Custom color of the selected sprite (hex RGB)."},
        {ERowType::Slider, "Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Opacity of the custom color (0-100%)."},
        {ERowType::Toggle, "Rainbow", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Animate the custom color with a hue cycle."},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Rainbow hue rotation speed for the selected sprite."},
};

// v1.56.283: resolves the config ints (enable, color, alpha, anim, speed x10)
// for the Custom colors Sprite/Entity selection. Sprite: 0=Tee 1=Legs
// 2=Entities(by EntIdx) 3=Guns 4=Hook 5=HUD 6=Particles 7=Emotions.
// EntIdx: 0=Hookable 1=Unhookable 2=Freeze 3=Death 4=Others.
struct SCustomColorCfg
{
        int *pOn;
        unsigned int *pColor;
        int *pAlpha;
        int *pAnim;
        int *pSpeed;
};

static SCustomColorCfg CustomColorCfgFor(int SpriteIdx, int EntIdx)
{
        SCustomColorCfg Cfg = {nullptr, nullptr, nullptr, nullptr, nullptr};
        if(SpriteIdx == 2)
        {
                switch(EntIdx)
                {
                case 0: Cfg = {&g_Config.m_KxRainbowEntHookable, &g_Config.m_KxRainbowEntHookableColor, &g_Config.m_KxRainbowEntHookableAlpha, &g_Config.m_KxRainbowEntHookableAnim, &g_Config.m_KxRainbowEntHookableSpeed}; break;
                case 1: Cfg = {&g_Config.m_KxRainbowEntUnhookable, &g_Config.m_KxRainbowEntUnhookableColor, &g_Config.m_KxRainbowEntUnhookableAlpha, &g_Config.m_KxRainbowEntUnhookableAnim, &g_Config.m_KxRainbowEntUnhookableSpeed}; break;
                case 2: Cfg = {&g_Config.m_KxRainbowEntFreeze, &g_Config.m_KxRainbowEntFreezeColor, &g_Config.m_KxRainbowEntFreezeAlpha, &g_Config.m_KxRainbowEntFreezeAnim, &g_Config.m_KxRainbowEntFreezeSpeed}; break;
                case 3: Cfg = {&g_Config.m_KxRainbowEntDeath, &g_Config.m_KxRainbowEntDeathColor, &g_Config.m_KxRainbowEntDeathAlpha, &g_Config.m_KxRainbowEntDeathAnim, &g_Config.m_KxRainbowEntDeathSpeed}; break;
                default: Cfg = {&g_Config.m_KxRainbowEntOthers, &g_Config.m_KxRainbowEntOthersColor, &g_Config.m_KxRainbowEntOthersAlpha, &g_Config.m_KxRainbowEntOthersAnim, &g_Config.m_KxRainbowEntOthersSpeed}; break;
                }
        }
        else
        {
                switch(SpriteIdx)
                {
                case 0: Cfg = {&g_Config.m_KxRainbowTee, &g_Config.m_KxRainbowTeeColor, &g_Config.m_KxRainbowTeeAlpha, &g_Config.m_KxRainbowTeeAnim, &g_Config.m_KxRainbowTeeSpeed}; break;
                case 1: Cfg = {&g_Config.m_KxRainbowLegs, &g_Config.m_KxRainbowLegsColor, &g_Config.m_KxRainbowLegsAlpha, &g_Config.m_KxRainbowLegsAnim, &g_Config.m_KxRainbowLegsSpeed}; break;
                case 3: Cfg = {&g_Config.m_KxRainbowGuns, &g_Config.m_KxRainbowGunsColor, &g_Config.m_KxRainbowGunsAlpha, &g_Config.m_KxRainbowGunsAnim, &g_Config.m_KxRainbowGunsSpeed}; break;
                case 4: Cfg = {&g_Config.m_KxRainbowHook, &g_Config.m_KxRainbowHookColor, &g_Config.m_KxRainbowHookAlpha, &g_Config.m_KxRainbowHookAnim, &g_Config.m_KxRainbowHookSpeed}; break;
                case 5: Cfg = {&g_Config.m_KxRainbowHud, &g_Config.m_KxRainbowHudColor, &g_Config.m_KxRainbowHudAlpha, &g_Config.m_KxRainbowHudAnim, &g_Config.m_KxRainbowHudSpeed}; break;
                case 6: Cfg = {&g_Config.m_KxRainbowParticles, &g_Config.m_KxRainbowParticlesColor, &g_Config.m_KxRainbowParticlesAlpha, &g_Config.m_KxRainbowParticlesAnim, &g_Config.m_KxRainbowParticlesSpeed}; break;
                default: Cfg = {&g_Config.m_KxRainbowEmoticons, &g_Config.m_KxRainbowEmoticonsColor, &g_Config.m_KxRainbowEmoticonsAlpha, &g_Config.m_KxRainbowEmoticonsAnim, &g_Config.m_KxRainbowEmoticonsSpeed}; break;
                }
        }
        return Cfg;
}

// v1.56.283: reads the Sprite/Entity dropdown selections from a "Custom colors" row.
static void CustomColorSelection(const SRow &Row, int *pSpriteIdx, int *pEntIdx)
{
        *pSpriteIdx = 0;
        *pEntIdx = 0;
        for(int i = 0; i < Row.childCount; ++i)
        {
                if(str_comp(Row.pChildren[i].pName, "Sprite") == 0)
                        *pSpriteIdx = Row.pChildren[i].valueIdx;
                else if(str_comp(Row.pChildren[i].pName, "Entity") == 0)
                        *pEntIdx = Row.pChildren[i].valueIdx;
        }
        if(*pSpriteIdx < 0 || *pSpriteIdx > 7)
                *pSpriteIdx = 0;
        if(*pEntIdx < 0 || *pEntIdx > 4)
                *pEntIdx = 0;
}

// v1.56.263: "Admin Alarm" expandable — per-level, like AimBot per-weapon:
// the Level dropdown selects which level's settings are being edited. The
// alarm shows a LOCAL chat echo; {n} = admin nickname. "Every ms" is visible
// only when Repeat is on (AdminAlarmChildVisible).
static const char *const g_AdminLevelOpts[] = {"Helper", "Moderator", "Admin"};
static SRow g_AdminAlarmChildren[] = {
        {ERowType::Dropdown, "Admin level", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_AdminLevelOpts, 3, 0, nullptr, 0,
                "Which admin level's alarm is being edited."},
        {ERowType::Input, "Message", nullptr, false, false, 0, 0, 0, 0, nullptr, "Admin on server!", "text...", nullptr, 0, 0, nullptr, 0,
                "Alarm message; {n} = admin nickname."},
        {ERowType::Label, "Use {n} for nickname", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Replace {n} in the message with the admin's nickname."},
        {ERowType::Toggle, "Repeat", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Repeat the alarm while an admin is present."},
        {ERowType::Slider, "Every ms", nullptr, false, false, 100.0f, 10000.0f, 100.0f, 1000.0f, "ms", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Repeat interval of the alarm (ms)."},
};

// v6.3: ArrayList HUD — on-screen list of enabled features.
static const char *const g_ArrayListXAnchorOpts[] = {"Auto", "Right", "Center", "Left"};
static const char *const g_ArrayListYAnchorOpts[] = {"Auto", "Top", "Bottom"};
static const char *const g_ArrayListSortOpts[] = {"Name", "Name Length", "History"};
static const char *const g_ArrayListStyleOpts[] = {"Peas", "Group", "DDNet Peas", "DDNet Group"};
static const char *const g_ArrayListRainbowModeOpts[] = {"Group", "Peas", "Peas+Gradient"};
static SRow g_ArrayListChildren[] = {
        {ERowType::DoubleButton, "Editor", "Reset", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Toggle the ArrayList editor (closes the ClickGUI).", "Reset the list position to its default."},
        {ERowType::Dropdown, "X Anchor", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListXAnchorOpts, 4, 0, nullptr, 0,
                "Horizontal anchor of the list (Auto detects the screen side)."},
        {ERowType::Dropdown, "Y Anchor", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListYAnchorOpts, 3, 0, nullptr, 0,
                "Vertical anchor of the list."},
        {ERowType::Slider, "Max Length", nullptr, false, false, 0.0f, 2000.0f, 10.0f, 0.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Wrap feature names wider than this (px, 0 = off)."},
        {ERowType::Dropdown, "Sort Type", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListSortOpts, 3, 0, nullptr, 0,
                "Order of the enabled features."},
        {ERowType::Dropdown, "Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListStyleOpts, 4, 0, nullptr, 0,
                "Visual style of the list blocks."},
        {ERowType::Slider, "Indentation", nullptr, false, false, 0.0f, 40.0f, 1.0f, 2.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Vertical gap between features (the blocks move with it)."},
        {ERowType::Slider, "ArrayList Size", nullptr, false, false, 0.1f, 2.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Overall scale of the HUD (like ClickGUI Scale)."},
        {ERowType::Input, "Text Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "ffffff", "hex...", nullptr, 0, 0, nullptr, 0,
                "Text color of the list (hex RGB)."},
        {ERowType::Toggle, "Rainbow Text", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Animate the text color with a hue cycle."},
        {ERowType::Dropdown, "Rainbow Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListRainbowModeOpts, 3, 0, nullptr, 0,
                "Group = one color, Peas = per feature, Peas+Gradient = per character."},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Rainbow hue rotation speed."},
        {ERowType::Slider, "Step", nullptr, false, false, 1.0f, 180.0f, 1.0f, 25.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Hue offset between features / across a row."},
        {ERowType::Slider, "Text Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the feature text."},
        {ERowType::Slider, "Main Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the block borders."},
        {ERowType::Slider, "Panel Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the block background."},
};
// v6.4: ArrayInfo — same settings model as the ArrayList, but the content is
// an info line (logo | watermark | FPS | session | tee | nick | functions).
static SRow g_ArrayInfoChildren[] = {
        {ERowType::DoubleButton, "Editor", "Reset", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Toggle the ArrayInfo editor (closes the ClickGUI).", "Reset the info bar position to its default."},
        {ERowType::Dropdown, "X Anchor", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListXAnchorOpts, 4, 0, nullptr, 0,
                "Horizontal anchor of the info bar (Auto detects the screen side)."},
        {ERowType::Toggle, "Kinetix Logo", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the Kinetix logo icon."},
        {ERowType::Input, "Watermark", nullptr, false, false, 0, 0, 0, 0, nullptr, "t.me/KinetixClient", "watermark...", nullptr, 0, 0, nullptr, 0,
                "Watermark text (empty = hidden)."},
        {ERowType::Toggle, "Show FPS", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the current FPS as \"1234 FPS\"."},
        {ERowType::Toggle, "Session Time", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show how long the client has been running."},
        {ERowType::Toggle, "Tee", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the main tee icon."},
        {ERowType::Toggle, "Nickname", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the current nickname."},
        {ERowType::Toggle, "Functions", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Show the number of enabled functions as \"X Functions\"."},
        {ERowType::Slider, "Max Length", nullptr, false, false, 0.0f, 2000.0f, 10.0f, 600.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Wrap the info line wider than this (px, 0 = off)."},
        {ERowType::Dropdown, "Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListStyleOpts, 4, 0, nullptr, 0,
                "Visual style of the info blocks."},
        {ERowType::Slider, "Indentation", nullptr, false, false, 0.0f, 40.0f, 1.0f, 2.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Vertical gap between info lines (the blocks move with it)."},
        {ERowType::Slider, "ArrayInfo Size", nullptr, false, false, 0.1f, 2.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Overall scale of the info bar (like ClickGUI Scale)."},
        {ERowType::Input, "Text Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "ffffff", "hex...", nullptr, 0, 0, nullptr, 0,
                "Text color of the info line (hex RGB)."},
        {ERowType::Toggle, "Rainbow Text", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Animate the text color with a hue cycle."},
        {ERowType::Dropdown, "Rainbow Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListRainbowModeOpts, 3, 0, nullptr, 0,
                "Group = one color, Peas = per line, Peas+Gradient = per character."},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Rainbow hue rotation speed."},
        {ERowType::Slider, "Step", nullptr, false, false, 1.0f, 180.0f, 1.0f, 25.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Hue offset between lines / across a line."},
        {ERowType::Slider, "Text Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the info text."},
        {ERowType::Slider, "Main Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the block borders."},
        {ERowType::Slider, "Panel Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the block background."},
};
// v6.7: ArrayPlayer — now-playing media card. Same settings model as
// ArrayList, minus Max Length / Sort Type and the Peas styles. Indentation is
// the vertical gap between the track title and the artist.
static const char *const g_ArrayPlayerStyleOpts[] = {"Group", "DDNet Group"};
static SRow g_ArrayPlayerChildren[] = {
        {ERowType::DoubleButton, "Editor", "Reset", false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Toggle the ArrayPlayer editor (closes the ClickGUI).", "Reset the player position to its default."},
        {ERowType::Dropdown, "X Anchor", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListXAnchorOpts, 4, 0, nullptr, 0,
                "Horizontal alignment of the title/artist text inside the card."},
        {ERowType::Dropdown, "Style", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayPlayerStyleOpts, 2, 0, nullptr, 0,
                "Visual style of the player card."},
        {ERowType::Slider, "Indentation", nullptr, false, false, 0.0f, 40.0f, 1.0f, 4.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Vertical gap between the track title and the artist."},
        {ERowType::Slider, "ArrayPlayer Size", nullptr, false, false, 0.1f, 2.0f, 0.1f, 1.0f, "x", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Overall scale of the player (like ClickGUI Scale)."},
        {ERowType::Input, "Text Color", nullptr, false, false, 0, 0, 0, 0, nullptr, "ffffff", "hex...", nullptr, 0, 0, nullptr, 0,
                "Text color of the player (hex RGB)."},
        {ERowType::Toggle, "Dynamic Scale", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Resize the player by dragging its bottom-right corner in the editor."},
        {ERowType::Slider, "Height ArrayPlayer", nullptr, false, false, 40.0f, 600.0f, 1.0f, 90.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Card height in pixels."},
        {ERowType::Slider, "Weight ArrayPlayer", nullptr, false, false, 100.0f, 1000.0f, 1.0f, 320.0f, "px", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Card width in pixels."},
        {ERowType::Toggle, "Rainbow Text", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Animate the title/artist color with a hue cycle."},
        {ERowType::Dropdown, "Rainbow Mode", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, g_ArrayListRainbowModeOpts, 3, 0, nullptr, 0,
                "Group = one color, Peas = per line, Peas+Gradient = per character."},
        {ERowType::Slider, "Speed", nullptr, false, false, 0.1f, 10.0f, 0.1f, 1.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Rainbow hue rotation speed."},
        {ERowType::Slider, "Step", nullptr, false, false, 1.0f, 180.0f, 1.0f, 25.0f, "", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Hue offset between the title and artist."},
        {ERowType::Slider, "Text Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the title/artist text."},
        {ERowType::Slider, "Main Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the card border."},
        {ERowType::Slider, "Panel Opacity", nullptr, false, false, 0.0f, 100.0f, 1.0f, 100.0f, "%", nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Opacity of the card background."},
};
static SRow g_VisualsRows[] = {
        {ERowType::Expandable, "Custom colors", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_CustomColorsChildren, 7, "Master switch: off = original game look."},
        {ERowType::Toggle, "Hide particles", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Hide in-game particles."},
        // v1.56.260: moved from the Advanced panel.
        {ERowType::Expandable, "Trajectory", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_TrajectoryChildren, 7,
                "Predict projectile trajectories."},
        {ERowType::Expandable, "ESP", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_EspChildren, 21,
                "Draw pointers / boxes to targets."},
        {ERowType::Expandable, "Hitboxes", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_HitboxesChildren, 17,
                "Draw tee collision / trigger hitboxes."},
        // v1.56.262: bypass locks + admin alarm.
        {ERowType::Toggle, "Bypass Zoom Lock", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Bypass the server zoom lock."},
        {ERowType::Toggle, "Bypass Asset Lock", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0,
                "Bypass the asset (map) lock."},
        {ERowType::Expandable, "Admin Alarm", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_AdminAlarmChildren, 5,
                "Local alarm when an admin/helper/moderator is present."},
        {ERowType::Expandable, "ArrayList", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_ArrayListChildren, 16,
                "On-screen list of enabled features (drag to move)."},
        {ERowType::Expandable, "ArrayInfo", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_ArrayInfoChildren, 21,
                "On-screen info bar (drag to move)."},
        {ERowType::Expandable, "ArrayPlayer", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_ArrayPlayerChildren, 16,
                "On-screen now-playing media card (drag to move)."},
};

// v1.56.286: Search panel — real-time filtering of every other panel.
static SRow g_SearchParamChildren[] = {
        {ERowType::Toggle, "Case Sensitive", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Match letter case exactly."},
        {ERowType::Toggle, "Whole Word", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Match whole words only (no substring hits)."},
        {ERowType::Toggle, "Use Regex", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Treat the query as an ECMAScript regular expression."},
        {ERowType::Toggle, "Fuzzy Match", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, 0, "Match when every query character appears in order."},
};
static SRow g_SearchRows[] = {
        {ERowType::Input, "Search", nullptr, false, false, 0, 0, 0, 0, nullptr, g_SearchQuery, "search...", nullptr, 0, 0, nullptr, 0, "Type to filter all panels in real time. Empty = show everything."},
        {ERowType::Expandable, "Search parameters", nullptr, false, false, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, g_SearchParamChildren, 4},
};

static SPanel g_Panels[NUM_PANELS] = {
        {"Functions", g_FunctionsRows, sizeof(g_FunctionsRows) / sizeof(g_FunctionsRows[0])},
        // v1.56.260: Visuals panel replaces Advanced (same slot position).
        {"Visuals", g_VisualsRows, sizeof(g_VisualsRows) / sizeof(g_VisualsRows[0])},
        {"Settings", g_SettingsRows, sizeof(g_SettingsRows) / sizeof(g_SettingsRows[0])},
        {"Pathfinder", g_PathfinderRows, sizeof(g_PathfinderRows) / sizeof(g_PathfinderRows[0])},
        {"Binds", g_BindsRows, sizeof(g_BindsRows) / sizeof(g_BindsRows[0])},
        {"Info", g_InfoRows, sizeof(g_InfoRows) / sizeof(g_InfoRows[0])},
        {"Code Execution (Lua)", g_CodeExecRows, sizeof(g_CodeExecRows) / sizeof(g_CodeExecRows[0])},
        {"TAS", g_TasRows, sizeof(g_TasRows) / sizeof(g_TasRows[0])},
        // v1.56.280: Real-Time Pathfinder — placed AFTER all other panels.
        {"Real-Time Pathfinder", g_RtPathfinderRows, sizeof(g_RtPathfinderRows) / sizeof(g_RtPathfinderRows[0])},
        // v1.56.282: Dummy Manager — last panel (NUM_PANELS = 10).
        {"Dummy Manager", g_DummyMgrRows, sizeof(g_DummyMgrRows) / sizeof(g_DummyMgrRows[0])},
        // v1.56.286: Search — real-time filter of every other panel (last panel).
        {"Search", g_SearchRows, sizeof(g_SearchRows) / sizeof(g_SearchRows[0])},
};

// Conditional visibility for Functions panel top-level rows.
// Mirror aim X/Y visible only when Aim mode = Mirror (valueIdx=1).
// Smart aim visible only when Aim mode = Smart (valueIdx=0).
// All other rows always visible.
// v1.56.286: search matching (the Search panel). The compiled regex lives
// here so a query change recompiles it once, not per row.
static bool SearchIsWordChar(char C)
{
        return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
}
static char SearchFoldChar(char C)
{
        return (C >= 'A' && C <= 'Z') ? (char)(C + ('a' - 'A')) : C;
}
static std::regex g_SearchRegexCompiled;
static bool g_SearchRegexValid = false;

static bool SearchSubstrMatch(const char *pName)
{
        const int QueryLen = str_length(g_SearchQuery);
        if(QueryLen == 0)
                return true;
        for(const char *p = pName; *p; ++p)
        {
                const char *pAt = g_SearchCaseSensitive ? str_find(p, g_SearchQuery) : str_find_nocase(p, g_SearchQuery);
                if(!pAt)
                        return false;
                if(!g_SearchWholeWord)
                        return true;
                const bool OkBefore = (pAt == pName) || !SearchIsWordChar(pAt[-1]);
                const char *pAfter = pAt + QueryLen;
                const bool OkAfter = (*pAfter == 0) || !SearchIsWordChar(*pAfter);
                if(OkBefore && OkAfter)
                        return true;
                p = pAt;
        }
        return false;
}
static bool SearchFuzzyMatch(const char *pName)
{
        const char *p = pName;
        for(const char *pQ = g_SearchQuery; *pQ; ++pQ)
        {
                const char QC = g_SearchCaseSensitive ? *pQ : SearchFoldChar(*pQ);
                bool Found = false;
                while(*p)
                {
                        const char PC = g_SearchCaseSensitive ? *p : SearchFoldChar(*p);
                        ++p;
                        if(PC == QC)
                        {
                                Found = true;
                                break;
                        }
                }
                if(!Found)
                        return false;
        }
        return true;
}
static bool SearchMatchName(const char *pName)
{
        if(!g_SearchQuery[0])
                return true;
        if(g_SearchRegex)
        {
                if(!g_SearchRegexValid)
                        return false;
                if(g_SearchWholeWord)
                {
                        const int NameLen = str_length(pName);
                        const std::cregex_iterator End;
                        for(std::cregex_iterator It(pName, pName + NameLen, g_SearchRegexCompiled); It != End; ++It)
                        {
                                const int Pos = (int)It->position();
                                const int Len = (int)It->length();
                                if(Len == 0)
                                        continue;
                                const bool OkBefore = (Pos == 0) || !SearchIsWordChar(pName[Pos - 1]);
                                const bool OkAfter = (Pos + Len >= NameLen) || !SearchIsWordChar(pName[Pos + Len]);
                                if(OkBefore && OkAfter)
                                        return true;
                        }
                        return false;
                }
                return std::regex_search(pName, g_SearchRegexCompiled);
        }
        if(g_SearchFuzzy)
                return SearchFuzzyMatch(pName);
        return SearchSubstrMatch(pName);
}
// v1.56.286: search visibility gates — false = the row is hidden by the
// active search. The Search panel's own rows are marked always-matching in
// SearchUpdateMatches(), so the call sites need no panel special-casing.
static bool SearchKeepsRow(const SRow &row)
{
        if(g_SearchQuery[0] == 0)
                return true;
        return row.m_SearchMatch || row.m_SearchChildMatch;
}
static bool SearchKeepsChild(const SRow &child)
{
        if(g_SearchQuery[0] == 0)
                return true;
        return child.m_SearchMatch;
}
// v1.56.286: recompute the match flags for every row of every panel. Called
// from OnRender when the query or a filter toggle changes.
static void SearchUpdateMatches()
{
        g_SearchRegexValid = false;
        if(g_SearchRegex && g_SearchQuery[0])
        {
                try
                {
                        std::regex::flag_type Flags = std::regex::ECMAScript;
                        if(!g_SearchCaseSensitive)
                                Flags |= std::regex::icase;
                        g_SearchRegexCompiled.assign(g_SearchQuery, Flags);
                        g_SearchRegexValid = true;
                }
                catch(const std::regex_error &)
                {
                        g_SearchRegexValid = false;
                }
        }
        const bool Active = g_SearchQuery[0] != 0;
        for(int p = 0; p < NUM_PANELS; ++p)
        {
                SPanel &panel = g_Panels[p];
                const bool IsSearchPanel = (str_comp(panel.pTitle, "Search") == 0);
                for(int r = 0; r < panel.rowCount; ++r)
                {
                        SRow &row = panel.pRows[r];
                        if(IsSearchPanel || !Active)
                        {
                                row.m_SearchMatch = true;
                                row.m_SearchChildMatch = false;
                                if(row.type == ERowType::Expandable && row.pChildren)
                                {
                                        for(int c = 0; c < row.childCount; ++c)
                                                row.pChildren[c].m_SearchMatch = true;
                                }
                                continue;
                        }
                        row.m_SearchMatch = SearchMatchName(row.pName);
                        row.m_SearchChildMatch = false;
                        if(row.type == ERowType::Expandable && row.pChildren)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        child.m_SearchMatch = SearchMatchName(child.pName);
                                        if(child.m_SearchMatch)
                                                row.m_SearchChildMatch = true;
                                }
                        }
                }
        }
}
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
                // v1.56.280: RT Pathfinder "Show Speed" is visible when Show Path
                // OR Show Previous Path is on (TAS has no Show Previous Path child —
                // its behavior is unchanged).
                bool showAnyPath = false;
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Show Path") == 0 && pChildren[i].on)
                                showAnyPath = true;
                        if(str_comp(pChildren[i].pName, "Show Previous Path") == 0 && pChildren[i].on)
                                showAnyPath = true;
                }
                return showAnyPath;
        }
        return true;
}

// Spawner children: entity rows only in Entity mode, Tee rows only for the Tee
// entity, tile rows only in Tile mode, X/Y only in Coordinates mode.
static bool TasSpawnerChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
	const int Mode = pChildren[TAS_SPAWN_ROW_MODE].valueIdx;
	const int Entity = (int)(pChildren[TAS_SPAWN_ROW_ENTITY_ID].value + 0.5f);
	const int Coord = pChildren[TAS_SPAWN_ROW_COORD_MODE].valueIdx;
	const bool IsEntity = Mode == 0;
	const bool IsTile = Mode == 1;
	const bool IsTee = Entity == TAS_SPAWN_ENT_TEE;
	// Tiles carrying a client-side number: Tune (the tune zone) and tele tiles
	// (the tele number pairing TeleIn with TeleOut).
	const int TileId = CTas::SpawnerTileId((int)(pChildren[TAS_SPAWN_ROW_TILE_ID].value + 0.5f));
	const bool TileUsesNumber = IsTile && (TileId == TILE_TUNE || TileId == TILE_TELEIN ||
		TileId == TILE_TELEINEVIL || TileId == TILE_TELEOUT || TileId == TILE_TELECHECKIN);

	switch(idx)
	{
	case TAS_SPAWN_ROW_ENTITY_ID:
	case TAS_SPAWN_ROW_APPLY_PHYSICS:
		return IsEntity;
	case TAS_SPAWN_ROW_TIMELINE:
	case TAS_SPAWN_ROW_PREV_X:
	case TAS_SPAWN_ROW_PREV_Y:
		// Timeline Tick / PrevPos X/Y only affect the tee; projectiles use
		// Vel X/Y for their physics instead.
		return IsEntity && IsTee;
	case TAS_SPAWN_ROW_TILE_ID:
	case TAS_SPAWN_ROW_TILE_PHYSICS:
		return IsTile;
	case TAS_SPAWN_ROW_TILE_NUMBER:
		return TileUsesNumber;
	case TAS_SPAWN_ROW_NAME:
	case TAS_SPAWN_ROW_CLAN:
	case TAS_SPAWN_ROW_ID:
	case TAS_SPAWN_ROW_SKIN:
	case TAS_SPAWN_ROW_CC_TEE:
	case TAS_SPAWN_ROW_TEE_COLOR:
	case TAS_SPAWN_ROW_CC_LEGS:
	case TAS_SPAWN_ROW_LEGS_COLOR:
	case TAS_SPAWN_ROW_HOOK_STRENGTH:
	case TAS_SPAWN_ROW_HOOK_MODE:
	case TAS_SPAWN_ROW_PRACTICE:
	case TAS_SPAWN_ROW_WEAPON_ID:
		return IsEntity && IsTee;
	case TAS_SPAWN_ROW_PROJECTILE_FREEZE:
	case TAS_SPAWN_ROW_PROJECTILE_EXPLOSIVE:
	case TAS_SPAWN_ROW_PROJECTILE_SOUND:
		return IsEntity && Entity == TAS_SPAWN_ENT_PROJECTILE;
	case TAS_SPAWN_ROW_LASER_TYPE:
		return IsEntity && Entity == TAS_SPAWN_ENT_LASER;
	case TAS_SPAWN_ROW_PICKUP_TYPE:
	case TAS_SPAWN_ROW_SUBTYPE:
		return IsEntity && Entity == TAS_SPAWN_ENT_PICKUP;
	case TAS_SPAWN_ROW_SWITCH:
		return IsEntity && (Entity == TAS_SPAWN_ENT_DOOR || Entity == TAS_SPAWN_ENT_DRAGGER ||
			Entity == TAS_SPAWN_ENT_PLASMA || Entity == TAS_SPAWN_ENT_PICKUP);
	case TAS_SPAWN_ROW_COORD_MODE:
		return true;
	case TAS_SPAWN_ROW_X:
	case TAS_SPAWN_ROW_Y:
		return Coord == 1;
	// v6.2: physics fields of the non-tee entities.
	case TAS_SPAWN_ROW_DIR:
		return IsEntity && !IsTee && Entity != TAS_SPAWN_ENT_PICKUP;
	case TAS_SPAWN_ROW_VEL_X:
	case TAS_SPAWN_ROW_VEL_Y:
	case TAS_SPAWN_ROW_LIFESPAN:
		return IsEntity && Entity == TAS_SPAWN_ENT_PROJECTILE;
	case TAS_SPAWN_ROW_LASER_REACH:
		return IsEntity && Entity == TAS_SPAWN_ENT_LASER;
	case TAS_SPAWN_ROW_LASER_LENGTH:
		// Only the door entity uses the length (draggers/plasma compute it themselves).
		return IsEntity && Entity == TAS_SPAWN_ENT_DOOR;
	case TAS_SPAWN_ROW_DRAGGER_STRENGTH:
		return IsEntity && Entity == TAS_SPAWN_ENT_DRAGGER;
	case TAS_SPAWN_ROW_SPEEDUP_FORCE:
	case TAS_SPAWN_ROW_SPEEDUP_MAX_SPEED:
	case TAS_SPAWN_ROW_SPEEDUP_ANGLE:
		return IsTile && TileId == TILE_SPEED_BOOST;
	default:
		return true;
	}
}

// v6.3: ArrayList children — Text Color is replaced by the rainbow controls
// while rainbow is on; Step only exists for the non-Group rainbow modes.
static bool ArrayListChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        const bool Rainbow = g_Config.m_KxArrayListRainbow != 0;
        if(str_comp(row.pName, "Text Color") == 0)
                return !Rainbow;
        if(str_comp(row.pName, "Rainbow Mode") == 0 || str_comp(row.pName, "Speed") == 0)
                return Rainbow;
        if(str_comp(row.pName, "Step") == 0)
                return Rainbow && g_Config.m_KxArrayListRainbowMode != 0;
        return true;
}

// v6.4: ArrayInfo children — same rainbow gating as the ArrayList.
static bool ArrayInfoChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        const bool Rainbow = g_Config.m_KxArrayInfoRainbow != 0;
        if(str_comp(row.pName, "Text Color") == 0)
                return !Rainbow;
        if(str_comp(row.pName, "Rainbow Mode") == 0 || str_comp(row.pName, "Speed") == 0)
                return Rainbow;
        if(str_comp(row.pName, "Step") == 0)
                return Rainbow && g_Config.m_KxArrayInfoRainbowMode != 0;
        return true;
}

// v6.7: ArrayPlayer children — same rainbow gating as the ArrayList/ArrayInfo.
static bool ArrayPlayerChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        const bool Rainbow = g_Config.m_KxArrayPlayerRainbow != 0;
        if(str_comp(row.pName, "Text Color") == 0)
                return !Rainbow;
        if(str_comp(row.pName, "Rainbow Mode") == 0 || str_comp(row.pName, "Speed") == 0)
                return Rainbow;
        if(str_comp(row.pName, "Step") == 0)
                return Rainbow && g_Config.m_KxArrayPlayerRainbowMode != 0;
        return true;
}

// v6.3: tooltip opacity rows live under the Show ToolTips toggle.
static bool ClickGuiSettingsChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "ToolTips Text Opacity") == 0 ||
                str_comp(row.pName, "ToolTips Main Opacity") == 0 ||
                str_comp(row.pName, "ToolTips Panel Opacity") == 0)
                return g_Config.m_KxUiShowTooltips != 0;
        return true;
}

// Unified child-visibility dispatcher for the TAS panel's expandables.
static bool TasRowChildrenVisible(const SRow &row, int c)
{
        if(str_comp(row.pName, "ArrayList") == 0)
                return ArrayListChildVisible(row.pChildren, row.childCount, c);
        if(str_comp(row.pName, "ArrayInfo") == 0)
                return ArrayInfoChildVisible(row.pChildren, row.childCount, c);
        if(str_comp(row.pName, "ArrayPlayer") == 0)
                return ArrayPlayerChildVisible(row.pChildren, row.childCount, c);
        if(str_comp(row.pName, "ClickGUI Settings") == 0)
                return ClickGuiSettingsChildVisible(row.pChildren, row.childCount, c);
        if(str_comp(row.pName, "Visuals") == 0)
                return TasVisualsChildVisible(row.pChildren, row.childCount, c);
        if(str_comp(row.pName, "Spawner") == 0)
                return TasSpawnerChildVisible(row.pChildren, row.childCount, c);
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

// Fake Aim: Speed hidden for Robot (1) and Real (5); Jitter only for Chaos (4).
// v1.56.247: added Chaos/Real modes, Jitter + Distance from center sliders.
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
                return modeIdx != 1 && modeIdx != 5;
        if(str_comp(row.pName, "Jitter") == 0)
                return modeIdx == 4;
        // v1.56.262: Show Server-Side Aim — visible only when Show for me is on.
        if(str_comp(row.pName, "Show Server-Side Aim") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Show for me") == 0)
                                return pChildren[i].on != 0;
                }
                return false;
        }
        return true;
}
// v1.56.262: Admin Alarm — "Every ms" visible only when Repeat is on.
static bool AdminAlarmChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Every ms") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Repeat") == 0)
                                return pChildren[i].on != 0;
                }
                return false;
        }
        return true;
}
// v1.56.263: BalanceBot — "Targets IDs" visible only when Targets filter != 0
// (same rule as AimBot / TriggerBot / ESP).
static bool BalanceBotChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Targets") == 0)
                                return pChildren[i].valueIdx != 0;
                }
                return false;
        }
        // v1.56.275: "Target ID" visible only when DDrace Team = Target (3).
        if(str_comp(row.pName, "Target ID") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "DDrace Team") == 0)
                                return pChildren[i].valueIdx == 3;
                }
                return false;
        }
        return true;
}

// v1.56.92: AimBot — raycast AB fields visible only when Rules=2 (Insert line + Raycast)
// v1.56.292: settings block being edited in the AimBot / TriggerBot panel.
// The Hook weapon (last entry) edits the per-entity block selected by the
// Entity Mode dropdown; everything else edits the weapon's own block.
static CAimBot::SWeaponSettings &AbEditSettings(CAimBot &AimBot)
{
        int w = AimBot.m_AimBotSelectedWeapon;
        if(w < 0 || w >= CAimBot::NUM_WEAPONS_AIM)
                w = 0;
        if(w == CAimBot::NUM_WEAPONS_AIM - 1) // Hook
                return AimBot.AimBotHookSettings(AimBot.m_AimBotSelectedEntity);
        return AimBot.m_AimBotWeapons[w];
}

static CAimBot::SWeaponSettings &TbEditSettings(CAimBot &AimBot)
{
        int w = AimBot.m_TriggerBotSelectedWeapon;
        if(w < 0 || w >= CAimBot::NUM_WEAPONS_TRIGGER)
                w = 0;
        if(w == CAimBot::NUM_WEAPONS_TRIGGER - 1) // Hook
                return AimBot.TriggerBotHookSettings(AimBot.m_TriggerBotSelectedEntity);
        return AimBot.m_TriggerBotWeapons[w];
}

static bool AimBotChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        // v1.56.292: selected Weapon (row "Weapon") — the Hook entry (last)
        // edits a per-entity settings block.
        int weaponIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Weapon") == 0)
                {
                        weaponIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        const bool hookSelected = (weaponIdx == CAimBot::NUM_WEAPONS_AIM - 1);
        // v1.56.294: a per-entity block is edited when the Hook weapon is
        // selected AND the Entity Mode is not Tee (0). The Tee block targets
        // players, so it keeps every row.
        int entityIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Entity Mode") == 0)
                {
                        entityIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        const bool isEntityBlock = hookSelected && entityIdx != 0;
        // v1.56.294: the Hook entry is not a held weapon — it can never be the
        // auto-switch target, so the row is dead for every Hook block.
        if(hookSelected && str_comp(row.pName, "Auto Switch Weapon") == 0)
                return false;
        // v1.56.293/294: player-target filters, prediction (map blocks don't
        // move), the player line-of-sight Rules, the raycast-angle toggle
        // (entities always aim at the raycast point), FNG Freeze and the weapon
        // auto-switch are meaningless for map entities — hidden while a
        // per-entity block is edited.
        if(isEntityBlock && (str_comp(row.pName, "Team") == 0 || str_comp(row.pName, "Friend") == 0 ||
                str_comp(row.pName, "Dummy") == 0 || str_comp(row.pName, "Clanmate") == 0 ||
                str_comp(row.pName, "Freeze") == 0 || str_comp(row.pName, "Targets") == 0 ||
                str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0 ||
                str_comp(row.pName, "Rules") == 0 || str_comp(row.pName, "Use raycast angle (AB)") == 0 ||
                str_comp(row.pName, "Predict") == 0 || str_comp(row.pName, "FNG Freeze") == 0 ||
                str_comp(row.pName, "Auto Switch Weapon") == 0))
                return false;
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
                if(str_comp(pChildren[i].pName, "Targets") == 0)
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
        // v1.56.262: Smooth Speed / Center snap visible only in Smooth mode.
        // While Silent is on the Mode dropdown shows g_ABModeOptsNoTrigger
        // ("Trigger" hidden) — remap the displayed index back to the stored mode.
        if(str_comp(row.pName, "Smooth Speed") == 0 || str_comp(row.pName, "Center snap") == 0)
        {
                bool silent = true;
                int modeDisp = 0;
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Silent") == 0)
                                silent = pChildren[i].on != 0;
                        else if(str_comp(pChildren[i].pName, "Mode") == 0)
                                modeDisp = pChildren[i].valueIdx;
                }
                const int mode = silent ? (modeDisp == 0 ? 0 : modeDisp == 1 ? 2 : 3) : modeDisp;
                return mode == 3;
        }
        // v1.56.291: Hook "Entity Mode" — visible only for Weapon = Hook.
        if(str_comp(row.pName, "Entity Mode") == 0)
                return hookSelected;
        return true;
}

// v1.56.92: TriggerBot — same as AimBot
static bool TriggerBotChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        // v1.56.292: selected Weapon (row "Weapon") — the Hook entry (last)
        // edits a per-entity settings block.
        int weaponIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Weapon") == 0)
                {
                        weaponIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        const bool hookSelected = (weaponIdx == CAimBot::NUM_WEAPONS_TRIGGER - 1);
        // v1.56.294: per-entity block = Hook weapon + Entity Mode != Tee (0).
        int entityIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Entity Mode") == 0)
                {
                        entityIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        const bool isEntityBlock = hookSelected && entityIdx != 0;
        // v1.56.294: the Hook entry is not a held weapon — it can never be the
        // auto-switch target, so the row is dead for every Hook block.
        if(hookSelected && str_comp(row.pName, "Auto Switch Weapon") == 0)
                return false;
        // v1.56.293/294: filters, Rules, FNG Freeze and the weapon auto-switch
        // are meaningless for map entities — hidden for per-entity blocks.
        if(isEntityBlock && (str_comp(row.pName, "Team") == 0 || str_comp(row.pName, "Friend") == 0 ||
                str_comp(row.pName, "Dummy") == 0 || str_comp(row.pName, "Clanmate") == 0 ||
                str_comp(row.pName, "Freeze") == 0 || str_comp(row.pName, "Targets") == 0 ||
                str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0 ||
                str_comp(row.pName, "Rules") == 0 || str_comp(row.pName, "FNG Freeze") == 0 ||
                str_comp(row.pName, "Auto Switch Weapon") == 0))
                return false;
        int targetsIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Targets") == 0)
                {
                        targetsIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
                return targetsIdx != 0;
        // v1.56.292: Hook "Entity Mode" — visible only for Weapon = Hook.
        if(str_comp(row.pName, "Entity Mode") == 0)
                return hookSelected;
        return true;
}

// v1.56.282: Dummy Manager sync mirrors — updated each frame by
// UpdateDummyManagerRows() so the static visibility helpers below (no access
// to CClickGui members) can see the engine dummy limit and staged-proxy state.
static int g_DummyLimit = 8;
static bool g_ProxyDirty = false;

// v1.56.282: prefix test without a <string.h> dependency.
static bool DummyMgrIsPrefix(const char *pStr, const char *pPrefix)
{
        for(; *pPrefix; ++pStr, ++pPrefix)
        {
                if(*pStr != *pPrefix)
                        return false;
        }
        return true;
}

// v1.56.282: "D<N> Connect"/"D<N> Switch" -> N (any digit count), else -1.
static int DummyMgrParseD(const char *pName)
{
        if(!pName || pName[0] != 'D' || pName[1] < '0' || pName[1] > '9')
                return -1;
        int v = 0;
        const char *p = pName + 1;
        for(; *p >= '0' && *p <= '9'; ++p)
                v = v * 10 + (*p - '0');
        return (*p == ' ' && v >= 1 && v < MAX_DUMMIES) ? v : -1;
}

// v1.56.282: "Main" -> 0, "Dummy <N>" -> N, else -1.
static int DummyMgrParseDummyName(const char *pName)
{
        if(!pName)
                return -1;
        if(str_comp(pName, "Main") == 0)
                return 0;
        if(!DummyMgrIsPrefix(pName, "Dummy "))
                return -1;
        int v = 0;
        const char *p = pName + 6;
        if(*p < '0' || *p > '9')
                return -1;
        for(; *p >= '0' && *p <= '9'; ++p)
                v = v * 10 + (*p - '0');
        return (*p == '\0' && v >= 1 && v < MAX_DUMMIES) ? v : -1;
}

// v1.56.282: "Open Dummy <N> Profile"/"Open Dummy <N> Tee" -> N, else -1.
static int DummyMgrParseOpenName(const char *pName)
{
        if(!pName || !DummyMgrIsPrefix(pName, "Open Dummy "))
                return -1;
        int v = 0;
        const char *p = pName + 11;
        if(*p < '0' || *p > '9')
                return -1;
        for(; *p >= '0' && *p <= '9'; ++p)
                v = v * 10 + (*p - '0');
        return (v >= 1 && v < MAX_DUMMIES) ? v : -1;
}

// v1.56.90: Dummies — "Switch to Main" hidden when already on main player.
static bool DummiesChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(!row.pName)
                return true;
        if(str_comp(row.pName, "Switch to Main") == 0)
                return g_Config.m_ClDummy != 0; // visible only when on a dummy
        return true;
}

// v1.56.282: Dummy Manager top-level visibility — "Reconnect (Apply)" only
// while staged proxy changes exist.
static bool DummyManagerRowVisible(const SRow *pRows, int rowCount, int idx)
{
        if(idx < 0 || idx >= rowCount)
                return false;
        const SRow &row = pRows[idx];
        if(!row.pName)
                return true;
        if(str_comp(row.pName, "Reconnect (Apply)") == 0)
                return g_ProxyDirty;
        return true;
}

// v1.56.282: Dummy Manager child visibility — D% rows follow the runtime
// dummy limit; Profiles/Tee children follow the "Edit Mode" dropdown (child 0):
// 0 = Per-Dummy (Open Dummy %d buttons), 1 = All (Open All), 2 = Targets
// (target toggles + Open Targets).
static bool DummyManagerChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &ch = pChildren[idx];
        if(pChildren == g_DummyConnectChildren)
        {
                if(!DummiesChildVisible(pChildren, childCount, idx))
                        return false;
                if(ch.type == ERowType::DoubleButton)
                {
                        const int d = DummyMgrParseD(ch.pName);
                        return d >= 1 && d < g_DummyLimit;
                }
                return true;
        }
        if(pChildren == g_DummyProxyChildren)
                return idx < g_DummyLimit;
        if(pChildren == g_DummyProfilesChildren || pChildren == g_DummyTeeChildren ||
           pChildren == g_DummyProfilesMaster || pChildren == g_DummyTeeMaster)
        {
                if(ch.type == ERowType::Dropdown)
                        return true; // "Edit Mode"
                const int mode = pChildren[0].valueIdx;
                if(ch.type == ERowType::Toggle)
                {
                        const int d = DummyMgrParseDummyName(ch.pName);
                        return mode == 2 && d >= 0 && d < g_DummyLimit;
                }
                if(ch.type == ERowType::Button)
                {
                        if(str_comp(ch.pName, "Open Main Profile") == 0 || str_comp(ch.pName, "Open Main Tee") == 0)
                                return true;
                        if(str_comp(ch.pName, "Open Targets Profile") == 0 || str_comp(ch.pName, "Open Targets Tee") == 0)
                                return mode == 2;
                        if(str_comp(ch.pName, "Open All Profile") == 0 || str_comp(ch.pName, "Open All Tee") == 0)
                                return mode == 1;
                        const int d = DummyMgrParseOpenName(ch.pName);
                        return mode == 0 && d >= 1 && d < g_DummyLimit;
                }
        }
        return true;
}

// v1.56.282: NOTE — child/top-level visibility for the Dummy Manager is
// handled dynamically via childCount/rowCount truncation + per-mode child
// compaction (see UpdateDummyManagerRows), so no per-site checks are needed.
// v1.56.256: Hitboxes — "Hitbox Style"/"Box Speed" visible only when their group
// toggle is on ("Tee tile" idx 0, "Tee trigger" idx 3); "Box Speed" also requires
// style 2 (Dotted Box).
// v1.56.257: "Show for others" is always visible; the ESP-like filter rows
// (idx >= 7) require it.
static bool HitboxesChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        const bool tile = pChildren[0].on;
        const bool trigger = pChildren[3].on;
        const bool showOthers = pChildren[6].on;
        if(str_comp(row.pName, "Hitbox Style") == 0)
                return idx == 1 ? tile : trigger;
        if(str_comp(row.pName, "Box Speed") == 0)
        {
                if(idx == 2)
                        return tile && pChildren[1].valueIdx == 2; // tile Dotted Box
                return trigger && pChildren[4].valueIdx == 2; // trigger Dotted Box
        }
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
                return showOthers && pChildren[15].valueIdx != 0;
        // v1.56.275: "Target ID" visible only when DDrace Team = Target (3).
        if(str_comp(row.pName, "Target ID") == 0)
                return showOthers && pChildren[7].valueIdx == 3;
        if(idx >= 7)
                return showOthers;
        return true;
}
// v1.56.258: Block — "Main ID" visible only when Auto Main is off (idx 0);
// "Rescue IDs"/"Unrescue IDs" visible only when Rescue is on (idx 4).
static bool HelpersChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Main ID") == 0)
                return !pChildren[0].on;
        if(str_comp(row.pName, "Rescue IDs") == 0 || str_comp(row.pName, "Unrescue IDs") == 0)
                return pChildren[4].on;
        return true;
}
// v1.56.104: ESP — "X"/"Y" visible only when Mode = 1 (Screen coordinates).
// v1.56.113: "Pointer Speed" visible only when Pointer Style = 2 or 3 (Dotted line / Dotted Arrow).
// v1.56.254: renamed rows; "Box Speed" visible only when Box Style = 1 (Dotted box).
static bool EspChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        // v1.56.256: shared dispatcher — the Hitboxes expandable reuses this gate
        // at every call site (checked via the static children array identity).
        if(pChildren == g_HitboxesChildren)
                return HitboxesChildVisible(pChildren, childCount, idx);
        // v1.56.275: "Target ID" visible only when DDrace Team = Target (3).
        if(str_comp(row.pName, "Target ID") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "DDrace Team") == 0)
                                return pChildren[i].valueIdx == 3;
                }
                return false;
        }
        int modeIdx = 0;
        int styleIdx = 0;
        int boxStyleIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Mode") == 0)
                        modeIdx = pChildren[i].valueIdx;
                else if(str_comp(pChildren[i].pName, "Pointer Style") == 0)
                        styleIdx = pChildren[i].valueIdx;
                else if(str_comp(pChildren[i].pName, "Box Style") == 0)
                        boxStyleIdx = pChildren[i].valueIdx;
        }
        if(str_comp(row.pName, "X") == 0 || str_comp(row.pName, "Y") == 0)
                return modeIdx == 1; // Screen coordinates
        if(str_comp(row.pName, "Pointer Speed") == 0)
                return styleIdx == 2 || styleIdx == 3; // Dotted line / Dotted Arrow
        if(str_comp(row.pName, "Box Speed") == 0)
                return boxStyleIdx == 1; // Dotted box
        int targetsIdx = 0;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Targets") == 0)
                {
                        targetsIdx = pChildren[i].valueIdx;
                        break;
                }
        }
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
                return targetsIdx != 0;
        return true;
}
// v1.56.273: Spoof Spectate — "Fake spectate X/Y" visible only in Follow
// Coordinates mode (0); "Fake Follow ID" only in Follow Player (1).
static bool SpoofSpectateChildVisible(const SRow *pChildren, int childCount, int idx)
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
        if(str_comp(row.pName, "Fake spectate X") == 0 || str_comp(row.pName, "Fake spectate Y") == 0)
                return modeIdx == 0;
        if(str_comp(row.pName, "Fake Follow ID") == 0)
                return modeIdx == 1;
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
// v1.56.275: Visuals rainbow expandables — "Speed" visible only when the
// "Rainbow" anim toggle is on.
// v1.56.283: unified per-child visibility for the combo expandables
// ("Custom colors", "ClickGUI Settings", Pathfinder "Settings"). The parent
// is identified by sniffing its child set - the three sets are disjoint.
static bool IsRainbowVisualRow(const char *pName)
{
        return str_comp(pName, "Custom colors") == 0 ||
                str_comp(pName, "ClickGUI Settings") == 0 ||
                str_comp(pName, "Settings") == 0;
}
static bool VisualRainbowChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &ch = pChildren[idx];
        bool HasSprite = false, HasRainbowMain = false, HasAStarWeight = false;
        for(int i = 0; i < childCount; ++i)
        {
                if(str_comp(pChildren[i].pName, "Sprite") == 0)
                        HasSprite = true;
                else if(str_comp(pChildren[i].pName, "Rainbow Main Color") == 0)
                        HasRainbowMain = true;
                else if(str_comp(pChildren[i].pName, "A* Weight") == 0)
                        HasAStarWeight = true;
        }
        if(HasSprite)
        {
                // Custom colors: Entity only for Sprite = Entities (2);
                // Speed only when the Rainbow toggle is on.
                if(str_comp(ch.pName, "Entity") == 0)
                {
                        for(int i = 0; i < childCount; ++i)
                        {
                                if(str_comp(pChildren[i].pName, "Sprite") == 0)
                                        return pChildren[i].valueIdx == 2;
                        }
                        return false;
                }
                if(str_comp(ch.pName, "Speed") == 0)
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
        if(HasRainbowMain)
        {
                // ClickGUI Settings: Speed only when Rainbow Main Color is on;
                // Panels width/height only when Dynamic scale (Beta) is off.
                auto FindChild = [&](const char *pFind) -> const SRow * {
                        for(int i = 0; i < childCount; ++i)
                        {
                                if(str_comp(pChildren[i].pName, pFind) == 0)
                                        return &pChildren[i];
                        }
                        return nullptr;
                };
                if(str_comp(ch.pName, "Speed") == 0)
                {
                        const SRow *pRainbow = FindChild("Rainbow Main Color");
                        return pRainbow && pRainbow->on;
                }
                if(str_comp(ch.pName, "Panels width") == 0 || str_comp(ch.pName, "Panels height") == 0)
                {
                        const SRow *pDyn = FindChild("Dynamic scale (Beta)");
                        return pDyn && !pDyn->on;
                }
                // v1.56.287: Blur Radius only when BG Blur is on.
                if(str_comp(ch.pName, "Blur Radius") == 0)
                {
                        const SRow *pBlur = FindChild("BG Blur");
                        return pBlur && pBlur->on;
                }
                return true;
        }
        if(HasAStarWeight)
        {
                // Pathfinder Settings: A* Weight + Freeze Support need Custom
                // effort (6); Beam width also Beam algorithm (1); Evolutions
                // also RHEA (2); Performance is always visible.
                if(str_comp(ch.pName, "Performance") == 0)
                        return true;
                if(g_Config.m_KxPfEffort != 6)
                        return false;
                if(str_comp(ch.pName, "Beam width") == 0)
                        return g_Config.m_KxPfAlgorithm == 1;
                if(str_comp(ch.pName, "Evolutions") == 0)
                        return g_Config.m_KxPfAlgorithm == 2;
                return true;
        }
        return true;
}
static bool BindsChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        // v6.7: Without Repeat needs a non-ID order (plain id cycling has no
        // meaningful "repeat").
        if(str_comp(row.pName, "Without Repeat") == 0)
                return g_Config.m_KxSdsOrder != 0;
        // v6.7: "Target ID" visible only when DDrace Team = Target (3).
        if(str_comp(row.pName, "Target ID") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "DDrace Team") == 0)
                                return pChildren[i].valueIdx == 3;
                }
        }
        // v6.7: "Targets IDs" visible only when Targets filter != Both.
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Targets") == 0)
                                return pChildren[i].valueIdx != 0;
                }
        }
        return true;
}
// v6.7: Smart Copy Moves — "Target ID" only when DDrace Team = Target (3);
// "Targets IDs"/"Untargets IDs" only when Targets filter != Both.
static bool SmartCopyChildVisible(const SRow *pChildren, int childCount, int idx)
{
        if(idx < 0 || idx >= childCount)
                return false;
        const SRow &row = pChildren[idx];
        if(str_comp(row.pName, "Target ID") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "DDrace Team") == 0)
                                return pChildren[i].valueIdx == 3;
                }
        }
        if(str_comp(row.pName, "Targets IDs") == 0 || str_comp(row.pName, "Untargets IDs") == 0)
        {
                for(int i = 0; i < childCount; ++i)
                {
                        if(str_comp(pChildren[i].pName, "Targets") == 0)
                                return pChildren[i].valueIdx != 0;
                }
        }
        return true;
}
// v1.56.129: BAF — "Angles" + "Silent" visible only when Aim toggle is on.
static bool AfChildVisible(const SRow *pChildren, int childCount, int idx)
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
// TPS row: the slider track spans the whole row except the Pause toggle
// pinned to the right edge (checkbox + label + double gap to the track).
static constexpr float KX_TPS_TOGGLE_W = 88.0f;
// Color with a stored ARGB alpha multiplied by the UI fade alpha — so an
// AA channel below ff actually renders translucent (WithAlpha alone would
// overwrite the stored alpha).
static ColorRGBA KxColorWithAlpha(unsigned RGBA, float Alpha)
{
        const float StoredA = ((RGBA >> 24) & 0xff) / 255.0f;
        return ColorRGBA(RGBA, true).WithAlpha(Alpha * StoredA);
}
static float KxTpsTrackW(const void *pRow, float RowW)
{
        if(pRow && str_comp(((const SRow *)pRow)->pName, "TPS") == 0)
                return RowW - 4.0f - KX_TPS_TOGGLE_W;
        return RowW - 4.0f;
}

// Offscreen margin: how far outside the visible screen panels fly when hidden.
static constexpr float OFFSCREEN_MARGIN = 100.0f;

// v6.2: "visual" expandables have no checkbox — clicking them only expands or
// collapses (they are pure containers, not on/off switches).
static bool IsVisualExpandableName(const char *pName)
{
        return str_comp(pName, "Visuals") == 0 ||
                str_comp(pName, "Line rendering") == 0 ||
                str_comp(pName, "TAS") == 0 ||
                str_comp(pName, "Settings") == 0 ||
                str_comp(pName, "Binds") == 0 ||
                str_comp(pName, "Spawner") == 0;
}

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
        case ERowType::ToggleInput: // v1.56.282
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

// v6.2: greedy word wrap for Label rows. The glyph width is estimated instead
// of calling ITextRender: the same layout pass also runs during the ClickGUI
// startup preload (CClickGui::OnInit), where touching the font system hangs
// the loading screen. ~8 px per glyph matches the 16 px font closely enough
// for informational labels.
static void KxWrapText(const char *pText, float MaxWidth, std::vector<std::string> &vOut)
{
        vOut.clear();
        if(!pText || !pText[0])
        {
                vOut.push_back("");
                return;
        }
        const float GlyphWidth = 8.0f;
        std::string Line;
        const char *p = pText;
        while(*p)
        {
                while(*p == ' ')
                        ++p;
                if(!*p)
                        break;
                const char *pStart = p;
                while(*p && *p != ' ')
                        ++p;
                const std::string Word(pStart, p);
                const std::string Try = Line.empty() ? Word : Line + " " + Word;
                if(!Line.empty() && (float)str_length(Try.c_str()) * GlyphWidth > MaxWidth)
                {
                        vOut.push_back(Line);
                        Line = Word;
                }
                else
                        Line = Try;
        }
        vOut.push_back(Line);
}

static int KxLabelLineCount(const char *pText, float MaxWidth)
{
        std::vector<std::string> vLines;
        KxWrapText(pText, MaxWidth, vLines);
        return (int)vLines.size();
}

// Row height taking the wrapped Label height into account.
static float RowHeightForRow(const SRow &row)
{
        if(row.type != ERowType::Label)
                return RowHeight(row.type);
        return std::max(ROW_H_TOGGLE, 10.0f + 20.0f * (float)std::max(1, row.m_LabelLines));
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

// v1.56.299: effective panel layout offsets. The master KxUiSettingsOn toggle
// owns every UI setting: while it is off the built-in defaults are used, so
// Panels Start X/Y and the X/Y indentation no longer leak into the layout.
void CClickGui::EffectivePanelLayout(float &StartX, float &StartY, float &IndentX, float &IndentY) const
{
        const bool On = g_Config.m_KxUiSettingsOn != 0;
        StartX = On ? (float)g_Config.m_KxPanelsStartX : 60.0f;
        StartY = On ? (float)g_Config.m_KxPanelsStartY : 90.0f;
        IndentX = On ? (float)g_Config.m_KxPanelsIndentX : 30.0f;
        IndentY = On ? (float)g_Config.m_KxPanelsIndentY : 12.0f;
}

// v1.56.299: drag-to-sort is a UI setting too — master off falls back to the
// built-in default (enabled).
bool CClickGui::EffectiveDragToSort() const
{
        return g_Config.m_KxUiSettingsOn != 0 ? (g_Config.m_KxDragToSort != 0) : true;
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
        // v1.56.260: configurable start offset (Settings → Panels Start X/Y).
        // v1.56.285: Panels X/Y-indentation = the gap BETWEEN panels, set
        // ABSOLUTELY by the sliders (0 = panels touch — no hidden base offset).
        // v1.56.299: these come through EffectivePanelLayout(), so the master
        // UI toggle off falls back to the built-in defaults.
        float startX, startY, indentX, indentY;
        EffectivePanelLayout(startX, startY, indentX, indentY);
        const float spacingX = std::max(indentX, 0.0f); // gap between panels horizontally
        const float spacingY = std::max(indentY, 0.0f); // gap between rows vertically

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

// Draws a string with per-character colours through the text renderer's colour
// splits. Unlike drawing every character separately, this keeps the glyph
// bearings and kerning intact: per-character calls treat each glyph as the
// first one, so the "no first character x-bearing" flag shifts letters that
// have a non-zero left bearing and makes the text look uneven.
static void KxDrawTextColorSplits(ITextRender *pTextRender, float x, float y, float size, const char *pText, ColorRGBA BaseColor, const std::vector<STextColorSplit> &vSplits)
{
        CTextCursor Cursor;
        Cursor.SetPosition(vec2(x, y));
        Cursor.m_FontSize = size;
        Cursor.m_vColorSplits = vSplits;
        pTextRender->TextColor(BaseColor);
        pTextRender->TextEx(&Cursor, pText, -1);
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
// v1.56.285: exponential smoothing (k = 7 / DURATION) — 1-exp(-7x) matches the
	// CSS curve cubic-bezier(0.16,1,0.3,1) almost exactly (87.5% at 30% time,
	// 97% at half time), so the pace matches the pre-284 build instead of
	// crawling in an exponential tail, and a mid-flight target flip stays
	// value-continuous (reverse-close fix). Snaps to the target when close
	// enough so the "val == 0" layout-cache boundary still fires.
void CClickGui::UpdateAnimations(float dt)
{
	const float expandSpeed = (EXPAND_ANIM_DURATION > 0.0f) ? (1.0f / EXPAND_ANIM_DURATION) : 1.0f;

	auto lerpToward = [dt](float &val, float target, float speed) {
		val += (target - val) * (1.0f - std::exp(-(7.0f * speed) * dt));
		if(std::fabs(target - val) < 0.002f)
			val = target;
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
                                lerpToward(row.m_AnimExpand, (row.expanded || row.m_SearchChildMatch) ? 1.0f : 0.0f, expandSpeed);
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

// v1.56.282: Dummy Manager — apply the staged proxies and reconnect.
// 1) Push every slot's proxy to the engine (IClient::SetDummyProxy);
// 2) reconnect connected dummies whose proxy changed (a fresh connection picks
//    up the new SOCKS5 socket; the engine's 5s reconnect delay is respected);
// 3) fully reconnect the MAIN connection if its proxy changed (UDP routing is
//    bound at connect time);
// 4) snapshot the applied state and clear m_ProxyDirty.
void CClickGui::ApplyProxiesAndReconnect()
{
        CDummyData &DD = GameClient()->m_BotNet.m_Dummy;

        for(int D = 0; D < MAX_DUMMIES; ++D)
                Client()->SetDummyProxy(D, DD.m_aProfiles[D].m_ProxyOn, DD.m_aProfiles[D].m_aProxy);

        for(int D = 1; D < MAX_DUMMIES; ++D)
        {
                const SDummyProfile &P = DD.m_aProfiles[D];
                const bool Changed = m_aaAppliedProxyOn[D] != (P.m_ProxyOn != 0) ||
                        str_comp(m_aaAppliedProxy[D], P.m_aProxy) != 0;
                if(!Changed || !Client()->DummyConnected(D))
                        continue;
                Client()->DummyDisconnect(D, nullptr);
                if(!Client()->DummyConnectingDelayed(D))
                        Client()->DummyConnect(D);
        }

        // Main: full reconnect when the main proxy changed.
        {
                const SDummyProfile &P = DD.m_aProfiles[0];
                const bool MainChanged = m_aaAppliedProxyOn[0] != (P.m_ProxyOn != 0) ||
                        str_comp(m_aaAppliedProxy[0], P.m_aProxy) != 0;
                if(MainChanged && Client()->State() != IClient::STATE_OFFLINE)
                {
                        // Copy the address first — Disconnect() may clear it.
                        char aAddr[256];
                        str_copy(aAddr, Client()->ConnectAddressString(), sizeof(aAddr));
                        Client()->Disconnect();
                        Client()->Connect(aAddr);
                }
        }

        for(int D = 0; D < MAX_DUMMIES; ++D)
        {
                m_aaAppliedProxyOn[D] = DD.m_aProfiles[D].m_ProxyOn != 0;
                str_copy(m_aaAppliedProxy[D], DD.m_aProfiles[D].m_aProxy, sizeof(m_aaAppliedProxy[D]));
        }
        m_ProxyDirty = false;
}

// v1.56.282: Dummy Manager — per-frame sync (called from OnRender).
// Mirrors the engine dummy limit and staged-proxy state into the file-static
// visibility mirrors, syncs the limit slider, the proxy ToggleInput rows and
// the Profiles/Tee target toggles from CBotNet::m_Dummy.
void CClickGui::UpdateDummyManagerRows()
{
        InitDummyManagerRows();

        CDummyData &DD = GameClient()->m_BotNet.m_Dummy;
        const int Limit = Client()->DummyLimit();
        g_DummyLimit = (Limit >= 1 && Limit <= MAX_DUMMIES) ? Limit : 8;
        g_ProxyDirty = m_ProxyDirty;

        // v1.56.282: dynamic visibility via row/child counts (no per-site checks):
        // "Reconnect (Apply)" is the LAST top-level row — hide it by truncating
        // rowCount while nothing is staged; connect/proxy children show only the
        // slots below the runtime limit; Profiles/Tee children are compacted per
        // "Edit Mode" (DummyManagerChildVisible decides visibility).
        SPanel &MgrPanel = g_Panels[NUM_PANELS - 2];
        MgrPanel.rowCount = m_ProxyDirty ? DUMMYMGR_ROW_APPLY + 1 : DUMMYMGR_ROW_APPLY;
        g_DummyMgrRows[DUMMYMGR_ROW_CONNECT].childCount = 2 + (g_DummyLimit - 1);
        g_DummyMgrRows[DUMMYMGR_ROW_PROXY].childCount = g_DummyLimit;
        for(int pass = 0; pass < 2; ++pass)
        {
                SRow *pMaster = (pass == 0) ? g_DummyProfilesMaster : g_DummyTeeMaster;
                SRow *pRows = (pass == 0) ? g_DummyProfilesChildren : g_DummyTeeChildren;
                const int Total = 4 + MAX_DUMMIES + (MAX_DUMMIES - 1);
                pMaster[0].valueIdx = pRows[0].valueIdx; // keep the live Edit Mode selection
                int n = 0;
                for(int i = 0; i < Total; ++i)
                {
                        if(DummyManagerChildVisible(pMaster, Total, i))
                                pRows[n++] = pMaster[i];
                }
                g_DummyMgrRows[(pass == 0) ? DUMMYMGR_ROW_PROFILES : DUMMYMGR_ROW_TEE].childCount = n;
        }

        // Slider "Dummies" <- engine limit (don't stomp while the user drags it).
        const int RowCount = (int)(sizeof(g_DummyMgrRows) / sizeof(g_DummyMgrRows[0]));
        const bool DraggingLimit = (m_DragSliderPanel == NUM_PANELS - 2 && m_DragSliderChild < 0 &&
                m_DragSliderRow >= 0 && m_DragSliderRow < RowCount &&
                str_comp(g_Panels[NUM_PANELS - 2].pRows[m_DragSliderRow].pName, "Dummies") == 0);
        if(!DraggingLimit)
                g_DummyMgrRows[DUMMYMGR_ROW_LIMIT].value = (float)g_DummyLimit;

        // Proxy ToggleInput rows <- per-slot profile proxy state.
        for(int D = 0; D < MAX_DUMMIES; ++D)
        {
                SRow &ch = g_DummyProxyChildren[D];
                ch.on = DD.m_aProfiles[D].m_ProxyOn != 0;
                if(!(m_InputEditing && m_InputPanel == NUM_PANELS - 2 && m_InputRow == DUMMYMGR_ROW_PROXY && m_InputChild == D))
                {
                        str_copy(ch.m_aInputBuf, DD.m_aProfiles[D].m_aProxy, sizeof(ch.m_aInputBuf));
                        ch.m_InputInitialized = true;
                }
        }

        // Profiles/Tee target toggles <- ClickGUI-local target selections.
        for(int pass = 0; pass < 2; ++pass)
        {
                SRow *pRows = (pass == 0) ? g_DummyProfilesChildren : g_DummyTeeChildren;
                const bool *pTargets = (pass == 0) ? m_aProfileTargets : m_aTeeTargets;
                for(int i = 0; i < 4 + MAX_DUMMIES + (MAX_DUMMIES - 1); ++i)
                {
                        SRow &ch = pRows[i];
                        if(ch.type != ERowType::Toggle)
                                continue;
                        const int d = DummyMgrParseDummyName(ch.pName);
                        if(d >= 0)
                                ch.on = pTargets[d];
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
// v1.56.284: EXE SHA-256 — computed once per session from the running executable.
// Extracted so it can run during CClickGui::OnInit() (client loading stage)
// instead of stalling the first ClickGUI open.
static void KxComputeExeHash()
{
        static bool s_Done = false;
        if(s_Done)
                return;
        s_Done = true;
        str_copy(g_InfoDyn_ExeHash, "EXE SHA-256: n/a", sizeof(g_InfoDyn_ExeHash));
        char aExePath[1024];
        aExePath[0] = 0;
#ifdef _WIN32
        GetModuleFileNameA(nullptr, aExePath, (unsigned long)sizeof(aExePath));
#endif
        if(aExePath[0])
        {
                IOHANDLE File = io_open(aExePath, IOFLAG_READ);
                if(File)
                {
                        const long FileSize = io_length(File);
                        if(FileSize > 0 && FileSize < 512 * 1024 * 1024)
                        {
                                unsigned char *pData = (unsigned char *)malloc((size_t)FileSize);
                                if(pData)
                                {
                                        if(io_read(File, pData, (unsigned)FileSize) == (unsigned)FileSize)
                                        {
                                                SHA256_DIGEST Digest = sha256(pData, (size_t)FileSize);
                                                char aHashStr[SHA256_MAXSTRSIZE];
                                                sha256_str(Digest, aHashStr, sizeof(aHashStr));
                                                str_format(g_InfoDyn_ExeHash, sizeof(g_InfoDyn_ExeHash), "EXE SHA-256: %s", aHashStr);
                                        }
                                        free(pData);
                                }
                        }
                        io_close(File);
                }
        }
}

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

        // v1.56.284: EXE SHA-256 — computed once per session from the running
        // executable file (precomputed during CClickGui::OnInit()).
        KxComputeExeHash();
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

static void TasSpawnerEnsureInput(SRow &Row)
{
        if(Row.m_InputInitialized)
                return;
        if(Row.pInputValue)
                str_copy(Row.m_aInputBuf, Row.pInputValue, sizeof(Row.m_aInputBuf));
        Row.m_InputInitialized = true;
}

static float TasSpawnerReadFloat(SRow &Row)
{
        TasSpawnerEnsureInput(Row);
        return (float)atof(Row.m_aInputBuf);
}

static int TasSpawnerReadInt(SRow &Row)
{
        TasSpawnerEnsureInput(Row);
        return atoi(Row.m_aInputBuf);
}

// Pushes the Spawner panel rows into CTas::SpawnerSettings() and updates the
// dynamic labels / conditional dropdown entries. The rows are the source of
// truth, so this runs every frame while the ClickGUI is open (and again right
// before a Spawn/Kill click).
static void SyncTasSpawnerRows(CTas &Tas)
{
        SRow *pRows = g_TasSpawnerChildren;
        STasSpawnerSettings &S = Tas.SpawnerSettings();

        // "Previous Spawn" is only offered after at least one spawn — fix the
        // option list before reading the selection.
        const bool HasPrev = Tas.SpawnerHasPrevious();
        pRows[TAS_SPAWN_ROW_COORD_MODE].pOptions = HasPrev ? g_TasSpawnerCoordOpts : g_TasSpawnerCoordOptsNoPrev;
        pRows[TAS_SPAWN_ROW_COORD_MODE].optionCount = HasPrev ? 3 : 2;
        if(pRows[TAS_SPAWN_ROW_COORD_MODE].valueIdx >= pRows[TAS_SPAWN_ROW_COORD_MODE].optionCount)
                pRows[TAS_SPAWN_ROW_COORD_MODE].valueIdx = 0;

        S.m_Mode = pRows[TAS_SPAWN_ROW_MODE].valueIdx == 1 ? TAS_SPAWN_MODE_TILE : TAS_SPAWN_MODE_ENTITY;

        const int EntityCount = CTas::SpawnerEntityCount();
        SRow &EntityRow = pRows[TAS_SPAWN_ROW_ENTITY_ID];
        EntityRow.value = std::clamp(EntityRow.value, 0.0f, (float)(EntityCount - 1));
        S.m_Entity = (int)(EntityRow.value + 0.5f);
        if(S.m_Entity < 0 || S.m_Entity >= EntityCount)
                S.m_Entity = 0;

        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_NAME]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_CLAN]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_SKIN]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_TEE_COLOR]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_LEGS_COLOR]);
        str_copy(S.m_aName, pRows[TAS_SPAWN_ROW_NAME].m_aInputBuf, sizeof(S.m_aName));
        str_copy(S.m_aClan, pRows[TAS_SPAWN_ROW_CLAN].m_aInputBuf, sizeof(S.m_aClan));
        S.m_Id = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_ID]);
        str_copy(S.m_aSkin, pRows[TAS_SPAWN_ROW_SKIN].m_aInputBuf, sizeof(S.m_aSkin));
        str_copy(S.m_aColorBody, pRows[TAS_SPAWN_ROW_CC_TEE].on ? pRows[TAS_SPAWN_ROW_TEE_COLOR].m_aInputBuf : "", sizeof(S.m_aColorBody));
        str_copy(S.m_aColorFeet, pRows[TAS_SPAWN_ROW_CC_LEGS].on ? pRows[TAS_SPAWN_ROW_LEGS_COLOR].m_aInputBuf : "", sizeof(S.m_aColorFeet));

        S.m_HookStrength = (int)SnapSliderStep(pRows[TAS_SPAWN_ROW_HOOK_STRENGTH]);
        S.m_HookStrengthMode = pRows[TAS_SPAWN_ROW_HOOK_MODE].valueIdx;
        S.m_PracticeParticles = pRows[TAS_SPAWN_ROW_PRACTICE].on;
        // Weapon ID: -1 = bare tee.
        S.m_WeaponId = (int)SnapSliderStep(pRows[TAS_SPAWN_ROW_WEAPON_ID]);

        S.m_EntityPhysics = pRows[TAS_SPAWN_ROW_APPLY_PHYSICS].on;
        S.m_PrevPosX = TasSpawnerReadFloat(pRows[TAS_SPAWN_ROW_PREV_X]);
        S.m_PrevPosY = TasSpawnerReadFloat(pRows[TAS_SPAWN_ROW_PREV_Y]);
        S.m_EntityTimelineTick = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_TIMELINE]);

        SRow &TileRow = pRows[TAS_SPAWN_ROW_TILE_ID];
        TileRow.value = std::clamp(TileRow.value, 0.0f, (float)(CTas::SpawnerTileCount() - 1));
        S.m_Tile = CTas::SpawnerTileId((int)(TileRow.value + 0.5f));
        S.m_TilePhysics = pRows[TAS_SPAWN_ROW_TILE_PHYSICS].on;
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_TILE_NUMBER]);
        S.m_TileNumber = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_TILE_NUMBER]);
        // Tele numbers are 1-255 (0 = "no teleport" in the engine) — clamp the
        // input field itself so the stored value is always valid.
        if(S.m_Tile == TILE_TELEIN || S.m_Tile == TILE_TELEINEVIL || S.m_Tile == TILE_TELEOUT || S.m_Tile == TILE_TELECHECKIN)
        {
                const int Raw = S.m_TileNumber;
                S.m_TileNumber = std::clamp(S.m_TileNumber, 1, 255);
                if(Raw != S.m_TileNumber)
                {
                        char aBuf[8];
                        str_format(aBuf, sizeof(aBuf), "%d", S.m_TileNumber);
                        str_copy(pRows[TAS_SPAWN_ROW_TILE_NUMBER].m_aInputBuf, aBuf);
                }
        }

        S.m_CoordsMode = pRows[TAS_SPAWN_ROW_COORD_MODE].valueIdx;
        S.m_X = TasSpawnerReadFloat(pRows[TAS_SPAWN_ROW_X]);
        S.m_Y = TasSpawnerReadFloat(pRows[TAS_SPAWN_ROW_Y]);
        S.m_Snap = pRows[TAS_SPAWN_ROW_SNAP].on;
        S.m_LoopDelay = (int)SnapSliderStep(pRows[TAS_SPAWN_ROW_LOOP_DELAY]);

        // v6.2: physics fields of the non-tee entities.
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_VEL_X]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_VEL_Y]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_LIFESPAN]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_LASER_REACH]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_LASER_LENGTH]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_DRAGGER_STRENGTH]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_SUBTYPE]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_SWITCH]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_SPEEDUP_FORCE]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_SPEEDUP_MAX_SPEED]);
        TasSpawnerEnsureInput(pRows[TAS_SPAWN_ROW_SPEEDUP_ANGLE]);
        S.m_Dir = pRows[TAS_SPAWN_ROW_DIR].valueIdx;
        S.m_VelX = TasSpawnerReadFloat(pRows[TAS_SPAWN_ROW_VEL_X]);
        S.m_VelY = TasSpawnerReadFloat(pRows[TAS_SPAWN_ROW_VEL_Y]);
        S.m_LifeSpan = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_LIFESPAN]);
        S.m_LaserReach = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_LASER_REACH]);
        S.m_LaserLength = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_LASER_LENGTH]);
        S.m_DraggerStrength = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_DRAGGER_STRENGTH]);
        // v6.3: per-entity parameters.
        S.m_ProjectileFreeze = pRows[TAS_SPAWN_ROW_PROJECTILE_FREEZE].on;
        S.m_ProjectileExplosive = pRows[TAS_SPAWN_ROW_PROJECTILE_EXPLOSIVE].on;
        S.m_ProjectileSound = pRows[TAS_SPAWN_ROW_PROJECTILE_SOUND].on;
        S.m_LaserType = pRows[TAS_SPAWN_ROW_LASER_TYPE].valueIdx;
        S.m_PickupType = pRows[TAS_SPAWN_ROW_PICKUP_TYPE].valueIdx;
        S.m_Subtype = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_SUBTYPE]);
        S.m_SwitchNumber = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_SWITCH]);
        S.m_SpeedupForce = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_SPEEDUP_FORCE]);
        S.m_SpeedupMaxSpeed = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_SPEEDUP_MAX_SPEED]);
        S.m_SpeedupAngle = TasSpawnerReadInt(pRows[TAS_SPAWN_ROW_SPEEDUP_ANGLE]);

        // Engine -> UI (dynamic value labels and the loop button).
        str_format(g_TasDyn_EntityName, sizeof(g_TasDyn_EntityName), "%s", CTas::SpawnerEntityName(S.m_Entity));
        str_format(g_TasDyn_TileName, sizeof(g_TasDyn_TileName), "%s", CTas::SpawnerTileName((int)(TileRow.value + 0.5f)));
        pRows[TAS_SPAWN_ROW_ENTITY_ID].pValueLabel = g_TasDyn_EntityName;
        pRows[TAS_SPAWN_ROW_TILE_ID].pValueLabel = g_TasDyn_TileName;
        str_copy(g_TasDyn_Loop, Tas.SpawnerLoopActive() ? "Stop Loop Spawn" : "Loop Spawn", sizeof(g_TasDyn_Loop));
}

// v6.2: Playground Settings rows <-> CTas. The header checkbox is the master
// switch of the whole group: with it off every virtual subsystem is disabled
// (the individual toggles keep their state and come back with the master).
static void SyncTasPgRows(CTas &Tas)
{
        SRow *pRows = g_TasPgChildren;
        SRow *pMaster = nullptr;
        for(int r = 0; r < (int)(sizeof(g_TasRows) / sizeof(g_TasRows[0])); ++r)
                if(str_comp(g_TasRows[r].pName, "Playground Settings") == 0)
                {
                        pMaster = &g_TasRows[r];
                        break;
                }
        if(!pMaster)
                return;

        // v6.3: PlaygroundReset() wipes the world; mirror that in the rows so
        // the virtual switches do not immediately re-enable themselves (which
        // kept the Reset Playground button visible after pressing it).
        static int s_PgResetSerial = -1;
        if(s_PgResetSerial != Tas.PgResetSerial())
        {
                s_PgResetSerial = Tas.PgResetSerial();
                pMaster->on = false;
                pRows[TAS_PG_ROW_VIRTUAL_RCON].on = false;
                pRows[TAS_PG_ROW_VIRTUAL_CHAT].on = false;
                pRows[TAS_PG_ROW_PRACTICE].on = false;
        }

        static bool s_PgInit = false;
        if(!s_PgInit)
        {
                s_PgInit = true;
                pMaster->on = Tas.PgMaster();
                pRows[TAS_PG_ROW_VIRTUAL_RCON].on = Tas.PgVirtualRcon();
                pRows[TAS_PG_ROW_VIRTUAL_CHAT].on = Tas.PgVirtualChat();
                pRows[TAS_PG_ROW_PRACTICE].on = Tas.PgPractice();
        }

        Tas.SetPgMaster(pMaster->on);
        const bool Master = pMaster->on;
        Tas.SetPgVirtualRcon(Master && pRows[TAS_PG_ROW_VIRTUAL_RCON].on);
        Tas.SetPgVirtualChat(Master && pRows[TAS_PG_ROW_VIRTUAL_CHAT].on);
        Tas.SetPgPractice(Master && pRows[TAS_PG_ROW_PRACTICE].on);
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

	// Spawner rows -> engine settings (and dynamic labels back into the rows).
	SyncTasSpawnerRows(Tas);
	// v6.2: Playground Settings (virtual rcon / chat / practice).
	SyncTasPgRows(Tas);

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
					// v6.2: enabling the master toggle no longer pops the
					// expandable open — expanding stays a separate action.
					Row.on = Tas.AutoRfEnabled();
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
                {
                        Tas.SetTps(Row.value);
                        // Pause moved onto the TPS row's right button (v6.3).
                        Row.on = Tas.Paused();
                }
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
        char aDir[1024];
        Storage()->GetCompletePath(IStorage::TYPE_SAVE, "Kinetix/Scripts", aDir, sizeof(aDir));
        char aPath[1024];
        if(KxPickScriptFile(aPath, sizeof(aPath), aDir))
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
        // v6.5: central Kinetix data folder (save dir; on Android the app's
        // external files dir, i.e. Android/data/<pkg>/files/Kinetix/Scripts).
        Storage()->CreateFolder("Kinetix", IStorage::TYPE_SAVE);
        Storage()->CreateFolder("Kinetix/Scripts", IStorage::TYPE_SAVE);
        Storage()->ListDirectory(IStorage::TYPE_SAVE, "Kinetix/Scripts", ScriptBrowserListCb, &m_vScriptBrowserFiles);
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
        Storage()->GetCompletePath(IStorage::TYPE_SAVE, "Kinetix/Scripts", aComplete, sizeof(aComplete));
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
                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f * m_MainOpacity));
                        DrawText(listX + 6.0f, rowY + 6.0f, 16.0f, m_vScriptBrowserFiles[i].c_str(),
                                ColorRGBA(0xffd8d8e8, true).WithAlpha(alpha));
                }
                Graphics()->ClipDisable();
        }

        const bool cancelHover = PointInRect(m_MousePos, boxX, cancelY, boxW, 34.0f);
        DrawRoundedRect(boxX, cancelY, boxW, 34.0f, 4.0f, IGraphics::CORNER_ALL,
                KxColorWithAlpha(cancelHover ? m_AccentColorRGBA : 0xff282834u, (cancelHover ? alpha * 0.6f : alpha) * m_MainOpacity));
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

void CClickGui::OnInit()
{
        // Preload the ClickGUI during the client loading stage so the first
        // open does not hitch:
        //  - Dummy Manager row tables (lazily built otherwise)
        //  - blur framebuffers (two full-resolution FBOs; expensive with blur on)
        //  - EXE SHA-256 (reads the whole executable once)
        //  - Yoga layout caches for all panels (the 93 yoga::Node allocations)
        InitDummyManagerRows();
        UpdateDummyManagerRows();
        EnsureBlurFbos();
        KxComputeExeHash();
        // v6.4: ArrayInfo logo icon (drawn inline in the info bar).
        m_ArrayInfoLogo = Graphics()->LoadTexture("kinetix_icon.png", IStorage::TYPE_ALL);

        m_bPreloadLayout = true;
        for(int i = 0; i < NUM_PANELS; ++i)
                RenderPanel(i, vec2(0.0f, 0.0f), 0.0f);
        m_bPreloadLayout = false;
}

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
                // v1.56.260: collapsed state persists across server changes —
                // panels only reset on client exit (process lifetime).
                m_aPanelScroll[i] = 0.0f;
                m_aPanelScrollStored[i] = 0.0f;
                m_aPanelScrollTarget[i] = 0.0f;
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

        // v1.56.287: free the BG Blur frame buffers (recreated on demand).
        DestroyBlurFbos();
}

// v1.56.290: BG Blur — frees the blur frame buffers.
void CClickGui::DestroyBlurFbos()
{
	IGraphics::CFrameBufferHandle *apFbs[] = {&m_SceneFB, &m_BlurFbTmp};
	for(auto *pFb : apFbs)
	{
		if(pFb->IsValid())
			Graphics()->DestroyFrameBuffer(*pFb);
	}
	m_BlurFbScreenWidth = 0;
	m_BlurFbScreenHeight = 0;
}

// v1.56.290: BG Blur — (re)creates the capture frame buffers at the current
// screen size. Called on demand; recreates on resolution change.
void CClickGui::EnsureBlurFbos()
{
	const int ScreenW = Graphics()->ScreenWidth();
	const int ScreenH = Graphics()->ScreenHeight();
	if(ScreenW <= 0 || ScreenH <= 0)
		return;
	if(m_SceneFB.IsValid() && m_BlurFbTmp.IsValid() && m_BlurFbScreenWidth == ScreenW && m_BlurFbScreenHeight == ScreenH)
		return;

	DestroyBlurFbos();
	if(!Graphics()->HasFrameBufferSupport())
		return;

	// v1.56.298: one capture for the horizontal pass, one buffer for its result
	m_SceneFB = Graphics()->CreateFrameBuffer((size_t)ScreenW, (size_t)ScreenH);
	m_BlurFbTmp = Graphics()->CreateFrameBuffer((size_t)ScreenW, (size_t)ScreenH);
	if(!m_SceneFB.IsValid() || !m_BlurFbTmp.IsValid())
	{
		DestroyBlurFbos();
		return;
	}

	m_BlurFbScreenWidth = ScreenW;
	m_BlurFbScreenHeight = ScreenH;
}

// v1.56.298: BG Blur — a true separable gaussian blur, the standard two-pass
// algorithm. The screen (the game behind the GUI) is captured once, then blurred
// with a 1D gaussian kernel: first horizontally, then vertically. Consecutive
// texel pairs are fetched with a single bilinear tap (offset i + w1/(w0+w1),
// combined weight w0+w1), which halves the draw count while keeping the kernel
// exact. Unlike the old sparse 2D tap cloud this kernel is sampled densely (one
// texel per step), so there is no undersampling -> no "worm" ghosting, and the
// radius maps continuously to sigma (no level switches, strictly monotonic).
void CClickGui::RenderBlurBackground(float Fade)
{
	EnsureBlurFbos();
	if(!m_SceneFB.IsValid() || !m_BlurFbTmp.IsValid())
		return;

	// v1.56.289: the blur strength follows the ClickGUI open/close animation
	// (m_AnimProgress is already the eased value, i.e. an ease-out curve), so
	// the backdrop smoothly unblurs on close and blurs in on open instead of
	// popping. Radius 0 means "no blur" and leaves the screen untouched.
	const float Radius = std::clamp((float)g_Config.m_KxUiBgBlurRadius, 0.0f, 100.0f) * std::clamp(Fade, 0.0f, 1.0f);
	if(Radius <= 0.05f)
		return; // no blur requested: leave the screen untouched

	// save the current projection (restored below for the panel rendering)
	const CScreenRect OldScreen = Graphics()->GetScreen();

	const float screenW = VScreenWidth();
	const float screenH = VScreenHeight();
	if(screenW <= 0.0f || screenH <= 0.0f)
	{
		Graphics()->MapScreen(OldScreen);
		return;
	}

	const float PhysW = (float)Graphics()->ScreenWidth();
	const float PhysH = (float)Graphics()->ScreenHeight();
	const float UiScale = (m_UiScale > 0.0f) ? m_UiScale : 1.0f;

	// capture the screen (the game behind the GUI)
	Graphics()->CopyScreenToFrameBuffer(m_SceneFB);

	// the radius maps continuously to the gaussian sigma (virtual pixels); the
	// kernel itself is evaluated in physical texels of the capture
	const float MinDim = std::min(screenW, screenH);
	const float Sigma = std::max(0.01f, (Radius / 100.0f) * MinDim * 0.013f);
	const float SigmaTex = Sigma * UiScale;

	Graphics()->MapScreenToSize(screenW, screenH);
	Graphics()->WrapClamp();
	const IGraphics::CQuadItem FullQuad(0.0f, 0.0f, screenW, screenH);

	// one separable 1D gaussian pass: the source is drawn into the current render
	// target as a running weighted average of the gaussian taps
	auto BlurPass = [&](bool Horizontal, IGraphics::CTextureHandle Texture) {
		const float InvTex = Horizontal ? (1.0f / PhysW) : (1.0f / PhysH);
		const int R = std::max(1, (int)std::ceil(2.0f * SigmaTex));
		const float TwoSigmaSq = 2.0f * SigmaTex * SigmaTex;

		Graphics()->TextureSet(Texture);

		// the centre texel (the capture itself) is drawn opaque
		Graphics()->BlendNone();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		Graphics()->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
		Graphics()->QuadsDrawTL(&FullQuad, 1);
		Graphics()->QuadsEnd();

		Graphics()->BlendNormal();
		Graphics()->QuadsBegin();
		float CumulativeWeight = 1.0f;
		auto AddTap = [&](float OffsetTexels, float Weight) {
			if(Weight < 0.0001f)
				return;
			const float Alpha = Weight / (CumulativeWeight + Weight);
			CumulativeWeight += Weight;
			const float D = OffsetTexels * InvTex;
			const float Du = Horizontal ? D : 0.0f;
			const float Dv = Horizontal ? 0.0f : D;
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
			Graphics()->QuadsSetSubset(Du, Dv, 1.0f + Du, 1.0f + Dv);
			Graphics()->QuadsDrawTL(&FullQuad, 1);
		};
		// bilinear pairing: sampling between texels i and i+1 at i + w1/(w0+w1)
		// yields exactly the two gaussian-weighted texels for a single tap
		for(int i = 1; i + 1 <= R; i += 2)
		{
			const float W0 = std::exp(-(float)(i * i) / TwoSigmaSq);
			const float W1 = std::exp(-(float)((i + 1) * (i + 1)) / TwoSigmaSq);
			const float PairWeight = W0 + W1;
			const float Pos = (float)i + W1 / PairWeight;
			AddTap(Pos, PairWeight);
			AddTap(-Pos, PairWeight);
		}
		if(R % 2 == 1)
		{
			const float W = std::exp(-(float)(R * R) / TwoSigmaSq);
			AddTap((float)R, W);
			AddTap((float)-R, W);
		}
		Graphics()->QuadsEnd();
	};

	// horizontal pass: capture -> screen
	BlurPass(true, Graphics()->GetFrameBufferTexture(m_SceneFB));
	// store the horizontally blurred screen in a separate frame buffer (the
	// vertical pass must not read an image that is rewritten in the same frame)
	Graphics()->CopyScreenToFrameBuffer(m_BlurFbTmp);
	// vertical pass: horizontal result -> screen
	BlurPass(false, Graphics()->GetFrameBufferTexture(m_BlurFbTmp));

	// restore the render state for the panel rendering
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	Graphics()->BlendNormal();
	Graphics()->WrapNormal();
	Graphics()->TextureClear();
	Graphics()->MapScreen(OldScreen);
}

void CClickGui::OnRender()
{
	const bool isOpen = g_Config.m_ClClickGui != 0;

	// v6.3: the ArrayList HUD lives in game and keeps rendering while the
	// ClickGUI is closed (and while the editor preview is active).
	const bool ArrayListVisible = ShouldRenderArrayList();
	const bool ArrayInfoVisible = ShouldRenderArrayInfo();
	const bool ArrayPlayerVisible = ShouldRenderArrayPlayer();

	// Perf: when fully closed (including the close animation), there is nothing
	// to update or draw besides the ArrayList HUD. The heavy panel/UI sync
	// below used to run every frame and only bail out at the very end of this
	// function, burning ~0.4ms/frame even with the ClickGUI closed.
	if(!isOpen && m_AnimProgress <= 0.0f)
	{
		if(!ArrayListVisible && !ArrayInfoVisible && !ArrayPlayerVisible)
		{
			m_ArrayListDragging = false;
			m_ArrayInfoDragging = false;
			m_ArrayPlayerDragging = false;
			m_ArrayPlayerResizing = false;
			return;
		}
		// Minimal per-frame setup for the HUD-only path (scale + cursor).
		const float physH = (float)Graphics()->ScreenHeight();
		const float GuiScale = (g_Config.m_KxUiSettingsOn != 0) ? (g_Config.m_KxClickGuiScale / 10.0f) : 1.0f;
		m_UiScale = ((physH > 0.0f) ? (physH / 1080.0f) : 1.0f) * GuiScale;
		if(!m_TouchActive)
			m_MousePos = Input()->NativeMousePos() / (m_UiScale > 0.0f ? m_UiScale : 1.0f);
		// The editor is a positioning mode: keep the OS cursor visible so the
		// HUD can be dragged. The console manages its own mouse mode.
		if((m_ArrayListEditor || m_ArrayInfoEditor || m_ArrayPlayerEditor) && !GameClient()->m_GameConsole.IsActive())
			Input()->MouseModeAbsolute();
		// v6.3: poll the left button directly so dragging works even if the
		// press event never reaches CClickGui::OnInput while the GUI is closed.
		if(!Input()->KeyIsPressed(KEY_MOUSE_1))
		{
			m_ArrayListDragging = false;
			m_ArrayInfoDragging = false;
			m_ArrayPlayerDragging = false;
			m_ArrayPlayerResizing = false;
			m_ArrayPlayerBtnWasDown = false;
		}
		else
		{
			// v6.7: media transport buttons (edge-triggered, HUD-only path).
			if(m_ArrayPlayerBtnRectValid && !m_ArrayPlayerEditor && !m_ArrayPlayerBtnWasDown)
			{
				for(int b = 0; b < 3; ++b)
				{
					if(PointInRect(m_MousePos, m_aArrayPlayerBtnRect[b][0], m_aArrayPlayerBtnRect[b][1], m_aArrayPlayerBtnRect[b][2], m_aArrayPlayerBtnRect[b][3]))
					{
						KxMedia::Command(b);
						m_ArrayPlayerBtnWasDown = true;
						break;
					}
				}
			}
			if(m_ArrayListEditor && !m_ArrayListDragging && ArrayListHitTest(m_MousePos))
			{
				m_ArrayListDragging = true;
				m_ArrayListDragStartMouse = m_MousePos;
				m_ArrayListDragStartPos = vec2(m_ArrayListRectX, m_ArrayListRectY);
			}
			if(m_ArrayInfoEditor && !m_ArrayInfoDragging && ArrayInfoHitTest(m_MousePos))
			{
				m_ArrayInfoDragging = true;
				m_ArrayInfoDragStartMouse = m_MousePos;
				m_ArrayInfoDragStartPos = vec2(m_ArrayInfoRectX, m_ArrayInfoRectY);
			}
			if(m_ArrayPlayerEditor && !m_ArrayPlayerDragging && !m_ArrayPlayerResizing)
			{
				if(ArrayPlayerResizeHitTest(m_MousePos))
				{
					m_ArrayPlayerResizing = true;
					m_ArrayPlayerResizeStartMouse = m_MousePos;
					m_ArrayPlayerResizeStartW = g_Config.m_KxArrayPlayerWidth;
					m_ArrayPlayerResizeStartH = g_Config.m_KxArrayPlayerHeight;
				}
				else if(ArrayPlayerHitTest(m_MousePos))
				{
					m_ArrayPlayerDragging = true;
					m_ArrayPlayerDragStartMouse = m_MousePos;
					m_ArrayPlayerDragStartPos = vec2(m_ArrayPlayerRectX, m_ArrayPlayerRectY);
				}
			}
		}
		Graphics()->MapScreenToSize(VScreenWidth(), VScreenHeight());
		Graphics()->TextureClear();
		if(ArrayListVisible)
		{
			UpdateArrayList(Client()->RenderFrameTime());
			RenderArrayList(1.0f);
		}
		if(ArrayInfoVisible)
		{
			UpdateArrayInfo(Client()->RenderFrameTime());
			RenderArrayInfo(1.0f);
		}
		if(ArrayPlayerVisible)
		{
			UpdateArrayPlayer(Client()->RenderFrameTime());
			RenderArrayPlayer(1.0f);
		}
		const bool HoverPlayerResize = ArrayPlayerResizeHitTest(m_MousePos);
		const bool HoverPlayer = ArrayPlayerHitTest(m_MousePos);
		KxSetCursorShape(HoverPlayerResize ? 5 : (((m_ArrayListDragging || ArrayListHitTest(m_MousePos)) || (m_ArrayInfoDragging || ArrayInfoHitTest(m_MousePos)) || (m_ArrayPlayerDragging || HoverPlayer)) ? 9 : 0));
		return;
	}

	// v1.56.255: UI persistence — accent/panel colors + rainbow + blackout.
	// Pull once after settings.cfg is loaded, push every frame so changes
	// are written back and saved on exit.
	// v1.56.284: master KxUiSettingsOn=0 renders built-in defaults and does
	// NOT push, so the saved config survives toggling the master back on.
	{
		static bool s_Init = false;
		static bool s_Off = false;
		if(!s_Init)
		{
			s_Init = true;
			unsigned hex = 0;
			if(sscanf(g_Config.m_KxUiAccentColor, "%6x", &hex) == 1)
			{
				m_AccentColorRGBA = 0xff000000u | (hex & 0xffffff);
				m_SavedAccentColorRGBA = m_AccentColorRGBA;
			}
			hex = 0;
			if(sscanf(g_Config.m_KxUiPanelColor, "%6x", &hex) == 1)
				m_PanelBgRGBA = 0xff000000u | (hex & 0xffffff);
			m_MainOpacity = g_Config.m_KxUiMainOpacity / 100.0f;
			m_PanelOpacity = g_Config.m_KxUiPanelOpacity / 100.0f;
			m_RainbowEnabled = g_Config.m_KxUiRainbow != 0;
			m_RainbowSpeed = g_Config.m_KxUiRainbowSpeed / 10.0f;
			m_BgBlackout = g_Config.m_KxUiBlackout;
			s_Off = (g_Config.m_KxUiSettingsOn == 0);
		}
		else if(g_Config.m_KxUiSettingsOn == 0)
		{
			m_AccentColorRGBA = 0xff9d4edd;
			m_SavedAccentColorRGBA = m_AccentColorRGBA;
			m_PanelBgRGBA = 0xff161622;
			m_RainbowEnabled = false;
			m_RainbowSpeed = 1.0f;
			m_BgBlackout = 30;
			s_Off = true;
		}
		else
		{
			if(s_Off)
			{
				// master turned back on — restore the saved state from config.
				unsigned hex = 0;
				if(sscanf(g_Config.m_KxUiAccentColor, "%6x", &hex) == 1)
				{
					m_AccentColorRGBA = 0xff000000u | (hex & 0xffffff);
					m_SavedAccentColorRGBA = m_AccentColorRGBA;
				}
				hex = 0;
				if(sscanf(g_Config.m_KxUiPanelColor, "%6x", &hex) == 1)
					m_PanelBgRGBA = 0xff000000u | (hex & 0xffffff);
				m_MainOpacity = g_Config.m_KxUiMainOpacity / 100.0f;
				m_PanelOpacity = g_Config.m_KxUiPanelOpacity / 100.0f;
				m_RainbowEnabled = g_Config.m_KxUiRainbow != 0;
				m_RainbowSpeed = g_Config.m_KxUiRainbowSpeed / 10.0f;
				m_BgBlackout = g_Config.m_KxUiBlackout;
				s_Off = false;
			}
			str_format(g_Config.m_KxUiAccentColor, sizeof(g_Config.m_KxUiAccentColor), "%06x", m_SavedAccentColorRGBA & 0xffffff);
			str_format(g_Config.m_KxUiPanelColor, sizeof(g_Config.m_KxUiPanelColor), "%06x", m_PanelBgRGBA & 0xffffff);
			g_Config.m_KxUiMainOpacity = (int)(m_MainOpacity * 100.0f + 0.5f);
			g_Config.m_KxUiPanelOpacity = (int)(m_PanelOpacity * 100.0f + 0.5f);
			g_Config.m_KxUiRainbow = m_RainbowEnabled ? 1 : 0;
			g_Config.m_KxUiRainbowSpeed = (int)(m_RainbowSpeed * 10.0f + 0.5f);
			g_Config.m_KxUiBlackout = m_BgBlackout;
		}
	}

        // v1.56.212: DPI-independent UI scale. m_UiScale = screenH / 1080 maps
        // physical pixels into the virtual 1920x1080 space used by all ClickGUI
        // geometry, so panels keep the same physical size on every resolution
        // as they have at 1920x1080.
        const float physH = (float)Graphics()->ScreenHeight();
        // v1.56.284: ClickGUI Scale applies only when UI settings are enabled.
        const float GuiScale = (g_Config.m_KxUiSettingsOn != 0) ? (g_Config.m_KxClickGuiScale / 10.0f) : 1.0f;
        m_UiScale = ((physH > 0.0f) ? (physH / 1080.0f) : 1.0f) * GuiScale;

        // v1.56.284: live panel geometry for the PANEL_WIDTH/MAX_BODY_HEIGHT
        // macros (KxPanelsWidth/KxPanelsHeight when UI settings are on).
        g_PanelW = (g_Config.m_KxUiSettingsOn != 0) ? (float)g_Config.m_KxPanelsWidth : 300.0f;

        // v1.56.284: smooth scrolling — the stored scroll approaches the
        // target exponentially each frame (frame-rate independent inertia).
        {
                const float ScrollDt = Client()->RenderFrameTime();
                const float ScrollFactor = 1.0f - std::exp(-14.0f * ScrollDt);
                for(int i = 0; i < NUM_PANELS; ++i)
                {
                        const float ScrollDelta = m_aPanelScrollTarget[i] - m_aPanelScrollStored[i];
                        if(ScrollDelta * ScrollDelta > 0.25f)
                                m_aPanelScrollStored[i] += ScrollDelta * ScrollFactor;
                        else
                                m_aPanelScrollStored[i] = m_aPanelScrollTarget[i];
                }
        }
        g_PanelMaxBodyH = (g_Config.m_KxUiSettingsOn != 0) ? (float)g_Config.m_KxPanelsHeight : 600.0f;
        // Keep the panel (and its resize edge) inside the screen.
        g_PanelMaxBodyH = std::min(g_PanelMaxBodyH, std::max(200.0f, VScreenHeight() - 120.0f));

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
                // Rows are initialized lazily; with the early-out at the top of
                // OnRender the per-frame sync at the end of the function no longer
                // runs while closed, so make sure the Dummy Manager rows exist
                // before the visibility pass below dereferences them.
                UpdateDummyManagerRows();

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
                // (So if kx_helpers / kx_random_aim / kx_show_trajectory were enabled
                // via console before opening ClickGUI, the checkboxes reflect that.)
                for(int p = 0; p < NUM_PANELS; ++p)
                {
                        for(int r = 0; r < g_Panels[p].rowCount; ++r)
                        {
                                SRow &row = g_Panels[p].pRows[r];
                                if(str_comp(row.pName, "Helpers") == 0)
                                        row.on = g_Config.m_KxHelpers;
                                else if(str_comp(row.pName, "Trajectory") == 0)
                                        row.on = g_Config.m_KxShowTrajectory != 0;
                                // Advanced → Laser unfreeze expandable is master toggle.
                                else if(str_comp(row.pName, "Laser unfreeze") == 0)
                                        row.on = g_Config.m_KxLaserUnfreeze != 0;
				else if(str_comp(row.pName, "Fake Aim") == 0 && str_comp(g_Panels[p].pTitle, "Functions") == 0)
					row.on = g_Config.m_KxFakeAim != 0;
                                else if(str_comp(row.pName, "ESP") == 0)
                                        row.on = g_Config.m_KxEsp != 0;
                                else if(str_comp(row.pName, "Avoid Freeze") == 0)
                                        row.on = g_Config.m_KxAvoidFreeze != 0;
                                // Functions panel — Fly/Copy Moves sub-section
                                else if(str_comp(row.pName, "Preserve input") == 0)
                                        row.on = g_Config.m_KxDummyHammerKeepInputs != 0;
                                else if(str_comp(row.pName, "Custom latency") == 0)
                                        row.on = g_Config.m_KxDummyHammerCustomLatency != 0;
                                else if(str_comp(row.pName, "Auto fly") == 0)
                                        row.on = g_Config.m_KxDummyHammerAuto != 0;
                                else if(str_comp(row.pName, "Fix Dummy Hammer") == 0)
                                        row.on = g_Config.m_KxFixDummyHammer != 0;
                                else if(str_comp(row.pName, "Auto Triple Fly") == 0)
                                        row.on = g_Config.m_KxTripleFly != 0;
                                // v1.56.262: Visuals bypasses + Admin Alarm + Functions headers.
                                else if(str_comp(row.pName, "Bypass Zoom Lock") == 0)
                                        row.on = g_Config.m_KxZoomUnlock != 0;
                                else if(str_comp(row.pName, "Bypass Asset Lock") == 0)
                                        row.on = g_Config.m_KxAssetUnlock != 0;
                                else if(str_comp(row.pName, "Admin Alarm") == 0)
                                        row.on = g_Config.m_KxAdminAlarm != 0;
                                else if(str_comp(row.pName, "Auto Fire") == 0)
                                        row.on = g_Config.m_KxAutoFire != 0;
                                else if(str_comp(row.pName, "BalanceBot") == 0)
                                        row.on = g_Config.m_KxBalanceBot != 0;
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
                                // v6.7: Smart Dummy Switch expandable is master toggle.
                                else if(str_comp(row.pName, "Smart dummy switch") == 0)
                                        row.on = g_Config.m_KxSdsEnabled != 0;
                                // Pathfinder top-level sliders/toggles
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
                                // v1.56.280: Real-Time Pathfinder top-level rows.
                                // v1.56.281: "Reconstruct" expandable header checkbox is the
                                // master toggle (kx_rt_pf_reconstruct); the trigger slider
                                // lives inside it. "Future Limit" is also a master toggle (kx_rt_pf_future_limit).
                                else if(str_comp(row.pName, "Reconstruct") == 0)
                                {
                                        row.on = g_Config.m_KxRtPfReconstruct != 0;
                                        row.pChildren[0].value = (float)g_Config.m_KxRtPfTrigger;
                                }
                                else if(str_comp(row.pName, "Future Limit") == 0)
                                {
                                        row.on = g_Config.m_KxRtPfFutureLimit != 0;
                                        row.pChildren[0].value = (float)g_Config.m_KxRtPfFutureTicks;
                                }
                                // Settings → IRC/Spoofer top-level expandables are master toggles.
                                else if(str_comp(row.pName, "IRC") == 0)
                                        row.on = g_Config.m_KxIrcEnabled != 0;
                                else if(str_comp(row.pName, "Spoofer") == 0)
                                        row.on = (g_Config.m_KxVersionSpoof != 0 || g_Config.m_KxSpoofClientStr != 0);
                                else if(str_comp(row.pName, "Spoof Zoom") == 0)
                                        row.on = g_Config.m_KxSpoofZoom != 0;
                                else if(str_comp(row.pName, "Spoof Spectate") == 0)
                                        row.on = g_Config.m_KxSpoofSpectate != 0;
                                // v1.56.257: Hitboxes expandable header = master toggle.
                                else if(str_comp(row.pName, "Hitboxes") == 0)
                                        row.on = g_Config.m_KxHitboxes != 0;
                                // v1.56.199: Binds panel sync moved to every-frame loop (see below).
                                // Sync children of expandable rows
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                        // v6.7: Smart Copy Moves children (index-keyed: Sort Type,
                                        // Apply Mode, Apply Dummies slider, filters + inputs).
                                        // First in the chain: no earlier branch matches these rows.
                                        if(str_comp(row.pName, "Smart Copy Moves") == 0)
                                        {
                                                switch(c)
                                                {
                                                case 0: child.valueIdx = g_Config.m_KxSmartCopySort; break;
                                                case 1: child.valueIdx = g_Config.m_KxSmartCopyApply; break;
                                                case 2:
                                                {
                                                        // Slider max follows the Dummy Manager limit (incl. main).
                                                        const int MaxCnt = g_DummyLimit >= 2 ? g_DummyLimit - 1 : 1;
                                                        child.max = (float)MaxCnt;
                                                        int Cnt = g_Config.m_KxSmartCopyCount;
                                                        if(Cnt < 1)
                                                                Cnt = 1;
                                                        if(Cnt > MaxCnt)
                                                                Cnt = MaxCnt;
                                                        child.value = (float)Cnt;
                                                        break;
                                                }
                                                case 3: child.valueIdx = g_Config.m_KxSmartCopyTeam; break;
                                                case 4: child.valueIdx = g_Config.m_KxSmartCopyDdraceTeam; break;
                                                case 5:
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_format(child.m_aInputBuf, sizeof(child.m_aInputBuf), "%d", g_Config.m_KxSmartCopyDdraceTeamId);
                                                                child.m_InputInitialized = true;
                                                        }
                                                        break;
                                                case 6: child.valueIdx = g_Config.m_KxSmartCopyFriend; break;
                                                case 7: child.valueIdx = g_Config.m_KxSmartCopyClanmate; break;
                                                case 8: child.valueIdx = g_Config.m_KxSmartCopyFreeze; break;
                                                case 9: child.valueIdx = g_Config.m_KxSmartCopyTargetsMode; break;
                                                case 10:
                                                        child.pName = (g_Config.m_KxSmartCopyTargetsMode == 2) ? "Untargets IDs" : "Targets IDs";
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(child.m_aInputBuf, g_Config.m_KxSmartCopyTargets, sizeof(child.m_aInputBuf));
                                                                child.m_InputInitialized = true;
                                                        }
                                                        break;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Latency (ms)") == 0 && str_comp(row.pName, "Custom latency") == 0)
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
                                        else if(str_comp(child.pName, "Freeze") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfAvoidFreeze != 0;
                                        else if(str_comp(child.pName, "Teleport") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfAvoidTeleport != 0;
                                        else if(str_comp(child.pName, "Death") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfAvoidDeath != 0;
                                        else if(str_comp(child.pName, "Direction") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfDirection != 0;
                                        else if(str_comp(child.pName, "Jump") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfJump != 0;
                                        else if(str_comp(child.pName, "Hook") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfHook != 0;
                                        else if(str_comp(child.pName, "Aim") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfAim != 0;
                                        else if(str_comp(child.pName, "FOV") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxAfFov;
                                        else if(str_comp(child.pName, "Angles") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxAfAngles;
                                        else if(str_comp(child.pName, "Silent") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.on = g_Config.m_KxAfSilent != 0;
                                        else if(str_comp(child.pName, "Ticks") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxAfTicks;
                                        else if(str_comp(child.pName, "Ticks Before") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                                child.value = (float)g_Config.m_KxAfTicksBefore;
                                        // Fake Aim children
					else if(str_comp(child.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0 && str_comp(g_Panels[p].pTitle, "Functions") == 0)
						child.valueIdx = g_Config.m_KxFakeAimMode;
                                        else if(str_comp(child.pName, "Speed") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.value = (float)g_Config.m_KxFakeAimSpeed;
                                        else if(str_comp(child.pName, "Jitter") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.value = (float)g_Config.m_KxFakeAimJitter;
                                        else if(str_comp(child.pName, "Distance from center") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.value = (float)g_Config.m_KxFakeAimDistCenter;
                                        else if(str_comp(child.pName, "Show for me") == 0)
                                                child.on = g_Config.m_KxFakeAimShowForMe != 0;
                                        // v1.56.263: Admin Alarm children — per-level, like AimBot
                                        // per-weapon: the Level dropdown selects which level's
                                        // settings are being edited.
                                        else if(str_comp(child.pName, "Admin level") == 0 && str_comp(row.pName, "Admin Alarm") == 0)
                                                child.valueIdx = g_Config.m_KxAdminAlarmLevel;
                                        else if(str_comp(child.pName, "Message") == 0 && str_comp(row.pName, "Admin Alarm") == 0)
                                        {
                                                const char *pAlarmMsg = (g_Config.m_KxAdminAlarmLevel == 0) ? g_Config.m_KxAdminAlarmMsg1 :
                                                        (g_Config.m_KxAdminAlarmLevel == 1) ? g_Config.m_KxAdminAlarmMsg2 : g_Config.m_KxAdminAlarmMsg3;
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, pAlarmMsg, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Repeat") == 0 && str_comp(row.pName, "Admin Alarm") == 0)
                                                child.on = (g_Config.m_KxAdminAlarmLevel == 0) ? g_Config.m_KxAdminAlarmRep1 != 0 :
                                                        (g_Config.m_KxAdminAlarmLevel == 1) ? g_Config.m_KxAdminAlarmRep2 != 0 : g_Config.m_KxAdminAlarmRep3 != 0;
                                        else if(str_comp(child.pName, "Every ms") == 0 && str_comp(row.pName, "Admin Alarm") == 0)
                                                child.value = (float)((g_Config.m_KxAdminAlarmLevel == 0) ? g_Config.m_KxAdminAlarmMs1 :
                                                        (g_Config.m_KxAdminAlarmLevel == 1) ? g_Config.m_KxAdminAlarmMs2 : g_Config.m_KxAdminAlarmMs3);
                                        // v1.56.262: Fake Aim — Show Server-Side Aim.
                                        else if(str_comp(child.pName, "Show Server-Side Aim") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.on = g_Config.m_KxFakeAimShowServerSide != 0;
                                        // v1.56.263: Fake Aim — Predict.
                                        else if(str_comp(child.pName, "Predict") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                                                child.on = g_Config.m_KxFakeAimPredict != 0;
                                        // v1.56.262: Auto Fire + BalanceBot children.
                                        else if(str_comp(child.pName, "Delay") == 0 && str_comp(row.pName, "Auto Fire") == 0)
                                                child.value = (float)g_Config.m_KxAutoFireDelay;
                                        else if(str_comp(child.pName, "Vertical Filter") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBalanceBotVertical;
                                        else if(str_comp(child.pName, "Radius Filter") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.value = (float)g_Config.m_KxBalanceBotRadius;
                                        else if(str_comp(child.pName, "Stand Radius") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.value = (float)g_Config.m_KxBalanceBotStandRadius;
                                        else if(str_comp(child.pName, "Team") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbTeam;
                                        // v1.56.275: BalanceBot DDrace Team + Target ID.
                                        else if(str_comp(child.pName, "DDrace Team") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbDDraceTeam;
                                        else if(str_comp(child.pName, "Target ID") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_format(child.m_aInputBuf, sizeof(child.m_aInputBuf), "%d", g_Config.m_KxBbDDraceTeamId);
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Friend") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbFriend;
                                        else if(str_comp(child.pName, "Dummy") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbDummy;
                                        else if(str_comp(child.pName, "Clanmate") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbClanmate;
                                        else if(str_comp(child.pName, "Freeze") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbFreeze;
                                        else if(str_comp(child.pName, "Off while freeze") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.on = g_Config.m_KxBbOffWhileFreeze != 0;
                                        else if(str_comp(child.pName, "FNG Freeze") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.on = g_Config.m_KxBbFngFreeze != 0;
                                        else if(str_comp(child.pName, "Targets") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                child.valueIdx = g_Config.m_KxBbTargetsFilter;
                                        else if((str_comp(child.pName, "Targets IDs") == 0 || str_comp(child.pName, "Untargets IDs") == 0) && str_comp(row.pName, "BalanceBot") == 0)
                                        {
                                                child.pName = (g_Config.m_KxBbTargetsFilter == 2) ? "Untargets IDs" : "Targets IDs";
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxBbTargetsIds, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        // v1.56.254: ESP children — renamed (Pointer Style/Speed, unprefixed
                                        // filters, new Box Style/Box Speed). Renamed branches are parent-gated
                                        // to "ESP" since the generic names now collide with other panels.
                                        else if(str_comp(child.pName, "Team") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspTeamFilter;
                                        // v1.56.275: ESP DDrace Team + Target ID.
                                        else if(str_comp(child.pName, "DDrace Team") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspDDraceTeam;
                                        else if(str_comp(child.pName, "Target ID") == 0 && str_comp(row.pName, "ESP") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_format(child.m_aInputBuf, sizeof(child.m_aInputBuf), "%d", g_Config.m_KxEspDDraceTeamId);
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Friend") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspFriendFilter;
                                        else if(str_comp(child.pName, "Dummy") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspDummyFilter;
                                        else if(str_comp(child.pName, "Clanmate") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspClanmateFilter;
                                        else if(str_comp(child.pName, "Targets") == 0 && str_comp(row.pName, "ESP") == 0)
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
                                        else if(str_comp(child.pName, "Freeze") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspFreezeFilter;
                                        else if(str_comp(child.pName, "FNG Freeze") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.on = g_Config.m_KxEspFngFreeze != 0;
                                        else if(str_comp(child.pName, "Mode") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspMode;
                                        else if(str_comp(child.pName, "Pointer Style") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspStyle;
                                        else if(str_comp(child.pName, "Pointer Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.value = (float)g_Config.m_KxEspSpeed;
                                        else if(str_comp(child.pName, "Box Style") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.valueIdx = g_Config.m_KxEspBoxStyle;
                                        else if(str_comp(child.pName, "Box Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.value = (float)g_Config.m_KxEspBoxSpeed;
                                        // v1.56.256: ESP pointer/box toggles + box scale sliders.
                                        else if(str_comp(child.pName, "Pointer") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.on = g_Config.m_KxEspPointer != 0;
                                        else if(str_comp(child.pName, "Box") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.on = g_Config.m_KxEspBox != 0;
                                        else if(str_comp(child.pName, "X Box scale") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.value = (float)g_Config.m_KxEspBoxScaleX;
                                        else if(str_comp(child.pName, "Y Box scale") == 0 && str_comp(row.pName, "ESP") == 0)
                                                child.value = (float)g_Config.m_KxEspBoxScaleY;
                                        // v1.56.256: Hitboxes children — the duplicated "Hitbox Style"/
                                        // "Box Speed" rows are disambiguated by child index.
                                        else if(str_comp(row.pName, "Hitboxes") == 0)
                                        {
                                                switch(c)
                                                {
                                                case 0: child.on = g_Config.m_KxHitboxesTeeTile != 0; break;
                                                case 1: child.valueIdx = g_Config.m_KxHitboxesTileStyle; break;
                                                case 2: child.value = (float)g_Config.m_KxHitboxesTileSpeed; break;
                                                case 3: child.on = g_Config.m_KxHitboxesTeeTrigger != 0; break;
                                                case 4: child.valueIdx = g_Config.m_KxHitboxesTriggerStyle; break;
                                                case 5: child.value = (float)g_Config.m_KxHitboxesTriggerSpeed; break;
                                                case 6: child.on = g_Config.m_KxHitboxesShowOthers != 0; break;
                                                // v1.56.275: DDrace Team + Target ID + new Team filter.
                                                case 7: child.valueIdx = g_Config.m_KxHitboxesDDraceTeam; break;
                                                case 8:
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_format(child.m_aInputBuf, sizeof(child.m_aInputBuf), "%d", g_Config.m_KxHitboxesDDraceTeamId);
                                                                child.m_InputInitialized = true;
                                                        }
                                                        break;
                                                case 9: child.valueIdx = g_Config.m_KxHitboxesTeamFilter; break;
                                                case 10: child.valueIdx = g_Config.m_KxHitboxesFriendFilter; break;
                                                case 11: child.valueIdx = g_Config.m_KxHitboxesDummyFilter; break;
                                                case 12: child.valueIdx = g_Config.m_KxHitboxesClanmateFilter; break;
                                                case 13: child.valueIdx = g_Config.m_KxHitboxesFreezeFilter; break;
                                                // v1.56.275: FNG Freeze.
                                                case 14: child.on = g_Config.m_KxHitboxesFngFreeze != 0; break;
                                                case 15: child.valueIdx = g_Config.m_KxHitboxesTargetsFilterMode; break;
                                                case 16:
                                                        child.pName = (g_Config.m_KxHitboxesTargetsFilterMode == 2) ? "Untargets IDs" : "Targets IDs";
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(child.m_aInputBuf, g_Config.m_KxHitboxesTargetsFilter, sizeof(child.m_aInputBuf));
                                                                child.m_InputInitialized = true;
                                                        }
                                                        break;
                                                }
                                        }
                                        else if(str_comp(child.pName, "X") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                                {
                                                                                        child.value = (float)g_Config.m_KxEspScreenX;
                                                                                        child.max = (float)Graphics()->ScreenWidth();
                                                                                }
                                        else if(str_comp(child.pName, "Y") == 0 && str_comp(row.pName, "ESP") == 0)
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
                                        // v1.56.273: Spoof Zoom / Spoof Spectate children.
                                        else if(str_comp(child.pName, "Fake Zoom") == 0)
                                                child.value = g_Config.m_KxSpoofZoomValue / 10.0f;
                                        else if(str_comp(child.pName, "Mode") == 0 && str_comp(row.pName, "Spoof Spectate") == 0)
                                                child.valueIdx = g_Config.m_KxSpoofSpectateMode;
                                        else if(str_comp(child.pName, "Fake spectate X") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char aBuf[16];
                                                        str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_KxSpoofSpectateX);
                                                        str_copy(child.m_aInputBuf, aBuf, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Fake spectate Y") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char aBuf[16];
                                                        str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_KxSpoofSpectateY);
                                                        str_copy(child.m_aInputBuf, aBuf, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Fake Follow ID") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char aBuf[16];
                                                        str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_KxSpoofSpectateFollowId);
                                                        str_copy(child.m_aInputBuf, aBuf, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        // Pathfinder → Visuals children
                                        else if(str_comp(child.pName, "Show field") == 0)
                                                child.on = g_Config.m_KxPfShowField != 0;
                                        else if(str_comp(child.pName, "Show hooks") == 0)
                                                child.on = g_Config.m_KxPfShowHooks != 0;
                                        else if(str_comp(child.pName, "Show branches") == 0)
                                                child.on = g_Config.m_KxPfShowBranches != 0;
                                        else if(str_comp(child.pName, "Show speed") == 0)
                                                child.on = g_Config.m_KxPfShowSpeed != 0;
                                        // v1.56.280: Real-Time Pathfinder Visuals children
                                        // (panel-gated: TAS/Pathfinder use overlapping child labels)
                                        else if(str_comp(row.pName, "Visuals") == 0 && str_comp(g_Panels[p].pTitle, "Real-Time Pathfinder") == 0)
                                        {
                                                if(str_comp(child.pName, "Show Path") == 0)
                                                        child.on = g_Config.m_KxRtPfShowPath != 0;
                                                else if(str_comp(child.pName, "Show Hooks") == 0)
                                                        child.on = g_Config.m_KxRtPfShowHooks != 0;
                                                else if(str_comp(child.pName, "Show Field") == 0)
                                                        child.on = g_Config.m_KxRtPfShowField != 0;
                                                else if(str_comp(child.pName, "Show Previous Path") == 0)
                                                        child.on = g_Config.m_KxRtPfShowPrevPath != 0;
                                                else if(str_comp(child.pName, "Show Speed") == 0)
                                                        child.on = g_Config.m_KxRtPfShowSpeed != 0;
                                        }
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
                                        auto &s = (w == CAimBot::NUM_WEAPONS_AIM - 1) ? aimbot.AimBotHookSettings(aimbot.m_AimBotSelectedEntity) : aimbot.m_AimBotWeapons[w];
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Weapon") == 0)
                                                        ch.valueIdx = w;
                                                else if(str_comp(ch.pName, "Entity Mode") == 0)
                                                        ch.valueIdx = aimbot.m_AimBotSelectedEntity;
                                                else if(str_comp(ch.pName, "Enable") == 0)
                                                        ch.on = s.m_Enabled;
                                                else if(str_comp(ch.pName, "FOV") == 0)
                                                        ch.value = s.m_Fov;
                                                else if(str_comp(ch.pName, "Radius") == 0)
                                                {
                                                        // v1.56.297: hook TILE blocks only reach the last FULL
                                                        // hook step (≈362) — the clamped tip never attaches to
                                                        // a tile. The Tee block (players) keeps the full hook
                                                        // length (380 + grab radius).
                                                        const bool HookTile = w == CAimBot::NUM_WEAPONS_AIM - 1 && aimbot.m_AimBotSelectedEntity != 0;
                                                        ch.max = HookTile ? CAimBot::HOOK_TILE_REACH : aimbot.GetWeaponRadius(w);
                                                        // Keep old configs (380) inside the real tile reach.
                                                        if(HookTile && s.m_Radius > ch.max)
                                                                s.m_Radius = ch.max;
                                                        ch.value = s.m_Radius;
                                                }
                                                else if(str_comp(ch.pName, "Show FOV") == 0)
                                                        ch.on = aimbot.m_AimBotShowFov;
                                                else if(str_comp(ch.pName, "Show Radius") == 0)
                                                        ch.on = aimbot.m_AimBotShowRadius;
                                                else if(str_comp(ch.pName, "Auto Switch Weapon") == 0)
                                                        ch.on = s.m_AutoSwitchWeapon != 0;
                                                else if(str_comp(ch.pName, "Use raycast (Show)") == 0)
                                                        ch.on = s.m_UseRaycastShow;
                                                else if(str_comp(ch.pName, "Rules") == 0)
                                                        ch.valueIdx = s.m_Rules;
                                                else if(str_comp(ch.pName, "Use raycast angle (AB)") == 0)
                                                        ch.on = s.m_UseAngle;
                                                else if(str_comp(ch.pName, "Silent") == 0)
                                                        ch.on = s.m_Silent;
                                                else if(str_comp(ch.pName, "Mode") == 0)
                                                {
                                                        // v1.56.262: while Silent is on the dropdown hides the
                                                        // "Trigger" option — swap the option list and remap.
                                                        if(s.m_Silent)
                                                        {
                                                                ch.pOptions = g_ABModeOptsNoTrigger;
                                                                ch.optionCount = 3;
                                                                ch.valueIdx = (s.m_AimMode == 2) ? 1 : (s.m_AimMode == 3) ? 2 : 0;
                                                        }
                                                        else
                                                        {
                                                                ch.pOptions = g_ABModeOpts;
                                                                ch.optionCount = 4;
                                                                ch.valueIdx = s.m_AimMode;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Smooth Speed") == 0)
                                                        ch.value = s.m_SmoothSpeed;
                                                else if(str_comp(ch.pName, "Center snap") == 0)
                                                        ch.on = s.m_SmoothCenterSnap;
                                                else if(str_comp(ch.pName, "Team") == 0)
                                                        ch.valueIdx = s.m_TeamFilter;
                                                else if(str_comp(ch.pName, "Friend") == 0)
                                                        ch.valueIdx = s.m_FriendFilter;
                                                else if(str_comp(ch.pName, "Predict") == 0)
                                                        ch.on = s.m_Predict;
                                                else if(str_comp(ch.pName, "Jitter") == 0)
                                                        ch.value = s.m_Jitter;
                                                else if(str_comp(ch.pName, "Dummy") == 0)
                                                        ch.valueIdx = s.m_DummyFilter;
                                                else if(str_comp(ch.pName, "Clanmate") == 0)
                                                        ch.valueIdx = s.m_ClanmateFilter;
                                                else if(str_comp(ch.pName, "Freeze") == 0)
                                                        ch.valueIdx = s.m_FreezeFilter;
                                                else if(str_comp(ch.pName, "Off while freeze") == 0)
                                                        ch.on = s.m_OffWhileFreeze != 0;
                                                else if(str_comp(ch.pName, "FNG Freeze") == 0)
                                                        ch.on = s.m_FngFreeze != 0;
                                                else if(str_comp(ch.pName, "Priority") == 0)
                                                        ch.valueIdx = s.m_Priority;
                                                else if(str_comp(ch.pName, "Targets") == 0)
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
                                        auto &s = (w == CAimBot::NUM_WEAPONS_TRIGGER - 1) ? aimbot.TriggerBotHookSettings(aimbot.m_TriggerBotSelectedEntity) : aimbot.m_TriggerBotWeapons[w];
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Weapon") == 0)
                                                        ch.valueIdx = w;
                                                else if(str_comp(ch.pName, "Enable") == 0)
                                                        ch.on = s.m_Enabled;
                                                else if(str_comp(ch.pName, "Entity Mode") == 0)
                                                        ch.valueIdx = aimbot.m_TriggerBotSelectedEntity;
                                                else if(str_comp(ch.pName, "Trigger mode") == 0)
                                                        ch.valueIdx = s.m_TriggerMode;
                                                else if(str_comp(ch.pName, "FOV") == 0)
                                                        ch.value = s.m_Fov;
                                                else if(str_comp(ch.pName, "Radius") == 0)
                                                {
                                                        // v1.56.297: same tile reach accounting as the AimBot
                                                        // hook block — tiles attach only up to the last full step.
                                                        const bool HookTile = w == CAimBot::NUM_WEAPONS_TRIGGER - 1 && aimbot.m_TriggerBotSelectedEntity != 0;
                                                        ch.max = HookTile ? CAimBot::HOOK_TILE_REACH : aimbot.GetWeaponRadius(w);
                                                        if(HookTile && s.m_Radius > ch.max)
                                                                s.m_Radius = ch.max;
                                                        ch.value = s.m_Radius;
                                                }
                                                else if(str_comp(ch.pName, "Show FOV") == 0)
                                                        ch.on = aimbot.m_TriggerBotShowFov;
                                                else if(str_comp(ch.pName, "Show Radius") == 0)
                                                        ch.on = aimbot.m_TriggerBotShowRadius;
                                                else if(str_comp(ch.pName, "Auto Switch Weapon") == 0)
                                                        ch.on = s.m_AutoSwitchWeapon != 0;
                                                else if(str_comp(ch.pName, "Use raycast (Show)") == 0)
                                                        ch.on = s.m_UseRaycastShow;
                                                else if(str_comp(ch.pName, "Rules") == 0)
                                                        ch.valueIdx = s.m_Rules;
                                                else if(str_comp(ch.pName, "Latency") == 0)
                                                        ch.value = (float)s.m_RandomLatency;
                                                else if(str_comp(ch.pName, "Team") == 0)
                                                        ch.valueIdx = s.m_TeamFilter;
                                                else if(str_comp(ch.pName, "Friend") == 0)
                                                        ch.valueIdx = s.m_FriendFilter;
                                                else if(str_comp(ch.pName, "Dummy") == 0)
                                                        ch.valueIdx = s.m_DummyFilter;
                                                else if(str_comp(ch.pName, "Clanmate") == 0)
                                                        ch.valueIdx = s.m_ClanmateFilter;
                                                else if(str_comp(ch.pName, "Freeze") == 0)
                                                        ch.valueIdx = s.m_FreezeFilter;
                                                else if(str_comp(ch.pName, "Off while freeze") == 0)
                                                        ch.on = s.m_OffWhileFreeze != 0;
                                                else if(str_comp(ch.pName, "Use silent channel AB") == 0)
                                                        ch.on = s.m_UseSilentChannelAB != 0;
                                                else if(str_comp(ch.pName, "FNG Freeze") == 0)
                                                        ch.on = s.m_FngFreeze != 0;
                                                else if(str_comp(ch.pName, "Priority") == 0)
                                                        ch.valueIdx = s.m_Priority;
                                                else if(str_comp(ch.pName, "Targets") == 0)
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
                                else if(str_comp(row.pName, "Spoof Zoom") == 0)
                                {
                                        row.on = g_Config.m_KxSpoofZoom != 0;
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Fake Zoom") == 0)
                                                        ch.value = g_Config.m_KxSpoofZoomValue / 10.0f;
                                        }
                                }
                                else if(str_comp(row.pName, "Spoof Spectate") == 0)
                                {
                                        row.on = g_Config.m_KxSpoofSpectate != 0;
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Mode") == 0)
                                                        ch.valueIdx = g_Config.m_KxSpoofSpectateMode;
                                                else if(str_comp(ch.pName, "Fake spectate X") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char aBuf[16];
                                                                str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_KxSpoofSpectateX);
                                                                str_copy(ch.m_aInputBuf, aBuf, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Fake spectate Y") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char aBuf[16];
                                                                str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_KxSpoofSpectateY);
                                                                str_copy(ch.m_aInputBuf, aBuf, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Fake Follow ID") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char aBuf[16];
                                                                str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_KxSpoofSpectateFollowId);
                                                                str_copy(ch.m_aInputBuf, aBuf, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
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
                                if(str_comp(row.pName, "Helpers") == 0)
                                {
                                        row.on = g_Config.m_KxHelpers;
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &ch = row.pChildren[c];
                                                if(str_comp(ch.pName, "Auto Main") == 0) ch.on = g_Config.m_KxAutoMain;
                                                else if(str_comp(ch.pName, "Main ID") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char b[16];
                                                                // v1.56.259: -1 = no main → empty field
                                                                if(g_Config.m_KxMain >= 0)
                                                                        str_format(b, sizeof(b), "%d", g_Config.m_KxMain);
                                                                else
                                                                        b[0] = '\0';
                                                                str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "All Target") == 0) ch.on = g_Config.m_KxTargetAll;
                                                else if(str_comp(ch.pName, "Target IDs") == 0 || str_comp(ch.pName, "Untarget IDs") == 0)
                                                {
                                                        // v1.56.258: dynamic name — All Target turns the list into exclusions.
                                                        ch.pName = (g_Config.m_KxTargetAll != 0) ? "Untarget IDs" : "Target IDs";
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, bn.m_aTargetIDsStr, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Rescue") == 0) ch.on = g_Config.m_KxRescue;
                                                else if(str_comp(ch.pName, "All Rescue") == 0) ch.on = g_Config.m_KxRescueAll;
                                                else if(str_comp(ch.pName, "Rescue IDs") == 0 || str_comp(ch.pName, "Unrescue IDs") == 0)
                                                {
                                                        ch.pName = (g_Config.m_KxRescueAll != 0) ? "Unrescue IDs" : "Rescue IDs";
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, bn.m_aRescueIDsStr, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Dummy IDs") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                str_copy(ch.m_aInputBuf, bn.m_aHelpersDummyIDsStr, sizeof(ch.m_aInputBuf));
                                                                ch.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(ch.pName, "Fire Dist") == 0) ch.value = (float)g_Config.m_KxFireDist;
                                                else if(str_comp(ch.pName, "Hook Dist") == 0) ch.value = (float)g_Config.m_KxHookDist;
                                                else if(str_comp(ch.pName, "Rescue Dist") == 0)
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
                                                else if(str_comp(ch.pName, "MStand Dist") == 0)
                                                                                        {
                                                                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                                                                {
                                                                                                        char b[16]; str_format(b, sizeof(b), "%d", (int)g_Config.m_KxMainStandDist);
                                                                                                        str_copy(ch.m_aInputBuf, b, sizeof(ch.m_aInputBuf));
                                                                                                        ch.m_InputInitialized = true;
                                                                                                }
                                                                                        }
                                                // v1.56.258: legacy attack toggles removed — movement is handled by the Block pathfinder.
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
                                                else if(ch.type != ERowType::DoubleButton && ch.pName && ch.pName[0] == 'D' && DummyMgrParseD(ch.pName) >= 1)
                                                {
                                                        const int d = DummyMgrParseD(ch.pName);
                                                        if(strstr(ch.pName, "Connect"))
                                                                ch.on = GameClient()->Client()->DummyConnected(d);
                                                        else if(strstr(ch.pName, "Switch"))
                                                                ch.on = (g_Config.m_ClDummy == d);
                                                }
                                                // v1.56.282: DoubleButton (Connect+Switch+Send) — ch.on=connected, ch.value=active, ch.expanded=send
                                                else if(ch.type == ERowType::DoubleButton && ch.pName && ch.pName[0] == 'D')
                                                {
                                                        const int d = DummyMgrParseD(ch.pName);
                                                        if(d >= 1 && d < MAX_DUMMIES)
                                                        {
                                                                ch.on = GameClient()->Client()->DummyConnected(d);
                                                                ch.value = (g_Config.m_ClDummy == d) ? 1.0f : 0.0f;
                                                                ch.expanded = m_aDummySendEnabled[d];
                                                        }
                                                }
                                        }
                                }
                        }
                }
        }
        // v1.56.283: every-frame sync of the combo expandables. The former
        // top-level rows (Main/Panel Color, BG blackout, ClickGUI Scale, Panels
        // Start X/Y, Drag-To-Sort, per-sprite Custom color *) live as children
        // now, so they are synced inside their parent's branch below.
        for(int p = 0; p < NUM_PANELS; ++p)
        {
                for(int r = 0; r < g_Panels[p].rowCount; ++r)
                {
                        SRow &row = g_Panels[p].pRows[r];
                        if(str_comp(row.pName, "ClickGUI Settings") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxUiSettingsOn != 0;
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const bool Dragging = (m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c);
                                        if(str_comp(child.pName, "Main Color") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char aHex[8];
                                                        str_format(aHex, sizeof(aHex), "%06x", m_AccentColorRGBA & 0xffffff);
                                                        str_copy(child.m_aInputBuf, aHex, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Rainbow Main Color") == 0)
                                                child.on = m_RainbowEnabled;
                                        else if(str_comp(child.pName, "Speed") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = m_RainbowSpeed;
                                        }
                                        else if(str_comp(child.pName, "Panel Color") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        char aHex[8];
                                                        str_format(aHex, sizeof(aHex), "%06x", m_PanelBgRGBA & 0xffffff);
                                                        str_copy(child.m_aInputBuf, aHex, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "BG Blackout") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)m_BgBlackout;
                                        }
                                        else if(str_comp(child.pName, "Main Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = m_MainOpacity * 100.0f;
                                        }
                                        else if(str_comp(child.pName, "Panel Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = m_PanelOpacity * 100.0f;
                                        }
                                        else if(str_comp(child.pName, "ClickGUI Scale") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxClickGuiScale / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Panels Start X") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPanelsStartX;
                                        }
                                        else if(str_comp(child.pName, "Panels Start Y") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPanelsStartY;
                                        }
                                        else if(str_comp(child.pName, "Panels X-indentation") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPanelsIndentX;
                                        }
                                        else if(str_comp(child.pName, "Panels Y-indentation") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPanelsIndentY;
                                        }
                                        else if(str_comp(child.pName, "Dynamic scale (Beta)") == 0)
                                                child.on = g_Config.m_KxPanelsDynamicScale != 0;
                                        else if(str_comp(child.pName, "Panels width") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPanelsWidth;
                                        }
                                        else if(str_comp(child.pName, "Panels height") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPanelsHeight;
                                        }
                                        else if(str_comp(child.pName, "Drag-To-Sort") == 0)
                                                child.on = g_Config.m_KxDragToSort != 0;
                                        // v1.56.287: BG Blur — toggle + radius (config-synced).
                                        else if(str_comp(child.pName, "BG Blur") == 0)
                                                child.on = g_Config.m_KxUiBgBlur != 0;
                                        else if(str_comp(child.pName, "Blur Radius") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxUiBgBlurRadius;
                                        }
                                        // v6.3: ToolTips master + per-part opacity.
                                        else if(str_comp(child.pName, "Show ToolTips") == 0)
                                                child.on = g_Config.m_KxUiShowTooltips != 0;
                                        else if(str_comp(child.pName, "ToolTips Text Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxUiTooltipTextOpacity;
                                        }
                                        else if(str_comp(child.pName, "ToolTips Main Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxUiTooltipMainOpacity;
                                        }
                                        else if(str_comp(child.pName, "ToolTips Panel Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxUiTooltipPanelOpacity;
                                        }
                                }
                        }
                        // v6.3: ArrayList HUD settings.
                        else if(str_comp(row.pName, "ArrayList") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxArrayListEnabled != 0;
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const bool Dragging = (m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c);
                                        if(str_comp(child.pName, "X Anchor") == 0)
                                                child.valueIdx = g_Config.m_KxArrayListXAnchor;
                                        else if(str_comp(child.pName, "Y Anchor") == 0)
                                                child.valueIdx = g_Config.m_KxArrayListYAnchor;
                                        else if(str_comp(child.pName, "Sort Type") == 0)
                                                child.valueIdx = g_Config.m_KxArrayListSort;
                                        else if(str_comp(child.pName, "Style") == 0)
                                                child.valueIdx = g_Config.m_KxArrayListStyle;
                                        else if(str_comp(child.pName, "Indentation") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayListIndent;
                                        }
                                        else if(str_comp(child.pName, "ArrayList Size") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxArrayListSize / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Text Color") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxArrayListTextColor, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Rainbow Text") == 0)
                                                child.on = g_Config.m_KxArrayListRainbow != 0;
                                        else if(str_comp(child.pName, "Rainbow Mode") == 0)
                                                child.valueIdx = g_Config.m_KxArrayListRainbowMode;
                                        else if(str_comp(child.pName, "Speed") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxArrayListRainbowSpeed / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Step") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayListRainbowStep;
                                        }
                                        else if(str_comp(child.pName, "Text Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayListTextOpacity;
                                        }
                                        else if(str_comp(child.pName, "Main Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayListMainOpacity;
                                        }
                                        else if(str_comp(child.pName, "Panel Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayListPanelOpacity;
                                        }
                                        else if(str_comp(child.pName, "Max Length") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayListMaxLength;
                                        }
                                }
                        }
                        // v6.4: ArrayInfo settings.
                        else if(str_comp(row.pName, "ArrayInfo") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxArrayInfoEnabled != 0;
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const bool Dragging = (m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c);
                                        if(str_comp(child.pName, "X Anchor") == 0)
                                                child.valueIdx = g_Config.m_KxArrayInfoXAnchor;
                                        else if(str_comp(child.pName, "Style") == 0)
                                                child.valueIdx = g_Config.m_KxArrayInfoStyle;
                                        else if(str_comp(child.pName, "Indentation") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayInfoIndent;
                                        }
                                        else if(str_comp(child.pName, "Kinetix Logo") == 0)
                                                child.on = g_Config.m_KxArrayInfoLogo != 0;
                                        else if(str_comp(child.pName, "Watermark") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxArrayInfoWatermark, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Show FPS") == 0)
                                                child.on = g_Config.m_KxArrayInfoShowFps != 0;
                                        else if(str_comp(child.pName, "Session Time") == 0)
                                                child.on = g_Config.m_KxArrayInfoSessionTime != 0;
                                        else if(str_comp(child.pName, "Tee") == 0)
                                                child.on = g_Config.m_KxArrayInfoTee != 0;
                                        else if(str_comp(child.pName, "Nickname") == 0)
                                                child.on = g_Config.m_KxArrayInfoNickname != 0;
                                        else if(str_comp(child.pName, "Functions") == 0)
                                                child.on = g_Config.m_KxArrayInfoFunctions != 0;
                                        else if(str_comp(child.pName, "Max Length") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayInfoMaxLength;
                                        }
                                        else if(str_comp(child.pName, "ArrayInfo Size") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxArrayInfoSize / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Text Color") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxArrayInfoTextColor, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Rainbow Text") == 0)
                                                child.on = g_Config.m_KxArrayInfoRainbow != 0;
                                        else if(str_comp(child.pName, "Rainbow Mode") == 0)
                                                child.valueIdx = g_Config.m_KxArrayInfoRainbowMode;
                                        else if(str_comp(child.pName, "Speed") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxArrayInfoRainbowSpeed / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Step") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayInfoRainbowStep;
                                        }
                                        else if(str_comp(child.pName, "Text Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayInfoTextOpacity;
                                        }
                                        else if(str_comp(child.pName, "Main Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayInfoMainOpacity;
                                        }
                                        else if(str_comp(child.pName, "Panel Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayInfoPanelOpacity;
                                        }
                                }
                        }
                        // v6.7: ArrayPlayer settings.
                        else if(str_comp(row.pName, "ArrayPlayer") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxArrayPlayerEnabled != 0;
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const bool Dragging = (m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c);
                                        if(str_comp(child.pName, "X Anchor") == 0)
                                                child.valueIdx = g_Config.m_KxArrayPlayerXAnchor;
                                        else if(str_comp(child.pName, "Style") == 0)
                                                child.valueIdx = g_Config.m_KxArrayPlayerStyle;
                                        else if(str_comp(child.pName, "Indentation") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerIndent;
                                        }
                                        else if(str_comp(child.pName, "ArrayPlayer Size") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxArrayPlayerSize / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Text Color") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxArrayPlayerTextColor, sizeof(child.m_aInputBuf));
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Dynamic Scale") == 0)
                                                child.on = g_Config.m_KxArrayPlayerDynamicScale != 0;
                                        else if(str_comp(child.pName, "Height ArrayPlayer") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerHeight;
                                        }
                                        else if(str_comp(child.pName, "Weight ArrayPlayer") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerWidth;
                                        }
                                        else if(str_comp(child.pName, "Rainbow Text") == 0)
                                                child.on = g_Config.m_KxArrayPlayerRainbow != 0;
                                        else if(str_comp(child.pName, "Rainbow Mode") == 0)
                                                child.valueIdx = g_Config.m_KxArrayPlayerRainbowMode;
                                        else if(str_comp(child.pName, "Speed") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxArrayPlayerRainbowSpeed / 10.0f;
                                        }
                                        else if(str_comp(child.pName, "Step") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerRainbowStep;
                                        }
                                        else if(str_comp(child.pName, "Text Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerTextOpacity;
                                        }
                                        else if(str_comp(child.pName, "Main Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerMainOpacity;
                                        }
                                        else if(str_comp(child.pName, "Panel Opacity") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxArrayPlayerPanelOpacity;
                                        }
                                }
                        }
                        // v1.56.283: Custom colors — one branch for the selected Sprite/Entity.
                        else if(str_comp(row.pName, "Custom colors") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxCustomColors != 0;
                                int SpriteIdx = 0, EntIdx = 0;
                                CustomColorSelection(row, &SpriteIdx, &EntIdx);
                                const SCustomColorCfg Cfg = CustomColorCfgFor(SpriteIdx, EntIdx);
                                if(Cfg.pOn)
                                {
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                SRow &child = row.pChildren[c];
                                                const bool Dragging = (m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c);
                                                if(str_comp(child.pName, "Enable") == 0)
                                                        child.on = *Cfg.pOn != 0;
                                                else if(str_comp(child.pName, "Color") == 0)
                                                {
                                                        if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                        {
                                                                char aHex[8];
                                                                str_format(aHex, sizeof(aHex), "%06x", *Cfg.pColor & 0xffffff);
                                                                str_copy(child.m_aInputBuf, aHex, sizeof(child.m_aInputBuf));
                                                                child.m_InputInitialized = true;
                                                        }
                                                }
                                                else if(str_comp(child.pName, "Opacity") == 0)
                                                {
                                                        if(!Dragging)
                                                                child.value = (float)*Cfg.pAlpha;
                                                }
                                                else if(str_comp(child.pName, "Rainbow") == 0)
                                                        child.on = *Cfg.pAnim != 0;
                                                else if(str_comp(child.pName, "Speed") == 0)
                                                {
                                                        if(!Dragging)
                                                                child.value = (float)*Cfg.pSpeed / 10.0f;
                                                }
                                        }
                                }
                        }
                        // v1.56.283: Pathfinder "Settings" visual expandable children.
                        else if(str_comp(row.pName, "Settings") == 0 && row.type == ERowType::Expandable && str_comp(g_Panels[p].pTitle, "Pathfinder") == 0)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const bool Dragging = (m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == c);
                                        if(str_comp(child.pName, "A* Weight") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = g_Config.m_KxPfAStarWeight / 100.0f;
                                        }
                                        else if(str_comp(child.pName, "Evolutions") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPfRheaGenerations;
                                        }
                                        else if(str_comp(child.pName, "Beam width") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPfBeamWidth;
                                        }
                                        else if(str_comp(child.pName, "Performance") == 0)
                                        {
                                                if(!Dragging)
                                                        child.value = (float)g_Config.m_KxPfPerf;
                                        }
                                        else if(str_comp(child.pName, "Freeze Support (Beta)") == 0)
                                                child.on = g_Config.m_KxPfFreezeSupport != 0;
                                }
                        }
                        // v1.56.260: Panels Start X/Y + Drag-To-Sort.
                        else if(str_comp(row.pName, "Panels Start X") == 0 && row.type == ERowType::Slider)
                        {
                                if(!(m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == -1))
                                        row.value = (float)g_Config.m_KxPanelsStartX;
                        }
                        else if(str_comp(row.pName, "Panels Start Y") == 0 && row.type == ERowType::Slider)
                        {
                                if(!(m_DragSliderPanel == p && m_DragSliderRow == r && m_DragSliderChild == -1))
                                        row.value = (float)g_Config.m_KxPanelsStartY;
                        }
                        else if(str_comp(row.pName, "Drag-To-Sort") == 0 && row.type == ERowType::Toggle)
                                row.on = g_Config.m_KxDragToSort != 0;
                        // v1.56.260: Visuals panel.
                        else if(str_comp(row.pName, "Hide particles") == 0 && row.type == ERowType::Toggle)
                                row.on = g_Config.m_KxHideParticles != 0;
                        // v1.56.275: Visuals rainbow expandables — Color/Opacity/Rainbow/Speed.
                        else if(str_comp(row.pName, "Custom color tee") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowTee != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowTeeColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowTeeAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowTeeAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowTeeSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color legs") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowLegs != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowLegsColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowLegsAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowLegsAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowLegsSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color entities") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowEnt != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowEntColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowEntAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowEntAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowEntSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color guns") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowGuns != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowGunsColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowGunsAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowGunsAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowGunsSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color hook") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowHook != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowHookColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowHookAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowHookAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowHookSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color HUD") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowHud != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowHudColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowHudAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowHudAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowHudSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color particles") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowParticles != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowParticlesColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowParticlesAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowParticlesAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowParticlesSpeed / 10.0f;
                        }
                        else if(str_comp(row.pName, "Custom color emoticons") == 0 && row.type == ERowType::Expandable)
                        {
                                row.on = g_Config.m_KxRainbowEmoticons != 0;
                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != 0)
                                {
                                        char aHex[8];
                                        str_format(aHex, sizeof(aHex), "%06x", g_Config.m_KxRainbowEmoticonsColor & 0xffffff);
                                        str_copy(row.pChildren[0].m_aInputBuf, aHex, sizeof(row.pChildren[0].m_aInputBuf));
                                        row.pChildren[0].m_InputInitialized = true;
                                }
                                row.pChildren[1].value = (float)g_Config.m_KxRainbowEmoticonsAlpha;
                                row.pChildren[2].on = g_Config.m_KxRainbowEmoticonsAnim != 0;
                                row.pChildren[3].value = (float)g_Config.m_KxRainbowEmoticonsSpeed / 10.0f;
                        }
                        else if(p == 4 && row.type == ERowType::Dropdown && str_comp(row.pName, "Bind Mode") == 0)
                                row.valueIdx = g_Config.m_KxBindsMode;
                        // v1.56.199: Binds panel — sync input fields with actual bound keys
                        // every frame (not just on open), so console bind changes appear
                        // immediately while ClickGUI is open.
                        // v1.56.283: Binds expandable — sync all bind input children
                        // every frame (console bind changes reflect immediately).
                        else if(p == 4 && row.type == ERowType::Expandable && str_comp(row.pName, "Binds") == 0)
                        {
                                for(int c = 0; c < row.childCount; ++c)
                                {
                                        SRow &child = row.pChildren[c];
                                        const char *pCmd = nullptr;
                                        if(str_comp(child.pName, "ClickGUI") == 0) pCmd = "toggle cl_clickgui 1 0";
                                        else if(str_comp(child.pName, "AimBot") == 0) pCmd = "toggle kx_aimbot 1 0";
                                        else if(str_comp(child.pName, "TriggerBot") == 0) pCmd = "toggle kx_triggerbot 1 0";
                                        else if(str_comp(child.pName, "Fake Aim") == 0) pCmd = "toggle kx_fake_aim 1 0";
                                        else if(str_comp(child.pName, "Laser Unfreeze") == 0) pCmd = "toggle kx_laser_unfreeze 1 0";
                                        else if(str_comp(child.pName, "Copy Moves Filter") == 0) pCmd = "toggle kx_dummy_copy_moves_filter 1 0";
                                        else if(str_comp(child.pName, "Copy Moves Latency") == 0) pCmd = "toggle kx_copy_moves_latency_enabled 1 0";
                                        else if(str_comp(child.pName, "Trajectory") == 0) pCmd = "toggle kx_show_trajectory 1 0";
                                        else if(str_comp(child.pName, "ESP") == 0) pCmd = "toggle kx_esp 1 0";
                                        else if(str_comp(child.pName, "Avoid Freeze") == 0) pCmd = "toggle kx_avoid_freeze 1 0";
                                        else if(str_comp(child.pName, "Helpers") == 0) pCmd = "toggle kx_helpers 1 0";
                                        else if(str_comp(child.pName, "Auto Triple Fly") == 0) pCmd = "toggle kx_triple_fly 1 0";
                                        else if(str_comp(child.pName, "Auto Fire") == 0) pCmd = "toggle kx_auto_fire 1 0";
                                        else if(str_comp(child.pName, "BalanceBot") == 0) pCmd = "toggle kx_balance_bot 1 0";
                                        else if(str_comp(child.pName, "TAS Record") == 0) pCmd = "toggle kx_tas_record 1 0";
                                        else if(str_comp(child.pName, "TAS Pause") == 0) pCmd = "toggle kx_tas_pause 1 0";
                                        else if(str_comp(child.pName, "TAS Rewind") == 0) pCmd = "kx_tas_rewind";
                                        else if(str_comp(child.pName, "TAS Forward") == 0) pCmd = "kx_tas_forward";
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
                                        // v6.7: Smart Dummy Switch children (index order: Button,
                                        // Order Type, Without Repeat, filters + inputs).
                                        else if(str_comp(child.pName, "Order Type") == 0)
                                                child.valueIdx = g_Config.m_KxSdsOrder;
                                        else if(str_comp(child.pName, "Without Repeat") == 0)
                                                child.on = g_Config.m_KxSdsNoRepeat != 0;
                                        else if(str_comp(child.pName, "Team") == 0)
                                                child.valueIdx = g_Config.m_KxSdsTeam;
                                        else if(str_comp(child.pName, "DDrace Team") == 0)
                                                child.valueIdx = g_Config.m_KxSdsDdraceTeam;
                                        else if(str_comp(child.pName, "Target ID") == 0)
                                        {
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_format(child.m_aInputBuf, sizeof(child.m_aInputBuf), "%d", g_Config.m_KxSdsDdraceTeamId);
                                                        child.m_InputInitialized = true;
                                                }
                                        }
                                        else if(str_comp(child.pName, "Friend") == 0)
                                                child.valueIdx = g_Config.m_KxSdsFriend;
                                        else if(str_comp(child.pName, "Clanmate") == 0)
                                                child.valueIdx = g_Config.m_KxSdsClanmate;
                                        else if(str_comp(child.pName, "Freeze") == 0)
                                                child.valueIdx = g_Config.m_KxSdsFreeze;
                                        else if(str_comp(child.pName, "Targets") == 0)
                                                child.valueIdx = g_Config.m_KxSdsTargetsMode;
                                        else if(str_comp(child.pName, "Targets IDs") == 0 || str_comp(child.pName, "Untargets IDs") == 0)
                                        {
                                                child.pName = (g_Config.m_KxSdsTargetsMode == 2) ? "Untargets IDs" : "Targets IDs";
                                                if(!m_InputEditing || m_InputPanel != p || m_InputRow != r || m_InputChild != c)
                                                {
                                                        str_copy(child.m_aInputBuf, g_Config.m_KxSdsTargets, sizeof(child.m_aInputBuf));
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
                                                case KX_LINE_HITBOXES: child.on = g_Config.m_KxLineHitboxesRainbow != 0; break;
                                                case KX_LINE_RT_PATHFINDER: child.on = g_Config.m_KxLineRtPathfinderRainbow != 0; break;
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
									case KX_LINE_HITBOXES: child.value = (float)g_Config.m_KxLineHitboxesRainbowSpeed / 10.0f; break;
									case KX_LINE_RT_PATHFINDER: child.value = (float)g_Config.m_KxLineRtPathfinderRainbowSpeed / 10.0f; break;
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
									case KX_LINE_HITBOXES: child.on = g_Config.m_KxLineHitboxesGradient != 0; break;
									case KX_LINE_RT_PATHFINDER: child.on = g_Config.m_KxLineRtPathfinderGradient != 0; break;
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
                                                case KX_LINE_HITBOXES: child.value = (float)g_Config.m_KxLineHitboxesGradientStep; break;
                                                case KX_LINE_RT_PATHFINDER: child.value = (float)g_Config.m_KxLineRtPathfinderGradientStep; break;
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
        // v6.3: entering the ArrayList editor closes the GUI but keeps the
        // cursor visible, so skip the instant hide in that case.
        if(!isOpen && wasOpenThisFrame && !GameClient()->m_GameConsole.IsActive() && !m_ArrayListEditor && !m_ArrayInfoEditor)
                Input()->MouseModeRelative();

        // Drive animation toward the current cl_clickgui target (0 or 1).
        // v1.56.285: exponential approach (k = 7 / DURATION — matches the CSS
        // curve cubic-bezier(0.16,1,0.3,1)). m_AnimProgress now IS the eased
        // value: fast-start on open, fast-start vanish on close, and a
        // mid-flight flip (sharp close right after open) stays value-continuous.
        // The old linear timeline + asymmetric ease-OUT curve stalled the close
        // for half of the duration and then snapped shut (broken exit anims).
        const float target = isOpen ? 1.0f : 0.0f;
        const float dt = Client()->RenderFrameTime();
        const float speed = (ANIM_DURATION > 0.0f) ? (1.0f / ANIM_DURATION) : 1.0f;
        if(m_AnimProgress != target)
        {
                m_AnimProgress += (target - m_AnimProgress) * (1.0f - std::exp(-(7.0f * speed) * dt));
                if(std::fabs(target - m_AnimProgress) < 0.002f)
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
        UpdateDummyManagerRows(); // v1.56.282: Dummy Manager panel sync

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
                {
                        // v6.3: the ArrayList editor needs a visible cursor even
                        // though the ClickGUI itself is closed.
                        if(m_ArrayListEditor)
                                Input()->MouseModeAbsolute();
                        else
                                Input()->MouseModeRelative();
                }
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
                // v1.56.260: Drag-To-Sort off → free positioning, no slot swaps.
                for(int s = 0; s < NUM_PANELS && EffectiveDragToSort(); ++s)
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

        // v1.56.285: panel edge resize (Dynamic scale) — live width/height,
        // measured from the panel's CURRENT position: its slot home shifts
        // while the width changes (the grid reflows around the new width), so
        // the old drag-start + delta math overshot on every panel except the
        // leftmost one. The bottom edge accounts for the header height.
        if(m_ResizePanel >= 0)
        {
                const vec2 PanelPos = m_aPanelPosValid[m_ResizePanel] ? m_aPanelPos[m_ResizePanel] : PanelHomePos(m_ResizePanel);
                if(m_ResizeEdge & 1)
                {
                        const float w = std::clamp(m_MousePos.x - PanelPos.x, 150.0f, 800.0f);
                        g_Config.m_KxPanelsWidth = (int)w;
                        g_PanelW = w;
                }
                if(m_ResizeEdge & 2)
                {
                        const float h = std::clamp(m_MousePos.y - PanelPos.y - HEADER_HEIGHT, 200.0f, 2000.0f);
                        g_Config.m_KxPanelsHeight = (int)h;
                        g_PanelMaxBodyH = h;
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
                        const float trackW = KxTpsTrackW(pTarget, PANEL_WIDTH - BODY_PADDING * 2 - indent);
                        // TPS row: the thumb center travels track minus thumb
                        // width, so the mouse maps onto the same span.
                        float pct;
                        if(str_comp(pTarget->pName, "TPS") == 0)
                                pct = (m_MousePos.x - trackX - SLIDER_THUMB_W * 0.5f) / (trackW - SLIDER_THUMB_W);
                        else
                                pct = (m_MousePos.x - trackX) / trackW;
                        pct = std::clamp(pct, 0.0f, 1.0f);
{
                                                // v1.56.284: snap to the step for ANY step > 0
                                                // (fractional steps included), clamped to [min, max].
                                                float raw = pTarget->min + pct * (pTarget->max - pTarget->min);
                                                if(pTarget->step > 0.0f)
                                                {
                                                        pTarget->value = pTarget->min + roundf((raw - pTarget->min) / pTarget->step) * pTarget->step;
                                                        if(pTarget->value > pTarget->max)
                                                                pTarget->value = pTarget->max;
                                                        if(pTarget->value < pTarget->min)
                                                                pTarget->value = pTarget->min;
                                                }
                                                else
                                                        pTarget->value = raw;
                                        }
                        // v6.3: ArrayList HUD + ToolTips sliders. Parent-gated because
                        // names like "Speed"/"Step"/"Main Opacity" are shared with other
                        // expandables (the generic branches below would steal them).
                        if(str_comp(row.pName, "ArrayList") == 0)
                        {
                                if(str_comp(pTarget->pName, "Indentation") == 0)
                                        g_Config.m_KxArrayListIndent = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "ArrayList Size") == 0)
                                        g_Config.m_KxArrayListSize = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Max Length") == 0)
                                        g_Config.m_KxArrayListMaxLength = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Speed") == 0)
                                        g_Config.m_KxArrayListRainbowSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Step") == 0)
                                        g_Config.m_KxArrayListRainbowStep = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Text Opacity") == 0)
                                        g_Config.m_KxArrayListTextOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                        g_Config.m_KxArrayListMainOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                        g_Config.m_KxArrayListPanelOpacity = (int)(pTarget->value + 0.5f);
                        }
                        else if(str_comp(row.pName, "ArrayInfo") == 0)
                        {
                                if(str_comp(pTarget->pName, "Max Length") == 0)
                                        g_Config.m_KxArrayInfoMaxLength = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "ArrayInfo Size") == 0)
                                        g_Config.m_KxArrayInfoSize = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Indentation") == 0)
                                        g_Config.m_KxArrayInfoIndent = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Speed") == 0)
                                        g_Config.m_KxArrayInfoRainbowSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Step") == 0)
                                        g_Config.m_KxArrayInfoRainbowStep = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Text Opacity") == 0)
                                        g_Config.m_KxArrayInfoTextOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                        g_Config.m_KxArrayInfoMainOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                        g_Config.m_KxArrayInfoPanelOpacity = (int)(pTarget->value + 0.5f);
                        }
                        else if(str_comp(row.pName, "ArrayPlayer") == 0)
                        {
                                if(str_comp(pTarget->pName, "Indentation") == 0)
                                        g_Config.m_KxArrayPlayerIndent = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "ArrayPlayer Size") == 0)
                                        g_Config.m_KxArrayPlayerSize = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Height ArrayPlayer") == 0)
                                        g_Config.m_KxArrayPlayerHeight = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Weight ArrayPlayer") == 0)
                                        g_Config.m_KxArrayPlayerWidth = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Speed") == 0)
                                        g_Config.m_KxArrayPlayerRainbowSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "Step") == 0)
                                        g_Config.m_KxArrayPlayerRainbowStep = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Text Opacity") == 0)
                                        g_Config.m_KxArrayPlayerTextOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                        g_Config.m_KxArrayPlayerMainOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                        g_Config.m_KxArrayPlayerPanelOpacity = (int)(pTarget->value + 0.5f);
                        }
                        else if(str_comp(row.pName, "ClickGUI Settings") == 0 && str_comp(pTarget->pName, "ToolTips Text Opacity") == 0)
                                g_Config.m_KxUiTooltipTextOpacity = (int)(pTarget->value + 0.5f);
                        else if(str_comp(row.pName, "ClickGUI Settings") == 0 && str_comp(pTarget->pName, "ToolTips Main Opacity") == 0)
                                g_Config.m_KxUiTooltipMainOpacity = (int)(pTarget->value + 0.5f);
                        else if(str_comp(row.pName, "ClickGUI Settings") == 0 && str_comp(pTarget->pName, "ToolTips Panel Opacity") == 0)
                                g_Config.m_KxUiTooltipPanelOpacity = (int)(pTarget->value + 0.5f);
                        else if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Custom latency") == 0)
                                g_Config.m_KxDummyHammerLatencyMs = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "BG Blackout") == 0)
                                m_BgBlackout = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                m_MainOpacity = pTarget->value / 100.0f;
                        else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                m_PanelOpacity = pTarget->value / 100.0f;
                        // v1.56.260: Panels Start X/Y live update while dragging.
                        else if(str_comp(pTarget->pName, "Panels Start X") == 0)
                                g_Config.m_KxPanelsStartX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels Start Y") == 0)
                                g_Config.m_KxPanelsStartY = (int)pTarget->value;
                        // v1.56.283: Panels indentation + width/height live update.
                        else if(str_comp(pTarget->pName, "Panels X-indentation") == 0)
                                g_Config.m_KxPanelsIndentX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels Y-indentation") == 0)
                                g_Config.m_KxPanelsIndentY = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels width") == 0)
                                g_Config.m_KxPanelsWidth = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels height") == 0)
                                g_Config.m_KxPanelsHeight = (int)pTarget->value;
                        // v1.56.287: BG Blur radius live update while dragging.
                        else if(str_comp(pTarget->pName, "Blur Radius") == 0)
                                g_Config.m_KxUiBgBlurRadius = (int)pTarget->value;
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
								case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderRainbowSpeed = rawVal; break;
                                case KX_LINE_ESP: g_Config.m_KxLineEspRainbowSpeed = rawVal; break;
                                case KX_LINE_TAS: g_Config.m_KxLineTasRainbowSpeed = rawVal; break;
                                 case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesRainbowSpeed = rawVal; break;
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
								case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderGradientStep = rawVal; break;
                                case KX_LINE_ESP: g_Config.m_KxLineEspGradientStep = rawVal; break;
                                case KX_LINE_TAS: g_Config.m_KxLineTasGradientStep = rawVal; break;
                                 case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesGradientStep = rawVal; break;
                                }
                        }
                        else if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Copy Moves Latency") == 0)
                                g_Config.m_KxCopyMovesLatency = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ratio") == 0 && str_comp(row.pName, "Freedom Path") == 0)
                                g_Config.m_KxPfFreedomRatio = (int)(pTarget->value * 10.0f + 0.5f);
                        // v6.7: Smart Copy Moves top-N slider.
                        else if(str_comp(pTarget->pName, "Apply Dummies") == 0)
                                g_Config.m_KxSmartCopyCount = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                g_Config.m_KxTripleFlyTriggerRadius = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                g_Config.m_KxTripleFlyRadius = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Edge in ms") == 0 && str_comp(row.pName, "Edge bind") == 0)
                                g_Config.m_KxEdgeBindMs = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Version ID") == 0)
                                g_Config.m_KxVersionSpoofId = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Fake Zoom") == 0)
                                g_Config.m_KxSpoofZoomValue = (int)(pTarget->value * 10.0f + 0.5f);
                        else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeFov = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Trigger ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                g_Config.m_KxLaserUnfreezeTriggerTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                g_Config.m_KxAfTicks = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Ticks Before") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                g_Config.m_KxAfTicksBefore = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                g_Config.m_KxAfAngles = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                g_Config.m_KxAfFov = (int)pTarget->value;
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
                                        case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderSize = (int)pTarget->value; break;
                                        case KX_LINE_ESP: g_Config.m_KxLineEspSize = (int)pTarget->value; break;
                                        case KX_LINE_TAS: g_Config.m_KxLineTasSize = (int)pTarget->value; break;
                                         case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesSize = (int)pTarget->value; break;
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
                                        case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderAlpha = (int)pTarget->value; break;
                                        case KX_LINE_ESP: g_Config.m_KxLineEspAlpha = (int)pTarget->value; break;
                                        case KX_LINE_TAS: g_Config.m_KxLineTasAlpha = (int)pTarget->value; break;
                                         case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesAlpha = (int)pTarget->value; break;
                                        }
                                        }
                        }
                        else if(str_comp(pTarget->pName, "Layer") == 0)
                                g_Config.m_KxLineRenderingLayer = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "X") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspScreenX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Y") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspScreenY = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Pointer Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspSpeed = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Box Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspBoxSpeed = (int)pTarget->value;
                        // v1.56.256: ESP box scale sliders.
                        else if(str_comp(pTarget->pName, "X Box scale") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspBoxScaleX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Y Box scale") == 0 && str_comp(row.pName, "ESP") == 0)
                                g_Config.m_KxEspBoxScaleY = (int)pTarget->value;
                        // v1.56.256: Hitboxes "Box Speed" sliders — same label, two rows (by index).
                        else if(str_comp(pTarget->pName, "Box Speed") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                        {
                                if(m_DragSliderChild == 2)
                                        g_Config.m_KxHitboxesTileSpeed = (int)pTarget->value;
                                else if(m_DragSliderChild == 5)
                                        g_Config.m_KxHitboxesTriggerSpeed = (int)pTarget->value;
                        }
                        // v1.56.178: Trajectory per-type slider — writes to CTrajectory.m_aTypes[sel].
                        else if(str_comp(pTarget->pName, "Prediction Ticks") == 0 && str_comp(row.pName, "Trajectory") == 0)
                        {
                                CTrajectory &traj = GameClient()->m_Trajectory;
                                int sel = traj.m_SelectedType;
                                if(sel >= 0 && sel < CTrajectory::NUM_TRAJ_TYPES)
                                        traj.m_aTypes[sel].m_PredictionTicks = (int)pTarget->value;
                        }
                        // v1.56.283: Visuals — Custom colors Speed/Opacity sliders for
                        // the selected Sprite/Entity (before the generic "Speed"
                        // branch, which would otherwise write Fake Aim speed).
                        else if(str_comp(pTarget->pName, "Speed") == 0 && str_comp(row.pName, "Custom colors") == 0)
                        {
                                int SpriteIdx = 0, EntIdx = 0;
                                CustomColorSelection(row, &SpriteIdx, &EntIdx);
                                const SCustomColorCfg Cfg = CustomColorCfgFor(SpriteIdx, EntIdx);
                                if(Cfg.pOn)
                                        *Cfg.pSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                        }
                        else if(str_comp(pTarget->pName, "Opacity") == 0 && str_comp(row.pName, "Custom colors") == 0)
                        {
                                int SpriteIdx = 0, EntIdx = 0;
                                CustomColorSelection(row, &SpriteIdx, &EntIdx);
                                const SCustomColorCfg Cfg = CustomColorCfgFor(SpriteIdx, EntIdx);
                                if(Cfg.pOn)
                                        *Cfg.pAlpha = (int)(pTarget->value + 0.5f);
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
                                                if(str_comp(parentRow.pName, "ClickGUI Settings") == 0)
                                                        isRainbow = true;
                                        }
                                }
                                if(isRainbow)
                                        m_RainbowSpeed = pTarget->value;
                                else
                                        g_Config.m_KxFakeAimSpeed = (int)pTarget->value;
                        }
                        // v1.56.247: Fake Aim Chaos jitter strength + Distance from center.
                        else if(str_comp(pTarget->pName, "Jitter") == 0)
                        {
                                // v1.56.268: "Jitter" is shared by Fake Aim and the per-weapon
                                // AimBot slider after the label cleanup. Disambiguate by which
                                // panel the slider is in — same technique as "Speed" above.
                                bool isAimBot = false;
                                if(m_DragSliderChild >= 0 && m_DragSliderPanel >= 0)
                                {
                                        SPanel &dragPanel = g_Panels[m_DragSliderPanel];
                                        if(m_DragSliderRow >= 0 && m_DragSliderRow < dragPanel.rowCount)
                                        {
                                                SRow &parentRow = dragPanel.pRows[m_DragSliderRow];
                                                if(str_comp(parentRow.pName, "AimBot") == 0)
                                                        isAimBot = true;
                                        }
                                }
                                if(isAimBot)
                                {
                                        CAimBot &ab = GameClient()->m_AimBot;
                                        auto &s = AbEditSettings(ab);
                                        s.m_Jitter = pTarget->value;
                                }
                                else
                                        g_Config.m_KxFakeAimJitter = (int)pTarget->value;
                        }
                        else if(str_comp(pTarget->pName, "Distance from center") == 0)
                                g_Config.m_KxFakeAimDistCenter = (int)pTarget->value;
                        // v1.56.262: Admin Alarm interval + Auto Fire + BalanceBot radii.
                        else if(str_comp(pTarget->pName, "Every ms") == 0)
                        {
                                // per level (Level dropdown selects which one is edited)
                                if(g_Config.m_KxAdminAlarmLevel == 0)
                                        g_Config.m_KxAdminAlarmMs1 = (int)pTarget->value;
                                else if(g_Config.m_KxAdminAlarmLevel == 1)
                                        g_Config.m_KxAdminAlarmMs2 = (int)pTarget->value;
                                else
                                        g_Config.m_KxAdminAlarmMs3 = (int)pTarget->value;
                        }
                        else if(str_comp(pTarget->pName, "Delay") == 0)
                                g_Config.m_KxAutoFireDelay = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Radius Filter") == 0)
                                g_Config.m_KxBalanceBotRadius = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Stand Radius") == 0)
                                g_Config.m_KxBalanceBotStandRadius = (int)pTarget->value;
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
                        // v1.56.280: RT Pathfinder sliders (live while dragging)
                        else if(str_comp(pTarget->pName, "Reconstruct Trigger") == 0)
                                g_Config.m_KxRtPfTrigger = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Future Ticks") == 0)
                                g_Config.m_KxRtPfFutureTicks = (int)pTarget->value;
                        // v1.56.282: Dummy Manager — live dummy limit while dragging.
                        else if(str_comp(pTarget->pName, "Dummies") == 0)
                                Client()->SetDummyLimit((int)pTarget->value);
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
                                                auto &s = AbEditSettings(ab);
                                                if(str_comp(pTarget->pName, "FOV") == 0)
                                                        s.m_Fov = pTarget->value;
                                                else if(str_comp(pTarget->pName, "Radius") == 0)
                                                        s.m_Radius = pTarget->value;
                                                else if(str_comp(pTarget->pName, "Jitter") == 0)
                                                        s.m_Jitter = pTarget->value;
                                                else if(str_comp(pTarget->pName, "Smooth Speed") == 0)
                                                        s.m_SmoothSpeed = pTarget->value;
                                        }
                                        else if(str_comp(parentRow.pName, "TriggerBot") == 0)
                                        {
                                                auto &s = TbEditSettings(ab);
                                                if(str_comp(pTarget->pName, "FOV") == 0)
                                                        s.m_Fov = pTarget->value;
                                                else if(str_comp(pTarget->pName, "Radius") == 0)
                                                        s.m_Radius = pTarget->value;
                                                else if(str_comp(pTarget->pName, "Latency") == 0)
                                                        s.m_RandomLatency = (int)pTarget->value;
                                        }
                                }
                        }
                }
        }

        // v1.56.285: m_AnimProgress is already the eased value (exponential
        // approach, k = 7 / DURATION ≈ the CSS curve) — use it directly for
        // both directions. CubicBezierEase(linear progress) is an ease-OUT
        // curve: applied to a decreasing progress it stalled the close for
        // half of the duration and then snapped shut (broken exit animation).
        float eased = m_AnimProgress;
        const float alpha = eased;

        // Set up pixel-space projection. The screen is mapped into the virtual
        // 1920x1080 space (DPI-scaled), so all hardcoded geometry below renders
        // at the same physical size on every resolution.
        const float screenW = VScreenWidth();
        const float screenH = VScreenHeight();
        Graphics()->MapScreenToSize(screenW, screenH);

        Graphics()->TextureClear();

        // v6.3: the HUD is ALWAYS drawn UNDER the ClickGUI — the blur capture and
        // the blackout are painted over it, so it appears exactly as it will in
        // game (behind the dim/blur) instead of floating above the panels.
        if(ArrayListVisible)
        {
                UpdateArrayList(Client()->RenderFrameTime());
                RenderArrayList(1.0f);
        }
        if(ArrayInfoVisible)
        {
                UpdateArrayInfo(Client()->RenderFrameTime());
                RenderArrayInfo(1.0f);
        }
        if(ArrayPlayerVisible)
        {
                UpdateArrayPlayer(Client()->RenderFrameTime());
                RenderArrayPlayer(1.0f);
        }

        // v1.56.287: BG Blur — capture the rendered game behind the GUI, blur it
        // and draw it fullscreen under the dim overlay.
        // v1.56.299: the blur strength is multiplied by the eased open/close
        // progress, so the backdrop unblurs smoothly on close and blurs in on
        // open instead of popping (the capture's alpha is unreliable, so the
        // fade rides the radius, not the alpha).
        if(eased > 0.0f && g_Config.m_KxUiSettingsOn != 0 && g_Config.m_KxUiBgBlur != 0)
                RenderBlurBackground(eased);

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

        // v1.56.286: Search panel — live query polling + match-flag recompute.
        // While the Search field is being edited, the query follows the edit
        // buffer in real time. On any query/filter change the match flags are
        // recomputed; the layout signatures pick the change up automatically
        // (hidden rows drop out of the signature) and the panel bodies glide
        // to their new height via the smoothed body height in RenderPanel.
        {
                static char s_LastQuery[128] = {0};
                static bool s_LastCase = false;
                static bool s_LastWhole = false;
                static bool s_LastRegex = false;
                static bool s_LastFuzzy = false;
                static bool s_SearchInit = false;
                SRow *pEditing = (SRow *)GetEditingInputRowPtr();
                if(m_InputEditing && pEditing == &g_Panels[NUM_PANELS - 1].pRows[0])
                        str_copy(g_SearchQuery, m_InputBuf, sizeof(g_SearchQuery));
                if(!s_SearchInit || str_comp(s_LastQuery, g_SearchQuery) != 0 ||
                        s_LastCase != g_SearchCaseSensitive || s_LastWhole != g_SearchWholeWord ||
                        s_LastRegex != g_SearchRegex || s_LastFuzzy != g_SearchFuzzy)
                {
                        str_copy(s_LastQuery, g_SearchQuery, sizeof(s_LastQuery));
                        s_LastCase = g_SearchCaseSensitive;
                        s_LastWhole = g_SearchWholeWord;
                        s_LastRegex = g_SearchRegex;
                        s_LastFuzzy = g_SearchFuzzy;
                        s_SearchInit = true;
                        SearchUpdateMatches();
                }
        }

        // v1.56.285: PANEL_WIDTH changed since the last frame (ClickGUI Settings
        // slider or edge resize)? The cached Yoga layouts were built with the
        // old width — invalidate them all so the rows reflow. Without this the
        // row visuals kept the cached width while collisions used the live one.
        static float s_LastLayoutW = -1.0f;
        if(g_PanelW != s_LastLayoutW)
        {
                for(int p = 0; p < NUM_PANELS; ++p)
                        m_aLayoutCacheDirty[p] = true;
                s_LastLayoutW = g_PanelW;
        }

        // v1.56.285: Panels X/Y-indentation is the GAP BETWEEN panels (user
        // clarification) — applied inside SlotHomePos as spacingX/spacingY.
        // When a layout offset config changes (Indent or Start X/Y), force
        // every panel to glide to its new slot home so the sliders give live
        // feedback even with KxDragToSort off.
        {
                static float s_LastIndentX = -1.0f;
                static float s_LastIndentY = -1.0f;
                static float s_LastStartX = -1.0f;
                static float s_LastStartY = -1.0f;
                static int s_LastUiOn = -1;
                static int s_LastDragSort = -1;
                // v1.56.299: compare the EFFECTIVE values, so flipping the master
                // toggle off (back to defaults) also re-homes the panels.
                float indentX, indentY, startXCfg, startYCfg;
                EffectivePanelLayout(startXCfg, startYCfg, indentX, indentY);
                const int UiOn = g_Config.m_KxUiSettingsOn != 0;
                const int DragSort = EffectiveDragToSort() ? 1 : 0;
                if(s_LastIndentX != indentX || s_LastIndentY != indentY ||
                        s_LastStartX != startXCfg || s_LastStartY != startYCfg ||
                        s_LastUiOn != UiOn || s_LastDragSort != DragSort)
                {
                        s_LastIndentX = indentX;
                        s_LastIndentY = indentY;
                        s_LastStartX = startXCfg;
                        s_LastStartY = startYCfg;
                        s_LastUiOn = UiOn;
                        s_LastDragSort = DragSort;
                        g_ForceRehome = true;
                }
        }

        // v1.56.285: per-frame OS cursor shape for the resize zones (0 = arrow).
        int CursorShape = 0;

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
        else if(EffectiveDragToSort() || g_ForceRehome)
        {
                // Non-dragged panel: glide toward its slot home (supports drag-to-sort
                // swaps, and re-layout after a Panels Start/Indent X/Y change). When
                // already at home this is a no-op.
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

                // v1.56.285: resize cursor feedback (Dynamic scale) — Windows-style
                // edge cursors while hovering a panel's right/bottom edge strip.
                // The loop runs in z-ascending order, so the last hit (= topmost
                // panel) wins. The actual resize hit-test lives in HandleMouseDown.
                if(g_Config.m_KxPanelsDynamicScale != 0 && g_Config.m_KxUiSettingsOn != 0)
                {
                        const float ph = m_aPanelCurrentHeight[i];
                        const bool RightEdge = PointInRect(m_MousePos, current.x + g_PanelW - 7.0f, current.y, 7.0f, ph);
                        const bool BottomEdge = !m_aPanelCollapsed[i] && PointInRect(m_MousePos, current.x, current.y + ph - 7.0f, g_PanelW, 7.0f);
                        if(RightEdge && BottomEdge)
                                CursorShape = 5; // SDL_SYSTEM_CURSOR_SIZENWSE (corner)
                        else if(RightEdge)
                                CursorShape = 7; // SDL_SYSTEM_CURSOR_SIZEWE (right edge)
                        else if(BottomEdge)
                                CursorShape = 8; // SDL_SYSTEM_CURSOR_SIZENS (bottom edge)
                }
        }

        // v1.56.285: stop the forced re-home once every panel has arrived —
        // a later manual drag then stays where the user put it.
        if(g_ForceRehome)
        {
                bool allHome = true;
                for(int p = 0; p < NUM_PANELS && allHome; ++p)
                {
                        if(!m_aPanelPosValid[p])
                                continue;
                        const vec2 homePos = PanelHomePos(p);
                        if(std::fabs(m_aPanelPos[p].x - homePos.x) > 1.0f || std::fabs(m_aPanelPos[p].y - homePos.y) > 1.0f)
                                allHome = false;
                }
                if(allHome)
                        g_ForceRehome = false;
        }

        // v1.56.285: apply the OS cursor for the resize zones (the SDL system
        // cursors are lazy-created in input.cpp; setting the same shape every
        // frame is a no-op on the OS side). While the GUI is not fully open,
        // force the plain arrow so no stale resize shape leaks between sessions.
        if(m_AnimProgress < 0.95f)
                CursorShape = 0;
        if(m_ResizePanel >= 0)
                CursorShape = (m_ResizeEdge == 1) ? 7 : ((m_ResizeEdge == 2) ? 8 : 5);

        // v6.3: the ArrayList HUD is drawn earlier (under the dim/blur), so there
        // is no second render pass above the panels.

        KxSetCursorShape(CursorShape);

        // Render dropdown popup ON TOP of all panels (if open).
        RenderDropdownPopup(alpha);

        if(m_FileBrowserOpen)
                RenderFileBrowser(alpha);

        // v1.56.284: ToolTip on top of everything — shown after the row has
        // been hovered steadily for TOOLTIP_DELAY seconds. Suppressed while
        // dragging a panel/slider, editing an input or with any popup open.
        // v1.56.288: use the ABSOLUTE client time (GlobalTime), not the frame
        // delta (RenderFrameTime) — the delta stays ~0.016 so the hover timer
        // never accumulated and the tooltip never appeared.
        const float ttNow = Client()->GlobalTime();
        const bool ttAllowed = !m_Dragging && m_DragSliderPanel < 0 && !m_DropdownOpen && !m_InputEditing && !m_FileBrowserOpen && g_Config.m_KxUiShowTooltips != 0;
        if(ttAllowed && m_pToolTipRow && ttNow - m_ToolTipRowTime <= 0.05f)
        {
                if(m_pToolTipRow != m_pToolTipStable)
                {
                        m_pToolTipStable = m_pToolTipRow;
                        m_ToolTipHoverSince = ttNow;
                }
                if(m_aToolTipText[0] && ttNow - m_ToolTipHoverSince >= TOOLTIP_DELAY)
                        RenderToolTip(alpha);
        }
        else
        {
                m_pToolTipStable = nullptr;
                m_ToolTipHoverSince = -1.0f;
        }

        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ============================================================================
// CClickGui — UX (input handling)
// ============================================================================

bool CClickGui::OnInput(const IInput::CEvent &Event)
{
        // The ClickGUI toggle bind must also work while the menu is open: CMenus
        // consumes all input when active, so the event never reaches the bind
        // chain (m_Binds sits after m_Menus). Handle the toggle here instead —
        // but ONLY in that case, so in game the normal bind path runs unchanged
        // (binds may contain more commands than just the toggle). The console
        // check matters because ClickGUI sits before m_GameConsole in the input
        // chain: typing a key bound to cl_clickgui into the console must not
        // toggle the GUI. Every press flips the config, exactly like the bind
        // itself — including mid-animation, so a second press reverses the
        // animation instead of being swallowed.
        if(GameClient()->m_Menus.IsActive() && !GameClient()->m_GameConsole.IsActive() &&
                (Event.m_Flags & IInput::FLAG_PRESS))
        {
                // Same lookup as CBinds::OnInput: a modifier key (e.g. RSHIFT) is
                // bound in the slot WITHOUT its own modifier bit, so it must be
                // excluded from the current modifier mask or the lookup misses.
                const int KeyModifierMask = CBinds::GetModifierMaskOfKey(Event.m_Key);
                const int ModifierMask = CBinds::GetModifierMask(Input()) & ~KeyModifierMask;
                const char *pBind = GameClient()->m_Binds.Get(Event.m_Key, ModifierMask);
                if(pBind && str_find(pBind, "cl_clickgui"))
                {
                        g_Config.m_ClClickGui = g_Config.m_ClClickGui ? 0 : 1;
                        return true;
                }
        }

        // v6.3: the ArrayList editor captures ALL mouse input, exactly like the
        // open ClickGUI (1:1) — nothing leaks into the game. The HUD is dragged
        // by holding it.
        const bool ArrayEditorActive = (m_ArrayListEditor && ShouldRenderArrayList()) || (m_ArrayInfoEditor && ShouldRenderArrayInfo()) || (m_ArrayPlayerEditor && ShouldRenderArrayPlayer());
        if(g_Config.m_ClClickGui == 0 && ArrayEditorActive && !GameClient()->m_GameConsole.IsActive())
        {
                const vec2 MP = Input()->NativeMousePos() / (m_UiScale > 0.0f ? m_UiScale : 1.0f);
                if((Event.m_Flags & IInput::FLAG_PRESS) && Event.m_Key == KEY_MOUSE_1)
                {
                        if(m_ArrayListEditor && ArrayListHitTest(MP))
                        {
                                m_ArrayListDragging = true;
                                m_ArrayListDragStartMouse = MP;
                                m_ArrayListDragStartPos = vec2(m_ArrayListRectX, m_ArrayListRectY);
                        }
                        if(m_ArrayInfoEditor && ArrayInfoHitTest(MP))
                        {
                                m_ArrayInfoDragging = true;
                                m_ArrayInfoDragStartMouse = MP;
                                m_ArrayInfoDragStartPos = vec2(m_ArrayInfoRectX, m_ArrayInfoRectY);
                        }
                        if(m_ArrayPlayerEditor && !m_ArrayPlayerDragging && !m_ArrayPlayerResizing)
                        {
                                if(ArrayPlayerResizeHitTest(MP))
                                {
                                        m_ArrayPlayerResizing = true;
                                        m_ArrayPlayerResizeStartMouse = MP;
                                        m_ArrayPlayerResizeStartW = g_Config.m_KxArrayPlayerWidth;
                                        m_ArrayPlayerResizeStartH = g_Config.m_KxArrayPlayerHeight;
                                }
                                else if(ArrayPlayerHitTest(MP))
                                {
                                        m_ArrayPlayerDragging = true;
                                        m_ArrayPlayerDragStartMouse = MP;
                                        m_ArrayPlayerDragStartPos = vec2(m_ArrayPlayerRectX, m_ArrayPlayerRectY);
                                }
                        }
                        return true;
                }
                if((Event.m_Flags & IInput::FLAG_RELEASE) && Event.m_Key == KEY_MOUSE_1)
                {
                        m_ArrayListDragging = false;
                        m_ArrayInfoDragging = false;
                        m_ArrayPlayerDragging = false;
                        m_ArrayPlayerResizing = false;
                        return true;
                }
                if(Event.m_Key >= KEY_MOUSE_1 && Event.m_Key <= KEY_MOUSE_8)
                        return true;
                if((Event.m_Flags & IInput::FLAG_PRESS) && (Event.m_Key == KEY_MOUSE_WHEEL_UP || Event.m_Key == KEY_MOUSE_WHEEL_DOWN))
                        return true;
        }

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
        // v6.3: the HUD editors own the cursor exactly like the open GUI.
        if((m_ArrayListEditor && ShouldRenderArrayList()) || (m_ArrayInfoEditor && ShouldRenderArrayInfo()) || (m_ArrayPlayerEditor && ShouldRenderArrayPlayer()))
                return true;
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
bool CClickGui::OnTouchState(std::vector<IInput::CTouchFingerState> &vTouchFingerStates)
{
        // v6.3: ArrayList editor — the ClickGUI is closed, but the HUD must stay
        // draggable and touches must not be redirected to the game. On desktop
        // (no touch fingers ever) the drag state must NOT be cleared here — the
        // mouse drag is owned by OnInput/OnRender; clearing it every frame made
        // the polling restart the drag at the current cursor position (zero net
        // movement, i.e. the HUD never moved).
        if((m_ArrayListEditor || m_ArrayInfoEditor || m_ArrayPlayerEditor) && g_Config.m_ClClickGui == 0 && m_AnimProgress <= 0.0f)
        {
                const bool HadTouch = m_TouchActive;
                m_TouchActive = !vTouchFingerStates.empty();
                if(!vTouchFingerStates.empty())
                {
                        const vec2 ScreenSize = vec2((float)Graphics()->ScreenWidth(), (float)Graphics()->ScreenHeight());
                        const vec2 FingerPos = vTouchFingerStates.front().m_Position;
                        m_MousePos = vec2(FingerPos.x * ScreenSize.x, FingerPos.y * ScreenSize.y) / (m_UiScale > 0.0f ? m_UiScale : 1.0f);
                        if(m_ArrayListEditor && !m_ArrayListDragging && ArrayListHitTest(m_MousePos))
                        {
                                m_ArrayListDragging = true;
                                m_ArrayListDragStartMouse = m_MousePos;
                                m_ArrayListDragStartPos = vec2(m_ArrayListRectX, m_ArrayListRectY);
                        }
                        if(m_ArrayInfoEditor && !m_ArrayInfoDragging && ArrayInfoHitTest(m_MousePos))
                        {
                                m_ArrayInfoDragging = true;
                                m_ArrayInfoDragStartMouse = m_MousePos;
                                m_ArrayInfoDragStartPos = vec2(m_ArrayInfoRectX, m_ArrayInfoRectY);
                        }
                        if(m_ArrayPlayerEditor && !m_ArrayPlayerDragging && !m_ArrayPlayerResizing)
                        {
                                if(ArrayPlayerResizeHitTest(m_MousePos))
                                {
                                        m_ArrayPlayerResizing = true;
                                        m_ArrayPlayerResizeStartMouse = m_MousePos;
                                        m_ArrayPlayerResizeStartW = g_Config.m_KxArrayPlayerWidth;
                                        m_ArrayPlayerResizeStartH = g_Config.m_KxArrayPlayerHeight;
                                }
                                else if(ArrayPlayerHitTest(m_MousePos))
                                {
                                        m_ArrayPlayerDragging = true;
                                        m_ArrayPlayerDragStartMouse = m_MousePos;
                                        m_ArrayPlayerDragStartPos = vec2(m_ArrayPlayerRectX, m_ArrayPlayerRectY);
                                }
                        }
                }
                else
                {
                        if(HadTouch)
                        {
                                m_ArrayListDragging = false;
                                m_ArrayInfoDragging = false;
                                m_ArrayPlayerDragging = false;
                                m_ArrayPlayerResizing = false;
                        }
                        m_TouchActive = false;
                }
                return true;
        }

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

        // v6.3: the ArrayList HUD is drawn UNDER the ClickGUI and is only
        // draggable in the editor (GUI closed), so clicks here go to the panels.

        // v6.7: ArrayPlayer media transport buttons — clickable with the GUI
        // open too (the card itself is not a ClickGUI panel).
        if(m_ArrayPlayerBtnRectValid && !m_ArrayPlayerEditor && ShouldRenderArrayPlayer())
        {
                for(int b = 0; b < 3; ++b)
                {
                        if(PointInRect(mousePos, m_aArrayPlayerBtnRect[b][0], m_aArrayPlayerBtnRect[b][1], m_aArrayPlayerBtnRect[b][2], m_aArrayPlayerBtnRect[b][3]))
                        {
                                KxMedia::Command(b);
                                return;
                        }
                }
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

                // v1.56.284: panel edge resize (Dynamic scale) — right/bottom
                // edges take priority over header drag and row clicks.
                if(g_Config.m_KxPanelsDynamicScale != 0 && g_Config.m_KxUiSettingsOn != 0)
                {
                        const float pw = g_PanelW;
                        const float ph = m_aPanelCurrentHeight[i];
                        const bool RightEdge = PointInRect(mousePos, ppos.x + pw - 7.0f, ppos.y, 7.0f, ph);
                        const bool BottomEdge = !m_aPanelCollapsed[i] &&
                                PointInRect(mousePos, ppos.x, ppos.y + ph - 7.0f, pw, 7.0f);
                        if(RightEdge || BottomEdge)
                        {
                                BringToFront(i);
                                m_ResizePanel = i;
                                m_ResizeEdge = (RightEdge ? 1 : 0) | (BottomEdge ? 2 : 0);
                                m_ResizeStartMouse = mousePos;
                                m_ResizeStartW = g_PanelW;
                                m_ResizeStartH = g_PanelMaxBodyH;
                                return;
                        }
                }

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
                                        IsVisualExpandableName(pTarget->pName);
                                         // v1.56.281: Future Limit is a master toggle now (click dispatch below).
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
                                        if(str_comp(pTarget->pName, "Helpers") == 0)
                                                g_Config.m_KxHelpers = pTarget->on;
                                        // v1.56.282: Dummy Manager — Profiles/Tee target toggles (ClickGUI-local).
                                        else if((str_comp(row.pName, "Dummy Profiles") == 0 || str_comp(row.pName, "Dummy Tee") == 0) && pTarget->type == ERowType::Toggle)
                                        {
                                                const int d = DummyMgrParseDummyName(pTarget->pName);
                                                if(d >= 0)
                                                {
                                                        if(str_comp(row.pName, "Dummy Tee") == 0)
                                                                m_aTeeTargets[d] = pTarget->on;
                                                        else
                                                                m_aProfileTargets[d] = pTarget->on;
                                                }
                                        }
                                        // v1.56.260: Visuals panel toggles.
                                        else if(str_comp(pTarget->pName, "Custom color tee") == 0)
                                                g_Config.m_KxRainbowTee = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom color legs") == 0)
                                                g_Config.m_KxRainbowLegs = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom color guns") == 0)
                                                g_Config.m_KxRainbowGuns = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom color hook") == 0)
                                                g_Config.m_KxRainbowHook = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom color HUD") == 0)
                                                g_Config.m_KxRainbowHud = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom color particles") == 0)
                                                g_Config.m_KxRainbowParticles = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom color emoticons") == 0)
                                                g_Config.m_KxRainbowEmoticons = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Hide particles") == 0)
                                                g_Config.m_KxHideParticles = pTarget->on ? 1 : 0;
                                        // v1.56.262: Visuals bypasses + Admin Alarm + Functions headers.
                                        else if(str_comp(pTarget->pName, "Bypass Zoom Lock") == 0)
                                                g_Config.m_KxZoomUnlock = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Bypass Asset Lock") == 0)
                                                g_Config.m_KxAssetUnlock = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Admin Alarm") == 0)
                                                g_Config.m_KxAdminAlarm = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Repeat") == 0 && str_comp(row.pName, "Admin Alarm") == 0)
                                        {
                                                // per level (Level dropdown selects which one is edited)
                                                if(g_Config.m_KxAdminAlarmLevel == 0)
                                                        g_Config.m_KxAdminAlarmRep1 = pTarget->on ? 1 : 0;
                                                else if(g_Config.m_KxAdminAlarmLevel == 1)
                                                        g_Config.m_KxAdminAlarmRep2 = pTarget->on ? 1 : 0;
                                                else
                                                        g_Config.m_KxAdminAlarmRep3 = pTarget->on ? 1 : 0;
                                        }
                                        else if(str_comp(pTarget->pName, "Auto Fire") == 0)
                                                g_Config.m_KxAutoFire = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "BalanceBot") == 0)
                                                g_Config.m_KxBalanceBot = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Drag-To-Sort") == 0)
                                                g_Config.m_KxDragToSort = pTarget->on ? 1 : 0;
                                        // v1.56.283: combo expandable headers + Dynamic scale.
                                        else if(str_comp(pTarget->pName, "ClickGUI Settings") == 0)
                                                g_Config.m_KxUiSettingsOn = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom colors") == 0)
                                                g_Config.m_KxCustomColors = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Dynamic scale (Beta)") == 0)
                                                g_Config.m_KxPanelsDynamicScale = pTarget->on ? 1 : 0;
                                        // v1.56.287: BG Blur toggle.
                                        else if(str_comp(pTarget->pName, "BG Blur") == 0)
                                                g_Config.m_KxUiBgBlur = pTarget->on ? 1 : 0;
                                        // v6.3: ArrayList master + rainbow + tooltips master.
                                        else if(str_comp(pTarget->pName, "ArrayList") == 0)
                                                g_Config.m_KxArrayListEnabled = pTarget->on ? 1 : 0;
                                        // v6.4: ArrayInfo master + info toggles.
                                        else if(str_comp(pTarget->pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoEnabled = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "ArrayPlayer") == 0)
                                                g_Config.m_KxArrayPlayerEnabled = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Kinetix Logo") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoLogo = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show FPS") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoShowFps = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Session Time") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoSessionTime = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Tee") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoTee = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Nickname") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoNickname = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Functions") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoFunctions = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Rainbow Text") == 0 && str_comp(row.pName, "ArrayList") == 0)
                                                g_Config.m_KxArrayListRainbow = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Rainbow Text") == 0 && str_comp(row.pName, "ArrayInfo") == 0)
                                                g_Config.m_KxArrayInfoRainbow = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Rainbow Text") == 0 && str_comp(row.pName, "ArrayPlayer") == 0)
                                                g_Config.m_KxArrayPlayerRainbow = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Dynamic Scale") == 0 && str_comp(row.pName, "ArrayPlayer") == 0)
                                                g_Config.m_KxArrayPlayerDynamicScale = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Show ToolTips") == 0)
                                                g_Config.m_KxUiShowTooltips = pTarget->on ? 1 : 0;
                                        // v1.56.283: Custom colors — per-selection Enable
                                        // (AimBot/TriggerBot "Enable" are parent-gated below).
                                        else if(str_comp(pTarget->pName, "Enable") == 0 && str_comp(row.pName, "Custom colors") == 0)
                                        {
                                                int SpriteIdx = 0, EntIdx = 0;
                                                CustomColorSelection(row, &SpriteIdx, &EntIdx);
                                                const SCustomColorCfg Cfg = CustomColorCfgFor(SpriteIdx, EntIdx);
                                                if(Cfg.pOn)
                                                        *Cfg.pOn = pTarget->on ? 1 : 0;
                                        }
                                        // v1.56.275: FNG Freeze — per-context (ninja counts as frozen in FNG).
                                        // v1.56.292: aimbot/triggerbot FNG Freeze edits the per-entity
                                        // block when the Hook weapon is selected.
                                        else if(str_comp(pTarget->pName, "FNG Freeze") == 0 && str_comp(row.pName, "AimBot") == 0)
                                                AbEditSettings(GameClient()->m_AimBot).m_FngFreeze = pTarget->on;
                                        else if(str_comp(pTarget->pName, "FNG Freeze") == 0 && str_comp(row.pName, "TriggerBot") == 0)
                                                TbEditSettings(GameClient()->m_AimBot).m_FngFreeze = pTarget->on;
                                        else if(str_comp(pTarget->pName, "FNG Freeze") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspFngFreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "FNG Freeze") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                g_Config.m_KxHitboxesFngFreeze = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "FNG Freeze") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                g_Config.m_KxBbFngFreeze = pTarget->on ? 1 : 0;
                                        // no weapons); the AimBot/TriggerBot ones are per weapon
                                        // in their own child blocks.
                                        else if(str_comp(pTarget->pName, "Off while freeze") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                g_Config.m_KxBbOffWhileFreeze = pTarget->on ? 1 : 0;
                                        // v1.56.261: Custom color entities master toggle.
                                        else if(str_comp(pTarget->pName, "Custom color entities") == 0)
                                                g_Config.m_KxRainbowEnt = pTarget->on ? 1 : 0;
                                        // v1.56.286: Search filter toggles (Search panel).
                                        else if(str_comp(pTarget->pName, "Case Sensitive") == 0 && str_comp(panel.pTitle, "Search") == 0)
                                                g_SearchCaseSensitive = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Whole Word") == 0 && str_comp(panel.pTitle, "Search") == 0)
                                                g_SearchWholeWord = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Use Regex") == 0 && str_comp(panel.pTitle, "Search") == 0)
                                                g_SearchRegex = pTarget->on;
                                        else if(str_comp(pTarget->pName, "Fuzzy Match") == 0 && str_comp(panel.pTitle, "Search") == 0)
                                                g_SearchFuzzy = pTarget->on;
                                        // v1.56.283: Custom colors — per-selection Rainbow anim toggle.
                                        else if(str_comp(pTarget->pName, "Rainbow") == 0 && str_comp(row.pName, "Custom colors") == 0)
                                        {
                                                int SpriteIdx = 0, EntIdx = 0;
                                                CustomColorSelection(row, &SpriteIdx, &EntIdx);
                                                const SCustomColorCfg Cfg = CustomColorCfgFor(SpriteIdx, EntIdx);
                                                if(Cfg.pOn)
                                                        *Cfg.pAnim = pTarget->on ? 1 : 0;
                                        }
                                        else if(str_comp(pTarget->pName, "Trajectory") == 0)
                                                g_Config.m_KxShowTrajectory = pTarget->on ? 1 : 0;
                                        // Advanced → Laser unfreeze expandable is master toggle.
                                        else if(str_comp(pTarget->pName, "Laser unfreeze") == 0)
                                                g_Config.m_KxLaserUnfreeze = pTarget->on ? 1 : 0;
					else if(str_comp(pTarget->pName, "Fake Aim") == 0 && str_comp(g_Panels[i].pTitle, "Functions") == 0)
						g_Config.m_KxFakeAim = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "ESP") == 0)
                                                g_Config.m_KxEsp = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Avoid Freeze") == 0)
                                                g_Config.m_KxAvoidFreeze = pTarget->on ? 1 : 0;
                                        // Functions panel — Fly/Copy Moves sub-section
                                        else if(str_comp(pTarget->pName, "Preserve input") == 0)
                                                g_Config.m_KxDummyHammerKeepInputs = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Custom latency") == 0)
                                                g_Config.m_KxDummyHammerCustomLatency = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Auto fly") == 0)
                                                g_Config.m_KxDummyHammerAuto = pTarget->on ? 1 : 0;
                                        else if(str_comp(pTarget->pName, "Fix Dummy Hammer") == 0)
                                                g_Config.m_KxFixDummyHammer = pTarget->on ? 1 : 0;
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
                                        // C1061: MSVC caps block nesting at 128 levels and this
                                        // dispatch chain outgrew it. Everything from the AimBot/
                                        // TriggerBot master toggles down lives in HandleToggleTail;
                                        // it runs as this chain's final else-if, so the
                                        // first-match-wins order of the original chain is unchanged.
                                        else if(HandleToggleTail(pTarget, &row, &panel))
                                        {
                                        }
                                        return;
                                }
                        }
                        if(pTarget->type == ERowType::Button || pTarget->type == ERowType::DoubleButton)
                        {
                                // v6.3: ArrayList Editor / Reset. The right half (Reset)
                                // only exists once the list position was moved.
                                if(str_comp(pTarget->pName, "Editor") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        const int Kind = (str_comp(row.pName, "ArrayInfo") == 0) ? 1 : (str_comp(row.pName, "ArrayPlayer") == 0) ? 2 : 0;
                                        const bool Modified = Kind == 1 ? ArrayInfoPositionModified() : Kind == 2 ? ArrayPlayerPositionModified() : ArrayListPositionModified();
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(Modified && mousePos.x >= rx + 2.0f + btnW)
                                        {
                                                if(Kind == 1)
                                                        ResetArrayInfoPosition();
                                                else if(Kind == 2)
                                                        ResetArrayPlayerPosition();
                                                else
                                                        ResetArrayListPosition();
                                        }
                                        else
                                        {
                                                bool &EditorRef = Kind == 1 ? m_ArrayInfoEditor : Kind == 2 ? m_ArrayPlayerEditor : m_ArrayListEditor;
                                                EditorRef = !EditorRef;
                                                if(EditorRef)
                                                {
                                                        if(Kind != 0)
                                                                m_ArrayListEditor = false;
                                                        if(Kind != 1)
                                                                m_ArrayInfoEditor = false;
                                                        if(Kind != 2)
                                                                m_ArrayPlayerEditor = false;
                                                        g_Config.m_ClClickGui = 0;
                                                }
                                        }
                                        return;
                                }
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
                                // v1.56.280: RT Pathfinding button — IDLE <-> RUNNING (no FINISHED state).
                                else if(str_comp(pTarget->pName, "RT Pathfinding") == 0 && pTarget->type == ERowType::Button)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        if(BN.m_RtPfState == 0)
                                        {
                                                BN.m_RtPfState = 1;
                                                BN.RtPfResetRun();
                                        }
                                        else
                                        {
                                                BN.RtPfStop();
                                        }
                                }
                                // RT tile editors — SEPARATE storage from the Pathfinder editors.
                                else if(str_comp(pTarget->pName, "RT Editor Forbidden Tiles") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(!BN.m_RtPfForbiddenTiles.empty() && mousePos.x >= rx + 2.0f + btnW)
                                                BN.RtPfClearForbiddenTiles();
                                        else
                                        {
                                                BN.m_RtPfEditorMode = (BN.m_RtPfEditorMode == 1) ? 0 : 1;
                                                if(BN.m_RtPfEditorMode != 0 && !BN.m_MapGridLoaded)
                                                        BN.LoadMapGrid();
                                        }
                                }
                                else if(str_comp(pTarget->pName, "RT Editor Finish Tiles") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        CBotNet &BN = GameClient()->m_BotNet;
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(!BN.m_RtPfCustomFinishTiles.empty() && mousePos.x >= rx + 2.0f + btnW)
                                                BN.RtPfClearCustomFinishTiles();
                                        else
                                        {
                                                BN.m_RtPfEditorMode = (BN.m_RtPfEditorMode == 2) ? 0 : 2;
                                                if(BN.m_RtPfEditorMode != 0 && !BN.m_MapGridLoaded)
                                                        BN.LoadMapGrid();
                                        }
                                }
                                // TAS panel buttons.
                                else if(pTarget->pName == g_TasDyn_Action)
                                {
                                        CTas &Tas = GameClient()->m_Tas;
                                        // v6.3: right half = "Reset Record" (shown only when
                                        // the take buffer holds something or a file is loaded).
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(Tas.RecordDirty() && pTarget->type == ERowType::DoubleButton && mousePos.x >= rx + 2.0f + btnW)
                                                Tas.ResetRecord();
                                        else if(Tas.Mode() == 1)
                                                Tas.ToggleRecord();
                                        else
                                                Tas.TogglePlay();
                                }
                                else if(str_comp(row.pName, "Spawner") == 0 && (pTarget->type == ERowType::Button || pTarget->type == ERowType::DoubleButton))
                                {
                                        CTas &Tas = GameClient()->m_Tas;
                                        SyncTasSpawnerRows(Tas);
                                        if(str_comp(pTarget->pName, "Spawn") == 0)
                                                Tas.SpawnerSpawn();
                                        else if(str_comp(pTarget->pName, "Kill") == 0 && pTarget->type == ERowType::DoubleButton)
                                        {
                                                const float btnW = (rw - 6.0f) * 0.5f;
                                                if(mousePos.x >= rx + 2.0f + btnW)
                                                        Tas.SpawnerKillAll();
                                                else
                                                        Tas.SpawnerKill();
                                        }
                                        else if(str_comp(pTarget->pName, g_TasDyn_Loop) == 0)
                                                Tas.SpawnerToggleLoop();
                                }
                                else if(pTarget->pName == g_TasDyn_Join)
                                {
                                        CTas &Tas = GameClient()->m_Tas;
                                        // v6.2: the right half is "Reset Playground",
                                        // shown only while joined and something exists to drop.
                                        const bool ResetVisible = Tas.PlaygroundDirty();
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(ResetVisible && pTarget->type == ERowType::DoubleButton && mousePos.x >= rx + 2.0f + btnW)
                                                Tas.PlaygroundReset();
                                        else
                                                Tas.JoinLeavePlayground();
                                }
                                // v6.2: Rewind | Forward in one row.
                                else if(str_comp(pTarget->pName, "Rewind") == 0 && pTarget->type == ERowType::DoubleButton)
                                {
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        if(mousePos.x >= rx + 2.0f + btnW)
                                                GameClient()->m_Tas.StepForward();
                                        else
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
                                                char aTasDir[1024];
                                                Storage()->GetCompletePath(IStorage::TYPE_SAVE, "Kinetix/TAS", aTasDir, sizeof(aTasDir));
                                                char aTasPath[1024];
                                                if(KxPickTasFile(aTasPath, sizeof(aTasPath), aTasDir))
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
                                // v1.56.282: Dummy Manager — profile/tee buttons open the DDNet
                                // settings in a per-dummy edit session (CMenus::BeginDummyEdit).
                                else if(pTarget->type == ERowType::Button && (str_comp(row.pName, "Dummy Profiles") == 0 || str_comp(row.pName, "Dummy Tee") == 0))
                                {
                                        const bool Tee = str_comp(row.pName, "Dummy Tee") == 0;
                                        if(str_comp(pTarget->pName, "Open Main Profile") == 0 || str_comp(pTarget->pName, "Open Main Tee") == 0)
                                        {
                                                // Main = vanilla player profile — end any active dummy
                                                // session first, otherwise the tab stays on "Dummy N".
                                                g_Config.m_ClClickGui = 0;
                                                GameClient()->m_Menus.EndDummyEdit();
                                                GameClient()->m_Menus.OpenDummySettingsPage(Tee);
                                        }
                                        else if(str_comp(pTarget->pName, "Open All Profile") == 0 || str_comp(pTarget->pName, "Open All Tee") == 0)
                                        {
                                                g_Config.m_ClClickGui = 0;
                                                GameClient()->m_Menus.BeginDummyEdit(1, 1, Tee, nullptr);
                                                GameClient()->m_Menus.OpenDummySettingsPage(Tee);
                                        }
                                        else if(str_comp(pTarget->pName, "Open Targets Profile") == 0 || str_comp(pTarget->pName, "Open Targets Tee") == 0)
                                        {
                                                g_Config.m_ClClickGui = 0;
                                                GameClient()->m_Menus.BeginDummyEdit(1, 2, Tee, Tee ? m_aTeeTargets : m_aProfileTargets);
                                                GameClient()->m_Menus.OpenDummySettingsPage(Tee);
                                        }
                                        else if(DummyMgrParseOpenName(pTarget->pName) >= 1)
                                        {
                                                // "Open Dummy %d Profile"/"Open Dummy %d Tee" — per-dummy session.
                                                g_Config.m_ClClickGui = 0;
                                                GameClient()->m_Menus.BeginDummyEdit(DummyMgrParseOpenName(pTarget->pName), 0, Tee, nullptr);
                                                GameClient()->m_Menus.OpenDummySettingsPage(Tee);
                                        }
                                        return;
                                }
                                // v1.56.282: Dummy Manager — apply staged proxies and reconnect.
                                else if(pTarget->type == ERowType::Button && str_comp(pTarget->pName, "Reconnect (Apply)") == 0)
                                {
                                        ApplyProxiesAndReconnect();
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
                                        const int d = DummyMgrParseD(pTarget->pName);
                                        if(d < 1)
                                                return;
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
                                                        if(d >= 1 && d < MAX_DUMMIES)
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
                                // TPS row: the right edge is the Pause toggle, not the slider.
                                if(str_comp(pTarget->pName, "TPS") == 0 && mousePos.x >= rx + rw - KX_TPS_TOGGLE_W)
                                {
                                        pTarget->on = !pTarget->on;
                                        GameClient()->m_Tas.SetPaused(pTarget->on);
                                        return;
                                }
                                // Start slider drag.
                                m_DragSliderPanel = i;
                                m_DragSliderRow = outRow;
                                m_DragSliderChild = outChild;
                                const float trackX = rx + 2.0f;
                                const float trackW = KxTpsTrackW(pTarget, rw);
                                float pct;
                                if(str_comp(pTarget->pName, "TPS") == 0)
                                        pct = (mousePos.x - trackX - SLIDER_THUMB_W * 0.5f) / (trackW - SLIDER_THUMB_W);
                                else
                                        pct = (mousePos.x - trackX) / trackW;
                                pct = std::clamp(pct, 0.0f, 1.0f);
                                {
                                        float raw = pTarget->min + pct * (pTarget->max - pTarget->min);
                                        if(pTarget->step >= 1.0f)
                                                pTarget->value = pTarget->min + roundf((raw - pTarget->min) / pTarget->step) * pTarget->step;
                                        else
                                                pTarget->value = raw;
                                }
                                // v6.3: ArrayList HUD + ToolTips sliders (parent-gated:
                                // "Speed"/"Step"/"Main Opacity" are shared names).
                                if(str_comp(row.pName, "ArrayList") == 0)
                                {
                                        if(str_comp(pTarget->pName, "Indentation") == 0)
                                                g_Config.m_KxArrayListIndent = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "ArrayList Size") == 0)
                                                g_Config.m_KxArrayListSize = (int)(pTarget->value * 10.0f + 0.5f);
                                        else if(str_comp(pTarget->pName, "Max Length") == 0)
                                                g_Config.m_KxArrayListMaxLength = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Speed") == 0)
                                                g_Config.m_KxArrayListRainbowSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                                        else if(str_comp(pTarget->pName, "Step") == 0)
                                                g_Config.m_KxArrayListRainbowStep = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Text Opacity") == 0)
                                                g_Config.m_KxArrayListTextOpacity = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                                g_Config.m_KxArrayListMainOpacity = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                                g_Config.m_KxArrayListPanelOpacity = (int)(pTarget->value + 0.5f);
                                }
                                else if(str_comp(row.pName, "ArrayInfo") == 0)
                                {
                                        if(str_comp(pTarget->pName, "Max Length") == 0)
                                                g_Config.m_KxArrayInfoMaxLength = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "ArrayInfo Size") == 0)
                                                g_Config.m_KxArrayInfoSize = (int)(pTarget->value * 10.0f + 0.5f);
                                        else if(str_comp(pTarget->pName, "Indentation") == 0)
                                                g_Config.m_KxArrayInfoIndent = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Speed") == 0)
                                                g_Config.m_KxArrayInfoRainbowSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                                        else if(str_comp(pTarget->pName, "Step") == 0)
                                                g_Config.m_KxArrayInfoRainbowStep = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Text Opacity") == 0)
                                                g_Config.m_KxArrayInfoTextOpacity = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                                g_Config.m_KxArrayInfoMainOpacity = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                                g_Config.m_KxArrayInfoPanelOpacity = (int)(pTarget->value + 0.5f);
                                }
                                else if(str_comp(row.pName, "ArrayPlayer") == 0)
                                {
                                        if(str_comp(pTarget->pName, "Indentation") == 0)
                                                g_Config.m_KxArrayPlayerIndent = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "ArrayPlayer Size") == 0)
                                                g_Config.m_KxArrayPlayerSize = (int)(pTarget->value * 10.0f + 0.5f);
                                        else if(str_comp(pTarget->pName, "Height ArrayPlayer") == 0)
                                                g_Config.m_KxArrayPlayerHeight = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Weight ArrayPlayer") == 0)
                                                g_Config.m_KxArrayPlayerWidth = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Speed") == 0)
                                                g_Config.m_KxArrayPlayerRainbowSpeed = (int)(pTarget->value * 10.0f + 0.5f);
                                        else if(str_comp(pTarget->pName, "Step") == 0)
                                                g_Config.m_KxArrayPlayerRainbowStep = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Text Opacity") == 0)
                                                g_Config.m_KxArrayPlayerTextOpacity = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Main Opacity") == 0)
                                                g_Config.m_KxArrayPlayerMainOpacity = (int)(pTarget->value + 0.5f);
                                        else if(str_comp(pTarget->pName, "Panel Opacity") == 0)
                                                g_Config.m_KxArrayPlayerPanelOpacity = (int)(pTarget->value + 0.5f);
                                }
                                else if(str_comp(row.pName, "ClickGUI Settings") == 0 && str_comp(pTarget->pName, "ToolTips Text Opacity") == 0)
                                        g_Config.m_KxUiTooltipTextOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(row.pName, "ClickGUI Settings") == 0 && str_comp(pTarget->pName, "ToolTips Main Opacity") == 0)
                                        g_Config.m_KxUiTooltipMainOpacity = (int)(pTarget->value + 0.5f);
                                else if(str_comp(row.pName, "ClickGUI Settings") == 0 && str_comp(pTarget->pName, "ToolTips Panel Opacity") == 0)
                                        g_Config.m_KxUiTooltipPanelOpacity = (int)(pTarget->value + 0.5f);
                                // v1.56.282: Dummy Manager — "Dummies" slider sets the runtime dummy limit.
                                else if(str_comp(pTarget->pName, "Dummies") == 0)
                                        Client()->SetDummyLimit((int)pTarget->value);
                                else if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Custom latency") == 0)
                                        g_Config.m_KxDummyHammerLatencyMs = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Latency (ms)") == 0 && str_comp(row.pName, "Copy Moves Latency") == 0)
                                        g_Config.m_KxCopyMovesLatency = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ratio") == 0 && str_comp(row.pName, "Freedom Path") == 0)
                                        g_Config.m_KxPfFreedomRatio = (int)(pTarget->value * 10.0f + 0.5f);
                                // v6.7: Smart Copy Moves top-N slider.
                                else if(str_comp(pTarget->pName, "Apply Dummies") == 0)
                                        g_Config.m_KxSmartCopyCount = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                        g_Config.m_KxTripleFlyTriggerRadius = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Radius") == 0 && str_comp(row.pName, "Auto Triple Fly") == 0)
                                        g_Config.m_KxTripleFlyRadius = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Edge in ms") == 0 && str_comp(row.pName, "Edge bind") == 0)
                                        g_Config.m_KxEdgeBindMs = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Version ID") == 0)
                                        g_Config.m_KxVersionSpoofId = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Fake Zoom") == 0)
                                        g_Config.m_KxSpoofZoomValue = (int)(pTarget->value * 10.0f + 0.5f);
                                else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeFov = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Trigger ticks") == 0 && str_comp(row.pName, "Laser unfreeze") == 0)
                                        g_Config.m_KxLaserUnfreezeTriggerTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                        g_Config.m_KxAfTicks = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Ticks Before") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                        g_Config.m_KxAfTicksBefore = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Angles") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                        g_Config.m_KxAfAngles = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "FOV") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                                        g_Config.m_KxAfFov = (int)pTarget->value;
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
								case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderSize = (int)pTarget->value; break;
                                                case KX_LINE_ESP: g_Config.m_KxLineEspSize = (int)pTarget->value; break;
                                                case KX_LINE_TAS: g_Config.m_KxLineTasSize = (int)pTarget->value; break;
                                         case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesSize = (int)pTarget->value; break;
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
								case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderAlpha = (int)pTarget->value; break;
                                                case KX_LINE_ESP: g_Config.m_KxLineEspAlpha = (int)pTarget->value; break;
                                                case KX_LINE_TAS: g_Config.m_KxLineTasAlpha = (int)pTarget->value; break;
                                         case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesAlpha = (int)pTarget->value; break;
                                                }
                                                }
                                }
                                else if(str_comp(pTarget->pName, "Layer") == 0)
                                        g_Config.m_KxLineRenderingLayer = (int)pTarget->value;
                                 else if(str_comp(pTarget->pName, "X") == 0 && str_comp(row.pName, "ESP") == 0)
                                         g_Config.m_KxEspScreenX = (int)pTarget->value;
                                 else if(str_comp(pTarget->pName, "Y") == 0 && str_comp(row.pName, "ESP") == 0)
                                         g_Config.m_KxEspScreenY = (int)pTarget->value;
                                 else if(str_comp(pTarget->pName, "Pointer Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                         g_Config.m_KxEspSpeed = (int)pTarget->value;
                                 else if(str_comp(pTarget->pName, "Box Speed") == 0 && str_comp(row.pName, "ESP") == 0)
                                         g_Config.m_KxEspBoxSpeed = (int)pTarget->value;
                                 // v1.56.256: ESP box scale sliders.
                                 else if(str_comp(pTarget->pName, "X Box scale") == 0 && str_comp(row.pName, "ESP") == 0)
                                         g_Config.m_KxEspBoxScaleX = (int)pTarget->value;
                                 else if(str_comp(pTarget->pName, "Y Box scale") == 0 && str_comp(row.pName, "ESP") == 0)
                                         g_Config.m_KxEspBoxScaleY = (int)pTarget->value;
                                 // v1.56.256: Hitboxes "Box Speed" sliders — same label, two rows (by index).
                                 else if(str_comp(pTarget->pName, "Box Speed") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                 {
                                         if(outChild == 2)
                                                 g_Config.m_KxHitboxesTileSpeed = (int)pTarget->value;
                                         else if(outChild == 5)
                                                 g_Config.m_KxHitboxesTriggerSpeed = (int)pTarget->value;
                                 }
                                // v1.56.178: Trajectory per-type slider (drag path) — writes to CTrajectory.m_aTypes[sel].
                                else if(str_comp(pTarget->pName, "Prediction Ticks") == 0 && str_comp(row.pName, "Trajectory") == 0)
                                {
                                        CTrajectory &traj = GameClient()->m_Trajectory;
                                        int sel = traj.m_SelectedType;
                                        if(sel >= 0 && sel < CTrajectory::NUM_TRAJ_TYPES)
                                                traj.m_aTypes[sel].m_PredictionTicks = (int)pTarget->value;
                                }
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
                                // v1.56.280: RT Pathfinder sliders (commit on click)
                                else if(str_comp(pTarget->pName, "Reconstruct Trigger") == 0)
                                        g_Config.m_KxRtPfTrigger = (int)pTarget->value;
                                else if(str_comp(pTarget->pName, "Future Ticks") == 0)
                                        g_Config.m_KxRtPfFutureTicks = (int)pTarget->value;
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
                        if(pTarget->type == ERowType::ToggleInput) // v1.56.282: Dummy Manager proxy rows
                        {
                                const float inW = rw / 2.0f;
                                const float inX = rx + rw - inW - 2.0f;
                                const float inY = ry + 2.0f;
                                const float inH = rh - 4.0f;
                                if(PointInRect(mousePos, inX, inY, inW, inH))
                                {
                                        // Click on the input box → start editing (same as Input).
                                        if(!pTarget->m_InputInitialized)
                                        {
                                                if(pTarget->pInputValue)
                                                        str_copy(pTarget->m_aInputBuf, pTarget->pInputValue, sizeof(pTarget->m_aInputBuf));
                                                pTarget->m_InputInitialized = true;
                                        }
                                        m_InputEditing = true;
                                        m_InputPanel = i;
                                        m_InputRow = outRow;
                                        m_InputChild = outChild;
                                        m_InputLen = (int)str_length(pTarget->m_aInputBuf);
                                        if(m_InputLen >= INPUT_BUF_SIZE)
                                                m_InputLen = INPUT_BUF_SIZE - 1;
                                        mem_copy(m_InputBuf, pTarget->m_aInputBuf, m_InputLen);
                                        m_InputBuf[m_InputLen] = 0;
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
                                        Input()->StartTextInput();
                                        return;
                                }
                                // Left part (checkbox + name) → toggle the proxy for this slot.
                                if(mousePos.x < inX)
                                {
                                        pTarget->on = !pTarget->on;
                                        const int D = (int)(pTarget - g_DummyProxyChildren);
                                        if(D >= 0 && D < MAX_DUMMIES)
                                        {
                                                GameClient()->m_BotNet.m_Dummy.m_aProfiles[D].m_ProxyOn = pTarget->on ? 1 : 0;
                                                GameClient()->m_BotNet.m_Dummy.Save();
                                                m_ProxyDirty = true;
                                        }
                                }
                                return;
                        }
                        if(pTarget->type == ERowType::Input)
                        {
                                // Click on the input box → start editing. Geometry
                                // must match the render: fixed width, right-aligned.
                                const float inW = (PANEL_WIDTH - BODY_PADDING * 2) * 0.5f;
                                const float inX = rx + rw - inW - 2.0f;
                                const float inY = ry + 2.0f;
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

// C1061 (MSVC 128-level block-nesting limit): tail of the toggle dispatch
// chain in HandleMouseDown — everything from the AimBot/TriggerBot master
// toggles down. It must stay the chain's FINAL else-if: the row-gated child
// blocks at its top (row == "AimBot"/"TriggerBot"/...) may only run when no
// earlier plain-name branch of the head chain matched. Returns true when a
// branch matched. pTargetV/pRowV/pPanelV are SRow*/SRow*/SPanel* — void*
// like ApplyInputCommit because those types are invisible to the header.
bool CClickGui::HandleToggleTail(void *pTargetV, void *pRowV, void *pPanelV)
{
        SRow *pTarget = (SRow *)pTargetV;
        SRow &row = *(SRow *)pRowV;
        SPanel &panel = *(SPanel *)pPanelV;
        // v1.56.83: AimBot/TriggerBot expandable master toggles
        if(str_comp(pTarget->pName, "AimBot") == 0)
                g_Config.m_KxAimBot = pTarget->on;
        else if(str_comp(pTarget->pName, "TriggerBot") == 0)
                g_Config.m_KxTriggerBot = pTarget->on;
        // v1.56.83: AimBot per-weapon child toggles (parent = "AimBot")
        else if(str_comp(row.pName, "AimBot") == 0)
        {
                CAimBot &ab = GameClient()->m_AimBot;
                {
                        auto &s = AbEditSettings(ab);
                        if(str_comp(pTarget->pName, "Enable") == 0)
                            s.m_Enabled = pTarget->on;
                        else if(str_comp(pTarget->pName, "Show FOV") == 0)
                            ab.m_AimBotShowFov = pTarget->on;
                        else if(str_comp(pTarget->pName, "Show Radius") == 0)
                            ab.m_AimBotShowRadius = pTarget->on;
                        else if(str_comp(pTarget->pName, "Auto Switch Weapon") == 0)
                            s.m_AutoSwitchWeapon = pTarget->on;
                        else if(str_comp(pTarget->pName, "Use raycast (Show)") == 0)
                            s.m_UseRaycastShow = pTarget->on;
                        else if(str_comp(pTarget->pName, "Use raycast angle (AB)") == 0)
                            s.m_UseAngle = pTarget->on;
                        else if(str_comp(pTarget->pName, "Predict") == 0)
                            s.m_Predict = pTarget->on;
                        else if(str_comp(pTarget->pName, "Silent") == 0)
                        {
                            s.m_Silent = pTarget->on;
                            // "Trigger" is hidden while Silent is on — remap.
                            if(s.m_Silent && s.m_AimMode == 1)
                                s.m_AimMode = 0;
                        }
                        else if(str_comp(pTarget->pName, "Center snap") == 0)
                            s.m_SmoothCenterSnap = pTarget->on;
                        else if(str_comp(pTarget->pName, "Off while freeze") == 0)
                            s.m_OffWhileFreeze = pTarget->on;
                }
        }
        // v1.56.83: TriggerBot per-weapon child toggles (parent = "TriggerBot")
        else if(str_comp(row.pName, "TriggerBot") == 0)
        {
                CAimBot &tb = GameClient()->m_AimBot;
                {
                        auto &s = TbEditSettings(tb);
                        if(str_comp(pTarget->pName, "Enable") == 0)
                            s.m_Enabled = pTarget->on;
                        else if(str_comp(pTarget->pName, "Show FOV") == 0)
                            tb.m_TriggerBotShowFov = pTarget->on;
                        else if(str_comp(pTarget->pName, "Show Radius") == 0)
                            tb.m_TriggerBotShowRadius = pTarget->on;
                        else if(str_comp(pTarget->pName, "Auto Switch Weapon") == 0)
                            s.m_AutoSwitchWeapon = pTarget->on;
                        else if(str_comp(pTarget->pName, "Use raycast (Show)") == 0)
                            s.m_UseRaycastShow = pTarget->on;
                        else if(str_comp(pTarget->pName, "Use silent channel AB") == 0)
                            s.m_UseSilentChannelAB = pTarget->on;
                        else if(str_comp(pTarget->pName, "Off while freeze") == 0)
                            s.m_OffWhileFreeze = pTarget->on;
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
        // v1.56.258: Block children toggles.
        else if(str_comp(row.pName, "Helpers") == 0 && pTarget != &row)
        {
                if(str_comp(pTarget->pName, "Auto Main") == 0) g_Config.m_KxAutoMain = pTarget->on;
                else if(str_comp(pTarget->pName, "All Target") == 0) g_Config.m_KxTargetAll = pTarget->on;
                else if(str_comp(pTarget->pName, "Rescue") == 0) g_Config.m_KxRescue = pTarget->on;
                else if(str_comp(pTarget->pName, "All Rescue") == 0) g_Config.m_KxRescueAll = pTarget->on;
        }
        // Copy moves Filter expandable is master toggle.
        else if(str_comp(pTarget->pName, "Copy Moves Filter") == 0)
                g_Config.m_KxDummyCopyMovesFilter = pTarget->on ? 1 : 0;
        // v6.7: Smart Copy Moves expandable is master toggle.
        else if(str_comp(pTarget->pName, "Smart Copy Moves") == 0)
                g_Config.m_KxSmartCopyMoves = pTarget->on ? 1 : 0;
        // v6.7: Smart Dummy Switch expandable is master toggle.
        else if(str_comp(pTarget->pName, "Smart dummy switch") == 0)
                g_Config.m_KxSdsEnabled = pTarget->on ? 1 : 0;
        // v6.7: Smart Dummy Switch Without Repeat (clear the visited round).
        else if(str_comp(pTarget->pName, "Without Repeat") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
        {
                g_Config.m_KxSdsNoRepeat = pTarget->on ? 1 : 0;
                GameClient()->m_BotNet.SdsResetVisited();
        }
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
        else if(str_comp(pTarget->pName, "Freeze") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfAvoidFreeze = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Teleport") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfAvoidTeleport = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Death") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfAvoidDeath = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Direction") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfDirection = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Jump") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfJump = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Hook") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfHook = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Aim") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfAim = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Silent") == 0 && str_comp(row.pName, "Avoid Freeze") == 0)
                g_Config.m_KxAfSilent = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Show for me") == 0)
                g_Config.m_KxFakeAimShowForMe = pTarget->on ? 1 : 0;
        // v1.56.262: Fake Aim — Show Server-Side Aim.
        else if(str_comp(pTarget->pName, "Show Server-Side Aim") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                g_Config.m_KxFakeAimShowServerSide = pTarget->on ? 1 : 0;
        // v1.56.263: Fake Aim — Predict.
        else if(str_comp(pTarget->pName, "Predict") == 0 && str_comp(row.pName, "Fake Aim") == 0)
                g_Config.m_KxFakeAimPredict = pTarget->on ? 1 : 0;
        // v1.56.256: ESP pointer/box visibility toggles.
        else if(str_comp(pTarget->pName, "Pointer") == 0 && str_comp(row.pName, "ESP") == 0)
                g_Config.m_KxEspPointer = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Box") == 0 && str_comp(row.pName, "ESP") == 0)
                g_Config.m_KxEspBox = pTarget->on ? 1 : 0;
        // v1.56.256: Hitboxes toggles.
        else if(str_comp(pTarget->pName, "Tee tile") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                g_Config.m_KxHitboxesTeeTile = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Tee trigger") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                g_Config.m_KxHitboxesTeeTrigger = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Show for others") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                g_Config.m_KxHitboxesShowOthers = pTarget->on ? 1 : 0;
        // v1.56.257: Hitboxes expandable header = master toggle (like IRC/Spoofer).
        else if(str_comp(pTarget->pName, "Hitboxes") == 0)
                g_Config.m_KxHitboxes = pTarget->on ? 1 : 0;
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
        // v1.56.280: RT Pathfinder top-level toggle
        else if(str_comp(pTarget->pName, "Reconstruct") == 0)
                g_Config.m_KxRtPfReconstruct = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Future Limit") == 0)
                g_Config.m_KxRtPfFutureLimit = pTarget->on ? 1 : 0;
        // RT Pathfinder Visuals children (panel-gated: TAS/Pathfinder
        // use overlapping child labels — generic names are safe here).
        else if(str_comp(row.pName, "Visuals") == 0 && str_comp(panel.pTitle, "Real-Time Pathfinder") == 0)
        {
                if(str_comp(pTarget->pName, "Show Path") == 0)
                        g_Config.m_KxRtPfShowPath = pTarget->on ? 1 : 0;
                else if(str_comp(pTarget->pName, "Show Hooks") == 0)
                        g_Config.m_KxRtPfShowHooks = pTarget->on ? 1 : 0;
                else if(str_comp(pTarget->pName, "Show Field") == 0)
                        g_Config.m_KxRtPfShowField = pTarget->on ? 1 : 0;
                else if(str_comp(pTarget->pName, "Show Previous Path") == 0)
                        g_Config.m_KxRtPfShowPrevPath = pTarget->on ? 1 : 0;
                else if(str_comp(pTarget->pName, "Show Speed") == 0)
                        g_Config.m_KxRtPfShowSpeed = pTarget->on ? 1 : 0;
        }
        // Settings → IRC/Spoofer expandables are master toggles.
        else if(str_comp(pTarget->pName, "IRC") == 0)
                g_Config.m_KxIrcEnabled = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Spoofer") == 0)
        {
                g_Config.m_KxVersionSpoof = pTarget->on ? 1 : 0;
                g_Config.m_KxSpoofClientStr = pTarget->on ? 1 : 0;
        }
        // v1.56.273: Spoof Zoom / Spoof Spectate headers = master toggles.
        else if(str_comp(pTarget->pName, "Spoof Zoom") == 0)
                g_Config.m_KxSpoofZoom = pTarget->on ? 1 : 0;
        else if(str_comp(pTarget->pName, "Spoof Spectate") == 0)
                g_Config.m_KxSpoofSpectate = pTarget->on ? 1 : 0;
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
        else if(str_comp(pTarget->pName, "Rainbow Main Color") == 0)
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
                case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderRainbow = pTarget->on ? 1 : 0; break;
                case KX_LINE_ESP: g_Config.m_KxLineEspRainbow = pTarget->on ? 1 : 0; break;
                case KX_LINE_TAS: g_Config.m_KxLineTasRainbow = pTarget->on ? 1 : 0; break;
                case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesRainbow = pTarget->on ? 1 : 0; break;
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
                case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderGradient = pTarget->on ? 1 : 0; break;
                case KX_LINE_ESP: g_Config.m_KxLineEspGradient = pTarget->on ? 1 : 0; break;
                case KX_LINE_TAS: g_Config.m_KxLineTasGradient = pTarget->on ? 1 : 0; break;
                case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesGradient = pTarget->on ? 1 : 0; break;
                }
        }
        else
                return false;
        return true;
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
                        // v1.56.260: Panels Start X/Y commit on slider release.
                        else if(str_comp(pTarget->pName, "Panels Start X") == 0)
                                g_Config.m_KxPanelsStartX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels Start Y") == 0)
                                g_Config.m_KxPanelsStartY = (int)pTarget->value;
                        // v1.56.283: Panels indentation + width/height commit.
                        else if(str_comp(pTarget->pName, "Panels X-indentation") == 0)
                                g_Config.m_KxPanelsIndentX = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels Y-indentation") == 0)
                                g_Config.m_KxPanelsIndentY = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels width") == 0)
                                g_Config.m_KxPanelsWidth = (int)pTarget->value;
                        else if(str_comp(pTarget->pName, "Panels height") == 0)
                                g_Config.m_KxPanelsHeight = (int)pTarget->value;
                }
        }
        m_Dragging = false;
        m_DragPanelIdx = -1;
        m_ArrayListDragging = false;
        m_DragSliderPanel = -1;
        m_DragSliderRow = -1;
        m_DragSliderChild = -1;
        m_ResizePanel = -1;
        m_ResizeEdge = 0;
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
                        // v1.56.284: write to the smooth-scroll target; the stored value
                        // approaches it exponentially each frame (inertia).
                        m_aPanelScrollTarget[i] += dir;
                        if(m_aPanelScrollTarget[i] < 0.0f)
                                m_aPanelScrollTarget[i] = 0.0f;
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
                const float rh = RowHeightForRow(row);
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
                                        // v1.56.273: Spoof Spectate mode.
                                        else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "Spoof Spectate") == 0)
                                                g_Config.m_KxSpoofSpectateMode = o;
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
					else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0 && str_comp(panel.pTitle, "Functions") == 0)
						g_Config.m_KxFakeAimMode = o;
                                        // v1.56.254: ESP dropdowns — renamed, parent-gated to "ESP";
                                        // Fake Aim "Mode" above is now row-gated so the renamed ESP
                                        // "Mode" cannot fall into it.
                                        else if(str_comp(target.pName, "Team") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspTeamFilter = o;
                                        // v1.56.275: ESP DDrace Team.
                                        else if(str_comp(target.pName, "DDrace Team") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspDDraceTeam = o;
                                        else if(str_comp(target.pName, "Friend") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspFriendFilter = o;
                                        else if(str_comp(target.pName, "Dummy") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspDummyFilter = o;
                                        else if(str_comp(target.pName, "Clanmate") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspClanmateFilter = o;
                                        else if(str_comp(target.pName, "Freeze") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspFreezeFilter = o;
                                        else if(str_comp(target.pName, "Targets") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspTargetsFilterMode = o;
                                        else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspMode = o;
                                        else if(str_comp(target.pName, "Pointer Style") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspStyle = o;
                                        else if(str_comp(target.pName, "Box Style") == 0 && str_comp(row.pName, "ESP") == 0)
                                                g_Config.m_KxEspBoxStyle = o;
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
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                const float crh = RowHeightForRow(child);
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
                                                        // v6.7: Smart Dummy Switch order + filters.
                                                        else if(str_comp(target.pName, "Order Type") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                        {
                                                                g_Config.m_KxSdsOrder = o;
                                                                GameClient()->m_BotNet.SdsResetVisited();
                                                        }
                                                        else if(str_comp(target.pName, "Team") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsTeam = o;
                                                        else if(str_comp(target.pName, "DDrace Team") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsDdraceTeam = o;
                                                        else if(str_comp(target.pName, "Friend") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsFriend = o;
                                                        else if(str_comp(target.pName, "Clanmate") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsClanmate = o;
                                                        else if(str_comp(target.pName, "Freeze") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsFreeze = o;
                                                        else if(str_comp(target.pName, "Targets") == 0 && str_comp(row.pName, "Smart dummy switch") == 0)
                                                                g_Config.m_KxSdsTargetsMode = o;
                                                        // v6.7: Smart Copy Moves dropdowns.
                                                        else if(str_comp(target.pName, "Sort Type") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopySort = o;
                                                        else if(str_comp(target.pName, "Apply Mode") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyApply = o;
                                                        else if(str_comp(target.pName, "Team") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyTeam = o;
                                                        else if(str_comp(target.pName, "DDrace Team") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyDdraceTeam = o;
                                                        else if(str_comp(target.pName, "Friend") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyFriend = o;
                                                        else if(str_comp(target.pName, "Clanmate") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyClanmate = o;
                                                        else if(str_comp(target.pName, "Freeze") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyFreeze = o;
                                                        else if(str_comp(target.pName, "Targets") == 0 && str_comp(row.pName, "Smart Copy Moves") == 0)
                                                                g_Config.m_KxSmartCopyTargetsMode = o;
                                                        // v1.56.273: Spoof Spectate mode.
                                                        else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "Spoof Spectate") == 0)
                                                                g_Config.m_KxSpoofSpectateMode = o;
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
							else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "Fake Aim") == 0 && str_comp(panel.pTitle, "Functions") == 0)
								g_Config.m_KxFakeAimMode = o;
                                                        // ESP dropdowns
                                                        else if(str_comp(target.pName, "Team") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspTeamFilter = o;
                                                        // v1.56.275: ESP DDrace Team.
                                                        else if(str_comp(target.pName, "DDrace Team") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspDDraceTeam = o;
                                                        else if(str_comp(target.pName, "Friend") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspFriendFilter = o;
                                                        else if(str_comp(target.pName, "Dummy") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspDummyFilter = o;
                                                        else if(str_comp(target.pName, "Clanmate") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspClanmateFilter = o;
                                                        else if(str_comp(target.pName, "Freeze") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspFreezeFilter = o;
                                                        else if(str_comp(target.pName, "Targets") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspTargetsFilterMode = o;
                                                        else if(str_comp(target.pName, "Mode") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspMode = o;
                                                        else if(str_comp(target.pName, "Pointer Style") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspStyle = o;
                                                        else if(str_comp(target.pName, "Box Style") == 0 && str_comp(row.pName, "ESP") == 0)
                                                                g_Config.m_KxEspBoxStyle = o;
                                                        // v1.56.256: Hitboxes style dropdowns — same label, two rows (by index).
                                                        else if(str_comp(target.pName, "Hitbox Style") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                        {
                                                                if(c == 1)
                                                                        g_Config.m_KxHitboxesTileStyle = o;
                                                                else if(c == 4)
                                                                        g_Config.m_KxHitboxesTriggerStyle = o;
                                                        }
                                                        // v1.56.257: Hitboxes filter dropdowns — same labels as ESP, own cvars.
                                                        else if(str_comp(target.pName, "Team") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesTeamFilter = o;
                                                        // v1.56.275: Hitboxes DDrace Team.
                                                        else if(str_comp(target.pName, "DDrace Team") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesDDraceTeam = o;
                                                        else if(str_comp(target.pName, "Friend") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesFriendFilter = o;
                                                        else if(str_comp(target.pName, "Dummy") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesDummyFilter = o;
                                                        else if(str_comp(target.pName, "Clanmate") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesClanmateFilter = o;
                                                        else if(str_comp(target.pName, "Freeze") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesFreezeFilter = o;
                                                        else if(str_comp(target.pName, "Targets") == 0 && str_comp(row.pName, "Hitboxes") == 0)
                                                                g_Config.m_KxHitboxesTargetsFilterMode = o;
                                                        // v1.56.262: Admin Alarm + BalanceBot dropdowns.
                                                        else if(str_comp(target.pName, "Admin level") == 0 && str_comp(row.pName, "Admin Alarm") == 0)
                                                                g_Config.m_KxAdminAlarmLevel = o;
                                                        else if(str_comp(target.pName, "Vertical Filter") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBalanceBotVertical = o;
                                                        else if(str_comp(target.pName, "Team") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbTeam = o;
                                                        // v1.56.275: BalanceBot DDrace Team.
                                                        else if(str_comp(target.pName, "DDrace Team") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbDDraceTeam = o;
                                                        else if(str_comp(target.pName, "Friend") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbFriend = o;
                                                        else if(str_comp(target.pName, "Dummy") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbDummy = o;
                                                        else if(str_comp(target.pName, "Clanmate") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbClanmate = o;
                                                        else if(str_comp(target.pName, "Freeze") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbFreeze = o;
                                                        else if(str_comp(target.pName, "Targets") == 0 && str_comp(row.pName, "BalanceBot") == 0)
                                                                g_Config.m_KxBbTargetsFilter = o;
                                                        // v1.56.83: AimBot per-weapon dropdowns (parent = "AimBot")
                                                        else if(str_comp(row.pName, "AimBot") == 0)
                                                        {
                                                                CAimBot &ab = GameClient()->m_AimBot;
                                                                if(str_comp(target.pName, "Weapon") == 0)
                                                                        ab.m_AimBotSelectedWeapon = o;
                                                                // v1.56.292: Entity Mode is a GUI selection (not
                                                                // saved) — it also switches the edited block below.
                                                                else if(str_comp(target.pName, "Entity Mode") == 0)
                                                                        ab.m_AimBotSelectedEntity = o;
                                                                else
                                                                {
                                                                        auto &s = AbEditSettings(ab);
                                                                        if(str_comp(target.pName, "Mode") == 0)
                                                                        {
                                                                                // v1.56.262: while Silent is on the dropdown hides
                                                                                // "Trigger" — remap the displayed index.
                                                                                s.m_AimMode = s.m_Silent ? (o == 0 ? 0 : o == 1 ? 2 : 3) : o;
                                                                        }
                                                                        else if(str_comp(target.pName, "Rules") == 0)
                                                                                s.m_Rules = o;
                                                                        else if(str_comp(target.pName, "Team") == 0)
                                                                                s.m_TeamFilter = o;
                                                                        else if(str_comp(target.pName, "Friend") == 0)
                                                                                s.m_FriendFilter = o;
                                                                        else if(str_comp(target.pName, "Dummy") == 0)
                                                                                s.m_DummyFilter = o;
                                                                        else if(str_comp(target.pName, "Clanmate") == 0)
                                                                                s.m_ClanmateFilter = o;
                                                                        else if(str_comp(target.pName, "Freeze") == 0)
                                                                                s.m_FreezeFilter = o;
                                                                        else if(str_comp(target.pName, "Priority") == 0)
                                                                                s.m_Priority = o;
                                                                        else if(str_comp(target.pName, "Targets") == 0)
                                                                                s.m_TargetsFilter = o;
                                                                }
                                                        }
                                                        // v1.56.83: TriggerBot per-weapon dropdowns (parent = "TriggerBot")
                                                        else if(str_comp(row.pName, "TriggerBot") == 0)
                                                        {
                                                                CAimBot &tb = GameClient()->m_AimBot;
                                                                if(str_comp(target.pName, "Weapon") == 0)
                                                                        tb.m_TriggerBotSelectedWeapon = o;
                                                                // v1.56.292: Entity Mode is a GUI selection (not
                                                                // saved) — it also switches the edited block below.
                                                                else if(str_comp(target.pName, "Entity Mode") == 0)
                                                                        tb.m_TriggerBotSelectedEntity = o;
                                                                else
                                                                {
                                                                        auto &s = TbEditSettings(tb);
                                                                        if(str_comp(target.pName, "Trigger mode") == 0)
                                                                                s.m_TriggerMode = o;
                                                                        else if(str_comp(target.pName, "Rules") == 0)
                                                                                s.m_Rules = o;
                                                                        else if(str_comp(target.pName, "Team") == 0)
                                                                                s.m_TeamFilter = o;
                                                                        else if(str_comp(target.pName, "Friend") == 0)
                                                                                s.m_FriendFilter = o;
                                                                        else if(str_comp(target.pName, "Dummy") == 0)
                                                                                s.m_DummyFilter = o;
                                                                        else if(str_comp(target.pName, "Clanmate") == 0)
                                                                                s.m_ClanmateFilter = o;
                                                                        else if(str_comp(target.pName, "Freeze") == 0)
                                                                                s.m_FreezeFilter = o;
                                                                        else if(str_comp(target.pName, "Priority") == 0)
                                                                                s.m_Priority = o;
                                                                        else if(str_comp(target.pName, "Targets") == 0)
                                                                                s.m_TargetsFilter = o;
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
                                                        // v6.3: ArrayList HUD dropdowns.
                                                        else if(str_comp(row.pName, "ArrayList") == 0)
                                                        {
                                                                if(str_comp(target.pName, "X Anchor") == 0)
                                                                        g_Config.m_KxArrayListXAnchor = o;
                                                                else if(str_comp(target.pName, "Y Anchor") == 0)
                                                                        g_Config.m_KxArrayListYAnchor = o;
                                                                else if(str_comp(target.pName, "Sort Type") == 0)
                                                                        g_Config.m_KxArrayListSort = o;
                                                                else if(str_comp(target.pName, "Style") == 0)
                                                                        g_Config.m_KxArrayListStyle = o;
                                                                else if(str_comp(target.pName, "Rainbow Mode") == 0)
                                                                        g_Config.m_KxArrayListRainbowMode = o;
                                                        }
                                                        // v6.4: ArrayInfo dropdowns.
                                                        else if(str_comp(row.pName, "ArrayInfo") == 0)
                                                        {
                                                                if(str_comp(target.pName, "X Anchor") == 0)
                                                                {
                                                                        // The info bar never moves with the anchor — the
                                                                        // anchor only aligns the lines inside the box.
                                                                        g_Config.m_KxArrayInfoXAnchor = o;
                                                                }
                                                                else if(str_comp(target.pName, "Style") == 0)
                                                                        g_Config.m_KxArrayInfoStyle = o;
                                                                else if(str_comp(target.pName, "Rainbow Mode") == 0)
                                                                        g_Config.m_KxArrayInfoRainbowMode = o;
                                                        }
                                                        // v6.7: ArrayPlayer dropdowns.
                                                        else if(str_comp(row.pName, "ArrayPlayer") == 0)
                                                        {
                                                                if(str_comp(target.pName, "X Anchor") == 0)
                                                                        g_Config.m_KxArrayPlayerXAnchor = o;
                                                                else if(str_comp(target.pName, "Style") == 0)
                                                                        g_Config.m_KxArrayPlayerStyle = o;
                                                                else if(str_comp(target.pName, "Rainbow Mode") == 0)
                                                                        g_Config.m_KxArrayPlayerRainbowMode = o;
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
                                        KxColorWithAlpha(m_AccentColorRGBA, alpha * 0.4f * ia * m_MainOpacity));
                        }
                        if(o == target.valueIdx)
                        {
                                DrawRoundedRect(ddX + 3.0f, itemY + 6.0f, 2.5f, DROPDOWN_H - 12.0f, 1.0f,
                                        IGraphics::CORNER_ALL,
                                        KxColorWithAlpha(m_AccentColorRGBA, alpha * ia * m_MainOpacity));
                        }
                        const ColorRGBA optCol = (o == target.valueIdx)
                                ? KxColorWithAlpha(m_AccentColorRGBA, alpha * ia * m_MainOpacity)
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
if(!SearchKeepsRow(row))
                                        continue;
                                const float rh = RowHeightForRow(row);
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
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                const float crh = RowHeightForRow(child);
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
                        // v6.2: Enter submits the Playground Settings RCON/chat
                        // fields (they clear afterwards, like a real console).
                        if(m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0)
                        {
                                SPanel &Panel = g_Panels[m_InputPanel];
                                if(str_comp(Panel.pTitle, "TAS") == 0 && m_InputRow < Panel.rowCount)
                                {
                                        SRow &Row = Panel.pRows[m_InputRow];
                                        if(str_comp(Row.pName, "Playground Settings") == 0 && m_InputChild >= 0 && m_InputChild < Row.childCount)
                                        {
                                                SRow &Child = Row.pChildren[m_InputChild];
                                                if(str_comp(Child.pName, "RCON Command") == 0 && Child.m_aInputBuf[0])
                                                {
                                                        GameClient()->m_Tas.PgRconEcho(Child.m_aInputBuf);
                                                        GameClient()->m_Tas.PgRconExecute(Child.m_aInputBuf);
                                                        Child.m_aInputBuf[0] = '\0';
                                                        Child.m_InputInitialized = true;
                                                }
                                                else if(str_comp(Child.pName, "Chat") == 0 && Child.m_aInputBuf[0])
                                                {
                                                        if(GameClient()->m_Chat.TasVirtual())
                                                                GameClient()->m_Chat.SendChat(0, Child.m_aInputBuf);
                                                        Child.m_aInputBuf[0] = '\0';
                                                        Child.m_InputInitialized = true;
                                                }
                                        }
                                }
                        }
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
		if((str_comp(g_Panels[panelIdx].pTitle, "TAS") == 0 && !TasRowVisible(g_Panels[panelIdx].pRows, g_Panels[panelIdx].rowCount, r)) || (str_comp(g_Panels[panelIdx].pTitle, "Dummy Manager") == 0 && !DummyManagerRowVisible(g_Panels[panelIdx].pRows, g_Panels[panelIdx].rowCount, r)))
			continue;
                const float rh = RowHeightForRow(row);
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
                // v1.56.286: search auto-expand — children of a parent with
                // matching children stay hit-testable while the search forces
                // them visible (m_SearchChildMatch is false when inactive).
                if(row.type == ERowType::Expandable && (row.expanded || row.m_SearchChildMatch) && row.pChildren)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                SRow &child = row.pChildren[c];
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                if(!SearchKeepsChild(child))
                                        continue;
                                const float crh = RowHeightForRow(child);
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

        // v1.56.282: Dummy Manager proxy inputs — identified by array membership
        // ("Main"/"Dummy N" names are too generic for the name chain below).
        if(pRow >= g_DummyProxyChildren && pRow < g_DummyProxyChildren + MAX_DUMMIES)
        {
                const int D = (int)(pRow - g_DummyProxyChildren);
                CDummyData &DD = GameClient()->m_BotNet.m_Dummy;
                str_copy(DD.m_aProfiles[D].m_aProxy, pRow->m_aInputBuf, sizeof(DD.m_aProfiles[D].m_aProxy));
                DD.Save();
                m_ProxyDirty = true;
                return;
        }

        // Main Color (hex, e.g. "9d4edd"). Was "ClickGUI Color" before v1.56.199.
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
        // v6.3: ArrayList text color (hex).
        else if(str_comp(pRow->pName, "Text Color") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount && str_comp(g_Panels[m_InputPanel].pRows[m_InputRow].pName, "ArrayList") == 0)
        {
                unsigned hex = 0;
                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                        str_format(g_Config.m_KxArrayListTextColor, sizeof(g_Config.m_KxArrayListTextColor), "%06x", hex & 0xffffff);
        }
        // v6.4: ArrayInfo text color + watermark.
        else if(str_comp(pRow->pName, "Text Color") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount && str_comp(g_Panels[m_InputPanel].pRows[m_InputRow].pName, "ArrayInfo") == 0)
        {
                unsigned hex = 0;
                if(sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                        str_format(g_Config.m_KxArrayInfoTextColor, sizeof(g_Config.m_KxArrayInfoTextColor), "%06x", hex & 0xffffff);
        }
        else if(str_comp(pRow->pName, "Watermark") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount && str_comp(g_Panels[m_InputPanel].pRows[m_InputRow].pName, "ArrayInfo") == 0)
        {
                str_copy(g_Config.m_KxArrayInfoWatermark, pRow->m_aInputBuf, sizeof(g_Config.m_KxArrayInfoWatermark));
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
                        case KX_LINE_RT_PATHFINDER: g_Config.m_KxLineRtPathfinderColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_ESP: g_Config.m_KxLineEspColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_TAS: g_Config.m_KxLineTasColor = 0xff000000u | (hex & 0xffffff); break;
                        case KX_LINE_HITBOXES: g_Config.m_KxLineHitboxesColor = 0xff000000u | (hex & 0xffffff); break;
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
        // v1.56.263: Admin Alarm message — per level (Level dropdown selects).
        else if(str_comp(pRow->pName, "Message") == 0)
        {
                char *pAlarmDst = (g_Config.m_KxAdminAlarmLevel == 0) ? g_Config.m_KxAdminAlarmMsg1 :
                        (g_Config.m_KxAdminAlarmLevel == 1) ? g_Config.m_KxAdminAlarmMsg2 : g_Config.m_KxAdminAlarmMsg3;
                str_copy(pAlarmDst, pRow->m_aInputBuf, 128);
        }
        else if((str_comp(pRow->pName, "Targets IDs") == 0 || str_comp(pRow->pName, "Untargets IDs") == 0) && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount)
        {
                const char *pOwner = g_Panels[m_InputPanel].pRows[m_InputRow].pName;
                CAimBot &ab = GameClient()->m_AimBot;
                if(str_comp(pOwner, "AimBot") == 0)
                {
                        auto &s = AbEditSettings(ab);
                        str_copy(s.m_aTargetsFilter, pRow->m_aInputBuf, sizeof(s.m_aTargetsFilter));
                }
                else if(str_comp(pOwner, "TriggerBot") == 0)
                {
                        auto &s = TbEditSettings(ab);
                        str_copy(s.m_aTargetsFilter, pRow->m_aInputBuf, sizeof(s.m_aTargetsFilter));
                }
                else if(str_comp(pOwner, "ESP") == 0)
                        str_copy(g_Config.m_KxEspTargetsFilter, pRow->m_aInputBuf, sizeof(g_Config.m_KxEspTargetsFilter));
                // v1.56.257: Hitboxes → Targets IDs.
                else if(str_comp(pOwner, "Hitboxes") == 0)
                        str_copy(g_Config.m_KxHitboxesTargetsFilter, pRow->m_aInputBuf, sizeof(g_Config.m_KxHitboxesTargetsFilter));
                // v1.56.262: BalanceBot → Targets IDs.
                else if(str_comp(pOwner, "BalanceBot") == 0)
                        str_copy(g_Config.m_KxBbTargetsIds, pRow->m_aInputBuf, sizeof(g_Config.m_KxBbTargetsIds));
                // v6.7: Smart Copy Moves / Smart Dummy Switch → Targets IDs.
                else if(str_comp(pOwner, "Smart Copy Moves") == 0)
                        str_copy(g_Config.m_KxSmartCopyTargets, pRow->m_aInputBuf, sizeof(g_Config.m_KxSmartCopyTargets));
                else if(str_comp(pOwner, "Smart dummy switch") == 0)
                        str_copy(g_Config.m_KxSdsTargets, pRow->m_aInputBuf, sizeof(g_Config.m_KxSdsTargets));
        }
        // Block children: string IDs.
        else if(str_comp(pRow->pName, "Main ID") == 0)
        {
                // v1.56.259: empty field = no main (-1), not player 0.
                g_Config.m_KxMain = pRow->m_aInputBuf[0] ? atoi(pRow->m_aInputBuf) : -1;
        }
        else if(str_comp(pRow->pName, "Dummy ID") == 0)
                g_Config.m_KxTripleFlyDummyId = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Target IDs") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount && str_comp(g_Panels[m_InputPanel].pRows[m_InputRow].pName, "Auto Triple Fly") == 0)
                str_copy(g_Config.m_KxTripleFlyTargetIds, pRow->m_aInputBuf, sizeof(g_Config.m_KxTripleFlyTargetIds));
        else if(str_comp(pRow->pName, "Target IDs") == 0 || str_comp(pRow->pName, "Untarget IDs") == 0)
                str_copy(GameClient()->m_BotNet.m_aTargetIDsStr, pRow->m_aInputBuf, sizeof(GameClient()->m_BotNet.m_aTargetIDsStr));
        else if(str_comp(pRow->pName, "Rescue IDs") == 0 || str_comp(pRow->pName, "Unrescue IDs") == 0)
                str_copy(GameClient()->m_BotNet.m_aRescueIDsStr, pRow->m_aInputBuf, sizeof(GameClient()->m_BotNet.m_aRescueIDsStr));
        // "Dummy IDs" lives in two panels (Helpers + SDS): guard by the owning row.
        else if(str_comp(pRow->pName, "Dummy IDs") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount && str_comp(g_Panels[m_InputPanel].pRows[m_InputRow].pName, "Helpers") == 0)
                str_copy(GameClient()->m_BotNet.m_aHelpersDummyIDsStr, pRow->m_aInputBuf, sizeof(GameClient()->m_BotNet.m_aHelpersDummyIDsStr));
        // Block children: numeric distances.
        else if(str_comp(pRow->pName, "Rescue Dist") == 0) g_Config.m_KxRescueRadius = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Target Dist") == 0) g_Config.m_KxTargetDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Main Dist") == 0) g_Config.m_KxMainDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Stand Dist") == 0) g_Config.m_KxStandDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "MStand Dist") == 0) g_Config.m_KxMainStandDist = (int)atof(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Buttons") == 0) str_copy(g_Config.m_KxEdgeBindButtons, pRow->m_aInputBuf, sizeof(g_Config.m_KxEdgeBindButtons));
        // v6.7: the SDS "Dummy IDs" row was replaced by Order Type + filters.
        // v1.56.273: Spoof Spectate numeric fields.
        else if(str_comp(pRow->pName, "Fake spectate X") == 0) g_Config.m_KxSpoofSpectateX = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Fake spectate Y") == 0) g_Config.m_KxSpoofSpectateY = atoi(pRow->m_aInputBuf);
        else if(str_comp(pRow->pName, "Fake Follow ID") == 0) g_Config.m_KxSpoofSpectateFollowId = atoi(pRow->m_aInputBuf);
        // v1.56.275: DDrace team Target ID (ESP/Hitboxes/BalanceBot).
        else if(str_comp(pRow->pName, "Target ID") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount)
        {
                const char *pOwner = g_Panels[m_InputPanel].pRows[m_InputRow].pName;
                if(str_comp(pOwner, "ESP") == 0)
                        g_Config.m_KxEspDDraceTeamId = atoi(pRow->m_aInputBuf);
                else if(str_comp(pOwner, "Hitboxes") == 0)
                        g_Config.m_KxHitboxesDDraceTeamId = atoi(pRow->m_aInputBuf);
                else if(str_comp(pOwner, "BalanceBot") == 0)
                        g_Config.m_KxBbDDraceTeamId = atoi(pRow->m_aInputBuf);
                // v6.7: Smart Copy Moves / Smart Dummy Switch → DDrace team Target ID.
                else if(str_comp(pOwner, "Smart Copy Moves") == 0)
                        g_Config.m_KxSmartCopyDdraceTeamId = atoi(pRow->m_aInputBuf);
                else if(str_comp(pOwner, "Smart dummy switch") == 0)
                        g_Config.m_KxSdsDdraceTeamId = atoi(pRow->m_aInputBuf);
        }
        // v1.56.283: Visuals custom-color Color input — the Sprite/Entity
        // dropdowns on the "Custom colors" owner row select the cvar.
        else if(str_comp(pRow->pName, "Color") == 0 && m_InputPanel >= 0 && m_InputPanel < NUM_PANELS && m_InputRow >= 0 && m_InputRow < g_Panels[m_InputPanel].rowCount)
        {
                const SRow &pOwner = g_Panels[m_InputPanel].pRows[m_InputRow];
                if(str_comp(pOwner.pName, "Custom colors") == 0)
                {
                        int SpriteIdx = 0, EntIdx = 0;
                        CustomColorSelection(pOwner, &SpriteIdx, &EntIdx);
                        const SCustomColorCfg Cfg = CustomColorCfgFor(SpriteIdx, EntIdx);
                        unsigned hex = 0;
                        if(Cfg.pColor && sscanf(pRow->m_aInputBuf, "%6x", &hex) == 1)
                                *Cfg.pColor = 0xff000000u | (hex & 0xffffff);
                }
        }
        // v1.56.171 BUG9: Binds panel — unified bind/unbind logic.
        // For each bind row: look up the command string, find the currently bound key
        // (if any), unbind it, then bind the new key (if non-empty). When the field is
        // emptied, only the unbind runs. All kx_ targets are now cvars, so
        // `toggle kx_X 1 0` works (no more "Invalid command: 'kx_helpers'").
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
                else if(str_comp(pRow->pName, "Avoid Freeze") == 0) pCmd = "toggle kx_avoid_freeze 1 0";
                else if(str_comp(pRow->pName, "Helpers") == 0) pCmd = "toggle kx_helpers 1 0";
                                else if(str_comp(pRow->pName, "Auto Triple Fly") == 0) pCmd = "toggle kx_triple_fly 1 0";
                                else if(str_comp(pRow->pName, "Auto Fire") == 0) pCmd = "toggle kx_auto_fire 1 0";
                                else if(str_comp(pRow->pName, "BalanceBot") == 0) pCmd = "toggle kx_balance_bot 1 0";
else if(str_comp(pRow->pName, "TAS Record") == 0) pCmd = "toggle kx_tas_record 1 0";
                                else if(str_comp(pRow->pName, "TAS Pause") == 0) pCmd = "toggle kx_tas_pause 1 0";
                                else if(str_comp(pRow->pName, "TAS Rewind") == 0) pCmd = "kx_tas_rewind";
                                                else if(str_comp(pRow->pName, "TAS Forward") == 0) pCmd = "kx_tas_forward";
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
// v1.56.284: ToolTip — ClickGUI-styled tooltip rendered near the cursor.
// ============================================================================

void CClickGui::RenderToolTip(float alpha)
{
        if(!m_aToolTipText[0])
                return;

        constexpr float FontSz = 15.0f;
        constexpr float LineH = 19.0f;
        constexpr float PadX = 8.0f;
        constexpr float PadY = 6.0f;
        constexpr float MaxTextW = 340.0f;
        constexpr int MaxLines = 6;

        // Greedy word wrap into at most MaxLines lines of MaxTextW pixels.
        char aLines[MaxLines][256];
        int lineCount = 0;
        char aCur[256];
        aCur[0] = '\0';
        const char *p = m_aToolTipText;
        while(*p && lineCount < MaxLines)
        {
                while(*p == ' ')
                        ++p;
                if(!*p)
                        break;
                const char *pEnd = p;
                while(*pEnd && *pEnd != ' ')
                        ++pEnd;
                char aWord[192];
                const int wordLen = (int)(pEnd - p);
                if(wordLen <= 0 || wordLen >= (int)sizeof(aWord))
                        break;
                str_copy(aWord, p, wordLen + 1);
                const int beforeLen = str_length(aCur);
                if(beforeLen)
                        str_append(aCur, " ", sizeof(aCur));
                str_append(aCur, aWord, sizeof(aCur));
                if(beforeLen > 0 && TextRender()->TextWidth(FontSz, aCur) > MaxTextW)
                {
                        aCur[beforeLen] = '\0';
                        str_copy(aLines[lineCount++], aCur, sizeof(aLines[0]));
                        str_copy(aCur, aWord, sizeof(aCur));
                }
                p = (*pEnd) ? pEnd + 1 : pEnd;
        }
        if(aCur[0] && lineCount < MaxLines)
                str_copy(aLines[lineCount++], aCur, sizeof(aLines[0]));
        if(lineCount == 0)
                return;

        float textW = 0.0f;
        for(int i = 0; i < lineCount; ++i)
                textW = std::max(textW, TextRender()->TextWidth(FontSz, aLines[i]));
        const float boxW = textW + PadX * 2.0f;
        const float boxH = (float)lineCount * LineH + PadY * 2.0f;

        // Right-below the cursor, clamped inside the screen (flips above the
        // cursor when there is no room below).
        float x = m_MousePos.x + 16.0f;
        float y = m_MousePos.y + 20.0f;
        if(x + boxW > VScreenWidth() - 4.0f)
                x = std::max(4.0f, VScreenWidth() - 4.0f - boxW);
        if(y + boxH > VScreenHeight() - 4.0f)
                y = m_MousePos.y - boxH - 10.0f;
        if(y < 4.0f)
                y = 4.0f;

        // ClickGUI look: rounded panel + a real 1 px accent stroke. v6.3: the
        // ClickGUI Settings → ToolTips opacities apply per part; with the UI
        // master off they are ignored (built-in defaults).
        const bool UiOn = g_Config.m_KxUiSettingsOn != 0;
        const float TextOp = (UiOn ? g_Config.m_KxUiTooltipTextOpacity : 100) / 100.0f;
        const float MainOp = (UiOn ? g_Config.m_KxUiTooltipMainOpacity : 100) / 100.0f;
        const float PanelOp = (UiOn ? g_Config.m_KxUiTooltipPanelOpacity : 100) / 100.0f;
        DrawRoundedRect(x, y, boxW, boxH, 4.0f, IGraphics::CORNER_ALL,
                KxColorWithAlpha(m_PanelBgRGBA, alpha * 0.97f * PanelOp));
        DrawRoundedOutline(x + 0.5f, y + 0.5f, boxW - 1.0f, boxH - 1.0f, 3.5f,
                KxColorWithAlpha(m_AccentColorRGBA, alpha * MainOp), true, true);
        for(int i = 0; i < lineCount; ++i)
                DrawText(x + PadX, y + PadY + (float)i * LineH, FontSz, aLines[i],
                        ColorRGBA(1.0f, 1.0f, 1.0f, alpha * TextOp));
}

// ============================================================================
// v6.3: ArrayList HUD — on-screen list of enabled features.
// ============================================================================

// Default anchor position (basis points): top-right corner.
static constexpr int KX_ARRAYLIST_DEF_POS_X = 9850;
static constexpr int KX_ARRAYLIST_DEF_POS_Y = 1000;
static constexpr int KX_ARRAYINFO_DEF_POS_X = 7000;
static constexpr int KX_ARRAYINFO_DEF_POS_Y = 500;
static constexpr int KX_ARRAYPLAYER_DEF_POS_X = 5000;
static constexpr int KX_ARRAYPLAYER_DEF_POS_Y = 5000;

// Full-saturation hue -> RGB.
static void KxHueToRgb(float Hue, float &R, float &G, float &B)
{
        Hue = fmodf(Hue, 360.0f);
        if(Hue < 0.0f)
                Hue += 360.0f;
        const float H = Hue / 60.0f;
        const int I = (int)H;
        const float F = H - (float)I;
        const float Q = 1.0f - F;
        switch(I % 6)
        {
        case 0: R = 1.0f; G = F; B = 0.0f; break;
        case 1: R = Q; G = 1.0f; B = 0.0f; break;
        case 2: R = 0.0f; G = 1.0f; B = F; break;
        case 3: R = 0.0f; G = Q; B = 1.0f; break;
        case 4: R = F; G = 0.0f; B = 1.0f; break;
        default: R = 1.0f; G = 0.0f; B = Q; break;
        }
}

// v6.3: real 1 px rounded-rect stroke, drawn as a line polyline. Unlike the
// outer/inner filled-rect trick, a translucent border composites exactly once,
// which is what the Main/Panel opacity sliders need. Top/bottom edges can be
// suppressed so touching blocks do not draw a double border at the seam.
void CClickGui::DrawRoundedOutline(float x, float y, float w, float h, float r, ColorRGBA Color, bool DrawTop, bool DrawBottom)
{
        if(w <= 0.0f || h <= 0.0f)
                return;
        constexpr float KxPi = 3.14159265358979f;
        r = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);

        std::vector<IGraphics::CLineItem> vLines;
        vLines.reserve(64);
        auto Add = [&](float x0, float y0, float x1, float y1) {
                if(x0 != x1 || y0 != y1)
                        vLines.emplace_back(x0, y0, x1, y1);
        };

        const float L = x, T = y, R = x + w, B = y + h;
        const float TopY = DrawTop ? (T + r) : T;
        const float BotY = DrawBottom ? (B - r) : B;
        if(BotY > TopY)
        {
                Add(L, TopY, L, BotY);
                Add(R, TopY, R, BotY);
        }

        constexpr int KxArcSeg = 6;
        auto Arc = [&](float cx, float cy, float A0, float A1) {
                float px = cx + cosf(A0) * r, py = cy + sinf(A0) * r;
                for(int i = 1; i <= KxArcSeg; ++i)
                {
                        const float A = A0 + (A1 - A0) * (float)i / (float)KxArcSeg;
                        const float nx = cx + cosf(A) * r, ny = cy + sinf(A) * r;
                        Add(px, py, nx, ny);
                        px = nx;
                        py = ny;
                }
        };

        if(r > 0.01f)
        {
                if(DrawTop)
                {
                        Arc(L + r, T + r, KxPi, KxPi * 1.5f);
                        Add(L + r, T, R - r, T);
                        Arc(R - r, T + r, KxPi * 1.5f, KxPi * 2.0f);
                }
                if(DrawBottom)
                {
                        Arc(R - r, B - r, 0.0f, KxPi * 0.5f);
                        Add(L + r, B, R - r, B);
                        Arc(L + r, B - r, KxPi * 0.5f, KxPi);
                }
        }
        else
        {
                if(DrawTop)
                        Add(L, T, R, T);
                if(DrawBottom)
                        Add(L, B, R, B);
        }

        if(vLines.empty())
                return;
        Graphics()->LinesBegin();
        Graphics()->SetColor(Color.r, Color.g, Color.b, Color.a);
        Graphics()->LinesDraw(vLines.data(), vLines.size());
        Graphics()->LinesEnd();
}

// Feature table: display name + the toggle that drives it. Runtime features
// (Pathfinder / TAS Play / Playground) are appended separately.
struct SArrayListFeature
{
        const char *pName;
        int *pOn;
};
static SArrayListFeature g_ArrayListFeatures[] = {
        // Functions panel — all toggles and expandable master switches.
        {"Helpers", &g_Config.m_KxHelpers},
        {"AimBot", &g_Config.m_KxAimBot},
        {"TriggerBot", &g_Config.m_KxTriggerBot},
        {"Auto Fire", &g_Config.m_KxAutoFire},
        {"BalanceBot", &g_Config.m_KxBalanceBot},
        {"Preserve Input", &g_Config.m_KxDummyHammerKeepInputs},
        {"Custom Latency", &g_Config.m_KxDummyHammerCustomLatency},
        {"Auto Fly", &g_Config.m_KxDummyHammerAuto},
        {"Fix Dummy Hammer", &g_Config.m_KxFixDummyHammer},
        {"Auto Triple Fly", &g_Config.m_KxTripleFly},
        {"Smart Copy Moves", &g_Config.m_KxSmartCopyMoves},
        {"Copy Moves Filter", &g_Config.m_KxDummyCopyMovesFilter},
        {"Mirror Direction", &g_Config.m_KxDummyCopyMirrorDir},
        {"Mirror Aim X", &g_Config.m_KxDummyCopyMirrorAimX},
        {"Mirror Aim Y", &g_Config.m_KxDummyCopyMirrorAimY},
        {"Smart Aim", &g_Config.m_KxDummyCopySmartAim},
        {"Fly Ride", &g_Config.m_KxFlyRide},
        {"Copy Moves Latency", &g_Config.m_KxCopyMovesLatencyEnabled},
        {"Laser Unfreeze", &g_Config.m_KxLaserUnfreeze},
        {"Fake Aim", &g_Config.m_KxFakeAim},
        {"Avoid Freeze", &g_Config.m_KxAvoidFreeze},
        // Visuals panel.
        {"Custom Colors", &g_Config.m_KxCustomColors},
        {"Hide Particles", &g_Config.m_KxHideParticles},
        {"Trajectory", &g_Config.m_KxShowTrajectory},
        {"ESP", &g_Config.m_KxEsp},
        {"Hitboxes", &g_Config.m_KxHitboxes},
        {"Bypass Zoom Lock", &g_Config.m_KxZoomUnlock},
        {"Bypass Asset Lock", &g_Config.m_KxAssetUnlock},
        {"Admin Alarm", &g_Config.m_KxAdminAlarm},
        // Settings panel (ClickGUI Settings excluded by design).
        {"IRC", &g_Config.m_KxIrcEnabled},
        {"Spoof Zoom", &g_Config.m_KxSpoofZoom},
        {"Spoof Spectate", &g_Config.m_KxSpoofSpectate},
        // Pathfinder / Real-Time Pathfinder masters.
        {"Freedom Path", &g_Config.m_KxPfFreedom},
        {"Reconstruct", &g_Config.m_KxRtPfReconstruct},
        {"Future Limit", &g_Config.m_KxRtPfFutureLimit},
        // TAS masters. (Record/Play/Playground are runtime states, appended below.)
        {"Auto Rewind/Forward", &g_Config.m_KxTasAutoRf},
};

// v6.3: "Name Length" sorting uses the RENDERED width (current font +
// ArrayList Size), not the character count, so the order matches what the eye
// sees on screen.
struct SArrayListWidthEntry
{
        std::string Name;
        float Width;
};

static void ArrayListSortItems(std::vector<std::string> &vItems, const std::vector<std::string> &vHistory,
        const std::vector<SArrayListWidthEntry> &vWidths)
{
        auto WidthOf = [&](const std::string &S) {
                for(const SArrayListWidthEntry &E : vWidths)
                        if(E.Name == S)
                                return E.Width;
                return 0.0f;
        };
        auto Less = [&](const std::string &A, const std::string &B) -> bool {
                const int Sort = g_Config.m_KxArrayListSort;
                if(Sort == 1)
                {
                        const float WA = WidthOf(A), WB = WidthOf(B);
                        if(std::fabs(WA - WB) > 0.01f)
                                return WA < WB;
                        return str_comp(A.c_str(), B.c_str()) < 0;
                }
                if(Sort == 2)
                {
                        auto Idx = [&](const std::string &S) {
                                for(size_t i = 0; i < vHistory.size(); ++i)
                                        if(vHistory[i] == S)
                                                return (int)i;
                                return 1000000;
                        };
                        return Idx(A) < Idx(B);
                }
                return str_comp(A.c_str(), B.c_str()) < 0;
        };
        // Insertion sort: the list is tiny and this avoids an <algorithm> dependency.
        for(size_t i = 1; i < vItems.size(); ++i)
        {
                std::string Key = vItems[i];
                size_t j = i;
                while(j > 0 && Less(Key, vItems[j - 1]))
                {
                        vItems[j] = vItems[j - 1];
                        --j;
                }
                vItems[j] = Key;
        }
}

bool CClickGui::ShouldRenderArrayList() const
{
        if(g_Config.m_KxArrayListEnabled == 0 && !m_ArrayListEditor)
                return false;
        if(GameClient()->m_Menus.IsActive())
                return false;
        const int State = Client()->State();
        return State == IClient::STATE_ONLINE || State == IClient::STATE_DEMOPLAYBACK;
}

bool CClickGui::ArrayListPositionModified() const
{
        return g_Config.m_KxArrayListPosX != KX_ARRAYLIST_DEF_POS_X ||
                g_Config.m_KxArrayListPosY != KX_ARRAYLIST_DEF_POS_Y;
}

void CClickGui::ResetArrayListPosition()
{
        g_Config.m_KxArrayListPosX = KX_ARRAYLIST_DEF_POS_X;
        g_Config.m_KxArrayListPosY = KX_ARRAYLIST_DEF_POS_Y;
}

bool CClickGui::ArrayListHitTest(vec2 mousePos) const
{
        if(!m_ArrayListRectValid)
                return false;
        return mousePos.x >= m_ArrayListRectX && mousePos.x <= m_ArrayListRectX + m_ArrayListRectW &&
                mousePos.y >= m_ArrayListRectY && mousePos.y <= m_ArrayListRectY + m_ArrayListRectH;
}

// v6.7: the REAL enabled-feature list (no editor sample). Single source of
// truth for both the ArrayList HUD and the ArrayInfo "X Functions" count.
void CClickGui::ArrayListBuildEnabled(std::vector<std::string> &vOut) const
{
        for(const SArrayListFeature &F : g_ArrayListFeatures)
                if(*F.pOn)
                        vOut.emplace_back(F.pName);
        // Runtime states (the literal exceptions from the ClickGUI toggles).
        CBotNet &BN = GameClient()->m_BotNet;
        if(BN.m_PfState != PF_STATE_IDLE)
                vOut.emplace_back("Pathfinder");
        if(GameClient()->m_Tas.IsPlaying())
                vOut.emplace_back("TAS Play");
        if(GameClient()->m_Tas.IsJoined())
                vOut.emplace_back("Playground");
        if(g_Config.m_KxTasRecord)
                vOut.emplace_back("TAS Record");
        // TAS Fake Aim is active only during playback, not just when enabled.
        if(GameClient()->m_Tas.FakeAimActive())
                vOut.emplace_back("TAS Fake Aim");
        // Spoofer's header is an OR of two configs (version / client string).
        if(g_Config.m_KxVersionSpoof || g_Config.m_KxSpoofClientStr)
                vOut.emplace_back("Spoofer");
        // Smart Dummy Switch is enabled while its trigger bind is set.
        {
                char aSdsKey[64];
                GameClient()->m_Binds.GetKey("kx_dummy_switch", aSdsKey, sizeof(aSdsKey));
                if(aSdsKey[0])
                        vOut.emplace_back("Smart Dummy Switch");
        }
}

void CClickGui::ArrayListBuildList(std::vector<std::string> &vOut) const
{
        vOut.clear();
        if(m_ArrayListEditor)
        {
                static const char *s_apSample[] = {"AimBot", "TriggerBot", "Pathfinder", "TAS Play", "Playground"};
                for(const char *pName : s_apSample)
                        vOut.emplace_back(pName);
                return;
        }
        ArrayListBuildEnabled(vOut);
}

void CClickGui::ArrayListUpdateHistory(const std::vector<std::string> &vEnabled)
{
        // Drop entries that are no longer enabled (they are re-appended on the
        // next enable, at the end of the order).
        for(size_t i = 0; i < m_vArrayListHistory.size();)
        {
                bool Found = false;
                for(const std::string &S : vEnabled)
                        if(S == m_vArrayListHistory[i])
                        {
                                Found = true;
                                break;
                        }
                if(Found)
                        ++i;
                else
                        m_vArrayListHistory.erase(m_vArrayListHistory.begin() + i);
        }
        // Append newly enabled features in enable order.
        for(const std::string &S : vEnabled)
        {
                bool Found = false;
                for(const std::string &H : m_vArrayListHistory)
                        if(H == S)
                        {
                                Found = true;
                                break;
                        }
                if(!Found)
                        m_vArrayListHistory.push_back(S);
        }
}

void CClickGui::UpdateArrayList(float dt)
{
        m_ArrayListHue += dt * (g_Config.m_KxArrayListRainbowSpeed / 10.0f) * 60.0f;

        if(!m_ArrayListDragging || !m_ArrayListRectValid)
                return;

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        // Relative drag: press position -> current position delta applied to the
        // HUD top-left captured at press time (no offset drift). While the editor
        // is open the drag is deliberately unclamped, so the HUD never bumps
        // against the screen edges; the in-game render keeps it visible.
        const float Left = m_ArrayListDragStartPos.x + (m_MousePos.x - m_ArrayListDragStartMouse.x);
        const float Top = m_ArrayListDragStartPos.y + (m_MousePos.y - m_ArrayListDragStartMouse.y);

        // The stored point is always the list's left edge; the X anchor only
        // aligns the rows inside the list (matches RenderArrayList). The Y
        // anchor only affects the row order.
        g_Config.m_KxArrayListPosX = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
        g_Config.m_KxArrayListPosY = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
}

void CClickGui::RenderArrayList(float alpha)
{
        m_ArrayListRectValid = false;

        std::vector<std::string> vItems;
        ArrayListBuildList(vItems);
        ArrayListUpdateHistory(vItems);
        if(vItems.empty())
                return;

        // The font size (and therefore the rendered width) is needed before the
        // sort: "Name Length" compares TextWidth, not string size.
        const bool UiOn = g_Config.m_KxUiSettingsOn != 0;
        const float SizeScale = (UiOn ? g_Config.m_KxArrayListSize : 10) / 10.0f;
        const float FontSz = 15.0f * SizeScale;

        std::vector<SArrayListWidthEntry> vSortWidths;
        vSortWidths.reserve(vItems.size());
        for(const std::string &S : vItems)
                vSortWidths.push_back({S, TextRender()->TextWidth(FontSz, S.c_str())});

        ArrayListSortItems(vItems, m_vArrayListHistory, vSortWidths);

        // The Y anchor only decides where new features are appended: "Top"
        // reverses the row order so new entries appear at the top, "Bottom"
        // keeps the sorted order (new entries at the bottom). The list itself
        // is always positioned by its top edge, so switching the anchor never
        // moves it.
        const float YRatio = g_Config.m_KxArrayListPosY / 10000.0f;
        int YA = g_Config.m_KxArrayListYAnchor; // 0 Auto, 1 Top, 2 Bottom
        if(YA == 0)
                YA = (YRatio < 0.5f) ? 1 : 2;
        if(YA == 1)
        {
                for(size_t a = 0, b = vItems.size(); a + 1 < b; ++a, --b)
                {
                        std::string Tmp = vItems[a];
                        vItems[a] = vItems[b - 1];
                        vItems[b - 1] = Tmp;
                }
        }

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        if(ScreenW <= 0.0f || ScreenH <= 0.0f)
                return;

        // UiOn/SizeScale/FontSz are computed before the sort (Name Length uses
        // the rendered width); only the paddings are needed here.
        const float PadX = 7.0f * SizeScale;
        const float PadY = 4.0f * SizeScale;
        const float Indent = (float)g_Config.m_KxArrayListIndent * SizeScale;
        const int Style = g_Config.m_KxArrayListStyle;
        const bool GroupStyle = (Style == 1 || Style == 3);
        const bool DDNetStyle = (Style >= 2);
        const bool Rainbow = g_Config.m_KxArrayListRainbow != 0;
        const int RainbowMode = g_Config.m_KxArrayListRainbowMode;
        const float RainbowStep = (float)g_Config.m_KxArrayListRainbowStep;
        const float TextOp = (UiOn ? g_Config.m_KxArrayListTextOpacity : 100) / 100.0f;
        const float MainOp = (UiOn ? g_Config.m_KxArrayListMainOpacity : 100) / 100.0f;
        const float PanelOp = (UiOn ? g_Config.m_KxArrayListPanelOpacity : 100) / 100.0f;

        unsigned TextHex = 0xffffff;
        sscanf(g_Config.m_KxArrayListTextColor, "%6x", &TextHex);

        const size_t Count = vItems.size();
        const float MaxLenPx = (UiOn ? (float)g_Config.m_KxArrayListMaxLength : 0.0f);
        std::vector<std::vector<std::string>> vItemLines(Count);
        std::vector<float> vTextW(Count);
        std::vector<float> vItemH(Count);
        float MaxTextW = 0.0f;
        for(size_t i = 0; i < Count; ++i)
        {
                std::vector<std::string> &vLines = vItemLines[i];
                const char *pText = vItems[i].c_str();
                if(MaxLenPx <= 0.0f)
                        vLines.emplace_back(pText);
                else
                {
                        // Greedy word wrap at the physical Max Length (px).
                        std::string Cur;
                        const char *p = pText;
                        while(*p)
                        {
                                while(*p == ' ')
                                        ++p;
                                if(!*p)
                                        break;
                                const char *pStart = p;
                                while(*p && *p != ' ')
                                        ++p;
                                std::string Word(pStart, p);
                                std::string Try = Cur.empty() ? Word : Cur + " " + Word;
                                if(!Cur.empty() && TextRender()->TextWidth(FontSz, Try.c_str()) > MaxLenPx)
                                {
                                        vLines.push_back(Cur);
                                        Cur = Word;
                                }
                                else
                                        Cur = Try;
                        }
                        if(!Cur.empty())
                                vLines.push_back(Cur);
                        if(vLines.empty())
                                vLines.emplace_back("");
                }
                float LineMaxW = 0.0f;
                for(const std::string &L : vLines)
                        LineMaxW = std::max(LineMaxW, TextRender()->TextWidth(FontSz, L.c_str()));
                vTextW[i] = LineMaxW;
                vItemH[i] = (float)vLines.size() * FontSz + PadY * 2.0f;
                MaxTextW = std::max(MaxTextW, LineMaxW);
        }

        float BoxW, BoxH;
        if(GroupStyle)
        {
                BoxW = MaxTextW + PadX * 2.0f;
                float LinesH = 0.0f;
                for(size_t i = 0; i < Count; ++i)
                        LinesH += (float)vItemLines[i].size() * FontSz;
                BoxH = LinesH + (float)(Count - 1) * Indent + PadY * 2.0f;
        }
        else
        {
                BoxW = MaxTextW + PadX * 2.0f;
                float RowsH = 0.0f;
                for(size_t i = 0; i < Count; ++i)
                        RowsH += vItemH[i];
                BoxH = RowsH + (float)(Count - 1) * Indent;
        }

        const float XRatio = g_Config.m_KxArrayListPosX / 10000.0f;
        int XA = g_Config.m_KxArrayListXAnchor;
        if(XA == 0)
                XA = (XRatio < 1.0f / 3.0f) ? 3 : ((XRatio > 2.0f / 3.0f) ? 1 : 2);
        const float AnchorY = YRatio * ScreenH;
        // The stored point is always the list's left edge. The X anchor only
        // aligns the rows inside the list; it never moves the list on screen.
        float Left = XRatio * ScreenW;
        // The Y anchor only affects the row order (see above); the list is always
        // positioned by its top edge so switching the anchor never moves it.
        float Top = AnchorY;
        if(!m_ArrayListEditor)
        {
                // In game the HUD is kept on screen. The editor preview is
                // dragged freely and must not bump against the screen edges.
                Left = std::clamp(Left, 0.0f, std::max(0.0f, ScreenW - BoxW));
                Top = std::clamp(Top, 0.0f, std::max(0.0f, ScreenH - BoxH));
        }

        m_ArrayListRectX = Left;
        m_ArrayListRectY = Top;
        m_ArrayListRectW = BoxW;
        m_ArrayListRectH = BoxH;
        m_ArrayListRectValid = true;

        // ClickGUI tooltip look: one rounded border rect + one rounded background
        // rect (no per-edge seams, so opacity never shows stray pixels).
        // DDNet styles use the DDNet tooltip palette: dark gray, no accent.
        const ColorRGBA BorderCol = DDNetStyle
                ? ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f * alpha * MainOp)
                : KxColorWithAlpha(m_AccentColorRGBA, alpha * MainOp);
        const ColorRGBA BgCol = DDNetStyle
                ? ColorRGBA(0.2f, 0.2f, 0.2f, 0.8f * alpha * PanelOp)
                : KxColorWithAlpha(m_PanelBgRGBA, alpha * 0.97f * PanelOp);

        auto DrawBlock = [&](float x, float y, float w, float h, int Corners, bool DrawTop, bool DrawBottom) {
                DrawRoundedRect(x, y, w, h, 4.0f, Corners, BgCol);
                DrawRoundedOutline(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 3.5f, BorderCol, DrawTop, DrawBottom);
        };

        auto DrawRowText = [&](float x, float y, const char *pText, size_t RowIdx) {
                if(!Rainbow)
                {
                        DrawText(x, y, FontSz, pText, ColorRGBA(TextHex, true).WithAlpha(alpha * TextOp));
                        return;
                }
                if(RainbowMode == 2)
                {
                        // One gradient across the WHOLE HUD (all blocks), left to right.
                        // The hue decreases with x, so as the base hue advances with
                        // time the gradient visibly flows from the left to the right.
                        // The colour splits let the whole string be drawn in one
                        // TextEx call, so glyph bearings/kerning stay intact.
                        std::vector<STextColorSplit> vSplits;
                        vSplits.reserve((size_t)str_length(pText));
                        const char *pCur = pText;
                        int BytePos = 0;
                        while(*pCur)
                        {
                                const char *pNext = pCur;
                                str_utf8_decode(&pNext);
                                const int CharLen = (int)(pNext - pCur);
                                const float CharX = x + TextRender()->TextWidth(FontSz, pText, BytePos);
                                const float t = (BoxW > 0.0f) ? std::clamp((CharX - Left) / BoxW, 0.0f, 1.0f) : 0.0f;
                                float R, G, B;
                                KxHueToRgb(m_ArrayListHue - RainbowStep * t, R, G, B);
                                vSplits.emplace_back(BytePos, CharLen, ColorRGBA(R, G, B, alpha * TextOp));
                                BytePos += CharLen;
                                pCur = pNext;
                        }
                        KxDrawTextColorSplits(TextRender(), x, y, FontSz, pText, ColorRGBA(1.0f, 1.0f, 1.0f, alpha * TextOp), vSplits);
                        return;
                }
                float R, G, B;
                KxHueToRgb(m_ArrayListHue + (float)RowIdx * (RainbowMode == 1 ? RainbowStep : 0.0f), R, G, B);
                DrawText(x, y, FontSz, pText, ColorRGBA(R, G, B, alpha * TextOp));
        };

        if(GroupStyle)
        {
                DrawBlock(Left, Top, BoxW, BoxH, IGraphics::CORNER_ALL, true, true);
                float ty = Top + PadY;
                for(size_t i = 0; i < Count; ++i)
                {
                        // The shared panel never moves with the X anchor; the
                        // anchor only aligns the rows inside it.
                        for(const std::string &Line : vItemLines[i])
                        {
                                const float lw = TextRender()->TextWidth(FontSz, Line.c_str());
                                float tx = Left + PadX;
                                if(XA == 2)
                                        tx = Left + (BoxW - lw) * 0.5f;
                                else if(XA == 1)
                                        tx = Left + BoxW - PadX - lw;
                                DrawRowText(tx, ty, Line.c_str(), i);
                                ty += FontSz;
                        }
                        ty += Indent;
                }
        }
        else
        {
                struct SKxRect { float x, y, w, h; };
                std::vector<SKxRect> vRects(Count);
                float CurY = Top;
                for(size_t i = 0; i < Count; ++i)
                {
                        const float bw = vTextW[i] + PadX * 2.0f;
                        // The X anchor aligns the row blocks inside the list's
                        // bounding box; it never moves the list on screen.
                        float bx;
                        if(XA == 3)
                                bx = Left;
                        else if(XA == 2)
                                bx = Left + (BoxW - bw) * 0.5f;
                        else
                                bx = Left + BoxW - bw;
                        SKxRect R;
                        R.x = bx;
                        R.y = CurY;
                        R.w = bw;
                        R.h = vItemH[i];
                        vRects[i] = R;
                        CurY += vItemH[i] + Indent;
                }

                const bool Touching = (Indent < 0.5f);
                if(!Touching)
                {
                        for(size_t i = 0; i < Count; ++i)
                        {
                                const SKxRect &R = vRects[i];
                                DrawBlock(R.x, R.y, R.w, R.h, IGraphics::CORNER_ALL, true, true);
                                float ly = R.y + PadY;
                                for(const std::string &Line : vItemLines[i])
                                {
                                        DrawRowText(R.x + PadX, ly, Line.c_str(), i);
                                        ly += FontSz;
                                }
                        }
                }
                else
                {
                        // Blocks literally connect: the fill gets square corners on
                        // every side covered by the neighbour above/below, and the
                        // accent outline is drawn only on the exposed part of the
                        // union. Widths differ per row, so the seam is handled
                        // span-by-span instead of suppressing whole edges.
                        constexpr float KxR = 4.0f;
                        constexpr float KxPi = 3.14159265358979f;
                        std::vector<IGraphics::CLineItem> vLines;
                        auto AddSeg = [&](float x0, float y0, float x1, float y1) {
                                if(x0 != x1 || y0 != y1)
                                        vLines.emplace_back(x0, y0, x1, y1);
                        };
                        auto AddArc = [&](float cx, float cy, float a0, float a1) {
                                constexpr int KxSeg = 6;
                                float px = cx + cosf(a0) * KxR, py = cy + sinf(a0) * KxR;
                                for(int s = 1; s <= KxSeg; ++s)
                                {
                                        const float a = a0 + (a1 - a0) * (float)s / (float)KxSeg;
                                        const float nx = cx + cosf(a) * KxR, ny = cy + sinf(a) * KxR;
                                        AddSeg(px, py, nx, ny);
                                        px = nx;
                                        py = ny;
                                }
                        };

                        for(size_t i = 0; i < Count; ++i)
                        {
                                const SKxRect &R = vRects[i];
                                const SKxRect *pA = (i > 0) ? &vRects[i - 1] : nullptr;
                                const SKxRect *pB = (i + 1 < Count) ? &vRects[i + 1] : nullptr;
                                const bool OverA = pA && std::min(R.x + R.w, pA->x + pA->w) > std::max(R.x, pA->x) + 0.5f;
                                const bool OverB = pB && std::min(R.x + R.w, pB->x + pB->w) > std::max(R.x, pB->x) + 0.5f;

                                const float AC0 = OverA ? std::max(R.x, pA->x) : 0.0f;
                                const float AC1 = OverA ? std::min(R.x + R.w, pA->x + pA->w) : 0.0f;
                                const float BC0 = OverB ? std::max(R.x, pB->x) : 0.0f;
                                const float BC1 = OverB ? std::min(R.x + R.w, pB->x + pB->w) : 0.0f;
                                // Round a seam corner only when the exposed sliver is at
                                // least as wide as the corner radius. A row that is only a
                                // fraction of a pixel wider would otherwise get an arc
                                // curving back into the block right at the seam.
                                const bool TLr = !OverA || (AC0 - R.x) > KxR;
                                const bool TRr = !OverA || (R.x + R.w - AC1) > KxR;
                                const bool BLr = !OverB || (BC0 - R.x) > KxR;
                                const bool BRr = !OverB || (R.x + R.w - BC1) > KxR;

                                int Corners = IGraphics::CORNER_NONE;
                                if(TLr) Corners |= IGraphics::CORNER_TL;
                                if(TRr) Corners |= IGraphics::CORNER_TR;
                                if(BLr) Corners |= IGraphics::CORNER_BL;
                                if(BRr) Corners |= IGraphics::CORNER_BR;
                                DrawRoundedRect(R.x, R.y, R.w, R.h, KxR, Corners, BgCol);

                                AddSeg(R.x, R.y + (TLr ? KxR : 0.0f), R.x, R.y + R.h - (BLr ? KxR : 0.0f));
                                AddSeg(R.x + R.w, R.y + (TRr ? KxR : 0.0f), R.x + R.w, R.y + R.h - (BRr ? KxR : 0.0f));
                                if(TLr) AddArc(R.x + KxR, R.y + KxR, KxPi, KxPi * 1.5f);
                                if(TRr) AddArc(R.x + R.w - KxR, R.y + KxR, KxPi * 1.5f, KxPi * 2.0f);
                                if(BRr) AddArc(R.x + R.w - KxR, R.y + R.h - KxR, 0.0f, KxPi * 0.5f);
                                if(BLr) AddArc(R.x + KxR, R.y + R.h - KxR, KxPi * 0.5f, KxPi);

                                // Draw only the exposed part of each seam edge. The inset
                                // is the corner radius when that corner is actually rounded;
                                // for a square seam corner the segment runs all the way to
                                // the row's edge, closing the step instead of leaving a gap.
                                if(OverA)
                                {
                                        const float TopL = R.x + (TLr ? KxR : 0.0f);
                                        if(AC0 > TopL)
                                                AddSeg(TopL, R.y, AC0, R.y);
                                        const float TopR = R.x + R.w - (TRr ? KxR : 0.0f);
                                        if(TopR > AC1)
                                                AddSeg(AC1, R.y, TopR, R.y);
                                }
                                else
                                        AddSeg(R.x + (TLr ? KxR : 0.0f), R.y, R.x + R.w - (TRr ? KxR : 0.0f), R.y);

                                if(OverB)
                                {
                                        const float BotL = R.x + (BLr ? KxR : 0.0f);
                                        if(BC0 > BotL)
                                                AddSeg(BotL, R.y + R.h, BC0, R.y + R.h);
                                        const float BotR = R.x + R.w - (BRr ? KxR : 0.0f);
                                        if(BotR > BC1)
                                                AddSeg(BC1, R.y + R.h, BotR, R.y + R.h);
                                }
                                else
                                        AddSeg(R.x + (BLr ? KxR : 0.0f), R.y + R.h, R.x + R.w - (BRr ? KxR : 0.0f), R.y + R.h);
                        }

                        if(!vLines.empty())
                        {
                                Graphics()->LinesBegin();
                                Graphics()->SetColor(BorderCol.r, BorderCol.g, BorderCol.b, BorderCol.a);
                                Graphics()->LinesDraw(vLines.data(), vLines.size());
                                Graphics()->LinesEnd();
                        }
                        for(size_t i = 0; i < Count; ++i)
                        {
                                const SKxRect &R = vRects[i];
                                float ly = R.y + PadY;
                                for(const std::string &Line : vItemLines[i])
                                {
                                        DrawRowText(R.x + PadX, ly, Line.c_str(), i);
                                        ly += FontSz;
                                }
                        }
                }
        }

        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ============================================================================
// v6.4: ArrayInfo HUD — info line rendered in game.
// ============================================================================

bool CClickGui::ShouldRenderArrayInfo() const
{
        if(g_Config.m_KxArrayInfoEnabled == 0 && !m_ArrayInfoEditor)
                return false;
        if(GameClient()->m_Menus.IsActive())
                return false;
        const int State = Client()->State();
        return State == IClient::STATE_ONLINE || State == IClient::STATE_DEMOPLAYBACK;
}

bool CClickGui::ArrayInfoPositionModified() const
{
        return g_Config.m_KxArrayInfoPosX != KX_ARRAYINFO_DEF_POS_X ||
                g_Config.m_KxArrayInfoPosY != KX_ARRAYINFO_DEF_POS_Y;
}

void CClickGui::ResetArrayInfoPosition()
{
        g_Config.m_KxArrayInfoPosX = KX_ARRAYINFO_DEF_POS_X;
        g_Config.m_KxArrayInfoPosY = KX_ARRAYINFO_DEF_POS_Y;
}

bool CClickGui::ArrayInfoHitTest(vec2 mousePos) const
{
        if(!m_ArrayInfoRectValid)
                return false;
        return mousePos.x >= m_ArrayInfoRectX && mousePos.x <= m_ArrayInfoRectX + m_ArrayInfoRectW &&
                mousePos.y >= m_ArrayInfoRectY && mousePos.y <= m_ArrayInfoRectY + m_ArrayInfoRectH;
}

void CClickGui::UpdateArrayInfo(float dt)
{
        m_ArrayInfoHue += dt * (g_Config.m_KxArrayInfoRainbowSpeed / 10.0f) * 60.0f;

        if(!m_ArrayInfoDragging || !m_ArrayInfoRectValid)
                return;

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        // Relative drag: press position -> current position delta. While the
        // editor is open the drag is deliberately unclamped, so the bar never
        // bumps against the screen edges; the in-game render keeps it visible.
        const float Left = m_ArrayInfoDragStartPos.x + (m_MousePos.x - m_ArrayInfoDragStartMouse.x);
        const float Top = m_ArrayInfoDragStartPos.y + (m_MousePos.y - m_ArrayInfoDragStartMouse.y);

        // The stored point is always the info box's left edge; the X anchor
        // only aligns the lines inside the box, it never repositions the box.
        g_Config.m_KxArrayInfoPosX = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
        g_Config.m_KxArrayInfoPosY = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
}

void CClickGui::RenderArrayInfo(float alpha)
{
        m_ArrayInfoRectValid = false;

        const bool UiOn = g_Config.m_KxUiSettingsOn != 0;
        const float SizeScale = (UiOn ? g_Config.m_KxArrayInfoSize : 10) / 10.0f;
        const float FontSz = 15.0f * SizeScale;
        const float PadX = 7.0f * SizeScale;
        const float PadY = 4.0f * SizeScale;
        const float LineGap = (UiOn ? (float)g_Config.m_KxArrayInfoIndent : 2.0f) * SizeScale;

        const int Style = g_Config.m_KxArrayInfoStyle;
        const bool GroupStyle = (Style == 1 || Style == 3);
        const bool DDNetStyle = (Style >= 2);
        const bool Rainbow = g_Config.m_KxArrayInfoRainbow != 0;
        const int RainbowMode = g_Config.m_KxArrayInfoRainbowMode;
        const float RainbowStep = (float)g_Config.m_KxArrayInfoRainbowStep;
        const float TextOp = (UiOn ? g_Config.m_KxArrayInfoTextOpacity : 100) / 100.0f;
        const float MainOp = (UiOn ? g_Config.m_KxArrayInfoMainOpacity : 100) / 100.0f;
        const float PanelOp = (UiOn ? g_Config.m_KxArrayInfoPanelOpacity : 100) / 100.0f;
        const float MaxLen = (UiOn ? (float)g_Config.m_KxArrayInfoMaxLength : 0.0f);
        const bool Editor = m_ArrayInfoEditor;

        unsigned TextHex = 0xffffff;
        sscanf(g_Config.m_KxArrayInfoTextColor, "%6x", &TextHex);

        // ---- info pieces (order: logo | watermark | FPS | session | tee | nick | functions) ----
        struct SPiece { int Kind; std::string Text; float Width; }; // 0=text, 1=logo, 2=tee
        std::vector<SPiece> vPieces;

        const float LogoH = FontSz;
        const float LogoW = LogoH;
        const float TeeSz = FontSz;

        auto AddTextPiece = [&](const char *pText) {
                if(!pText || !pText[0])
                        return;
                SPiece P;
                P.Kind = 0;
                P.Text = pText;
                P.Width = TextRender()->TextWidth(FontSz, pText);
                vPieces.push_back(P);
        };
        auto AddIconPiece = [&](int Kind, float W) {
                SPiece P;
                P.Kind = Kind;
                P.Width = W;
                vPieces.push_back(P);
        };

        if(g_Config.m_KxArrayInfoLogo || Editor)
                AddIconPiece(1, LogoW);
        if(g_Config.m_KxArrayInfoWatermark[0] || Editor)
                AddTextPiece(g_Config.m_KxArrayInfoWatermark);
        if(g_Config.m_KxArrayInfoShowFps || Editor)
        {
                char aBuf[64];
                const float Avg = Client()->FrameTimeAverage();
                const int Fps = (int)(1.0f / std::max(0.0001f, Avg) + 0.5f);
                str_format(aBuf, sizeof(aBuf), "%d FPS", Fps);
                AddTextPiece(aBuf);
        }
        if(g_Config.m_KxArrayInfoSessionTime || Editor)
        {
                char aBuf[64];
                const int Minutes = (int)(Client()->GlobalTime() / 60.0f);
                str_format(aBuf, sizeof(aBuf), "%d Minute(s)", Minutes);
                AddTextPiece(aBuf);
        }
        if(g_Config.m_KxArrayInfoTee || Editor)
                AddIconPiece(2, TeeSz);
        if(g_Config.m_KxArrayInfoNickname || Editor)
        {
                const char *pNick = g_Config.m_PlayerName[0] ? g_Config.m_PlayerName : "tee";
                AddTextPiece(pNick);
        }
        if(g_Config.m_KxArrayInfoFunctions || Editor)
        {
                // v6.7: count the REAL enabled list (ArrayListBuildEnabled) — the
                // same source the ArrayList renders — so the "X Functions" count
                // always matches the list. Editor mode shows the sample list but
                // still counts the real one.
                std::vector<std::string> vFuncs;
                ArrayListBuildEnabled(vFuncs);
                char aBuf[64];
                str_format(aBuf, sizeof(aBuf), "%d Functions", (int)vFuncs.size());
                AddTextPiece(aBuf);
        }

        if(vPieces.empty())
                return;

        const float SepW = TextRender()->TextWidth(FontSz, " | ");
        // Ink width of the bar itself (first-character bearing is suppressed in
        // both TextWidth and DrawText, so this is exactly the drawn width).
        const float BarW = TextRender()->TextWidth(FontSz, "|");

        // ---- wrap pieces into lines at the physical Max Length ----
        std::vector<std::vector<int>> vLines;
        std::vector<float> vLineW;
        {
                std::vector<int> vCur;
                float CurW = 0.0f;
                for(int i = 0; i < (int)vPieces.size(); ++i)
                {
                        const float AddW = vPieces[i].Width + (vCur.empty() ? 0.0f : SepW);
                        if(MaxLen > 0.0f && !vCur.empty() && CurW + AddW > MaxLen)
                        {
                                vLines.push_back(vCur);
                                vLineW.push_back(CurW);
                                vCur.clear();
                                vCur.push_back(i);
                                CurW = vPieces[i].Width;
                        }
                        else
                        {
                                vCur.push_back(i);
                                CurW += AddW;
                        }
                }
                if(!vCur.empty())
                {
                        vLines.push_back(vCur);
                        vLineW.push_back(CurW);
                }
        }

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        if(ScreenW <= 0.0f || ScreenH <= 0.0f)
                return;

        float BoxW = 0.0f;
        for(float W : vLineW)
                BoxW = std::max(BoxW, W + PadX * 2.0f);
        const float RowH = FontSz + PadY * 2.0f;
        float BoxH;
        if(GroupStyle)
                BoxH = (float)vLines.size() * FontSz + (float)(vLines.size() - 1) * LineGap + PadY * 2.0f;
        else
                BoxH = (float)vLines.size() * RowH + (float)(vLines.size() - 1) * LineGap;

        const float XRatio = g_Config.m_KxArrayInfoPosX / 10000.0f;
        int XA = g_Config.m_KxArrayInfoXAnchor;
        if(XA == 0)
                XA = (XRatio < 1.0f / 3.0f) ? 3 : ((XRatio > 2.0f / 3.0f) ? 1 : 2);
        const float AnchorX = XRatio * ScreenW;
        const float AnchorY = (g_Config.m_KxArrayInfoPosY / 10000.0f) * ScreenH;
        // The info bar itself never moves with the X anchor: the stored point
        // is always the box's left edge and the anchor only aligns the lines
        // inside the box (same rule as the Group ArrayList).
        float Left = AnchorX;
        float Top = AnchorY;
        if(!m_ArrayInfoEditor)
        {
                // In game the bar is kept on screen. The editor preview is
                // dragged freely and must not bump against the screen edges.
                Left = std::clamp(Left, 0.0f, std::max(0.0f, ScreenW - BoxW));
                Top = std::clamp(Top, 0.0f, std::max(0.0f, ScreenH - BoxH));
        }

        m_ArrayInfoRectX = Left;
        m_ArrayInfoRectY = Top;
        m_ArrayInfoRectW = BoxW;
        m_ArrayInfoRectH = BoxH;
        m_ArrayInfoRectValid = true;

        // Start the editor drag here as well: this runs in both the HUD-only
        // path and the ClickGUI path, and the rect above is guaranteed current
        // (the early-path poll can see the previous frame's rect on the first
        // frame after entering the editor).
        if(m_ArrayInfoEditor && !m_ArrayInfoDragging && Input()->KeyIsPressed(KEY_MOUSE_1) && ArrayInfoHitTest(m_MousePos))
        {
                m_ArrayInfoDragging = true;
                m_ArrayInfoDragStartMouse = m_MousePos;
                m_ArrayInfoDragStartPos = vec2(m_ArrayInfoRectX, m_ArrayInfoRectY);
        }

        const ColorRGBA BorderCol = DDNetStyle
                ? ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f * alpha * MainOp)
                : KxColorWithAlpha(m_AccentColorRGBA, alpha * MainOp);
        const ColorRGBA BgCol = DDNetStyle
                ? ColorRGBA(0.2f, 0.2f, 0.2f, 0.8f * alpha * PanelOp)
                : KxColorWithAlpha(m_PanelBgRGBA, alpha * 0.97f * PanelOp);

        auto DrawBlock = [&](float x, float y, float w, float h) {
                DrawRoundedRect(x, y, w, h, 4.0f, IGraphics::CORNER_ALL, BgCol);
                DrawRoundedOutline(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 3.5f, BorderCol, true, true);
        };

        auto DrawTextColored = [&](float x, float y, const char *pText, int LineIdx) {
                if(!Rainbow)
                {
                        DrawText(x, y, FontSz, pText, ColorRGBA(TextHex, true).WithAlpha(alpha * TextOp));
                        return;
                }
                if(RainbowMode == 2)
                {
                        // Draw the whole piece in one TextEx call with per-character
                        // colour splits, so glyph bearings and kerning stay intact.
                        std::vector<STextColorSplit> vSplits;
                        vSplits.reserve((size_t)str_length(pText));
                        const char *pCur = pText;
                        int BytePos = 0;
                        while(*pCur)
                        {
                                const char *pNext = pCur;
                                str_utf8_decode(&pNext);
                                const int CharLen = (int)(pNext - pCur);
                                const float CharX = x + TextRender()->TextWidth(FontSz, pText, BytePos);
                                const float t = (BoxW > 0.0f) ? std::clamp((CharX - Left) / BoxW, 0.0f, 1.0f) : 0.0f;
                                float R, G, B;
                                KxHueToRgb(m_ArrayInfoHue - RainbowStep * t, R, G, B);
                                vSplits.emplace_back(BytePos, CharLen, ColorRGBA(R, G, B, alpha * TextOp));
                                BytePos += CharLen;
                                pCur = pNext;
                        }
                        KxDrawTextColorSplits(TextRender(), x, y, FontSz, pText, ColorRGBA(1.0f, 1.0f, 1.0f, alpha * TextOp), vSplits);
                        return;
                }
                float R, G, B;
                KxHueToRgb(m_ArrayInfoHue + (float)LineIdx * (RainbowMode == 1 ? RainbowStep : 0.0f), R, G, B);
                DrawText(x, y, FontSz, pText, ColorRGBA(R, G, B, alpha * TextOp));
        };

        auto DrawPiece = [&](float &x, float y, int PieceIdx, int LineIdx) {
                const SPiece &P = vPieces[PieceIdx];
                if(P.Kind == 0)
                        DrawTextColored(x, y, P.Text.c_str(), LineIdx);
                else if(P.Kind == 1)
                {
                        if(m_ArrayInfoLogo.IsValid())
                        {
                                Graphics()->BlendNormal();
                                Graphics()->TextureSet(m_ArrayInfoLogo);
                                Graphics()->QuadsBegin();
                                Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, alpha * TextOp));
                                IGraphics::CQuadItem Quad(x, y, LogoW, LogoH);
                                Graphics()->QuadsDrawTL(&Quad, 1);
                                Graphics()->QuadsEnd();
                                Graphics()->TextureClear();
                        }
                }
                else if(P.Kind == 2)
                {
                        const auto *pSkin = GameClient()->m_Skins.Find(g_Config.m_ClPlayerSkin);
                        if(pSkin)
                        {
                                CTeeRenderInfo Info;
                                Info.Apply(pSkin);
                                Info.m_Size = TeeSz;
                                vec2 OffsetToMid;
                                CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &Info, OffsetToMid);
                                RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, EMOTE_NORMAL, vec2(1.0f, 0.0f),
                                        vec2(x + TeeSz * 0.5f, y + FontSz * 0.5f + OffsetToMid.y), alpha * TextOp);
                        }
                }
                x += P.Width;
        };

        if(GroupStyle)
        {
                DrawBlock(Left, Top, BoxW, BoxH);
                float ty = Top + PadY;
                for(size_t i = 0; i < vLines.size(); ++i)
                {
                        float tx = Left + PadX;
                        if(XA == 2)
                                tx = Left + (BoxW - vLineW[i]) * 0.5f;
                        else if(XA == 1)
                                tx = Left + BoxW - PadX - vLineW[i];
                        const std::vector<int> &Line = vLines[i];
                        for(size_t k = 0; k < Line.size(); ++k)
                        {
                                if(k > 0)
                                {
                                        // Center the bar in the fixed separator slot. Drawing
                                        // " | " offsets it by the bar's left bearing, which
                                        // made the gap before it look wider than after it.
                                        DrawTextColored(tx + (SepW - BarW) * 0.5f, ty, "|", (int)i);
                                        tx += SepW;
                                }
                                DrawPiece(tx, ty, Line[k], (int)i);
                        }
                        ty += FontSz + LineGap;
                }
        }
        else
        {
                float ry = Top;
                for(size_t i = 0; i < vLines.size(); ++i)
                {
                        const float bw = vLineW[i] + PadX * 2.0f;
                        // The anchor aligns each line inside the fixed info box.
                        float bx;
                        if(XA == 3)
                                bx = Left;
                        else if(XA == 2)
                                bx = Left + (BoxW - bw) * 0.5f;
                        else
                                bx = Left + BoxW - bw;
                        DrawBlock(bx, ry, bw, RowH);
                        float tx = bx + PadX;
                        const std::vector<int> &Line = vLines[i];
                        for(size_t k = 0; k < Line.size(); ++k)
                        {
                                if(k > 0)
                                {
                                        // Center the bar in the fixed separator slot (see above).
                                        DrawTextColored(tx + (SepW - BarW) * 0.5f, ry + PadY, "|", (int)i);
                                        tx += SepW;
                                }
                                DrawPiece(tx, ry + PadY, Line[k], (int)i);
                        }
                        ry += RowH + LineGap;
                }
        }

        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ============================================================================
// v6.7: ArrayPlayer HUD — now-playing media card.
// ============================================================================

bool CClickGui::ShouldRenderArrayPlayer() const
{
        if(g_Config.m_KxArrayPlayerEnabled == 0 && !m_ArrayPlayerEditor)
                return false;
        if(GameClient()->m_Menus.IsActive())
                return false;
        const int State = Client()->State();
        return State == IClient::STATE_ONLINE || State == IClient::STATE_DEMOPLAYBACK;
}

bool CClickGui::ArrayPlayerPositionModified() const
{
        return g_Config.m_KxArrayPlayerPosX != KX_ARRAYPLAYER_DEF_POS_X ||
                g_Config.m_KxArrayPlayerPosY != KX_ARRAYPLAYER_DEF_POS_Y;
}

void CClickGui::ResetArrayPlayerPosition()
{
        g_Config.m_KxArrayPlayerPosX = KX_ARRAYPLAYER_DEF_POS_X;
        g_Config.m_KxArrayPlayerPosY = KX_ARRAYPLAYER_DEF_POS_Y;
}

bool CClickGui::ArrayPlayerHitTest(vec2 mousePos) const
{
        if(!m_ArrayPlayerRectValid)
                return false;
        return mousePos.x >= m_ArrayPlayerRectX && mousePos.x <= m_ArrayPlayerRectX + m_ArrayPlayerRectW &&
                mousePos.y >= m_ArrayPlayerRectY && mousePos.y <= m_ArrayPlayerRectY + m_ArrayPlayerRectH;
}

bool CClickGui::ArrayPlayerResizeHitTest(vec2 mousePos) const
{
        if(!m_ArrayPlayerRectValid || !m_ArrayPlayerEditor || g_Config.m_KxArrayPlayerDynamicScale == 0)
                return false;
        const float Zone = 16.0f;
        return mousePos.x >= m_ArrayPlayerRectX + m_ArrayPlayerRectW - Zone &&
                mousePos.x <= m_ArrayPlayerRectX + m_ArrayPlayerRectW &&
                mousePos.y >= m_ArrayPlayerRectY + m_ArrayPlayerRectH - Zone &&
                mousePos.y <= m_ArrayPlayerRectY + m_ArrayPlayerRectH;
}

void CClickGui::UpdateArrayPlayer(float dt)
{
        m_ArrayPlayerHue += dt * (g_Config.m_KxArrayPlayerRainbowSpeed / 10.0f) * 60.0f;

        if(!m_ArrayPlayerRectValid)
                return;

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        if(ScreenW <= 0.0f || ScreenH <= 0.0f)
                return;

        const float SizeScale = (g_Config.m_KxUiSettingsOn != 0) ? (g_Config.m_KxArrayPlayerSize / 10.0f) : 1.0f;
        const float S = std::max(0.01f, SizeScale);

        if(m_ArrayPlayerResizing)
        {
                const int W = (int)(m_ArrayPlayerResizeStartW + (m_MousePos.x - m_ArrayPlayerResizeStartMouse.x) / S + 0.5f);
                const int H = (int)(m_ArrayPlayerResizeStartH + (m_MousePos.y - m_ArrayPlayerResizeStartMouse.y) / S + 0.5f);
                g_Config.m_KxArrayPlayerWidth = std::clamp(W, 100, 1000);
                g_Config.m_KxArrayPlayerHeight = std::clamp(H, 40, 600);
                return;
        }
        if(!m_ArrayPlayerDragging)
                return;

        const float Left = m_ArrayPlayerDragStartPos.x + (m_MousePos.x - m_ArrayPlayerDragStartMouse.x);
        const float Top = m_ArrayPlayerDragStartPos.y + (m_MousePos.y - m_ArrayPlayerDragStartMouse.y);
        g_Config.m_KxArrayPlayerPosX = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
        g_Config.m_KxArrayPlayerPosY = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
}

static void KxEllipsize(char *pDst, int DstSize, const char *pSrc, float MaxW, float FontSz, ITextRender *pTextRender)
{
        if(DstSize <= 0)
                return;
        str_copy(pDst, pSrc, DstSize);
        if(MaxW <= 0.0f || pTextRender->TextWidth(FontSz, pDst) <= MaxW)
                return;

        int Len = str_length(pDst);
        while(Len > 0)
        {
                --Len;
                while(Len > 0 && (pDst[Len] & 0xC0) == 0x80)
                        --Len;
                pDst[Len] = '\0';
                char aTry[256];
                str_format(aTry, sizeof(aTry), "%s...", pDst);
                if(pTextRender->TextWidth(FontSz, aTry) <= MaxW)
                {
                        str_copy(pDst, aTry, DstSize);
                        return;
                }
        }
        str_copy(pDst, "...", DstSize);
}

// v6.7: rounds the corners of a decoded RGBA image in place (alpha fades to 0
// outside the rounded rect). The ArrayPlayer cover uses it because there is
// no rounded texture clipping in IGraphics.
static void KxRoundImageCorners(CImageInfo &Img, float Radius)
{
	if(Img.m_Format != CImageInfo::FORMAT_RGBA || !Img.m_pData ||
		Img.m_Width == 0 || Img.m_Height == 0)
		return;
	const int Wp = (int)Img.m_Width;
	const int Hp = (int)Img.m_Height;
	const float Rad = std::clamp(Radius, 1.0f, std::min(Wp, Hp) * 0.5f);
	uint8_t *pData = Img.m_pData;
	for(int y = 0; y < Hp; ++y)
	{
		for(int x = 0; x < Wp; ++x)
		{
			const float Cx = std::min((float)x, (float)(Wp - 1 - x));
			const float Cy = std::min((float)y, (float)(Hp - 1 - y));
			if(Cx >= Rad || Cy >= Rad)
				continue; // not in a corner zone
			const float Dx = Rad - Cx;
			const float Dy = Rad - Cy;
			const float Dist = std::sqrt(Dx * Dx + Dy * Dy);
			const float A = std::clamp(Rad - Dist + 1.0f, 0.0f, 1.0f); // ~1px feather
			if(A < 1.0f)
				pData[(y * Wp + x) * 4 + 3] = (uint8_t)(pData[(y * Wp + x) * 4 + 3] * A);
		}
	}
}

void CClickGui::RenderArrayPlayer(float alpha)
{
        m_ArrayPlayerRectValid = false;

        KxMedia::Update();
        const SKxMediaInfo Info = KxMedia::Get();

        // No track and not editing -> nothing to draw (the user asked for the
        // player to disappear entirely when nothing is playing).
        if(!Info.m_Valid && !m_ArrayPlayerEditor)
                return;

        const bool UiOn = g_Config.m_KxUiSettingsOn != 0;
        const float S = (UiOn ? g_Config.m_KxArrayPlayerSize : 10) / 10.0f;
        const float W = (float)(UiOn ? g_Config.m_KxArrayPlayerWidth : 320) * S;
        const float H = (float)(UiOn ? g_Config.m_KxArrayPlayerHeight : 90) * S;
        if(W <= 0.0f || H <= 0.0f)
                return;

        const float Pad = 12.0f * S; // v6.7: 1.5x the old 8px inset
        const float FontSz = 15.0f * S;
        const float TitleSz = FontSz * 1.3f; // v6.7: bigger track title
        const float ArtistSz = FontSz * 0.75f; // v6.7: smaller artist line
        const float TimeSz = 13.0f * S; // v6.7: bigger time label
        const float Indent = (UiOn ? (float)g_Config.m_KxArrayPlayerIndent : 4.0f) * S;
        const float TextOp = (UiOn ? g_Config.m_KxArrayPlayerTextOpacity : 100) / 100.0f;
        const float MainOp = (UiOn ? g_Config.m_KxArrayPlayerMainOpacity : 100) / 100.0f;
        const float PanelOp = (UiOn ? g_Config.m_KxArrayPlayerPanelOpacity : 100) / 100.0f;
        const bool DDNetStyle = UiOn && g_Config.m_KxArrayPlayerStyle == 1;
        const bool Rainbow = UiOn && g_Config.m_KxArrayPlayerRainbow != 0;
        const int RainbowMode = g_Config.m_KxArrayPlayerRainbowMode;
        const float RainbowStep = (float)g_Config.m_KxArrayPlayerRainbowStep;

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        if(ScreenW <= 0.0f || ScreenH <= 0.0f)
                return;

        float Left = (g_Config.m_KxArrayPlayerPosX / 10000.0f) * ScreenW;
        float Top = (g_Config.m_KxArrayPlayerPosY / 10000.0f) * ScreenH;
        if(!m_ArrayPlayerEditor)
        {
                Left = std::clamp(Left, 0.0f, std::max(0.0f, ScreenW - W));
                Top = std::clamp(Top, 0.0f, std::max(0.0f, ScreenH - H));
        }

        m_ArrayPlayerRectX = Left;
        m_ArrayPlayerRectY = Top;
        m_ArrayPlayerRectW = W;
        m_ArrayPlayerRectH = H;
        m_ArrayPlayerRectValid = true;

        // ---- layout ----
        const float BarsH = 14.0f * S;
        // Bottom row (mockup): thick progress line at the very bottom, the
        // time label sits above the slider (right) and the visualizer bars
        // stand on the slider; the media buttons live ON the cover row (no
        // dedicated row), centered below the title/artist text block.
        const float LineY = Top + H - Pad;
        const float TrackH = std::max(2.0f, 2.5f * S); // progress line thickness
        const float BtnH = 20.0f * S; // media buttons
        const float BtnW = 26.0f * S;
        const float BarsBase = LineY - TrackH * 0.5f - 4.0f * S; // bars right above the slider
        const float TimeY = BarsBase - TimeSz;
        const float TextAreaTop = Top + Pad;
        const float TextAreaBottom = BarsBase - std::max(BarsH, TimeSz) - 4.0f * S;
        // Buttons sit at the BOTTOM edge of the cover (aligned with TextAreaBottom).
        const float ButtonsTop = TextAreaBottom - BtnH;

        // ---- album art (decode once per track; the media thread already
        // decoded the cover to RGBA via WIC) ----
        if(Info.m_ArtSerial != m_ArrayPlayerArtSerial || (Info.m_vArt.empty() && m_ArrayPlayerArt.IsValid()))
        {
                m_ArrayPlayerArtSerial = Info.m_ArtSerial;
                if(m_ArrayPlayerArt.IsValid())
                        Graphics()->UnloadTexture(&m_ArrayPlayerArt);
                if(!Info.m_vArt.empty() && Info.m_ArtW > 0 && Info.m_ArtH > 0)
                {
                        CImageInfo Img;
                        Img.m_Width = Info.m_ArtW;
                        Img.m_Height = Info.m_ArtH;
                        Img.m_Format = CImageInfo::FORMAT_RGBA;
                        Img.Allocate();
                        mem_copy(Img.m_pData, Info.m_vArt.data(), std::min(Img.DataSize(), Info.m_vArt.size()));
                        KxRoundImageCorners(Img, std::min(Img.m_Width, Img.m_Height) * 0.12f);
                        m_ArrayPlayerArt = Graphics()->LoadTextureRawMove(Img, 0);
                }
        }

        unsigned TextHex = 0xffffff;
        sscanf(g_Config.m_KxArrayPlayerTextColor, "%6x", &TextHex);

        const ColorRGBA BorderCol = DDNetStyle
                ? ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f * alpha * MainOp)
                : KxColorWithAlpha(m_AccentColorRGBA, alpha * MainOp);
        const ColorRGBA BgCol = DDNetStyle
                ? ColorRGBA(0.2f, 0.2f, 0.2f, 0.8f * alpha * PanelOp)
                : KxColorWithAlpha(m_PanelBgRGBA, alpha * 0.97f * PanelOp);

        DrawRoundedRect(Left, Top, W, H, 5.0f * S, IGraphics::CORNER_ALL, BgCol);
        DrawRoundedOutline(Left + 0.5f, Top + 0.5f, W - 1.0f, H - 1.0f, 4.5f * S, BorderCol, true, true);

        const bool HasArt = m_ArrayPlayerArt.IsValid() || (m_ArrayPlayerEditor && !Info.m_Valid);
        // Mockup: the cover spans the TEXT ZONE height (title+artist area),
        // leaving the bottom row clear — not the full card height.
        const float ArtSize = std::max(0.0f, std::min(H - Pad * 2.0f, TextAreaBottom - TextAreaTop));
        float TextX = Left + Pad;
        if(HasArt)
                TextX += ArtSize + Pad;
        const float TextW = std::max(10.0f, Left + W - Pad - TextX);

        // Title top-aligned with the cover (mockup), artist right below.
        const float TextY = TextAreaTop;

        int XA = UiOn ? g_Config.m_KxArrayPlayerXAnchor : 0;
        if(XA == 0)
                XA = 3; // default: left-aligned inside the card

        auto DrawPlayerText = [&](float x, float y, const char *pText, int LineIdx, float Sz, float AlphaMul, bool Bold) {
                // Fake bold: the text renderer has no font weight, so titles are
                // drawn twice with a small x offset.
                const float BoldDx = Bold ? std::max(0.5f, Sz * 0.05f) : 0.0f;
                for(int Pass = 0; Pass < (Bold ? 2 : 1); ++Pass)
                {
                        const float Px = x + Pass * BoldDx;
                        if(!Rainbow)
                        {
                                DrawText(Px, y, Sz, pText, ColorRGBA(TextHex, true).WithAlpha(alpha * TextOp * AlphaMul));
                                continue;
                        }
                        if(RainbowMode == 2)
                        {
                                std::vector<STextColorSplit> vSplits;
                                vSplits.reserve((size_t)str_length(pText));
                                const char *pCur = pText;
                                int BytePos = 0;
                                while(*pCur)
                                {
                                        const char *pNext = pCur;
                                        str_utf8_decode(&pNext);
                                        const int CharLen = (int)(pNext - pCur);
                                        const float CharX = Px + TextRender()->TextWidth(Sz, pText, BytePos);
                                        const float t = (W > 0.0f) ? std::clamp((CharX - Left) / W, 0.0f, 1.0f) : 0.0f;
                                        float R, G, B;
                                        KxHueToRgb(m_ArrayPlayerHue - RainbowStep * t, R, G, B);
                                        vSplits.emplace_back(BytePos, CharLen, ColorRGBA(R, G, B, alpha * TextOp * AlphaMul));
                                        BytePos += CharLen;
                                        pCur = pNext;
                                }
                                KxDrawTextColorSplits(TextRender(), Px, y, Sz, pText, ColorRGBA(1.0f, 1.0f, 1.0f, alpha * TextOp * AlphaMul), vSplits);
                                continue;
                        }
                        float R, G, B;
                        KxHueToRgb(m_ArrayPlayerHue + (float)LineIdx * (RainbowMode == 1 ? RainbowStep : 0.0f), R, G, B);
                        DrawText(Px, y, Sz, pText, ColorRGBA(R, G, B, alpha * TextOp * AlphaMul));
                }
        };

        // ---- art ----
        if(HasArt)
        {
                if(m_ArrayPlayerArt.IsValid())
                {
                        Graphics()->BlendNormal();
                        Graphics()->TextureSet(m_ArrayPlayerArt);
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, alpha * TextOp));
                        IGraphics::CQuadItem Quad(Left + Pad, Top + Pad, ArtSize, ArtSize);
                        Graphics()->QuadsDrawTL(&Quad, 1);
                        Graphics()->QuadsEnd();
                        Graphics()->TextureClear();
                }
                else
                {
                        DrawRoundedRect(Left + Pad, Top + Pad, ArtSize, ArtSize, 4.0f * S,
                                IGraphics::CORNER_ALL, ColorRGBA(0.0f, 0.0f, 0.0f, alpha * 0.65f));
                }
        }

        // ---- title / artist ----
        const char *pTitleSrc = (Info.m_Valid && Info.m_aTitle[0]) ? Info.m_aTitle : (m_ArrayPlayerEditor ? "Track Title" : "Unknown");
        const char *pArtistSrc = (Info.m_Valid && Info.m_aArtist[0]) ? Info.m_aArtist : (m_ArrayPlayerEditor ? "Artist" : "Unknown");
        char aTitle[256];
        char aArtist[256];
        KxEllipsize(aTitle, sizeof(aTitle), pTitleSrc, TextW, TitleSz, TextRender());
        KxEllipsize(aArtist, sizeof(aArtist), pArtistSrc, TextW, ArtistSz, TextRender());

        auto AlignTextX = [&](const char *pText, float Sz) {
                const float tw = TextRender()->TextWidth(Sz, pText);
                if(XA == 2)
                        return TextX + (TextW - tw) * 0.5f;
                if(XA == 1)
                        return TextX + TextW - tw;
                return TextX;
        };
        DrawPlayerText(AlignTextX(aTitle, TitleSz), TextY, aTitle, 0, TitleSz, 1.0f, true);
        DrawPlayerText(AlignTextX(aArtist, ArtistSz), TextY + TitleSz + Indent, aArtist, 1, ArtistSz, 0.6f, false);

        // ---- time / progress ----
        const double Duration = Info.m_Valid ? Info.m_Duration : 180.0;
        const double Position = Info.m_Valid ? KxMedia::GetPosition() : (m_ArrayPlayerEditor ? 72.0 : 0.0);
        const int PosSec = (int)Position;
        const int DurSec = (int)Duration;
        char aTime[64];
        str_format(aTime, sizeof(aTime), "%d:%02d / %d:%02d", PosSec / 60, PosSec % 60, DurSec / 60, DurSec % 60);
        const float TimeW = TextRender()->TextWidth(TimeSz, aTime);
        const float LineX0 = Left + Pad;
        const float LineX1 = Left + W - Pad;
        const float Ratio = Duration > 0.0 ? (float)std::clamp(Position / Duration, 0.0, 1.0) : 0.0f;

        // v6.7: thick line + bigger thumb, both follow the rainbow settings.
        const float ThumbD = 10.0f * S;
        ColorRGBA TrackBgCol;
        if(DDNetStyle)
                TrackBgCol = ColorRGBA(0.0f, 0.0f, 0.0f, alpha * 0.35f * MainOp);
        else
        {
                const ColorRGBA Tint = KxColorWithAlpha(m_PanelBgRGBA, 1.0f);
                TrackBgCol = ColorRGBA(std::min(1.0f, Tint.r * 2.5f), std::min(1.0f, Tint.g * 2.5f), std::min(1.0f, Tint.b * 2.5f), alpha * 0.8f * MainOp);
        }
        DrawRoundedRect(LineX0, LineY - TrackH * 0.5f, LineX1 - LineX0, TrackH, TrackH * 0.5f, IGraphics::CORNER_ALL, TrackBgCol);

        ColorRGBA ProgressCol;
        if(Rainbow)
        {
                float R, G, B;
                KxHueToRgb(m_ArrayPlayerHue + (RainbowMode == 0 ? 0.0f : RainbowStep * Ratio), R, G, B);
                ProgressCol = ColorRGBA(R, G, B, alpha * MainOp);
        }
        else
                ProgressCol = DDNetStyle ? ColorRGBA(0.9f, 0.9f, 0.9f, alpha * MainOp) : KxColorWithAlpha(m_AccentColorRGBA, alpha * MainOp);

        if(Ratio > 0.0f)
        {
                DrawRoundedRect(LineX0, LineY - TrackH * 0.5f, (LineX1 - LineX0) * Ratio, TrackH, TrackH * 0.5f,
                        IGraphics::CORNER_ALL, ProgressCol);
                DrawRoundedRect(LineX0 + (LineX1 - LineX0) * Ratio - ThumbD * 0.5f, LineY - ThumbD * 0.5f, ThumbD, ThumbD, ThumbD * 0.5f,
                        IGraphics::CORNER_ALL, ProgressCol);
        }

        DrawText(LineX1, TimeY, TimeSz, aTime,
                ColorRGBA(TextHex, true).WithAlpha(alpha * 0.8f * TextOp), TEXTALIGN_RIGHT);

        // ---- media transport buttons: previous / play-pause / next ----
        // Centered in the span between the cover and the right card edge,
        // directly above the slider. Icons are drawn from primitives (bars and
        // freeform quads) — the game font has no media glyphs.
        const float BtnGap = 8.0f * S;
        const float BtnGroupW = BtnW * 3.0f + BtnGap * 2.0f;
        // Centered on the FULL card width (left..right edges), not the text span.
        const float BtnX0 = Left + (W - BtnGroupW) * 0.5f;
        for(int b = 0; b < 3; ++b)
        {
                const float bx = BtnX0 + b * (BtnW + BtnGap);
                m_aArrayPlayerBtnRect[b][0] = bx;
                m_aArrayPlayerBtnRect[b][1] = ButtonsTop;
                m_aArrayPlayerBtnRect[b][2] = BtnW;
                m_aArrayPlayerBtnRect[b][3] = BtnH;
                const bool Hover = !m_ArrayPlayerEditor && PointInRect(m_MousePos, bx, ButtonsTop, BtnW, BtnH);
                if(Hover)
                        DrawRoundedRect(bx, ButtonsTop, BtnW, BtnH, 3.0f * S, IGraphics::CORNER_ALL,
                                ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.08f));

                ColorRGBA IconCol;
                if(Rainbow)
                {
                        float R, G, B;
                        KxHueToRgb(m_ArrayPlayerHue - RainbowStep * (RainbowMode == 0 ? 0.0f : (float)b / 2.0f), R, G, B);
                        IconCol = ColorRGBA(R, G, B, alpha * TextOp);
                }
                else
                        IconCol = ColorRGBA(TextHex, true).WithAlpha(alpha * TextOp * (Hover ? 1.0f : 0.8f));

                const float cy = ButtonsTop + BtnH * 0.5f;
                const float ih = BtnH * 0.5f; // icon height
                const float BarW2 = std::max(1.5f, 2.0f * S); // icon bar thickness
                Graphics()->BlendNormal();

                if(b == 0)
                {
                        // previous: |◀
                        DrawRoundedRect(bx + BtnW * 0.22f, cy - ih * 0.5f, BarW2, ih, BarW2 * 0.5f, IGraphics::CORNER_ALL, IconCol);
                        const float tx0 = bx + BtnW * 0.22f + BarW2 + 1.0f * S;
                        const float tx1 = bx + BtnW * 0.78f;
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(IconCol);
                        IGraphics::CFreeformItem Tri(tx1, cy - ih * 0.5f, tx1, cy + ih * 0.5f, tx0, cy, tx0, cy);
                        Graphics()->QuadsDrawFreeform(&Tri, 1);
                        Graphics()->QuadsEnd();
                }
                else if(b == 1)
                {
                        if(Info.m_Playing)
                        {
                                // pause: two bars
                                const float pw = (BtnW * 0.42f - BarW2) * 0.5f;
                                DrawRoundedRect(bx + BtnW * 0.29f, cy - ih * 0.5f, BarW2, ih, BarW2 * 0.5f, IGraphics::CORNER_ALL, IconCol);
                                DrawRoundedRect(bx + BtnW * 0.29f + BarW2 + pw, cy - ih * 0.5f, BarW2, ih, BarW2 * 0.5f, IGraphics::CORNER_ALL, IconCol);
                        }
                        else
                        {
                                // play: ▶
                                const float tx0 = bx + BtnW * 0.30f;
                                const float tx1 = bx + BtnW * 0.70f;
                                Graphics()->QuadsBegin();
                                Graphics()->SetColor(IconCol);
                                IGraphics::CFreeformItem Tri(tx0, cy - ih * 0.5f, tx0, cy + ih * 0.5f, tx1, cy, tx1, cy);
                                Graphics()->QuadsDrawFreeform(&Tri, 1);
                                Graphics()->QuadsEnd();
                        }
                }
                else
                {
                        // next: ▶|
                        const float tx0 = bx + BtnW * 0.22f;
                        const float tx1 = bx + BtnW * 0.78f - BarW2 - 1.0f * S;
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(IconCol);
                        IGraphics::CFreeformItem Tri(tx0, cy - ih * 0.5f, tx0, cy + ih * 0.5f, tx1, cy, tx1, cy);
                        Graphics()->QuadsDrawFreeform(&Tri, 1);
                        Graphics()->QuadsEnd();
                        DrawRoundedRect(bx + BtnW * 0.78f - BarW2, cy - ih * 0.5f, BarW2, ih, BarW2 * 0.5f, IGraphics::CORNER_ALL, IconCol);
                }
        }
        m_ArrayPlayerBtnRectValid = true;

        // ---- level-driven visualizer bars floating above the progress line ----
        const int Bands = SKxMediaInfo::NUM_BANDS;
        const float BarAreaW = std::max(20.0f * S, LineX1 - TimeW - 10.0f * S - LineX0);
        const float BarGap = std::max(1.0f, 1.5f * S);
        const float BarW = std::max(1.0f, (BarAreaW - BarGap * (Bands - 1)) / (float)Bands);
        for(int i = 0; i < Bands; ++i)
        {
                float Level;
                if(Info.m_Valid)
                        Level = Info.m_aBands[i];
                else if(m_ArrayPlayerEditor)
                        Level = 0.22f + 0.55f * std::fabs(std::sin((float)i * 0.7f + m_ArrayPlayerHue * 0.02f));
                else
                        Level = 0.0f;
                const float Bh = Level * BarsH;
                if(Bh < 1.0f)
                        continue; // silence — no stub dashes along the line
                ColorRGBA BarCol;
                if(Rainbow)
                {
                        // Group = one hue, Peas/Peas+Gradient = gradient across bars.
                        float R, G, B;
                        const float t = RainbowMode == 0 ? 0.0f : (float)i / (float)(Bands - 1);
                        KxHueToRgb(m_ArrayPlayerHue - RainbowStep * t, R, G, B);
                        BarCol = ColorRGBA(R, G, B, alpha * MainOp);
                }
                else
                        BarCol = DDNetStyle ? ColorRGBA(0.75f, 0.75f, 0.75f, alpha * MainOp) : KxColorWithAlpha(m_AccentColorRGBA, alpha * MainOp);
                const float bx = LineX0 + (float)i * (BarW + BarGap);
                DrawRoundedRect(bx, BarsBase - Bh, BarW, Bh, 1.0f * S, IGraphics::CORNER_ALL, BarCol);
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
                if(!SearchKeepsRow(row))
                        continue;
                sig = sig * 31u + (unsigned)r + 1u;
                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                {
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                if(!SearchKeepsChild(row.pChildren[c]))
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
                        auto addRowNode = [&](SRow &row, bool isChild) {
                                if(cache.rowCount >= MAX_ROWS)
                                        return;
                                YGNodeRef n = YGNodeNew();
                                if(row.type == ERowType::Label)
                                {
                                        const float LabelW = PANEL_WIDTH - BODY_PADDING * 2.0f - (isChild ? SUB_LIST_INDENT : 0.0f) - 8.0f;
                                        row.m_LabelLines = KxLabelLineCount(row.pName, LabelW);
                                }
                                YGNodeStyleSetHeight(n, RowHeightForRow(row));
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
                                if(!SearchKeepsRow(row))
                                        continue;
                                addRowNode(row, false);
                                if(row.type == ERowType::Expandable && row.pChildren && row.m_AnimExpand > 0.0f)
                                {
                                        for(int c = 0; c < row.childCount; ++c)
                                        {
                                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                                if(!SearchKeepsChild(row.pChildren[c]))
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
                if(rowCount == 0)
                        bodyHeightFull = 0.0f; // v1.56.286: search-filtered empty panel — header only
                for(int i = 0; i < rowCount; ++i)
                        rowNodes[i] = cache.aRowNodes[i];
        }

        // Preload pass (CClickGui::OnInit): only the layout cache is needed,
        // skip all drawing.
        if(m_bPreloadLayout)
                return;

// v1.56.61: scale body height by the visibility (panel open/close).
	// v1.56.284: the collapse anim is exponentially smoothed — use it raw
	// (EaseBothDirections jumped when the target flipped mid-animation).
	const float easedVisibility = 1.0f - collapseAnim;

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
                        const float expandProgress = row.m_AnimExpand;
                        float childrenBlockH = 0.0f;
                        int visibleCount = 0;
                        for(int c = 0; c < row.childCount; ++c)
                        {
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                if(!SearchKeepsChild(row.pChildren[c]))
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
        float visibleBodyHeight = scrollable ? std::min(bodyHeight, MAX_BODY_HEIGHT) : bodyHeight;
        // v1.56.286: smooth the body height — the search reflow rebuilds the
        // layout instantly (rows appear/disappear at once) and the panel bottom
        // glides to the new height instead of snapping.
        {
                const float TargetBodyH = visibleBodyHeight;
                const float dtAnim = Client()->RenderFrameTime();
                const float SmoothK = 1.0f - std::exp(-14.0f * dtAnim);
                g_aSmoothBodyH[panelIdx] += (TargetBodyH - g_aSmoothBodyH[panelIdx]) * SmoothK;
                if(std::fabs(TargetBodyH - g_aSmoothBodyH[panelIdx]) < 0.5f)
                        g_aSmoothBodyH[panelIdx] = TargetBodyH;
                visibleBodyHeight = g_aSmoothBodyH[panelIdx];
        }

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
        {
                m_aPanelScrollStored[panelIdx] = scrollRef;
                // v1.56.284: keep the smooth-scroll target inside the clamp
                // range so inertia doesn't fight the content bounds.
                if(m_aPanelScrollTarget[panelIdx] < 0.0f)
                        m_aPanelScrollTarget[panelIdx] = 0.0f;
                if(m_aPanelScrollTarget[panelIdx] > maxScroll)
                        m_aPanelScrollTarget[panelIdx] = maxScroll;
        }
        const float scrollOffset = scrollable ? scrollRef : 0.0f;

        // ---- Step 2: panel geometry ----
        const float panelH = HEADER_HEIGHT + visibleBodyHeight;
        const float px = pos.x;
        const float py = pos.y;

        Graphics()->BlendNormal();

        DrawRoundedRect(px + 4.0f, py + 4.0f, PANEL_WIDTH, panelH,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.15f * m_PanelOpacity));

        // Panel border (1px-ish outline via slightly larger dark rect behind).
        DrawRoundedRect(px - 1.0f, py - 1.0f, PANEL_WIDTH + 2.0f, panelH + 2.0f,
                5.0f, IGraphics::CORNER_ALL,
                ColorRGBA(COL_PANEL_BORDER, true).WithAlpha(alpha * m_PanelOpacity));

        // Panel background (stored ARGB alpha multiplies the UI fade alpha).
        DrawRoundedRect(px, py, PANEL_WIDTH, panelH,
                5.0f, IGraphics::CORNER_ALL,
                KxColorWithAlpha(m_PanelBgRGBA, alpha * m_PanelOpacity));

        // ---- Step 3: header ----
        // Header is the top HEADER_HEIGHT px; rounded only on top corners so it
        // blends with the body below.
        DrawRoundedRect(px, py, PANEL_WIDTH, HEADER_HEIGHT,
                5.0f, IGraphics::CORNER_T,
                KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity));

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
                        ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(alpha * 0.5f * m_PanelOpacity));

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
                if(!SearchKeepsRow(row))
                                        continue;
                                const YGNodeRef node = rowNodes[rowIdx];
                const float rxLocal = YGNodeLayoutGetLeft(node);
                const float ryLocal = YGNodeLayoutGetTop(node);
                const float rw = YGNodeLayoutGetWidth(node);
                const float rh = YGNodeLayoutGetHeight(node);
                const float rx = bodyX + rxLocal;
                const float ry = bodyY + ryLocal + rowYOffset;
                ++rowIdx;

                // v1.56.289: record the hovered top-level row for the ToolTip
                // (previously only child rows were recorded, so headers and
                // top-level toggles never showed a tooltip). Children are
                // processed later in the same iteration and override this.
                {
                        const char *pTT = row.pToolTip;
                        if(row.type == ERowType::DoubleButton && row.pToolTip2 &&
                                m_MousePos.x >= rx + rw * 0.5f)
                                pTT = row.pToolTip2;
                        // TPS slider: hovering the Pause checkbox on the right shows
                        // the toggle's own tooltip instead of the slider's.
                        else if(row.type == ERowType::Slider && row.pToolTip2 &&
                                str_comp(row.pName, "TPS") == 0 &&
                                m_MousePos.x >= rx + rw - KX_TPS_TOGGLE_W)
                                pTT = row.pToolTip2;
                        if(pTT && PointInRect(m_MousePos, rx, ry, rw, rh))
                        {
                                str_copy(m_aToolTipText, pTT, sizeof(m_aToolTipText));
                                m_pToolTipRow = pTT;
                                m_ToolTipRowTime = Client()->GlobalTime();
                        }
                }

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

                        // Value chip — v1.56.284: always the value snapped to the
                        // step, with decimals derived from the step size.
                        const float ShownVal = SnapSliderStep(row);
                        const int StepDec = SliderStepDecimals(row);
                        char valBuf[32];
                        if(StepDec == 0)
                        {
                                if(row.pSuffix)
                                        str_format(valBuf, sizeof(valBuf), "%d%s", (int)ShownVal, row.pSuffix);
                                else
                                        str_format(valBuf, sizeof(valBuf), "%d", (int)ShownVal);
                        }
                        else
                        {
                                if(row.pSuffix)
                                        str_format(valBuf, sizeof(valBuf), "%.*f%s", StepDec, ShownVal, row.pSuffix);
                                else
                                        str_format(valBuf, sizeof(valBuf), "%.*f", StepDec, ShownVal);
                        }
                        const char *pValueText = row.pValueLabel ? row.pValueLabel : valBuf;
                        // TPS row: slider lives in the left half, Pause toggle on the right.
                        const bool IsTpsRow = str_comp(row.pName, "TPS") == 0;
                        const float valW = TextRender()->TextWidth(17.0f, pValueText) + 10.0f;
                        const float valX = IsTpsRow ? rx + rw - KX_TPS_TOGGLE_W - valW - 4.0f : rx + rw - valW - 2.0f;
                        const float valY = ry + 1.0f;
                        DrawRoundedRect(valX, valY, valW, 18.0f, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(alpha));
                        DrawText(valX + 6.0f, valY + 3.0f, 17.0f, pValueText, textCol);

                        // Track.
                        const float trackX = rx + 2.0f;
                        const float trackY = ry + rh - SLIDER_TRACK_H - 4.0f;
                        const float trackW = KxTpsTrackW(&row, rw);
                        DrawRoundedRect(trackX, trackY, trackW, SLIDER_TRACK_H, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_SLIDER_TRACK, true).WithAlpha(alpha));

                        // Thumb. On the TPS row the track is shortened by the
                        // Pause toggle, so the thumb travels within the track
                        // minus its own width and never crosses into the gap.
                        float pct = 0.0f;
                        if(row.max > row.min)
                                pct = (row.value - row.min) / (row.max - row.min);
                        pct = std::clamp(pct, 0.0f, 1.0f);
                        float thumbX;
                        if(IsTpsRow)
                        {
                                const float Span = trackW - SLIDER_THUMB_W;
                                thumbX = trackX + SLIDER_THUMB_W * 0.5f + Span * pct;
                        }
                        else
                                thumbX = trackX + trackW * pct;
                        const float thumbY = trackY + SLIDER_TRACK_H * 0.5f;
                        DrawRoundedRect(thumbX - SLIDER_THUMB_W * 0.5f, thumbY - SLIDER_THUMB_H * 0.5f,
                                SLIDER_THUMB_W, SLIDER_THUMB_H, 3.0f, IGraphics::CORNER_ALL,
                                KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity));

                        // TPS row: Pause toggle pinned to the right edge (same
                        // checkbox style as Toggle rows; row.on = paused).
                        if(IsTpsRow)
                        {
                                const float cbY = ry + (rh - CHECK_W) * 0.5f;
                                const float pLabelW = TextRender()->TextWidth(18.0f, "Pause");
                                const float cbX = rx + rw - 2.0f - pLabelW - 4.0f - CHECK_W;
                                DrawRoundedRect(cbX, cbY, CHECK_W, CHECK_W, 3.0f,
                                        IGraphics::CORNER_ALL,
                                        ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                if(row.on)
                                        DrawText(cbX + CHECK_W * 0.5f, cbY - 2.0f, 24.0f, "\xe2\x9c\x95" /* ✕ */,
                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                DrawText(cbX + CHECK_W + 4.0f, cbY + 3.0f, 18.0f, "Pause", textCol);
                        }
                        break;
                }
                case ERowType::Input:
                {
                        // [name (compact)] [text input box]
                        DrawText(rx + 2.0f, ry + 5.0f, 17.0f, row.pName, textCol);
                        // v6.3: fixed input width — child rows are narrower (sub-list
                        // indent), dividing their width by 2 made the field narrower
                        // than in top-level rows. Width now always matches the
                        // top-level row, right-aligned to the shared right edge.
                        const float inW = (PANEL_WIDTH - BODY_PADDING * 2) * 0.5f;
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
                                editingThis ? KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
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
                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f * m_MainOpacity));
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
                case ERowType::ToggleInput: // v1.56.282: [✕] [name] [input box]
                {
                        DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                        if(row.on)
                                DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x95",
                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        DrawText(rx + CHECK_W + 4.0f, ry + 5.0f, 17.0f, row.pName, textCol);

                        const float inW = rw / 2.0f;
                        const float inX = rx + rw - inW - 2.0f;
                        const float inY = ry + 2.0f;
                        const float inH = rh - 4.0f;
                        const bool editingThis = (m_InputEditing && m_InputPanel == panelIdx && m_InputRow == r && m_InputChild < 0);
                        DrawRoundedRect(inX, inY, inW, inH, 2.0f,
                                IGraphics::CORNER_ALL,
                                ColorRGBA(COL_INPUT_BG, true).WithAlpha(alpha));
                        DrawRoundedRect(inX - 1.0f, inY - 1.0f, inW + 2.0f, inH + 2.0f, 2.0f,
                                IGraphics::CORNER_ALL,
                                editingThis ? KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
                        if(!row.m_InputInitialized)
                        {
                                if(row.pInputValue)
                                        str_copy(row.m_aInputBuf, row.pInputValue, sizeof(row.m_aInputBuf));
                                row.m_InputInitialized = true;
                        }
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
                                inCol = (row.m_aInputBuf[0]) ? textCol : ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha);
                        }
                        if(pText)
                                DrawText(inX + 4.0f, inY + 4.0f, 17.0f, pText, inCol);
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
                        // [name] [select box] — name aligned with the checkbox
                        // column of toggle rows (rx + 1).
                        DrawText(rx + 1.0f, ry + 5.0f, 18.0f, row.pName, textCol);
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
                        // v6.2: visual expandables (Spawner, Visuals, ...) have no
                        // checkbox — the row only opens/closes.
                        const bool VisualRow = IsVisualExpandableName(row.pName);
                        if(!VisualRow)
                        {
                                DrawRoundedRect(rx + 1.0f, ry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                        IGraphics::CORNER_ALL,
                                        ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                if(row.on)
                                        DrawText(rx + 1.0f + CHECK_W * 0.5f, ry + 3.0f - 2.0f, 24.0f, "\xe2\x9c\x95",
                                                ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                        }
                        // Visual expandables have no checkbox — their text starts
                        // at the same column as the checkbox of toggle rows.
                        DrawText(rx + (VisualRow ? 1.0f : CHECK_W + 4.0f), ry + 5.0f, 18.0f, row.pName,
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
                        ColorRGBA btnCol = KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity); // default purple
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
                        // v1.56.280: RT Pathfinding - green (IDLE) / red (RUNNING), no Finish state.
                        if(str_comp(row.pName, "RT Pathfinding") == 0)
                        {
                                CBotNet &BN = GameClient()->m_BotNet;
                                if(BN.m_RtPfState == 0)
                                {
                                        btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha); // green
                                        pLabel = "Pathfinding";
                                }
                                else
                                {
                                        btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha); // red
                                        pLabel = "Stop";
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
                        // v6.2: Spawner action colors — Spawn/Loop green, Kill/Stop red.
                        else if(str_comp(row.pName, "Spawn") == 0)
                                btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                        else if(str_comp(row.pName, "Kill") == 0)
                                btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha);
                        else if(str_comp(row.pName, g_TasDyn_Loop) == 0)
                                btnCol = GameClient()->m_Tas.SpawnerLoopActive()
                                        ? ColorRGBA(0xffc43d3d, true).WithAlpha(alpha)
                                        : ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
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
                        // v6.3: ArrayList Editor / Reset. The right half (Reset) only
                        // appears once the list position has been moved.
                        if(str_comp(row.pName, "Editor") == 0)
                        {
                                const bool Modified = ArrayListPositionModified();
                                // Editor green, Exit Editor red; Reset is the Clear-yellow half.
                                const ColorRGBA leftCol = (m_ArrayListEditor ? ColorRGBA(0xffc43d3d, true) : ColorRGBA(0xff3da35a, true)).WithAlpha(alpha);
                                const char *pLeftLabel = m_ArrayListEditor ? "Exit Editor" : "Editor";
                                if(!Modified)
                                {
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(15.0f, pLeftLabel);
                                        DrawText(rx + (rw - lw) * 0.5f, ry + 8.0f, 15.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                                else
                                {
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(13.0f, pLeftLabel);
                                        DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 13.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        const float rx2 = rx + 2.0f + btnW + 2.0f;
                                        DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, ColorRGBA(0xffe69528, true).WithAlpha(alpha));
                                        const float rw2 = TextRender()->TextWidth(13.0f, "Reset");
                                        DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 13.0f, "Reset",
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                        }
                        // v1.56.150: Pathfinder DoubleButton — Pathfinding (left) + Paste (right).
                        // Paste visible only when m_PfState == FINISHED (path ready).
                        // When Paste hidden: Pathfinding takes full row width.
                        else if(str_comp(row.pName, "Pathfinding") == 0)
                        {
                                CBotNet &BN = GameClient()->m_BotNet;
                                // Left button colors (Pathfinding state).
                                ColorRGBA leftCol = KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity);
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
                        else if(str_comp(row.pName, "RT Editor Forbidden Tiles") == 0 || str_comp(row.pName, "RT Editor Finish Tiles") == 0)
                        {
                                CBotNet &BN = GameClient()->m_BotNet;
                                const bool isForbidden = (str_comp(row.pName, "RT Editor Forbidden Tiles") == 0);
                                const bool editorActive = isForbidden ? (BN.m_RtPfEditorMode == 1) : (BN.m_RtPfEditorMode == 2);
                                const bool clearVisible = isForbidden ? !BN.m_RtPfForbiddenTiles.empty() : !BN.m_RtPfCustomFinishTiles.empty();
                                ColorRGBA leftCol = editorActive
                                        ? ColorRGBA(0xffc43d3d, true).WithAlpha(alpha)
                                        : ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                const char *pLeftLabel = editorActive ? "Exit Editor" : row.pName;
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
                                                IGraphics::CORNER_ALL, ColorRGBA(0xffc43d3d, true).WithAlpha(alpha));
                                        const char *pClearLabel = "Clear";
                                        const float cw = TextRender()->TextWidth(11.0f, pClearLabel);
                                        DrawText(rx2 + (btnW - cw) * 0.5f, ry + 8.0f, 11.0f, pClearLabel,
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
                                const ColorRGBA leftCol = KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity);
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
                        // v6.2: Join/Leave Playground + Reset Playground (the reset
                        // half appears only when there is something to drop).
                        else if(str_comp(row.pName, g_TasDyn_Join) == 0)
                        {
                                CTas &Tas = GameClient()->m_Tas;
                                const bool Joined = Tas.IsJoined();
                                const ColorRGBA leftCol = Joined
                                        ? ColorRGBA(0xffc43d3d, true).WithAlpha(alpha)
                                        : ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                const char *pLeftLabel = Joined ? "Leave Playground" : "Join Playground";
                                // v6.2: the Reset half shows whenever the playground
                                // has any content — even outside the playground.
                                if(!Tas.PlaygroundDirty())
                                {
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(15.0f, pLeftLabel);
                                        DrawText(rx + (rw - lw) * 0.5f, ry + 8.0f, 15.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                                else
                                {
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(12.0f, pLeftLabel);
                                        DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 12.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        const float rx2 = rx + 2.0f + btnW + 2.0f;
                                        DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, ColorRGBA(0xffc43d3d, true).WithAlpha(alpha));
                                        const float rw2 = TextRender()->TextWidth(12.0f, "Reset Playground");
                                        DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 12.0f, "Reset Playground",
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                        }
                        // v6.2: Rewind (left) + Forward (right) in one row.
                        else if(str_comp(row.pName, "Rewind") == 0)
                        {
                                const float btnW = (rw - 6.0f) * 0.5f;
                                DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                        IGraphics::CORNER_ALL, KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity));
                                const float lw = TextRender()->TextWidth(15.0f, "Rewind");
                                DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 15.0f, "Rewind",
                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                const float rx2 = rx + 2.0f + btnW + 2.0f;
                                DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                        IGraphics::CORNER_ALL, KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity));
                                const float rw2 = TextRender()->TextWidth(15.0f, "Forward");
                                DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 15.0f, "Forward",
                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                        }
                        // v6.3: Play/Record + Reset Record (reset half only when the
                        // take buffer holds ticks or a .tas file is loaded).
                        else if(str_comp(row.pName, g_TasDyn_Action) == 0)
                        {
                                CTas &Tas = GameClient()->m_Tas;
                                const bool Active = Tas.IsPlaying() || Tas.IsRealPlaying() || Tas.IsRecording();
                                const ColorRGBA leftCol = Active
                                        ? ColorRGBA(0xffc43d3d, true).WithAlpha(alpha)
                                        : ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                const char *pLeftLabel = row.pName;
                                if(!Tas.RecordDirty())
                                {
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, rw - 4.0f, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(16.0f, pLeftLabel);
                                        DrawText(rx + (rw - lw) * 0.5f, ry + 8.0f, 16.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                                else
                                {
                                        const float btnW = (rw - 6.0f) * 0.5f;
                                        DrawRoundedRect(rx + 2.0f, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, leftCol);
                                        const float lw = TextRender()->TextWidth(14.0f, pLeftLabel);
                                        DrawText(rx + 2.0f + (btnW - lw) * 0.5f, ry + 8.0f, 14.0f, pLeftLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        const float rx2 = rx + 2.0f + btnW + 2.0f;
                                        DrawRoundedRect(rx2, ry + 2.0f, btnW, rh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, ColorRGBA(0xffc43d3d, true).WithAlpha(alpha));
                                        const float rw2 = TextRender()->TextWidth(13.0f, "Reset Record");
                                        DrawText(rx2 + (btnW - rw2) * 0.5f, ry + 8.0f, 13.0f, "Reset Record",
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                }
                        }
                        break;
                }
                case ERowType::Label:
                {
                        // v1.56.176: Label — non-interactive text row (Info panel).
                        // v6.2: wrapped to the row width; the row height comes from
                        // the wrapped line count computed in the layout pass.
                        std::vector<std::string> vLines;
                        KxWrapText(row.pName, rw - 8.0f, vLines);
                        float LabelY = ry + 5.0f;
                        for(const std::string &Line : vLines)
                        {
                                DrawText(rx + 4.0f, LabelY, 16.0f, Line.c_str(),
                                        ColorRGBA(0xffb8b8c8, true).WithAlpha(alpha));
                                LabelY += 20.0f;
                        }
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
                                        if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                        if(!SearchKeepsChild(row.pChildren[c]))
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
                                if(!TasRowChildrenVisible(row, c))
                                        continue;
                                if(str_comp(row.pName, "Fake Aim") == 0 && !FakeAimChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Auto Triple Fly") == 0 && !TripleFlyChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if(str_comp(row.pName, "AimBot") == 0 && !AimBotChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "TriggerBot") == 0 && !TriggerBotChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Dummies") == 0 && !DummiesChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c))) continue;
                                if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c))) continue;
                                if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c)) continue;
                                if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
                                        continue;
                                if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c))) continue;
                                if(!SearchKeepsChild(row.pChildren[c]))
                                        continue;
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
                                        ColorRGBA(COL_SUB_LIST_BG, true).WithAlpha(alpha * 0.6f * m_PanelOpacity));
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
                                if(!TasRowChildrenVisible(row, c))
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
if(((str_comp(row.pName, "ESP") == 0 || str_comp(row.pName, "Hitboxes") == 0) && !EspChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Helpers") == 0 && !HelpersChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Spoofer") == 0 && !SpooferChildVisible(row.pChildren, row.childCount, c) || (str_comp(row.pName, "Spoof Spectate") == 0 && !SpoofSpectateChildVisible(row.pChildren, row.childCount, c)))
        continue;
if(str_comp(row.pName, "Avoid Freeze") == 0 && !AfChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Trajectory") == 0 && !TrajectoryChildVisible(row.pChildren, row.childCount, c))
        continue;
if(str_comp(row.pName, "Line rendering") == 0 && !LineRenderingChildVisible(row.pChildren, row.childCount, c) || IsRainbowVisualRow(row.pName) && !VisualRainbowChildVisible(row.pChildren, row.childCount, c))
        continue;
if((str_comp(row.pName, "Smart dummy switch") == 0 && !BindsChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Smart Copy Moves") == 0 && !SmartCopyChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "Admin Alarm") == 0 && !AdminAlarmChildVisible(row.pChildren, row.childCount, c)) || (str_comp(row.pName, "BalanceBot") == 0 && !BalanceBotChildVisible(row.pChildren, row.childCount, c)))
        continue;
                                if(!SearchKeepsChild(child))
                                        continue;
                                const YGNodeRef cnode = rowNodes[rowIdx];
                                const float crx = bodyX + YGNodeLayoutGetLeft(cnode);
                                const float cry = bodyY + YGNodeLayoutGetTop(cnode) + rowYOffset;
                                const float crw = YGNodeLayoutGetWidth(cnode);
                                const float crh = YGNodeLayoutGetHeight(cnode);
                                ++rowIdx;

                                // v1.56.284: record hovered child row for the ToolTip.
                                // v1.56.288: DoubleButton — left half uses pToolTip,
                                // right half uses pToolTip2 (falls back to pToolTip).
                                // Keying on the chosen pointer means switching halves
                                // restarts the hover delay, so each button has its own.
                                const char *pTT = child.pToolTip;
                                if(child.type == ERowType::DoubleButton && child.pToolTip2 &&
                                        m_MousePos.x >= crx + crw * 0.5f)
                                        pTT = child.pToolTip2;
                                if(pTT && PointInRect(m_MousePos, crx, cry, crw, crh))
                                {
                                        str_copy(m_aToolTipText, pTT, sizeof(m_aToolTipText));
                                        m_pToolTipRow = pTT;
                                        m_ToolTipRowTime = Client()->GlobalTime();
                                }

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
                                        // v1.56.284: stepped value + step-derived decimals.
                                        const float cShown = SnapSliderStep(child);
                                        const int cDec = SliderStepDecimals(child);
                                        char vbuf[32];
                                        if(cDec == 0)
                                        {
                                                if(child.pSuffix)
                                                        str_format(vbuf, sizeof(vbuf), "%d%s", (int)cShown, child.pSuffix);
                                                else
                                                        str_format(vbuf, sizeof(vbuf), "%d", (int)cShown);
                                        }
                                        else
                                        {
                                                if(child.pSuffix)
                                                        str_format(vbuf, sizeof(vbuf), "%.*f%s", cDec, cShown, child.pSuffix);
                                                else
                                                        str_format(vbuf, sizeof(vbuf), "%.*f", cDec, cShown);
                                        }
                                        const char *pVText = child.pValueLabel ? child.pValueLabel : vbuf;
                                        const float vw = TextRender()->TextWidth(17.0f, pVText) + 10.0f;
                                        const float vx = crx + crw - vw - 2.0f;
                                        DrawRoundedRect(vx, cry + 1.0f, vw, 18.0f, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_VALUE_BG, true).WithAlpha(alpha));
                                        DrawText(vx + 6.0f, cry + 2.0f, 17.0f, pVText, cText);
                                        const float trx = crx + 2.0f;
                                        const float trY = cry + crh - SLIDER_TRACK_H - 4.0f;
                                        const float trW = crw - 4.0f;
                                        DrawRoundedRect(trx, trY, trW, SLIDER_TRACK_H, 2.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_SLIDER_TRACK, true).WithAlpha(alpha));
                                        // v1.56.284: thumb at the snapped (step) position.
                                        float cpct = 0.0f;
                                        if(child.max > child.min)
                                                cpct = (cShown - child.min) / (child.max - child.min);
                                        cpct = std::clamp(cpct, 0.0f, 1.0f);
                                        const float thX = trx + trW * cpct;
                                        const float thY = trY + SLIDER_TRACK_H * 0.5f;
                                        DrawRoundedRect(thX - SLIDER_THUMB_W * 0.5f, thY - SLIDER_THUMB_H * 0.5f,
                                                SLIDER_THUMB_W, SLIDER_THUMB_H, 3.0f, IGraphics::CORNER_ALL,
                                                KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity));
                                        break;
                                }
                                case ERowType::Input:
                                {
                                        DrawText(crx + 2.0f, cry + 5.0f, 17.0f, child.pName, cText);
                                        // Same fixed width / right alignment as top-level
                                        // input rows, so child fields aren't narrower.
                                        const float iw = (PANEL_WIDTH - BODY_PADDING * 2) * 0.5f;
                                        const float ix = crx + crw - iw - 2.0f;
                                        const float iy = cry + 2.0f;
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
                                                editingChild ? KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
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
                                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f * m_MainOpacity));
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
                                case ERowType::ToggleInput: // v1.56.282: [✕] [name] [input box]
                                {
                                        DrawRoundedRect(crx + 1.0f, cry + 3.0f, CHECK_W, CHECK_W, 3.0f,
                                                IGraphics::CORNER_ALL,
                                                ColorRGBA(COL_CHECK_SQUARE, true).WithAlpha(alpha));
                                        if(child.on)
                                                DrawText(crx + 1.0f + CHECK_W * 0.5f, cry + 3.0f - 2.0f, 22.0f, "\xe2\x9c\x95",
                                                        ColorRGBA(COL_CHECK_ON, true).WithAlpha(alpha), TEXTALIGN_CENTER);
                                        DrawText(crx + CHECK_W + 4.0f, cry + 5.0f, 17.0f, child.pName, cText);

                                        const float iw = crw / 2.0f;
                                        const float ix = crx + crw - iw - 2.0f;
                                        const float iy = cry + 2.0f;
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
                                                editingChild ? KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity) : ColorRGBA(COL_INPUT_BORDER, true).WithAlpha(alpha * 0.5f));
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
                                                ic = (child.m_aInputBuf[0]) ? cText : ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha);
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
                                                        ColorRGBA(m_AccentColorRGBA, true).WithAlpha(alpha * 0.5f * m_MainOpacity));
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
                                        ColorRGBA btnCol = KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity);
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
                                        // v6.2: Spawner action colors — Spawn/Loop green, Kill/Stop red.
                                        if(str_comp(child.pName, "Spawn") == 0)
                                                btnCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                        else if(str_comp(child.pName, "Kill") == 0)
                                                btnCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha);
                                        else if(str_comp(child.pName, g_TasDyn_Loop) == 0)
                                                btnCol = GameClient()->m_Tas.SpawnerLoopActive()
                                                        ? ColorRGBA(0xffc43d3d, true).WithAlpha(alpha)
                                                        : ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                        DrawRoundedRect(crx + 2.0f, cry + 2.0f, crw - 4.0f, crh - 4.0f, 4.0f,
                                                IGraphics::CORNER_ALL, btnCol);
                                        const float labelW = TextRender()->TextWidth(15.0f, pLabel);
                                        DrawText(crx + (crw - labelW) * 0.5f, cry + 7.0f, 15.0f, pLabel,
                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                        break;
                                }
                                case ERowType::Label:
                                {
                                        // v1.56.258: Label child — non-interactive text.
                                        // v6.2: wrapped like every other label.
                                        std::vector<std::string> vLines;
                                        KxWrapText(child.pName, crw - 8.0f, vLines);
                                        float LabelY = cry + 5.0f;
                                        for(const std::string &Line : vLines)
                                        {
                                                DrawText(crx + 4.0f, LabelY, 15.0f, Line.c_str(),
                                                        ColorRGBA(0xffb8b8c8, true).WithAlpha(alpha));
                                                LabelY += 20.0f;
                                        }
                                        break;
                                }
                                case ERowType::DoubleButton:
                                {
                                        // v6.3: ArrayList Editor / Reset (child of the ArrayList
                                        // expandable). The right half appears only after the HUD
                                        // position was moved.
                                        if(child.pName && str_comp(child.pName, "Editor") == 0)
                                        {
                                                const int Kind = (str_comp(row.pName, "ArrayInfo") == 0) ? 1 : (str_comp(row.pName, "ArrayPlayer") == 0) ? 2 : 0;
                                                const bool EditorActive = Kind == 1 ? m_ArrayInfoEditor : Kind == 2 ? m_ArrayPlayerEditor : m_ArrayListEditor;
                                                const bool Modified = Kind == 1 ? ArrayInfoPositionModified() : Kind == 2 ? ArrayPlayerPositionModified() : ArrayListPositionModified();
                                                // Editor green, Exit Editor red; Reset the Clear-yellow half.
                                                const ColorRGBA leftCol = (EditorActive ? ColorRGBA(0xffc43d3d, true) : ColorRGBA(0xff3da35a, true)).WithAlpha(alpha);
                                                const char *pLeftLabel = EditorActive ? "Exit Editor" : "Editor";
                                                if(!Modified)
                                                {
                                                        DrawRoundedRect(crx + 2.0f, cry + 2.0f, crw - 4.0f, crh - 4.0f, 4.0f,
                                                                IGraphics::CORNER_ALL, leftCol);
                                                        const float lw = TextRender()->TextWidth(15.0f, pLeftLabel);
                                                        DrawText(crx + (crw - lw) * 0.5f, cry + 8.0f, 15.0f, pLeftLabel,
                                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                }
                                                else
                                                {
                                                        const float btnW = (crw - 6.0f) * 0.5f;
                                                        DrawRoundedRect(crx + 2.0f, cry + 2.0f, btnW, crh - 4.0f, 4.0f,
                                                                IGraphics::CORNER_ALL, leftCol);
                                                        const float lw = TextRender()->TextWidth(13.0f, pLeftLabel);
                                                        DrawText(crx + 2.0f + (btnW - lw) * 0.5f, cry + 8.0f, 13.0f, pLeftLabel,
                                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                        const float rx2 = crx + 2.0f + btnW + 2.0f;
                                                        DrawRoundedRect(rx2, cry + 2.0f, btnW, crh - 4.0f, 4.0f,
                                                                IGraphics::CORNER_ALL, ColorRGBA(0xffe69528, true).WithAlpha(alpha));
                                                        const float rw2 = TextRender()->TextWidth(13.0f, "Reset");
                                                        DrawText(rx2 + (btnW - rw2) * 0.5f, cry + 8.0f, 13.0f, "Reset",
                                                                ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                }
                                                break;
                                        }
                                        // v6.3: generic two-half double button (Kill | Kill All) —
                                        // both halves red, labels from pName / pToolTip2.
                                        if(child.pName && str_comp(child.pName, "Kill") == 0 && child.pToolTip2)
                                        {
                                                const float btnW = (crw - 6.0f) * 0.5f;
                                                const ColorRGBA Red = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha);
                                                DrawRoundedRect(crx + 2.0f, cry + 2.0f, btnW, crh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, Red);
                                                const float lw1 = TextRender()->TextWidth(13.0f, "Kill");
                                                DrawText(crx + 2.0f + (btnW - lw1) * 0.5f, cry + 8.0f, 13.0f, "Kill",
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                const float rx2 = crx + 2.0f + btnW + 2.0f;
                                                DrawRoundedRect(rx2, cry + 2.0f, btnW, crh - 4.0f, 4.0f,
                                                        IGraphics::CORNER_ALL, Red);
                                                const float lw2 = TextRender()->TextWidth(13.0f, "Kill All");
                                                DrawText(rx2 + (btnW - lw2) * 0.5f, cry + 8.0f, 13.0f, "Kill All",
                                                        ColorRGBA(0xffffffff, true).WithAlpha(alpha));
                                                break;
                                        }
                                        // v1.56.90: DoubleButton child — Connect + Switch + Send toggle (Dummies).
                                        // If dummy NOT connected: only Connect button (full row width).
                                        // If dummy connected: Connect (left) + Switch (middle) + Send checkbox (right).
                                        // v1.56.197: left button label is dynamic — "D# Connect" (idle),
                                        // "D# Connecting..." (connecting), "D# Disconnect" (connected).
                                        ColorRGBA leftCol = KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity);
                                        if(child.on) // child.on = connected state
                                                leftCol = ColorRGBA(0xff3da35a, true).WithAlpha(alpha);
                                        // v1.56.197: build dynamic left-button label based on connection state.
                                        // pName is const (static "D# Connect"), so render from a local buffer
                                        // when the state requires a different word.
                                        const char *pLeftLabel = child.pName;
                                        char aLeftLabel[32];
                                        if(child.pName && DummyMgrParseD(child.pName) >= 1)
                                        {
                                                const int d = DummyMgrParseD(child.pName);
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
                                                ColorRGBA rightCol = KxColorWithAlpha(m_AccentColorRGBA, alpha * m_MainOpacity);
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
// v1.56.284: cut the body at the screen bottom edge.
                ClipEnableVirtual(bodyX, bodyY, bodyW, std::min(visibleBodyHeight, std::max(0.0f, VScreenHeight() - bodyY)));

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
