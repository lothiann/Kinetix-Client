-- NAME: Tile Highlight
-- VERSION: 1.0
-- AUTHOR: Kinetix
-- DESCRIPTION: Colored highlight of the tile under the aim cursor (death = red, freeze = blue, solid = yellow, free = green). Renders on the same layer with the same zoom math as the pathfinder tile editor.

local D = config.m_ClDummy
local TS = 32

while true do
    local cam = gc.m_Camera.m_Center
    local zoom = gc.m_Camera.m_Zoom
    local target = gc.m_Controls.m_aTargetPos[D]

    local aimx = cam.x + (target.x - cam.x) * zoom
    local aimy = cam.y + (target.y - cam.y) * zoom
    local tx = math.floor(aimx / TS)
    local ty = math.floor(aimy / TS)

    gfx.texture_clear()
    gfx.map_screen_to_interface(cam.x, cam.y, zoom)

    local col = gc:Collision()
    local game = col and col:GetTile(tx, ty) or -1
    local solid = col and col:IsSolid(tx, ty) or -1

    if game == 2 then
        gfx.draw_rect(tx * TS, ty * TS, TS, TS, 1, 0.15, 0.1, 0.55)
    elseif game == 9 then
        gfx.draw_rect(tx * TS, ty * TS, TS, TS, 0.25, 0.55, 1, 0.55)
    elseif solid == 1 then
        gfx.draw_rect(tx * TS, ty * TS, TS, TS, 1, 1, 0, 0.4)
    else
        gfx.draw_rect(tx * TS, ty * TS, TS, TS, 0, 1, 0, 0.35)
    end

    gfx.draw_line(tx * TS, ty * TS, (tx + 1) * TS, ty * TS, 0, 0, 0, 0.7)
    gfx.draw_line((tx + 1) * TS, ty * TS, (tx + 1) * TS, (ty + 1) * TS, 0, 0, 0, 0.7)
    gfx.draw_line((tx + 1) * TS, (ty + 1) * TS, tx * TS, (ty + 1) * TS, 0, 0, 0, 0.7)
    gfx.draw_line(tx * TS, (ty + 1) * TS, tx * TS, ty * TS, 0, 0, 0, 0.7)

    gfx.map_screen(0, 0, 300 * gfx.aspect(), 300)
    text.color(1, 1, 1, 1)
    text.draw(10, 10, 8, string.format("tile %d,%d  game=%d  solid=%d  zoom=%.2f", tx, ty, game, solid, zoom))

    local n = 0
    for i = 0, 63 do
        local c = gc.m_aClients[i]
        if c and c.m_Active then n = n + 1 end
    end
    local layers = gc:Layers()
    local gl = layers and layers:GameLayer()
    local dims = gl and string.format("%dx%d", gl.m_Width, gl.m_Height) or "?"
    text.color(1, 0.8, 0.3, 1)
    text.draw(10, 22, 8, string.format("players=%d  map=%s  race=%ds", n, dims, gc:CurrentRaceTime()))

    coroutine.yield()
end
