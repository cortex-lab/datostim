/*
 * Copyright (c) 2025 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************************************/
/*  Imports                                                                                      */
/*************************************************************************************************/

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include <cglm/cglm.h>

#include <datoviz_protocol.h>
#include <datoviz_types.h>

#include "datostim.h"



/*************************************************************************************************/
/*  Constants                                                                                    */
/*************************************************************************************************/

#define DSTIM_DEFAULT_WIDTH      960
#define DSTIM_DEFAULT_HEIGHT     400
#define DSTIM_DEFAULT_BACKGROUND 127, 127, 127, 255

#define DSTIM_DEFAULT_SQUARE_WIDTH  100
#define DSTIM_DEFAULT_SQUARE_HEIGHT 100

#define DSTIM_MAX_SCREENS 8
#define DSTIM_MAX_LAYERS  16

#define DSTIM_DEFAULT_SQUARE_COLOR 0, 255, 255, 255

#define SQUARE_VERTEX_COUNT 6



/*************************************************************************************************/
/*  Macros                                                                                       */
/*************************************************************************************************/

#define GET_SCREEN                                                                                \
    if (screen_idx >= DSTIM_MAX_SCREENS)                                                          \
    {                                                                                             \
        log_error("screen_idx must be lower than %d", DSTIM_MAX_SCREENS);                         \
        return;                                                                                   \
    }                                                                                             \
    DScreen* screen = &stim->screens[screen_idx];

#define GET_LAYER                                                                                 \
    if (layer_idx >= DSTIM_MAX_LAYERS)                                                            \
    {                                                                                             \
        log_error("layer_idx must be lower than %d", DSTIM_MAX_LAYERS);                           \
        return;                                                                                   \
    }                                                                                             \
    DLayer* layer = &stim->layers[layer_idx];



/*************************************************************************************************/
/*  Typedefs                                                                                     */
/*************************************************************************************************/

typedef struct DScreen DScreen;
typedef struct DLayer DLayer;
typedef struct DStim DStim;
typedef struct DStimSquareVertex DStimSquareVertex;
typedef struct DStimVertex DStimVertex;
typedef struct DStimPush DStimPush;



/*************************************************************************************************/
/*  Enums                                                                                        */
/*************************************************************************************************/

typedef enum
{
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} LogLevel;



/*************************************************************************************************/
/*  Logging                                                                                      */
/*************************************************************************************************/

static const char* level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
static const char* level_colors[] = {"\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m"};

