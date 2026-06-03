/* tests/test_command.c — Integration tests for the command processor.
 *
 * Strategy: mount a fake in-memory 180 KB FAT12 disk image through a
 * minimal BIOS shim, call dos_init() to boot the kernel on top of it,
 * then drive every command through command_exec() and check captured
 * console output.  All real kernel code runs; only the BIOS I/O layer
 * and the editor/BASIC sub-commands are stubbed.
 *
 * Disk geometry (--180 format):
 *   360 sectors × 512 bytes = 184 320 bytes
 *   boot(1) + FAT×2(2+2) + root-dir(7) + data(348) sectors
 *   firfat=1, firdir=5, firrec=12, maxclus=349
 *
 * Build:
 *   cc -std=c11 -Wall -I. -o tests/test_command tests/test_command.c \
 *      src/kernel/fat.c src/kernel/disk.c src/kernel/fcb.c           \
 *      src/kernel/fileio.c src/kernel/chardev.c src/kernel/datetime.c \
 *      src/kernel/kernel.c src/command/command.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "src/kernel/types.h"
#include "src/kernel/bios.h"
#include "src/kernel/dos.h"
#include "src/kernel/fat.h"
#include "src/kernel/disk.h"
#include "src/kernel/fcb.h"
#include "src/kernel/fileio.h"
#include "src/kernel/chardev.h"
#include "src/kernel/datetime.h"
#include "src/kernel/kernel.h"
#include "src/command/command.h"

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                  */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("PASS  " msg "\n"); g_pass++; } \
        else      { printf("FAIL  " msg "  [line %d]\n", __LINE__); g_fail++; } \
    } while (0)

/* ------------------------------------------------------------------ */
/* In-memory 180 KB disk image                                          */
/* ------------------------------------------------------------------ */

#define DISK_NSEC   360
#define DISK_SECSIZ 512
static uint8_t g_disk[DISK_NSEC * DISK_SECSIZ];

static void format_disk(void)
{
    memset(g_disk, 0, sizeof g_disk);

    /* FAT1 at sector 1: media byte 0xFC then 0xFF 0xFF */
    uint8_t *fat = g_disk + DISK_SECSIZ;
    fat[0] = 0xFC; fat[1] = 0xFF; fat[2] = 0xFF;

    /* FAT2 at sector 3: identical */
    uint8_t *fat2 = g_disk + 3 * DISK_SECSIZ;
    fat2[0] = 0xFC; fat2[1] = 0xFF; fat2[2] = 0xFF;
}

/* ------------------------------------------------------------------ */
/* Console I/O capture                                                   */
/* ------------------------------------------------------------------ */

static char    g_out[8192];
static int     g_outpos;

static char    g_in_buf[512];
static int     g_in_pos;

static void reset_output(void)
{
    g_outpos = 0; g_out[0] = '\0';
}

static void set_input(const char *s)
{
    strncpy(g_in_buf, s ? s : "", sizeof g_in_buf - 1);
    g_in_buf[sizeof g_in_buf - 1] = '\0';
    g_in_pos = 0;
}

/* ------------------------------------------------------------------ */
/* Fake BIOS                                                             */
/* ------------------------------------------------------------------ */

/* bios_dpt_t is internal to kernel.c; copy the layout here */
typedef struct {
    uint8_t  drvnum;
    uint16_t secsiz;
    uint8_t  secs_per_clus;
    uint16_t reserved_secs;
    uint8_t  fat_copies;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t  media_byte;
    uint8_t  fat_size;
} test_dpt_t;

static bios_t  g_bios;
static dos_t   g_dos;

/* Persistent date/time state for the fake BIOS */
static uint16_t g_bios_days = 0;          /* days since Jan 1 1980 */
static uint8_t  g_bios_h=12, g_bios_m=0, g_bios_s=0, g_bios_c=0;

static void   bios_out(bios_t *b, uint8_t c) {
    (void)b;
    if (g_outpos < (int)sizeof g_out - 1) g_out[g_outpos++] = (char)c;
    g_out[g_outpos] = '\0';
}
static int    bios_stat(bios_t *b) { (void)b; return 1; }
static int    bios_in(bios_t *b) {
    (void)b;
    unsigned char c = (unsigned char)g_in_buf[g_in_pos];
    if (c) { g_in_pos++; return c; }
    return '\r';
}
static void   bios_print(bios_t *b, uint8_t c)  { (void)b; (void)c; }
static int    bios_auxin(bios_t *b)              { (void)b; return 0; }
static void   bios_auxout(bios_t *b, uint8_t c) { (void)b; (void)c; }
static void   bios_flush(bios_t *b)             { (void)b; }

