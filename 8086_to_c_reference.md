# 8086 Assembly → C Porting Reference
### Specific to MS-DOS 1.25 (MSDOS.ASM / COMMAND.ASM)

---

## 1. Registers

### General-purpose registers

| 16-bit | 8-bit high | 8-bit low | Typical role in this codebase |
|--------|-----------|-----------|-------------------------------|
| `AX`   | `AH`      | `AL`      | Function return value; dividend/quotient in MUL/DIV; I/O byte |
| `BX`   | `BH`      | `BL`      | Base pointer into tables; cluster number; buffer pointer |
| `CX`   | `CH`      | `CL`      | Loop counter (`LOOP`); shift count; byte/word count for REP |
| `DX`   | `DH`      | `DL`      | Second word of 32-bit operands; sector number; drive number |

The 8-bit halves can be used independently. `AX = (AH << 8) | AL`.

```c
// MOV AH, 0          →  ah = 0;  (ax = (ah<<8)|al still valid)
// MOV AL, [THISDRV]  →  al = thisdrv;
// MOV AX, BX         →  ax = bx;
```

### Index and pointer registers

| Register | Description | C analogue |
|----------|-------------|------------|
| `SI`     | Source index; used with `LODS*`, `MOVS*`, `CMPS*` | `uint8_t *src` |
| `DI`     | Destination index; used with `STOS*`, `MOVS*`, `SCAS*` | `uint8_t *dst` |
| `BP`     | Base pointer — points to current Drive Parameter Block | `struct dpb *bp` |
| `SP`     | Stack pointer | (managed by C runtime) |

### Segment registers

| Register | Purpose in this code |
|----------|----------------------|
| `CS`     | Code segment = DOS kernel segment. Also used as a fast `DS` substitute since `DS=CS` inside the kernel. |
| `DS`     | Data segment. Set to `CS` at kernel entry; temporarily changed for far memory copies. |
| `ES`     | Extra segment. Points to user FCB segment during file operations. |
| `SS`     | Stack segment. Swapped between DOS internal stack and user stack around system calls. |

In C, all these segment distinctions collapse to plain pointers once you pick a single flat address space. Segment:offset address = `segment * 16 + offset`.

---

## 2. Flags and Condition Codes

The FLAGS register controls all conditional branches. Each C comparison below is the condition under which the corresponding jump is taken.

| Flag | Name | Set when |
|------|------|----------|
| `ZF` | Zero | Result == 0 |
| `CF` | Carry | Unsigned overflow / borrow |
| `SF` | Sign | Result < 0 (MSB set) |
| `OF` | Overflow | Signed overflow |
| `DF` | Direction | String ops go high→low when set (via `STD`) |

### Jump instructions → C conditions

| Jump | Unsigned meaning | Signed meaning | C condition |
|------|-----------------|----------------|-------------|
| `JZ` / `JE` | — | — | `result == 0` |
| `JNZ` / `JNE` | — | — | `result != 0` |
| `JC` / `JB` / `JNAE` | below | — | `a < b` (unsigned) |
| `JNC` / `JAE` / `JNB` | above or equal | — | `a >= b` (unsigned) |
| `JA` / `JNBE` | above | — | `a > b` (unsigned) |
| `JBE` / `JNA` | below or equal | — | `a <= b` (unsigned) |
| `JL` / `JNGE` | — | less | `a < b` (signed) |
| `JGE` / `JNL` | — | greater or equal | `a >= b` (signed) |
| `JG` / `JNLE` | — | greater | `a > b` (signed) |
| `JLE` / `JNG` | — | less or equal | `a <= b` (signed) |
| `JS` | — | — | `result < 0` (sign bit set) |
| `JNS` | — | — | `result >= 0` |
| `JCXZ` | — | — | `cx == 0` |

**Key rule**: comparisons involving clusters, sector numbers, drive numbers, and byte counts use **unsigned** branches (`JA`/`JB`/`JAE`/`JBE`). Comparisons involving error codes or flags often use **signed** branches (`JS`/`JNS`/`JL`/`JG`).

---

## 3. Common Instructions → C

### Data movement

```asm
MOV  AX, BX          ; ax = bx;
MOV  AL, [BX]        ; al = *(uint8_t*)bx;
MOV  [DI], AL        ; *(uint8_t*)di = al;
MOV  AX, [BP.SECSIZ] ; ax = bp->secsiz;   (structure member via BP)
MOV  WORD PTR [SI], 0 ; *(uint16_t*)si = 0;
XCHG AX, BX          ; { uint16_t t = ax; ax = bx; bx = t; }
XCHG AL, [BX]        ; { uint8_t t = al; al = *(uint8_t*)bx; *(uint8_t*)bx = t; }
```

### Load effective address

```asm
LEA  DI, [SI+BX]     ; di = si + bx;   (address, not value)
LEA  DI, [BP.MAXCLUS]; di = (uint16_t*)&bp->maxclus;
```

