/* tests/test_basic.c — Unit tests for the BASIC interpreter (basic.c).
 *
 * Strategy: compile basic.c with stub implementations of the kernel
 * functions it calls (chardev_out/in, dos_makefcb, dos_open, dos_seqrd,
 * dos_close, dos_setdma).  The stub file-system serves a BASIC program
 * stored as a C string, and all console output is captured to a buffer.
 * basic_run() is the entry point under test.
 *
 * Build:
 *   cc -std=c11 -Wall -I. -o tests/test_basic \
 *      tests/test_basic.c src/command/basic.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* Pull in all kernel type headers (no implementations needed) */
#include "src/kernel/types.h"
#include "src/kernel/bios.h"
#include "src/kernel/dos.h"
#include "src/kernel/chardev.h"
#include "src/kernel/fcb.h"
#include "src/kernel/fileio.h"
#include "src/kernel/kernel.h"
#include "src/command/basic.h"

/* ------------------------------------------------------------------ */
/* Global test state                                                     */
/* ------------------------------------------------------------------ */

static dos_t g_dos;                 /* minimal dos_t; only carpos is used */

static char  g_out[8192];           /* captured output */
static int   g_outpos;

static char  g_input_buf[512];      /* queued console input */
static int   g_input_pos;

/* The BASIC program text served as a fake "disk file" */
static const char *g_prog;
static int         g_prog_pos;

/* ------------------------------------------------------------------ */
/* Stub: chardev_out — capture to g_out, track carpos                   */
/* ------------------------------------------------------------------ */

void chardev_out(dos_t *dos, uint8_t c)
{
    if (g_outpos < (int)sizeof(g_out) - 1)
        g_out[g_outpos++] = (char)c;
    g_out[g_outpos] = '\0';

    if (dos) {
        if (c == '\r' || c == '\n') dos->carpos = 0;
        else                        dos->carpos++;
    }
}

void chardev_crlf(dos_t *dos)
{
    chardev_out(dos, '\r');
    chardev_out(dos, '\n');
}

/* ------------------------------------------------------------------ */
/* Stub: chardev_in — return from g_input_buf; '\r' when exhausted      */
/* ------------------------------------------------------------------ */

int chardev_in(dos_t *dos)
{
    (void)dos;
    unsigned char c = (unsigned char)g_input_buf[g_input_pos];
    if (c) { g_input_pos++; return (int)c; }
    return '\r';   /* simulate Enter when no more queued input */
}

/* ------------------------------------------------------------------ */
/* Stub: dos_setdma — store the DMA pointer                             */
/* ------------------------------------------------------------------ */

void dos_setdma(dos_t *dos, uint8_t *addr)
{
    dos->dmaadd = addr;
}

/* ------------------------------------------------------------------ */
/* Stub: dos_makefcb — skip the filename, clear FCB                     */
/* ------------------------------------------------------------------ */

uint8_t dos_makefcb(dos_t *dos, const char **src, fcb_t *fcb, uint8_t flags)
{
    (void)dos; (void)flags;
    /* advance past the filename token */
    while (**src && **src != ' ' && **src != '\0') (*src)++;
    memset(fcb, 0, sizeof *fcb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stub: dos_open — report file size from g_prog                        */
/* ------------------------------------------------------------------ */

uint8_t dos_open(dos_t *dos, fcb_t *fcb)
{
    (void)dos;
    if (!g_prog) return DOS_ERR;
    fcb->filsiz = (uint32_t)strlen(g_prog);
    fcb->recsiz = 128;
    return DOS_OK;
}

/* ------------------------------------------------------------------ */
/* Stub: dos_seqrd — copy 128-byte chunks from g_prog into dmaadd       */
/* ------------------------------------------------------------------ */

uint8_t dos_seqrd(dos_t *dos, fcb_t *fcb)
{
    (void)fcb;
    size_t len = g_prog ? strlen(g_prog) : 0;
    if ((size_t)g_prog_pos >= len) return DOS_EOF;

    size_t remaining = len - (size_t)g_prog_pos;
    size_t n = remaining >= 128 ? 128 : remaining;

    memcpy(dos->dmaadd, g_prog + g_prog_pos, n);
    if (n < 128) memset((char *)dos->dmaadd + n, 0, 128 - n);
    g_prog_pos += (int)n;

    /* DOS_PARTIAL signals the last block */
    return ((size_t)g_prog_pos >= len) ? DOS_PARTIAL : DOS_OK;
}

/* ------------------------------------------------------------------ */
/* Stub: dos_close — no-op                                              */
/* ------------------------------------------------------------------ */

uint8_t dos_close(dos_t *dos, fcb_t *fcb)
{
    (void)dos; (void)fcb;
    return DOS_OK;
}

/* ------------------------------------------------------------------ */
/* Test harness                                                          */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("PASS  " msg "\n"); g_pass++; } \
        else      { printf("FAIL  " msg "  [line %d]\n", __LINE__); g_fail++; } \
    } while (0)

