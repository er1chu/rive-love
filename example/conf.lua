function love.conf(t)
    t.window.title = "Rive + Love2D"
    t.window.width = 500
    t.window.height = 500
    t.window.resizable = true
    t.window.minwidth = 400
    t.window.minheight = 300
    t.window.stencil = true  -- required for Rive clipping
    t.window.msaa = 4        -- anti-aliasing for smoother edges

    t.modules.joystick = false
    t.modules.physics = false
end
