#include "edit.h"
#include "../kernel/fcb.h"
#include "../kernel/fileio.h"
#include "../kernel/kernel.h"
#include "../kernel/chardev.h"
#include "../kernel/keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -----------------------------------------------------------------------
 * Editor buffer
 *
 * Each line is stored as a heap-allocated, null-terminated char array.
 * Lines do not include the line ending; CRLF is added on save.
 * ----------------------------------------------------------------------- */

#define ED_INIT_LINES  64
#define ED_INIT_COLS   80
#define ED_MAX_COLS    1024   /* hard cap per line */
#define ED_TAB_WIDTH   4

typedef struct {
    char *buf;   /* null-terminated content, no newline */
    int   len;   /* strlen(buf) */
    int   cap;   /* allocated capacity including null terminator */
} edline_t;

typedef struct {
    edline_t *lines;    /* line array */
    int       nlines;   /* number of lines */
    int       lcap;     /* capacity of lines[] */

    int       row;      /* cursor row  (0-based in buffer) */
    int       col;      /* cursor col  (0-based in buffer) */
    int       top;      /* first visible row (scroll offset) */
    int       left;     /* first visible col (horizontal scroll) */

    int       scr_rows; /* usable content rows = terminal_rows - 2 */
    int       scr_cols; /* terminal columns */

    bool      modified;
    bool      running;
    char      filename[14];   /* 8.3 name + drive + null */
} ed_t;

/* -----------------------------------------------------------------------
 * Line helpers
 * ----------------------------------------------------------------------- */

static edline_t *line_ensure_cap(edline_t *l, int needed)
{
    if (needed + 1 <= l->cap) return l;
    int new_cap = l->cap * 2;
    if (new_cap < needed + 1) new_cap = needed + 1;
    char *nb = realloc(l->buf, (size_t)new_cap);
    if (!nb) return NULL;
    l->buf = nb;
    l->cap = new_cap;
    return l;
}

static edline_t make_line(const char *src, int len)
{
    edline_t l;
    l.cap = len + 1 + 16;
    l.buf = malloc((size_t)l.cap);
    if (!l.buf) { l.cap = 0; l.len = 0; return l; }
    memcpy(l.buf, src, (size_t)len);
    l.buf[len] = '\0';
    l.len = len;
    return l;
}

static edline_t make_empty_line(void)
{
    return make_line("", 0);
}

/* -----------------------------------------------------------------------
 * Buffer management
 * ----------------------------------------------------------------------- */

static int ed_ensure_lines(ed_t *e, int needed)
{
    if (needed <= e->lcap) return 0;
    int nc = e->lcap * 2;
    if (nc < needed) nc = needed;
    edline_t *nb = realloc(e->lines, (size_t)nc * sizeof(edline_t));
    if (!nb) return -1;
    e->lines = nb;
    e->lcap  = nc;
    return 0;
}

static void ed_init(ed_t *e, const char *filename, int rows, int cols)
{
    memset(e, 0, sizeof(*e));
    e->lcap     = ED_INIT_LINES;
    e->lines    = malloc((size_t)e->lcap * sizeof(edline_t));
    e->nlines   = 1;
    e->lines[0] = make_empty_line();
    e->scr_rows = rows - 2;   /* minus status bar + help bar */
    e->scr_cols = cols;
    e->running  = true;
    strncpy(e->filename, filename, sizeof(e->filename) - 1);
}

static void ed_free(ed_t *e)
{
    for (int i = 0; i < e->nlines; i++) free(e->lines[i].buf);
    free(e->lines);
}

/* Insert a new empty line at position 'at', shifting lines below down. */
static int ed_insert_line(ed_t *e, int at, edline_t l)
{
    if (ed_ensure_lines(e, e->nlines + 1) != 0) return -1;
    memmove(e->lines + at + 1, e->lines + at,
            (size_t)(e->nlines - at) * sizeof(edline_t));
    e->lines[at] = l;
    e->nlines++;
    return 0;
}

