local ffi = require("ffi")

-- ── C API declarations (mirrors rive_love.h) ─────────────────────────

ffi.cdef[[
typedef struct rive_love_context rive_love_context;
typedef struct rive_love_file    rive_love_file;
typedef struct rive_love_scene   rive_love_scene;

const char* rive_love_get_error(void);

rive_love_context* rive_love_init(void);
void               rive_love_shutdown(rive_love_context* ctx);

rive_love_file* rive_love_file_load(rive_love_context* ctx,
                                     const uint8_t* data,
                                     size_t data_len);
void            rive_love_file_destroy(rive_love_file* file);
int32_t         rive_love_file_artboard_count(rive_love_file* file);
const char*     rive_love_file_artboard_name(rive_love_file* file,
                                              int32_t index);

rive_love_scene* rive_love_scene_create_anim(rive_love_file* file,
                                              int32_t artboard_index,
                                              int32_t anim_index);
rive_love_scene* rive_love_scene_create_sm(rive_love_file* file,
                                            int32_t artboard_index,
                                            int32_t sm_index);
void             rive_love_scene_destroy(rive_love_scene* scene);

float   rive_love_scene_width(rive_love_scene* scene);
float   rive_love_scene_height(rive_love_scene* scene);
int32_t rive_love_scene_anim_count(rive_love_scene* scene);
const char* rive_love_scene_anim_name(rive_love_scene* scene, int32_t index);
int32_t rive_love_scene_sm_count(rive_love_scene* scene);
const char* rive_love_scene_sm_name(rive_love_scene* scene, int32_t index);
int32_t rive_love_scene_input_count(rive_love_scene* scene);
const char* rive_love_scene_input_name(rive_love_scene* scene, int32_t index);
int32_t rive_love_scene_set_bool(rive_love_scene* scene,
                                  const char* name, int32_t value);
int32_t rive_love_scene_set_number(rive_love_scene* scene,
                                    const char* name, float value);
int32_t rive_love_scene_fire_trigger(rive_love_scene* scene,
                                      const char* name);
int32_t rive_love_scene_advance(rive_love_scene* scene, float dt);
int32_t rive_love_scene_render(rive_love_scene* scene,
                                float frame_width, float frame_height);

typedef struct rive_love_draw_info {
    const float*    vertices;
    int32_t         vertex_count;
    const uint16_t* indices;
    int32_t         index_count;
    int32_t         draw_mode;
    int32_t         clip_index;
    int32_t         fill_type;
    float color_r, color_g, color_b, color_a;
    float grad_start_x, grad_start_y;
    float grad_end_x, grad_end_y;
    const float* grad_colors;
    const float* grad_stops;
    int32_t      grad_stop_count;
    int32_t      blend_mode;
    float        opacity;
} rive_love_draw_info;

int32_t rive_love_scene_get_draw(rive_love_scene* scene,
                                  int32_t index,
                                  rive_love_draw_info* out);
]]

-- ── Load dylib from same directory as this Lua file ──────────────────
local source = love.filesystem.getSource()
local C = ffi.load(source .. "/rive_love.dylib")

-- ── Module table ─────────────────────────────────────────────────────
local M = {}

