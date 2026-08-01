// (c) Kinetix. Deferred line-render overlay.
//
// Line rendering layer system (v1.56.104):
//   Components that draw world-space lines (aimbot FOV/radius, ESP, trajectory,
//   laser unfreeze) normally render in their own OnRender(). The registration
//   order in CGameClient places most of them BEFORE m_MapLayersForeground, so
//   their lines end up BEHIND foreground map tiles (blocks).
//
//   g_Config.m_KxLineRenderingLayer lets the user fix this:
//     Layer == 0 → render immediately (current behavior, behind fg blocks).
//     Layer >= 1 → push segments to a deferred queue; CKinetixLines renders
//                  them AFTER m_MapLayersForeground (on top of blocks).
//
//   Line-rendering components call KinetixEnqueueLine() with pre-computed
//   segment data (endpoints + color + half-width). CKinetixLines::OnRender
//   drains the queue once per frame with the world projection set up.
//
//   halfWidth == 0  → 1px thin line (Graphics()->LinesBegin).
//   halfWidth > 0  → quad of the given half-width (Graphics()->QuadsBegin).

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_KINETIX_LINES_H
#define GAME_CLIENT_COMPONENTS_KINETIX_KINETIX_LINES_H

#include <base/color.h>
#include <base/vmath.h>

#include <game/client/component.h>

// Deferred segment pushed by line-rendering components when Layer > 0.
struct SKinetixLineSegment
{
        vec2 p0;
        vec2 p1;
        ColorRGBA color;
        float halfWidth; // 0 = thin 1px line, >0 = quad half-width
};

// Push a segment to the deferred queue (main thread only). Called by
// aimbot/ESP/trajectory/laser when g_Config.m_KxLineRenderingLayer > 0.
void KinetixEnqueueLine(const vec2 &p0, const vec2 &p1, ColorRGBA color, float halfWidth);

// CKinetixLines — registered AFTER m_MapLayersForeground so its OnRender runs
// after foreground map tiles are drawn. Drains the deferred queue each frame.
class CKinetixLines : public CComponent
{
public:
        CKinetixLines() = default;
        ~CKinetixLines() override = default;

        int Sizeof() const override { return sizeof(*this); }
        void OnReset() override;
        void OnRender() override;
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_KINETIX_LINES_H