/* Remove line at 'at', shifting lines above up. */
static void ed_remove_line(ed_t *e, int at)
{
    free(e->lines[at].buf);
    memmove(e->lines + at, e->lines + at + 1,
            (size_t)(e->nlines - at - 1) * sizeof(edline_t));
    e->nlines--;
}

/* Insert character c at (row, col). */
static void ed_insert_char(ed_t *e, int row, int col, char c)
{
    edline_t *l = &e->lines[row];
    if (!line_ensure_cap(l, l->len + 1)) return;
    if (l->len + 1 >= ED_MAX_COLS) return;
    memmove(l->buf + col + 1, l->buf + col, (size_t)(l->len - col + 1));
    l->buf[col] = c;
    l->len++;
}

/* Delete character at (row, col). */
static void ed_delete_char(ed_t *e, int row, int col)
{
    edline_t *l = &e->lines[row];
    if (col >= l->len) return;
    memmove(l->buf + col, l->buf + col + 1, (size_t)(l->len - col));
    l->len--;
}

/* Split line at col (Enter key): current line becomes [0..col-1],
 * new line inserted below with [col..end]. */
static void ed_split_line(ed_t *e, int row, int col)
{
    edline_t *cur = &e->lines[row];
    edline_t  newl = make_line(cur->buf + col, cur->len - col);
    cur->len = col;
    cur->buf[col] = '\0';
    ed_insert_line(e, row + 1, newl);
}

/* Join line row+1 onto the end of line row (Backspace at col 0). */
static void ed_join_lines(ed_t *e, int row)
{
    edline_t *cur  = &e->lines[row];
    edline_t *next = &e->lines[row + 1];
    if (!line_ensure_cap(cur, cur->len + next->len)) return;
    memcpy(cur->buf + cur->len, next->buf, (size_t)next->len + 1);
    cur->len += next->len;
    ed_remove_line(e, row + 1);
}

/* -----------------------------------------------------------------------
 * FAT12 file I/O
 * ----------------------------------------------------------------------- */

static int ed_load(ed_t *e, dos_t *dos)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    const char *p = e->filename;
    dos_makefcb(dos, &p, &fcb, 0);

    if (dos_open(dos, &fcb) != DOS_OK)
        return 0;   /* new file — start empty */

    uint32_t filsiz  = fcb.filsiz;
    uint8_t *old_dma = dos->dmaadd;
    uint8_t  buf[128];
    dos_setdma(dos, buf);
    fcb.recsiz = 128;

    /* Read everything into a flat heap buffer then parse into lines. */
    char *flat = malloc(filsiz + 2);
    if (!flat) { dos_close(dos, &fcb); dos_setdma(dos, old_dma); return -1; }

    uint32_t pos = 0;
    uint8_t  rc;
    while ((rc = dos_seqrd(dos, &fcb)) == DOS_OK || rc == DOS_PARTIAL) {
        uint16_t n = 128;
        if (rc == DOS_PARTIAL) {
            uint32_t rem = filsiz % 128;
            n = rem ? (uint16_t)rem : 128;
        }
        if (pos + n > filsiz) n = (uint16_t)(filsiz - pos);
        memcpy(flat + pos, buf, n);
        pos += n;
        if (rc == DOS_PARTIAL) break;
    }
    /* Ensure the loop sees a terminating '\n' for files that don't end
     * in one.  When the file already ends in '\n', set flat[pos] to
     * '\0' so the loop does not create a spurious empty trailing line. */
    if (pos > 0 && flat[pos - 1] == '\n')
        flat[pos] = '\0';
    else
        flat[pos] = '\n';
    flat[pos + 1] = '\0';

    dos_close(dos, &fcb);
    dos_setdma(dos, old_dma);

    /* Free the default empty line. */
    free(e->lines[0].buf);
    e->nlines = 0;

    /* Split flat buffer into lines on \n (strip \r). */
    char *start = flat;
    for (uint32_t i = 0; i <= pos; i++) {
        if (flat[i] == '\n') {
            flat[i] = '\0';
            int len = (int)(flat + i - start);
            if (len > 0 && start[len - 1] == '\r') len--;
            if (ed_ensure_lines(e, e->nlines + 1) != 0) break;
            e->lines[e->nlines++] = make_line(start, len);
            start = flat + i + 1;
        }
    }
    if (e->nlines == 0)
        e->lines[e->nlines++] = make_empty_line();

    free(flat);
    return 0;
}

