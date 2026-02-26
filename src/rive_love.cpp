// rive_love.cpp — C++ bridge between rive-runtime and Love2D
//
// Uses TessRenderer's CPU tessellation to convert Rive vector paths into
// triangle meshes, then exposes them through a flat C API for Lua/FFI to
// consume and render as Love2D Mesh objects.

#include "rive_love.h"

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/factory.hpp"
#include "rive/renderer.hpp"
#include "rive/shapes/paint/color.hpp"
#include "rive/shapes/paint/blend_mode.hpp"
#include "rive/tess/tess_renderer.hpp"
#include "rive/tess/tess_render_path.hpp"
#include "rive/tess/contour_stroke.hpp"
#include "rive/math/mat2d.hpp"
#include "rive/math/mat4.hpp"

#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <unordered_set>

using namespace rive;

// ── Thread-local error string ────────────────────────────────────────

static thread_local std::string g_lastError;

static void setError(const char* msg)
{
    g_lastError = msg ? msg : "";
}

// ── Draw command stored by our renderer ──────────────────────────────

struct DrawCommand
{
    std::vector<float> vertices;    // x,y pairs
    std::vector<uint16_t> indices;  // triangle indices

    int32_t draw_mode = RIVE_LOVE_DRAW_NORMAL;
    int32_t clip_index = 0;

    int32_t fill_type = RIVE_LOVE_FILL_SOLID;
    float color_r = 1, color_g = 1, color_b = 1, color_a = 1;

    // Gradient data
    float grad_start_x = 0, grad_start_y = 0;
    float grad_end_x = 0, grad_end_y = 0;
    std::vector<float> grad_colors; // 4 floats per stop
    std::vector<float> grad_stops;

    int32_t blend_mode = RIVE_LOVE_BLEND_SRC_OVER;
    float opacity = 1.0f;
};

// ── LoveGradient ─────────────────────────────────────────────────────

class LoveGradient : public LITE_RTTI_OVERRIDE(RenderShader, LoveGradient)
{
public:
    int type; // 1=linear, 2=radial
    float start_x, start_y;
    float end_x, end_y;
    std::vector<float> colors; // RGBA per stop
    std::vector<float> stops;

    LoveGradient(int type,
                 float sx, float sy, float ex, float ey,
                 const ColorInt rawColors[],
                 const float rawStops[],
                 size_t count)
        : type(type), start_x(sx), start_y(sy), end_x(ex), end_y(ey)
    {
        colors.resize(count * 4);
        stops.resize(count);
        for (size_t i = 0; i < count; i++)
        {
            colors[i * 4 + 0] = (float)colorRed(rawColors[i]) / 255.0f;
            colors[i * 4 + 1] = (float)colorGreen(rawColors[i]) / 255.0f;
            colors[i * 4 + 2] = (float)colorBlue(rawColors[i]) / 255.0f;
            colors[i * 4 + 3] = colorOpacity(rawColors[i]);
            stops[i] = rawStops[i];
        }
    }
};

// ── LoveRenderPath ───────────────────────────────────────────────────

class LoveRenderPath : public LITE_RTTI_OVERRIDE(TessRenderPath, LoveRenderPath)
{
    using Super = LITE_RTTI_OVERRIDE(TessRenderPath, LoveRenderPath);

public:
    std::vector<Vec2D> m_vertices;
    std::vector<uint16_t> m_indices;
    AABB m_bounds;

    LoveRenderPath() {}
    LoveRenderPath(RawPath& rawPath, FillRule fillRule)
        : Super(rawPath, fillRule) {}

    void addTriangles(Span<const Vec2D> vts,
                      Span<const uint16_t> idx) override
    {
        // Offset indices by current vertex count
        uint16_t offset = (uint16_t)m_vertices.size();
        m_vertices.insert(m_vertices.end(), vts.begin(), vts.end());
        for (auto i : idx)
        {
            m_indices.push_back(i + offset);
        }
    }

    void setTriangulatedBounds(const AABB& value) override
    {
        m_bounds = value;
    }

    void rewind() override
    {
        TessRenderPath::rewind();
        m_vertices.clear();
        m_indices.clear();
    }

