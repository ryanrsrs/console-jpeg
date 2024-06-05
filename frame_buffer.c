#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "frame_buffer.h"

static void destroy_dumb_buffer(int fd_drm, drm_handle_t handle)
{
    struct drm_mode_destroy_dumb arg = { .handle = handle };
    ioctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
}

struct Frame_Buffer* frame_buffer_create(int fd_drm, uint32_t width,
                                        uint32_t height)
{
    struct drm_mode_create_dumb arg = {
        .height = height,
        .width = width,
        .bpp = 24
    };

    struct Frame_Buffer* fb = malloc(sizeof(struct Frame_Buffer));
    if (fb == 0) {
        fprintf(stderr, "Out of memory at line %i.\n", __LINE__);
        return 0;
    }

    int err = ioctl(fd_drm, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (err) {
        perror("ioctl(DRM_IOCTL_MODE_CREATE_DUMB)");
        free(fb);
        return 0;
    }

    uint32_t handles[4] = { arg.handle };
    uint32_t pitches[4] = { arg.pitch };
    uint32_t offsets[4] = { 0 };
    uint32_t fb_id;
    err = drmModeAddFB2(fd_drm, width, height, DRM_FORMAT_RGB888,
            handles, pitches, offsets, &fb_id, 0);
    if (err) {
        perror("drmModeAddFB2()");
        destroy_dumb_buffer(fd_drm, arg.handle);
        free(fb);
        return 0;
    }

    fb->fd_drm = fd_drm;
    fb->width = width;
    fb->height = height;
    fb->stride = arg.pitch;
    fb->size = arg.size;
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
        perror("drmPrimeHandleToFD()");
        fb->fd_dma = -1;
        return -1;
    }

    void* ptr = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fb->fd_dma, 0);
    if (ptr == 0 || ptr == MAP_FAILED) {
        perror("mmap(fd_dma)");
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

uint8_t* pixel_offset(struct Frame_Buffer* fb, int x, int y)
{
    return fb->pixels + fb->stride * y + 3 * x;
}

// Used to fill BGR frame buffer with a solid RGB color.
void memset24(uint8_t* buf, uint32_t color, size_t n) {
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

void draw_borders(struct Frame_Buffer* fb, uint32_t color,  int left, int right,
    int top, int bottom)
{
    uint8_t* pixels = fb->pixels;
    int i;
    for (i = 0; i < top; i++) {
        memset24(pixels, color, fb->width);
        pixels += fb->stride;
    }

    if (left || right) {
        int n = fb->height - bottom;
        for (; i < n; i++) {
            if (left) memset24(pixels, color, left);
            if (right) memset24(pixels + 3 * (fb->width - right), color, right);
            pixels += fb->stride;
        }
    }
    else {
        pixels += fb->stride * (fb->height - bottom - top);
    }

    for (i = 0; i < bottom; i++) {
        memset24(pixels, color, fb->width);
        pixels += fb->stride;
    }
}

void fill_rect(struct Frame_Buffer* fb, uint32_t color, int left, int top,
    int width, int height)
{
    if (left < 0 || left >= fb->width)  return;
    if (top  < 0 || top  >= fb->height) return;

    if (width  < 0 || left + width  > fb->width)  width  = fb->width  - left;
    if (height < 0 || top  + height > fb->height) height = fb->height - top;

    uint8_t* pixels = pixel_offset(fb, left, top);
    int i;
    for (i = 0; i < height; i++) {
        memset24(pixels, color, width);
        pixels += fb->stride;
    }
}

// Allocate extra pixels to two borders.
void split_border(int extra, int* left, int* right)
{
    int half = extra >> 1;
    *left = half;
    *right = extra - half;
}

