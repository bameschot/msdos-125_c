#include "kernel.h"
#include "fat.h"
#include "disk.h"
#include "fcb.h"
#include "fileio.h"
#include "chardev.h"
#include "datetime.h"
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Drive configuration from BIOS
 * ----------------------------------------------------------------------- */

/* Drive Parameter Table as supplied by BIOS during init */
typedef struct {
    uint8_t  drvnum;        /* physical unit number */
    uint16_t secsiz;
    uint8_t  secs_per_clus; /* 1, 2, 4, 8, ... */
    uint16_t reserved_secs; /* FIRFAT */
    uint8_t  fat_copies;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t  media_byte;    /* ignored in FAT12 (we derive from FAT[0]) */
    uint8_t  fat_size;      /* sectors per FAT */
} bios_dpt_t;

static void build_dpb(dos_t *dos, dpb_t *dp, uint8_t devnum,
                      const bios_dpt_t *dpt)
{
    dp->devnum   = devnum;
    dp->drvnum   = dpt->drvnum;
    dp->secsiz   = dpt->secsiz;
    dp->clusmsk  = (uint8_t)(dpt->secs_per_clus - 1);

    /* clusshft = log2(secs_per_clus) */
    uint8_t sft = 0;
    uint8_t v   = dpt->secs_per_clus;
    while (v > 1) { v >>= 1; sft++; }
    dp->clusshft = sft;

    dp->firfat   = dpt->reserved_secs;
    dp->fatcnt   = dpt->fat_copies;
    dp->maxent   = dpt->root_entries;
    dp->fatsiz   = dpt->fat_size;

    /* Directory starts after all FAT copies */
    dp->firdir   = (uint16_t)(dp->firfat + dp->fatcnt * dp->fatsiz);

    /* Data area starts after directory */
    uint16_t dir_secs = (dp->maxent * DIRENT_SIZE + dp->secsiz - 1) / dp->secsiz;
    dp->firrec   = (uint16_t)(dp->firdir + dir_secs);

    /* Total clusters */
    uint16_t data_secs = (uint16_t)(dpt->total_sectors - dp->firrec);
    dp->maxclus  = (uint16_t)(data_secs / (dp->clusmsk + 1) + 1);

    /* Point FAT at pool (skip 2-byte header) */
    fat_buf_t *fb = &dos->fat_pool[devnum];
    fb->dev_dirty = devnum & 0x3F;
    fb->fat_dirty = 0;
    dp->fat       = fb->data;
    /* Mark FAT dirty so it gets read on first access */
    dp->fat[-1]   = 0;   /* not dirty yet */
    dp->fat[-2]   = devnum & 0x3F;
}

/* -----------------------------------------------------------------------
 * dos_init
 * ----------------------------------------------------------------------- */

/* Simplified init: we ask BIOS for drive config via get_drive_config.
 * Config is an array of bios_dpt_t. */
int dos_init(dos_t *dos, bios_t *bios)
{
    memset(dos, 0, sizeof(*dos));
    dos->bios     = bios;
    dos->bufsecno = 0xFFFFFFFF;
    dos->bufdrvno = 0xFF;
    dos->dirbufid = 0xFFFFFFFF;
    dos->dmaadd   = dos->psp + 0x80;   /* default DMA = PSP:0080 */
    dos->delall   = 0xE5;
    dos->daycnt   = 0xFFFF;

    bios_dpt_t dpts[MAX_DRIVES];
    int n = 0;
    if (bios->get_drive_config)
        n = bios->get_drive_config(bios, dpts, MAX_DRIVES);

    if (n <= 0) {
        fprintf(stderr, "DOS: no drives configured\n");
        return -1;
    }

    dos->num_io     = (uint8_t)n;
    dos->num_drives = (uint8_t)n;

    for (int i = 0; i < n; i++)
        build_dpb(dos, &dos->drvtab[i], (uint8_t)i, &dpts[i]);

    /* Read FAT for drive 0 to validate disk */
    dos->thisdrv = 0;
    int rc = disk_fatread(dos);
    if (rc != 0) {
        fprintf(stderr, "DOS: could not read FAT for drive A:\n");
        return rc;
    }

    return 0;
}