    // Trigger tessellation and return whether data is available
    bool ensureTriangulated()
    {
        m_vertices.clear();
        m_indices.clear();
        triangulate();
        return !m_vertices.empty() && !m_indices.empty();
    }
};

// ── LoveRenderPaint ──────────────────────────────────────────────────

class LoveRenderPaint : public LITE_RTTI_OVERRIDE(RenderPaint, LoveRenderPaint)
{
public:
    RenderPaintStyle m_style = RenderPaintStyle::fill;
    float m_color[4] = {0, 0, 0, 1}; // RGBA
    BlendMode m_blendMode = BlendMode::srcOver;
    rcp<LoveGradient> m_gradient;

    // Stroke properties
    float m_strokeThickness = 0;
    StrokeJoin m_strokeJoin = StrokeJoin::miter;
    StrokeCap m_strokeCap = StrokeCap::butt;
    bool m_strokeDirty = false;
    std::unique_ptr<ContourStroke> m_stroke;

    void style(RenderPaintStyle value) override
    {
        m_style = value;
        if (value == RenderPaintStyle::stroke && !m_stroke)
        {
            m_stroke = std::make_unique<ContourStroke>();
        }
    }

    void color(ColorInt value) override
    {
        m_color[0] = (float)colorRed(value) / 255.0f;
        m_color[1] = (float)colorGreen(value) / 255.0f;
        m_color[2] = (float)colorBlue(value) / 255.0f;
        m_color[3] = colorOpacity(value);
        m_gradient = nullptr;
    }

    void thickness(float value) override
    {
        m_strokeThickness = value;
        m_strokeDirty = true;
    }

    void join(StrokeJoin value) override
    {
        m_strokeJoin = value;
        m_strokeDirty = true;
    }

    void cap(StrokeCap value) override
    {
        m_strokeCap = value;
        m_strokeDirty = true;
    }

    void blendMode(BlendMode value) override
    {
        m_blendMode = value;
    }

    void shader(rcp<RenderShader> shader) override
    {
        m_gradient = lite_rtti_rcp_cast<LoveGradient>(shader);
    }

    void invalidateStroke() override
    {
        m_strokeDirty = true;
    }
};

// ── LoveRenderBuffer ─────────────────────────────────────────────────

class LoveRenderBuffer : public LITE_RTTI_OVERRIDE(RenderBuffer, LoveRenderBuffer)
{
    using Super = LITE_RTTI_OVERRIDE(RenderBuffer, LoveRenderBuffer);
    std::vector<uint8_t> m_data;

public:
    LoveRenderBuffer(RenderBufferType type, RenderBufferFlags flags, size_t size)
        : Super(type, flags, size), m_data(size) {}

    const uint8_t* data() const { return m_data.data(); }

protected:
    void* onMap() override { return m_data.data(); }
    void onUnmap() override {}
};

// ── LoveTessRenderer ─────────────────────────────────────────────────

class LoveTessRenderer : public TessRenderer
{
public:
    std::vector<DrawCommand> m_draws;

    LoveTessRenderer() : TessRenderer() {}

    // Reset clip tracking so all clip paths are re-emitted each frame.
    // This is needed because Love2D clears the stencil buffer each frame.
    void resetClipState()
    {
        m_clipPaths.clear();
        m_clipCount = 0;
        m_IsClippingDirty = true;
    }

    void orthographicProjection(float left, float right,
                                float bottom, float top,
                                float near, float far) override
    {
        // Build standard orthographic projection matrix
        // Love2D uses top-left origin, so we use top=0, bottom=height
        Mat4 proj;
        proj[0] = 2.0f / (right - left);
        proj[1] = 0.0f;
        proj[2] = 0.0f;
        proj[3] = 0.0f;

        proj[4] = 0.0f;
        proj[5] = 2.0f / (top - bottom);
        proj[6] = 0.0f;
        proj[7] = 0.0f;

        proj[8] = 0.0f;
        proj[9] = 0.0f;
        proj[10] = -1.0f;
        proj[11] = 0.0f;

        proj[12] = (right + left) / (left - right);
        proj[13] = (top + bottom) / (bottom - top);
        proj[14] = 0.0f;
        proj[15] = 1.0f;

        projection(proj);
    }

