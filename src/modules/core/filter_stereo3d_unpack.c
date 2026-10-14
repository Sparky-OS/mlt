/*
 * filter_stereo3d_unpack.c -- take the views out of a stereoscopic 3D video
 * Copyright (C) 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <framework/mlt_factory.h>
#include <framework/mlt_filter.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_log.h>
#include <framework/mlt_pool.h>
#include <framework/mlt_producer.h>

#ifdef HAVE_STEREO_POLICY
#include <stereo-policy.h>
#endif

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// The packings, named as the avformat producer's meta.media.stereo3d.type names them (the frame packing
// arrangement types of H.264 Table D-8 and H.265 Table D.8, Matroska's StereoMode in RFC 9559)
typedef enum {
    packing_none,
    packing_side_by_side,
    packing_top_and_bottom,
    packing_frame_sequence,
    packing_checkerboard,
    packing_side_by_side_quincunx,
    packing_lines,
    packing_columns,
} packing_t;

static packing_t packing_from_name(const char *name)
{
    static const struct
    {
        const char *name;
        packing_t packing;
    } names[] = {
        {"side_by_side", packing_side_by_side},
        {"top_and_bottom", packing_top_and_bottom},
        {"frame_sequence", packing_frame_sequence},
        {"checkerboard", packing_checkerboard},
        {"side_by_side_quincunx", packing_side_by_side_quincunx},
        {"lines", packing_lines},
        {"columns", packing_columns},
    };
    if (name) {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!strcmp(name, names[i].name))
                return names[i].packing;
        }
    }
    return packing_none;
}

typedef struct
{
    uint8_t *data;
    int width;
    int height;
} view_t;

typedef struct
{
    pthread_mutex_t mutex;
    // A frame sequence has one view in each frame: the views of the pair the frame is in, with the places they are from
    view_t latest[2];
    mlt_position latest_position[2];
    // A producer of our own for the other frame of a pair: the one the frame came from is driven by whoever plays it
    mlt_producer partner;
    char *partner_resource;
} private_data;

// Whether the views of a side by side or top and bottom frame are half or full size: the producer says when it can
// (meta.media.stereo3d.views, from what the stream says: a Matroska track's plane size, then the shape), else the
// edition's shared rule judges the frame as it is (the stereo policy library). The views of the other packings are shown
// at the frame's size. Without the library, views are taken for half size.
static int full_views(mlt_frame frame, packing_t packing, int width, int height)
{
    mlt_producer original = mlt_frame_get_original_producer(frame);
    const char *views = original ? mlt_properties_get(MLT_PRODUCER_PROPERTIES(original), "meta.media.stereo3d.views") : NULL;
    if (views)
        return !strcmp(views, "full");
#ifdef HAVE_STEREO_POLICY
    int layout = packing == packing_side_by_side ? STEREO_POLICY_LAYOUT_SIDE_BY_SIDE
                 : packing == packing_top_and_bottom ? STEREO_POLICY_LAYOUT_TOP_AND_BOTTOM
                                                     : STEREO_POLICY_LAYOUT_SIDE_BY_SIDE_QUINCUNX;
    double sar = mlt_frame_get_aspect_ratio(frame);
    int sar_num = sar > 0 ? (int) (sar * 10000 + 0.5) : 0;
    return stereo_policy_video_views(width, height, sar_num, 10000, 0, 0, STEREO_POLICY_UPSAMPLED_UNKNOWN, layout, NULL) == 1;
#else
    return 0;
#endif
}

static void view_size(mlt_frame frame, packing_t packing, int width, int height, int *view_width, int *view_height)
{
    *view_width = width;
    *view_height = height;
    if (packing != packing_side_by_side && packing != packing_side_by_side_quincunx && packing != packing_top_and_bottom)
        return;
    if (!full_views(frame, packing, width, height))
        return;
    if (packing == packing_top_and_bottom)
        *view_height = height / 2;
    else
        *view_width = width / 2;
}

// The samples one view has in the frame, slot 0 being the first view of the packing and 1 the second. The first view
// is at the left, on top, in the first row, in the first column, or at the top left pixel.
static void stored_view(
    const uint8_t *src, int width, int height, packing_t packing, int slot, uint8_t *dst, int stored_width, int stored_height)
{
    const int stride = width * 4;
    for (int y = 0; y < stored_height; y++) {
        uint8_t *out = dst + (size_t) y * stored_width * 4;
        switch (packing) {
        case packing_side_by_side:
        case packing_side_by_side_quincunx:
            memcpy(out, src + (size_t) y * stride + (size_t) slot * stored_width * 4, (size_t) stored_width * 4);
            break;
        case packing_top_and_bottom:
            memcpy(out, src + (size_t) (y + slot * stored_height) * stride, (size_t) stored_width * 4);
            break;
        case packing_lines:
            memcpy(out, src + (size_t) (2 * y + slot) * stride, (size_t) stored_width * 4);
            break;
        case packing_columns:
            for (int x = 0; x < stored_width; x++)
                memcpy(out + (size_t) x * 4, src + (size_t) y * stride + (size_t) (2 * x + slot) * 4, 4);
            break;
        case packing_checkerboard:
            for (int x = 0; x < stored_width; x++)
                memcpy(out + (size_t) x * 4, src + (size_t) y * stride + (size_t) (2 * x + ((slot + y) & 1)) * 4, 4);
            break;
        default:
            break;
        }
    }
}

// Bilinear scaling of the samples of a view to the size it is shown at
static void scale_view(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        double fy = dh > 1 && sh > 1 ? (double) y * (sh - 1) / (dh - 1) : 0;
        int y0 = (int) fy;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        double wy = fy - y0;
        for (int x = 0; x < dw; x++) {
            double fx = dw > 1 && sw > 1 ? (double) x * (sw - 1) / (dw - 1) : 0;
            int x0 = (int) fx;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            double wx = fx - x0;
            for (int c = 0; c < 4; c++) {
                double top = src[((size_t) y0 * sw + x0) * 4 + c] * (1 - wx) + src[((size_t) y0 * sw + x1) * 4 + c] * wx;
                double bottom = src[((size_t) y1 * sw + x0) * 4 + c] * (1 - wx) + src[((size_t) y1 * sw + x1) * 4 + c] * wx;
                dst[((size_t) y * dw + x) * 4 + c] = (uint8_t) (top * (1 - wy) + bottom * wy + 0.5);
            }
        }
    }
}

static void set_view(view_t *view, const uint8_t *data, int width, int height)
{
    if (view->width != width || view->height != height) {
        mlt_pool_release(view->data);
        view->data = mlt_pool_alloc((size_t) width * height * 4);
        view->width = width;
        view->height = height;
    }
    memcpy(view->data, data, (size_t) width * height * 4);
}

static int filter_get_image(mlt_frame frame,
                            uint8_t **image,
                            mlt_image_format *format,
                            int *width,
                            int *height,
                            int writable)
{
    mlt_filter filter = mlt_frame_pop_service(frame);
    mlt_properties properties = MLT_FILTER_PROPERTIES(filter);
    private_data *pdata = (private_data *) filter->child;

    // The full colour picture: asking for it here makes the producer convert from what it decoded straight to RGBA,
    // so that views that are interleaved at the pixel are not mixed by a conversion to a subsampled format
    *format = mlt_image_rgba;
    int error = mlt_frame_get_image(frame, image, format, width, height, 0);
    packing_t packing = packing_from_name(mlt_properties_get(properties, "packing"));
    if (error || !*image || *format != mlt_image_rgba || packing == packing_none)
        return error;

    const int invert = mlt_properties_get_int(properties, "invert");
    const char *output = mlt_properties_get(properties, "output");
    const int sbs = output && !strcmp(output, "side_by_side");
    const int want_right = output && !strcmp(output, "right");
    const int sw = *width;
    const int sh = *height;
    int vw, vh;
    view_size(frame, packing, sw, sh, &vw, &vh);

    // The views, left and right
    uint8_t *eyes[2] = {NULL, NULL};
    int eye_width = vw;
    int eye_height = vh;
    if (packing == packing_frame_sequence) {
        // The frame has one view: which, the stream says when it can (the SEI), otherwise the frames come in turns from the first
        // view, in the frame's place in what it comes from. The other frame of the pair is the next one if this is the first view
        // and the previous one if it is the second; a player that is paused or has seeked has not fetched it, so it is fetched here,
        // from a producer of our own.
        mlt_properties frame_properties = MLT_FRAME_PROPERTIES(frame);
        const char *view = mlt_properties_get(frame_properties, "meta.media.stereo3d.view");
        // A player that has a project of many clips says which file, and where its first frame is, with resource and offset
        const char *source = mlt_properties_get(properties, "resource");
        const int offset = source && *source ? mlt_properties_get_int(properties, "offset") : 0;
        const mlt_position position = mlt_frame_original_position(frame) - offset;
        int is_right;
        if (view)
            is_right = !strcmp(view, "right");
        else
            is_right = (position & 1) != invert;
        const mlt_position other_position = is_right ? position - 1 : position + 1;
        pthread_mutex_lock(&pdata->mutex);
        set_view(&pdata->latest[is_right], *image, sw, sh);
        pdata->latest_position[is_right] = position;
        if (!pdata->latest[!is_right].data || pdata->latest_position[!is_right] != other_position) {
            mlt_producer original = mlt_frame_get_original_producer(frame);
            const char *resource = source && *source ? source
                : original ? mlt_properties_get(MLT_PRODUCER_PROPERTIES(original), "resource") : NULL;
            if (resource && other_position >= 0) {
                if (!pdata->partner || !pdata->partner_resource || strcmp(pdata->partner_resource, resource)) {
                    mlt_producer_close(pdata->partner);
                    free(pdata->partner_resource);
                    pdata->partner = mlt_factory_producer(mlt_service_profile(MLT_FILTER_SERVICE(filter)), "avformat", resource);
                    pdata->partner_resource = strdup(resource);
                }
                if (pdata->partner) {
                    mlt_frame other = NULL;
                    mlt_producer_seek(pdata->partner, other_position);
                    mlt_service_get_frame(MLT_PRODUCER_SERVICE(pdata->partner), &other, 0);
                    if (other) {
                        mlt_image_format other_format = mlt_image_rgba;
                        int other_width = sw;
                        int other_height = sh;
                        uint8_t *other_image = NULL;
                        if (!mlt_frame_get_image(other, &other_image, &other_format, &other_width, &other_height, 0)
                            && other_image && other_format == mlt_image_rgba) {
                            set_view(&pdata->latest[!is_right], other_image, other_width, other_height);
                            pdata->latest_position[!is_right] = other_position;
                        }
                        mlt_frame_close(other);
                    }
                }
            }
        }
        view_t *left = &pdata->latest[0];
        view_t *right = &pdata->latest[1];
        if (!left->data)
            left = right;
        if (!right->data)
            right = left;
        eye_width = left->width;
        eye_height = left->height;
        size_t eye_bytes = (size_t) eye_width * eye_height * 4;
        eyes[0] = mlt_pool_alloc(eye_bytes);
        eyes[1] = mlt_pool_alloc(eye_bytes);
        memcpy(eyes[0], left->data, eye_bytes);
        if (right->width == eye_width && right->height == eye_height)
            memcpy(eyes[1], right->data, eye_bytes);
        else
            scale_view(right->data, right->width, right->height, eyes[1], eye_width, eye_height);
        pthread_mutex_unlock(&pdata->mutex);
    } else {
        int stored_width = sw;
        int stored_height = sh;
        if (packing == packing_side_by_side || packing == packing_side_by_side_quincunx
            || packing == packing_columns || packing == packing_checkerboard)
            stored_width = sw / 2;
        if (packing == packing_top_and_bottom || packing == packing_lines)
            stored_height = sh / 2;
        uint8_t *stored = mlt_pool_alloc((size_t) stored_width * stored_height * 4);
        for (int slot = 0; slot < 2; slot++) {
            stored_view(*image, sw, sh, packing, slot, stored, stored_width, stored_height);
            // The first view is the left eye's unless the stream says the right eye's comes first
            uint8_t **eye = &eyes[slot ^ (invert ? 1 : 0)];
            *eye = mlt_pool_alloc((size_t) vw * vh * 4);
            if (stored_width == vw && stored_height == vh)
                memcpy(*eye, stored, (size_t) vw * vh * 4);
            else
                scale_view(stored, stored_width, stored_height, *eye, vw, vh);
        }
        mlt_pool_release(stored);
    }

    // What was asked for: both views side by side with the left one first (the full side by side), or one of them
    int out_width = sbs ? eye_width * 2 : eye_width;
    uint8_t *out = mlt_pool_alloc((size_t) out_width * eye_height * 4);
    if (sbs) {
        for (int y = 0; y < eye_height; y++) {
            memcpy(out + (size_t) y * out_width * 4, eyes[0] + (size_t) y * eye_width * 4, (size_t) eye_width * 4);
            memcpy(out + ((size_t) y * out_width + eye_width) * 4,
                   eyes[1] + (size_t) y * eye_width * 4,
                   (size_t) eye_width * 4);
        }
    } else {
        memcpy(out, eyes[want_right ? 1 : 0], (size_t) eye_width * eye_height * 4);
    }
    mlt_pool_release(eyes[0]);
    mlt_pool_release(eyes[1]);

    *image = out;
    *width = out_width;
    *height = eye_height;
    mlt_frame_set_image(frame, out, out_width * eye_height * 4, mlt_pool_release);
    mlt_properties frame_properties = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(frame_properties, "width", out_width);
    mlt_properties_set_int(frame_properties, "height", eye_height);
    return 0;
}

static mlt_frame filter_process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, filter_get_image);
    return frame;
}

static void filter_close(mlt_filter filter)
{
    private_data *pdata = (private_data *) filter->child;
    if (pdata) {
        mlt_pool_release(pdata->latest[0].data);
        mlt_pool_release(pdata->latest[1].data);
        mlt_producer_close(pdata->partner);
        free(pdata->partner_resource);
        pthread_mutex_destroy(&pdata->mutex);
        free(pdata);
    }
    filter->child = NULL;
    filter->close = NULL;
    filter->parent.close = NULL;
    mlt_service_close(&filter->parent);
}

mlt_filter filter_stereo3d_unpack_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    mlt_filter filter = mlt_filter_new();
    private_data *pdata = calloc(1, sizeof(private_data));
    if (filter && pdata) {
        mlt_properties properties = MLT_FILTER_PROPERTIES(filter);
        pthread_mutex_init(&pdata->mutex, NULL);
        mlt_properties_set_or_default(properties, "output", NULL, "side_by_side");
        if (arg)
            mlt_properties_set(properties, "packing", arg);
        filter->child = pdata;
        filter->close = filter_close;
        filter->process = filter_process;
    } else {
        free(pdata);
        if (filter) {
            mlt_filter_close(filter);
            filter = NULL;
        }
    }
    return filter;
}
