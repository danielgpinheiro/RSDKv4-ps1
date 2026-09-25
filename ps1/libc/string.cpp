/*
 * PS1 port — minimal C library shim (string functions).
 * mem* are provided by psyqo; only the str* functions are implemented here.
 */
#include "string.h"
#include "stdlib.h"

size_t strlen(const char *s) {
    const char *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d++ = *src++))
        --n;
    while (n--)
        *d++ = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d)
        ++d;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d)
        ++d;
    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) {
        ++a;
        ++b;
        --n;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        ++s;
    }
    return (c == 0) ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    if (c == 0)
        return strchr(s, 0);
    while (*s) {
        if (*s == (char)c)
            last = s;
        ++s;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; ++haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) {
            ++h;
            ++n;
        }
        if (!*n)
            return (char *)haystack;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; ++s) {
        for (const char *a = accept; *a; ++a)
            if (*s == *a)
                return (char *)s;
    }
    return 0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (*s && strchr(accept, *s)) {
        ++s;
        ++n;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (*s && !strchr(reject, *s)) {
        ++s;
        ++n;
    }
    return n;
}

static char *tok_state = 0;

char *strtok(char *s, const char *delim) {
    if (s)
        tok_state = s;
    else if (!tok_state)
        return 0;
    // skip leading delimiters
    while (*tok_state && strchr(delim, *tok_state))
        ++tok_state;
    if (!*tok_state)
        return 0;
    char *start = tok_state;
    while (*tok_state && !strchr(delim, *tok_state))
        ++tok_state;
    if (*tok_state) {
        *tok_state = 0;
        ++tok_state;
    } else {
        tok_state = 0;
    }
    return start;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p   = (char *)malloc(len);
    if (p)
        strcpy(p, s);
    return p;
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && (ascii_lower(*a) == ascii_lower(*b))) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)ascii_lower(*a) - (int)(unsigned char)ascii_lower(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && (ascii_lower(*a) == ascii_lower(*b))) {
        ++a;
        ++b;
        --n;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)ascii_lower(*a) - (int)(unsigned char)ascii_lower(*b);
}