void log_log(LogLevel level, const char* fmt, ...)
{
    // Time string
    char timebuf[20];
    time_t t = time(NULL);
    struct tm* lt = localtime(&t);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", lt);

    // Variadic args
    va_list args;
    va_start(args, fmt);

    // Print to stdout with color and formatting
    fprintf(stderr, "%s[%s] %s: ", level_colors[level], timebuf, level_names[level]);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\x1b[0m\n");

    va_end(args);
}

// Shorthand macros
#define log_trace(...) log_log(LOG_TRACE, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __VA_ARGS__)



/*************************************************************************************************/
/*  Structs                                                                                      */
/*************************************************************************************************/

struct DScreen
{
    uvec2 offset;
    uvec2 size;
    mat4 projection;
};



struct DLayer
{
    mat4 view;

    vec2 tex_offset;
    vec2 tex_size;
    uvec2 img_size;

    cvec4 mask;
    cvec4 min_color;
    cvec4 max_color;

    float tex_angle;
    uint8_t* rgba;

    DStimInterpolation interpolation;
    DStimBlending blending;

    bool is_periodic;
    bool is_visible;
};



struct DStim
{
    DvzApp* app;
    DvzBatch* batch;

    // Window size.
    uint32_t width;
    uint32_t height;

    DvzId canvas_id;

    DvzId background_graphics_id;
    DvzId background_vertex_id;
    DvzId background_params_id;

    DvzId square_graphics_id;
    DvzId square_vertex_id;
    DvzId square_params_id;

    DvzId sphere_graphics_id;
    DvzId sphere_vertex_id;
    DvzId sphere_index_id;

    DvzId texture_id;
    DvzId sampler_id;

    mat4 model;

    uint32_t sphere_index_count;

    uint32_t screen_count;
    DScreen screens[DSTIM_MAX_SCREENS];

    uint32_t layer_count;
    DLayer layers[DSTIM_MAX_LAYERS];
};



/*************************************************************************************************/
/*  GPU structs                                                                                  */
/*************************************************************************************************/

struct DStimSquareVertex
{
    vec3 pos;
};



struct DStimVertex
{
    vec3 vertexPos;
    vec2 vertexUV;
};



struct DStimPush
{
    mat4 projection;
    vec4 min_color;
    vec4 max_color;
    vec2 tex_offset;
    vec2 tex_size;
    float tex_angle;
};



/*************************************************************************************************/
/*  Utils                                                                                        */
/*************************************************************************************************/

static void* read_file(const char* filename, DvzSize* size)
{
    /* The returned pointer must be freed by the caller. */

    void* buffer = NULL;
    DvzSize length = 0;
    FILE* f = fopen(filename, "rb");

    if (!f)
    {
        printf("Could not find %s.\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    length = (DvzSize)ftell(f);
    if (size != NULL)
        *size = length;
    fseek(f, 0, SEEK_SET);
    // NOTE: for safety, add a zero byte at the end to ensure a text file is
    // loaded into a 0-terminated string.
    buffer = (void*)calloc((size_t)length + 1, 1);
    fread(buffer, 1, (size_t)length, f);
    fclose(f);

    return buffer;
}



static void set_shaders_glsl(
    DvzBatch* batch, DvzId graphics_id, const char* vertex_filename, const char* fragment_filename)
{
    // Vertex shader.
    char* vertex_glsl = read_file(vertex_filename, NULL);
    DvzRequest req = dvz_create_glsl(batch, DVZ_SHADER_VERTEX, vertex_glsl);
    FREE(vertex_glsl);

    // Assign the shader to the graphics pipe.
    DvzId square_vertex = req.id;
    dvz_set_shader(batch, graphics_id, square_vertex);

    // Fragment shader.
    char* fragment_glsl = read_file(fragment_filename, NULL);
    req = dvz_create_glsl(batch, DVZ_SHADER_FRAGMENT, fragment_glsl);
    FREE(fragment_glsl);

    // Assign the shader to the graphics pipe.
    DvzId fragment_id = req.id;
    dvz_set_shader(batch, graphics_id, fragment_id);
}



static void set_shaders_spv(
    DvzBatch* batch, DvzId graphics_id, const char* vertex_filename, const char* fragment_filename)
{
    // Vertex shader.
    DvzSize vertex_size = 0;
    unsigned char* vertex_spv = read_file(vertex_filename, &vertex_size);
    DvzRequest req = dvz_create_spirv(batch, DVZ_SHADER_VERTEX, vertex_size, vertex_spv);
    FREE(vertex_spv);

    // Assign the shader to the graphics pipe.
    DvzId square_vertex = req.id;
    dvz_set_shader(batch, graphics_id, square_vertex);

    // Fragment shader.
    DvzSize fragment_size = 0;
    unsigned char* fragment_spv = read_file(fragment_filename, &fragment_size);
    req = dvz_create_spirv(batch, DVZ_SHADER_FRAGMENT, fragment_size, fragment_spv);
    FREE(fragment_spv);

    // Assign the shader to the graphics pipe.
    DvzId fragment_id = req.id;
    dvz_set_shader(batch, graphics_id, fragment_id);
}



/*************************************************************************************************/
/*  Helpers                                                                                      */
/*************************************************************************************************/

static DvzId create_square_pipeline(DvzBatch* batch)
{
    // Create a custom graphics.
    DvzRequest req = dvz_create_graphics(batch, DVZ_GRAPHICS_CUSTOM, 0);
    DvzId graphics_id = req.id;

    set_shaders_spv(batch, graphics_id, "shaders/square.vert.spv", "shaders/square.frag.spv");

    // Primitive topology.
    dvz_set_primitive(batch, graphics_id, DVZ_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    // Polygon mode.
    dvz_set_polygon(batch, graphics_id, DVZ_POLYGON_MODE_FILL);

    // Vertex binding.
    dvz_set_vertex(batch, graphics_id, 0, sizeof(DStimSquareVertex), DVZ_VERTEX_INPUT_RATE_VERTEX);

    // Vertex attrs.
    dvz_set_attr(
        batch, graphics_id, 0, 0, DVZ_FORMAT_R32G32B32_SFLOAT, offsetof(DStimSquareVertex, pos));

    // Slots.
    dvz_set_slot(batch, graphics_id, 0, DVZ_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    return graphics_id;
}



static DvzId create_sphere_pipeline(DvzBatch* batch)
{
    // Create a custom graphics.
    DvzRequest req = dvz_create_graphics(batch, DVZ_GRAPHICS_CUSTOM, 0);
    DvzId graphics_id = req.id;

    set_shaders_spv(batch, graphics_id, "shaders/sphere.vert.spv", "shaders/sphere.frag.spv");

    // Primitive topology.
    dvz_set_primitive(batch, graphics_id, DVZ_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    // dvz_set_primitive(batch, graphics_id, DVZ_PRIMITIVE_TOPOLOGY_POINT_LIST);
    dvz_set_blend(batch, graphics_id, DVZ_BLEND_STANDARD);
    // dvz_set_cull(batch, graphics_id, DVZ_CULL_MODE_BACK);
    dvz_set_front(batch, graphics_id, DVZ_FRONT_FACE_CLOCKWISE);

    // Polygon mode.
    dvz_set_polygon(batch, graphics_id, DVZ_POLYGON_MODE_FILL);

    // Vertex binding.
    dvz_set_vertex(batch, graphics_id, 0, sizeof(DStimVertex), DVZ_VERTEX_INPUT_RATE_VERTEX);

    // Vertex attrs.
    dvz_set_attr(
        batch, graphics_id, 0, 0, //
        DVZ_FORMAT_R32G32B32_SFLOAT, offsetof(DStimVertex, vertexPos));

    dvz_set_attr(
        batch, graphics_id, 0, 1, //
        DVZ_FORMAT_R32G32_SFLOAT, offsetof(DStimVertex, vertexUV));

    // Slots.
    // dvz_set_slot(batch, graphics_id, 0, DVZ_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    dvz_set_slot(batch, graphics_id, 0, DVZ_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    // Push constants.
    dvz_set_push(
        batch, graphics_id, DVZ_SHADER_VERTEX | DVZ_SHADER_FRAGMENT, 0, sizeof(DStimPush));

    return graphics_id;
}



// In normalized device coordinates (whole window = [-1..+1]).
static void upload_rectangle(DvzBatch* batch, DvzId vertex_id, vec2 offset, vec2 shape)
{
    ANN(batch);
    ASSERT(vertex_id != DVZ_ID_NONE);

    float x = offset[0];
    float y = offset[1];
    float w = shape[0];
    float h = shape[1];
    // log_info("pos: %f %f %f %f", x, y, w, h);

    DStimSquareVertex data[] = {

        // lower triangle
        {{x, y, 0}},
        {{x + w, y, 0}},
        {{x, y + h, 0}}, //

        // upper triangle
        {{x + w, y + h, 0}},
        {{x, y + h, 0}},
        {{x + w, y, 0}},

    };

    DvzRequest req = dvz_upload_dat(batch, vertex_id, 0, sizeof(data), data, 0);
}



static void rectangle_color(
    DvzBatch* batch, DvzId params_id, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    ANN(batch);
    ASSERT(params_id != DVZ_ID_NONE);

    // NOTE: from uint8_t to float [0.0, 1.0] for GPU uniform
    vec4 color = {red / 255.0, green / 255.0, blue / 255.0, alpha / 255.0};

    // log_info("color: %f %f %f %f", color[0], color[1], color[2], color[3]);

    DvzRequest req = dvz_upload_dat(batch, params_id, 0, sizeof(vec4), &color, 0);
}



/*************************************************************************************************/
/*  DStim helper functions                                                                       */
/*************************************************************************************************/

static void create_background(DStim* stim)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Create the graphics pipelines.
    stim->background_graphics_id = create_square_pipeline(batch);

    // Create the vertex buffer dat for the background.
    DvzRequest req = dvz_create_dat(
        batch, DVZ_BUFFER_TYPE_VERTEX, SQUARE_VERTEX_COUNT * sizeof(DStimSquareVertex),
        DVZ_DAT_FLAGS_PERSISTENT_STAGING);
    stim->background_vertex_id = req.id;
    req = dvz_bind_vertex(batch, stim->background_graphics_id, 0, stim->background_vertex_id, 0);

    // UBO.
    req = dvz_create_dat(
        batch, DVZ_BUFFER_TYPE_UNIFORM, sizeof(vec4), DVZ_DAT_FLAGS_PERSISTENT_STAGING);
    stim->background_params_id = req.id;
    req = dvz_bind_dat(batch, stim->background_graphics_id, 0, stim->background_params_id, 0);
}



static void create_square(DStim* stim)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Create the graphics pipelines.
    stim->square_graphics_id = create_square_pipeline(batch);

    // Create the vertex buffer dat for the square.
    DvzRequest req = dvz_create_dat(
        batch, DVZ_BUFFER_TYPE_VERTEX, SQUARE_VERTEX_COUNT * sizeof(DStimSquareVertex),
        DVZ_DAT_FLAGS_PERSISTENT_STAGING);
    stim->square_vertex_id = req.id;
    req = dvz_bind_vertex(batch, stim->square_graphics_id, 0, stim->square_vertex_id, 0);

    // UBO.
    req = dvz_create_dat(
        batch, DVZ_BUFFER_TYPE_UNIFORM, sizeof(vec4), DVZ_DAT_FLAGS_PERSISTENT_STAGING);
    stim->square_params_id = req.id;
    req = dvz_bind_dat(batch, stim->square_graphics_id, 0, stim->square_params_id, 0);
}



static void create_sphere_vertex_buffer(DStim* stim, uint32_t sphere_vertex_count)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Create the vertex buffer dat for the sphere.
    DvzRequest req = dvz_create_dat(
        batch, DVZ_BUFFER_TYPE_VERTEX, sphere_vertex_count * sizeof(DStimVertex), 0);
    stim->sphere_vertex_id = req.id;

    req = dvz_bind_vertex(batch, stim->sphere_graphics_id, 0, stim->sphere_vertex_id, 0);
}



static void create_sphere_index_buffer(DStim* stim, uint32_t sphere_index_count)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Create the vertex buffer dat for the sphere.
    DvzRequest req =
        dvz_create_dat(batch, DVZ_BUFFER_TYPE_INDEX, sphere_index_count * sizeof(DvzIndex), 0);
    stim->sphere_index_id = req.id;

    req = dvz_bind_index(batch, stim->sphere_graphics_id, stim->sphere_index_id, 0);
}



static void load_sphere_vertex_data(DStim* stim, uint32_t sphere_vertex_count)
{
    // Load vertex data from disk.
    DvzSize buffer_size = 0;
    DStimVertex* vertices = read_file("data/vertex", &buffer_size);
    ASSERT(buffer_size > 0);
    ASSERT(buffer_size == sphere_vertex_count * sizeof(DStimVertex));
    dstim_vertices(stim, sphere_vertex_count, vertices);
    FREE(vertices);
}



static void load_sphere_index_data(DStim* stim, uint32_t sphere_index_count)
{
    DvzSize buffer_size = 0;
    DvzIndex* indices = read_file("data/index", &buffer_size);
    ASSERT(buffer_size > 0);
    ASSERT(buffer_size == sphere_index_count * sizeof(DvzIndex));
    dstim_indices(stim, sphere_index_count, indices);
    FREE(indices);
}



static void create_texture(DStim* stim)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Texture.
    DvzRequest req = dvz_create_tex(batch, 2, DVZ_FORMAT_R8G8B8A8_UNORM, (uvec3){3, 3, 1}, 0);
    stim->texture_id = req.id;

    req = dvz_create_sampler(batch, DVZ_FILTER_NEAREST, DVZ_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    stim->sampler_id = req.id;

    dvz_bind_tex(
        batch, stim->sphere_graphics_id, 1, stim->texture_id, stim->sampler_id, (uvec3){0, 0, 0});
}



static void load_texture(DStim* stim)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzSize tex_size = 0;
    char* img = read_file("data/img", &tex_size);
    assert(tex_size == 3 * 3 * 4 * 1);
    DvzRequest req = dvz_upload_tex(
        batch, stim->texture_id, (uvec3){0, 0, 0}, (uvec3){3, 3, 1}, tex_size, img, 0);
    FREE(img);
}



/*************************************************************************************************/
/*  DStim functions                                                                              */
/*************************************************************************************************/

DStim* dstim_init(uint32_t width, uint32_t height)
{
    if (width == 0)
    {
        log_error("width cannot be zero");
        return NULL;
    }
    if (height == 0)
    {
        log_error("height cannot be zero");
        return NULL;
    }

    DStim* stim = (DStim*)calloc(1, sizeof(DStim));
    stim->width = width;
    stim->height = height;

    // App.
    // --------------------------------------------------------------------------------------------

    DvzApp* app = dvz_app(0);
    DvzBatch* batch = dvz_app_batch(app);

    stim->app = app;
    stim->batch = batch;

    DvzRequest req = {0};


    // Background.
    // --------------------------------------------------------------------------------------------

    create_background(stim);
    upload_rectangle(batch, stim->background_vertex_id, (vec2){-1, -1}, (vec2){+2, +2});
    dstim_background(stim, DSTIM_DEFAULT_BACKGROUND);


    // Square.
    // --------------------------------------------------------------------------------------------

    create_square(stim);
    uint32_t sw = DSTIM_DEFAULT_SQUARE_WIDTH;
    uint32_t sh = DSTIM_DEFAULT_SQUARE_HEIGHT;
    dstim_square_pos(stim, width - sw, height - sh, sw, sh);
    dstim_square_color(stim, DSTIM_DEFAULT_SQUARE_COLOR);


    // Sphere.
    // --------------------------------------------------------------------------------------------

    uint32_t sphere_vertex_count = 20706;
    stim->sphere_graphics_id = create_sphere_pipeline(batch);

    // Create the vertex buffer dat for the sphere.
    create_sphere_vertex_buffer(stim, sphere_vertex_count);
    load_sphere_vertex_data(stim, sphere_vertex_count); // load from disk

    // Create the index buffer dat for the sphere.// load from disk
    stim->sphere_index_count = 124236;
    create_sphere_index_buffer(stim, stim->sphere_index_count);
    load_sphere_index_data(stim, stim->sphere_index_count);

    create_texture(stim);
    load_texture(stim);


    // Canvas.
    // --------------------------------------------------------------------------------------------

    req = dvz_create_canvas(batch, width, height, DVZ_DEFAULT_CLEAR_COLOR, 0);
    stim->canvas_id = req.id;


    return stim;
}



void dstim_background(DStim* stim, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    ANN(stim);
    rectangle_color(stim->batch, stim->background_params_id, red, green, blue, alpha);
}



void dstim_vertices(DStim* stim, uint32_t vertex_count, DStimVertex* vertices)
{
    ANN(stim);
    ASSERT(vertex_count > 0);
    ANN(vertices);

    DvzSize buffer_size = vertex_count * sizeof(DStimVertex);
    ASSERT(buffer_size > 0);
    ASSERT(stim->sphere_vertex_id != DVZ_ID_NONE);
    DvzRequest req =
        dvz_upload_dat(stim->batch, stim->sphere_vertex_id, 0, buffer_size, vertices, 0);
}



void dstim_indices(DStim* stim, uint32_t index_count, uint32_t* indices)
{
    ANN(stim);
    ASSERT(index_count > 0);
    ANN(indices);

    DvzSize buffer_size = index_count * sizeof(DvzIndex);
    ASSERT(buffer_size > 0);
    ASSERT(stim->sphere_index_id != DVZ_ID_NONE);
    DvzRequest req =
        dvz_upload_dat(stim->batch, stim->sphere_index_id, 0, buffer_size, indices, 0);
}



void dstim_square_pos(DStim* stim, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    ANN(stim);

    // from pixels to NDC
    float xf = -1 + 2.0 * (float)x / (float)stim->width;
    float yf = -1 + 2.0 * (float)y / (float)stim->height;

    float wf = 2.0 * (float)w / (float)stim->width;
    float hf = 2.0 * (float)h / (float)stim->height;

    upload_rectangle(stim->batch, stim->square_vertex_id, (vec2){xf, yf}, (vec2){wf, hf});
}



void dstim_square_color(DStim* stim, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    ANN(stim);
    rectangle_color(stim->batch, stim->square_params_id, red, green, blue, alpha);
}



void dstim_model(DStim* stim, mat4 model)
{
    ANN(stim);
    glm_mat4_copy(model, stim->model);

    // TODO: model_has_changed
}



double dstim_update(DStim* stim)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzId canvas_id = stim->canvas_id;
    ASSERT(canvas_id != DVZ_ID_NONE);

    // HACK: if model_has_changed or view_has_changed:
    //     recreate pipeline with spec constant for view


    // Commands.
    // --------------------------------------------------------------------------------------------

    // Begin recording.
    dvz_record_begin(batch, canvas_id);

    // Viewport.
    dvz_record_viewport(batch, canvas_id, DVZ_DEFAULT_VIEWPORT, DVZ_DEFAULT_VIEWPORT);

    // Background.
    dvz_record_draw(batch, canvas_id, stim->background_graphics_id, 0, SQUARE_VERTEX_COUNT, 0, 1);


    DScreen* screen = NULL;
    DLayer* layer = NULL;
    DStimPush push = {0};

    // Loop over all screens.
    for (uint32_t screen_idx = 0; screen_idx < stim->screen_count; screen_idx++)
    {
        screen = &stim->screens[screen_idx];
        ANN(screen);

        // Screen viewport.
        dvz_record_viewport(
            batch, canvas_id,                             //
            (vec2){screen->offset[0], screen->offset[1]}, //
            (vec2){screen->size[0], screen->size[1]});

        // Push constants.
        glm_mat4_copy(screen->projection, push.projection);

        // Loop over all layers.
        for (uint32_t layer_idx = 0; layer_idx < stim->layer_count; layer_idx++)
        {
            layer = &stim->layers[layer_idx];
            ANN(layer);

            // Do not draw invisible layers.
            if (!layer->is_visible)
                continue;

            // Push constants struct.
            push.min_color[0] = layer->min_color[0] / 255.0;
            push.min_color[1] = layer->min_color[1] / 255.0;
            push.min_color[2] = layer->min_color[2] / 255.0;
            push.min_color[3] = layer->min_color[3] / 255.0;

            push.max_color[0] = layer->max_color[0] / 255.0;
            push.max_color[1] = layer->max_color[1] / 255.0;
            push.max_color[2] = layer->max_color[2] / 255.0;
            push.max_color[3] = layer->max_color[3] / 255.0;

            push.tex_offset[0] = layer->tex_offset[0];
            push.tex_offset[1] = layer->tex_offset[1];

            push.tex_size[0] = layer->tex_size[0];
            push.tex_size[1] = layer->tex_size[1];

            push.tex_angle = layer->tex_angle;

            // TODO: support color mask, will require Datoviz Rendering Protocol updates.

            // Send the push constant to the command buffer.
            dvz_record_push(
                batch, canvas_id, stim->sphere_graphics_id,
                DVZ_SHADER_VERTEX | DVZ_SHADER_FRAGMENT, 0, sizeof(DStimPush), &push);

            // Sphere.
            dvz_record_draw_indexed(
                batch, canvas_id, stim->sphere_graphics_id, 0, 0, stim->sphere_index_count, 0, 1);
        }
    }

    // Square.
    dvz_record_draw(batch, canvas_id, stim->square_graphics_id, 0, SQUARE_VERTEX_COUNT, 0, 1);

    // End recording.
    dvz_record_end(batch, canvas_id);


    // Update the canvas.
    dvz_app_submit(stim->app);

    // TODO: returns the update timestamp when the update has finished
    return 0;
}



