#pragma once
#include "../kernel/dos.h"

/* COMMAND.COM — the MS-DOS 1.25 command interpreter. */

/* Run the command interpreter loop.  Does not return until the user
 * types EXIT or an unrecoverable error occurs. */
void command_run(dos_t *dos);

/* Execute a single command line.  Returns 0 on success, -1 on fatal error. */
int command_exec(dos_t *dos, const char *line);
