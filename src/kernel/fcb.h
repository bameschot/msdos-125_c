#pragma once
#include "dos.h"

/* -----------------------------------------------------------------------
 * FCB and directory operations:
 *   MOVNAME, GETNAME, FINDNAME, GETENTRY, NEXTENTRY, GETFILE
 *   OPEN, CLOSE, CREATE, DELETE, RENAME
 *   SRCHFRST, SRCHNXT, MAKEFCB
 * ----------------------------------------------------------------------- */

/* Parse FCB pointed to by fcb_ptr; set dos->name1, dos->attrib, dos->thisdrv.
 * Returns 0 on success, -1 on bad drive/filename. */
int  fcb_movname(dos_t *dos, fcb_t *fcb);

/* Same as fcb_movname but also searches directory.  Sets dos->thisbp etc.
 * On success: dirbuf has matching entry, *out_bx = offset into dirbuf,
 * *out_si = offset of first-cluster field, *out_bh<0 = device, else drive. */
int  fcb_getname(dos_t *dos, fcb_t *fcb,
                 uint16_t *out_bx, uint16_t *out_si, int16_t *out_bh);

/* Like fcb_getname but also sets result_es:result_di = original FCB. */
int  fcb_getfile(dos_t *dos, fcb_t *fcb,
                 uint16_t *out_bx, uint16_t *out_si, int16_t *out_bh);

/* Check if name1 matches a named device.
 * Returns device index (0-based) or -1. */
int  fcb_devname(dos_t *dos);

/* System call implementations */
uint8_t dos_open(dos_t *dos, fcb_t *fcb);
uint8_t dos_close(dos_t *dos, fcb_t *fcb);
uint8_t dos_create(dos_t *dos, fcb_t *fcb);
uint8_t dos_delete(dos_t *dos, fcb_t *fcb);
uint8_t dos_rename(dos_t *dos, fcb_t *fcb);
uint8_t dos_srchfrst(dos_t *dos, fcb_t *fcb);
uint8_t dos_srchnxt(dos_t *dos, fcb_t *fcb);

/* Parse filename string into FCB (MAKEFCB, INT 21H fn 41H).
 * src = input string pointer (advanced past parsed text).
 * flags: bit0=scan delimiters, bit1=default drive, bit2=default name, bit3=default ext.
 * Returns 0=unambiguous, 1=wildcard, 0xFF=error. */
uint8_t dos_makefcb(dos_t *dos, const char **src, fcb_t *fcb, uint8_t flags);

