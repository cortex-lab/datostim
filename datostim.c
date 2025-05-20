/*
 * Copyright (c) 2025 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************************************/
/*  Imports                                                                                      */
/*************************************************************************************************/

#include <math.h>
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

#define DSTIM_DEFAULT_SQUARE_COLOR     0, 255, 255, 255
#define DSTIM_ALTERNATIVE_SQUARE_COLOR 255, 255, 0, 255

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
    stim->screen_count = MAX(stim->screen_count, screen_idx + 1);                                 \
    DScreen* screen = &stim->screens[screen_idx];

#define GET_LAYER                                                                                 \
    if (layer_idx >= DSTIM_MAX_LAYERS)                                                            \
    {                                                                                             \
        log_error("layer_idx must be lower than %d", DSTIM_MAX_LAYERS);                           \
        return;                                                                                   \
    }                                                                                             \
    stim->layer_count = MAX(stim->layer_count, layer_idx + 1);                                    \
    DLayer* layer = &stim->layers[layer_idx];

#define TOUCH_LAYER         layer->is_dirty = true;
#define TOUCH_LAYER_TEXTURE layer->is_texture_dirty = true;



/*************************************************************************************************/
/*  Typedefs                                                                                     */
/*************************************************************************************************/

typedef struct DScreen DScreen;
typedef struct DLayer DLayer;
typedef struct DStim DStim;
typedef struct DStimSquareVertex DStimSquareVertex;
typedef struct DStimVertex DStimVertex;
typedef struct DStimPush DStimPush;
// typedef struct DStimParams DStimParams;



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

    int mask;
    cvec4 min_color;
    cvec4 max_color;

    float tex_angle;

    // texture width and height
    uint32_t tex_width;
    uint32_t tex_height;

    // Texture data.
    DvzSize tex_nbytes;
    uint8_t* rgba;

    DvzFormat format;
    DStimInterpolation interpolation;
    DStimBlend blend;

    bool is_periodic;
    bool is_visible;       // false by default
    bool is_blank;         // need to prepare the pipeline
    bool is_dirty;         // need to update the layer's parameters with push constant
    bool is_texture_dirty; // need to upload the texture data again
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

    // NOTE: for now, 1 graphics pipeline per layer to support multiple fixed states and texture
    // bindings (multiple descriptors per pipeline not yet supported by Datoviz Rendering
    // Protocol).
    DvzId sphere_graphics_ids[DSTIM_MAX_LAYERS];
    DvzId sphere_vertex_id;
    DvzId sphere_index_id;

    // NOTE: 1 texture and sampler per layer (hence, per sphere graphics pipeline).
    DvzId texture_ids[DSTIM_MAX_LAYERS];
    DvzId sampler_ids[DSTIM_MAX_LAYERS];

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



// NOTE: maxPushConstantsSize needs to be >= 256 on the GPU
struct DStimPush
{
    mat4 model;
    mat4 view;
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

static void* _cpy(DvzSize size, const void* data)
{
    if (data == NULL)
        return NULL;
    void* data_cpy = malloc(size);
    memcpy(data_cpy, data, size);
    return data_cpy;
}



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

    // DEBUG
    // dvz_set_primitive(batch, graphics_id, DVZ_PRIMITIVE_TOPOLOGY_POINT_LIST);

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
}



static void bind_sphere_vertex_buffer(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    ASSERT(layer_idx < DSTIM_MAX_LAYERS);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    dvz_bind_vertex(batch, stim->sphere_graphics_ids[layer_idx], 0, stim->sphere_vertex_id, 0);
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
}



