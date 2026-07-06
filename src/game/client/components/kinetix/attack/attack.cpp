#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

bool CBotNet::HasLineOfSightTiles(int r1, int c1, int r2, int c2)
{
        float x1 = c1 + 0.5f, y1 = r1 + 0.5f;
        float x2 = c2 + 0.5f, y2 = r2 + 0.5f;
        float d = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
        int steps = pf_max((int)(d * 3.0f), 1);
        int prevR = -1, prevC = -1;

        for(int i = 0; i <= steps; i++)
        {
                float t = (float)i / steps;
                float x = x1 + (x2 - x1) * t;
                float y = y1 + (y2 - y1) * t;
                int r = (int)y, c = (int)x;
                if(r == prevR && c == prevC)
                        continue;
                if(!IsTileWalkable(c, r))
                        return false;
                if(prevR >= 0 && abs(r - prevR) + abs(c - prevC) == 2)
                {
                        if(!IsTileWalkable(c, prevR) && !IsTileWalkable(prevC, r))
                                return false;
                }
                prevR = r;
                prevC = c;
        }
        return true;
}

// =========================================================
// SHARED A* ENGINE (v1.56.32 refactor)
// One implementation of the 8-directional Dijkstra/A* loop, used by both
// ComputePathfinder (single source = target tile, freeze passable) and
// ComputePathfinderRescue (multi-source = safe tiles, freeze impassable).
// =========================================================
