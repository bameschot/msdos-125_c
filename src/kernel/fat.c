#include "fat.h"
#include "disk.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * FAT12 pack / unpack
 * Three bytes hold two 12-bit entries.  Cluster N at byte N*3/2.
 * If N is even  → lower 12 bits of the 16-bit word.
 * If N is odd   → upper 12 bits.
 * ----------------------------------------------------------------------- */

uint16_t fat_unpack(const uint8_t *fat, uint16_t n)
{
    uint32_t off = (uint32_t)n + (n >> 1);
    uint16_t word = (uint16_t)fat[off] | ((uint16_t)fat[off + 1] << 8);
    if (n & 1)
        return word >> 4;
    else
        return word & 0x0FFF;
}

void fat_pack(uint8_t *fat, uint16_t n, uint16_t val)
{
    uint32_t off = (uint32_t)n + (n >> 1);
    uint16_t word = (uint16_t)fat[off] | ((uint16_t)fat[off + 1] << 8);
    if (n & 1) {
        word = (word & 0x000F) | ((val & 0x0FFF) << 4);
    } else {
        word = (word & 0xF000) | (val & 0x0FFF);
    }
    fat[off]     = (uint8_t)(word & 0xFF);
    fat[off + 1] = (uint8_t)(word >> 8);
}

/* -----------------------------------------------------------------------
 * Chain traversal (FNDCLUS)
 * Uses the cached lstclus/cluspos in the FCB to avoid walking from the
 * start when possible.
 * ----------------------------------------------------------------------- */

uint16_t fat_fndclus(dos_t *dos, dpb_t *dp, fcb_t *fcb,
                     uint16_t count, uint16_t *out_pos)
{
    (void)dos;
    uint8_t  *fat = dp->fat;
    uint16_t  clus = fcb->lstclus;
    uint16_t  pos  = fcb->cluspos;

    if (clus == 0) {
        /* new / empty file */
        clus = fcb->firclus;
        pos  = 0;
    } else if (count < pos) {
        /* need to go backwards — restart from beginning */
        clus = fcb->firclus;
        pos  = 0;
    } else {
        count -= pos;
    }

    while (count > 0 && !fat12_is_eof(clus)) {
        clus = fat_unpack(fat, clus);
        pos++;
        count--;
    }

    *out_pos = pos;
    return clus;
}

/* -----------------------------------------------------------------------
 * Follow chain to last cluster (GETEOF)
 * ----------------------------------------------------------------------- */

uint16_t fat_geteof(const uint8_t *fat, uint16_t bx)
{
    while (true) {
        uint16_t next = fat_unpack(fat, bx);
        if (fat12_is_eof(next))
            return bx;
        bx = next;
    }
}

/* -----------------------------------------------------------------------
 * Release entire chain (RELEASE / RELBLKS)
 * ----------------------------------------------------------------------- */

void fat_relblks(dos_t *dos, dpb_t *dp, uint16_t bx, uint16_t eof_val)
{
    (void)dos;
    uint8_t *fat = dp->fat;
    while (true) {
        uint16_t next = fat_unpack(fat, bx);
        fat_pack(fat, bx, eof_val);
        eof_val = FAT12_FREE;          /* subsequent entries become free */
        if (fat12_is_eof(next) || fat12_is_free(next))
            break;
        if (next >= dp->maxclus)
            break;
        bx = next;
    }
    /* Mark FAT dirty */
    dp->fat[-1] = 1;
}

void fat_release(dos_t *dos, dpb_t *dp, uint16_t bx)
{
    fat_relblks(dos, dp, bx, FAT12_FREE);
}

/* -----------------------------------------------------------------------
 * Allocate clusters (ALLOCATE)
 * Searches the FAT for free clusters using the two-pointer technique from
 * the original (TRYOUT / TRYIN): scan up from last_cluster and down from
 * last_cluster simultaneously.
 * Returns first allocated cluster, 0 on full disk.
 * ----------------------------------------------------------------------- */

uint16_t fat_allocate(dos_t *dos, dpb_t *dp, fcb_t *fcb,
                      uint16_t tail, uint16_t tail_pos, uint16_t count)
{
    (void)dos; (void)tail_pos;
    if (count == 0) return tail;

    uint8_t  *fat        = dp->fat;
    uint16_t  maxclus    = dp->maxclus;
    uint16_t  first_alloc = 0;
    uint16_t  prev        = tail;          /* last cluster in existing chain */
    uint16_t  scan_up     = (tail > 1) ? tail : 2;
    uint16_t  scan_down   = scan_up;
    uint16_t  allocated   = 0;

    while (allocated < count) {
        /* Find a free cluster by scanning both directions */
        uint16_t found = 0;

        /* scan upward */
        uint16_t try = scan_up;
        while (try <= maxclus) {
            if (fat12_is_free(fat_unpack(fat, try))) {
                found = try;
                scan_up = try + 1;
                break;
            }
            try++;
        }
        if (!found && scan_down > 2) {
            /* scan downward */
            try = scan_down - 1;
            while (try >= 2) {
                if (fat12_is_free(fat_unpack(fat, try))) {
                    found = try;
                    scan_down = try;
                    break;
                }
                try--;
            }
        }
        if (!found)
            return 0;          /* disk full */

        fat_pack(fat, found, FAT12_EOF);   /* temporary EOF mark */
        if (prev)
            fat_pack(fat, prev, found);    /* chain prev → found */
        else if (fcb)
            fcb->firclus = found;          /* first cluster of new file */

        if (!first_alloc)
            first_alloc = found;
        prev = found;
        allocated++;
    }

    dp->fat[-1] = 1;    /* FAT dirty */
    return first_alloc;
}

/* -----------------------------------------------------------------------
 * FAT I/O (FATWRT / FIGFAT logic)
 * ----------------------------------------------------------------------- */

int fat_write(dos_t *dos, dpb_t *dp)
{
    if (!dp->fat[-1])
        return 0;                   /* not dirty */

    uint32_t sec    = dp->firfat;
    uint8_t  copies = dp->fatcnt;
    uint8_t  secs   = dp->fatsiz;

    for (uint8_t i = 0; i < copies; i++) {
        uint16_t done = 0;
        int rc = dos->bios->disk_write(dos->bios, dp->drvnum, dos->verflg != 0,
                                       dp->fat, secs, sec, &done);
        if (rc != 0)
            return rc;
        sec += secs;
    }
    dp->fat[-1] = 0;
    return 0;
}

int fat_read(dos_t *dos, dpb_t *dp)
{
    uint32_t sec  = dp->firfat;
    uint8_t  secs = dp->fatsiz;
    uint16_t done = 0;
    int rc = dos->bios->disk_read(dos->bios, dp->drvnum,
                                  dp->fat, secs, sec, &done);
    if (rc != 0)
        return rc;
    dp->fat[-1] = 0;

    /* Let BIOS remap the device number based on FAT media byte */
    if (dos->bios->mapdev) {
        uint8_t newdev = dos->bios->mapdev(dos->bios, dp->drvnum, dp->fat[0]);
        dp->fat[-2] = newdev & 0x3F;
    } else {
        dp->fat[-2] = dp->drvnum & 0x3F;
    }
    return 0;
}