static void bind_sphere_index_buffer(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    ASSERT(layer_idx < DSTIM_MAX_LAYERS);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    dvz_bind_index(batch, stim->sphere_graphics_ids[layer_idx], stim->sphere_index_id, 0);
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



static void
create_texture(DStim* stim, uint32_t layer_idx, DvzFormat format, uint32_t width, uint32_t height)
{
    ANN(stim);
    GET_LAYER

    ASSERT(width > 0);
    ASSERT(height > 0);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Texture.
    DvzRequest req = dvz_create_tex(batch, 2, format, (uvec3){width, height, 1}, 0);
    stim->texture_ids[layer_idx] = req.id;
}



static void create_sampler(
    DStim* stim, uint32_t layer_idx, DvzFilter filter, DvzSamplerAddressMode address_mode)
{
    ANN(stim);
    ASSERT(layer_idx < DSTIM_MAX_LAYERS);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzRequest req = dvz_create_sampler(batch, filter, address_mode);
    stim->sampler_ids[layer_idx] = req.id;
}



static void bind_texture(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    ASSERT(layer_idx < DSTIM_MAX_LAYERS);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzId tex_id = stim->texture_ids[layer_idx];
    ASSERT(tex_id != DVZ_ID_NONE);

    dvz_bind_tex(
        batch, stim->sphere_graphics_ids[layer_idx], 0, tex_id, stim->sampler_ids[layer_idx],
        (uvec3){0, 0, 0});
}



static void upload_texture(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzId tex_id = stim->texture_ids[layer_idx];
    ASSERT(tex_id != DVZ_ID_NONE);

    uint32_t width = layer->tex_width;
    uint32_t height = layer->tex_height;
    DvzSize tex_nbytes = layer->tex_nbytes;
    uint8_t* rgba = layer->rgba;

    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(tex_nbytes > 0);
    ANN(rgba);

    dvz_upload_tex(
        stim->batch, tex_id, (uvec3){0, 0, 0}, (uvec3){width, height, 1}, tex_nbytes, rgba, 0);
}



static void set_blend(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzBlendType blend = DVZ_BLEND_DISABLE;

    if (layer->blend == DSTIM_BLEND_DST)
    {
        blend = DVZ_BLEND_DESTINATION;
    }

    dvz_set_blend(batch, stim->sphere_graphics_ids[layer_idx], blend);
}



static void set_mask(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    dvz_set_mask(batch, stim->sphere_graphics_ids[layer_idx], layer->mask);
}



static void prepare_sphere_pipeline(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    stim->sphere_graphics_ids[layer_idx] = create_sphere_pipeline(batch);

    // Bind buffers to the new pipeline.
    bind_sphere_vertex_buffer(stim, layer_idx);
    bind_sphere_index_buffer(stim, layer_idx);

    // Create texture.
    DvzFormat format = layer->format;
    uint32_t width = layer->tex_width;
    uint32_t height = layer->tex_height;

    // NOTE TODO: cannot change texture size after layer creation. Need to emit texture resize
    // request.
    create_texture(stim, layer_idx, format, width, height);

    // Create sampler.
    DvzFilter filter = layer->interpolation == DSTIM_INTERPOLATION_NEAREST ? DVZ_FILTER_NEAREST
                                                                           : DVZ_FILTER_LINEAR;

    DvzSamplerAddressMode address_mode = layer->is_periodic
                                             ? DVZ_SAMPLER_ADDRESS_MODE_REPEAT
                                             : DVZ_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

    create_sampler(stim, layer_idx, filter, address_mode);

    // Bind the texture and sampler to the layer's pipeline.
    bind_texture(stim, layer_idx);

    // NOTE: address the case where these parameters change afterwards.
    set_blend(stim, layer_idx);
    set_mask(stim, layer_idx);
}



static void push_sphere_pipeline(DStim* stim, uint32_t layer_idx, DStimPush* push)
{
    ANN(stim);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Send the push constant to the command buffer.
    dvz_record_push(
        batch, stim->canvas_id, stim->sphere_graphics_ids[layer_idx],
        DVZ_SHADER_VERTEX | DVZ_SHADER_FRAGMENT, 0, sizeof(DStimPush), push);
}



static void draw_sphere_pipeline(DStim* stim, uint32_t layer_idx)
{
    ANN(stim);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Sphere.
    dvz_record_draw_indexed(
        batch, stim->canvas_id, stim->sphere_graphics_ids[layer_idx], 0, 0,
        stim->sphere_index_count, 0, 1);
}



static void fill_push(DStim* stim, uint32_t layer_idx, mat4 projection, DStimPush* push)
{
    ANN(stim);
    ANN(push);
    GET_LAYER

    DvzBatch* batch = stim->batch;
    ANN(batch);

    // Per-screen projection matrix.
    glm_mat4_copy(projection, push->projection);

    // Per-layer view matrix.
    glm_mat4_copy(layer->view, push->view);

    // Push constants parameters.
    push->min_color[0] = layer->min_color[0] / 255.0;
    push->min_color[1] = layer->min_color[1] / 255.0;
    push->min_color[2] = layer->min_color[2] / 255.0;
    push->min_color[3] = layer->min_color[3] / 255.0;

    push->max_color[0] = layer->max_color[0] / 255.0;
    push->max_color[1] = layer->max_color[1] / 255.0;
    push->max_color[2] = layer->max_color[2] / 255.0;
    push->max_color[3] = layer->max_color[3] / 255.0;

    push->tex_offset[0] = layer->tex_offset[0];
    push->tex_offset[1] = layer->tex_offset[1];

    push->tex_size[0] = layer->tex_size[0];
    push->tex_size[1] = layer->tex_size[1];

    push->tex_angle = layer->tex_angle;
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
    for (uint32_t i = 0; i < DSTIM_MAX_LAYERS; i++)
    {
        stim->layers[i].is_blank = true;
    }

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

    // Create the vertex buffer dat for the sphere.
    create_sphere_vertex_buffer(stim, sphere_vertex_count);
    load_sphere_vertex_data(stim, sphere_vertex_count); // load from disk

    // Create the index buffer dat for the sphere.// load from disk
    stim->sphere_index_count = 124236;
    create_sphere_index_buffer(stim, stim->sphere_index_count);
    load_sphere_index_data(stim, stim->sphere_index_count);


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
}



void dstim_cleanup(DStim* stim)
{
    ANN(stim);

    // Cleanup.
    dvz_app_destroy(stim->app);

    // Free texture copies in layers.
    for (uint32_t layer_idx = 0; layer_idx < stim->layer_count; layer_idx++)
    {
        if (stim->layers[layer_idx].rgba != NULL)
        {
            FREE(stim->layers[layer_idx].rgba);
        }
    }

    FREE(stim);
}



/*************************************************************************************************/
/*  Events                                                                                       */
/*************************************************************************************************/

void dstim_mouse(DStim* stim, double* x, double* y, DvzMouseButton* button)
{
    ANN(stim);
    ANN(stim->app);
    dvz_app_mouse(stim->app, stim->canvas_id, x, y, button);
}



void dstim_keyboard(DStim* stim, DvzKeyCode* key)
{
    ANN(stim);
    ANN(stim->app);
    dvz_app_keyboard(stim->app, stim->canvas_id, key);
}



static inline double _time_to_double(uint64_t seconds, uint64_t nanoseconds)
{
    return (double)seconds + (double)nanoseconds * 1e-9;
}



double dstim_time(DStim* stim)
{
    DvzTime time = {0};
    dvz_time(&time);
    return _time_to_double(time.seconds, time.nanoseconds);
}



double dstim_frame_time(DStim* stim)
{
    ANN(stim);
    ANN(stim->app);
    dvz_app_wait(stim->app);

    // Return the presentation time.
    uint64_t seconds = 0;
    uint64_t nanoseconds = 0;
    dvz_app_timestamps(stim->app, stim->canvas_id, 1, &seconds, &nanoseconds);
    return _time_to_double(seconds, nanoseconds);
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
    DStim* stim, uint32_t layer_idx, DvzFormat format, //
    uint32_t width, uint32_t height, DvzSize tex_nbytes, uint8_t* rgba)
{
    ANN(stim);

    ASSERT(tex_nbytes > 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ANN(rgba);

    GET_LAYER
    TOUCH_LAYER_TEXTURE

    layer->format = format;
    layer->tex_width = width;
    layer->tex_height = height;
    layer->tex_nbytes = tex_nbytes;

    // Free the existing copy if a new one is passed.
    if (layer->rgba != NULL)
    {
        FREE(layer->rgba);
    }
    layer->rgba =
        _cpy(tex_nbytes, rgba); // NOTE: make a copy for safety, but will need to free it.
}



void dstim_layer_interpolation(DStim* stim, uint32_t layer_idx, DStimInterpolation interpolation)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
    layer->interpolation = interpolation;
}



void dstim_layer_periodic(DStim* stim, uint32_t layer_idx, bool is_periodic)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
    layer->is_periodic = is_periodic;
}



