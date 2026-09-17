// (c) Kinetix. ESP component — see esp.h for overview.

#include <game/client/components/kinetix/esp.h>

#include <base/color.h>
#include <base/math.h>
#include <stdlib.h>
#include <cmath>
#include <engine/shared/config.h>
#include <engine/graphics.h>
#include <game/client/gameclient.h>
#include <game/client/components/camera.h>
#include <game/client/components/kinetix/kinetix_internal.h>
#include <game/client/components/kinetix/kinetix_lines.h>

#include <generated/protocol.h>
#include <game/gamecore.h> // v1.56.256: CCharacterCore::PhysicalSize() for hitbox sizes

void CEsp::OnReset()
{
}

// v1.56.257: shared filter body — callers pass their own config filter values.
bool CEsp::PassesFiltersImpl(int ClientId, int LocalId, int teamFilter, int ddraceTeamFilter, int ddraceTeamId, int friendFilter, int dummyFilter, int clanmateFilter, int targetsFilterMode, const char *pTargetsIds, int freezeFilter) const
{
	CGameClient *pGame = GameClient();
	if(!pGame->m_aClients[ClientId].m_Active)
		return false;
	if(!pGame->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	// Active dummy itself is never an ESP target.
	if(ClientId == LocalId)
		return false;

	// Team filter: 0=both 1=my 2=war 3=blue 4=red (v1.56.275).
	// "Team" = REAL team (red/blue); My/War compare real team with ours.
	const int MyRealTeam = pGame->m_aClients[LocalId].m_Team;
	if(teamFilter == 1)
	{
		if(pGame->m_aClients[ClientId].m_Team != MyRealTeam)
			return false;
	}
	else if(teamFilter == 2)
	{
		if(pGame->m_aClients[ClientId].m_Team == MyRealTeam)
			return false;
	}
	else if(teamFilter == 3)
	{
		if(pGame->m_aClients[ClientId].m_Team != TEAM_BLUE)
			return false;
	}
	else if(teamFilter == 4)
	{
		if(pGame->m_aClients[ClientId].m_Team != TEAM_RED)
			return false;
	}
	// DDrace team filter: 0=both 1=my 2=other 3=target id (v1.56.275)
	if(ddraceTeamFilter == 1 && !pGame->m_Teams.SameTeam(LocalId, ClientId))
		return false;
	if(ddraceTeamFilter == 2 && pGame->m_Teams.SameTeam(LocalId, ClientId))
		return false;
	if(ddraceTeamFilter == 3 && pGame->m_Teams.Team(ClientId) != ddraceTeamId)
		return false;
	// Friend filter: 0=both 1=ignore 2=only
	if(friendFilter == 1 && pGame->m_aClients[ClientId].m_Friend)
		return false;
	if(friendFilter == 2 && !pGame->m_aClients[ClientId].m_Friend)
		return false;
	// Dummy filter: 0=both 1=ignore 2=only
	bool isDummy = false;
	for(int d = 0; d < MAX_DUMMIES; d++)
	{
		if(pGame->m_aLocalIds[d] == ClientId)
		{
			isDummy = true;
			break;
		}
	}
	if(dummyFilter == 1 && isDummy)
		return false;
	if(dummyFilter == 2 && !isDummy)
		return false;
	if(clanmateFilter == 1 && str_comp(pGame->m_aClients[ClientId].m_aClan, pGame->m_aClients[LocalId].m_aClan) == 0)
		return false;
	if(clanmateFilter == 2 && str_comp(pGame->m_aClients[ClientId].m_aClan, pGame->m_aClients[LocalId].m_aClan) != 0)
		return false;
	if(targetsFilterMode != 0)
	{
		bool found = false;
		bool anyValid = false;
		const char *pIds = pTargetsIds;
		while(*pIds)
		{
			while(*pIds && (*pIds == ' ' || *pIds == ',' || *pIds == ';'))
				pIds++;
			if(!*pIds)
				break;
			if(*pIds >= '0' && *pIds <= '9')
			{
				const int id = atoi(pIds);
				if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
				{
					anyValid = true;
					if(id == ClientId)
						found = true;
				}
			}
			while(*pIds && *pIds != ' ' && *pIds != ',' && *pIds != ';')
				pIds++;
		}
		if(targetsFilterMode == 1 && anyValid && !found)
			return false;
		if(targetsFilterMode == 2 && anyValid && found)
			return false;
	}
	// Freeze filter: 0=both 1=freeze 2=no_freeze (LiveFrozen covers servers
	// like FNG that hide the unfreeze tick — same check as the freeze flakes)
	bool isFrozen = pGame->m_aClients[ClientId].m_FreezeEnd != 0 ||
		pGame->m_aClients[ClientId].m_LiveFrozen;
	// v1.56.275: FNG Freeze — ninja counts as frozen (FNG does not sync freeze).
	if(g_Config.m_KxEspFngFreeze && pGame->m_Snap.m_aCharacters[ClientId].m_Cur.m_Weapon == WEAPON_NINJA)
		isFrozen = true;
	if(freezeFilter == 1 && !isFrozen)
		return false;
	if(freezeFilter == 2 && isFrozen)
		return false;
	return true;
}

bool CEsp::PassesFilters(int ClientId, int LocalId) const
{
	return PassesFiltersImpl(ClientId, LocalId, g_Config.m_KxEspTeamFilter, g_Config.m_KxEspDDraceTeam, g_Config.m_KxEspDDraceTeamId, g_Config.m_KxEspFriendFilter, g_Config.m_KxEspDummyFilter, g_Config.m_KxEspClanmateFilter, g_Config.m_KxEspTargetsFilterMode, g_Config.m_KxEspTargetsFilter, g_Config.m_KxEspFreezeFilter);
}

bool CEsp::PassesHitboxesFilters(int ClientId, int LocalId) const
{
	return PassesFiltersImpl(ClientId, LocalId, g_Config.m_KxHitboxesTeamFilter, g_Config.m_KxHitboxesDDraceTeam, g_Config.m_KxHitboxesDDraceTeamId, g_Config.m_KxHitboxesFriendFilter, g_Config.m_KxHitboxesDummyFilter, g_Config.m_KxHitboxesClanmateFilter, g_Config.m_KxHitboxesTargetsFilterMode, g_Config.m_KxHitboxesTargetsFilter, g_Config.m_KxHitboxesFreezeFilter);
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

// v1.56.254: square box outline around a target player (Box Style).
// 0=Box — solid outline; 1=Dotted box — dashes move along the outline,
// animated with Box Speed. Same line color/size/opacity pipeline as EmitLine
// (per-component ESP line settings + rainbow gradient), same deferred
// (Layer > 0) vs immediate rendering split.
void CEsp::EmitBox(const vec2 &center, int playerIndex) const
{
	auto boxCol = [&](int segIdx) -> ColorRGBA {
		ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_ESP, segIdx), true);
		c.a = KxLineAlpha(KX_LINE_ESP);
		return c;
	};
	int lineSize = KxLineSize(KX_LINE_ESP);
	float halfWidth = lineSize > 0 ? 0.5f + (float)(lineSize - 1) * 0.25f : 0.0f;

	// v1.56.256: X/Y Box scale — per-axis size scaling (box dimensions, not line width).
	const float halfX = 32.0f * (float)g_Config.m_KxEspBoxScaleX / 100.0f; // 100% = 64px wide
	const float halfY = 32.0f * (float)g_Config.m_KxEspBoxScaleY / 100.0f; // 100% = 64px tall
	const vec2 corners[4] = {
		vec2(center.x - halfX, center.y - halfY),
		vec2(center.x + halfX, center.y - halfY),
		vec2(center.x + halfX, center.y + halfY),
		vec2(center.x - halfX, center.y + halfY),
	};

	const bool dotted = g_Config.m_KxEspBoxStyle == 1;
	const float dash = 8.0f, gap = 6.0f, pattern = dash + gap;
	float animOffset = 0.0f;
	if(dotted)
	{
		float speedFactor = (float)g_Config.m_KxEspBoxSpeed;
		float t = (float)GameClient()->Client()->GameTick(g_Config.m_ClDummy) / 50.0f;
		animOffset = fmodf(t * speedFactor, pattern);
		if(animOffset < 0.0f)
			animOffset += pattern;
	}

	for(int e = 0; e < 4; e++)
	{
		const vec2 &p0 = corners[e];
		const vec2 &p1 = corners[(e + 1) % 4];
		if(!dotted)
		{
			if(g_Config.m_KxLineRenderingLayer > 0)
				KinetixEnqueueLine(p0, p1, boxCol(playerIndex + e), halfWidth);
			else
				DrawSegment(p0, p1, boxCol(playerIndex + e), halfWidth);
			continue;
		}
		vec2 dir = p1 - p0;
		const float len = length(dir);
		if(len < 0.001f)
			continue;
		dir /= len;
		float pos = animOffset; // dashes flow along the outline direction
		int dashIdx = 0;
		while(pos < len)
		{
			float dashStart = pos;
			float dashEnd = pos + dash;
			if(dashEnd > 0.0f && dashStart < len)
			{
				if(dashStart < 0.0f)
					dashStart = 0.0f;
				if(dashEnd > len)
					dashEnd = len;
				vec2 segStart = p0 + dir * dashStart;
				vec2 segEnd = p0 + dir * dashEnd;
				if(g_Config.m_KxLineRenderingLayer > 0)
					KinetixEnqueueLine(segStart, segEnd, boxCol(playerIndex + e * 8 + dashIdx), halfWidth);
				else
					DrawSegment(segStart, segEnd, boxCol(playerIndex + e * 8 + dashIdx), halfWidth);
				dashIdx++;
			}
			pos += pattern;
		}
	}
}

