// (c) Kinetix. Array* HUD translation unit — ArrayList / ArrayInfo /
// ArrayPlayer (+ the shared helpers and feature table). Moved out of
// clickgui.cpp; the CClickGui method declarations stay in clickgui.h.

#include <game/client/components/kinetix/clickgui.h>
#include <game/client/components/kinetix/array_shared.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/media_info.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <base/color.h>
#include <base/fs.h>
#include <base/io.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/animstate.h>
#include <game/client/components/console.h>
#include <game/client/components/skins.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>

#if defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
// windows.h maps DrawText to DrawTextW (UNICODE) — that breaks every
// CClickGui::DrawText call in this file.
#undef DrawText
#endif

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
        {"Jet Ride", &g_Config.m_KxJetRide},
        {"Hook Ride", &g_Config.m_KxHookRide},
        {"Moonwalk", &g_Config.m_KxMoonwalk},
        {"Quick Stop", &g_Config.m_KxQuickStop},
        {"Hide Chat Bubble", &g_Config.m_KxHideChatBubble},
        {"Fast Input", &g_Config.m_KxFastInput},
        {"Auto Team", &g_Config.m_KxAutoTeam},
        {"Anti Afk", &g_Config.m_KxAntiAfk},
        {"Auto Vote", &g_Config.m_KxAutoVote},
        {"Auto Vote Kick", &g_Config.m_KxAutoVoteKick},
        {"Copy Moves Latency", &g_Config.m_KxCopyMovesLatencyEnabled},
        {"Laser Unfreeze", &g_Config.m_KxLaserUnfreeze},
        {"Fake Aim", &g_Config.m_KxFakeAim},
        {"Avoid Freeze", &g_Config.m_KxAvoidFreeze},
        {"AledBot", &g_Config.m_KxAledEnabled},
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
        {"Auto Verify", &g_Config.m_KxAutoVerify},
        {"Discord RPC", &g_Config.m_KxDiscordRpc},
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
        {
                const bool GuiEdit = m_ArrayListEditorGuiPass && g_Config.m_KxArrayListShowMode == 2;
                (GuiEdit ? g_Config.m_KxArrayListPosXGui : g_Config.m_KxArrayListPosX) = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
                (GuiEdit ? g_Config.m_KxArrayListPosYGui : g_Config.m_KxArrayListPosY) = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
        }
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
        const bool GuiPos = g_Config.m_ClClickGui != 0 && g_Config.m_KxArrayListShowMode == 2;
        const float YRatio = (GuiPos ? g_Config.m_KxArrayListPosYGui : g_Config.m_KxArrayListPosY) / 10000.0f;
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
        // v6.14: colors are AARRGGBB — the alpha byte replaces the opacity sliders.
        unsigned TextArgb = 0xffffffff;
        sscanf(g_Config.m_KxArrayListTextColor, "%8x", &TextArgb);
        unsigned MainArgb = 0xff9d4edd;
        sscanf(g_Config.m_KxArrayListMainColor, "%8x", &MainArgb);
        unsigned PanelArgb = 0xff161622;
        sscanf(g_Config.m_KxArrayListPanelColor, "%8x", &PanelArgb);
        const float TextOp = UiOn ? ((TextArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float MainOp = UiOn ? ((MainArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float PanelOp = UiOn ? ((PanelArgb >> 24) & 0xff) / 255.0f : 0.0f;

        // Full ARGB: ColorRGBA(v, true) unpacks the alpha from bits 24-31, so
        // masking it off here would leave every color with alpha = 0.
        unsigned TextHex = TextArgb;

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

        const float XRatio = (GuiPos ? g_Config.m_KxArrayListPosXGui : g_Config.m_KxArrayListPosX) / 10000.0f;
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
                : KxColorWithAlpha(MainArgb, alpha);
        const ColorRGBA BgCol = DDNetStyle
                ? ColorRGBA(0.2f, 0.2f, 0.2f, 0.8f * alpha * PanelOp)
                : KxColorWithAlpha(PanelArgb, alpha * 0.97f);

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
        {
                const bool GuiEdit = m_ArrayInfoEditorGuiPass && g_Config.m_KxArrayInfoShowMode == 2;
                (GuiEdit ? g_Config.m_KxArrayInfoPosXGui : g_Config.m_KxArrayInfoPosX) = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
                (GuiEdit ? g_Config.m_KxArrayInfoPosYGui : g_Config.m_KxArrayInfoPosY) = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
        }
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
        // v6.14: colors are AARRGGBB — the alpha byte replaces the opacity sliders.
        unsigned TextArgb = 0xffffffff;
        sscanf(g_Config.m_KxArrayInfoTextColor, "%8x", &TextArgb);
        unsigned MainArgb = 0xff9d4edd;
        sscanf(g_Config.m_KxArrayInfoMainColor, "%8x", &MainArgb);
        unsigned PanelArgb = 0xff161622;
        sscanf(g_Config.m_KxArrayInfoPanelColor, "%8x", &PanelArgb);
        const float TextOp = UiOn ? ((TextArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float MainOp = UiOn ? ((MainArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float PanelOp = UiOn ? ((PanelArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float MaxLen = (UiOn ? (float)g_Config.m_KxArrayInfoMaxLength : 0.0f);
        const bool Editor = m_ArrayInfoEditor;

        // Full ARGB: ColorRGBA(v, true) unpacks the alpha from bits 24-31, so
        // masking it off here would leave every color with alpha = 0.
        unsigned TextHex = TextArgb;

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

        const bool GuiPos = g_Config.m_ClClickGui != 0 && g_Config.m_KxArrayInfoShowMode == 2;
        const float XRatio = (GuiPos ? g_Config.m_KxArrayInfoPosXGui : g_Config.m_KxArrayInfoPosX) / 10000.0f;
        int XA = g_Config.m_KxArrayInfoXAnchor;
        if(XA == 0)
                XA = (XRatio < 1.0f / 3.0f) ? 3 : ((XRatio > 2.0f / 3.0f) ? 1 : 2);
        const float AnchorX = XRatio * ScreenW;
        const float AnchorY = ((GuiPos ? g_Config.m_KxArrayInfoPosYGui : g_Config.m_KxArrayInfoPosY) / 10000.0f) * ScreenH;
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
                : KxColorWithAlpha(MainArgb, alpha);
        const ColorRGBA BgCol = DDNetStyle
                ? ColorRGBA(0.2f, 0.2f, 0.2f, 0.8f * alpha * PanelOp)
                : KxColorWithAlpha(PanelArgb, alpha * 0.97f);

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
        {
                const bool GuiEdit = m_ArrayPlayerEditorGuiPass && g_Config.m_KxArrayPlayerShowMode == 2;
                (GuiEdit ? g_Config.m_KxArrayPlayerPosXGui : g_Config.m_KxArrayPlayerPosX) = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
                (GuiEdit ? g_Config.m_KxArrayPlayerPosYGui : g_Config.m_KxArrayPlayerPosY) = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
        }
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
        // v6.14: colors are AARRGGBB — the alpha byte replaces the opacity sliders.
        unsigned TextArgb = 0xffffffff;
        sscanf(g_Config.m_KxArrayPlayerTextColor, "%8x", &TextArgb);
        unsigned MainArgb = 0xff9d4edd;
        sscanf(g_Config.m_KxArrayPlayerMainColor, "%8x", &MainArgb);
        unsigned PanelArgb = 0xff161622;
        sscanf(g_Config.m_KxArrayPlayerPanelColor, "%8x", &PanelArgb);
        const float TextOp = UiOn ? ((TextArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float MainOp = UiOn ? ((MainArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const float PanelOp = UiOn ? ((PanelArgb >> 24) & 0xff) / 255.0f : 0.0f;
        const bool DDNetStyle = UiOn && g_Config.m_KxArrayPlayerStyle == 1;
        const bool Rainbow = UiOn && g_Config.m_KxArrayPlayerRainbow != 0;
        const int RainbowMode = g_Config.m_KxArrayPlayerRainbowMode;
        const float RainbowStep = (float)g_Config.m_KxArrayPlayerRainbowStep;

        const float ScreenW = VScreenWidth();
        const float ScreenH = VScreenHeight();
        if(ScreenW <= 0.0f || ScreenH <= 0.0f)
                return;

        const bool GuiPos = g_Config.m_ClClickGui != 0 && g_Config.m_KxArrayPlayerShowMode == 2;
        float Left = ((GuiPos ? g_Config.m_KxArrayPlayerPosXGui : g_Config.m_KxArrayPlayerPosX) / 10000.0f) * ScreenW;
        float Top = ((GuiPos ? g_Config.m_KxArrayPlayerPosYGui : g_Config.m_KxArrayPlayerPosY) / 10000.0f) * ScreenH;
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

        unsigned TextHex = 0xffffffff;
        sscanf(g_Config.m_KxArrayPlayerTextColor, "%8x", &TextHex);

        const ColorRGBA BorderCol = DDNetStyle
                ? ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f * alpha * MainOp)
                : KxColorWithAlpha(MainArgb, alpha);
        const ColorRGBA BgCol = DDNetStyle
                ? ColorRGBA(0.2f, 0.2f, 0.2f, 0.8f * alpha * PanelOp)
                : KxColorWithAlpha(PanelArgb, alpha * 0.97f);

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
// v6.13: ArrayAgent — CAgent component (chat + HTTP SSE + tools).
// ============================================================================
#include <engine/shared/json.h>

#include <base/time.h>

#include <game/client/components/kinetix/code_exec/code_exec.h>

static const char *const KX_AGENT_SYSTEM_PROMPT =
	"You are Kinetix Agent, an interactive assistant built into the Kinetix client (a DDNet/Teeworlds client). "
	"You help the user with tasks in-game by running Lua through your tools — see the tool descriptions for details.\n"
	"\n"
	"# Tone and style\n"
	"Be concise, direct and to the point. No filler before or after the answer (no \"Sure!\", no summaries of what you will do). "
	"Use the tools to complete tasks; all text you output is shown to the user as-is. "
	"If you cannot help with something, say it briefly and offer an alternative. "
	"Reply in the user's language.\n"
	"\n"
	"Tools: code_execution (Lua in the client, optional background jobs), shell (a shell available on this system), read, write, edit, glob, grep, console (game console commands), documentation (Lua bindings).";

static const char *const KX_AGENT_TOOLS_JSON =
	"\"tools\":["
	"{\"type\":\"function\",\"function\":{\"name\":\"code_execution\",\"description\":\"Execute Lua code inside the Kinetix client (Code Execution). print(...) output and errors are returned to you as the tool result. Save files with the code itself (io/os) if needed — the tool only executes; file/net/os access is gated by the user's Execution Settings and may be denied. Keep scripts short, never loop forever, one complete script per call.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"Full Lua source code to execute\"},\"background\":{\"type\":\"boolean\",\"description\":\"Start the script in the background and return a job id immediately; pass that id back in job to read its output, or set kill to stop it. The script keeps running while you continue.\"}},\"required\":[\"code\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"documentation\",\"description\":\"Get the Lua binding documentation for ONE section: a class name (CConfig, CBotNet, CNetObj_PlayerInput, CCharacterCore, CCollision, CGameWorld, CChat, CControls, CCodeExec, CGameInfo, CCamera, CBinds) or net / io / os. Call it BEFORE writing code whenever you are not sure what is available. Do not guess APIs. The query may be free text containing the section name.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"description\":\"Section to look up, e.g. CConfig or net\"}},\"required\":[\"section\"]}}}"
	"]";

// v6.14: tools appended to the base list (read / write / edit / glob / grep /
// console). Raw string literals keep the embedded JSON quotes readable.
// The shell entry is built at request time: it lists ONLY the shells that are
// actually present in PATH (an unavailable shell is never mentioned).
static const char *const KX_AGENT_TOOLS_EXTRA_JSON =
	R"JSON(,{"type":"function","function":{"name":"read","description":"Read a text file and return its lines with line numbers. Use offset/limit for big files. Relative paths resolve against the agent workdir (the Kinetix folder by default).","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"offset":{"type":"integer","description":"First line, 0-based (default 0)"},"limit":{"type":"integer","description":"Max lines (default 2000)"}},"required":["path"]}}})JSON"
	R"JSON(,{"type":"function","function":{"name":"write","description":"Write a text file (creates parent folders, overwrites existing content).","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"content":{"type":"string","description":"Full file content"}},"required":["path","content"]}}})JSON"
	R"JSON(,{"type":"function","function":{"name":"edit","description":"Replace an exact substring in a file. oldString must match exactly once - include enough surrounding context to make it unique. Use this instead of rewriting whole files.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"oldString":{"type":"string","description":"Exact text to replace"},"newString":{"type":"string","description":"Replacement text"}},"required":["path","oldString","newString"]}}})JSON"
	R"JSON(,{"type":"function","function":{"name":"glob","description":"Find files by wildcard pattern, e.g. **/*.cpp or src/**/*.h. Recursive, relative to the agent workdir.","parameters":{"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern"}},"required":["pattern"]}}})JSON"
	R"JSON(,{"type":"function","function":{"name":"grep","description":"Search file contents recursively for a text pattern (case-insensitive). Returns path:line: text matches, capped.","parameters":{"type":"object","properties":{"pattern":{"type":"string","description":"Text to search for"},"path":{"type":"string","description":"Folder to search (optional, defaults to the workdir)"},"include":{"type":"string","description":"Optional file filter glob, e.g. *.cpp"}},"required":["pattern"]}}})JSON"
	R"JSON(,{"type":"function","function":{"name":"console","description":"Execute a game console command (echo test, kx_tas_pause, say hello, ...) and return the console lines printed by it. Chat replies from the server are not captured.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"Console command line"}},"required":["command"]}}})JSON";

// Documentation registry — sections the agent can look up. Member lists are
// taken from the LuaBridge3 bindings (code_exec_bindings.cpp).
struct SAgentDocSection
{
	const char *pSection;
	const char *pDoc;
};
static const SAgentDocSection g_aAgentDocSections[] = {
	{"sections",
		"Available sections: CConfig, CGameClient, CBotNet, CGameWorld, CCharacterCore, CNetObj_PlayerInput, "
		"CCollision, CChat, CControls, CCodeExec, CGameInfo, CCamera, CBinds, net, io, os, globals. "
		"Call documentation again with one of these names in the query."},
	{"CConfig",
		"Global `Config` (CConfig) exposes every kx_* config variable as a property. "
		"CConfig.get(\"name\") / CConfig.set(\"name\", value) also work for dynamic access. "
		"Examples: Config.KxAimBot, Config.KxArrayListEnabled, Config.ClPlayerName. "
		"All variable names are listed in config_variables.h; kx_* = Kinetix, cl_* = client, g_* = game."},
	{"CGameClient",
		"Global `GameClient` (CGameClient): Config(), Collision(), Layers(), CurrentRaceTime(), LastRaceTick(), "
		"IsTeamPlay(), IsOtherTeam(), IsLocalCharSuper(), SendKill(), Echo(), Predict(), PredictDummy(), "
		"DummyResetInput(), GetTuning(), GetItemName(), IntersectCharacter(), ClientVersion7(), DDNetVersion(), DDNetVersionStr(). "
		"Snapshot access: GameClient.Snap (CSnapState: m_aCharacters[i] with m_Active/m_Pos/m_Input, m_apPlayerInfos, m_pGameDataObj, m_SpecInfo), "
		"GameClient.m_aClients[i] (CClientData: m_aName, m_aClan, m_aSkinName, m_RenderPos, m_Predicted). "
		"Local character input: GameClient.m_Controls.m_aInputData[0] (CNetObj_PlayerInput), GameClient.m_Controls.m_aMousePos[0]."},
	{"CBotNet",
		"Global `BotNet` (CBotNet): IsDummyActive(), IsTileFreeze(x,y), IsTileWalkable(x,y), ProcessDummy(), "
		"ResetDummyInputs(), m_aLastMapName. Bot input: GameClient.m_BotNet.m_Dummy / m_Player (CDummyData with m_aInputs). "
		"BotNet also drives AimBot/TriggerBot/Pathfinder internals (advanced use)."},
	{"CGameWorld",
		"GameClient.GameWorld() (CGameWorld): FindEntities<..>(Pos, Radius), FindFirst<..>(), GetCharacterById(id), "
		"GetEntity(id, type), GameTick(), GameTickSpeed(), Clear(), Collision(), Teams(), Switchers(), GlobalTuning(), TuningList(), m_Core (CWorldCore). "
		"CEntity base: m_Pos, GetPos(), GetId(), CanCollide(), Keep()."},
	{"CCharacterCore",
		"CCharacterCore: m_Pos, m_Vel, m_HookPos, m_HookDir, m_HookedPlayer, m_Input, m_aWeapons, m_Tuning. "
		"Methods: Tick(), TickDeferred(), Move(), Reset(), Init(), Read()/Write(), ReadDDNet(), Quantize(), "
		"PhysicalSize(), PhysicalSizeVec2(), SetCoreWorld(), SetTeamsCore()."},
	{"CNetObj_PlayerInput",
		"CNetObj_PlayerInput: m_Direction (-1/0/1), m_Jump (0/1, 2 = jump tick edge), m_Fire (0-2), m_Hook (0/1), "
		"m_PlayerState, m_WantedWeapon, m_NextWeapon, m_PrevWeapon, m_AimX, m_AimY (int coordinates relative to the tee). "
		"Write it into GameClient.m_Controls.m_aInputData[0] / m_BotNet.m_Dummy.m_aInputs[...] to drive a tee."},
	{"CCollision",
		"Global `Collision` (CCollision): GetTile(x,y)/GetFrontTile, GetIndex(), IsSolid(x,y), IsHookBlocker(), IsNoLaser(), "
		"IsOnGround(Pos), IntersectLine(Pos0, Pos1, &OutCollision, &OutBeforeCollision), IntersectLineTeleHook/TeleWeapon(), "
		"GetSwitchType/Number/Delay(x,y), GetSpeedup(x,y), GetTileIndex(), GetTileFlags(), GetMapIndex(), Height(Pos)."},
	{"CChat",
		"Global `Chat` (CChat): SendChat(text), AddLine(ClientId, Team, Text) (ClientId CChat::LUA_MSG / CHAT_CLIENT msg types), "
		"Echo(text), GetRecentLines(), ClearLines(), EnableMode()/DisableMode(), RegisterCommand(name, params, help, callback)."},
	{"CControls",
		"GameClient.m_Controls (CControls): m_aInputData[0] (CNetObj_PlayerInput), m_aMousePos[0] (vec2), m_aTargetPos[0], "
		"m_aInputDirectionLeft/Right[0], ClampMousePos(), GetMinMouseDistance()/GetMaxMouseDistance(), ResetInput()."},
	{"CCodeExec",
		"Global `CodeExec` (CCodeExec): Execute(code, chunkName) -> bool (starts a script; it runs co-operatively over frames), "
		"Stop(), GetState() (STATE_IDLE / STATE_RUNNING / STATE_ERROR), GetLastError(), m_aCodeBuffer. "
		"Only one script runs at a time. print(...) writes to chat."},
	{"CGameInfo",
		"GameClient.m_GameInfo / Snap.m_pGameInfoObj (CGameInfo): m_GameState, m_GameStateFlags, m_GameStartTick, "
		"m_WarmupTimer, m_SuddenDeath, m_RoundStartTick, m_BasesComplete, m_CurrentNumPlayers."},
	{"CCamera",
		"GameClient.m_Camera (CCamera): SetZoom(steps, force), ZoomAllowed(), ZoomStepsToValue(), CamType(), "
		"Deadzone(), FollowFactor(), m_Center, SpectatingPlayer(), ResetAutoSpecCamera()."},
	{"CBinds",
		"Global `Binds` (CBinds): Bind(keyId, command), Get(keyId, modifierMask), GetKey(pBindStr, pBuf, size), UnbindAll(), SetDefaults()."},
	{"net",
		"net.get(url[, timeoutSec]) / net.post(url, body[, timeoutSec]) / net.post_json(url, jsonBody, headersTable[, timeoutSec]) "
		"-> NetJob. Job: :done() (bool), :status(), :result() (string, only after done), :code() (HTTP status), :abort(). "
		"Requires the user's Execution Settings to allow Internet access, otherwise the call throws. 16MB response cap."},
	{"io",
		"Standard Lua io. When the sandbox (Execution Settings) is ON, all paths resolve inside <userdir>/Kinetix/Sandbox "
		"(io.open(rel) writes there; absolute paths must stay inside it, os.execute/io.popen are denied while the sandbox is on). "
		"Available: io.open, io.lines, io.input, io.output, os.remove, os.rename, dofile, loadfile, os.tmpname, require."},
	{"os",
		"Standard Lua os. os.execute(cmd) / io.popen(cmd) require Execution Settings: master ON, 'OS Commands' ON and the sandbox OFF. "
		"os.time(), os.date(), os.clock(), os.getenv, os.remove, os.rename are the usual helpers."},
	{"globals",
		"Globals: Config (CConfig), GameClient (CGameClient), BotNet (CBotNet), Collision (CCollision), Chat (CChat), "
		"Binds (CBinds), CodeExec (CCodeExec), GameClient.Controls/m_Camera/m_Spectator/m_Voting and more — see the per-class sections. "
		"print(...) -> chat; vec2(x,y) arithmetic is available."},
};

static std::string AgentDocLookup(const std::string &Query)
{
	std::string Q = Query;
	for(char &C : Q)
		C = (char)tolower((unsigned char)C);
	// Empty query or a "list" style query -> return the section list.
	if(Q.empty() || Q.find("section") != std::string::npos || Q.find("list") != std::string::npos ||
		Q.find("раздел") != std::string::npos || Q.find("справк") != std::string::npos)
	{
		for(const SAgentDocSection &S : g_aAgentDocSections)
			if(str_comp_nocase(S.pSection, "sections") == 0)
				return S.pDoc;
	}
	// Exact match first.
	for(const SAgentDocSection &S : g_aAgentDocSections)
		if(str_comp_nocase(S.pSection, Q.c_str()) == 0)
			return S.pDoc;
	// Substring match: the section name must appear in the query (the agent
	// sends free text like "Переменные класса CBotNet").
	std::string Best;
	for(const SAgentDocSection &S : g_aAgentDocSections)
	{
		if(str_comp_nocase(S.pSection, "sections") == 0)
			continue;
		std::string Name = S.pSection;
		for(char &C : Name)
			C = (char)tolower((unsigned char)C);
		if(Q.find(Name) != std::string::npos)
		{
			Best = S.pDoc;
			break;
		}
	}
	if(Best.empty())
	{
		for(const SAgentDocSection &S : g_aAgentDocSections)
			if(str_comp_nocase(S.pSection, "sections") == 0)
				Best = S.pDoc;
	}
	return Best;
}

static void AgentJsonEscape(const std::string &In, std::string &Out)
{
	for(unsigned char C : In)
	{
		switch(C)
		{
		case '"': Out += "\\\""; break;
		case '\\': Out += "\\\\"; break;
		case '\n': Out += "\\n"; break;
		case '\r': Out += "\\r"; break;
		case '\t': Out += "\\t"; break;
		default:
			if(C < 0x20)
			{
				char aBuf[8];
				str_format(aBuf, sizeof(aBuf), "\\u%04x", C);
				Out += aBuf;
			}
			else
				Out += (char)C;
		}
	}
}

static std::string AgentJsonField(const char *pName, const std::string &Value)
{
	std::string Out = "\"";
	Out += pName;
	Out += "\":\"";
	AgentJsonEscape(Value, Out);
	Out += "\"";
	return Out;
}

// {"name":"value" — object opening with the first field.
static std::string AgentJsonOpen(const char *pName, const std::string &Value)
{
	return std::string("{") + AgentJsonField(pName, Value);
}

void CAgent::OnInit()
{
	RefreshModels();
	// v6.14: sessions — start on the main page (when enabled) or in a fresh
	// temp session.
	m_CurrentSessionTime = std::time(nullptr);
	m_CurrentSessionTemp = true;
	RefreshSessions();
	m_SessionListVisible = g_Config.m_KxArrayAgentShowMainPage != 0;
	if(!m_SessionListVisible)
		NewSession();
}

void CAgent::OnReset()
{
	StopGeneration();
}

void CAgent::RefreshModels()
{
	m_vModels.clear();
	const char *pCursor = g_Config.m_KxArrayAgentModels;
	while(*pCursor)
	{
		while(*pCursor == ' ' || *pCursor == ',')
			++pCursor;
		if(!*pCursor)
			break;
		std::string Model;
		while(*pCursor && *pCursor != ',')
			Model += *pCursor++;
		// trim trailing spaces
		while(!Model.empty() && Model.back() == ' ')
			Model.pop_back();
		if(!Model.empty())
			m_vModels.push_back(Model);
	}
	if(m_vModels.empty())
		m_vModels.push_back("gpt-3.5-turbo");
	if(m_ModelIdx < 0 || m_ModelIdx >= (int)m_vModels.size())
		m_ModelIdx = 0;
}

void CAgent::Send(const char *pText)
{
	// v6.14: /exit and /sessions return to the main page (and stop a running
	// generation first).
	if(pText && (str_comp(pText, "/exit") == 0 || str_comp(pText, "/sessions") == 0))
	{
		ExitToSessionList();
		return;
	}
	if(IsBusy() || !pText || !pText[0])
		return;
	if(m_CurrentSessionTime == 0)
		m_CurrentSessionTime = std::time(nullptr);
	m_aError[0] = '\0';
	SAgentMessage User;
	User.m_Role = ROLE_USER;
	User.m_Text = pText;
	User.m_Time = std::time(nullptr);
	m_vMessages.push_back(std::move(User));
	if(m_CurrentSessionTitle.empty())
		m_CurrentSessionTitle = SessionTitle();
	m_vApiMessages.push_back(AgentJsonOpen("role", "user") + "," + AgentJsonField("content", pText) + "}");
	m_RoundTrips = 0; // per-conversation counter (fallback tool-call ids)
	StartRequest();
}

void CAgent::StopGeneration()
{
	if(m_pRequest)
		m_pRequest->Abort();
	if(GameClient()->m_CodeExec.ToolCaptureActive())
		GameClient()->m_CodeExec.Stop();
	m_pRequest = nullptr;
	m_State = AGENT_IDLE;
	m_vToolCalls.clear();
	m_vPendingToolCalls.clear();
	m_ToolRun = TOOLRUN_NONE;
	// Kill a running shell command and collect its threads; on non-Windows the
	// popen thread cannot be interrupted, so it is detached and frees itself.
	if(m_pShellJob)
	{
		m_pShellJob->m_Kill = true;
#if defined(CONF_FAMILY_WINDOWS)
		{
			std::lock_guard<std::mutex> Lock(m_pShellJob->m_Mutex);
			if(m_pShellJob->m_pProcess)
				TerminateProcess((HANDLE)m_pShellJob->m_pProcess, 1);
		}
		if(m_pShellJob->m_Thread.joinable())
			m_pShellJob->m_Thread.join();
		if(m_pShellJob->m_OutThread.joinable())
			m_pShellJob->m_OutThread.join();
		if(m_pShellJob->m_ErrThread.joinable())
			m_pShellJob->m_ErrThread.join();
#else
		if(m_pShellJob->m_Thread.joinable())
			m_pShellJob->m_Thread.detach();
#endif
		m_pShellJob.reset();
	}
	if(m_ActiveBgJob >= 0)
	{
		for(SBgJob &J : m_vBgJobs)
		{
			if(J.m_Id != m_ActiveBgJob)
				continue;
			J.m_Killed = true;
			J.m_Finished = true;
			break;
		}
		m_ActiveBgJob = -1;
	}
	m_SseBuf.clear();
	m_SseDone = false;
	m_AbortRequested = false;
	for(SAgentMessage &M : m_vMessages)
		M.m_Streaming = false;
}