    void drawPath(RenderPath* path, RenderPaint* paint) override
    {
        LITE_RTTI_CAST_OR_RETURN(lovePath, LoveRenderPath*, path);
        LITE_RTTI_CAST_OR_RETURN(lovePaint, LoveRenderPaint*, paint);

        // Apply any pending clip changes
        applyClipping();

        const Mat2D& world = transform();
        float opacity = modulatedOpacity();

        if (lovePaint->m_style == RenderPaintStyle::stroke)
        {
            drawStroke(lovePath, lovePaint, world, opacity);
        }
        else
        {
            drawFill(lovePath, lovePaint, world, opacity);
        }
    }

private:
    // ── Clipping state ───────────────────────────────────────────
    std::vector<SubPath> m_clipPaths;
    int m_clipCount = 0;

    void applyClipping()
    {
        if (!m_IsClippingDirty)
        {
            return;
        }
        m_IsClippingDirty = false;

        RenderState& state = m_Stack.back();
        auto currentClipLength = m_clipPaths.size();

        if (currentClipLength == state.clipPaths.size())
        {
            bool allSame = true;
            for (size_t i = 0; i < currentClipLength; i++)
            {
                if (state.clipPaths[i].path() != m_clipPaths[i].path())
                {
                    allSame = false;
                    break;
                }
            }
            if (allSame) return;
        }

        // Decrement removed paths
        std::unordered_set<RenderPath*> alreadyApplied;
        for (auto& appliedPath : m_clipPaths)
        {
            bool decr = true;
            for (auto& nextClipPath : state.clipPaths)
            {
                if (nextClipPath.path() == appliedPath.path())
                {
                    decr = false;
                    alreadyApplied.insert(appliedPath.path());
                    break;
                }
            }
            if (decr)
            {
                LITE_RTTI_CAST_OR_CONTINUE(sokolPath, LoveRenderPath*, appliedPath.path());
                emitClipDraw(sokolPath, appliedPath.transform(), RIVE_LOVE_DRAW_CLIP_DECR);
            }
        }

        // Increment new paths
        for (auto& nextClipPath : state.clipPaths)
        {
            if (alreadyApplied.count(nextClipPath.path())) continue;
            LITE_RTTI_CAST_OR_CONTINUE(sokolPath, LoveRenderPath*, nextClipPath.path());
            emitClipDraw(sokolPath, nextClipPath.transform(), RIVE_LOVE_DRAW_CLIP_INCR);
        }

        m_clipCount = (int)state.clipPaths.size();
        m_clipPaths = state.clipPaths;
    }

    void emitClipDraw(LoveRenderPath* path, const Mat2D& pathTransform, int32_t mode)
    {
        if (!path->ensureTriangulated()) return;

        DrawCommand cmd;
        cmd.draw_mode = mode;
        cmd.clip_index = 0;

        // Transform vertices
        auto& verts = path->m_vertices;
        cmd.vertices.resize(verts.size() * 2);
        for (size_t i = 0; i < verts.size(); i++)
        {
            Vec2D p = pathTransform * verts[i];
            cmd.vertices[i * 2 + 0] = p.x;
            cmd.vertices[i * 2 + 1] = p.y;
        }
        cmd.indices = path->m_indices;

        m_draws.push_back(std::move(cmd));
    }

    // ── Fill rendering ───────────────────────────────────────────

    void drawFill(LoveRenderPath* path, LoveRenderPaint* paint,
                  const Mat2D& world, float opacity)
    {
        if (!path->ensureTriangulated()) return;

        DrawCommand cmd;
        cmd.draw_mode = RIVE_LOVE_DRAW_NORMAL;
        cmd.clip_index = m_clipCount;
        cmd.opacity = opacity;

        // Transform vertices from local to viewport
        auto& verts = path->m_vertices;
        cmd.vertices.resize(verts.size() * 2);
        for (size_t i = 0; i < verts.size(); i++)
        {
            Vec2D p = world * verts[i];
            cmd.vertices[i * 2 + 0] = p.x;
            cmd.vertices[i * 2 + 1] = p.y;
        }
        cmd.indices = path->m_indices;

        fillPaintInfo(cmd, paint, world, opacity);

        m_draws.push_back(std::move(cmd));
    }

