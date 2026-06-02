#include "bios_host.h"
#include "../kernel/types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#  include <conio.h>
#  include <windows.h>
#else
#  include <termios.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>
#  define _BSD_SOURCE  /* for cfmakeraw on older glibc */
#endif

/* -----------------------------------------------------------------------
 * Terminal raw mode
 *
 * Full raw-mode setup so that:
 *   - IXON/IXOFF cleared: Ctrl+S and Ctrl+Q are passed to the application,
 *     not swallowed for software flow control.
 *   - ICRNL cleared: CR is not translated to NL on input.
 *   - ECHO/ICANON/IEXTEN cleared: no line editing, no extended processing.
 *   - ISIG cleared: Ctrl+C/Z do not send signals (we handle them ourselves).
 *   - VSTART/VSTOP explicitly disabled: belt-and-suspenders for XON/XOFF.
 *   - OPOST kept: output still does CR/LF translation so printf works.
 * ----------------------------------------------------------------------- */

#ifndef _WIN32
static struct termios g_saved_termios;
static int            g_raw_mode = 0;

static void enter_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    struct termios raw = g_saved_termios;

    /* Input: disable flow control, CR→NL, parity checks, stripping */
    raw.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY |
                                ICRNL | INLCR | IGNCR |
                                BRKINT | INPCK | ISTRIP);

    /* Output: keep OPOST so printf("\r\n") still works */
    raw.c_oflag |= OPOST;

    /* Control: 8-bit characters */
    raw.c_cflag |= CS8;

    /* Local: no echo, no canonical, no extended processing, no signals */
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHOE | ECHOK | ECHONL |
                                ICANON | IEXTEN | ISIG);

    /* One byte at a time, no timeout */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    /* Explicitly disable start/stop characters so Ctrl+Q and Ctrl+S
     * are never intercepted regardless of IXON/IXOFF state. */
#ifdef VSTART
    raw.c_cc[VSTART] = 0;
#endif
#ifdef VSTOP
    raw.c_cc[VSTOP]  = 0;
#endif

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode = 1;
}

static void leave_raw_mode(void)
{
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_raw_mode = 0;
    }
}

/* Shared EOF flag — set when stdin reaches end of file. */
static int g_eof = 0;

/* -----------------------------------------------------------------------
 * Low-level stdin read using read(2) instead of getchar().
 *
 * This is critical for correct escape-sequence decoding: getchar() pulls
 * bytes into a stdio internal buffer, so a subsequent select() on the raw
 * file descriptor may report "no data" even though the full ESC [ A
 * sequence is already buffered — causing the 50 ms timeout to fire and
 * KEY_ESC to be returned instead of KEY_UP.  Using read() directly keeps
 * select() and the actual byte source in sync.
 * ----------------------------------------------------------------------- */

/* Read exactly one byte from STDIN_FILENO. Returns -1 on EOF/error. */
static int stdin_readbyte(void)
{
    if (g_eof) return -1;
    uint8_t  c;
    ssize_t  n;
    do { n = read(STDIN_FILENO, &c, 1); } while (n < 0 && errno == EINTR);
    if (n <= 0) { g_eof = 1; return -1; }
    return (int)(unsigned int)c;
}

/* Return true if at least one byte is available within `us` microseconds.
 * Because we use read() (not getchar()), this accurately reflects reality. */
static int stdin_ready(int us)
{
    struct timeval tv = { 0, us };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}
#endif

/* -----------------------------------------------------------------------
 * Console BIOS callbacks
 * ----------------------------------------------------------------------- */

static int hb_stat(bios_t *b)
{
    (void)b;
    if (g_eof) return 1;
#ifdef _WIN32
    return _kbhit() ? 1 : 0;
#else
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0 ? 1 : 0;
#endif
}

