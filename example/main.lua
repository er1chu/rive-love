local rive = require("rive_love")

local file, scene

function love.load()
    file = rive.load("fire_button.riv")

    -- Enumerate artboards
    print("Artboards:")
    for _, ab in ipairs(file:artboards()) do
        print(("  [%d] %s"):format(ab.index, ab.name))
    end

    -- Create scene (default artboard, first state machine)
    scene = file:stateMachine(0, 0)

    -- Enumerate animations and state machines on this artboard
    print("Animations:")
    for _, a in ipairs(scene:animations()) do
        print(("  [%d] %s"):format(a.index, a.name))
    end

    print("State machines:")
    for _, sm in ipairs(scene:stateMachines()) do
        print(("  [%d] %s"):format(sm.index, sm.name))
    end

    -- Enumerate state machine inputs
    print("Inputs:")
    for _, input in ipairs(scene:inputs()) do
        print(("  [%d] %s"):format(input.index, input.name))
    end

    print(("Artboard size: %g x %g"):format(scene:width(), scene:height()))
end

function love.update(dt)
    if scene then
        scene:advance(dt)
    end
end

function love.draw()
    love.graphics.clear(0.15, 0.15, 0.15, 1, true, true)

    if scene then
        scene:drawFit()
    end

    love.graphics.setColor(1, 1, 1)
    love.graphics.print("FPS: " .. love.timer.getFPS(), 10, 10)
end

function love.mousepressed(x, y, button)
    if scene and button == 1 then
        local ax, ay = scene:screenToArtboard(x, y)
        if ax then scene:pointerDown(ax, ay) end
    end
end

function love.mousemoved(x, y)
    if scene then
        local ax, ay = scene:screenToArtboard(x, y)
        if ax then scene:pointerMove(ax, ay) end
    end
end

function love.mousereleased(x, y, button)
    if scene and button == 1 then
        local ax, ay = scene:screenToArtboard(x, y)
        if ax then scene:pointerUp(ax, ay) end
    end
end

function love.keypressed(key)
    if key == "q" or key == "escape" then
        love.event.quit()
    end
end

function love.quit()
    scene = nil
    file = nil
    rive.shutdown()
end
