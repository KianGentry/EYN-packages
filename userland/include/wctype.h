#pragma once

#include <wchar.h>

typedef unsigned long wctype_t;

int iswspace(wint_t c);
int iswdigit(wint_t c);
wint_t towlower(wint_t c);
