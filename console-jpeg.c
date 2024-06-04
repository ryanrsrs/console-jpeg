#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <turbojpeg.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "stb_image_resize2.h"

volatile bool Quit = false;

void ctrl_c_handler(int signum)
{
    Quit = true;
}

// Return floating point seconds since first call
// First call always returns 0.0
double time_f()
{
    static struct timespec ts_zero = { 0, 0 };

    if (ts_zero.tv_sec == 0 && ts_zero.tv_nsec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts_zero);
        return 0.0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double f = (ts.tv_nsec - ts_zero.tv_nsec) * 1e-9;
    f += ts.tv_sec - ts_zero.tv_sec;
    return f;
}

void sleep_f(double secs)
{
    uint64_t ns = secs * 1e9;
    struct timespec rem;
    rem.tv_sec  = ns / 1000000000ULL;
    rem.tv_nsec = ns % 1000000000ULL;
    while (nanosleep(&rem, &rem)) {
        if (errno != EINTR) break;
        if (Quit) break; // caught ctrl-c
        // else ignore interrupted system call
    }
}

// Memory-mapped jpeg file.
struct Mapped_Jpeg {
    char* filename;
    unsigned char* data;
    size_t length;
};

struct Mapped_Jpeg* jpeg_create(const char* filename)
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
    char* name = strdup(filename);
    if (jpeg == 0 || name == 0) {
        free(jpeg);
        free(name);
        munmap(addr, st.st_size);
        fprintf(stderr, "Out of memory at line %i.\n", __LINE__);
        return 0;
    }
    jpeg->filename = name;
    jpeg->data = (unsigned char*)addr;
    jpeg->length = st.st_size;
    return jpeg;
}

void jpeg_destroy(struct Mapped_Jpeg* jpeg)
{
    munmap(jpeg->data, jpeg->length);
    free(jpeg->filename);
    free(jpeg);
}


struct Frame_Buffer {
    int fd_drm;

    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t size;

    // divided by 4
    uint32_t pitch32;
    uint32_t size32;

    // dumb buffer
    drm_handle_t handle;

    // frame buffer
    uint32_t fb_id;

    // mmap
    int fd_dma;
    void* pixels;
};

void destroy_dumb_buffer(int fd_drm, drm_handle_t handle)
{
    struct drm_mode_destroy_dumb arg = { .handle = handle };
    ioctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
}

