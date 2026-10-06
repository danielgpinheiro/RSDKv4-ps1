/*
 * PS1 port — minimal C library shim (stdlib.h)
 *
 * Memory allocation is delegated to psyqo's allocator (psyqo_malloc /
 * psyqo_free / psyqo_realloc, which also backs operator new/delete). Only the
 * small numeric/string-conversion and misc helpers are implemented here.
 */
#ifndef PS1_STDLIB_H
#define PS1_STDLIB_H

#include <stddef.h>

/* psyqo allocator (psyqo_malloc / psyqo_free / psyqo_realloc / psyqo_heap_end): its own header, as upstream nugget
   turned them into inline wrappers of common/libc/alloc.h (docs/32 9d) */
#include "psyqo/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif


void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

int   abs(int n);
long  labs(long n);
long long llabs(long long n);

int    atoi(const char *s);
long   atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long   strtol(const char *s, char **endp, int base);
unsigned long strtoul(const char *s, char **endp, int base);
double strtod(const char *s, char **endp);

int  rand(void);
void srand(unsigned seed);

void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));

[[noreturn]] void abort(void);
[[noreturn]] void exit(int status);

#ifdef __cplusplus
}
#endif

#endif /* PS1_STDLIB_H */
