# MS-DOS 1.25 C Port — Codebase Guide

This document describes the complete C port of MS-DOS 1.25 that lives in
`src/`.  The original 8086 assembly sources are in `v1.25/`; see
`v1.25/CLAUDE.md` for an annotated walkthrough of the assembly and
`v1.25/8086_to_c_reference.md` for the full porting reference.

---

## Directory Layout

```
src/
  kernel/          DOS kernel — FAT12, FCB API, INT 21H dispatcher
    types.h        On-disk data structures (FCB, DPB, DIRENT)
    keys.h         Extended key-code constants (KEY_UP, KEY_CTRL_W, …)
    bios.h         Hardware abstraction vtable (bios_t)
    dos.h          Central DOS state struct (dos_t) + dos_call()
    fat.h / fat.c  FAT12 pack/unpack, cluster allocation & chain ops
    disk.h / disk.c  Sector buffers, directory buffer, raw disk I/O
    fcb.h / fcb.c  FCB open/close/create/delete/rename, search, MAKEFCB
    fileio.h / fileio.c  Sequential, random & block file read/write
    chardev.h / chardev.c  Console I/O system calls (fn 01h–0Ch)
    datetime.h / datetime.c  Date/time system calls (fn 2Ah–2Eh)
    kernel.h / kernel.c  dos_init(), dos_call() dispatcher (fn 00h–2Eh)

  command/         Command processor (COMMAND.COM equivalent)
    command.h / command.c  Shell loop + all internal commands
    edit.h / edit.c  Full-screen text editor (EDIT command)

  host/            POSIX host wrapper — replaces IO.ASM
    bios_host.h / bios_host.c  termios console, disk image I/O, clock
    main.c         Entry point, --format mode, boot sequence
```

**Line counts (approximate):**

| File | Lines |
|------|-------|
| `command/command.c` | 1 200 |
| `host/bios_host.c` | 605 |
| `command/edit.c` | 610 |
| `kernel/fcb.c` | 590 |
| `kernel/fileio.c` | 447 |
| `kernel/kernel.c` | 375 |
| `kernel/chardev.c` | 235 |
| `kernel/fat.c` | 228 |
| `kernel/disk.c` | 215 |
| `kernel/datetime.c` | 172 |
| Headers (all) | ~570 |
| **Total** | **~5 250** |

---

## Core Data Types (`kernel/types.h`)

### `fcb_t` — File Control Block (37 bytes, `#pragma pack(1)`)

```c
typedef struct {
    uint8_t  drive;      // 1=A, 0=default
    uint8_t  name[8];    // space-padded filename
    uint8_t  ext[3];     // space-padded extension
    uint16_t extent;     // current block: record = (extent<<7)|nr
    uint16_t recsiz;     // bytes per record (default 128)
    uint32_t filsiz;     // file size in bytes
    uint16_t fdate;      // packed date  bits[15:9]=year-1980 [8:5]=mon [4:0]=day
    uint16_t ftime;      // packed time  bits[15:11]=hr [10:5]=min [4:0]=sec/2
    uint8_t  devid;      // bit7=device, bit6=0→dirty
    uint16_t firclus;    // first cluster
    uint16_t lstclus;    // last cluster accessed (chain cache)
    uint16_t cluspos;    // chain position of lstclus
    uint8_t  _pad;
    uint8_t  nr;         // next record (sequential position bits[6:0])
    uint8_t  rr[4];      // random record (24-bit or 32-bit)
} fcb_t;
```

Record position encoding: `record = (fcb->extent << 7) | (fcb->nr & 0x7F)`

### `dpb_t` — Drive Parameter Block

```c
typedef struct dpb_s {
    uint8_t  devnum;     // I/O driver index
    uint8_t  drvnum;     // physical unit number
    uint16_t secsiz;     // bytes per sector
    uint8_t  clusmsk;    // sectors/cluster − 1  (always 2^n − 1)
    uint8_t  clusshft;   // log2(sectors/cluster)
    uint16_t firfat;     // first FAT sector
    uint8_t  fatcnt;     // number of FAT copies
    uint16_t maxent;     // max root directory entries
    uint16_t firrec;     // first data sector (cluster 2 starts here)
    uint16_t maxclus;    // total clusters + 1
    uint8_t  fatsiz;     // sectors per FAT copy
    uint16_t firdir;     // first directory sector
    uint8_t *fat;        // pointer into FAT buffer pool (fat[-2]=devdirty, fat[-1]=fatdirty)
} dpb_t;
```

