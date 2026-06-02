#include "command.h"
#include "../kernel/chardev.h"
#include "../kernel/fcb.h"
#include "../kernel/fileio.h"
#include "../kernel/disk.h"
#include "../kernel/fat.h"
#include "../kernel/datetime.h"
#include "../kernel/kernel.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define VERSION_STR "MS-DOS version 1.25\r\nCopyright 1981,82 Microsoft, Inc.\r\n"

/* -----------------------------------------------------------------------
 * Utility helpers
 * ----------------------------------------------------------------------- */

static void print_str(dos_t *dos, const char *s)
{
    while (*s) chardev_out(dos, (uint8_t)*s++);
}

static void print_crlf(dos_t *dos)
{
    chardev_out(dos, '\r'); chardev_out(dos, '\n');
}

static void print_char(dos_t *dos, char c)
{
    chardev_out(dos, (uint8_t)c);
}

/* Left-pad a number with spaces to width w. */
static void print_uint(dos_t *dos, uint32_t n, int w)
{
    char buf[12];
    int  i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (n == 0) buf[--i] = '0';
    while (n) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    int len = (int)(sizeof(buf) - 1 - i);
    while (len++ < w) print_char(dos, ' ');
    print_str(dos, buf + i);
}

/* Skip leading spaces. */
static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Copy a word into buf (max len), return pointer past word. */
static const char *copy_word(const char *p, char *buf, int max)
{
    int i = 0;
    p = skip_space(p);
    while (*p && *p != ' ' && *p != '\t' && i < max - 1)
        buf[i++] = (char)toupper((unsigned char)*p++);
    buf[i] = '\0';
    return p;
}

/* -----------------------------------------------------------------------
 * DIR command
 * ----------------------------------------------------------------------- */

static void cmd_dir(dos_t *dos, const char *args)
{
    args = skip_space(args);

    /* Parse optional filespec */
    uint8_t  pattern[11];
    bool     has_spec = (*args != '\0');
    uint8_t  drv      = dos->curdrv;

    memset(pattern, '?', 11);

    if (has_spec) {
        char word[16];
        copy_word(args, word, sizeof(word));

        /* Drive letter? */
        if (word[1] == ':') {
            drv = (uint8_t)(toupper((unsigned char)word[0]) - 'A');
            memmove(word, word + 2, strlen(word) - 1);
        }
        str_to_dos_name(word, pattern);
    }

    dos->thisdrv = drv;
    if (disk_fatread(dos) != 0) {
        print_str(dos, "Invalid drive\r\n");
        return;
    }
    dpb_t   *dp           = dos->thisbp;
    uint16_t ents_per_sec = dp->secsiz / DIRENT_SIZE;
    uint16_t total_entries = dp->maxent;

    /* Header */
    print_str(dos, "\r\n Directory of  ");
    print_char(dos, (char)('A' + drv));
    print_str(dos, ":\\\r\n\r\n");

    uint32_t total_bytes = 0;
    uint16_t file_count  = 0;
    uint16_t entry       = 0;
    char     namebuf[13];

    while (entry < total_entries) {
        uint16_t block = entry / ents_per_sec;
        uint16_t off   = (entry % ents_per_sec) * DIRENT_SIZE;

        if (disk_dirread(dos, dp, block) != 0) break;
        dirent_t *de = (dirent_t *)(dos->dirbuf + off);

        uint8_t first = de->name[0];
        if (first == DIRENT_END) break;

        if (first != DIRENT_FREE && !(de->attrib & (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME))) {
            /* Check pattern */
            bool match = true;
            for (int i = 0; i < 11; i++) {
                if (pattern[i] != '?' && pattern[i] != de->name[i]) {
                    match = false; break;
                }
            }
            if (match) {
                dos_name_to_str(de->name, namebuf);
                /* Filename (left-padded to 12 chars) */
                int nlen = (int)strlen(namebuf);
                print_str(dos, namebuf);
                for (int i = nlen; i < 13; i++) print_char(dos, ' ');

                if (de->attrib & ATTR_DIR) {
                    print_str(dos, " <DIR>     ");
                } else {
                    print_uint(dos, de->filsiz, 9);
                    print_char(dos, ' ');
                }

                /* Date */
                uint16_t yr; uint8_t mo, dy;
                dos_date_unpack(de->fdate, &yr, &mo, &dy);
                yr += 1980;
                print_uint(dos, mo, 2); print_char(dos, '-');
                print_uint(dos, dy, 2); print_char(dos, '-');
                print_uint(dos, yr % 100, 2);
                print_char(dos, ' ');

                /* Time */
                uint8_t h, m, s;
                dos_time_unpack(de->ftime, &h, &m, &s);
                print_uint(dos, h, 2); print_char(dos, ':');
                print_uint(dos, m, 2);

                print_crlf(dos);
                total_bytes += de->filsiz;
                file_count++;
            }
        }
        entry++;
    }

    print_crlf(dos);
    print_uint(dos, file_count, 5);
    print_str(dos, " File(s) ");
    print_uint(dos, total_bytes, 9);
    print_str(dos, " bytes\r\n");

    /* Free space */
    uint32_t free_clus = 0;
    for (uint16_t c = 2; c < dp->maxclus; c++) {
        if (fat12_is_free(fat_unpack(dp->fat, c))) free_clus++;
    }
    uint32_t free_bytes = free_clus * (dp->clusmsk + 1) * dp->secsiz;
    print_uint(dos, free_bytes, 10);
    print_str(dos, " bytes free\r\n");
}

