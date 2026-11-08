// (c) Kinetix. Array* HUD translation unit — ArrayList / ArrayInfo /
// ArrayPlayer (+ the shared helpers and feature table). Moved out of
// clickgui.cpp; the CClickGui method declarations stay in clickgui.h.

#include <game/client/components/kinetix/clickgui.h>
#include <game/client/components/kinetix/array_shared.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/media_info.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

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

#include <game/client/animstate.h>
#include <game/client/components/skins.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>

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
	"Reply in the user's language.";

static const char *const KX_AGENT_TOOLS_JSON =
	"\"tools\":["
	"{\"type\":\"function\",\"function\":{\"name\":\"code_execution\",\"description\":\"Execute Lua code inside the Kinetix client (Code Execution). print(...) output and errors are returned to you as the tool result. Save files with the code itself (io/os) if needed — the tool only executes; file/net/os access is gated by the user's Execution Settings and may be denied. Keep scripts short, never loop forever, one complete script per call.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"Full Lua source code to execute\"}},\"required\":[\"code\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"documentation\",\"description\":\"Get the Lua binding documentation for ONE section: a class name (CConfig, CBotNet, CNetObj_PlayerInput, CCharacterCore, CCollision, CGameWorld, CChat, CControls, CCodeExec, CGameInfo, CCamera, CBinds) or net / io / os. Call it BEFORE writing code whenever you are not sure what is available. Do not guess APIs. The query may be free text containing the section name.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"description\":\"Section to look up, e.g. CConfig or net\"}},\"required\":[\"section\"]}}}"
	"]";

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
	if(IsBusy() || !pText || !pText[0])
		return;
	m_aError[0] = '\0';
	SAgentMessage User;
	User.m_Role = ROLE_USER;
	User.m_Text = pText;
	User.m_Time = std::time(nullptr);
	m_vMessages.push_back(std::move(User));
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
	m_WaitToolExec = false;
	m_SseBuf.clear();
	m_SseDone = false;
	m_AbortRequested = false;
	for(SAgentMessage &M : m_vMessages)
		M.m_Streaming = false;
}

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
	Body += KX_AGENT_TOOLS_JSON;
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
		if(json_value *pRoot = JsonParse(Call.m_Args.c_str(), Call.m_Args.size()))
		{
			if(pRoot->type == json_object)
				if(const json_value *pCode = json_object_get(pRoot, "code"))
					if(const char *pS = json_string_get(pCode))
						Code = pS;
			json_value_free(pRoot);
		}
		if(Code.empty())
			AppendToolResult(Call.m_Id, "code_execution", "error: no Lua code provided");
		// STATE_ERROR is sticky (never auto-resets) — a failed tool script must
		// not block later calls: only a RUNNING script or an active capture is
		// really busy. Execute() itself resets the state via Stop().
		else if(Exec.GetState() == CCodeExec::STATE_RUNNING || Exec.ToolCaptureActive())
			AppendToolResult(Call.m_Id, "code_execution", "error: Code Execution is busy, try again later");
		else
		{
			Exec.ToolCaptureStart();
			Exec.Execute(Code.c_str(), "agent_tool");
			m_WaitToolExec = true;
		}
	}
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
	if(m_WaitToolExec)
	{
		if(Exec.GetState() == CCodeExec::STATE_RUNNING)
			return; // the tool script is still executing co-operatively
		m_WaitToolExec = false;
		std::vector<std::string> vOut = Exec.ToolCaptureFinish();
		std::string Result;
		if(vOut.empty())
			Result = "(no output)";
		for(size_t i = 0; i < vOut.size(); ++i)
		{
			if(i > 0)
				Result += "\n";
			Result += vOut[i];
		}
		AppendToolResult(m_ActiveTool.m_Id, "code_execution", Result);
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
	if(!m_WaitToolExec && m_vPendingToolCalls.empty() && m_State == AGENT_TOOL_RUN)
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
};

