# rive_love

> **Alpha** — API may change.

Rive animation runtime for Love2D (macOS). Renders .riv files using CPU tessellation via rive-runtime's TessRenderer, exposed as Love2D Mesh objects with gradient shaders and stencil clipping.

## Quick Start

1. Clone rive-runtime:
   ```
   git clone https://github.com/rive-app/rive-runtime deps/rive-runtime
   ```

2. Build the native library:
   ```
   ./build.sh
   ```

3. Copy `rive_love.lua` and `rive_love.dylib` into your Love2D project folder.

4. Use it:
   ```lua
   local rive = require("rive_love")

   local scene

   function love.load()
       local file = rive.load("animation.riv")
       scene = file:stateMachine()
   end

   function love.update(dt)
       scene:advance(dt)
   end

   function love.draw()
       love.graphics.clear(0.15, 0.15, 0.15)
       scene:drawFit()
   end

   function love.quit()
       scene = nil
       rive.shutdown()
   end
   ```

## Configuration

Your `conf.lua` **must** enable the stencil buffer for Rive clipping to work:

```lua
function love.conf(t)
    t.window.stencil = true
end
```

## API Reference

All indices are **0-based** (matching Rive/C conventions).

### Module

| Function | Returns | Description |
|----------|---------|-------------|
| `rive.load(filepath)` | File | Load a .riv file. Auto-initializes the runtime. |
| `rive.shutdown()` | | Free the internal context. Call in `love.quit()`. |

### File

| Method | Returns | Description |
|--------|---------|-------------|
| `file:artboardCount()` | number | Number of artboards in the file. |
| `file:artboardName(index)` | string or nil | Name of artboard at index. |
| `file:artboards()` | table | `{ {index=0, name="..."}, ... }` for all artboards. |
| `file:animation([artboard], [anim])` | Scene | Create an animation scene. Args can be indices or name strings. |
| `file:stateMachine([artboard], [sm])` | Scene | Create a state machine scene. Args can be indices or name strings. |

Both `animation()` and `stateMachine()` accept indices (numbers) or names (strings) for either argument:

```lua
file:stateMachine()                     -- artboard 0, sm 0
file:stateMachine(0, 0)                 -- same
file:stateMachine("My Artboard", 0)     -- artboard by name
file:stateMachine(0, "State Machine 1") -- sm by name
file:stateMachine("My Artboard", "SM")  -- both by name
```

### Scene

**Dimensions:**

| Method | Returns | Description |
|--------|---------|-------------|
| `scene:width()` | number | Artboard width. |
| `scene:height()` | number | Artboard height. |

**Enumeration:**

| Method | Returns | Description |
|--------|---------|-------------|
| `scene:animCount()` | number | Number of animations on the artboard. |
| `scene:animName(index)` | string or nil | Animation name at index. |
| `scene:animations()` | table | `{ {index, name}, ... }` for all animations. |
| `scene:stateMachineCount()` | number | Number of state machines. |
| `scene:stateMachineName(index)` | string or nil | State machine name at index. |
| `scene:stateMachines()` | table | `{ {index, name}, ... }` for all state machines. |
| `scene:inputCount()` | number | Number of state machine inputs. |
| `scene:inputName(index)` | string or nil | Input name at index. |
| `scene:inputs()` | table | `{ {index, name}, ... }` for all inputs. |

**State machine inputs:**

| Method | Returns | Description |
|--------|---------|-------------|
| `scene:setBool(name, value)` | boolean | Set a bool input. Returns true if found. |
| `scene:setNumber(name, value)` | boolean | Set a number input. Returns true if found. |
| `scene:fireTrigger(name)` | boolean | Fire a trigger. Returns true if found. |

**Update and render:**

| Method | Returns | Description |
|--------|---------|-------------|
| `scene:advance(dt)` | boolean | Advance by `dt` seconds. Returns true if still animating. |
| `scene:draw([x], [y], [w], [h])` | number | Draw at position with viewport size. Returns draw command count. Defaults to artboard size. |
| `scene:drawFit([x], [y], [w], [h])` | | Draw scaled to fit and centered within rectangle. Defaults to full window. |

**Debug:**

| Method | Returns | Description |
|--------|---------|-------------|
| `scene:stats([w], [h])` | table | Draw command breakdown: `{total, normal, clip_incr, clip_decr, solid, linear, radial, max_clip}`. |

## Known Limitations

- **macOS only** — builds a .dylib. Linux/Windows would need platform-specific builds.
- **No image rendering** — embedded raster images in .riv files are ignored.
- **No text rendering** — Rive text features are not supported.
- **No pointer events** — interactive hover/click state machines need manual input wiring (see below).
- **Maximum 16 gradient stops** per fill.
- **0-based indices** — matching Rive/C conventions, not Lua's 1-based.

### Pointer Events (Future)

To support interactive Rive files that respond to mouse hover/click, you would need to add `pointerDown`/`pointerMove`/`pointerUp` C API functions that call `StateMachineInstance::pointerDown(Vec2D)` etc., then forward Love2D mouse events with coordinates converted from screen space to artboard space.
