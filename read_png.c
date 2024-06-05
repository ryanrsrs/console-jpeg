#include <stdint.h>
#include <stdio.h>

#include <spng.h>

#include "stb_image_resize2.h"

#include "frame_buffer.h"
#include "util.h"
#include "read_png.h"

int read_png(const char* filename, struct Frame_Buffer* fb)
{
    double t0 = time_f();
    bool trace_resize = true;
    if (trace_resize) {
        fprintf(stderr, "\nPNG %s\n", filename);
    }

    FILE* png_file = fopen(filename, "rb");
    if (png_file == 0) {
        fprintf(stderr, "Can't open png file %s\n", filename);
        return -1;
    }

    spng_ctx* ctx = spng_ctx_new(0);
    if (ctx == 0) {
        fprintf(stderr, "Error: spng_ctx_new(0) failed.\n");
        fclose(png_file);
        return -1;
    }

    int err = spng_set_png_file(ctx, png_file);
    if (err) {
        fprintf(stderr, "Error: spng_set_png_file() failed.\n");
        spng_ctx_free(ctx);
        fclose(png_file);
        return -1;
    }

    struct spng_ihdr ihdr;
    err = spng_get_ihdr(ctx, &ihdr);
    if (err) {
        fprintf(stderr, "Error: spng_get_ihdr() failed.\n");
        spng_ctx_free(ctx);
        fclose(png_file);
        return -1;
    }

    /* Determine output image size */
    size_t temp_size;
    err = spng_decoded_image_size(ctx, SPNG_FMT_RGB8, &temp_size);
    if (err) {
        fprintf(stderr, "Error: spng_decoded_image_size() failed.\n");
        spng_ctx_free(ctx);
        fclose(png_file);
        return -1;
    }

    /* Decode to 8-bit RGBA */

    uint8_t* temp = malloc(temp_size);
    err = spng_decode_image(ctx, temp, temp_size, SPNG_FMT_RGB8, 0);
    if (err) {
        fprintf(stderr, "Error: spng_decoded_image_size() failed.\n");
        spng_ctx_free(ctx);
        free(temp);
        fclose(png_file);
        return -1;
    }

    int img_w = ihdr.width;
    int img_h = ihdr.height;
    fprintf(stderr, "  img %i x %i size=%i\n", img_w, img_h, temp_size);

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
    stbir_resize_init(&rsz, temp, img_w, img_h, img_w*3,
        pixels, resize_width, resize_height, fb->stride,
        STBIR_RGB, STBIR_TYPE_UINT8);

    // swap channels
    stbir_set_pixel_layouts(&rsz, STBIR_RGB, STBIR_BGR);

    int ok = stbir_resize_extended(&rsz);
    if (ok == 0) {
        fprintf(stderr, "stbir_resize_extended() error\n");
    }

    double t2 = time_f();

    /* Free context memory */
    spng_ctx_free(ctx);
    free(temp);
    fclose(png_file);

    fprintf(stderr, "  png: %6.3f\n", t1 - t0);
    fprintf(stderr, "  rsz: %6.3f\n", t2 - t1);

    return 0;
}
