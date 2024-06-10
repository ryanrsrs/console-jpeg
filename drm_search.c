#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "drm_search.h"
#include "util.h"

// Global list of all cards with frame buffers (no render-only nodes).
struct Card_list All_Cards;

// The specific card and connector we will use.
struct Card* My_Card = 0;
struct Connector* My_Conn = 0;

static struct Connector* connector_create(int fd_drm, int conn_ix, uint32_t conn_id);
static void connector_free(struct Connector* conn);
static void connector_print(struct Connector* conn, const char* dev_path, int* total_conn_ix, FILE* out);

static struct Card* card_create(const char* dev_path);
static void card_free(struct Card* card);
static void card_print(struct Card* card, int* total_conn_ix, FILE* out);


struct Connector* connector_create(int fd_drm, int conn_ix, uint32_t conn_id)
{
    drmModeConnector* drm_conn = drmModeGetConnector(fd_drm, conn_id);
    if (drm_conn == 0) {
        fprintf(File_Error, "Error: drmModeGetConnector(): %s\n",
                strerror(errno));
        return 0;
    }

    struct Connector* conn = malloc(sizeof(struct Connector));
    if (conn == 0) {
        fprintf(File_Error, "Error: malloc(struct Connector) failed.\n");
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
        fprintf(File_Error, "Error: malloc(struct Card) failed.\n");
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

int choose_pixel_format(uint32_t *four_cc)
{
    drmModePlaneRes* res = drmModeGetPlaneResources(My_Card->fd_drm);
    if (res == 0) {
        fprintf(stderr, "drmModeGetPlaneResources() returned null.\n");
        return -1;
    }

    const struct Pixel_Format* best_pf = 0;

    int plane_i;
    for (plane_i = 0; plane_i < res->count_planes; plane_i++) {
        drmModePlane* plane = drmModeGetPlane(My_Card->fd_drm,
                                res->planes[plane_i]);
        if (plane == 0) continue;

        int i;
        for (i = 0; i < plane->count_formats; i++) {
            uint32_t fmt = plane->formats[i];
            const struct Pixel_Format* pf = lookup_pixel_format(fmt);
            //fprintf(stderr, "pixel format: '%s'%s\n", four_cc_to_str(fmt), pf ? " *" : "");
            if (pf == 0) continue;
            if (best_pf == 0 || pf->rank < best_pf->rank) {
                best_pf = pf;
            }
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(res);

    if (best_pf == 0) {
        // didn't find an acceptable format
        return -1;
    }

    *four_cc = best_pf->four_cc;
    return 0;
}

// Not exactly thread-safe, but won't crash.
const char* four_cc_to_str(uint32_t four_cc)
{
    // 10 buffers of 5 chars each
    static char buf[10][5] = { 0 };
    static int buf_i = 0;

    int i = buf_i++;
    int n = sizeof(buf) / sizeof(buf[0]);
    char* p = buf[i % n];
    p[0] = 255 & four_cc;
    p[1] = 255 & (four_cc >> 8);
    p[2] = 255 & (four_cc >> 16);
    p[3] = 255 & (four_cc >> 24);
    p[4] = 0;
    return p;
}

uint32_t str_to_four_cc(const char* s)
{
    uint32_t four_cc = 0;
    uint8_t ch;
    int i;
    for (i = 0; (ch = *s++) && i < 4; i++) {
        four_cc |= ch << (i * 8);
    }
    return four_cc;
}

static struct Pixel_Format My_Pixel_Formats[] = {
    { DRM_FORMAT_BGR888,   3, 1, 1 },
    { DRM_FORMAT_RGB888,   3, 2, 0 },
    { DRM_FORMAT_XBGR8888, 4, 3, 1 },
    { DRM_FORMAT_XRGB8888, 4, 4, 0 }
};

const struct Pixel_Format* lookup_pixel_format(uint32_t drm_four_cc)
{
    int i, n = sizeof(My_Pixel_Formats) / sizeof(My_Pixel_Formats[0]);
    for (i = 0; i < n; i++) {
        if (My_Pixel_Formats[i].four_cc == drm_four_cc) break;
    }
    return (i < n) ? &My_Pixel_Formats[i] : 0;
}

int override_pixel_format_preference(uint32_t best_four_cc)
{
    const struct Pixel_Format* pf = lookup_pixel_format(best_four_cc);
    if (pf == 0) {
        fprintf(stderr, "Error: override_pixel_format_preference() '%s' not found.\n",
            four_cc_to_str(best_four_cc));
        return -1;
    }
    ((struct Pixel_Format*)pf)->rank = 0;
    return 0;
}
