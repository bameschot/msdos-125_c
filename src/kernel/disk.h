#pragma once
#include "dos.h"

/* -----------------------------------------------------------------------
 * Disk buffer management and sector-level I/O (DREAD / DWRITE / HARDERR).
 * ----------------------------------------------------------------------- */

/* Ensure FAT is current for drive thisdrv; set dos->thisbp. */
int  disk_fatread(dos_t *dos);

/* Compute DPB pointer from device number. */
dpb_t *disk_getbp(dos_t *dos, uint8_t devnum);

/* Low-level sector read; calls HARDERR on error. */
int  disk_read(dos_t *dos, dpb_t *dp, uint8_t *buf,
               uint16_t count, uint32_t sector);

/* Low-level sector write; calls HARDERR on error. */
int  disk_write(dos_t *dos, dpb_t *dp, const uint8_t *buf,
                uint16_t count, uint32_t sector);

/* Ensure the single sector buffer holds 'sector' from drive dp.
 * write_mode = true when about to write (may skip pre-read).
 * Returns pointer to buffer, NULL on error. */
uint8_t *disk_bufsec(dos_t *dos, dpb_t *dp,
                     uint16_t cluster, uint8_t sec_in_clus,
                     bool write_mode);

/* Flush dirty sector buffer if dirty. */
int disk_buf_flush(dos_t *dos);

/* Read directory sector 'block' into dirbuf. */
int  disk_dirread(dos_t *dos, dpb_t *dp, uint16_t block);

/* Write back dirty directory buffer. */
int  disk_dirwrite(dos_t *dos, dpb_t *dp);

/* Check and flush directory buffer if dirty. */
int  disk_chkdirwrite(dos_t *dos, dpb_t *dp);

/* Compute absolute sector number for cluster+offset. */
uint32_t disk_figrec(dpb_t *dp, uint16_t cluster, uint8_t sec_in_clus);

/* Write all dirty FATs back (WRTFATS / DSKRESET). */
int  disk_reset(dos_t *dos);
