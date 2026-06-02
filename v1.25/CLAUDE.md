# MS-DOS 1.25 — Source Code Overview

This folder contains the complete source and binary distribution of **MS-DOS version 1.25**, the first general OEM release of MS-DOS, dated **March 3, 1982**. The code was written entirely in **8086 assembly** by Tim Paterson at Seattle Computer Products (SCP). It was donated to the Computer History Museum and described in a Dec 16, 2013 email from Paterson to CHM curator Len Shustek (see `Tim_Paterson_16Dec2013_email.txt`).

MS-DOS 1.25 corresponds to IBM PC DOS 1.1 (which was MS-DOS 1.24 with one minor addition: a `00` end-of-directory marker added to speed up directory searches).

---

## Directory Layout

```
v1.25/
├── Tim_Paterson_16Dec2013_email.txt   # Provenance note from Tim Paterson
├── source/                            # 8086 assembly source files
│   ├── STDDOS.ASM                     # Top-level build entry point (22 lines)
│   ├── MSDOS.ASM                      # DOS kernel (4,030 lines)
│   ├── COMMAND.ASM                    # Command processor (2,165 lines)
│   ├── IO.ASM                         # SCP-specific I/O / BIOS layer (1,933 lines)
│   ├── ASM.ASM                        # SCP 8086 assembler v2.44 (4,005 lines)
│   ├── TRANS.ASM                      # Z80-to-8086 source translator v2.21 (1,212 lines)
│   └── HEX2BIN.ASM                    # Intel HEX-to-binary converter v1.02 (213 lines)
└── bin/                               # Pre-assembled binaries shipped to OEMs
    ├── COMMAND.COM                    # Command interpreter
    ├── CHKDSK.COM / COMP.COM / ...    # External DOS utilities
    ├── EDLIN.COM                      # Line editor
    ├── DEBUG.COM                      # Debugger
    ├── FORMAT.COM / DISKCOPY.COM ...  # Disk utilities
    ├── LINK.EXE / EXE2BIN.EXE        # Linker and EXE-to-COM converter
    ├── BASIC.COM / BASICA.COM         # Microsoft BASIC interpreters
    └── *.BAS                          # Sample BASIC programs
```

---

## Source Files

### `STDDOS.ASM` — Build Entry Point
A 22-line wrapper that sets assembly-time boolean switches and then `INCLUDE`s `MSDOS.ASM`. Key flags:
- `MSVER = TRUE`, `IBM = FALSE` — builds the standard MS-DOS variant (not IBM PC DOS)
- `HIGHMEM = FALSE` — DOS loads at low memory
- `DSKTEST = FALSE` — disk debug re-entrancy mode off

The IBM vs. MS-DOS split controls keyboard escape sequences (IBM uses function-key scan codes; SCP/MS uses letter-based escape sequences), the command prompt character (`>` vs `:`), and whether the extent field in FCBs is zeroed on open.

### `MSDOS.ASM` — DOS Kernel
The heart of the operating system. 4,030 lines of 8086 assembly implementing:

**Interrupt / call dispatch**
- `INT 20H` (`QUIT`) — program terminate
- `INT 21H` (`COMMAND`) — the main system call interrupt, dispatching on `AH` (0–46 decimal)
- CP/M-compatible `CALL 5` entry point (`ENTRY`) — remaps `CL` to `AH` and rejoins the interrupt path
- `INT 24H` — fatal error handler (`FATAL`/`FATAL1`/`HARDERR`)

**System call table** (`DISPATCH`, functions 0–46):
| Range | Description |
|-------|-------------|
| 0–12  | Character I/O (console in/out, printer, aux, buffered input, status) |
| 13–18 | Disk / FCB management (reset, select, open, close, search first/next) |
| 19–28 | FCB file operations (delete, sequential read/write, create, rename, random I/O, file size) |
| 29–35 | Drive info, DMA address, FAT pointer, random record positioning |
| 36–46 | Extended functions: set vector, new base segment, block read/write, FCB parse, date/time, verify |

