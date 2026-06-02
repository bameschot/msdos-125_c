#pragma once
#include "dos.h"

/* FAT12 read/write and cluster chain operations. */

/* Read FAT entry for cluster n */
uint16_t fat_unpack(const uint8_t *fat, uint16_t n);

/* Write FAT entry for cluster n */
void fat_pack(uint8_t *fat, uint16_t n, uint16_t val);

/* Walk chain from cluster 'start', skipping 'count' clusters.
 * Returns final cluster; *out_pos = position of that cluster in chain.
 * If count > chain length, returns last cluster (which will be EOF). */
uint16_t fat_fndclus(dos_t *dos, dpb_t *dp, fcb_t *fcb, uint16_t count,
                     uint16_t *out_pos);

/* Follow chain from cluster bx to end.  Returns last cluster. */
uint16_t fat_geteof(const uint8_t *fat, uint16_t bx);

/* Free chain starting at cluster bx. */
void fat_release(dos_t *dos, dpb_t *dp, uint16_t bx);

/* Free chain from bx onwards but put EOF mark in bx's entry.
 * (RELBLKS entry point — called with dx=0xFFF to truncate) */
void fat_relblks(dos_t *dos, dpb_t *dp, uint16_t bx, uint16_t eof_val);

/* Allocate 'count' new clusters, chaining them after 'tail'
 * (tail=0 means file is new).  Updates FCB firclus if needed.
 * Returns first allocated cluster, or 0 on disk full. */
uint16_t fat_allocate(dos_t *dos, dpb_t *dp, fcb_t *fcb,
                      uint16_t tail, uint16_t tail_pos, uint16_t count);

/* Write FAT back to disk (all copies). */
int fat_write(dos_t *dos, dpb_t *dp);

/* Read FAT from disk (called when disk may have changed). */
int fat_read(dos_t *dos, dpb_t *dp);