// v1.56.256: Hitboxes — 28x28 tile-collision square and the round trigger
// hitbox (radius 28) drawn on the active dummy; with Show for others, also
// on other players (optionally filtered like ESP).
void CEsp::EmitHitboxes(int LocalId) const
{
	if(!g_Config.m_KxHitboxes) // v1.56.257: master toggle on the Hitboxes header.
		return;
	CGameClient *pGame = GameClient();
	const bool tile = g_Config.m_KxHitboxesTeeTile != 0;
	const bool trigger = g_Config.m_KxHitboxesTeeTrigger != 0;
	if(!tile && !trigger)
		return;

	// Engine hitbox sizes (gamecore.h): tile collision is a 28x28 square, the
	// projectile/hook trigger hitbox is a circle of radius PhysicalSize().
	const float halfTile = CCharacterCore::PhysicalSize() * 0.5f;
	const float triggerRadius = CCharacterCore::PhysicalSize();
	auto draw = [&](int clientId) {
		const vec2 &pos = pGame->m_aClients[clientId].m_RenderPos;
		if(tile)
			EmitRectShape(pos, halfTile, halfTile, g_Config.m_KxHitboxesTileStyle, g_Config.m_KxHitboxesTileSpeed, clientId);
		if(trigger)
			EmitCircleShape(pos, triggerRadius, g_Config.m_KxHitboxesTriggerStyle, g_Config.m_KxHitboxesTriggerSpeed, clientId);
	};

	// Active dummy.
	if(pGame->m_Snap.m_aCharacters[LocalId].m_Active)
		draw(LocalId);

	// Other players (Show for others, optionally filtered like ESP).
	if(!g_Config.m_KxHitboxesShowOthers)
		return;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId || !pGame->m_aClients[i].m_Active || !pGame->m_Snap.m_aCharacters[i].m_Active)
			continue;
		if(!PassesHitboxesFilters(i, LocalId))
			continue;
		draw(i);
	}
}

