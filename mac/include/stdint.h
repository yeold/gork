/* stdint.h for gork's half of the Mac build: Retro68's gcc leaves it to
   newlib, which gork's half does not use; the compiler's own types. */
#ifndef GORK_MAC_STDINT_H
#define GORK_MAC_STDINT_H
#pragma GCC system_header
typedef __INT8_TYPE__    int8_t;
typedef __INT16_TYPE__   int16_t;
typedef __INT32_TYPE__   int32_t;
typedef __INT64_TYPE__   int64_t;
typedef __UINT8_TYPE__   uint8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __UINT64_TYPE__  uint64_t;
typedef __INTPTR_TYPE__  intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTMAX_TYPE__  intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;
#define INT32_MAX  __INT32_MAX__
#define INT64_MAX  __INT64_MAX__
#define UINT32_MAX __UINT32_MAX__
#define UINT64_MAX __UINT64_MAX__
#define SIZE_MAX   __SIZE_MAX__
#define INTMAX_MAX __INTMAX_MAX__
#endif
