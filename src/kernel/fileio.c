#include "fileio.h"
#include "fcb.h"
#include "fat.h"
#include "disk.h"
#include "datetime.h"
#include <string.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * GETREC — decode FCB extent/nr into a 32-bit record number
 * ----------------------------------------------------------------------- */

uint32_t fileio_getrec(const fcb_t *fcb)
{
    /* Record = (EXTENT << 7) | NR[6:0]
     * NR holds bits 6:0 of the record number.
     * EXTENT holds bits 22:7 (one EXTENT = 128 records). */
    return ((uint32_t)fcb->extent << 7) | (fcb->nr & 0x7F);
}

/* -----------------------------------------------------------------------
 * SETNREX — encode record position back into FCB
 * ----------------------------------------------------------------------- */

void fileio_setnrex(fcb_t *fcb, uint32_t new_pos, uint16_t reccnt, uint8_t dskerr)
{
    /* Encode new_pos (the next sequential position) into NR and EXTENT.
     * Also update the RR (random record) field with the same value. */
    (void)reccnt; (void)dskerr;
    fcb->nr     = (uint8_t)(new_pos & 0x7F);
    fcb->extent = (uint16_t)(new_pos >> 7);
    fcb->rr[0]  = (uint8_t)(new_pos & 0xFF);
    fcb->rr[1]  = (uint8_t)((new_pos >> 8)  & 0xFF);
    fcb->rr[2]  = (uint8_t)((new_pos >> 16) & 0xFF);
    fcb->rr[3]  = (uint8_t)((new_pos >> 24) & 0xFF);
}

/* -----------------------------------------------------------------------
 * SETUP — prepare dos state for a read/write of 'count' records at 'recpos'
 * Returns false if no transfer needed (e.g., beyond EOF for read).
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t bytpos;
    uint16_t secpos;
    uint16_t bytsecpos;
    uint16_t clusnum;
    uint16_t bytcnt1;
    uint16_t seccnt;
    uint16_t bytcnt2;
    bool     is_device;
    uint8_t  devid;
} setup_result_t;

static bool setup(dos_t *dos, fcb_t *fcb, uint32_t recpos, uint16_t count,
                  bool writing, setup_result_t *r)
{
    uint16_t recsiz = fcb->recsiz ? fcb->recsiz : 128;
    uint8_t  drv    = fcb->drive ? fcb->drive - 1 : dos->curdrv;

    dos->fcb     = fcb;
    dos->thisdrv = drv;
    dos->reccnt  = count;
    dos->recpos  = recpos;
    dos->dskerr  = 0;
    dos->trans   = false;
    dos->nextadd = dos->dmaadd;

    r->devid     = fcb->devid;
    r->is_device = (fcb->devid & FCB_DEVID_DEVICE) != 0;

    if (r->is_device) return true;

    if (disk_fatread(dos) != 0) { dos->dskerr = DOS_NOFILE; return false; }
    dpb_t *dp = dos->thisbp;

    uint64_t bytpos64 = (uint64_t)recpos * recsiz;
    r->bytpos = (uint32_t)bytpos64;

    /* For read: trim to bytes actually available in the file. */
    uint32_t total_bytes = (uint32_t)count * recsiz;
    if (!writing) {
        uint32_t remaining = (fcb->filsiz > r->bytpos) ? fcb->filsiz - r->bytpos : 0;
        if (remaining == 0) { dos->dskerr = DOS_EOF; return false; }
        if (remaining < total_bytes)
            total_bytes = remaining;   /* may be less than one full record */
    }
    r->secpos    = (uint16_t)(r->bytpos / dp->secsiz);
    r->bytsecpos = (uint16_t)(r->bytpos % dp->secsiz);
    r->clusnum   = (uint16_t)(r->secpos >> dp->clusshft);

    /* BREAKDOWN */
    uint16_t first_part = 0;
    uint32_t remain     = total_bytes;
    if (r->bytsecpos) {
        first_part = dp->secsiz - r->bytsecpos;
        if (first_part > (uint16_t)remain) first_part = (uint16_t)remain;
        remain -= first_part;
    }
    r->bytcnt1 = first_part;
    r->seccnt  = (uint16_t)(remain / dp->secsiz);
    r->bytcnt2 = (uint16_t)(remain % dp->secsiz);

    dos->bytpos    = r->bytpos;
    dos->bytcnt1   = r->bytcnt1;
    dos->seccnt    = r->seccnt;
    dos->bytcnt2   = r->bytcnt2;
    dos->bytsecpos = r->bytsecpos;
    dos->curfat    = dp->fat;

    return true;
}