static int hb_in(bios_t *b)
{
    (void)b;
    if (g_eof) return 0x1A;
#ifdef _WIN32
    int c = _getch();
    if (c < 0) { g_eof = 1; return 0x1A; }
    return c;
#else
    int c = stdin_readbyte();
    if (c < 0) return 0x1A;
    /* Translate bare LF (piped input) to CR for the DOS layer */
    if (c == '\n') return '\r';
    return c;
#endif
}

static void hb_out(bios_t *b, uint8_t c)
{
    (void)b;
    putchar(c);
    fflush(stdout);
}

static void hb_print(bios_t *b, uint8_t c)
{
    (void)b;
    fputc(c, stderr);
}

static int  hb_auxin(bios_t *b)  { (void)b; return 0; }
static void hb_auxout(bios_t *b, uint8_t c) { (void)b; (void)c; }
static void hb_flush(bios_t *b)  { (void)b; }

/* -----------------------------------------------------------------------
 * getkey — extended keyboard read with ANSI escape-sequence decoding
 * ----------------------------------------------------------------------- */

#ifndef _WIN32
/* Read one raw byte for escape-sequence parsing (no \n→\r translation). */
static int raw_getbyte(void)
{
    int c = stdin_readbyte();
    return (c < 0) ? KEY_ESC : c;
}
#endif

static int hb_getkey(bios_t *b)
{
    (void)b;
    if (g_eof) return KEY_ESC;
#ifdef _WIN32
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        /* Extended key from Windows console */
        int ext = _getch();
        switch (ext) {
        case 72: return KEY_UP;
        case 80: return KEY_DOWN;
        case 75: return KEY_LEFT;
        case 77: return KEY_RIGHT;
        case 71: return KEY_HOME;
        case 79: return KEY_END;
        case 73: return KEY_PGUP;
        case 81: return KEY_PGDN;
        case 83: return KEY_DEL;
        case 82: return KEY_INS;
        default: return KEY_ESC;
        }
    }
    return c;
#else
    int c = raw_getbyte();
    if (c != 0x1B) return c;          /* plain ASCII or control character */

    /* ESC received — check whether more bytes follow (escape sequence).
     * Because we use read() not getchar(), select() accurately reflects
     * what is buffered in the kernel: all bytes of an arrow-key sequence
     * (ESC [ A) arrive together, so 50 ms is far more than enough. */
    if (!stdin_ready(50000)) return KEY_ESC;

    int c2 = raw_getbyte();

    if (c2 == '[') {
        if (!stdin_ready(50000)) return KEY_ESC;
        int c3 = raw_getbyte();
        switch (c3) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '1':
            if (stdin_ready(50000)) {
                int c4 = raw_getbyte();
                if (c4 == ';' && stdin_ready(50000)) {
                    /* ESC[1;5X — ctrl+arrow combos */
                    raw_getbyte();   /* modifier digit */
                    if (stdin_ready(50000)) {
                        int c5 = raw_getbyte();
                        if (c5 == 'H') return KEY_CTRL_HOME;
                        if (c5 == 'F') return KEY_CTRL_END;
                    }
                    return KEY_ESC;
                }
                /* ESC[1~ = Home */
                if (c4 == '~') return KEY_HOME;
            }
            return KEY_HOME;
        case '2': if (stdin_ready(50000)) raw_getbyte(); return KEY_INS;
        case '3': if (stdin_ready(50000)) raw_getbyte(); return KEY_DEL;
        case '4': if (stdin_ready(50000)) raw_getbyte(); return KEY_END;
        case '5': if (stdin_ready(50000)) raw_getbyte(); return KEY_PGUP;
        case '6': if (stdin_ready(50000)) raw_getbyte(); return KEY_PGDN;
        case '7': if (stdin_ready(50000)) raw_getbyte(); return KEY_HOME;
        case '8': if (stdin_ready(50000)) raw_getbyte(); return KEY_END;
        }
    } else if (c2 == 'O') {
        if (!stdin_ready(50000)) return KEY_ESC;
        int c3 = raw_getbyte();
        switch (c3) {
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'P': return KEY_F1;
        case 'Q': return KEY_F2;
        case 'R': return KEY_F3;
        case 'S': return KEY_F4;
        }
    }
    return KEY_ESC;
