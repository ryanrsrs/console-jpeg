#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_search.h"
#include "frame_buffer.h"
#include "util.h"

static void destroy_dumb_buffer(int fd_drm, drm_handle_t handle)
{
    struct drm_mode_destroy_dumb arg = { .handle = handle };
    ioctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
}

// Used to fill BGR frame buffer with a solid RGB color.
static void memset_bgr24(uint8_t* buf, uint32_t color, size_t n) {
    uint8_t r = 255 & (color >> 16);
    uint8_t g = 255 & (color >> 8);
    uint8_t b = 255 & color;

    size_t i;
    for (i = 0; i < n; i++) {
        *buf++ = b;
        *buf++ = g;
        *buf++ = r;
    }
}

static void memset_rgb24(uint8_t* buf, uint32_t color, size_t n) {
    uint8_t r = 255 & (color >> 16);
    uint8_t g = 255 & (color >> 8);
    uint8_t b = 255 & color;

    size_t i;
    for (i = 0; i < n; i++) {
        *buf++ = r;
        *buf++ = g;
        *buf++ = b;
    }
}

static void memset_bgr32(uint8_t* buf, uint32_t color, size_t n) {
    uint8_t x = 255 & (color >> 24);
    uint8_t r = 255 & (color >> 16);
    uint8_t g = 255 & (color >> 8);
    uint8_t b = 255 & color;

    size_t i;
    for (i = 0; i < n; i++) {
        *buf++ = b;
        *buf++ = g;
        *buf++ = r;
        *buf++ = x;
    }
}

static void memset_rgb32(uint8_t* buf, uint32_t color, size_t n) {
    uint8_t x = 255 & (color >> 24);
    uint8_t r = 255 & (color >> 16);
    uint8_t g = 255 & (color >> 8);
    uint8_t b = 255 & color;

    size_t i;
    for (i = 0; i < n; i++) {
        *buf++ = r;
        *buf++ = g;
        *buf++ = b;
        *buf++ = x;
    }
}

struct Frame_Buffer* frame_buffer_create(int fd_drm, uint32_t width,
    uint32_t height, uint32_t pixel_format)
{
    const struct Pixel_Format* pf = lookup_pixel_format(pixel_format);

    struct drm_mode_create_dumb arg = {
        .height = height,
        .width = width,
    };
    arg.bpp = pf->bytes_per_pixel * 8;

    struct Frame_Buffer* fb = malloc(sizeof(struct Frame_Buffer));
    if (fb == 0) {
        fprintf(File_Error, "Error: Out of memory at line %i.\n", __LINE__);
        return 0;
    }

    int err = ioctl(fd_drm, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (err) {
        fprintf(File_Error, "Error: ioctl(DRM_IOCTL_MODE_CREATE_DUMB): %s\n",
                strerror(errno));
        free(fb);
        return 0;
    }

    uint32_t handles[4] = { arg.handle };
    uint32_t pitches[4] = { arg.pitch };
    uint32_t offsets[4] = { 0 };
    uint32_t fb_id;

    err = drmModeAddFB2(fd_drm, width, height, pixel_format,
            handles, pitches, offsets, &fb_id, 0);
    if (err) {
        fprintf(File_Error, "Error: drmModeAddFB2(): %s\n",
                strerror(errno));
        destroy_dumb_buffer(fd_drm, arg.handle);
        free(fb);
        return 0;
    }

    fb->fd_drm = fd_drm;
    fb->width = width;
    fb->height = height;
    fb->stride = arg.pitch;
    fb->size = arg.size;
    fb->pixel_format = pixel_format;
    fb->bytes_per_pixel = pf->bytes_per_pixel;
    fb->red_first = pf->red_first;
    if (pf->bytes_per_pixel == 3) {
        fb->pixel_set = pf->red_first ? memset_rgb24 : memset_bgr24;
    }
    else {
        fb->pixel_set = pf->red_first ? memset_rgb32 : memset_bgr32;
    }
    fb->handle = arg.handle;
    fb->fb_id = fb_id;
    fb->fd_dma = -1;
    fb->pixels = 0;

    return fb;
}

void frame_buffer_destroy(struct Frame_Buffer* fb)
{
    if (fb->pixels) {
        frame_buffer_unmap(fb);
    }
    drmModeRmFB(fb->fd_drm, fb->fb_id);
    destroy_dumb_buffer(fb->fd_drm, fb->handle);
    free(fb);
}

int frame_buffer_map(struct Frame_Buffer* fb)
{
    int err = drmPrimeHandleToFD(fb->fd_drm, fb->handle,
            DRM_CLOEXEC | DRM_RDWR, &fb->fd_dma);
    if (err || fb->fd_dma < 0) {
        fprintf(File_Error, "Error: drmPrimeHandleToFD(): %s\n",
                strerror(errno));
        fb->fd_dma = -1;
        return -1;
    }

    void* ptr = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fb->fd_dma, 0);
    if (ptr == 0 || ptr == MAP_FAILED) {
        fprintf(File_Error, "Error: mmap(fd_dma): %s\n",
                strerror(errno));
        close(fb->fd_dma);
        fb->fd_dma = -1;
        return -1;
    }
    fb->pixels = ptr;

    return 0;
}