// Built per request from the shells actually present in PATH (defined below).
static std::string AgentBuildShellToolJson();

void CAgent::StartRequest()
{
	// Request body: system prompt + transcript + tools (+ effort).
	std::string Body = "{";
	Body += AgentJsonField("model", m_vModels[std::clamp(m_ModelIdx, 0, (int)m_vModels.size() - 1)]) + ",";
	Body += "\"stream\":true,";
	if(m_EffortIdx > 0)
	{
		const char *apEffort[] = {nullptr, "low", "high", "max"};
		Body += AgentJsonField("reasoning_effort", apEffort[m_EffortIdx]) + ",";
	}
	Body += "\"messages\":[" + AgentJsonOpen("role", "system") + "," + AgentJsonField("content", KX_AGENT_SYSTEM_PROMPT) + "}";
	for(const std::string &Msg : m_vApiMessages)
	{
		Body += ",";
		Body += Msg;
	}
	Body += "],";
	// v6.14: the extra tools are appended inside the tools array.
	std::string ToolsJson = KX_AGENT_TOOLS_JSON;
	// Expose the code_execution "file" option (execute a Lua file from the
	// workdir instead of inline code) and drop the required "code" argument.
	{
		const std::string Q(1, (char)34);
		const std::string PropNeedle = Q + "properties" + Q + ":{" + Q + "code" + Q;
		const std::string FileProp = Q + "properties" + Q + ":{" + Q + "file" + Q + ":{" +
			Q + "type" + Q + ":" + Q + "string" + Q + "," +
			Q + "description" + Q + ":" + Q + "Path to a Lua file to execute instead of inline code (relative to the workdir)" + Q +
			"}," + Q + "code" + Q;
		const size_t PropPos = ToolsJson.find(PropNeedle);
		if(PropPos != std::string::npos)
			ToolsJson.replace(PropPos, PropNeedle.size(), FileProp);
		const std::string ReqNeedle = Q + "required" + Q + ":[" + Q + "code" + Q + "]";
		const std::string ReqNew = Q + "required" + Q + ":[]";
		const size_t ReqPos = ToolsJson.find(ReqNeedle);
		if(ReqPos != std::string::npos)
			ToolsJson.replace(ReqPos, ReqNeedle.size(), ReqNew);
	}
	const size_t ToolsClose = ToolsJson.rfind(']');
	if(ToolsClose != std::string::npos)
	{
		// The shell tool lists only shells found in PATH; it disappears
		// entirely when none is available.
		std::string ExtraTools = AgentBuildShellToolJson();
		ExtraTools += KX_AGENT_TOOLS_EXTRA_JSON;
		ToolsJson.insert(ToolsClose, ExtraTools);
	}
	Body += ToolsJson;
	Body += "}";

	char aUrl[512];
	str_format(aUrl, sizeof(aUrl), "%s/chat/completions", g_Config.m_KxArrayAgentEndpoint);

	std::unique_ptr<IHttpRequest> pRequest = HttpPostJson(aUrl, Body.c_str());
	pRequest->Timeout(CTimeout{4000, 0, 1, 60}); // no total timeout — SSE may idle
	pRequest->MaxResponseSize(64 * 1024 * 1024);
	pRequest->FailOnErrorStatus(false);
	pRequest->LogProgress(HTTPLOG::FAILURE);
	pRequest->AllowHttp(true); // user endpoint may be plain http (e.g. LM Studio on 127.0.0.1)
	pRequest->HeaderString("Authorization", g_Config.m_KxArrayAgentApiKey);
	pRequest->HeaderString("Accept", "text/event-stream");
	pRequest->WriteToMemory();

	m_pRequest = std::shared_ptr<IHttpRequest>(pRequest.release());
	Http()->Run(m_pRequest);

	// The streaming assistant message (updated live by the parser).
	SAgentMessage A;
	A.m_Role = ROLE_ASSISTANT;
	A.m_Streaming = true;
	A.m_Time = std::time(nullptr);
	m_vMessages.push_back(std::move(A));
	m_PendingText.clear();
	m_PendingReasoning.clear();
	m_vToolCalls.clear();
	m_FinishReason.clear();
	m_UsageTokens = -1;
	m_SseBuf.clear();
	m_SseDone = false;
	m_AbortRequested = false;
	m_BusyMeta = false;
	m_RequestStart = std::chrono::steady_clock::now();
	m_RoundTrips++;
	m_State = AGENT_REQUEST;
}

void CAgent::ConsumeStream()
{
	unsigned char aBuf[16384];
	size_t Taken;
	while((Taken = m_pRequest->StreamTake(aBuf, sizeof(aBuf))) > 0)
		m_SseBuf.append((const char *)aBuf, Taken);

	// Process complete SSE lines.
	size_t Start = 0;
	while(true)
	{
		size_t NL = m_SseBuf.find('\n', Start);
		if(NL == std::string::npos)
			break;
		std::string Line = m_SseBuf.substr(Start, NL - Start);
		Start = NL + 1;
		if(!Line.empty() && Line.back() == '\r')
			Line.pop_back();
		if(str_startswith(Line.c_str(), "data:"))
		{
			const char *pData = Line.c_str() + 5;
			while(*pData == ' ')
				++pData;
			if(str_comp(pData, "[DONE]") == 0)
				m_SseDone = true;
			else
				ProcessChunk(pData);
		}
	}
	m_SseBuf.erase(0, Start);

	// v6.13: keep the live streaming message text in sync — the HUD renders
	// the message's m_Text, not m_PendingText.
	if(!m_vMessages.empty() && m_vMessages.back().m_Streaming && m_vMessages.back().m_Role == ROLE_ASSISTANT)
	{
		m_vMessages.back().m_Text = m_PendingText;
		m_vMessages.back().m_Reasoning = m_PendingReasoning;
	}
}

void CAgent::ProcessChunk(const char *pJson)
{
	json_value *pRoot = JsonParse(pJson, str_length(pJson));
	if(!pRoot)
		return;
	if(pRoot->type == json_object)
	{
		// Error payloads: {"error": {...}} (LM Studio style) — surface as red.
		// Note: "error": null is a NON-null json_value node of type json_null —
		// several proxies emit it in every chunk; only treat an object/string
		// payload as a real error.
		const json_value *pError = json_object_get(pRoot, "error");
		if(pError && (pError->type == json_object || pError->type == json_string))
		{
			std::string Text = "API error";
			if(pError->type == json_object)
			{
				if(const json_value *pMsg = json_object_get(pError, "message"))
					if(json_string_get(pMsg))
						Text = json_string_get(pMsg);
			}
			else if(pError->type == json_string)
			{
				Text = json_string_get(pError);
			}
			SAgentMessage Err;
			Err.m_Role = ROLE_ERROR;
			Err.m_Text = Text;
			m_vMessages.push_back(std::move(Err));
			str_copy(m_aError, Text.c_str(), sizeof(m_aError));
			m_AbortRequested = true;
		}
		else if(const json_value *pChoices = json_object_get(pRoot, "choices"))
		{
			if(pChoices->type == json_array && json_array_length(pChoices) > 0)
			{
				const json_value *pChoice = json_array_get(pChoices, 0);
				if(const json_value *pDelta = json_object_get(pChoice, "delta"))
				{
					if(const json_value *pContent = json_object_get(pDelta, "content"))
					{
						if(const char *pText = json_string_get(pContent))
							m_PendingText += pText;
					}
					// Reasoning models (deepseek-reasoner etc.) stream the
					// thinking in a separate delta field.
					if(const json_value *pReason = json_object_get(pDelta, "reasoning_content"))
					{
						if(const char *pText = json_string_get(pReason))
							m_PendingReasoning += pText;
					}
					if(const json_value *pCalls = json_object_get(pDelta, "tool_calls"))
					{
						if(pCalls->type == json_array)
						{
							for(int i = 0; i < json_array_length(pCalls); i++)
							{
								const json_value *pCall = json_array_get(pCalls, i);
								int Index = 0;
								if(const json_value *pIdx = json_object_get(pCall, "index"))
									Index = json_int_get(pIdx);
								while((int)m_vToolCalls.size() <= Index)
									m_vToolCalls.emplace_back();
								SToolCall &Call = m_vToolCalls[Index];
								if(const json_value *pId = json_object_get(pCall, "id"))
									if(const char *pS = json_string_get(pId))
										Call.m_Id = pS;
								if(const json_value *pFn = json_object_get(pCall, "function"))
								{
									if(const json_value *pName = json_object_get(pFn, "name"))
										if(const char *pS = json_string_get(pName))
											Call.m_Name = pS;
									if(const json_value *pArgs = json_object_get(pFn, "arguments"))
										if(const char *pS = json_string_get(pArgs))
											Call.m_Args += pS;
								}
							}
						}
					}
				}
				if(const json_value *pFinish = json_object_get(pChoice, "finish_reason"))
				{
					if(const char *pS = json_string_get(pFinish))
						m_FinishReason = pS;
				}
			}
		}
		if(const json_value *pUsage = json_object_get(pRoot, "usage"))
		{
			int Tokens = 0;
			if(const json_value *pV = json_object_get(pUsage, "total_tokens"))
				Tokens += json_int_get(pV);
			if(const json_value *pV = json_object_get(pUsage, "completion_tokens"))
				Tokens += json_int_get(pV);
			if(Tokens > 0)
				m_UsageTokens = Tokens;
		}
	}
	json_value_free(pRoot);
}

void CAgent::FinishRequest(EHttpState State)
{
	const int WorkSec = (int)std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - m_RequestStart).count();
	m_pRequest = nullptr;

	const bool Failed = State != EHttpState::DONE || m_AbortRequested;
	if(State != EHttpState::DONE && State != EHttpState::ABORTED)
		str_copy(m_aError, "HTTP request failed", sizeof(m_aError));

	// The live streaming message is finalized IN PLACE (mutating it instead of
	// removing/re-adding kept the list stable while streaming and across tool
	// round trips).
	CAgent::SAgentMessage *pStream = nullptr;
	if(!m_vMessages.empty() && m_vMessages.back().m_Streaming && m_vMessages.back().m_Role == ROLE_ASSISTANT)
		pStream = &m_vMessages.back();

	if(Failed)
	{
		if(!m_aError[0])
			str_copy(m_aError, "Request failed", sizeof(m_aError));
		SAgentMessage Err;
		Err.m_Role = ROLE_ERROR;
		Err.m_Text = m_aError;
		m_vMessages.push_back(std::move(Err));
		if(pStream)
			pStream->m_Streaming = false; // keep the partial answer visible
		m_State = AGENT_IDLE;
		SaveCurrentSession();
		return;
	}

	if(m_FinishReason == "tool_calls" && !m_vToolCalls.empty())
	{
		// Assistant turn with tool calls — replay it in the API transcript.
		std::string Assistant = AgentJsonOpen("role", "assistant");
		if(!m_PendingText.empty())
			Assistant += "," + AgentJsonField("content", m_PendingText);
		Assistant += ",\"tool_calls\":[";
		bool First = true;
		for(const SToolCall &Call : m_vToolCalls)
		{
			if(!First)
				Assistant += ",";
			First = false;
			Assistant += "{" + AgentJsonField("id", Call.m_Id.empty() ? "call_kx_" + std::to_string(m_RoundTrips) : Call.m_Id) +
				",\"type\":\"function\",\"function\":{" + AgentJsonField("name", Call.m_Name) + "," +
				AgentJsonField("arguments", Call.m_Args.empty() ? "{}" : Call.m_Args) + "}}";
		}
		Assistant += "]}";
		m_vApiMessages.push_back(Assistant);
		// Display: the assistant text (if any) + one tool block per call.
		if(pStream)
			m_vMessages.pop_back(); // streaming placeholder is replaced below
		// A reasoning-only turn must NOT push an empty assistant bubble (it
		// rendered as a blank line).
		if(!m_PendingText.empty())
		{
			SAgentMessage A;
			A.m_Role = ROLE_ASSISTANT;
			A.m_Text = m_PendingText;
			A.m_Reasoning = m_PendingReasoning;
			A.m_Time = std::time(nullptr);
			m_vMessages.push_back(std::move(A));
		}
		for(const SToolCall &Call : m_vToolCalls)
		{
			SAgentMessage T;
			T.m_Role = ROLE_TOOL;
			T.m_ToolCallId = Call.m_Id;
			T.m_ToolName = Call.m_Name.empty() ? "code_execution" : Call.m_Name;
			T.m_ToolArgs = Call.m_Args;
			T.m_Time = std::time(nullptr);
			m_vMessages.push_back(std::move(T));
		}
		m_vPendingToolCalls = m_vToolCalls;
		m_vToolCalls.clear();
		m_State = AGENT_TOOL_RUN;
		SaveCurrentSession();
		return;
	}

	// Plain assistant answer — finalize the streaming message in place.
	const int Tokens = m_UsageTokens > 0 ? m_UsageTokens : (int)((m_PendingText.size() + 3) / 4);
	if(pStream)
	{
		pStream->m_Streaming = false;
		pStream->m_Tokens = Tokens;
		pStream->m_WorkSec = WorkSec;
		pStream->m_Reasoning = m_PendingReasoning;
		if(pStream->m_Text.empty())
			pStream->m_Text = "(empty response)";
	}
	else
	{
		SAgentMessage A;
		A.m_Role = ROLE_ASSISTANT;
		A.m_Text = m_PendingText.empty() ? "(empty response)" : m_PendingText;
		A.m_Tokens = Tokens;
		A.m_WorkSec = WorkSec;
		A.m_Time = std::time(nullptr);
		m_vMessages.push_back(std::move(A));
	}
	m_vApiMessages.push_back(AgentJsonOpen("role", "assistant") + "," + AgentJsonField("content", m_PendingText.empty() ? "(empty response)" : m_PendingText) + "}");
	m_State = AGENT_IDLE;
	SaveCurrentSession();
}

// ============================================================================
// v6.14: external tools — shell / files / console / background jobs.
// ============================================================================

static std::string AgentJoinLines(const std::vector<std::string> &vLines)
{
	std::string Out;
	for(size_t i = 0; i < vLines.size(); ++i)
	{
		if(i > 0)
			Out += "\n";
		Out += vLines[i];
	}
	return Out;
}

// '/' separators, "." and ".." resolved. "C:" drive prefixes are kept.
static std::string AgentNormalizePath(const std::string &In)
{
	std::string S = In;
	for(char &C : S)
		if(C == '\\')
			C = '/';
	std::string Drive;
	bool Abs = false;
	if(S.size() >= 2 && S[1] == ':')
	{
		Abs = true;
		Drive = S.substr(0, 2);
		S = S.substr(2);
	}
	else if(!S.empty() && S[0] == '/')
		Abs = true;
	std::vector<std::string> vParts;
	size_t i = 0;
	while(i < S.size())
	{
		size_t j = S.find('/', i);
		if(j == std::string::npos)
			j = S.size();
		const std::string Part = S.substr(i, j - i);
		i = j + 1;
		if(Part.empty() || Part == ".")
			continue;
		if(Part == "..")
		{
			if(!vParts.empty() && vParts.back() != "..")
				vParts.pop_back();
			else if(!Abs)
				vParts.push_back("..");
			continue;
		}
		vParts.push_back(Part);
	}
	std::string Joined;
	for(size_t k = 0; k < vParts.size(); ++k)
	{
		if(k > 0)
			Joined += "/";
		Joined += vParts[k];
	}
	if(Abs)
		return Drive.empty() ? ("/" + Joined) : (Drive + "/" + Joined);
	return Joined;
}

std::string CAgent::AgentFilesRoot() const
{
	if(g_Config.m_KxArrayAgentWorkdir[0])
		return AgentNormalizePath(g_Config.m_KxArrayAgentWorkdir);
	Storage()->CreateFolder("Kinetix", IStorage::TYPE_SAVE);
	char aBuf[512];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "Kinetix", aBuf, sizeof(aBuf));
	return AgentNormalizePath(aBuf);
}

std::string CAgent::AgentResolvePath(const std::string &Path) const
{
	const std::string Norm = AgentNormalizePath(Path);
	const bool Abs = (!Norm.empty() && Norm[0] == '/') || (Norm.size() >= 2 && Norm[1] == ':');
	if(Abs)
		return Norm;
	return AgentNormalizePath(AgentFilesRoot() + "/" + Norm);
}

static bool AgentReadFile(const std::string &Path, std::string &Out, std::string &Err)
{
	IOHANDLE File = io_open(Path.c_str(), IOFLAG_READ);
	if(!File)
	{
		Err = "cannot open file";
		return false;
	}
	char *pData = io_read_all_str(File);
	io_close(File);
	if(!pData)
	{
		Err = "read failed";
		return false;
	}
	Out = pData;
	free(pData);
	return true;
}

static bool AgentWriteFile(const std::string &Path, const std::string &Data, std::string &Err)
{
	const size_t Slash = Path.find_last_of('/');
	if(Slash != std::string::npos && Slash > 0)
		fs_makedir_rec_for(Path.substr(0, Slash).c_str());
	IOHANDLE File = io_open(Path.c_str(), IOFLAG_WRITE);
	if(!File)
	{
		Err = "cannot open for writing";
		return false;
	}
	const bool Ok = io_write(File, Data.data(), (unsigned)Data.size()) == (unsigned)Data.size();
	io_close(File);
	if(!Ok)
	{
		Err = "write failed";
		return false;
	}
	return true;
}

static int AgentToLower(int C) { return (C >= 'A' && C <= 'Z') ? C - 'A' + 'a' : C; }

// Case-insensitive wildcard match (* and ?).
static bool AgentWildMatch(const char *pPattern, const char *pName)
{
	while(*pPattern)
	{
		if(*pPattern == '*')
		{
			while(*pPattern == '*')
				++pPattern;
			if(!*pPattern)
				return true;
			for(const char *p = pName;; ++p)
			{
				if(AgentWildMatch(pPattern, p))
					return true;
				if(!*p)
					return false;
			}
		}
		if(!*pName)
			return false;
		if(*pPattern != '?' && AgentToLower(*pPattern) != AgentToLower(*pName))
			return false;
		++pPattern;
		++pName;
	}
	return *pName == '\0';
}

// ============================================================================
// v6.14: sessions — one JSON file per chat in Kinetix/Sessions. The HUD main
// page lists them; /exit and /sessions return to it.
// ============================================================================
// ""name"" built without escaped quote literals.
static std::string AgentJsonKey(const char *pName)
{
	std::string Out;
	Out += (char)34;
	Out += pName;
	Out += (char)34;
	return Out;
}

static std::string AgentJsonRawInt(const char *pName, long long Value)
{
	return AgentJsonKey(pName) + ":" + std::to_string(Value);
}

static std::string AgentSessionTrim(const std::string &S)
{
	size_t B = 0, E = S.size();
	while(B < E && (S[B] == ' ' || S[B] == (char)9 || S[B] == (char)10 || S[B] == (char)13))
		++B;
	while(E > B && (S[E - 1] == ' ' || S[E - 1] == (char)9 || S[E - 1] == (char)10 || S[E - 1] == (char)13))
		--E;
	return S.substr(B, E - B);
}

std::string CAgent::SessionsDir() const
{
	Storage()->CreateFolder("Kinetix", IStorage::TYPE_SAVE);
	Storage()->CreateFolder("Kinetix/Sessions", IStorage::TYPE_SAVE);
	char aBuf[512];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "Kinetix/Sessions", aBuf, sizeof(aBuf));
	return AgentNormalizePath(aBuf);
}

std::string CAgent::SessionTitle() const
{
	for(const SAgentMessage &M : m_vMessages)
	{
		if(M.m_Role != ROLE_USER)
			continue;
		std::string T = AgentSessionTrim(M.m_Text);
		const size_t NL = T.find((char)10);
		if(NL != std::string::npos)
			T = T.substr(0, NL);
		if(T.empty())
			continue;
		if(T.size() > 40)
			T = T.substr(0, 40) + "...";
		return T;
	}
	return "";
}

std::string CAgent::BuildSessionJson() const
{
	std::string J = "{";
	J += AgentJsonField("title", m_CurrentSessionTitle) + ",";
	J += AgentJsonRawInt("time", (long long)m_CurrentSessionTime) + ",";
	J += AgentJsonKey("messages") + ":[";
	for(size_t i = 0; i < m_vMessages.size(); ++i)
	{
		const SAgentMessage &M = m_vMessages[i];
		if(i > 0)
			J += ",";
		J += "{";
		J += AgentJsonRawInt("role", M.m_Role) + ",";
		J += AgentJsonField("text", M.m_Text) + ",";
		J += AgentJsonField("reasoning", M.m_Reasoning) + ",";
		J += AgentJsonRawInt("time", (long long)M.m_Time) + ",";
		J += AgentJsonRawInt("tokens", M.m_Tokens) + ",";
		J += AgentJsonRawInt("work", M.m_WorkSec) + ",";
		J += AgentJsonField("tool_name", M.m_ToolName) + ",";
		J += AgentJsonField("tool_args", M.m_ToolArgs) + ",";
		J += AgentJsonField("tool_result", M.m_ToolResult) + ",";
		J += AgentJsonField("tool_id", M.m_ToolCallId);
		J += "}";
	}
	J += "],";
	J += AgentJsonKey("api") + ":[";
	for(size_t i = 0; i < m_vApiMessages.size(); ++i)
	{
		if(i > 0)
			J += ",";
		J += (char)34;
		AgentJsonEscape(m_vApiMessages[i], J);
		J += (char)34;
	}
	J += "]}";
	return J;
}

void CAgent::SaveCurrentSession()
{
	if(g_Config.m_KxArrayAgentSaveSessions == 0)
		return;
	if(m_vMessages.empty())
		return;
	if(m_CurrentSessionTitle.empty())
		m_CurrentSessionTitle = SessionTitle();
	if(m_CurrentSessionTime == 0)
		m_CurrentSessionTime = std::time(nullptr);
	const std::string Dir = SessionsDir();
	if(m_CurrentSessionId.empty())
	{
		const std::string Base = "session_" + std::to_string((long long)m_CurrentSessionTime);
		std::string Id = Base;
		int Suffix = 2;
		std::string Probe, ProbeErr;
		while(AgentReadFile(Dir + "/" + Id + ".json", Probe, ProbeErr))
			Id = Base + "_" + std::to_string(Suffix++);
		m_CurrentSessionId = Id;
	}
	std::string Err;
	AgentWriteFile(Dir + "/" + m_CurrentSessionId + ".json", BuildSessionJson(), Err);
	m_CurrentSessionTemp = false;
	RefreshSessions();
}

bool CAgent::LoadSessionFile(const std::string &Id)
{
	std::string Data, Err;
	if(!AgentReadFile(SessionsDir() + "/" + Id + ".json", Data, Err))
		return false;
	json_value *pRoot = JsonParse(Data.c_str(), Data.size());
	if(!pRoot)
		return false;
	if(pRoot->type != json_object)
	{
		json_value_free(pRoot);
		return false;
	}
	auto GetStr = [](const json_value *pObj, const char *pKey, std::string &Out) {
		const json_value *pV = json_object_get(pObj, pKey);
		if(pV && pV->type == json_string)
			Out = json_string_get(pV);
	};
	auto GetInt = [](const json_value *pObj, const char *pKey, int &Out) {
		const json_value *pV = json_object_get(pObj, pKey);
		if(pV && (pV->type == json_integer || pV->type == json_double))
			Out = json_int_get(pV);
	};
	std::vector<SAgentMessage> vMsgs;
	std::vector<std::string> vApi;
	const json_value *pMsgs = json_object_get(pRoot, "messages");
	if(pMsgs && pMsgs->type == json_array)
	{
		const int N = json_array_length(pMsgs);
		for(int i = 0; i < N; ++i)
		{
			const json_value *pM = json_array_get(pMsgs, i);
			if(!pM || pM->type != json_object)
				continue;
			SAgentMessage M;
			GetInt(pM, "role", M.m_Role);
			GetStr(pM, "text", M.m_Text);
			GetStr(pM, "reasoning", M.m_Reasoning);
			int T = 0;
			GetInt(pM, "time", T);
			M.m_Time = (time_t)T;
			GetInt(pM, "tokens", M.m_Tokens);
			GetInt(pM, "work", M.m_WorkSec);
			GetStr(pM, "tool_name", M.m_ToolName);
			GetStr(pM, "tool_args", M.m_ToolArgs);
			GetStr(pM, "tool_result", M.m_ToolResult);
			GetStr(pM, "tool_id", M.m_ToolCallId);
			vMsgs.push_back(std::move(M));
		}
	}
	const json_value *pApi = json_object_get(pRoot, "api");
	if(pApi && pApi->type == json_array)
	{
		const int N = json_array_length(pApi);
		for(int i = 0; i < N; ++i)
		{
			const json_value *pV = json_array_get(pApi, i);
			if(pV && pV->type == json_string)
				vApi.push_back(json_string_get(pV));
		}
	}
	m_vMessages = std::move(vMsgs);
	m_vApiMessages = std::move(vApi);
	m_CurrentSessionId = Id;
	m_CurrentSessionTemp = false;
	m_CurrentSessionTitle.clear();
	GetStr(pRoot, "title", m_CurrentSessionTitle);
	int T = 0;
	GetInt(pRoot, "time", T);
	m_CurrentSessionTime = (time_t)T;
	json_value_free(pRoot);
	return true;
}

void CAgent::RefreshSessions()
{
	m_vSessions.clear();
	struct SScan
	{
		std::vector<SSessionInfo> *m_pOut;
		std::string m_Dir;
	};
	SScan Scan;
	Scan.m_pOut = &m_vSessions;
	Scan.m_Dir = SessionsDir();
	fs_listdir(Scan.m_Dir.c_str(), [](const char *pName, int IsDir, int DirType, void *pUser) -> int {
		(void)DirType;
		if(IsDir)
			return 0;
		SScan *pScan = (SScan *)pUser;
		const char *pDot = str_rchr(pName, '.');
		if(!pDot || str_comp_nocase(pDot, ".json") != 0)
			return 0;
		SSessionInfo Info;
		Info.m_Id = std::string(pName, (size_t)(pDot - pName));
		Info.m_Title = Info.m_Id;
		std::string Data, Err;
		if(AgentReadFile(pScan->m_Dir + "/" + pName, Data, Err))
		{
			json_value *pRoot = JsonParse(Data.c_str(), Data.size());
			if(pRoot && pRoot->type == json_object)
			{
				const json_value *pV = json_object_get(pRoot, "title");
				if(pV && pV->type == json_string && json_string_get(pV)[0])
					Info.m_Title = json_string_get(pV);
				pV = json_object_get(pRoot, "time");
				if(pV && (pV->type == json_integer || pV->type == json_double))
					Info.m_Time = (time_t)json_int_get(pV);
			}
			if(pRoot)
				json_value_free(pRoot);
		}
		pScan->m_pOut->push_back(std::move(Info));
		return 0;
	}, 0, &Scan);
	std::sort(m_vSessions.begin(), m_vSessions.end(), [](const SSessionInfo &A, const SSessionInfo &B) {
		return A.m_Time > B.m_Time;
	});
}