#endif
}

/* -----------------------------------------------------------------------
 * getscreensize — query terminal dimensions via TIOCGWINSZ
 * ----------------------------------------------------------------------- */

static void hb_getscreensize(bios_t *b, int *rows, int *cols)
{
    (void)b;
    *rows = 24; *cols = 80;   /* safe defaults */
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 4)  *rows = ws.ws_row;
        if (ws.ws_col > 20) *cols = ws.ws_col;
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        *cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        *rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    }
#endif
}

static void hb_cls(bios_t *b)
{
    (void)b;
    printf("\033[2J\033[H");
    fflush(stdout);
}

/* -----------------------------------------------------------------------
 * Disk I/O
 * ----------------------------------------------------------------------- */

static int hb_disk_read(bios_t *b, uint8_t unit,
                         uint8_t *buf, uint16_t count, uint32_t sector,
                         uint16_t *sectors_done)
{
    host_bios_t *hb  = (host_bios_t*)b;
    if (unit >= hb->num_drives || !hb->drives[unit]) return -1;
    FILE    *f    = hb->drives[unit];
    uint16_t ssz  = hb->secsiz[unit];
    long     off  = (long)sector * ssz;
    if (fseek(f, off, SEEK_SET) != 0) return -1;
    uint16_t done = 0;
    while (done < count) {
        if (fread(buf + done * ssz, ssz, 1, f) != 1) break;
        done++;
    }
    if (sectors_done) *sectors_done = done;
    return (done == count) ? 0 : -1;
}

static int hb_disk_write(bios_t *b, uint8_t unit, bool verify,
                          const uint8_t *buf, uint16_t count, uint32_t sector,
                          uint16_t *sectors_done)
{
    (void)verify;
    host_bios_t *hb  = (host_bios_t*)b;
    if (unit >= hb->num_drives || !hb->drives[unit]) return -1;
    FILE    *f    = hb->drives[unit];
    uint16_t ssz  = hb->secsiz[unit];
    long     off  = (long)sector * ssz;
    if (fseek(f, off, SEEK_SET) != 0) return -1;
    uint16_t done = 0;
    while (done < count) {
        if (fwrite(buf + done * ssz, ssz, 1, f) != 1) break;
        done++;
    }
    fflush(f);
    if (sectors_done) *sectors_done = done;
    return (done == count) ? 0 : -1;
}

static int hb_disk_change(bios_t *b, uint8_t unit)
{
    (void)b; (void)unit;
    return 0;   /* image file is never swapped — disk has not changed */
}

/* -----------------------------------------------------------------------
 * Date / Time
 * ----------------------------------------------------------------------- */

static void hb_gettime(bios_t *b, uint16_t *days_out,
                        uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *c)
{
    (void)b;
    time_t    t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    /* Days since Jan 1 1980 */
    struct tm epoch = {0};
    epoch.tm_year = 80; epoch.tm_mon = 0; epoch.tm_mday = 1;
    time_t epoch_t = mktime(&epoch);
    *days_out = (uint16_t)((t - epoch_t) / 86400);
    *h = (uint8_t)tm.tm_hour;
    *m = (uint8_t)tm.tm_min;
    *s = (uint8_t)tm.tm_sec;
    *c = 0;
}

static void hb_settime(bios_t *b, uint8_t h, uint8_t m, uint8_t s, uint8_t c)
{
    (void)b; (void)h; (void)m; (void)s; (void)c;
    /* Cannot set host system time without root; silently ignore */
}

static void hb_setdate(bios_t *b, uint16_t days)
{
    (void)b; (void)days;
}