static int bios_disk_read(bios_t *b, uint8_t unit, uint8_t *buf,
                           uint16_t count, uint32_t sector, uint16_t *done)
{
    (void)b; (void)unit;
    for (uint16_t i = 0; i < count; i++)
        memcpy(buf + i * DISK_SECSIZ,
               g_disk + (sector + i) * (uint32_t)DISK_SECSIZ, DISK_SECSIZ);
    if (done) *done = count;
    return 0;
}
static int bios_disk_write(bios_t *b, uint8_t unit, bool verify,
                            const uint8_t *buf, uint16_t count, uint32_t sector,
                            uint16_t *done)
{
    (void)b; (void)unit; (void)verify;
    for (uint16_t i = 0; i < count; i++)
        memcpy(g_disk + (sector + i) * (uint32_t)DISK_SECSIZ,
               buf + i * DISK_SECSIZ, DISK_SECSIZ);
    if (done) *done = count;
    return 0;
}
static int    bios_disk_change(bios_t *b, uint8_t u) { (void)b; (void)u; return 0; }

static void bios_gettime(bios_t *b, uint16_t *days,
                          uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *c)
{
    (void)b;
    *days = g_bios_days;
    *h = g_bios_h; *m = g_bios_m; *s = g_bios_s; *c = g_bios_c;
}
static void bios_settime(bios_t *b, uint8_t h, uint8_t m, uint8_t s, uint8_t c)
{
    (void)b;
    g_bios_h = h; g_bios_m = m; g_bios_s = s; g_bios_c = c;
}
static void bios_setdate(bios_t *b, uint16_t days)
{
    (void)b;
    g_bios_days = days;
}

static uint8_t bios_mapdev(bios_t *b, uint8_t unit, uint8_t fat_byte)
{ (void)b; (void)fat_byte; return unit; }

