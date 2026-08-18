// (c) Kinetix. Deferred line-render overlay — see kinetix_lines.h for overview.

#include <game/client/components/kinetix/kinetix_lines.h>
#include <game/client/components/kinetix/kinetix_internal.h> // KxLineUpdateRainbow

#include <engine/graphics.h>
#include <game/client/gameclient.h>
#include <game/client/components/camera.h>

#include <vector>

namespace
{
        // Deferred segment queue. Main-thread only (all callers run in OnRender).
        // Cleared once per frame by CKinetixLines::OnRender after drawing.
        std::vector<SKinetixLineSegment> g_DeferredLines;
}

// v1.56.201: Per-component rainbow hue state (defined here, declared in kinetix_internal.h).
float g_aLineRainbowHue[KX_LINE_COUNT] = {};

void KinetixEnqueueLine(const vec2 &p0, const vec2 &p1, ColorRGBA color, float halfWidth)
{
        g_DeferredLines.push_back({p0, p1, color, halfWidth});
}

void CKinetixLines::OnReset()
{
        g_DeferredLines.clear();
}

void CKinetixLines::OnRender()
{
        // v1.56.204: update per-component rainbow hue each frame.
        // Use RenderFrameTime() for framerate-independent rotation (same as Rainbow Color in clickgui.cpp).
        // The old fixed 1/60 dt caused speed to scale with FPS (100x too fast at 6000fps).
        KxLineUpdateRainbow(Client()->RenderFrameTime());

        if(g_DeferredLines.empty())
                return;

        // Set up world projection (same as aimbot/ESP use) so the enqueued
        // world-space segments land on the correct screen coordinates.
        CGameClient *pGame = GameClient();
        Graphics()->MapScreenToInterface(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y, pGame->m_Camera.m_Zoom);

        Graphics()->TextureClear();

        // Two passes: thick quads (halfWidth > 0) then thin lines (halfWidth == 0),
        // to minimise Graphics state switches.
        Graphics()->QuadsBegin();
        for(const auto &seg : g_DeferredLines)
        {
                if(seg.halfWidth <= 0.0f)
                        continue;
                vec2 dir = seg.p1 - seg.p0;
                float len = length(dir);
                if(len < 0.001f)
                        continue;
                dir /= len;
                vec2 perp = vec2(dir.y, -dir.x) * seg.halfWidth;
                IGraphics::CFreeformItem q(
                        seg.p0.x - perp.x, seg.p0.y - perp.y, seg.p0.x + perp.x, seg.p0.y + perp.y,
                        seg.p1.x - perp.x, seg.p1.y - perp.y, seg.p1.x + perp.x, seg.p1.y + perp.y);
                Graphics()->SetColor(seg.color.r, seg.color.g, seg.color.b, seg.color.a);
                Graphics()->QuadsDrawFreeform(&q, 1);
        }
        Graphics()->QuadsEnd();

        Graphics()->LinesBegin();
        for(const auto &seg : g_DeferredLines)
        {
                if(seg.halfWidth > 0.0f)
                        continue;
                Graphics()->SetColor(seg.color.r, seg.color.g, seg.color.b, seg.color.a);
                IGraphics::CLineItem line(seg.p0.x, seg.p0.y, seg.p1.x, seg.p1.y);
                Graphics()->LinesDraw(&line, 1);
        }
        Graphics()->LinesEnd();

        g_DeferredLines.clear();
}
