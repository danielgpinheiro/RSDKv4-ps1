/*
 * PS1 port — self-contained <stdint.h>.
 *
 * The mipsel-none-elf toolchain is built --without-headers: its own
 * <stdint.h> is a wrapper that #include_next's a newlib <stdint.h> which
 * does not exist here, so <stdint.h> fails to compile. We define the
 * exact-width types directly from GCC's builtin type macros instead.
 */
#ifndef PS1_STDINT_H
#define PS1_STDINT_H

typedef __INT8_TYPE__   int8_t;
typedef __INT16_TYPE__  int16_t;
typedef __INT32_TYPE__  int32_t;
typedef __INT64_TYPE__  int64_t;
typedef __UINT8_TYPE__  uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;

typedef __INT_LEAST8_TYPE__   int_least8_t;
typedef __INT_LEAST16_TYPE__  int_least16_t;
typedef __INT_LEAST32_TYPE__  int_least32_t;
typedef __INT_LEAST64_TYPE__  int_least64_t;
typedef __UINT_LEAST8_TYPE__  uint_least8_t;
typedef __UINT_LEAST16_TYPE__ uint_least16_t;
typedef __UINT_LEAST32_TYPE__ uint_least32_t;
typedef __UINT_LEAST64_TYPE__ uint_least64_t;

typedef __INT_FAST8_TYPE__   int_fast8_t;
typedef __INT_FAST16_TYPE__  int_fast16_t;
typedef __INT_FAST32_TYPE__  int_fast32_t;
typedef __INT_FAST64_TYPE__  int_fast64_t;
typedef __UINT_FAST8_TYPE__  uint_fast8_t;
typedef __UINT_FAST16_TYPE__ uint_fast16_t;
typedef __UINT_FAST32_TYPE__ uint_fast32_t;
typedef __UINT_FAST64_TYPE__ uint_fast64_t;

typedef __INTPTR_TYPE__  intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTMAX_TYPE__  intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;

#define INT8_MIN   (-__INT8_MAX__ - 1)
#define INT8_MAX   (__INT8_MAX__)
#define UINT8_MAX  (__UINT8_MAX__)
#define INT16_MIN  (-__INT16_MAX__ - 1)
#define INT16_MAX  (__INT16_MAX__)
#define UINT16_MAX (__UINT16_MAX__)
#define INT32_MIN  (-__INT32_MAX__ - 1)
#define INT32_MAX  (__INT32_MAX__)
#define UINT32_MAX (__UINT32_MAX__)
#define INT64_MIN  (-__INT64_MAX__ - 1)
#define INT64_MAX  (__INT64_MAX__)
#define UINT64_MAX (__UINT64_MAX__)

#define INT_LEAST8_MIN   INT8_MIN
#define INT_LEAST8_MAX   INT8_MAX
#define UINT_LEAST8_MAX  UINT8_MAX
#define INT_LEAST16_MIN  INT16_MIN
#define INT_LEAST16_MAX  INT16_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MIN  INT32_MIN
#define INT_LEAST32_MAX  INT32_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MIN  INT64_MIN
#define INT_LEAST64_MAX  INT64_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MIN    INT8_MIN
#define INT_FAST8_MAX    INT8_MAX
#define UINT_FAST8_MAX   UINT8_MAX
#define INT_FAST16_MIN   INT16_MIN
#define INT_FAST16_MAX   INT16_MAX
#define UINT_FAST16_MAX  UINT16_MAX
#define INT_FAST32_MIN   INT32_MIN
#define INT_FAST32_MAX   INT32_MAX
#define UINT_FAST32_MAX  UINT32_MAX
#define INT_FAST64_MIN   INT64_MIN
#define INT_FAST64_MAX   INT64_MAX
#define UINT_FAST64_MAX  UINT64_MAX

#define INTPTR_MIN  (-__INTPTR_MAX__ - 1)
#define INTPTR_MAX  (__INTPTR_MAX__)
#define UINTPTR_MAX (__UINTPTR_MAX__)

#define INTMAX_MIN  (-__INTMAX_MAX__ - 1)
#define INTMAX_MAX  (__INTMAX_MAX__)
#define UINTMAX_MAX (__UINTMAX_MAX__)

#define SIZE_MAX    (__SIZE_MAX__)
#define PTRDIFF_MIN (-__PTRDIFF_MAX__ - 1)
#define PTRDIFF_MAX (__PTRDIFF_MAX__)

#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX (2147483647)
#define WINT_MIN  (0u)
#define WINT_MAX  (4294967295u)

#define INT8_C(v)   (v)
#define UINT8_C(v)  (v)
#define INT16_C(v)  (v)
#define UINT16_C(v) (v)
#define INT32_C(v)  (v)
#define UINT32_C(v) (v)
#define INT64_C(v)  (v ## LL)
#define UINT64_C(v) (v ## ULL)
#define INTMAX_C(v) (v ## LL)
#define UINTMAX_C(v) (v ## ULL)

#endif /* PS1_STDINT_H */
