/*
 * PS1 port — minimal C library shim (stdio).
 *
 * Self-contained printf family (with real IEEE float %f/%e/%g) and sscanf
 * (with %[...] scansets, as used by IniParser). File I/O is stubbed — the
 * CD-ROM ISO9660 backing arrives in Phase 6.
 */
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "math.h"

#include "common/syscalls/syscalls.h"

/* ------------------------------------------------------------------ */
/* printf core                                                        */
/* ------------------------------------------------------------------ */

struct outbuf {
    char *buf;
    size_t len; /* characters emitted so far */
    size_t cap; /* capacity including terminating NUL; 0 = unlimited */
};

static void ob_putc(struct outbuf *ob, char c) {
    if (ob->cap == 0 || ob->len + 1 < ob->cap) {
        if (ob->buf)
            ob->buf[ob->len] = c;
    }
    ob->len++;
}

/* Convert unsigned to digits (most-significant first). buf needs 33 bytes. */
static void utoa32(unsigned int val, char *buf, unsigned base, int upper) {
    char tmp[33];
    int n = 0;
    do {
        unsigned d = val % base;
        val /= base;
        tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
    } while (val);
    for (int i = 0; i < n; ++i)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

/* Convert signed int to decimal, including sign. buf needs 12 bytes. */
static void itoa32(int val, char *buf) {
    unsigned u;
    if (val < 0) {
        buf[0] = '-';
        u      = (unsigned)(-(val + 1)) + 1u;
        utoa32(u, buf + 1, 10, 0);
    } else {
        utoa32((unsigned)val, buf, 10, 0);
    }
}

/* Double -> decimal string with `precision` fractional digits. buf needs 80. */
static void ftoa(double val, char *buf, int precision) {
    char *p = buf;
    if (val < 0.0) {
        *p++ = '-';
        val  = -val;
    }
    double rounder = 0.5;
    for (int i = 0; i < precision; ++i)
        rounder *= 0.1;
    val += rounder;

    unsigned long ipart = (unsigned long)val;
    double fpart        = val - (double)ipart;

    utoa32((unsigned)ipart, p, 10, 0);
    p += strlen(p);
    if (precision > 0) {
        *p++ = '.';
        for (int i = 0; i < precision; ++i) {
            fpart *= 10.0;
            int d = (int)fpart;
            *p++  = (char)('0' + d);
            fpart -= (double)d;
        }
    }
    *p = 0;
}

static void output_number(struct outbuf *ob, const char *digits, int len, int width, int prec,
                          int minus, int zero, int plus, int space) {
    int sign       = 0;
    const char *num = digits;
    if (*num == '-') {
        sign = '-';
        ++num;
        --len;
    } else if (plus) {
        sign = '+';
    } else if (space) {
        sign = ' ';
    }

    int numlen = len;
    if (prec > numlen)
        numlen = prec;
    int total = numlen + (sign ? 1 : 0);
    int pad   = width - total;

    if (zero && !minus && prec < 0) {
        if (sign)
            ob_putc(ob, (char)sign);
        while (pad-- > 0)
            ob_putc(ob, '0');
    } else {
        if (!minus)
            while (pad-- > 0)
                ob_putc(ob, ' ');
        if (sign)
            ob_putc(ob, (char)sign);
    }
    int zp = prec - len;
    while (zp-- > 0)
        ob_putc(ob, '0');
    for (int i = 0; i < len; ++i)
        ob_putc(ob, num[i]);
    if (minus)
        while (pad-- > 0)
            ob_putc(ob, ' ');
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct outbuf ob;
    ob.buf = buf;
    ob.len = 0;
    ob.cap = n;
    if (buf && n)
        buf[0] = 0;

    const char *f = fmt;
    while (*f) {
        if (*f != '%') {
            ob_putc(&ob, *f++);
            continue;
        }
        ++f;

        int minus = 0, plus = 0, space = 0, zero = 0, hash = 0;
        for (;; ++f) {
            char c = *f;
            if (c == '-')
                minus = 1;
            else if (c == '+')
                plus = 1;
            else if (c == ' ')
                space = 1;
            else if (c == '0')
                zero = 1;
            else if (c == '#')
                hash = 1;
            else
                break;
        }

        int width = 0;
        if (*f == '*') {
            width = va_arg(ap, int);
            ++f;
            if (width < 0) {
                minus = 1;
                width = -width;
            }
        } else {
            while (*f >= '0' && *f <= '9') {
                width = width * 10 + (*f - '0');
                ++f;
            }
        }

        int prec = -1;
        if (*f == '.') {
            ++f;
            prec = 0;
            if (*f == '*') {
                prec = va_arg(ap, int);
                ++f;
            } else {
                while (*f >= '0' && *f <= '9') {
                    prec = prec * 10 + (*f - '0');
                    ++f;
                }
            }
        }

        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'L')
            ++f;

        char spec = *f++;

        switch (spec) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s)
                    s = "(null)";
                int slen = (int)strlen(s);
                if (prec >= 0 && slen > prec)
                    slen = prec;
                int pad = width - slen;
                if (!minus)
                    while (pad-- > 0)
                        ob_putc(&ob, ' ');
                for (int i = 0; i < slen; ++i)
                    ob_putc(&ob, s[i]);
                if (minus)
                    while (pad-- > 0)
                        ob_putc(&ob, ' ');
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                int pad = width - 1;
                if (!minus)
                    while (pad-- > 0)
                        ob_putc(&ob, ' ');
                ob_putc(&ob, c);
                if (minus)
                    while (pad-- > 0)
                        ob_putc(&ob, ' ');
                break;
            }
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                char tmp[12];
                itoa32(v, tmp);
                output_number(&ob, tmp, (int)strlen(tmp), width, prec, minus, zero, plus, space);
                break;
            }
            case 'u': {
                unsigned v = va_arg(ap, unsigned);
                char tmp[12];
                utoa32(v, tmp, 10, 0);
                output_number(&ob, tmp, (int)strlen(tmp), width, prec, minus, zero, plus, space);
                break;
            }
            case 'x':
            case 'X': {
                unsigned v = va_arg(ap, unsigned);
                char tmp[12];
                utoa32(v, tmp, 16, spec == 'X');
                output_number(&ob, tmp, (int)strlen(tmp), width, prec, minus, zero, plus, space);
                break;
            }
            case 'o': {
                unsigned v = va_arg(ap, unsigned);
                char tmp[12];
                utoa32(v, tmp, 8, 0);
                output_number(&ob, tmp, (int)strlen(tmp), width, prec, minus, zero, plus, space);
                break;
            }
            case 'p': {
                void *p = va_arg(ap, void *);
                char tmp[12];
                utoa32((unsigned)p, tmp, 16, 0);
                output_number(&ob, tmp, (int)strlen(tmp), width, prec, minus, zero, plus, space);
                break;
            }
            case 'f':
            case 'F': {
                double v = va_arg(ap, double);
                int p    = prec < 0 ? 6 : prec;
                char tmp[80];
                ftoa(v, tmp, p);
                int len = (int)strlen(tmp);
                int pad = width - len;
                if (!minus)
                    while (pad-- > 0)
                        ob_putc(&ob, zero ? '0' : ' ');
                for (int i = 0; i < len; ++i)
                    ob_putc(&ob, tmp[i]);
                if (minus)
                    while (pad-- > 0)
                        ob_putc(&ob, ' ');
                break;
            }
            case '%':
                ob_putc(&ob, '%');
                break;
            default:
                ob_putc(&ob, '%');
                ob_putc(&ob, spec);
                break;
        }
    }

    if (buf && n)
        buf[ob.len < n ? ob.len : n - 1] = 0;
    return (int)ob.len;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, 0x7fffffff, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    static char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    syscall_write(1, buf, (size_t)n);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/* sscanf core                                                        */