**FAT filesystem implementation**
- 12-bit FAT entries stored in packed 3-byte pairs; `UNPACK` / `PACK` handle the odd/even cluster arithmetic
- `FATREAD` / `FATWRT` — read/write the File Allocation Table, with multiple FAT copy support
- `FIGFAT` / `DIRREAD` / `DIRWRITE` — directory sector buffering
- `FNDCLUS` — cluster chain traversal using cached `LSTCLUS`/`CLUSPOS` fields in the FCB to avoid re-walking from the start
- `ALLOCATE` / `RELEASE` — cluster allocation and freeing

**32-byte directory entries** (as documented inline):
- Bytes 0–10: filename + extension
- Byte 11: attributes (bits 1–2 = hidden)
- Bytes 22–23: time (H:M:S/2 packed)
- Bytes 24–25: date (Y-1980/M/D packed)
- Bytes 26–27: first cluster
- Bytes 28–31: file size in bytes

**FCB structure** (`FCBLOCK`): drive, name, extent, record size, file size, date/time, device ID, first/last cluster, cluster position, next record, random record.

**Drive Parameter Block** (`DPBLOCK`): device number, physical unit, sector size, cluster mask/shift, FAT location and count, directory entries, first data sector, max cluster count, FAT size, FAT pointer.

**I/O buffering**: single-sector disk buffer with dirty flag; reads are suppressed on full-sector writes (`VALSEC` tracking). Buffer is flushed on drive switch or close.

**Device names**: `CON`, `AUX`, `PRN`, `NUL`, `COM1` (IBM only) are recognized in directory searches (`DEVNAME`/`IOCHK`) and routed to BIOS character I/O routines rather than disk I/O.

**Error handling**: `HARDERR` identifies which disk area (reserved/FAT/directory/data) caused the error and invokes `INT 24H` with a structured error code. The user stack is restored before the interrupt so the handler runs in user context.

**BIOS interface** (segment `SEGBIOS` at `40H` for SCP / `60H` for IBM): far-call stubs for console status/in/out, printer, aux in/out, disk read/write, disk-change detect, date/time get/set, console flush, and drive map.

### `COMMAND.ASM` — Command Processor
2,165 lines implementing the shell (`COMMAND.COM`). Structured in three resident/init/transient segments:

- **Resident portion** (`CODERES`/`DATARES`): interrupt handlers for `INT 22H` (terminate), `INT 23H` (Ctrl-C), `INT 24H` (fatal error), `INT 27H` (TSR stay-resident). Also contains the checksum routine that detects whether the transient has been overwritten and reloads it from disk if needed.
- **Init code** (`INIT`): run once at startup, then discarded.
- **Transient portion** (`TRANCODE`/`TRANDATA`/`TRANSPACE`): all command parsing and execution — internal commands (DIR, COPY, DEL, REN, TYPE, ECHO, PAUSE, REM, DATE, TIME, VER, CLS, etc.) and external program loading via `EXEC`-equivalent logic.

Assembly flags: `IBMVER`/`MSVER` switch the command prompt (`>`/`:`) and default drive for COMMAND.COM itself.

### `IO.ASM` — SCP Hardware BIOS (1,933 lines)
The hardware abstraction layer specific to Seattle Computer Products hardware. **Not used by OEMs**, who supplied their own equivalent. Covers:
- CPU Support card at I/O base `F0H` for character I/O (polled or interrupt-driven, selectable via `INTINP`)
- Multiport Serial card at `10H` for AUX/printer (selectable vs. parallel port)
- Disk controllers: SCP, Tarbell SD, Tarbell DD, Cromemco 4FDC, Cromemco 16FDC (assembly-time selection)
- 8-inch and 5.25-inch drive geometry (LARGE/COMBIN/SMALL/CUSTOM configurations)
- PerSci fast-seek support
- `CONVERT` mode: drives A/B in new Microsoft FAT format, C/D in old SCP format