### `dirent_t` — 32-byte Directory Entry

```c
typedef struct {
    uint8_t  name[8];    // 0xE5=deleted, 0x00=end-of-directory
    uint8_t  ext[3];
    uint8_t  attrib;     // ATTR_HIDDEN=0x02, ATTR_SYSTEM=0x04, ATTR_ARCHIVE=0x20
    uint8_t  _res[10];
    uint16_t ftime;
    uint16_t fdate;
    uint16_t firclus;
    uint32_t filsiz;
} dirent_t;
```

---

## Hardware Abstraction Layer (`kernel/bios.h`, `kernel/keys.h`)

`bios_t` is a struct of function pointers filled by the host.  The kernel
never accesses hardware directly — it always calls through this vtable.

```c
struct bios_s {
    // Console
    int   (*stat)(bios_t*);                        // non-zero if key available
    int   (*in)(bios_t*);                          // read one byte, no echo
    void  (*out)(bios_t*, uint8_t);                // write one byte to console
    void  (*print)(bios_t*, uint8_t);              // write to printer
    int   (*auxin)(bios_t*);                       // aux port read
    void  (*auxout)(bios_t*, uint8_t);             // aux port write
    void  (*flush)(bios_t*);                       // flush input buffer

    // Disk (returns 0=ok, non-zero=error; *sectors_done set on error)
    int   (*disk_read) (bios_t*, unit, buf, count, sector, *done);
    int   (*disk_write)(bios_t*, unit, verify, buf, count, sector, *done);
    int   (*disk_change)(bios_t*, unit);           // -1=changed, 0=same, 1=unknown

    // Date / time
    void  (*gettime)(bios_t*, *days, *h, *m, *s, *c);
    void  (*settime)(bios_t*, h, m, s, c);
    void  (*setdate)(bios_t*, days_since_1980);

    // Screen
    void  (*cls)(bios_t*);                         // clear screen; may be NULL

    // Extended keyboard (for the editor)
    int   (*getkey)(bios_t*);                      // returns KEY_* constant; may be NULL
    void  (*getscreensize)(bios_t*, *rows, *cols); // falls back to 24×80; may be NULL

    // Drive mapping / geometry
    uint8_t (*mapdev)(bios_t*, unit, fat_byte);
    int     (*get_drive_config)(bios_t*, cfg_out, max_drives);

    void *priv;  // host-private data
};
```

### Key codes (`kernel/keys.h`)

```
0x01–0xFF  plain ASCII / control characters
0x101      KEY_UP       0x102  KEY_DOWN   0x103  KEY_LEFT    0x104  KEY_RIGHT
0x105      KEY_HOME     0x106  KEY_END    0x107  KEY_PGUP    0x108  KEY_PGDN
0x109      KEY_DEL      0x10A  KEY_INS    0x10B–0x114  KEY_F1–KEY_F10
0x115      KEY_CTRL_HOME          0x116  KEY_CTRL_END

Editor bindings (safe — no terminal special meaning):
  0x17  KEY_CTRL_W  Save (Write)
  0x18  KEY_CTRL_X  Quit (eXit)
  0x07  KEY_CTRL_G  Goto line

NOT used as bindings (terminal may intercept):
  0x13  KEY_CTRL_S  XOFF flow control
  0x11  KEY_CTRL_Q  XON  flow control
```

---

## DOS State (`kernel/dos.h`)

All global state from `CS:DATA`/`CS:CONSTANTS` in the assembly is
consolidated into a single `dos_t` struct.  The most important fields:

```c
typedef struct dos_s {
    bios_t  *bios;

    // Drives
    uint8_t  num_drives, num_io, curdrv;
    uint16_t curdir_clus[MAX_DRIVES]; // current directory cluster per drive (0 = root)
    char     curdir_path[MAX_DRIVES][64]; // display path e.g. "\DOCS\WORK"
    dpb_t    drvtab[MAX_DRIVES];      // DPB array (one per logical drive)
    fat_buf_t fat_pool[MAX_DRIVES];   // FAT buffers; fat_pool[i].data == dpb.fat

    // DMA — disk transfer address (set by dos_setdma / fn 1Ah)
    uint8_t *dmaadd;

    // Single-sector data buffer (write-through, one slot)
    uint8_t  buffer[MAX_SEC_SIZE];
    uint32_t bufsecno;    // 0xFFFFFFFF = empty
    uint8_t  bufdrvno;    // 0xFF = empty
    uint8_t  dirtybuf;
    dpb_t   *bufdrvbp;

    // Directory sector buffer (separate from data buffer)
    uint8_t  dirbuf[MAX_SEC_SIZE];
    uint32_t dirbufid;    // (devnum<<24)|absolute_sector, 0xFFFFFFFF = empty
    uint8_t  dirtydir;

    // Date / time
    uint8_t day, month; uint16_t year; uint16_t daycnt; uint8_t weekday;

    // Console
    uint8_t carpos, pflag, verflg;

    // Directory search state (set by MOVNAME / FINDNAME)
    uint16_t lastent, entfree;
    uint8_t  name1[11], name2[11], name3[12], attrib;
    bool     extfcb, creating; uint8_t delall;

    // Per-transfer state (set by SETUP, used by LOAD/STORE)
    uint8_t  thisdrv; dpb_t *thisbp; uint8_t *curfat;
    uint8_t  dskerr, seccluspos; bool trans, preread, readop;
    fcb_t   *fcb; uint8_t *nextadd;
    uint32_t recpos, bytpos;
    uint16_t reccnt, lastpos, clusnum, secpos, valsec;
    uint16_t bytsecpos, bytcnt1, bytcnt2, seccnt;

    // Console line buffers
    uint8_t inbuf[128], conbuf[256]; uint16_t contpos;

    // Hooks
    int (*fatal_handler)(dos_t*, uint8_t drive, uint8_t area_rw);

    uint8_t psp[256]; uint16_t endmem;
} dos_t;
```

`fat_buf_t` has a 2-byte prefix before the FAT data:
- `fat[-2]` = device number (bits 0–5) + dirty flag (bit 7)
- `fat[-1]` = FAT-data dirty flag (non-zero → needs writeback)

---

## FAT12 Layer (`kernel/fat.h / fat.c`)

```c
uint16_t fat_unpack(const uint8_t *fat, uint16_t n);
void     fat_pack  (uint8_t *fat, uint16_t n, uint16_t val);
```
Three bytes hold two 12-bit entries.  For cluster N: byte offset = N + N/2;
if N is odd, shift right 4 bits; mask to 12 bits.

```c
uint16_t fat_fndclus(dos_t*, dpb_t*, fcb_t*, uint16_t count, uint16_t *out_pos);
```
Walk the chain `count` clusters forward.  Uses `fcb->lstclus` / `cluspos`
as a cache to avoid walking from the start.  Returns `FAT12_EOF` if the
file has no clusters yet (firclus == 0).

```c
uint16_t fat_allocate(dos_t*, dpb_t*, fcb_t*, uint16_t tail, uint16_t tail_pos, uint16_t count);
void     fat_release (dos_t*, dpb_t*, uint16_t start_clus);
void     fat_relblks (dos_t*, dpb_t*, uint16_t bx, uint16_t eof_val);
int      fat_write   (dos_t*, dpb_t*);   // write all FAT copies back to disk
int      fat_read    (dos_t*, dpb_t*);   // read FAT from disk; call mapdev
```

**FAT special values:**
```
0x000  free    0xFF7  bad    0xFF8–0xFFF  EOF
```

---

## Disk Buffer Layer (`kernel/disk.h / disk.c`)

Two independent sector buffers:

| Buffer | Field | Purpose |
|--------|-------|---------|
| Data | `dos->buffer` + `bufsecno` + `dirtybuf` | Single-sector write-through cache for file data |
| Directory | `dos->dirbuf` + `dirbufid` + `dirtydir` | One directory sector (root or subdir) |