-- ── Gradient shader (created lazily) ─────────────────────────────────
local _gradient_shader = nil
local GRADIENT_SHADER_CODE = [[
varying float gradientT;

#ifdef VERTEX
uniform float fillType;
uniform vec2 gradStart;
uniform vec2 gradEnd;

vec4 position(mat4 transform_projection, vec4 vertex_position) {
    vec2 pos = vertex_position.xy;
    float ft = fillType;

    if (ft > 1.5) {
        // Radial gradient: t = distance from center / radius
        float radius = distance(gradStart, gradEnd);
        gradientT = (radius > 0.0) ? clamp(distance(pos, gradStart) / radius, 0.0, 1.0) : 0.0;
    } else {
        // Linear gradient: t = projection onto gradient line
        vec2 gradDir = gradEnd - gradStart;
        float len2 = dot(gradDir, gradDir);
        gradientT = (len2 > 0.0) ? clamp(dot(pos - gradStart, gradDir) / len2, 0.0, 1.0) : 0.0;
    }

    return transform_projection * vertex_position;
}
#endif

#ifdef PIXEL
uniform float stopCount;
uniform vec4 colors[16];
uniform float stops[16];

vec4 effect(vec4 vertex_color, Image texture, vec2 texture_coords, vec2 screen_coords) {
    float t = gradientT;
    int sc = int(stopCount + 0.5);
    vec4 result = colors[0];

    for (int i = 1; i < 16; i++) {
        if (i >= sc) break;
        if (t <= stops[i]) {
            float range = stops[i] - stops[i-1];
            float localT = (range > 0.0) ? (t - stops[i-1]) / range : 0.0;
            result = mix(colors[i-1], colors[i], localT);
            break;
        }
        result = colors[i];
    }

    // Apply opacity to alpha only — RGB stays unmultiplied for Love2D's alphamultiply blend
    return vec4(result.rgb, result.a * vertex_color.a);
}
#endif
]]

local function getGradientShader()
    if not _gradient_shader then
        _gradient_shader = love.graphics.newShader(GRADIENT_SHADER_CODE)
    end
    return _gradient_shader
end

-- ── Context (singleton) ──────────────────────────────────────────────
local ctx = nil

function M.init()
    if ctx ~= nil then return end
    ctx = C.rive_love_init()
    if ctx == nil then
        error("rive_love: init failed: " .. ffi.string(C.rive_love_get_error()))
    end
end

function M.shutdown()
    if ctx == nil then return end
    C.rive_love_shutdown(ctx)
    ctx = nil
end

-- ── File class ───────────────────────────────────────────────────────
local File = {}
File.__index = File

-- Forward declare Scene
local Scene = {}
Scene.__index = Scene

