/* tests/test_edit.c — Unit tests for the screen editor (edit.c).
 *
 * Strategy: edit.c is included directly so all static helpers are
 * accessible.  Kernel/DOS functions that the editor calls are provided
 * as stubs here.  Rendering (ANSI codes via printf) is suppressed during
 * calls that would produce terminal noise by redirecting stdout to
 * /dev/null for the duration of that call.
 *
 * Test coverage:
 *   · Buffer operations  — make_line, ed_insert_char, ed_delete_char,
 *                          ed_split_line, ed_join_lines
 *   · Cursor / scroll    — ed_clamp_col, ed_scroll
 *   · Key handling       — all printable + every special key in ed_handle_key
 *   · File I/O           — ed_load (CRLF, LF, empty), ed_save (content check),
 *                          round-trip load→save
 *
 * Build:
 *   cc -std=c11 -Wall -I. -o tests/test_edit tests/test_edit.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include "src/kernel/types.h"
#include "src/kernel/bios.h"
#include "src/kernel/dos.h"
#include "src/kernel/chardev.h"
#include "src/kernel/fcb.h"
#include "src/kernel/fileio.h"
#include "src/kernel/kernel.h"
#include "src/kernel/keys.h"

/* ------------------------------------------------------------------ */
/* Minimal test harness (writes to stderr so ANSI stdout is separate)   */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

#define CHECK(cond, msg) \
    do { \
        if (cond) { fprintf(stderr, "PASS  " msg "\n"); g_pass++; } \
        else      { fprintf(stderr, "FAIL  " msg "  [line %d]\n", \
                            __LINE__); g_fail++; } \
    } while (0)

/* ------------------------------------------------------------------ */
/* Stdout suppression (for calls that emit ANSI sequences)              */
/* ------------------------------------------------------------------ */

static int g_saved_stdout = -1;

static void stdout_quiet(void)
{
    fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
}