/* -----------------------------------------------------------------------
 * Drive configuration
 * ----------------------------------------------------------------------- */

/* BPB (BIOS Parameter Block) layout at byte 11 of a FAT12 boot sector */
#pragma pack(push,1)
typedef struct {
    uint16_t bytes_per_sec;
    uint8_t  secs_per_clus;
    uint16_t reserved_secs;
    uint8_t  fat_copies;
    uint16_t root_entries;
    uint16_t total_secs;
    uint8_t  media;
    uint16_t secs_per_fat;
    uint16_t secs_per_track;
    uint16_t num_heads;
    uint16_t hidden_secs;
} bpb_t;
#pragma pack(pop)

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
} drive_cfg_t;   /* matches bios_dpt_t in kernel.c */

static int hb_get_drive_config(bios_t *b, void *cfg_out, int max_drives)
{
    host_bios_t  *hb  = (host_bios_t*)b;
    drive_cfg_t  *out = (drive_cfg_t*)cfg_out;
    int           n   = 0;

    for (int i = 0; i < hb->num_drives && i < max_drives; i++) {
        FILE *f = hb->drives[i];
        if (!f) continue;

        /* Read boot sector */
        uint8_t boot[512];
        fseek(f, 0, SEEK_SET);
        if (fread(boot, 512, 1, f) != 1) continue;

        bpb_t bpb;
        memcpy(&bpb, boot + 11, sizeof(bpb));

        out[n].drvnum        = (uint8_t)i;
        out[n].secsiz        = bpb.bytes_per_sec;
        out[n].secs_per_clus = bpb.secs_per_clus;
        out[n].reserved_secs = bpb.reserved_secs;
        out[n].fat_copies    = bpb.fat_copies;
        out[n].root_entries  = bpb.root_entries;
        out[n].total_sectors = bpb.total_secs;
        out[n].media_byte    = bpb.media;
        out[n].fat_size      = (uint8_t)bpb.secs_per_fat;

        hb->secsiz[n] = bpb.bytes_per_sec;
        n++;
    }
    return n;
}

static uint8_t hb_mapdev(bios_t *b, uint8_t unit, uint8_t fat_byte)
{
    (void)b; (void)fat_byte;
    return unit;
}

/* -----------------------------------------------------------------------
 * host_bios_init / shutdown
 * ----------------------------------------------------------------------- */

int host_bios_init(host_bios_t *hb, const char **img_paths, int n_drives)
{
    memset(hb, 0, sizeof(*hb));
    bios_t *b = &hb->base;

    b->stat             = hb_stat;
    b->in               = hb_in;
    b->out              = hb_out;
    b->print            = hb_print;
    b->auxin            = hb_auxin;
    b->auxout           = hb_auxout;
    b->flush            = hb_flush;
    b->cls              = hb_cls;
    b->disk_read        = hb_disk_read;
    b->disk_write       = hb_disk_write;
    b->disk_change      = hb_disk_change;
    b->gettime          = hb_gettime;
    b->settime          = hb_settime;
    b->setdate          = hb_setdate;
    b->mapdev           = hb_mapdev;
    b->get_drive_config = hb_get_drive_config;
    b->getkey           = hb_getkey;
    b->getscreensize    = hb_getscreensize;

    if (n_drives > 16) n_drives = 16;
    hb->num_drives = (uint8_t)n_drives;

    for (int i = 0; i < n_drives; i++) {
        hb->drives[i] = fopen(img_paths[i], "r+b");
        if (!hb->drives[i]) {
            fprintf(stderr, "Cannot open disk image '%s': %s\n",
                    img_paths[i], strerror(errno));
            return -1;
        }
        hb->secsiz[i] = 512;   /* default; overwritten by get_drive_config */
    }

#ifndef _WIN32
    enter_raw_mode();
#endif
    return 0;
}