Segments at `40H`, 2 KB maximum. Loaded at boot and resident below DOS.

### `ASM.ASM` — SCP 8086 Assembler v2.44 (4,005 lines)
Tim Paterson's own 8086 assembler, used to assemble earlier versions of DOS before Microsoft's MASM was available. Features:
- Two-pass assembler producing Intel HEX or binary output
- Supports `IF`/`ENDIF` (nested), `EQU`, `ORG`, `PUT`, `DB`/`DW`, all 8086 mnemonics
- 8087 FPU mnemonics added in v2.40 (Nov 1981), with Intel "reverse-bit" bug fix in v2.41
- 1 KB source buffer
- Produces `.PRN` listing files and `.HEX` output

### `HEX2BIN.ASM` — Intel HEX Converter v1.02 (213 lines)
Small utility to convert Intel HEX-format files to raw binary. Used in the build pipeline with the assembler above.

### `TRANS.ASM` — Z80-to-8086 Translator v2.21 (1,212 lines)
Automated source-level translator from Z80 assembly to 8086 assembly, developed at SCP to port the original QDOS/86-DOS from an earlier Z80 codebase. Runs under 86-DOS.

---

## Binary Distribution (`bin/`)

Pre-assembled binaries that were shipped to OEM customers:

| File | Description |
|------|-------------|
| `COMMAND.COM` | Shell / command interpreter |
| `CHKDSK.COM` | Disk and FAT integrity checker |
| `COMP.COM` | File compare |
| `DEBUG.COM` | Machine-level debugger |
| `DISKCOMP.COM` | Disk compare |
| `DISKCOPY.COM` | Disk copy |
| `EDLIN.COM` | Line-oriented text editor |
| `FORMAT.COM` | Disk formatter |
| `MODE.COM` | Console/printer mode control |
| `SETCLOCK.COM` | Hardware clock setter |
| `SYS.COM` | Transfer system files to a disk |
| `EXE2BIN.EXE` | Convert `.EXE` to `.COM` format |
| `LINK.EXE` | Object file linker |
| `BASIC.COM` / `BASICA.COM` | Microsoft BASIC interpreters |
| `*.BAS` | Sample BASIC programs (ART, BALL, CALENDAR, CIRCLE, COLORBAR, COMM, DONKEY, MORTGAGE, MUSIC, PIECHART, SAMPLES, SPACE) |

---

## Key Design Points

- **Dual-mode assembly**: a single `MSDOS.ASM` source produces either the standard MS-DOS build or the IBM PC DOS build by toggling `IBM`/`MSVER` boolean equates. Differences include keyboard handling, prompt character, FCB extent zeroing, number of named devices, and BIOS segment address.
- **CP/M compatibility**: the `CALL 5` entry point and FCB-based API (functions 0–35) mirror CP/M 2.2 semantics deliberately, easing software porting. The `ENTRY` dispatcher remaps the CP/M `CL` function number to `AH` and merges with the INT 21H path.
- **Single-tasking, single-segment DOS**: the entire kernel lives in one `DOSGROUP` combining `CODE`, `CONSTANTS`, and `DATA` segments. `DS=CS=ES=SS=DOSGROUP` during kernel execution; the user's stack is saved/restored across every system call.
- **12-bit FAT**: supports up to 4,080 clusters per volume (entries `> 0FF8H` = EOF, `0` = free). Cluster 0 is reserved as EOF trap, cluster 1 reserved for future use, data begins at cluster 2.
- **No memory management**: no `EXEC` (function 4BH), no `ALLOC`/`FREE` (functions 48H/49H) — those arrived in DOS 2.0. Programs are simply loaded at a fixed address.
- **Version 1.25 change**: the only difference from 1.24 (IBM PC DOS 1.1) is writing a `00` byte at the end of the directory to allow early termination of directory searches, improving performance.
