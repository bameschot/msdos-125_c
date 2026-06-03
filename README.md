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
make demo     # builds demo.img — a 720 KB disk pre-loaded with BASIC programs
make test     # runs all 461 unit and integration tests
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

## Demo Disk

A ready-to-boot 720 KB disk image pre-loaded with BASIC programs and documentation is included.  Build it with:

```sh
make demo          # creates demo.img  (requires ./msdos — run make first)
./msdos demo.img   # boot into it
```

Once inside, start with:

```
A>TYPE README.TXT      read the quick-start guide
A>TYPE OS.TXT          full OS command reference
A>TYPE BASIC.TXT       BASIC interpreter language guide
A>DIR                  list everything on the disk
```

### BASIC programs on the demo disk

| File | Description |
|------|-------------|
| `FIB.BAS` | Fibonacci sequence |
| `GUESS.BAS` | Number guessing game |
| `CALC.BAS` | 4-function calculator with memory |
| `TIMES.BAS` | Multiplication tables |
| `HAMURABI.BAS` | Govern ancient Sumeria — classic 1973 strategy game |
| `PRIMES.BAS` | List all prime numbers up to a given limit |
| `CONVERT.BAS` | Unit converter (temperature, length, weight) |
| `LOAN.BAS` | Monthly loan payment calculator |
| `STATS.BAS` | Descriptive statistics: count, mean, min, max, range, std dev |
| `QUIZ.BAS` | Mental arithmetic quiz — addition, subtraction, multiplication, division |

Run any program with `BASIC filename.BAS`, edit one with `EDIT filename.BAS`.

The disk image is produced by [`basic/make_disk.py`](basic/make_disk.py), which formats a blank image via the `msdos` binary and then writes all files by patching the raw FAT12 bytes directly from Python.

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
| `EDIT file` | Full-screen text editor — view, edit and save a file |
| `BASIC file` | Run a BASIC program file |
| `CREATE name` | Create an empty (zero-byte) file |
| `MKDIR dir` / `MD dir` | Create a subdirectory |
| `RMDIR dir` / `RD dir` | Remove an empty subdirectory |
| `CD [path]` / `CHDIR [path]` | Display or change the current directory |
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

## EDIT Command

`EDIT` is a full-screen terminal text editor backed by the FAT12 disk layer.  It opens any text file for viewing and editing, and writes the result back to the disk image on save.  A new file is created if the name does not yet exist.

```
A>EDIT NOTES.TXT
```

```
 EDIT: NOTES.TXT      Ln 1     Col 1     [No changes]
 ──────────────────────────────────────────────────────
 file content here…



 ──────────────────────────────────────────────────────
 ^W Save  ^X Quit  ^G Goto  Arrows Move  Home/End  PgUp/PgDn  Del/BS Delete  Esc Quit
```

**Key bindings:**

| Key | Action |
|-----|--------|
| Arrow keys | Move cursor |
| Home / End | Start / end of current line |
| Ctrl+Home / Ctrl+End | Start / end of file |
| Page Up / Page Down | Scroll one screen |
| Backspace | Delete character before cursor; join lines at column 0 |
| Delete | Delete character at cursor; join with next line at end of line |
| Enter | Insert line break |
| Tab | Insert 4 spaces |
| **Ctrl+W** | **Save file** (Write) |
| **Ctrl+X** / Esc | **Quit** — prompts if there are unsaved changes |
| Ctrl+G | Go to a specific line number |

> **Note on control keys:** `Ctrl+S` (XOFF) and `Ctrl+Q` (XON) are the POSIX flow-control characters and may be intercepted by the terminal before they reach the application.  The editor uses `Ctrl+W` and `Ctrl+X` instead, which have no terminal meaning and are always delivered in raw mode.

---

## BASIC Command

`BASIC` runs a line-numbered BASIC source file (`.BAS`) stored on the FAT12 disk image.  It is a pure C interpreter — no x86 code is generated or executed.

---

### Writing and running your first program

#### Step 1 — create and edit the file

```
A>EDIT HELLO.BAS
```

The full-screen editor opens.  Type your program, then press **Ctrl+W** to save and **Ctrl+X** to quit.

#### Step 2 — run it

```
A>BASIC HELLO.BAS
```

#### Step 3 — edit and re-run

```
A>EDIT HELLO.BAS       ← make changes
A>BASIC HELLO.BAS      ← run again
```

You can also keep your programs organised in subdirectories:

```
A>MKDIR PROGRAMS
A>CD PROGRAMS
A:\PROGRAMS>EDIT HELLO.BAS
A:\PROGRAMS>BASIC HELLO.BAS
```

---

### Syntax reference

Every line begins with a **line number** (1–65535).  The interpreter executes lines in ascending numeric order.  Lines are separated by newlines; multiple statements on one line are separated by `:`.

```
10 REM this is a comment
20 PRINT "Hello"; " "; "World"   : REM inline second statement
```

#### Variables

| Kind | Names | Capacity |
|------|-------|---------|
| Numeric | `A` – `Z` | 64-bit float (`double`) |
| String | `A$` – `Z$` | Up to 255 characters |

Variables are global and zero / empty-string initialised.  There are no arrays.

#### Statements