    // ── Stroke rendering ─────────────────────────────────────────

    void drawStroke(LoveRenderPath* path, LoveRenderPaint* paint,
                    const Mat2D& world, float opacity)
    {
        if (!paint->m_stroke) return;

        // Extrude stroke to triangle strip
        paint->m_stroke->reset();
        Mat2D identity;
        path->extrudeStroke(paint->m_stroke.get(),
                            paint->m_strokeJoin,
                            paint->m_strokeCap,
                            paint->m_strokeThickness / 2.0f,
                            identity);

        const auto& strip = paint->m_stroke->triangleStrip();
        if (strip.size() < 3) return;

        // Convert triangle strip to indexed triangles per sub-path segment
        std::vector<Vec2D> allVerts;
        std::vector<uint16_t> allIndices;

        paint->m_stroke->resetRenderOffset();
        size_t strokeStart, strokeEnd;
        while (paint->m_stroke->nextRenderOffset(strokeStart, strokeEnd))
        {
            size_t length = strokeEnd - strokeStart;
            if (length < 3) continue;

            uint16_t baseVertex = (uint16_t)allVerts.size();
            for (size_t i = strokeStart; i < strokeEnd; i++)
            {
                allVerts.push_back(strip[i]);
            }

            // Convert strip to triangles with correct winding
            for (size_t i = 0, end = length - 2; i < end; i++)
            {
                if ((i % 2) == 1)
                {
                    allIndices.push_back(baseVertex + (uint16_t)i);
                    allIndices.push_back(baseVertex + (uint16_t)(i + 1));
                    allIndices.push_back(baseVertex + (uint16_t)(i + 2));
                }
                else
                {
                    allIndices.push_back(baseVertex + (uint16_t)i);
                    allIndices.push_back(baseVertex + (uint16_t)(i + 2));
                    allIndices.push_back(baseVertex + (uint16_t)(i + 1));
                }
            }
        }

        if (allVerts.empty() || allIndices.empty()) return;

        DrawCommand cmd;
        cmd.draw_mode = RIVE_LOVE_DRAW_NORMAL;
        cmd.clip_index = m_clipCount;
        cmd.opacity = opacity;

        // Transform vertices
        cmd.vertices.resize(allVerts.size() * 2);
        for (size_t i = 0; i < allVerts.size(); i++)
        {
            Vec2D p = world * allVerts[i];
            cmd.vertices[i * 2 + 0] = p.x;
            cmd.vertices[i * 2 + 1] = p.y;
        }
        cmd.indices = std::move(allIndices);

        fillPaintInfo(cmd, paint, world, opacity);

        m_draws.push_back(std::move(cmd));
    }

    // ── Fill paint/gradient info into a DrawCommand ──────────────

    void fillPaintInfo(DrawCommand& cmd, LoveRenderPaint* paint,
                       const Mat2D& world, float opacity)
    {
        // Blend mode mapping
        switch (paint->m_blendMode)
        {
            case BlendMode::srcOver:
                cmd.blend_mode = RIVE_LOVE_BLEND_SRC_OVER;
                break;
            case BlendMode::screen:
                cmd.blend_mode = RIVE_LOVE_BLEND_SCREEN;
                break;
            case BlendMode::colorDodge:
                cmd.blend_mode = RIVE_LOVE_BLEND_ADDITIVE;
                break;
            case BlendMode::multiply:
                cmd.blend_mode = RIVE_LOVE_BLEND_MULTIPLY;
                break;
            default:
                cmd.blend_mode = RIVE_LOVE_BLEND_SRC_OVER;
                break;
        }

        if (paint->m_gradient)
        {
            auto& g = paint->m_gradient;
            cmd.fill_type = g->type; // 1=linear, 2=radial

            // Transform gradient endpoints by the world matrix
            Vec2D start = world * Vec2D(g->start_x, g->start_y);
            Vec2D end = world * Vec2D(g->end_x, g->end_y);
            cmd.grad_start_x = start.x;
            cmd.grad_start_y = start.y;
            cmd.grad_end_x = end.x;
            cmd.grad_end_y = end.y;

            cmd.grad_colors = g->colors;
            cmd.grad_stops = g->stops;
        }
        else
        {
            cmd.fill_type = RIVE_LOVE_FILL_SOLID;
            cmd.color_r = paint->m_color[0];
            cmd.color_g = paint->m_color[1];
            cmd.color_b = paint->m_color[2];
            cmd.color_a = paint->m_color[3] * opacity;
        }
    }
};