/* ------------------------------------------------------------------ */

static int is_space_c(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    const char *s = str;
    const char *f = fmt;
    int assigned  = 0;

    while (*f) {
        if (is_space_c(*f)) {
            while (is_space_c(*s))
                ++s;
            ++f;
            continue;
        }
        if (*f != '%') {
            while (is_space_c(*s))
                ++s;
            if (*s != *f)
                return assigned;
            ++s;
            ++f;
            continue;
        }
        ++f; /* skip % */
        if (*f == '%') {
            while (is_space_c(*s))
                ++s;
            if (*s != '%')
                return assigned;
            ++s;
            ++f;
            continue;
        }

        int suppress = 0;
        if (*f == '*') {
            suppress = 1;
            ++f;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f - '0');
            ++f;
        }
        if (width == 0)
            width = 0x7fffffff;
        while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'z' || *f == 'j' || *f == 't')
            ++f;
        char spec = *f++;

        switch (spec) {
            case 's': {
                while (is_space_c(*s))
                    ++s;
                if (suppress) {
                    while (*s && !is_space_c(*s) && width-- > 0)
                        ++s;
                } else {
                    char *dst = va_arg(ap, char *);
                    int n     = 0;
                    while (*s && !is_space_c(*s) && n < width)
                        dst[n++] = *s++;
                    dst[n] = 0;
                    if (n)
                        ++assigned;
                    else
                        return assigned;
                }
                break;
            }
            case '[': {
                int negate = 0;
                if (*f == '^') {
                    negate = 1;
                    ++f;
                }
                char set[256];
                memset(set, 0, sizeof(set));
                if (*f == ']') {
                    set[(unsigned char)']'] = 1;
                    ++f;
                }
                while (*f && *f != ']') {
                    set[(unsigned char)*f] = 1;
                    ++f;
                }
                if (*f == ']')
                    ++f;
                if (suppress) {
                    while (*s && width-- > 0) {
                        unsigned char c = (unsigned char)*s;
                        int in          = set[c] ? 1 : 0;
                        if (negate)
                            in = !in;
                        if (!in)
                            break;
                        ++s;
                    }
                } else {
                    char *dst = va_arg(ap, char *);
                    int n     = 0;
                    while (*s && n < width) {
                        unsigned char c = (unsigned char)*s;
                        int in          = set[c] ? 1 : 0;
                        if (negate)
                            in = !in;
                        if (!in)
                            break;
                        dst[n++] = *s++;
                    }
                    dst[n] = 0;
                    if (n)
                        ++assigned;
                    else
                        return assigned;
                }
                break;
            }
            case 'c': {
                if (width == 0x7fffffff)
                    width = 1;
                if (suppress) {
                    while (*s && width-- > 0)
                        ++s;
                } else {
                    char *dst = va_arg(ap, char *);
                    int n     = 0;
                    while (*s && n < width)
                        dst[n++] = *s++;
                    if (n)
                        ++assigned;
                }
                break;
            }
            case 'd':
            case 'i': {
                while (is_space_c(*s))
                    ++s;
                char *end;
                long v = strtol(s, &end, spec == 'i' ? 0 : 10);
                if (end == s)
                    return assigned;
                if (!suppress) {
                    *va_arg(ap, int *) = (int)v;
                    ++assigned;
                }
                s = end;
                break;
            }
            case 'u': {
                while (is_space_c(*s))
                    ++s;
                char *end;
                unsigned long v = strtoul(s, &end, 10);
                if (end == s)
                    return assigned;
                if (!suppress) {
                    *va_arg(ap, unsigned *) = (unsigned)v;
                    ++assigned;
                }
                s = end;
                break;
            }
            case 'x':
            case 'X': {
                while (is_space_c(*s))
                    ++s;
                char *end;
                unsigned long v = strtoul(s, &end, 16);
                if (end == s)
                    return assigned;
                if (!suppress) {
                    *va_arg(ap, unsigned *) = (unsigned)v;
                    ++assigned;
                }
                s = end;
                break;
            }
            case 'o': {
                while (is_space_c(*s))
                    ++s;
                char *end;
                unsigned long v = strtoul(s, &end, 8);
                if (end == s)
                    return assigned;
                if (!suppress) {
                    *va_arg(ap, unsigned *) = (unsigned)v;
                    ++assigned;
                }
                s = end;
                break;
            }
            case 'f':
            case 'e':
            case 'g':
            case 'E':
            case 'G': {
                while (is_space_c(*s))
                    ++s;
                char *end;
                double v = strtod(s, &end);
                if (end == s)
                    return assigned;
                if (!suppress) {
                    *va_arg(ap, float *) = (float)v;
                    ++assigned;
                }
                s = end;
                break;
            }
            case 'n': {
                if (!suppress)
                    *va_arg(ap, int *) = (int)(s - str);
                break;
            }
            default:
                return assigned;
        }
    }
    return assigned;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/* File I/O — implemented in ps1/cdrom_fs.cpp (Phase 6, CD-ROM ISO9660) */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* psyqo vxprintf shim                                                */
/* ------------------------------------------------------------------ */

/* psyqo's xprintf.c provides vxprintf; kernel.o and font.o (inside libpsyqo.a)
 * reference it, which would otherwise drag xprintf.o into the link — and
 * xprintf.o also defines vsprintf/vsnprintf, colliding with ours above.
 * Providing vxprintf here keeps xprintf.o out of the link entirely. We format
 * the whole string and invoke the callback once; psyqo's callbacks
 * (syscall_write and font glyph emission) are stateless across calls, so a
 * single invocation is equivalent to xprintf's per-segment calls. */
extern "C" int vxprintf(void (*func)(const char *, int, void *), void *opaque, const char *fmt, va_list ap) {
    static char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    int len = n;
    if (len > (int)sizeof(buf) - 1)
        len = (int)sizeof(buf) - 1;
    if (len > 0)
        func(buf, len, opaque);
    return n;
}
