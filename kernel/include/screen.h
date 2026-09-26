#ifndef SCREEN_H
#define SCREEN_H

#include "typesk.h"

void print_string(const char *str, uint8 color);

void print_hex(uint32 value, uint8 color);

void clear_screen(uint8 color);

void print_hex64(uint64 value, uint8 color);

/* One character, and a destructive backspace. Both exist for the terminal
 * echo path in keyboard.c; print_string cannot express either. */
void print_char(char c, uint8 color);
void print_backspace(uint8 color);

#endif