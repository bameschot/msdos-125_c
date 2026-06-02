#pragma once
#include "dos.h"

/* File I/O: SEQRD, SEQWRT, RNDRD, RNDWRT, BLKRD, BLKWRT, FILESIZE, SETRNDREC */

/* Compute 32-bit record position from FCB extent/nr fields (GETREC). */
uint32_t fileio_getrec(const fcb_t *fcb);

/* Set FCB extent/nr fields from a 32-bit record position (SETNREX). */
void fileio_setnrex(fcb_t *fcb, uint32_t recpos, uint16_t reccnt, uint8_t dskerr);

/* Sequential read (fn 20). */
uint8_t dos_seqrd(dos_t *dos, fcb_t *fcb);

/* Sequential write (fn 21). */
uint8_t dos_seqwrt(dos_t *dos, fcb_t *fcb);

/* Random read (fn 33). */
uint8_t dos_rndrd(dos_t *dos, fcb_t *fcb);

/* Random write (fn 34). */
uint8_t dos_rndwrt(dos_t *dos, fcb_t *fcb);

/* Block read (fn 39). count in/out via reccnt field of call registers. */
uint8_t dos_blkrd(dos_t *dos, fcb_t *fcb, uint16_t count, uint16_t *out_count);

/* Block write (fn 40). */
uint8_t dos_blkwrt(dos_t *dos, fcb_t *fcb, uint16_t count, uint16_t *out_count);

/* File size in records (fn 35). Result placed in FCB rr field. */
uint8_t dos_filesize(dos_t *dos, fcb_t *fcb);

/* Set random record from current position (fn 36). */
uint8_t dos_setrndrec(dos_t *dos, fcb_t *fcb);
