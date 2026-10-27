/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "menus.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <unordered_map>

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/components/menu_background.h>
#include <game/client/gameclient.h>
#include <game/client/components/kinetix/bot_control.h>
#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/code_exec/code_exec.h>
#include <algorithm>
#include <game/client/skin.h>
#include <game/client/ui.h>
#include <game/localization.h>

void CMenus::SetNeedSendInfo()
{
        // Kinetix Dummy Manager: while a dummy profile/tee session is active
        // the edits go to that dummy's profile, not to the main player config.
        if(m_DummyEditSlot >= 1)
        {
                if(m_DummyEditScope == 0)
                        GameClient()->m_BotNet.m_Dummy.Save();
                // All/Targets scopes are applied to every target in EndDummyEdit().
                return;
        }
        if(m_Dummy)
                m_NeedSendDummyinfo = true;
        else
                m_NeedSendinfo = true;
}

// Kinetix Dummy Manager: start a dummy profile/tee edit session. The settings
// UI opens on the Player (Profiles) or Tee (Dummy Tee) tab which renames to
// "Dummy N" (localized); the main tee (slot 0) keeps the vanilla player_*
// config and needs no session.
// v1.56.282: open the settings at the Player/Tee tab (Dummy Manager buttons).
// Both page variables are set (works from the main menu and in-game), the
// settings sub-tab comes from the Tee flag, then the menus are activated.
// m_MenuPage/m_GamePage are protected — hence this public CMenus wrapper
// (the ClickGUI Dummy Manager panel calls it).
void CMenus::OpenDummySettingsPage(bool Tee)
{
        SetActive(true);
        m_GamePage = PAGE_SETTINGS;
        // v6.4 (20.1.1): the main menu is a separate start screen (m_ShowStart);
        // switching m_MenuPage under it is invisible. Leave the start screen
        // and switch the page the same way the start menu's own buttons do.
        SetShowStart(false);
        SetMenuPage(PAGE_SETTINGS);
        g_Config.m_UiSettingsPage = Tee ? SETTINGS_TEE : SETTINGS_PLAYER;
}