void host_bios_shutdown(host_bios_t *hb)
{
#ifndef _WIN32
    leave_raw_mode();
#endif
    for (int i = 0; i < hb->num_drives; i++) {
        if (hb->drives[i]) { fclose(hb->drives[i]); hb->drives[i] = NULL; }
    }
}

/* -----------------------------------------------------------------------
 * host_bios_format — create a FAT12 disk image
 * ----------------------------------------------------------------------- */

int host_bios_format(const char *path,
                     uint8_t secs_per_track, uint8_t heads,
                     uint16_t total_sectors, uint16_t secsiz,
                     uint8_t secs_per_clus)
{
    FILE *f = fopen(path, "w+b");
    if (!f) { perror("format: cannot create"); return -1; }

    /* Choose FAT parameters */
    uint8_t  fat_copies    = 2;
    uint16_t root_entries  = 112;   /* standard for 720 KB */
    uint16_t reserved_secs = 1;

    uint16_t dir_secs   = (root_entries * DIRENT_SIZE + secsiz - 1) / secsiz;
    /* Iterate to find FAT size */
    uint8_t  fat_size = 3;
    for (int iter = 0; iter < 10; iter++) {
        uint16_t data_start = (uint16_t)(reserved_secs + fat_copies * fat_size + dir_secs);
        uint16_t data_secs  = (uint16_t)(total_sectors - data_start);
        uint16_t clusters   = (uint16_t)(data_secs / secs_per_clus);
        uint16_t fat_bytes  = (uint16_t)((clusters + 2) * 3 / 2 + 1);
        uint8_t  new_fatsz  = (uint8_t)((fat_bytes + secsiz - 1) / secsiz);
        if (new_fatsz == fat_size) break;
        fat_size = new_fatsz;
    }

    /* Write blank image */
    uint8_t zero[512] = {0};
    for (uint32_t s = 0; s < total_sectors; s++) {
        if (secsiz != 512) {
            uint8_t *buf = calloc(secsiz, 1);
            fwrite(buf, secsiz, 1, f);
            free(buf);
        } else {
            fwrite(zero, 512, 1, f);
        }
    }

    /* Build boot sector */
    uint8_t boot[512];
    memset(boot, 0, sizeof(boot));
    boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;  /* JMP + NOP */
    memcpy(boot + 3, "MSDOS125", 8);

    bpb_t bpb = {0};
    bpb.bytes_per_sec  = secsiz;
    bpb.secs_per_clus  = secs_per_clus;
    bpb.reserved_secs  = reserved_secs;
    bpb.fat_copies     = fat_copies;
    bpb.root_entries   = root_entries;
    bpb.total_secs     = total_sectors;
    bpb.media          = (total_sectors == 2880) ? 0xF0 :
                         (total_sectors == 1440) ? 0xF9 : 0xFD;
    bpb.secs_per_fat   = fat_size;
    bpb.secs_per_track = secs_per_track;
    bpb.num_heads      = heads;
    memcpy(boot + 11, &bpb, sizeof(bpb));
    boot[510] = 0x55; boot[511] = 0xAA;

    fseek(f, 0, SEEK_SET);
    fwrite(boot, 512, 1, f);

    /* Write FAT copies (media byte in first 3 bytes each) */
    uint8_t fat_buf[4096] = {0};
    fat_buf[0] = bpb.media;
    fat_buf[1] = 0xFF;
    fat_buf[2] = 0xFF;
    for (int copy = 0; copy < fat_copies; copy++) {
        long fat_off = (long)(reserved_secs + copy * fat_size) * secsiz;
        fseek(f, fat_off, SEEK_SET);
        for (int s = 0; s < fat_size; s++)
            fwrite(fat_buf + (s == 0 ? 0 : 3), secsiz, 1, f);
        memset(fat_buf, 0, 3);
    }

    fflush(f);
    fclose(f);
    fprintf(stderr, "Formatted %s: %u sectors, FAT size %u\n",
            path, total_sectors, fat_size);
    return 0;
}