void CAgent::NewSession()
{
	SaveCurrentSession();
	StopGeneration();
	m_vMessages.clear();
	m_vApiMessages.clear();
	m_CurrentSessionId.clear();
	m_CurrentSessionTitle.clear();
	m_CurrentSessionTemp = true;
	m_CurrentSessionTime = std::time(nullptr);
	m_SessionListVisible = false;
}

void CAgent::OpenSession(const std::string &Id)
{
	if(Id == "temp_current")
	{
		m_SessionListVisible = false;
		return;
	}
	SaveCurrentSession();
	StopGeneration();
	if(LoadSessionFile(Id))
		m_SessionListVisible = false;
}

void CAgent::ExitToSessionList()
{
	SaveCurrentSession();
	StopGeneration();
	if(g_Config.m_KxArrayAgentShowMainPage == 0)
	{
		NewSession();
		return;
	}
	RefreshSessions();
	m_SessionListVisible = true;
}

struct SGlobEntry
{
	std::string m_Name;
	bool m_Dir;
};

static void AgentGlobRec(const std::string &AbsDir, const std::string &RelDir,
	const std::vector<std::string> &vPat, size_t Idx, std::vector<std::string> &vOut)
{
	if(vOut.size() >= 300 || Idx >= vPat.size())
		return;
	std::vector<SGlobEntry> vEntries;
	fs_listdir(AbsDir.c_str(), [](const char *pName, int IsDir, int DirType, void *pUser) -> int {
		(void)DirType;
		auto *pV = (std::vector<SGlobEntry> *)pUser;
		pV->push_back({pName, IsDir != 0});
		return 0;
	}, 0, &vEntries);
	const std::string &Comp = vPat[Idx];
	const bool Last = (Idx + 1 == vPat.size());
	if(Comp == "**")
	{
		AgentGlobRec(AbsDir, RelDir, vPat, Idx + 1, vOut);
		for(const SGlobEntry &E : vEntries)
		{
			if(!E.m_Dir)
				continue;
			AgentGlobRec(AbsDir + "/" + E.m_Name, RelDir + E.m_Name + "/", vPat, Idx, vOut);
			if(vOut.size() >= 300)
				return;
		}
		return;
	}
	for(const SGlobEntry &E : vEntries)
	{
		if(!AgentWildMatch(Comp.c_str(), E.m_Name.c_str()))
			continue;
		if(Last)
		{
			if(!E.m_Dir)
			{
				vOut.push_back(RelDir + E.m_Name);
				if(vOut.size() >= 300)
					return;
			}
		}
		else if(E.m_Dir)
		{
			AgentGlobRec(AbsDir + "/" + E.m_Name, RelDir + E.m_Name + "/", vPat, Idx + 1, vOut);
			if(vOut.size() >= 300)
				return;
		}
	}
}

// Case-insensitive substring search (grep).
static bool AgentContainsNoCase(const std::string &Hay, const std::string &Needle)
{
	if(Needle.empty())
		return true;
	if(Hay.size() < Needle.size())
		return false;
	for(size_t i = 0; i + Needle.size() <= Hay.size(); ++i)
	{
		size_t j = 0;
		while(j < Needle.size() && AgentToLower((unsigned char)Hay[i + j]) == AgentToLower((unsigned char)Needle[j]))
			++j;
		if(j == Needle.size())
			return true;
	}
	return false;
}

// Recursively collects every file below AbsDir (relative paths in vOut).
static void AgentCollectFiles(const std::string &AbsDir, const std::string &RelDir, std::vector<std::string> &vOut, int Max)
{
	if((int)vOut.size() >= Max)
		return;
	std::vector<SGlobEntry> vEntries;
	fs_listdir(AbsDir.c_str(), [](const char *pName, int IsDir, int DirType, void *pUser) -> int {
		(void)DirType;
		auto *pV = (std::vector<SGlobEntry> *)pUser;
		pV->push_back({pName, IsDir != 0});
		return 0;
	}, 0, &vEntries);
	for(const SGlobEntry &E : vEntries)
	{
		if(E.m_Dir)
			AgentCollectFiles(AbsDir + "/" + E.m_Name, RelDir + E.m_Name + "/", vOut, Max);
		else
			vOut.push_back(RelDir + E.m_Name);
		if((int)vOut.size() >= Max)
			return;
	}
}

void CAgent::RunFilesTool(const SToolCall &Call)
{
	std::string Action, Path, Content, Old, New, Pattern, Include;
	int Offset = 0;
	int Limit = 2000;
	if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
	{
		if(pRoot->type == json_object)
		{
			if(const json_value *pV = json_object_get(pRoot, "action"))
				if(const char *pS = json_string_get(pV))
					Action = pS;
			if(const json_value *pV = json_object_get(pRoot, "path"))
				if(const char *pS = json_string_get(pV))
					Path = pS;
			if(const json_value *pV = json_object_get(pRoot, "content"))
				if(const char *pS = json_string_get(pV))
					Content = pS;
			if(const json_value *pV = json_object_get(pRoot, "old"))
				if(const char *pS = json_string_get(pV))
					Old = pS;
			if(const json_value *pV = json_object_get(pRoot, "new"))
				if(const char *pS = json_string_get(pV))
					New = pS;
			if(const json_value *pV = json_object_get(pRoot, "oldString"))
				if(const char *pS = json_string_get(pV))
					Old = pS;
			if(const json_value *pV = json_object_get(pRoot, "newString"))
				if(const char *pS = json_string_get(pV))
					New = pS;
			if(const json_value *pV = json_object_get(pRoot, "include"))
				if(const char *pS = json_string_get(pV))
					Include = pS;
			if(const json_value *pV = json_object_get(pRoot, "pattern"))
				if(const char *pS = json_string_get(pV))
					Pattern = pS;
			if(const json_value *pV = json_object_get(pRoot, "offset"))
				Offset = json_int_get(pV);
			if(const json_value *pV = json_object_get(pRoot, "limit"))
				Limit = json_int_get(pV);
		}
		json_value_free(pRoot);
	}
	// Separate tools (read/write/edit/glob/grep) carry their action in the
	// tool name; the legacy combined "files" tool used an action field.
	if(Action.empty() && Call.m_Name != "files")
		Action = Call.m_Name;
	if(Action.empty())
	{
		AppendToolResult(Call.m_Id, "files", "error: missing action");
		return;
	}
	if(Action != "glob" && Action != "list" && Path.empty())
	{
		AppendToolResult(Call.m_Id, "files", "error: missing path");
		return;
	}
	if(Action == "read")
	{
		const std::string Resolved = AgentResolvePath(Path);
		std::string Data, Err;
		if(!AgentReadFile(Resolved, Data, Err))
		{
			AppendToolResult(Call.m_Id, "files", "error: " + Err + ": " + Resolved);
			return;
		}
		std::vector<std::string> vLines;
		size_t Start = 0;
		while(Start <= Data.size())
		{
			size_t NL = Data.find('\n', Start);
			if(NL == std::string::npos)
			{
				vLines.push_back(Data.substr(Start));
				break;
			}
			std::string L = Data.substr(Start, NL - Start);
			if(!L.empty() && L.back() == '\r')
				L.pop_back();
			vLines.push_back(L);
			Start = NL + 1;
		}
		if(!vLines.empty() && vLines.back().empty())
			vLines.pop_back();
		const int Total = (int)vLines.size();
		if(Offset < 0)
			Offset = 0;
		if(Limit <= 0 || Limit > 2000)
			Limit = 2000;
		const int End = std::min(Total, Offset + Limit);
		std::string Out;
		for(int i = Offset; i < End; ++i)
		{
			char aNum[32];
			str_format(aNum, sizeof(aNum), "%5d	", i + 1);
			Out += aNum;
			Out += vLines[i];
			Out += "\n";
			if(Out.size() > 200 * 1024)
			{
				Out += "(output truncated)\n";
				break;
			}
		}
		if(Out.empty())
			Out = "(empty range; the file has " + std::to_string(Total) + " lines)";
		else if(End < Total)
			Out += "(" + std::to_string(Total - End) + " more lines; use offset/limit)\n";
		AppendToolResult(Call.m_Id, "files", Out);
	}
	else if(Action == "write")
	{
		const std::string Resolved = AgentResolvePath(Path);
		std::string Err;
		if(!AgentWriteFile(Resolved, Content, Err))
		{
			AppendToolResult(Call.m_Id, "files", "error: " + Err + ": " + Resolved);
			return;
		}
		AppendToolResult(Call.m_Id, "files", "wrote " + std::to_string(Content.size()) + " bytes to " + Resolved);
	}
	else if(Action == "edit")
	{
		const std::string Resolved = AgentResolvePath(Path);
		if(Old.empty())
		{
			AppendToolResult(Call.m_Id, "files", "error: empty old text");
			return;
		}
		std::string Data, Err;
		if(!AgentReadFile(Resolved, Data, Err))
		{
			AppendToolResult(Call.m_Id, "files", "error: " + Err + ": " + Resolved);
			return;
		}
		const size_t Pos = Data.find(Old);
		if(Pos == std::string::npos)
		{
			AppendToolResult(Call.m_Id, "files", "error: old text not found in " + Resolved);
			return;
		}
		if(Data.find(Old, Pos + 1) != std::string::npos)
		{
			AppendToolResult(Call.m_Id, "files", "error: old text appears more than once; include more context");
			return;
		}
		Data.replace(Pos, Old.size(), New);
		if(!AgentWriteFile(Resolved, Data, Err))
		{
			AppendToolResult(Call.m_Id, "files", "error: " + Err + ": " + Resolved);
			return;
		}
		AppendToolResult(Call.m_Id, "files", "edited " + Resolved);
	}
	else if(Action == "list")
	{
		const std::string Resolved = Path.empty() ? AgentFilesRoot() : AgentResolvePath(Path);
		if(fs_is_file(Resolved.c_str()))
		{
			AppendToolResult(Call.m_Id, "files", Resolved + " (file)");
			return;
		}
		if(!fs_is_dir(Resolved.c_str()))
		{
			AppendToolResult(Call.m_Id, "files", "error: not a file or folder: " + Resolved);
			return;
		}
		struct SListCtx
		{
			std::string m_Out;
			int m_Count = 0;
		} Ctx;
		fs_listdir(Resolved.c_str(), [](const char *pName, int IsDir, int DirType, void *pUser) -> int {
			(void)DirType;
			auto *pCtx = (SListCtx *)pUser;
			pCtx->m_Out += pName;
			if(IsDir)
				pCtx->m_Out += "/";
			pCtx->m_Out += "\n";
			return ++pCtx->m_Count >= 500 ? 1 : 0;
		}, 0, &Ctx);
		if(Ctx.m_Out.empty())
			Ctx.m_Out = "(empty folder)";
		AppendToolResult(Call.m_Id, "files", Resolved + ":\n" + Ctx.m_Out);
	}
	else if(Action == "glob")
	{
		if(Pattern.empty())
		{
			AppendToolResult(Call.m_Id, "files", "error: missing pattern");
			return;
		}
		std::string Pat = Pattern;
		for(char &C : Pat)
			if(C == '\\')
				C = '/';
		bool AbsPat = false;
		std::string DrivePat;
		if(Pat.size() >= 2 && Pat[1] == ':')
		{
			AbsPat = true;
			DrivePat = Pat.substr(0, 2);
			Pat = Pat.substr(2);
		}
		else if(!Pat.empty() && Pat[0] == '/')
		{
			AbsPat = true;
			Pat = Pat.substr(1);
		}
		std::vector<std::string> vPat;
		size_t i = 0;
		while(i <= Pat.size())
		{
			size_t j = Pat.find('/', i);
			if(j == std::string::npos)
				j = Pat.size();
			const std::string Part = Pat.substr(i, j - i);
			if(!Part.empty())
				vPat.push_back(Part);
			if(j >= Pat.size())
				break;
			i = j + 1;
		}
		if(vPat.empty())
		{
			AppendToolResult(Call.m_Id, "files", "error: empty pattern");
			return;
		}
		const std::string BaseDir = AbsPat ? (DrivePat.empty() ? "/" : DrivePat + "/") : AgentFilesRoot();
		std::vector<std::string> vOut;
		AgentGlobRec(BaseDir, "", vPat, 0, vOut);
		if(vOut.empty())
			AppendToolResult(Call.m_Id, "files", "(no matches)");
		else
		{
			std::string Out;
			for(size_t k = 0; k < vOut.size(); ++k)
				Out += vOut[k] + "\n";
			AppendToolResult(Call.m_Id, "files", Out);
		}
	}
	else if(Action == "grep")
	{
		if(Pattern.empty())
		{
			AppendToolResult(Call.m_Id, "files", "error: missing pattern");
			return;
		}
		const std::string Root = Path.empty() ? AgentFilesRoot() : AgentResolvePath(Path);
		std::vector<std::string> vFiles;
		AgentCollectFiles(Root, "", vFiles, 5000);
		std::string Out;
		int Matches = 0;
		for(const std::string &Rel : vFiles)
		{
			if(Matches >= 200)
				break;
			if(!Include.empty())
			{
				const size_t Slash = Rel.find_last_of('/');
				const std::string Name = Slash == std::string::npos ? Rel : Rel.substr(Slash + 1);
				if(!AgentWildMatch(Include.c_str(), Name.c_str()))
					continue;
			}
			std::string Data, Err;
			if(!AgentReadFile(Root + "/" + Rel, Data, Err))
				continue;
			size_t Start = 0;
			int LineNo = 0;
			while(Start <= Data.size() && Matches < 200)
			{
				size_t NL = Data.find('\n', Start);
				if(NL == std::string::npos)
					NL = Data.size();
				std::string Line = Data.substr(Start, NL - Start);
				++LineNo;
				Start = NL + 1;
				if(!AgentContainsNoCase(Line, Pattern))
					continue;
				if(Line.size() > 220)
					Line = Line.substr(0, 220) + "...";
				Out += Rel + ":" + std::to_string(LineNo) + ": " + Line + "\n";
				++Matches;
			}
		}
		AppendToolResult(Call.m_Id, "files", Out.empty() ? std::string("(no matches)") : Out);
	}
	else
		AppendToolResult(Call.m_Id, "files", "error: unknown action '" + Action + "'");
}

