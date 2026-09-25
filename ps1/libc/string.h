/*
 * PS1 port — minimal C library shim (string.h)
 *
 * The psyqo toolchain is freestanding: it ships no C library. This file
 * provides the standard string/memory functions RSDKv2 relies on.
 *
 * NOTE: memcpy/memset/memmove/memcmp are already provided by psyqo
 * (common/crt0/memory-s.s and memory-c.c, linked into libpsyqo.a). They are
 * declared here for compilation; the definitions resolve at link time.
 */
#ifndef PS1_STRING_H
#define PS1_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strtok(char *s, const char *delim);
char  *strdup(const char *s);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);

#ifdef __cplusplus
}
#endif

#endif /* PS1_STRING_H */
