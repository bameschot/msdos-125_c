#pragma once
#include "types.h"
#include "bios.h"

/* -----------------------------------------------------------------------
 * DOS global state — everything that was CS:DATA / CS:CONSTANTS in
 * MSDOS.ASM, collapsed into a single C struct.
 * ----------------------------------------------------------------------- */

#define MAX_DRIVES   16
#define MAX_FAT_SIZE (4096 + 2)   /* 12-bit FAT: up to 4096 clusters, +2 prefix bytes */
#define MAX_SEC_SIZE 1024          /* largest supported sector size */

/* Two-byte FAT buffer prefix layout:
 *   fat_hdr[0]: bits 0-5 = device num, bit 7 = dirty (needs mapdev call)
 *   fat_hdr[1]: non-zero = FAT data needs writeback
 * fat pointer in dpb_t points to fat_hdr[2] (the actual FAT bytes).
 */
typedef struct {
    uint8_t dev_dirty;   /* device + "needs remap" flag */
    uint8_t fat_dirty;
    uint8_t data[MAX_FAT_SIZE];
} fat_buf_t;

typedef struct dos_s {
    bios_t  *bios;

    /* ----- drives ----- */
    uint8_t  num_drives;            /* logical drive count */
    uint8_t  num_io;                /* physical drive table count */
    uint8_t  curdrv;                /* current drive (0=A) */
    uint16_t curdir_clus[MAX_DRIVES]; /* current dir cluster per drive (0=root) */
    char     curdir_path[MAX_DRIVES][64]; /* display path e.g. "\DOCS\WORK" */
    dpb_t    drvtab[MAX_DRIVES];    /* DPB array */
    fat_buf_t fat_pool[MAX_DRIVES]; /* one FAT buffer per drive */

    /* ----- DMA (disk transfer address) ----- */
    uint8_t *dmaadd;                /* default 128-byte scratch at program base */

    /* ----- single-sector disk buffer ----- */
    uint8_t  buffer[MAX_SEC_SIZE];
    uint32_t bufsecno;              /* absolute sector in buffer (0xFFFFFFFF=none) */
    uint8_t  bufdrvno;              /* drive owning buffer (0xFF=none) */
    uint8_t  dirtybuf;
    dpb_t   *bufdrvbp;

    /* ----- directory sector buffer ----- */
    uint8_t  dirbuf[MAX_SEC_SIZE];
    uint32_t dirbufid;              /* ((uint32_t)devnum<<24)|sector, 0xFFFFFFFF=none */

    /* ----- date/time ----- */
    uint8_t  day, month;
    uint16_t year;                  /* year - 1980 */
    uint16_t daycnt;                /* days since Jan 1 1980 */
    uint8_t  weekday;               /* 0=Sun */

    /* ----- console state ----- */
    uint8_t  carpos;                /* cursor column */
    uint8_t  startpos;
    uint8_t  pflag;                 /* printer echo */
    uint8_t  verflg;                /* verify after write */

    /* ----- directory buffer dirty flag ----- */
    uint8_t  dirtydir;              /* directory sector needs writeback */

    /* ----- directory search state ----- */
    uint16_t lastent;               /* last entry number searched */
    uint16_t entfree;               /* first free entry offset */

    /* ----- filename scratch ----- */
    uint8_t  name1[11];
    uint8_t  name2[11];
    uint8_t  name3[12];
    uint8_t  attrib;
    bool     extfcb;                /* extended FCB in use */
    bool     creating;
    uint8_t  delall;                /* 0xE5 when DEL *.* */

    /* ----- per-transfer state (SETUP sets these) ----- */
    uint8_t  thisdrv;
    dpb_t   *thisbp;               /* DPB for current drive */
    uint8_t *curfat;               /* FAT data pointer for current drive */

    uint8_t  seccluspos;
    uint8_t  dskerr;
    bool     trans;
    bool     preread;
    bool     readop;               /* false=read, true=write (for HARDERR) */

    fcb_t   *fcb;                  /* FCB being operated on */
    uint8_t *nextadd;              /* advancing DMA pointer */
    uint32_t recpos;
    uint16_t reccnt;
    uint16_t lastpos;
    uint16_t clusnum;
    uint16_t secpos;
    uint16_t valsec;
    uint16_t bytsecpos;
    uint32_t bytpos;
    uint16_t bytcnt1;
    uint16_t bytcnt2;
    uint16_t seccnt;
    uint16_t entfree2;             /* alias used by fileio */

    /* ----- console line buffers ----- */
    uint8_t  inbuf[128];           /* line editing work area */
    uint8_t  conbuf[256];          /* BUFIN buffer (first 2 bytes = max/actual count) */
    uint16_t contpos;              /* offset into conbuf for next char */

    /* ----- error handlers ----- */
    int (*fatal_handler)(struct dos_s *dos, uint8_t drive, uint8_t area_rw);

    /* ----- program base for NEWBASE / EXEC ----- */
    uint8_t  psp[256];             /* current program's PSP */
    uint16_t endmem;               /* first unavailable segment (paragraphs) */
} dos_t;

/* Initialise DOS state.  bios must already be configured. */
int  dos_init(dos_t *dos, bios_t *bios);

/* Invoke a DOS system call (fn = function number, regs = argument block).
 * Returns AL result byte. */
typedef struct {
    uint16_t ax, bx, cx, dx, si, di, ds, es;
} dos_regs_t;

uint8_t dos_call(dos_t *dos, uint8_t fn, dos_regs_t *r);

/* Convenience: translate an 11-char padded name to "NAME.EXT\0" */
void dos_name_to_str(const uint8_t name11[11], char out[13]);
void str_to_dos_name(const char *str, uint8_t name11[11]);
