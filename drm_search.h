#ifndef DRM_SEARCH_H
#define DRM_SEARCH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/queue.h>

#include <xf86drmMode.h>

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
STAILQ_HEAD(Card_list, Card);
extern struct Card_list All_Cards;

// The specific card and connector we will use.
extern struct Card* My_Card;
extern struct Connector* My_Conn;


// Populate the All_Cards list. Only call this once.
// If a device path is given, only that device is opened.
// If device path is null, all frame buffer cards are opened.
// Return 0 if success and we found at least 1 output.
int populate_cards(const char* dev_path_or_null);

// Show list of cards to user.
// Command line flag: --list
void print_all_cards(FILE* out);

// Look through All_Cards and pick a card and connector to use.
// Sets My_Card and My_Conn.
// output_ix is picked by the user --out=x, or -1 to pick the first
// connected connector.
void pick_output(int output_ix);

// After we are done listing/picking, free resources we won't be using.
void close_other_cards_and_connectors();

int choose_pixel_format(uint32_t *four_cc);



const char* four_cc_to_str(uint32_t four_cc);

uint32_t str_to_four_cc(const char* s);

struct Pixel_Format {
    uint32_t four_cc;
    int bytes_per_pixel;
    int rank; // rank >= 0, lower is better
    int red_first;
};

const struct Pixel_Format* lookup_pixel_format(uint32_t drm_four_cc);

int override_pixel_format_preference(uint32_t best_four_cc);




#endif