void CMenus::BeginDummyEdit(int Slot, int Scope, bool Tee, const bool *pTargets)
{
        m_DummyEditSlot = Slot;
        m_DummyEditScope = Scope;
        m_DummyEditIsTee = Tee;
        m_SkinListScrollToSelected = true;

        if(pTargets)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        m_aDummyEditTargets[D] = pTargets[D];
        }
        else
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        m_aDummyEditTargets[D] = false;
        }

        // Single-dummy session: materialize the inherited (shared dummy_*)
        // values into the profile, so the editor shows what the dummy actually
        // uses instead of empty "inherit" fields. After this the profile owns
        // the values and stops following the shared config.
        if(Scope == 0 && Slot >= 1)
        {
                SDummyProfile &P = GameClient()->m_BotNet.m_Dummy.m_aProfiles[Slot];
                if(!P.m_aName[0])
                        str_copy(P.m_aName, g_Config.m_ClDummyName, sizeof(P.m_aName));
                if(!P.m_aClan[0])
                        str_copy(P.m_aClan, g_Config.m_ClDummyClan, sizeof(P.m_aClan));
                if(P.m_Country < 0)
                        P.m_Country = g_Config.m_ClDummyCountry;
                if(!P.m_aSkin[0])
                        str_copy(P.m_aSkin, g_Config.m_ClDummySkin, sizeof(P.m_aSkin));
                if(P.m_UseCustomColor < 0)
                        P.m_UseCustomColor = g_Config.m_ClDummyUseCustomColor;
                if(P.m_ColorBody == 0xFFFFFFFF)
                        P.m_ColorBody = (unsigned)g_Config.m_ClDummyColorBody;
                if(P.m_ColorFeet == 0xFFFFFFFF)
                        P.m_ColorFeet = (unsigned)g_Config.m_ClDummyColorFeet;
                if(P.m_DefaultEyes < 0)
                        P.m_DefaultEyes = g_Config.m_ClDummyDefaultEyes;
                GameClient()->m_BotNet.m_Dummy.Save();
        }

        // Prefill the scratch profile from the first target (All/Targets scopes),
        // falling back to the shared dummy_* config for inherited values.
        if(Scope != 0 && Slot >= 1)
        {
                const CDummyData *pDummy = &GameClient()->m_BotNet.m_Dummy;
                int First = -1;
                for(int D = 1; D < MAX_DUMMIES && First < 0; D++)
                {
                        if(Scope == 1 || m_aDummyEditTargets[D])
                                First = D;
                }
                if(First < 0)
                        First = 1;
                const SDummyProfile &P = pDummy->m_aProfiles[First];
                str_copy(m_aDummyEditScratchName, P.m_aName[0] ? P.m_aName : g_Config.m_ClDummyName, sizeof(m_aDummyEditScratchName));
                str_copy(m_aDummyEditScratchClan, P.m_aClan[0] ? P.m_aClan : g_Config.m_ClDummyClan, sizeof(m_aDummyEditScratchClan));
                m_DummyEditScratchCountry = P.m_Country >= 0 ? P.m_Country : g_Config.m_ClDummyCountry;
                str_copy(m_aDummyEditScratchSkin, P.m_aSkin[0] ? P.m_aSkin : g_Config.m_ClDummySkin, sizeof(m_aDummyEditScratchSkin));
                m_DummyEditScratchUseCustomColor = P.m_UseCustomColor >= 0 ? P.m_UseCustomColor : g_Config.m_ClDummyUseCustomColor;
                m_aDummyEditScratchColorBody = P.m_ColorBody != 0xFFFFFFFF ? P.m_ColorBody : (unsigned)g_Config.m_ClDummyColorBody;
                m_aDummyEditScratchColorFeet = P.m_ColorFeet != 0xFFFFFFFF ? P.m_ColorFeet : (unsigned)g_Config.m_ClDummyColorFeet;
                m_DummyEditScratchEyes = P.m_DefaultEyes >= 0 ? P.m_DefaultEyes : g_Config.m_ClDummyDefaultEyes;
        }
}

// Finish the session: apply the scratch profile (All/Targets) to every target,
// send the per-slot info changes and revert the dynamic tab back to "Player".
void CMenus::EndDummyEdit()
{
        if(m_DummyEditSlot < 0)
                return;

        if(m_DummyEditSlot >= 1)
        {
                if(m_DummyEditScope == 0)
                {
                        // Single dummy — the edits went straight into the profile.
                        if(Client()->DummyConnected(m_DummyEditSlot))
                                GameClient()->SendDummyInfo(false, m_DummyEditSlot);
                }
                else
                {
                        // All/Targets — apply the scratch profile to every target.
                        const bool All = m_DummyEditScope == 1;
                        for(int D = 1; D < MAX_DUMMIES; D++)
                        {
                                if(!All && !m_aDummyEditTargets[D])
                                        continue;
                                SDummyProfile &P = GameClient()->m_BotNet.m_Dummy.m_aProfiles[D];
                                str_copy(P.m_aName, m_aDummyEditScratchName, sizeof(P.m_aName));
                                str_copy(P.m_aClan, m_aDummyEditScratchClan, sizeof(P.m_aClan));
                                P.m_Country = m_DummyEditScratchCountry;
                                str_copy(P.m_aSkin, m_aDummyEditScratchSkin, sizeof(P.m_aSkin));
                                P.m_UseCustomColor = m_DummyEditScratchUseCustomColor;
                                P.m_ColorBody = m_aDummyEditScratchColorBody;
                                P.m_ColorFeet = m_aDummyEditScratchColorFeet;
                                P.m_DefaultEyes = m_DummyEditScratchEyes;
                                if(Client()->DummyConnected(D))
                                        GameClient()->SendDummyInfo(false, D);
                        }
                        // "Main" targeted in Targets mode → also update the shared
                        // dummy_* config every inheriting dummy falls back to.
                        if(m_DummyEditScope == 2 && m_aDummyEditTargets[0])
                        {
                                str_copy(g_Config.m_ClDummyName, m_aDummyEditScratchName, sizeof(g_Config.m_ClDummyName));
                                str_copy(g_Config.m_ClDummyClan, m_aDummyEditScratchClan, sizeof(g_Config.m_ClDummyClan));
                                g_Config.m_ClDummyCountry = m_DummyEditScratchCountry;
                                str_copy(g_Config.m_ClDummySkin, m_aDummyEditScratchSkin, sizeof(g_Config.m_ClDummySkin));
                                g_Config.m_ClDummyUseCustomColor = m_DummyEditScratchUseCustomColor;
                                g_Config.m_ClDummyColorBody = m_aDummyEditScratchColorBody;
                                g_Config.m_ClDummyColorFeet = m_aDummyEditScratchColorFeet;
                                g_Config.m_ClDummyDefaultEyes = m_DummyEditScratchEyes;
                                if(Client()->DummyConnected(1))
                                        GameClient()->SendDummyInfo(false, 1);
                        }
                }
                GameClient()->m_BotNet.m_Dummy.Save();
        }

        m_DummyEditSlot = -1;
        m_DummyEditScope = 0;
        m_DummyEditIsTee = false;
}

