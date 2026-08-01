// (c) Kinetix. ESP component — see esp.h for overview.

#include <game/client/components/kinetix/esp.h>

#include <base/color.h>
#include <base/math.h>
#include <engine/shared/config.h>
#include <engine/graphics.h>
#include <game/client/gameclient.h>
#include <game/client/components/camera.h>
#include <game/client/components/controls.h>
#include <game/client/components/kinetix/kinetix_lines.h>

#include <game/generated/protocol.h>

void CEsp::OnReset()
{
}

bool CEsp::PassesFilters(int ClientId, int LocalId) const
{
	CGameClient *pGame = GameClient();
	if(!pGame->m_aClients[ClientId].m_Active)
		return false;
	if(!pGame->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	// Active dummy itself is never an ESP target.
	if(ClientId == LocalId)
		return false;

	// Team filter: 0=both 1=war 2=my
	if(g_Config.m_KxEspTeamFilter == 1)
	{
		if(pGame->m_Teams.SameTeam(LocalId, ClientId))
			return false;
	}
	else if(g_Config.m_KxEspTeamFilter == 2)
	{
		if(!pGame->m_Teams.SameTeam(LocalId, ClientId))
			return false;
	}
	// Friend filter: 0=both 1=ignore
	if(g_Config.m_KxEspFriendFilter == 1 && pGame->m_aClients[ClientId].m_Friend)
		return false;
	// Dummy filter: 0=both 1=ignore
	bool isDummy = false;
	for(int d = 0; d < MAX_DUMMIES; d++)
	{
		if(pGame->m_aLocalIds[d] == ClientId)
		{
			isDummy = true;
			break;
		}
	}
	if(g_Config.m_KxEspDummyFilter == 1 && isDummy)
		return false;
	// Freeze filter: 0=both 1=freeze 2=no_freeze
	bool isFrozen = pGame->m_aClients[ClientId].m_FreezeEnd != 0;
	if(g_Config.m_KxEspFreezeFilter == 1 && !isFrozen)
		return false;
	if(g_Config.m_KxEspFreezeFilter == 2 && isFrozen)
		return false;
	return true;
}

void CEsp::EmitLine(const vec2 &p0, const vec2 &p1) const
{
	// Shared Line rendering settings.
	ColorRGBA col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_KxLineRenderingColor, true));
	float alpha = (float)g_Config.m_KxLineRenderingAlpha / 100.0f;
	col.a = alpha;
	int lineSize = g_Config.m_KxLineRenderingSize;
	float halfWidth = lineSize > 0 ? 0.5f + (float)(lineSize - 1) * 0.25f : 0.0f;

	if(g_Config.m_KxLineRenderingLayer > 0)
	{
		// Deferred: CKinetixLines draws after m_MapLayersForeground (on top of blocks).
		KinetixEnqueueLine(p0, p1, col, halfWidth);
	}
	else
	{
		// Immediate: drawn now (behind foreground blocks).
		Graphics()->TextureClear();
		if(halfWidth > 0.0f)
		{
			vec2 dir = p1 - p0;
			float len = length(dir);
			if(len < 0.001f)
				return;
			dir /= len;
			vec2 perp = vec2(dir.y, -dir.x) * halfWidth;
			Graphics()->QuadsBegin();
			IGraphics::CFreeformItem q(
				p0.x - perp.x, p0.y - perp.y, p0.x + perp.x, p0.y + perp.y,
				p1.x - perp.x, p1.y - perp.y, p1.x + perp.x, p1.y + perp.y);
			Graphics()->SetColor(col.r, col.g, col.b, col.a);
			Graphics()->QuadsDrawFreeform(&q, 1);
			Graphics()->QuadsEnd();
		}
		else
		{
			Graphics()->LinesBegin();
			Graphics()->SetColor(col.r, col.g, col.b, col.a);
			IGraphics::CLineItem line(p0.x, p0.y, p1.x, p1.y);
			Graphics()->LinesDraw(&line, 1);
			Graphics()->LinesEnd();
		}
	}
}

void CEsp::OnRender()
{
	if(!g_Config.m_KxEsp)
		return;

	CGameClient *pGame = GameClient();
	int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return;

	// Determine point A (origin of all ESP lines).
	vec2 pointA;
	if(g_Config.m_KxEspMode == 1)
	{
		// Screen coordinates → world space.
		// MapScreenToInterface maps camera center to screen center; a screen
		// pixel offset from center scales by 1/zoom to world units.
		float sw = (float)Graphics()->ScreenWidth();
		float sh = (float)Graphics()->ScreenHeight();
		float zoom = pGame->m_Camera.m_Zoom;
		vec2 screenCenter(sw * 0.5f, sh * 0.5f);
		vec2 screenPoint((float)g_Config.m_KxEspScreenX, (float)g_Config.m_KxEspScreenY);
		pointA = pGame->m_Camera.m_Center + (screenPoint - screenCenter) / zoom;
	}
	else
	{
		// Active player — interpolated render position (same as aimbot).
		if(!pGame->m_Snap.m_aCharacters[LocalId].m_Active)
			return;
		pointA = pGame->m_aClients[LocalId].m_RenderPos;
	}

	// Set up world projection for immediate-mode drawing (deferred mode also
	// uses world space; CKinetixLines sets its own projection).
	Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

	// Draw a line from pointA to every player passing the filters.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!PassesFilters(i, LocalId))
			continue;
		vec2 target = pGame->m_aClients[i].m_RenderPos;
		EmitLine(pointA, target);
	}
}