### Arithmetic

```asm
ADD  AX, BX           ; ax += bx;
ADC  DX, 0            ; dx += carry;    (ripple carry for 32-bit add)
SUB  AX, CX           ; ax -= cx;
SBB  BX, 0            ; bx -= borrow;   (borrow propagation for 32-bit sub)
INC  AX               ; ax++;
DEC  BX               ; bx--;
NEG  AX               ; ax = -ax;       (two's complement negate)
MUL  BX               ; DX:AX = (uint32_t)ax * bx;  (unsigned, result in DX:AX)
DIV  BX               ; AX = DX:AX / bx;  DX = DX:AX % bx;  (unsigned)
CBW                   ; ax = (int16_t)(int8_t)al;  (sign-extend AL to AX)
CWD                   ; dx:ax = (int32_t)ax;        (sign-extend AX to DX:AX)
```

32-bit arithmetic pattern (this codebase uses DX:AX constantly):
```asm
; 32-bit add: DX:AX += CX:BX
ADD  AX, BX
ADC  DX, CX
```
```c
uint32_t dxax = ((uint32_t)dx << 16) | ax;
uint32_t cxbx = ((uint32_t)cx << 16) | bx;
dxax += cxbx;
dx = dxax >> 16;  ax = dxax & 0xFFFF;
```

### Bitwise and shift

```asm
AND  AX, 0FFFh        ; ax &= 0x0FFF;
OR   AL, 40H          ; al |= 0x40;
XOR  AX, AX           ; ax = 0;          (canonical zero idiom)
NOT  AL               ; al = ~al;
TEST AL, 3            ; (al & 3) — sets flags, discards result
SHL  AX, 1            ; ax <<= 1;  CF gets the evicted bit
SHR  AX, 1            ; ax >>= 1;  unsigned right shift
SAR  AL, 1            ; al >>= 1;  signed (arithmetic) right shift
RCL  BX, 1            ; bx = (bx << 1) | carry; carry = old MSB
RCR  AL, 1            ; al = (carry << 7) | (al >> 1); carry = old LSB
```

Multi-bit shift — 8086 only allows `SHL r,1` or `SHL r,CL` (count in CL):
```asm
MOV  CL, 4
SHL  AX, CL           ; ax <<= 4;
```

### String/block operations

These operate on `DS:SI` → `ES:DI`, count in `CX`. Direction controlled by `DF` (clear = auto-increment via `CLD`, set = auto-decrement via `STD`).

| Instruction | C equivalent |
|-------------|-------------|
| `LODSB` | `al = *si++;` |
| `LODSW` | `ax = *(uint16_t*)si; si += 2;` |
| `STOSB` | `*di++ = al;` |
| `STOSW` | `*(uint16_t*)di = ax; di += 2;` |
| `MOVSB` | `*di++ = *si++;` |
| `MOVSW` | `*(uint16_t*)di = *(uint16_t*)si; si+=2; di+=2;` |
| `SCASB` | `al - *di++; set flags` (scan for value) |
| `CMPSB` | `*si++ - *di++; set flags` (compare strings) |
| `REP MOVSW` | `memcpy(di, si, cx*2); di+=cx*2; si+=cx*2; cx=0;` |
| `REP STOSB` | `memset(di, al, cx); di+=cx; cx=0;` |
| `REP STOSW` | `wmemset(di, ax, cx); di+=cx*2; cx=0;` |
| `REPE CMPSB` | compare while equal, up to CX bytes |
| `REPNE SCASB` | scan until `al == *di`, up to CX bytes |

Common block-move pattern:
```asm
MOV  CX, 11
REP  MOVSW          ; copies 22 bytes (11 words)
```
```c
memcpy(di, si, 22);
```

### Stack operations

```asm
PUSH AX             ; sp -= 2; *(uint16_t*)sp = ax;
POP  BX             ; bx = *(uint16_t*)sp; sp += 2;
PUSHF               ; push FLAGS register
POPF                ; pop FLAGS register
```

### Control flow

```asm
CALL label          ; near call (within same segment)
CALL FAR PTR BIOSREAD ; far call to another segment (e.g., BIOS)
RET                 ; near return
IRET                ; interrupt return (pops IP, CS, FLAGS)
JMP  label          ; unconditional jump
JMP  SHORT label    ; short jump (±127 bytes) — no C difference
JMP  CS:[BX+DISPATCH] ; indirect jump through table = dispatch[bx/2]()
LOOP label          ; if (--cx != 0) goto label;
```

---

## 4. Memory and Addressing Model

### Segmentation

The 8086 has a 20-bit address space split into 16-byte paragraphs. Every address is `segment:offset` where the physical byte is at `segment*16 + offset`.

In this kernel, `DS=CS=ES=SS=DOSGROUP` during normal execution. The segment value never changes except for temporary far copies. When porting to C with a flat memory model, collapse all segment references to a single base pointer or raw `uint8_t*`.