/* -----------------------------------------------------------------------
 * TYPE command
 * ----------------------------------------------------------------------- */

static void cmd_type(dos_t *dos, const char *args)
{
    char word[16];
    args = skip_space(args);
    copy_word(args, word, sizeof(word));
    if (!word[0]) { print_str(dos, "Required parameter missing\r\n"); return; }

    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    const char *p = word;
    dos_makefcb(dos, &p, &fcb, 0);

    if (dos_open(dos, &fcb) != 0) {
        print_str(dos, "File not found\r\n"); return;
    }

    uint8_t buf[128];
    dos_setdma(dos, buf);
    fcb.recsiz = 1;

    while (dos_seqrd(dos, &fcb) == DOS_OK) {
        uint8_t c = buf[0];
        if (c == 0x1A) break;   /* Ctrl-Z = EOF */
        chardev_out(dos, c);
    }
    dos_close(dos, &fcb);
    dos_setdma(dos, dos->psp + 0x80);
}

/* -----------------------------------------------------------------------
 * COPY command
 * ----------------------------------------------------------------------- */

static void cmd_copy(dos_t *dos, const char *args)
{
    char src_name[16], dst_name[16];
    args = skip_space(args);
    args = copy_word(args, src_name, sizeof(src_name));
    args = skip_space(args);
    copy_word(args, dst_name, sizeof(dst_name));

    if (!src_name[0]) { print_str(dos, "Required parameter missing\r\n"); return; }
    if (!dst_name[0]) { print_str(dos, "Required parameter missing\r\n"); return; }

    fcb_t src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    const char *p;
    p = src_name; dos_makefcb(dos, &p, &src, 0);
    p = dst_name; dos_makefcb(dos, &p, &dst, 0);

    if (dos_open(dos, &src) != 0) {
        print_str(dos, "File not found\r\n"); return;
    }
    if (dos_create(dos, &dst) != 0) {
        dos_close(dos, &src);
        print_str(dos, "Cannot create destination\r\n"); return;
    }

    uint8_t buf[128];
    dos_setdma(dos, buf);
    src.recsiz = 128;
    dst.recsiz = 128;
    uint32_t bytes_copied = 0;
    uint8_t  rc;

    while ((rc = dos_seqrd(dos, &src)) == DOS_OK || rc == DOS_PARTIAL) {
        uint16_t n = (rc == DOS_PARTIAL) ? (uint16_t)(src.filsiz % 128) : 128;
        /* Write n bytes */
        if (n > 0) {
            dos_seqwrt(dos, &dst);
            bytes_copied += n;
        }
        if (rc == DOS_PARTIAL || buf[0] == 0x1A) break;
    }

    dst.filsiz = src.filsiz;
    dos_close(dos, &src);
    dos_close(dos, &dst);
    dos_setdma(dos, dos->psp + 0x80);

    print_uint(dos, bytes_copied, 1);
    print_str(dos, " bytes copied\r\n");
}

/* -----------------------------------------------------------------------
 * DEL / ERASE command
 * ----------------------------------------------------------------------- */

static void cmd_del(dos_t *dos, const char *args)
{
    char word[16];
    args = skip_space(args);
    copy_word(args, word, sizeof(word));
    if (!word[0]) { print_str(dos, "Required parameter missing\r\n"); return; }

    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    const char *p = word;
    dos_makefcb(dos, &p, &fcb, 0);

    /* Check for *.* and warn */
    bool wild = true;
    for (int i = 0; i < 11; i++)
        if (((uint8_t*)&fcb)[i + 1] != '?') { wild = false; break; }
    if (wild) {
        print_str(dos, "Are you sure (Y/N)? ");
        int c = dos_conin(dos);
        print_crlf(dos);
        if (toupper(c) != 'Y') return;
    }

    if (dos_delete(dos, &fcb) != 0)
        print_str(dos, "File not found\r\n");
}

