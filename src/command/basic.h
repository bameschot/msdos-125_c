#pragma once
#include "../kernel/dos.h"

/* basic_run — load and execute the named BASIC source file from the
 * current FAT12 directory.  Prints errors to the console via dos. */
void basic_run(dos_t *dos, const char *filename);
