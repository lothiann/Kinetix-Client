-- NAME: HUD Info
-- VERSION: 1.0
-- AUTHOR: Kinetix
-- DESCRIPTION: Info overlay: game tick and time, screen and camera, mouse position, modifier keys, player count, map size and race time.

while true do
    gfx.map_screen(0, 0, 300 * gfx.aspect(), 300)

    text.color(1, 1, 1, 1)
    text.draw(5, 5, 6, string.format("tick %d  global %.1fs  frame %.1fms", cl.GameTick(), cl.GlobalTime(), cl.RenderFrameTime() * 1000))
    text.draw(5, 14, 6, string.format("screen %dx%d  zoom %.2f", gfx.width(), gfx.height(), gc.m_Camera.m_Zoom))

    local cam = gc.m_Camera.m_Center
    text.draw(5, 23, 6, string.format("cam %.0f,%.0f", cam.x, cam.y))

    local mp = input.mouse_pos()
    local mods = (input.shift_pressed() and "S" or "-") .. (input.alt_pressed() and "A" or "-") .. (input.modifier_pressed() and "C" or "-")
    text.draw(5, 32, 6, string.format("mouse %.0f,%.0f  mods %s", mp.x, mp.y, mods))

    if input.pressed(input.KEY_SPACE) then
        text.color(0.3, 1, 0.3, 1)
        text.draw(5, 41, 6, "SPACE held")
    end

    local n = 0
    for i = 0, CONST.MAX_CLIENTS - 1 do
        local c = gc.m_aClients[i]
        if c and c.m_Active then n = n + 1 end
    end
    local layers = gc:Layers()
    local gl = layers and layers:GameLayer()
    local dims = gl and string.format("%dx%d", gl.m_Width, gl.m_Height) or "?"
    text.color(1, 0.8, 0.3, 1)
    text.draw(5, 50, 6, string.format("players %d  map %s  race %.1fs", n, dims, gc:CurrentRaceTime()))

    coroutine.yield()
end