struct Frame_Buffer* frame_buffer_create(int fd_drm, uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb arg = {
        .height = height,
        .width = width,
        .bpp = 32
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

    uint32_t fb_id;
    err = drmModeAddFB(fd_drm, width, height, 24, 32,
            arg.pitch, arg.handle, &fb_id);
    if (err) {
        perror("drmModeAddFB()");
        destroy_dumb_buffer(fd_drm, arg.handle);
        free(fb);
        return 0;
    }

    fb->fd_drm = fd_drm;
    fb->width = width;
    fb->height = height;
    fb->pitch = arg.pitch;
    fb->size = arg.size;
    fb->pitch32 = fb->pitch / sizeof(uint32_t);
    fb->size32 = fb->size / sizeof(uint32_t);
    fb->handle = arg.handle;
    fb->fb_id = fb_id;
    fb->fd_dma = -1;
    fb->pixels = 0;

    return fb;
}

void frame_buffer_unmap(struct Frame_Buffer* fb)
{
    munmap(fb->pixels, fb->size);
    close(fb->fd_dma);
    fb->pixels = 0;
    fb->fd_dma = -1;
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

// Two for double buffering.
struct Frame_Buffer* FB0 = 0;
struct Frame_Buffer* FB1 = 0;

void swap_frame_buffers()
{
    struct Frame_Buffer* x = FB0;
    FB0 = FB1;
    FB1 = x;
}

// Allocate and memory map the two global frame buffes
int create_two_frame_buffers(int fd_drm, uint32_t width, uint32_t height)
{
    FB0 = frame_buffer_create(fd_drm, width, height);
    if (FB0 == 0) {
        return -1;
    }

    if (frame_buffer_map(FB0)) {
        frame_buffer_destroy(FB0);
        FB0 = 0;
        return -1;
    }

    FB1 = frame_buffer_create(fd_drm, width, height);
    if (FB1 == 0) {
        frame_buffer_destroy(FB0);
        FB0 = 0;
        return -1;
    }

    if (frame_buffer_map(FB1)) {
        frame_buffer_destroy(FB0);
        frame_buffer_destroy(FB1);
        FB0 = 0;
        FB1 = 0;
        return -1;
    }
    return 0;
}

// Used to fill BGRX frame buffer with a solid RGB color.
void memset32(uint32_t* buf, uint32_t color, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        buf[i] = color;
    }
}

// Background color for border around jpegs.
uint32_t BG_Color = 0;

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

// Allocate extra pixels to two borders.
void split_border(int extra, int* left, int* right)
{
    int half = extra >> 1;
    *left = half;
    *right = extra - half;
}

void make_resize_strategy(struct resize_strategy* strat,
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

int jpeg_decode(struct Mapped_Jpeg* jpeg, struct Frame_Buffer* fb)
{
    double t0 = time_f();
    bool trace_resize = false;
    if (trace_resize) {
        fprintf(stderr, "\nJpeg %s\n", jpeg->filename);
    }

    tjhandle inst = tjInitDecompress();
    if (inst == 0) {
        fprintf(stderr, "jpeg init: %s\n", tjGetErrorStr2(0));
        return -1;
    }

    // Read jpeg header
    int jpeg_width, jpeg_height, subsamp, color;
    int err = tjDecompressHeader3(inst, jpeg->data, jpeg->length,
        &jpeg_width, &jpeg_height, &subsamp, &color);
    if (err < 0) {
        fprintf(stderr, "jpeg header: %s\n", tjGetErrorStr2(inst));
        tjDestroy(inst);
        return -1;
    }

    struct resize_strategy strat;
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

    uint32_t* pixels = (uint32_t*)fb->pixels;

    // top border + left border of first row
    int border = strat.border_top * fb->pitch32 + strat.border_left;
    memset32(pixels, BG_Color, border);
    pixels += border;

    int border_height = 0;

    if (strat.resize_width) {
        size_t temp_size = strat.decode_width * strat.decode_height * 4;
        void* temp = malloc(temp_size);
        if (temp == 0) {
            fprintf(stderr, "Malloc(%i MB) failed during of 2-step resize.\n",
                (int)(temp_size >> 20));
            tjDestroy(inst);
            return -1;
        }

        err = tjDecompress2(inst, jpeg->data, jpeg->length,
                temp, strat.decode_width, strat.decode_width * 4,
                strat.decode_height, TJPF_BGRX, 0);
        if (err < 0) {
            fprintf(stderr, "jpeg decompress: %s\n", tjGetErrorStr2(inst));
            free(temp);
            tjDestroy(inst);
            return -1;
        }

        double t1 = time_f();

        stbir_resize_uint8_linear(temp, strat.decode_width , strat.decode_height,
            strat.decode_width * 4, (uint8_t*)pixels, strat.resize_width,
            strat.resize_height, fb->pitch, STBIR_4CHANNEL);

        double t2 = time_f();

        if (trace_resize) {
            fprintf(stderr, "  jpeg:   %5.3f\n", t1 - t0);
            fprintf(stderr, "  resize: %5.3f\n", t2 - t1);
        }

        pixels += strat.resize_width;
        border_height = strat.resize_height;

        free(temp);
        tjDestroy(inst);
    }
    else {
        err = tjDecompress2(inst, jpeg->data, jpeg->length, (uint8_t*)pixels,
            strat.decode_width, fb->pitch, strat.decode_height, TJPF_BGRX, 0);
        if (err < 0) {
            fprintf(stderr, "jpeg decompress: %s\n", tjGetErrorStr2(inst));
            tjDestroy(inst);
            return -1;
        }

        double t1 = time_f();

        if (trace_resize) {
            fprintf(stderr, "  jpeg:   %5.3f\n", t1 - t0);
        }

        pixels += strat.decode_width;
        border_height = strat.decode_height;

        tjDestroy(inst);
    }

    // right border of current row + left border of next row
    border = strat.border_left + strat.border_right;
    if (border) {
        int i;
        for (i = 0; i < border_height - 1; i++) {
            memset32(pixels, BG_Color, border);
            pixels += fb->pitch32;
        }
    }
    else {
        // no left or right border
        pixels += fb->pitch32 * (border_height - 1);
    }

    // right border of last row + bottom border
    border = strat.border_right + strat.border_bottom * fb->pitch32;
    memset32(pixels, BG_Color, border);

    if (trace_resize) {
        double t_total = time_f() - t0;
        fprintf(stderr, "  total:  %5.3f\n", t_total);
    }

    return 0;
}


// Graphics cards, connectors, and displays.
// How to enumerate them, present them to the user, and pick one.

// One connector, e.g. an HDMI port
struct Connector {
    STAILQ_ENTRY(Connector) pointers;
    drmModeConnector* drm_conn;
    int conn_ix;
    bool good;
    int best_mode_ix;
};

// One graphics card, e.g. /dev/dri/card0
// each card has a list of connectors.
struct Card {
    STAILQ_ENTRY(Card) pointers;

    char* dev_path; // just for printing messages
    int fd_drm;
    drmModeRes* drm_res;

    STAILQ_HEAD(Connector_list, Connector) connectors;
};

// Global list of all cards with frame buffers (no render-only nodes).
STAILQ_HEAD(Card_list, Card) All_Cards;



struct Connector* connector_create(int fd_drm, int conn_ix, uint32_t conn_id)
{
    drmModeConnector* drm_conn = drmModeGetConnector(fd_drm, conn_id);
    if (drm_conn == 0) {
        perror("drmModeGetConnector()");
        return 0;
    }

    struct Connector* conn = malloc(sizeof(struct Connector));
    if (conn == 0) {
        perror("malloc(struct Connector)");
        drmModeFreeConnector(drm_conn);
        return 0;
    }
    conn->drm_conn = drm_conn;
    conn->conn_ix = conn_ix;
    conn->good = (drm_conn->connection == DRM_MODE_CONNECTED) && (drm_conn->count_modes > 0);

    int best_ix = -1;
    int mode_ix;
    for (mode_ix = 0; mode_ix < drm_conn->count_modes; mode_ix++) {
        drmModeModeInfo* mode = &drm_conn->modes[mode_ix];
        if (best_ix == -1) {
            // default to first if none are preferred
            best_ix = mode_ix;
        }
        if (mode->type & DRM_MODE_TYPE_PREFERRED) {
            // stop searching after finding preferred
            best_ix = mode_ix;
            break;
        }
    }
    conn->best_mode_ix = best_ix;
    return conn;
}

void connector_free(struct Connector* conn)
{
    drmModeFreeConnector(conn->drm_conn);
    free(conn);
}

void connector_print(struct Connector* conn, const char* dev_path, int* total_conn_ix, FILE* out)
{
    fprintf(out, "Output %i: Connector %i (%i) %s-%i %s",
        (*total_conn_ix)++,
        conn->conn_ix,
        conn->drm_conn->connector_id,
        drmModeGetConnectorTypeName(conn->drm_conn->connector_type),
        conn->drm_conn->connector_type_id,
        conn->drm_conn->connection == DRM_MODE_CONNECTED ?
            "(connected)" : "(disconnected)");

    if (conn->best_mode_ix >= 0) {
        drmModeModeInfo* mode_info = &conn->drm_conn->modes[conn->best_mode_ix];
        fprintf(out, " %ix%i@%i\n", mode_info->hdisplay, mode_info->vdisplay,
            mode_info->vrefresh);
    } else {
        fprintf(out, "\n");
    }
}



struct Card* card_create(const char* dev_path)
{
    int fd_drm = open(dev_path, O_RDWR | O_CLOEXEC);
    if (fd_drm < 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "Card %s: %s\n", dev_path, strerror(errno));
        }
        return 0;
    }

    drmModeRes* drm_res = drmModeGetResources(fd_drm);
    if (drm_res == 0) {
        //fprintf(stderr, "Card %s: no display resources.\n", dev_path);
        close(fd_drm);
        return 0;
    }

    struct Card* card = malloc(sizeof(struct Card));
    if (card == 0) {
        perror("malloc(struct Card)");
        drmModeFreeResources(drm_res);
        close(fd_drm);
        return 0;
    }
    card->fd_drm = fd_drm;
    card->drm_res = drm_res;
    card->dev_path = strdup(dev_path);

    STAILQ_INIT(&card->connectors);

    int conn_ix;
    for (conn_ix = 0; conn_ix < drm_res->count_connectors; conn_ix++) {
        uint32_t conn_id = drm_res->connectors[conn_ix];
        struct Connector* conn = connector_create(fd_drm, conn_ix, conn_id);
        if (conn) {
            STAILQ_INSERT_TAIL(&card->connectors, conn, pointers);
        }
    }

    return card;
}

