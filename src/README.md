# MS-DOS 1.25 C Port — Source Layout

Port of MS-DOS 1.25 (MSDOS.ASM + COMMAND.ASM) to C, with a POSIX CLI wrapper.

## Building

```sh
make          # produces ./msdos
make clean
```

## Creating a disk image

```sh
./msdos --format disk.img --720   # 720 KB (default)
./msdos --format disk.img --360   # 360 KB
./msdos --format disk.img --180   # 180 KB
```

## Running

```sh
./msdos disk.img              # one drive (A:)
./msdos disk.img drive_b.img  # two drives (A: B:)
```

The process reads the BPB from the boot sector of each image to configure the drive geometry. Images must be pre-formatted (see above) or created by an external tool (e.g., `mkfs.fat`).

## Source files

```
src/
  kernel/
    types.h      -- FCB, DPB, DIRENT structs; FAT12 constants
    bios.h       -- BIOS vtable (hardware interface)
    dos.h        -- dos_t: all DOS globals; dos_call(); dos_regs_t
    fat.h/c      -- FAT12 pack/unpack, cluster allocation/release
    disk.h/c     -- sector buffer, directory buffer, disk I/O wrappers
    fcb.h/c      -- FCB open/close/create/delete/rename, SRCHFRST/NXT, MAKEFCB
    fileio.h/c   -- SEQRD/WRT, RNDRD/WRT, BLKRD/WRT, FILESIZE, SETRNDREC
    chardev.h/c  -- console I/O system calls (fn 1–12)
    datetime.h/c -- date/time system calls (fn 2A–2E)
    kernel.h/c   -- dos_init(), dos_call() INT 21H dispatcher
  command/
    command.h/c  -- command interpreter (DIR, TYPE, COPY, DEL, REN, ...)
  host/
    bios_host.h/c -- POSIX implementation of bios_t; disk image I/O; termios
    main.c        -- entry point; --format mode; boots DOS
```

## Architecture

```
main.c
  └── host_bios_init()     sets up bios_t with POSIX I/O
  └── dos_init()           reads BPB from image(s), builds DPBs, reads FAT
  └── command_run()        COMMAND.COM interpreter loop
        └── dos_bufin()    reads a line of input (chardev layer → bios)
        └── command_exec() dispatches internal or external commands
              └── dos_call() for all FAT/file operations (INT 21H)
```

## Internal commands

DIR, TYPE, COPY, DEL/ERASE, REN/RENAME, DATE, TIME, VER, CLS, ECHO, PAUSE, REM, CHKDSK

## Limitations

- External .COM/.EXE execution is not supported (would require an x86 emulator).
- No subdirectory support (matching MS-DOS 1.x; no CD/MD/RD).
- FAT12 only; FAT16/FAT32 images will not load correctly.
- Printer (PRN/LST) output goes to stderr.
- AUX (auxiliary) port is a no-op.
