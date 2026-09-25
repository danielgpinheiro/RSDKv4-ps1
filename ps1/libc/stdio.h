/*
 * PS1 port — minimal C library shim (stdio.h)
 *
 * Provides a self-contained sprintf/snprintf/vsprintf/vsnprintf/printf and an
 * sscanf. The %f/%g/%e specifiers format real IEEE float/double values (unlike
 * psyqo's xprintf, whose %f expects fixed-point — RSDKv2 passes real floats).
 *
 * File I/O (fopen/fread/...) is stubbed for Phase 2 of the port; the real
 * CD-ROM ISO9660 backing is added in Phase 6.
 */
#ifndef PS1_STDIO_H
#define PS1_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _FILE {
    uint32_t lba;   /* ISO9660 extent start (sector) */
    uint32_t size;  /* file size in bytes */
    uint32_t pos;   /* current byte offset */
    int      valid; /* nonzero while open */
    const uint8_t *mem; /* resident file (cdrom_fs PS1FsPreloadDir): served from RAM, no CD access */
} FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF      (-1)

#define stdin  ((FILE *)0)
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)

#define BUFSIZ 512
#define FILENAME_MAX 256

int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
int   fflush(FILE *f);
char *fgets(char *s, int n, FILE *f);
int   fputs(const char *s, FILE *f);
int   fgetc(FILE *f);
int   fputc(int c, FILE *f);
int   getc(FILE *f);
int   putc(int c, FILE *f);
int   puts(const char *s);
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);
void  perror(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* PS1_STDIO_H */