/* Run a BASIC program given as a C string; return captured output */
static const char *run_prog(const char *prog)
{
    g_prog      = prog;
    g_prog_pos  = 0;
    g_outpos    = 0;
    g_out[0]    = '\0';
    g_dos.carpos = 0;
    basic_run(&g_dos, "TEST.BAS");
    return g_out;
}

/* Like run_prog but also pre-loads console input (for INPUT statement) */
static const char *run_prog_input(const char *prog, const char *input)
{
    strncpy(g_input_buf, input ? input : "", sizeof(g_input_buf) - 1);
    g_input_buf[sizeof(g_input_buf) - 1] = '\0';
    g_input_pos = 0;
    return run_prog(prog);
}

/* ------------------------------------------------------------------ */
/* PRINT                                                                 */
/* ------------------------------------------------------------------ */

static void test_print(void)
{
    printf("\n--- PRINT ---\n");
    const char *o;

    o = run_prog("10 PRINT \"HELLO\"\n20 END\n");
    CHECK(strcmp(o, "HELLO\r\n") == 0, "PRINT string literal");

    o = run_prog("10 PRINT 42\n20 END\n");
    CHECK(strcmp(o, "42\r\n") == 0, "PRINT integer");

    o = run_prog("10 PRINT 3.14\n20 END\n");
    CHECK(strcmp(o, "3.14\r\n") == 0, "PRINT float");

    o = run_prog("10 PRINT -7\n20 END\n");
    CHECK(strcmp(o, "-7\r\n") == 0, "PRINT negative integer");

    /* Empty PRINT emits only CRLF */
    o = run_prog("10 PRINT\n20 END\n");
    CHECK(strcmp(o, "\r\n") == 0, "PRINT with no args emits CRLF");

    /* Semicolon suppresses CRLF between items */
    o = run_prog("10 PRINT \"AB\"; \"CD\"\n20 END\n");
    CHECK(strcmp(o, "ABCD\r\n") == 0, "PRINT items with semicolon concat");

    /* Trailing semicolon suppresses the final CRLF */
    o = run_prog("10 PRINT \"X\";\n20 PRINT \"Y\"\n30 END\n");
    CHECK(strcmp(o, "XY\r\n") == 0, "trailing semicolon suppresses CRLF");

    /* PRINT on two consecutive lines */
    o = run_prog("10 PRINT \"A\"\n20 PRINT \"B\"\n30 END\n");
    CHECK(strcmp(o, "A\r\nB\r\n") == 0, "two PRINT statements");

    /* ? as alias for PRINT */
    o = run_prog("10 ? \"HI\"\n20 END\n");
    CHECK(strcmp(o, "HI\r\n") == 0, "? is alias for PRINT");
}

/* ------------------------------------------------------------------ */
/* Arithmetic                                                            */
/* ------------------------------------------------------------------ */