### Accessing structure members through BP

`BP` holds a pointer to the current Drive Parameter Block (`struct dpb`). Member access uses structure offsets defined with `STRUC`:

```asm
MOV  AL, [BP.DEVNUM]    ; al = bp->devnum;
MOV  AX, [BP.SECSIZ]    ; ax = bp->secsiz;
ADD  BP, DPBSIZ         ; bp++;  (advance to next DPB in array)
```

### `CS:` segment override

When `DS` is temporarily set to something else, variables in the DOS data segment are accessed with `CS:` prefix (because CODE and DATA are in the same group):

```asm
MOV  CS:[THISDRV], AL   ; dosdata.thisdrv = al;
MOV  AX, CS:[SPSAVE]    ; ax = dosdata.spsave;
```

In C, all these become accesses to a global `dosdata` struct.

### Far pointers and `LDS` / `LES`

```asm
LDS  SI, DWORD PTR [SPSAVE]    ; DS:SI = *(uint32_t*)&spsave;
                                ; i.e., si = spsave.offset; ds = spsave.segment;
LES  DI, DWORD PTR [DMAADD]    ; ES:DI = *(uint32_t*)&dmaadd;
```

In this code, `SPSAVE`/`SSSAVE` hold the user's `SS:SP`, and `DMAADD` holds `seg:offset` of the DMA (disk transfer) buffer. In C, represent these as `uint8_t*` pointers (with the full flat address computed once).

### The `[BX+DISPATCH]` indirect dispatch table

```asm
MOV  BL, AH          ; function number
MOV  BH, 0
SHL  BX, 1           ; word index
CALL CS:[BX+DISPATCH] ; dispatch[fn]()
```
```c
typedef void (*syscall_fn)(void);
static const syscall_fn dispatch[] = {
    abort, conin, conout, reader, punch, list, ...
};
dispatch[ah]();
```

---

## 5. Calling Conventions in This Codebase

The 8086 has no standard C ABI here. All functions communicate through **registers** and a shared **global data area** (`CS:DATA`). There are no stack-passed arguments except for `PUSH`/`POP` used within a single function.

### Input register conventions (by function)

Most subroutines document their inputs in a comment block. Common patterns:

| Function | Key inputs | Key outputs |
|----------|-----------|-------------|
| `UNPACK` | `BX`=cluster, `BP`=DPB ptr, `SI`=FAT ptr | `DI`=FAT entry, `ZF` if free |
| `PACK` | `BX`=cluster, `DX`=data, `SI`=FAT ptr | FAT updated in memory |
| `GETBP` | `AL`=device number | `BP`=DPB pointer |
| `DREAD` | `BX:DS`=buffer, `CX`=sector count, `DX`=sector num, `BP`=DPB | `CF` on error |
| `DWRITE` | same as DREAD, `AH`=verify flag | `CF` on error |
| `LOAD` | `DS:DI`=FCB, `DX:AX`=record position, `CX`=count | `DX:AX`=last record, `CX`=bytes read |
| `STORE` | same as LOAD | same |
| `ALLOCATE` | `BX`=last cluster (0 if new), `CX`=clusters needed, `SI`=FAT | `CF` if disk full, else `BX`=first new cluster |
| `RELEASE` | `BX`=start cluster, `SI`=FAT, `BP`=DPB | chain freed |
| `SETUP` | `DS:DI`=FCB, `DX:AX`=record pos, `CX`=count | many globals set; returns 1 level up if CX=0 |
| `HARDERR` | `AX`=error code, `DX`=sector, `BP`=DPB | calls INT 24H |

### Return values

- **`AL`** = 8-bit return / error code (0=success, 0xFF=error for most FCB calls)
- **`AX`** = 16-bit return or first word of a pair
- **`CF`** (carry flag) = error indicator for disk I/O (`JC` to handle error)
- **`ZF`** (zero flag) = boolean result for searches and attribute checks

In C terms:
```c
// AL = 0 → return 0; AL = -1 (0xFF) → return -1;
// CF set → return -1; CF clear → return 0;
```

### "Return 1 level up" pattern

`SETUP` calls `POP BX` to discard its own return address when no transfer will occur, effectively returning to its *caller's* caller:

```asm
SETUP:
    ...
    JCXZ    NOROOM
    ...
RET
NOROOM:
    POP     BX      ; discard return address
    RET             ; return to SETUP's caller's caller
```
```c
// In C, replicate with a sentinel return value that the immediate caller propagates:
if (setup(...) == NOROOM) return NOROOM;
```

---

## 6. Data Structures

### FCB — File Control Block (`FCBLOCK` struc, 37 bytes)

