#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>

// signal handler got ctrl-c
extern volatile bool Quit;

// Background color for border around images.
extern uint32_t BG_Color;

double time_f();
void sleep_f(double secs);

// Sets Quit = true.
void ctrl_c_handler(int signum);


#endif