/* Write helper: flush a 128-byte record to disk. */
typedef struct {
    uint8_t  buf[128];
    int      pos;
    fcb_t   *fcb;
    dos_t   *dos;
    uint8_t *saved_dma;
    int      err;
} wbuf_t;

static void wbuf_flush(wbuf_t *w)
{
    if (w->pos == 0 || w->err) return;
    memset(w->buf + w->pos, 0, (size_t)(128 - w->pos));
    dos_setdma(w->dos, w->buf);
    uint8_t rc = dos_seqwrt(w->dos, w->fcb);
    if (rc != DOS_OK && rc != DOS_PARTIAL) w->err = 1;
    w->pos = 0;
}

static void wbuf_write_byte(wbuf_t *w, uint8_t c)
{
    if (w->err) return;
    w->buf[w->pos++] = c;
    if (w->pos == 128) wbuf_flush(w);
}

static void wbuf_write(wbuf_t *w, const char *data, int n)
{
    for (int i = 0; i < n; i++)
        wbuf_write_byte(w, (uint8_t)data[i]);
}

static int ed_save(ed_t *e, dos_t *dos)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    const char *p = e->filename;
    dos_makefcb(dos, &p, &fcb, 0);

    if (dos_create(dos, &fcb) != DOS_OK) return -1;
    fcb.recsiz = 128;

    uint8_t *old_dma = dos->dmaadd;
    wbuf_t w = { .pos = 0, .fcb = &fcb, .dos = dos,
                 .saved_dma = old_dma, .err = 0 };

    uint32_t exact_bytes = 0;
    for (int i = 0; i < e->nlines; i++) {
        exact_bytes += (uint32_t)e->lines[i].len + 2;   /* +2 for CRLF */
        wbuf_write(&w, e->lines[i].buf, e->lines[i].len);
        wbuf_write(&w, "\r\n", 2);
    }
    /* Flush the final partial record (pads with zeros to 128 bytes). */
    wbuf_flush(&w);

    /* Fix up the file size: dos_seqwrt reports full records; trim the
     * padding zeros added to the last record so the directory entry
     * reflects the exact byte count. */
    fcb.filsiz = exact_bytes;

    dos_setdma(dos, old_dma);
    dos_close(dos, &fcb);

    return w.err ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * Screen rendering (ANSI)
 * ----------------------------------------------------------------------- */

static void ed_ansi(const char *seq) { fputs(seq, stdout); }

static void ed_gotoxy(int row, int col)  /* 0-based */
{
    printf("\033[%d;%dH", row + 1, col + 1);
}

static void ed_render(ed_t *e)
{
    ed_ansi("\033[?25l");   /* hide cursor during redraw */

    /* ---- status bar (row 0) ---- */
    ed_gotoxy(0, 0);
    ed_ansi("\033[7m");     /* reverse video */
    printf(" EDIT: %-13s  Ln %-5d Col %-4d  %s",
           e->filename, e->row + 1, e->col + 1,
           e->modified ? "[Modified]  " : "[No changes]");
    ed_ansi("\033[0K\033[0m");   /* clear to EOL, normal video */

    /* ---- content rows ---- */
    for (int r = 0; r < e->scr_rows; r++) {
        ed_gotoxy(r + 1, 0);
        int li = e->top + r;
        if (li < e->nlines) {
            const char *text = e->lines[li].buf;
            int   len  = e->lines[li].len;
            int   start = e->left;
            int   avail = len - start;
            if (avail < 0) avail = 0;
            if (avail > e->scr_cols) avail = e->scr_cols;
            if (avail > 0) fwrite(text + start, 1, (size_t)avail, stdout);
        }
        ed_ansi("\033[0K");   /* clear to end of line */
    }

    /* ---- help bar (last row) ---- */
    ed_gotoxy(e->scr_rows + 1, 0);
    ed_ansi("\033[7m");
    printf(" ^W Save  ^X Quit  ^G Goto  Arrows Move  "
           "Home/End  PgUp/PgDn  Del/BS Delete  Esc Quit");
    ed_ansi("\033[0K\033[0m");

    /* ---- position cursor ---- */
    int vis_row = e->row - e->top;
    int vis_col = e->col - e->left;
    ed_gotoxy(vis_row + 1, vis_col);
    ed_ansi("\033[?25h");   /* show cursor */
    fflush(stdout);
}