/* -----------------------------------------------------------------------
 * Device I/O helpers
 * ----------------------------------------------------------------------- */

static uint8_t read_device(dos_t *dos, fcb_t *fcb, uint32_t recpos, uint16_t count)
{
    uint8_t devid = fcb->devid & ~FCB_DEVID_DEVICE;
    uint8_t dev   = devid & 0x3F;
    uint16_t recsiz = fcb->recsiz ? fcb->recsiz : 128;
    uint32_t total  = (uint32_t)count * recsiz;
    uint8_t *dst    = dos->dmaadd;

    for (uint32_t i = 0; i < total; i++) {
        int c;
        if (dev == DEV_NUL)       c = 0x1A;
        else if (dev == DEV_AUX)  c = dos->bios->auxin(dos->bios);
        else                      c = dos->bios->in(dos->bios);   /* CON */
        *dst++ = (uint8_t)c;
        if (c == 0x1A) {
            /* EOF */
            fcb->devid &= (uint8_t)~FCB_DEVID_DIRTY;
            break;
        }
    }
    fileio_setnrex(fcb, recpos, count, 0);
    return DOS_OK;
}

static uint8_t write_device(dos_t *dos, fcb_t *fcb, uint32_t recpos, uint16_t count)
{
    uint8_t dev   = fcb->devid & 0x3F & ~(uint8_t)(FCB_DEVID_DEVICE >> 1);
    dev &= 0x3F;
    uint16_t recsiz = fcb->recsiz ? fcb->recsiz : 128;
    uint32_t total  = (uint32_t)count * recsiz;
    const uint8_t *src = dos->dmaadd;

    for (uint32_t i = 0; i < total; i++) {
        uint8_t c = *src++;
        if (c == 0x1A) break;
        if (dev == DEV_NUL)       {}
        else if (dev == DEV_PRN || dev == DEV_LST)
                                  dos->bios->print(dos->bios, c);
        else if (dev == DEV_AUX)  dos->bios->auxout(dos->bios, c);
        else                      dos->bios->out(dos->bios, c);   /* CON */
    }
    fileio_setnrex(fcb, recpos, count, 0);
    return DOS_OK;
}

/* -----------------------------------------------------------------------
 * Core LOAD / STORE
 * ----------------------------------------------------------------------- */