```c
#pragma pack(1)
typedef struct {
    uint8_t  drive;         // +0  drive number (1=A, 0=default)
    uint8_t  name[8];       // +1  filename
    uint8_t  ext[3];        // +9  extension
    uint16_t extent;        // +12 current extent (high byte = extent, low = block)
    uint16_t recsiz;        // +14 record size in bytes (default 128)
    uint32_t filsiz;        // +16 file size in bytes
    uint16_t fdate;         // +20 date of last write (packed: bits 15-9=year-1980, 8-5=month, 4-0=day)
    uint16_t ftime;         // +22 time of last write (packed: bits 15-11=hour, 10-5=min, 4-0=sec/2)
    uint8_t  devid;         // +24 device ID: bit7=device(not file), bit6=dirty or EOF
    uint16_t firclus;       // +25 first cluster of file
    uint16_t lstclus;       // +27 last cluster accessed (cache)
    uint16_t cluspos;       // +29 position of lstclus in chain (cache)
    uint8_t  _pad;          // +31 padding (forces NR to offset 32)
    uint8_t  nr;            // +32 next record (sequential)
    uint8_t  rr[4];         // +33 random record (24-bit or 32-bit)
} FCB;                      // total: 37 bytes
#pragma pack()

// devid flags
#define DEVID_DEVICE    0x80  // bit 7: named I/O device (not a file)
#define DEVID_DIRTY     0x40  // bit 6: file has been written (if file) / EOF (if device)
```

The `FILDIRENT` and `DRVBP` fields overlap `FILSIZ` and `DRVBP` during `SEARCH FIRST` / `SEARCH NEXT` (they are not simultaneously valid).

**Extended FCB**: if `drive == 0xFF`, the FCB is 7 bytes longer at the front:
```c
typedef struct {
    uint8_t  flag;      // 0xFF
    uint8_t  _res[5];   // reserved
    uint8_t  attrib;    // attribute byte
    FCB      fcb;       // normal FCB follows
} ExtFCB;
```

### DPB — Drive Parameter Block (`DPBLOCK` struc, 20 bytes)

```c
#pragma pack(1)
typedef struct {
    uint8_t  devnum;    // +0  I/O driver index
    uint8_t  drvnum;    // +1  physical unit number
    uint16_t secsiz;    // +2  bytes per sector
    uint8_t  clusmsk;   // +4  sectors-per-cluster minus 1 (always 2^n - 1)
    uint8_t  clusshft;  // +5  log2(sectors per cluster)
    uint16_t firfat;    // +6  first sector of FAT area
    uint8_t  fatcnt;    // +8  number of FAT copies
    uint16_t maxent;    // +9  max directory entries
    uint16_t firrec;    // +11 first data sector (cluster 2 starts here)
    uint16_t maxclus;   // +13 total clusters + 1 (highest valid cluster number)
    uint8_t  fatsiz;    // +15 sectors per FAT copy
    uint16_t firdir;    // +16 first directory sector
    uint16_t fat;       // +18 pointer/offset to in-memory FAT buffer
} DPB;                  // total: 20 bytes
#pragma pack()

#define DPBSIZ  20

// Conversion macros
#define CLUSTER_TO_SECTOR(dpb, clus, sec_in_clus) \
    (((clus) - 2) * ((dpb)->clusmsk + 1) + (sec_in_clus) + (dpb)->firrec)
```

### 32-byte Directory Entry

```c
#pragma pack(1)
typedef struct {
    uint8_t  name[8];   // +0  filename (0xE5 = deleted, 0x00 = end of directory)
    uint8_t  ext[3];    // +8  extension
    uint8_t  attrib;    // +11 attributes (bits 1,2 = hidden)
    uint8_t  _res[10];  // +12 reserved
    uint16_t ftime;     // +22 time (bits 15-11=hr, 10-5=min, 4-0=sec/2)
    uint16_t fdate;     // +24 date (bits 15-9=year-1980, 8-5=mon, 4-0=day)
    uint16_t firclus;   // +26 first cluster
    uint32_t filsiz;    // +28 file size in bytes
} DIRENT;               // total: 32 bytes
#pragma pack()

#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define DIRENT_DELETED  0xE5
#define DIRENT_END      0x00
```

### STKPTRS — Saved User Register Frame

When the kernel saves user registers on the user's stack, they land in this order (pushed in reverse, so the struct is in pop order):

```c
#pragma pack(2)
typedef struct {
    uint16_t ax;    // pushed last → lowest address on stack after saves
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t si;
    uint16_t di;
    uint16_t bp;
    uint16_t ds;
    uint16_t es;
    uint16_t ip;    // from interrupt
    uint16_t cs;
    uint16_t flags;
} STKPTRS;
#pragma pack()
```

The kernel accesses this through `SPSAVE`/`SSSAVE` to patch return registers (e.g., `MOV [SI.BXSAVE], BX` writes `bx` into the saved frame so the caller sees it in BX after `IRET`).

---

## 7. 12-bit FAT Arithmetic

The 12-bit FAT packs two entries into every three bytes. Cluster N's entry:

