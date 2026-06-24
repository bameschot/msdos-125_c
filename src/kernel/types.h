#pragma once
#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * All on-disk and in-memory data structures from MSDOS.ASM.
 * See 8086_to_c_reference.md for field-by-field annotations.
 * ----------------------------------------------------------------------- */

/* FCB — File Control Block (37 bytes) */
#pragma pack(push,1)
typedef struct {
    uint8_t  drive;         /* 1=A, 0=default */
    uint8_t  name[8];
    uint8_t  ext[3];
    uint16_t extent;        /* current block/extent */
    uint16_t recsiz;        /* record size (default 128) */
    uint32_t filsiz;        /* file size in bytes */
    uint16_t fdate;         /* packed date: bits 15-9=year-1980, 8-5=mon, 4-0=day */
    uint16_t ftime;         /* packed time: bits 15-11=hr, 10-5=min, 4-0=sec/2 */
    uint8_t  devid;         /* bit7=device, bit6=dirty/EOF */
    uint16_t firclus;       /* first cluster */
    uint16_t lstclus;       /* last cluster accessed (cache) */
    uint16_t cluspos;       /* position of lstclus in chain */
    uint8_t  _pad;
    uint8_t  nr;            /* next record (sequential) */
    uint8_t  rr[4];         /* random record (24 or 32 bit) */
} fcb_t;
#pragma pack(pop)

#define FCB_DEVID_DEVICE 0x80
#define FCB_DEVID_DIRTY  0x40

/* Extended FCB: 7-byte prefix when first byte is 0xFF */
#pragma pack(push,1)
typedef struct {
    uint8_t  flag;          /* 0xFF */
    uint8_t  _res[5];
    uint8_t  attrib;
} extfcb_prefix_t;
#pragma pack(pop)

/* DPB — Drive Parameter Block */
typedef struct dpb_s {
    uint8_t  devnum;        /* I/O driver index */
    uint8_t  drvnum;        /* physical unit number */
    uint16_t secsiz;        /* bytes per sector */
    uint8_t  clusmsk;       /* sectors/cluster - 1 */
    uint8_t  clusshft;      /* log2(sectors/cluster) */
    uint16_t firfat;        /* first FAT sector */
    uint8_t  fatcnt;        /* number of FAT copies */
    uint16_t maxent;        /* max directory entries */
    uint16_t firrec;        /* first data sector */
    uint16_t maxclus;       /* total clusters + 1 */
    uint8_t  fatsiz;        /* sectors per FAT copy */
    uint16_t firdir;        /* first directory sector */
    /* Points into fat_buf_t.data (see dos.h).  fat[-1]=FAT dirty flag,
     * fat[-2]=device index; negative indexing is safe because fat_buf_t
     * allocates a 2-byte header before data[]. */
    uint8_t *fat;
} dpb_t;

/* 32-byte directory entry */
#pragma pack(push,1)
typedef struct {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attrib;
    uint8_t  _res[10];
    uint16_t ftime;
    uint16_t fdate;
    uint16_t firclus;
    uint32_t filsiz;
} dirent_t;
#pragma pack(pop)

#define DIRENT_SIZE   32
#define ATTR_READONLY 0x01
#define ATTR_HIDDEN   0x02
#define ATTR_SYSTEM   0x04
#define ATTR_VOLUME   0x08
#define ATTR_DIR      0x10
#define ATTR_ARCHIVE  0x20
#define DIRENT_FREE   0xE5
#define DIRENT_END    0x00

/* FAT12 */
#define FAT12_FREE    0x000
#define FAT12_EOF_MIN 0xFF8
#define FAT12_EOF     0xFFF
#define FAT12_BAD     0xFF7

static inline bool fat12_is_eof(uint16_t v)  { return v >= FAT12_EOF_MIN; }
static inline bool fat12_is_free(uint16_t v) { return v == FAT12_FREE;    }

/* Named device indices */
#define DEV_CON 0
#define DEV_AUX 1
#define DEV_PRN 2
#define DEV_NUL 3
#define DEV_LST 4
#define NUMDEV  5

/* DOS error codes returned in AL */
#define DOS_OK      0
#define DOS_ERR   0xFF  /* -1 as uint8_t */
#define DOS_EOF     1
#define DOS_NOSPACE 2   /* record trimmed (segment wrap) */
#define DOS_PARTIAL 3
#define DOS_NOFILE  4

/* Disk error action returned by INT 24H handler */
#define ERRACT_RETRY  1
#define ERRACT_IGNORE 0
#define ERRACT_ABORT  2