function M.load(filepath)
    M.init()  -- auto-init on first use
    local contents, size = love.filesystem.read("string", filepath)
    if not contents then
        error("rive_love: could not read file: " .. filepath)
    end
    local ptr = ffi.cast("const uint8_t*", contents)
    local handle = C.rive_love_file_load(ctx, ptr, #contents)
    if handle == nil then
        error("rive_love: load failed: " .. ffi.string(C.rive_love_get_error()))
    end
    local self = setmetatable({}, File)
    self._handle = ffi.gc(handle, C.rive_love_file_destroy)
    self._data = contents  -- prevent GC of backing data
    return self
end

function File:artboardCount()
    return tonumber(C.rive_love_file_artboard_count(self._handle))
end

function File:artboardName(index)
    local s = C.rive_love_file_artboard_name(self._handle, index)
    return s ~= nil and ffi.string(s) or nil
end

function File:artboards()
    local result = {}
    for i = 0, self:artboardCount() - 1 do
        result[#result + 1] = { index = i, name = self:artboardName(i) }
    end
    return result
end

-- Name lookup helpers (local)
local function findArtboardIndex(file_handle, name)
    local count = tonumber(C.rive_love_file_artboard_count(file_handle))
    for i = 0, count - 1 do
        local s = C.rive_love_file_artboard_name(file_handle, i)
        if s ~= nil and ffi.string(s) == name then return i end
    end
    return nil
end

local function findAnimIndex(scene_handle, name)
    local count = tonumber(C.rive_love_scene_anim_count(scene_handle))
    for i = 0, count - 1 do
        local s = C.rive_love_scene_anim_name(scene_handle, i)
        if s ~= nil and ffi.string(s) == name then return i end
    end
    return nil
end

local function findSmIndex(scene_handle, name)
    local count = tonumber(C.rive_love_scene_sm_count(scene_handle))
    for i = 0, count - 1 do
        local s = C.rive_love_scene_sm_name(scene_handle, i)
        if s ~= nil and ffi.string(s) == name then return i end
    end
    return nil
end

-- File:animation([artboard], [anim])
-- Arguments can be 0-based indices (number) or names (string).
function File:animation(artboard, anim)
    local ab_idx = artboard or 0
    if type(artboard) == "string" then
        ab_idx = findArtboardIndex(self._handle, artboard)
        if not ab_idx then error("rive_love: artboard not found: " .. artboard) end
    end

    local anim_idx = anim or 0
    if type(anim) == "string" then
        -- Need a temporary scene to enumerate animation names on this artboard
        local tmp = C.rive_love_scene_create_anim(self._handle, ab_idx, 0)
        if tmp ~= nil then
            anim_idx = findAnimIndex(tmp, anim)
            C.rive_love_scene_destroy(tmp)
        end
        if not anim_idx then error("rive_love: animation not found: " .. anim) end
    end

    return Scene._new(
        C.rive_love_scene_create_anim(self._handle, ab_idx, anim_idx))
end

-- File:stateMachine([artboard], [sm])
-- Arguments can be 0-based indices (number) or names (string).
function File:stateMachine(artboard, sm)
    local ab_idx = artboard or 0
    if type(artboard) == "string" then
        ab_idx = findArtboardIndex(self._handle, artboard)
        if not ab_idx then error("rive_love: artboard not found: " .. artboard) end
    end

    local sm_idx = sm or 0
    if type(sm) == "string" then
        local tmp = C.rive_love_scene_create_sm(self._handle, ab_idx, 0)
        if tmp ~= nil then
            sm_idx = findSmIndex(tmp, sm)
            C.rive_love_scene_destroy(tmp)
        end
        if not sm_idx then error("rive_love: state machine not found: " .. sm) end
    end

    return Scene._new(
        C.rive_love_scene_create_sm(self._handle, ab_idx, sm_idx))
end

-- ── Scene class ──────────────────────────────────────────────────────

function Scene._new(handle)
    if handle == nil then
        error("rive_love: scene create failed: " .. ffi.string(C.rive_love_get_error()))
    end
    local self = setmetatable({}, Scene)
    self._handle = ffi.gc(handle, C.rive_love_scene_destroy)
    self._draw_info = ffi.new("rive_love_draw_info")
    self._mesh_pool = {}
    return self
end

function Scene:width()  return tonumber(C.rive_love_scene_width(self._handle)) end
function Scene:height() return tonumber(C.rive_love_scene_height(self._handle)) end

function Scene:advance(dt)
    return C.rive_love_scene_advance(self._handle, dt) ~= 0
end

-- Artboard-level enumeration
function Scene:animCount()
    return tonumber(C.rive_love_scene_anim_count(self._handle))
end

function Scene:animName(index)
    local s = C.rive_love_scene_anim_name(self._handle, index)
    return s ~= nil and ffi.string(s) or nil
end

function Scene:stateMachineCount()
    return tonumber(C.rive_love_scene_sm_count(self._handle))
end

function Scene:stateMachineName(index)
    local s = C.rive_love_scene_sm_name(self._handle, index)
    return s ~= nil and ffi.string(s) or nil
end

function Scene:inputCount()
    return tonumber(C.rive_love_scene_input_count(self._handle))
end

function Scene:inputName(index)
    local s = C.rive_love_scene_input_name(self._handle, index)
    return s ~= nil and ffi.string(s) or nil
end

-- Convenience: return tables of {index, name} records
function Scene:animations()
    local result = {}
    for i = 0, self:animCount() - 1 do
        result[#result + 1] = { index = i, name = self:animName(i) }
    end
    return result
end

function Scene:stateMachines()
    local result = {}
    for i = 0, self:stateMachineCount() - 1 do
        result[#result + 1] = { index = i, name = self:stateMachineName(i) }
    end
    return result
end

function Scene:inputs()
    local result = {}
    for i = 0, self:inputCount() - 1 do
        result[#result + 1] = { index = i, name = self:inputName(i) }
    end
    return result
end

-- State machine inputs (return true if input was found)
function Scene:setBool(name, value)
    return C.rive_love_scene_set_bool(self._handle, name, value and 1 or 0) == 0
end
function Scene:setNumber(name, value)
    return C.rive_love_scene_set_number(self._handle, name, value) == 0
end
function Scene:fireTrigger(name)
    return C.rive_love_scene_fire_trigger(self._handle, name) == 0
end

-- ── Mesh pool ────────────────────────────────────────────────────────
local VERTEX_FORMAT = {
    {"VertexPosition", "float", 2},
    {"VertexColor",    "float", 4},
}

local function ensureMesh(pool, index, vc)
    local entry = pool[index]
    if entry and entry.cap >= vc then
        return entry.mesh
    end
    local cap = math.max(vc, 64)
    local mesh = love.graphics.newMesh(VERTEX_FORMAT, cap, "triangles", "stream")
    pool[index] = { mesh = mesh, cap = cap }
    return mesh
end

-- ── Scene:stats() — render and return draw command breakdown ─────────
function Scene:stats(w, h)
    w = w or self:width()
    h = h or self:height()
    local draw_count = C.rive_love_scene_render(self._handle, w, h)
    local info = self._draw_info
    local s = { total = tonumber(draw_count), normal = 0, clip_incr = 0, clip_decr = 0,
                solid = 0, linear = 0, radial = 0, max_clip = 0 }
    for i = 0, draw_count - 1 do
        C.rive_love_scene_get_draw(self._handle, i, info)
        local dm = tonumber(info.draw_mode)
        local ft = tonumber(info.fill_type)
        local ci = tonumber(info.clip_index)
        if dm == 0 then s.normal = s.normal + 1
        elseif dm == 1 then s.clip_incr = s.clip_incr + 1
        else s.clip_decr = s.clip_decr + 1 end
        if dm == 0 then
            if ft == 0 then s.solid = s.solid + 1
            elseif ft == 1 then s.linear = s.linear + 1
            else s.radial = s.radial + 1 end
        end
        if ci > s.max_clip then s.max_clip = ci end
    end
    return s
end

-- ── Scene:draw() — main rendering method ─────────────────────────────
function Scene:draw(x, y, w, h)
    x = x or 0
    y = y or 0
    w = w or self:width()
    h = h or self:height()

    local draw_count = C.rive_love_scene_render(self._handle, w, h)
    if draw_count <= 0 then return 0 end

    local info = self._draw_info
    local pool = self._mesh_pool

    love.graphics.push("all")
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.translate(x, y)

    for i = 0, draw_count - 1 do
        C.rive_love_scene_get_draw(self._handle, i, info)

        local vc = tonumber(info.vertex_count)
        local ic = tonumber(info.index_count)
        if vc > 0 and ic > 0 then
            local draw_mode = tonumber(info.draw_mode)
            local fill_type = tonumber(info.fill_type)
            local clip_idx  = tonumber(info.clip_index)

            local mesh = ensureMesh(pool, i + 1, vc)

            -- Fill vertex positions + colors
            local verts = info.vertices
            if fill_type == 0 then
                -- Solid: bake fill color into vertex color
                local r = tonumber(info.color_r)
                local g = tonumber(info.color_g)
                local b = tonumber(info.color_b)
                local a = tonumber(info.color_a)
                for v = 0, vc - 1 do
                    mesh:setVertex(v + 1,
                        tonumber(verts[v*2]), tonumber(verts[v*2+1]),
                        r, g, b, a)
                end
            else
                -- Gradient: white vertex color, opacity in alpha
                local a = tonumber(info.opacity)
                for v = 0, vc - 1 do
                    mesh:setVertex(v + 1,
                        tonumber(verts[v*2]), tonumber(verts[v*2+1]),
                        1, 1, 1, a)
                end
            end

            -- Set triangle index map (Love2D is 1-based)
            local idx_table = {}
            local indices = info.indices
            for j = 0, ic - 1 do
                idx_table[j + 1] = tonumber(indices[j]) + 1
            end
            mesh:setVertexMap(idx_table)
            mesh:setDrawRange(1, ic)

            -- Handle draw modes ──────────────────────────────────
            if draw_mode == 1 then
                -- CLIP_INCR: draw into stencil buffer (increment)
                love.graphics.stencil(function()
                    love.graphics.setShader()
                    love.graphics.draw(mesh)
                end, "increment", 0, true)

            elseif draw_mode == 2 then
                -- CLIP_DECR: draw into stencil buffer (decrement)
                love.graphics.stencil(function()
                    love.graphics.setShader()
                    love.graphics.draw(mesh)
                end, "decrement", 0, true)

            else
                -- NORMAL: render with optional stencil test
                if clip_idx > 0 then
                    love.graphics.setStencilTest("gequal", clip_idx)
                else
                    love.graphics.setStencilTest()
                end

                -- Blend mode
                local bm = tonumber(info.blend_mode)
                if bm == 1 then     -- screen
                    love.graphics.setBlendMode("add", "alphamultiply")
                elseif bm == 2 then -- additive
                    love.graphics.setBlendMode("add", "alphamultiply")
                elseif bm == 3 then -- multiply
                    love.graphics.setBlendMode("multiply", "premultiplied")
                else                -- srcOver (default)
                    love.graphics.setBlendMode("alpha", "alphamultiply")
                end

                -- Shader (gradient or none)
                if fill_type ~= 0 then
                    local shader = getGradientShader()
                    love.graphics.setShader(shader)
                    shader:send("fillType", fill_type)
                    shader:send("gradStart", {
                        tonumber(info.grad_start_x),
                        tonumber(info.grad_start_y)
                    })
                    shader:send("gradEnd", {
                        tonumber(info.grad_end_x),
                        tonumber(info.grad_end_y)
                    })
                    local sc = tonumber(info.grad_stop_count)
                    shader:send("stopCount", sc)

                    local colors = {}
                    local gc = info.grad_colors
                    for s = 0, math.min(sc, 16) - 1 do
                        colors[s + 1] = {
                            tonumber(gc[s*4]), tonumber(gc[s*4+1]),
                            tonumber(gc[s*4+2]), tonumber(gc[s*4+3])
                        }
                    end
                    for s = #colors + 1, 16 do colors[s] = {0,0,0,0} end
                    shader:send("colors", unpack(colors))

                    local stops = {}
                    local gs = info.grad_stops
                    for s = 0, math.min(sc, 16) - 1 do
                        stops[s + 1] = tonumber(gs[s])
                    end
                    for s = #stops + 1, 16 do stops[s] = 1.0 end
                    shader:send("stops", unpack(stops))
                else
                    love.graphics.setShader()
                end

                love.graphics.draw(mesh)
            end
        end
    end

    love.graphics.setStencilTest()
    love.graphics.setShader()
    love.graphics.pop()
    return tonumber(draw_count)
end

-- Draw the scene scaled to fit and centered within the given rectangle.
-- Defaults to the full window.
function Scene:drawFit(x, y, w, h)
    x = x or 0
    y = y or 0
    w = w or love.graphics.getWidth()
    h = h or love.graphics.getHeight()

    local sw, sh = self:width(), self:height()
    local scale = math.min(w / sw, h / sh)
    local dw, dh = sw * scale, sh * scale
    local dx, dy = x + (w - dw) / 2, y + (h - dh) / 2

    love.graphics.push()
    love.graphics.translate(dx, dy)
    love.graphics.scale(scale)
    self:draw(0, 0, sw, sh)
    love.graphics.pop()
end

M.File = File
M.Scene = Scene
return M
