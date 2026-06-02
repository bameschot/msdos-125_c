# MS-DOS 1.25 C Port — Command Reference

This document covers every command, system call, and convention supported by
the MS-DOS 1.25 C port.  It is divided into four parts:

1. **Using the shell** — the command prompt, file naming, wildcards
2. **Internal commands** — commands built into COMMAND.COM
3. **System-level conventions** — FCB layout, FAT12, device names
4. **INT 21H system call reference** — all 47 DOS functions (for programs)

---

## Part 1 — Using the Shell

### The command prompt

After booting, the shell displays a prompt showing the current drive letter:

```
A>
```

Type a command and press **Enter**.  Commands are not case-sensitive.

### Changing drives

To switch to a different drive, type the drive letter followed by a colon:

```
A>B:
B>A:
```

### File naming

MS-DOS 1.25 uses the **8.3 format**: up to eight characters for the filename,
a period, and up to three characters for the extension.

| Character class | Allowed in name/extension |
|-----------------|--------------------------|
| Letters A–Z     | Yes (stored upper-case)  |
| Digits 0–9      | Yes                      |
| `$ % ' - _ @ ~ ! ( ) { }` | Yes         |
| Space, `.` `"` `/` `[` `]` `:` `+` `=` `;` `,` | No (delimiters) |
| Control characters | No                   |

Examples of valid filenames: `AUTOEXEC.BAT`, `MSDOS.SYS`, `README.TXT`,
`DATA`, `FILE1`.

### Wildcards

Two wildcard characters may appear in a filename argument:

| Wildcard | Meaning |
|----------|---------|
| `?`      | Matches exactly one character in that position |
| `*`      | Matches any sequence of characters from that position to the end of the name or extension field |

`*.*` matches every file.  `*.BAS` matches every file with extension `BAS`.
`FILE?.TXT` matches `FILE1.TXT`, `FILEA.TXT`, etc.

### Drive specifier in a filename

Most commands accept an optional drive letter prefix:

```
A>TYPE B:README.TXT
A>DIR  B:*.BAS
```

If omitted, the current drive is used.

### Key combinations at the prompt