void card_free(struct Card* card)
{
    drmModeFreeResources(card->drm_res);
    close(card->fd_drm);
    free(card->dev_path);
    free(card);
}

void card_print(struct Card* card, int* total_conn_ix, FILE* out)
{
    fprintf(out, "Card %s\n", card->dev_path);

    struct Connector* conn;
    STAILQ_FOREACH(conn, &card->connectors, pointers) {
        connector_print(conn, card->dev_path, total_conn_ix, out);
    }
}

// Populate the All_Cards list. Only call this once.
// If a device path is given, only that device is opened.
// If device path is null, all frame buffer cards are opened.
// Return 0 if success and we found at least 1 output.
int populate_cards(const char* dev_path_or_null)
{
    STAILQ_INIT(&All_Cards);

    if (dev_path_or_null) {
        // just the specified card
        struct Card* card = card_create(dev_path_or_null);
        if (card == 0) {
            fprintf(stderr, "card_create(%s): %s\n", dev_path_or_null,
                strerror(errno));
            return -1;
        }
        STAILQ_INSERT_TAIL(&All_Cards, card, pointers);
        return 0;
    }

    // Try /dev/dri/card0, card1, etc.
    int outputs = 0;
    int card;
    for (card = 0; card < 10; card++) {
        char card_path[64];
        snprintf(card_path, sizeof(card_path), "/dev/dri/card%i", card);
        //fprintf(stderr, "Opening %s\n", card_path);

        struct Card* card = card_create(card_path);
        if (card == 0) {
            if (errno == ENOENT) {
                // no more cards
                break;
            }
            // else ignore error and try next card
        }
        else {
            // found a card with connectors, add to global list
            outputs += card->drm_res->count_connectors;
            STAILQ_INSERT_TAIL(&All_Cards, card, pointers);
        }
    }
    if (outputs == 0) {
        fprintf(stderr, "No outputs found.\n");
        errno = ENODEV;
        return -1;
    }
    return 0;
}

