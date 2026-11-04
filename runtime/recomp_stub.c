/*
 * recomp_stub.c - used when no recompiled game code has been generated
 * yet: every instruction runs through the interpreter.
 */
#include "nes.h"

int recomp_run(void) { return 0; }
const char *recomp_rom_sha1 = NULL;