void dstim_layer_blend(DStim* stim, uint32_t layer_idx, DStimBlend blend)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
    layer->blend = blend;
}



void dstim_layer_mask(DStim* stim, uint32_t layer_idx, bool red, bool green, bool blue, bool alpha)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER

    layer->mask = (red ? DVZ_MASK_COLOR_R : 0) |   //
                  (green ? DVZ_MASK_COLOR_G : 0) | //
                  (blue ? DVZ_MASK_COLOR_B : 0) |  //
                  (alpha ? DVZ_MASK_COLOR_A : 0);  //
}



void dstim_layer_view(DStim* stim, uint32_t layer_idx, mat4 view)
{
    ANN(stim);
    // per-layer view matrix

    GET_LAYER
    TOUCH_LAYER
    glm_mat4_copy(view, layer->view);
}



void dstim_layer_angle(DStim* stim, uint32_t layer_idx, float tex_angle)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
    layer->tex_angle = tex_angle;
}



void dstim_layer_offset(DStim* stim, uint32_t layer_idx, float tex_x, float tex_y)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
    layer->tex_offset[0] = tex_x;
    layer->tex_offset[1] = tex_y;
}



void dstim_layer_size(DStim* stim, uint32_t layer_idx, float tex_size_x, float tex_size_y)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
    layer->tex_size[0] = tex_size_x;
    layer->tex_size[1] = tex_size_y;
}