/* -----------------------------------------------------------------------
 * Scrolling / cursor clamping
 * ----------------------------------------------------------------------- */

static void ed_clamp_col(ed_t *e)
{
    int max_col = e->lines[e->row].len;
    if (e->col > max_col) e->col = max_col;
    if (e->col < 0) e->col = 0;
}

static void ed_scroll(ed_t *e)
{
    /* Vertical */
    if (e->row < e->top)
        e->top = e->row;
    if (e->row >= e->top + e->scr_rows)
        e->top = e->row - e->scr_rows + 1;

    /* Horizontal: keep cursor visible */
    if (e->col < e->left)
        e->left = e->col;
    if (e->col >= e->left + e->scr_cols)
        e->left = e->col - e->scr_cols + 1;
}

/* -----------------------------------------------------------------------
 * Go-to-line dialog (Ctrl+G)
 * ----------------------------------------------------------------------- */

static void ed_goto_line(ed_t *e, dos_t *dos)
{
    /* Draw prompt at bottom */
    ed_gotoxy(e->scr_rows + 1, 0);
    ed_ansi("\033[7m");
    fputs(" Goto line: ", stdout);
    ed_ansi("\033[0m\033[0K");
    fflush(stdout);

    /* Read digits via DOS buffered input */
    uint8_t buf[20] = {18, 0};
    uint8_t *old_dma = dos->dmaadd;
    dos_setdma(dos, buf);
    dos_bufin(dos, buf);
    dos_setdma(dos, old_dma);

    buf[2 + buf[1]] = '\0';
    int target = atoi((char*)buf + 2);
    if (target >= 1 && target <= e->nlines) {
        e->row = target - 1;
        e->col = 0;
    }
}

/* -----------------------------------------------------------------------
 * Save confirmation dialog
 * ----------------------------------------------------------------------- */

/* Returns true if caller should proceed (discard changes). */
static bool ed_confirm_discard(ed_t *e, dos_t *dos)
{
    ed_gotoxy(e->scr_rows + 1, 0);
    ed_ansi("\033[7m");
    fputs(" Unsaved changes — save before quit? (Y=save, N=discard, C=cancel) ", stdout);
    ed_ansi("\033[0m\033[0K");
    fflush(stdout);

    while (true) {
        int k = dos->bios->getkey ? dos->bios->getkey(dos->bios)
                                  : dos->bios->in(dos->bios);
        char c = (char)toupper((unsigned char)(k & 0xFF));
        if (c == 'Y') { ed_save(e, dos); return true; }
        if (c == 'N') return true;
        if (c == 'C' || k == KEY_ESC) return false;
    }
}

/* -----------------------------------------------------------------------
 * Key handler
 * ----------------------------------------------------------------------- */