```c
int      disk_fatread(dos_t*);                    // ensure FAT current for dos->thisdrv
dpb_t   *disk_getbp  (dos_t*, uint8_t devnum);   // devnum → DPB pointer
int      disk_read   (dos_t*, dpb_t*, buf, count, sector);   // with HARDERR retry
int      disk_write  (dos_t*, dpb_t*, buf, count, sector);
uint8_t *disk_bufsec (dos_t*, dpb_t*, cluster, sec_in_clus, write_mode);
int      disk_buf_flush(dos_t*);
int      disk_dirread (dos_t*, dpb_t*, uint16_t block);
int      disk_dirwrite(dos_t*, dpb_t*);
int      disk_chkdirwrite(dos_t*, dpb_t*);        // write if dirty
uint32_t disk_figrec  (dpb_t*, uint16_t cluster, uint8_t sec_in_clus);
int      disk_reset   (dos_t*);                   // flush all buffers + FATs
```

**`disk_dirread(dos, dp, block)`** is subdirectory-aware.  `block` is a
sequential sector index within the current directory:

- If `dos->curdir_clus[dp->devnum] == 0` (root): computes
  `sector = dp->firdir + block`, bounded by `dp->maxent`.
- Otherwise (subdir): walks the FAT chain — `block / spc` gives the cluster
  index, `block % spc` the sector within that cluster — then calls
  `disk_figrec`.  Returns −1 at end-of-chain.

`dirbufid` encodes `(devnum << 24) | absolute_sector` so `disk_dirwrite`
can write to the correct on-disk sector without needing to recompute the
address.  All higher-level directory operations (`findname_impl`, `dos_create`,
`dos_delete`, `cmd_mkdir`, `cmd_dir`, …) call `disk_dirread` and therefore
work transparently in the current directory without any command-level changes.

`disk_bufsec` pre-read logic: skip pre-read when `secpos >= valsec`
(sector has never been written); pre-read when `secpos < valsec`
(overwriting existing data).  This matches the assembly BUFWRT `JA NOREAD`.

HARDERR: on disk error, classifies the sector (reserved/FAT/dir/data),
calls `dos->fatal_handler`; returns IGNORE (continue), RETRY, or ABORT.

---

## FCB / Directory Layer (`kernel/fcb.h / fcb.c`)

```c
int fcb_movname(dos_t*, fcb_t*);   // parse FCB → name1, attrib, thisdrv
int fcb_getname(dos_t*, fcb_t*, *bx, *si, *bh);  // movname + FINDNAME
int fcb_devname(dos_t*);           // check name1 against PRN/LST/NUL/AUX/CON
```

`findname_impl` scans the **current directory** for `dos->name1`, honouring
`?` wildcards and attribute filtering.

- **Starting position**: begins at `dos->lastent` (0xFFFF = start from
  entry 0).  This allows `dos_srchnxt` to resume after the last hit without
  rescanning from the beginning.
- **Termination**: for the root directory, stops after `dp->maxent` entries;
  for a subdirectory, stops at `DIRENT_END` or when `disk_dirread` returns
  −1 (end of cluster chain) — no fixed limit.
- Tracks `dos->lastent` (last matched entry index) and `dos->entfree` (first
  free slot, used by `dos_create` and `cmd_mkdir`).

System call implementations (return `DOS_OK`=0 or `DOS_ERR`=0xFF):
```c
dos_open, dos_close, dos_create, dos_delete, dos_rename,
dos_srchfrst, dos_srchnxt, dos_makefcb
```

`dos_makefcb` (fn 29h) parses `"[D:]NAME.EXT"` from a string into an FCB.
Returns 0=unambiguous, 1=wildcards present, 0xFF=bad drive.

---

## File I/O Layer (`kernel/fileio.h / fileio.c`)

### Record position encoding / decoding

```c
uint32_t fileio_getrec(const fcb_t *fcb);
// Returns:  (fcb->extent << 7) | (fcb->nr & 0x7F)

void fileio_setnrex(fcb_t *fcb, uint32_t new_pos, ...);
// Sets:  fcb->nr = new_pos & 0x7F;  fcb->extent = new_pos >> 7;
//        fcb->rr[0..3] = new_pos (little-endian 32-bit)
```

### SETUP → LOAD/STORE pipeline

`setup()` (internal):
1. Compute `bytpos = recpos × recsiz`
2. For reads: trim `total_bytes` to `min(count×recsiz, filsiz−bytpos)`
3. Compute `secpos`, `bytsecpos`, `clusnum` from `bytpos`
4. BREAKDOWN: split `total_bytes` into `bytcnt1` (partial first sector) +
   `seccnt` (whole sectors) + `bytcnt2` (partial last sector)

