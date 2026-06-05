/*
 * globals.c — Global variables shared between main.c and other modules.
 *
 * This file exists so that the test binary can link against buffer.o
 * (which references session_arena) without pulling in main.o.
 */

#include "arena.h"

Arena *session_arena = NULL;
