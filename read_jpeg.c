#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <turbojpeg.h>

#include "stb_image_resize2.h"

#include "drm_search.h"
#include "frame_buffer.h"
#include "util.h"
#include "read_jpeg.h"

// Memory-mapped jpeg file.
struct Mapped_Jpeg {
    unsigned char* data;
    size_t length;
};

static struct Mapped_Jpeg* jpeg_create(const char* filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(File_Error, "Error: open(): %s\n", strerror(errno));
        return 0;
    }

    struct stat st;
    int err = fstat(fd, &st);
    if (err < 0) {
        fprintf(File_Error, "Error: fstat(): %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    // very arbitrary limit
    const int max_mb = 500;
    if ((st.st_size >> 20) > max_mb) {
        fprintf(File_Error, "Error: Input jpeg larger than %i MB.\n", max_mb);
        close(fd);
        return 0;
    }

    void* addr = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        fprintf(File_Error, "Error: mmap(): %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    close(fd);

    struct Mapped_Jpeg* jpeg = malloc(sizeof(struct Mapped_Jpeg));
    if (jpeg == 0) {
        free(jpeg);
        munmap(addr, st.st_size);
        fprintf(File_Error, "Error: Out of memory at line %i.\n", __LINE__);
        return 0;
    }
    jpeg->data = (unsigned char*)addr;
    jpeg->length = st.st_size;
    return jpeg;
}

static void jpeg_destroy(struct Mapped_Jpeg* jpeg)
{
    munmap(jpeg->data, jpeg->length);
    free(jpeg);
}

// How to get an arbitrary size jpeg onto a fixed size screen.
// We have 2 scaling methods:
//  1) jpeg scaled decode: 1x, 1/2, 1/4, 1/8
//  2) stbir_resize() general-purpose image resizer
// If possible, we try to decode the jpeg directly to the framebuffer and avoid
// doing an unnecessary resize.
struct resize_strategy {
    // jpeg native size
    int src_width;
    int src_height;

    // screen / framebuffer size
    int dst_width;
    int dst_height;

    // jpeg decode size
    // src scaled by 1x, 1/2, 1/4, 1/8
    int decode_width;
    int decode_height;

    // use stbir_resize to scale decode size to screen size
    // will be 0 if decode size matches a screen dimension (resize not needed)
    int resize_width;
    int resize_height;

    // borders because jpeg aspect ratio may differ from screen
    int border_left;
    int border_right;
    int border_top;
    int border_bottom;
};

static void make_resize_strategy(struct resize_strategy* strat,
    int src_w, int src_h, int dst_w, int dst_h)
{
    strat->src_width = src_w;
    strat->src_height = src_h;

    strat->dst_width = dst_w;
    strat->dst_height = dst_h;

    strat->decode_width = src_w;
    strat->decode_height = src_h;

    strat->resize_width = 0;
    strat->resize_height = 0;

    strat->border_top = 0;
    strat->border_bottom = 0;
    strat->border_left = 0;
    strat->border_right = 0;

    if (src_w == dst_w && src_h == dst_h) {
        // best case, jpeg matches screen exactly
        return;
    }

    int scale_w;
    int scale_h;

    tjscalingfactor sf = { 1 };
    for (sf.denom = 1; sf.denom <= 8; sf.denom <<= 1) {
        // check if we can do a scaled decode directly to framebuffer
        scale_w = TJSCALED(src_w, sf);
        scale_h = TJSCALED(src_h, sf);
        if (scale_w == dst_w && scale_h < dst_h) {
            // direct decode to framebuffer, borders on top/bottom
            strat->decode_width = scale_w;
            strat->decode_height = scale_h;
            split_border(dst_h - scale_h,
                &strat->border_top, &strat->border_bottom);
            return;
        }
        if (scale_w < dst_w && scale_h == dst_h) {
            // direct decode to framebuffer, borders on left/right
            strat->decode_width = scale_w;
            strat->decode_height = scale_h;
            split_border(dst_w - scale_w,
                &strat->border_left, &strat->border_right);
            return;
        }
    }

    // no exact decode, pick smallest decode size that is larger than screen
    for (sf.denom = 8; sf.denom >= 1; sf.denom >>= 1) {
        scale_w = TJSCALED(src_w, sf);
        scale_h = TJSCALED(src_h, sf);
        if (scale_w > dst_w || scale_h > dst_h) {
            // good intermediate size
            break;
        }
    }

    // temp image will be this size
    strat->decode_width = scale_w;
    strat->decode_height = scale_h;

    // set borders to preserve aspect ratio
    if (scale_w * dst_h > scale_h * dst_w) {
        // borders on top / bottom
        strat->resize_width = dst_w;
        strat->resize_height = scale_h * dst_w / scale_w;
        split_border(abs(strat->resize_height - dst_h),
            &strat->border_top, &strat->border_bottom);
    }
    else {
        // borders on left / right
        strat->resize_width = scale_w * dst_h / scale_h;
        strat->resize_height = dst_h;
        split_border(abs(strat->resize_width - dst_w),
            &strat->border_left, &strat->border_right);
    }
}

int read_jpeg(const char* filename, struct Frame_Buffer* fb)
{
    double t2, t1, t0 = time_f();

    struct Mapped_Jpeg* jpeg = 0;
    tjhandle inst = 0;

    int img_w;
    int img_h;

    struct resize_strategy strat;

    uint8_t* temp_pixels = 0;

    int err = 0;
    int ret = -1;

    if (Verbose) fprintf(File_Info, "\nJPEG %s\n", filename);

    enum TJPF dec_fmt;
    stbir_pixel_layout rsz_fmt_in;
    stbir_pixel_layout rsz_fmt_out;

    switch (fb->pixel_format) {
        case DRM_FORMAT_BGR888:
                dec_fmt = TJPF_RGB;
                rsz_fmt_in = STBIR_RGB;
                rsz_fmt_out = STBIR_RGB;
                break;

        case DRM_FORMAT_RGB888:
                dec_fmt = TJPF_BGR;
                rsz_fmt_in = STBIR_BGR;
                rsz_fmt_out = STBIR_BGR;
                break;

        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
                dec_fmt = TJPF_RGBX;
                rsz_fmt_in = STBIR_4CHANNEL;
                rsz_fmt_out = STBIR_4CHANNEL;
                break;

        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
                dec_fmt = TJPF_BGRX;
                rsz_fmt_in = STBIR_4CHANNEL;
                rsz_fmt_out = STBIR_4CHANNEL;
                break;

        default:
                fprintf(File_Error, "Error: Unknown pixel format '%s'\n",
                    four_cc_to_str(fb->pixel_format));
                goto Cleanup;
    }

    jpeg = jpeg_create(filename);
    if (jpeg == 0) goto Cleanup;

    inst = tjInitDecompress();
    if (inst == 0) {
        fprintf(File_Error, "Error: tjInitDecompress(): %s\n",
                tjGetErrorStr2(0));
        goto Cleanup;
    }

    {
        // Read jpeg header
        int subsamp, color;
        err = tjDecompressHeader3(inst, jpeg->data, jpeg->length,
            &img_w, &img_h, &subsamp, &color);
        if (err < 0) {
            fprintf(File_Error, "Error: tjDecompressHeader3(): %s\n",
                    tjGetErrorStr2(inst));
            goto Cleanup;
        }
    }

    make_resize_strategy(&strat, img_w, img_h, fb->width, fb->height);

    if (Verbose) {
        fprintf(File_Info, "  source %5i x %5i\n", strat.src_width,    strat.src_height);
        fprintf(File_Info, "  decode %5i x %5i\n", strat.decode_width, strat.decode_height);
        fprintf(File_Info, "  resize %5i x %5i\n", strat.resize_width, strat.resize_height);
        fprintf(File_Info, "  dest   %5i x %5i\n", strat.dst_width,    strat.dst_height);
        fprintf(File_Info, "  border  %i %i %i %i\n", strat.border_left, strat.border_right,
             strat.border_top, strat.border_bottom);
    }

    if (strat.resize_width == 0) {
        // resize not required
        uint8_t* pixels = get_pixels(fb, strat.border_left, strat.border_top);
        err = tjDecompress2(inst, jpeg->data, jpeg->length,
            pixels, strat.decode_width, fb->stride, strat.decode_height,
            dec_fmt, 0);
        if (err < 0) {
            fprintf(File_Error, "Error: tjDecompress2(): %s\n",
                    tjGetErrorStr2(inst));
            goto Cleanup;
        }

        t1 = time_f();

        if (Verbose) fprintf(File_Info, "  jpeg    %5.3f sec\n", t1 - t0);
    }
    else {
        // resize and temp buffer required
        size_t temp_size = strat.decode_width * strat.decode_height * fb->bytes_per_pixel;
        temp_pixels = malloc(temp_size);
        if (temp_pixels == 0) {
            fprintf(File_Error, "Error: malloc(%i MB) failed.\n",
                    (int)(temp_size >> 20));
            err = -1;
            goto Cleanup;
        }

        int decode_stride = strat.decode_width * fb->bytes_per_pixel;
        err = tjDecompress2(inst, jpeg->data, jpeg->length,
                temp_pixels, strat.decode_width, decode_stride, strat.decode_height,
                dec_fmt, 0);
        if (err < 0) {
            fprintf(File_Error, "Error: tjDecompress2(): %s\n",
                    tjGetErrorStr2(inst));
            goto Cleanup;
        }

        t1 = time_f();

        uint8_t* pixels = get_pixels(fb, strat.border_left, strat.border_top);
        STBIR_RESIZE rsz;
        stbir_resize_init(&rsz, temp_pixels, strat.decode_width, strat.decode_height,
            decode_stride, pixels, strat.resize_width, strat.resize_height,
            fb->stride, rsz_fmt_in, STBIR_TYPE_UINT8);

        stbir_set_pixel_layouts(&rsz, rsz_fmt_in, rsz_fmt_out);

        int ok = stbir_resize_extended(&rsz);
        if (ok == 0) {
            fprintf(File_Error, "Error: stbir_resize_extended() failed.\n");
            goto Cleanup;
        }

        get_pixels(fb, strat.border_left, strat.border_top)[0] = 255;

        t2 = time_f();

        if (Verbose) {
            fprintf(File_Info, "  jpeg    %5.3f sec\n", t1 - t0);
            fprintf(File_Info, "  resize  %5.3f sec\n", t2 - t1);
        }
    }

    draw_borders(fb, BG_Color, strat.border_left, strat.border_right,
        strat.border_top, strat.border_bottom);

    ret = 0;

Cleanup:
    if (temp_pixels) free(temp_pixels);
    if (inst) tjDestroy(inst);
    if (jpeg) jpeg_destroy(jpeg);

    if (Verbose) fprintf(File_Info, "  total   %5.3f sec\n", time_f() - t0);

    return ret;
}
