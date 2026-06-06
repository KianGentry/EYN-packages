#include <ctype.h>
#include <wctype.h>
#include <wchar.h>

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int ispunct(int c) {
    // ASCII punctuation: visible, non-alnum, non-space.
    if (c <= 0x20 || c >= 0x7F) return 0;
    return !isalnum(c);
}

int isprint(int c) {
    return c >= 0x20 && c <= 0x7E;
}

int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

int iswspace(wint_t c) {
    return isspace((int)c);
}

int iswdigit(wint_t c) {
    return isdigit((int)c);
}

wint_t towlower(wint_t c) {
    return (wint_t)tolower((int)c);
}

size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps) {
    (void)ps;
    if (!s) return 1;
    if ((unsigned int)wc > 0xFFu) return (size_t)-1;
    s[0] = (char)wc;
    return 1;
}

int wcwidth(wchar_t wc) {
    if (wc == 0) return 0;
    if ((unsigned int)wc < 0x20u) return -1;
    if ((unsigned int)wc >= 0x7fu && (unsigned int)wc < 0xa0u) return -1;
    return 1;
}
