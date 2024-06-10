#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include <drm.h>

struct Frame_Buffer {
    int fd_drm;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;

    uint32_t pixel_format;
    uint32_t bytes_per_pixel;
    bool red_first;
    void (*pixel_set)(uint8_t*, uint32_t, size_t);


    // dumb buffer
    drm_handle_t handle;

    // frame buffer
    uint32_t fb_id;

    // mmap
    int fd_dma;
    uint8_t* pixels;
};

struct Frame_Buffer* frame_buffer_create(int fd_drm,
                    uint32_t width, uint32_t height, uint32_t pixel_format);

void frame_buffer_destroy(struct Frame_Buffer* fb);

int frame_buffer_map(struct Frame_Buffer* fb);
void frame_buffer_unmap(struct Frame_Buffer* fb);


uint8_t* get_pixels(struct Frame_Buffer* fb, int x, int y);

void fill_pixels(struct Frame_Buffer* fb, uint32_t color, int x, int y, int n);

void draw_borders(struct Frame_Buffer* fb, uint32_t color,  int left, int right,
    int top, int bottom);

void fill_rect(struct Frame_Buffer* fb, uint32_t color, int left, int top,
    int width, int height);

void swizzle_copy(bool swizzle, uint32_t bytes_per_pixel,
    uint8_t* src, uint32_t src_w, uint32_t src_h, uint32_t src_stride,
    uint8_t* dst, uint32_t dst_stride);

// Allocate extra pixels to two borders.
void split_border(int extra, int* left, int* right);

#endif
