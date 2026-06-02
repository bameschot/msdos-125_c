#pragma once
#include "dos.h"

/* Date/time functions (system calls 42-45). */

/* Read hardware clock; update dos date/time fields if day changed. */
void dos_readtime(dos_t *dos);

/* Pack current date/time into FCB-format words. */
void dos_date_pack(dos_t *dos, uint16_t *fdate, uint16_t *ftime);

/* Unpack FAT date word → year (1980-based), month, day. */
void dos_date_unpack(uint16_t fdate, uint16_t *year, uint8_t *month, uint8_t *day);

/* Unpack FAT time word → h, m, s (seconds already multiplied by 2). */
void dos_time_unpack(uint16_t ftime, uint8_t *h, uint8_t *m, uint8_t *s);

/* System calls */
void    dos_getdate(dos_t *dos, uint16_t *year, uint8_t *month,
                    uint8_t *day, uint8_t *dow);
uint8_t dos_setdate(dos_t *dos, uint16_t year, uint8_t month, uint8_t day);
void    dos_gettime(dos_t *dos, uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *c);
uint8_t dos_settime(dos_t *dos, uint8_t h, uint8_t m, uint8_t s, uint8_t c);