void dstim_cleanup(DStim* stim)
{
    ANN(stim);

    // Cleanup.
    dvz_app_destroy(stim->app);
}



/*************************************************************************************************/
/*  Screen                                                                                       */
/*************************************************************************************************/

void dstim_screen(DStim* stim, uint32_t screen_idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    ANN(stim);

    GET_SCREEN
    screen->offset[0] = x;
    screen->offset[1] = y;
    screen->size[0] = w;
    screen->size[1] = h;
}



void dstim_projection(DStim* stim, uint32_t screen_idx, mat4 projection)
{
    ANN(stim);

    GET_SCREEN
    glm_mat4_copy(projection, screen->projection);
}



/*************************************************************************************************/
/*  Layer                                                                                        */
/*************************************************************************************************/

void dstim_layer_texture(
    DStim* stim, uint32_t layer_idx, uint32_t width, uint32_t height, uint8_t* rgba)
{
    ANN(stim);

    GET_LAYER
    layer->img_size[0] = width;
    layer->img_size[1] = height;
    layer->rgba = rgba;
}



void dstim_layer_interpolation(DStim* stim, uint32_t layer_idx, DStimInterpolation interpolation)
{
    ANN(stim);

    GET_LAYER
    layer->interpolation = interpolation;
}



void dstim_layer_periodic(DStim* stim, uint32_t layer_idx, bool is_periodic)
{
    ANN(stim);

    GET_LAYER
    layer->is_periodic = is_periodic;
}