void CMenus::RenderSettings(CUIRect MainView)
{
        // v1.56.167 BUG5: clamp ui_settings_page. The config max in config_variables.h
        // was left at 10 after SETTINGS_KINETIX was removed in v1.56.166 (SETTINGS_LENGTH
        // shrunk 11→10, valid range is now 0-9). Subscribers with ui_settings_page=10 in
        // their saved settings_ddnet.cfg (from an older Kinetix build that had the Kinetix
        // tab) would crash on Settings open via dbg_assert_failed("ui_settings_page invalid").
        // Even after fixing the config max to 9, clamp here as defense-in-depth so any future
        // enum shrink cannot crash the client — the worst case is a silent reset to LANGUAGE.
        if(g_Config.m_UiSettingsPage < 0 || g_Config.m_UiSettingsPage >= SETTINGS_LENGTH)
                g_Config.m_UiSettingsPage = SETTINGS_LANGUAGE;

        // render background
        CUIRect Button, TabBar, RestartBar;
        MainView.VSplitRight(120.0f, &MainView, &TabBar);
        MainView.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);
        MainView.Margin(20.0f, &MainView);

        const bool NeedRestart = m_NeedRestartGraphics || m_NeedRestartSound || m_NeedRestartUpdate;
        if(NeedRestart)
        {
                MainView.HSplitBottom(20.0f, &MainView, &RestartBar);
                MainView.HSplitBottom(10.0f, &MainView, nullptr);
        }

        TabBar.HSplitTop(50.0f, &Button, &TabBar);
        Button.Draw(ms_ColorTabbarActive, IGraphics::CORNER_BR, 10.0f);

	const char *apTabs[SETTINGS_LENGTH] = {
		Localize("Language"),
		Localize("General"),
		Localize("Player"),
		Client()->IsSixup() ? "Tee 0.7" : Localize("Tee"),
		Localize("Appearance"),
		Localize("Controls"),
		Localize("Graphics"),
		Localize("Sound"),
		Localize("DDNet"),
		Localize("Assets"),
		Localize("Credits")};
	static CButtonContainer s_aTabButtons[SETTINGS_LENGTH];

        for(int i = 0; i < SETTINGS_LENGTH; i++)
        {
                TabBar.HSplitTop(10.0f, nullptr, &TabBar);
                TabBar.HSplitTop(26.0f, &Button, &TabBar);
                if(DoButton_MenuTab(&s_aTabButtons[i], apTabs[i], g_Config.m_UiSettingsPage == i, &Button, IGraphics::CORNER_R, &m_aAnimatorsSettingsTab[i]))
                        g_Config.m_UiSettingsPage = i;
        }

	if(g_Config.m_UiSettingsPage == SETTINGS_LANGUAGE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_LANGUAGE);
		RenderLanguageSettings(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_GENERAL)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_GENERAL);
		RenderSettingsGeneral(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_PLAYER)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_PLAYER);
		RenderSettingsPlayer(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_TEE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_TEE);
		if(Client()->IsSixup())
			RenderSettingsTee7(MainView);
		else
			RenderSettingsTee(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_APPEARANCE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_APPEARANCE);
		RenderSettingsAppearance(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_CONTROLS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_CONTROLS);
		m_MenusSettingsControls.Render(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_GRAPHICS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_GRAPHICS);
		RenderSettingsGraphics(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_SOUND)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_SOUND);
		RenderSettingsSound(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_DDNET)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_DDNET);
		RenderSettingsDDNet(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_ASSETS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_ASSETS);
		RenderSettingsAssets(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_CREDITS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_CREDITS);
		RenderSettingsCredits(MainView);
	}
	else
	{
		dbg_assert_failed("ui_settings_page invalid");
	}

        if(NeedRestart)
        {
                CUIRect RestartWarning, RestartButton;
                RestartBar.VSplitRight(125.0f, &RestartWarning, &RestartButton);
                RestartWarning.VSplitRight(10.0f, &RestartWarning, nullptr);
                if(m_NeedRestartUpdate)
                {
                        TextRender()->TextColor(1.0f, 0.4f, 0.4f, 1.0f);
                        Ui()->DoLabel(&RestartWarning, Localize("DDNet Client needs to be restarted to complete update!"), 14.0f, TEXTALIGN_ML);
                        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
                }
                else
                {
                        Ui()->DoLabel(&RestartWarning, Localize("You must restart the game for all settings to take effect."), 14.0f, TEXTALIGN_ML);
                }

                static CButtonContainer s_RestartButton;
                if(DoButton_Menu(&s_RestartButton, Localize("Restart"), 0, &RestartButton))
                {
                        if(Client()->State() == IClient::STATE_ONLINE || GameClient()->Editor()->HasUnsavedData())
                        {
                                m_Popup = POPUP_RESTART;
                        }
                        else
                        {
                                Client()->Restart();
                        }
                }
        }
}

