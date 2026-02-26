#ifndef RIVE_LOVE_H
#define RIVE_LOVE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque handle types ──────────────────────────────────────────────
typedef struct rive_love_context rive_love_context;
typedef struct rive_love_file    rive_love_file;
typedef struct rive_love_scene   rive_love_scene;

// ── Error handling ───────────────────────────────────────────────────
// Returns the last error message, or NULL if no error.
// The string is valid until the next rive_love_* call.
const char* rive_love_get_error(void);

// ── Context lifecycle ────────────────────────────────────────────────
// Creates the internal Factory + TessRenderer. Call once at startup.
rive_love_context* rive_love_init(void);
void               rive_love_shutdown(rive_love_context* ctx);

// ── File loading ─────────────────────────────────────────────────────
// Load a .riv file from a byte buffer. Returns NULL on failure.
rive_love_file* rive_love_file_load(rive_love_context* ctx,
                                     const uint8_t* data,
                                     size_t data_len);
void            rive_love_file_destroy(rive_love_file* file);

// Query artboards in a loaded file.
int32_t     rive_love_file_artboard_count(rive_love_file* file);
const char* rive_love_file_artboard_name(rive_love_file* file,
                                          int32_t index);

// ── Scene creation ───────────────────────────────────────────────────
// A "scene" is an artboard instance + one active animation or state machine.
//
// Create a scene with a linear animation.
// artboard_index: 0 for default artboard, or a specific index.
// anim_index: index of the linear animation to play.
rive_love_scene* rive_love_scene_create_anim(rive_love_file* file,
                                              int32_t artboard_index,
                                              int32_t anim_index);

// Create a scene with a state machine.
rive_love_scene* rive_love_scene_create_sm(rive_love_file* file,
                                            int32_t artboard_index,
                                            int32_t sm_index);

void rive_love_scene_destroy(rive_love_scene* scene);

// ── Artboard queries ─────────────────────────────────────────────────
float rive_love_scene_width(rive_love_scene* scene);
float rive_love_scene_height(rive_love_scene* scene);

// ── Animation queries ────────────────────────────────────────────────
int32_t     rive_love_scene_anim_count(rive_love_scene* scene);
const char* rive_love_scene_anim_name(rive_love_scene* scene,
                                       int32_t index);

// ── State machine queries ────────────────────────────────────────────
int32_t     rive_love_scene_sm_count(rive_love_scene* scene);
const char* rive_love_scene_sm_name(rive_love_scene* scene,
                                     int32_t index);

// ── State machine input control ──────────────────────────────────────
int32_t     rive_love_scene_input_count(rive_love_scene* scene);
const char* rive_love_scene_input_name(rive_love_scene* scene,
                                        int32_t index);

// Returns 0 on success, -1 if input not found or wrong type.
int32_t rive_love_scene_set_bool(rive_love_scene* scene,
                                  const char* name,
                                  int32_t value);
int32_t rive_love_scene_set_number(rive_love_scene* scene,
                                    const char* name,
                                    float value);
int32_t rive_love_scene_fire_trigger(rive_love_scene* scene,
                                      const char* name);

// ── Pointer events (coordinates in artboard space) ──────────────────
// Forward mouse/touch events to the state machine.
// Returns 0 on success, -1 if no state machine is active.
int32_t rive_love_scene_pointer_down(rive_love_scene* scene,
                                      float x, float y);
int32_t rive_love_scene_pointer_move(rive_love_scene* scene,
                                      float x, float y);
int32_t rive_love_scene_pointer_up(rive_love_scene* scene,
                                    float x, float y);

// ── Advance + Render ─────────────────────────────────────────────────
// Advance the animation/state machine by dt seconds.
// Returns 1 if still playing, 0 if animation completed.
int32_t rive_love_scene_advance(rive_love_scene* scene, float dt);

// Render the current frame into internal draw buffers.
// frame_width/frame_height define the viewport for Fit::contain alignment.
// Returns the number of draw commands generated (>= 0).
int32_t rive_love_scene_render(rive_love_scene* scene,
                                float frame_width,
                                float frame_height);

// ── Draw command retrieval ───────────────────────────────────────────

// Draw mode (stencil operations for clipping)
#define RIVE_LOVE_DRAW_NORMAL    0
#define RIVE_LOVE_DRAW_CLIP_INCR 1
#define RIVE_LOVE_DRAW_CLIP_DECR 2

// Fill type
#define RIVE_LOVE_FILL_SOLID     0
#define RIVE_LOVE_FILL_LINEAR    1
#define RIVE_LOVE_FILL_RADIAL    2
#define RIVE_LOVE_FILL_IMAGE     3

// Blend modes
#define RIVE_LOVE_BLEND_SRC_OVER 0
#define RIVE_LOVE_BLEND_SCREEN   1
#define RIVE_LOVE_BLEND_ADDITIVE 2
#define RIVE_LOVE_BLEND_MULTIPLY 3

// Per-draw-command info. Filled by rive_love_scene_get_draw().
// Pointer fields point into internal buffers valid until the next
// rive_love_scene_render() call — Lua must consume them immediately.
typedef struct rive_love_draw_info {
    // Geometry (pre-transformed to viewport coordinates)
    const float*    vertices;       // x,y pairs — 2 floats per vertex
    int32_t         vertex_count;   // number of vertices
    const uint16_t* indices;        // triangle indices (0-based)
    int32_t         index_count;    // number of indices (multiple of 3)

    // Stencil / clipping
    int32_t draw_mode;              // RIVE_LOVE_DRAW_*
    int32_t clip_index;             // stencil reference value (0 = unclipped)

    // Fill
    int32_t fill_type;              // RIVE_LOVE_FILL_*

    // Solid color (when fill_type == FILL_SOLID)
    float color_r, color_g, color_b, color_a;

    // Gradient (when fill_type != FILL_SOLID)
    float grad_start_x, grad_start_y;
    float grad_end_x, grad_end_y;
    const float* grad_colors;       // RGBA per stop (4 floats × stop_count)
    const float* grad_stops;        // position per stop (stop_count floats)
    int32_t      grad_stop_count;

    // Blend mode
    int32_t blend_mode;             // RIVE_LOVE_BLEND_*

    // Opacity (pre-multiplied into color_a for solid, but separate for gradients)
    float opacity;

    // Image (when fill_type == FILL_IMAGE)
    int32_t      image_id;          // 0 = no image
    const float* uv_coords;        // u,v pairs per vertex (2 floats × vertex_count)
} rive_love_draw_info;

// Get draw command at index [0, draw_count). Returns 0 on success, -1 on error.
int32_t rive_love_scene_get_draw(rive_love_scene* scene,
                                  int32_t index,
                                  rive_love_draw_info* out);

// ── Image query ─────────────────────────────────────────────────────
// Query decoded image data for creating textures on the Lua side.
typedef struct rive_love_image_info {
    int32_t width;
    int32_t height;
    const uint8_t* pixels;      // RGBA, 4 bytes per pixel
    int32_t data_size;           // width * height * 4
} rive_love_image_info;

// Get image pixel data by ID (from draw_info.image_id).
// Returns 0 on success, -1 if image not found.
int32_t rive_love_image_get(rive_love_context* ctx,
                             int32_t image_id,
                             rive_love_image_info* out);

#ifdef __cplusplus
}
#endif
#endif // RIVE_LOVE_H