void CAgent::RunConsoleTool(const SToolCall &Call)
{
	std::string Command;
	if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
	{
		if(pRoot->type == json_object)
			if(const json_value *pV = json_object_get(pRoot, "command"))
				if(const char *pS = json_string_get(pV))
					Command = pS;
		json_value_free(pRoot);
	}
	if(Command.empty())
	{
		AppendToolResult(Call.m_Id, "console", "error: missing command");
		return;
	}
	CGameConsole &GameConsole = GameClient()->m_GameConsole;
	std::vector<std::string> vLines;
	GameConsole.BeginOutputCapture();
	GameClient()->Console()->ExecuteLine(Command.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
	GameConsole.EndOutputCapture(vLines);
	const std::string Out = vLines.empty() ? std::string("(no console output)") : AgentJoinLines(vLines);
	AppendToolResult(Call.m_Id, "console", Out);
}

// ---- shell ---------------------------------------------------------------

#if defined(CONF_FAMILY_WINDOWS)
static std::wstring AgentUtf8ToWide(const std::string &S)
{
	if(S.empty())
		return std::wstring();
	const int Len = MultiByteToWideChar(CP_UTF8, 0, S.c_str(), (int)S.size(), nullptr, 0);
	std::wstring W((size_t)Len, 0);
	if(Len > 0)
		MultiByteToWideChar(CP_UTF8, 0, S.c_str(), (int)S.size(), &W[0], Len);
	return W;
}
#endif

// Runs one shell command on a worker thread. The command goes into a temp
// script file so no quoting layer can mangle it; stdout/stderr are collected
// by two reader threads; a timeout (or m_Kill) terminates the process.
void CAgent::ShellThreadProc(std::shared_ptr<SShellJob> pJob, std::string Shell, std::string Command, std::string Cwd, int TimeoutSec)
{
#if defined(CONF_FAMILY_WINDOWS)
	char aTempDir[MAX_PATH];
	if(GetTempPathA(MAX_PATH, aTempDir) == 0)
		str_copy(aTempDir, ".", sizeof(aTempDir));
	static std::atomic<int> s_TempCounter{0};
	const char *pExt = Shell == "powershell" ? "ps1" : (Shell == "bash" ? "sh" : "bat");
	char aScript[MAX_PATH];
	str_format(aScript, sizeof(aScript), "%skx_agent_%lu_%d.%s", aTempDir,
		(unsigned long)GetCurrentProcessId(), (int)s_TempCounter++, pExt);
	{
		FILE *pFile = fopen(aScript, "wb");
		if(!pFile)
		{
			std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
			pJob->m_Err = "cannot create the temporary script";
			pJob->m_ExitCode = -1;
			pJob->m_Done = true;
			return;
		}
		if(Shell == "powershell")
		{
			// PS 5.1 only reads a UTF-8 script with a BOM.
			fputc(0xEF, pFile);
			fputc(0xBB, pFile);
			fputc(0xBF, pFile);
		}
		fwrite(Command.data(), 1, Command.size(), pFile);
		fclose(pFile);
	}
	std::wstring CmdLine;
	const std::wstring ScriptW = AgentUtf8ToWide(aScript);
	if(Shell == "powershell")
		CmdLine = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + ScriptW + L"\"";
	else if(Shell == "bash")
		CmdLine = L"bash.exe \"" + ScriptW + L"\"";
	else
		CmdLine = L"cmd.exe /d /c \"" + ScriptW + L"\"";
	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE hOutR = nullptr, hOutW = nullptr, hErrR = nullptr, hErrW = nullptr;
	if(!CreatePipe(&hOutR, &hOutW, &sa, 0) || !CreatePipe(&hErrR, &hErrW, &sa, 0))
	{
		DeleteFileA(aScript);
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		pJob->m_Err = "cannot create the output pipes";
		pJob->m_ExitCode = -1;
		pJob->m_Done = true;
		return;
	}
	SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);
	HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = hNul;
	si.hStdOutput = hOutW;
	si.hStdError = hErrW;
	PROCESS_INFORMATION pi = {};
	const std::wstring CwdW = AgentUtf8ToWide(Cwd);
	const BOOL Started = CreateProcessW(nullptr, &CmdLine[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
		CwdW.empty() ? nullptr : CwdW.c_str(), &si, &pi);
	CloseHandle(hOutW);
	CloseHandle(hErrW);
	if(hNul != INVALID_HANDLE_VALUE)
		CloseHandle(hNul);
	if(!Started)
	{
		CloseHandle(hOutR);
		CloseHandle(hErrR);
		DeleteFileA(aScript);
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		pJob->m_Err = "CreateProcess failed (shell not found?)";
		pJob->m_ExitCode = -1;
		pJob->m_Done = true;
		return;
	}
	{
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		pJob->m_pProcess = (void *)pi.hProcess;
	}
	pJob->m_OutThread = std::thread([pJob, hOutR]() {
		char aBuf[4096];
		DWORD Read = 0;
		while(ReadFile(hOutR, aBuf, sizeof(aBuf), &Read, nullptr) && Read > 0)
		{
			std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
			if(pJob->m_Out.size() < 48 * 1024)
				pJob->m_Out.append(aBuf, Read);
		}
		CloseHandle(hOutR);
	});
	pJob->m_ErrThread = std::thread([pJob, hErrR]() {
		char aBuf[4096];
		DWORD Read = 0;
		while(ReadFile(hErrR, aBuf, sizeof(aBuf), &Read, nullptr) && Read > 0)
		{
			std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
			if(pJob->m_Err.size() < 48 * 1024)
				pJob->m_Err.append(aBuf, Read);
		}
		CloseHandle(hErrR);
	});
	const ULONGLONG StartTick = GetTickCount64();
	bool TimedOut = false;
	while(true)
	{
		if(WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0)
			break;
		if(pJob->m_Kill.load())
		{
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			break;
		}
		if((int)((GetTickCount64() - StartTick) / 1000) >= TimeoutSec)
		{
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			TimedOut = true;
			break;
		}
	}
	DWORD ExitCode = 0;
	GetExitCodeProcess(pi.hProcess, &ExitCode);
	{
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		pJob->m_pProcess = nullptr;
		pJob->m_ExitCode = (int)ExitCode;
		pJob->m_TimedOut = TimedOut;
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	DeleteFileA(aScript);
	pJob->m_Done = true;
#else
	(void)Shell;
	(void)Cwd;
	(void)TimeoutSec;
	FILE *pPipe = popen(Command.c_str(), "r");
	if(!pPipe)
	{
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		pJob->m_Err = "failed to start the shell";
		pJob->m_ExitCode = -1;
		pJob->m_Done = true;
		return;
	}
	char aBuf[4096];
	size_t Read = 0;
	while((Read = fread(aBuf, 1, sizeof(aBuf), pPipe)) > 0)
	{
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		if(pJob->m_Out.size() < 48 * 1024)
			pJob->m_Out.append(aBuf, Read);
	}
	const int Code = pclose(pPipe);
	{
		std::lock_guard<std::mutex> Lock(pJob->m_Mutex);
		pJob->m_ExitCode = Code;
	}
	pJob->m_Done = true;
#endif
}

// True when pExe is found in one of the PATH directories.
static bool AgentShellInPath(const char *pExe)
{
	if(!pExe || !pExe[0])
		return false;
	const char *pPath = getenv("PATH");
	if(!pPath)
		return false;
#ifdef CONF_FAMILY_WINDOWS
	const char Sep = ';';
#else
	const char Sep = ':';
#endif
	const char *p = pPath;
	while(*p)
	{
		const char *pEnd = p;
		while(*pEnd && *pEnd != Sep)
			++pEnd;
		const std::string Dir(p, pEnd - p);
		if(!Dir.empty())
		{
			const std::string Candidate = Dir + "/" + pExe;
			if(fs_is_file(Candidate.c_str()))
				return true;
		}
		if(!*pEnd)
			break;
		p = pEnd + 1;
	}
	return false;
}

static const char *AgentShellExeName(const std::string &Name)
{
	if(Name == "powershell")
		return "powershell.exe";
	if(Name == "cmd")
		return "cmd.exe";
	if(Name == "bash")
		return "bash.exe";
	return nullptr;
}

// Builds the shell tool JSON with the enum limited to the shells that really
// exist in PATH. Returns "" when no shell is available — the tool is then not
// advertised to the model at all.
static std::string AgentBuildShellToolJson()
{
	static const char *const s_apShells[] = {"powershell", "cmd", "bash"};
	std::string Enum, Names;
	for(const char *pShell : s_apShells)
	{
		if(!AgentShellInPath(AgentShellExeName(pShell)))
			continue;
		if(!Enum.empty())
		{
			Enum += ",";
			Names += ", ";
		}
		Enum += std::string(1, '"') + pShell + '"';
		Names += pShell;
	}
	if(Enum.empty())
		return "";
	std::string Json =
		R"JSON(,{"type":"function","function":{"name":"shell","description":"Run a shell command and return stdout, stderr and the exit code. It runs on a worker thread so the game never freezes; output is capped. The working directory defaults to the agent workdir.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"Command line to execute"},"shell":{"type":"string","enum":[)JSON";
	Json += Enum;
	Json += R"JSON(],"description":"Shell to use (optional; available: )JSON";
	Json += Names;
	Json += R"JSON()"},"cwd":{"type":"string","description":"Working directory (optional)"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30, max 300)"}},"required":["command"]}}})JSON";
	return Json;
}

void CAgent::RunShellTool(const SToolCall &Call)
{
	std::string Command, Shell, Cwd;
	int TimeoutSec = 30;
	if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
	{
		if(pRoot->type == json_object)
		{
			if(const json_value *pV = json_object_get(pRoot, "command"))
				if(const char *pS = json_string_get(pV))
					Command = pS;
			if(const json_value *pV = json_object_get(pRoot, "shell"))
				if(const char *pS = json_string_get(pV))
					Shell = pS;
			if(const json_value *pV = json_object_get(pRoot, "cwd"))
				if(const char *pS = json_string_get(pV))
					Cwd = pS;
			if(const json_value *pV = json_object_get(pRoot, "timeout"))
				TimeoutSec = json_int_get(pV);
		}
		json_value_free(pRoot);
	}
	if(Command.empty())
	{
		AppendToolResult(Call.m_Id, "shell", "error: missing command");
		return;
	}
	if(m_pShellJob && !m_pShellJob->m_Done.load())
	{
		AppendToolResult(Call.m_Id, "shell", "error: a shell command is already running");
		return;
	}
	if(Shell.empty())
	{
		switch(g_Config.m_KxExecShell)
		{
		case 0: Shell = "powershell"; break;
		case 2:
		case 3: Shell = "bash"; break;
		default: Shell = "cmd"; break;
		}
	}
	// Only shells that exist in PATH are usable: fall back to the first
	// available one, and hide the tool entirely when there is none.
	const char *pShellExe = AgentShellExeName(Shell);
	if(!pShellExe || !AgentShellInPath(pShellExe))
	{
		if(AgentShellInPath("powershell.exe"))
			Shell = "powershell";
		else if(AgentShellInPath("cmd.exe"))
			Shell = "cmd";
		else if(AgentShellInPath("bash.exe"))
			Shell = "bash";
		else
		{
			AppendToolResult(Call.m_Id, "shell", "error: no shell is available in PATH");
			return;
		}
	}
	const std::string WorkDir = Cwd.empty() ? AgentFilesRoot() : AgentResolvePath(Cwd);
	TimeoutSec = std::clamp(TimeoutSec, 1, 300);
	auto pJob = std::make_shared<SShellJob>();
	m_pShellJob = pJob;
	m_ToolRun = TOOLRUN_SHELL;
	pJob->m_Thread = std::thread(&CAgent::ShellThreadProc, pJob, Shell, Command, WorkDir, TimeoutSec);
}

void CAgent::PollShellJob()
{
	if(!m_pShellJob)
	{
		AppendToolResult(m_ActiveTool.m_Id, "shell", "error: shell job lost");
		m_ToolRun = TOOLRUN_NONE;
		return;
	}
	if(m_pShellJob->m_Thread.joinable())
		m_pShellJob->m_Thread.join();
	if(m_pShellJob->m_OutThread.joinable())
		m_pShellJob->m_OutThread.join();
	if(m_pShellJob->m_ErrThread.joinable())
		m_pShellJob->m_ErrThread.join();
	std::string Out, Err;
	int ExitCode = -1;
	bool TimedOut = false;
	{
		std::lock_guard<std::mutex> Lock(m_pShellJob->m_Mutex);
		Out = m_pShellJob->m_Out;
		Err = m_pShellJob->m_Err;
		ExitCode = m_pShellJob->m_ExitCode;
		TimedOut = m_pShellJob->m_TimedOut;
	}
	std::string Result = "exit_code: " + std::to_string(ExitCode);
	if(TimedOut)
		Result += " (timed out)";
	Result += "\n--- stdout ---\n" + (Out.empty() ? std::string("(empty)") : Out);
	Result += "\n--- stderr ---\n" + (Err.empty() ? std::string("(empty)") : Err);
	AppendToolResult(m_ActiveTool.m_Id, "shell", Result);
	m_pShellJob.reset();
	m_ToolRun = TOOLRUN_NONE;
}

// ---- background code_execution jobs -------------------------------------

// A background script that finished must release the tool capture even if the
// model never asks for its output, otherwise every later code_execution would
// report "busy". Called once per frame.
void CAgent::PollBgJob()
{
	if(m_ActiveBgJob < 0)
		return;
	CCodeExec &Exec = GameClient()->m_CodeExec;
	if(Exec.GetState() == CCodeExec::STATE_RUNNING)
		return;
	const std::vector<std::string> vOut = Exec.ToolCaptureFinish();
	const std::string Out = vOut.empty() ? std::string("(no output)") : AgentJoinLines(vOut);
	for(SBgJob &J : m_vBgJobs)
	{
		if(J.m_Id == m_ActiveBgJob)
		{
			J.m_Finished = true;
			J.m_Output = Out;
			break;
		}
	}
	m_ActiveBgJob = -1;
}

std::string CAgent::BgJobResult(int Id, bool Kill)
{
	CCodeExec &Exec = GameClient()->m_CodeExec;
	if(Id == m_ActiveBgJob)
	{
		if(Kill)
		{
			Exec.Stop();
			PollBgJob();
			for(SBgJob &J : m_vBgJobs)
			{
				if(J.m_Id != Id)
					continue;
				J.m_Killed = true;
				return "job " + std::to_string(Id) + ": killed\n" + J.m_Output;
			}
		}
		if(Exec.GetState() == CCodeExec::STATE_RUNNING)
		{
			const std::vector<std::string> vOut = Exec.ToolCapturePeek();
			return "job " + std::to_string(Id) + ": running\n" + (vOut.empty() ? std::string("(no output yet)") : AgentJoinLines(vOut));
		}
		PollBgJob();
	}
	for(const SBgJob &J : m_vBgJobs)
	{
		if(J.m_Id != Id)
			continue;
		std::string Head = "job " + std::to_string(Id) + (J.m_Killed ? ": killed" : (J.m_Finished ? ": finished" : ": running"));
		return Head + "\n" + J.m_Output;
	}
	return "error: no background job with id " + std::to_string(Id);
}

void CAgent::RunJobOutputTool(const SToolCall &Call)
{
	int Id = 0;
	if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
	{
		if(pRoot->type == json_object)
			if(const json_value *pV = json_object_get(pRoot, "id"))
				Id = json_int_get(pV);
		json_value_free(pRoot);
	}
	AppendToolResult(Call.m_Id, "job_output", BgJobResult(Id, false));
}

void CAgent::RunJobKillTool(const SToolCall &Call)
{
	int Id = 0;
	if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
	{
		if(pRoot->type == json_object)
			if(const json_value *pV = json_object_get(pRoot, "id"))
				Id = json_int_get(pV);
		json_value_free(pRoot);
	}
	AppendToolResult(Call.m_Id, "job_kill", BgJobResult(Id, true));
}

// Executes the next queued tool call. code_execution is asynchronous (the
// Lua coroutine runs over frames) — its result is collected in OnRender.
void CAgent::RunTool()
{
	const SToolCall Call = m_ActiveTool;
	CCodeExec &Exec = GameClient()->m_CodeExec;
	if(Call.m_Name == "documentation")
	{
		std::string Section;
		if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
		{
			if(pRoot->type == json_object)
				if(const json_value *pSection = json_object_get(pRoot, "section"))
					if(const char *pS = json_string_get(pSection))
						Section = pS;
			json_value_free(pRoot);
		}
		AppendToolResult(Call.m_Id, Call.m_Name.empty() ? "documentation" : Call.m_Name, AgentDocLookup(Section));
	}
	else if(Call.m_Name == "code_execution" || Call.m_Name.empty())
	{
		std::string Code;
		std::string File;
		bool Background = false;
		int JobId = 0;
		bool KillJob = false;
		if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
		{
			if(pRoot->type == json_object)
			{
				if(const json_value *pCode = json_object_get(pRoot, "code"))
					if(pCode->type == json_string)
						Code = json_string_get(pCode);
				if(const json_value *pFile = json_object_get(pRoot, "file"))
					if(pFile->type == json_string)
						File = json_string_get(pFile);
				if(const json_value *pBg = json_object_get(pRoot, "background"))
					Background = json_boolean_get(pBg) != 0;
				if(const json_value *pJob = json_object_get(pRoot, "job"))
					JobId = json_int_get(pJob);
				if(const json_value *pKill = json_object_get(pRoot, "kill"))
					KillJob = json_boolean_get(pKill) != 0;
			}
			json_value_free(pRoot);
		}
		// v6.14: execute a Lua file from the workdir instead of an inline script.
		if(JobId == 0 && Code.empty() && !File.empty())
		{
			const std::string Resolved = AgentResolvePath(File);
			std::string Data, Err;
			if(!AgentReadFile(Resolved, Data, Err))
			{
				AppendToolResult(Call.m_Id, "code_execution", "error: " + Err + ": " + Resolved);
				return;
			}
			Code = Data;
		}
		if(JobId > 0)
			AppendToolResult(Call.m_Id, "code_execution", BgJobResult(JobId, KillJob));
		else if(Code.empty())
			AppendToolResult(Call.m_Id, "code_execution", "error: no Lua code provided");
		// STATE_ERROR is sticky (never auto-resets) — a failed tool script must
		// not block later calls: only a RUNNING script or an active capture is
		// really busy. Execute() itself resets the state via Stop().
		else if(Exec.GetState() == CCodeExec::STATE_RUNNING || Exec.ToolCaptureActive())
			AppendToolResult(Call.m_Id, "code_execution", "error: Code Execution is busy, try again later");
		else if(Background)
		{
			// Start the script and return a job id at once: the tool loop keeps
			// going, job_output/job_kill read and stop the script by id.
			Exec.ToolCaptureStart();
			Exec.Execute(Code.c_str(), "agent_tool_bg");
			const int Id = m_NextBgJobId++;
			SBgJob Job;
			Job.m_Id = Id;
			m_vBgJobs.push_back(Job);
			while((int)m_vBgJobs.size() > 32)
				m_vBgJobs.erase(m_vBgJobs.begin());
			m_ActiveBgJob = Id;
			AppendToolResult(Call.m_Id, "code_execution", "started background job " + std::to_string(Id) +
				" — pass this id back in job to read its output, or set kill to stop it");
		}
		else
		{
			Exec.ToolCaptureStart();
			Exec.Execute(Code.c_str(), "agent_tool");
			m_ToolRun = TOOLRUN_LUA;
		}
	}
	else if(Call.m_Name == "shell")
		RunShellTool(Call);
	else if(Call.m_Name == "read" || Call.m_Name == "write" || Call.m_Name == "edit" ||
		Call.m_Name == "glob" || Call.m_Name == "grep" || Call.m_Name == "files")
		RunFilesTool(Call);
	else if(Call.m_Name == "console")
		RunConsoleTool(Call);
	else
		AppendToolResult(Call.m_Id, Call.m_Name, "error: unknown tool '" + Call.m_Name + "'");
}

void CAgent::AppendToolResult(const std::string &CallId, const std::string &Name, const std::string &Result)
{
	m_vApiMessages.push_back(AgentJsonOpen("role", "tool") + "," +
		AgentJsonField("tool_call_id", CallId.empty() ? "call_kx_" + std::to_string(m_RoundTrips) : CallId) + "," +
		AgentJsonField("name", Name) + "," + AgentJsonField("content", Result) + "}");
	// Attach the result to its display block ([ Code Execution > ]).
	for(SAgentMessage &M : m_vMessages)
	{
		if(M.m_Role == ROLE_TOOL && M.m_ToolCallId == (CallId.empty() ? "call_kx_" + std::to_string(m_RoundTrips) : CallId))
		{
			M.m_ToolResult = Result;
			break;
		}
	}
	SaveCurrentSession();
}

void CAgent::OnRender()
{
	RefreshModels();

	if(m_State == AGENT_REQUEST && m_pRequest)
	{
		ConsumeStream();
		if(m_pRequest->Done())
		{
			ConsumeStream();
			FinishRequest(m_pRequest->State());
		}
	}

	if(m_State != AGENT_TOOL_RUN)
		return;

	CCodeExec &Exec = GameClient()->m_CodeExec;
	if(m_ToolRun == TOOLRUN_LUA)
	{
		if(Exec.GetState() == CCodeExec::STATE_RUNNING)
			return; // the tool script is still executing co-operatively
		m_ToolRun = TOOLRUN_NONE;
		const std::vector<std::string> vOut = Exec.ToolCaptureFinish();
		AppendToolResult(m_ActiveTool.m_Id, "code_execution",
			vOut.empty() ? std::string("(no output)") : AgentJoinLines(vOut));
	}
	else if(m_ToolRun == TOOLRUN_SHELL)
	{
		if(m_pShellJob && !m_pShellJob->m_Done.load())
			return; // the worker thread is still running
		PollShellJob();
	}
	else
	{
		if(m_vPendingToolCalls.empty())
		{
			StartRequest();
			return;
		}
		m_ActiveTool = m_vPendingToolCalls.back();
		m_vPendingToolCalls.pop_back();
		RunTool();
	}

	// All queued tools consumed (none of them waits for a script)? Continue
	// the conversation with a new request.
	if(m_ToolRun == TOOLRUN_NONE && m_vPendingToolCalls.empty() && m_State == AGENT_TOOL_RUN)
		StartRequest();
}

// ============================================================================
// v6.13: ArrayAgent HUD — CClickGui render/interaction (state in CAgent).
// ============================================================================

static constexpr int KX_ARRAYAGENT_DEF_POS_X = 5000;
static constexpr int KX_ARRAYAGENT_DEF_POS_Y = 5000;
static const char *const g_AgentEffortOpts[] = {"Default", "Low", "High", "Max"};

// HH:MM | DD.MM.YYYY for a message timestamp.
static void AgentFormatTime(time_t Time, char *pBuf, int BufSize)
{
	time_t T = Time > 0 ? Time : std::time(nullptr);
	struct tm Local;
#ifdef _WIN32
	localtime_s(&Local, &T);
#else
	localtime_r(&T, &Local);
#endif
	str_format(pBuf, BufSize, "%02d:%02d | %02d.%02d.%04d", Local.tm_hour, Local.tm_min, Local.tm_mday, Local.tm_mon + 1, Local.tm_year + 1900);
}

// Greedy word wrap measured with the real font (pixel-accurate).
// ============================================================================
// v6.13: markdown rendering for agent messages — headers (#..######), bold,
// italic, strikethrough, inline code, fenced code blocks, bullet lists.
// ============================================================================
enum
{
	AGSEG_NORMAL = 0,
	AGSEG_BOLD,
	AGSEG_ITALIC, // also strikethrough (dimmed)
	AGSEG_CODE,
	AGSEG_CODEBLOCK,
	// v6.14: syntax highlighting styles for tool code/diff rendering.
	AGSEG_KW,
	AGSEG_STR,
	AGSEG_NUM,
	AGSEG_COM,
	AGSEG_FUNC,
	AGSEG_PUNCT,
	AGSEG_VAR,
};

struct SAgentSeg
{
	std::string m_Text;
	int m_Style = AGSEG_NORMAL;
};

// Block: 0 = paragraph, 1 = header (m_Level 1..6), 2 = fenced code line,
// m_Level < 0 = blank paragraph break.
struct SAgentBlock
{
	int m_Type = 0;
	int m_Level = 0;
	std::string m_Text;
	std::string m_Lang; // fenced code block language (type 2)
};

// One display line: pre-wrapped, styled segments + drawing size.
struct SAgentLine
{
	std::string m_Plain;
	std::vector<SAgentSeg> m_Segs;
	float m_Size = 0.0f;
	int m_Type = 0;
	int m_Level = 0;
	std::string m_Lang; // code block language (type 2/3)
};

// Inline markdown -> styled segments (**bold**, *italic*, ~~strike~~, `code`).
static void AgentParseInline(const std::string &Line, std::vector<SAgentSeg> &vOut)
{
	size_t i = 0;
	std::string Cur;
	auto Flush = [&]() {
		if(!Cur.empty())
		{
			vOut.push_back({Cur, AGSEG_NORMAL});
			Cur.clear();
		}
	};
	while(i < Line.size())
	{
		if(Line[i] == '`')
		{
			size_t End = Line.find('`', i + 1);
			if(End != std::string::npos)
			{
				Flush();
				vOut.push_back({Line.substr(i + 1, End - i - 1), AGSEG_CODE});
				i = End + 1;
				continue;
			}
		}
		if(Line.compare(i, 2, "**") == 0)
		{
			size_t End = Line.find("**", i + 2);
			if(End != std::string::npos)
			{
				Flush();
				vOut.push_back({Line.substr(i + 2, End - i - 2), AGSEG_BOLD});
				i = End + 2;
				continue;
			}
		}
		if(Line.compare(i, 2, "~~") == 0)
		{
			size_t End = Line.find("~~", i + 2);
			if(End != std::string::npos)
			{
				Flush();
				vOut.push_back({Line.substr(i + 2, End - i - 2), AGSEG_ITALIC});
				i = End + 2;
				continue;
			}
		}
		if(Line[i] == '*')
		{
			size_t End = Line.find('*', i + 1);
			if(End != std::string::npos && End > i + 1)
			{
				Flush();
				vOut.push_back({Line.substr(i + 1, End - i - 1), AGSEG_ITALIC});
				i = End + 1;
				continue;
			}
		}
		Cur += Line[i++];
	}
	Flush();
	if(vOut.empty())
		vOut.push_back({"", AGSEG_NORMAL});
}

// Message text -> block list (fence tracking, headers, bullets, paragraphs).
static void AgentParseBlocks(const char *pText, std::vector<SAgentBlock> &vOut)
{
	vOut.clear();
	if(!pText || !pText[0])
	{
		vOut.push_back(SAgentBlock{});
		return;
	}
	std::vector<std::string> vRaw;
	{
		std::string Cur;
		for(const char *p = pText; *p; ++p)
		{
			if(*p == '\n')
			{
				vRaw.push_back(Cur);
				Cur.clear();
			}
			else
				Cur += *p;
		}
		vRaw.push_back(Cur);
	}
	bool InFence = false;
	std::string FenceLang;
	for(const std::string &Line : vRaw)
	{
		if(Line.compare(0, 3, "```") == 0)
		{
			if(!InFence)
				FenceLang = AgentSessionTrim(Line.substr(3));
			InFence = !InFence;
			continue;
		}
		if(InFence)
		{
			// Each source line stays its own block (keeps code line structure).
			SAgentBlock B;
			B.m_Type = 2;
			B.m_Text = Line;
			B.m_Lang = FenceLang;
			vOut.push_back(std::move(B));
			continue;
		}
		size_t H = 0;
		while(H < Line.size() && Line[H] == '#')
			++H;
		if(H > 0 && H <= 6 && (H == Line.size() || Line[H] == ' '))
		{
			SAgentBlock B;
			B.m_Type = 1;
			B.m_Level = (int)H;
			B.m_Text = H < Line.size() ? Line.substr(H + 1) : "";
			vOut.push_back(std::move(B));
			continue;
		}
		bool AllSpace = true;
		for(char C : Line)
			if(C != ' ' && C != '\t')
			{
				AllSpace = false;
				break;
			}
		if(AllSpace)
		{
			// Blank line = paragraph break (rendered as a half-height spacer).
			SAgentBlock B;
			B.m_Level = -1;
			vOut.push_back(std::move(B));
			continue;
		}
		std::string T = Line;
		if(T.compare(0, 2, "- ") == 0 || T.compare(0, 2, "* ") == 0)
			T = "\xe2\x80\xa2 " + T.substr(2); // • bullet
		// Merge consecutive paragraph lines; blocks flow after headers/blanks.
		if(!vOut.empty() && vOut.back().m_Type == 0 && vOut.back().m_Level == 0)
			vOut.back().m_Text += "\n" + T;
		else
		{
			SAgentBlock B;
			B.m_Text = T;
			vOut.push_back(std::move(B));
		}
	}
}

// Splits a word wider than MaxW into width-fitting chunks (UTF-8 aware) so
// long tokens (Config.KxArrayListEnabled, URLs, code) wrap instead of
// overflowing the panel.
static std::vector<std::string> AgentChunkWord(const std::string &Word, float MaxW, float Size, ITextRender *pTextRender);

// Word-wrap styled words into display lines (plain-string measuring).
static void AgentWrapSegWords(const std::vector<SAgentSeg> &vWords, float MaxW, float Size, ITextRender *pTextRender, int Type, int Level, std::vector<SAgentLine> &vOut)
{
	std::vector<SAgentSeg> Cur;
	std::string CurPlain;
	auto FlushLine = [&]() {
		SAgentLine L;
		L.m_Plain = CurPlain;
		L.m_Segs = Cur;
		L.m_Size = Size;
		L.m_Type = Type;
		L.m_Level = Level;
		vOut.push_back(std::move(L));
		Cur.clear();
		CurPlain.clear();
	};
	for(const SAgentSeg &W : vWords)
	{
		// Long tokens wrap by character chunks instead of overflowing.
		for(const std::string &Chunk : AgentChunkWord(W.m_Text, MaxW, Size, pTextRender))
		{
			std::string Try = CurPlain.empty() ? Chunk : CurPlain + " " + Chunk;
			if(MaxW > 0.0f && !CurPlain.empty() && pTextRender->TextWidth(Size, Try.c_str()) > MaxW)
				FlushLine();
			if(!Cur.empty() && Cur.back().m_Style == W.m_Style)
				Cur.back().m_Text += " " + Chunk;
			else
			{
				SAgentSeg S = W;
				S.m_Text = Cur.empty() ? Chunk : " " + Chunk;
				Cur.push_back(std::move(S));
			}
			CurPlain = CurPlain.empty() ? Chunk : CurPlain + " " + Chunk;
		}
	}
	if(!CurPlain.empty())
		FlushLine();
}

// Full message -> wrapped display lines with styles/sizes.
static void AgentLayoutMessage(const char *pText, float MaxW, float FontSz, ITextRender *pTextRender, std::vector<SAgentLine> &vOut)
{
	vOut.clear();
	if(!pText || !pText[0])
		return; // empty text -> NO lines at all (no blank bubble)
	std::vector<SAgentBlock> vBlocks;
	AgentParseBlocks(pText, vBlocks);
	bool PrevCode = false;
	for(const SAgentBlock &B : vBlocks)
	{
		if(B.m_Type == 2 && !PrevCode)
		{
			// Fenced code block: language header strip above the first line.
			SAgentLine H;
			H.m_Type = 3;
			H.m_Size = FontSz; // one text line tall
			H.m_Lang = B.m_Lang;
			vOut.push_back(std::move(H));
		}
		PrevCode = (B.m_Type == 2);
		if(B.m_Level < 0) // blank paragraph spacer
		{
			SAgentLine L;
			L.m_Size = FontSz * 0.5f;
			vOut.push_back(std::move(L));
			continue;
		}
		float Size = FontSz;
		if(B.m_Type == 1)
			Size = FontSz * (B.m_Level == 1 ? 1.45f : (B.m_Level == 2 ? 1.3f : 1.2f));
			// A block can hold several source lines (consecutive paragraph
			// lines and list items are merged with a newline): lay every one
			// out as its own display line, so a newline inside a block never
			// ends up inside a wrapped word (that broke bullet lists).
			size_t SubStart = 0;
			while(SubStart <= B.m_Text.size())
			{
				size_t SubEnd = B.m_Text.find('\n', SubStart);
				if(SubEnd == std::string::npos)
					SubEnd = B.m_Text.size();
				const std::string SubLine = B.m_Text.substr(SubStart, SubEnd - SubStart);
				std::vector<SAgentSeg> vWords;
				if(B.m_Type == 2)
					vWords.push_back({SubLine, AGSEG_CODEBLOCK}); // whole code line = one word (keeps spacing)
				else
				{
					std::vector<SAgentSeg> vSegs;
					AgentParseInline(SubLine, vSegs);
					for(const SAgentSeg &Sg : vSegs)
					{
						std::string Cur;
						for(char C : Sg.m_Text)
						{
							if(C == ' ')
							{
								if(!Cur.empty())
								{
									vWords.push_back({Cur, Sg.m_Style});
									Cur.clear();
								}
							}
							else
								Cur += C;
						}
						if(!Cur.empty())
							vWords.push_back({Cur, Sg.m_Style});
					}
					if(vWords.empty())
						vWords.push_back({"", AGSEG_NORMAL});
				}
				size_t Before = vOut.size();
				AgentWrapSegWords(vWords, MaxW, Size, pTextRender, B.m_Type, B.m_Level, vOut);
				if(B.m_Type == 2)
					for(size_t k = Before; k < vOut.size(); ++k)
						vOut[k].m_Lang = B.m_Lang;
				if(vOut.size() == Before && !SubLine.empty())
				{
					SAgentLine L;
					L.m_Size = Size;
					L.m_Type = B.m_Type;
					L.m_Level = B.m_Level;
					L.m_Lang = B.m_Lang;
					vOut.push_back(std::move(L));
				}
				if(SubEnd >= B.m_Text.size())
					break;
				SubStart = SubEnd + 1;
			}
	}
	// Empty text -> NO filler line (an empty assistant bubble otherwise
	// renders as a blank row).
}

// Splits a word wider than MaxW into width-fitting chunks (UTF-8 aware) so
// long tokens (Config.KxArrayListEnabled, URLs, code) wrap instead of
// overflowing the panel.
static std::vector<std::string> AgentChunkWord(const std::string &Word, float MaxW, float Size, ITextRender *pTextRender)
{
	std::vector<std::string> Out;
	if(MaxW <= 0.0f || pTextRender->TextWidth(Size, Word.c_str()) <= MaxW)
	{
		Out.push_back(Word);
		return Out;
	}
	std::string Cur;
	const char *p = Word.c_str();
	while(*p)
	{
		const char *pNext = p;
		str_utf8_decode(&pNext);
		std::string Try = Cur + std::string(p, (size_t)(pNext - p));
		if(!Cur.empty() && pTextRender->TextWidth(Size, Try.c_str()) > MaxW)
		{
			Out.push_back(Cur);
			Cur = std::string(p, (size_t)(pNext - p));
		}
		else
			Cur = Try;
		p = pNext;
	}
	if(!Cur.empty())
		Out.push_back(Cur);
	return Out;
}

static void AgentWrapText(const char *pText, float MaxW, float FontSz, ITextRender *pTextRender, std::vector<std::string> &vOut){
	vOut.clear();
	if(!pText || !pText[0])
	{
		vOut.emplace_back("");
		return;
	}
	std::string Line;
	const char *p = pText;
	std::string CurWord;
	auto FlushWord = [&](bool ForceLine) {
		if(CurWord.empty())
			return;
		// Long tokens wrap by character chunks instead of overflowing.
		std::vector<std::string> vChunks = AgentChunkWord(CurWord, MaxW, FontSz, pTextRender);
		for(const std::string &Chunk : vChunks)
		{
			std::string Try = Line.empty() ? Chunk : Line + " " + Chunk;
			if(MaxW <= 0.0f || pTextRender->TextWidth(FontSz, Try.c_str()) <= MaxW || Line.empty())
			{
				if(!Line.empty() && ForceLine && pTextRender->TextWidth(FontSz, Try.c_str()) > MaxW)
				{
					vOut.push_back(Line);
					Line = Chunk;
				}
				else
					Line = Try;
			}
			else
			{
				vOut.push_back(Line);
				Line = Chunk;
			}
		}
		CurWord.clear();
	};
	while(*p)
	{
		if(*p == '\n')
		{
			FlushWord(false);
			vOut.push_back(Line);
			Line.clear();
			++p;
			continue;
		}
		if(*p == ' ')
		{
			FlushWord(true);
			++p;
			continue;
		}
		CurWord += *p++;
	}
	FlushWord(true);
	if(!Line.empty())
		vOut.push_back(Line);
	if(vOut.empty())
		vOut.emplace_back("");
}

bool CClickGui::ShouldRenderArrayAgent() const
{
	if(g_Config.m_KxArrayAgentEnabled == 0 && !m_AgentEditor)
		return false;
	// The chat is usable in the menu too (it does not need a game state).
	return true;
}

// Show Mode: 0=In Game, 1=In ClickGUI, 2=In Game+ClickGUI (two positions),
// 3=Both (one position for both).
static bool AgentShowModeGame() { return g_Config.m_KxArrayAgentShowMode == 0 || g_Config.m_KxArrayAgentShowMode == 2 || g_Config.m_KxArrayAgentShowMode == 3; }
static bool AgentShowModeGui() { return g_Config.m_KxArrayAgentShowMode == 1 || g_Config.m_KxArrayAgentShowMode == 2 || g_Config.m_KxArrayAgentShowMode == 3; }
static bool AgentShowModeDualPos() { return g_Config.m_KxArrayAgentShowMode == 2; }

bool CClickGui::ArrayAgentPositionModified() const
{
	const bool GuiPos = AgentShowModeDualPos() && m_AgentEditorGuiPass;
	return (GuiPos ? g_Config.m_KxArrayAgentPosXGui : g_Config.m_KxArrayAgentPosX) != KX_ARRAYAGENT_DEF_POS_X ||
		(GuiPos ? g_Config.m_KxArrayAgentPosYGui : g_Config.m_KxArrayAgentPosY) != KX_ARRAYAGENT_DEF_POS_Y;
}

void CClickGui::ResetArrayAgentPosition()
{
	if(AgentShowModeDualPos() && m_AgentEditorGuiPass)
	{
		g_Config.m_KxArrayAgentPosXGui = KX_ARRAYAGENT_DEF_POS_X;
		g_Config.m_KxArrayAgentPosYGui = KX_ARRAYAGENT_DEF_POS_Y;
	}
	else
	{
		g_Config.m_KxArrayAgentPosX = KX_ARRAYAGENT_DEF_POS_X;
		g_Config.m_KxArrayAgentPosY = KX_ARRAYAGENT_DEF_POS_Y;
	}
}

bool CClickGui::ArrayAgentHitTest(vec2 mousePos) const
{
	if(!m_AgentRectValid)
		return false;
	return mousePos.x >= m_AgentRectX && mousePos.x <= m_AgentRectX + m_AgentRectW &&
		mousePos.y >= m_AgentRectY && mousePos.y <= m_AgentRectY + m_AgentRectH;
}

bool CClickGui::ArrayAgentHeaderHitTest(vec2 mousePos) const
{
	if(!m_AgentRectValid)
		return false;
	const float S = (g_Config.m_KxUiSettingsOn != 0) ? (g_Config.m_KxArrayAgentSize / 10.0f) : 1.0f;
	const float HeaderH = 38.0f * S;
	return mousePos.x >= m_AgentRectX && mousePos.x <= m_AgentRectX + m_AgentRectW &&
		mousePos.y >= m_AgentRectY && mousePos.y <= m_AgentRectY + HeaderH;
}

bool CClickGui::ArrayAgentResizeHitTest(vec2 mousePos) const
{
	if(!m_AgentRectValid || !m_AgentEditor || g_Config.m_KxArrayAgentDynamicScale == 0)
		return false;
	const float Zone = 16.0f;
	return mousePos.x >= m_AgentRectX + m_AgentRectW - Zone &&
		mousePos.x <= m_AgentRectX + m_AgentRectW &&
		mousePos.y >= m_AgentRectY + m_AgentRectH - Zone &&
		mousePos.y <= m_AgentRectY + m_AgentRectH;
}

void CClickGui::UpdateArrayAgent(float dt)
{
	(void)dt;

	if(!m_AgentRectValid)
		return;

	const float ScreenW = VScreenWidth();
	const float ScreenH = VScreenHeight();
	if(ScreenW <= 0.0f || ScreenH <= 0.0f)
		return;

	const float SizeScale = (g_Config.m_KxUiSettingsOn != 0) ? (g_Config.m_KxArrayAgentSize / 10.0f) : 1.0f;
	const float S = std::max(0.01f, SizeScale);

	if(m_AgentResizing)
	{
		const int NW = (int)(m_AgentResizeStartW + (m_MousePos.x - m_AgentResizeStartMouse.x) / S + 0.5f);
		const int NH = (int)(m_AgentResizeStartH + (m_MousePos.y - m_AgentResizeStartMouse.y) / S + 0.5f);
		g_Config.m_KxArrayAgentWidth = std::clamp(NW, 200, 1200);
		g_Config.m_KxArrayAgentHeight = std::clamp(NH, 200, 1200);
		return;
	}
	if(!m_AgentDragging)
		return;

	const float Left = m_AgentDragStartPos.x + (m_MousePos.x - m_AgentDragStartMouse.x);
	const float Top = m_AgentDragStartPos.y + (m_MousePos.y - m_AgentDragStartMouse.y);
	// Show Mode = In Game+ClickGUI + ClickGUI editor → write the GUI position.
	int &rPosX = AgentShowModeDualPos() && m_AgentEditorGuiPass ? g_Config.m_KxArrayAgentPosXGui : g_Config.m_KxArrayAgentPosX;
	int &rPosY = AgentShowModeDualPos() && m_AgentEditorGuiPass ? g_Config.m_KxArrayAgentPosYGui : g_Config.m_KxArrayAgentPosY;
	rPosX = (int)((Left / ScreenW) * 10000.0f + (Left >= 0.0f ? 0.5f : -0.5f));
	rPosY = (int)((Top / ScreenH) * 10000.0f + (Top >= 0.0f ? 0.5f : -0.5f));
}

// Message side per role (X Anchor: 0 Auto, 1 Right, 2 Left, 3 Only Right, 4
// Only Left). Auto = the chat standard: user right, AI left.
// Strip leading/trailing blank lines (models often end with "\n" — the
// leftover spacer produced empty gaps between messages).
static std::string AgentTrimBlank(const std::string &S)
{
	size_t B = S.find_first_not_of(" \n\t\r");
	if(B == std::string::npos)
		return "";
	size_t E = S.find_last_not_of(" \n\t\r");
	return S.substr(B, E - B + 1);
}

// Tabs render as missing glyphs in the chat font — convert them to spaces.
static std::string AgentSanitize(const std::string &S)
{
	std::string Out;
	Out.reserve(S.size());
	for(char C : S)
	{
		if(C == '\t')
			Out += "    ";
		else if(C == '\r')
			continue;
		else
			Out += C;
	}
	return Out;
}

// v6.14: one line of the edit tool's diff view.
struct SToolDiffLine
{
	int m_Type = 0; // 0 = context, 1 = added, 2 = removed
	int m_OldNo = 0; // old file line number (0 = none)
	int m_NewNo = 0; // new file line number (0 = none)
	std::string m_Text; // raw line (highlighted at draw time)
};

// ============================================================================
// v6.14: tool widget presentation. Every tool gets a title, a resolved path,
// a language/shell chip and a typed body — the HUD never shows raw JSON.
// The code body and the edit diff are syntax highlighted per language.
// ============================================================================
enum
{
	AGTOOL_LANG_PLAIN = 0,
	AGTOOL_LANG_LUA,
	AGTOOL_LANG_SH,
	AGTOOL_LANG_C,
	AGTOOL_LANG_PY,
	AGTOOL_LANG_JS,
	AGTOOL_LANG_JSON,
};

// Color of one highlighted segment, keeping the caller's alpha.
static ColorRGBA AgentCodeSegColor(int Style, ColorRGBA Base)
{
	switch(Style)
	{
	case AGSEG_KW: return ColorRGBA(0xffc586c0, true).WithAlpha(Base.a);
	case AGSEG_STR: return ColorRGBA(0xffce9178, true).WithAlpha(Base.a);
	case AGSEG_NUM: return ColorRGBA(0xffb5cea8, true).WithAlpha(Base.a);
	case AGSEG_COM: return ColorRGBA(0xff6a9955, true).WithAlpha(Base.a);
	case AGSEG_FUNC: return ColorRGBA(0xffdcdcaa, true).WithAlpha(Base.a);
	case AGSEG_PUNCT: return ColorRGBA(0xffb0b0c0, true).WithAlpha(Base.a);
	}
	return Base;
}

static bool AgentIsPunct(char C)
{
	switch(C)
	{
	case '=': case '+': case '-': case '*': case '/': case '%': case '^': case '#':
	case '<': case '>': case '~': case '!': case '&': case '|': case '?': case ':':
	case ';': case ',': case '.': case '(': case ')': case '{': case '}': case '[': case ']':
		return true;
	}
	return false;
}

static std::string AgentBaseName(const std::string &Path)
{
	size_t Slash = Path.find_last_of('/');
	const size_t BSlash = Path.find_last_of((char)92);
	if(BSlash != std::string::npos && (Slash == std::string::npos || BSlash > Slash))
		Slash = BSlash;
	return Slash == std::string::npos ? Path : Path.substr(Slash + 1);
}

static std::string AgentLower(const std::string &In)
{
	std::string Out = In;
	for(size_t i = 0; i < Out.size(); ++i)
		Out[i] = (char)AgentToLower((unsigned char)Out[i]);
	return Out;
}

static int AgentLangFromPath(const std::string &Path)
{
	const std::string Name = AgentLower(AgentBaseName(Path));
	const size_t Dot = Name.find_last_of('.');
	if(Dot == std::string::npos)
		return AGTOOL_LANG_PLAIN;
	const std::string Ext = Name.substr(Dot + 1);
	if(Ext == "lua")
		return AGTOOL_LANG_LUA;
	if(Ext == "sh" || Ext == "ps1" || Ext == "bat" || Ext == "cmd")
		return AGTOOL_LANG_SH;
	if(Ext == "c" || Ext == "cc" || Ext == "cpp" || Ext == "cxx" || Ext == "h" || Ext == "hpp" || Ext == "hh")
		return AGTOOL_LANG_C;
	if(Ext == "py" || Ext == "pyw")
		return AGTOOL_LANG_PY;
	if(Ext == "js" || Ext == "mjs" || Ext == "ts" || Ext == "tsx" || Ext == "jsx")
		return AGTOOL_LANG_JS;
	if(Ext == "json")
		return AGTOOL_LANG_JSON;
	return AGTOOL_LANG_PLAIN;
}

static bool AgentIsKeyword(const std::string &Word, int Lang)
{
	static const char *const s_apLua[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", nullptr};
	static const char *const s_apSh[] = {"if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "case", "esac", "function", "return", "local", "export", "set", "unset", "shift", "until", "select", "time", nullptr};
	static const char *const s_apC[] = {"if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "struct", "class", "enum", "union", "typedef", "static", "const", "constexpr", "inline", "virtual", "override", "public", "private", "protected", "namespace", "using", "template", "typename", "new", "delete", "this", "nullptr", "true", "false", "void", "int", "float", "double", "char", "bool", "auto", "unsigned", "signed", "long", "short", "size_t", "extern", "volatile", "mutable", "friend", "operator", "sizeof", "decltype", "noexcept", "try", "catch", "throw", "goto", nullptr};
	static const char *const s_apPy[] = {"and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None", "nonlocal", "not", "or", "pass", "raise", "return", "True", "try", "while", "with", "yield", nullptr};
	static const char *const s_apJs[] = {"function", "var", "let", "const", "if", "else", "for", "while", "do", "return", "class", "new", "this", "true", "false", "null", "undefined", "typeof", "instanceof", "switch", "case", "break", "continue", "try", "catch", "finally", "throw", "async", "await", "import", "export", "from", "extends", "super", "static", "delete", "in", "of", "void", "yield", nullptr};
	if(Lang == AGTOOL_LANG_PLAIN || Lang == AGTOOL_LANG_JSON)
		return false;
	const char *const *ppList = s_apC;
	if(Lang == AGTOOL_LANG_LUA)
		ppList = s_apLua;
	else if(Lang == AGTOOL_LANG_SH)
		ppList = s_apSh;
	else if(Lang == AGTOOL_LANG_PY)
		ppList = s_apPy;
	else if(Lang == AGTOOL_LANG_JS)
		ppList = s_apJs;
	for(int i = 0; ppList[i]; ++i)
		if(Word == ppList[i])
			return true;
	return false;
}

static void AgentPushSeg(std::vector<SAgentSeg> &vSegs, const std::string &Text, int Style)
{
	if(Text.empty())
		return;
	if(!vSegs.empty() && vSegs.back().m_Style == Style)
		vSegs.back().m_Text += Text;
	else
		vSegs.push_back({Text, Style});
}

// One-line tokenizer: comments, strings, numbers, keywords, calls, punctuation.
static void AgentHighlightCodeLine(const std::string &Line, int Lang, std::vector<SAgentSeg> &vSegs)
{
	vSegs.clear();
	const size_t N = Line.size();
	size_t i = 0;
	while(i < N)
	{
		const char C = Line[i];
		// Comments.
		if(Lang == AGTOOL_LANG_LUA && C == '-' && i + 1 < N && Line[i + 1] == '-')
		{
			AgentPushSeg(vSegs, Line.substr(i), AGSEG_COM);
			break;
		}
		if((Lang == AGTOOL_LANG_C || Lang == AGTOOL_LANG_JS) && C == '/' && i + 1 < N && Line[i + 1] == '/')
		{
			AgentPushSeg(vSegs, Line.substr(i), AGSEG_COM);
			break;
		}
		if((Lang == AGTOOL_LANG_SH || Lang == AGTOOL_LANG_PY) && C == '#' && (i == 0 || Line[i - 1] == ' ' || Line[i - 1] == (char)9))
		{
			AgentPushSeg(vSegs, Line.substr(i), AGSEG_COM);
			break;
		}
		// Strings.
		if(C == '"' || C == (char)39 || ((Lang == AGTOOL_LANG_SH || Lang == AGTOOL_LANG_JS) && C == '`'))
		{
			size_t j = i + 1;
			while(j < N)
			{
				if(Line[j] == (char)92 && j + 1 < N)
				{
					j += 2;
					continue;
				}
				if(Line[j] == C)
				{
					++j;
					break;
				}
				++j;
			}
			AgentPushSeg(vSegs, Line.substr(i, j - i), AGSEG_STR);
			i = j;
			continue;
		}
		// Lua long string [[ ... ]].
		if(Lang == AGTOOL_LANG_LUA && C == '[' && i + 1 < N && Line[i + 1] == '[')
		{
			const size_t End = Line.find("]]", i + 2);
			const size_t j = End == std::string::npos ? N : End + 2;
			AgentPushSeg(vSegs, Line.substr(i, j - i), AGSEG_STR);
			i = j;
			continue;
		}
		// Numbers.
		if((C >= '0' && C <= '9') || (C == '.' && i + 1 < N && Line[i + 1] >= '0' && Line[i + 1] <= '9'))
		{
			size_t j = i + 1;
			while(j < N)
			{
				const char D = Line[j];
				if((D >= '0' && D <= '9') || (D >= 'a' && D <= 'f') || (D >= 'A' && D <= 'F') || D == 'x' || D == 'X' || D == '.' || D == '_')
					++j;
				else
					break;
			}
			AgentPushSeg(vSegs, Line.substr(i, j - i), AGSEG_NUM);
			i = j;
			continue;
		}
		// Identifiers / keywords / calls.
		if((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_')
		{
			size_t j = i + 1;
			while(j < N)
			{
				const char D = Line[j];
				if((D >= 'A' && D <= 'Z') || (D >= 'a' && D <= 'z') || (D >= '0' && D <= '9') || D == '_')
					++j;
				else
					break;
			}
			const std::string Word = Line.substr(i, j - i);
			int Style = AGSEG_NORMAL;
			if(AgentIsKeyword(Word, Lang))
				Style = AGSEG_KW;
			else
			{
				size_t k = j;
				while(k < N && (Line[k] == ' ' || Line[k] == (char)9))
					++k;
				if(k < N && Line[k] == '(')
					Style = AGSEG_FUNC;
			}
			AgentPushSeg(vSegs, Word, Style);
			i = j;
			continue;
		}
		// Punctuation / operators.
		if(AgentIsPunct(C))
		{
			AgentPushSeg(vSegs, std::string(1, C), AGSEG_PUNCT);
			++i;
			continue;
		}
		AgentPushSeg(vSegs, std::string(1, C), AGSEG_NORMAL);
		++i;
	}
	if(vSegs.empty())
		AgentPushSeg(vSegs, "", AGSEG_NORMAL);
}

// Splits code on real newlines and hard-wraps overlong lines by character
// chunks (word wrapping would destroy indentation and spacing).
static void AgentWrapCodeLines(const std::string &Code, float MaxW, float FontSz, ITextRender *pTextRender, std::vector<std::string> &vOut, int MaxLines)
{
	vOut.clear();
	const std::string S = AgentSanitize(Code);
	bool Truncated = false;
	size_t Start = 0;
	while(Start < S.size())
	{
		size_t NL = S.find((char)10, Start);
		if(NL == std::string::npos)
			NL = S.size();
		std::string Line = S.substr(Start, NL - Start);
		while(MaxW > 0.0f && Line.size() > 1 && pTextRender->TextWidth(FontSz, Line.c_str()) > MaxW)
		{
			size_t Cut = Line.size();
			while(Cut > 1 && pTextRender->TextWidth(FontSz, Line.substr(0, Cut).c_str()) > MaxW)
				--Cut;
			vOut.push_back(Line.substr(0, Cut));
			Line = Line.substr(Cut);
			if((int)vOut.size() >= MaxLines)
			{
				Truncated = true;
				break;
			}
		}
		if(Truncated)
			break;
		vOut.push_back(Line);
		if((int)vOut.size() >= MaxLines)
		{
			Truncated = true;
			break;
		}
		Start = NL + 1;
	}
	if(vOut.empty())
		vOut.emplace_back("");
	if(Truncated)
		vOut.push_back("...");
}

// Display name of the code block language.
static const char *AgentLangName(int Lang)
{
	switch(Lang)
	{
	case AGTOOL_LANG_LUA: return "Lua";
	case AGTOOL_LANG_SH: return "Shell";
	case AGTOOL_LANG_C: return "C++";
	case AGTOOL_LANG_PY: return "Python";
	case AGTOOL_LANG_JS: return "JavaScript";
	case AGTOOL_LANG_JSON: return "JSON";
	}
	return "Text";
}

// Markdown fence info string ("```lua" / "```cpp") -> language id.
static int AgentLangFromName(const std::string &Name)
{
	const std::string L = AgentLower(AgentSessionTrim(Name));
	if(L == "lua")
		return AGTOOL_LANG_LUA;
	if(L == "py" || L == "python" || L == "py3")
		return AGTOOL_LANG_PY;
	if(L == "js" || L == "javascript" || L == "ts" || L == "typescript" || L == "node")
		return AGTOOL_LANG_JS;
	if(L == "json")
		return AGTOOL_LANG_JSON;
	if(L == "sh" || L == "bash" || L == "shell" || L == "zsh" || L == "powershell" || L == "ps1" || L == "bat" || L == "cmd")
		return AGTOOL_LANG_SH;
	if(L == "c" || L == "cpp" || L == "c++" || L == "cc" || L == "cxx" || L == "h" || L == "hpp" || L == "hh")
		return AGTOOL_LANG_C;
	return AGTOOL_LANG_PLAIN;
}

// Read-tool output carries a "%5d<TAB>" line-number prefix per line. Parse it
// for the HUD gutter instead of drawing the raw prefix text (which made the
// numbers wander depending on the digit count).
static void AgentBuildNumberedLines(const std::string &Text, float MaxW, float FontSz, ITextRender *pTextRender,
	std::vector<std::string> &vOut, std::vector<int> &vNos, int MaxLines)
{
	vOut.clear();
	vNos.clear();
	const std::string S = Text;
	size_t Start = 0;
	while(Start < S.size())
	{
		size_t NL = S.find((char)10, Start);
		if(NL == std::string::npos)
			NL = S.size();
		std::string Line = S.substr(Start, NL - Start);
		if(!Line.empty() && Line.back() == (char)13)
			Line.pop_back();
		size_t i = 0;
		while(i < Line.size() && Line[i] == ' ')
			++i;
		size_t d = i;
		while(d < Line.size() && Line[d] >= '0' && Line[d] <= '9')
			++d;
		int No = 0;
		std::string Rest = Line;
		if(d > i && d < Line.size() && Line[d] == (char)9)
		{
			No = atoi(Line.substr(i, d - i).c_str());
			Rest = Line.substr(d + 1);
		}
		std::vector<std::string> vChunks;
		AgentWrapCodeLines(Rest, MaxW, FontSz, pTextRender, vChunks, MaxLines);
		for(const std::string &Chunk : vChunks)
		{
			vOut.push_back(Chunk);
			vNos.push_back(No);
			if((int)vOut.size() >= MaxLines)
				break;
		}
		if((int)vOut.size() >= MaxLines)
			break;
		Start = NL + 1;
	}
	if(vOut.empty())
	{
		vOut.emplace_back("");
		vNos.push_back(0);
	}
}

static void AgentSplitLines(const std::string &Text, std::vector<std::string> &vOut)
{
	vOut.clear();
	const std::string S = AgentSanitize(Text);
	size_t Start = 0;
	while(Start < S.size())
	{
		size_t NL = S.find((char)10, Start);
		if(NL == std::string::npos)
			NL = S.size();
		vOut.push_back(S.substr(Start, NL - Start));
		Start = NL + 1;
	}
}

// Builds the compact diff view of the edit tool: 2 context lines around the
// replaced block, removed lines (red, old numbers), added lines (green, new
// numbers). Works both before and after the edit was applied to the file.
static void AgentBuildDiff(const std::string &FileData, const std::string &OldStr, const std::string &NewStr, std::vector<SToolDiffLine> &vOut, int &Add, int &Del)
{
	vOut.clear();
	std::vector<std::string> vOld, vNew;
	AgentSplitLines(OldStr, vOld);
	AgentSplitLines(NewStr, vNew);
	Del = (int)vOld.size();
	Add = (int)vNew.size();

	int OldStartLine = 0; // 1-based line of the change in the old file
	int MatchedLines = 0;
	if(!FileData.empty())
	{
		size_t Pos = std::string::npos;
		size_t MatchLen = 0;
		if(!NewStr.empty())
		{
			Pos = FileData.find(NewStr);
			if(Pos != std::string::npos)
				MatchLen = NewStr.size();
		}
		if(Pos == std::string::npos && !OldStr.empty())
		{
			Pos = FileData.find(OldStr);
			if(Pos != std::string::npos)
				MatchLen = OldStr.size();
		}
		if(Pos != std::string::npos)
		{
			OldStartLine = 1;
			for(size_t k = 0; k < Pos; ++k)
				if(FileData[k] == (char)10)
					++OldStartLine;
			MatchedLines = 1;
			for(size_t k = 0; k < MatchLen; ++k)
				if(FileData[Pos + k] == (char)10)
					++MatchedLines;
			if(MatchLen > 0 && FileData[Pos + MatchLen - 1] == (char)10)
				--MatchedLines;
		}
	}

	std::vector<std::string> vAll;
	if(OldStartLine > 0)
		AgentSplitLines(FileData, vAll);

	// Context before.
	if(OldStartLine > 0)
	{
		for(int i = std::max(1, OldStartLine - 2); i < OldStartLine; ++i)
		{
			if(i > (int)vAll.size())
				break;
			SToolDiffLine D;
			D.m_Type = 0;
			D.m_OldNo = i;
			D.m_NewNo = i;
			D.m_Text = vAll[i - 1];
			vOut.push_back(D);
		}
	}
	for(size_t i = 0; i < vOld.size(); ++i)
	{
		SToolDiffLine D;
		D.m_Type = 2;
		D.m_OldNo = OldStartLine > 0 ? OldStartLine + (int)i : 0;
		D.m_Text = vOld[i];
		vOut.push_back(D);
	}
	for(size_t i = 0; i < vNew.size(); ++i)
	{
		SToolDiffLine D;
		D.m_Type = 1;
		D.m_NewNo = OldStartLine > 0 ? OldStartLine + (int)i : 0;
		D.m_Text = vNew[i];
		vOut.push_back(D);
	}
	// Context after.
	if(OldStartLine > 0)
	{
		const int AfterOld = OldStartLine + std::max(1, MatchedLines);
		const int AfterNew = OldStartLine + (int)vNew.size();
		for(int i = 0; i < 2; ++i)
		{
			const int OldNo = AfterOld + i;
			if(OldNo < 1 || OldNo > (int)vAll.size())
				break;
			SToolDiffLine D;
			D.m_Type = 0;
			D.m_OldNo = OldNo;
			D.m_NewNo = AfterNew + i;
			D.m_Text = vAll[OldNo - 1];
			vOut.push_back(D);
		}
	}
}

static bool AgentMessageRight(int Role, int XAnchor)
{
	const bool Right = (XAnchor == 1 || XAnchor == 3);
	const bool Left = (XAnchor == 2 || XAnchor == 4);
	if(Role == CAgent::ROLE_USER)
		return Right || (!Left && XAnchor == 0);
	// AI/tool/error messages mirror the user side.
	if(XAnchor == 3 || XAnchor == 4)
		return Right; // "Only Right"/"Only Left": everything on that side
	return !Right && XAnchor == 0 ? false : (XAnchor == 2);
}

// v6.13: cached message layout. Markdown parsing + word wrapping (which
// measures every word with TextRender()->TextWidth) is expensive; doing it
// for every message on every frame dropped the FPS hard as the chat grew
// (each tool block carries code + output). The layout is rebuilt only when
// the message text / tool payload or the layout parameters change.
struct SMessageLayout
{
	float Height = 0.0f;
	float MetaW = 0.0f;
	float ContentW = 0.0f; // widest display line (user bubble shrink-to-fit)
	std::vector<SAgentLine> vLines; // markdown display lines (styled)
	std::vector<std::string> vReasonLines; // Thought header (mode 1) or full text (mode 2)
	bool ReasonCollapsed = false; // Reasoning = Show (collapsible header)
	std::vector<std::string> vReasonContent; // full wrapped reasoning (mode 1)
	float ReasonBodyH = 0.0f; // full thought-body height (clipped by the anim)
	std::vector<std::string> vToolCodeLines; // Terminal: input (code/section)
	bool ToolHasInput = false; // input present (else no input box)
	std::vector<std::string> vToolOutLines;  // Terminal: result output
	// v6.14: per-tool presentation (no raw JSON in the HUD).
	std::string ToolTitle;  // e.g. "Edit array.cpp"
	std::string ToolPath;   // resolved path shown under the title
	std::string ToolChip;   // language / shell chip ("lua", "powershell")
	int ToolLang = 0;       // AGTOOL_LANG_* for the code/diff highlighter
	bool ToolInputDim = false; // input lines are a plain path, not code
	std::vector<SToolDiffLine> vToolDiff; // edit: diff lines (empty for other tools)
	int ToolAdd = 0; // edit: added lines
	int ToolDel = 0; // edit: removed lines
	// v6.14: read output is a numbered code block (line numbers parsed from the
	// tool result). Per-display-line file numbers.
	std::vector<int> vToolOutNos;
	bool ToolOutIsCode = false;
};

struct SAgentLayoutCache
{
	bool m_Valid = false;
	int m_Role = -1;
	float m_MaxW = -1.0f;
	float m_FontSz = -1.0f;
	int m_ReasoningMode = -1;
	std::string m_SrcText;
	std::string m_SrcReasoning;
	std::string m_SrcToolArgs;
	std::string m_SrcToolResult;
	SMessageLayout m_Layout;
};
static std::vector<SAgentLayoutCache> g_vAgentLayoutCache;

// Builds the expensive, cacheable part of a message layout (markdown lines,
// wrapped tool code/output, wrapped reasoning). Per-frame values (meta width,
// height, animations) are NOT part of this.
static void AgentBuildMessageLayout(const CAgent::SAgentMessage &Msg, float TextW, float FontSz, float S, int ReasoningMode, ITextRender *pTextRender, const CAgent *pAgent, SMessageLayout &L)
{
	if(Msg.m_Role == CAgent::ROLE_TOOL)
	{
		// v6.14: parse the arguments and present the tool per its kind — the
		// HUD never shows raw JSON.
			std::string Code, Section, Path, Pattern, OldStr, NewStr, Command, ShellName, Content, File;
			int JobId = 0;
		bool KillJob = false;
		bool BgJob = false;
		if(json_value *pRoot = JsonParse(Msg.m_ToolArgs.c_str(), Msg.m_ToolArgs.size()))
		{
			if(pRoot->type == json_object)
			{
				auto GetStr = [&](const char *pKey, std::string &Out) {
					const json_value *pV = json_object_get(pRoot, pKey);
					if(pV && pV->type == json_string)
						Out = json_string_get(pV);
				};
				GetStr("code", Code);
				GetStr("section", Section);
				GetStr("path", Path);
				GetStr("pattern", Pattern);
				GetStr("oldString", OldStr);
				GetStr("newString", NewStr);
					GetStr("command", Command);
					GetStr("shell", ShellName);
					GetStr("content", Content);
					GetStr("file", File);
				if(const json_value *pV = json_object_get(pRoot, "job"))
					if(pV->type == json_integer)
						JobId = json_int_get(pV);
				if(const json_value *pV = json_object_get(pRoot, "kill"))
					if(pV->type == json_boolean)
						KillJob = json_boolean_get(pV) != 0;
				if(const json_value *pV = json_object_get(pRoot, "background"))
					if(pV->type == json_boolean)
						BgJob = json_boolean_get(pV) != 0;
			}
			json_value_free(pRoot);
		}

		const std::string Name = Msg.m_ToolName;
		std::string ResolvedPath = Path;
		if(!Path.empty() && pAgent)
			ResolvedPath = pAgent->ToolResolvePath(Path);

		std::string Input;
		int InputLang = AGTOOL_LANG_PLAIN;
		bool HasInput = false;

			if(Name == "code_execution")
			{
				L.ToolTitle = "Code Execution";
				L.ToolChip = "lua";
				Input = Code;
				InputLang = AGTOOL_LANG_LUA;
				HasInput = !Code.empty();
				if(!HasInput && !File.empty())
				{
					// v6.14: execution of a file instead of an inline script — show
					// the file path and its contents.
					const std::string ResolvedFile = pAgent ? pAgent->ToolResolvePath(File) : File;
					L.ToolPath = ResolvedFile;
					std::string FileData, ReadErr;
					if(pAgent && AgentReadFile(ResolvedFile, FileData, ReadErr) && !FileData.empty())
					{
						Input = FileData;
						HasInput = true;
						const int FLang = AgentLangFromPath(File);
						InputLang = FLang != AGTOOL_LANG_PLAIN ? FLang : AGTOOL_LANG_LUA;
					}
				}
				if(!HasInput)
				{
				// Background-job control call (no code): describe it instead of
				// dumping raw JSON.
				if(JobId > 0 && KillJob)
					Input = "Kill background job #" + std::to_string(JobId);
				else if(JobId > 0)
					Input = "Read output of background job #" + std::to_string(JobId);
				else if(BgJob)
					Input = "Start background job";
				else
					Input = AgentTrimBlank(Msg.m_ToolArgs);
				InputLang = AGTOOL_LANG_PLAIN;
				HasInput = !Input.empty();
			}
		}
		else if(Name == "documentation")
		{
			L.ToolTitle = "Documentation";
			Input = Section.empty() ? AgentTrimBlank(Msg.m_ToolArgs) : Section;
			HasInput = !Input.empty();
		}
		else if(Name == "shell")
		{
			L.ToolTitle = "Shell";
			L.ToolChip = ShellName;
			Input = Command;
			InputLang = AGTOOL_LANG_SH;
			HasInput = !Command.empty();
		}
		else if(Name == "console")
		{
			L.ToolTitle = "Console";
			Input = Command;
			HasInput = !Command.empty();
		}
		else if(Name == "read")
		{
			L.ToolTitle = "Read " + AgentBaseName(Path);
			L.ToolPath = ResolvedPath;
			L.ToolLang = AgentLangFromPath(Path);
		}
		else if(Name == "write")
		{
			L.ToolTitle = "Write " + AgentBaseName(Path);
			L.ToolPath = ResolvedPath;
			L.ToolLang = AgentLangFromPath(Path);
			Input = Content;
			InputLang = L.ToolLang;
			HasInput = !Content.empty();
		}
		else if(Name == "edit")
		{
			L.ToolTitle = "Edit " + AgentBaseName(Path);
			L.ToolPath = ResolvedPath;
			L.ToolLang = AgentLangFromPath(Path);
			std::string FileData, ReadErr;
			if(pAgent && !ResolvedPath.empty())
				AgentReadFile(ResolvedPath, FileData, ReadErr);
			AgentBuildDiff(FileData, OldStr, NewStr, L.vToolDiff, L.ToolAdd, L.ToolDel);
		}
		else if(Name == "glob" || Name == "grep")
		{
			L.ToolTitle = (Name == "glob" ? "Glob " : "Grep ") + Pattern;
			L.ToolPath = Path.empty() ? std::string() : ResolvedPath;
		}
		else
		{
			L.ToolTitle = Name;
			Input = AgentTrimBlank(Msg.m_ToolArgs);
			HasInput = !Input.empty();
		}

			L.ToolHasInput = HasInput;
			if(HasInput)
				AgentWrapCodeLines(Input, TextW - 16.0f * S, FontSz, pTextRender, L.vToolCodeLines, 200);
			if(!L.vToolDiff.empty())
				L.vToolOutLines.clear();
			else if(Name == "read")
			{
				// The result carries the file's line numbers as text — parse them
				// into a proper gutter.
				AgentBuildNumberedLines(AgentTrimBlank(Msg.m_ToolResult), TextW - 16.0f * S, FontSz, pTextRender, L.vToolOutLines, L.vToolOutNos, 200);
				L.ToolOutIsCode = true;
			}
			else
				AgentWrapCodeLines(AgentTrimBlank(Msg.m_ToolResult), TextW - 16.0f * S, FontSz, pTextRender, L.vToolOutLines, 200);
				if(InputLang != AGTOOL_LANG_PLAIN)
					L.ToolLang = InputLang;
				// Language tag shown before the tool title (Read/Write/Edit).
				if((Name == "read" || Name == "write" || Name == "edit") && L.ToolLang != AGTOOL_LANG_PLAIN)
					L.ToolChip = AgentLangName(L.ToolLang);
		}
		else
		{
			AgentLayoutMessage(AgentSanitize(AgentTrimBlank(Msg.m_Text)).c_str(), TextW, FontSz, pTextRender, L.vLines);
		for(const SAgentLine &Ln : L.vLines)
			L.ContentW = std::max(L.ContentW, pTextRender->TextWidth(Ln.m_Size, Ln.m_Plain.c_str()));
		// Reasoning: 0 = hide, 1 = collapsible "Thought" block, 2 = always open.
		if(Msg.m_Role == CAgent::ROLE_ASSISTANT && !Msg.m_Reasoning.empty() && ReasoningMode > 0)
		{
			if(ReasoningMode == 1)
			{
				// "Thought" header; the body is revealed line-by-line by the
				// animation (smooth in BOTH directions).
				L.ReasonCollapsed = true;
				L.vReasonLines.push_back("Thought");
				AgentWrapText(AgentSanitize(AgentTrimBlank(Msg.m_Reasoning)).c_str(), TextW - 12.0f * S, FontSz, pTextRender, L.vReasonContent);
				L.ReasonBodyH = (float)L.vReasonContent.size() * (FontSz + 3.0f * S);
			}
			else
				AgentWrapText(AgentSanitize(AgentTrimBlank(Msg.m_Reasoning)).c_str(), TextW, FontSz, pTextRender, L.vReasonLines);
		}
	}
}

void CClickGui::RenderArrayAgent(float alpha, bool GuiPass)
{
	m_AgentRectValid = false;

	CAgent &Agent = GameClient()->m_Agent;

	// The editor renders in ONE pass only (the one whose position it edits).
	if(m_AgentEditor && m_AgentEditorGuiPass != GuiPass)
		return;

	const bool UiOn = g_Config.m_KxUiSettingsOn != 0;
	const float S = (UiOn ? g_Config.m_KxArrayAgentSize : 10) / 10.0f;
	const float W = (float)(UiOn ? g_Config.m_KxArrayAgentWidth : 480) * S;
	const float H = (float)(UiOn ? g_Config.m_KxArrayAgentHeight : 560) * S;
	if(W <= 0.0f || H <= 0.0f)
		return;

	const float Pad = 10.0f * S;
	const float FontSz = 15.0f * S;
	const float MetaSz = 11.0f * S;
	const float HeaderH = 38.0f * S;
	const float Indent = (UiOn ? (float)g_Config.m_KxArrayAgentIndent : 6.0f) * S;
	const float TextOp = 1.0f;
	const float MainOp = 1.0f;
	const float PanelOp = 1.0f;
	const bool DDNetStyle = UiOn && g_Config.m_KxArrayAgentStyle == 1;
	const bool ShowTime = UiOn && g_Config.m_KxArrayAgentShowTime != 0;
	const bool ShowWork = UiOn && g_Config.m_KxArrayAgentShowTimeWork != 0;
	const bool ShowUsage = UiOn && g_Config.m_KxArrayAgentShowUsage != 0;
	const bool ShowCopy = UiOn && g_Config.m_KxArrayAgentShowCopy != 0;
	const int ReasoningMode = UiOn ? g_Config.m_KxArrayAgentReasoning : 1;

	const float ScreenW = VScreenWidth();
	const float ScreenH = VScreenHeight();
	if(ScreenW <= 0.0f || ScreenH <= 0.0f)
		return;

	// Position: the ClickGUI pass uses its own position when Show Mode keeps
	// two positions; otherwise everything shares the game position.
	const bool DualPos = AgentShowModeDualPos();
	const int PosXVar = (GuiPass && DualPos) ? g_Config.m_KxArrayAgentPosXGui : g_Config.m_KxArrayAgentPosX;
	const int PosYVar = (GuiPass && DualPos) ? g_Config.m_KxArrayAgentPosYGui : g_Config.m_KxArrayAgentPosY;
	float Left = (PosXVar / 10000.0f) * ScreenW;
	float Top = (PosYVar / 10000.0f) * ScreenH;
	if(!m_AgentEditor)
	{
		// Clamp against the CURRENT visible height: a collapsed panel is only
		// HeaderH tall and must be draggable down to the screen bottom.
		const float VisibleH = HeaderH + (H - HeaderH) * m_AgentCollapseAnim;
		Left = std::clamp(Left, 0.0f, std::max(0.0f, ScreenW - W));
		Top = std::clamp(Top, 0.0f, std::max(0.0f, ScreenH - VisibleH));
	}
	// ClickGUI pass: fly in/out with the ClickGUI open animation, exactly like
	// the panels do (lerp from a random offscreen point by m_AnimProgress). The
	// editor pass stays exact so dragging maps 1:1 to the mouse, and "Both"
	// (one shared position) never opens/closes — the panel is always on screen.
	if(GuiPass && !m_AgentEditor && g_Config.m_KxArrayAgentShowMode != 3)
	{
		if(!m_AgentOffscreenValid)
		{
			const float Margin = 40.0f;
			const int Side = (int)(std::rand() % 4);
			if(Side == 0)
				m_AgentOffscreenPos = vec2(-W - Margin - (float)(std::rand() % 200), Top);
			else if(Side == 1)
				m_AgentOffscreenPos = vec2(ScreenW + Margin + (float)(std::rand() % 200), Top);
			else if(Side == 2)
				m_AgentOffscreenPos = vec2(Left, -H - Margin - (float)(std::rand() % 200));
			else
				m_AgentOffscreenPos = vec2(Left, ScreenH + Margin + (float)(std::rand() % 200));
			m_AgentOffscreenValid = true;
		}
		Left = m_AgentOffscreenPos.x + (Left - m_AgentOffscreenPos.x) * m_AnimProgress;
		Top = m_AgentOffscreenPos.y + (Top - m_AgentOffscreenPos.y) * m_AnimProgress;
	}

	m_AgentRectX = Left;
	m_AgentRectY = Top;
	m_AgentRectW = W;
	m_AgentRectH = HeaderH + (H - HeaderH) * m_AgentCollapseAnim; // follows the collapse animation
	m_AgentRectValid = true;

	// Colors are ARGB hex (alpha byte kept).
	unsigned TextHex = 0xffffffff;
	sscanf(g_Config.m_KxArrayAgentTextColor, "%8x", &TextHex);
	unsigned MainHex = 0xff9d4edd;
	sscanf(g_Config.m_KxArrayAgentMainColor, "%8x", &MainHex);
	unsigned PanelHex = 0xff161622;
	sscanf(g_Config.m_KxArrayAgentPanelColor, "%8x", &PanelHex);
	// v6.14: per-color mode (0 = Static, 1 = Rainbow). The rainbow hue is
	// time-based, so it keeps animating without a per-frame accumulator.
	const float HueT = (float)::time(nullptr);
	auto AgentRainbow = [&](int SpeedX10, int RbMode, int StepDeg, int Index) -> ColorRGBA {
		float R, G, B;
		const float Hue = HueT * (SpeedX10 / 10.0f) * 40.0f + (float)Index * (RbMode == 0 ? 0.0f : (float)StepDeg);
		KxHueToRgb(Hue, R, G, B);
		return ColorRGBA(R, G, B, 1.0f);
	};
	ColorRGBA MainCol = ColorRGBA(MainHex, true).WithAlpha(alpha * MainOp);
	if(g_Config.m_KxArrayAgentMainMode)
		MainCol = AgentRainbow(g_Config.m_KxArrayAgentMainRainbowSpeed, g_Config.m_KxArrayAgentMainRainbowMode, g_Config.m_KxArrayAgentMainRainbowStep, 0).WithAlpha(alpha * MainOp);
	const ColorRGBA AccentCol = DDNetStyle ? ColorRGBA(0.15f, 0.15f, 0.15f, alpha * MainOp) : MainCol;
	ColorRGBA TextCol = ColorRGBA(TextHex, true);
	if(g_Config.m_KxArrayAgentTextMode)
		TextCol = AgentRainbow(g_Config.m_KxArrayAgentTextRainbowSpeed, g_Config.m_KxArrayAgentTextRainbowMode, g_Config.m_KxArrayAgentTextRainbowStep, 0);
	ColorRGBA PanelCol = ColorRGBA(PanelHex, true).WithAlpha(alpha * 0.97f * PanelOp);
	if(g_Config.m_KxArrayAgentPanelMode)
		PanelCol = AgentRainbow(g_Config.m_KxArrayAgentPanelRainbowSpeed, g_Config.m_KxArrayAgentPanelRainbowMode, g_Config.m_KxArrayAgentPanelRainbowStep, 0).WithAlpha(alpha * 0.97f * PanelOp);

	// ClickGUI panel look, 1:1 with the GUI panels: shadow + COL_PANEL_BORDER
	// outline + panel bg + the faint sub-list tint under the body + accent
	// header with the collapse arrow + centered title. The body height follows
	// the collapse animation (GUI panels slide the same way).
	const float DrawH = HeaderH + (H - HeaderH) * m_AgentCollapseAnim;
	const ColorRGBA BorderCol = DDNetStyle
		? ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f * alpha * MainOp)
		: ColorRGBA(0xff2a2a3e, true).WithAlpha(alpha * MainOp);
	DrawRoundedRect(Left + 4.0f * S, Top + 4.0f * S, W, DrawH, 5.0f * S, IGraphics::CORNER_ALL,
		ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.15f * PanelOp));
	DrawRoundedRect(Left - 1.0f, Top - 1.0f, W + 2.0f, DrawH + 2.0f, 5.0f * S, IGraphics::CORNER_ALL, BorderCol);
	DrawRoundedRect(Left, Top, W, DrawH, 5.0f * S, IGraphics::CORNER_ALL, PanelCol);
	if(!m_AgentCollapsed)
		DrawRoundedRect(Left, Top + HeaderH, W, H - HeaderH, 0.0f, IGraphics::CORNER_NONE,
			ColorRGBA(0x2e000000, true).WithAlpha(alpha * 0.5f * PanelOp));
	DrawRoundedRect(Left, Top, W, HeaderH, 5.0f * S, IGraphics::CORNER_T, AccentCol);
	// Panel-style collapse arrow (▶ collapsed / ▼ open) on the left.
	{
		const char *pArrowStr = m_AgentCollapsed ? "\xe2\x96\xb6" : "\xe2\x96\xbc";
		DrawText(Left + 10.0f * S, Top + 9.0f * S, 18.0f * S, pArrowStr,
			ColorRGBA(0xffdddddd, true).WithAlpha(alpha));
	}
	const float TitleW = TextRender()->TextWidth(18.0f * S, "Agent");
	DrawText(Left + (W - TitleW) * 0.5f, Top + (HeaderH - 18.0f * S) * 0.5f + 1.0f * S, 18.0f * S, "Agent",
		ColorRGBA(0xffffffff, true).WithAlpha(alpha));

	// Fully collapsed (animation finished): header-only card. The rect covers
	// just the header so drag / hit tests keep working.
	if(m_AgentCollapseAnim <= 0.01f)
	{
		m_AgentRectX = Left;
		m_AgentRectY = Top;
		m_AgentRectW = W;
		m_AgentRectH = HeaderH;
		m_AgentRectValid = true;
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		return;
	}

	// Text draw helper (kept as a lambda for symmetric call sites).
	auto DrawAgentText = [&](float x, float y, const char *pText, float Sz, ColorRGBA Col) {
		DrawText(x, y, Sz, pText, Col);
	};

	// ---- bottom bar geometry: ONE field container (input + dropdowns + send) ----
	const float InputH = 26.0f * S;
	const float BtnH2 = 24.0f * S;
	const float FieldPad = 6.0f * S;
	const float FieldH = FieldPad + InputH + 4.0f * S + BtnH2 + FieldPad;
	const float BottomH = FieldH + Pad;
	const float FieldY = Top + H - BottomH;
	const float InputY = FieldY + FieldPad;
	const float Row2Y = InputY + InputH + 4.0f * S;
	const float SendW = 64.0f * S;

	// ---- messages ----
	const float MsgTop = Top + HeaderH + 4.0f * S;
	// The animated panel bottom (RenderPanel-style): the field container and
	// the messages clip against it while the collapse animation plays.
	const float PanelBottom = Top + HeaderH + (H - HeaderH) * m_AgentCollapseAnim;
	// v6.14: the session main page uses the full body height (no input bar).
	const bool MainPage = Agent.SessionListVisible();
	const float MsgBottom = MainPage ? (PanelBottom - Pad) : std::min(FieldY - 4.0f * S, PanelBottom);
	const float MsgAreaH = std::max(0.0f, MsgBottom - MsgTop);
	const float BubbleW = W - Pad * 2.0f - 14.0f * S;
	const int XA = g_Config.m_KxArrayAgentXAnchor;

	// ---- main page: session list ("+ New Session" + sessions + tip) ----
	if(MainPage)
	{
		const float ListX = Left + Pad;
		const float ListW = W - Pad * 2.0f;
		Graphics()->ClipEnable(Left, MsgTop, W, MsgAreaH);
		m_AgentSessionRects.clear();
		m_AgentSessionIds.clear();
		float sy = MsgTop + 4.0f * S;
		// "+ New Session" row. The plus is drawn with two bars (an icon, not
		// a text glyph).
		{
			const float RowH = 30.0f * S;
			const bool Hover = PointInRect(m_MousePos, ListX, sy, ListW, RowH);
			if(Hover)
				DrawRoundedRect(ListX, sy, ListW, RowH, 4.0f * S, IGraphics::CORNER_ALL,
					ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.07f));
			const float PlusX = ListX + 10.0f * S;
			const float PlusY = sy + (RowH - 12.0f * S) * 0.5f;
			DrawRoundedRect(PlusX + 4.5f * S, PlusY, 3.0f * S, 12.0f * S, 1.0f * S,
				IGraphics::CORNER_ALL, MainCol.WithAlpha(alpha));
			DrawRoundedRect(PlusX, PlusY + 4.5f * S, 12.0f * S, 3.0f * S, 1.0f * S,
				IGraphics::CORNER_ALL, MainCol.WithAlpha(alpha));
			DrawAgentText(PlusX + 20.0f * S, sy + (RowH - FontSz) * 0.5f, "New Session", FontSz,
				TextCol.WithAlpha(alpha));
			m_aAgentNewSessionRect[0] = ListX;
			m_aAgentNewSessionRect[1] = sy;
			m_aAgentNewSessionRect[2] = ListW;
			m_aAgentNewSessionRect[3] = RowH;
			sy += RowH + 4.0f * S;
		}
		DrawRoundedRect(ListX, sy, ListW, 1.0f * S, 0.0f, IGraphics::CORNER_NONE,
			ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.08f));
		sy += 8.0f * S;
		// Rows: the current unsaved temp session first, then saved sessions.
		struct SSessionRow
		{
			std::string m_Id;
			std::string m_Title;
			time_t m_Time;
			bool m_Temp;
		};
		std::vector<SSessionRow> vRows;
		if(Agent.CurrentSessionIsTemp() && Agent.HasMessages())
		{
			SSessionRow R;
			R.m_Id = "temp_current";
			R.m_Title = Agent.CurrentSessionTitle();
			R.m_Time = Agent.CurrentSessionTime();
			R.m_Temp = true;
			vRows.push_back(R);
		}
		for(const CAgent::SSessionInfo &SInfo : Agent.Sessions())
		{
			SSessionRow R;
			R.m_Id = SInfo.m_Id;
			R.m_Title = SInfo.m_Title;
			R.m_Time = SInfo.m_Time;
			R.m_Temp = false;
			vRows.push_back(R);
		}
		if(vRows.empty())
			DrawAgentText(ListX + 10.0f * S, sy + 4.0f * S, "No sessions yet.", FontSz,
				ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.8f));
		for(const SSessionRow &R : vRows)
		{
			const float RowH = 26.0f * S;
			const bool Hover = PointInRect(m_MousePos, ListX, sy, ListW, RowH);
			if(Hover)
				DrawRoundedRect(ListX, sy, ListW, RowH, 4.0f * S, IGraphics::CORNER_ALL,
					ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.07f));
			char aTime[64];
			{
				time_t T = R.m_Time > 0 ? R.m_Time : std::time(nullptr);
				struct tm Local;
#ifdef _WIN32
				localtime_s(&Local, &T);
#else
				localtime_r(&T, &Local);
#endif
				str_format(aTime, sizeof(aTime), "%02d:%02d %02d.%02d.%04d", Local.tm_hour, Local.tm_min,
					Local.tm_mday, Local.tm_mon + 1, Local.tm_year + 1900);
			}
			const float TimeW = TextRender()->TextWidth(MetaSz, aTime);
			std::string Label = R.m_Temp ? ("Temp Session: " + R.m_Title) : R.m_Title;
			if(Label.empty())
				Label = R.m_Temp ? std::string("Temp Session") : R.m_Id;
			const float MaxTitleW = std::max(20.0f * S, ListW - 28.0f * S - TimeW);
			if(TextRender()->TextWidth(FontSz, Label.c_str()) > MaxTitleW)
			{
				while(!Label.empty() && TextRender()->TextWidth(FontSz, (Label + "...").c_str()) > MaxTitleW)
					Label.pop_back();
				Label += "...";
			}
			DrawAgentText(ListX + 10.0f * S, sy + (RowH - FontSz) * 0.5f, Label.c_str(), FontSz,
				(R.m_Temp ? MainCol : TextCol).WithAlpha(alpha));
			DrawAgentText(ListX + ListW - TimeW - 8.0f * S, sy + (RowH - MetaSz) * 0.5f, aTime, MetaSz,
				ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.85f));
			m_AgentSessionRects.push_back(ListX);
			m_AgentSessionRects.push_back(sy);
			m_AgentSessionRects.push_back(ListW);
			m_AgentSessionRects.push_back(RowH);
			m_AgentSessionIds.push_back(R.m_Id);
			sy += RowH + 2.0f * S;
		}
		// Bottom tip.
		const std::string Tip = std::string("Tip: Write ") + (char)34 + "/exit" + (char)34 + " or " +
			(char)34 + "/sessions" + (char)34 + " to return to this page.";
		const float TipW = TextRender()->TextWidth(MetaSz, Tip.c_str());
		DrawAgentText(Left + (W - TipW) * 0.5f, MsgBottom - MetaSz - 4.0f * S, Tip.c_str(), MetaSz,
			ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.7f));
		Graphics()->ClipDisable();
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		return;
	}

	// Layout all messages: heights + wrapped lines. The heavy part (markdown
	// parsing, word wrapping and per-word TextWidth measuring) is cached per
	// message — see g_vAgentLayoutCache — and only rebuilt when the source
	// text or the layout parameters change. Re-measuring the whole chat every
	// frame tanked the FPS once dozens of tool blocks had accumulated.
	const int MsgCount = (int)Agent.m_vMessages.size();
	if((int)g_vAgentLayoutCache.size() != MsgCount)
		g_vAgentLayoutCache.resize(MsgCount);
	std::vector<SMessageLayout *> vLayouts;
	vLayouts.reserve(MsgCount);
	float ContentH = 0.0f;
	// Ease the per-message expand animations (tool blocks, reasoning).
	const float AnimDt = Client()->RenderFrameTime();
	for(CAgent::SAgentMessage &M : Agent.m_vMessages)
	{
		const float ExpandTarget = M.m_Expanded ? 1.0f : 0.0f;
		M.m_ExpandAnim += (ExpandTarget - M.m_ExpandAnim) * (1.0f - std::exp(-14.0f * AnimDt));
		if(std::fabs(ExpandTarget - M.m_ExpandAnim) < 0.01f)
			M.m_ExpandAnim = ExpandTarget;
		const float ReasonTarget = (M.m_ReasonOpen || ReasoningMode == 2) ? 1.0f : 0.0f;
		M.m_ReasonAnim += (ReasonTarget - M.m_ReasonAnim) * (1.0f - std::exp(-14.0f * AnimDt));
		if(std::fabs(ReasonTarget - M.m_ReasonAnim) < 0.01f)
			M.m_ReasonAnim = ReasonTarget;
	}
	for(int MsgBuildIdx = 0; MsgBuildIdx < MsgCount; ++MsgBuildIdx)
	{
		const CAgent::SAgentMessage &Msg = Agent.m_vMessages[MsgBuildIdx];
		SAgentLayoutCache &Cache = g_vAgentLayoutCache[MsgBuildIdx];
		const float TextW = BubbleW - (Msg.m_Role == CAgent::ROLE_USER ? 16.0f * S : 4.0f * S);
		const bool CacheHit = Cache.m_Valid && Cache.m_Role == (int)Msg.m_Role &&
			Cache.m_MaxW == TextW && Cache.m_FontSz == FontSz &&
			Cache.m_ReasoningMode == ReasoningMode &&
			Cache.m_SrcText == Msg.m_Text && Cache.m_SrcReasoning == Msg.m_Reasoning &&
			Cache.m_SrcToolArgs == Msg.m_ToolArgs && Cache.m_SrcToolResult == Msg.m_ToolResult;
		if(!CacheHit)
		{
			Cache.m_Layout = SMessageLayout{};
				AgentBuildMessageLayout(Msg, TextW, FontSz, S, ReasoningMode, TextRender(), &Agent, Cache.m_Layout);
			Cache.m_Valid = true;
			Cache.m_Role = (int)Msg.m_Role;
			Cache.m_MaxW = TextW;
			Cache.m_FontSz = FontSz;
			Cache.m_ReasoningMode = ReasoningMode;
			Cache.m_SrcText = Msg.m_Text;
			Cache.m_SrcReasoning = Msg.m_Reasoning;
			Cache.m_SrcToolArgs = Msg.m_ToolArgs;
			Cache.m_SrcToolResult = Msg.m_ToolResult;
		}
		SMessageLayout &L = Cache.m_Layout;
		float MetaW = 0.0f;
		// Stats appear only on the FINAL assistant answer (tokens>0 is set by
		// FinishRequest for the plain-answer finalize; intermediate tool-loop
		// turns and the live streaming message get no meta).
		if(Msg.m_Role == CAgent::ROLE_ASSISTANT && !Msg.m_Streaming && Msg.m_Tokens > 0)
		{
			std::string Meta;
			bool Has = false;
			if(ShowUsage && Msg.m_Tokens > 0)
			{
				Meta += std::to_string(Msg.m_Tokens) + " Tokens";
				Has = true;
			}
			if(ShowWork && Msg.m_WorkSec > 0)
			{
				Meta += Has ? " | " : "";
				Meta += std::to_string(Msg.m_WorkSec) + "s";
				Has = true;
			}
			if(ShowTime)
			{
				char aTime[64];
				AgentFormatTime(Msg.m_Time, aTime, sizeof(aTime));
				Meta += Has ? " | " : "";
				Meta += aTime;
				Has = true;
			}
			if(!Meta.empty())
				MetaW = TextRender()->TextWidth(MetaSz, Meta.c_str());
		}
		float TextH = 0.0f;
		for(const SAgentLine &Ln : L.vLines)
			TextH += Ln.m_Size + 3.0f * S;
		TextH += (float)L.vReasonLines.size() * (FontSz + 3.0f * S) + L.ReasonBodyH * Msg.m_ReasonAnim;
		if((!L.vReasonLines.empty() || L.ReasonBodyH > 0.0f) && Msg.m_Role == CAgent::ROLE_ASSISTANT)
			TextH += 4.0f * S; // small gap between reasoning and the answer
		if(Msg.m_Role == CAgent::ROLE_TOOL)
		{
			// Header: one line, or title + path for the file tools.
			const float HeaderH2 = L.ToolPath.empty() ? (FontSz + 10.0f * S) : (FontSz + MetaSz + 13.0f * S);
			TextH += HeaderH2;
			if(Msg.m_ExpandAnim > 0.01f)
			{
				float Inner = 4.0f * S;
				if(!L.vToolDiff.empty())
					Inner += (float)L.vToolDiff.size() * (FontSz + 1.0f * S) + 6.0f * S;
				else
				{
					if(L.ToolHasInput)
						Inner += (float)L.vToolCodeLines.size() * (FontSz + 2.0f * S) + 4.0f * S;
					Inner += (float)L.vToolOutLines.size() * (FontSz + 2.0f * S) + 6.0f * S;
				}
				TextH += (Inner + 2.0f * S) * Msg.m_ExpandAnim;
			}
		}
		else if(Msg.m_Role == CAgent::ROLE_USER || Msg.m_Role == CAgent::ROLE_ERROR)
			TextH += 8.0f * S; // bubble padding
		L.Height = TextH + (MetaW > 0.0f ? MetaSz + 3.0f * S : 0.0f) + 6.0f * S;
		L.MetaW = MetaW;
			vLayouts.push_back(&L);
			ContentH += L.Height;
	}
	ContentH += std::max(0, MsgCount - 1) * Indent;

	// Scroll: offset from the TOP of the content; sticks to the bottom while
	// reading (new messages keep the view pinned). The wheel drives a TARGET
	// and the actual offset eases toward it — smooth scrolling.
	const float MaxScroll = std::max(0.0f, ContentH - MsgAreaH);
	m_AgentScrollTarget = std::clamp(m_AgentScrollTarget, 0.0f, MaxScroll);
	if(m_AgentStickBottom)
		m_AgentScroll = m_AgentScrollTarget = MaxScroll;
	else
	{
		const float ScrollDt = Client()->RenderFrameTime();
		m_AgentScroll += (m_AgentScrollTarget - m_AgentScroll) * (1.0f - std::exp(-18.0f * ScrollDt));
		if(std::fabs(m_AgentScrollTarget - m_AgentScroll) < 0.5f)
			m_AgentScroll = m_AgentScrollTarget;
	}
	m_AgentScroll = std::clamp(m_AgentScroll, 0.0f, MaxScroll);
	m_AgentStickBottom = m_AgentScrollTarget >= MaxScroll - 1.0f;

	// Scrollbar drag: reposition from the mouse while held.
	const float ScrollTrackH = MsgAreaH;
	const float ScrollThumbH = std::max(30.0f * S, ScrollTrackH * std::min(1.0f, MsgAreaH / std::max(1.0f, ContentH)));
	if(m_AgentScrollbarDrag && MaxScroll > 0.0f)
	{
		const float TrackTop = MsgTop;
		const float Rel = (m_MousePos.y - TrackTop - m_AgentScrollbarGrabOffset) / std::max(1.0f, ScrollTrackH - ScrollThumbH);
		m_AgentScroll = std::clamp(Rel * MaxScroll, 0.0f, MaxScroll);
		m_AgentScrollTarget = m_AgentScroll;
		m_AgentStickBottom = m_AgentScroll >= MaxScroll - 1.0f;
	}
	float Y = MsgTop - m_AgentScroll;
	// Clip the messages to the message area (long text must not overflow into
	// the bottom bar / outside the panel — same as ClickGUI row clipping).
	Graphics()->ClipEnable(Left, MsgTop, W, MsgAreaH);
	m_AgentCopyRects.clear();
	m_AgentCopyHover = -1;
	m_AgentToggleRects.clear();
	m_AgentToggleMsgIdx.clear();
	m_AgentToggleKind.clear();
	int MsgIdx = 0;
	for(const CAgent::SAgentMessage &Msg : Agent.m_vMessages)
	{
		SMessageLayout &L = *vLayouts[MsgIdx];
		const bool Right = AgentMessageRight(Msg.m_Role, XA);
		// User bubbles shrink to fit their content (up to the full width).
		const bool UserMsg = Msg.m_Role == CAgent::ROLE_USER;
		const float MsgBubbleW = UserMsg ? std::min(BubbleW, std::max(L.ContentW + 16.0f * S, 30.0f * S)) : BubbleW;
		float BubbleX = Left + Pad;
		if(Right)
			BubbleX = Left + W - Pad - MsgBubbleW;

		const float TextX = BubbleX + (UserMsg ? 8.0f * S : 2.0f * S);
		const float TextY = Y + 4.0f * S;

		ColorRGBA MsgCol = TextCol.WithAlpha(alpha * TextOp);
		if(Msg.m_Role == CAgent::ROLE_ERROR)
			MsgCol = ColorRGBA(0xffc43d3d, true).WithAlpha(alpha * TextOp);

		// User messages get a rounded bubble (LIGHTER than the panel bg);
		// assistant text is plain; errors are red (per the mockup).
		if(UserMsg || Msg.m_Role == CAgent::ROLE_ERROR)
		{
			const float BubbleH = L.Height - 6.0f * S;
			ColorRGBA BubbleCol = UserMsg
				? (DDNetStyle ? ColorRGBA(0.25f, 0.25f, 0.28f, 0.9f * alpha * PanelOp) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f * alpha * PanelOp))
				: ColorRGBA(0xc4, 0x3d, 0x3d, 0.18f * alpha * PanelOp);
			DrawRoundedRect(BubbleX, Y, MsgBubbleW, BubbleH, 5.0f * S, IGraphics::CORNER_ALL, BubbleCol);
		}

		float ly = TextY;
		// "Thought" block (reasoning): grey header + chevron, body revealed by
		// the animation behind a thin left border line — chat-standard style.
		ColorRGBA ReasonCol = ColorRGBA(0xff909090, true).WithAlpha(alpha * TextOp);
		ColorRGBA ThoughtHeadCol = ColorRGBA(0xff888888, true).WithAlpha(alpha * TextOp);
		if(L.ReasonCollapsed)
		{
			// Header: "Thought" [+ duration] + chevron. Clickable in both states.
			const bool Hover = PointInRect(m_MousePos, Left + Pad, ly - 1.0f * S, BubbleW, FontSz + 4.0f * S);
			m_AgentToggleRects.push_back(Left + Pad);
			m_AgentToggleRects.push_back(ly - 1.0f * S);
			m_AgentToggleRects.push_back(BubbleW);
			m_AgentToggleRects.push_back(FontSz + 4.0f * S);
			m_AgentToggleMsgIdx.push_back(MsgIdx);
			m_AgentToggleKind.push_back(1);
			DrawAgentText(TextX, ly, "Thought", FontSz * 0.9f, Hover ? ColorRGBA(0xffcccccc, true).WithAlpha(alpha * TextOp) : ThoughtHeadCol);
			float hx = TextX + TextRender()->TextWidth(FontSz * 0.9f, "Thought");
			if(Msg.m_WorkSec > 0)
			{
				std::string Dur = " \xc2\xb7 " + std::to_string(Msg.m_WorkSec) + "s";
				DrawAgentText(hx, ly, Dur.c_str(), FontSz * 0.9f, ColorRGBA(0xff666666, true).WithAlpha(alpha * TextOp));
				hx += TextRender()->TextWidth(FontSz * 0.9f, Dur.c_str());
			}
			// Expandable-style arrow (same glyphs as the GUI expandables).
			DrawAgentText(hx + 6.0f * S, ly, Msg.m_ReasonOpen ? "\xe2\x96\xbc" : "\xe2\x96\xb6", FontSz * 0.8f, ThoughtHeadCol);
			ly += FontSz + 3.0f * S;
			// Body: thin left border + ALL lines, CLIPPED by the animated height
			// (slide reveal like the GUI panel bodies, not per-line pops).
			const float ClipH = L.ReasonBodyH * Msg.m_ReasonAnim;
			if(ClipH > 0.5f)
			{
				const float CY0 = std::max(ly, MsgTop);
				const float CY1 = std::min(ly + ClipH, MsgBottom);
				if(CY1 > CY0)
				{
					Graphics()->ClipEnable(Left, CY0, W, CY1 - CY0);
					DrawRoundedRect(TextX + 4.0f * S, ly, 2.0f * S, L.ReasonBodyH - 2.0f * S, 1.0f * S, IGraphics::CORNER_ALL,
						ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.12f * PanelOp));
					const float BodyX = TextX + 12.0f * S;
					float by = ly;
					for(const std::string &Line : L.vReasonContent)
					{
						DrawAgentText(BodyX, by, Line.c_str(), FontSz, ReasonCol);
						by += FontSz + 3.0f * S;
					}
					Graphics()->ClipDisable();
					// ClipEnable/ClipDisable is a single global state (not a
					// stack) — restore the message-area clip we are nested in.
					Graphics()->ClipEnable(Left, MsgTop, W, MsgAreaH);
				}
			}
			ly += L.ReasonBodyH * Msg.m_ReasonAnim;
		}
		else
		{
			for(size_t ri = 0; ri < L.vReasonLines.size(); ++ri)
			{
				DrawAgentText(TextX, ly, L.vReasonLines[ri].c_str(), FontSz, ReasonCol);
				ly += FontSz + 3.0f * S;
			}
		}
		if((!L.vReasonLines.empty() || L.ReasonBodyH > 0.0f) && Msg.m_Role == CAgent::ROLE_ASSISTANT)
			ly += 4.0f * S; // small gap between reasoning and the answer
			// Tool widget: header (icon + title + path + chip/stats + chevron) and
			// a typed body (highlighted code, diff, output) revealed by the
			// expand animation. No raw JSON is ever drawn.
			if(Msg.m_Role == CAgent::ROLE_TOOL)
			{
				const float HeaderH2 = L.ToolPath.empty() ? (FontSz + 10.0f * S) : (FontSz + MetaSz + 13.0f * S);
				const float BoxW = BubbleW;
				const bool Hover = PointInRect(m_MousePos, BubbleX, ly, BoxW, HeaderH2);
				float Inner = 4.0f * S;
				if(!L.vToolDiff.empty())
					Inner += (float)L.vToolDiff.size() * (FontSz + 1.0f * S) + 6.0f * S;
				else
				{
					if(L.ToolHasInput)
						Inner += (float)L.vToolCodeLines.size() * (FontSz + 2.0f * S) + 4.0f * S;
					Inner += (float)L.vToolOutLines.size() * (FontSz + 2.0f * S) + 6.0f * S;
				}
				const float BodyFullH = Inner + 2.0f * S;
				const float WidgetH = HeaderH2 + BodyFullH * Msg.m_ExpandAnim;
				DrawRoundedRect(BubbleX, ly, BoxW, WidgetH, 6.0f * S, IGraphics::CORNER_ALL,
					ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.05f * PanelOp));
				DrawRoundedOutline(BubbleX + 0.5f, ly + 0.5f, BoxW - 1.0f, WidgetH - 1.0f, 5.0f * S,
					ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.10f * PanelOp), true, true);
				// Icon (primitives only): pencil for edit, magnifier for glob/grep,
				// document sheet for the file tools, terminal ">_" otherwise.
				const float IconX = BubbleX + 8.0f * S;
				const float IconY = ly + 6.0f * S;
				const bool EditIcon = Msg.m_ToolName == "edit";
				const bool SearchIcon = Msg.m_ToolName == "glob" || Msg.m_ToolName == "grep";
				const bool DocIcon = Msg.m_ToolName == "documentation" || Msg.m_ToolName == "read" || Msg.m_ToolName == "write";
				DrawRoundedOutline(IconX, IconY, 13.0f * S, 11.0f * S, 2.0f * S, ThoughtHeadCol, true, true);
				auto IconSeg = [&](float Ax, float Ay, float Bx, float By, float HalfW) {
					vec2 D = vec2(Bx - Ax, By - Ay);
					const float Len = length(D);
					if(Len < 0.001f)
						return;
					D /= Len;
					const vec2 P = vec2(-D.y, D.x) * HalfW;
					IGraphics::CFreeformItem Q(Ax - P.x, Ay - P.y, Ax + P.x, Ay + P.y,
						Bx - P.x, By - P.y, Bx + P.x, By + P.y);
					Graphics()->QuadsDrawFreeform(&Q, 1);
				};
				if(EditIcon)
				{
					Graphics()->SetColor(ThoughtHeadCol.r, ThoughtHeadCol.g, ThoughtHeadCol.b, ThoughtHeadCol.a);
					Graphics()->QuadsBegin();
					IconSeg(IconX + 2.6f * S, IconY + 8.4f * S, IconX + 10.2f * S, IconY + 1.4f * S, 1.1f * S);
					IconSeg(IconX + 1.6f * S, IconY + 9.4f * S, IconX + 3.4f * S, IconY + 7.6f * S, 0.8f * S);
					Graphics()->QuadsEnd();
				}
				else if(SearchIcon)
				{
					DrawRoundedOutline(IconX + 1.4f * S, IconY + 1.0f * S, 7.0f * S, 7.0f * S, 3.5f * S, ThoughtHeadCol, true, true);
					Graphics()->SetColor(ThoughtHeadCol.r, ThoughtHeadCol.g, ThoughtHeadCol.b, ThoughtHeadCol.a);
					Graphics()->QuadsBegin();
					IconSeg(IconX + 7.6f * S, IconY + 7.6f * S, IconX + 11.0f * S, IconY + 10.4f * S, 0.9f * S);
					Graphics()->QuadsEnd();
				}
				else if(DocIcon)
				{
					for(int li = 0; li < 3; ++li)
						DrawRoundedRect(IconX + 2.5f * S, IconY + 2.5f * S + li * 3.0f * S, 8.0f * S, 1.2f * S, 0.6f * S,
							IGraphics::CORNER_ALL, ThoughtHeadCol);
				}
				else
				{
					const float GlyphSz = FontSz * 0.55f;
					const float BarW = 3.5f * S;
					const float BarH = 1.2f * S;
					const float Gap = 0.6f * S;
					const float GlyphW = TextRender()->TextWidth(GlyphSz, ">");
					const float GlyphH = TextRender()->TextBoundingBox(GlyphSz, ">").m_H;
					const float TotalW = GlyphW + Gap + BarW;
					const float GX = IconX + (13.0f * S - TotalW) * 0.5f;
					const float GY = IconY + (11.0f * S - GlyphH) * 0.5f;
					DrawAgentText(GX, GY, ">", GlyphSz, ThoughtHeadCol);
					DrawRoundedRect(GX + GlyphW + Gap, GY + GlyphH - BarH, BarW, BarH, 0.6f * S,
						IGraphics::CORNER_ALL, ThoughtHeadCol);
				}
					// Language tag (prefix) + title (+ resolved path line).
					float hx = IconX + 13.0f * S + 6.0f * S;
					const ColorRGBA TitleCol = Hover ? ColorRGBA(0xffcccccc, true).WithAlpha(alpha * TextOp) : ThoughtHeadCol;
					if(!L.ToolChip.empty())
					{
						const float ChipW = TextRender()->TextWidth(MetaSz, L.ToolChip.c_str()) + 8.0f * S;
						const float ChipH = MetaSz + 5.0f * S;
						DrawRoundedRect(hx, ly + 3.0f * S, ChipW, ChipH, 3.0f * S, IGraphics::CORNER_ALL,
							ColorRGBA(0.4f, 0.6f, 1.0f, alpha * 0.16f * PanelOp));
						DrawAgentText(hx + 4.0f * S, ly + 4.0f * S, L.ToolChip.c_str(), MetaSz,
							ColorRGBA(0xff9ecbff, true).WithAlpha(alpha * TextOp));
						hx += ChipW + 6.0f * S;
					}
					if(L.ToolPath.empty())
						DrawAgentText(hx, ly + 5.0f * S, L.ToolTitle.c_str(), FontSz * 0.9f, TitleCol);
					else
					{
						DrawAgentText(hx, ly + 3.0f * S, L.ToolTitle.c_str(), FontSz * 0.9f, TitleCol);
						DrawAgentText(hx, ly + 4.0f * S + FontSz * 0.9f, L.ToolPath.c_str(), MetaSz,
							ColorRGBA(0xff777788, true).WithAlpha(alpha * TextOp));
					}
				// Right side: chevron, edit stats, chip.
				float rx = BubbleX + BoxW - 20.0f * S;
				const char *pChevron = Msg.m_Expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6";
				DrawAgentText(rx, ly + 5.0f * S, pChevron, FontSz * 0.8f, ThoughtHeadCol);
				rx -= TextRender()->TextWidth(FontSz * 0.8f, pChevron) + 4.0f * S;
				if(Msg.m_ToolName == "edit")
				{
					char aStat[32];
					if(L.ToolDel > 0)
					{
						str_format(aStat, sizeof(aStat), "-%d", L.ToolDel);
						const float w = TextRender()->TextWidth(MetaSz, aStat);
						rx -= w;
						DrawAgentText(rx, ly + 6.0f * S, aStat, MetaSz, ColorRGBA(0xffe05561, true).WithAlpha(alpha * TextOp));
						rx -= 5.0f * S;
					}
					if(L.ToolAdd > 0)
					{
						str_format(aStat, sizeof(aStat), "+%d", L.ToolAdd);
						const float w = TextRender()->TextWidth(MetaSz, aStat);
						rx -= w;
						DrawAgentText(rx, ly + 6.0f * S, aStat, MetaSz, ColorRGBA(0xff4ec9b0, true).WithAlpha(alpha * TextOp));
						rx -= 5.0f * S;
					}
				}
					m_AgentToggleRects.push_back(BubbleX);
				m_AgentToggleRects.push_back(ly);
				m_AgentToggleRects.push_back(BoxW);
				m_AgentToggleRects.push_back(HeaderH2);
				m_AgentToggleMsgIdx.push_back(MsgIdx);
				m_AgentToggleKind.push_back(0);
				ly += HeaderH2;
				if(Msg.m_ExpandAnim > 0.01f)
				{
					const float TotalH = BodyFullH * Msg.m_ExpandAnim;
					// Inner box (slightly lighter again); ALL lines are drawn and
					// CLIPPED by the animated height — slide reveal like the GUI
					// panel bodies.
					const float CY0 = std::max(ly, MsgTop);
					const float CY1 = std::min(ly + TotalH, MsgBottom);
					if(CY1 > CY0)
					{
						const float BX = BubbleX + 6.0f * S;
						const float BW = BoxW - 12.0f * S;
						Graphics()->ClipEnable(BX, CY0, BW, CY1 - CY0);
						DrawRoundedRect(BX, ly, BW, TotalH, 4.0f * S, IGraphics::CORNER_ALL,
							ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.05f * PanelOp));
						float cy = ly + 4.0f * S;
						if(!L.vToolDiff.empty())
						{
							// Diff view: tinted rows, colored gutter bar, old/new
							// line numbers, highlighted text.
							for(const SToolDiffLine &D : L.vToolDiff)
							{
								const float LineH = FontSz + 1.0f * S;
								if(D.m_Type == 1)
									DrawRoundedRect(BX + 2.0f * S, cy - 0.5f * S, BW - 4.0f * S, LineH, 2.0f * S,
										IGraphics::CORNER_ALL, ColorRGBA(0.3f, 0.9f, 0.5f, alpha * 0.10f));
								else if(D.m_Type == 2)
									DrawRoundedRect(BX + 2.0f * S, cy - 0.5f * S, BW - 4.0f * S, LineH, 2.0f * S,
										IGraphics::CORNER_ALL, ColorRGBA(0.95f, 0.35f, 0.35f, alpha * 0.10f));
								if(D.m_Type == 1)
									DrawRoundedRect(BX + 2.0f * S, cy, 1.5f * S, LineH - 1.0f * S, 0.5f * S,
										IGraphics::CORNER_ALL, ColorRGBA(0.36f, 0.85f, 0.55f, alpha * 0.9f));
								else if(D.m_Type == 2)
									DrawRoundedRect(BX + 2.0f * S, cy, 1.5f * S, LineH - 1.0f * S, 0.5f * S,
										IGraphics::CORNER_ALL, ColorRGBA(0.93f, 0.42f, 0.42f, alpha * 0.9f));
								char aNum[32];
								if(D.m_OldNo > 0)
								{
									str_format(aNum, sizeof(aNum), "%d", D.m_OldNo);
									DrawAgentText(BX + 7.0f * S, cy, aNum, MetaSz,
										ColorRGBA(0xff666677, true).WithAlpha(alpha * TextOp));
								}
								if(D.m_NewNo > 0)
								{
									str_format(aNum, sizeof(aNum), "%d", D.m_NewNo);
									DrawAgentText(BX + 33.0f * S, cy, aNum, MetaSz,
										ColorRGBA(0xff666677, true).WithAlpha(alpha * TextOp));
								}
								std::vector<SAgentSeg> vSegs;
								AgentHighlightCodeLine(D.m_Text, L.ToolLang, vSegs);
								float tx = BX + 62.0f * S;
								for(const SAgentSeg &Seg : vSegs)
								{
									DrawAgentText(tx, cy, Seg.m_Text.c_str(), FontSz, AgentCodeSegColor(Seg.m_Style, MsgCol));
									tx += TextRender()->TextWidth(FontSz, Seg.m_Text.c_str());
								}
								cy += LineH;
							}
						}
						else
						{
								if(L.ToolHasInput)
								{
									const bool Prompt = Msg.m_ToolName == "shell" || Msg.m_ToolName == "console";
									if(Prompt)
									{
										for(size_t li = 0; li < L.vToolCodeLines.size(); ++li)
										{
											if(li == 0)
												DrawAgentText(BX + 8.0f * S, cy, "$", FontSz,
													ColorRGBA(0xff6fcf97, true).WithAlpha(alpha * TextOp));
											DrawAgentText(BX + 18.0f * S, cy, L.vToolCodeLines[li].c_str(), FontSz, MsgCol);
											cy += FontSz + 2.0f * S;
										}
										cy += 4.0f * S;
									}
									else
									{
										for(size_t li = 0; li < L.vToolCodeLines.size(); ++li)
										{
											std::vector<SAgentSeg> vSegs;
											AgentHighlightCodeLine(L.vToolCodeLines[li], L.ToolLang, vSegs);
											float tx = BX + 8.0f * S;
											for(const SAgentSeg &Seg : vSegs)
											{
												DrawAgentText(tx, cy, Seg.m_Text.c_str(), FontSz, AgentCodeSegColor(Seg.m_Style, MsgCol));
												tx += TextRender()->TextWidth(FontSz, Seg.m_Text.c_str());
											}
											cy += FontSz + 2.0f * S;
										}
										cy += 4.0f * S;
									}
								}
								if(!L.vToolOutLines.empty())
								{
									if(L.ToolOutIsCode)
									{
										// read: right-aligned line-number gutter + highlighted text
										int MaxNo = 1;
										for(size_t ni = 0; ni < L.vToolOutNos.size(); ++ni)
											MaxNo = std::max(MaxNo, L.vToolOutNos[ni]);
										char aG[16];
										str_format(aG, sizeof(aG), "%d", MaxNo);
										const float GutterW = TextRender()->TextWidth(MetaSz, aG) + 8.0f * S;
										for(size_t li = 0; li < L.vToolOutLines.size(); ++li)
										{
											if(li < L.vToolOutNos.size() && L.vToolOutNos[li] > 0)
											{
												char aNum[16];
												str_format(aNum, sizeof(aNum), "%d", L.vToolOutNos[li]);
												const float NW = TextRender()->TextWidth(MetaSz, aNum);
												DrawAgentText(BX + 6.0f * S + GutterW - 4.0f * S - NW, cy + 1.0f * S, aNum, MetaSz,
													ColorRGBA(0xff5a5a6a, true).WithAlpha(alpha * TextOp));
											}
											std::vector<SAgentSeg> vSegs;
											AgentHighlightCodeLine(L.vToolOutLines[li], L.ToolLang, vSegs);
											float tx = BX + 6.0f * S + GutterW;
											for(const SAgentSeg &Seg : vSegs)
											{
												DrawAgentText(tx, cy, Seg.m_Text.c_str(), FontSz, AgentCodeSegColor(Seg.m_Style, MsgCol));
												tx += TextRender()->TextWidth(FontSz, Seg.m_Text.c_str());
											}
											cy += FontSz + 2.0f * S;
										}
									}
									else
									{
										for(const std::string &Line : L.vToolOutLines)
										{
											DrawAgentText(BX + 8.0f * S, cy, Line.c_str(), FontSz, MsgCol);
											cy += FontSz + 2.0f * S;
										}
									}
								}
						}
						Graphics()->ClipDisable();
						// Single global clip state: restore the message-area clip
						// (otherwise every following message draws unclipped).
						Graphics()->ClipEnable(Left, MsgTop, W, MsgAreaH);
					}
					ly += TotalH;
				}
		}
			else
			{
				// Markdown display lines: headers accent + larger, bold white,
				// italic dimmed. Fenced code blocks get a language header strip,
				// their own background, a line-number gutter and syntax
				// highlighting.
				int CodeNo = 0;
				for(const SAgentLine &Ln : L.vLines)
				{
					if(Ln.m_Type == 3)
					{
						// Code block header strip with the language name.
						const float HdrH = Ln.m_Size;
						DrawRoundedRect(BubbleX, ly, BubbleW, HdrH, 3.0f * S,
							IGraphics::CORNER_ALL, ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.08f * PanelOp));
						DrawAgentText(BubbleX + 6.0f * S, ly + (HdrH - MetaSz) * 0.5f, AgentLangName(AgentLangFromName(Ln.m_Lang)), MetaSz,
							ColorRGBA(0xff9ecbff, true).WithAlpha(alpha * TextOp));
						CodeNo = 0;
						ly += HdrH;
						continue;
					}
					if(Ln.m_Type == 2)
					{
						// Code line: background slot + right-aligned line number
						// + highlighted text. Slots touch but never overlap (an
						// overlap would double-blend the translucent tint).
						DrawRoundedRect(BubbleX, ly, BubbleW, Ln.m_Size + 3.0f * S, 0.0f,
							IGraphics::CORNER_NONE, ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.05f * PanelOp));
						++CodeNo;
						char aNum[16];
						str_format(aNum, sizeof(aNum), "%d", CodeNo);
						const float GutterW = TextRender()->TextWidth(MetaSz, "000") + 6.0f * S;
						const float NW = TextRender()->TextWidth(MetaSz, aNum);
						DrawAgentText(TextX + GutterW - NW, ly, aNum, MetaSz,
							ColorRGBA(0xff5a5a6a, true).WithAlpha(alpha * TextOp));
						std::vector<SAgentSeg> vSegs;
						AgentHighlightCodeLine(Ln.m_Plain, AgentLangFromName(Ln.m_Lang), vSegs);
						float x = TextX + GutterW + 4.0f * S;
						for(const SAgentSeg &Seg : vSegs)
						{
							DrawAgentText(x, ly, Seg.m_Text.c_str(), Ln.m_Size, AgentCodeSegColor(Seg.m_Style, MsgCol));
							x += TextRender()->TextWidth(Ln.m_Size, Seg.m_Text.c_str());
						}
						ly += Ln.m_Size + 3.0f * S;
						continue;
					}
					CodeNo = 0;
					ColorRGBA LineCol = MsgCol;
					if(Ln.m_Type == 1)
						LineCol = MainCol.WithAlpha(alpha * TextOp);
					float x = TextX;
					for(const SAgentSeg &Seg : Ln.m_Segs)
					{
						ColorRGBA SegCol = LineCol;
						if(Ln.m_Type != 1)
						{
							if(Seg.m_Style == AGSEG_BOLD)
								SegCol = ColorRGBA(0xffffffff, true).WithAlpha(alpha * TextOp);
							else if(Seg.m_Style == AGSEG_ITALIC)
								SegCol = ColorRGBA(0xffa8a8b8, true).WithAlpha(alpha * TextOp * 0.8f);
							else if(Seg.m_Style == AGSEG_CODE || Seg.m_Style == AGSEG_CODEBLOCK)
								SegCol = MainCol.WithAlpha(alpha * TextOp);
						}
						DrawAgentText(x, ly, Seg.m_Text.c_str(), Ln.m_Size, SegCol);
						x += TextRender()->TextWidth(Ln.m_Size, Seg.m_Text.c_str());
					}
					ly += Ln.m_Size + 3.0f * S;
				}
			}

		// Streaming tail: a blinking caret while the answer arrives.
		if(Msg.m_Streaming && Agent.IsBusy())
		{
			const bool CaretOn = std::fmod(Client()->LocalTime(), 1.0f) < 0.5f;
			if(CaretOn)
			{
				float CaretX = TextX;
				if(!L.vLines.empty())
				{
					const SAgentLine &LastLine = L.vLines.back();
					CaretX = TextX + TextRender()->TextWidth(LastLine.m_Size, LastLine.m_Plain.c_str());
				}
				DrawRoundedRect(CaretX + 2.0f * S, ly - FontSz + 2.0f * S, 1.5f * S, FontSz - 4.0f * S, 0.0f,
					IGraphics::CORNER_NONE, ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.8f * TextOp));
			}
		}

		// Meta line (assistant usage / work time / timestamp) — final answer only.
		if(Msg.m_Role == CAgent::ROLE_ASSISTANT && !Msg.m_Streaming && Msg.m_Tokens > 0)
		{
			std::string Meta;
			bool Has = false;
			if(ShowUsage && Msg.m_Tokens > 0)
			{
				Meta += std::to_string(Msg.m_Tokens) + " Tokens";
				Has = true;
			}
			if(ShowWork && Msg.m_WorkSec > 0)
			{
				Meta += Has ? " | " : "";
				Meta += std::to_string(Msg.m_WorkSec) + "s";
				Has = true;
			}
			if(ShowTime)
			{
				char aTime[64];
				AgentFormatTime(Msg.m_Time, aTime, sizeof(aTime));
				Meta += Has ? " | " : "";
				Meta += aTime;
			}
			if(!Meta.empty())
			{
				const float MetaX = Right ? BubbleX + BubbleW - TextRender()->TextWidth(MetaSz, Meta.c_str()) : TextX;
				DrawText(MetaX, ly + 2.0f * S, MetaSz, Meta.c_str(),
					ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.75f));
				ly += MetaSz + 3.0f * S;
			}
		}

		// Copy button (hover).
		if(ShowCopy)
		{
			const float CopyW = 40.0f * S;
			const float CopyH = 14.0f * S;
			float CopyX = Right ? BubbleX : BubbleX + BubbleW - CopyW;
			float CopyY = Y;
			const float BtnW = 16.0f * S;
			float IconX = (Right ? BubbleX + MsgBubbleW - BtnW : BubbleX);
			(void)CopyX;
			(void)CopyY;
			(void)CopyW;
			(void)CopyH;
			const bool Hover = PointInRect(m_MousePos, IconX, Y, BtnW, CopyH);
			if(Hover)
				m_AgentCopyHover = MsgIdx;
			m_AgentCopyRects.push_back(IconX);
			m_AgentCopyRects.push_back(Y);
			m_AgentCopyRects.push_back(BtnW);
			m_AgentCopyRects.push_back(CopyH);
			if(Hover)
				DrawRoundedRect(IconX, Y, BtnW, CopyH, 2.0f * S, IGraphics::CORNER_ALL,
					ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.12f));
			DrawText(IconX + (BtnW - TextRender()->TextWidth(11.0f * S, "Copy")) * 0.5f, Y + 1.0f * S, 11.0f * S,
				"Copy", ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * (Hover ? 1.0f : 0.55f)));
		}

		Y += L.Height + Indent;
		MsgIdx++;
	}
	Graphics()->ClipDisable();

	// Scrollbar (right side): wide-enough thumb, draggable, top-based offset.
	if(MaxScroll > 0.0f)
	{
		const float TrackH = MsgBottom - MsgTop;
		const float ThumbH = std::max(30.0f * S, TrackH * std::min(1.0f, MsgAreaH / std::max(1.0f, ContentH)));
		const float ThumbY = MsgTop + (TrackH - ThumbH) * (m_AgentScroll / MaxScroll);
		const float ThumbX = Left + W - 9.0f * S;
		const float ThumbW = 6.0f * S;
		m_aAgentScrollbarThumbRect[0] = ThumbX - 2.0f * S;
		m_aAgentScrollbarThumbRect[1] = ThumbY;
		m_aAgentScrollbarThumbRect[2] = ThumbW + 4.0f * S;
		m_aAgentScrollbarThumbRect[3] = ThumbH;
		const bool DragHover = m_AgentScrollbarDrag ||
			PointInRect(m_MousePos, ThumbX, ThumbY, ThumbW, ThumbH);
		DrawRoundedRect(ThumbX, ThumbY, ThumbW, ThumbH, 3.0f * S, IGraphics::CORNER_ALL,
			ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * (DragHover ? 0.85f : 0.45f)));
	}

	// ---- ONE field container: input line + model/effort + Send ----
	// Clipped by the animated panel bottom (RenderPanel-style body clip).
	const bool Busy = Agent.IsBusy();
	const float InputW = W - Pad * 2.0f;
	const float FieldX = Left + Pad;
	const float FieldClipH = PanelBottom - FieldY;
	Graphics()->ClipEnable(Left, FieldY, W, std::max(0.0f, FieldClipH));
	DrawRoundedRect(FieldX + 3.0f * S, FieldY + 3.0f * S, InputW, FieldH, 4.0f * S, IGraphics::CORNER_ALL,
		ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.15f));
	DrawRoundedRect(FieldX - 1.0f, FieldY - 1.0f, InputW + 2.0f, FieldH + 2.0f, 4.0f * S, IGraphics::CORNER_ALL,
		ColorRGBA(0xff2a2a3e, true).WithAlpha(alpha));
	DrawRoundedRect(FieldX, FieldY, InputW, FieldH, 4.0f * S, IGraphics::CORNER_ALL,
		ColorRGBA(0xff0d0d18, true).WithAlpha(alpha));

	m_aAgentInputRect[0] = FieldX + FieldPad;
	m_aAgentInputRect[1] = InputY;
	m_aAgentInputRect[2] = InputW - FieldPad * 2.0f;
	m_aAgentInputRect[3] = InputH;
	const char *pInputText = Agent.m_aInput;
	const bool HasInput = pInputText[0] != '\0';
	const float TextY = InputY + (InputH - FontSz) * 0.5f;
	// Horizontal scroll: keep the caret inside the input field.
	float InputScroll = 0.0f;
	float CaretPx = 1.0f * S;
	if(m_AgentInputEditing)
	{
		char aPrefix[1024];
		const int PrefixLen = std::min(m_AgentInputCursor, (int)sizeof(aPrefix) - 1);
		mem_copy(aPrefix, pInputText, PrefixLen);
		aPrefix[PrefixLen] = 0;
		CaretPx = TextRender()->TextWidth(FontSz, aPrefix) + 1.0f * S;
		const float ViewW = m_aAgentInputRect[2] - 4.0f * S;
		if(CaretPx > ViewW)
			InputScroll = CaretPx - ViewW;
	}
	Graphics()->ClipEnable(m_aAgentInputRect[0], InputY + 1.0f * S, m_aAgentInputRect[2], InputH - 2.0f * S);
	if(HasInput)
		DrawText(m_aAgentInputRect[0] - InputScroll, TextY, FontSz, pInputText,
			TextCol.WithAlpha(alpha * TextOp));
	else
		DrawText(m_aAgentInputRect[0], TextY, FontSz, "Type your message...",
			ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.6f));
	if(m_AgentInputEditing)
	{
		const bool CaretOn = std::fmod(Client()->LocalTime(), 1.0f) < 0.5f;
		if(CaretOn)
			DrawRoundedRect(m_aAgentInputRect[0] + CaretPx - InputScroll,
				InputY + 4.0f * S, 1.5f * S, InputH - 8.0f * S, 0.0f, IGraphics::CORNER_NONE,
				ColorRGBA(1.0f, 1.0f, 1.0f, alpha));
	}
	Graphics()->ClipDisable();
	// Restore the field-container clip (ClipDisable is a single global state).
	Graphics()->ClipEnable(Left, FieldY, W, std::max(0.0f, FieldClipH));
	// Editing highlight: thin accent underline inside the container.
	if(m_AgentInputEditing)
		DrawRoundedRect(m_aAgentInputRect[0], InputY + InputH - 1.5f * S, m_aAgentInputRect[2], 1.5f * S, 0.0f,
			IGraphics::CORNER_NONE, MainCol.WithAlpha(alpha * MainOp));

	// Bottom row INSIDE the same container: dropdowns (transparent boxes with
	// hover highlight, ClickGUI look) + accent Send.
	const auto DrawMiniDrop = [&](float X, float Y, float BW, float BH, const char *pLabel, bool Hover) {
		if(Hover)
			DrawRoundedRect(X, Y, BW, BH, 3.0f * S, IGraphics::CORNER_ALL,
				ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.08f));
		DrawText(X + 6.0f * S, Y + (BH - 13.0f * S) * 0.5f, 13.0f * S, pLabel,
			TextCol.WithAlpha(alpha * TextOp));
		DrawText(X + BW - 16.0f * S, Y + (BH - 13.0f * S) * 0.5f, 13.0f * S, "\xe2\x88\xa8",
			ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.8f));
	};

	const std::vector<std::string> &vModels = Agent.Models();
	const char *pModel = vModels.empty() ? "?" : vModels[std::clamp(Agent.m_ModelIdx, 0, (int)vModels.size() - 1)].c_str();
	char aModelLabel[64];
	str_copy(aModelLabel, pModel, sizeof(aModelLabel) - 8);
	float ModelW = TextRender()->TextWidth(13.0f * S, aModelLabel) + 18.0f * S;
	ModelW = std::max(ModelW, 110.0f * S);
	m_aAgentModelBtnRect[0] = FieldX + FieldPad;
	m_aAgentModelBtnRect[1] = Row2Y;
	m_aAgentModelBtnRect[2] = ModelW;
	m_aAgentModelBtnRect[3] = BtnH2;
	DrawMiniDrop(m_aAgentModelBtnRect[0], m_aAgentModelBtnRect[1], ModelW, BtnH2, aModelLabel,
		PointInRect(m_MousePos, m_aAgentModelBtnRect[0], m_aAgentModelBtnRect[1], ModelW, BtnH2) || m_AgentDropdown == 1);

	const char *pEffort = g_AgentEffortOpts[std::clamp(Agent.m_EffortIdx, 0, 3)];
	char aEffortLabel[32];
	str_copy(aEffortLabel, pEffort, sizeof(aEffortLabel) - 8);
	const float EffortW = std::max(TextRender()->TextWidth(13.0f * S, aEffortLabel) + 18.0f * S, 70.0f * S);
	m_aAgentEffortBtnRect[0] = m_aAgentModelBtnRect[0] + ModelW + 4.0f * S;
	m_aAgentEffortBtnRect[1] = Row2Y;
	m_aAgentEffortBtnRect[2] = EffortW;
	m_aAgentEffortBtnRect[3] = BtnH2;
	DrawMiniDrop(m_aAgentEffortBtnRect[0], m_aAgentEffortBtnRect[1], EffortW, BtnH2, aEffortLabel,
		PointInRect(m_MousePos, m_aAgentEffortBtnRect[0], m_aAgentEffortBtnRect[1], EffortW, BtnH2) || m_AgentDropdown == 2);

	m_aAgentSendRect[0] = FieldX + InputW - SendW - FieldPad;
	m_aAgentSendRect[1] = Row2Y;
	m_aAgentSendRect[2] = SendW;
	m_aAgentSendRect[3] = BtnH2;
	const bool SendHover = PointInRect(m_MousePos, m_aAgentSendRect[0], m_aAgentSendRect[1], SendW, BtnH2);
	if(Busy)
		DrawRoundedRect(m_aAgentSendRect[0], m_aAgentSendRect[1], SendW, BtnH2, 4.0f * S, IGraphics::CORNER_ALL,
			ColorRGBA(0xffc43d3d, true).WithAlpha(alpha));
	else
		DrawRoundedRect(m_aAgentSendRect[0], m_aAgentSendRect[1], SendW, BtnH2, 4.0f * S, IGraphics::CORNER_ALL,
			MainCol.WithAlpha(alpha * (SendHover ? 1.0f : 0.85f)));
	const char *pSendLabel = Busy ? "Stop" : "Send";
	const float SendLW = TextRender()->TextWidth(15.0f * S, pSendLabel);
	DrawText(m_aAgentSendRect[0] + (SendW - SendLW) * 0.5f, m_aAgentSendRect[1] + (BtnH2 - 15.0f * S) * 0.5f, 15.0f * S, pSendLabel,
		ColorRGBA(0xffffffff, true).WithAlpha(alpha));
	Graphics()->ClipDisable();

	// ---- dropdown popup (drawn last, on top; ClickGUI style) ----
	if(m_AgentDropdown != 0)
	{
		const bool IsModel = m_AgentDropdown == 1;
		const int OptCount = IsModel ? (int)vModels.size() : 4;
		const float ItemH = 20.0f * S;
		const float PopupW = IsModel ? ModelW : EffortW;
		const float PopupH = ItemH * OptCount + 4.0f * S;
		const float PopupX = IsModel ? m_aAgentModelBtnRect[0] : m_aAgentEffortBtnRect[0];
		float PopupY = m_aAgentModelBtnRect[1] - PopupH - 2.0f * S;
		if(PopupY < Top + HeaderH)
			PopupY = m_aAgentModelBtnRect[1] + BtnH2 + 2.0f * S;
		m_aAgentDropdownRect[0] = PopupX;
		m_aAgentDropdownRect[1] = PopupY;
		m_aAgentDropdownRect[2] = PopupW;
		m_aAgentDropdownRect[3] = PopupH;
		DrawRoundedRect(PopupX + 4.0f * S, PopupY + 4.0f * S, PopupW, PopupH, 4.0f * S, IGraphics::CORNER_ALL,
			ColorRGBA(0xff000000, true).WithAlpha(alpha * 0.15f));
		DrawRoundedRect(PopupX - 1.0f, PopupY - 1.0f, PopupW + 2.0f, PopupH + 2.0f, 4.0f * S, IGraphics::CORNER_ALL,
			ColorRGBA(0xff2a2a3e, true).WithAlpha(alpha));
		DrawRoundedRect(PopupX, PopupY, PopupW, PopupH, 4.0f * S, IGraphics::CORNER_ALL,
			ColorRGBA(0xff0d0d18, true).WithAlpha(alpha * 0.98f));
		m_AgentDropdownHover = -1;
		for(int i = 0; i < OptCount; ++i)
		{
			const float ItemY = PopupY + 2.0f * S + i * ItemH;
			const bool Hover = PointInRect(m_MousePos, PopupX, ItemY, PopupW, ItemH);
			if(Hover)
				m_AgentDropdownHover = i;
			if(Hover)
				DrawRoundedRect(PopupX + 2.0f, ItemY, PopupW - 4.0f, ItemH, 2.0f * S, IGraphics::CORNER_ALL,
					MainCol.WithAlpha(alpha * 0.4f));
			const int Selected = IsModel ? Agent.m_ModelIdx : Agent.m_EffortIdx;
			if(i == Selected)
				DrawRoundedRect(PopupX + 3.0f, ItemY + 5.0f * S, 2.5f * S, ItemH - 10.0f * S, 1.0f * S, IGraphics::CORNER_ALL,
					MainCol.WithAlpha(alpha));
			const char *pOpt = IsModel ? vModels[std::clamp(i, 0, (int)vModels.size() - 1)].c_str() : g_AgentEffortOpts[i];
			DrawText(PopupX + 10.0f * S, ItemY + (ItemH - 13.0f * S) * 0.5f, 13.0f * S, pOpt,
				ColorRGBA(i == Selected ? MainHex : 0xffd8d8e8, true).WithAlpha(alpha));
		}
	}

	// Reset the text renderer color state — KxDrawTextColorSplits leaves the
	// last rainbow color behind and would tint whatever renders after us.
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