// Show list of cards to user.
// Command line flag: --list
void print_all_cards(FILE* out)
{
    int total_conn_ix = 0;

    struct Card* card;
    STAILQ_FOREACH(card, &All_Cards, pointers) {
        card_print(card, &total_conn_ix, out);
    }
}

// The specific card and connector we will use.
struct Card* My_Card = 0;
struct Connector* My_Conn = 0;

// Look through All_Cards and pick a card and connector to use.
// output_ix is picked by the user --out=x, or -1 to pick the first
// connected connector.
void pick_output(int output_ix)
{
    int conn_ix = 0;

    struct Card* card;
    STAILQ_FOREACH(card, &All_Cards, pointers) {
        struct Connector* conn;
        STAILQ_FOREACH(conn, &card->connectors, pointers) {
            if (output_ix == conn_ix || (output_ix == -1 && conn->good)) {
                // Use this connector.
                My_Card = card;
                My_Conn = conn;
                return;
            }
            conn_ix++;
        }
    }
}

// After we are done listing/picking, free resources we won't be using.
void close_other_cards_and_connectors()
{
    struct Card* card;
    while ((card = STAILQ_FIRST(&All_Cards))) {
        if (card == My_Card) break;
        STAILQ_REMOVE_HEAD(&All_Cards, pointers);
        card_free(card);
    }
    if (card == 0) return;

    struct Card* del;
    while ((del = STAILQ_NEXT(card, pointers))) {
        STAILQ_REMOVE(&All_Cards, del, Card, pointers);
        card_free(del);
    }

    struct Connector* conn;
    while ((conn = STAILQ_FIRST(&card->connectors))) {
        if (conn == My_Conn) break;
        STAILQ_REMOVE_HEAD(&card->connectors, pointers);
        connector_free(conn);
    }
    if (conn == 0) return;

    struct Connector* del_conn;
    while ((del_conn = STAILQ_NEXT(conn, pointers))) {
        STAILQ_REMOVE(&card->connectors, del_conn, Connector, pointers);
        connector_free(del_conn);
    }
}