void frame_buffer_unmap(struct Frame_Buffer* fb)
{
    munmap(fb->pixels, fb->size);
    close(fb->fd_dma);
    fb->pixels = 0;
    fb->fd_dma = -1;
}

uint8_t* get_pixels(struct Frame_Buffer* fb, int x, int y)
{
    return fb->pixels + y * fb->stride + x * fb->bytes_per_pixel;
}

void fill_pixels(struct Frame_Buffer* fb, uint32_t color, int x, int y, int n)
{
    uint8_t* pixels = get_pixels(fb, x, y);
    fb->pixel_set(pixels, color, n);
}

void draw_borders(struct Frame_Buffer* fb, uint32_t color,  int left, int right,
    int top, int bottom)
{
    int y;
    for (y = 0; y < top; y++) {
        fill_pixels(fb, color, 0, y, fb->width);
    }

    if (left || right) {
        int n = fb->height - bottom;
        for (; y < n; y++) {
            if (left) fill_pixels(fb, color, 0, y, left);
            if (right) fill_pixels(fb, color, fb->width - right, y, right);
        }
    }

    for (y = fb->height - bottom; y < fb->height; y++) {
        fill_pixels(fb, color, 0, y, fb->width);
    }
}

void fill_rect(struct Frame_Buffer* fb, uint32_t color, int left, int top,
    int width, int height)
{
    if (left < 0 || left >= fb->width)  return;
    if (top  < 0 || top  >= fb->height) return;

    if (width  < 0 || left + width  > fb->width)  width  = fb->width  - left;
    if (height < 0 || top  + height > fb->height) height = fb->height - top;

    int y;
    for (y = top; y < top + height; y++) {
        fill_pixels(fb, color, left, y, width);
    }
}

// Allocate extra pixels to two borders.
void split_border(int extra, int* left, int* right)
{
    int half = extra >> 1;
    *left = half;
    *right = extra - half;
}

void swizzle_copy(bool swizzle, uint32_t bytes_per_pixel,
    uint8_t* src, uint32_t src_w, uint32_t src_h, uint32_t src_stride,
    uint8_t* dst, uint32_t dst_stride)
{
    if (!swizzle) {
        int bytes = src_w * bytes_per_pixel;
        int y;
        for (y = 0; y < src_h; y++) {
            memcpy(dst, src, bytes);
            src += src_stride;
            dst += dst_stride;
        }
    }
    else if (bytes_per_pixel == 3) {
        int x, y;
        for (y = 0; y < src_h; y++) {
            for (x = 0; x < src_w; x++) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                src += 3;
                dst += 3;
            }
            src += src_stride - 3 * src_w;
            dst += dst_stride - 3 * src_w;
        }
    }
    else if (bytes_per_pixel == 4) {
        int x, y;
        for (y = 0; y < src_h; y++) {
            for (x = 0; x < src_w; x++) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
                src += 4;
                dst += 4;
            }
            src += src_stride - 4 * src_w;
            dst += dst_stride - 4 * src_w;
        }
    }
}