dpb_t *dos_getdpb(dos_t *dos, uint8_t drv)
{
    if (drv >= dos->num_drives) return NULL;
    dos->thisdrv = drv;
    if (disk_fatread(dos) != 0) return NULL;
    return dos->thisbp;
}

void dos_setdma(dos_t *dos, uint8_t *addr)
{
    dos->dmaadd = addr;
}

/* -----------------------------------------------------------------------
 * dos_call — INT 21H dispatcher
 * ----------------------------------------------------------------------- */

uint8_t dos_call(dos_t *dos, uint8_t fn, dos_regs_t *r)
{
    uint8_t al = (uint8_t)(r->ax & 0xFF);
    uint8_t dl = (uint8_t)(r->dx & 0xFF);
    uint8_t dh = (uint8_t)(r->dx >> 8);
    uint8_t ch = (uint8_t)(r->cx >> 8);
    uint8_t cl = (uint8_t)(r->cx & 0xFF);
    fcb_t  *fcb = (fcb_t*)(uintptr_t)r->dx;

    switch (fn) {
    case 0x00:   /* ABORT */
        disk_reset(dos);
        return 0;

    case 0x01:   /* CONIN */
        al = dos_conin(dos);
        break;

    case 0x02:   /* CONOUT */
        dos_conout(dos, dl);
        break;

    case 0x03:   /* READER */
        al = dos_reader(dos);
        break;

    case 0x04:   /* PUNCH */
        dos_punch(dos, dl);
        break;

    case 0x05:   /* LIST */
        dos_list(dos, dl);
        break;

    case 0x06:   /* RAWIO */
        al = dos_rawio(dos, dl);
        break;

    case 0x07:   /* RAWINP */
        al = dos_rawinp(dos);
        break;

    case 0x08:   /* IN (no echo) */
        al = dos_in(dos);
        break;

    case 0x09:   /* PRTBUF */
        dos_prtbuf(dos, (const char*)(uintptr_t)r->dx);
        break;

    case 0x0A:   /* BUFIN */
        dos_bufin(dos, (uint8_t*)(uintptr_t)r->dx);
        break;

    case 0x0B:   /* CONSTAT */
        al = dos_constat(dos);
        break;

    case 0x0C:   /* FLUSHKB */
        dos_flushkb(dos);
        /* Re-execute the indicated function */
        if (al && al <= 0x0B) {
            dos_regs_t r2 = *r;
            r2.ax = al;
            al = dos_call(dos, al, &r2);
        }
        break;

    case 0x0D:   /* DSKRESET */
        disk_reset(dos);
        al = 0;
        break;

    case 0x0E:   /* SELDSK */
        if (dl < dos->num_drives) dos->curdrv = dl;
        al = dos->num_drives;
        break;

    case 0x0F:   /* OPEN */
        al = dos_open(dos, fcb);
        break;

    case 0x10:   /* CLOSE */
        al = dos_close(dos, fcb);
        break;

    case 0x11:   /* SRCHFRST */
        al = dos_srchfrst(dos, fcb);
        break;

    case 0x12:   /* SRCHNXT */
        al = dos_srchnxt(dos, fcb);
        break;

    case 0x13:   /* DELETE */
        al = dos_delete(dos, fcb);
        break;

    case 0x14:   /* SEQRD */
        al = dos_seqrd(dos, fcb);
        break;

    case 0x15:   /* SEQWRT */
        al = dos_seqwrt(dos, fcb);
        break;

    case 0x16:   /* CREATE */
        al = dos_create(dos, fcb);
        break;

    case 0x17:   /* RENAME */
        al = dos_rename(dos, fcb);
        break;

    case 0x18:   /* INUSE — unused */
    case 0x1D:   /* GETRDONLY — unused */
    case 0x1E:   /* SETATTRIB — unused */
    case 0x20:   /* USERCODE — unused */
        al = 0;
        break;

    case 0x19:   /* GETDRV */
        al = dos->curdrv;
        break;

    case 0x1A:   /* SETDMA */
        dos_setdma(dos, (uint8_t*)(uintptr_t)r->dx);
        break;

    case 0x1B:   /* GETFATPT (default drive) */ {
        dos->thisdrv = dos->curdrv;
        if (disk_fatread(dos) == 0) {
            dpb_t *dp = dos->thisbp;
            r->bx  = (uint16_t)(uintptr_t)dp->fat;
            r->dx  = (uint16_t)(dp->maxclus - 1);
            r->cx  = dp->secsiz;
            al     = (uint8_t)(dp->clusmsk + 1);
        } else al = DOS_ERR;
        break;
    }

    case 0x1C:   /* GETFATPTDL */ {
        uint8_t d = dl ? (uint8_t)(dl - 1) : dos->curdrv;
        dos->thisdrv = d;
        if (disk_fatread(dos) == 0) {
            dpb_t *dp = dos->thisbp;
            r->bx  = (uint16_t)(uintptr_t)dp->fat;
            r->dx  = (uint16_t)(dp->maxclus - 1);
            r->cx  = dp->secsiz;
            al     = (uint8_t)(dp->clusmsk + 1);
        } else al = DOS_ERR;
        break;
    }

    case 0x1F:   /* GETDSKPT */ {
        dos->thisdrv = dos->curdrv;
        if (disk_fatread(dos) == 0)
            r->bx = (uint16_t)(uintptr_t)dos->thisbp;
        break;
    }

    case 0x21:   /* RNDRD */
        al = dos_rndrd(dos, fcb);
        break;

    case 0x22:   /* RNDWRT */
        al = dos_rndwrt(dos, fcb);
        break;

    case 0x23:   /* FILESIZE */
        al = dos_filesize(dos, fcb);
        break;

    case 0x24:   /* SETRNDREC */
        al = dos_setrndrec(dos, fcb);
        break;

    case 0x25:   /* SETVECT — no-op in C port (no real interrupt table) */
        break;

    case 0x26:   /* NEWBASE — set up new program base */
        /* Simplified: just record DX as new program segment (paragraphs) */
        dos->endmem = r->dx;
        al = 0;
        break;

    case 0x27:   /* BLKRD */ {
        uint16_t cnt = r->cx, done = 0;
        al = dos_blkrd(dos, fcb, cnt, &done);
        r->cx = done;
        break;
    }

    case 0x28:   /* BLKWRT */ {
        uint16_t cnt = r->cx, done = 0;
        al = dos_blkwrt(dos, fcb, cnt, &done);
        r->cx = done;
        break;
    }

    case 0x29:   /* MAKEFCB */ {
        const char *src = (const char*)(uintptr_t)r->si;
        al = dos_makefcb(dos, &src, (fcb_t*)(uintptr_t)r->di, al);
        r->si = (uint16_t)(uintptr_t)src;
        break;
    }

    case 0x2A: { /* GETDATE */
        uint16_t yr; uint8_t mo, dy, dow;
        dos_getdate(dos, &yr, &mo, &dy, &dow);
        r->cx = yr;
        r->dx = (uint16_t)(((uint16_t)mo << 8) | dy);
        al    = dow;
        break;
    }

    case 0x2B: { /* SETDATE */
        al = dos_setdate(dos, r->cx, dh, dl);
        break;
    }

    case 0x2C: { /* GETTIME */
        uint8_t h, m, s, c;
        dos_gettime(dos, &h, &m, &s, &c);
        r->cx = (uint16_t)(((uint16_t)h << 8) | m);
        r->dx = (uint16_t)(((uint16_t)s << 8) | c);
        al = 0;
        break;
    }

    case 0x2D: { /* SETTIME */
        al = dos_settime(dos, ch, cl, dh, dl);
        break;
    }

    case 0x2E:   /* VERIFY */
        dos->verflg = al & 1;
        break;

    default:
        al = 0;
        break;
    }

    r->ax = (r->ax & 0xFF00) | al;
    return al;
}