// One display line: pre-wrapped, styled segments + drawing size.
struct SAgentLine
{
	std::string m_Plain;
	std::vector<SAgentSeg> m_Segs;
	float m_Size = 0.0f;
	int m_Type = 0;
	int m_Level = 0;
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
	for(const std::string &Line : vRaw)
	{
		if(Line.compare(0, 3, "```") == 0)
		{
			InFence = !InFence;
			continue;
		}
		if(InFence)
		{
			// Each source line stays its own block (keeps code line structure).
			SAgentBlock B;
			B.m_Type = 2;
			B.m_Text = Line;
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
	for(const SAgentBlock &B : vBlocks)
	{
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
				if(vOut.size() == Before && !SubLine.empty())
				{
					SAgentLine L;
					L.m_Size = Size;
					L.m_Type = B.m_Type;
					L.m_Level = B.m_Level;
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
static void AgentBuildMessageLayout(const CAgent::SAgentMessage &Msg, float TextW, float FontSz, float S, int ReasoningMode, ITextRender *pTextRender, SMessageLayout &L)
{
	if(Msg.m_Role == CAgent::ROLE_TOOL)
	{
		// Terminal widget: input (code / section / raw args fallback) + output.
		std::string Code;
		bool HasInput = false;
		if(json_value *pRoot = JsonParse(Msg.m_ToolArgs.c_str(), Msg.m_ToolArgs.size()))
		{
			if(pRoot->type == json_object)
			{
				const json_value *pV = json_object_get(pRoot, "code");
				if(!pV)
					pV = json_object_get(pRoot, "section");
				if(const char *pS = json_string_get(pV))
				{
					Code = pS;
					HasInput = !Code.empty();
				}
			}
			json_value_free(pRoot);
		}
		if(!HasInput)
		{
			// The model didn't send a code/section field (or sent nothing
			// parseable) — show the raw arguments so the input is never a
			// silent empty box.
			Code = AgentTrimBlank(Msg.m_ToolArgs);
			HasInput = !Code.empty();
		}
		L.ToolHasInput = HasInput;
		// The terminal body is indented (icon/gutter) — wrap to the INNER
		// box width or the lines overflow the widget (and the panel).
		AgentWrapText(AgentSanitize(Code).c_str(), TextW - 16.0f * S, FontSz, pTextRender, L.vToolCodeLines);
		AgentWrapText(AgentSanitize(AgentTrimBlank(Msg.m_ToolResult)).c_str(), TextW - 16.0f * S, FontSz, pTextRender, L.vToolOutLines);
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
		Left = std::clamp(Left, 0.0f, std::max(0.0f, ScreenW - W));
		Top = std::clamp(Top, 0.0f, std::max(0.0f, ScreenH - H));
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
	const ColorRGBA MainCol = ColorRGBA(MainHex, true).WithAlpha(alpha * MainOp);
	const ColorRGBA AccentCol = DDNetStyle ? ColorRGBA(0.15f, 0.15f, 0.15f, alpha * MainOp) : MainCol;
	const ColorRGBA TextCol = ColorRGBA(TextHex, true);

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
	DrawRoundedRect(Left, Top, W, DrawH, 5.0f * S, IGraphics::CORNER_ALL,
		ColorRGBA(PanelHex, true).WithAlpha(alpha * 0.97f * PanelOp));
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
	const float MsgBottom = std::min(FieldY - 4.0f * S, PanelBottom);
	const float MsgAreaH = std::max(0.0f, MsgBottom - MsgTop);
	const float BubbleW = W - Pad * 2.0f - 14.0f * S;
	const int XA = g_Config.m_KxArrayAgentXAnchor;

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
			AgentBuildMessageLayout(Msg, TextW, FontSz, S, ReasoningMode, TextRender(), Cache.m_Layout);
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
			const float HeaderH2 = FontSz + 10.0f * S;
			TextH += HeaderH2;
			if(Msg.m_ExpandAnim > 0.01f)
			{
				// One inner code box: input lines, gap, output lines.
				float Inner = 4.0f * S;
				if(L.ToolHasInput)
					Inner += (float)L.vToolCodeLines.size() * (FontSz + 2.0f * S) + 4.0f * S;
				Inner += (float)L.vToolOutLines.size() * (FontSz + 2.0f * S) + 6.0f * S;
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
	// reading (new messages keep the view pinned).
	const float MaxScroll = std::max(0.0f, ContentH - MsgAreaH);
	if(m_AgentStickBottom)
		m_AgentScroll = MaxScroll;
	m_AgentScroll = std::clamp(m_AgentScroll, 0.0f, MaxScroll);
	m_AgentStickBottom = m_AgentScroll >= MaxScroll - 1.0f;

	// Scrollbar drag: reposition from the mouse while held.
	const float ScrollTrackH = MsgAreaH;
	const float ScrollThumbH = std::max(30.0f * S, ScrollTrackH * std::min(1.0f, MsgAreaH / std::max(1.0f, ContentH)));
	if(m_AgentScrollbarDrag && MaxScroll > 0.0f)
	{
		const float TrackTop = MsgTop;
		const float Rel = (m_MousePos.y - TrackTop - m_AgentScrollbarGrabOffset) / std::max(1.0f, ScrollTrackH - ScrollThumbH);
		m_AgentScroll = std::clamp(Rel * MaxScroll, 0.0f, MaxScroll);
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
		// Terminal-style tool widget: header (icon + name + chevron) + animated
		// inner code box (input, output).
		if(Msg.m_Role == CAgent::ROLE_TOOL)
		{
			const char *pPretty = Msg.m_ToolName == "code_execution" ? "Code Execution" :
				(Msg.m_ToolName == "documentation" ? "Documentation" : Msg.m_ToolName.c_str());
			const float HeaderH2 = FontSz + 10.0f * S;
			const float BoxW = BubbleW;
			const bool Hover = PointInRect(m_MousePos, BubbleX, ly, BoxW, HeaderH2);
			// Widget box: slightly lighter than the panel + border.
			float WidgetH = HeaderH2;
			if(Msg.m_ExpandAnim > 0.01f)
			{
				float Inner = 4.0f * S;
				if(L.ToolHasInput)
					Inner += (float)L.vToolCodeLines.size() * (FontSz + 2.0f * S) + 4.0f * S;
				Inner += (float)L.vToolOutLines.size() * (FontSz + 2.0f * S) + 6.0f * S;
				WidgetH += (Inner + 2.0f * S) * Msg.m_ExpandAnim;
			}
			DrawRoundedRect(BubbleX, ly, BoxW, WidgetH, 6.0f * S, IGraphics::CORNER_ALL,
				ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.05f * PanelOp));
			DrawRoundedOutline(BubbleX + 0.5f, ly + 0.5f, BoxW - 1.0f, WidgetH - 1.0f, 5.0f * S,
				ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.10f * PanelOp), true, true);
			// Header: real mini-icon drawn with primitives (terminal ">_" in a
			// box / document sheet) + name + expandable-style arrow.
			const bool DocIcon = Msg.m_ToolName == "documentation";
			DrawRoundedOutline(BubbleX + 8.0f * S, ly + 6.0f * S, 13.0f * S, 11.0f * S, 2.0f * S,
				ThoughtHeadCol, true, true);
			if(DocIcon)
			{
				// Document: three text lines inside the sheet.
				for(int li = 0; li < 3; ++li)
					DrawRoundedRect(BubbleX + 10.5f * S, ly + 8.5f * S + li * 3.0f * S, 8.0f * S, 1.2f * S, 0.6f * S,
						IGraphics::CORNER_ALL, ThoughtHeadCol);
			}
			else
			{
				// Terminal: ">_" centered inside the box. The glyph's font box
				// is centered vertically; the underscore bar sits on its
				// baseline. Everything is measured, not hand-offset, so the
				// icon stays centered at any scale/font size.
				const float GlyphSz = FontSz * 0.55f;
				const float BarW = 3.5f * S;
				const float BarH = 1.2f * S;
				const float Gap = 0.6f * S;
				const float GlyphW = TextRender()->TextWidth(GlyphSz, ">");
				const float GlyphH = TextRender()->TextBoundingBox(GlyphSz, ">").m_H;
				const float TotalW = GlyphW + Gap + BarW;
				const float GX = BubbleX + 8.0f * S + (13.0f * S - TotalW) * 0.5f;
				const float GY = ly + 6.0f * S + (11.0f * S - GlyphH) * 0.5f;
				DrawAgentText(GX, GY, ">", GlyphSz, ThoughtHeadCol);
				DrawRoundedRect(GX + GlyphW + Gap, GY + GlyphH - BarH, BarW, BarH, 0.6f * S,
					IGraphics::CORNER_ALL, ThoughtHeadCol);
			}
			float hx = BubbleX + 8.0f * S + 13.0f * S + 6.0f * S;
			DrawAgentText(hx, ly + 5.0f * S, pPretty, FontSz * 0.9f, Hover ? ColorRGBA(0xffcccccc, true).WithAlpha(alpha * TextOp) : ThoughtHeadCol);
			DrawAgentText(BubbleX + BoxW - 20.0f * S, ly + 5.0f * S, Msg.m_Expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6", FontSz * 0.8f, ThoughtHeadCol);
			m_AgentToggleRects.push_back(BubbleX);
			m_AgentToggleRects.push_back(ly);
			m_AgentToggleRects.push_back(BoxW);
			m_AgentToggleRects.push_back(HeaderH2);
			m_AgentToggleMsgIdx.push_back(MsgIdx);
			m_AgentToggleKind.push_back(0);
			ly += HeaderH2;
			if(Msg.m_ExpandAnim > 0.01f)
			{
				float Inner = 4.0f * S;
				if(L.ToolHasInput)
					Inner += (float)L.vToolCodeLines.size() * (FontSz + 2.0f * S) + 4.0f * S;
				Inner += (float)L.vToolOutLines.size() * (FontSz + 2.0f * S) + 6.0f * S;
				const float TotalH = (Inner + 2.0f * S) * Msg.m_ExpandAnim;
				// Inner code box (slightly lighter again); ALL lines are drawn
				// and CLIPPED by the animated height — slide reveal, smooth in
				// both directions, exactly like the GUI panel bodies.
				const float CY0 = std::max(ly, MsgTop);
				const float CY1 = std::min(ly + TotalH, MsgBottom);
				if(CY1 > CY0)
				{
					Graphics()->ClipEnable(BubbleX + 6.0f * S, CY0, BoxW - 12.0f * S, CY1 - CY0);
					DrawRoundedRect(BubbleX + 6.0f * S, ly, BoxW - 12.0f * S, TotalH, 4.0f * S, IGraphics::CORNER_ALL,
						ColorRGBA(1.0f, 1.0f, 1.0f, alpha * 0.05f * PanelOp));
					float cy = ly + 4.0f * S;
					if(L.ToolHasInput)
					{
						for(const std::string &Line : L.vToolCodeLines)
						{
							DrawAgentText(BubbleX + 14.0f * S, cy, Line.c_str(), FontSz, ReasonCol);
							cy += FontSz + 2.0f * S;
						}
						cy += 4.0f * S;
					}
					for(const std::string &Line : L.vToolOutLines)
					{
						DrawAgentText(BubbleX + 14.0f * S, cy, Line.c_str(), FontSz, MsgCol);
						cy += FontSz + 2.0f * S;
					}
					Graphics()->ClipDisable();
				}
				ly += TotalH;
			}
		}
		else
		{
			// Markdown display lines: headers accent + larger, bold white, italic
			// dimmed, code accent.
			for(const SAgentLine &Ln : L.vLines)
			{
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
	if(HasInput)
		DrawText(m_aAgentInputRect[0], InputY + (InputH - FontSz) * 0.5f, FontSz, pInputText,
			TextCol.WithAlpha(alpha * TextOp));
	else
		DrawText(m_aAgentInputRect[0], InputY + (InputH - FontSz) * 0.5f, FontSz, "Type your message...",
			ColorRGBA(0xff8a8aa8, true).WithAlpha(alpha * 0.6f));
	// Editing highlight: thin accent underline inside the container.
	if(m_AgentInputEditing)
	{
		DrawRoundedRect(m_aAgentInputRect[0], InputY + InputH - 1.5f * S, m_aAgentInputRect[2], 1.5f * S, 0.0f,
			IGraphics::CORNER_NONE, MainCol.WithAlpha(alpha * MainOp));
		const bool CaretOn = std::fmod(Client()->LocalTime(), 1.0f) < 0.5f;
		if(CaretOn)
		{
			const float BeforeW = TextRender()->TextWidth(FontSz, pInputText);
			DrawRoundedRect(m_aAgentInputRect[0] + (HasInput ? BeforeW : 0.0f) + 1.0f * S,
				InputY + 4.0f * S, 1.5f * S, InputH - 8.0f * S, 0.0f, IGraphics::CORNER_NONE,
				ColorRGBA(1.0f, 1.0f, 1.0f, alpha));
		}
	}

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
			while(Start > 0 && (Agent.m_aInput[Start] & 0xC0) == 0x80)
				--Start;
			memmove(Agent.m_aInput + Start, Agent.m_aInput + m_AgentInputCursor,
				str_length(Agent.m_aInput + m_AgentInputCursor) + 1);
			m_AgentInputCursor = Start;
		}
		return true;
	}
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

