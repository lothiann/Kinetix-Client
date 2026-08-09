#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

void CBotNet::RenderPackage(int Dummy)
{
        if(!g_Config.m_KxKinodynamic)
                return;
        if(Dummy < 0 || Dummy >= MAX_DUMMIES)
                return;

        const CKinodynamicCache &cache = m_aDummies[Dummy].m_KinoCache;
        if(!cache.IsValid() || cache.vPath.size() < 2)
                return;

        // Render kinodynamic predicted path — use fixed alpha, independent of trajectory settings
        Graphics()->TextureClear();

        const ColorRGBA KinoColor = ColorRGBA(0.2f, 1.0f, 0.4f, 1.0f);
        const float PathAlpha = 0.8f;
        const float HalfWidth = 1.5f;

        {
                std::vector<IGraphics::CFreeformItem> vQuads;
                vQuads.reserve(cache.vPath.size() - 1);

                for(size_t i = 1; i < cache.vPath.size(); i++)
                {
                        vec2 p0 = cache.vPath[i - 1];
                        vec2 p1 = cache.vPath[i];
                        vec2 Dir = normalize(p1 - p0);
                        vec2 Perp = vec2(Dir.y, -Dir.x) * HalfWidth;

                        vQuads.emplace_back(
                                p0.x - Perp.x, p0.y - Perp.y,
                                p0.x + Perp.x, p0.y + Perp.y,
                                p1.x - Perp.x, p1.y - Perp.y,
                                p1.x + Perp.x, p1.y + Perp.y);
                }

                Graphics()->QuadsBegin();
                for(size_t i = 0; i < vQuads.size(); i++)
                {
                        float t = (float)(i + 1) / (float)cache.vPath.size();
                        float Alpha = PathAlpha * (1.0f - t * 0.7f);
                        Graphics()->SetColor(KinoColor.r, KinoColor.g, KinoColor.b, Alpha);
                        Graphics()->QuadsDrawFreeform(&vQuads[i], 1);
                }
                Graphics()->QuadsEnd();
        }
}

void CBotNet::RenderVectorField()
{
        if(!g_Config.m_KxKinodynamic || !g_Config.m_KxAtkPathfinder || !m_MapGridLoaded)
                return;
        if(!m_pfDist)
                return;

        CGameClient *pGame = GameClient();

        // Get camera position for viewport culling
        float CamX = pGame->m_Camera.m_Center.x;
        float CamY = pGame->m_Camera.m_Center.y;
        float CamW = pGame->m_Camera.m_Zoom * Graphics()->ScreenWidth() / 2.0f;
        float CamH = pGame->m_Camera.m_Zoom * Graphics()->ScreenHeight() / 2.0f;

        int minTX = pf_max(0, (int)((CamX - CamW) / 32.0f) - 1);
        int maxTX = pf_min(m_MapWidth - 1, (int)((CamX + CamW) / 32.0f) + 1);
        int minTY = pf_max(0, (int)((CamY - CamH) / 32.0f) - 1);
        int maxTY = pf_min(m_MapHeight - 1, (int)((CamY + CamH) / 32.0f) + 1);

        // Sample every 2 tiles for performance
        int step = 2;

        Graphics()->TextureClear();
        Graphics()->LinesBegin();

        for(int ty = minTY; ty <= maxTY; ty += step)
        {
                for(int tx = minTX; tx <= maxTX; tx += step)
                {
                        int idx = ty * m_MapWidth + tx;
                        if(m_pfDist[idx] >= 1e17f)
                                continue;
                        if(!IsTileWalkable(tx, ty))
                                continue;

                        float currentD = m_pfDist[idx];

                        // Find the neighbor with the smallest distance (gradient direction)
                        float bestDx = 0, bestDy = 0;
                        float bestDist = currentD;
                        int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
                        int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

                        for(int d = 0; d < 8; d++)
                        {
                                int nr = ty + dr[d];
                                int nc = tx + dc[d];
                                if(nr < 0 || nc < 0 || nr >= m_MapHeight || nc >= m_MapWidth)
                                        continue;
                                int nIdx = nr * m_MapWidth + nc;
                                if(m_pfDist[nIdx] < bestDist)
                                {
                                        bestDist = m_pfDist[nIdx];
                                        bestDx = (float)(nc - tx);
                                        bestDy = (float)(nr - ty);
                                }
                        }

                        if(bestDx == 0 && bestDy == 0)
                                continue;

                        float len = sqrtf(bestDx * bestDx + bestDy * bestDy);
                        if(len < 0.001f)
                                continue;

                        bestDx /= len;
                        bestDy /= len;

                        float cx = tx * 32.0f + 16.0f;
                        float cy = ty * 32.0f + 16.0f;
                        float arrowLen = 12.0f;

                        vec2 start(cx, cy);
                        vec2 end(cx + bestDx * arrowLen, cy + bestDy * arrowLen);

                        // Color based on distance: close to target = green, far = yellow
                        float maxVisDist = 100.0f;
                        float ratio = pf_min(currentD / maxVisDist, 1.0f);
                        Graphics()->SetColor(1.0f, 1.0f - ratio * 0.5f, 0.0f, 0.4f);
                        IGraphics::CLineItem Line(start, end);
                        Graphics()->LinesDraw(&Line, 1);
                }
        }

        Graphics()->LinesEnd();
}
