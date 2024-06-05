#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <turbojpeg.h>

#include "stb_image_resize2.h"

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
        perror("open(jpeg)");
        return 0;
    }

    struct stat st;
    int err = fstat(fd, &st);
    if (err < 0) {
        perror("fstat(jpeg)");
        close(fd);
        return 0;
    }

    // very arbitrary limit
    if ((st.st_size >> 20) > 500) {
        fprintf(stderr, "Input jpeg larger than 500 MB.\n");
        close(fd);
        return 0;
    }

    void* addr = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap(jpeg)");
        close(fd);
        return 0;
    }

    close(fd);

    struct Mapped_Jpeg* jpeg = malloc(sizeof(struct Mapped_Jpeg));
    if (jpeg == 0) {
        free(jpeg);
        munmap(addr, st.st_size);
        fprintf(stderr, "Out of memory at line %i.\n", __LINE__);
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
    struct Mapped_Jpeg* jpeg = jpeg_create(filename);
    if (jpeg == 0) {
        return -1; // bad filename or corrupt image
    }

    tjhandle inst = tjInitDecompress();
    if (inst == 0) {
        fprintf(stderr, "jpeg init: %s\n", tjGetErrorStr2(0));
        jpeg_destroy(jpeg);
        return -1;
    }

    void* temp = 0;

    bool trace_resize = true;
    double t0, t1, t2;
    struct resize_strategy strat;
    uint8_t* pixels;

    t0 = time_f();
    if (trace_resize) {
        fprintf(stderr, "\nJpeg %s\n", filename);
    }

    // Read jpeg header
    int jpeg_width, jpeg_height, subsamp, color;
    int err = tjDecompressHeader3(inst, jpeg->data, jpeg->length,
        &jpeg_width, &jpeg_height, &subsamp, &color);
    if (err < 0) {
        fprintf(stderr, "jpeg header: %s\n", tjGetErrorStr2(inst));
        goto Cleanup;
    }

    make_resize_strategy(&strat, jpeg_width, jpeg_height,
        fb->width, fb->height);

    if (trace_resize) {
        fprintf(stderr, "  src %5i x %5i\n", strat.src_width, strat.src_height);
        fprintf(stderr, "  dec %5i x %5i\n", strat.decode_width, strat.decode_height);
        fprintf(stderr, "  rsz %5i x %5i\n", strat.resize_width, strat.resize_height);
        fprintf(stderr, "  dst %5i x %5i\n", strat.dst_width, strat.dst_height);
        fprintf(stderr, "  border %i %i %i %i\n", strat.border_left, strat.border_right,
             strat.border_top, strat.border_bottom);
    }

    draw_borders(fb, BG_Color, strat.border_left, strat.border_right,
        strat.border_top, strat.border_bottom);

    pixels = pixel_offset(fb, strat.border_left, strat.border_top);

    if (strat.resize_width) {
        size_t temp_size = strat.decode_width * strat.decode_height * 3;
        temp = malloc(temp_size);
        if (temp == 0) {
            fprintf(stderr, "Malloc(%i MB) failed during of 2-step resize.\n",
                (int)(temp_size >> 20));
            err = -1;
            goto Cleanup;
        }

        err = tjDecompress2(inst, jpeg->data, jpeg->length,
                temp, strat.decode_width, strat.decode_width * 3,
                strat.decode_height, TJPF_BGR, 0);
        if (err < 0) {
            fprintf(stderr, "jpeg decompress: %s\n", tjGetErrorStr2(inst));
            goto Cleanup;
        }

        t1 = time_f();

        STBIR_RESIZE rsz;
        stbir_resize_init(&rsz, temp, strat.decode_width, strat.decode_height,
            strat.decode_width * 3, pixels, strat.resize_width, strat.resize_height,
            fb->stride, STBIR_RGB, STBIR_TYPE_UINT8);
        stbir_resize_extended(&rsz);

        t2 = time_f();

        if (trace_resize) {
            fprintf(stderr, "  jpeg:   %5.3f\n", t1 - t0);
            fprintf(stderr, "  resize: %5.3f\n", t2 - t1);
        }
    }
    else {
        err = tjDecompress2(inst, jpeg->data, jpeg->length, (uint8_t*)pixels,
            strat.decode_width, fb->stride, strat.decode_height, TJPF_BGR, 0);
        if (err < 0) {
            fprintf(stderr, "jpeg decompress: %s\n", tjGetErrorStr2(inst));
            goto Cleanup;
        }

        t1 = time_f();

        if (trace_resize) {
            fprintf(stderr, "  jpeg:   %5.3f\n", t1 - t0);
        }
    }

    if (trace_resize) {
        double t_total = time_f() - t0;
        fprintf(stderr, "  total:  %5.3f\n", t_total);
    }

    err = 0;

Cleanup:
    if (temp) free(temp);
    tjDestroy(inst);
    jpeg_destroy(jpeg);

    return err;
}
