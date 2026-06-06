#pragma once

#include <stddef.h>

typedef int wint_t;
typedef struct {
	unsigned int __state;
} mbstate_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

size_t wcslen(const wchar_t* s);
int wcscmp(const wchar_t* a, const wchar_t* b);
wchar_t* wcschr(const wchar_t* s, wchar_t c);
size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps);
int wcwidth(wchar_t wc);
