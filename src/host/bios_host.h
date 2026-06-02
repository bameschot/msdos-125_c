#pragma once
#include <stdio.h>
#include "../kernel/bios.h"

/* -----------------------------------------------------------------------
 * Host BIOS implementation: replaces IO.ASM.
 *
 * Console I/O: uses POSIX termios for raw key input; stdout for output.
 * Disk I/O:    reads/writes a raw FAT12 image file.
 * Date/time:   uses the host system clock.
 * ----------------------------------------------------------------------- */

typedef struct {
    bios_t   base;          /* must be first — cast-compatible */

    /* Console */
    int      saved_tty;     /* 1 if termios was changed */

    /* Disk images: one .img file per drive */
    FILE    *drives[16];
    uint16_t secsiz[16];    /* bytes per sector for each drive */
    uint8_t  num_drives;
} host_bios_t;

/* Initialise host BIOS.
 * img_paths: array of disk image file paths (one per drive).
 * n_drives:  number of drives.
 * Returns 0 on success. */
int  host_bios_init(host_bios_t *hb, const char **img_paths, int n_drives);

/* Shut down: restore terminal, close files. */
void host_bios_shutdown(host_bios_t *hb);

/* Create a blank FAT12 disk image (720 KB by default).
 * Fills in the BPB and root directory; calls FORMAT logic. */
int  host_bios_format(const char *path, uint8_t secs_per_track,
                      uint8_t heads, uint16_t total_sectors,
                      uint16_t secsiz, uint8_t secs_per_clus);
