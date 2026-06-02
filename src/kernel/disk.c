#include "disk.h"
#include "fat.h"
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * HARDERR — invoke the fatal error handler.
 * In our C port, this calls dos->fatal_handler().  If it returns RETRY
 * the caller should retry; IGNORE = continue; ABORT = terminate.
 * ----------------------------------------------------------------------- */

static int harderr(dos_t *dos, dpb_t *dp, uint32_t sector, bool write,
                   int err_code)
{
    uint8_t area = 0;
    if (sector >= dp->firrec)       area = 3;   /* data */
    else if (sector >= dp->firdir)  area = 2;   /* directory */
    else if (sector >= dp->firfat)  area = 1;   /* FAT */
    uint8_t info = (uint8_t)((area << 1) | (write ? 1 : 0));

    if (dos->fatal_handler)
        return dos->fatal_handler(dos, dp->drvnum, info);
    /* Default: ignore */
    (void)err_code;
    return ERRACT_IGNORE;
}

/* -----------------------------------------------------------------------
 * disk_read / disk_write — wrappers around BIOS with retry on error
 * ----------------------------------------------------------------------- */

int disk_read(dos_t *dos, dpb_t *dp, uint8_t *buf,
              uint16_t count, uint32_t sector)
{
retry:;
    uint16_t done = 0;
    int rc = dos->bios->disk_read(dos->bios, dp->drvnum,
                                  buf, count, sector, &done);
    if (rc == 0) return 0;
    int action = harderr(dos, dp, sector, false, rc);
    if (action == ERRACT_RETRY) goto retry;
    return rc;
}

int disk_write(dos_t *dos, dpb_t *dp, const uint8_t *buf,
               uint16_t count, uint32_t sector)
{
retry:;
    uint16_t done = 0;
    int rc = dos->bios->disk_write(dos->bios, dp->drvnum, dos->verflg != 0,
                                   buf, count, sector, &done);
    if (rc == 0) return 0;
    int action = harderr(dos, dp, sector, true, rc);
    if (action == ERRACT_RETRY) goto retry;
    return rc;
}

/* -----------------------------------------------------------------------
 * disk_figrec — cluster + sector-within-cluster → absolute sector
 * ----------------------------------------------------------------------- */

uint32_t disk_figrec(dpb_t *dp, uint16_t cluster, uint8_t sec_in_clus)
{
    return (uint32_t)(cluster - 2) * (dp->clusmsk + 1)
           + sec_in_clus + dp->firrec;
}

/* -----------------------------------------------------------------------
 * disk_getbp / disk_fatread
 * ----------------------------------------------------------------------- */

dpb_t *disk_getbp(dos_t *dos, uint8_t devnum)
{
    uint8_t clean = devnum & 0x3F;
    if (clean >= dos->num_io) return NULL;
    return &dos->drvtab[clean];
}

int disk_fatread(dos_t *dos)
{
    uint8_t drv = dos->thisdrv;
    if (drv >= dos->num_drives) return -1;

    dpb_t *dp = &dos->drvtab[drv];

    /* Check if disk may have changed */
    int changed = 1;
    if (dos->bios->disk_change)
        changed = dos->bios->disk_change(dos->bios, dp->drvnum);

    if (changed != 0) {
        /* Invalidate buffers */
        if (dos->bufdrvno == dp->drvnum) {
            dos->bufsecno  = 0xFFFFFFFF;
            dos->bufdrvno  = 0xFF;
            dos->dirtybuf  = 0;
        }
        dos->dirbufid = 0xFFFFFFFF;

        int rc = fat_read(dos, dp);
        if (rc != 0) return rc;
    }

    dos->thisbp  = dp;
    dos->curfat  = dp->fat;
    return 0;
}

/* -----------------------------------------------------------------------
 * Directory buffer
 * ----------------------------------------------------------------------- */

int disk_dirread(dos_t *dos, dpb_t *dp, uint16_t block)
{
    /* Flush if dirty */
    int rc = disk_chkdirwrite(dos, dp);
    if (rc != 0) return rc;

    uint32_t id = ((uint32_t)dp->drvnum << 24) | block;
    if (dos->dirbufid == id) return 0;

    uint32_t sec = dp->firdir + block;
    rc = disk_read(dos, dp, dos->dirbuf, 1, sec);
    if (rc == 0) dos->dirbufid = id;
    return rc;
}

int disk_dirwrite(dos_t *dos, dpb_t *dp)
{
    uint32_t block = dos->dirbufid & 0x00FFFFFF;
    uint32_t sec   = dp->firdir + block;
    return disk_write(dos, dp, dos->dirbuf, 1, sec);
}

int disk_chkdirwrite(dos_t *dos, dpb_t *dp)
{
    if (!dos->dirtydir) return 0;
    int rc = disk_dirwrite(dos, dp);
    dos->dirtydir = 0;
    return rc;
}

/* -----------------------------------------------------------------------
 * Single-sector data buffer (BUFSEC)
 * ----------------------------------------------------------------------- */

int disk_buf_flush(dos_t *dos)
{
    if (!dos->dirtybuf) return 0;
    dpb_t *dp = dos->bufdrvbp;
    int rc = disk_write(dos, dp, dos->buffer, 1, dos->bufsecno);
    dos->dirtybuf = 0;
    return rc;
}

uint8_t *disk_bufsec(dos_t *dos, dpb_t *dp,
                     uint16_t cluster, uint8_t sec_in_clus,
                     bool write_mode)
{
    uint32_t phys = disk_figrec(dp, cluster, sec_in_clus);

    if (phys == dos->bufsecno && dp->drvnum == dos->bufdrvno)
        return dos->buffer;          /* already in buffer */

    /* Flush dirty buffer */
    if (dos->dirtybuf) {
        int rc = disk_buf_flush(dos);
        if (rc != 0) return NULL;
    }

    /* Pre-read unless this is a full-sector overwrite of new space */
    bool need_read = !write_mode || (dos->secpos <= dos->valsec);
    if (need_read) {
        dos->bufsecno  = 0xFFFFFFFF;
        dos->bufdrvno  = 0xFF;
        int rc = disk_read(dos, dp, dos->buffer, 1, phys);
        if (rc != 0) return NULL;
    }

    dos->bufsecno  = phys;
    dos->bufdrvno  = dp->drvnum;
    dos->bufdrvbp  = dp;
    return dos->buffer;
}

/* -----------------------------------------------------------------------
 * disk_reset — write all dirty FATs and buffer (DSKRESET / WRTFATS)
 * ----------------------------------------------------------------------- */

int disk_reset(dos_t *dos)
{
    int rc = disk_buf_flush(dos);
    for (uint8_t i = 0; i < dos->num_io; i++) {
        int r2 = fat_write(dos, &dos->drvtab[i]);
        if (r2 && !rc) rc = r2;
    }
    return rc;
}