static void test_arithmetic(void)
{
    printf("\n--- Arithmetic ---\n");
    const char *o;

    o = run_prog("10 PRINT 2+3\n20 END\n");
    CHECK(strcmp(o, "5\r\n") == 0, "addition");

    o = run_prog("10 PRINT 10-3\n20 END\n");
    CHECK(strcmp(o, "7\r\n") == 0, "subtraction");

    o = run_prog("10 PRINT 3*4\n20 END\n");
    CHECK(strcmp(o, "12\r\n") == 0, "multiplication");

    o = run_prog("10 PRINT 10/4\n20 END\n");
    CHECK(strcmp(o, "2.5\r\n") == 0, "division producing float");

    o = run_prog("10 PRINT 8/2\n20 END\n");
    CHECK(strcmp(o, "4\r\n") == 0, "division producing integer");

    o = run_prog("10 PRINT 2^10\n20 END\n");
    CHECK(strcmp(o, "1024\r\n") == 0, "exponentiation");

    /* Operator precedence: * before + */
    o = run_prog("10 PRINT 2+3*4\n20 END\n");
    CHECK(strcmp(o, "14\r\n") == 0, "precedence: * before +");

    /* Parentheses override precedence */
    o = run_prog("10 PRINT (2+3)*4\n20 END\n");
    CHECK(strcmp(o, "20\r\n") == 0, "parentheses override precedence");

    /* Unary minus */
    o = run_prog("10 PRINT -5\n20 END\n");
    CHECK(strcmp(o, "-5\r\n") == 0, "unary minus literal");

    o = run_prog("10 A=3\n20 PRINT -A\n30 END\n");
    CHECK(strcmp(o, "-3\r\n") == 0, "unary minus on variable");

    /* Division by zero triggers error */
    o = run_prog("10 PRINT 1/0\n20 END\n");
    CHECK(strstr(o, "DIVISION BY ZERO") != NULL, "division by zero error");
    CHECK(strstr(o, "IN 10") != NULL, "error reports line number");
}

/* ------------------------------------------------------------------ */
/* Comparison operators                                                  */
/* ------------------------------------------------------------------ */

