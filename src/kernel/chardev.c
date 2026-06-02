#include "chardev.h"
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Low-level character output (OUT) — updates cursor column.
 * ----------------------------------------------------------------------- */

void chardev_out(dos_t *dos, uint8_t c)
{
    bios_t *b = dos->bios;
    if (c >= 0x20 || c == 0x7F) {
        dos->carpos++;
    } else if (c == '\r') {
        dos->carpos = 0;
    } else if (c == '\b') {
        if (dos->carpos) dos->carpos--;
    } else if (c == '\t') {
        uint8_t spaces = (uint8_t)(8 - (dos->carpos & 7));
        dos->carpos += spaces;
        while (spaces--) b->out(b, ' ');
        if (dos->pflag) b->print(b, '\t');
        return;
    }
    b->out(b, c);
    if (dos->pflag) b->print(b, c);
}

void chardev_crlf(dos_t *dos)
{
    chardev_out(dos, '\r');
    chardev_out(dos, '\n');
}

/* -----------------------------------------------------------------------
 * Status check: handle Ctrl-S (pause), Ctrl-P (printer toggle),
 * Ctrl-C (abort).  Returns character if something was typed.
 * ----------------------------------------------------------------------- */

static int stat_check(dos_t *dos)
{
    bios_t *b = dos->bios;
    if (!b->stat(b)) return 0;
    int c = b->in(b);
    if (c == ('S' - '@')) {
        /* Ctrl-S: pause */
        b->in(b);
        return 0;
    }
    if (c == ('P' - '@')) {
        dos->pflag = !dos->pflag;
        return 0;
    }
    if (c == ('C' - '@')) {
        chardev_out(dos, '^'); chardev_out(dos, 'C');
        chardev_crlf(dos);
        /* In the real DOS this would execute INT 23H. Here we just abort. */
        return -1;   /* signal Ctrl-C */
    }
    return c;
}

int chardev_in(dos_t *dos)
{
    bios_t *b = dos->bios;
    /* Spin until a key is available, handling Ctrl-S/P/C inline. */
    while (true) {
        while (!b->stat(b)) {}     /* wait for key */
        int c = b->in(b);          /* read it */
        if (c == ('S' - '@')) {
            /* Ctrl-S: pause until another key */
            while (!b->stat(b)) {}
            b->in(b);
            continue;
        }
        if (c == ('P' - '@')) {
            dos->pflag ^= 1;
            continue;
        }
        if (c == ('C' - '@')) {
            chardev_out(dos, '^'); chardev_out(dos, 'C');
            chardev_crlf(dos);
            return -1;
        }
        return c;
    }
}

/* -----------------------------------------------------------------------
 * System calls
 * ----------------------------------------------------------------------- */

uint8_t dos_conin(dos_t *dos)
{
    int c = chardev_in(dos);
    if (c < 0) return 3;   /* Ctrl-C */
    chardev_out(dos, (uint8_t)c);
    return (uint8_t)c;
}

void dos_conout(dos_t *dos, uint8_t c) { chardev_out(dos, c); }

uint8_t dos_reader(dos_t *dos)
{
    stat_check(dos);
    return (uint8_t)dos->bios->auxin(dos->bios);
}

void dos_punch(dos_t *dos, uint8_t c)
{
    stat_check(dos);
    dos->bios->auxout(dos->bios, c);
}

void dos_list(dos_t *dos, uint8_t c)
{
    stat_check(dos);
    dos->bios->print(dos->bios, c);
}

uint8_t dos_rawio(dos_t *dos, uint8_t dl)
{
    bios_t *b = dos->bios;
    if (dl == 0xFF) {
        /* Input request: return 0 (with ZF set) if no char ready, else char. */
        if (!b->stat(b)) return 0;
        return (uint8_t)b->in(b);
    }
    b->out(b, dl);
    return dl;
}

uint8_t dos_rawinp(dos_t *dos)
{
    return (uint8_t)dos->bios->in(dos->bios);
}

uint8_t dos_in(dos_t *dos)
{
    int c = chardev_in(dos);
    return (c < 0) ? 3 : (uint8_t)c;
}

void dos_prtbuf(dos_t *dos, const char *s)
{
    while (*s && *s != '$')
        chardev_out(dos, (uint8_t)*s++);
}

/* -----------------------------------------------------------------------
 * BUFIN (fn 10) — buffered line input with minimal line editing.
 * buf[0] = max chars, buf[1] = actual count on return, buf[2..] = chars.
 * Supports backspace and Ctrl-C.
 * ----------------------------------------------------------------------- */

void dos_bufin(dos_t *dos, uint8_t *buf)
{
    uint8_t maxlen  = buf[0];
    uint8_t count   = 0;
    uint8_t *data   = buf + 2;
    bios_t  *b      = dos->bios;

    if (maxlen == 0) return;

    /* If a previous line is in the template (buf[1]>0), use it as default. */
    uint8_t template_len = buf[1];
    (void)template_len;   /* simplified: ignore template */

    dos->startpos = dos->carpos;

    for (;;) {
        int c = chardev_in(dos);
        if (c < 0) {
            /* Ctrl-C */
            chardev_out(dos, '^'); chardev_out(dos, 'C');
            chardev_crlf(dos);
            count = 0;
            break;
        }
        uint8_t ch = (uint8_t)c;

        if (ch == 0x1A) {
            /* Ctrl-Z / EOF: treat as empty line, signal EOF via count=0 */
            count = 0;
            break;
        }
        if (ch == '\r' || ch == '\n') {
            chardev_out(dos, '\r');
            b->out(b, '\n');
            break;
        }

        if ((ch == '\b' || ch == 0x7F) && count > 0) {
            count--;
            /* Erase previous char */
            b->out(b, '\b'); b->out(b, ' '); b->out(b, '\b');
            if (dos->carpos) dos->carpos--;
            continue;
        }

        if (ch == ('X' - '@') || ch == 0x1B) {
            /* Ctrl-X / ESC: cancel line */
            b->out(b, '\\');
            chardev_crlf(dos);
            /* Reprint position spaces */
            for (uint8_t i = 0; i < dos->startpos; i++) b->out(b, ' ');
            count = 0;
            continue;
        }

        if (count < maxlen - 1) {
            data[count++] = ch;
            /* Echo */
            if (ch >= 0x20) {
                chardev_out(dos, ch);
            } else {
                b->out(b, '^');
                b->out(b, (uint8_t)(ch + '@'));
            }
        }
    }

    buf[1] = count;
    if (count < maxlen) data[count] = '\r';
}

uint8_t dos_constat(dos_t *dos)
{
    return dos->bios->stat(dos->bios) ? 0xFF : 0x00;
}

void dos_flushkb(dos_t *dos)
{
    if (dos->bios->flush) dos->bios->flush(dos->bios);
}