void dstim_layer_min_color(
    DStim* stim, uint32_t layer_idx, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    ANN(stim);

    GET_LAYER
    TOUCH_LAYER
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
    TOUCH_LAYER
    layer->max_color[0] = red;
    layer->max_color[1] = green;
    layer->max_color[2] = blue;
    layer->max_color[3] = alpha;
}



void dstim_layer_show(DStim* stim, uint32_t layer_idx, bool is_visible)
{
    ANN(stim);

    GET_LAYER
    layer->is_visible = is_visible;
}



/*************************************************************************************************/
/*  Draw function                                                                                */
/*************************************************************************************************/

void dstim_update(DStim* stim)
{
    ANN(stim);

    DvzBatch* batch = stim->batch;
    ANN(batch);

    DvzId canvas_id = stim->canvas_id;
    ASSERT(canvas_id != DVZ_ID_NONE);

    // Begin recording.
    dvz_record_begin(batch, canvas_id);

    // Viewport.
    dvz_record_viewport(batch, canvas_id, (vec2){0, 0}, (vec2){stim->width, stim->height});

    // Background.
    dvz_record_draw(batch, canvas_id, stim->background_graphics_id, 0, SQUARE_VERTEX_COUNT, 0, 1);


    DScreen* screen = NULL;
    DLayer* layer = NULL;
    DStimPush push = {0};
    DvzId sphere_graphics_id = DVZ_ID_NONE;

    // Global model matrix.
    glm_mat4_copy(stim->model, push.model);

    // First pass: go through all layers and prepare them if needed.
    for (uint32_t layer_idx = 0; layer_idx < stim->layer_count; layer_idx++)
    {
        layer = &stim->layers[layer_idx];
        ANN(layer);

        // Only once per application: create the pipeline, texture, sampler, and make the bindings.
        if (layer->is_blank)
        {
            log_debug("layer %d: prepare sphere pipeline", layer_idx);
            prepare_sphere_pipeline(stim, layer_idx);
            layer->is_blank = false;
        }

        // Every time the texture data changes: upload it.
        if (layer->is_texture_dirty)
        {
            log_debug("layer %d: upload texture", layer_idx);
            upload_texture(stim, layer_idx);
            layer->is_texture_dirty = false;
        }
    }


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

        // Loop over all layers.
        for (uint32_t layer_idx = 0; layer_idx < stim->layer_count; layer_idx++)
        {
            layer = &stim->layers[layer_idx];
            ANN(layer);

            // Do not draw invisible layers.
            if (!layer->is_visible)
                continue;

            log_debug("layer %d: record draw command", layer_idx);
            fill_push(stim, layer_idx, screen->projection, &push);
            push_sphere_pipeline(stim, layer_idx, &push);
            draw_sphere_pipeline(stim, layer_idx);

            // TODO: use dynamic state instead

            // NOTE TODO: optimization: once fixed state updates are supported in DRP, avoid
            // recreating the pipeline if the states have not changed (or perhaps this should be
            // done by the renderer)
        }
    }

    // Square.
    dvz_record_viewport(batch, canvas_id, (vec2){0, 0}, (vec2){stim->width, stim->height});
    dvz_record_draw(batch, canvas_id, stim->square_graphics_id, 0, SQUARE_VERTEX_COUNT, 0, 1);

    // End recording.
    dvz_record_end(batch, canvas_id);


    // Update the canvas.
    dvz_app_submit(stim->app);
}