`do_load` / `do_store` drive the actual I/O using `disk_bufsec` (partial
sectors go through the sector cache) and `disk_read`/`disk_write`
(whole sectors bypass the cache for efficiency).

Public API:
```c
dos_seqrd / dos_seqwrt    // fn 14h/15h — advance NR by 1 after transfer
dos_rndrd / dos_rndwrt    // fn 21h/22h — position from FCB.RR
dos_blkrd / dos_blkwrt    // fn 27h/28h — CX records from FCB.RR
dos_filesize              // fn 23h — result in FCB.RR
dos_setrndrec             // fn 24h — copy sequential pos → FCB.RR
```

Error codes returned in AL: `0=ok  1=EOF  2=segment-wrap  3=partial  4=no-file`

---

## Console I/O (`kernel/chardev.h / chardev.c`)

```c
void chardev_out(dos_t*, uint8_t c);   // OUT: write + track carpos
int  chardev_in (dos_t*);              // IN:  read + Ctrl-S/P/C handling
```

`chardev_in` spin-waits on `bios->stat()` then calls `bios->in()`.
Handles inline: Ctrl-S (pause), Ctrl-P (toggle printer echo), Ctrl-C
(print `^C`, CRLF, return −1).

`dos_bufin` (fn 0Ah): buffered line input.  `buf[0]`=max length,
`buf[1]`=actual length on return, `buf[2..]`=characters.
Supports Backspace (erase), Ctrl-X/ESC (cancel line), Ctrl-Z (EOF).

---

## Date / Time (`kernel/datetime.h / datetime.c`)

Dates are stored as **days since Jan 1 1980**.  The kernel maintains
`dos->daycnt`, `dos->year`/`month`/`day`, `dos->weekday`.

`dos_readtime` calls `bios->gettime()` and recomputes the date only when
the day count changes (lazy update matching the assembly READTIME).

Packed FAT date/time:
```
fdate: bits[15:9]=year−1980  [8:5]=month  [4:0]=day
ftime: bits[15:11]=hour      [10:5]=minute [4:0]=seconds/2
```

---

## Kernel Init & Dispatch (`kernel/kernel.h / kernel.c`)

### `dos_init(dos_t *dos, bios_t *bios)`

1. Calls `bios->get_drive_config()` to get BPB data for each drive
2. Builds `drvtab[]` DPBs from BPB fields (sector size, FAT size,
   directory entries, cluster geometry)
3. Points each `dpb->fat` into `dos->fat_pool[i].data`
4. Reads FAT for drive 0 to validate the first disk

`bios->get_drive_config()` fills an array of `drive_cfg_t` structs
(secsiz, secs_per_clus, reserved_secs, fat_copies, root_entries,
total_sectors, fat_size).  The host reads these from the BPB at offset 11
of the boot sector.

### `dos_call(dos_t *dos, uint8_t fn, dos_regs_t *r)`

INT 21H dispatcher.  `fn` = function number (AH), `r` = register block.
Returns AL result.  All 47 functions 00h–2Eh are implemented:

| Range | Functions |
|-------|-----------|
| 00h–0Ch | Character I/O (CONIN, CONOUT, BUFIN, RAWIO, PRTBUF, …) |
| 0Dh–1Fh | Disk/FCB (OPEN, CLOSE, CREATE, DELETE, RENAME, SRCHFRST/NXT, SEQRD/WRT, SETDMA, GETFATPT, …) |
| 21h–28h | Random/block I/O (RNDRD/WRT, BLKRD/WRT, FILESIZE, SETRNDREC) |
| 29h–2Eh | Utilities (MAKEFCB, GETDATE, SETDATE, GETTIME, SETTIME, VERIFY) |

---

## Command Processor (`command/command.h / command.c`)

`command_run(dos_t*)` — main interpreter loop:
1. Print prompt: `A>` in root, `A:\DOCS>` in a subdirectory
   (`dos->curdir_path[curdrv]` is appended when non-empty)
2. Read line via `dos_bufin`
3. Call `command_exec` → dispatch

`command_exec` dispatches:
- Drive change (`A:`, `B:`, …)
- Internal commands via `cmd_table[]`
- `EXIT` → return −1 (breaks the loop)
- Unknown → `run_external()` (looks for `.COM` on disk; cannot execute x86 binary)

