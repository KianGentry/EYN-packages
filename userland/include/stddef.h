#pragma once

// Minimal stddef.h for EYN-OS userland (i386, freestanding).

typedef unsigned int size_t;
typedef int ptrdiff_t;

#ifndef __cplusplus
typedef int wchar_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#endif