void dstim_layer_blending(DStim* stim, uint32_t layer_idx, DStimBlending blending)
{
    ANN(stim);
    // per-layer blending options (there will be a few predefined options)

    GET_LAYER
    layer->blending = blending;

    /*
    case {'none' ''}
        glBlendFunc(GL.ONE, GL.ZERO);
    case {'dst' 'destination'}
        glBlendFunc(GL.DST_ALPHA, GL.ONE_MINUS_DST_ALPHA);
    case {'src' 'source'}
        glBlendFunc(GL.SRC_ALPHA, GL.ONE_MINUS_SRC_ALPHA);
    case {'1-src' '1-source'}
        glBlendFunc(GL.ONE_MINUS_SRC_ALPHA, GL.SRC_ALPHA);

    */
}



void dstim_layer_mask(DStim* stim, uint32_t layer_idx, bool red, bool green, bool blue, bool alpha)
{
    ANN(stim);

    GET_LAYER
    layer->mask[0] = red;
    layer->mask[1] = green;
    layer->mask[2] = blue;
    layer->mask[3] = alpha;
}



void dstim_layer_view(DStim* stim, uint32_t layer_idx, mat4 view)
{
    ANN(stim);
    // per-layer view matrix

    GET_LAYER
    glm_mat4_copy(view, layer->view);

    /*
    warning: only the first layer's view can be set for now
    HACK: view_has_changed
    */
}



