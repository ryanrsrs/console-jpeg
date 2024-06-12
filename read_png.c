#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>

#include <spng.h>

#include "stb_image_resize2.h"

#include "drm_search.h"
#include "frame_buffer.h"
#include "util.h"
#include "read_png.h"

int read_png(const char* filename, struct Frame_Buffer* fb)
{
    double t0 = 0;
    double t1 = 0;
    double t2 = 0;

    if (Verbose) {
        t0 = time_f();
        fprintf(File_Info, "\nPNG %s\n", filename);
    }

    FILE* png_file = 0;
    spng_ctx* ctx = 0;
    uint8_t* temp_pixels = 0;
    size_t temp_size;
    int ret = 0;
    int err = 0;

    int img_w = 0;
    int img_h = 0;

    int dst_w = fb->width;
    int dst_h = fb->height;

    int border_left = 0;
    int border_right = 0;
    int border_top = 0;
    int border_bottom = 0;

    struct spng_ihdr ihdr;

    enum spng_format dec_fmt;
    stbir_pixel_layout rsz_fmt_in;
    stbir_pixel_layout rsz_fmt_out;

    switch (fb->pixel_format) {
        case DRM_FORMAT_BGR888:
                dec_fmt = SPNG_FMT_RGB8;
                rsz_fmt_in = STBIR_RGB;
                rsz_fmt_out = STBIR_RGB;
                break;

        case DRM_FORMAT_RGB888:
                dec_fmt = SPNG_FMT_RGB8;
                rsz_fmt_in = STBIR_RGB;
                rsz_fmt_out = STBIR_BGR;
                break;

        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
                dec_fmt = SPNG_FMT_RGBA8;
                rsz_fmt_in = STBIR_4CHANNEL;
                rsz_fmt_out = STBIR_4CHANNEL;
                break;

        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
                dec_fmt = SPNG_FMT_RGBA8;
                rsz_fmt_in = STBIR_RGBA_PM;
                rsz_fmt_out = STBIR_BGRA_PM;
                break;

        default:
                fprintf(File_Error, "Error: Unknown pixel format '%s'\n",
                    four_cc_to_str(fb->pixel_format));
                goto Cleanup;
    }

    png_file = fopen(filename, "rb");
    if (png_file == 0) {
        fprintf(File_Error, "Error: Can't open png file %s\n", filename);
        ret = -1;
        goto Cleanup;
    }

    ctx = spng_ctx_new(0);
    if (ctx == 0) {
        fprintf(File_Error, "Error: spng_ctx_new(0) failed.\n");
        ret = -1;
        goto Cleanup;
    }

    err = spng_set_png_file(ctx, png_file);
    if (err) {
        fprintf(File_Error, "Error: spng_set_png_file() %s\n",
                spng_strerror(err));
        ret = -1;
        goto Cleanup;
    }

    err = spng_get_ihdr(ctx, &ihdr);
    if (err) {
        fprintf(File_Error, "Error: spng_get_ihdr() %s\n",
                spng_strerror(err));
        ret = -1;
        goto Cleanup;
    }
    img_w = ihdr.width;
    img_h = ihdr.height;

    /* Determine output image size */
    err = spng_decoded_image_size(ctx, dec_fmt, &temp_size);
    if (err) {
        fprintf(File_Error, "Error: spng_decoded_image_size() %s\n",
                spng_strerror(err));
        ret = -1;
        goto Cleanup;
    }

    if (Verbose) {
        fprintf(File_Info, "  source %5i x %5i\n", img_w, img_h);
    }

    if (img_w == dst_w && img_h <= dst_h && rsz_fmt_in == rsz_fmt_out) {
        // direct decode to framebuffer
        // widths must match since libspng doesn't do stride
        split_border(dst_h - img_h, &border_top, &border_bottom);

        uint8_t* pixels = get_pixels(fb, 0, border_top);
        err = spng_decode_image(ctx, pixels, temp_size, dec_fmt, 0);
        if (err) {
            fprintf(File_Error, "Error: spng_decoded_image_size() %s\n",
                    spng_strerror(err));
            ret = -1;
            goto Cleanup;
        }

        if (Verbose) {
            t1 = time_f();
            fprintf(File_Info, "  dest   %5i x %5i\n", fb->width, fb->height);
            fprintf(File_Info, "  border  %i %i %i %i\n",
                    border_left, border_right, border_top, border_bottom);
            fprintf(File_Info, "  png    %6.3f sec\n", t1 - t0);
        }
    }
    else if ((img_w <  dst_w && img_h == dst_h) ||
             (img_w == dst_w && img_h <= dst_h))
    {
        // need a temp buffer because different row sizes or format
        // but do not need to resample
        split_border(dst_w - img_w, &border_left, &border_right);
        split_border(dst_h - img_h, &border_top, &border_bottom);

        temp_pixels = malloc(temp_size);
        if (temp_pixels == 0) {
            fprintf(File_Error, "Error: malloc(%i MB) failed.\n",
                    (int)(temp_size >> 20));
        }

        err = spng_decode_image(ctx, temp_pixels, temp_size, dec_fmt, 0);
        if (err) {
            fprintf(File_Error, "Error: spng_decoded_image_size() %s\n",
                    spng_strerror(err));
            ret = -1;
            goto Cleanup;
        }

        if (Verbose) t1 = time_f();

        swizzle_copy(rsz_fmt_in != rsz_fmt_out, fb->bytes_per_pixel,
            temp_pixels, img_w, img_h, img_w * fb->bytes_per_pixel,
            get_pixels(fb, border_left, border_top), fb->stride);

        if (Verbose) {
            t2 = time_f();
            fprintf(File_Info, "  dest   %5i x %5i\n", fb->width, fb->height);
            fprintf(File_Info, "  border  %i %i %i %i\n",
                    border_left, border_right, border_top, border_bottom);
            fprintf(File_Info, "  png    %6.3f sec\n", t1 - t0);
            fprintf(File_Info, "  copy   %6.3f sec\n", t2 - t1);
        }
    }
    else {
        // do a full resize
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

        temp_pixels = malloc(temp_size);
        err = spng_decode_image(ctx, temp_pixels, temp_size, dec_fmt, 0);
        if (err) {
            fprintf(File_Error, "Error: spng_decoded_image_size() %s\n",
                    spng_strerror(err));
            ret = -1;
            goto Cleanup;
        }

        if (Verbose) t1 = time_f();

        uint8_t* pixels = get_pixels(fb, border_left, border_top);
        STBIR_RESIZE rsz;
        stbir_resize_init(&rsz, temp_pixels, img_w, img_h, 0,
            pixels, resize_width, resize_height, fb->stride,
            rsz_fmt_in, STBIR_TYPE_UINT8);

        // swap channels
        stbir_set_pixel_layouts(&rsz, rsz_fmt_in, rsz_fmt_out);

        int ok = stbir_resize_extended(&rsz);
        if (ok == 0) {
            fprintf(File_Error, "Error: stbir_resize_extended() failed.\n");
            ret = -1;
            goto Cleanup;
        }

        if (Verbose) {
            t2 = time_f();
            fprintf(File_Info, "  resize %5i x %5i\n", resize_width, resize_height);
            fprintf(File_Info, "  dest   %5i x %5i\n", fb->width, fb->height);
            fprintf(File_Info, "  border  %i %i %i %i\n",
                    border_left, border_right, border_top, border_bottom);
            fprintf(File_Info, "  png    %6.3f sec\n", t1 - t0);
            fprintf(File_Info, "  resize %6.3f sec\n", t2 - t1);
        }
    }

    draw_borders(fb, BG_Color, border_left, border_right, border_top, border_bottom);

    ret = 0;

Cleanup:
    if (temp_pixels) free(temp_pixels);
    if (ctx) spng_ctx_free(ctx);
    if (png_file) fclose(png_file);

    if (Verbose) fprintf(File_Info, "  total  %6.3f sec\n", time_f() - t0);

    return ret;
}