**Internal commands:**

| Command | Key behaviour |
|---------|--------------|
| `DIR [spec]` | Scan current directory; wildcards; shows `<DIR>` for subdirs; free bytes |
| `TYPE file` | Sequential 1-byte reads; stops at Ctrl-Z (0x1A) |
| `OPEN file` | Like TYPE but shows ALL bytes; non-printable as `^X`; high bytes as `.` |
| `COPY src dst` | 128-byte record sequential read→write loop; sets filsiz explicitly |
| `DEL/ERASE spec` | Wildcards; prompts Y/N for `*.*`; `dos->delall` set to 0x00 for DEL *.* |
| `REN/RENAME old new` | Wildcards in both names; checks for duplicate |
| `EDIT file` | Launches full-screen editor (see below) |
| `CREATE name` | `dos_create` + `dos_close`; rejects wildcards |
| `MKDIR/MD name` | Allocate cluster; write `.`/`..`; write ATTR_DIR entry in current dir |
| `RMDIR/RD name` | Verify empty; mark entry deleted; release cluster chain |
| `CD/CHDIR [path]` | Change `dos->curdir_clus[drv]` and `curdir_path`; `..` reads `..` entry's `firclus` |
| `CHKDSK` | Counts free FAT12 clusters; reports total/free bytes |
| `DATE`, `TIME` | Display then optionally set via `sscanf` |
| `ECHO`, `REM`, `PAUSE`, `VER`, `CLS`, `HELP` | Trivial |

**Subdirectory design note:** `cmd_mkdir` sets `dotdot->firclus =
dos->curdir_clus[dos->thisdrv]` so `..` always points to the actual parent
cluster (0 for root-level directories).  `cmd_rmdir` rejects removal of the
current working directory (`clus == dos->curdir_clus[drv]`).  `cmd_cd` with
`..` reads the `..` entry's `firclus` field directly from the directory's
first sector to find the parent — no separate parent tracking is needed.

---

## Text Editor (`command/edit.h / edit.c`)

`editor_run(dos_t*, filename)` — called by `cmd_edit`.

### Buffer model

```c
typedef struct { char *buf; int len; int cap; } edline_t;

typedef struct {
    edline_t *lines;  int nlines;  int lcap;  // dynamic line array
    int row, col;                              // cursor (0-based)
    int top, left;                             // scroll offsets
    int scr_rows, scr_cols;                    // visible area
    bool modified, running;
    char filename[14];
} ed_t;
```

Lines are heap-allocated, null-terminated, without newline characters.
The array grows via `realloc`.

### File I/O

**Load** (`ed_load`): reads the file in 128-byte FCB records into a flat
heap buffer, then splits on `\n` (stripping `\r`) into the line array.

**Save** (`ed_save`): streams all lines + CRLF through a 128-byte write
buffer (`wbuf_t`), flushing full records via `dos_seqwrt`.  Sets
`fcb.filsiz = exact_bytes` before `dos_close` to trim FAT12 zero-padding.

### Screen rendering

ANSI escape sequences written directly to `stdout` via `printf` (faster
than routing through `bios->out` byte-by-byte).

Layout:
```
Row 0:          status bar  (reverse video: filename, Ln/Col, Modified flag)
Rows 1..rows-2: content     (horizontal scroll if line > scr_cols)
Row rows-1:     help bar    (reverse video: key bindings)
```

Cursor is hidden (`\033[?25l`) during redraw and restored after.

### Key bindings

| Key | Code | Action |
|-----|------|--------|
| Arrow keys | `KEY_UP/DOWN/LEFT/RIGHT` | Move cursor |
| Home / End | `KEY_HOME / KEY_END` | Line start/end |
| Ctrl+Home / Ctrl+End | `KEY_CTRL_HOME/END` | File start/end |
| Page Up / Page Down | `KEY_PGUP / KEY_PGDN` | Scroll ±scr_rows |
| Backspace / 0x7F | — | Delete before; join at col 0 |
| Delete | `KEY_DEL` | Delete at cursor; join at EOL |
| Enter | `KEY_ENTER` (0x0D) | Split line |
| Tab | `KEY_TAB` (0x09) | Insert 4 spaces |
| **Ctrl+W** | 0x17 | **Save** (Write) |
| **Ctrl+X** / Esc | 0x18 / 0x1B | **Quit** (with Y/N/C prompt if modified) |
| Ctrl+G | 0x07 | Goto line (prompt via `dos_bufin`) |