static void ed_handle_key(ed_t *e, dos_t *dos, int k)
{
    edline_t *cur = &e->lines[e->row];

    switch (k) {

    /* ---- navigation ---- */
    case KEY_UP:
        if (e->row > 0) { e->row--; ed_clamp_col(e); }
        break;
    case KEY_DOWN:
        if (e->row < e->nlines - 1) { e->row++; ed_clamp_col(e); }
        break;
    case KEY_LEFT:
        if (e->col > 0) { e->col--; }
        else if (e->row > 0) { e->row--; e->col = e->lines[e->row].len; }
        break;
    case KEY_RIGHT:
        if (e->col < cur->len) { e->col++; }
        else if (e->row < e->nlines - 1) { e->row++; e->col = 0; }
        break;
    case KEY_HOME:
        e->col = 0;
        break;
    case KEY_END:
        e->col = cur->len;
        break;
    case KEY_CTRL_HOME:
        e->row = 0; e->col = 0; e->top = 0;
        break;
    case KEY_CTRL_END:
        e->row = e->nlines - 1;
        e->col = e->lines[e->row].len;
        break;
    case KEY_PGUP:
        e->row -= e->scr_rows;
        if (e->row < 0) e->row = 0;
        e->top -= e->scr_rows;
        if (e->top < 0) e->top = 0;
        ed_clamp_col(e);
        break;
    case KEY_PGDN:
        e->row += e->scr_rows;
        if (e->row >= e->nlines) e->row = e->nlines - 1;
        e->top += e->scr_rows;
        if (e->top > e->row) e->top = e->row;
        ed_clamp_col(e);
        break;

    /* ---- editing ---- */
    case KEY_ENTER:
        ed_split_line(e, e->row, e->col);
        e->row++;
        e->col = 0;
        e->modified = true;
        break;

    case KEY_BACKSPACE:
    case 0x7F:
        if (e->col > 0) {
            ed_delete_char(e, e->row, --e->col);
            e->modified = true;
        } else if (e->row > 0) {
            int prev_len = e->lines[e->row - 1].len;
            ed_join_lines(e, e->row - 1);
            e->row--;
            e->col = prev_len;
            e->modified = true;
        }
        break;

    case KEY_DEL:
        cur = &e->lines[e->row];   /* re-fetch after potential realloc */
        if (e->col < cur->len) {
            ed_delete_char(e, e->row, e->col);
            e->modified = true;
        } else if (e->row < e->nlines - 1) {
            ed_join_lines(e, e->row);
            e->modified = true;
        }
        break;

    case KEY_TAB:
        for (int i = 0; i < ED_TAB_WIDTH; i++) {
            ed_insert_char(e, e->row, e->col, ' ');
            e->col++;
        }
        e->modified = true;
        break;

    /* ---- commands ---- */
    case KEY_CTRL_W:                  /* Ctrl+W — Save (Write) */
        if (ed_save(e, dos) == 0) e->modified = false;
        break;

    case KEY_CTRL_G:                  /* Ctrl+G — Goto line */
        ed_goto_line(e, dos);
        break;

    case KEY_ESC:                     /* Esc    — Quit       */
    case KEY_CTRL_X:                  /* Ctrl+X — Quit (eXit)*/
        if (!e->modified || ed_confirm_discard(e, dos))
            e->running = false;
        break;

    default:
        /* Printable ASCII — insert at cursor */
        if (k >= 0x20 && k <= 0x7E) {
            ed_insert_char(e, e->row, e->col, (char)k);
            e->col++;
            e->modified = true;
        }
        break;
    }

    ed_scroll(e);
}

/* -----------------------------------------------------------------------
 * Public entry point
 * ----------------------------------------------------------------------- */

void editor_run(dos_t *dos, const char *filename)
{
    if (!dos->bios->getkey) {
        /* No extended keyboard support — cannot run screen editor */
        printf("EDIT requires an interactive terminal.\r\n");
        fflush(stdout);
        return;
    }

    int rows = 24, cols = 80;
    if (dos->bios->getscreensize)
        dos->bios->getscreensize(dos->bios, &rows, &cols);

    if (rows < 6 || cols < 20) {
        printf("Terminal too small for EDIT (need at least 20x6).\r\n");
        fflush(stdout);
        return;
    }

    ed_t e;
    ed_init(&e, filename, rows, cols);

    if (ed_load(&e, dos) != 0) {
        printf("Cannot read file: %s\r\n", filename);
        ed_free(&e);
        return;
    }

    /* Clear screen and enter editor loop */
    ed_ansi("\033[2J");
    ed_render(&e);

    while (e.running) {
        int k = dos->bios->getkey(dos->bios);
        ed_handle_key(&e, dos, k);
        ed_render(&e);
    }

    /* Restore clean terminal state */
    ed_ansi("\033[2J\033[H\033[0m");
    fflush(stdout);
    ed_free(&e);
}
