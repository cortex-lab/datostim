/*
 * Copyright (c) 2025 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************************************/
/*  Datostim header file                                                                         */
/*************************************************************************************************/

#ifndef DSTIM_HEADER
#define DSTIM_HEADER



/*************************************************************************************************/
/*  Macros                                                                                       */
/*************************************************************************************************/

#ifdef __cplusplus
#define LANG_CPP
#define EXTERN_C_ON                                                                               \
    extern "C"                                                                                    \
    {
#define EXTERN_C_OFF }
#else
#define LANG_C
#define EXTERN_C_ON
#define EXTERN_C_OFF
#endif

#ifndef DSTIM_EXPORT
#if CC_MSVC
#ifdef DSTIM_SHARED
#define DSTIM_EXPORT __declspec(dllexport)
#else
#define DSTIM_EXPORT __declspec(dllimport)
#endif
#define DSTIM_INLINE __forceinline
#else
#define DSTIM_EXPORT __attribute__((visibility("default")))
#define DSTIM_INLINE static inline __attribute((always_inline))
#endif
#endif

#ifndef ASSERT
#define ASSERT(x) assert(x)
#endif


#ifndef ANN
#define ANN(x) ASSERT((x) != NULL);
#endif



/*************************************************************************************************/
/*  Typedefs                                                                                     */
/*************************************************************************************************/

// Forward declarations.
typedef struct DStim DStim;
typedef struct DStimVertex DStimVertex;



/*************************************************************************************************/
/*  Enums                                                                                        */
/*************************************************************************************************/

typedef enum
{
    DSTIM_INTERPOLATION_NEAREST = 0,
    DSTIM_INTERPOLATION_LINEAR = 1,
} DStimInterpolation;



typedef enum
{
    DSTIM_BLENDING_NONE,
    DSTIM_BLENDING_DST,
    DSTIM_BLENDING_SRC,
    DSTIM_BLENDING_ONE_MINUS_SRC,
} DStimBlending;

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



EXTERN_C_ON

/*************************************************************************************************/
/*  Functions                                                                                    */
/*************************************************************************************************/

DSTIM_EXPORT DStim* dstim_init(uint32_t width, uint32_t height); // open a fixed window



DSTIM_EXPORT void
dstim_background(DStim* stim, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);



DSTIM_EXPORT void dstim_vertices(
    DStim* stim, uint32_t vertex_count,
    DStimVertex* vertices); // by default, will automatically set the sphere vertices



DSTIM_EXPORT void dstim_indices(
    DStim* stim, uint32_t index_count,
    uint32_t* indices); // by default, will automatically set the sphere indices



DSTIM_EXPORT void dstim_square_pos(
    DStim* stim, uint32_t x, uint32_t y, uint32_t w, uint32_t h); // position and size in pixels



DSTIM_EXPORT void
dstim_square_color(DStim* stim, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);



DSTIM_EXPORT void dstim_model(DStim* stim, mat4 model); // global model matrix



DSTIM_EXPORT void
dstim_screen(DStim* stim, uint32_t screen_idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h);



DSTIM_EXPORT void dstim_projection(DStim* stim, uint32_t screen_idx, mat4 projection);



DSTIM_EXPORT void dstim_layer_texture(
    DStim* stim, uint32_t layer_idx, DvzFormat format, uint32_t width, uint32_t height,
    DvzSize tex_nbytes, uint8_t* rgba);



DSTIM_EXPORT void dstim_layer_interpolation(
    DStim* stim, uint32_t layer_idx, DStimInterpolation interpolation); // 0=nearest, 1=linear



DSTIM_EXPORT void dstim_layer_periodic(DStim* stim, uint32_t layer_idx, bool is_periodic);



DSTIM_EXPORT void dstim_layer_blending(
    DStim* stim, uint32_t layer_idx,
    DStimBlending blending); // per-layer blending options (there will be a few predefined options)



DSTIM_EXPORT void
dstim_layer_mask(DStim* stim, uint32_t layer_idx, bool red, bool green, bool blue, bool alpha);



DSTIM_EXPORT void
dstim_layer_view(DStim* stim, uint32_t layer_idx, mat4 view); // per-layer view matrix



DSTIM_EXPORT void dstim_layer_angle(DStim* stim, uint32_t layer_idx, float tex_angle);



DSTIM_EXPORT void dstim_layer_offset(DStim* stim, uint32_t layer_idx, float tex_x, float tex_y);



DSTIM_EXPORT void
dstim_layer_size(DStim* stim, uint32_t layer_idx, float tex_size_x, float tex_size_y);



DSTIM_EXPORT void dstim_layer_min_color(
    DStim* stim, uint32_t layer_idx, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);



DSTIM_EXPORT void dstim_layer_max_color(
    DStim* stim, uint32_t layer_idx, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);



DSTIM_EXPORT void dstim_layer_show(DStim* stim, uint32_t layer_idx, bool is_visible); // show/hide



DSTIM_EXPORT double
dstim_update(DStim* stim); // send all updates since that last call to this function to the GPU,
                           // and returns the update timestamp when the update has finished



DSTIM_EXPORT void dstim_cleanup(DStim* stim); // close the window



EXTERN_C_OFF

#endif