void dstim_layer_angle(DStim* stim, uint32_t layer_idx, float tex_angle)
{
    ANN(stim);

    GET_LAYER
    layer->tex_angle = tex_angle;
}



void dstim_layer_offset(DStim* stim, uint32_t layer_idx, float tex_x, float tex_y)
{
    ANN(stim);

    GET_LAYER
    layer->tex_offset[0] = tex_x;
    layer->tex_offset[1] = tex_y;
}



void dstim_layer_size(DStim* stim, uint32_t layer_idx, float tex_width, float tex_height)
{
    ANN(stim);

    GET_LAYER
    layer->tex_size[0] = tex_width;
    layer->tex_size[1] = tex_height;
}



void dstim_layer_min_color(
    DStim* stim, uint32_t layer_idx, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    ANN(stim);

    GET_LAYER
    layer->min_color[0] = red;
    layer->min_color[1] = green;
    layer->min_color[2] = blue;
    layer->min_color[3] = alpha;
}



void dstim_layer_max_color(
    DStim* stim, uint32_t layer_idx, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    ANN(stim);

    GET_LAYER
    layer->max_color[0] = red;
    layer->max_color[1] = green;
    layer->max_color[2] = blue;
    layer->max_color[3] = alpha;
}



void dstim_layer_toggle(DStim* stim, uint32_t layer_idx, bool is_visible)
{
    ANN(stim);

    GET_LAYER
    layer->is_visible = is_visible;
}



/*************************************************************************************************/
/*  Entry point                                                                                  */
/*************************************************************************************************/

int main(int argc, char** argv)
{
    DStim* stim = dstim_init(DSTIM_DEFAULT_WIDTH, DSTIM_DEFAULT_HEIGHT);

    // Important: run at least once.
    dstim_update(stim);

    // DEBUG
    dvz_app_run(stim->app, 0);

    dstim_cleanup(stim);
    return 0;
}