bool CClickGui::HandleAgentClick(vec2 mousePos)
{
	CAgent &Agent = GameClient()->m_Agent;

	// Panel-style collapse: the arrow zone in the header toggles the body.
	const float Sc = (g_Config.m_KxUiSettingsOn != 0) ? (g_Config.m_KxArrayAgentSize / 10.0f) : 1.0f;
	if(m_AgentRectValid && PointInRect(mousePos,
		m_AgentRectX + 4.0f * Sc, m_AgentRectY + 4.0f * Sc, 26.0f * Sc, 30.0f * Sc))
	{
		m_AgentCollapsed = !m_AgentCollapsed;
		return true;
	}
	// Collapsed (or collapsing): nothing else to click (stale rects from the
	// last open frame must not act); the header drag still works via `false`.
	if(m_AgentCollapseAnim <= 0.5f)
		return false;

	// The header is the drag handle and visually sits ABOVE the scrolled
	// message content (which is clipped there). Toggle/copy rects of messages
	// scrolled under the header must never steal a header click.
	if(ArrayAgentHeaderHitTest(mousePos))
		return false;

	// v6.14: main page (session list).
	if(Agent.SessionListVisible())
	{
		if(PointInRect(mousePos, m_aAgentNewSessionRect[0], m_aAgentNewSessionRect[1],
			m_aAgentNewSessionRect[2], m_aAgentNewSessionRect[3]))
		{
			Agent.NewSession();
			m_AgentScroll = m_AgentScrollTarget = 0.0f;
			m_AgentStickBottom = true;
			return true;
		}
		const int SessionCount = (int)m_AgentSessionRects.size() / 4;
		for(int i = 0; i < SessionCount; ++i)
		{
			if(PointInRect(mousePos, m_AgentSessionRects[i * 4], m_AgentSessionRects[i * 4 + 1],
				m_AgentSessionRects[i * 4 + 2], m_AgentSessionRects[i * 4 + 3]))
			{
				if(i < (int)m_AgentSessionIds.size())
					Agent.OpenSession(m_AgentSessionIds[i]);
				m_AgentScroll = m_AgentScrollTarget = 0.0f;
				m_AgentStickBottom = true;
				return true;
			}
		}
		if(ArrayAgentHeaderHitTest(mousePos))
			return false;
		return ArrayAgentHitTest(mousePos);
	}

	// Open dropdown first: item hit selects, click outside closes.
	if(m_AgentDropdown != 0)
	{
		const bool IsModel = m_AgentDropdown == 1;
		const bool InPopup = PointInRect(mousePos, m_aAgentDropdownRect[0], m_aAgentDropdownRect[1],
			m_aAgentDropdownRect[2], m_aAgentDropdownRect[3]);
		if(InPopup && m_AgentDropdownHover >= 0)
		{
			if(IsModel)
				Agent.m_ModelIdx = m_AgentDropdownHover;
			else
				Agent.m_EffortIdx = m_AgentDropdownHover;
			m_AgentDropdown = 0;
			return true;
		}
		m_AgentDropdown = 0;
		if(InPopup)
			return true;
		// A click on one of the dropdown buttons while a popup is open just
		// closes it (the close happened above) — consume so the button code
		// below doesn't immediately reopen it.
		if(PointInRect(mousePos, m_aAgentModelBtnRect[0], m_aAgentModelBtnRect[1], m_aAgentModelBtnRect[2], m_aAgentModelBtnRect[3]) ||
			PointInRect(mousePos, m_aAgentEffortBtnRect[0], m_aAgentEffortBtnRect[1], m_aAgentEffortBtnRect[2], m_aAgentEffortBtnRect[3]))
			return true;
	}

	// Scrollbar: grab the thumb (drag movement is applied in RenderArrayAgent).
	if(!m_AgentScrollbarDrag &&
		PointInRect(mousePos, m_aAgentScrollbarThumbRect[0], m_aAgentScrollbarThumbRect[1],
			m_aAgentScrollbarThumbRect[2], m_aAgentScrollbarThumbRect[3]))
	{
		m_AgentScrollbarDrag = true;
		m_AgentScrollbarGrabOffset = mousePos.y - m_aAgentScrollbarThumbRect[1];
		return true;
	}
	if(m_AgentScrollbarDrag)
		return true; // dragging: swallow clicks until release

	// Clickable block headers ([ Code Execution > ] / Reasoning).
	{
		const int ToggleCount = (int)m_AgentToggleRects.size() / 4;
		for(int i = 0; i < ToggleCount; ++i)
		{
			if(PointInRect(mousePos, m_AgentToggleRects[i * 4], m_AgentToggleRects[i * 4 + 1],
				m_AgentToggleRects[i * 4 + 2], m_AgentToggleRects[i * 4 + 3]))
			{
				const int MsgI = m_AgentToggleMsgIdx[i];
				if(MsgI >= 0 && MsgI < (int)Agent.m_vMessages.size())
				{
					CAgent::SAgentMessage &M = Agent.m_vMessages[MsgI];
					if(m_AgentToggleKind[i] == 0)
						M.m_Expanded = !M.m_Expanded;
					else
						M.m_ReasonOpen = !M.m_ReasonOpen;
				}
				return true;
			}
		}
	}

	// Copy buttons.
	const int CopyCount = (int)m_AgentCopyRects.size() / 4;
	for(int i = 0; i < CopyCount; ++i)
	{
		if(PointInRect(mousePos, m_AgentCopyRects[i * 4], m_AgentCopyRects[i * 4 + 1],
			m_AgentCopyRects[i * 4 + 2], m_AgentCopyRects[i * 4 + 3]))
		{
			if(i < (int)Agent.m_vMessages.size())
				Input()->SetClipboardText(Agent.m_vMessages[i].m_Text.c_str());
			return true;
		}
	}

	// Send / Stop.
	if(PointInRect(mousePos, m_aAgentSendRect[0], m_aAgentSendRect[1], m_aAgentSendRect[2], m_aAgentSendRect[3]))
	{
		if(Agent.IsBusy())
			Agent.StopGeneration();
		else if(Agent.m_aInput[0])
		{
			Agent.Send(Agent.m_aInput);
			Agent.m_aInput[0] = '\0';
			m_AgentInputCursor = 0;
			// Keep the focus so the next message can be typed right away.
		}
		return true;
	}

	// Model / effort dropdown buttons.
	if(PointInRect(mousePos, m_aAgentModelBtnRect[0], m_aAgentModelBtnRect[1], m_aAgentModelBtnRect[2], m_aAgentModelBtnRect[3]))
	{
		m_AgentDropdown = (m_AgentDropdown == 1) ? 0 : 1;
		return true;
	}
	if(PointInRect(mousePos, m_aAgentEffortBtnRect[0], m_aAgentEffortBtnRect[1], m_aAgentEffortBtnRect[2], m_aAgentEffortBtnRect[3]))
	{
		m_AgentDropdown = (m_AgentDropdown == 2) ? 0 : 2;
		return true;
	}

	// Input field.
	if(PointInRect(mousePos, m_aAgentInputRect[0], m_aAgentInputRect[1], m_aAgentInputRect[2], m_aAgentInputRect[3]))
	{
		if(!m_AgentInputEditing)
		{
			m_AgentInputEditing = true;
			// v6.13: SDL text input must be ON or FLAG_TEXT events never
			// arrive (the row inputs do the same).
			Input()->StartTextInput();
		}
		return true;
	}
	if(m_AgentInputEditing)
	{
		m_AgentInputEditing = false;
		Input()->StopTextInput();
	}

	// The header is NOT consumed — it is the drag handle.
	if(ArrayAgentHeaderHitTest(mousePos))
		return false;

	// Any other click inside the panel (messages area etc.) is consumed too.
	return ArrayAgentHitTest(mousePos);
}

