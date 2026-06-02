#pragma once
#include "dos.h"

/* -----------------------------------------------------------------------
 * Kernel initialisation and system call dispatch.
 * ----------------------------------------------------------------------- */

/* Initialise DOS from disk (reads FAT, sets up DPBs).
 * bios must be set up before calling.
 * Returns 0 on success. */
int dos_init(dos_t *dos, bios_t *bios);

/* Execute one DOS system call (INT 21H equivalent).
 * fn  = function number (AH)
 * r   = register arguments; AL in r->ax on return holds result.
 */
uint8_t dos_call(dos_t *dos, uint8_t fn, dos_regs_t *r);

/* Helper to get drive DPB (ensures FAT is current). */
dpb_t *dos_getdpb(dos_t *dos, uint8_t drv);

/* Set the DMA (disk transfer) address for subsequent FCB operations. */
void dos_setdma(dos_t *dos, uint8_t *addr);
