#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <stdint.h>

#include <drm.h>

struct Frame_Buffer {
    int fd_drm;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;

    // dumb buffer
    drm_handle_t handle;

    // frame buffer
    uint32_t fb_id;

    // mmap
    int fd_dma;
    uint8_t* pixels;
};

struct Frame_Buffer* frame_buffer_create(int fd_drm,
                    uint32_t width, uint32_t height);

void frame_buffer_destroy(struct Frame_Buffer* fb);

int frame_buffer_map(struct Frame_Buffer* fb);
void frame_buffer_unmap(struct Frame_Buffer* fb);


uint8_t* pixel_offset(struct Frame_Buffer* fb, int x, int y);

// Used to fill BGR frame buffer with a solid RGB color.
void memset24(uint8_t* buf, uint32_t color, size_t n);

void draw_borders(struct Frame_Buffer* fb, uint32_t color,  int left, int right,
    int top, int bottom);

void fill_rect(struct Frame_Buffer* fb, uint32_t color, int left, int top,
    int width, int height);


// Allocate extra pixels to two borders.
void split_border(int extra, int* left, int* right);

#endif