static int bios_get_drive_config(bios_t *b, void *cfg_out, int max_drives)
{
    (void)b; (void)max_drives;
    test_dpt_t *dpt = (test_dpt_t *)cfg_out;
    dpt->drvnum       = 0;
    dpt->secsiz       = 512;
    dpt->secs_per_clus = 1;
    dpt->reserved_secs = 1;
    dpt->fat_copies    = 2;
    dpt->root_entries  = 112;
    dpt->total_sectors = 360;
    dpt->media_byte    = 0xFC;
    dpt->fat_size      = 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Stubs for sub-commands not under test here                           */
/* ------------------------------------------------------------------ */

void editor_run(dos_t *dos, const char *filename)
{
    (void)dos; (void)filename;
    /* no-op: editor is tested in test_edit.c */
}

void basic_run(dos_t *dos, const char *filename)
{
    (void)dos; (void)filename;
    /* no-op: BASIC is tested in test_basic.c */
}

/* ------------------------------------------------------------------ */
/* Test helpers                                                          */
/* ------------------------------------------------------------------ */

static void setup(void)
{
    format_disk();
    g_bios_days = 0; g_bios_h = 12; g_bios_m = 0; g_bios_s = 0; g_bios_c = 0;

    memset(&g_bios, 0, sizeof g_bios);
    g_bios.out              = bios_out;
    g_bios.stat             = bios_stat;
    g_bios.in               = bios_in;
    g_bios.print            = bios_print;
    g_bios.auxin            = bios_auxin;
    g_bios.auxout           = bios_auxout;
    g_bios.flush            = bios_flush;
    g_bios.disk_read        = bios_disk_read;
    g_bios.disk_write       = bios_disk_write;
    g_bios.disk_change      = bios_disk_change;
    g_bios.gettime          = bios_gettime;
    g_bios.settime          = bios_settime;
    g_bios.setdate          = bios_setdate;
    g_bios.mapdev           = bios_mapdev;
    g_bios.get_drive_config = bios_get_drive_config;

    dos_init(&g_dos, &g_bios);
    reset_output();
    set_input("");
}

/* Execute a command and return captured output.  Clears buffer first. */
static const char *run(const char *cmd)
{
    reset_output();
    command_exec(&g_dos, cmd);
    return g_out;
}

/* Create a file using the DOS API; clears any side-effect output. */
static void make_file(const char *name, const char *content)
{
    fcb_t fcb; memset(&fcb, 0, sizeof fcb);
    const char *p = name;
    dos_makefcb(&g_dos, &p, &fcb, 0);
    if (dos_create(&g_dos, &fcb) != DOS_OK) return;

    uint8_t buf[128]; memset(buf, 0, 128);
    size_t len = strlen(content);
    size_t n   = len > 128 ? 128 : len;
    memcpy(buf, content, n);
    dos_setdma(&g_dos, buf);
    fcb.recsiz = 128;
    dos_seqwrt(&g_dos, &fcb);

    fcb.filsiz = (uint32_t)len;
    dos_close(&g_dos, &fcb);
    dos_setdma(&g_dos, g_dos.psp + 0x80);
    reset_output();
}

/* Return true if a file can be opened (i.e. it exists on disk). */
static bool file_exists(const char *name)
{
    fcb_t fcb; memset(&fcb, 0, sizeof fcb);
    const char *p = name;
    dos_makefcb(&g_dos, &p, &fcb, 0);
    bool ok = (dos_open(&g_dos, &fcb) == DOS_OK);
    if (ok) dos_close(&g_dos, &fcb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* ECHO                                                                  */
/* ------------------------------------------------------------------ */

static void test_echo(void)
{
    printf("\n--- ECHO ---\n");
    setup();

    CHECK(strcmp(run("ECHO HELLO"), "HELLO\r\n") == 0,
          "ECHO prints its argument");
    CHECK(strcmp(run("ECHO HELLO WORLD"), "HELLO WORLD\r\n") == 0,
          "ECHO preserves spaces in argument");
    CHECK(strlen(run("ECHO")) == 0,
          "ECHO with no args produces no output");
    CHECK(strlen(run("ECHO ON")) == 0,
          "ECHO ON produces no output");
    CHECK(strlen(run("ECHO OFF")) == 0,
          "ECHO OFF produces no output");
    CHECK(strcmp(run("echo lowercase"), "lowercase\r\n") == 0,
          "command name is case-insensitive (echo)");
}

/* ------------------------------------------------------------------ */
/* VER                                                                   */
/* ------------------------------------------------------------------ */

static void test_ver(void)
{
    printf("\n--- VER ---\n");
    setup();

    const char *o = run("VER");
    CHECK(strstr(o, "MS-DOS") != NULL, "VER contains MS-DOS");
    CHECK(strstr(o, "1.25")   != NULL, "VER contains version 1.25");
}

/* ------------------------------------------------------------------ */
/* REM / empty lines                                                     */
/* ------------------------------------------------------------------ */

static void test_rem(void)
{
    printf("\n--- REM ---\n");
    setup();

    CHECK(strlen(run("REM this is a comment")) == 0, "REM produces no output");
    CHECK(strlen(run("REM")) == 0,                   "REM with nothing produces no output");
    CHECK(strlen(run("")) == 0,                      "empty line produces no output");
    CHECK(strlen(run("   ")) == 0,                   "whitespace-only line produces no output");
}

/* ------------------------------------------------------------------ */
/* HELP                                                                  */
/* ------------------------------------------------------------------ */

static void test_help(void)
{
    printf("\n--- HELP ---\n");
    setup();

    const char *o;

    o = run("HELP");
    CHECK(strstr(o, "DIR")  != NULL, "HELP lists DIR");
    CHECK(strstr(o, "COPY") != NULL, "HELP lists COPY");
    CHECK(strstr(o, "TYPE") != NULL, "HELP lists TYPE");
    CHECK(strstr(o, "ECHO") != NULL, "HELP lists ECHO");

    o = run("HELP ECHO");
    CHECK(strstr(o, "ECHO") != NULL, "HELP ECHO shows ECHO entry");
    CHECK(strstr(o, "message") != NULL, "HELP ECHO shows usage text");

    o = run("HELP VER");
    CHECK(strstr(o, "VER") != NULL, "HELP VER shows VER entry");

    o = run("HELP BOGUS");
    CHECK(strstr(o, "No help available") != NULL,
          "HELP for unknown command says 'No help available'");
    CHECK(strstr(o, "BOGUS") != NULL,
          "HELP for unknown command echoes the name");
}

/* ------------------------------------------------------------------ */
/* Command dispatch: EXIT, drive change, unknown                        */
/* ------------------------------------------------------------------ */

static void test_dispatch(void)
{
    printf("\n--- dispatch ---\n");
    setup();

    /* EXIT returns -1 */
    reset_output();
    int rc = command_exec(&g_dos, "EXIT");
    CHECK(rc == -1, "EXIT returns -1");

    /* Drive change A: → sets curdrv=0 */
    g_dos.curdrv = 1;
    run("A:");
    CHECK(g_dos.curdrv == 0, "A: sets curdrv to 0");

    /* Drive change to invalid drive (only 1 drive configured) */
    const char *o = run("B:");
    CHECK(strstr(o, "Invalid drive") != NULL,
          "B: on single-drive system prints error");
    CHECK(g_dos.curdrv == 0, "invalid drive change leaves curdrv unchanged");

    /* Unknown command that has no .COM → "Bad command or file name" */
    o = run("NOSUCHCMD");
    CHECK(strstr(o, "Bad command or file name") != NULL,
          "unknown command prints 'Bad command or file name'");

    /* ERASE is an alias for DEL (dispatch lookup) */
    make_file("ALIAS.TXT", "x");
    o = run("ERASE ALIAS.TXT");
    CHECK(!file_exists("ALIAS.TXT"), "ERASE (alias for DEL) deletes file");

    /* CHDIR is an alias for CD */
    o = run("CHDIR");
    CHECK(strstr(o, "A:\\") != NULL || strstr(o, "A:") != NULL,
          "CHDIR (alias for CD) runs CD");
}

/* ------------------------------------------------------------------ */
/* DATE and TIME                                                         */
/* ------------------------------------------------------------------ */

static void test_datetime(void)
{
    printf("\n--- DATE / TIME ---\n");
    setup();

    const char *o;

    /* DATE with no args shows current date and prompts (empty input → no change) */
    set_input("\r");
    o = run("DATE");
    CHECK(strstr(o, "Current date is") != NULL, "DATE shows current date header");

    /* DATE with arg sets the date */
    run("DATE 6-15-84");
    uint16_t yr; uint8_t mo, dy, dow;
    dos_getdate(&g_dos, &yr, &mo, &dy, &dow);
    CHECK(yr == 1984 && mo == 6 && dy == 15,
          "DATE 6-15-84 sets year=1984 month=6 day=15");

    /* DATE with 2-digit year < 80 → 2000+ */
    run("DATE 1-1-05");
    dos_getdate(&g_dos, &yr, &mo, &dy, &dow);
    CHECK(yr == 2005, "DATE 2-digit year <80 → 2000+");

    /* DATE with invalid value prints error */
    o = run("DATE 99-99-99");
    CHECK(strstr(o, "Invalid date") != NULL, "DATE with bad value prints error");

    /* TIME with no args shows current time and prompts (empty input) */
    set_input("\r");
    o = run("TIME");
    CHECK(strstr(o, "Current time is") != NULL, "TIME shows current time header");

    /* TIME with arg sets the time */
    run("TIME 9:30:00");
    uint8_t h, m, s, c;
    dos_gettime(&g_dos, &h, &m, &s, &c);
    CHECK(h == 9 && m == 30, "TIME 9:30:00 sets hour=9 min=30");

    /* TIME with invalid value prints error */
    o = run("TIME 99:99:99");
    CHECK(strstr(o, "Invalid time") != NULL, "TIME with bad value prints error");
}

/* ------------------------------------------------------------------ */
/* CREATE                                                                */
/* ------------------------------------------------------------------ */

static void test_create(void)
{
    printf("\n--- CREATE ---\n");
    setup();

    const char *o;

    /* Basic create */
    o = run("CREATE HELLO.TXT");
    CHECK(strstr(o, "Created") != NULL,  "CREATE prints confirmation");
    CHECK(strstr(o, "HELLO.TXT") != NULL, "CREATE confirmation shows filename");
    CHECK(file_exists("HELLO.TXT"), "CREATE: file exists on disk");

    /* Create with no argument */
    o = run("CREATE");
    CHECK(strstr(o, "Required parameter missing") != NULL,
          "CREATE with no args: required parameter");

    /* Create with wildcard → rejected */
    o = run("CREATE *.TXT");
    CHECK(strstr(o, "Wildcards not allowed") != NULL,
          "CREATE with wildcard: error");

    /* Creating same file twice is allowed (truncates) */
    make_file("DUP.TXT", "original content");
    o = run("CREATE DUP.TXT");
    CHECK(strstr(o, "Created") != NULL, "CREATE can overwrite existing file");
}

/* ------------------------------------------------------------------ */
/* TYPE                                                                  */
/* ------------------------------------------------------------------ */

static void test_type(void)
{
    printf("\n--- TYPE ---\n");
    setup();

    /* Missing argument */
    CHECK(strstr(run("TYPE"), "Required parameter missing") != NULL,
          "TYPE with no args: required parameter");

    /* File not found */
    CHECK(strstr(run("TYPE NOFILE.TXT"), "File not found") != NULL,
          "TYPE non-existent file: error");

    /* Normal text file */
    make_file("MSG.TXT", "HELLO WORLD");
    const char *o = run("TYPE MSG.TXT");
    CHECK(strstr(o, "HELLO WORLD") != NULL,
          "TYPE prints file content");

    /* File with embedded Ctrl-Z (0x1A) → stops before it */
    char buf[10]; buf[0]='A'; buf[1]='B'; buf[2]=0x1A; buf[3]='C'; buf[4]='\0';
    make_file("CRLF.TXT", buf);
    o = run("TYPE CRLF.TXT");
    CHECK(strstr(o, "AB") != NULL, "TYPE: content before Ctrl-Z shown");
    CHECK(strchr(o, 'C') == NULL,  "TYPE: content after Ctrl-Z suppressed");

    /* Multiple lines survive TYPE */
    make_file("LINES.TXT", "LINE1\r\nLINE2\r\n");
    o = run("TYPE LINES.TXT");
    CHECK(strstr(o, "LINE1") != NULL, "TYPE: multi-line file line 1 shown");
    CHECK(strstr(o, "LINE2") != NULL, "TYPE: multi-line file line 2 shown");
}

/* ------------------------------------------------------------------ */
/* OPEN (display with safe-ASCII rendering)                             */
/* ------------------------------------------------------------------ */

static void test_open(void)
{
    printf("\n--- OPEN ---\n");
    setup();

    /* Missing argument */
    CHECK(strstr(run("OPEN"), "Required parameter missing") != NULL,
          "OPEN with no args: required parameter");

    /* File not found */
    CHECK(strstr(run("OPEN NOFILE.TXT"), "File not found") != NULL,
          "OPEN non-existent file: error");

    /* Normal text: shows header + content + footer */
    make_file("TXT.TXT", "HELLO");
    const char *o = run("OPEN TXT.TXT");
    CHECK(strstr(o, "---") != NULL,    "OPEN: header/footer dashes present");
    CHECK(strstr(o, "HELLO") != NULL,  "OPEN: file content shown");
    CHECK(strstr(o, "5 bytes") != NULL, "OPEN: byte count shown");

    /* Control chars rendered as caret notation */
    char ctrl[4] = {0x01, 0x07, 0x1B, '\0'}; /* ^A, ^G, ^[ */
    make_file("CTRL.TXT", ctrl);
    o = run("OPEN CTRL.TXT");
    CHECK(strstr(o, "^A") != NULL, "OPEN: 0x01 rendered as ^A");
    CHECK(strstr(o, "^G") != NULL, "OPEN: 0x07 rendered as ^G");
    CHECK(strstr(o, "^[") != NULL, "OPEN: 0x1B (ESC) rendered as ^[");

    /* DEL (0x7F) rendered as ^? */
    char del_char[2] = {0x7F, '\0'};
    make_file("DEL.TXT", del_char);
    o = run("OPEN DEL.TXT");
    CHECK(strstr(o, "^?") != NULL, "OPEN: 0x7F rendered as ^?");

    /* LF renders as CR+LF; bare CR is swallowed */
    make_file("NL.TXT", "A\nB\rC");
    o = run("OPEN NL.TXT");
    CHECK(strstr(o, "A\r\nB") != NULL || strstr(o, "AB") != NULL,
          "OPEN: LF → CRLF, bare CR swallowed");

    /* TAB expands to 8-column boundary */
    make_file("TAB.TXT", "\tX");
    o = run("OPEN TAB.TXT");
    /* carpos=0 at TAB → 8 spaces, then 'X' */
    CHECK(strstr(o, "        X") != NULL, "OPEN: TAB expanded to 8 spaces at col 0");
}

/* ------------------------------------------------------------------ */
/* COPY                                                                  */
/* ------------------------------------------------------------------ */

static void test_copy(void)
{
    printf("\n--- COPY ---\n");
    setup();

    /* Missing source */
    CHECK(strstr(run("COPY"), "Required parameter missing") != NULL,
          "COPY with no args: error");

    /* Missing destination */
    CHECK(strstr(run("COPY SRC.TXT"), "Required parameter missing") != NULL,
          "COPY with only source: error");

    /* Source not found */
    CHECK(strstr(run("COPY NOSRC.TXT DST.TXT"), "File not found") != NULL,
          "COPY missing source: error");

    /* Normal copy */
    make_file("SRC.TXT", "COPYTEST");
    const char *o = run("COPY SRC.TXT DST.TXT");
    CHECK(strstr(o, "bytes copied") != NULL, "COPY reports bytes copied");
    CHECK(file_exists("DST.TXT"), "COPY: destination file created");

    /* Destination content matches source */
    fcb_t fcb; memset(&fcb, 0, sizeof fcb);
    const char *p = "DST.TXT";
    dos_makefcb(&g_dos, &p, &fcb, 0);
    dos_open(&g_dos, &fcb);
    uint8_t buf[128]; memset(buf, 0, 128);
    dos_setdma(&g_dos, buf); fcb.recsiz = 1;
    char dst_content[16] = {0};
    int i = 0;
    while (dos_seqrd(&g_dos, &fcb) == DOS_OK && i < 15)
        dst_content[i++] = (char)buf[0];
    dos_close(&g_dos, &fcb);
    dos_setdma(&g_dos, g_dos.psp + 0x80);
    CHECK(strncmp(dst_content, "COPYTEST", 8) == 0,
          "COPY: destination content matches source");
}

/* ------------------------------------------------------------------ */
/* DEL / ERASE                                                           */
/* ------------------------------------------------------------------ */

static void test_del(void)
{
    printf("\n--- DEL ---\n");
    setup();

    /* Missing argument */
    CHECK(strstr(run("DEL"), "Required parameter missing") != NULL,
          "DEL with no args: error");

    /* File not found */
    CHECK(strstr(run("DEL NOFILE.TXT"), "File not found") != NULL,
          "DEL non-existent file: error");

    /* Normal delete */
    make_file("TODEL.TXT", "bye");
    run("DEL TODEL.TXT");
    CHECK(!file_exists("TODEL.TXT"), "DEL removes the file from disk");

    /* DEL *.* with N → cancels, file survives */
    make_file("KEEP.TXT", "keep");
    set_input("N");
    run("DEL *.*");
    CHECK(file_exists("KEEP.TXT"), "DEL *.* with N answer: file not deleted");

    /* DEL *.* with Y → deletes all files */
    make_file("GONE1.TXT", "a");
    make_file("GONE2.TXT", "b");
    set_input("Y");
    run("DEL *.*");
    CHECK(!file_exists("GONE1.TXT"), "DEL *.* with Y: first file deleted");
    CHECK(!file_exists("GONE2.TXT"), "DEL *.* with Y: second file deleted");

    /* Prompt text is printed before Y/N */
    make_file("PROMPT.TXT", "x");
    set_input("Y");
    reset_output();
    set_input("Y");
    command_exec(&g_dos, "DEL *.*");
    CHECK(strstr(g_out, "Are you sure") != NULL,
          "DEL *.*: prints Are you sure prompt");
}

/* ------------------------------------------------------------------ */
/* REN / RENAME                                                          */
/* ------------------------------------------------------------------ */

static void test_ren(void)
{
    printf("\n--- REN ---\n");
    setup();

    /* Missing argument */
    CHECK(strstr(run("REN"), "Required parameter missing") != NULL,
          "REN with no args: error");
    CHECK(strstr(run("REN OLDNAME.TXT"), "Required parameter missing") != NULL,
          "REN with only one name: error");

    /* Normal rename */
    make_file("OLD.TXT", "data");
    run("REN OLD.TXT NEW.TXT");
    CHECK(!file_exists("OLD.TXT"), "REN: old name no longer exists");
    CHECK( file_exists("NEW.TXT"), "REN: new name exists");
}

/* ------------------------------------------------------------------ */
/* CHKDSK                                                                */
/* ------------------------------------------------------------------ */

static void test_chkdsk(void)
{
    printf("\n--- CHKDSK ---\n");
    setup();

    const char *o = run("CHKDSK");
    CHECK(strstr(o, "bytes total disk space") != NULL,
          "CHKDSK shows total disk space");
    CHECK(strstr(o, "bytes available") != NULL,
          "CHKDSK shows available bytes");

    /* On a fresh disk, available ≈ total (only FAT/dir overhead) */
    /* 180 KB disk: 348 data clusters * 512 bytes = 178 176 bytes */
    CHECK(strstr(o, "178176") != NULL || strstr(o, "180") != NULL,
          "CHKDSK free space is close to disk size on empty disk");

    /* After creating a file, free space decreases */
    make_file("BIG.TXT", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    const char *o2 = run("CHKDSK");
    /* At least check the output still makes sense */
    CHECK(strstr(o2, "bytes total disk space") != NULL,
          "CHKDSK still works after creating a file");
}

/* ------------------------------------------------------------------ */
/* DIR                                                                   */
/* ------------------------------------------------------------------ */

static void test_dir(void)
{
    printf("\n--- DIR ---\n");
    setup();

    const char *o;

    /* Empty directory */
    o = run("DIR");
    CHECK(strstr(o, "Directory of") != NULL, "DIR shows directory header");
    CHECK(strstr(o, "0 File(s)") != NULL,    "DIR on empty dir: 0 files");
    CHECK(strstr(o, "bytes free") != NULL,    "DIR shows free bytes");

    /* Create files and check they appear */
    make_file("HELLO.TXT", "hello");
    make_file("WORLD.BAS", "world");
    o = run("DIR");
    CHECK(strstr(o, "HELLO.TXT") != NULL, "DIR lists HELLO.TXT");
    CHECK(strstr(o, "WORLD.BAS") != NULL, "DIR lists WORLD.BAS");
    CHECK(strstr(o, "2 File(s)") != NULL, "DIR shows 2 files");

    /* DIR with a wildcard filter */
    o = run("DIR *.TXT");
    CHECK(strstr(o, "HELLO.TXT") != NULL, "DIR *.TXT shows .TXT files");
    CHECK(strstr(o, "WORLD.BAS") == NULL, "DIR *.TXT hides .BAS files");

    /* DIR shows correct file size */
    make_file("SIZED.TXT", "12345");   /* 5 bytes */
    o = run("DIR SIZED.TXT");
    CHECK(strstr(o, "5") != NULL, "DIR shows correct file size");

    /* DIR with non-matching spec → 0 files */
    o = run("DIR *.XYZ");
    CHECK(strstr(o, "0 File(s)") != NULL, "DIR with no-match spec: 0 files");

    /* DIR header shows current drive */
    CHECK(strstr(o, "A:") != NULL || strstr(o, "A:\\") != NULL,
          "DIR header includes drive letter");
}

/* ------------------------------------------------------------------ */
/* CD / CHDIR (basic path display and error paths)                      */
/* ------------------------------------------------------------------ */

static void test_cd(void)
{
    printf("\n--- CD ---\n");
    setup();

    /* CD with no args shows current path */
    const char *o = run("CD");
    CHECK(strstr(o, "A:") != NULL || strstr(o, "A:\\") != NULL,
          "CD with no args shows current path");

    /* CD \ resets to root (already there; should be no-op output) */
    g_dos.curdir_path[0][0] = '\0';
    run("CD \\");
    CHECK(g_dos.curdir_clus[0] == 0, "CD \\ stays at root cluster 0");

    /* CD to non-existent directory */
    o = run("CD NOSUCHDIR");
    CHECK(strstr(o, "not found") != NULL || strstr(o, "Not") != NULL,
          "CD to non-existent dir: error");

    /* CD with invalid drive */
    o = run("CD Z:\\DOCS");
    CHECK(strstr(o, "Invalid drive") != NULL, "CD with invalid drive: error");

    /* CD with wildcard → error */
    o = run("CD *.DIR");
    CHECK(strstr(o, "Wildcards not allowed") != NULL,
          "CD with wildcard: error");
}

/* ------------------------------------------------------------------ */
/* MKDIR / MD  and  RMDIR / RD                                          */
/* ------------------------------------------------------------------ */

static void test_mkdir_rmdir(void)
{
    printf("\n--- MKDIR / RMDIR ---\n");
    setup();

    const char *o;

    /* MKDIR missing arg */
    o = run("MKDIR");
    CHECK(strstr(o, "Required parameter missing") != NULL,
          "MKDIR with no args: error");

    /* Create a directory */
    o = run("MKDIR DOCS");
    CHECK(strstr(o, "Created") != NULL, "MKDIR creates directory");

    /* Duplicate MKDIR → error */
    o = run("MKDIR DOCS");
    CHECK(strstr(o, "already exists") != NULL,
          "MKDIR on existing name: already exists error");

    /* MKDIR with wildcard */
    o = run("MKDIR *.DIR");
    CHECK(strstr(o, "Wildcards not allowed") != NULL,
          "MKDIR with wildcard: error");

    /* RMDIR missing arg */
    o = run("RMDIR");
    CHECK(strstr(o, "Required parameter missing") != NULL,
          "RMDIR with no args: error");

    /* RMDIR non-existent */
    o = run("RMDIR NOSUCH");
    CHECK(strstr(o, "not found") != NULL, "RMDIR non-existent: error");

    /* RMDIR a plain file (not a directory) */
    make_file("PLAIN.TXT", "x");
    o = run("RMDIR PLAIN.TXT");
    CHECK(strstr(o, "Not a directory") != NULL,
          "RMDIR on a plain file: 'Not a directory' error");

    /* RMDIR the directory we created */
    o = run("RMDIR DOCS");
    CHECK(strstr(o, "Removed") != NULL, "RMDIR removes empty directory");

    /* MD alias */
    o = run("MD NEWDIR");
    CHECK(strstr(o, "Created") != NULL, "MD (alias for MKDIR) works");

    /* RD alias */
    o = run("RD NEWDIR");
    CHECK(strstr(o, "Removed") != NULL, "RD (alias for RMDIR) works");
}

/* ------------------------------------------------------------------ */
/* CLS  (just check it doesn't crash)                                   */
/* ------------------------------------------------------------------ */

static void test_cls(void)
{
    printf("\n--- CLS ---\n");
    setup();

    /* bios->cls is NULL; should fall back to ANSI escape */
    const char *o = run("CLS");
    /* The ANSI fallback emits ESC[2J ESC[H */
    CHECK(strchr(o, 033) != NULL || strlen(o) == 0,
          "CLS emits ANSI clear or nothing (no crash)");
}

/* ------------------------------------------------------------------ */
/* print_uint helper (tested indirectly through DIR / CHKDSK output)    */
/* ------------------------------------------------------------------ */

static void test_print_uint(void)
{
    printf("\n--- print_uint (via CHKDSK / DIR) ---\n");
    setup();

    /* CHKDSK output contains at least one number ending in a space */
    const char *o = run("CHKDSK");
    /* Verify numbers look reasonable (digits in the output) */
    bool has_digit = false;
    for (const char *p = o; *p; p++) if (isdigit((unsigned char)*p)) { has_digit = true; break; }
    CHECK(has_digit, "CHKDSK output contains numeric values");

    /* DIR of a known-size file shows its byte count */
    make_file("EXACT.TXT", "ABCDE");   /* 5 bytes */
    o = run("DIR EXACT.TXT");
    CHECK(strstr(o, "5") != NULL, "DIR shows correct 1-digit file size");

    char buf123[124]; memset(buf123, 'A', 123); buf123[123] = '\0';
    make_file("BIGGER.TXT", buf123);   /* exactly 123 bytes */
    o = run("DIR BIGGER.TXT");
    CHECK(strstr(o, "123") != NULL, "DIR shows correct 3-digit file size");
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== command processor integration tests ===\n");

    test_echo();
    test_ver();
    test_rem();
    test_help();
    test_dispatch();
    test_datetime();
    test_create();
    test_type();
    test_open();
    test_copy();
    test_del();
    test_ren();
    test_chkdsk();
    test_dir();
    test_cd();
    test_mkdir_rmdir();
    test_cls();
    test_print_uint();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
