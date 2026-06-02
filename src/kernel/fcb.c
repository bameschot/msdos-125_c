#include "fcb.h"
#include "disk.h"
#include "fat.h"
#include "datetime.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Device names (IONAME) — 4 chars each, no null terminator.
 * Order matches NUMDEV (5 for non-IBM).
 * ----------------------------------------------------------------------- */

static const char devnames[NUMDEV][4] = {
    "PRN ", "LST ", "NUL ", "AUX ", "CON "
};

/* devid in FCB: 0x80 | device_index (inverted from table) */
static int8_t devid_from_idx(int i) {
    return (int8_t)(0x80 | (int8_t)(NUMDEV - 1 - i));
}

int fcb_devname(dos_t *dos)
{
    for (int i = 0; i < NUMDEV; i++) {
        if (memcmp(dos->name1, devnames[i], 4) == 0) {
            /* Remaining 7 bytes must be spaces */
            bool ok = true;
            for (int j = 4; j < 11; j++) {
                if (dos->name1[j] != ' ') { ok = false; break; }
            }
            if (ok) return i;
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * MOVNAME — parse FCB, fill dos->name1, dos->attrib, dos->thisdrv
 * ----------------------------------------------------------------------- */

int fcb_movname(dos_t *dos, fcb_t *fcb)
{
    uint8_t *p   = (uint8_t*)fcb;
    uint8_t  drv = p[0];
    uint8_t  att = 0;

    dos->extfcb   = false;
    dos->creating = false;
    dos->delall   = 0xE5;   /* normal delete marker */

    if (drv == 0xFF) {
        /* extended FCB */
        dos->extfcb = true;
        att = p[6];
        drv = p[7];
        p  += 7;
    }
    dos->attrib = att;

    /* Drive: 0 = current */
    if (drv == 0)
        drv = dos->curdrv;
    else
        drv--;

    if (drv >= dos->num_drives) return -1;
    dos->thisdrv = drv;

    /* Copy 11-char filename, upper-case */
    const uint8_t *name = p + 1;
    if (name[0] == ' ') return -1;
    for (int i = 0; i < 11; i++)
        dos->name1[i] = (uint8_t)toupper(name[i]);

    return 0;
}

/* -----------------------------------------------------------------------
 * Helper: scan directory entry starting at block 'block', offset 'off'.
 * Returns dirent_t* pointing into dos->dirbuf, or NULL if done.
 * ----------------------------------------------------------------------- */

static dirent_t *get_dir_entry(dos_t *dos, dpb_t *dp,
                               uint16_t *block, uint16_t *off)
{
    uint16_t ents_per_sec = dp->secsiz / DIRENT_SIZE;
    uint16_t total_blocks = (dp->maxent + ents_per_sec - 1) / ents_per_sec;

    while (*block < total_blocks) {
        if (disk_dirread(dos, dp, *block) != 0) return NULL;
        if (*off < dp->secsiz) {
            dirent_t *de = (dirent_t *)(dos->dirbuf + *off);
            return de;
        }
        (*block)++;
        *off = 0;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * FINDNAME — search directory for dos->name1
 * On success: dos->dirbuf has the entry; *out_bx = byte offset into dirbuf;
 * *out_si = offset of firclus; *out_bh is device index (negative) or drive.
 * Sets dos->lastent, dos->entfree.
 * ----------------------------------------------------------------------- */

static int findname_impl(dos_t *dos, dpb_t *dp,
                         uint16_t *out_bx, uint16_t *out_si,
                         int16_t *out_bh)
{
    uint16_t ents_per_sec = dp->secsiz / DIRENT_SIZE;

    /* Start from dos->lastent so SRCHNXT can resume after the last hit.
     * 0xFFFF is the sentinel meaning "start from the beginning". */
    uint16_t entry_num = (dos->lastent == 0xFFFF) ? 0 : dos->lastent;

    /* Root directory has a fixed entry count; subdirectories do not —
     * we stop only when DIRENT_END or disk_dirread signals end-of-chain. */
    bool     is_root  = (dos->curdir_clus[dos->thisdrv] == 0);
    uint16_t max_ent  = is_root ? dp->maxent : 0xFFFFU;

    dos->entfree = 0xFFFF;

    for (; entry_num < max_ent; entry_num++) {
        uint16_t block = entry_num / ents_per_sec;
        uint16_t off   = (entry_num % ents_per_sec) * DIRENT_SIZE;

        /* disk_dirread returns -1 at end-of-chain for subdirs */
        if (disk_dirread(dos, dp, block) != 0) break;
        dirent_t *de = (dirent_t *)(dos->dirbuf + off);

        uint8_t first = de->name[0];

        if (first == DIRENT_END) {
            if (dos->entfree == 0xFFFF) dos->entfree = entry_num;
            break;
        }

        if (first == DIRENT_FREE) {
            if (dos->entfree == 0xFFFF) dos->entfree = entry_num;
        } else {
            /* Compare name with wildcards */
            bool match = true;
            for (int i = 0; i < 11; i++) {
                uint8_t expected = dos->name1[i];
                uint8_t actual   = ((uint8_t*)de)[i];
                if (expected != '?' && expected != actual) {
                    match = false; break;
                }
            }
            if (match) {
                /* Attribute filter */
                uint8_t att    = dos->attrib;
                uint8_t de_att = de->attrib;
                if (!dos->creating) {
                    if ((~att & de_att & (ATTR_HIDDEN | ATTR_SYSTEM)) != 0)
                        match = false;
                }
            }
            if (match) {
                dos->lastent = entry_num;
                *out_bx = off;
                *out_si = (uint16_t)((uint8_t*)&de->firclus - dos->dirbuf);
                *out_bh = (int16_t)dos->thisdrv;
                return 0;
            }
        }
    }
    return -1;   /* not found */
}

int fcb_getname(dos_t *dos, fcb_t *fcb,
                uint16_t *out_bx, uint16_t *out_si, int16_t *out_bh)
{
    if (fcb_movname(dos, fcb) != 0) return -1;

    /* Check for named device */
    int dev = fcb_devname(dos);
    if (dev >= 0) {
        *out_bh = devid_from_idx(dev);
        return 0;
    }

    /* Ensure FAT is current */
    dos->thisdrv = dos->thisdrv;
    if (disk_fatread(dos) != 0) return -1;
    dpb_t *dp = dos->thisbp;

    return findname_impl(dos, dp, out_bx, out_si, out_bh);
}

int fcb_getfile(dos_t *dos, fcb_t *fcb,
                uint16_t *out_bx, uint16_t *out_si, int16_t *out_bh)
{
    return fcb_getname(dos, fcb, out_bx, out_si, out_bh);
}

/* -----------------------------------------------------------------------
 * OPEN (system call 15)
 * ----------------------------------------------------------------------- */

static void open_fill_fcb(dos_t *dos, fcb_t *fcb, dirent_t *de)
{
    fcb->drive  = dos->thisdrv + 1;
    fcb->extent = 0;
    fcb->recsiz = 128;
    fcb->firclus = de->firclus;
    fcb->lstclus = 0;
    fcb->cluspos = 0;
    fcb->filsiz  = de->filsiz;
    fcb->fdate   = de->fdate;
    fcb->ftime   = de->ftime;
    fcb->devid   = dos->thisbp->drvnum | FCB_DEVID_DIRTY;  /* not dirty but open */
    fcb->nr      = 0;
    memset(fcb->rr, 0, 4);
}

uint8_t dos_open(dos_t *dos, fcb_t *fcb)
{
    uint16_t bx, si; int16_t bh;
    if (fcb_getfile(dos, fcb, &bx, &si, &bh) != 0) return DOS_ERR;

    if (bh < 0) {
        /* Named device */
        fcb->extent = 0;
        fcb->recsiz = 128;
        fcb->filsiz = 0;
        dos_date_pack(dos, &fcb->fdate, &fcb->ftime);
        fcb->devid   = (uint8_t)(bh) | FCB_DEVID_DEVICE;
        return DOS_OK;
    }

    dirent_t *de = (dirent_t *)(dos->dirbuf + bx);
    open_fill_fcb(dos, fcb, de);
    return DOS_OK;
}

/* -----------------------------------------------------------------------
 * CLOSE (system call 16)
 * ----------------------------------------------------------------------- */

uint8_t dos_close(dos_t *dos, fcb_t *fcb)
{
    /* Only close dirty files (bit 6 clear = dirty in this code) */
    if (fcb->devid & (FCB_DEVID_DEVICE | FCB_DEVID_DIRTY))
        return DOS_OK;    /* device or not written — nothing to do */

    /* Flush buffer if on same drive */
    if (dos->bufdrvno == (fcb->drive - 1) && dos->dirtybuf) {
        disk_buf_flush(dos);
    }

    /* Re-find directory entry */
    uint16_t bx, si; int16_t bh;
    if (fcb_getfile(dos, fcb, &bx, &si, &bh) != 0) {
        fat_write(dos, dos->thisbp);
        return DOS_ERR;
    }

    dirent_t *de = (dirent_t *)(dos->dirbuf + bx);
    de->firclus  = fcb->firclus;
    de->filsiz   = fcb->filsiz;
    de->fdate    = fcb->fdate;
    de->ftime    = fcb->ftime;

    dos->dirtydir = 1;
    disk_chkdirwrite(dos, dos->thisbp);
    fat_write(dos, dos->thisbp);
    return DOS_OK;
}

/* -----------------------------------------------------------------------
 * CREATE (system call 22)
 * ----------------------------------------------------------------------- */

uint8_t dos_create(dos_t *dos, fcb_t *fcb)
{
    if (fcb_movname(dos, fcb) != 0) return DOS_ERR;

    /* Wildcards not allowed in create */
    for (int i = 0; i < 11; i++)
        if (dos->name1[i] == '?') return DOS_ERR;

    dos->creating = true;

    if (disk_fatread(dos) != 0) return DOS_ERR;
    dpb_t *dp = dos->thisbp;

    uint16_t bx, si; int16_t bh;
    int found = findname_impl(dos, dp, &bx, &si, &bh);

    if (found == 0 && bh >= 0) {
        /* File exists — free its clusters first */
        dirent_t *de = (dirent_t *)(dos->dirbuf + bx);
        if (de->firclus && de->firclus < dp->maxclus) {
            fat_release(dos, dp, de->firclus);
            fat_write(dos, dp);
        }
    } else {
        /* New entry — find free slot */
        if (dos->entfree == 0xFFFF) return DOS_ERR;   /* directory full */
        uint16_t ents_per_sec = dp->secsiz / DIRENT_SIZE;
        uint16_t block = dos->entfree / ents_per_sec;
        uint16_t off   = (dos->entfree % ents_per_sec) * DIRENT_SIZE;
        if (disk_dirread(dos, dp, block) != 0) return DOS_ERR;
        bx = off;
    }

    dirent_t *de = (dirent_t *)(dos->dirbuf + bx);
    memcpy(de->name, dos->name1, 8);
    memcpy(de->ext,  dos->name1 + 8, 3);
    de->attrib  = dos->attrib;
    de->firclus = 0;
    de->filsiz  = 0;
    dos_date_pack(dos, &de->fdate, &de->ftime);
    memset(de->_res, 0, sizeof(de->_res));

    dos->dirtydir = 1;
    disk_dirwrite(dos, dp);

    /* Open the newly created entry */
    open_fill_fcb(dos, fcb, de);
    fcb->devid = dp->drvnum;   /* not dirty, not device */
    return DOS_OK;
}

/* -----------------------------------------------------------------------
 * DELETE (system call 19)
 * ----------------------------------------------------------------------- */

uint8_t dos_delete(dos_t *dos, fcb_t *fcb)
{
    if (fcb_movname(dos, fcb) != 0) return DOS_ERR;

    /* Check for *.* (DEL *.*) */
    bool all = true;
    for (int i = 0; i < 11; i++)
        if (dos->name1[i] != '?') { all = false; break; }
    if (all) dos->delall = 0;    /* use 0x00 instead of 0xE5 for DEL *.* */

    if (disk_fatread(dos) != 0) return DOS_ERR;
    dpb_t *dp = dos->thisbp;

    uint16_t bx, si; int16_t bh;
    int found = findname_impl(dos, dp, &bx, &si, &bh);
    if (found != 0) return DOS_ERR;
    if (bh < 0) return DOS_ERR;   /* can't delete device */

    bool deleted_any = false;
    while (found == 0) {
        dirent_t *de = (dirent_t *)(dos->dirbuf + bx);
        de->name[0]  = dos->delall ? dos->delall : 0xE5;
        dos->dirtydir = 1;

        if (de->firclus && de->firclus < dp->maxclus)
            fat_release(dos, dp, de->firclus);

        deleted_any = true;

        /* Continue searching for more matches (wildcard delete) */
        dos->lastent++;
        found = findname_impl(dos, dp, &bx, &si, &bh);
    }

    disk_chkdirwrite(dos, dp);
    fat_write(dos, dp);
    return deleted_any ? DOS_OK : DOS_ERR;
}

/* -----------------------------------------------------------------------
 * RENAME (system call 23)
 * ----------------------------------------------------------------------- */

uint8_t dos_rename(dos_t *dos, fcb_t *fcb)
{
    if (fcb_movname(dos, fcb) != 0) return DOS_ERR;

    /* Second name is at FCB+17 (relative to start of FCB) */
    uint8_t *p = (uint8_t*)fcb;
    if (p[0] == 0xFF) p += 7;

    uint8_t name2[11];
    for (int i = 0; i < 11; i++)
        name2[i] = (uint8_t)toupper(p[17 + i]);

    if (disk_fatread(dos) != 0) return DOS_ERR;
    dpb_t *dp = dos->thisbp;

    uint16_t bx, si; int16_t bh;
    if (findname_impl(dos, dp, &bx, &si, &bh) != 0) return DOS_ERR;
    if (bh < 0) return DOS_ERR;

    dirent_t *de = (dirent_t *)(dos->dirbuf + bx);

    /* Build new name applying wildcards */
    uint8_t newname[11];
    for (int i = 0; i < 11; i++)
        newname[i] = (name2[i] == '?') ? ((uint8_t*)de)[i] : name2[i];

    /* Check for duplicate */
    memcpy(dos->name1, newname, 11);
    uint16_t bx2, si2; int16_t bh2;
    dos->lastent = 0xFFFF;
    if (findname_impl(dos, dp, &bx2, &si2, &bh2) == 0) return DOS_ERR;

    /* Restore match and apply new name */
    memcpy(dos->name1, ((uint8_t*)de), 11);  /* re-find original */
    memcpy(de->name, newname, 8);
    memcpy(de->ext,  newname + 8, 3);
    dos->dirtydir = 1;
    disk_chkdirwrite(dos, dp);
    return DOS_OK;
}

/* -----------------------------------------------------------------------
 * SEARCH FIRST / SEARCH NEXT (system calls 17, 18)
 * ----------------------------------------------------------------------- */

uint8_t dos_srchfrst(dos_t *dos, fcb_t *fcb)
{
    uint16_t bx, si; int16_t bh;
    dos->lastent = 0xFFFF;

    if (fcb_getfile(dos, fcb, &bx, &si, &bh) != 0) {
        /* Not found — store -1 in fildirent field */
        *(uint16_t*)((uint8_t*)fcb + 16) = 0xFFFF;
        return DOS_ERR;
    }

    /* Store search state in FCB */
    *(uint16_t*)((uint8_t*)fcb + 16) = dos->lastent;   /* fildirent */
    *(uint16_t*)((uint8_t*)fcb + 14) = (uint16_t)(uintptr_t)dos->thisbp; /* drvbp */

    /* Copy directory entry to DMA buffer */
    dirent_t *de  = (dirent_t *)(dos->dirbuf + bx);
    uint8_t  *dma = dos->dmaadd;
    if (dos->extfcb) {
        *dma++ = 0xFF;
        memset(dma, 0, 5); dma += 5;
        *dma++ = dos->attrib;
    }
    *dma++ = dos->thisdrv + 1;
    memcpy(dma, de, DIRENT_SIZE);
    return DOS_OK;
}

uint8_t dos_srchnxt(dos_t *dos, fcb_t *fcb)
{
    if (fcb_movname(dos, fcb) != 0) return DOS_ERR;

    uint16_t last = *(uint16_t*)((uint8_t*)fcb + 16);
    if (last == 0xFFFF) return DOS_ERR;

    dos->thisbp  = (dpb_t*)(uintptr_t)*(uint16_t*)((uint8_t*)fcb + 14);
    dos->thisdrv = dos->thisbp->drvnum;
    dos->lastent = last;

    uint16_t bx, si; int16_t bh;
    dos->lastent++;   /* advance to next */
    if (findname_impl(dos, dos->thisbp, &bx, &si, &bh) != 0) {
        *(uint16_t*)((uint8_t*)fcb + 16) = 0xFFFF;
        return DOS_ERR;
    }

    *(uint16_t*)((uint8_t*)fcb + 16) = dos->lastent;

    dirent_t *de  = (dirent_t *)(dos->dirbuf + bx);
    uint8_t  *dma = dos->dmaadd;
    *dma++ = dos->thisdrv + 1;
    memcpy(dma, de, DIRENT_SIZE);
    return DOS_OK;
}

/* -----------------------------------------------------------------------
 * MAKEFCB (system call 41H) — parse "D:FILENAME.EXT" into FCB
 * ----------------------------------------------------------------------- */

static bool is_delimiter(char c)
{
    return c == '.' || c == '"' || c == '/' || c == '[' || c == ']'
        || c == ':' || c == '+' || c == '=' || c == ';' || c == ','
        || c == '\t' || c == ' ' || c < 32;
}

static int getlet(const char **pp)
{
    unsigned char c = (unsigned char)**pp;
    (*pp)++;
    if (c == 0) return 0;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (is_delimiter((char)c)) return 0;
    return c;
}

uint8_t dos_makefcb(dos_t *dos, const char **src, fcb_t *fcb, uint8_t flags)
{
    uint8_t *out = (uint8_t*)fcb;

    /* Default drive */
    if (!(flags & 0x02))
        out[0] = 0;

    /* Skip separators if requested */
    if (flags & 0x01) {
        while (**src && is_delimiter(**src) && **src != ':') (*src)++;
        if (is_delimiter(**src)) (*src)++;
        while (**src == ' ' || **src == '\t') (*src)++;
    } else {
        while (**src == ' ' || **src == '\t') (*src)++;
    }

    /* Check for drive letter */
    const char *probe = *src;
    int c = getlet(&probe);
    if (c && *probe == ':') {
        /* Drive specifier */
        if (c >= 'A' && c <= 'Z') out[0] = (uint8_t)(c - '@');
        else out[0] = 0xFF;  /* bad drive */
        *src = probe + 1;
    }

    /* Name (up to 8 chars) */
    uint8_t *name = out + 1;
    uint8_t  wild = 0;
    int      n    = 0;
    if (!(flags & 0x04)) memset(name, ' ', 8);
    while (n < 8) {
        c = getlet(src);
        if (!c) { (*src)--; break; }
        if (c == '*') { memset(name + n, '?', 8 - n); wild = 1; break; }
        name[n++] = (uint8_t)c;
        if (c == '?') wild = 1;
    }

    /* Extension (up to 3 chars) */
    uint8_t *ext = out + 9;
    if (!(flags & 0x08)) memset(ext, ' ', 3);
    if (**src == '.') {
        (*src)++;
        n = 0;
        while (n < 3) {
            c = getlet(src);
            if (!c) { (*src)--; break; }
            if (c == '*') { memset(ext + n, '?', 3 - n); wild = 1; break; }
            ext[n++] = (uint8_t)c;
            if (c == '?') wild = 1;
        }
    }

    /* Zero the rest of FCB */
    memset(out + 12, 0, 4);

    (void)dos;
    return wild ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Utility
 * ----------------------------------------------------------------------- */

void dos_name_to_str(const uint8_t name11[11], char out[13])
{
    int i = 0, j = 0;
    for (; i < 8 && name11[i] != ' '; i++) out[j++] = name11[i];
    if (name11[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && name11[i] != ' '; i++) out[j++] = name11[i];
    }
    out[j] = '\0';
}

void str_to_dos_name(const char *str, uint8_t name11[11])
{
    memset(name11, ' ', 11);
    int i = 0;
    while (*str && *str != '.' && i < 8)
        name11[i++] = (uint8_t)toupper((unsigned char)*str++);
    if (*str == '.') str++;
    i = 8;
    while (*str && i < 11)
        name11[i++] = (uint8_t)toupper((unsigned char)*str++);
}