int write_png(const char* filename, struct Frame_Buffer* fb)
{
    double t0 = 0;
    double t1 = 0;
    double t2 = 0;

    if (Verbose) {
        t0 = time_f();
        fprintf(File_Info, "\nWrite PNG %s\n", filename);
    }

    FILE* png_file = 0;
    spng_ctx* ctx = 0;
    uint8_t* temp_pixels = 0;
    size_t temp_size;
    int ret = 0;
    int err = 0;

    const uint8_t* pixel_src;
    uint8_t* pixel_dst;

    int img_w = fb->width;
    int img_h = fb->height;

    struct spng_ihdr ihdr = { 0 };
    ihdr.width = img_w;
    ihdr.height = img_h;
    ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR;
    ihdr.bit_depth = 8;

    png_file = fopen(filename, "wb");
    if (png_file == 0) {
        fprintf(File_Error, "Error: Can't create png file %s\n", filename);
        ret = -1;
        goto Cleanup;
    }

    ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    if (ctx == 0) {
        fprintf(File_Error, "Error: spng_ctx_new(SPNG_CTX_ENCODER) failed.\n");
        ret = -1;
        goto Cleanup;
    }

    err = spng_set_png_file(ctx, png_file);
    if (err) {
        fprintf(File_Error, "Error: spng_set_png_file() %s\n",
                spng_strerror(err));
        ret = -1;
        goto Cleanup;
    }

    err = spng_set_ihdr(ctx, &ihdr);
    if (err) {
        fprintf(File_Error, "Error: spng_set_ihdr() %s\n",
                spng_strerror(err));
        ret = -1;
        goto Cleanup;
    }

    temp_size = img_w * 3 * img_h;
    temp_pixels = malloc(temp_size);

    pixel_src = get_pixels(fb, 0, 0);
    pixel_dst = temp_pixels;
    if (fb->pixel_format == DRM_FORMAT_BGR888) {
        int y, bytes = img_w * 3;
        for (y = 0; y < img_h; y++) {
            memcpy(pixel_dst, pixel_src, bytes);
            pixel_src += fb->stride;
            pixel_dst += bytes;
        }
    }
    else if (fb->pixel_format == DRM_FORMAT_RGB888) {
        int x, y;
        for (y = 0; y < img_h; y++) {
            for (x = 0; x < img_w; x++) {
                pixel_dst[0] = pixel_src[2];
                pixel_dst[1] = pixel_src[1];
                pixel_dst[2] = pixel_src[0];
                pixel_src += 3;
                pixel_dst += 3;
            }
            pixel_src += fb->stride - 3 * img_w;
        }
    }
    else if (fb->pixel_format == DRM_FORMAT_BGRA8888 ||
             fb->pixel_format == DRM_FORMAT_BGRX8888)
    {
        int x, y;
        for (y = 0; y < img_h; y++) {
            for (x = 0; x < img_w; x++) {
                pixel_dst[0] = pixel_src[0];
                pixel_dst[1] = pixel_src[1];
                pixel_dst[2] = pixel_src[2];
                pixel_src += 4;
                pixel_dst += 3;
            }
            pixel_src += fb->stride - 4 * img_w;
        }
    }
    else if (fb->pixel_format == DRM_FORMAT_RGBA8888 ||
             fb->pixel_format == DRM_FORMAT_RGBX8888)
    {
        int x, y;
        for (y = 0; y < img_h; y++) {
            for (x = 0; x < img_w; x++) {
                pixel_dst[0] = pixel_src[2];
                pixel_dst[1] = pixel_src[1];
                pixel_dst[2] = pixel_src[0];
                pixel_src += 4;
                pixel_dst += 3;
            }
            pixel_src += fb->stride - 4 * img_w;
        }
    }
    else {
        fprintf(File_Error, "Error: Unknown pixel format '%s'\n",
            four_cc_to_str(fb->pixel_format));
        goto Cleanup;
    }

    if (Verbose) t1 = time_f();

    ret = spng_encode_image(ctx, temp_pixels, temp_size, SPNG_FMT_PNG,
                SPNG_ENCODE_FINALIZE);
    if (err) {
        fprintf(File_Error, "Error: spng_encode_image() %s\n",
                spng_strerror(err));
        ret = -1;
        goto Cleanup;
    }

    if (Verbose) {
        t2 = time_f();

        long png_size = ftell(png_file);
        fprintf(File_Info, "  source %5i x %5i  '%s'\n", img_w, img_h,
                four_cc_to_str(fb->pixel_format));
        fprintf(File_Info, "  wrote %li bytes\n", png_size);
        fprintf(File_Info, "  setup  %6.3f sec\n", t1 - t0);
        fprintf(File_Info, "  encode %6.3f sec\n", t2 - t1);
    }

    ret = 0;

Cleanup:
    if (temp_pixels) free(temp_pixels);
    if (ctx) spng_ctx_free(ctx);
    if (png_file) fclose(png_file);

    if (Verbose) fprintf(File_Info, "  total  %6.3f sec\n", time_f() - t0);

    return ret;
}
