/*
 * basic.c — rudimentary line-numbered BASIC interpreter for the MS-DOS 1.25
 * C port.
 *
 * Supported statements:
 *   REM  PRINT  LET  INPUT  IF…THEN  GOTO  GOSUB  RETURN  FOR…TO…STEP
 *   NEXT  END  STOP
 * Multi-statement lines separated by ':'
 *
 * Variables: A–Z (numeric double), A$–Z$ (string ≤255 chars)
 * Operators:  + - * / ^  (numeric);  +  (string concat)
 *             = <> < > <= >=   (return -1 true / 0 false)
 *             AND  OR  NOT
 * Functions:  INT ABS SQR RND LEN ASC VAL CHR$ STR$ LEFT$ RIGHT$ MID$
 */

#include "basic.h"
#include "../kernel/chardev.h"
#include "../kernel/fcb.h"
#include "../kernel/fileio.h"
#include "../kernel/kernel.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define MAX_LINES   1000
#define STR_MAX     255
#define FOR_DEPTH   16
#define GOSUB_DEPTH 32

/* -----------------------------------------------------------------------
 * Data types
 * ----------------------------------------------------------------------- */

typedef enum { VT_NUM, VT_STR } vtype_t;

typedef struct {
    vtype_t type;
    double  num;
    char    str[STR_MAX + 1];
} val_t;

typedef struct {
    uint16_t linenum;
    char    *text;   /* statement text (no leading line number) */
} bas_line_t;

typedef struct {
    int    var;      /* 0–25 = A–Z */
    double limit;
    double step;
    int    ret_pc;   /* line index to jump back to (the FOR line) */
} for_frame_t;

typedef struct {
    bas_line_t  lines[MAX_LINES];
    int         nlines;
    int         pc;            /* current line index */
    const char *stmt;          /* pointer within current line text */
    double      numvars[26];
    char        strvars[26][STR_MAX + 1];
    for_frame_t forstack[FOR_DEPTH];
    int         fsp;
    int         gosubstack[GOSUB_DEPTH];
    int         gsp;
    bool        running;
    bool        error;
    dos_t      *dos;
} bas_t;

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

static val_t  eval_expr  (bas_t *b, const char **p);
static void   exec_line  (bas_t *b, const char *text);
static void   bas_error  (bas_t *b, const char *msg);

/* -----------------------------------------------------------------------
 * Output helpers
 * ----------------------------------------------------------------------- */

static void bprint(bas_t *b, const char *s)
{
    while (*s) chardev_out(b->dos, (uint8_t)*s++);
}

static void bprintc(bas_t *b, char c)
{
    chardev_out(b->dos, (uint8_t)c);
}

static void bprintcrlf(bas_t *b)
{
    chardev_out(b->dos, '\r');
    chardev_out(b->dos, '\n');
}

static void bprint_double(bas_t *b, double v)
{
    char tmp[64];
    /* Use integer format when the value is a whole number */
    if (v == (long long)v && v >= -1e15 && v <= 1e15)
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    else
        snprintf(tmp, sizeof(tmp), "%g", v);
    bprint(b, tmp);
}

/* -----------------------------------------------------------------------
 * Console input (for INPUT statement)
 * ----------------------------------------------------------------------- */

static void bread_line(bas_t *b, char *buf, int maxlen)
{
    int pos = 0;
    for (;;) {
        int c = chardev_in(b->dos);
        if (c < 0) break;                /* Ctrl-C */
        if (c == '\r' || c == '\n') {
            bprintcrlf(b);
            break;
        }
        if ((c == '\b' || c == 0x7F) && pos > 0) {
            pos--;
            chardev_out(b->dos, '\b');
            chardev_out(b->dos, ' ');
            chardev_out(b->dos, '\b');
            continue;
        }
        if (c >= 0x20 && pos < maxlen - 1) {
            buf[pos++] = (char)c;
            chardev_out(b->dos, (uint8_t)c);
        }
    }
    buf[pos] = '\0';
}