/*************************************************************************************************/
/*  Entry point                                                                                  */
/*************************************************************************************************/

static void _on_timer(DvzApp* app, DvzId window_id, DvzTimerEvent* ev)
{
    DStim* stim = (DStim*)ev->user_data;
    assert(stim != NULL);

    // Get current time.
    double time = dstim_time(stim);

    // Get mouse state.
    double x = 0;
    double y = 0;
    DvzMouseButton button = {0};
    dstim_mouse(stim, &x, &y, &button);

    // Get keyboard state.
    DvzKeyCode key = {0};
    dstim_keyboard(stim, &key);

    // Display information.
    // log_info("time: %.3f, mouse (%.0f, %.0f), button %d, keyboard %d", time, x, y, button, key);

    double offset = -90 + 30 * fmod(ev->time, 5.0);
    dstim_layer_offset(stim, 0, offset, 0);
    dstim_layer_offset(stim, 1, offset, 0);

    // Sync square.
    if (ev->step_idx % 2 == 0)
        dstim_square_color(stim, DSTIM_DEFAULT_SQUARE_COLOR);
    else
        dstim_square_color(stim, DSTIM_ALTERNATIVE_SQUARE_COLOR);

    dstim_update(stim);
}


int main(int argc, char** argv)
{
    DStim* stim = dstim_init(DSTIM_DEFAULT_WIDTH, DSTIM_DEFAULT_HEIGHT);

    // Model.
    mat4* model = read_file("data/model", NULL);
    dstim_model(stim, *model);
    FREE(model);

    // Screens.
    {
        float w3 = (float)DSTIM_DEFAULT_WIDTH / 3.0;
        float h = (float)DSTIM_DEFAULT_HEIGHT;

        dstim_screen(stim, 0, 0 * w3, 0, w3, h);
        dstim_screen(stim, 1, 1 * w3, 0, w3, h);
        dstim_screen(stim, 2, 2 * w3, 0, w3, h);

        mat4* proj1 = read_file("data/screen1", NULL);
        mat4* proj2 = read_file("data/screen2", NULL);
        mat4* proj3 = read_file("data/screen3", NULL);

        dstim_projection(stim, 0, *proj1);
        dstim_projection(stim, 1, *proj2);
        dstim_projection(stim, 2, *proj3);

        FREE(proj1);
        FREE(proj2);
        FREE(proj3);
    }

    mat4* view = read_file("data/view", NULL);

    // Layers.
    if (0)
    {
        uint32_t width = 3;
        uint32_t height = 3;
        DvzSize tex_nbytes = 0;
        uint8_t* rgba = read_file("data/img", &tex_nbytes);
        ASSERT(tex_nbytes > 0);
        ASSERT(tex_nbytes == width * height * 4 * sizeof(uint8_t));
        dstim_layer_texture(stim, 0, DVZ_FORMAT_R8G8B8A8_UNORM, width, height, tex_nbytes, rgba);
        FREE(rgba);

        // Layer parameters.
        dstim_layer_blend(stim, 0, DSTIM_BLEND_NONE);
        dstim_layer_periodic(stim, 0, false);
        dstim_layer_angle(stim, 0, 45.0);
        dstim_layer_offset(stim, 0, 0, 0);
        dstim_layer_mask(stim, 0, true, true, true, true);
        dstim_layer_size(stim, 0, 150, 150);
        dstim_layer_min_color(stim, 0, 0, 0, 0, 0);
        dstim_layer_max_color(stim, 0, 255, 255, 255, 255);
        dstim_layer_view(stim, 0, *view);
        dstim_layer_show(stim, 0, true);
    }

    // Layer 0: Gaussian stencil.
    if (1)
    {
        uint32_t width = 61;
        uint32_t height = 61;
        DvzSize tex_nbytes = 0;
        uint8_t* rgba = read_file("data/gaussianStencil", &tex_nbytes);
        ASSERT(tex_nbytes > 0);
        ASSERT(tex_nbytes == width * height * 4 * sizeof(uint8_t));
        dstim_layer_texture(stim, 0, DVZ_FORMAT_R8G8B8A8_UNORM, width, height, tex_nbytes, rgba);
        FREE(rgba);

        // Layer parameters.
        dstim_layer_blend(stim, 0, DSTIM_BLEND_NONE);
        dstim_layer_mask(stim, 0, false, false, false, true);
        dstim_layer_interpolation(stim, 0, DSTIM_INTERPOLATION_LINEAR);
        dstim_layer_periodic(stim, 0, false);
        dstim_layer_view(stim, 0, *view);
        dstim_layer_angle(stim, 0, 0.0);
        dstim_layer_offset(stim, 0, -90, 0);
        dstim_layer_size(stim, 0, 64.8, 64.8);
        dstim_layer_min_color(stim, 0, 0, 0, 0, 0);
        dstim_layer_max_color(stim, 0, 255, 255, 255, 255);
        dstim_layer_show(stim, 0, true);
    }

    // Layer 1: sinusoid grating.
    if (1)
    {
        uint32_t width = 37;
        uint32_t height = 1;
        DvzSize tex_nbytes = 0;
        uint8_t* rgba = read_file("data/sinusoidGrating", &tex_nbytes);
        ASSERT(tex_nbytes > 0);
        ASSERT(tex_nbytes == width * height * 4 * sizeof(uint8_t));
        dstim_layer_texture(stim, 1, DVZ_FORMAT_R8G8B8A8_UNORM, width, height, tex_nbytes, rgba);
        FREE(rgba);

        // Layer parameters.
        dstim_layer_view(stim, 1, *view);
        dstim_layer_blend(stim, 1, DSTIM_BLEND_DST);
        dstim_layer_mask(stim, 1, true, true, true, true);
        dstim_layer_interpolation(stim, 1, DSTIM_INTERPOLATION_LINEAR);
        dstim_layer_periodic(stim, 1, true);
        dstim_layer_angle(stim, 1, 0.0);
        dstim_layer_offset(stim, 1, -90, 0);
        dstim_layer_size(stim, 1, 5.2632, 180);
        dstim_layer_min_color(stim, 1, 0, 0, 0, 0);
        dstim_layer_max_color(stim, 1, 255, 255, 255, 255);
        dstim_layer_show(stim, 1, true);
    }


    // Important: run at least once.
    dstim_update(stim);

    // Timer.
    float dt = 0.05;
    dvz_app_timer(stim->app, 0, dt, 0);
    dvz_app_on_timer(stim->app, _on_timer, stim);

    // DEBUG
    dvz_app_run(stim->app, 0);

    // Cleanup.
    dstim_cleanup(stim);
    FREE(view);
    return 0;
}