| Key        | Action |
|------------|--------|
| **Backspace** | Erase the last character typed |
| **Ctrl-X** or **Esc** | Cancel the current line (prints `\` and starts a new one) |
| **Ctrl-C** | Abort the current command |
| **Ctrl-S** | Pause output; any key resumes |
| **Ctrl-P** | Toggle printer echo (copy console output to printer) |

---

## Part 2 — Internal Commands

All commands listed below are built into the shell (`COMMAND.COM`) and are
always available regardless of what is on the disk.

---

### CLS — Clear Screen

```
CLS
```

Clears the console screen and moves the cursor to the top-left corner.

---

### CHKDSK — Check Disk

```
CHKDSK
```

Reports the total disk capacity and the number of bytes currently free.
In this port, CHKDSK operates on the current drive and does not repair
the FAT or directory.

**Output example:**

```
   730112 bytes total disk space
   729088 bytes available on disk
```

---

### COPY — Copy Files

```
COPY source destination
```

Copies a file from `source` to `destination`.  Both arguments are required.
Wildcards are **not** supported in this version; specify exact filenames.

**Examples:**

```
COPY README.TXT BACKUP.TXT
COPY B:DATA.DAT A:DATA.DAT
```

**Notes:**
- If the destination file already exists it is overwritten.
- Copying a file to itself is not detected and will corrupt the copy.
- 128 bytes are transferred per record; the destination file size matches
  the source exactly.

---

### DATE — Display or Set the Date

```
DATE
DATE mm-dd-yy
```

With no argument, displays the current date and prompts for a new one.
Press **Enter** at the prompt to leave the date unchanged.

With an argument, sets the date immediately without prompting.

**Date format:** `mm-dd-yy`  (month, day, two-digit year)

- Two-digit years 80–99 are interpreted as 1980–1999.
- Two-digit years 00–79 are interpreted as 2000–2079.

**Examples:**

```
DATE                   ← display and optionally set
DATE 06-15-82          ← set to June 15, 1982
DATE 12-31-99          ← set to December 31, 1999
```

**Output example:**

```
Current date is Mon  6- 2-2026
Enter new date (mm-dd-yy):
```

---

### DEL / ERASE — Delete Files

```
DEL   filespec
ERASE filespec
```

Deletes one or more files matching `filespec`.  Wildcards are supported.

When `filespec` is `*.*`, the shell asks for confirmation:

```
Are you sure (Y/N)?
```

Type **Y** and press Enter to proceed; any other key cancels.

**Examples:**

```
DEL TEMP.TXT
ERASE *.BAK
DEL *.*
```

**Notes:**
- Deleted files cannot be recovered in this version.
- The disk space occupied by the file is returned to the free pool
  immediately.
- Read-only or hidden files are matched normally (MS-DOS 1.x has no
  read-only enforcement at the shell level).

---

### DIR — List Directory

```
DIR [filespec]
```

Lists the files in the root directory of the current (or specified) drive.

With no argument, lists all non-hidden files.  With a `filespec`, lists only
matching files; wildcards are supported.

**Output columns:** filename, `<DIR>` or file size in bytes, last-modified
date (`mm-dd-yy`), last-modified time (`hh:mm`).

**Summary line:** number of files listed, total bytes used, bytes free on disk.

**Examples:**

```
DIR
DIR *.BAS
DIR B:
DIR B:*.TXT
```

**Output example:**

```
 Directory of  A:\

README   TXT        512 06- 2-26 14:30
AUTOEXEC BAT        128 06- 1-26  9:00
   2 File(s)        640 bytes
    728448 bytes free
```

---

### ECHO — Display a Message

```
ECHO [message]
ECHO ON
ECHO OFF
```

With a message argument, prints that text to the console followed by a
newline.  `ECHO ON` and `ECHO OFF` are accepted but have no effect in this
port (command echoing is always on).  `ECHO` with no argument does nothing.

**Examples:**

```
ECHO Hello, World!
ECHO This is MS-DOS 1.25
```

---

### EXIT — Quit the Shell

```
EXIT
```

Terminates the command interpreter and returns control to the host
environment.  Any dirty disk buffers and FAT copies are written back to
disk before exiting.

---

### OPEN — Display File Contents as ASCII

```
OPEN filename
```

Opens a file and displays its full contents on the console, rendering every
byte as a safe ASCII representation.  Unlike `TYPE`, `OPEN` does **not** stop
at a Ctrl-Z (0x1A) byte, so the entire file is always shown.

**Byte rendering rules:**

| Byte range | Displayed as |
|------------|-------------|
| `0x09` TAB | Spaces to the next 8-column tab stop |
| `0x0A` LF  | Newline |
| `0x0D` CR  | Suppressed (LF drives line breaks) |
| `0x20`–`0x7E` | Character as-is (printable ASCII) |
| `0x7F` DEL | `^?` |
| `0x00`–`0x1F` other control chars | Caret notation: `^@` `^A` … `^_` |
| `0x80`–`0xFF` high bytes | `.` (non-ASCII marker) |

A header line (`--- FILENAME ---`) is printed before the content, and a
footer (`--- N bytes ---`) after it.

**Examples:**

```
OPEN README.TXT
OPEN B:CONFIG.SYS
OPEN BINARY.COM
```

**Error conditions:**

| Message | Cause |
|---------|-------|
| `Required parameter missing` | No filename supplied |
| `File not found` | File does not exist on the current drive |

---

### PAUSE — Wait for a Keypress

```
PAUSE
```

Prints `Strike a key when ready . . .` and waits for the user to press any
key before continuing.

---

### MKFILE — Create an Empty File

```
MKFILE filename
```

Creates a new, empty (zero-byte) file with the given name in the current
directory of the current drive.  If a file with that name already exists it
is truncated to zero bytes (equivalent to `CREATE` followed by `CLOSE`).

Wildcards (`?`, `*`) are not permitted.  The name must follow the 8.3
convention.

**Examples:**

```
MKFILE NOTES.TXT
MKFILE DATA.DAT
MKFILE B:EMPTY.BIN
```

**Error conditions:**

| Message | Cause |
|---------|-------|
| `Required parameter missing` | No filename supplied |
| `Invalid file name` | Name contains illegal characters |
| `Wildcards not allowed` | Name contains `?` or `*` |
| `Cannot create file` | Disk full or root directory full |

---

### REM — Comment

```
REM [text]
```

Does nothing.  Used to embed comments in batch-style input.

**Example:**

```
REM This line is ignored
```

---

### REN / RENAME — Rename a File

```
REN   oldname newname
RENAME oldname newname
```

Renames a file.  The new name must not already exist on the same disk.
Wildcards are supported in both names; a `?` in `newname` preserves the
corresponding character from `oldname`.

**Examples:**

```
REN OLDFILE.TXT NEWFILE.TXT
REN *.BAK *.OLD
```

---

### TIME — Display or Set the Time

```
TIME
TIME hh:mm[:ss[.cc]]
```

With no argument, displays the current time and prompts for a new one.
Press **Enter** to leave the time unchanged.

With an argument, sets the time immediately.

**Time format:** `hh:mm` or `hh:mm:ss` or `hh:mm:ss.cc`

- `hh` = hours (0–23), 24-hour format.
- `mm` = minutes (0–59).
- `ss` = seconds (0–59), optional.
- `cc` = hundredths of a second (0–99), optional.

**Examples:**

```
TIME                   ← display and optionally set
TIME 14:30             ← set to 14:30:00
TIME  9:05:30.00       ← set to 09:05:30
```

**Output example:**

```
Current time is 14:30:00.00
Enter new time:
```

---

### TYPE — Display a File

```
TYPE filename
```

Reads a file and writes its contents to the console.  A **Ctrl-Z** (0x1A)
byte in the file is treated as end-of-file (CP/M and MS-DOS convention).

**Example:**

```
TYPE README.TXT
TYPE B:AUTOEXEC.BAT
```

---

### VER — Display Version

```
VER
```

Prints the DOS version string.

**Output:**

```
MS-DOS version 1.25
Copyright 1981,82 Microsoft, Inc.
```

---

### HELP — Display Command List

```
HELP
HELP command
```

With no argument, lists all available internal commands with a one-line
description.  With a command name, prints a brief usage summary for that
command.

---

## Part 3 — System-Level Conventions

### Named I/O Devices

The following names are recognised as I/O devices wherever a filename is
expected.  They can be opened, read from, or written to using FCB calls.

| Device | Description |
|--------|-------------|
| `CON`  | Console: keyboard input / screen output |
| `AUX`  | Auxiliary (serial) port |
| `PRN`  | Printer (parallel) port |
| `LST`  | Printer — alias for PRN |
| `NUL`  | Null device: reads always return Ctrl-Z; writes are discarded |

### FCB — File Control Block (37 bytes)

Programs that use INT 21H FCB calls must supply a 37-byte FCB at a known
address (commonly `DS:005CH` in the PSP).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | Drive | 0=default, 1=A, 2=B, … |
| 1 | 8 | Name | Filename, space-padded |
| 9 | 3 | Ext | Extension, space-padded |
| 12 | 2 | Extent | Current block/extent |
| 14 | 2 | RecSiz | Record size (default 128) |
| 16 | 4 | FilSiz | File size in bytes |
| 20 | 2 | FDate | Packed date |
| 22 | 2 | FTime | Packed time |
| 24 | 1 | DevId | Bit 7=device, bit 6=dirty/EOF |
| 25 | 2 | FirClus | First cluster |
| 27 | 2 | LstClus | Last cluster (cache) |
| 29 | 2 | ClusPos | Chain position of LstClus |
| 31 | 1 | (pad) | |
| 32 | 1 | NR | Next record (sequential) |
| 33 | 4 | RR | Random record (24-bit or 32-bit) |

**Extended FCB**: if byte 0 is `0xFF`, a 7-byte prefix precedes the normal
FCB.  Byte 6 of the prefix holds the attribute byte; byte 7 is the drive
field of the normal FCB.

### Packed Date Format (FAT)

Stored in a 16-bit word:

```
Bits 15–9 : Year minus 1980 (0–119)
Bits  8–5 : Month (1–12)
Bits  4–0 : Day   (1–31)
```

### Packed Time Format (FAT)

Stored in a 16-bit word:

```
Bits 15–11 : Hours   (0–23)
Bits 10– 5 : Minutes (0–59)
Bits  4– 0 : Seconds / 2 (0–29, multiply by 2 for actual seconds)
```

### FAT12 Disk Layout

```
Sector 0        : Boot sector (contains BPB at offset 11)
Sectors 1..F    : FAT copy 1
Sectors F+1..2F : FAT copy 2  (if fatcnt=2)
Next dir_secs   : Root directory (maxent × 32 bytes)
Remaining       : Data area (clusters 2, 3, 4, …)
```

**FAT entry values:**

| Value | Meaning |
|-------|---------|
| `0x000` | Free cluster |
| `0x001` | Reserved |
| `0x002–0xFF6` | Next cluster in chain |
| `0xFF7` | Bad cluster |
| `0xFF8–0xFFF` | End of chain (EOF) |

### Standard Disk Geometries

| Format | Capacity | Tracks | Heads | Sec/Track | Total Sectors |
|--------|----------|--------|-------|-----------|---------------|
| `--180` | 180 KB | 40 | 1 | 9 | 360 |
| `--360` | 360 KB | 40 | 2 | 9 | 720 |
| `--720` | 720 KB | 80 | 2 | 9 | 1440 |

---

## Part 4 — INT 21H System Call Reference

Programs invoke DOS services by loading a function number into **AH** and
calling `INT 21H` (or using the `CALL 5` CP/M compatibility entry).

### Character I/O Functions (0x00–0x0C)

| AH | Name | Input | Output | Description |
|----|------|-------|--------|-------------|
| 00 | ABORT | — | — | Terminate program; flush FATs |
| 01 | CONIN | — | AL=char | Read character from console with echo |
| 02 | CONOUT | DL=char | — | Write character to console |
| 03 | READER | — | AL=char | Read character from AUX port |
| 04 | PUNCH | DL=char | — | Write character to AUX port |
| 05 | LIST | DL=char | — | Write character to printer |
| 06 | RAWIO | DL=char or FFH | AL=char (if DL=FF and key ready) | Raw console I/O; DL=FF checks status without waiting |
| 07 | RAWINP | — | AL=char | Read console char, no echo, no Ctrl-C check |
| 08 | IN | — | AL=char | Read console char, no echo, Ctrl-C aborts |
| 09 | PRTBUF | DS:DX→string | — | Write `$`-terminated string to console |
| 0A | BUFIN | DS:DX→buffer | buffer filled | Buffered line input (see below) |
| 0B | CONSTAT | — | AL=FFH if ready, 00H if not | Console input status |
| 0C | FLUSHKB | AL=fn (1/6/7/8/A) | AL=result | Flush input buffer, then call function AL |

**BUFIN buffer layout** (`DS:DX`):

| Offset | Description |
|--------|-------------|
| 0 | Maximum characters to accept (set by caller) |
| 1 | Actual characters read (set by DOS on return) |
| 2+ | Input characters; byte after last = `0x0D` |

### Disk and Drive Functions (0x0D–0x1F)

| AH | Name | Input | Output | Description |
|----|------|-------|--------|-------------|
| 0D | DSKRESET | — | — | Flush all dirty buffers and FAT copies to disk |
| 0E | SELDSK | DL=drive (0=A) | AL=drive count | Select default drive |
| 0F | OPEN | DS:DX→FCB | AL=00/FF | Open existing file |
| 10 | CLOSE | DS:DX→FCB | AL=00/FF | Close file (write directory entry) |
| 11 | SRCHFRST | DS:DX→FCB | AL=00/FF; DTA=dirent | Search directory for first match |
| 12 | SRCHNXT | DS:DX→FCB | AL=00/FF; DTA=dirent | Search directory for next match |
| 13 | DELETE | DS:DX→FCB | AL=00/FF | Delete file(s); wildcards allowed |
| 14 | SEQRD | DS:DX→FCB | AL=error | Sequential read one record to DTA |
| 15 | SEQWRT | DS:DX→FCB | AL=error | Sequential write one record from DTA |
| 16 | CREATE | DS:DX→FCB | AL=00/FF | Create or truncate file |
| 17 | RENAME | DS:DX→FCB (name at +1, new name at +17) | AL=00/FF | Rename file; wildcards allowed |
| 19 | GETDRV | — | AL=current drive | Get current default drive (0=A) |
| 1A | SETDMA | DS:DX→buffer | — | Set DMA (disk transfer) address |
| 1B | GETFATPT | — | DS:BX→FAT; AL=secs/clus; CX=secsiz; DX=clusters | Get FAT info for default drive |
| 1C | GETFATPTDL | DL=drive (0=default) | same as 1B | Get FAT info for specified drive |
| 1F | GETDSKPT | — | DS:BX→DPB | Get Drive Parameter Block for current drive |

**OPEN / CLOSE / CREATE return values (AL):**

| AL | Meaning |
|----|---------|
| 00 | Success |
| FF | Error (file not found, disk full, bad name, …) |

**Sequential read/write error codes (AL):**

| AL | Meaning |
|----|---------|
| 00 | Success |
| 01 | EOF — no data read (read past end of file) |
| 02 | Segment boundary — record count was trimmed to fit in 64 KB |
| 03 | Partial record — last record padded with zeros |

### Random and Block File I/O (0x21–0x28)

| AH | Name | Input | Output | Description |
|----|------|-------|--------|-------------|
| 21 | RNDRD | DS:DX→FCB; FCB.RR=record# | AL=error; DTA=data | Random read one record at FCB.RR |
| 22 | RNDWRT | DS:DX→FCB; FCB.RR=record# | AL=error | Random write one record at FCB.RR |
| 23 | FILESIZE | DS:DX→FCB | AL=00/FF; FCB.RR=size in records | Get file size in records (uses FCB.RecSiz) |
| 24 | SETRNDREC | DS:DX→FCB | FCB.RR updated | Set FCB.RR from current sequential position |
| 27 | BLKRD | DS:DX→FCB; CX=record count; FCB.RR=start | AL=error; CX=records read | Block read CX records starting at FCB.RR |
| 28 | BLKWRT | DS:DX→FCB; CX=record count; FCB.RR=start | AL=error; CX=records written | Block write CX records starting at FCB.RR |

**FCB.RR (random record field, bytes 33–36):**
- For `RecSiz >= 64`: 3 bytes significant (24-bit record number, ~8 million records max).
- For `RecSiz < 64`: 4 bytes significant (32-bit record number).

### Utility Functions (0x25–0x2E)

| AH | Name | Input | Output | Description |
|----|------|-------|--------|-------------|
| 25 | SETVECT | AL=interrupt#; DS:DX→handler | — | Set interrupt vector |
| 26 | NEWBASE | DX=new segment | — | Set up new program base segment (PSP) |
| 29 | MAKEFCB | DS:SI→string; ES:DI→FCB; AL=flags | AL=0/1/FF; SI advanced | Parse filename string into FCB |
| 2A | GETDATE | — | CX=year; DH=month; DL=day; AL=weekday (0=Sun) | Get current date |
| 2B | SETDATE | CX=year; DH=month; DL=day | AL=00/FF | Set current date |
| 2C | GETTIME | — | CH=h; CL=m; DH=s; DL=1/100s | Get current time |
| 2D | SETTIME | CH=h; CL=m; DH=s; DL=1/100s | AL=00/FF | Set current time |
| 2E | VERIFY | AL=0 or 1 | — | 0=disable verify after write; 1=enable |

**MAKEFCB flags (AL):**

| Bit | Meaning when set |
|-----|-----------------|
| 0 | Skip leading separator characters before parsing |
| 1 | Keep current drive field (don't zero it) |
| 2 | Keep current name field |
| 3 | Keep current extension field |

**MAKEFCB return values (AL):**

| AL | Meaning |
|----|---------|
| 00 | Filename parsed, no wildcards |
| 01 | Filename parsed, contains `?` or `*` (ambiguous) |
| FF | Invalid drive specifier |

### Interrupt Vectors Used by DOS

| Vector | Hex | Purpose |
|--------|-----|---------|
| INT 20H | 0x20 | Program terminate (same as AH=00H, INT 21H) |
| INT 21H | 0x21 | DOS system call dispatcher |
| INT 22H | 0x22 | Terminate address (called on program exit) |
| INT 23H | 0x23 | Ctrl-C handler address |
| INT 24H | 0x24 | Fatal error (critical error) handler |
| INT 27H | 0x27 | Terminate and stay resident |

**INT 24H (fatal error) handler receives:**

- `AL` = drive number
- `AH` bits 1–2 = disk area (0=reserved, 1=FAT, 2=directory, 3=data)
- `AH` bit 0 = 0 for read error, 1 for write error

Handler must return:

| AL | Action |
|----|--------|
| 0 | Ignore the error and continue |
| 1 | Retry the operation |
| 2 | Abort the program |

---

## Appendix A — Error Messages

| Message | Cause |
|---------|-------|
| `Bad command or file name` | Command not found internally and no matching .COM file on disk |
| `File not found` | Specified file does not exist |
| `Cannot create destination` | Destination cannot be created (disk full or bad name) |
| `Duplicate file name or file not found` | RENAME target already exists or source not found |
| `Required parameter missing` | Command was given without a required filename argument |
| `Invalid drive specification` | Drive letter not configured |
| `Invalid date` | Date out of range or invalid format |
| `Invalid time` | Time out of range or invalid format |
| `Are you sure (Y/N)?` | Prompt before `DEL *.*` |
| `External command execution not supported in C port: NAME` | A .COM file was found on disk but cannot be executed |

---

## Appendix B — Limits

| Item | Limit |
|------|-------|
| Maximum filename length | 8 characters + `.` + 3 extension |
| Maximum logical drives | 16 |
| Maximum root directory entries | Set by disk geometry (typically 112 for 720 KB) |
| Maximum clusters per volume | 4,078 (FAT12 limit) |
| Maximum file size | 32 MB (30 bits) |
| Maximum record size (FCB) | 65,535 bytes |
| Maximum random record number | 16,777,215 (24-bit) or 4,294,967,295 (32-bit, RecSiz < 64) |
| Maximum sector size | 1,024 bytes |
| Disk formats supported | FAT12 only |
| Subdirectories | Not supported (MS-DOS 1.x limitation) |

---

## Appendix C — CP/M Compatibility

MS-DOS 1.25 provides a CP/M 2.2-compatible FCB API.  Programs written for
CP/M that call DOS at address `CS:0005H` (the `CALL 5` entry point) are
supported.  The function number is taken from `CL` (not `AH`) and remapped
onto the INT 21H dispatcher.

Functions 0–35 decimal (0x00–0x23) have direct CP/M equivalents.  Functions
36–46 (0x24–0x2E) are extended MS-DOS functions not available through
`CALL 5`.