```c
// Read FAT entry for cluster N
uint16_t fat_unpack(const uint8_t *fat, uint16_t n) {
    uint32_t byte_offset = n + (n >> 1);  // n * 1.5
    uint16_t word = *(uint16_t*)(fat + byte_offset);
    if (n & 1)
        return word >> 4;       // odd cluster: upper 12 bits
    else
        return word & 0x0FFF;   // even cluster: lower 12 bits
}

// Write FAT entry for cluster N
void fat_pack(uint8_t *fat, uint16_t n, uint16_t val) {
    uint32_t byte_offset = n + (n >> 1);
    uint16_t *p = (uint16_t*)(fat + byte_offset);
    if (n & 1) {
        *p = (*p & 0x000F) | (val << 4);
    } else {
        *p = (*p & 0xF000) | (val & 0x0FFF);
    }
}

// Special FAT values
#define FAT_FREE        0x000   // cluster is free
#define FAT_EOF_MIN     0xFF8   // 0xFF8–0xFFF = end of chain
#define FAT_EOF         0xFFF   // standard EOF marker

#define fat_is_eof(v)   ((v) >= FAT_EOF_MIN)
```

The in-memory FAT buffer has a two-byte header before the first entry:
- `fat[-2]` = device number + dirty flag (bit 7 = dirty, bits 0-5 = device num)
- `fat[-1]` = dirty byte for the FAT itself (non-zero = needs write-back)

---

## 8. Interrupt and System Call Mechanics

### INT 21H dispatch (kernel entry)

```c
// Conceptual C representation of SAVREGS / LEAVE / REDISP
void int21h_handler(STKPTRS *regs) {
    uint8_t fn = regs->ax >> 8;  // AH = function number
    if (fn > MAXCOM) { regs->ax &= 0xFF00; return; }  // BADCALL

    spsave = regs; sssave = user_ss;  // save user stack
    // switch to DOS stack (IOSTACK or DSKSTACK)

    dispatch[fn]();  // call handler; result left in AL

    regs->ax = (regs->ax & 0xFF00) | al;  // patch AL in saved frame
    // restore user stack; IRET
}
```

### BIOS far calls

All hardware calls go through fixed offsets in the BIOS segment (segment `0x40` for SCP, `0x60` for IBM). In C, represent these as function pointers initialized at boot:

```c
// BIOS jump table (each entry is a 3-byte JMP instruction)
typedef void (*bios_fn)(void);

// Initialized during DOSINIT
extern bios_fn bios_stat;       // console status
extern bios_fn bios_in;         // console read
extern bios_fn bios_out;        // console write
extern bios_fn bios_print;      // printer write
extern bios_fn bios_auxin;      // aux read
extern bios_fn bios_auxout;     // aux write
extern int  (*bios_read)(uint8_t dev, uint8_t *buf, uint16_t count, uint16_t sector);
extern int  (*bios_write)(uint8_t dev, uint8_t verify, uint8_t *buf, uint16_t count, uint16_t sector);
extern int  (*bios_dskchg)(uint8_t dev);  // returns -1=changed, 0=unchanged, 1=don't know
extern void (*bios_setdate)(uint16_t days);
extern void (*bios_settime)(uint8_t h, uint8_t m, uint8_t s, uint8_t c);
extern void (*bios_gettime)(uint16_t *days_out, uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *c);
extern void (*bios_flush)(void);
extern void (*bios_mapdev)(uint8_t dev, uint8_t fat_first_byte); // returns new dev num
```

### INT 24H — Fatal Error

The fatal error interrupt is invoked with the user's stack restored. On return:
- `AL == 0`: ignore
- `AL == 1`: retry
- `AL == 2`: abort (`JMP ERROR`)

```c
typedef int (*fatal_error_handler)(uint8_t drive, uint8_t area_rw);
// area_rw: bits 1-2 = area (0=reserved, 1=FAT, 2=dir, 3=data), bit 0 = 0=read/1=write
```

---

## 9. Global Data Area (DOS Variables)

These are the `CS:DATA`/`CS:CONSTANTS` variables. In C, collect them into a global struct:

