#pragma once

#include <stddef.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

void* malloc(size_t n);
void free(void* p);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* p, size_t n);

int atexit(void (*fn)(void));
char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int putenv(char* string);
extern char** environ;
int mkstemp(char* template_str);
char* mkdtemp(char* template_str);

void _eyn_libc_init(int argc, char** argv, char** envp);

void abort(void);
void exit(int code);

#ifndef RAND_MAX
#define RAND_MAX 32767
#endif

#ifndef MB_CUR_MAX
#define MB_CUR_MAX 1
#endif

int rand(void);
void srand(unsigned int seed);
long random(void);
void srandom(unsigned int seed);
void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));

unsigned long strtoul(const char* nptr, char** endptr, int base);
long double strtold(const char* nptr, char** endptr);
long strtol(const char* nptr, char** endptr, int base);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);

int atoi(const char* s);
long atol(const char* s);
long long atoll(const char* s);

int    abs(int x);
long   labs(long x);
long long llabs(long long x);