// Match the beginning part of a string, and return pointer to
// the character after.
// const char* input = "--value=123";
// const char* suffix = match_prefix(input, "--value=")'
// suffix points to '123'
const char* match_prefix(const char* s, const char* prefix)
{
    int n = strlen(prefix);
    if (!strncmp(s, prefix, n)) {
        return s + n;
    }
    return 0;
}

void print_usage(const char* fmt, ...)
{
    FILE* out = stderr;
    va_list ap;
    if (fmt) {
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fprintf(out, "\n");
    }
    fprintf(out, "Usage: ./console-jpeg [options] [commands]\n");
    fprintf(out, "\n");
    fprintf(out, "Options:\n");
    fprintf(out, "--list                List available outputs\n");
    fprintf(out, "--dev=/dev/dri/card1  Specify device (rarely needed!)\n");
    fprintf(out, "--out=N               Select output port (from --list)\n");
    fprintf(out, "\n");
    fprintf(out, "Commands:\n");
    fprintf(out, "bgcolor:ffffff Set background/border color to hex RGB.\n");
    fprintf(out, "clear          Fill screen with bgcolor.\n");
    fprintf(out, "black          Fill screen with black.\n");
    fprintf(out, "white          Fill screen with white.\n");
    fprintf(out, "jpeg:file.jpg  Display a jpeg on the screen.\n");
    fprintf(out, "file.jpg       You can skip the jpeg: prefix.\n");
    fprintf(out, "wait:1.23      Pause x seconds.\n");
    fprintf(out, "halt           Stop forever (Ctrl-C to quit).\n");
    fprintf(out, "exit           Quit program.\n");
    fprintf(out, "sleep          Put the display to sleep.\n");
    fprintf(out, "\n");
    fprintf(out, "After processing command line arguments, console-jpeg\n");
    fprintf(out, "reads further commands from stdin. Use a shell script\n");
    fprintf(out, "to pass in jpeg filenames for display. Make sure the\n");
    fprintf(out, "output of the command-generating program is line buffered.\n");
    fprintf(out, "\n");
    fprintf(out, "On the Raspberry Pi 4, console-jpeg automatically picks\n");
    fprintf(out, "the correct /dev/dri/card. You don't need to use --dev.\n");
}