| Statement | Example | Notes |
|-----------|---------|-------|
| `REM` | `10 REM comment` | Comment; `'` is also accepted |
| `PRINT` | `10 PRINT "x = "; X` | `;` joins with no gap, `,` advances to next 14-column tab stop; trailing `;` suppresses the newline |
| `LET` | `10 LET A = A + 1` | `LET` is optional — `10 A = A + 1` works too |
| `INPUT` | `10 INPUT "Name: "; N$` | Prints optional prompt, reads a line; backspace works |
| `IF…THEN` | `10 IF X > 0 THEN 50` | Jumps to line 50 when true |
| `IF…THEN` | `10 IF X > 0 THEN PRINT "pos"` | Executes inline statement when true |
| `GOTO` | `10 GOTO 100` | Unconditional jump |
| `GOSUB` | `10 GOSUB 500` | Call subroutine at line 500 (stack depth 32) |
| `RETURN` | `500 RETURN` | Return from subroutine |
| `FOR…NEXT` | `10 FOR I = 1 TO 10 STEP 2` | Loop; `STEP` defaults to 1; nest up to 16 deep |
| `NEXT` | `20 NEXT I` | End of loop body |
| `END` / `STOP` | `99 END` | Halt program |

#### Operators

| Category | Symbols | Notes |
|----------|---------|-------|
| Arithmetic | `+  -  *  /  ^` | `^` = power; standard precedence |
| String | `+` | Concatenation when both sides are strings |
| Comparison | `=  <>  <  >  <=  >=` | Return `-1` (true) or `0` (false) |
| Logical | `AND  OR  NOT` | Operate on numeric truth values |

Parentheses can be used freely: `(A + B) * C`.

#### Built-in functions

| Function | Returns | Example |
|----------|---------|---------|
| `INT(x)` | Floor toward −∞ | `INT(3.9)` → `3`, `INT(-1.1)` → `-2` |
| `ABS(x)` | Absolute value | `ABS(-5)` → `5` |
| `SQR(x)` | Square root | `SQR(9)` → `3` |
| `RND(x)` | Random float 0 ≤ r < 1 | `INT(RND(1)*6)+1` — dice roll |
| `LEN(s$)` | String length | `LEN("hi")` → `2` |
| `ASC(s$)` | ASCII code of first char | `ASC("A")` → `65` |
| `VAL(s$)` | Parse string as number | `VAL("42")` → `42` |
| `CHR$(n)` | Character from ASCII code | `CHR$(65)` → `"A"` |
| `STR$(n)` | Number to string | `STR$(3.14)` → `"3.14"` |
| `LEFT$(s$, n)` | First n characters | `LEFT$("Hello", 3)` → `"Hel"` |
| `RIGHT$(s$, n)` | Last n characters | `RIGHT$("Hello", 3)` → `"llo"` |
| `MID$(s$, start[, len])` | Substring (1-based) | `MID$("Hello", 2, 3)` → `"ell"` |

---

### Example programs

#### Hello World

```basic
10 PRINT "Hello, World!"
20 END
```

#### Fibonacci sequence

```basic
10 LET A = 0
20 LET B = 1
30 FOR I = 1 TO 10
40   PRINT A
50   LET C = A + B
60   LET A = B
70   LET B = C
80 NEXT I
90 END
```

#### Number guessing game

```basic
10 LET S = INT(RND(1) * 100) + 1
20 PRINT "Guess a number between 1 and 100"
30 INPUT "Your guess: "; G
40 IF G = S THEN 90
50 IF G < S THEN PRINT "Too low"
60 IF G > S THEN PRINT "Too high"
70 GOTO 30
90 PRINT "Correct!"
100 END
```

#### Subroutine example

```basic
10 FOR I = 1 TO 3
20   GOSUB 100
30 NEXT I
40 END
100 PRINT "Hello from subroutine #"; I
110 RETURN
```

#### String operations

```basic
10 INPUT "Enter your name: "; N$
20 PRINT "Hello, "; N$; "!"
30 PRINT "Your name has "; LEN(N$); " characters"
40 PRINT "First letter: "; LEFT$(N$, 1)
50 END
```

---

### Errors

Errors are reported as `?MESSAGE IN line N` and execution halts immediately:

```
?UNDEFINED LINE 999 IN 50
?DIVISION BY ZERO IN 30
?TYPE MISMATCH IN 20
?SYNTAX ERROR IN 10
?GOSUB OVERFLOW IN 40
?NEXT WITHOUT FOR IN 70
```

**Not supported:** arrays, `DATA`/`READ`/`RESTORE`, `WHILE`/`WEND`, `DEF FN`, `ON GOTO`/`GOSUB`.

---

## Subdirectory Navigation

`MKDIR`, `RMDIR`, `CD`, and their aliases (`MD`, `RD`, `CHDIR`) extend the original flat MS-DOS 1.25 filesystem with full subdirectory support.  Directories can be nested to arbitrary depth.  All file commands (`DIR`, `TYPE`, `COPY`, `DEL`, `EDIT`, `CREATE`, …) operate on the **current directory** automatically.

```
A>MKDIR DOCS
A>CD DOCS
A:\DOCS>MKDIR WORK
A:\DOCS>CD WORK
A:\DOCS\WORK>CREATE NOTE.TXT
A:\DOCS\WORK>CD ..
A:\DOCS>CD ..
A>
```

| Command | Action |
|---------|--------|
| `MKDIR name` / `MD name` | Create a subdirectory in the current directory |
| `RMDIR name` / `RD name` | Remove an empty subdirectory |
| `CD name` | Enter a subdirectory |
| `CD ..` | Go up one level |
| `CD \` | Return to root |
| `CD` | Print the current path |

The shell prompt reflects the current location:

```
A>              ← root
A:\DOCS>        ← inside DOCS
A:\DOCS\WORK>   ← nested inside WORK
```

The kernel's directory layer (`disk_dirread`, `findname_impl`) is fully context-aware: the current directory cluster is stored in `dos->curdir_clus[drive]` and consulted on every directory operation, so no command-level changes were needed to make file ops subdirectory-aware.

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
    edit.h/c     -- Full-screen text editor (EDIT command)
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
- **Subdirectory depth** is limited in practice by the root directory entry count (112 entries for 720 KB images) and by available clusters.
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
