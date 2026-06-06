#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#ifndef EOF
#define EOF (-1)
#endif

#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif

/*
 * ABI-INVARIANT: SEEK_* values match POSIX and the kernel SYSCALL_LSEEK
 * whence encoding (isr.c syscall 110).  Do not renumber.
 */
#ifndef SEEK_SET
#define SEEK_SET 0  /* seek from beginning of file */
#define SEEK_CUR 1  /* seek from current position  */
#define SEEK_END 2  /* seek from end of file       */
#endif

typedef struct FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);

int fputc(int c, FILE* f);
int fgetc(FILE* f);
int ungetc(int c, FILE* f);
char* fgets(char* s, int size, FILE* f);
int putc(int c, FILE* f);
int fputs(const char* s, FILE* f);
int fflush(FILE* f);

int vfprintf(FILE* f, const char* fmt, va_list ap);
int vprintf(const char* fmt, va_list ap);
int fprintf(FILE* f, const char* fmt, ...);
int printf(const char* fmt, ...);
int vsnprintf(char* buf, size_t sz, const char* fmt, va_list ap);
int snprintf(char* buf, size_t sz, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int vsprintf(char* buf, const char* fmt, va_list ap);
int sscanf(const char* str, const char* fmt, ...);
int vsscanf(const char* str, const char* fmt, va_list ap);
int fscanf(FILE* f, const char* fmt, ...);
void setbuf(FILE* f, char* buf);  /* no-op on EYN-OS: FILE is always unbuffered */
int setvbuf(FILE* f, char* buf, int mode, size_t size);
int getchar(void);
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream);
ssize_t getline(char** lineptr, size_t* n, FILE* stream);
int dprintf(int fd, const char* fmt, ...);
int fileno(FILE* stream);
FILE* fdopen(int fd, const char* mode);

// GNU-style; used by chibicc for buffering.
FILE* open_memstream(char** bufp, size_t* sizep);

int putchar(int ch);
int puts(const char* s);

/* File positioning -- backed by SYSCALL_LSEEK (110). */
int    fseek(FILE* f, long offset, int whence);
long   ftell(FILE* f);
void   rewind(FILE* f);
int    feof(FILE* f);
int    ferror(FILE* f);