`Ctrl+S` (0x13, XOFF) and `Ctrl+Q` (0x11, XON) are **not used** — they are
POSIX flow-control characters that terminals may intercept before the
process receives them.

---

## Host BIOS (`host/bios_host.h / bios_host.c`)

`host_bios_t` — first member is `bios_t base` (cast-compatible).

### Terminal (POSIX)

`enter_raw_mode()` sets full raw mode:
- `c_iflag`: clears `IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR | BRKINT | INPCK | ISTRIP`
- `c_lflag`: clears `ECHO | ECHOE | ECHOK | ECHONL | ICANON | IEXTEN | ISIG`
- `c_oflag`: keeps `OPOST` (so `printf("\r\n")` still works)
- `c_cc[VSTART] = c_cc[VSTOP] = 0` — disables XON/XOFF characters
- `VMIN=1, VTIME=0` — one byte at a time, no timeout

All stdin reads use **`read(STDIN_FILENO, &c, 1)`** — never `getchar()`.
This keeps `select()` and the actual read source in sync; `getchar()` has
a stdio internal buffer that causes `select()` to falsely report "no data"
for bytes already pulled out of the kernel buffer, breaking escape-sequence
detection.

### Escape sequence decoding (`hb_getkey`)

1. `raw_getbyte()` reads one byte
2. If not 0x1B → return as-is
3. `stdin_ready(50000)` — wait up to 50 ms for next byte
4. If nothing follows → return `KEY_ESC` (lone Escape)
5. Parse `ESC [` sequences: `A/B/C/D/H/F` → arrow/home/end; `1~`–`8~` →
   home/ins/del/end/pgup/pgdn; `1;5H/F` → Ctrl+Home/End
6. Parse `ESC O` sequences: `H/F/P/Q/R/S` → home/end/F1-F4

### Disk I/O

One `FILE*` per drive, opened `"r+b"` in `host_bios_init`.
`hb_disk_read` / `hb_disk_write`: `fseek(f, sector * secsiz, SEEK_SET)` +
`fread` / `fwrite`.  Sector size read from the BPB at init.

### Disk image formatting (`host_bios_format`)

Creates a raw FAT12 image:
1. Writes `total_sectors` zero-filled sectors
2. Builds BPB at boot sector offset 11; writes boot signature `0x55AA`
3. Iterates FAT size to convergence (FAT size depends on cluster count,
   which depends on FAT size)
4. Writes FAT copies (media byte + `0xFF 0xFF` at start)

Supported geometries:

| Flag | Capacity | Sectors | Heads | Sec/Track | Clusters |
|------|----------|---------|-------|-----------|---------|
| `--180` | 180 KB | 360 | 1 | 9 | 1 sec/clus |
| `--360` | 360 KB | 720 | 2 | 9 | 2 sec/clus |
| `--720` | 720 KB | 1440 | 2 | 9 | 2 sec/clus |

### Entry point (`host/main.c`)

```
main(argc, argv)
  --format path [--720|--360|--180]  → host_bios_format() + exit
  path [path...]                     → host_bios_init()
                                       dos_init()
                                       command_run()
                                       disk_reset()   (flush all buffers)
                                       host_bios_shutdown()
```

---

## Build

```sh
make            # produces ./msdos
make clean
make format-test  # formats test.img --720 and boots it
```

`CFLAGS`: `-std=c11 -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L`

Dependencies: standard C library + POSIX (`termios`, `select`, `ioctl`,
`read`).  No external libraries.

---

## Known Limitations

- **No external command execution** — `.COM`/`.EXE` files on the FAT12 image
  cannot be run (would require an x86 emulator).
- **Subdirectory cluster extension not implemented** — if a subdirectory fills
  its initial cluster (16 entries for a 512-byte sector), creating new entries
  inside it will fail.  Root directories are not affected (they have a fixed
  large area).
- **FAT12 only** — FAT16/FAT32 images will not load correctly.
- **Single sector buffer** — matching the original; only one data sector is
  cached in RAM at a time.
- **Printer output** goes to `stderr` on the host.
- **AUX port** is a no-op.