static uint8_t do_load(dos_t *dos, fcb_t *fcb, uint32_t recpos, uint16_t count)
{
    setup_result_t r;
    if (!setup(dos, fcb, recpos, count, false, &r)) {
        fileio_setnrex(fcb, recpos, 0, dos->dskerr);
        return dos->dskerr;
    }
    if (r.is_device) return read_device(dos, fcb, recpos, count);

    uint16_t recsiz = fcb->recsiz ? fcb->recsiz : 128;
    dpb_t   *dp     = dos->thisbp;
    uint8_t *dma    = dos->dmaadd;
    uint8_t  sic    = (uint8_t)(r.secpos & dp->clusmsk);

    /* Find starting cluster */
    uint16_t clpos;
    uint16_t clus = fat_fndclus(dos, dp, fcb, r.clusnum, &clpos);
    if (fat12_is_eof(clus)) {
        dos->dskerr = DOS_EOF;
        fileio_setnrex(fcb, recpos, 0, DOS_EOF);
        return DOS_EOF;
    }
    fcb->lstclus = clus;
    fcb->cluspos = clpos;

    uint32_t bytes_read = 0;
    uint32_t total_bytes = (uint32_t)count * recsiz;

    /* First partial sector */
    if (r.bytcnt1 > 0) {
        uint8_t *buf = disk_bufsec(dos, dp, clus, sic, false);
        if (!buf) goto io_err;
        memcpy(dma, buf + r.bytsecpos, r.bytcnt1);
        dma       += r.bytcnt1;
        bytes_read += r.bytcnt1;
        sic++;
        if (sic > dp->clusmsk) {
            uint16_t next = fat_unpack(dp->fat, clus);
            if (!fat12_is_eof(next)) {
                clus = next; clpos++;
                fcb->lstclus = clus; fcb->cluspos = clpos;
            }
            sic = 0;
        }
    }

    /* Whole sectors */
    for (uint16_t s = 0; s < r.seccnt; s++) {
        uint32_t sec = disk_figrec(dp, clus, sic);
        if (disk_read(dos, dp, dma, 1, sec) != 0) goto io_err;
        dma        += dp->secsiz;
        bytes_read += dp->secsiz;
        sic++;
        if (sic > dp->clusmsk) {
            uint16_t next = fat_unpack(dp->fat, clus);
            if (!fat12_is_eof(next)) {
                clus = next; clpos++;
                fcb->lstclus = clus; fcb->cluspos = clpos;
            }
            sic = 0;
        }
    }

    /* Last partial sector */
    if (r.bytcnt2 > 0) {
        uint8_t *buf = disk_bufsec(dos, dp, clus, sic, false);
        if (!buf) goto io_err;
        memcpy(dma, buf, r.bytcnt2);
        bytes_read += r.bytcnt2;
    }

    /* Pad the last partial record with zeros (matching assembly SETFCB logic).
     * 'dma' now points to the byte after all whole-sector data; r.bytcnt2
     * bytes of partial sector follow, so the fill starts at dma + r.bytcnt2,
     * which is exactly NEXTADD in the original. */
    uint16_t transferred_recs = (uint16_t)(bytes_read / recsiz);
    uint16_t remainder        = (uint16_t)(bytes_read % recsiz);
    if (remainder && transferred_recs < count) {
        memset(dma + r.bytcnt2, 0, recsiz - remainder);
        transferred_recs++;
        dos->dskerr = DOS_PARTIAL;
    }
    if (transferred_recs < count) dos->dskerr = DOS_EOF;

    fileio_setnrex(fcb, recpos + transferred_recs, 0, dos->dskerr);
    return dos->dskerr;

io_err:
    dos->dskerr = DOS_EOF;
    fileio_setnrex(fcb, recpos, 0, DOS_EOF);
    return DOS_EOF;
}

