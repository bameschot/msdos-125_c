#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "keys.h"

/* -----------------------------------------------------------------------
 * BIOS interface — the hardware abstraction layer that replaces IO.ASM.
 * The host fills a bios_t struct; the kernel calls through it.
 * ----------------------------------------------------------------------- */

typedef struct bios_s bios_t;

struct bios_s {
    /* Console */
    int     (*stat)(bios_t *b);                         /* non-zero if key available */
    int     (*in)(bios_t *b);                           /* read char, no echo */
    void    (*out)(bios_t *b, uint8_t c);               /* write char to console */
    void    (*print)(bios_t *b, uint8_t c);             /* write char to printer */
    int     (*auxin)(bios_t *b);                        /* read from aux port */
    void    (*auxout)(bios_t *b, uint8_t c);            /* write to aux port */
    void    (*flush)(bios_t *b);                        /* flush input buffer */

    /* Disk
     * read/write: return 0 on success, non-zero error code on failure.
     * unit = dpb->drvnum (physical drive number)
     * buf  = sector buffer
     * count = number of sectors
     * sector = absolute logical sector number
     * On error, *sectors_done is set to how many sectors completed before error.
     */
    int     (*disk_read)(bios_t *b, uint8_t unit,
                         uint8_t *buf, uint16_t count, uint32_t sector,
                         uint16_t *sectors_done);
    int     (*disk_write)(bios_t *b, uint8_t unit, bool verify,
                          const uint8_t *buf, uint16_t count, uint32_t sector,
                          uint16_t *sectors_done);
    /* Returns: -1 = changed, 0 = unchanged, 1 = don't know. Negative = error. */
    int     (*disk_change)(bios_t *b, uint8_t unit);

    /* Date/time
     * gettime: fills h/m/s/c (0-23, 0-59, 0-59, 0-99).
     * Returns day count since Jan 1 1980 in *days_out.
     */
    void    (*gettime)(bios_t *b, uint16_t *days_out,
                       uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *c);
    void    (*settime)(bios_t *b, uint8_t h, uint8_t m, uint8_t s, uint8_t c);
    void    (*setdate)(bios_t *b, uint16_t days_since_1980);

    /* Screen control (optional) */
    void    (*cls)(bios_t *b);          /* clear screen; may be NULL */

    /* Extended keyboard read — blocks until a key is pressed.
     * Returns a KEY_* constant (see keys.h).  Handles ANSI escape sequences
     * for arrow keys, Page Up/Down, Home, End, Delete, Insert, F-keys.
     * May be NULL; the editor will fall back to bios->in() if so. */
    int     (*getkey)(bios_t *b);

    /* Query current terminal dimensions.
     * Falls back to 24×80 if the implementation cannot determine the size.
     * May be NULL. */
    void    (*getscreensize)(bios_t *b, int *rows, int *cols);

    /* Drive mapping: called after reading FAT to allow remapping.
     * Returns new device number.  unit = old unit, fat_byte = FAT[0]. */
    uint8_t (*mapdev)(bios_t *b, uint8_t unit, uint8_t fat_byte);

    /* Disk geometry: called during init.
     * drive_params must be filled with an array of {unit, dpt_ptr} pairs.
     * Returns number of drives configured (0 on failure). */
    int     (*get_drive_config)(bios_t *b, void *cfg_out, int max_drives);

    /* Host-private data */
    void    *priv;
};