static void test_comparison(void)
{
    printf("\n--- Comparison ---\n");
    const char *o;

    o = run_prog("10 PRINT 3=3\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "equal: true returns -1");

    o = run_prog("10 PRINT 3=4\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "equal: false returns 0");

    o = run_prog("10 PRINT 3<4\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "less-than true");

    o = run_prog("10 PRINT 4<3\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "less-than false");

    o = run_prog("10 PRINT 4>3\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "greater-than true");

    o = run_prog("10 PRINT 3<>4\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "not-equal true");

    o = run_prog("10 PRINT 3<>3\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "not-equal false");

    o = run_prog("10 PRINT 3<=3\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "less-or-equal: equal case");

    o = run_prog("10 PRINT 4<=3\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "less-or-equal: false case");

    o = run_prog("10 PRINT 3>=3\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "greater-or-equal: equal case");

    /* String comparison */
    o = run_prog("10 PRINT \"ABC\"=\"ABC\"\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "string equal: true");

    o = run_prog("10 PRINT \"ABC\"=\"DEF\"\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "string equal: false");

    o = run_prog("10 PRINT \"ABC\"<\"DEF\"\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "string less-than: true");
}

/* ------------------------------------------------------------------ */
/* Logical operators                                                     */
/* ------------------------------------------------------------------ */

static void test_logical(void)
{
    printf("\n--- Logical ---\n");
    const char *o;

    o = run_prog("10 PRINT -1 AND -1\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "AND: T AND T = T");

    o = run_prog("10 PRINT -1 AND 0\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "AND: T AND F = F");

    o = run_prog("10 PRINT 0 AND 0\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "AND: F AND F = F");

    o = run_prog("10 PRINT 0 OR -1\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "OR: F OR T = T");

    o = run_prog("10 PRINT 0 OR 0\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "OR: F OR F = F");

    o = run_prog("10 PRINT NOT 0\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "NOT: NOT F = T");

    o = run_prog("10 PRINT NOT -1\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0,  "NOT: NOT T = F");

    /* Compound: (1=1) AND (2=2) */
    o = run_prog("10 PRINT (1=1) AND (2=2)\n20 END\n");
    CHECK(strcmp(o, "-1\r\n") == 0, "compound AND of two comparisons");

    /* Short-circuit chain: (1=1) AND (1=2) */
    o = run_prog("10 PRINT (1=1) AND (1=2)\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0, "compound AND: T AND F = F");
}

/* ------------------------------------------------------------------ */
/* Variables                                                             */
/* ------------------------------------------------------------------ */

static void test_variables(void)
{
    printf("\n--- Variables ---\n");
    const char *o;

    /* Numeric variable, implicit LET */
    o = run_prog("10 A=42\n20 PRINT A\n30 END\n");
    CHECK(strcmp(o, "42\r\n") == 0, "numeric variable assign and read");

    /* Explicit LET */
    o = run_prog("10 LET B=7\n20 PRINT B\n30 END\n");
    CHECK(strcmp(o, "7\r\n") == 0, "LET keyword (explicit)");

    /* String variable */
    o = run_prog("10 A$=\"WORLD\"\n20 PRINT A$\n30 END\n");
    CHECK(strcmp(o, "WORLD\r\n") == 0, "string variable assign and read");

    /* Multiple numeric variables */
    o = run_prog("10 A=3\n20 B=4\n30 PRINT A*A+B*B\n40 END\n");
    CHECK(strcmp(o, "25\r\n") == 0, "two numeric variables in expression");

    /* String concatenation via + */
    o = run_prog("10 A$=\"FOO\"\n20 B$=\"BAR\"\n30 PRINT A$+B$\n40 END\n");
    CHECK(strcmp(o, "FOOBAR\r\n") == 0, "string variable concatenation");

    /* Uninitialized numeric variable defaults to 0 */
    o = run_prog("10 PRINT Z\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0, "uninitialized numeric var == 0");

    /* Uninitialized string variable defaults to empty */
    o = run_prog("10 PRINT Z$\n20 END\n");
    CHECK(strcmp(o, "\r\n") == 0, "uninitialized string var == empty");
}

/* ------------------------------------------------------------------ */
/* Built-in functions                                                    */
/* ------------------------------------------------------------------ */

static void test_functions(void)
{
    printf("\n--- Built-in functions ---\n");
    const char *o;

    /* INT: floor toward negative infinity */
    o = run_prog("10 PRINT INT(3.7)\n20 END\n");
    CHECK(strcmp(o, "3\r\n") == 0, "INT(3.7) = 3");

    o = run_prog("10 PRINT INT(-3.7)\n20 END\n");
    CHECK(strcmp(o, "-4\r\n") == 0, "INT(-3.7) = -4  (floor, not truncate)");

    o = run_prog("10 PRINT INT(5)\n20 END\n");
    CHECK(strcmp(o, "5\r\n") == 0, "INT of whole number unchanged");

    /* ABS */
    o = run_prog("10 PRINT ABS(-5)\n20 END\n");
    CHECK(strcmp(o, "5\r\n") == 0, "ABS(-5) = 5");

    o = run_prog("10 PRINT ABS(3)\n20 END\n");
    CHECK(strcmp(o, "3\r\n") == 0, "ABS(3) = 3");

    /* SQR */
    o = run_prog("10 PRINT SQR(9)\n20 END\n");
    CHECK(strcmp(o, "3\r\n") == 0, "SQR(9) = 3");

    o = run_prog("10 PRINT SQR(4)\n20 END\n");
    CHECK(strcmp(o, "2\r\n") == 0, "SQR(4) = 2");

    /* RND: result is in [0,1) so RND(1)>=0 is always true (-1) */
    o = run_prog("10 R=RND(1)\n20 IF R>=0 THEN PRINT \"OK\"\n30 END\n");
    CHECK(strcmp(o, "OK\r\n") == 0, "RND(1) is in [0,1)");

    /* LEN */
    o = run_prog("10 PRINT LEN(\"HELLO\")\n20 END\n");
    CHECK(strcmp(o, "5\r\n") == 0, "LEN(\"HELLO\") = 5");

    o = run_prog("10 PRINT LEN(\"\")\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0, "LEN(\"\") = 0");

    /* ASC */
    o = run_prog("10 PRINT ASC(\"A\")\n20 END\n");
    CHECK(strcmp(o, "65\r\n") == 0, "ASC(\"A\") = 65");

    o = run_prog("10 PRINT ASC(\"ABC\")\n20 END\n");
    CHECK(strcmp(o, "65\r\n") == 0, "ASC uses only first char");

    /* VAL */
    o = run_prog("10 PRINT VAL(\"42\")\n20 END\n");
    CHECK(strcmp(o, "42\r\n") == 0, "VAL(\"42\") = 42");

    o = run_prog("10 PRINT VAL(\"3.14\")\n20 END\n");
    CHECK(strcmp(o, "3.14\r\n") == 0, "VAL(\"3.14\") = 3.14");

    o = run_prog("10 PRINT VAL(\"0\")\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0, "VAL(\"0\") = 0");

    /* CHR$ */
    o = run_prog("10 PRINT CHR$(65)\n20 END\n");
    CHECK(strcmp(o, "A\r\n") == 0, "CHR$(65) = \"A\"");

    o = run_prog("10 PRINT CHR$(48)\n20 END\n");
    CHECK(strcmp(o, "0\r\n") == 0, "CHR$(48) = \"0\"");

    /* STR$ */
    o = run_prog("10 PRINT STR$(42)\n20 END\n");
    CHECK(strcmp(o, "42\r\n") == 0, "STR$(42) = \"42\"");

    o = run_prog("10 PRINT STR$(-7)\n20 END\n");
    CHECK(strcmp(o, "-7\r\n") == 0, "STR$(-7) = \"-7\"");

    /* LEFT$ */
    o = run_prog("10 PRINT LEFT$(\"HELLO\",3)\n20 END\n");
    CHECK(strcmp(o, "HEL\r\n") == 0, "LEFT$(\"HELLO\",3) = \"HEL\"");

    o = run_prog("10 PRINT LEFT$(\"HELLO\",0)\n20 END\n");
    CHECK(strcmp(o, "\r\n") == 0, "LEFT$(\"HELLO\",0) = \"\"");

    o = run_prog("10 PRINT LEFT$(\"HI\",99)\n20 END\n");
    CHECK(strcmp(o, "HI\r\n") == 0, "LEFT$ clamps to string length");

    /* RIGHT$ */
    o = run_prog("10 PRINT RIGHT$(\"HELLO\",3)\n20 END\n");
    CHECK(strcmp(o, "LLO\r\n") == 0, "RIGHT$(\"HELLO\",3) = \"LLO\"");

    o = run_prog("10 PRINT RIGHT$(\"HELLO\",0)\n20 END\n");
    CHECK(strcmp(o, "\r\n") == 0, "RIGHT$(\"HELLO\",0) = \"\"");

    /* MID$ with explicit length */
    o = run_prog("10 PRINT MID$(\"HELLO\",2,3)\n20 END\n");
    CHECK(strcmp(o, "ELL\r\n") == 0, "MID$(\"HELLO\",2,3) = \"ELL\"");

    /* MID$ from position 1 (whole string) */
    o = run_prog("10 PRINT MID$(\"HELLO\",1)\n20 END\n");
    CHECK(strcmp(o, "HELLO\r\n") == 0, "MID$(s,1) returns full string");

    /* MID$ past end returns empty */
    o = run_prog("10 PRINT MID$(\"HI\",10)\n20 END\n");
    CHECK(strcmp(o, "\r\n") == 0, "MID$ past end returns empty");

    /* Type mismatch: numeric function on string */
    o = run_prog("10 PRINT ABS(\"X\")\n20 END\n");
    CHECK(strstr(o, "TYPE MISMATCH") != NULL, "ABS on string: TYPE MISMATCH");

    /* Type mismatch: string function on number */
    o = run_prog("10 PRINT LEN(42)\n20 END\n");
    CHECK(strstr(o, "TYPE MISMATCH") != NULL, "LEN on number: TYPE MISMATCH");
}

/* ------------------------------------------------------------------ */
/* GOTO                                                                  */
/* ------------------------------------------------------------------ */

static void test_goto(void)
{
    printf("\n--- GOTO ---\n");
    const char *o;

    /* Skip line 20 */
    o = run_prog("10 GOTO 30\n20 PRINT \"SKIP\"\n30 PRINT \"HERE\"\n40 END\n");
    CHECK(strcmp(o, "HERE\r\n") == 0, "GOTO skips lines");

    /* Jump backwards */
    o = run_prog(
        "10 A=0\n"
        "20 A=A+1\n"
        "30 IF A>=3 THEN GOTO 50\n"
        "40 GOTO 20\n"
        "50 PRINT A\n"
        "60 END\n");
    CHECK(strcmp(o, "3\r\n") == 0, "GOTO backwards: loop to 3");

    /* Undefined line number */
    o = run_prog("10 GOTO 999\n20 END\n");
    CHECK(strstr(o, "UNDEFINED LINE 999") != NULL, "GOTO undefined line: error");
}

/* ------------------------------------------------------------------ */
/* GOSUB / RETURN                                                        */
/* ------------------------------------------------------------------ */

static void test_gosub(void)
{
    printf("\n--- GOSUB/RETURN ---\n");
    const char *o;

    /* Basic subroutine */
    o = run_prog(
        "10 GOSUB 100\n"
        "20 PRINT \"BACK\"\n"
        "30 END\n"
        "100 PRINT \"SUB\"\n"
        "110 RETURN\n");
    CHECK(strcmp(o, "SUB\r\nBACK\r\n") == 0, "basic GOSUB/RETURN");

    /* Subroutine called twice */
    o = run_prog(
        "10 GOSUB 100\n"
        "20 GOSUB 100\n"
        "30 END\n"
        "100 PRINT \"X\"\n"
        "110 RETURN\n");
    CHECK(strcmp(o, "X\r\nX\r\n") == 0, "GOSUB called twice");

    /* Nested GOSUB */
    o = run_prog(
        "10 GOSUB 100\n"
        "20 END\n"
        "100 GOSUB 200\n"
        "110 PRINT \"A\"\n"
        "120 RETURN\n"
        "200 PRINT \"B\"\n"
        "210 RETURN\n");
    CHECK(strcmp(o, "B\r\nA\r\n") == 0, "nested GOSUB");

    /* RETURN without GOSUB */
    o = run_prog("10 RETURN\n20 END\n");
    CHECK(strstr(o, "RETURN WITHOUT GOSUB") != NULL, "RETURN without GOSUB: error");
}

/* ------------------------------------------------------------------ */
/* FOR / NEXT                                                            */
/* ------------------------------------------------------------------ */

static void test_for_next(void)
{
    printf("\n--- FOR/NEXT ---\n");
    const char *o;

    /* Basic loop 1 to 3 */
    o = run_prog(
        "10 FOR I=1 TO 3\n"
        "20 PRINT I\n"
        "30 NEXT I\n"
        "40 END\n");
    CHECK(strcmp(o, "1\r\n2\r\n3\r\n") == 0, "FOR loop 1 to 3");

    /* Explicit STEP 2 */
    o = run_prog(
        "10 FOR I=0 TO 6 STEP 2\n"
        "20 PRINT I\n"
        "30 NEXT I\n"
        "40 END\n");
    CHECK(strcmp(o, "0\r\n2\r\n4\r\n6\r\n") == 0, "FOR loop with STEP 2");

    /* Negative STEP (count down) */
    o = run_prog(
        "10 FOR I=3 TO 1 STEP -1\n"
        "20 PRINT I\n"
        "30 NEXT I\n"
        "40 END\n");
    CHECK(strcmp(o, "3\r\n2\r\n1\r\n") == 0, "FOR loop with negative STEP");

    /* start > limit with positive step: body still executes once (classic BASIC) */
    o = run_prog(
        "10 FOR I=5 TO 1\n"
        "20 PRINT I\n"
        "30 NEXT I\n"
        "40 PRINT \"DONE\"\n"
        "50 END\n");
    CHECK(strstr(o, "DONE") != NULL, "FOR with start>limit: exits after body runs once");

    /* Nested FOR loops */
    o = run_prog(
        "10 FOR I=1 TO 2\n"
        "20 FOR J=1 TO 2\n"
        "30 PRINT I*10+J\n"
        "40 NEXT J\n"
        "50 NEXT I\n"
        "60 END\n");
    CHECK(strcmp(o, "11\r\n12\r\n21\r\n22\r\n") == 0, "nested FOR loops");

    /* Accumulator via loop */
    o = run_prog(
        "10 S=0\n"
        "20 FOR I=1 TO 5\n"
        "30 S=S+I\n"
        "40 NEXT I\n"
        "50 PRINT S\n"
        "60 END\n");
    CHECK(strcmp(o, "15\r\n") == 0, "FOR accumulates sum 1..5 = 15");

    /* NEXT without FOR */
    o = run_prog("10 NEXT I\n20 END\n");
    CHECK(strstr(o, "NEXT WITHOUT FOR") != NULL, "NEXT without FOR: error");
}

/* ------------------------------------------------------------------ */
/* IF / THEN                                                             */
/* ------------------------------------------------------------------ */

static void test_if_then(void)
{
    printf("\n--- IF/THEN ---\n");
    const char *o;

    /* THEN with inline statement (true) */
    o = run_prog("10 IF 1=1 THEN PRINT \"YES\"\n20 END\n");
    CHECK(strcmp(o, "YES\r\n") == 0, "IF true: inline THEN executes");

    /* THEN with inline statement (false) */
    o = run_prog("10 IF 1=2 THEN PRINT \"NO\"\n20 PRINT \"OK\"\n30 END\n");
    CHECK(strcmp(o, "OK\r\n") == 0, "IF false: inline THEN skipped");

    /* THEN with line number (true) */
    o = run_prog(
        "10 IF 1=1 THEN 30\n"
        "20 PRINT \"SKIP\"\n"
        "30 PRINT \"HERE\"\n"
        "40 END\n");
    CHECK(strcmp(o, "HERE\r\n") == 0, "IF true: THEN lineno jumps");

    /* THEN with line number (false — falls through) */
    o = run_prog(
        "10 IF 1=2 THEN 40\n"
        "20 PRINT \"NOSKIP\"\n"
        "30 END\n"
        "40 PRINT \"JUMPED\"\n"
        "50 END\n");
    CHECK(strcmp(o, "NOSKIP\r\n") == 0, "IF false: THEN lineno not taken");

    /* IF using variable */
    o = run_prog(
        "10 X=5\n"
        "20 IF X>3 THEN PRINT \"BIG\"\n"
        "30 END\n");
    CHECK(strcmp(o, "BIG\r\n") == 0, "IF with variable comparison");

    /* Chained IF: only the true branch runs */
    o = run_prog(
        "10 A=2\n"
        "20 IF A=1 THEN PRINT \"ONE\"\n"
        "30 IF A=2 THEN PRINT \"TWO\"\n"
        "40 IF A=3 THEN PRINT \"THREE\"\n"
        "50 END\n");
    CHECK(strcmp(o, "TWO\r\n") == 0, "chained IF: only matching branch fires");
}

/* ------------------------------------------------------------------ */
/* REM / multi-statement / END / STOP                                   */
/* ------------------------------------------------------------------ */

static void test_misc(void)
{
    printf("\n--- REM / multi-stmt / END / STOP ---\n");
    const char *o;

    /* REM skips rest of line */
    o = run_prog("10 REM THIS IS A COMMENT\n20 PRINT \"OK\"\n30 END\n");
    CHECK(strcmp(o, "OK\r\n") == 0, "REM skips to next line");

    /* ' as comment character */
    o = run_prog("10 PRINT \"A\" ' this is inline comment\n20 END\n");
    /* After PRINT "A", the ' starts a comment — CRLF is already emitted */
    CHECK(strncmp(o, "A\r\n", 3) == 0, "' starts inline comment");

    /* Multi-statement with colon */
    o = run_prog("10 A=1 : B=2 : PRINT A+B\n20 END\n");
    CHECK(strcmp(o, "3\r\n") == 0, "colon-separated statements on one line");

    /* STOP halts before END */
    o = run_prog("10 PRINT \"A\"\n20 STOP\n30 PRINT \"B\"\n40 END\n");
    CHECK(strcmp(o, "A\r\n") == 0, "STOP halts execution");

    /* END at the last line */
    o = run_prog("10 PRINT \"X\"\n20 END\n");
    CHECK(strcmp(o, "X\r\n") == 0, "END terminates program");

    /* Program with lines out of order — should be sorted by line number */
    o = run_prog("30 END\n10 PRINT \"FIRST\"\n20 PRINT \"SECOND\"\n");
    CHECK(strcmp(o, "FIRST\r\nSECOND\r\n") == 0, "lines sorted by number");
}

/* ------------------------------------------------------------------ */
/* INPUT statement                                                       */
/* ------------------------------------------------------------------ */

static void test_input(void)
{
    printf("\n--- INPUT ---\n");
    const char *o;

    /* Numeric INPUT: user types "42", program prints it back */
    o = run_prog_input(
        "10 INPUT A\n"
        "20 PRINT A\n"
        "30 END\n",
        "42");
    CHECK(strstr(o, "42") != NULL, "INPUT numeric: value read correctly");

    /* String INPUT */
    o = run_prog_input(
        "10 INPUT A$\n"
        "20 PRINT A$\n"
        "30 END\n",
        "HELLO");
    CHECK(strstr(o, "HELLO") != NULL, "INPUT string: value read correctly");

    /* Prompt string */
    o = run_prog_input(
        "10 INPUT \"Enter:\"; A\n"
        "20 PRINT A\n"
        "30 END\n",
        "7");
    CHECK(strstr(o, "Enter:") != NULL, "INPUT with prompt string printed");
    CHECK(strstr(o, "7") != NULL, "INPUT with prompt: value stored");
}

/* ------------------------------------------------------------------ */
/* Integration: realistic small programs                                 */
/* ------------------------------------------------------------------ */

static void test_programs(void)
{
    printf("\n--- Integration programs ---\n");
    const char *o;

    /* Fibonacci: F(1)=1, F(2)=1, F(3)=2, F(4)=3, F(5)=5 */
    o = run_prog(
        "10 A=1\n"
        "20 B=1\n"
        "30 FOR I=1 TO 5\n"
        "40 PRINT A\n"
        "50 C=A+B\n"
        "60 A=B\n"
        "70 B=C\n"
        "80 NEXT I\n"
        "90 END\n");
    CHECK(strcmp(o, "1\r\n1\r\n2\r\n3\r\n5\r\n") == 0, "Fibonacci first 5 terms");

    /* Factorial via GOSUB */
    o = run_prog(
        "10 N=5\n"
        "20 F=1\n"
        "30 GOSUB 100\n"
        "40 PRINT F\n"
        "50 END\n"
        "100 FOR I=2 TO N\n"
        "110 F=F*I\n"
        "120 NEXT I\n"
        "130 RETURN\n");
    CHECK(strcmp(o, "120\r\n") == 0, "5! = 120 via GOSUB");

    /* String processing: build a string in a loop */
    o = run_prog(
        "10 S$=\"\"\n"
        "20 FOR I=1 TO 5\n"
        "30 S$=S$+STR$(I)\n"
        "40 NEXT I\n"
        "50 PRINT S$\n"
        "60 END\n");
    CHECK(strcmp(o, "12345\r\n") == 0, "string built by concatenating STR$(I) in loop");

    /* Countdown using GOTO */
    o = run_prog(
        "10 C=3\n"
        "20 IF C=0 THEN GOTO 60\n"
        "30 PRINT C\n"
        "40 C=C-1\n"
        "50 GOTO 20\n"
        "60 PRINT \"GO\"\n"
        "70 END\n");
    CHECK(strcmp(o, "3\r\n2\r\n1\r\nGO\r\n") == 0, "countdown from 3 via GOTO");

    /* Compute 10 * 10 = 100 via repeated addition */
    o = run_prog(
        "10 S=0\n"
        "20 FOR I=1 TO 10\n"
        "30 S=S+10\n"
        "40 NEXT I\n"
        "50 PRINT S\n"
        "60 END\n");
    CHECK(strcmp(o, "100\r\n") == 0, "10*10 via repeated addition in loop");
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== BASIC interpreter unit tests ===\n");

    memset(&g_dos, 0, sizeof g_dos);

    test_print();
    test_arithmetic();
    test_comparison();
    test_logical();
    test_variables();
    test_functions();
    test_goto();
    test_gosub();
    test_for_next();
    test_if_then();
    test_misc();
    test_input();
    test_programs();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
