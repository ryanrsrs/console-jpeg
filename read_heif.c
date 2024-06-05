#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libheif/heif.h>

#include "stb_image_resize2.h"

#include "frame_buffer.h"
#include "util.h"
#include "read_heif.h"

int read_heif(const char* filename, struct Frame_Buffer* fb)
{
    static bool already = false;
    if (!already) {
        heif_init(0);
        already = true;
    }

    double t0 = time_f();
    bool trace_resize = true;
    if (trace_resize) {
        fprintf(stderr, "\nHEIF %s\n", filename);
    }

    struct heif_context* ctx = heif_context_alloc();
    heif_context_read_from_file(ctx, filename, 0);

    // get a handle to the primary image
    struct heif_image_handle* handle;
    heif_context_get_primary_image_handle(ctx, &handle);

    //int img_w = heif_image_handle_get_width(handle);
    //int img_h = heif_image_handle_get_height(handle);

    // decode the image and convert colorspace to RGB
    struct heif_image* img;
    heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, 0);

    int img_w = heif_image_get_width(img, heif_channel_interleaved);
    int img_h = heif_image_get_height(img, heif_channel_interleaved);
    int img_stride;
    const uint8_t* img_pixels = heif_image_get_plane_readonly(img,
        heif_channel_interleaved, &img_stride);
    fprintf(stderr, "  img %i x %i (%i)\n", img_w, img_h, img_stride);

    int dst_w = fb->width;
    int dst_h = fb->height;

    int resize_width;
    int resize_height;

    int border_left = 0;
    int border_right = 0;
    int border_top = 0;
    int border_bottom = 0;

    // set borders to preserve aspect ratio
    if (img_w * dst_h > img_h * dst_w) {
        // borders on top / bottom
        resize_width = dst_w;
        resize_height = img_h * dst_w / img_w;
        split_border(abs(resize_height - dst_h),
            &border_top, &border_bottom);
    }
    else {
        // borders on left / right
        resize_width = img_w * dst_h / img_h;
        resize_height = dst_h;
        split_border(abs(resize_width - dst_w),
            &border_left, &border_right);
    }

    draw_borders(fb, BG_Color, border_left, border_right, border_top, border_bottom);

    double t1 = time_f();

    uint8_t* pixels = pixel_offset(fb, border_left, border_top);
    STBIR_RESIZE rsz;
    stbir_resize_init(&rsz, img_pixels, img_w, img_h, img_stride,
        pixels, resize_width, resize_height, fb->stride,
        STBIR_RGB, STBIR_TYPE_UINT8);

    // swap channels
    stbir_set_pixel_layouts(&rsz, STBIR_RGB, STBIR_BGR);

    int ok = stbir_resize_extended(&rsz);
    if (ok == 0) {
        fprintf(stderr, "stbir_resize_extended() error\n");
    }

    double t2 = time_f();

    // clean up resources
    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);

    fprintf(stderr, "  heif:  %6.3f\n", t1 - t0);
    fprintf(stderr, "  rsz:   %6.3f\n", t2 - t1);
    fprintf(stderr, "  total: %6.3f\n", time_f() - t0);

    return 0;
}
