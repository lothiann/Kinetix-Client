// (c) Kinetix. ESP component — see esp.h for overview.

#include <game/client/components/kinetix/esp.h>

#include <base/color.h>
#include <base/math.h>
#include <cmath>
#include <engine/shared/config.h>
#include <engine/graphics.h>
#include <game/client/gameclient.h>
#include <game/client/components/camera.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <game/client/components/kinetix/kinetix_lines.h>

#include <generated/protocol.h>

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

// v1.56.113: Draw a single solid segment (immediate mode).
// Used by Line/Arrow styles and for arrowhead.
void CEsp::DrawSegment(const vec2 &p0, const vec2 &p1, ColorRGBA col, float halfWidth) const
{
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

// v1.56.113: Draw arrowhead at p1 (2 short lines forming a V pointing toward p1).
void CEsp::DrawArrowhead(const vec2 &p0, const vec2 &p1, ColorRGBA col, float halfWidth) const
{
	vec2 dir = p1 - p0;
	float len = length(dir);
	if(len < 0.001f)
		return;
	dir /= len;
	float arrowLen = 16.0f; // fixed = half tile (32px tile / 2)
	if(arrowLen > len * 0.4f)
		arrowLen = len * 0.4f;
	float angle = 30.0f * (pi / 180.0f);
	float ca = cosf(angle), sa = sinf(angle);
	vec2 leftDir = vec2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
	vec2 rightDir = vec2(dir.x * ca + dir.y * sa, -dir.x * sa + dir.y * ca);
	vec2 leftTip = p1 - leftDir * arrowLen;
	vec2 rightTip = p1 - rightDir * arrowLen;
	DrawSegment(p1, leftTip, col, halfWidth);
	DrawSegment(p1, rightTip, col, halfWidth);
}

void CEsp::EmitLine(const vec2 &p0, const vec2 &p1, int playerIndex) const
{
	// v1.56.210: per-segment gradient color. When Gradient is ON (and
	// Rainbow ON), each segment gets hue = baseHue + segIdx*step. The
	// counter starts at playerIndex so lines to different players get
	// different hues. Otherwise (no gradient / no rainbow) every segment
	// returns the same color (same as before).
	auto segCol = [&](int segIdx) -> ColorRGBA {
		ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_ESP, segIdx), true);
		c.a = KxLineAlpha(KX_LINE_ESP);
		return c;
	};
	int lineSize = KxLineSize(KX_LINE_ESP);
	float halfWidth = lineSize > 0 ? 0.5f + (float)(lineSize - 1) * 0.25f : 0.0f;

	int style = g_Config.m_KxEspStyle; // 0=Line 1=Arrow 2=Dotted 3=DottedArrow
	bool isDotted = (style == 2 || style == 3);
	bool hasArrow = (style == 1 || style == 3);

	if(g_Config.m_KxLineRenderingLayer > 0)
	{
		// Deferred: push to CKinetixLines queue.
		if(isDotted)
		{
			const float dash = 8.0f, gap = 6.0f, pattern = dash + gap;
			vec2 dir = p1 - p0;
			float totalLen = length(dir);
			if(totalLen < 0.001f)
				return;
			dir /= totalLen;
			float speedFactor = (float)g_Config.m_KxEspSpeed;
			float t = (float)GameClient()->Client()->GameTick(g_Config.m_ClDummy) / 50.0f;
			float animOffset = fmodf(t * speedFactor, pattern);
			if(animOffset < 0.0f)
				animOffset += pattern;
			float pos = animOffset; // v1.56.116: +animOffset = dashes move toward target (p1)
			int dashIdx = 0;
			while(pos < totalLen)
			{
				float dashStart = pos;
				float dashEnd = pos + dash;
				if(dashEnd > 0.0f && dashStart < totalLen)
				{
					if(dashStart < 0.0f) dashStart = 0.0f;
					if(dashEnd > totalLen) dashEnd = totalLen;
					vec2 segStart = p0 + dir * dashStart;
					vec2 segEnd = p0 + dir * dashEnd;
					KinetixEnqueueLine(segStart, segEnd, segCol(playerIndex + dashIdx), halfWidth);
					dashIdx++;
				}
				pos += pattern;
			}
			if(hasArrow)
			{
				vec2 adir = p1 - p0;
				float alen = length(adir);
				if(alen > 0.001f)
				{
					adir /= alen;
					float arrowLen = 16.0f; // fixed = half tile (32px tile / 2)
					if(arrowLen > alen * 0.4f) arrowLen = alen * 0.4f;
					float angle = 30.0f * (pi / 180.0f);
					float ca = cosf(angle), sa = sinf(angle);
					vec2 leftDir = vec2(adir.x * ca - adir.y * sa, adir.x * sa + adir.y * ca);
					vec2 rightDir = vec2(adir.x * ca + adir.y * sa, -adir.x * sa + adir.y * ca);
					vec2 leftTip = p1 - leftDir * arrowLen;
					vec2 rightTip = p1 - rightDir * arrowLen;
					KinetixEnqueueLine(p1, leftTip, segCol(playerIndex + dashIdx), halfWidth);
					KinetixEnqueueLine(p1, rightTip, segCol(playerIndex + dashIdx + 1), halfWidth);
				}
			}
		}
		else
		{
			KinetixEnqueueLine(p0, p1, segCol(playerIndex), halfWidth);
			if(hasArrow)
			{
				vec2 adir = p1 - p0;
				float alen = length(adir);
				if(alen > 0.001f)
				{
					adir /= alen;
					float arrowLen = 16.0f; // fixed = half tile (32px tile / 2)
					if(arrowLen > alen * 0.4f) arrowLen = alen * 0.4f;
					float angle = 30.0f * (pi / 180.0f);
					float ca = cosf(angle), sa = sinf(angle);
					vec2 leftDir = vec2(adir.x * ca - adir.y * sa, adir.x * sa + adir.y * ca);
					vec2 rightDir = vec2(adir.x * ca + adir.y * sa, -adir.x * sa + adir.y * ca);
					vec2 leftTip = p1 - leftDir * arrowLen;
					vec2 rightTip = p1 - rightDir * arrowLen;
					KinetixEnqueueLine(p1, leftTip, segCol(playerIndex + 1), halfWidth);
					KinetixEnqueueLine(p1, rightTip, segCol(playerIndex + 2), halfWidth);
				}
			}
		}
	}
	else
	{
		// Immediate mode.
		if(isDotted)
		{
			const float dash = 8.0f, gap = 6.0f, pattern = dash + gap;
			vec2 dir = p1 - p0;
			float totalLen = length(dir);
			if(totalLen < 0.001f)
				return;
			dir /= totalLen;
			float speedFactor = (float)g_Config.m_KxEspSpeed;
			float t = (float)GameClient()->Client()->GameTick(g_Config.m_ClDummy) / 50.0f;
			float animOffset = fmodf(t * speedFactor, pattern);
			if(animOffset < 0.0f)
				animOffset += pattern;
			float pos = animOffset; // v1.56.116: +animOffset = dashes move toward target (p1)
			int dashIdx = 0;
			while(pos < totalLen)
			{
				float dashStart = pos;
				float dashEnd = pos + dash;
				if(dashEnd > 0.0f && dashStart < totalLen)
				{
					if(dashStart < 0.0f) dashStart = 0.0f;
					if(dashEnd > totalLen) dashEnd = totalLen;
					vec2 segStart = p0 + dir * dashStart;
					vec2 segEnd = p0 + dir * dashEnd;
					DrawSegment(segStart, segEnd, segCol(playerIndex + dashIdx), halfWidth);
					dashIdx++;
				}
				pos += pattern;
			}
			if(hasArrow)
				DrawArrowhead(p0, p1, segCol(playerIndex + dashIdx), halfWidth);
		}
		else
		{
			DrawSegment(p0, p1, segCol(playerIndex), halfWidth);
			if(hasArrow)
				DrawArrowhead(p0, p1, segCol(playerIndex + 1), halfWidth);
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
		// DDNet's visible world region is [Center - W/2, Center - H/2] to
		// [Center + W/2, Center + H/2] where W,H = CalcScreenParams(aspect, zoom).
		// Convert screen pixel → normalized [0,1] → world offset from center.
		float sw = (float)Graphics()->ScreenWidth();
		float sh = (float)Graphics()->ScreenHeight();
		float aspect = Graphics()->ScreenAspect();
		float worldW, worldH;
		Graphics()->CalcScreenParams(aspect, pGame->m_Camera.m_Zoom, &worldW, &worldH);
		float nx = (float)g_Config.m_KxEspScreenX / sw;
		float ny = (float)g_Config.m_KxEspScreenY / sh;
		pointA.x = pGame->m_Camera.m_Center.x + (nx - 0.5f) * worldW;
		pointA.y = pGame->m_Camera.m_Center.y + (ny - 0.5f) * worldH;
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
		EmitLine(pointA, target, i);
	}
}
