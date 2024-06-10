#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#include "drm_search.h"
#include "frame_buffer.h"
#include "read_jpeg.h"
#include "read_heif.h"
#include "read_png.h"
#include "util.h"

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
int create_two_frame_buffers(int fd_drm, uint32_t width, uint32_t height,
            uint32_t pixel_format)
{
    FB0 = frame_buffer_create(fd_drm, width, height, pixel_format);
    if (FB0 == 0) {
        return -1;
    }

    if (frame_buffer_map(FB0)) {
        frame_buffer_destroy(FB0);
        FB0 = 0;
        return -1;
    }

    FB1 = frame_buffer_create(fd_drm, width, height, pixel_format);
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

bool match_case_suffix_list(const char* s, ...)
{
    const char* p = strrchr(s, '/');
    if (p == 0) p = s;
    p = strrchr(p, '.');
    if (p == 0) return false;

    bool match;
    va_list args;
    va_start(args, s);
    do {
        const char* suffix = va_arg(args, const char*);
        if (suffix == 0) break;
        match = !strcasecmp(p, suffix);
    } while (!match);

    va_end(args);
    return match;
}

void print_usage(FILE* out, const char* fmt, ...)
{
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
    fprintf(out, "heif:file.heic Display a heif on the screen.\n");
    fprintf(out, "png:file.png   Display a png on the screen.\n");
    fprintf(out, "file.jpg       No prefix, determine type from extension.\n");
    fprintf(out, "wait:1.23      Pause x seconds.\n");
    fprintf(out, "halt           Stop forever (Ctrl-C to quit).\n");
    fprintf(out, "exit           Quit program.\n");
    fprintf(out, "sleep          Put the display to sleep.\n");
    fprintf(out, "\n");
    fprintf(out, "After processing command line arguments, console-jpeg\n");
    fprintf(out, "reads further commands from stdin. Use a shell script\n");
    fprintf(out, "to pass in image filenames for display. Make sure the\n");
    fprintf(out, "output of the command-generating program is line buffered.\n");
    fprintf(out, "\n");
    fprintf(out, "On the Raspberry Pi 4, console-jpeg automatically picks\n");
    fprintf(out, "the correct /dev/dri/card. You don't need to use --dev.\n");
}

int main(int argc, const char* argv[])
{
    File_Info = stdout;
    File_Error = stderr;

    const char* arg_dev_path = 0;
    bool flag_list_outputs = false;
    int chose_output = -1;

    const char* arg;
    int argi;
    // process initial command line args starting with '-'
    for (argi = 1; (arg = argv[argi]) && arg[0] == '-'; argi++) {
        if ((arg = match_prefix(argv[argi], "--dev=")))
        {
            arg_dev_path = arg;
        }
        else if ((arg = match_prefix(argv[argi], "--fmt=")))
        {
            uint32_t four_cc = str_to_four_cc(arg);
            override_pixel_format_preference(four_cc);
        }
        else if (!strcmp(argv[argi], "-l") ||
                 !strcmp(argv[argi], "--list"))
        {
            flag_list_outputs = true;
        }
        else if ((arg = match_prefix(argv[argi], "-o=")) ||
                 (arg = match_prefix(argv[argi], "--out=")))
        {
            chose_output = strtoul(arg, 0, 10);
        }
        else if (!strcmp(argv[argi], "-v") ||
                 !strcmp(argv[argi], "--verbose"))
        {
            Verbose = true;
        }
        else if (!strcmp(argv[argi], "-h") ||
                 !strcmp(argv[argi], "--help"))
        {
            print_usage(File_Error, 0);
            return 0;
        }
        else {
            print_usage(File_Error, "Bad option: %s\n", argv[argi]);
            return 2;
        }
    }

    populate_cards(arg_dev_path);

    if (flag_list_outputs) {
        print_all_cards(File_Info);
        return 0;
    }

    pick_output(chose_output);

    if (My_Card == 0 || My_Conn == 0) {
        fprintf(File_Error, "Error: No output found.\n");
        return 1;
    }

    close_other_cards_and_connectors();

    drmModeModeInfo* mode_info = &My_Conn->drm_conn->modes[My_Conn->best_mode_ix];

    drmModeEncoder* encoder = drmModeGetEncoder(My_Card->fd_drm,
        My_Conn->drm_conn->encoder_id);
    if (encoder == 0) {
        fprintf(File_Error, "Error: No encoder.\n");
        return 1;
    }

    uint32_t pixel_format;
    if (choose_pixel_format(&pixel_format)) {
        fprintf(File_Error, "Error: No acceptable pixel format.\n");
        return 1;
    }
    const struct Pixel_Format* pf = lookup_pixel_format(pixel_format);
    uint32_t bytes_per_pixel = pf->bytes_per_pixel;

    if (Verbose) {
        fprintf(File_Info, "Picked '%s', %i bytes/pix\n",
            four_cc_to_str(pixel_format), bytes_per_pixel);
    }

    uint32_t width = mode_info->hdisplay;
    uint32_t height = mode_info->vdisplay;
    int err = create_two_frame_buffers(My_Card->fd_drm, width, height, pixel_format);
    if (err) {
        return 2;
    }

    uint32_t crtc_id = encoder->crtc_id;
    drmModeCrtc* saved_crtc = drmModeGetCrtc(My_Card->fd_drm, crtc_id);

    install_ctrl_c_handler();

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
        if (*command == 0) {
            //continue;
            command = "flip";
        }

        if (!strcmp(command, "black")) {
            fill_rect(FB0, 0x000000, 0, 0, -1, -1);
        }
        else if (!strcmp(command, "white")) {
            fill_rect(FB0, 0xffffff, 0, 0, -1, -1);
        }
        else if (!strcmp(command, "clear")) {
            fill_rect(FB0, BG_Color, 0, 0, -1, -1);
        }
        else if (!strcmp(command, "flip")) {
            // swap buffers again without drawing
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
            // An image file.
            enum {
                FMT_JPEG,
                FMT_HEIF,
                FMT_PNG
            } fmt;
            const char* filename = 0;

            if ((arg = match_prefix(command, "jpeg:"))) {
                fmt = FMT_JPEG;
                filename = arg;
            }
            else if ((arg = match_prefix(command, "heif:"))) {
                fmt = FMT_HEIF;
                filename = arg;
            }
            else if ((arg = match_prefix(command, "png:"))) {
                fmt = FMT_PNG;
                filename = arg;
            }
            else if (match_case_suffix_list(command, ".jpg", ".jpeg", 0)) {
                fmt = FMT_JPEG;
                filename = command;
            }
            else if (match_case_suffix_list(command, ".heif", ".heic", 0)) {
                fmt = FMT_HEIF;
                filename = command;
            }
            else if (match_case_suffix_list(command, ".png", 0)) {
                fmt = FMT_PNG;
                filename = command;
            }
            else {
                fprintf(File_Error, "Error: Unknown file type: %s\n", command);
                continue;
            }

            if (fmt == FMT_JPEG) {
                if (read_jpeg(filename, FB0)) {
                    continue;
                }
            }
            else if (fmt == FMT_HEIF) {
                if (read_heif(filename, FB0)) {
                    continue;
                }
            }
            else if (fmt == FMT_PNG) {
                if (read_png(filename, FB0)) {
                    continue;
                }
            }
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