// ── LoveFactory ──────────────────────────────────────────────────────

class LoveFactory : public Factory
{
public:
    rcp<RenderBuffer> makeRenderBuffer(RenderBufferType type,
                                       RenderBufferFlags flags,
                                       size_t sizeInBytes) override
    {
        return make_rcp<LoveRenderBuffer>(type, flags, sizeInBytes);
    }

    rcp<RenderShader> makeLinearGradient(float sx, float sy,
                                         float ex, float ey,
                                         const ColorInt colors[],
                                         const float stops[],
                                         size_t count) override
    {
        return rcp<RenderShader>(
            new LoveGradient(1, sx, sy, ex, ey, colors, stops, count));
    }

    rcp<RenderShader> makeRadialGradient(float cx, float cy,
                                         float radius,
                                         const ColorInt colors[],
                                         const float stops[],
                                         size_t count) override
    {
        return rcp<RenderShader>(
            new LoveGradient(2, cx, cy, cx + radius, cy, colors, stops, count));
    }

    rcp<RenderPath> makeRenderPath(RawPath& rawPath, FillRule fillRule) override
    {
        return make_rcp<LoveRenderPath>(rawPath, fillRule);
    }

    rcp<RenderPath> makeEmptyRenderPath() override
    {
        return make_rcp<LoveRenderPath>();
    }

    rcp<RenderPaint> makeRenderPaint() override
    {
        return make_rcp<LoveRenderPaint>();
    }

    rcp<RenderImage> decodeImage(Span<const uint8_t>) override
    {
        // Image rendering not yet supported
        return nullptr;
    }
};

// ── Internal handle types ────────────────────────────────────────────

struct rive_love_context
{
    std::unique_ptr<LoveFactory> factory;
    std::unique_ptr<LoveTessRenderer> renderer;
};

struct rive_love_file
{
    rive_love_context* ctx;
    rcp<File> file;
};

struct rive_love_scene
{
    rive_love_file* file; // back-reference for factory
    std::unique_ptr<ArtboardInstance> artboard;

    // One of these is set
    std::unique_ptr<LinearAnimationInstance> animation;
    std::unique_ptr<StateMachineInstance> stateMachine;
};

// ── C API Implementation ─────────────────────────────────────────────