```c
// CONSTANTS segment (read-mostly)
static const char ioname_ms[]  = "PRN LST NUL AUX CON ";   // 4 chars each, no null
static const char divmes[]     = "\r\nDivide overflow\r\n$";

static uint8_t  carpos;         // current cursor column position
static uint8_t  startpos;       // column at start of current input line
static uint8_t  pflag;          // printer echo flag (non-zero = echo to printer)
static uint8_t  dirtydir;       // directory buffer needs write-back
static uint8_t  numdrv;         // total number of logical drives
static uint8_t  numio;          // number of physical drive tables
static uint8_t  verflg;         // verify-after-write flag (function 2EH)
static uint16_t contpos;        // continuation position in console line buffer

// DMA (disk transfer area) — stored as segment:offset pair
static uint16_t dmaadd_off;     // default = 0x0080 (PSP DMA area)
static uint16_t dmaadd_seg;

static uint16_t endmem;         // first unavailable segment (paragraphs)
static uint16_t maxsec;         // largest sector size across all drives
static uint16_t buffer_off;     // offset of single-sector disk buffer
static uint16_t bufsecno;       // sector number in buffer (0 = invalid)
static uint8_t  bufdrvno;       // drive number owning buffer (0xFF = none)
static uint8_t  dirtybuf;       // buffer needs write-back
static uint16_t bufdrvbp;       // DPB pointer for drive owning buffer
static uint16_t dirbufid;       // sector ID of directory buffer (0xFFFF = invalid)

// Date/time
static uint8_t  day, month;
static uint16_t year;           // year - 1980
static uint16_t daycnt;         // total days since epoch (0 = Jan 1 1980)
static uint8_t  weekday;        // 0=Sunday

static uint8_t  curdrv;         // current default drive (0=A)
static uint16_t drvtab;         // offset of DPB array

// DATA segment (per-call scratch)
static uint8_t  inbuf[128];     // input line working buffer
static uint8_t  conbuf[131];    // console BUFIN buffer

static uint16_t lastent;        // last directory entry number searched
static uint8_t  exithold[4];    // saved exit address (far pointer)
static uint16_t fatbase;        // (init scratch)
static uint8_t  name1[11];      // current filename being searched
static uint8_t  attrib;         // attribute byte for current operation
static uint8_t  name2[11];      // second name (RENAME target)
static uint8_t  name3[12];      // third name (RENAME source copy)
static uint8_t  extfcb;         // 0xFF if extended FCB in use
static uint8_t  creating;       // non-zero during CREATE
static uint8_t  delall;         // 0 normally; 0xE5 when deleting *.*

static uint16_t spsave, sssave; // user SS:SP saved at kernel entry
static uint16_t contstk;        // SP at INT 24H call site

// Per-transfer scratch (set by SETUP, used by LOAD/STORE)
static uint8_t  seccluspos;     // sector within cluster for current transfer
static uint8_t  dskerr;         // 0=ok, 1=EOF/error, 2=segment wrap, 3=partial last record, 4=no file
static uint8_t  trans;          // non-zero after first sector transfer happens
static uint8_t  preread;        // 0 = must pre-read sector before write
static uint8_t  readop;         // 0=read, 1=write (for HARDERR)
static uint8_t  thisdrv;        // physical drive unit for current op

static uint16_t fcb_ptr;        // offset of current FCB
static uint16_t nextadd;        // current DMA offset (advances during transfer)
static uint32_t recpos;         // record position in file (32-bit)
static uint16_t reccnt;         // record count for block operations
static uint16_t lastpos;        // cluster chain position of last cluster
static uint16_t clusnum;        // current cluster number
static uint16_t secpos;         // sector position of transfer start
static uint16_t valsec;         // count of sectors previously written (for no-preread opt)
static uint16_t bytsecpos;      // byte offset within first sector
static uint32_t bytpos;         // byte position in file (32-bit)
static uint16_t bytcnt1;        // bytes to transfer in first partial sector
static uint16_t bytcnt2;        // bytes to transfer in last partial sector
static uint16_t seccnt;         // count of whole sectors in transfer
static uint16_t entfree;        // offset of first free directory entry found
```

---

## 10. Key Code Patterns and Their C Equivalents

### Cluster-to-sector conversion (`FIGREC`)

```asm
FIGREC:
    DEC  DX          ; cluster - 1
    DEC  DX          ; cluster - 2  (cluster 2 = first data cluster)
    SHL  DX, CL      ; * sectors_per_cluster (CL = clusshft)
    OR   DL, BL      ; + sector within cluster
    ADD  DX, [BP.FIRREC]  ; + first data sector
```
```c
uint16_t figrec(const DPB *dpb, uint16_t cluster, uint8_t sec_in_cluster) {
    return ((cluster - 2) << dpb->clusshft) | sec_in_cluster + dpb->firrec;
}
```

### FAT dirty byte protocol

Every in-memory FAT buffer has a 2-byte prefix at `fat - 2`:
```c
// fat is uint8_t* pointing to byte 0 of the FAT data
uint8_t  fat_devno  = fat[-2];   // bits 0-5 = device, bit 7 = dirty
uint8_t  fat_dirty  = fat[-1];   // non-zero = FAT needs write-back
```

### `SETUP` — record position to byte position

```c
// Record position (DX:AX) * record_size = byte position
// Then byte position / sector_size = sector number, remainder = byte offset in sector
// Sector number >> clusshft = cluster number
uint32_t bytpos = (uint64_t)recpos * recsiz;  // may need 64-bit intermediate
uint16_t secpos  = (uint16_t)(bytpos / dpb->secsiz);
uint16_t bytsecpos = (uint16_t)(bytpos % dpb->secsiz);
uint16_t clusnum = secpos >> dpb->clusshft;
```

### Cluster chain traversal (`FNDCLUS`)

