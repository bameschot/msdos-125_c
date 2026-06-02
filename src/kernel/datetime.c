#include "datetime.h"

/* Days in each month (non-leap year; February adjusted dynamically) */
static const uint8_t month_days[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Days in a 4-year cycle (matching FOURYEARS / YRTAB logic from MSDOS.ASM) */
#define FOURYEARS (3*365 + 366)

static bool is_leap(uint16_t year_1980)
{
    /* year_1980 is years since 1980.  1980 is leap, so year%4==0 → leap. */
    return (year_1980 & 3) == 0;
}

/* Compute day-of-year count for a given date. */
static uint16_t days_from_epoch(uint16_t year_1980, uint8_t month, uint8_t day)
{
    uint16_t days = 0;

    /* Complete 4-year blocks */
    uint16_t blocks = year_1980 / 4;
    days += (uint16_t)(blocks * FOURYEARS);

    /* Remaining years within block */
    uint16_t rem_y = year_1980 % 4;
    for (uint16_t y = 0; y < rem_y; y++) {
        days += (uint16_t)(((y & 3) == 0) ? 366 : 365);
    }

    /* Months within current year */
    for (uint8_t m = 1; m < month; m++) {
        uint8_t d = month_days[m - 1];
        if (m == 2 && is_leap(year_1980)) d = 29;
        days += d;
    }

    days += day - 1;   /* days are 1-based */
    return days;
}

static void date_from_epoch(uint16_t daycnt, uint16_t *year_1980,
                            uint8_t *month, uint8_t *day)
{
    uint16_t y = 0;
    while (true) {
        uint16_t ydays = is_leap(y) ? 366 : 365;
        if (daycnt < ydays) break;
        daycnt -= ydays;
        y++;
    }
    *year_1980 = y;

    uint8_t m = 1;
    while (m <= 12) {
        uint8_t d = month_days[m - 1];
        if (m == 2 && is_leap(y)) d = 29;
        if (daycnt < d) break;
        daycnt -= d;
        m++;
    }
    *month = m;
    *day   = (uint8_t)(daycnt + 1);
}

/* -----------------------------------------------------------------------
 * dos_readtime — read clock; update date if day changed
 * ----------------------------------------------------------------------- */

void dos_readtime(dos_t *dos)
{
    bios_t  *b = dos->bios;
    uint16_t days;
    uint8_t  h, m, s, c;
    b->gettime(b, &days, &h, &m, &s, &c);

    if (days == dos->daycnt) return;
    if (days >= FOURYEARS * 30) return;   /* sanity: < 120 years */

    dos->daycnt = days;

    uint16_t yr; uint8_t mo, dy;
    date_from_epoch(days, &yr, &mo, &dy);
    dos->year    = yr;
    dos->month   = mo;
    dos->day     = dy;

    /* Day of week: epoch day 0 = Jan 1 1980 = Tuesday */
    dos->weekday = (uint8_t)((days + 2) % 7);
}

/* -----------------------------------------------------------------------
 * Pack / unpack
 * ----------------------------------------------------------------------- */

void dos_date_pack(dos_t *dos, uint16_t *fdate, uint16_t *ftime)
{
    dos_readtime(dos);

    bios_t  *b = dos->bios;
    uint16_t days;
    uint8_t  h, m, s, c;
    b->gettime(b, &days, &h, &m, &s, &c);

    *fdate = (uint16_t)(((dos->year & 0x7F) << 9) | ((dos->month & 0x0F) << 5) | (dos->day & 0x1F));
    *ftime = (uint16_t)(((h & 0x1F) << 11) | ((m & 0x3F) << 5) | ((s >> 1) & 0x1F));
}

void dos_date_unpack(uint16_t fdate, uint16_t *year, uint8_t *month, uint8_t *day)
{
    *year  = (fdate >> 9) & 0x7F;
    *month = (fdate >> 5) & 0x0F;
    *day   = fdate & 0x1F;
}

void dos_time_unpack(uint16_t ftime, uint8_t *h, uint8_t *m, uint8_t *s)
{
    *h = (ftime >> 11) & 0x1F;
    *m = (ftime >> 5)  & 0x3F;
    *s = (uint8_t)((ftime & 0x1F) << 1);
}

/* -----------------------------------------------------------------------
 * System calls
 * ----------------------------------------------------------------------- */

void dos_getdate(dos_t *dos, uint16_t *year, uint8_t *month,
                 uint8_t *day, uint8_t *dow)
{
    dos_readtime(dos);
    *year  = dos->year + 1980;
    *month = dos->month;
    *day   = dos->day;
    *dow   = dos->weekday;
}

uint8_t dos_setdate(dos_t *dos, uint16_t year, uint8_t month, uint8_t day)
{
    if (year < 1980 || year > 2099) return DOS_ERR;
    if (month < 1 || month > 12)    return DOS_ERR;
    if (day < 1)                    return DOS_ERR;

    uint16_t y1980 = year - 1980;
    uint8_t  dmax  = month_days[month - 1];
    if (month == 2 && is_leap(y1980)) dmax = 29;
    if (day > dmax) return DOS_ERR;

    dos->year    = y1980;
    dos->month   = month;
    dos->day     = day;
    dos->daycnt  = days_from_epoch(y1980, month, day);
    dos->weekday = (uint8_t)((dos->daycnt + 2) % 7);

    if (dos->bios->setdate)
        dos->bios->setdate(dos->bios, dos->daycnt);

    return DOS_OK;
}

void dos_gettime(dos_t *dos, uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *c)
{
    uint16_t days;
    dos->bios->gettime(dos->bios, &days, h, m, s, c);
}

uint8_t dos_settime(dos_t *dos, uint8_t h, uint8_t m, uint8_t s, uint8_t c)
{
    if (h >= 24 || m >= 60 || s >= 60 || c >= 100) return DOS_ERR;
    if (dos->bios->settime) dos->bios->settime(dos->bios, h, m, s, c);
    return DOS_OK;
}
