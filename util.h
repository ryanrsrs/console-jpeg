#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// signal handler got ctrl-c
extern volatile bool Quit;

// print stats
extern bool Verbose;

// Background color for border around images.
extern uint32_t BG_Color;

// stdout and stderr
extern FILE* File_Info;
extern FILE* File_Error;

double time_f();
void sleep_f(double secs);

// Sets Quit = true on ctrl-c.
void install_ctrl_c_handler();

#endif