/* -----------------------------------------------------------------------
 * REN / RENAME command
 * ----------------------------------------------------------------------- */

static void cmd_ren(dos_t *dos, const char *args)
{
    char src_name[16], dst_name[16];
    args = skip_space(args);
    args = copy_word(args, src_name, sizeof(src_name));
    args = skip_space(args);
    copy_word(args, dst_name, sizeof(dst_name));

    if (!src_name[0] || !dst_name[0]) {
        print_str(dos, "Required parameter missing\r\n"); return;
    }

    /* Build combined FCB: src at offset 0, dst at offset 17 */
    uint8_t buf[37 + 11];
    memset(buf, 0, sizeof(buf));
    fcb_t *fcb = (fcb_t*)buf;
    const char *p;
    p = src_name; dos_makefcb(dos, &p, fcb, 0);

    /* Copy dst name into FCB+17 */
    uint8_t dst11[11];
    memset(dst11, ' ', 11);
    p = dst_name;
    str_to_dos_name(p, dst11);
    memcpy(buf + 17, dst11, 11);

    if (dos_rename(dos, fcb) != 0)
        print_str(dos, "Duplicate file name or file not found\r\n");
}

/* -----------------------------------------------------------------------
 * DATE command
 * ----------------------------------------------------------------------- */

static void cmd_date(dos_t *dos, const char *args)
{
    args = skip_space(args);
    uint16_t yr; uint8_t mo, dy, dow;
    dos_getdate(dos, &yr, &mo, &dy, &dow);

    static const char *daynames[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    print_str(dos, "Current date is ");
    print_str(dos, daynames[dow % 7]);
    print_char(dos, ' ');
    print_uint(dos, mo, 1); print_char(dos, '-');
    print_uint(dos, dy, 2); print_char(dos, '-');
    print_uint(dos, yr, 4);
    print_crlf(dos);

    if (!*args) {
        print_str(dos, "Enter new date (mm-dd-yy): ");
        uint8_t buf[3] = {0x82, 0, 0};
        dos_bufin(dos, buf);
        print_crlf(dos);
        args = (const char*)buf + 2;
    }

    if (*args && *args != '\r') {
        int m2, d2, y2;
        if (sscanf(args, "%d-%d-%d", &m2, &d2, &y2) == 3) {
            if (y2 < 80) y2 += 2000;
            else if (y2 < 100) y2 += 1900;
            if (dos_setdate(dos, (uint16_t)y2, (uint8_t)m2, (uint8_t)d2) != 0)
                print_str(dos, "Invalid date\r\n");
        }
    }
}

/* -----------------------------------------------------------------------
 * TIME command
 * ----------------------------------------------------------------------- */

static void cmd_time(dos_t *dos, const char *args)
{
    args = skip_space(args);
    uint8_t h, m, s, c;
    dos_gettime(dos, &h, &m, &s, &c);

    print_str(dos, "Current time is ");
    print_uint(dos, h, 1); print_char(dos, ':');
    print_uint(dos, m, 2); print_char(dos, ':');
    print_uint(dos, s, 2); print_char(dos, '.');
    print_uint(dos, c, 2);
    print_crlf(dos);

    if (!*args) {
        print_str(dos, "Enter new time: ");
        uint8_t buf[3] = {0x82, 0, 0};
        dos_bufin(dos, buf);
        print_crlf(dos);
        args = (const char*)buf + 2;
    }

    if (*args && *args != '\r') {
        int h2, m2, s2 = 0, c2 = 0;
        if (sscanf(args, "%d:%d:%d.%d", &h2, &m2, &s2, &c2) >= 2) {
            if (dos_settime(dos, (uint8_t)h2, (uint8_t)m2,
                            (uint8_t)s2, (uint8_t)c2) != 0)
                print_str(dos, "Invalid time\r\n");
        }
    }
}

/* -----------------------------------------------------------------------
 * VER command
 * ----------------------------------------------------------------------- */

static void cmd_ver(dos_t *dos, const char *args)
{
    (void)args;
    print_str(dos, "\r\n" VERSION_STR "\r\n");
}

/* -----------------------------------------------------------------------
 * CLS command
 * ----------------------------------------------------------------------- */

static void cmd_cls(dos_t *dos, const char *args)
{
    (void)args;
    if (dos->bios->cls)
        dos->bios->cls(dos->bios);
    else {
        /* ANSI escape: clear screen + home cursor */
        print_str(dos, "\033[2J\033[H");
    }
}

/* -----------------------------------------------------------------------
 * ECHO command
 * ----------------------------------------------------------------------- */

static void cmd_echo(dos_t *dos, const char *args)
{
    args = skip_space(args);
    if (!*args || !strcmp(args, "OFF") || !strcmp(args, "ON")) {
        /* No output for these */
        return;
    }
    print_str(dos, args);
    print_crlf(dos);
}

/* -----------------------------------------------------------------------
 * PAUSE command
 * ----------------------------------------------------------------------- */

static void cmd_pause(dos_t *dos, const char *args)
{
    (void)args;
    print_str(dos, "Strike a key when ready . . . ");
    dos_in(dos);
    print_crlf(dos);
}

/* -----------------------------------------------------------------------
 * CHKDSK — simplified (report disk space only)
 * ----------------------------------------------------------------------- */

static void cmd_chkdsk(dos_t *dos, const char *args)
{
    (void)args;
    uint8_t drv = dos->curdrv;
    dos->thisdrv = drv;
    if (disk_fatread(dos) != 0) { print_str(dos, "Invalid drive\r\n"); return; }
    dpb_t *dp = dos->thisbp;

    uint32_t total_clus = dp->maxclus - 1;
    uint32_t free_clus  = 0;
    for (uint16_t c = 2; c < dp->maxclus; c++) {
        if (fat12_is_free(fat_unpack(dp->fat, c))) free_clus++;
    }
    uint32_t total_bytes = total_clus * (dp->clusmsk + 1) * dp->secsiz;
    uint32_t free_bytes  = free_clus  * (dp->clusmsk + 1) * dp->secsiz;

    print_uint(dos, total_bytes, 9); print_str(dos, " bytes total disk space\r\n");
    print_uint(dos, free_bytes,  9); print_str(dos, " bytes available on disk\r\n");
}

/* -----------------------------------------------------------------------
 * HELP command
 * ----------------------------------------------------------------------- */

static const struct {
    const char *name;
    const char *usage;
    const char *desc;
} help_entries[] = {
    { "CLS",    "CLS",
      "Clear the screen." },
    { "CHKDSK", "CHKDSK",
      "Show total and free disk space." },
    { "COPY",   "COPY source destination",
      "Copy a file.  Both names required; no wildcards." },
    { "DATE",   "DATE [mm-dd-yy]",
      "Display or set the current date." },
    { "DEL",    "DEL filespec",
      "Delete one or more files.  Wildcards allowed." },
    { "DIR",    "DIR [filespec]",
      "List directory contents.  Wildcards allowed." },
    { "ECHO",   "ECHO [message]",
      "Display a message on the console." },
    { "ERASE",  "ERASE filespec",
      "Alias for DEL." },
    { "EXIT",   "EXIT",
      "Quit the command interpreter." },
    { "PAUSE",  "PAUSE",
      "Wait for a keypress before continuing." },
    { "REM",    "REM [text]",
      "Comment line; ignored." },
    { "REN",    "REN oldname newname",
      "Rename a file.  Wildcards allowed." },
    { "RENAME", "RENAME oldname newname",
      "Alias for REN." },
    { "TIME",   "TIME [hh:mm[:ss[.cc]]]",
      "Display or set the current time." },
    { "TYPE",   "TYPE filename",
      "Display a file on the console." },
    { "VER",    "VER",
      "Display the MS-DOS version." },
    { NULL, NULL, NULL }
};

static void cmd_help(dos_t *dos, const char *args)
{
    args = skip_space(args);

    /* HELP <command>: look up specific command */
    if (*args && *args != '\r') {
        char word[16];
        copy_word(args, word, sizeof(word));
        for (int i = 0; help_entries[i].name; i++) {
            if (strcmp(word, help_entries[i].name) == 0) {
                print_str(dos, "\r\n  ");
                print_str(dos, help_entries[i].usage);
                print_crlf(dos);
                print_str(dos, "    ");
                print_str(dos, help_entries[i].desc);
                print_crlf(dos);
                print_crlf(dos);
                return;
            }
        }
        print_str(dos, "No help available for: ");
        print_str(dos, word);
        print_crlf(dos);
        return;
    }

    /* HELP with no argument: list all commands */
    print_str(dos, "\r\nMS-DOS 1.25 Internal Commands\r\n");
    print_str(dos, "------------------------------\r\n\r\n");
    for (int i = 0; help_entries[i].name; i++) {
        /* Left-align name in 10 chars */
        int nlen = (int)strlen(help_entries[i].name);
        print_str(dos, "  ");
        print_str(dos, help_entries[i].name);
        for (int s = nlen; s < 10; s++) print_char(dos, ' ');
        print_str(dos, help_entries[i].desc);
        print_crlf(dos);
    }
    print_crlf(dos);
    print_str(dos, "  Drive change:  A:  B:  C:  ...\r\n");
    print_str(dos, "\r\nType HELP <command> for detailed usage.\r\n\r\n");
}

/* -----------------------------------------------------------------------
 * Internal command table
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*fn)(dos_t *dos, const char *args);
} cmd_entry_t;

static const cmd_entry_t cmd_table[] = {
    { "DIR",    cmd_dir    },
    { "TYPE",   cmd_type   },
    { "COPY",   cmd_copy   },
    { "DEL",    cmd_del    },
    { "ERASE",  cmd_del    },
    { "REN",    cmd_ren    },
    { "RENAME", cmd_ren    },
    { "DATE",   cmd_date   },
    { "TIME",   cmd_time   },
    { "VER",    cmd_ver    },
    { "CLS",    cmd_cls    },
    { "ECHO",   cmd_echo   },
    { "PAUSE",  cmd_pause  },
    { "REM",    NULL       },   /* comment — ignore */
    { "CHKDSK", cmd_chkdsk },
    { "HELP",   cmd_help   },
    { NULL,     NULL       }
};

