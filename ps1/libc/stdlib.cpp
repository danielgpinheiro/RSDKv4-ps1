/*
 * PS1 port — minimal C library shim (stdlib functions).
 * malloc/free/realloc are thin wrappers over psyqo's allocator.
 */
#include "stdlib.h"
#include "string.h"

void *malloc(size_t size) {
    return psyqo_malloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p      = psyqo_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    return psyqo_realloc(ptr, size);
}

void free(void *ptr) {
    psyqo_free(ptr);
}

int abs(int n) {
    return n < 0 ? -n : n;
}

long labs(long n) {
    return n < 0 ? -n : n;
}

long long llabs(long long n) {
    return n < 0 ? -n : n;
}

int atoi(const char *s) {
    return (int)strtol(s, 0, 10);
}

long atol(const char *s) {
    return strtol(s, 0, 10);
}

long long atoll(const char *s) {
    long long r = 0;
    int neg     = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        ++s;
    if (*s == '-') {
        neg = 1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        r = r * 10 + (*s - '0');
        ++s;
    }
    return neg ? -r : r;
}

long strtol(const char *s, char **endp, int base) {
    long r   = 0;
    int neg  = 0;
    int any  = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        ++s;
    if (*s == '-') {
        neg = 1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    for (;; ++s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        r  = r * base + d;
        any = 1;
    }
    if (endp)
        *endp = (char *)s;
    if (!any)
        return 0;
    return neg ? -r : r;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    unsigned long r = 0;
    int any         = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        ++s;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    for (;; ++s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        r  = r * base + d;
        any = 1;
    }
    if (endp)
        *endp = (char *)s;
    if (!any)
        return 0;
    return r;
}

double atof(const char *s) {
    return strtod(s, 0);
}

double strtod(const char *s, char **endp) {
    const char *start = s;
    double r          = 0.0;
    int neg           = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        ++s;
    if (*s == '-') {
        neg = 1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        r = r * 10.0 + (double)(*s - '0');
        ++s;
    }
    if (*s == '.') {
        ++s;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            r += (double)(*s - '0') * scale;
            scale *= 0.1;
            ++s;
        }
    }
    if (*s == 'e' || *s == 'E') {
        ++s;
        int eneg = 0;
        if (*s == '-') {
            eneg = 1;
            ++s;
        } else if (*s == '+') {
            ++s;
        }
        int e = 0;
        while (*s >= '0' && *s <= '9') {
            e = e * 10 + (*s - '0');
            ++s;
        }
        double mult = 1.0;
        for (int i = 0; i < e; ++i)
            mult *= (eneg ? 0.1 : 10.0);
        r *= mult;
    }
    if (endp)
        *endp = (char *)s;
    if (s == start)
        return 0.0;
    return neg ? -r : r;
}

static unsigned long rng_state = 1;

int rand(void) {
    // simple LCG
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (int)((rng_state >> 16) & 0x7FFF);
}

void srand(unsigned seed) {
    rng_state = seed ? seed : 1;
}

void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *)) {
    // insertion sort — adequate for the tiny arrays RSDKv2 sorts at runtime
    char *b = (char *)base;
    for (size_t i = 1; i < nmemb; ++i) {
        char tmp[64];
        if (size > sizeof(tmp))
            continue; // too big to sort with this shim
        char *key = (char *)malloc(size);
        if (!key)
            return;
        memcpy(key, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, key) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            --j;
        }
        memcpy(b + j * size, key, size);
        free(key);
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = nmemb;
    const char *b = (const char *)base;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c      = cmp(key, b + mid * size);
        if (c < 0)
            hi = mid;
        else if (c > 0)
            lo = mid + 1;
        else
            return (void *)(b + mid * size);
    }
    return 0;
}

void abort(void) {
    // loop forever; psyqo's Kernel::abort is richer but this is the C-level
    // escape hatch. Emit a break so emulators show the crash point.
    __asm__ volatile("break 0");
    for (;;)
        ;
}

void exit(int status) {
    (void)status;
    __asm__ volatile("break 0");
    for (;;)
        ;
}
