#pragma once
#include "dos.h"

/* Character device system calls (fn 1-12) and helpers. */

void    chardev_out(dos_t *dos, uint8_t c);     /* OUT — write char + track column */
int     chardev_in(dos_t *dos);                 /* IN  — read char with Ctrl-C check */
void    chardev_crlf(dos_t *dos);               /* emit CR LF */

/* System calls */
uint8_t dos_conin(dos_t *dos);                  /* fn 1: read + echo */
void    dos_conout(dos_t *dos, uint8_t c);      /* fn 2: write console char */
uint8_t dos_reader(dos_t *dos);                 /* fn 3: aux input */
void    dos_punch(dos_t *dos, uint8_t c);       /* fn 4: aux output */
void    dos_list(dos_t *dos, uint8_t c);        /* fn 5: printer output */
uint8_t dos_rawio(dos_t *dos, uint8_t dl);      /* fn 6: raw console I/O */
uint8_t dos_rawinp(dos_t *dos);                 /* fn 7: raw input (no echo, no Ctrl-C) */
uint8_t dos_in(dos_t *dos);                     /* fn 8: input no echo, Ctrl-C check */
void    dos_prtbuf(dos_t *dos, const char *s);  /* fn 9: print $-terminated string */
void    dos_bufin(dos_t *dos, uint8_t *buf);    /* fn 10: buffered line input */
uint8_t dos_constat(dos_t *dos);                /* fn 11: console status */
void    dos_flushkb(dos_t *dos);                /* fn 12: flush + re-execute */
