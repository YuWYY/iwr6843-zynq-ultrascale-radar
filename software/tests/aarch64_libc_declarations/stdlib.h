#ifndef TEST_STDLIB_H
#define TEST_STDLIB_H
#include <stddef.h>
void *malloc(size_t);void free(void *);void *calloc(size_t,size_t);
unsigned long strtoul(const char *,char **,int);
#endif
