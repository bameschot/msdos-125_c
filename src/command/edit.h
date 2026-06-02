#pragma once
#include "../kernel/dos.h"

/* -----------------------------------------------------------------------
 * EDIT — screen-based text file editor.
 * Opens filename from the current FAT12 drive, lets the user view and
 * edit the content, and writes it back on save.  Creates a new file if
 * the name does not yet exist on disk.
 * ----------------------------------------------------------------------- */

void editor_run(dos_t *dos, const char *filename);
