#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>

#include "stb_image_resize2.h"

#include "drm_search.h"
#include "frame_buffer.h"
#include "util.h"
#include "read_heif.h"

#ifdef NO_HEIF_SUPPORT
int read_heif(const char* filename, struct Frame_Buffer* fb)
{
    fprintf(File_Error, "Error: console-jpeg was built without HEIF support.\n");
    return -1;
}
#else

#include <libheif/heif.h>

int read_heif(const char* filename, struct Frame_Buffer* fb)
{
    double t0 = 0;
    double t1 = 0;
    double t2 = 0;

    if (Verbose) {
        t0 = time_f();
        fprintf(File_Info, "\nHEIF %s\n", filename);
    }

    static bool already = false;
    if (!already) {
        heif_init(0);
        already = true;

        if (Verbose) {
            t1 = time_f();
            fprintf(File_Info, "  init   %6.3f sec\n", t1 - t0);
            t0 = t1;
        }
    }

    enum heif_chroma dec_fmt;
    stbir_pixel_layout rsz_fmt_in;
    stbir_pixel_layout rsz_fmt_out;

    switch (fb->pixel_format) {
        case DRM_FORMAT_BGR888:
                dec_fmt = heif_chroma_interleaved_RGB;
                rsz_fmt_in = STBIR_RGB;
                rsz_fmt_out = STBIR_RGB;
                break;

        case DRM_FORMAT_RGB888:
                dec_fmt = heif_chroma_interleaved_RGB;
                rsz_fmt_in = STBIR_RGB;
                rsz_fmt_out = STBIR_BGR;
                break;

        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
                dec_fmt = heif_chroma_interleaved_RGBA;
                rsz_fmt_in = STBIR_4CHANNEL;
                rsz_fmt_out = STBIR_4CHANNEL;
                break;

        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
                dec_fmt = heif_chroma_interleaved_RGBA;
                rsz_fmt_in = STBIR_RGBA_PM;
                rsz_fmt_out = STBIR_BGRA_PM;
                break;

        default:
                fprintf(File_Error, "Error: Unknown pixel format '%s'\n",
                    four_cc_to_str(fb->pixel_format));
                return -1;
    }

    struct heif_context* ctx = heif_context_alloc();
    struct heif_image_handle* handle = 0;
    struct heif_image* img = 0;
    struct heif_error err = { heif_error_Ok };
    int ret = -1;

    int img_w = 0;
    int img_h = 0;
    int img_stride = 0;

    int dst_w = fb->width;
    int dst_h = fb->height;

    int border_left = 0;
    int border_right = 0;
    int border_top = 0;
    int border_bottom = 0;

    const uint8_t* img_pixels = 0;

    err = heif_context_read_from_file(ctx, filename, 0);
    if (err.code != heif_error_Ok) goto HeifError;

    // get a handle to the primary image
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) goto HeifError;

    // decode the image and convert colorspace to RGB
    err = heif_decode_image(handle, &img, heif_colorspace_RGB, dec_fmt, 0);
    if (err.code != heif_error_Ok) goto HeifError;

    img_w = heif_image_get_width(img, heif_channel_interleaved);
    img_h = heif_image_get_height(img, heif_channel_interleaved);
    if (img_w < 0 || img_h < 0) {
        fprintf(File_Error, "Error: heif_image_get_width(): interleaved channel not present.\n");
        goto Cleanup;
    }

    img_pixels = heif_image_get_plane_readonly(img, heif_channel_interleaved, &img_stride);
    if (img_pixels == 0) {
        fprintf(File_Error, "Error: heif_image_get_plane_readonly(): interleaved channel not present.\n");
        goto Cleanup;
    }

    // always resize, since heifs are pretty much never exactly screen size
    int resize_width;
    int resize_height;

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

    if (Verbose) {
        fprintf(File_Info, "  source %5i x %5i\n", img_w, img_h);
        fprintf(File_Info, "  resize %5i x %5i\n", resize_width, resize_height);
        fprintf(File_Info, "  dest   %5i x %5i\n", fb->width, fb->height);
        fprintf(File_Info, "  border  %i %i %i %i\n", border_left, border_right,
                                                  border_top, border_bottom);
        t1 = time_f();
    }


    uint8_t* pixels = get_pixels(fb, border_left, border_top);
    STBIR_RESIZE rsz;
    stbir_resize_init(&rsz, img_pixels, img_w, img_h, img_stride,
        pixels, resize_width, resize_height, fb->stride,
        rsz_fmt_in, STBIR_TYPE_UINT8);

    // swap channels
    stbir_set_pixel_layouts(&rsz, rsz_fmt_in, rsz_fmt_out);

    int ok = stbir_resize_extended(&rsz);
    if (ok == 0) {
        fprintf(File_Error, "Error: stbir_resize_extended() failed.\n");
        goto Cleanup;
    }

    draw_borders(fb, BG_Color, border_left, border_right, border_top, border_bottom);

    if (Verbose) {
        t2 = time_f();
        fprintf(File_Info, "  heif   %6.3f sec\n", t1 - t0);
        fprintf(File_Info, "  resize %6.3f sec\n", t2 - t1);
    }

    ret = 0;

HeifError:
    if (err.code != heif_error_Ok) {
        fprintf(File_Error, "Error: libheif: %s\n", err.message);
    }

Cleanup:
    if (img) heif_image_release(img);
    if (handle) heif_image_handle_release(handle);
    heif_context_free(ctx);

    if (Verbose) fprintf(File_Info, "  total  %6.3f sec\n", time_f() - t0);

    return ret;
}

#endif // NO_HEIF_SUPPORT else