bool CMenus::RenderHslaScrollbars(CUIRect *pRect, unsigned int *pColor, bool Alpha, float DarkestLight)
{
        const unsigned PrevPackedColor = *pColor;
        ColorHSLA Color(*pColor, Alpha);
        const ColorHSLA OriginalColor = Color;
        const char *apLabels[] = {Localize("Hue"), Localize("Sat."), Localize("Lht."), Localize("Alpha")};
        const float SizePerEntry = 20.0f;
        const float MarginPerEntry = 5.0f;
        const float PreviewMargin = 2.5f;
        const float PreviewHeight = 40.0f + 2 * PreviewMargin;
        const float OffY = (SizePerEntry + MarginPerEntry) * (3 + (Alpha ? 1 : 0)) - PreviewHeight;

        CUIRect Preview;
        pRect->VSplitLeft(PreviewHeight, &Preview, pRect);
        Preview.HSplitTop(OffY / 2.0f, nullptr, &Preview);
        Preview.HSplitTop(PreviewHeight, &Preview, nullptr);

        Preview.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 4.0f + PreviewMargin);
        Preview.Margin(PreviewMargin, &Preview);
        Preview.Draw(color_cast<ColorRGBA>(Color.UnclampLighting(DarkestLight)), IGraphics::CORNER_ALL, 4.0f + PreviewMargin);

        auto &&RenderHueRect = [&](CUIRect *pColorRect) {
                float CurXOff = pColorRect->x;
                const float SizeColor = pColorRect->w / 6;

		// red to yellow
		{
			Graphics()->SetColor4(
				ColorRGBA(1, 0, 0, 1),
				ColorRGBA(1, 1, 0, 1),
				ColorRGBA(1, 1, 0, 1),
				ColorRGBA(1, 0, 0, 1));

                        IGraphics::CFreeformItem Freeform(
                                CurXOff, pColorRect->y,
                                CurXOff + SizeColor, pColorRect->y,
                                CurXOff, pColorRect->y + pColorRect->h,
                                CurXOff + SizeColor, pColorRect->y + pColorRect->h);
                        Graphics()->QuadsDrawFreeform(&Freeform, 1);
                }

		// yellow to green
		CurXOff += SizeColor;
		{
			Graphics()->SetColor4(
				ColorRGBA(1, 1, 0, 1),
				ColorRGBA(0, 1, 0, 1),
				ColorRGBA(0, 1, 0, 1),
				ColorRGBA(1, 1, 0, 1));

                        IGraphics::CFreeformItem Freeform(
                                CurXOff, pColorRect->y,
                                CurXOff + SizeColor, pColorRect->y,
                                CurXOff, pColorRect->y + pColorRect->h,
                                CurXOff + SizeColor, pColorRect->y + pColorRect->h);
                        Graphics()->QuadsDrawFreeform(&Freeform, 1);
                }

		CurXOff += SizeColor;
		// green to turquoise
		{
			Graphics()->SetColor4(
				ColorRGBA(0, 1, 0, 1),
				ColorRGBA(0, 1, 1, 1),
				ColorRGBA(0, 1, 1, 1),
				ColorRGBA(0, 1, 0, 1));

                        IGraphics::CFreeformItem Freeform(
                                CurXOff, pColorRect->y,
                                CurXOff + SizeColor, pColorRect->y,
                                CurXOff, pColorRect->y + pColorRect->h,
                                CurXOff + SizeColor, pColorRect->y + pColorRect->h);
                        Graphics()->QuadsDrawFreeform(&Freeform, 1);
                }

		CurXOff += SizeColor;
		// turquoise to blue
		{
			Graphics()->SetColor4(
				ColorRGBA(0, 1, 1, 1),
				ColorRGBA(0, 0, 1, 1),
				ColorRGBA(0, 0, 1, 1),
				ColorRGBA(0, 1, 1, 1));

                        IGraphics::CFreeformItem Freeform(
                                CurXOff, pColorRect->y,
                                CurXOff + SizeColor, pColorRect->y,
                                CurXOff, pColorRect->y + pColorRect->h,
                                CurXOff + SizeColor, pColorRect->y + pColorRect->h);
                        Graphics()->QuadsDrawFreeform(&Freeform, 1);
                }

		CurXOff += SizeColor;
		// blue to purple
		{
			Graphics()->SetColor4(
				ColorRGBA(0, 0, 1, 1),
				ColorRGBA(1, 0, 1, 1),
				ColorRGBA(1, 0, 1, 1),
				ColorRGBA(0, 0, 1, 1));

                        IGraphics::CFreeformItem Freeform(
                                CurXOff, pColorRect->y,
                                CurXOff + SizeColor, pColorRect->y,
                                CurXOff, pColorRect->y + pColorRect->h,
                                CurXOff + SizeColor, pColorRect->y + pColorRect->h);
                        Graphics()->QuadsDrawFreeform(&Freeform, 1);
                }

		CurXOff += SizeColor;
		// purple to red
		{
			Graphics()->SetColor4(
				ColorRGBA(1, 0, 1, 1),
				ColorRGBA(1, 0, 0, 1),
				ColorRGBA(1, 0, 0, 1),
				ColorRGBA(1, 0, 1, 1));

                        IGraphics::CFreeformItem Freeform(
                                CurXOff, pColorRect->y,
                                CurXOff + SizeColor, pColorRect->y,
                                CurXOff, pColorRect->y + pColorRect->h,
                                CurXOff + SizeColor, pColorRect->y + pColorRect->h);
                        Graphics()->QuadsDrawFreeform(&Freeform, 1);
                }
        };

        auto &&RenderSaturationRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColor) {
                ColorHSLA LeftColor = color_cast<ColorHSLA>(CurColor);
                ColorHSLA RightColor = color_cast<ColorHSLA>(CurColor);

                LeftColor.s = 0.0f;
                RightColor.s = 1.0f;

                const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(LeftColor);
                const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(RightColor);

                Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

                IGraphics::CFreeformItem Freeform(
                        pColorRect->x, pColorRect->y,
                        pColorRect->x + pColorRect->w, pColorRect->y,
                        pColorRect->x, pColorRect->y + pColorRect->h,
                        pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
                Graphics()->QuadsDrawFreeform(&Freeform, 1);
        };

        auto &&RenderLightingRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColor) {
                ColorHSLA LeftColor = color_cast<ColorHSLA>(CurColor);
                ColorHSLA RightColor = color_cast<ColorHSLA>(CurColor);

                LeftColor.l = DarkestLight;
                RightColor.l = 1.0f;

                const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(LeftColor);
                const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(RightColor);

                Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

                IGraphics::CFreeformItem Freeform(
                        pColorRect->x, pColorRect->y,
                        pColorRect->x + pColorRect->w, pColorRect->y,
                        pColorRect->x, pColorRect->y + pColorRect->h,
                        pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
                Graphics()->QuadsDrawFreeform(&Freeform, 1);
        };

        auto &&RenderAlphaRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColorFull) {
                const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(color_cast<ColorHSLA>(CurColorFull).WithAlpha(0.0f));
                const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(color_cast<ColorHSLA>(CurColorFull).WithAlpha(1.0f));

                Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

                IGraphics::CFreeformItem Freeform(
                        pColorRect->x, pColorRect->y,
                        pColorRect->x + pColorRect->w, pColorRect->y,
                        pColorRect->x, pColorRect->y + pColorRect->h,
                        pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
                Graphics()->QuadsDrawFreeform(&Freeform, 1);
        };

        for(int i = 0; i < 3 + Alpha; i++)
        {
                CUIRect Button, Label;
                pRect->HSplitTop(SizePerEntry, &Button, pRect);
                pRect->HSplitTop(MarginPerEntry, nullptr, pRect);
                Button.VSplitLeft(140.0f, &Label, &Button);
                Label.VMargin(10.0f, &Label);

                Button.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 1.0f);

                CUIRect Rail;
                Button.Margin(2.0f, &Rail);

                char aBuf[32];

                // Hue
                if(i == 0)
                        str_format(aBuf, sizeof(aBuf), "%s: %.1f° (%03d)", apLabels[i], Color[i] * 360.0f, round_to_int(Color[i] * 255.0f));
                // Lht
                else if(i == 2)
                {
                        // handle internal light clamping, see `UnclampLighting`
                        float Lht = DarkestLight + Color[i] * (1.0f - DarkestLight);
                        str_format(aBuf, sizeof(aBuf), "%s: %.1f%% (%03d)", apLabels[i], Lht * 100.0f, round_to_int(Color[i] * 255.0f));
                }
                // Sat and Alpha
                else
                        str_format(aBuf, sizeof(aBuf), "%s: %.1f%% (%03d)", apLabels[i], Color[i] * 100.0f, round_to_int(Color[i] * 255.0f));
                Ui()->DoLabel(&Label, aBuf, 12.0f, TEXTALIGN_ML);

                ColorRGBA HandleColor;
                Graphics()->TextureClear();
                Graphics()->TrianglesBegin();
                if(i == 0)
                {
                        RenderHueRect(&Rail);
                        HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, 1.0f, 0.5f, 1.0f));
                }
                else if(i == 1)
                {
                        RenderSaturationRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, 1.0f, 0.5f, 1.0f)));
                        HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, 0.5f, 1.0f));
                }
                else if(i == 2)
                {
                        RenderLightingRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, 0.5f, 1.0f)));
                        HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, Color.l, 1.0f).UnclampLighting(DarkestLight));
                }
                else if(i == 3)
                {
                        RenderAlphaRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, Color.l, 1.0f).UnclampLighting(DarkestLight)));
                        HandleColor = color_cast<ColorRGBA>(Color.UnclampLighting(DarkestLight));
                }
                Graphics()->TrianglesEnd();

                Color[i] = Ui()->DoScrollbarH(&((char *)pColor)[i], &Button, Color[i], &HandleColor);
        }

        if(OriginalColor != Color)
        {
                *pColor = Color.Pack(Alpha);
        }
        return PrevPackedColor != *pColor;
}