bool CClickGui::HandleAgentTextInput(const IInput::CEvent &Event)
{
	CAgent &Agent = GameClient()->m_Agent;
	if(Event.m_Flags & IInput::FLAG_TEXT)
	{
		const int Len = str_length(Agent.m_aInput);
		const int AddLen = str_length(Event.m_aText);
		if(Len + AddLen < (int)sizeof(Agent.m_aInput))
		{
			memmove(Agent.m_aInput + m_AgentInputCursor + AddLen, Agent.m_aInput + m_AgentInputCursor,
				Len - m_AgentInputCursor + 1);
			mem_copy(Agent.m_aInput + m_AgentInputCursor, Event.m_aText, AddLen);
			m_AgentInputCursor += AddLen;
		}
		return true;
	}
	if(!(Event.m_Flags & IInput::FLAG_PRESS))
		return true;
	switch(Event.m_Key)
	{
	case KEY_BACKSPACE:
	{
		if(m_AgentInputCursor > 0)
		{
			// Erase one UTF-8 character before the cursor.
			int Start = m_AgentInputCursor - 1;
			while(Start > 0 && ((unsigned char)Agent.m_aInput[Start] & 0xC0) == 0x80)
				--Start;
			memmove(Agent.m_aInput + Start, Agent.m_aInput + m_AgentInputCursor,
				str_length(Agent.m_aInput + m_AgentInputCursor) + 1);
			m_AgentInputCursor = Start;
		}
		return true;
	}
	case KEY_DELETE:
	{
		const int Len = str_length(Agent.m_aInput);
		if(m_AgentInputCursor < Len)
		{
			int End = m_AgentInputCursor + 1;
			while(End < Len && ((unsigned char)Agent.m_aInput[End] & 0xC0) == 0x80)
				++End;
			memmove(Agent.m_aInput + m_AgentInputCursor, Agent.m_aInput + End,
				str_length(Agent.m_aInput + End) + 1);
		}
		return true;
	}
	case KEY_LEFT:
	{
		if(m_AgentInputCursor > 0)
		{
			--m_AgentInputCursor;
			while(m_AgentInputCursor > 0 && ((unsigned char)Agent.m_aInput[m_AgentInputCursor] & 0xC0) == 0x80)
				--m_AgentInputCursor;
		}
		return true;
	}
	case KEY_RIGHT:
	{
		const int Len = str_length(Agent.m_aInput);
		if(m_AgentInputCursor < Len)
		{
			++m_AgentInputCursor;
			while(m_AgentInputCursor < Len && ((unsigned char)Agent.m_aInput[m_AgentInputCursor] & 0xC0) == 0x80)
				++m_AgentInputCursor;
		}
		return true;
	}
	case KEY_HOME:
		m_AgentInputCursor = 0;
		return true;
	case KEY_END:
		m_AgentInputCursor = str_length(Agent.m_aInput);
		return true;
	case KEY_RETURN:
	case KEY_KP_ENTER:
		if(Agent.m_aInput[0])
		{
			Agent.Send(Agent.m_aInput);
			Agent.m_aInput[0] = '\0';
			m_AgentInputCursor = 0;
		}
		return true;
	case KEY_ESCAPE:
		m_AgentInputEditing = false;
		Input()->StopTextInput();
		return true;
	default:
		return true;
	}
}