```c
// Skip 'count' clusters along the chain from the FCB's cached position.
// Returns the cluster at position 'count' from the start.
uint16_t fndclus(const uint8_t *fat, FCB *fcb, uint16_t count, uint16_t *out_pos) {
    uint16_t clus = fcb->lstclus;
    uint16_t pos  = fcb->cluspos;
    if (clus == 0 || count < pos) {
        clus = fcb->firclus;  pos = 0;
    }
    count -= pos;
    while (count-- && !fat_is_eof(clus)) {
        clus = fat_unpack(fat, clus);
        pos++;
    }
    *out_pos = pos;
    return clus;
}
```

### Date/time packing (`DATE16`)

```c
// Returns:  AX = packed date, DX = packed time
uint16_t pack_date(uint8_t year_minus_1980, uint8_t month, uint8_t day) {
    return ((uint16_t)(year_minus_1980) << 9) | ((uint16_t)month << 5) | day;
}
uint16_t pack_time(uint8_t hour, uint8_t minute, uint8_t second) {
    return ((uint16_t)hour << 11) | ((uint16_t)minute << 5) | (second >> 1);
}

void unpack_date(uint16_t d, uint8_t *y, uint8_t *m, uint8_t *day) {
    *y   = (d >> 9) & 0x7F;   // year - 1980
    *m   = (d >> 5) & 0x0F;
    *day = d & 0x1F;
}
void unpack_time(uint16_t t, uint8_t *h, uint8_t *m, uint8_t *s) {
    *h = (t >> 11) & 0x1F;
    *m = (t >> 5)  & 0x3F;
    *s = (t & 0x1F) << 1;
}
```

### Sector buffer management

The kernel keeps exactly **one** sector in a global buffer. Before reading a different sector, the dirty buffer is flushed:

```c
// Simplified BUFSEC logic
uint8_t *bufsec(DPB *dpb, uint16_t cluster, uint8_t sec_in_clus, bool write_mode) {
    uint16_t phys_sec = figrec(dpb, cluster, sec_in_clus);
    if (phys_sec != bufsecno || dpb->drvnum != bufdrvno) {
        if (dirtybuf) { dwrite(bufdrvbp, buffer, 1, bufsecno); dirtybuf = 0; }
        if (!write_mode || secpos <= valsec)  // need pre-read?
            dread(dpb, buffer, 1, phys_sec);
        bufsecno = phys_sec;
        bufdrvno = dpb->drvnum;
    }
    return buffer;
}
```

### String output (`OUTMES` / `PRTBUF`)

CP/M-style `$`-terminated strings:
```c
void outmes(const char *s) {
    while (*s != '$') out(*s++);
}
```

### Device name recognition (`DEVNAME`)

Device names are exactly 4 characters (`CON `, `AUX `, `PRN `, `NUL `, `LST `). The check ignores the extension (requires 2 blanks after the name):

```c
static const char devnames[][4] = {"PRN ","LST ","NUL ","AUX ","CON "};
#define NUMDEV 5

// Returns device index (0-based from end), or -1 if not a device
int devname_check(const uint8_t name[11]) {
    for (int i = 0; i < NUMDEV; i++) {
        if (memcmp(name, devnames[i], 4) == 0 &&
            name[4] == ' ' && name[5] == ' ')
            return NUMDEV - 1 - i;
    }
    return -1;
}
```

### `GETREC` — Compute record position from FCB extent/NR fields

The EXTENT field and NR (next record) together encode a 24-bit record number:
```c
// NR is bits 0-6, EXTENT low byte is bits 7-13, EXTENT high byte is bits 14-20
uint32_t getrec(const FCB *fcb) {
    uint16_t nr     = fcb->nr;
    uint16_t extent = fcb->extent;
    // NR bit 7 becomes bit 7 of record number; extent encodes the upper bits
    // From the assembly: AL=NR, DX=EXTENT; SHL AL,1; SHR DX,1; RCR AL,1
    uint32_t rec = ((uint32_t)(extent >> 1) << 8) | nr;
    return rec;
}
```

---

## 11. Assembly Idioms Quick Reference

```asm
XOR  AX, AX             ; ax = 0;            (fastest zero)
OR   AX, AX             ; flags = ax; (test for zero/sign without changing AX)
TEST BYTE PTR [X], -1   ; if (x != 0) ...    (-1 = 0xFF, tests all bits)
TEST BYTE PTR [X], 0C0H ; if (x & 0xC0) ...
CMP  AX, -1             ; if (ax == 0xFFFF) ...   (0xFFFF stored as -1)
NOT  AH                 ; ah = ~ah;          (toggle all bits; used for toggling flags)
MOV  AL, 0              ; al = 0;            (doesn't affect flags)
XOR  AL, AL             ; al = 0;            (affects flags)
AND  AL, 3FH            ; al &= 0x3F;        (mask out top 2 bits: devid clean)
OR   AL, 40H            ; al |= 0x40;        (set bit 6: mark file dirty)
MOV  BYTE PTR [X], -1   ; x = 0xFF;          (flag value meaning "invalid" or "all")
CMP  AX, WORD PTR [Y]   ; compare 16-bit memory
JS   label              ; if (result < 0)    (jump if sign bit set — used to check bit 7)
JNS  label              ; if (result >= 0)   (jump if bit 7 clear)
```