// v1.56.256: Hitboxes shape emitters. Style: 0=Filled (always drawn immediately —
// the deferred queue is lines-only), 1=Box (outline), 2=Dotted Box (dashes flow
// along the outline, animated with `speed`). Outlines honor the line layer.
void CEsp::EmitRectShape(const vec2 &center, float halfW, float halfH, int style, int speed, int playerIndex) const
{
	auto col = [&](int segIdx) -> ColorRGBA {
		ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_HITBOXES, segIdx), true);
		c.a = KxLineAlpha(KX_LINE_HITBOXES);
		return c;
	};
	int lineSize = KxLineSize(KX_LINE_HITBOXES);
	float halfWidth = lineSize > 0 ? 0.5f + (float)(lineSize - 1) * 0.25f : 0.0f;

	if(style == 0)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		const ColorRGBA fill = col(playerIndex);
		Graphics()->SetColor(fill.r, fill.g, fill.b, fill.a);
		IGraphics::CQuadItem q(center.x - halfW, center.y - halfH, halfW * 2.0f, halfH * 2.0f);
		Graphics()->QuadsDrawTL(&q, 1); // v1.56.257: TL variant — QuadsDraw() centers on (x, y)
		Graphics()->QuadsEnd();
		return;
	}

	const vec2 corners[4] = {
		vec2(center.x - halfW, center.y - halfH),
		vec2(center.x + halfW, center.y - halfH),
		vec2(center.x + halfW, center.y + halfH),
		vec2(center.x - halfW, center.y + halfH),
	};
	const bool dotted = style == 2;
	const float dash = 8.0f, gap = 6.0f, pattern = dash + gap;
	float animOffset = 0.0f;
	if(dotted)
	{
		float t = (float)GameClient()->Client()->GameTick(g_Config.m_ClDummy) / 50.0f;
		animOffset = fmodf(t * (float)speed, pattern);
		if(animOffset < 0.0f)
			animOffset += pattern;
	}
	for(int e = 0; e < 4; e++)
	{
		const vec2 &p0 = corners[e];
		const vec2 p1 = corners[(e + 1) % 4];
		vec2 dir = p1 - p0;
		const float len = length(dir);
		if(len < 0.001f)
			continue;
		dir /= len;
		if(!dotted)
		{
			if(g_Config.m_KxLineRenderingLayer > 0)
				KinetixEnqueueLine(p0, p1, col(playerIndex + e), halfWidth);
			else
				DrawSegment(p0, p1, col(playerIndex + e), halfWidth);
			continue;
		}
		float pos = animOffset;
		int dashIdx = 0;
		while(pos < len)
		{
			float dashStart = pos;
			float dashEnd = pos + dash;
			if(dashEnd > 0.0f && dashStart < len)
			{
				if(dashStart < 0.0f)
					dashStart = 0.0f;
				if(dashEnd > len)
					dashEnd = len;
				vec2 segStart = p0 + dir * dashStart;
				vec2 segEnd = p0 + dir * dashEnd;
				if(g_Config.m_KxLineRenderingLayer > 0)
					KinetixEnqueueLine(segStart, segEnd, col(playerIndex + e * 8 + dashIdx), halfWidth);
				else
					DrawSegment(segStart, segEnd, col(playerIndex + e * 8 + dashIdx), halfWidth);
				dashIdx++;
			}
			pos += pattern;
		}
	}
}