/* -----------------------------------------------------------------------
 * Error
 * ----------------------------------------------------------------------- */

static void bas_error(bas_t *b, const char *msg)
{
    if (b->error) return;
    b->error   = true;
    b->running = false;
    bprint(b, "?");
    bprint(b, msg);
    if (b->pc >= 0 && b->pc < b->nlines) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), " IN %u", (unsigned)b->lines[b->pc].linenum);
        bprint(b, tmp);
    }
    bprintcrlf(b);
}

/* -----------------------------------------------------------------------
 * Skip whitespace
 * ----------------------------------------------------------------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* -----------------------------------------------------------------------
 * Keyword matching (case-insensitive, not part of a longer identifier)
 * ----------------------------------------------------------------------- */

static int match_kw(const char **p, const char *kw)
{
    const char *s = skip_ws(*p);
    size_t len = strlen(kw);
    if (strncasecmp(s, kw, len) == 0) {
        char after = s[len];
        /* keyword must not be followed by alphanumeric or $ */
        if (!isalnum((unsigned char)after) && after != '$') {
            *p = s + len;
            return 1;
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Read an uppercase identifier (keyword/variable name) into buf
 * Returns length; buf is NUL-terminated and uppercased.
 * Also consumes a trailing '$' if present.
 * ----------------------------------------------------------------------- */

static int read_ident(const char *p, char *buf, int bufsz)
{
    int n = 0;
    while (isalpha((unsigned char)*p) && n < bufsz - 2) {
        buf[n++] = (char)toupper((unsigned char)*p++);
    }
    if (n == 0) { buf[0] = '\0'; return 0; }
    if (*p == '$' && n < bufsz - 1) {
        buf[n++] = '$';
        p++;
    }
    buf[n] = '\0';
    return n;
}

/* -----------------------------------------------------------------------
 * Line lookup — find line index by line number
 * ----------------------------------------------------------------------- */

static int find_line(bas_t *b, uint16_t num)
{
    for (int i = 0; i < b->nlines; i++)
        if (b->lines[i].linenum == num) return i;
    return -1;
}

/* -----------------------------------------------------------------------
 * Expression evaluator
 *
 * All functions return val_t by value.  On error they call bas_error and
 * return a zero/empty value; callers check b->error after every eval.
 * ----------------------------------------------------------------------- */

static val_t make_num(double n) { val_t v; v.type = VT_NUM; v.num = n; v.str[0] = '\0'; return v; }
static val_t make_str(const char *s)
{
    val_t v; v.type = VT_STR; v.num = 0;
    strncpy(v.str, s ? s : "", STR_MAX); v.str[STR_MAX] = '\0';
    return v;
}

/* forward */
static val_t eval_expr(bas_t *b, const char **p);

/* --- primary: NUMBER | STRING | VARIABLE | FUNC(...) | '(' expr ')' --- */
static val_t eval_primary(bas_t *b, const char **pp)
{
    const char *p = skip_ws(*pp);

    /* Parenthesised subexpression */
    if (*p == '(') {
        p++;
        val_t v = eval_expr(b, &p);
        p = skip_ws(p);
        if (*p == ')') p++;
        *pp = p;
        return v;
    }

    /* Numeric literal */
    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        char *end;
        double n = strtod(p, &end);
        *pp = end;
        return make_num(n);
    }

    /* String literal */
    if (*p == '"') {
        p++;
        char buf[STR_MAX + 1]; int i = 0;
        while (*p && *p != '"' && i < STR_MAX) buf[i++] = *p++;
        if (*p == '"') p++;
        buf[i] = '\0';
        *pp = p;
        return make_str(buf);
    }

    /* Functions and variables (both start with a letter) */
    if (isalpha((unsigned char)*p)) {
        char ident[16];
        const char *start = p;
        read_ident(p, ident, sizeof(ident));
        p += strlen(ident) - (ident[strlen(ident)-1] == '$' ? 0 : 0);
        /* advance p by the raw identifier length (without $ already included) */
        p = start;
        { /* re-read advancing p manually */
            int n = 0;
            while (isalpha((unsigned char)*p) && n < (int)sizeof(ident)-2)
                ident[n++] = (char)toupper((unsigned char)*p++);
            if (*p == '$' && n < (int)sizeof(ident)-1) { ident[n++] = '$'; p++; }
            ident[n] = '\0';
        }

        /* Check for built-in functions */

        /* INT(x) */
        if (strcmp(ident, "INT") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            return make_num(floor(a.num));
        }
        /* ABS(x) */
        if (strcmp(ident, "ABS") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            return make_num(fabs(a.num));
        }
        /* SQR(x) */
        if (strcmp(ident, "SQR") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            return make_num(sqrt(a.num));
        }
        /* RND(x) — returns 0..1 */
        if (strcmp(ident, "RND") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            eval_expr(b, &p);   /* consume argument, ignore value */
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            return make_num((double)rand() / ((double)RAND_MAX + 1.0));
        }
        /* LEN(s$) */
        if (strcmp(ident, "LEN") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_STR) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            return make_num((double)strlen(a.str));
        }
        /* ASC(s$) */
        if (strcmp(ident, "ASC") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_STR) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            return make_num(a.str[0] ? (double)(unsigned char)a.str[0] : 0.0);
        }
        /* VAL(s$) */
        if (strcmp(ident, "VAL") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_STR) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            return make_num(atof(a.str));
        }
        /* CHR$(n) */
        if (strcmp(ident, "CHR$") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_str(""); }
            char s[2]; s[0] = (char)(int)a.num; s[1] = '\0';
            return make_str(s);
        }
        /* STR$(n) */
        if (strcmp(ident, "STR$") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t a = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (a.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_str(""); }
            char tmp[64];
            if (a.num == (long long)a.num && a.num >= -1e15 && a.num <= 1e15)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)a.num);
            else
                snprintf(tmp, sizeof(tmp), "%g", a.num);
            return make_str(tmp);
        }
        /* LEFT$(s$, n) */
        if (strcmp(ident, "LEFT$") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t s = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ',') p++;
            val_t n = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (s.type != VT_STR || n.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_str(""); }
            int len = (int)n.num;
            if (len < 0) len = 0;
            if (len > (int)strlen(s.str)) len = (int)strlen(s.str);
            char buf[STR_MAX + 1];
            strncpy(buf, s.str, (size_t)len); buf[len] = '\0';
            return make_str(buf);
        }
        /* RIGHT$(s$, n) */
        if (strcmp(ident, "RIGHT$") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t s = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ',') p++;
            val_t n = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (s.type != VT_STR || n.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_str(""); }
            int slen = (int)strlen(s.str);
            int len  = (int)n.num;
            if (len < 0) len = 0;
            if (len > slen) len = slen;
            return make_str(s.str + slen - len);
        }
        /* MID$(s$, start [, len]) — 1-based */
        if (strcmp(ident, "MID$") == 0) {
            p = skip_ws(p); if (*p == '(') p++;
            val_t s   = eval_expr(b, &p);
            p = skip_ws(p); if (*p == ',') p++;
            val_t sta = eval_expr(b, &p);
            int take = -1;
            p = skip_ws(p);
            if (*p == ',') {
                p++;
                val_t ln = eval_expr(b, &p);
                take = (int)ln.num;
            }
            p = skip_ws(p); if (*p == ')') p++;
            *pp = p;
            if (s.type != VT_STR || sta.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_str(""); }
            int slen  = (int)strlen(s.str);
            int start = (int)sta.num - 1;  /* convert to 0-based */
            if (start < 0) start = 0;
            if (start > slen) start = slen;
            int avail = slen - start;
            if (take < 0 || take > avail) take = avail;
            char buf[STR_MAX + 1];
            strncpy(buf, s.str + start, (size_t)take); buf[take] = '\0';
            return make_str(buf);
        }

        /* Variable */
        /* ident is already uppercased; ends with '$' for string vars */
        if (strlen(ident) >= 1 && isalpha((unsigned char)ident[0])) {
            int idx = ident[0] - 'A';
            if (ident[1] == '$') {
                *pp = p;
                return make_str(b->strvars[idx]);
            } else {
                *pp = p;
                return make_num(b->numvars[idx]);
            }
        }
    }

    bas_error(b, "SYNTAX ERROR");
    *pp = p;
    return make_num(0);
}