### Multiply by small constant without MUL

```asm
SHL  AX, 1              ; ax *= 2
SHL  AX, 1              ; ax *= 2  (total: *= 4)
; DPBSIZ=20: MUL AH where AH=20 — uses the 8-bit form: DX:AX = AL * AH
MOV  AH, DPBSIZ
MUL  AH                 ; AX = AL * 20  (DX not used; fits in AX)
```

### `CBW` to clear a register's high byte

```asm
XOR  AH, AH             ; ah = 0;  (one way)
CBW                     ; ax = sign_extend(al) — only correct if AL has no sign bit set!
```

`CBW` is often used to zero `AH` when it's known `AL < 128`. Use with care in C translation — it's not simply `ax = al`; it's `ax = (int16_t)(int8_t)al`.

---

## 12. Segment Layout → C Memory Map

At runtime, the MS-DOS binary occupies a contiguous block starting at a paragraph boundary. The groups and segments lay out as:

```
DOSGROUP:
    CODE      (code segment)      -- functions
    CONSTANTS (constants segment) -- string tables, flag bytes, drive tables
    DATA      (data segment)      -- per-call scratch, stacks, buffers
        ├── INBUF[128]           -- console line working buffer
        ├── CONBUF[131]          -- BUFIN buffer
        ├── name1/name2/name3    -- filename scratch
        ├── spsave, sssave       -- user stack save
        ├── per-transfer vars    -- recpos, bytpos, clusnum, ...
        ├── IOSTACK[128]         -- stack for functions 0-12
        ├── DSKSTACK[128]        -- stack for functions >12
        └── DIRBUF[secsiz]       -- directory sector buffer
            [BUFFER = DIRBUF + secsiz]      -- data sector buffer
            [DRVTAB = BUFFER + secsiz]      -- array of DPB structs
            [FAT buffers]                    -- one per logical drive
```

In a flat C port, allocate all of these as a single static block and compute offsets at init time, as the original `DOSINIT` does.

---

## 13. Assembler-Specific Notation

| ASM syntax | Meaning in C |
|------------|-------------|
| `DB 0, 1, 2` | `uint8_t[] = {0, 1, 2}` |
| `DW 1234H` | `uint16_t = 0x1234` |
| `LABEL DUP (?)` | uninitialized array |
| `EQU` | `#define` or `enum` constant |
| `IF IBM ... ENDIF` | `#if IBM ... #endif` |
| `OFFSET DOSGROUP:X` | `&x` (address of symbol) |
| `BYTE PTR [X]` | `*(uint8_t*)&x` |
| `WORD PTR [X]` | `*(uint16_t*)&x` |
| `DWORD PTR [X]` | `*(uint32_t*)&x` |
| `STRUC` / `ENDS` | `struct { ... }` |
| `=` (not `EQU`) | alias for current location, like a `const` computed at assembly time |
| `PROC FAR` / `ENDP` | function that uses far call/ret (CS pushed/popped) |
| `ASSUME CS:DOSGROUP` | tells assembler default segment for unlabeled references |
| `GROUP` | linker: place all listed segments contiguously |

---

## 14. Porting Checklist

- [ ] All 16-bit arithmetic that produces 32-bit results (MUL, DIV) must use `uint32_t` in C.
- [ ] Byte/word reads from structure offsets via BP → typed struct member access.
- [ ] FAT 12-bit pack/unpack must handle both even and odd cluster numbers.
- [ ] The sector buffer (`DIRBUF` / `BUFFER`) has a fixed size = `maxsec` bytes each; allocate dynamically based on drive geometry.
- [ ] The `SPSAVE`/`SSSAVE` pattern collapses: in C, just pass `STKPTRS*` into every handler.
- [ ] `CALL FAR PTR BIOSxxx` → function-pointer calls through a BIOS vtable struct.
- [ ] `INT 24H` (fatal error) → `setjmp`/`longjmp` or a callback function pointer.
- [ ] `XCHG AL, [BX]` with memory is atomic on 8086 but not relevant in C; use plain assignment.
- [ ] `STD`/`CLD` control string direction: in `MOVS` backwards (for overlapping memmove), use `memmove()`.
- [ ] The `LOOP` instruction decrements CX before checking: `while (--cx)` not `while (cx--)`.
- [ ] Error returns of `0xFF` (`-1` as uint8_t) map to `return -1` in an `int`-returning C function.
- [ ] `DELALL` flag (`0` vs `0xE5`) controls whether deleted directory entries are marked with `0xE5` (normal) or `0x00` (DEL *.* special case). Check this two-byte variable (`CREATING`/`DELALL`) accessed as a word.