void CEsp::EmitCircleShape(const vec2 &center, float radius, int style, int speed, int playerIndex) const
{
	auto col = [&](int segIdx) -> ColorRGBA {
		ColorRGBA c = ColorRGBA(KxLineColorAt(KX_LINE_HITBOXES, segIdx), true);
		c.a = KxLineAlpha(KX_LINE_HITBOXES);
		return c;
	};
	int lineSize = KxLineSize(KX_LINE_HITBOXES);
	float halfWidth = lineSize > 0 ? 0.5f + (float)(lineSize - 1) * 0.25f : 0.0f;

	if(style == 0)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		const ColorRGBA fill = col(playerIndex);
		Graphics()->SetColor(fill.r, fill.g, fill.b, fill.a);
		Graphics()->DrawCircle(center.x, center.y, radius, 32);
		Graphics()->QuadsEnd();
		return;
	}

	const bool dotted = style == 2;
	const float dash = 8.0f, gap = 6.0f, pattern = dash + gap;
	float animOffset = 0.0f;
	if(dotted)
	{
		float t = (float)GameClient()->Client()->GameTick(g_Config.m_ClDummy) / 50.0f;
		animOffset = fmodf(t * (float)speed, pattern);
		if(animOffset < 0.0f)
			animOffset += pattern;
	}

	const float arc = 2.0f * pi * radius;
	if(!dotted)
	{
		// Solid outline: chords between sampled points (sagitta < 0.3 units at r=28).
		const int segments = 24;
		vec2 prev = center + vec2(radius, 0.0f);
		for(int i = 1; i <= segments; i++)
		{
			const float a = (float)i / (float)segments * 2.0f * pi;
			const vec2 cur = center + vec2(cosf(a), sinf(a)) * radius;
			if(g_Config.m_KxLineRenderingLayer > 0)
				KinetixEnqueueLine(prev, cur, col(playerIndex + i - 1), halfWidth);
			else
				DrawSegment(prev, cur, col(playerIndex + i - 1), halfWidth);
			prev = cur;
		}
		return;
	}

	// Dotted: dashes flow along the outline (arc-length parameterized chords).
	float pos = animOffset;
	int dashIdx = 0;
	while(pos < arc)
	{
		float s0 = pos;
		float s1 = pos + dash;
		if(s1 > 0.0f && s0 < arc)
		{
			if(s0 < 0.0f)
				s0 = 0.0f;
			if(s1 > arc)
				s1 = arc;
			const float a0 = s0 / arc * 2.0f * pi;
			const float a1 = s1 / arc * 2.0f * pi;
			const vec2 p0 = center + vec2(cosf(a0), sinf(a0)) * radius;
			const vec2 p1 = center + vec2(cosf(a1), sinf(a1)) * radius;
			if(g_Config.m_KxLineRenderingLayer > 0)
				KinetixEnqueueLine(p0, p1, col(playerIndex + dashIdx), halfWidth);
			else
				DrawSegment(p0, p1, col(playerIndex + dashIdx), halfWidth);
			dashIdx++;
		}
		pos += pattern;
	}
}

void CEsp::OnRender()
{
	CGameClient *pGame = GameClient();
	int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return;

	// v1.56.256: Hitboxes component — runs independently of the ESP master toggle.
	EmitHitboxes(LocalId);

	if(!g_Config.m_KxEsp)
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
	// v1.56.254: each target also gets a box outline (Box Style/Box Speed).
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!PassesFilters(i, LocalId))
			continue;
		vec2 target = pGame->m_aClients[i].m_RenderPos;
		// v1.56.256: Pointer/Box visibility toggles.
		if(g_Config.m_KxEspPointer)
			EmitLine(pointA, target, i);
		if(g_Config.m_KxEspBox)
			EmitBox(target, i);
	}
}