/* --- unary: NOT / unary minus --- */
static val_t eval_unary(bas_t *b, const char **pp)
{
    const char *p = skip_ws(*pp);
    if (*p == '-') {
        p++;
        val_t v = eval_unary(b, &p);
        *pp = p;
        if (v.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
        return make_num(-v.num);
    }
    if (match_kw(&p, "NOT")) {
        p = skip_ws(p);
        val_t v = eval_unary(b, &p);
        *pp = p;
        if (v.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
        return make_num(v.num == 0.0 ? -1.0 : 0.0);
    }
    return eval_primary(b, pp);
}

/* --- power: a ^ b --- */
static val_t eval_pow(bas_t *b, const char **pp)
{
    val_t left = eval_unary(b, pp);
    for (;;) {
        const char *p = skip_ws(*pp);
        if (*p != '^') break;
        p++;
        val_t right = eval_unary(b, &p);
        *pp = p;
        if (left.type != VT_NUM || right.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
        left = make_num(pow(left.num, right.num));
    }
    return left;
}

/* --- multiply / divide --- */
static val_t eval_mul(bas_t *b, const char **pp)
{
    val_t left = eval_pow(b, pp);
    for (;;) {
        const char *p = skip_ws(*pp);
        if (*p != '*' && *p != '/') break;
        char op = *p++;
        val_t right = eval_pow(b, &p);
        *pp = p;
        if (left.type != VT_NUM || right.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
        if (op == '/' && right.num == 0.0) { bas_error(b, "DIVISION BY ZERO"); return make_num(0); }
        left = make_num(op == '*' ? left.num * right.num : left.num / right.num);
    }
    return left;
}

/* --- add / subtract (also string concat with +) --- */
static val_t eval_add(bas_t *b, const char **pp)
{
    val_t left = eval_mul(b, pp);
    for (;;) {
        const char *p = skip_ws(*pp);
        if (*p != '+' && *p != '-') break;
        char op = *p++;
        val_t right = eval_mul(b, &p);
        *pp = p;
        if (b->error) return make_num(0);
        if (op == '+' && left.type == VT_STR && right.type == VT_STR) {
            char buf[STR_MAX + 1];
            strncpy(buf, left.str, STR_MAX); buf[STR_MAX] = '\0';
            size_t l = strlen(buf);
            strncpy(buf + l, right.str, STR_MAX - l); buf[STR_MAX] = '\0';
            left = make_str(buf);
        } else {
            if (left.type != VT_NUM || right.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
            left = make_num(op == '+' ? left.num + right.num : left.num - right.num);
        }
    }
    return left;
}

/* --- comparison --- */
static val_t eval_cmp(bas_t *b, const char **pp)
{
    val_t left = eval_add(b, pp);
    if (b->error) return make_num(0);
    const char *p = skip_ws(*pp);

    /* Determine operator */
    int op = 0;  /* 0=none 1=< 2=> 3== 4=<> 5=<= 6=>= */
    if (p[0] == '<' && p[1] == '>') { op = 4; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { op = 5; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { op = 6; p += 2; }
    else if (p[0] == '<') { op = 1; p++; }
    else if (p[0] == '>') { op = 2; p++; }
    else if (p[0] == '=') { op = 3; p++; }

    if (op == 0) return left;

    val_t right = eval_add(b, &p);
    *pp = p;
    if (b->error) return make_num(0);

    int cmp;
    if (left.type == VT_STR && right.type == VT_STR) {
        cmp = strcmp(left.str, right.str);
    } else if (left.type == VT_NUM && right.type == VT_NUM) {
        cmp = (left.num < right.num) ? -1 : (left.num > right.num) ? 1 : 0;
    } else {
        bas_error(b, "TYPE MISMATCH"); return make_num(0);
    }

    double result = 0.0;
    switch (op) {
        case 1: result = cmp <  0 ? -1.0 : 0.0; break;
        case 2: result = cmp >  0 ? -1.0 : 0.0; break;
        case 3: result = cmp == 0 ? -1.0 : 0.0; break;
        case 4: result = cmp != 0 ? -1.0 : 0.0; break;
        case 5: result = cmp <= 0 ? -1.0 : 0.0; break;
        case 6: result = cmp >= 0 ? -1.0 : 0.0; break;
    }
    return make_num(result);
}

/* --- AND --- */
static val_t eval_and(bas_t *b, const char **pp)
{
    val_t left = eval_cmp(b, pp);
    for (;;) {
        const char *p = skip_ws(*pp);
        if (!match_kw(&p, "AND")) break;
        val_t right = eval_cmp(b, &p);
        *pp = p;
        if (b->error) return make_num(0);
        if (left.type != VT_NUM || right.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
        left = make_num((left.num != 0.0 && right.num != 0.0) ? -1.0 : 0.0);
    }
    return left;
}

/* --- OR (top-level) --- */
static val_t eval_expr(bas_t *b, const char **pp)
{
    val_t left = eval_and(b, pp);
    for (;;) {
        const char *p = skip_ws(*pp);
        if (!match_kw(&p, "OR")) break;
        val_t right = eval_and(b, &p);
        *pp = p;
        if (b->error) return make_num(0);
        if (left.type != VT_NUM || right.type != VT_NUM) { bas_error(b, "TYPE MISMATCH"); return make_num(0); }
        left = make_num((left.num != 0.0 || right.num != 0.0) ? -1.0 : 0.0);
    }
    return left;
}

/* -----------------------------------------------------------------------
 * goto_line — set pc to the line with the given number
 * ----------------------------------------------------------------------- */

static void goto_line(bas_t *b, uint16_t num)
{
    int idx = find_line(b, num);
    if (idx < 0) {
        char tmp[40]; snprintf(tmp, sizeof(tmp), "UNDEFINED LINE %u", (unsigned)num);
        bas_error(b, tmp); return;
    }
    b->pc = idx;
    b->stmt = b->lines[idx].text;
}

/* -----------------------------------------------------------------------
 * assign_var — write a val_t into the appropriate variable slot
 * ----------------------------------------------------------------------- */

static void assign_var(bas_t *b, const char *name, val_t v)
{
    if (!isalpha((unsigned char)name[0])) { bas_error(b, "SYNTAX ERROR"); return; }
    int idx = toupper((unsigned char)name[0]) - 'A';
    int is_str = (name[1] == '$');
    if (is_str) {
        if (v.type == VT_NUM) {
            /* auto-convert number to string */
            char tmp[64];
            if (v.num == (long long)v.num && v.num >= -1e15 && v.num <= 1e15)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v.num);
            else
                snprintf(tmp, sizeof(tmp), "%g", v.num);
            strncpy(b->strvars[idx], tmp, STR_MAX); b->strvars[idx][STR_MAX] = '\0';
        } else {
            strncpy(b->strvars[idx], v.str, STR_MAX); b->strvars[idx][STR_MAX] = '\0';
        }
    } else {
        if (v.type == VT_STR) { bas_error(b, "TYPE MISMATCH"); return; }
        b->numvars[idx] = v.num;
    }
}

/* -----------------------------------------------------------------------
 * exec_one_stmt — execute a single statement (text points past any leading
 * whitespace).  Returns true if the line's pc/stmt were altered (GOTO etc.)
 * ----------------------------------------------------------------------- */

static bool exec_one_stmt(bas_t *b, const char *s)
{
    s = skip_ws(s);
    if (!*s || *s == '\n' || *s == '\r') return false;

    /* REM or ' */
    if (strncasecmp(s, "REM", 3) == 0 || *s == '\'') return false;

    /* PRINT */
    if (match_kw(&s, "PRINT") || match_kw(&s, "?")) {
        s = skip_ws(s);
        bool need_crlf = true;
        if (!*s || *s == ':' || *s == '\0') {
            bprintcrlf(b); return false;
        }
        while (*s && *s != ':') {
            if (*s == ';' || *s == ',') {
                if (*s == ',') {
                    /* tab to next 14-column boundary */
                    int col = b->dos->carpos;
                    int spaces = 14 - (col % 14);
                    for (int i = 0; i < spaces; i++) bprintc(b, ' ');
                }
                s++;
                s = skip_ws(s);
                if (!*s || *s == ':') { need_crlf = false; break; }
                continue;
            }
            val_t v = eval_expr(b, &s);
            if (b->error) return false;
            if (v.type == VT_NUM) bprint_double(b, v.num);
            else                  bprint(b, v.str);
            s = skip_ws(s);
            if (*s == ';') {
                s++;
                s = skip_ws(s);
                if (!*s || *s == ':') { need_crlf = false; break; }
            } else if (*s == ',') {
                /* handled at top of loop */
            } else {
                break;
            }
        }
        if (need_crlf) bprintcrlf(b);
        return false;
    }

    /* INPUT */
    if (match_kw(&s, "INPUT")) {
        s = skip_ws(s);
        /* Optional prompt string followed by ; */
        if (*s == '"') {
            const char *q = s;
            val_t prompt = eval_expr(b, &q);
            q = skip_ws(q);
            if (*q == ';') {
                bprint(b, prompt.str);
                s = q + 1;
            }
        }
        s = skip_ws(s);
        /* Read variable name */
        char varname[4] = {0};
        if (isalpha((unsigned char)*s)) {
            varname[0] = (char)toupper((unsigned char)*s++);
            if (*s == '$') { varname[1] = '$'; s++; }
        } else {
            bas_error(b, "SYNTAX ERROR"); return false;
        }
        bprint(b, "? ");
        char linebuf[STR_MAX + 1];
        bread_line(b, linebuf, sizeof(linebuf));
        if (varname[1] == '$') {
            assign_var(b, varname, make_str(linebuf));
        } else {
            assign_var(b, varname, make_num(atof(linebuf)));
        }
        return false;
    }

    /* LET (optional keyword) */
    if (match_kw(&s, "LET")) s = skip_ws(s);

    /* Assignment: detect "VAR =" */
    if (isalpha((unsigned char)*s)) {
        const char *varstart = s;
        char varname[4] = {0};
        varname[0] = (char)toupper((unsigned char)*s++);
        if (*s == '$') { varname[1] = '$'; s++; }
        s = skip_ws(s);
        if (*s == '=') {
            s++;
            val_t v = eval_expr(b, &s);
            if (!b->error) assign_var(b, varname, v);
            return false;
        }
        /* Not an assignment — reset pointer and fall through to error */
        s = varstart;
    }

    /* GOTO */
    if (match_kw(&s, "GOTO")) {
        s = skip_ws(s);
        char *end;
        uint16_t num = (uint16_t)strtoul(s, &end, 10);
        goto_line(b, num);
        return true;
    }

    /* GOSUB */
    if (match_kw(&s, "GOSUB")) {
        s = skip_ws(s);
        char *end;
        uint16_t num = (uint16_t)strtoul(s, &end, 10);
        if (b->gsp >= GOSUB_DEPTH) { bas_error(b, "GOSUB OVERFLOW"); return false; }
        b->gosubstack[b->gsp++] = b->pc + 1;
        goto_line(b, num);
        return true;
    }

    /* RETURN */
    if (match_kw(&s, "RETURN")) {
        if (b->gsp <= 0) { bas_error(b, "RETURN WITHOUT GOSUB"); return false; }
        b->pc = b->gosubstack[--b->gsp];
        if (b->pc < b->nlines)
            b->stmt = b->lines[b->pc].text;
        else
            b->running = false;
        return true;
    }

    /* FOR */
    if (match_kw(&s, "FOR")) {
        s = skip_ws(s);
        if (!isalpha((unsigned char)*s)) { bas_error(b, "SYNTAX ERROR"); return false; }
        int var = toupper((unsigned char)*s++) - 'A';
        s = skip_ws(s);
        if (*s != '=') { bas_error(b, "SYNTAX ERROR"); return false; }
        s++;
        val_t start = eval_expr(b, &s);
        if (b->error) return false;
        if (!match_kw(&s, "TO")) { bas_error(b, "SYNTAX ERROR"); return false; }
        val_t limit = eval_expr(b, &s);
        if (b->error) return false;
        double step = 1.0;
        if (match_kw(&s, "STEP")) {
            val_t sv = eval_expr(b, &s);
            if (b->error) return false;
            step = sv.num;
        }
        if (b->fsp >= FOR_DEPTH) { bas_error(b, "FOR OVERFLOW"); return false; }
        b->numvars[var] = start.num;
        for_frame_t *f = &b->forstack[b->fsp++];
        f->var    = var;
        f->limit  = limit.num;
        f->step   = step;
        f->ret_pc = b->pc;  /* index of the FOR line itself */
        return false;
    }

    /* NEXT */
    if (match_kw(&s, "NEXT")) {
        if (b->fsp <= 0) { bas_error(b, "NEXT WITHOUT FOR"); return false; }
        /* optional variable name — we just use the top of stack */
        for_frame_t *f = &b->forstack[b->fsp - 1];
        b->numvars[f->var] += f->step;
        bool done = (f->step >= 0) ? (b->numvars[f->var] > f->limit)
                                   : (b->numvars[f->var] < f->limit);
        if (!done) {
            /* jump back to the line AFTER the FOR line */
            int next_pc = f->ret_pc + 1;
            if (next_pc < b->nlines) {
                b->pc   = next_pc;
                b->stmt = b->lines[next_pc].text;
            }
            return true;
        } else {
            b->fsp--;  /* loop exhausted */
        }
        return false;
    }

    /* IF … THEN */
    if (match_kw(&s, "IF")) {
        val_t cond = eval_expr(b, &s);
        if (b->error) return false;
        if (!match_kw(&s, "THEN")) { bas_error(b, "SYNTAX ERROR"); return false; }
        s = skip_ws(s);
        if (cond.num != 0.0) {
            /* THEN linenumber  or  THEN statement */
            if (isdigit((unsigned char)*s)) {
                char *end;
                uint16_t num = (uint16_t)strtoul(s, &end, 10);
                goto_line(b, num);
                return true;
            } else {
                return exec_one_stmt(b, s);
            }
        }
        return false;
    }

    /* END / STOP */
    if (match_kw(&s, "END") || match_kw(&s, "STOP")) {
        b->running = false;
        return false;
    }

    bas_error(b, "SYNTAX ERROR");
    return false;
}

/* -----------------------------------------------------------------------
 * exec_line — execute all ':'-separated statements in a line
 * Returns true if pc/stmt were changed by the statements.
 * ----------------------------------------------------------------------- */

static void exec_line(bas_t *b, const char *text)
{
    const char *p = text;
    for (;;) {
        p = skip_ws(p);
        if (!*p) break;
        if (exec_one_stmt(b, p)) return;   /* pc/stmt already updated */
        if (b->error || !b->running) return;
        /* Advance past this statement (skip to next ':') */
        while (*p && *p != ':') {
            if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
            else p++;
        }
        if (*p == ':') p++;
    }
}

/* -----------------------------------------------------------------------
 * bas_load — read the file from disk into the lines[] array
 * ----------------------------------------------------------------------- */

static int bas_load(bas_t *b, const char *filename)
{
    dos_t *dos = b->dos;

    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    const char *p = filename;
    dos_makefcb(dos, &p, &fcb, 0);

    if (dos_open(dos, &fcb) != DOS_OK) return -1;

    uint32_t filsiz = fcb.filsiz;
    if (filsiz == 0) { dos_close(dos, &fcb); return 0; }

    /* Read entire file into a heap buffer */
    char *flat = malloc(filsiz + 2);
    if (!flat) { dos_close(dos, &fcb); return -2; }

    uint8_t *old_dma = dos->dmaadd;
    uint8_t  buf[128];
    dos_setdma(dos, buf);
    fcb.recsiz = 128;

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
    flat[pos]     = '\n';
    flat[pos + 1] = '\0';

    dos_close(dos, &fcb);
    dos_setdma(dos, old_dma);

    /* Parse lines: each line is  LINENUM  STATEMENT_TEXT\n */
    char *line = flat;
    for (uint32_t i = 0; i <= pos; i++) {
        if (flat[i] != '\n') continue;
        flat[i] = '\0';
        /* strip trailing \r */
        size_t ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[ll-1] = '\0';

        /* skip empty lines */
        const char *lp = line;
        while (*lp == ' ') lp++;
        if (*lp == '\0') { line = flat + i + 1; continue; }

        /* parse line number */
        if (!isdigit((unsigned char)*lp)) { line = flat + i + 1; continue; }
        char *end;
        unsigned long lnum = strtoul(lp, &end, 10);
        if (b->nlines >= MAX_LINES) {
            free(flat); return -3;
        }
        /* skip one space after line number */
        while (*end == ' ') end++;

        b->lines[b->nlines].linenum = (uint16_t)lnum;
        b->lines[b->nlines].text    = strdup(end);
        b->nlines++;

        line = flat + i + 1;
    }

    free(flat);

    /* Insertion-sort by line number (already sorted in practice) */
    for (int i = 1; i < b->nlines; i++) {
        bas_line_t key = b->lines[i];
        int j = i - 1;
        while (j >= 0 && b->lines[j].linenum > key.linenum) {
            b->lines[j + 1] = b->lines[j];
            j--;
        }
        b->lines[j + 1] = key;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * bas_free — release heap memory
 * ----------------------------------------------------------------------- */

static void bas_free(bas_t *b)
{
    for (int i = 0; i < b->nlines; i++) {
        free(b->lines[i].text);
        b->lines[i].text = NULL;
    }
}

/* -----------------------------------------------------------------------
 * basic_run — public entry point
 * ----------------------------------------------------------------------- */

void basic_run(dos_t *dos, const char *filename)
{
    bas_t *b = calloc(1, sizeof(*b));
    if (!b) {
        chardev_out(dos, 'M'); /* out of memory — best effort */
        return;
    }
    b->dos     = dos;
    b->running = true;
    b->pc      = 0;
    b->fsp     = 0;
    b->gsp     = 0;

    int rc = bas_load(b, filename);
    if (rc == -1) {
        const char *msg = "File not found\r\n";
        while (*msg) chardev_out(dos, (uint8_t)*msg++);
        free(b);
        return;
    }
    if (rc < 0 || b->nlines == 0) {
        const char *msg = (rc == -2) ? "Out of memory\r\n" : "Empty program\r\n";
        while (*msg) chardev_out(dos, (uint8_t)*msg++);
        bas_free(b); free(b);
        return;
    }

    b->stmt = b->lines[0].text;

    while (b->running && !b->error && b->pc < b->nlines) {
        int current_pc = b->pc;
        exec_line(b, b->lines[current_pc].text);
        if (b->error || !b->running) break;
        /* If exec_line didn't change pc, advance to next line */
        if (b->pc == current_pc) {
            b->pc++;
            if (b->pc < b->nlines)
                b->stmt = b->lines[b->pc].text;
        }
    }

    bas_free(b);
    free(b);
}