extern "C" {

__attribute__((visibility("default")))
const char* rive_love_get_error(void)
{
    return g_lastError.empty() ? nullptr : g_lastError.c_str();
}

__attribute__((visibility("default")))
rive_love_context* rive_love_init(void)
{
    auto ctx = new rive_love_context();
    ctx->factory = std::make_unique<LoveFactory>();
    ctx->renderer = std::make_unique<LoveTessRenderer>();
    setError("");
    return ctx;
}

__attribute__((visibility("default")))
void rive_love_shutdown(rive_love_context* ctx)
{
    delete ctx;
}

__attribute__((visibility("default")))
rive_love_file* rive_love_file_load(rive_love_context* ctx,
                                     const uint8_t* data,
                                     size_t data_len)
{
    if (!ctx || !data || data_len == 0)
    {
        setError("invalid arguments to file_load");
        return nullptr;
    }

    ImportResult result;
    auto file = File::import(
        Span<const uint8_t>(data, data_len),
        ctx->factory.get(),
        &result);

    if (result != ImportResult::success || !file)
    {
        switch (result)
        {
            case ImportResult::unsupportedVersion:
                setError("unsupported .riv file version");
                break;
            case ImportResult::malformed:
                setError("malformed .riv file");
                break;
            default:
                setError("unknown import error");
                break;
        }
        return nullptr;
    }

    auto f = new rive_love_file();
    f->ctx = ctx;
    f->file = std::move(file);
    setError("");
    return f;
}

__attribute__((visibility("default")))
void rive_love_file_destroy(rive_love_file* file)
{
    delete file;
}

__attribute__((visibility("default")))
int32_t rive_love_file_artboard_count(rive_love_file* file)
{
    if (!file) return 0;
    return (int32_t)file->file->artboardCount();
}

__attribute__((visibility("default")))
const char* rive_love_file_artboard_name(rive_love_file* file,
                                          int32_t index)
{
    if (!file || index < 0 || (size_t)index >= file->file->artboardCount())
        return nullptr;

    // Cache the name string to keep the pointer valid
    static thread_local std::string s_name;
    s_name = file->file->artboardNameAt((size_t)index);
    return s_name.c_str();
}

__attribute__((visibility("default")))
rive_love_scene* rive_love_scene_create_anim(rive_love_file* file,
                                              int32_t artboard_index,
                                              int32_t anim_index)
{
    if (!file)
    {
        setError("null file");
        return nullptr;
    }

    auto artboard = file->file->artboardAt((size_t)artboard_index);
    if (!artboard)
    {
        setError("invalid artboard index");
        return nullptr;
    }

    auto anim = artboard->animationAt((size_t)anim_index);
    if (!anim)
    {
        setError("invalid animation index");
        return nullptr;
    }

    auto scene = new rive_love_scene();
    scene->file = file;
    scene->animation = std::move(anim);
    scene->artboard = std::move(artboard);
    setError("");
    return scene;
}

__attribute__((visibility("default")))
rive_love_scene* rive_love_scene_create_sm(rive_love_file* file,
                                            int32_t artboard_index,
                                            int32_t sm_index)
{
    if (!file)
    {
        setError("null file");
        return nullptr;
    }

    auto artboard = file->file->artboardAt((size_t)artboard_index);
    if (!artboard)
    {
        setError("invalid artboard index");
        return nullptr;
    }

    auto sm = artboard->stateMachineAt((size_t)sm_index);
    if (!sm)
    {
        setError("invalid state machine index");
        return nullptr;
    }

    auto scene = new rive_love_scene();
    scene->file = file;
    scene->stateMachine = std::move(sm);
    scene->artboard = std::move(artboard);
    setError("");
    return scene;
}

__attribute__((visibility("default")))
void rive_love_scene_destroy(rive_love_scene* scene)
{
    delete scene;
}

__attribute__((visibility("default")))
float rive_love_scene_width(rive_love_scene* scene)
{
    if (!scene || !scene->artboard) return 0;
    return scene->artboard->width();
}

__attribute__((visibility("default")))
float rive_love_scene_height(rive_love_scene* scene)
{
    if (!scene || !scene->artboard) return 0;
    return scene->artboard->height();
}

__attribute__((visibility("default")))
int32_t rive_love_scene_anim_count(rive_love_scene* scene)
{
    if (!scene || !scene->artboard) return 0;
    return (int32_t)scene->artboard->animationCount();
}

__attribute__((visibility("default")))
const char* rive_love_scene_anim_name(rive_love_scene* scene,
                                       int32_t index)
{
    if (!scene || !scene->artboard) return nullptr;
    if (index < 0 || (size_t)index >= scene->artboard->animationCount())
        return nullptr;
    static thread_local std::string s_name;
    s_name = scene->artboard->animationNameAt((size_t)index);
    return s_name.c_str();
}

__attribute__((visibility("default")))
int32_t rive_love_scene_sm_count(rive_love_scene* scene)
{
    if (!scene || !scene->artboard) return 0;
    return (int32_t)scene->artboard->stateMachineCount();
}

__attribute__((visibility("default")))
const char* rive_love_scene_sm_name(rive_love_scene* scene,
                                     int32_t index)
{
    if (!scene || !scene->artboard) return nullptr;
    if (index < 0 || (size_t)index >= scene->artboard->stateMachineCount())
        return nullptr;
    static thread_local std::string s_name;
    s_name = scene->artboard->stateMachineNameAt((size_t)index);
    return s_name.c_str();
}

__attribute__((visibility("default")))
int32_t rive_love_scene_input_count(rive_love_scene* scene)
{
    if (!scene || !scene->stateMachine) return 0;
    return (int32_t)scene->stateMachine->inputCount();
}

__attribute__((visibility("default")))
const char* rive_love_scene_input_name(rive_love_scene* scene,
                                        int32_t index)
{
    if (!scene || !scene->stateMachine) return nullptr;
    if (index < 0 || (size_t)index >= scene->stateMachine->inputCount())
        return nullptr;
    auto input = scene->stateMachine->input((size_t)index);
    if (!input) return nullptr;
    static thread_local std::string s_name;
    s_name = input->name();
    return s_name.c_str();
}

__attribute__((visibility("default")))
int32_t rive_love_scene_set_bool(rive_love_scene* scene,
                                  const char* name,
                                  int32_t value)
{
    if (!scene || !scene->stateMachine || !name) return -1;
    auto input = scene->stateMachine->getBool(name);
    if (!input) return -1;
    input->value(value != 0);
    return 0;
}

__attribute__((visibility("default")))
int32_t rive_love_scene_set_number(rive_love_scene* scene,
                                    const char* name,
                                    float value)
{
    if (!scene || !scene->stateMachine || !name) return -1;
    auto input = scene->stateMachine->getNumber(name);
    if (!input) return -1;
    input->value(value);
    return 0;
}

__attribute__((visibility("default")))
int32_t rive_love_scene_fire_trigger(rive_love_scene* scene,
                                      const char* name)
{
    if (!scene || !scene->stateMachine || !name) return -1;
    auto input = scene->stateMachine->getTrigger(name);
    if (!input) return -1;
    input->fire();
    return 0;
}

__attribute__((visibility("default")))
int32_t rive_love_scene_advance(rive_love_scene* scene, float dt)
{
    if (!scene) return 0;

    bool keepGoing = false;
    if (scene->stateMachine)
    {
        keepGoing = scene->stateMachine->advanceAndApply(dt);
    }
    else if (scene->animation)
    {
        keepGoing = scene->animation->advanceAndApply(dt);
    }

    return keepGoing ? 1 : 0;
}

__attribute__((visibility("default")))
int32_t rive_love_scene_render(rive_love_scene* scene,
                                float frame_width,
                                float frame_height)
{
    if (!scene || !scene->artboard) return 0;

    auto* renderer = scene->file->ctx->renderer.get();
    renderer->m_draws.clear();
    renderer->resetClipState();

    // Set up orthographic projection: origin at top-left
    renderer->orthographicProjection(0, frame_width, 0, frame_height, 0, 1);

    renderer->save();

    // Align artboard to viewport with Fit::contain, centered
    renderer->align(Fit::contain,
                    Alignment::center,
                    AABB(0, 0, frame_width, frame_height),
                    scene->artboard->bounds());

    // Draw artboard into our renderer (populates m_draws)
    scene->artboard->draw(renderer);

    renderer->restore();

    return (int32_t)renderer->m_draws.size();
}

__attribute__((visibility("default")))
int32_t rive_love_scene_get_draw(rive_love_scene* scene,
                                  int32_t index,
                                  rive_love_draw_info* out)
{
    if (!scene || !out) return -1;

    auto* renderer = scene->file->ctx->renderer.get();
    if (index < 0 || (size_t)index >= renderer->m_draws.size()) return -1;

    const DrawCommand& cmd = renderer->m_draws[(size_t)index];

    out->vertices = cmd.vertices.data();
    out->vertex_count = (int32_t)(cmd.vertices.size() / 2);
    out->indices = cmd.indices.data();
    out->index_count = (int32_t)cmd.indices.size();

    out->draw_mode = cmd.draw_mode;
    out->clip_index = cmd.clip_index;

    out->fill_type = cmd.fill_type;
    out->color_r = cmd.color_r;
    out->color_g = cmd.color_g;
    out->color_b = cmd.color_b;
    out->color_a = cmd.color_a;

    out->grad_start_x = cmd.grad_start_x;
    out->grad_start_y = cmd.grad_start_y;
    out->grad_end_x = cmd.grad_end_x;
    out->grad_end_y = cmd.grad_end_y;
    out->grad_colors = cmd.grad_colors.empty() ? nullptr : cmd.grad_colors.data();
    out->grad_stops = cmd.grad_stops.empty() ? nullptr : cmd.grad_stops.data();
    out->grad_stop_count = (int32_t)(cmd.grad_stops.size());

    out->blend_mode = cmd.blend_mode;
    out->opacity = cmd.opacity;

    return 0;
}

} // extern "C"
