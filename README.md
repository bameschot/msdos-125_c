# MS-DOS 1.25 — C Port

A complete port of **MS-DOS version 1.25** from 8086 assembly to C, with a POSIX command-line wrapper that runs on modern systems.  The port covers the DOS kernel (`MSDOS.ASM`), the command processor (`COMMAND.ASM`), and replaces the hardware I/O layer (`IO.ASM`) with a host BIOS implementation backed by raw FAT12 disk image files.

---

## Attribution

The original MS-DOS source code was written by **Tim Paterson** at Seattle Computer Products and released by Microsoft.  The assembly sources in the [`v1.25/`](v1.25/) directory are archived from the official Microsoft MS-DOS repository:

> **[https://github.com/microsoft/ms-dos](https://github.com/microsoft/ms-dos)**

MS-DOS 1.25 was the first general OEM release of MS-DOS (March 3, 1982).  It corresponds to IBM PC DOS 1.1 with one change: a `0x00` end-of-directory marker was added to speed up directory searches.

The assembly sources were made available by Tim Paterson in a December 2013 email to the Computer History Museum (see [`v1.25/Tim_Paterson_16Dec2013_email.txt`](v1.25/Tim_Paterson_16Dec2013_email.txt)).

---

## What This Project Is

| Component | Original file | C port |
|-----------|--------------|--------|
| DOS kernel (FAT12, FCB API, INT 21H) | `v1.25/source/MSDOS.ASM` | `src/kernel/` |
| Command processor (shell) | `v1.25/source/COMMAND.ASM` | `src/command/` |
| Hardware I/O (BIOS layer) | `v1.25/source/IO.ASM` | `src/host/bios_host.c` |
| CLI wrapper / entry point | — | `src/host/main.c` |

The port targets **C11** and has no dependencies beyond the standard C library and POSIX termios.

---

## Building

```sh
make          # produces ./msdos
make clean
```

Tested on macOS and Linux with `clang` and `gcc`.

---

## Quick Start

### 1. Create a blank FAT12 disk image

```sh
./msdos --format disk.img --720   # 720 KB  (default)
./msdos --format disk.img --360   # 360 KB
./msdos --format disk.img --180   # 180 KB
```

### 2. Boot into the shell

```sh
./msdos disk.img
```

You will see:

```
MS-DOS version 1.25
Copyright 1981,82 Microsoft, Inc.

A>
```

### 3. Use multiple drives

```sh
./msdos drive_a.img drive_b.img   # A: and B:
```

---

## Shell Commands

| Command | Description |
|---------|-------------|
| `DIR [spec]` | List directory; wildcards supported |
| `TYPE file` | Display a text file (stops at Ctrl-Z) |
| `OPEN file` | Display any file as ASCII; shows every byte including non-printable |
| `COPY src dst` | Copy a file |
| `DEL spec` / `ERASE spec` | Delete files; wildcards supported |
| `REN old new` / `RENAME old new` | Rename a file; wildcards supported |
| `MKFILE name` | Create an empty (zero-byte) file |
| `CHKDSK` | Show total and free disk space |
| `DATE [mm-dd-yy]` | Display or set the date |
| `TIME [hh:mm[:ss]]` | Display or set the time |
| `VER` | Display version string |
| `CLS` | Clear the screen |
| `ECHO [text]` | Print text to the console |
| `PAUSE` | Wait for a keypress |
| `REM [text]` | Comment line (no-op) |
| `HELP [cmd]` | List commands or show usage for one command |
| `EXIT` | Quit the shell |
| `X:` | Switch current drive (e.g. `B:`) |

For full documentation see [`HELP.md`](HELP.md).

---

## OPEN Command

`OPEN` is an extension beyond the original MS-DOS 1.25 command set.  It opens any file and renders its entire contents as safe ASCII — unlike `TYPE`, it does not stop at a Ctrl-Z byte and it makes all non-printable bytes visible:

| Byte range | Displayed as |
|------------|-------------|
| `0x09` TAB | Spaces to next 8-column tab stop |
| `0x0A` LF  | Newline |
| `0x0D` CR  | Suppressed |
| `0x20`–`0x7E` | Character as-is |
| `0x7F` DEL | `^?` |
| `0x00`–`0x1F` other control chars | `^@` through `^_` (caret notation) |
| `0x80`–`0xFF` high bytes | `.` |

```
A>OPEN HELLO.TXT

--- HELLO.TXT ---
Hello from MS-DOS 1.25!
This is line two.
--- 45 bytes ---
```

---

## Architecture

```
src/
  kernel/
    types.h      -- FCB, DPB, DIRENT structs; FAT12 constants
    bios.h       -- Hardware abstraction interface (function-pointer vtable)
    dos.h        -- dos_t: all DOS globals; dos_call(); dos_regs_t
    fat.h/c      -- FAT12 pack/unpack, chain traversal, allocation, writeback
    disk.h/c     -- Sector buffer, directory buffer, low-level disk I/O
    fcb.h/c      -- OPEN, CLOSE, CREATE, DELETE, RENAME, SRCHFRST/NXT, MAKEFCB
    fileio.h/c   -- SEQRD/WRT, RNDRD/WRT, BLKRD/WRT, FILESIZE, SETRNDREC
    chardev.h/c  -- Console I/O system calls (INT 21H fn 01h–0Ch)
    datetime.h/c -- Date/time system calls (INT 21H fn 2Ah–2Eh)
    kernel.h/c   -- dos_init(), dos_call() — INT 21H dispatcher (fn 00h–2Eh)
  command/
    command.h/c  -- Command interpreter loop and all internal commands
  host/
    bios_host.h/c -- POSIX implementation: termios console, disk image I/O
    main.c        -- Entry point; --format mode; boots DOS
```

### How it boots

```
main()
  └─ host_bios_init()     reads BPB from each .img file; configures bios_t vtable
  └─ dos_init()           builds DPBs from BPB data; reads FAT for drive A:
  └─ command_run()        prints banner; loops reading commands via dos_bufin()
        └─ command_exec() dispatches internal commands or reports external ones
              └─ dos_call() for all file/disk operations (INT 21H equivalent)
```

### INT 21H system calls implemented

All 47 functions from the original MS-DOS 1.25 are present (fn `00h`–`2Eh`):

- **00h–0Ch** — Character I/O (CONIN, CONOUT, BUFIN, RAWIO, PRTBUF, …)
- **0Dh–1Fh** — Disk and FCB management (OPEN, CLOSE, CREATE, DELETE, RENAME, SRCHFRST, SRCHNXT, SEQRD, SEQWRT, SETDMA, GETFATPT, …)
- **21h–28h** — Random and block file I/O (RNDRD, RNDWRT, BLKRD, BLKWRT, FILESIZE, SETRNDREC)
- **29h–2Eh** — Utilities (MAKEFCB, GETDATE, SETDATE, GETTIME, SETTIME, VERIFY)

---

## Disk Image Format

Images are raw FAT12 sector dumps — the same format written by the original MS-DOS `FORMAT` command.  Any standard FAT12 `.img` file can be used.

The `--format` flag creates a fresh image with a correct BPB, two FAT copies, and an empty root directory:

| Flag | Capacity | Sectors | Heads | Sec/Track |
|------|----------|---------|-------|-----------|
| `--180` | 180 KB | 360 | 1 | 9 |
| `--360` | 360 KB | 720 | 2 | 9 |
| `--720` | 720 KB | 1440 | 2 | 9 |

---

## Limitations

- **No external command execution.** `.COM` and `.EXE` files on the disk can be found by the shell, but cannot be run — that would require an x86 emulator.
- **No subdirectories.** MS-DOS 1.x had a flat root-only directory; `CD`, `MD`, and `RD` do not exist.
- **FAT12 only.** FAT16/FAT32 images will not load.
- **Single disk buffer.** As in the original, only one sector is cached in RAM at a time.
- **Printer output** goes to `stderr` on the host.
- **AUX port** is a no-op.

---

## Reference Documents

| File | Description |
|------|-------------|
| [`v1.25/CLAUDE.md`](v1.25/CLAUDE.md) | Annotated walkthrough of every source file in the original v1.25 assembly |
| [`v1.25/8086_to_c_reference.md`](v1.25/8086_to_c_reference.md) | 8086 → C porting reference: registers, instructions, data structures, idioms |
| [`HELP.md`](HELP.md) | Full command and INT 21H system call reference for this port |
| [`src/README.md`](src/README.md) | Source layout and build notes |

---

## License

The original MS-DOS source code (`v1.25/`) is © Microsoft Corporation and is made available under the terms of the [MIT License](https://github.com/microsoft/ms-dos/blob/master/LICENSE).

The C port in `src/` is an independent reimplementation written from scratch using the assembly as a specification, also released under the MIT License.