static void stdout_loud(void)
{
    if (g_saved_stdout >= 0) {
        fflush(stdout);
        dup2(g_saved_stdout, STDOUT_FILENO);
        close(g_saved_stdout);
        g_saved_stdout = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Fake file system state                                               */
/* ------------------------------------------------------------------ */

static dos_t g_dos;
static bios_t g_bios;

/* ---- load stub (dos_open / dos_seqrd) ---- */
static const char *g_load_content;   /* file text to serve */
static int         g_load_pos;

/* ---- save stub (dos_create / dos_seqwrt / dos_close) ---- */
static char g_save_buf[8192];
static int  g_save_pos;

/* ---- bufin stub (dos_bufin — used by Ctrl+G goto dialog) ---- */
static char g_bufin_result[32];

/* ---- bios confirm-dialog key (used by ed_confirm_discard) ---- */
static int g_confirm_key = 'N';

/* ------------------------------------------------------------------ */
/* Kernel stubs                                                          */
/* ------------------------------------------------------------------ */

void dos_setdma(dos_t *dos, uint8_t *addr)
{
    dos->dmaadd = addr;
}

uint8_t dos_makefcb(dos_t *dos, const char **src, fcb_t *fcb, uint8_t flags)
{
    (void)dos; (void)flags;
    while (**src && **src != ' ') (*src)++;
    memset(fcb, 0, sizeof *fcb);
    return 0;
}

uint8_t dos_open(dos_t *dos, fcb_t *fcb)
{
    (void)dos;
    if (!g_load_content) return DOS_ERR;
    g_load_pos   = 0;
    fcb->filsiz  = (uint32_t)strlen(g_load_content);
    fcb->recsiz  = 128;
    return DOS_OK;
}

uint8_t dos_seqrd(dos_t *dos, fcb_t *fcb)
{
    (void)fcb;
    size_t len = g_load_content ? strlen(g_load_content) : 0;
    if ((size_t)g_load_pos >= len) return DOS_EOF;
    size_t remaining = len - (size_t)g_load_pos;
    size_t n = remaining >= 128 ? 128 : remaining;
    memcpy(dos->dmaadd, g_load_content + g_load_pos, n);
    if (n < 128) memset((char *)dos->dmaadd + n, 0, 128 - n);
    g_load_pos += (int)n;
    return ((size_t)g_load_pos >= len) ? DOS_PARTIAL : DOS_OK;
}

uint8_t dos_create(dos_t *dos, fcb_t *fcb)
{
    (void)dos; (void)fcb;
    g_save_pos = 0;
    g_save_buf[0] = '\0';
    return DOS_OK;
}

uint8_t dos_seqwrt(dos_t *dos, fcb_t *fcb)
{
    (void)fcb;
    if (g_save_pos + 128 < (int)sizeof(g_save_buf)) {
        memcpy(g_save_buf + g_save_pos, dos->dmaadd, 128);
        g_save_pos += 128;
        g_save_buf[g_save_pos] = '\0';
    }
    return DOS_OK;
}

uint8_t dos_close(dos_t *dos, fcb_t *fcb)
{
    (void)dos;
    /* Trim to exact file size set by ed_save */
    if (fcb->filsiz < (uint32_t)g_save_pos)
        g_save_pos = (int)fcb->filsiz;
    g_save_buf[g_save_pos] = '\0';
    return DOS_OK;
}

void dos_bufin(dos_t *dos, uint8_t *buf)
{
    (void)dos;
    strncpy((char *)buf + 2, g_bufin_result, buf[0] - 1);
    buf[1] = (uint8_t)strlen(g_bufin_result);
    buf[2 + buf[1]] = '\0';
}

/* ---- mock BIOS for confirm dialog ---- */
static int mock_getkey(bios_t *b) { (void)b; return g_confirm_key; }
static void mock_getscreensize(bios_t *b, int *rows, int *cols)
{
    (void)b; *rows = 24; *cols = 80;
}

/* ------------------------------------------------------------------ */
/* Include edit.c — exposes all static functions                        */
/* ------------------------------------------------------------------ */

#include "../src/command/edit.c"

/* ------------------------------------------------------------------ */
/* Helpers                                                               */
/* ------------------------------------------------------------------ */

/* Allocate a fresh editor with N pre-populated lines (null-terminated). */
static ed_t *make_ed(int rows, int cols, const char **content, int nlines)
{
    ed_t *e = calloc(1, sizeof *e);
    ed_init(e, "TEST.TXT", rows, cols);
    /* Replace the single empty line with provided content */
    free(e->lines[0].buf);
    e->nlines = 0;
    for (int i = 0; i < nlines; i++) {
        ed_ensure_lines(e, e->nlines + 1);
        e->lines[e->nlines++] = make_line(content[i], (int)strlen(content[i]));
    }
    if (e->nlines == 0)
        e->lines[e->nlines++] = make_empty_line();
    return e;
}

static void free_ed(ed_t *e) { ed_free(e); free(e); }

/* ------------------------------------------------------------------ */
/* make_line / ed_init                                                  */
/* ------------------------------------------------------------------ */

static void test_make_line(void)
{
    fprintf(stderr, "\n--- make_line / ed_init ---\n");

    /* make_line copies text and sets len */
    edline_t l = make_line("HELLO", 5);
    CHECK(strcmp(l.buf, "HELLO") == 0, "make_line: buf matches source");
    CHECK(l.len == 5, "make_line: len set correctly");
    CHECK(l.cap > 5, "make_line: cap has room for NUL");
    free(l.buf);

    /* make_empty_line gives zero-length line */
    edline_t e = make_empty_line();
    CHECK(e.len == 0, "make_empty_line: len == 0");
    CHECK(e.buf != NULL, "make_empty_line: buf allocated");
    CHECK(e.buf[0] == '\0', "make_empty_line: buf is NUL string");
    free(e.buf);

    /* ed_init: single empty line, cursor at (0,0), not modified */
    ed_t ed;
    ed_init(&ed, "FOO.TXT", 24, 80);
    CHECK(ed.nlines == 1, "ed_init: starts with 1 line");
    CHECK(ed.lines[0].len == 0, "ed_init: initial line is empty");
    CHECK(ed.row == 0 && ed.col == 0, "ed_init: cursor at (0,0)");
    CHECK(!ed.modified, "ed_init: not modified");
    CHECK(ed.running, "ed_init: running == true");
    CHECK(ed.scr_rows == 22, "ed_init: scr_rows = rows-2");
    CHECK(ed.scr_cols == 80, "ed_init: scr_cols = cols");
    ed_free(&ed);
}

/* ------------------------------------------------------------------ */
/* ed_insert_char                                                        */
/* ------------------------------------------------------------------ */

static void test_insert_char(void)
{
    fprintf(stderr, "\n--- ed_insert_char ---\n");
    ed_t ed;
    ed_init(&ed, "T", 24, 80);

    /* Insert at col 0 on empty line */
    ed_insert_char(&ed, 0, 0, 'A');
    CHECK(strcmp(ed.lines[0].buf, "A") == 0, "insert at col 0: first char");
    CHECK(ed.lines[0].len == 1, "insert: len updated");

    /* Prepend: insert at col 0 with existing content */
    ed_insert_char(&ed, 0, 0, 'Z');
    CHECK(strcmp(ed.lines[0].buf, "ZA") == 0, "prepend: inserts before existing");

    /* Insert in the middle */
    ed_insert_char(&ed, 0, 1, 'X');
    CHECK(strcmp(ed.lines[0].buf, "ZXA") == 0, "insert middle: shifts right");

    /* Append at the end */
    ed_insert_char(&ed, 0, 3, '!');
    CHECK(strcmp(ed.lines[0].buf, "ZXA!") == 0, "append: extends line");

    /* Insert builds a word correctly with sequential calls */
    free(ed.lines[0].buf);
    ed.lines[0] = make_empty_line();
    const char *word = "HELLO";
    for (int i = 0; word[i]; i++) {
        ed_insert_char(&ed, 0, i, word[i]);
    }
    CHECK(strcmp(ed.lines[0].buf, "HELLO") == 0, "sequential inserts build word");
    CHECK(ed.lines[0].len == 5, "sequential inserts: len == 5");

    ed_free(&ed);
}

/* ------------------------------------------------------------------ */
/* ed_delete_char                                                        */
/* ------------------------------------------------------------------ */

static void test_delete_char(void)
{
    fprintf(stderr, "\n--- ed_delete_char ---\n");
    const char *init[] = { "ABCDE" };
    ed_t *e = make_ed(24, 80, init, 1);

    /* Delete first character */
    ed_delete_char(e, 0, 0);
    CHECK(strcmp(e->lines[0].buf, "BCDE") == 0, "delete at col 0 removes first char");

    /* Delete middle character */
    ed_delete_char(e, 0, 1);   /* removes 'C' from "BCDE" */
    CHECK(strcmp(e->lines[0].buf, "BDE") == 0, "delete middle char");

    /* Delete last character */
    ed_delete_char(e, 0, 2);   /* removes 'E' from "BDE" */
    CHECK(strcmp(e->lines[0].buf, "BD") == 0, "delete last char");

    /* Delete at col >= len is a no-op */
    ed_delete_char(e, 0, 5);   /* out of bounds */
    CHECK(strcmp(e->lines[0].buf, "BD") == 0, "delete out-of-bounds: no-op");
    CHECK(e->lines[0].len == 2, "delete out-of-bounds: len unchanged");

    /* Delete all chars one by one */
    ed_delete_char(e, 0, 0);
    ed_delete_char(e, 0, 0);
    CHECK(e->lines[0].len == 0, "delete all chars: len == 0");
    CHECK(e->lines[0].buf[0] == '\0', "delete all chars: empty string");

    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* ed_split_line (Enter key)                                            */
/* ------------------------------------------------------------------ */

static void test_split_line(void)
{
    fprintf(stderr, "\n--- ed_split_line ---\n");
    const char *init[] = { "ABCDE" };
    ed_t *e;

    /* Split at col 0: new empty line before, original untouched */
    e = make_ed(24, 80, init, 1);
    ed_split_line(e, 0, 0);
    CHECK(e->nlines == 2, "split at 0: 2 lines");
    CHECK(strcmp(e->lines[0].buf, "") == 0,    "split at 0: first line empty");
    CHECK(strcmp(e->lines[1].buf, "ABCDE") == 0, "split at 0: second has original");
    free_ed(e);

    /* Split at middle: two halves */
    e = make_ed(24, 80, init, 1);
    ed_split_line(e, 0, 2);
    CHECK(e->nlines == 2, "split at 2: 2 lines");
    CHECK(strcmp(e->lines[0].buf, "AB") == 0,    "split at 2: first = left half");
    CHECK(strcmp(e->lines[1].buf, "CDE") == 0,   "split at 2: second = right half");
    free_ed(e);

    /* Split at end: new empty line appended */
    e = make_ed(24, 80, init, 1);
    ed_split_line(e, 0, 5);
    CHECK(e->nlines == 2, "split at end: 2 lines");
    CHECK(strcmp(e->lines[0].buf, "ABCDE") == 0, "split at end: original intact");
    CHECK(strcmp(e->lines[1].buf, "") == 0,       "split at end: new line empty");
    free_ed(e);

    /* Split an already-split buffer (multi-line) */
    const char *twolines[] = { "FOO", "BAR" };
    e = make_ed(24, 80, twolines, 2);
    ed_split_line(e, 0, 1);          /* split "FOO" at col 1 → "F", "OO", "BAR" */
    CHECK(e->nlines == 3, "split in multi-line: 3 lines");
    CHECK(strcmp(e->lines[0].buf, "F") == 0,   "split multi: line 0");
    CHECK(strcmp(e->lines[1].buf, "OO") == 0,  "split multi: line 1");
    CHECK(strcmp(e->lines[2].buf, "BAR") == 0, "split multi: line 2 unchanged");
    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* ed_join_lines (Backspace at col 0)                                   */
/* ------------------------------------------------------------------ */

static void test_join_lines(void)
{
    fprintf(stderr, "\n--- ed_join_lines ---\n");
    const char *init[] = { "AB", "CD", "EF" };
    ed_t *e;

    /* Join first two lines */
    e = make_ed(24, 80, init, 3);
    ed_join_lines(e, 0);
    CHECK(e->nlines == 2, "join: line count decremented");
    CHECK(strcmp(e->lines[0].buf, "ABCD") == 0, "join: content merged");
    CHECK(strcmp(e->lines[1].buf, "EF") == 0,   "join: third line not affected");
    free_ed(e);

    /* Join second and third lines */
    e = make_ed(24, 80, init, 3);
    ed_join_lines(e, 1);
    CHECK(e->nlines == 2, "join second+third: count 2");
    CHECK(strcmp(e->lines[0].buf, "AB") == 0,   "join second+third: first intact");
    CHECK(strcmp(e->lines[1].buf, "CDEF") == 0, "join second+third: merged");
    free_ed(e);

    /* Join with an empty next line */
    const char *withempty[] = { "HELLO", "" };
    e = make_ed(24, 80, withempty, 2);
    int prev_len = e->lines[0].len;
    ed_join_lines(e, 0);
    CHECK(e->nlines == 1, "join with empty next: 1 line");
    CHECK(e->lines[0].len == prev_len, "join with empty: len unchanged");
    CHECK(strcmp(e->lines[0].buf, "HELLO") == 0, "join with empty: content intact");
    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* ed_clamp_col                                                          */
/* ------------------------------------------------------------------ */

static void test_clamp_col(void)
{
    fprintf(stderr, "\n--- ed_clamp_col ---\n");
    const char *init[] = { "ABC" };   /* len == 3 */
    ed_t *e = make_ed(24, 80, init, 1);

    /* col past line length → clamped to len */
    e->col = 10;
    ed_clamp_col(e);
    CHECK(e->col == 3, "clamp: col > len → clamped to len");

    /* col exactly at len → unchanged */
    e->col = 3;
    ed_clamp_col(e);
    CHECK(e->col == 3, "clamp: col == len → unchanged");

    /* col within line → unchanged */
    e->col = 1;
    ed_clamp_col(e);
    CHECK(e->col == 1, "clamp: col < len → unchanged");

    /* col on empty line always clamped to 0 */
    e->lines[0].len = 0;
    e->lines[0].buf[0] = '\0';
    e->col = 5;
    ed_clamp_col(e);
    CHECK(e->col == 0, "clamp: empty line → col == 0");

    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* ed_scroll                                                             */
/* ------------------------------------------------------------------ */

static void test_scroll(void)
{
    fprintf(stderr, "\n--- ed_scroll ---\n");
    /* Build a buffer with enough lines to scroll (scr_rows=5 for small window) */
    const char *lines[30];
    for (int i = 0; i < 30; i++) lines[i] = "X";
    ed_t *e = make_ed(7, 40, lines, 30);  /* scr_rows = 7-2 = 5 */

    /* Cursor above top → top moves up */
    e->top = 10; e->row = 8;
    ed_scroll(e);
    CHECK(e->top == 8, "scroll up: top follows cursor above");

    /* Cursor below visible area → top adjusts so cursor is last visible */
    e->top = 0; e->row = 6;   /* scr_rows=5 → visible rows 0..4; row 6 is off */
    ed_scroll(e);
    CHECK(e->top == 2, "scroll down: top = row - scr_rows + 1");

    /* Cursor within visible range → no change */
    e->top = 5; e->row = 7;   /* rows 5..9 visible; row 7 is in range */
    ed_scroll(e);
    CHECK(e->top == 5, "scroll: no change when cursor in range");

    /* Horizontal: cursor left of left margin */
    e->left = 20; e->col = 15;
    ed_scroll(e);
    CHECK(e->left == 15, "hscroll: left follows cursor left of margin");

    /* Horizontal: cursor past right edge → left adjusts */
    e->left = 0; e->col = 42;   /* scr_cols=40 → visible 0..39; col 42 is off */
    ed_scroll(e);
    CHECK(e->left == 3, "hscroll: left = col - scr_cols + 1");

    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* Key handling — editing                                               */
/* ------------------------------------------------------------------ */

static void test_key_editing(void)
{
    fprintf(stderr, "\n--- key editing ---\n");
    const char *init[] = { "HELLO", "WORLD" };
    ed_t *e;

    /* Printable char: inserted at cursor, col advances, modified set */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 0;
    ed_handle_key(e, NULL, 'X');
    CHECK(strcmp(e->lines[0].buf, "XHELLO") == 0, "printable: inserted at col 0");
    CHECK(e->col == 1, "printable: col advances");
    CHECK(e->modified, "printable: sets modified");
    free_ed(e);

    /* Multiple printable chars build a string */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 5;   /* after 'O' in "HELLO" */
    ed_handle_key(e, NULL, ' ');
    ed_handle_key(e, NULL, 'H');
    ed_handle_key(e, NULL, 'I');
    CHECK(strcmp(e->lines[0].buf, "HELLO HI") == 0, "typing builds string");
    free_ed(e);

    /* Backspace mid-line: deletes char before cursor */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 3;
    ed_handle_key(e, NULL, KEY_BACKSPACE);
    CHECK(strcmp(e->lines[0].buf, "HELO") == 0, "backspace mid-line: removes char before");
    CHECK(e->col == 2, "backspace: col decrements");
    CHECK(e->modified, "backspace: sets modified");
    free_ed(e);

    /* Backspace at col 0, row > 0: join with previous line */
    e = make_ed(24, 80, init, 2);
    e->row = 1; e->col = 0;
    int prev_len = e->lines[0].len;
    ed_handle_key(e, NULL, KEY_BACKSPACE);
    CHECK(e->nlines == 1, "backspace at col 0: join reduces line count");
    CHECK(strcmp(e->lines[0].buf, "HELLOWORLD") == 0, "backspace join: content merged");
    CHECK(e->row == 0, "backspace join: row moves up");
    CHECK(e->col == prev_len, "backspace join: col at junction");
    free_ed(e);

    /* Backspace at col 0, row 0: no-op */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 0;
    ed_handle_key(e, NULL, KEY_BACKSPACE);
    CHECK(e->nlines == 2, "backspace at (0,0): no-op");
    CHECK(strcmp(e->lines[0].buf, "HELLO") == 0, "backspace at (0,0): content unchanged");
    free_ed(e);

    /* 0x7F (DEL char) behaves like KEY_BACKSPACE */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 3;
    ed_handle_key(e, NULL, 0x7F);
    CHECK(strcmp(e->lines[0].buf, "HELO") == 0, "0x7F: same as backspace");
    free_ed(e);

    /* KEY_DEL mid-line: deletes char AT cursor */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 2;   /* cursor on 'L' */
    ed_handle_key(e, NULL, KEY_DEL);
    CHECK(strcmp(e->lines[0].buf, "HELO") == 0, "KEY_DEL: removes char at cursor");
    CHECK(e->col == 2, "KEY_DEL: col unchanged");
    CHECK(e->modified, "KEY_DEL: sets modified");
    free_ed(e);

    /* KEY_DEL at EOL: joins next line */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 5;   /* EOL of "HELLO" */
    ed_handle_key(e, NULL, KEY_DEL);
    CHECK(e->nlines == 1, "KEY_DEL at EOL: joins next line");
    CHECK(strcmp(e->lines[0].buf, "HELLOWORLD") == 0, "KEY_DEL join: content merged");
    CHECK(e->row == 0, "KEY_DEL join: row unchanged");
    free_ed(e);

    /* KEY_DEL at last line EOL: no-op */
    e = make_ed(24, 80, init, 2);
    e->row = 1; e->col = 5;
    ed_handle_key(e, NULL, KEY_DEL);
    CHECK(e->nlines == 2, "KEY_DEL at last line EOL: no-op");
    free_ed(e);

    /* KEY_ENTER (split line) */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 2;
    ed_handle_key(e, NULL, KEY_ENTER);
    CHECK(e->nlines == 3, "KEY_ENTER: line count increases");
    CHECK(strcmp(e->lines[0].buf, "HE") == 0,  "KEY_ENTER: first part");
    CHECK(strcmp(e->lines[1].buf, "LLO") == 0, "KEY_ENTER: second part");
    CHECK(e->row == 1 && e->col == 0, "KEY_ENTER: cursor at start of new line");
    CHECK(e->modified, "KEY_ENTER: sets modified");
    free_ed(e);

    /* KEY_TAB inserts 4 spaces */
    e = make_ed(24, 80, init, 2);
    e->row = 0; e->col = 0;
    ed_handle_key(e, NULL, KEY_TAB);
    CHECK(strncmp(e->lines[0].buf, "    ", 4) == 0, "KEY_TAB: 4 spaces inserted");
    CHECK(e->col == 4, "KEY_TAB: col advances by 4");
    CHECK(e->modified, "KEY_TAB: sets modified");
    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* Key handling — navigation                                            */
/* ------------------------------------------------------------------ */

static void test_key_navigation(void)
{
    fprintf(stderr, "\n--- key navigation ---\n");
    const char *init[] = { "HELLO", "WORLD", "DONE" };
    ed_t *e;

    /* KEY_UP */
    e = make_ed(24, 80, init, 3);
    e->row = 1; e->col = 3;
    ed_handle_key(e, NULL, KEY_UP);
    CHECK(e->row == 0, "KEY_UP: row decrements");
    CHECK(e->col == 3, "KEY_UP: col preserved if in range");
    free_ed(e);

    /* KEY_UP at row 0: no-op */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 2;
    ed_handle_key(e, NULL, KEY_UP);
    CHECK(e->row == 0, "KEY_UP at row 0: no-op");
    free_ed(e);

    /* KEY_DOWN */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 2;
    ed_handle_key(e, NULL, KEY_DOWN);
    CHECK(e->row == 1, "KEY_DOWN: row increments");
    free_ed(e);

    /* KEY_DOWN at last row: no-op */
    e = make_ed(24, 80, init, 3);
    e->row = 2; e->col = 0;
    ed_handle_key(e, NULL, KEY_DOWN);
    CHECK(e->row == 2, "KEY_DOWN at last row: no-op");
    free_ed(e);

    /* KEY_UP clamps col when next line is shorter */
    e = make_ed(24, 80, init, 3);
    e->row = 2; e->col = 4;   /* "DONE" len=4, col=4 (EOL) */
    /* set line 1 to be shorter */
    free(e->lines[1].buf);
    e->lines[1] = make_line("HI", 2);
    e->row = 2; e->col = 4;
    ed_handle_key(e, NULL, KEY_UP);  /* move to "HI", col clamped to 2 */
    CHECK(e->row == 1 && e->col == 2, "KEY_UP clamps col to shorter line");
    free_ed(e);

    /* KEY_LEFT mid-line */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 3;
    ed_handle_key(e, NULL, KEY_LEFT);
    CHECK(e->col == 2, "KEY_LEFT mid-line: col decrements");
    CHECK(e->row == 0, "KEY_LEFT mid-line: row unchanged");
    free_ed(e);

    /* KEY_LEFT at col 0, row > 0: wraps to end of previous line */
    e = make_ed(24, 80, init, 3);
    e->row = 1; e->col = 0;
    ed_handle_key(e, NULL, KEY_LEFT);
    CHECK(e->row == 0, "KEY_LEFT wrap: row decrements");
    CHECK(e->col == e->lines[0].len, "KEY_LEFT wrap: col == prev line length");
    free_ed(e);

    /* KEY_LEFT at (0,0): no-op */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 0;
    ed_handle_key(e, NULL, KEY_LEFT);
    CHECK(e->row == 0 && e->col == 0, "KEY_LEFT at (0,0): no-op");
    free_ed(e);

    /* KEY_RIGHT mid-line */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 2;
    ed_handle_key(e, NULL, KEY_RIGHT);
    CHECK(e->col == 3, "KEY_RIGHT mid-line: col increments");
    CHECK(e->row == 0, "KEY_RIGHT mid-line: row unchanged");
    free_ed(e);

    /* KEY_RIGHT at EOL, not last line: wraps to start of next line */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 5;   /* EOL of "HELLO" */
    ed_handle_key(e, NULL, KEY_RIGHT);
    CHECK(e->row == 1 && e->col == 0, "KEY_RIGHT wrap: goes to start of next line");
    free_ed(e);

    /* KEY_RIGHT at last line EOL: no-op */
    e = make_ed(24, 80, init, 3);
    e->row = 2; e->col = 4;
    ed_handle_key(e, NULL, KEY_RIGHT);
    CHECK(e->row == 2 && e->col == 4, "KEY_RIGHT at last line EOL: no-op");
    free_ed(e);

    /* KEY_HOME */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 4;
    ed_handle_key(e, NULL, KEY_HOME);
    CHECK(e->col == 0, "KEY_HOME: col = 0");
    CHECK(e->row == 0, "KEY_HOME: row unchanged");
    free_ed(e);

    /* KEY_END */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 0;
    ed_handle_key(e, NULL, KEY_END);
    CHECK(e->col == 5, "KEY_END: col = line length");
    CHECK(e->row == 0, "KEY_END: row unchanged");
    free_ed(e);

    /* KEY_CTRL_HOME */
    e = make_ed(24, 80, init, 3);
    e->row = 2; e->col = 3; e->top = 2;
    ed_handle_key(e, NULL, KEY_CTRL_HOME);
    CHECK(e->row == 0 && e->col == 0, "KEY_CTRL_HOME: cursor at (0,0)");
    CHECK(e->top == 0, "KEY_CTRL_HOME: top reset to 0");
    free_ed(e);

    /* KEY_CTRL_END */
    e = make_ed(24, 80, init, 3);
    e->row = 0; e->col = 0;
    ed_handle_key(e, NULL, KEY_CTRL_END);
    CHECK(e->row == 2, "KEY_CTRL_END: cursor at last line");
    CHECK(e->col == (int)strlen("DONE"), "KEY_CTRL_END: col at end of last line");
    free_ed(e);

    /* KEY_PGUP */
    const char *many[30]; for (int i=0;i<30;i++) many[i]="X";
    e = make_ed(7, 80, many, 30);  /* scr_rows=5 */
    e->row = 20; e->top = 16;
    ed_handle_key(e, NULL, KEY_PGUP);
    CHECK(e->row == 15, "KEY_PGUP: row -= scr_rows");
    CHECK(e->top == 11, "KEY_PGUP: top -= scr_rows");
    free_ed(e);

    /* KEY_PGUP at top: clamps to 0 */
    e = make_ed(7, 80, many, 30);
    e->row = 2; e->top = 1;
    ed_handle_key(e, NULL, KEY_PGUP);
    CHECK(e->row == 0, "KEY_PGUP at top: row clamps to 0");
    CHECK(e->top == 0, "KEY_PGUP at top: top clamps to 0");
    free_ed(e);

    /* KEY_PGDN */
    e = make_ed(7, 80, many, 30);
    e->row = 5; e->top = 3;
    ed_handle_key(e, NULL, KEY_PGDN);
    CHECK(e->row == 10, "KEY_PGDN: row += scr_rows");
    free_ed(e);

    /* KEY_PGDN at bottom: clamps to last line */
    e = make_ed(7, 80, many, 30);
    e->row = 28; e->top = 25;
    ed_handle_key(e, NULL, KEY_PGDN);
    CHECK(e->row == 29, "KEY_PGDN at bottom: row clamps to nlines-1");
    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* Key handling — commands                                              */
/* ------------------------------------------------------------------ */

static void test_key_commands(void)
{
    fprintf(stderr, "\n--- key commands ---\n");
    const char *init[] = { "HELLO" };
    ed_t *e;

    /* KEY_CTRL_X on unmodified buffer → exits immediately, no dialog */
    e = make_ed(24, 80, init, 1);
    e->modified = false;
    ed_handle_key(e, NULL, KEY_CTRL_X);
    CHECK(!e->running, "KEY_CTRL_X (unmodified): running set to false");
    free_ed(e);

    /* KEY_ESC also quits when unmodified */
    e = make_ed(24, 80, init, 1);
    e->modified = false;
    ed_handle_key(e, NULL, KEY_ESC);
    CHECK(!e->running, "KEY_ESC (unmodified): running set to false");
    free_ed(e);

    /* KEY_CTRL_X on modified buffer with 'N' (discard) → quits */
    e = make_ed(24, 80, init, 1);
    e->modified = true;
    g_confirm_key = 'N';
    stdout_quiet();
    ed_handle_key(e, &g_dos, KEY_CTRL_X);
    stdout_loud();
    CHECK(!e->running, "KEY_CTRL_X (modified, N): running false after discard");
    free_ed(e);

    /* KEY_CTRL_X on modified buffer with 'C' (cancel) → stays running */
    e = make_ed(24, 80, init, 1);
    e->modified = true;
    g_confirm_key = 'C';
    stdout_quiet();
    ed_handle_key(e, &g_dos, KEY_CTRL_X);
    stdout_loud();
    CHECK(e->running, "KEY_CTRL_X (modified, C): running stays true on cancel");
    free_ed(e);

    /* KEY_CTRL_W saves file and clears modified flag */
    e = make_ed(24, 80, init, 1);
    e->modified = true;
    ed_handle_key(e, &g_dos, KEY_CTRL_W);
    CHECK(!e->modified, "KEY_CTRL_W: clears modified flag after save");
    CHECK(strstr(g_save_buf, "HELLO\r\n") != NULL, "KEY_CTRL_W: content written to disk");
    free_ed(e);
}

/* ------------------------------------------------------------------ */
/* File I/O — ed_load                                                   */
/* ------------------------------------------------------------------ */

static void test_load(void)
{
    fprintf(stderr, "\n--- ed_load ---\n");
    ed_t ed;

    /* File not found → start with single empty line */
    ed_init(&ed, "NOFILE.TXT", 24, 80);
    g_load_content = NULL;
    ed_load(&ed, &g_dos);
    CHECK(ed.nlines == 1, "load non-existent: 1 empty line");
    CHECK(ed.lines[0].len == 0, "load non-existent: line is empty");
    ed_free(&ed);

    /* Single line, CRLF terminated */
    ed_init(&ed, "T.TXT", 24, 80);
    g_load_content = "HELLO\r\n";
    ed_load(&ed, &g_dos);
    CHECK(ed.nlines == 1, "load single CRLF line: 1 line");
    CHECK(strcmp(ed.lines[0].buf, "HELLO") == 0, "load single CRLF: content without CRLF");
    ed_free(&ed);

    /* Single line, LF-only terminated */
    ed_init(&ed, "T.TXT", 24, 80);
    g_load_content = "HELLO\n";
    ed_load(&ed, &g_dos);
    CHECK(ed.nlines == 1, "load single LF line: 1 line");
    CHECK(strcmp(ed.lines[0].buf, "HELLO") == 0, "load LF-only: \\r stripped");
    ed_free(&ed);

    /* Multiple lines with CRLF */
    ed_init(&ed, "T.TXT", 24, 80);
    g_load_content = "ALPHA\r\nBETA\r\nGAMMA\r\n";
    ed_load(&ed, &g_dos);
    CHECK(ed.nlines == 3, "load 3 CRLF lines: 3 lines");
    CHECK(strcmp(ed.lines[0].buf, "ALPHA") == 0, "load multi: line 0");
    CHECK(strcmp(ed.lines[1].buf, "BETA") == 0,  "load multi: line 1");
    CHECK(strcmp(ed.lines[2].buf, "GAMMA") == 0, "load multi: line 2");
    ed_free(&ed);

    /* File with blank lines in the middle */
    ed_init(&ed, "T.TXT", 24, 80);
    g_load_content = "FIRST\r\n\r\nTHIRD\r\n";
    ed_load(&ed, &g_dos);
    CHECK(ed.nlines == 3, "load with blank line: 3 lines");
    CHECK(ed.lines[1].len == 0, "load blank line: middle line is empty");
    ed_free(&ed);

    /* Empty file (filsiz == 0) → one empty line */
    ed_init(&ed, "T.TXT", 24, 80);
    g_load_content = "";
    ed_load(&ed, &g_dos);
    CHECK(ed.nlines == 1, "load empty file: 1 empty line");
    CHECK(ed.lines[0].len == 0, "load empty file: line is empty");
    ed_free(&ed);
}

/* ------------------------------------------------------------------ */
/* File I/O — ed_save                                                   */
/* ------------------------------------------------------------------ */

static void test_save(void)
{
    fprintf(stderr, "\n--- ed_save ---\n");
    ed_t ed;

    /* Save single line → "LINE\r\n" */
    const char *one[] = { "LINE" };
    ed_t *e = make_ed(24, 80, one, 1);
    ed_save(e, &g_dos);
    CHECK(strcmp(g_save_buf, "LINE\r\n") == 0, "save single line: CRLF appended");
    CHECK(g_save_pos == 6, "save single line: exact byte count");
    free_ed(e);

    /* Save three lines */
    const char *three[] = { "ALPHA", "BETA", "GAMMA" };
    e = make_ed(24, 80, three, 3);
    ed_save(e, &g_dos);
    CHECK(strcmp(g_save_buf, "ALPHA\r\nBETA\r\nGAMMA\r\n") == 0,
          "save 3 lines: correct CRLF-joined content");
    free_ed(e);

    /* Save empty buffer → single CRLF */
    const char *empty[] = { "" };
    e = make_ed(24, 80, empty, 1);
    ed_save(e, &g_dos);
    CHECK(strcmp(g_save_buf, "\r\n") == 0, "save empty line: single CRLF");
    free_ed(e);

    /* Save with blank line in the middle */
    const char *withblank[] = { "A", "", "B" };
    e = make_ed(24, 80, withblank, 3);
    ed_save(e, &g_dos);
    CHECK(strcmp(g_save_buf, "A\r\n\r\nB\r\n") == 0,
          "save with blank line: blank becomes bare CRLF");
    free_ed(e);

    /* Round-trip: load content, then save, compare */
    ed_init(&ed, "RT.TXT", 24, 80);
    g_load_content = "ONE\r\nTWO\r\nTHREE\r\n";
    ed_load(&ed, &g_dos);
    ed_save(&ed, &g_dos);
    CHECK(strcmp(g_save_buf, "ONE\r\nTWO\r\nTHREE\r\n") == 0,
          "round-trip: load then save reproduces original");
    ed_free(&ed);

    /* Round-trip after editing: insert char then save */
    ed_init(&ed, "RT.TXT", 24, 80);
    g_load_content = "HELLO\r\nWORLD\r\n";
    ed_load(&ed, &g_dos);
    ed.row = 0; ed.col = 5;
    ed_insert_char(&ed, 0, 5, '!');   /* "HELLO!" */
    ed_save(&ed, &g_dos);
    CHECK(strcmp(g_save_buf, "HELLO!\r\nWORLD\r\n") == 0,
          "round-trip with edit: modified content saved correctly");
    ed_free(&ed);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== editor unit tests ===\n");

    memset(&g_bios, 0, sizeof g_bios);
    g_bios.getkey         = mock_getkey;
    g_bios.getscreensize  = mock_getscreensize;
    memset(&g_dos, 0, sizeof g_dos);
    g_dos.bios = &g_bios;

    test_make_line();
    test_insert_char();
    test_delete_char();
    test_split_line();
    test_join_lines();
    test_clamp_col();
    test_scroll();
    test_key_editing();
    test_key_navigation();
    test_key_commands();
    test_load();
    test_save();

    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