int main(int argc, const char* argv[])
{
    const char* arg_dev_path = 0;
    bool flag_list_outputs = false;
    int chose_output = -1;

    const char* arg;
    int argi;
    // process initial command line args starting with '-'
    for (argi = 1; (arg = argv[argi]) && arg[0] == '-'; argi++) {
        if ((arg = match_prefix(argv[argi], "--dev="))) {
            arg_dev_path = arg;
        }
        else if (!strcmp(argv[argi], "--list")) {
            flag_list_outputs = true;
        }
        else if ((arg = match_prefix(argv[argi], "--out="))) {
            chose_output = strtoul(arg, 0, 10);
        }
        else if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) {
            print_usage(0);
            return 0;
        }
        else {
            print_usage("Bad option: %s\n", argv[argi]);
            return 2;
        }
    }

    populate_cards(arg_dev_path);

    if (flag_list_outputs) {
        print_all_cards(stdout);
        return 0;
    }

    pick_output(chose_output);

    if (My_Card == 0 || My_Conn == 0) {
        fprintf(stderr, "No output found.\n");
        return 1;
    }

    close_other_cards_and_connectors();

    drmModeModeInfo* mode_info = &My_Conn->drm_conn->modes[My_Conn->best_mode_ix];

    drmModeEncoder* encoder = drmModeGetEncoder(My_Card->fd_drm,
        My_Conn->drm_conn->encoder_id);
    if (encoder == 0) {
        fprintf(stderr, "No encoder.\n");
        return 1;
    }

    uint32_t width = mode_info->hdisplay;
    uint32_t height = mode_info->vdisplay;
    int err = create_two_frame_buffers(My_Card->fd_drm, width, height);
    if (err) {
        return 2;
    }

    uint32_t crtc_id = encoder->crtc_id;
    drmModeCrtc* saved_crtc = drmModeGetCrtc(My_Card->fd_drm, crtc_id);

    struct sigaction act = { 0 };
    act.sa_handler = ctrl_c_handler;
    act.sa_flags = 0; // no SA_RESTART, otherwise fgets blocks ctrl+c
    sigaction(SIGINT, &act, 0);

    // Double buffering:
    // First buffer uses drmModeSetCrtc().
    // Subsequent buffers use drmModePageFlip().
    // Also need drmModeSetCrtc() to come out of display power-down.
    bool first_flip = true;
    char buf[1024];
    int ret = 0;
    while (!Quit) {
        // Process commands, frist from the command line, then from stdin.
        const char* command;
        if (argi < argc) {
            command = argv[argi++];
        }
        else if (fgets(buf, sizeof(buf), stdin)) {
            // strip newline
            char* nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            command = buf;
        }
        else {
            // EOF
            break;
        }

        // skip empty lines
        if (*command == 0) continue;

        if (!strcmp(command, "black")) {
            memset32((uint32_t*)FB0->pixels, 0, FB0->size32);
        }
        else if (!strcmp(command, "white")) {
            memset32((uint32_t*)FB0->pixels, 0xffffff, FB0->size32);
        }
        else if (!strcmp(command, "clear")) {
            memset32((uint32_t*)FB0->pixels, BG_Color, FB0->size32);
        }
        else if ((arg = match_prefix(command, "wait:"))) {
            // pause for x.x seconds
            double t = strtod(arg, 0);
            sleep_f(t);
            continue; // since we didn't draw anything
        }
        else if ((arg = match_prefix(command, "bgcolor:"))) {
            BG_Color = strtoul(arg, 0, 16);
            continue; // no drawing, don't flip the buffers
        }
        else if (!strcmp(command, "sleep")) {
            // put display to sleep
            // next jpeg or clear will wake it up
            err = drmModeSetCrtc(My_Card->fd_drm, crtc_id, 0, 0, 0, 0, 0, 0);
            if (err) {
                perror("drmModeSetCrtc(sleep)");
                ret = 3;
                goto Cleanup;
            }
            first_flip = true;
            continue;
        }
        else if (!strcmp(command, "halt")) {
            // wait forever
            while (!Quit) sleep(10);
            break;
        }
        else if (!strcmp(command, "exit")) {
            break;
        }
        else {
            const char* filename;
            if ((arg = match_prefix(command, "jpeg:"))) {
                filename = arg;
            }
            else {
                // plain filename, no explicit "jpeg:"
                filename = command;
            }

            struct Mapped_Jpeg* jpeg = jpeg_create(filename);
            if (jpeg == 0) continue; // bad filename or corrupt image
            err = jpeg_decode(jpeg, FB0);
            jpeg_destroy(jpeg);
            if (err) continue;
        }

        if (first_flip) {
            first_flip = false;
            err = drmModeSetCrtc(My_Card->fd_drm, crtc_id, FB0->fb_id, 0, 0,
                         &My_Conn->drm_conn->connector_id, 1, mode_info);
            if (err) {
                perror("drmModeSetCrtc(FB0)");
                ret = 3;
                goto Cleanup;
            }
        }
        else {
            // Schedule buffer flip.
            // May need to wait for vblank and retry if was are generating
            // frames faster than the refresh rate.
            while (!Quit) {
                err = drmModePageFlip(My_Card->fd_drm, crtc_id, FB0->fb_id, 0, 0);
                if (err == 0) {
                    // success
                    break;
                }
                if (errno != EBUSY) {
                    // a real error
                    perror("drmModePageFlip(FB0)");
                    ret = 3;
                    goto Cleanup;
                }

                // EBUSY
                // We tried to schedule a buffer flip while another flip
                // was still pending. We are drawing frames faster than the
                // monitor refresh.
                //
                // This only happens when clearing the screen or showing
                // small jpegs. It's not worth setting up events and a select
                // loop. We handle it here.
                //
                // drmWaitVBlank() was always returning EINVAL
                // Sleeping for 5 ms and retrying works, too.
                sleep_f(5e-3);
            }
        }
        swap_frame_buffers();
    }

Cleanup:
    if (saved_crtc) {
        drmModeSetCrtc(My_Card->fd_drm, saved_crtc->crtc_id,
            saved_crtc->buffer_id, saved_crtc->x, saved_crtc->y,
            &My_Conn->drm_conn->connector_id, 1, &saved_crtc->mode);
    }
    return ret;
}
