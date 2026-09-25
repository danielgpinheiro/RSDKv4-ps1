/*
 * PS1 port — minimal <ctype.h>.
 * RSDKv2 uses tolower/toupper (via transitively-included <ctype.h> on hosted
 * builds); the freestanding toolchain has no ctype, so provide the standard
 * character-classification set as static inline helpers.
 */
#ifndef PS1_CTYPE_H
#define PS1_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

static inline int tolower(int c) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static inline int toupper(int c) {
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

static inline int isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline int isdigit(int c) {
    return c >= '0' && c <= '9';
}

static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static inline int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

static inline int islower(int c) {
    return c >= 'a' && c <= 'z';
}

static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int iscntrl(int c) {
    return (c >= 0 && c < 0x20) || c == 0x7f;
}

static inline int ispunct(int c) {
    return c >= 0x21 && c <= 0x7e && !isalnum(c) && !isspace(c);
}

static inline int isprint(int c) {
    return c >= 0x20 && c <= 0x7e;
}

static inline int isgraph(int c) {
    return c >= 0x21 && c <= 0x7e;
}

static inline int isblank(int c) {
    return c == ' ' || c == '\t';
}

#ifdef __cplusplus
}
#endif

#endif /* PS1_CTYPE_H */