static uint8_t do_store(dos_t *dos, fcb_t *fcb, uint32_t recpos, uint16_t count)
{
    setup_result_t r;
    if (!setup(dos, fcb, recpos, count, true, &r)) {
        fileio_setnrex(fcb, recpos, 0, dos->dskerr);
        return dos->dskerr;
    }
    if (r.is_device) return write_device(dos, fcb, recpos, count);

    uint16_t recsiz = fcb->recsiz ? fcb->recsiz : 128;
    dpb_t   *dp     = dos->thisbp;
    const uint8_t *dma = dos->dmaadd;
    uint8_t  sic    = (uint8_t)(r.secpos & dp->clusmsk);

    /* Mark file dirty */
    fcb->devid &= (uint8_t)~FCB_DEVID_DIRTY;
    dos_date_pack(dos, &fcb->fdate, &fcb->ftime);

    /* Find / allocate starting cluster */
    uint16_t clpos;
    uint16_t clus = fat_fndclus(dos, dp, fcb, r.clusnum, &clpos);

    /* Allocate any needed clusters.
     * last_clus_needed = chain position of the cluster holding the last byte,
     * computed as (last_byte_index / secsiz) >> clusshft, matching the
     * assembly: DIV secsiz → SHR AX, clusshft. */
    uint32_t total_write = (uint32_t)count * recsiz;
    uint32_t last_clus_needed = 0;
    if (total_write > 0) {
        uint32_t last_byte_idx = r.bytpos + total_write - 1;
        last_clus_needed = (last_byte_idx / dp->secsiz) >> dp->clusshft;
    }
    if (fat12_is_eof(clus)) {
        uint16_t tail = fcb->lstclus;
        uint16_t need = (uint16_t)(last_clus_needed - clpos + 1);
        clus = fat_allocate(dos, dp, fcb, tail, clpos, need);
        if (!clus) {
            dos->dskerr = DOS_EOF;
            fileio_setnrex(fcb, recpos, 0, DOS_EOF);
            return DOS_EOF;
        }
        fcb->lstclus = clus;
        fcb->cluspos = r.clusnum;
    }

    uint32_t bytes_written = 0;
    uint32_t total_bytes   = (uint32_t)count * recsiz;

    /* Update valsec: sectors previously written */
    dos->valsec = (uint16_t)((fcb->filsiz + dp->secsiz - 1) / dp->secsiz);

    /* First partial sector */
    if (r.bytcnt1 > 0) {
        uint8_t *buf = disk_bufsec(dos, dp, clus, sic, true);
        if (!buf) goto io_err;
        memcpy(buf + r.bytsecpos, dma, r.bytcnt1);
        dos->dirtybuf = 1;
        dma          += r.bytcnt1;
        bytes_written += r.bytcnt1;
        sic++;
        if (sic > dp->clusmsk) {
            uint16_t next = fat_unpack(dp->fat, clus);
            if (!fat12_is_eof(next)) {
                clus = next; clpos++;
                fcb->lstclus = clus; fcb->cluspos = clpos;
            }
            sic = 0;
        }
    }

    /* Whole sectors */
    for (uint16_t s = 0; s < r.seccnt; s++) {
        uint32_t sec = disk_figrec(dp, clus, sic);
        if (disk_write(dos, dp, dma, 1, sec) != 0) goto io_err;
        dma           += dp->secsiz;
        bytes_written += dp->secsiz;
        sic++;
        if (sic > dp->clusmsk) {
            uint16_t next = fat_unpack(dp->fat, clus);
            if (!fat12_is_eof(next)) {
                clus = next; clpos++;
                fcb->lstclus = clus; fcb->cluspos = clpos;
            }
            sic = 0;
        }
    }

    /* Last partial sector */
    if (r.bytcnt2 > 0) {
        uint8_t *buf = disk_bufsec(dos, dp, clus, sic, true);
        if (!buf) goto io_err;
        memcpy(buf, dma, r.bytcnt2);
        dos->dirtybuf  = 1;
        bytes_written += r.bytcnt2;
    }

    /* Update file size */
    uint32_t new_end = r.bytpos + bytes_written;
    if (new_end > fcb->filsiz) fcb->filsiz = new_end;

    fileio_setnrex(fcb, recpos, count, 0);
    return DOS_OK;

io_err:
    dos->dskerr = DOS_EOF;
    uint16_t done = (uint16_t)(bytes_written / recsiz);
    fileio_setnrex(fcb, recpos, done, DOS_EOF);
    return DOS_EOF;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

uint8_t dos_seqrd(dos_t *dos, fcb_t *fcb)
{
    uint32_t pos = fileio_getrec(fcb);
    uint8_t  rc  = do_load(dos, fcb, pos, 1);
    /* Advance position if at least one record (or partial) was transferred
     * (DOS_OK or DOS_PARTIAL), matching FINSEQ: CX!=0 → pos+1. */
    if (rc == DOS_OK || rc == DOS_PARTIAL)
        fileio_setnrex(fcb, pos + 1, 0, 0);
    return rc;
}

uint8_t dos_seqwrt(dos_t *dos, fcb_t *fcb)
{
    uint32_t pos = fileio_getrec(fcb);
    uint8_t  rc  = do_store(dos, fcb, pos, 1);
    if (rc == DOS_OK || rc == DOS_PARTIAL)
        fileio_setnrex(fcb, pos + 1, 0, 0);
    return rc;
}

uint8_t dos_rndrd(dos_t *dos, fcb_t *fcb)
{
    uint32_t pos = (uint32_t)fcb->rr[0] | ((uint32_t)fcb->rr[1] << 8)
                 | ((uint32_t)fcb->rr[2] << 16) | ((uint32_t)fcb->rr[3] << 24);
    return do_load(dos, fcb, pos, 1);
}

uint8_t dos_rndwrt(dos_t *dos, fcb_t *fcb)
{
    uint32_t pos = (uint32_t)fcb->rr[0] | ((uint32_t)fcb->rr[1] << 8)
                 | ((uint32_t)fcb->rr[2] << 16) | ((uint32_t)fcb->rr[3] << 24);
    return do_store(dos, fcb, pos, 1);
}

uint8_t dos_blkrd(dos_t *dos, fcb_t *fcb, uint16_t count, uint16_t *out_count)
{
    uint32_t pos = (uint32_t)fcb->rr[0] | ((uint32_t)fcb->rr[1] << 8)
                 | ((uint32_t)fcb->rr[2] << 16);
    uint8_t rc = do_load(dos, fcb, pos, count);
    if (out_count) *out_count = dos->reccnt;
    return rc;
}

uint8_t dos_blkwrt(dos_t *dos, fcb_t *fcb, uint16_t count, uint16_t *out_count)
{
    uint32_t pos = (uint32_t)fcb->rr[0] | ((uint32_t)fcb->rr[1] << 8)
                 | ((uint32_t)fcb->rr[2] << 16);
    uint8_t rc = do_store(dos, fcb, pos, count);
    if (out_count) *out_count = dos->reccnt;
    return rc;
}

uint8_t dos_filesize(dos_t *dos, fcb_t *fcb)
{
    uint16_t bx, si; int16_t bh;
    if (fcb_getfile(dos, fcb, &bx, &si, &bh) != 0) return DOS_ERR;

    uint16_t recsiz = fcb->recsiz ? fcb->recsiz : 128;
    uint32_t filsiz = 0;

    if (bh >= 0) {
        dirent_t *de = (dirent_t*)(dos->dirbuf + bx);
        filsiz = de->filsiz;
    }

    uint32_t recs = (filsiz + recsiz - 1) / recsiz;
    /* Store 24/32 bit result in rr field (offset 33) */
    uint8_t *rr = ((uint8_t*)fcb) + 33;
    rr[0] = (uint8_t)(recs & 0xFF);
    rr[1] = (uint8_t)((recs >> 8) & 0xFF);
    rr[2] = (uint8_t)((recs >> 16) & 0xFF);
    if (recsiz < 64) rr[3] = (uint8_t)((recs >> 24) & 0xFF);
    return DOS_OK;
}

uint8_t dos_setrndrec(dos_t *dos, fcb_t *fcb)
{
    uint32_t pos = fileio_getrec(fcb);
    uint8_t *rr  = ((uint8_t*)fcb) + 33;
    rr[0] = (uint8_t)(pos & 0xFF);
    rr[1] = (uint8_t)((pos >> 8) & 0xFF);
    rr[2] = (uint8_t)((pos >> 16) & 0xFF);
    if (fcb->recsiz < 64) rr[3] = (uint8_t)((pos >> 24) & 0xFF);
    (void)dos;
    return DOS_OK;
}