/* -----------------------------------------------------------------------
 * External command stub
 * ----------------------------------------------------------------------- */

static void run_external(dos_t *dos, const char *cmd, const char *args)
{
    /* Try to find CMD.COM or CMD.EXE on the current drive's root directory.
     * Since we cannot execute x86 binary code in this C port, we just
     * report the command as not found. */
    (void)args;

    /* Search for it anyway so users can see what's on the disk */
    char name[16];
    snprintf(name, sizeof(name), "%s.COM", cmd);

    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    const char *p = name;
    dos_makefcb(dos, &p, &fcb, 0);

    if (dos_open(dos, &fcb) == DOS_OK) {
        dos_close(dos, &fcb);
        print_str(dos, "External command execution not supported in C port: ");
        print_str(dos, cmd);
        print_crlf(dos);
    } else {
        print_str(dos, "Bad command or file name\r\n");
    }
}

/* -----------------------------------------------------------------------
 * command_exec — parse and dispatch one command line
 * ----------------------------------------------------------------------- */

int command_exec(dos_t *dos, const char *line)
{
    line = skip_space(line);
    if (!*line || *line == '\r' || *line == '\n') return 0;

    /* Check for drive change: "A:", "B:", etc. */
    if (line[1] == ':' && !line[2]) {
        uint8_t drv = (uint8_t)(toupper((unsigned char)line[0]) - 'A');
        if (drv < dos->num_drives) {
            dos->curdrv = drv;
        } else {
            print_str(dos, "Invalid drive specification\r\n");
        }
        return 0;
    }

    /* Extract command name */
    char cmd[16];
    const char *args;
    args = copy_word(line, cmd, sizeof(cmd));

    /* Search internal table */
    for (int i = 0; cmd_table[i].name; i++) {
        if (strcmp(cmd, cmd_table[i].name) == 0) {
            if (cmd_table[i].fn) cmd_table[i].fn(dos, args);
            return 0;
        }
    }

    /* EXIT: terminate interpreter */
    if (strcmp(cmd, "EXIT") == 0) return -1;

    /* Not internal — try external */
    run_external(dos, cmd, args);
    return 0;
}

/* -----------------------------------------------------------------------
 * command_run — main interpreter loop
 * ----------------------------------------------------------------------- */

void command_run(dos_t *dos)
{
    print_str(dos, "\r\n" VERSION_STR "\r\n");

    uint8_t linebuf[3 + 256];

    for (;;) {
        /* Print prompt: "A>" */
        print_char(dos, (char)('A' + dos->curdrv));
        print_char(dos, '>');

        /* Read line */
        linebuf[0] = 0xFE;   /* max 254 chars */
        linebuf[1] = 0;
        dos_bufin(dos, linebuf);
        print_crlf(dos);

        /* Null-terminate */
        uint8_t len = linebuf[1];
        linebuf[2 + len] = '\0';

        /* EOF on stdin (Ctrl-Z / pipe closed) */
        if (linebuf[1] == 0 && linebuf[2] == '\r') { break; }
        if (linebuf[1] == 0) break;

        int rc = command_exec(dos, (const char*)linebuf + 2);
        if (rc < 0) break;
    }
}
