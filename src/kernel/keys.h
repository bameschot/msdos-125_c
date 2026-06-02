#pragma once

/* -----------------------------------------------------------------------
 * Extended key codes returned by bios_t.getkey().
 * Values 0x01–0xFF are plain ASCII characters.
 * Values 0x100+ are special (non-printable) keys.
 * ----------------------------------------------------------------------- */

#define KEY_BACKSPACE   0x08
#define KEY_TAB         0x09
#define KEY_ENTER       0x0D
#define KEY_ESC         0x1B

#define KEY_UP          0x101
#define KEY_DOWN        0x102
#define KEY_LEFT        0x103
#define KEY_RIGHT       0x104
#define KEY_HOME        0x105
#define KEY_END         0x106
#define KEY_PGUP        0x107
#define KEY_PGDN        0x108
#define KEY_DEL         0x109
#define KEY_INS         0x10A
#define KEY_F1          0x10B
#define KEY_F2          0x10C
#define KEY_F3          0x10D
#define KEY_F4          0x10E
#define KEY_F5          0x10F
#define KEY_F10         0x114

/* -----------------------------------------------------------------------
 * Editor control-key bindings.
 *
 * Ctrl+Q (0x11, XON) and Ctrl+S (0x13, XOFF) are the POSIX flow-control
 * characters.  Even in raw mode some terminal emulators intercept them
 * before the bytes reach the application.  The bindings below use only
 * characters that have no terminal or OS special meaning:
 *
 *   Ctrl+W  (0x17, ETB) — Save (Write)
 *   Ctrl+X  (0x18, CAN) — Quit (eXit)
 *   Ctrl+G  (0x07, BEL) — Goto line
 * ----------------------------------------------------------------------- */
#define KEY_CTRL_W      0x17   /* Save (Write) */
#define KEY_CTRL_X      0x18   /* Quit (eXit)  */
#define KEY_CTRL_G      0x07   /* Goto line    */
#define KEY_CTRL_HOME   0x115  /* Start of file */
#define KEY_CTRL_END    0x116  /* End of file  */

/* Old aliases kept for reference — do NOT use these as editor bindings */
#define KEY_CTRL_S      0x13   /* XOFF — terminal flow control */
#define KEY_CTRL_Q      0x11   /* XON  — terminal flow control */
