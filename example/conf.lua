function love.conf(t)
    t.window.title = "Rive + Love2D"
    t.window.width = 800
    t.window.height = 600
    t.window.resizable = true
    t.window.minwidth = 400
    t.window.minheight = 300
    t.window.stencil = true  -- required for Rive clipping

    t.modules.joystick = false
    t.modules.physics = false
end
