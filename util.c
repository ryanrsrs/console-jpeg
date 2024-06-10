#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

#include "util.h"

volatile bool Quit = false;

bool Verbose = false;

uint32_t BG_Color = 0;

FILE* File_Info;
FILE* File_Error;


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

static void ctrl_c_handler(int signum)
{
    Quit = true;
}

void install_ctrl_c_handler()
{
    struct sigaction act = { 0 };
    act.sa_handler = ctrl_c_handler;
    act.sa_flags = 0; // no SA_RESTART, otherwise fgets blocks ctrl+c
    sigaction(SIGINT, &act, 0);
}
